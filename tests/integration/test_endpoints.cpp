#include <iostream>
#include <cassert>
#include <vector>
#include <map>
#include <string>
#include <functional>

#include "sockets/udp_socket.hpp"
#include "sockets/socket_manager.hpp"
#include "packet/packet.hpp"
#include "path/path_manager.hpp"
#include "connection/client.hpp"
#include "connection/server.hpp"

// Tests of Phase 11: a Client and a Server endpoint talk over loopback, inside one process
// and one event loop. Nothing is configured by hand on the server: the connection and its
// paths are created from the packets it receives.
//
// Loopback accepts every address of 127.0.0.0/8, so "two interfaces" are simply two
// addresses: 127.0.1.1 and 127.0.1.2 for the client, 127.0.0.1 and 127.0.0.2 for the server.

// Runs the event loop until done() is true (or max_ms passed). Returns true if done() was reached.
static bool run_until(SocketManager &manager, Server &server, Client &client,
                      const std::function<bool()> &done, uint64_t max_ms,
                      bool client_alive = true)
{
    uint64_t start_us = Packet::current_time_us();
    bool reached = false;

    manager.on_receive([&](int fd, const uint8_t *data, size_t len, std::string ip, uint16_t port) {
        if (server.owns_socket(fd))
        {
            server.handle_datagram(fd, data, len, ip, port);
        }
        else if (client_alive && client.owns_socket(fd))
        {
            client.handle_datagram(fd, data, len, ip, port);
        }
    });

    manager.on_tick([&]() {
        if (client_alive)
        {
            client.on_tick();
        }
        server.on_tick();

        if (done())
        {
            reached = true;
            manager.stop();
        }
        else if (Packet::current_time_us() - start_us > max_ms * 1000)
        {
            manager.stop();
        }
    });

    manager.run(1);
    return reached;
}

static std::vector<uint8_t> pattern(size_t size)
{
    std::vector<uint8_t> data(size);
    for (size_t i = 0; i < size; i++)
        data[i] = static_cast<uint8_t>((i * 131 + 17) % 256);
    return data;
}

// What the server application does in these tests: when a request is complete on a stream,
// it answers on the same stream with reply_size bytes and ends the stream.
static void serve_replies(Server &server, size_t reply_size)
{
    server.on_new_connection([reply_size](Connection *connection) {
        connection->on_stream_data([connection, reply_size](uint32_t stream_id, uint64_t, const std::vector<uint8_t> &, bool fin) {
            if (fin)
            {
                std::vector<uint8_t> reply = pattern(reply_size + stream_id); // a different size per stream
                connection->write(stream_id, reply.data(), reply.size());
                connection->close_stream(stream_id);
            }
        });
    });
}

void test_multihomed_server()
{
    std::cout << "\n[TEST] Server with two addresses: connection and paths are created from the packets...\n";

    Server server;
    assert(server.listen("127.0.0.1", 19311));
    assert(server.listen("127.0.0.2", 19311));
    assert(!server.listen("127.0.0.1", 19311)); // the address is already in use
    assert(server.get_listen_addresses().size() == 2);
    serve_replies(server, 1024 * 1024);

    // What the server knew about the connection when it ended
    size_t server_paths = 0;
    std::string server_path1_remote;
    bool server_closed_by_peer = false;
    std::string server_close_reason;
    server.on_connection_closed([&](Connection *connection) {
        server_paths = connection->get_paths().size();
        if (connection->get_path(1) != nullptr)
            server_path1_remote = connection->get_path(1)->get_remote_ip();
        server_closed_by_peer = connection->is_closed_by_peer();
        server_close_reason = connection->get_close_reason();
    });

    Client client;
    assert(client.add_local_address("127.0.1.1"));
    assert(client.add_local_address("127.0.1.2"));
    assert(client.connect("127.0.0.1", 19311));
    assert(!client.connect("127.0.0.1", 19311)); // the initial path exists already

    Connection *connection = client.get_connection();

    // Before the server answers there is only the initial path
    assert(connection->get_paths().size() == 1);
    assert(connection->get_path(0) != nullptr);

    std::vector<uint8_t> received;
    bool fin_received = false;
    connection->on_stream_data([&](uint32_t stream_id, uint64_t offset, const std::vector<uint8_t> &data, bool fin) {
        assert(stream_id == 1);
        assert(offset == received.size());
        received.insert(received.end(), data.begin(), data.end());
        if (fin)
            fin_received = true;
    });

    SocketManager manager;
    server.register_sockets(manager);
    client.register_sockets(manager);

    // The first packet of the client opens the connection: there is no handshake
    std::string request = "hello";
    connection->write(1, reinterpret_cast<const uint8_t *>(request.data()), request.size());
    connection->close_stream(1);
    connection->send_pending();

    assert(run_until(manager, server, client, [&]() { return fin_received; }, 20000));

    assert(received == pattern(1024 * 1024 + 1));
    assert(server.get_connections_accepted() == 1);
    assert(server.get_num_connections() == 1);

    // The server advertised its two addresses (ADD_ADDRESS): the client opened a second
    // path, with an odd Path ID, from its second interface to the second address
    assert(connection->get_paths().size() == 2);
    Path *path1 = connection->get_path(1);
    assert(path1 != nullptr);
    assert(path1->get_local_ip() == "127.0.1.2");
    assert(path1->get_remote_ip() == "127.0.0.2" && path1->get_remote_port() == 19311);
    assert(connection->get_path(0)->get_remote_ip() == "127.0.0.1");

    // The server used the new path for data, although nobody configured it there
    assert(path1->get_received_packet_handler()->get_packets_received() > 5);
    assert(path1->get_rtt_stats()->get_smoothed_rtt() > 0); // from the PING that announced the path
    std::cout << "[Client] path 0 received " << connection->get_path(0)->get_received_packet_handler()->get_packets_received()
              << " packets, path 1 received " << path1->get_received_packet_handler()->get_packets_received() << "\n";

    // The client is done: the server is told at once (no timeout) and forgets the connection
    connection->close("transfer complete");
    assert(connection->is_closed() && !connection->is_closed_by_peer());
    assert(run_until(manager, server, client, [&]() { return server.get_num_connections() == 0; }, 5000));

    assert(server_paths == 2);
    assert(server_path1_remote == "127.0.1.2");
    assert(server_closed_by_peer);
    assert(server_close_reason == "the other side closed the connection (its reason: transfer complete)");

    // A late packet of the closed connection must not open a new one
    Packet late(FLAG_NONE, connection->get_connection_id(), 0, 999999);
    late.add_frame(Frame::make_ping());
    std::vector<uint8_t> raw = late.serialize();
    UDPSocket stray;
    assert(stray.sendTo(raw.data(), raw.size(), "127.0.0.1", 19311) > 0);
    run_until(manager, server, client, []() { return false; }, 50);
    assert(server.get_connections_accepted() == 1);
    assert(server.get_num_connections() == 0);

    std::cout << "  -> PASSED\n";
}

void test_single_homed_server()
{
    std::cout << "\n[TEST] Server with one address, client with two: both paths go to the same address...\n";

    Server server;
    assert(server.listen("127.0.0.1", 19312));
    serve_replies(server, 512 * 1024);

    Client client(0xABCDEF);
    assert(client.add_local_address("127.0.1.1"));
    assert(client.add_local_address("127.0.1.2"));
    assert(client.connect("127.0.0.1", 19312));
    Connection *connection = client.get_connection();
    assert(connection->get_connection_id() == 0xABCDEF);

    size_t received = 0;
    bool fin_received = false;
    connection->on_stream_data([&](uint32_t, uint64_t, const std::vector<uint8_t> &data, bool fin) {
        received += data.size();
        if (fin)
            fin_received = true;
    });

    SocketManager manager;
    server.register_sockets(manager);
    client.register_sockets(manager);

    connection->write(1, reinterpret_cast<const uint8_t *>("x"), 1);
    connection->close_stream(1);
    connection->send_pending();

    assert(run_until(manager, server, client, [&]() { return fin_received; }, 20000));
    assert(received == 512 * 1024 + 1);

    // The smartphone case: two interfaces towards one server address
    assert(connection->get_paths().size() == 2);
    assert(connection->get_path(1)->get_local_ip() == "127.0.1.2");
    assert(connection->get_path(1)->get_remote_ip() == "127.0.0.1");
    assert(connection->get_path(1)->get_remote_port() == 19312);

    // On the server the two paths share one socket and differ by the address of the client
    Connection *server_connection = server.get_connection(0xABCDEF);
    assert(server_connection != nullptr);
    assert(server_connection->get_paths().size() == 2);
    assert(server_connection->get_path(0)->get_remote_ip() == "127.0.1.1");
    assert(server_connection->get_path(1)->get_remote_ip() == "127.0.1.2");
    assert(server_connection->get_path(0)->get_socket() == server_connection->get_path(1)->get_socket());

    connection->close();
    assert(run_until(manager, server, client, [&]() { return server.get_num_connections() == 0; }, 5000));

    std::cout << "  -> PASSED\n";
}

void test_single_path_client()
{
    std::cout << "\n[TEST] Client with one address: the connection stays single-path...\n";

    Server server;
    assert(server.listen("127.0.0.1", 19313));
    assert(server.listen("127.0.0.2", 19313));
    serve_replies(server, 256 * 1024);

    Client client;
    assert(client.add_local_address("127.0.1.1"));
    assert(!client.add_local_address("not-an-address"));
    assert(client.connect("127.0.0.2", 19313)); // any address of the server can be the first one
    Connection *connection = client.get_connection();

    bool fin_received = false;
    connection->on_stream_data([&](uint32_t, uint64_t, const std::vector<uint8_t> &, bool fin) {
        if (fin)
            fin_received = true;
    });

    SocketManager manager;
    server.register_sockets(manager);
    client.register_sockets(manager);

    connection->write(1, reinterpret_cast<const uint8_t *>("x"), 1);
    connection->close_stream(1);
    connection->send_pending();

    assert(run_until(manager, server, client, [&]() { return fin_received; }, 20000));

    // The server advertised two addresses, but the client has no other interface to use
    assert(connection->get_paths().size() == 1);
    assert(connection->get_path(0)->get_remote_ip() == "127.0.0.2");
    assert(connection->get_duplicated_packets() == 0);

    connection->close();
    assert(run_until(manager, server, client, [&]() { return server.get_num_connections() == 0; }, 5000));

    std::cout << "  -> PASSED\n";
}

void test_several_streams()
{
    std::cout << "\n[TEST] Several requests, each on its own stream, over one connection...\n";

    Server server;
    assert(server.listen("127.0.0.1", 19314));
    serve_replies(server, 1000);

    Client client;
    assert(client.add_local_address("127.0.1.1"));
    assert(client.add_local_address("127.0.1.2"));
    assert(client.connect("127.0.0.1", 19314));
    Connection *connection = client.get_connection();

    std::map<uint32_t, std::vector<uint8_t>> replies;
    size_t finished = 0;
    connection->on_stream_data([&](uint32_t stream_id, uint64_t offset, const std::vector<uint8_t> &data, bool fin) {
        assert(offset == replies[stream_id].size()); // in order inside every stream
        replies[stream_id].insert(replies[stream_id].end(), data.begin(), data.end());
        if (fin)
            finished++;
    });

    SocketManager manager;
    server.register_sockets(manager);
    client.register_sockets(manager);

    // Five requests on the streams 1, 3, 5, 7, 9 (a client uses odd stream numbers)
    for (uint32_t stream_id = 1; stream_id <= 9; stream_id += 2)
    {
        std::string request = "request on stream " + std::to_string(stream_id);
        connection->write(stream_id, reinterpret_cast<const uint8_t *>(request.data()), request.size());
        connection->close_stream(stream_id);
    }
    connection->send_pending();

    assert(run_until(manager, server, client, [&]() { return finished == 5; }, 20000));

    for (uint32_t stream_id = 1; stream_id <= 9; stream_id += 2)
    {
        assert(replies[stream_id] == pattern(1000 + stream_id));
    }
    assert(server.get_connections_accepted() == 1); // one connection for all the streams

    connection->close();
    assert(run_until(manager, server, client, [&]() { return server.get_num_connections() == 0; }, 5000));

    std::cout << "  -> PASSED\n";
}

void test_idle_timeout()
{
    std::cout << "\n[TEST] A client that disappears: the server closes the connection after the idle timeout...\n";

    Server server;
    server.set_idle_timeout(300 * 1000); // 300 ms instead of 30 s
    assert(server.listen("127.0.0.1", 19315));
    serve_replies(server, 2000);

    std::string close_reason;
    bool closed_by_peer = true;
    server.on_connection_closed([&](Connection *connection) {
        close_reason = connection->get_close_reason();
        closed_by_peer = connection->is_closed_by_peer();
    });

    Client client;
    assert(client.add_local_address("127.0.1.1"));
    assert(client.connect("127.0.0.1", 19315));
    Connection *connection = client.get_connection();

    bool fin_received = false;
    connection->on_stream_data([&](uint32_t, uint64_t, const std::vector<uint8_t> &, bool fin) {
        if (fin)
            fin_received = true;
    });

    SocketManager manager;
    server.register_sockets(manager);
    client.register_sockets(manager);

    connection->write(1, reinterpret_cast<const uint8_t *>("x"), 1);
    connection->close_stream(1);
    connection->send_pending();
    assert(run_until(manager, server, client, [&]() { return fin_received; }, 20000));
    assert(server.get_num_connections() == 1);

    // From now on the client is gone: it neither reads nor sends, and never closes
    uint64_t start_us = Packet::current_time_us();
    assert(run_until(manager, server, client, [&]() { return server.get_num_connections() == 0; }, 5000, false));
    uint64_t waited_ms = (Packet::current_time_us() - start_us) / 1000;

    assert(close_reason == "nothing was received from the other side for 300 ms (idle timeout)");
    assert(!closed_by_peer);
    assert(waited_ms >= 250 && waited_ms < 2000);
    std::cout << "[Server] connection closed after " << waited_ms << " ms of silence\n";

    std::cout << "  -> PASSED\n";
}

void test_path_manager_rules()
{
    std::cout << "\n[TEST] Path manager: which paths are created, and towards which address...\n";

    // ---- Server side: paths come from the packets received ----
    {
        Connection connection(1);
        PathManager manager(&connection);

        Path *path0 = manager.on_packet_received(0, nullptr, "10.0.0.2", "10.0.0.1", 5000);
        assert(path0 != nullptr && path0->get_id() == 0);
        assert(path0->get_remote_ip() == "10.0.0.1" && path0->get_remote_port() == 5000);

        // Another packet of the same path: the same Path object
        assert(manager.on_packet_received(0, nullptr, "10.0.0.2", "10.0.0.1", 5000) == path0);
        assert(manager.get_num_paths() == 1);

        // The client is behind a NAT that changed its port: the path follows, its state is kept
        assert(manager.on_packet_received(0, nullptr, "10.0.0.2", "10.0.0.1", 6000) == path0);
        assert(path0->get_remote_port() == 6000);
        assert(manager.get_num_paths() == 1);

        // Paths created by a client have an odd Path ID: an even one is refused
        assert(manager.on_packet_received(2, nullptr, "10.0.1.2", "10.0.1.1", 5001) == nullptr);
        Path *path1 = manager.on_packet_received(1, nullptr, "10.0.1.2", "10.0.1.1", 5001);
        assert(path1 != nullptr && path1->get_id() == 1);
        assert(manager.get_num_paths() == 2);
        assert(connection.get_paths().size() == 2);
    }

    // ---- Client side: the connection started on the SECOND address of the server ----
    {
        Connection connection(2);
        PathManager manager(&connection);
        manager.add_local_socket(nullptr, "10.0.1.1");
        manager.add_local_socket(nullptr, "10.0.0.1");
        assert(manager.open_initial_path("10.0.1.2", 4433) != nullptr);

        std::vector<NetworkAddress> advertised(2);
        advertised[0].ip = "10.0.0.2";
        advertised[0].port = 4433;
        advertised[1].ip = "10.0.1.2";
        advertised[1].port = 4433;

        // The second interface goes to the address that no path uses yet
        std::cout << "(the \"Cannot send\" line below is expected: these paths have no socket)\n";
        assert(manager.on_add_address(advertised) == 1);
        assert(connection.get_path(1) != nullptr);
        assert(connection.get_path(1)->get_local_ip() == "10.0.0.1");
        assert(connection.get_path(1)->get_remote_ip() == "10.0.0.2");

        // The same frame again (it was retransmitted): nothing more to open
        assert(manager.on_add_address(advertised) == 0);
        assert(manager.get_num_paths() == 2);
    }

    std::cout << "  -> PASSED\n";
}

int main()
{
    std::cout << "=== Running Client / Server Endpoint Tests (Phase 11) ===\n";
    test_multihomed_server();
    test_single_homed_server();
    test_single_path_client();
    test_several_streams();
    test_idle_timeout();
    test_path_manager_rules();
    std::cout << "\n=== All Endpoint Tests Passed Successfully! ===\n";
    return 0;
}
