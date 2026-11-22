// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

namespace Remote
{
struct TorrentMetadataFile
{
    QString path;
    qint64 size = 0;
};

struct TorrentMetadata
{
    QString name;
    QString comment;
    QString infoHashV1;
    QString infoHashV2;
    QStringList trackers; // canonical announce/announce-list URLs; never displayed with passkeys
    bool isPrivate = false;
    QDateTime created;
    QList<TorrentMetadataFile> files;
    qint64 totalSize = 0;
};

// Read-only metadata parser: no libtorrent engine or NAS access on the
// desktop. Input size and recursion depth are bounded. Uploaded metadata
// is still validated independently by the qBittorrent server.
class TorrentMetadataReader final
{
public:
    static bool read(const QByteArray &bencoded, TorrentMetadata &out,
                     QString *error = nullptr);
};
} // namespace Remote
