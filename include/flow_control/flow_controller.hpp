#ifndef FLOW_CONTROLLER_HPP
#define FLOW_CONTROLLER_HPP

#include <bits/stdc++.h>
#include "packet/frame.hpp"

using namespace std;

// Same values as the reference implementation. There they are exchanged during the
// handshake, we have no handshake, so both hosts simply start with the same constants.
const uint64_t RECEIVE_STREAM_FLOW_CONTROL_WINDOW         = 32 * 1024;        // 32 kB
const uint64_t RECEIVE_CONNECTION_FLOW_CONTROL_WINDOW     = 48 * 1024;        // 48 kB
const uint64_t MAX_RECEIVE_STREAM_FLOW_CONTROL_WINDOW     = 16 * 1024 * 1024; // 16 MB (value used in the paper)
const uint64_t MAX_RECEIVE_CONNECTION_FLOW_CONTROL_WINDOW = 24 * 1024 * 1024; // 24 MB
const double CONNECTION_FLOW_CONTROL_MULTIPLIER = 1.5;

// Flow control of one stream, or of the whole connection (stream_id 0).
// All the windows are absolute byte offsets, like in the WINDOW_UPDATE frame.
//   Sender side:   we may send until bytes_sent reaches send_window.
//   Receiver side: the peer may send until receive_window, which we move
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
    uint64_t get_highest_received() const;
    uint64_t get_bytes_read() const;

private:
    void maybe_adjust_window_increment(uint64_t now_us, uint64_t rtt_us);

    uint32_t stream_id;
    bool contributes_to_connection;

    uint64_t bytes_sent = 0;
    uint64_t send_window = 0;

    uint64_t last_window_update_time = 0; // 0 = never

    uint64_t bytes_read = 0;
    uint64_t highest_received = 0;
    uint64_t receive_window = 0;
    uint64_t receive_window_increment = 0;
    uint64_t max_receive_window_increment = 0;
};

// Keeps the flow controller of every stream plus the one of the connection,
// and applies every operation to both levels.
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
    map<uint32_t, FlowController> stream_flow_controllers;
    FlowController connection_flow_controller;
};

#endif
