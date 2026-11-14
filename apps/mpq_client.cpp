#include <bits/stdc++.h>

#include "sockets/socket_manager.hpp"
#include "connection/client.hpp"
#include "app_common.hpp"
#include "sha256.hpp"

using namespace std;

// mpq_client: downloads one file from an mpq_server over one or two paths.
//
//   mpq_client --server 10.0.0.2:4433 --local 10.0.0.1 --local 10.0.1.1 --get big.bin --out big.bin
//
// The first --local address carries the initial path. Each further --local address gives one
// more path, opened once the server has answered. With a single --local the transfer is
// single-path (the baseline of the evaluation).
//
// The last line printed is a summary that scripts can read:
//   RESULT status=ok file=big.bin bytes=20971520 time_ms=2417 goodput_kbps=69413 sha256=... paths=2

static void usage()
{
    cout << "usage: mpq_client --server IP:PORT --local IP [--local IP] --get NAME --out FILE [options]\n"
         << "  --stats FILE     write the state of every path to FILE (CSV) every 100 ms\n"
         << "  --timeout SEC    give up after SEC seconds (default: 600)\n";
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IOLBF, 0);

    Arguments args(argc, argv, {"help"});
    if (!args.ok() || args.has("help") || !args.has("server") || !args.has("local") ||
        !args.has("get") || !args.has("out"))
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

    string name = args.get("get");
    string out_path = args.get("out");
    uint64_t timeout_us = static_cast<uint64_t>(args.get_number("timeout", 600)) * 1000000ULL;

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

    ofstream out(out_path, ios::binary | ios::trunc);
    if (!out)
    {
        cout << "error: cannot create the file " << out_path
             << " given with --out (does the folder exist, and may you write there?)\n";
        return 1;
    }

    StatsWriter stats;
    if (args.has("stats") && !stats.open(args.get("stats")))
    {
        cout << "error: cannot create the file " << args.get("stats")
             << " given with --stats (does the folder exist, and may you write there?)\n";
        return 1;
    }

    Connection *connection = client.get_connection();
    const uint32_t STREAM_ID = 1;

    // State of the download
    string header;              // first line of the answer: "OK <size>" or "ERR <reason>"
    bool header_done = false;
    uint64_t expected_bytes = 0;
    uint64_t received_bytes = 0;
    bool finished = false;      // the end of the stream (FIN) arrived
    string error;
    uint64_t finish_us = 0;
    Sha256 sha;

    connection->on_stream_data([&](uint32_t stream_id, uint64_t /*offset*/, const vector<uint8_t> &data, bool fin) {
        if (stream_id != STREAM_ID)
        {
            return;
        }

        size_t body_start = 0;
        if (!header_done)
        {
            // The answer starts with one line of text, the file follows
            while (body_start < data.size() && !header_done)
            {
                char c = static_cast<char>(data[body_start++]);
                if (c == '\n')
                    header_done = true;
                else
                    header.push_back(c);
            }
            if (header_done)
            {
                if (header.substr(0, 3) == "OK ")
                    expected_bytes = strtoull(header.c_str() + 3, nullptr, 10);
                else
                    error = "the server refused the request: " +
                            (header.substr(0, 4) == "ERR " ? header.substr(4) : header);
            }
        }

        if (header_done && error.empty() && body_start < data.size())
        {
            const uint8_t *body = data.data() + body_start;
            size_t body_length = data.size() - body_start;
            out.write(reinterpret_cast<const char *>(body), static_cast<streamsize>(body_length));
            sha.update(body, body_length);
            received_bytes += body_length;
        }

        if (fin)
        {
            finished = true;
            finish_us = Packet::current_time_us();
        }
    });

    SocketManager manager;
    client.register_sockets(manager);

    manager.on_receive([&](int fd, const uint8_t *data, size_t len, std::string ip, uint16_t port) {
        client.handle_datagram(fd, data, len, ip, port);
    });

    uint64_t start_us = 0;

    manager.on_tick([&]() {
        client.on_tick();
        stats.maybe_sample(*connection, received_bytes);

        if (finished)
        {
            // Everything is here: the last ACKs leave, the server is told, and we stop
            connection->close("transfer complete");
            manager.stop();
        }
        else if (connection->is_closed())
        {
            error = "the connection was closed before the end: " + connection->get_close_reason();
            manager.stop();
        }
        else if (Packet::current_time_us() - start_us > timeout_us)
        {
            error = "the file was not complete after " + to_string(timeout_us / 1000000) +
                    " seconds (--timeout), so the client gave up";
            connection->close("the client gave up waiting (timeout)");
            manager.stop();
        }
    });

    // The request. The time of the transfer starts with the first packet of the connection
    // and ends when the last byte of the file is received (as in the paper).
    string request = "GET " + name + "\n";
    start_us = Packet::current_time_us();
    connection->write(STREAM_ID, reinterpret_cast<const uint8_t *>(request.data()), request.size());
    connection->close_stream(STREAM_ID);
    connection->send_pending();

    manager.run(1);
    out.close();
    stats.sample(*connection, received_bytes);

    if (error.empty() && !finished)
    {
        error = "the transfer stopped before the whole file arrived";
    }
    if (error.empty() && received_bytes != expected_bytes)
    {
        error = "received " + to_string(received_bytes) + " bytes, but the server said the file has " +
                to_string(expected_bytes) + " bytes";
    }

    print_path_statistics(*connection);

    if (!error.empty())
    {
        cout << "RESULT status=failed file=" << name << " bytes=" << received_bytes << " error=\"" << error << "\"\n";
        return 1;
    }

    uint64_t time_ms = max<uint64_t>(1, (finish_us - start_us) / 1000);
    uint64_t goodput_kbps = received_bytes * 8 / time_ms; // kilobits per second
    cout << "RESULT status=ok file=" << name << " bytes=" << received_bytes << " time_ms=" << time_ms
         << " goodput_kbps=" << goodput_kbps << " sha256=" << sha.hex_digest()
         << " paths=" << connection->get_paths().size() << "\n";
    return 0;
}
