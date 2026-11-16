#include<bits/stdc++.h>

#include "packet/frame.hpp"
#include "reliability/rtt_stats.hpp"
#include "congestion_control/congestion.hpp"
#include "congestion_control/hybrid_slow_start.hpp"
#include "congestion_control/olia_sender.hpp"
#include "path/path.hpp"

using namespace std;

// Manual unit tests for OLIA and HybridSlowStart using fake timestamps

const uint64_t T0=1000000;
const uint64_t MSS=CongestionDefaults::MSS_BYTES;

// Send and ack count packets (window_limited=true when cwnd is full)
static void send_and_ack(OliaSender &sender, uint64_t &packet_number, int count, bool window_limited)
{
    for(int i=0; i<count; i++){
        packet_number++;
        sender.on_packet_sent(T0, 0, packet_number, MSS, true);
        uint64_t bytes_in_flight=window_limited ? sender.get_congestion_window() : 0;
        sender.on_packet_acked(packet_number, MSS, bytes_in_flight);
    }
}

void test_hybrid_slow_start()
{
    cout<<"[TEST] HybridSlowStart exits on RTT rise before loss\n";

    // 1. Flat RTT keeps slow start going
    HybridSlowStart h;
    h.on_packet_sent(100);
    for(int i=0; i<8; i++)
        assert(!h.should_exit_slow_start(50000, 50000, 40));
    assert(h.is_started());
    h.on_packet_acked(101); // round closes
    assert(!h.is_started());

    // 2. Next round: 60ms vs 50ms min (> 56.25ms threshold) exits after 8 samples
    h.on_packet_sent(200);
    for(int i=0; i<7; i++)
        assert(!h.should_exit_slow_start(60000, 50000, 40));
    assert(h.should_exit_slow_start(60000, 50000, 40));
    assert(h.should_exit_slow_start(50000, 50000, 40)); // stays flagged

    // 3. Do not exit while cwnd < 16 packets
    HybridSlowStart small;
    for(int i=0; i<8; i++)
        assert(!small.should_exit_slow_start(60000, 50000, 10));
    assert(small.should_exit_slow_start(60000, 50000, 10));

    // 4. Min rise threshold is 4ms
    HybridSlowStart fast;
    for(int i=0; i<8; i++)
        assert(!fast.should_exit_slow_start(11000, 8000, 40)); // +3ms too small
    fast.restart();
    for(int i=0; i<7; i++)
        assert(!fast.should_exit_slow_start(13000, 8000, 40));
    assert(fast.should_exit_slow_start(13000, 8000, 40)); // +5ms ok

    // 5. Max rise threshold is 16ms
    HybridSlowStart slow;
    for(int i=0; i<7; i++)
        assert(!slow.should_exit_slow_start(420000, 400000, 40));
    assert(slow.should_exit_slow_start(420000, 400000, 40)); // +20ms > 16ms

    // 6. Reset on RTO
    h.restart();
    assert(!h.should_exit_slow_start(50000, 50000, 40));

    cout<<"  -> PASS\n";
}

void test_slow_start()
{
    cout<<"[TEST] Slow start: +1 packet per ACK when window is full\n";

    RttStats rtt;
    rtt.update_rtt(50000, 0);
    OliaSender sender(&rtt);
    assert(sender.get_name()=="olia");

    assert(sender.get_congestion_window_packets()==CongestionDefaults::INITIAL_WINDOW_PACKETS);
    assert(sender.get_congestion_window()==CongestionDefaults::INITIAL_WINDOW_PACKETS*MSS);
    assert(sender.get_slow_start_threshold_packets()==CongestionDefaults::MAX_WINDOW_PACKETS);
    assert(sender.in_slow_start());
    assert(!sender.in_recovery());

    uint64_t pn=0;
    send_and_ack(sender, pn, 10, true);
    assert(sender.get_congestion_window_packets()==42);

    // No growth when window is not limiting
    send_and_ack(sender, pn, 10, false);
    assert(sender.get_congestion_window_packets()==42);

    // Slow start grows when > half of cwnd is in flight
    pn++;
    sender.on_packet_sent(T0, 0, pn, MSS, true);
    sender.on_packet_acked(pn, MSS, sender.get_congestion_window()/2+1);
    assert(sender.get_congestion_window_packets()==43);

    // Pure ACK packet ignored
    sender.on_packet_sent(T0, 0, pn+1000, 50, false);
    assert(sender.get_congestion_window_packets()==43);

    // Capped at max_window
    OliaSender capped(&rtt, 32, 40);
    uint64_t pn2=0;
    send_and_ack(capped, pn2, 50, true);
    assert(capped.get_congestion_window_packets()==40);

    cout<<"  -> PASS\n";
}

void test_loss_and_recovery()
{
    cout<<"[TEST] Loss halves cwnd once per loss event; no growth in recovery\n";

    RttStats rtt;
    rtt.update_rtt(50000, 0);
    OliaSender sender(&rtt);

    // Ack 1..3 (cwnd 32->35), send 4..40
    uint64_t pn=0;
    send_and_ack(sender, pn, 3, true);
    assert(sender.get_congestion_window_packets()==35);
    for(pn=4; pn<=40; pn++)
        sender.on_packet_sent(T0, 0, pn, MSS, true);

    // Loss of 5 cuts cwnd 35->17
    sender.on_packet_lost(5, MSS, 0);
    assert(sender.get_congestion_window_packets()==17);
    assert(sender.get_slow_start_threshold_packets()==17);
    assert(!sender.in_slow_start());
    assert(sender.in_recovery());

    // Other losses in same flight do not cut again
    sender.on_packet_lost(6, MSS, 0);
    sender.on_packet_lost(30, MSS, 0);
    assert(sender.get_congestion_window_packets()==17);

    // Old ACKs stay in recovery (no growth)
    sender.on_packet_acked(10, MSS, sender.get_congestion_window());
    sender.on_packet_acked(40, MSS, sender.get_congestion_window());
    assert(sender.in_recovery());
    assert(sender.get_congestion_window_packets()==17);

    // ACK for packet sent after cut ends recovery
    sender.on_packet_sent(T0, 0, 41, MSS, true);
    sender.on_packet_acked(41, MSS, sender.get_congestion_window());
    assert(!sender.in_recovery());

    // Next loss event cuts 17->8
    for(pn=42; pn<=50; pn++)
        sender.on_packet_sent(T0, 0, pn, MSS, true);
    sender.on_packet_lost(45, MSS, 0);
    assert(sender.get_congestion_window_packets()==8);

    // Floor is 2 packets
    uint64_t next=51;
    for(int i=0; i<6; i++){
        sender.on_packet_sent(T0, 0, next, MSS, true);
        sender.on_packet_lost(next, MSS, 0);
        next++;
    }
    assert(sender.get_congestion_window_packets()==CongestionDefaults::MIN_WINDOW_PACKETS);

    cout<<"  -> PASS\n";
}

void test_retransmission_timeout()
{
    cout<<"[TEST] RTO resets cwnd to 2 packets and enters slow start\n";

    RttStats rtt;
    rtt.update_rtt(50000, 0);
    OliaSender sender(&rtt);

    uint64_t pn=0;
    send_and_ack(sender, pn, 8, true);
    assert(sender.get_congestion_window_packets()==40);
    for(uint64_t i=pn+1; i<=pn+10; i++)
        sender.on_packet_sent(T0, 0, i, MSS, true);

    // Loss cuts 40->20, then RTO resets to 2 with ssthresh=10
    sender.on_packet_lost(pn+1, MSS, 0);
    sender.on_packet_lost(pn+2, MSS, 0);
    assert(sender.get_congestion_window_packets()==20);
    sender.on_retransmission_timeout(true);

    assert(sender.get_congestion_window_packets()==CongestionDefaults::MIN_WINDOW_PACKETS);
    assert(sender.get_slow_start_threshold_packets()==10);
    assert(sender.in_slow_start());
    assert(!sender.in_recovery());
    assert(sender.get_smoothed_bytes_between_losses()==0); // coupling reset

    // Slow start back up to ssthresh=10
    pn+=10;
    send_and_ack(sender, pn, 8, true);
    assert(sender.get_congestion_window_packets()==10);
    assert(!sender.in_slow_start());

    // RTO with no resend changes nothing
    sender.on_retransmission_timeout(false);
    assert(sender.get_congestion_window_packets()==10);

    cout<<"  -> PASS\n";
}

void test_single_path_congestion_avoidance()
{
    cout<<"[TEST] OLIA single path: +1 packet per RTT\n";

    RttStats rtt;
    rtt.update_rtt(50000, 0);
    OliaSender sender(&rtt, 16);
    sender.exit_slow_start();
    assert(!sender.in_slow_start());

    // 17 ACKs add 1 packet (integer rounding)
    uint64_t pn=0;
    send_and_ack(sender, pn, 16, true);
    assert(sender.get_congestion_window_packets()==16);
    send_and_ack(sender, pn, 1, true);
    assert(sender.get_congestion_window_packets()==17);

    // No growth when window is unused
    send_and_ack(sender, pn, 100, false);
    assert(sender.get_congestion_window_packets()==17);

    // Single path has epsilon=0
    assert(sender.get_epsilon_num()==0);

    cout<<"  -> PASS\n";
}

void test_two_equal_paths()
{
    cout<<"[TEST] OLIA two equal paths: 4x slower growth per path\n";

    RttStats rtt_a, rtt_b;
    rtt_a.update_rtt(50000, 0);
    rtt_b.update_rtt(50000, 0);
    OliaSender a(&rtt_a, 16);
    OliaSender b(&rtt_b, 16);
    a.exit_slow_start();
    b.exit_slow_start();

    map<uint8_t, OliaSender *> senders={{0, &a}, {1, &b}};
    a.set_olia_senders(&senders);
    b.set_olia_senders(&senders);

    // Increase per ACK = 1/(4w) -> ~69 ACKs for +1 packet
    uint64_t pn=0;
    send_and_ack(a, pn, 68, true);
    assert(a.get_congestion_window_packets()==16);
    send_and_ack(a, pn, 1, true);
    assert(a.get_congestion_window_packets()==17);

    // ACKs on A do not change B
    assert(b.get_congestion_window_packets()==16);

    // Equal windows -> epsilon=0
    assert(a.get_epsilon_num()==0&&b.get_epsilon_num()==0);

    cout<<"  -> PASS\n";
}

void test_rtt_weighting()
{
    cout<<"[TEST] OLIA RTT weighting: shorter RTT path grows faster\n";

    RttStats rtt_fast, rtt_slow;
    rtt_fast.update_rtt(20000, 0);
    rtt_slow.update_rtt(100000, 0);
    OliaSender fast(&rtt_fast, 16);
    OliaSender slow(&rtt_slow, 16);
    fast.exit_slow_start();
    slow.exit_slow_start();

    map<uint8_t, OliaSender *> senders={{0, &fast}, {1, &slow}};
    fast.set_olia_senders(&senders);
    slow.set_olia_senders(&senders);

    // Fast path (~23 ACKs per packet)
    uint64_t pn=0;
    send_and_ack(fast, pn, 30, true);
    assert(fast.get_congestion_window_packets()==17);

    // Slow path (~580 ACKs per packet)
    uint64_t pn2=0;
    send_and_ack(slow, pn2, 300, true);
    assert(slow.get_congestion_window_packets()==16);

    cout<<"  -> PASS\n";
}

void test_epsilon_moves_window_to_the_best_path()
{
    cout<<"[TEST] OLIA epsilon shifts window from largest path to best path\n";

    RttStats rtt_a, rtt_b;
    rtt_a.update_rtt(50000, 0);
    rtt_b.update_rtt(50000, 0);

    // Path A: small cwnd (10) with high quality; Path B: largest cwnd (20) with lower quality
    OliaSender a(&rtt_a, 10);
    OliaSender b(&rtt_b, 18);
    a.exit_slow_start();
    b.exit_slow_start();

    // Grow B from 18 to 20 above ssthresh
    uint64_t pn_a=0, pn_b=0;
    while(b.get_congestion_window_packets()<20)
        send_and_ack(b, pn_b, 1, true);

    map<uint8_t, OliaSender *> senders={{0, &a}, {1, &b}};
    a.set_olia_senders(&senders);
    b.set_olia_senders(&senders);

    send_and_ack(a, pn_a, 685, false); // count bytes without growing cwnd
    assert(a.get_congestion_window_packets()==10&&b.get_congestion_window_packets()==20);
    assert(a.get_smoothed_bytes_between_losses()==685*MSS);
    assert(b.get_smoothed_bytes_between_losses()<100000);

    // A gets +1/2, B gets -1/2
    send_and_ack(a, pn_a, 1, true);
    assert(a.get_epsilon_num()==1&&a.get_epsilon_den()==2);
    assert(b.get_epsilon_num()==-1&&b.get_epsilon_den()==2);

    // A grows faster with positive epsilon
    send_and_ack(a, pn_a, 19, true);
    assert(a.get_congestion_window_packets()==11);

    // B shrinks slowly with negative epsilon (even without loss)
    send_and_ack(b, pn_b, 200, true);
    assert(b.get_congestion_window_packets()==20);
    send_and_ack(b, pn_b, 100, true);
    assert(b.get_congestion_window_packets()==19);
    assert(a.get_congestion_window_packets()==11);

    // Loss on B cuts only B
    for(uint64_t i=1; i<=5; i++)
        b.on_packet_sent(T0, 0, pn_b+i, MSS, true);
    b.on_packet_lost(pn_b+1, MSS, 0);
    assert(b.get_congestion_window_packets()==9);
    assert(a.get_congestion_window_packets()==11);

    cout<<"  -> PASS\n";
}

void test_very_different_rtts_do_not_overflow()
{
    cout<<"[TEST] OLIA 128-bit math with 100us vs 2s RTTs\n";

    // 100us vs 2s needs 128-bit math
    RttStats rtt_fast, rtt_slow;
    rtt_fast.update_rtt(100, 0);
    rtt_slow.update_rtt(2000000, 0);
    OliaSender fast(&rtt_fast, 100);
    OliaSender slow(&rtt_slow, 100);
    fast.exit_slow_start();
    slow.exit_slow_start();

    map<uint8_t, OliaSender *> senders={{0, &fast}, {1, &slow}};
    fast.set_olia_senders(&senders);
    slow.set_olia_senders(&senders);

    // Fast path grows ~1 packet per window (103 ACKs)
    uint64_t pn=0;
    send_and_ack(fast, pn, 102, true);
    assert(fast.get_congestion_window_packets()==100);
    send_and_ack(fast, pn, 1, true);
    assert(fast.get_congestion_window_packets()==101);

    // Slow path barely moves and does not overflow
    uint64_t pn2=0;
    send_and_ack(slow, pn2, 2000, true);
    assert(slow.get_congestion_window_packets()==100);

    // Large window: sub-1/1024 increments round to 0
    OliaSender big(&rtt_fast, 2000);
    big.exit_slow_start();
    uint64_t pn3=0;
    send_and_ack(big, pn3, 5000, true);
    assert(big.get_congestion_window_packets()==2000);

    cout<<"  -> PASS\n";
}

// Sample data frame list
static vector<Frame> data_frames()
{
    vector<Frame> frames;
    frames.push_back(Frame::make_stream(1, 0, false, vector<uint8_t>(100, 'x')));
    return frames;
}

void test_path_uses_olia()
{
    cout<<"[TEST] Path updates OliaSender on send and ACK\n";

    Path path(0, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
    OliaSender *olia=path.get_congestion();
    SentPacketHandler *handler=path.get_sent_packet_handler();
    assert(olia->get_name()=="olia");
    assert(olia->get_congestion_window_packets()==CongestionDefaults::INITIAL_WINDOW_PACKETS);

    // Fill 32-packet cwnd with 34 packets
    for(uint64_t pn=1; pn<=34; pn++)
        assert(handler->sent_packet(path.pop_packet_number(), data_frames(), MSS, T0+pn));
    assert(!path.sending_allowed());

    // ACK 1..10 -> slow start grows cwnd 32->42
    AckFrame ack;
    ack.largest_acked=10;
    ack.ranges={{1, 10}};
    assert(handler->received_ack(ack, 1, T0+50000));
    assert(olia->get_congestion_window_packets()==42);
    assert(path.sending_allowed());

    // Send 35..40 then ACK 40 -> 11..34 lost, cwnd halved once to 21
    for(uint64_t pn=35; pn<=40; pn++)
        assert(handler->sent_packet(path.pop_packet_number(), data_frames(), MSS, T0+100000));
    AckFrame ack2;
    ack2.largest_acked=40;
    ack2.ranges={{40, 40}, {1, 10}};
    assert(handler->received_ack(ack2, 2, T0+200000));

    assert(handler->get_losses()==24);
    uint64_t window_after_loss=olia->get_congestion_window_packets();
    assert(window_after_loss==21); // (42 + 1) / 2
    assert(olia->get_slow_start_threshold_packets()==21);
    assert(!olia->in_slow_start());
    assert(olia->in_recovery());

    cout<<"  -> PASS\n";
}

int main()
{
    cout<<"=== Congestion Control Tests ===\n";
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
    cout<<"=== All Congestion Control Tests Passed ===\n";
    return 0;
}
