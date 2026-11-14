#include <bits/stdc++.h>

#include "reliability/send_stream.hpp"

using namespace std;

SendStream::SendStream(uint32_t stream_id)
{
    this->stream_id = stream_id;
}

uint32_t SendStream::get_stream_id() const
{
    return stream_id;
}

size_t SendStream::get_unsent_bytes() const
{
    return buffer.size();
}

void SendStream::write(const uint8_t *data, size_t length)
{
    if (closed || data == nullptr || length == 0)
    {
        return;
    }
    buffer.insert(buffer.end(), data, data + length);
}

void SendStream::close()
{
    closed = true;
}

bool SendStream::has_data() const
{
    return !buffer.empty() || (closed && !fin_sent);
}

bool SendStream::pop_stream_frame(size_t max_frame_size, Frame &out_frame, uint64_t send_window)
{
    if (!has_data() || max_frame_size < STREAM_FRAME_HEADER_SIZE)
    {
        return false;
    }

    size_t data_len = min(buffer.size(), max_frame_size - STREAM_FRAME_HEADER_SIZE);

    // Flow control: do not send beyond the window advertised by the receiver
    if (data_len > send_window)
    {
        data_len = static_cast<size_t>(send_window);
    }

    // The FIN goes in the frame that carries the last byte of the stream
    bool fin = closed && (data_len == buffer.size());

    // A frame without data is only useful to close the stream
    if (data_len == 0 && !fin)
    {
        return false;
    }

    vector<uint8_t> data(buffer.begin(), buffer.begin() + data_len);
    buffer.erase(buffer.begin(), buffer.begin() + data_len);

    out_frame = Frame::make_stream(stream_id, next_offset, fin, data);

    next_offset += data_len;
    if (fin)
    {
        fin_sent = true;
    }
    return true;
}

uint64_t SendStream::get_next_offset() const
{
    return next_offset;
}
