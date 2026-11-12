#include "MainWindow.h"
#include "Theme.h"
#include <QComboBox>
#include <QAbstractButton>
#include <QEventLoop>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QScreen>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <algorithm>

static int painterErrors = 0;
static void messages(QtMsgType type, const QMessageLogContext&, const QString& message) {
    if (message.contains("Painter not active") || message.contains("one painter at a time"))
        ++painterErrors;
    std::fprintf(stderr, "%s\n", qPrintable(message));
    if (type == QtFatalMsg) std::abort();
}
static void settle() {
    QEventLoop loop;
    QTimer::singleShot(300, &loop, &QEventLoop::quit);
    loop.exec();
}
static void require(bool condition, const char* message) {
    if (!condition) qFatal("%s", message);
}
static double luminance(const QColor& color) {
    auto linear = [](double channel) {
        return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF()) + 0.0722 * linear(color.blueF());
}
static double contrast(const QColor& foreground, const QColor& background) {
    const auto first = luminance(foreground), second = luminance(background);
    return (std::max(first, second) + 0.05) / (std::min(first, second) + 0.05);
}
static void doubleClick(QWidget* widget) {
    // Use the empty drag area rather than a title-bar control.
    const QPoint point(widget->width() / 3, widget->height() / 2);
    QMouseEvent event(QEvent::MouseButtonDblClick, point, widget->mapToGlobal(point),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
    settle();
}
int main(int argc, char** argv) {
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    qInstallMessageHandler(messages);
    QPalette dark;
    dark.setColor(QPalette::Window, Qt::black);
    dark.setColor(QPalette::WindowText, Qt::white);
    app.setPalette(dark);
    glassboot::applyTheme(app);
    QTemporaryDir temp;
    require(temp.isValid(), "Could not create test directory");
    // This fixture only lists an image. Missing payload prevents all writes.
    glassboot::MainWindow window(temp.path(), temp.filePath("never-written.img"), 1024ULL << 20);
    window.show();
    settle();
    const auto available = window.screen()->availableGeometry();
    require(available.contains(window.geometry()), "Window exceeds available screen at startup");
    require((window.geometry().center() - available.center()).manhattanLength() <= 4,
            "Window is not centered");
    require(window.windowFlags().testFlag(Qt::FramelessWindowHint), "Native title bar is still present");
    auto* maximize = window.findChild<QAbstractButton*>("windowMaximize");
    auto* minimize = window.findChild<QAbstractButton*>("windowMinimize");
    auto* close = window.findChild<QAbstractButton*>("windowClose");
    require(maximize && minimize && close, "Missing window controls");
    maximize->click();
    settle();
    require(window.isMaximized(), "Maximize control failed");
    maximize->click();
    settle();
    require(!window.isMaximized(), "Restore control failed");
    auto* titleBar = window.findChild<QWidget*>("titleBar");
    require(titleBar, "Missing draggable title bar");
    doubleClick(titleBar);
    require(window.isMaximized(), "Title-bar double-click did not maximize");
    doubleClick(titleBar);
    require(!window.isMaximized(), "Title-bar double-click did not restore");
    minimize->click();
    settle();
    require(window.isMinimized(), "Minimize control failed");
    window.showNormal();
    settle();
    auto* begin = window.findChild<QPushButton*>("getStarted");
    auto* install = window.findChild<QPushButton*>("installAction");
    auto* drive = window.findChild<QComboBox*>("driveSelector");
    require(begin && install && drive, "Missing primary controls");
    require(begin->isVisible() && !install->isVisible(), "Welcome flow is incorrect");
    begin->click();
    settle();
    // A host with physical USB drives must not change which device this test targets.
    int fixtureIndex = -1;
    for (int index = 0; index < drive->count(); ++index)
        if (drive->itemData(index).toString().endsWith(temp.filePath("never-written.img"))) fixtureIndex = index;
    require(fixtureIndex >= 0, "Test image is missing from the drive selector");
    drive->setCurrentIndex(fixtureIndex);
    require(drive->currentData().toString().endsWith("never-written.img"), "Wrong drive selected");
    // Interrupt transitions repeatedly, as happens during quick selection and hot-plug changes.
    for (int change = 0; change < 4; ++change) {
        drive->setCurrentIndex(-1);
        app.processEvents();
        drive->setCurrentIndex(fixtureIndex);
        app.processEvents();
    }
    settle();
    require(!install->isEnabled(), "The missing-payload fixture unexpectedly permits writes");
    for (const auto size : {window.size(), QSize(360, 280)}) {
        window.resize(size);
        settle();
        const QRect action(install->mapTo(&window, QPoint()), install->size());
        require(close->isVisible() && window.rect().contains(QRect(close->mapTo(&window, QPoint()), close->size())), "Title bar controls are clipped");
        require(install->isVisible() && window.rect().contains(action), "Install action is clipped");
        require(install->visibleRegion().boundingRect().contains(install->rect()), "Install action is obscured");
    }
    // Disabling and refreshing children after the transition must repaint normally.
    drive->setEnabled(false);
    settle();
    drive->setEnabled(true);
    QMetaObject::invokeMethod(&window, "refreshDrives", Qt::DirectConnection);
    settle();
    window.grab();
    require(painterErrors == 0, "Graphics effects broke child painting after a state change");
    QMessageBox confirmation(QMessageBox::Warning, "Format this drive?",
        "Everything on the selected drive will be erased.", QMessageBox::Cancel, &window);
    confirmation.show();
    settle();
    require(confirmation.palette().color(QPalette::Window).lightness() > 220,
            "Confirmation inherited a dark background");
    require(contrast(confirmation.palette().color(QPalette::WindowText),
                     confirmation.palette().color(QPalette::Window)) >= 4.5,
            "Confirmation text has insufficient contrast");
    confirmation.close();
    close->click();
    settle();
    require(!window.isVisible(), "Close control failed");
    std::puts("Custom title bar controls, centered startup, sticky action, and readable dialogs passed");
    return 0;
}
