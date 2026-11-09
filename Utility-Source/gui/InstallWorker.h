#pragma once
#include "BlockDevice.h"
#include "Payload.h"

#include <QObject>
#include <QString>

#include <atomic>

namespace glassboot {

// DiskInfo ids starting with this prefix name a disk-image file instead of a real disk.
inline constexpr const char* kImageIdPrefix = "image:";

// Runs the installer on a background thread. Create it, move it to a QThread, then
// invoke run() through a queued connection.
class InstallWorker : public QObject {
    Q_OBJECT
public:
    // update: refresh boot files only (keeps the data partition) instead of a full install.
    InstallWorker(DiskInfo disk, Payload payload, bool update = false);
    void cancel() { cancel_ = true; }

public slots:
    void run();

signals:
    void progress(int permille, const QString& step);
    void logLine(const QString& line);
    void finished(bool ok, const QString& message);

private:
    DiskInfo disk_;
    Payload payload_;
    bool update_;
    std::atomic<bool> cancel_{false};
};

} // namespace glassboot
