// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/MappedContentTreeWidget.h"
#include <QDir>
#include <QDrag>
#include <QFileInfo>
#include <QMimeData>
#include <QSet>
#include <QUrl>

MappedContentTreeWidget::MappedContentTreeWidget(QWidget *parent)
    : QTreeWidget(parent)
{
}

void MappedContentTreeWidget::setPathMappings(
    const QList<Remote::PathMapping> &mappings)
{
    m_mappings = mappings;
}

void MappedContentTreeWidget::setRemoteRoots(const QString &savePath,
                                             const QString &contentPath)
{
    m_savePath = savePath;
    m_contentPath = contentPath;
}

QMimeData *MappedContentTreeWidget::mappedSelectionMimeData() const
{
    const auto selected = selectedItems();
    if (m_mappings.isEmpty() || selected.isEmpty()) return nullptr;

    QList<QUrl> urls;
    QSet<QString> emitted;
    for (QTreeWidgetItem *item : selected)
    {
        // Do not repeat selected files underneath a selected folder.
        bool coveredByParent = false;
        for (QTreeWidgetItem *parent = item->parent(); parent; parent = parent->parent())
            if (selected.contains(parent)) { coveredByParent = true; break; }
        if (coveredByParent) continue;

        QStringList parts;
        for (QTreeWidgetItem *node = item; node; node = node->parent())
        {
            const QString part = node->text(0);
            if (part.isEmpty() || part == QLatin1String(".") || part == QLatin1String("..")
                || part.contains(QLatin1Char('/')) || part.contains(QLatin1Char('\\')))
                return nullptr; // Never follow a remote path traversal.
            parts.prepend(part);
        }
        const QString relative = parts.join(QLatin1Char('/'));
        const bool folder = !item->data(0, Qt::UserRole).isValid();
        QStringList candidates;
        if (!m_contentPath.isEmpty())
        {
            candidates.append(QDir::cleanPath(m_contentPath + QLatin1Char('/') + relative));
            if (!folder && QFileInfo(m_contentPath).fileName() == relative)
                candidates.append(m_contentPath); // Single-file content path.
        }
        if (!m_savePath.isEmpty())
            candidates.append(QDir::cleanPath(m_savePath + QLatin1Char('/') + relative));

        QString resolvedPath;
        for (const QString &remote : candidates)
        {
            if (!remote.startsWith(QLatin1Char('/'))) continue;
            const QString local = Remote::PathMappingStore::toLocal(remote, m_mappings);
            if (local.isEmpty()) continue;
            const QFileInfo localInfo(local);
            if (!(folder ? localInfo.isDir() : localInfo.isFile()) || !localInfo.isReadable())
                continue;
            const QString canonical = localInfo.canonicalFilePath();
            if (canonical.isEmpty()) continue;

            // Symlinks must not lead outside every configured local mount.
            for (const auto &mapping : m_mappings)
            {
                const QString root = QFileInfo(mapping.localRoot).canonicalFilePath();
                if (root.isEmpty()) continue;
                if (canonical == root || canonical.startsWith(
                    root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/')))
                {
                    resolvedPath = canonical;
                    break;
                }
            }
            if (!resolvedPath.isEmpty()) break;
        }
        // Never promise partially accessible collections to the file manager.
        if (resolvedPath.isEmpty()) return nullptr;
        if (!emitted.contains(resolvedPath))
        {
            emitted.insert(resolvedPath);
            urls.append(QUrl::fromLocalFile(resolvedPath));
        }
    }
    if (urls.isEmpty()) return nullptr;
    auto *mime = new QMimeData;
    mime->setUrls(urls); // Interoperable text/uri-list, not custom Qt MIME.
    return mime;
}

void MappedContentTreeWidget::startDrag(Qt::DropActions)
{
    QMimeData *mime = mappedSelectionMimeData();
    if (!mime) return;
    auto *drag = new QDrag(this);
    drag->setMimeData(mime);
    drag->exec(Qt::CopyAction, Qt::CopyAction); // Never move NAS data.
    drag->deleteLater();
}
