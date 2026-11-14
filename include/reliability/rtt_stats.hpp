#ifndef RTT_STATS_HPP
#define RTT_STATS_HPP

#include <bits/stdc++.h>

using namespace std;

// Round-trip-time estimator. Each path has its own one, because paths can have
// very different delays. All values are in microseconds, 0 means "no sample yet".
class RttStats
{
public:
    // send_delta_us = time between sending a packet and receiving its ACK
    // ack_delay_us  = time the receiver waited before sending that ACK
    void update_rtt(uint64_t send_delta_us, uint64_t ack_delay_us);

    uint64_t get_smoothed_rtt() const;
    uint64_t get_latest_rtt() const;
    uint64_t get_min_rtt() const;
    uint64_t get_mean_deviation() const;

private:
    uint64_t rtt_floor = 0;
    uint64_t recent_rtt = 0;
    uint64_t srtt_us = 0;
    uint64_t rttvar_us = 0;
};

#endif
