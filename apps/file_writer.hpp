#ifndef FILE_WRITER_HPP
#define FILE_WRITER_HPP

#include<bits/stdc++.h>

using namespace std;

/*
 Writes downloaded bytes to disk on a separate thread.
 Keeps disk writes from blocking the single-threaded network loop.
 Only call write(), failed(), and finish() from the network thread.
*/
class FileWriter
{
public:
    // Size of each write block passed to disk thread (256 kB)
    static constexpr size_t BLOCK_SIZE=256*1024;
    // Max bytes buffered in memory before write() waits for disk (64 MB)
    static constexpr size_t MAX_QUEUED_BYTES=64*1024*1024;

    FileWriter()=default;
    FileWriter(const FileWriter &)=delete;
    FileWriter &operator=(const FileWriter &)=delete;
    ~FileWriter();

    // Open/truncate file and start disk thread
    bool open(const string &path);

    // Queue bytes for disk thread (drops data after an error)
    void write(const uint8_t *data, size_t length);

    // True if a disk write failed
    bool failed() const;

    // Wait for pending writes, close file, and stop thread; "" on success or error text
    string finish();

private:
    void disk_loop();

    string path;
    ofstream file;
    thread disk_thread;

    mutable mutex lock;
    condition_variable work_ready;  // woken when a full block or close is ready
    condition_variable room_freed;  // woken when disk thread finishes a block
    deque<vector<uint8_t>> blocks;
    size_t queued_bytes=0;
    bool closing=false;
    bool finished=false;

    atomic<bool> write_error{false};
    string error_text; // written before write_error=true, read after
};

#endif
