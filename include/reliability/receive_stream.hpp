#ifndef RECEIVE_STREAM_HPP
#define RECEIVE_STREAM_HPP

#include<bits/stdc++.h>
#include "packet/frame.hpp"
#include "reliability/stream_frame_sorter.hpp"

using namespace std;

// Receive side of one stream: reorders frames and tracks FIN
class ReceiveStream
{
public:
    ReceiveStream(uint32_t stream_id);

    uint32_t get_stream_id() const;

    /*
     Add a STREAM frame (out-of-order and duplicates are ok).
     Returns false if it conflicts with stream end (moved FIN or data past FIN).
    */
    bool add_stream_frame(const StreamFrame &frame);

    // Pop next in-order data; fin=true on the final chunk. False if next byte is missing.
    bool pop(uint64_t &offset, vector<uint8_t> &data, bool &fin);

    // Offset of next byte to deliver
    uint64_t get_read_offset() const;

    // True once all bytes up to FIN are delivered
    bool is_finished() const;

    uint64_t get_buffered_bytes() const;

private:
    uint32_t stream_id;
    StreamFrameSorter sorter;

    // Track FIN as an offset because frames can get trimmed on overlap
    bool fin_known=false;
    uint64_t fin_offset=0;
    bool fin_delivered=false;

    // Highest offset + len accepted so far
    uint64_t highest_end=0;
};

#endif
