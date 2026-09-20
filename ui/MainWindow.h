// RemotePlay - ui/MainWindow.h
// Role-selection shell: HOME (Host game / Join session) / HOST / CLIENT /
// SETTINGS pages in a QStackedWidget.
#pragma once

#include <QMainWindow>

class QStackedWidget;
class QPushButton;

namespace rp::config { struct Config; }

namespace rp::ui {

class HostWindow;
class ClientWindow;
class SettingsWindow;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(config::Config& config, QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void showHome();
    void showHost();
    void showClient();
    void showSettings();

private:
    QWidget* buildHomePage();

    config::Config& config_;
    QStackedWidget* pages_ = nullptr;
    HostWindow* hostPage_ = nullptr;
    ClientWindow* clientPage_ = nullptr;
    SettingsWindow* settingsPage_ = nullptr;
};

} // namespace rp::ui
