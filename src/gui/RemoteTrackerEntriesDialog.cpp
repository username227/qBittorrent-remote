// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/RemoteTrackerEntriesDialog.h"

#include "api/RemoteSession.h"
#include "ui_trackerentriesdialog.h"

#include <QDialogButtonBox>
#include <QJsonObject>
#include <QMap>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <utility>

namespace
{
QString trackerIdentity(const QString &url, int tier)
{
    return QString::number(tier) + QChar(0) + url;
}
}

RemoteTrackerEntriesDialog::RemoteTrackerEntriesDialog(
    Remote::RemoteSession *session, QStringList hashes,
    std::function<bool()> selectionStillValid, QWidget *parent,
    QSet<QString> protectedHashes)
    : QDialog(parent)
    , m_ui(std::make_unique<Ui::TrackerEntriesDialog>())
    , m_session(session)
    , m_hashes(std::move(hashes))
    , m_selectionStillValid(std::move(selectionStillValid))
    , m_protectedHashes(std::move(protectedHashes))
{
    m_ui->setupUi(this); // Upstream qBittorrent's unchanged Qt Designer layout.
    setObjectName(QStringLiteral("remoteTrackerEntriesDialog"));
    m_ui->plainTextEdit->setEnabled(false);
    m_ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(false);
    connect(m_ui->buttonBox, &QDialogButtonBox::accepted,
            this, &RemoteTrackerEntriesDialog::apply);
    connect(m_ui->buttonBox, &QDialogButtonBox::rejected,
            this, &QDialog::reject);

    if (m_session)
    {
        connect(m_session, &Remote::RemoteSession::torrentDetailsReceived,
                this, [this](const QString &hash, int tab, const QJsonValue &value) {
                    if (tab == 1)
                        received(hash, tab, value.toArray());
                });
        connect(m_session, &Remote::RemoteSession::torrentDetailsFailed,
                this, &RemoteTrackerEntriesDialog::failed);
    }
    QTimer::singleShot(0, this, &RemoteTrackerEntriesDialog::loadNext);
}

RemoteTrackerEntriesDialog::~RemoteTrackerEntriesDialog() = default;

QVector<RemoteTrackerEntriesDialog::Entry>
RemoteTrackerEntriesDialog::entriesFromServer(const QJsonArray &data)
{
    QVector<Entry> entries;
    for (const QJsonValue &value : data)
    {
        const QJsonObject row = value.toObject();
        const QString url = row.value(QStringLiteral("url")).toString();
        if (url.isEmpty() || url.startsWith(QLatin1String("** [")))
            continue; // DHT / PeX / LSD pseudo-trackers aren't editable URLs.
        entries.append({url, row.value(QStringLiteral("tier")).toInt(0)});
    }
    return entries;
}

QString RemoteTrackerEntriesDialog::entriesText(const QVector<Entry> &entries)
{
    QMap<int, QString> tiers;
    int maxTier = -1;
    for (const Entry &entry : entries)
    {
        if (entry.tier < 0)
            continue;
        tiers[entry.tier] += entry.url + QLatin1Char('\n');
        maxTier = std::max(maxTier, entry.tier);
    }
    QString text = tiers.value(0);
    for (int tier = 1; tier <= maxTier; ++tier)
        text += QLatin1Char('\n') + tiers.value(tier);
    return text;
}

void RemoteTrackerEntriesDialog::loadNext()
{
    if (!m_session || !m_session->isConnected()
        || !m_selectionStillValid || !m_selectionStillValid())
    {
        failed({}, 1, tr("The remote server or torrent selection changed."));
        return;
    }
    if (m_index >= m_hashes.size())
    {
        m_ui->plainTextEdit->setPlainText(entriesText(m_commonEntries));
        m_ui->plainTextEdit->setEnabled(true);
        m_ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(true);
        m_loaded = true;
        return;
    }
    m_session->requestTorrentDetails(m_hashes.at(m_index), 1);
}

void RemoteTrackerEntriesDialog::received(
    const QString &hash, int tab, const QJsonArray &data)
{
    if (tab != 1 || m_loaded || m_index >= m_hashes.size()
        || hash != m_hashes.at(m_index))
        return;

    const QVector<Entry> entries = entriesFromServer(data);
    m_existingEntries.insert(hash, entriesText(entries));
    if (m_index == 0)
        m_commonEntries = entries;
    else
    {
        QSet<QString> identities;
        for (const Entry &entry : entries)
            identities.insert(trackerIdentity(entry.url, entry.tier));
        m_commonEntries.removeIf([&](const Entry &entry) {
            return !identities.contains(trackerIdentity(entry.url, entry.tier));
        });
    }

    ++m_index;
    QTimer::singleShot(0, this, &RemoteTrackerEntriesDialog::loadNext);
}

void RemoteTrackerEntriesDialog::failed(
    const QString &hash, int tab, const QString &reason)
{
    if (tab != 1 || m_loaded || m_index >= m_hashes.size())
        return;
    if (!hash.isEmpty() && hash != m_hashes.at(m_index))
        return;
    QMessageBox::warning(this, tr("Edit trackers"),
        tr("Could not load every selected torrent's tracker list: %1").arg(reason));
    reject();
}

void RemoteTrackerEntriesDialog::apply()
{
    if (!m_loaded || !m_session || !m_session->isConnected()
        || !m_selectionStillValid || !m_selectionStillValid())
        return;

    const QString replacement = m_ui->plainTextEdit->toPlainText();
    QSet<QString> newUrls;
    for (QString line : replacement.split(QLatin1Char('\n')))
    {
        line = line.trimmed();
        if (line.isEmpty())
            continue; // Blank lines are upstream tier separators.
        const QUrl url(line, QUrl::StrictMode);
        const QString scheme = url.scheme().toLower();
        if (!url.isValid() || url.host().isEmpty()
            || (scheme != QLatin1String("http")
                && scheme != QLatin1String("https")
                && scheme != QLatin1String("udp"))
            || line.contains(QLatin1Char('|')))
        {
            QMessageBox::warning(this, tr("Edit trackers"),
                                 tr("Invalid tracker URL: %1").arg(line));
            return;
        }
        newUrls.insert(line);
    }

    // Validate the entire selection before modifying ANY torrent. Counts
    // include all announce URLs, not only the currently announcing tier.
    // An existing private torrent may replace or remove trackers, but may
    // never gain more URLs. Recovery from zero to ONE URL is permitted.
    for (const QString &hash : m_hashes)
    {
        if (!m_protectedHashes.contains(hash))
            continue;
        QSet<QString> oldUrls;
        for (const QString &line : m_existingEntries.value(hash).split(QLatin1Char('\n')))
        {
            const QString url = line.trimmed();
            if (!url.isEmpty())
                oldUrls.insert(url);
        }
        if (newUrls.size() > oldUrls.size()
            && !(oldUrls.isEmpty() && newUrls.size() == 1))
        {
            QMessageBox blocked(QMessageBox::Warning,
                tr("Additional trackers blocked"),
                tr("For your safety, qBittorrent Remote does not allow adding "
                   "additional trackers to private torrents. Doing so can expose "
                   "the private swarm and affect ratio accounting.\n\n"
                   "You can still edit or remove existing trackers, or restore "
                   "one tracker to a trackerless torrent.\n\n"
                   "If you know what you are doing and still want to add "
                   "trackers, please use the server's qBittorrent Web UI.\n\n"
                   "No torrents were changed."),
                QMessageBox::Ok, this);
            blocked.setObjectName(QStringLiteral("privateTorrentTrackerAdditionBlocked"));
            blocked.exec();
            return;
        }
    }

    // OK means REPLACE ALL, not merge. Every selected torrent receives the
    // complete text, including blank-line tiers, even when no trackers
    // were initially common. Empty text removes all trackers.
    for (const QString &hash : m_hashes)
        m_session->replaceTorrentTrackers(hash, m_existingEntries.value(hash),
                                          replacement, m_protectedHashes.contains(hash));
    accept();
}
