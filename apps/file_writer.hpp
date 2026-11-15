#ifndef FILE_WRITER_HPP
#define FILE_WRITER_HPP

#include <bits/stdc++.h>

using namespace std;

// Writes a downloaded file from a thread of its own.
//
// Why: the network loop is a single thread. If it wrote to the disk itself, every time the
// disk falls behind (Linux makes write() wait once too much unsaved data has piled up in
// memory, which happens after a few hundred MB to a few GB) the loop would stop reading
// packets and sending ACKs, and the sender would conclude that the path is broken.
// With this class the network thread only copies the bytes into memory; this thread
// saves them to the disk at the disk's own pace.
//
// Only the network thread calls write(), failed() and finish().
class FileWriter
{
public:
    // Data is handed to the disk thread in blocks of this size
    static constexpr size_t BLOCK_SIZE = 256 * 1024;
    // At most this much data waits in memory for the disk (about 10 s of a 50 Mbit/s download).
    // Only if the disk falls further behind than this does write() wait for it.
    static constexpr size_t MAX_QUEUED_BYTES = 64 * 1024 * 1024;

    FileWriter() = default;
    FileWriter(const FileWriter &) = delete;
    FileWriter &operator=(const FileWriter &) = delete;
    ~FileWriter();

    // Creates (or empties) the file and starts the disk thread. False if the file cannot be created.
    bool open(const string &path);

    // Queues bytes for the file. Returns at once unless MAX_QUEUED_BYTES are already waiting.
    // After a write error the bytes are dropped.
    void write(const uint8_t *data, size_t length);

    // True once a write to the disk has failed (disk full, ...)
    bool failed() const;

    // Waits until everything is on the disk, closes the file and stops the thread.
    // Returns "" on success, otherwise a message saying what went wrong.
    string finish();

private:
    void disk_loop();

    string path;
    ofstream file;
    thread disk_thread;

    mutable mutex lock;
    condition_variable work_ready;  // signalled when a full block (or the end) is there
    condition_variable room_freed;  // signalled when the disk thread has written a block
    deque<vector<uint8_t>> blocks;
    size_t queued_bytes = 0;
    bool closing = false;
    bool finished = false;

    atomic<bool> write_error{false};
    string error_text; // set by the disk thread before write_error, read after it
};

#endif
