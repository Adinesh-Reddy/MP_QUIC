#ifndef FLOW_CONTROLLER_HPP
#define FLOW_CONTROLLER_HPP

#include<bits/stdc++.h>
#include "packet/frame.hpp"

using namespace std;

/*
 Default receive windows (both sides start here; auto-tuning can double them up to max).
*/
namespace FlowWindows
{
    constexpr uint64_t STREAM_INITIAL=32*1024;            // 32 kB
    constexpr uint64_t CONNECTION_INITIAL=48*1024;        // 48 kB
    constexpr uint64_t STREAM_MAX=16*1024*1024;         // 16 MB
    constexpr uint64_t CONNECTION_MAX=24*1024*1024;     // 24 MB
    // Keep connection window >= CONNECTION_FACTOR * stream window so it does not block a stream
    constexpr double CONNECTION_FACTOR=1.5;
}

/*
 Flow control for one stream, or for the whole connection (stream_id 0).
 Windows are absolute byte offsets:
   Sender: can send up to send_limit.
   Receiver: peer can send up to advertised_limit, updated via WINDOW_UPDATE.
*/
class FlowController
{
public:
    FlowController(uint32_t stream_id, bool contributes_to_connection);

    bool get_contributes_to_connection() const;

    // Sender side
    void add_bytes_sent(uint64_t n);
    uint64_t get_bytes_sent() const;

    // Move send_limit forward when WINDOW_UPDATE arrives (true if increased)
    bool update_send_window(uint64_t new_offset);

    // Remaining bytes allowed to send
    uint64_t send_window_size() const;
    uint64_t get_send_window() const;

    // Receiver side: save highest offset seen on stream; returns how much it grew
    uint64_t update_highest_received(uint64_t byte_offset);

    // Connection level: add stream growth
    void increment_highest_received(uint64_t increment);

    void add_bytes_read(uint64_t n, uint64_t now_us);

    // Check if a WINDOW_UPDATE frame should be sent
    bool maybe_update_window(uint64_t now_us, uint64_t rtt_us, uint64_t &new_increment, uint64_t &new_offset);

    void ensure_minimum_window_increment(uint64_t inc);

    // True if peer sent past the advertised window
    bool check_flow_control_violation() const;

    uint64_t get_receive_window() const;
    uint64_t get_receive_window_increment() const;
    uint64_t get_highest_rx_offset() const;
    uint64_t get_bytes_read() const;

private:
    void autotune_step(uint64_t now_us, uint64_t rtt_us);

    uint32_t stream_id;
    bool contributes_to_connection;

    uint64_t sent_bytes=0;
    uint64_t send_limit=0;

    uint64_t last_update_us=0; // 0 = never

    uint64_t read_bytes=0;
    uint64_t highest_rx_offset=0;
    uint64_t advertised_limit=0;
    uint64_t window_step=0;
    uint64_t max_window_step=0;
};

// Manages per-stream flow controllers plus the connection-level one
class FlowControlManager
{
public:
    FlowControlManager();

    // Sender side: remaining bytes allowed on stream (min of stream and connection windows)
    uint64_t send_window_size(uint32_t stream_id);
    void add_bytes_sent(uint32_t stream_id, uint64_t n);

    // Handle received WINDOW_UPDATE frame
    bool update_window(uint32_t stream_id, uint64_t byte_offset);

    // Receiver side: update highest offset; false if peer broke the flow control limit
    bool update_highest_received(uint32_t stream_id, uint64_t byte_offset);

    void add_bytes_read(uint32_t stream_id, uint64_t n, uint64_t now_us);

    // Return WINDOW_UPDATE frames to send now
    vector<WindowUpdateFrame> get_window_updates(uint64_t now_us, uint64_t rtt_us);

    // Current receive window for stream (0 = connection)
    uint64_t get_receive_window(uint32_t stream_id);

    FlowController *get_flow_controller(uint32_t stream_id);

private:
    map<uint32_t, FlowController> per_stream;
    FlowController connection_level;
};

#endif
