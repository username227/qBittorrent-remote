// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/TorrentMetadataReader.h"

#include <QStandardItemModel>
#include <QTreeView>

// Remote adaptation of the native TorrentContentWidget display.
// Keep QTreeView, column model, folder/file icons and hierarchical rows,
// without linking to the desktop libtorrent content handler.
// Pre-add priority changes are intentionally read-only on Web API 2.11.4.
class RemoteTorrentContentWidget final : public QTreeView
{
    Q_OBJECT

public:
    explicit RemoteTorrentContentWidget(QWidget *parent = nullptr);

    void setTorrentFiles(const QList<Remote::TorrentMetadataFile> &files);
    void setFilterText(const QString &text);
    int previewFileCount() const { return m_previewFileCount; }

private:
    bool filterRows(const QModelIndex &parent, const QString &pattern, bool inheritedMatch);
    QStandardItemModel *m_model = nullptr;
    int m_previewFileCount = 0;
    QString m_filterText;
};
