#ifndef MP_QUIC_MINI_PATH_HPP
#define MP_QUIC_MINI_PATH_HPP

#include <bits/stdc++.h>
#include "sockets/udp_socket.hpp"
#include "packet/frame.hpp"
#include "reliability/rtt_stats.hpp"
#include "congestion_control/congestion.hpp"
#include "congestion_control/olia_sender.hpp"
#include "reliability/sent_packet_handler.hpp"
#include "reliability/received_packet_handler.hpp"

using namespace std;

// One path of a connection: a pair of addresses plus everything that must be kept separate
// per path, namely its own packet numbers, RTT estimate, congestion window and ACK state.
class Path
{
public:
    Path(uint8_t id, const string &local_ip, uint16_t local_port,
         const string &remote_ip, uint16_t remote_port, UDPSocket *sock);

    // The handlers keep pointers to members of this object, so a Path must not be copied
    Path(const Path &) = delete;
    Path &operator=(const Path &) = delete;

    ssize_t send(const uint8_t *data, size_t length);

    uint8_t get_id();
    string get_local_ip();
    uint16_t get_local_port();
    string get_remote_ip();
    uint16_t get_remote_port();
    UDPSocket *get_socket();

    // The address of the peer on this path changed (for example a NAT gave the client a new
    // port). The path keeps its Path ID, its RTT and its congestion state.
    void set_remote_address(const string &ip, uint16_t port);

    // Packet numbers of this path, they start at 1 and increase by 1.
    // peek = number the next packet will get, pop = use it.
    uint64_t peek_packet_number();
    uint64_t pop_packet_number();

    RttStats *get_rtt_stats();

    // Congestion control of this path (OLIA). The Connection links the senders of all its paths.
    OliaSender *get_congestion();
    SentPacketHandler *get_sent_packet_handler();
    ReceivedPacketHandler *get_received_packet_handler();

    // True if the congestion window of this path has room for a new packet
    bool sending_allowed();

    // ACK frame for the packets received on this path (with the Path ID filled in).
    // Returns false if no ACK has to be sent now.
    bool get_ack_frame(uint64_t now_us, AckFrame &out_ack);

    // Time of the last packet received on this path (0 = nothing received yet)
    uint64_t get_last_network_activity_time();
    void set_last_network_activity_time(uint64_t time_us);

    // A path is "potentially failed" when its retransmission timer (RTO) fired and nothing
    // was received on it since the last packet we sent. Such a path is not used for sending
    // until a packet is received on it again.
    bool is_potentially_failed();
    void set_potentially_failed(bool failed);

private:
    // Called by the sent packet handler when the RTO fires, returns true if the path looks broken
    bool on_rto(uint64_t last_sent_time_us);

    uint8_t path_id;
    string local_ip;
    uint16_t local_port;
    string remote_ip;
    uint16_t remote_port;
    UDPSocket *socket = nullptr;

    uint64_t next_packet_number = 1;
    uint64_t last_heard_us = 0;
    bool looks_broken = false;

    // rtt_stats and congestion must stay declared before sent_packet_handler (it points to them)
    RttStats rtt_stats;
    OliaSender congestion;
    SentPacketHandler sent_packet_handler;
    ReceivedPacketHandler received_packet_handler;
};

#endif
