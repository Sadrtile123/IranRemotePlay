// Phase 16 — host coordinator implementation. See HostCoordinator.h.

#include "HostCoordinator.h"

#include "../common/Log.h"
#include "../networking/Protocol.h"

#include <QMetaObject>

#include <algorithm>
#include <cstring>

namespace rp::app {

using proto::Id;
using proto::msg::InputPermission;

namespace {
// Deterministic UDP sessionId from the session code + client id (both sides
// can compute/verify it; the STREAM_START message carries it authoritatively).
uint32_t makeUdpSessionId(const std::string& code, uint32_t clientId) {
    uint32_t h = 2166136261u;
    for (char c : code) h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
    h = (h ^ clientId) * 16777619u;
    return h | 1u;   // never 0 (0 = "accept any")
}

InputPermission toProto(const common::InputPermissions& p) {
    InputPermission m;
    m.controller = p.controller;
    m.keyboard = p.keyboard;
    m.mouse = p.mouse;
    m.vibration = p.vibration;
    return m;
}
} // namespace

HostCoordinator::HostCoordinator(config::Config& cfg, QObject* parent)
    : QObject(parent), cfg_(cfg), injector_(), abr_(cfg_.video.bitrateMbps * 1000),
      alive_(std::make_shared<std::atomic<bool>>(true)) {
    std::string err;
    injector_.init(&err);   // ViGEm optional; kb/m always available
    ui_.gamepadStatus = QString::fromStdString(injector_.gamepadStatus());

    connect(&statsTimer_, &QTimer::timeout, this, &HostCoordinator::onStatsTimer);
    statsTimer_.setInterval(1000);
}

HostCoordinator::~HostCoordinator() { stop(); alive_->store(false); }

bool HostCoordinator::startLan(const host::HostLaunchSettings& settings, QString* err) {
    stop();
    internetMode_ = false;
    settings_ = std::make_unique<host::HostLaunchSettings>(settings);
    settings_->audioBitrateKbps = cfg_.audio.bitrateKbps;
    host_ = std::make_unique<host::HostApp>();
    wireHostEvents();
    if (!host_->start(settings)) {
        if (err) *err = "Could not start the session (is the listen port already in use? See the log for details.)";
        host_.reset();
        return false;
    }
    ui_.internetMode = false;
    ui_.tcpPort = host_->port();
    ui_.sessionCode = QString::fromStdString(host_->sessionCode());
    sessionCode_ = host_->sessionCode();
    expectedFps_ = settings.fps;
    abr_ = AdaptiveBitrate(settings.bitrateKbps);
    abrEnabled_ = true;
    statsTimer_.start();
    emit stateChanged();
    return true;
}

bool HostCoordinator::startInternet(const host::HostLaunchSettings& settings,
                                    const QString& serverAddress, uint16_t serverPort, QString* err) {
    stop();
    internetMode_ = true;
    serverHost_ = serverAddress.toStdString();
    serverPort_ = serverPort;
    settings_ = std::make_unique<host::HostLaunchSettings>(settings);
    settings_->audioBitrateKbps = cfg_.audio.bitrateKbps;

    signaling_ = std::make_unique<signaling::SignalingClient>();
    std::string serr;
    if (!signaling_->connect(serverHost_, serverPort_, 6000, &serr)) {
        if (err) *err = QString::fromStdString("Signaling server: " + serr);
        signaling_.reset();
        return false;
    }
    const auto reg = signaling_->hostRegister(settings.hostName.empty() ? "Host" : settings.hostName);
    if (!reg) {
        if (err) *err = "Signaling server rejected the registration.";
        signaling_.reset();
        return false;
    }
    sessionCode_ = reg->code;
    relayTcpPort_ = static_cast<uint16_t>(reg->relayTcpPort);
    relayUdpPort_ = static_cast<uint16_t>(reg->relayUdpPort);
    hostToken_ = reg->hostToken;

    // Session runs over relay pipes; HostApp->HostSession handles them.
    host_ = std::make_unique<host::HostApp>();
    wireHostEvents();

    signaling_->setClientJoinedHandler([this](const signaling::ClientJoined& cj) {
        // Runs on the signaling reader thread -> marshal to Qt thread.
        QMetaObject::invokeMethod(this, [this, cj] {
            logLine(QString("Player %1 (%2) joined via server").arg(cj.playerIndex + 1).arg(QString::fromStdString(cj.name)));
            // Dial this player's relay pipe; the session then sees a new client.
            host_->connectRelayClient(serverHost_, relayTcpPort_, cj.token, cj.playerIndex);
            emit clientTableChanged();
        }, Qt::QueuedConnection);
    });
    signaling_->setClientLeftHandler([this](int playerIndex) {
        QMetaObject::invokeMethod(this, [this, playerIndex] {
            logLine(QString("Player %1 left").arg(playerIndex + 1));
        }, Qt::QueuedConnection);
    });

    ui_.internetMode = true;
    ui_.sessionCode = QString::fromStdString(sessionCode_);
    ui_.serverAddress = serverAddress;
    ui_.tcpPort = 0;
    expectedFps_ = settings.fps;
    abr_ = AdaptiveBitrate(settings.bitrateKbps);
    statsTimer_.start();
    emit stateChanged();
    return true;
}

void HostCoordinator::stop() {
    statsTimer_.stop();
    // 1) Stop media flow first: this joins each streamer's capture/UDP threads
    //    (and waits for any in-flight start() to finish, since start/stop are
    //    serialized inside the streamer).
    for (auto& [id, cs] : streams_) {
        if (cs.streamer) cs.streamer->stop("coordinator stop");
        injector_.clientDisconnected(cs.playerIndex);
    }
    // 2) Join the stream-start workers (no more detached threads).
    for (auto& t : initThreads_) {
        if (t.joinable()) t.join();
    }
    initThreads_.clear();
    streams_.clear();
    if (host_) { host_->stop(); host_.reset(); }
    if (signaling_) { signaling_->disconnect(); signaling_.reset(); }
    ui_ = HostUiState{};
    emit stateChanged();
}

void HostCoordinator::wireHostEvents() {
    host::HostSession::Events ev;
    ev.onLog = [this](log::Level lvl, const std::string& msg) {
        QMetaObject::invokeMethod(this, [this, lvl, msg] {
            emit logLine(QString("[%1] %2").arg(lvl == log::Level::Warning ? "warn" : "info")
                         .arg(QString::fromStdString(msg)));
        }, Qt::QueuedConnection);
    };
    ev.onApprovalRequest = [this](uint32_t clientId, const std::string& name) {
        QMetaObject::invokeMethod(this, [this, clientId, name] {
            emit approvalRequest(clientId, QString::fromStdString(name));
        }, Qt::QueuedConnection);
    };
    ev.onClientUpdated = [this](const host::ClientRow& row) {
        QMetaObject::invokeMethod(this, [this, row] {
            if (row.state == common::ConnectionState::Connected) {
                beginKeyExchange(row.id);
            } else if (row.state == common::ConnectionState::Disconnected) {
                teardownClient(row.id, false);
            }
            emit clientTableChanged();
        }, Qt::QueuedConnection);
    };
    ev.onInputPermissions = [this](uint32_t clientId, const common::InputPermissions perms) {
        QMetaObject::invokeMethod(this, [this, clientId, perms] {
            if (auto* cs = findStream(clientId)) {
                injector_.setPermissions(cs->playerIndex, toProto(perms));
                if (cs->streamer && perms.controller) {
                    injector_.setRumbleSender([this, id = clientId](uint8_t player, uint8_t l, uint8_t r) {
                        if (auto* s = findStream(id)) s->streamer->sendRumble(player, l, r);
                    });
                }
            }
        }, Qt::QueuedConnection);
    };
    // App messages arrive on the session network thread -> marshal.
    ev.onAppMessage = [this](uint32_t clientId, uint16_t type, const std::vector<uint8_t>& payload) {
        QMetaObject::invokeMethod(this, [this, clientId, type, payload] {
            handleAppMessage(clientId, type, payload);
        }, Qt::QueuedConnection);
    };
    host_->setEvents(ev);
}

void HostCoordinator::handleAppMessage(uint32_t clientId, uint16_t type, const std::vector<uint8_t>& payload) {
    const auto env = proto::decodeMessage(type, payload.data(), static_cast<uint32_t>(payload.size()));
    if (!env) return;

    switch (env->type) {
        case Id::KeyExchange: {
            auto* m = std::get_if<proto::msg::KeyExchange>(&env->body);
            if (!m) return;
            auto* cs = findStream(clientId);
            if (!cs || cs->keysReady || !cs->ecdh) return;

            cs->channel = std::make_shared<crypto::SecureChannel>();
            if (!cs->channel->finish(*cs->ecdh, cs->ourSalt, m->publicKey, m->salt, sessionCode_, true)) {
                emit errorOccurred("Key exchange failed for a player; disconnecting them.");
                host_->kick(clientId);
                return;
            }
            // Confirm to the client; wait for their SESSION_KEY_READY.
            proto::Envelope ready;
            ready.type = Id::SessionKeyReady;
            host_->sendAppMessage(clientId, static_cast<uint16_t>(Id::SessionKeyReady),
                             proto::encodeMessage(ready));
            break;
        }
        case Id::SessionKeyReady: {
            auto* cs = findStream(clientId);
            if (!cs || !cs->channel) return;
            cs->keysReady = true;
            startStreamFor(*cs);
            break;
        }
        case Id::StreamStop: {
            auto* cs = findStream(clientId);
            if (cs) {
                if (cs->streamer) cs->streamer->stop("client requested");
                injector_.clientDisconnected(cs->playerIndex);
            }
            break;
        }
        case Id::KeyframeRequest: {
            auto* cs = findStream(clientId);
            if (cs && cs->streamer) cs->streamer->requestKeyframe();
            break;
        }
        default: break;
    }
}

uint8_t HostCoordinator::assignPlayerIndex() {
    bool used[4] = { false, false, false, false };
    for (const auto& [id, cs] : streams_) {
        if (cs.playerIndex < 4) used[cs.playerIndex] = true;
    }
    for (uint8_t i = 0; i < 4; ++i) if (!used[i]) return i;
    return 3;
}

void HostCoordinator::beginKeyExchange(uint32_t clientId) {
    if (findStream(clientId)) return;    // already in flight
    ClientStream cs;
    cs.clientId = clientId;
    cs.playerIndex = assignPlayerIndex();
    cs.ecdh = std::make_unique<crypto::EcdhKeyPair>();
    std::string err;
    auto kp = crypto::generateEcdhP256();
    if (!kp) {
        emit errorOccurred("Cryptography unavailable (CNG). Cannot secure this session.");
        host_->kick(clientId);
        return;
    }
    *cs.ecdh = std::move(*kp);
    cs.ourSalt = crypto::randomBytes(16);

    proto::msg::KeyExchange kx;
    kx.publicKey = cs.ecdh->publicKey;
    kx.salt = cs.ourSalt;
    proto::Envelope env;
    env.type = Id::KeyExchange;
    env.body = std::move(kx);
    host_->sendAppMessage(clientId, static_cast<uint16_t>(Id::KeyExchange),
                             proto::encodeMessage(env));

    streams_[clientId] = std::move(cs);
    logLine(QString("Starting secure channel with player %1...").arg(streams_[clientId].playerIndex + 1));
}

void HostCoordinator::startStreamFor(ClientStream& cs) {
    if (!settings_) return;
    StreamConfig sc;
    sc.codec = settings_->codec;
    sc.width = settings_->resolution.width;
    sc.height = settings_->resolution.height;
    sc.fps = settings_->fps;
    sc.bitrateKbps = settings_->bitrateKbps;
    sc.gop = settings_->fps * 2;
    sc.audioEnabled = settings_->audioEnabled;
    sc.audioBitrateKbps = settings_->audioBitrateKbps;
    sc.captureMode = settings_->captureMode;
    sc.outputIndex = settings_->outputIndex;
    sc.windowHwnd = settings_->windowHwnd;
    sc.gameName = settings_->gameName;

    auto streamer = std::make_shared<HostStreamer>();
    cs.streamer = streamer;
    const uint32_t udpSession = makeUdpSessionId(sessionCode_, cs.clientId);
    const uint8_t player = cs.playerIndex;
    const bool internet = internetMode_;
    const std::string serverHost = serverHost_;
    const uint16_t relayUdp = relayUdpPort_;
    const std::string token = hostToken_;
    const auto cryptoSink = cs.channel;   // installed before bind: no plaintext window
    const auto alive = alive_;
    const uint32_t clientId = cs.clientId;

    cs.streamer->setErrorCallback([this, clientId](const std::string& msg) {
        QMetaObject::invokeMethod(this, [this, clientId, msg] {
            emit encoderTrouble(QString::fromStdString(msg));
            // A streamer that stopped itself (fatal capture/encoder error) is
            // torn down here so the client slot frees up.
            if (auto* cs2 = findStream(clientId)) {
                if (cs2->streamer && !cs2->streamer->running()) {
                    cs2->streamer->stop("fatal pipeline error");
                    injector_.clientDisconnected(cs2->playerIndex);
                }
            }
            (void)clientId;
        }, Qt::QueuedConnection);
    });

    // Heavy init (capture + encoder) off the UI thread. The thread is kept
    // joinable and reaped in stop() — no more detached captures of `this`.
    initThreads_.emplace_back([this, sc, udpSession, player, internet, serverHost, relayUdp, token, clientId, streamer, cryptoSink, alive]() mutable {
        std::string err;
        const bool ok = streamer->start(sc, udpSession, player, cryptoSink,
            [this, alive, clientId, player, sc, streamer, internet, serverHost, relayUdp, token](uint16_t udpPort, uint32_t sid) {
                // Called from the streamer thread before its loop starts.
                if (!alive->load()) return;   // coordinator gone: streamer is stopped by owner
                proto::msg::StreamStart ss;
                ss.udpPort = udpPort;
                ss.sessionId = sid;
                ss.playerIndex = player;
                ss.codec = static_cast<uint8_t>(sc.codec);
                ss.width = static_cast<uint16_t>(sc.width);
                ss.height = static_cast<uint16_t>(sc.height);
                ss.fps = static_cast<uint8_t>(sc.fps);
                ss.bitrateKbps = static_cast<uint32_t>(sc.bitrateKbps);
                proto::Envelope env;
                env.type = Id::StreamStart;
                env.body = std::move(ss);
                // Thread-safe (posts to session io).
                host_->sendAppMessage(clientId, static_cast<uint16_t>(Id::StreamStart),
                                      proto::encodeMessage(env));
                if (internet) {
                    streamer->setClientEndpoint(serverHost, relayUdp);
                    asio::ip::udp::endpoint relay(asio::ip::make_address(serverHost), relayUdp);
                    streamer->udpTransport().setRelayFallback(relay);
                    streamer->udpTransport().sendRelayBind(token);
                }
                (void)sc;
                QMetaObject::invokeMethod(this, [this] { emit stateChanged(); }, Qt::QueuedConnection);
            }, &err);
        if (!alive->load()) return;
        QMetaObject::invokeMethod(this, [this, ok, err, clientId, player] {
            if (!ok) {
                emit errorOccurred(QString("Stream start failed: %1").arg(QString::fromStdString(err)));
                host_->kick(clientId);
            } else {
                // Route input datagrams into the injector (permission-checked).
                // NOTE: `player` is captured by value — this handler runs on the
                // UDP io thread and must not touch the streams_ map.
                if (auto* cs3 = findStream(clientId)) {
                    if (cs3->streamer) {
                        cs3->streamer->setInputHandler(
                            [this, player](const net::AssembledFrame& f) {
                                injector_.applyInput(player, f.data.data(), f.data.size());
                            });
                    }
                }
                emit stateChanged();
            }
        }, Qt::QueuedConnection);
    });

    ui_.gamepadStatus = QString::fromStdString(injector_.gamepadStatus());
}

uint8_t HostCoordinator::playerIndexOf(uint32_t clientId) {
    if (auto* cs = findStream(clientId)) return cs->playerIndex;
    return 0;
}

void HostCoordinator::teardownClient(uint32_t clientId, bool /*kick*/) {
    if (auto it = streams_.find(clientId); it != streams_.end()) {
        if (it->second.streamer) it->second.streamer->stop("client disconnected");
        injector_.clientDisconnected(it->second.playerIndex);
        streams_.erase(it);
        emit stateChanged();
    }
}

HostCoordinator::ClientStream* HostCoordinator::findStream(uint32_t clientId) {
    auto it = streams_.find(clientId);
    return it == streams_.end() ? nullptr : &it->second;
}

std::vector<host::ClientRow> HostCoordinator::clients() const {
    return host_ ? host_->clients() : std::vector<host::ClientRow>{};
}

void HostCoordinator::approve(uint32_t clientId, bool accept) { if (host_) host_->approve(clientId, accept); }
void HostCoordinator::kick(uint32_t clientId) {
    if (host_) {
        teardownClient(clientId, true);
        host_->kick(clientId);
    }
}
void HostCoordinator::setClientInput(uint32_t clientId, bool enabled) {
    if (host_) host_->setClientInputEnabled(clientId, enabled);
}
void HostCoordinator::setManualBitrate(int kbps) {
    abr_.setManual(kbps);
    abrEnabled_ = false;
    for (auto& [id, cs] : streams_) if (cs.streamer) cs.streamer->setBitrate(kbps);
    ui_.bitrateTarget = kbps;
    emit stateChanged();
}

void HostCoordinator::restartEncoders() {
    for (auto& [id, cs] : streams_) {
        if (cs.streamer) cs.streamer->requestEncoderRestart();   // swap happens on the capture thread
    }
    emit stateChanged();
}

void HostCoordinator::switchEncodersToSoftware() {
    for (auto& [id, cs] : streams_) {
        if (cs.streamer) cs.streamer->requestSoftwareEncoder();
    }
    emit stateChanged();
}

void HostCoordinator::onStatsTimer() {
    if (!host_) return;
    // Aggregate first streamer's stats + ABR decision.
    double rtt = 0, lossPct = 0, jitter = 0;
    bool any = false;
    for (auto& [id, cs] : streams_) {
        if (!cs.streamer) continue;
        any = true;
        const auto st = cs.streamer->stats();
        rtt = st.udp.avgRttMs;
        jitter = st.udp.jitterMs;
        const uint64_t lost = st.udp.lostFrames;
        const uint64_t assembled = st.udp.assembledFrames;
        lossPct = (lost + assembled) > 0 ? (double(lost) / double(lost + assembled)) * 100.0 : 0.0;
        ui_.fps = st.fps;
        ui_.captureMs = st.captureMs;
        ui_.encodeMs = st.encodeMs;
        ui_.bitrateKbps = st.actualBitrateKbps;
        if (ui_.encoderName.isEmpty()) ui_.encoderName = QString::fromStdString(st.encoderName);
    }

    if (any && abrEnabled_) {
        media::LinkSample s;
        s.rttMs = rtt;
        s.lossPercent = lossPct;
        s.jitterMs = jitter;
        s.decodeQueueFrames = 0;   // client-side info not fed back in v1 (documented)
        s.fpsRatio = expectedFps_ > 0 ? ui_.fps / expectedFps_ : 1.0;
        const auto next = abr_.update(s);
        if (next) {
            for (auto& [id, cs] : streams_) if (cs.streamer) cs.streamer->setBitrate(*next);
        }
    }
    ui_.rttMs = rtt;
    ui_.lossPercent = lossPct;
    ui_.jitterMs = jitter;
    ui_.bitrateTarget = abr_.currentKbps();
    ui_.abrActive = abrEnabled_;
    emit stateChanged();
}

} // namespace rp::app
