#ifndef CLIENT_HPP
#define CLIENT_HPP

#include<bits/stdc++.h>
#include "sockets/udp_socket.hpp"
#include "sockets/socket_manager.hpp"
#include "path/path_manager.hpp"
#include "connection/connection.hpp"

using namespace std;

/*
 Client endpoint: connects to one server using one path per local address.
 Extra paths open after the server sends ADD_ADDRESS.
 Uses an external SocketManager (register_sockets, handle_datagram, on_tick).
*/
class Client
{
public:
    Client();                       // random Connection ID
    Client(uint64_t connection_id);

    // Bind UDP socket on local IP (port 0 = OS picks a free port)
    bool add_local_address(const string &ip, uint16_t port=0);

    bool connect(const string &server_ip, uint16_t server_port);

    void register_sockets(SocketManager &manager);
    bool owns_socket(int fd) const;

    void handle_datagram(int fd, const uint8_t *data, size_t length, const string &src_ip, uint16_t src_port);
    void on_tick();

    Connection *get_connection();
    PathManager *get_path_manager();

private:
    static uint64_t random_connection_id();

    // Destroy order (reverse): path_manager -> connection -> sockets
    vector<unique_ptr<UDPSocket>> sockets;
    Connection connection;
    PathManager path_manager;
};

#endif
