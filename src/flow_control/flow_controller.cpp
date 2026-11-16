#include<bits/stdc++.h>

#include "flow_control/flow_controller.hpp"

using namespace std;

FlowController::FlowController(uint32_t stream_id, bool contributes_to_connection)
{
    this->stream_id=stream_id;
    this->contributes_to_connection=contributes_to_connection;

    if(stream_id==0){
        advertised_limit=FlowWindows::CONNECTION_INITIAL;
        max_window_step=FlowWindows::CONNECTION_MAX;
    }
    else{
        advertised_limit=FlowWindows::STREAM_INITIAL;
        max_window_step=FlowWindows::STREAM_MAX;
    }
    window_step=advertised_limit;

    // Both sides start with the same window size
    send_limit=advertised_limit;
}

bool FlowController::get_contributes_to_connection() const
{
    return contributes_to_connection;
}

void FlowController::add_bytes_sent(uint64_t n)
{
    sent_bytes+=n;
}

uint64_t FlowController::get_bytes_sent() const
{
    return sent_bytes;
}

bool FlowController::update_send_window(uint64_t new_offset)
{
    // Window only moves forward; ignore old delayed updates
    if(new_offset>send_limit){
        send_limit=new_offset;
        return true;
    }
    return false;
}

uint64_t FlowController::send_window_size() const
{
    if(sent_bytes>send_limit){
        return 0;
    }
    return send_limit-sent_bytes;
}

uint64_t FlowController::get_send_window() const
{
    return send_limit;
}

uint64_t FlowController::update_highest_received(uint64_t byte_offset)
{
    // Out-of-order older frames do not move the highest offset
    if(byte_offset<=highest_rx_offset){
        return 0;
    }
    uint64_t increment=byte_offset-highest_rx_offset;
    highest_rx_offset=byte_offset;
    return increment;
}

void FlowController::increment_highest_received(uint64_t increment)
{
    highest_rx_offset+=increment;
}

void FlowController::add_bytes_read(uint64_t n, uint64_t now_us)
{
    // Start the auto-tuning timer on the first read
    if(read_bytes==0){
        last_update_us=now_us;
    }
    read_bytes+=n;
}

bool FlowController::maybe_update_window(uint64_t now_us, uint64_t rtt_us, uint64_t &new_increment, uint64_t &new_offset)
{
    // read_bytes stays <= advertised_limit
    uint64_t room_left=(advertised_limit>read_bytes) ? (advertised_limit-read_bytes) : 0;

    // Send a new limit when less than half the window step is left
    if(room_left<window_step/2){
        uint64_t previous_step=window_step;

        autotune_step(now_us, rtt_us);

        new_increment=0;
        if(window_step!=previous_step){
            new_increment=window_step;
        }

        last_update_us=now_us;
        advertised_limit=read_bytes+window_step;
        new_offset=advertised_limit;
        return true;
    }

    new_increment=window_step;
    new_offset=advertised_limit;
    return false;
}

// Double window_step if two updates happen within 2 RTTs
void FlowController::autotune_step(uint64_t now_us, uint64_t rtt_us)
{
    // Need a previous update time and RTT sample
    if(last_update_us==0||rtt_us==0){
        return;
    }

    // Updates are far enough apart; no need to grow
    if(now_us-last_update_us>=2*rtt_us){
        return;
    }

    window_step=min(2*window_step, max_window_step);
}

void FlowController::ensure_minimum_window_increment(uint64_t inc)
{
    if(inc>window_step){
        window_step=min(inc, max_window_step);
        last_update_us=0; // skip auto-tuning on next update since step just grew
    }
}

bool FlowController::check_flow_control_violation() const
{
    return highest_rx_offset>advertised_limit;
}

uint64_t FlowController::get_receive_window() const
{
    return advertised_limit;
}

uint64_t FlowController::get_receive_window_increment() const
{
    return window_step;
}

uint64_t FlowController::get_highest_rx_offset() const
{
    return highest_rx_offset;
}

uint64_t FlowController::get_bytes_read() const
{
    return read_bytes;
}

FlowControlManager::FlowControlManager()
    : connection_level(0, false)
{
}

FlowController *FlowControlManager::get_flow_controller(uint32_t stream_id)
{
    if(stream_id==0){
        return &connection_level;
    }

    // Create stream controller on first use
    auto it=per_stream.find(stream_id);
    if(it==per_stream.end()){
        it=per_stream.emplace(stream_id, FlowController(stream_id, true)).first;
    }
    return &it->second;
}

uint64_t FlowControlManager::send_window_size(uint32_t stream_id)
{
    FlowController *flow=get_flow_controller(stream_id);
    uint64_t result=flow->send_window_size();

    if(flow->get_contributes_to_connection()){
        result=min(result, connection_level.send_window_size());
    }
    return result;
}

void FlowControlManager::add_bytes_sent(uint32_t stream_id, uint64_t n)
{
    FlowController *flow=get_flow_controller(stream_id);
    flow->add_bytes_sent(n);

    if(flow->get_contributes_to_connection()){
        connection_level.add_bytes_sent(n);
    }
}

bool FlowControlManager::update_window(uint32_t stream_id, uint64_t byte_offset)
{
    return get_flow_controller(stream_id)->update_send_window(byte_offset);
}

bool FlowControlManager::update_highest_received(uint32_t stream_id, uint64_t byte_offset)
{
    FlowController *flow=get_flow_controller(stream_id);
    uint64_t increment=flow->update_highest_received(byte_offset);

    if(flow->check_flow_control_violation()){
        return false;
    }

    if(flow->get_contributes_to_connection()){
        connection_level.increment_highest_received(increment);
        if(connection_level.check_flow_control_violation()){
            return false;
        }
    }
    return true;
}

void FlowControlManager::add_bytes_read(uint32_t stream_id, uint64_t n, uint64_t now_us)
{
    FlowController *flow=get_flow_controller(stream_id);
    flow->add_bytes_read(n, now_us);

    if(flow->get_contributes_to_connection()){
        connection_level.add_bytes_read(n, now_us);
    }
}

vector<WindowUpdateFrame> FlowControlManager::get_window_updates(uint64_t now_us, uint64_t rtt_us)
{
    vector<WindowUpdateFrame> result;
    uint64_t new_increment=0;
    uint64_t offset=0;

    // Stream WINDOW_UPDATEs
    for(auto &[stream_id, flow] : per_stream){
        if(flow.maybe_update_window(now_us, rtt_us, new_increment, offset)){
            WindowUpdateFrame frame;
            frame.stream_id=stream_id;
            frame.byte_offset=offset;
            result.push_back(frame);

            // Growing stream window also grows the connection window
            if(flow.get_contributes_to_connection()&&new_increment!=0){
                connection_level.ensure_minimum_window_increment(
                    static_cast<uint64_t>(new_increment*FlowWindows::CONNECTION_FACTOR));
            }
        }
    }

    // Connection WINDOW_UPDATE
    if(connection_level.maybe_update_window(now_us, rtt_us, new_increment, offset)){
        WindowUpdateFrame frame;
        frame.stream_id=0;
        frame.byte_offset=offset;
        result.push_back(frame);
    }

    return result;
}

uint64_t FlowControlManager::get_receive_window(uint32_t stream_id)
{
    return get_flow_controller(stream_id)->get_receive_window();
}
