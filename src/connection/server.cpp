#include<bits/stdc++.h>

#include "connection/server.hpp"

using namespace std;

// Requested UDP socket buffer size (4 MB; OS may cap it)
static const int SOCKET_BUFFER_SIZE=4*1024*1024;

// Max recently closed connection IDs to remember
static const size_t MAX_RECENTLY_CLOSED=1024;

bool Server::listen(const string &ip, uint16_t port)
{
    unique_ptr<UDPSocket> socket(new UDPSocket());
    if(!socket->bind(ip, port)){
        return false;
    }
    socket->set_buffer_size(SOCKET_BUFFER_SIZE);

    ListenSocket listen_socket;
    listen_socket.address.ip=ip;
    listen_socket.address.port=socket->get_local_port();
    listen_socket.socket=move(socket);
    listen_sockets.push_back(move(listen_socket));
    return true;
}

void Server::register_sockets(SocketManager &manager)
{
    for(auto &listen_socket : listen_sockets){
        manager.add_socket(listen_socket.socket.get());
    }
}

bool Server::owns_socket(int fd) const
{
    for(const auto &listen_socket : listen_sockets){
        if(listen_socket.socket->get_fd()==fd)
            return true;
    }
    return false;
}

void Server::on_new_connection(ConnectionCallback cb)
{
    new_connection_callback=cb;
}

void Server::on_connection_closed(ConnectionCallback cb)
{
    connection_closed_callback=cb;
}

void Server::set_idle_timeout(uint64_t timeout_us)
{
    idle_timeout_us=timeout_us;
}

size_t Server::get_num_connections() const
{
    return connections.size();
}

uint64_t Server::get_connections_accepted() const
{
    return connections_accepted;
}

Connection *Server::get_connection(uint64_t connection_id)
{
    auto it=connections.find(connection_id);
    if(it==connections.end()){
        return nullptr;
    }
    return it->second.connection.get();
}

vector<NetworkAddress> Server::get_listen_addresses() const
{
    vector<NetworkAddress> addresses;
    for(const auto &listen_socket : listen_sockets){
        addresses.push_back(listen_socket.address);
    }
    return addresses;
}

void Server::handle_datagram(int fd, const uint8_t *data, size_t length, const string &src_ip, uint16_t src_port)
{
    // Find which listen socket received this packet
    ListenSocket *listen_socket=nullptr;
    for(auto &candidate : listen_sockets){
        if(candidate.socket->get_fd()==fd)
            listen_socket=&candidate;
    }
    if(listen_socket==nullptr){
        return;
    }

    // Read Connection ID and Path ID from public header
    uint64_t connection_id=0;
    uint8_t path_id=0;
    if(!Packet::peek_header(data, length, connection_id, path_id)){
        return;
    }

    auto it=connections.find(connection_id);
    if(it==connections.end()){
        // Ignore late packets for already closed connections
        if(find(recently_closed.begin(), recently_closed.end(), connection_id)!=recently_closed.end()){
            return;
        }

        // New connection must start with a valid packet on Path 0
        Packet first_packet;
        if(path_id!=0||!Packet::deserialize(data, length, first_packet)){
            return;
        }

        ServerConnection entry;
        entry.connection.reset(new Connection(connection_id));
        entry.connection->set_idle_timeout(idle_timeout_us);
        entry.path_manager.reset(new PathManager(entry.connection.get()));
        it=connections.emplace(connection_id, move(entry)).first;
        connections_accepted++;

        cout<<"[CONN] Connection "<<connection_id<<" accepted from "<<src_ip<<":"<<src_port<<"\n";

        if(it->second.path_manager->on_packet_received(path_id, listen_socket->socket.get(),
                                                        listen_socket->address.ip, src_ip, src_port)==nullptr){
            connections.erase(it);
            return;
        }

        // Send all server listen addresses to client
        it->second.connection->advertise_addresses(get_listen_addresses());

        if(new_connection_callback!=nullptr){
            new_connection_callback(it->second.connection.get());
        }
    }
    else{
        // Known connection: look up or create path
        if(it->second.path_manager->on_packet_received(path_id, listen_socket->socket.get(),
                                                        listen_socket->address.ip, src_ip, src_port)==nullptr){
            return;
        }
    }

    it->second.connection->handle_packet(data, length);
}

void Server::on_tick()
{
    for(auto it=connections.begin(); it!=connections.end();){
        Connection *connection=it->second.connection.get();
        connection->on_tick();

        if(!connection->is_closed()){
            ++it;
            continue;
        }

        // Connection closed: notify app, remember ID, and erase
        if(connection_closed_callback!=nullptr){
            connection_closed_callback(connection);
        }
        recently_closed.push_back(it->first);
        if(recently_closed.size()>MAX_RECENTLY_CLOSED){
            recently_closed.pop_front();
        }
        it=connections.erase(it);
    }
}
