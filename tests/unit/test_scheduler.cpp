#include <iostream>
#include <cassert>
#include <vector>
#include <map>

#include "packet/frame.hpp"
#include "path/path.hpp"
#include "scheduler/scheduler.hpp"

// Tests of the lowest-RTT scheduler (Phase 8). The paths have no socket: the scheduler only
// looks at their state (smoothed RTT, congestion window, potentially failed), which the
// tests set directly.

const uint64_t T0 = 1000000;

// Helper: frames of a data packet
static std::vector<Frame> data_frames()
{
    std::vector<Frame> frames;
    frames.push_back(Frame::make_stream(1, 0, false, std::vector<uint8_t>(100, 'x')));
    return frames;
}

// Gives the path an RTT measurement (a path that never got an ACK has no RTT)
static void set_rtt(Path &path, uint64_t rtt_us)
{
    path.get_rtt_stats()->update_rtt(rtt_us, 0);
}

// Sends packets on the path until its congestion window is full
static void fill_window(Path &path)
{
    while (path.sending_allowed())
    {
        assert(path.get_sent_packet_handler()->sent_packet(path.pop_packet_number(), data_frames(), 1350, T0));
    }
}

void test_single_path()
{
    std::cout << "[TEST] Single path: it is used whenever its window has room...\n";

    Path p0(0, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
    std::map<uint8_t, Path *> paths = {{0, &p0}};
    LowestRttScheduler scheduler;
    assert(scheduler.get_name() == "lowest-rtt");

    assert(scheduler.select_path(paths, false) == &p0);

    // With a single path there is no choice, even if it looks broken
    p0.set_potentially_failed(true);
    assert(scheduler.select_path(paths, false) == &p0);

    // Window full: new data waits, a retransmission goes out anyway
    fill_window(p0);
    assert(scheduler.select_path(paths, false) == nullptr);
    assert(scheduler.select_path(paths, true) == &p0);

    std::map<uint8_t, Path *> no_paths;
    assert(scheduler.select_path(no_paths, false) == nullptr);

    std::cout << "  -> PASSED\n";
}

void test_unknown_rtt()
{
    std::cout << "[TEST] No RTT known yet: the path that sent the fewest packets is taken...\n";

    Path p0(0, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
    Path p1(1, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
    std::map<uint8_t, Path *> paths = {{0, &p0}, {1, &p1}};
    LowestRttScheduler scheduler;

    // Nothing sent anywhere: the lower Path ID wins the tie
    assert(scheduler.select_path(paths, false) == &p0);

    scheduler.on_packet_sent(&p0);
    assert(scheduler.packets_placed_on(0) == 1 && scheduler.packets_placed_on(1) == 0);
    assert(scheduler.select_path(paths, false) == &p1);

    scheduler.on_packet_sent(&p1);
    assert(scheduler.select_path(paths, false) == &p0); // tie again

    scheduler.on_packet_sent(&p1);
    scheduler.on_packet_sent(&p1);
    assert(scheduler.select_path(paths, false) == &p0);

    std::cout << "  -> PASSED\n";
}

void test_lowest_rtt()
{
    std::cout << "[TEST] Known RTTs: the lowest smoothed RTT wins, a known RTT beats an unknown one...\n";

    Path p0(0, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
    Path p1(1, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
    std::map<uint8_t, Path *> paths = {{0, &p0}, {1, &p1}};
    LowestRttScheduler scheduler;

    // Only path 1 has an RTT (even a large one): we know it works, path 0 is unknown
    set_rtt(p1, 80000);
    assert(scheduler.select_path(paths, false) == &p1);

    // The number of packets sent does not matter anymore once an RTT is known
    for (int i = 0; i < 10; i++)
        scheduler.on_packet_sent(&p1);
    assert(scheduler.select_path(paths, false) == &p1);

    // Now path 0 is measured too and it is faster
    set_rtt(p0, 20000);
    assert(scheduler.select_path(paths, false) == &p0);

    // The choice follows the RTT, not the Path ID
    Path q0(0, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
    Path q1(1, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
    std::map<uint8_t, Path *> qpaths = {{0, &q0}, {1, &q1}};
    set_rtt(q0, 50000);
    set_rtt(q1, 15000);
    assert(scheduler.select_path(qpaths, false) == &q1);

    // Same RTT on both: the lower Path ID
    Path r0(0, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
    Path r1(1, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
    std::map<uint8_t, Path *> rpaths = {{0, &r0}, {1, &r1}};
    set_rtt(r0, 30000);
    set_rtt(r1, 30000);
    assert(scheduler.select_path(rpaths, false) == &r0);

    std::cout << "  -> PASSED\n";
}

void test_congestion_window()
{
    std::cout << "[TEST] A path with a full congestion window is skipped, except for retransmissions...\n";

    Path p0(0, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
    Path p1(1, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
    std::map<uint8_t, Path *> paths = {{0, &p0}, {1, &p1}};
    LowestRttScheduler scheduler;

    set_rtt(p0, 10000); // fast path
    set_rtt(p1, 60000); // slow path
    assert(scheduler.select_path(paths, false) == &p0);

    // The fast path is full: new data goes on the slow path
    fill_window(p0);
    assert(scheduler.select_path(paths, false) == &p1);

    // A retransmission is not blocked by the window: it takes the fast path
    assert(scheduler.select_path(paths, true) == &p0);

    // Both full: nothing can be sent, except a retransmission
    fill_window(p1);
    assert(scheduler.select_path(paths, false) == nullptr);
    assert(scheduler.select_path(paths, true) == &p0);

    std::cout << "  -> PASSED\n";
}

void test_potentially_failed()
{
    std::cout << "[TEST] A potentially failed path is ignored...\n";

    Path p0(0, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
    Path p1(1, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
    std::map<uint8_t, Path *> paths = {{0, &p0}, {1, &p1}};
    LowestRttScheduler scheduler;

    set_rtt(p0, 10000);
    set_rtt(p1, 60000);

    // The fast path breaks: everything goes on the slow one, retransmissions too
    p0.set_potentially_failed(true);
    assert(scheduler.select_path(paths, false) == &p1);
    assert(scheduler.select_path(paths, true) == &p1);

    // The slow path is full and the fast one is broken: new data waits
    fill_window(p1);
    assert(scheduler.select_path(paths, false) == nullptr);
    assert(scheduler.select_path(paths, true) == &p1);

    // The path works again (the Connection clears the flag when a packet arrives on it)
    p0.set_potentially_failed(false);
    assert(scheduler.select_path(paths, false) == &p0);

    // Every path looks broken: we keep trying, with the usual rule (lowest RTT)
    Path q0(0, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
    Path q1(1, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
    std::map<uint8_t, Path *> qpaths = {{0, &q0}, {1, &q1}};
    set_rtt(q0, 40000);
    set_rtt(q1, 25000);
    q0.set_potentially_failed(true);
    q1.set_potentially_failed(true);
    assert(scheduler.select_path(qpaths, false) == &q1);

    std::cout << "  -> PASSED\n";
}

void test_duplicate_path()
{
    std::cout << "[TEST] Duplication on another path while the RTT of the chosen path is unknown...\n";

    Path p0(0, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
    Path p1(1, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
    std::map<uint8_t, Path *> paths = {{0, &p0}, {1, &p1}};
    LowestRttScheduler scheduler;

    // A packet was sent on path 0 (RTT unknown): path 1 did not carry it yet -> duplicate there
    scheduler.on_packet_sent(&p0);
    assert(scheduler.select_duplicate_path(paths, &p0) == &p1);

    // Once the duplicate is sent both paths carried the same number of packets: no second copy
    scheduler.on_packet_sent(&p1);
    assert(scheduler.select_duplicate_path(paths, &p0) == nullptr);
    assert(scheduler.select_duplicate_path(paths, &p1) == nullptr);

    // Next packet on path 0: duplicated again
    scheduler.on_packet_sent(&p0);
    assert(scheduler.select_duplicate_path(paths, &p0) == &p1);

    // No duplicate on a path that looks broken
    p1.set_potentially_failed(true);
    assert(scheduler.select_duplicate_path(paths, &p0) == nullptr);
    p1.set_potentially_failed(false);

    // As soon as the RTT of the chosen path is known, duplication stops
    set_rtt(p0, 30000);
    assert(scheduler.select_duplicate_path(paths, &p0) == nullptr);

    // A duplicate is ordinary traffic for the other path: it needs room in its window
    Path q0(0, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
    Path q1(1, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
    std::map<uint8_t, Path *> qpaths = {{0, &q0}, {1, &q1}};
    LowestRttScheduler scheduler2;
    scheduler2.on_packet_sent(&q0);
    fill_window(q1);
    assert(scheduler2.select_duplicate_path(qpaths, &q0) == nullptr);

    // With a single path there is nowhere to duplicate
    std::map<uint8_t, Path *> single = {{0, &q0}};
    assert(scheduler2.select_duplicate_path(single, &q0) == nullptr);
    assert(scheduler2.select_duplicate_path(qpaths, nullptr) == nullptr);

    std::cout << "  -> PASSED\n";
}

int main()
{
    std::cout << "=== Running MP-QUIC Mini Scheduler Tests (Phase 8) ===\n";
    test_single_path();
    test_unknown_rtt();
    test_lowest_rtt();
    test_congestion_window();
    test_potentially_failed();
    test_duplicate_path();
    std::cout << "=== All Scheduler Tests Passed Successfully! ===\n";
    return 0;
}
