#include "MainWindow.h"
#include "TitleBar.h"
#include "GlassWidgets.h"

#include "Elevation.h"
#include "InstallWorker.h"
#include "Installer.h"
#include "Mbr.h"
#include "Gpt.h"
#include "DataFilesystem.h"

#include <QCloseEvent>
#include <QApplication>
#include <QScreen>
#include <QCursor>
#include <QShowEvent>
#include <QScrollArea>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPropertyAnimation>
#include <QVariantAnimation>
#include <QComboBox>
#include <QDateTime>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QStackedWidget>
#include <QFrame>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>

namespace glassboot {
namespace {



bool useGpt(const DiskInfo& disk) {
    return disk.sectorSize == 4096 || disk.sizeBytes > (uint64_t(2) << 40);
}

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
    setWindowFlag(Qt::FramelessWindowHint);
    new DialogMotion(this);

    // The pages scroll independently; the action/status dock never scrolls away.
    auto* shell = new GlassCanvas(this);
    auto* shellLayout = new QVBoxLayout(shell);
    shellLayout->setContentsMargins(0, 0, 0, 0);
    shellLayout->setSpacing(0);
    shellLayout->addWidget(new TitleBar(this));
    auto* pages = new QStackedWidget;
    shellLayout->addWidget(pages, 1);
    setCentralWidget(shell);
    setStyleSheet(R"(
        QMainWindow { background: #edf1fc; color: #202b49; }
        QDialog, QMessageBox { background: #f7f8fe; color: #202b49; }
        QStackedWidget, QScrollArea, QWidget#page { background: transparent; border: none; }
        QWidget { font-size: 14px; }
        QLabel { color: #202b49; background: transparent; }
        QLabel[muted="true"] { color: #52617f; }
        QPushButton { color: #34466e; border: none; padding: 10px 20px; min-height: 24px; font-weight: 600; }
        QDialog QPushButton { background: #e8edfc; border: 1px solid white; border-radius: 18px; }
        QDialog QPushButton:hover { background: #dce4fc; }
        QDialog QPushButton:default { background: #5266c9; color: white; }
        QPushButton:disabled { color: #71809b; }
        QComboBox { background: rgba(255, 255, 255, 160); color: #263657;
            border: 1px solid rgba(255, 255, 255, 240); border-radius: 15px;
            padding: 11px 16px; min-height: 22px; }
        QComboBox:hover { background: rgba(255, 255, 255, 220); border-color: #b4c3ee; }
        QComboBox:focus { border: 1px solid #778ed9; }
        QComboBox:disabled { color: #71809b; background: rgba(255, 255, 255, 90); }
        QComboBox::drop-down { border: none; width: 32px; }
        QComboBox::down-arrow { image: url(:/gui/chevron.xpm); width: 12px; height: 8px; }
        QComboBox QAbstractItemView { background: #f7f8fe; color: #263657;
            selection-background-color: #dce5ff; selection-color: #263657; padding: 8px; border-radius: 12px; }
        QToolButton { color: #5267a5; border: none; padding: 6px; background: transparent; }
        QToolButton:hover { background: rgba(255, 255, 255, 140); border-radius: 10px; }
        QProgressBar { background: rgba(132, 152, 199, 45); border: 1px solid rgba(255, 255, 255, 160);
            border-radius: 4px; height: 8px; max-height: 8px; }
        QProgressBar::chunk { background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
            stop:0 #7eabf0, stop:0.5 #6f7bdd, stop:1 #a68cdb); border-radius: 3px; }
        QPlainTextEdit { background: #f1f4fd; color: #263657; border: 1px solid #d6dff5; border-radius: 14px; padding: 10px; }
        QToolTip { background: #344466; color: white; border: none; padding: 7px; }
        QMessageBox QLabel { color: #202b49; }
        QScrollBar:vertical { background: transparent; width: 7px; margin: 3px 0; }
        QScrollBar::handle:vertical { background: rgba(110, 130, 180, 95); min-height: 24px; border-radius: 3px; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
    )");
    shell->setObjectName("shell");
    auto makePage = [pages](QWidget* content) {
        content->setObjectName("page");
        auto* scroll = new QScrollArea;
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setWidget(content);
        content->setAutoFillBackground(false);
        scroll->viewport()->setAutoFillBackground(false);
        pages->addWidget(scroll);
    };
    auto* welcome = new QWidget;
    auto* intro = new QVBoxLayout(welcome);
    intro->setContentsMargins(24, 24, 24, 24);
    intro->setSpacing(18);
    intro->addStretch();
    auto* mark = new QLabel(QStringLiteral("G"));
    mark->setAlignment(Qt::AlignCenter);
    mark->setFixedSize(80, 80);
    mark->setStyleSheet("background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #b1c9ff, stop:0.45 #7c95e4, stop:1 #9b7ddc); color: white; border: 2px solid rgba(255,255,255,210); border-radius: 28px; font-size: 42px; font-weight: 600;");
    intro->addWidget(mark, 0, Qt::AlignHCenter);
    auto* title = new QLabel(tr("Welcome to Glassboot"));
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet("font-size: 28px; font-weight: 700;");
    title->setWordWrap(true);
    intro->addWidget(title);
    auto* description = new QLabel(tr("One USB drive. All your boot images.\nInstall Glassboot, then copy your ISO files onto the drive."));
    description->setAlignment(Qt::AlignCenter);
    description->setWordWrap(true);
    description->setProperty("muted", true);
    intro->addWidget(description);
    auto* begin = new GlassButton(tr("Get started"));
    begin->setObjectName("getStarted");
    begin->setProperty("primary", true);
    intro->addWidget(begin, 0, Qt::AlignHCenter);
    intro->addStretch();
    makePage(welcome);

    auto* central = new QWidget;
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(20, 20, 20, 20);
    root->setSpacing(12);
    root->addStretch();
    auto* heading = new QLabel(tr("Prepare your USB drive"));
    heading->setWordWrap(true);
    heading->setAlignment(Qt::AlignCenter);
    heading->setStyleSheet("font-size: 26px; font-weight: 700;");
    root->addWidget(heading);
    auto* card = new GlassPanel;
    deviceCard_ = card;
    card->setObjectName("card");
    auto* deviceLayout = new QVBoxLayout(card);
    deviceLayout->setContentsMargins(18, 16, 18, 16);
    deviceLayout->setSpacing(12);
    auto* deviceHeader = new QHBoxLayout;
    auto* deviceLabel = new QLabel(tr("USB drive"));
    deviceLabel->setStyleSheet("font-weight: 700;");
    deviceHeader->addWidget(deviceLabel);
    deviceHeader->addStretch();
    refreshButton_ = new GlassButton(tr("Refresh"));
    deviceHeader->addWidget(refreshButton_);
    deviceLayout->addLayout(deviceHeader);
    list_ = new QComboBox;
    list_->setObjectName("driveSelector");
    list_->setMinimumContentsLength(8);
    list_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    deviceLabel->setBuddy(list_);
    deviceLayout->addWidget(list_);
    driveInfo_ = new QLabel;
    driveInfo_->setWordWrap(true);
    driveInfo_->setMinimumWidth(0);
    driveInfo_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    driveInfo_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    driveInfo_->setProperty("muted", true);
    deviceLayout->addWidget(driveInfo_);
    root->addWidget(card);
    notice_ = new QLabel;
    notice_->setWordWrap(true);
    notice_->setVisible(false);
    root->addWidget(notice_);
    root->addStretch();
    makePage(central);

    auto* dock = new GlassPanel;
    statusPanel_ = dock;
    dock->setObjectName("dock");
    auto* dockLayout = new QVBoxLayout(dock);
    dockLayout->setContentsMargins(20, 12, 20, 16);
    dockLayout->setSpacing(10);
    auto* statusRow = new QHBoxLayout;
    statusDot_ = new QLabel;
    statusDot_->setFixedSize(10, 10);
    statusDot_->setStyleSheet("background: #6b82ce; border-radius: 5px;");
    statusRow->addWidget(statusDot_);
    status_ = new QLabel;
    status_->setObjectName("installStatus");
    status_->setWordWrap(true);
    status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    statusRow->addWidget(status_, 1);
    detailsToggle_ = new QToolButton;
    detailsToggle_->setText(tr("Show details"));
    statusRow->addWidget(detailsToggle_);
    dockLayout->addLayout(statusRow);
    progress_ = new QProgressBar;
    progress_->setObjectName("installProgress");
    progress_->setRange(0, 1000);
    progress_->setValue(0);
    progress_->setTextVisible(false);
    dockLayout->addWidget(progress_);
    auto* actions = new QWidget;
    auto* buttons = new QHBoxLayout(actions);
    buttons->setContentsMargins(0, 0, 0, 0);
    cancelButton_ = new GlassButton(tr("Cancel"));
    cancelButton_->hide();
    installButton_ = new GlassButton(tr("&Install"));
    installButton_->setObjectName("installAction");
    installButton_->setProperty("primary", true);
    updateButton_ = new GlassButton(tr("&Update"));
    updateButton_->setObjectName("updateAction");
    updateButton_->setProperty("primary", true);
    updateButton_->hide();
    buttons->addWidget(cancelButton_);
    buttons->addWidget(installButton_, 1);
    buttons->addWidget(updateButton_, 1);
    dockLayout->addWidget(actions);
    actions->hide();
    shellLayout->addWidget(dock);

    auto* details = new QDialog(this);
    details->setWindowTitle(tr("Show details"));
    auto* detailLayout = new QVBoxLayout(details);
    log_ = new QPlainTextEdit;
    log_->setReadOnly(true);
    log_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    log_->setMaximumBlockCount(2000);
    detailLayout->addWidget(log_);
    auto* closeDetails = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(closeDetails, &QDialogButtonBox::rejected, details, &QDialog::hide);
    detailLayout->addWidget(closeDetails);
    details->resize(520, 300);
    connect(detailsToggle_, &QToolButton::clicked, details, [details] { details->show(); details->raise(); });

    connect(begin, &QPushButton::clicked, this, [this, pages, actions, card] {
        crossfade(pages, [this, pages, actions] {
            pages->setCurrentIndex(1);
            actions->show();
            updateButtons();
            list_->setFocus();
        });
        card->animateChange();
    });
    progressAnimation_ = new QPropertyAnimation(progress_, "value", this);
    progressAnimation_->setDuration(motionDuration(220));
    progressAnimation_->setEasingCurve(QEasingCurve::OutCubic);
    pulse_ = new QVariantAnimation(this);
    pulse_->setStartValue(QColor("#6b82ce"));
    pulse_->setKeyValueAt(0.5, QColor("#b6a8ee"));
    pulse_->setEndValue(QColor("#6b82ce"));
    pulse_->setDuration(1400);
    pulse_->setLoopCount(-1);
    connect(pulse_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        statusDot_->setStyleSheet(QString("background: %1; border-radius: 5px;").arg(value.value<QColor>().name()));
    });
    connect(list_, &QComboBox::currentIndexChanged, this, [this, card] {
        updateButtons();
        card->animateChange();
    });
    connect(refreshButton_, &QPushButton::clicked, this, [this, card] {
        crossfade(card, [this] { refreshDrives(); });
        card->animateChange();
    });
    connect(installButton_, &QPushButton::clicked, this, &MainWindow::startInstall);
    connect(updateButton_, &QPushButton::clicked, this, &MainWindow::startUpdate);
    connect(cancelButton_, &QPushButton::clicked, this, &MainWindow::cancelInstall);

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

    const QSignalBlocker selectionGuard(list_);
    list_->clear();
    list_->setPlaceholderText(tr("Select a USB drive"));
    int previousIndex = -1;
    for (const auto& d : compatible_) {
        list_->addItem(QString("%1 \u2014 %2").arg(driveName(d), q(formatSize(d.sizeBytes))), q(d.id));
        if (d.id == previousId) previousIndex = list_->count() - 1;
    }
    list_->setCurrentIndex(previousIndex >= 0 ? previousIndex : compatible_.size() == 1 ? 0 : -1);
    if (compatible_.empty())
        list_->setPlaceholderText(tr("No compatible USB drives found. Plug one in."));

    // Explain what was left out, so a missing stick isn't a mystery.
    QStringList reasons;
    for (const auto& [d, why] : incompatible_)
        if (!d.system) reasons << QString("%1 (%2): %3").arg(driveName(d), q(formatSize(d.sizeBytes)), q(why));
    refreshButton_->setToolTip(reasons.isEmpty() ? tr("Only the system disk was hidden.") : reasons.join("\n"));
    updateButtons();
}

const DiskInfo* MainWindow::selectedDrive() const {
    if (list_->currentIndex() < 0) return nullptr;
    const QString id = list_->currentData().toString();
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
    if (d) {
        const auto it = installed_.find(d->id);
        const QString state = it != installed_.end() && it->second.found
            ? tr("Glassboot %1 installed \u00b7 Update keeps your files.").arg(q(it->second.version))
            : tr("Ready for a new install \u00b7 This will erase the drive.");
        const QString text = q(d->id) + "\n" + state;
        if (driveInfo_->text() != text) crossfade(driveInfo_, [this, text] { driveInfo_->setText(text); });
    } else {
        const QString text = tr("Choose the drive you want to use. Check its name and capacity.");
        if (driveInfo_->text() != text) crossfade(driveInfo_, [this, text] { driveInfo_->setText(text); });
    }
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
    installButton_->setVisible(!hasGlassboot);
    installButton_->setDefault(!hasGlassboot);
}

void MainWindow::setBusy(bool busy) {
    busy_ = busy;
    deviceCard_->animateChange();
    statusPanel_->animateChange();
    if (busy) pulse_->start();
    else {
        pulse_->stop();
        statusDot_->setStyleSheet("background: #6b82ce; border-radius: 5px;");
    }
    list_->setEnabled(!busy);
    refreshButton_->setEnabled(!busy);
    cancelButton_->setVisible(busy);
    cancelButton_->setEnabled(busy);
    updateButtons();
}

void MainWindow::startInstall() {
    const DiskInfo* selected = selectedDrive();
    if (!selected || !payload_ || !elevated_ || busy_) return;
    // Hot-plug scans can rebuild the list while the confirmation runs its event loop.
    const DiskInfo confirmedDisk = *selected;
    const DiskInfo* d = &confirmedDisk;

    try {
        auto layout = useGpt(*d) ? computeGptLayout(d->sizeBytes, payload_->espMb, 0, d->sectorSize)
                                 : Layout::compute(d->sizeBytes, payload_->espMb);
        checkDataFilesystem("exfat", layout.dataSectors * 512);
    } catch (const std::exception& e) {
        QMessageBox::warning(this, tr("Invalid disk layout"), q(e.what()));
        return;
    }

    QString what = QString("<b>%1</b> (%2, %3)").arg(driveName(*d).toHtmlEscaped(), q(formatSize(d->sizeBytes)),
                                                      q(d->id).toHtmlEscaped());
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
    const DiskInfo* selected = selectedDrive();
    if (!selected || !payload_ || !elevated_ || busy_) return;
    // Hot-plug scans can rebuild the list while the confirmation runs its event loop.
    const DiskInfo confirmedDisk = *selected;
    const DiskInfo* d = &confirmedDisk;
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
    lastStep_.clear();
    // Animated "busy" bar until the first real progress arrives (opening and locking the
    // drive can take a few seconds, especially on Windows).
    progress_->setRange(0, 0);
    status_->setText(tr("Preparing the drive…"));
    setBusy(true);

    thread_ = new QThread(this);
    InstallOptions options;
    options.gpt = useGpt(d);
    options.dataFilesystem = "exfat";
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
    progressAnimation_->stop();
    progressAnimation_->setStartValue(progress_->value());
    progressAnimation_->setEndValue(permille);
    progressAnimation_->start();
    const QString text = step + QString("\u2026 %1%").arg(permille / 10);
    if (lastStep_ != step) {
        lastStep_ = step;
        crossfade(status_, [this, text] { status_->setText(text); });
        statusPanel_->animateChange();
    } else status_->setText(text);
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
    progressAnimation_->stop();
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
        statusDot_->setStyleSheet("background: #b3261e; border-radius: 5px;");
        log_->window()->show();
        QMessageBox::critical(this, tr("Installation failed"),
                              tr("%1<br><br>The drive may not be usable until it is installed again.")
                                  .arg(message.toHtmlEscaped()));
    }
    refreshDrives();
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    if (positioned_) return;
    positioned_ = true;
    // Work in logical pixels on the monitor containing the cursor, including its taskbar.
    QScreen* target = QGuiApplication::screenAt(QCursor::pos());
    if (!target) target = screen();
    const QRect available = target->availableGeometry();
    const QSize wanted(std::min(660, available.width() - 24),
                       std::min(600, available.height() - 48));
    resize(wanted.expandedTo(QSize(280, 240)));
    move(available.center() - rect().center());
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
