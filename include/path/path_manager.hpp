#ifndef PATH_MANAGER_HPP
#define PATH_MANAGER_HPP

#include<bits/stdc++.h>
#include "sockets/udp_socket.hpp"
#include "packet/frame.hpp"
#include "path/path.hpp"
#include "connection/connection.hpp"

using namespace std;

// Manages paths for a connection
class PathManager
{
public:
    PathManager(Connection *connection);

    // Register local socket and IP
    void add_local_socket(UDPSocket *socket, const string &local_ip);

    // Open initial path (Path ID 0)
    Path *open_initial_path(const string &remote_ip, uint16_t remote_port);

    // Open secondary paths when ADD_ADDRESS arrives
    size_t on_add_address(const vector<NetworkAddress> &addresses);

    // Find or create path when a packet arrives on the server
    Path *on_packet_received(uint8_t path_id, UDPSocket *socket, const string &local_ip,
                             const string &remote_ip, uint16_t remote_port);

    size_t get_num_paths() const;

private:
    Path *create_path(uint8_t path_id, UDPSocket *socket, const string &local_ip,
                      const string &remote_ip, uint16_t remote_port);

    struct LocalInterface
    {
        UDPSocket *socket;
        string ip;
        bool has_path;
    };

    Connection *connection;
    vector<unique_ptr<Path>> paths;

    vector<LocalInterface> local_interfaces;
    NetworkAddress initial_remote;

    // Client path IDs are odd (1, 3, ...)
    unsigned next_path_id=1;
};

#endif
