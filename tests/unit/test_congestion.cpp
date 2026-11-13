#include <iostream>
#include <cassert>
#include <vector>
#include <map>

#include "packet/frame.hpp"
#include "reliability/rtt_stats.hpp"
#include "congestion_control/congestion.hpp"
#include "congestion_control/hybrid_slow_start.hpp"
#include "congestion_control/olia_sender.hpp"
#include "path/path.hpp"

// Tests of the congestion control (Phase 9 & 10): the window of one path (slow start,
// loss, timeout) and the coupling of the paths with OLIA. Everything is driven by hand,
// with made-up packets and times, so the results are exact numbers.

const uint64_t T0 = 1000000;
const uint64_t MSS = DEFAULT_TCP_MSS;

// One packet is sent and acknowledged.
// window_limited = the sender was filling its whole window (only then the window may grow).
static void send_and_ack(OliaSender &sender, uint64_t &packet_number, int count, bool window_limited)
{
    for (int i = 0; i < count; i++)
    {
        packet_number++;
        sender.on_packet_sent(T0, 0, packet_number, MSS, true);
        uint64_t bytes_in_flight = window_limited ? sender.get_congestion_window() : 0;
        sender.on_packet_acked(packet_number, MSS, bytes_in_flight);
    }
}

void test_hybrid_slow_start()
{
    std::cout << "[TEST] Hybrid slow start: a growing RTT ends slow start before any loss...\n";

    // 1. Stable RTT: slow start goes on
    HybridSlowStart h;
    h.on_packet_sent(100);
    for (int i = 0; i < 8; i++)
        assert(!h.should_exit_slow_start(50000, 50000, 40));
    assert(h.is_started());
    h.on_packet_acked(101); // a packet sent after the round began is acked: the round is over
    assert(!h.is_started());

    // 2. Next round: the RTT is 60 ms while the lowest ever seen is 50 ms.
    //    The limit is 50 + 50/8 = 56.25 ms, so after 8 samples slow start must stop.
    h.on_packet_sent(200);
    for (int i = 0; i < 7; i++)
        assert(!h.should_exit_slow_start(60000, 50000, 40));
    assert(h.should_exit_slow_start(60000, 50000, 40));
    assert(h.should_exit_slow_start(50000, 50000, 40)); // once found, it stays

    // 3. With a window below 16 packets the detection itself does not end slow start
    //    (the answer comes with the next RTT measurement, as in the reference implementation)
    HybridSlowStart small;
    for (int i = 0; i < 8; i++)
        assert(!small.should_exit_slow_start(60000, 50000, 10));
    assert(small.should_exit_slow_start(60000, 50000, 10));

    // 4. The increase must be at least 4 ms, also on a path with a very short RTT
    HybridSlowStart fast;
    for (int i = 0; i < 8; i++)
        assert(!fast.should_exit_slow_start(11000, 8000, 40)); // +3 ms: not enough
    fast.restart();
    for (int i = 0; i < 7; i++)
        assert(!fast.should_exit_slow_start(13000, 8000, 40));
    assert(fast.should_exit_slow_start(13000, 8000, 40)); // +5 ms

    // 5. ... and never more than 16 ms, also on a path with a very long RTT
    HybridSlowStart slow;
    for (int i = 0; i < 7; i++)
        assert(!slow.should_exit_slow_start(420000, 400000, 40));
    assert(slow.should_exit_slow_start(420000, 400000, 40)); // +20 ms > 16 ms (400/8 would be 50 ms)

    // 6. After a restart (retransmission timeout) everything starts again
    h.restart();
    assert(!h.should_exit_slow_start(50000, 50000, 40));

    std::cout << "  -> PASSED\n";
}

void test_slow_start()
{
    std::cout << "[TEST] Slow start: one more packet per ACK, only when the window is really used...\n";

    RttStats rtt;
    rtt.update_rtt(50000, 0);
    OliaSender sender(&rtt);
    assert(sender.get_name() == "olia");

    assert(sender.get_congestion_window_packets() == INITIAL_CONGESTION_WINDOW);
    assert(sender.get_congestion_window() == INITIAL_CONGESTION_WINDOW * MSS);
    assert(sender.get_slow_start_threshold_packets() == DEFAULT_MAX_CONGESTION_WINDOW);
    assert(sender.in_slow_start());
    assert(!sender.in_recovery());

    uint64_t pn = 0;
    send_and_ack(sender, pn, 10, true);
    assert(sender.get_congestion_window_packets() == 42);

    // The application has little to send: the window is not used, so it does not grow
    send_and_ack(sender, pn, 10, false);
    assert(sender.get_congestion_window_packets() == 42);

    // In slow start, using more than half of the window is enough
    pn++;
    sender.on_packet_sent(T0, 0, pn, MSS, true);
    sender.on_packet_acked(pn, MSS, sender.get_congestion_window() / 2 + 1);
    assert(sender.get_congestion_window_packets() == 43);

    // A packet with only an ACK frame is not congestion controlled
    sender.on_packet_sent(T0, 0, pn + 1000, 50, false);
    assert(sender.get_congestion_window_packets() == 43);

    // The window never passes the maximum
    OliaSender capped(&rtt, 32, 40);
    uint64_t pn2 = 0;
    send_and_ack(capped, pn2, 50, true);
    assert(capped.get_congestion_window_packets() == 40);

    std::cout << "  -> PASSED\n";
}

void test_loss_and_recovery()
{
    std::cout << "[TEST] Loss: the window is halved once per loss event, no growth during recovery...\n";

    RttStats rtt;
    rtt.update_rtt(50000, 0);
    OliaSender sender(&rtt);

    // Packets 1..3 are acked (window 32 -> 35), then 4..40 are sent
    uint64_t pn = 0;
    send_and_ack(sender, pn, 3, true);
    assert(sender.get_congestion_window_packets() == 35);
    for (pn = 4; pn <= 40; pn++)
        sender.on_packet_sent(T0, 0, pn, MSS, true);

    // Packet 5 is lost: window 35 -> 17, slow start is over
    sender.on_packet_lost(5, MSS, 0);
    assert(sender.get_congestion_window_packets() == 17);
    assert(sender.get_slow_start_threshold_packets() == 17);
    assert(!sender.in_slow_start());
    assert(sender.in_recovery());

    // Packets 6 and 30 were sent before the cut: same loss event, the window is not cut again
    sender.on_packet_lost(6, MSS, 0);
    sender.on_packet_lost(30, MSS, 0);
    assert(sender.get_congestion_window_packets() == 17);

    // ACKs for packets sent before the cut: we are still recovering, no growth
    sender.on_packet_acked(10, MSS, sender.get_congestion_window());
    sender.on_packet_acked(40, MSS, sender.get_congestion_window());
    assert(sender.in_recovery());
    assert(sender.get_congestion_window_packets() == 17);

    // A packet sent after the cut is acked: recovery is over
    sender.on_packet_sent(T0, 0, 41, MSS, true);
    sender.on_packet_acked(41, MSS, sender.get_congestion_window());
    assert(!sender.in_recovery());

    // A loss among newer packets is a new loss event: 17 -> 8
    for (pn = 42; pn <= 50; pn++)
        sender.on_packet_sent(T0, 0, pn, MSS, true);
    sender.on_packet_lost(45, MSS, 0);
    assert(sender.get_congestion_window_packets() == 8);

    // The window never goes below 2 packets
    uint64_t next = 51;
    for (int i = 0; i < 6; i++)
    {
        sender.on_packet_sent(T0, 0, next, MSS, true);
        sender.on_packet_lost(next, MSS, 0);
        next++;
    }
    assert(sender.get_congestion_window_packets() == DEFAULT_MINIMUM_CONGESTION_WINDOW);

    std::cout << "  -> PASSED\n";
}

void test_retransmission_timeout()
{
    std::cout << "[TEST] Retransmission timeout: back to 2 packets and slow start...\n";

    RttStats rtt;
    rtt.update_rtt(50000, 0);
    OliaSender sender(&rtt);

    uint64_t pn = 0;
    send_and_ack(sender, pn, 8, true);
    assert(sender.get_congestion_window_packets() == 40);
    for (uint64_t i = pn + 1; i <= pn + 10; i++)
        sender.on_packet_sent(T0, 0, i, MSS, true);

    // An RTO first reports the packets as lost (40 -> 20), then the timeout itself
    sender.on_packet_lost(pn + 1, MSS, 0);
    sender.on_packet_lost(pn + 2, MSS, 0);
    assert(sender.get_congestion_window_packets() == 20);
    sender.on_retransmission_timeout(true);

    assert(sender.get_congestion_window_packets() == DEFAULT_MINIMUM_CONGESTION_WINDOW);
    assert(sender.get_slow_start_threshold_packets() == 10);
    assert(sender.in_slow_start());
    assert(!sender.in_recovery());
    assert(sender.get_smoothed_bytes_between_losses() == 0); // the OLIA history is cleared

    // Slow start again, up to the threshold
    pn += 10;
    send_and_ack(sender, pn, 8, true);
    assert(sender.get_congestion_window_packets() == 10);
    assert(!sender.in_slow_start());

    // A timeout where nothing was retransmitted changes nothing
    sender.on_retransmission_timeout(false);
    assert(sender.get_congestion_window_packets() == 10);

    std::cout << "  -> PASSED\n";
}

void test_single_path_congestion_avoidance()
{
    std::cout << "[TEST] OLIA with one path: one more packet per window, like TCP...\n";

    RttStats rtt;
    rtt.update_rtt(50000, 0);
    OliaSender sender(&rtt, 16);
    sender.exit_slow_start();
    assert(!sender.in_slow_start());

    // With a window of 16 packets, about 16 ACKs (one round trip) give one more packet.
    // (17 exactly: every ACK adds 63/1024 of a packet because of the integer rounding.)
    uint64_t pn = 0;
    send_and_ack(sender, pn, 16, true);
    assert(sender.get_congestion_window_packets() == 16);
    send_and_ack(sender, pn, 1, true);
    assert(sender.get_congestion_window_packets() == 17);

    // No growth when the window is not used
    send_and_ack(sender, pn, 100, false);
    assert(sender.get_congestion_window_packets() == 17);

    // Alone, a path has nothing to trade with: epsilon is 0
    assert(sender.get_epsilon_num() == 0);

    std::cout << "  -> PASSED\n";
}

void test_two_equal_paths()
{
    std::cout << "[TEST] OLIA with two equal paths: each one grows 4 times slower than a single path...\n";

    RttStats rtt_a, rtt_b;
    rtt_a.update_rtt(50000, 0);
    rtt_b.update_rtt(50000, 0);
    OliaSender a(&rtt_a, 16);
    OliaSender b(&rtt_b, 16);
    a.exit_slow_start();
    b.exit_slow_start();

    std::map<uint8_t, OliaSender *> senders = {{0, &a}, {1, &b}};
    a.set_olia_senders(&senders);
    b.set_olia_senders(&senders);

    // Increase per ACK: w / (2w)^2 = 1 / (4w), so about 4 * 16 = 64 ACKs for one packet
    // (69 exactly, with the integer rounding). Together the two paths grow by half of
    // what one TCP connection would: the connection is not more aggressive than one TCP.
    uint64_t pn = 0;
    send_and_ack(a, pn, 68, true);
    assert(a.get_congestion_window_packets() == 16);
    send_and_ack(a, pn, 1, true);
    assert(a.get_congestion_window_packets() == 17);

    // The ACKs of path A do not change the window of path B
    assert(b.get_congestion_window_packets() == 16);

    // Same windows, so nothing to move between the paths
    assert(a.get_epsilon_num() == 0 && b.get_epsilon_num() == 0);

    std::cout << "  -> PASSED\n";
}

void test_rtt_weighting()
{
    std::cout << "[TEST] OLIA with different RTTs: the path with the short RTT grows faster...\n";

    RttStats rtt_fast, rtt_slow;
    rtt_fast.update_rtt(20000, 0);
    rtt_slow.update_rtt(100000, 0);
    OliaSender fast(&rtt_fast, 16);
    OliaSender slow(&rtt_slow, 16);
    fast.exit_slow_start();
    slow.exit_slow_start();

    std::map<uint8_t, OliaSender *> senders = {{0, &fast}, {1, &slow}};
    fast.set_olia_senders(&senders);
    slow.set_olia_senders(&senders);

    // Fast path: (w/20^2) / (w/20 + w/100)^2 = 1 / (1.44 w)  ->  about 23 ACKs for one packet
    uint64_t pn = 0;
    send_and_ack(fast, pn, 30, true);
    assert(fast.get_congestion_window_packets() == 17);

    // Slow path: (w/100^2) / (w/20 + w/100)^2 = 1 / (36 w)   ->  about 580 ACKs for one packet
    uint64_t pn2 = 0;
    send_and_ack(slow, pn2, 300, true);
    assert(slow.get_congestion_window_packets() == 16);

    std::cout << "  -> PASSED\n";
}

void test_epsilon_moves_window_to_the_best_path()
{
    std::cout << "[TEST] OLIA epsilon: window moves from the path with the largest window to the best path...\n";

    RttStats rtt_a, rtt_b;
    rtt_a.update_rtt(50000, 0);
    rtt_b.update_rtt(50000, 0);

    // Path A: small window (10) but good quality, about 1,000,000 bytes acked without a loss.
    // Path B: largest window (20) but poor quality, far fewer bytes.
    OliaSender a(&rtt_a, 10);
    OliaSender b(&rtt_b, 18);
    a.exit_slow_start();
    b.exit_slow_start();

    // B first grows alone from 18 to 20 packets, so that it is above its slow start
    // threshold (18), as a path is after some time in congestion avoidance
    uint64_t pn_a = 0, pn_b = 0;
    while (b.get_congestion_window_packets() < 20)
        send_and_ack(b, pn_b, 1, true);

    std::map<uint8_t, OliaSender *> senders = {{0, &a}, {1, &b}};
    a.set_olia_senders(&senders);
    b.set_olia_senders(&senders);

    send_and_ack(a, pn_a, 685, false); // not window limited: counts bytes, the window stays
    assert(a.get_congestion_window_packets() == 10 && b.get_congestion_window_packets() == 20);
    assert(a.get_smoothed_bytes_between_losses() == 685 * MSS);
    assert(b.get_smoothed_bytes_between_losses() < 100000);

    // The first window increase computes epsilon for both paths:
    // A is the best path and does not have the largest window -> +1/2
    // B has the largest window                                -> -1/2
    send_and_ack(a, pn_a, 1, true);
    assert(a.get_epsilon_num() == 1 && a.get_epsilon_den() == 2);
    assert(b.get_epsilon_num() == -1 && b.get_epsilon_den() == 2);

    // Path A now gains a packet in about 17 ACKs. Without epsilon its share of the
    // coupled increase alone would need about 90 ACKs.
    send_and_ack(a, pn_a, 19, true);
    assert(a.get_congestion_window_packets() == 11);

    // Path B does not grow anymore: its window slowly goes DOWN, although nothing was lost
    // (about 4/1024 of a packet per ACK, so one packet after about 256 ACKs)
    send_and_ack(b, pn_b, 200, true);
    assert(b.get_congestion_window_packets() == 20);
    send_and_ack(b, pn_b, 100, true);
    assert(b.get_congestion_window_packets() == 19);
    assert(a.get_congestion_window_packets() == 11);

    // A loss on path B halves B only
    for (uint64_t i = 1; i <= 5; i++)
        b.on_packet_sent(T0, 0, pn_b + i, MSS, true);
    b.on_packet_lost(pn_b + 1, MSS, 0);
    assert(b.get_congestion_window_packets() == 9);
    assert(a.get_congestion_window_packets() == 11);

    std::cout << "  -> PASSED\n";
}

void test_very_different_rtts_do_not_overflow()
{
    std::cout << "[TEST] OLIA with very different RTTs (the products need 128-bit arithmetic)...\n";

    // 100 microseconds against 2 seconds: the squares used by OLIA do not fit in 64 bits
    RttStats rtt_fast, rtt_slow;
    rtt_fast.update_rtt(100, 0);
    rtt_slow.update_rtt(2000000, 0);
    OliaSender fast(&rtt_fast, 100);
    OliaSender slow(&rtt_slow, 100);
    fast.exit_slow_start();
    slow.exit_slow_start();

    std::map<uint8_t, OliaSender *> senders = {{0, &fast}, {1, &slow}};
    fast.set_olia_senders(&senders);
    slow.set_olia_senders(&senders);

    // The fast path behaves almost like a single path: one packet per window
    // (103 ACKs with the integer rounding)
    uint64_t pn = 0;
    send_and_ack(fast, pn, 102, true);
    assert(fast.get_congestion_window_packets() == 100);
    send_and_ack(fast, pn, 1, true);
    assert(fast.get_congestion_window_packets() == 101);

    // The slow path practically does not grow, and its window does not jump anywhere
    uint64_t pn2 = 0;
    send_and_ack(slow, pn2, 2000, true);
    assert(slow.get_congestion_window_packets() == 100);

    // Very large windows: every ACK adds less than 1/1024 of a packet, which the integer
    // arithmetic of the reference implementation rounds to nothing. The window stays.
    OliaSender big(&rtt_fast, 2000);
    big.exit_slow_start();
    uint64_t pn3 = 0;
    send_and_ack(big, pn3, 5000, true);
    assert(big.get_congestion_window_packets() == 2000);

    std::cout << "  -> PASSED\n";
}

// Helper: frames of a data packet
static std::vector<Frame> data_frames()
{
    std::vector<Frame> frames;
    frames.push_back(Frame::make_stream(1, 0, false, std::vector<uint8_t>(100, 'x')));
    return frames;
}

void test_path_uses_olia()
{
    std::cout << "[TEST] A path drives its OLIA sender from the packets it sends and the ACKs it gets...\n";

    Path path(0, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
    OliaSender *olia = path.get_congestion();
    SentPacketHandler *handler = path.get_sent_packet_handler();
    assert(olia->get_name() == "olia");
    assert(olia->get_congestion_window_packets() == INITIAL_CONGESTION_WINDOW);

    // 34 full packets: the window (32 packets) is full
    for (uint64_t pn = 1; pn <= 34; pn++)
        assert(handler->sent_packet(path.pop_packet_number(), data_frames(), MSS, T0 + pn));
    assert(!path.sending_allowed());

    // ACK for packets 1..10: slow start, +1 packet per acked packet
    AckFrame ack;
    ack.largest_acked = 10;
    ack.ranges = {{1, 10}};
    assert(handler->received_ack(ack, 1, T0 + 50000));
    assert(olia->get_congestion_window_packets() == 42);
    assert(path.sending_allowed());

    // Packets 35..40 are sent later, then an ACK for 40 only: 11..39 are missing.
    // The old ones (11..34) are declared lost -> one loss event, the window is halved once.
    for (uint64_t pn = 35; pn <= 40; pn++)
        assert(handler->sent_packet(path.pop_packet_number(), data_frames(), MSS, T0 + 100000));
    AckFrame ack2;
    ack2.largest_acked = 40;
    ack2.ranges = {{40, 40}, {1, 10}};
    assert(handler->received_ack(ack2, 2, T0 + 200000));

    assert(handler->get_losses() == 24);
    uint64_t window_after_loss = olia->get_congestion_window_packets();
    assert(window_after_loss == 21); // (42 + 1 for the ACK of packet 40) / 2
    assert(olia->get_slow_start_threshold_packets() == 21);
    assert(!olia->in_slow_start());
    assert(olia->in_recovery());

    std::cout << "  -> PASSED\n";
}

int main()
{
    std::cout << "=== Running MP-QUIC Mini Congestion Control Tests (Phase 9 & 10) ===\n";
    test_hybrid_slow_start();
    test_slow_start();
    test_loss_and_recovery();
    test_retransmission_timeout();
    test_single_path_congestion_avoidance();
    test_two_equal_paths();
    test_rtt_weighting();
    test_epsilon_moves_window_to_the_best_path();
    test_very_different_rtts_do_not_overflow();
    test_path_uses_olia();
    std::cout << "=== All Congestion Control Tests Passed Successfully! ===\n";
    return 0;
}
