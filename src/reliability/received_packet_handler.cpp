#include <bits/stdc++.h>

#include "reliability/received_packet_handler.hpp"

using namespace std;

bool ReceivedPacketHandler::is_duplicate(uint64_t packet_number) const
{
    // Find the last range that starts at or before packet_number
    auto it = ranges.upper_bound(packet_number);
    if (it == ranges.begin())
    {
        return false;
    }
    --it;
    return packet_number <= it->second;
}

bool ReceivedPacketHandler::received_packet(uint64_t packet_number, bool should_instigate_ack, uint64_t now_us)
{
    if (packet_number == 0)
    {
        return false;
    }

    packets++;

    if (packet_number > largest_observed)
    {
        largest_observed = packet_number;
        largest_observed_received_time = now_us;
    }

    if (packet_number <= lower_limit)
    {
        return true;
    }

    add_to_history(packet_number);
    maybe_queue_ack(packet_number, should_instigate_ack, now_us);
    return true;
}

void ReceivedPacketHandler::add_to_history(uint64_t packet_number)
{
    if (is_duplicate(packet_number))
    {
        return;
    }

    // next = first range above packet_number, prev = last range below it
    auto next_it = ranges.upper_bound(packet_number);
    bool has_prev = (next_it != ranges.begin());
    auto prev_it = next_it;
    if (has_prev)
    {
        --prev_it;
    }

    bool extends_prev = has_prev && (prev_it->second + 1 == packet_number);
    bool extends_next = (next_it != ranges.end()) && (next_it->first == packet_number + 1);

    if (extends_prev && extends_next)
    {
        // The packet fills the hole between two ranges: merge them
        prev_it->second = next_it->second;
        ranges.erase(next_it);
    }
    else if (extends_prev)
    {
        prev_it->second = packet_number;
    }
    else if (extends_next)
    {
        uint64_t last = next_it->second;
        ranges.erase(next_it);
        ranges[packet_number] = last;
    }
    else
    {
        ranges[packet_number] = packet_number;
    }

    // Keep at most MAX_ACK_RANGES ranges, the lowest (oldest) one is dropped
    if (ranges.size() > MAX_ACK_RANGES)
    {
        lower_limit = max(lower_limit, ranges.begin()->second);
        ranges.erase(ranges.begin());
    }
}

void ReceivedPacketHandler::maybe_queue_ack(uint64_t packet_number, bool should_instigate_ack, uint64_t now_us)
{
    packets_received_since_last_ack++;

    if (should_instigate_ack)
    {
        retransmittable_packets_received_since_last_ack++;
    }

    // Always ack the first packet
    if (!has_last_ack)
    {
        ack_queued = true;
    }

    // The packet is below the largest one we already acked: it was reported
    // missing in the last ACK, so tell the sender immediately that it arrived
    if (has_last_ack && packet_number < last_ack_largest_acked)
    {
        ack_queued = true;
    }

    // A new hole appeared above the last ACK: report it immediately
    if (has_last_ack && !ranges.empty() && ranges.rbegin()->first > last_ack_largest_acked)
    {
        ack_queued = true;
    }

    if (!ack_queued && should_instigate_ack)
    {
        if (retransmittable_packets_received_since_last_ack >= RETRANSMITTABLE_PACKETS_BEFORE_ACK)
        {
            ack_queued = true;
        }
        else if (ack_alarm == 0)
        {
            // Wait a little, maybe a second packet arrives and one ACK covers both
            ack_alarm = now_us + ACK_SEND_DELAY_US;
        }
    }

    if (ack_queued)
    {
        ack_alarm = 0; // the ACK goes out now, no need for the timer
    }
}

bool ReceivedPacketHandler::get_ack_frame(uint64_t now_us, AckFrame &out_ack)
{
    if (!ack_queued && (ack_alarm == 0 || ack_alarm > now_us))
    {
        return false;
    }

    if (ranges.empty())
    {
        return false;
    }

    AckFrame ack;
    ack.largest_acked = largest_observed;

    // How long we held the largest packet, the sender removes this from its RTT sample
    uint64_t delay = now_us - largest_observed_received_time;
    ack.ack_delay_us = static_cast<uint32_t>(min<uint64_t>(delay, UINT32_MAX));

    // Highest range first
    for (auto it = ranges.rbegin(); it != ranges.rend(); ++it)
    {
        AckRange r;
        r.first = it->first;
        r.last = it->second;
        ack.ranges.push_back(r);
    }

    has_last_ack = true;
    last_ack_largest_acked = ack.largest_acked;
    ack_alarm = 0;
    ack_queued = false;
    packets_received_since_last_ack = 0;
    retransmittable_packets_received_since_last_ack = 0;

    out_ack = ack;
    return true;
}

uint64_t ReceivedPacketHandler::get_alarm_timeout() const
{
    return ack_alarm;
}

uint64_t ReceivedPacketHandler::get_largest_observed() const
{
    return largest_observed;
}

uint64_t ReceivedPacketHandler::get_packets_received() const
{
    return packets;
}

size_t ReceivedPacketHandler::get_num_ranges() const
{
    return ranges.size();
}
