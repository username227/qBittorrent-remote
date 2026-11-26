// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "api/WebSeedUrl.h"
#include "api/TrackerDetailAction.h"

#include "api/PathMappingStore.h"
#include <QJsonObject>
#include <QJsonValue>
#include <QHash>
#include <QSet>
#include <QTimer>
#include <QWidget>

class QSettings;
class QSplitter;
class FilterSidebar;
class QSortFilterProxyModel;
class QTableView;
class TorrentModel;
class TorrentDetailsWidget;
class PropTabBar;
class QTabWidget;
class RemoteRssWidget;
class RemoteExecutionLogWidget;
namespace Remote { class RemoteSession; }

// One independent native transfer view per connected qBittorrent instance.
// The original upstream Qt widgets and icon resources are shared, never
// the server's torrent state, search filters, selection or transfer rates.
class ServerPane final : public QWidget
{
    Q_OBJECT
public:
    enum class TorrentContextAction {
        Start, Stop, ForceStart, Remove, ForceRecheck, ForceReannounce,
        QueueTop, QueueUp, QueueDown, QueueBottom, OpenDestinationFolder,
        SetLocation, Rename, EditTrackers, ManageContent, TorrentOptions,
        SuperSeeding, SequentialDownload, FirstLastPieces, ExportTorrent
    };
    Q_ENUM(TorrentContextAction)

    explicit ServerPane(QWidget *parent = nullptr);

    FilterSidebar *sidebar() const noexcept { return m_sidebar; }
    QTableView *table() const noexcept { return m_table; }
    TorrentModel *model() const noexcept { return m_model; }
    QStringList categories() const { return m_categories.values(); }
    const QJsonObject &categoryDefinitions() const { return m_categoryDefinitions; }
    const QSet<QString> &tagDefinitions() const { return m_tags; }
    const QHash<QString, QSet<QString>> &trackerDefinitions() const { return m_trackers; }
    bool hasFullSync() const { return m_hasFullSync; }
    const QJsonObject &serverState() const noexcept { return m_serverState; }
    bool queueingEnabled() const { return m_serverState.value(QStringLiteral("queueing")).toBool(); }
    void clearQueueCapability();
    QSortFilterProxyModel *proxy() const noexcept { return m_proxy; }

    void setRssAvailable(Remote::RemoteSession *session, bool visible);
    void setRssLoadExternalImages(bool enabled);
    void setSpeedGraphsEnabled(bool enabled);
    void showRss();
    bool isRssSelected() const;
    bool isTransfersSelected() const;
    void setLogAvailable(Remote::RemoteSession *session, bool visible, int types);
    void showLog();
    void showTransfers();
    void applySync(const QJsonObject &payload);
    QString downloadRateText() const;
    QString uploadRateText() const;
    QString freeSpaceText() const;
    QString connectionStatus() const { return m_serverState.value(QStringLiteral("connection_status")).toString(); }
    void setProgressFollowsStateColors(bool enabled);
    void setPresentation(bool fullTrackerUrl, const QString &dateFormat);
    void refreshBytePresentation();
    void setContentPathMappings(const QList<Remote::PathMapping> &mappings);
    void setContentDragEnabled(bool enabled);
    void setSeparateTrackerStatusFilter(bool enabled);
    void setSearchTerm(const QString &text);
    const QString &searchTerm() const noexcept { return m_search; }
    void setSearchColumn(int index);
    int searchColumn() const noexcept { return m_searchColumn; }
    void displayTorrentDetails(const QString &hash, int tab, const QJsonValue &data);
    void displayTorrentDetailsError(const QString &hash, int tab, const QString &reason);
    void refreshSelectedDetails();
    void setTrackerTierEditingSupported(bool supported);
    void setSelectedTrackerReannounceSupported(bool supported);
    void finishTrackerTierEdit(const QString &hash);
    void setCommentEditingSupported(bool supported);
    void finishCommentEdit(const QString &hash);
    void finishPeerAction(const QString &hash, const QString &message);
    void finishWebSeedAction(const QString &hash);
    void refreshFontMetrics();
    void saveLayout(QSettings &settings, const QString &profileId) const;
    void restoreLayout(QSettings &settings, const QString &profileId);

signals:
    void sectionChanged();
    void rssCloseRequested();
    void logCloseRequested();
    // Native torrent-list context menu delegates existing actions to the
    // owning MainWindow; the active server and its selection remain isolated.
    void torrentContextActionRequested(TorrentContextAction action);
    // The menu displays server-owned categories and tags; MainWindow uses
    // the active session's selected hashes when dispatching these commands.
    void torrentCategoryRequested(const QString &category);
    void torrentNewCategoryRequested();
    void torrentTagRequested(const QString &tag, bool add);
    void torrentNewTagsRequested();
    void torrentClearTagsRequested();
    void torrentAutoManagementRequested(bool enabled);
    void torrentModeRequested(TorrentContextAction action, bool enabled);
    void torrentDetailsRequested(const QString &hash, int tab, bool availability);
    void filePriorityRequested(const QString &hash, const QList<int> &ids, int priority);
    void trackerTiersRequested(const QString &hash, const QJsonObject &tiers);
    void trackerDetailActionRequested(const QString &hash, Remote::TrackerDetailAction action,
                                      const QStringList &urls, const QString &replacement);
    void commentEditRequested(const QString &hash, const QString &comment);
    void addPeersRequested(const QString &hash, const QStringList &peers);
    void banPeersRequested(const QString &hash, const QStringList &peers);
    void webSeedActionRequested(const QString &hash, Remote::WebSeedAction action,
                                const QStringList &urls, const QString &replacement);

private:
    void showTorrentContextMenu(const QPoint &position);
    void updateSelectedTorrent();
    void requestSelectedDetails();
    void updateQueueColumn();
    bool m_queueColumnWanted = true;
    QSettings *m_layoutSettings = nullptr;
    QString m_layoutPrefix;
    QTabWidget *m_sectionTabs = nullptr;
    QWidget *m_transfers = nullptr;
    RemoteRssWidget *m_rssWidget = nullptr;
    RemoteExecutionLogWidget *m_logWidget = nullptr;
    void updateSectionTabVisibility();
    void addSectionCloseButton(QWidget *page, const QString &name, bool rss);
    bool m_rssLoadExternalImages = false;
    FilterSidebar *m_sidebar = nullptr;
    QTableView *m_table = nullptr;
    TorrentModel *m_model = nullptr;
    QSortFilterProxyModel *m_proxy = nullptr;
    QJsonObject m_serverState;
    QSet<QString> m_categories;
    QJsonObject m_categoryDefinitions;
    bool m_hasFullSync = false;
    QSet<QString> m_tags;
    QHash<QString, QSet<QString>> m_trackers;
    QString m_search;
    int m_searchColumn = 0;
    QString m_selectedHash;
    TorrentDetailsWidget *m_details = nullptr;
    PropTabBar *m_detailTabs = nullptr;
    bool m_speedGraphsEnabled = true;
    QTimer m_detailsRefresh;
    QSplitter *m_outerSplitter = nullptr;
    QSplitter *m_detailsSplitter = nullptr;
    QByteArray m_expandedDetailsState;
    int m_originalDetailsHandleWidth = -1;
};
