#include <bits/stdc++.h>
#include <endian.h>
#include <arpa/inet.h>

#include "../include/packet.hpp"

using namespace std;

Packet::Packet()
    : flags(FLAG_NONE), path_id(0), packet_num(0), ack_num(0), timestamp_us(0) {}

Packet::Packet(uint8_t flags, uint8_t path_id, uint32_t packet_num, uint32_t ack_num,
               uint64_t timestamp_us, const vector<uint8_t> &payload)
    : flags(flags), path_id(path_id), packet_num(packet_num), ack_num(ack_num),
      timestamp_us(timestamp_us), payload(payload) {}

uint8_t Packet::get_flags() const
{
    return flags;
}

void Packet::set_flags(uint8_t f)
{
    flags = f;
}

bool Packet::has_flag(uint8_t flag) const
{
    return (flags & flag) != 0;
}

void Packet::set_flag(uint8_t flag)
{
    flags |= flag;
}

void Packet::clear_flag(uint8_t flag)
{
    flags &= ~flag;
}

uint8_t Packet::get_path_id() const
{
    return path_id;
}

void Packet::set_path_id(uint8_t id)
{
    path_id = id;
}

size_t Packet::get_payload_len() const
{
    return payload.size();
}

uint32_t Packet::get_packet_num() const
{
    return packet_num;
}

void Packet::set_packet_num(uint32_t num)
{
    packet_num = num;
}

uint32_t Packet::get_ack_num() const
{
    return ack_num;
}

void Packet::set_ack_num(uint32_t ack)
{
    ack_num = ack;
}

uint64_t Packet::get_timestamp_us() const
{
    return timestamp_us;
}

void Packet::set_timestamp_us(uint64_t ts)
{
    timestamp_us = ts;
}

const vector<uint8_t> &Packet::get_payload() const
{
    return payload;
}

void Packet::set_payload(const vector<uint8_t> &data)
{
    payload = data;
}

vector<uint8_t> Packet::serialize() const
{
    if (payload.size() > UINT16_MAX)
    {
        throw length_error("Payload exceeds maximum packet size (65535 bytes)");
    }

    size_t total_size = HEADER_SIZE + payload.size();
    vector<uint8_t> buffer(total_size);

    buffer[0] = flags;
    buffer[1] = path_id;

    uint16_t plen = htons(static_cast<uint16_t>(payload.size()));
    memcpy(buffer.data() + 2, &plen, sizeof(plen));

    uint32_t pnum = htonl(packet_num);
    memcpy(buffer.data() + 4, &pnum, sizeof(pnum));

    uint32_t anum = htonl(ack_num);
    memcpy(buffer.data() + 8, &anum, sizeof(anum));

    uint64_t ts = htobe64(timestamp_us);
    memcpy(buffer.data() + 12, &ts, sizeof(ts));

    if (!payload.empty())
    {
        memcpy(buffer.data() + HEADER_SIZE, payload.data(), payload.size());
    }

    return buffer;
}

bool Packet::deserialize(const uint8_t *buffer, size_t length, Packet &out_packet)
{
    if (buffer == nullptr || length < HEADER_SIZE)
    {
        return false;
    }

    uint8_t flags = buffer[0];
    uint8_t path_id = buffer[1];

    uint16_t raw_plen = 0;
    memcpy(&raw_plen, buffer + 2, sizeof(raw_plen));
    uint16_t payload_len = ntohs(raw_plen);

    if (length != HEADER_SIZE + payload_len)
    {
        return false;
    }

    uint32_t raw_pnum = 0;
    memcpy(&raw_pnum, buffer + 4, sizeof(raw_pnum));

    uint32_t raw_anum = 0;
    memcpy(&raw_anum, buffer + 8, sizeof(raw_anum));

    uint64_t raw_ts = 0;
    memcpy(&raw_ts, buffer + 12, sizeof(raw_ts));

    out_packet.flags = flags;
    out_packet.path_id = path_id;
    out_packet.packet_num = ntohl(raw_pnum);
    out_packet.ack_num = ntohl(raw_anum);
    out_packet.timestamp_us = be64toh(raw_ts);

    if (payload_len > 0)
    {
        out_packet.payload.assign(buffer + HEADER_SIZE, buffer + HEADER_SIZE + payload_len);
    }
    else
    {
        out_packet.payload.clear();
    }

    return true;
}

uint64_t Packet::current_time_us()
{
    auto now = chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(chrono::duration_cast<chrono::microseconds>(now).count());
}
