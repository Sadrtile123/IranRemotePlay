// RemotePlay - ui/HostWindow.cpp
#include "ui/HostWindow.h"

#include "common/Config.h"
#include "common/Paths.h"
#include "common/Types.h"
#include "ui/Theme.h"

#include <QComboBox>
#include <QDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <QMetaObject>

namespace rp::ui {

using namespace std::chrono_literals;

namespace {

QString stateToText(common::ConnectionState s) { return QString::fromUtf8(common::toString(s)); }

} // namespace

HostWindow::HostWindow(config::Config& config, QWidget* parent)
    : QWidget(parent), config_(config) {
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(24, 18, 24, 18);
    rootLayout->setSpacing(12);

    auto* header = new QHBoxLayout();
    auto* title = new QLabel(QStringLiteral("HOST A GAME"), this);
    title->setFont(headerFont(18));
    auto* backButton = new QPushButton(QStringLiteral("< Back"), this);
    header->addWidget(title);
    header->addStretch(1);
    header->addWidget(backButton);
    rootLayout->addLayout(header);
    connect(backButton, &QPushButton::clicked, this, &HostWindow::backToHomeRequested);

    // --- Settings group -----------------------------------------------------
    auto* settingsBox = new QGroupBox(QStringLiteral("Stream settings"), this);
    auto* form = new QFormLayout(settingsBox);

    gameEdit_ = new QLineEdit(settingsBox);
    gameEdit_->setPlaceholderText(QStringLiteral("e.g. Rayman Legends"));
    form->addRow(QStringLiteral("Game:"), gameEdit_);

    captureCombo_ = new QComboBox(settingsBox);
    captureCombo_->addItem(QStringLiteral("Game window (preferred)"));
    captureCombo_->addItem(QStringLiteral("Entire monitor (fallback)"));
    form->addRow(QStringLiteral("Capture:"), captureCombo_);

    resolutionCombo_ = new QComboBox(settingsBox);
    for (const auto& r : common::Resolution::presets()) {
        resolutionCombo_->addItem(QString::fromStdString(r.toString()));
    }
    form->addRow(QStringLiteral("Resolution:"), resolutionCombo_);

    fpsCombo_ = new QComboBox(settingsBox);
    for (int f : common::fpsPresets()) {
        fpsCombo_->addItem(QString::number(f) + QStringLiteral(" FPS"));
    }
    form->addRow(QStringLiteral("Frame rate:"), fpsCombo_);

    codecCombo_ = new QComboBox(settingsBox);
    codecCombo_->addItem(QStringLiteral("H.264 (compatibility default)"));
    codecCombo_->addItem(QStringLiteral("HEVC / H.265"));
    codecCombo_->addItem(QStringLiteral("AV1"));
    form->addRow(QStringLiteral("Codec:"), codecCombo_);

    bitrateSpin_ = new QSpinBox(settingsBox);
    bitrateSpin_->setRange(2, 50);
    bitrateSpin_->setSuffix(QStringLiteral(" Mbps"));
    bitrateSpin_->setValue(config_.video.bitrateMbps);
    form->addRow(QStringLiteral("Bitrate:"), bitrateSpin_);

    portSpin_ = new QSpinBox(settingsBox);
    portSpin_->setRange(1024, 65535);
    portSpin_->setValue(config_.network.listenPort);
    form->addRow(QStringLiteral("Listen port:"), portSpin_);

    rootLayout->addWidget(settingsBox);

    // --- Session controls ---------------------------------------------------
    startButton_ = new QPushButton(QStringLiteral("START HOSTING"), this);
    startButton_->setMinimumHeight(40);
    stopButton_ = new QPushButton(QStringLiteral("STOP HOSTING"), this);
    stopButton_->setMinimumHeight(40);
    stopButton_->setVisible(false);

    codeLabel_ = new QLabel(this);
    codeLabel_->setFont(headerFont(26));
    codeLabel_->setAlignment(Qt::AlignCenter);
    codeLabel_->setStyleSheet(QStringLiteral("color:#42a0ff;"));
    codeLabel_->setVisible(false);

    addressLabel_ = new QLabel(this);
    addressLabel_->setAlignment(Qt::AlignCenter);
    addressLabel_->setWordWrap(true);
    addressLabel_->setVisible(false);

    rootLayout->addWidget(startButton_);
    rootLayout->addWidget(stopButton_);
    rootLayout->addWidget(codeLabel_);
    rootLayout->addWidget(addressLabel_);

    // --- Player table -------------------------------------------------------
    auto* playersBox = new QGroupBox(QStringLiteral("Connected players"), this);
    auto* playersLayout = new QVBoxLayout(playersBox);
    table_ = new QTableWidget(0, 5, playersBox);
    table_->setHorizontalHeaderLabels({QStringLiteral("Player"), QStringLiteral("Name"),
                                       QStringLiteral("Controller"), QStringLiteral("Latency"),
                                       QStringLiteral("Status")});
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->verticalHeader()->setVisible(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setMinimumHeight(140);
    playersLayout->addWidget(table_);

    auto* rowButtons = new QHBoxLayout();
    kickButton_ = new QPushButton(QStringLiteral("KICK"), playersBox);
    inputButton_ = new QPushButton(QStringLiteral("DISABLE INPUT"), playersBox);
    rowButtons->addWidget(kickButton_);
    rowButtons->addWidget(inputButton_);
    rowButtons->addStretch(1);
    playersLayout->addLayout(rowButtons);
    rootLayout->addWidget(playersBox, 1);

    // --- Log ---------------------------------------------------------------
    logView_ = new QPlainTextEdit(this);
    logView_->setReadOnly(true);
    logView_->setMaximumBlockCount(400);
    logView_->setMaximumHeight(120);
    rootLayout->addWidget(logView_);

    pollTimer_ = new QTimer(this);
    pollTimer_->setInterval(500);
    connect(pollTimer_, &QTimer::timeout, this, &HostWindow::refreshPlayerTable);

    connect(startButton_, &QPushButton::clicked, this, &HostWindow::startHosting);
    connect(stopButton_, &QPushButton::clicked, this, &HostWindow::stopHosting);
    connect(kickButton_, &QPushButton::clicked, this, &HostWindow::kickSelected);
    connect(inputButton_, &QPushButton::clicked, this, &HostWindow::toggleInputSelected);

    wireSessionEvents();

    // Initial form values from config.
    resolutionCombo_->setCurrentIndex(
        [this]() {
            const std::string r = config_.video.resolution.toString();
            for (int i = 0; i < resolutionCombo_->count(); ++i) {
                if (resolutionCombo_->itemText(i).toStdString() == r) return i;
            }
            return 1; // 1920x1080 default
        }());
    const int fpsIndex = [this]() {
        const auto& presets = common::fpsPresets();
        for (size_t i = 0; i < presets.size(); ++i) {
            if (presets[i] == config_.video.fps) return static_cast<int>(i);
        }
        return 1; // 60 default
    }();
    fpsCombo_->setCurrentIndex(fpsIndex);
    switch (config_.video.codec) {
        case common::VideoCodec::Hevc: codecCombo_->setCurrentIndex(1); break;
        case common::VideoCodec::Av1: codecCombo_->setCurrentIndex(2); break;
        case common::VideoCodec::H264:
        default: codecCombo_->setCurrentIndex(0); break;
    }
}

void HostWindow::wireSessionEvents() {
    host::HostSession::Events ev;

    // All events arrive on the network thread -> marshal to the UI thread.
    ev.onApprovalRequest = [this](uint32_t id, const std::string& name) {
        QMetaObject::invokeMethod(this,
                                  [this, id, name] {
                                      showApprovalDialog(id, QString::fromStdString(name));
                                  },
                                  Qt::QueuedConnection);
    };
    ev.onLog = [this](rp::log::Level level, const std::string& message) {
        QMetaObject::invokeMethod(this,
                                  [this, level, message] {
                                      QString prefix;
                                      switch (level) {
                                          case rp::log::Level::Trace: prefix = QStringLiteral("[TRACE]"); break;
                                          case rp::log::Level::Debug: prefix = QStringLiteral("[DEBUG]"); break;
                                          case rp::log::Level::Info: prefix = QStringLiteral("[INFO]"); break;
                                          case rp::log::Level::Warning: prefix = QStringLiteral("[WARNING]"); break;
                                          case rp::log::Level::Error: prefix = QStringLiteral("[ERROR]"); break;
                                          case rp::log::Level::Critical: prefix = QStringLiteral("[CRITICAL]"); break;
                                      }
                                      appendLog(prefix + QStringLiteral(" ") +
                                                QString::fromStdString(message));
                                  },
                                  Qt::QueuedConnection);
    };
    app_.setEvents(std::move(ev));
}

host::HostLaunchSettings HostWindow::collectSettings() const {
    host::HostLaunchSettings s;
    s.gameName = gameEdit_->text().trimmed().toStdString();
    s.captureMode = captureCombo_->currentIndex() == 1 ? common::CaptureMode::Monitor
                                                       : common::CaptureMode::Window;
    if (const auto r = common::Resolution::parse(resolutionCombo_->currentText().toStdString())) {
        s.resolution = *r;
    } else {
        s.resolution = common::Resolution{1920, 1080};
    }
    s.fps = common::fpsPresets()[static_cast<size_t>(
        qBound(0, fpsCombo_->currentIndex(), static_cast<int>(common::fpsPresets().size()) - 1))];
    s.bitrateKbps = static_cast<uint32_t>(bitrateSpin_->value()) * 1000;
    switch (codecCombo_->currentIndex()) {
        case 1: s.codec = common::VideoCodec::Hevc; break;
        case 2: s.codec = common::VideoCodec::Av1; break;
        default: s.codec = common::VideoCodec::H264; break;
    }
    s.listenPort = static_cast<uint16_t>(portSpin_->value());
    s.requireApproval = config_.input.requireHostApproval;
    s.inputDefaults = common::InputPermissions{config_.input.controller, config_.input.keyboard,
                                               config_.input.mouse, config_.input.vibration};
    return s;
}

void HostWindow::startHosting() {
    auto settings = collectSettings();
    if (settings.gameName.empty()) {
        settings.gameName = "Desktop"; // streaming-the-current-window scenario
    }

    // HostApp::start() passes the stored events to the fresh HostSession, so
    // the wiring done in wireSessionEvents() stays valid across restarts.
    if (!app_.start(settings)) {
        appendLog(QStringLiteral("[ERROR] Could not start hosting (port %1 unavailable).")
                      .arg(settings.listenPort));
        return;
    }

    setHostingUi(true);
    codeLabel_->setText(QString::fromStdString(app_.sessionCode()));

    QString addresses = QStringLiteral("Give your friend:  <b>IP:port + session code</b><br>");
    const auto ips = paths::localIPv4Addresses();
    QStringList shown;
    for (const auto& ip : ips) shown << QString::fromStdString(ip);
    if (shown.isEmpty()) shown << QStringLiteral("127.0.0.1");
    addresses += QStringLiteral("This PC: %1:%2").arg(shown.join(QStringLiteral(", ")),
                                                      QString::number(app_.port()));
    addresses += QStringLiteral("<br><i>Signaling-based code-only joins arrive with the "
                                "networking phase (see docs/ROADMAP.md).</i>");
    addressLabel_->setText(addresses);

    appendLog(QStringLiteral("[INFO] Session created. Code: %1")
                  .arg(QString::fromStdString(app_.sessionCode())));
    refreshPlayerTable();
    pollTimer_->start();
}

void HostWindow::stopHosting() {
    app_.stop();
    setHostingUi(false);
    pollTimer_->stop();
    table_->setRowCount(0);
    appendLog(QStringLiteral("[INFO] Hosting stopped."));
}

void HostWindow::stopIfHosting() {
    if (app_.hosting()) {
        stopHosting();
    }
}

void HostWindow::setHostingUi(bool hosting) {
    startButton_->setVisible(!hosting);
    stopButton_->setVisible(hosting);
    codeLabel_->setVisible(hosting);
    addressLabel_->setVisible(hosting);
    gameEdit_->setEnabled(!hosting);
    captureCombo_->setEnabled(!hosting);
    resolutionCombo_->setEnabled(!hosting);
    fpsCombo_->setEnabled(!hosting);
    codecCombo_->setEnabled(!hosting);
    bitrateSpin_->setEnabled(!hosting);
    portSpin_->setEnabled(!hosting);
}

void HostWindow::showApprovalDialog(uint32_t clientId, const QString& playerName) {
    auto* dialog = new QDialog(this);
    dialog->setWindowTitle(QStringLiteral("Join request"));
    dialog->setModal(true);

    auto* label = new QLabel(
        QStringLiteral("<b>%1</b> wants to join your session.").arg(playerName), dialog);
    label->setAlignment(Qt::AlignCenter);

    auto* countdown = new QLabel(QStringLiteral("Auto-reject in 60 s"), dialog);
    countdown->setAlignment(Qt::AlignCenter);

    auto* accept = new QPushButton(QStringLiteral("ACCEPT"), dialog);
    auto* reject = new QPushButton(QStringLiteral("REJECT"), dialog);
    accept->setMinimumHeight(34);
    reject->setMinimumHeight(34);

    auto* buttons = new QHBoxLayout();
    buttons->addWidget(accept);
    buttons->addWidget(reject);

    auto* layout = new QVBoxLayout(dialog);
    layout->addWidget(label);
    layout->addWidget(countdown);
    layout->addLayout(buttons);

    QObject::connect(accept, &QPushButton::clicked, dialog, [this, dialog, clientId]() {
        app_.approve(clientId, true);
        dialog->accept();
    });
    QObject::connect(reject, &QPushButton::clicked, dialog, [this, dialog, clientId]() {
        app_.approve(clientId, false);
        dialog->reject();
    });

    // Auto-reject after the timeout (matches the session's approval window).
    // secondsLeft lives in a shared_ptr so the timer can never dangle.
    auto secondsLeft = std::make_shared<int>(60);
    auto* timer = new QTimer(dialog);
    timer->setInterval(1000);
    QObject::connect(timer, &QTimer::timeout, dialog,
                     [this, dialog, clientId, countdown, timer, secondsLeft]() {
                         --(*secondsLeft);
                         if (*secondsLeft <= 0) {
                             timer->stop();
                             app_.approve(clientId, false);
                             dialog->reject();
                             return;
                         }
                         countdown->setText(
                             QStringLiteral("Auto-reject in %1 s").arg(*secondsLeft));
                     });

    timer->start();
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void HostWindow::refreshPlayerTable() {
    const auto rows = app_.clients();
    table_->setRowCount(static_cast<int>(rows.size()));
    int row = 0;
    for (const auto& r : rows) {
        auto* idItem = new QTableWidgetItem(QString::number(r.id));
        auto* nameItem = new QTableWidgetItem(QString::fromStdString(r.name));
        auto* padItem = new QTableWidgetItem(
            QStringLiteral("- (controller support: Phase 8)"));
        auto* latencyItem = new QTableWidgetItem(
            r.state == common::ConnectionState::Connected
                ? QStringLiteral("%1 ms").arg(r.rttMs)
                : QStringLiteral("-"));
        auto* stateItem = new QTableWidgetItem(stateToText(r.state));
        idItem->setFlags(idItem->flags() & ~Qt::ItemIsEditable);
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
        padItem->setFlags(padItem->flags() & ~Qt::ItemIsEditable);
        latencyItem->setFlags(latencyItem->flags() & ~Qt::ItemIsEditable);
        stateItem->setFlags(stateItem->flags() & ~Qt::ItemIsEditable);
        table_->setItem(row, 0, idItem);
        table_->setItem(row, 1, nameItem);
        table_->setItem(row, 2, padItem);
        table_->setItem(row, 3, latencyItem);
        table_->setItem(row, 4, stateItem);
        ++row;
    }

    // Reflect the selected row's input state on the toggle button.
    const int selected = table_->currentRow();
    if (selected >= 0 && selected < static_cast<int>(rows.size())) {
        inputButton_->setText(rows[static_cast<size_t>(selected)].inputEnabled
                                  ? QStringLiteral("DISABLE INPUT")
                                  : QStringLiteral("ENABLE INPUT"));
    }
}

void HostWindow::kickSelected() {
    const int row = table_->currentRow();
    if (row < 0 || row >= table_->rowCount()) return;
    const uint32_t id = table_->item(row, 0)->text().toUInt();
    app_.kick(id);
}

void HostWindow::toggleInputSelected() {
    const int row = table_->currentRow();
    if (row < 0 || row >= table_->rowCount()) return;
    const uint32_t id = table_->item(row, 0)->text().toUInt();
    const auto rows = app_.clients();
    for (const auto& r : rows) {
        if (r.id == id) {
            app_.setClientInputEnabled(id, !r.inputEnabled);
            return;
        }
    }
}

void HostWindow::appendLog(const QString& line) { logView_->appendPlainText(line); }

} // namespace rp::ui
