// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/FilterSidebar.h"

#include "gui/upstream/transferlistfilterswidgetitem.h"

#include <QFrame>
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
        new QListWidgetItem(QIcon(iconPath), entry.first, list);
    }

    list->setCurrentRow(0);
    const int rowHeight = qMax(list->fontMetrics().height() + 4, list->sizeHintForRow(0));
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
