// RemotePlay - client/ClientSession.h
// Client-side session state machine on its own Asio network thread.
//
//   CONNECTING      -> TCP connect + CLIENT_HELLO sent
//   AUTHENTICATING  -> waiting for host approval and capabilities
//   CONNECTED       -> codec negotiated; heartbeat ping every 2 s
//   DISCONNECTED    -> terminal (host reject, kick, error, or user action)
//
// All public methods are thread-safe; events fire on the network thread.
#pragma once

#include <vector>
#include "common/Log.h"
#include "common/Types.h"
#include "common/Version.h"
#include "networking/Protocol.h"
#include "networking/TcpTransport.h"

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace rp::client {

struct ClientSessionParams {
    std::string clientName = "Player";
    std::string sessionCode;                 // "ABC7-K92P"
    std::string hostAddress = "127.0.0.1";   // IP or hostname (direct mode, Phase 1)
    uint16_t hostPort = kDefaultListenPort;
    uint32_t connectTimeoutMs = 5000;
    // H.264 is the compatibility baseline until decoder probing lands (Phase 5).
    std::vector<common::VideoCodec> supportedCodecs = {common::VideoCodec::H264};
};

// Thread-safe status snapshot for UI polling.
struct ClientStatus {
    common::ConnectionState state = common::ConnectionState::Disconnected;
    std::string hostName;
    std::string gameName;
    common::VideoCodec codec = common::VideoCodec::H264;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps = 0;
    uint32_t bitrateKbps = 0;
    uint32_t rttMs = 0;
    common::InputPermissions input;
    std::string lastError;
    uint64_t bytesIn = 0;
    uint64_t bytesOut = 0;
};

class ClientSession {
public:
    struct Events {
        std::function<void(common::ConnectionState state)> onState;
        // Fired when the host name/game/negotiated stream parameters are known.
        std::function<void(const ClientStatus& status)> onConnected;
        std::function<void(const std::string& reason)> onDisconnected;
        std::function<void(const std::string& message)> onError;
        std::function<void(common::InputPermissions perms)> onInputPermissions;
        std::function<void(rp::log::Level level, const std::string& message)> onLog;
        // App-extension messages (>= 0x0010) from the host while CONNECTED
        // (key exchange, stream control) are forwarded here.
        std::function<void(uint16_t type, const std::vector<uint8_t>& payload)> onAppMessage;
    };

    explicit ClientSession(Events events = {});
    ~ClientSession();

    ClientSession(const ClientSession&) = delete;
    ClientSession& operator=(const ClientSession&) = delete;

    // Begins the connect+handshake sequence. Fails asynchronously via onError
    // (connect timeout/refused). No-op when already active.
    void start(const ClientSessionParams& params);

    // Graceful disconnect: sends BYE, then closes.
    void stop();

    // App-extension message to the host (thread-safe; posts to the network
    // thread). Type must be >= 0x0010; payload is pre-encoded by the caller.
    void sendAppMessage(uint16_t type, const std::vector<uint8_t>& payload);

    // Internet mode (Phase 13): dial the signaling server's relay port and run
    // the normal handshake over the paired pipe (own io thread; replaces the
    // direct connect path).
    void startViaRelay(const ClientSessionParams& params, const std::string& serverHost,
                       uint16_t relayTcpPort, const std::string& token);

    // Tests: attach an already-paired connection.
    void attachConnection(net::TcpConnection::Ptr conn, const ClientSessionParams& params);

    [[nodiscard]] bool active() const { return running_.load(); }
    [[nodiscard]] ClientStatus status() const;

private:
    void runNetworkThread();
    void runNetworkThreadBody();   // io_.run() shared by both start paths
    void joinNetworkThread(); // safe from any thread except the io thread itself
    void handleFrame(uint16_t type, const uint8_t* data, uint32_t size);
    void handleConnected(net::TcpConnection::Ptr conn);
    void handleClosed(const std::error_code& ec);
    void handleHostCapabilities(const proto::msg::HostCapabilities& caps);
    void handleNegotiation(const proto::msg::NegotiationResult& result);
    void handleHostReject(const proto::msg::HostReject& reject);
    void handleKick(const proto::msg::Kick& kick);
    void handlePing(const proto::msg::Ping& ping);
    void handlePong(const proto::msg::Pong& pong);
    void handleInputPermission(const proto::msg::InputPermission& perms);

    void send(const proto::Envelope& msg);
    void sendHello();
    void startHeartbeat();
    void finish(common::ConnectionState finalState, const std::string& reason,
                bool notifyDisconnected);
    void setState(common::ConnectionState state);
    void updateStatus(const std::function<void(ClientStatus&)>& mutator);
    void logEvent(rp::log::Level level, const std::string& message);

    Events events_;
    ClientSessionParams params_;

    asio::io_context io_;
    using WorkGuard = asio::executor_work_guard<asio::io_context::executor_type>;
    std::unique_ptr<WorkGuard> work_;
    std::thread thread_;
    std::unique_ptr<net::TcpClient> connector_;
    net::TcpConnection::Ptr connection_;
    std::string relayServerHost_;
    uint16_t relayServerPort_ = 0;
    std::string relayToken_;
    std::unique_ptr<asio::steady_timer> heartbeat_;
    std::chrono::steady_clock::time_point lastActivity_{};

    mutable std::mutex statusMutex_;
    ClientStatus status_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::promise<void> threadDone_;
    std::future<void> threadDoneFuture_;
};

} // namespace rp::client
