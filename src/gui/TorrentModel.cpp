// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/TorrentModel.h"
#include "gui/BytePresentation.h"
#include "gui/DatePresentation.h"
#include <QUrl>
#include "gui/ProgressDisplay.h"

#include <QDateTime>
#include <QIcon>
#include <QGuiApplication>
#include <QPalette>
#include <QColor>
#include <QJsonArray>
#include <QJsonValue>
#include <QLocale>

namespace
{
const QStringList &columnNames()
{
    static const QStringList names {
        QStringLiteral("Name"), QStringLiteral("Size"), QStringLiteral("Progress"),
        QStringLiteral("Status"), QStringLiteral("Seeds"), QStringLiteral("Peers"),
        QStringLiteral("Down Speed"), QStringLiteral("Up Speed"), QStringLiteral("ETA"),
        QStringLiteral("Ratio"), QStringLiteral("Category"), QStringLiteral("Tags"),
        QStringLiteral("Availability"), QStringLiteral("Added On"),
        QStringLiteral("Completed On"), QStringLiteral("Created On"),
        QStringLiteral("Tracker"), QStringLiteral("Down Limit"), QStringLiteral("Up Limit"),
        QStringLiteral("Downloaded"), QStringLiteral("Uploaded"),
        QStringLiteral("Session Downloaded"), QStringLiteral("Session Uploaded"),
        QStringLiteral("Remaining"), QStringLiteral("Time Active"), QStringLiteral("Save Path"),
        QStringLiteral("Incomplete Save Path"), QStringLiteral("Completed"),
        QStringLiteral("Ratio Limit"), QStringLiteral("Last Seen Complete"),
        QStringLiteral("Last Activity"), QStringLiteral("Total Size"),
        QStringLiteral("Info Hash v1"), QStringLiteral("Info Hash v2"),
        QStringLiteral("Reannounce In"), QStringLiteral("Private"),
        QStringLiteral("Popularity"), QStringLiteral("Queue Position")
    };
    return names;
}

const QStringList &fieldKeys()
{
    static const QStringList keys {
        QStringLiteral("name"), QStringLiteral("size"), QStringLiteral("progress"),
        QStringLiteral("state"), QStringLiteral("num_seeds"), QStringLiteral("num_leechs"),
        QStringLiteral("dlspeed"), QStringLiteral("upspeed"), QStringLiteral("eta"),
        QStringLiteral("ratio"), QStringLiteral("category"), QStringLiteral("tags"),
        QStringLiteral("availability"), QStringLiteral("added_on"),
        QStringLiteral("completion_on"), QStringLiteral("creation_date"),
        QStringLiteral("tracker"), QStringLiteral("dl_limit"), QStringLiteral("up_limit"),
        QStringLiteral("downloaded"), QStringLiteral("uploaded"),
        QStringLiteral("downloaded_session"), QStringLiteral("uploaded_session"),
        QStringLiteral("amount_left"), QStringLiteral("time_active"), QStringLiteral("save_path"),
        QStringLiteral("download_path"), QStringLiteral("completed"),
        QStringLiteral("ratio_limit"), QStringLiteral("seen_complete"),
        QStringLiteral("last_activity"), QStringLiteral("total_size"),
        QStringLiteral("infohash_v1"), QStringLiteral("infohash_v2"),
        QStringLiteral("reannounce"), QStringLiteral("private"),
        QStringLiteral("popularity"), QStringLiteral("priority")
    };
    return keys;
}

QString bytesText(double bytes, int precision = 1)
{
    return BytePresentation::format(bytes, precision, false, 5);
}

QString durationText(qint64 seconds)
{
    if (seconds < 0 || seconds >= 8640000)
        return QStringLiteral("∞");
    const qint64 days = seconds / 86400;
    const qint64 hours = (seconds % 86400) / 3600;
    const qint64 minutes = (seconds % 3600) / 60;
    const qint64 secs = seconds % 60;
    if (days)
        return QStringLiteral("%1d %2h").arg(days).arg(hours);
    if (hours)
        return QStringLiteral("%1h %2m").arg(hours).arg(minutes);
    return QStringLiteral("%1m %2s").arg(minutes).arg(secs);
}

QString stateText(const QString &state)
{
    static const QHash<QString, QString> friendly {
        {QStringLiteral("downloading"), QStringLiteral("Downloading")},
        {QStringLiteral("metaDL"), QStringLiteral("Downloading metadata")},
        {QStringLiteral("forcedMetaDL"), QStringLiteral("Forced downloading metadata")},
        {QStringLiteral("allocating"), QStringLiteral("Allocating")},
        {QStringLiteral("queuedForChecking"), QStringLiteral("Queued for checking")},
        {QStringLiteral("forcedDL"), QStringLiteral("Forced downloading")},
        {QStringLiteral("stalledDL"), QStringLiteral("Stalled downloading")},
        {QStringLiteral("uploading"), QStringLiteral("Seeding")},
        {QStringLiteral("forcedUP"), QStringLiteral("Forced seeding")},
        {QStringLiteral("stalledUP"), QStringLiteral("Seeding")},
        {QStringLiteral("pausedDL"), QStringLiteral("Paused")},
        {QStringLiteral("pausedUP"), QStringLiteral("Paused")},
        {QStringLiteral("stoppedDL"), QStringLiteral("Stopped")},
        {QStringLiteral("stoppedUP"), QStringLiteral("Stopped")},
        {QStringLiteral("queuedDL"), QStringLiteral("Queued")},
        {QStringLiteral("queuedUP"), QStringLiteral("Queued")},
        {QStringLiteral("checkingDL"), QStringLiteral("Checking")},
        {QStringLiteral("checkingUP"), QStringLiteral("Checking")},
        {QStringLiteral("checkingResumeData"), QStringLiteral("Checking")},
        {QStringLiteral("moving"), QStringLiteral("Moving")},
        {QStringLiteral("error"), QStringLiteral("Errored")},
        {QStringLiteral("missingFiles"), QStringLiteral("Missing files")}
    };
    return friendly.value(state, state);
}

QString stateIcon(const QString &state)
{
    if (state == QLatin1String("downloading") || state == QLatin1String("forcedDL")
        || state == QLatin1String("metaDL") || state == QLatin1String("forcedMetaDL"))
        return QStringLiteral("downloading");
    if (state == QLatin1String("uploading") || state == QLatin1String("forcedUP"))
        return QStringLiteral("upload");
    if (state.startsWith(QLatin1String("stalled")) || state == QLatin1String("allocating"))
        return QStringLiteral("filter-stalled");
    if (state.startsWith(QLatin1String("paused")) || state.startsWith(QLatin1String("stopped")))
        return QStringLiteral("stopped");
    if (state == QLatin1String("error") || state == QLatin1String("missingFiles"))
        return QStringLiteral("error");
    return QStringLiteral("filter-all");
}

bool byteColumn(int col)
{
    return col == 1 || col == 19 || col == 20 || col == 21 || col == 22
        || col == 23 || col == 27 || col == 31;
}
bool speedColumn(int col) { return col == 6 || col == 7 || col == 17 || col == 18; }
bool timeColumn(int col) { return col == 13 || col == 14 || col == 15 || col == 29 || col == 30; }
bool durationColumn(int col) { return col == 8 || col == 24 || col == 34; }
}

TorrentModel::TorrentModel(QObject *parent) : QAbstractTableModel(parent) {}

int TorrentModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_hashes.size();
}

int TorrentModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : columnNames().size();
}

QVariant TorrentModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal
        || section < 0 || section >= columnNames().size())
        return {};
    return columnNames().at(section);
}

QVariant TorrentModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_hashes.size()
        || index.column() < 0 || index.column() >= fieldKeys().size())
        return {};

    const QJsonObject record = m_torrents.value(m_hashes.at(index.row()));
    const int col = index.column();
    // Canonical upstream serializer keys take precedence, including explicit
    // null privacy. Retain compatibility with the older client aliases.
    QJsonValue value = record.value(fieldKeys().at(col));
    if (value.isUndefined() && col == 15)
        value = record.value(QStringLiteral("created_on"));
    if (value.isUndefined() && col == 35)
        value = record.value(QStringLiteral("is_private"));
    // The configured ratio_limit can be -2 (inherit) or a dormant custom
    // value. The native column displays the effective limit, serialized as
    // max_ratio since 4.1.0. Preserve the raw record for editing actions.
    if (col == 28)
        value = record.value(QStringLiteral("max_ratio"));

    if (role == Qt::DecorationRole && col == 0)
        return QIcon(QStringLiteral(":/icons/") + stateIcon(record.value(QStringLiteral("state")).toString())
                     + QStringLiteral(".svg"));
    if (role == Qt::TextAlignmentRole && col != 0 && col != 3 && col != 10 && col != 11
        && col != 16 && col != 25 && col != 26 && col != 32 && col != 33)
        return static_cast<int>(Qt::AlignVCenter | Qt::AlignRight);
    if (role == Qt::UserRole)
        return value.isUndefined() ? QVariant() : value.toVariant();
    if (role == StateColorRole || (role == Qt::ForegroundRole && m_useStateColors))
    {
        const QString state = record.value(QStringLiteral("state")).toString();
        const bool dark = QGuiApplication::palette().color(QPalette::Base).lightness() < 128;

        // Match qBittorrent 5.3.0's Default UI Theme (uiThemeCommon.h,
        // Color::Primer in color.h), not the desktop's ordinary Text color.
        // In particular, stalled seeding must stay BLUE, not become black
        // on light themes; stalled downloads remain GREEN.
        // Remote-client divergence: surface the same server-reported errors
        // as the Tracker error and Other error sidebar filters. Do not infer
        // errors from missing tracker data, warnings, or an idle torrent.
        if (state == QLatin1String("error") || state == QLatin1String("missingFiles")
            || record.value(QStringLiteral("has_tracker_error")).toBool()
            || record.value(QStringLiteral("has_other_announce_error")).toBool())
            return QColor(dark ? QStringLiteral("#f85149") : QStringLiteral("#cf222e"));
        if (state == QLatin1String("downloading") || state == QLatin1String("forcedDL")
            || state == QLatin1String("metaDL") || state == QLatin1String("forcedMetaDL")
            || state == QLatin1String("checkingDL") || state == QLatin1String("checkingUP")
            || state == QLatin1String("checkingResumeData") || state == QLatin1String("moving"))
            return QColor(dark ? QStringLiteral("#3fb950") : QStringLiteral("#1a7f37"));
        if (state == QLatin1String("stalledDL"))
            return QColor(dark ? QStringLiteral("#238636") : QStringLiteral("#2da44e"));
        if (state == QLatin1String("uploading") || state == QLatin1String("forcedUP"))
            return QColor(dark ? QStringLiteral("#58a6ff") : QStringLiteral("#0969da"));
        if (state == QLatin1String("stalledUP"))
            return QColor(dark ? QStringLiteral("#1f6feb") : QStringLiteral("#0969da"));
        if (state == QLatin1String("queuedDL") || state == QLatin1String("queuedUP"))
            return QColor(dark ? QStringLiteral("#845306") : QStringLiteral("#7d4e00"));
        if (state == QLatin1String("stoppedDL") || state == QLatin1String("pausedDL"))
            return QColor(dark ? QStringLiteral("#8b949e") : QStringLiteral("#57606a"));
        if (state == QLatin1String("stoppedUP") || state == QLatin1String("pausedUP"))
            return QColor(dark ? QStringLiteral("#a371f7") : QStringLiteral("#8250df"));
        // Unknown states should use the native desktop palette.
    }

    if (role != Qt::DisplayRole && role != Qt::ToolTipRole)
        return {};

    // As in upstream TransferListModel::displayValue(TR_SEEDS / TR_PEERS):
    // first show connected peers (num_seeds / num_leechs), then the total
    // swarm reported by the server (num_complete / num_incomplete).
    // UserRole above remains the numeric connected count for proper sorting.
    if (col == 4 || col == 5)
    {
        const QJsonValue total = record.value(col == 4
            ? QStringLiteral("num_complete") : QStringLiteral("num_incomplete"));
        if (!value.isDouble() || !total.isDouble())
            return QStringLiteral("—"); // Never pretend an unknown total is zero.
        const qint64 connected = static_cast<qint64>(value.toDouble());
        const qint64 totalCount = static_cast<qint64>(total.toDouble());
        const QString state = record.value(QStringLiteral("state")).toString();
        const bool hideZeros = m_hideZeroValues && (m_hideZeroMode == 0
            || state.startsWith(QLatin1String("stopped")) || state.startsWith(QLatin1String("paused")));
        if (role == Qt::DisplayRole && hideZeros && connected == 0 && totalCount == 0)
            return QString();
        return QStringLiteral("%1 (%2)").arg(QString::number(connected),
                                              QString::number(totalCount));
    }
    if (value.isUndefined() || value.isNull())
        return QStringLiteral("—");
    if (col == 28 && (!value.isDouble() || value.toDouble() < -1))
        return QStringLiteral("—");
    // Native "Hide zero and infinity values" changes presentation only.
    // UserRole retains the complete server value for sorting/actions.
    if (role == Qt::DisplayRole && m_hideZeroValues && value.isDouble())
    {
        const QString state = record.value(QStringLiteral("state")).toString();
        const bool applies = m_hideZeroMode == 0
            || state.startsWith(QLatin1String("stopped")) || state.startsWith(QLatin1String("paused"));
        const double number = value.toDouble();
        const bool numericColumn = col == 4 || col == 5 || col == 6
            || col == 7 || col == 8 || col == 9 || col == 12
            || col == 17 || col == 18 || col == 19 || col == 20
            || col == 21 || col == 22 || col == 23 || col == 24
            || col == 28 || col == 34 || col == 36;
        const bool infinite = (number < 0
            && (col == 8 || col == 9 || col == 28 || col == 34))
            || (col == 8 && number >= 8640000)
            || ((col == 17 || col == 18) && number <= 0);
        if (applies && numericColumn && (number == 0 || infinite))
            return QString();
    }
    if (col == 3)
        return stateText(value.toString());
    if (value.isBool())
        return value.toBool() ? QStringLiteral("Yes") : QStringLiteral("No");
    if (value.isString())
    {
        if (col == 16)
        {
            const QString url = value.toString();
            const QUrl parsed(url);
            // Do not expose passkeys in hostname-mode tooltips. Keep the
            // original URL in UserRole for existing sorting and operations.
            if (!m_fullTrackerUrl)
                return parsed.host().isEmpty() ? (url.isEmpty() ? QString() : QStringLiteral("—")) : parsed.host();
            return url;
        }
        return value.toString();
    }
    if (!value.isDouble())
        return QStringLiteral("—");

    const double number = value.toDouble();
    if (col == 2)
        return ProgressDisplay::percent(number);
    // As in native qBittorrent: the Web API's "availability" is the
    // number of distributed copies, not a percentage. -1 means unknown.
    if (col == 12)
        return number < 0 ? QStringLiteral("N/A")
            : QLocale().toString(number, 'f', 3);
    if (byteColumn(col))
        return bytesText(number, col == 1 ? 2 : 1);
    if (speedColumn(col))
    {
        // Per-torrent limit getters expose nonpositive values for unlimited.
        if ((col == 17 || col == 18) && number <= 0)
            return QStringLiteral("∞");
        return number < 0 ? QStringLiteral("—") : bytesText(number) + QStringLiteral("/s");
    }
    if (durationColumn(col))
        return durationText(static_cast<qint64>(number));
    if (timeColumn(col))
        return DatePresentation::format(static_cast<qint64>(number), m_dateFormat);
    if (col == 9 || col == 28 || col == 36)
        return number == -1 ? QStringLiteral("∞") : (number < 0
            ? QStringLiteral("—") : QLocale().toString(number, 'f', 2));
    // Both the historical helper and the modern serializer already convert
    // the internal zero-based queue to one-based positions; zero is unqueued.
    if (col == 37)
        return number == 0 ? QStringLiteral("*") : (number < 0
            ? QStringLiteral("—") : QLocale().toString(static_cast<qint64>(number)));
    return QLocale().toString(static_cast<qint64>(number));
}

void TorrentModel::applySync(const QJsonObject &payload)
{
    const QJsonObject patches = payload.value(QStringLiteral("torrents")).toObject();

    if (payload.value(QStringLiteral("full_update")).toBool())
    {
        beginResetModel();
        m_hashes.clear();
        m_torrents.clear();
        m_hasFullSync = true;
        for (auto it = patches.constBegin(); it != patches.constEnd(); ++it)
        {
            if (!it.value().isObject())
            {
                m_hasFullSync = false;
                continue;
            }
            m_hashes.append(it.key());
            m_torrents.insert(it.key(), it.value().toObject());
        }
        endResetModel();
        return;
    }

    const QJsonArray removed = payload.value(QStringLiteral("torrents_removed")).toArray();
    for (const QJsonValue &value : removed)
    {
        const QString hash = value.toString();
        const int row = m_hashes.indexOf(hash);
        if (row < 0)
            continue;
        beginRemoveRows({}, row, row);
        m_hashes.removeAt(row);
        m_torrents.remove(hash);
        endRemoveRows();
    }

    for (auto it = patches.constBegin(); it != patches.constEnd(); ++it)
    {
        if (!it.value().isObject())
        {
            m_hasFullSync = false;
            continue;
        }
        const QString hash = it.key();
        const int row = m_hashes.indexOf(hash);
        QJsonObject record = row < 0 ? QJsonObject() : m_torrents.value(hash);
        const QJsonObject changes = it.value().toObject();
        for (auto change = changes.constBegin(); change != changes.constEnd(); ++change)
            record.insert(change.key(), change.value());

        m_torrents.insert(hash, record);
        if (row < 0)
        {
            const int next = m_hashes.size();
            beginInsertRows({}, next, next);
            m_hashes.append(hash);
            endInsertRows();
        }
        else
        {
            emit dataChanged(index(row, 0), index(row, columnCount() - 1),
                             {Qt::DisplayRole, Qt::UserRole, Qt::DecorationRole,
                               Qt::ForegroundRole, StateColorRole});
        }
    }
}

void TorrentModel::setHideZeroValues(bool enabled, int mode)
{
    mode = qBound(0, mode, 1);
    if (m_hideZeroValues == enabled && m_hideZeroMode == mode)
        return;
    m_hideZeroValues = enabled;
    m_hideZeroMode = mode;
    if (!m_hashes.isEmpty())
        emit dataChanged(index(0, 0), index(m_hashes.size() - 1, columnCount() - 1),
                         {Qt::DisplayRole});
}

void TorrentModel::setUseStateColors(bool enabled)
{
    if (m_useStateColors == enabled)
        return;
    m_useStateColors = enabled;
    if (!m_hashes.isEmpty())
        emit dataChanged(index(0, 0), index(m_hashes.size() - 1, columnCount() - 1),
                         {Qt::ForegroundRole});
}

void TorrentModel::setPresentation(bool fullTrackerUrl, const QString &dateFormat)
{
    if (m_fullTrackerUrl == fullTrackerUrl && m_dateFormat == dateFormat) return;
    m_fullTrackerUrl = fullTrackerUrl;
    m_dateFormat = dateFormat;
    if (!m_hashes.isEmpty())
        emit dataChanged(index(0, 0), index(m_hashes.size() - 1, columnCount() - 1),
            {Qt::DisplayRole, Qt::ToolTipRole});
}

void TorrentModel::refreshBytePresentation()
{
    if (!m_hashes.isEmpty())
        emit dataChanged(index(0, 0), index(m_hashes.size() - 1, columnCount() - 1),
            {Qt::DisplayRole, Qt::ToolTipRole});
}

void TorrentModel::clear()
{
    beginResetModel();
    m_hasFullSync = false;
    m_hashes.clear();
    m_torrents.clear();
    endResetModel();
}

QString TorrentModel::hashForRow(int row) const
{
    return row >= 0 && row < m_hashes.size() ? m_hashes.at(row) : QString();
}

const QHash<QString, QJsonObject> &TorrentModel::torrents() const noexcept
{
    return m_torrents;
}
