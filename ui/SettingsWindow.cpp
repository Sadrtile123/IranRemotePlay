// RemotePlay - ui/SettingsWindow.cpp
#include "ui/SettingsWindow.h"

#include "common/Config.h"
#include "common/Paths.h"
#include "common/Types.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace rp::ui {

SettingsWindow::SettingsWindow(config::Config& config, QWidget* parent)
    : QWidget(parent), config_(config) {
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(24, 18, 24, 18);
    rootLayout->setSpacing(12);

    auto* header = new QHBoxLayout();
    auto* title = new QLabel(QStringLiteral("SETTINGS"), this);
    title->setFont(headerFont(18));
    auto* backButton = new QPushButton(QStringLiteral("< Back"), this);
    header->addWidget(title);
    header->addStretch(1);
    header->addWidget(backButton);
    rootLayout->addLayout(header);
    connect(backButton, &QPushButton::clicked, this, &SettingsWindow::backToHomeRequested);

    // --- Video ---------------------------------------------------------------
    auto* videoBox = new QGroupBox(QStringLiteral("Video"), this);
    auto* videoForm = new QFormLayout(videoBox);

    codecCombo_ = new QComboBox(videoBox);
    codecCombo_->addItem(QStringLiteral("H.264 (compatibility default)"));
    codecCombo_->addItem(QStringLiteral("HEVC / H.265"));
    codecCombo_->addItem(QStringLiteral("AV1"));
    videoForm->addRow(QStringLiteral("Codec:"), codecCombo_);

    resolutionCombo_ = new QComboBox(videoBox);
    for (const auto& r : common::Resolution::presets()) {
        resolutionCombo_->addItem(QString::fromStdString(r.toString()));
    }
    videoForm->addRow(QStringLiteral("Resolution:"), resolutionCombo_);

    fpsCombo_ = new QComboBox(videoBox);
    for (int f : common::fpsPresets()) {
        fpsCombo_->addItem(QString::number(f) + QStringLiteral(" FPS"));
    }
    videoForm->addRow(QStringLiteral("Frame rate:"), fpsCombo_);

    bitrateSpin_ = new QSpinBox(videoBox);
    bitrateSpin_->setRange(2, 50);
    bitrateSpin_->setSuffix(QStringLiteral(" Mbps"));
    videoForm->addRow(QStringLiteral("Bitrate:"), bitrateSpin_);
    rootLayout->addWidget(videoBox);

    // --- Audio ---------------------------------------------------------------
    auto* audioBox = new QGroupBox(QStringLiteral("Audio"), this);
    auto* audioForm = new QFormLayout(audioBox);
    audioBitrateCombo_ = new QComboBox(audioBox);
    audioBitrateCombo_->addItem(QStringLiteral("64 kbps"));
    audioBitrateCombo_->addItem(QStringLiteral("96 kbps"));
    audioBitrateCombo_->addItem(QStringLiteral("128 kbps"));
    audioBitrateCombo_->addItem(QStringLiteral("192 kbps"));
    audioForm->addRow(QStringLiteral("Opus bitrate:"), audioBitrateCombo_);
    auto* audioNote = new QLabel(
        QStringLiteral("Opus bitrate for the streamed game audio."), audioBox);
    audioNote->setWordWrap(true);
    audioNote->setProperty("muted", true);
    audioForm->addRow(QString(), audioNote);
    rootLayout->addWidget(audioBox);

    // --- Input ----------------------------------------------------------------
    auto* inputBox = new QGroupBox(QStringLiteral("Input permissions (host authority)"), this);
    auto* inputForm = new QFormLayout(inputBox);
    controllerCheck_ = new QCheckBox(QStringLiteral("Allow controller input"), inputBox);
    keyboardCheck_ = new QCheckBox(QStringLiteral("Allow keyboard input"), inputBox);
    mouseCheck_ = new QCheckBox(QStringLiteral("Allow mouse input"), inputBox);
    vibrationCheck_ = new QCheckBox(QStringLiteral("Allow vibration feedback"), inputBox);
    approvalCheck_ = new QCheckBox(QStringLiteral("Require host approval for join requests"),
                                   inputBox);
    inputForm->addRow(QString(), controllerCheck_);
    inputForm->addRow(QString(), keyboardCheck_);
    inputForm->addRow(QString(), mouseCheck_);
    inputForm->addRow(QString(), vibrationCheck_);
    inputForm->addRow(QString(), approvalCheck_);
    rootLayout->addWidget(inputBox);

    // --- Network ---------------------------------------------------------------
    auto* netBox = new QGroupBox(QStringLiteral("Network"), this);
    auto* netForm = new QFormLayout(netBox);
    maxBitrateSpin_ = new QSpinBox(netBox);
    maxBitrateSpin_->setRange(2, 50);
    maxBitrateSpin_->setSuffix(QStringLiteral(" Mbps"));
    netForm->addRow(QStringLiteral("Maximum bitrate:"), maxBitrateSpin_);
    jitterSpin_ = new QSpinBox(netBox);
    jitterSpin_->setRange(10, 200);
    jitterSpin_->setSuffix(QStringLiteral(" ms"));
    netForm->addRow(QStringLiteral("Jitter buffer:"), jitterSpin_);
    auto* jitterNote = new QLabel(
        QStringLiteral("Video buffering before display - higher smooths jitter, adds latency."), netBox);
    jitterNote->setWordWrap(true);
    jitterNote->setProperty("muted", true);
    netForm->addRow(QString(), jitterNote);
    portSpin_ = new QSpinBox(netBox);
    portSpin_->setRange(1024, 65535);
    netForm->addRow(QStringLiteral("Listen port:"), portSpin_);
    rootLayout->addWidget(netBox);

    // --- Profile ----------------------------------------------------------------
    auto* profileBox = new QGroupBox(QStringLiteral("Profile"), this);
    auto* profileForm = new QFormLayout(profileBox);
    nameEdit_ = new QLineEdit(profileBox);
    nameEdit_->setPlaceholderText(QStringLiteral("Display name when joining sessions"));
    profileForm->addRow(QStringLiteral("Name:"), nameEdit_);
    rootLayout->addWidget(profileBox);

    // --- Save -------------------------------------------------------------------
    auto* footer = new QHBoxLayout();
    saveButton_ = new QPushButton(QStringLiteral("SAVE SETTINGS"), this);
    saveButton_->setObjectName("primary");
    saveButton_->setMinimumHeight(36);
    auto* folderButton = new QPushButton(QStringLiteral("Open config folder"), this);
    folderButton->setCursor(Qt::PointingHandCursor);
    connect(folderButton, &QPushButton::clicked, this, [] {
        const QString dir = QString::fromStdString(rp::paths::configDir());
        if (QFileInfo::exists(dir)) QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });
    savedLabel_ = new QLabel(this);
    savedLabel_->setProperty("muted", true);
    footer->addWidget(saveButton_);
    footer->addWidget(folderButton);
    footer->addWidget(savedLabel_);
    footer->addStretch(1);
    rootLayout->addLayout(footer);
    rootLayout->addStretch(1);

    connect(saveButton_, &QPushButton::clicked, this, &SettingsWindow::save);

    loadFromConfig();
}

void SettingsWindow::loadFromConfig() {
    switch (config_.video.codec) {
        case common::VideoCodec::Hevc: codecCombo_->setCurrentIndex(1); break;
        case common::VideoCodec::Av1: codecCombo_->setCurrentIndex(2); break;
        case common::VideoCodec::H264:
        default: codecCombo_->setCurrentIndex(0); break;
    }
    const std::string res = config_.video.resolution.toString();
    for (int i = 0; i < resolutionCombo_->count(); ++i) {
        if (resolutionCombo_->itemText(i).toStdString() == res) {
            resolutionCombo_->setCurrentIndex(i);
            break;
        }
    }
    const auto& fps = common::fpsPresets();
    for (size_t i = 0; i < fps.size(); ++i) {
        if (fps[i] == config_.video.fps) {
            fpsCombo_->setCurrentIndex(static_cast<int>(i));
            break;
        }
    }
    bitrateSpin_->setValue(config_.video.bitrateMbps);

    switch (config_.audio.bitrateKbps) {
        case 64: audioBitrateCombo_->setCurrentIndex(0); break;
        case 96: audioBitrateCombo_->setCurrentIndex(1); break;
        case 192: audioBitrateCombo_->setCurrentIndex(3); break;
        case 128:
        default: audioBitrateCombo_->setCurrentIndex(2); break;
    }

    controllerCheck_->setChecked(config_.input.controller);
    keyboardCheck_->setChecked(config_.input.keyboard);
    mouseCheck_->setChecked(config_.input.mouse);
    vibrationCheck_->setChecked(config_.input.vibration);
    approvalCheck_->setChecked(config_.input.requireHostApproval);

    maxBitrateSpin_->setValue(config_.network.maxBitrateMbps);
    jitterSpin_->setValue(config_.network.jitterBufferMs);
    portSpin_->setValue(config_.network.listenPort);

    nameEdit_->setText(QString::fromStdString(config_.profile.name));
}

void SettingsWindow::save() {
    switch (codecCombo_->currentIndex()) {
        case 1: config_.video.codec = common::VideoCodec::Hevc; break;
        case 2: config_.video.codec = common::VideoCodec::Av1; break;
        default: config_.video.codec = common::VideoCodec::H264; break;
    }
    if (const auto r = common::Resolution::parse(resolutionCombo_->currentText().toStdString())) {
        config_.video.resolution = *r;
    }
    config_.video.fps = common::fpsPresets()[static_cast<size_t>(fpsCombo_->currentIndex())];
    config_.video.bitrateMbps = bitrateSpin_->value();

    static const int kAudioChoices[] = {64, 96, 128, 192};
    const int audioIdx = audioBitrateCombo_->currentIndex();
    config_.audio.bitrateKbps = kAudioChoices[qBound(0, audioIdx, 3)];

    config_.input.controller = controllerCheck_->isChecked();
    config_.input.keyboard = keyboardCheck_->isChecked();
    config_.input.mouse = mouseCheck_->isChecked();
    config_.input.vibration = vibrationCheck_->isChecked();
    config_.input.requireHostApproval = approvalCheck_->isChecked();

    config_.network.maxBitrateMbps = maxBitrateSpin_->value();
    config_.network.jitterBufferMs = jitterSpin_->value();
    config_.network.listenPort = portSpin_->value();

    config_.profile.name = nameEdit_->text().trimmed().toStdString();
    if (config_.profile.name.size() > 32) config_.profile.name.resize(32);

    config_.sanitize();

    if (config_.save(paths::configFilePath())) {
        savedLabel_->setText(QStringLiteral("Saved."));
    } else {
        savedLabel_->setText(QStringLiteral("Could not write config file!"));
    }
    QTimer::singleShot(2500, savedLabel_, [l = savedLabel_]() { l->setText(QString()); });
}

} // namespace rp::ui
