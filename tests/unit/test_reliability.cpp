#include <iostream>
#include <cassert>
#include <vector>

#include "packet/frame.hpp"
#include "reliability/rtt_stats.hpp"
#include "congestion_control/congestion.hpp"
#include "reliability/send_stream.hpp"
#include "reliability/sent_packet_handler.hpp"
#include "reliability/received_packet_handler.hpp"

// All the times given to the handlers are made up (microseconds), so the tests do not depend on the real clock
const uint64_t T0 = 1000000;

void test_rtt_stats()
{
    std::cout << "[TEST] RTT estimation (smoothed RTT, deviation, ack delay)...\n";

    RttStats rtt;
    assert(rtt.get_smoothed_rtt() == 0);

    // First sample: smoothed = sample, deviation = sample / 2
    rtt.update_rtt(100000, 0);
    assert(rtt.get_smoothed_rtt() == 100000);
    assert(rtt.get_mean_deviation() == 50000);
    assert(rtt.get_latest_rtt() == 100000);
    assert(rtt.get_min_rtt() == 100000);

    // Second sample: 60 ms measured, receiver held the packet 10 ms -> sample = 50 ms
    rtt.update_rtt(60000, 10000);
    assert(rtt.get_latest_rtt() == 50000);
    assert(rtt.get_mean_deviation() == (3 * 50000 + 50000) / 4); // |100000 - 50000| = 50000
    assert(rtt.get_smoothed_rtt() == (7 * 100000 + 50000) / 8);  // 93750
    assert(rtt.get_min_rtt() == 60000);                          // min uses the raw measurement

    // Ack delay larger than the measurement -> the delay is ignored
    rtt.update_rtt(5000, 8000);
    assert(rtt.get_latest_rtt() == 5000);

    // A zero measurement is ignored
    uint64_t before = rtt.get_smoothed_rtt();
    rtt.update_rtt(0, 0);
    assert(rtt.get_smoothed_rtt() == before);

    std::cout << "  -> PASSED\n";
}

void test_send_stream_chunking()
{
    std::cout << "[TEST] Chunking application data into STREAM frames...\n";

    SendStream stream(7);
    assert(!stream.has_data());

    std::vector<uint8_t> data(3000);
    for (size_t i = 0; i < data.size(); i++)
        data[i] = static_cast<uint8_t>(i % 251);
    stream.write(data.data(), data.size());
    assert(stream.has_data());

    Frame f;
    size_t max_frame = 1332; // room left in a packet after the 18-byte header
    size_t chunk = max_frame - STREAM_FRAME_HEADER_SIZE;

    // No room for even one byte of data
    assert(!stream.pop_stream_frame(STREAM_FRAME_HEADER_SIZE, f));
    assert(!stream.pop_stream_frame(5, f));

    assert(stream.pop_stream_frame(max_frame, f));
    assert(f.type == FRAME_STREAM);
    assert(f.stream.stream_id == 7);
    assert(f.stream.offset == 0);
    assert(f.stream.data.size() == chunk);
    assert(!f.stream.fin);
    assert(f.size() == max_frame);

    assert(stream.pop_stream_frame(max_frame, f));
    assert(f.stream.offset == chunk);
    assert(f.stream.data.size() == chunk);
    assert(f.stream.data[0] == data[chunk]);

    // Closing the stream: the last frame carries the FIN
    stream.close();
    assert(stream.pop_stream_frame(max_frame, f));
    assert(f.stream.offset == 2 * chunk);
    assert(f.stream.data.size() == 3000 - 2 * chunk);
    assert(f.stream.data.back() == data.back());
    assert(f.stream.fin);
    assert(stream.get_next_offset() == 3000);

    assert(!stream.has_data());
    assert(!stream.pop_stream_frame(max_frame, f));

    // A stream closed without data sends one empty frame with the FIN
    SendStream empty_stream(9);
    empty_stream.close();
    assert(empty_stream.has_data());
    assert(empty_stream.pop_stream_frame(max_frame, f));
    assert(f.stream.fin && f.stream.data.empty() && f.stream.offset == 0);
    assert(!empty_stream.has_data());

    std::cout << "  -> PASSED\n";
}

void test_received_packet_handler_ack_rules()
{
    std::cout << "[TEST] ACK generation rules (immediate, every 2 packets, delayed)...\n";

    ReceivedPacketHandler h;
    AckFrame ack;

    // Packet number 0 does not exist
    assert(!h.received_packet(0, true, T0));
    assert(!h.get_ack_frame(T0, ack));

    // The first packet is acked immediately
    assert(h.received_packet(1, true, T0));
    assert(h.get_ack_frame(T0, ack));
    assert(ack.largest_acked == 1);
    assert(ack.ranges.size() == 1 && ack.ranges[0].first == 1 && ack.ranges[0].last == 1);
    assert(!h.get_ack_frame(T0, ack)); // nothing new to ack

    // One data packet alone: delayed ACK timer (25 ms)
    assert(h.received_packet(2, true, T0 + 1000));
    assert(h.get_alarm_timeout() == T0 + 1000 + ACK_SEND_DELAY_US);
    assert(!h.get_ack_frame(T0 + 10000, ack));
    assert(h.get_ack_frame(T0 + 1000 + ACK_SEND_DELAY_US, ack));
    assert(ack.largest_acked == 2);
    assert(ack.ack_delay_us == ACK_SEND_DELAY_US); // the time we held packet 2
    assert(ack.ranges.size() == 1 && ack.ranges[0].first == 1 && ack.ranges[0].last == 2);
    assert(h.get_alarm_timeout() == 0);

    // Two data packets: ACK immediately after the second one
    assert(h.received_packet(3, true, T0 + 50000));
    assert(!h.get_ack_frame(T0 + 50000, ack));
    assert(h.received_packet(4, true, T0 + 51000));
    assert(h.get_ack_frame(T0 + 51000, ack));
    assert(ack.largest_acked == 4);
    assert(ack.ack_delay_us == 0);

    // A packet with only an ACK frame does not make us send an ACK
    assert(h.received_packet(5, false, T0 + 60000));
    assert(h.get_alarm_timeout() == 0);
    assert(!h.get_ack_frame(T0 + 200000, ack));

    std::cout << "  -> PASSED\n";
}

void test_received_packet_handler_ranges()
{
    std::cout << "[TEST] Received packet number ranges (gaps, merging, duplicates)...\n";

    ReceivedPacketHandler h;
    AckFrame ack;

    for (uint64_t pn = 1; pn <= 5; pn++)
        assert(h.received_packet(pn, true, T0));
    assert(h.get_ack_frame(T0, ack));
    assert(ack.ranges.size() == 1 && ack.lowest_acked() == 1 && ack.largest_acked == 5);

    // Packets 6 and 7 are missing: the new hole is reported immediately
    assert(h.received_packet(8, true, T0 + 1000));
    assert(h.get_ack_frame(T0 + 1000, ack));
    assert(ack.largest_acked == 8);
    assert(ack.ranges.size() == 2);
    assert(ack.ranges[0].first == 8 && ack.ranges[0].last == 8); // highest range first
    assert(ack.ranges[1].first == 1 && ack.ranges[1].last == 5);

    // A packet that was reported missing arrives late: acked immediately
    assert(h.received_packet(6, true, T0 + 2000));
    assert(h.get_ack_frame(T0 + 2000, ack));
    assert(ack.largest_acked == 8);
    assert(ack.ack_delay_us == 1000); // packet 8 (the largest) arrived 1 ms ago
    assert(ack.ranges.size() == 2);
    assert(ack.ranges[1].first == 1 && ack.ranges[1].last == 6);

    // Packet 7 fills the hole: the two ranges are merged
    assert(h.received_packet(7, true, T0 + 3000));
    assert(h.get_ack_frame(T0 + 3000, ack));
    assert(ack.ranges.size() == 1);
    assert(ack.ranges[0].first == 1 && ack.ranges[0].last == 8);
    assert(h.get_num_ranges() == 1);

    // A range can also grow downwards
    assert(h.received_packet(12, true, T0 + 4000));
    assert(h.received_packet(11, true, T0 + 4000));
    assert(h.get_ack_frame(T0 + 4000, ack));
    assert(ack.ranges.size() == 2);
    assert(ack.ranges[0].first == 11 && ack.ranges[0].last == 12);

    // Duplicates
    assert(h.is_duplicate(1) && h.is_duplicate(8) && h.is_duplicate(11));
    assert(!h.is_duplicate(9) && !h.is_duplicate(10) && !h.is_duplicate(13));
    assert(h.received_packet(3, true, T0 + 5000)); // duplicate does not change the ranges
    assert(h.get_num_ranges() == 2);
    assert(h.get_largest_observed() == 12);

    std::cout << "  -> PASSED\n";
}

void test_received_packet_handler_range_limit()
{
    std::cout << "[TEST] At most 256 ACK ranges are kept...\n";

    ReceivedPacketHandler h;

    // Only odd packet numbers arrive: every packet is its own range
    for (uint64_t i = 0; i < 300; i++)
        assert(h.received_packet(2 * i + 1, true, T0));

    assert(h.get_num_ranges() == MAX_ACK_RANGES);

    AckFrame ack;
    assert(h.get_ack_frame(T0, ack));
    assert(ack.ranges.size() == MAX_ACK_RANGES);
    assert(ack.largest_acked == 599);
    assert(ack.lowest_acked() == 2 * (300 - 256) + 1); // the 44 lowest ranges were dropped

    // A dropped packet number is accepted but not put back in the ACK
    assert(h.received_packet(1, true, T0));
    assert(h.get_num_ranges() == MAX_ACK_RANGES);

    std::cout << "  -> PASSED\n";
}

// Helper: frames of a data packet
static std::vector<Frame> data_frames()
{
    std::vector<Frame> frames;
    frames.push_back(Frame::make_stream(1, 0, false, std::vector<uint8_t>(100, 'x')));
    return frames;
}

void test_sent_packet_handler()
{
    std::cout << "[TEST] Sent packet tracking and ACK processing...\n";

    RttStats rtt;
    FixedWindowCongestion congestion;
    SentPacketHandler h(&rtt, &congestion);

    // Packets 1..5, 1000 bytes each, sent 1 ms apart
    for (uint64_t pn = 1; pn <= 5; pn++)
        assert(h.sent_packet(pn, data_frames(), 1000, T0 + pn * 1000));

    assert(h.get_bytes_in_flight() == 5000);
    assert(h.get_num_outstanding() == 5);
    assert(h.get_least_unacked() == 1);

    // Packet numbers must increase
    assert(!h.sent_packet(3, data_frames(), 1000, T0 + 9000));

    // A packet with only an ACK frame is not tracked
    AckFrame some_ack;
    some_ack.largest_acked = 1;
    some_ack.ranges = {{1, 1}};
    std::vector<Frame> ack_only;
    ack_only.push_back(Frame::make_ack(some_ack));
    assert(h.sent_packet(6, ack_only, 50, T0 + 9000));
    assert(h.get_bytes_in_flight() == 5000);
    assert(h.get_num_outstanding() == 5);
    assert(h.get_packets_sent() == 6);

    // ACK for a packet that was never sent is invalid
    AckFrame bad;
    bad.largest_acked = 10;
    bad.ranges = {{1, 10}};
    assert(!h.received_ack(bad, 1, T0 + 50000));
    assert(h.get_bytes_in_flight() == 5000);

    // ACK for packets 1 and 3 (2 is missing), carried by the peer's packet 1.
    // Packet 3 was sent at T0 + 3000, the ACK arrives 50 ms later, receiver held it 2 ms.
    AckFrame ack1;
    ack1.largest_acked = 3;
    ack1.ack_delay_us = 2000;
    ack1.ranges = {{3, 3}, {1, 1}};
    assert(h.received_ack(ack1, 1, T0 + 3000 + 50000));
    assert(rtt.get_latest_rtt() == 48000);
    assert(rtt.get_smoothed_rtt() == 48000);
    assert(h.get_bytes_in_flight() == 3000);
    assert(h.get_num_outstanding() == 3); // 2, 4, 5
    assert(h.get_largest_acked() == 3);
    assert(h.get_least_unacked() == 2);

    // An older ACK that arrives late (carried by a packet number we already saw) is ignored
    AckFrame ack_old;
    ack_old.largest_acked = 5;
    ack_old.ranges = {{5, 5}};
    assert(h.received_ack(ack_old, 1, T0 + 60000));
    assert(h.get_bytes_in_flight() == 3000);

    // The same ACK carried by a newer packet is processed: 5 is acked, new RTT sample
    assert(h.received_ack(ack_old, 2, T0 + 5000 + 40000));
    assert(h.get_bytes_in_flight() == 2000);
    assert(rtt.get_latest_rtt() == 40000);
    assert(h.get_largest_acked() == 5);

    // ACK for everything: 2 and 4 are newly acked. Packet 5 (the largest) was already
    // acked, so there is no new RTT sample.
    AckFrame ack_all;
    ack_all.largest_acked = 5;
    ack_all.ranges = {{1, 5}};
    assert(h.received_ack(ack_all, 3, T0 + 500000));
    assert(rtt.get_latest_rtt() == 40000);
    assert(h.get_bytes_in_flight() == 0);
    assert(h.get_num_outstanding() == 0);
    assert(h.get_least_unacked() == 6);

    // A repeated ACK changes nothing
    assert(h.received_ack(ack_all, 4, T0 + 600000));
    assert(h.get_bytes_in_flight() == 0);

    std::cout << "  -> PASSED\n";
}

void test_sending_allowed()
{
    std::cout << "[TEST] Sending stops when the congestion window is full...\n";

    RttStats rtt;
    FixedWindowCongestion congestion(4); // 4 packets = 5840 bytes
    SentPacketHandler h(&rtt, &congestion);
    assert(congestion.get_congestion_window() == 4 * DEFAULT_TCP_MSS);

    uint64_t pn = 1;
    while (h.sending_allowed())
    {
        assert(h.sent_packet(pn, data_frames(), 1350, T0 + pn));
        pn++;
    }
    // 5 packets of 1350 bytes = 6750 bytes > 5840
    assert(h.get_num_outstanding() == 5);
    assert(h.get_bytes_in_flight() == 5 * 1350);

    // An ACK frees the window again
    AckFrame ack;
    ack.largest_acked = 2;
    ack.ranges = {{1, 2}};
    assert(h.received_ack(ack, 1, T0 + 30000));
    assert(h.get_bytes_in_flight() == 3 * 1350);
    assert(h.sending_allowed());

    std::cout << "  -> PASSED\n";
}

int main()
{
    std::cout << "=== Running MP-QUIC Mini Reliability Tests (Phase 4 & 5) ===\n";
    test_rtt_stats();
    test_send_stream_chunking();
    test_received_packet_handler_ack_rules();
    test_received_packet_handler_ranges();
    test_received_packet_handler_range_limit();
    test_sent_packet_handler();
    test_sending_allowed();
    std::cout << "=== All Reliability Tests Passed Successfully! ===\n";
    return 0;
}
