// Phase 4 tests — UDP protocol logic + real loopback transport.
// Runs on every platform (pure logic + localhost sockets).

#include <algorithm>
#include <string>
#include <utility>
#include <vector>
#include "networking/UdpProtocol.h"
#include "networking/UdpTransport.h"

#include "TestHarness.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

using namespace rp::net;

RP_TEST(udp_header_roundtrip) {
    UdpHeader h;
    h.type = 0; h.flags = 0x03; h.sessionId = 0xDEADBEEFu; h.sequence = 123456789ull;
    h.fragmentIndex = 2; h.fragmentCount = 7; h.timestampNs = 987654321ull; h.payloadSize = 100;

    uint8_t buf[kUdpHeaderSize];
    encodeUdpHeader(buf, h);
    UdpHeader out;
    CHECK(decodeUdpHeader(buf, sizeof(buf) + 100, out));
    CHECK_EQ(h.sessionId, out.sessionId);
    CHECK_EQ(h.sequence, out.sequence);
    CHECK_EQ(h.fragmentIndex, out.fragmentIndex);
    CHECK_EQ(h.fragmentCount, out.fragmentCount);
    CHECK_EQ(h.timestampNs, out.timestampNs);
    CHECK_EQ(h.payloadSize, out.payloadSize);
    CHECK_EQ(h.flags, out.flags);

    // Malformed inputs rejected.
    buf[0] = 0x11;                                    // bad magic
    CHECK(!decodeUdpHeader(buf, sizeof(buf) + 100, out));
    buf[0] = 0x52; buf[1] = 0x52; buf[2] = 0x02;      // bad version
    CHECK(!decodeUdpHeader(buf, sizeof(buf) + 100, out));
    uint8_t tiny[10];
    CHECK(!decodeUdpHeader(tiny, 10, out));           // too small
}

RP_TEST(udp_fragment_reassembly) {
    // 5000-byte frame -> 5 fragments.
    std::vector<uint8_t> payload(5000);
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>(i * 31 + 7);

    const uint16_t frags = fragmentCountFor(payload.size());
    CHECK_EQ(frags, uint16_t(5));

    UdpHeader h;
    h.type = 0; h.flags = static_cast<uint8_t>(UdpFlag::Fragment) | static_cast<uint8_t>(UdpFlag::Keyframe);
    h.sessionId = 42; h.sequence = 100; h.fragmentCount = frags; h.timestampNs = 555; h.payloadSize = 0;

    FragmentReassembler reasm(UdpType::Video);
    AssembledFrame frame;
    bool completed = false;
    for (uint16_t i = 0; i < frags; ++i) {
        const size_t off = static_cast<size_t>(i) * kMaxUdpPayload;
        const size_t n = std::min(kMaxUdpPayload, payload.size() - off);
        h.fragmentIndex = i;
        h.payloadSize = static_cast<uint16_t>(n);
        const bool c = reasm.feed(h, payload.data() + off, frame);
        if (i + 1 == frags) { CHECK(c); completed = true; }
        else CHECK(!c);
    }
    CHECK(completed);
    CHECK_EQ(frame.data.size(), payload.size());
    CHECK(std::memcmp(payload.data(), frame.data.data(), payload.size()) == 0);
    CHECK(frame.keyframe);
    CHECK_EQ(frame.sequence, uint64_t(100));

    // Out-of-order fragments reassemble too.
    FragmentReassembler reasm2(UdpType::Video);
    const uint16_t order[5] = { 2, 0, 4, 1, 3 };
    AssembledFrame frame2;
    for (uint16_t k = 0; k < 5; ++k) {
        const uint16_t i = order[k];
        const size_t off = static_cast<size_t>(i) * kMaxUdpPayload;
        const size_t n = std::min(kMaxUdpPayload, payload.size() - off);
        h.fragmentIndex = i;
        h.payloadSize = static_cast<uint16_t>(n);
        const bool c = reasm2.feed(h, payload.data() + off, frame2);
        CHECK_EQ(c, (k == 4));
    }
    CHECK(std::memcmp(payload.data(), frame2.data.data(), payload.size()) == 0);

    // Duplicate fragment ignored (last one re-fed).
    CHECK(!reasm2.feed(h, payload.data(), frame2));

    // Single-fragment frame completes immediately.
    UdpHeader single;
    single.type = 1; single.sessionId = 42; single.sequence = 200; single.fragmentCount = 1;
    single.fragmentIndex = 0; single.timestampNs = 1; single.payloadSize = 10;
    uint8_t small[10]{ 1,2,3,4,5,6,7,8,9,10 };
    AssembledFrame f3;
    CHECK(reasm2.feed(single, small, f3));
    CHECK_EQ(f3.data.size(), size_t(10));
    CHECK_EQ(f3.sequence, uint64_t(200));
}

RP_TEST(udp_sequence_tracker) {
    SequenceTracker t;
    CHECK_EQ(t.track(10), int64_t(0));   // first
    CHECK_EQ(t.track(11), int64_t(1));   // in order
    CHECK_EQ(t.track(11), int64_t(1));   // duplicate
    CHECK_EQ(t.track(14), int64_t(3));   // gap of 2 lost (12,13) -> returns gap 3
    CHECK_EQ(t.lost(), uint64_t(2));
    CHECK_EQ(t.duplicates(), uint64_t(1));
    CHECK_EQ(t.track(12), int64_t(0));   // reordered first arrival (late, not duplicate)
    CHECK_EQ(t.duplicates(), uint64_t(1));
    CHECK_EQ(t.late(), uint64_t(1));
    CHECK_EQ(t.track(15), int64_t(1));
    CHECK_EQ(t.lost(), uint64_t(2));
}

RP_TEST(udp_jitter_buffer) {
    JitterBuffer jb(20);
    AssembledFrame a, b;
    a.sequence = 1; a.data = { 1 };
    b.sequence = 2; b.data = { 2 };
    const uint64_t t0 = 1'000'000'000ull;
    jb.push(std::move(a), t0);
    std::vector<AssembledFrame> out;
    jb.pop(out, t0 + 5'000'000);          // 5ms waited, not enough
    CHECK(out.empty());
    jb.push(std::move(b), t0 + 10'000'000);
    jb.pop(out, t0 + 25'000'000);         // first waited 25ms -> pops, second only 15ms -> stays
    CHECK_EQ(out.size(), size_t(1));
    CHECK_EQ(out[0].sequence, uint64_t(1));
    jb.pop(out, t0 + 35'000'000);         // second now waited 25ms -> pops
    CHECK_EQ(out.size(), size_t(2));
    CHECK_EQ(out[1].sequence, uint64_t(2));
}

RP_TEST(udp_transport_loopback) {
    UdpTransport host, client;
    std::string err;
    CHECK(host.bind("127.0.0.1", 0, &err));
    CHECK(client.bind("127.0.0.1", 0, &err));
    host.setSessionId(7);
    client.setSessionId(7);
    CHECK(client.setPeer("127.0.0.1", host.localPort(), &err));
    host.setDropUnknownRemote(false);      // host learns endpoint from first packet

    // Ping/pong RTT over localhost.
    client.startPings(20);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto st = client.stats();
    CHECK(st.avgRttMs > 0.0 && st.avgRttMs < 100.0);

    // Send large video frames (fragmented) client -> host.
    std::vector<uint8_t> frameData(4096, 0xAB);
    std::atomic<int> videoFrames{ 0 };
    std::atomic<bool> sawKeyframe{ false };
    std::atomic<bool> stop{ false };
    std::thread consumer([&] {
        std::vector<AssembledFrame> out;
        while (!stop.load()) {
            host.pollVideo(steadyNowNs(), out);
            for (auto& f : out) { videoFrames.fetch_add(1); if (f.keyframe) sawKeyframe.store(true); }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    for (int i = 0; i < 20; ++i) {
        client.sendFrame(UdpType::Video, i == 0, frameData.data(), frameData.size(), steadyNowNs());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    stop.store(true);
    consumer.join();
    CHECK(videoFrames.load() >= 19);      // tolerate one early race
    CHECK(sawKeyframe.load());

    // Small control datagram host -> client.
    std::atomic<int> controlSeen{ 0 };
    client.setControlCallback([&](UdpType, const std::vector<uint8_t>&) { controlSeen.fetch_add(1); });
    host.sendSmall(UdpType::StreamStart, "go", 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(controlSeen.load() >= 1);

    // Input frames bypass jitter: direct callback on host.
    std::atomic<int> inputSeen{ 0 };
    host.setInputCallback([&](const AssembledFrame&) { inputSeen.fetch_add(1); });
    for (int i = 0; i < 5; ++i) {
        uint8_t inputState[16]{};
        client.sendSmall(UdpType::Input, inputState, sizeof(inputState));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(inputSeen.load() >= 4);

    client.stopPings();
    client.stop();
    host.stop();
}

RP_TEST_MAIN("udp")
