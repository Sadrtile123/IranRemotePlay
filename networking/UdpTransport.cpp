// Phase 4 — UDP media transport implementation. See UdpTransport.h.

#include "UdpTransport.h"

#include "../common/Log.h"

#include <cstring>

namespace rp::net {

uint64_t steadyNowNs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

UdpTransport::UdpTransport() = default;

UdpTransport::~UdpTransport() { stop(); }

bool UdpTransport::bind(const std::string& localAddress, uint16_t port, std::string* err) {
    stop();
    try {
        io_ = std::make_unique<asio::io_context>();
        socket_ = std::make_unique<asio::ip::udp::socket>(*io_);
        asio::ip::udp::endpoint ep(asio::ip::make_address(localAddress.empty() ? "0.0.0.0" : localAddress), port);
        socket_->open(ep.protocol());
        socket_->non_blocking(false);
        asio::socket_base::reuse_address reuse(true);
        socket_->set_option(reuse);
        asio::socket_base::send_buffer_size sendBuf(4 * 1024 * 1024);
        socket_->set_option(sendBuf);
        asio::socket_base::receive_buffer_size recvBuf(4 * 1024 * 1024);
        socket_->set_option(recvBuf);
        socket_->bind(ep);
        localPort_.store(socket_->local_endpoint().port(), std::memory_order_relaxed);

        running_.store(true);
        ioThread_ = std::thread([this] { runIo(); });
        return true;
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        socket_.reset();
        io_.reset();
        return false;
    }
}

void UdpTransport::stop() {
    if (!running_.exchange(false)) return;
    if (io_) {
        asio::post(*io_, [this] {
            if (pingTimer_) pingTimer_->cancel();
            if (socket_) {
                asio::error_code ec;
                socket_->close(ec);
            }
        });
    }
    if (ioThread_.joinable()) ioThread_.join();
    pingTimer_.reset();
    socket_.reset();
    io_.reset();
    localPort_.store(0);
}

bool UdpTransport::setPeer(const std::string& host, uint16_t port, std::string* err) {
    try {
        asio::ip::udp::resolver resolver(*io_);
        auto results = resolver.resolve(asio::ip::udp::v4(), host, std::to_string(port));
        if (results.empty()) { if (err) *err = "resolve failed"; return false; }
        return setPeer(*results.begin());
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }
}

bool UdpTransport::setPeer(asio::ip::udp::endpoint ep) {
    std::lock_guard<std::mutex> lk(peerMutex_);
    peer_ = std::move(ep);
    havePeer_.store(true);
    return true;
}

void UdpTransport::lockToRemote(const asio::ip::udp::endpoint& ep) {
    std::lock_guard<std::mutex> lk(peerMutex_);
    lockedRemote_ = ep;
}

void UdpTransport::runIo() {
    doReceive();
    io_->run();
}

void UdpTransport::doReceive() {
    if (!socket_ || !running_.load()) return;
    socket_->async_receive_from(
        asio::buffer(recvBuf_.data(), recvBuf_.size()), senderEndpoint_,
        [this](const asio::error_code& ec, size_t bytes) {
            if (ec == asio::error::operation_aborted || !running_.load()) return;
            if (!ec) {
                handleDatagram(recvBuf_.data(), bytes, senderEndpoint_);
            }
            doReceive();
        });
}

void UdpTransport::handleDatagram(const uint8_t* buf, size_t size, const asio::ip::udp::endpoint& from) {
    recvDatagrams_.fetch_add(1, std::memory_order_relaxed);
    recvBytes_.fetch_add(size, std::memory_order_relaxed);

    UdpHeader h;
    if (!decodeUdpHeader(buf, size, h)) { malformed_.fetch_add(1); return; }
    if (sessionId_ != 0 && h.sessionId != sessionId_) { wrongSession_.fetch_add(1); return; }

    // Phase 12: decrypt payload before anything else consumes it. The header
    // (bound as AAD) already validated above. Fail closed.
    std::vector<uint8_t> decryptedStorage;
    const uint8_t* payloadPtr = buf + kUdpHeaderSize;
    size_t payloadLen = h.payloadSize;
    if (h.flags & static_cast<uint8_t>(UdpFlag::Encrypted)) {
        if (!secure_) { authDrops_.fetch_add(1); return; }          // unexpected ciphertext
        std::vector<uint8_t> clear;
        if (!secure_->open(buf, payloadPtr, payloadLen, clear)) {   // header = full AAD
            authDrops_.fetch_add(1);
            return;
        }
        decryptedStorage = std::move(clear);
        payloadPtr = decryptedStorage.data();
        payloadLen = decryptedStorage.size();
    } else if (secure_ && secure_->active()) {
        authDrops_.fetch_add(1);                                    // plaintext after keys active
        return;
    }

    // Learn the peer address from the first valid datagram (host side after
    // the client started sending, or both sides after a hole punch).
    if (!havePeer_.load()) {
        setPeer(from);
        RP_DEBUG() << "[udp] learned peer " << from.address().to_string() << ":" << from.port();
    }

    if (dropUnknownRemote_.load()) {
        std::lock_guard<std::mutex> lk(peerMutex_);
        if (lockedRemote_ && *lockedRemote_ != from) {
            // Unknown source: ignore silently (spoofing / stray traffic).
            return;
        }
    }

    const uint8_t* payload = payloadPtr;
    (void)payloadLen;   // length implied by h.payloadSize / decrypted size
    const UdpType type = static_cast<UdpType>(h.type);

    // Inter-arrival jitter estimate (EWMA of arrival-interval deviation, ns).
    {
        const uint64_t now = steadyNowNs();
        std::lock_guard<std::mutex> lk(mediaMutex_);
        if (lastArrivalNs_ != 0 && now > lastArrivalNs_) {
            const double dev = static_cast<double>(now - lastArrivalNs_);
            jitterEWMA_ = jitterEWMA_ == 0.0 ? dev : (jitterEWMA_ * 0.9 + dev * 0.1);
        }
        lastArrivalNs_ = now;
    }

    switch (type) {
        case UdpType::Ping: {
            // Echo as Pong (same payload).
            std::vector<uint8_t> pong(payload, payload + h.payloadSize);
            sendSmall(UdpType::Pong, pong.data(), pong.size());
            return;
        }
        case UdpType::Pong: {
            if (h.payloadSize >= 12) {
                uint64_t sentNs; std::memcpy(&sentNs, payload, 8);
                uint32_t seq;     std::memcpy(&seq, payload + 8, 4);
                const double rttMs = static_cast<double>(steadyNowNs() - sentNs) / 1e6;
                if (sentNs <= steadyNowNs()) {
                    lastRttMs_.store(rttMs);
                    double avg = avgRttMs_.load();
                    avg = avg == 0.0 ? rttMs : (avg * 0.8 + rttMs * 0.2);
                    avgRttMs_.store(avg);
                    (void)seq;
                }
            }
            return;
        }
        default: break;
    }

    // Media & control payloads.
    if (type == UdpType::Control || type == UdpType::KeyframeRequest ||
        type == UdpType::StreamStart || type == UdpType::StreamStop) {
        std::vector<uint8_t> data(payload, payload + h.payloadSize);
        if (controlCb_) controlCb_(type, data);
        return;
    }

    // Media reassembly path.
    AssembledFrame frame;
    bool complete = false;
    {
        std::lock_guard<std::mutex> lk(mediaMutex_);
        switch (type) {
            case UdpType::Video: {
                const int64_t gap = videoSeq_.track(h.sequence);
                complete = videoReassembly_.feed(h, payload, frame);
                videoReassembly_.gc();
                if (gap > 1 && autoKeyframeRequest_.load()) {
                    const uint64_t now = steadyNowNs();
                    if (now - lastKeyframeReqNs_ > 500'000'000ull) {   // rate limit: 2/s
                        lastKeyframeReqNs_ = now;
                        // Posting (not calling) avoids nested locks; runs on the io thread.
                        asio::post(*io_, [this] { requestKeyframe(); });
                    }
                }
                break;
            }
            case UdpType::Audio: {
                const int64_t gap = audioSeq_.track(h.sequence);
                (void)gap;
                complete = audioReassembly_.feed(h, payload, frame);
                audioReassembly_.gc();
                break;
            }
            case UdpType::Input: {
                complete = inputReassembly_.feed(h, payload, frame);
                break;
            }
            default: break;
        }
    }
    if (complete) handleMediaComplete(std::move(frame));
}

void UdpTransport::handleMediaComplete(AssembledFrame&& frame) {
    const UdpType t = frame.type;
    if (t == UdpType::Input) {
        InputCallback cb = inputCb_;   // callbacks are set once at setup; copy is fine
        if (cb) cb(frame);
        return;
    }
    std::lock_guard<std::mutex> lk(mediaMutex_);
    const uint64_t now = steadyNowNs();
    if (t == UdpType::Video) {
        videoJitter_.setTargetMs(videoJitterTargetMs_.load());
        videoJitter_.push(std::move(frame), now);
    } else if (t == UdpType::Audio) {
        audioJitter_.setTargetMs(audioJitterTargetMs_.load());
        audioJitter_.push(std::move(frame), now);
    }
}

void UdpTransport::sendFrame(UdpType type, bool keyframe, const void* data, size_t size, uint64_t timestampNs) {
    if (!running_.load() || !socket_ || !havePeer_.load()) return;
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    const uint16_t frags = fragmentCountFor(size);
    if (frags < 1) return;
    sentFrames_.fetch_add(1, std::memory_order_relaxed);

    // Whole frame shares ONE sequence; fragments are distinguished by fragmentIndex.
    const uint64_t frameSeq = seqCounter_[static_cast<size_t>(type)].fetch_add(1, std::memory_order_relaxed);

    for (uint16_t fi = 0; fi < frags; ++fi) {
        const size_t off = static_cast<size_t>(fi) * kMaxUdpPayload;
        const uint16_t payloadSize = static_cast<uint16_t>(
            (fi + 1 == frags) ? (size - off) : kMaxUdpPayload);

        UdpHeader h;
        h.type = static_cast<uint8_t>(type);
        h.flags = static_cast<uint8_t>((keyframe ? static_cast<uint8_t>(UdpFlag::Keyframe) : 0)
                                       | ((frags > 1) ? static_cast<uint8_t>(UdpFlag::Fragment) : 0));
        h.sessionId = sessionId_;
        h.sequence = frameSeq;
        h.fragmentIndex = fi;
        h.fragmentCount = frags;
        h.timestampNs = timestampNs;
        h.payloadSize = payloadSize;

        std::vector<uint8_t> dg(kUdpHeaderSize + payloadSize);
        encodeUdpHeader(dg.data(), h);
        std::memcpy(dg.data() + kUdpHeaderSize, bytes + off, payloadSize);

        postDatagram(dg);
    }
}

void UdpTransport::sendSmall(UdpType type, const void* data, size_t size) {
    if (!running_.load() || !socket_ || !havePeer_.load()) return;
    if (size > kMaxUdpPayload) size = kMaxUdpPayload;
    UdpHeader h;
    h.type = static_cast<uint8_t>(type);
    h.sessionId = sessionId_;
    h.sequence = seqCounter_[static_cast<size_t>(type)].fetch_add(1, std::memory_order_relaxed);
    h.fragmentCount = 1;
    h.fragmentIndex = 0;
    h.timestampNs = steadyNowNs();
    h.payloadSize = static_cast<uint16_t>(size);

    std::vector<uint8_t> dg(kUdpHeaderSize + size);
    encodeUdpHeader(dg.data(), h);
    if (size) std::memcpy(dg.data() + kUdpHeaderSize, data, size);
    postDatagram(dg);
}

void UdpTransport::sendRelayBind(const std::string& token32hex) {
    if (!running_.load() || !socket_ || !havePeer_.load()) return;
    std::lock_guard<std::mutex> lk(peerMutex_);
    const asio::ip::udp::endpoint target = peer_;
    // Raw packet: "RPBIND" + 32 hex chars (not the RemotePlay media header —
    // the relay consumes it before any media flows).
    std::vector<uint8_t> pkt(6 + 32, 0);
    std::memcpy(pkt.data(), "RPBIND", 6);
    std::memcpy(pkt.data() + 6, token32hex.data(), std::min<size_t>(32, token32hex.size()));
    asio::post(*io_, [this, target, pkt = std::move(pkt)] {
        if (!socket_) return;
        asio::error_code ec;
        socket_->send_to(asio::buffer(pkt), target, 0, ec);
        if (ec) sendErrors_.fetch_add(1);
    });
}

void UdpTransport::sendPunch(const asio::ip::udp::endpoint& to) {
    if (!running_.load() || !socket_) return;
    UdpHeader h;
    h.type = static_cast<uint8_t>(UdpType::Control);
    h.flags = static_cast<uint8_t>(UdpFlag::Fragment);   // marker bit reused for punch packets
    h.sessionId = sessionId_;
    h.sequence = 0;
    h.payloadSize = 4;
    std::vector<uint8_t> dg(kUdpHeaderSize + 4);
    encodeUdpHeader(dg.data(), h);
    const uint32_t punch = 0x50494d50;   // "PIMP" punch marker
    std::memcpy(dg.data() + kUdpHeaderSize, &punch, 4);
    asio::post(*io_, [this, to, dg = std::move(dg)] {
        asio::error_code ec;
        socket_->send_to(asio::buffer(dg), to, 0, ec);
        if (ec) sendErrors_.fetch_add(1);
        else { sentDatagrams_.fetch_add(1); sentBytes_.fetch_add(dg.size()); }
    });
}

void UdpTransport::postDatagram(std::vector<uint8_t>& dg) {
    std::lock_guard<std::mutex> lk(peerMutex_);
    asio::post(*io_, [this, dg = std::move(dg)]() mutable {
        if (!socket_ || !havePeer_.load()) return;

        // Phase 12: seal the payload in place (header stays clear; AAD).
        std::vector<uint8_t> out;
        const uint8_t* payload = dg.data() + kUdpHeaderSize;
        const size_t payloadLen = dg.size() - kUdpHeaderSize;
        if (secure_) {
            if (!secure_->seal(dg.data(), payload, payloadLen, out)) {
                return;   // fail closed: never send unsealed media
            }
            UdpHeader h;
            decodeUdpHeader(dg.data(), dg.size(), h);
            h.flags |= static_cast<uint8_t>(UdpFlag::Encrypted);
            h.payloadSize = static_cast<uint16_t>(out.size());
            dg.assign(kUdpHeaderSize + out.size(), 0);
            encodeUdpHeader(dg.data(), h);
            std::memcpy(dg.data() + kUdpHeaderSize, out.data(), out.size());
        }

        asio::error_code ec;
        socket_->send_to(asio::buffer(dg), peer_, 0, ec);
        if (ec) sendErrors_.fetch_add(1, std::memory_order_relaxed);
        else {
            sentDatagrams_.fetch_add(1, std::memory_order_relaxed);
            sentBytes_.fetch_add(dg.size(), std::memory_order_relaxed);
        }
    });
}

void UdpTransport::pollVideo(uint64_t nowNs, std::vector<AssembledFrame>& out) {
    std::lock_guard<std::mutex> lk(mediaMutex_);
    videoJitter_.pop(out, nowNs);
}

void UdpTransport::pollAudio(uint64_t nowNs, std::vector<AssembledFrame>& out) {
    std::lock_guard<std::mutex> lk(mediaMutex_);
    audioJitter_.pop(out, nowNs);
}

void UdpTransport::startPings(unsigned intervalMs) {
    if (!io_ || !running_.load()) return;
    pingIntervalMs_.store(intervalMs);
    if (!pingTimer_) pingTimer_ = std::make_unique<asio::steady_timer>(*io_);
    sendPingOnce();
}

void UdpTransport::sendPingOnce() {
    if (!running_.load()) return;
    {
        std::vector<uint8_t> payload(12);
        const uint64_t now = steadyNowNs();
        const uint32_t seq = pingSeq_.fetch_add(1);
        std::memcpy(payload.data(), &now, 8);
        std::memcpy(payload.data() + 8, &seq, 4);
        sendSmall(UdpType::Ping, payload.data(), payload.size());
    }
    pingTimer_->expires_after(std::chrono::milliseconds(pingIntervalMs_.load()));
    pingTimer_->async_wait([this](const asio::error_code& ec) {
        if (ec || !running_.load() || pingIntervalMs_.load() == 0) return;
        sendPingOnce();
    });
}

void UdpTransport::stopPings() { pingIntervalMs_.store(0); }

UdpStatsSnapshot UdpTransport::stats() const {
    UdpStatsSnapshot s;
    s.sentDatagrams = sentDatagrams_.load();
    s.sentBytes = sentBytes_.load();
    s.sendErrors = sendErrors_.load();
    s.sentFrames = sentFrames_.load();
    s.recvDatagrams = recvDatagrams_.load();
    s.recvBytes = recvBytes_.load();
    s.malformed = malformed_.load();
    s.wrongSession = wrongSession_.load();
    s.lastRttMs = lastRttMs_.load();
    s.avgRttMs = avgRttMs_.load();
    std::lock_guard<std::mutex> lk(mediaMutex_);
    s.assembledFrames = videoReassembly_.completedFrames() + audioReassembly_.completedFrames();
    s.droppedIncomplete = videoReassembly_.droppedIncomplete() + audioReassembly_.droppedIncomplete();
    s.lostFrames = videoSeq_.lost() + audioSeq_.lost();
    s.duplicates = videoSeq_.duplicates() + audioSeq_.duplicates();
    s.lateFrames = videoSeq_.late() + audioSeq_.late();
    s.reorderedFrames = videoJitter_.reordered() + audioJitter_.reordered();
    s.jitterMs = jitterEWMA_ / 1e6;
    s.jitterTargetMs = videoJitterTargetMs_.load();
    s.jitterQueue = videoJitter_.size();
    return s;
}

void UdpTransport::resetStats() {
    sentDatagrams_ = sentBytes_ = sendErrors_ = sentFrames_ = 0;
    recvDatagrams_ = recvBytes_ = malformed_ = wrongSession_ = 0;
    std::lock_guard<std::mutex> lk(mediaMutex_);
    videoSeq_.reset(); audioSeq_.reset();
    videoJitter_.reset(); audioJitter_.reset();
    jitterEWMA_ = 0.0;
}

} // namespace rp::net
