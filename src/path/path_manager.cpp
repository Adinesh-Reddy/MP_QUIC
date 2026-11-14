#include <bits/stdc++.h>

#include "path/path_manager.hpp"

using namespace std;

PathManager::PathManager(Connection *connection)
{
    this->connection = connection;
}

size_t PathManager::get_num_paths() const
{
    return paths.size();
}

Path *PathManager::create_path(uint8_t path_id, UDPSocket *socket, const string &local_ip,
                               const string &remote_ip, uint16_t remote_port)
{
    uint16_t local_port = (socket != nullptr) ? socket->get_local_port() : 0;

    unique_ptr<Path> path(new Path(path_id, local_ip, local_port, remote_ip, remote_port, socket));
    if (!connection->add_path(path.get()))
    {
        return nullptr; // the connection already has a path with this ID
    }

    cout << "Path " << (int)path_id << " created: " << local_ip << ":" << local_port
         << " <-> " << remote_ip << ":" << remote_port << "\n";

    paths.push_back(move(path));
    return paths.back().get();
}

void PathManager::add_local_socket(UDPSocket *socket, const string &local_ip)
{
    LocalInterface interface;
    interface.socket = socket;
    interface.ip = local_ip;
    interface.has_path = false;
    local_interfaces.push_back(interface);
}

Path *PathManager::open_initial_path(const string &remote_ip, uint16_t remote_port)
{
    if (local_interfaces.empty() || local_interfaces[0].has_path)
    {
        return nullptr;
    }

    initial_remote.ip = remote_ip;
    initial_remote.port = remote_port;

    Path *path = create_path(0, local_interfaces[0].socket, local_interfaces[0].ip, remote_ip, remote_port);
    if (path != nullptr)
    {
        local_interfaces[0].has_path = true;
    }
    return path;
}

size_t PathManager::on_add_address(const vector<NetworkAddress> &addresses)
{
    // Addresses of the server that no path of ours goes to yet
    vector<NetworkAddress> candidates;
    for (const auto &address : addresses)
    {
        bool in_use = false;
        for (const auto &path : paths)
        {
            if (path->get_remote_ip() == address.ip && path->get_remote_port() == address.port)
                in_use = true;
        }
        if (!in_use)
        {
            candidates.push_back(address);
        }
    }

    size_t next_candidate = 0;
    size_t opened = 0;

    for (auto &interface : local_interfaces)
    {
        if (interface.has_path)
        {
            continue;
        }
        if (next_path_id > 255)
        {
            break; // the Path ID is one byte
        }

        // The i-th free interface goes to the i-th free address of the server. (The reference
        // implementation opens a path for every pair of addresses; with two disjoint networks
        // only the matching pairs can work, so we open those.)
        NetworkAddress remote = initial_remote;
        if (next_candidate < candidates.size())
        {
            remote = candidates[next_candidate++];
        }

        Path *path = create_path(static_cast<uint8_t>(next_path_id), interface.socket, interface.ip,
                                 remote.ip, remote.port);
        if (path == nullptr)
        {
            continue;
        }
        interface.has_path = true;
        next_path_id += 2;
        opened++;

        // Send a PING to inform the peer of the existence of the path
        // and to get its first RTT measurement
        connection->send_ping(path->get_id());
    }

    return opened;
}

Path *PathManager::on_packet_received(uint8_t path_id, UDPSocket *socket, const string &local_ip,
                                      const string &remote_ip, uint16_t remote_port)
{
    for (auto &path : paths)
    {
        if (path->get_id() != path_id)
        {
            continue;
        }

        // The peer now sends from another address on this path (NAT rebinding):
        // answer there, the state of the path is kept
        if (path->get_remote_ip() != remote_ip || path->get_remote_port() != remote_port)
        {
            cout << "Path " << (int)path_id << ": the other side now sends from "
                 << remote_ip << ":" << remote_port << ", so our packets now go there\n";
            path->set_remote_address(remote_ip, remote_port);
        }
        return path.get();
    }

    // A new path. Paths are created by the client: the initial one (0) and odd IDs.
    if (path_id != 0 && path_id % 2 == 0)
    {
        return nullptr;
    }
    return create_path(path_id, socket, local_ip, remote_ip, remote_port);
}
