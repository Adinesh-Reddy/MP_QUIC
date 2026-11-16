#ifndef HYBRID_SLOW_START_HPP
#define HYBRID_SLOW_START_HPP

#include<bits/stdc++.h>

using namespace std;

/*
 Hybrid slow start: exits slow start early when RTT starts rising (queue filling up),
 before packet loss happens. Compares the min RTT of each round against global min RTT.
*/
class HybridSlowStart
{
public:
    // Start a new round in slow start
    void start_receive_round(uint64_t last_sent);

    // True if this ACK is from a packet sent after current round started
    bool is_end_of_round(uint64_t ack) const;

    // Check new RTT sample; true if we should exit slow start (window is in packets)
    bool should_exit_slow_start(uint64_t latest_rtt_us, uint64_t min_rtt_us, uint64_t congestion_window);

    void on_packet_sent(uint64_t packet_number);
    void on_packet_acked(uint64_t acked_packet_number);

    bool is_started() const;

    // Reset current round and rise flag (used after RTO)
    void restart();

    // Min window (packets) before slow start can exit
    static constexpr uint64_t MIN_EXIT_WINDOW_PACKETS=16;
    // RTT samples checked at start of each round
    static constexpr uint32_t SAMPLES_PER_ROUND=8;
    // Exit threshold: min_rtt / 2^RISE_SHIFT (1/8), clamped to [MIN_RISE_US, MAX_RISE_US]
    static constexpr uint32_t RISE_SHIFT=3;
    static constexpr uint64_t MIN_RISE_US=4000;
    static constexpr uint64_t MAX_RISE_US=16000;

private:
    uint64_t round_last_packet=0;  // ACK past this ends the round
    uint64_t newest_sent=0;
    bool round_open=false;
    uint64_t round_min_rtt=0;
    uint32_t round_samples=0;
    bool rise_detected=false;
};

#endif
