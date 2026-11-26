// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QComboBox>
#include <QWidget>
#include <functional>

class QLineEdit;
class QToolButton;
class QMenu;
class QAction;
class QSettings;

// Remote adaptation of qBittorrent's FileSystemPathComboEdit.
// Preserve its native combo + small folder-icon button arrangement while
// delegating browsing/translation to the selected server's path mappings.
// Unlike upstream, this has no local-engine Path dependency.
class RemoteFileSystemPathComboEdit final : public QWidget
{
    Q_OBJECT

public:
    explicit RemoteFileSystemPathComboEdit(QWidget *parent = nullptr);

    QComboBox *comboBox() const { return m_combo; }
    QLineEdit *lineEdit() const;
    void setEditable(bool editable);
    void setInsertPolicy(QComboBox::InsertPolicy policy);
    void setCurrentIndex(int index);
    void addItem(const QString &text);
    void setEditText(const QString &text);
    QString currentText() const;

    enum class BrowseMode { Remote, Mounted, OpenMapped };
    struct BrowseAvailability { bool remote; bool mounted; bool openMapped; };
    // Opt-in: other existing path widgets retain their original browse signal.
    void configureFilesystemBrowse(QSettings &settings, const QString &profileId,
        std::function<BrowseAvailability()> availability);
    void refreshFilesystemBrowse();

signals:
    void browseRequested();
    void filesystemBrowseRequested(BrowseMode mode);

private:
    QComboBox *m_combo = nullptr;
    QToolButton *m_browse = nullptr;
    QMenu *m_browseMenu = nullptr;
    QAction *m_remoteBrowse = nullptr;
    QAction *m_mountedBrowse = nullptr;
    QAction *m_openMapped = nullptr;
    QSettings *m_browseSettings = nullptr;
    QString m_browseKey;
    std::function<BrowseAvailability()> m_availability;
    BrowseMode m_effectiveMode = BrowseMode::Remote;
    void chooseBrowseMode(BrowseMode mode);
};
