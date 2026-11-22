// SPDX-License-Identifier: GPL-3.0-or-later
#include "api/PathMappingStore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QSet>
#include <QVariantList>
#include <QVariantMap>

namespace
{
QString localPath(const QString &path)
{
    const QString normalized = QDir::fromNativeSeparators(path.trimmed());
#ifdef Q_OS_WIN
    // The old code saved Z:/ as Z:. Unlike Z:/, Z: is drive-relative
    // (the current directory on Z:), so repair that legacy form as well.
    if (normalized.size() >= 2 && normalized.at(0).isLetter()
        && normalized.at(1) == QLatin1Char(':')
        && (normalized.size() == 2
            || (normalized.size() == 3 && normalized.at(2) == QLatin1Char('/'))))
        return normalized.left(2) + QLatin1Char('/');
#endif
    return QDir::cleanPath(normalized);
}
QString remotePath(const QString &path)
{
    return QDir::cleanPath(path.trimmed());
}
QString stripTrailingSlash(QString path)
{
    // A Windows drive root is Z:/, never Z:. The trailing separator
    // makes it absolute and must be preserved through validation/storage.
    if (path.size() == 3 && path.at(0).isLetter()
        && path.at(1) == QLatin1Char(':')
        && path.at(2) == QLatin1Char('/'))
        return path;
    while (path.endsWith(QLatin1Char('/')) && path.size() > 1)
        path.chop(1);
    return path;
}
bool matchesRoot(const QString &path, const QString &root)
{
    return path == root
        || (root != QLatin1String("/")
            && path.startsWith(root.endsWith(QLatin1Char('/'))
                ? root : root + QLatin1Char('/')));
}
QString joinedPath(const QString &root, const QString &suffix)
{
    // A drive root already ends with '/'; ordinary directory roots do not.
    // Join either kind without losing or duplicating the path boundary.
    const QString tail = suffix.startsWith(QLatin1Char('/'))
        ? suffix.mid(1) : suffix;
    return QDir::cleanPath(root
        + (root.endsWith(QLatin1Char('/')) ? QString() : QStringLiteral("/"))
        + tail);
}
}

namespace Remote
{

PathMappingStore::PathMappingStore(QSettings &settings)
    : m_settings(settings)
{
}

QString PathMappingStore::settingsKey(const QString &profileId)
{
    return QStringLiteral("RemotePaths/Servers/")
        + QString::fromLatin1(QCryptographicHash::hash(
            profileId.toUtf8(), QCryptographicHash::Sha256).toHex());
}

QList<PathMapping> PathMappingStore::mappings(const QString &profileId) const
{
    if (profileId.isEmpty())
        return {};

    QList<PathMapping> entries;
    const QVariantList saved = m_settings.value(settingsKey(profileId)).toList();
    for (const QVariant &value : saved)
    {
        const QVariantMap row = value.toMap();
        const QString local = row.value(QStringLiteral("local")).toString();
        const QString remote = row.value(QStringLiteral("remote")).toString();
        if (!local.isEmpty() && !remote.isEmpty())
            entries.append({localPath(local), remote});
    }
    return entries;
}

bool PathMappingStore::validate(const QList<PathMapping> &mappings, QString *error)
{
    QSet<QString> locals;
    QSet<QString> remotes;
    for (const PathMapping &entry : mappings)
    {
        const QString local = stripTrailingSlash(localPath(entry.localRoot));
        const QString remote = stripTrailingSlash(remotePath(entry.remoteRoot));
        if (entry.localRoot.trimmed().isEmpty()
            || !QDir::isAbsolutePath(local)
            || local == QLatin1String("/")
            || entry.remoteRoot.trimmed().isEmpty()
            || !entry.remoteRoot.trimmed().startsWith(QLatin1Char('/'))
            || entry.remoteRoot.contains(QLatin1Char('\\'))
            || remote == QLatin1String("/"))
        {
            if (error)
                *error = QObject::tr("Both paths must be absolute directories. "
                                     "The remote path must be a POSIX path starting with /, "
                                     "and neither mapping root can be /.");
            return false;
        }
        if (locals.contains(local) || remotes.contains(remote))
        {
            if (error)
                *error = QObject::tr("Each local and remote directory may appear only once "
                                     "in a server's mappings.");
            return false;
        }
        locals.insert(local);
        remotes.insert(remote);
    }
    return true;
}

bool PathMappingStore::replace(const QString &profileId,
                               const QList<PathMapping> &entries, QString *error)
{
    if (profileId.isEmpty())
    {
        if (error)
            *error = QObject::tr("Select a saved server before editing mappings.");
        return false;
    }
    if (!validate(entries, error))
        return false;

    QVariantList encoded;
    for (const PathMapping &entry : entries)
    {
        encoded.append(QVariantMap{
            {QStringLiteral("local"), stripTrailingSlash(localPath(entry.localRoot))},
            {QStringLiteral("remote"), stripTrailingSlash(remotePath(entry.remoteRoot))}
        });
    }

    const QString key = settingsKey(profileId);
    if (encoded.isEmpty())
        m_settings.remove(key);
    else
        m_settings.setValue(key, encoded);
    m_settings.sync();
    if (m_settings.status() == QSettings::NoError)
        return true;
    if (error)
        *error = QObject::tr("Could not save filesystem mappings.");
    return false;
}

void PathMappingStore::remove(const QString &profileId)
{
    if (!profileId.isEmpty())
        m_settings.remove(settingsKey(profileId));
}

QString PathMappingStore::toRemote(const QString &local, const QList<PathMapping> &entries)
{
    const QString path = localPath(local);
    QString bestRoot;
    QString bestDest;
    for (const PathMapping &entry : entries)
    {
        const QString root = stripTrailingSlash(localPath(entry.localRoot));
        if (matchesRoot(path, root) && root.size() > bestRoot.size())
        {
            bestRoot = root;
            bestDest = stripTrailingSlash(remotePath(entry.remoteRoot));
        }
    }
    return bestRoot.isEmpty() ? QString() : joinedPath(bestDest, path.mid(bestRoot.size()));
}

QString PathMappingStore::toLocal(const QString &remote, const QList<PathMapping> &entries)
{
    const QString path = remotePath(remote);
    QString bestRoot;
    QString bestDest;
    for (const PathMapping &entry : entries)
    {
        const QString root = stripTrailingSlash(remotePath(entry.remoteRoot));
        if (matchesRoot(path, root) && root.size() > bestRoot.size())
        {
            bestRoot = root;
            bestDest = stripTrailingSlash(localPath(entry.localRoot));
        }
    }
    return bestRoot.isEmpty() ? QString() : joinedPath(bestDest, path.mid(bestRoot.size()));
}

} // namespace Remote
