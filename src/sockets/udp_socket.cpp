#include <iostream>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "sockets/udp_socket.hpp"

using namespace std;

UDPSocket::UDPSocket()
{
    fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (fd < 0)
    {
        cout << "Error: could not create a UDP socket (" << strerror(errno) << ")\n";
    }
}

UDPSocket::~UDPSocket()
{
    close();
}

bool UDPSocket::bind(const string &ip, uint16_t port)
{
    if (!isValid())
        return false;

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0)
    {
        cout << "Error: \"" << ip << "\" is not a valid IPv4 address (example of a good one: 10.0.0.1)\n";
        return false;
    }

    if (::bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        int reason = errno;
        cout << "Error: could not use the address " << ip << ":" << port << " (" << strerror(reason) << ")\n";
        // The usual causes, with what to do
        if (reason == EADDRNOTAVAIL)
            cout << "  This computer does not have the address " << ip
                 << ". Check its addresses with: ip -br -4 addr\n";
        else if (reason == EADDRINUSE)
            cout << "  Another program already uses port " << port
                 << ". Maybe an old mpq_server is still running: stop it with: pkill mpq_server\n";
        else if (reason == EACCES)
            cout << "  Ports below 1024 need root. Use a higher port, for example 4433.\n";
        return false;
    }

    cout << "Opened a UDP socket on " << ip << ":" << get_local_port() << "\n";
    return true;
}

ssize_t UDPSocket::sendTo(const uint8_t *data, size_t length, const string &dest_ip, uint16_t dest_port)
{
    if (!isValid())
        return -1;

    sockaddr_in dest_addr;
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(dest_port);

    if (inet_pton(AF_INET, dest_ip.c_str(), &dest_addr.sin_addr) <= 0)
    {
        cout << "Error: cannot send, \"" << dest_ip << "\" is not a valid IPv4 address\n";
        return -1;
    }

    ssize_t sent = sendto(fd, data, length, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (sent < 0)
    {
        int reason = errno;
        cout << "Could not send a packet to " << dest_ip << ":" << dest_port << " (" << strerror(reason) << ")";
        if (reason == ENETUNREACH || reason == EHOSTUNREACH)
            cout << ": there is no route to this address, check the cable and the routes of this path";
        else if (reason == EAGAIN || reason == EWOULDBLOCK || reason == ENOBUFS)
            cout << ": the send buffer is full, this packet is dropped and will be sent again later";
        cout << "\n";
    }
    return sent;
}

ssize_t UDPSocket::recvFrom(uint8_t *buffer, size_t max_length, string &src_ip, uint16_t &src_port)
{
    if (!isValid())
        return -1;

    sockaddr_in src_addr;
    socklen_t addr_len = sizeof(src_addr);

    ssize_t received = recvfrom(fd, buffer, max_length, 0, (struct sockaddr *)&src_addr, &addr_len);
    if (received > 0)
    {
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &src_addr.sin_addr, ip_str, sizeof(ip_str));
        src_ip = ip_str;
        src_port = ntohs(src_addr.sin_port);
    }
    return received;
}

uint16_t UDPSocket::get_local_port()
{
    if (!isValid())
        return 0;

    sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    if (getsockname(fd, (struct sockaddr *)&addr, &addr_len) < 0)
    {
        return 0;
    }
    return ntohs(addr.sin_port);
}

bool UDPSocket::set_buffer_size(int bytes)
{
    if (!isValid())
        return false;

    bool ok = true;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes)) < 0)
        ok = false;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes)) < 0)
        ok = false;
    return ok;
}

int UDPSocket::get_fd()
{
    return fd;
}

bool UDPSocket::isValid() const
{
    return fd >= 0;
}

void UDPSocket::close()
{
    if (fd >= 0)
    {
        ::close(fd);
        fd = -1;
    }
}
