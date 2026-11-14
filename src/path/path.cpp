#include <bits/stdc++.h>
#include "path/path.hpp"

using namespace std;

Path::Path(uint8_t id, const string &local_ip, uint16_t local_port,
           const string &remote_ip, uint16_t remote_port, UDPSocket *sock)
    : congestion(&rtt_stats), sent_packet_handler(&rtt_stats, &congestion)
{
    this->path_id = id;
    this->local_ip = local_ip;
    this->local_port = local_port;
    this->remote_ip = remote_ip;
    this->remote_port = remote_port;
    this->socket = sock;

    sent_packet_handler.set_rto_callback([this](uint64_t last_sent_time_us) {
        return on_rto(last_sent_time_us);
    });
}

ssize_t Path::send(const uint8_t *data, size_t length)
{
    if (socket == nullptr || !socket->isValid())
    {
        cout << "Error: cannot send on path " << (int)path_id << " because its socket is not open\n";
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

void Path::set_remote_address(const string &ip, uint16_t port)
{
    remote_ip = ip;
    remote_port = port;
}

uint64_t Path::peek_packet_number()
{
    return next_packet_number;
}

uint64_t Path::pop_packet_number()
{
    return next_packet_number++;
}

RttStats *Path::get_rtt_stats()
{
    return &rtt_stats;
}

OliaSender *Path::get_congestion()
{
    return &congestion;
}

SentPacketHandler *Path::get_sent_packet_handler()
{
    return &sent_packet_handler;
}

ReceivedPacketHandler *Path::get_received_packet_handler()
{
    return &received_packet_handler;
}

bool Path::sending_allowed()
{
    return sent_packet_handler.sending_allowed();
}

bool Path::get_ack_frame(uint64_t now_us, AckFrame &out_ack)
{
    if (!received_packet_handler.get_ack_frame(now_us, out_ack))
    {
        return false;
    }
    out_ack.path_id = path_id;
    return true;
}

uint64_t Path::get_last_network_activity_time()
{
    return last_network_activity_time;
}

void Path::set_last_network_activity_time(uint64_t time_us)
{
    last_network_activity_time = time_us;
}

bool Path::on_rto(uint64_t last_sent_time_us)
{
    // Was there any activity on this path since the last packet we sent?
    if (last_network_activity_time < last_sent_time_us)
    {
        potentially_failed = true;
        cout << "Path " << (int)path_id << " may be broken (marked \"potentially failed\"): nothing came back on it "
             << "before the retransmission timer (RTO) ran out. Its unacknowledged packets will be sent again, "
             << "on another path if there is one.\n";
        return true;
    }
    return false;
}

bool Path::is_potentially_failed()
{
    return potentially_failed;
}

void Path::set_potentially_failed(bool failed)
{
    potentially_failed = failed;
}
