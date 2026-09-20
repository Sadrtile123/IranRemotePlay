// RemotePlay - tests/test_protocol.cpp
// Message encode/decode round-trips and codec negotiation logic.
#include "networking/Protocol.h"

#include "TestHarness.hpp"

using namespace rp;
using namespace rp::proto;

namespace rp::proto {

// Test-only equality for round-trip verification. Declared before roundTrip
// so both ordinary lookup at the template definition and ADL find them.
bool operator==(const msg::ClientHello& a, const msg::ClientHello& b) {
    return a.clientName == b.clientName && a.sessionCode == b.sessionCode &&
           a.appVersion == b.appVersion;
}
bool operator==(const msg::HostReject& a, const msg::HostReject& b) {
    return a.reasonCode == b.reasonCode && a.reasonText == b.reasonText;
}
bool operator==(const msg::HostApproved&, const msg::HostApproved&) { return true; }
bool operator==(const msg::HostCapabilities& a, const msg::HostCapabilities& b) {
    return a.hostName == b.hostName && a.gameName == b.gameName &&
           a.captureMode == b.captureMode && a.width == b.width && a.height == b.height &&
           a.fps == b.fps && a.bitrateKbps == b.bitrateKbps &&
           a.offeredCodecs == b.offeredCodecs;
}
bool operator==(const msg::ClientCapabilities& a, const msg::ClientCapabilities& b) {
    return a.supportedCodecs == b.supportedCodecs;
}
bool operator==(const msg::NegotiationResult& a, const msg::NegotiationResult& b) {
    return a.accepted == b.accepted && a.codec == b.codec && a.width == b.width &&
           a.height == b.height && a.fps == b.fps && a.bitrateKbps == b.bitrateKbps &&
           a.note == b.note;
}
bool operator==(const msg::Ping& a, const msg::Ping& b) { return a.epochMs == b.epochMs; }
bool operator==(const msg::Pong& a, const msg::Pong& b) { return a.echoEpochMs == b.echoEpochMs; }
bool operator==(const msg::InputPermission& a, const msg::InputPermission& b) {
    return a.controller == b.controller && a.keyboard == b.keyboard && a.mouse == b.mouse &&
           a.vibration == b.vibration;
}
bool operator==(const msg::Kick& a, const msg::Kick& b) { return a.reason == b.reason; }
bool operator==(const msg::Bye& a, const msg::Bye& b) { return a.reason == b.reason; }

} // namespace rp::proto

namespace {

template <typename T>
bool roundTrip(const Envelope& in, Id expectedType) {
    const auto payload = encodeMessage(in);
    const auto out = decodeMessage(static_cast<uint16_t>(in.type), payload.data(),
                                    static_cast<uint32_t>(payload.size()));
    if (!out) return false;
    if (out->type != expectedType) return false;
    const T* lhs = std::get_if<T>(&in.body);
    const T* rhs = std::get_if<T>(&out->body);
    return lhs && rhs && *lhs == *rhs;
}

} // namespace

RP_TEST(client_hello_roundtrip) {
    Envelope e;
    e.type = Id::ClientHello;
    e.body = msg::ClientHello{"Mahdyar", "ABC7-K92P", "0.1.0"};
    CHECK(roundTrip<msg::ClientHello>(e, Id::ClientHello));
}

RP_TEST(host_reject_roundtrip) {
    Envelope e;
    e.type = Id::HostReject;
    e.body = msg::HostReject{2, "rejected by host"};
    CHECK(roundTrip<msg::HostReject>(e, Id::HostReject));
}

RP_TEST(host_approved_roundtrip) {
    Envelope e;
    e.type = Id::HostApproved;
    e.body = msg::HostApproved{};
    CHECK(roundTrip<msg::HostApproved>(e, Id::HostApproved));
}

RP_TEST(host_capabilities_roundtrip) {
    msg::HostCapabilities caps;
    caps.hostName = "Mahdyar's PC";
    caps.gameName = "Rayman Legends";
    caps.captureMode = 0;
    caps.width = 1920;
    caps.height = 1080;
    caps.fps = 60;
    caps.bitrateKbps = 8000;
    caps.offeredCodecs = {common::VideoCodec::Hevc, common::VideoCodec::H264};

    Envelope e;
    e.type = Id::HostCapabilities;
    e.body = caps;
    CHECK(roundTrip<msg::HostCapabilities>(e, Id::HostCapabilities));
}

RP_TEST(client_capabilities_roundtrip) {
    Envelope e;
    e.type = Id::ClientCapabilities;
    e.body = msg::ClientCapabilities{{common::VideoCodec::H264}};
    CHECK(roundTrip<msg::ClientCapabilities>(e, Id::ClientCapabilities));
}

RP_TEST(negotiation_result_roundtrip) {
    msg::NegotiationResult r;
    r.accepted = true;
    r.codec = 1;
    r.width = 2560;
    r.height = 1440;
    r.fps = 120;
    r.bitrateKbps = 12000;
    r.note = "HEVC";

    Envelope e;
    e.type = Id::NegotiationResult;
    e.body = r;
    CHECK(roundTrip<msg::NegotiationResult>(e, Id::NegotiationResult));
}

RP_TEST(ping_pong_roundtrip) {
    Envelope p;
    p.type = Id::Ping;
    p.body = msg::Ping{1234567890};
    CHECK(roundTrip<msg::Ping>(p, Id::Ping));

    Envelope q;
    q.type = Id::Pong;
    q.body = msg::Pong{1234567890};
    CHECK(roundTrip<msg::Pong>(q, Id::Pong));
}

RP_TEST(input_permission_roundtrip) {
    Envelope e;
    e.type = Id::InputPermission;
    e.body = msg::InputPermission{true, false, false, true};
    CHECK(roundTrip<msg::InputPermission>(e, Id::InputPermission));
}

RP_TEST(kick_bye_roundtrip) {
    Envelope k;
    k.type = Id::Kick;
    k.body = msg::Kick{"Kicked by host"};
    CHECK(roundTrip<msg::Kick>(k, Id::Kick));

    Envelope b;
    b.type = Id::Bye;
    b.body = msg::Bye{"Disconnected by user"};
    CHECK(roundTrip<msg::Bye>(b, Id::Bye));
}

RP_TEST(unknown_id_rejected) {
    const uint8_t dummy[4] = {0, 0, 0, 0};
    CHECK(!decodeMessage(0x00FF, dummy, sizeof(dummy)).has_value());
    CHECK(!knownId(0x00FF));
    CHECK(knownId(static_cast<uint16_t>(Id::ClientHello)));
}

RP_TEST(truncated_payload_rejected) {
    Envelope e;
    e.type = Id::ClientHello;
    e.body = msg::ClientHello{"Mahdyar", "ABC7-K92P", "0.1.0"};
    const auto payload = encodeMessage(e);
    // Cut away the last byte -> decode must fail, not truncate silently.
    CHECK(!decodeMessage(static_cast<uint16_t>(Id::ClientHello), payload.data(),
                         static_cast<uint32_t>(payload.size() - 1)).has_value());
}

RP_TEST(trailing_garbage_rejected) {
    Envelope e;
    e.type = Id::Ping;
    e.body = msg::Ping{42};
    auto payload = encodeMessage(e);
    payload.push_back(0x00); // extra byte
    CHECK(!decodeMessage(static_cast<uint16_t>(Id::Ping), payload.data(),
                         static_cast<uint32_t>(payload.size())).has_value());
}

RP_TEST(codec_negotiation_prefers_host_order) {
    const auto r1 = negotiateCodec({common::VideoCodec::Hevc, common::VideoCodec::H264},
                                   {common::VideoCodec::H264, common::VideoCodec::Hevc});
    CHECK(r1.has_value());
    CHECK_EQ(*r1, common::VideoCodec::Hevc); // host preference wins

    const auto r2 = negotiateCodec({common::VideoCodec::Hevc, common::VideoCodec::H264},
                                   {common::VideoCodec::H264});
    CHECK(r2.has_value());
    CHECK_EQ(*r2, common::VideoCodec::H264); // fallback

    const auto r3 = negotiateCodec({common::VideoCodec::Av1}, {common::VideoCodec::H264});
    CHECK(!r3.has_value()); // no overlap

    const auto r4 = negotiateCodec({}, {common::VideoCodec::H264});
    CHECK(!r4.has_value()); // empty host list
}

RP_TEST_MAIN("protocol")
