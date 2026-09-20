// RemotePlay - common/Config.h
// JSON configuration persisted at %APPDATA%\RemotePlay\config.json.
// Layout (see spec section 27):
// {
//   "video": { "codec": "h264", "resolution": "1920x1080", "fps": 60, "bitrateMbps": 8 },
//   "audio": { "codec": "opus", "bitrateKbps": 128 },
//   "input": { "controller": true, "keyboard": false, "mouse": false, "vibration": true, "requireHostApproval": true },
//   "network": { "maxBitrateMbps": 15, "jitterBufferMs": 30, "listenPort": 41717, "relayEnabled": false },
//   "profile": { "name": "" }
// }
#pragma once

#include "common/Types.h"

#include <string>

namespace rp::config {

struct VideoSettings {
    common::VideoCodec codec = common::VideoCodec::H264;
    common::Resolution resolution{};
    int fps = 60;              // 30 / 60 / 120
    int bitrateMbps = 8;       // 2..50
};

struct AudioSettings {
    common::AudioCodec codec = common::AudioCodec::Opus;
    int bitrateKbps = 128;     // 64 / 96 / 128 / 192
};

struct InputSettings {
    bool controller = true;
    bool keyboard = false;
    bool mouse = false;
    bool vibration = true;
    bool requireHostApproval = true;
};

struct NetworkSettings {
    int maxBitrateMbps = 15;
    int jitterBufferMs = 30;
    int listenPort = 41717;
    bool relayEnabled = false; // relay fallback arrives with the signaling phase
};

struct ProfileSettings {
    std::string name; // display name; falls back to the OS user name
};

struct Config {
    VideoSettings video;
    AudioSettings audio;
    InputSettings input;
    NetworkSettings network;
    ProfileSettings profile;

    // Loads from the given path, applying defaults for missing/invalid keys.
    // Returns defaults when the file does not exist or is corrupt.
    [[nodiscard]] static Config load(const std::string& filePath);

    // Validates and clamps all fields into their documented ranges.
    void sanitize();

    // Writes JSON with two-space indentation. Creates parent directories.
    [[nodiscard]] bool save(const std::string& filePath) const;
};

} // namespace rp::config
