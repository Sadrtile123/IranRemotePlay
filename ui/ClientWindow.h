#pragma once
// RemotePlay - ui/ClientWindow.h
// v0.1.1 — join card with recent hosts, working F11/Escape fullscreen
// (window-level, hides the bottom bar), host:port parsing, live status.

#include "../common/Config.h"
#include "VideoWidget.h"

#include <QWidget>

#include <QString>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;

namespace rp::app { class ClientCoordinator; }

namespace rp::ui {

class ClientWindow : public QWidget {
    Q_OBJECT
public:
    explicit ClientWindow(config::Config& cfg, QWidget* parent = nullptr);
    ~ClientWindow() override;

    void disconnectIfActive();

signals:
    void backToHomeRequested();

private slots:
    void join();
    void leave();
    void onStateChanged();
    void onDisconnected(const QString& reason);
    void onConnectedToHost(const QString& hostName, const QString& gameName, int playerIndex);
    void toggleFullscreen();
    void showError(const QString& message);

private:
    void buildUi();
    [[nodiscard]] std::vector<QString> statsLines() const;
    void setStreamingUi(bool streaming);
    void setVideoFullscreen(bool fullscreen);
    void updateQualityBanner();
    void loadRecentHosts();
    void rememberHost(const QString& host);

    config::Config& cfg_;
    app::ClientCoordinator* coordinator_ = nullptr;

    // join form
    QWidget* joinForm_ = nullptr;
    QComboBox* modeCombo_ = nullptr;
    QLineEdit* nameEdit_ = nullptr;
    QComboBox* hostCombo_ = nullptr;        // editable + history
    QLineEdit* codeEdit_ = nullptr;
    QPushButton* joinButton_ = nullptr;
    QPushButton* backButton_ = nullptr;
    QLabel* joinStatusLabel_ = nullptr;

    // stream view
    VideoWidget* video_ = nullptr;
    QWidget* bottomBar_ = nullptr;
    QPushButton* leaveButton_ = nullptr;
    QPushButton* fullscreenButton_ = nullptr;
    QLabel* infoLabel_ = nullptr;
    bool videoFullscreen_ = false;
};

} // namespace rp::ui
