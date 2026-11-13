#ifndef OLIA_SENDER_HPP
#define OLIA_SENDER_HPP

#include <bits/stdc++.h>
#include "congestion_control/congestion.hpp"
#include "reliability/rtt_stats.hpp"
#include "congestion_control/hybrid_slow_start.hpp"

using namespace std;

// Same values as the reference implementation (windows are counted in packets)
const uint64_t DEFAULT_MAX_CONGESTION_WINDOW = 2500;
const uint64_t DEFAULT_MINIMUM_CONGESTION_WINDOW = 2;
const uint64_t MAX_BURST_BYTES = 3 * DEFAULT_TCP_MSS;

// The window computations of OLIA use integers multiplied by 2^10 to keep some precision
const unsigned OLIA_SCALE = 10;

// When a packet is lost the window of the path is divided by this value. OLIA halves the
// window, like TCP. (The reference implementation multiplies by 0.85 instead: its sender
// was copied from the CUBIC sender of QUIC, which imitates two connections.)
const uint64_t OLIA_LOSS_WINDOW_DIVISOR = 2;

// Congestion control of ONE path with OLIA, the coupled multipath scheme used in the paper.
//
// Alone, a path behaves like TCP: slow start (the window grows by one packet per ACK),
// then congestion avoidance (about one packet per round trip), and the window is cut when
// a packet is lost.
//
// "Coupled" means that in congestion avoidance the paths of a connection do not grow
// independently. For every ACK on path r the window w_r grows by
//
//        w_r / rtt_r^2
//   ----------------------  +  epsilon_r / w_r          (in packets)
//   ( SUM_p  w_p / rtt_p )^2
//
// The first term makes all the paths together about as aggressive as ONE TCP connection,
// so a multipath connection is fair to the single-path flows that share a bottleneck.
// The second term moves window between the paths: a path that is among the best ones
// (many bytes acknowledged between two losses, short RTT) but does not have the largest
// window gets a positive epsilon; the paths with the largest window get a negative one.
//
// All the senders of a connection know each other through a shared table (olia_senders).
class OliaSender : public CongestionController
{
public:
    OliaSender(RttStats *rtt_stats,
               uint64_t initial_congestion_window = INITIAL_CONGESTION_WINDOW,
               uint64_t max_congestion_window = DEFAULT_MAX_CONGESTION_WINDOW);

    // Table of all the OLIA senders of the connection (Path ID -> sender), this one included.
    // Without a table the sender is alone and behaves like a single-path connection.
    void set_olia_senders(map<uint8_t, OliaSender *> *senders);

    // ---- CongestionController ----
    void on_packet_sent(uint64_t now_us, uint64_t bytes_in_flight, uint64_t packet_number,
                        uint64_t bytes, bool is_retransmittable) override;
    void on_packet_acked(uint64_t packet_number, uint64_t acked_bytes, uint64_t bytes_in_flight) override;
    void on_packet_lost(uint64_t packet_number, uint64_t lost_bytes, uint64_t bytes_in_flight) override;
    void on_retransmission_timeout(bool packets_retransmitted) override;
    void maybe_exit_slow_start() override;
    uint64_t get_congestion_window() const override; // in bytes
    string get_name() const override;

    // ---- State, for the tests and the statistics ----
    uint64_t get_congestion_window_packets() const;
    uint64_t get_slow_start_threshold_packets() const;
    uint64_t get_slow_start_threshold() const; // in bytes

    // Slow start: the window is below the threshold
    bool in_slow_start() const;

    // Recovery: a loss cut the window and no packet sent after the cut was acknowledged yet
    bool in_recovery() const;

    // Leave slow start now (the threshold becomes the current window)
    void exit_slow_start();

    // Largest number of bytes acknowledged between two losses (the last two intervals)
    uint64_t get_smoothed_bytes_between_losses() const;

    // epsilon of this path, as numerator / denominator (set by the last window increase)
    int get_epsilon_num() const;
    uint32_t get_epsilon_den() const;

private:
    void maybe_increase_cwnd(uint64_t bytes_in_flight);
    bool is_cwnd_limited(uint64_t bytes_in_flight) const;

    // The senders of the connection (only this one if no table was given)
    vector<OliaSender *> all_senders();

    void compute_epsilon();
    uint64_t congestion_window_after_ack();

    void olia_reset();

    RttStats *rtt_stats = nullptr;
    map<uint8_t, OliaSender *> *olia_senders = nullptr;
    HybridSlowStart hybrid_slow_start;

    uint64_t largest_sent_packet_number = 0;
    uint64_t largest_acked_packet_number = 0;

    // Largest packet number sent when the window was last cut. Losses of packets up to
    // this number belong to the same loss event and do not cut the window again.
    uint64_t largest_sent_at_last_cutback = 0;

    uint64_t congestion_window;     // in packets
    uint64_t slowstart_threshold;   // in packets
    uint64_t min_congestion_window; // in packets
    uint64_t max_congestion_window; // in packets

    // ---- OLIA state ----
    uint64_t loss1 = 0; // total bytes acked two losses ago
    uint64_t loss2 = 0; // total bytes acked at the last loss
    uint64_t loss3 = 0; // total bytes acked now
    int epsilon_num = 0;
    uint32_t epsilon_den = 1;
    int64_t snd_cwnd_cnt = 0; // fraction of a packet gained so far, multiplied by 2^10
};

#endif
