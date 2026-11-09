#include "ConfigDialog.h"
#include "Elevation.h"
#include "MainWindow.h"
#include "Payload.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QLibraryInfo>
#include <QLocale>
#include <QMessageBox>
#include <QTranslator>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("Glassboot Installer");
    QApplication::setApplicationVersion(GLASSBOOT_INSTALLER_VERSION);

    // Translations (translations/glassboot_<lang>.ts, compiled into the program), plus Qt's
    // own strings (standard buttons) when Qt's translations are installed.
    QTranslator appTr, qtTr;
    if (appTr.load(QLocale(), "glassboot", "_", ":/i18n")) app.installTranslator(&appTr);
    if (qtTr.load(QLocale(), "qtbase", "_", QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        app.installTranslator(&qtTr);

    QCommandLineParser parser;
    parser.setApplicationDescription("Creates a Glassboot multi-ISO USB drive.");
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption payloadOpt("payload", "Directory containing the Glassboot payload.", "dir");
    QCommandLineOption noElevateOpt("no-elevate", "Don't ask to restart with administrator rights.");
    // Developer/testing aid: offer a disk-image file as an extra "drive" so the whole GUI
    // flow (list, confirm, progress, verify) can be exercised without real hardware.
    QCommandLineOption testImageOpt("test-image", "Also list FILE as a drive (writes a disk image).", "file");
    QCommandLineOption testSizeOpt("test-size", "Size of --test-image in MiB (default 4096).", "mib", "4096");
    testImageOpt.setFlags(QCommandLineOption::HiddenFromHelp);
    testSizeOpt.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOption(payloadOpt);
    parser.addOption(noElevateOpt);
    parser.addOption(testImageOpt);
    parser.addOption(testSizeOpt);
    QCommandLineOption settingsOpt("settings", "Only edit the boot menu settings of the Glassboot drive mounted at DIR.",
                                   "dir");
    parser.addOption(settingsOpt);
    parser.process(app);

    if (parser.isSet(settingsOpt)) {
        glassboot::ConfigDialog dlg(parser.value(settingsOpt));
        return dlg.exec() == QDialog::Accepted ? 0 : 1;
    }

    QString payloadDir = parser.value(payloadOpt);
    if (payloadDir.isEmpty())
        payloadDir = QString::fromStdString(
            glassboot::Payload::locate(QCoreApplication::applicationDirPath().toStdString()));

    // On Windows the manifest already requests administrator rights. On Linux, offer to
    // restart through pkexec so the user doesn't have to start the GUI with sudo.
    if (!glassboot::isElevated() && !parser.isSet(noElevateOpt)) {
        auto r = QMessageBox::question(nullptr, QObject::tr("Administrator rights needed"),
                                       QObject::tr("Writing to a USB drive needs administrator (root) rights.\n\n"
                                                   "Restart the installer with administrator rights?"));
        if (r == QMessageBox::Yes) {
            QStringList args{"--no-elevate"};
            if (!payloadDir.isEmpty()) args << "--payload" << payloadDir;
            if (glassboot::relaunchElevated(args)) return 0;
            QMessageBox::warning(nullptr, QObject::tr("Could not restart"),
                                 QObject::tr("Could not obtain administrator rights. Drives will be listed "
                                             "but cannot be written."));
        }
    }

    glassboot::MainWindow w(payloadDir, parser.value(testImageOpt), parser.value(testSizeOpt).toULongLong() << 20);
    w.show();
    return app.exec();
}
