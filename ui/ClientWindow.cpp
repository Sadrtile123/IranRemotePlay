// Phase 16 — client window implementation. See ClientWindow.h.

#include "ClientWindow.h"
#include "../app/ClientCoordinator.h"
#include "Theme.h"

#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QStackedLayout>
#include <QVBoxLayout>

namespace rp::ui {

using app::ClientCoordinator;

ClientWindow::ClientWindow(config::Config& cfg, QWidget* parent) : QWidget(parent), cfg_(cfg) {
    coordinator_ = new ClientCoordinator(cfg_, this);
    buildUi();

    connect(coordinator_, &ClientCoordinator::logLine, this, [this](const QString& l) {
        infoLabel_->setText(l.left(140));
    });
    connect(coordinator_, &ClientCoordinator::stateChanged, this, &ClientWindow::onStateChanged);
    connect(coordinator_, &ClientCoordinator::disconnected, this, &ClientWindow::onDisconnected);
    connect(coordinator_, &ClientCoordinator::errorOccurred, this, [this](const QString& msg) {
        joinStatusLabel_->setText(msg);
    });
    connect(coordinator_, &ClientCoordinator::connectedToHost, this, &ClientWindow::onConnectedToHost);
}

ClientWindow::~ClientWindow() { disconnectIfActive(); }

void ClientWindow::buildUi() {
    auto* rootLayout = new QVBoxLayout(this);

    // ---------------- join form ----------------
    joinForm_ = new QWidget;
    auto* box = new QGroupBox(tr("Join a session"));
    auto* form = new QVBoxLayout(box);

    form->addWidget(new QLabel(tr("Your name:")));
    nameEdit_ = new QLineEdit(QString::fromStdString(cfg_.profile.name));
    form->addWidget(nameEdit_);

    form->addWidget(new QLabel(tr("Connection mode:")));
    modeCombo_ = new QComboBox;
    modeCombo_->addItem(tr("LAN / direct (host IP)"));
    modeCombo_->addItem(tr("Internet (signaling server)"));
    form->addWidget(modeCombo_);

    form->addWidget(new QLabel(tr("Host / server address:")));
    hostEdit_ = new QLineEdit;
    hostEdit_->setPlaceholderText("192.168.1.20  or  play.example.com");
    form->addWidget(hostEdit_);

    form->addWidget(new QLabel(tr("Session code:")));
    codeEdit_ = new QLineEdit;
    codeEdit_->setPlaceholderText("e.g. 57EA6A6Q");
    form->addWidget(codeEdit_);

    joinButton_ = new QPushButton(tr("Join"));
    joinButton_->setObjectName("primary");
    connect(joinButton_, &QPushButton::clicked, this, &ClientWindow::join);
    form->addWidget(joinButton_);

    joinStatusLabel_ = new QLabel;
    joinStatusLabel_->setWordWrap(true);
    form->addWidget(joinStatusLabel_);

    backButton_ = new QPushButton(tr("Back"));
    connect(backButton_, &QPushButton::clicked, this, [this] {
        disconnectIfActive();
        emit backToHomeRequested();
    });
    form->addWidget(backButton_);

    auto* wrap = new QHBoxLayout;
    wrap->addStretch(1);
    wrap->addWidget(box);
    wrap->addStretch(1);
    joinForm_->setLayout(wrap);
    rootLayout->addWidget(joinForm_, 1);

    // ---------------- stream view ----------------
    video_ = new VideoWidget;
    video_->setStatsProvider([this] { return statsLines(); });
    coordinator_->setVideoWidget(video_);
    rootLayout->addWidget(video_, 1);
    video_->setVisible(false);

    auto* bottomBar = new QWidget;
    auto* bar = new QHBoxLayout(bottomBar);
    infoLabel_ = new QLabel;
    infoLabel_->setWordWrap(true);
    bar->addWidget(infoLabel_, 1);
    leaveButton_ = new QPushButton(tr("Disconnect"));
    connect(leaveButton_, &QPushButton::clicked, this, &ClientWindow::leave);
    bar->addWidget(leaveButton_);
    rootLayout->addWidget(bottomBar);
    bottomBar->setVisible(false);

    setLayout(rootLayout);
}

std::vector<QString> ClientWindow::statsLines() const {
    const auto s = coordinator_->uiState();
    return {
        QString("Player %1 on %2's session").arg(s.playerIndex).arg(s.hostName.isEmpty() ? "?" : s.hostName),
        QString("RTT %1 ms | loss %2% | jitter %3 ms").arg(s.rttMs, 0, 'f', 0).arg(s.lossPercent, 0, 'f', 1).arg(s.jitterMs, 0, 'f', 1),
        QString("In %1 Mbps | FPS %2 | decode %3 ms").arg(s.bitrateKbps / 1000.0, 0, 'f', 1).arg(s.fps, 0, 'f', 0).arg(s.decodeMs, 0, 'f', 1),
        QString("Video %1x%2 (%3)").arg(s.width).arg(s.height).arg(s.decoderName.isEmpty() ? "-" : s.decoderName),
        QString("Audio %1").arg(s.audioActive ? QString("on (%1 ms dec)").arg(s.audioMs, 0, 'f', 1) : "off"),
    };
}

void ClientWindow::join() {
    const QString code = codeEdit_->text().trimmed();
    const QString host = hostEdit_->text().trimmed();
    const QString name = nameEdit_->text().trimmed().isEmpty() ? "Player" : nameEdit_->text().trimmed();
    if (code.isEmpty() || host.isEmpty()) {
        joinStatusLabel_->setText(tr("Enter the host/server address and the session code."));
        return;
    }
    joinStatusLabel_->setText(tr("Connecting..."));

    QString err;
    const bool ok = modeCombo_->currentIndex() == 1
        ? coordinator_->startInternet(host, 9000, code, name, &err)
        : coordinator_->startLan(host, cfg_.network.listenPort, name, code, &err);
    if (!ok) {
        joinStatusLabel_->setText(err.isEmpty() ? tr("Could not connect.") : err);
        return;
    }
    setStreamingUi(true);
}

void ClientWindow::leave() {
    coordinator_->stop();
    setStreamingUi(false);
}

void ClientWindow::disconnectIfActive() {
    if (coordinator_ && coordinator_->active()) {
        coordinator_->stop();
        setStreamingUi(false);
    }
}

void ClientWindow::setStreamingUi(bool streaming) {
    joinForm_->setVisible(!streaming);
    video_->setVisible(streaming);
    leaveButton_->parentWidget()->setVisible(streaming);
    if (streaming) {
        video_->setFocus();
        video_->setQualityBanner(tr("Connecting to stream..."));
    } else {
        video_->setQualityBanner("");
    }
}

void ClientWindow::onStateChanged() {
    const auto s = coordinator_->uiState();
    infoLabel_->setText(s.status);
    updateQualityBanner();
}

void ClientWindow::onConnectedToHost(const QString& hostName, const QString& gameName, int playerIndex) {
    infoLabel_->setText(tr("Connected to %1 - %2 (you are Player %3)")
                            .arg(hostName, gameName.isEmpty() ? "?" : gameName).arg(playerIndex));
}

void ClientWindow::onDisconnected(const QString& reason) {
    setStreamingUi(false);
    joinStatusLabel_->setText(tr("Disconnected: %1").arg(reason));
}

void ClientWindow::updateQualityBanner() {
    const auto s = coordinator_->uiState();
    QString banner;
    if (s.lossPercent > 5.0) banner = tr("Poor connection - packet loss %1%").arg(s.lossPercent, 0, 'f', 1);
    else if (s.rttMs > 150.0) banner = tr("High latency - %1 ms").arg(s.rttMs, 0, 'f', 0);
    else if (s.fps > 0 && s.fps < 25.0) banner = tr("Low framerate - %1 FPS").arg(s.fps, 0, 'f', 0);
    video_->setQualityBanner(banner);
}

} // namespace rp::ui
