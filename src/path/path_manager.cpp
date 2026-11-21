#include<bits/stdc++.h>

#include "path/path_manager.hpp"

using namespace std;

PathManager::PathManager(Connection *connection)
{
    this->connection=connection;
}

size_t PathManager::get_num_paths() const
{
    return paths.size();
}

Path *PathManager::create_path(uint8_t path_id, UDPSocket *socket, const string &local_ip,
                               const string &remote_ip, uint16_t remote_port)
{
    uint16_t local_port=(socket!=nullptr) ? socket->get_local_port() : 0;

    unique_ptr<Path> path(new Path(path_id, local_ip, local_port, remote_ip, remote_port, socket));
    if(!connection->add_path(path.get())){
        return nullptr; // path ID already exists
    }

    cout<<"[PATH] Path "<<(int)path_id<<" created: "<<local_ip<<":"<<local_port
         <<" <-> "<<remote_ip<<":"<<remote_port<<"\n";

    paths.push_back(move(path));
    return paths.back().get();
}

void PathManager::add_local_socket(UDPSocket *socket, const string &local_ip)
{
    LocalInterface interface;
    interface.socket=socket;
    interface.ip=local_ip;
    interface.has_path=false;
    local_interfaces.push_back(interface);
}

Path *PathManager::open_initial_path(const string &remote_ip, uint16_t remote_port)
{
    if(local_interfaces.empty()||local_interfaces[0].has_path){
        return nullptr;
    }

    initial_remote.ip=remote_ip;
    initial_remote.port=remote_port;

    Path *path=create_path(0, local_interfaces[0].socket, local_interfaces[0].ip, remote_ip, remote_port);
    if(path!=nullptr){
        local_interfaces[0].has_path=true;
    }
    return path;
}

size_t PathManager::on_add_address(const vector<NetworkAddress> &addresses)
{
    // Peer addresses not yet used by any path
    vector<NetworkAddress> candidates;
    for(const auto &address : addresses){
        bool in_use=any_of(paths.begin(), paths.end(), [&](const unique_ptr<Path> &path) {
            return path->get_remote_ip()==address.ip&&path->get_remote_port()==address.port;
        });
        if(!in_use){
            candidates.push_back(address);
        }
    }

    size_t next_candidate=0;
    size_t opened=0;

    for(auto &interface : local_interfaces){
        if(interface.has_path){
            continue;
        }
        if(next_path_id>255){
            break; // Path ID fits in 1 byte
        }

        // Pair free local interfaces with free peer addresses in order
        NetworkAddress remote=initial_remote;
        if(next_candidate<candidates.size()){
            remote=candidates[next_candidate++];
        }

        Path *path=create_path(static_cast<uint8_t>(next_path_id), interface.socket, interface.ip,
                                 remote.ip, remote.port);
        if(path==nullptr){
            continue;
        }
        interface.has_path=true;
        next_path_id+=2;
        opened++;

        // Send PING to announce the new path and measure initial RTT
        connection->send_ping(path->get_id());
    }

    return opened;
}

Path *PathManager::on_packet_received(uint8_t path_id, UDPSocket *socket, const string &local_ip,
                                      const string &remote_ip, uint16_t remote_port)
{
    auto known=find_if(paths.begin(), paths.end(),
                         [&](const unique_ptr<Path> &path) { return path->get_id()==path_id; });
    if(known!=paths.end()){
        Path *path=known->get();

        // Update peer address if it changed (e.g., NAT rebind)
        if(path->get_remote_ip()!=remote_ip||path->get_remote_port()!=remote_port){
            cout<<"[PATH] Path "<<(int)path_id<<" remote address updated to "
                 <<remote_ip<<":"<<remote_port<<"\n";
            path->set_remote_address(remote_ip, remote_port);
        }
        return path;
    }

    // Only accept path 0 or odd path IDs from client
    if(path_id!=0&&path_id%2==0){
        return nullptr;
    }
    return create_path(path_id, socket, local_ip, remote_ip, remote_port);
}
