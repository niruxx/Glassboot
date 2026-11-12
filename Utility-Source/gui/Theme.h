#pragma once
#include <QApplication>
#include <QPalette>
#include <QColor>

namespace glassboot {
inline void applyTheme(QApplication& app) {
    app.setStyle("Fusion");
    // An explicit palette also covers dialogs created before the main window and
    // disabled text, so the light application remains readable on a dark desktop.
    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#f7f8fe"));
    palette.setColor(QPalette::WindowText, QColor("#202b49"));
    palette.setColor(QPalette::Base, QColor("#ffffff"));
    palette.setColor(QPalette::AlternateBase, QColor("#f0f3fc"));
    palette.setColor(QPalette::Text, QColor("#202b49"));
    palette.setColor(QPalette::Button, QColor("#e8edfc"));
    palette.setColor(QPalette::ButtonText, QColor("#34466e"));
    palette.setColor(QPalette::Highlight, QColor("#5266c9"));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::ToolTipBase, QColor("#34466e"));
    palette.setColor(QPalette::ToolTipText, Qt::white);
    palette.setColor(QPalette::PlaceholderText, QColor("#52617f"));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor("#71809b"));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#71809b"));
    app.setPalette(palette);
}
} // namespace glassboot
