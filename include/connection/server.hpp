#ifndef SERVER_HPP
#define SERVER_HPP

#include<bits/stdc++.h>
#include "sockets/udp_socket.hpp"
#include "sockets/socket_manager.hpp"
#include "packet/packet.hpp"
#include "path/path_manager.hpp"
#include "connection/connection.hpp"

using namespace std;

typedef function<void(Connection *connection)> ConnectionCallback;

/*
 Server endpoint: listens on one or more addresses and handles multiple clients.
 First packet on Path ID 0 opens a new Connection and sends ADD_ADDRESS with all listen IPs.
 Uses an external SocketManager (register_sockets, handle_datagram, on_tick).
*/
class Server
{
public:
    // Bind UDP socket on ip:port (call multiple times for multihomed server)
    bool listen(const string &ip, uint16_t port);

    void register_sockets(SocketManager &manager);
    bool owns_socket(int fd) const;

    // Called once per new connection (app registers stream callback here)
    void on_new_connection(ConnectionCallback cb);

    // Called right before a closed connection is removed
    void on_connection_closed(ConnectionCallback cb);

    void handle_datagram(int fd, const uint8_t *data, size_t length, const string &src_ip, uint16_t src_port);
    void on_tick();

    size_t get_num_connections() const;
    uint64_t get_connections_accepted() const;
    Connection *get_connection(uint64_t connection_id);
    vector<NetworkAddress> get_listen_addresses() const;

    // Idle timeout for newly accepted connections
    void set_idle_timeout(uint64_t timeout_us);

private:
    struct ListenSocket
    {
        unique_ptr<UDPSocket> socket;
        NetworkAddress address;
    };

    struct ServerConnection
    {
        unique_ptr<Connection> connection;
        unique_ptr<PathManager> path_manager;
    };

    vector<ListenSocket> listen_sockets;
    map<uint64_t, ServerConnection> connections;

    // Recently closed IDs (stops late packets from reopening a connection)
    deque<uint64_t> recently_closed;

    ConnectionCallback new_connection_callback=nullptr;
    ConnectionCallback connection_closed_callback=nullptr;
    uint64_t connections_accepted=0;
    uint64_t idle_timeout_us=Connection::DEFAULT_IDLE_TIMEOUT_US;
};

#endif
