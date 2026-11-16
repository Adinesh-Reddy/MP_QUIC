#include<bits/stdc++.h>

#include "packet/frame.hpp"
#include "path/path.hpp"
#include "scheduler/scheduler.hpp"

using namespace std;

// Unit tests for LowestRttScheduler (no sockets needed)

const uint64_t T0=1000000;

// Sample data frame list
static vector<Frame> data_frames()
{
    vector<Frame> frames;
    frames.push_back(Frame::make_stream(1, 0, false, vector<uint8_t>(100, 'x')));
    return frames;
}

// Feed one RTT sample to path
static void set_rtt(Path &path, uint64_t rtt_us)
{
    path.get_rtt_stats()->update_rtt(rtt_us, 0);
}

// Fill path congestion window
static void fill_window(Path &path)
{
    while(path.sending_allowed()){
        assert(path.get_sent_packet_handler()->sent_packet(path.pop_packet_number(), data_frames(), 1350, T0));
    }
}

void test_single_path()
{
    cout<<"[TEST] Single path selection\n";

    Path p0(0, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
    map<uint8_t, Path *> paths={{0, &p0}};
    LowestRttScheduler scheduler;
    assert(scheduler.get_name()=="lowest-rtt");

    assert(scheduler.select_path(paths, false)==&p0);

    // Single path is used even if potentially failed
    p0.set_potentially_failed(true);
    assert(scheduler.select_path(paths, false)==&p0);

    // Full window blocks new data, not retransmissions
    fill_window(p0);
    assert(scheduler.select_path(paths, false)==nullptr);
    assert(scheduler.select_path(paths, true)==&p0);

    map<uint8_t, Path *> no_paths;
    assert(scheduler.select_path(no_paths, false)==nullptr);

    cout<<"  -> PASS\n";
}

void test_unknown_rtt()
{
    cout<<"[TEST] Unknown RTT picks path with fewest packets\n";

    Path p0(0, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
    Path p1(1, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
    map<uint8_t, Path *> paths={{0, &p0}, {1, &p1}};
    LowestRttScheduler scheduler;

    // Tie: lower Path ID wins
    assert(scheduler.select_path(paths, false)==&p0);

    scheduler.on_packet_sent(&p0);
    assert(scheduler.packets_placed_on(0)==1&&scheduler.packets_placed_on(1)==0);
    assert(scheduler.select_path(paths, false)==&p1);

    scheduler.on_packet_sent(&p1);
    assert(scheduler.select_path(paths, false)==&p0); // tie again

    scheduler.on_packet_sent(&p1);
    scheduler.on_packet_sent(&p1);
    assert(scheduler.select_path(paths, false)==&p0);

    cout<<"  -> PASS\n";
}

void test_lowest_rtt()
{
    cout<<"[TEST] Lowest smoothed RTT wins over unknown or slower RTT\n";

    Path p0(0, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
    Path p1(1, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
    map<uint8_t, Path *> paths={{0, &p0}, {1, &p1}};
    LowestRttScheduler scheduler;

    // Measured path beats unmeasured path
    set_rtt(p1, 80000);
    assert(scheduler.select_path(paths, false)==&p1);

    // Packet count ignored once RTT is known
    for(int i=0; i<10; i++)
        scheduler.on_packet_sent(&p1);
    assert(scheduler.select_path(paths, false)==&p1);

    // Faster path 0 wins once measured
    set_rtt(p0, 20000);
    assert(scheduler.select_path(paths, false)==&p0);

    // Lower RTT wins regardless of Path ID
    Path q0(0, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
    Path q1(1, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
    map<uint8_t, Path *> qpaths={{0, &q0}, {1, &q1}};
    set_rtt(q0, 50000);
    set_rtt(q1, 15000);
    assert(scheduler.select_path(qpaths, false)==&q1);

    // Equal RTT: lower Path ID wins
    Path r0(0, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
    Path r1(1, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
    map<uint8_t, Path *> rpaths={{0, &r0}, {1, &r1}};
    set_rtt(r0, 30000);
    set_rtt(r1, 30000);
    assert(scheduler.select_path(rpaths, false)==&r0);

    cout<<"  -> PASS\n";
}

void test_congestion_window()
{
    cout<<"[TEST] Full cwnd skipped for new data, allowed for resends\n";

    Path p0(0, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
    Path p1(1, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
    map<uint8_t, Path *> paths={{0, &p0}, {1, &p1}};
    LowestRttScheduler scheduler;

    set_rtt(p0, 10000); // fast
    set_rtt(p1, 60000); // slow
    assert(scheduler.select_path(paths, false)==&p0);

    // Fast full -> new data moves to slow
    fill_window(p0);
    assert(scheduler.select_path(paths, false)==&p1);

    // Retransmission still takes fast path
    assert(scheduler.select_path(paths, true)==&p0);

    // Both full -> only retransmissions allowed
    fill_window(p1);
    assert(scheduler.select_path(paths, false)==nullptr);
    assert(scheduler.select_path(paths, true)==&p0);

    cout<<"  -> PASS\n";
}

void test_potentially_failed()
{
    cout<<"[TEST] Potentially failed path is skipped\n";

    Path p0(0, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
    Path p1(1, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
    map<uint8_t, Path *> paths={{0, &p0}, {1, &p1}};
    LowestRttScheduler scheduler;

    set_rtt(p0, 10000);
    set_rtt(p1, 60000);

    // Fast path broken -> use slow path
    p0.set_potentially_failed(true);
    assert(scheduler.select_path(paths, false)==&p1);
    assert(scheduler.select_path(paths, true)==&p1);

    // Slow path full, fast path broken -> wait
    fill_window(p1);
    assert(scheduler.select_path(paths, false)==nullptr);
    assert(scheduler.select_path(paths, true)==&p1);

    // Fast path recovered
    p0.set_potentially_failed(false);
    assert(scheduler.select_path(paths, false)==&p0);

    // All paths broken -> keep trying lowest RTT
    Path q0(0, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
    Path q1(1, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
    map<uint8_t, Path *> qpaths={{0, &q0}, {1, &q1}};
    set_rtt(q0, 40000);
    set_rtt(q1, 25000);
    q0.set_potentially_failed(true);
    q1.set_potentially_failed(true);
    assert(scheduler.select_path(qpaths, false)==&q1);

    cout<<"  -> PASS\n";
}

void test_duplicate_path()
{
    cout<<"[TEST] Startup duplication while RTT is unknown\n";

    Path p0(0, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
    Path p1(1, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
    map<uint8_t, Path *> paths={{0, &p0}, {1, &p1}};
    LowestRttScheduler scheduler;

    // Packet on path 0 -> duplicate on path 1
    scheduler.on_packet_sent(&p0);
    assert(scheduler.select_duplicate_path(paths, &p0)==&p1);

    // Same packet count now -> no extra copy
    scheduler.on_packet_sent(&p1);
    assert(scheduler.select_duplicate_path(paths, &p0)==nullptr);
    assert(scheduler.select_duplicate_path(paths, &p1)==nullptr);

    // Next packet on path 0 -> duplicate again
    scheduler.on_packet_sent(&p0);
    assert(scheduler.select_duplicate_path(paths, &p0)==&p1);

    // Skip broken path for duplicate
    p1.set_potentially_failed(true);
    assert(scheduler.select_duplicate_path(paths, &p0)==nullptr);
    p1.set_potentially_failed(false);

    // Stop duplicating once RTT is known
    set_rtt(p0, 30000);
    assert(scheduler.select_duplicate_path(paths, &p0)==nullptr);

    // Duplicate needs cwnd room on target path
    Path q0(0, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
    Path q1(1, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
    map<uint8_t, Path *> qpaths={{0, &q0}, {1, &q1}};
    LowestRttScheduler scheduler2;
    scheduler2.on_packet_sent(&q0);
    fill_window(q1);
    assert(scheduler2.select_duplicate_path(qpaths, &q0)==nullptr);

    // Single path -> no duplicate
    map<uint8_t, Path *> single={{0, &q0}};
    assert(scheduler2.select_duplicate_path(single, &q0)==nullptr);
    assert(scheduler2.select_duplicate_path(qpaths, nullptr)==nullptr);

    cout<<"  -> PASS\n";
}

int main()
{
    cout<<"=== Scheduler Tests ===\n";
    test_single_path();
    test_unknown_rtt();
    test_lowest_rtt();
    test_congestion_window();
    test_potentially_failed();
    test_duplicate_path();
    cout<<"=== All Scheduler Tests Passed ===\n";
    return 0;
}
