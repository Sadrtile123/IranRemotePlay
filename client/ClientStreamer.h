#pragma once
// Phase 4/5 — client streaming engine.
//
// Owns the client media pipeline:
//   receive (UdpTransport thread) -> jitter buffer -> decode thread -> present callback
// Starts RTT probes, requests a keyframe on start and after stalls, and feeds
// the UI both video frames and live statistics.

#include "../audio/WasapiPlayer.h"
#include "../decoding/VideoDecoder.h"
#include "../encoding/AudioCodec.h"
#include "../networking/UdpTransport.h"

#include "../common/Types.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace rp {

using common::VideoCodec;

struct ClientStreamStats {
    net::UdpStatsSnapshot udp;          // rtt, loss, jitter, bitrate in
    double decodeMs = 0.0;
    double audioDecodeMs = 0.0;
    double audioBufferedMs = 0.0;
    uint64_t audioUnderruns = 0;
    double fps = 0.0;                   // decoded fps
    uint64_t framesDecoded = 0;
    int width = 0, height = 0;
    std::string decoderName;
    bool audioActive = false;
};

class ClientStreamer {
public:
    using PresentCallback = std::function<void(const DecodedFrame&)>;
    using ErrorCallback = std::function<void(const std::string&)>;

    ClientStreamer();
    ~ClientStreamer();

    ClientStreamer(const ClientStreamer&) = delete;
    ClientStreamer& operator=(const ClientStreamer&) = delete;

    // `hostAddress` is the TCP peer address (LAN mode); the UDP port comes
    // from the TCP STREAM_START message.
    bool start(const std::string& hostAddress, uint16_t udpPort, uint32_t sessionId,
               VideoCodec codec, std::shared_ptr<net::UdpCryptoSink> crypto,
               PresentCallback present, ErrorCallback onError,
               std::string* err = nullptr);

    void stop();

    // Host's UDP endpoint moved (hole punch / relay switch).
    void setHostEndpoint(const std::string& address, uint16_t port);

    [[nodiscard]] ClientStreamStats stats() const;
    [[nodiscard]] bool running() const { return running_.load(); }
    [[nodiscard]] net::UdpTransport& udpTransport() { return *transport_; }

    void setVideoJitterTargetMs(unsigned ms) { if (transport_) transport_->setVideoJitterTargetMs(ms); }

private:
    void decodeLoop();

    std::string hostAddress_;
    uint16_t udpPort_ = 0;
    uint32_t sessionId_ = 0;
    VideoCodec codec_ = VideoCodec::H264;

    std::unique_ptr<VideoDecoder> decoder_;
    std::unique_ptr<AudioDecoderOpus> audioDecoder_;
    std::unique_ptr<audio::WasapiPlayer> audioPlayer_;
    std::unique_ptr<net::UdpTransport> transport_;            
    std::atomic<bool> audioActive_{ false };

    std::thread decodeThread_;
    std::atomic<bool> running_{ false };

    PresentCallback presentCb_;
    ErrorCallback errorCb_;

    // decode fps measurement
    mutable std::mutex statsMutex_;
    uint64_t statFramesWindow_ = 0;
    uint64_t statWindowStartMs_ = 0;
    double measuredFps_ = 0.0;
};

} // namespace rp
