#include<bits/stdc++.h>

#include "sockets/udp_socket.hpp"
#include "path/path.hpp"
#include "sockets/socket_manager.hpp"
#include "packet/packet.hpp"
#include "connection/connection.hpp"

using namespace std;

/*
 Two-path loopback tests: client sends data to server over Path 0 and Path 1.
 Network faults (drop, reorder, delay, bottleneck queue) are simulated in the test loop.
*/

struct Scenario
{
    string name;
    size_t total_bytes=0;
    double loss_rate=0.0;        // random drop rate (0..1)
    double late_rate=0.0;        // random late delivery rate (0..1)
    int path_delay_ms[2]={0, 0}; // extra one-way delay per path (ms)

    // Bottleneck per path (rate in B/s, 0 = unlimited; queue in packets)
    double path_capacity[2]={0, 0};
    size_t path_queue_packets[2]={0, 0};
    int tick_ms=10;              // event loop tick (ms)

    int kill_path=-1;            // drop all packets on this path after kill_after bytes
    size_t kill_after=0;
    size_t restore_after=0;      // restore killed path after this many bytes (0 = never)
    bool single_path=false;      // use only path 0
    int expect_majority_path=-1; // path that must carry more packets
    bool expect_congestion=false; // expect queue drops and window cuts
    bool expect_load_sharing=false; // expect both paths to carry data
};

struct HeldDatagram
{
    int fd;
    vector<uint8_t> data;
    uint64_t release_us; // delivery time (0 = next loop tick)
};

void run_scenario(const Scenario &scenario)
{
    cout<<"\n[TEST] "<<scenario.name<<"\n";

    const uint64_t CONNECTION_ID=0xC0FFEE;
    const uint32_t STREAM_ID=1;
    const size_t TOTAL_BYTES=scenario.total_bytes;

    UDPSocket client_sock0, client_sock1, server_sock0, server_sock1;
    assert(client_sock0.bind("127.0.0.1", 19101));
    assert(client_sock1.bind("127.0.0.1", 19102));
    assert(server_sock0.bind("127.0.0.1", 19201));
    assert(server_sock1.bind("127.0.0.1", 19202));

    // Must use same Path ID on both sides
    Path client_path0(0, "127.0.0.1", 19101, "127.0.0.1", 19201, &client_sock0);
    Path client_path1(1, "127.0.0.1", 19102, "127.0.0.1", 19202, &client_sock1);
    Path server_path0(0, "127.0.0.1", 19201, "127.0.0.1", 19101, &server_sock0);
    Path server_path1(1, "127.0.0.1", 19202, "127.0.0.1", 19102, &server_sock1);

    Connection client(CONNECTION_ID);
    Connection server(CONNECTION_ID);
    assert(client.add_path(&client_path0));
    assert(server.add_path(&server_path0));
    assert(!client.add_path(&client_path0)); // duplicate Path ID rejected

    if(!scenario.single_path){
        assert(client.add_path(&client_path1));
        assert(server.add_path(&server_path1));
    }

    // Default scheduler is lowest-rtt
    assert(client.get_scheduler()->get_name()=="lowest-rtt");

    // Test payload
    vector<uint8_t> sent_data(TOTAL_BYTES);
    for(size_t i=0; i<sent_data.size(); i++)
        sent_data[i]=static_cast<uint8_t>((i*31+7)%256);

    vector<uint8_t> received_data;
    bool fin_received=false;

    // Delivered bytes must be strictly in order and exact
    server.on_stream_data([&](uint32_t stream_id, uint64_t offset, const vector<uint8_t> &data, bool fin) {
        assert(stream_id==STREAM_ID);
        assert(!fin_received);
        assert(offset==received_data.size());
        received_data.insert(received_data.end(), data.begin(), data.end());
        if(fin){
            fin_received=true;
        }
    });

    // Map each socket fd to its connection and path index
    map<int, Connection *> owner;
    owner[client_sock0.get_fd()]=&client;
    owner[client_sock1.get_fd()]=&client;
    owner[server_sock0.get_fd()]=&server;
    owner[server_sock1.get_fd()]=&server;

    map<int, int> path_of;
    path_of[client_sock0.get_fd()]=0;
    path_of[server_sock0.get_fd()]=0;
    path_of[client_sock1.get_fd()]=1;
    path_of[server_sock1.get_fd()]=1;

    Path *client_paths[2]={&client_path0, &client_path1};
    Path *server_paths[2]={&server_path0, &server_path1};

    SocketManager manager;
    assert(manager.add_socket(&client_sock0));
    assert(manager.add_socket(&client_sock1));
    assert(manager.add_socket(&server_sock0));
    assert(manager.add_socket(&server_sock1));

    // Simulated network problems
    mt19937 rng(12345);
    uniform_real_distribution<double> chance(0.0, 1.0);
    vector<HeldDatagram> held;
    size_t datagrams=0, dropped=0, late=0, bottleneck_dropped=0;

    // Bottleneck departure queues per socket fd
    map<int, deque<uint64_t>> queue_departures;

    bool path_dead=false;            // killed path is down
    bool path_was_killed=false;
    bool path_was_restored=false;
    bool client_saw_failure=false;   // client saw potentially failed path
    uint64_t sent_on_killed_path_at_restore=0;

    manager.on_receive([&](int fd, const uint8_t *data, size_t len, string /*ip*/, uint16_t /*port*/) {
        datagrams++;
        int path_index=path_of[fd];

        // Simulate path cut and restore based on bytes received
        if(scenario.kill_path>=0){
            if(!path_was_killed&&received_data.size()>=scenario.kill_after){
                path_dead=true;
                path_was_killed=true;
            }
            if(path_dead&&scenario.restore_after>0&&received_data.size()>=scenario.restore_after){
                path_dead=false;
                path_was_restored=true;
                sent_on_killed_path_at_restore=
                    client_paths[scenario.kill_path]->get_sent_packet_handler()->get_packets_sent();
            }
        }

        if(path_dead&&path_index==scenario.kill_path){
            dropped++;
            return;
        }
        if(chance(rng)<scenario.loss_rate){
            dropped++;
            return;
        }

        uint64_t now_us=Packet::current_time_us();
        uint64_t release=now_us;
        bool hold=false;

        // Bottleneck queue simulation
        if(scenario.path_capacity[path_index]>0){
            deque<uint64_t> &queue=queue_departures[fd];
            while(!queue.empty()&&queue.front()<=now_us){
                queue.pop_front();
            }
            if(queue.size()>=scenario.path_queue_packets[path_index]){
                bottleneck_dropped++; // queue full
                return;
            }
            uint64_t start=queue.empty() ? now_us : queue.back();
            release=start+static_cast<uint64_t>(len*1000000.0/scenario.path_capacity[path_index]);
            queue.push_back(release);
            hold=true;
        }

        // Fixed one-way path delay
        if(scenario.path_delay_ms[path_index]>0){
            release+=scenario.path_delay_ms[path_index]*1000ULL;
            hold=true;
        }

        if(hold){
            held.push_back({fd, vector<uint8_t>(data, data+len), release});
            return;
        }
        // Delay packet by 1 tick to simulate reordering
        if(chance(rng)<scenario.late_rate){
            late++;
            held.push_back({fd, vector<uint8_t>(data, data+len), 0});
            return;
        }

        owner[fd]->handle_packet(data, len);
    });

    uint64_t start_us=Packet::current_time_us();
    bool finished=false;

    manager.on_tick([&]() {
        // Deliver held datagrams whose release time passed
        uint64_t now_us=Packet::current_time_us();
        vector<HeldDatagram> waiting;
        waiting.swap(held);
        for(const auto &d : waiting){
            if(d.release_us<=now_us){
                owner[d.fd]->handle_packet(d.data.data(), d.data.size());
            }
            else{
                held.push_back(d);
            }
        }

        client.on_tick();
        server.on_tick();

        if(scenario.kill_path>=0&&client_paths[scenario.kill_path]->is_potentially_failed()){
            client_saw_failure=true;
        }

        // Done when server has FIN and client got all ACKs
        if(fin_received&&client.all_data_acknowledged()){
            finished=true;
            manager.stop();
        }
        else if(Packet::current_time_us()-start_us>60ULL*1000*1000){ // 60s safety timeout
            manager.stop();
        }
    });

    client.write(STREAM_ID, sent_data.data(), sent_data.size());
    client.close_stream(STREAM_ID);
    assert(client.has_stream_data());
    assert(!client.all_data_acknowledged());
    client.send_pending();

    manager.run(scenario.tick_ms);

    uint64_t elapsed_ms=(Packet::current_time_us()-start_us)/1000;
    cout<<"[INFO] Net: "<<datagrams<<" pkts, "<<dropped<<" dropped, "
         <<late<<" late, "<<bottleneck_dropped<<" qdrop, "<<elapsed_ms<<" ms\n";

    assert(finished);
    assert(!client.is_closed());
    assert(!server.is_closed());
    assert(received_data.size()==TOTAL_BYTES);
    assert(received_data==sent_data);
    cout<<"[INFO] Server received "<<received_data.size()<<" bytes in order\n";

    uint64_t total_retransmissions=0;
    uint64_t sent_packets[2]={0, 0};
    for(int i=0; i<2; i++){
        SentPacketHandler *sent=client_paths[i]->get_sent_packet_handler();
        ReceivedPacketHandler *rcvd=server_paths[i]->get_received_packet_handler();

        cout<<"[INFO] Path "<<i<<": tx="<<sent->get_packets_sent()
             <<" lost="<<sent->get_losses()<<" retx="<<sent->get_retransmissions()
             <<" rx="<<rcvd->get_packets_received()
             <<" srtt="<<client_paths[i]->get_rtt_stats()->get_smoothed_rtt()<<"us"
             <<(client_paths[i]->is_potentially_failed() ? " FAILED" : "")<<"\n";

        OliaSender *olia=client_paths[i]->get_congestion();
        cout<<"[INFO] CC "<<i<<": "<<olia->get_name()<<" cwnd="
             <<olia->get_congestion_window_packets()<<" ssthresh="
             <<olia->get_slow_start_threshold_packets()<<" "
             <<(olia->in_slow_start() ? "slow-start" : "cong-avoid")<<"\n";

        total_retransmissions+=sent->get_retransmissions();
        sent_packets[i]=sent->get_packets_sent();

        // No stream data left unacked (probe PINGs may still be in flight)
        assert(!sent->has_outstanding_stream_frames());
        assert(!sent->has_retransmissions());
        assert(rcvd->get_largest_observed()<=sent->get_last_sent_packet_number());
    }

    cout<<"[INFO] Sched "<<client.get_scheduler()->get_name()<<": dup="
         <<client.get_duplicated_packets()<<" ping="
         <<client.get_ping_probes()<<" paths_tx="
         <<client.get_paths_frames_sent()<<" paths_rx_srv="
         <<server.get_paths_frames_sent()<<"\n";

    // Flow control must raise window via WINDOW_UPDATE for transfers > STREAM_INITIAL
    FlowController *client_fc=client.get_flow_control_manager()->get_flow_controller(STREAM_ID);
    assert(client_fc->get_bytes_sent()==TOTAL_BYTES);
    assert(client_fc->get_bytes_sent()<=client_fc->get_send_window());
    if(TOTAL_BYTES>FlowWindows::STREAM_INITIAL){
        assert(client_fc->get_send_window()>FlowWindows::STREAM_INITIAL);
    }
    cout<<"[INFO] Flow window: "<<FlowWindows::STREAM_INITIAL<<" -> "<<client_fc->get_send_window()<<" bytes\n";

    if(scenario.loss_rate>0){
        // Lost data must be resent
        assert(total_retransmissions>0);
    }

    if(scenario.single_path){
        assert(sent_packets[1]==0);
        assert(client.get_duplicated_packets()==0); // no second path
    }
    else{
        // Startup duplication runs until first ACK arrives
        assert(client.get_duplicated_packets()>0);
        assert(client.get_duplicated_packets()<=64);
    }

    if(scenario.kill_path<0&&scenario.loss_rate==0&&scenario.late_rate==0){
        // No path failed: no PATHS frame sent
        assert(client.get_paths_frames_sent()==0);
        assert(server.get_paths_frames_sent()==0);
    }

    if(scenario.kill_path<0&&!scenario.single_path&&scenario.loss_rate==0){
        // Both paths working: both get RTT and carry packets
        assert(client_path0.get_rtt_stats()->get_smoothed_rtt()>0);
        assert(client_path1.get_rtt_stats()->get_smoothed_rtt()>0);
        assert(sent_packets[0]>0&&sent_packets[1]>0);
    }

    if(scenario.expect_majority_path>=0){
        // Lowest-RTT path carries most packets
        int fast=scenario.expect_majority_path;
        int slow=1-fast;
        assert(sent_packets[fast]>sent_packets[slow]);
        assert(client_paths[fast]->get_rtt_stats()->get_smoothed_rtt()<
               client_paths[slow]->get_rtt_stats()->get_smoothed_rtt());
        cout<<"[INFO] Fast path "<<fast<<" share: "
             <<(100*sent_packets[fast])/(sent_packets[0]+sent_packets[1])<<"%\n";
    }

    if(scenario.path_capacity[0]>0||scenario.path_capacity[1]>0){
        double capacity=scenario.path_capacity[0]+(scenario.single_path ? 0 : scenario.path_capacity[1]);
        double goodput=TOTAL_BYTES*1000.0/elapsed_ms;
        cout<<"[INFO] Goodput: "<<static_cast<uint64_t>(goodput/1000)<<"/"
             <<static_cast<uint64_t>(capacity/1000)<<" kB/s ("
             <<static_cast<int>(100*goodput/capacity)<<"%)\n";

        if(scenario.expect_congestion){
            // Queue overflow must trigger loss and exit slow start
            assert(bottleneck_dropped>0);
            uint64_t total_losses=0;
            for(int i=0; i<2; i++){
                total_losses+=client_paths[i]->get_sent_packet_handler()->get_losses();
                if(sent_packets[i]>0){
                    assert(client_paths[i]->get_congestion()->get_slow_start_threshold_packets()<
                           CongestionDefaults::MAX_WINDOW_PACKETS);
                }
            }
            assert(total_losses>0);

            // Goodput stays within [60%, 105%] of bottleneck
            assert(goodput<=1.05*capacity);
            assert(goodput>=0.60*capacity);
        }

        if(scenario.expect_load_sharing){
            // Both paths carry >= 20% of packets and beat best single path
            uint64_t all_packets=sent_packets[0]+sent_packets[1];
            assert(sent_packets[0]*5>=all_packets&&sent_packets[1]*5>=all_packets);

            double best_single_path=max(scenario.path_capacity[0], scenario.path_capacity[1]);
            assert(goodput>1.10*best_single_path);
            cout<<"[INFO] Gain over best single path: +"
                 <<static_cast<int>(100*goodput/best_single_path-100)<<"%\n";
        }
    }

    if(scenario.kill_path>=0){
        int killed=scenario.kill_path;
        int other=1-killed;

        // Client detected broken path, probed with PING, and moved traffic to other path
        assert(path_was_killed);
        assert(client_saw_failure);
        assert(client.get_ping_probes()>0);
        assert(!client_paths[other]->is_potentially_failed());

        // PATHS frame sent on working path
        assert(client.get_paths_frames_sent()+server.get_paths_frames_sent()>0);
        assert(client.get_paths_frames_received()+server.get_paths_frames_received()>0);

        if(scenario.restore_after>0){
            // Path recovered: client uses it again
            assert(path_was_restored);
            assert(!client_paths[killed]->is_potentially_failed());
            assert(sent_packets[killed]>sent_on_killed_path_at_restore+20);
            cout<<"[INFO] Repaired path "<<killed<<" sent "
                 <<sent_packets[killed]-sent_on_killed_path_at_restore<<" more packets\n";
        }
        else{
            assert(client_paths[killed]->is_potentially_failed());
        }
    }

    cout<<"  -> PASS\n";
}

// Packet number 0 is invalid and must not revive a potentially failed path
static void test_invalid_packet_does_not_revive_path()
{
    cout<<"\n[TEST] Packet 0 does not revive broken path\n";

    UDPSocket socket;
    assert(socket.bind("127.0.0.1", 0));
    const uint64_t CONNECTION_ID=77;
    Connection connection(CONNECTION_ID);
    // Port 9 is discard port
    Path path(0, "127.0.0.1", socket.get_local_port(), "127.0.0.1", 9, &socket);
    assert(connection.add_path(&path));
    path.set_potentially_failed(true);

    Packet invalid(CONNECTION_ID, 0, 0);
    invalid.add_frame(Frame::make_ping());
    vector<uint8_t> raw=invalid.serialize();
    connection.handle_packet(raw.data(), raw.size());
    assert(path.is_potentially_failed());
    assert(path.get_last_network_activity_time()==0);

    Packet valid(CONNECTION_ID, 0, 1);
    valid.add_frame(Frame::make_ping());
    raw=valid.serialize();
    connection.handle_packet(raw.data(), raw.size());
    assert(!path.is_potentially_failed());
    assert(path.get_last_network_activity_time()!=0);

    cout<<"  -> PASS\n";
}

int main()
{
    cout<<"=== Two-Path End-to-End Tests ===\n";

    test_invalid_packet_does_not_revive_path();

    // Reliability tests
    Scenario clean;
    clean.name="Clean network";
    clean.total_bytes=2*1024*1024;
    run_scenario(clean);

    Scenario loss;
    loss.name="5% loss both ways";
    loss.total_bytes=1024*1024;
    loss.loss_rate=0.05;
    run_scenario(loss);

    Scenario reorder;
    reorder.name="10% packet reordering";
    reorder.total_bytes=1024*1024;
    reorder.late_rate=0.10;
    run_scenario(reorder);

    Scenario both;
    both.name="Loss + reordering";
    both.total_bytes=512*1024;
    both.loss_rate=0.03;
    both.late_rate=0.05;
    run_scenario(both);

    Scenario fail;
    fail.name="Path 1 fails mid-transfer";
    fail.total_bytes=2*1024*1024;
    fail.kill_path=1;
    fail.kill_after=512*1024;
    run_scenario(fail);

    Scenario single;
    single.name="Single path with 5% loss";
    single.total_bytes=512*1024;
    single.loss_rate=0.05;
    single.single_path=true;
    run_scenario(single);

    // Scheduler tests
    Scenario slow1;
    slow1.name="Slow path 1 (30ms): lowest RTT wins";
    slow1.total_bytes=2*1024*1024;
    slow1.path_delay_ms[1]=30;
    slow1.expect_majority_path=0;
    run_scenario(slow1);

    Scenario slow0;
    slow0.name="Slow path 0 (30ms): lowest RTT wins";
    slow0.total_bytes=2*1024*1024;
    slow0.path_delay_ms[0]=30;
    slow0.expect_majority_path=1;
    run_scenario(slow0);

    Scenario handover;
    handover.name="Path 0 fails and recovers (handover)";
    handover.total_bytes=4*1024*1024;
    handover.path_delay_ms[1]=20;
    handover.kill_path=0;
    handover.kill_after=1024*1024;
    handover.restore_after=2*1024*1024;
    run_scenario(handover);

    // Congestion control tests
    Scenario one_bottleneck;
    one_bottleneck.name="Single 8 Mbit/s bottleneck";
    one_bottleneck.total_bytes=2*1024*1024;
    one_bottleneck.single_path=true;
    one_bottleneck.path_capacity[0]=1000000;
    one_bottleneck.path_queue_packets[0]=30;
    one_bottleneck.path_delay_ms[0]=5;
    one_bottleneck.tick_ms=1;
    one_bottleneck.expect_congestion=true;
    run_scenario(one_bottleneck);

    Scenario two_bottlenecks;
    two_bottlenecks.name="Two bottlenecks (8 + 4 Mbit/s): OLIA sharing";
    two_bottlenecks.total_bytes=3*1024*1024;
    two_bottlenecks.path_capacity[0]=1000000;
    two_bottlenecks.path_capacity[1]=500000;
    two_bottlenecks.path_queue_packets[0]=30;
    two_bottlenecks.path_queue_packets[1]=30;
    two_bottlenecks.path_delay_ms[0]=5;
    two_bottlenecks.path_delay_ms[1]=5;
    two_bottlenecks.tick_ms=1;
    two_bottlenecks.expect_congestion=true;
    two_bottlenecks.expect_load_sharing=true;
    run_scenario(two_bottlenecks);

    cout<<"\n=== All End-to-End Tests Passed ===\n";
    return 0;
}
