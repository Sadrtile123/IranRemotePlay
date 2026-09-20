#pragma once
// Phase 16 — client-side application orchestration.
//
// Bridges: ClientSession (TCP handshake) + SecureChannel (ECDH/AES-GCM) +
// ClientStreamer (decode/render) + InputSender (pads/kb/mouse) +
// VideoWidget (presentation, marshaled to the UI thread) + SignalingClient
// (join by code, relay media endpoint).

#include <vector>
#include "../adapt/AdaptiveBitrate.h"
#include "../client/ClientApp.h"
#include "../client/ClientStreamer.h"

#include "../common/Config.h"
#include "../input/InputSender.h"
#include "../security/SecureChannel.h"
#include "../signaling/SignalingClient.h"
#include "../ui/VideoWidget.h"

#include <QObject>
#include <QString>
#include <QTimer>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace rp::app {

struct ClientUiState {
    QString status;
    QString hostName;
    QString gameName;
    QString decoderName;
    double rttMs = 0, lossPercent = 0, jitterMs = 0;
    double fps = 0, decodeMs = 0, audioMs = 0;
    double bitrateKbps = 0;
    int playerIndex = 0;
    bool audioActive = false;
    int width = 0, height = 0;
};

class ClientCoordinator : public QObject {
    Q_OBJECT
public:
    explicit ClientCoordinator(config::Config& cfg, QObject* parent = nullptr);
    ~ClientCoordinator() override;

    // LAN/direct: host address + port + session code.
    [[nodiscard]] bool startLan(const QString& hostAddress, uint16_t port, const QString& playerName,
                                const QString& sessionCode, QString* err = nullptr);

    // Internet: join by code through the signaling server.
    [[nodiscard]] bool startInternet(const QString& serverAddress, uint16_t serverPort,
                                     const QString& sessionCode, const QString& playerName,
                                     QString* err = nullptr);

    void stop();
    [[nodiscard]] bool active() const;

    // Video presentation target (the VideoWidget). Must be set before start.
    void setVideoWidget(rp::ui::VideoWidget* w);
    void setKeyboardMouseEnabled(bool kb, bool mouse);

    [[nodiscard]] ClientUiState uiState() const { return ui_; }

signals:
    void stateChanged();
    void logLine(const QString& line);
    void disconnected(const QString& reason);
    void errorOccurred(const QString& message);
    void connectedToHost(const QString& hostName, const QString& gameName, int playerIndex);

private slots:
    void onStatsTimer();
    void onStreamStartWatchdog();

private:
    void wireSessionEvents();
    void handleAppMessage(uint16_t type, const std::vector<uint8_t>& payload);
    void beginKeyExchange();
    void startStream(uint16_t udpPort, uint32_t udpSessionId);
    void wireInputForwarding();
    bool inputWired_ = false;         // guard against duplicate connections on stream restart

    config::Config& cfg_;
    std::unique_ptr<client::ClientApp> client_;
    std::unique_ptr<client::ClientSessionParams> params_;
    std::unique_ptr<signaling::SignalingClient> signaling_;
    std::shared_ptr<crypto::SecureChannel> channel_;
    std::unique_ptr<crypto::EcdhKeyPair> ecdh_;
    std::vector<uint8_t> ourSalt_;
    bool keysReady_ = false;
    bool internetMode_ = false;
    std::string serverHost_;
    uint16_t relayUdpPort_ = 0;
    std::string playerToken_;
    std::string sessionCode_;
    std::string hostAddress_;

    std::shared_ptr<ClientStreamer> streamer_;
    std::thread streamThread_;                    // stream-start worker (joined in stop())
    std::unique_ptr<input::InputSender> inputSender_;
    ui::VideoWidget* videoWidget_ = nullptr;    // not owned

    QTimer statsTimer_;
    QTimer streamStartWatchdog_;                 // "host never sent STREAM_START" detector
    ClientUiState ui_;
    uint64_t lastRecvBytes_ = 0;
    std::shared_ptr<std::atomic<bool>> alive_;   // detached-thread lifetime guard
    common::VideoCodec negotiatedCodec_ = common::VideoCodec::H264;
};

} // namespace rp::app
