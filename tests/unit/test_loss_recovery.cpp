#include<bits/stdc++.h>

#include "packet/frame.hpp"
#include "reliability/rtt_stats.hpp"
#include "congestion_control/congestion.hpp"
#include "path/path.hpp"
#include "reliability/send_stream.hpp"
#include "reliability/receive_stream.hpp"
#include "reliability/stream_frame_sorter.hpp"
#include "flow_control/flow_controller.hpp"
#include "reliability/sent_packet_handler.hpp"

using namespace std;

// Fake clock times in microseconds
const uint64_t T0=1000000;

static vector<uint8_t> bytes(const string &s)
{
    return vector<uint8_t>(s.begin(), s.end());
}

static string text(const vector<uint8_t> &v)
{
    return string(v.begin(), v.end());
}

// Sample data frame list
static vector<Frame> data_frames()
{
    vector<Frame> frames;
    frames.push_back(Frame::make_stream(1, 0, false, vector<uint8_t>(100, 'x')));
    return frames;
}

// Receiver: reorder buffer

void test_sorter_reordering()
{
    cout<<"[TEST] StreamFrameSorter delivers out-of-order frames in order\n";

    StreamFrameSorter sorter;
    uint64_t offset=0;
    vector<uint8_t> data;

    assert(!sorter.pop(offset, data));

    // Push in order 2, 3, 1
    assert(sorter.push(5, bytes("56789")));
    assert(sorter.push(10, bytes("ABCDE")));
    assert(!sorter.pop(offset, data)); // missing byte 0
    assert(sorter.get_buffered_bytes()==10);
    assert(sorter.get_read_position()==0);

    assert(sorter.push(0, bytes("01234")));

    assert(sorter.pop(offset, data));
    assert(offset==0&&text(data)=="01234");
    assert(sorter.pop(offset, data));
    assert(offset==5&&text(data)=="56789");
    assert(sorter.pop(offset, data));
    assert(offset==10&&text(data)=="ABCDE");
    assert(!sorter.pop(offset, data));

    assert(sorter.get_read_position()==15);
    assert(sorter.get_buffered_bytes()==0);

    cout<<"  -> PASS\n";
}

void test_sorter_duplicates_and_overlaps()
{
    cout<<"[TEST] StreamFrameSorter trims duplicates and overlaps\n";

    StreamFrameSorter sorter;
    uint64_t offset=0;
    vector<uint8_t> data;

    // Stream content: "0123456789ABCDEFGHIJ" (20 bytes)
    assert(sorter.push(5, bytes("56789")));
    assert(!sorter.push(5, bytes("56789"))); // exact duplicate
    assert(!sorter.push(6, bytes("678")));   // inside buffered chunk
    assert(sorter.get_buffered_bytes()==5);

    // Overlap start: only "34" is new
    assert(sorter.push(3, bytes("3456")));
    assert(sorter.get_buffered_bytes()==7);

    // Overlap end: only "AB" is new
    assert(sorter.push(8, bytes("89AB")));
    assert(sorter.get_buffered_bytes()==9);

    assert(sorter.push(15, bytes("FGH")));

    // Full frame fills only remaining gaps [0,3), [12,15), [18,20)
    assert(sorter.push(0, bytes("0123456789ABCDEFGHIJ")));
    assert(sorter.get_buffered_bytes()==20);

    string all;
    uint64_t expected_offset=0;
    while(sorter.pop(offset, data)){
        assert(offset==expected_offset);
        expected_offset+=data.size();
        all+=text(data);
    }
    assert(all=="0123456789ABCDEFGHIJ");

    // Bytes already delivered are ignored
    assert(!sorter.push(0, bytes("01234")));
    assert(!sorter.push(15, bytes("FGHIJ")));

    // Overlapping read position keeps only new tail
    assert(sorter.push(18, bytes("IJKL")));
    assert(sorter.pop(offset, data));
    assert(offset==20&&text(data)=="KL");

    cout<<"  -> PASS\n";
}

void test_receive_stream_fin()
{
    cout<<"[TEST] ReceiveStream in-order delivery and FIN\n";

    uint64_t offset=0;
    vector<uint8_t> data;
    bool fin=false;

    // 1. FIN frame arrives first
    {
        ReceiveStream stream(1);
        StreamFrame last;
        last.stream_id=1;
        last.offset=5;
        last.fin=true;
        last.data=bytes("56789");
        stream.add_stream_frame(last);
        assert(!stream.pop(offset, data, fin));

        StreamFrame first;
        first.stream_id=1;
        first.offset=0;
        first.data=bytes("01234");
        stream.add_stream_frame(first);

        assert(stream.pop(offset, data, fin));
        assert(offset==0&&text(data)=="01234"&&!fin);
        assert(!stream.is_finished());
        assert(stream.pop(offset, data, fin));
        assert(offset==5&&text(data)=="56789"&&fin);
        assert(stream.is_finished());
        assert(!stream.pop(offset, data, fin));
    }

    // 2. Data delivered first, FIN-only frame arrives later
    {
        ReceiveStream stream(1);
        StreamFrame f;
        f.stream_id=1;
        f.offset=0;
        f.data=bytes("01234");
        stream.add_stream_frame(f);
        assert(stream.pop(offset, data, fin));
        assert(!fin);

        StreamFrame fin_only;
        fin_only.stream_id=1;
        fin_only.offset=5;
        fin_only.fin=true;
        stream.add_stream_frame(fin_only);
        assert(stream.pop(offset, data, fin));
        assert(offset==5&&data.empty()&&fin);
        assert(stream.is_finished());
    }

    // 3. Overlapping FIN retransmission still ends at right byte
    {
        ReceiveStream stream(1);
        StreamFrame tail;
        tail.stream_id=1;
        tail.offset=5;
        tail.fin=true;
        tail.data=bytes("56789");
        stream.add_stream_frame(tail);

        StreamFrame big; // overlapping resend 2..9 + FIN
        big.stream_id=1;
        big.offset=2;
        big.fin=true;
        big.data=bytes("23456789");
        stream.add_stream_frame(big);

        StreamFrame head;
        head.stream_id=1;
        head.offset=0;
        head.data=bytes("01");
        stream.add_stream_frame(head);

        string all;
        int fin_count=0;
        while(stream.pop(offset, data, fin)){
            assert(offset==all.size());
            all+=text(data);
            if(fin)
                fin_count++;
            else
                assert(all.size()<10); // FIN only with last byte
        }
        assert(all=="0123456789");
        assert(fin_count==1);
        assert(stream.is_finished());
    }

    cout<<"  -> PASS\n";
}

// Reject frames that conflict with stream end offset
void test_receive_stream_end_is_fixed()
{
    cout<<"[TEST] ReceiveStream rejects moved FIN or data past FIN\n";

    auto frame=[](uint64_t offset, const string &content, bool fin) {
        StreamFrame f;
        f.stream_id=1;
        f.offset=offset;
        f.data=bytes(content);
        f.fin=fin;
        return f;
    };

    // 1. Conflicting FIN offset
    {
        ReceiveStream stream(1);
        assert(stream.add_stream_frame(frame(5, "56789", true)));
        assert(!stream.add_stream_frame(frame(5, "567", true)));      // ends at 8 != 10
        assert(!stream.add_stream_frame(frame(5, "56789AB", true)));  // ends at 12 != 10
        assert(stream.add_stream_frame(frame(5, "56789", true)));     // same end: ok
        assert(stream.add_stream_frame(frame(2, "23456789", true)));  // same end: ok
    }

    // 2. Data past FIN
    {
        ReceiveStream stream(1);
        assert(stream.add_stream_frame(frame(0, "01234", true)));
        assert(!stream.add_stream_frame(frame(3, "345", false)));     // past 5
        assert(stream.add_stream_frame(frame(1, "1234", false)));     // inside: ok
    }

    // 3. FIN before already received bytes
    {
        ReceiveStream stream(1);
        assert(stream.add_stream_frame(frame(5, "56789", false)));    // up to 10
        assert(!stream.add_stream_frame(frame(0, "0123", true)));     // FIN at 4 < 10
        assert(stream.add_stream_frame(frame(10, "", true)));         // FIN at 10: ok
    }

    cout<<"  -> PASS\n";
}

// Flow control

void test_flow_controller_sender()
{
    cout<<"[TEST] FlowControl sender limit\n";

    FlowController fc(5, true);
    assert(fc.send_window_size()==FlowWindows::STREAM_INITIAL);

    fc.add_bytes_sent(FlowWindows::STREAM_INITIAL);
    assert(fc.send_window_size()==0);

    // WINDOW_UPDATE moves limit forward
    assert(fc.update_send_window(65536));
    assert(fc.send_window_size()==65536-FlowWindows::STREAM_INITIAL);

    // Ignore older smaller WINDOW_UPDATE
    assert(!fc.update_send_window(40000));
    assert(fc.get_send_window()==65536);

    // SendStream stops at send_window
    SendStream stream(5);
    vector<uint8_t> data(3000, 'd');
    stream.write(data.data(), data.size());
    stream.close();

    Frame f;
    assert(stream.pop_stream_frame(1332, f, 500));
    assert(f.stream.data.size()==500&&!f.stream.fin);
    assert(!stream.pop_stream_frame(1332, f, 0));
    assert(stream.pop_stream_frame(1332, f, 100000));
    assert(f.stream.offset==500&&f.stream.data.size()==1316);
    assert(stream.pop_stream_frame(1332, f, 1184));
    assert(f.stream.data.size()==1184&&f.stream.fin);

    cout<<"  -> PASS\n";
}

void test_flow_controller_receiver()
{
    cout<<"[TEST] FlowControl WINDOW_UPDATE and auto-tuning\n";

    FlowController fc(5, true);
    uint64_t new_increment=0;
    uint64_t new_offset=0;
    const uint64_t window=FlowWindows::STREAM_INITIAL; // 32768

    // Highest received offset only increases
    assert(fc.update_highest_received(1000)==1000);
    assert(fc.update_highest_received(500)==0);
    assert(fc.update_highest_received(27000)==26000);
    assert(!fc.check_flow_control_violation());

    // Over half window left: no update
    fc.add_bytes_read(10000, T0);
    assert(!fc.maybe_update_window(T0, 0, new_increment, new_offset));
    assert(new_offset==window);

    // Under half left: send update
    fc.add_bytes_read(7000, T0);
    assert(fc.maybe_update_window(T0, 0, new_increment, new_offset));
    assert(new_offset==17000+window);
    assert(new_increment==0); // no RTT sample
    assert(fc.get_receive_window()==17000+window);

    assert(!fc.maybe_update_window(T0, 0, new_increment, new_offset));

    // Update within 2 RTTs (1ms < 100ms) doubles window_step
    fc.add_bytes_read(20000, T0+1000);
    assert(fc.maybe_update_window(T0+1000, 50000, new_increment, new_offset));
    assert(new_increment==2*window);
    assert(new_offset==37000+2*window);

    // Update after >= 2 RTTs keeps window_step
    fc.add_bytes_read(50000, T0+500000);
    assert(fc.maybe_update_window(T0+500000, 50000, new_increment, new_offset));
    assert(new_increment==0);
    assert(fc.get_receive_window_increment()==2*window);
    assert(new_offset==87000+2*window);

    // Peer past advertised limit -> violation
    FlowController strict(7, true);
    strict.update_highest_received(window);
    assert(!strict.check_flow_control_violation());
    strict.update_highest_received(window+1);
    assert(strict.check_flow_control_violation());

    cout<<"  -> PASS\n";
}

void test_flow_control_manager()
{
    cout<<"[TEST] FlowControlManager stream and connection limits\n";

    const uint64_t stream_window=FlowWindows::STREAM_INITIAL;   // 32768
    const uint64_t conn_window=FlowWindows::CONNECTION_INITIAL; // 49152

    // Sender side
    FlowControlManager sender;
    assert(sender.send_window_size(1)==stream_window);

    sender.add_bytes_sent(1, stream_window);
    assert(sender.send_window_size(1)==0);

    // Another stream is limited by remaining connection window
    assert(sender.send_window_size(3)==conn_window-stream_window);

    // Stream 1 grows; connection window still limits
    assert(sender.update_window(1, 100000));
    assert(sender.send_window_size(1)==conn_window-stream_window);

    // Connection window grows
    assert(sender.update_window(0, 200000));
    assert(sender.send_window_size(1)==100000-stream_window);

    // Receiver side
    FlowControlManager receiver;
    assert(receiver.update_highest_received(1, stream_window));
    assert(!receiver.update_highest_received(1, stream_window+1));

    // Sum of two streams exceeds connection window
    FlowControlManager receiver2;
    assert(receiver2.update_highest_received(1, 30000));
    assert(!receiver2.update_highest_received(3, 30000));

    // WINDOW_UPDATE generation
    FlowControlManager receiver3;
    assert(receiver3.update_highest_received(1, 20000));
    receiver3.add_bytes_read(1, 20000, T0);

    vector<WindowUpdateFrame> updates=receiver3.get_window_updates(T0, 0);
    assert(updates.size()==1);
    assert(updates[0].stream_id==1);
    assert(updates[0].byte_offset==20000+stream_window);
    assert(receiver3.get_receive_window(1)==20000+stream_window);
    assert(receiver3.get_receive_window(0)==conn_window);

    assert(receiver3.update_highest_received(1, 30000));
    receiver3.add_bytes_read(1, 10000, T0+1000);
    updates=receiver3.get_window_updates(T0+1000, 0);
    assert(updates.size()==1);
    assert(updates[0].stream_id==0);
    assert(updates[0].byte_offset==30000+conn_window);

    assert(receiver3.get_window_updates(T0+1000, 0).empty());

    cout<<"  -> PASS\n";
}

// Sender: loss detection and timers

void test_time_based_loss_detection()
{
    cout<<"[TEST] Loss detection (9/8 RTT threshold)\n";

    // 1. Missing packet not old enough yet -> arms early loss timer
    {
        RttStats rtt;
        FixedWindowCongestion congestion;
        SentPacketHandler h(&rtt, &congestion);

        for(uint64_t pn=1; pn<=4; pn++)
            assert(h.sent_packet(pn, data_frames(), 1000, T0+(pn-1)*1000));

        // ACK 1 and 3 -> RTT=40ms (loss threshold = 45ms)
        AckFrame ack;
        ack.largest_acked=3;
        ack.ranges={{3, 3}, {1, 1}};
        assert(h.received_ack(ack, 1, T0+2000+40000));
        assert(rtt.get_smoothed_rtt()==40000);

        // Packet 2 is 41ms old -> timer fires at 45ms
        assert(h.get_losses()==0);
        assert(!h.has_retransmissions());
        assert(h.get_num_outstanding()==2); // 2 and 4
        assert(h.get_alarm_timeout()==T0+1000+45000);

        // Timer fires -> packet 2 declared lost
        h.on_alarm(T0+1000+45000+1);
        assert(h.get_losses()==1);
        assert(h.get_num_outstanding()==1);
        assert(h.get_bytes_in_flight()==1000);
        assert(h.has_retransmissions());

        SentPacket lost;
        assert(h.dequeue_packet_for_retransmission(lost));
        assert(lost.packet_number==2);
        assert(lost.frames.size()==1&&lost.frames[0].type==FRAME_STREAM);
        assert(h.get_retransmissions()==1);
        assert(!h.dequeue_packet_for_retransmission(lost));

        // Next alarm is TLP for packet 4 (2*RTT)
        assert(h.get_alarm_timeout()==T0+3000+80000);

        // Late ACK for lost packet is harmless
        AckFrame late;
        late.largest_acked=3;
        late.ranges={{1, 3}};
        assert(h.received_ack(late, 2, T0+60000));
        assert(h.get_num_outstanding()==1);
    }

    // 2. Missing packet already > 9/8 RTT old -> lost immediately
    {
        RttStats rtt;
        FixedWindowCongestion congestion;
        SentPacketHandler h(&rtt, &congestion);

        assert(h.sent_packet(1, data_frames(), 1000, T0));
        assert(h.sent_packet(2, data_frames(), 1000, T0+1000));
        assert(h.sent_packet(3, data_frames(), 1000, T0+20000));

        AckFrame ack;
        ack.largest_acked=3;
        ack.ranges={{3, 3}, {1, 1}};
        assert(h.received_ack(ack, 1, T0+60000)); // RTT=40ms, packet 2 is 59ms old

        assert(h.get_losses()==1);
        assert(h.has_retransmissions());
        assert(h.get_num_outstanding()==0);
        assert(h.get_bytes_in_flight()==0);
        assert(h.get_alarm_timeout()==0);
    }

    cout<<"  -> PASS\n";
}

// Without an RTT sample, first alarm is 500ms RTO (not TLP)
void test_first_alarm_without_rtt_is_an_rto()
{
    cout<<"[TEST] First alarm without RTT is 500ms RTO\n";

    RttStats rtt;
    FixedWindowCongestion congestion;
    SentPacketHandler h(&rtt, &congestion);

    int rto_calls=0;
    h.set_rto_callback([&](uint64_t /*last_sent_time_us*/) {
        rto_calls++;
        return true; // mark path broken
    });

    assert(h.sent_packet(1, data_frames(), 1000, T0));
    assert(h.sent_packet(2, data_frames(), 1000, T0+1000));
    assert(rtt.get_smoothed_rtt()==0);
    assert(h.get_alarm_timeout()==T0+1000+LossRecovery::INITIAL_RTO_US);

    h.on_alarm(T0+1000+LossRecovery::INITIAL_RTO_US);
    assert(rto_calls==1);
    assert(h.get_num_outstanding()==0);
    SentPacket p;
    assert(h.dequeue_packet_for_retransmission(p)&&p.packet_number==1);
    assert(h.dequeue_packet_for_retransmission(p)&&p.packet_number==2);
    assert(h.get_losses()==2);

    cout<<"  -> PASS\n";
}

void test_tail_loss_probe_and_rto()
{
    cout<<"[TEST] 2 tail loss probes then RTO with backoff\n";

    RttStats rtt;
    FixedWindowCongestion congestion;
    SentPacketHandler h(&rtt, &congestion);

    bool path_failed=false;
    int rto_calls=0;
    uint64_t rto_last_sent_time=0;
    h.set_rto_callback([&](uint64_t last_sent_time_us) {
        rto_calls++;
        rto_last_sent_time=last_sent_time_us;
        return path_failed;
    });

    assert(h.get_alarm_timeout()==0);

    // Send 1..6 (1ms apart)
    for(uint64_t pn=1; pn<=6; pn++)
        assert(h.sent_packet(pn, data_frames(), 1000, T0+(pn-1)*1000));

    // Initial RTO = 500ms
    assert(h.get_alarm_timeout()==T0+5000+LossRecovery::INITIAL_RTO_US);

    // ACK 1 -> RTT=50ms, dev=25ms
    AckFrame ack;
    ack.largest_acked=1;
    ack.ranges={{1, 1}};
    assert(h.received_ack(ack, 1, T0+50000));
    assert(rtt.get_smoothed_rtt()==50000&&rtt.get_mean_deviation()==25000);

    // TLP = max(2*50, 1.5*50+100) = 175ms
    assert(h.get_alarm_timeout()==T0+5000+175000);

    // Probe 1 resends packet 6 (not counted as loss)
    h.on_alarm(T0+5000+175000);
    SentPacket p;
    assert(h.dequeue_packet_for_retransmission(p));
    assert(p.packet_number==6);
    assert(h.get_losses()==0);
    assert(h.get_num_outstanding()==4);
    assert(rto_calls==0);

    // Probe 2 resends packet 5
    h.on_alarm(T0+400000);
    assert(h.dequeue_packet_for_retransmission(p));
    assert(p.packet_number==5);
    assert(h.get_num_outstanding()==3);
    assert(rto_calls==0);

    // Next alarm is MIN_RTO_US (200ms)
    assert(h.get_alarm_timeout()==T0+5000+LossRecovery::MIN_RTO_US);

    // 1st RTO (path alive): resends oldest 2 packets
    h.on_alarm(T0+500000);
    assert(rto_calls==1);
    assert(rto_last_sent_time==T0+5000);
    assert(h.dequeue_packet_for_retransmission(p)&&p.packet_number==2);
    assert(h.dequeue_packet_for_retransmission(p)&&p.packet_number==3);
    assert(!h.dequeue_packet_for_retransmission(p));
    assert(h.get_losses()==2);
    assert(h.get_num_outstanding()==1);

    // Backoff doubles next RTO
    assert(h.get_alarm_timeout()==T0+500000+2*LossRecovery::MIN_RTO_US);

    // 2nd RTO (path broken): requeues everything
    assert(h.sent_packet(7, data_frames(), 1000, T0+600000));
    assert(h.sent_packet(8, data_frames(), 1000, T0+601000));
    assert(h.get_alarm_timeout()==T0+601000+2*LossRecovery::MIN_RTO_US);

    path_failed=true;
    h.on_alarm(T0+1100000);
    assert(rto_calls==2);
    assert(h.dequeue_packet_for_retransmission(p)&&p.packet_number==4);
    assert(h.dequeue_packet_for_retransmission(p)&&p.packet_number==7);
    assert(h.dequeue_packet_for_retransmission(p)&&p.packet_number==8);
    assert(h.get_losses()==5);
    assert(h.get_num_outstanding()==0);
    assert(h.get_bytes_in_flight()==0);
    assert(h.get_alarm_timeout()==0);
    assert(h.get_retransmissions()==7);

    // ACK resets probe/RTO counters
    assert(h.sent_packet(9, data_frames(), 1000, T0+2000000));
    assert(h.sent_packet(10, data_frames(), 1000, T0+2001000));
    AckFrame ack9;
    ack9.largest_acked=9;
    ack9.ranges={{9, 9}};
    assert(h.received_ack(ack9, 2, T0+2050000));
    assert(h.get_num_outstanding()==1);
    uint64_t srtt=rtt.get_smoothed_rtt();
    assert(h.get_alarm_timeout()==T0+2001000+2*srtt);

    cout<<"  -> PASS\n";
}

void test_retransmissions_bypass_congestion_window()
{
    cout<<"[TEST] Retransmissions allowed when cwnd is full\n";

    RttStats rtt;
    FixedWindowCongestion congestion(1); // 1 * 1460B
    SentPacketHandler h(&rtt, &congestion);

    // Get initial RTT sample
    assert(h.sent_packet(1, data_frames(), 1350, T0));
    AckFrame ack;
    ack.largest_acked=1;
    ack.ranges={{1, 1}};
    assert(h.received_ack(ack, 1, T0+50000));

    for(uint64_t pn=2; pn<=4; pn++)
        assert(h.sent_packet(pn, data_frames(), 1350, T0+50000+pn));
    assert(!h.sending_allowed());

    // Probe queues packet 4 -> sending_allowed becomes true for resend
    h.on_alarm(T0+600000);
    assert(h.get_bytes_in_flight()==2700);
    assert(h.has_retransmissions());
    assert(h.sending_allowed());

    SentPacket p;
    assert(h.dequeue_packet_for_retransmission(p));
    assert(!h.sending_allowed());

    cout<<"  -> PASS\n";
}

void test_path_potentially_failed()
{
    cout<<"[TEST] Path marked potentially failed on silent RTO\n";

    // Paths below have no RTT sample, so every alarm is an RTO

    // 1. Nothing ever received -> first RTO marks broken
    {
        Path path(1, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
        SentPacketHandler *h=path.get_sent_packet_handler();
        assert(!path.is_potentially_failed());

        for(uint64_t pn=1; pn<=4; pn++)
            assert(h->sent_packet(path.pop_packet_number(), data_frames(), 1000, T0+pn*1000));
        assert(h->get_alarm_timeout()==T0+4000+LossRecovery::INITIAL_RTO_US);
        assert(!path.is_potentially_failed());

        h->on_alarm(T0+4000+LossRecovery::INITIAL_RTO_US);
        assert(path.is_potentially_failed());

        // All packets requeued for other paths
        assert(h->get_num_outstanding()==0);
        int count=0;
        SentPacket p;
        while(h->dequeue_packet_for_retransmission(p))
            count++;
        assert(count==4);

        // Cleared when packet arrives again
        path.set_potentially_failed(false);
        assert(!path.is_potentially_failed());
    }

    // 2. Packet arrived after last send -> normal RTO, not marked broken
    {
        Path path(3, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
        SentPacketHandler *h=path.get_sent_packet_handler();

        for(uint64_t pn=1; pn<=4; pn++)
            assert(h->sent_packet(path.pop_packet_number(), data_frames(), 1000, T0+pn*1000));
        path.set_last_network_activity_time(T0+100000);

        h->on_alarm(T0+600000);
        assert(!path.is_potentially_failed());
        assert(h->get_num_outstanding()==2);
    }

    // 3. Path dies after an ACK -> 2nd silent RTO marks it broken
    {
        Path path(5, "127.0.0.1", 0, "127.0.0.1", 0, nullptr);
        SentPacketHandler *h=path.get_sent_packet_handler();

        for(uint64_t pn=1; pn<=8; pn++)
            assert(h->sent_packet(path.pop_packet_number(), data_frames(), 1000, T0+pn*1000));
        path.set_last_network_activity_time(T0+100000);

        h->on_alarm(T0+600000);  // 1st RTO: 2 packets
        assert(!path.is_potentially_failed());
        assert(h->get_num_outstanding()==6);

        h->on_alarm(T0+1800000); // 2nd RTO: silent since 1st RTO -> broken
        assert(path.is_potentially_failed());
        assert(h->get_num_outstanding()==0);
        assert(h->get_losses()==8);
    }

    cout<<"  -> PASS\n";
}

int main()
{
    cout<<"=== Loss Recovery Tests ===\n";
    test_sorter_reordering();
    test_sorter_duplicates_and_overlaps();
    test_receive_stream_fin();
    test_receive_stream_end_is_fixed();
    test_flow_controller_sender();
    test_flow_controller_receiver();
    test_flow_control_manager();
    test_time_based_loss_detection();
    test_first_alarm_without_rtt_is_an_rto();
    test_tail_loss_probe_and_rto();
    test_retransmissions_bypass_congestion_window();
    test_path_potentially_failed();
    cout<<"=== All Loss Recovery Tests Passed ===\n";
    return 0;
}
