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
class RemoteSession final : public QObject
{
    Q_OBJECT

public:
    using CertificateConfirmation = std::function<bool(const CertificateChallenge &)>;

    enum class TorrentCommand
    {
        Start, Stop, Delete, IncreasePriority, DecreasePriority,
        TopPriority, BottomPriority
    };

    explicit RemoteSession(QObject *parent = nullptr);

    void setCertificateTrust(QString pinnedFingerprint, CertificateConfirmation confirmation);
    void connectToServer(const ServerProfile &profile, const QString &password);
    void disconnectFromServer();
    bool isConnected() const noexcept;

    void commandTorrents(TorrentCommand command, const QStringList &hashes,
                         bool deleteFiles = false);
    void addTorrentUrls(const QString &urls);
    void addTorrentFile(const QString &filename, const QByteArray &contents);
    void requestTorrentDetails(const QString &hash, int tab);
    void setFilePriority(const QString &hash, const QList<int> &fileIds, int priority);

signals:
    void connected(const QString &version, const QString &apiVersion);
    void failed(const QString &reason);
    void mainDataUpdated(const QJsonObject &data);
    void operationSucceeded(const QString &description);
    void operationFailed(const QString &description, const QString &reason);
    void torrentDetailsReceived(const QString &hash, int tab, const QJsonValue &data);
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
                        const QString &description, bool checkAccepted = false);
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
        QString detailHash;
        int detailTab = -1;
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
