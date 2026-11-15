#include <bits/stdc++.h>

#include "reliability/sent_packet_handler.hpp"

using namespace std;
using namespace LossRecovery;

SentPacketHandler::SentPacketHandler(RttStats *rtt_stats, CongestionController *congestion)
{
    this->rtt_stats = rtt_stats;
    this->congestion = congestion;
}

void SentPacketHandler::set_rto_callback(RtoCallback cb)
{
    rto_handler = cb;
}

bool SentPacketHandler::sent_packet(uint64_t packet_number, const vector<Frame> &frames, uint64_t length, uint64_t now_us)
{
    // Numbers on a path only go up, resent data included
    if (packet_number <= highest_sent)
    {
        return false;
    }

    highest_sent = packet_number;
    total_sent++;

    SentPacket packet;
    packet.packet_number = packet_number;
    packet.length = length;
    packet.send_time_us = now_us;

    // ACK frames are rebuilt fresh every time, so there is no point in keeping them
    for (const auto &frame : frames)
    {
        if (frame.is_retransmittable())
        {
            packet.frames.push_back(frame);
        }
    }

    // A packet holding only an ACK is never acknowledged by the peer, so it is not tracked
    bool carries_data = !packet.frames.empty();
    if (carries_data)
    {
        // Only data packets restart the timers. Counting pure ACKs as well would let a host
        // that keeps acknowledging push its own retransmissions further and further away.
        last_data_sent_us = now_us;
        unacked_bytes += length;
        in_flight[packet_number] = packet;
    }

    congestion->on_packet_sent(now_us, unacked_bytes, packet_number, length, carries_data);

    rearm_timer();
    return true;
}

bool SentPacketHandler::received_ack(const AckFrame &ack, uint64_t with_packet_number, uint64_t rcv_time_us)
{
    if (ack.ranges.empty() || ack.largest_acked > highest_sent)
    {
        return false; // it covers a number we never used
    }

    // An ACK carried by an older packet than one already processed tells nothing new
    if (with_packet_number != 0)
    {
        if (with_packet_number <= newest_ack_carrier)
        {
            return true;
        }
        newest_ack_carrier = with_packet_number;
    }

    // Nothing above the contiguous acknowledged prefix: a plain repeat
    if (ack.largest_acked <= highest_contiguous_acked())
    {
        return true;
    }
    highest_acked = max(highest_acked, ack.largest_acked);

    // One RTT sample per ACK, taken from its largest packet number
    if (sample_rtt(ack.largest_acked, ack.ack_delay_us, rcv_time_us))
    {
        // A clearly growing RTT means the queue is building up: slow start may end before any loss
        congestion->maybe_exit_slow_start();
    }

    for (uint64_t packet_number : find_newly_acked(ack))
    {
        uint64_t length = in_flight[packet_number].length;
        forget_acked(packet_number);
        congestion->on_packet_acked(packet_number, length, unacked_bytes);
    }

    declare_lost_packets(rcv_time_us);
    rearm_timer();

    return true;
}

uint64_t SentPacketHandler::highest_contiguous_acked() const
{
    if (in_flight.empty())
    {
        return highest_acked;
    }
    // Everything below the oldest packet still in flight has been acknowledged
    return in_flight.begin()->first - 1;
}

bool SentPacketHandler::sample_rtt(uint64_t acked_number, uint64_t ack_delay_us, uint64_t rcv_time_us)
{
    // Already removed means an earlier ACK covered it: no fresh sample
    auto it = in_flight.find(acked_number);
    if (it == in_flight.end())
    {
        return false;
    }

    uint64_t sent_at = it->second.send_time_us;
    if (rcv_time_us <= sent_at)
    {
        return false;
    }

    rtt_stats->update_rtt(rcv_time_us - sent_at, ack_delay_us);
    return true;
}

vector<uint64_t> SentPacketHandler::find_newly_acked(const AckFrame &ack) const
{
    vector<uint64_t> acked;

    // We walk the packets in flight from low to high numbers, so the ACK ranges are walked
    // in the same direction: from the last range in the frame (lowest) to the first (highest)
    size_t range = ack.ranges.size() - 1;

    for (auto it = in_flight.lower_bound(ack.lowest_acked()); it != in_flight.end(); ++it)
    {
        uint64_t number = it->first;
        if (number > ack.largest_acked)
        {
            break;
        }

        while (number > ack.ranges[range].last && range > 0)
        {
            range--;
        }

        if (number >= ack.ranges[range].first && number <= ack.ranges[range].last)
        {
            acked.push_back(number);
        }
    }

    return acked;
}

void SentPacketHandler::forget_acked(uint64_t packet_number)
{
    auto it = in_flight.find(packet_number);
    if (it == in_flight.end())
    {
        return;
    }
    unacked_bytes -= it->second.length;

    // The path answered, so the backoff of both timers starts again from zero
    rtos_in_a_row = 0;
    probes_in_a_row = 0;
    in_flight.erase(it);
}

// Time-based loss detection. A packet numbered below the highest acknowledged one is called
// lost once it has been in flight for more than 9/8 of an RTT: later packets already got
// through, and the extra eighth leaves room for some reordering.
void SentPacketHandler::declare_lost_packets(uint64_t now_us)
{
    early_loss_deadline_us = 0;

    uint64_t rtt = max(rtt_stats->get_latest_rtt(), rtt_stats->get_smoothed_rtt());
    uint64_t loss_delay = rtt + rtt / 8; // (1 + 1/8) * RTT

    vector<uint64_t> lost;
    for (const auto &[number, packet] : in_flight)
    {
        if (number > highest_acked)
        {
            break;
        }

        uint64_t age = (now_us > packet.send_time_us) ? (now_us - packet.send_time_us) : 0;
        if (age > loss_delay)
        {
            total_lost++;
            lost.push_back(number);
        }
        else if (early_loss_deadline_us == 0)
        {
            // Too young for now: come back exactly when it reaches the limit (first such packet only)
            early_loss_deadline_us = now_us + loss_delay - age;
        }
    }

    for (uint64_t number : lost)
    {
        uint64_t length = in_flight[number].length;
        move_to_lost_queue(number);
        congestion->on_packet_lost(number, length, unacked_bytes);
    }
}

void SentPacketHandler::rearm_timer()
{
    // No data in flight, nothing to wait for
    if (in_flight.empty())
    {
        timer_deadline_us = 0;
        return;
    }

    if (early_loss_deadline_us != 0)
    {
        timer_deadline_us = early_loss_deadline_us;
        return;
    }

    if (rtt_stats->get_smoothed_rtt() != 0 && probes_in_a_row < PROBES_BEFORE_RTO)
    {
        timer_deadline_us = last_data_sent_us + probe_duration();
        return;
    }

    timer_deadline_us = last_data_sent_us + max(rto_duration(), MIN_TIMER_US);
}

uint64_t SentPacketHandler::get_alarm_timeout() const
{
    return timer_deadline_us;
}

void SentPacketHandler::on_alarm(uint64_t now_us)
{
    // Everything got acknowledged in the meantime
    if (in_flight.empty())
    {
        timer_deadline_us = 0;
        return;
    }

    if (early_loss_deadline_us != 0)
    {
        // A packet just became old enough to call lost
        declare_lost_packets(now_us);
    }
    else if (rtt_stats->get_smoothed_rtt() != 0 && probes_in_a_row < PROBES_BEFORE_RTO)
    {
        // Tail loss probe: if the last packets were lost, no later packet will ever reveal
        // it, so the newest one is sent again to draw an ACK out of the peer.
        // Same condition as in rearm_timer(): a probe timer needs an RTT, so without an RTT
        // sample the alarm that just fired was an RTO and is handled as one below.
        send_tail_probe();
        probes_in_a_row++;
    }
    else
    {
        // RTO: let the path decide whether it still looks alive
        bool path_looks_broken = (rto_handler != nullptr) && rto_handler(last_data_sent_us);

        if (path_looks_broken)
        {
            // Nothing in flight here will arrive: hand all of it to the other paths
            requeue_everything();
        }
        else
        {
            requeue_oldest_two();

            // Treat this RTO resend as the path's latest transmission, even if the scheduler
            // puts the frames on another path. Should the next RTO find the path still silent,
            // it is then reported as potentially failed. Without this, a path whose last ACK
            // came after its last packet would only give back two packets per RTO, while
            // the RTO keeps doubling.
            last_data_sent_us = now_us;
        }
        rtos_in_a_row++;
    }

    rearm_timer();
}

void SentPacketHandler::move_to_lost_queue(uint64_t packet_number)
{
    auto it = in_flight.find(packet_number);
    if (it == in_flight.end())
    {
        return;
    }
    unacked_bytes -= it->second.length;
    lost_queue.push_back(it->second);
    in_flight.erase(it);
}

void SentPacketHandler::give_up_on(uint64_t packet_number)
{
    uint64_t length = in_flight[packet_number].length;
    move_to_lost_queue(packet_number);
    total_lost++;
    congestion->on_packet_lost(packet_number, length, unacked_bytes);
}

void SentPacketHandler::send_tail_probe()
{
    if (!in_flight.empty())
    {
        move_to_lost_queue(in_flight.rbegin()->first);
    }
}

void SentPacketHandler::requeue_everything()
{
    while (!in_flight.empty())
    {
        give_up_on(in_flight.begin()->first);
    }
    congestion->on_retransmission_timeout(true);
}

void SentPacketHandler::requeue_oldest_two()
{
    for (int i = 0; i < 2 && !in_flight.empty(); i++)
    {
        give_up_on(in_flight.begin()->first);
    }
    congestion->on_retransmission_timeout(true);
}

bool SentPacketHandler::dequeue_packet_for_retransmission(SentPacket &out_packet)
{
    if (lost_queue.empty())
    {
        return false;
    }
    out_packet = lost_queue.front();
    lost_queue.pop_front();
    total_resent++;
    return true;
}

bool SentPacketHandler::has_retransmissions() const
{
    return !lost_queue.empty();
}

bool SentPacketHandler::has_outstanding_stream_frames() const
{
    return any_of(in_flight.begin(), in_flight.end(), [](const auto &entry) {
        const vector<Frame> &frames = entry.second.frames;
        return any_of(frames.begin(), frames.end(), [](const Frame &frame) { return frame.type == FRAME_STREAM; });
    });
}

uint64_t SentPacketHandler::rto_duration() const
{
    // RTO = smoothed RTT + 4 * mean deviation, or the initial value before any sample
    uint64_t rto = 0;
    if (rtt_stats->get_smoothed_rtt() != 0)
    {
        rto = rtt_stats->get_smoothed_rtt() + 4 * rtt_stats->get_mean_deviation();
    }
    if (rto == 0)
    {
        rto = INITIAL_RTO_US;
    }
    rto = max(rto, MIN_RTO_US);

    // Exponential backoff: each RTO without an ACK in between doubles the next one
    uint32_t shift = min<uint32_t>(rtos_in_a_row, 16);
    rto = rto << shift;
    return min(rto, MAX_RTO_US);
}

uint64_t SentPacketHandler::probe_duration() const
{
    uint64_t rtt = rtt_stats->get_smoothed_rtt();
    if (in_flight.size() > 1)
    {
        return max(2 * rtt, rtt * 3 / 2 + MIN_TIMER_US / 2);
    }
    return max(2 * rtt, MIN_PROBE_TIMEOUT_US);
}

bool SentPacketHandler::sending_allowed() const
{
    // Too many packets to keep track of: wait, even for retransmissions
    if (lost_queue.size() + in_flight.size() >= MAX_TRACKED_PACKETS)
    {
        return false;
    }

    // Resending lost data is always allowed, even with a full congestion window
    bool window_full = unacked_bytes > congestion->get_congestion_window();
    return !window_full || !lost_queue.empty();
}

uint64_t SentPacketHandler::get_bytes_in_flight() const
{
    return unacked_bytes;
}

uint64_t SentPacketHandler::get_largest_acked() const
{
    return highest_acked;
}

uint64_t SentPacketHandler::get_least_unacked() const
{
    return highest_contiguous_acked() + 1;
}

uint64_t SentPacketHandler::get_last_sent_packet_number() const
{
    return highest_sent;
}

size_t SentPacketHandler::get_num_outstanding() const
{
    return in_flight.size();
}

uint64_t SentPacketHandler::get_packets_sent() const
{
    return total_sent;
}

uint64_t SentPacketHandler::get_retransmissions() const
{
    return total_resent;
}

uint64_t SentPacketHandler::get_losses() const
{
    return total_lost;
}
