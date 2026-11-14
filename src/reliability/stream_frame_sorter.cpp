#include <bits/stdc++.h>

#include "reliability/stream_frame_sorter.hpp"

using namespace std;

bool StreamFrameSorter::push(uint64_t offset, const vector<uint8_t> &data)
{
    uint64_t start = offset;
    uint64_t end = offset + data.size();

    // Bytes below delivered_up_to were already delivered
    if (start < delivered_up_to)
    {
        start = delivered_up_to;
    }
    if (start >= end)
    {
        return false;
    }

    bool stored_something = false;
    uint64_t cursor = start; // first byte of the frame we still have to place

    // it = first chunk that starts after cursor
    auto it = waiting.upper_bound(cursor);

    // The chunk before it may already cover the beginning of the frame
    if (it != waiting.begin())
    {
        auto before = it;
        --before;
        uint64_t before_end = before->first + before->second.size();
        if (before_end > cursor)
        {
            cursor = before_end;
        }
    }

    // Walk over the frame and store only the parts that fall in a hole
    while (cursor < end)
    {
        uint64_t next_start = (it != waiting.end()) ? it->first : end;
        uint64_t piece_end = min(end, next_start);

        if (piece_end > cursor)
        {
            waiting[cursor] = vector<uint8_t>(data.begin() + (cursor - offset), data.begin() + (piece_end - offset));
            waiting_bytes += piece_end - cursor;
            stored_something = true;
        }

        if (it == waiting.end() || next_start >= end)
        {
            break;
        }

        // Skip the bytes we already have and continue after that chunk
        cursor = max(cursor, it->first + it->second.size());
        ++it;
    }

    return stored_something;
}

bool StreamFrameSorter::pop(uint64_t &offset, vector<uint8_t> &data)
{
    auto it = waiting.begin();
    if (it == waiting.end() || it->first != delivered_up_to)
    {
        return false; // the next byte is still missing
    }

    offset = it->first;
    data = it->second;

    delivered_up_to += data.size();
    waiting_bytes -= data.size();
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
