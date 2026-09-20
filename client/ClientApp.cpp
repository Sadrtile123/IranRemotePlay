// RemotePlay - client/ClientApp.cpp
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "client/ClientApp.h"

namespace rp::client {

ClientApp::ClientApp() = default;

ClientApp::~ClientApp() { disconnect(); }

void ClientApp::connect(const JoinSettings& settings) {
    disconnect();
    auto session = std::make_unique<ClientSession>(events_);

    ClientSessionParams params;
    params.clientName = settings.displayName.empty() ? "Player" : settings.displayName;
    params.sessionCode = settings.sessionCode;
    params.hostAddress = settings.hostAddress;
    params.hostPort = settings.hostPort;
    params.connectTimeoutMs = settings.connectTimeoutMs;
    params.supportedCodecs = {common::VideoCodec::H264}; // baseline until Phase 5

    session->start(params);
    session_ = std::move(session);
}

void ClientApp::sendAppMessage(uint16_t type, const std::vector<uint8_t>& payload) {
    if (session_) session_->sendAppMessage(type, payload);
}

void ClientApp::startViaRelay(const ClientSessionParams& params, const std::string& serverHost,
                              uint16_t relayTcpPort, const std::string& token) {
    if (session_) session_->startViaRelay(params, serverHost, relayTcpPort, token);
}

void ClientApp::disconnect() {
    if (!session_) return;
    session_->stop();
    session_.reset();
}

ClientStatus ClientApp::status() const {
    return session_ ? session_->status() : ClientStatus{};
}

} // namespace rp::client
