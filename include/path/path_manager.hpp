#ifndef PATH_MANAGER_HPP
#define PATH_MANAGER_HPP

#include <bits/stdc++.h>
#include "sockets/udp_socket.hpp"
#include "packet/frame.hpp"
#include "path/path.hpp"
#include "connection/connection.hpp"

using namespace std;

// The path manager controls the creation of the paths of one connection (paper, Section 3,
// Path Management). It owns the Path objects and gives them to the Connection.
//
// Client: the connection starts on the initial path (Path ID 0), from the first local
// interface. When the server answers and advertises its addresses (ADD_ADDRESS frame), the
// client opens one path from each of its other interfaces. Paths created by the client have
// an odd Path ID (1, 3, ...). A PING is sent on a new path to tell the server it exists.
//
// Server: it creates nothing by itself. A path appears when the first packet with a new
// Path ID arrives; the source address of that packet is the remote address of the path.
// No handshake is needed on the new path, it can carry data at once.
class PathManager
{
public:
    PathManager(Connection *connection);

    // ---- Client side ----

    // A local interface (a socket bound to one of the addresses of the host).
    // The order matters: the first one carries the initial path.
    void add_local_socket(UDPSocket *socket, const string &local_ip);

    // Opens the initial path (Path ID 0) from the first local interface
    Path *open_initial_path(const string &remote_ip, uint16_t remote_port);

    // The server advertised its addresses: opens a path from every local interface that has
    // none yet. Returns the number of paths opened.
    //   - an advertised address that is not used by a path yet is taken, in the order given
    //   - if none is left (the server has a single address) the path goes to the address
    //     of the initial path
    size_t on_add_address(const vector<NetworkAddress> &addresses);

    // ---- Server side ----

    // A packet arrived for this connection. Returns the path it belongs to:
    //   - a new Path ID: the path is created (only 0 and odd IDs, the ones a client may use)
    //   - a known Path ID from a new source address: the remote address of the path is updated
    // Returns nullptr if the Path ID is not acceptable.
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

    // Path ID of the next path created by the client: odd numbers
    unsigned next_path_id = 1;
};

#endif
