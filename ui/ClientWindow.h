// RemotePlay - ui/ClientWindow.h
// Client page: name + host address + session code, CONNECT, live status and
// ping, DISCONNECT, log pane.
#pragma once

#include "client/ClientApp.h"

#include <QWidget>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTimer;

namespace rp::config { struct Config; }

namespace rp::ui {

class ClientWindow : public QWidget {
    Q_OBJECT
public:
    explicit ClientWindow(config::Config& config, QWidget* parent = nullptr);

    void disconnectIfConnected();

signals:
    void backToHomeRequested();

private slots:
    void connectToHost();
    void disconnectFromHost();
    void refreshStatus();
    void appendLog(const QString& line);

private:
    void setConnectedUi(bool connected);
    [[nodiscard]] client::JoinSettings collectSettings() const;
    void wireSessionEvents();

    config::Config& config_;
    client::ClientApp app_;

    QLineEdit* nameEdit_ = nullptr;
    QLineEdit* addressEdit_ = nullptr;
    QSpinBox* portSpin_ = nullptr;
    QLineEdit* codeEdit_ = nullptr;
    QPushButton* connectButton_ = nullptr;
    QPushButton* disconnectButton_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* detailsLabel_ = nullptr;
    QLabel* pingLabel_ = nullptr;
    QLabel* inputLabel_ = nullptr;
    QPlainTextEdit* logView_ = nullptr;
    QTimer* pollTimer_ = nullptr;
};

} // namespace rp::ui
