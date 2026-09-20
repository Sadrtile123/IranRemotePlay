// RemotePlay - common/Config.cpp
#include <string>
#include "common/Config.h"

#include "common/Log.h"
#include "common/Version.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace rp::config {

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

int clampInt(const json& j, const char* key, int def, int lo, int hi) {
    if (!j.is_object() || !j.contains(key)) return def;
    const json& v = j.at(key);
    if (!v.is_number_integer() && !v.is_number_unsigned()) return def;
    long long n = v.get<long long>();
    if (n < lo) n = lo;
    if (n > hi) n = hi;
    return static_cast<int>(n);
}

bool boolOr(const json& j, const char* key, bool def) {
    if (!j.is_object() || !j.contains(key)) return def;
    const json& v = j.at(key);
    return v.is_boolean() ? v.get<bool>() : def;
}

std::string stringOr(const json& j, const char* key, const std::string& def) {
    if (!j.is_object() || !j.contains(key)) return def;
    const json& v = j.at(key);
    return v.is_string() ? v.get<std::string>() : def;
}

} // namespace

void Config::sanitize() {
    // Codec
    // (already an enum; nothing to do)

    // FPS: only the supported presets
    const auto& fps = common::fpsPresets();
    if (std::find(fps.begin(), fps.end(), video.fps) == fps.end()) video.fps = 60;

    // Bitrate: 2..50 Mbps
    video.bitrateMbps = std::clamp(video.bitrateMbps, 2, 50);

    // Resolution: snap to the closest preset if out of range
    if (video.resolution.width < 320 || video.resolution.width > 8192 ||
        video.resolution.height < 240 || video.resolution.height > 8192) {
        video.resolution = common::Resolution{1920, 1080};
    }

    // Audio bitrate: 64 / 96 / 128 / 192
    const int audioChoices[] = {64, 96, 128, 192};
    if (std::find(std::begin(audioChoices), std::end(audioChoices), audio.bitrateKbps) ==
        std::end(audioChoices)) {
        audio.bitrateKbps = 128;
    }

    // Network
    network.maxBitrateMbps = std::clamp(network.maxBitrateMbps, 2, 50);
    network.jitterBufferMs = std::clamp(network.jitterBufferMs, 10, 200);
    network.listenPort = static_cast<int>(std::clamp<long long>(network.listenPort, 1, 65535));
}

Config Config::load(const std::string& filePath) {
    Config cfg;
    std::error_code ec;
    if (!fs::exists(fs::path(filePath), ec)) {
        return cfg; // defaults
    }
    std::ifstream in(filePath);
    if (!in.is_open()) {
        RP_WARN() << "Config file '" << filePath << "' could not be opened; using defaults.";
        return cfg;
    }
    json root;
    try {
        in >> root;
    } catch (const json::parse_error& e) {
        RP_WARN() << "Config file is corrupt (" << e.what() << "); using defaults.";
        return cfg;
    }
    if (!root.is_object()) {
        RP_WARN() << "Config root is not an object; using defaults.";
        return cfg;
    }

    const json& video = root.contains("video") ? root.at("video") : json::object();
    cfg.video.codec = common::videoCodecFromString(stringOr(video, "codec", "h264"))
                          .value_or(common::VideoCodec::H264);
    cfg.video.resolution =
        common::Resolution::parse(stringOr(video, "resolution", "1920x1080"))
            .value_or(common::Resolution{1920, 1080});
    cfg.video.fps = clampInt(video, "fps", 60, 1, 1000);
    cfg.video.bitrateMbps = clampInt(video, "bitrateMbps", 8, 2, 50);

    const json& audio = root.contains("audio") ? root.at("audio") : json::object();
    cfg.audio.bitrateKbps = clampInt(audio, "bitrateKbps", 128, 32, 512);
    // codec: only "opus" is meaningful today
    const std::string audioCodec = stringOr(audio, "codec", "opus");
    if (audioCodec != "opus") {
        RP_WARN() << "Unknown audio codec '" << audioCodec << "' in config; using opus.";
    }
    cfg.audio.codec = common::AudioCodec::Opus;

    const json& input = root.contains("input") ? root.at("input") : json::object();
    cfg.input.controller = boolOr(input, "controller", true);
    cfg.input.keyboard = boolOr(input, "keyboard", false);
    cfg.input.mouse = boolOr(input, "mouse", false);
    cfg.input.vibration = boolOr(input, "vibration", true);
    cfg.input.requireHostApproval = boolOr(input, "requireHostApproval", true);

    const json& network = root.contains("network") ? root.at("network") : json::object();
    cfg.network.maxBitrateMbps = clampInt(network, "maxBitrateMbps", 15, 2, 50);
    cfg.network.jitterBufferMs = clampInt(network, "jitterBufferMs", 30, 10, 200);
    cfg.network.listenPort = clampInt(network, "listenPort", kDefaultListenPort, 1, 65535);
    cfg.network.relayEnabled = boolOr(network, "relayEnabled", false);

    const json& profile = root.contains("profile") ? root.at("profile") : json::object();
    cfg.profile.name = stringOr(profile, "name", "");
    if (cfg.profile.name.size() > 32) cfg.profile.name.resize(32);

    cfg.sanitize();
    return cfg;
}

bool Config::save(const std::string& filePath) const {
    const fs::path p(filePath);
    if (p.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        if (ec) {
            RP_ERROR() << "Could not create config directory: " << ec.message();
            return false;
        }
    }
    json root;
    root["video"]["codec"] = common::videoCodecString(video.codec);
    root["video"]["resolution"] = video.resolution.toString();
    root["video"]["fps"] = video.fps;
    root["video"]["bitrateMbps"] = video.bitrateMbps;
    root["audio"]["codec"] = "opus";
    root["audio"]["bitrateKbps"] = audio.bitrateKbps;
    root["input"]["controller"] = input.controller;
    root["input"]["keyboard"] = input.keyboard;
    root["input"]["mouse"] = input.mouse;
    root["input"]["vibration"] = input.vibration;
    root["input"]["requireHostApproval"] = input.requireHostApproval;
    root["network"]["maxBitrateMbps"] = network.maxBitrateMbps;
    root["network"]["jitterBufferMs"] = network.jitterBufferMs;
    root["network"]["listenPort"] = network.listenPort;
    root["network"]["relayEnabled"] = network.relayEnabled;
    root["profile"]["name"] = profile.name;

    std::ofstream out(filePath, std::ios::trunc);
    if (!out.is_open()) {
        RP_ERROR() << "Could not write config file '" << filePath << "'.";
        return false;
    }
    out << root.dump(2) << '\n';
    return out.good();
}

} // namespace rp::config
