// RemotePlay - networking/Protocol.cpp
#include "networking/Protocol.h"

#include <algorithm>

namespace rp::proto {

bool knownId(uint16_t raw) {
    switch (static_cast<Id>(raw)) {
        case Id::ClientHello:
        case Id::HostReject:
        case Id::HostApproved:
        case Id::HostCapabilities:
        case Id::ClientCapabilities:
        case Id::NegotiationResult:
        case Id::Ping:
        case Id::Pong:
        case Id::InputPermission:
        case Id::Kick:
        case Id::Bye:
            return true;
        default:
            return false;
    }
}

const char* idName(Id id) {
    switch (id) {
        case Id::ClientHello: return "CLIENT_HELLO";
        case Id::HostReject: return "HOST_REJECT";
        case Id::HostApproved: return "HOST_APPROVED";
        case Id::HostCapabilities: return "HOST_CAPABILITIES";
        case Id::ClientCapabilities: return "CLIENT_CAPABILITIES";
        case Id::NegotiationResult: return "NEGOTIATION_RESULT";
        case Id::Ping: return "PING";
        case Id::Pong: return "PONG";
        case Id::InputPermission: return "INPUT_PERMISSION";
        case Id::Kick: return "KICK";
        case Id::Bye: return "BYE";
        case Id::StreamStart: return "STREAM_START";
        case Id::StreamStop: return "STREAM_STOP";
        case Id::KeyframeRequest: return "KEYFRAME_REQUEST";
    }
    return "UNKNOWN";
}

const char* reasonName(Reason r) {
    switch (r) {
        case Reason::Ok: return "ok";
        case Reason::BadSessionCode: return "bad session code";
        case Reason::RejectedByHost: return "rejected by host";
        case Reason::ProtocolError: return "protocol error";
        case Reason::UnsupportedVersion: return "unsupported protocol version";
        case Reason::InactivityTimeout: return "inactivity timeout";
        case Reason::Kicked: return "kicked by host";
        case Reason::ServerFull: return "session full";
        case Reason::NoCommonCodec: return "no common codec";
    }
    return "unknown";
}

namespace {

using net::ByteReader;
using net::ByteWriter;

void writeVideoCodecList(ByteWriter& w, const std::vector<common::VideoCodec>& list) {
    w.u8(static_cast<uint8_t>(std::min<size_t>(list.size(), 32)));
    for (size_t i = 0; i < list.size() && i < 32; ++i) {
        w.u8(static_cast<uint8_t>(list[i]));
    }
}

std::vector<common::VideoCodec> readVideoCodecList(ByteReader& r) {
    std::vector<common::VideoCodec> out;
    const uint8_t n = r.u8();
    out.reserve(n);
    for (uint8_t i = 0; i < n; ++i) {
        out.push_back(static_cast<common::VideoCodec>(r.u8()));
    }
    return out;
}

// Per-message writers ------------------------------------------------------

void writeMsg(ByteWriter& w, const msg::ClientHello& m) {
    w.str(m.clientName);
    w.str(m.sessionCode);
    w.str(m.appVersion);
}

void writeMsg(ByteWriter& w, const msg::HostReject& m) {
    w.u16(m.reasonCode);
    w.str(m.reasonText);
}

void writeMsg(ByteWriter& /*w*/, const msg::HostApproved&) {}

void writeMsg(ByteWriter& w, const msg::HostCapabilities& m) {
    w.str(m.hostName);
    w.str(m.gameName);
    w.u8(m.captureMode);
    w.u16(m.width);
    w.u16(m.height);
    w.u8(m.fps);
    w.u32(m.bitrateKbps);
    writeVideoCodecList(w, m.offeredCodecs);
}

void writeMsg(ByteWriter& w, const msg::ClientCapabilities& m) {
    writeVideoCodecList(w, m.supportedCodecs);
}

void writeMsg(ByteWriter& w, const msg::NegotiationResult& m) {
    w.boolean(m.accepted);
    w.u8(m.codec);
    w.u16(m.width);
    w.u16(m.height);
    w.u8(m.fps);
    w.u32(m.bitrateKbps);
    w.str(m.note);
}

void writeMsg(ByteWriter& w, const msg::Ping& m) { w.u64(m.epochMs); }

void writeMsg(ByteWriter& w, const msg::Pong& m) { w.u64(m.echoEpochMs); }

void writeMsg(ByteWriter& w, const msg::InputPermission& m) {
    w.boolean(m.controller);
    w.boolean(m.keyboard);
    w.boolean(m.mouse);
    w.boolean(m.vibration);
}

void writeMsg(ByteWriter& w, const msg::Kick& m) { w.str(m.reason); }

void writeMsg(ByteWriter& w, const msg::Bye& m) { w.str(m.reason); }

// Per-message readers ------------------------------------------------------

bool readMsg(ByteReader& r, msg::ClientHello& m) {
    m.clientName = r.str();
    m.sessionCode = r.str();
    m.appVersion = r.str();
    return r.ok();
}

bool readMsg(ByteReader& r, msg::HostReject& m) {
    m.reasonCode = r.u16();
    m.reasonText = r.str();
    return r.ok();
}

bool readMsg(ByteReader& r, msg::HostApproved&) { return r.ok() && r.remaining() == 0; }

bool readMsg(ByteReader& r, msg::HostCapabilities& m) {
    m.hostName = r.str();
    m.gameName = r.str();
    m.captureMode = r.u8();
    m.width = r.u16();
    m.height = r.u16();
    m.fps = r.u8();
    m.bitrateKbps = r.u32();
    m.offeredCodecs = readVideoCodecList(r);
    return r.ok();
}

bool readMsg(ByteReader& r, msg::ClientCapabilities& m) {
    m.supportedCodecs = readVideoCodecList(r);
    return r.ok();
}

bool readMsg(ByteReader& r, msg::NegotiationResult& m) {
    m.accepted = r.boolean();
    m.codec = r.u8();
    m.width = r.u16();
    m.height = r.u16();
    m.fps = r.u8();
    m.bitrateKbps = r.u32();
    m.note = r.str();
    return r.ok();
}

bool readMsg(ByteReader& r, msg::Ping& m) {
    m.epochMs = r.u64();
    return r.ok() && r.remaining() == 0;
}

bool readMsg(ByteReader& r, msg::Pong& m) {
    m.echoEpochMs = r.u64();
    return r.ok() && r.remaining() == 0;
}

bool readMsg(ByteReader& r, msg::InputPermission& m) {
    m.controller = r.boolean();
    m.keyboard = r.boolean();
    m.mouse = r.boolean();
    m.vibration = r.boolean();
    return r.ok() && r.remaining() == 0;
}

bool readMsg(ByteReader& r, msg::Kick& m) {
    m.reason = r.str();
    return r.ok();
}

bool readMsg(ByteReader& r, msg::Bye& m) {
    m.reason = r.str();
    return r.ok();
}

} // namespace

std::vector<uint8_t> encodeMessage(const Envelope& e) {
    net::ByteWriter w;
    std::visit([&w](const auto& m) { writeMsg(w, m); }, e.body);
    return w.take();
}

std::optional<Envelope> decodeMessage(uint16_t rawType, const uint8_t* data, uint32_t size) {
    if (!knownId(rawType)) return std::nullopt;
    const Id type = static_cast<Id>(rawType);
    net::ByteReader r(data, size);
    Envelope e;
    e.type = type;
    bool ok = false;

    // Read into a local value, then emplace it into the variant. (Reading
    // directly with std::get<> would hit the default-initialized alternative.)
    switch (type) {
        case Id::ClientHello: {
            msg::ClientHello m;
            ok = readMsg(r, m);
            if (ok) e.body = std::move(m);
            break;
        }
        case Id::HostReject: {
            msg::HostReject m;
            ok = readMsg(r, m);
            if (ok) e.body = std::move(m);
            break;
        }
        case Id::HostApproved: {
            msg::HostApproved m;
            ok = readMsg(r, m);
            if (ok) e.body = std::move(m);
            break;
        }
        case Id::HostCapabilities: {
            msg::HostCapabilities m;
            ok = readMsg(r, m);
            if (ok) e.body = std::move(m);
            break;
        }
        case Id::ClientCapabilities: {
            msg::ClientCapabilities m;
            ok = readMsg(r, m);
            if (ok) e.body = std::move(m);
            break;
        }
        case Id::NegotiationResult: {
            msg::NegotiationResult m;
            ok = readMsg(r, m);
            if (ok) e.body = std::move(m);
            break;
        }
        case Id::Ping: {
            msg::Ping m;
            ok = readMsg(r, m);
            if (ok) e.body = std::move(m);
            break;
        }
        case Id::Pong: {
            msg::Pong m;
            ok = readMsg(r, m);
            if (ok) e.body = std::move(m);
            break;
        }
        case Id::InputPermission: {
            msg::InputPermission m;
            ok = readMsg(r, m);
            if (ok) e.body = std::move(m);
            break;
        }
        case Id::Kick: {
            msg::Kick m;
            ok = readMsg(r, m);
            if (ok) e.body = std::move(m);
            break;
        }
        case Id::Bye: {
            msg::Bye m;
            ok = readMsg(r, m);
            if (ok) e.body = std::move(m);
            break;
        }
        default:
            return std::nullopt;
    }
    if (!ok) return std::nullopt;
    return e;
}

std::optional<common::VideoCodec> negotiateCodec(
    const std::vector<common::VideoCodec>& hostPreference,
    const std::vector<common::VideoCodec>& clientSupported) {
    for (const common::VideoCodec want : hostPreference) {
        if (std::find(clientSupported.begin(), clientSupported.end(), want) != clientSupported.end()) {
            return want;
        }
    }
    return std::nullopt;
}

} // namespace rp::proto
