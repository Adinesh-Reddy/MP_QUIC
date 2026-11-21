#include<bits/stdc++.h>
#include<endian.h>
#include<arpa/inet.h>

#include "packet/frame.hpp"

using namespace std;

// Write integers in big-endian (network) byte order

static void put_u16(vector<uint8_t> &out, uint16_t value)
{
    uint16_t net=htobe16(value);
    const uint8_t *p=reinterpret_cast<const uint8_t *>(&net);
    out.insert(out.end(), p, p+sizeof(net));
}

static void put_u32(vector<uint8_t> &out, uint32_t value)
{
    uint32_t net=htobe32(value);
    const uint8_t *p=reinterpret_cast<const uint8_t *>(&net);
    out.insert(out.end(), p, p+sizeof(net));
}

static void put_u64(vector<uint8_t> &out, uint64_t value)
{
    uint64_t net=htobe64(value);
    const uint8_t *p=reinterpret_cast<const uint8_t *>(&net);
    out.insert(out.end(), p, p+sizeof(net));
}

// Return false if buffer does not have enough bytes left

static bool get_u8(const uint8_t *buffer, size_t length, size_t &pos, uint8_t &value)
{
    if(length-pos<sizeof(value))
        return false;
    value=buffer[pos];
    pos+=sizeof(value);
    return true;
}

static bool get_u16(const uint8_t *buffer, size_t length, size_t &pos, uint16_t &value)
{
    if(length-pos<sizeof(value))
        return false;
    uint16_t net=0;
    memcpy(&net, buffer+pos, sizeof(net));
    value=be16toh(net);
    pos+=sizeof(value);
    return true;
}

static bool get_u32(const uint8_t *buffer, size_t length, size_t &pos, uint32_t &value)
{
    if(length-pos<sizeof(value))
        return false;
    uint32_t net=0;
    memcpy(&net, buffer+pos, sizeof(net));
    value=be32toh(net);
    pos+=sizeof(value);
    return true;
}

static bool get_u64(const uint8_t *buffer, size_t length, size_t &pos, uint64_t &value)
{
    if(length-pos<sizeof(value))
        return false;
    uint64_t net=0;
    memcpy(&net, buffer+pos, sizeof(net));
    value=be64toh(net);
    pos+=sizeof(value);
    return true;
}

// Variable-length ints (see frame.hpp)

static size_t varint_size(uint64_t value)
{
    if(value<(1ULL<<6))
        return 1;
    if(value<(1ULL<<14))
        return 2;
    if(value<(1ULL<<30))
        return 4;
    return 8;
}

static void put_varint(vector<uint8_t> &out, uint64_t value)
{
    size_t size=varint_size(value);

    // Top 2 bits of byte 0 store the size prefix
    uint8_t prefix=0x00;
    if(size==2)
        prefix=0x40;
    else if(size==4)
        prefix=0x80;
    else if(size==8)
        prefix=0xC0;

    for(size_t i=0; i<size; i++){
        uint8_t byte=static_cast<uint8_t>((value>>(8*(size-1-i)))&0xFF);
        if(i==0){
            byte|=prefix;
        }
        out.push_back(byte);
    }
}

static bool get_varint(const uint8_t *buffer, size_t length, size_t &pos, uint64_t &value)
{
    if(length-pos<1)
        return false;

    size_t size=static_cast<size_t>(1)<<(buffer[pos]>>6); // 1, 2, 4 or 8
    if(length-pos<size)
        return false;

    value=buffer[pos]&0x3F;
    for(size_t i=1; i<size; i++){
        value=(value<<8)|buffer[pos+i];
    }
    pos+=size;
    return true;
}

// Count valid ACK ranges up to MAX_ACK_RANGES
static size_t writable_ack_ranges(const AckFrame &ack)
{
    size_t count=0;
    for(size_t i=0; i<ack.ranges.size()&&i<MAX_ACK_RANGES; i++){
        const AckRange &r=ack.ranges[i];
        if(r.first==0||r.first>r.last||r.last-r.first>MAX_VARINT_VALUE){
            break;
        }
        if(i==0){
            if(r.last!=ack.largest_acked)
                break;
        }
        else{
            const AckRange &above=ack.ranges[i-1];
            if(r.last+1>=above.first||above.first-r.last-1>MAX_VARINT_VALUE)
                break;
        }
        count++;
    }
    return count;
}

uint64_t AckFrame::lowest_acked() const
{
    if(ranges.empty())
        return 0;
    return ranges.back().first;
}

bool AckFrame::acks_packet(uint64_t packet_number) const
{
    for(const auto &r : ranges){
        if(packet_number>=r.first&&packet_number<=r.last)
            return true;
    }
    return false;
}

Frame Frame::make_stream(uint32_t stream_id, uint64_t offset, bool fin, const vector<uint8_t> &data)
{
    Frame frame;
    frame.type=FRAME_STREAM;
    frame.stream.stream_id=stream_id;
    frame.stream.offset=offset;
    frame.stream.fin=fin;
    frame.stream.data=data;
    return frame;
}

Frame Frame::make_ack(const AckFrame &ack)
{
    Frame frame;
    frame.type=FRAME_ACK;
    frame.ack=ack;
    return frame;
}

Frame Frame::make_window_update(uint32_t stream_id, uint64_t byte_offset)
{
    Frame frame;
    frame.type=FRAME_WINDOW_UPDATE;
    frame.window_update.stream_id=stream_id;
    frame.window_update.byte_offset=byte_offset;
    return frame;
}

Frame Frame::make_ping()
{
    Frame frame;
    frame.type=FRAME_PING;
    return frame;
}

Frame Frame::make_paths(const PathsFrame &paths)
{
    Frame frame;
    frame.type=FRAME_PATHS;
    frame.paths=paths;
    return frame;
}

Frame Frame::make_add_address(const vector<NetworkAddress> &addresses)
{
    Frame frame;
    frame.type=FRAME_ADD_ADDRESS;
    frame.add_address.addresses=addresses;
    return frame;
}

Frame Frame::make_connection_close(uint32_t error_code, const string &reason)
{
    Frame frame;
    frame.type=FRAME_CONNECTION_CLOSE;
    frame.connection_close.error_code=error_code;
    frame.connection_close.reason=reason.substr(0, MAX_CLOSE_REASON_LENGTH);
    return frame;
}

bool Frame::is_retransmittable() const
{
    return type!=FRAME_ACK;
}

size_t Frame::size() const
{
    switch(type){
    case FRAME_STREAM:
        return STREAM_FRAME_HEADER_SIZE+stream.data.size();
    case FRAME_ACK:
    {
        size_t total=ACK_FRAME_HEADER_SIZE;
        size_t num_ranges=writable_ack_ranges(ack);
        for(size_t i=0; i<num_ranges; i++){
            if(i>0){
                total+=varint_size(ack.ranges[i-1].first-ack.ranges[i].last-1); // gap
            }
            total+=varint_size(ack.ranges[i].last-ack.ranges[i].first); // length
        }
        return total;
    }
    case FRAME_WINDOW_UPDATE:
        return WINDOW_UPDATE_FRAME_SIZE;
    case FRAME_PATHS:
        return PATHS_FRAME_HEADER_SIZE+min<size_t>(paths.paths.size(), UINT8_MAX)*PATHS_ENTRY_SIZE;
    case FRAME_ADD_ADDRESS:
        return ADD_ADDRESS_FRAME_HEADER_SIZE+
               min<size_t>(add_address.addresses.size(), UINT8_MAX)*ADD_ADDRESS_ENTRY_SIZE;
    case FRAME_CONNECTION_CLOSE:
        return CONNECTION_CLOSE_FRAME_HEADER_SIZE+min(connection_close.reason.size(), MAX_CLOSE_REASON_LENGTH);
    default:
        return PING_FRAME_SIZE;
    }
}

void Frame::serialize(vector<uint8_t> &out) const
{
    out.push_back(type);

    switch(type){
    case FRAME_STREAM:
    {
        if(stream.data.size()>UINT16_MAX){
            throw length_error("STREAM frame too big: "+to_string(stream.data.size())+
                               " bytes (max 65535)");
        }
        put_u32(out, stream.stream_id);
        put_u64(out, stream.offset);
        out.push_back(stream.fin ? 1 : 0);
        put_u16(out, static_cast<uint16_t>(stream.data.size()));
        out.insert(out.end(), stream.data.begin(), stream.data.end());
        break;
    }
    case FRAME_ACK:
    {
        // Write up to MAX_ACK_RANGES highest ranges
        size_t num_ranges=writable_ack_ranges(ack);
        out.push_back(ack.path_id);
        put_u64(out, ack.largest_acked);
        put_u32(out, ack.ack_delay_us);
        put_u16(out, static_cast<uint16_t>(num_ranges));
        for(size_t i=0; i<num_ranges; i++){
            if(i>0){
                // Missing packets between this range and the one above it
                put_varint(out, ack.ranges[i-1].first-ack.ranges[i].last-1);
            }
            // Packet count in this range minus 1
            put_varint(out, ack.ranges[i].last-ack.ranges[i].first);
        }
        break;
    }
    case FRAME_ADD_ADDRESS:
    {
        size_t num_addresses=min<size_t>(add_address.addresses.size(), UINT8_MAX);
        out.push_back(static_cast<uint8_t>(num_addresses));
        for(size_t i=0; i<num_addresses; i++){
            in_addr ipv4;
            if(inet_pton(AF_INET, add_address.addresses[i].ip.c_str(), &ipv4)!=1){
                throw invalid_argument("Bad IPv4 address in ADD_ADDRESS: \""+add_address.addresses[i].ip+"\"");
            }
            const uint8_t *p=reinterpret_cast<const uint8_t *>(&ipv4.s_addr);
            out.insert(out.end(), p, p+4);
            put_u16(out, add_address.addresses[i].port);
        }
        break;
    }
    case FRAME_CONNECTION_CLOSE:
    {
        size_t reason_length=min(connection_close.reason.size(), MAX_CLOSE_REASON_LENGTH);
        put_u32(out, connection_close.error_code);
        put_u16(out, static_cast<uint16_t>(reason_length));
        out.insert(out.end(), connection_close.reason.begin(), connection_close.reason.begin()+reason_length);
        break;
    }
    case FRAME_PATHS:
    {
        size_t num_paths=min<size_t>(paths.paths.size(), UINT8_MAX);
        out.push_back(static_cast<uint8_t>(num_paths));
        for(size_t i=0; i<num_paths; i++){
            out.push_back(paths.paths[i].path_id);
            put_u32(out, paths.paths[i].rtt_us);
        }
        break;
    }
    case FRAME_WINDOW_UPDATE:
    {
        put_u32(out, window_update.stream_id);
        put_u64(out, window_update.byte_offset);
        break;
    }
    default:
        break; // PING has no body
    }
}

bool Frame::parse(const uint8_t *buffer, size_t length, size_t &consumed, Frame &out_frame)
{
    if(buffer==nullptr||length<1){
        return false;
    }

    size_t pos=0;
    Frame frame;
    frame.type=buffer[pos++];

    switch(frame.type){
    case FRAME_STREAM:
    {
        uint8_t fin=0;
        uint16_t data_len=0;
        if(!get_u32(buffer, length, pos, frame.stream.stream_id)||
            !get_u64(buffer, length, pos, frame.stream.offset)||
            !get_u8(buffer, length, pos, fin)||
            !get_u16(buffer, length, pos, data_len)){
            return false;
        }
        if(fin>1||length-pos<data_len){
            return false;
        }
        // Empty STREAM frame is valid only with FIN
        if(data_len==0&&fin==0){
            return false;
        }
        frame.stream.fin=(fin==1);
        frame.stream.data.assign(buffer+pos, buffer+pos+data_len);
        pos+=data_len;
        break;
    }
    case FRAME_ACK:
    {
        uint16_t num_ranges=0;
        if(!get_u8(buffer, length, pos, frame.ack.path_id)||
            !get_u64(buffer, length, pos, frame.ack.largest_acked)||
            !get_u32(buffer, length, pos, frame.ack.ack_delay_us)||
            !get_u16(buffer, length, pos, num_ranges)){
            return false;
        }
        if(num_ranges==0||num_ranges>MAX_ACK_RANGES){
            return false;
        }
        frame.ack.ranges.reserve(num_ranges);

        // Highest range ends at largest_acked; packet 0 is invalid
        uint64_t first_length=0;
        if(!get_varint(buffer, length, pos, first_length)||first_length>=frame.ack.largest_acked){
            return false;
        }
        AckRange highest;
        highest.last=frame.ack.largest_acked;
        highest.first=frame.ack.largest_acked-first_length;
        frame.ack.ranges.push_back(highest);

        // Next ranges are relative to the previous range
        for(size_t i=1; i<num_ranges; i++){
            uint64_t gap=0;
            uint64_t range_length=0;
            if(!get_varint(buffer, length, pos, gap)||!get_varint(buffer, length, pos, range_length)){
                return false;
            }

            // Must have a gap >= 1 and stay above packet 0
            uint64_t above_first=frame.ack.ranges.back().first;
            if(gap==0||above_first<gap+2){
                return false;
            }

            AckRange r;
            r.last=above_first-gap-1;
            if(range_length>=r.last){
                return false;
            }
            r.first=r.last-range_length;
            frame.ack.ranges.push_back(r);
        }
        break;
    }
    case FRAME_WINDOW_UPDATE:
    {
        if(!get_u32(buffer, length, pos, frame.window_update.stream_id)||
            !get_u64(buffer, length, pos, frame.window_update.byte_offset)){
            return false;
        }
        break;
    }
    case FRAME_PING:
        break;
    case FRAME_PATHS:
    {
        uint8_t num_paths=0;
        if(!get_u8(buffer, length, pos, num_paths)||num_paths==0){
            return false;
        }
        for(size_t i=0; i<num_paths; i++){
            PathInfo info;
            if(!get_u8(buffer, length, pos, info.path_id)||!get_u32(buffer, length, pos, info.rtt_us)){
                return false;
            }
            frame.paths.paths.push_back(info);
        }
        break;
    }
    case FRAME_ADD_ADDRESS:
    {
        uint8_t num_addresses=0;
        if(!get_u8(buffer, length, pos, num_addresses)||num_addresses==0){
            return false;
        }
        for(size_t i=0; i<num_addresses; i++){
            if(length-pos<ADD_ADDRESS_ENTRY_SIZE){
                return false;
            }
            in_addr ipv4;
            memcpy(&ipv4.s_addr, buffer+pos, 4);
            pos+=4;

            char text[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &ipv4, text, sizeof(text));

            NetworkAddress address;
            address.ip=text;
            if(!get_u16(buffer, length, pos, address.port)){
                return false;
            }
            frame.add_address.addresses.push_back(address);
        }
        break;
    }
    case FRAME_CONNECTION_CLOSE:
    {
        uint16_t reason_length=0;
        if(!get_u32(buffer, length, pos, frame.connection_close.error_code)||
            !get_u16(buffer, length, pos, reason_length)){
            return false;
        }
        if(reason_length>MAX_CLOSE_REASON_LENGTH||length-pos<reason_length){
            return false;
        }
        frame.connection_close.reason.assign(reinterpret_cast<const char *>(buffer+pos), reason_length);
        pos+=reason_length;
        break;
    }
    default:
        return false; // unknown frame type
    }

    consumed=pos;
    out_frame=frame;
    return true;
}
