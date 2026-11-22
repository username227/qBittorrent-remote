// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

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

// One independent native transfer view per connected qBittorrent instance.
// The original upstream Qt widgets and icon resources are shared, never
// the server's torrent state, search filters, selection or transfer rates.
class ServerPane final : public QWidget
{
    Q_OBJECT
public:
    enum class TorrentContextAction { Start, Stop, Remove };
    Q_ENUM(TorrentContextAction)

    explicit ServerPane(QWidget *parent = nullptr);

    FilterSidebar *sidebar() const noexcept { return m_sidebar; }
    QTableView *table() const noexcept { return m_table; }
    TorrentModel *model() const noexcept { return m_model; }
    QSortFilterProxyModel *proxy() const noexcept { return m_proxy; }

    void applySync(const QJsonObject &payload);
    QString rateText() const;
    void setSearchTerm(const QString &text);
    const QString &searchTerm() const noexcept { return m_search; }
    void setSearchColumn(int index);
    int searchColumn() const noexcept { return m_searchColumn; }
    void displayTorrentDetails(const QString &hash, int tab, const QJsonValue &data);
    void displayTorrentDetailsError(const QString &hash, int tab, const QString &reason);
    void refreshSelectedDetails();
    void refreshFontMetrics();
    void saveLayout(QSettings &settings, const QString &profileId) const;
    void restoreLayout(QSettings &settings, const QString &profileId);

signals:
    // Native torrent-list context menu delegates existing actions to the
    // owning MainWindow; the active server and its selection remain isolated.
    void torrentContextActionRequested(TorrentContextAction action);
    void torrentDetailsRequested(const QString &hash, int tab);
    void filePriorityRequested(const QString &hash, const QList<int> &ids, int priority);

private:
    void showTorrentContextMenu(const QPoint &position);
    void updateSelectedTorrent();
    void requestSelectedDetails();
    FilterSidebar *m_sidebar = nullptr;
    QTableView *m_table = nullptr;
    TorrentModel *m_model = nullptr;
    QSortFilterProxyModel *m_proxy = nullptr;
    QJsonObject m_serverState;
    QSet<QString> m_categories;
    QSet<QString> m_tags;
    QHash<QString, QSet<QString>> m_trackers;
    QString m_search;
    int m_searchColumn = 0;
    QString m_selectedHash;
    TorrentDetailsWidget *m_details = nullptr;
    PropTabBar *m_detailTabs = nullptr;
    QTimer m_detailsRefresh;
    QSplitter *m_outerSplitter = nullptr;
    QSplitter *m_detailsSplitter = nullptr;
    QByteArray m_expandedDetailsState;
    int m_originalDetailsHandleWidth = -1;
};
