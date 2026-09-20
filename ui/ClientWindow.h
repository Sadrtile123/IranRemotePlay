#pragma once
// Phase 16 — client window: join form, full stream view (VideoWidget with
// F10 stats overlay), quality banner, input forwarding.

#include "../common/Config.h"
#include "VideoWidget.h"

#include <QWidget>

#include <QString>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

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

private:
    void buildUi();
    [[nodiscard]] std::vector<QString> statsLines() const;
    void setStreamingUi(bool streaming);
    void updateQualityBanner();

    config::Config& cfg_;
    app::ClientCoordinator* coordinator_ = nullptr;

    // join form
    QWidget* joinForm_ = nullptr;
    QComboBox* modeCombo_ = nullptr;
    QLineEdit* nameEdit_ = nullptr;
    QLineEdit* hostEdit_ = nullptr;        // LAN: host address / Internet: server address
    QLineEdit* codeEdit_ = nullptr;
    QPushButton* joinButton_ = nullptr;
    QPushButton* backButton_ = nullptr;
    QLabel* joinStatusLabel_ = nullptr;

    // stream view
    VideoWidget* video_ = nullptr;
    QPushButton* leaveButton_ = nullptr;
    QLabel* infoLabel_ = nullptr;
};

} // namespace rp::ui
