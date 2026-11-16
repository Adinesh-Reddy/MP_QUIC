#ifndef STREAM_FRAME_SORTER_HPP
#define STREAM_FRAME_SORTER_HPP

#include<bits/stdc++.h>

using namespace std;

/*
 Reorder buffer for one stream. Trims duplicate/overlapping bytes and gives
 data out in increasing offset order, each byte once.
*/
class StreamFrameSorter
{
public:
    // Store missing bytes in [offset, offset + data.size()); false if fully duplicate
    bool push(uint64_t offset, const vector<uint8_t> &data);

    // Get next in-order chunk; false if next byte is still missing
    bool pop(uint64_t &offset, vector<uint8_t> &data);

    // Offset of next byte pop() will give
    uint64_t get_read_position() const;

    size_t get_num_chunks() const;
    uint64_t get_buffered_bytes() const;

private:
    // Out-of-order chunks: start_offset -> bytes (non-overlapping, >= delivered_up_to)
    map<uint64_t, vector<uint8_t>> waiting;

    uint64_t delivered_up_to=0;
    uint64_t waiting_bytes=0;
};

#endif
