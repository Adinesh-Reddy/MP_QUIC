#include <bits/stdc++.h>
#include "../include/path.hpp"

using namespace std;

Path::Path(uint8_t id, const string &local_ip, uint16_t local_port,
           const string &remote_ip, uint16_t remote_port, UDPSocket *sock)
{
    this->path_id = id;
    this->local_ip = local_ip;
    this->local_port = local_port;
    this->remote_ip = remote_ip;
    this->remote_port = remote_port;
    this->socket = sock;
}

ssize_t Path::send(const uint8_t *data, size_t length)
{
    if (socket == nullptr || !socket->isValid())
    {
        cout << "Cannot send on Path " << (int)path_id << ": socket is invalid\n";
        return -1;
    }
    return socket->sendTo(data, length, remote_ip, remote_port);
}

uint8_t Path::get_id()
{
    return path_id;
}

string Path::get_local_ip()
{
    return local_ip;
}

uint16_t Path::get_local_port()
{
    return local_port;
}

string Path::get_remote_ip()
{
    return remote_ip;
}

uint16_t Path::get_remote_port()
{
    return remote_port;
}

UDPSocket *Path::get_socket()
{
    return socket;
}
