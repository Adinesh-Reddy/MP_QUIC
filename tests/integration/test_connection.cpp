#include <iostream>
#include <cassert>
#include <vector>
#include <map>
#include <random>
#include <string>
#include <deque>
#include <algorithm>

#include "sockets/udp_socket.hpp"
#include "path/path.hpp"
#include "sockets/socket_manager.hpp"
#include "packet/packet.hpp"
#include "connection/connection.hpp"

// End-to-end tests over loopback: a client sends a block of data to a server over two paths.
//
//   Path 0: client 127.0.0.1:19101  <->  server 127.0.0.1:19201
//   Path 1: client 127.0.0.1:19102  <->  server 127.0.0.1:19202
//
// The network problems are simulated in the receive callback of the test (a datagram can be
// dropped, handed over late, held for a fixed delay, or sent through a bottleneck with a
// limited rate and a limited queue), so the protocol code itself has no test hooks.

struct Scenario
{
    std::string name;
    size_t total_bytes = 0;
    double loss_rate = 0.0;        // probability that a datagram is dropped (both directions)
    double late_rate = 0.0;        // probability that a datagram is handed over late (after newer ones)
    int path_delay_ms[2] = {0, 0}; // extra one-way delay of each path (makes its RTT about twice that)

    // Bottleneck of each path, in both directions: datagrams leave at this rate, at most
    // path_queue_packets wait in the queue, the ones that find the queue full are dropped
    double path_capacity[2] = {0, 0};       // bytes per second (0 = no limit)
    size_t path_queue_packets[2] = {0, 0};
    int tick_ms = 10;                       // period of the event loop

    int kill_path = -1;            // this path drops everything once the server has kill_after bytes
    size_t kill_after = 0;
    size_t restore_after = 0;      // the killed path works again once the server has this many bytes (0 = never)
    bool single_path = false;      // only path 0 is given to the connections
    int expect_majority_path = -1; // this path must carry more packets than the other one
    bool expect_congestion = false; // the bottleneck must have dropped packets and the windows must have been cut
    bool expect_load_sharing = false; // both paths must carry a real share of the data
};

struct HeldDatagram
{
    int fd;
    std::vector<uint8_t> data;
    uint64_t release_us; // time at which it is handed over (0 = at the next turn of the event loop)
};

void run_scenario(const Scenario &scenario)
{
    std::cout << "\n[SCENARIO] " << scenario.name << "\n";

    const uint64_t CONNECTION_ID = 0xC0FFEE;
    const uint32_t STREAM_ID = 1;
    const size_t TOTAL_BYTES = scenario.total_bytes;

    UDPSocket client_sock0, client_sock1, server_sock0, server_sock1;
    assert(client_sock0.bind("127.0.0.1", 19101));
    assert(client_sock1.bind("127.0.0.1", 19102));
    assert(server_sock0.bind("127.0.0.1", 19201));
    assert(server_sock1.bind("127.0.0.1", 19202));

    // Both hosts use the same Path ID for the same path
    Path client_path0(0, "127.0.0.1", 19101, "127.0.0.1", 19201, &client_sock0);
    Path client_path1(1, "127.0.0.1", 19102, "127.0.0.1", 19202, &client_sock1);
    Path server_path0(0, "127.0.0.1", 19201, "127.0.0.1", 19101, &server_sock0);
    Path server_path1(1, "127.0.0.1", 19202, "127.0.0.1", 19102, &server_sock1);

    Connection client(CONNECTION_ID);
    Connection server(CONNECTION_ID);
    assert(client.add_path(&client_path0));
    assert(server.add_path(&server_path0));
    assert(!client.add_path(&client_path0)); // same Path ID twice is refused

    if (!scenario.single_path)
    {
        assert(client.add_path(&client_path1));
        assert(server.add_path(&server_path1));
    }

    // The default scheduler sends on the path with the lowest RTT
    assert(client.get_scheduler()->get_name() == "lowest-rtt");

    // Data to transfer
    std::vector<uint8_t> sent_data(TOTAL_BYTES);
    for (size_t i = 0; i < sent_data.size(); i++)
        sent_data[i] = static_cast<uint8_t>((i * 31 + 7) % 256);

    std::vector<uint8_t> received_data;
    bool fin_received = false;

    // The data must come out of the connection in order, every byte once,
    // whatever happened to the packets on the way
    server.on_stream_data([&](uint32_t stream_id, uint64_t offset, const std::vector<uint8_t> &data, bool fin) {
        assert(stream_id == STREAM_ID);
        assert(!fin_received);
        assert(offset == received_data.size());
        received_data.insert(received_data.end(), data.begin(), data.end());
        if (fin)
        {
            fin_received = true;
        }
    });

    // One event loop for the four sockets: each socket belongs to one connection and one path
    std::map<int, Connection *> owner;
    owner[client_sock0.get_fd()] = &client;
    owner[client_sock1.get_fd()] = &client;
    owner[server_sock0.get_fd()] = &server;
    owner[server_sock1.get_fd()] = &server;

    std::map<int, int> path_of;
    path_of[client_sock0.get_fd()] = 0;
    path_of[server_sock0.get_fd()] = 0;
    path_of[client_sock1.get_fd()] = 1;
    path_of[server_sock1.get_fd()] = 1;

    Path *client_paths[2] = {&client_path0, &client_path1};
    Path *server_paths[2] = {&server_path0, &server_path1};

    SocketManager manager;
    assert(manager.add_socket(&client_sock0));
    assert(manager.add_socket(&client_sock1));
    assert(manager.add_socket(&server_sock0));
    assert(manager.add_socket(&server_sock1));

    // Simulated network problems
    std::mt19937 rng(12345);
    std::uniform_real_distribution<double> chance(0.0, 1.0);
    std::vector<HeldDatagram> held;
    size_t datagrams = 0, dropped = 0, late = 0, bottleneck_dropped = 0;

    // Bottleneck queues, one per socket (= one per path and direction):
    // the times at which the datagrams waiting in the queue will have left it
    std::map<int, std::deque<uint64_t>> queue_departures;

    bool path_dead = false;            // the killed path is currently cut
    bool path_was_killed = false;
    bool path_was_restored = false;
    bool client_saw_failure = false;   // the client marked the killed path potentially failed at some point
    uint64_t sent_on_killed_path_at_restore = 0;

    manager.on_receive([&](int fd, const uint8_t *data, size_t len, std::string /*ip*/, uint16_t /*port*/) {
        datagrams++;
        int path_index = path_of[fd];

        // Cut and repair of a path, driven by how much the server has received so far
        if (scenario.kill_path >= 0)
        {
            if (!path_was_killed && received_data.size() >= scenario.kill_after)
            {
                path_dead = true;
                path_was_killed = true;
            }
            if (path_dead && scenario.restore_after > 0 && received_data.size() >= scenario.restore_after)
            {
                path_dead = false;
                path_was_restored = true;
                sent_on_killed_path_at_restore =
                    client_paths[scenario.kill_path]->get_sent_packet_handler()->get_packets_sent();
            }
        }

        if (path_dead && path_index == scenario.kill_path)
        {
            dropped++;
            return;
        }
        if (chance(rng) < scenario.loss_rate)
        {
            dropped++;
            return;
        }

        uint64_t now_us = Packet::current_time_us();
        uint64_t release = now_us;
        bool hold = false;

        // Bottleneck: the datagram waits for the ones before it, then takes len / capacity to leave
        if (scenario.path_capacity[path_index] > 0)
        {
            std::deque<uint64_t> &queue = queue_departures[fd];
            while (!queue.empty() && queue.front() <= now_us)
            {
                queue.pop_front();
            }
            if (queue.size() >= scenario.path_queue_packets[path_index])
            {
                bottleneck_dropped++; // queue full: this is what congestion looks like
                return;
            }
            uint64_t start = queue.empty() ? now_us : queue.back();
            release = start + static_cast<uint64_t>(len * 1000000.0 / scenario.path_capacity[path_index]);
            queue.push_back(release);
            hold = true;
        }

        // Fixed delay of the path
        if (scenario.path_delay_ms[path_index] > 0)
        {
            release += scenario.path_delay_ms[path_index] * 1000ULL;
            hold = true;
        }

        if (hold)
        {
            held.push_back({fd, std::vector<uint8_t>(data, data + len), release});
            return;
        }
        // Reordering: the datagram is handed over at the next turn, after the ones that follow it
        if (chance(rng) < scenario.late_rate)
        {
            late++;
            held.push_back({fd, std::vector<uint8_t>(data, data + len), 0});
            return;
        }

        owner[fd]->handle_packet(data, len);
    });

    uint64_t start_us = Packet::current_time_us();
    bool finished = false;

    manager.on_tick([&]() {
        // Held datagrams whose time has come arrive now
        uint64_t now_us = Packet::current_time_us();
        std::vector<HeldDatagram> waiting;
        waiting.swap(held);
        for (const auto &d : waiting)
        {
            if (d.release_us <= now_us)
            {
                owner[d.fd]->handle_packet(d.data.data(), d.data.size());
            }
            else
            {
                held.push_back(d);
            }
        }

        client.on_tick();
        server.on_tick();

        if (scenario.kill_path >= 0 && client_paths[scenario.kill_path]->is_potentially_failed())
        {
            client_saw_failure = true;
        }

        // Done when the server has everything and the client got an ACK for all the data
        if (fin_received && client.all_data_acknowledged())
        {
            finished = true;
            manager.stop();
        }
        else if (Packet::current_time_us() - start_us > 60ULL * 1000 * 1000) // 60 seconds, something is wrong
        {
            manager.stop();
        }
    });

    client.write(STREAM_ID, sent_data.data(), sent_data.size());
    client.close_stream(STREAM_ID);
    assert(client.has_stream_data());
    assert(!client.all_data_acknowledged());
    client.send_pending();

    manager.run(scenario.tick_ms);

    uint64_t elapsed_ms = (Packet::current_time_us() - start_us) / 1000;
    std::cout << "[Network] " << datagrams << " datagrams, " << dropped << " dropped, "
              << late << " handed over late, " << bottleneck_dropped << " dropped by a full queue, "
              << elapsed_ms << " ms\n";

    assert(finished);
    assert(!client.is_closed());
    assert(!server.is_closed());
    assert(received_data.size() == TOTAL_BYTES);
    assert(received_data == sent_data);
    std::cout << "[Server] Received " << received_data.size() << " bytes in order, content identical\n";

    uint64_t total_retransmissions = 0;
    uint64_t sent_packets[2] = {0, 0};
    for (int i = 0; i < 2; i++)
    {
        SentPacketHandler *sent = client_paths[i]->get_sent_packet_handler();
        ReceivedPacketHandler *rcvd = server_paths[i]->get_received_packet_handler();

        std::cout << "[Path " << i << "] sent " << sent->get_packets_sent()
                  << " packets (" << sent->get_losses() << " lost, " << sent->get_retransmissions()
                  << " retransmitted), server received " << rcvd->get_packets_received()
                  << ", smoothed RTT " << client_paths[i]->get_rtt_stats()->get_smoothed_rtt() << " us"
                  << (client_paths[i]->is_potentially_failed() ? ", POTENTIALLY FAILED" : "") << "\n";

        OliaSender *olia = client_paths[i]->get_congestion();
        std::cout << "[Congestion " << i << "] " << olia->get_name() << ": window "
                  << olia->get_congestion_window_packets() << " packets, threshold "
                  << olia->get_slow_start_threshold_packets() << ", "
                  << (olia->in_slow_start() ? "slow start" : "congestion avoidance") << "\n";

        total_retransmissions += sent->get_retransmissions();
        sent_packets[i] = sent->get_packets_sent();

        // No stream data is left unacknowledged on the client
        // (a PING that probes a broken path can still be waiting for its ACK)
        assert(!sent->has_outstanding_stream_frames());
        assert(!sent->has_retransmissions());
        assert(rcvd->get_largest_observed() <= sent->get_last_sent_packet_number());
    }

    std::cout << "[Scheduler] " << client.get_scheduler()->get_name() << ": "
              << client.get_duplicated_packets() << " packets duplicated at start, "
              << client.get_ping_probes() << " PING probes, "
              << client.get_paths_frames_sent() << " PATHS frames sent by the client, "
              << server.get_paths_frames_sent() << " by the server\n";

    // Flow control: the server opened its windows with WINDOW_UPDATE frames, otherwise the
    // client could never have sent more than the initial window
    FlowController *client_fc = client.get_flow_control_manager()->get_flow_controller(STREAM_ID);
    assert(client_fc->get_bytes_sent() == TOTAL_BYTES);
    assert(client_fc->get_bytes_sent() <= client_fc->get_send_window());
    if (TOTAL_BYTES > FlowWindows::STREAM_INITIAL)
    {
        assert(client_fc->get_send_window() > FlowWindows::STREAM_INITIAL);
    }
    std::cout << "[Flow control] stream send window grew from " << FlowWindows::STREAM_INITIAL
              << " to " << client_fc->get_send_window() << " bytes\n";

    if (scenario.loss_rate > 0)
    {
        // Lost data was sent again
        assert(total_retransmissions > 0);
    }

    if (scenario.single_path)
    {
        assert(sent_packets[1] == 0);
        assert(client.get_duplicated_packets() == 0); // nowhere to duplicate
    }
    else
    {
        // Start-up duplication: the first packets go on both paths because no RTT is known yet.
        // It stops with the first ACK, so only a small part of the transfer is duplicated.
        assert(client.get_duplicated_packets() > 0);
        assert(client.get_duplicated_packets() <= 64);
    }

    if (scenario.kill_path < 0 && scenario.loss_rate == 0 && scenario.late_rate == 0)
    {
        // No path failed, so no PATHS frame was needed
        assert(client.get_paths_frames_sent() == 0);
        assert(server.get_paths_frames_sent() == 0);
    }

    if (scenario.kill_path < 0 && !scenario.single_path && scenario.loss_rate == 0)
    {
        // Both paths work: each one got an RTT measurement of its own
        assert(client_path0.get_rtt_stats()->get_smoothed_rtt() > 0);
        assert(client_path1.get_rtt_stats()->get_smoothed_rtt() > 0);
        assert(sent_packets[0] > 0 && sent_packets[1] > 0);
    }

    if (scenario.expect_majority_path >= 0)
    {
        // Lowest RTT first: the fast path carries most of the packets, whatever its Path ID
        int fast = scenario.expect_majority_path;
        int slow = 1 - fast;
        assert(sent_packets[fast] > sent_packets[slow]);
        assert(client_paths[fast]->get_rtt_stats()->get_smoothed_rtt() <
               client_paths[slow]->get_rtt_stats()->get_smoothed_rtt());
        std::cout << "[Scheduler] fast path " << fast << " carried "
                  << (100 * sent_packets[fast]) / (sent_packets[0] + sent_packets[1]) << "% of the packets\n";
    }

    if (scenario.path_capacity[0] > 0 || scenario.path_capacity[1] > 0)
    {
        double capacity = scenario.path_capacity[0] + (scenario.single_path ? 0 : scenario.path_capacity[1]);
        double goodput = TOTAL_BYTES * 1000.0 / elapsed_ms;
        std::cout << "[Goodput] " << static_cast<uint64_t>(goodput / 1000) << " kB/s, the bottleneck"
                  << (scenario.single_path ? " allows " : "s allow ") << static_cast<uint64_t>(capacity / 1000)
                  << " kB/s (" << static_cast<int>(100 * goodput / capacity) << "% used)\n";

        if (scenario.expect_congestion)
        {
            // The sender pushed until the queue overflowed, lost packets, and cut its window:
            // slow start ended on every path that carried data
            assert(bottleneck_dropped > 0);
            uint64_t total_losses = 0;
            for (int i = 0; i < 2; i++)
            {
                total_losses += client_paths[i]->get_sent_packet_handler()->get_losses();
                if (sent_packets[i] > 0)
                {
                    assert(client_paths[i]->get_congestion()->get_slow_start_threshold_packets() <
                           CongestionDefaults::MAX_WINDOW_PACKETS);
                }
            }
            assert(total_losses > 0);

            // Nothing can go faster than the bottleneck (5% tolerance for the timing of the test),
            // and a good congestion control keeps it busy
            assert(goodput <= 1.05 * capacity);
            assert(goodput >= 0.60 * capacity);
        }

        if (scenario.expect_load_sharing)
        {
            // Both paths carry a real part of the data ...
            uint64_t all_packets = sent_packets[0] + sent_packets[1];
            assert(sent_packets[0] * 5 >= all_packets && sent_packets[1] * 5 >= all_packets);

            // ... and together they are faster than the best path alone could ever be
            double best_single_path = std::max(scenario.path_capacity[0], scenario.path_capacity[1]);
            assert(goodput > 1.10 * best_single_path);
            std::cout << "[Goodput] " << static_cast<int>(100 * goodput / best_single_path - 100)
                      << "% faster than the best path alone could be\n";
        }
    }

    if (scenario.kill_path >= 0)
    {
        int killed = scenario.kill_path;
        int other = 1 - killed;

        // The client noticed the broken path (RTO), probed it with a PING,
        // and continued on the other path
        assert(path_was_killed);
        assert(client_saw_failure);
        assert(client.get_ping_probes() > 0);
        assert(!client_paths[other]->is_potentially_failed());

        // The host that noticed the failure first told the other one with a PATHS frame,
        // sent on the path that still works (it can be the client or the server)
        assert(client.get_paths_frames_sent() + server.get_paths_frames_sent() > 0);
        assert(client.get_paths_frames_received() + server.get_paths_frames_received() > 0);

        if (scenario.restore_after > 0)
        {
            // The path was repaired: the client uses it again
            assert(path_was_restored);
            assert(!client_paths[killed]->is_potentially_failed());
            assert(sent_packets[killed] > sent_on_killed_path_at_restore + 20);
            std::cout << "[Scheduler] path " << killed << " carried "
                      << sent_packets[killed] - sent_on_killed_path_at_restore
                      << " more packets after it was repaired\n";
        }
        else
        {
            assert(client_paths[killed]->is_potentially_failed());
        }
    }

    std::cout << "  -> PASSED\n";
}

int main()
{
    std::cout << "=== Running Two-Path End-to-End Tests (Phase 4 to 10) ===\n";

    // ---- Reliability (Phase 4 to 7) ----
    Scenario clean;
    clean.name = "Clean network";
    clean.total_bytes = 2 * 1024 * 1024;
    run_scenario(clean);

    Scenario loss;
    loss.name = "5% packet loss in both directions";
    loss.total_bytes = 1024 * 1024;
    loss.loss_rate = 0.05;
    run_scenario(loss);

    Scenario reorder;
    reorder.name = "10% of the packets arrive late (reordering)";
    reorder.total_bytes = 1024 * 1024;
    reorder.late_rate = 0.10;
    run_scenario(reorder);

    Scenario both;
    both.name = "Loss and reordering together";
    both.total_bytes = 512 * 1024;
    both.loss_rate = 0.03;
    both.late_rate = 0.05;
    run_scenario(both);

    Scenario fail;
    fail.name = "Path 1 fails during the transfer";
    fail.total_bytes = 2 * 1024 * 1024;
    fail.kill_path = 1;
    fail.kill_after = 512 * 1024;
    run_scenario(fail);

    Scenario single;
    single.name = "Single path with 5% packet loss";
    single.total_bytes = 512 * 1024;
    single.loss_rate = 0.05;
    single.single_path = true;
    run_scenario(single);

    // ---- Scheduler (Phase 8) ----
    Scenario slow1;
    slow1.name = "Path 1 is slow (30 ms each way): lowest RTT first";
    slow1.total_bytes = 2 * 1024 * 1024;
    slow1.path_delay_ms[1] = 30;
    slow1.expect_majority_path = 0;
    run_scenario(slow1);

    Scenario slow0;
    slow0.name = "Path 0 is slow (30 ms each way): the choice follows the RTT, not the Path ID";
    slow0.total_bytes = 2 * 1024 * 1024;
    slow0.path_delay_ms[0] = 30;
    slow0.expect_majority_path = 1;
    run_scenario(slow0);

    Scenario handover;
    handover.name = "Fast path 0 fails, then is repaired: handover to the slow path and back";
    handover.total_bytes = 4 * 1024 * 1024;
    handover.path_delay_ms[1] = 20;
    handover.kill_path = 0;
    handover.kill_after = 1024 * 1024;
    handover.restore_after = 2 * 1024 * 1024;
    run_scenario(handover);

    // ---- Congestion control (Phase 9 & 10) ----
    Scenario one_bottleneck;
    one_bottleneck.name = "One path limited to 8 Mbit/s: the window finds the capacity of the path";
    one_bottleneck.total_bytes = 2 * 1024 * 1024;
    one_bottleneck.single_path = true;
    one_bottleneck.path_capacity[0] = 1000000;
    one_bottleneck.path_queue_packets[0] = 30;
    one_bottleneck.path_delay_ms[0] = 5;
    one_bottleneck.tick_ms = 1;
    one_bottleneck.expect_congestion = true;
    run_scenario(one_bottleneck);

    Scenario two_bottlenecks;
    two_bottlenecks.name = "Two limited paths, 8 Mbit/s and 4 Mbit/s: OLIA uses both";
    two_bottlenecks.total_bytes = 3 * 1024 * 1024;
    two_bottlenecks.path_capacity[0] = 1000000;
    two_bottlenecks.path_capacity[1] = 500000;
    two_bottlenecks.path_queue_packets[0] = 30;
    two_bottlenecks.path_queue_packets[1] = 30;
    two_bottlenecks.path_delay_ms[0] = 5;
    two_bottlenecks.path_delay_ms[1] = 5;
    two_bottlenecks.tick_ms = 1;
    two_bottlenecks.expect_congestion = true;
    two_bottlenecks.expect_load_sharing = true;
    run_scenario(two_bottlenecks);

    std::cout << "\n=== All End-to-End Tests Passed! ===\n";
    return 0;
}
