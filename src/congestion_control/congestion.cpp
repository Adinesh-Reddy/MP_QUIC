#include <bits/stdc++.h>

#include "congestion_control/congestion.hpp"

using namespace std;

FixedWindowCongestion::FixedWindowCongestion(uint64_t window_packets)
{
    congestion_window = window_packets;
}

void FixedWindowCongestion::on_packet_sent(uint64_t /*now_us*/, uint64_t /*bytes_in_flight*/, uint64_t /*packet_number*/,
                                           uint64_t /*bytes*/, bool /*is_retransmittable*/)
{
}

void FixedWindowCongestion::on_packet_acked(uint64_t /*packet_number*/, uint64_t /*acked_bytes*/, uint64_t /*bytes_in_flight*/)
{
}

void FixedWindowCongestion::on_packet_lost(uint64_t /*packet_number*/, uint64_t /*lost_bytes*/, uint64_t /*bytes_in_flight*/)
{
}

void FixedWindowCongestion::on_retransmission_timeout(bool /*packets_retransmitted*/)
{
}

void FixedWindowCongestion::maybe_exit_slow_start()
{
}

string FixedWindowCongestion::get_name() const
{
    return "fixed-window";
}

uint64_t FixedWindowCongestion::get_congestion_window() const
{
    return congestion_window * DEFAULT_TCP_MSS;
}
