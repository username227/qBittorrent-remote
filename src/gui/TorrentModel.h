// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QAbstractTableModel>
#include <QHash>
#include <QJsonObject>
#include <QStringList>

// Stores one remote server's torrent data. /sync/maindata sends patches,
// not complete torrent objects, except during full_update. Never merge
// records between server sessions; each connection owns its own model.
class TorrentModel final : public QAbstractTableModel
{
    Q_OBJECT
public:
    explicit TorrentModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

    void applySync(const QJsonObject &payload);
    void clear();
    QString hashForRow(int row) const;
    const QHash<QString, QJsonObject> &torrents() const noexcept;

private:
    QHash<QString, QJsonObject> m_torrents;
    QStringList m_hashes;
};
