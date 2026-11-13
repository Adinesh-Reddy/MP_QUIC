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

static SocketManager *running_manager = nullptr;

static void stop_on_signal(int)
{
    if (running_manager != nullptr)
    {
        running_manager->stop();
    }
}

// What the server remembers about one connection
struct ConnectionState
{
    map<uint32_t, string> requests; // stream -> text received so far
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

// Sends the answer to one request on its stream
static void answer_request(Connection *connection, uint32_t stream_id, const string &request, const string &dir)
{
    string line = request.substr(0, request.find('\n'));
    istringstream words(line);
    string command;
    words >> command;

    if (command == "GET")
    {
        string name;
        words >> name;

        // Only a plain file name: nothing that could leave the folder
        vector<uint8_t> content;
        if (name.empty() || name.find('/') != string::npos || name.find("..") != string::npos)
        {
            string reply = "ERR bad file name\n";
            connection->write(stream_id, reinterpret_cast<const uint8_t *>(reply.data()), reply.size());
        }
        else if (!read_file(dir + "/" + name, content))
        {
            string reply = "ERR file not found\n";
            connection->write(stream_id, reinterpret_cast<const uint8_t *>(reply.data()), reply.size());
            cout << "GET " << name << ": not found\n";
        }
        else
        {
            string header = "OK " + to_string(content.size()) + "\n";
            connection->write(stream_id, reinterpret_cast<const uint8_t *>(header.data()), header.size());
            connection->write(stream_id, content.data(), content.size());
            cout << "GET " << name << ": sending " << content.size() << " bytes\n";
        }
    }
    else if (command == "REQ")
    {
        long id = 0, reply_size = 0;
        words >> id >> reply_size;
        reply_size = max(1L, min(reply_size, 1024L * 1024L));

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

int main(int argc, char **argv)
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
        if (!parse_address(text, address) || !server.listen(address.ip, address.port))
        {
            cout << "error: cannot listen on " << text << "\n";
            return 1;
        }
    }

    StatsWriter stats;
    if (args.has("stats") && !stats.open(args.get("stats")))
    {
        cout << "error: cannot write " << args.get("stats") << "\n";
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
        state->start_us = Packet::current_time_us();
        states[connection->get_connection_id()] = state;
        sampled_connection = connection;

        // A request is complete when the client ends the stream
        connection->on_stream_data([connection, state, dir](uint32_t stream_id, uint64_t /*offset*/,
                                                            const vector<uint8_t> &data, bool fin) {
            string &request = state->requests[stream_id];
            if (request.size() + data.size() <= 4096) // a request is a short line of text
            {
                request.append(data.begin(), data.end());
            }
            if (fin)
            {
                answer_request(connection, stream_id, request, dir);
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
        server.on_tick(); // may remove a connection that just ended
    });

    cout << "mpq_server: serving the files of " << dir << "\n";
    manager.run(1); // 1 ms: the timers of the protocol need a finer clock than the 10 ms default

    return 0;
}
