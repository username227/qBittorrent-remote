// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonObject>
#include <QWidget>

class FilterSidebar;
class QSortFilterProxyModel;
class QTableView;
class TorrentModel;

// One independent native transfer view per connected qBittorrent instance.
// The original upstream Qt widgets and icon resources are shared, never
// the server's torrent state, search filters, selection or transfer rates.
class ServerPane final : public QWidget
{
public:
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

private:
    FilterSidebar *m_sidebar = nullptr;
    QTableView *m_table = nullptr;
    TorrentModel *m_model = nullptr;
    QSortFilterProxyModel *m_proxy = nullptr;
    QJsonObject m_serverState;
    QString m_search;
    int m_searchColumn = 0;
};
