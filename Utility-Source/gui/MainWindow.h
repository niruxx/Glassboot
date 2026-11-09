#pragma once
#include "BlockDevice.h"
#include "Installer.h"
#include "Payload.h"

#include <QMainWindow>

#include <map>
#include <optional>
#include <string>
#include <vector>

class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QThread;
class QTimer;
class QToolButton;
class QTreeWidget;

namespace glassboot {

class InstallWorker;

// One screen: a list of drives Glassboot can be installed on, and an Install button
// that formats the selected drive.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    // `testImage`/`testImageBytes`: optional disk-image file listed as an extra drive (testing).
    MainWindow(const QString& payloadDir, const QString& testImage = {}, quint64 testImageBytes = 0,
               QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* e) override;

private slots:
    void refreshDrives();
    void updateButtons();
    void startInstall();
    void startUpdate();
    void cancelInstall();
    void openSettings();
    void onProgress(int permille, const QString& step);
    void onLog(const QString& line);
    void onFinished(bool ok, const QString& message);

private:
    void loadPayload(const QString& dir);
    void startWorker(const DiskInfo& d, bool update);
    ExistingInstall probe(const DiskInfo& d) const;
    void setBusy(bool busy);
    const DiskInfo* selectedDrive() const;
    std::vector<std::string> driveSignature() const;

    std::optional<Payload> payload_;
    QString payloadError_;
    std::optional<DiskInfo> testImage_;
    std::vector<DiskInfo> compatible_;
    std::map<std::string, ExistingInstall> installed_; // by DiskInfo::id
    bool lastWasUpdate_ = false;
    std::vector<std::pair<DiskInfo, std::string>> incompatible_;
    bool elevated_ = false;
    bool busy_ = false;

    QTreeWidget* list_ = nullptr;
    QLabel* hiddenNote_ = nullptr;
    QLabel* notice_ = nullptr;
    QLabel* status_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QToolButton* detailsToggle_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QPushButton* cancelButton_ = nullptr;
    QPushButton* installButton_ = nullptr;
    QPushButton* updateButton_ = nullptr;
    QPushButton* configButton_ = nullptr;
    QTimer* hotplug_ = nullptr;

    QThread* thread_ = nullptr;
    InstallWorker* worker_ = nullptr;
};

} // namespace glassboot
