// RemotePlay - client/ClientSession.cpp
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <variant>
#include <vector>
#include "client/ClientSession.h"

#include <utility>

namespace rp::client {

using namespace std::chrono_literals;

namespace {

constexpr auto kHeartbeatPeriod = 2s;
constexpr auto kHeartbeatTimeout = 12s;

uint64_t steadyNowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

} // namespace

ClientSession::ClientSession(Events events) : events_(std::move(events)) {}

ClientSession::~ClientSession() { joinNetworkThread(); }

void ClientSession::joinNetworkThread() {
    if (thread_.joinable()) {
        io_.stop();
        thread_.join();
    }
    io_.reset(); // allow a subsequent start()
    connection_.reset();
    connector_.reset();
    heartbeat_.reset();
}

void ClientSession::start(const ClientSessionParams& params) {
    joinNetworkThread(); // reap any previous run
    if (running_.exchange(true)) return;
    stopping_ = false;
    params_ = params;

    updateStatus([](ClientStatus& s) {
        s.state = common::ConnectionState::Connecting;
        s.hostName.clear();
        s.gameName.clear();
        s.lastError.clear();
        s.rttMs = 0;
        s.bytesIn = 0;
        s.bytesOut = 0;
    });
    setState(common::ConnectionState::Connecting);
    logEvent(rp::log::Level::Info, "Connecting to " + params_.hostAddress + ":" +
                                       std::to_string(params_.hostPort) + "...");

    work_ = std::make_unique<WorkGuard>(asio::make_work_guard(io_));
    connector_ = std::make_unique<net::TcpClient>(io_);
    threadDone_ = std::promise<void>{};
    threadDoneFuture_ = threadDone_.get_future();
    thread_ = std::thread([this] {
        runNetworkThread();
        threadDone_.set_value();
    });
}

void ClientSession::attachConnection(net::TcpConnection::Ptr conn, const ClientSessionParams& params) {
    joinNetworkThread();
    if (running_.exchange(true)) return;
    stopping_ = false;
    params_ = params;

    updateStatus([](ClientStatus& s) {
        s.state = common::ConnectionState::Connecting;
        s.hostName.clear();
        s.gameName.clear();
        s.lastError.clear();
        s.rttMs = 0;
    });
    setState(common::ConnectionState::Connecting);
    logEvent(rp::log::Level::Info, "Attaching relay-paired connection to " + conn->remoteAddress());

    work_ = std::make_unique<WorkGuard>(asio::make_work_guard(io_));
    threadDone_ = std::promise<void>{};
    threadDoneFuture_ = threadDone_.get_future();
    thread_ = std::thread([this, conn] {
        handleConnected(conn);
        heartbeat_ = std::make_unique<asio::steady_timer>(io_);
        heartbeat_->expires_after(kHeartbeatPeriod);
        heartbeat_->async_wait([this](const std::error_code& ec) {
            if (ec || stopping_.load()) return;
            startHeartbeat();
        });
        runNetworkThreadBody();
        threadDone_.set_value();
    });
}

void ClientSession::runNetworkThreadBody() {
    io_.run();
}

void ClientSession::startViaRelay(const ClientSessionParams& params, const std::string& serverHost,
                                  uint16_t relayTcpPort, const std::string& token) {
    joinNetworkThread();
    if (running_.exchange(true)) return;
    stopping_ = false;
    params_ = params;

    updateStatus([](ClientStatus& s) {
        s.state = common::ConnectionState::Connecting;
        s.hostName.clear();
        s.gameName.clear();
        s.lastError.clear();
        s.rttMs = 0;
    });
    setState(common::ConnectionState::Connecting);
    logEvent(rp::log::Level::Info, "Connecting via relay " + serverHost + ":" +
                                       std::to_string(relayTcpPort) + "...");

    work_ = std::make_unique<WorkGuard>(asio::make_work_guard(io_));
    connector_ = std::make_unique<net::TcpClient>(io_);
    threadDone_ = std::promise<void>{};
    threadDoneFuture_ = threadDone_.get_future();
    relayServerHost_ = serverHost;
    relayServerPort_ = relayTcpPort;
    relayToken_ = token;
    thread_ = std::thread([this] {
        connector_->connectRelay(relayServerHost_, relayServerPort_, relayToken_, "client", 15000,
                        [this](std::error_code ec, net::TcpConnection::Ptr conn) {
                            if (ec) {
                                const std::string msg =
                                    "Relay connection failed: " + ec.message();
                                logEvent(rp::log::Level::Error, msg);
                                if (events_.onError) events_.onError(msg);
                                finish(common::ConnectionState::Disconnected, msg, true);
                                return;
                            }
                            handleConnected(std::move(conn));
                        });

        heartbeat_ = std::make_unique<asio::steady_timer>(io_);
        heartbeat_->expires_after(kHeartbeatPeriod);
        heartbeat_->async_wait([this](const std::error_code& ec) {
            if (ec || stopping_.load()) return;
            startHeartbeat();
        });

        io_.run();
        threadDone_.set_value();
    });
}

void ClientSession::runNetworkThread() {
    connector_->connect(params_.hostAddress, params_.hostPort, params_.connectTimeoutMs,
                        [this](std::error_code ec, net::TcpConnection::Ptr conn) {
                            if (ec) {
                                const std::string msg =
                                    "Connection failed: " + ec.message();
                                logEvent(rp::log::Level::Error, msg);
                                if (events_.onError) events_.onError(msg);
                                finish(common::ConnectionState::Disconnected, msg, true);
                                return;
                            }
                            handleConnected(std::move(conn));
                        });

    heartbeat_ = std::make_unique<asio::steady_timer>(io_);
    heartbeat_->expires_after(kHeartbeatPeriod);
    heartbeat_->async_wait([this](const std::error_code& ec) {
        if (ec || stopping_.load()) return;
        startHeartbeat();
    });

    io_.run();
}

void ClientSession::handleConnected(net::TcpConnection::Ptr conn) {
    connection_ = conn;
    lastActivity_ = std::chrono::steady_clock::now();
    updateStatus([&conn](ClientStatus& s) { (void)conn; s.bytesIn = 0; s.bytesOut = 0; });

    conn->setHandlers(
        [this](uint16_t type, uint8_t /*flags*/, const uint8_t* data, uint32_t size) {
            handleFrame(type, data, size);
        },
        [this](const std::error_code& ec) { handleClosed(ec); });
    conn->start();

    logEvent(rp::log::Level::Info, "TCP established with " + conn->remoteAddress());
    sendHello();
    setState(common::ConnectionState::Authenticating);
}

void ClientSession::sendHello() {
    proto::msg::ClientHello hello;
    hello.clientName = params_.clientName;
    hello.sessionCode = params_.sessionCode;
    hello.appVersion = kAppVersion;

    proto::Envelope env;
    env.type = proto::Id::ClientHello;
    env.body = std::move(hello);
    send(env);
}

void ClientSession::handleFrame(uint16_t type, const uint8_t* data, uint32_t size) {
    if (stopping_.load()) return;
    lastActivity_ = std::chrono::steady_clock::now();

    if (!proto::knownId(type)) {
        const std::string msg = "Host sent an unknown message type (0x" +
                                std::to_string(type) + ")";
        if (events_.onError) events_.onError(msg);
        finish(common::ConnectionState::Disconnected, msg, true);
        return;
    }
    const auto env = proto::decodeMessage(type, data, size);
    if (!env) {
        const std::string msg = "Host sent a malformed message";
        if (events_.onError) events_.onError(msg);
        finish(common::ConnectionState::Disconnected, msg, true);
        return;
    }

    switch (env->type) {
        case proto::Id::HostReject:
            if (auto* m = std::get_if<proto::msg::HostReject>(&env->body)) handleHostReject(*m);
            break;
        case proto::Id::HostApproved:
            logEvent(rp::log::Level::Info, "Host approved the join request");
            break;
        case proto::Id::HostCapabilities:
            if (auto* m = std::get_if<proto::msg::HostCapabilities>(&env->body)) {
                handleHostCapabilities(*m);
            }
            break;
        case proto::Id::NegotiationResult:
            if (auto* m = std::get_if<proto::msg::NegotiationResult>(&env->body)) handleNegotiation(*m);
            break;
        case proto::Id::Ping:
            if (auto* m = std::get_if<proto::msg::Ping>(&env->body)) handlePing(*m);
            break;
        case proto::Id::Pong:
            if (auto* m = std::get_if<proto::msg::Pong>(&env->body)) handlePong(*m);
            break;
        case proto::Id::InputPermission:
            if (auto* m = std::get_if<proto::msg::InputPermission>(&env->body)) {
                handleInputPermission(*m);
            }
            break;
        case proto::Id::Kick:
            if (auto* m = std::get_if<proto::msg::Kick>(&env->body)) handleKick(*m);
            break;
        case proto::Id::Bye:
            finish(common::ConnectionState::Disconnected, "Host closed the session", true);
            break;
        default:
            if (type >= 0x0010 && status_.state == common::ConnectionState::Connected && events_.onAppMessage) {
                std::vector<uint8_t> payload(data, data + size);
                events_.onAppMessage(type, payload);
                break;
            }
            logEvent(rp::log::Level::Warning,
                     "Ignoring unexpected message " +
                         std::string(proto::idName(env->type)));
            break;
    }
}

void ClientSession::sendAppMessage(uint16_t type, const std::vector<uint8_t>& payload) {
    if (type < 0x0010) return;
    asio::post(io_, [this, type, payload] {
        if (connection_) connection_->send(type, payload);
    });
}

void ClientSession::handleHostCapabilities(const proto::msg::HostCapabilities& caps) {
    updateStatus([&caps](ClientStatus& s) {
        s.hostName = caps.hostName;
        s.gameName = caps.gameName;
        s.width = caps.width;
        s.height = caps.height;
        s.fps = caps.fps;
        s.bitrateKbps = caps.bitrateKbps;
    });
    logEvent(rp::log::Level::Info, "Host: " + caps.hostName + ", game: " + caps.gameName);

    proto::msg::ClientCapabilities cc;
    cc.supportedCodecs = params_.supportedCodecs;
    proto::Envelope env;
    env.type = proto::Id::ClientCapabilities;
    env.body = std::move(cc);
    send(env);
}

void ClientSession::handleNegotiation(const proto::msg::NegotiationResult& result) {
    if (!result.accepted) {
        const std::string msg = "Codec negotiation failed: " + result.note;
        // Final state FIRST so observers see a consistent snapshot.
        finish(common::ConnectionState::Disconnected, msg, true);
        if (events_.onError) events_.onError(msg);
        return;
    }
    updateStatus([&result](ClientStatus& s) {
        s.codec = static_cast<common::VideoCodec>(result.codec);
        s.width = result.width;
        s.height = result.height;
        s.fps = result.fps;
        s.bitrateKbps = result.bitrateKbps;
        s.state = common::ConnectionState::Connected;
    });
    logEvent(rp::log::Level::Info, "Connected (codec " + result.note + ", " +
                                       std::to_string(result.width) + "x" +
                                       std::to_string(result.height) + "@" +
                                       std::to_string(result.fps) + ")");
    setState(common::ConnectionState::Connected);

    ClientStatus snapshot = status();
    if (events_.onConnected) events_.onConnected(snapshot);
}

void ClientSession::handleHostReject(const proto::msg::HostReject& reject) {
    const std::string msg = "Host rejected: " + reject.reasonText;
    logEvent(rp::log::Level::Warning, msg);
    finish(common::ConnectionState::Disconnected, msg, true);
    if (events_.onError) events_.onError(msg);
}

void ClientSession::handleKick(const proto::msg::Kick& kick) {
    const std::string msg = "Kicked by host: " + kick.reason;
    logEvent(rp::log::Level::Warning, msg);
    finish(common::ConnectionState::Disconnected, msg, true);
}

void ClientSession::handlePing(const proto::msg::Ping& ping) {
    proto::msg::Pong pong;
    pong.echoEpochMs = ping.epochMs;
    proto::Envelope env;
    env.type = proto::Id::Pong;
    env.body = std::move(pong);
    send(env);
}

void ClientSession::handlePong(const proto::msg::Pong& pong) {
    const uint64_t now = steadyNowMs();
    const uint32_t sample =
        pong.echoEpochMs < now ? static_cast<uint32_t>(now - pong.echoEpochMs) : 0;
    updateStatus([sample](ClientStatus& s) {
        s.rttMs = s.rttMs == 0 ? sample : (s.rttMs * 7 + sample) / 8;
    });
}

void ClientSession::handleInputPermission(const proto::msg::InputPermission& perms) {
    common::InputPermissions p;
    p.controller = perms.controller;
    p.keyboard = perms.keyboard;
    p.mouse = perms.mouse;
    p.vibration = perms.vibration;
    updateStatus([&p](ClientStatus& s) { s.input = p; });
    if (events_.onInputPermissions) events_.onInputPermissions(p);
}

void ClientSession::startHeartbeat() {
    if (stopping_.load()) return;

    // Watchdog: no traffic at all for kHeartbeatTimeout -> dead connection.
    if (std::chrono::steady_clock::now() - lastActivity_ > kHeartbeatTimeout) {
        const std::string msg = "Connection timed out (no data from host)";
        logEvent(rp::log::Level::Warning, msg);
        finish(common::ConnectionState::Disconnected, msg, true);
        return;
    }
    // Only ping once CONNECTED; while authenticating the host drives the clock.
    common::ConnectionState st = status().state;
    if (st == common::ConnectionState::Connected || st == common::ConnectionState::Streaming) {
        proto::msg::Ping ping;
        ping.epochMs = steadyNowMs();
        proto::Envelope env;
        env.type = proto::Id::Ping;
        env.body = std::move(ping);
        send(env);
    }

    heartbeat_->expires_after(kHeartbeatPeriod);
    heartbeat_->async_wait([this](const std::error_code& ec) {
        if (ec || stopping_.load()) return;
        startHeartbeat();
    });
}

void ClientSession::send(const proto::Envelope& msg) {
    if (!connection_) return;
    const std::vector<uint8_t> payload = proto::encodeMessage(msg);
    updateStatus([&payload](ClientStatus& s) { s.bytesOut += payload.size(); });
    connection_->send(static_cast<uint16_t>(msg.type), payload);
}

void ClientSession::handleClosed(const std::error_code& ec) {
    if (stopping_.load()) return;
    const bool clean = !ec || ec == asio::error::eof || ec == asio::error::operation_aborted;
    const std::string msg =
        clean ? "Connection closed by host" : ("Connection lost: " + ec.message());
    logEvent(clean ? rp::log::Level::Info : rp::log::Level::Warning, msg);
    finish(common::ConnectionState::Disconnected, msg, true);
}

void ClientSession::stop() {
    if (!running_.exchange(false)) {
        // Already finished (host kicked/rejected/...): just reap the thread.
        joinNetworkThread();
        return;
    }
    stopping_ = true;

    // Graceful BYE: enqueue it on the io thread and let the write drain.
    asio::post(io_, [this] {
        if (connection_ && connection_->isOpen()) {
            proto::msg::Bye bye;
            bye.reason = "Disconnected by user";
            proto::Envelope env;
            env.type = proto::Id::Bye;
            env.body = std::move(bye);
            connection_->sendAndClose(static_cast<uint16_t>(env.type),
                                      proto::encodeMessage(env));
        }
    });
    work_.reset(); // run() exits once the queue drains

    // Give the BYE up to 700 ms to flush before forcing the loop down.
    if (threadDoneFuture_.valid()) {
        threadDoneFuture_.wait_for(700ms);
    }
    joinNetworkThread();

    updateStatus([](ClientStatus& s) { s.state = common::ConnectionState::Disconnected; });
    if (events_.onDisconnected) events_.onDisconnected("Disconnected");
}

void ClientSession::finish(common::ConnectionState finalState, const std::string& reason,
                           bool notifyDisconnected) {
    updateStatus([&finalState, &reason](ClientStatus& s) {
        s.state = finalState;
        if (!reason.empty()) s.lastError = reason;
    });
    setState(finalState);

    if (connection_) connection_->close();
    connection_.reset();

    running_ = false;
    // Called on the io thread itself: release the guard, stop the loop, and
    // let the thread exit on its own. The next start()/destructor reaps it.
    work_.reset();
    io_.stop();

    if (notifyDisconnected && events_.onDisconnected) events_.onDisconnected(reason);
}

ClientStatus ClientSession::status() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return status_;
}

void ClientSession::setState(common::ConnectionState state) {
    updateStatus([state](ClientStatus& s) { s.state = state; });
    if (events_.onState) events_.onState(state);
}

void ClientSession::updateStatus(const std::function<void(ClientStatus&)>& mutator) {
    std::lock_guard<std::mutex> lock(statusMutex_);
    mutator(status_);
}

void ClientSession::logEvent(rp::log::Level level, const std::string& message) {
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

} // namespace rp::client
