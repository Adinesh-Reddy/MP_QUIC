#ifndef UDP_SOCKET
#define UDP_SOCKET


#include <bits/stdc++.h>
#include <sys/types.h>
using namespace std;


class UDPSocket{
    private:
    int fd = -1;

    public:

    UDPSocket();
    ~UDPSocket();

    UDPSocket (const UDPSocket& )= delete;
    UDPSocket& operator=(const UDPSocket& )= delete;
    ssize_t sendTo(const uint8_t *data, size_t length, const string &dest_ip, uint16_t dest_port);
    ssize_t recvFrom(uint8_t *buffer, size_t max_length,string &src_ip, uint16_t &src_port);

    bool bind(const string& ip,uint16_t port);

    // Port the socket is bound to (useful after bind(ip, 0), where the system chooses it)
    uint16_t get_local_port();

    // Asks the system for larger send and receive buffers, so that a burst of packets
    // is not dropped before the program had the time to read it
    bool set_buffer_size(int bytes);
    bool isValid() const;
    void close();
    int get_fd();
};







#endif