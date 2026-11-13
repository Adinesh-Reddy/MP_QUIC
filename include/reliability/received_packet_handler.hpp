#ifndef RECEIVED_PACKET_HANDLER_HPP
#define RECEIVED_PACKET_HANDLER_HPP

#include <bits/stdc++.h>
#include "packet/frame.hpp"

using namespace std;

// Same values as the reference implementation
const uint64_t ACK_SEND_DELAY_US = 25 * 1000;       // delayed ACK timer (25 ms)
const int RETRANSMITTABLE_PACKETS_BEFORE_ACK = 2;   // ACK at least every 2nd data packet

// Receiver side of one path: remembers which packet numbers arrived on this path
// and decides when an ACK frame has to be sent back.
class ReceivedPacketHandler
{
public:
    // True if this packet number was already received on this path
    bool is_duplicate(uint64_t packet_number) const;

    // Must be called for every packet received on this path.
    // should_instigate_ack = the packet has at least one retransmittable frame.
    // Returns false if the packet number is invalid (packet numbers start at 1).
    bool received_packet(uint64_t packet_number, bool should_instigate_ack, uint64_t now_us);

    // Returns true and fills out_ack when an ACK has to be sent now
    // (an ACK is queued, or the delayed ACK timer expired).
    // The caller sets the path_id of the frame.
    bool get_ack_frame(uint64_t now_us, AckFrame &out_ack);

    // Time at which the delayed ACK must be sent, 0 if the timer is not running
    uint64_t get_alarm_timeout() const;

    uint64_t get_largest_observed() const;
    uint64_t get_packets_received() const;
    size_t get_num_ranges() const;

private:
    void add_to_history(uint64_t packet_number);
    void maybe_queue_ack(uint64_t packet_number, bool should_instigate_ack, uint64_t now_us);

    // Received packet numbers stored as ranges: first -> last, lowest range first
    map<uint64_t, uint64_t> ranges;

    uint64_t largest_observed = 0;
    uint64_t largest_observed_received_time = 0;

    // Packets with a number <= lower_limit are not acknowledged anymore
    // (their range was dropped because more than MAX_ACK_RANGES ranges existed)
    uint64_t lower_limit = 0;

    int packets_received_since_last_ack = 0;
    int retransmittable_packets_received_since_last_ack = 0;
    bool ack_queued = false;
    uint64_t ack_alarm = 0;

    bool has_last_ack = false;
    uint64_t last_ack_largest_acked = 0;

    uint64_t packets = 0;
};

#endif
