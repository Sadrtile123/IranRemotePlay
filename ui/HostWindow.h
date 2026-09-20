#pragma once
// Phase 16 — host window: session setup, player management, live stats,
// approval + crash-recovery dialogs, internet/LAN modes.

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
    void onManualBitrate(int kbps);
    void onAbrToggled(bool on);

private:
    void buildUi();
    void setHostingUi(bool hosting);

    config::Config& cfg_;
    app::HostCoordinator* coordinator_ = nullptr;

    // setup panel
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

    // session panel
    QLabel* codeLabel_ = nullptr;
    QLabel* addressLabel_ = nullptr;
    QLabel* gamepadLabel_ = nullptr;
    QTableWidget* playersTable_ = nullptr;
    QPushButton* kickButton_ = nullptr;
    QPushButton* inputButton_ = nullptr;

    // stats panel
    QLabel* statsLabel_ = nullptr;
    QSlider* bitrateSlider_ = nullptr;
    QCheckBox* abrCheck_ = nullptr;

    QTextEdit* logView_ = nullptr;
    QTimer* refreshTimer_;
};

} // namespace rp::ui
