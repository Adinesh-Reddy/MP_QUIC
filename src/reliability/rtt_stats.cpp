#include <bits/stdc++.h>

#include "reliability/rtt_stats.hpp"

using namespace std;

void RttStats::update_rtt(uint64_t send_delta_us, uint64_t ack_delay_us)
{
    if (send_delta_us == 0)
    {
        return; // not a usable measurement
    }

    // min_rtt uses the raw measurement, it is not corrected with the ack delay
    if (min_rtt == 0 || min_rtt > send_delta_us)
    {
        min_rtt = send_delta_us;
    }

    // Remove the time the receiver held the packet, but only if the result stays positive
    uint64_t sample = send_delta_us;
    if (sample > ack_delay_us)
    {
        sample -= ack_delay_us;
    }
    latest_rtt = sample;

    if (smoothed_rtt == 0)
    {
        // First sample
        smoothed_rtt = sample;
        mean_deviation = sample / 2;
    }
    else
    {
        // mean_deviation = 3/4 * mean_deviation + 1/4 * |smoothed_rtt - sample|
        // smoothed_rtt   = 7/8 * smoothed_rtt   + 1/8 * sample
        // (the deviation must be updated first, it uses the old smoothed_rtt)
        uint64_t diff = (smoothed_rtt > sample) ? (smoothed_rtt - sample) : (sample - smoothed_rtt);
        mean_deviation = (3 * mean_deviation + diff) / 4;
        smoothed_rtt = (7 * smoothed_rtt + sample) / 8;
    }
}

uint64_t RttStats::get_smoothed_rtt() const
{
    return smoothed_rtt;
}

uint64_t RttStats::get_latest_rtt() const
{
    return latest_rtt;
}

uint64_t RttStats::get_min_rtt() const
{
    return min_rtt;
}

uint64_t RttStats::get_mean_deviation() const
{
    return mean_deviation;
}
