// SPDX-License-Identifier: GPL-3.0-or-later
#include "api/RemoteSession.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QHostAddress>
#include <QNetworkCookieJar>
#include <QSslConfiguration>
#include <QUrlQuery>

#include <algorithm>
#include <utility>

namespace
{
// Approval of an exact certificate pin may waive *only* problems caused by
// a private/self-signed certificate authority. Hostname mismatch, expiry,
// revocation, invalid signatures, weak keys and other errors are fatal.
bool isUntrustedIssuerOnly(const QList<QSslError> &errors,
                           bool allowLegacyIpCommonName)
{
    if (errors.isEmpty())
        return false;
    for (const QSslError &error : errors)
    {
        switch (error.error())
        {
        case QSslError::SelfSignedCertificate:
        case QSslError::SelfSignedCertificateInChain:
        case QSslError::UnableToGetLocalIssuerCertificate:
        case QSslError::UnableToGetIssuerCertificate:
        case QSslError::UnableToVerifyFirstCertificate:
        case QSslError::CertificateUntrusted:
            break;
        case QSslError::HostNameMismatch:
            if (allowLegacyIpCommonName)
                break;
            return false;
        default:
            return false;
        }
    }
    return true;
}

QString certificateFingerprint(const QSslCertificate &certificate)
{
    return QString::fromLatin1(
        certificate.digest(QCryptographicHash::Sha256).toHex());
}

bool withinValidityPeriod(const QSslCertificate &certificate)
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    return !certificate.isNull()
        && certificate.effectiveDate().isValid()
        && certificate.expiryDate().isValid()
        && certificate.effectiveDate() <= now
        && now <= certificate.expiryDate();
}

bool matchesLegacyIpCommonName(const QSslCertificate &certificate,
                               const QString &hostname)
{
    QHostAddress requestedAddress;
    if (!requestedAddress.setAddress(hostname)
        || !certificate.subjectAlternativeNames().isEmpty())
        return false;

    // Legacy private/self-signed certificates frequently omit SAN entirely.
    // Only an exact IP literal in Common Name qualifies for compatibility;
    // any SAN present takes precedence and cannot be bypassed by Common Name.
    for (const QString &commonName : certificate.subjectInfo(QSslCertificate::CommonName))
    {
        QHostAddress namedAddress;
        if (namedAddress.setAddress(commonName) && namedAddress == requestedAddress)
            return true;
    }
    return false;
}

bool matchesIpAddressSubjectAltName(const QSslCertificate &certificate,
                                    const QString &hostname)
{
    QHostAddress requestedAddress;
    if (!requestedAddress.setAddress(hostname))
    {
        // Qt verifies DNS hostnames; this extra check applies to IP URLs.
        return true;
    }

    const auto alternativeNames = certificate.subjectAlternativeNames();
    for (const QString &entry : alternativeNames.values(QSsl::IpAddressEntry))
    {
        QHostAddress namedAddress;
        if (namedAddress.setAddress(entry) && namedAddress == requestedAddress)
            return true;
    }

    // No SAN at all: permit an exact legacy CN as a hostname identity.
    // Approval of an untrusted certificate is still required and pinned.
    return matchesLegacyIpCommonName(certificate, hostname);
}
} // namespace

namespace Remote
{

RemoteSession::RemoteSession(QObject *parent)
    : QObject(parent)
    , m_network(this)
{
    m_network.setCookieJar(new QNetworkCookieJar(&m_network));
    m_syncTimer.setInterval(2000);
    connect(&m_syncTimer, &QTimer::timeout, this, &RemoteSession::requestMainData);
}

void RemoteSession::setCertificateTrust(QString pinnedFingerprint,
                                        CertificateConfirmation confirmation)
{
    m_pinnedFingerprint = std::move(pinnedFingerprint);
    m_confirmation = std::move(confirmation);
}

QNetworkRequest RemoteSession::requestFor(const QString &endpoint) const
{
    QNetworkRequest request(m_address->apiUrl(endpoint));
    request.setRawHeader("Referer",
                         m_address->baseUrl().toString(QUrl::FullyEncoded).toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(15000);
    return request;
}

void RemoteSession::armReply(QNetworkReply *reply)
{
    m_tlsFailure.clear();
    m_pendingReply = reply;

    // Both callbacks run synchronously on this thread. In particular, Qt
    // requires ignoreSslErrors() inside the signal handler, not later in a
    // queued callback. Every request is guarded, including logout.
    connect(reply, &QNetworkReply::sslErrors, this,
            [this, reply](const QList<QSslError> &errors) {
                handleSslErrors(reply, errors);
            }, Qt::DirectConnection);
    connect(reply, &QNetworkReply::encrypted, this,
            [this, reply]() {
                handleEncrypted(reply);
            }, Qt::DirectConnection);
}

bool RemoteSession::approveCertificate(const QSslCertificate &certificate,
                                       const QStringList &errors)
{
    if (!withinValidityPeriod(certificate) || !m_confirmation || !m_address)
        return false;

    const QString presented = certificateFingerprint(certificate);
    if (presented == m_pinnedFingerprint && !presented.isEmpty())
        return true;

    const CertificateChallenge challenge {
        certificate,
        m_address->baseUrl().toString(),
        presented,
        m_pinnedFingerprint,
        errors
    };

    // Only the UI can authorize and persist a trust decision. A rejected or
    // failed persistence operation does not advance the trusted fingerprint.
    if (!m_confirmation(challenge))
        return false;

    m_pinnedFingerprint = presented;
    return true;
}

void RemoteSession::handleSslErrors(QNetworkReply *reply,
                                    const QList<QSslError> &errors)
{
    if (m_pendingReply != reply || !m_address)
        return;

    const QSslCertificate certificate = reply->sslConfiguration().peerCertificate();
    if (!withinValidityPeriod(certificate))
    {
        m_tlsFailure = tr("TLS certificate is missing, expired or not yet valid.");
        reply->abort();
        return;
    }

    const QString hostname = m_address->baseUrl().host();
    if (!matchesIpAddressSubjectAltName(certificate, hostname))
    {
        m_tlsFailure = tr("TLS certificate rejected: the server IP address does "
                          "not match an IP Subject Alternative Name or a legacy "
                          "Common Name on a certificate without SAN.");
        reply->abort();
        return;
    }

    const bool legacyIp = matchesLegacyIpCommonName(certificate, hostname);
    if (!isUntrustedIssuerOnly(errors, legacyIp))
    {
        m_tlsFailure = tr("TLS certificate rejected: hostname mismatch, expiry, "
                          "or another verification error that cannot be overridden.");
        reply->abort();
        return;
    }

    QStringList descriptions;
    for (const QSslError &error : errors)
        descriptions.append(error.errorString());
    if (legacyIp)
        descriptions.append(tr("Legacy certificate without Subject Alternative Names; "
                               "the server IP matches its Common Name. "
                               "Regenerating with an IP SAN is recommended."));

    if (!approveCertificate(certificate, descriptions))
    {
        m_tlsFailure = tr("Certificate trust declined or the certificate changed.");
        reply->abort();
        return;
    }

    // Never call ignoreSslErrors() without the inspected, whitelisted list.
    reply->ignoreSslErrors(errors);
}

void RemoteSession::handleEncrypted(QNetworkReply *reply)
{
    if (m_pendingReply != reply || !m_address)
        return;

    const QSslCertificate certificate = reply->sslConfiguration().peerCertificate();
    if (!withinValidityPeriod(certificate))
    {
        m_tlsFailure = tr("TLS certificate is invalid or expired.");
        reply->abort();
        return;
    }

    // Perform the endpoint identity check even for certificates that would
    // otherwise be trusted by the system or a previously stored fingerprint.
    // Legacy CN-only certificates are allowed when the exact IP matches.
    if (!matchesIpAddressSubjectAltName(certificate, m_address->baseUrl().host()))
    {
        m_tlsFailure = tr("TLS certificate rejected: the server IP address does "
                          "not match an IP Subject Alternative Name or a legacy "
                          "Common Name on a certificate without SAN.");
        reply->abort();
        return;
    }

    if (m_pinnedFingerprint.isEmpty())
        return;

    // Even a CA-valid replacement must be explicitly accepted if an old
    // fingerprint was pinned. Qt emits encrypted before sending user data.
    if (!approveCertificate(certificate, {}))
    {
        m_tlsFailure = tr("The server certificate changed and was not trusted.");
        reply->abort();
    }
}

void RemoteSession::connectToServer(const ServerProfile &profile,
                                    const QString &password)
{
    if (m_pendingReply || m_connected)
    {
        fail(tr("This connection is already active."));
        return;
    }

    m_syncTimer.stop();
    m_operations.clear();
    m_syncRid = 0;
    m_address = ServerAddress::parse(profile.url);
    if (!m_address)
    {
        fail(tr("The server URL is invalid."));
        return;
    }

    QNetworkRequest request = requestFor(QStringLiteral("api/v2/auth/login"));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/x-www-form-urlencoded"));

    QUrlQuery form;
    form.addQueryItem(QStringLiteral("username"), profile.username);
    form.addQueryItem(QStringLiteral("password"), password);
    const QByteArray body = form.toString(QUrl::FullyEncoded).toUtf8();
    // No password is saved in a profile, certificate store or session object.

    QNetworkReply *reply = m_network.post(request, body);
    armReply(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        checkReply(reply, [this](const QByteArray &response) {
            if (response.trimmed() != QByteArrayLiteral("Ok."))
            {
                fail(tr("Login failed. Check your username and password."));
                return;
            }
            requestVersion();
        });
    });
}

void RemoteSession::checkReply(
    QNetworkReply *reply,
    const std::function<void(const QByteArray &)> &onSuccess,
    const std::function<void(const QString &)> &onFailure)
{
    if (m_pendingReply != reply)
    {
        reply->deleteLater();
        return;
    }
    m_pendingReply.clear();
    const auto reject = [this, &onFailure](const QString &reason) {
        if (onFailure)
            onFailure(reason);
        else
            fail(reason);
    };

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool redirected = reply->attribute(
        QNetworkRequest::RedirectionTargetAttribute).isValid();
    const auto error = reply->error();
    const QString errorText = reply->errorString();
    // Aborted TLS replies need no body. Reading them after abort emits a
    // misleading "device not open" warning and hides the real TLS failure.
    const QByteArray response = (error == QNetworkReply::NoError)
        ? reply->readAll() : QByteArray();
    reply->deleteLater();

    if (!m_tlsFailure.isEmpty())
    {
        const QString failure = m_tlsFailure;
        m_tlsFailure.clear();
        reject(failure);
        return;
    }
    if (redirected || (status >= 300 && status < 400))
    {
        reject(tr("The server redirected the API request. Redirects are blocked."));
        return;
    }
    if (status == 403)
    {
        reject(tr("Access denied (HTTP 403). Too many failed logins may have triggered an IP ban."));
        return;
    }
    if (status == 401)
    {
        reject(tr("Authentication was rejected (HTTP 401)."));
        return;
    }
    if (error != QNetworkReply::NoError)
    {
        if (error == QNetworkReply::SslHandshakeFailedError)
            reject(tr("TLS certificate validation failed. The certificate was not trusted."));
        else
            reject(tr("Cannot reach the server: %1").arg(errorText));
        return;
    }
    if (status != 200)
    {
        reject(tr("Unexpected HTTP response: %1").arg(status));
        return;
    }
    onSuccess(response);
    if (m_connected && !m_operations.isEmpty())
        QTimer::singleShot(0, this, &RemoteSession::pumpOperations);
}

void RemoteSession::requestVersion()
{
    QNetworkReply *reply = m_network.get(requestFor(QStringLiteral("api/v2/app/version")));
    armReply(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        checkReply(reply, [this](const QByteArray &response) {
            const QString version = QString::fromUtf8(response).trimmed();
            if (version.isEmpty())
            {
                fail(tr("The server returned an empty qBittorrent version."));
                return;
            }
            requestApiVersion(version);
        });
    });
}

void RemoteSession::requestApiVersion(const QString &version)
{
    QNetworkReply *reply = m_network.get(
        requestFor(QStringLiteral("api/v2/app/webapiVersion")));
    armReply(reply);
    connect(reply, &QNetworkReply::finished, this, [this, version, reply]() {
        checkReply(reply, [this, version](const QByteArray &response) {
            const QString apiVersion = QString::fromUtf8(response).trimmed();
            if (apiVersion.isEmpty())
            {
                fail(tr("The server returned an empty Web API version."));
                return;
            }
            m_connected = true;
            emit connected(version, apiVersion);
            // The first request uses rid=0 for the full server snapshot.
            // Later polls return only modifications/deletions.
            if (m_connected)
            {
                m_syncTimer.start();
                QTimer::singleShot(0, this, &RemoteSession::requestMainData);
            }
        });
    });
}

void RemoteSession::requestMainData()
{
    if (!m_connected || m_pendingReply || !m_address)
        return;

    QNetworkRequest request = requestFor(QStringLiteral("api/v2/sync/maindata"));
    QUrl url = request.url();
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("rid"), QString::number(m_syncRid));
    url.setQuery(query);
    request.setUrl(url);

    QNetworkReply *reply = m_network.get(request);
    armReply(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        checkReply(reply, [this](const QByteArray &bytes) {
            QJsonParseError parseError;
            const QJsonDocument json = QJsonDocument::fromJson(bytes, &parseError);
            if (parseError.error != QJsonParseError::NoError || !json.isObject())
            {
                fail(tr("Invalid /sync/maindata JSON: %1").arg(parseError.errorString()));
                return;
            }
            const QJsonObject payload = json.object();
            const qint64 rid = payload.value(QStringLiteral("rid")).toInteger(-1);
            if (rid < 0
                || (payload.contains(QStringLiteral("torrents"))
                    && !payload.value(QStringLiteral("torrents")).isObject())
                || (payload.contains(QStringLiteral("torrents_removed"))
                    && !payload.value(QStringLiteral("torrents_removed")).isArray())
                || (m_syncRid == 0 && !payload.value(QStringLiteral("full_update")).toBool()))
            {
                fail(tr("Invalid or incomplete /sync/maindata response."));
                return;
            }

            m_syncRid = rid;
            emit mainDataUpdated(payload);
        });
    });
}

void RemoteSession::queueOperation(const QString &endpoint, const QByteArray &body,
                                   const QByteArray &contentType,
                                   const QString &description, bool checkAccepted,
                                   std::function<void()> onAccepted,
                                   std::function<void(const QString &)> onRejected)
{
    if (!m_connected || !m_address)
    {
        const QString reason = tr("Server is not connected.");
        emit operationFailed(description, reason);
        if (onRejected)
            onRejected(reason);
        return;
    }
    QueuedOperation operation{endpoint, body, contentType, description, checkAccepted};
    operation.onAccepted = std::move(onAccepted);
    operation.onRejected = std::move(onRejected);
    m_operations.enqueue(std::move(operation));
    pumpOperations();
}

void RemoteSession::pumpOperations()
{
    // The existing secure TLS/checkReply path tracks one outstanding reply.
    // Serialize all operations with /sync/maindata so no request can overwrite
    // another reply's TLS verification context or session error state.
    if (!m_connected || m_pendingReply || m_operations.isEmpty())
        return;

    const QueuedOperation operation = m_operations.dequeue();
    QNetworkRequest request = requestFor(operation.endpoint);
    if (!operation.detailsRequest)
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QString::fromLatin1(operation.contentType));
    QNetworkReply *reply = operation.detailsRequest
        ? m_network.get(request) : m_network.post(request, operation.body);
    armReply(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, operation]() {
        checkReply(reply, [this, operation](const QByteArray &response) {
            if (operation.detailsRequest)
            {
                QJsonParseError error;
                const QJsonDocument json = QJsonDocument::fromJson(response, &error);
                const bool expectsObject = (operation.detailTab == 0 || operation.detailTab == 2);
                if (error.error != QJsonParseError::NoError
                    || (expectsObject && !json.isObject())
                    || (!expectsObject && !json.isArray()))
                {
                    emit torrentDetailsFailed(operation.detailHash, operation.detailTab,
                                              tr("Unexpected torrent-details response."));
                    return;
                }
                emit torrentDetailsReceived(operation.detailHash, operation.detailTab,
                                             expectsObject ? QJsonValue(json.object())
                                                           : QJsonValue(json.array()));
            }
            else if (operation.checkAccepted && response.trimmed() == QByteArrayLiteral("Fails."))
            {
                const QString reason = tr("The server rejected the request.");
                emit operationFailed(operation.description, reason);
                if (operation.onRejected)
                    operation.onRejected(reason);
            }
            else
            {
                emit operationSucceeded(operation.description);
                if (operation.onAccepted)
                    operation.onAccepted();
            }
        }, [this, operation](const QString &reason) {
            if (operation.detailsRequest)
                emit torrentDetailsFailed(operation.detailHash, operation.detailTab, reason);
            else
            {
                emit operationFailed(operation.description, reason);
                if (operation.onRejected)
                    operation.onRejected(reason);
            }
        });
        // Continue the queue even when an individual command was rejected.
        if (m_connected && !m_pendingReply)
            QTimer::singleShot(0, this, &RemoteSession::pumpOperations);
    });
}

void RemoteSession::requestTorrentDetails(const QString &hash, int tab)
{
    static const QRegularExpression validHash(
        QStringLiteral("^(?:[a-fA-F0-9]{40}|[a-fA-F0-9]{64})$"));
    if (!m_connected || !m_address || !validHash.match(hash).hasMatch()
        || tab < 0 || tab > 4)
        return;

    static const QStringList endpoints {
        QStringLiteral("torrents/properties"),
        QStringLiteral("torrents/trackers"),
        QStringLiteral("sync/torrentPeers"),
        QStringLiteral("torrents/webseeds"),
        QStringLiteral("torrents/files")
    };
    // Discard queued requests for old selections or inactive pages. Already
    // running replies are still verified and discarded by the receiving pane.
    for (int i = m_operations.size() - 1; i >= 0; --i)
        if (m_operations.at(i).detailsRequest)
            m_operations.removeAt(i);

    const QString url = QStringLiteral("api/v2/%1?hash=%2%3")
        .arg(endpoints.at(tab), QString::fromLatin1(QUrl::toPercentEncoding(hash)),
             tab == 2 ? QStringLiteral("&rid=0") : QString());
    m_operations.enqueue({url, {}, {}, {}, false, true, hash, tab});
    pumpOperations();
}

void RemoteSession::setFilePriority(const QString &hash, const QList<int> &fileIds,
                                    int priority)
{
    static const QRegularExpression validHash(
        QStringLiteral("^(?:[a-fA-F0-9]{40}|[a-fA-F0-9]{64})$"));
    if (!validHash.match(hash).hasMatch() || fileIds.isEmpty()
        || (priority != 0 && priority != 1 && priority != 6 && priority != 7))
    {
        emit operationFailed(tr("Set file priorities"), tr("Invalid hash, file IDs or priority."));
        return;
    }
    QStringList ids;
    for (int id : fileIds)
    {
        if (id < 0)
        {
            emit operationFailed(tr("Set file priorities"), tr("Invalid file index."));
            return;
        }
        ids.append(QString::number(id));
    }
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("hash"), hash);
    form.addQueryItem(QStringLiteral("id"), ids.join(QLatin1Char('|')));
    form.addQueryItem(QStringLiteral("priority"), QString::number(priority));
    queueOperation(QStringLiteral("api/v2/torrents/filePrio"),
                   form.toString(QUrl::FullyEncoded).toUtf8(),
                   QByteArrayLiteral("application/x-www-form-urlencoded"),
                   tr("Set file priorities"), true);
}

void RemoteSession::commandTorrents(TorrentCommand command,
                                    const QStringList &hashes, bool deleteFiles)
{
    static const QRegularExpression validHash(QStringLiteral("^(?:[a-fA-F0-9]{40}|[a-fA-F0-9]{64})$"));
    if (hashes.isEmpty())
    {
        emit operationFailed(tr("Torrent action"), tr("No torrents selected."));
        return;
    }
    for (const QString &hash : hashes)
    {
        if (!validHash.match(hash).hasMatch())
        {
            emit operationFailed(tr("Torrent action"), tr("Invalid torrent hash."));
            return;
        }
    }

    QString endpoint;
    QString description;
    switch (command)
    {
    case TorrentCommand::Start:
        endpoint = QStringLiteral("start"); description = tr("Start torrents"); break;
    case TorrentCommand::Stop:
        endpoint = QStringLiteral("stop"); description = tr("Stop torrents"); break;
    case TorrentCommand::ForceStart:
        endpoint = QStringLiteral("setForceStart"); description = tr("Force start torrents"); break;
    case TorrentCommand::ForceRecheck:
        endpoint = QStringLiteral("recheck"); description = tr("Force recheck torrents"); break;
    case TorrentCommand::ForceReannounce:
        endpoint = QStringLiteral("reannounce"); description = tr("Force reannounce torrents"); break;
    case TorrentCommand::Delete:
        endpoint = QStringLiteral("delete"); description = tr("Remove torrents"); break;
    case TorrentCommand::IncreasePriority:
        endpoint = QStringLiteral("increasePrio"); description = tr("Increase queue priority"); break;
    case TorrentCommand::DecreasePriority:
        endpoint = QStringLiteral("decreasePrio"); description = tr("Decrease queue priority"); break;
    case TorrentCommand::TopPriority:
        endpoint = QStringLiteral("topPrio"); description = tr("Move to top of queue"); break;
    case TorrentCommand::BottomPriority:
        endpoint = QStringLiteral("bottomPrio"); description = tr("Move to bottom of queue"); break;
    }

    QUrlQuery form;
    form.addQueryItem(QStringLiteral("hashes"), hashes.join(QLatin1Char('|')));
    // Web API setForceStart requires value=true in addition to the hashes.
    if (command == TorrentCommand::ForceStart)
        form.addQueryItem(QStringLiteral("value"), QStringLiteral("true"));
    if (command == TorrentCommand::Delete)
        form.addQueryItem(QStringLiteral("deleteFiles"),
                          deleteFiles ? QStringLiteral("true") : QStringLiteral("false"));
    queueOperation(QStringLiteral("api/v2/torrents/") + endpoint,
                   form.toString(QUrl::FullyEncoded).toUtf8(),
                   QByteArrayLiteral("application/x-www-form-urlencoded"), description);
}

void RemoteSession::setTorrentLocation(const QStringList &hashes, const QString &remotePath)
{
    static const QRegularExpression validHash(
        QStringLiteral("^(?:[a-fA-F0-9]{40}|[a-fA-F0-9]{64})$"));
    if (hashes.isEmpty() || std::any_of(hashes.cbegin(), hashes.cend(),
            [&validHash](const QString &hash) { return !validHash.match(hash).hasMatch(); })
        || remotePath.isEmpty() || remotePath == QLatin1String("/")
        || !remotePath.startsWith(QLatin1Char('/'))
        || remotePath.contains(QLatin1Char('\\'))
        || remotePath.contains(QLatin1Char('\n'))
        || remotePath.contains(QLatin1Char('\r')))
    {
        emit operationFailed(tr("Set torrent location"),
            tr("Select valid torrents and a non-root absolute remote POSIX folder."));
        return;
    }
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("hashes"), hashes.join(QLatin1Char('|')));
    form.addQueryItem(QStringLiteral("location"), remotePath);
    queueOperation(QStringLiteral("api/v2/torrents/setLocation"),
                   form.toString(QUrl::FullyEncoded).toUtf8(),
                   QByteArrayLiteral("application/x-www-form-urlencoded"),
                   tr("Set torrent location"));
}


namespace
{
bool validSelectedTorrentHashes(const QStringList &hashes)
{
    static const QRegularExpression validHash(
        QStringLiteral("^(?:[a-fA-F0-9]{40}|[a-fA-F0-9]{64})$"));
    return !hashes.isEmpty() && std::all_of(hashes.cbegin(), hashes.cend(),
        [](const QString &hash) { return validHash.match(hash).hasMatch(); });
}
bool validSelectedTags(const QStringList &tags)
{
    return !tags.isEmpty() && tags.size() <= 128
        && std::all_of(tags.cbegin(), tags.cend(), [](const QString &tag) {
            return !tag.trimmed().isEmpty() && tag == tag.trimmed()
                && !tag.contains(QLatin1Char(','))
                && !tag.contains(QLatin1Char('\n'))
                && !tag.contains(QLatin1Char('\r'));
        });
}
}

void RemoteSession::setTorrentCategory(const QStringList &hashes, const QString &category)
{
    if (!validSelectedTorrentHashes(hashes) || category.contains(QLatin1Char('\n'))
        || category.contains(QLatin1Char('\r')))
    {
        emit operationFailed(tr("Set torrent category"), tr("Invalid torrent selection or category."));
        return;
    }
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("hashes"), hashes.join(QLatin1Char('|')));
    form.addQueryItem(QStringLiteral("category"), category);
    queueOperation(QStringLiteral("api/v2/torrents/setCategory"),
                   form.toString(QUrl::FullyEncoded).toUtf8(),
                   QByteArrayLiteral("application/x-www-form-urlencoded"),
                   tr("Set torrent category"));
}

void RemoteSession::createAndAssignTorrentCategory(const QStringList &hashes,
                                                  const QString &category)
{
    if (!validSelectedTorrentHashes(hashes) || category.trimmed().isEmpty()
        || category != category.trimmed() || category.contains(QLatin1Char('\n'))
        || category.contains(QLatin1Char('\r')))
    {
        emit operationFailed(tr("Create torrent category"), tr("Invalid selection or category name."));
        return;
    }
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("category"), category);
    // Empty means the server applies its configured default save location.
    form.addQueryItem(QStringLiteral("savePath"), QString());
    queueOperation(QStringLiteral("api/v2/torrents/createCategory"),
                   form.toString(QUrl::FullyEncoded).toUtf8(),
                   QByteArrayLiteral("application/x-www-form-urlencoded"),
                   tr("Create torrent category"), true,
                   [this, hashes, category]() { setTorrentCategory(hashes, category); });
}

void RemoteSession::changeTorrentTags(const QStringList &hashes,
                                      const QStringList &tags, bool add)
{
    // Empty tags is intentional only when clearing all memberships.
    if (!validSelectedTorrentHashes(hashes) || (!tags.isEmpty() && !validSelectedTags(tags))
        || (add && tags.isEmpty()))
    {
        emit operationFailed(tr("Change torrent tags"), tr("Invalid selection or tag names."));
        return;
    }
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("hashes"), hashes.join(QLatin1Char('|')));
    form.addQueryItem(QStringLiteral("tags"), tags.join(QLatin1Char(',')));
    queueOperation(QStringLiteral("api/v2/torrents/")
                   + (add ? QStringLiteral("addTags") : QStringLiteral("removeTags")),
                   form.toString(QUrl::FullyEncoded).toUtf8(),
                   QByteArrayLiteral("application/x-www-form-urlencoded"),
                   add ? tr("Add torrent tags") : tr("Remove torrent tags"));
}

void RemoteSession::createAndAddTorrentTags(const QStringList &hashes,
                                             const QStringList &tags)
{
    if (!validSelectedTorrentHashes(hashes) || !validSelectedTags(tags))
    {
        emit operationFailed(tr("Create torrent tags"), tr("Invalid selection or tag names."));
        return;
    }
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("tags"), tags.join(QLatin1Char(',')));
    queueOperation(QStringLiteral("api/v2/torrents/createTags"),
                   form.toString(QUrl::FullyEncoded).toUtf8(),
                   QByteArrayLiteral("application/x-www-form-urlencoded"),
                   tr("Create torrent tags"), true,
                   [this, hashes, tags]() { changeTorrentTags(hashes, tags, true); });
}

void RemoteSession::setTorrentAutoManagement(const QStringList &hashes, bool enabled)
{
    if (!validSelectedTorrentHashes(hashes))
    {
        emit operationFailed(tr("Automatic Torrent Management"),
                             tr("Invalid torrent selection."));
        return;
    }
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("hashes"), hashes.join(QLatin1Char('|')));
    form.addQueryItem(QStringLiteral("enable"),
                      enabled ? QStringLiteral("true") : QStringLiteral("false"));
    queueOperation(QStringLiteral("api/v2/torrents/setAutoManagement"),
                   form.toString(QUrl::FullyEncoded).toUtf8(),
                   QByteArrayLiteral("application/x-www-form-urlencoded"),
                   tr("Automatic Torrent Management"));
}

void RemoteSession::renameTorrentContent(const QString &hash, const QString &oldPath,
                                         const QString &newPath)
{
    static const QRegularExpression validHash(
        QStringLiteral("^(?:[a-fA-F0-9]{40}|[a-fA-F0-9]{64})$"));
    const auto validRelative = [](const QString &path) {
        if (path.isEmpty() || path.startsWith(QLatin1Char('/'))
            || path.contains(QLatin1Char('\\'))
            || path.contains(QLatin1Char('\r'))
            || path.contains(QLatin1Char('\n')))
            return false;
        const QStringList parts = path.split(QLatin1Char('/'));
        return std::all_of(parts.cbegin(), parts.cend(), [](const QString &part) {
            return !part.isEmpty() && part != QLatin1String(".")
                && part != QLatin1String("..");
        });
    };
    if (!validHash.match(hash).hasMatch() || !validRelative(oldPath)
        || !validRelative(newPath))
    {
        emit operationFailed(tr("Rename torrent file"), tr("Invalid torrent or file path."));
        emit torrentContentRenameFinished(hash, oldPath, false, tr("Invalid file path."));
        return;
    }
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("hash"), hash);
    form.addQueryItem(QStringLiteral("oldPath"), oldPath);
    form.addQueryItem(QStringLiteral("newPath"), newPath);
    queueOperation(QStringLiteral("api/v2/torrents/renameFile"),
        form.toString(QUrl::FullyEncoded).toUtf8(),
        QByteArrayLiteral("application/x-www-form-urlencoded"),
        tr("Rename torrent file"), true,
        [this, hash, oldPath]() {
            emit torrentContentRenameFinished(hash, oldPath, true, {});
        },
        [this, hash, oldPath](const QString &reason) {
            emit torrentContentRenameFinished(hash, oldPath, false, reason);
        });
}

void RemoteSession::setSuperSeeding(const QStringList &hashes, bool enabled)
{
    if (!validSelectedTorrentHashes(hashes))
    {
        emit operationFailed(tr("Super seeding mode"), tr("Invalid torrent selection."));
        return;
    }
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("hashes"), hashes.join(QLatin1Char('|')));
    form.addQueryItem(QStringLiteral("value"), enabled ? QStringLiteral("true") : QStringLiteral("false"));
    queueOperation(QStringLiteral("api/v2/torrents/setSuperSeeding"),
        form.toString(QUrl::FullyEncoded).toUtf8(),
        QByteArrayLiteral("application/x-www-form-urlencoded"), tr("Super seeding mode"), true);
}

void RemoteSession::toggleTorrentMode(const QStringList &hashes, bool sequential)
{
    if (!validSelectedTorrentHashes(hashes))
    {
        emit operationFailed(tr("Torrent download mode"), tr("Invalid torrent selection."));
        return;
    }
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("hashes"), hashes.join(QLatin1Char('|')));
    queueOperation(QStringLiteral("api/v2/torrents/")
            + (sequential ? QStringLiteral("toggleSequentialDownload")
                          : QStringLiteral("toggleFirstLastPiecePrio")),
        form.toString(QUrl::FullyEncoded).toUtf8(),
        QByteArrayLiteral("application/x-www-form-urlencoded"),
        sequential ? tr("Download in sequential order") : tr("Download first and last pieces first"), true);
}

void RemoteSession::renameTorrent(const QString &hash, const QString &name)
{
    static const QRegularExpression validHash(
        QStringLiteral("^(?:[a-fA-F0-9]{40}|[a-fA-F0-9]{64})$"));
    const QString normalizedName = QString(name).replace(
        QRegularExpression(QStringLiteral("\\r?\\n|\\r")), QStringLiteral(" "));
    if (!validHash.match(hash).hasMatch() || normalizedName.trimmed().isEmpty())
    {
        emit operationFailed(tr("Rename torrent"), tr("A valid torrent hash and nonempty name are required."));
        return;
    }

    QUrlQuery form;
    form.addQueryItem(QStringLiteral("hash"), hash);
    form.addQueryItem(QStringLiteral("name"), normalizedName);
    queueOperation(QStringLiteral("api/v2/torrents/rename"),
                   form.toString(QUrl::FullyEncoded).toUtf8(),
                   QByteArrayLiteral("application/x-www-form-urlencoded"),
                   tr("Rename torrent"));
}

namespace
{
void appendAddOptions(QUrlQuery &form, const AddTorrentOptions &options)
{
    // Explicit remote paths only. Empty means let the server choose.
    form.addQueryItem(QStringLiteral("autoTMM"), options.autoTMM
        ? QStringLiteral("true") : QStringLiteral("false"));
    if (!options.savePath.isEmpty() && !options.autoTMM)
        form.addQueryItem(QStringLiteral("savepath"), options.savePath);
    if (options.useDownloadPath && !options.downloadPath.isEmpty() && !options.autoTMM)
    {
        form.addQueryItem(QStringLiteral("useDownloadPath"), QStringLiteral("true"));
        form.addQueryItem(QStringLiteral("downloadPath"), options.downloadPath);
    }
    if (!options.category.isEmpty())
        form.addQueryItem(QStringLiteral("category"), options.category);
    if (!options.tags.isEmpty())
        form.addQueryItem(QStringLiteral("tags"), options.tags);
    form.addQueryItem(QStringLiteral("stopped"), options.stopped
        ? QStringLiteral("true") : QStringLiteral("false"));
    if (options.sequential)
        form.addQueryItem(QStringLiteral("sequentialDownload"), QStringLiteral("true"));
    if (options.firstLastPiece)
        form.addQueryItem(QStringLiteral("firstLastPiecePrio"), QStringLiteral("true"));
    if (options.queueTop)
        form.addQueryItem(QStringLiteral("addToTopOfQueue"), QStringLiteral("true"));
    if (options.seedMode)
        form.addQueryItem(QStringLiteral("seedMode"), QStringLiteral("true"));
    if (options.contentLayout == QLatin1String("Original")
        || options.contentLayout == QLatin1String("Subfolder")
        || options.contentLayout == QLatin1String("NoSubfolder"))
        form.addQueryItem(QStringLiteral("contentLayout"), options.contentLayout);
}

void appendMultipartField(QByteArray &body, const QByteArray &boundary,
                          const QByteArray &field, const QString &value)
{
    if (value.isEmpty())
        return;
    body += QByteArrayLiteral("--") + boundary
        + QByteArrayLiteral("\r\nContent-Disposition: form-data; name=\"")
        + field + QByteArrayLiteral("\"\r\n\r\n")
        + value.toUtf8() + QByteArrayLiteral("\r\n");
}
} // namespace

void RemoteSession::addTorrentUrls(const QString &urls)
{
    addTorrentUrls(urls, {});
}

void RemoteSession::addTorrentUrls(const QString &urls, const AddTorrentOptions &options)
{
    const QStringList lines = urls.split(QLatin1Char('\n'));
    QStringList checked;
    for (QString line : lines)
    {
        line = line.trimmed();
        if (line.isEmpty())
            continue;
        const QUrl url(line, QUrl::StrictMode);
        const QString scheme = url.scheme().toLower();
        if (!url.isValid()
            || (scheme != QLatin1String("magnet") && scheme != QLatin1String("http")
                && scheme != QLatin1String("https"))
            || ((scheme == QLatin1String("http") || scheme == QLatin1String("https"))
                && url.host().isEmpty()))
        {
            emit operationFailed(tr("Add torrent links"), tr("Use valid magnet, HTTP or HTTPS URLs."));
            return;
        }
        checked.append(line);
    }
    if (checked.isEmpty())
    {
        emit operationFailed(tr("Add torrent links"), tr("No torrent URLs supplied."));
        return;
    }
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("urls"), checked.join(QLatin1Char('\n')));
    appendAddOptions(form, options);
    queueOperation(QStringLiteral("api/v2/torrents/add"),
                   form.toString(QUrl::FullyEncoded).toUtf8(),
                   QByteArrayLiteral("application/x-www-form-urlencoded"),
                   tr("Add torrent links"), true);
}

void RemoteSession::addTorrentTrackers(const QString &hash, const QStringList &urls)
{
    static const QRegularExpression validHash(
        QStringLiteral("^(?:[a-fA-F0-9]{40}|[a-fA-F0-9]{64})$"));
    if (!validHash.match(hash).hasMatch() || urls.isEmpty() || urls.size() > 256)
    {
        emit operationFailed(tr("Merge torrent trackers"), tr("Invalid torrent hash or tracker list."));
        return;
    }
    QStringList validated;
    for (const QString &raw : urls)
    {
        const QUrl url(raw, QUrl::StrictMode);
        const QString scheme = url.scheme().toLower();
        if (!url.isValid() || url.host().isEmpty()
            || (scheme != QLatin1String("https") && scheme != QLatin1String("http")
                && scheme != QLatin1String("udp"))
            || raw.contains(QLatin1Char('\n')) || raw.contains(QLatin1Char('\r')))
        {
            emit operationFailed(tr("Merge torrent trackers"), tr("Invalid tracker URL."));
            return;
        }
        if (!validated.contains(raw))
            validated.append(raw);
    }
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("hash"), hash);
    form.addQueryItem(QStringLiteral("urls"), validated.join(QLatin1Char('\n')));
    queueOperation(QStringLiteral("api/v2/torrents/addTrackers"),
                   form.toString(QUrl::FullyEncoded).toUtf8(),
                   QByteArrayLiteral("application/x-www-form-urlencoded"),
                   tr("Merge torrent trackers"), true,
                   [this, hash]() { emit torrentTrackersChanged(hash); });
}



namespace
{
bool validEditableTrackerUrl(const QString &raw)
{
    const QUrl url(raw, QUrl::StrictMode);
    const QString scheme = url.scheme().toLower();
    return url.isValid() && !url.host().isEmpty()
        && (scheme == QLatin1String("https")
            || scheme == QLatin1String("http")
            || scheme == QLatin1String("udp"))
        && !raw.contains(QLatin1Char('|'))
        && !raw.contains(QLatin1Char('\n'))
        && !raw.contains(QLatin1Char('\r'));
}
}

void RemoteSession::editTorrentTracker(const QString &hash, const QString &oldUrl,
                                      const QString &newUrl)
{
    static const QRegularExpression validHash(
        QStringLiteral("^(?:[a-fA-F0-9]{40}|[a-fA-F0-9]{64})$"));
    if (!validHash.match(hash).hasMatch()
        || !validEditableTrackerUrl(oldUrl)
        || !validEditableTrackerUrl(newUrl)
        || oldUrl == newUrl)
    {
        emit operationFailed(tr("Edit torrent tracker"), tr("Invalid hash or tracker URL."));
        return;
    }
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("hash"), hash);
    form.addQueryItem(QStringLiteral("url"), oldUrl);
    form.addQueryItem(QStringLiteral("newUrl"), newUrl);
    queueOperation(QStringLiteral("api/v2/torrents/editTracker"),
                   form.toString(QUrl::FullyEncoded).toUtf8(),
                   QByteArrayLiteral("application/x-www-form-urlencoded"),
                   tr("Edit torrent tracker"), true,
                   [this, hash, oldUrl]() {
                       emit torrentTrackersChanged(hash);
                       emit torrentTrackerEditFinished(hash, oldUrl, true, {});
                   },
                   [this, hash, oldUrl](const QString &reason) {
                       emit torrentTrackerEditFinished(hash, oldUrl, false, reason);
                   });
}

void RemoteSession::removeTorrentTrackers(const QString &hash, const QStringList &urls)
{
    static const QRegularExpression validHash(
        QStringLiteral("^(?:[a-fA-F0-9]{40}|[a-fA-F0-9]{64})$"));
    if (!validHash.match(hash).hasMatch() || urls.isEmpty() || urls.size() > 128)
    {
        emit operationFailed(tr("Remove torrent trackers"), tr("Invalid torrent or tracker list."));
        return;
    }
    QStringList encoded;
    for (const QString &url : urls)
    {
        if (!validEditableTrackerUrl(url))
        {
            emit operationFailed(tr("Remove torrent trackers"), tr("Invalid tracker URL."));
            return;
        }
        // removeTrackers decodes each URL after it has parsed the form data.
        // Preserve tracker query parameters/passkeys across both decodings.
        const QString item = QString::fromLatin1(QUrl::toPercentEncoding(url));
        if (!encoded.contains(item))
            encoded.append(item);
    }
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("hash"), hash);
    form.addQueryItem(QStringLiteral("urls"), encoded.join(QLatin1Char('|')));
    queueOperation(QStringLiteral("api/v2/torrents/removeTrackers"),
                   form.toString(QUrl::FullyEncoded).toUtf8(),
                   QByteArrayLiteral("application/x-www-form-urlencoded"),
                   tr("Remove torrent trackers"), true,
                   [this, hash]() { emit torrentTrackersChanged(hash); });
}


void RemoteSession::replaceTorrentTrackers(const QString &hash,
                                           const QString &previousEntries,
                                           const QString &replacementEntries,
                                           bool privateOrUnknown)
{
    static const QRegularExpression validHash(
        QStringLiteral("^(?:[a-fA-F0-9]{40}|[a-fA-F0-9]{64})$"));
    const auto trackerUrls = [](const QString &text, QStringList &urls) -> bool {
        urls.clear();
        for (QString line : text.split(QLatin1Char('\n')))
        {
            line = line.trimmed();
            if (line.isEmpty())
                continue;
            const QUrl url(line, QUrl::StrictMode);
            const QString scheme = url.scheme().toLower();
            if (!url.isValid() || url.host().isEmpty()
                || (scheme != QLatin1String("http")
                    && scheme != QLatin1String("https")
                    && scheme != QLatin1String("udp"))
                || line.contains(QLatin1Char('|'))
                || line.contains(QLatin1Char('\r'))
                || line.contains(QChar(0)))
                return false;
            if (!urls.contains(line))
                urls.append(line);
        }
        return true;
    };

    QStringList existing, replacement;
    if (!validHash.match(hash).hasMatch()
        || !trackerUrls(previousEntries, existing)
        || !trackerUrls(replacementEntries, replacement))
    {
        emit operationFailed(tr("Replace torrent trackers"),
                             tr("Invalid torrent hash or tracker URL. No trackers were modified."));
        return;
    }

    // Defense in depth: never permit the remote API to expand a private
    // torrent's announce URL count. A trackerless torrent may recover with
    // ONE new URL; swapping or removing existing URLs is permitted.
    if (privateOrUnknown && replacement.size() > existing.size()
        && !(existing.isEmpty() && replacement.size() == 1))
    {
        emit operationFailed(tr("Replace torrent trackers"),
                             tr("Adding additional trackers to private torrents is blocked. "
                                "Use the server's Web UI if necessary."));
        return;
    }

    if (previousEntries == replacementEntries)
    {
        emit torrentTrackersChanged(hash);
        return;
    }

    // The server parses blank lines as tier boundaries. Preserve the exact
    // grouped text rather than flattening it into a single tier.
    const QString newText = replacementEntries;
    const auto sendReplacement = [this, hash, previousEntries, newText]() {
        if (newText.trimmed().isEmpty())
        {
            emit torrentTrackersChanged(hash);
            return;
        }
        const QByteArray form = QByteArrayLiteral("hash=")
            + QUrl::toPercentEncoding(hash)
            + QByteArrayLiteral("&urls=")
            + QUrl::toPercentEncoding(newText);
        queueOperation(QStringLiteral("api/v2/torrents/addTrackers"),
                       form,
                       QByteArrayLiteral("application/x-www-form-urlencoded"),
                       tr("Replace torrent trackers"), true,
                       [this, hash]() { emit torrentTrackersChanged(hash); },
                       [this, hash, previousEntries](const QString &) {
                           // If adding the replacement fails after removal,
                           // make a best-effort restoration of the original
                           // URLs and tiers. The ordinary operation failure
                           // remains visible even if restoration succeeds.
                           if (previousEntries.trimmed().isEmpty())
                               return;
                           const QByteArray restore = QByteArrayLiteral("hash=")
                               + QUrl::toPercentEncoding(hash)
                               + QByteArrayLiteral("&urls=")
                               + QUrl::toPercentEncoding(previousEntries);
                           queueOperation(QStringLiteral("api/v2/torrents/addTrackers"),
                               restore,
                               QByteArrayLiteral("application/x-www-form-urlencoded"),
                               tr("Restore original torrent trackers"), true,
                               [this, hash]() { emit torrentTrackersChanged(hash); });
                       });
    };

    // Keep remote operations serialized and run the full-list update only
    // after an optional native private-tracker edit has succeeded.
    const auto removeThenAdd = [this, hash, sendReplacement](
        const QStringList &current, std::function<void()> undoEdit = {}) {
        if (current.isEmpty())
        {
            sendReplacement();
            return;
        }

        QStringList encoded;
        for (const QString &url : current)
            encoded.append(QString::fromLatin1(QUrl::toPercentEncoding(url)));
        QUrlQuery remove;
        remove.addQueryItem(QStringLiteral("hash"), hash);
        remove.addQueryItem(QStringLiteral("urls"), encoded.join(QLatin1Char('|')));
        queueOperation(QStringLiteral("api/v2/torrents/removeTrackers"),
            remove.toString(QUrl::FullyEncoded).toUtf8(),
            QByteArrayLiteral("application/x-www-form-urlencoded"),
            tr("Replace torrent trackers"), true, sendReplacement,
            [undoEdit](const QString &) {
                if (undoEdit)
                    undoEdit();
            });
    };

    // On private torrents, qBittorrent's own editTracker Web API calls
    // TorrentImpl::replaceTrackers(), which clears old peer connections.
    // A genuine old -> new URL substitution can use that path before
    // the full replacement. Never create a fake edit just to clear peers.
    if (privateOrUnknown)
    {
        const QSet<QString> oldUrls(existing.cbegin(), existing.cend());
        const QSet<QString> newUrls(replacement.cbegin(), replacement.cend());
        QString removed, added;
        for (const QString &url : existing)
            if (!newUrls.contains(url))
            {
                removed = url;
                break;
            }
        for (const QString &url : replacement)
            if (!oldUrls.contains(url))
            {
                added = url;
                break;
            }
        if (!removed.isEmpty() && !added.isEmpty())
        {
            QUrlQuery edit;
            edit.addQueryItem(QStringLiteral("hash"), hash);
            edit.addQueryItem(QStringLiteral("url"), removed);
            edit.addQueryItem(QStringLiteral("newUrl"), added);
            queueOperation(QStringLiteral("api/v2/torrents/editTracker"),
                edit.toString(QUrl::FullyEncoded).toUtf8(),
                QByteArrayLiteral("application/x-www-form-urlencoded"),
                tr("Replace private torrent tracker"), true,
                [this, hash, existing, removed, added, removeThenAdd]() {
                    QStringList currentlyOnServer = existing;
                    currentlyOnServer.replace(currentlyOnServer.indexOf(removed), added);
                    const auto undoEdit = [this, hash, removed, added]() {
                        QUrlQuery undo;
                        undo.addQueryItem(QStringLiteral("hash"), hash);
                        undo.addQueryItem(QStringLiteral("url"), added);
                        undo.addQueryItem(QStringLiteral("newUrl"), removed);
                        queueOperation(QStringLiteral("api/v2/torrents/editTracker"),
                            undo.toString(QUrl::FullyEncoded).toUtf8(),
                            QByteArrayLiteral("application/x-www-form-urlencoded"),
                            tr("Restore original private tracker"), true);
                    };
                    removeThenAdd(currentlyOnServer, undoEdit);
                });
            return;
        }
    }

    removeThenAdd(existing);

}

void RemoteSession::addTorrentFile(const QString &filename, const QByteArray &contents)
{
    addTorrentFile(filename, contents, {});
}

void RemoteSession::addTorrentFile(const QString &filename, const QByteArray &contents,
                                   const AddTorrentOptions &options)
{
    addTorrentFiles({{filename, contents}}, options);
}

void RemoteSession::addTorrentFiles(const QList<TorrentUpload> &files,
                                    const AddTorrentOptions &options)
{
    const QString description = files.size() == 1
        ? tr("Add torrent file") : tr("Add %1 torrent files").arg(files.size());
    // Bound the total request before allocating the multipart body. Metadata
    // uploads only; never upload downloaded content or local mount paths.
    constexpr qint64 maxSingleBytes = 32LL * 1024 * 1024;
    constexpr qint64 maxBatchBytes = 128LL * 1024 * 1024;
    if (files.isEmpty() || files.size() > 256)
    {
        emit operationFailed(description, tr("Select between 1 and 256 torrent files."));
        return;
    }
    qint64 total = 0;
    for (const TorrentUpload &file : files)
    {
        if (file.contents.isEmpty() || file.contents.size() > maxSingleBytes
            || total > maxBatchBytes - file.contents.size())
        {
            emit operationFailed(description,
                tr("Torrent metadata must be nonempty, at most 32 MiB each and 128 MiB total."));
            return; // No partially submitted batches.
        }
        total += file.contents.size();
    }

    const QByteArray boundary = QByteArrayLiteral("qbtRemote-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces).toLatin1();
    QByteArray body;
    body.reserve(static_cast<qsizetype>(total + 8192 + files.size() * 384));

    QUrlQuery fields;
    appendAddOptions(fields, options);
    for (const auto &field : fields.queryItems(QUrl::FullyDecoded))
        appendMultipartField(body, boundary, field.first.toLatin1(), field.second);

    QSet<QString> usedNames;
    for (const TorrentUpload &file : files)
    {
        // An upstream parser may key uploads by filename. Avoid collisions
        // when users select same-basename files from different directories.
        QString cleanName = QFileInfo(file.filename).fileName();
        cleanName.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")),
                          QStringLiteral("_"));
        if (cleanName.isEmpty())
            cleanName = QStringLiteral("upload.torrent");
        const QString original = cleanName;
        int suffix = 2;
        while (usedNames.contains(cleanName))
        {
            const int dot = original.lastIndexOf(QLatin1Char('.'));
            cleanName = dot > 0
                ? original.left(dot) + QLatin1Char('_') + QString::number(suffix++)
                    + original.mid(dot)
                : original + QLatin1Char('_') + QString::number(suffix++);
        }
        usedNames.insert(cleanName);

        body += QByteArrayLiteral("--") + boundary + QByteArrayLiteral("\r\n");
        body += QByteArrayLiteral(
            "Content-Disposition: form-data; name=\"torrents\"; filename=\"")
            + cleanName.toUtf8() + QByteArrayLiteral("\"\r\n");
        body += QByteArrayLiteral("Content-Type: application/x-bittorrent\r\n\r\n");
        body += file.contents;
        body += QByteArrayLiteral("\r\n");
    }
    body += QByteArrayLiteral("--") + boundary + QByteArrayLiteral("--\r\n");

    // A single authenticated multipart POST passes shared remote add options
    // and all selected .torrent files to qBittorrent's real Web API.
    queueOperation(QStringLiteral("api/v2/torrents/add"), body,
                   QByteArrayLiteral("multipart/form-data; boundary=") + boundary,
                   description, true);
}

bool RemoteSession::isConnected() const noexcept
{
    return m_connected;
}

void RemoteSession::disconnectFromServer()
{
    m_syncTimer.stop();
    m_operations.clear();
    if (m_pendingReply)
    {
        auto *reply = m_pendingReply.data();
        m_pendingReply.clear();
        reply->abort();
        reply->deleteLater();
    }

    if (!m_connected || !m_address)
    {
        deleteLater();
        return;
    }
    m_connected = false;
    QNetworkReply *reply = m_network.post(
        requestFor(QStringLiteral("api/v2/auth/logout")), QByteArray());
    armReply(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        m_pendingReply.clear();
        reply->deleteLater();
        deleteLater();
    });
}

void RemoteSession::fail(const QString &reason)
{
    m_syncTimer.stop();
    m_operations.clear();
    m_connected = false;
    emit failed(reason);
}

} // namespace Remote
