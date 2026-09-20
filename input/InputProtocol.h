#pragma once
// Phases 8-11 — input wire protocol.
//
// Wire format v1 (both ends are x86-64 Windows; packed PODs, little-endian):
//   UdpType::Input datagram payload:
//     [1 byte kind]  0 = controller state, 1 = keyboard/mouse event
//     ...            struct follows
//   UdpType::Control datagram payload for rumble (host -> client):
//     "R" tag + left/right motor bytes.
//
// Controller states are sent as FULL states (idempotent: a lost packet just
// means one missed update; the next state supersedes it). Keyboard/mouse are
// EVENT packets (order matters; the loss of a keyup would stick a key, so
// input datagrams are small and unfragmented, and the host re-releases all
// keys when a client disconnects).

#include <cstdint>
#include <cstring>
#include <vector>

namespace rp::input {

// Gamepad button bits (XInput-compatible layout).
enum GamepadButton : uint16_t {
    BtnA = 1u << 0,  BtnB = 1u << 1,  BtnX = 1u << 2,  BtnY = 1u << 3,
    BtnLB = 1u << 4, BtnRB = 1u << 5, BtnBack = 1u << 6, BtnStart = 1u << 7,
    BtnGuide = 1u << 8, BtnLS = 1u << 9, BtnRS = 1u << 10,
    DUp = 1u << 11, DDown = 1u << 12, DLeft = 1u << 13, DRight = 1u << 14,
};

#pragma pack(push, 1)
struct GameControllerState {      // 22 bytes
    uint8_t  version = 1;
    uint8_t  connected = 0;
    uint16_t buttons = 0;
    uint8_t  leftTrigger = 0;     // 0..255
    uint8_t  rightTrigger = 0;    // 0..255
    int16_t  thumbLX = 0, thumbLY = 0, thumbRX = 0, thumbRY = 0;   // -32768..32767
    uint64_t timestampNs = 0;
};
static_assert(sizeof(GameControllerState) == 22, "wire format v1");

enum class InputEventType : uint8_t {
    KeyDown = 1,
    KeyUp = 2,
    MouseMove = 3,       // absolute normalized coords in x/y (0..65535)
    MouseButtonDown = 4, // `code` = 0 left, 1 right, 2 middle, 3 x1, 4 x2
    MouseButtonUp = 5,
    MouseWheel = 6,      // `delta` = wheel steps (positive = away)
};
static_assert(static_cast<int>(InputEventType::MouseWheel) == 6);

struct InputEvent {               // 18 bytes
    uint8_t  type = 0;            // InputEventType
    uint8_t  code = 0;            // key: Windows virtual-key code; mouse: button id
    uint16_t x = 0, y = 0;        // mouse: absolute 0..65535 normalized
    int16_t  delta = 0;           // wheel
    uint8_t  pad0 = 0;
    uint8_t  flags = 0;           // bit0: extended key (right ctrl/alt etc.)
    uint64_t timestampNs = 0;
};
static_assert(sizeof(InputEvent) == 18, "wire format v1");

struct RumbleCommand {            // 4 bytes (sent inside a Control datagram)
    uint8_t  tag = 'R';
    uint8_t  leftMotor = 0;       // 0..255
    uint8_t  rightMotor = 0;      // 0..255
    uint8_t  playerIndex = 0;     // 0..3
};
static_assert(sizeof(RumbleCommand) == 4);
#pragma pack(pop)

inline std::vector<uint8_t> packControllerState(const GameControllerState& s) {
    std::vector<uint8_t> v(1 + sizeof(s));
    v[0] = 0;                     // kind: controller
    std::memcpy(v.data() + 1, &s, sizeof(s));
    return v;
}

inline std::vector<uint8_t> packInputEvent(const InputEvent& e) {
    std::vector<uint8_t> v(1 + sizeof(e));
    v[0] = 1;                     // kind: event
    std::memcpy(v.data() + 1, &e, sizeof(e));
    return v;
}

inline bool unpackInput(const uint8_t* data, size_t size, GameControllerState& outState, InputEvent& outEvent) {
    if (size < 1) return false;
    if (data[0] == 0 && size == 1 + sizeof(GameControllerState)) {
        std::memcpy(&outState, data + 1, sizeof(outState));
        return true;
    }
    if (data[0] == 1 && size == 1 + sizeof(InputEvent)) {
        std::memcpy(&outEvent, data + 1, sizeof(outEvent));
        return true;
    }
    return false;
}

} // namespace rp::input
