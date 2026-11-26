#pragma once
#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QSettings>
#include <utility>

namespace glassboot {
inline bool darkModeEnabled() {
    return QSettings().value("appearance/darkMode", true).toBool();
}

inline void saveDarkMode(bool dark) {
    QSettings().setValue("appearance/darkMode", dark);
}

inline void setDarkMode(QApplication& app, bool dark) {
    QPalette palette;
    const auto color = [dark](QPalette::ColorRole role, const char* light, const char* darkColor) {
        return std::pair(role, QColor(dark ? darkColor : light));
    };
    for (const auto& [role, value] : {
             color(QPalette::Window, "#edf1f7", "#101013"),
             color(QPalette::WindowText, "#192334", "#edf2fa"),
             color(QPalette::Base, "#ffffff", "#18181d"),
             color(QPalette::AlternateBase, "#eef2f8", "#24242b"),
             color(QPalette::Text, "#192334", "#edf2fa"),
             color(QPalette::Button, "#f2f5fa", "#202027"),
             color(QPalette::ButtonText, "#26344b", "#edf2fa"),
             color(QPalette::Mid, "#a1afc2", "#555560"),
             color(QPalette::Dark, "#4a5c73", "#acbbcf"),
             color(QPalette::Highlight, "#3458af", "#a9bfff"),
             color(QPalette::HighlightedText, "#ffffff", "#111c31"),
             color(QPalette::ToolTipBase, "#24334a", "#263447"),
             color(QPalette::ToolTipText, "#ffffff", "#edf2fa"),
             color(QPalette::PlaceholderText, "#4a5c73", "#acbbcf"),
             color(QPalette::Link, "#3458af", "#a9bfff"),
         }) {
        palette.setColor(role, value);
    }
    const QColor disabled(dark ? "#96a4b7" : "#576880");
    palette.setColor(QPalette::Disabled, QPalette::Text, disabled);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    palette.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
    app.setPalette(palette);
    app.setStyleSheet(QStringLiteral(R"(
        QMainWindow, QDialog { background: palette(window); }
        QStackedWidget, QScrollArea, QWidget#page { background: transparent; border: none; }
        QLabel[muted="true"] { color: palette(dark); }
        QFrame#card, QFrame#dock, QWidget#titleBar { background: palette(base); }
        QFrame#card { border: none; border-radius: 4px; }
        QFrame#dock { border-top: 1px solid palette(mid); }
        QWidget#titleBar { border-bottom: 1px solid palette(mid); }
        QLabel#welcomeMark { background: palette(highlight); color: palette(highlighted-text);
            border-radius: 6px; font-size: 42px; font-weight: 600; }
        QPushButton, QToolButton { background: palette(button); color: palette(button-text);
            border: 1px solid palette(mid); border-radius: 3px; font-weight: 600; }
        QPushButton { padding: 8px 16px; min-height: 20px; }
        QToolButton { padding: 5px 8px; }
        QPushButton:hover, QToolButton:hover { background: palette(alternate-base); }
        QPushButton:pressed, QToolButton:pressed { background: palette(window); }
        QPushButton[primary="true"]:enabled, QPushButton:default:enabled, QToolButton:checked {
            background: palette(highlight); color: palette(highlighted-text); }
        QPushButton[primary="true"]:hover, QPushButton:default:hover, QToolButton:checked:hover {
            border-color: palette(highlighted-text); }
        QPushButton:focus, QToolButton:focus, QComboBox:focus, QLineEdit:focus, QPlainTextEdit:focus {
            border: 2px solid palette(highlight); }
        QToolButton:checked:focus, QPushButton[primary="true"]:enabled:focus, QPushButton:default:enabled:focus {
            border-color: palette(highlighted-text); }
        QComboBox, QLineEdit, QPlainTextEdit { background: palette(base); color: palette(text);
            border: 1px solid palette(mid); border-radius: 3px; padding: 6px 9px; }
        QComboBox::drop-down { border: none; width: 26px; }
        QComboBox::down-arrow { image: url(:/gui/chevron.xpm); width: 12px; height: 8px; }
        QComboBox QAbstractItemView { background: palette(base); color: palette(text);
            selection-background-color: palette(highlight); selection-color: palette(highlighted-text); }
        QProgressBar { background: palette(alternate-base); border: 1px solid palette(mid);
            border-radius: 3px; min-height: 6px; max-height: 6px; }
        QProgressBar::chunk { background: palette(highlight); border-radius: 2px; }
        QToolTip { background: palette(tooltip-base); color: palette(tooltip-text); padding: 5px; }
    )"));
}

inline void applyTheme(QApplication& app) {
    app.setStyle("Fusion");
    setDarkMode(app, darkModeEnabled());
}
} // namespace glassboot
