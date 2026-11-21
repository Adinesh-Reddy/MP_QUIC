#include<bits/stdc++.h>

#include "file_writer.hpp"

using namespace std;

FileWriter::~FileWriter()
{
    finish();
}

bool FileWriter::open(const string &file_path)
{
    path=file_path;
    file.open(path, ios::binary|ios::trunc);
    if(!file){
        return false;
    }
    disk_thread=thread(&FileWriter::disk_loop, this);
    return true;
}

void FileWriter::write(const uint8_t *data, size_t length)
{
    if(length==0||write_error){
        return;
    }

    unique_lock<mutex> guard(lock);

    // Wait if disk queue is full (allow at least one block when queue is empty)
    room_freed.wait(guard, [&] {
        return queued_bytes==0||queued_bytes+length<=MAX_QUEUED_BYTES||write_error;
    });
    if(write_error){
        return;
    }

    // Batch small packets into BLOCK_SIZE chunks for fewer disk writes
    bool new_block=blocks.empty()||blocks.back().size()+length>BLOCK_SIZE;
    if(new_block){
        blocks.emplace_back();
        blocks.back().reserve(max(BLOCK_SIZE, length));
    }
    blocks.back().insert(blocks.back().end(), data, data+length);
    queued_bytes+=length;

    // Wake disk thread when a full block is ready
    if(new_block&&blocks.size()>1){
        work_ready.notify_one();
    }
}

bool FileWriter::failed() const
{
    return write_error;
}

void FileWriter::disk_loop()
{
    while(true){
        vector<uint8_t> block;
        {
            unique_lock<mutex> guard(lock);
            // Wait for a full block or finish()
            work_ready.wait(guard, [&] { return blocks.size()>1||closing; });
            if(blocks.empty()){
                return; // done
            }
            block=move(blocks.front());
            blocks.pop_front();
        }

        if(!write_error){
            file.write(reinterpret_cast<const char *>(block.data()), static_cast<streamsize>(block.size()));
            if(!file){
                error_text="failed to write to "+path;
                write_error=true;
            }
        }

        {
            lock_guard<mutex> guard(lock);
            queued_bytes-=block.size();
        }
        room_freed.notify_one();
    }
}

string FileWriter::finish()
{
    if(finished){
        return error_text;
    }
    finished=true;

    if(disk_thread.joinable()){
        {
            lock_guard<mutex> guard(lock);
            closing=true;
        }
        work_ready.notify_one();
        disk_thread.join();
    }

    if(file.is_open()){
        file.close();
        if(error_text.empty()&&!file){
            error_text="failed to close "+path;
        }
    }
    return error_text;
}
