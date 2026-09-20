// RemotePlay - client/ClientApp.h
// Qt-free facade between the UI and ClientSession.
#pragma once

#include <memory>
#include <utility>
#include <vector>
#include "client/ClientSession.h"
#include "common/Config.h"

#include <string>

namespace rp::client {

struct JoinSettings {
    std::string displayName;
    std::string sessionCode;   // "ABC7-K92P"
    std::string hostAddress;   // direct connection (signaling arrives Phase 13)
    uint16_t hostPort = kDefaultListenPort;
    uint32_t connectTimeoutMs = 5000;
};

class ClientApp {
public:
    ClientApp();
    ~ClientApp();

    ClientApp(const ClientApp&) = delete;
    ClientApp& operator=(const ClientApp&) = delete;

    void setEvents(ClientSession::Events events) { events_ = std::move(events); }

    void connect(const JoinSettings& settings);
    void disconnect();

    // Phase 12/13 forwarders to the live session.
    void sendAppMessage(uint16_t type, const std::vector<uint8_t>& payload);
    void startViaRelay(const ClientSessionParams& params, const std::string& serverHost,
                       uint16_t relayTcpPort, const std::string& token);
    [[nodiscard]] ClientSession& session() { return *session_; }   // set only while connected

    [[nodiscard]] bool connected() const { return session_ && session_->active(); }
    [[nodiscard]] ClientStatus status() const;

private:
    ClientSession::Events events_;
    std::unique_ptr<ClientSession> session_;
};

} // namespace rp::client
