#include <bits/stdc++.h>

#include "reliability/stream_frame_sorter.hpp"

using namespace std;

bool StreamFrameSorter::push(uint64_t offset, const vector<uint8_t> &data)
{
    uint64_t start = offset;
    uint64_t end = offset + data.size();

    // Bytes below read_position were already delivered
    if (start < read_position)
    {
        start = read_position;
    }
    if (start >= end)
    {
        return false;
    }

    bool inserted = false;
    uint64_t cur = start; // first byte of the frame we still have to place

    // it = first chunk that starts after cur
    auto it = chunks.upper_bound(cur);

    // The chunk before it may already cover the beginning of the frame
    if (it != chunks.begin())
    {
        auto prev_it = it;
        --prev_it;
        uint64_t prev_end = prev_it->first + prev_it->second.size();
        if (prev_end > cur)
        {
            cur = prev_end;
        }
    }

    // Walk over the frame and store only the parts that fall in a hole
    while (cur < end)
    {
        uint64_t next_start = (it != chunks.end()) ? it->first : end;
        uint64_t piece_end = min(end, next_start);

        if (piece_end > cur)
        {
            chunks[cur] = vector<uint8_t>(data.begin() + (cur - offset), data.begin() + (piece_end - offset));
            buffered_bytes += piece_end - cur;
            inserted = true;
        }

        if (it == chunks.end() || next_start >= end)
        {
            break;
        }

        // Skip the bytes we already have and continue after that chunk
        cur = max(cur, it->first + it->second.size());
        ++it;
    }

    return inserted;
}

bool StreamFrameSorter::pop(uint64_t &offset, vector<uint8_t> &data)
{
    auto it = chunks.begin();
    if (it == chunks.end() || it->first != read_position)
    {
        return false; // the next byte is still missing
    }

    offset = it->first;
    data = it->second;

    read_position += data.size();
    buffered_bytes -= data.size();
    chunks.erase(it);
    return true;
}

uint64_t StreamFrameSorter::get_read_position() const
{
    return read_position;
}

size_t StreamFrameSorter::get_num_chunks() const
{
    return chunks.size();
}

uint64_t StreamFrameSorter::get_buffered_bytes() const
{
    return buffered_bytes;
}
