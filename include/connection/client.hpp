#ifndef CLIENT_HPP
#define CLIENT_HPP

#include <bits/stdc++.h>
#include "sockets/udp_socket.hpp"
#include "sockets/socket_manager.hpp"
#include "path/path_manager.hpp"
#include "connection/connection.hpp"

using namespace std;

// Client endpoint: one connection to a server, over one path per local interface.
//
//   Client client;
//   client.add_local_address("10.0.0.1");      // first interface: carries the initial path
//   client.add_local_address("10.0.1.1");      // second interface: a second path is opened later
//   client.connect("10.0.0.2", 4433);          // creates Path ID 0
//   client.get_connection()->write(...);       // the first packet opens the connection
//
// The second path is opened when the server has answered and advertised its addresses
// (ADD_ADDRESS frame). With a single local address the connection stays single-path.
//
// Like the Server, the client does not run its own event loop: give its sockets to a
// SocketManager, pass every datagram to handle_datagram() and call on_tick() regularly.
class Client
{
public:
    Client();                       // random Connection ID
    Client(uint64_t connection_id);

    // Binds a UDP socket on one of the addresses of this host (port 0 = chosen by the system)
    bool add_local_address(const string &ip, uint16_t port = 0);

    bool connect(const string &server_ip, uint16_t server_port);

    void register_sockets(SocketManager &manager);
    bool owns_socket(int fd) const;

    void handle_datagram(int fd, const uint8_t *data, size_t length, const string &src_ip, uint16_t src_port);
    void on_tick();

    Connection *get_connection();
    PathManager *get_path_manager();

private:
    static uint64_t random_connection_id();

    // Order of the members = order of creation. They are destroyed in the opposite order:
    // first the paths (path manager), then the connection, then the sockets.
    vector<unique_ptr<UDPSocket>> sockets;
    Connection connection;
    PathManager path_manager;
};

#endif
