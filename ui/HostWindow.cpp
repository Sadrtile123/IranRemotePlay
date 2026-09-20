// Phase 16 — host window implementation. See HostWindow.h.

#include "HostWindow.h"
#include "../app/HostCoordinator.h"
#include "../capture/ScreenCapture.h"
#include "../common/Config.h"
#include "Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTableWidget>
#include <QTextStream>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>

namespace rp::ui {

using app::HostCoordinator;
using host::HostLaunchSettings;

HostWindow::HostWindow(config::Config& cfg, QWidget* parent) : QWidget(parent), cfg_(cfg) {
    coordinator_ = new HostCoordinator(cfg_, this);
    buildUi();

    connect(coordinator_, &HostCoordinator::logLine, this, &HostWindow::appendLog);
    connect(coordinator_, &HostCoordinator::approvalRequest, this, &HostWindow::onApprovalRequest);
    connect(coordinator_, &HostCoordinator::encoderTrouble, this, &HostWindow::onEncoderTrouble);
    connect(coordinator_, &HostCoordinator::errorOccurred, this, [](const QString& msg) {
        QMessageBox::warning(nullptr, "RemotePlay", msg);
    });
    connect(coordinator_, &HostCoordinator::stateChanged, this, &HostWindow::onStateChanged);
    connect(coordinator_, &HostCoordinator::clientTableChanged, this, &HostWindow::refreshPlayerTable);

    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(1000);
    connect(refreshTimer_, &QTimer::timeout, this, &HostWindow::refreshPlayerTable);
    refreshTimer_->start();

    refreshCaptureSources();
}

HostWindow::~HostWindow() { stopIfHosting(); }

void HostWindow::buildUi() {
    auto* root = new QHBoxLayout(this);

    // ---------------- left: setup ----------------
    auto* setupBox = new QGroupBox(tr("Host a session"));
    auto* setupLay = new QVBoxLayout(setupBox);

    setupLay->addWidget(new QLabel(tr("Your name:")));
    auto* nameEdit = new QLineEdit(QString::fromStdString(cfg_.profile.name));
    setupLay->addWidget(nameEdit);

    setupLay->addWidget(new QLabel(tr("Connection mode:")));
    modeCombo_ = new QComboBox;
    modeCombo_->addItem(tr("LAN / direct (IP + code)"));
    modeCombo_->addItem(tr("Internet (signaling server, no port forwarding)"));
    setupLay->addWidget(modeCombo_);
    serverEdit_ = new QLineEdit("play.example.com");
    serverEdit_->setPlaceholderText("signaling server address");
    serverEdit_->setVisible(false);
    setupLay->addWidget(serverEdit_);
    connect(modeCombo_, &QComboBox::currentIndexChanged, this, [this](int idx) {
        serverEdit_->setVisible(idx == 1);
    });

    setupLay->addWidget(new QLabel(tr("Game:")));
    gameEdit_ = new QLineEdit("Rayman Legends");
    setupLay->addWidget(gameEdit_);

    setupLay->addWidget(new QLabel(tr("Capture source:")));
    sourceCombo_ = new QComboBox;
    setupLay->addWidget(sourceCombo_);
    auto* rescanButton = new QPushButton(tr("Rescan windows"));
    connect(rescanButton, &QPushButton::clicked, this, &HostWindow::refreshCaptureSources);
    setupLay->addWidget(rescanButton);

    auto* resRow = new QHBoxLayout;
    resolutionCombo_ = new QComboBox;
    for (const char* r : { "1280x720", "1920x1080", "2560x1440", "3840x2160" }) {
        resolutionCombo_->addItem(r);
    }
    resolutionCombo_->setCurrentIndex(1);
    fpsCombo_ = new QComboBox;
    fpsCombo_->addItem("30");
    fpsCombo_->addItem("60");
    fpsCombo_->addItem("120");
    fpsCombo_->setCurrentIndex(1);
    resRow->addWidget(resolutionCombo_);
    resRow->addWidget(fpsCombo_);
    setupLay->addLayout(resRow);

    codecCombo_ = new QComboBox;
    codecCombo_->addItem(tr("H.264 (best compatibility)"));
    codecCombo_->addItem(tr("H.265 / HEVC"));
    setupLay->addWidget(codecCombo_);

    auto* brRow = new QHBoxLayout;
    bitrateSpin_ = new QSpinBox;
    bitrateSpin_->setRange(2, 50);
    bitrateSpin_->setValue(cfg_.video.bitrateMbps);
    bitrateSpin_->setSuffix(" Mbps");
    brRow->addWidget(bitrateSpin_);
    audioCheck_ = new QCheckBox(tr("Stream audio"));
    audioCheck_->setChecked(true);
    brRow->addWidget(audioCheck_);
    setupLay->addLayout(brRow);

    startButton_ = new QPushButton(tr("Start hosting"));
    startButton_->setObjectName("primary");
    connect(startButton_, &QPushButton::clicked, this, &HostWindow::startHosting);
    setupLay->addWidget(startButton_);

    stopButton_ = new QPushButton(tr("Stop"));
    stopButton_->setEnabled(false);
    connect(stopButton_, &QPushButton::clicked, this, &HostWindow::stopHosting);
    setupLay->addWidget(stopButton_);

    backButton_ = new QPushButton(tr("Back"));
    connect(backButton_, &QPushButton::clicked, this, [this] {
        stopIfHosting();
        emit backToHomeRequested();
    });
    setupLay->addWidget(backButton_);
    setupLay->addStretch(1);
    root->addWidget(setupBox, 1);

    // ---------------- right: session ----------------
    auto* sessionBox = new QGroupBox(tr("Session"));
    auto* sessionLay = new QVBoxLayout(sessionBox);

    codeLabel_ = new QLabel(tr("not hosting"));
    codeLabel_->setObjectName("sessionCode");
    sessionLay->addWidget(codeLabel_);

    addressLabel_ = new QLabel("-");
    sessionLay->addWidget(addressLabel_);
    gamepadLabel_ = new QLabel("-");
    gamepadLabel_->setWordWrap(true);
    sessionLay->addWidget(gamepadLabel_);

    playersTable_ = new QTableWidget(0, 4);
    playersTable_->setHorizontalHeaderLabels({ tr("Player"), tr("Ping"), tr("Input"), tr("State") });
    playersTable_->horizontalHeader()->setStretchLastSection(true);
    playersTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    playersTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    sessionLay->addWidget(playersTable_);

    auto* actRow = new QHBoxLayout;
    kickButton_ = new QPushButton(tr("Kick"));
    connect(kickButton_, &QPushButton::clicked, this, &HostWindow::kickSelected);
    inputButton_ = new QPushButton(tr("Toggle input"));
    connect(inputButton_, &QPushButton::clicked, this, &HostWindow::toggleInputSelected);
    actRow->addWidget(kickButton_);
    actRow->addWidget(inputButton_);
    sessionLay->addLayout(actRow);

    statsLabel_ = new QLabel;
    statsLabel_->setWordWrap(true);
    sessionLay->addWidget(statsLabel_);

    auto* abrRow = new QHBoxLayout;
    abrCheck_ = new QCheckBox(tr("Adaptive bitrate"));
    abrCheck_->setChecked(true);
    connect(abrCheck_, &QCheckBox::toggled, this, &HostWindow::onAbrToggled);
    bitrateSlider_ = new QSlider(Qt::Horizontal);
    bitrateSlider_->setRange(2, 50);
    bitrateSlider_->setValue(cfg_.video.bitrateMbps);
    connect(bitrateSlider_, &QSlider::valueChanged, this, &HostWindow::onManualBitrate);
    abrRow->addWidget(abrCheck_, 1);
    abrRow->addWidget(new QLabel(tr("manual:")));
    abrRow->addWidget(bitrateSlider_, 1);
    sessionLay->addLayout(abrRow);

    logView_ = new QTextEdit;
    logView_->setReadOnly(true);
    logView_->setMaximumHeight(140);
    sessionLay->addWidget(logView_);

    root->addWidget(sessionBox, 2);
    setLayout(root);
}

void HostWindow::refreshCaptureSources() {
    sourceCombo_->clear();
    // Visible windows (window capture = default, best for games).
    for (const auto& w : listCaptureWindows()) {
        const QString title = QString::fromWCharArray(w.title.c_str()) + "  [" +
                              QString::fromWCharArray(w.processExe.c_str()) + "]";
        sourceCombo_->addItem(title, QVariant::fromValue<void*>(w.hwnd));
    }
    // Full monitors.
    for (const auto& o : enumerateOutputs()) {
        const QString name = tr("Monitor %1 (%2x%3)").arg(o.index + 1).arg(o.width).arg(o.height);
        sourceCombo_->addItem(name, QVariant::fromValue<void*>(nullptr));
    }
}

void HostWindow::startHosting() {
    HostLaunchSettings s;
    s.gameName = gameEdit_->text().toStdString();
    s.resolution.width = 1920;
    s.resolution.height = 1080;
    const QString res = resolutionCombo_->currentText();
    const int wx = res.indexOf('x');
    if (wx > 0) {
        s.resolution.width = res.left(wx).toInt();
        s.resolution.height = res.mid(wx + 1).toInt();
    }
    s.fps = static_cast<uint32_t>(fpsCombo_->currentText().toInt());
    s.bitrateKbps = static_cast<uint32_t>(bitrateSpin_->value()) * 1000;
    s.codec = codecCombo_->currentIndex() == 1 ? common::VideoCodec::Hevc : common::VideoCodec::H264;
    s.audioEnabled = audioCheck_->isChecked();
    s.audioBitrateKbps = cfg_.audio.bitrateKbps;

    const QVariant source = sourceCombo_->currentData();
    s.windowHwnd = source.value<void*>();
    s.captureMode = s.windowHwnd ? common::CaptureMode::Window : common::CaptureMode::Monitor;
    // Monitor index = rows after the windows; keep simple: window rows first.
    s.outputIndex = s.windowHwnd ? 0 : (sourceCombo_->currentIndex() - sourceCombo_->count() + 1);

    QString err;
    bool ok = false;
    if (modeCombo_->currentIndex() == 1) {
        ok = coordinator_->startInternet(s, serverEdit_->text(), 9000, &err);
    } else {
        ok = coordinator_->startLan(s, &err);
    }
    if (!ok) {
        QMessageBox::warning(this, tr("RemotePlay"), err.isEmpty() ? tr("Could not start hosting.") : err);
        return;
    }
    setHostingUi(true);
}

void HostWindow::stopHosting() {
    coordinator_->stop();
    setHostingUi(false);
    appendLog(tr("Session stopped."));
}

void HostWindow::stopIfHosting() {
    if (coordinator_ && coordinator_->hosting()) {
        coordinator_->stop();
        setHostingUi(false);
    }
}

void HostWindow::setHostingUi(bool hosting) {
    startButton_->setEnabled(!hosting);
    stopButton_->setEnabled(hosting);
    modeCombo_->setEnabled(!hosting);
    serverEdit_->setEnabled(!hosting);
    gameEdit_->setEnabled(!hosting);
    sourceCombo_->setEnabled(!hosting);
}

void HostWindow::onStateChanged() {
    const auto st = coordinator_->uiState();
    if (!st.sessionCode.isEmpty()) {
        QString where = st.internetMode ? tr("via %1").arg(st.serverAddress) : tr("LAN port %1").arg(st.tcpPort);
        codeLabel_->setText(st.sessionCode);
        addressLabel_->setText(where);
    } else {
        codeLabel_->setText(tr("not hosting"));
        addressLabel_->setText("-");
    }
    gamepadLabel_->setText(tr("Virtual gamepads: %1").arg(st.gamepadStatus.isEmpty() ? "-" : st.gamepadStatus));
    statsLabel_->setText(
        tr("Encoder: %1   FPS: %2   Capture: %3 ms   Encode: %4 ms\n"
           "Send: %5 Mbps (target %6)   Ping: %7 ms   Loss: %8%   Jitter: %9 ms")
            .arg(st.encoderName.isEmpty() ? "-" : st.encoderName)
            .arg(st.fps, 0, 'f', 0)
            .arg(st.captureMs, 0, 'f', 1)
            .arg(st.encodeMs, 0, 'f', 1)
            .arg(st.bitrateKbps / 1000.0, 0, 'f', 1)
            .arg(st.bitrateTarget / 1000.0, 0, 'f', 1)
            .arg(st.rttMs, 0, 'f', 0)
            .arg(st.lossPercent, 0, 'f', 1)
            .arg(st.jitterMs, 0, 'f', 1));
}

void HostWindow::refreshPlayerTable() {
    const auto rows = coordinator_->clients();
    playersTable_->setRowCount(static_cast<int>(rows.size()));
    int r = 0;
    for (const auto& c : rows) {
        auto* nameItem = new QTableWidgetItem(QString::fromStdString(c.name));
        auto* pingItem = new QTableWidgetItem(c.rttMs ? QString::number(c.rttMs) + " ms" : "-");
        auto* inputItem = new QTableWidgetItem(c.inputEnabled ? tr("on") : tr("off"));
        auto* stateItem = new QTableWidgetItem(QString::fromStdString(common::toString(c.state)));
        playersTable_->setItem(r, 0, nameItem);
        playersTable_->setItem(r, 1, pingItem);
        playersTable_->setItem(r, 2, inputItem);
        playersTable_->setItem(r, 3, stateItem);
        ++r;
    }
}

void HostWindow::kickSelected() {
    const int row = playersTable_->currentRow();
    if (row < 0) return;
    const auto rows = coordinator_->clients();
    if (row < static_cast<int>(rows.size())) {
        coordinator_->kick(rows[row].id);
    }
}

void HostWindow::toggleInputSelected() {
    const int row = playersTable_->currentRow();
    if (row < 0) return;
    const auto rows = coordinator_->clients();
    if (row < static_cast<int>(rows.size())) {
        coordinator_->setClientInput(rows[row].id, !rows[row].inputEnabled);
    }
}

void HostWindow::onApprovalRequest(uint32_t clientId, const QString& name) {
    QMessageBox box(this);
    box.setWindowTitle(tr("Player wants to join"));
    box.setText(tr("%1 requests to join your session.").arg(name));
    box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    box.button(QMessageBox::Yes)->setText(tr("Accept"));
    box.button(QMessageBox::No)->setText(tr("Reject"));
    box.setDefaultButton(QMessageBox::Yes);
    QTimer::singleShot(60000, &box, [&box] { if (box.isVisible()) box.reject(); });
    const auto choice = box.exec();
    coordinator_->approve(clientId, choice == QMessageBox::Yes);
}

void HostWindow::onEncoderTrouble(const QString& message) {
    QMessageBox box(this);
    box.setWindowTitle(tr("Streaming problem"));
    box.setText(message);
    box.addButton(tr("Restart encoder"), QMessageBox::AcceptRole);
    box.addButton(tr("Switch to software"), QMessageBox::ActionRole);
    box.addButton(tr("Ignore"), QMessageBox::RejectRole);
    switch (box.exec()) {
        case 0: coordinator_->restartEncoders(); break;
        case 1: coordinator_->switchEncodersToSoftware(); break;
        default: break;
    }
}

void HostWindow::onManualBitrate(int mbps) {
    if (abrCheck_->isChecked()) abrCheck_->setChecked(false);   // manual = off ABR
    coordinator_->setManualBitrate(mbps * 1000);
}

void HostWindow::onAbrToggled(bool on) {
    coordinator_->setAbrEnabled(on);
}

void HostWindow::appendLog(const QString& line) {
    logView_->append(line);
}

} // namespace rp::ui
