// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/FilterSidebar.h"
#include "gui/BytePresentation.h"
#include "gui/upstream/transferlistfilterswidgetitem.h"

#include <QCheckBox>
#include <QFrame>
#include <QHeaderView>
#include <QIcon>
#include <QJsonArray>
#include <QListWidget>
#include <QListWidgetItem>
#include <QLayout>
#include <QLocale>
#include <QMenu>
#include <QPalette>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <utility>

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


constexpr char unknownLocation[] = "<unknown-save-path>";

// Parse only the textual remote save_path. No QDir::canonicalPath, symlink
// lookup, mount discovery, or merging of apparent QNAP aliases.
QStringList directoryHierarchy(const QString &path)
{
    QStringList dirs;
    QChar separator;
    QString prefix;
    if (path.startsWith(QLatin1Char('/')))
        separator = QLatin1Char('/');
    else if (path.size() >= 3 && path.at(1) == QLatin1Char(':')
             && path.at(2) == QLatin1Char('\\'))
        separator = QLatin1Char('\\');
    else
        return dirs;

    for (const QString &part : path.split(separator, Qt::SkipEmptyParts))
    {
        if (separator == QLatin1Char('/') || !prefix.isEmpty())
            prefix += separator;
        prefix += part;
        dirs.append(prefix);
    }
    if (dirs.isEmpty() && separator == QLatin1Char('/'))
        dirs.append(QStringLiteral("/"));
    return dirs;
}

QString torrentDirectory(const QJsonObject &torrent)
{
    const QStringList dirs = directoryHierarchy(
        torrent.value(QStringLiteral("save_path")).toString());
    return dirs.isEmpty() ? QString::fromLatin1(unknownLocation) : dirs.constLast();
}

QString parentDirectory(const QString &path)
{
    if (path == QLatin1String("/") || path == QLatin1String(unknownLocation))
        return {};
    const QChar sep = path.startsWith(QLatin1Char('/'))
        ? QLatin1Char('/') : QLatin1Char('\\');
    const int index = path.lastIndexOf(sep);
    return index <= 0 ? QString() : path.left(index);
}

QString directoryName(const QString &path)
{
    if (path == QLatin1String(unknownLocation))
        return QObject::tr("No save path reported");
    if (path == QLatin1String("/"))
        return path;
    const QChar sep = path.startsWith(QLatin1Char('/'))
        ? QLatin1Char('/') : QLatin1Char('\\');
    return path.mid(path.lastIndexOf(sep) + 1);
}

bool statusMatches(const QString &key, const QJsonObject &torrent)
{
    if (key == QLatin1String("all"))
        return true;
    const QString state = torrent.value(QStringLiteral("state")).toString();
    const bool finished = torrent.value(QStringLiteral("progress")).toDouble() >= 1.0;
    // Native TorrentImpl::isDownloading/isUploading include queued and
    // checking torrents; downloading also includes stopped downloads.
    // These are overlapping status filters, not transfer-rate categories.
    const bool downloading = state == QLatin1String("downloading")
        || state == QLatin1String("forcedDL") || state == QLatin1String("metaDL")
        || state == QLatin1String("forcedMetaDL") || state == QLatin1String("stalledDL")
        || state == QLatin1String("checkingDL") || state == QLatin1String("queuedDL")
        || state == QLatin1String("stoppedDL") || state == QLatin1String("pausedDL")
        || state == QLatin1String("allocating");
    const bool seeding = state == QLatin1String("uploading")
        || state == QLatin1String("forcedUP") || state == QLatin1String("stalledUP")
        || state == QLatin1String("queuedUP") || state == QLatin1String("checkingUP");
    const bool stopped = state.startsWith(QLatin1String("stopped"))
        || state.startsWith(QLatin1String("paused"));
    const bool active = torrent.value(QStringLiteral("dlspeed")).toDouble() > 0
        || torrent.value(QStringLiteral("upspeed")).toDouble() > 0;
    if (key == QLatin1String("downloading")) return downloading;
    if (key == QLatin1String("seeding")) return seeding;
    if (key == QLatin1String("completed")) return finished;
    // Native isRunning() is !isStopped(). The Web API exposes a state
    // rather than the underlying stopped flag for moving/error/checking
    // states; follow the official WebUI's non-stopped-state interpretation
    // while not treating a missing or unknown state as running.
    if (key == QLatin1String("running"))
        return !stopped && !state.isEmpty() && state != QLatin1String("unknown");
    if (key == QLatin1String("stopped")) return stopped;
    if (key == QLatin1String("active")) return active;
    if (key == QLatin1String("inactive")) return !active;
    if (key == QLatin1String("stalled")) return state.startsWith(QLatin1String("stalled"));
    if (key == QLatin1String("stalledUP")) return state == QLatin1String("stalledUP");
    if (key == QLatin1String("stalledDL")) return state == QLatin1String("stalledDL");
    if (key == QLatin1String("checking")) return state.startsWith(QLatin1String("checking"))
        || state == QLatin1String("queuedForChecking");
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
    Q_INIT_RESOURCE(qbittorrent_icons);
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
    m_trackerStatuses = section(tr("Tracker status"),
                                QStringLiteral("trackerStatusFilterList"));
    const auto groupMenu = [this](QListWidget *list, Group group) {
        list->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(list, &QListWidget::customContextMenuRequested, this,
            [this, list, group](const QPoint &position) { showGroupOptions(list, group, position); });
    };
    groupMenu(m_status, Group::Status);
    groupMenu(m_categories, Group::Categories);
    groupMenu(m_tags, Group::Tags);
    groupMenu(m_trackers, Group::Trackers);
    groupMenu(m_trackerStatuses, Group::TrackerStatus);
    m_trackerStatusSection = qobject_cast<TransferListFiltersWidgetItem *>(
        m_trackerStatuses->parentWidget());
    if (m_trackerStatusSection)
        m_trackerStatusSection->hide();

    // The section caption and collapse arrow are the exact same upstream
    // widget used for Status/Categories/Tags/Trackers. A native QTreeWidget
    // supplies the hierarchy, without a separate nested scrollbar.
    m_directories = new QTreeWidget(container);
    m_directories->setObjectName(QStringLiteral("directoryFilterTree"));
    m_directories->setColumnCount(2);
    m_directories->setHeaderHidden(true);
    m_directories->setFrameShape(QFrame::NoFrame);
    m_directories->setRootIsDecorated(true);
    m_directories->setIndentation(15);
    m_directories->setUniformRowHeights(true);
    m_directories->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_directories->setSelectionMode(QAbstractItemView::SingleSelection);
    m_directories->setIconSize(QSize(16, 16));
    m_directories->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_directories->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // A single outer *vertical* scrollbar is retained. Horizontal scrolling
    // of the tree itself is allowed, so deeply nested/long names and the
    // right-aligned size column are never permanently cut off.
    m_directories->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_directories->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_directories->setTextElideMode(Qt::ElideNone);
    m_directories->header()->setStretchLastSection(false);
    m_directories->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_directories->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_directorySection = new TransferListFiltersWidgetItem(
        tr("Directories"), m_directories, container);
    m_directorySection->setObjectName(QStringLiteral("directoryFilterSection"));
    m_directorySection->setChecked(true);
    // The upstream section wrapper aligns its contents left. Override only
    // this added tree's alignment so it uses the full available sidebar width.
    m_directorySection->layout()->setAlignment(m_directories, Qt::AlignTop);
    m_directorySection->setToolTip(tr(
        "Select All to show every torrent. Directory counts and filtering "
        "match only their exact save paths unless Include subfolders is enabled "
        "in the right-click menu."));
    layout->addWidget(m_directorySection);

    connect(m_directories, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem *item) {
        const QString path = item ? item->data(0, Qt::UserRole).toString() : QString();
        if (m_selectedDirectory != path)
        {
            m_selectedDirectory = path;
            emit filtersChanged();
        }
    });
    connect(m_directories, &QTreeWidget::itemExpanded, this,
            [this](QTreeWidgetItem *item) {
        const QString path = item->data(0, Qt::UserRole).toString();
        if (!path.isEmpty())
            m_expandedDirectories.insert(path);
        resizeDirectoryTree();
    });
    connect(m_directories, &QTreeWidget::itemCollapsed, this,
            [this](QTreeWidgetItem *item) {
        m_expandedDirectories.remove(item->data(0, Qt::UserRole).toString());
        resizeDirectoryTree();
    });

    // Right-click the section caption or any directory row to toggle
    // "Include subfolders". Direct-only counts/filtering is the default.
    const auto enableMenu = [this](QWidget *target) {
        target->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(target, &QWidget::customContextMenuRequested, this,
                [this, target](const QPoint &p) { showDirectoryOptions(p, target); });
    };
    if (auto *caption = m_directorySection->findChild<QCheckBox *>())
        enableMenu(caption);
    // QAbstractScrollArea handles context menus through its own viewport
    // event machinery. The view itself must own the custom context menu,
    // not its viewport; connect it only once.
    enableMenu(m_directories);

    layout->addStretch();
    setWidget(container);
    updateCounts({});
}

void FilterSidebar::setSeparateTrackerStatusFilter(bool separate)
{
    if (m_separateTrackerStatus == separate)
        return;
    m_separateTrackerStatus = separate;
    if (m_trackerStatusSection)
        m_trackerStatusSection->setVisible(separate);
}

void FilterSidebar::setHideZeroStatusFilters(bool hide)
{
    if (m_hideZeroStatusFilters == hide)
        return;
    m_hideZeroStatusFilters = hide;
    applyStatusVisibility();
}

void FilterSidebar::applyStatusVisibility()
{
    if (!m_status)
        return;
    bool resetSelection = false;
    int visible = 0;
    for (int i = 0; i < m_status->count(); ++i)
    {
        QListWidgetItem *item = m_status->item(i);
        const QString key = item->data(Qt::UserRole).toString();
        const bool hidden = m_hideZeroStatusFilters && key != QLatin1String("all")
            && m_statusCounts.value(key) == 0;
        resetSelection |= hidden && m_status->currentItem() == item;
        item->setHidden(hidden);
        visible += !hidden;
    }
    if (resetSelection)
        m_status->setCurrentRow(0);
    const int rowHeight = qMax(m_status->fontMetrics().height() + 4,
                               m_status->iconSize().height() + 4);
    m_status->setFixedHeight(rowHeight * visible + 2 * m_status->frameWidth());
}

void FilterSidebar::refreshFontMetrics()
{
    for (auto *list : {m_status, m_categories, m_tags, m_trackers,
                       m_trackerStatuses})
    {
        const int rowHeight = qMax(list->fontMetrics().height() + 4,
                                   list->iconSize().height() + 4);
        list->doItemsLayout();
        int visible = 0;
        for (int row = 0; row < list->count(); ++row)
            visible += !list->item(row)->isHidden();
        list->setFixedHeight(rowHeight * visible + 2 * list->frameWidth());
    }
    resizeDirectoryTree();
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

    m_statusCounts.clear();
    for (const Entry &entry : std::as_const(statusRows))
        m_statusCounts.insert(entry.key, entry.count);

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
        {QStringLiteral("all"), tr("All"), "trackers", static_cast<int>(torrents.size())}
    };
    const QList<Entry> trackerStatusRows {
        {QStringLiteral("all"), tr("All"), "trackers", static_cast<int>(torrents.size())},
        {QStringLiteral("trackerless"), tr("Trackerless"), "trackerless", trackerless},
        {QStringLiteral("tracker-error"), tr("Tracker error"), "tracker-error", trackerError},
        {QStringLiteral("other-error"), tr("Other error"), "tracker-error", otherError},
        {QStringLiteral("warning"), tr("Warning"), "tracker-warning", warning}
    };
    if (!m_separateTrackerStatus)
        for (int i = 1; i < trackerStatusRows.size(); ++i)
            trackerRows.append(trackerStatusRows.at(i));
    QStringList sortedHosts = m_trackerHosts.keys();
    sortedHosts.sort(Qt::CaseInsensitive);
    for (const QString &host : sortedHosts)
        trackerRows.append({QStringLiteral("tracker:") + host,
                            host, "trackers", m_trackerHosts.value(host).size()});

    bool lostSelection = false;
    lostSelection |= updateList(m_status, statusRows);
    applyStatusVisibility();
    lostSelection |= updateList(m_categories, categoryRows);
    lostSelection |= updateList(m_tags, tagRows);
    lostSelection |= updateList(m_trackers, trackerRows);
    lostSelection |= updateList(m_trackerStatuses, trackerStatusRows);
    updateDirectoryTree(torrents);
    if (lostSelection)
        emit filtersChanged();
}


void FilterSidebar::updateDirectoryTree(const QHash<QString, QJsonObject> &torrents)
{
    QHash<QString, DirectoryTotals> next;
    for (const QJsonObject &torrent : torrents)
    {
        QStringList hierarchy = directoryHierarchy(
            torrent.value(QStringLiteral("save_path")).toString());
        if (hierarchy.isEmpty())
            hierarchy.append(QString::fromLatin1(unknownLocation));
        const qint64 size = qMax<qint64>(0,
            static_cast<qint64>(torrent.value(QStringLiteral("size")).toDouble()));
        for (int i = 0; i < hierarchy.size(); ++i)
        {
            DirectoryTotals &entry = next[hierarchy.at(i)];
            ++entry.recursiveCount;
            entry.recursiveBytes += size;
            if (i == hierarchy.size() - 1)
            {
                ++entry.directCount;
                entry.directBytes += size;
            }
        }
    }

    QSet<QString> existing;
    for (auto it = m_directoryItems.cbegin(); it != m_directoryItems.cend(); ++it)
        if (!it.key().isEmpty())
            existing.insert(it.key());
    QSet<QString> incoming;
    for (auto it = next.cbegin(); it != next.cend(); ++it)
        incoming.insert(it.key());
    const bool rebuild = m_directoryItems.isEmpty() || existing != incoming;
    m_directoryTotals = std::move(next);
    if (rebuild)
    {
        // Most sync updates change torrent rates, not directory names.
        // Preserve selection, expanded branches and outer scroll position.
        const int scrollY = verticalScrollBar()->value();
        const QSignalBlocker blocker(m_directories);
        m_directories->clear();
        m_directoryItems.clear();

        auto *all = new QTreeWidgetItem(m_directories);
        all->setData(0, Qt::UserRole, QString());
        all->setIcon(0, QIcon(QStringLiteral(":/icons/filter-all.svg")));
        all->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        m_directoryItems.insert(QString(), all);

        QStringList paths = m_directoryTotals.keys();
        paths.sort(Qt::CaseInsensitive);
        if (!m_expansionInitialized && !paths.isEmpty())
        {
            // The sidebar is intentionally vertically scrollable. Show all
            // discovered save-directory branches initially: deeper paths
            // must not seem absent simply because a parent was collapsed.
            // Subsequent collapses and expansion state remain user-controlled.
            for (const QString &path : paths)
                m_expandedDirectories.insert(path);
            m_expansionInitialized = true;
        }
        for (const QString &path : paths)
        {
            const QString parentPath = parentDirectory(path);
            auto *parent = parentPath.isEmpty()
                ? nullptr : m_directoryItems.value(parentPath);
            auto *item = parent ? new QTreeWidgetItem(parent)
                                : new QTreeWidgetItem(m_directories);
            item->setData(0, Qt::UserRole, path);
            item->setIcon(0, QIcon(QStringLiteral(":/icons/directory.svg")));
            item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
            item->setToolTip(0, path);
            m_directoryItems.insert(path, item);
        }
        for (auto it = m_directoryItems.cbegin(); it != m_directoryItems.cend(); ++it)
            if (!it.key().isEmpty())
                it.value()->setExpanded(m_expandedDirectories.contains(it.key()));

        if (!m_directoryItems.contains(m_selectedDirectory))
            m_selectedDirectory.clear();
        m_directories->setCurrentItem(m_directoryItems.value(m_selectedDirectory));
        resizeDirectoryTree();
        verticalScrollBar()->setValue(scrollY);
    }
    updateDirectoryLabels();
}

void FilterSidebar::updateDirectoryLabels()
{
    if (!m_directories)
        return;

    int totalCount = 0;
    qint64 totalBytes = 0;
    for (const DirectoryTotals &stats : m_directoryTotals)
    {
        totalCount += stats.directCount;
        totalBytes += stats.directBytes;
    }
    if (auto *all = m_directoryItems.value(QString()))
    {
        all->setText(0, tr("All (%1)").arg(totalCount));
        all->setText(1, BytePresentation::dataSize(totalBytes));
    }
    for (auto it = m_directoryTotals.cbegin(); it != m_directoryTotals.cend(); ++it)
    {
        auto *item = m_directoryItems.value(it.key());
        if (!item)
            continue;
        const DirectoryTotals &stats = it.value();
        const int count = m_includeSubfolders ? stats.recursiveCount : stats.directCount;
        const qint64 bytes = m_includeSubfolders ? stats.recursiveBytes : stats.directBytes;
        item->setText(0, tr("%1 (%2)").arg(directoryName(it.key())).arg(count));
        item->setText(1, BytePresentation::dataSize(bytes));
    }
}

void FilterSidebar::resizeDirectoryTree()
{
    if (!m_directories)
        return;

    const auto countVisible = [&](const auto &self, QTreeWidgetItem *item) -> int {
        int count = 1;
        if (item->isExpanded())
            for (int i = 0; i < item->childCount(); ++i)
                count += self(self, item->child(i));
        return count;
    };
    int rows = 0;
    for (int i = 0; i < m_directories->topLevelItemCount(); ++i)
        rows += countVisible(countVisible, m_directories->topLevelItem(i));

    const int heightPerRow = std::max({
        m_directories->fontMetrics().height() + 4,
        m_directories->iconSize().height() + 4,
        m_directories->sizeHintForRow(0)
    });
    // Only the containing FilterSidebar scrolls; no nested scrollbars.
    const int horizontalBarHeight = m_directories->horizontalScrollBar()->sizeHint().height();
    m_directories->setFixedHeight(qMax(1, rows) * heightPerRow
                                  + horizontalBarHeight
                                  + 2 * m_directories->frameWidth());
}

// Action names and order follow the upstream filter widgets. Like upstream,
// the four transfer actions operate on the currently visible transfer list.
void FilterSidebar::showGroupOptions(QListWidget *list, Group group, const QPoint &position)
{
    if (!m_groupActionsEnabled)
        return;
    if (auto *item = list->itemAt(position))
        list->setCurrentItem(item);
    const QString key = choice(list);
    const bool category = group == Group::Categories;
    const bool tag = group == Group::Tags;
    const bool tracker = group == Group::Trackers;
    const bool named = category ? key.startsWith(QLatin1String("category:"))
        : tag ? key.startsWith(QLatin1String("tag:"))
        : tracker && key.startsWith(QLatin1String("tracker:"));
    QMenu menu(list);
    menu.setObjectName(QStringLiteral("sidebarGroupContextMenu"));
    const auto add = [&](const char *icon, const QString &label, const char *name, GroupAction action) {
        auto *entry = menu.addAction(QIcon(QStringLiteral(":/icons/") + QString::fromLatin1(icon)
            + QStringLiteral(".svg")), label);
        entry->setObjectName(QString::fromLatin1(name));
        connect(entry, &QAction::triggered, this, [this, group, key, action]() {
            emit groupActionRequested(group, key, action);
        });
    };
    if (category || tag)
    {
        add("list-add", category ? tr("Add category...") : tr("Add tag..."), "sidebarGroupAdd", GroupAction::Add);
        if (named)
        {
            if (category)
            {
                add("list-add", tr("Add subcategory..."), "sidebarGroupAddSubcategory", GroupAction::AddSubcategory);
                add("edit-rename", tr("Edit category..."), "sidebarGroupEdit", GroupAction::Edit);
            }
            add("edit-clear", category ? tr("Remove category") : tr("Remove tag"), "sidebarGroupRemove", GroupAction::Remove);
        }
        add("edit-clear", category ? tr("Remove unused categories") : tr("Remove unused tags"),
            "sidebarGroupRemoveUnused", GroupAction::RemoveUnused);
        menu.addSeparator();
    }
    else if (tracker && named)
    {
        add("edit-clear", tr("Remove tracker"), "sidebarGroupRemove", GroupAction::Remove);
        menu.addSeparator();
    }
    appendTorrentActions(menu, group, key);
    menu.exec(list->viewport()->mapToGlobal(position));
}

void FilterSidebar::appendTorrentActions(QMenu &menu, Group group, const QString &key)
{
    const auto add = [&](const char *icon, const QString &label, const char *name, GroupAction action) {
        auto *entry = menu.addAction(QIcon(QStringLiteral(":/icons/") + QString::fromLatin1(icon)
            + QStringLiteral(".svg")), label);
        entry->setObjectName(QString::fromLatin1(name));
        entry->setEnabled(m_groupActionsEnabled);
        connect(entry, &QAction::triggered, this, [this, group, key, action]() {
            emit groupActionRequested(group, key, action);
        });
    };
    add("torrent-start", tr("Start torrents"), "sidebarGroupStart", GroupAction::Start);
    add("torrent-start-forced", tr("Force start torrents"), "sidebarGroupForceStart", GroupAction::ForceStart);
    add("torrent-stop", tr("Stop torrents"), "sidebarGroupStop", GroupAction::Stop);
    add("list-remove", tr("Remove torrents"), "sidebarGroupRemoveTorrents", GroupAction::RemoveTorrents);
}

void FilterSidebar::showDirectoryOptions(const QPoint &position, QWidget *source)
{
    // A row context menu selects that directory; the section caption retains
    // the current selection, including All. Match the other filter menus.
    const bool onTree = source == m_directories;
    if (onTree)
        if (auto *item = m_directories->itemAt(position))
            m_directories->setCurrentItem(item);
    QMenu menu(source);
    menu.setObjectName(QStringLiteral("directoryFilterOptions"));
    auto *toggle = menu.addAction(tr("Include subfolders"));
    toggle->setObjectName(QStringLiteral("directoryIncludeSubfolders"));
    toggle->setCheckable(true);
    toggle->setChecked(m_includeSubfolders);
    connect(toggle, &QAction::toggled, this, [this](const bool checked) {
        if (m_includeSubfolders == checked)
            return;
        m_includeSubfolders = checked;
        updateDirectoryLabels();
        emit filtersChanged();
    });
    menu.addSeparator();
    appendTorrentActions(menu, Group::Directories, m_selectedDirectory);
    menu.exec((onTree ? m_directories->viewport() : source)->mapToGlobal(position));
}

void FilterSidebar::saveDirectoryState(QSettings &settings, const QString &layoutRoot) const
{
    const QString root = layoutRoot + QStringLiteral("/directories");
    settings.setValue(root + QStringLiteral("/includeSubfolders"), m_includeSubfolders);
    settings.setValue(root + QStringLiteral("/selectedPath"), m_selectedDirectory);
    QStringList expanded = m_expandedDirectories.values();
    expanded.sort(Qt::CaseSensitive);
    settings.setValue(root + QStringLiteral("/expandedPaths"), expanded);
    settings.setValue(root + QStringLiteral("/sectionExpanded"),
                      m_directorySection->isChecked());
}

void FilterSidebar::restoreDirectoryState(QSettings &settings, const QString &layoutRoot)
{
    const QString root = layoutRoot + QStringLiteral("/directories");
    m_includeSubfolders = settings.value(
        root + QStringLiteral("/includeSubfolders"), false).toBool();
    m_selectedDirectory = settings.value(root + QStringLiteral("/selectedPath")).toString();
    m_expansionInitialized = settings.contains(root + QStringLiteral("/expandedPaths"));
    m_expandedDirectories.clear();
    for (const QString &path : settings.value(
             root + QStringLiteral("/expandedPaths")).toStringList())
        m_expandedDirectories.insert(path);
    m_directorySection->setChecked(
        settings.value(root + QStringLiteral("/sectionExpanded"), true).toBool());
    if (!m_directoryItems.isEmpty())
    {
        const QSignalBlocker block(m_directories);
        for (auto it = m_directoryItems.cbegin(); it != m_directoryItems.cend(); ++it)
            if (!it.key().isEmpty())
                it.value()->setExpanded(m_expandedDirectories.contains(it.key()));
        if (auto *selected = m_directoryItems.value(m_selectedDirectory))
            m_directories->setCurrentItem(selected);
    }
    resizeDirectoryTree();
    updateDirectoryLabels();
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
    const QString statusFilter = m_separateTrackerStatus
        ? choice(m_trackerStatuses) : tracker;
    if (statusFilter == QLatin1String("trackerless")
        && m_torrentsWithTrackers.contains(hash))
        return false;
    if (statusFilter == QLatin1String("tracker-error")
        && !torrent.value(QStringLiteral("has_tracker_error")).toBool())
        return false;
    if (statusFilter == QLatin1String("other-error")
        && !torrent.value(QStringLiteral("has_other_announce_error")).toBool())
        return false;
    if (statusFilter == QLatin1String("warning")
        && !torrent.value(QStringLiteral("has_tracker_warning")).toBool())
        return false;
    if (tracker.startsWith(QLatin1String("tracker:"))
        && !m_trackerHosts.value(tracker.mid(8)).contains(hash))
        return false;

    // One additional AND term, never an alternate filtering mode.
    if (!m_selectedDirectory.isEmpty())
    {
        const QString path = torrentDirectory(torrent);
        if (path != m_selectedDirectory)
        {
            if (!m_includeSubfolders
                || m_selectedDirectory == QLatin1String(unknownLocation))
                return false;
            const QChar separator = m_selectedDirectory.startsWith(QLatin1Char('/'))
                ? QLatin1Char('/') : QLatin1Char('\\');
            const QString prefix = m_selectedDirectory == QLatin1String("/")
                ? QStringLiteral("/") : m_selectedDirectory + separator;
            if (!path.startsWith(prefix, Qt::CaseSensitive))
                return false;
        }
    }
    return true;
}
