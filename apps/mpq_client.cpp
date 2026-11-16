#include<bits/stdc++.h>

#include "sockets/socket_manager.hpp"
#include "connection/client.hpp"
#include "app_common.hpp"
#include "sha256.hpp"
#include "file_writer.hpp"

using namespace std;

/*
 mpq_client: downloads one file from mpq_server over one or more paths.
   mpq_client --server 10.0.0.2:4433 --local 10.0.0.1 --local 10.0.1.1 --get big.bin --out big.bin
 First --local is path 0; extra --local IPs open after ADD_ADDRESS.
 Prints a final RESULT line for scripts.
*/

static void usage()
{
    cout<<"Usage: mpq_client --server IP:PORT --local IP [--local IP] --get NAME --out FILE [options]\n"
         <<"  --stats FILE     write path stats CSV every 100 ms\n"
         <<"  --timeout SEC    timeout in seconds (default: 600)\n";
}

static int run(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IOLBF, 0);

    Arguments args(argc, argv, {"help"});
    if(!args.ok()||args.has("help")||!args.has("server")||!args.has("local")||
        !args.has("get")||!args.has("out")){
        if(!args.ok())
            cout<<"[ERROR] "<<args.error()<<"\n";
        usage();
        return 2;
    }

    NetworkAddress server_address;
    if(!parse_address(args.get("server"), server_address)){
        cout<<"[ERROR] Invalid --server \""<<args.get("server")<<"\" (expected IP:PORT)\n";
        return 2;
    }

    string name=args.get("get");
    string out_path=args.get("out");
    uint64_t timeout_us=static_cast<uint64_t>(args.get_number("timeout", 600))*1000000ULL;

    Client client;
    for(const string &ip : args.get_all("local")){
        if(!client.add_local_address(ip)){
            cout<<"[ERROR] Cannot use local IP "<<ip<<"\n";
            return 1;
        }
    }
    if(!client.connect(server_address.ip, server_address.port)){
        cout<<"[ERROR] Cannot open initial path to "<<args.get("server")<<"\n";
        return 1;
    }

    // Write file on a separate thread so slow disk never blocks network loop
    FileWriter out;
    if(!out.open(out_path)){
        cout<<"[ERROR] Cannot open output file "<<out_path<<"\n";
        return 1;
    }

    StatsWriter stats;
    if(args.has("stats")&&!stats.open(args.get("stats"))){
        cout<<"[ERROR] Cannot open stats file "<<args.get("stats")<<"\n";
        return 1;
    }

    Connection *connection=client.get_connection();
    const uint32_t STREAM_ID=1;

    // Download state
    string header;              // first line: "OK <size>" or "ERR <reason>"
    bool header_done=false;
    uint64_t expected_bytes=0;
    uint64_t received_bytes=0;
    bool finished=false;      // FIN arrived
    bool write_failed=false;  // disk thread error
    string error;
    uint64_t finish_us=0;
    Sha256 sha;

    connection->on_stream_data([&](uint32_t stream_id, uint64_t /*offset*/, const vector<uint8_t> &data, bool fin) {
        if(stream_id!=STREAM_ID){
            return;
        }

        size_t body_start=0;
        if(!header_done){
            // First line is header; file bytes come right after
            while(body_start<data.size()&&!header_done){
                char c=static_cast<char>(data[body_start++]);
                if(c=='\n')
                    header_done=true;
                else
                    header.push_back(c);
            }
            if(header_done){
                if(header.substr(0, 3)=="OK ")
                    expected_bytes=strtoull(header.c_str()+3, nullptr, 10);
                else
                    error="server error: "+
                            (header.substr(0, 4)=="ERR " ? header.substr(4) : header);
            }
        }

        if(header_done&&error.empty()&&body_start<data.size()){
            const uint8_t *body=data.data()+body_start;
            size_t body_length=data.size()-body_start;
            out.write(body, body_length); // copy to memory queue (non-blocking)
            sha.update(body, body_length);
            received_bytes+=body_length;
        }

        if(fin){
            finished=true;
            finish_us=Packet::current_time_us();
        }
    });

    SocketManager manager;
    client.register_sockets(manager);

    manager.on_receive([&](int fd, const uint8_t *data, size_t len, string ip, uint16_t port) {
        client.handle_datagram(fd, data, len, ip, port);
    });

    uint64_t start_us=0;

    manager.on_tick([&]() {
        client.on_tick();
        stats.maybe_sample(*connection, received_bytes);

        if(out.failed()){
            // Stop download on disk write error
            write_failed=true;
            connection->close("disk write failed");
            manager.stop();
        }
        else if(finished){
            // Done: send close frame and stop
            connection->close("transfer complete");
            manager.stop();
        }
        else if(connection->is_closed()){
            error="connection closed early: "+connection->get_close_reason();
            manager.stop();
        }
        else if(Packet::current_time_us()-start_us>timeout_us){
            error="timed out after "+to_string(timeout_us/1000000)+" s";
            connection->close("timeout");
            manager.stop();
        }
    });

    // Send GET request and time from first packet to last byte
    string request="GET "+name+"\n";
    start_us=Packet::current_time_us();
    connection->write(STREAM_ID, reinterpret_cast<const uint8_t *>(request.data()), request.size());
    connection->close_stream(STREAM_ID);
    connection->send_pending();

    manager.run(1);

    // Wait for disk thread to finish writing
    string write_error=out.finish();
    if(!write_error.empty()&&(error.empty()||write_failed)){
        error=write_error;
    }
    stats.sample(*connection, received_bytes);

    if(error.empty()&&!finished){
        error="transfer stopped before end of file";
    }
    if(error.empty()&&received_bytes!=expected_bytes){
        error="got "+to_string(received_bytes)+" bytes, expected "+to_string(expected_bytes);
    }

    print_path_statistics(*connection);

    if(!error.empty()){
        cout<<"RESULT status=failed file="<<name<<" bytes="<<received_bytes<<" error=\""<<error<<"\"\n";
        return 1;
    }

    uint64_t time_ms=max<uint64_t>(1, (finish_us-start_us)/1000);
    uint64_t goodput_kbps=received_bytes*8/time_ms; // kbps
    cout<<"RESULT status=ok file="<<name<<" bytes="<<received_bytes<<" time_ms="<<time_ms
         <<" goodput_kbps="<<goodput_kbps<<" sha256="<<sha.hex_digest()
         <<" paths="<<connection->get_paths().size()<<"\n";
    return 0;
}

// Catch internal exceptions and print a clean message
int main(int argc, char **argv)
{
    try{
        return run(argc, argv);
    }
    catch(const exception &e){
        cout<<"[ERROR] mpq_client internal error: "<<e.what()<<"\n";
        return 3;
    }
}
