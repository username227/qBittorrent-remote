// SPDX-License-Identifier: GPL-3.0-or-later
#include "api/RemoteSession.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QHostAddress>
#include <QNetworkCookieJar>
#include <QSslConfiguration>
#include <QUrlQuery>

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
    const std::function<void(const QByteArray &)> &onSuccess)
{
    if (m_pendingReply != reply)
    {
        reply->deleteLater();
        return;
    }
    m_pendingReply.clear();

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
        fail(failure);
        return;
    }
    if (redirected || (status >= 300 && status < 400))
    {
        fail(tr("The server redirected the API request. Redirects are blocked."));
        return;
    }
    if (status == 403)
    {
        fail(tr("Access denied (HTTP 403). Too many failed logins may have triggered an IP ban."));
        return;
    }
    if (status == 401)
    {
        fail(tr("Authentication was rejected (HTTP 401)."));
        return;
    }
    if (error != QNetworkReply::NoError)
    {
        if (error == QNetworkReply::SslHandshakeFailedError)
            fail(tr("TLS certificate validation failed. The certificate was not trusted."));
        else
            fail(tr("Cannot reach the server: %1").arg(errorText));
        return;
    }
    if (status != 200)
    {
        fail(tr("Unexpected HTTP response: %1").arg(status));
        return;
    }
    onSuccess(response);
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

bool RemoteSession::isConnected() const noexcept
{
    return m_connected;
}

void RemoteSession::disconnectFromServer()
{
    m_syncTimer.stop();
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
    m_connected = false;
    emit failed(reason);
}

} // namespace Remote
