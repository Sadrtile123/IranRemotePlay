// RemotePlay - ui/MainWindow.cpp
#include "ui/MainWindow.h"

#include "common/Version.h"
#include "common/Config.h"
#include "ui/ClientWindow.h"
#include "ui/HostWindow.h"
#include "ui/SettingsWindow.h"
#include "ui/Theme.h"

#include <QCloseEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace rp::ui {

MainWindow::MainWindow(config::Config& config, QWidget* parent)
    : QMainWindow(parent), config_(config) {
    setWindowTitle(QStringLiteral("RemotePlay"));
    resize(980, 680);

    pages_ = new QStackedWidget(this);
    hostPage_ = new HostWindow(config_, pages_);
    clientPage_ = new ClientWindow(config_, pages_);
    settingsPage_ = new SettingsWindow(config_, pages_);
    pages_->addWidget(buildHomePage());
    pages_->addWidget(hostPage_);
    pages_->addWidget(clientPage_);
    pages_->addWidget(settingsPage_);

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(pages_);
    setCentralWidget(central);

    connect(hostPage_, &HostWindow::backToHomeRequested, this, &MainWindow::showHome);
    connect(clientPage_, &ClientWindow::backToHomeRequested, this, &MainWindow::showHome);
}

QWidget* MainWindow::buildHomePage() {
    auto* page = new QWidget(this);
    page->setAutoFillBackground(true);

    auto* title = new QLabel(QStringLiteral("REMOTEPLAY"), page);
    title->setFont(headerFont(34));
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet(QStringLiteral("color:#42a0ff; letter-spacing:6px;"));

    auto* subtitle = new QLabel(
        QStringLiteral("Play local multiplayer games together, over the Internet."), page);
    subtitle->setAlignment(Qt::AlignCenter);

    auto* hostButton = new QPushButton(QStringLiteral("HOST GAME"), page);
    hostButton->setMinimumHeight(44);
    auto* joinButton = new QPushButton(QStringLiteral("JOIN SESSION"), page);
    joinButton->setMinimumHeight(44);
    auto* settingsButton = new QPushButton(QStringLiteral("Settings"), page);

    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(64, 48, 64, 32);
    layout->setSpacing(14);
    layout->addStretch(2);
    layout->addWidget(title);
    layout->addWidget(subtitle);
    layout->addStretch(1);
    layout->addWidget(hostButton);
    layout->addWidget(joinButton);
    layout->addSpacing(10);
    layout->addWidget(settingsButton, 0, Qt::AlignHCenter);
    layout->addStretch(2);

    auto* version = new QLabel(
        QStringLiteral("v%1 - Phase 1: session handshake (streaming arrives with "
                       "Phase 2+)").arg(QLatin1String(kAppVersion)), page);
    version->setAlignment(Qt::AlignHCenter);
    layout->addWidget(version);

    connect(hostButton, &QPushButton::clicked, this, &MainWindow::showHost);
    connect(joinButton, &QPushButton::clicked, this, &MainWindow::showClient);
    connect(settingsButton, &QPushButton::clicked, this, &MainWindow::showSettings);
    return page;
}

void MainWindow::showHome() { pages_->setCurrentIndex(0); }
void MainWindow::showHost() { pages_->setCurrentIndex(1); }
void MainWindow::showClient() { pages_->setCurrentIndex(2); }
void MainWindow::showSettings() { pages_->setCurrentIndex(3); }

void MainWindow::closeEvent(QCloseEvent* event) {
    // Tear sessions down cleanly before the UI disappears.
    hostPage_->stopIfHosting();
    clientPage_->disconnectIfConnected();
    event->accept();
}

} // namespace rp::ui
