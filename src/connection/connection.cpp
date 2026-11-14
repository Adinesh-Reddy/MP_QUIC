#include <bits/stdc++.h>

#include "connection/connection.hpp"

using namespace std;

Connection::Connection(uint64_t connection_id)
{
    this->connection_id = connection_id;
    this->scheduler = &default_scheduler;
    this->last_activity_us = Packet::current_time_us();
}

const map<uint8_t, Path *> &Connection::get_paths() const
{
    return paths;
}

bool Connection::send_ping(uint8_t path_id)
{
    if (closed || get_path(path_id) == nullptr)
    {
        return false;
    }
    control_frames[path_id].push_back(Frame::make_ping());
    send_pending();
    return true;
}

void Connection::advertise_addresses(const vector<NetworkAddress> &addresses)
{
    if (closed || paths.empty() || addresses.empty())
    {
        return;
    }

    // It travels on a usable path, with the next packet sent on it
    Path *target = scheduler->select_path(paths, true);
    if (target == nullptr)
    {
        target = paths.begin()->second;
    }
    control_frames[target->get_id()].push_back(Frame::make_add_address(addresses));
}

void Connection::on_add_address(AddAddressCallback cb)
{
    add_address_callback = cb;
}

void Connection::set_idle_timeout(uint64_t timeout_us)
{
    idle_timeout_us = timeout_us;
}

bool Connection::is_closed_by_peer() const
{
    return closed_by_peer;
}

void Connection::close(const string &reason)
{
    if (closed)
    {
        return;
    }

    // The ACKs that are due leave first, then the peer is told that we are done
    send_pending();
    send_connection_close(CLOSE_NO_ERROR, reason);

    closed = true;
    close_reason = reason;
}

// One packet with a CONNECTION_CLOSE frame on every path: if a path is broken the frame
// still arrives on the other one. It is not retransmitted; if all the copies are lost the
// peer closes with its idle timeout.
void Connection::send_connection_close(uint32_t error_code, const string &reason)
{
    uint64_t now_us = Packet::current_time_us();
    for (auto &entry : paths)
    {
        Path *path = entry.second;
        Packet packet(FLAG_NONE, connection_id, path->get_id(), path->peek_packet_number());
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
    if (path == nullptr || paths.find(path->get_id()) != paths.end())
    {
        return false;
    }
    paths[path->get_id()] = path;

    // Coupled congestion control: the sender of this path joins the others
    olia_senders[path->get_id()] = path->get_congestion();
    path->get_congestion()->set_olia_senders(&olia_senders);
    return true;
}

Path *Connection::get_path(uint8_t path_id)
{
    auto it = paths.find(path_id);
    if (it == paths.end())
    {
        return nullptr;
    }
    return it->second;
}

void Connection::on_stream_data(StreamDataCallback cb)
{
    stream_callback = cb;
}

void Connection::write(uint32_t stream_id, const uint8_t *data, size_t length)
{
    auto it = send_streams.find(stream_id);
    if (it == send_streams.end())
    {
        it = send_streams.emplace(stream_id, SendStream(stream_id)).first;
    }
    it->second.write(data, length);
}

void Connection::close_stream(uint32_t stream_id)
{
    auto it = send_streams.find(stream_id);
    if (it == send_streams.end())
    {
        it = send_streams.emplace(stream_id, SendStream(stream_id)).first;
    }
    it->second.close();
}

bool Connection::has_new_stream_data() const
{
    for (const auto &entry : send_streams)
    {
        if (entry.second.has_data())
            return true;
    }
    return false;
}

bool Connection::has_stream_data() const
{
    if (has_new_stream_data() || !retransmission_queue.empty())
    {
        return true;
    }

    // Lost packets whose frames were not taken over yet
    for (const auto &entry : paths)
    {
        if (entry.second->get_sent_packet_handler()->has_retransmissions())
            return true;
    }
    return false;
}

uint64_t Connection::get_bytes_in_flight() const
{
    uint64_t total = 0;
    for (const auto &entry : paths)
    {
        total += entry.second->get_sent_packet_handler()->get_bytes_in_flight();
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

void Connection::close_with_error(const string &reason, uint32_t error_code)
{
    if (!closed)
    {
        // The peer is told why, then we stop
        send_connection_close(error_code, reason);

        closed = true;
        close_reason = reason;
        cout << "Connection " << connection_id << " closed: " << reason << "\n";
    }
}

// The flow control auto-tuning uses the largest smoothed RTT of all the paths
uint64_t Connection::get_max_smoothed_rtt()
{
    uint64_t max_rtt = 0;
    for (auto &entry : paths)
    {
        max_rtt = max(max_rtt, entry.second->get_rtt_stats()->get_smoothed_rtt());
    }
    return max_rtt;
}

void Connection::set_scheduler(Scheduler *new_scheduler)
{
    if (new_scheduler != nullptr)
    {
        scheduler = new_scheduler;
    }
}

Scheduler *Connection::get_scheduler()
{
    return scheduler;
}

uint64_t Connection::get_duplicated_packets() const
{
    return duplicated_packets;
}

uint64_t Connection::get_ping_probes() const
{
    return ping_probes;
}

uint64_t Connection::get_paths_frames_sent() const
{
    return paths_frames_sent;
}

uint64_t Connection::get_paths_frames_received() const
{
    return paths_frames_received;
}

// Builds a PATHS frame: the list of our paths with their smoothed RTT, a potentially failed
// path being marked with a special RTT value. It is queued on a path that works.
// Without it the peer would keep answering on the broken path and only find out
// with a retransmission timeout of its own (paper, Section 4.3).
void Connection::queue_paths_frame()
{
    if (paths.size() < 2)
    {
        return; // with a single path there is no other path to tell the peer about
    }

    PathsFrame info;
    for (auto &entry : paths)
    {
        PathInfo path_info;
        path_info.path_id = entry.first;
        if (entry.second->is_potentially_failed())
        {
            path_info.rtt_us = PATHS_RTT_POTENTIALLY_FAILED;
        }
        else
        {
            uint64_t rtt = entry.second->get_rtt_stats()->get_smoothed_rtt();
            path_info.rtt_us = static_cast<uint32_t>(min<uint64_t>(rtt, PATHS_RTT_POTENTIALLY_FAILED - 1));
        }
        info.paths.push_back(path_info);
    }

    // A PATHS frame that is still waiting to be sent is out of date now
    for (auto &entry : control_frames)
    {
        deque<Frame> &queue = entry.second;
        queue.erase(remove_if(queue.begin(), queue.end(),
                              [](const Frame &frame) { return frame.type == FRAME_PATHS; }),
                    queue.end());
    }

    // The scheduler gives a usable path (the congestion window does not hold back a control frame)
    Path *target = scheduler->select_path(paths, true);
    if (target == nullptr)
    {
        return;
    }
    control_frames[target->get_id()].push_back(Frame::make_paths(info));
    paths_frames_sent++;
}

bool Connection::all_data_acknowledged() const
{
    if (has_stream_data())
    {
        return false;
    }
    for (const auto &entry : paths)
    {
        if (entry.second->get_sent_packet_handler()->has_outstanding_stream_frames())
            return false;
    }
    return true;
}

// Queues one PING on a potentially failed path. If the path works again the peer answers
// with an ACK on it, and receiving that packet clears the "potentially failed" state.
// If the PING is lost, the RTO of the path gives it back and it is sent again (each time
// after a longer wait, because the RTO doubles), so the path keeps being tested.
void Connection::queue_ping_probe(Path *path)
{
    deque<Frame> &queue = control_frames[path->get_id()];

    // One probe at a time is enough
    for (const auto &frame : queue)
    {
        if (frame.type == FRAME_PING)
            return;
    }

    queue.push_back(Frame::make_ping());
    ping_probes++;
}

// Asks the flow control if the receive windows must be moved forward. The resulting
// WINDOW_UPDATE frames are queued on ALL the paths: if they were lost on a slow or
// broken path the sender would stay blocked (paper, Section 3, Packet Scheduling).
void Connection::queue_window_updates(uint64_t now_us)
{
    vector<WindowUpdateFrame> updates = flow_control_manager.get_window_updates(now_us, get_max_smoothed_rtt());

    for (const auto &update : updates)
    {
        for (auto &entry : paths)
        {
            control_frames[entry.first].push_back(Frame::make_window_update(update.stream_id, update.byte_offset));
        }
    }
}

// Takes the packets that the paths declared lost and queues their frames again.
// The frames are independent of the packet (and of the path) that carried them, so a
// STREAM frame goes to the common retransmission queue and can leave on any path.
// Returns true if at least one lost packet was found.
bool Connection::collect_retransmissions()
{
    bool has_retransmission = false;
    bool paths_frame_lost = false;

    for (auto &entry : paths)
    {
        bool lost_on_this_path = false;

        SentPacket lost_packet;
        while (entry.second->get_sent_packet_handler()->dequeue_packet_for_retransmission(lost_packet))
        {
            has_retransmission = true;
            lost_on_this_path = true;

            for (const auto &frame : lost_packet.frames)
            {
                switch (frame.type)
                {
                case FRAME_STREAM:
                    retransmission_queue.push_back(frame);
                    break;
                case FRAME_WINDOW_UPDATE:
                    // Only retransmit a WINDOW_UPDATE if we haven't sent another one with a higher offset since
                    if (frame.window_update.byte_offset >= flow_control_manager.get_receive_window(frame.window_update.stream_id))
                    {
                        control_frames[entry.first].push_back(frame);
                    }
                    break;
                case FRAME_PATHS:
                    // The state of the paths may have changed since: a new frame is built below
                    paths_frame_lost = true;
                    break;
                default:
                    control_frames[entry.first].push_back(frame);
                    break;
                }
            }
        }

        // And try pinging on potentially failed paths
        if (lost_on_this_path && entry.second->is_potentially_failed())
        {
            queue_ping_probe(entry.second);
        }
    }

    if (paths_frame_lost)
    {
        queue_paths_frame();
    }

    return has_retransmission;
}

// If an ACK is due on this path, it is put in the packet
void Connection::add_ack_frame(Path *path, Packet &packet, uint64_t now_us)
{
    // No room for an ACK with at least one range: leave it for the next packet
    // (must be checked first, asking the path for the ACK frame resets its ACK state)
    if (packet.size() + ACK_FRAME_HEADER_SIZE + MAX_VARINT_SIZE > MAX_PACKET_SIZE)
    {
        return;
    }

    AckFrame ack;
    if (!path->get_ack_frame(now_us, ack))
    {
        return;
    }

    // Up to 256 ranges normally fit. If the packet is already partly filled (or the numbers
    // are unusually large) the lowest ranges are left out: the highest ones are the newest.
    Frame frame = Frame::make_ack(ack);
    while (frame.ack.ranges.size() > 1 && packet.size() + frame.size() > MAX_PACKET_SIZE)
    {
        frame.ack.ranges.pop_back();
    }
    packet.add_frame(frame);
}

void Connection::add_control_frames(Path *path, Packet &packet)
{
    deque<Frame> &queue = control_frames[path->get_id()];
    while (!queue.empty() && packet.size() + queue.front().size() <= MAX_PACKET_SIZE)
    {
        packet.add_frame(queue.front());
        queue.pop_front();
    }
}

void Connection::add_retransmission_frames(Packet &packet)
{
    while (!retransmission_queue.empty())
    {
        Frame &frame = retransmission_queue.front();

        size_t space = MAX_PACKET_SIZE - packet.size();
        if (space <= STREAM_FRAME_HEADER_SIZE)
        {
            break;
        }
        size_t max_data = space - STREAM_FRAME_HEADER_SIZE;

        if (frame.stream.data.size() > max_data)
        {
            // The frame does not fit (the packet already carries other frames):
            // send the first part now, the rest stays in the queue
            vector<uint8_t> first_part(frame.stream.data.begin(), frame.stream.data.begin() + max_data);
            packet.add_frame(Frame::make_stream(frame.stream.stream_id, frame.stream.offset, false, first_part));

            frame.stream.data.erase(frame.stream.data.begin(), frame.stream.data.begin() + max_data);
            frame.stream.offset += max_data;
            break;
        }

        packet.add_frame(frame);
        retransmission_queue.pop_front();
    }
}

void Connection::add_new_stream_frames(Packet &packet)
{
    for (auto &entry : send_streams)
    {
        uint32_t stream_id = entry.first;
        Frame frame;

        while (packet.size() < MAX_PACKET_SIZE &&
               entry.second.pop_stream_frame(MAX_PACKET_SIZE - packet.size(), frame,
                                             flow_control_manager.send_window_size(stream_id)))
        {
            // New data uses flow control credit (retransmitted data does not, it was counted already)
            flow_control_manager.add_bytes_sent(stream_id, frame.stream.data.size());
            packet.add_frame(frame);
        }
    }
}

void Connection::send_packet(Path *path, const Packet &packet, uint64_t now_us)
{
    vector<uint8_t> raw = packet.serialize();

    // If the socket refuses the datagram we still record the packet as sent: for the
    // protocol this is the same as a packet lost in the network, and it is recovered the same way
    path->send(raw.data(), raw.size());

    path->pop_packet_number();
    path->get_sent_packet_handler()->sent_packet(packet.get_packet_number(), packet.get_frames(), raw.size(), now_us);
}

// Start-up duplication (paper, Section 3, Packet Scheduling). When stream data was just sent
// on a path whose RTT is still unknown, the scheduler can ask for a copy on another path.
// The copy is a normal packet of that other path (own packet number, tracked and
// acknowledged separately). The receiver keeps whichever copy arrives first and its
// reorder buffer drops the other one.
void Connection::send_duplicate(Path *sent_on, const Packet &packet, uint64_t now_us)
{
    Path *other_path = scheduler->select_duplicate_path(paths, sent_on);
    if (other_path == nullptr)
    {
        return;
    }

    Packet duplicate(FLAG_NONE, connection_id, other_path->get_id(), other_path->peek_packet_number());

    // Same STREAM frames, same offsets. They use no new flow control credit.
    for (const auto &frame : packet.get_frames())
    {
        if (frame.type == FRAME_STREAM)
        {
            duplicate.add_frame(frame);
        }
    }
    if (duplicate.get_frames().empty())
    {
        return;
    }

    // What that path has to send anyway travels along if there is room left
    add_ack_frame(other_path, duplicate, now_us);
    add_control_frames(other_path, duplicate);

    send_packet(other_path, duplicate, now_us);
    scheduler->on_packet_sent(other_path);
    duplicated_packets++;
}

void Connection::send_pending()
{
    if (closed)
    {
        return;
    }

    uint64_t now_us = Packet::current_time_us();

    queue_window_updates(now_us);

    // Repeatedly try sending until we don't have any more data, or run out of the congestion window
    while (true)
    {
        // We first check for retransmissions
        bool has_retransmission = collect_retransmissions();

        if (retransmission_queue.empty() && !has_new_stream_data())
        {
            break;
        }

        // The scheduler chooses the path (lowest smoothed RTT among the usable paths)
        Path *path = scheduler->select_path(paths, has_retransmission);
        if (path == nullptr)
        {
            break; // no path can be used now
        }

        // A packet takes the next number of the path it is sent on,
        // so retransmitted frames always travel under a new packet number
        Packet packet(FLAG_NONE, connection_id, path->get_id(), path->peek_packet_number());

        add_ack_frame(path, packet, now_us);
        add_control_frames(path, packet);
        add_retransmission_frames(packet);
        add_new_stream_frames(packet);

        if (packet.get_frames().empty())
        {
            break;
        }

        bool has_stream_frame = false;
        for (const auto &frame : packet.get_frames())
        {
            if (frame.type == FRAME_STREAM)
                has_stream_frame = true;
        }

        send_packet(path, packet, now_us);
        scheduler->on_packet_sent(path);

        // No stream data could be added: we are blocked by the flow control,
        // nothing more to do until a WINDOW_UPDATE arrives
        if (!has_stream_frame)
        {
            break;
        }

        // Duplicate traffic when it was sent on an unknown performing path
        send_duplicate(path, packet, now_us);
    }

    send_acks_and_control_frames(now_us);
}

// Sends what could not travel with data: the ACK of each path (on the path where the
// packets were received) and the control frames queued for each path.
// These small packets are not held back by the congestion window.
void Connection::send_acks_and_control_frames(uint64_t now_us)
{
    for (auto &entry : paths)
    {
        Path *path = entry.second;

        while (true)
        {
            Packet packet(FLAG_NONE, connection_id, path->get_id(), path->peek_packet_number());

            add_ack_frame(path, packet, now_us);
            add_control_frames(path, packet);

            if (packet.get_frames().empty())
            {
                break;
            }
            send_packet(path, packet, now_us);
        }
    }
}

void Connection::handle_packet(const uint8_t *data, size_t length)
{
    if (closed)
    {
        return;
    }

    Packet packet;
    if (!Packet::deserialize(data, length, packet))
    {
        return; // malformed packet
    }

    if (packet.get_connection_id() != connection_id)
    {
        return; // not for this connection
    }

    // The Path ID in the header tells on which path the packet was sent
    Path *path = get_path(packet.get_path_id());
    if (path == nullptr)
    {
        return;
    }

    uint64_t now_us = Packet::current_time_us();
    path->set_last_network_activity_time(now_us);
    last_activity_us = now_us;

    // We just received a packet on that path, so it works
    path->set_potentially_failed(false);

    ReceivedPacketHandler *received_handler = path->get_received_packet_handler();
    bool duplicate = received_handler->is_duplicate(packet.get_packet_number());

    if (!received_handler->received_packet(packet.get_packet_number(), packet.is_retransmittable(), now_us))
    {
        return; // invalid packet number
    }

    // A duplicate still makes us answer with an ACK, but its frames were already handled
    if (!duplicate)
    {
        for (const auto &frame : packet.get_frames())
        {
            handle_frame(frame, packet, now_us);
            if (closed)
            {
                return;
            }
        }
    }

    // ACKs and WINDOW_UPDATEs may have opened a window, and we may have an ACK to send
    send_pending();
}

void Connection::handle_frame(const Frame &frame, const Packet &packet, uint64_t now_us)
{
    switch (frame.type)
    {
    case FRAME_STREAM:
        handle_stream_frame(frame.stream, now_us);
        break;
    case FRAME_ACK:
    {
        // The Path ID inside the ACK frame says which path (packet number space) is acknowledged.
        // It can be different from the path the ACK arrived on.
        Path *acked_path = get_path(frame.ack.path_id);
        if (acked_path == nullptr)
        {
            break;
        }

        // The number of the carrying packet can only order ACKs that came on the acked path itself
        uint64_t with_packet_number = 0;
        if (frame.ack.path_id == packet.get_path_id())
        {
            with_packet_number = packet.get_packet_number();
        }

        if (!acked_path->get_sent_packet_handler()->received_ack(frame.ack, with_packet_number, now_us))
        {
            close_with_error("the other side acknowledged a packet that we never sent (protocol error)");
        }
        break;
    }
    case FRAME_WINDOW_UPDATE:
        // The peer allows us to send more data
        flow_control_manager.update_window(frame.window_update.stream_id, frame.window_update.byte_offset);
        break;
    case FRAME_PING:
        break; // nothing to do, receiving the packet already triggers an ACK
    case FRAME_ADD_ADDRESS:
        // The peer owns these addresses: the path manager decides if new paths are opened
        if (add_address_callback != nullptr)
        {
            add_address_callback(frame.add_address.addresses);
        }
        break;
    case FRAME_CONNECTION_CLOSE:
        // The peer is done (or gave up): nothing more will come, so we stop too
        closed = true;
        closed_by_peer = true;
        close_reason = "the other side closed the connection";
        if (!frame.connection_close.reason.empty())
        {
            close_reason += " (its reason: " + frame.connection_close.reason + ")";
        }
        break;
    case FRAME_PATHS:
    {
        // The peer tells us which of its paths look broken: we stop using them as well.
        // The mark is removed as usual, when a packet arrives on the path again.
        paths_frames_received++;
        for (const auto &path_info : frame.paths.paths)
        {
            if (path_info.rtt_us != PATHS_RTT_POTENTIALLY_FAILED)
            {
                continue;
            }
            // This very packet proves that the path it arrived on works
            if (path_info.path_id == packet.get_path_id())
            {
                continue;
            }
            Path *failed_path = get_path(path_info.path_id);
            if (failed_path != nullptr)
            {
                failed_path->set_potentially_failed(true);
            }
        }
        break;
    }
    default:
        break;
    }
}

void Connection::handle_stream_frame(const StreamFrame &frame, uint64_t now_us)
{
    uint32_t stream_id = frame.stream_id;

    // Flow control: the peer must stay inside the windows we advertised
    uint64_t max_offset = frame.offset + frame.data.size();
    if (!flow_control_manager.update_highest_received(stream_id, max_offset))
    {
        close_with_error("the other side sent more data than we allowed it to send (flow control limit broken)");
        return;
    }

    auto it = receive_streams.find(stream_id);
    if (it == receive_streams.end())
    {
        it = receive_streams.emplace(stream_id, ReceiveStream(stream_id)).first;
    }
    ReceiveStream &stream = it->second;

    // The reorder buffer keeps the frame until everything before it has arrived
    stream.add_stream_frame(frame);

    // Deliver everything that is now in order
    uint64_t offset = 0;
    vector<uint8_t> data;
    bool fin = false;
    while (stream.pop(offset, data, fin))
    {
        // The data leaves our buffer, so the peer gets this space back (WINDOW_UPDATE)
        flow_control_manager.add_bytes_read(stream_id, data.size(), now_us);

        if (stream_callback != nullptr)
        {
            stream_callback(stream_id, offset, data, fin);
        }
    }
}

void Connection::on_tick()
{
    if (closed)
    {
        return;
    }

    uint64_t now_us = Packet::current_time_us();

    // Idle timeout: the peer disappeared without closing the connection
    if (idle_timeout_us != 0 && now_us > last_activity_us && now_us - last_activity_us > idle_timeout_us)
    {
        // Shown in seconds, or in milliseconds when it is not a whole number of seconds (tests)
        string limit = (idle_timeout_us % 1000000 == 0) ? to_string(idle_timeout_us / 1000000) + " s"
                                                        : to_string(idle_timeout_us / 1000) + " ms";
        close_with_error("nothing was received from the other side for " + limit + " (idle timeout)",
                         CLOSE_IDLE_TIMEOUT);
        return;
    }

    // Loss detection timers of the paths (loss timer, tail loss probe, RTO)
    bool path_newly_failed = false;
    for (auto &entry : paths)
    {
        bool was_failed = entry.second->is_potentially_failed();

        SentPacketHandler *sent_handler = entry.second->get_sent_packet_handler();
        uint64_t alarm = sent_handler->get_alarm_timeout();
        if (alarm != 0 && alarm <= now_us)
        {
            sent_handler->on_alarm(now_us);
        }

        if (!was_failed && entry.second->is_potentially_failed())
        {
            path_newly_failed = true;
        }
    }

    // An RTO just marked a path as potentially failed: tell the peer with a PATHS frame.
    // It leaves in the next packet, the one that carries the retransmitted data.
    if (path_newly_failed)
    {
        queue_paths_frame();
    }

    send_pending();
}
