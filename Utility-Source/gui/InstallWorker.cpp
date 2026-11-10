#include "InstallWorker.h"

#include "Installer.h"

namespace glassboot {

InstallWorker::InstallWorker(DiskInfo disk, Payload payload, bool update, InstallOptions options)
    : disk_(std::move(disk)), payload_(std::move(payload)), update_(update), options_(options) {}

void InstallWorker::run() {
    try {
        emit logLine(QString::fromStdString("Target: " + disk_.displayName()));
        emit progress(0, tr("Preparing the drive"));
        const std::string prefix = kImageIdPrefix;
        const bool image = disk_.id.rfind(prefix, 0) == 0;
        const std::string imagePath = image ? disk_.id.substr(prefix.size()) : std::string();
        std::unique_ptr<BlockDevice> dev;
        if (image) dev = update_ ? openImageFile(imagePath, true) : createImageFile(imagePath, disk_.sizeBytes);
        else dev = openDisk(disk_);

        InstallCallbacks cb;
        int last = -1;
        cb.progress = [&](double f, const std::string& step) {
            int pm = int(f * 1000);
            if (pm != last) {
                last = pm;
                emit progress(pm, QString::fromStdString(step));
            }
        };
        cb.log = [&](const std::string& line) { emit logLine(QString::fromStdString(line)); };
        if (update_) update(*dev, payload_, cb, cancel_);
        else install(*dev, payload_, cb, cancel_, options_);
        emit finished(true, QString());
    } catch (const std::exception& e) {
        emit finished(false, QString::fromStdString(e.what()));
    }
}

} // namespace glassboot
