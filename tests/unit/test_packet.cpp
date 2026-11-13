#include <iostream>
#include <cassert>
#include <cstring>
#include <vector>
#include <string>
#include <stdexcept>
#include <endian.h>

#include "packet/packet.hpp"

void test_header_size_and_layout()
{
    std::cout << "[TEST] 18-Byte public header size and binary layout...\n";
    assert(sizeof(PacketHeader) == 18);
    assert(Packet::HEADER_SIZE == 18);

    uint64_t connection_id = 0x1122334455667788ULL;
    uint64_t packet_num = 0xAABBCCDDEEFF0011ULL;

    Packet pkt(FLAG_NONE, connection_id, 3, packet_num);
    pkt.add_frame(Frame::make_ping());

    std::vector<uint8_t> raw = pkt.serialize();
    assert(raw.size() == 18 + PING_FRAME_SIZE);
    assert(raw.size() == pkt.size());

    // Verify explicit byte offsets
    assert(raw[0] == FLAG_NONE); // Offset 0: flags

    uint64_t raw_cid;
    std::memcpy(&raw_cid, raw.data() + 1, 8); // Offset 1: connection_id
    assert(be64toh(raw_cid) == connection_id);

    assert(raw[9] == 3); // Offset 9: path_id

    uint64_t raw_pn;
    std::memcpy(&raw_pn, raw.data() + 10, 8); // Offset 10: packet_number
    assert(be64toh(raw_pn) == packet_num);

    assert(raw[18] == FRAME_PING); // Offset 18: first frame

    std::cout << "  -> PASSED\n";
}

void test_stream_frame_round_trip()
{
    std::cout << "[TEST] STREAM frame layout and round trip...\n";

    std::string msg = "Hello Multipath QUIC!";
    std::vector<uint8_t> data(msg.begin(), msg.end());

    Packet pkt(FLAG_NONE, 9999ULL, 1, 42ULL);
    pkt.add_frame(Frame::make_stream(5, 1100ULL, true, data));

    std::vector<uint8_t> raw = pkt.serialize();
    assert(raw.size() == 18 + STREAM_FRAME_HEADER_SIZE + data.size());

    // STREAM frame: type(1) stream_id(4) offset(8) fin(1) length(2) data
    assert(raw[18] == FRAME_STREAM);
    uint32_t raw_sid;
    std::memcpy(&raw_sid, raw.data() + 19, 4);
    assert(be32toh(raw_sid) == 5);
    uint64_t raw_off;
    std::memcpy(&raw_off, raw.data() + 23, 8);
    assert(be64toh(raw_off) == 1100ULL);
    assert(raw[31] == 1);
    uint16_t raw_len;
    std::memcpy(&raw_len, raw.data() + 32, 2);
    assert(be16toh(raw_len) == data.size());
    assert(std::memcmp(raw.data() + 34, data.data(), data.size()) == 0);

    Packet parsed;
    assert(Packet::deserialize(raw.data(), raw.size(), parsed));
    assert(parsed.get_flags() == FLAG_NONE);
    assert(parsed.get_connection_id() == 9999ULL);
    assert(parsed.get_path_id() == 1);
    assert(parsed.get_packet_number() == 42ULL);
    assert(parsed.get_packet_num() == 42ULL);
    assert(parsed.is_retransmittable());
    assert(parsed.get_frames().size() == 1);

    const Frame &f = parsed.get_frames()[0];
    assert(f.type == FRAME_STREAM);
    assert(f.stream.stream_id == 5);
    assert(f.stream.offset == 1100ULL);
    assert(f.stream.fin);
    assert(f.stream.data == data);

    // A FIN-only frame (no data) is valid, an empty frame without FIN is not
    Packet fin_pkt(FLAG_NONE, 1, 0, 1);
    fin_pkt.add_frame(Frame::make_stream(5, 2000ULL, true, {}));
    std::vector<uint8_t> fin_raw = fin_pkt.serialize();
    assert(Packet::deserialize(fin_raw.data(), fin_raw.size(), parsed));
    assert(parsed.get_frames()[0].stream.fin);
    assert(parsed.get_frames()[0].stream.data.empty());

    fin_raw[31] = 0; // clear the FIN byte
    assert(!Packet::deserialize(fin_raw.data(), fin_raw.size(), parsed));

    std::cout << "  -> PASSED\n";
}

void test_ack_frame_round_trip()
{
    std::cout << "[TEST] ACK frame (Path ID + packet number ranges) round trip...\n";

    AckFrame ack;
    ack.path_id = 3;
    ack.largest_acked = 45;
    ack.ack_delay_us = 1200; // 1.2 ms
    ack.ranges = {
        {40, 45}, // Packets 40 to 45 received
        {35, 37}, // Gap at 38-39; packets 35 to 37 received
        {10, 10}};

    assert(ack.lowest_acked() == 10);
    assert(ack.acks_packet(42) && ack.acks_packet(35) && ack.acks_packet(10));
    assert(!ack.acks_packet(38) && !ack.acks_packet(11) && !ack.acks_packet(46));

    // The ACK travels on path 1 but acknowledges packets of path 3
    Packet pkt(FLAG_NONE, 777, 1, 12);
    pkt.add_frame(Frame::make_ack(ack));
    assert(!pkt.is_retransmittable()); // ACK-only packet

    std::vector<uint8_t> raw = pkt.serialize();

    // ACK frame: type(1) path_id(1) largest_acked(8) ack_delay_us(4) num_ranges(2), then
    // first_range_length, then gap + length for every further range (1 byte each here)
    assert(raw.size() == 18 + ACK_FRAME_HEADER_SIZE + 5);
    assert(raw[18] == FRAME_ACK);
    assert(raw[19] == 3);  // path_id
    assert(raw[27] == 45); // largest_acked (last of its 8 bytes)
    assert(raw[32] == 0 && raw[33] == 3); // num_ranges
    assert(raw[34] == 5);  // first range 40..45: 6 packets, minus 1
    assert(raw[35] == 2);  // gap: packets 38 and 39 are missing
    assert(raw[36] == 2);  // range 35..37: 3 packets, minus 1
    assert(raw[37] == 24); // gap: packets 11..34 are missing
    assert(raw[38] == 0);  // range 10..10: 1 packet, minus 1

    Packet parsed;
    assert(Packet::deserialize(raw.data(), raw.size(), parsed));
    assert(parsed.get_path_id() == 1);
    assert(parsed.get_frames().size() == 1);

    const Frame &f = parsed.get_frames()[0];
    assert(f.type == FRAME_ACK);
    assert(f.ack.path_id == 3);
    assert(f.ack.largest_acked == 45);
    assert(f.ack.ack_delay_us == 1200);
    assert(f.ack.ranges.size() == 3);
    assert(f.ack.ranges[0].first == 40 && f.ack.ranges[0].last == 45);
    assert(f.ack.ranges[1].first == 35 && f.ack.ranges[1].last == 37);
    assert(f.ack.ranges[2].first == 10 && f.ack.ranges[2].last == 10);

    std::cout << "  -> PASSED\n";
}

void test_ack_frame_256_ranges()
{
    std::cout << "[TEST] ACK frame with 256 ranges fits in one packet, large numbers keep their value...\n";

    // 1. 256 ranges: only the odd packet numbers 1, 3, ... 511 arrived
    AckFrame ack;
    ack.path_id = 1;
    ack.largest_acked = 511;
    for (uint64_t pn = 511;; pn -= 2)
    {
        ack.ranges.push_back({pn, pn});
        if (pn == 1)
            break;
    }
    assert(ack.ranges.size() == MAX_ACK_RANGES);

    Packet pkt(FLAG_NONE, 1, 1, 9);
    pkt.add_frame(Frame::make_ack(ack));
    std::vector<uint8_t> raw = pkt.serialize();

    // 1 byte for the first range, then 2 bytes (gap + length) for each of the 255 others
    assert(raw.size() == 18 + ACK_FRAME_HEADER_SIZE + 1 + 2 * 255);
    assert(raw.size() <= MAX_PACKET_SIZE);

    Packet parsed;
    assert(Packet::deserialize(raw.data(), raw.size(), parsed));
    const AckFrame &back = parsed.get_frames()[0].ack;
    assert(back.ranges.size() == MAX_ACK_RANGES);
    assert(back.largest_acked == 511 && back.lowest_acked() == 1);
    for (size_t i = 0; i < back.ranges.size(); i++)
    {
        assert(back.ranges[i].first == ack.ranges[i].first);
        assert(back.ranges[i].last == ack.ranges[i].last);
    }

    // A 257th range is not sent (the highest 256 are kept)
    AckFrame too_many = ack;
    too_many.largest_acked = 513;
    too_many.ranges.insert(too_many.ranges.begin(), {513, 513});
    Packet pkt257(FLAG_NONE, 1, 1, 10);
    pkt257.add_frame(Frame::make_ack(too_many));
    std::vector<uint8_t> raw257 = pkt257.serialize();
    assert(Packet::deserialize(raw257.data(), raw257.size(), parsed));
    assert(parsed.get_frames()[0].ack.ranges.size() == MAX_ACK_RANGES);
    assert(parsed.get_frames()[0].ack.lowest_acked() == 3);

    // 2. Large numbers: the numbers of the frame take 2, 4 or 8 bytes when they need to
    AckFrame big;
    big.path_id = 0;
    big.largest_acked = 5000000100ULL;
    big.ranges = {
        {5000000000ULL, 5000000100ULL}, // length 100            -> 2 bytes
        {70000ULL, 4000000000ULL},      // gap 999,999,999       -> 4 bytes, length 3,999,930,000 -> 8 bytes
        {1ULL, 100ULL}};                // gap 69,899            -> 4 bytes, length 99            -> 2 bytes

    Packet big_pkt(FLAG_NONE, 1, 0, 11);
    big_pkt.add_frame(Frame::make_ack(big));
    std::vector<uint8_t> big_raw = big_pkt.serialize();
    assert(big_raw.size() == 18 + ACK_FRAME_HEADER_SIZE + 2 + 4 + 8 + 4 + 2);

    assert(Packet::deserialize(big_raw.data(), big_raw.size(), parsed));
    const AckFrame &big_back = parsed.get_frames()[0].ack;
    assert(big_back.ranges.size() == 3);
    for (size_t i = 0; i < 3; i++)
    {
        assert(big_back.ranges[i].first == big.ranges[i].first);
        assert(big_back.ranges[i].last == big.ranges[i].last);
    }

    std::cout << "  -> PASSED\n";
}

void test_paths_frame_round_trip()
{
    std::cout << "[TEST] PATHS frame round trip...\n";

    PathsFrame paths;
    paths.paths = {{0, 15000}, {1, PATHS_RTT_POTENTIALLY_FAILED}};

    Packet pkt(FLAG_NONE, 42, 0, 7);
    pkt.add_frame(Frame::make_paths(paths));
    assert(pkt.is_retransmittable()); // a lost PATHS frame is sent again

    std::vector<uint8_t> raw = pkt.serialize();
    assert(raw.size() == 18 + PATHS_FRAME_HEADER_SIZE + 2 * PATHS_ENTRY_SIZE);
    assert(raw[18] == FRAME_PATHS);
    assert(raw[19] == 2); // number of paths
    assert(raw[20] == 0); // first Path ID
    assert(raw[25] == 1); // second Path ID
    assert(raw[26] == 0xFF && raw[29] == 0xFF); // "potentially failed" marker

    Packet parsed;
    assert(Packet::deserialize(raw.data(), raw.size(), parsed));
    const Frame &f = parsed.get_frames()[0];
    assert(f.type == FRAME_PATHS);
    assert(f.paths.paths.size() == 2);
    assert(f.paths.paths[0].path_id == 0 && f.paths.paths[0].rtt_us == 15000);
    assert(f.paths.paths[1].path_id == 1 && f.paths.paths[1].rtt_us == PATHS_RTT_POTENTIALLY_FAILED);

    std::cout << "  -> PASSED\n";
}

void test_add_address_frame_round_trip()
{
    std::cout << "[TEST] ADD_ADDRESS frame round trip...\n";

    std::vector<NetworkAddress> addresses(2);
    addresses[0].ip = "10.0.0.2";
    addresses[0].port = 4433;
    addresses[1].ip = "192.168.56.20";
    addresses[1].port = 65000;

    Packet pkt(FLAG_NONE, 42, 0, 3);
    pkt.add_frame(Frame::make_add_address(addresses));
    assert(pkt.is_retransmittable()); // a lost ADD_ADDRESS frame is sent again

    std::vector<uint8_t> raw = pkt.serialize();
    assert(raw.size() == 18 + ADD_ADDRESS_FRAME_HEADER_SIZE + 2 * ADD_ADDRESS_ENTRY_SIZE);
    assert(raw[18] == FRAME_ADD_ADDRESS);
    assert(raw[19] == 2); // number of addresses
    assert(raw[20] == 10 && raw[21] == 0 && raw[22] == 0 && raw[23] == 2); // 10.0.0.2
    assert(raw[24] == 0x11 && raw[25] == 0x51);                            // port 4433 = 0x1151

    Packet parsed;
    assert(Packet::deserialize(raw.data(), raw.size(), parsed));
    const Frame &f = parsed.get_frames()[0];
    assert(f.type == FRAME_ADD_ADDRESS);
    assert(f.add_address.addresses.size() == 2);
    assert(f.add_address.addresses[0].ip == "10.0.0.2" && f.add_address.addresses[0].port == 4433);
    assert(f.add_address.addresses[1].ip == "192.168.56.20" && f.add_address.addresses[1].port == 65000);

    // Cut off, or no address at all -> rejected
    Packet out;
    assert(!Packet::deserialize(raw.data(), raw.size() - 1, out));
    std::vector<uint8_t> bad = raw;
    bad[19] = 0;
    assert(!Packet::deserialize(bad.data(), bad.size(), out));

    // Something that is not an IPv4 address cannot be put in a frame
    std::vector<NetworkAddress> wrong(1);
    wrong[0].ip = "not-an-address";
    wrong[0].port = 1;
    Packet wrong_pkt(FLAG_NONE, 1, 0, 1);
    wrong_pkt.add_frame(Frame::make_add_address(wrong));
    bool threw = false;
    try
    {
        wrong_pkt.serialize();
    }
    catch (const std::invalid_argument & /*e*/)
    {
        threw = true;
    }
    assert(threw);

    std::cout << "  -> PASSED\n";
}

void test_connection_close_frame_round_trip()
{
    std::cout << "[TEST] CONNECTION_CLOSE frame round trip...\n";

    Packet pkt(FLAG_NONE, 42, 1, 9);
    pkt.add_frame(Frame::make_connection_close(CLOSE_NO_ERROR, "transfer complete"));

    std::vector<uint8_t> raw = pkt.serialize();
    assert(raw.size() == 18 + CONNECTION_CLOSE_FRAME_HEADER_SIZE + 17);
    assert(raw[18] == FRAME_CONNECTION_CLOSE);

    Packet parsed;
    assert(Packet::deserialize(raw.data(), raw.size(), parsed));
    const Frame &f = parsed.get_frames()[0];
    assert(f.type == FRAME_CONNECTION_CLOSE);
    assert(f.connection_close.error_code == CLOSE_NO_ERROR);
    assert(f.connection_close.reason == "transfer complete");

    // An empty reason and an error code
    Packet pkt2(FLAG_NONE, 42, 1, 10);
    pkt2.add_frame(Frame::make_connection_close(CLOSE_PROTOCOL_ERROR, ""));
    std::vector<uint8_t> raw2 = pkt2.serialize();
    assert(raw2.size() == 18 + CONNECTION_CLOSE_FRAME_HEADER_SIZE);
    assert(Packet::deserialize(raw2.data(), raw2.size(), parsed));
    assert(parsed.get_frames()[0].connection_close.error_code == CLOSE_PROTOCOL_ERROR);
    assert(parsed.get_frames()[0].connection_close.reason.empty());

    // A very long reason is shortened, the frame always fits in a packet
    Packet pkt3(FLAG_NONE, 42, 1, 11);
    pkt3.add_frame(Frame::make_connection_close(CLOSE_NO_ERROR, std::string(5000, 'x')));
    std::vector<uint8_t> raw3 = pkt3.serialize();
    assert(Packet::deserialize(raw3.data(), raw3.size(), parsed));
    assert(parsed.get_frames()[0].connection_close.reason.size() == MAX_CLOSE_REASON_LENGTH);

    // The reason is cut off -> rejected
    Packet out;
    assert(!Packet::deserialize(raw.data(), raw.size() - 3, out));

    // The header alone can be read without parsing the frames
    uint64_t connection_id = 0;
    uint8_t path_id = 0;
    assert(Packet::peek_header(raw.data(), raw.size(), connection_id, path_id));
    assert(connection_id == 42 && path_id == 1);
    assert(!Packet::peek_header(raw.data(), 17, connection_id, path_id));

    std::cout << "  -> PASSED\n";
}

void test_multiple_frames_in_one_packet()
{
    std::cout << "[TEST] Several frames in one packet...\n";

    AckFrame ack;
    ack.path_id = 0;
    ack.largest_acked = 7;
    ack.ranges = {{1, 7}};

    Packet pkt(FLAG_NONE, 55, 0, 9);
    pkt.add_frame(Frame::make_ack(ack));
    pkt.add_frame(Frame::make_window_update(0, 65536ULL));
    pkt.add_frame(Frame::make_stream(1, 0, false, std::vector<uint8_t>(100, 'A')));
    pkt.add_frame(Frame::make_stream(3, 500, false, std::vector<uint8_t>(50, 'B')));
    pkt.add_frame(Frame::make_ping());

    std::vector<uint8_t> raw = pkt.serialize();
    assert(raw.size() == pkt.size());

    Packet parsed;
    assert(Packet::deserialize(raw.data(), raw.size(), parsed));
    assert(parsed.get_frames().size() == 5);
    assert(parsed.get_frames()[0].type == FRAME_ACK);
    assert(parsed.get_frames()[1].type == FRAME_WINDOW_UPDATE);
    assert(parsed.get_frames()[1].window_update.stream_id == 0);
    assert(parsed.get_frames()[1].window_update.byte_offset == 65536ULL);
    assert(parsed.get_frames()[2].type == FRAME_STREAM);
    assert(parsed.get_frames()[2].stream.stream_id == 1);
    assert(parsed.get_frames()[2].stream.data.size() == 100);
    assert(parsed.get_frames()[3].stream.stream_id == 3);
    assert(parsed.get_frames()[3].stream.offset == 500);
    assert(parsed.get_frames()[4].type == FRAME_PING);
    assert(parsed.is_retransmittable());

    std::cout << "  -> PASSED\n";
}

void test_flags_manipulation()
{
    std::cout << "[TEST] Flag manipulation...\n";
    Packet pkt;
    assert(pkt.get_flags() == FLAG_NONE);

    pkt.set_flag(0x01);
    assert(pkt.has_flag(0x01));
    assert(!pkt.has_flag(0x02));

    pkt.set_flag(0x02);
    assert(pkt.get_flags() == 0x03);

    pkt.clear_flag(0x01);
    assert(!pkt.has_flag(0x01));
    assert(pkt.has_flag(0x02));

    pkt.add_frame(Frame::make_ping());
    std::vector<uint8_t> buf = pkt.serialize();
    Packet parsed;
    assert(Packet::deserialize(buf.data(), buf.size(), parsed));
    assert(parsed.get_flags() == 0x02);

    std::cout << "  -> PASSED\n";
}

void test_packet_size_limit()
{
    std::cout << "[TEST] Maximum packet size (1350 bytes)...\n";

    // 1. A packet of exactly MAX_PACKET_SIZE bytes -> Success
    size_t max_data = MAX_PACKET_SIZE - Packet::HEADER_SIZE - STREAM_FRAME_HEADER_SIZE;
    Packet max_pkt(FLAG_NONE, 100ULL, 0, 1ULL);
    max_pkt.add_frame(Frame::make_stream(1, 0, false, std::vector<uint8_t>(max_data, 0xAB)));
    assert(max_pkt.size() == MAX_PACKET_SIZE);

    std::vector<uint8_t> max_buf = max_pkt.serialize();
    assert(max_buf.size() == MAX_PACKET_SIZE);

    Packet parsed_max;
    assert(Packet::deserialize(max_buf.data(), max_buf.size(), parsed_max));
    assert(parsed_max.get_frames()[0].stream.data.size() == max_data);
    assert(parsed_max.get_frames()[0].stream.data[max_data - 1] == 0xAB);

    // 2. One byte more -> serialize must throw std::length_error
    Packet overflow_pkt(FLAG_NONE, 100ULL, 0, 2ULL);
    overflow_pkt.add_frame(Frame::make_stream(1, 0, false, std::vector<uint8_t>(max_data + 1, 0xCD)));

    bool threw_exception = false;
    try
    {
        overflow_pkt.serialize();
    }
    catch (const std::length_error & /*e*/)
    {
        threw_exception = true;
    }
    assert(threw_exception);

    std::cout << "  -> PASSED\n";
}

void test_corrupted_and_truncated_deserialization()
{
    std::cout << "[TEST] Error handling for malformed / truncated packets...\n";

    Packet out;
    // 1. Null buffer
    assert(!Packet::deserialize(nullptr, 18, out));

    // 2. Length less than header size (17 bytes)
    uint8_t short_buf[17] = {0};
    assert(!Packet::deserialize(short_buf, sizeof(short_buf), out));

    // 3. Header only, no frame -> Rejected
    uint8_t header_only[18] = {0};
    assert(!Packet::deserialize(header_only, sizeof(header_only), out));

    // 4. STREAM frame says 10 bytes of data but the buffer ends early -> Rejected
    Packet pkt10(FLAG_NONE, 1ULL, 0, 1ULL);
    pkt10.add_frame(Frame::make_stream(1, 0, false, std::vector<uint8_t>(10, 'A')));
    std::vector<uint8_t> full10 = pkt10.serialize();
    assert(full10.size() == 18 + 16 + 10);
    assert(!Packet::deserialize(full10.data(), full10.size() - 5, out));

    // 5. Extra bytes after the last frame that are not a valid frame -> Rejected
    std::vector<uint8_t> extra_bytes_buf = full10;
    extra_bytes_buf.push_back(0xFF); // unknown frame type
    assert(!Packet::deserialize(extra_bytes_buf.data(), extra_bytes_buf.size(), out));

    // 6. Truncated in the middle of a frame header -> Rejected
    assert(!Packet::deserialize(full10.data(), 18 + 5, out));

    // 7. Invalid ACK frames -> Rejected. Start from a valid one and damage one byte at a time.
    AckFrame ack;
    ack.path_id = 0;
    ack.largest_acked = 45;
    ack.ranges = {{40, 45}, {35, 37}, {10, 10}};
    Packet good(FLAG_NONE, 1, 0, 1);
    good.add_frame(Frame::make_ack(ack));
    std::vector<uint8_t> valid = good.serialize();
    assert(Packet::deserialize(valid.data(), valid.size(), out));

    std::vector<uint8_t> bad = valid;
    bad[33] = 0; // no range at all
    assert(!Packet::deserialize(bad.data(), bad.size(), out));

    bad = valid;
    bad[34] = 45; // first range would start at packet number 0
    assert(!Packet::deserialize(bad.data(), bad.size(), out));

    bad = valid;
    bad[35] = 0; // gap of 0: two ranges that touch are really one range
    assert(!Packet::deserialize(bad.data(), bad.size(), out));

    bad = valid;
    bad[37] = 34; // gap so large that the next range would end at packet number 0
    assert(!Packet::deserialize(bad.data(), bad.size(), out));

    bad = valid;
    bad[38] = 10; // last range would start at packet number 0
    assert(!Packet::deserialize(bad.data(), bad.size(), out));

    bad = valid;
    bad.pop_back(); // the last range is cut off
    assert(!Packet::deserialize(bad.data(), bad.size(), out));

    // An ACK built with ranges in the wrong order is not written as it is: only the valid
    // leading ranges are sent, so the peer never receives an inconsistent ACK
    AckFrame unordered;
    unordered.largest_acked = 20;
    unordered.ranges = {{15, 20}, {17, 18}};
    Packet partial(FLAG_NONE, 1, 0, 1);
    partial.add_frame(Frame::make_ack(unordered));
    std::vector<uint8_t> partial_raw = partial.serialize();
    assert(Packet::deserialize(partial_raw.data(), partial_raw.size(), out));
    assert(out.get_frames()[0].ack.ranges.size() == 1);

    // 8. Invalid PATHS frames -> Rejected
    PathsFrame paths;
    paths.paths = {{0, 15000}, {1, PATHS_RTT_POTENTIALLY_FAILED}};
    Packet paths_pkt(FLAG_NONE, 1, 0, 1);
    paths_pkt.add_frame(Frame::make_paths(paths));
    std::vector<uint8_t> paths_raw = paths_pkt.serialize();
    assert(Packet::deserialize(paths_raw.data(), paths_raw.size(), out));
    assert(!Packet::deserialize(paths_raw.data(), paths_raw.size() - 1, out)); // cut off

    bad = paths_raw;
    bad[19] = 0; // no path listed
    assert(!Packet::deserialize(bad.data(), bad.size(), out));

    std::cout << "  -> PASSED\n";
}

int main()
{
    std::cout << "=== Running MP-QUIC Mini Packet and Frame Tests ===\n";
    test_header_size_and_layout();
    test_stream_frame_round_trip();
    test_ack_frame_round_trip();
    test_ack_frame_256_ranges();
    test_paths_frame_round_trip();
    test_add_address_frame_round_trip();
    test_connection_close_frame_round_trip();
    test_multiple_frames_in_one_packet();
    test_flags_manipulation();
    test_packet_size_limit();
    test_corrupted_and_truncated_deserialization();
    std::cout << "=== All Packet Tests Passed Successfully! ===\n";
    return 0;
}
