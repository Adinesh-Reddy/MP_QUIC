#ifndef MP_QUIC_MINI_PATH_HPP
#define MP_QUIC_MINI_PATH_HPP

#include <bits/stdc++.h>
#include "udp_socket.hpp"

using namespace std;

class Path
{
public:
    Path(uint8_t id, const string &local_ip, uint16_t local_port,
         const string &remote_ip, uint16_t remote_port, UDPSocket *sock);

    ssize_t send(const uint8_t *data, size_t length);

    uint8_t get_id();
    string get_local_ip();
    uint16_t get_local_port();
    string get_remote_ip();
    uint16_t get_remote_port();
    UDPSocket *get_socket();

private:
    uint8_t path_id;
    string local_ip;
    uint16_t local_port;
    string remote_ip;
    uint16_t remote_port;
    UDPSocket *socket = nullptr;
};

#endif
