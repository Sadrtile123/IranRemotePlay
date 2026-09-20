// RemotePlay - host/HostApp.cpp
#include "host/HostApp.h"

#include "common/Paths.h"

#include <algorithm>

namespace rp::host {

namespace {

// The display name friends see, e.g. "Mahdyar's PC".
std::string hostDisplayName() {
    const std::string user = paths::userName();
    if (!user.empty()) return user + "'s PC";
    return "Host PC";
}

} // namespace

HostApp::HostApp() = default;

HostApp::~HostApp() { stop(); }

HostLaunchSettings HostApp::settingsFromConfig(const config::Config& cfg) {
    HostLaunchSettings s;
    s.captureMode = common::CaptureMode::Window; // UI selectable; persisted later
    s.resolution = cfg.video.resolution;
    s.fps = static_cast<uint32_t>(cfg.video.fps);
    s.bitrateKbps = static_cast<uint32_t>(cfg.video.bitrateMbps) * 1000;
    s.bitrateKbps = std::min<uint32_t>(s.bitrateKbps,
                                       static_cast<uint32_t>(cfg.network.maxBitrateMbps) * 1000);
    s.codec = cfg.video.codec;
    s.listenPort = static_cast<uint16_t>(cfg.network.listenPort);
    s.requireApproval = cfg.input.requireHostApproval;
    s.inputDefaults = common::InputPermissions{
        cfg.input.controller, cfg.input.keyboard, cfg.input.mouse, cfg.input.vibration};
    s.maxClients = kMaxClients;
    return s;
}

bool HostApp::start(const HostLaunchSettings& settings) {
    stop();
    auto session = std::make_unique<HostSession>(events_);

    HostSessionParams params;
    params.hostName = hostDisplayName();
    params.gameName = settings.gameName;
    params.captureMode = settings.captureMode;
    params.resolution = settings.resolution;
    params.fps = settings.fps;
    params.bitrateKbps = settings.bitrateKbps;
    params.preferredCodec = settings.codec;
    params.listenPort = settings.listenPort;
    params.autoApprove = !settings.requireApproval;
    params.inputDefaults = settings.inputDefaults;
    params.maxClients = settings.maxClients;

    if (!session->start(params)) return false;
    current_ = settings;
    session_ = std::move(session);
    return true;
}

void HostApp::stop() {
    if (!session_) return;
    session_->stop();
    session_.reset();
}

std::string HostApp::sessionCode() const {
    return session_ ? session_->sessionCode() : std::string();
}

uint16_t HostApp::port() const { return session_ ? session_->port() : 0; }

std::vector<ClientRow> HostApp::clients() const {
    return session_ ? session_->clients() : std::vector<ClientRow>{};
}

void HostApp::approve(uint32_t clientId, bool accept) {
    if (session_) session_->approveClient(clientId, accept);
}

void HostApp::kick(uint32_t clientId) {
    if (session_) session_->kickClient(clientId);
}

void HostApp::setClientInputEnabled(uint32_t clientId, bool enabled) {
    if (session_) session_->setClientInput(clientId, enabled);
}

} // namespace rp::host
