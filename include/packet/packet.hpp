#ifndef PACKET_HPP
#define PACKET_HPP

#include<bits/stdc++.h>
#include<sys/types.h>

#include "packet/frame.hpp"

using namespace std;

// Public header flags (DATA/ACK/WINDOW_UPDATE are frames inside the packet)
const uint8_t FLAG_NONE=0x00;

// Max UDP payload size (fits inside 1500-byte MTU with IP/UDP/tunnel headers)
const size_t MAX_PACKET_SIZE=1350;

// 18-byte public header; path_id selects the packet number space
#pragma pack(push, 1)
struct PacketHeader
{
    uint8_t  flags;
    uint64_t connection_id;
    uint8_t  path_id;
    uint64_t packet_number;
};
#pragma pack(pop)

using MPQUICHeader=PacketHeader;

class Packet
{
public:
    static constexpr size_t HEADER_SIZE=18;

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

    // True if packet has at least one reliable frame (anything except ACK)
    bool is_retransmittable() const;

    vector<uint8_t> serialize() const;
    static bool deserialize(const uint8_t *buffer, size_t length, Packet &out_packet);

    // Read only connection_id and path_id without parsing frames
    static bool peek_header(const uint8_t *buffer, size_t length, uint64_t &connection_id, uint8_t &path_id);

    static uint64_t current_time_us();

private:
    uint8_t  flags=FLAG_NONE;
    uint64_t connection_id=0;
    uint8_t  path_id=0;
    uint64_t packet_number=0;
    vector<Frame> frames;
};

#endif
