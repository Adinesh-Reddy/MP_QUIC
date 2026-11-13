#include <bits/stdc++.h>
#include <unistd.h>
#include <sys/epoll.h>

#include "sockets/socket_manager.hpp"

using namespace std;

SocketManager::SocketManager()
{
    epoll_fd = epoll_create1(0);
    if (epoll_fd < 0)
    {
        cout << "Failed to create epoll: " << strerror(errno) << "\n";
    }
}

SocketManager::~SocketManager()
{
    stop();
    if (epoll_fd >= 0)
    {
        close(epoll_fd);
        epoll_fd = -1;
    }
}

bool SocketManager::add_socket(UDPSocket *sock)
{
    if (epoll_fd < 0 || sock == nullptr || !sock->isValid())
    {
        return false;
    }

    epoll_event event;
    event.events = EPOLLIN;
    event.data.fd = sock->get_fd();

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock->get_fd(), &event) < 0)
    {
        cout << "epoll_ctl ADD failed: " << strerror(errno) << "\n";
        return false;
    }

    sockets[sock->get_fd()] = sock;
    return true;
}

void SocketManager::on_receive(RecvCallback cb)
{
    callback = cb;
}

void SocketManager::on_tick(TickCallback cb)
{
    tick_callback = cb;
}

void SocketManager::run(int tick_ms)
{
    if (epoll_fd < 0)
        return;

    running = true;
    cout << "SocketManager event loop started.\n";

    epoll_event events[16];
    uint8_t buffer[65536];

    while (running)
    {
        int num_events = epoll_wait(epoll_fd, events, 16, tick_ms);

        if (num_events < 0)
        {
            if (errno == EINTR)
                continue;
            cout << "epoll_wait error: " << strerror(errno) << "\n";
            break;
        }

        for (int i = 0; i < num_events; i++)
        {
            int ready_fd = events[i].data.fd;

            if (sockets.find(ready_fd) == sockets.end())
                continue;

            UDPSocket *sock = sockets[ready_fd];
            string src_ip;
            uint16_t src_port = 0;

            while (true)
            {
                ssize_t received = sock->recvFrom(buffer, sizeof(buffer), src_ip, src_port);
                if (received <= 0)
                    break;
                if (callback != nullptr)
                {
                    callback(ready_fd, buffer, (size_t)received, src_ip, src_port);
                }
            }
        }

        if (tick_callback != nullptr)
        {
            tick_callback();
        }
    }

    cout << "SocketManager event loop stopped.\n";
}

void SocketManager::stop()
{
    running = false;
}
