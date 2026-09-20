#pragma once
// Phase 16 — host-side application orchestration.
//
// Bridges: HostApp (TCP session) + HostStreamer (media pipeline) +
// SecureChannel (ECDH/AES-GCM) + InputInjector (ViGEm/SendInput) +
// AdaptiveBitrate + SignalingClient (internet mode).
//
// Per-client flow (LAN or internet):
//   Connected -> KEY_EXCHANGE -> derive keys -> SESSION_KEY_READY ->
//   start streamer -> STREAM_START(udpPort, sessionId, playerIndex)
// Media + input then flow over UDP, AES-256-GCM sealed.

#include "../adapt/AdaptiveBitrate.h"
#include "../common/Config.h"
#include "../common/Types.h"

#include "../host/HostApp.h"
#include "../host/HostStreamer.h"
#include "../input/InputInjector.h"
#include "../security/SecureChannel.h"
#include "../signaling/SignalingClient.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTimer>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rp::app {

using media::AdaptiveBitrate;

struct HostUiState {
    QString sessionCode;
    QString gameName;
    QString encoderName;
    QString gamepadStatus;
    uint16_t tcpPort = 0;
    bool internetMode = false;
    QString serverAddress;
    // aggregated per-second stats of the first active streamer
    double fps = 0, captureMs = 0, encodeMs = 0, bitrateKbps = 0;
    double rttMs = 0, lossPercent = 0, jitterMs = 0;
    int bitrateTarget = 0;
    bool abrActive = true;
};

class HostCoordinator : public QObject {
    Q_OBJECT
public:
    explicit HostCoordinator(config::Config& cfg, QObject* parent = nullptr);
    ~HostCoordinator() override;

    // LAN mode: HostApp listens; session code shown to friends.
    [[nodiscard]] bool startLan(const host::HostLaunchSettings& settings, QString* err = nullptr);

    // Internet mode: register with the signaling server; clients join by code
    // and arrive via the relay (no port forwarding needed).
    [[nodiscard]] bool startInternet(const host::HostLaunchSettings& settings,
                                     const QString& serverAddress, uint16_t serverPort,
                                     QString* err = nullptr);

    void stop();

    [[nodiscard]] HostUiState uiState() const { return ui_; }
    [[nodiscard]] std::vector<host::ClientRow> clients() const;
    void approve(uint32_t clientId, bool accept);
    void kick(uint32_t clientId);
    void setClientInput(uint32_t clientId, bool enabled);
    void setManualBitrate(int kbps);
    void setAbrEnabled(bool on) { abrEnabled_ = on; }
    void restartEncoders();          // recovery dialog action
    void switchEncodersToSoftware(); // recovery dialog action

    [[nodiscard]] bool hosting() const { return host_ && host_->hosting(); }

signals:
    // Marshaled to the Qt main thread.
    void stateChanged();
    void logLine(const QString& line);
    void clientTableChanged();
    void approvalRequest(uint32_t clientId, const QString& name);
    void errorOccurred(const QString& message);
    void encoderTrouble(const QString& message);   // recovery dialog trigger

private slots:
    void onStatsTimer();

private:
    struct ClientStream {
        uint32_t clientId = 0;
        uint8_t playerIndex = 0;
        std::string name;
        bool keysReady = false;
        std::unique_ptr<crypto::EcdhKeyPair> ecdh;
        std::vector<uint8_t> ourSalt;
        std::shared_ptr<crypto::SecureChannel> channel;
        std::shared_ptr<HostStreamer> streamer;
    };

    // Helpers (run on Qt thread; internal callbacks marshal here).
    void wireHostEvents();
    void handleAppMessage(uint32_t clientId, uint16_t type, const std::vector<uint8_t>& payload);
    void beginKeyExchange(uint32_t clientId);
    void startStreamFor(ClientStream& cs);
    void teardownClient(uint32_t clientId, bool kick);
    [[nodiscard]] ClientStream* findStream(uint32_t clientId);
    [[nodiscard]] uint8_t playerIndexOf(uint32_t clientId);
    uint8_t assignPlayerIndex();

    config::Config& cfg_;
    std::unique_ptr<host::HostApp> host_;
    std::unique_ptr<host::HostLaunchSettings> settings_;
    std::unique_ptr<signaling::SignalingClient> signaling_;
    std::string serverHost_;
    uint16_t serverPort_ = 0;
    uint16_t relayTcpPort_ = 0;
    uint16_t relayUdpPort_ = 0;
    std::string hostToken_;
    bool internetMode_ = false;
    std::string sessionCode_;       // also used as crypto binding string

    input::InputInjector injector_;
    std::map<uint32_t, ClientStream> streams_;
    std::vector<std::thread> initThreads_;   // stream-start workers; joined in stop()
    QTimer statsTimer_;
    AdaptiveBitrate abr_;
    bool abrEnabled_ = true;
    double expectedFps_ = 60.0;

    HostUiState ui_;
    std::shared_ptr<std::atomic<bool>> alive_;   // background-thread lifetime guard
};

} // namespace rp::app
