#ifndef RECEIVE_STREAM_HPP
#define RECEIVE_STREAM_HPP

#include <bits/stdc++.h>
#include "packet/frame.hpp"
#include "reliability/stream_frame_sorter.hpp"

using namespace std;

// Receiving side of one stream: reorder buffer + end of stream (FIN) tracking
class ReceiveStream
{
public:
    ReceiveStream(uint32_t stream_id);

    uint32_t get_stream_id() const;

    // Stores a received STREAM frame (in any order, duplicates are fine)
    void add_stream_frame(const StreamFrame &frame);

    // Next piece of data in stream order. fin is true on the piece that ends the stream.
    // Returns false if the next byte has not arrived yet.
    bool pop(uint64_t &offset, vector<uint8_t> &data, bool &fin);

    // Offset of the next byte that will be delivered
    uint64_t get_read_offset() const;

    // True once the whole stream (up to the FIN) was delivered
    bool is_finished() const;

    uint64_t get_buffered_bytes() const;

private:
    uint32_t stream_id;
    StreamFrameSorter sorter;

    // The FIN is remembered as an offset and not kept on a frame: a frame can be cut
    // when it overlaps data we already have, and then it would not end the stream anymore
    bool fin_known = false;
    uint64_t fin_offset = 0;
    bool fin_delivered = false;
};

#endif
