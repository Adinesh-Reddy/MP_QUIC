#ifndef STREAM_FRAME_SORTER_HPP
#define STREAM_FRAME_SORTER_HPP

#include <bits/stdc++.h>

using namespace std;

// Reorder buffer of one stream. STREAM frames can arrive in any order (they travel on
// different paths and can be retransmitted), their byte offset is enough to put them
// back in order. Data that was already received is cut away, so every byte of the
// stream is given out exactly once and in increasing offset order.
class StreamFrameSorter
{
public:
    // Stores the bytes of [offset, offset + data.size()) that are still missing.
    // Returns false if the frame brought nothing new (duplicate).
    bool push(uint64_t offset, const vector<uint8_t> &data);

    // Gives the next in-order piece of data, false if the next byte has not arrived yet
    bool pop(uint64_t &offset, vector<uint8_t> &data);

    // Offset of the next byte that pop() will deliver
    uint64_t get_read_position() const;

    size_t get_num_chunks() const;
    uint64_t get_buffered_bytes() const;

private:
    // Data that arrived ahead of the read position: start offset -> bytes.
    // Pieces never overlap, and all of them start at or after delivered_up_to.
    map<uint64_t, vector<uint8_t>> waiting;

    uint64_t delivered_up_to = 0;
    uint64_t waiting_bytes = 0;
};

#endif
