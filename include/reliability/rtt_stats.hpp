#ifndef RTT_STATS_HPP
#define RTT_STATS_HPP

#include<bits/stdc++.h>

using namespace std;

// Round-trip-time tracker per path (microseconds; 0 = no sample yet)
class RttStats
{
public:
    /*
     send_delta_us = time from sending packet to getting its ACK
     ack_delay_us  = time receiver held the packet before sending ACK
    */
    void update_rtt(uint64_t send_delta_us, uint64_t ack_delay_us);

    uint64_t get_smoothed_rtt() const;
    uint64_t get_latest_rtt() const;
    uint64_t get_min_rtt() const;
    uint64_t get_mean_deviation() const;

private:
    uint64_t rtt_floor=0;
    uint64_t recent_rtt=0;
    uint64_t srtt_us=0;
    uint64_t rttvar_us=0;
};

#endif
