#include <bits/stdc++.h>

#include "congestion_control/hybrid_slow_start.hpp"

using namespace std;

void HybridSlowStart::start_receive_round(uint64_t last_sent)
{
    end_packet_number = last_sent;
    current_min_rtt = 0;
    rtt_sample_count = 0;
    started = true;
}

bool HybridSlowStart::is_end_of_round(uint64_t ack) const
{
    return end_packet_number < ack;
}

bool HybridSlowStart::should_exit_slow_start(uint64_t latest_rtt_us, uint64_t min_rtt_us, uint64_t congestion_window)
{
    if (!started)
    {
        // Time to start the hybrid slow start
        start_receive_round(last_sent_packet_number);
    }
    if (hystart_found)
    {
        return true;
    }

    // Delay increase detection: compare the lowest RTT of the current round with the lowest
    // RTT seen on the path. Only the first few (8) samples of the round are used.
    rtt_sample_count++;
    if (rtt_sample_count <= HYBRID_START_MIN_SAMPLES)
    {
        if (current_min_rtt == 0 || current_min_rtt > latest_rtt_us)
        {
            current_min_rtt = latest_rtt_us;
        }
    }

    // We only need to check this once per round
    if (rtt_sample_count == HYBRID_START_MIN_SAMPLES)
    {
        // The RTT must have grown by 1/8 of the min RTT, but not less than 4 ms and not more than 16 ms
        uint64_t threshold = min_rtt_us >> HYBRID_START_DELAY_FACTOR_EXP;
        threshold = min(threshold, HYBRID_START_DELAY_MAX_THRESHOLD_US);
        threshold = max(threshold, HYBRID_START_DELAY_MIN_THRESHOLD_US);

        if (current_min_rtt > min_rtt_us + threshold)
        {
            hystart_found = true;
        }
    }

    // Exit from slow start if the window is at least 16 packets and an increasing delay is found
    return congestion_window >= HYBRID_START_LOW_WINDOW && hystart_found;
}

void HybridSlowStart::on_packet_sent(uint64_t packet_number)
{
    last_sent_packet_number = packet_number;
}

void HybridSlowStart::on_packet_acked(uint64_t acked_packet_number)
{
    // End the round when the last packet of the burst is acknowledged,
    // the next round starts with the next RTT measurement
    if (is_end_of_round(acked_packet_number))
    {
        started = false;
    }
}

bool HybridSlowStart::is_started() const
{
    return started;
}

void HybridSlowStart::restart()
{
    started = false;
    hystart_found = false;
}
