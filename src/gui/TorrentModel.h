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
    // State palette is available independently of the text-color preference.
    enum { StateColorRole = Qt::UserRole + 1 };

    explicit TorrentModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

    void applySync(const QJsonObject &payload);
    void clear();
    void setHideZeroValues(bool enabled, int mode);
    void setUseStateColors(bool enabled);
    void setPresentation(bool fullTrackerUrl, const QString &dateFormat);
    QString hashForRow(int row) const;
    const QHash<QString, QJsonObject> &torrents() const noexcept;
    bool hasFullSync() const noexcept { return m_hasFullSync; }

private:
    QHash<QString, QJsonObject> m_torrents;
    QStringList m_hashes;
    bool m_hasFullSync = false;
    bool m_hideZeroValues = false;
    int m_hideZeroMode = 0;
    bool m_useStateColors = true;
    bool m_fullTrackerUrl = false;
    QString m_dateFormat;
};
