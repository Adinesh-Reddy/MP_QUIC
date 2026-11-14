#include <bits/stdc++.h>

#include "congestion_control/hybrid_slow_start.hpp"

using namespace std;

void HybridSlowStart::start_receive_round(uint64_t last_sent)
{
    round_last_packet = last_sent;
    round_min_rtt = 0;
    round_samples = 0;
    round_open = true;
}

bool HybridSlowStart::is_end_of_round(uint64_t ack) const
{
    return round_last_packet < ack;
}

bool HybridSlowStart::should_exit_slow_start(uint64_t latest_rtt_us, uint64_t min_rtt_us, uint64_t congestion_window)
{
    if (!round_open)
    {
        // First sample since the last round closed: a new round starts here
        start_receive_round(newest_sent);
    }
    if (rise_detected)
    {
        return true;
    }

    // Track the smallest RTT among the first SAMPLES_PER_ROUND samples of the round
    round_samples++;
    if (round_samples <= SAMPLES_PER_ROUND)
    {
        if (round_min_rtt == 0 || round_min_rtt > latest_rtt_us)
        {
            round_min_rtt = latest_rtt_us;
        }
    }

    // Once those samples are in, decide (only once per round)
    if (round_samples == SAMPLES_PER_ROUND)
    {
        // Required rise: 1/8 of the min RTT, but at least 4 ms and at most 16 ms
        uint64_t threshold = clamp(min_rtt_us >> RISE_SHIFT, MIN_RISE_US, MAX_RISE_US);

        if (round_min_rtt > min_rtt_us + threshold)
        {
            rise_detected = true;
        }
    }

    // Leave slow start on a rise, but only once the window has reached a useful size
    return congestion_window >= MIN_EXIT_WINDOW_PACKETS && rise_detected;
}

void HybridSlowStart::on_packet_sent(uint64_t packet_number)
{
    newest_sent = packet_number;
}

void HybridSlowStart::on_packet_acked(uint64_t acked_packet_number)
{
    // The ACK of the round's last packet closes the round;
    // the next RTT sample opens the following one
    if (is_end_of_round(acked_packet_number))
    {
        round_open = false;
    }
}

bool HybridSlowStart::is_started() const
{
    return round_open;
}

void HybridSlowStart::restart()
{
    round_open = false;
    rise_detected = false;
}
