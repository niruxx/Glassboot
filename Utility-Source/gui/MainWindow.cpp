#include "MainWindow.h"

#include "ConfigDialog.h"
#include "Elevation.h"
#include "InstallWorker.h"
#include "Installer.h"
#include "Mbr.h"
#include "Gpt.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QSpinBox>
#include <QFormLayout>
#include <QDateTime>
#include <QFileDialog>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace glassboot {
namespace {

enum Column { ColName, ColSize, ColGlassboot, ColDevice, ColInUse, ColCount };

QString q(const std::string& s) { return QString::fromStdString(s); }

QString driveName(const DiskInfo& d) {
    QString name = q(d.vendor);
    if (!d.model.empty()) name += (name.isEmpty() ? "" : " ") + q(d.model);
    return name.isEmpty() ? QObject::tr("USB drive") : name;
}

QString mountList(const DiskInfo& d) {
    QStringList m;
    for (const auto& p : d.mountPoints) m << q(p);
    return m.join(", ");
}

} // namespace

MainWindow::MainWindow(const QString& payloadDir, const QString& testImage, quint64 testImageBytes, QWidget* parent)
    : QMainWindow(parent) {
    elevated_ = isElevated();
    if (!testImage.isEmpty()) {
        DiskInfo t;
        t.id = kImageIdPrefix + testImage.toStdString();
        t.vendor = "Test image";
        t.sizeBytes = testImageBytes;
        t.usb = true;
        testImage_ = t;
        elevated_ = true; // writing a file needs no special rights
    }
    setWindowTitle(tr("Glassboot Installer"));

    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);
    root->setSpacing(8);

    auto* heading = new QLabel(tr("<h2 style='margin:0'>Glassboot</h2>"
                                  "Choose a USB drive to turn into a multi-ISO boot drive."));
    heading->setWordWrap(true);
    root->addWidget(heading);

    // ---- drive list (compatible drives only)
    list_ = new QTreeWidget;
    list_->setColumnCount(ColCount);
    list_->setHeaderLabels({tr("Drive"), tr("Size"), tr("Glassboot"), tr("Device"), tr("Currently")});
    list_->setRootIsDecorated(false);
    list_->setUniformRowHeights(true);
    list_->setAllColumnsShowFocus(true);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->header()->setStretchLastSection(true);
    list_->header()->setSectionResizeMode(ColName, QHeaderView::Stretch);
    list_->header()->setSectionResizeMode(ColSize, QHeaderView::ResizeToContents);
    list_->header()->setSectionResizeMode(ColGlassboot, QHeaderView::ResizeToContents);
    list_->header()->setSectionResizeMode(ColDevice, QHeaderView::ResizeToContents);
    list_->setMinimumHeight(150);
    root->addWidget(list_, 2);

    auto* listFooter = new QHBoxLayout;
    hiddenNote_ = new QLabel;
    hiddenNote_->setWordWrap(true);
    hiddenNote_->setStyleSheet("color: palette(placeholder-text);");
    refreshButton_ = new QPushButton(tr("Refresh"));
    listFooter->addWidget(hiddenNote_, 1);
    listFooter->addWidget(refreshButton_);
    root->addLayout(listFooter);

    // ---- problems that block installing at all (no admin rights, missing boot files)
    notice_ = new QLabel;
    notice_->setWordWrap(true);
    notice_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    notice_->setVisible(false);
    root->addWidget(notice_);

    auto* diskOptions = new QFormLayout;
    partitionStyle_ = new QComboBox;
    partitionStyle_->addItem(tr("MBR (BIOS and UEFI)"), false);
    partitionStyle_->addItem(tr("GPT (UEFI only, supports disks over 2 TiB)"), true);
    diskOptions->addRow(tr("Partition table:"), partitionStyle_);
    reserve_ = new QSpinBox;
    reserve_->setRange(0, 2000000000);
    reserve_->setSuffix(tr(" MiB"));
    diskOptions->addRow(tr("Reserve at end of disk:"), reserve_);
    root->addLayout(diskOptions);

    // ---- progress
    status_ = new QLabel;
    status_->setWordWrap(true);
    root->addWidget(status_);
    progress_ = new QProgressBar;
    progress_->setRange(0, 1000);
    progress_->setTextVisible(false);
    root->addWidget(progress_);

    detailsToggle_ = new QToolButton;
    detailsToggle_->setText(tr("Show details"));
    detailsToggle_->setCheckable(true);
    detailsToggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    detailsToggle_->setArrowType(Qt::RightArrow);
    detailsToggle_->setAutoRaise(true);
    root->addWidget(detailsToggle_, 0, Qt::AlignLeft);
    log_ = new QPlainTextEdit;
    log_->setReadOnly(true);
    log_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    log_->setMaximumBlockCount(2000);
    log_->setVisible(false);
    root->addWidget(log_, 1);

    // ---- buttons
    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    cancelButton_ = new QPushButton(tr("Cancel"));
    cancelButton_->setVisible(false);
    installButton_ = new QPushButton(tr("&Install"));
    installButton_->setMinimumWidth(120);
    updateButton_ = new QPushButton(tr("&Update"));
    updateButton_->setMinimumWidth(120);
    updateButton_->setToolTip(tr("Refresh Glassboot on this drive. Your ISO files are kept."));
    updateButton_->setVisible(false);
    configButton_ = new QPushButton(tr("&Settings…"));
    configButton_->setToolTip(tr("Boot menu settings: default entry, timeout, names, hidden files, "
                                 "persistence, password (glassboot.cfg on the drive)."));
    configButton_->setVisible(false);
    buttons->insertWidget(0, configButton_);
    buttons->addWidget(cancelButton_);
    buttons->addWidget(installButton_);
    buttons->addWidget(updateButton_);
    root->addLayout(buttons);

    setCentralWidget(central);
    resize(640, 440);

    connect(list_, &QTreeWidget::itemSelectionChanged, this, &MainWindow::updateButtons);
    connect(list_, &QTreeWidget::itemDoubleClicked, this, [this] {
        if (updateButton_->isVisible()) startUpdate();
        else startInstall();
    });
    connect(refreshButton_, &QPushButton::clicked, this, &MainWindow::refreshDrives);
    connect(installButton_, &QPushButton::clicked, this, &MainWindow::startInstall);
    connect(updateButton_, &QPushButton::clicked, this, &MainWindow::startUpdate);
    connect(configButton_, &QPushButton::clicked, this, &MainWindow::openSettings);
    connect(cancelButton_, &QPushButton::clicked, this, &MainWindow::cancelInstall);
    connect(detailsToggle_, &QToolButton::toggled, this, [this](bool on) {
        log_->setVisible(on);
        detailsToggle_->setText(on ? tr("Hide details") : tr("Show details"));
        detailsToggle_->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
    });

    // Re-scan when drives are plugged in or removed.
    hotplug_ = new QTimer(this);
    hotplug_->setInterval(2000);
    connect(hotplug_, &QTimer::timeout, this, [this] {
        if (busy_) return;
        std::vector<std::string> now;
        try {
            for (const auto& d : listDisks()) now.push_back(d.id + ":" + std::to_string(d.sizeBytes));
        } catch (...) {
            return;
        }
        if (testImage_) now.push_back(testImage_->id + ":" + std::to_string(testImage_->sizeBytes));
        std::sort(now.begin(), now.end());
        if (now != driveSignature()) refreshDrives();
    });
    hotplug_->start();

    loadPayload(payloadDir);
    refreshDrives();
}

MainWindow::~MainWindow() {
    if (thread_) {
        if (worker_) worker_->cancel();
        thread_->quit();
        thread_->wait();
    }
}

void MainWindow::loadPayload(const QString& dir) {
    payload_.reset();
    payloadError_.clear();
    if (dir.isEmpty()) {
        payloadError_ = tr("The Glassboot boot files were not found. Reinstall the program, or put the "
                           "<tt>payload</tt> folder next to it.");
    } else {
        try {
            payload_ = Payload::load(dir.toStdString());
        } catch (const std::exception& e) {
            payloadError_ = tr("The Glassboot boot files are damaged: %1").arg(q(e.what()).toHtmlEscaped());
        }
    }

    QStringList problems;
    if (!payloadError_.isEmpty()) problems << payloadError_;
    if (!elevated_)
        problems << tr("Not running as administrator, so drives can't be formatted. Restart the "
                       "installer and allow it to make changes.");
    notice_->setText(problems.isEmpty() ? QString() : "<b>" + problems.join("<br>") + "</b>");
    notice_->setVisible(!problems.isEmpty());
    status_->setText(payload_ ? tr("Glassboot %1 ready.").arg(q(payload_->version)) : QString());
}

std::vector<std::string> MainWindow::driveSignature() const {
    std::vector<std::string> sig;
    for (const auto& d : compatible_) sig.push_back(d.id + ":" + std::to_string(d.sizeBytes));
    for (const auto& [d, why] : incompatible_) sig.push_back(d.id + ":" + std::to_string(d.sizeBytes));
    std::sort(sig.begin(), sig.end());
    return sig;
}

void MainWindow::refreshDrives() {
    if (busy_) return;
    const DiskInfo* prev = selectedDrive();
    const std::string previousId = prev ? prev->id : std::string();
    const uint32_t espMb = payload_ ? payload_->espMb : kDefaultEspMb;

    compatible_.clear();
    incompatible_.clear();
    try {
        for (auto& d : listDisks()) {
            std::string why = incompatibility(d, espMb);
            if (why.empty()) compatible_.push_back(d);
            else incompatible_.emplace_back(d, why);
        }
    } catch (const std::exception& e) {
        onLog(tr("Could not list drives: %1").arg(q(e.what())));
    }
    if (testImage_) compatible_.push_back(*testImage_);
    // listDisks() order is stable; keep the hot-plug signature comparable.
    std::sort(compatible_.begin(), compatible_.end(), [](auto& a, auto& b) { return a.id < b.id; });

    installed_.clear();
    for (const auto& d : compatible_) installed_[d.id] = probe(d);

    list_->clear();
    for (const auto& d : compatible_) {
        auto* item = new QTreeWidgetItem(list_);
        item->setText(ColName, driveName(d));
        item->setText(ColSize, q(formatSize(d.sizeBytes)));
        const auto& ex = installed_[d.id];
        item->setText(ColGlassboot, ex.found ? q(ex.version) : tr("—"));
        item->setText(ColDevice, q(d.id));
        item->setText(ColInUse, d.mountPoints.empty() ? tr("—") : mountList(d));
        item->setData(ColName, Qt::UserRole, q(d.id));
        item->setToolTip(ColName, q(d.displayName()));
        if (d.id == previousId) item->setSelected(true);
    }
    if (compatible_.empty()) {
        auto* item = new QTreeWidgetItem(list_);
        item->setText(ColName, elevated_ || !incompatible_.empty()
                                   ? tr("No compatible USB drives found. Plug one in.")
                                   : tr("No drives visible (administrator rights needed)."));
        item->setFlags(Qt::NoItemFlags);
    } else if (!list_->selectedItems().count() && compatible_.size() == 1) {
        list_->topLevelItem(0)->setSelected(true); // only one choice: preselect it
    }

    // Explain what was left out, so a missing stick isn't a mystery.
    QStringList reasons;
    for (const auto& [d, why] : incompatible_)
        if (!d.system) reasons << QString("%1 (%2): %3").arg(driveName(d), q(formatSize(d.sizeBytes)), q(why));
    size_t hidden = incompatible_.size();
    hiddenNote_->setText(hidden == 0 ? QString()
                                     : tr("%n other drive(s) hidden: not compatible.", "", int(hidden)));
    hiddenNote_->setToolTip(reasons.isEmpty() ? tr("Only the system disk was hidden.") : reasons.join("\n"));
    updateButtons();
}

const DiskInfo* MainWindow::selectedDrive() const {
    const auto items = list_->selectedItems();
    if (items.isEmpty()) return nullptr;
    const QString id = items.first()->data(ColName, Qt::UserRole).toString();
    for (const auto& d : compatible_)
        if (q(d.id) == id) return &d;
    return nullptr;
}

ExistingInstall MainWindow::probe(const DiskInfo& d) const {
    const std::string prefix = kImageIdPrefix;
    if (d.id.rfind(prefix, 0) == 0) {
        try {
            auto img = openImageFile(d.id.substr(prefix.size()), false);
            return detectInstall(*img);
        } catch (...) {
            return {};
        }
    }
    return probeInstall(d);
}

void MainWindow::updateButtons() {
    const DiskInfo* d = selectedDrive();
    const bool ready = !busy_ && d && payload_ && elevated_;
    bool hasGlassboot = false;
    if (d) {
        auto it = installed_.find(d->id);
        hasGlassboot = it != installed_.end() && it->second.found;
    }
    // A drive that already has Glassboot: Update (keeps ISOs) is the main action.
    updateButton_->setVisible(hasGlassboot);
    updateButton_->setEnabled(ready);
    updateButton_->setDefault(hasGlassboot);
    installButton_->setText(hasGlassboot ? tr("Re&install…") : tr("&Install"));
    installButton_->setEnabled(ready);
    installButton_->setDefault(!hasGlassboot);
    // Settings are files on the mounted data partition: no administrator rights needed.
    configButton_->setVisible(hasGlassboot);
    configButton_->setEnabled(!busy_ && d);
}

void MainWindow::openSettings() {
    const DiskInfo* d = selectedDrive();
    if (!d) return;
    QStringList mounts;
    for (const auto& m : d->mountPoints) mounts << q(m);
    QString root = ConfigDialog::findDataRoot(mounts);
    if (root.isEmpty()) {
        QMessageBox::information(this, tr("Glassboot settings"),
                                 tr("The GLASSBOOT drive is not mounted (it has no drive letter or "
                                    "mount point). Choose its folder in the next window."));
        root = QFileDialog::getExistingDirectory(this, tr("Select the GLASSBOOT drive"));
        if (root.isEmpty()) return;
    }
    ConfigDialog dlg(root, this);
    dlg.exec();
}

void MainWindow::setBusy(bool busy) {
    busy_ = busy;
    list_->setEnabled(!busy);
    refreshButton_->setEnabled(!busy);
    partitionStyle_->setEnabled(!busy);
    reserve_->setEnabled(!busy);
    cancelButton_->setVisible(busy);
    cancelButton_->setEnabled(busy);
    updateButtons();
}

void MainWindow::startInstall() {
    const DiskInfo* d = selectedDrive();
    if (!d || !payload_ || !elevated_ || busy_) return;

    try {
        uint64_t reserveBytes = uint64_t(reserve_->value()) << 20;
        if (partitionStyle_->currentData().toBool()) computeGptLayout(d->sizeBytes, payload_->espMb, reserveBytes);
        else Layout::compute(d->sizeBytes, payload_->espMb, reserveBytes);
    } catch (const std::exception& e) {
        QMessageBox::warning(this, tr("Invalid disk layout"), q(e.what()));
        return;
    }

    QString what = QString("<b>%1</b> (%2, %3)").arg(driveName(*d).toHtmlEscaped(), q(formatSize(d->sizeBytes)),
                                                      q(d->id).toHtmlEscaped());
    what += tr("<br>%1; reserved space: %2 MiB").arg(partitionStyle_->currentText()).arg(reserve_->value());
    if (!d->mountPoints.empty()) what += tr("<br>currently %1").arg(mountList(*d).toHtmlEscaped());
    QMessageBox box(QMessageBox::Warning, tr("Format this drive?"),
                    tr("%1<br><br>The drive will be formatted and <b>everything on it will be erased</b>.")
                        .arg(what),
                    QMessageBox::Cancel, this);
    auto* format = box.addButton(tr("&Format and install"), QMessageBox::DestructiveRole);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != format) return;
    startWorker(*d, false);
}

void MainWindow::startUpdate() {
    const DiskInfo* d = selectedDrive();
    if (!d || !payload_ || !elevated_ || busy_) return;
    const auto& ex = installed_[d->id];
    QMessageBox box(QMessageBox::Question, tr("Update Glassboot?"),
                    tr("<b>%1</b> (%2)<br><br>Glassboot %3 on this drive will be updated to %4.<br>"
                       "Your ISO files and everything else on the GLASSBOOT drive are kept.")
                        .arg(driveName(*d).toHtmlEscaped(), q(formatSize(d->sizeBytes)), q(ex.version).toHtmlEscaped(),
                             q(payload_->version).toHtmlEscaped()),
                    QMessageBox::Cancel, this);
    auto* go = box.addButton(tr("&Update"), QMessageBox::AcceptRole);
    box.setDefaultButton(go);
    box.exec();
    if (box.clickedButton() != go) return;
    startWorker(*d, true);
}

void MainWindow::startWorker(const DiskInfo& diskRef, bool update) {
    const DiskInfo d = diskRef; // the list is rebuilt while we run
    lastWasUpdate_ = update;
    log_->clear();
    // Animated "busy" bar until the first real progress arrives (opening and locking the
    // drive can take a few seconds, especially on Windows).
    progress_->setRange(0, 0);
    status_->setText(tr("Preparing the drive…"));
    setBusy(true);

    thread_ = new QThread(this);
    InstallOptions options;
    options.gpt = partitionStyle_->currentData().toBool();
    options.reserveBytes = uint64_t(reserve_->value()) << 20;
    worker_ = new InstallWorker(d, *payload_, update, options);
    worker_->moveToThread(thread_);
    connect(thread_, &QThread::started, worker_, &InstallWorker::run);
    connect(worker_, &InstallWorker::progress, this, &MainWindow::onProgress);
    connect(worker_, &InstallWorker::logLine, this, &MainWindow::onLog);
    connect(worker_, &InstallWorker::finished, this, &MainWindow::onFinished);
    connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);
    thread_->start();
}

void MainWindow::cancelInstall() {
    if (!worker_) return;
    worker_->cancel();
    cancelButton_->setEnabled(false);
    status_->setText(tr("Cancelling…"));
}

void MainWindow::onProgress(int permille, const QString& step) {
    if (permille <= 0 && progress_->maximum() == 0) return; // stay animated until real progress
    progress_->setRange(0, 1000);
    progress_->setValue(permille);
    status_->setText(step + QString("… %1%").arg(permille / 10));
}

void MainWindow::onLog(const QString& line) {
    log_->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss  ") + line);
}

void MainWindow::onFinished(bool ok, const QString& message) {
    thread_->quit();
    thread_->wait();
    thread_->deleteLater();
    thread_ = nullptr;
    worker_ = nullptr;
    setBusy(false);
    if (progress_->maximum() == 0) { // failed before any progress: stop the animation
        progress_->setRange(0, 1000);
        progress_->setValue(0);
    }

    if (ok) {
        progress_->setValue(1000);
        if (lastWasUpdate_) {
            status_->setText(tr("Done. Glassboot updated; your ISO files were kept."));
            QMessageBox::information(this, tr("Glassboot updated"),
                                     tr("Glassboot %1 is now on the drive. Your ISO files were not touched.")
                                         .arg(q(payload_->version)));
        } else {
            status_->setText(tr("Done. Copy .iso files onto the GLASSBOOT drive."));
            QMessageBox::information(this, tr("Glassboot installed"),
                                     tr("The drive is ready.<br><br>Copy .iso files onto the <b>GLASSBOOT</b> "
                                        "drive (any folder), then boot a computer from it."));
        }
    } else {
        onLog(tr("FAILED: %1").arg(message));
        status_->setText(tr("<b>Failed:</b> %1").arg(message.toHtmlEscaped()));
        if (!detailsToggle_->isChecked()) detailsToggle_->setChecked(true);
        QMessageBox::critical(this, tr("Installation failed"),
                              tr("%1<br><br>The drive may not be usable until it is installed again.")
                                  .arg(message.toHtmlEscaped()));
    }
    refreshDrives();
}

void MainWindow::closeEvent(QCloseEvent* e) {
    if (!busy_) { e->accept(); return; }
    e->ignore();
    auto r = QMessageBox::question(this, tr("Installation in progress"),
                                   tr("Stop the installation? The drive will be left unusable."));
    if (r != QMessageBox::Yes || !worker_) return;
    // Close once the worker has stopped.
    connect(worker_, &InstallWorker::finished, this, [this] { QTimer::singleShot(0, this, &QWidget::close); });
    cancelInstall();
}

} // namespace glassboot
