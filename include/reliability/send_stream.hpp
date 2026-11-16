#ifndef SEND_STREAM_HPP
#define SEND_STREAM_HPP

#include<bits/stdc++.h>
#include "packet/frame.hpp"

using namespace std;

/*
 Send side of one stream: buffers app bytes and splits them into STREAM frames.
 Each frame carries its byte offset so the receiver can reorder frames across paths.
*/
class SendStream
{
public:
    SendStream(uint32_t stream_id);

    uint32_t get_stream_id() const;

    // Add bytes to stream buffer
    void write(const uint8_t *data, size_t length);

    // Mark end of stream; last frame will set FIN=1
    void close();

    // True if data or FIN is waiting to be framed
    bool has_data() const;

    /*
     Build next STREAM frame fitting within max_frame_size and send_window.
     Returns false if packet space or flow control window is full.
    */
    bool pop_stream_frame(size_t max_frame_size, Frame &out_frame, uint64_t send_window=UINT64_MAX);

    // Next byte offset to send
    uint64_t get_next_offset() const;

    // Bytes written by app that are not framed yet (for chunked file writes)
    size_t get_unsent_bytes() const;

private:
    uint32_t stream_id;
    deque<uint8_t> buffer;      // unsent app bytes
    uint64_t next_offset=0;
    bool closed=false;
    bool fin_sent=false;
};

#endif
