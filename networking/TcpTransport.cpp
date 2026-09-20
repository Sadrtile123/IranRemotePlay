// RemotePlay - networking/TcpTransport.cpp
#include "networking/TcpTransport.h"

#include "common/Log.h"

#include <utility>

namespace rp::net {

namespace {

std::string endpointToString(const asio::ip::tcp::socket& s) {
    asio::error_code ec;
    const auto ep = s.remote_endpoint(ec);
    if (ec) return "unknown";
    return ep.address().to_string() + ":" + std::to_string(ep.port());
}

} // namespace

// ---------------------------------------------------------------------------
// TcpConnection
// ---------------------------------------------------------------------------

TcpConnection::TcpConnection(asio::ip::tcp::socket socket, std::string label)
    : ex_(socket.get_executor()), socket_(std::move(socket)), label_(std::move(label)) {}

TcpConnection::~TcpConnection() {
    std::error_code ec;
    socket_.close(ec);
}

TcpConnection::Ptr TcpConnection::make(asio::ip::tcp::socket socket, std::string label) {
    // Private constructor trick: new is allowed inside the class.
    struct Enable : TcpConnection {
        Enable(asio::ip::tcp::socket s, std::string l) : TcpConnection(std::move(s), std::move(l)) {}
    };
    return std::make_shared<Enable>(std::move(socket), std::move(label));
}

void TcpConnection::setHandlers(FrameHandler onFrame, CloseHandler onClose) {
    onFrame_ = std::move(onFrame);
    onClose_ = std::move(onClose);
}

void TcpConnection::start() {
    auto self = shared_from_this();
    asio::post(ex_, [self] { if (!self->closed_) self->readHeader(); });
}

void TcpConnection::send(uint16_t type, uint8_t flags, const uint8_t* payload,
                         uint32_t payloadSize) {
    std::vector<uint8_t> frame = makeFrame(type, flags, payload, payloadSize);
    auto self = shared_from_this();
    asio::post(ex_, [self, frame = std::move(frame)]() mutable {
        if (self->closed_) return;
        self->outbox_.push_back(std::move(frame));
        self->pumpWrite();
    });
}

void TcpConnection::send(uint16_t type, const std::vector<uint8_t>& payload) {
    send(type, 0, payload.empty() ? nullptr : payload.data(),
         static_cast<uint32_t>(payload.size()));
}

void TcpConnection::sendAndClose(uint16_t type, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> frame = makeFrame(type, payload);
    auto self = shared_from_this();
    asio::post(ex_, [self, frame = std::move(frame)]() mutable {
        if (self->closed_) return;
        self->outbox_.push_back(std::move(frame));
        self->closeAfterWrite_ = true;
        self->pumpWrite();
    });
}

void TcpConnection::close() {
    auto self = shared_from_this();
    asio::post(ex_, [self] { self->shutdown(std::error_code()); });
}

bool TcpConnection::isOpen() const { return !closed_; }

const std::string& TcpConnection::label() const { return label_; }

std::string TcpConnection::remoteAddress() const { return endpointToString(socket_); }

void TcpConnection::readHeader() {
    auto self = shared_from_this();
    asio::async_read(socket_, asio::buffer(headerBuf_),
                     [self](std::error_code ec, size_t /*bytes*/) {
                         if (ec) {
                             self->shutdown(ec);
                             return;
                         }
                         const auto h = parseFrameHeader(self->headerBuf_.data(),
                                                         self->headerBuf_.size());
                         if (!h) {
                             RP_DEBUG() << "Protocol violation from " << self->label_
                                        << ": invalid frame header";
                             self->shutdown(asio::error::invalid_argument);
                             return;
                         }
                         self->readBody(h->type, h->flags, h->payloadSize);
                     });
}

void TcpConnection::readBody(uint16_t type, uint8_t flags, uint32_t payloadSize) {
    auto self = shared_from_this();
    if (payloadSize == 0) {
        if (self->onFrame_) self->onFrame_(type, flags, nullptr, 0);
        if (!self->closed_) self->readHeader();
        return;
    }
    bodyBuf_.resize(payloadSize);
    asio::async_read(socket_, asio::buffer(bodyBuf_),
                     [self, type, flags, payloadSize](std::error_code ec, size_t /*bytes*/) {
                         if (ec) {
                             self->shutdown(ec);
                             return;
                         }
                         if (self->onFrame_) {
                             self->onFrame_(type, flags, self->bodyBuf_.data(), payloadSize);
                         }
                         if (!self->closed_) self->readHeader();
                     });
}

void TcpConnection::pumpWrite() {
    if (writing_ || outbox_.empty() || closed_) return;
    writing_ = true;
    auto self = shared_from_this();
    asio::async_write(socket_, asio::buffer(outbox_.front()),
                      [self](std::error_code ec, size_t /*bytes*/) {
                          self->writing_ = false;
                          if (ec) {
                              self->shutdown(ec);
                              return;
                          }
                          if (!self->outbox_.empty()) self->outbox_.pop_front();
                          if (self->closeAfterWrite_ && self->outbox_.empty()) {
                              self->shutdown(std::error_code());
                              return;
                          }
                          self->pumpWrite();
                      });
}

void TcpConnection::shutdown(const std::error_code& ec) {
    if (closed_) return;
    closed_ = true;
    std::error_code ignore;
    socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ignore);
    socket_.close(ignore);
    outbox_.clear();
    if (onClose_) onClose_(ec);
}

// ---------------------------------------------------------------------------
// TcpServer
// ---------------------------------------------------------------------------

TcpServer::TcpServer(asio::io_context& io, uint16_t port)
    : io_(io), acceptor_(io, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port)) {
    acceptor_.set_option(asio::socket_base::reuse_address(true));
}

TcpServer::~TcpServer() { stop(); }

void TcpServer::setConnectionHandler(ConnectionHandler handler) {
    onConnection_ = std::move(handler);
    acceptNext();
}

uint16_t TcpServer::port() const {
    asio::error_code ec;
    const auto ep = acceptor_.local_endpoint(ec);
    return ec ? 0 : ep.port();
}

void TcpServer::stop() {
    if (stopped_.exchange(true)) return;
    asio::post(io_, [this] {
        std::error_code ec;
        acceptor_.close(ec);
    });
}

void TcpServer::acceptNext() {
    if (stopped_) return;
    acceptor_.async_accept(
        [this](std::error_code ec, asio::ip::tcp::socket socket) {
            if (stopped_) return;
            if (ec == asio::error::operation_aborted) return;
            if (ec) {
                RP_WARN() << "Accept failed: " << ec.message();
            } else {
                const std::string label = endpointToString(socket);
                RP_DEBUG() << "Accepted connection from " << label;
                auto conn = TcpConnection::make(std::move(socket), label);
                if (onConnection_) onConnection_(conn);
            }
            acceptNext();
        });
}

// ---------------------------------------------------------------------------
// TcpClient
// ---------------------------------------------------------------------------

namespace {

// Self-contained connect attempt: keeps itself alive while any async
// operation is outstanding and guarantees the callback fires exactly once.
struct ConnectAttempt : std::enable_shared_from_this<ConnectAttempt> {
    ConnectAttempt(asio::io_context& io, std::string host, uint16_t port, uint32_t timeoutMs,
                   TcpClient::ConnectResult result)
        : socket(io), timer(io), host(std::move(host)), port(std::to_string(port)),
          timeoutMs(timeoutMs), result(std::move(result)) {}

    void start() {
        auto self = shared_from_this();
        timer.expires_after(std::chrono::milliseconds(timeoutMs));
        timer.async_wait([self](std::error_code ec) {
            if (ec == asio::error::operation_aborted) return; // finished in time
            if (self->finished) return;
            self->finish(std::make_error_code(std::errc::timed_out), nullptr);
            std::error_code cancelEc;
            self->socket.close(cancelEc);
            self->resolver.cancel();
        });
        resolver.async_resolve(host, port,
                               [self](std::error_code ec, asio::ip::tcp::resolver::results_type results) {
                                   if (self->finished) return;
                                   if (ec) {
                                       self->finish(ec, nullptr);
                                       return;
                                   }
                                   asio::async_connect(
                                       self->socket, results,
                                       [self](std::error_code ec2, asio::ip::tcp::endpoint) {
                                           if (self->finished) return;
                                           if (ec2) {
                                               self->finish(ec2, nullptr);
                                               return;
                                           }
                                           auto conn = TcpConnection::make(
                                               std::move(self->socket), "client");
                                           self->finish(std::error_code(), conn);
                                       });
                               });
    }

    void cancel() {
        if (finished) return;
        finished = true;
        std::error_code ec;
        socket.close(ec);
        resolver.cancel();
        timer.cancel();
    }

    void finish(const std::error_code& ec, TcpConnection::Ptr conn) {
        if (finished) return;
        finished = true;
        timer.cancel();
        result(ec, std::move(conn));
    }

    asio::ip::tcp::socket socket;
    asio::steady_timer timer;
    asio::ip::tcp::resolver resolver{socket.get_executor()};
    std::string host;
    std::string port;
    uint32_t timeoutMs;
    TcpClient::ConnectResult result;
    bool finished = false;
};


// Phase 13 — relay handshake attempt: connect, exchange JSON lines, adopt.
struct RelayConnectAttempt : std::enable_shared_from_this<RelayConnectAttempt> {
    RelayConnectAttempt(asio::io_context& io, std::string host, uint16_t port,
                        std::string token, std::string role, uint32_t timeoutMs,
                        TcpClient::ConnectResult result)
        : socket(io), timer(io), host(std::move(host)), port(std::to_string(port)),
          token(std::move(token)), role(std::move(role)), timeoutMs(timeoutMs),
          result(std::move(result)) {}

    void start() {
        auto self = shared_from_this();
        timer.expires_after(std::chrono::milliseconds(timeoutMs));
        timer.async_wait([self](std::error_code ec) {
            if (ec == asio::error::operation_aborted) return;
            if (self->finished) return;
            self->finish(std::make_error_code(std::errc::timed_out), nullptr);
            std::error_code cancelEc;
            self->socket.close(cancelEc);
            self->resolver.cancel();
        });
        resolver.async_resolve(host, port,
            [self](std::error_code ec, asio::ip::tcp::resolver::results_type results) {
                if (self->finished) return;
                if (ec) { self->finish(ec, nullptr); return; }
                asio::async_connect(self->socket, results,
                    [self](std::error_code ec2, asio::ip::tcp::endpoint) {
                        if (self->finished) return;
                        if (ec2) { self->finish(ec2, nullptr); return; }
                        self->sendHandshake();
                    });
            });
    }

    void sendHandshake() {
        auto self = shared_from_this();
        // Minimal JSON; the server only needs type/token/role.
        const std::string line = "{\"type\":\"relay\",\"token\":\"" + token + "\",\"role\":\"" + role + "\"}\n";
        asio::async_write(socket, asio::buffer(line),
            [self](std::error_code ec, size_t) {
                if (self->finished) return;
                if (ec) { self->finish(ec, nullptr); return; }
                self->readReply();
            });
    }

    void readReply() {
        auto self = shared_from_this();
        asio::async_read_until(socket, replyBuf, '\n',
            [self](std::error_code ec, size_t) {
                if (self->finished) return;
                if (ec) { self->finish(ec, nullptr); return; }
                std::istream is(&self->replyBuf);
                std::string line;
                std::getline(is, line);
                if (line.find("relay_rejected") != std::string::npos) {
                    self->finish(std::make_error_code(std::errc::connection_refused), nullptr);
                    return;
                }
                if (line.find("relay_paired") != std::string::npos) {
                    auto conn = TcpConnection::make(std::move(self->socket), "relay");
                    self->finish(std::error_code(), conn);
                    return;
                }
                // relay_accepted: wait for the pairing line.
                self->readReply();
            });
    }

    void cancel() {
        if (finished) return;
        finished = true;
        std::error_code ec;
        socket.close(ec);
        resolver.cancel();
        timer.cancel();
    }

    void finish(const std::error_code& ec, TcpConnection::Ptr conn) {
        if (finished) return;
        finished = true;
        timer.cancel();
        result(ec, std::move(conn));
    }

    asio::ip::tcp::socket socket;
    asio::steady_timer timer;
    asio::ip::tcp::resolver resolver{socket.get_executor()};
    asio::streambuf replyBuf;
    std::string host;
    std::string port;
    std::string token;
    std::string role;
    uint32_t timeoutMs;
    TcpClient::ConnectResult result;
    bool finished = false;
};

} // namespace

TcpClient::TcpClient(asio::io_context& io) : io_(io) {}

void TcpClient::connect(const std::string& host, uint16_t port, uint32_t timeoutMs,
                        ConnectResult result) {
    auto attempt = std::make_shared<ConnectAttempt>(io_, host, port, timeoutMs, std::move(result));
    attempt_ = attempt;
    attempt->start();
}

void TcpClient::cancel() {
    if (auto a = std::static_pointer_cast<ConnectAttempt>(attempt_)) {
        a->cancel();
        attempt_.reset();
    }
}


void TcpClient::connectRelay(const std::string& host, uint16_t port, const std::string& token,
                             const std::string& role, uint32_t timeoutMs, ConnectResult result) {
    auto attempt = std::make_shared<RelayConnectAttempt>(io_, host, port, token, role, timeoutMs,
                                                         std::move(result));
    attempt_ = attempt;
    attempt->start();
}

} // namespace rp::net
