// Phase 16 — client coordinator implementation. See ClientCoordinator.h.

#include "ClientCoordinator.h"

#include "../common/Log.h"
#include "../decoding/VideoDecoder.h"
#include "../networking/Protocol.h"
#include "../ui/VideoWidget.h"

#include <QMetaObject>

#include "../common/WinHeaders.h"
#include <windows.h>

#include <algorithm>

namespace rp::app {

using proto::Id;

// Qt::Key -> Windows VK mapping (defined at the bottom of this file).
uint32_t qtKeyToVk(int key);
bool isExtendedKey(int key);

ClientCoordinator::ClientCoordinator(config::Config& cfg, QObject* parent)
    : QObject(parent), cfg_(cfg), alive_(std::make_shared<std::atomic<bool>>(true)) {
    connect(&statsTimer_, &QTimer::timeout, this, &ClientCoordinator::onStatsTimer);
    statsTimer_.setInterval(1000);

    // Fires when the host never sends STREAM_START: with the old code the UI
    // sat on "waiting for stream" forever with no hint about what was wrong.
    streamStartWatchdog_.setSingleShot(true);
    streamStartWatchdog_.setInterval(15000);
    connect(&streamStartWatchdog_, &QTimer::timeout, this, &ClientCoordinator::onStreamStartWatchdog);
}

ClientCoordinator::~ClientCoordinator() { stop(); alive_->store(false); }

void ClientCoordinator::setVideoWidget(rp::ui::VideoWidget* w) { videoWidget_ = w; }

void ClientCoordinator::setKeyboardMouseEnabled(bool kb, bool mouse) {
    if (inputSender_) inputSender_->setEnabled(true, kb, mouse);
}

bool ClientCoordinator::startLan(const QString& hostAddress, uint16_t port, const QString& playerName,
                                 const QString& sessionCode, QString* err) {
    (void)err;   // connection errors surface asynchronously via errorOccurred
    stop();
    internetMode_ = false;
    hostAddress_ = hostAddress.toStdString();
    sessionCode_ = sessionCode.toStdString();

    params_ = std::make_unique<client::ClientSessionParams>();
    params_->clientName = playerName.toStdString();
    params_->sessionCode = sessionCode_;
    params_->hostAddress = hostAddress_;
    params_->hostPort = port;
    params_->supportedCodecs = VideoDecoder::supportedCodecs();   // real decoder capability
    if (params_->supportedCodecs.empty()) params_->supportedCodecs = { common::VideoCodec::H264 };

    client_ = std::make_unique<client::ClientApp>();
    wireSessionEvents();
    client::JoinSettings js;
    js.displayName = params_->clientName;
    js.sessionCode = sessionCode_;
    js.hostAddress = hostAddress_;
    js.hostPort = port;
    client_->connect(js);
    statsTimer_.start();
    return true;
}

bool ClientCoordinator::startInternet(const QString& serverAddress, uint16_t serverPort,
                                      const QString& sessionCode, const QString& playerName,
                                      QString* err) {
    stop();
    internetMode_ = true;
    serverHost_ = serverAddress.toStdString();
    sessionCode_ = sessionCode.toStdString();

    signaling_ = std::make_unique<signaling::SignalingClient>();
    std::string serr;
    if (!signaling_->connect(serverHost_, serverPort, 6000, &serr)) {
        if (err) *err = QString::fromStdString("Signaling server: " + serr);
        signaling_.reset();
        return false;
    }
    const auto joined = signaling_->join(sessionCode_, playerName.toStdString());
    if (!joined) {
        if (err) *err = "Join failed: unknown code or session full.";
        signaling_->disconnect();
        signaling_.reset();
        return false;
    }
    relayUdpPort_ = static_cast<uint16_t>(joined->relayUdpPort);
    playerToken_ = joined->token;
    hostAddress_ = joined->hostPublicAddr.empty() ? serverHost_ : joined->hostPublicAddr;

    params_ = std::make_unique<client::ClientSessionParams>();
    params_->clientName = playerName.toStdString();
    params_->sessionCode = sessionCode_;
    params_->hostAddress = serverHost_;
    params_->supportedCodecs = VideoDecoder::supportedCodecs();
    if (params_->supportedCodecs.empty()) params_->supportedCodecs = { common::VideoCodec::H264 };

    client_ = std::make_unique<client::ClientApp>();
    wireSessionEvents();
    // Dial the player's relay pipe; the session handshake runs over it.
    client_->startViaRelay(*params_, serverHost_,
                                     static_cast<uint16_t>(joined->relayTcpPort), joined->token);
    ui_.playerIndex = joined->playerIndex + 1;
    statsTimer_.start();
    return true;
}

bool ClientCoordinator::active() const { return client_ && client_->connected(); }

void ClientCoordinator::stop() {
    statsTimer_.stop();
    streamStartWatchdog_.stop();
    inputWired_ = false;
    if (inputSender_) { inputSender_->stop(); inputSender_.reset(); }
    if (streamer_) { streamer_->stop(); }   // waits for an in-flight start() to finish
    if (streamThread_.joinable()) streamThread_.join();   // no more detached threads
    streamer_.reset();
    channel_.reset();
    ecdh_.reset();
    ourSalt_.clear();
    keysReady_ = false;
    if (client_) { client_->disconnect(); client_.reset(); }
    if (signaling_) { signaling_->disconnect(); signaling_.reset(); }
    ui_ = ClientUiState{};
    emit stateChanged();
}

void ClientCoordinator::wireSessionEvents() {
    client::ClientSession::Events ev;
    ev.onLog = [this](log::Level lvl, const std::string& msg) {
        (void)lvl;
        QMetaObject::invokeMethod(this, [this, msg] {
            emit logLine(QString::fromStdString(msg));
        }, Qt::QueuedConnection);
    };
    ev.onState = [this](common::ConnectionState state) {
        QMetaObject::invokeMethod(this, [this, state] {
            switch (state) {
                case common::ConnectionState::Connected:
                    ui_.status = "Connected - securing session...";
                    beginKeyExchange();
                    break;
                case common::ConnectionState::Disconnected:
                    ui_.status = "Disconnected";
                    if (streamer_) streamer_->stop();
                    break;
                default: break;
            }
            emit stateChanged();
        }, Qt::QueuedConnection);
    };
    ev.onConnected = [this](const client::ClientStatus& st) {
        QMetaObject::invokeMethod(this, [this, st] {
            ui_.hostName = QString::fromStdString(st.hostName);
            ui_.gameName = QString::fromStdString(st.gameName);
            negotiatedCodec_ = st.codec;
            emit connectedToHost(ui_.hostName, ui_.gameName, ui_.playerIndex);
            emit stateChanged();
        }, Qt::QueuedConnection);
    };
    ev.onDisconnected = [this](const std::string& reason) {
        QMetaObject::invokeMethod(this, [this, reason] {
            emit disconnected(QString::fromStdString(reason));
        }, Qt::QueuedConnection);
    };
    ev.onError = [this](const std::string& msg) {
        QMetaObject::invokeMethod(this, [this, msg] {
            emit errorOccurred(QString::fromStdString(msg));
        }, Qt::QueuedConnection);
    };
    ev.onInputPermissions = [this](const common::InputPermissions perms) {
        QMetaObject::invokeMethod(this, [this, perms] {
            if (inputSender_) inputSender_->setEnabled(perms.controller, perms.keyboard, perms.mouse);
        }, Qt::QueuedConnection);
    };
    ev.onAppMessage = [this](uint16_t type, const std::vector<uint8_t>& payload) {
        QMetaObject::invokeMethod(this, [this, type, payload] {
            handleAppMessage(type, payload);
        }, Qt::QueuedConnection);
    };
    client_->setEvents(ev);
}

void ClientCoordinator::beginKeyExchange() {
    if (ecdh_ || !client_ || !client_->connected()) return;
    auto kp = crypto::generateEcdhP256();
    if (!kp) {
        emit errorOccurred("Cryptography unavailable (CNG); cannot secure this session.");
        client_->disconnect();
        return;
    }
    ecdh_ = std::make_unique<crypto::EcdhKeyPair>(std::move(*kp));
    ourSalt_ = crypto::randomBytes(16);

    proto::msg::KeyExchange kx;
    kx.publicKey = ecdh_->publicKey;
    kx.salt = ourSalt_;
    proto::Envelope env;
    env.type = Id::KeyExchange;
    env.body = std::move(kx);
    client_->sendAppMessage(static_cast<uint16_t>(Id::KeyExchange), proto::encodeMessage(env));
}

void ClientCoordinator::handleAppMessage(uint16_t type, const std::vector<uint8_t>& payload) {
    const auto env = proto::decodeMessage(type, payload.data(), static_cast<uint32_t>(payload.size()));
    if (!env) return;

    switch (env->type) {
        case Id::KeyExchange: {
            auto* m = std::get_if<proto::msg::KeyExchange>(&env->body);
            if (!m || !ecdh_) return;
            channel_ = std::make_shared<crypto::SecureChannel>();
            if (!channel_->finish(*ecdh_, ourSalt_, m->publicKey, m->salt, sessionCode_, false)) {
                emit errorOccurred("Key exchange failed.");
                client_->disconnect();
                return;
            }
            proto::Envelope ready;
            ready.type = Id::SessionKeyReady;
            client_->sendAppMessage(static_cast<uint16_t>(Id::SessionKeyReady),
                                              proto::encodeMessage(ready));
            ui_.status = "Secure session established";
            break;
        }
        case Id::SessionKeyReady: {
            keysReady_ = true;
            ui_.status = "Waiting for the host to start the stream...";
            streamStartWatchdog_.start();
            emit stateChanged();
            break;
        }
        case Id::StreamStart: {
            auto* m = std::get_if<proto::msg::StreamStart>(&env->body);
            if (!m) return;
            keysReady_ = true;
            streamStartWatchdog_.stop();
            negotiatedCodec_ = static_cast<common::VideoCodec>(m->codec);
            startStream(m->udpPort, m->sessionId);
            break;
        }
        case Id::StreamStop: {
            if (streamer_) streamer_->stop();
            if (inputSender_) inputSender_->stop();
            ui_.status = "Stream stopped";
            emit stateChanged();
            break;
        }
        default: break;
    }
}

void ClientCoordinator::startStream(uint16_t udpPort, uint32_t udpSessionId) {
    if (streamer_) return;    // already running

    streamer_ = std::make_shared<ClientStreamer>();
    streamer_->setVideoJitterTargetMs(static_cast<unsigned>(cfg_.network.jitterBufferMs));
    const auto crypto = channel_;                 // installed before bind
    const auto alive = alive_;
    const auto weakStreamer = std::weak_ptr(streamer_);

    rp::ui::VideoWidget* widget = videoWidget_;
    // Decode thread -> UI thread. The DecodedFrame travels via shared_ptr (no
    // 8 MB full copy per frame any more); VideoWidget::presentFrame does the
    // single copy into its QImage.
    const auto present = [widget](std::shared_ptr<rp::DecodedFrame> df) {
        if (!widget || !df) return;
        QMetaObject::invokeMethod(widget, [widget, df] {
            widget->presentFrame(df->bgra.data(), df->width, df->height, df->width * 4);
        }, Qt::QueuedConnection);
    };
    const auto onError = [this](const std::string& msg) {
        QMetaObject::invokeMethod(this, [this, msg] {
            emit errorOccurred(QString::fromStdString(msg));
        }, Qt::QueuedConnection);
    };

    const uint8_t slot = static_cast<uint8_t>(ui_.playerIndex > 0 ? ui_.playerIndex - 1 : 0);
    const bool kb = cfg_.input.keyboard;
    const bool mouse = cfg_.input.mouse;
    if (streamThread_.joinable()) streamThread_.join();   // reap any previous worker
    const bool viaRelay = internetMode_;
    const std::string host = internetMode_ ? serverHost_ : hostAddress_;
    const uint16_t port = internetMode_ ? relayUdpPort_ : udpPort;
    const std::string token = playerToken_;
    streamThread_ = std::thread([this, weakStreamer, present, onError, viaRelay, host, port, udpPort,
                                 udpSessionId, token, codec = negotiatedCodec_, crypto, alive, slot, kb, mouse]() mutable {
        const auto streamer = weakStreamer.lock();
        if (!streamer) return;
        std::string serr;
        if (!streamer->start(host, viaRelay ? port : udpPort, udpSessionId, codec, crypto,
                             present, onError, &serr)) {
            if (!alive->load()) return;
            QMetaObject::invokeMethod(this, [this, serr] {
                emit errorOccurred(QString("Stream failed to start: %1").arg(QString::fromStdString(serr)));
            }, Qt::QueuedConnection);
            return;
        }
        if (viaRelay) {
            streamer->udpTransport().sendRelayBind(token);
            asio::ip::udp::endpoint relay(asio::ip::make_address(host), port);
            streamer->udpTransport().setRelayFallback(relay);
        }
        if (!alive->load()) return;
        QMetaObject::invokeMethod(this, [this, weakStreamer, slot, kb, mouse] {
            ui_.status = "Streaming";
            streamStartWatchdog_.stop();
            // Input forwarding starts only AFTER the transport exists (the
            // streamer is running now) - fixes the null-transport race.
            const auto s = weakStreamer.lock();
            if (!s || !inputSender_ || inputSender_->running()) { emit stateChanged(); return; }
            std::string ierr;
            if (!inputSender_->start(&s->udpTransport(), slot, &ierr)) {
                emit logLine(QString("Input: %1").arg(QString::fromStdString(ierr)));
            } else {
                inputSender_->setEnabled(true, kb, mouse);
            }
            wireInputForwarding();
            emit stateChanged();
        }, Qt::QueuedConnection);
    });
}

void ClientCoordinator::wireInputForwarding() {
    if (!videoWidget_ || !inputSender_ || inputWired_) return;
    inputWired_ = true;   // one connection set per video widget; a reconnect
                          // reuses them (they read the CURRENT input sender)
    // VideoWidget emits on the UI thread; InputSender queue is thread-safe.
    connect(videoWidget_, &rp::ui::VideoWidget::keyPressed, this,
            [this](int key, bool repeat) {
        if (!inputSender_) return;
        rp::input::InputEvent e;
        e.type = static_cast<uint8_t>(rp::input::InputEventType::KeyDown);
        e.code = static_cast<uint8_t>(qtKeyToVk(key));
        e.flags = isExtendedKey(key) ? 1 : 0;
        e.timestampNs = rp::net::steadyNowNs();
        if (!repeat) inputSender_->queueEvent(e);
    });
    connect(videoWidget_, &rp::ui::VideoWidget::keyReleased, this, [this](int key) {
        if (!inputSender_) return;
        rp::input::InputEvent e;
        e.type = static_cast<uint8_t>(rp::input::InputEventType::KeyUp);
        e.code = static_cast<uint8_t>(qtKeyToVk(key));
        e.flags = isExtendedKey(key) ? 1 : 0;
        e.timestampNs = rp::net::steadyNowNs();
        inputSender_->queueEvent(e);
    });
    connect(videoWidget_, &rp::ui::VideoWidget::mouseMoved, this, [this](int x, int y) {
        if (!inputSender_ || !videoWidget_) return;
        rp::input::InputEvent e;
        e.type = static_cast<uint8_t>(rp::input::InputEventType::MouseMove);
        // Normalize to the video rect (not the whole widget) so the game maps 1:1.
        const QRect video = videoWidget_->videoDestRect();
        const int vx = video.contains(x, y) ? x - video.x() : std::clamp(x - video.x(), 0, video.width() - 1);
        const int vy = video.contains(x, y) ? y - video.y() : std::clamp(y - video.y(), 0, video.height() - 1);
        e.x = static_cast<uint16_t>((std::clamp(vx, 0, video.width() - 1) * 65535) / std::max(1, video.width() - 1));
        e.y = static_cast<uint16_t>((std::clamp(vy, 0, video.height() - 1) * 65535) / std::max(1, video.height() - 1));
        e.timestampNs = rp::net::steadyNowNs();
        inputSender_->queueEvent(e);
    });
    const auto buttonEvent = [this](bool down) {
        return [this, down](int button, int /*x*/, int /*y*/) {
            if (!inputSender_) return;
            rp::input::InputEvent e;
            e.type = static_cast<uint8_t>(down ? rp::input::InputEventType::MouseButtonDown
                                               : rp::input::InputEventType::MouseButtonUp);
            switch (button) {
                case Qt::LeftButton: e.code = 0; break;
                case Qt::RightButton: e.code = 1; break;
                case Qt::MiddleButton: e.code = 2; break;
                case Qt::XButton1: e.code = 3; break;
                default: e.code = 4; break;
            }
            e.timestampNs = rp::net::steadyNowNs();
            inputSender_->queueEvent(e);
        };
    };
    connect(videoWidget_, &rp::ui::VideoWidget::mouseButtonPressed, this, buttonEvent(true));
    connect(videoWidget_, &rp::ui::VideoWidget::mouseButtonReleased, this, buttonEvent(false));
    connect(videoWidget_, &rp::ui::VideoWidget::mouseWheel, this, [this](int delta) {
        if (!inputSender_) return;
        rp::input::InputEvent e;
        e.type = static_cast<uint8_t>(rp::input::InputEventType::MouseWheel);
        e.delta = static_cast<int16_t>(std::clamp(delta / 120, -100, 100));
        e.timestampNs = rp::net::steadyNowNs();
        inputSender_->queueEvent(e);
    });
    // Fullscreen toggling is handled by ClientWindow (window-level, F11 +
    // double-click); the old connection here called showFullScreen() on the
    // child video widget, which does nothing for a non-top-level widget.
}

void ClientCoordinator::onStreamStartWatchdog() {
    if (streamer_ && streamer_->running()) return;   // stream came up; ignore
    emit errorOccurred(
        "The host did not start the video stream within 15 seconds. "
        "The host app may have hit an error (see its log under %LOCALAPPDATA%\\RemotePlay\\Logs) "
        "or the connection is blocked. Disconnecting.");
    ui_.status = "Stream never started";
    emit stateChanged();
    stop();
}

void ClientCoordinator::onStatsTimer() {
    if (streamer_) {
        const auto st = streamer_->stats();
        ui_.rttMs = st.udp.avgRttMs;
        ui_.jitterMs = st.udp.jitterMs;
        const uint64_t lost = st.udp.lostFrames;
        const uint64_t total = st.udp.assembledFrames + lost;
        ui_.lossPercent = total ? (double(lost) / double(total)) * 100.0 : 0.0;
        ui_.fps = st.fps;
        ui_.decodeMs = st.decodeMs;
        ui_.audioMs = st.audioDecodeMs;
        ui_.audioActive = st.audioActive;
        const double bps = st.udp.recvBytes > lastRecvBytes_ ? (st.udp.recvBytes - lastRecvBytes_) * 8.0 : 0.0;
        lastRecvBytes_ = st.udp.recvBytes;
        ui_.bitrateKbps = bps / 1000.0;
        ui_.width = st.width;
        ui_.height = st.height;
        ui_.decoderName = QString::fromStdString(st.decoderName);
        emit stateChanged();
    }
}

// --- Qt::Key -> Windows VK mapping (subset; unknown keys are dropped) ---
uint32_t qtKeyToVk(int key);
bool isExtendedKey(int key);

uint32_t qtKeyToVk(int key) {
    if (key >= 'A' && key <= 'Z') return 'A' + (key - 'A');
    if (key >= '0' && key <= '9') return '0' + (key - '0');
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24) return 0x70 + (key - Qt::Key_F1);
    switch (key) {
        case Qt::Key_Space: return VK_SPACE;
        case Qt::Key_Return: case Qt::Key_Enter: return VK_RETURN;
        case Qt::Key_Escape: return VK_ESCAPE;
        case Qt::Key_Backspace: return VK_BACK;
        case Qt::Key_Tab: return VK_TAB;
        case Qt::Key_Shift: return VK_SHIFT;
        case Qt::Key_Control: return VK_CONTROL;
        case Qt::Key_Alt: return VK_MENU;
        case Qt::Key_Pause: return VK_PAUSE;
        case Qt::Key_CapsLock: return VK_CAPITAL;
        case Qt::Key_PageUp: return VK_PRIOR;
        case Qt::Key_PageDown: return VK_NEXT;
        case Qt::Key_End: return VK_END;
        case Qt::Key_Home: return VK_HOME;
        case Qt::Key_Left: return VK_LEFT;
        case Qt::Key_Up: return VK_UP;
        case Qt::Key_Right: return VK_RIGHT;
        case Qt::Key_Down: return VK_DOWN;
        case Qt::Key_Insert: return VK_INSERT;
        case Qt::Key_Delete: return VK_DELETE;
        case Qt::Key_Minus: return VK_OEM_MINUS;
        case Qt::Key_Equal: return VK_OEM_PLUS;
        case Qt::Key_BracketLeft: return VK_OEM_4;
        case Qt::Key_BracketRight: return VK_OEM_6;
        case Qt::Key_Backslash: return VK_OEM_5;
        case Qt::Key_Semicolon: return VK_OEM_1;
        case Qt::Key_Apostrophe: return VK_OEM_7;
        case Qt::Key_Comma: return VK_OEM_COMMA;
        case Qt::Key_Period: return VK_OEM_PERIOD;
        case Qt::Key_Slash: return VK_OEM_2;
        case Qt::Key_QuoteLeft: return VK_OEM_3;
        default: return 0;   // unmapped: dropped
    }
}

bool isExtendedKey(int key) {
    switch (key) {
        case Qt::Key_PageUp: case Qt::Key_PageDown: case Qt::Key_End: case Qt::Key_Home:
        case Qt::Key_Left: case Qt::Key_Up: case Qt::Key_Right: case Qt::Key_Down:
        case Qt::Key_Insert: case Qt::Key_Delete: case Qt::Key_Alt:
            return true;
        default:
            return false;
    }
}

} // namespace rp::app
