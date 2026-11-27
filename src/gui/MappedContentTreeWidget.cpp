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
