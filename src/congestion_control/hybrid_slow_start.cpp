#include<bits/stdc++.h>

#include "congestion_control/hybrid_slow_start.hpp"

using namespace std;

void HybridSlowStart::start_receive_round(uint64_t last_sent)
{
    round_last_packet=last_sent;
    round_min_rtt=0;
    round_samples=0;
    round_open=true;
}

bool HybridSlowStart::is_end_of_round(uint64_t ack) const
{
    return round_last_packet<ack;
}

bool HybridSlowStart::should_exit_slow_start(uint64_t latest_rtt_us, uint64_t min_rtt_us, uint64_t congestion_window)
{
    if(!round_open){
        // Start a new round on first sample
        start_receive_round(newest_sent);
    }
    if(rise_detected){
        return true;
    }

    // Track min RTT in the first SAMPLES_PER_ROUND samples of this round
    round_samples++;
    if(round_samples<=SAMPLES_PER_ROUND){
        if(round_min_rtt==0||round_min_rtt>latest_rtt_us){
            round_min_rtt=latest_rtt_us;
        }
    }

    // Check threshold after collecting enough samples
    if(round_samples==SAMPLES_PER_ROUND){
        uint64_t threshold=clamp(min_rtt_us>>RISE_SHIFT, MIN_RISE_US, MAX_RISE_US);

        if(round_min_rtt>min_rtt_us+threshold){
            rise_detected=true;
        }
    }

    // Exit slow start only when window is big enough
    return congestion_window>=MIN_EXIT_WINDOW_PACKETS&&rise_detected;
}

void HybridSlowStart::on_packet_sent(uint64_t packet_number)
{
    newest_sent=packet_number;
}

void HybridSlowStart::on_packet_acked(uint64_t acked_packet_number)
{
    // Close current round when its last packet is acked
    if(is_end_of_round(acked_packet_number)){
        round_open=false;
    }
}

bool HybridSlowStart::is_started() const
{
    return round_open;
}

void HybridSlowStart::restart()
{
    round_open=false;
    rise_detected=false;
}
