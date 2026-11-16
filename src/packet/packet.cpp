#include<bits/stdc++.h>
#include<endian.h>
#include<arpa/inet.h>

#include "packet/packet.hpp"

using namespace std;

Packet::Packet()
    : flags(FLAG_NONE), connection_id(0), path_id(0), packet_number(0) {}

Packet::Packet(uint8_t flags, uint64_t connection_id, uint8_t path_id, uint64_t packet_number)
    : flags(flags), connection_id(connection_id), path_id(path_id), packet_number(packet_number) {}

uint8_t Packet::get_flags() const
{
    return flags;
}

void Packet::set_flags(uint8_t f)
{
    flags=f;
}

bool Packet::has_flag(uint8_t flag) const
{
    return (flags&flag)!=0;
}

void Packet::set_flag(uint8_t flag)
{
    flags|=flag;
}

void Packet::clear_flag(uint8_t flag)
{
    flags&=~flag;
}

uint64_t Packet::get_connection_id() const
{
    return connection_id;
}

void Packet::set_connection_id(uint64_t cid)
{
    connection_id=cid;
}

uint8_t Packet::get_path_id() const
{
    return path_id;
}

void Packet::set_path_id(uint8_t id)
{
    path_id=id;
}

uint64_t Packet::get_packet_number() const
{
    return packet_number;
}

void Packet::set_packet_number(uint64_t num)
{
    packet_number=num;
}

uint64_t Packet::get_packet_num() const
{
    return packet_number;
}

void Packet::set_packet_num(uint64_t num)
{
    packet_number=num;
}

const vector<Frame> &Packet::get_frames() const
{
    return frames;
}

void Packet::add_frame(const Frame &frame)
{
    frames.push_back(frame);
}

void Packet::clear_frames()
{
    frames.clear();
}

size_t Packet::size() const
{
    size_t total_size=HEADER_SIZE;
    for(const auto &frame : frames){
        total_size+=frame.size();
    }
    return total_size;
}

bool Packet::is_retransmittable() const
{
    for(const auto &frame : frames){
        if(frame.is_retransmittable())
            return true;
    }
    return false;
}

vector<uint8_t> Packet::serialize() const
{
    size_t total_size=size();
    if(total_size>MAX_PACKET_SIZE){
        throw length_error("Packet too big: "+to_string(total_size)+" bytes (max "+
                           to_string(MAX_PACKET_SIZE)+")");
    }

    vector<uint8_t> buffer(HEADER_SIZE);
    buffer.reserve(total_size);

    buffer[0]=flags;

    uint64_t cid=htobe64(connection_id);
    memcpy(buffer.data()+1, &cid, sizeof(cid));

    buffer[9]=path_id;

    uint64_t pnum=htobe64(packet_number);
    memcpy(buffer.data()+10, &pnum, sizeof(pnum));

    for(const auto &frame : frames){
        frame.serialize(buffer);
    }

    return buffer;
}

bool Packet::deserialize(const uint8_t *buffer, size_t length, Packet &out_packet)
{
    if(buffer==nullptr||length<HEADER_SIZE){
        return false;
    }

    uint64_t raw_cid=0;
    memcpy(&raw_cid, buffer+1, sizeof(raw_cid));

    uint64_t raw_pnum=0;
    memcpy(&raw_pnum, buffer+10, sizeof(raw_pnum));

    // Read frames after header
    vector<Frame> parsed_frames;
    size_t pos=HEADER_SIZE;
    while(pos<length){
        Frame frame;
        size_t consumed=0;
        if(!Frame::parse(buffer+pos, length-pos, consumed, frame)){
            return false;
        }
        parsed_frames.push_back(frame);
        pos+=consumed;
    }

    // Empty packet is invalid
    if(parsed_frames.empty()){
        return false;
    }

    out_packet.flags=buffer[0];
    out_packet.connection_id=be64toh(raw_cid);
    out_packet.path_id=buffer[9];
    out_packet.packet_number=be64toh(raw_pnum);
    out_packet.frames=parsed_frames;

    return true;
}

bool Packet::peek_header(const uint8_t *buffer, size_t length, uint64_t &connection_id, uint8_t &path_id)
{
    if(buffer==nullptr||length<HEADER_SIZE){
        return false;
    }

    uint64_t raw_cid=0;
    memcpy(&raw_cid, buffer+1, sizeof(raw_cid));
    connection_id=be64toh(raw_cid);
    path_id=buffer[9];
    return true;
}

uint64_t Packet::current_time_us()
{
    auto now=chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(chrono::duration_cast<chrono::microseconds>(now).count());
}
