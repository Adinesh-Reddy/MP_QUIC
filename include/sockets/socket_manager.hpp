#ifndef SOCKET_MANAGER
#define SOCKET_MANAGER

#include<bits/stdc++.h>
#include "sockets/udp_socket.hpp"

using namespace std;

// Callbacks for packet arrival and timer tick
typedef function<void(int fd, const uint8_t *data, size_t len, string ip, uint16_t port)> RecvCallback;
typedef function<void()> TickCallback;

class SocketManager
{
public:
    SocketManager();
    ~SocketManager();

    bool add_socket(UDPSocket *sock);
    void on_receive(RecvCallback cb);
    void on_tick(TickCallback cb);
    void run(int tick_ms=10);
    void stop();

private:
    int epoll_fd=-1;
    bool running=false;
    unordered_map<int, UDPSocket *> sockets;
    RecvCallback callback=nullptr;
    TickCallback tick_callback=nullptr;
};

#endif
