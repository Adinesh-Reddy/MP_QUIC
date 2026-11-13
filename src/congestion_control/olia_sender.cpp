#include <bits/stdc++.h>

#include "congestion_control/olia_sender.hpp"

using namespace std;

// The products of windows, RTTs and byte counts used by OLIA do not always fit in 64 bits
// (they overflow when the RTTs of the paths are very different), so they are computed
// with 128-bit integers.
typedef unsigned __int128 uint128;

OliaSender::OliaSender(RttStats *rtt_stats, uint64_t initial_congestion_window, uint64_t max_congestion_window)
{
    this->rtt_stats = rtt_stats;
    this->congestion_window = initial_congestion_window;
    this->min_congestion_window = DEFAULT_MINIMUM_CONGESTION_WINDOW;
    this->max_congestion_window = max_congestion_window;

    // No threshold at the beginning: slow start goes on until something stops it
    this->slowstart_threshold = max_congestion_window;
}

void OliaSender::set_olia_senders(map<uint8_t, OliaSender *> *senders)
{
    olia_senders = senders;
}

vector<OliaSender *> OliaSender::all_senders()
{
    vector<OliaSender *> senders;
    if (olia_senders == nullptr || olia_senders->empty())
    {
        senders.push_back(this);
        return senders;
    }
    for (auto &entry : *olia_senders)
    {
        senders.push_back(entry.second);
    }
    return senders;
}

string OliaSender::get_name() const
{
    return "olia";
}

uint64_t OliaSender::get_congestion_window() const
{
    return congestion_window * DEFAULT_TCP_MSS;
}

uint64_t OliaSender::get_congestion_window_packets() const
{
    return congestion_window;
}

uint64_t OliaSender::get_slow_start_threshold() const
{
    return slowstart_threshold * DEFAULT_TCP_MSS;
}

uint64_t OliaSender::get_slow_start_threshold_packets() const
{
    return slowstart_threshold;
}

bool OliaSender::in_slow_start() const
{
    return congestion_window < slowstart_threshold;
}

bool OliaSender::in_recovery() const
{
    return largest_acked_packet_number <= largest_sent_at_last_cutback && largest_acked_packet_number != 0;
}

void OliaSender::exit_slow_start()
{
    slowstart_threshold = congestion_window;
}

uint64_t OliaSender::get_smoothed_bytes_between_losses() const
{
    return max(loss3 - loss2, loss2 - loss1);
}

int OliaSender::get_epsilon_num() const
{
    return epsilon_num;
}

uint32_t OliaSender::get_epsilon_den() const
{
    return epsilon_den;
}

void OliaSender::olia_reset()
{
    loss1 = 0;
    loss2 = 0;
    loss3 = 0;
    epsilon_num = 0;
    epsilon_den = 1;
    snd_cwnd_cnt = 0;
}

void OliaSender::on_packet_sent(uint64_t /*now_us*/, uint64_t /*bytes_in_flight*/, uint64_t packet_number,
                                uint64_t /*bytes*/, bool is_retransmittable)
{
    // Only data packets count (a packet with only an ACK frame is not congestion controlled)
    if (!is_retransmittable)
    {
        return;
    }
    largest_sent_packet_number = packet_number;
    hybrid_slow_start.on_packet_sent(packet_number);
}

void OliaSender::maybe_exit_slow_start()
{
    if (in_slow_start() &&
        hybrid_slow_start.should_exit_slow_start(rtt_stats->get_latest_rtt(), rtt_stats->get_min_rtt(), congestion_window))
    {
        exit_slow_start();
    }
}

// The window only grows when the sender really uses it. If the application has little to
// send (or another limit holds it back) the window says nothing about what the path can carry.
bool OliaSender::is_cwnd_limited(uint64_t bytes_in_flight) const
{
    uint64_t window = get_congestion_window();
    if (bytes_in_flight >= window)
    {
        return true;
    }
    uint64_t available_bytes = window - bytes_in_flight;
    bool slow_start_limited = in_slow_start() && bytes_in_flight > window / 2;
    return slow_start_limited || available_bytes <= MAX_BURST_BYTES;
}

void OliaSender::on_packet_acked(uint64_t packet_number, uint64_t acked_bytes, uint64_t bytes_in_flight)
{
    largest_acked_packet_number = max(packet_number, largest_acked_packet_number);

    // The window does not grow while we recover from a loss
    if (in_recovery())
    {
        return;
    }

    // OLIA measures the quality of a path with the bytes acknowledged between two losses
    loss3 += acked_bytes;

    maybe_increase_cwnd(bytes_in_flight);

    if (in_slow_start())
    {
        hybrid_slow_start.on_packet_acked(packet_number);
    }
}

void OliaSender::maybe_increase_cwnd(uint64_t bytes_in_flight)
{
    // Do not increase the congestion window unless the sender is close to using the current window
    if (!is_cwnd_limited(bytes_in_flight))
    {
        return;
    }
    if (congestion_window >= max_congestion_window)
    {
        return;
    }

    if (in_slow_start())
    {
        // Slow start, exponential growth: one more packet for each ACK
        congestion_window++;
        return;
    }

    // Congestion avoidance: the coupled increase of OLIA
    compute_epsilon();
    uint64_t new_window = congestion_window_after_ack();
    congestion_window = max(min_congestion_window, min(max_congestion_window, new_window));
}

// Computes epsilon for every path of the connection.
//   best paths       = the paths with the largest (bytes between losses) / rtt^2
//   max window paths = the paths with the largest window
// A best path that does not have the largest window should get more traffic: positive epsilon.
// The paths with the largest window give some of it away: negative epsilon.
// If every best path already has the largest window nothing has to move: epsilon = 0.
void OliaSender::compute_epsilon()
{
    vector<OliaSender *> senders = all_senders();

    uint64_t max_cwnd = 0;
    for (OliaSender *sender : senders)
    {
        max_cwnd = max(max_cwnd, sender->congestion_window);
    }

    // Quality of the best path, kept as a fraction best_bytes / best_rtt (rtt squared)
    uint128 best_rtt = 0;
    uint128 best_bytes = 0;
    for (OliaSender *sender : senders)
    {
        uint128 rtt = sender->rtt_stats->get_smoothed_rtt();
        uint128 tmp_rtt = rtt * rtt;
        uint128 tmp_bytes = sender->get_smoothed_bytes_between_losses();

        // tmp_bytes / tmp_rtt >= best_bytes / best_rtt, written without division
        if (tmp_bytes * best_rtt >= best_bytes * tmp_rtt)
        {
            best_rtt = tmp_rtt;
            best_bytes = tmp_bytes;
        }
    }

    // M = number of paths with the largest window,
    // b_not_m = number of best paths that do not have the largest window
    uint32_t m = 0;
    uint32_t b_not_m = 0;
    for (OliaSender *sender : senders)
    {
        if (sender->congestion_window == max_cwnd)
        {
            m++;
        }
        else
        {
            uint128 rtt = sender->rtt_stats->get_smoothed_rtt();
            uint128 tmp_rtt = rtt * rtt;
            uint128 tmp_bytes = sender->get_smoothed_bytes_between_losses();
            if (tmp_bytes * best_rtt >= best_bytes * tmp_rtt)
            {
                b_not_m++;
            }
        }
    }

    uint32_t num_paths = static_cast<uint32_t>(senders.size());
    for (OliaSender *sender : senders)
    {
        if (b_not_m == 0)
        {
            sender->epsilon_num = 0;
            sender->epsilon_den = 1;
            continue;
        }

        uint128 rtt = sender->rtt_stats->get_smoothed_rtt();
        uint128 tmp_rtt = rtt * rtt;
        uint128 tmp_bytes = sender->get_smoothed_bytes_between_losses();

        if (sender->congestion_window < max_cwnd && tmp_bytes * best_rtt >= best_bytes * tmp_rtt)
        {
            // A best path with a small window: epsilon = +1 / (number of paths * b_not_m)
            sender->epsilon_num = 1;
            sender->epsilon_den = num_paths * b_not_m;
        }
        else if (sender->congestion_window == max_cwnd)
        {
            // A path with the largest window: epsilon = -1 / (number of paths * m)
            sender->epsilon_num = -1;
            sender->epsilon_den = num_paths * m;
        }
        else
        {
            sender->epsilon_num = 0;
            sender->epsilon_den = 1;
        }
    }
}

// The window of this path after one more ACK in congestion avoidance.
// With S = 2^10 (the scale), W = window * S and
//   rate = ( SUM_p  window_p * S * rtt_r / rtt_p )^2
// the increase for one ACK is, in 1/S of a packet:
//   S * ( epsilon_num * rate + epsilon_den * W^2 ) / ( epsilon_den * window * rate )
// which is the formula of the header:  w_r / rtt_r^2 / (SUM w_p / rtt_p)^2  +  epsilon_r / w_r.
// The parts are added up in snd_cwnd_cnt; when a whole packet is reached the window moves by one.
uint64_t OliaSender::congestion_window_after_ack()
{
    // rate: the windows of all the paths, each one weighted by (our RTT / its RTT).
    // It starts at 1 because it is used as a divisor.
    uint128 path_rtt = rtt_stats->get_smoothed_rtt();
    uint128 rate = 1;
    for (OliaSender *sender : all_senders())
    {
        uint128 sender_rtt = sender->rtt_stats->get_smoothed_rtt();
        if (sender_rtt != 0)
        {
            uint128 scaled_window = static_cast<uint128>(sender->congestion_window) << OLIA_SCALE;
            rate += scaled_window * path_rtt / sender_rtt;
        }
    }
    rate *= rate;

    uint128 cwnd_scaled = static_cast<uint128>(congestion_window) << OLIA_SCALE;
    uint128 own_term = static_cast<uint128>(epsilon_den) * cwnd_scaled * cwnd_scaled;

    uint128 inc_den = static_cast<uint128>(epsilon_den) * congestion_window * rate;
    if (inc_den == 0)
    {
        inc_den = 1;
    }

    // Calculate the increasing term, scaling is used to reduce the rounding effect
    if (epsilon_num == -1)
    {
        if (own_term < rate)
        {
            uint128 inc_num = rate - own_term;
            snd_cwnd_cnt -= static_cast<int64_t>((inc_num << OLIA_SCALE) / inc_den);
        }
        else
        {
            uint128 inc_num = own_term - rate;
            snd_cwnd_cnt += static_cast<int64_t>((inc_num << OLIA_SCALE) / inc_den);
        }
    }
    else
    {
        uint128 inc_num = static_cast<uint128>(epsilon_num) * rate + own_term;
        snd_cwnd_cnt += static_cast<int64_t>((inc_num << OLIA_SCALE) / inc_den);
    }

    uint64_t new_window = congestion_window;
    const int64_t one_packet = (static_cast<int64_t>(1) << OLIA_SCALE) - 1;

    if (snd_cwnd_cnt >= one_packet)
    {
        new_window++;
        snd_cwnd_cnt = 0;
    }
    else if (snd_cwnd_cnt <= -one_packet)
    {
        new_window = max<uint64_t>(1, congestion_window - 1);
        snd_cwnd_cnt = 0;
    }
    return new_window;
}

void OliaSender::on_packet_lost(uint64_t packet_number, uint64_t /*lost_bytes*/, uint64_t /*bytes_in_flight*/)
{
    // Once a loss occurs, any losses in packets already sent are treated as a single
    // loss event, since they are expected: the window is cut only once
    if (packet_number <= largest_sent_at_last_cutback)
    {
        return;
    }

    // A new interval between two losses begins
    loss1 = loss2;
    loss2 = loss3;

    // Only the window of the path that lost the packet is reduced
    congestion_window = congestion_window / OLIA_LOSS_WINDOW_DIVISOR;
    if (congestion_window < min_congestion_window)
    {
        congestion_window = min_congestion_window;
    }
    slowstart_threshold = congestion_window;
    largest_sent_at_last_cutback = largest_sent_packet_number;
}

void OliaSender::on_retransmission_timeout(bool packets_retransmitted)
{
    largest_sent_at_last_cutback = 0;
    if (!packets_retransmitted)
    {
        return;
    }

    // Nothing is known about the path anymore: start again from the smallest window
    hybrid_slow_start.restart();
    olia_reset();
    slowstart_threshold = congestion_window / 2;
    congestion_window = min_congestion_window;
}
