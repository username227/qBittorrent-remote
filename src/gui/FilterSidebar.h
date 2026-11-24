// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QScrollArea>
#include <QSet>
#include <QString>
#include <QtGlobal>

class QListWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QSettings;
class TransferListFiltersWidgetItem;

// Four unmodified, composable native qBittorrent filter lists, followed by
// a native-styled remote directory tree. No filesystem scans or symlink reads.
class FilterSidebar final : public QScrollArea
{
    Q_OBJECT
public:
    explicit FilterSidebar(QWidget *parent = nullptr);
    enum class Group { Status, Categories, Tags, Trackers, TrackerStatus };
    enum class GroupAction { Add, AddSubcategory, Edit, Remove, RemoveUnused, Start, ForceStart, Stop, RemoveTorrents };
    Q_ENUM(Group)
    Q_ENUM(GroupAction)
    void setGroupActionsEnabled(bool enabled) { m_groupActionsEnabled = enabled; }

    void refreshFontMetrics();
    void setHideZeroStatusFilters(bool hide);
    void setSeparateTrackerStatusFilter(bool separate);
    void updateCounts(const QHash<QString, QJsonObject> &torrents,
                      const QSet<QString> &categories = {},
                      const QSet<QString> &tags = {},
                      const QHash<QString, QSet<QString>> &trackers = {});
    bool matches(const QString &hash, const QJsonObject &torrent) const;
    void saveDirectoryState(QSettings &settings, const QString &layoutRoot) const;
    void restoreDirectoryState(QSettings &settings, const QString &layoutRoot);

signals:
    void filtersChanged();
    void groupActionRequested(Group group, const QString &key, GroupAction action);

private:
    struct DirectoryTotals {
        int directCount = 0;
        int recursiveCount = 0;
        qint64 directBytes = 0;
        qint64 recursiveBytes = 0;
    };
    void updateDirectoryTree(const QHash<QString, QJsonObject> &torrents);
    void updateDirectoryLabels();
    void resizeDirectoryTree();
    void showDirectoryOptions(const QPoint &position, QWidget *source);
    void showGroupOptions(QListWidget *list, Group group, const QPoint &position);
    bool m_groupActionsEnabled = false;

    QListWidget *m_status = nullptr;
    QListWidget *m_categories = nullptr;
    QListWidget *m_tags = nullptr;
    QListWidget *m_trackers = nullptr;
    QListWidget *m_trackerStatuses = nullptr;
    TransferListFiltersWidgetItem *m_trackerStatusSection = nullptr;
    QTreeWidget *m_directories = nullptr;
    TransferListFiltersWidgetItem *m_directorySection = nullptr;
    QHash<QString, QTreeWidgetItem *> m_directoryItems;
    QHash<QString, DirectoryTotals> m_directoryTotals;
    QSet<QString> m_expandedDirectories;
    QString m_selectedDirectory;
    bool m_includeSubfolders = false;
    bool m_hideZeroStatusFilters = false;
    bool m_separateTrackerStatus = false;
    QHash<QString, int> m_statusCounts;
    void applyStatusVisibility();
    bool m_expansionInitialized = false;
    QHash<QString, QSet<QString>> m_trackerHosts;
    QSet<QString> m_torrentsWithTrackers;
};
