#ifndef SENT_PACKET_HANDLER_HPP
#define SENT_PACKET_HANDLER_HPP

#include <bits/stdc++.h>
#include "packet/frame.hpp"
#include "reliability/rtt_stats.hpp"
#include "congestion_control/congestion.hpp"

using namespace std;

// Timing limits of loss recovery on one path (all times in microseconds)
namespace LossRecovery
{
    // Upper bound on the packets we keep track of: twice the largest congestion window
    // (2500 packets), so a full window can be outstanding while another one is being resent
    constexpr size_t MAX_TRACKED_PACKETS = 5000;

    constexpr uint64_t INITIAL_RTO_US = 500 * 1000;      // used until the first RTT sample exists
    constexpr uint64_t MIN_RTO_US = 200 * 1000;          // the RTO never goes below this
    constexpr uint64_t MAX_RTO_US = 60 * 1000 * 1000;    // ... nor above this, even after backoff
    constexpr uint64_t MIN_TIMER_US = 200 * 1000;        // floor for the RTO alarm; half of it pads the probe timer
    constexpr uint64_t MIN_PROBE_TIMEOUT_US = 10 * 1000; // probe timer floor with one packet in flight
    constexpr uint32_t PROBES_BEFORE_RTO = 2;            // tail loss probes tried before falling back to an RTO
}

// One packet that was sent and is not acknowledged yet
struct SentPacket
{
    uint64_t packet_number = 0;
    vector<Frame> frames;       // only the frames worth resending if the packet is lost
    uint64_t length = 0;        // size of the whole packet in bytes
    uint64_t send_time_us = 0;
};

// Called when the retransmission timer (RTO) of the path fires. It receives the time of the
// last data packet sent and returns true if the path looks broken (potentially failed).
typedef std::function<bool(uint64_t last_sent_time_us)> RtoCallback;

// Sender side of one path. It keeps every data packet sent on the path until an ACK covers
// it, takes RTT samples from those ACKs, and decides when a packet counts as lost.
// A lost packet is not resent as such: it goes into a queue, and the Connection copies its
// frames into fresh packets, possibly on a different path.
class SentPacketHandler
{
public:
    SentPacketHandler(RttStats *rtt_stats, CongestionController *congestion);

    void set_rto_callback(RtoCallback cb);

    // Records a packet that was just sent on this path.
    // Returns false if the packet number is not higher than the previous one.
    bool sent_packet(uint64_t packet_number, const vector<Frame> &frames, uint64_t length, uint64_t now_us);

    // Processes an ACK frame for this path.
    // with_packet_number = number of the packet that carried the ACK; it lets us ignore an
    // older ACK that arrives after a newer one. Pass 0 when the ACK came on another path,
    // since packet numbers of different paths cannot be compared.
    // Returns false only if the ACK is invalid (it covers a packet we never sent).
    bool received_ack(const AckFrame &ack, uint64_t with_packet_number, uint64_t rcv_time_us);

    // Time at which on_alarm() must be called, 0 if nothing is outstanding
    uint64_t get_alarm_timeout() const;

    // The timer expired: early loss check, tail loss probe or RTO, whichever is due
    void on_alarm(uint64_t now_us);

    // Takes the next lost packet out of the queue; false if the queue is empty
    bool dequeue_packet_for_retransmission(SentPacket &out_packet);
    bool has_retransmissions() const;

    // True if a packet carrying stream data is still waiting for its ACK
    bool has_outstanding_stream_frames() const;

    // True if the congestion window (and the tracking limit) allow one more packet
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
    // Highest packet number below which everything has been acknowledged
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

    RttStats *rtt_stats = nullptr;
    CongestionController *congestion = nullptr;
    RtoCallback rto_handler = nullptr;

    // Data packets in flight, keyed (and therefore ordered) by packet number
    map<uint64_t, SentPacket> in_flight;

    // Packets declared lost; the Connection takes them out and resends their frames
    deque<SentPacket> lost_queue;

    uint64_t highest_sent = 0;           // number of the last packet sent on this path
    uint64_t highest_acked = 0;          // largest packet number any ACK has covered
    uint64_t newest_ack_carrier = 0;     // number of the newest packet whose ACK we processed
    uint64_t bytes_in_flight = 0;

    // When the last data packet left; both the probe timer and the RTO count from here
    uint64_t last_data_sent_us = 0;

    // Moment at which a packet below highest_acked becomes old enough to call lost (0 = none)
    uint64_t early_loss_deadline_us = 0;

    // When on_alarm() is due (0 = timer off)
    uint64_t timer_deadline_us = 0;

    // RTOs and tail loss probes fired since the last ACK that freed a packet
    uint32_t rtos_in_a_row = 0;
    uint32_t probes_in_a_row = 0;

    uint64_t total_sent = 0;
    uint64_t total_resent = 0;
    uint64_t total_lost = 0;
};

#endif
