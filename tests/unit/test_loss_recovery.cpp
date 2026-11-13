#include <iostream>
#include <cassert>
#include <vector>
#include <string>

#include "packet/frame.hpp"
#include "reliability/rtt_stats.hpp"
#include "congestion_control/congestion.hpp"
#include "path/path.hpp"
#include "reliability/send_stream.hpp"
#include "reliability/receive_stream.hpp"
#include "reliability/stream_frame_sorter.hpp"
#include "flow_control/flow_controller.hpp"
#include "reliability/sent_packet_handler.hpp"

// All the times given to the classes are made up (microseconds), so the tests do not depend on the real clock
const uint64_t T0 = 1000000;

static std::vector<uint8_t> bytes(const std::string &s)
{
    return std::vector<uint8_t>(s.begin(), s.end());
}

static std::string text(const std::vector<uint8_t> &v)
{
    return std::string(v.begin(), v.end());
}

// Helper: frames of a data packet
static std::vector<Frame> data_frames()
{
    std::vector<Frame> frames;
    frames.push_back(Frame::make_stream(1, 0, false, std::vector<uint8_t>(100, 'x')));
    return frames;
}

// ---------------------------------------------------------------------------
// Receiver: reorder buffer
// ---------------------------------------------------------------------------

void test_sorter_reordering()
{
    std::cout << "[TEST] Reorder buffer: out-of-order frames are delivered in order...\n";

    StreamFrameSorter sorter;
    uint64_t offset = 0;
    std::vector<uint8_t> data;

    assert(!sorter.pop(offset, data));

    // Frames arrive in the order 2, 3, 1
    assert(sorter.push(5, bytes("56789")));
    assert(sorter.push(10, bytes("ABCDE")));
    assert(!sorter.pop(offset, data)); // byte 0 is still missing
    assert(sorter.get_buffered_bytes() == 10);
    assert(sorter.get_read_position() == 0);

    assert(sorter.push(0, bytes("01234")));

    assert(sorter.pop(offset, data));
    assert(offset == 0 && text(data) == "01234");
    assert(sorter.pop(offset, data));
    assert(offset == 5 && text(data) == "56789");
    assert(sorter.pop(offset, data));
    assert(offset == 10 && text(data) == "ABCDE");
    assert(!sorter.pop(offset, data));

    assert(sorter.get_read_position() == 15);
    assert(sorter.get_buffered_bytes() == 0);

    std::cout << "  -> PASSED\n";
}

void test_sorter_duplicates_and_overlaps()
{
    std::cout << "[TEST] Reorder buffer: duplicates and overlapping frames...\n";

    StreamFrameSorter sorter;
    uint64_t offset = 0;
    std::vector<uint8_t> data;

    // Stream content: "0123456789ABCDEFGHIJ" (20 bytes)
    assert(sorter.push(5, bytes("56789")));
    assert(!sorter.push(5, bytes("56789"))); // exact duplicate
    assert(!sorter.push(6, bytes("678")));   // completely inside what we have
    assert(sorter.get_buffered_bytes() == 5);

    // Overlaps the beginning of the buffered chunk: only "34" is new
    assert(sorter.push(3, bytes("3456")));
    assert(sorter.get_buffered_bytes() == 7);

    // Overlaps the end of the buffered chunk: only "AB" is new
    assert(sorter.push(8, bytes("89AB")));
    assert(sorter.get_buffered_bytes() == 9);

    // A separate chunk further away
    assert(sorter.push(15, bytes("FGH")));

    // One big frame that covers everything: it only fills the holes [0,3), [12,15) and [18,20)
    assert(sorter.push(0, bytes("0123456789ABCDEFGHIJ")));
    assert(sorter.get_buffered_bytes() == 20);

    std::string all;
    uint64_t expected_offset = 0;
    while (sorter.pop(offset, data))
    {
        assert(offset == expected_offset); // no hole, no byte twice
        expected_offset += data.size();
        all += text(data);
    }
    assert(all == "0123456789ABCDEFGHIJ");

    // Data below the read position was already delivered
    assert(!sorter.push(0, bytes("01234")));
    assert(!sorter.push(15, bytes("FGHIJ")));

    // A frame that starts in delivered data: only the new part is kept
    assert(sorter.push(18, bytes("IJKL")));
    assert(sorter.pop(offset, data));
    assert(offset == 20 && text(data) == "KL");

    std::cout << "  -> PASSED\n";
}

void test_receive_stream_fin()
{
    std::cout << "[TEST] Receive stream: in-order delivery and end of stream (FIN)...\n";

    uint64_t offset = 0;
    std::vector<uint8_t> data;
    bool fin = false;

    // 1. The last frame (with the FIN) arrives first
    {
        ReceiveStream stream(1);
        StreamFrame last;
        last.stream_id = 1;
        last.offset = 5;
        last.fin = true;
        last.data = bytes("56789");
        stream.add_stream_frame(last);
        assert(!stream.pop(offset, data, fin));

        StreamFrame first;
        first.stream_id = 1;
        first.offset = 0;
        first.data = bytes("01234");
        stream.add_stream_frame(first);

        assert(stream.pop(offset, data, fin));
        assert(offset == 0 && text(data) == "01234" && !fin);
        assert(!stream.is_finished());
        assert(stream.pop(offset, data, fin));
        assert(offset == 5 && text(data) == "56789" && fin);
        assert(stream.is_finished());
        assert(!stream.pop(offset, data, fin));
    }

    // 2. All the data is delivered, then a frame with only the FIN arrives
    {
        ReceiveStream stream(1);
        StreamFrame f;
        f.stream_id = 1;
        f.offset = 0;
        f.data = bytes("01234");
        stream.add_stream_frame(f);
        assert(stream.pop(offset, data, fin));
        assert(!fin);

        StreamFrame fin_only;
        fin_only.stream_id = 1;
        fin_only.offset = 5;
        fin_only.fin = true;
        stream.add_stream_frame(fin_only);
        assert(stream.pop(offset, data, fin));
        assert(offset == 5 && data.empty() && fin);
        assert(stream.is_finished());
    }

    // 3. A retransmitted frame with the FIN overlaps data we already have: it is cut,
    //    but the stream must still end at the right place
    {
        ReceiveStream stream(1);
        StreamFrame tail;
        tail.stream_id = 1;
        tail.offset = 5;
        tail.fin = true;
        tail.data = bytes("56789");
        stream.add_stream_frame(tail);

        StreamFrame big; // retransmission that was built differently: bytes 2..9 + FIN
        big.stream_id = 1;
        big.offset = 2;
        big.fin = true;
        big.data = bytes("23456789");
        stream.add_stream_frame(big);

        StreamFrame head;
        head.stream_id = 1;
        head.offset = 0;
        head.data = bytes("01");
        stream.add_stream_frame(head);

        std::string all;
        int fin_count = 0;
        while (stream.pop(offset, data, fin))
        {
            assert(offset == all.size());
            all += text(data);
            if (fin)
                fin_count++;
            else
                assert(all.size() < 10); // the FIN must come with the last byte, not before
        }
        assert(all == "0123456789");
        assert(fin_count == 1);
        assert(stream.is_finished());
    }

    std::cout << "  -> PASSED\n";
}

// ---------------------------------------------------------------------------
// Flow control
// ---------------------------------------------------------------------------

void test_flow_controller_sender()
{
    std::cout << "[TEST] Flow control: the sender stops at the advertised window...\n";

    FlowController fc(5, true);
    assert(fc.send_window_size() == RECEIVE_STREAM_FLOW_CONTROL_WINDOW);

    fc.add_bytes_sent(RECEIVE_STREAM_FLOW_CONTROL_WINDOW);
    assert(fc.send_window_size() == 0); // blocked

    // A WINDOW_UPDATE moves the window forward
    assert(fc.update_send_window(65536));
    assert(fc.send_window_size() == 65536 - RECEIVE_STREAM_FLOW_CONTROL_WINDOW);

    // An old WINDOW_UPDATE that arrives late never moves the window backwards
    assert(!fc.update_send_window(40000));
    assert(fc.get_send_window() == 65536);

    // SendStream obeys the window it is given
    SendStream stream(5);
    std::vector<uint8_t> data(3000, 'd');
    stream.write(data.data(), data.size());
    stream.close();

    Frame f;
    assert(stream.pop_stream_frame(1332, f, 500));
    assert(f.stream.data.size() == 500 && !f.stream.fin);
    assert(!stream.pop_stream_frame(1332, f, 0)); // no credit: nothing, not even the FIN
    assert(stream.pop_stream_frame(1332, f, 100000));
    assert(f.stream.offset == 500 && f.stream.data.size() == 1316);
    assert(stream.pop_stream_frame(1332, f, 1184));
    assert(f.stream.data.size() == 1184 && f.stream.fin); // exactly the credit that was left

    std::cout << "  -> PASSED\n";
}

void test_flow_controller_receiver()
{
    std::cout << "[TEST] Flow control: WINDOW_UPDATE generation and auto-tuning...\n";

    FlowController fc(5, true);
    uint64_t new_increment = 0;
    uint64_t new_offset = 0;
    const uint64_t window = RECEIVE_STREAM_FLOW_CONTROL_WINDOW; // 32768

    // Highest received offset: only grows
    assert(fc.update_highest_received(1000) == 1000);
    assert(fc.update_highest_received(500) == 0); // reordered frame
    assert(fc.update_highest_received(27000) == 26000);
    assert(!fc.check_flow_control_violation());

    // More than half of the increment is still available: no WINDOW_UPDATE
    fc.add_bytes_read(10000, T0);
    assert(!fc.maybe_update_window(T0, 0, new_increment, new_offset));
    assert(new_offset == window);

    // Less than half is left: the window moves to bytes_read + increment
    fc.add_bytes_read(7000, T0);
    assert(fc.maybe_update_window(T0, 0, new_increment, new_offset));
    assert(new_offset == 17000 + window);
    assert(new_increment == 0); // increment unchanged (no RTT known)
    assert(fc.get_receive_window() == 17000 + window);

    // Immediately afterwards nothing more to send
    assert(!fc.maybe_update_window(T0, 0, new_increment, new_offset));

    // The next update is needed only 1 ms later, much less than 2 RTTs (RTT = 50 ms):
    // the window is too small for this connection, so the increment doubles
    fc.add_bytes_read(20000, T0 + 1000);
    assert(fc.maybe_update_window(T0 + 1000, 50000, new_increment, new_offset));
    assert(new_increment == 2 * window);
    assert(new_offset == 37000 + 2 * window);

    // The following update comes after a long time: the increment stays
    fc.add_bytes_read(50000, T0 + 500000);
    assert(fc.maybe_update_window(T0 + 500000, 50000, new_increment, new_offset));
    assert(new_increment == 0);
    assert(fc.get_receive_window_increment() == 2 * window);
    assert(new_offset == 87000 + 2 * window);

    // The peer sends beyond the advertised window: violation
    FlowController strict(7, true);
    strict.update_highest_received(window);
    assert(!strict.check_flow_control_violation());
    strict.update_highest_received(window + 1);
    assert(strict.check_flow_control_violation());

    std::cout << "  -> PASSED\n";
}

void test_flow_control_manager()
{
    std::cout << "[TEST] Flow control: stream level and connection level together...\n";

    const uint64_t stream_window = RECEIVE_STREAM_FLOW_CONTROL_WINDOW;   // 32768
    const uint64_t conn_window = RECEIVE_CONNECTION_FLOW_CONTROL_WINDOW; // 49152

    // ---- Sender side ----
    FlowControlManager sender;
    assert(sender.send_window_size(1) == stream_window);

    sender.add_bytes_sent(1, stream_window);
    assert(sender.send_window_size(1) == 0); // stream 1 is blocked

    // Another stream is only limited by what is left of the connection window
    assert(sender.send_window_size(3) == conn_window - stream_window);

    // WINDOW_UPDATE for stream 1: now the connection window is the limit
    assert(sender.update_window(1, 100000));
    assert(sender.send_window_size(1) == conn_window - stream_window);

    // WINDOW_UPDATE for the connection (stream 0)
    assert(sender.update_window(0, 200000));
    assert(sender.send_window_size(1) == 100000 - stream_window);

    // ---- Receiver side ----
    FlowControlManager receiver;
    assert(receiver.update_highest_received(1, stream_window));
    assert(!receiver.update_highest_received(1, stream_window + 1)); // stream window exceeded

    // Two streams inside their own window, but together beyond the connection window
    FlowControlManager receiver2;
    assert(receiver2.update_highest_received(1, 30000));
    assert(!receiver2.update_highest_received(3, 30000));

    // WINDOW_UPDATE frames
    FlowControlManager receiver3;
    assert(receiver3.update_highest_received(1, 20000));
    receiver3.add_bytes_read(1, 20000, T0);

    std::vector<WindowUpdateFrame> updates = receiver3.get_window_updates(T0, 0);
    assert(updates.size() == 1); // only the stream, the connection window still has room
    assert(updates[0].stream_id == 1);
    assert(updates[0].byte_offset == 20000 + stream_window);
    assert(receiver3.get_receive_window(1) == 20000 + stream_window);
    assert(receiver3.get_receive_window(0) == conn_window);

    assert(receiver3.update_highest_received(1, 30000));
    receiver3.add_bytes_read(1, 10000, T0 + 1000);
    updates = receiver3.get_window_updates(T0 + 1000, 0);
    assert(updates.size() == 1); // now the connection window is more than half used
    assert(updates[0].stream_id == 0);
    assert(updates[0].byte_offset == 30000 + conn_window);

    assert(receiver3.get_window_updates(T0 + 1000, 0).empty());

    std::cout << "  -> PASSED\n";
}

// ---------------------------------------------------------------------------
// Sender: loss detection and retransmission timers
// ---------------------------------------------------------------------------

void test_time_based_loss_detection()
{
    std::cout << "[TEST] Loss detection: packet below the largest acked, older than 9/8 RTT...\n";

    // 1. The missing packet is not old enough when the ACK arrives: a loss timer is set
    {
        RttStats rtt;
        FixedWindowCongestion congestion;
        SentPacketHandler h(&rtt, &congestion);

        for (uint64_t pn = 1; pn <= 4; pn++)
            assert(h.sent_packet(pn, data_frames(), 1000, T0 + (pn - 1) * 1000));

        // Packets 1 and 3 are acked, 2 is missing. Packet 3 was sent at T0 + 2000 -> RTT = 40 ms
        AckFrame ack;
        ack.largest_acked = 3;
        ack.ranges = {{3, 3}, {1, 1}};
        assert(h.received_ack(ack, 1, T0 + 2000 + 40000));
        assert(rtt.get_smoothed_rtt() == 40000);

        // Packet 2 (sent at T0 + 1000) is 41 ms old, the limit is 45 ms: wait 4 more ms
        assert(h.get_losses() == 0);
        assert(!h.has_retransmissions());
        assert(h.get_num_outstanding() == 2); // 2 and 4
        assert(h.get_alarm_timeout() == T0 + 1000 + 45000);

        // The loss timer fires: packet 2 is lost. Packet 4 is above the largest acked, it is not judged.
        h.on_alarm(T0 + 1000 + 45000 + 1);
        assert(h.get_losses() == 1);
        assert(h.get_num_outstanding() == 1);
        assert(h.get_bytes_in_flight() == 1000);
        assert(h.has_retransmissions());

        SentPacket lost;
        assert(h.dequeue_packet_for_retransmission(lost));
        assert(lost.packet_number == 2);
        assert(lost.frames.size() == 1 && lost.frames[0].type == FRAME_STREAM);
        assert(h.get_retransmissions() == 1);
        assert(!h.dequeue_packet_for_retransmission(lost));

        // Next alarm: tail loss probe for packet 4 (last sent at T0 + 3000, one packet outstanding -> 2 * RTT)
        assert(h.get_alarm_timeout() == T0 + 3000 + 80000);

        // A late ACK for the packet we declared lost is harmless
        AckFrame late;
        late.largest_acked = 3;
        late.ranges = {{1, 3}};
        assert(h.received_ack(late, 2, T0 + 60000));
        assert(h.get_num_outstanding() == 1);
    }

    // 2. The missing packet is already old enough: it is lost as soon as the ACK is processed
    {
        RttStats rtt;
        FixedWindowCongestion congestion;
        SentPacketHandler h(&rtt, &congestion);

        assert(h.sent_packet(1, data_frames(), 1000, T0));
        assert(h.sent_packet(2, data_frames(), 1000, T0 + 1000));
        assert(h.sent_packet(3, data_frames(), 1000, T0 + 20000));

        AckFrame ack;
        ack.largest_acked = 3;
        ack.ranges = {{3, 3}, {1, 1}};
        assert(h.received_ack(ack, 1, T0 + 60000)); // RTT = 40 ms, packet 2 is 59 ms old

        assert(h.get_losses() == 1);
        assert(h.has_retransmissions());
        assert(h.get_num_outstanding() == 0);
        assert(h.get_bytes_in_flight() == 0);
        assert(h.get_alarm_timeout() == 0); // nothing outstanding, no alarm
    }

    std::cout << "  -> PASSED\n";
}

void test_tail_loss_probe_and_rto()
{
    std::cout << "[TEST] Timers: 2 tail loss probes, then RTO with exponential backoff...\n";

    RttStats rtt;
    FixedWindowCongestion congestion;
    SentPacketHandler h(&rtt, &congestion);

    bool path_failed = false;
    int rto_calls = 0;
    uint64_t rto_last_sent_time = 0;
    h.set_rto_callback([&](uint64_t last_sent_time_us) {
        rto_calls++;
        rto_last_sent_time = last_sent_time_us;
        return path_failed;
    });

    assert(h.get_alarm_timeout() == 0);

    // Packets 1..6 sent 1 ms apart
    for (uint64_t pn = 1; pn <= 6; pn++)
        assert(h.sent_packet(pn, data_frames(), 1000, T0 + (pn - 1) * 1000));

    // No RTT sample yet: default RTO of 500 ms after the last packet
    assert(h.get_alarm_timeout() == T0 + 5000 + DEFAULT_RTO_TIMEOUT_US);

    // ACK for packet 1 -> RTT 50 ms (deviation 25 ms). Then silence: 2..6 stay outstanding.
    AckFrame ack;
    ack.largest_acked = 1;
    ack.ranges = {{1, 1}};
    assert(h.received_ack(ack, 1, T0 + 50000));
    assert(rtt.get_smoothed_rtt() == 50000 && rtt.get_mean_deviation() == 25000);

    // Tail loss probe timer: max(2 * RTT, 1.5 * RTT + 100 ms) = 175 ms after the last packet
    assert(h.get_alarm_timeout() == T0 + 5000 + 175000);

    // 1st probe: the newest packet (6) is sent again. It is not counted as a loss.
    h.on_alarm(T0 + 5000 + 175000);
    SentPacket p;
    assert(h.dequeue_packet_for_retransmission(p));
    assert(p.packet_number == 6);
    assert(h.get_losses() == 0);
    assert(h.get_num_outstanding() == 4);
    assert(rto_calls == 0);

    // 2nd probe: packet 5
    h.on_alarm(T0 + 400000);
    assert(h.dequeue_packet_for_retransmission(p));
    assert(p.packet_number == 5);
    assert(h.get_num_outstanding() == 3); // 2, 3, 4
    assert(rto_calls == 0);

    // Both probes are used: now the RTO. RTT + 4 * deviation = 150 ms, but at least 200 ms.
    assert(h.get_alarm_timeout() == T0 + 5000 + MIN_RTO_TIMEOUT_US);

    // 1st RTO, the path is still alive: only the two oldest packets are retransmitted
    h.on_alarm(T0 + 500000);
    assert(rto_calls == 1);
    assert(rto_last_sent_time == T0 + 5000);
    assert(h.dequeue_packet_for_retransmission(p) && p.packet_number == 2);
    assert(h.dequeue_packet_for_retransmission(p) && p.packet_number == 3);
    assert(!h.dequeue_packet_for_retransmission(p));
    assert(h.get_losses() == 2);
    assert(h.get_num_outstanding() == 1); // 4

    // Exponential backoff: the next RTO is twice as long, counted from this RTO
    assert(h.get_alarm_timeout() == T0 + 500000 + 2 * MIN_RTO_TIMEOUT_US);

    // 2nd RTO, this time the path is reported as potentially failed: everything is retransmitted
    assert(h.sent_packet(7, data_frames(), 1000, T0 + 600000));
    assert(h.sent_packet(8, data_frames(), 1000, T0 + 601000));
    assert(h.get_alarm_timeout() == T0 + 601000 + 2 * MIN_RTO_TIMEOUT_US);

    path_failed = true;
    h.on_alarm(T0 + 1100000);
    assert(rto_calls == 2);
    assert(h.dequeue_packet_for_retransmission(p) && p.packet_number == 4);
    assert(h.dequeue_packet_for_retransmission(p) && p.packet_number == 7);
    assert(h.dequeue_packet_for_retransmission(p) && p.packet_number == 8);
    assert(h.get_losses() == 5);
    assert(h.get_num_outstanding() == 0);
    assert(h.get_bytes_in_flight() == 0);
    assert(h.get_alarm_timeout() == 0);
    assert(h.get_retransmissions() == 7);

    // An ACK resets the probe and RTO counters: the next timer is a tail loss probe again
    assert(h.sent_packet(9, data_frames(), 1000, T0 + 2000000));
    assert(h.sent_packet(10, data_frames(), 1000, T0 + 2001000));
    AckFrame ack9;
    ack9.largest_acked = 9;
    ack9.ranges = {{9, 9}};
    assert(h.received_ack(ack9, 2, T0 + 2050000));
    assert(h.get_num_outstanding() == 1);
    uint64_t srtt = rtt.get_smoothed_rtt();
    assert(h.get_alarm_timeout() == T0 + 2001000 + 2 * srtt); // one packet outstanding -> 2 * RTT

    std::cout << "  -> PASSED\n";
}

void test_retransmissions_bypass_congestion_window()
{
    std::cout << "[TEST] Retransmissions are allowed when the congestion window is full...\n";

    RttStats rtt;
    FixedWindowCongestion congestion(1); // 1 packet = 1460 bytes
    SentPacketHandler h(&rtt, &congestion);

    for (uint64_t pn = 1; pn <= 3; pn++)
        assert(h.sent_packet(pn, data_frames(), 1350, T0 + pn));
    assert(!h.sending_allowed()); // 4050 bytes in flight

    // The timer fires and queues packet 3 for retransmission: 2700 bytes are still in flight
    h.on_alarm(T0 + 600000);
    assert(h.get_bytes_in_flight() == 2700);
    assert(h.has_retransmissions());
    assert(h.sending_allowed());

    SentPacket p;
    assert(h.dequeue_packet_for_retransmission(p));
    assert(!h.sending_allowed());

    std::cout << "  -> PASSED\n";
}

void test_path_potentially_failed()
{
    std::cout << "[TEST] Path is marked potentially failed by an RTO without network activity...\n";

    // 1. Nothing is ever received on the path
    {
        Path path(1, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
        SentPacketHandler *h = path.get_sent_packet_handler();
        assert(!path.is_potentially_failed());

        for (uint64_t pn = 1; pn <= 4; pn++)
            assert(h->sent_packet(path.pop_packet_number(), data_frames(), 1000, T0 + pn * 1000));

        h->on_alarm(T0 + 600000);  // probe
        h->on_alarm(T0 + 1200000); // probe
        assert(!path.is_potentially_failed());
        assert(h->get_num_outstanding() == 2);

        h->on_alarm(T0 + 1800000); // RTO
        assert(path.is_potentially_failed());

        // Everything that was outstanding on the path is given back for retransmission
        assert(h->get_num_outstanding() == 0);
        int count = 0;
        SentPacket p;
        while (h->dequeue_packet_for_retransmission(p))
            count++;
        assert(count == 4);

        // The path is usable again once the Connection sees a packet on it
        path.set_potentially_failed(false);
        assert(!path.is_potentially_failed());
    }

    // 2. Something was received after the last packet we sent: the path is alive, only a normal RTO
    {
        Path path(3, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
        SentPacketHandler *h = path.get_sent_packet_handler();

        for (uint64_t pn = 1; pn <= 4; pn++)
            assert(h->sent_packet(path.pop_packet_number(), data_frames(), 1000, T0 + pn * 1000));
        path.set_last_network_activity_time(T0 + 100000);

        h->on_alarm(T0 + 600000);
        h->on_alarm(T0 + 1200000);
        h->on_alarm(T0 + 1800000); // RTO
        assert(!path.is_potentially_failed());
        assert(h->get_num_outstanding() == 0); // the two oldest (the two newest went as probes)
    }

    // 3. The path breaks right after an ACK arrived: the first RTO still sees activity after the
    //    last packet, but the second RTO in a row without anything received marks the path
    {
        Path path(5, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
        SentPacketHandler *h = path.get_sent_packet_handler();

        for (uint64_t pn = 1; pn <= 8; pn++)
            assert(h->sent_packet(path.pop_packet_number(), data_frames(), 1000, T0 + pn * 1000));
        path.set_last_network_activity_time(T0 + 100000); // last packet ever received on this path

        h->on_alarm(T0 + 600000);  // probe
        h->on_alarm(T0 + 1200000); // probe
        h->on_alarm(T0 + 1800000); // 1st RTO: two oldest packets
        assert(!path.is_potentially_failed());
        assert(h->get_num_outstanding() == 4);

        h->on_alarm(T0 + 3000000); // 2nd RTO: nothing was received since the 1st one
        assert(path.is_potentially_failed());
        assert(h->get_num_outstanding() == 0); // everything is given back at once
        assert(h->get_losses() == 6);
    }

    std::cout << "  -> PASSED\n";
}

int main()
{
    std::cout << "=== Running MP-QUIC Mini Loss Recovery Tests (Phase 6 & 7) ===\n";
    test_sorter_reordering();
    test_sorter_duplicates_and_overlaps();
    test_receive_stream_fin();
    test_flow_controller_sender();
    test_flow_controller_receiver();
    test_flow_control_manager();
    test_time_based_loss_detection();
    test_tail_loss_probe_and_rto();
    test_retransmissions_bypass_congestion_window();
    test_path_potentially_failed();
    std::cout << "=== All Loss Recovery Tests Passed Successfully! ===\n";
    return 0;
}
