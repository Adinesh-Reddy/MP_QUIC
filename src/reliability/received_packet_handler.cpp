#include<bits/stdc++.h>

#include "reliability/received_packet_handler.hpp"

using namespace std;
using namespace AckPolicy;

bool ReceivedPacketHandler::is_duplicate(uint64_t packet_number) const
{
    // Last range starting at or before packet_number
    auto it=received_ranges.upper_bound(packet_number);
    if(it==received_ranges.begin()){
        return false;
    }
    --it;
    return packet_number<=it->second;
}

bool ReceivedPacketHandler::received_packet(uint64_t packet_number, bool should_instigate_ack, uint64_t now_us)
{
    if(packet_number==0){
        return false;
    }

    total_received++;

    if(packet_number>highest_seen){
        highest_seen=packet_number;
        highest_seen_at_us=now_us;
    }

    if(packet_number<=forgotten_up_to){
        return true;
    }

    remember(packet_number);
    decide_ack(packet_number, should_instigate_ack, now_us);
    return true;
}

void ReceivedPacketHandler::remember(uint64_t packet_number)
{
    if(is_duplicate(packet_number)){
        return;
    }

    // Merge with neighbor ranges if adjacent
    auto above=received_ranges.upper_bound(packet_number);
    bool has_below=(above!=received_ranges.begin());
    auto below=above;
    if(has_below){
        --below;
    }

    bool joins_below=has_below&&(below->second+1==packet_number);
    bool joins_above=(above!=received_ranges.end())&&(above->first==packet_number+1);

    if(joins_below&&joins_above){
        // Fills gap between two ranges: merge them
        below->second=above->second;
        received_ranges.erase(above);
    }
    else if(joins_below){
        below->second=packet_number;
    }
    else if(joins_above){
        uint64_t last=above->second;
        received_ranges.erase(above);
        received_ranges[packet_number]=last;
    }
    else{
        received_ranges[packet_number]=packet_number;
    }

    // Cap stored ranges at MAX_ACK_RANGES by dropping oldest
    if(received_ranges.size()>MAX_ACK_RANGES){
        forgotten_up_to=max(forgotten_up_to, received_ranges.begin()->second);
        received_ranges.erase(received_ranges.begin());
    }
}

void ReceivedPacketHandler::decide_ack(uint64_t packet_number, bool needs_ack, uint64_t now_us)
{
    if(needs_ack){
        unacked_data_packets++;
    }

    // Always ack the first packet right away
    if(!sent_an_ack){
        ack_now=true;
    }
    else{
        // Packet filled an older gap: ack right away
        if(packet_number<highest_in_last_ack){
            ack_now=true;
        }

        // New gap opened above last ACK: ack right away
        if(!received_ranges.empty()&&received_ranges.rbegin()->first>highest_in_last_ack){
            ack_now=true;
        }
    }

    if(!ack_now&&needs_ack){
        if(unacked_data_packets>=DATA_PACKETS_PER_ACK){
            ack_now=true;
        }
        else if(delayed_ack_deadline_us==0){
            // Delay ACK briefly to bundle with next packet
            delayed_ack_deadline_us=now_us+DELAY_US;
        }
    }

    if(ack_now){
        delayed_ack_deadline_us=0; // sending now, no timer needed
    }
}

bool ReceivedPacketHandler::get_ack_frame(uint64_t now_us, AckFrame &out_ack)
{
    bool timer_expired=(delayed_ack_deadline_us!=0&&delayed_ack_deadline_us<=now_us);
    if(!ack_now&&!timer_expired){
        return false;
    }

    if(received_ranges.empty()){
        return false;
    }

    AckFrame ack;
    ack.largest_acked=highest_seen;

    // Delay since largest packet arrived (sender subtracts this from RTT)
    uint64_t held_for=now_us-highest_seen_at_us;
    ack.ack_delay_us=static_cast<uint32_t>(min<uint64_t>(held_for, UINT32_MAX));

    // Write ranges from highest to lowest
    for(auto it=received_ranges.rbegin(); it!=received_ranges.rend(); ++it){
        AckRange range;
        range.first=it->first;
        range.last=it->second;
        ack.ranges.push_back(range);
    }

    sent_an_ack=true;
    highest_in_last_ack=ack.largest_acked;
    delayed_ack_deadline_us=0;
    ack_now=false;
    unacked_data_packets=0;

    out_ack=ack;
    return true;
}

uint64_t ReceivedPacketHandler::get_alarm_timeout() const
{
    return delayed_ack_deadline_us;
}

uint64_t ReceivedPacketHandler::get_largest_observed() const
{
    return highest_seen;
}

uint64_t ReceivedPacketHandler::get_packets_received() const
{
    return total_received;
}

size_t ReceivedPacketHandler::get_num_ranges() const
{
    return received_ranges.size();
}
