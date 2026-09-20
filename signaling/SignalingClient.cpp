// Phase 13 — signaling client implementation. See SignalingClient.h.

#include "SignalingClient.h"

#include "../common/Log.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>

namespace rp::signaling {
namespace {
using json = nlohmann::json;
}

SignalingClient::~SignalingClient() { disconnect(); }

bool SignalingClient::connect(const std::string& host, uint16_t port, uint32_t timeoutMs, std::string* err) {
    disconnect();
    try {
        io_ = std::make_unique<asio::io_context>();
        work_ = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(
            asio::make_work_guard(*io_));
        socket_ = std::make_unique<asio::ip::tcp::socket>(*io_);

        asio::ip::tcp::resolver resolver(*io_);
        auto results = resolver.resolve(asio::ip::tcp::v4(), host, std::to_string(port));
        if (results.empty()) {
            if (err) *err = "resolve failed";
            socket_.reset(); work_.reset(); io_.reset();
            return false;
        }

        asio::ip::tcp::endpoint ep = *results.begin();
        // Blocking connect with timeout via async + future.
        std::future<void> connected = std::async(std::launch::async, [&] {
            asio::error_code ec;
            socket_->connect(ep, ec);
            if (ec) throw std::system_error(ec);
        });
        if (connected.wait_for(std::chrono::milliseconds(timeoutMs)) == std::future_status::timeout) {
            std::error_code ignore;
            socket_->close(ignore);
            if (err) *err = "connect timeout";
            work_.reset(); io_.reset(); socket_.reset();
            return false;
        }
        try { connected.get(); }
        catch (const std::system_error& e) { if (err) *err = e.code().message(); work_.reset(); io_.reset(); socket_.reset(); return false; }

        running_.store(true);
        readerThread_ = std::thread([this] { readerLoop(); });
        RP_INFO() << "[signaling] connected to " << host << ":" << port;
        return true;
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        work_.reset(); io_.reset(); socket_.reset();
        return false;
    }
}

void SignalingClient::disconnect() {
    if (!running_.exchange(false)) return;
    // Shutdown() wakes the blocking read_until in the reader thread; close()
    // then releases the handle. (The io_context has no runner: this class
    // uses synchronous socket IO only.)
    // shutdown() wakes the blocking read_until in the reader thread; join it
    // BEFORE touching the socket object again (close during a concurrent
    // read is use-after-free).
    if (socket_) {
        std::error_code ec;
        socket_->shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    }
    if (readerThread_.joinable()) readerThread_.join();
    if (socket_) {
        std::error_code ec;
        socket_->close(ec);
        socket_.reset();
    }
    work_.reset();      // release the work guard BEFORE destroying the context
    io_.reset();
}

bool SignalingClient::writeLine(const std::string& line) {
    if (!socket_ || !running_.load()) return false;
    std::lock_guard<std::mutex> lk(writeMutex_);
    asio::error_code ec;
    asio::write(*socket_, asio::buffer(line + "\n"), ec);
    return !ec;
}

std::optional<std::string> SignalingClient::readLine(uint32_t timeoutMs) {
    // Response lines are stashed by the reader thread (pendingLine_).
    std::unique_lock<std::mutex> lk(readMutex_);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (pendingLine_.empty()) {
        if (std::cv_status::timeout == readCv_.wait_until(lk, deadline)) return std::nullopt;
        if (!running_.load()) return std::nullopt;
    }
    std::string line = std::move(pendingLine_);
    pendingLine_.clear();
    return line;
}

void SignalingClient::readerLoop() {
    asio::streambuf buf;
    while (running_.load()) {
        asio::error_code ec;
        size_t n = asio::read_until(*socket_, buf, '\n', ec);
        if (ec || n == 0) break;
        std::istream is(&buf);
        std::string line;
        std::getline(is, line);
        if (line.empty()) continue;
        // Response types are stashed for the waiting request; notifications
        // are dispatched inline without touching pendingLine_.
        {
            json j;
            bool parsed = true;
            try { j = json::parse(line); }
            catch (const json::exception&) { parsed = false; }
            const std::string type = parsed ? j.value("type", "") : "";
            const bool isResponse = (type == "host_registered" || type == "joined" ||
                                     type == "join_rejected" || type == "error" ||
                                     type == "keepalive_ok");
            if (isResponse) {
                {
                    std::lock_guard<std::mutex> lk(readMutex_);
                    pendingLine_ = line;
                }
                readCv_.notify_all();
            } else {
                handleNotification(line);
            }
        }
    }
    running_.store(false);
    readCv_.notify_all();
}

void SignalingClient::handleNotification(const std::string& line) {
    json j;
    try { j = json::parse(line); }
    catch (const json::exception&) { return; }
    const std::string type = j.value("type", "");

    if (type == "client_joined") {
        if (onClientJoined_) {
            ClientJoined cj;
            cj.playerIndex = j.value("player_index", -1);
            cj.name = j.value("name", "");
            cj.token = j.value("token", "");
            if (j.contains("client_public")) {
                cj.clientPublicAddr = j["client_public"].value("addr", "");
                cj.clientPublicPort = j["client_public"].value("port", 0);
            }
            onClientJoined_(cj);
        }
    } else if (type == "client_left") {
        if (onClientLeft_) onClientLeft_(j.value("player_index", -1));
    } else if (type == "session_closed") {
        if (onLog_) onLog_("Session closed by host");
        running_.store(false);
    } else if (type == "keepalive_ok" || type == "host_registered" || type == "joined" ||
               type == "join_rejected" || type == "error") {
        // Request/response types: already stashed for the caller.
    } else {
        if (onLog_) onLog_("signaling: " + type);
    }
}

std::optional<HostRegisterResult> SignalingClient::hostRegister(const std::string& hostName) {
    json req = { {"type", "host_register"}, {"name", hostName} };
    if (!writeLine(req.dump())) return std::nullopt;
    auto line = readLine(5000);
    if (!line) return std::nullopt;
    try {
        const json j = json::parse(*line);
        if (j.value("type", "") != "host_registered") return std::nullopt;
        HostRegisterResult r;
        r.sessionId = j.value("session_id", 0);
        r.code = j.value("code", "");
        r.maxPlayers = j.value("max_players", 4);
        r.hostToken = j.value("host_token", "");
        r.relayTcpPort = j.value("relay_tcp_port", 0);
        r.relayUdpPort = j.value("relay_udp_port", 0);
        return r;
    } catch (const json::exception&) { return std::nullopt; }
}

std::optional<JoinResult> SignalingClient::join(const std::string& code, const std::string& playerName) {
    json req = { {"type", "join"}, {"code", code}, {"name", playerName} };
    if (!writeLine(req.dump())) return std::nullopt;
    auto line = readLine(5000);
    if (!line) return std::nullopt;
    try {
        const json j = json::parse(*line);
        if (j.value("type", "") != "joined") return std::nullopt;
        JoinResult r;
        r.sessionId = j.value("session_id", 0);
        r.playerIndex = j.value("player_index", 0);
        r.hostName = j.value("host_name", "");
        r.token = j.value("token", "");
        r.relayTcpPort = j.value("relay_tcp_port", 0);
        r.relayUdpPort = j.value("relay_udp_port", 0);
        if (j.contains("host_public")) {
            r.hostPublicAddr = j["host_public"].value("addr", "");
            r.hostPublicPort = j["host_public"].value("port", 0);
        }
        return r;
    } catch (const json::exception&) { return std::nullopt; }
}

} // namespace rp::signaling
