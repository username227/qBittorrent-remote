// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "api/WebSeedUrl.h"

#include "api/PathMappingStore.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QWidget>

class QLabel;
class RemotePiecesBar;
class QTableWidget;
class QTreeWidget;
class SpeedGraphWidget;
namespace Ui { class PropertiesWidget; }

// Original qBittorrent property-panel layout, adapted for Web API-only data.
// There are no local torrent or filesystem objects in this widget.
class TorrentDetailsWidget final : public QWidget
{
    Q_OBJECT
public:
    explicit TorrentDetailsWidget(QWidget *parent = nullptr);
    ~TorrentDetailsWidget() override;

    void showTab(int tab);
    int currentTab() const noexcept { return m_currentTab; }
    bool wantsPieceAvailability() const;
    void setSelection(const QString &hash, const QJsonObject &summary = {});
    void setDateFormat(const QString &pattern);
    void displayDetails(const QString &hash, int tab, const QJsonValue &data);
    void displayError(const QString &hash, int tab, const QString &reason);
    void appendRates(double downloadBytesPerSec, double uploadBytesPerSec);
    void setContentPathMappings(const QList<Remote::PathMapping> &mappings);
    void setContentDragEnabled(bool enabled);
    void setTrackerTierEditingSupported(bool supported);
    void finishTrackerTierEdit(const QString &hash);
    void setCommentEditingSupported(bool supported);
    void finishCommentEdit(const QString &hash);
    void finishPeerAction(const QString &hash, const QString &message);
    void finishWebSeedAction(const QString &hash);

signals:
    void filePriorityRequested(const QString &hash, const QList<int> &ids, int priority);
    void trackerTiersRequested(const QString &hash, const QJsonObject &tiers);
    void commentEditRequested(const QString &hash, const QString &comment);
    void addPeersRequested(const QString &hash, const QStringList &peers);
    void banPeersRequested(const QString &hash, const QStringList &peers);
    void webSeedActionRequested(const QString &hash, Remote::WebSeedAction action,
                                const QStringList &urls, const QString &replacement);

private:
    void displayGeneral(const QJsonObject &properties);
    void updateCommentEditor();
    void editComment();
    void displayTrackers(const QJsonArray &trackers);
    QJsonObject adjustedTrackerTiers(int delta) const;
    void updateTrackerTierButtons();
    void changeTrackerTiers(int delta);
    void displayPeers(const QJsonObject &peers);
    void copySelectedPeers();
    QStringList selectedPeerEndpoints() const;
    bool canAddPeers() const;
    void addPeers();
    void banSelectedPeers();
    void displayWebSeeds(const QJsonArray &sources);
    QStringList selectedWebSeeds() const;
    bool canEditWebSeeds() const;
    void addWebSeed();
    void editWebSeed();
    void removeWebSeeds();
    void copyWebSeeds();
    void displayFiles(const QJsonArray &files);
    void chooseFilePriorities(int priority, bool allFiles);
    void updateFileFolders();
    void resetPages();
    void setValue(const char *labelObjectName, const QString &text);

    Ui::PropertiesWidget *m_ui = nullptr;
    QTreeWidget *m_trackers = nullptr;
    QTableWidget *m_peers = nullptr;
    QJsonObject m_peerRecords;
    QJsonValue m_peerPrivacy;
    QString m_pendingPeerHash;
    QString m_pendingWebSeedHash;
    bool m_webSeedsKnown = false;
    SpeedGraphWidget *m_speedGraph = nullptr;
    RemotePiecesBar *m_progressBar = nullptr;
    RemotePiecesBar *m_availabilityBar = nullptr;
    QLabel *m_error = nullptr;
    QString m_hash;
    QString m_pendingTrackerTierHash;
    bool m_trackerTierSupported = false;
    bool m_trackerTiersKnown = false;
    bool m_commentEditingSupported = false;
    bool m_commentKnown = false;
    QString m_pendingCommentHash;
    QJsonObject m_summary;
    QJsonObject m_lastGeneral;
    QString m_dateFormat;
    int m_currentTab = 0;
};
