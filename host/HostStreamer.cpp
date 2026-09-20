// Phase 4/5 — host streaming engine implementation. See HostStreamer.h.

#include "HostStreamer.h"

#include "../common/Config.h"
#include "../common/Log.h"
#include "../input/InputProtocol.h"

#include <chrono>
#include <cmath>

namespace rp {
namespace {
double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}
} // namespace

HostStreamer::HostStreamer() = default;

HostStreamer::~HostStreamer() { stop("destroyed"); }

bool HostStreamer::start(const StreamConfig& cfg, uint32_t sessionId, uint8_t playerIndex,
                         std::shared_ptr<net::UdpCryptoSink> crypto,
                         const std::function<void(uint16_t, uint32_t)>& onStreamStart, std::string* err) {
    if (running_.exchange(true)) { if (err) *err = "already running"; running_.store(false); return false; }
    cfg_ = cfg;
    sessionId_ = sessionId;
    playerIndex_ = playerIndex;

    // ---- UDP transport (one socket per client) ----
    transport_ = std::make_unique<net::UdpTransport>();
    transport_->setSessionId(sessionId_);
    if (crypto) transport_->setCryptoSink(std::move(crypto));   // BEFORE bind: no plaintext window
    transport_->setControlCallback([this](net::UdpType t, const std::vector<uint8_t>& p) {
        handleUdpControl(t, p);
    });
    // Host accepts the client's source endpoint once known; before that we
    // learn it from the client's first datagram.
    if (!transport_->bind("0.0.0.0", 0, err)) {         // ephemeral port, given to the client via TCP
        transport_.reset();
        running_.store(false);
        return false;
    }

    // ---- capture ----
    capture_ = std::make_unique<DisplayCapture>();
    if (!capture_->init(cfg.outputIndex, err)) {
        transport_->stop();
        transport_.reset();
        capture_.reset();
        running_.store(false);
        return false;
    }

    // ---- encoder ----
    VideoEncoderParams ep;
    ep.codec = cfg.codec;
    ep.width = cfg.width;
    ep.height = cfg.height;
    ep.fps = cfg.fps;
    ep.bitrateKbps = cfg.bitrateKbps;
    ep.gop = cfg.gop > 0 ? cfg.gop : cfg.fps * 2;
    if (!forcedEncoder_.empty()) ep.preferredEncoder = forcedEncoder_;
    encoder_ = std::make_unique<VideoEncoder>();
    if (!encoder_->init(ep, err)) {
        transport_->stop();
        transport_.reset();
        capture_.reset();
        encoder_.reset();
        running_.store(false);
        return false;
    }

    RP_INFO() << "[streamer] player " << (playerIndex_ + 1) << " udp=" << transport_->localPort()
              << " encoder=" << encoder_->encoderName()
              << " " << cfg_.width << "x" << cfg_.height << "@" << cfg_.fps;

    {
        std::lock_guard<std::mutex> lk(statsMutex_);
        statFramesWindow_ = statBytesWindow_ = 0;
        statWindowStartMs_ = nowMs();
        measuredFps_ = measuredKbps_ = 0.0;
    }

    if (onStreamStart) onStreamStart(transport_->localPort(), sessionId_);

    transport_->sendStreamStart();       // UDP-side signal (TCP carries the authoritative copy)

    // ---- audio (loopback capture -> Opus -> UDP) ----
    if (cfg_.audioEnabled) {
        audioCapture_ = std::make_unique<audio::WasapiCapture>();
        audioEncoder_ = std::make_unique<AudioEncoderOpus>();
        std::string aerr;
        if (audioEncoder_->init(cfg_.audioBitrateKbps, &aerr)) {
            const uint32_t sid = sessionId_;
            net::UdpTransport* tp = transport_.get();
            AudioEncoderOpus* enc = audioEncoder_.get();
            if (!audioCapture_->start([tp, sid, enc](const int16_t* samples, size_t frames, uint64_t tsUs) {
                    std::vector<AudioPacket> pkts;
                    std::string e;
                    if (enc->encode(samples, frames, tsUs, pkts, &e)) {
                        for (const AudioPacket& p : pkts) {
                            tp->sendFrame(net::UdpType::Audio, false, p.data.data(), p.data.size(),
                                          p.ptsUs * 1000ull);   // us -> ns
                        }
                    }
                }, &aerr)) {
                RP_WARN() << "[streamer] audio disabled: " << aerr;
                audioCapture_.reset();
                audioEncoder_.reset();
            }
        } else {
            RP_WARN() << "[streamer] opus encoder unavailable, audio disabled: " << aerr;
            audioEncoder_.reset();
        }
    }

    captureThread_ = std::thread([this] { captureLoop(); });
    return true;
}

void HostStreamer::stop(const std::string& reason) {
    if (!running_.exchange(false)) return;
    RP_INFO() << "[streamer] stopping (" << reason << ")";
    if (transport_) transport_->sendStreamStop();
    if (captureThread_.joinable()) captureThread_.join();
    if (audioCapture_) audioCapture_->stop();
    if (audioEncoder_) {
        std::vector<AudioPacket> rest;
        audioEncoder_->encode(nullptr, 0, 0, rest, nullptr);
        audioEncoder_.reset();
    }
    audioCapture_.reset();
    if (encoder_) {
        std::vector<EncodedPacket> rest;
        encoder_->flush(rest);
        encoder_.reset();
    }
    if (transport_) { transport_->stop(); transport_.reset(); }
    capture_.reset();
}

void HostStreamer::captureLoop() {
    const int timeoutMs = std::clamp(1000 / std::max(1, cfg_.fps) * 2, 8, 100);
    rp::CaptureFrame frame;
    std::vector<EncodedPacket> packets;

    while (running_.load()) {
        bool timedOut = false;
        std::string err;
        if (cfg_.captureMode == common::CaptureMode::Window && cfg_.windowHwnd) {
            // Refresh crop every frame (window may move/resize).
            if (!capture_->updateWindowCrop(cfg_.windowHwnd)) {
                // Window gone: fall back to full monitor and report once.
                RP_WARN() << "[streamer] capture window vanished, falling back to monitor";
                cfg_.captureMode = common::CaptureMode::Monitor;
                capture_->clearCrop();
            }
        }
        if (!capture_->capture(frame, timeoutMs, &timedOut, &err)) {
            if (timedOut) continue;                    // no new frame yet
            RP_ERROR() << "[streamer] capture failed: " << err;
            if (errorCb_) errorCb_("Capture failed: " + err);
            // Attempt full re-init once per second.
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            capture_->init(cfg_.outputIndex, nullptr);
            continue;
        }
        if (!encoder_) break;

        if (!encoder_->encode(frame.data, frame.width, frame.height, frame.stride, packets, &err)) {
            RP_ERROR() << "[streamer] encode failed: " << err;
            if (errorCb_) errorCb_("Encoder failed: " + err);
            // Crash-recovery: try a full encoder restart, then software.
            std::string err2;
            if (!restartEncoder(&err2)) {
                forcedEncoder_ = "libx264";
                if (!restartEncoder(&err2)) {
                    running_.store(false);
                    if (errorCb_) errorCb_("Encoder recovery failed: " + err2);
                    break;
                }
            }
            continue;
        }

        for (const EncodedPacket& p : packets) {
            transport_->sendFrame(net::UdpType::Video, p.keyframe, p.data.data(), p.data.size(),
                                  frame.timestampUs * 1000ull);   // us -> ns
        }

        // Rolling measurement window (1 s).
        {
            std::lock_guard<std::mutex> lk(statsMutex_);
            statFramesWindow_ += packets.size();
            for (const auto& p : packets) statBytesWindow_ += p.data.size();
            const double windowMs = nowMs() - statWindowStartMs_;
            if (windowMs >= 1000.0) {
                measuredFps_ = static_cast<double>(statFramesWindow_) * 1000.0 / windowMs;
                measuredKbps_ = static_cast<double>(statBytesWindow_) * 8.0 / 1000.0 / (windowMs / 1000.0);
                statFramesWindow_ = statBytesWindow_ = 0;
                statWindowStartMs_ = nowMs();
            }
        }

        // Apply a pending bitrate change at a safe point (rare due to hysteresis).
        if (encoder_->bitrateDirty()) encoder_->applyBitrateIfDirty();
    }
}

void HostStreamer::handleUdpControl(net::UdpType type, const std::vector<uint8_t>& payload) {
    (void)payload;
    switch (type) {
        case net::UdpType::KeyframeRequest:
            requestKeyframe();
            break;
        case net::UdpType::StreamStart:
            // Client confirmed media channel; nothing to do (transport learns endpoint).
            break;
        case net::UdpType::StreamStop:
            RP_INFO() << "[streamer] client requested stream stop via UDP";
            stop("client stream stop");
            break;
        default:
            break;
    }
}

void HostStreamer::setClientEndpoint(const std::string& address, uint16_t port) {
    if (!transport_) return;
    std::string err;
    if (transport_->setPeer(address, port, &err)) {
        RP_INFO() << "[streamer] client endpoint " << address << ":" << port;
    } else {
        RP_WARN() << "[streamer] setClientEndpoint failed: " << err;
    }
}

void HostStreamer::setInputHandler(std::function<void(const net::AssembledFrame&)> handler) {
    if (transport_) transport_->setInputCallback(std::move(handler));
}

void HostStreamer::sendRumble(uint8_t playerIndex, uint8_t leftMotor, uint8_t rightMotor) {
    if (!transport_) return;
    rp::input::RumbleCommand rc;
    rc.playerIndex = playerIndex;
    rc.leftMotor = leftMotor;
    rc.rightMotor = rightMotor;
    transport_->sendSmall(net::UdpType::Control, &rc, sizeof(rc));
}

void HostStreamer::requestKeyframe() {
    if (encoder_) encoder_->requestKeyframe();
}

void HostStreamer::setBitrate(int kbps) {
    if (encoder_) encoder_->setBitrate(kbps);
    cfg_.bitrateKbps = kbps;
}

bool HostStreamer::restartEncoder(std::string* err) {
    if (!running_.load() || !capture_) { if (err) *err = "not running"; return false; }
    VideoEncoderParams ep;
    ep.codec = cfg_.codec;
    ep.width = cfg_.width;
    ep.height = cfg_.height;
    ep.fps = cfg_.fps;
    ep.bitrateKbps = encoder_ ? encoder_->bitrateKbps() : cfg_.bitrateKbps;
    ep.gop = cfg_.gop > 0 ? cfg_.gop : cfg_.fps * 2;
    if (!forcedEncoder_.empty()) ep.preferredEncoder = forcedEncoder_;
    auto enc = std::make_unique<VideoEncoder>();
    if (!enc->init(ep, err)) return false;
    encoder_ = std::move(enc);           // swap on success only
    RP_INFO() << "[streamer] encoder restarted: " << encoder_->encoderName();
    return true;
}

bool HostStreamer::switchToSoftwareEncoder(std::string* err) {
    if (cfg_.codec == VideoCodec::Hevc) forcedEncoder_ = "libx265";
    else if (cfg_.codec == VideoCodec::Av1) forcedEncoder_ = "libsvtav1";
    else forcedEncoder_ = "libx264";
    return restartEncoder(err);
}

HostStreamStats HostStreamer::stats() const {
    HostStreamStats s;
    if (encoder_) {
        s.encodeMs = encoder_->averageEncodeMs();
        s.encoderName = encoder_->encoderName();
        s.framesEncoded = encoder_->framesEncoded();
    }
    if (audioEncoder_) s.audioEncodeMs = audioEncoder_->averageEncodeMs();
    if (capture_) s.captureMs = capture_->averageCaptureMs();
    if (transport_) s.udp = transport_->stats();
    {
        std::lock_guard<std::mutex> lk(statsMutex_);
        s.fps = measuredFps_;
        s.actualBitrateKbps = measuredKbps_;
    }
    return s;
}

} // namespace rp
