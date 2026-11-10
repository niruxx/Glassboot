#include "ConfigDialog.h"

#include "Pbkdf2.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QSaveFile>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace glassboot {
namespace {

enum Column { ColFile, ColAlias, ColArgs, ColPersist, ColClass, ColCount };
constexpr int PathRole = Qt::UserRole;
constexpr int FolderRole = Qt::UserRole + 1;

QString q(const std::string& s) { return QString::fromStdString(s); }
std::string s(const QString& q) { return q.toStdString(); }

// The same files and folders the boot menu lists (gb_scan in the GRUB module).
bool bootableFile(const QString& name) {
    static const QStringList ext{"iso", "wim", "img", "vhd", "vhdx", "efi"};
    return ext.contains(QFileInfo(name).suffix().toLower());
}
bool skippedDir(const QString& name) {
    return name.startsWith('.') || name.startsWith('$') ||
           name.compare("System Volume Information", Qt::CaseInsensitive) == 0 ||
           name.compare("glassboot", Qt::CaseInsensitive) == 0;
}

// Persistence presets: the label and kernel argument each live-system family expects.
struct Preset {
    const char* name;
    const char* label;
    const char* arg;
    bool persistenceConf; // live-boot wants a persistence.conf with "/ union"
};
const Preset kPresets[] = {
    {QT_TRANSLATE_NOOP("glassboot::ConfigDialog", "Ubuntu, Linux Mint, Pop!_OS (casper)"), "casper-rw", "persistent", false},
    {QT_TRANSLATE_NOOP("glassboot::ConfigDialog", "Debian live, Kali (live-boot)"), "persistence", "persistence", true},
    {QT_TRANSLATE_NOOP("glassboot::ConfigDialog", "Arch Linux (archiso)"), "arch-rw", "cow_label=arch-rw", false},
};

QString mkfsExt4() {
    QString p = QStandardPaths::findExecutable("mkfs.ext4");
    if (p.isEmpty()) p = QStandardPaths::findExecutable("mkfs.ext4", {"/usr/sbin", "/sbin"});
    return p;
}

} // namespace

QString ConfigDialog::findDataRoot(const QStringList& mountPoints) {
    for (const auto& mp : mountPoints) {
        QStorageInfo si(mp);
        if (si.isValid() && si.name().compare("GLASSBOOT", Qt::CaseInsensitive) == 0) return si.rootPath();
    }
    for (const auto& mp : mountPoints) { // label changed by the user: look for our folder
        QDir d(mp);
        if (d.exists("glassboot") && !d.exists("EFI")) return d.absolutePath();
    }
    return {};
}

ConfigDialog::ConfigDialog(const QString& root, QWidget* parent) : QDialog(parent), root_(root) {
    setWindowTitle(tr("Glassboot settings - %1").arg(QDir::toNativeSeparators(root)));
    resize(860, 560);
    auto* layout = new QVBoxLayout(this);
    tabs_ = new QTabWidget;
    layout->addWidget(tabs_);

    // ---- Menu
    auto* menuPage = new QWidget;
    auto* form = new QFormLayout(menuPage);

    auto* timeoutRow = new QHBoxLayout;
    timeoutOn_ = new QCheckBox(tr("Start the default entry automatically after"));
    timeout_ = new QSpinBox;
    timeout_->setRange(0, 3600);
    timeout_->setSuffix(tr(" seconds"));
    timeout_->setValue(10);
    timeoutRow->addWidget(timeoutOn_);
    timeoutRow->addWidget(timeout_);
    timeoutRow->addStretch();
    connect(timeoutOn_, &QCheckBox::toggled, timeout_, &QWidget::setEnabled);
    form->addRow(tr("Timeout:"), timeoutRow);

    default_ = new QComboBox;
    form->addRow(tr("Default entry:"), default_);

    view_ = new QComboBox;
    view_->addItem(tr("List: every file, with its folder"), "list");
    view_->addItem(tr("Tree: folders are sub-menus"), "tree");
    form->addRow(tr("Menu layout:"), view_);

    auto* themeRow = new QHBoxLayout;
    theme_ = new QLineEdit;
    theme_->setPlaceholderText(tr("none (text menu)"));
    auto* themeBrowse = new QPushButton(tr("Browse..."));
    connect(themeBrowse, &QPushButton::clicked, this, &ConfigDialog::browseTheme);
    themeRow->addWidget(theme_);
    themeRow->addWidget(themeBrowse);
    form->addRow(tr("Theme:"), themeRow);
    language_ = new QComboBox;
    language_->addItem(tr("English"), "en");
    language_->addItem(tr("Spanish"), "es");
    form->addRow(tr("Boot menu language:"), language_);

    auto* pwRow = new QHBoxLayout;
    passwordState_ = new QLabel;
    auto* pwSet = new QPushButton(tr("Set password..."));
    passwordRemove_ = new QPushButton(tr("Remove"));
    connect(pwSet, &QPushButton::clicked, this, &ConfigDialog::setPassword);
    connect(passwordRemove_, &QPushButton::clicked, this, &ConfigDialog::removePassword);
    pwRow->addWidget(passwordState_, 1);
    pwRow->addWidget(pwSet);
    pwRow->addWidget(passwordRemove_);
    form->addRow(tr("Password:"), pwRow);
    tabs_->addTab(menuPage, tr("Menu"));

    // ---- Images
    auto* imgPage = new QWidget;
    auto* imgLayout = new QVBoxLayout(imgPage);
    auto* hint = new QLabel(tr("Untick a file or folder to hide it from the boot menu. Double-click a "
                               "cell to give an entry a menu name, extra Linux kernel arguments, or a "
                               "persistence file."));
    hint->setWordWrap(true);
    imgLayout->addWidget(hint);
    images_ = new QTreeWidget;
    images_->setColumnCount(ColCount);
    images_->setHeaderLabels({tr("File"), tr("Menu name"), tr("Kernel arguments"), tr("Persistence file"), tr("Icon class")});
    images_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    images_->setUniformRowHeights(true);
    images_->setAllColumnsShowFocus(true);
    connect(images_, &QTreeWidget::itemDoubleClicked, this, &ConfigDialog::onItemDoubleClicked);
    connect(images_, &QTreeWidget::currentItemChanged, this, &ConfigDialog::updatePersistButton);
    imgLayout->addWidget(images_);
    auto* imgButtons = new QHBoxLayout;
    persistButton_ = new QPushButton(tr("Create persistence file..."));
    connect(persistButton_, &QPushButton::clicked, this, &ConfigDialog::createPersistence);
    imgButtons->addWidget(persistButton_);
    imgButtons->addStretch();
    imgLayout->addLayout(imgButtons);
    tabs_->addTab(imgPage, tr("Images"));

    // ---- File preview
    preview_ = new QPlainTextEdit;
    preview_->setReadOnly(true);
    preview_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    tabs_->addTab(preview_, tr("glassboot.cfg"));
    connect(tabs_, &QTabWidget::currentChanged, this, &ConfigDialog::updatePreview);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &ConfigDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &ConfigDialog::reject);
    layout->addWidget(buttons);

    load();
}

QString ConfigDialog::cfgPath() const { return QDir(root_).filePath("glassboot/glassboot.cfg"); }

void ConfigDialog::load() {
    QFile f(cfgPath());
    if (f.open(QIODevice::ReadOnly)) config_ = Config::parse(f.readAll().toStdString());

    bool ok = false;
    int t = q(config_.global("timeout")).toInt(&ok);
    timeoutOn_->setChecked(ok && t >= 0);
    timeout_->setEnabled(timeoutOn_->isChecked());
    if (ok && t >= 0) timeout_->setValue(t);
    view_->setCurrentIndex(config_.global("view") == "tree" ? 1 : 0);
    theme_->setText(q(config_.global("theme")));
    language_->setCurrentIndex(config_.global("language") == "es" ? 1 : 0);
    password_ = q(config_.global("password"));
    updatePasswordLabel();

    scanImages();

    default_->addItem(tr("(the first entry)"), QString());
    std::function<void(QTreeWidgetItem*)> addFiles = [&](QTreeWidgetItem* it) {
        for (int i = 0; i < it->childCount(); ++i) {
            auto* c = it->child(i);
            if (c->data(ColFile, FolderRole).toBool()) addFiles(c);
            else default_->addItem(c->data(ColFile, PathRole).toString(), c->data(ColFile, PathRole));
        }
    };
    addFiles(images_->invisibleRootItem());
    QString def = q(config_.global("default"));
    if (!def.isEmpty()) {
        int i = default_->findData(def, Qt::UserRole, Qt::MatchFixedString); // case-insensitive
        if (i < 0) {
            default_->addItem(tr("%1 (not on the drive)").arg(def), def);
            i = default_->count() - 1;
        }
        default_->setCurrentIndex(i);
    }
    updatePersistButton();
}

QTreeWidgetItem* ConfigDialog::folderItem(const QString& path) {
    QTreeWidgetItem* parent = images_->invisibleRootItem();
    QString cur;
    for (const auto& part : path.split('/', Qt::SkipEmptyParts)) {
        cur += "/" + part;
        QTreeWidgetItem* found = nullptr;
        for (int i = 0; i < parent->childCount() && !found; ++i)
            if (parent->child(i)->data(ColFile, PathRole).toString() == cur) found = parent->child(i);
        if (!found) {
            found = new QTreeWidgetItem(parent);
            found->setText(ColFile, part + "/");
            found->setData(ColFile, PathRole, cur);
            found->setData(ColFile, FolderRole, true);
            found->setFlags(found->flags() | Qt::ItemIsUserCheckable);
            found->setCheckState(ColFile, config_.hidden(s(cur)) ? Qt::Unchecked : Qt::Checked);
            found->setExpanded(true);
        }
        parent = found;
    }
    return parent;
}

void ConfigDialog::scanImages() {
    images_->clear();
    std::function<void(const QString&)> walk = [&](const QString& rel) {
        QDir dir(QDir(root_).filePath(rel.mid(1)));
        const auto entries = dir.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden,
                                               QDir::Name | QDir::IgnoreCase | QDir::DirsFirst);
        for (const auto& e : entries) {
            QString path = rel + (rel.endsWith('/') ? "" : "/") + e.fileName();
            if (e.isDir()) {
                if (!skippedDir(e.fileName())) walk(path);
            } else if (bootableFile(e.fileName())) {
                auto* it = new QTreeWidgetItem(folderItem(rel));
                it->setText(ColFile, e.fileName());
                it->setToolTip(ColFile, path);
                it->setData(ColFile, PathRole, path);
                it->setData(ColFile, FolderRole, false);
                it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
                it->setCheckState(ColFile, config_.hidden(s(path)) ? Qt::Unchecked : Qt::Checked);
                it->setText(ColAlias, q(config_.value(Config::Kind::Alias, s(path))));
                it->setText(ColArgs, q(config_.value(Config::Kind::Args, s(path))));
                it->setText(ColPersist, q(config_.value(Config::Kind::Persist, s(path))));
                it->setText(ColClass, q(config_.value(Config::Kind::Class, s(path))));
            }
        }
    };
    walk("/");
    for (int c = 0; c < ColCount; ++c) {
        images_->resizeColumnToContents(c);
        images_->setColumnWidth(c, std::max(images_->columnWidth(c) + 12, 120));
    }
}

void ConfigDialog::onItemDoubleClicked(QTreeWidgetItem* item, int column) {
    if (!item || item->data(ColFile, FolderRole).toBool() || column == ColFile) return;
    item->setFlags(item->flags() | Qt::ItemIsEditable);
    images_->editItem(item, column);
}

void ConfigDialog::updatePersistButton() {
    auto* it = images_->currentItem();
    bool iso = it && !it->data(ColFile, FolderRole).toBool() &&
               it->data(ColFile, PathRole).toString().endsWith(".iso", Qt::CaseInsensitive);
#ifdef Q_OS_WIN
    persistButton_->setEnabled(false);
    persistButton_->setToolTip(tr("Persistence files hold a Linux (ext4) file system and can only be "
                                  "created on Linux for now: scripts/make-persistence.sh, or this "
                                  "installer on Linux."));
#else
    persistButton_->setEnabled(iso);
    persistButton_->setToolTip(iso ? QString() : tr("Select a Linux ISO first."));
#endif
}

void ConfigDialog::updatePasswordLabel() {
    passwordState_->setText(password_.isEmpty()
                                ? tr("Not set: anyone can start any entry.")
                                : tr("Set: user name \"glassboot\" and the password are asked before booting."));
    passwordRemove_->setEnabled(!password_.isEmpty());
}

void ConfigDialog::setPassword() {
    QDialog d(this);
    d.setWindowTitle(tr("Menu password"));
    auto* f = new QFormLayout(&d);
    auto* p1 = new QLineEdit;
    auto* p2 = new QLineEdit;
    p1->setEchoMode(QLineEdit::Password);
    p2->setEchoMode(QLineEdit::Password);
    auto* note = new QLabel(tr("The boot menu will ask for the user name <b>glassboot</b> and this "
                               "password. Only a salted PBKDF2 hash is stored on the drive."));
    note->setWordWrap(true);
    f->addRow(note);
    f->addRow(tr("Password:"), p1);
    f->addRow(tr("Again:"), p2);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    f->addRow(bb);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    connect(bb, &QDialogButtonBox::accepted, &d, [&] {
        if (p1->text().isEmpty()) return;
        if (p1->text() != p2->text()) {
            QMessageBox::warning(&d, tr("Menu password"), tr("The passwords don't match."));
            return;
        }
        d.accept();
    });
    if (d.exec() != QDialog::Accepted) return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    password_ = q(grubPasswordHash(p1->text().toStdString()));
    QApplication::restoreOverrideCursor();
    updatePasswordLabel();
}

void ConfigDialog::removePassword() {
    password_.clear();
    updatePasswordLabel();
}

void ConfigDialog::browseTheme() {
    QString f = QFileDialog::getOpenFileName(this, tr("Choose a GRUB theme file on the Glassboot drive"), root_,
                                             tr("GRUB theme (*.txt)"));
    if (f.isEmpty()) return;
    QString rel = QDir(root_).relativeFilePath(f);
    if (rel.startsWith("..") || QDir::isAbsolutePath(rel)) {
        QMessageBox::warning(this, tr("Theme"),
                             tr("The theme must be stored on the Glassboot drive. Copy its folder "
                                "(e.g. into glassboot\\themes) and choose it again."));
        return;
    }
    theme_->setText("/" + rel);
}

void ConfigDialog::createPersistence() {
    auto* it = images_->currentItem();
    if (!it || it->data(ColFile, FolderRole).toBool()) return;
    const QString iso = it->data(ColFile, PathRole).toString();
    const QString mkfs = mkfsExt4();
    if (mkfs.isEmpty()) {
        QMessageBox::warning(this, tr("Persistence"), tr("mkfs.ext4 was not found. Install e2fsprogs."));
        return;
    }

    QDialog d(this);
    d.setWindowTitle(tr("Create a persistence file"));
    auto* f = new QFormLayout(&d);
    auto* note = new QLabel(tr("Changes made in the live system (files, settings, installed programs) "
                               "will be kept in this file on the Glassboot drive."));
    note->setWordWrap(true);
    f->addRow(note);
    f->addRow(tr("ISO:"), new QLabel(iso));
    auto* family = new QComboBox;
    for (const auto& p : kPresets) family->addItem(tr(p.name));
    f->addRow(tr("Distribution:"), family);
    auto* size = new QSpinBox;
    qint64 freeGiB = QStorageInfo(root_).bytesAvailable() >> 30;
    size->setRange(1, int(std::max<qint64>(1, std::min<qint64>(freeGiB - 1, 1024))));
    size->setValue(std::min(4, size->maximum()));
    size->setSuffix(tr(" GiB"));
    f->addRow(tr("Size:"), size);
    auto* name = new QLineEdit("/glassboot/persistence/" + QFileInfo(iso).completeBaseName() + ".dat");
    f->addRow(tr("File:"), name);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    f->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    if (d.exec() != QDialog::Accepted) return;

    const Preset& preset = kPresets[family->currentIndex()];
    const QString rel = q(Config::normPath(s(name->text())));
    const QString file = QDir(root_).filePath(rel.mid(1));
    if (QFileInfo::exists(file) &&
        QMessageBox::question(this, tr("Persistence"), tr("%1 already exists. Replace it (its contents are lost)?").arg(rel)) !=
            QMessageBox::Yes)
        return;

    // Allocate the file in one go (keeps it in one piece on a freshly formatted drive), then
    // put an ext4 file system with the expected label into it.
    const qint64 bytes = qint64(size->value()) << 30;
    QString error;
    QProgressDialog progress(tr("Creating %1 (%2 GiB)...").arg(rel).arg(size->value()), QString(), 0, 0, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.show();
    QThread* t = QThread::create([&] {
        QDir().mkpath(QFileInfo(file).absolutePath());
        QFile::remove(file);
        QFile out(file);
        if (!out.open(QIODevice::WriteOnly) || !out.resize(bytes)) {
            error = tr("Could not create %1: %2").arg(rel, out.errorString());
            out.remove();
            return;
        }
        out.close();
        QStringList args{"-q", "-F", "-L", QString::fromLatin1(preset.label), "-E", "nodiscard"};
        QTemporaryDir content;
        if (preset.persistenceConf) {
            QFile conf(content.filePath("persistence.conf"));
            if (conf.open(QIODevice::WriteOnly)) conf.write("/ union\n");
            args << "-d" << content.path();
        }
        args << file;
        QProcess p;
        p.start(mkfs, args);
        if (!p.waitForFinished(-1) || p.exitCode() != 0) {
            error = tr("mkfs.ext4 failed: %1").arg(QString::fromLocal8Bit(p.readAllStandardError()).trimmed());
            QFile::remove(file);
        }
    });
    QEventLoop loop;
    connect(t, &QThread::finished, &loop, &QEventLoop::quit);
    t->start();
    loop.exec();
    t->deleteLater();
    progress.close();
    if (!error.isEmpty()) {
        QMessageBox::warning(this, tr("Persistence"), error);
        return;
    }

    it->setText(ColPersist, rel);
    QStringList words = it->text(ColArgs).split(' ', Qt::SkipEmptyParts);
    for (const auto& w : QString::fromLatin1(preset.arg).split(' '))
        if (!words.contains(w)) words << w;
    it->setText(ColArgs, words.join(' '));
    QMessageBox::information(this, tr("Persistence"),
                             tr("Created %1. Save the settings to use it.").arg(rel));
}

void ConfigDialog::collect() {
    auto setOr = [&](const char* key, const QString& value, const QString& dflt) {
        // Write a setting only when it differs from the default or was already in the file.
        if (value == dflt && config_.global(key).empty()) return;
        config_.setGlobal(key, s(value));
    };
    if (timeoutOn_->isChecked()) config_.setGlobal("timeout", std::to_string(timeout_->value()));
    else config_.setGlobal("timeout", "");
    config_.setGlobal("default", s(default_->currentData().toString()));
    setOr("view", view_->currentData().toString(), "list");
    QString theme = theme_->text().trimmed();
    config_.setGlobal("theme", theme.isEmpty() ? std::string() : Config::normPath(s(theme)));
    config_.setGlobal("password", s(password_));
    setOr("language", language_->currentData().toString(), "en");

    std::function<void(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* parent) {
        for (int i = 0; i < parent->childCount(); ++i) {
            auto* it = parent->child(i);
            std::string path = s(it->data(ColFile, PathRole).toString());
            config_.setHidden(path, it->checkState(ColFile) == Qt::Unchecked);
            if (it->data(ColFile, FolderRole).toBool()) {
                walk(it);
                continue;
            }
            config_.setValue(Config::Kind::Alias, path, s(it->text(ColAlias)));
            config_.setValue(Config::Kind::Args, path, s(it->text(ColArgs)));
            config_.setValue(Config::Kind::Persist, path, s(it->text(ColPersist)));
            config_.setValue(Config::Kind::Class, path, s(it->text(ColClass)));
        }
    };
    walk(images_->invisibleRootItem());
}

void ConfigDialog::updatePreview() {
    if (tabs_->currentWidget() != preview_) return;
    collect();
    preview_->setPlainText(q(config_.serialize()));
}

void ConfigDialog::accept() {
    collect();
    QDir().mkpath(QDir(root_).filePath("glassboot"));
    QSaveFile f(cfgPath());
    QByteArray data = QByteArray::fromStdString(config_.serialize());
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit()) {
        QMessageBox::warning(this, tr("Glassboot settings"),
                             tr("Could not save %1: %2").arg(QDir::toNativeSeparators(cfgPath()), f.errorString()));
        return;
    }
    QDialog::accept();
}

} // namespace glassboot
