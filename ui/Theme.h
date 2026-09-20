// RemotePlay - ui/Theme.h
// Dark theme applied via palette + a small application-wide stylesheet.
#pragma once

#include <QApplication>
#include <QFont>
#include <QPalette>

namespace rp::ui {

void applyDarkTheme(QApplication& app);
[[nodiscard]] QFont headerFont(int pointSize);

} // namespace rp::ui
