#ifndef CONNECTION_HPP
#define CONNECTION_HPP

#include<bits/stdc++.h>
#include "packet/packet.hpp"
#include "path/path.hpp"
#include "reliability/send_stream.hpp"
#include "reliability/receive_stream.hpp"
#include "flow_control/flow_controller.hpp"
#include "scheduler/scheduler.hpp"

using namespace std;

// Stream data callback (in-order bytes, fin=true on last chunk)
typedef function<void(uint32_t stream_id, uint64_t offset, const vector<uint8_t> &data, bool fin)> StreamDataCallback;

// Called when peer sends ADD_ADDRESS frame
typedef function<void(const vector<NetworkAddress> &addresses)> AddAddressCallback;

/*
 One MP-QUIC connection: manages streams, paths, and packet build/parse.
 Not thread-safe; call only from the SocketManager thread.
*/
class Connection
{
public:
    // Close connection after 30s of no packets
    static constexpr uint64_t DEFAULT_IDLE_TIMEOUT_US=30ULL*1000*1000;

    Connection(uint64_t connection_id);

    // Do not copy (holds pointer to default_scheduler)
    Connection(const Connection &)=delete;
    Connection &operator=(const Connection &)=delete;

    uint64_t get_connection_id() const;

    // Register a path created by PathManager
    bool add_path(Path *path);
    Path *get_path(uint8_t path_id);
    const map<uint8_t, Path *> &get_paths() const;

    // Send PING on a path
    bool send_ping(uint8_t path_id);

    // Send local addresses via ADD_ADDRESS frame
    void advertise_addresses(const vector<NetworkAddress> &addresses);
    void on_add_address(AddAddressCallback cb);

    void on_stream_data(StreamDataCallback cb);

    // Queue data to send on stream (split into STREAM frames when sending)
    void write(uint32_t stream_id, const uint8_t *data, size_t length);
    void close_stream(uint32_t stream_id);

    // Unsent bytes buffered on stream (0 if unknown); lets apps write large files in chunks
    size_t unsent_bytes(uint32_t stream_id) const;

    // Pass each received UDP packet here
    void handle_packet(const uint8_t *data, size_t length);

    // Called regularly by SocketManager: runs timers, sends ACKs, resends, and queued data
    void on_tick();

    // Send retransmissions, new stream data (within windows), ACKs, and WINDOW_UPDATEs
    void send_pending();

    // True if new or lost stream data still needs to be sent
    bool has_stream_data() const;

    // Total unacked bytes across all paths
    uint64_t get_bytes_in_flight() const;

    // True when all written stream bytes have been acked (in-flight PINGs do not block this)
    bool all_data_acknowledged() const;

    // Set packet scheduler (not owned by Connection; default is lowest-RTT)
    void set_scheduler(Scheduler *new_scheduler);
    Scheduler *get_scheduler();

    // Scheduler stats
    uint64_t get_duplicated_packets() const; // packets duplicated on another path (unknown RTT)
    uint64_t get_ping_probes() const;        // PING probes queued for broken paths

    // PATHS frames let peer stop using a broken path right away instead of waiting for timeout
    uint64_t get_paths_frames_sent() const;
    uint64_t get_paths_frames_received() const;

    FlowControlManager *get_flow_control_manager();

    // Flush ACKs, send CONNECTION_CLOSE on all paths, and stop
    void close(const string &reason="");

    // True if closed by close(), peer CONNECTION_CLOSE, protocol error, or idle timeout
    bool is_closed() const;
    bool is_closed_by_peer() const;
    string get_close_reason() const;

    void set_idle_timeout(uint64_t timeout_us);

private:
    bool has_unsent_stream_data() const;
    uint64_t largest_path_rtt();

    void schedule_window_updates(uint64_t now_us);
    bool gather_lost_frames();

    void attach_ack(Path *path, Packet &packet, uint64_t now_us);
    void attach_control_frames(Path *path, Packet &packet);
    void attach_lost_data(Packet &packet);
    void attach_new_data(Packet &packet);

    void send_packet(Path *path, const Packet &packet, uint64_t now_us);
    void send_startup_copy(Path *sent_on, const Packet &packet, uint64_t now_us);
    void probe_with_ping(Path *path);
    void announce_path_states();
    void flush_acks_and_control(uint64_t now_us);

    void dispatch_frame(const Frame &frame, const Packet &packet, uint64_t now_us);
    void deliver_stream_frame(const StreamFrame &frame, uint64_t now_us);

    void abort_with(const string &reason, uint32_t error_code=CLOSE_PROTOCOL_ERROR);
    void broadcast_close(uint32_t error_code, const string &reason);

    uint64_t connection_id;
    map<uint8_t, Path *> paths;

    // Coupled OLIA senders across all paths of this connection
    map<uint8_t, OliaSender *> coupled_senders;

    map<uint32_t, SendStream> send_streams;
    map<uint32_t, ReceiveStream> receive_streams;
    FlowControlManager flow_control_manager;

    // Lost STREAM frames waiting to be resent on any usable path
    deque<Frame> lost_stream_frames;

    // Queued control frames per path (Path ID -> frames)
    map<uint8_t, deque<Frame>> pending_control;

    StreamDataCallback data_handler=nullptr;
    AddAddressCallback address_handler=nullptr;

    uint64_t idle_timeout_us=DEFAULT_IDLE_TIMEOUT_US;
    uint64_t last_received_us=0; // time of the last packet received
    bool closed_by_peer=false;

    LowestRttScheduler default_scheduler;
    Scheduler *scheduler=nullptr;
    uint64_t copies_sent=0;
    uint64_t probes_queued=0;
    uint64_t path_reports_sent=0;
    uint64_t path_reports_received=0;

    bool closed=false;
    string close_reason;
};

#endif
