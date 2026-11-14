#include <bits/stdc++.h>

#include "connection/connection.hpp"

using namespace std;

Connection::Connection(uint64_t connection_id)
{
    this->connection_id = connection_id;
    this->scheduler = &default_scheduler;
    this->last_received_us = Packet::current_time_us();
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
    pending_control[path_id].push_back(Frame::make_ping());
    send_pending();
    return true;
}

void Connection::advertise_addresses(const vector<NetworkAddress> &addresses)
{
    if (closed || paths.empty() || addresses.empty())
    {
        return;
    }

    // It rides along with the next packet of a usable path
    Path *target = scheduler->select_path(paths, true);
    if (target == nullptr)
    {
        target = paths.begin()->second;
    }
    pending_control[target->get_id()].push_back(Frame::make_add_address(addresses));
}

void Connection::on_add_address(AddAddressCallback cb)
{
    address_handler = cb;
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

    // Flush the ACKs that are due, then let the peer know we are done
    send_pending();
    broadcast_close(CLOSE_NO_ERROR, reason);

    closed = true;
    close_reason = reason;
}

// Sends a CONNECTION_CLOSE frame on every path, so it gets through even if one path is
// broken. It is never resent: should every copy be lost, the peer's idle timeout ends it.
void Connection::broadcast_close(uint32_t error_code, const string &reason)
{
    uint64_t now_us = Packet::current_time_us();
    for (const auto &[path_id, path] : paths)
    {
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

    // Coupled congestion control: this path's sender joins the shared table
    coupled_senders[path->get_id()] = path->get_congestion();
    path->get_congestion()->set_olia_senders(&coupled_senders);
    return true;
}

Path *Connection::get_path(uint8_t path_id)
{
    auto it = paths.find(path_id);
    return (it == paths.end()) ? nullptr : it->second;
}

void Connection::on_stream_data(StreamDataCallback cb)
{
    data_handler = cb;
}

void Connection::write(uint32_t stream_id, const uint8_t *data, size_t length)
{
    // try_emplace creates the stream on first use and leaves an existing one alone
    send_streams.try_emplace(stream_id, stream_id).first->second.write(data, length);
}

void Connection::close_stream(uint32_t stream_id)
{
    send_streams.try_emplace(stream_id, stream_id).first->second.close();
}

size_t Connection::unsent_bytes(uint32_t stream_id) const
{
    auto it = send_streams.find(stream_id);
    return (it == send_streams.end()) ? 0 : it->second.get_unsent_bytes();
}

bool Connection::has_unsent_stream_data() const
{
    return any_of(send_streams.begin(), send_streams.end(),
                  [](const auto &entry) { return entry.second.has_data(); });
}

bool Connection::has_stream_data() const
{
    if (has_unsent_stream_data() || !lost_stream_frames.empty())
    {
        return true;
    }

    // Lost packets whose frames have not been collected yet
    return any_of(paths.begin(), paths.end(), [](const auto &entry) {
        return entry.second->get_sent_packet_handler()->has_retransmissions();
    });
}

uint64_t Connection::get_bytes_in_flight() const
{
    uint64_t total = 0;
    for (const auto &[path_id, path] : paths)
    {
        total += path->get_sent_packet_handler()->get_bytes_in_flight();
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
    if (closed)
    {
        return;
    }

    // Tell the peer why, then stop
    broadcast_close(error_code, reason);

    closed = true;
    close_reason = reason;
    cout << "Connection " << connection_id << " closed: " << reason << "\n";
}

// Flow-control auto-tuning works with the slowest path's smoothed RTT
uint64_t Connection::largest_path_rtt()
{
    uint64_t largest = 0;
    for (const auto &[path_id, path] : paths)
    {
        largest = max(largest, path->get_rtt_stats()->get_smoothed_rtt());
    }
    return largest;
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

// Builds a PATHS frame listing our paths with their smoothed RTT, where a potentially failed
// path carries a reserved RTT value, and queues it on a path that works. Otherwise the peer
// would go on answering over the broken path until its own retransmission timer noticed.
void Connection::announce_path_states()
{
    if (paths.size() < 2)
    {
        return; // with one path there is no alternative to point the peer to
    }

    PathsFrame info;
    for (const auto &[path_id, path] : paths)
    {
        PathInfo path_info;
        path_info.path_id = path_id;
        if (path->is_potentially_failed())
        {
            path_info.rtt_us = PATHS_RTT_POTENTIALLY_FAILED;
        }
        else
        {
            uint64_t rtt = path->get_rtt_stats()->get_smoothed_rtt();
            path_info.rtt_us = static_cast<uint32_t>(min<uint64_t>(rtt, PATHS_RTT_POTENTIALLY_FAILED - 1));
        }
        info.paths.push_back(path_info);
    }

    // Any PATHS frame still waiting in a queue is now stale
    for (auto &[path_id, queue] : pending_control)
    {
        queue.erase(remove_if(queue.begin(), queue.end(),
                              [](const Frame &frame) { return frame.type == FRAME_PATHS; }),
                    queue.end());
    }

    // Ask the scheduler for a usable path; a control frame is not held back by the congestion window
    Path *target = scheduler->select_path(paths, true);
    if (target == nullptr)
    {
        return;
    }
    pending_control[target->get_id()].push_back(Frame::make_paths(info));
    path_reports_sent++;
}

bool Connection::all_data_acknowledged() const
{
    if (has_stream_data())
    {
        return false;
    }
    return none_of(paths.begin(), paths.end(), [](const auto &entry) {
        return entry.second->get_sent_packet_handler()->has_outstanding_stream_frames();
    });
}

// Queues one PING on a potentially failed path. If the path has come back, the peer's ACK
// arrives on it and receiving that packet clears the mark. If the PING is lost, the path's
// RTO hands it back and it is sent once more, each time after a longer wait since the RTO
// doubles, so the path keeps being tested.
void Connection::probe_with_ping(Path *path)
{
    deque<Frame> &queue = pending_control[path->get_id()];

    // A single probe in the queue is enough
    bool already_queued = any_of(queue.begin(), queue.end(),
                                 [](const Frame &frame) { return frame.type == FRAME_PING; });
    if (already_queued)
    {
        return;
    }

    queue.push_back(Frame::make_ping());
    probes_queued++;
}

// Asks flow control whether a receive window should move forward. Each resulting
// WINDOW_UPDATE is queued on EVERY path: one copy stuck on a slow or broken path
// would otherwise leave the sender blocked.
void Connection::schedule_window_updates(uint64_t now_us)
{
    vector<WindowUpdateFrame> updates = flow_control_manager.get_window_updates(now_us, largest_path_rtt());

    for (const auto &update : updates)
    {
        for (const auto &[path_id, path] : paths)
        {
            pending_control[path_id].push_back(Frame::make_window_update(update.stream_id, update.byte_offset));
        }
    }
}

// Collects the packets the paths have declared lost and requeues their frames. A frame is
// not tied to the packet or the path that carried it, so lost STREAM data joins one shared
// queue and may leave on any path. Returns true if any lost packet was found.
bool Connection::gather_lost_frames()
{
    bool has_retransmission = false;
    bool paths_frame_lost = false;

    for (const auto &[path_id, path] : paths)
    {
        bool lost_on_this_path = false;

        SentPacket lost_packet;
        while (path->get_sent_packet_handler()->dequeue_packet_for_retransmission(lost_packet))
        {
            has_retransmission = true;
            lost_on_this_path = true;

            for (const auto &frame : lost_packet.frames)
            {
                switch (frame.type)
                {
                case FRAME_STREAM:
                    lost_stream_frames.push_back(frame);
                    break;
                case FRAME_WINDOW_UPDATE:
                    // Resend it only if no later update has already raised the limit past it
                    if (frame.window_update.byte_offset >= flow_control_manager.get_receive_window(frame.window_update.stream_id))
                    {
                        pending_control[path_id].push_back(frame);
                    }
                    break;
                case FRAME_PATHS:
                    // Path states may have changed in the meantime: a fresh frame is built below
                    paths_frame_lost = true;
                    break;
                default:
                    pending_control[path_id].push_back(frame);
                    break;
                }
            }
        }

        // A broken path that just lost packets gets a PING to find out when it is back
        if (lost_on_this_path && path->is_potentially_failed())
        {
            probe_with_ping(path);
        }
    }

    if (paths_frame_lost)
    {
        announce_path_states();
    }

    return has_retransmission;
}

// Adds this path's ACK frame to the packet, if one is due
void Connection::attach_ack(Path *path, Packet &packet, uint64_t now_us)
{
    // Not even room for an ACK with a single range: leave it for the next packet.
    // This check comes first because fetching the ACK frame resets the path's ACK state.
    if (packet.size() + ACK_FRAME_HEADER_SIZE + MAX_VARINT_SIZE > MAX_PACKET_SIZE)
    {
        return;
    }

    AckFrame ack;
    if (!path->get_ack_frame(now_us, ack))
    {
        return;
    }

    // 256 ranges normally fit. If the packet is already partly used, or the numbers are
    // unusually large, the lowest ranges are dropped: the highest ones are the most recent.
    Frame frame = Frame::make_ack(ack);
    while (frame.ack.ranges.size() > 1 && packet.size() + frame.size() > MAX_PACKET_SIZE)
    {
        frame.ack.ranges.pop_back();
    }
    packet.add_frame(frame);
}

void Connection::attach_control_frames(Path *path, Packet &packet)
{
    deque<Frame> &queue = pending_control[path->get_id()];
    while (!queue.empty() && packet.size() + queue.front().size() <= MAX_PACKET_SIZE)
    {
        packet.add_frame(queue.front());
        queue.pop_front();
    }
}

void Connection::attach_lost_data(Packet &packet)
{
    while (!lost_stream_frames.empty())
    {
        Frame &frame = lost_stream_frames.front();

        size_t space = MAX_PACKET_SIZE - packet.size();
        if (space <= STREAM_FRAME_HEADER_SIZE)
        {
            break;
        }
        size_t max_data = space - STREAM_FRAME_HEADER_SIZE;

        if (frame.stream.data.size() > max_data)
        {
            // Too big for the space left: send the front part now and keep the rest queued
            vector<uint8_t> first_part(frame.stream.data.begin(), frame.stream.data.begin() + max_data);
            packet.add_frame(Frame::make_stream(frame.stream.stream_id, frame.stream.offset, false, first_part));

            frame.stream.data.erase(frame.stream.data.begin(), frame.stream.data.begin() + max_data);
            frame.stream.offset += max_data;
            break;
        }

        packet.add_frame(frame);
        lost_stream_frames.pop_front();
    }
}

void Connection::attach_new_data(Packet &packet)
{
    for (auto &[stream_id, stream] : send_streams)
    {
        Frame frame;

        while (packet.size() < MAX_PACKET_SIZE &&
               stream.pop_stream_frame(MAX_PACKET_SIZE - packet.size(), frame,
                                             flow_control_manager.send_window_size(stream_id)))
        {
            // Fresh data consumes flow-control credit; resent data was already counted the first time
            flow_control_manager.add_bytes_sent(stream_id, frame.stream.data.size());
            packet.add_frame(frame);
        }
    }
}

void Connection::send_packet(Path *path, const Packet &packet, uint64_t now_us)
{
    vector<uint8_t> raw = packet.serialize();

    // Even if the socket refuses the datagram, the packet is recorded as sent. To the protocol
    // that is no different from a loss in the network, and it is recovered the same way.
    path->send(raw.data(), raw.size());

    path->pop_packet_number();
    path->get_sent_packet_handler()->sent_packet(packet.get_packet_number(), packet.get_frames(), raw.size(), now_us);
}

// Start-up duplication. Stream data just sent on a path with no RTT yet may also be copied
// onto another path, if the scheduler asks for it. The copy is an ordinary packet of that
// other path, with its own packet number, tracked and acknowledged on its own. The receiver
// keeps whichever copy arrives first; its reorder buffer discards the other.
void Connection::send_startup_copy(Path *sent_on, const Packet &packet, uint64_t now_us)
{
    Path *other_path = scheduler->select_duplicate_path(paths, sent_on);
    if (other_path == nullptr)
    {
        return;
    }

    Packet duplicate(FLAG_NONE, connection_id, other_path->get_id(), other_path->peek_packet_number());

    // Identical STREAM frames at identical offsets; they consume no extra flow-control credit
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

    // Anything that path owes anyway rides along if there is room
    attach_ack(other_path, duplicate, now_us);
    attach_control_frames(other_path, duplicate);

    send_packet(other_path, duplicate, now_us);
    scheduler->on_packet_sent(other_path);
    copies_sent++;
}

void Connection::send_pending()
{
    if (closed)
    {
        return;
    }

    uint64_t now_us = Packet::current_time_us();

    schedule_window_updates(now_us);

    // Keep building packets until the data runs out or no path can take more
    while (true)
    {
        // Lost data always goes first
        bool has_retransmission = gather_lost_frames();

        if (lost_stream_frames.empty() && !has_unsent_stream_data())
        {
            break;
        }

        // The scheduler picks the usable path with the lowest smoothed RTT
        Path *path = scheduler->select_path(paths, has_retransmission);
        if (path == nullptr)
        {
            break; // no path can be used now
        }

        // Every packet takes the next number of its path, so resent frames never reuse an old number
        Packet packet(FLAG_NONE, connection_id, path->get_id(), path->peek_packet_number());

        attach_ack(path, packet, now_us);
        attach_control_frames(path, packet);
        attach_lost_data(packet);
        attach_new_data(packet);

        if (packet.get_frames().empty())
        {
            break;
        }

        const vector<Frame> &frames = packet.get_frames();
        bool has_stream_frame = any_of(frames.begin(), frames.end(),
                                       [](const Frame &frame) { return frame.type == FRAME_STREAM; });

        send_packet(path, packet, now_us);
        scheduler->on_packet_sent(path);

        // No stream data fitted: flow control is blocking us until a WINDOW_UPDATE comes
        if (!has_stream_frame)
        {
            break;
        }

        // Copy the data onto a second path while this one has no RTT yet
        send_startup_copy(path, packet, now_us);
    }

    flush_acks_and_control(now_us);
}

// Sends whatever could not ride along with data: each path's ACK (on the path whose packets
// it covers) and each path's queued control frames. These small packets are not held back
// by the congestion window.
void Connection::flush_acks_and_control(uint64_t now_us)
{
    for (const auto &[path_id, path] : paths)
    {
        while (true)
        {
            Packet packet(FLAG_NONE, connection_id, path->get_id(), path->peek_packet_number());

            attach_ack(path, packet, now_us);
            attach_control_frames(path, packet);

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

    // The header's Path ID says which path the packet belongs to
    Path *path = get_path(packet.get_path_id());
    if (path == nullptr)
    {
        return;
    }

    uint64_t now_us = Packet::current_time_us();
    path->set_last_network_activity_time(now_us);
    last_received_us = now_us;

    // A packet just arrived on this path, so the path works
    path->set_potentially_failed(false);

    ReceivedPacketHandler *received_handler = path->get_received_packet_handler();
    bool duplicate = received_handler->is_duplicate(packet.get_packet_number());

    if (!received_handler->received_packet(packet.get_packet_number(), packet.is_retransmittable(), now_us))
    {
        return; // invalid packet number
    }

    // A duplicate is still acknowledged, but its frames were handled the first time
    if (!duplicate)
    {
        for (const auto &frame : packet.get_frames())
        {
            dispatch_frame(frame, packet, now_us);
            if (closed)
            {
                return;
            }
        }
    }

    // ACKs and WINDOW_UPDATEs may have opened a window, and an ACK of our own may be due
    send_pending();
}

void Connection::dispatch_frame(const Frame &frame, const Packet &packet, uint64_t now_us)
{
    switch (frame.type)
    {
    case FRAME_STREAM:
        deliver_stream_frame(frame.stream, now_us);
        break;
    case FRAME_ACK:
    {
        // The ACK frame's own Path ID names the packet number space it covers, which may be
        // a different path from the one it arrived on
        Path *acked_path = get_path(frame.ack.path_id);
        if (acked_path == nullptr)
        {
            break;
        }

        // The carrying packet's number only orders ACKs that arrive on the path they cover
        uint64_t with_packet_number = 0;
        if (frame.ack.path_id == packet.get_path_id())
        {
            with_packet_number = packet.get_packet_number();
        }

        if (!acked_path->get_sent_packet_handler()->received_ack(frame.ack, with_packet_number, now_us))
        {
            abort_with("the other side acknowledged a packet that we never sent (protocol error)");
        }
        break;
    }
    case FRAME_WINDOW_UPDATE:
        // The peer raised one of our send limits
        flow_control_manager.update_window(frame.window_update.stream_id, frame.window_update.byte_offset);
        break;
    case FRAME_PING:
        break; // nothing else to do: receiving the packet already schedules an ACK
    case FRAME_ADD_ADDRESS:
        // Addresses owned by the peer; the path manager decides whether to open paths to them
        if (address_handler != nullptr)
        {
            address_handler(frame.add_address.addresses);
        }
        break;
    case FRAME_CONNECTION_CLOSE:
        // The peer has finished or given up: nothing more will come, so we stop as well
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
        // The peer reports which paths look broken to it; we stop using them too.
        // The mark clears the usual way, when a packet arrives on that path again.
        path_reports_received++;
        for (const auto &path_info : frame.paths.paths)
        {
            if (path_info.rtt_us != PATHS_RTT_POTENTIALLY_FAILED)
            {
                continue;
            }
            // The path this packet came in on obviously works
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

void Connection::deliver_stream_frame(const StreamFrame &frame, uint64_t now_us)
{
    uint32_t stream_id = frame.stream_id;

    // Flow control: the peer must stay within the limits we advertised
    uint64_t max_offset = frame.offset + frame.data.size();
    if (!flow_control_manager.update_highest_received(stream_id, max_offset))
    {
        abort_with("the other side sent more data than we allowed it to send (flow control limit broken)");
        return;
    }

    ReceiveStream &stream = receive_streams.try_emplace(stream_id, stream_id).first->second;

    // The reorder buffer holds the frame until everything before it has arrived
    stream.add_stream_frame(frame);

    // Hand over everything that is now contiguous
    uint64_t offset = 0;
    vector<uint8_t> data;
    bool fin = false;
    while (stream.pop(offset, data, fin))
    {
        // Delivered data frees buffer space, which the peer gets back through a WINDOW_UPDATE
        flow_control_manager.add_bytes_read(stream_id, data.size(), now_us);

        if (data_handler != nullptr)
        {
            data_handler(stream_id, offset, data, fin);
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

    // Idle timeout: the peer vanished without closing the connection
    if (idle_timeout_us != 0 && now_us > last_received_us && now_us - last_received_us > idle_timeout_us)
    {
        // Shown in seconds, or in milliseconds when it is not a whole number of seconds (tests)
        string limit = (idle_timeout_us % 1000000 == 0) ? to_string(idle_timeout_us / 1000000) + " s"
                                                        : to_string(idle_timeout_us / 1000) + " ms";
        abort_with("nothing was received from the other side for " + limit + " (idle timeout)",
                         CLOSE_IDLE_TIMEOUT);
        return;
    }

    // Loss-recovery timers of every path (early loss, tail loss probe, RTO)
    bool path_newly_failed = false;
    for (const auto &[path_id, path] : paths)
    {
        bool was_failed = path->is_potentially_failed();

        SentPacketHandler *sent_handler = path->get_sent_packet_handler();
        uint64_t alarm = sent_handler->get_alarm_timeout();
        if (alarm != 0 && alarm <= now_us)
        {
            sent_handler->on_alarm(now_us);
        }

        if (!was_failed && path->is_potentially_failed())
        {
            path_newly_failed = true;
        }
    }

    // An RTO has just marked a path as potentially failed: report it with a PATHS frame,
    // which leaves with the next packet, the one carrying the resent data
    if (path_newly_failed)
    {
        announce_path_states();
    }

    send_pending();
}
