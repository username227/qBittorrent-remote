// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/ServerAddress.h"
#include "api/ServerProfile.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonObject>
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
    bool autoTMM = false;
    bool useDownloadPath = false;
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

    void commandTorrents(TorrentCommand command, const QStringList &hashes,
                         bool deleteFiles = false);
    void renameTorrent(const QString &hash, const QString &name);
    void setTorrentLocation(const QStringList &hashes, const QString &remotePath);
    void setTorrentCategory(const QStringList &hashes, const QString &category);
    void createAndAssignTorrentCategory(const QStringList &hashes, const QString &category);
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
    void requestPreferences();
    // Session-wide operations affect only this authenticated NAS connection.
    void setAlternativeSpeedLimitsEnabled(bool enabled);
    void setPreferences(const QJsonObject &changes);
    void renameTorrentContent(const QString &hash, const QString &oldPath,
                              const QString &newPath);
    void addTorrentUrls(const QString &urls);
    void addTorrentUrls(const QString &urls, const AddTorrentOptions &options);
    void addTorrentFile(const QString &filename, const QByteArray &contents);
    void addTorrentFile(const QString &filename, const QByteArray &contents,
                        const AddTorrentOptions &options);
    void addTorrentFiles(const QList<TorrentUpload> &files,
                         const AddTorrentOptions &options);
    // Merge only explicitly supplied announce URLs into the existing remote
    // torrent; no local tracker traffic and no re-add of torrent metadata.
    void addTorrentTrackers(const QString &hash, const QStringList &urls);
    void editTorrentTracker(const QString &hash, const QString &oldUrl,
                            const QString &newUrl);
    void removeTorrentTrackers(const QString &hash, const QStringList &urls);
    // Native Edit trackers: replace the complete list, preserving blank-line tiers.
    // The Web API implements this as a checked remove followed by add.
    void replaceTorrentTrackers(const QString &hash, const QString &previousEntries,
                                const QString &replacementEntries, bool privateOrUnknown);
    void requestTorrentDetails(const QString &hash, int tab);
    void setFilePriority(const QString &hash, const QList<int> &fileIds, int priority);

signals:
    void connected(const QString &version, const QString &apiVersion);
    void failed(const QString &reason);
    void mainDataUpdated(const QJsonObject &data);
    void preferencesReceived(const QJsonObject &preferences);
    void preferencesFailed(const QString &reason);
    void operationSucceeded(const QString &description);
    void operationFailed(const QString &description, const QString &reason);
    void torrentDetailsReceived(const QString &hash, int tab, const QJsonValue &data);
    void torrentTrackersChanged(const QString &hash);
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
    bool approveCertificate(const QSslCertificate &certificate, const QStringList &errors);
    void requestVersion();
    void requestApiVersion(const QString &version);
    void requestMainData();
    void pumpOperations();
    void queueOperation(const QString &endpoint, const QByteArray &body,
                        const QByteArray &contentType,
                        const QString &description, bool checkAccepted = false,
                        std::function<void()> onAccepted = {},
                        std::function<void(const QString &)> onRejected = {});
    void checkReply(QNetworkReply *reply,
                    const std::function<void(const QByteArray &)> &onSuccess,
                    const std::function<void(const QString &)> &onFailure = {});
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
        bool preferencesRequest = false;
        QString exportHash;
        QString detailHash;
        int detailTab = -1;
        // Used only for dependent create-then-assign operations. A failed
        // category/tag creation must never enqueue an invalid assignment.
        std::function<void()> onAccepted;
        std::function<void(const QString &)> onRejected;
    };

    QNetworkAccessManager m_network;
    QQueue<QueuedOperation> m_operations;
    std::optional<ServerAddress> m_address;
    QPointer<QNetworkReply> m_pendingReply;
    QString m_pinnedFingerprint;
    QString m_tlsFailure;
    CertificateConfirmation m_confirmation;
    bool m_connected = false;
    qint64 m_syncRid = 0;
    QTimer m_syncTimer;
};

} // namespace Remote
