// RemotePlay - host/HostSession.h
// Host-side session state machine running on its own Asio network thread.
//
// State flow per client (mirrors common::ConnectionState):
//   CONNECTING      -> waiting for CLIENT_HELLO (10 s timeout)
//   AUTHENTICATING  -> session code validated, waiting for host APPROVAL
//                      (75 s timeout to cover the human decision)
//   CONNECTED       -> capabilities exchanged, codec negotiated, heartbeat 2 s
//   DISCONNECTED    -> terminal
//
// All public methods are thread-safe; events fire on the network thread.
#pragma once

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
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rp::host {

struct HostSessionParams {
    std::string hostName = "Host";            // e.g. "Mahdyar's PC"
    std::string gameName;                     // e.g. "Rayman Legends"
    common::CaptureMode captureMode = common::CaptureMode::Window;
    common::Resolution resolution{};
    uint32_t fps = 60;
    uint32_t bitrateKbps = 8000;
    common::VideoCodec preferredCodec = common::VideoCodec::H264;
    uint16_t listenPort = kDefaultListenPort; // 0 = ephemeral (tests)
    bool autoApprove = false;                 // tests / "require approval" disabled
    common::InputPermissions inputDefaults;
    size_t maxClients = kMaxClients;
};

// Plain data snapshot for UI consumption (thread-safe copy).
struct ClientRow {
    uint32_t id = 0;
    std::string name;
    common::ConnectionState state = common::ConnectionState::Disconnected;
    uint32_t rttMs = 0;
    bool inputEnabled = true;
    std::string address;
};

class HostSession {
public:
    struct Events {
        // Fired once when the listener is ready (actual port + session code).
        std::function<void(uint16_t port, const std::string& sessionCode)> onListening;
        // A client presented a valid session code and awaits the host decision.
        std::function<void(uint32_t clientId, const std::string& name)> onApprovalRequest;
        // Any state / row change (connect, approve, negotiate, disconnect).
        std::function<void(const ClientRow& row)> onClientUpdated;
        std::function<void(uint32_t clientId, common::InputPermissions perms)> onInputPermissions;
        std::function<void(rp::log::Level level, const std::string& message)> onLog;
        std::function<void()> onStopped;
        // App-extension messages (Phases 12-16): KEY_EXCHANGE, SESSION_KEY_READY,
        // STREAM_START, STREAM_STOP, KEYFRAME_REQUEST arriving from a CONNECTED
        // client are forwarded here instead of being rejected by the state machine.
        std::function<void(uint32_t clientId, uint16_t type, const std::vector<uint8_t>& payload)> onAppMessage;
    };

    explicit HostSession(Events events = {});
    ~HostSession();

    HostSession(const HostSession&) = delete;
    HostSession& operator=(const HostSession&) = delete;

    // Starts the network thread and listener. Returns false if the port cannot
    // be bound (the onListening event will not fire in that case).
    [[nodiscard]] bool start(const HostSessionParams& params);

    // Stops the session, closing all client connections. Idempotent.
    void stop();

    [[nodiscard]] bool running() const { return running_.load(); }
    [[nodiscard]] const std::string& sessionCode() const { return sessionCode_; }
    [[nodiscard]] uint16_t port() const { return actualPort_; }

    // Thread-safe client table copy for UI polling.
    [[nodiscard]] std::vector<ClientRow> clients() const;

    // Host decisions (thread-safe; posted onto the network thread).
    void approveClient(uint32_t clientId, bool accept);
    void kickClient(uint32_t clientId, const std::string& reason = "Kicked by host");
    // Disables/enables all input forwarding for this client.
    void setClientInput(uint32_t clientId, bool enableAll);

    // App-extension message to a CONNECTED client (thread-safe; posts to the
    // network thread). Types must be >= 0x0010 (streaming/app range).
    void sendAppMessage(uint32_t clientId, uint16_t type, const std::vector<uint8_t>& payload);

    // Internet mode (Phase 13): attach an already-relay-paired connection as
    // a new client (instead of a TCP accept). Thread-safe.
    void attachConnection(net::TcpConnection::Ptr conn);

    [[nodiscard]] static std::chrono::milliseconds steadyNowMs();

private:
    void runNetworkThread();
    void handleNewConnection(net::TcpConnection::Ptr conn);
    void handleFrame(uint32_t clientId, uint16_t type, uint8_t flags, const uint8_t* data,
                     uint32_t size);
    void handleClientClosed(uint32_t clientId, const std::error_code& ec);
    void handleClientHello(uint32_t clientId, const proto::msg::ClientHello& hello);
    void handleClientCapabilities(uint32_t clientId,
                                  const proto::msg::ClientCapabilities& caps);
    void handlePing(uint32_t clientId, const proto::msg::Ping& ping);
    void handlePong(uint32_t clientId, const proto::msg::Pong& pong);
    void handleBye(uint32_t clientId, const proto::msg::Bye& bye);

    void sendTo(uint32_t clientId, const proto::Envelope& msg);
    void sendAndClose(uint32_t clientId, const proto::Envelope& msg);
    void dropClient(uint32_t clientId, bool sendReject, proto::Reason reason,
                    const std::string& text);
    void setState(uint32_t clientId, common::ConnectionState state);
    void pushInputPermissions(uint32_t clientId);
    void housekeeping();
    void rebuildSnapshot();
    void protocolError(const std::string& what);
    void logEvent(rp::log::Level level, const std::string& message);

    // Internal record; touched only on the network thread.
    struct ClientRecord {
        uint32_t id = 0;
        std::string name;
        common::ConnectionState state = common::ConnectionState::Connecting;
        std::chrono::steady_clock::time_point lastActivity{};
        std::chrono::steady_clock::time_point lastHostPing{};
        net::TcpConnection::Ptr connection;
        common::InputPermissions input;
        uint32_t rttMs = 0;
        std::string address;
    };

    Events events_;
    HostSessionParams params_;

    asio::io_context io_;
    using WorkGuard = asio::executor_work_guard<asio::io_context::executor_type>;
    std::unique_ptr<WorkGuard> work_;
    std::thread thread_;
    std::unique_ptr<net::TcpServer> server_;
    asio::steady_timer housekeeper_{io_};

    std::map<uint32_t, ClientRecord> clients_;      // network thread only
    mutable std::mutex snapshotMutex_;
    std::vector<ClientRow> snapshot_;               // guarded copy for UI

    uint32_t nextClientId_ = 1;
    std::string sessionCode_;
    uint16_t actualPort_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
};

} // namespace rp::host
