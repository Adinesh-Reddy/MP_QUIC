#include <bits/stdc++.h>

#include "file_writer.hpp"

using namespace std;

FileWriter::~FileWriter()
{
    finish();
}

bool FileWriter::open(const string &file_path)
{
    path = file_path;
    file.open(path, ios::binary | ios::trunc);
    if (!file)
    {
        return false;
    }
    disk_thread = thread(&FileWriter::disk_loop, this);
    return true;
}

void FileWriter::write(const uint8_t *data, size_t length)
{
    if (length == 0 || write_error)
    {
        return;
    }

    unique_lock<mutex> guard(lock);

    // The disk is very far behind: wait for room (normally never happens).
    // An empty queue always takes the data, so even a huge single write cannot wait forever.
    room_freed.wait(guard, [&] {
        return queued_bytes == 0 || queued_bytes + length <= MAX_QUEUED_BYTES || write_error;
    });
    if (write_error)
    {
        return;
    }

    // Small pieces (one packet each) are gathered into large blocks: fewer, bigger disk writes
    bool new_block = blocks.empty() || blocks.back().size() + length > BLOCK_SIZE;
    if (new_block)
    {
        blocks.emplace_back();
        blocks.back().reserve(max(BLOCK_SIZE, length));
    }
    blocks.back().insert(blocks.back().end(), data, data + length);
    queued_bytes += length;

    // Wake the disk thread only when a block is complete (it waits for full blocks)
    if (new_block && blocks.size() > 1)
    {
        work_ready.notify_one();
    }
}

bool FileWriter::failed() const
{
    return write_error;
}

void FileWriter::disk_loop()
{
    while (true)
    {
        vector<uint8_t> block;
        {
            unique_lock<mutex> guard(lock);
            // Write only complete blocks, except at the end where the last partial one goes too
            work_ready.wait(guard, [&] { return blocks.size() > 1 || closing; });
            if (blocks.empty())
            {
                return; // closing and everything written
            }
            block = move(blocks.front());
            blocks.pop_front();
        }

        if (!write_error)
        {
            file.write(reinterpret_cast<const char *>(block.data()), static_cast<streamsize>(block.size()));
            if (!file)
            {
                error_text = "could not write to " + path + " (is the disk full?)";
                write_error = true; // after error_text: whoever sees the flag can read the text
            }
        }

        {
            lock_guard<mutex> guard(lock);
            queued_bytes -= block.size();
        }
        room_freed.notify_one();
    }
}

string FileWriter::finish()
{
    if (finished)
    {
        return error_text;
    }
    finished = true;

    if (disk_thread.joinable())
    {
        {
            lock_guard<mutex> guard(lock);
            closing = true;
        }
        work_ready.notify_one();
        disk_thread.join();
    }

    // close() writes what the stream still buffers: a full disk can show up only here
    if (file.is_open())
    {
        file.close();
        if (error_text.empty() && !file)
        {
            error_text = "could not finish writing " + path + " (is the disk full?)";
        }
    }
    return error_text;
}
