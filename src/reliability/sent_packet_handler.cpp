#include <bits/stdc++.h>

#include "reliability/sent_packet_handler.hpp"

using namespace std;

SentPacketHandler::SentPacketHandler(RttStats *rtt_stats, CongestionController *congestion)
{
    this->rtt_stats = rtt_stats;
    this->congestion = congestion;
}

void SentPacketHandler::set_rto_callback(RtoCallback cb)
{
    on_rto_callback = cb;
}

bool SentPacketHandler::sent_packet(uint64_t packet_number, const vector<Frame> &frames, uint64_t length, uint64_t now_us)
{
    // Packet numbers only increase on a path, also for retransmissions
    if (packet_number <= last_sent_packet_number)
    {
        return false;
    }

    last_sent_packet_number = packet_number;
    packets++;

    SentPacket packet;
    packet.packet_number = packet_number;
    packet.length = length;
    packet.send_time_us = now_us;

    // ACK frames are never retransmitted, so we do not keep them
    for (const auto &frame : frames)
    {
        if (frame.is_retransmittable())
        {
            packet.frames.push_back(frame);
        }
    }

    // A packet with only an ACK frame is not tracked: the peer does not acknowledge it
    bool is_retransmittable = !packet.frames.empty();
    if (is_retransmittable)
    {
        // Only these packets restart the timers. (The reference implementation also counts
        // ACK-only packets here, which can delay a retransmission while we keep sending ACKs.)
        last_sent_time = now_us;
        bytes_in_flight += length;
        packet_history[packet_number] = packet;
    }

    congestion->on_packet_sent(now_us, bytes_in_flight, packet_number, length, is_retransmittable);

    update_loss_detection_alarm();
    return true;
}

bool SentPacketHandler::received_ack(const AckFrame &ack, uint64_t with_packet_number, uint64_t rcv_time_us)
{
    if (ack.ranges.empty() || ack.largest_acked > last_sent_packet_number)
    {
        return false; // ACK for a packet that was never sent
    }

    // Duplicate or out-of-order ACK: a newer ACK was already processed
    if (with_packet_number != 0)
    {
        if (with_packet_number <= largest_received_packet_with_ack)
        {
            return true;
        }
        largest_received_packet_with_ack = with_packet_number;
    }

    // Repeated ACK: it does not acknowledge anything above what we already know
    if (ack.largest_acked <= largest_in_order_acked())
    {
        return true;
    }
    largest_acked = max(largest_acked, ack.largest_acked);

    // The RTT is measured with the largest acked packet only
    bool rtt_updated = maybe_update_rtt(ack.largest_acked, ack.ack_delay_us, rcv_time_us);
    if (rtt_updated)
    {
        // A clearly growing RTT means the path is filling up: slow start can stop before a loss
        congestion->maybe_exit_slow_start();
    }

    vector<uint64_t> acked_packets = determine_newly_acked_packets(ack);
    for (uint64_t packet_number : acked_packets)
    {
        uint64_t length = packet_history[packet_number].length;
        on_packet_acked(packet_number);
        congestion->on_packet_acked(packet_number, length, bytes_in_flight);
    }

    detect_lost_packets(rcv_time_us);
    update_loss_detection_alarm();

    return true;
}

uint64_t SentPacketHandler::largest_in_order_acked() const
{
    if (!packet_history.empty())
    {
        return packet_history.begin()->first - 1;
    }
    return largest_acked;
}

bool SentPacketHandler::maybe_update_rtt(uint64_t largest_acked_pn, uint64_t ack_delay_us, uint64_t rcv_time_us)
{
    // If the packet is not in the history anymore it was already acked, no new sample
    auto it = packet_history.find(largest_acked_pn);
    if (it == packet_history.end())
    {
        return false;
    }

    if (rcv_time_us <= it->second.send_time_us)
    {
        return false;
    }

    rtt_stats->update_rtt(rcv_time_us - it->second.send_time_us, ack_delay_us);
    return true;
}

vector<uint64_t> SentPacketHandler::determine_newly_acked_packets(const AckFrame &ack) const
{
    vector<uint64_t> acked_packets;

    // The history is walked upwards, so the ACK ranges are walked from the
    // lowest one (the last in the frame) to the highest one
    size_t range_index = ack.ranges.size() - 1;

    for (auto it = packet_history.lower_bound(ack.lowest_acked()); it != packet_history.end(); ++it)
    {
        uint64_t packet_number = it->first;
        if (packet_number > ack.largest_acked)
        {
            break;
        }

        while (packet_number > ack.ranges[range_index].last && range_index > 0)
        {
            range_index--;
        }

        if (packet_number >= ack.ranges[range_index].first && packet_number <= ack.ranges[range_index].last)
        {
            acked_packets.push_back(packet_number);
        }
    }

    return acked_packets;
}

void SentPacketHandler::on_packet_acked(uint64_t packet_number)
{
    auto it = packet_history.find(packet_number);
    if (it == packet_history.end())
    {
        return;
    }
    bytes_in_flight -= it->second.length;
    rto_count = 0;
    tlp_count = 0;
    packet_history.erase(it);
}

// Time based loss detection. A packet below the largest acked one is lost when it was
// sent more than 9/8 of an RTT ago: packets sent after it were acknowledged, and it had
// enough time to arrive even with some reordering.
void SentPacketHandler::detect_lost_packets(uint64_t now_us)
{
    loss_time = 0;

    uint64_t max_rtt = max(rtt_stats->get_latest_rtt(), rtt_stats->get_smoothed_rtt());
    uint64_t delay_until_lost = max_rtt + max_rtt / 8; // (1 + 1/8) * RTT

    vector<uint64_t> lost_packets;
    for (const auto &entry : packet_history)
    {
        const SentPacket &packet = entry.second;

        if (packet.packet_number > largest_acked)
        {
            break;
        }

        uint64_t time_since_sent = (now_us > packet.send_time_us) ? (now_us - packet.send_time_us) : 0;
        if (time_since_sent > delay_until_lost)
        {
            losses++;
            lost_packets.push_back(packet.packet_number);
        }
        else if (loss_time == 0)
        {
            // Not old enough yet: check again when it is (this is only entered once per call)
            loss_time = now_us + delay_until_lost - time_since_sent;
        }
    }

    for (uint64_t packet_number : lost_packets)
    {
        uint64_t length = packet_history[packet_number].length;
        queue_packet_for_retransmission(packet_number);
        congestion->on_packet_lost(packet_number, length, bytes_in_flight);
    }
}

void SentPacketHandler::update_loss_detection_alarm()
{
    // Cancel the alarm if no packets are outstanding
    if (packet_history.empty())
    {
        alarm = 0;
        return;
    }

    if (loss_time != 0)
    {
        // Time based loss detection
        alarm = loss_time;
    }
    else if (rtt_stats->get_smoothed_rtt() != 0 && tlp_count < MAX_TAIL_LOSS_PROBES)
    {
        // Tail loss probe
        alarm = last_sent_time + compute_tlp_timeout();
    }
    else
    {
        // RTO
        alarm = last_sent_time + max(compute_rto_timeout(), MIN_RETRANSMISSION_TIME_US);
    }
}

uint64_t SentPacketHandler::get_alarm_timeout() const
{
    return alarm;
}

void SentPacketHandler::on_alarm(uint64_t now_us)
{
    // Do we really have a packet to retransmit?
    if (packet_history.empty())
    {
        alarm = 0;
        return;
    }

    if (loss_time != 0)
    {
        // Time based loss detection
        detect_lost_packets(now_us);
    }
    else if (tlp_count < MAX_TAIL_LOSS_PROBES)
    {
        // Tail loss probe: the last packets may be lost and no later packet exists
        // to reveal it, so resend the newest one to provoke an ACK
        retransmit_tlp();
        tlp_count++;
    }
    else
    {
        // RTO: ask the path if it still looks alive
        bool potentially_failed = false;
        if (on_rto_callback != nullptr)
        {
            potentially_failed = on_rto_callback(last_sent_time);
        }

        if (potentially_failed)
        {
            // Everything outstanding on this path must go on another path
            retransmit_all_packets();
        }
        else
        {
            retransmit_oldest_two_packets();

            // This RTO retransmission counts as the last transmission of the path, even if the
            // scheduler sends the frames on another path. If the next RTO fires and still nothing
            // was received on this path, the path is then reported as potentially failed.
            // (Not in the reference implementation: there a broken path whose last ACK arrived
            // after its last packet would give back only two packets per RTO, with the RTO
            // doubling every time.)
            last_sent_time = now_us;
        }
        rto_count++;
    }

    update_loss_detection_alarm();
}

void SentPacketHandler::queue_packet_for_retransmission(uint64_t packet_number)
{
    auto it = packet_history.find(packet_number);
    if (it == packet_history.end())
    {
        return;
    }
    bytes_in_flight -= it->second.length;
    retransmission_queue.push_back(it->second);
    packet_history.erase(it);
}

void SentPacketHandler::queue_rto(uint64_t packet_number)
{
    uint64_t length = packet_history[packet_number].length;
    queue_packet_for_retransmission(packet_number);
    losses++;
    congestion->on_packet_lost(packet_number, length, bytes_in_flight);
}

void SentPacketHandler::retransmit_tlp()
{
    if (!packet_history.empty())
    {
        queue_packet_for_retransmission(packet_history.rbegin()->first);
    }
}

void SentPacketHandler::retransmit_all_packets()
{
    while (!packet_history.empty())
    {
        queue_rto(packet_history.begin()->first);
    }
    congestion->on_retransmission_timeout(true);
}

void SentPacketHandler::retransmit_oldest_two_packets()
{
    for (int i = 0; i < 2 && !packet_history.empty(); i++)
    {
        queue_rto(packet_history.begin()->first);
    }
    congestion->on_retransmission_timeout(true);
}

bool SentPacketHandler::dequeue_packet_for_retransmission(SentPacket &out_packet)
{
    if (retransmission_queue.empty())
    {
        return false;
    }
    out_packet = retransmission_queue.front();
    retransmission_queue.pop_front();
    retransmissions++;
    return true;
}

bool SentPacketHandler::has_retransmissions() const
{
    return !retransmission_queue.empty();
}

bool SentPacketHandler::has_outstanding_stream_frames() const
{
    for (const auto &entry : packet_history)
    {
        for (const auto &frame : entry.second.frames)
        {
            if (frame.type == FRAME_STREAM)
                return true;
        }
    }
    return false;
}

uint64_t SentPacketHandler::compute_rto_timeout() const
{
    // RTO = smoothed RTT + 4 * mean deviation
    uint64_t rto = 0;
    if (rtt_stats->get_smoothed_rtt() != 0)
    {
        rto = rtt_stats->get_smoothed_rtt() + 4 * rtt_stats->get_mean_deviation();
    }
    if (rto == 0)
    {
        rto = DEFAULT_RTO_TIMEOUT_US;
    }
    rto = max(rto, MIN_RTO_TIMEOUT_US);

    // Exponential backoff: the RTO doubles every time it fires without an ACK in between
    uint32_t shift = min<uint32_t>(rto_count, 16);
    rto = rto << shift;
    return min(rto, MAX_RTO_TIMEOUT_US);
}

uint64_t SentPacketHandler::compute_tlp_timeout() const
{
    uint64_t rtt = rtt_stats->get_smoothed_rtt();
    if (packet_history.size() > 1)
    {
        return max(2 * rtt, rtt * 3 / 2 + MIN_RETRANSMISSION_TIME_US / 2);
    }
    return max(2 * rtt, MIN_TAIL_LOSS_PROBE_TIMEOUT_US);
}

bool SentPacketHandler::sending_allowed() const
{
    bool congestion_limited = bytes_in_flight > congestion->get_congestion_window();
    bool max_tracked_limited = (retransmission_queue.size() + packet_history.size()) >= MAX_TRACKED_SENT_PACKETS;

    // Retransmissions are always allowed, also when the congestion window is full
    bool have_retransmissions = !retransmission_queue.empty();
    return !max_tracked_limited && (!congestion_limited || have_retransmissions);
}

uint64_t SentPacketHandler::get_bytes_in_flight() const
{
    return bytes_in_flight;
}

uint64_t SentPacketHandler::get_largest_acked() const
{
    return largest_acked;
}

uint64_t SentPacketHandler::get_least_unacked() const
{
    return largest_in_order_acked() + 1;
}

uint64_t SentPacketHandler::get_last_sent_packet_number() const
{
    return last_sent_packet_number;
}

size_t SentPacketHandler::get_num_outstanding() const
{
    return packet_history.size();
}

uint64_t SentPacketHandler::get_packets_sent() const
{
    return packets;
}

uint64_t SentPacketHandler::get_retransmissions() const
{
    return retransmissions;
}

uint64_t SentPacketHandler::get_losses() const
{
    return losses;
}
