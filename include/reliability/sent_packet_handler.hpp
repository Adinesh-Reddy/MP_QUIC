#ifndef SENT_PACKET_HANDLER_HPP
#define SENT_PACKET_HANDLER_HPP

#include <bits/stdc++.h>
#include "packet/frame.hpp"
#include "reliability/rtt_stats.hpp"
#include "congestion_control/congestion.hpp"

using namespace std;

// Same values as the reference implementation
const size_t MAX_TRACKED_SENT_PACKETS = 5000;                     // 2 * max congestion window of 2500 packets
const uint64_t DEFAULT_RTO_TIMEOUT_US = 500 * 1000;               // RTO when there is no RTT sample yet
const uint64_t MIN_RTO_TIMEOUT_US = 200 * 1000;                   // smallest RTO
const uint64_t MAX_RTO_TIMEOUT_US = 60 * 1000 * 1000;             // largest RTO (after backoff)
const uint64_t MIN_RETRANSMISSION_TIME_US = 200 * 1000;
const uint64_t MIN_TAIL_LOSS_PROBE_TIMEOUT_US = 10 * 1000;
const uint32_t MAX_TAIL_LOSS_PROBES = 2;                          // tail loss probes sent before an RTO

// One packet that was sent and is not acknowledged yet
struct SentPacket
{
    uint64_t packet_number = 0;
    vector<Frame> frames;       // only the retransmittable frames, needed if the packet is lost
    uint64_t length = 0;        // size of the whole packet in bytes
    uint64_t send_time_us = 0;
};

// Called when the retransmission timer (RTO) of the path fires. It receives the time of the
// last packet sent and returns true if the path looks broken (potentially failed).
typedef std::function<bool(uint64_t last_sent_time_us)> RtoCallback;

// Sender side of one path: remembers the packets sent on this path until they are
// acknowledged, measures the RTT with the ACK frames, and decides which packets are lost.
// A lost packet is never sent again as it is: it is put in the retransmission queue and
// the Connection puts its frames in new packets (maybe on another path).
class SentPacketHandler
{
public:
    SentPacketHandler(RttStats *rtt_stats, CongestionController *congestion);

    void set_rto_callback(RtoCallback cb);

    // Records a packet that was just sent on this path.
    // Returns false if the packet number is not higher than the previous one.
    bool sent_packet(uint64_t packet_number, const vector<Frame> &frames, uint64_t length, uint64_t now_us);

    // Processes an ACK frame for this path.
    // with_packet_number = number of the packet that carried the ACK, used to drop
    // ACKs that arrive out of order. Pass 0 if the ACK came on another path
    // (packet numbers of different paths cannot be compared).
    // Returns false only if the ACK is invalid (it acknowledges a packet we never sent).
    bool received_ack(const AckFrame &ack, uint64_t with_packet_number, uint64_t rcv_time_us);

    // Time at which on_alarm() must be called, 0 if nothing is outstanding
    uint64_t get_alarm_timeout() const;

    // The alarm expired: loss timer, tail loss probe or RTO (in this order)
    void on_alarm(uint64_t now_us);

    // Takes the next lost packet out of the retransmission queue, false if it is empty
    bool dequeue_packet_for_retransmission(SentPacket &out_packet);
    bool has_retransmissions() const;

    // True if a packet carrying stream data is still waiting for its ACK
    bool has_outstanding_stream_frames() const;

    // True if a new packet may be sent on this path (congestion window)
    bool sending_allowed() const;

    uint64_t get_bytes_in_flight() const;
    uint64_t get_largest_acked() const;
    uint64_t get_least_unacked() const;
    uint64_t get_last_sent_packet_number() const;
    size_t get_num_outstanding() const;

    // Statistics
    uint64_t get_packets_sent() const;
    uint64_t get_retransmissions() const;
    uint64_t get_losses() const;

private:
    // Every packet up to this number is acknowledged
    uint64_t largest_in_order_acked() const;

    bool maybe_update_rtt(uint64_t largest_acked_pn, uint64_t ack_delay_us, uint64_t rcv_time_us);
    vector<uint64_t> determine_newly_acked_packets(const AckFrame &ack) const;
    void on_packet_acked(uint64_t packet_number);

    void detect_lost_packets(uint64_t now_us);
    void update_loss_detection_alarm();

    void queue_packet_for_retransmission(uint64_t packet_number);
    void queue_rto(uint64_t packet_number);
    void retransmit_tlp();
    void retransmit_all_packets();
    void retransmit_oldest_two_packets();

    uint64_t compute_rto_timeout() const;
    uint64_t compute_tlp_timeout() const;

    RttStats *rtt_stats = nullptr;
    CongestionController *congestion = nullptr;
    RtoCallback on_rto_callback = nullptr;

    // Outstanding packets ordered by packet number
    map<uint64_t, SentPacket> packet_history;

    // Packets declared lost, waiting for the Connection to resend their frames
    deque<SentPacket> retransmission_queue;

    uint64_t last_sent_packet_number = 0;
    uint64_t largest_acked = 0;
    uint64_t largest_received_packet_with_ack = 0;
    uint64_t bytes_in_flight = 0;

    // Time the last retransmittable packet was sent, the TLP and RTO timers start from it
    uint64_t last_sent_time = 0;

    // Time at which the next packet will be considered lost (0 = not set)
    uint64_t loss_time = 0;

    // The alarm timeout (0 = not set)
    uint64_t alarm = 0;

    // Number of RTOs / tail loss probes sent without receiving an ACK
    uint32_t rto_count = 0;
    uint32_t tlp_count = 0;

    uint64_t packets = 0;
    uint64_t retransmissions = 0;
    uint64_t losses = 0;
};

#endif
