// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/FilterSidebar.h"
#include "gui/upstream/transferlistfilterswidgetitem.h"

#include <QFrame>
#include <QIcon>
#include <QJsonArray>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPalette>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

namespace
{
struct Entry
{
    QString key;
    QString label;
    const char *icon;
    int count = 0;
};

QString choice(const QListWidget *list)
{
    return (list && list->currentItem())
        ? list->currentItem()->data(Qt::UserRole).toString()
        : QStringLiteral("all");
}

QString trackerHost(const QString &tracker)
{
    const QString hostname = QUrl(tracker).host().toLower();
    // A private tracker passkey can live in the URL path or query. Never
    // put a full announce URL into the sidebar's visible labels.
    return hostname.isEmpty() ? QStringLiteral("(unknown tracker)") : hostname;
}

QStringList torrentTags(const QJsonObject &torrent)
{
    QStringList result;
    for (const QString &raw : torrent.value(QStringLiteral("tags")).toString()
                                  .split(QLatin1Char(','), Qt::SkipEmptyParts))
    {
        const QString tag = raw.trimmed();
        if (!tag.isEmpty())
            result.append(tag);
    }
    return result;
}

bool statusMatches(const QString &key, const QJsonObject &torrent)
{
    if (key == QLatin1String("all"))
        return true;
    const QString state = torrent.value(QStringLiteral("state")).toString();
    const bool finished = torrent.value(QStringLiteral("progress")).toDouble() >= 1.0;
    const bool downloading = state == QLatin1String("downloading")
        || state == QLatin1String("forcedDL") || state == QLatin1String("metaDL")
        || state == QLatin1String("stalledDL");
    const bool seeding = state == QLatin1String("uploading")
        || state == QLatin1String("forcedUP") || state == QLatin1String("stalledUP");
    const bool stopped = state.startsWith(QLatin1String("stopped"))
        || state.startsWith(QLatin1String("paused"));
    const bool active = torrent.value(QStringLiteral("dlspeed")).toDouble() > 0
        || torrent.value(QStringLiteral("upspeed")).toDouble() > 0;
    if (key == QLatin1String("downloading")) return downloading;
    if (key == QLatin1String("seeding")) return seeding;
    if (key == QLatin1String("completed")) return finished;
    if (key == QLatin1String("running")) return !stopped && (downloading || seeding);
    if (key == QLatin1String("stopped")) return stopped;
    if (key == QLatin1String("active")) return active;
    if (key == QLatin1String("inactive")) return !active;
    if (key == QLatin1String("stalled")) return state.startsWith(QLatin1String("stalled"));
    if (key == QLatin1String("stalledUP")) return state == QLatin1String("stalledUP");
    if (key == QLatin1String("stalledDL")) return state == QLatin1String("stalledDL");
    if (key == QLatin1String("checking")) return state.startsWith(QLatin1String("checking"));
    if (key == QLatin1String("moving")) return state == QLatin1String("moving");
    if (key == QLatin1String("errored"))
        return state == QLatin1String("error") || state == QLatin1String("missingFiles");
    return false;
}

void fitList(QListWidget *list)
{
#ifdef Q_OS_WIN
    const int rowHeight = qMax(list->fontMetrics().height(), list->iconSize().height()) + 4;
#else
    const int rowHeight = qMax(list->fontMetrics().height() + 4,
                              list->sizeHintForRow(0));
#endif
    list->setFixedHeight(qMax(0, rowHeight) * list->count()
                         + 2 * list->frameWidth());
}

QListWidget *makeList(QWidget *parent, const QString &name)
{
    auto *list = new QListWidget(parent);
    list->setObjectName(name);
    list->setFrameShape(QFrame::NoFrame);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    list->setUniformItemSizes(true);
    list->setSpacing(0);
    list->setIconSize(QSize(16, 16));
    return list;
}

bool updateList(QListWidget *list, const QList<Entry> &entries)
{
    const QString before = choice(list);
    bool sameKeys = list->count() == entries.size();
    if (sameKeys)
        for (int row = 0; row < entries.size(); ++row)
            if (list->item(row)->data(Qt::UserRole).toString() != entries[row].key)
            {
                sameKeys = false;
                break;
            }

    if (!sameKeys)
    {
        const QSignalBlocker blocker(list);
        list->clear();
        int selected = 0;
        for (int row = 0; row < entries.size(); ++row)
        {
            const Entry &entry = entries[row];
            auto *item = new QListWidgetItem(
                QIcon(QStringLiteral(":/icons/") + QString::fromLatin1(entry.icon)
                      + QStringLiteral(".svg")), QString(), list);
            item->setData(Qt::UserRole, entry.key);
#ifdef Q_OS_WIN
            item->setSizeHint(QSize(0, qMax(list->fontMetrics().height(),
                                            list->iconSize().height()) + 4));
#endif
            if (entry.key == before)
                selected = row;
        }
        if (!entries.isEmpty())
            list->setCurrentRow(selected);
        fitList(list);
    }

    for (int row = 0; row < entries.size(); ++row)
    {
        const Entry &entry = entries[row];
        const QString label = QStringLiteral("%1 (%2)").arg(entry.label).arg(entry.count);
        if (list->item(row)->text() != label)
            list->item(row)->setText(label);
    }
    return before != choice(list);
}
} // namespace

FilterSidebar::FilterSidebar(QWidget *parent)
    : QScrollArea(parent)
{
    setObjectName(QStringLiteral("filterSidebar"));
    setWidgetResizable(true);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setBackgroundRole(QPalette::Base);

    auto *container = new QWidget(this);
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 2, 0, 0);
    layout->setSpacing(2);
    layout->setAlignment(Qt::AlignTop);

    const auto section = [this, container, layout](const QString &title,
                                                   const QString &objectName) {
        auto *list = makeList(container, objectName);
        auto *wrapper = new TransferListFiltersWidgetItem(title, list, container);
        wrapper->setChecked(true);
        layout->addWidget(wrapper);
        connect(list, &QListWidget::currentItemChanged, this,
                [this](QListWidgetItem *, QListWidgetItem *) { emit filtersChanged(); });
        return list;
    };
    m_status = section(tr("Status"), QStringLiteral("filterList"));
    m_categories = section(tr("Categories"), QStringLiteral("categoryFilterList"));
    m_tags = section(tr("Tags"), QStringLiteral("tagFilterList"));
    m_trackers = section(tr("Trackers"), QStringLiteral("trackerFilterList"));

    layout->addStretch();
    setWidget(container);
    updateCounts({});
}

void FilterSidebar::refreshFontMetrics()
{
    for (auto *list : {m_status, m_categories, m_tags, m_trackers})
    {
        const int rowHeight = qMax(list->fontMetrics().height() + 4,
                                   list->iconSize().height() + 4);
        list->doItemsLayout();
        list->setFixedHeight(rowHeight * list->count() + 2 * list->frameWidth());
    }
}

void FilterSidebar::updateCounts(const QHash<QString, QJsonObject> &torrents,
                                 const QSet<QString> &categories,
                                 const QSet<QString> &tags,
                                 const QHash<QString, QSet<QString>> &trackers)
{
    static const struct { const char *key; const char *label; const char *icon; } statuses[] = {
        {"all", "All", "filter-all"}, {"downloading", "Downloading", "downloading"},
        {"seeding", "Seeding", "upload"}, {"completed", "Completed", "checked-completed"},
        {"running", "Running", "torrent-start"}, {"stopped", "Stopped", "stopped"},
        {"active", "Active", "filter-active"}, {"inactive", "Inactive", "filter-inactive"},
        {"stalled", "Stalled", "filter-stalled"}, {"stalledUP", "Stalled Uploading", "stalledUP"},
        {"stalledDL", "Stalled Downloading", "stalledDL"}, {"checking", "Checking", "force-recheck"},
        {"moving", "Moving", "set-location"}, {"errored", "Errored", "error"}
    };

    QList<Entry> statusRows;
    for (const auto &status : statuses)
    {
        const QString key = QString::fromLatin1(status.key);
        int count = 0;
        for (const QJsonObject &torrent : torrents)
            count += statusMatches(key, torrent);
        statusRows.append({key, tr(status.label), status.icon, count});
    }

    QSet<QString> categoryNames = categories;
    QSet<QString> tagNames = tags;
    m_trackerHosts.clear();
    m_torrentsWithTrackers.clear();

    // /sync/maindata supplies tracker URLs with the *set* of torrent
    // hashes for each. One torrent can belong to several tracker hosts.
    for (auto it = trackers.cbegin(); it != trackers.cend(); ++it)
    {
        const QString host = trackerHost(it.key());
        for (const QString &hash : it.value())
        {
            if (!torrents.contains(hash))
                continue;
            m_trackerHosts[host].insert(hash);
            m_torrentsWithTrackers.insert(hash);
        }
    }
    for (auto it = torrents.cbegin(); it != torrents.cend(); ++it)
    {
        const QJsonObject &torrent = it.value();
        const QString category = torrent.value(QStringLiteral("category")).toString().trimmed();
        if (!category.isEmpty())
            categoryNames.insert(category);
        for (const QString &tag : torrentTags(torrent))
            tagNames.insert(tag);
        // Older Web API implementations might not send the tracker map.
        // The torrent's primary tracker still provides a useful fallback.
        const QString primary = torrent.value(QStringLiteral("tracker")).toString();
        if (!primary.isEmpty())
        {
            m_trackerHosts[trackerHost(primary)].insert(it.key());
            m_torrentsWithTrackers.insert(it.key());
        }
    }

    int uncategorized = 0;
    int untagged = 0;
    int trackerless = 0;
    int trackerError = 0;
    int otherError = 0;
    int warning = 0;
    for (auto it = torrents.cbegin(); it != torrents.cend(); ++it)
    {
        const QJsonObject &torrent = it.value();
        uncategorized += torrent.value(QStringLiteral("category")).toString().trimmed().isEmpty();
        untagged += torrentTags(torrent).isEmpty();
        trackerless += !m_torrentsWithTrackers.contains(it.key());
        trackerError += torrent.value(QStringLiteral("has_tracker_error")).toBool();
        otherError += torrent.value(QStringLiteral("has_other_announce_error")).toBool();
        warning += torrent.value(QStringLiteral("has_tracker_warning")).toBool();
    }

    QList<Entry> categoryRows {
        {QStringLiteral("all"), tr("All"), "view-categories", static_cast<int>(torrents.size())},
        {QStringLiteral("uncategorized"), tr("Uncategorized"), "view-categories", uncategorized}
    };
    QStringList sortedCategories(categoryNames.cbegin(), categoryNames.cend());
    sortedCategories.sort(Qt::CaseInsensitive);
    for (const QString &name : sortedCategories)
    {
        int count = 0;
        for (const QJsonObject &torrent : torrents)
            count += torrent.value(QStringLiteral("category")).toString() == name;
        categoryRows.append({QStringLiteral("category:") + name,
                             name, "view-categories", count});
    }

    QList<Entry> tagRows {
        {QStringLiteral("all"), tr("All"), "tags", static_cast<int>(torrents.size())},
        {QStringLiteral("untagged"), tr("Untagged"), "tags", untagged}
    };
    QStringList sortedTags(tagNames.cbegin(), tagNames.cend());
    sortedTags.sort(Qt::CaseInsensitive);
    for (const QString &name : sortedTags)
    {
        int count = 0;
        for (const QJsonObject &torrent : torrents)
            count += torrentTags(torrent).contains(name);
        tagRows.append({QStringLiteral("tag:") + name, name, "tags", count});
    }

    QList<Entry> trackerRows {
        {QStringLiteral("all"), tr("All"), "trackers", static_cast<int>(torrents.size())},
        {QStringLiteral("trackerless"), tr("Trackerless"), "trackerless", trackerless},
        {QStringLiteral("tracker-error"), tr("Tracker error"), "tracker-error", trackerError},
        {QStringLiteral("other-error"), tr("Other error"), "tracker-error", otherError},
        {QStringLiteral("warning"), tr("Warning"), "tracker-warning", warning}
    };
    QStringList sortedHosts = m_trackerHosts.keys();
    sortedHosts.sort(Qt::CaseInsensitive);
    for (const QString &host : sortedHosts)
        trackerRows.append({QStringLiteral("tracker:") + host,
                            host, "trackers", m_trackerHosts.value(host).size()});

    bool lostSelection = false;
    lostSelection |= updateList(m_status, statusRows);
    lostSelection |= updateList(m_categories, categoryRows);
    lostSelection |= updateList(m_tags, tagRows);
    lostSelection |= updateList(m_trackers, trackerRows);
    if (lostSelection)
        emit filtersChanged();
}

bool FilterSidebar::matches(const QString &hash, const QJsonObject &torrent) const
{
    if (!statusMatches(choice(m_status), torrent))
        return false;

    const QString category = choice(m_categories);
    const QString currentCategory = torrent.value(QStringLiteral("category")).toString().trimmed();
    if (category == QLatin1String("uncategorized"))
    {
        if (!currentCategory.isEmpty()) return false;
    }
    else if (category.startsWith(QLatin1String("category:")))
    {
        if (currentCategory != category.mid(9)) return false;
    }

    const QString tag = choice(m_tags);
    const QStringList currentTags = torrentTags(torrent);
    if (tag == QLatin1String("untagged"))
    {
        if (!currentTags.isEmpty()) return false;
    }
    else if (tag.startsWith(QLatin1String("tag:")))
    {
        if (!currentTags.contains(tag.mid(4))) return false;
    }

    const QString tracker = choice(m_trackers);
    if (tracker == QLatin1String("trackerless"))
        return !m_torrentsWithTrackers.contains(hash);
    if (tracker == QLatin1String("tracker-error"))
        return torrent.value(QStringLiteral("has_tracker_error")).toBool();
    if (tracker == QLatin1String("other-error"))
        return torrent.value(QStringLiteral("has_other_announce_error")).toBool();
    if (tracker == QLatin1String("warning"))
        return torrent.value(QStringLiteral("has_tracker_warning")).toBool();
    if (tracker.startsWith(QLatin1String("tracker:")))
        return m_trackerHosts.value(tracker.mid(8)).contains(hash);
    return true;
}
