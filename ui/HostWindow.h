// RemotePlay - ui/HostWindow.h
// Host page: stream settings, START HOSTING, session code display, join-request
// approval dialogs, connected-player table with KICK / DISABLE INPUT.
#pragma once

#include "host/HostApp.h"

#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QPlainTextEdit;
class QTableWidget;
class QTimer;

namespace rp::config { struct Config; }

namespace rp::ui {

class HostWindow : public QWidget {
    Q_OBJECT
public:
    explicit HostWindow(config::Config& config, QWidget* parent = nullptr);

    void stopIfHosting();

signals:
    void backToHomeRequested();

private slots:
    void startHosting();
    void stopHosting();
    void kickSelected();
    void toggleInputSelected();
    void refreshPlayerTable();
    void appendLog(const QString& line);

private:
    void showApprovalDialog(uint32_t clientId, const QString& playerName);
    void setHostingUi(bool hosting);
    void wireSessionEvents();
    [[nodiscard]] host::HostLaunchSettings collectSettings() const;

    config::Config& config_;
    host::HostApp app_;

    // Settings form
    QLineEdit* gameEdit_ = nullptr;
    QComboBox* captureCombo_ = nullptr;
    QComboBox* resolutionCombo_ = nullptr;
    QComboBox* fpsCombo_ = nullptr;
    QComboBox* codecCombo_ = nullptr;
    QSpinBox* bitrateSpin_ = nullptr;
    QSpinBox* portSpin_ = nullptr;

    // Hosting UI
    QPushButton* startButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QLabel* codeLabel_ = nullptr;
    QLabel* addressLabel_ = nullptr;
    QTableWidget* table_ = nullptr;
    QPushButton* kickButton_ = nullptr;
    QPushButton* inputButton_ = nullptr;
    QPlainTextEdit* logView_ = nullptr;
    QTimer* pollTimer_ = nullptr;
};

} // namespace rp::ui
