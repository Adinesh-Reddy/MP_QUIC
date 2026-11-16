#include<bits/stdc++.h>

#include "sockets/udp_socket.hpp"
#include "path/path.hpp"
#include "sockets/socket_manager.hpp"
#include "packet/packet.hpp"

using namespace std;

atomic<bool> received_packet{false};
atomic<int> tick_count{0};

void on_tick_handler()
{
    tick_count++;
}

void on_packet_received(int /*fd*/, const uint8_t *data, size_t len, string src_ip, uint16_t src_port)
{
    cout<<"[TEST] Got "<<len<<" bytes from "<<src_ip<<":"<<src_port<<"\n";
    Packet pkt;
    bool ok=Packet::deserialize(data, len, pkt);
    assert(ok);
    assert(pkt.get_connection_id()==1001);
    assert(pkt.get_path_id()==1);
    assert(pkt.get_packet_num()==42);
    assert(pkt.get_frames().size()==1);

    const Frame &frame=pkt.get_frames()[0];
    assert(frame.type==FRAME_STREAM);
    assert(frame.stream.stream_id==1);
    assert(frame.stream.offset==0);

    string payload_str(frame.stream.data.begin(), frame.stream.data.end());
    assert(payload_str=="End-to-end packet transmission test");
    cout<<"[TEST] Packet ok, payload: "<<payload_str<<"\n";
    received_packet=true;
}

int main()
{
    cout<<"[TEST] Socket + Path + Packet...\n";

    UDPSocket receiver_sock;
    assert(receiver_sock.bind("127.0.0.1", 19001));

    SocketManager manager;
    assert(manager.add_socket(&receiver_sock));
    manager.on_receive(on_packet_received);
    manager.on_tick(on_tick_handler);

    thread manager_thread([&manager]() {
        manager.run(10);
    });

    this_thread::sleep_for(chrono::milliseconds(50));

    UDPSocket sender_sock;
    Path path1(1, "127.0.0.1", 19002, "127.0.0.1", 19001, &sender_sock);

    string message="End-to-end packet transmission test";
    vector<uint8_t> payload(message.begin(), message.end());
    Packet send_pkt(1001, 1, 42);
    send_pkt.add_frame(Frame::make_stream(1, 0, false, payload));

    vector<uint8_t> serialized=send_pkt.serialize();
    ssize_t sent=path1.send(serialized.data(), serialized.size());
    assert(sent==static_cast<ssize_t>(serialized.size()));

    for(int i=0; i<20&&!received_packet; ++i){
        this_thread::sleep_for(chrono::milliseconds(50));
    }
    assert(received_packet.load());
    assert(tick_count.load()>0);

    manager.stop();
    manager_thread.join();

    cout<<"  -> PASS\n";
    return 0;
}

