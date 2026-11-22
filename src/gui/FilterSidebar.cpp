// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/FilterSidebar.h"

#include "gui/upstream/transferlistfilterswidgetitem.h"

#include <QFrame>
#include <QJsonObject>
#include <QVector>
#include <QIcon>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPalette>
#include <QSizePolicy>
#include <QVBoxLayout>
#include <QWidget>

#include <initializer_list>
#include <utility>

namespace
{
using Item = std::pair<QString, const char *>;

QListWidget *makeFilterList(QWidget *parent, std::initializer_list<Item> entries)
{
    auto *list = new QListWidget(parent);
    list->setObjectName(QStringLiteral("filterList"));
    list->setFrameShape(QFrame::NoFrame);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    list->setUniformItemSizes(true);
    list->setSpacing(0);
    list->setIconSize(QSize(16, 16));

    for (const auto &entry : entries)
    {
        const QString iconPath = QStringLiteral(":/icons/")
            + QString::fromLatin1(entry.second) + QStringLiteral(".svg");
        auto *item = new QListWidgetItem(QIcon(iconPath), entry.first, list);
#ifdef Q_OS_WIN
        // Windows' native item delegate adds much larger vertical padding
        // than the upstream qBittorrent sidebar on Linux. Only on Windows,
        // provide an explicit, DPI-aware row size: room for the inherited
        // font and the 16px icon, plus 4 logical pixels of breathing space.
        const int compactHeight = qMax(list->fontMetrics().height(),
                                       list->iconSize().height()) + 4;
        item->setSizeHint(QSize(0, compactHeight));
#else
        Q_UNUSED(item);
#endif
    }

    list->setCurrentRow(0);
#ifdef Q_OS_WIN
    // Match the actual Windows item height so the fixed-height list has no
    // extra blank space after its last row.
    const int rowHeight = qMax(list->fontMetrics().height(),
                               list->iconSize().height()) + 4;
#else
    // Linux sizing remains exactly as before: native Qt style decides.
    const int rowHeight = qMax(list->fontMetrics().height() + 4, list->sizeHintForRow(0));
#endif
    list->setFixedHeight((rowHeight * list->count()) + (2 * list->frameWidth()));
    return list;
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

    const auto section = [container, layout](const QString &title, QListWidget *list)
    {
        // The collapsible section header is the original upstream widget.
        auto *wrapper = new TransferListFiltersWidgetItem(title, list, container);
        wrapper->setChecked(true);
        layout->addWidget(wrapper);
    };

    section(tr("Status"), makeFilterList(container, {
        {tr("All (0)"), "filter-all"},
        {tr("Downloading (0)"), "downloading"},
        {tr("Seeding (0)"), "upload"},
        {tr("Completed (0)"), "checked-completed"},
        {tr("Running (0)"), "torrent-start"},
        {tr("Stopped (0)"), "stopped"},
        {tr("Active (0)"), "filter-active"},
        {tr("Inactive (0)"), "filter-inactive"},
        {tr("Stalled (0)"), "filter-stalled"},
        {tr("Stalled Uploading (0)"), "stalledUP"},
        {tr("Stalled Downloading (0)"), "stalledDL"},
        {tr("Checking (0)"), "force-recheck"},
        {tr("Moving (0)"), "set-location"},
        {tr("Errored (0)"), "error"}
    }));

    section(tr("Categories"), makeFilterList(container, {
        {tr("All (0)"), "view-categories"},
        {tr("Uncategorized (0)"), "view-categories"}
    }));

    section(tr("Tags"), makeFilterList(container, {
        {tr("All (0)"), "tags"},
        {tr("Untagged (0)"), "tags"}
    }));

    section(tr("Trackers"), makeFilterList(container, {
        {tr("All (0)"), "trackers"},
        {tr("Trackerless (0)"), "trackerless"},
        {tr("Tracker error (0)"), "tracker-error"},
        {tr("Other error (0)"), "tracker-error"},
        {tr("Warning (0)"), "tracker-warning"}
    }));

    layout->addStretch();
    setWidget(container);
}

void FilterSidebar::updateCounts(const QHash<QString, QJsonObject> &torrents)
{
    const QList<QListWidget *> lists = findChildren<QListWidget *>(QStringLiteral("filterList"));
    if (lists.size() != 4)
        return;

    const QStringList labels {
        QStringLiteral("All"), QStringLiteral("Downloading"), QStringLiteral("Seeding"),
        QStringLiteral("Completed"), QStringLiteral("Running"), QStringLiteral("Stopped"),
        QStringLiteral("Active"), QStringLiteral("Inactive"), QStringLiteral("Stalled"),
        QStringLiteral("Stalled Uploading"), QStringLiteral("Stalled Downloading"),
        QStringLiteral("Checking"), QStringLiteral("Moving"), QStringLiteral("Errored")
    };
    QVector<int> counts(14, 0);
    int uncategorized = 0;
    int untagged = 0;
    int trackerless = 0;
    int errors = 0;
    int warnings = 0;

    for (const QJsonObject &torrent : torrents)
    {
        const QString state = torrent.value(QStringLiteral("state")).toString();
        const bool finished = torrent.value(QStringLiteral("progress")).toDouble() >= 1;
        const bool down = state == QLatin1String("downloading") || state == QLatin1String("forcedDL")
                          || state == QLatin1String("metaDL") || state == QLatin1String("stalledDL");
        const bool up = state == QLatin1String("uploading") || state == QLatin1String("forcedUP")
                        || state == QLatin1String("stalledUP");
        const bool stopped = state.startsWith(QLatin1String("stopped"))
                             || state.startsWith(QLatin1String("paused"));
        const bool active = torrent.value(QStringLiteral("dlspeed")).toDouble() > 0
                            || torrent.value(QStringLiteral("upspeed")).toDouble() > 0;
        ++counts[0];
        counts[1] += down;
        counts[2] += up;
        counts[3] += finished;
        counts[4] += !stopped && (down || up);
        counts[5] += stopped;
        counts[6] += active;
        counts[7] += !active;
        counts[8] += state.startsWith(QLatin1String("stalled"));
        counts[9] += state == QLatin1String("stalledUP");
        counts[10] += state == QLatin1String("stalledDL");
        counts[11] += state.startsWith(QLatin1String("checking"));
        counts[12] += state == QLatin1String("moving");
        counts[13] += state == QLatin1String("error") || state == QLatin1String("missingFiles");

        uncategorized += torrent.value(QStringLiteral("category")).toString().isEmpty();
        untagged += torrent.value(QStringLiteral("tags")).toString().isEmpty();
        trackerless += torrent.value(QStringLiteral("tracker")).toString().isEmpty();
        errors += state == QLatin1String("error");
        warnings += state == QLatin1String("missingFiles");
    }

    for (int row = 0; row < counts.size(); ++row)
        lists.at(0)->item(row)->setText(
            QStringLiteral("%1 (%2)").arg(labels.at(row)).arg(counts.at(row)));

    const auto update = [](QListWidget *list, int row, const QString &label, int count) {
        list->item(row)->setText(QStringLiteral("%1 (%2)").arg(label).arg(count));
    };
    update(lists.at(1), 0, QStringLiteral("All"), torrents.size());
    update(lists.at(1), 1, QStringLiteral("Uncategorized"), uncategorized);
    update(lists.at(2), 0, QStringLiteral("All"), torrents.size());
    update(lists.at(2), 1, QStringLiteral("Untagged"), untagged);
    update(lists.at(3), 0, QStringLiteral("All"), torrents.size());
    update(lists.at(3), 1, QStringLiteral("Trackerless"), trackerless);
    update(lists.at(3), 2, QStringLiteral("Tracker error"), errors);
    update(lists.at(3), 3, QStringLiteral("Other error"), warnings);
    update(lists.at(3), 4, QStringLiteral("Warning"), warnings);
}
