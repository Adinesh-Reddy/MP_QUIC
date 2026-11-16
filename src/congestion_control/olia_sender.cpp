#include<bits/stdc++.h>

#include "congestion_control/olia_sender.hpp"

using namespace std;
using namespace CongestionDefaults;
using namespace OliaTuning;

// Use 128-bit ints because window * rtt^2 products can overflow 64 bits
typedef unsigned __int128 uint128;

// Path quality = bytes_between_losses / rtt^2 (kept as fraction to avoid division)
struct PathQuality
{
    uint128 bytes=0;
    uint128 rtt_squared=0;

    // this >= other, i.e. bytes / rtt_squared >= other.bytes / other.rtt_squared
    bool at_least(const PathQuality &other) const
    {
        return bytes*other.rtt_squared>=other.bytes*rtt_squared;
    }
};

static PathQuality quality_of(OliaSender *sender, const RttStats *rtt)
{
    PathQuality quality;
    uint128 srtt=rtt->get_smoothed_rtt();
    quality.rtt_squared=srtt*srtt;
    quality.bytes=sender->get_smoothed_bytes_between_losses();
    return quality;
}

OliaSender::OliaSender(RttStats *rtt_stats, uint64_t initial_congestion_window, uint64_t max_congestion_window)
{
    this->rtt_stats=rtt_stats;
    this->window=initial_congestion_window;
    this->min_window=MIN_WINDOW_PACKETS;
    // Keep max_window >= min_window so clamp() stays valid
    this->max_window=max(max_congestion_window, MIN_WINDOW_PACKETS);

    // Start in slow start until loss or RTT rise
    this->ssthresh=max_congestion_window;
}

void OliaSender::set_olia_senders(map<uint8_t, OliaSender *> *senders)
{
    peers=senders;
}

vector<OliaSender *> OliaSender::connection_senders()
{
    if(peers==nullptr||peers->empty()){
        return {this};
    }

    vector<OliaSender *> senders;
    for(const auto &[path_id, sender] : *peers){
        senders.push_back(sender);
    }
    return senders;
}

string OliaSender::get_name() const
{
    return "olia";
}

uint64_t OliaSender::get_congestion_window() const
{
    return window*MSS_BYTES;
}

uint64_t OliaSender::get_congestion_window_packets() const
{
    return window;
}

uint64_t OliaSender::get_slow_start_threshold() const
{
    return ssthresh*MSS_BYTES;
}

uint64_t OliaSender::get_slow_start_threshold_packets() const
{
    return ssthresh;
}

bool OliaSender::in_slow_start() const
{
    return window<ssthresh;
}

bool OliaSender::in_recovery() const
{
    return newest_acked<=recovery_end&&newest_acked!=0;
}

void OliaSender::exit_slow_start()
{
    ssthresh=window;
}

uint64_t OliaSender::get_smoothed_bytes_between_losses() const
{
    return max(acked_total-acked_at_last_loss, acked_at_last_loss-acked_at_older_loss);
}

int OliaSender::get_epsilon_num() const
{
    return epsilon_num;
}

uint32_t OliaSender::get_epsilon_den() const
{
    return epsilon_den;
}

void OliaSender::reset_coupling()
{
    acked_at_older_loss=0;
    acked_at_last_loss=0;
    acked_total=0;
    epsilon_num=0;
    epsilon_den=1;
    window_credit=0;
}

void OliaSender::on_packet_sent(uint64_t /*now_us*/, uint64_t /*bytes_in_flight*/, uint64_t packet_number,
                                uint64_t /*bytes*/, bool is_retransmittable)
{
    // Ignore pure ACK packets
    if(!is_retransmittable){
        return;
    }
    newest_sent=packet_number;
    hybrid_slow_start.on_packet_sent(packet_number);
}

void OliaSender::maybe_exit_slow_start()
{
    if(!in_slow_start()){
        return;
    }
    if(hybrid_slow_start.should_exit_slow_start(rtt_stats->get_latest_rtt(), rtt_stats->get_min_rtt(), window)){
        exit_slow_start();
    }
}

// Only grow the window when we are actually filling it
bool OliaSender::window_is_limiting(uint64_t bytes_in_flight) const
{
    uint64_t window_bytes=get_congestion_window();
    if(bytes_in_flight>=window_bytes){
        return true;
    }
    uint64_t room_left=window_bytes-bytes_in_flight;
    bool slow_start_limited=in_slow_start()&&bytes_in_flight>window_bytes/2;
    return slow_start_limited||room_left<=MAX_BURST_BYTES;
}

void OliaSender::on_packet_acked(uint64_t packet_number, uint64_t acked_bytes, uint64_t bytes_in_flight)
{
    newest_acked=max(packet_number, newest_acked);

    // Do not grow while recovering from loss
    if(in_recovery()){
        return;
    }

    // Track bytes acked between losses for OLIA
    acked_total+=acked_bytes;

    grow_window(bytes_in_flight);

    if(in_slow_start()){
        hybrid_slow_start.on_packet_acked(packet_number);
    }
}

void OliaSender::grow_window(uint64_t bytes_in_flight)
{
    if(!window_is_limiting(bytes_in_flight)||window>=max_window){
        return;
    }

    if(in_slow_start()){
        // Slow start: +1 packet per ACK
        window++;
        return;
    }

    // Congestion avoidance: OLIA coupled increase
    update_epsilons();
    window=clamp(window_after_coupled_increase(), min_window, max_window);
}

/*
 Compute epsilon for each path:
   best paths: highest (bytes between losses) / rtt^2
   largest-window paths: paths with max window
 Best paths with smaller windows get +epsilon; largest-window paths get -epsilon.
*/
void OliaSender::update_epsilons()
{
    vector<OliaSender *> senders=connection_senders();

    uint64_t largest_window=0;
    for(OliaSender *sender : senders){
        largest_window=max(largest_window, sender->window);
    }

    // Find best quality (keep later path on tie)
    PathQuality best;
    for(OliaSender *sender : senders){
        PathQuality quality=quality_of(sender, sender->rtt_stats);
        if(quality.at_least(best)){
            best=quality;
        }
    }

    // Count largest-window paths and best-but-smaller paths
    uint32_t largest_count=0;
    uint32_t best_but_smaller=0;
    for(OliaSender *sender : senders){
        if(sender->window==largest_window){
            largest_count++;
        }
        else if(quality_of(sender, sender->rtt_stats).at_least(best)){
            best_but_smaller++;
        }
    }

    uint32_t num_paths=static_cast<uint32_t>(senders.size());
    for(OliaSender *sender : senders){
        sender->epsilon_num=0;
        sender->epsilon_den=1;
        if(best_but_smaller==0){
            continue;
        }

        if(sender->window<largest_window&&quality_of(sender, sender->rtt_stats).at_least(best)){
            // Best path with smaller window: +1 / (paths * best_but_smaller)
            sender->epsilon_num=1;
            sender->epsilon_den=num_paths*best_but_smaller;
        }
        else if(sender->window==largest_window){
            // Largest-window path: -1 / (paths * largest_count)
            sender->epsilon_num=-1;
            sender->epsilon_den=num_paths*largest_count;
        }
    }
}

/*
 Compute next window using scaled integer math (S = 2^10 = 1024).
 Fractional increase accumulates in window_credit until +/-1 full packet.
*/
uint64_t OliaSender::window_after_coupled_increase()
{
    // rate = (SUM_p window_p * S * own_rtt / rtt_p)^2 (start at 1 to avoid div by 0)
    uint128 own_rtt=rtt_stats->get_smoothed_rtt();
    uint128 rate=1;
    for(OliaSender *sender : connection_senders()){
        uint128 sender_rtt=sender->rtt_stats->get_smoothed_rtt();
        if(sender_rtt!=0){
            uint128 scaled_window=static_cast<uint128>(sender->window)<<SCALE_SHIFT;
            rate+=scaled_window*own_rtt/sender_rtt;
        }
    }
    rate*=rate;

    uint128 scaled_own_window=static_cast<uint128>(window)<<SCALE_SHIFT;
    uint128 own_term=static_cast<uint128>(epsilon_den)*scaled_own_window*scaled_own_window;

    uint128 denominator=static_cast<uint128>(epsilon_den)*window*rate;
    if(denominator==0){
        denominator=1;
    }

    // Handle negative epsilon separately to avoid unsigned underflow
    if(epsilon_num==-1){
        if(own_term<rate){
            uint128 numerator=rate-own_term;
            window_credit-=static_cast<int64_t>((numerator<<SCALE_SHIFT)/denominator);
        }
        else{
            uint128 numerator=own_term-rate;
            window_credit+=static_cast<int64_t>((numerator<<SCALE_SHIFT)/denominator);
        }
    }
    else{
        uint128 numerator=static_cast<uint128>(epsilon_num)*rate+own_term;
        window_credit+=static_cast<int64_t>((numerator<<SCALE_SHIFT)/denominator);
    }

    uint64_t new_window=window;
    const int64_t one_packet=(static_cast<int64_t>(1)<<SCALE_SHIFT)-1;

    if(window_credit>=one_packet){
        new_window++;
        window_credit=0;
    }
    else if(window_credit<=-one_packet){
        new_window=max<uint64_t>(1, window-1);
        window_credit=0;
    }
    return new_window;
}

void OliaSender::on_packet_lost(uint64_t packet_number, uint64_t /*lost_bytes*/, uint64_t /*bytes_in_flight*/)
{
    // Only cut window once per loss event
    if(packet_number<=recovery_end){
        return;
    }

    // Shift loss byte counters
    acked_at_older_loss=acked_at_last_loss;
    acked_at_last_loss=acked_total;

    // Cut window only on the path that lost the packet
    window=window/LOSS_DIVISOR;
    if(window<min_window){
        window=min_window;
    }
    ssthresh=window;
    recovery_end=newest_sent;
}

void OliaSender::on_retransmission_timeout(bool packets_retransmitted)
{
    recovery_end=0;
    if(!packets_retransmitted){
        return;
    }

    // Reset path state and go back to min_window
    hybrid_slow_start.restart();
    reset_coupling();
    ssthresh=window/2;
    window=min_window;
}
