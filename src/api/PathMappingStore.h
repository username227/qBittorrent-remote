// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QSettings>
#include <QString>

namespace Remote
{

// A mapping relates a locally mounted NAS directory to the corresponding
// absolute POSIX path visible to qBittorrent on the remote server.
// These are nonsecret client settings; no credentials or network sessions.
struct PathMapping
{
    QString localRoot;
    QString remoteRoot;

    bool operator==(const PathMapping &other) const
    {
        return localRoot == other.localRoot && remoteRoot == other.remoteRoot;
    }
};

class PathMappingStore final
{
public:
    explicit PathMappingStore(QSettings &settings);

    QList<PathMapping> mappings(const QString &profileId) const;
    bool replace(const QString &profileId, const QList<PathMapping> &mappings,
                 QString *error = nullptr);
    void remove(const QString &profileId);

    // Syntactic validation only. This cannot verify that a remote directory
    // exists or that an unmounted local directory is currently accessible.
    static bool validate(const QList<PathMapping> &mappings, QString *error = nullptr);

    // Empty means that no configured root matches. Match on a directory
    // boundary, preferring the longest root. Equal local roots use the
    // first configured mapping (the user's preferred remote destination).
    static QString toRemote(const QString &localPath, const QList<PathMapping> &mappings);
    static QString toLocal(const QString &remotePath, const QList<PathMapping> &mappings);

private:
    static QString settingsKey(const QString &profileId);

    QSettings &m_settings;
};

} // namespace Remote
