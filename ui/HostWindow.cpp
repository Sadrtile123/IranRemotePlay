// RemotePlay - ui/HostWindow.cpp
// v0.1.1 — redesign + button/behavior fixes:
//  * capture source rows now carry their monitor index in item data (the old
//    index arithmetic produced negative output indices -> "stream start
//    failed" whenever a non-last monitor was selected)
//  * host display name is actually sent (was ignored)
//  * "Stream audio" checkbox is honored (was force-enabled)
//  * encoder/error reports are rate-limited + auto-close (the old modal
//    message-box storm from the capture retry loop froze the app -> "not
//    responding" -> close)
//  * copy session code + open logs folder buttons

#include "HostWindow.h"
#include "../app/HostCoordinator.h"
#include "../capture/ScreenCapture.h"
#include "../common/Config.h"
#include "../common/Paths.h"
#include "Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QCheckBox>
#include <QComboBox>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaType>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QTableWidget>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTimer>
#include <QUrl>
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
    connect(coordinator_, &HostCoordinator::errorOccurred, this, &HostWindow::showError);
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
    root->setContentsMargins(18, 14, 18, 14);
    root->setSpacing(14);

    // ---------------- left column: setup ----------------
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* setupHost = new QWidget;
    auto* setupLay = new QVBoxLayout(setupHost);
    setupLay->setContentsMargins(0, 0, 6, 0);
    setupLay->setSpacing(12);

    auto* headerRow = new QHBoxLayout;
    auto* title = new QLabel(tr("Host a session"));
    title->setFont(headerFont(16));
    auto* backBtn = new QPushButton(tr("Back"));
    connect(backBtn, &QPushButton::clicked, this, [this] {
        stopIfHosting();
        emit backToHomeRequested();
    });
    headerRow->addWidget(title);
    headerRow->addStretch(1);
    headerRow->addWidget(backBtn);
    setupLay->addLayout(headerRow);

    auto* setupBox = new QGroupBox(tr("SESSION SETUP"));
    auto* form = new QVBoxLayout(setupBox);
    form->setSpacing(8);

    form->addWidget(new QLabel(tr("Your name:")));
    nameEdit_ = new QLineEdit(QString::fromStdString(cfg_.profile.name));
    if (nameEdit_->text().isEmpty()) nameEdit_->setText(QStringLiteral("Host"));
    form->addWidget(nameEdit_);

    form->addWidget(new QLabel(tr("Game:")));
    gameEdit_ = new QLineEdit;
    gameEdit_->setPlaceholderText(tr("e.g. Rayman Legends"));
    form->addWidget(gameEdit_);

    form->addWidget(new QLabel(tr("Connection mode:")));
    modeCombo_ = new QComboBox;
    modeCombo_->addItem(tr("LAN / direct (IP + code)"));
    modeCombo_->addItem(tr("Internet (signaling server, no port forwarding)"));
    form->addWidget(modeCombo_);
    serverEdit_ = new QLineEdit;
    serverEdit_->setPlaceholderText(tr("signaling server address (host:port)"));
    serverEdit_->setVisible(false);
    form->addWidget(serverEdit_);
    connect(modeCombo_, &QComboBox::currentIndexChanged, this, [this](int idx) {
        serverEdit_->setVisible(idx == 1);
    });

    form->addWidget(new QLabel(tr("Capture source:")));
    sourceCombo_ = new QComboBox;
    form->addWidget(sourceCombo_);
    rescanButton_ = new QPushButton(tr("Rescan windows"));
    connect(rescanButton_, &QPushButton::clicked, this, &HostWindow::refreshCaptureSources);
    form->addWidget(rescanButton_);

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
    resRow->addWidget(resolutionCombo_, 1);
    resRow->addWidget(fpsCombo_, 1);
    form->addLayout(resRow);

    codecCombo_ = new QComboBox;
    codecCombo_->addItem(tr("H.264 (best compatibility)"));
    codecCombo_->addItem(tr("H.265 / HEVC"));
    form->addWidget(codecCombo_);

    auto* brRow = new QHBoxLayout;
    bitrateSpin_ = new QSpinBox;
    bitrateSpin_->setRange(2, 50);
    bitrateSpin_->setValue(cfg_.video.bitrateMbps);
    bitrateSpin_->setSuffix(" Mbps");
    audioCheck_ = new QCheckBox(tr("Stream audio"));
    audioCheck_->setChecked(true);
    brRow->addWidget(bitrateSpin_, 1);
    brRow->addWidget(audioCheck_);
    form->addLayout(brRow);

    startButton_ = new QPushButton(tr("Start hosting"));
    startButton_->setObjectName("primary");
    connect(startButton_, &QPushButton::clicked, this, &HostWindow::startHosting);
    form->addWidget(startButton_);

    stopButton_ = new QPushButton(tr("Stop hosting"));
    stopButton_->setObjectName("danger");
    stopButton_->setEnabled(false);
    connect(stopButton_, &QPushButton::clicked, this, &HostWindow::stopHosting);
    form->addWidget(stopButton_);

    setupLay->addWidget(setupBox);
    setupLay->addStretch(1);
    scroll->setWidget(setupHost);
    root->addWidget(scroll, 2);

    // ---------------- right column: session ----------------
    auto* sessionCol = new QVBoxLayout;
    sessionCol->setSpacing(12);

    auto* sessionBox = new QGroupBox(tr("LIVE SESSION"));
    auto* sessionLay = new QVBoxLayout(sessionBox);
    sessionLay->setSpacing(8);

    codeLabel_ = new QLabel(tr("NOT HOSTING"));
    codeLabel_->setObjectName("sessionCode");
    codeLabel_->setAlignment(Qt::AlignCenter);
    sessionLay->addWidget(codeLabel_);

    addressLabel_ = new QLabel(tr("Start hosting to get a shareable code."));
    addressLabel_->setAlignment(Qt::AlignCenter);
    addressLabel_->setProperty("muted", true);
    addressLabel_->setWordWrap(true);
    sessionLay->addWidget(addressLabel_);

    auto* codeRow = new QHBoxLayout;
    copyButton_ = new QPushButton(tr("Copy code"));
    copyButton_->setEnabled(false);
    connect(copyButton_, &QPushButton::clicked, this, &HostWindow::copySessionCode);
    logsButton_ = new QPushButton(tr("Open logs"));
    connect(logsButton_, &QPushButton::clicked, this, &HostWindow::openLogsFolder);
    codeRow->addWidget(copyButton_, 1);
    codeRow->addWidget(logsButton_, 1);
    sessionLay->addLayout(codeRow);

    gamepadLabel_ = new QLabel("-");
    gamepadLabel_->setProperty("muted", true);
    gamepadLabel_->setWordWrap(true);
    sessionLay->addWidget(gamepadLabel_);

    playersTable_ = new QTableWidget(0, 4);
    playersTable_->setHorizontalHeaderLabels({ tr("Player"), tr("Ping"), tr("Input"), tr("State") });
    playersTable_->horizontalHeader()->setStretchLastSection(true);
    playersTable_->verticalHeader()->setVisible(false);
    playersTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    playersTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    playersTable_->setAlternatingRowColors(true);
    playersTable_->setMinimumHeight(120);
    sessionLay->addWidget(playersTable_, 1);

    auto* actRow = new QHBoxLayout;
    kickButton_ = new QPushButton(tr("Kick player"));
    kickButton_->setObjectName("danger");
    connect(kickButton_, &QPushButton::clicked, this, &HostWindow::kickSelected);
    inputButton_ = new QPushButton(tr("Toggle input"));
    connect(inputButton_, &QPushButton::clicked, this, &HostWindow::toggleInputSelected);
    actRow->addWidget(kickButton_, 1);
    actRow->addWidget(inputButton_, 1);
    sessionLay->addLayout(actRow);

    sessionCol->addWidget(sessionBox, 3);

    // ---------------- stats + control ----------------
    auto* statsBox = new QGroupBox(tr("STREAM STATISTICS"));
    auto* statsGrid = new QGridLayout(statsBox);
    statsGrid->setSpacing(6);
    statsGrid->setContentsMargins(12, 18, 12, 10);
    makeStatCell(tr("FPS"), &statFps_, &statFpsV_);
    makeStatCell(tr("Encoder"), &statEncoder_, &statEncoderV_);
    makeStatCell(tr("Capture"), &statCapture_, &statCaptureV_);
    makeStatCell(tr("Encode"), &statEncode_, &statEncodeV_);
    makeStatCell(tr("Bitrate"), &statBitrate_, &statBitrateV_);
    makeStatCell(tr("Ping"), &statPing_, &statPingV_);
    makeStatCell(tr("Loss"), &statLoss_, &statLossV_);
    makeStatCell(tr("Jitter"), &statJitter_, &statJitterV_);
    statsGrid->addWidget(statFps_, 0, 0);      statsGrid->addWidget(statFpsV_, 1, 0);
    statsGrid->addWidget(statEncoder_, 0, 1);  statsGrid->addWidget(statEncoderV_, 1, 1);
    statsGrid->addWidget(statCapture_, 0, 2);  statsGrid->addWidget(statCaptureV_, 1, 2);
    statsGrid->addWidget(statEncode_, 0, 3);   statsGrid->addWidget(statEncodeV_, 1, 3);
    statsGrid->addWidget(statBitrate_, 2, 0);  statsGrid->addWidget(statBitrateV_, 3, 0);
    statsGrid->addWidget(statPing_, 2, 1);     statsGrid->addWidget(statPingV_, 3, 1);
    statsGrid->addWidget(statLoss_, 2, 2);     statsGrid->addWidget(statLossV_, 3, 2);
    statsGrid->addWidget(statJitter_, 2, 3);   statsGrid->addWidget(statJitterV_, 3, 3);
    sessionCol->addWidget(statsBox, 1);

    auto* abrRow = new QHBoxLayout;
    abrCheck_ = new QCheckBox(tr("Adaptive bitrate"));
    abrCheck_->setChecked(true);
    connect(abrCheck_, &QCheckBox::toggled, this, &HostWindow::onAbrToggled);
    bitrateSlider_ = new QSlider(Qt::Horizontal);
    bitrateSlider_->setRange(2, 50);
    bitrateSlider_->setValue(cfg_.video.bitrateMbps);
    bitrateSlider_->setToolTip(tr("Manual bitrate (Mbps) - turns adaptive off"));
    connect(bitrateSlider_, &QSlider::valueChanged, this, &HostWindow::onManualBitrate);
    abrRow->addWidget(abrCheck_, 1);
    abrRow->addWidget(bitrateSlider_, 2);
    sessionCol->addLayout(abrRow);

    logView_ = new QTextEdit;
    logView_->setReadOnly(true);
    logView_->setMaximumHeight(150);
    logView_->setPlaceholderText(tr("Session log..."));
    sessionCol->addWidget(logView_);

    root->addLayout(sessionCol, 3);
    setLayout(root);
}

void HostWindow::makeStatCell(const QString& name, QLabel** nameLabel, QLabel** valueLabel) {
    *nameLabel = new QLabel(name);
    (*nameLabel)->setObjectName("statName");
    *valueLabel = new QLabel("-");
    (*valueLabel)->setObjectName("statValue");
}

void HostWindow::refreshCaptureSources() {
    sourceCombo_->clear();
    // Visible windows (window capture = default, best for games).
    for (const auto& w : listCaptureWindows()) {
        const QString title = QString::fromWCharArray(w.title.c_str()) + "  [" +
                              QString::fromWCharArray(w.processExe.c_str()) + "]";
        // Item data: window rows carry the HWND; monitor rows carry the index.
        sourceCombo_->addItem(title, QVariant::fromValue<void*>(w.hwnd));
    }
    // Full monitors.
    for (const auto& o : enumerateOutputs()) {
        const QString name = tr("Monitor %1 (%2x%3)").arg(o.index + 1).arg(o.width).arg(o.height);
        QVariant v;
        v.setValue(o.index);
        sourceCombo_->addItem(name, v);
    }
    if (sourceCombo_->count() > 0) sourceCombo_->setCurrentIndex(0);
}

void HostWindow::startHosting() {
    HostLaunchSettings s;
    s.hostName = nameEdit_->text().trimmed().toStdString();
    s.gameName = gameEdit_->text().trimmed().toStdString();
    if (s.gameName.empty()) s.gameName = "a game";
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

    // Capture source: window rows store the HWND; monitor rows store the
    // monitor index. (The old position arithmetic computed wrong - sometimes
    // negative - indices for every monitor except the last one.)
    const QVariant source = sourceCombo_->count() > 0 ? sourceCombo_->currentData() : QVariant();
    void* hwnd = nullptr;
    int monitorIndex = 0;
    if (source.isValid()) {
        // Window rows: stored via QVariant::fromValue<void*> (QMetaType::VoidStar).
        if (source.metaType().id() == QMetaType::VoidStar) {
            hwnd = source.value<void*>();
        } else {
            monitorIndex = source.toInt();
        }
    }
    s.windowHwnd = hwnd;
    s.captureMode = hwnd ? common::CaptureMode::Window : common::CaptureMode::Monitor;
    s.outputIndex = monitorIndex;

    QString err;
    bool ok = false;
    if (modeCombo_->currentIndex() == 1) {
        // Server field may be "host" or "host:port".
        QString server = serverEdit_->text().trimmed();
        uint16_t serverPort = 9000;
        const int colon = server.lastIndexOf(':');
        if (colon > 0 && server.indexOf(':') == colon) {
            const int p = server.mid(colon + 1).toInt();
            if (p > 0 && p <= 65535) {
                serverPort = static_cast<uint16_t>(p);
                server = server.left(colon);
            }
        }
        ok = coordinator_->startInternet(s, server, serverPort, &err);
    } else {
        ok = coordinator_->startLan(s, &err);
    }
    if (!ok) {
        showError(err.isEmpty() ? tr("Could not start hosting.") : err);
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
    copyButton_->setEnabled(hosting);
    modeCombo_->setEnabled(!hosting);
    serverEdit_->setEnabled(!hosting);
    gameEdit_->setEnabled(!hosting);
    sourceCombo_->setEnabled(!hosting);
    nameEdit_->setEnabled(!hosting);
    rescanButton_->setEnabled(!hosting);
    if (!hosting) {
        codeLabel_->setText(tr("NOT HOSTING"));
        addressLabel_->setText(tr("Start hosting to get a shareable code."));
    }
}

void HostWindow::onStateChanged() {
    const auto st = coordinator_->uiState();
    if (!st.sessionCode.isEmpty()) {
        QString where = st.internetMode ? tr("via %1").arg(st.serverAddress)
                                        : tr("LAN - tell players your IP + port %1").arg(st.tcpPort);
        codeLabel_->setText(st.sessionCode);
        addressLabel_->setText(where);
    } else if (coordinator_ && !coordinator_->hosting()) {
        codeLabel_->setText(tr("NOT HOSTING"));
        addressLabel_->setText(tr("Start hosting to get a shareable code."));
    }
    gamepadLabel_->setText(tr("Virtual gamepads: %1").arg(st.gamepadStatus.isEmpty() ? "-" : st.gamepadStatus));
    statFpsV_->setText(st.fps > 0 ? QString::number(st.fps, 'f', 0) : "-");
    statEncoderV_->setText(st.encoderName.isEmpty() ? "-" : st.encoderName);
    statCaptureV_->setText(st.captureMs > 0 ? QString::number(st.captureMs, 'f', 1) + " ms" : "-");
    statEncodeV_->setText(st.encodeMs > 0 ? QString::number(st.encodeMs, 'f', 1) + " ms" : "-");
    statBitrateV_->setText(st.bitrateKbps > 0 ? QString::number(st.bitrateKbps / 1000.0, 'f', 1) + " Mbps" : "-");
    statPingV_->setText(st.rttMs > 0 ? QString::number(st.rttMs, 'f', 0) + " ms" : "-");
    statLossV_->setText(st.lossPercent > 0 ? QString::number(st.lossPercent, 'f', 1) + "%" : "-");
    statJitterV_->setText(st.jitterMs > 0 ? QString::number(st.jitterMs, 'f', 1) + " ms" : "-");
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
    if (row < 0) {
        showError(tr("Select a player in the table first."));
        return;
    }
    const auto rows = coordinator_->clients();
    if (row < static_cast<int>(rows.size())) {
        coordinator_->kick(rows[row].id);
    }
}

void HostWindow::toggleInputSelected() {
    const int row = playersTable_->currentRow();
    if (row < 0) {
        showError(tr("Select a player in the table first."));
        return;
    }
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
    // Recoverable (or at least actionable) pipeline trouble. Still uses a
    // dialog because the user can pick a recovery action, but rate-limited so
    // a failing pipeline can never stack dialogs again.
    showError(message);
}

void HostWindow::showError(const QString& message) {
    if (message.isEmpty()) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // Suppress duplicates shown within 10 s; one live box at a time.
    if (activeErrorBox_ && activeErrorBox_->isVisible()) return;
    if (message == lastErrorText_ && now - lastErrorShownAt_ < 10000) {
        appendLog(message);
        return;
    }
    lastErrorText_ = message;
    lastErrorShownAt_ = now;

    appendLog(tr("[error] %1").arg(message));

    auto* box = new QMessageBox(this);
    activeErrorBox_ = box;
    box->setIcon(QMessageBox::Warning);
    box->setWindowTitle(tr("RemotePlay"));
    box->setText(message);
    box->setAttribute(Qt::WA_DeleteOnClose);
    connect(box, &QMessageBox::destroyed, this, [this] { activeErrorBox_ = nullptr; });
    // Non-modal: the app stays usable; auto-closes after 10 s.
    QTimer::singleShot(10000, box, [box] { if (box->isVisible()) box->close(); });
    box->show();
}

void HostWindow::onManualBitrate(int mbps) {
    if (abrCheck_->isChecked()) abrCheck_->setChecked(false);   // manual = off ABR
    coordinator_->setManualBitrate(mbps * 1000);
}

void HostWindow::onAbrToggled(bool on) {
    coordinator_->setAbrEnabled(on);
}

void HostWindow::copySessionCode() {
    const auto st = coordinator_->uiState();
    if (st.sessionCode.isEmpty()) return;
    QApplication::clipboard()->setText(st.sessionCode);
    appendLog(tr("Session code copied to clipboard."));
}

void HostWindow::openLogsFolder() {
    const QString dir = QString::fromStdString(rp::paths::logDir());
    if (QFileInfo::exists(dir)) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    } else {
        showError(tr("Log folder does not exist yet: %1").arg(dir));
    }
}

void HostWindow::appendLog(const QString& line) {
    // Bound the log pane: keep the last 300 blocks.
    if (logView_->document()->blockCount() > 300) {
        QTextCursor cur(logView_->document());
        cur.movePosition(QTextCursor::Start);
        cur.movePosition(QTextCursor::NextBlock, QTextCursor::KeepAnchor,
                         logView_->document()->blockCount() - 300);
        cur.removeSelectedText();
    }
    logView_->append(line);
}

} // namespace rp::ui
