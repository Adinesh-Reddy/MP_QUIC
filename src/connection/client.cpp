#include <bits/stdc++.h>

#include "connection/client.hpp"

using namespace std;

// Socket buffers asked from the system (it may give less)
static const int SOCKET_BUFFER_SIZE = 4 * 1024 * 1024;

uint64_t Client::random_connection_id()
{
    random_device device;
    uint64_t id = (static_cast<uint64_t>(device()) << 32) | device();
    return id;
}

Client::Client()
    : connection(random_connection_id()), path_manager(&connection)
{
    // When the server advertises its addresses, the path manager opens the other paths
    connection.on_add_address([this](const vector<NetworkAddress> &addresses) {
        path_manager.on_add_address(addresses);
    });
}

Client::Client(uint64_t connection_id)
    : connection(connection_id), path_manager(&connection)
{
    connection.on_add_address([this](const vector<NetworkAddress> &addresses) {
        path_manager.on_add_address(addresses);
    });
}

bool Client::add_local_address(const string &ip, uint16_t port)
{
    unique_ptr<UDPSocket> socket(new UDPSocket());
    if (!socket->bind(ip, port))
    {
        return false;
    }
    socket->set_buffer_size(SOCKET_BUFFER_SIZE);

    path_manager.add_local_socket(socket.get(), ip);
    sockets.push_back(move(socket));
    return true;
}

bool Client::connect(const string &server_ip, uint16_t server_port)
{
    return path_manager.open_initial_path(server_ip, server_port) != nullptr;
}

void Client::register_sockets(SocketManager &manager)
{
    for (auto &socket : sockets)
    {
        manager.add_socket(socket.get());
    }
}

bool Client::owns_socket(int fd) const
{
    for (const auto &socket : sockets)
    {
        if (socket->get_fd() == fd)
            return true;
    }
    return false;
}

void Client::handle_datagram(int /*fd*/, const uint8_t *data, size_t length, const string & /*src_ip*/, uint16_t /*src_port*/)
{
    // The Path ID in the header tells the connection which path the packet belongs to
    connection.handle_packet(data, length);
}

void Client::on_tick()
{
    connection.on_tick();
}

Connection *Client::get_connection()
{
    return &connection;
}

PathManager *Client::get_path_manager()
{
    return &path_manager;
}
