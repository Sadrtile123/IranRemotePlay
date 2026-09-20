// RemotePlay - networking/Protocol.h
// Control-channel message definitions (Phase 1: TCP session handshake,
// capability/codec negotiation, approval, heartbeat, kick, input permissions).
//
// Handshake flow:
//   CLIENT                                HOST
//     | --- CLIENT_HELLO ---------------> |   validate session code
//     |                                   |   (human approval: ACCEPT / REJECT)
//     | <-- HOST_REJECT ----------------- |   (bad code / rejected / full)
//     | <-- HOST_APPROVED --------------- |
//     | <-- HOST_CAPABILITIES ----------- |
//     | --- CLIENT_CAPABILITIES --------> |
//     | <-- NEGOTIATION_RESULT ---------- |   state: CONNECTED
//     | --- PING / PONG ----------------> |   both directions
//     | <-- INPUT_PERMISSION ------------ |   on host toggles
//     | <-- KICK ------------------------ |
//     | --- BYE ------------------------> |
#pragma once

#include "common/Types.h"
#include "networking/Packet.h"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace rp::proto {

// Message identifiers. Stable values; do not renumber.
enum class Id : uint16_t {
    ClientHello = 0x0001,
    HostReject = 0x0002,
    HostApproved = 0x0003,
    HostCapabilities = 0x0004,
    ClientCapabilities = 0x0005,
    NegotiationResult = 0x0006,
    Ping = 0x0007,
    Pong = 0x0008,
    InputPermission = 0x0009,
    Kick = 0x000A,
    Bye = 0x000B,

    // Reserved for the streaming phase (documented, not sent yet).
    StreamStart = 0x0010,
    StreamStop = 0x0011,
    KeyframeRequest = 0x0012,
};

[[nodiscard]] bool knownId(uint16_t raw);
[[nodiscard]] const char* idName(Id id);

// Planned UDP media datagram types (spec section 5). Phase 4.
enum class UdpType : uint8_t {
    Video = 0,
    Audio = 1,
    Input = 2,
    Control = 3,
    Ping = 4,
    Pong = 5,
    KeyframeRequest = 6,
    StreamStart = 7,
    StreamStop = 8,
};

// Host reject / disconnect reasons.
enum class Reason : uint16_t {
    Ok = 0,
    BadSessionCode = 1,
    RejectedByHost = 2,
    ProtocolError = 3,
    UnsupportedVersion = 4,
    InactivityTimeout = 5,
    Kicked = 6,
    ServerFull = 7,
    NoCommonCodec = 8,
};

[[nodiscard]] const char* reasonName(Reason r);

namespace msg {

struct ClientHello {
    std::string clientName;    // display name, e.g. "Mahdyar"
    std::string sessionCode;   // e.g. "ABC7-K92P"
    std::string appVersion;    // informational, e.g. "0.1.0"
};

struct HostReject {
    uint16_t reasonCode = 0;
    std::string reasonText;
};

struct HostApproved {};

struct HostCapabilities {
    std::string hostName;      // e.g. "Mahdyar's PC"
    std::string gameName;      // e.g. "Rayman Legends"
    uint8_t captureMode = 0;   // common::CaptureMode
    uint16_t width = 1920;
    uint16_t height = 1080;
    uint8_t fps = 60;
    uint32_t bitrateKbps = 8000;
    std::vector<common::VideoCodec> offeredCodecs; // preference order, H.264 always included
};

struct ClientCapabilities {
    std::vector<common::VideoCodec> supportedCodecs;
};

struct NegotiationResult {
    bool accepted = false;
    uint8_t codec = 0;         // common::VideoCodec
    uint16_t width = 1920;
    uint16_t height = 1080;
    uint8_t fps = 60;
    uint32_t bitrateKbps = 8000;
    std::string note;
};

struct Ping {
    uint64_t epochMs = 0;      // sender's steady-clock milliseconds
};

struct Pong {
    uint64_t echoEpochMs = 0;  // echoes the Ping value verbatim
};

struct InputPermission {
    bool controller = true;
    bool keyboard = false;
    bool mouse = false;
    bool vibration = true;
};

struct Kick {
    std::string reason;
};

struct Bye {
    std::string reason;
};

// Phase 4+ — sent by the host after negotiation, when streaming actually begins.
// The client targets `udpPort` with the UDP media transport and stamps every
// datagram with `sessionId` (spoof/misroute rejection on both ends).
struct StreamStart {
    uint16_t udpPort = 0;         // host UDP port for THIS client (player)
    uint32_t sessionId = 0;
    uint8_t  playerIndex = 0;     // 0..3 (PLAYER 1..4)
    uint8_t  codec = 0;           // common::VideoCodec
    uint16_t width = 1920;
    uint16_t height = 1080;
    uint8_t fps = 60;
    uint32_t bitrateKbps = 8000;
};

struct StreamStop {
    std::string reason;
};

struct KeyframeRequest {};

} // namespace msg

using Body = std::variant<msg::ClientHello, msg::HostReject, msg::HostApproved, msg::HostCapabilities,
                          msg::ClientCapabilities, msg::NegotiationResult, msg::Ping, msg::Pong,
                          msg::InputPermission, msg::Kick, msg::Bye,
                          msg::StreamStart, msg::StreamStop, msg::KeyframeRequest>;

struct Envelope {
    Id type = Id::ClientHello;
    Body body;
};

// Serializes a message body to payload bytes.
[[nodiscard]] std::vector<uint8_t> encodeMessage(const Envelope& e);

// Decodes a payload according to the given type id. Returns nullopt on
// malformed payloads (unknown id, truncated data, trailing garbage).
[[nodiscard]] std::optional<Envelope> decodeMessage(uint16_t rawType, const uint8_t* data,
                                                    uint32_t size);

// Picks the first host-preferred codec that the client also supports.
[[nodiscard]] std::optional<common::VideoCodec> negotiateCodec(
    const std::vector<common::VideoCodec>& hostPreference,
    const std::vector<common::VideoCodec>& clientSupported);

} // namespace rp::proto
