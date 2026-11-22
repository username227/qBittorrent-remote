// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QScrollArea>
#include <QSet>
#include <QString>

class QListWidget;

// Four independent, composable native qBittorrent sidebar filters.
// All counts and categories/tags/tracker hosts come from this server's
// Web API sync state. No client-side tracker requests are ever made.
class FilterSidebar final : public QScrollArea
{
    Q_OBJECT
public:
    explicit FilterSidebar(QWidget *parent = nullptr);

    void refreshFontMetrics();
    void updateCounts(const QHash<QString, QJsonObject> &torrents,
                      const QSet<QString> &categories = {},
                      const QSet<QString> &tags = {},
                      const QHash<QString, QSet<QString>> &trackers = {});
    bool matches(const QString &hash, const QJsonObject &torrent) const;

signals:
    void filtersChanged();

private:
    QListWidget *m_status = nullptr;
    QListWidget *m_categories = nullptr;
    QListWidget *m_tags = nullptr;
    QListWidget *m_trackers = nullptr;
    QHash<QString, QSet<QString>> m_trackerHosts;
    QSet<QString> m_torrentsWithTrackers;
};
