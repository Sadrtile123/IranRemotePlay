// RemotePlay - app/main.cpp
// Application entry point: logging, config, dark theme, main window.
#include "common/Config.h"
#include "common/Log.h"
#include "common/Paths.h"
#include "common/Version.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QIcon>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>

namespace {

// Simple programmatic app icon: rounded accent square with a play glyph.
QIcon makeAppIcon() {
    QPixmap pm(256, 256);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath card;
    card.addRoundedRect(QRectF(16, 16, 224, 224), 52, 52);
    p.fillPath(card, QColor(41, 134, 232));
    QPainterPath inner;
    inner.addRoundedRect(QRectF(16, 16, 224, 224), 52, 52);
    QPen pen(QColor(77, 163, 255), 10);
    p.setPen(pen);
    p.drawPath(inner);
    QPolygonF play;
    play << QPointF(100, 86) << QPointF(100, 170) << QPointF(172, 128);
    QPainterPath tri;
    tri.addPolygon(play);
    p.fillPath(tri, Qt::white);
    p.end();
    return QIcon(pm);
}

} // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("RemotePlay"));
    QApplication::setApplicationVersion(QString::fromLatin1(rp::kAppVersion));
    QApplication::setOrganizationName(QStringLiteral("RemotePlay"));
    QApplication::setWindowIcon(makeAppIcon());

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
