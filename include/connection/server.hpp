#ifndef SERVER_HPP
#define SERVER_HPP

#include <bits/stdc++.h>
#include "sockets/udp_socket.hpp"
#include "sockets/socket_manager.hpp"
#include "packet/packet.hpp"
#include "path/path_manager.hpp"
#include "connection/connection.hpp"

using namespace std;

typedef std::function<void(Connection *connection)> ConnectionCallback;

// Server endpoint: listens on one or several addresses and serves any number of clients.
//
// There is no handshake (the cryptographic handshake of QUIC is left out of this project):
// the first packet of a client, on Path ID 0, opens the connection. The server then
//   - creates the Connection, identified by the Connection ID of the packet
//   - creates the path from the source address of the packet
//   - advertises all its addresses with an ADD_ADDRESS frame, so that the client can open
//     more paths. They appear here when their first packet arrives.
//
// The server does not run its own event loop. Give its sockets to a SocketManager
// (register_sockets), pass every datagram to handle_datagram() and call on_tick() regularly.
class Server
{
public:
    // Opens a UDP socket on this address. Can be called several times (multihomed server).
    bool listen(const string &ip, uint16_t port);

    void register_sockets(SocketManager &manager);
    bool owns_socket(int fd) const;

    // Called once for each new connection: the application sets its stream callback there
    void on_new_connection(ConnectionCallback cb);

    // Called when a connection is over, just before it is removed
    void on_connection_closed(ConnectionCallback cb);

    void handle_datagram(int fd, const uint8_t *data, size_t length, const string &src_ip, uint16_t src_port);
    void on_tick();

    size_t get_num_connections() const;
    uint64_t get_connections_accepted() const;
    Connection *get_connection(uint64_t connection_id);
    vector<NetworkAddress> get_listen_addresses() const;

    // Idle timeout given to the new connections
    void set_idle_timeout(uint64_t timeout_us);

private:
    struct ListenSocket
    {
        unique_ptr<UDPSocket> socket;
        NetworkAddress address;
    };

    // The path manager is declared after the connection: it is destroyed first,
    // and with it the paths the connection points to
    struct ServerConnection
    {
        unique_ptr<Connection> connection;
        unique_ptr<PathManager> path_manager;
    };

    vector<ListenSocket> listen_sockets;
    map<uint64_t, ServerConnection> connections;

    // Connections that ended a short time ago: late packets for them must not open a new one
    deque<uint64_t> recently_closed;

    ConnectionCallback new_connection_callback = nullptr;
    ConnectionCallback connection_closed_callback = nullptr;
    uint64_t connections_accepted = 0;
    uint64_t idle_timeout_us = Connection::DEFAULT_IDLE_TIMEOUT_US;
};

#endif
