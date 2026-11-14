#ifndef RECEIVED_PACKET_HANDLER_HPP
#define RECEIVED_PACKET_HANDLER_HPP

#include <bits/stdc++.h>
#include "packet/frame.hpp"

using namespace std;

// When the receiver of a path sends its ACKs
namespace AckPolicy
{
    // A lone data packet is acknowledged after this delay, hoping a second one joins it
    constexpr uint64_t DELAY_US = 25 * 1000;
    // ... but never more than this many data packets go unacknowledged
    constexpr int DATA_PACKETS_PER_ACK = 2;
}

// Receiver side of one path: keeps track of the packet numbers that arrived on this path
// and decides when an ACK frame must go back.
class ReceivedPacketHandler
{
public:
    // True if this packet number was already received on this path
    bool is_duplicate(uint64_t packet_number) const;

    // Must be called for every packet received on this path.
    // should_instigate_ack = the packet carries at least one frame that must be acknowledged.
    // Returns false if the packet number is invalid (packet numbers start at 1).
    bool received_packet(uint64_t packet_number, bool should_instigate_ack, uint64_t now_us);

    // Returns true and fills out_ack when an ACK must be sent now
    // (one was requested, or the delayed-ACK timer ran out).
    // The caller sets the path_id of the frame.
    bool get_ack_frame(uint64_t now_us, AckFrame &out_ack);

    // Time at which the delayed ACK is due, 0 if the timer is off
    uint64_t get_alarm_timeout() const;

    uint64_t get_largest_observed() const;
    uint64_t get_packets_received() const;
    size_t get_num_ranges() const;

private:
    void remember(uint64_t packet_number);
    void decide_ack(uint64_t packet_number, bool needs_ack, uint64_t now_us);

    // Received packet numbers as ranges first -> last, lowest range first
    map<uint64_t, uint64_t> received_ranges;

    uint64_t highest_seen = 0;
    uint64_t highest_seen_at_us = 0;

    // Numbers up to this one are no longer reported: their range was dropped when
    // there were more than MAX_ACK_RANGES of them
    uint64_t forgotten_up_to = 0;

    int unacked_data_packets = 0;
    bool ack_now = false;
    uint64_t delayed_ack_deadline_us = 0;

    bool sent_an_ack = false;
    uint64_t highest_in_last_ack = 0;

    uint64_t total_received = 0;
};

#endif
