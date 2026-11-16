#include<bits/stdc++.h>

#include "reliability/stream_frame_sorter.hpp"

using namespace std;

bool StreamFrameSorter::push(uint64_t offset, const vector<uint8_t> &data)
{
    uint64_t start=offset;
    uint64_t end=offset+data.size();

    // Skip bytes already delivered
    if(start<delivered_up_to){
        start=delivered_up_to;
    }
    if(start>=end){
        return false;
    }

    bool stored_something=false;
    uint64_t cursor=start; // next unplaced byte offset

    // First chunk starting after cursor
    auto it=waiting.upper_bound(cursor);

    // Previous chunk may overlap start of this frame
    if(it!=waiting.begin()){
        auto before=it;
        --before;
        uint64_t before_end=before->first+before->second.size();
        if(before_end>cursor){
            cursor=before_end;
        }
    }

    // Fill only missing gaps
    while(cursor<end){
        uint64_t next_start=(it!=waiting.end()) ? it->first : end;
        uint64_t piece_end=min(end, next_start);

        if(piece_end>cursor){
            waiting[cursor]=vector<uint8_t>(data.begin()+(cursor-offset), data.begin()+(piece_end-offset));
            waiting_bytes+=piece_end-cursor;
            stored_something=true;
        }

        if(it==waiting.end()||next_start>=end){
            break;
        }

        // Jump past existing chunk
        cursor=max(cursor, it->first+it->second.size());
        ++it;
    }

    return stored_something;
}

bool StreamFrameSorter::pop(uint64_t &offset, vector<uint8_t> &data)
{
    auto it=waiting.begin();
    if(it==waiting.end()||it->first!=delivered_up_to){
        return false; // next byte still missing
    }

    offset=it->first;
    data=it->second;

    delivered_up_to+=data.size();
    waiting_bytes-=data.size();
    waiting.erase(it);
    return true;
}

uint64_t StreamFrameSorter::get_read_position() const
{
    return delivered_up_to;
}

size_t StreamFrameSorter::get_num_chunks() const
{
    return waiting.size();
}

uint64_t StreamFrameSorter::get_buffered_bytes() const
{
    return waiting_bytes;
}
