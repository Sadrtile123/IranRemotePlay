// RemotePlay - ui/ClientWindow.cpp
#include "ui/ClientWindow.h"

#include "common/Config.h"
#include "common/Paths.h"
#include "common/Types.h"
#include "ui/Theme.h"

#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QMetaObject>

#include <QRegularExpression>

namespace rp::ui {

namespace {

QString stateToText(common::ConnectionState s) { return QString::fromUtf8(common::toString(s)); }

} // namespace

ClientWindow::ClientWindow(config::Config& config, QWidget* parent)
    : QWidget(parent), config_(config) {
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(24, 18, 24, 18);
    rootLayout->setSpacing(12);

    auto* header = new QHBoxLayout();
    auto* title = new QLabel(QStringLiteral("JOIN A SESSION"), this);
    title->setFont(headerFont(18));
    auto* backButton = new QPushButton(QStringLiteral("< Back"), this);
    header->addWidget(title);
    header->addStretch(1);
    header->addWidget(backButton);
    rootLayout->addLayout(header);
    connect(backButton, &QPushButton::clicked, this, &ClientWindow::backToHomeRequested);

    // --- Join form ----------------------------------------------------------
    auto* joinBox = new QGroupBox(QStringLiteral("Connection"), this);
    auto* form = new QFormLayout(joinBox);

    nameEdit_ = new QLineEdit(joinBox);
    nameEdit_->setPlaceholderText(QStringLiteral("Your display name"));
    if (!config_.profile.name.empty()) {
        nameEdit_->setText(QString::fromStdString(config_.profile.name));
    }
    form->addRow(QStringLiteral("Your name:"), nameEdit_);

    addressEdit_ = new QLineEdit(joinBox);
    addressEdit_->setPlaceholderText(QStringLiteral("Host IP, e.g. 192.168.1.20"));
    addressEdit_->setText(QStringLiteral("127.0.0.1"));
    form->addRow(QStringLiteral("Host address:"), addressEdit_);

    portSpin_ = new QSpinBox(joinBox);
    portSpin_->setRange(1024, 65535);
    portSpin_->setValue(config_.network.listenPort);
    form->addRow(QStringLiteral("Port:"), portSpin_);

    codeEdit_ = new QLineEdit(joinBox);
    codeEdit_->setPlaceholderText(QStringLiteral("Session code, e.g. ABC7-K92P"));
    codeEdit_->setMaxLength(14); // "XXXX-XXXX" plus typing room
    form->addRow(QStringLiteral("Session code:"), codeEdit_);

    // Friendly typing: uppercase and keep only code characters.
    connect(codeEdit_, &QLineEdit::textEdited, codeEdit_, [this](const QString& text) {
        QString filtered;
        for (const QChar ch : text) {
            const char16_t c = ch.unicode();
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '2' && c <= '9') || c == '-';
            if (ok) filtered.append(ch.toUpper());
        }
        if (filtered != text) {
            codeEdit_->setText(filtered);
        }
    });

    rootLayout->addWidget(joinBox);

    connectButton_ = new QPushButton(QStringLiteral("CONNECT"), this);
    connectButton_->setMinimumHeight(40);
    disconnectButton_ = new QPushButton(QStringLiteral("DISCONNECT"), this);
    disconnectButton_->setMinimumHeight(40);
    disconnectButton_->setVisible(false);
    rootLayout->addWidget(connectButton_);
    rootLayout->addWidget(disconnectButton_);

    // --- Status -------------------------------------------------------------
    auto* statusBox = new QGroupBox(QStringLiteral("Session"), this);
    auto* statusLayout = new QVBoxLayout(statusBox);

    statusLabel_ = new QLabel(QStringLiteral("Not connected"), statusBox);
    statusLabel_->setAlignment(Qt::AlignCenter);
    statusLabel_->setFont(headerFont(14));
    statusLayout->addWidget(statusLabel_);

    detailsLabel_ = new QLabel(QStringLiteral("-"), statusBox);
    detailsLabel_->setAlignment(Qt::AlignCenter);
    detailsLabel_->setWordWrap(true);
    statusLayout->addWidget(detailsLabel_);

    pingLabel_ = new QLabel(QStringLiteral("Ping: -"), statusBox);
    pingLabel_->setAlignment(Qt::AlignCenter);
    statusLayout->addWidget(pingLabel_);

    inputLabel_ = new QLabel(QStringLiteral("Input: -"), statusBox);
    inputLabel_->setAlignment(Qt::AlignCenter);
    statusLayout->addWidget(inputLabel_);

    rootLayout->addWidget(statusBox, 1);

    logView_ = new QPlainTextEdit(this);
    logView_->setReadOnly(true);
    logView_->setMaximumBlockCount(400);
    logView_->setMaximumHeight(120);
    rootLayout->addWidget(logView_);

    pollTimer_ = new QTimer(this);
    pollTimer_->setInterval(500);
    connect(pollTimer_, &QTimer::timeout, this, &ClientWindow::refreshStatus);

    connect(connectButton_, &QPushButton::clicked, this, &ClientWindow::connectToHost);
    connect(disconnectButton_, &QPushButton::clicked, this, &ClientWindow::disconnectFromHost);

    wireSessionEvents();
}

void ClientWindow::wireSessionEvents() {
    client::ClientSession::Events ev;

    ev.onState = [this](common::ConnectionState state) {
        QMetaObject::invokeMethod(this,
                                  [this, state] {
                                      statusLabel_->setText(stateToText(state));
                                  },
                                  Qt::QueuedConnection);
    };
    ev.onConnected = [this](const client::ClientStatus&) {
        QMetaObject::invokeMethod(this,
                                  [this] {
                                      setConnectedUi(true);
                                      appendLog(QStringLiteral("[INFO] Session established."));
                                  },
                                  Qt::QueuedConnection);
    };
    ev.onDisconnected = [this](const std::string& reason) {
        QMetaObject::invokeMethod(this,
                                  [this, reason] {
                                      setConnectedUi(false);
                                      statusLabel_->setText(QStringLiteral("DISCONNECTED"));
                                      appendLog(QStringLiteral("[INFO] Disconnected: %1")
                                                    .arg(QString::fromStdString(reason)));
                                  },
                                  Qt::QueuedConnection);
    };
    ev.onError = [this](const std::string& message) {
        QMetaObject::invokeMethod(this,
                                  [this, message] {
                                      appendLog(QStringLiteral("[ERROR] %1")
                                                    .arg(QString::fromStdString(message)));
                                  },
                                  Qt::QueuedConnection);
    };
    ev.onLog = [this](rp::log::Level level, const std::string& message) {
        QMetaObject::invokeMethod(this,
                                  [this, level, message] {
                                      QString prefix = QStringLiteral("[INFO]");
                                      switch (level) {
                                          case rp::log::Level::Warning: prefix = QStringLiteral("[WARNING]"); break;
                                          case rp::log::Level::Error: prefix = QStringLiteral("[ERROR]"); break;
                                          case rp::log::Level::Critical: prefix = QStringLiteral("[CRITICAL]"); break;
                                          case rp::log::Level::Debug: prefix = QStringLiteral("[DEBUG]"); break;
                                          case rp::log::Level::Trace: prefix = QStringLiteral("[TRACE]"); break;
                                          default: break;
                                      }
                                      appendLog(prefix + QStringLiteral(" ") +
                                                QString::fromStdString(message));
                                  },
                                  Qt::QueuedConnection);
    };
    app_.setEvents(std::move(ev));
}

client::JoinSettings ClientWindow::collectSettings() const {
    client::JoinSettings s;
    s.displayName = nameEdit_->text().trimmed().toStdString();
    s.hostAddress = addressEdit_->text().trimmed().toStdString();
    s.hostPort = static_cast<uint16_t>(portSpin_->value());
    s.sessionCode = codeEdit_->text().trimmed().toStdString();
    return s;
}

void ClientWindow::connectToHost() {
    auto settings = collectSettings();

    if (settings.displayName.empty()) {
        appendLog(QStringLiteral("[ERROR] Enter your display name first."));
        return;
    }
    if (settings.hostAddress.empty()) {
        appendLog(QStringLiteral("[ERROR] Enter the host address first."));
        return;
    }
    // Validate the session code shape (ABC7-K92P).
    static const QRegularExpression re(QStringLiteral("^[A-Z2-9]{4}-?[A-Z2-9]{4}$"));
    const QString code = QString::fromStdString(settings.sessionCode);
    if (!re.match(code).hasMatch()) {
        appendLog(QStringLiteral("[ERROR] Session code looks wrong. Format: ABC7-K92P"));
        return;
    }

    // Remember the name for future sessions.
    config_.profile.name = settings.displayName;
    if (!config_.save(paths::configFilePath())) {
        appendLog(QStringLiteral("[WARNING] Could not save settings."));
    }

    wireSessionEvents(); // events live on the app and are passed to new sessions
    app_.connect(settings);

    setConnectedUi(true);
    statusLabel_->setText(QStringLiteral("CONNECTING"));
    appendLog(QStringLiteral("[INFO] Connecting to %1:%2 with code %3...")
                  .arg(QString::fromStdString(settings.hostAddress))
                  .arg(settings.hostPort)
                  .arg(code));
    pollTimer_->start();
}

void ClientWindow::disconnectFromHost() {
    app_.disconnect();
    setConnectedUi(false);
    statusLabel_->setText(QStringLiteral("DISCONNECTED"));
    pollTimer_->stop();
    refreshStatus();
}

void ClientWindow::disconnectIfConnected() {
    if (app_.connected()) {
        disconnectFromHost();
    }
}

void ClientWindow::setConnectedUi(bool connected) {
    connectButton_->setVisible(!connected);
    disconnectButton_->setVisible(connected);
    nameEdit_->setEnabled(!connected);
    addressEdit_->setEnabled(!connected);
    portSpin_->setEnabled(!connected);
    codeEdit_->setEnabled(!connected);
}

void ClientWindow::refreshStatus() {
    const auto st = app_.status();

    if (st.state == common::ConnectionState::Connected) {
        statusLabel_->setText(QStringLiteral("CONNECTED"));
    } else if (st.state == common::ConnectionState::Authenticating) {
        statusLabel_->setText(QStringLiteral("WAITING FOR HOST APPROVAL"));
    }

    if (!st.hostName.empty()) {
        detailsLabel_->setText(QStringLiteral("Connected to: <b>%1</b><br>Game: <b>%2</b>"
                                              "<br>Stream: %3, %4x%5 @ %6 FPS, %7 Mbps")
                                   .arg(QString::fromStdString(st.hostName),
                                        QString::fromStdString(st.gameName),
                                        QString::fromStdString(common::videoCodecString(st.codec)))
                                   .arg(st.width)
                                   .arg(st.height)
                                   .arg(st.fps)
                                   .arg(QString::number(st.bitrateKbps / 1000.0, 'f', 1)));
    }

    pingLabel_->setText(st.state == common::ConnectionState::Connected
                            ? QStringLiteral("Ping: %1 ms").arg(st.rttMs)
                            : QStringLiteral("Ping: -"));

    inputLabel_->setText(QStringLiteral("Input: %1")
                             .arg(st.input.allDisabled()
                                      ? QStringLiteral("disabled by host")
                                      : (st.input.controller
                                             ? QStringLiteral("controller enabled")
                                             : QStringLiteral("limited"))));
}

void ClientWindow::appendLog(const QString& line) { logView_->appendPlainText(line); }

} // namespace rp::ui
