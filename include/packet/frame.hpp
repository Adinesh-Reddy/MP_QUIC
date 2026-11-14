#ifndef FRAME_HPP
#define FRAME_HPP

#include <bits/stdc++.h>

using namespace std;

// A packet is only a container: everything inside it, data and control alike, is a frame.
// Frames are therefore not bound to the packet or the path that first carried them.
const uint8_t FRAME_STREAM        = 0x01;
const uint8_t FRAME_ACK              = 0x02;
const uint8_t FRAME_CONNECTION_CLOSE = 0x03;
const uint8_t FRAME_WINDOW_UPDATE    = 0x04;
const uint8_t FRAME_PING             = 0x07;
const uint8_t FRAME_ADD_ADDRESS      = 0x10;
const uint8_t FRAME_PATHS            = 0x12;
// Reserved: 0x11 CLOSE_PATH

// Serialized sizes in bytes (every frame starts with a 1-byte type)
const size_t STREAM_FRAME_HEADER_SIZE = 16; // type(1) stream_id(4) offset(8) fin(1) length(2)
const size_t ACK_FRAME_HEADER_SIZE    = 16; // type(1) path_id(1) largest_acked(8) ack_delay_us(4) num_ranges(2)
const size_t WINDOW_UPDATE_FRAME_SIZE = 13; // type(1) stream_id(4) byte_offset(8)
const size_t PING_FRAME_SIZE          = 1;  // type(1)
const size_t PATHS_FRAME_HEADER_SIZE  = 2;  // type(1) num_paths(1)
const size_t PATHS_ENTRY_SIZE         = 5;  // path_id(1) rtt_us(4)
const size_t ADD_ADDRESS_FRAME_HEADER_SIZE      = 2; // type(1) num_addresses(1)
const size_t ADD_ADDRESS_ENTRY_SIZE             = 6; // ipv4(4) port(2)
const size_t CONNECTION_CLOSE_FRAME_HEADER_SIZE = 7; // type(1) error_code(4) reason_length(2)
const size_t MAX_CLOSE_REASON_LENGTH            = 200;

// After its 16-byte header an ACK frame lists the ranges in a compact form:
//   first_range_length              (packets in the highest range, minus 1)
//   then for every further range:   gap     (number of missing packets above this range)
//                                   length  (packets in this range, minus 1)
// Each of these numbers is a variable-length integer: it takes 1, 2, 4 or 8 bytes, the two
// highest bits of its first byte say which (00 = 1 byte, 01 = 2, 10 = 4, 11 = 8 bytes).
// Small numbers are the usual case, so a range normally costs 2 bytes and 256 ranges
// fit in one packet.
const size_t MAX_VARINT_SIZE = 8;
const uint64_t MAX_VARINT_VALUE = (1ULL << 62) - 1;

// An ACK frame lists at most 256 ranges of received packet numbers
const size_t MAX_ACK_RANGES = 256;

// Carries application data. The offset is the position of the data in the stream,
// it stays the same when the data is retransmitted in another packet or on another path.
struct StreamFrame
{
    uint32_t stream_id = 0;
    uint64_t offset = 0;
    bool fin = false;
    vector<uint8_t> data;
};

// A block of consecutive packet numbers [first, last] that were received
struct AckRange
{
    uint64_t first;
    uint64_t last;
};

// Acknowledges packets (not bytes) received on one path.
// path_id tells which packet number space the ranges belong to.
struct AckFrame
{
    uint8_t path_id = 0;
    uint64_t largest_acked = 0;
    uint32_t ack_delay_us = 0;  // time the receiver held the largest packet before sending this ACK
    vector<AckRange> ranges;    // highest range first, ranges[0].last == largest_acked

    uint64_t lowest_acked() const;
    bool acks_packet(uint64_t packet_number) const;
};

// Advertises the receive window as an absolute byte offset (stream_id 0 = whole connection)
struct WindowUpdateFrame
{
    uint32_t stream_id = 0;
    uint64_t byte_offset = 0;
};

// RTT value that means "this path is potentially failed" in a PATHS frame
const uint32_t PATHS_RTT_POTENTIALLY_FAILED = UINT32_MAX;

// One path as seen by the host that sends the PATHS frame
struct PathInfo
{
    uint8_t path_id = 0;
    uint32_t rtt_us = 0; // its smoothed RTT (0 = not measured yet), or PATHS_RTT_POTENTIALLY_FAILED
};

// Gives the peer an overview of the sender's paths: which ones exist, their RTT, and which
// ones look broken. The peer can then stop using a broken path right away instead of
// discovering the failure through a timeout of its own.
struct PathsFrame
{
    vector<PathInfo> paths;
};

// An IPv4 address and a UDP port
struct NetworkAddress
{
    string ip;
    uint16_t port = 0;
};

// Advertises every address a host owns, so the peer can open paths towards them.
// A server with two interfaces tells the client its second address this way,
// and the client can then open a path towards it.
struct AddAddressFrame
{
    vector<NetworkAddress> addresses;
};

// Why a connection was closed
const uint32_t CLOSE_NO_ERROR       = 0; // the application is done
const uint32_t CLOSE_PROTOCOL_ERROR = 1; // the peer broke the protocol
const uint32_t CLOSE_IDLE_TIMEOUT   = 2; // nothing was received for too long

// Tells the peer that the connection is over, so that it does not have to wait for a timeout
struct ConnectionCloseFrame
{
    uint32_t error_code = CLOSE_NO_ERROR;
    string reason;
};

class Frame
{
public:
    uint8_t type = FRAME_PING;

    // Only the member matching 'type' is meaningful
    StreamFrame stream;
    AckFrame ack;
    WindowUpdateFrame window_update;
    PathsFrame paths;
    AddAddressFrame add_address;
    ConnectionCloseFrame connection_close;

    static Frame make_stream(uint32_t stream_id, uint64_t offset, bool fin, const vector<uint8_t> &data);
    static Frame make_ack(const AckFrame &ack);
    static Frame make_window_update(uint32_t stream_id, uint64_t byte_offset);
    static Frame make_ping();
    static Frame make_paths(const PathsFrame &paths);
    static Frame make_add_address(const vector<NetworkAddress> &addresses);
    static Frame make_connection_close(uint32_t error_code, const string &reason);

    // ACK frames are never retransmitted, every other frame is
    bool is_retransmittable() const;

    size_t size() const;
    void serialize(vector<uint8_t> &out) const;

    // Parses one frame from the start of buffer, consumed = number of bytes it used
    static bool parse(const uint8_t *buffer, size_t length, size_t &consumed, Frame &out_frame);
};

#endif
