#ifndef SOCKET_MANAGER
#define SOCKET_MANAGER

#include "bits/stdc++.h"
#include "sockets/udp_socket.hpp"

// std::function (instead of a plain function pointer) so that a callback can be a lambda
// that knows which Connection object the data belongs to. Plain functions still work.
typedef std::function<void(int fd, const uint8_t *data, size_t len, std::string ip, uint16_t port)> RecvCallback;
typedef std::function<void()> TickCallback;

class SocketManager
{
public:
    SocketManager();
    ~SocketManager();

    bool add_socket(UDPSocket *sock);
    void on_receive(RecvCallback cb);
    void on_tick(TickCallback cb);
    void run(int tick_ms = 10);
    void stop();

private:
    int epoll_fd = -1;
    bool running = false;
    std::unordered_map<int, UDPSocket *> sockets;
    RecvCallback callback = nullptr;
    TickCallback tick_callback = nullptr;
};

#endif