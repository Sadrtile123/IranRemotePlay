// Phase 4/5 — host streaming engine implementation. See HostStreamer.h.
//
// v0.1.1 hardening:
//  * start()/stop() serialized by stopMutex_ (stop could previously race the
//    coordinator's init thread and destroy members mid-start).
//  * stop() is safe when invoked from the UDP io thread (StreamStop control
//    callback): UdpTransport::stop() no longer self-joins.
//  * Encoder restarts are performed ON the capture thread (request flags)
//    instead of swapping the encoder under the encode call.
//  * encoder_/transport_ are shared_ptr snapshots: keyframe requests (UDP io
//    thread), stats (Qt timer) and rumble (ViGEm thread) can never touch a
//    destroyed object during teardown, and no accessor can deadlock against
//    stop()'s joins.
//  * Capture failures retry a bounded number of times, then stop the stream
//    with a clear error instead of looping forever + spamming dialogs.
//  * COM (MTA) initialized on threads that touch DXGI.

#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "HostStreamer.h"

#include "../common/Config.h"
#include "../common/Log.h"
#include "../input/InputProtocol.h"

#include <objbase.h>

#include <chrono>
#include <cmath>

namespace rp {
namespace {
double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}
constexpr int kMaxConsecutiveCaptureFailures = 10;   // ~5 s of re-init attempts
} // namespace

HostStreamer::HostStreamer() = default;

HostStreamer::~HostStreamer() { stop("destroyed"); }

std::shared_ptr<VideoEncoder> HostStreamer::currentEncoder() const {
    std::lock_guard<std::mutex> lk(memberMutex_);
    return encoder_;
}
void HostStreamer::setEncoder(std::shared_ptr<VideoEncoder> e) {
    std::lock_guard<std::mutex> lk(memberMutex_);
    encoder_ = std::move(e);
}
std::shared_ptr<net::UdpTransport> HostStreamer::currentTransport() const {
    std::lock_guard<std::mutex> lk(memberMutex_);
    return transport_;
}
void HostStreamer::setTransport(std::shared_ptr<net::UdpTransport> t) {
    std::lock_guard<std::mutex> lk(memberMutex_);
    transport_ = std::move(t);
}

bool HostStreamer::start(const StreamConfig& cfg, uint32_t sessionId, uint8_t playerIndex,
                         std::shared_ptr<net::UdpCryptoSink> crypto,
                         const std::function<void(uint16_t, uint32_t)>& onStreamStart, std::string* err) {
    std::lock_guard<std::mutex> lk(stopMutex_);
    if (running_.exchange(true)) { if (err) *err = "already running"; running_.store(false); return false; }
    cfg_ = cfg;
    sessionId_ = sessionId;
    playerIndex_ = playerIndex;

    // Some DXGI/WASAPI entry points require COM on the calling thread.
    HRESULT cohr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coInit = SUCCEEDED(cohr);

    auto fail = [&](const char* what) {
        if (err && err->empty()) *err = what;
        if (coInit) CoUninitialize();
        running_.store(false);
        return false;
    };

    // ---- UDP transport (one socket per client) ----
    auto transport = std::make_shared<net::UdpTransport>();
    transport->setSessionId(sessionId_);
    if (crypto) transport->setCryptoSink(std::move(crypto));   // BEFORE bind: no plaintext window
    transport->setControlCallback([this](net::UdpType t, const std::vector<uint8_t>& p) {
        handleUdpControl(t, p);
    });
    // Host accepts the client's source endpoint once known; before that we
    // learn it from the client's first datagram.
    if (!transport->bind("0.0.0.0", 0, err)) {                 // ephemeral port, given to the client via TCP
        return fail("could not bind the UDP media socket");
    }
    setTransport(transport);

    // ---- capture ----
    capture_ = std::make_unique<DisplayCapture>();
    if (!capture_->init(cfg.outputIndex, err)) {
        currentTransport()->stop();
        setTransport(nullptr);
        capture_.reset();
        return fail("screen capture initialization failed");
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
    auto encoder = std::make_shared<VideoEncoder>();
    if (!encoder->init(ep, err)) {
        currentTransport()->stop();
        setTransport(nullptr);
        capture_.reset();
        return fail("video encoder initialization failed");
    }
    setEncoder(encoder);

    RP_INFO() << "[streamer] player " << (playerIndex_ + 1) << " udp=" << transport->localPort()
              << " encoder=" << encoder->encoderName()
              << " " << cfg_.width << "x" << cfg_.height << "@" << cfg_.fps;

    {
        std::lock_guard<std::mutex> slk(statsMutex_);
        statFramesWindow_ = statBytesWindow_ = 0;
        statWindowStartMs_ = nowMs();
        measuredFps_ = measuredKbps_ = 0.0;
    }

    if (onStreamStart) onStreamStart(transport->localPort(), sessionId_);

    transport->sendStreamStart();       // UDP-side signal (TCP carries the authoritative copy)

    // ---- audio (loopback capture -> Opus -> UDP) ----
    if (cfg_.audioEnabled) {
        audioCapture_ = std::make_unique<audio::WasapiCapture>();
        audioEncoder_ = std::make_unique<AudioEncoderOpus>();
        std::string aerr;
        if (audioEncoder_->init(cfg_.audioBitrateKbps, &aerr)) {
            const uint32_t sid = sessionId_;
            auto tp = transport;                  // keeps the transport alive for the audio thread
            AudioEncoderOpus* aenc = audioEncoder_.get();
            if (!audioCapture_->start([tp, sid, aenc](const int16_t* samples, size_t frames, uint64_t tsUs) {
                    std::vector<AudioPacket> pkts;
                    std::string e;
                    if (aenc->encode(samples, frames, tsUs, pkts, &e)) {
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

    restartRequested_.store(false);
    captureThread_ = std::thread([this] { captureLoop(); });
    if (coInit) CoUninitialize();
    return true;
}

void HostStreamer::stop(const std::string& reason) {
    // May be called from: coordinator (Qt thread), the UDP io thread (client
    // StreamStop), or the destructor. running_ admits exactly one media-flow
    // stop, but the thread join below must ALSO run when the capture loop
    // terminated itself (terminal error) — otherwise a joinable std::thread
    // would be destroyed and terminate the process.
    const bool wasRunning = running_.exchange(false);
    std::lock_guard<std::mutex> lk(stopMutex_);
    if (wasRunning) {
        RP_INFO() << "[streamer] stopping (" << reason << ")";
        if (const auto tp = currentTransport()) tp->sendStreamStop();
    }
    if (captureThread_.joinable()) captureThread_.join();
    if (!wasRunning && !audioCapture_ && !audioEncoder_ && !capture_ && !encoder_ && !transport_) {
        return;   // fully torn down already
    }
    if (audioCapture_) audioCapture_->stop();
    if (audioEncoder_) {
        std::vector<AudioPacket> rest;
        audioEncoder_->encode(nullptr, 0, 0, rest, nullptr);
        audioEncoder_.reset();
    }
    audioCapture_.reset();
    if (const auto e = currentEncoder()) {
        std::vector<EncodedPacket> rest;
        e->flush(rest);
    }
    setEncoder(nullptr);
    if (const auto tp = currentTransport()) { tp->stop(); }
    setTransport(nullptr);
    capture_.reset();
}

void HostStreamer::captureLoop() {
    // COM for the DXGI re-init path on this thread.
    HRESULT cohr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coInit = SUCCEEDED(cohr);

    const int timeoutMs = std::clamp(1000 / std::max(1, cfg_.fps) * 2, 8, 100);
    rp::CaptureFrame frame;
    std::vector<EncodedPacket> packets;
    int consecutiveFailures = 0;

    while (running_.load()) {
        // Deferred encoder restart (requested by the UI thread): the swap
        // happens here where the encoder is actually used.
        if (restartRequested_.exchange(false)) {
            std::string rerr;
            if (!performEncoderRestart(&rerr)) {
                RP_ERROR() << "[streamer] encoder restart failed: " << rerr;
                if (errorCb_) errorCb_("Encoder restart failed: " + rerr);
                // Fall back to software once, then give up.
                forcedEncoder_ = cfg_.codec == VideoCodec::Hevc ? "libx265" : "libx264";
                if (!performEncoderRestart(&rerr)) {
                    running_.store(false);
                    if (errorCb_) errorCb_("Encoder recovery failed: " + rerr);
                    break;
                }
            }
        }

        const auto tp = currentTransport();
        auto enc = currentEncoder();
        if (!enc || !tp) break;

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
            ++consecutiveFailures;
            if (consecutiveFailures >= kMaxConsecutiveCaptureFailures) {
                running_.store(false);
                if (errorCb_) {
                    errorCb_("Screen capture failed repeatedly: " + err +
                             ". The stream has been stopped (another capture app or a secure desktop may be blocking it).");
                }
                break;
            }
            // Attempt full re-init (bounded by the failure cap above).
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            capture_->init(cfg_.outputIndex, nullptr);
            continue;
        }
        consecutiveFailures = 0;

        if (!enc->encode(frame.data, frame.width, frame.height, frame.stride, packets, &err)) {
            RP_ERROR() << "[streamer] encode failed: " << err;
            // Crash recovery path: swap the encoder on the next loop pass.
            restartRequested_.store(true);
            continue;
        }

        for (const EncodedPacket& p : packets) {
            tp->sendFrame(net::UdpType::Video, p.keyframe, p.data.data(), p.data.size(),
                          frame.timestampUs * 1000ull);   // us -> ns
        }

        // Rolling measurement window (1 s).
        {
            std::lock_guard<std::mutex> slk(statsMutex_);
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
        if (enc->bitrateDirty()) enc->applyBitrateIfDirty();
    }

    if (coInit) CoUninitialize();
}

bool HostStreamer::performEncoderRestart(std::string* err) {
    if (!running_.load() || !capture_) { if (err) *err = "not running"; return false; }
    VideoEncoderParams ep;
    ep.codec = cfg_.codec;
    ep.width = cfg_.width;
    ep.height = cfg_.height;
    ep.fps = cfg_.fps;
    ep.bitrateKbps = [&] {
        const auto cur = currentEncoder();
        return cur ? cur->bitrateKbps() : cfg_.bitrateKbps;
    }();
    ep.gop = cfg_.gop > 0 ? cfg_.gop : cfg_.fps * 2;
    if (!forcedEncoder_.empty()) ep.preferredEncoder = forcedEncoder_;
    auto enc = std::make_shared<VideoEncoder>();
    if (!enc->init(ep, err)) return false;
    RP_INFO() << "[streamer] encoder restarted: " << enc->encoderName();
    setEncoder(std::move(enc));            // old one dies when the loop's local ref drops
    return true;
}

void HostStreamer::handleUdpControl(net::UdpType type, const std::vector<uint8_t>& payload) {
    (void)payload;
    switch (type) {
        case net::UdpType::KeyframeRequest:
            requestKeyframe();               // lock-free snapshot; cannot deadlock with stop()
            break;
        case net::UdpType::StreamStart:
            // Client confirmed media channel; nothing to do (transport learns endpoint).
            break;
        case net::UdpType::StreamStop:
            RP_INFO() << "[streamer] client requested stream stop via UDP";
            stop("client stream stop");   // safe from the io thread since v0.1.1
            break;
        default:
            break;
    }
}

void HostStreamer::setClientEndpoint(const std::string& address, uint16_t port) {
    if (const auto tp = currentTransport()) {
        std::string err;
        if (tp->setPeer(address, port, &err)) {
            RP_INFO() << "[streamer] client endpoint " << address << ":" << port;
        } else {
            RP_WARN() << "[streamer] setClientEndpoint failed: " << err;
        }
    }
}

void HostStreamer::setInputHandler(std::function<void(const net::AssembledFrame&)> handler) {
    if (const auto tp = currentTransport()) tp->setInputCallback(std::move(handler));
}

void HostStreamer::sendRumble(uint8_t playerIndex, uint8_t leftMotor, uint8_t rightMotor) {
    if (const auto tp = currentTransport()) {
        rp::input::RumbleCommand rc;
        rc.playerIndex = playerIndex;
        rc.leftMotor = leftMotor;
        rc.rightMotor = rightMotor;
        tp->sendSmall(net::UdpType::Control, &rc, sizeof(rc));
    }
}

void HostStreamer::requestKeyframe() {
    if (const auto enc = currentEncoder()) enc->requestKeyframe();
}

void HostStreamer::setBitrate(int kbps) {
    if (const auto enc = currentEncoder()) enc->setBitrate(kbps);
    cfg_.bitrateKbps = kbps;
}

void HostStreamer::requestEncoderRestart() {
    if (running_.load()) restartRequested_.store(true);
}

void HostStreamer::requestSoftwareEncoder() {
    if (cfg_.codec == VideoCodec::Hevc) forcedEncoder_ = "libx265";
    else if (cfg_.codec == VideoCodec::Av1) forcedEncoder_ = "libsvtav1";
    else forcedEncoder_ = "libx264";
    if (running_.load()) restartRequested_.store(true);
}

HostStreamStats HostStreamer::stats() const {
    HostStreamStats s;
    // Snapshot under stopMutex_ so a concurrent stop() cannot reset members
    // while we read them (stats() runs on the UI timer thread).
    std::lock_guard<std::mutex> lk(stopMutex_);
    if (const auto e = currentEncoder()) {
        s.encodeMs = e->averageEncodeMs();
        s.encoderName = e->encoderName();
        s.framesEncoded = e->framesEncoded();
    }
    if (audioEncoder_) s.audioEncodeMs = audioEncoder_->averageEncodeMs();
    if (capture_) s.captureMs = capture_->averageCaptureMs();
    if (const auto tp = currentTransport()) s.udp = tp->stats();
    {
        std::lock_guard<std::mutex> slk(statsMutex_);
        s.fps = measuredFps_;
        s.actualBitrateKbps = measuredKbps_;
    }
    return s;
}

} // namespace rp
