#include "Elevation.h"

#include <QCoreApplication>
#include <QProcess>
#include <QStandardPaths>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#endif

namespace glassboot {

bool isElevated() {
#ifdef Q_OS_WIN
    BOOL admin = FALSE;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    PSID group = nullptr;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &group)) {
        CheckTokenMembership(nullptr, group, &admin);
        FreeSid(group);
    }
    return admin;
#else
    return ::geteuid() == 0;
#endif
}

bool relaunchElevated(const QStringList& args) {
    QString exe = QCoreApplication::applicationFilePath();
#ifdef Q_OS_WIN
    QString params;
    for (const auto& a : args) params += "\"" + QString(a).replace("\"", "\\\"") + "\" ";
    auto r = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"runas", reinterpret_cast<LPCWSTR>(exe.utf16()),
                                                     reinterpret_cast<LPCWSTR>(params.utf16()), nullptr, SW_SHOWNORMAL));
    return r > 32;
#else
    // Relaunch the outer AppImage so root obtains its own mount/extraction and
    // bundled runtime environment rather than an executable on a user's FUSE mount.
    if (!qEnvironmentVariable("APPIMAGE").isEmpty()) exe = qEnvironmentVariable("APPIMAGE");
    const QString pkexec = QStandardPaths::findExecutable("pkexec");
    if (pkexec.isEmpty()) return false;
    // pkexec clears the environment; pass through what a GUI needs to reach the display.
    QStringList full{"env"};
    if (!qEnvironmentVariable("APPIMAGE").isEmpty()) full << "APPIMAGE_EXTRACT_AND_RUN=1";
    for (const char* var : {"DISPLAY", "XAUTHORITY", "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR", "XDG_SESSION_TYPE",
                            "QT_QPA_PLATFORM", "LANG"}) {
        QByteArray v = qgetenv(var);
        if (!v.isEmpty()) full << QString("%1=%2").arg(var, QString::fromLocal8Bit(v));
    }
    full << exe << args;
    return QProcess::startDetached(pkexec, full);
#endif
}

} // namespace glassboot
