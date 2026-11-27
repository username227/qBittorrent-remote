// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/MappedContentTreeWidget.h"
#include <QDir>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QIcon>
#include <QMenu>
#include <QMessageBox>
#include <memory>
#include <QDrag>
#include <QFileInfo>
#include <QMimeData>
#include <QMimeDatabase>
#include <QPersistentModelIndex>
#include <QTreeWidgetItemIterator>
#include <QMouseEvent>
#include <QKeyEvent>
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

void MappedContentTreeWidget::setContentDragEnabled(bool enabled)
{
    m_contentDragEnabled = enabled;
    setDragEnabled(enabled);
}

void MappedContentTreeWidget::mousePressEvent(QMouseEvent *event)
{
    // Match upstream TorrentContentWidget: Alt temporarily reverses the
    // preferred interaction between drag-and-drop and multiple selection.
    setDragEnabled(m_contentDragEnabled != event->modifiers().testFlag(Qt::AltModifier));
    QTreeWidget::mousePressEvent(event);
}

namespace {
bool isOpenKey(const QKeyEvent *event)
{
    return (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
        && (event->modifiers() == Qt::NoModifier || event->modifiers() == Qt::KeypadModifier);
}
}

bool MappedContentTreeWidget::event(QEvent *event)
{
    // Reserve unmodified Enter for the focused Content view instead of
    // depending on window-level shortcut activation or dialog defaults.
    if (m_openByEnter && event->type() == QEvent::ShortcutOverride
        && isOpenKey(static_cast<QKeyEvent *>(event)))
    {
        event->accept();
        return true;
    }
    return QTreeWidget::event(event);
}

void MappedContentTreeWidget::keyPressEvent(QKeyEvent *event)
{
    if (m_openByEnter && isOpenKey(event))
    {
        event->accept();
        if (!event->isAutoRepeat()) emit openByEnterRequested();
        return;
    }
    QTreeWidget::keyPressEvent(event);
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
            resolvedPath = mappedExistingPath(remote, folder);
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

QString MappedContentTreeWidget::mappedExistingPath(const QString &remote, bool folder) const
{
    if (!remote.startsWith(QLatin1Char('/'))) return {};
    const QString local = Remote::PathMappingStore::toLocal(remote, m_mappings);
    if (local.isEmpty()) return {};
    const QFileInfo localInfo(local);
    if (!(folder ? localInfo.isDir() : localInfo.isFile()) || !localInfo.isReadable())
        return {};
    const QString canonical = localInfo.canonicalFilePath();
    if (canonical.isEmpty()) return {};

    // Symlinks must not lead outside every configured local mount.
    for (const auto &mapping : m_mappings)
    {
        const QString root = QFileInfo(mapping.localRoot).canonicalFilePath();
        if (root.isEmpty()) continue;
        if (canonical == root || canonical.startsWith(
            root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/')))
            return canonical;
    }
    return {};
}

QString MappedContentTreeWidget::mappedIncompletePreviewPath() const
{
    const auto selected = selectedItems();
    if (selected.size() != 1 || !m_contentPath.startsWith(QLatin1Char('/'))) return {};
    auto *item = selected.first();
    if (!item->data(0, Qt::UserRole).isValid()) return {};
    const QVariant progress = item->data(2, Qt::UserRole);
    if (!progress.isValid() || !(progress.toDouble() > 0.0 && progress.toDouble() < 1.0))
        return {};
    const QString mime = QMimeDatabase().mimeTypeForFile(
        item->text(0), QMimeDatabase::MatchExtension).name();
    if (!mime.startsWith(QLatin1String("audio/")) && !mime.startsWith(QLatin1String("video/")))
        return {};

    QStringList parts;
    for (auto *node = item; node; node = node->parent())
    {
        const QString part = node->text(0);
        if (part.isEmpty() || part == QLatin1String(".") || part == QLatin1String("..")
            || part.contains(QLatin1Char('/')) || part.contains(QLatin1Char('\\')))
            return {};
        parts.prepend(part);
    }
    // Upstream TorrentImpl::contentPath() is actualStorageLocation plus
    // the sole file's logical path, or the common first-level folder
    // (if any). Unlike save_path, it follows temporary download storage.
    int fileCount = 0;
    for (QTreeWidgetItemIterator it(const_cast<MappedContentTreeWidget *>(this)); *it; ++it)
        if ((*it)->data(0, Qt::UserRole).isValid() && ++fileCount > 1) break;
    const QString content = QDir::cleanPath(m_contentPath);
    QString remote;
    const QString relative = parts.join(QLatin1Char('/'));
    if (fileCount == 1)
    {
        if (!content.endsWith(QLatin1Char('/') + relative)) return {};
        remote = content;
    }
    else if (topLevelItemCount() == 1 && parts.size() > 1)
    {
        if (!content.endsWith(QLatin1Char('/') + parts.first())) return {};
        parts.removeFirst();
        remote = content + QLatin1Char('/') + parts.join(QLatin1Char('/'));
    }
    else
        remote = content + QLatin1Char('/') + relative;
    remote = QDir::cleanPath(remote);

    // The files endpoint exposes logical names, not actualFilePath().
    // Try only the two known on-disk names at the authoritative location.
    // Do not guess between them if both exist, or fall back to save_path.
    const QString partial = remote + QStringLiteral(".!qB");
    const QString plainLocal = Remote::PathMappingStore::toLocal(remote, m_mappings);
    const QString partialLocal = Remote::PathMappingStore::toLocal(partial, m_mappings);
    if (!plainLocal.isEmpty() && !partialLocal.isEmpty()
        && QFileInfo::exists(plainLocal) && QFileInfo::exists(partialLocal))
        return {};
    const QString plain = mappedExistingPath(remote, false);
    const QString suffixed = mappedExistingPath(partial, false);
    return plain.isEmpty() ? suffixed : plain;
}

QString MappedContentTreeWidget::mappedSinglePath() const
{
    if (selectedItems().size() != 1) return {};
    const std::unique_ptr<QMimeData> mime(mappedSelectionMimeData());
    if (!mime || mime->urls().size() != 1) return {};
    return mime->urls().first().toLocalFile();
}

QString MappedContentTreeWidget::mappedParentPath(const QString &path) const
{
    if (path.isEmpty()) return {};
    const QFileInfo parent(QFileInfo(path).absolutePath());
    if (!parent.isDir() || !parent.isReadable()) return {};
    const QString canonical = parent.canonicalFilePath();
    if (canonical.isEmpty()) return {};
    // Do not expose a directory outside the configured mounts merely because
    // the selected item itself is a mount root.
    for (const auto &mapping : m_mappings)
    {
        const QString root = QFileInfo(mapping.localRoot).canonicalFilePath();
        if (!root.isEmpty() && (canonical == root || canonical.startsWith(
            root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/'))))
            return canonical;
    }
    return {};
}

void MappedContentTreeWidget::openMappedPath(const QString &path)
{
    if (path.isEmpty() || !QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
        QMessageBox::information(this, tr("Cannot open content"),
            tr("The mapped local file or folder could not be opened."));
}

void MappedContentTreeWidget::openSelectedMappedItem()
{
    // Same containment and single-selection checks as the native menu.
    const QString path = mappedSinglePath();
    if (!path.isEmpty()) openMappedPath(path);
}

void MappedContentTreeWidget::addMappedOpenActions(QMenu &menu)
{
    const QString path = mappedSinglePath();
    const QString parent = mappedParentPath(path);
    const QString unavailable = tr("Select one accessible file or folder using this server's local path mappings.");
    const auto add = [&](const QString &name, const QString &label, const QString &icon,
                         bool enabled, auto callback) {
        auto *action = menu.addAction(QIcon(icon), label);
        action->setObjectName(name);
        action->setIconVisibleInMenu(true);
        action->setEnabled(enabled);
        if (!enabled) action->setToolTip(unavailable);
        connect(action, &QAction::triggered, this, callback);
    };
    // Resolve again when triggered: polling, mapping changes, or removed
    // files must not redirect a menu action to another selection.
    add(QStringLiteral("contentOpenMappedItem"), tr("Open"),
        QStringLiteral(":/icons/folder-documents.svg"), !path.isEmpty(),
        [this, path]() {
            if (!path.isEmpty() && mappedSinglePath() == path) openMappedPath(path);
        });
    const QString preview = mappedIncompletePreviewPath();
    const QPersistentModelIndex previewIndex(selectedItems().size() == 1
        ? indexFromItem(selectedItems().first(), 0) : QModelIndex());
    auto *previewAction = menu.addAction(tr("Preview incomplete file"));
    previewAction->setObjectName(QStringLiteral("contentPreviewIncomplete"));
    previewAction->setEnabled(!preview.isEmpty());
    previewAction->setToolTip(tr("Preview a partially downloaded audio or video file at its actual mapped storage location. Requires an associated local application."));
    connect(previewAction, &QAction::triggered, this, [this, preview, previewIndex]() {
        if (previewIndex.isValid() && selectedItems().size() == 1
            && indexFromItem(selectedItems().first(), 0) == previewIndex
            && !preview.isEmpty() && mappedIncompletePreviewPath() == preview)
            openMappedPath(preview);
    });
    add(QStringLiteral("contentOpenMappedParent"), tr("Open containing folder"),
        QStringLiteral(":/icons/directory.svg"), !parent.isEmpty(),
        [this, path, parent]() {
            if (!parent.isEmpty() && mappedSinglePath() == path
                && mappedParentPath(path) == parent) openMappedPath(parent);
        });
    add(QStringLiteral("contentCopyMappedPath"), tr("Copy local path"),
        QStringLiteral(":/icons/edit-copy.svg"), !path.isEmpty(),
        [this, path]() {
            if (!path.isEmpty() && mappedSinglePath() == path)
                QApplication::clipboard()->setText(QDir::toNativeSeparators(path));
        });
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
