#ifndef CONNECTION_HPP
#define CONNECTION_HPP

#include <bits/stdc++.h>
#include "packet/packet.hpp"
#include "path/path.hpp"
#include "reliability/send_stream.hpp"
#include "reliability/receive_stream.hpp"
#include "flow_control/flow_controller.hpp"
#include "scheduler/scheduler.hpp"

using namespace std;

// Called with the data of a stream, strictly in order and every byte exactly once.
// offset is the position of data in the stream, fin is true on the last call for the stream.
typedef std::function<void(uint32_t stream_id, uint64_t offset, const vector<uint8_t> &data, bool fin)> StreamDataCallback;

// Called when the peer advertises its addresses with an ADD_ADDRESS frame
typedef std::function<void(const vector<NetworkAddress> &addresses)> AddAddressCallback;

// A connection on which nothing is received for this long is closed (same value as the reference implementation)
const uint64_t DEFAULT_IDLE_TIMEOUT_US = 30ULL * 1000 * 1000;

// One MP-QUIC connection: owns the streams, uses the paths, builds packets out of
// frames and gives the received frames to the right handler.
// It is not thread safe, call everything from the SocketManager thread.
class Connection
{
public:
    Connection(uint64_t connection_id);

    // The connection points to its own default scheduler, so it must not be copied
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;

    uint64_t get_connection_id() const;

    // Paths are created outside and given to the connection (the path manager comes later).
    // Both hosts must use the same Path ID for the same path.
    // The congestion control of the path is linked with the one of the other paths (OLIA),
    // so a Path must not be used anymore once its Connection is destroyed.
    bool add_path(Path *path);
    Path *get_path(uint8_t path_id);
    const map<uint8_t, Path *> &get_paths() const;

    // Sends a PING on a path. A client does it on a path it just opened: the packet tells
    // the server that the path exists, and its ACK gives the first RTT of the path.
    bool send_ping(uint8_t path_id);

    // Tells the peer which addresses this host owns (ADD_ADDRESS frame)
    void advertise_addresses(const vector<NetworkAddress> &addresses);
    void on_add_address(AddAddressCallback cb);

    void on_stream_data(StreamDataCallback cb);

    // Application data to send on a stream. It is cut into STREAM frames when packets are built.
    void write(uint32_t stream_id, const uint8_t *data, size_t length);
    void close_stream(uint32_t stream_id);

    // Give every UDP datagram received for this connection to this function
    void handle_packet(const uint8_t *data, size_t length);

    // Call regularly (SocketManager tick): runs the loss detection timers of the paths,
    // then sends delayed ACKs, retransmissions and data that was waiting
    void on_tick();

    // Sends what can be sent now: retransmissions first, then new stream data (as far as
    // the congestion and flow control windows allow), then the ACKs and WINDOW_UPDATEs that are due
    void send_pending();

    // True if stream data still has to be put in a packet (new data, or lost data to retransmit)
    bool has_stream_data() const;

    // Total bytes sent and not acknowledged yet, over all paths
    uint64_t get_bytes_in_flight() const;

    // True when every byte written so far was sent and acknowledged by the peer.
    // (get_bytes_in_flight() can stay above 0 after that: a PING probing a broken path is also in flight.)
    bool all_data_acknowledged() const;

    // The packet scheduler chooses the path of every packet. By default it is the lowest-RTT
    // scheduler of the paper. The scheduler given here is not owned by the connection.
    void set_scheduler(Scheduler *new_scheduler);
    Scheduler *get_scheduler();

    // Statistics of the scheduler's work
    uint64_t get_duplicated_packets() const; // packets sent a second time on another path (unknown RTT)
    uint64_t get_ping_probes() const;        // PING frames queued to test a potentially failed path

    // PATHS frames: sent when one of our paths becomes potentially failed, so that the peer
    // stops using it at once (paper, Section 4.3)
    uint64_t get_paths_frames_sent() const;
    uint64_t get_paths_frames_received() const;

    FlowControlManager *get_flow_control_manager();

    // The application is done: sends what is still due (ACKs), tells the peer with a
    // CONNECTION_CLOSE frame on every path, and stops. Nothing is sent or handled afterwards.
    void close(const string &reason = "");

    // A connection is closed by close(), by a CONNECTION_CLOSE frame of the peer, when the
    // peer breaks the protocol, or when nothing was received for the idle timeout
    bool is_closed() const;
    bool is_closed_by_peer() const;
    string get_close_reason() const;

    void set_idle_timeout(uint64_t timeout_us);

private:
    bool has_new_stream_data() const;
    uint64_t get_max_smoothed_rtt();

    void queue_window_updates(uint64_t now_us);
    bool collect_retransmissions();

    void add_ack_frame(Path *path, Packet &packet, uint64_t now_us);
    void add_control_frames(Path *path, Packet &packet);
    void add_retransmission_frames(Packet &packet);
    void add_new_stream_frames(Packet &packet);

    void send_packet(Path *path, const Packet &packet, uint64_t now_us);
    void send_duplicate(Path *sent_on, const Packet &packet, uint64_t now_us);
    void queue_ping_probe(Path *path);
    void queue_paths_frame();
    void send_acks_and_control_frames(uint64_t now_us);

    void handle_frame(const Frame &frame, const Packet &packet, uint64_t now_us);
    void handle_stream_frame(const StreamFrame &frame, uint64_t now_us);

    void close_with_error(const string &reason, uint32_t error_code = CLOSE_PROTOCOL_ERROR);
    void send_connection_close(uint32_t error_code, const string &reason);

    uint64_t connection_id;
    map<uint8_t, Path *> paths;

    // The OLIA senders of all the paths: each one needs the window and the RTT of the others
    map<uint8_t, OliaSender *> olia_senders;

    map<uint32_t, SendStream> send_streams;
    map<uint32_t, ReceiveStream> receive_streams;
    FlowControlManager flow_control_manager;

    // STREAM frames of lost packets. They are sent before new data and can leave on any path.
    deque<Frame> retransmission_queue;

    // Control frames (WINDOW_UPDATE, PING) waiting to be sent on a given path: Path ID -> frames
    map<uint8_t, deque<Frame>> control_frames;

    StreamDataCallback stream_callback = nullptr;
    AddAddressCallback add_address_callback = nullptr;

    uint64_t idle_timeout_us = DEFAULT_IDLE_TIMEOUT_US;
    uint64_t last_activity_us = 0; // time of the last packet received
    bool closed_by_peer = false;

    LowestRttScheduler default_scheduler;
    Scheduler *scheduler = nullptr;
    uint64_t duplicated_packets = 0;
    uint64_t ping_probes = 0;
    uint64_t paths_frames_sent = 0;
    uint64_t paths_frames_received = 0;

    bool closed = false;
    string close_reason;
};

#endif
