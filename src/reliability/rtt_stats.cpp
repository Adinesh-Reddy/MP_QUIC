#include<bits/stdc++.h>

#include "reliability/rtt_stats.hpp"

using namespace std;

void RttStats::update_rtt(uint64_t send_delta_us, uint64_t ack_delay_us)
{
    if(send_delta_us==0){
        return; // invalid sample
    }

    // min_rtt uses raw sample without subtracting ack_delay
    if(rtt_floor==0||rtt_floor>send_delta_us){
        rtt_floor=send_delta_us;
    }

    // Subtract receiver ack_delay if result stays positive
    uint64_t sample=send_delta_us;
    if(sample>ack_delay_us){
        sample-=ack_delay_us;
    }
    recent_rtt=sample;

    if(srtt_us==0){
        // First sample
        srtt_us=sample;
        rttvar_us=sample/2;
    }
    else{
        /*
         rttvar = 3/4 * rttvar + 1/4 * |srtt - sample|
         srtt   = 7/8 * srtt   + 1/8 * sample
         Update rttvar first using the old srtt.
        */
        uint64_t diff=(srtt_us>sample) ? (srtt_us-sample) : (sample-srtt_us);
        rttvar_us=(3*rttvar_us+diff)/4;
        srtt_us=(7*srtt_us+sample)/8;
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
