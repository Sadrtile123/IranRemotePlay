// Phases 13/14 integration test — C++ app talking to the REAL Python
// signaling server (started as a subprocess): register, join, relay TCP pair
// with opaque RemotePlay frames, and UDP media relay forwarding.

#include "../networking/Packet.h"
#include "../networking/TcpTransport.h"
#include "../networking/UdpTransport.h"
#include "../signaling/SignalingClient.h"

#include "TestHarness.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <string>
#include <thread>

#if defined(_WIN32)
#define RP_POPEN  _popen
#define RP_PCLOSE _pclose
#else
#define RP_POPEN  popen
#define RP_PCLOSE pclose
#endif

using namespace rp::net;
using namespace rp::signaling;

namespace {
uint16_t pickPort() {
    static int salt = static_cast<int>(std::chrono::steady_clock::now().time_since_epoch().count() % 20000);
    return static_cast<uint16_t>(42000 + salt);
}
} // namespace

RP_TEST(signaling_end_to_end_with_python_server) {
    const uint16_t port = pickPort();

    // ---- start the real server ----
    // POSIX:  shell backgrounds the server and echoes the PID so popen returns.
    // Windows: `start /B` launches detached in the same console; the server is
    // never killed by the test (it self-expires, and CI VMs are ephemeral).
    const char* py = std::getenv("REMOTEPLAY_PYTHON");
    std::string cmd;
#if defined(_WIN32)
    std::string python = py && *py ? py : "python";
    cmd = "cd /d ..\\server\\signaling-server && start \"rp_server\" /B " + python +
          " server.py --host 127.0.0.1 --port " + std::to_string(port) +
          " >NUL 2>&1";
#else
    std::string python = py && *py ? py : "python3";
    cmd = "cd ../server/signaling-server && " + python +
          " server.py --host 127.0.0.1 --port " + std::to_string(port) +
          " >/tmp/rp_server_test.log 2>&1 & echo $!";
#endif
    FILE* pf = RP_POPEN(cmd.c_str(), "r");
    char buf[32]{};
    (void)fgets(buf, sizeof(buf), pf);
    RP_PCLOSE(pf);
    std::this_thread::sleep_for(std::chrono::milliseconds(700));

    SignalingClient hostSig, clientSig;
    std::string err;

    // ---- host registers ----
    CHECK(hostSig.connect("127.0.0.1", port, 3000, &err));
    const auto reg = hostSig.hostRegister("TestHostPC");
    CHECK(reg.has_value());
    CHECK(reg->code.size() == 8);
    CHECK(reg->relayTcpPort > 0);
    CHECK(reg->relayUdpPort > 0);

    // ---- host notification handler BEFORE the join (avoid the race) ----
    std::atomic<bool> sawJoin{ false };
    hostSig.setClientJoinedHandler([&](const ClientJoined& cj) {
        if (cj.playerIndex == 0 && cj.name == "Mahdyar") sawJoin.store(true);
    });

    // ---- client joins (bad code first) ----
    CHECK(clientSig.connect("127.0.0.1", port, 3000, &err));
    CHECK(!clientSig.join("XXXXXXXX", "Mahdyar").has_value());
    const auto joined = clientSig.join(reg->code, "Mahdyar");
    CHECK(joined.has_value());
    CHECK_EQ(joined->playerIndex, 0);
    CHECK(joined->hostPublicAddr == "127.0.0.1");

    // ---- host got the notification ----
    for (int i = 0; i < 50 && !sawJoin.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(sawJoin.load());

    // ---- relay TCP pair with RemotePlay frame protocol over it ----
    asio::io_context hostIo, clientIo;
    auto hostWork = asio::make_work_guard(hostIo);
    auto clientWork = asio::make_work_guard(clientIo);
    std::thread hostThread([&] { hostIo.run(); });
    std::thread clientThread([&] { clientIo.run(); });

    std::promise<TcpConnection::Ptr> hostConnP, clientConnP;
    auto hostConnF = hostConnP.get_future();
    auto clientConnF = clientConnP.get_future();

    TcpClient hostRelay(hostIo);
    TcpClient clientRelay(clientIo);
    hostRelay.connectRelay("127.0.0.1", static_cast<uint16_t>(reg->relayTcpPort),
                           joined->token, "host", 5000,
                           [&](std::error_code ec, TcpConnection::Ptr c) {
                               if (!ec) hostConnP.set_value(std::move(c));
                               else hostConnP.set_exception(std::make_exception_ptr(std::runtime_error(ec.message())));
                           });
    clientRelay.connectRelay("127.0.0.1", static_cast<uint16_t>(reg->relayTcpPort),
                             joined->token, "client", 5000,
                             [&](std::error_code ec, TcpConnection::Ptr c) {
                                 if (!ec) clientConnP.set_value(std::move(c));
                                 else clientConnP.set_exception(std::make_exception_ptr(std::runtime_error(ec.message())));
                             });

    TcpConnection::Ptr hostConn = hostConnF.get();
    TcpConnection::Ptr clientConn = clientConnF.get();
    CHECK(hostConn && clientConn);
    CHECK(hostConn->isOpen() && clientConn->isOpen());

    // opaque RemotePlay frames both ways
    std::atomic<int> hostGot{ 0 }, clientGot{ 0 };
    hostConn->setHandlers([&](uint16_t, uint8_t, const uint8_t*, uint32_t) { hostGot.fetch_add(1); },
                          [](const std::error_code&) {});
    clientConn->setHandlers([&](uint16_t, uint8_t, const uint8_t*, uint32_t) { clientGot.fetch_add(1); },
                            [](const std::error_code&) {});
    hostConn->start();
    clientConn->start();

    const std::vector<uint8_t> payload{ 0x01, 0x02, 0x03, 0x04, 0x05 };
    for (int i = 0; i < 5; ++i) {
        hostConn->send(0x0001, payload);
        clientConn->send(0x0002, payload);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(clientGot.load() >= 4);
    CHECK(hostGot.load() >= 4);

    hostConn->close();
    clientConn->close();
    hostWork.reset();
    clientWork.reset();
    hostThread.join();
    clientThread.join();

    // ---- UDP media relay: bind + encrypted-style media forwarding ----
    UdpTransport hostUdp, clientUdp;
    CHECK(hostUdp.bind("127.0.0.1", 0, &err));
    CHECK(clientUdp.bind("127.0.0.1", 0, &err));
    hostUdp.setSessionId(99);
    clientUdp.setSessionId(99);
    CHECK(hostUdp.setPeer("127.0.0.1", static_cast<uint16_t>(reg->relayUdpPort), &err));
    CHECK(clientUdp.setPeer("127.0.0.1", static_cast<uint16_t>(reg->relayUdpPort), &err));

    hostUdp.sendRelayBind(reg->hostToken);
    clientUdp.sendRelayBind(joined->token);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));   // let binds register

    std::atomic<int> videoFrames{ 0 };
    std::atomic<bool> stop{ false };
    std::thread consumer([&] {
        std::vector<AssembledFrame> frames;
        while (!stop.load()) {
            clientUdp.pollVideo(steadyNowNs(), frames);
            videoFrames.fetch_add(static_cast<int>(frames.size()));
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    std::vector<uint8_t> media(2048, 0xCD);
    for (int i = 0; i < 10; ++i) {
        hostUdp.sendFrame(UdpType::Video, i == 0, media.data(), media.size(), steadyNowNs());
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    stop.store(true);
    consumer.join();
    CHECK(videoFrames.load() >= 8);   // host->client via relay

    hostUdp.stop();
    clientUdp.stop();
    clientSig.disconnect();
    hostSig.disconnect();
}

RP_TEST_MAIN("signaling")
