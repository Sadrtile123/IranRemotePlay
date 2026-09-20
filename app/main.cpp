// RemotePlay - app/main.cpp
// Application entry point: logging, config, dark theme, main window.
#include "common/Config.h"
#include "common/Log.h"
#include "common/Paths.h"
#include "common/Version.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"

#include <QApplication>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("RemotePlay"));
    QApplication::setApplicationVersion(QString::fromLatin1(rp::kAppVersion));
    QApplication::setOrganizationName(QStringLiteral("RemotePlay"));

    rp::log::init(rp::paths::logDir());
    RP_INFO() << "RemotePlay " << rp::kAppVersion << " starting";

    rp::config::Config config = rp::config::Config::load(rp::paths::configFilePath());
    RP_INFO() << "Configuration loaded from " << rp::paths::configFilePath();

    rp::ui::applyDarkTheme(app);

    rp::ui::MainWindow window(config);
    window.show();

    const int code = QApplication::exec();
    RP_INFO() << "RemotePlay exiting with code " << code;
    return code;
}
