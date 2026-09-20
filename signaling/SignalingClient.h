#pragma once
// Phase 13 — client for the RemotePlay signaling server (JSON lines over TCP).
//
// Portable (standalone Asio + vendored nlohmann/json); used by both the host
// (register + client-join notifications + relay dialing) and the client
// (join + relay dialing). The signaling channel carries only names, codes,
// tokens and public addresses; media is always end-to-end encrypted.
//
// TLS: for production deployments put the server behind TLS (server.py
// --tls-cert/--tls-key or a reverse proxy) — documented in DEPLOYING.md.

#include <utility>
#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace rp::signaling {

struct HostRegisterResult {
    int sessionId = 0;
    std::string code;          // 8-char join code, e.g. "57EA6A6Q"
    int maxPlayers = 4;
    std::string hostToken;     // binds the host's UDP endpoint at the relay
    int relayTcpPort = 0;
    int relayUdpPort = 0;
};

struct JoinResult {
    int sessionId = 0;
    int playerIndex = 0;       // 0-based
    std::string hostName;
    std::string token;         // player relay token (TCP pair + UDP bind)
    int relayTcpPort = 0;
    int relayUdpPort = 0;
    std::string hostPublicAddr;
    int hostPublicPort = 0;
};

struct ClientJoined {
    int playerIndex = 0;
    std::string name;
    std::string token;
    std::string clientPublicAddr;
    int clientPublicPort = 0;
};

class SignalingClient {
public:
    using ClientJoinedHandler = std::function<void(const ClientJoined&)>;
    using ClientLeftHandler = std::function<void(int playerIndex)>;
    using LogHandler = std::function<void(const std::string& message)>;

    SignalingClient() = default;
    ~SignalingClient();

    SignalingClient(const SignalingClient&) = delete;
    SignalingClient& operator=(const SignalingClient&) = delete;

    // Synchronous connect (with timeout). One request/response round trip each:
    bool connect(const std::string& host, uint16_t port, uint32_t timeoutMs, std::string* err = nullptr);
    void disconnect();

    [[nodiscard]] std::optional<HostRegisterResult> hostRegister(const std::string& hostName);
    [[nodiscard]] std::optional<JoinResult> join(const std::string& code, const std::string& playerName);

    // Notifications (delivered on the internal reader thread).
    void setClientJoinedHandler(ClientJoinedHandler h) { onClientJoined_ = std::move(h); }
    void setClientLeftHandler(ClientLeftHandler h) { onClientLeft_ = std::move(h); }
    void setLogHandler(LogHandler h) { onLog_ = std::move(h); }

    [[nodiscard]] bool connected() const { return running_.load(); }

private:
    void readerLoop();
    bool writeLine(const std::string& line);
    std::optional<std::string> readLine(uint32_t timeoutMs);
    void handleNotification(const std::string& line);

    std::unique_ptr<asio::io_context> io_;
    std::unique_ptr<asio::ip::tcp::socket> socket_;
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> work_;
    std::thread readerThread_;
    std::atomic<bool> running_{ false };

    std::mutex writeMutex_;            // serializes request/response
    std::mutex readMutex_;             // response lines vs notifications
    std::condition_variable readCv_;
    std::string pendingLine_;          // one-shot response handoff

    ClientJoinedHandler onClientJoined_;
    ClientLeftHandler onClientLeft_;
    LogHandler onLog_;
};

} // namespace rp::signaling
