#pragma once
#include "Config.h"

#include <QDialog>
#include <QString>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace glassboot {

// Edits \glassboot\glassboot.cfg on a mounted Glassboot data partition (the GLASSBOOT
// drive). Settings are described in docs/CONFIG.md.
class ConfigDialog : public QDialog {
    Q_OBJECT
public:
    // `root`: the data partition's mount point / drive letter, e.g. "E:/" or "/media/me/GLASSBOOT".
    explicit ConfigDialog(const QString& root, QWidget* parent = nullptr);

    // The GLASSBOOT data partition among a drive's mount points, or empty.
    static QString findDataRoot(const QStringList& mountPoints);

public slots:
    void accept() override;

private slots:
    void setPassword();
    void removePassword();
    void browseTheme();
    void createPersistence();
    void onItemDoubleClicked(QTreeWidgetItem* item, int column);
    void updatePreview();
    void updatePersistButton();

private:
    void load();
    void scanImages();
    QTreeWidgetItem* folderItem(const QString& path);
    void collect(); // widgets -> config_
    QString cfgPath() const;
    void updatePasswordLabel();

    QString root_;
    Config config_;
    QString password_; // current hash ("" = none)

    QTabWidget* tabs_ = nullptr;
    QCheckBox* timeoutOn_ = nullptr;
    QSpinBox* timeout_ = nullptr;
    QComboBox* default_ = nullptr;
    QComboBox* view_ = nullptr;
    QComboBox* language_ = nullptr;
    QLineEdit* theme_ = nullptr;
    QLabel* passwordState_ = nullptr;
    QPushButton* passwordRemove_ = nullptr;
    QTreeWidget* images_ = nullptr;
    QPushButton* persistButton_ = nullptr;
    QPlainTextEdit* preview_ = nullptr;
};

} // namespace glassboot
