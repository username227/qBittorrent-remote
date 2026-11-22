// SPDX-License-Identifier: GPL-3.0-or-later
#include "api/TorrentMetadataReader.h"

#include <QCryptographicHash>
#include <QObject>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QUrl>

#include <limits>

namespace
{
struct Parser
{
    const QByteArray &data;
    qsizetype pos = 0;
    int nodes = 0;
    static constexpr int maxDepth = 48;
    static constexpr int maxNodes = 150000;

    bool bytes(QByteArray &out)
    {
        const qsizetype begin = pos;
        while (pos < data.size() && data[pos] >= '0' && data[pos] <= '9')
            ++pos;
        if (pos == begin || pos >= data.size() || data[pos] != ':'
            || pos - begin > 9)
            return false;
        bool ok = false;
        const qint64 length = data.mid(begin, pos - begin).toLongLong(&ok);
        if (!ok || length < 0 || length > data.size() - pos - 1)
            return false;
        ++pos;
        out = data.mid(pos, static_cast<qsizetype>(length));
        pos += static_cast<qsizetype>(length);
        return true;
    }

    bool value(QVariant &out, int depth = 0)
    {
        if (depth > maxDepth || ++nodes > maxNodes || pos >= data.size())
            return false;
        const char c = data[pos];
        if (c >= '0' && c <= '9')
        {
            QByteArray raw;
            if (!bytes(raw)) return false;
            out = raw;
            return true;
        }
        if (c == 'i')
        {
            const auto start = ++pos;
            while (pos < data.size() && data[pos] != 'e')
                ++pos;
            if (pos >= data.size() || pos == start || pos - start > 22)
                return false;
            bool ok = false;
            const qint64 number = data.mid(start, pos - start).toLongLong(&ok);
            ++pos;
            if (!ok) return false;
            out = number;
            return true;
        }
        if (c == 'l')
        {
            ++pos;
            QVariantList list;
            while (pos < data.size() && data[pos] != 'e')
            {
                QVariant element;
                if (!value(element, depth + 1)) return false;
                list.append(element);
            }
            if (pos >= data.size()) return false;
            ++pos;
            out = list;
            return true;
        }
        if (c == 'd')
        {
            ++pos;
            QVariantMap object;
            QByteArray previous;
            while (pos < data.size() && data[pos] != 'e')
            {
                QByteArray key;
                if (!bytes(key)) return false;
                // Canonical bencoding requires strictly ordered keys,
                // which also rules out duplicate dictionary entries.
                if (!previous.isEmpty() && key <= previous)
                    return false;
                previous = key;
                QVariant item;
                if (!value(item, depth + 1)) return false;
                object.insert(QString::fromUtf8(key), item);
            }
            if (pos >= data.size()) return false;
            ++pos;
            out = object;
            return true;
        }
        return false;
    }
};

QString string(const QVariant &v)
{
    return QString::fromUtf8(v.toByteArray());
}

bool goodPart(const QString &part)
{
    return !part.isEmpty() && part != QLatin1String(".")
        && part != QLatin1String("..")
        && !part.contains(QLatin1Char('/'))
        && !part.contains(QLatin1Char('\\'))
        && !part.contains(QChar::Null);
}

bool appendFile(Remote::TorrentMetadata &out, const QStringList &parts,
                const qint64 size)
{
    if (parts.isEmpty() || size < 0 || out.files.size() >= 100000)
        return false;
    for (const QString &part : parts)
        if (!goodPart(part)) return false;
    if (size > std::numeric_limits<qint64>::max() - out.totalSize)
        return false;
    const QString path = parts.join(QLatin1Char('/'));
    out.files.append({path, size});
    out.totalSize += size;
    return true;
}

bool fromV2Tree(const QVariantMap &tree, const QStringList &prefix,
                Remote::TorrentMetadata &out, int depth)
{
    if (depth > 48) return false;
    for (auto it = tree.cbegin(); it != tree.cend(); ++it)
    {
        if (it.key().isEmpty())
        {
            if (prefix.isEmpty()) return false;
            const QVariantMap leaf = it.value().toMap();
            if (!leaf.contains(QStringLiteral("length"))
                || !appendFile(out, prefix, leaf.value(QStringLiteral("length")).toLongLong()))
                return false;
        }
        else
        {
            if (!goodPart(it.key()) || !it.value().canConvert<QVariantMap>())
                return false;
            QStringList nestedPath = prefix;
            nestedPath.append(it.key());
            if (!fromV2Tree(it.value().toMap(), nestedPath, out, depth + 1))
                return false;
        }
    }
    return true;
}
}

namespace Remote
{
bool TorrentMetadataReader::read(const QByteArray &bytes, TorrentMetadata &out, QString *error)
{
    out = {};
    auto fail = [&error]() {
        if (error) *error = QObject::tr("Could not parse the selected .torrent metadata.");
        return false;
    };
    if (bytes.isEmpty() || bytes.size() > 32 * 1024 * 1024 || bytes.front() != 'd')
        return fail();

    Parser parser {bytes};
    ++parser.pos;
    QVariantMap root;
    QByteArray previous;
    QByteArray rawInfo;
    while (parser.pos < bytes.size() && bytes[parser.pos] != 'e')
    {
        QByteArray key;
        if (!parser.bytes(key) || (!previous.isEmpty() && key <= previous))
            return fail();
        previous = key;
        const qsizetype begin = parser.pos;
        QVariant value;
        if (!parser.value(value)) return fail();
        if (key == "info")
            rawInfo = bytes.mid(begin, parser.pos - begin);
        root.insert(QString::fromUtf8(key), value);
    }
    if (parser.pos >= bytes.size() || bytes[parser.pos++] != 'e'
        || parser.pos != bytes.size() || rawInfo.isEmpty())
        return fail();

    const QVariantMap info = root.value(QStringLiteral("info")).toMap();
    if (info.isEmpty()) return fail();
    // The info dictionary's private flag is covered by its infohash.
    // Native qBittorrent refuses tracker merging when *either* the
    // incoming or already-present torrent is private.
    out.isPrivate = info.value(QStringLiteral("private")).toLongLong() == 1;
    out.name = string(info.value(QStringLiteral("name.utf-8")));
    if (out.name.isEmpty()) out.name = string(info.value(QStringLiteral("name")));
    if (out.name.isEmpty()) return fail();
    out.comment = string(root.value(QStringLiteral("comment.utf-8")));
    if (out.comment.isEmpty()) out.comment = string(root.value(QStringLiteral("comment")));

    // These metadata URLs are the exact trackers offered to merge with an
    // already-present torrent. Never fetch, announce, or otherwise contact
    // any tracker locally. Preserve original order but discard duplicates.
    const auto collectTracker = [&out](const QVariant &value) {
        if (out.trackers.size() >= 256) return;
        const QString raw = string(value).trimmed();
        const QUrl url(raw, QUrl::StrictMode);
        const QString scheme = url.scheme().toLower();
        if (!url.isValid() || url.host().isEmpty()
            || (scheme != QLatin1String("https") && scheme != QLatin1String("http")
                && scheme != QLatin1String("udp"))
            || raw.contains(QLatin1Char('\n')) || raw.contains(QLatin1Char('\r')))
            return;
        if (!out.trackers.contains(raw))
            out.trackers.append(raw);
    };
    collectTracker(root.value(QStringLiteral("announce")));
    for (const QVariant &tier : root.value(QStringLiteral("announce-list")).toList())
    {
        if (tier.canConvert<QVariantList>())
            for (const QVariant &tracker : tier.toList())
                collectTracker(tracker);
        else
            collectTracker(tier);
    }

    const qint64 created = root.value(QStringLiteral("creation date")).toLongLong();
    if (created > 0 && created < 253402300800LL)
        out.created = QDateTime::fromSecsSinceEpoch(created);

    const bool v2 = info.value(QStringLiteral("meta version")).toInt() == 2;
    if (v2)
        out.infoHashV2 = QString::fromLatin1(
            QCryptographicHash::hash(rawInfo, QCryptographicHash::Sha256).toHex());
    // v1 and hybrid infohashes are based on the exact bencoded info span.
    if (!v2 || info.contains(QStringLiteral("pieces")))
        out.infoHashV1 = QString::fromLatin1(
            QCryptographicHash::hash(rawInfo, QCryptographicHash::Sha1).toHex());

    if (info.contains(QStringLiteral("files")))
    {
        const QVariantList files = info.value(QStringLiteral("files")).toList();
        if (files.isEmpty()) return fail();
        for (const QVariant &entry : files)
        {
            const QVariantMap obj = entry.toMap();
            const QVariantList path = obj.contains(QStringLiteral("path.utf-8"))
                ? obj.value(QStringLiteral("path.utf-8")).toList()
                : obj.value(QStringLiteral("path")).toList();
            QStringList components;
            for (const QVariant &piece : path) components.append(string(piece));
            if (!appendFile(out, components, obj.value(QStringLiteral("length")).toLongLong()))
                return fail();
        }
    }
    else if (v2 && info.contains(QStringLiteral("file tree")))
    {
        if (!fromV2Tree(info.value(QStringLiteral("file tree")).toMap(), {}, out, 0))
            return fail();
    }
    else if (info.contains(QStringLiteral("length")))
    {
        if (!appendFile(out, {out.name}, info.value(QStringLiteral("length")).toLongLong()))
            return fail();
    }
    else return fail();

    return !out.files.isEmpty() || fail();
}
} // namespace Remote
