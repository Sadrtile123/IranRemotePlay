// RemotePlay - networking/TcpTransport.h
// Standalone-Asio based TCP transport for the control channel.
//
// Threading contract:
//  * All socket operations are executed on a single io_context thread owned
//    by the session layer.
//  * Public methods (send/close/stop/connect) may be called from any thread;
//    they internally post onto the io_context.
//  * Frame and close callbacks are invoked on the io thread. Consumers must
//    marshal to their own thread (the Qt UI uses queued invocations).
#pragma once

#include "networking/Packet.h"

#include <asio.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace rp::net {

class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    using Ptr = std::shared_ptr<TcpConnection>;
    // (frameType, flags, payloadData, payloadSize)
    using FrameHandler = std::function<void(uint16_t, uint8_t, const uint8_t*, uint32_t)>;
    using CloseHandler = std::function<void(const std::error_code&)>;

    ~TcpConnection();

    // Adopts an already-connected (or accepted) socket.
    [[nodiscard]] static Ptr make(asio::ip::tcp::socket socket, std::string label);

    void setHandlers(FrameHandler onFrame, CloseHandler onClose);
    // Starts the read loop. Call once, after setHandlers.
    void start();

    // Thread-safe. Enqueues a full frame (header + payload).
    void send(uint16_t type, uint8_t flags, const uint8_t* payload, uint32_t payloadSize);
    void send(uint16_t type, const std::vector<uint8_t>& payload);
    // Sends all queued frames then closes once the write completes.
    void sendAndClose(uint16_t type, const std::vector<uint8_t>& payload);

    // Thread-safe. Idempotent.
    void close();

    [[nodiscard]] bool isOpen() const;
    [[nodiscard]] const std::string& label() const;
    [[nodiscard]] std::string remoteAddress() const; // "ip:port" or "unknown"

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

private:
    TcpConnection(asio::ip::tcp::socket socket, std::string label);

    void readHeader();
    void readBody(uint16_t type, uint8_t flags, uint32_t payloadSize);
    void pumpWrite();
    void shutdown(const std::error_code& ec);

    asio::any_io_executor ex_; // bound to the owning io_context
    asio::ip::tcp::socket socket_;
    const std::string label_;

    std::array<uint8_t, kFrameHeaderSize> headerBuf_{};
    std::vector<uint8_t> bodyBuf_;
    std::deque<std::vector<uint8_t>> outbox_;
    bool writing_ = false;
    bool closeAfterWrite_ = false;
    bool closed_ = false;

    FrameHandler onFrame_;
    CloseHandler onClose_;
};

class TcpServer {
public:
    // port == 0 picks an ephemeral port; use port() afterwards.
    TcpServer(asio::io_context& io, uint16_t port);
    ~TcpServer();

    using ConnectionHandler = std::function<void(TcpConnection::Ptr)>;

    void setConnectionHandler(ConnectionHandler handler);
    [[nodiscard]] uint16_t port() const;
    void stop(); // thread-safe

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

private:
    void acceptNext();

    asio::io_context& io_;
    asio::ip::tcp::acceptor acceptor_;
    ConnectionHandler onConnection_;
    std::atomic<bool> stopped_{false};
};

class TcpClient {
public:
    using ConnectResult = std::function<void(std::error_code, TcpConnection::Ptr)>;

    explicit TcpClient(asio::io_context& io);

    // Async connect with timeout. The callback is invoked exactly once, on the
    // io thread. Safe to call only when no previous attempt is in flight.
    void connect(const std::string& host, uint16_t port, uint32_t timeoutMs, ConnectResult result);

    // Phase 13 — relay connect: TCP connect to the signaling server's relay
    // port, speak the JSON pairing handshake ("relay"/token/role), then hand
    // back a normal TcpConnection running the RemotePlay frame protocol.
    // Runs entirely on this client's io_context (session thread safe).
    void connectRelay(const std::string& host, uint16_t port, const std::string& token,
                      const std::string& role, uint32_t timeoutMs, ConnectResult result);

    void cancel(); // cancels an in-flight attempt (callback fires with operation_aborted)

    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;

private:
    asio::io_context& io_;
    std::shared_ptr<void> attempt_; // shared state, see TcpTransport.cpp
};

} // namespace rp::net
