#include <bits/stdc++.h>

#include "reliability/rtt_stats.hpp"

using namespace std;

void RttStats::update_rtt(uint64_t send_delta_us, uint64_t ack_delay_us)
{
    if (send_delta_us == 0)
    {
        return; // not a usable measurement
    }

    // rtt_floor uses the raw measurement, it is not corrected with the ack delay
    if (rtt_floor == 0 || rtt_floor > send_delta_us)
    {
        rtt_floor = send_delta_us;
    }

    // Remove the time the receiver held the packet, but only if the result stays positive
    uint64_t sample = send_delta_us;
    if (sample > ack_delay_us)
    {
        sample -= ack_delay_us;
    }
    recent_rtt = sample;

    if (srtt_us == 0)
    {
        // First sample
        srtt_us = sample;
        rttvar_us = sample / 2;
    }
    else
    {
        // rttvar_us = 3/4 * rttvar_us + 1/4 * |srtt_us - sample|
        // srtt_us   = 7/8 * srtt_us   + 1/8 * sample
        // (the deviation must be updated first, it uses the old srtt_us)
        uint64_t diff = (srtt_us > sample) ? (srtt_us - sample) : (sample - srtt_us);
        rttvar_us = (3 * rttvar_us + diff) / 4;
        srtt_us = (7 * srtt_us + sample) / 8;
    }
}

uint64_t RttStats::get_smoothed_rtt() const
{
    return srtt_us;
}

uint64_t RttStats::get_latest_rtt() const
{
    return recent_rtt;
}

uint64_t RttStats::get_min_rtt() const
{
    return rtt_floor;
}

uint64_t RttStats::get_mean_deviation() const
{
    return rttvar_us;
}
