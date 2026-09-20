#pragma once
// Phase 4 — UDP media transport over standalone Asio.
//
// One UdpTransport = one bound socket = one peer's media session.
// The host creates one transport per client (ports base+playerIndex); the
// client creates a single transport targeting the host.
//
// Media frames are fragmented per UdpProtocol.h; assembled frames go through
// per-type jitter buffers. INPUT frames bypass the jitter buffer (latency
// first). PING/PONG run automatically for RTT measurement. The receive path
// and the send path both execute on one internal io_context thread, so all
// public methods are thread-safe.

#include <utility>
#include "UdpCrypto.h"
#include "UdpProtocol.h"

#include <asio.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace rp::net {

struct UdpStatsSnapshot {
    // send
    uint64_t sentDatagrams = 0, sentBytes = 0, sendErrors = 0;
    uint64_t sentFrames = 0;
    // receive
    uint64_t recvDatagrams = 0, recvBytes = 0, malformed = 0, wrongSession = 0;
    uint64_t assembledFrames = 0, droppedIncomplete = 0;
    // quality
    uint64_t lostFrames = 0, duplicates = 0, lateFrames = 0, reorderedFrames = 0;
    double   lastRttMs = 0.0, avgRttMs = 0.0;
    double   jitterMs = 0.0;
    unsigned jitterTargetMs = 0;
    size_t   jitterQueue = 0;
};

class UdpTransport {
public:
    using InputCallback = std::function<void(const AssembledFrame&)>;
    using ControlCallback = std::function<void(UdpType type, const std::vector<uint8_t>& payload)>;
    using LossCallback = std::function<void(uint64_t lostFrames)>;   // cumulative video loss changed

    UdpTransport();
    ~UdpTransport();

    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;

    // Binds and starts the receive thread. port 0 -> ephemeral (see localPort()).
    bool bind(const std::string& localAddress, uint16_t port, std::string* err = nullptr);
    void stop();

    [[nodiscard]] uint16_t localPort() const { return localPort_.load(std::memory_order_relaxed); }

    void setSessionId(uint32_t id) { sessionId_ = id; }

    // Where to send. May be called again (relay switch / hole-punch result).
    bool setPeer(const std::string& host, uint16_t port, std::string* err = nullptr);
    bool setPeer(asio::ip::udp::endpoint ep);

    // Drop datagrams from other sources once the real peer is known (set after
    // hole punch completes or on first valid packet from the expected host).
    void lockToRemote(const asio::ip::udp::endpoint& ep);

    // ---- media path ----
    // Video/audio frame (large; fragmented). Timestamp in ns for A/V sync.
    void sendFrame(UdpType type, bool keyframe, const void* data, size_t size, uint64_t timestampNs);

    // Small unfragmented datagram: input state, control, keyframe request.
    void sendSmall(UdpType type, const void* data, size_t size);

    void requestKeyframe() {
        static const uint8_t zero = 0;
        sendSmall(UdpType::KeyframeRequest, &zero, 1);
    }

    void sendStreamStart() { static const uint8_t zero = 0; sendSmall(UdpType::StreamStart, &zero, 1); }
    void sendStreamStop()  { static const uint8_t zero = 0; sendSmall(UdpType::StreamStop, &zero, 1); }

    // Phase 14 — registers this socket's public endpoint with the session's
    // UDP relay (RPBIND + 32-byte hex token) so the relay forwards the peer's
    // encrypted datagrams to us. Call after setPeer(relayEndpoint).
    void sendRelayBind(const std::string& token32hex);

    // Phase 14 — fallback endpoint (the relay). If the peer switched to a
    // direct path and it goes silent for >5 s, the transport returns to this
    // endpoint automatically.
    void setRelayFallback(asio::ip::udp::endpoint ep);

    // Hole-punch keepalive (tiny packet until the remote answers).
    void sendPunch(const asio::ip::udp::endpoint& to);

    // ---- receive configuration ----
    void setInputCallback(InputCallback cb) { inputCb_ = std::move(cb); }
    void setControlCallback(ControlCallback cb) { controlCb_ = std::move(cb); }
    void setVideoJitterTargetMs(unsigned ms) { videoJitterTargetMs_.store(ms); }
    void setAudioJitterTargetMs(unsigned ms) { audioJitterTargetMs_.store(ms); }

    // Pops ready video frames (call from the decode thread ~ every frame).
    void pollVideo(uint64_t nowNs, std::vector<AssembledFrame>& out);
    // Pops ready audio frames (call from the audio playback loop).
    void pollAudio(uint64_t nowNs, std::vector<AssembledFrame>& out);

    // RTT probes.
    void startPings(unsigned intervalMs);
    void stopPings();

    // Receiver-driven loss recovery: when a video sequence gap is detected,
    // automatically ask the host for a keyframe (rate-limited).
    void setAutoKeyframeRequest(bool enabled) { autoKeyframeRequest_ = enabled; }

    [[nodiscard]] UdpStatsSnapshot stats() const;
    void resetStats();

    [[nodiscard]] uint64_t authDrops() const { return authDrops_.load(); }

    // If true, packets from a non-locked remote are dropped (security default on).
    void setDropUnknownRemote(bool drop) { dropUnknownRemote_ = drop; }

    // Phase 12 — enables AES-256-GCM on every datagram payload (header stays
    // clear for relay routing; it is bound as AAD). Once the sink reports
    // active(), plaintext datagrams are rejected (fail closed).
    void setCryptoSink(std::shared_ptr<UdpCryptoSink> sink) { secure_ = std::move(sink); }
    [[nodiscard]] bool secure() const { return secure_ && secure_->active(); }

private:
    void runIo();
    void doReceive();
    void handleDatagram(const uint8_t* buf, size_t size, const asio::ip::udp::endpoint& from);
    void handleMediaComplete(AssembledFrame&& frame);
    void postDatagram(std::vector<uint8_t>& dg);
    void sendPingOnce();

    std::unique_ptr<asio::io_context> io_;
    std::unique_ptr<asio::ip::udp::socket> socket_;
    std::unique_ptr<asio::steady_timer> pingTimer_;
    std::thread ioThread_;
    std::atomic<std::thread::id> ioThreadId_{};      // set by the io thread itself
    asio::ip::udp::endpoint senderEndpoint_;
    asio::ip::udp::endpoint peer_;
    std::mutex peerMutex_;
    std::atomic<bool> havePeer_{ false };
    std::atomic<bool> running_{ false };
    std::atomic<uint16_t> localPort_{ 0 };
    uint32_t sessionId_ = 0;

    mutable std::mutex mediaMutex_;                 // guards reassemblers + jitter buffers (mutable for stats())
    FragmentReassembler videoReassembly_{ UdpType::Video };
    FragmentReassembler audioReassembly_{ UdpType::Audio };
    FragmentReassembler inputReassembly_{ UdpType::Input };
    SequenceTracker videoSeq_;
    SequenceTracker audioSeq_;
    JitterBuffer videoJitter_{ 30 };
    JitterBuffer audioJitter_{ 30 };
    std::array<std::atomic<uint64_t>, 9> seqCounter_{};   // per-type send sequence

    std::array<uint8_t, 2048> recvBuf_{};
    std::atomic<uint32_t> pingSeq_{ 0 };
    std::atomic<uint64_t> lastPongNs_{ 0 };
    std::atomic<unsigned> pingIntervalMs_{ 0 };
    std::atomic<double> lastRttMs_{ 0.0 };
    std::atomic<double> avgRttMs_{ 0.0 };
    std::atomic<double> jitterMs_{ 0.0 };
    uint64_t lastArrivalNs_ = 0;

    // interarrival jitter (RFC3550-style), guarded by mediaMutex_
    double jitterEWMA_ = 0.0;

    std::atomic<uint64_t> sentDatagrams_{ 0 }, sentBytes_{ 0 }, sendErrors_{ 0 }, sentFrames_{ 0 };
    std::atomic<uint64_t> recvDatagrams_{ 0 }, recvBytes_{ 0 }, malformed_{ 0 }, wrongSession_{ 0 };

    InputCallback inputCb_;
    ControlCallback controlCb_;
    std::shared_ptr<UdpCryptoSink> secure_;
    std::atomic<uint64_t> authDrops_{ 0 };
    std::atomic<bool> dropUnknownRemote_{ true };
    std::atomic<bool> autoKeyframeRequest_{ true };
    uint64_t lastKeyframeReqNs_ = 0;                    // guarded by mediaMutex_
    std::atomic<unsigned> videoJitterTargetMs_{ 30 };
    std::atomic<unsigned> audioJitterTargetMs_{ 30 };
    std::optional<asio::ip::udp::endpoint> lockedRemote_;   // guarded by peerMutex_
    std::optional<asio::ip::udp::endpoint> relayFallback_;  // guarded by peerMutex_
    std::atomic<uint64_t> lastRxAbsNs_{ 0 };
};

uint64_t steadyNowNs();

} // namespace rp::net
