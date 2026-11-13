#ifndef HYBRID_SLOW_START_HPP
#define HYBRID_SLOW_START_HPP

#include <bits/stdc++.h>

using namespace std;

// Same values as the reference implementation
const uint64_t HYBRID_START_LOW_WINDOW = 16;             // never leave slow start below 16 packets
const uint32_t HYBRID_START_MIN_SAMPLES = 8;             // RTT samples looked at in each round
const uint32_t HYBRID_START_DELAY_FACTOR_EXP = 3;        // threshold = min RTT / 2^3
const uint64_t HYBRID_START_DELAY_MIN_THRESHOLD_US = 4000;
const uint64_t HYBRID_START_DELAY_MAX_THRESHOLD_US = 16000;

// Hybrid slow start: a way to leave slow start BEFORE packets are lost.
// In slow start the window doubles every round trip. When the path gets full its queue
// grows and so does the RTT. This class compares the RTT at the beginning of each round
// (one round = one window of packets) with the lowest RTT ever seen: if it increased
// clearly, the path is filling up and slow start must stop.
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

private:
    uint64_t end_packet_number = 0;
    uint64_t last_sent_packet_number = 0;
    bool started = false;
    uint64_t current_min_rtt = 0;
    uint32_t rtt_sample_count = 0;
    bool hystart_found = false;
};

#endif
