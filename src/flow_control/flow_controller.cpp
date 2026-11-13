#include <bits/stdc++.h>

#include "flow_control/flow_controller.hpp"

using namespace std;

FlowController::FlowController(uint32_t stream_id, bool contributes_to_connection)
{
    this->stream_id = stream_id;
    this->contributes_to_connection = contributes_to_connection;

    if (stream_id == 0)
    {
        receive_window = RECEIVE_CONNECTION_FLOW_CONTROL_WINDOW;
        max_receive_window_increment = MAX_RECEIVE_CONNECTION_FLOW_CONTROL_WINDOW;
    }
    else
    {
        receive_window = RECEIVE_STREAM_FLOW_CONTROL_WINDOW;
        max_receive_window_increment = MAX_RECEIVE_STREAM_FLOW_CONTROL_WINDOW;
    }
    receive_window_increment = receive_window;

    // The peer starts with the same receive window, so this is what we may send at first
    send_window = receive_window;
}

bool FlowController::get_contributes_to_connection() const
{
    return contributes_to_connection;
}

void FlowController::add_bytes_sent(uint64_t n)
{
    bytes_sent += n;
}

uint64_t FlowController::get_bytes_sent() const
{
    return bytes_sent;
}

bool FlowController::update_send_window(uint64_t new_offset)
{
    // A window never moves backwards (an old WINDOW_UPDATE can arrive late)
    if (new_offset > send_window)
    {
        send_window = new_offset;
        return true;
    }
    return false;
}

uint64_t FlowController::send_window_size() const
{
    if (bytes_sent > send_window)
    {
        return 0;
    }
    return send_window - bytes_sent;
}

uint64_t FlowController::get_send_window() const
{
    return send_window;
}

uint64_t FlowController::update_highest_received(uint64_t byte_offset)
{
    // A smaller offset is normal when frames are reordered, nothing to do then
    if (byte_offset <= highest_received)
    {
        return 0;
    }
    uint64_t increment = byte_offset - highest_received;
    highest_received = byte_offset;
    return increment;
}

void FlowController::increment_highest_received(uint64_t increment)
{
    highest_received += increment;
}

void FlowController::add_bytes_read(uint64_t n, uint64_t now_us)
{
    // Pretend we sent a WINDOW_UPDATE when the first byte is read,
    // this way the auto-tuning already works for the first real WINDOW_UPDATE
    if (bytes_read == 0)
    {
        last_window_update_time = now_us;
    }
    bytes_read += n;
}

bool FlowController::maybe_update_window(uint64_t now_us, uint64_t rtt_us, uint64_t &new_increment, uint64_t &new_offset)
{
    // (bytes_read can never pass receive_window: data beyond the window is refused before it is read)
    uint64_t diff = (receive_window > bytes_read) ? (receive_window - bytes_read) : 0;

    // Less than half of the increment is left for the peer: move the window forward
    if (diff < receive_window_increment / 2)
    {
        uint64_t old_window_increment = receive_window_increment;

        maybe_adjust_window_increment(now_us, rtt_us);

        new_increment = 0;
        if (receive_window_increment != old_window_increment)
        {
            new_increment = receive_window_increment;
        }

        last_window_update_time = now_us;
        receive_window = bytes_read + receive_window_increment;
        new_offset = receive_window;
        return true;
    }

    new_increment = receive_window_increment;
    new_offset = receive_window;
    return false;
}

// Doubles the increment if we have to send WINDOW_UPDATEs too often
void FlowController::maybe_adjust_window_increment(uint64_t now_us, uint64_t rtt_us)
{
    if (last_window_update_time == 0)
    {
        return;
    }

    if (rtt_us == 0)
    {
        return;
    }

    uint64_t time_since_last_window_update = now_us - last_window_update_time;

    // The interval between the window updates is large enough, no need to increase the increment
    if (time_since_last_window_update >= 2 * rtt_us)
    {
        return;
    }

    receive_window_increment = min(2 * receive_window_increment, max_receive_window_increment);
}

void FlowController::ensure_minimum_window_increment(uint64_t inc)
{
    if (inc > receive_window_increment)
    {
        receive_window_increment = min(inc, max_receive_window_increment);
        last_window_update_time = 0; // disables the auto-tuning for the next window update
    }
}

bool FlowController::check_flow_control_violation() const
{
    return highest_received > receive_window;
}

uint64_t FlowController::get_receive_window() const
{
    return receive_window;
}

uint64_t FlowController::get_receive_window_increment() const
{
    return receive_window_increment;
}

uint64_t FlowController::get_highest_received() const
{
    return highest_received;
}

uint64_t FlowController::get_bytes_read() const
{
    return bytes_read;
}

FlowControlManager::FlowControlManager()
    : connection_flow_controller(0, false)
{
}

FlowController *FlowControlManager::get_flow_controller(uint32_t stream_id)
{
    if (stream_id == 0)
    {
        return &connection_flow_controller;
    }

    // The flow controller of a stream is created the first time the stream is used
    auto it = stream_flow_controllers.find(stream_id);
    if (it == stream_flow_controllers.end())
    {
        it = stream_flow_controllers.emplace(stream_id, FlowController(stream_id, true)).first;
    }
    return &it->second;
}

uint64_t FlowControlManager::send_window_size(uint32_t stream_id)
{
    FlowController *fc = get_flow_controller(stream_id);
    uint64_t res = fc->send_window_size();

    if (fc->get_contributes_to_connection())
    {
        res = min(res, connection_flow_controller.send_window_size());
    }
    return res;
}

void FlowControlManager::add_bytes_sent(uint32_t stream_id, uint64_t n)
{
    FlowController *fc = get_flow_controller(stream_id);
    fc->add_bytes_sent(n);

    if (fc->get_contributes_to_connection())
    {
        connection_flow_controller.add_bytes_sent(n);
    }
}

bool FlowControlManager::update_window(uint32_t stream_id, uint64_t byte_offset)
{
    return get_flow_controller(stream_id)->update_send_window(byte_offset);
}

bool FlowControlManager::update_highest_received(uint32_t stream_id, uint64_t byte_offset)
{
    FlowController *fc = get_flow_controller(stream_id);
    uint64_t increment = fc->update_highest_received(byte_offset);

    if (fc->check_flow_control_violation())
    {
        return false;
    }

    if (fc->get_contributes_to_connection())
    {
        connection_flow_controller.increment_highest_received(increment);
        if (connection_flow_controller.check_flow_control_violation())
        {
            return false;
        }
    }
    return true;
}

void FlowControlManager::add_bytes_read(uint32_t stream_id, uint64_t n, uint64_t now_us)
{
    FlowController *fc = get_flow_controller(stream_id);
    fc->add_bytes_read(n, now_us);

    if (fc->get_contributes_to_connection())
    {
        connection_flow_controller.add_bytes_read(n, now_us);
    }
}

vector<WindowUpdateFrame> FlowControlManager::get_window_updates(uint64_t now_us, uint64_t rtt_us)
{
    vector<WindowUpdateFrame> res;
    uint64_t new_increment = 0;
    uint64_t offset = 0;

    // WINDOW_UPDATEs of the streams
    for (auto &entry : stream_flow_controllers)
    {
        FlowController &fc = entry.second;
        if (fc.maybe_update_window(now_us, rtt_us, new_increment, offset))
        {
            WindowUpdateFrame frame;
            frame.stream_id = entry.first;
            frame.byte_offset = offset;
            res.push_back(frame);

            // When the window of a stream grows, the connection window must be able to follow
            if (fc.get_contributes_to_connection() && new_increment != 0)
            {
                connection_flow_controller.ensure_minimum_window_increment(
                    static_cast<uint64_t>(new_increment * CONNECTION_FLOW_CONTROL_MULTIPLIER));
            }
        }
    }

    // WINDOW_UPDATE of the connection
    if (connection_flow_controller.maybe_update_window(now_us, rtt_us, new_increment, offset))
    {
        WindowUpdateFrame frame;
        frame.stream_id = 0;
        frame.byte_offset = offset;
        res.push_back(frame);
    }

    return res;
}

uint64_t FlowControlManager::get_receive_window(uint32_t stream_id)
{
    return get_flow_controller(stream_id)->get_receive_window();
}
