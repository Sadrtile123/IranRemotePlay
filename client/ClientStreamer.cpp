// Phase 4/5 — client streaming engine implementation. See ClientStreamer.h.

#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "ClientStreamer.h"

#include "../common/Log.h"

#include <chrono>
#include <mutex>

namespace rp {
namespace {
double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}
} // namespace

ClientStreamer::ClientStreamer() = default;

ClientStreamer::~ClientStreamer() { stop(); }

bool ClientStreamer::start(const std::string& hostAddress, uint16_t udpPort, uint32_t sessionId,
                           VideoCodec codec, std::shared_ptr<net::UdpCryptoSink> crypto,
                           PresentCallback present, ErrorCallback onError,
                           std::string* err) {
    std::lock_guard<std::mutex> lk(stopMutex_);
    if (running_.exchange(true)) { if (err) *err = "already running"; running_.store(false); return false; }
    hostAddress_ = hostAddress;
    udpPort_ = udpPort;
    sessionId_ = sessionId;
    codec_ = codec;
    presentCb_ = std::move(present);
    errorCb_ = std::move(onError);

    transport_ = std::make_unique<net::UdpTransport>();
    transport_->setSessionId(sessionId_);
    if (crypto) transport_->setCryptoSink(std::move(crypto));   // BEFORE bind: no plaintext window
    if (!transport_->bind("0.0.0.0", 0, err)) {
        transport_.reset();
        running_.store(false);
        return false;
    }
    if (!transport_->setPeer(hostAddress_, udpPort_, err)) {
        transport_->stop();
        transport_.reset();
        running_.store(false);
        return false;
    }

    decoder_ = std::make_unique<VideoDecoder>();
    if (!decoder_->init(codec_, err)) {
        transport_->stop();
        transport_.reset();
        decoder_.reset();
        running_.store(false);
        return false;
    }

    RP_INFO() << "[client-streamer] udp=" << transport_->localPort() << " -> " << hostAddress_
              << ":" << udpPort_ << " decoder=" << decoder_->decoderName();

    {
        std::lock_guard<std::mutex> lk(statsMutex_);
        statFramesWindow_ = 0;
        statWindowStartMs_ = nowMs();
        measuredFps_ = 0.0;
    }

    // Announce ourselves (opens NAT mappings) + ask for a first keyframe.
    transport_->sendStreamStart();
    transport_->requestKeyframe();
    transport_->startPings(1000);

    // Audio path: lazy player start (first audio packet starts playback).
    audioDecoder_ = std::make_unique<AudioDecoderOpus>();
    std::string aerr;
    if (!audioDecoder_->init(&aerr)) {
        RP_WARN() << "[client-streamer] opus decoder unavailable, audio disabled: " << aerr;
        audioDecoder_.reset();
    }

    decodeThread_ = std::thread([this] { decodeLoop(); });
    return true;
}

void ClientStreamer::stop() {
    if (!running_.exchange(false)) return;
    std::lock_guard<std::mutex> lk(stopMutex_);
    RP_INFO() << "[client-streamer] stopping";
    if (transport_) transport_->sendStreamStop();
    if (decodeThread_.joinable()) decodeThread_.join();
    if (transport_) { transport_->stopPings(); transport_->stop(); transport_.reset(); }
    if (audioPlayer_) audioPlayer_->stop();
    audioPlayer_.reset();
    audioDecoder_.reset();
    decoder_.reset();
}

void ClientStreamer::setHostEndpoint(const std::string& address, uint16_t port) {
    if (!transport_) return;
    std::string err;
    if (!transport_->setPeer(address, port, &err)) {
        RP_WARN() << "[client-streamer] setHostEndpoint failed: " << err;
    }
}

void ClientStreamer::decodeLoop() {
    std::vector<net::AssembledFrame> frames;
    std::vector<DecodedFrame> decoded;
    std::vector<net::AssembledFrame> audioFrames;
    std::vector<int16_t> audioSamples;
    uint64_t lastFrameNs = 0;

    while (running_.load()) {
        frames.clear();
        decoded.clear();
        transport_->pollVideo(net::steadyNowNs(), frames);

        for (const auto& f : frames) {
            std::string derr;
            if (!decoder_->decode(f.data.data(), f.data.size(), f.timestampNs / 1000ull, f.keyframe,
                                  decoded, &derr)) {
                // Corrupt packet: decoder already reset; keyframe will be requested
                // by the transport's loss detection. Keep going.
                RP_DEBUG() << "[client-streamer] decode rejected packet: " << derr;
                continue;
            }
            lastFrameNs = net::steadyNowNs();
        }

        for (const DecodedFrame& df : decoded) {
            if (presentCb_) presentCb_(std::make_shared<DecodedFrame>(df));
            {
                std::lock_guard<std::mutex> lk(statsMutex_);
                ++statFramesWindow_;
                const double windowMs = nowMs() - statWindowStartMs_;
                if (windowMs >= 1000.0) {
                    measuredFps_ = static_cast<double>(statFramesWindow_) * 1000.0 / windowMs;
                    statFramesWindow_ = 0;
                    statWindowStartMs_ = nowMs();
                }
            }
        }

        // ---- audio path ----
        if (audioDecoder_) {
            audioFrames.clear();
            audioSamples.clear();
            transport_->pollAudio(net::steadyNowNs(), audioFrames);
            for (const auto& f : audioFrames) {
                std::string derr;
                if (audioDecoder_->decode(f.data.data(), f.data.size(), audioSamples, &derr)) {
                    if (!audioSamples.empty()) {
                        if (!audioPlayer_) {
                            audioPlayer_ = std::make_unique<audio::WasapiPlayer>();
                            std::string perr;
                            if (audioPlayer_->start(&perr)) audioActive_.store(true);
                            else {
                                RP_WARN() << "[client-streamer] audio playback unavailable: " << perr;
                                audioPlayer_.reset();
                            }
                        }
                        if (audioPlayer_) {
                            audioPlayer_->push(audioSamples.data(), audioSamples.size() / 2);
                        }
                    }
                }
            }
        }

        // Stall watchdog: no video for 2 s while running -> re-request keyframe.
        if (lastFrameNs != 0 && net::steadyNowNs() - lastFrameNs > 2'000'000'000ull) {
            transport_->requestKeyframe();
            lastFrameNs = net::steadyNowNs();     // avoid request storms
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

ClientStreamStats ClientStreamer::stats() const {
    ClientStreamStats s;
    std::lock_guard<std::mutex> lk(stopMutex_);
    if (transport_) s.udp = transport_->stats();
    if (decoder_) {
        s.decodeMs = decoder_->averageDecodeMs();
        s.framesDecoded = decoder_->framesDecoded();
        s.width = decoder_->width();
        s.height = decoder_->height();
        s.decoderName = decoder_->decoderName();
    }
    if (audioDecoder_) s.audioDecodeMs = audioDecoder_->averageDecodeMs();
    if (audioPlayer_) {
        s.audioBufferedMs = audioPlayer_->bufferedMs();
        s.audioUnderruns = audioPlayer_->underruns();
    }
    s.audioActive = audioActive_.load();
    {
        std::lock_guard<std::mutex> lk(statsMutex_);
        s.fps = measuredFps_;
    }
    return s;
}

} // namespace rp
