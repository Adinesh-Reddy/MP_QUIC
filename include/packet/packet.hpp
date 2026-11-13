#ifndef PACKET_HPP
#define PACKET_HPP

#include <bits/stdc++.h>
#include <sys/types.h>

#include "packet/frame.hpp"

using namespace std;

// Public header flags. DATA / ACK / WINDOW_UPDATE are no longer flags, they are frames
// inside the packet. The bits are kept free for connection setup in a later phase.
const uint8_t FLAG_NONE = 0x00;

// Largest UDP payload we send (same value as the reference implementation)
const size_t MAX_PACKET_SIZE = 1350;

// Public header: a few flags, the Connection ID, the Path ID and the Packet Number.
// The Path ID tells which path (and so which packet number space) the packet belongs to.
#pragma pack(push, 1)
struct PacketHeader
{
    uint8_t  flags;
    uint64_t connection_id;
    uint8_t  path_id;
    uint64_t packet_number;
};
#pragma pack(pop)

using MPQUICHeader = PacketHeader;

class Packet
{
public:
    static constexpr size_t HEADER_SIZE = 18;

    Packet();

    Packet(uint8_t flags, uint64_t connection_id, uint8_t path_id, uint64_t packet_number);

    uint8_t get_flags() const;
    void set_flags(uint8_t flags);
    bool has_flag(uint8_t flag) const;
    void set_flag(uint8_t flag);
    void clear_flag(uint8_t flag);

    uint64_t get_connection_id() const;
    void set_connection_id(uint64_t cid);

    uint8_t get_path_id() const;
    void set_path_id(uint8_t id);

    uint64_t get_packet_number() const;
    void set_packet_number(uint64_t num);
    uint64_t get_packet_num() const;
    void set_packet_num(uint64_t num);

    const vector<Frame> &get_frames() const;
    void add_frame(const Frame &frame);
    void clear_frames();

    // Total serialized size (header + all frames)
    size_t size() const;

    // True if at least one frame must be delivered reliably (anything except ACK).
    // Only such packets are tracked by the sender and make the receiver send an ACK.
    bool is_retransmittable() const;

    vector<uint8_t> serialize() const;
    static bool deserialize(const uint8_t *buffer, size_t length, Packet &out_packet);

    // Reads only the Connection ID and the Path ID of a datagram, without parsing the frames.
    // A server uses it to find out which connection and which path a datagram belongs to.
    static bool peek_header(const uint8_t *buffer, size_t length, uint64_t &connection_id, uint8_t &path_id);

    static uint64_t current_time_us();

private:
    uint8_t  flags = FLAG_NONE;
    uint64_t connection_id = 0;
    uint8_t  path_id = 0;
    uint64_t packet_number = 0;
    vector<Frame> frames;
};

#endif
