// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "api/PathMappingStore.h"
#include <QTreeWidget>

class QMimeData;
class QMouseEvent;
class QMenu;

// Only local files/folders accessible via a selected server's configured
// mount mapping can be dragged out of the remote Content tab.
class MappedContentTreeWidget final : public QTreeWidget
{
    Q_OBJECT
public:
    explicit MappedContentTreeWidget(QWidget *parent = nullptr);
    void setPathMappings(const QList<Remote::PathMapping> &mappings);
    void setRemoteRoots(const QString &savePath, const QString &contentPath);
    void setContentDragEnabled(bool enabled);
    QMimeData *mappedSelectionMimeData() const;
    void addMappedOpenActions(QMenu &menu);

protected:
    void startDrag(Qt::DropActions supportedActions) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    QString mappedSinglePath() const;
    QString mappedParentPath(const QString &path) const;
    void openMappedPath(const QString &path);
    QList<Remote::PathMapping> m_mappings;
    QString m_savePath;
    QString m_contentPath;
    bool m_contentDragEnabled = true;
};
