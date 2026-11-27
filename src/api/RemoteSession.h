// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "api/WebSeedUrl.h"
#include "api/TrackerDetailAction.h"

#include "api/ServerAddress.h"
#include "api/ServerProfile.h"
#include "api/ServerCapabilities.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonObject>
#include <QJsonArray>
#include <QMap>
#include <QJsonValue>
#include <QQueue>
#include <QByteArray>
#include <QStringList>
#include <QTimer>
#include <QObject>
#include <QPointer>
#include <QSslCertificate>
#include <QSslError>

#include <functional>
#include <optional>

namespace Remote
{

struct CertificateChallenge
{
    QSslCertificate certificate;
    QString endpoint;
    QString presentedFingerprint;
    QString previousFingerprint;
    QStringList verificationErrors;
    bool endpointIdentityMismatch = false;
};

// Each remote server owns an independent QNetworkAccessManager and cookie jar.
// TLS errors are never globally bypassed; an explicit, endpoint-bound
// certificate decision is required for a self-signed/untrusted certificate.
// Common, server-side options for both magnet/HTTP URLs and local .torrent
// metadata uploads. Never pass local mount paths to these fields.
struct AddTorrentOptions
{
    QString savePath;
    QString downloadPath;
    QString category;
    QString tags;
    QString contentLayout = QStringLiteral("Original");
    QString stopCondition; // Empty when the server does not advertise support.
    bool useServerDefaults = false;
    bool autoTMM = false;
    bool useDownloadPath = false;
    bool disableDownloadPath = false; // Explicitly use final save path for cross-server seeding.
    bool stopped = false;
    bool sequential = false;
    bool firstLastPiece = false;
    bool queueTop = false;
    bool seedMode = false;
};

// Only .torrent metadata, never downloaded content or a local filesystem
// destination. Multiple uploads use the same server-side AddTorrentOptions.
struct TorrentUpload
{
    QString filename;
    QByteArray contents;
};

class RemoteSession final : public QObject
{
    Q_OBJECT

public:
    using CertificateConfirmation = std::function<bool(const CertificateChallenge &)>;

    enum class TorrentCommand
    {
        Start, Stop, ForceStart, ForceRecheck, ForceReannounce, Delete,
        IncreasePriority, DecreasePriority, TopPriority, BottomPriority
    };

    explicit RemoteSession(QObject *parent = nullptr);

    void setCertificateTrust(QString pinnedFingerprint, CertificateConfirmation confirmation);
    void connectToServer(const ServerProfile &profile, const QString &password);
    void disconnectFromServer();
    bool isConnected() const noexcept;
    const ServerCapabilities &capabilities() const noexcept { return m_capabilities; }
    bool supportsTrackerUrlEditing() const noexcept { return m_connected && m_capabilities.trackerUrlEditing(); }
    bool supportsTrackerTierEditing() const noexcept { return m_connected && m_capabilities.trackerTierEditing(); }
    bool supportsCommentEditing() const noexcept { return m_connected && m_capabilities.commentEditing(); }
    bool supportsSelectedTrackerReannounce() const noexcept { return m_connected && m_capabilities.selectedTrackerReannounce(); }

    void commandTorrents(TorrentCommand command, const QStringList &hashes,
                         bool deleteFiles = false);
    void renameTorrent(const QString &hash, const QString &name);
    void setTorrentComment(const QString &hash, const QString &comment);
    void addTorrentPeers(const QString &hash, const QStringList &peers);
    void banTorrentPeers(const QString &hash, const QStringList &peers);
    void changeWebSeeds(const QString &hash, WebSeedAction action, const QStringList &urls, const QString &replacement);
    void setTorrentLocation(const QStringList &hashes, const QString &remotePath);
    void setTorrentCategory(const QStringList &hashes, const QString &category);
    void createAndAssignTorrentCategory(const QStringList &hashes, const QString &category);
    void saveCategory(const QString &category, const QJsonObject &fields, bool edit,
                      const QStringList &assignHashes = {});
    void removeCategories(const QStringList &categories);
    void manageTagDefinitions(const QStringList &tags, bool create);
    void changeTorrentTags(const QStringList &hashes, const QStringList &tags, bool add);
    void createAndAddTorrentTags(const QStringList &hashes, const QStringList &tags);
    void setTorrentAutoManagement(const QStringList &hashes, bool enabled);
    void setSuperSeeding(const QStringList &hashes, bool enabled);
    void toggleTorrentMode(const QStringList &hashes, bool sequential);
    void setTorrentSavePath(const QStringList &hashes, const QString &path);
    void setTorrentDownloadPath(const QStringList &hashes, const QString &path);
    void setTorrentRateLimit(const QStringList &hashes, bool upload, qint64 bytesPerSec);
    void setTorrentShareLimits(const QString &hash, const QJsonObject &shareLimits);
    void requestTorrentExport(const QString &hash);
    // Scoped callbacks isolate a dialog's requests from other consumers.
    using PreferencesResult = std::function<void(const QJsonObject &)>;
    using PreferencesError = std::function<void(const QString &)>;
    void requestPreferences(PreferencesResult received = {}, PreferencesError failed = {});
    void rotateWebUIAPIKey(PreferencesResult received, PreferencesError failed);
    void deleteWebUIAPIKey(std::function<void()> accepted, PreferencesError failed);
    void requestBuildInfo(PreferencesResult received, PreferencesError failed);
    void requestProcessInfo(PreferencesResult received, PreferencesError failed);
    using ArrayResult = std::function<void(const QJsonArray &)>;
    void requestLog(bool peers, int lastKnownId, ArrayResult received, PreferencesError failed);
    void requestDirectoryContent(const QString &path, bool directories, ArrayResult received, PreferencesError failed);
    bool supportsPathFreeSpace() const noexcept { return m_connected && m_capabilities.pathFreeSpace(); }
    void requestPathFreeSpace(const QString &path, std::function<void(qint64)> received, PreferencesError failed);
    void requestCookies(ArrayResult received, PreferencesError failed);
    void setCookies(const QJsonArray &cookies, std::function<void()> accepted, PreferencesError failed);
    void requestNetworkInterfaces(ArrayResult received, PreferencesError failed);
    void requestNetworkAddresses(const QString &iface, ArrayResult received, PreferencesError failed);
    using BytesResult = std::function<void(const QByteArray &)>;
    void createTorrentTask(const QMap<QString, QString> &params, PreferencesResult received, PreferencesError failed);
    void requestTorrentCreationStatus(const QString &taskId, ArrayResult received, PreferencesError failed);
    void downloadCreatedTorrent(const QString &taskId, BytesResult received, PreferencesError failed);
    void deleteTorrentCreationTask(const QString &taskId, std::function<void()> accepted, PreferencesError failed);
    // RSS stays on the authenticated server. Only these fixed RSS endpoints
    // are permitted; values are encoded once as form/query parameters.
    void requestRss(const QString &action, const QMap<QString, QString> &params,
                    PreferencesResult received, PreferencesError failed);
    void changeRss(const QString &action, const QMap<QString, QString> &params,
                   std::function<void()> accepted, PreferencesError failed);
    // Session-wide operations affect only this authenticated NAS connection.
    void setAlternativeSpeedLimitsEnabled(bool enabled);
    void setSessionPaused(bool paused);
    void sendTestEmail(std::function<void()> accepted, PreferencesError rejected);
    void setPreferences(const QJsonObject &changes,
                        std::function<void()> accepted = {}, PreferencesError rejected = {});
    void renameTorrentContent(const QString &hash, const QString &oldPath,
                              const QString &newPath);
    void renameTorrentContentItem(const QString &hash, const QString &oldPath,
                                  const QString &newPath, bool folder);
    void addTorrentUrls(const QString &urls);
    void addTorrentUrls(const QString &urls, const AddTorrentOptions &options);
    void addTorrentFile(const QString &filename, const QByteArray &contents);
    void addTorrentFile(const QString &filename, const QByteArray &contents,
                        const AddTorrentOptions &options);
    void addTorrentFiles(const QList<TorrentUpload> &files,
                         const AddTorrentOptions &options,
                         std::function<void()> accepted = {}, PreferencesError rejected = {});
    // Merge only explicitly supplied announce URLs into the existing remote
    // torrent; no local tracker traffic and no re-add of torrent metadata.
    void addTorrentTrackers(const QString &hash, const QStringList &urls);
    void editTorrentTracker(const QString &hash, const QString &oldUrl,
                            const QString &newUrl);
    void removeTorrentTrackers(const QString &hash, const QStringList &urls);
    void setTrackerTiers(const QString &hash, const QJsonObject &tiers);
    void changeDetailTrackers(const QString &hash, TrackerDetailAction action, const QStringList &urls, const QString &replacement);
    // Native Edit trackers: replace the complete list, preserving blank-line tiers.
    // The Web API implements this as a checked remove followed by add.
    void replaceTorrentTrackers(const QString &hash, const QString &previousEntries,
                                const QString &replacementEntries, bool privateOrUnknown);
    void requestTorrentDetails(const QString &hash, int tab, bool availability = false);
    void setRefreshInterval(int milliseconds);
    void setFilePriority(const QString &hash, const QList<int> &fileIds, int priority);

signals:
    void connected(const QString &version, const QString &apiVersion);
    void failed(const QString &reason);
    void disconnected();
    void mainDataUpdated(const QJsonObject &data);
    void preferencesReceived(const QJsonObject &preferences);
    // Narrow observation from successful reads, including scoped dialog reads.
    void dhtPreferenceObserved(const QJsonValue &enabled);
    void preferencesFailed(const QString &reason);
    void operationSucceeded(const QString &description);
    void operationFailed(const QString &description, const QString &reason);
    void torrentDetailsReceived(const QString &hash, int tab, const QJsonValue &data);
    void torrentTrackersChanged(const QString &hash);
    void trackerTierEditFinished(const QString &hash);
    void torrentCommentEditFinished(const QString &hash);
    void torrentPeerActionFinished(const QString &hash, const QString &message);
    void webSeedActionFinished(const QString &hash);
    // Identifies the exact edit for bulk progress and rejected requests.
    void torrentTrackerEditFinished(const QString &hash, const QString &oldUrl,
                                    bool success, const QString &reason);
    void torrentContentRenameFinished(const QString &hash, const QString &oldPath,
                                      bool success, const QString &reason);
    void torrentExportReceived(const QString &hash, const QByteArray &metadata);
    void torrentExportFailed(const QString &hash, const QString &reason);
    void torrentDetailsFailed(const QString &hash, int tab, const QString &reason);

private:
    QNetworkRequest requestFor(const QString &endpoint) const;
    void armReply(QNetworkReply *reply);
    void handleSslErrors(QNetworkReply *reply, const QList<QSslError> &errors);
    void handleEncrypted(QNetworkReply *reply);
    bool approveCertificate(const QSslCertificate &certificate, const QStringList &errors,
                            bool endpointIdentityMismatch = false);
    void requestVersion();
    void requestApiVersion(const QString &version);
    void requestMainData();
    void pumpOperations();
    void queueOperation(const QString &endpoint, const QByteArray &body,
                        const QByteArray &contentType,
                        const QString &description, bool checkAccepted = false,
                        std::function<void()> onAccepted = {},
                        std::function<void(const QString &)> onRejected = {},
                        bool reportOperation = true);
    void checkReply(QNetworkReply *reply,
                    const std::function<void(const QByteArray &)> &onSuccess,
                    const std::function<void(const QString &)> &onFailure = {},
                    bool allowNoContent = false);
    void fail(const QString &reason);

    struct QueuedOperation
    {
        QString endpoint;
        QByteArray body;
        QByteArray contentType;
        QString description;
        bool checkAccepted = false;
        bool detailsRequest = false;
        bool exportRequest = false;
        bool jsonObjectRequest = false;
        bool jsonArrayRequest = false;
        bool bytesRequest = false;
        bool jsonObjectPost = false;
        QString exportHash;
        QString detailHash;
        int detailTab = -1;
        quint64 detailGeneration = 0;
        // Used only for dependent create-then-assign operations. A failed
        // category/tag creation must never enqueue an invalid assignment.
        std::function<void()> onAccepted;
        std::function<void(const QString &)> onRejected;
        PreferencesResult onJsonObject;
        ArrayResult onJsonArray;
        BytesResult onBytes;
        bool reportOperation = true;
    };

    QNetworkAccessManager m_network;
    QQueue<QueuedOperation> m_operations;
    std::optional<ServerAddress> m_address;
    QPointer<QNetworkReply> m_pendingReply;
    QString m_pinnedFingerprint;
    QString m_tlsFailure;
    CertificateConfirmation m_confirmation;
    bool m_connected = false;
    ServerCapabilities m_capabilities;
    bool m_detailTrackersKnown = false;
    QStringList m_detailTrackerUrls;
    QString m_detailHash;
    int m_detailPage = -1;
    bool m_detailAvailability = false;
    quint64 m_detailGeneration = 0;
    int m_peerRid = 0;
    bool m_peerPrivacyKnown = false;
    bool m_peerPrivate = true;
    qint64 m_syncRid = 0;
    QTimer m_syncTimer;
};

} // namespace Remote
