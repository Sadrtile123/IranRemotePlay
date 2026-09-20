// RemotePlay - ui/Theme.cpp
#include "ui/Theme.h"

namespace rp::ui {

void applyDarkTheme(QApplication& app) {
    QPalette p;
    const QColor window(18, 18, 24);
    const QColor base(26, 26, 34);
    const QColor button(34, 34, 46);
    const QColor text(232, 232, 238);
    const QColor bright(66, 160, 255); // RemotePlay accent blue

    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, window);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, button);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::Highlight, bright);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::ToolTipBase, base);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, QColor(120, 120, 132));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(110, 110, 120));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(110, 110, 120));
    p.setColor(QPalette::Link, bright);
    app.setPalette(p);

    app.setStyleSheet(QStringLiteral(R"(
        QPushButton { min-height: 26px; padding: 6px 16px; }
        QLineEdit, QComboBox, QSpinBox { min-height: 22px; padding: 2px 8px; }
        QTableWidget { gridline-color: #2a2a36; }
        QHeaderView::section { padding: 4px; }
        QPlainTextEdit { padding: 4px; }
    )"));
}

QFont headerFont(int pointSize) {
    QFont f;
    f.setFamily(QStringLiteral("Segoe UI"));
    f.setPointSize(pointSize);
    f.setBold(true);
    return f;
}

} // namespace rp::ui
