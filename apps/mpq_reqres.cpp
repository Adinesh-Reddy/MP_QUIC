#include <bits/stdc++.h>

#include "sockets/socket_manager.hpp"
#include "connection/client.hpp"
#include "app_common.hpp"

using namespace std;

// mpq_reqres: request/response traffic for the network handover experiment. The client
// sends a small request at regular intervals; the server
// (mpq_server) answers each one at once. The program measures, for every request, the
// time until the whole answer is back.
//
//   mpq_reqres --server 10.0.0.2:4433 --local 10.0.0.1 --local 10.0.1.1 --out delays.csv
//
// Defaults: 750-byte requests, one every 400 ms.
// Every request uses its own stream (1, 3, 5, ...).

static void usage()
{
    cout << "usage: mpq_reqres --server IP:PORT --local IP [--local IP] [options]\n"
         << "  --interval-ms N   time between two requests (default: 400)\n"
         << "  --size N          size of a request and of its answer in bytes (default: 750)\n"
         << "  --count N         number of requests (default: 35, about 14 seconds)\n"
         << "  --out FILE        write request,sent_ms,delay_ms to FILE (CSV)\n";
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IOLBF, 0);

    Arguments args(argc, argv, {"help"});
    if (!args.ok() || args.has("help") || !args.has("server") || !args.has("local"))
    {
        if (!args.ok())
            cout << "error: " << args.error() << "\n";
        usage();
        return 2;
    }

    NetworkAddress server_address;
    if (!parse_address(args.get("server"), server_address))
    {
        cout << "error: --server must be written as IP:PORT, for example --server 10.0.2.2:4433 (you wrote \""
             << args.get("server") << "\")\n";
        return 2;
    }

    uint64_t interval_us = static_cast<uint64_t>(max(1L, args.get_number("interval-ms", 400))) * 1000ULL;
    size_t size = static_cast<size_t>(max(16L, args.get_number("size", 750)));
    size_t count = static_cast<size_t>(max(1L, args.get_number("count", 35)));

    Client client;
    for (const string &ip : args.get_all("local"))
    {
        if (!client.add_local_address(ip))
        {
            cout << "error: cannot use the local address " << ip << " given with --local (the reason is above)\n";
            return 1;
        }
    }
    if (!client.connect(server_address.ip, server_address.port))
    {
        cout << "error: could not create the first path to the server " << args.get("server") << "\n";
        return 1;
    }

    Connection *connection = client.get_connection();

    vector<uint64_t> sent_us(count, 0);
    vector<int64_t> delay_us(count, -1); // -1 = no answer
    vector<size_t> answer_bytes(count, 0);
    size_t next_request = 0;
    size_t answered = 0;

    // Request number k travels on stream 2k + 1
    connection->on_stream_data([&](uint32_t stream_id, uint64_t /*offset*/, const vector<uint8_t> &data, bool fin) {
        size_t k = (stream_id - 1) / 2;
        if (stream_id % 2 == 0 || k >= count)
        {
            return;
        }
        answer_bytes[k] += data.size();
        if (fin && delay_us[k] < 0)
        {
            delay_us[k] = static_cast<int64_t>(Packet::current_time_us() - sent_us[k]);
            answered++;
            cout << "REQUEST id=" << k << " sent_ms=" << (sent_us[k] - sent_us[0]) / 1000
                 << " delay_ms=" << delay_us[k] / 1000.0 << "\n";
        }
    });

    SocketManager manager;
    client.register_sockets(manager);

    manager.on_receive([&](int fd, const uint8_t *data, size_t len, std::string ip, uint16_t port) {
        client.handle_datagram(fd, data, len, ip, port);
    });

    uint64_t start_us = Packet::current_time_us();
    string error;

    manager.on_tick([&]() {
        uint64_t now_us = Packet::current_time_us();

        // Time for the next request?
        if (next_request < count && now_us >= start_us + next_request * interval_us)
        {
            size_t k = next_request++;
            uint32_t stream_id = static_cast<uint32_t>(2 * k + 1);

            // "REQ <id> <reply size>\n" filled up to the size of a request
            string request = "REQ " + to_string(k) + " " + to_string(size) + "\n";
            if (request.size() < size)
            {
                request.append(size - request.size(), 'q');
            }

            sent_us[k] = Packet::current_time_us();
            connection->write(stream_id, reinterpret_cast<const uint8_t *>(request.data()), request.size());
            connection->close_stream(stream_id);
        }

        client.on_tick();

        if (answered == count)
        {
            connection->close("all requests answered");
            manager.stop();
        }
        else if (connection->is_closed())
        {
            error = "the connection was closed before the end: " + connection->get_close_reason();
            manager.stop();
        }
        else if (next_request == count && now_us > sent_us[count - 1] + 10ULL * 1000000ULL)
        {
            // 10 seconds after the last request: the missing answers will not come
            connection->close("the client stopped waiting for the missing answers (timeout)");
            manager.stop();
        }
    });

    manager.run(1);

    if (error.empty() && answered < count)
    {
        error = to_string(count - answered) + " of " + to_string(count) +
                " requests got no answer within 10 seconds after the last request";
    }

    if (args.has("out"))
    {
        ofstream out(args.get("out"));
        out << "request,sent_ms,delay_ms\n";
        for (size_t k = 0; k < count; k++)
        {
            if (sent_us[k] == 0)
                continue;
            out << k << "," << (sent_us[k] - sent_us[0]) / 1000.0 << ",";
            if (delay_us[k] >= 0)
                out << delay_us[k] / 1000.0;
            else
                out << -1;
            out << "\n";
        }
    }

    print_path_statistics(*connection);

    // Summary: how many answers, and the largest delay (the handover shows up there)
    int64_t max_delay = -1;
    for (size_t k = 0; k < count; k++)
    {
        max_delay = max(max_delay, delay_us[k]);
    }
    cout << "RESULT status=" << ((answered == count && error.empty()) ? "ok" : "failed")
         << " requests=" << count << " answered=" << answered
         << " max_delay_ms=" << max_delay / 1000.0;
    if (!error.empty())
    {
        cout << " error=\"" << error << "\"";
    }
    cout << "\n";

    return (answered == count && error.empty()) ? 0 : 1;
}
