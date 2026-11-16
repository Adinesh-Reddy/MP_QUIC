#include<bits/stdc++.h>

#include "sockets/udp_socket.hpp"
#include "sockets/socket_manager.hpp"
#include "packet/packet.hpp"
#include "path/path_manager.hpp"
#include "connection/client.hpp"
#include "connection/server.hpp"

using namespace std;

/*
 Endpoint tests over loopback in one process (no manual server config):
 client 127.0.1.1 / 127.0.1.2, server 127.0.0.1 / 127.0.0.2.
*/

// Run event loop until done() or max_ms timeout
static bool run_until(SocketManager &manager, Server &server, Client &client,
                      const function<bool()> &done, uint64_t max_ms,
                      bool client_alive=true)
{
    uint64_t start_us=Packet::current_time_us();
    bool reached=false;

    manager.on_receive([&](int fd, const uint8_t *data, size_t len, string ip, uint16_t port) {
        if(server.owns_socket(fd)){
            server.handle_datagram(fd, data, len, ip, port);
        }
        else if(client_alive&&client.owns_socket(fd)){
            client.handle_datagram(fd, data, len, ip, port);
        }
    });

    manager.on_tick([&]() {
        if(client_alive){
            client.on_tick();
        }
        server.on_tick();

        if(done()){
            reached=true;
            manager.stop();
        }
        else if(Packet::current_time_us()-start_us>max_ms*1000){
            manager.stop();
        }
    });

    manager.run(1);
    return reached;
}

static vector<uint8_t> pattern(size_t size)
{
    vector<uint8_t> data(size);
    for(size_t i=0; i<size; i++)
        data[i]=static_cast<uint8_t>((i*131+17)%256);
    return data;
}

// Server replies with reply_size + stream_id bytes on each completed request stream
static void serve_replies(Server &server, size_t reply_size)
{
    server.on_new_connection([reply_size](Connection *connection) {
        connection->on_stream_data([connection, reply_size](uint32_t stream_id, uint64_t, const vector<uint8_t> &, bool fin) {
            if(fin){
                vector<uint8_t> reply=pattern(reply_size+stream_id); // unique size per stream
                connection->write(stream_id, reply.data(), reply.size());
                connection->close_stream(stream_id);
            }
        });
    });
}

void test_multihomed_server()
{
    cout<<"\n[TEST] Multihomed server (2 IPs): auto-creates connection and paths\n";

    Server server;
    assert(server.listen("127.0.0.1", 19311));
    assert(server.listen("127.0.0.2", 19311));
    assert(!server.listen("127.0.0.1", 19311)); // already bound
    assert(server.get_listen_addresses().size()==2);
    serve_replies(server, 1024*1024);

    // Server-side connection info captured on close
    size_t server_paths=0;
    string server_path1_remote;
    bool server_closed_by_peer=false;
    string server_close_reason;
    server.on_connection_closed([&](Connection *connection) {
        server_paths=connection->get_paths().size();
        if(connection->get_path(1)!=nullptr)
            server_path1_remote=connection->get_path(1)->get_remote_ip();
        server_closed_by_peer=connection->is_closed_by_peer();
        server_close_reason=connection->get_close_reason();
    });

    Client client;
    assert(client.add_local_address("127.0.1.1"));
    assert(client.add_local_address("127.0.1.2"));
    assert(client.connect("127.0.0.1", 19311));
    assert(!client.connect("127.0.0.1", 19311)); // path 0 already open

    Connection *connection=client.get_connection();

    // Only path 0 exists before server replies
    assert(connection->get_paths().size()==1);
    assert(connection->get_path(0)!=nullptr);

    vector<uint8_t> received;
    bool fin_received=false;
    connection->on_stream_data([&](uint32_t stream_id, uint64_t offset, const vector<uint8_t> &data, bool fin) {
        assert(stream_id==1);
        assert(offset==received.size());
        received.insert(received.end(), data.begin(), data.end());
        if(fin)
            fin_received=true;
    });

    SocketManager manager;
    server.register_sockets(manager);
    client.register_sockets(manager);

    // First client packet opens the connection (no handshake)
    string request="hello";
    connection->write(1, reinterpret_cast<const uint8_t *>(request.data()), request.size());
    connection->close_stream(1);
    connection->send_pending();

    assert(run_until(manager, server, client, [&]() { return fin_received; }, 20000));

    assert(received==pattern(1024*1024+1));
    assert(server.get_connections_accepted()==1);
    assert(server.get_num_connections()==1);

    // Client opened path 1 (odd ID) to server's second IP after ADD_ADDRESS
    assert(connection->get_paths().size()==2);
    Path *path1=connection->get_path(1);
    assert(path1!=nullptr);
    assert(path1->get_local_ip()=="127.0.1.2");
    assert(path1->get_remote_ip()=="127.0.0.2"&&path1->get_remote_port()==19311);
    assert(connection->get_path(0)->get_remote_ip()=="127.0.0.1");

    // Server also used path 1 automatically
    assert(path1->get_received_packet_handler()->get_packets_received()>5);
    assert(path1->get_rtt_stats()->get_smoothed_rtt()>0); // from initial PING
    cout<<"[INFO] Client path 0 rx="<<connection->get_path(0)->get_received_packet_handler()->get_packets_received()
         <<" path 1 rx="<<path1->get_received_packet_handler()->get_packets_received()<<"\n";

    // Client closes -> server removes connection right away
    connection->close("transfer complete");
    assert(connection->is_closed()&&!connection->is_closed_by_peer());
    assert(run_until(manager, server, client, [&]() { return server.get_num_connections()==0; }, 5000));

    assert(server_paths==2);
    assert(server_path1_remote=="127.0.1.2");
    assert(server_closed_by_peer);
    assert(server_close_reason=="the other side closed the connection (its reason: transfer complete)");

    // Late packet must not reopen closed connection
    Packet late(connection->get_connection_id(), 0, 999999);
    late.add_frame(Frame::make_ping());
    vector<uint8_t> raw=late.serialize();
    UDPSocket stray;
    assert(stray.sendTo(raw.data(), raw.size(), "127.0.0.1", 19311)>0);
    run_until(manager, server, client, []() { return false; }, 50);
    assert(server.get_connections_accepted()==1);
    assert(server.get_num_connections()==0);

    cout<<"  -> PASS\n";
}

void test_single_homed_server()
{
    cout<<"\n[TEST] Single-homed server (1 IP), dual-homed client (2 IPs)\n";

    Server server;
    assert(server.listen("127.0.0.1", 19312));
    serve_replies(server, 512*1024);

    Client client(0xABCDEF);
    assert(client.add_local_address("127.0.1.1"));
    assert(client.add_local_address("127.0.1.2"));
    assert(client.connect("127.0.0.1", 19312));
    Connection *connection=client.get_connection();
    assert(connection->get_connection_id()==0xABCDEF);

    size_t received=0;
    bool fin_received=false;
    connection->on_stream_data([&](uint32_t, uint64_t, const vector<uint8_t> &data, bool fin) {
        received+=data.size();
        if(fin)
            fin_received=true;
    });

    SocketManager manager;
    server.register_sockets(manager);
    client.register_sockets(manager);

    connection->write(1, reinterpret_cast<const uint8_t *>("x"), 1);
    connection->close_stream(1);
    connection->send_pending();

    assert(run_until(manager, server, client, [&]() { return fin_received; }, 20000));
    assert(received==512*1024+1);

    // Both client interfaces connect to the single server IP
    assert(connection->get_paths().size()==2);
    assert(connection->get_path(1)->get_local_ip()=="127.0.1.2");
    assert(connection->get_path(1)->get_remote_ip()=="127.0.0.1");
    assert(connection->get_path(1)->get_remote_port()==19312);

    // Server paths share one socket, separated by client IP
    Connection *server_connection=server.get_connection(0xABCDEF);
    assert(server_connection!=nullptr);
    assert(server_connection->get_paths().size()==2);
    assert(server_connection->get_path(0)->get_remote_ip()=="127.0.1.1");
    assert(server_connection->get_path(1)->get_remote_ip()=="127.0.1.2");
    assert(server_connection->get_path(0)->get_socket()==server_connection->get_path(1)->get_socket());

    connection->close();
    assert(run_until(manager, server, client, [&]() { return server.get_num_connections()==0; }, 5000));

    cout<<"  -> PASS\n";
}

void test_single_path_client()
{
    cout<<"\n[TEST] Single-path client (1 IP): stays single-path\n";

    Server server;
    assert(server.listen("127.0.0.1", 19313));
    assert(server.listen("127.0.0.2", 19313));
    serve_replies(server, 256*1024);

    Client client;
    assert(client.add_local_address("127.0.1.1"));
    assert(!client.add_local_address("not-an-address"));
    assert(client.connect("127.0.0.2", 19313)); // can start on any server IP
    Connection *connection=client.get_connection();

    bool fin_received=false;
    connection->on_stream_data([&](uint32_t, uint64_t, const vector<uint8_t> &, bool fin) {
        if(fin)
            fin_received=true;
    });

    SocketManager manager;
    server.register_sockets(manager);
    client.register_sockets(manager);

    connection->write(1, reinterpret_cast<const uint8_t *>("x"), 1);
    connection->close_stream(1);
    connection->send_pending();

    assert(run_until(manager, server, client, [&]() { return fin_received; }, 20000));

    // Only 1 local IP, so client keeps 1 path even after ADD_ADDRESS
    assert(connection->get_paths().size()==1);
    assert(connection->get_path(0)->get_remote_ip()=="127.0.0.2");
    assert(connection->get_duplicated_packets()==0);

    connection->close();
    assert(run_until(manager, server, client, [&]() { return server.get_num_connections()==0; }, 5000));

    cout<<"  -> PASS\n";
}

void test_several_streams()
{
    cout<<"\n[TEST] Multiple streams over one connection\n";

    Server server;
    assert(server.listen("127.0.0.1", 19314));
    serve_replies(server, 1000);

    Client client;
    assert(client.add_local_address("127.0.1.1"));
    assert(client.add_local_address("127.0.1.2"));
    assert(client.connect("127.0.0.1", 19314));
    Connection *connection=client.get_connection();

    map<uint32_t, vector<uint8_t>> replies;
    size_t finished=0;
    connection->on_stream_data([&](uint32_t stream_id, uint64_t offset, const vector<uint8_t> &data, bool fin) {
        assert(offset==replies[stream_id].size()); // in order per stream
        replies[stream_id].insert(replies[stream_id].end(), data.begin(), data.end());
        if(fin)
            finished++;
    });

    SocketManager manager;
    server.register_sockets(manager);
    client.register_sockets(manager);

    // 5 requests on odd client streams (1, 3, 5, 7, 9)
    for(uint32_t stream_id=1; stream_id<=9; stream_id+=2){
        string request="request on stream "+to_string(stream_id);
        connection->write(stream_id, reinterpret_cast<const uint8_t *>(request.data()), request.size());
        connection->close_stream(stream_id);
    }
    connection->send_pending();

    assert(run_until(manager, server, client, [&]() { return finished==5; }, 20000));

    for(uint32_t stream_id=1; stream_id<=9; stream_id+=2){
        assert(replies[stream_id]==pattern(1000+stream_id));
    }
    assert(server.get_connections_accepted()==1); // all streams shared one connection

    connection->close();
    assert(run_until(manager, server, client, [&]() { return server.get_num_connections()==0; }, 5000));

    cout<<"  -> PASS\n";
}

void test_idle_timeout()
{
    cout<<"\n[TEST] Idle timeout closes silent client\n";

    Server server;
    server.set_idle_timeout(300*1000); // 300 ms for test
    assert(server.listen("127.0.0.1", 19315));
    serve_replies(server, 2000);

    string close_reason;
    bool closed_by_peer=true;
    server.on_connection_closed([&](Connection *connection) {
        close_reason=connection->get_close_reason();
        closed_by_peer=connection->is_closed_by_peer();
    });

    Client client;
    assert(client.add_local_address("127.0.1.1"));
    assert(client.connect("127.0.0.1", 19315));
    Connection *connection=client.get_connection();

    bool fin_received=false;
    connection->on_stream_data([&](uint32_t, uint64_t, const vector<uint8_t> &, bool fin) {
        if(fin)
            fin_received=true;
    });

    SocketManager manager;
    server.register_sockets(manager);
    client.register_sockets(manager);

    connection->write(1, reinterpret_cast<const uint8_t *>("x"), 1);
    connection->close_stream(1);
    connection->send_pending();
    assert(run_until(manager, server, client, [&]() { return fin_received; }, 20000));
    assert(server.get_num_connections()==1);

    // Stop client loop to simulate silent drop
    uint64_t start_us=Packet::current_time_us();
    assert(run_until(manager, server, client, [&]() { return server.get_num_connections()==0; }, 5000, false));
    uint64_t waited_ms=(Packet::current_time_us()-start_us)/1000;

    assert(close_reason=="nothing was received from the other side for 300 ms (idle timeout)");
    assert(!closed_by_peer);
    assert(waited_ms>=250&&waited_ms<2000);
    cout<<"[INFO] Server closed silent connection after "<<waited_ms<<" ms\n";

    cout<<"  -> PASS\n";
}

void test_path_manager_rules()
{
    cout<<"\n[TEST] PathManager creation and address rules\n";

    // Server side: paths created from incoming packets
    {
        Connection connection(1);
        PathManager manager(&connection);

        Path *path0=manager.on_packet_received(0, nullptr, "10.0.0.2", "10.0.0.1", 5000);
        assert(path0!=nullptr&&path0->get_id()==0);
        assert(path0->get_remote_ip()=="10.0.0.1"&&path0->get_remote_port()==5000);

        // Same address returns existing path
        assert(manager.on_packet_received(0, nullptr, "10.0.0.2", "10.0.0.1", 5000)==path0);
        assert(manager.get_num_paths()==1);

        // NAT port change updates remote port without resetting path
        assert(manager.on_packet_received(0, nullptr, "10.0.0.2", "10.0.0.1", 6000)==path0);
        assert(path0->get_remote_port()==6000);
        assert(manager.get_num_paths()==1);

        // Reject even path IDs (>0); accept odd IDs
        assert(manager.on_packet_received(2, nullptr, "10.0.1.2", "10.0.1.1", 5001)==nullptr);
        Path *path1=manager.on_packet_received(1, nullptr, "10.0.1.2", "10.0.1.1", 5001);
        assert(path1!=nullptr&&path1->get_id()==1);
        assert(manager.get_num_paths()==2);
        assert(connection.get_paths().size()==2);
    }

    // Client side: started on server's second IP
    {
        Connection connection(2);
        PathManager manager(&connection);
        manager.add_local_socket(nullptr, "10.0.1.1");
        manager.add_local_socket(nullptr, "10.0.0.1");
        assert(manager.open_initial_path("10.0.1.2", 4433)!=nullptr);

        vector<NetworkAddress> advertised(2);
        advertised[0].ip="10.0.0.2";
        advertised[0].port=4433;
        advertised[1].ip="10.0.1.2";
        advertised[1].port=4433;

        // Second local IP pairs with unused server IP
        cout<<"[INFO] Expecting [ERROR] socket not open (no sockets in unit check)\n";
        assert(manager.on_add_address(advertised)==1);
        assert(connection.get_path(1)!=nullptr);
        assert(connection.get_path(1)->get_local_ip()=="10.0.0.1");
        assert(connection.get_path(1)->get_remote_ip()=="10.0.0.2");

        // Duplicate ADD_ADDRESS opens nothing extra
        assert(manager.on_add_address(advertised)==0);
        assert(manager.get_num_paths()==2);
    }

    cout<<"  -> PASS\n";
}

int main()
{
    cout<<"=== Endpoint Tests ===\n";
    test_multihomed_server();
    test_single_homed_server();
    test_single_path_client();
    test_several_streams();
    test_idle_timeout();
    test_path_manager_rules();
    cout<<"=== All Endpoint Tests Passed ===\n";
    return 0;
}
