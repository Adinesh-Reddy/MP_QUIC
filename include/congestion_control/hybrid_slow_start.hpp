#ifndef HYBRID_SLOW_START_HPP
#define HYBRID_SLOW_START_HPP

#include <bits/stdc++.h>

using namespace std;

// Hybrid slow start: ends slow start before the first loss instead of after it.
// While in slow start the window doubles every round trip, so it soon outgrows the path;
// from then on the extra packets sit in a queue and the RTT goes up. Once per round
// (a round = one window of packets) the smallest RTT of the round is compared with the
// smallest RTT the path has ever shown. A clear rise means the queue is filling up,
// and slow start stops while there is still no loss.
class HybridSlowStart
{
public:
    // Called for the start of each round in the slow start phase
    void start_receive_round(uint64_t last_sent);

    // True if this ACK is for a packet sent after the current round began
    bool is_end_of_round(uint64_t ack) const;

    // Called for every new RTT measurement. Returns true if slow start must be left.
    // congestion_window is in packets.
    bool should_exit_slow_start(uint64_t latest_rtt_us, uint64_t min_rtt_us, uint64_t congestion_window);

    void on_packet_sent(uint64_t packet_number);
    void on_packet_acked(uint64_t acked_packet_number);

    bool is_started() const;

    // Restart the slow start phase
    void restart();

    // Never leave slow start while the window is still this small (packets)
    static constexpr uint64_t MIN_EXIT_WINDOW_PACKETS = 16;
    // RTT samples taken at the start of each round to find its smallest RTT
    static constexpr uint32_t SAMPLES_PER_ROUND = 8;
    // A rise counts when it exceeds min RTT / 2^RISE_SHIFT (1/8), held between these bounds
    static constexpr uint32_t RISE_SHIFT = 3;
    static constexpr uint64_t MIN_RISE_US = 4000;
    static constexpr uint64_t MAX_RISE_US = 16000;

private:
    uint64_t round_last_packet = 0;  // the round ends when an ACK goes past this number
    uint64_t newest_sent = 0;
    bool round_open = false;
    uint64_t round_min_rtt = 0;
    uint32_t round_samples = 0;
    bool rise_detected = false;
};

#endif
