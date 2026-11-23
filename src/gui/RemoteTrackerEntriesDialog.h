// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDialog>
#include <QHash>
#include <QJsonArray>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QVector>

#include <functional>
#include <memory>

namespace Remote { class RemoteSession; }
namespace Ui { class TrackerEntriesDialog; }

// The original qBittorrent TrackerEntriesDialog .ui and its complete-list
// semantics, adapted only to fetch/apply state through a remote Web API.
class RemoteTrackerEntriesDialog final : public QDialog
{
    Q_OBJECT
public:
    RemoteTrackerEntriesDialog(Remote::RemoteSession *session, QStringList hashes,
                               std::function<bool()> selectionStillValid,
                               QWidget *parent = nullptr,
                               QSet<QString> protectedHashes = {});
    ~RemoteTrackerEntriesDialog() override;

private:
    struct Entry
    {
        QString url;
        int tier = 0;
    };

    void loadNext();
    void received(const QString &hash, int tab, const QJsonArray &data);
    void failed(const QString &hash, int tab, const QString &reason);
    void apply();

    static QVector<Entry> entriesFromServer(const QJsonArray &data);
    static QString entriesText(const QVector<Entry> &entries);

    std::unique_ptr<Ui::TrackerEntriesDialog> m_ui;
    QPointer<Remote::RemoteSession> m_session;
    QStringList m_hashes;
    std::function<bool()> m_selectionStillValid;
    QSet<QString> m_protectedHashes;
    QHash<QString, QString> m_existingEntries;
    QVector<Entry> m_commonEntries;
    int m_index = 0;
    bool m_loaded = false;
};
