// RemotePlay - host/HostSession.cpp
#include "host/HostSession.h"

#include "security/SessionCodes.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace rp::host {

using namespace std::chrono_literals;

using LogLevel = rp::log::Level; // convenience alias inside this file

namespace {

constexpr auto kHelloTimeout = 10s;      // client must send CLIENT_HELLO
constexpr auto kApprovalTimeout = 75s;   // human decision window
constexpr auto kHeartbeatTimeout = 15s;  // CONNECTED clients must stay chatty
constexpr auto kPingPeriod = 2s;

uint32_t sinceEpochMs(const std::chrono::steady_clock::time_point t) {
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count());
}

} // namespace

std::chrono::milliseconds HostSession::steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch());
}

HostSession::HostSession(Events events) : events_(std::move(events)) {}

HostSession::~HostSession() { stop(); }

bool HostSession::start(const HostSessionParams& params) {
    if (running_.exchange(true)) return true; // already running
    stopping_ = false;
    params_ = params;
    sessionCode_ = security::generateSessionCode();

    // Bind the listener before launching the thread so failures are reported
    // synchronously to the caller.
    asio::error_code bindEc;
    std::unique_ptr<net::TcpServer> server;
    try {
        server = std::make_unique<net::TcpServer>(io_, params_.listenPort);
    } catch (const asio::system_error& e) {
        bindEc = e.code();
    } catch (const std::exception&) {
        bindEc = asio::error::invalid_argument;
    }
    if (bindEc) {
        running_ = false;
        logEvent(rp::log::Level::Error, "Cannot listen on port " +
                                             std::to_string(params_.listenPort) + ": " +
                                             bindEc.message());
        return false;
    }
    actualPort_ = server->port();
    server_ = std::move(server);

    work_ = std::make_unique<WorkGuard>(asio::make_work_guard(io_));
    thread_ = std::thread([this] { runNetworkThread(); });

    logEvent(LogLevel::Info, "Session created (code " + sessionCode_ + ", port " +
                                 std::to_string(actualPort_) + ")");
    if (events_.onListening) events_.onListening(actualPort_, sessionCode_);
    return true;
}

void HostSession::runNetworkThread() {
    server_->setConnectionHandler(
        [this](net::TcpConnection::Ptr conn) { handleNewConnection(std::move(conn)); });

    housekeeper_.expires_after(1s);
    housekeeper_.async_wait([this](const std::error_code& ec) {
        if (!ec && !stopping_.load()) housekeeping();
    });

    io_.run();
}

void HostSession::stop() {
    if (!running_.exchange(false)) return;
    stopping_ = true;
    asio::post(io_, [this] {
        std::error_code ec;
        housekeeper_.cancel();
        if (server_) server_->stop();
        for (auto& [id, rec] : clients_) {
            if (rec.connection) rec.connection->close();
        }
    });
    work_.reset();
    io_.stop();
    if (thread_.joinable()) thread_.join();
    io_.reset();

    {
        std::lock_guard<std::mutex> lock(snapshotMutex_);
        snapshot_.clear();
    }
    clients_.clear();
    server_.reset();
    if (events_.onStopped) events_.onStopped();
}

std::vector<ClientRow> HostSession::clients() const {
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    return snapshot_;
}

void HostSession::approveClient(uint32_t clientId, bool accept) {
    asio::post(io_, [this, clientId, accept] {
        auto it = clients_.find(clientId);
        if (it == clients_.end()) return;
        ClientRecord& rec = it->second;
        if (rec.state != common::ConnectionState::Authenticating) return;

        if (!accept) {
            logEvent(LogLevel::Info, "Host rejected client '" + rec.name + "'");
            dropClient(clientId, true, proto::Reason::RejectedByHost, "The host rejected your join request.");
            return;
        }
        logEvent(LogLevel::Info, "Host accepted client '" + rec.name + "'");

        proto::msg::HostCapabilities caps;
        caps.hostName = params_.hostName;
        caps.gameName = params_.gameName;
        caps.captureMode = static_cast<uint8_t>(params_.captureMode);
        caps.width = static_cast<uint16_t>(params_.resolution.width);
        caps.height = static_cast<uint16_t>(params_.resolution.height);
        caps.fps = static_cast<uint8_t>(std::min<uint32_t>(params_.fps, 255));
        caps.bitrateKbps = params_.bitrateKbps;
        caps.offeredCodecs = {params_.preferredCodec, common::VideoCodec::H264};
        // de-duplicate while preserving preference order
        caps.offeredCodecs.erase(std::unique(caps.offeredCodecs.begin(), caps.offeredCodecs.end()),
                                 caps.offeredCodecs.end());

        proto::Envelope approved;
        approved.type = proto::Id::HostApproved;
        approved.body = proto::msg::HostApproved{};
        sendTo(clientId, approved);

        proto::Envelope env;
        env.type = proto::Id::HostCapabilities;
        env.body = std::move(caps);
        sendTo(clientId, env);
        // State advances to CONNECTED once capabilities are negotiated.
        rec.lastActivity = std::chrono::steady_clock::now();
    });
}

void HostSession::kickClient(uint32_t clientId, const std::string& reason) {
    asio::post(io_, [this, clientId, reason] {
        auto it = clients_.find(clientId);
        if (it == clients_.end()) return;
        logEvent(LogLevel::Info, "Kicking client '" + it->second.name + "': " + reason);
        proto::msg::Kick kick;
        kick.reason = reason;
        proto::Envelope env;
        env.type = proto::Id::Kick;
        env.body = std::move(kick);
        sendAndClose(clientId, env);
        setState(clientId, common::ConnectionState::Disconnected);
    });
}

void HostSession::setClientInput(uint32_t clientId, bool enableAll) {
    asio::post(io_, [this, clientId, enableAll] {
        auto it = clients_.find(clientId);
        if (it == clients_.end()) return;
        ClientRecord& rec = it->second;
        if (enableAll) {
            rec.input = params_.inputDefaults;
        } else {
            // NOTE: InputPermissions{} would keep the defaulted members true;
            // disabling input must zero every field explicitly.
            rec.input = common::InputPermissions{false, false, false, false};
        }
        logEvent(LogLevel::Info, "Input for '" + rec.name + "' " +
                                     (enableAll ? "enabled" : "disabled") + " by host");
        pushInputPermissions(clientId);
    });
}

void HostSession::sendAppMessage(uint32_t clientId, uint16_t type, const std::vector<uint8_t>& payload) {
    if (type < 0x0010) return;
    asio::post(io_, [this, clientId, type, payload] {
        auto it = clients_.find(clientId);
        if (it == clients_.end() || !it->second.connection) return;
        it->second.connection->send(type, payload);   // raw frame; caller pre-encodes
    });
}

void HostSession::connectRelayClient(const std::string& serverHost, uint16_t relayTcpPort,
                                     const std::string& token, int playerIndexHint) {
    (void)playerIndexHint;
    asio::post(io_, [this, serverHost, relayTcpPort, token] {
        auto dialer = std::make_unique<net::TcpClient>(io_);
        dialer->connectRelay(serverHost, relayTcpPort, token, "host", 10000,
                             [this](std::error_code ec, net::TcpConnection::Ptr conn) {
                                 if (ec) {
                                     logEvent(LogLevel::Error, "Relay connect failed: " + ec.message());
                                     return;
                                 }
                                 handleNewConnection(std::move(conn));
                             });
        // The TcpClient must outlive the async attempt: park it in a list
        // on the network thread until the callback fires.
        relayDialers_.push_back(std::move(dialer));
        if (relayDialers_.size() > 8) relayDialers_.pop_front();   // bound growth
    });
}

void HostSession::attachConnection(net::TcpConnection::Ptr conn) {
    asio::post(io_, [this, conn] {
        handleNewConnection(conn);
    });
}

void HostSession::handleNewConnection(net::TcpConnection::Ptr conn) {
    if (stopping_.load()) {
        conn->close();
        return;
    }
    const uint32_t id = nextClientId_++;
    ClientRecord rec;
    rec.id = id;
    rec.name = "pending";
    rec.state = common::ConnectionState::Connecting;
    rec.lastActivity = std::chrono::steady_clock::now();
    rec.connection = conn;
    rec.address = conn->remoteAddress();
    clients_[id] = std::move(rec);
    rebuildSnapshot();

    conn->setHandlers(
        [this, id](uint16_t type, uint8_t flags, const uint8_t* data, uint32_t size) {
            handleFrame(id, type, flags, data, size);
        },
        [this, id](const std::error_code& ec) { handleClientClosed(id, ec); });
    conn->start();
}

void HostSession::handleFrame(uint32_t clientId, uint16_t type, uint8_t /*flags*/,
                              const uint8_t* data, uint32_t size) {
    if (stopping_.load()) return;
    if (!proto::knownId(type)) {
        protocolError("unknown message type 0x" + std::to_string(type));
        dropClient(clientId, true, proto::Reason::ProtocolError, "Unknown message type.");
        return;
    }
    auto it = clients_.find(clientId);
    if (it == clients_.end()) return;
    it->second.lastActivity = std::chrono::steady_clock::now();

    const auto env = proto::decodeMessage(type, data, size);
    if (!env) {
        protocolError("malformed " + std::string(proto::idName(static_cast<proto::Id>(type))));
        dropClient(clientId, true, proto::Reason::ProtocolError, "Malformed message.");
        return;
    }

    switch (env->type) {
        case proto::Id::ClientHello:
            if (auto* m = std::get_if<proto::msg::ClientHello>(&env->body)) handleClientHello(clientId, *m);
            break;
        case proto::Id::ClientCapabilities:
            if (auto* m = std::get_if<proto::msg::ClientCapabilities>(&env->body)) {
                handleClientCapabilities(clientId, *m);
            }
            break;
        case proto::Id::Ping:
            if (auto* m = std::get_if<proto::msg::Ping>(&env->body)) handlePing(clientId, *m);
            break;
        case proto::Id::Pong:
            if (auto* m = std::get_if<proto::msg::Pong>(&env->body)) handlePong(clientId, *m);
            break;
        case proto::Id::Bye:
            if (auto* m = std::get_if<proto::msg::Bye>(&env->body)) handleBye(clientId, *m);
            break;
        default: {
            // App-extension range (0x0010+): forward to the app layer when the
            // client is CONNECTED; still rejected earlier in the handshake.
            const bool appRange = type >= 0x0010;
            const auto it2 = clients_.find(clientId);
            const bool connected = it2 != clients_.end() &&
                                   it2->second.state == common::ConnectionState::Connected;
            if (appRange && connected && events_.onAppMessage) {
                std::vector<uint8_t> payload(data, data + size);
                events_.onAppMessage(clientId, type, payload);
                break;
            }
            dropClient(clientId, true, proto::Reason::ProtocolError,
                       "Unexpected message in current state.");
            return;
        }
    }
}

void HostSession::handleClientHello(uint32_t clientId, const proto::msg::ClientHello& hello) {
    auto it = clients_.find(clientId);
    if (it == clients_.end()) return;
    ClientRecord& rec = it->second;
    if (rec.state != common::ConnectionState::Connecting) return; // duplicate HELLO: ignore

    rec.name = hello.clientName.empty() ? "Player" : hello.clientName;

    if (clients_.size() > params_.maxClients) {
        dropClient(clientId, true, proto::Reason::ServerFull, "The session already has the maximum number of players.");
        return;
    }
    if (!security::verifySessionCode(hello.sessionCode, sessionCode_)) {
        logEvent(LogLevel::Warning, "Client '" + rec.name + "' presented an invalid session code");
        dropClient(clientId, true, proto::Reason::BadSessionCode, "Invalid session code.");
        return;
    }

    rec.state = common::ConnectionState::Authenticating;
    rec.input = params_.inputDefaults;
    rebuildSnapshot();
    logEvent(LogLevel::Info, "Client '" + rec.name + "' connected from " + rec.address +
                                 " (RemotePlay " + (hello.appVersion.empty() ? "?" : hello.appVersion) + ")");

    if (params_.autoApprove) {
        approveClient(clientId, true);
    } else if (events_.onApprovalRequest) {
        events_.onApprovalRequest(clientId, rec.name);
    }
}

void HostSession::handleClientCapabilities(uint32_t clientId,
                                           const proto::msg::ClientCapabilities& caps) {
    auto it = clients_.find(clientId);
    if (it == clients_.end()) return;
    if (it->second.state != common::ConnectionState::Authenticating) return;

    const common::Resolution res = params_.resolution;
    const auto negotiated = proto::negotiateCodec({params_.preferredCodec, common::VideoCodec::H264},
                                                  caps.supportedCodecs);
    proto::msg::NegotiationResult result;
    if (!negotiated) {
        result.accepted = false;
        result.note = "No common video codec between host and client.";
    } else {
        result.accepted = true;
        result.codec = static_cast<uint8_t>(*negotiated);
        result.width = static_cast<uint16_t>(res.width);
        result.height = static_cast<uint16_t>(res.height);
        result.fps = static_cast<uint8_t>(std::min<uint32_t>(params_.fps, 255));
        result.bitrateKbps = params_.bitrateKbps;
        result.note = common::videoCodecString(*negotiated);
    }

    proto::Envelope env;
    env.type = proto::Id::NegotiationResult;
    env.body = std::move(result);
    sendTo(clientId, env);

    if (!negotiated) {
        dropClient(clientId, true, proto::Reason::NoCommonCodec, "No common codec.");
        return;
    }
    logEvent(LogLevel::Info, std::string("Negotiating ") + common::videoCodecString(*negotiated));
    setState(clientId, common::ConnectionState::Connected);
    pushInputPermissions(clientId);
}

void HostSession::handlePing(uint32_t clientId, const proto::msg::Ping& ping) {
    proto::msg::Pong pong;
    pong.echoEpochMs = ping.epochMs;
    proto::Envelope env;
    env.type = proto::Id::Pong;
    env.body = std::move(pong);
    sendTo(clientId, env);
}

void HostSession::handlePong(uint32_t clientId, const proto::msg::Pong& pong) {
    auto it = clients_.find(clientId);
    if (it == clients_.end()) return;
    const uint32_t now = static_cast<uint32_t>(sinceEpochMs(std::chrono::steady_clock::now()));
    const uint32_t sample = now - static_cast<uint32_t>(pong.echoEpochMs);
    ClientRecord& rec = it->second;
    // Exponential smoothing; start EWMA at the first sample.
    rec.rttMs = rec.rttMs == 0 ? sample : (rec.rttMs * 7 + sample) / 8;
    rebuildSnapshot();
}

void HostSession::handleBye(uint32_t clientId, const proto::msg::Bye& bye) {
    logEvent(LogLevel::Info, "Client said bye (" + bye.reason + ")");
    dropClient(clientId, false, proto::Reason::Ok, bye.reason);
}

void HostSession::handleClientClosed(uint32_t clientId, const std::error_code& ec) {
    if (stopping_.load()) return;
    auto it = clients_.find(clientId);
    if (it == clients_.end()) return;
    ClientRow row;
    row.id = clientId;
    row.name = it->second.name;
    row.state = common::ConnectionState::Disconnected;
    row.inputEnabled = !it->second.input.allDisabled();
    row.address = it->second.address;
    const std::string name = row.name;

    if (ec && ec != asio::error::eof && ec != asio::error::operation_aborted) {
        logEvent(LogLevel::Warning, "Client '" + name + "' connection lost: " + ec.message());
    } else {
        logEvent(LogLevel::Info, "Client '" + name + "' disconnected");
    }
    clients_.erase(clientId);
    {
        std::lock_guard<std::mutex> lock(snapshotMutex_);
        snapshot_.clear();
        for (const auto& [id, rec] : clients_) {
            ClientRow r;
            r.id = id;
            r.name = rec.name;
            r.state = rec.state;
            r.rttMs = rec.rttMs;
            r.inputEnabled = !rec.input.allDisabled();
            r.address = rec.address;
            snapshot_.push_back(std::move(r));
        }
    }
    if (events_.onClientUpdated) events_.onClientUpdated(row);
}

void HostSession::sendTo(uint32_t clientId, const proto::Envelope& msg) {
    auto it = clients_.find(clientId);
    if (it == clients_.end() || !it->second.connection) return;
    it->second.connection->send(static_cast<uint16_t>(msg.type),
                                proto::encodeMessage(msg));
}

void HostSession::sendAndClose(uint32_t clientId, const proto::Envelope& msg) {
    auto it = clients_.find(clientId);
    if (it == clients_.end() || !it->second.connection) return;
    it->second.connection->sendAndClose(static_cast<uint16_t>(msg.type),
                                        proto::encodeMessage(msg));
}

void HostSession::pushInputPermissions(uint32_t clientId) {
    auto it = clients_.find(clientId);
    if (it == clients_.end()) return;
    proto::msg::InputPermission perms;
    perms.controller = it->second.input.controller;
    perms.keyboard = it->second.input.keyboard;
    perms.mouse = it->second.input.mouse;
    perms.vibration = it->second.input.vibration;
    proto::Envelope env;
    env.type = proto::Id::InputPermission;
    env.body = std::move(perms);
    sendTo(clientId, env);
    if (events_.onInputPermissions) events_.onInputPermissions(clientId, it->second.input);
}

void HostSession::setState(uint32_t clientId, common::ConnectionState state) {
    auto it = clients_.find(clientId);
    if (it == clients_.end()) return;
    it->second.state = state;
    rebuildSnapshot();
    ClientRow row;
    {
        std::lock_guard<std::mutex> lock(snapshotMutex_);
        for (const auto& r : snapshot_) {
            if (r.id == clientId) {
                row = r;
                break;
            }
        }
    }
    if (events_.onClientUpdated) events_.onClientUpdated(row);
}

void HostSession::rebuildSnapshot() {
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    snapshot_.clear();
    for (const auto& [id, rec] : clients_) {
        ClientRow row;
        row.id = id;
        row.name = rec.name;
        row.state = rec.state;
        row.rttMs = rec.rttMs;
        row.inputEnabled = !rec.input.allDisabled();
        row.address = rec.address;
        snapshot_.push_back(std::move(row));
    }
}

void HostSession::dropClient(uint32_t clientId, bool sendReject, proto::Reason reason,
                             const std::string& text) {
    auto it = clients_.find(clientId);
    if (it == clients_.end()) return;
    if (sendReject) {
        proto::msg::HostReject reject;
        reject.reasonCode = static_cast<uint16_t>(reason);
        reject.reasonText = text;
        proto::Envelope env;
        env.type = proto::Id::HostReject;
        env.body = std::move(reject);
        sendAndClose(clientId, env);
    } else if (it->second.connection) {
        it->second.connection->close();
    }
    setState(clientId, common::ConnectionState::Disconnected);
    clients_.erase(clientId);
    rebuildSnapshot();
}

void HostSession::housekeeping() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<uint32_t> toDrop;

    for (auto& [id, rec] : clients_) {
        const auto idle = now - rec.lastActivity;
        switch (rec.state) {
            case common::ConnectionState::Connecting:
                if (idle > kHelloTimeout) toDrop.push_back(id);
                break;
            case common::ConnectionState::Authenticating:
                if (idle > kApprovalTimeout) toDrop.push_back(id);
                break;
            case common::ConnectionState::Connected:
            case common::ConnectionState::Streaming:
                if (idle > kHeartbeatTimeout) {
                    logEvent(LogLevel::Warning, "Client '" + rec.name + "' heartbeat timed out");
                    toDrop.push_back(id);
                }
                break;
            case common::ConnectionState::Disconnected:
                break;
        }
        // Host-initiated RTT probe every 2 s.
        if (rec.state == common::ConnectionState::Connected ||
            rec.state == common::ConnectionState::Streaming) {
            const auto sincePing = now - rec.lastHostPing;
            if (rec.lastHostPing.time_since_epoch().count() == 0 || sincePing >= kPingPeriod) {
                rec.lastHostPing = now;
                proto::msg::Ping ping;
                ping.epochMs = static_cast<uint64_t>(sinceEpochMs(now));
                proto::Envelope env;
                env.type = proto::Id::Ping;
                env.body = std::move(ping);
                sendTo(id, env);
            }
        }
    }

    for (uint32_t id : toDrop) {
        dropClient(id, true, proto::Reason::InactivityTimeout, "Connection timed out.");
    }

    housekeeper_.expires_after(1s);
    housekeeper_.async_wait([this](const std::error_code& ec) {
        if (!ec && !stopping_.load()) housekeeping();
    });
}

void HostSession::protocolError(const std::string& what) {
    logEvent(LogLevel::Warning, "Protocol error: " + what);
}

void HostSession::logEvent(rp::log::Level level, const std::string& message) {
    switch (level) {
        case rp::log::Level::Trace: RP_TRACE() << message; break;
        case rp::log::Level::Debug: RP_DEBUG() << message; break;
        case rp::log::Level::Info: RP_INFO() << message; break;
        case rp::log::Level::Warning: RP_WARN() << message; break;
        case rp::log::Level::Error: RP_ERROR() << message; break;
        case rp::log::Level::Critical: RP_CRIT() << message; break;
    }
    if (events_.onLog) events_.onLog(level, message);
}

} // namespace rp::host
