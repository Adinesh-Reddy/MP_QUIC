#ifndef PATH_MANAGER_HPP
#define PATH_MANAGER_HPP

#include<bits/stdc++.h>
#include "sockets/udp_socket.hpp"
#include "packet/frame.hpp"
#include "path/path.hpp"
#include "connection/connection.hpp"

using namespace std;

/*
 Owns and creates paths for one connection.
 Client: starts with path 0, then opens odd IDs (1, 3, ...) from other local sockets
 when ADD_ADDRESS arrives (sends PING to announce each new path).
 Server: creates paths when packets arrive with a new Path ID.
*/
class PathManager
{
public:
    PathManager(Connection *connection);

    // Client side: add local socket/IP (first one is used for initial path 0)
    void add_local_socket(UDPSocket *socket, const string &local_ip);

    // Open initial path (Path ID 0) from first local interface
    Path *open_initial_path(const string &remote_ip, uint16_t remote_port);

    /*
     Open paths from remaining local interfaces using peer addresses from ADD_ADDRESS.
     Falls back to initial remote address if no unused peer address is left.
     Returns count of new paths opened.
    */
    size_t on_add_address(const vector<NetworkAddress> &addresses);

    // Server side: find path, create on new valid ID (0 or odd), or update NAT address
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
