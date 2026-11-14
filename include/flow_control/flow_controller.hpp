#ifndef FLOW_CONTROLLER_HPP
#define FLOW_CONTROLLER_HPP

#include <bits/stdc++.h>
#include "packet/frame.hpp"

using namespace std;

// Receive windows. With no handshake to negotiate them, both hosts start from the same
// values. The windows then grow by auto-tuning (doubling) up to the maximums.
namespace FlowWindows
{
    constexpr uint64_t STREAM_INITIAL = 32 * 1024;            // 32 kB
    constexpr uint64_t CONNECTION_INITIAL = 48 * 1024;        // 48 kB
    constexpr uint64_t STREAM_MAX = 16 * 1024 * 1024;         // 16 MB
    constexpr uint64_t CONNECTION_MAX = 24 * 1024 * 1024;     // 24 MB
    // The connection window is kept at least this many times a growing stream window,
    // so that the connection level never becomes the bottleneck of a single stream
    constexpr double CONNECTION_FACTOR = 1.5;
}

// Flow control of one stream, or of the whole connection (stream_id 0).
// All the windows are absolute byte offsets, like in the WINDOW_UPDATE frame.
//   Sender side:   we may send until sent_bytes reaches send_limit.
//   Receiver side: the peer may send until advertised_limit, which we move
//                  forward (with a WINDOW_UPDATE) as the application reads data.
class FlowController
{
public:
    FlowController(uint32_t stream_id, bool contributes_to_connection);

    bool get_contributes_to_connection() const;

    // ---- Sender side ----
    void add_bytes_sent(uint64_t n);
    uint64_t get_bytes_sent() const;

    // Called when a WINDOW_UPDATE arrives, returns true if the window really moved forward
    bool update_send_window(uint64_t new_offset);

    // Bytes we are still allowed to send
    uint64_t send_window_size() const;
    uint64_t get_send_window() const;

    // ---- Receiver side ----
    // Stream level: remembers the highest offset received, returns by how much it grew
    uint64_t update_highest_received(uint64_t byte_offset);

    // Connection level: adds the growth of a stream
    void increment_highest_received(uint64_t increment);

    void add_bytes_read(uint64_t n, uint64_t now_us);

    // Returns true if a WINDOW_UPDATE must be sent. new_offset is the new window,
    // new_increment is the new window increment if it changed (0 otherwise).
    // rtt_us is the smoothed RTT of the connection, used to auto-tune the increment.
    bool maybe_update_window(uint64_t now_us, uint64_t rtt_us, uint64_t &new_increment, uint64_t &new_offset);

    void ensure_minimum_window_increment(uint64_t inc);

    // True if the peer sent beyond the window we advertised
    bool check_flow_control_violation() const;

    uint64_t get_receive_window() const;
    uint64_t get_receive_window_increment() const;
    uint64_t get_highest_rx_offset() const;
    uint64_t get_bytes_read() const;

private:
    void autotune_step(uint64_t now_us, uint64_t rtt_us);

    uint32_t stream_id;
    bool contributes_to_connection;

    uint64_t sent_bytes = 0;
    uint64_t send_limit = 0;

    uint64_t last_update_us = 0; // 0 = never

    uint64_t read_bytes = 0;
    uint64_t highest_rx_offset = 0;
    uint64_t advertised_limit = 0;
    uint64_t window_step = 0;
    uint64_t max_window_step = 0;
};

// Holds one flow controller per stream plus the connection-wide one, and applies
// every operation at both levels.
class FlowControlManager
{
public:
    FlowControlManager();

    // ---- Sender side ----
    // Bytes of new data we may send on this stream (limited by the stream and the connection window)
    uint64_t send_window_size(uint32_t stream_id);
    void add_bytes_sent(uint32_t stream_id, uint64_t n);

    // A WINDOW_UPDATE frame was received
    bool update_window(uint32_t stream_id, uint64_t byte_offset);

    // ---- Receiver side ----
    // Data up to byte_offset arrived on the stream. Returns false if the peer
    // went beyond a window we advertised (flow control violation).
    bool update_highest_received(uint32_t stream_id, uint64_t byte_offset);

    void add_bytes_read(uint32_t stream_id, uint64_t n, uint64_t now_us);

    // WINDOW_UPDATE frames that must be sent now (empty most of the time)
    vector<WindowUpdateFrame> get_window_updates(uint64_t now_us, uint64_t rtt_us);

    // Window currently advertised for a stream (0 = connection)
    uint64_t get_receive_window(uint32_t stream_id);

    FlowController *get_flow_controller(uint32_t stream_id);

private:
    map<uint32_t, FlowController> per_stream;
    FlowController connection_level;
};

#endif
