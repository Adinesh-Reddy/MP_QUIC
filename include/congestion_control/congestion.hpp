#ifndef CONGESTION_HPP
#define CONGESTION_HPP

#include<bits/stdc++.h>

using namespace std;

// Default window sizes (counted in MSS_BYTES packets, bytes = packets * MSS_BYTES)
namespace CongestionDefaults
{
    constexpr uint64_t MSS_BYTES=1460;
    constexpr uint64_t INITIAL_WINDOW_PACKETS=32; // start window
    constexpr uint64_t MIN_WINDOW_PACKETS=2;      // min window after loss
    constexpr uint64_t MAX_WINDOW_PACKETS=2500;   // max ~3.6 MB per path
}

// Congestion control interface for one path (implemented by OliaSender)
class CongestionController
{
public:
    virtual ~CongestionController() {}

    virtual void on_packet_sent(uint64_t now_us, uint64_t bytes_in_flight, uint64_t packet_number,
                                uint64_t bytes, bool is_retransmittable)=0;
    virtual void on_packet_acked(uint64_t packet_number, uint64_t acked_bytes, uint64_t bytes_in_flight)=0;
    virtual void on_packet_lost(uint64_t packet_number, uint64_t lost_bytes, uint64_t bytes_in_flight)=0;
    virtual void on_retransmission_timeout(bool packets_retransmitted)=0;

    // Called on new RTT sample; rising RTT can end slow start early
    virtual void maybe_exit_slow_start()=0;

    // Window in bytes
    virtual uint64_t get_congestion_window() const=0;

    virtual string get_name() const=0;
};

// Fixed window (does not change; used only by the unit tests)
class FixedWindowCongestion : public CongestionController
{
public:
    FixedWindowCongestion(uint64_t window_packets=CongestionDefaults::INITIAL_WINDOW_PACKETS);

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
