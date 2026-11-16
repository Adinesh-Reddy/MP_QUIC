#ifndef RECEIVED_PACKET_HANDLER_HPP
#define RECEIVED_PACKET_HANDLER_HPP

#include<bits/stdc++.h>
#include "packet/frame.hpp"

using namespace std;

// ACK timing rules per path
namespace AckPolicy
{
    // Wait up to 25ms for a second packet before acking a lone data packet
    constexpr uint64_t DELAY_US=25*1000;
    // Send ACK at least once every 2 data packets
    constexpr int DATA_PACKETS_PER_ACK=2;
}

// Receive side of one path: tracks received packet numbers and when to send ACKs
class ReceivedPacketHandler
{
public:
    // True if packet_number was already seen on this path
    bool is_duplicate(uint64_t packet_number) const;

    /*
     Record a received packet. should_instigate_ack = true if packet needs an ACK.
     Returns false if packet_number is invalid (must be >= 1).
    */
    bool received_packet(uint64_t packet_number, bool should_instigate_ack, uint64_t now_us);

    // Fill out_ack if an ACK is due now; caller fills path_id
    bool get_ack_frame(uint64_t now_us, AckFrame &out_ack);

    // Delayed-ACK timer deadline (0 = timer off)
    uint64_t get_alarm_timeout() const;

    uint64_t get_largest_observed() const;
    uint64_t get_packets_received() const;
    size_t get_num_ranges() const;

private:
    void remember(uint64_t packet_number);
    void decide_ack(uint64_t packet_number, bool needs_ack, uint64_t now_us);

    // Received ranges: first -> last, lowest first
    map<uint64_t, uint64_t> received_ranges;

    uint64_t highest_seen=0;
    uint64_t highest_seen_at_us=0;

    // Packet numbers <= this were dropped when ranges exceeded MAX_ACK_RANGES
    uint64_t forgotten_up_to=0;

    int unacked_data_packets=0;
    bool ack_now=false;
    uint64_t delayed_ack_deadline_us=0;

    bool sent_an_ack=false;
    uint64_t highest_in_last_ack=0;

    uint64_t total_received=0;
};

#endif
