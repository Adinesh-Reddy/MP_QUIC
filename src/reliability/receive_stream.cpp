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

bool ReceiveStream::add_stream_frame(const StreamFrame &frame)
{
    uint64_t end = frame.offset + frame.data.size();

    // Once known, the end of the stream never moves
    if (fin_known && (frame.fin ? end != fin_offset : end > fin_offset))
    {
        return false;
    }
    // A first FIN cannot cut off bytes that have already arrived
    if (frame.fin && !fin_known && end < highest_end)
    {
        return false;
    }

    highest_end = max(highest_end, end);

    // The FIN tells where the stream ends
    if (frame.fin)
    {
        fin_known = true;
        fin_offset = end;
    }

    if (!frame.data.empty())
    {
        sorter.push(frame.offset, frame.data);
    }
    return true;
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
