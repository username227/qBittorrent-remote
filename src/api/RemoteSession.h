// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/ServerAddress.h"
#include "api/ServerProfile.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonObject>
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

    explicit RemoteSession(QObject *parent = nullptr);

    void setCertificateTrust(QString pinnedFingerprint, CertificateConfirmation confirmation);
    void connectToServer(const ServerProfile &profile, const QString &password);
    void disconnectFromServer();
    bool isConnected() const noexcept;

signals:
    void connected(const QString &version, const QString &apiVersion);
    void failed(const QString &reason);
    void mainDataUpdated(const QJsonObject &data);

private:
    QNetworkRequest requestFor(const QString &endpoint) const;
    void armReply(QNetworkReply *reply);
    void handleSslErrors(QNetworkReply *reply, const QList<QSslError> &errors);
    void handleEncrypted(QNetworkReply *reply);
    bool approveCertificate(const QSslCertificate &certificate, const QStringList &errors);
    void requestVersion();
    void requestApiVersion(const QString &version);
    void requestMainData();
    void checkReply(QNetworkReply *reply,
                    const std::function<void(const QByteArray &)> &onSuccess);
    void fail(const QString &reason);

    QNetworkAccessManager m_network;
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
