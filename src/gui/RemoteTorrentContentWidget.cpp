// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/RemoteTorrentContentWidget.h"

#include <QFileIconProvider>
#include <QHeaderView>
#include <QHash>
#include <QLocale>
#include <QStandardItem>
#include <QStringList>

RemoteTorrentContentWidget::RemoteTorrentContentWidget(QWidget *parent)
    : QTreeView(parent)
    , m_model(new QStandardItemModel(this))
{
    // Original qBittorrent TorrentContentWidget is a QTreeView with a
    // three-column model and icons/check states, not a QTreeWidget.
    setObjectName(QStringLiteral("contentTreeView"));
    setModel(m_model);
    setRootIsDecorated(true);
    setUniformRowHeights(true);
    setSortingEnabled(false);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setExpandsOnDoubleClick(true);

    header()->setHighlightSections(false);
    header()->setStretchLastSection(false);
    header()->setSectionResizeMode(QHeaderView::Interactive);
    header()->setSectionResizeMode(0, QHeaderView::Stretch);
    header()->resizeSection(1, 82);
    header()->resizeSection(2, 99);
    m_model->setHorizontalHeaderLabels({
        tr("Name"), tr("Total Size"), tr("Download Priority")
    });
    setToolTip(tr("File priorities can be changed after adding the torrent "
                  "in the Content tab. Pre-add changes are not supported "
                  "on this server."));
}

void RemoteTorrentContentWidget::setTorrentFiles(
    const QList<Remote::TorrentMetadataFile> &files)
{
    m_model->clear();
    m_model->setHorizontalHeaderLabels({
        tr("Name"), tr("Total Size"), tr("Download Priority")
    });
    m_previewFileCount = 0;
    QFileIconProvider icons;
    QHash<QString, QStandardItem *> folders;
    QHash<QString, qint64> folderTotals;

    for (const auto &file : files)
    {
        const QStringList parts = file.path.split(QLatin1Char('/'),
                                                  Qt::SkipEmptyParts);
        if (parts.isEmpty())
            continue;
        ++m_previewFileCount;

        QString key;
        QStandardItem *folder = nullptr;
        for (int partIndex = 0; partIndex < parts.size(); ++partIndex)
        {
            const QString &part = parts.at(partIndex);
            key += QLatin1Char('/') + part;
            const bool isFile = (partIndex == parts.size() - 1);

            if (!isFile)
            {
                folderTotals[key] += file.size;
                QStandardItem *existing = folders.value(key);
                if (!existing)
                {
                    auto *name = new QStandardItem(icons.icon(QFileIconProvider::Folder), part);
                    name->setData(Qt::Checked, Qt::CheckStateRole);
                    name->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
                    name->setToolTip(toolTip());
                    auto *size = new QStandardItem;
                    auto *priority = new QStandardItem;
                    priority->setText(tr("Normal"));
                    if (folder)
                        folder->appendRow({name, size, priority});
                    else
                        m_model->appendRow({name, size, priority});
                    existing = name;
                    folders.insert(key, existing);
                }
                folder = existing;
            }
            else
            {
                auto *name = new QStandardItem(icons.icon(QFileIconProvider::File), part);
                name->setData(Qt::Checked, Qt::CheckStateRole);
                name->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
                name->setToolTip(toolTip());
                auto *size = new QStandardItem(
                    QLocale().formattedDataSize(file.size));
                size->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
                auto *priority = new QStandardItem(tr("Normal"));
                if (folder)
                    folder->appendRow({name, size, priority});
                else
                    m_model->appendRow({name, size, priority});
            }
        }
    }

    for (auto it = folders.cbegin(); it != folders.cend(); ++it)
    {
        QStandardItem *size = m_model->itemFromIndex(
            m_model->indexFromItem(it.value()).siblingAtColumn(1));
        if (size)
        {
            size->setText(QLocale().formattedDataSize(folderTotals.value(it.key())));
            size->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        }
    }

    expandToDepth(0);
    setFilterText(m_filterText);
}

void RemoteTorrentContentWidget::setFilterText(const QString &text)
{
    m_filterText = text.trimmed();
    filterRows(QModelIndex(), m_filterText, false);
}

bool RemoteTorrentContentWidget::filterRows(const QModelIndex &parent,
                                           const QString &pattern,
                                           bool inheritedMatch)
{
    bool anyMatch = false;
    for (int row = 0; row < m_model->rowCount(parent); ++row)
    {
        const QModelIndex index = m_model->index(row, 0, parent);
        const bool nameMatches = pattern.isEmpty()
            || index.data(Qt::DisplayRole).toString().contains(
                pattern, Qt::CaseInsensitive);
        const bool childMatches =
            filterRows(index, pattern, inheritedMatch || nameMatches);
        const bool visible = inheritedMatch || nameMatches || childMatches;
        setRowHidden(row, parent, !visible);
        anyMatch |= visible;
    }
    return anyMatch;
}
