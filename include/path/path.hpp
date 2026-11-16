#ifndef MP_QUIC_MINI_PATH_HPP
#define MP_QUIC_MINI_PATH_HPP

#include<bits/stdc++.h>
#include "sockets/udp_socket.hpp"
#include "packet/frame.hpp"
#include "reliability/rtt_stats.hpp"
#include "congestion_control/congestion.hpp"
#include "congestion_control/olia_sender.hpp"
#include "reliability/sent_packet_handler.hpp"
#include "reliability/received_packet_handler.hpp"

using namespace std;

/*
 One network path of a connection (addresses, packet numbers, RTT, OLIA, ACK state).
*/
class Path
{
public:
    Path(uint8_t id, const string &local_ip, uint16_t local_port,
         const string &remote_ip, uint16_t remote_port, UDPSocket *sock);

    // Handlers store pointers to members, so do not copy Path
    Path(const Path &)=delete;
    Path &operator=(const Path &)=delete;

    ssize_t send(const uint8_t *data, size_t length);

    uint8_t get_id();
    string get_local_ip();
    uint16_t get_local_port();
    string get_remote_ip();
    uint16_t get_remote_port();
    UDPSocket *get_socket();

    // Update peer address on NAT/port change (keeps Path ID, RTT, and congestion state)
    void set_remote_address(const string &ip, uint16_t port);

    // Packet numbers start at 1 (peek = check next, pop = take and increment)
    uint64_t peek_packet_number();
    uint64_t pop_packet_number();

    RttStats *get_rtt_stats();

    // OLIA congestion control (coupled across paths by Connection)
    OliaSender *get_congestion();
    SentPacketHandler *get_sent_packet_handler();
    ReceivedPacketHandler *get_received_packet_handler();

    // True if path congestion window has room
    bool sending_allowed();

    // Build ACK frame for this path (sets path_id); false if no ACK needed
    bool get_ack_frame(uint64_t now_us, AckFrame &out_ack);

    // Time of last packet received on this path (0 = none)
    uint64_t get_last_network_activity_time();
    void set_last_network_activity_time(uint64_t time_us);

    // True if RTO fired with no reply; skipped for sending until a packet arrives again
    bool is_potentially_failed();
    void set_potentially_failed(bool failed);

private:
    // Called on RTO; returns true if path looks broken
    bool on_rto(uint64_t last_sent_time_us);

    uint8_t path_id;
    string local_ip;
    uint16_t local_port;
    string remote_ip;
    uint16_t remote_port;
    UDPSocket *socket=nullptr;

    uint64_t next_packet_number=1;
    uint64_t last_heard_us=0;
    bool looks_broken=false;

    // Declare rtt_stats and congestion before sent_packet_handler (it holds pointers to them)
    RttStats rtt_stats;
    OliaSender congestion;
    SentPacketHandler sent_packet_handler;
    ReceivedPacketHandler received_packet_handler;
};

#endif
