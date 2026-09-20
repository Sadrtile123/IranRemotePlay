// RemotePlay - ui/MainWindow.cpp
// v0.1.1 — redesigned hero home page with clickable cards.

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
#include <QMouseEvent>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <functional>

namespace rp::ui {
namespace {

// A clickable card (host / join). std::function click handler keeps this
// Q_OBJECT-free and simple.
class HeroCard : public QFrame {
public:
    explicit HeroCard(const QString& title, const QString& desc, QWidget* parent = nullptr)
        : QFrame(parent) {
        setObjectName(QStringLiteral("heroCard"));
        setAttribute(Qt::WA_StyledBackground, true);   // required for QSS backgrounds on QFrame
        setCursor(Qt::PointingHandCursor);
        setFixedHeight(150);
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(20, 18, 20, 16);
        lay->setSpacing(6);
        auto* t = new QLabel(title, this);
        t->setObjectName(QStringLiteral("heroTitle"));
        auto* d = new QLabel(desc, this);
        d->setObjectName(QStringLiteral("heroDesc"));
        d->setWordWrap(true);
        lay->addWidget(t);
        lay->addWidget(d);
        lay->addStretch(1);
    }
    std::function<void()> onClick;
protected:
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton && onClick) onClick();
        QFrame::mousePressEvent(e);
    }
};

} // namespace

MainWindow::MainWindow(config::Config& config, QWidget* parent)
    : QMainWindow(parent), config_(config) {
    setWindowTitle(QStringLiteral("RemotePlay"));
    resize(1080, 720);

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
    page->setObjectName(QStringLiteral("homePage"));
    page->setAttribute(Qt::WA_StyledBackground, true);   // paint the QSS gradient

    auto* brand = new QLabel(QStringLiteral("REMOTEPLAY"), page);
    brand->setObjectName(QStringLiteral("brand"));
    brand->setAlignment(Qt::AlignCenter);

    auto* subtitle = new QLabel(
        QStringLiteral("Play local multiplayer games together, over the Internet."), page);
    subtitle->setAlignment(Qt::AlignCenter);
    subtitle->setStyleSheet(QStringLiteral("color:#9aa3b2;"));

    auto* hostCard = new HeroCard(
        QStringLiteral("Host a game"),
        QStringLiteral("Share your screen and audio. Friends connect with a session code and "
                       "their controllers, keyboard and mouse control your game."), page);
    hostCard->onClick = [this] { showHost(); };

    auto* joinCard = new HeroCard(
        QStringLiteral("Join a session"),
        QStringLiteral("Enter a session code from your friend and start playing in seconds. "
                       "F11 toggles fullscreen, F10 shows the stats overlay."), page);
    joinCard->onClick = [this] { showClient(); };

    auto* cards = new QHBoxLayout();
    cards->setSpacing(18);
    cards->addWidget(hostCard, 1);
    cards->addWidget(joinCard, 1);

    auto* settingsButton = new QPushButton(QStringLiteral("Settings"), page);
    settingsButton->setCursor(Qt::PointingHandCursor);

    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(72, 56, 72, 28);
    layout->setSpacing(12);
    layout->addStretch(2);
    layout->addWidget(brand);
    layout->addWidget(subtitle);
    layout->addSpacing(24);
    layout->addLayout(cards);
    layout->addStretch(2);
    layout->addWidget(settingsButton, 0, Qt::AlignHCenter);

    auto* version = new QLabel(QStringLiteral("v%1 - Windows x64")
                                   .arg(QLatin1String(kAppVersion)), page);
    version->setAlignment(Qt::AlignHCenter);
    version->setStyleSheet(QStringLiteral("color:#6b7180;"));
    layout->addWidget(version);

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
    clientPage_->disconnectIfActive();
    event->accept();
}

} // namespace rp::ui
