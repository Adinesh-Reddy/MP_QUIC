#ifndef SEND_STREAM_HPP
#define SEND_STREAM_HPP

#include <bits/stdc++.h>
#include "packet/frame.hpp"

using namespace std;

// Sending side of one stream: keeps the bytes written by the application and
// cuts them into STREAM frames. Every frame carries its byte offset in the stream,
// which is what lets the receiver put back in order data that came over different paths.
class SendStream
{
public:
    SendStream(uint32_t stream_id);

    uint32_t get_stream_id() const;

    // Appends application data to the stream
    void write(const uint8_t *data, size_t length);

    // No more data will be written: the last frame will carry the FIN bit
    void close();

    // True if there is data (or a FIN) that was not put in a frame yet
    bool has_data() const;

    // Builds the next STREAM frame, not larger than max_frame_size bytes once serialized.
    // send_window = bytes of new data the flow control still allows on this stream.
    // Returns false if nothing can be sent (no room in the packet or no flow control credit).
    bool pop_stream_frame(size_t max_frame_size, Frame &out_frame, uint64_t send_window = UINT64_MAX);

    // Offset of the next byte that will be put in a frame
    uint64_t get_next_offset() const;

private:
    uint32_t stream_id;
    deque<uint8_t> buffer;      // bytes written by the application, not sent yet
    uint64_t next_offset = 0;
    bool closed = false;
    bool fin_sent = false;
};

#endif
