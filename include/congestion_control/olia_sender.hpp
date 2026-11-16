#ifndef OLIA_SENDER_HPP
#define OLIA_SENDER_HPP

#include<bits/stdc++.h>
#include "congestion_control/congestion.hpp"
#include "reliability/rtt_stats.hpp"
#include "congestion_control/hybrid_slow_start.hpp"

using namespace std;

// Fixed OLIA constants
namespace OliaTuning
{
    // Window counts as full if less than this space is left
    constexpr uint64_t MAX_BURST_BYTES=3*CongestionDefaults::MSS_BYTES;

    // Fractional packets are scaled by 2^SCALE_SHIFT (1024)
    constexpr unsigned SCALE_SHIFT=10;

    // Halve window on loss (same as TCP Reno)
    constexpr uint64_t LOSS_DIVISOR=2;
}

/*
 OLIA (Opportunistic Linked-Increases Algorithm) congestion control for one path.
 Slow start and loss cut work like TCP, but congestion avoidance is coupled across paths:
   w_r / rtt_r^2 / (SUM_p w_p / rtt_p)^2 + epsilon_r / w_r
 First term keeps multipath fairness with single-path TCP.
 Second term shifts traffic from largest-window paths to best paths (positive epsilon).
*/
class OliaSender : public CongestionController
{
public:
    OliaSender(RttStats *rtt_stats,
               uint64_t initial_congestion_window=CongestionDefaults::INITIAL_WINDOW_PACKETS,
               uint64_t max_congestion_window=CongestionDefaults::MAX_WINDOW_PACKETS);

    // Table of all OLIA senders in this connection (Path ID -> sender)
    void set_olia_senders(map<uint8_t, OliaSender *> *senders);

    // CongestionController
    void on_packet_sent(uint64_t now_us, uint64_t bytes_in_flight, uint64_t packet_number,
                        uint64_t bytes, bool is_retransmittable) override;
    void on_packet_acked(uint64_t packet_number, uint64_t acked_bytes, uint64_t bytes_in_flight) override;
    void on_packet_lost(uint64_t packet_number, uint64_t lost_bytes, uint64_t bytes_in_flight) override;
    void on_retransmission_timeout(bool packets_retransmitted) override;
    void maybe_exit_slow_start() override;
    uint64_t get_congestion_window() const override; // bytes
    string get_name() const override;

    // Stats for tests and logs
    uint64_t get_congestion_window_packets() const;
    uint64_t get_slow_start_threshold_packets() const;
    uint64_t get_slow_start_threshold() const; // bytes

    bool in_slow_start() const;

    // True while recovering from a loss (before a packet sent after the cut is acked)
    bool in_recovery() const;

    void exit_slow_start();

    // Max bytes acked between losses (over last two intervals)
    uint64_t get_smoothed_bytes_between_losses() const;

    // Last computed epsilon = num / den
    int get_epsilon_num() const;
    uint32_t get_epsilon_den() const;

private:
    void grow_window(uint64_t bytes_in_flight);
    bool window_is_limiting(uint64_t bytes_in_flight) const;

    // All senders in this connection (just {this} if no table)
    vector<OliaSender *> connection_senders();

    void update_epsilons();
    uint64_t window_after_coupled_increase();

    void reset_coupling();

    RttStats *rtt_stats=nullptr;
    map<uint8_t, OliaSender *> *peers=nullptr;
    HybridSlowStart hybrid_slow_start;

    uint64_t newest_sent=0;
    uint64_t newest_acked=0;

    // Ignore extra losses up to this packet number (same loss event)
    uint64_t recovery_end=0;

    uint64_t window;      // packets
    uint64_t ssthresh;    // packets
    uint64_t min_window;  // packets
    uint64_t max_window;  // packets

    // Coupling state
    uint64_t acked_at_older_loss=0; // total acked at previous loss
    uint64_t acked_at_last_loss=0;  // total acked at last loss
    uint64_t acked_total=0;         // running acked byte total
    int epsilon_num=0;
    uint32_t epsilon_den=1;
    int64_t window_credit=0;        // fractional packet credit * 2^SCALE_SHIFT
};

#endif
