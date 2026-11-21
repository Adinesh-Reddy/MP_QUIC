#ifndef FRAME_HPP
#define FRAME_HPP

#include<bits/stdc++.h>

using namespace std;

// Frame type codes
const uint8_t FRAME_STREAM=0x01;
const uint8_t FRAME_ACK=0x02;
const uint8_t FRAME_CONNECTION_CLOSE=0x03;
const uint8_t FRAME_WINDOW_UPDATE=0x04;
const uint8_t FRAME_PING=0x07;
const uint8_t FRAME_ADD_ADDRESS=0x10;
const uint8_t FRAME_PATHS=0x12;

// Frame header sizes in bytes
const size_t STREAM_FRAME_HEADER_SIZE=16;
const size_t ACK_FRAME_HEADER_SIZE=16;
const size_t WINDOW_UPDATE_FRAME_SIZE=13;
const size_t PING_FRAME_SIZE=1;
const size_t PATHS_FRAME_HEADER_SIZE=2;
const size_t PATHS_ENTRY_SIZE=5;
const size_t ADD_ADDRESS_FRAME_HEADER_SIZE=2;
const size_t ADD_ADDRESS_ENTRY_SIZE=6;
const size_t CONNECTION_CLOSE_FRAME_HEADER_SIZE=7;
const size_t MAX_CLOSE_REASON_LENGTH=200;

// Variable-length integer limits for ACK ranges
const size_t MAX_VARINT_SIZE=8;
const uint64_t MAX_VARINT_VALUE=(1ULL<<62)-1;

// Max packet ranges in one ACK frame
const size_t MAX_ACK_RANGES=256;

// Stream data chunk with byte offset
struct StreamFrame
{
    uint32_t stream_id=0;
    uint64_t offset=0;
    bool fin=false;
    vector<uint8_t> data;
};

// Continuous range of received packet numbers [first, last]
struct AckRange
{
    uint64_t first;
    uint64_t last;
};

// Selective ACK for one path
struct AckFrame
{
    uint8_t path_id=0;
    uint64_t largest_acked=0;
    uint32_t ack_delay_us=0;
    vector<AckRange> ranges;

    uint64_t lowest_acked() const;
    bool acks_packet(uint64_t packet_number) const;
};

// Receive window update (stream_id 0 = connection level)
struct WindowUpdateFrame
{
    uint32_t stream_id=0;
    uint64_t byte_offset=0;
};

// Sentinel value marking a broken path in a PATHS frame
const uint32_t PATHS_RTT_POTENTIALLY_FAILED=UINT32_MAX;

// Path status entry for PATHS frame
struct PathInfo
{
    uint8_t path_id=0;
    uint32_t rtt_us=0;
};

// Lists active paths and their RTT or broken status
struct PathsFrame
{
    vector<PathInfo> paths;
};

// IPv4 address and UDP port
struct NetworkAddress
{
    string ip;
    uint16_t port=0;
};

// Advertises local addresses so peer can open additional paths
struct AddAddressFrame
{
    vector<NetworkAddress> addresses;
};

// Connection close error codes
const uint32_t CLOSE_NO_ERROR=0;
const uint32_t CLOSE_PROTOCOL_ERROR=1;
const uint32_t CLOSE_IDLE_TIMEOUT=2;

// Signals connection termination
struct ConnectionCloseFrame
{
    uint32_t error_code=CLOSE_NO_ERROR;
    string reason;
};

class Frame
{
public:
    uint8_t type=FRAME_PING;

    // Only the field matching 'type' is used
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

    // Every frame except ACK must be delivered reliably
    bool is_retransmittable() const;

    size_t size() const;
    void serialize(vector<uint8_t> &out) const;

    // Read one frame from buffer; consumed = bytes read
    static bool parse(const uint8_t *buffer, size_t length, size_t &consumed, Frame &out_frame);
};

#endif
