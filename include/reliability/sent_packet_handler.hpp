#ifndef SENT_PACKET_HANDLER_HPP
#define SENT_PACKET_HANDLER_HPP

#include<bits/stdc++.h>
#include "packet/frame.hpp"
#include "reliability/rtt_stats.hpp"
#include "congestion_control/congestion.hpp"

using namespace std;

// Loss recovery timing limits per path (microseconds)
namespace LossRecovery
{
    // Max tracked packets: in flight + waiting to be resent (2x max congestion window of 2500)
    constexpr size_t MAX_TRACKED_PACKETS=5000;

    constexpr uint64_t INITIAL_RTO_US=500*1000;      // RTO before first RTT sample
    constexpr uint64_t MIN_RTO_US=200*1000;          // min RTO floor
    constexpr uint64_t MAX_RTO_US=60*1000*1000;    // max RTO cap
    constexpr uint64_t MIN_TIMER_US=200*1000;        // min RTO alarm floor; half of it pads the probe timer
    constexpr uint64_t MIN_PROBE_TIMEOUT_US=10*1000; // min probe timer
    constexpr uint32_t PROBES_BEFORE_RTO=2;            // tail loss probes before RTO
}

// Sent packet waiting for ACK
struct SentPacket
{
    uint64_t packet_number=0;
    vector<Frame> frames;       // frames to resend if lost
    uint64_t length=0;        // packet size in bytes
    uint64_t send_time_us=0;
};

// Called on RTO; returns true if path looks broken (potentially failed)
typedef function<bool(uint64_t last_sent_time_us)> RtoCallback;

/*
 Send side of one path: tracks unacked packets, updates RTT from ACKs, and detects losses.
 Lost packets go to a queue so Connection can resend their frames on any path.
*/
class SentPacketHandler
{
public:
    SentPacketHandler(RttStats *rtt_stats, CongestionController *congestion);

    void set_rto_callback(RtoCallback cb);

    // Record a sent packet; false if packet_number is not strictly increasing
    bool sent_packet(uint64_t packet_number, const vector<Frame> &frames, uint64_t length, uint64_t now_us);

    /*
     Process an ACK for this path. with_packet_number = carrier packet number
     (pass 0 if ACK arrived on another path). False if ACK covers unsent packets.
    */
    bool received_ack(const AckFrame &ack, uint64_t with_packet_number, uint64_t rcv_time_us);

    // Next alarm deadline (0 = no packet in flight)
    uint64_t get_alarm_timeout() const;

    // Handle expired timer (early loss, probe, or RTO)
    void on_alarm(uint64_t now_us);

    // Pop next lost packet for resend; false if queue empty
    bool dequeue_packet_for_retransmission(SentPacket &out_packet);
    bool has_retransmissions() const;

    // True if any unacked packet has stream data
    bool has_outstanding_stream_frames() const;

    // True if congestion window and tracking limit allow sending
    bool sending_allowed() const;

    uint64_t get_bytes_in_flight() const;
    uint64_t get_largest_acked() const;
    uint64_t get_least_unacked() const;
    uint64_t get_last_sent_packet_number() const;
    size_t get_num_outstanding() const;

    // Stats
    uint64_t get_packets_sent() const;
    uint64_t get_retransmissions() const;
    uint64_t get_losses() const;

private:
    // Highest packet number where everything <= it is acked
    uint64_t highest_contiguous_acked() const;

    bool sample_rtt(uint64_t acked_number, uint64_t ack_delay_us, uint64_t rcv_time_us);
    vector<uint64_t> find_newly_acked(const AckFrame &ack) const;
    void forget_acked(uint64_t packet_number);

    void declare_lost_packets(uint64_t now_us);
    void rearm_timer();

    void move_to_lost_queue(uint64_t packet_number);
    void give_up_on(uint64_t packet_number);
    void send_tail_probe();
    void requeue_everything();
    void requeue_oldest_two();

    uint64_t rto_duration() const;
    uint64_t probe_duration() const;

    RttStats *rtt_stats=nullptr;
    CongestionController *congestion=nullptr;
    RtoCallback rto_handler=nullptr;

    // Unacked packets in flight (ordered by packet number)
    map<uint64_t, SentPacket> in_flight;

    // Lost packets queued for Connection to resend
    deque<SentPacket> lost_queue;

    uint64_t highest_sent=0;           // last packet number sent
    uint64_t highest_acked=0;          // largest packet number acked
    uint64_t newest_ack_carrier=0;     // newest carrier packet number processed
    uint64_t unacked_bytes=0;

    // Time last data packet was sent (starts probe and RTO timers)
    uint64_t last_data_sent_us=0;

    // Time when oldest packet below highest_acked becomes lost (0 = none)
    uint64_t early_loss_deadline_us=0;

    // Next alarm deadline (0 = timer off)
    uint64_t timer_deadline_us=0;

    // Consecutive RTOs and probes since last ACK freed a packet
    uint32_t rtos_in_a_row=0;
    uint32_t probes_in_a_row=0;

    uint64_t total_sent=0;
    uint64_t total_resent=0;
    uint64_t total_lost=0;
};

#endif
