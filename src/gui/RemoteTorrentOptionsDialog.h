// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/PathMappingStore.h"
#include <QDialog>
#include <QJsonObject>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QVector>
#include <functional>
#include <memory>

namespace Remote { class RemoteSession; }
namespace Ui { class TorrentOptionsDialog; }

// Upstream Qt Designer Torrent Options, using the owning remote session.
class RemoteTorrentOptionsDialog final : public QDialog
{
    Q_OBJECT
public:
    RemoteTorrentOptionsDialog(Remote::RemoteSession *session,
        QVector<QPair<QString, QJsonObject>> torrents, QStringList categories,
        QList<Remote::PathMapping> mappings, std::function<bool()> selectionStillValid,
        QWidget *parent = nullptr);
    ~RemoteTorrentOptionsDialog() override;
    void accept() override;
private:
    std::unique_ptr<Ui::TorrentOptionsDialog> m_ui;
    QPointer<Remote::RemoteSession> m_session;
    QVector<QPair<QString, QJsonObject>> m_torrents;
    QStringList m_categories;
    QList<Remote::PathMapping> m_mappings;
    std::function<bool()> m_selectionStillValid;
    QSet<QString> m_touched;
};
