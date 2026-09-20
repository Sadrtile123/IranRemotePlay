// RemotePlay - tests/test_handshake.cpp
// In-process integration tests: HostSession <-> ClientSession over loopback
// TCP. Exercises the complete control-channel protocol: HELLO, session-code
// validation, approval, capability exchange, codec negotiation, heartbeat
// RTT, input permissions, kick and graceful disconnect.
#include "client/ClientSession.h"
#include "host/HostSession.h"

#include "TestHarness.hpp"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace rp;
using namespace rp::host;
using namespace rp::client;
using namespace std::chrono_literals;

namespace {

// Condition-variable flag for cross-thread event waiting.
class Flag {
public:
    void set(const std::string& value = "") {
        {
            std::lock_guard<std::mutex> lock(m_);
            ready_ = true;
            value_ = value;
        }
        cv_.notify_all();
    }
    // Returns true when the flag was set within the timeout.
    bool wait(int timeoutMs, std::string* value = nullptr) {
        std::unique_lock<std::mutex> lock(m_);
        const bool ok = cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                     [this] { return ready_; });
        if (ok && value) *value = value_;
        return ok;
    }
    [[nodiscard]] bool isSet() const {
        std::lock_guard<std::mutex> lock(m_);
        return ready_;
    }

private:
    mutable std::mutex m_;
    std::condition_variable cv_;
    bool ready_ = false;
    std::string value_;
};

HostSessionParams defaultParams() {
    HostSessionParams p;
    p.hostName = "TestHost";
    p.gameName = "Rayman Legends";
    p.resolution = common::Resolution{1920, 1080};
    p.fps = 60;
    p.bitrateKbps = 8000;
    p.preferredCodec = common::VideoCodec::H264;
    p.listenPort = 0; // ephemeral
    p.autoApprove = true;
    return p;
}

// Polls a predicate on the (thread-safe) host client table.
bool waitForClient(host::HostSession& h, int timeoutMs,
                   const std::function<bool(const std::vector<host::ClientRow>&)>& pred) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred(h.clients())) return true;
        std::this_thread::sleep_for(20ms);
    }
    return pred(h.clients());
}

} // namespace

RP_TEST(handshake_auto_approve_and_heartbeat) {
    Flag listening;
    uint16_t port = 0;
    std::string code;

    host::HostSession::Events hev;
    hev.onListening = [&](uint16_t p, const std::string& c) {
        port = p;
        code = c;
        listening.set();
    };

    host::HostSession host(hev);
    CHECK(host.start(defaultParams()));
    CHECK(listening.wait(2000));
    CHECK(port != 0);
    CHECK(!code.empty());

    Flag connected;
    std::string disconnectReason;
    Flag disconnected;

    client::ClientSession::Events cev;
    cev.onConnected = [&](const client::ClientStatus&) { connected.set(); };
    cev.onDisconnected = [&](const std::string& reason) {
        disconnectReason = reason;
        disconnected.set(reason);
    };

    client::ClientSession client(cev);
    client::ClientSessionParams cp;
    cp.clientName = "Mahdyar";
    cp.sessionCode = code;
    cp.hostAddress = "127.0.0.1";
    cp.hostPort = port;
    cp.connectTimeoutMs = 3000;
    client.start(cp);

    CHECK(connected.wait(4000));
    const auto st = client.status();
    CHECK_EQ(st.state, common::ConnectionState::Connected);
    CHECK_EQ(st.hostName, std::string("TestHost"));
    CHECK_EQ(st.gameName, std::string("Rayman Legends"));
    CHECK_EQ(st.codec, common::VideoCodec::H264);
    CHECK_EQ(st.width, uint32_t(1920));
    CHECK_EQ(st.height, uint32_t(1080));
    CHECK_EQ(st.fps, uint32_t(60));
    CHECK_EQ(st.input.controller, true);

    // The host must see the client CONNECTED with a sane RTT after a few beats.
    CHECK(waitForClient(host, 6000, [](const std::vector<host::ClientRow>& rows) {
        return rows.size() == 1 && rows[0].state == common::ConnectionState::Connected;
    }));
    std::this_thread::sleep_for(2500ms); // >= one heartbeat round trip
    const auto rows = host.clients();
    CHECK_EQ(rows.size(), size_t(1));
    CHECK_EQ(rows[0].name, std::string("Mahdyar"));
    CHECK(rows[0].rttMs < 250);
    CHECK(client.status().rttMs < 250);

    // Kick -> client observes the disconnect.
    host.kickClient(rows[0].id);
    CHECK(disconnected.wait(4000));
    CHECK(disconnectReason.find("Kicked") != std::string::npos);

    client.stop();
    host.stop();
}

RP_TEST(handshake_rejects_wrong_session_code) {
    Flag listening;
    uint16_t port = 0;
    std::string code;

    host::HostSession::Events hev;
    hev.onListening = [&](uint16_t p, const std::string& c) {
        port = p;
        code = c;
        listening.set();
    };
    host::HostSession host(hev);
    CHECK(host.start(defaultParams()));
    CHECK(listening.wait(2000));

    std::string error;
    Flag errored;
    Flag disconnected;

    client::ClientSession::Events cev;
    cev.onError = [&](const std::string& msg) { error = msg; errored.set(msg); };
    cev.onDisconnected = [&](const std::string&) { disconnected.set(); };

    client::ClientSession client(cev);
    client::ClientSessionParams cp;
    cp.clientName = "Intruder";
    cp.sessionCode = "ZZZZ-ZZZZ"; // deliberately wrong
    cp.hostAddress = "127.0.0.1";
    cp.hostPort = port;
    client.start(cp);

    CHECK(errored.wait(4000));
    CHECK(error.find("session code") != std::string::npos ||
          error.find("Invalid") != std::string::npos);
    CHECK(disconnected.wait(2000));
    CHECK_EQ(client.status().state, common::ConnectionState::Disconnected);

    // Host must not have any lingering client.
    CHECK(waitForClient(host, 2000, [](const std::vector<host::ClientRow>& rows) {
        return rows.empty();
    }));

    client.stop();
    host.stop();
}

RP_TEST(handshake_manual_approval_flow) {
    Flag listening;
    uint16_t port = 0;
    std::string code;

    host::HostSession::Events hev;
    hev.onListening = [&](uint16_t p, const std::string& c) {
        port = p;
        code = c;
        listening.set();
    };
    Flag approvalRequest;
    uint32_t pendingId = 0;

    auto params = defaultParams();
    params.autoApprove = false;
    hev.onApprovalRequest = [&](uint32_t id, const std::string& name) {
        (void)name;
        pendingId = id;
        approvalRequest.set();
    };

    host::HostSession host(hev);
    CHECK(host.start(params));
    CHECK(listening.wait(2000));

    Flag connected;
    client::ClientSession::Events cev;
    cev.onConnected = [&](const client::ClientStatus&) { connected.set(); };
    // Wired before session construction so the host's INPUT_PERMISSION push
    // is actually observed by the test.
    Flag permsDisabled;
    cev.onInputPermissions = [&](common::InputPermissions p) {
        if (p.allDisabled()) permsDisabled.set();
    };

    client::ClientSession client(cev);
    client::ClientSessionParams cp;
    cp.clientName = "Alex";
    cp.sessionCode = code;
    cp.hostAddress = "127.0.0.1";
    cp.hostPort = port;
    client.start(cp);

    // No streaming until the host approves.
    std::this_thread::sleep_for(300ms);
    CHECK(!connected.isSet());

    CHECK(approvalRequest.wait(4000));
    host.approveClient(pendingId, true);
    CHECK(connected.wait(4000));

    // Disable input for the client -> INPUT_PERMISSION message arrives.
    host.setClientInput(pendingId, false);
    CHECK(permsDisabled.wait(4000));
    CHECK(!client.status().input.controller);
    CHECK(!client.status().input.keyboard);

    // Graceful client disconnect (BYE) -> host table empties.
    client.stop();
    CHECK(waitForClient(host, 4000, [](const std::vector<host::ClientRow>& rows) {
        return rows.empty();
    }));

    host.stop();
}

RP_TEST(handshake_codec_negotiation_failure) {
    Flag listening;
    uint16_t port = 0;
    std::string code;

    host::HostSession::Events hev;
    hev.onListening = [&](uint16_t p, const std::string& c) {
        port = p;
        code = c;
        listening.set();
    };
    host::HostSession host(hev);
    CHECK(host.start(defaultParams()));
    CHECK(listening.wait(2000));

    std::string error;
    Flag errored;
    client::ClientSession::Events cev;
    cev.onError = [&](const std::string& msg) { error = msg; errored.set(msg); };

    client::ClientSession client(cev);
    client::ClientSessionParams cp;
    cp.clientName = "AV1Only";
    cp.sessionCode = code;
    cp.hostAddress = "127.0.0.1";
    cp.hostPort = port;
    cp.supportedCodecs = {common::VideoCodec::Av1}; // host offers H.264/HEVC only
    client.start(cp);

    CHECK(errored.wait(4000));
    CHECK(error.find("codec") != std::string::npos ||
          error.find("Codec") != std::string::npos);
    CHECK_EQ(client.status().state, common::ConnectionState::Disconnected);

    client.stop();
    host.stop();
}

RP_TEST_MAIN("handshake")
