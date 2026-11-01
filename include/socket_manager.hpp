#ifndef SOCKET_MANAGER
#define SOCKET_MANAGER

#include "bits/stdc++.h"
#include "udp_socket.hpp"

typedef void (*RecvCallback)(int fd, const uint8_t *data, size_t len, std::string ip, uint16_t port);

class SocketManager
{
public:
    SocketManager();
    ~SocketManager();

    bool add_socket(UDPSocket *sock);
    void on_receive(RecvCallback cb);
    void run();
    void stop();

private:
    int epoll_fd = -1;
    bool running = false;
    std::unordered_map<int, UDPSocket *> sockets;
    RecvCallback callback = nullptr;
};

#endif