// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/PathMappingStore.h"
#include <QComboBox>
#include <QToolButton>
#include <QWidget>
#include <functional>

class QLineEdit;
class RemoteFilesystemBrowseButton;
class QMenu;
class QAction;
class QSettings;
namespace Remote { class RemoteSession; }

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
    // Fixed-server directory selector; shared by category and relocation dialogs.
    void configureDirectoryBrowse(QSettings &settings, const QString &profileId,
        const QString &serverName, Remote::RemoteSession *session,
        const QList<Remote::PathMapping> &mappings, bool translateTypedLocalPath = false);

signals:
    void browseRequested();
    void filesystemBrowseRequested(BrowseMode mode);

private:
    QComboBox *m_combo = nullptr;
    RemoteFilesystemBrowseButton *m_browse = nullptr;
};

// Shared split-button policy for combo fields, line fields and labelled buttons.
class RemoteFilesystemBrowseButton final : public QToolButton
{
    Q_OBJECT
public:
    using BrowseMode = RemoteFileSystemPathComboEdit::BrowseMode;
    using BrowseAvailability = RemoteFileSystemPathComboEdit::BrowseAvailability;
    struct PathContext {
        QString profileId, serverName;
        Remote::RemoteSession *session = nullptr;
        QList<Remote::PathMapping> mappings;
        QString current;
        bool directory = true;
        QString fileFilter;
    };
    explicit RemoteFilesystemBrowseButton(QWidget *parent = nullptr);
    void configure(QSettings &settings, std::function<QString()> profileId,
        std::function<BrowseAvailability()> availability, std::function<void(BrowseMode)> chosen);
    void configurePath(QSettings &settings, std::function<PathContext()> context,
        std::function<void(const QString &)> selected);
    void refresh();
    static RemoteFilesystemBrowseButton *attachToLineEdit(QLineEdit *line);
signals:
    void preferenceChanged();
private:
    void choose(BrowseMode mode);
    QString preferenceKey() const;
    QMenu *m_browseMenu = nullptr;
    QAction *m_remoteBrowse = nullptr;
    QAction *m_mountedBrowse = nullptr;
    QAction *m_openMapped = nullptr;
    QSettings *m_settings = nullptr;
    std::function<QString()> m_profileId;
    std::function<BrowseAvailability()> m_availability;
    std::function<void(BrowseMode)> m_chosen;
    BrowseMode m_effectiveMode = BrowseMode::Remote;
};
