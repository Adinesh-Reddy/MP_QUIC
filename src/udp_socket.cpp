#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "udp_socket.hpp"

using namespace std;

UDPSocket::UDPSocket()
{
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        cout << "Error in creation of socket in UDPSocket class\n";
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
        cout << "Invalid IP address: " << ip << "\n";
        return false;
    }

    if (::bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        cout << "Bind failed on " << ip << ":" << port << " - " << strerror(errno) << "\n";
        return false;
    }

    cout << "Socket bound to " << ip << ":" << port << "\n";
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
        cout << "Invalid destination IP: " << dest_ip << "\n";
        return -1;
    }

    ssize_t sent = sendto(fd, data, length, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (sent < 0)
    {
        cout << "sendto failed: " << strerror(errno) << "\n";
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
