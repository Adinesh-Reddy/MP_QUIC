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
    bool isValid() const;
    void close();
    int get_fd();
};







#endif