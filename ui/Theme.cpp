// RemotePlay - ui/Theme.cpp
// v0.1.1 — modern dark design system.
//
// The "UI looked off" root cause: QPalette alone cannot restyle native
// Windows widgets (combos, spins, scrollbars stay light). The Fusion style
// honors the full palette + QSS, giving a consistent dark app.

#include "Theme.h"

#include <QApplication>
#include <QFont>
#include <QPalette>

namespace rp::ui {

namespace {
// Palette tokens (kept in sync with the QSS below).
const char* kBg      = "#0e1015";   // app background
const char* kPanel   = "#151823";   // cards / panels
const char* kPanel2  = "#1b1f2c";   // raised elements (headers, inputs bg)
const char* kBorder  = "#272d3d";
const char* kAccent  = "#4da3ff";
const char* kText    = "#e8ecf2";
const char* kMuted   = "#9aa3b2";
const char* kDanger  = "#ff5d5d";
} // namespace

void applyDarkTheme(QApplication& app) {
    // Fusion renders every control from the palette/stylesheet — no mixed
    // native-light widgets.
    QApplication::setStyle(QStringLiteral("Fusion"));

    QFont base(QStringLiteral("Segoe UI"), 10);
    base.setStyleStrategy(QFont::PreferAntialias);
    app.setFont(base);

    QPalette p;
    p.setColor(QPalette::Window, QColor(kBg));
    p.setColor(QPalette::WindowText, QColor(kText));
    p.setColor(QPalette::Base, QColor(kPanel2));
    p.setColor(QPalette::AlternateBase, QColor(kPanel));
    p.setColor(QPalette::Text, QColor(kText));
    p.setColor(QPalette::Button, QColor(kPanel2));
    p.setColor(QPalette::ButtonText, QColor(kText));
    p.setColor(QPalette::Highlight, QColor(kAccent));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::ToolTipBase, QColor(kPanel2));
    p.setColor(QPalette::ToolTipText, QColor(kText));
    p.setColor(QPalette::PlaceholderText, QColor(kMuted));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(110, 110, 120));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(110, 110, 120));
    p.setColor(QPalette::Link, QColor(kAccent));
    p.setColor(QPalette::LinkVisited, QColor(kAccent));
    app.setPalette(p);

    app.setStyleSheet(QStringLiteral(R"(
/* ================= foundation ================= */
QWidget { background: transparent; color: %TEXT%; font-family: "Segoe UI"; }
QMainWindow, QDialog { background: %BG%; }
QWidget#homePage { background: qlineargradient(x1:0, y1:0, x2:0.35, y2:1,
        stop:0 #10131c, stop:1 #0c0e13); }

QToolTip { background: %PANEL2%; color: %TEXT%; border: 1px solid %BORDER%;
           padding: 6px 8px; border-radius: 4px; }

/* ================= buttons ================= */
QPushButton {
    background: %PANEL2%;
    border: 1px solid %BORDER%;
    border-radius: 6px;
    padding: 7px 16px;
    color: %TEXT%;
    font-weight: 600;
}
QPushButton:hover { border-color: %ACCENT%; background: #212636; }
QPushButton:pressed { background: #181c29; border-color: %ACCENT%; }
QPushButton:disabled { color: #6b7180; border-color: #20242f; background: #14161f; }

QPushButton#primary {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #4da3ff, stop:1 #2f86e8);
    border: 1px solid #2f7ee0;
    color: white;
    font-weight: 700;
    padding: 9px 18px;
}
QPushButton#primary:hover {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #6fb5ff, stop:1 #3f93f2);
    border-color: #6fb5ff;
}
QPushButton#primary:pressed { background: #2f7ed6; }
QPushButton#primary:disabled { background: #24425f; border-color: #24425f; color: #9aa8b5; }

QPushButton#danger { color: #ff8080; border-color: #55323a; }
QPushButton#danger:hover { background: #2c1b1f; border-color: %DANGER%; color: %DANGER%; }

/* big hero cards on the home page */
QFrame#heroCard {
    background: %PANEL%;
    border: 1px solid %BORDER%;
    border-radius: 10px;
}
QFrame#heroCard:hover { border-color: %ACCENT%; background: #181c2a; }

/* ================= inputs ================= */
QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {
    background: #10131c;
    border: 1px solid %BORDER%;
    border-radius: 6px;
    padding: 6px 10px;
    color: %TEXT%;
    min-height: 20px;
    selection-background-color: %ACCENT%;
    selection-color: white;
}
QLineEdit:focus, QSpinBox:focus, QComboBox:focus, QDoubleSpinBox:focus { border-color: %ACCENT%; }
QLineEdit:disabled, QSpinBox:disabled, QComboBox:disabled { color: #6b7180; }
QLineEdit[invalid="true"] { border-color: %DANGER%; }

QComboBox QAbstractItemView {
    background: %PANEL2%;
    border: 1px solid %BORDER%;
    color: %TEXT%;
    selection-background-color: %ACCENT%;
    selection-color: white;
    outline: none;
}
QComboBox::drop-down { border: none; width: 22px; }
QSpinBox::up-button, QSpinBox::down-button { width: 18px; border: none; background: transparent; }
QSpinBox::up-button:hover, QSpinBox::down-button:hover { background: #262b3a; border-radius: 3px; }

/* ================= group boxes (cards) ================= */
QGroupBox {
    background: %PANEL%;
    border: 1px solid %BORDER%;
    border-radius: 10px;
    margin-top: 14px;
    padding: 14px 12px 10px 12px;
    font-weight: 700;
}
QGroupBox::title {
    subcontrol-origin: margin;
    subcontrol-position: top left;
    left: 12px;
    padding: 0 4px;
    color: %ACCENT%;
    letter-spacing: 1px;
}

/* ================= tables ================= */
QTableWidget {
    background: #12151f;
    alternate-background-color: #161a26;
    border: 1px solid %BORDER%;
    border-radius: 6px;
    gridline-color: #20242f;
    selection-background-color: #2a4a72;
    selection-color: white;
    outline: none;
}
QTableWidget::item { padding: 4px 8px; border: none; }
QTableWidget::item:selected { background: #2a4a72; }
QHeaderView { background: transparent; border: none; }
QHeaderView::section {
    background: #1a1e2b;
    color: %MUTED%;
    border: none;
    border-right: 1px solid #20242f;
    padding: 6px 8px;
    font-weight: 700;
}

/* ================= text areas ================= */
QTextEdit, QPlainTextEdit, QTextBrowser {
    background: #10131c;
    border: 1px solid %BORDER%;
    border-radius: 6px;
    padding: 6px;
    color: %TEXT%;
    selection-background-color: %ACCENT%;
    selection-color: white;
    font-family: "Consolas";
    font-size: 9pt;
}

QLabel { color: %TEXT%; background: transparent; }
QLabel[muted="true"] { color: %MUTED%; font-weight: 400; }
QLabel#sessionCode {
    font-family: "Consolas";
    font-size: 26pt;
    font-weight: 700;
    color: %ACCENT%;
    letter-spacing: 6px;
    padding: 6px;
    background: #10131c;
    border: 1px solid %BORDER%;
    border-radius: 8px;
}
QLabel#statValue { font-family: "Consolas"; font-size: 11pt; color: %TEXT%; }
QLabel#statName  { color: %MUTED%; font-size: 8.5pt; font-weight: 400; }
QLabel#heroTitle {
    font-family: "Segoe UI";
    font-size: 15pt;
    font-weight: 800;
    color: %TEXT%;
}
QLabel#heroDesc { color: %MUTED%; font-weight: 400; }
QLabel#brand {
    color: %ACCENT%;
    font-family: "Segoe UI";
    font-size: 32pt;
    font-weight: 800;
    letter-spacing: 10px;
}
QLabel#joinStatus { color: %MUTED%; font-weight: 400; }

/* ================= check boxes / radios ================= */
QCheckBox { spacing: 8px; color: %TEXT%; }
QCheckBox::indicator {
    width: 16px; height: 16px;
    border: 1px solid %BORDER%;
    border-radius: 4px;
    background: #10131c;
}
QCheckBox::indicator:hover { border-color: %ACCENT%; }
QCheckBox::indicator:checked {
    background: %ACCENT%;
    border-color: %ACCENT%;
    image: none;
}
QCheckBox:disabled { color: #6b7180; }

/* ================= sliders ================= */
QSlider::groove:horizontal {
    height: 5px;
    background: #232837;
    border-radius: 2px;
}
QSlider::sub-page:horizontal { background: %ACCENT%; border-radius: 2px; }
QSlider::handle:horizontal {
    width: 14px; height: 14px;
    margin: -5px 0;
    background: #e8ecf2;
    border-radius: 7px;
}
QSlider::handle:horizontal:hover { background: white; }
QSlider::handle:horizontal:disabled { background: #4a4f5e; }

/* ================= scrollbars ================= */
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar::handle:vertical {
    background: #2c3140;
    border-radius: 5px;
    min-height: 30px;
}
QScrollBar::handle:vertical:hover { background: #3b4254; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
QScrollBar::handle:horizontal {
    background: #2c3140;
    border-radius: 5px;
    min-width: 30px;
}
QScrollBar::handle:horizontal:hover { background: #3b4254; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { height: 0; width: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

/* ================= message boxes ================= */
QMessageBox { background: %PANEL%; border: 1px solid %BORDER%; }
QMessageBox QLabel { color: %TEXT%; font-size: 10pt; }

/* ================= misc ================= */
QFrame#line { background: %BORDER%; max-height: 1px; border: none; }
QProgressBar {
    background: #10131c; border: 1px solid %BORDER%; border-radius: 5px;
    text-align: center; color: %TEXT%; height: 14px;
}
QProgressBar::chunk { background: %ACCENT%; border-radius: 5px; }
)")
        .replace("%BG%", kBg)
        .replace("%PANEL%", kPanel)
        .replace("%PANEL2%", kPanel2)
        .replace("%BORDER%", kBorder)
        .replace("%ACCENT%", kAccent)
        .replace("%TEXT%", kText)
        .replace("%MUTED%", kMuted)
        .replace("%DANGER%", kDanger));
}

QFont headerFont(int pointSize) {
    QFont f(QStringLiteral("Segoe UI"), pointSize);
    f.setBold(true);
    return f;
}

} // namespace rp::ui
