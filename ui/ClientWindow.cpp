// RemotePlay - ui/ClientWindow.cpp
// v0.1.1 — join card + recent hosts + real fullscreen (F11) + host:port
// parsing + live status. See ClientWindow.h.

#include "ClientWindow.h"
#include "../app/ClientCoordinator.h"
#include "Theme.h"

#include <QComboBox>
#include <QDateTime>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QShortcut>
#include <QTimer>
#include <QVBoxLayout>

namespace rp::ui {

using app::ClientCoordinator;

namespace {
constexpr int kMaxRecentHosts = 8;
}

ClientWindow::ClientWindow(config::Config& cfg, QWidget* parent) : QWidget(parent), cfg_(cfg) {
    coordinator_ = new ClientCoordinator(cfg_, this);
    buildUi();

    connect(coordinator_, &ClientCoordinator::logLine, this, [this](const QString& l) {
        infoLabel_->setText(l.left(160));
    });
    connect(coordinator_, &ClientCoordinator::stateChanged, this, &ClientWindow::onStateChanged);
    connect(coordinator_, &ClientCoordinator::disconnected, this, &ClientWindow::onDisconnected);
    connect(coordinator_, &ClientCoordinator::errorOccurred, this, &ClientWindow::showError);
    connect(coordinator_, &ClientCoordinator::connectedToHost, this, &ClientWindow::onConnectedToHost);

    // Fullscreen hotkey at WINDOW level: works before the video widget gains
    // focus and even while the join form is up. This was the "F11 does
    // nothing" bug: the toggle was only wired after the stream started AND
    // targeted a child widget.
    // NOTE: Escape is deliberately NOT a window shortcut - it must stay a
    // game key. It exits fullscreen via VideoWidget::escapePressed instead
    // (only consumed while fullscreen).
    auto* f11 = new QShortcut(QKeySequence(Qt::Key_F11), this);
    connect(f11, &QShortcut::activated, this, &ClientWindow::toggleFullscreen);
    connect(video_, &VideoWidget::escapePressed, this, [this] {
        if (videoFullscreen_) setVideoFullscreen(false);
    });
    // Double-click + F11 signals from the video surface: toggle regardless of
    // the requested direction (the child cannot know the window state).
    connect(video_, &VideoWidget::toggleFullscreenRequested, this, [this](bool) {
        toggleFullscreen();
    });
}

ClientWindow::~ClientWindow() { disconnectIfActive(); }

void ClientWindow::buildUi() {
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(18, 14, 18, 14);
    rootLayout->setSpacing(12);

    // ---------------- join form ----------------
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    joinForm_ = new QWidget;
    joinForm_->setObjectName(QStringLiteral("joinPage"));

    auto* headerRow = new QHBoxLayout;
    auto* title = new QLabel(tr("Join a session"));
    title->setFont(headerFont(16));
    backButton_ = new QPushButton(tr("Back"));
    connect(backButton_, &QPushButton::clicked, this, [this] {
        disconnectIfActive();
        emit backToHomeRequested();
    });
    headerRow->addWidget(title);
    headerRow->addStretch(1);
    headerRow->addWidget(backButton_);

    auto* box = new QGroupBox(tr("CONNECT"));
    auto* form = new QVBoxLayout(box);
    form->setSpacing(8);

    form->addWidget(new QLabel(tr("Your name:")));
    nameEdit_ = new QLineEdit(QString::fromStdString(cfg_.profile.name));
    if (nameEdit_->text().isEmpty()) nameEdit_->setText(QStringLiteral("Player"));
    form->addWidget(nameEdit_);

    form->addWidget(new QLabel(tr("Connection mode:")));
    modeCombo_ = new QComboBox;
    modeCombo_->addItem(tr("LAN / direct (host IP)"));
    modeCombo_->addItem(tr("Internet (signaling server)"));
    form->addWidget(modeCombo_);

    form->addWidget(new QLabel(tr("Host / server address:")));
    hostCombo_ = new QComboBox;
    hostCombo_->setEditable(true);
    hostCombo_->setInsertPolicy(QComboBox::NoInsert);
    hostCombo_->lineEdit()->setPlaceholderText(
        tr("192.168.1.20  or  192.168.1.20:%1  or  play.example.com").arg(cfg_.network.listenPort));
    loadRecentHosts();
    form->addWidget(hostCombo_);

    form->addWidget(new QLabel(tr("Session code:")));
    codeEdit_ = new QLineEdit;
    codeEdit_->setPlaceholderText(tr("e.g. 57EA6A6Q"));
    codeEdit_->setMaxLength(16);
    form->addWidget(codeEdit_);

    joinButton_ = new QPushButton(tr("Join"));
    joinButton_->setObjectName("primary");
    connect(joinButton_, &QPushButton::clicked, this, &ClientWindow::join);
    form->addWidget(joinButton_);

    joinStatusLabel_ = new QLabel;
    joinStatusLabel_->setObjectName(QStringLiteral("joinStatus"));
    joinStatusLabel_->setWordWrap(true);
    form->addWidget(joinStatusLabel_);

    auto* hint = new QLabel(tr("While streaming: F11 fullscreen - F10 stats overlay - Esc exits fullscreen."), box);
    hint->setProperty("muted", true);
    hint->setWordWrap(true);
    form->addWidget(hint);

    auto* wrap = new QVBoxLayout(joinForm_);
    wrap->setContentsMargins(0, 0, 6, 0);
    wrap->setSpacing(12);
    wrap->addLayout(headerRow);
    auto* center = new QHBoxLayout;
    center->addStretch(1);
    center->addWidget(box, 2);
    center->addStretch(1);
    wrap->addLayout(center);
    wrap->addStretch(1);
    scroll->setWidget(joinForm_);
    rootLayout->addWidget(scroll, 1);

    // ---------------- stream view ----------------
    video_ = new VideoWidget;
    video_->setStatsProvider([this] { return statsLines(); });
    coordinator_->setVideoWidget(video_);
    rootLayout->addWidget(video_, 1);
    video_->setVisible(false);

    bottomBar_ = new QWidget;
    auto* bar = new QHBoxLayout(bottomBar_);
    bar->setContentsMargins(0, 0, 0, 0);
    bar->setSpacing(10);
    infoLabel_ = new QLabel;
    infoLabel_->setWordWrap(true);
    infoLabel_->setProperty("muted", true);
    bar->addWidget(infoLabel_, 1);
    fullscreenButton_ = new QPushButton(tr("Fullscreen (F11)"));
    connect(fullscreenButton_, &QPushButton::clicked, this, &ClientWindow::toggleFullscreen);
    bar->addWidget(fullscreenButton_);
    leaveButton_ = new QPushButton(tr("Disconnect"));
    leaveButton_->setObjectName("danger");
    connect(leaveButton_, &QPushButton::clicked, this, &ClientWindow::leave);
    bar->addWidget(leaveButton_);
    rootLayout->addWidget(bottomBar_);
    bottomBar_->setVisible(false);

    setLayout(rootLayout);
}

void ClientWindow::loadRecentHosts() {
    QSettings s(QStringLiteral("RemotePlay"), QStringLiteral("RemotePlay"));
    const QStringList hosts = s.value(QStringLiteral("recentHosts")).toStringList();
    hostCombo_->clear();
    hostCombo_->addItems(hosts);
}

void ClientWindow::rememberHost(const QString& host) {
    QSettings s(QStringLiteral("RemotePlay"), QStringLiteral("RemotePlay"));
    QStringList hosts = s.value(QStringLiteral("recentHosts")).toStringList();
    hosts.removeAll(host);
    hosts.prepend(host);
    while (hosts.size() > kMaxRecentHosts) hosts.removeLast();
    s.setValue(QStringLiteral("recentHosts"), hosts);
    loadRecentHosts();
    hostCombo_->setCurrentText(host);
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
    const QString hostRaw = hostCombo_->currentText().trimmed();
    const QString name = nameEdit_->text().trimmed().isEmpty() ? "Player" : nameEdit_->text().trimmed();
    if (code.isEmpty() || hostRaw.isEmpty()) {
        joinStatusLabel_->setText(tr("Enter the host/server address and the session code."));
        return;
    }

    // Address may be "host", "host:port" (IPv4), or "[v6]:port". Default port
    // is the configured listen port — previously the client silently used its
    // own listenPort setting with no way to specify another one.
    QString host = hostRaw;
    uint16_t port = static_cast<uint16_t>(cfg_.network.listenPort);
    if (hostRaw.startsWith('[')) {                       // [v6]:port
        const int close = hostRaw.indexOf(']');
        if (close > 0) {
            host = hostRaw.left(close + 1);              // keep brackets for resolver? strip below
            host = hostRaw.mid(1, close - 1);
            if (hostRaw.size() > close + 2 && hostRaw.at(close + 1) == ':') {
                port = static_cast<uint16_t>(hostRaw.mid(close + 2).toUShort());
            }
        }
    } else {
        const int colon = hostRaw.lastIndexOf(':');
        if (colon > 0 && hostRaw.indexOf(':') == colon) { // exactly one colon -> host:port
            const QString portPart = hostRaw.mid(colon + 1);
            if (!portPart.isEmpty() && portPart.toInt() > 0 && portPart.toInt() <= 65535) {
                host = hostRaw.left(colon);
                port = static_cast<uint16_t>(portPart.toUShort());
            }
        }
    }

    rememberHost(hostRaw);

    joinStatusLabel_->setText(tr("Connecting..."));

    QString err;
    const bool ok = modeCombo_->currentIndex() == 1
        ? coordinator_->startInternet(host, 9000, code, name, &err)
        : coordinator_->startLan(host, port, name, code, &err);
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
    if (!streaming && videoFullscreen_) setVideoFullscreen(false);
    joinForm_->setVisible(!streaming);
    video_->setVisible(streaming);
    bottomBar_->setVisible(streaming);
    if (streaming) {
        video_->setFocus();
        video_->setQualityBanner(tr("Connecting to stream..."));
        infoLabel_->setText(tr("Connecting..."));
    } else {
        video_->setQualityBanner("");
    }
}

void ClientWindow::setVideoFullscreen(bool fullscreen) {
    if (videoFullscreen_ == fullscreen) return;
    videoFullscreen_ = fullscreen;
    video_->setFullscreenActive(fullscreen);   // fullscreen Esc is consumed; otherwise it's a game key
    // Fullscreen the WHOLE top-level window and hide the bar: calling
    // showFullScreen() on the child video widget alone does not produce a
    // real fullscreen window.
    QWidget* win = window();
    if (fullscreen) {
        win->showFullScreen();
        bottomBar_->setVisible(false);
        fullscreenButton_->setText(tr("Exit fullscreen (F11)"));
    } else {
        win->showNormal();
        if (video_->isVisible()) bottomBar_->setVisible(true);
        fullscreenButton_->setText(tr("Fullscreen (F11)"));
    }
}

void ClientWindow::toggleFullscreen() {
    setVideoFullscreen(!videoFullscreen_);
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

void ClientWindow::showError(const QString& message) {
    if (message.isEmpty()) return;
    joinStatusLabel_->setText(message);
    joinStatusLabel_->setStyleSheet(QStringLiteral("color:#ff8080;"));
    QTimer::singleShot(8000, this, [this] {
        joinStatusLabel_->setStyleSheet(QString());
    });
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
