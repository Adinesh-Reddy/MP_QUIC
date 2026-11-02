#ifndef PACKET_HPP
#define PACKET_HPP

#include <bits/stdc++.h>
#include <sys/types.h>

using namespace std;

const uint8_t FLAG_NONE = 0x00;
const uint8_t FLAG_DATA = 0x01;
const uint8_t FLAG_ACK  = 0x02;
const uint8_t FLAG_FIN  = 0x04;
const uint8_t FLAG_SYN  = 0x08;
const uint8_t FLAG_PING = 0x10;

#pragma pack(push, 1)
struct PacketHeader
{
    uint8_t flags;
    uint8_t path_id;
    uint16_t payload_len;
    uint32_t packet_num;
    uint32_t ack_num;
    uint64_t timestamp_us;
};
#pragma pack(pop)

class Packet
{
public:
    static const size_t HEADER_SIZE = 20;

    Packet();
    Packet(uint8_t flags, uint8_t path_id, uint32_t packet_num, uint32_t ack_num,
           uint64_t timestamp_us, const vector<uint8_t> &payload);

    uint8_t get_flags() const;
    void set_flags(uint8_t flags);
    bool has_flag(uint8_t flag) const;
    void set_flag(uint8_t flag);
    void clear_flag(uint8_t flag);

    uint8_t get_path_id() const;
    void set_path_id(uint8_t id);

    size_t get_payload_len() const;

    uint32_t get_packet_num() const;
    void set_packet_num(uint32_t num);

    uint32_t get_ack_num() const;
    void set_ack_num(uint32_t ack);

    uint64_t get_timestamp_us() const;
    void set_timestamp_us(uint64_t ts);

    const vector<uint8_t> &get_payload() const;
    void set_payload(const vector<uint8_t> &data);

    vector<uint8_t> serialize() const;
    static bool deserialize(const uint8_t *buffer, size_t length, Packet &out_packet);

    static uint64_t current_time_us();

private:
    uint8_t flags = FLAG_NONE;
    uint8_t path_id = 0;
    uint32_t packet_num = 0;
    uint32_t ack_num = 0;
    uint64_t timestamp_us = 0;
    vector<uint8_t> payload;
};

#endif
