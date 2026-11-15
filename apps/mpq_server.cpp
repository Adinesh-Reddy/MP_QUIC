#include <bits/stdc++.h>
#include <csignal>

#include "sockets/socket_manager.hpp"
#include "connection/server.hpp"
#include "app_common.hpp"

using namespace std;

// mpq_server: serves files (and small test replies) to MP-QUIC Mini clients.
//
//   mpq_server --listen 10.0.0.2:4433 --listen 10.0.1.2:4433 --dir files
//
// A client sends one request per stream, a line of text, and closes its side of the stream:
//   "GET <name>\n"              -> "OK <size>\n" followed by the file, or "ERR <reason>\n"
//   "REQ <id> <reply size>\n"   -> <reply size> bytes (request/response test of mpq_reqres)
// The server answers on the same stream and ends it with a FIN.
//
// A file is never read whole. The answer to a GET only opens the file; the file is then
// read in pieces of FILE_CHUNK_SIZE bytes, from the tick of the event loop, whenever less
// than SEND_BUFFER_TARGET bytes of it wait to be sent. So a 1 GB file costs about 2 MB of
// memory, and no packet waits for the disk: ACKs keep leaving while the file is sent.
// This is back-pressure on a single thread: the file is only read as fast as packets leave.

const size_t FILE_CHUNK_SIZE = 64 * 1024;          // bytes read from the file at once
const size_t SEND_BUFFER_TARGET = 2 * 1024 * 1024; // refill the stream when less than this waits

static SocketManager *running_manager = nullptr;

static void stop_on_signal(int)
{
    if (running_manager != nullptr)
    {
        running_manager->stop();
    }
}

// A file being sent on one stream
struct FileTransfer
{
    string name;
    unique_ptr<ifstream> file;
    uint64_t bytes_left = 0; // bytes of the file not written on the stream yet
};

// What the server remembers about one connection
struct ConnectionState
{
    Connection *connection = nullptr;
    map<uint32_t, string> requests;        // stream -> text received so far
    map<uint32_t, FileTransfer> transfers; // stream -> file being sent on it
    uint64_t start_us = 0;
};

static void usage()
{
    cout << "usage: mpq_server --listen IP:PORT [--listen IP:PORT ...] [options]\n"
         << "  --dir DIR            folder of the files that can be requested (default: .)\n"
         << "  --once               stop after the first connection is over\n"
         << "  --stats FILE         write the state of every path to FILE (CSV) every 100 ms\n"
         << "  --idle-timeout SEC   close a connection after SEC seconds of silence (default: 30)\n";
}

// Sends the answer to one request on its stream. For a file only the "OK <size>" line is
// written here; the file itself follows piece by piece (send_file_pieces).
static void answer_request(ConnectionState &state, uint32_t stream_id, const string &request, const string &dir)
{
    Connection *connection = state.connection;
    string line = request.substr(0, request.find('\n'));
    istringstream words(line);
    string command;
    words >> command;

    if (command == "GET")
    {
        string name;
        words >> name;

        // Only a plain file name: nothing that could leave the folder
        string path = dir + "/" + name;
        error_code error;
        bool is_file = filesystem::is_regular_file(path, error);
        uint64_t size = is_file ? filesystem::file_size(path, error) : 0;
        unique_ptr<ifstream> file;
        if (is_file && !error)
        {
            file.reset(new ifstream(path, ios::binary));
        }

        if (name.empty() || name.find('/') != string::npos || name.find("..") != string::npos)
        {
            string reply = "ERR bad file name\n";
            connection->write(stream_id, reinterpret_cast<const uint8_t *>(reply.data()), reply.size());
            cout << "GET " << name << ": refused, a file name cannot be empty or contain '/' or '..'\n";
        }
        else if (file == nullptr || !*file)
        {
            string reply = "ERR file not found\n";
            connection->write(stream_id, reinterpret_cast<const uint8_t *>(reply.data()), reply.size());
            cout << "GET " << name << ": there is no such file in " << dir << ", answered \"file not found\"\n";
        }
        else
        {
            string header = "OK " + to_string(size) + "\n";
            connection->write(stream_id, reinterpret_cast<const uint8_t *>(header.data()), header.size());
            cout << "GET " << name << ": sending " << size << " bytes\n";

            if (size > 0)
            {
                // The stream stays open: send_file_pieces writes the file and then ends the stream
                FileTransfer &transfer = state.transfers[stream_id];
                transfer.name = name;
                transfer.file = move(file);
                transfer.bytes_left = size;
                return;
            }
        }
    }
    else if (command == "REQ")
    {
        long id = 0, reply_size = 0;
        words >> id >> reply_size;
        reply_size = clamp(reply_size, 1L, 1024L * 1024L); // 1 byte to 1 MB

        vector<uint8_t> reply(static_cast<size_t>(reply_size), 'r');
        connection->write(stream_id, reply.data(), reply.size());
    }
    else
    {
        string reply = "ERR unknown request\n";
        connection->write(stream_id, reinterpret_cast<const uint8_t *>(reply.data()), reply.size());
    }

    connection->close_stream(stream_id);
}

// Writes the next pieces of the files being sent on this connection, as long as the stream
// has less than SEND_BUFFER_TARGET bytes waiting. Called from the tick of the event loop, so
// the work done at once is small and incoming packets (ACKs) are never kept waiting.
static void send_file_pieces(ConnectionState &state)
{
    Connection *connection = state.connection;
    // Allocated once per thread and reused on every call (today the server has one thread)
    static thread_local vector<char> chunk(FILE_CHUNK_SIZE);

    for (auto it = state.transfers.begin(); it != state.transfers.end();)
    {
        uint32_t stream_id = it->first;
        FileTransfer &transfer = it->second;

        while (transfer.bytes_left > 0 && connection->unsent_bytes(stream_id) < SEND_BUFFER_TARGET)
        {
            size_t wanted = static_cast<size_t>(min<uint64_t>(FILE_CHUNK_SIZE, transfer.bytes_left));
            transfer.file->read(chunk.data(), static_cast<streamsize>(wanted));
            size_t got = static_cast<size_t>(transfer.file->gcount());
            if (got == 0)
            {
                // The file became shorter while it was being sent: the client will see
                // that fewer bytes arrived than the size announced in the "OK" line
                cout << "GET " << transfer.name << ": could not read the rest of the file ("
                     << transfer.bytes_left << " bytes missing), the answer is cut short\n";
                transfer.bytes_left = 0;
                break;
            }
            connection->write(stream_id, reinterpret_cast<const uint8_t *>(chunk.data()), got);
            transfer.bytes_left -= got;
        }

        if (transfer.bytes_left == 0)
        {
            // Everything is written: the FIN follows the last byte
            connection->close_stream(stream_id);
            it = state.transfers.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

static int run(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IOLBF, 0); // a line is written as soon as it is complete, also into a file

    Arguments args(argc, argv, {"once", "help"});
    if (!args.ok() || args.has("help") || !args.has("listen"))
    {
        if (!args.ok())
            cout << "error: " << args.error() << "\n";
        usage();
        return 2;
    }

    string dir = args.get("dir", ".");
    bool once = args.has("once");

    Server server;
    server.set_idle_timeout(static_cast<uint64_t>(args.get_number("idle-timeout", 30)) * 1000000ULL);

    for (const string &text : args.get_all("listen"))
    {
        NetworkAddress address;
        if (!parse_address(text, address))
        {
            cout << "error: --listen must be written as IP:PORT, for example --listen 10.0.2.2:4433 (you wrote \""
                 << text << "\")\n";
            return 2;
        }
        if (!server.listen(address.ip, address.port))
        {
            cout << "error: cannot listen on " << text << " (the reason is above)\n";
            return 1;
        }
    }

    StatsWriter stats;
    if (args.has("stats") && !stats.open(args.get("stats")))
    {
        cout << "error: cannot create the file " << args.get("stats")
             << " given with --stats (does the folder exist, and may you write there?)\n";
        return 1;
    }

    SocketManager manager;
    server.register_sockets(manager);
    running_manager = &manager;
    signal(SIGINT, stop_on_signal);
    signal(SIGTERM, stop_on_signal);

    map<uint64_t, shared_ptr<ConnectionState>> states;
    Connection *sampled_connection = nullptr; // the connection written to the statistics file

    server.on_new_connection([&](Connection *connection) {
        shared_ptr<ConnectionState> state = make_shared<ConnectionState>();
        state->connection = connection;
        state->start_us = Packet::current_time_us();
        states[connection->get_connection_id()] = state;
        sampled_connection = connection;

        // A request is complete when the client ends the stream
        connection->on_stream_data([state, dir](uint32_t stream_id, uint64_t /*offset*/,
                                                const vector<uint8_t> &data, bool fin) {
            string &request = state->requests[stream_id];
            if (request.size() + data.size() <= 4096) // a request is a short line of text
            {
                request.append(data.begin(), data.end());
            }
            if (fin)
            {
                answer_request(*state, stream_id, request, dir);
                state->requests.erase(stream_id);
            }
        });
    });

    server.on_connection_closed([&](Connection *connection) {
        uint64_t id = connection->get_connection_id();
        uint64_t duration_ms = 0;
        if (states.count(id))
        {
            duration_ms = (Packet::current_time_us() - states[id]->start_us) / 1000;
        }
        uint64_t bytes_sent = connection->get_flow_control_manager()->get_flow_controller(0)->get_bytes_sent();

        cout << "CONNECTION id=" << id << " duration_ms=" << duration_ms << " bytes_sent=" << bytes_sent
             << " duplicated_packets=" << connection->get_duplicated_packets()
             << " reason=\"" << connection->get_close_reason() << "\"\n";
        print_path_statistics(*connection);

        if (sampled_connection == connection)
        {
            stats.sample(*connection, bytes_sent);
            sampled_connection = nullptr;
        }
        states.erase(id);

        if (once)
        {
            manager.stop();
        }
    });

    manager.on_receive([&](int fd, const uint8_t *data, size_t len, std::string ip, uint16_t port) {
        server.handle_datagram(fd, data, len, ip, port);
    });

    manager.on_tick([&]() {
        if (sampled_connection != nullptr && stats.is_open())
        {
            uint64_t bytes_sent =
                sampled_connection->get_flow_control_manager()->get_flow_controller(0)->get_bytes_sent();
            stats.maybe_sample(*sampled_connection, bytes_sent);
        }

        // The next pieces of the files being sent; server.on_tick() then puts them in packets
        for (auto &[id, state] : states)
        {
            if (!state->transfers.empty() && !state->connection->is_closed())
            {
                send_file_pieces(*state);
            }
        }

        server.on_tick(); // may remove a connection that just ended
    });

    cout << "mpq_server: serving the files of " << dir << "\n";
    manager.run(1); // 1 ms: the timers of the protocol need a finer clock than the 10 ms default

    return 0;
}

// An exception means a bug in the program (for example a packet built larger than the
// limit). Print it in plain words and exit, instead of an unexplained "terminate called".
int main(int argc, char **argv)
{
    try
    {
        return run(argc, argv);
    }
    catch (const exception &e)
    {
        cout << "mpq_server stopped because of an internal error: " << e.what() << "\n"
             << "  This is a bug in the program, not a problem of the network or of your input.\n";
        return 3;
    }
}
