// RemotePlay - ui/HostWindow.h
// v0.1.1 — redesigned; adds copy-code, open-logs, non-blocking error handling.

#pragma once

#include "../common/Config.h"
#include "../host/HostApp.h"
#include "VideoWidget.h"

#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTextEdit;
class QCheckBox;
class QSlider;
class QMessageBox;

namespace rp::app { class HostCoordinator; }

namespace rp::ui {

class HostWindow : public QWidget {
    Q_OBJECT
public:
    explicit HostWindow(config::Config& cfg, QWidget* parent = nullptr);
    ~HostWindow() override;

    void stopIfHosting();

signals:
    void backToHomeRequested();

private slots:
    void startHosting();
    void stopHosting();
    void refreshCaptureSources();
    void kickSelected();
    void toggleInputSelected();
    void refreshPlayerTable();
    void appendLog(const QString& line);
    void onStateChanged();
    void onApprovalRequest(uint32_t clientId, const QString& name);
    void onEncoderTrouble(const QString& message);
    void onManualBitrate(int mbps);
    void onAbrToggled(bool on);
    void copySessionCode();
    void openLogsFolder();
    void showError(const QString& message);   // rate-limited, auto-closing

private:
    void buildUi();
    void setHostingUi(bool hosting);
    void makeStatCell(const QString& name, QLabel** nameLabel, QLabel** valueLabel);

    config::Config& cfg_;
    app::HostCoordinator* coordinator_ = nullptr;

    // setup panel
    QLineEdit* nameEdit_ = nullptr;
    QComboBox* modeCombo_ = nullptr;        // LAN / Internet
    QLineEdit* serverEdit_ = nullptr;
    QLineEdit* gameEdit_ = nullptr;
    QComboBox* sourceCombo_ = nullptr;      // window list + monitors
    QComboBox* resolutionCombo_ = nullptr;
    QComboBox* fpsCombo_ = nullptr;
    QComboBox* codecCombo_ = nullptr;
    QSpinBox* bitrateSpin_ = nullptr;
    QCheckBox* audioCheck_ = nullptr;
    QPushButton* startButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QPushButton* backButton_ = nullptr;
    QPushButton* rescanButton_ = nullptr;

    // session panel
    QLabel* codeLabel_ = nullptr;
    QLabel* addressLabel_ = nullptr;
    QLabel* gamepadLabel_ = nullptr;
    QTableWidget* playersTable_ = nullptr;
    QPushButton* kickButton_ = nullptr;
    QPushButton* inputButton_ = nullptr;
    QPushButton* copyButton_ = nullptr;
    QPushButton* logsButton_ = nullptr;

    // stats panel
    QLabel* statFps_ = nullptr, * statFpsV_ = nullptr;
    QLabel* statEncoder_ = nullptr, * statEncoderV_ = nullptr;
    QLabel* statCapture_ = nullptr, * statCaptureV_ = nullptr;
    QLabel* statEncode_ = nullptr, * statEncodeV_ = nullptr;
    QLabel* statBitrate_ = nullptr, * statBitrateV_ = nullptr;
    QLabel* statPing_ = nullptr, * statPingV_ = nullptr;
    QLabel* statLoss_ = nullptr, * statLossV_ = nullptr;
    QLabel* statJitter_ = nullptr, * statJitterV_ = nullptr;
    QSlider* bitrateSlider_ = nullptr;
    QCheckBox* abrCheck_ = nullptr;

    QTextEdit* logView_ = nullptr;
    QTimer* refreshTimer_ = nullptr;

    // error display (one at a time, auto-closes; no modal-dialog storms)
    QMessageBox* activeErrorBox_ = nullptr;
    QString lastErrorText_;
    qint64 lastErrorShownAt_ = 0;
};

} // namespace rp::ui
