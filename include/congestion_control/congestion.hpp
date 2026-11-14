#ifndef CONGESTION_HPP
#define CONGESTION_HPP

#include <bits/stdc++.h>

using namespace std;

// Window sizes shared by every congestion controller. Windows are counted in packets of
// MSS_BYTES, the usual TCP segment size, so a window converts to bytes as packets * MSS_BYTES.
namespace CongestionDefaults
{
    constexpr uint64_t MSS_BYTES = 1460;
    constexpr uint64_t INITIAL_WINDOW_PACKETS = 32; // large enough to fill a short path in one RTT
    constexpr uint64_t MIN_WINDOW_PACKETS = 2;      // a loss never shrinks the window below this
    constexpr uint64_t MAX_WINDOW_PACKETS = 2500;   // about 3.6 MB in flight per path
}

// The events that the reliability layer reports to the congestion control of one path.
// OliaSender (olia_sender.hpp) implements it with OLIA, a coupled multipath scheme.
class CongestionController
{
public:
    virtual ~CongestionController() {}

    virtual void on_packet_sent(uint64_t now_us, uint64_t bytes_in_flight, uint64_t packet_number,
                                uint64_t bytes, bool is_retransmittable) = 0;
    virtual void on_packet_acked(uint64_t packet_number, uint64_t acked_bytes, uint64_t bytes_in_flight) = 0;
    virtual void on_packet_lost(uint64_t packet_number, uint64_t lost_bytes, uint64_t bytes_in_flight) = 0;
    virtual void on_retransmission_timeout(bool packets_retransmitted) = 0;

    // Called when an ACK gave a new RTT measurement: a growing RTT can end slow start early
    virtual void maybe_exit_slow_start() = 0;

    // Congestion window in bytes
    virtual uint64_t get_congestion_window() const = 0;

    virtual string get_name() const = 0;
};

// A window that never changes. It is not a congestion control scheme: it is kept for the
// unit tests of the other classes (their results must not depend on a moving window)
// and as a simple baseline to compare OLIA with.
class FixedWindowCongestion : public CongestionController
{
public:
    FixedWindowCongestion(uint64_t window_packets = CongestionDefaults::INITIAL_WINDOW_PACKETS);

    void on_packet_sent(uint64_t now_us, uint64_t bytes_in_flight, uint64_t packet_number,
                        uint64_t bytes, bool is_retransmittable) override;
    void on_packet_acked(uint64_t packet_number, uint64_t acked_bytes, uint64_t bytes_in_flight) override;
    void on_packet_lost(uint64_t packet_number, uint64_t lost_bytes, uint64_t bytes_in_flight) override;
    void on_retransmission_timeout(bool packets_retransmitted) override;
    void maybe_exit_slow_start() override;

    uint64_t get_congestion_window() const override;
    string get_name() const override;

private:
    uint64_t congestion_window; // in packets
};

#endif
