// RemotePlay - common/Types.h
// Shared enums and small value types used across host, client and protocol layers.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rp::common {

// Connection lifecycle states, mirroring the session-state model in the spec.
enum class ConnectionState {
    Connecting,
    Authenticating,
    Connected,
    Streaming,
    Disconnected,
};

[[nodiscard]] const char* toString(ConnectionState s);
[[nodiscard]] std::string connectionStateString(ConnectionState s);

// Video codecs. Values are stable protocol codes (do not renumber).
enum class VideoCodec : uint8_t {
    H264 = 0,  // default compatibility codec
    Hevc = 1,  // optional
    Av1 = 2,   // optional
};

[[nodiscard]] const char* toString(VideoCodec c);
[[nodiscard]] std::optional<VideoCodec> videoCodecFromString(const std::string& name);
[[nodiscard]] std::string videoCodecString(VideoCodec c);

// What the host captures.
enum class CaptureMode : uint8_t {
    Window = 0,   // preferred
    Monitor = 1,  // fallback
};

[[nodiscard]] const char* toString(CaptureMode m);

// Audio codecs (Opus is the only one used; enum keeps the protocol extensible).
enum class AudioCodec : uint8_t {
    Opus = 0,
};

struct Resolution {
    uint32_t width = 1920;
    uint32_t height = 1080;

    [[nodiscard]] std::string toString() const;             // "1920x1080"
    [[nodiscard]] static std::optional<Resolution> parse(const std::string& text);
    [[nodiscard]] static const std::vector<Resolution>& presets(); // 720p/1080p/1440p/4K
    [[nodiscard]] bool operator==(const Resolution& o) const { return width == o.width && height == o.height; }
};

// Per-client input permissions. The host is the authority and pushes these to
// the client. Keyboard and mouse default to OFF (privacy-first).
struct InputPermissions {
    bool controller = true;
    bool keyboard = false;
    bool mouse = false;
    bool vibration = true;

    [[nodiscard]] bool allDisabled() const { return !controller && !keyboard && !mouse; }
    [[nodiscard]] bool operator==(const InputPermissions& o) const {
        return controller == o.controller && keyboard == o.keyboard &&
               mouse == o.mouse && vibration == o.vibration;
    }
};

// Valid frame-rate settings offered by the UI.
[[nodiscard]] const std::vector<int>& fpsPresets(); // {30, 60, 120}

} // namespace rp::common
