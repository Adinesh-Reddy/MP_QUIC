#include<bits/stdc++.h>

#include "sockets/socket_manager.hpp"
#include "connection/server.hpp"
#include "app_common.hpp"

using namespace std;

/*
 mpq_server: serves files and test replies over MP-QUIC.
   mpq_server --listen 10.0.0.2:4433 --listen 10.0.1.2:4433 --dir files
 Requests (one per stream, ending with FIN):
   "GET <name>\n"            -> "OK <size>\n" + file data, or "ERR <reason>\n"
   "REQ <id> <reply size>\n" -> <reply size> bytes
 Files are read in FILE_CHUNK_SIZE chunks so memory stays small and ACKs never wait for disk.
*/

const size_t FILE_CHUNK_SIZE=64*1024;          // read chunk size
const size_t SEND_BUFFER_TARGET=2*1024*1024; // keep stream buffered up to 2 MB

static SocketManager *running_manager=nullptr;

static void stop_on_signal(int)
{
    if(running_manager!=nullptr){
        running_manager->stop();
    }
}

// Active file transfer on a stream
struct FileTransfer
{
    string name;
    unique_ptr<ifstream> file;
    uint64_t bytes_left=0; // file bytes not yet written to stream
};

// Per-connection server state
struct ConnectionState
{
    Connection *connection=nullptr;
    map<uint32_t, string> requests;        // stream -> partial request text
    map<uint32_t, FileTransfer> transfers; // stream -> active file transfer
    uint64_t start_us=0;
};

static void usage()
{
    cout<<"Usage: mpq_server --listen IP:PORT [--listen IP:PORT ...] [options]\n"
         <<"  --dir DIR            directory to serve files from (default: .)\n"
         <<"  --once               exit after first connection closes\n"
         <<"  --stats FILE         write path stats CSV every 100 ms\n"
         <<"  --idle-timeout SEC   idle timeout in seconds (default: 30)\n";
}

// Handle one completed request ("OK <size>" written here; file bytes sent by send_file_pieces)
static void answer_request(ConnectionState &state, uint32_t stream_id, const string &request, const string &dir)
{
    Connection *connection=state.connection;
    string line=request.substr(0, request.find('\n'));
    istringstream words(line);
    string command;
    words>>command;

    if(command=="GET"){
        string name;
        words>>name;

        // Only allow plain filenames inside dir
        string path=dir+"/"+name;
        error_code error;
        bool is_file=filesystem::is_regular_file(path, error);
        uint64_t size=is_file ? filesystem::file_size(path, error) : 0;
        unique_ptr<ifstream> file;
        if(is_file&&!error){
            file.reset(new ifstream(path, ios::binary));
        }

        if(name.empty()||name.find('/')!=string::npos||name.find("..")!=string::npos){
            string reply="ERR bad file name\n";
            connection->write(stream_id, reinterpret_cast<const uint8_t *>(reply.data()), reply.size());
            cout<<"[SERVER] GET "<<name<<": bad file name\n";
        }
        else if(file==nullptr||!*file){
            string reply="ERR file not found\n";
            connection->write(stream_id, reinterpret_cast<const uint8_t *>(reply.data()), reply.size());
            cout<<"[SERVER] GET "<<name<<": file not found in "<<dir<<"\n";
        }
        else{
            string header="OK "+to_string(size)+"\n";
            connection->write(stream_id, reinterpret_cast<const uint8_t *>(header.data()), header.size());
            cout<<"[SERVER] GET "<<name<<": sending "<<size<<" bytes\n";

            if(size>0){
                // Keep stream open until send_file_pieces finishes
                FileTransfer &transfer=state.transfers[stream_id];
                transfer.name=name;
                transfer.file=move(file);
                transfer.bytes_left=size;
                return;
            }
        }
    }
    else if(command=="REQ"){
        long id=0, reply_size=0;
        words>>id>>reply_size;
        reply_size=clamp(reply_size, 1L, 1024L*1024L); // 1 B to 1 MB

        vector<uint8_t> reply(static_cast<size_t>(reply_size), 'r');
        connection->write(stream_id, reply.data(), reply.size());
    }
    else{
        string reply="ERR unknown request\n";
        connection->write(stream_id, reinterpret_cast<const uint8_t *>(reply.data()), reply.size());
    }

    connection->close_stream(stream_id);
}

// Feed next file chunks while buffered stream bytes < SEND_BUFFER_TARGET
static void send_file_pieces(ConnectionState &state)
{
    Connection *connection=state.connection;
    static thread_local vector<char> chunk(FILE_CHUNK_SIZE);

    for(auto it=state.transfers.begin(); it!=state.transfers.end();){
        uint32_t stream_id=it->first;
        FileTransfer &transfer=it->second;

        while(transfer.bytes_left>0&&connection->unsent_bytes(stream_id)<SEND_BUFFER_TARGET){
            size_t wanted=static_cast<size_t>(min<uint64_t>(FILE_CHUNK_SIZE, transfer.bytes_left));
            transfer.file->read(chunk.data(), static_cast<streamsize>(wanted));
            size_t got=static_cast<size_t>(transfer.file->gcount());
            if(got==0){
                // File shrunk during transfer; client will detect length mismatch
                cout<<"[ERROR] GET "<<transfer.name<<": file read cut short ("
                     <<transfer.bytes_left<<" bytes left)\n";
                transfer.bytes_left=0;
                break;
            }
            connection->write(stream_id, reinterpret_cast<const uint8_t *>(chunk.data()), got);
            transfer.bytes_left-=got;
        }

        if(transfer.bytes_left==0){
            // All bytes written; send FIN
            connection->close_stream(stream_id);
            it=state.transfers.erase(it);
        }
        else{
            ++it;
        }
    }
}

static int run(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IOLBF, 0); // flush each log line immediately

    Arguments args(argc, argv, {"once", "help"});
    if(!args.ok()||args.has("help")||!args.has("listen")){
        if(!args.ok())
            cout<<"[ERROR] "<<args.error()<<"\n";
        usage();
        return 2;
    }

    string dir=args.get("dir", ".");
    bool once=args.has("once");

    Server server;
    server.set_idle_timeout(static_cast<uint64_t>(args.get_number("idle-timeout", 30))*1000000ULL);

    for(const string &text : args.get_all("listen")){
        NetworkAddress address;
        if(!parse_address(text, address)){
            cout<<"[ERROR] Invalid --listen \""<<text<<"\" (expected IP:PORT)\n";
            return 2;
        }
        if(!server.listen(address.ip, address.port)){
            cout<<"[ERROR] Cannot listen on "<<text<<"\n";
            return 1;
        }
    }

    StatsWriter stats;
    if(args.has("stats")&&!stats.open(args.get("stats"))){
        cout<<"[ERROR] Cannot open stats file "<<args.get("stats")<<"\n";
        return 1;
    }

    SocketManager manager;
    server.register_sockets(manager);
    running_manager=&manager;
    signal(SIGINT, stop_on_signal);
    signal(SIGTERM, stop_on_signal);

    map<uint64_t, shared_ptr<ConnectionState>> states;
    Connection *sampled_connection=nullptr; // connection tracked in stats CSV

    server.on_new_connection([&](Connection *connection) {
        shared_ptr<ConnectionState> state=make_shared<ConnectionState>();
        state->connection=connection;
        state->start_us=Packet::current_time_us();
        states[connection->get_connection_id()]=state;
        sampled_connection=connection;

        // Run request once client closes stream (FIN)
        connection->on_stream_data([state, dir](uint32_t stream_id, uint64_t /*offset*/,
                                                const vector<uint8_t> &data, bool fin) {
            string &request=state->requests[stream_id];
            if(request.size()+data.size()<=4096){ // max request line size
                request.append(data.begin(), data.end());
            }
            if(fin){
                answer_request(*state, stream_id, request, dir);
                state->requests.erase(stream_id);
            }
        });
    });

    server.on_connection_closed([&](Connection *connection) {
        uint64_t id=connection->get_connection_id();
        uint64_t duration_ms=0;
        if(states.count(id)){
            duration_ms=(Packet::current_time_us()-states[id]->start_us)/1000;
        }
        uint64_t bytes_sent=connection->get_flow_control_manager()->get_flow_controller(0)->get_bytes_sent();

        cout<<"CONNECTION id="<<id<<" duration_ms="<<duration_ms<<" bytes_sent="<<bytes_sent
             <<" duplicated_packets="<<connection->get_duplicated_packets()
             <<" reason=\""<<connection->get_close_reason()<<"\"\n";
        print_path_statistics(*connection);

        if(sampled_connection==connection){
            stats.sample(*connection, bytes_sent);
            sampled_connection=nullptr;
        }
        states.erase(id);

        if(once){
            manager.stop();
        }
    });

    manager.on_receive([&](int fd, const uint8_t *data, size_t len, string ip, uint16_t port) {
        server.handle_datagram(fd, data, len, ip, port);
    });

    manager.on_tick([&]() {
        if(sampled_connection!=nullptr&&stats.is_open()){
            uint64_t bytes_sent=
                sampled_connection->get_flow_control_manager()->get_flow_controller(0)->get_bytes_sent();
            stats.maybe_sample(*sampled_connection, bytes_sent);
        }

        // Feed file chunks before server.on_tick() sends packets
        for(auto &[id, state] : states){
            if(!state->transfers.empty()&&!state->connection->is_closed()){
                send_file_pieces(*state);
            }
        }

        server.on_tick(); // may erase closed connections
    });

    cout<<"[SERVER] Serving files from "<<dir<<"\n";
    manager.run(1); // 1 ms tick for protocol timers

    return 0;
}

// Catch internal exceptions and print a clean message
int main(int argc, char **argv)
{
    try{
        return run(argc, argv);
    }
    catch(const exception &e){
        cout<<"[ERROR] mpq_server internal error: "<<e.what()<<"\n";
        return 3;
    }
}
