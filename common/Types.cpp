// RemotePlay - common/Types.cpp
#include "common/Types.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace rp::common {

const char* toString(ConnectionState s) {
    switch (s) {
        case ConnectionState::Connecting: return "CONNECTING";
        case ConnectionState::Authenticating: return "AUTHENTICATING";
        case ConnectionState::Connected: return "CONNECTED";
        case ConnectionState::Streaming: return "STREAMING";
        case ConnectionState::Disconnected: return "DISCONNECTED";
    }
    return "UNKNOWN";
}

std::string connectionStateString(ConnectionState s) { return toString(s); }

const char* toString(VideoCodec c) {
    switch (c) {
        case VideoCodec::H264: return "H.264";
        case VideoCodec::Hevc: return "HEVC";
        case VideoCodec::Av1: return "AV1";
    }
    return "UNKNOWN";
}

std::string videoCodecString(VideoCodec c) { return toString(c); }

std::optional<VideoCodec> videoCodecFromString(const std::string& name) {
    std::string n;
    n.reserve(name.size());
    for (char ch : name) n.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    if (n == "h264" || n == "h.264" || n == "avc") return VideoCodec::H264;
    if (n == "hevc" || n == "h265" || n == "h.265") return VideoCodec::Hevc;
    if (n == "av1") return VideoCodec::Av1;
    return std::nullopt;
}

const char* toString(CaptureMode m) {
    switch (m) {
        case CaptureMode::Window: return "Window";
        case CaptureMode::Monitor: return "Monitor";
    }
    return "Unknown";
}

std::string Resolution::toString() const {
    std::ostringstream os;
    os << width << 'x' << height;
    return os.str();
}

std::optional<Resolution> Resolution::parse(const std::string& text) {
    // Accept "1920x1080", "1920X1080", "1920*1080", " 1920 x 1080 "
    std::string cleaned;
    cleaned.reserve(text.size());
    for (char ch : text) {
        if (!std::isspace(static_cast<unsigned char>(ch))) cleaned.push_back(ch);
    }
    for (char& ch : cleaned) {
        if (ch == 'X' || ch == '*' || ch == 'x') ch = 'x';
    }
    auto pos = cleaned.find('x');
    if (pos == std::string::npos) return std::nullopt;
    const std::string w = cleaned.substr(0, pos);
    const std::string h = cleaned.substr(pos + 1);
    if (w.empty() || h.empty()) return std::nullopt;
    for (char c : w) if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
    for (char c : h) if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
    unsigned long ww = std::strtoul(w.c_str(), nullptr, 10);
    unsigned long hh = std::strtoul(h.c_str(), nullptr, 10);
    if (ww < 320 || ww > 8192 || hh < 240 || hh > 8192) return std::nullopt;
    Resolution r;
    r.width = static_cast<uint32_t>(ww);
    r.height = static_cast<uint32_t>(hh);
    return r;
}

const std::vector<Resolution>& Resolution::presets() {
    static const std::vector<Resolution> kPresets = {
        Resolution{1280, 720},   // 720p
        Resolution{1920, 1080},  // 1080p
        Resolution{2560, 1440},  // 1440p
        Resolution{3840, 2160},  // 4K
    };
    return kPresets;
}

const std::vector<int>& fpsPresets() {
    static const std::vector<int> kFps = {30, 60, 120};
    return kFps;
}

} // namespace rp::common
