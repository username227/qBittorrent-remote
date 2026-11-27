// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDialog>
#include <QHash>
#include <QJsonArray>
#include <QPointer>
#include <QSet>
#include <QString>

#include <functional>
#include <memory>

class QStandardItemModel;
namespace Remote { class RemoteSession; }
namespace Ui { class TorrentContentLayoutDialog; }

// Native upstream TorrentContentLayoutDialog UI, using the server's
// file list and Web API renaming instead of a local libtorrent handle.
class RemoteTorrentContentLayoutDialog final : public QDialog
{
    Q_OBJECT
public:
    RemoteTorrentContentLayoutDialog(Remote::RemoteSession *session,
        QString hash, std::function<bool()> selectionStillValid,
        QWidget *parent = nullptr, QSet<int> selectedIds = {});
    ~RemoteTorrentContentLayoutDialog() override;
    void reject() override;

private:
    void reload();
    void received(const QString &hash, int tab, const QJsonArray &files);
    void fileRenameFinished(const QString &hash, const QString &oldPath,
                            bool success, const QString &reason);
    void updateApply();
    void updateCommonPath();
    void editCommonPath(const QString &newPrefix);
    void apply();
    bool selectionValid() const;
    static bool validTorrentRelativePath(const QString &path);

    std::unique_ptr<Ui::TorrentContentLayoutDialog> m_ui;
    QPointer<Remote::RemoteSession> m_session;
    QString m_hash;
    std::function<bool()> m_selectionStillValid;
    QStandardItemModel *m_model = nullptr;
    QHash<int, QString> m_original;
    QString m_commonPrefix;
    QHash<int, QString> m_commonSuffixes;
    QSet<int> m_selectedIds;
    bool m_initialSelection = true;
    QSet<QString> m_pendingPaths;
    int m_pending = 0;
    bool m_loading = false;
};
