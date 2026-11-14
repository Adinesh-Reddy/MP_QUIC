#ifndef OLIA_SENDER_HPP
#define OLIA_SENDER_HPP

#include <bits/stdc++.h>
#include "congestion_control/congestion.hpp"
#include "reliability/rtt_stats.hpp"
#include "congestion_control/hybrid_slow_start.hpp"

using namespace std;

// Fixed parameters of the OLIA window computations
namespace OliaTuning
{
    // When this little room is left in the window, the window counts as fully used
    constexpr uint64_t MAX_BURST_BYTES = 3 * CongestionDefaults::MSS_BYTES;

    // Fractions of a packet are kept as integers multiplied by 2^SCALE_SHIFT (1024)
    constexpr unsigned SCALE_SHIFT = 10;

    // A loss divides the window of its path by this. OLIA halves it, the same as TCP Reno.
    // (Some CUBIC-based senders use a factor of 0.85 instead, to imitate two connections.)
    constexpr uint64_t LOSS_DIVISOR = 2;
}

// Congestion control of ONE path with OLIA (Opportunistic Linked-Increases Algorithm).
//
// On its own a path behaves like TCP: slow start (one more packet per ACK), then congestion
// avoidance (about one more packet per round trip), and a cut of the window on a loss.
//
// The paths of a connection are coupled in congestion avoidance: they do not grow
// independently. Each ACK on path r grows its window w_r by
//
//        w_r / rtt_r^2
//   ----------------------  +  epsilon_r / w_r          (in packets)
//   ( SUM_p  w_p / rtt_p )^2
//
// The first term keeps all the paths together about as aggressive as a single TCP
// connection, which keeps a multipath connection fair to single-path flows sharing a
// bottleneck with it. The second term shifts traffic between the paths: a path that is
// among the best (many bytes delivered between two losses, short RTT) but does not have
// the largest window gets a positive epsilon, and the paths with the largest window get
// a negative one.
//
// The senders of one connection see each other through a shared table (set_olia_senders).
class OliaSender : public CongestionController
{
public:
    OliaSender(RttStats *rtt_stats,
               uint64_t initial_congestion_window = CongestionDefaults::INITIAL_WINDOW_PACKETS,
               uint64_t max_congestion_window = CongestionDefaults::MAX_WINDOW_PACKETS);

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

    // Recovery: a loss cut the window, and no packet sent after the cut has been acknowledged yet
    bool in_recovery() const;

    // Leave slow start now (the threshold becomes the current window)
    void exit_slow_start();

    // Bytes acknowledged between two losses: the larger of the last two such intervals
    uint64_t get_smoothed_bytes_between_losses() const;

    // epsilon of this path as numerator / denominator (set by the last window increase)
    int get_epsilon_num() const;
    uint32_t get_epsilon_den() const;

private:
    void grow_window(uint64_t bytes_in_flight);
    bool window_is_limiting(uint64_t bytes_in_flight) const;

    // Every sender of the connection (just this one when there is no table)
    vector<OliaSender *> connection_senders();

    void update_epsilons();
    uint64_t window_after_coupled_increase();

    void reset_coupling();

    RttStats *rtt_stats = nullptr;
    map<uint8_t, OliaSender *> *peers = nullptr;
    HybridSlowStart hybrid_slow_start;

    uint64_t newest_sent = 0;
    uint64_t newest_acked = 0;

    // Highest number sent when the window was last cut. Losses up to this number belong to
    // that same loss event and do not cut the window a second time.
    uint64_t recovery_end = 0;

    uint64_t window;      // in packets
    uint64_t ssthresh;    // slow start threshold, in packets
    uint64_t min_window;  // in packets
    uint64_t max_window;  // in packets

    // ---- coupling state ----
    uint64_t acked_at_older_loss = 0; // running byte total at the loss before the last one
    uint64_t acked_at_last_loss = 0;  // running byte total at the last loss
    uint64_t acked_total = 0;         // running byte total now
    int epsilon_num = 0;
    uint32_t epsilon_den = 1;
    int64_t window_credit = 0;        // fraction of a packet earned so far, times 2^SCALE_SHIFT
};

#endif
