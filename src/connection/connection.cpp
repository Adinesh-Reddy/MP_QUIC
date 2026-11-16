#include<bits/stdc++.h>

#include "connection/connection.hpp"

using namespace std;

Connection::Connection(uint64_t connection_id)
{
    this->connection_id=connection_id;
    this->scheduler=&default_scheduler;
    this->last_received_us=Packet::current_time_us();
}

const map<uint8_t, Path *> &Connection::get_paths() const
{
    return paths;
}

bool Connection::send_ping(uint8_t path_id)
{
    if(closed||get_path(path_id)==nullptr){
        return false;
    }
    pending_control[path_id].push_back(Frame::make_ping());
    send_pending();
    return true;
}

void Connection::advertise_addresses(const vector<NetworkAddress> &addresses)
{
    if(closed||paths.empty()||addresses.empty()){
        return;
    }

    // Send on next packet of a usable path
    Path *target=scheduler->select_path(paths, true);
    if(target==nullptr){
        target=paths.begin()->second;
    }
    pending_control[target->get_id()].push_back(Frame::make_add_address(addresses));
}

void Connection::on_add_address(AddAddressCallback cb)
{
    address_handler=cb;
}

void Connection::set_idle_timeout(uint64_t timeout_us)
{
    idle_timeout_us=timeout_us;
}

bool Connection::is_closed_by_peer() const
{
    return closed_by_peer;
}

void Connection::close(const string &reason)
{
    if(closed){
        return;
    }

    // Flush pending ACKs, then send close frame
    send_pending();
    broadcast_close(CLOSE_NO_ERROR, reason);

    closed=true;
    close_reason=reason;
}

// Send CONNECTION_CLOSE once on every path (idle timeout catches if all are lost)
void Connection::broadcast_close(uint32_t error_code, const string &reason)
{
    uint64_t now_us=Packet::current_time_us();
    for(const auto &[path_id, path] : paths){
        Packet packet(connection_id, path->get_id(), path->peek_packet_number());
        packet.add_frame(Frame::make_connection_close(error_code, reason));
        send_packet(path, packet, now_us);
    }
}

uint64_t Connection::get_connection_id() const
{
    return connection_id;
}

bool Connection::add_path(Path *path)
{
    if(path==nullptr||paths.find(path->get_id())!=paths.end()){
        return false;
    }
    paths[path->get_id()]=path;

    // Link path sender into OLIA coupled table
    coupled_senders[path->get_id()]=path->get_congestion();
    path->get_congestion()->set_olia_senders(&coupled_senders);
    return true;
}

Path *Connection::get_path(uint8_t path_id)
{
    auto it=paths.find(path_id);
    return (it==paths.end()) ? nullptr : it->second;
}

void Connection::on_stream_data(StreamDataCallback cb)
{
    data_handler=cb;
}

void Connection::write(uint32_t stream_id, const uint8_t *data, size_t length)
{
    // Create stream on first write if needed
    send_streams.try_emplace(stream_id, stream_id).first->second.write(data, length);
}

void Connection::close_stream(uint32_t stream_id)
{
    send_streams.try_emplace(stream_id, stream_id).first->second.close();
}

size_t Connection::unsent_bytes(uint32_t stream_id) const
{
    auto it=send_streams.find(stream_id);
    return (it==send_streams.end()) ? 0 : it->second.get_unsent_bytes();
}

bool Connection::has_unsent_stream_data() const
{
    return any_of(send_streams.begin(), send_streams.end(),
                  [](const auto &entry) { return entry.second.has_data(); });
}

bool Connection::has_stream_data() const
{
    if(has_unsent_stream_data()||!lost_stream_frames.empty()){
        return true;
    }

    // Lost packets waiting to be collected
    return any_of(paths.begin(), paths.end(), [](const auto &entry) {
        return entry.second->get_sent_packet_handler()->has_retransmissions();
    });
}

uint64_t Connection::get_bytes_in_flight() const
{
    uint64_t total=0;
    for(const auto &[path_id, path] : paths){
        total+=path->get_sent_packet_handler()->get_bytes_in_flight();
    }
    return total;
}

FlowControlManager *Connection::get_flow_control_manager()
{
    return &flow_control_manager;
}

bool Connection::is_closed() const
{
    return closed;
}

string Connection::get_close_reason() const
{
    return close_reason;
}

void Connection::abort_with(const string &reason, uint32_t error_code)
{
    if(closed){
        return;
    }

    // Notify peer then close
    broadcast_close(error_code, reason);

    closed=true;
    close_reason=reason;
    cout<<"[CONN] Connection "<<connection_id<<" closed: "<<reason<<"\n";
}

// Auto-tuning uses the slowest path's smoothed RTT
uint64_t Connection::largest_path_rtt()
{
    uint64_t largest=0;
    for(const auto &[path_id, path] : paths){
        largest=max(largest, path->get_rtt_stats()->get_smoothed_rtt());
    }
    return largest;
}

void Connection::set_scheduler(Scheduler *new_scheduler)
{
    if(new_scheduler!=nullptr){
        scheduler=new_scheduler;
    }
}

Scheduler *Connection::get_scheduler()
{
    return scheduler;
}

uint64_t Connection::get_duplicated_packets() const
{
    return copies_sent;
}

uint64_t Connection::get_ping_probes() const
{
    return probes_queued;
}

uint64_t Connection::get_paths_frames_sent() const
{
    return path_reports_sent;
}

uint64_t Connection::get_paths_frames_received() const
{
    return path_reports_received;
}

// Send PATHS frame listing path RTTs and broken paths so peer stops using dead paths right away
void Connection::announce_path_states()
{
    if(paths.size()<2){
        return; // only 1 path, no backup path
    }

    PathsFrame info;
    for(const auto &[path_id, path] : paths){
        PathInfo path_info;
        path_info.path_id=path_id;
        if(path->is_potentially_failed()){
            path_info.rtt_us=PATHS_RTT_POTENTIALLY_FAILED;
        }
        else{
            uint64_t rtt=path->get_rtt_stats()->get_smoothed_rtt();
            path_info.rtt_us=static_cast<uint32_t>(min<uint64_t>(rtt, PATHS_RTT_POTENTIALLY_FAILED-1));
        }
        info.paths.push_back(path_info);
    }

    // Drop any older queued PATHS frame
    for(auto &[path_id, queue] : pending_control){
        queue.erase(remove_if(queue.begin(), queue.end(),
                              [](const Frame &frame) { return frame.type==FRAME_PATHS; }),
                    queue.end());
    }

    // Pick a usable path (control frames ignore congestion window)
    Path *target=scheduler->select_path(paths, true);
    if(target==nullptr){
        return;
    }
    pending_control[target->get_id()].push_back(Frame::make_paths(info));
    path_reports_sent++;
}

bool Connection::all_data_acknowledged() const
{
    if(has_stream_data()){
        return false;
    }
    return none_of(paths.begin(), paths.end(), [](const auto &entry) {
        return entry.second->get_sent_packet_handler()->has_outstanding_stream_frames();
    });
}

// Queue one PING on a broken path to detect when it comes back
void Connection::probe_with_ping(Path *path)
{
    deque<Frame> &queue=pending_control[path->get_id()];

    // One queued PING is enough
    bool already_queued=any_of(queue.begin(), queue.end(),
                                 [](const Frame &frame) { return frame.type==FRAME_PING; });
    if(already_queued){
        return;
    }

    queue.push_back(Frame::make_ping());
    probes_queued++;
}

// Queue WINDOW_UPDATE on all paths so one broken path does not stall the sender
void Connection::schedule_window_updates(uint64_t now_us)
{
    vector<WindowUpdateFrame> updates=flow_control_manager.get_window_updates(now_us, largest_path_rtt());

    for(const auto &update : updates){
        for(const auto &[path_id, path] : paths){
            pending_control[path_id].push_back(Frame::make_window_update(update.stream_id, update.byte_offset));
        }
    }
}

// Collect lost packets from all paths and requeue frames (STREAM data can use any path)
bool Connection::gather_lost_frames()
{
    bool has_retransmission=false;
    bool paths_frame_lost=false;

    for(const auto &[path_id, path] : paths){
        bool lost_on_this_path=false;

        SentPacket lost_packet;
        while(path->get_sent_packet_handler()->dequeue_packet_for_retransmission(lost_packet)){
            has_retransmission=true;
            lost_on_this_path=true;

            for(const auto &frame : lost_packet.frames){
                switch(frame.type){
                case FRAME_STREAM:
                    lost_stream_frames.push_back(frame);
                    break;
                case FRAME_WINDOW_UPDATE:
                    // Only resend if still >= current receive window
                    if(frame.window_update.byte_offset>=flow_control_manager.get_receive_window(frame.window_update.stream_id)){
                        pending_control[path_id].push_back(frame);
                    }
                    break;
                case FRAME_PATHS:
                    // Rebuild fresh PATHS frame below
                    paths_frame_lost=true;
                    break;
                default:
                    pending_control[path_id].push_back(frame);
                    break;
                }
            }
        }

        // Probe broken path with PING
        if(lost_on_this_path&&path->is_potentially_failed()){
            probe_with_ping(path);
        }
    }

    if(paths_frame_lost){
        announce_path_states();
    }

    return has_retransmission;
}

// Attach ACK frame to packet if one is due
void Connection::attach_ack(Path *path, Packet &packet, uint64_t now_us)
{
    // Leave for next packet if not enough room (check before get_ack_frame resets state)
    if(packet.size()+ACK_FRAME_HEADER_SIZE+MAX_VARINT_SIZE>MAX_PACKET_SIZE){
        return;
    }

    AckFrame ack;
    if(!path->get_ack_frame(now_us, ack)){
        return;
    }

    // Drop oldest ACK ranges if packet is too full
    Frame frame=Frame::make_ack(ack);
    while(frame.ack.ranges.size()>1&&packet.size()+frame.size()>MAX_PACKET_SIZE){
        frame.ack.ranges.pop_back();
    }
    packet.add_frame(frame);
}

void Connection::attach_control_frames(Path *path, Packet &packet)
{
    deque<Frame> &queue=pending_control[path->get_id()];
    while(!queue.empty()&&packet.size()+queue.front().size()<=MAX_PACKET_SIZE){
        packet.add_frame(queue.front());
        queue.pop_front();
    }
}

void Connection::attach_lost_data(Packet &packet)
{
    while(!lost_stream_frames.empty()){
        Frame &frame=lost_stream_frames.front();

        size_t space=MAX_PACKET_SIZE-packet.size();
        if(space<=STREAM_FRAME_HEADER_SIZE){
            break;
        }
        size_t max_data=space-STREAM_FRAME_HEADER_SIZE;

        if(frame.stream.data.size()>max_data){
            // Split frame to fit remaining packet space
            vector<uint8_t> first_part(frame.stream.data.begin(), frame.stream.data.begin()+max_data);
            packet.add_frame(Frame::make_stream(frame.stream.stream_id, frame.stream.offset, false, first_part));

            frame.stream.data.erase(frame.stream.data.begin(), frame.stream.data.begin()+max_data);
            frame.stream.offset+=max_data;
            break;
        }

        packet.add_frame(frame);
        lost_stream_frames.pop_front();
    }
}

void Connection::attach_new_data(Packet &packet)
{
    for(auto &[stream_id, stream] : send_streams){
        Frame frame;

        while(packet.size()<MAX_PACKET_SIZE&&
               stream.pop_stream_frame(MAX_PACKET_SIZE-packet.size(), frame,
                                             flow_control_manager.send_window_size(stream_id))){
            // Only new data counts against flow control (resends were counted earlier)
            flow_control_manager.add_bytes_sent(stream_id, frame.stream.data.size());
            packet.add_frame(frame);
        }
    }
}

void Connection::send_packet(Path *path, const Packet &packet, uint64_t now_us)
{
    vector<uint8_t> raw=packet.serialize();

    // Record packet as sent even if socket send fails (recovered like a network drop)
    path->send(raw.data(), raw.size());

    path->pop_packet_number();
    path->get_sent_packet_handler()->sent_packet(packet.get_packet_number(), packet.get_frames(), raw.size(), now_us);
}

// Copy stream data onto a backup path while sent_on has no RTT yet (no extra flow control cost)
void Connection::send_startup_copy(Path *sent_on, const Packet &packet, uint64_t now_us)
{
    Path *other_path=scheduler->select_duplicate_path(paths, sent_on);
    if(other_path==nullptr){
        return;
    }

    Packet duplicate(connection_id, other_path->get_id(), other_path->peek_packet_number());

    // Copy STREAM frames with same offsets
    for(const auto &frame : packet.get_frames()){
        if(frame.type==FRAME_STREAM){
            duplicate.add_frame(frame);
        }
    }
    if(duplicate.get_frames().empty()){
        return;
    }

    // Piggyback pending ACK/control frames on duplicate packet
    attach_ack(other_path, duplicate, now_us);
    attach_control_frames(other_path, duplicate);

    send_packet(other_path, duplicate, now_us);
    scheduler->on_packet_sent(other_path);
    copies_sent++;
}

void Connection::send_pending()
{
    if(closed){
        return;
    }

    uint64_t now_us=Packet::current_time_us();

    schedule_window_updates(now_us);

    // Build packets until nothing left to send or all paths are full
    while(true){
        // Lost data goes first
        bool has_retransmission=gather_lost_frames();

        if(lost_stream_frames.empty()&&!has_unsent_stream_data()){
            break;
        }

        // Pick best path (lowest RTT)
        Path *path=scheduler->select_path(paths, has_retransmission);
        if(path==nullptr){
            break; // no usable path right now
        }

        // Use next packet number of this path
        Packet packet(connection_id, path->get_id(), path->peek_packet_number());

        attach_ack(path, packet, now_us);
        attach_control_frames(path, packet);
        attach_lost_data(packet);
        attach_new_data(packet);

        if(packet.get_frames().empty()){
            break;
        }

        const vector<Frame> &frames=packet.get_frames();
        bool has_stream_frame=any_of(frames.begin(), frames.end(),
                                       [](const Frame &frame) { return frame.type==FRAME_STREAM; });

        send_packet(path, packet, now_us);
        scheduler->on_packet_sent(path);

        // Blocked by flow control until WINDOW_UPDATE arrives
        if(!has_stream_frame){
            break;
        }

        // Duplicate on another path if RTT is still unknown
        send_startup_copy(path, packet, now_us);
    }

    flush_acks_and_control(now_us);
}

// Send leftover ACKs and control frames (not blocked by congestion window)
void Connection::flush_acks_and_control(uint64_t now_us)
{
    for(const auto &[path_id, path] : paths){
        while(true){
            Packet packet(connection_id, path->get_id(), path->peek_packet_number());

            attach_ack(path, packet, now_us);
            attach_control_frames(path, packet);

            if(packet.get_frames().empty()){
                break;
            }
            send_packet(path, packet, now_us);
        }
    }
}

void Connection::handle_packet(const uint8_t *data, size_t length)
{
    if(closed){
        return;
    }

    Packet packet;
    if(!Packet::deserialize(data, length, packet)){
        return; // malformed packet
    }

    if(packet.get_connection_id()!=connection_id){
        return; // not for this connection
    }

    // Look up path from header Path ID
    Path *path=get_path(packet.get_path_id());
    if(path==nullptr){
        return;
    }

    uint64_t now_us=Packet::current_time_us();

    ReceivedPacketHandler *received_handler=path->get_received_packet_handler();
    bool duplicate=received_handler->is_duplicate(packet.get_packet_number());

    if(!received_handler->received_packet(packet.get_packet_number(), packet.is_retransmittable(), now_us)){
        return; // bad packet number: ignore without resetting timers
    }

    // Valid packet: refresh idle timer and mark path alive
    path->set_last_network_activity_time(now_us);
    last_received_us=now_us;
    path->set_potentially_failed(false);

    // Still ACK duplicates, but do not process their frames twice
    if(!duplicate){
        for(const auto &frame : packet.get_frames()){
            dispatch_frame(frame, packet, now_us);
            if(closed){
                return;
            }
        }
    }

    // Send any ACKs or newly unblocked data
    send_pending();
}

void Connection::dispatch_frame(const Frame &frame, const Packet &packet, uint64_t now_us)
{
    switch(frame.type){
    case FRAME_STREAM:
        deliver_stream_frame(frame.stream, now_us);
        break;
    case FRAME_ACK:
    {
        // ACK frame's path_id may differ from the path that carried it
        Path *acked_path=get_path(frame.ack.path_id);
        if(acked_path==nullptr){
            break;
        }

        // Carrier packet number only orders ACKs that arrive on the same path
        uint64_t with_packet_number=0;
        if(frame.ack.path_id==packet.get_path_id()){
            with_packet_number=packet.get_packet_number();
        }

        if(!acked_path->get_sent_packet_handler()->received_ack(frame.ack, with_packet_number, now_us)){
            abort_with("the other side acknowledged a packet that we never sent (protocol error)");
        }
        break;
    }
    case FRAME_WINDOW_UPDATE:
        // Peer increased send window
        flow_control_manager.update_window(frame.window_update.stream_id, frame.window_update.byte_offset);
        break;
    case FRAME_PING:
        break; // ACK already scheduled
    case FRAME_ADD_ADDRESS:
        // Peer sent its local addresses; PathManager can open paths to them
        if(address_handler!=nullptr){
            address_handler(frame.add_address.addresses);
        }
        break;
    case FRAME_CONNECTION_CLOSE:
        // Peer closed the connection
        closed=true;
        closed_by_peer=true;
        close_reason="the other side closed the connection";
        if(!frame.connection_close.reason.empty()){
            close_reason+=" (its reason: "+frame.connection_close.reason+")";
        }
        break;
    case FRAME_PATHS:
    {
        // Mark paths the peer says look broken (cleared once a packet arrives on them)
        path_reports_received++;
        for(const auto &path_info : frame.paths.paths){
            if(path_info.rtt_us!=PATHS_RTT_POTENTIALLY_FAILED){
                continue;
            }
            // Packet just arrived on this path, so it works
            if(path_info.path_id==packet.get_path_id()){
                continue;
            }
            Path *failed_path=get_path(path_info.path_id);
            if(failed_path!=nullptr){
                failed_path->set_potentially_failed(true);
            }
        }
        break;
    }
    default:
        break;
    }
}

void Connection::deliver_stream_frame(const StreamFrame &frame, uint64_t now_us)
{
    uint32_t stream_id=frame.stream_id;

    // Check flow control limit
    uint64_t max_offset=frame.offset+frame.data.size();
    if(!flow_control_manager.update_highest_received(stream_id, max_offset)){
        abort_with("the other side sent more data than we allowed it to send (flow control limit broken)");
        return;
    }

    ReceiveStream &stream=receive_streams.try_emplace(stream_id, stream_id).first->second;

    // Buffer frame until earlier bytes arrive
    if(!stream.add_stream_frame(frame)){
        abort_with("the other side changed where stream "+to_string(stream_id)+
                   " ends (its STREAM frames disagree about the last byte; protocol error)");
        return;
    }

    // Deliver contiguous in-order bytes to app
    uint64_t offset=0;
    vector<uint8_t> data;
    bool fin=false;
    while(stream.pop(offset, data, fin)){
        // Update flow control after app reads data
        flow_control_manager.add_bytes_read(stream_id, data.size(), now_us);

        if(data_handler!=nullptr){
            data_handler(stream_id, offset, data, fin);
        }
    }
}

void Connection::on_tick()
{
    if(closed){
        return;
    }

    uint64_t now_us=Packet::current_time_us();

    // Close if nothing heard from peer within idle_timeout_us
    if(idle_timeout_us!=0&&now_us>last_received_us&&now_us-last_received_us>idle_timeout_us){
        // Format in seconds if whole seconds, else ms
        string limit=(idle_timeout_us%1000000==0) ? to_string(idle_timeout_us/1000000)+" s"
                                                        : to_string(idle_timeout_us/1000)+" ms";
        abort_with("nothing was received from the other side for "+limit+" (idle timeout)",
                         CLOSE_IDLE_TIMEOUT);
        return;
    }

    // Run loss-recovery timers on every path
    bool path_newly_failed=false;
    for(const auto &[path_id, path] : paths){
        bool was_failed=path->is_potentially_failed();

        SentPacketHandler *sent_handler=path->get_sent_packet_handler();
        uint64_t alarm=sent_handler->get_alarm_timeout();
        if(alarm!=0&&alarm<=now_us){
            sent_handler->on_alarm(now_us);
        }

        if(!was_failed&&path->is_potentially_failed()){
            path_newly_failed=true;
        }
    }

    // Tell peer via PATHS frame if a path just failed
    if(path_newly_failed){
        announce_path_states();
    }

    send_pending();
}
