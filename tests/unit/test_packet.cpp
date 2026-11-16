#include<bits/stdc++.h>
#include<endian.h>

#include "packet/packet.hpp"

using namespace std;

void test_header_size_and_layout()
{
    cout<<"[TEST] 17-byte public header layout\n";
    assert(sizeof(PacketHeader)==17);
    assert(Packet::HEADER_SIZE==17);

    uint64_t connection_id=0x1122334455667788ULL;
    uint64_t packet_num=0xAABBCCDDEEFF0011ULL;

    Packet pkt(connection_id, 3, packet_num);
    pkt.add_frame(Frame::make_ping());

    vector<uint8_t> raw=pkt.serialize();
    assert(raw.size()==17+PING_FRAME_SIZE);
    assert(raw.size()==pkt.size());

    // Verify explicit byte offsets
    uint64_t raw_cid;
    memcpy(&raw_cid, raw.data()+0, 8); // Offset 0: connection_id
    assert(be64toh(raw_cid)==connection_id);
    assert(raw[0]==0x11&&raw[7]==0x88); // big-endian: most significant byte first

    assert(raw[8]==3); // Offset 8: path_id

    uint64_t raw_pn;
    memcpy(&raw_pn, raw.data()+9, 8); // Offset 9: packet_number
    assert(be64toh(raw_pn)==packet_num);

    assert(raw[17]==FRAME_PING); // Offset 17: first frame

    // peek_header reads the same fields without parsing frames
    uint64_t peeked_cid=0;
    uint8_t peeked_path=0;
    assert(Packet::peek_header(raw.data(), raw.size(), peeked_cid, peeked_path));
    assert(peeked_cid==connection_id&&peeked_path==3);

    cout<<"  -> PASS\n";
}

void test_stream_frame_round_trip()
{
    cout<<"[TEST] STREAM frame layout and round trip\n";

    string msg="Hello Multipath QUIC!";
    vector<uint8_t> data(msg.begin(), msg.end());

    Packet pkt(9999ULL, 1, 42ULL);
    pkt.add_frame(Frame::make_stream(5, 1100ULL, true, data));

    vector<uint8_t> raw=pkt.serialize();
    assert(raw.size()==Packet::HEADER_SIZE+STREAM_FRAME_HEADER_SIZE+data.size());

    // STREAM frame: type(1) stream_id(4) offset(8) fin(1) length(2) data
    assert(raw[17]==FRAME_STREAM);
    uint32_t raw_sid;
    memcpy(&raw_sid, raw.data()+18, 4);
    assert(be32toh(raw_sid)==5);
    uint64_t raw_off;
    memcpy(&raw_off, raw.data()+22, 8);
    assert(be64toh(raw_off)==1100ULL);
    assert(raw[30]==1);
    uint16_t raw_len;
    memcpy(&raw_len, raw.data()+31, 2);
    assert(be16toh(raw_len)==data.size());
    assert(memcmp(raw.data()+33, data.data(), data.size())==0);

    Packet parsed;
    assert(Packet::deserialize(raw.data(), raw.size(), parsed));
    assert(parsed.get_connection_id()==9999ULL);
    assert(parsed.get_path_id()==1);
    assert(parsed.get_packet_number()==42ULL);
    assert(parsed.get_packet_num()==42ULL);
    assert(parsed.is_retransmittable());
    assert(parsed.get_frames().size()==1);

    const Frame &f=parsed.get_frames()[0];
    assert(f.type==FRAME_STREAM);
    assert(f.stream.stream_id==5);
    assert(f.stream.offset==1100ULL);
    assert(f.stream.fin);
    assert(f.stream.data==data);

    // Empty frame is valid only with FIN=1
    Packet fin_pkt(1, 0, 1);
    fin_pkt.add_frame(Frame::make_stream(5, 2000ULL, true, {}));
    vector<uint8_t> fin_raw=fin_pkt.serialize();
    assert(Packet::deserialize(fin_raw.data(), fin_raw.size(), parsed));
    assert(parsed.get_frames()[0].stream.fin);
    assert(parsed.get_frames()[0].stream.data.empty());

    fin_raw[30]=0; // clear FIN byte
    assert(!Packet::deserialize(fin_raw.data(), fin_raw.size(), parsed));

    cout<<"  -> PASS\n";
}

void test_ack_frame_round_trip()
{
    cout<<"[TEST] ACK frame round trip\n";

    AckFrame ack;
    ack.path_id=3;
    ack.largest_acked=45;
    ack.ack_delay_us=1200; // 1.2 ms
    ack.ranges={
        {40, 45}, // 40..45 received
        {35, 37}, // 38..39 missing; 35..37 received
        {10, 10}};

    assert(ack.lowest_acked()==10);
    assert(ack.acks_packet(42)&&ack.acks_packet(35)&&ack.acks_packet(10));
    assert(!ack.acks_packet(38)&&!ack.acks_packet(11)&&!ack.acks_packet(46));

    // ACK sent on path 1, acking path 3
    Packet pkt(777, 1, 12);
    pkt.add_frame(Frame::make_ack(ack));
    assert(!pkt.is_retransmittable()); // ACK-only

    vector<uint8_t> raw=pkt.serialize();

    // ACK wire format: header(16) + first_range_len + (gap + range_len)...
    assert(raw.size()==Packet::HEADER_SIZE+ACK_FRAME_HEADER_SIZE+5);
    assert(raw[17]==FRAME_ACK);
    assert(raw[18]==3);  // path_id
    assert(raw[26]==45); // largest_acked
    assert(raw[31]==0&&raw[32]==3); // num_ranges
    assert(raw[33]==5);  // 40..45: 6-1
    assert(raw[34]==2);  // gap 38..39
    assert(raw[35]==2);  // 35..37: 3-1
    assert(raw[36]==24); // gap 11..34
    assert(raw[37]==0);  // 10..10: 1-1

    Packet parsed;
    assert(Packet::deserialize(raw.data(), raw.size(), parsed));
    assert(parsed.get_path_id()==1);
    assert(parsed.get_frames().size()==1);

    const Frame &f=parsed.get_frames()[0];
    assert(f.type==FRAME_ACK);
    assert(f.ack.path_id==3);
    assert(f.ack.largest_acked==45);
    assert(f.ack.ack_delay_us==1200);
    assert(f.ack.ranges.size()==3);
    assert(f.ack.ranges[0].first==40&&f.ack.ranges[0].last==45);
    assert(f.ack.ranges[1].first==35&&f.ack.ranges[1].last==37);
    assert(f.ack.ranges[2].first==10&&f.ack.ranges[2].last==10);

    cout<<"  -> PASS\n";
}

void test_ack_frame_256_ranges()
{
    cout<<"[TEST] ACK frame with 256 ranges and large varints\n";

    // 1. 256 ranges (odd packets 1, 3, ..., 511)
    AckFrame ack;
    ack.path_id=1;
    ack.largest_acked=511;
    for(uint64_t pn=511;; pn-=2){
        ack.ranges.push_back({pn, pn});
        if(pn==1)
            break;
    }
    assert(ack.ranges.size()==MAX_ACK_RANGES);

    Packet pkt(1, 1, 9);
    pkt.add_frame(Frame::make_ack(ack));
    vector<uint8_t> raw=pkt.serialize();

    // 1 byte first range + 255 * 2 bytes
    assert(raw.size()==Packet::HEADER_SIZE+ACK_FRAME_HEADER_SIZE+1+2*255);
    assert(raw.size()<=MAX_PACKET_SIZE);

    Packet parsed;
    assert(Packet::deserialize(raw.data(), raw.size(), parsed));
    const AckFrame &back=parsed.get_frames()[0].ack;
    assert(back.ranges.size()==MAX_ACK_RANGES);
    assert(back.largest_acked==511&&back.lowest_acked()==1);
    for(size_t i=0; i<back.ranges.size(); i++){
        assert(back.ranges[i].first==ack.ranges[i].first);
        assert(back.ranges[i].last==ack.ranges[i].last);
    }

    // Drop oldest if > 256 ranges
    AckFrame too_many=ack;
    too_many.largest_acked=513;
    too_many.ranges.insert(too_many.ranges.begin(), {513, 513});
    Packet pkt257(1, 1, 10);
    pkt257.add_frame(Frame::make_ack(too_many));
    vector<uint8_t> raw257=pkt257.serialize();
    assert(Packet::deserialize(raw257.data(), raw257.size(), parsed));
    assert(parsed.get_frames()[0].ack.ranges.size()==MAX_ACK_RANGES);
    assert(parsed.get_frames()[0].ack.lowest_acked()==3);

    // 2. Multi-byte varints (2, 4, 8 bytes)
    AckFrame big;
    big.path_id=0;
    big.largest_acked=5000000100ULL;
    big.ranges={
        {5000000000ULL, 5000000100ULL}, // len 100 -> 2B
        {70000ULL, 4000000000ULL},      // gap 999,999,999 -> 4B, len 3,999,930,000 -> 8B
        {1ULL, 100ULL}};                // gap 69,899 -> 4B, len 99 -> 2B

    Packet big_pkt(1, 0, 11);
    big_pkt.add_frame(Frame::make_ack(big));
    vector<uint8_t> big_raw=big_pkt.serialize();
    assert(big_raw.size()==Packet::HEADER_SIZE+ACK_FRAME_HEADER_SIZE+2+4+8+4+2);

    assert(Packet::deserialize(big_raw.data(), big_raw.size(), parsed));
    const AckFrame &big_back=parsed.get_frames()[0].ack;
    assert(big_back.ranges.size()==3);
    for(size_t i=0; i<3; i++){
        assert(big_back.ranges[i].first==big.ranges[i].first);
        assert(big_back.ranges[i].last==big.ranges[i].last);
    }

    cout<<"  -> PASS\n";
}

void test_paths_frame_round_trip()
{
    cout<<"[TEST] PATHS frame round trip\n";

    PathsFrame paths;
    paths.paths={{0, 15000}, {1, PATHS_RTT_POTENTIALLY_FAILED}};

    Packet pkt(42, 0, 7);
    pkt.add_frame(Frame::make_paths(paths));
    assert(pkt.is_retransmittable());

    vector<uint8_t> raw=pkt.serialize();
    assert(raw.size()==Packet::HEADER_SIZE+PATHS_FRAME_HEADER_SIZE+2*PATHS_ENTRY_SIZE);
    assert(raw[17]==FRAME_PATHS);
    assert(raw[18]==2);
    assert(raw[19]==0);
    assert(raw[24]==1);
    assert(raw[25]==0xFF&&raw[28]==0xFF); // potentially failed

    Packet parsed;
    assert(Packet::deserialize(raw.data(), raw.size(), parsed));
    const Frame &f=parsed.get_frames()[0];
    assert(f.type==FRAME_PATHS);
    assert(f.paths.paths.size()==2);
    assert(f.paths.paths[0].path_id==0&&f.paths.paths[0].rtt_us==15000);
    assert(f.paths.paths[1].path_id==1&&f.paths.paths[1].rtt_us==PATHS_RTT_POTENTIALLY_FAILED);

    cout<<"  -> PASS\n";
}

void test_add_address_frame_round_trip()
{
    cout<<"[TEST] ADD_ADDRESS frame round trip\n";

    vector<NetworkAddress> addresses(2);
    addresses[0].ip="10.0.0.2";
    addresses[0].port=4433;
    addresses[1].ip="192.168.56.20";
    addresses[1].port=65000;

    Packet pkt(42, 0, 3);
    pkt.add_frame(Frame::make_add_address(addresses));
    assert(pkt.is_retransmittable());

    vector<uint8_t> raw=pkt.serialize();
    assert(raw.size()==Packet::HEADER_SIZE+ADD_ADDRESS_FRAME_HEADER_SIZE+2*ADD_ADDRESS_ENTRY_SIZE);
    assert(raw[17]==FRAME_ADD_ADDRESS);
    assert(raw[18]==2);
    assert(raw[19]==10&&raw[20]==0&&raw[21]==0&&raw[22]==2); // 10.0.0.2
    assert(raw[23]==0x11&&raw[24]==0x51);                    // port 4433 = 0x1151

    Packet parsed;
    assert(Packet::deserialize(raw.data(), raw.size(), parsed));
    const Frame &f=parsed.get_frames()[0];
    assert(f.type==FRAME_ADD_ADDRESS);
    assert(f.add_address.addresses.size()==2);
    assert(f.add_address.addresses[0].ip=="10.0.0.2"&&f.add_address.addresses[0].port==4433);
    assert(f.add_address.addresses[1].ip=="192.168.56.20"&&f.add_address.addresses[1].port==65000);

    // Reject truncated or empty ADD_ADDRESS
    Packet out;
    assert(!Packet::deserialize(raw.data(), raw.size()-1, out));
    vector<uint8_t> bad=raw;
    bad[18]=0;
    assert(!Packet::deserialize(bad.data(), bad.size(), out));

    // Reject invalid IPv4 string
    vector<NetworkAddress> wrong(1);
    wrong[0].ip="not-an-address";
    wrong[0].port=1;
    Packet wrong_pkt(1, 0, 1);
    wrong_pkt.add_frame(Frame::make_add_address(wrong));
    bool threw=false;
    try{
        wrong_pkt.serialize();
    }
    catch(const invalid_argument& /*e*/){
        threw=true;
    }
    assert(threw);

    cout<<"  -> PASS\n";
}

void test_connection_close_frame_round_trip()
{
    cout<<"[TEST] CONNECTION_CLOSE frame round trip\n";

    Packet pkt(42, 1, 9);
    pkt.add_frame(Frame::make_connection_close(CLOSE_NO_ERROR, "transfer complete"));

    vector<uint8_t> raw=pkt.serialize();
    assert(raw.size()==Packet::HEADER_SIZE+CONNECTION_CLOSE_FRAME_HEADER_SIZE+17);
    assert(raw[17]==FRAME_CONNECTION_CLOSE);

    Packet parsed;
    assert(Packet::deserialize(raw.data(), raw.size(), parsed));
    const Frame &f=parsed.get_frames()[0];
    assert(f.type==FRAME_CONNECTION_CLOSE);
    assert(f.connection_close.error_code==CLOSE_NO_ERROR);
    assert(f.connection_close.reason=="transfer complete");

    // Empty reason with error code
    Packet pkt2(42, 1, 10);
    pkt2.add_frame(Frame::make_connection_close(CLOSE_PROTOCOL_ERROR, ""));
    vector<uint8_t> raw2=pkt2.serialize();
    assert(raw2.size()==Packet::HEADER_SIZE+CONNECTION_CLOSE_FRAME_HEADER_SIZE);
    assert(Packet::deserialize(raw2.data(), raw2.size(), parsed));
    assert(parsed.get_frames()[0].connection_close.error_code==CLOSE_PROTOCOL_ERROR);
    assert(parsed.get_frames()[0].connection_close.reason.empty());

    // Long reason is capped to MAX_CLOSE_REASON_LENGTH
    Packet pkt3(42, 1, 11);
    pkt3.add_frame(Frame::make_connection_close(CLOSE_NO_ERROR, string(5000, 'x')));
    vector<uint8_t> raw3=pkt3.serialize();
    assert(Packet::deserialize(raw3.data(), raw3.size(), parsed));
    assert(parsed.get_frames()[0].connection_close.reason.size()==MAX_CLOSE_REASON_LENGTH);

    // Truncated reason rejected
    Packet out;
    assert(!Packet::deserialize(raw.data(), raw.size()-3, out));

    // peek_header reads CID and path_id without parsing frames
    uint64_t connection_id=0;
    uint8_t path_id=0;
    assert(Packet::peek_header(raw.data(), raw.size(), connection_id, path_id));
    assert(connection_id==42&&path_id==1);
    assert(Packet::peek_header(raw.data(), Packet::HEADER_SIZE, connection_id, path_id));    // exactly a header
    assert(!Packet::peek_header(raw.data(), Packet::HEADER_SIZE-1, connection_id, path_id)); // one byte short

    cout<<"  -> PASS\n";
}

void test_multiple_frames_in_one_packet()
{
    cout<<"[TEST] Multiple frames in one packet\n";

    AckFrame ack;
    ack.path_id=0;
    ack.largest_acked=7;
    ack.ranges={{1, 7}};

    Packet pkt(55, 0, 9);
    pkt.add_frame(Frame::make_ack(ack));
    pkt.add_frame(Frame::make_window_update(0, 65536ULL));
    pkt.add_frame(Frame::make_stream(1, 0, false, vector<uint8_t>(100, 'A')));
    pkt.add_frame(Frame::make_stream(3, 500, false, vector<uint8_t>(50, 'B')));
    pkt.add_frame(Frame::make_ping());

    vector<uint8_t> raw=pkt.serialize();
    assert(raw.size()==pkt.size());

    Packet parsed;
    assert(Packet::deserialize(raw.data(), raw.size(), parsed));
    assert(parsed.get_frames().size()==5);
    assert(parsed.get_frames()[0].type==FRAME_ACK);
    assert(parsed.get_frames()[1].type==FRAME_WINDOW_UPDATE);
    assert(parsed.get_frames()[1].window_update.stream_id==0);
    assert(parsed.get_frames()[1].window_update.byte_offset==65536ULL);
    assert(parsed.get_frames()[2].type==FRAME_STREAM);
    assert(parsed.get_frames()[2].stream.stream_id==1);
    assert(parsed.get_frames()[2].stream.data.size()==100);
    assert(parsed.get_frames()[3].stream.stream_id==3);
    assert(parsed.get_frames()[3].stream.offset==500);
    assert(parsed.get_frames()[4].type==FRAME_PING);
    assert(parsed.is_retransmittable());

    cout<<"  -> PASS\n";
}

void test_packet_size_limit()
{
    cout<<"[TEST] Max packet size (1350 bytes)\n";

    // 1. Exact MAX_PACKET_SIZE -> ok
    size_t max_data=MAX_PACKET_SIZE-Packet::HEADER_SIZE-STREAM_FRAME_HEADER_SIZE;
    Packet max_pkt(100ULL, 0, 1ULL);
    max_pkt.add_frame(Frame::make_stream(1, 0, false, vector<uint8_t>(max_data, 0xAB)));
    assert(max_pkt.size()==MAX_PACKET_SIZE);

    vector<uint8_t> max_buf=max_pkt.serialize();
    assert(max_buf.size()==MAX_PACKET_SIZE);

    Packet parsed_max;
    assert(Packet::deserialize(max_buf.data(), max_buf.size(), parsed_max));
    assert(parsed_max.get_frames()[0].stream.data.size()==max_data);
    assert(parsed_max.get_frames()[0].stream.data[max_data-1]==0xAB);

    // 2. 1 byte over -> throws length_error
    Packet overflow_pkt(100ULL, 0, 2ULL);
    overflow_pkt.add_frame(Frame::make_stream(1, 0, false, vector<uint8_t>(max_data+1, 0xCD)));

    bool threw_exception=false;
    try{
        overflow_pkt.serialize();
    }
    catch(const length_error& /*e*/){
        threw_exception=true;
    }
    assert(threw_exception);

    cout<<"  -> PASS\n";
}

void test_corrupted_and_truncated_deserialization()
{
    cout<<"[TEST] Malformed and truncated packet handling\n";

    Packet out;
    // 1. Null buffer
    assert(!Packet::deserialize(nullptr, Packet::HEADER_SIZE, out));

    // 2. Shorter than header (16 bytes)
    uint8_t short_buf[Packet::HEADER_SIZE-1]={0};
    assert(!Packet::deserialize(short_buf, sizeof(short_buf), out));

    // 3. Header only, no frames
    uint8_t header_only[Packet::HEADER_SIZE]={0};
    assert(!Packet::deserialize(header_only, sizeof(header_only), out));

    // 4. Truncated STREAM payload
    Packet pkt10(1ULL, 0, 1ULL);
    pkt10.add_frame(Frame::make_stream(1, 0, false, vector<uint8_t>(10, 'A')));
    vector<uint8_t> full10=pkt10.serialize();
    assert(full10.size()==Packet::HEADER_SIZE+16+10);
    assert(!Packet::deserialize(full10.data(), full10.size()-5, out));

    // 5. Trailing unknown frame type
    vector<uint8_t> extra_bytes_buf=full10;
    extra_bytes_buf.push_back(0xFF);
    assert(!Packet::deserialize(extra_bytes_buf.data(), extra_bytes_buf.size(), out));

    // 6. Truncated frame header
    assert(!Packet::deserialize(full10.data(), Packet::HEADER_SIZE+5, out));

    // 7. Invalid ACK frames (corrupt one byte at a time)
    AckFrame ack;
    ack.path_id=0;
    ack.largest_acked=45;
    ack.ranges={{40, 45}, {35, 37}, {10, 10}};
    Packet good(1, 0, 1);
    good.add_frame(Frame::make_ack(ack));
    vector<uint8_t> valid=good.serialize();
    assert(Packet::deserialize(valid.data(), valid.size(), out));

    vector<uint8_t> bad=valid;
    bad[32]=0; // 0 ranges
    assert(!Packet::deserialize(bad.data(), bad.size(), out));

    bad=valid;
    bad[33]=45; // reaches packet 0
    assert(!Packet::deserialize(bad.data(), bad.size(), out));

    bad=valid;
    bad[34]=0; // 0 gap (touching ranges)
    assert(!Packet::deserialize(bad.data(), bad.size(), out));

    bad=valid;
    bad[36]=34; // gap pushes next range to 0
    assert(!Packet::deserialize(bad.data(), bad.size(), out));

    bad=valid;
    bad[37]=10; // last range reaches 0
    assert(!Packet::deserialize(bad.data(), bad.size(), out));

    bad=valid;
    bad.pop_back(); // truncated last range
    assert(!Packet::deserialize(bad.data(), bad.size(), out));

    // Out-of-order ACK ranges only write valid leading ranges
    AckFrame unordered;
    unordered.largest_acked=20;
    unordered.ranges={{15, 20}, {17, 18}};
    Packet partial(1, 0, 1);
    partial.add_frame(Frame::make_ack(unordered));
    vector<uint8_t> partial_raw=partial.serialize();
    assert(Packet::deserialize(partial_raw.data(), partial_raw.size(), out));
    assert(out.get_frames()[0].ack.ranges.size()==1);

    // 8. Invalid PATHS frames
    PathsFrame paths;
    paths.paths={{0, 15000}, {1, PATHS_RTT_POTENTIALLY_FAILED}};
    Packet paths_pkt(1, 0, 1);
    paths_pkt.add_frame(Frame::make_paths(paths));
    vector<uint8_t> paths_raw=paths_pkt.serialize();
    assert(Packet::deserialize(paths_raw.data(), paths_raw.size(), out));
    assert(!Packet::deserialize(paths_raw.data(), paths_raw.size()-1, out)); // truncated

    bad=paths_raw;
    bad[18]=0; // 0 paths
    assert(!Packet::deserialize(bad.data(), bad.size(), out));

    cout<<"  -> PASS\n";
}

int main()
{
    cout<<"=== Packet and Frame Tests ===\n";
    test_header_size_and_layout();
    test_stream_frame_round_trip();
    test_ack_frame_round_trip();
    test_ack_frame_256_ranges();
    test_paths_frame_round_trip();
    test_add_address_frame_round_trip();
    test_connection_close_frame_round_trip();
    test_multiple_frames_in_one_packet();
    test_packet_size_limit();
    test_corrupted_and_truncated_deserialization();
    cout<<"=== All Packet Tests Passed ===\n";
    return 0;
}
