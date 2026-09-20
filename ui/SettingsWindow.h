// RemotePlay - ui/SettingsWindow.h
// Settings page backed by the persisted JSON config.
#pragma once

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace rp::config { struct Config; }

namespace rp::ui {

class SettingsWindow : public QWidget {
    Q_OBJECT
public:
    explicit SettingsWindow(config::Config& config, QWidget* parent = nullptr);

signals:
    void backToHomeRequested();

private slots:
    void save();

private:
    void loadFromConfig();

    config::Config& config_;

    QComboBox* codecCombo_ = nullptr;
    QComboBox* resolutionCombo_ = nullptr;
    QComboBox* fpsCombo_ = nullptr;
    QSpinBox* bitrateSpin_ = nullptr;

    QComboBox* audioBitrateCombo_ = nullptr;

    QCheckBox* controllerCheck_ = nullptr;
    QCheckBox* keyboardCheck_ = nullptr;
    QCheckBox* mouseCheck_ = nullptr;
    QCheckBox* vibrationCheck_ = nullptr;
    QCheckBox* approvalCheck_ = nullptr;

    QSpinBox* maxBitrateSpin_ = nullptr;
    QSpinBox* jitterSpin_ = nullptr;
    QSpinBox* portSpin_ = nullptr;

    QLineEdit* nameEdit_ = nullptr;
    QPushButton* saveButton_ = nullptr;
    QLabel* savedLabel_ = nullptr;
};

} // namespace rp::ui
