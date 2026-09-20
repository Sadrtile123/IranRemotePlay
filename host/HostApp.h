// RemotePlay - host/HostApp.h
// Qt-free facade between the UI and HostSession: applies user settings,
// owns the session lifecycle, and exposes thread-safe snapshots.
#pragma once

#include "host/HostSession.h"
#include "common/Config.h"

#include <string>
#include <vector>

namespace rp::host {

struct HostLaunchSettings {
    std::string gameName;                    // free text in Phase 1
    common::CaptureMode captureMode = common::CaptureMode::Window;
    common::Resolution resolution{};
    uint32_t fps = 60;
    uint32_t bitrateKbps = 8000;
    common::VideoCodec codec = common::VideoCodec::H264;
    uint16_t listenPort = kDefaultListenPort;
    bool requireApproval = true;
    common::InputPermissions inputDefaults;
    size_t maxClients = kMaxClients;
};

class HostApp {
public:
    HostApp();
    ~HostApp();

    HostApp(const HostApp&) = delete;
    HostApp& operator=(const HostApp&) = delete;

    // Translates persisted config into launch settings (filling defaults).
    [[nodiscard]] static HostLaunchSettings settingsFromConfig(const config::Config& cfg);

    void setEvents(HostSession::Events events) { events_ = std::move(events); }

    // Starts hosting. Returns false when the port cannot be bound.
    [[nodiscard]] bool start(const HostLaunchSettings& settings);
    void stop();

    [[nodiscard]] bool hosting() const { return session_ && session_->running(); }
    [[nodiscard]] std::string sessionCode() const;
    [[nodiscard]] uint16_t port() const;

    [[nodiscard]] std::vector<ClientRow> clients() const;
    void approve(uint32_t clientId, bool accept);
    void kick(uint32_t clientId);
    void setClientInputEnabled(uint32_t clientId, bool enabled);

private:
    HostSession::Events events_;
    std::unique_ptr<HostSession> session_;
    HostLaunchSettings current_;
};

} // namespace rp::host
