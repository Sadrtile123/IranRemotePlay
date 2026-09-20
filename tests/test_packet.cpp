// RemotePlay - tests/test_packet.cpp
// Frame construction/parsing and byte (de)serialization round-trips.
#include "networking/Packet.h"

#include "TestHarness.hpp"

#include <cstring>

using namespace rp::net;

RP_TEST(frame_roundtrip) {
    const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    const auto frame = makeFrame(0x1234, 0xAB, payload, sizeof(payload));

    CHECK_EQ(frame.size(), size_t(kFrameHeaderSize + sizeof(payload)));

    const auto h = parseFrameHeader(frame.data(), frame.size());
    CHECK(h.has_value());
    CHECK_EQ(h->magic, kFrameMagic);
    CHECK_EQ(h->version, uint8_t(kFrameVersion));
    CHECK_EQ(h->type, uint16_t(0x1234));
    CHECK_EQ(h->flags, uint8_t(0xAB));
    CHECK_EQ(h->payloadSize, uint32_t(sizeof(payload)));
    CHECK(std::memcmp(frame.data() + kFrameHeaderSize, payload, sizeof(payload)) == 0);
}

RP_TEST(frame_empty_payload) {
    const auto frame = makeFrame(0x0001, std::vector<uint8_t>{});
    CHECK_EQ(frame.size(), size_t(kFrameHeaderSize));
    const auto h = parseFrameHeader(frame.data(), frame.size());
    CHECK(h.has_value());
    CHECK_EQ(h->payloadSize, uint32_t(0));
}

RP_TEST(header_rejects_bad_magic) {
    auto frame = makeFrame(1, std::vector<uint8_t>{});
    frame[0] = 0x99; // corrupt magic
    const auto h = parseFrameHeader(frame.data(), frame.size());
    CHECK(!h.has_value());
}

RP_TEST(header_rejects_bad_version) {
    auto frame = makeFrame(1, std::vector<uint8_t>{});
    frame[2] = 9; // corrupt version
    const auto h = parseFrameHeader(frame.data(), frame.size());
    CHECK(!h.has_value());
}

RP_TEST(header_rejects_oversize_payload) {
    auto frame = makeFrame(1, std::vector<uint8_t>{});
    // payloadSize field at offset 6..9 (little-endian)
    frame[6] = 0xFF;
    frame[7] = 0xFF;
    frame[8] = 0x00;
    frame[9] = 0x01; // 0x10000FFFF-ish > kMaxControlPayload
    const auto h = parseFrameHeader(frame.data(), frame.size());
    CHECK(!h.has_value());
}

RP_TEST(header_rejects_short_buffer) {
    const uint8_t small[4] = {0x50, 0x52, 0x01, 0x00};
    CHECK(!parseFrameHeader(small, sizeof(small)).has_value());
    CHECK(!parseFrameHeader(nullptr, 0).has_value());
}

RP_TEST(byte_writer_reader_roundtrip) {
    ByteWriter w;
    w.u8(0xAB);
    w.u16(0xCDEF);
    w.u32(0x12345678);
    w.u64(0x0123456789ABCDEFULL);
    w.i32(-1234567);
    w.boolean(true);
    w.str("hello");
    const uint8_t raw[] = {1, 2, 3};
    w.bytes(raw, sizeof(raw));

    ByteReader r(w.data());
    CHECK_EQ(r.u8(), uint8_t(0xAB));
    CHECK_EQ(r.u16(), uint16_t(0xCDEF));
    CHECK_EQ(r.u32(), uint32_t(0x12345678));
    CHECK_EQ(r.u64(), uint64_t(0x0123456789ABCDEFULL));
    CHECK_EQ(r.i32(), int32_t(-1234567));
    CHECK_EQ(r.boolean(), true);
    CHECK_EQ(r.str(), std::string("hello"));
    uint8_t out[3] = {};
    r.bytes(out, 3);
    CHECK(std::memcmp(out, raw, 3) == 0);
    CHECK(r.ok());
    CHECK_EQ(r.remaining(), size_t(0));
}

RP_TEST(reader_underflow_marks_failed) {
    const uint8_t two[2] = {1, 2};
    ByteReader r(two, 2);
    (void)r.u8();
    (void)r.u32(); // only 1 byte left -> fails
    CHECK(!r.ok());
    CHECK_EQ(r.u16(), uint16_t(0));
}

RP_TEST(reader_bad_string_length_marks_failed) {
    const uint8_t buf[2] = {0xFF, 0xFF}; // length 65535 with no data
    ByteReader r(buf, 2);
    CHECK_EQ(r.str(), std::string());
    CHECK(!r.ok());
}

RP_TEST(writer_caps_string_length) {
    ByteWriter w;
    std::string big(5000, 'x');
    w.str(big);
    ByteReader r(w.data());
    const std::string out = r.str();
    CHECK_EQ(out.size(), size_t(1024)); // protocol cap
    CHECK(r.ok());
}

RP_TEST_MAIN("packet")
