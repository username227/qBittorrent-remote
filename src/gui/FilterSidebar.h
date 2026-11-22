// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QScrollArea>
#include <QHash>
#include <QJsonObject>

// Native qBittorrent filter sections with live counts from the remote server.
class FilterSidebar final : public QScrollArea
{
public:
    explicit FilterSidebar(QWidget *parent = nullptr);
    void refreshFontMetrics();
    void updateCounts(const QHash<QString, QJsonObject> &torrents);
};
