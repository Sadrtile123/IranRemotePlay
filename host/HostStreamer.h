#pragma once
// Phase 4/5 — host streaming engine.
//
// Owns the per-client media pipeline:
//   capture thread: DisplayCapture -> VideoEncoder -> UdpTransport (fragmented)
// The control channel (TCP, HostApp) tells the streamer when to start/stop
// per client and delivers keyframe requests arriving over UDP or TCP.
//
// Windows-only (capture + encoder). One HostStreamer instance per client.

#include <utility>
#include <vector>
#include "../audio/WasapiCapture.h"
#include "../capture/ScreenCapture.h"
#include "../encoding/AudioCodec.h"
#include "../encoding/VideoEncoder.h"
#include "../networking/UdpTransport.h"

#include "../common/Types.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace rp {

using common::VideoCodec;

struct StreamConfig {
    VideoCodec codec = VideoCodec::H264;
    int width = 1920;
    int height = 1080;
    int fps = 60;
    int bitrateKbps = 8000;
    int gop = 120;
    int audioBitrateKbps = 128;      // 64..192 (Opus)
    bool audioEnabled = true;
    common::CaptureMode captureMode = common::CaptureMode::Monitor;
    int outputIndex = 0;               // monitor duplication index
    void* windowHwnd = nullptr;        // window capture target (HWND)
    std::string gameName;
};

struct HostStreamStats {
    // pipeline
    double captureMs = 0.0;
    double encodeMs = 0.0;
    double audioEncodeMs = 0.0;
    double fps = 0.0;                  // achieved encode fps
    double actualBitrateKbps = 0.0;    // measured send rate
    // transport (from UdpTransport)
    net::UdpStatsSnapshot udp;
    // encoder
    std::string encoderName;
    uint64_t framesEncoded = 0;
};

class HostStreamer {
public:
    using ErrorCallback = std::function<void(const std::string& message)>;   // fatal pipeline errors

    HostStreamer();
    ~HostStreamer();

    HostStreamer(const HostStreamer&) = delete;
    HostStreamer& operator=(const HostStreamer&) = delete;

    // Binds a UDP socket for this client, starts capture+encode, notifies
    // `onStreamStart` with (udpPort, sessionId) so the TCP layer can send
    // STREAM_START to the client. `crypto` (optional) is installed BEFORE the
    // socket binds so no unencrypted window exists once keys are established.
    bool start(const StreamConfig& cfg, uint32_t sessionId, uint8_t playerIndex,
               std::shared_ptr<net::UdpCryptoSink> crypto,
               const std::function<void(uint16_t udpPort, uint32_t sessionId)>& onStreamStart,
               std::string* err = nullptr);

    void stop(const std::string& reason);

    // Client's UDP endpoint became known (e.g. it punched through / first packet).
    void setClientEndpoint(const std::string& address, uint16_t port);

    // Keyframe request from UDP control path or TCP.
    void requestKeyframe();

    // Adaptive bitrate (Phase 15) and recovery (Phase 16) entry points.
    void setBitrate(int kbps);
    // Asks the capture loop to re-initialize the encoder at the next frame
    // boundary. The swap happens ON the capture thread (the only user of the
    // encoder), which removes the restart-vs-encode data race that could
    // destroy the encoder mid-encode. Fire-and-forget: failures surface via
    // the error callback.
    void requestEncoderRestart();
    // Force software encoder (recovery dialog: "Switch to software").
    void requestSoftwareEncoder();

    // Input path: forwards raw Input datagrams to the handler (HostApp routes
    // them through the injector with permission checks).
    void setInputHandler(std::function<void(const net::AssembledFrame&)> handler);

    // Rumble backchannel: injector (game vibration) -> client's pad.
    void sendRumble(uint8_t playerIndex, uint8_t leftMotor, uint8_t rightMotor);

    [[nodiscard]] HostStreamStats stats() const;
    [[nodiscard]] bool running() const { return running_.load(); }
    [[nodiscard]] uint16_t udpPort() const { return transport_ ? transport_->localPort() : 0; }
    [[nodiscard]] net::UdpTransport& udpTransport() { return *transport_; }
    void setErrorCallback(ErrorCallback cb) { errorCb_ = std::move(cb); }

private:
    void captureLoop();
    void handleUdpControl(net::UdpType type, const std::vector<uint8_t>& payload);
    // Runs on the capture thread: performs a pending encoder swap.
    bool performEncoderRestart(std::string* err);
    // Thread-safe snapshots (leaf lock; NEVER held while joining threads).
    // encoder_ is touched by the capture loop, the Qt stats timer, the UDP io
    // thread (keyframe requests) and ViGEm (rumble) - a plain unique_ptr
    // reset during stop() could destroy it under a caller. Shared ownership
    // plus a snapshot makes each accessor safe.
    [[nodiscard]] std::shared_ptr<VideoEncoder> currentEncoder() const;
    void setEncoder(std::shared_ptr<VideoEncoder> e);
    [[nodiscard]] std::shared_ptr<net::UdpTransport> currentTransport() const;
    void setTransport(std::shared_ptr<net::UdpTransport> t);

    StreamConfig cfg_{};
    uint32_t sessionId_ = 0;
    uint8_t  playerIndex_ = 0;

    std::unique_ptr<DisplayCapture> capture_;
    std::shared_ptr<VideoEncoder> encoder_;          // guarded by memberMutex_
    std::shared_ptr<net::UdpTransport> transport_;   // guarded by memberMutex_
    std::unique_ptr<audio::WasapiCapture> audioCapture_;
    std::unique_ptr<AudioEncoderOpus> audioEncoder_;
    bool audioThreadError_ = false;
    mutable std::mutex memberMutex_;                 // leaf lock for the shared_ptrs above

    std::thread captureThread_;
    std::atomic<bool> running_{ false };

    // Serializes start()/stop()/stats() against each other (start/stop mutate
    // the member pointers; stats() reads them from the UI thread).
    mutable std::mutex stopMutex_;
    std::atomic<bool> restartRequested_{ false };   // capture loop swaps the encoder

    // stats
    mutable std::mutex statsMutex_;
    uint64_t statFramesWindow_ = 0;
    uint64_t statBytesWindow_ = 0;
    double statWindowStartMs_ = 0.0;
    double measuredFps_ = 0.0;
    double measuredKbps_ = 0.0;

    ErrorCallback errorCb_;
    std::string forcedEncoder_;    // non-empty: exact encoder to use (software recovery)
};

} // namespace rp
