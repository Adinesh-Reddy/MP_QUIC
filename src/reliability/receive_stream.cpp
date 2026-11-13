#include <bits/stdc++.h>

#include "reliability/receive_stream.hpp"

using namespace std;

ReceiveStream::ReceiveStream(uint32_t stream_id)
{
    this->stream_id = stream_id;
}

uint32_t ReceiveStream::get_stream_id() const
{
    return stream_id;
}

void ReceiveStream::add_stream_frame(const StreamFrame &frame)
{
    // The FIN tells where the stream ends
    if (frame.fin && !fin_known)
    {
        fin_known = true;
        fin_offset = frame.offset + frame.data.size();
    }

    if (!frame.data.empty())
    {
        sorter.push(frame.offset, frame.data);
    }
}

bool ReceiveStream::pop(uint64_t &offset, vector<uint8_t> &data, bool &fin)
{
    if (fin_delivered)
    {
        return false;
    }

    if (sorter.pop(offset, data))
    {
        fin = fin_known && (sorter.get_read_position() == fin_offset);
        if (fin)
        {
            fin_delivered = true;
        }
        return true;
    }

    // All the data was already delivered and the FIN arrived afterwards (or the stream is empty)
    if (fin_known && sorter.get_read_position() == fin_offset)
    {
        offset = fin_offset;
        data.clear();
        fin = true;
        fin_delivered = true;
        return true;
    }

    return false;
}

uint64_t ReceiveStream::get_read_offset() const
{
    return sorter.get_read_position();
}

bool ReceiveStream::is_finished() const
{
    return fin_delivered;
}

uint64_t ReceiveStream::get_buffered_bytes() const
{
    return sorter.get_buffered_bytes();
}
