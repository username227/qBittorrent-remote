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
#include "api/PeerEndpoint.h"
#include <QNetworkCookieJar>
#include <QSslConfiguration>
#include <QUrlQuery>
#include <memory>

#include <algorithm>
#include <utility>

namespace
{
// qBittorrent 5.2 changed editTracker's original URL parameter from
// origUrl to url. Use the same request for every URL edit, including
// private tracker replacement and rollback, and encode form values once.
QByteArray trackerUrlEditBody(Remote::ServerCapabilities::TrackerEditSyntax syntax, const QString &hash,
                              const QString &original, const QString &replacement)
{
    using Syntax = Remote::ServerCapabilities::TrackerEditSyntax;
    Q_ASSERT(syntax == Syntax::LegacyOrigUrl || syntax == Syntax::ModernUrl);
    return QByteArrayLiteral("hash=") + QUrl::toPercentEncoding(hash)
        + (syntax == Syntax::ModernUrl ? QByteArrayLiteral("&url=") : QByteArrayLiteral("&origUrl="))
        + QUrl::toPercentEncoding(original)
        + QByteArrayLiteral("&newUrl=") + QUrl::toPercentEncoding(replacement);
}

// Only issuer trust and endpoint-name errors are eligible for explicit
// endpoint-scoped certificate pinning. Expiry, revocation, invalid
// signatures, weak keys and all other reported errors remain fatal.
bool arePinnableVerificationErrors(const QList<QSslError> &errors)
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
        case QSslError::HostNameMismatch:
            break;
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
    // A conflicting SAN is still a mismatch, not a matching CN.
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
                                       const QStringList &errors,
                                       bool endpointIdentityMismatch)
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
        errors,
        endpointIdentityMismatch
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

    if (!arePinnableVerificationErrors(errors))
    {
        m_tlsFailure = tr("TLS certificate rejected: a verification error cannot be overridden.");
        reply->abort();
        return;
    }

    const QString hostname = m_address->baseUrl().host();
    const bool legacyIp = matchesLegacyIpCommonName(certificate, hostname);
    bool mismatch = !matchesIpAddressSubjectAltName(certificate, hostname);
    QStringList descriptions;
    for (const QSslError &error : errors)
    {
        descriptions.append(error.errorString());
        if (error.error() == QSslError::HostNameMismatch && !legacyIp)
            mismatch = true;
    }
    if (legacyIp)
        descriptions.append(tr("Legacy certificate without Subject Alternative Names; "
                               "the server IP matches its Common Name. "
                               "Regenerating with an IP SAN is recommended."));
    if (mismatch)
        descriptions.append(tr("Certificate identity mismatch: the certificate does not identify "
                               "the requested server address."));

    // Explicit approval of this exact leaf certificate for this endpoint is
    // an alternative identity check, not a global hostname exception.
    if (!approveCertificate(certificate, descriptions, mismatch))
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

    // Qt's hostname check may accept a DNS SAN or CN for an IP literal
    // on some backends. Keep our additional IP SAN check even when Qt
    // reports no errors. Only the exact endpoint-bound pin can waive it.
    if (!matchesIpAddressSubjectAltName(certificate, m_address->baseUrl().host()))
    {
        if (!approveCertificate(certificate, {tr(
                "Certificate identity mismatch: the certificate does not identify "
                "the requested server IP address.")}, true))
        {
            m_tlsFailure = tr("Certificate trust declined or the certificate changed.");
            reply->abort();
        }
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
    m_capabilities = {};
    ++m_detailGeneration;
    m_detailHash.clear();
    m_detailPage = -1;
    m_detailTrackersKnown = false;
    m_detailTrackerUrls.clear();
    m_detailAvailability = false;
    m_peerRid = 0;
    m_peerPrivacyKnown = false;
    m_peerPrivate = true;
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

    const QByteArray body = QByteArrayLiteral("username=") + QUrl::toPercentEncoding(profile.username)
        + QByteArrayLiteral("&password=") + QUrl::toPercentEncoding(password);
    // No password is saved in a profile, certificate store or session object.

    QNetworkReply *reply = m_network.post(request, body);
    armReply(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        checkReply(reply, [this, status](const QByteArray &response) {
            // Older servers return 200 with "Ok."; newer ones return 204.
            // Authenticated version reads must still succeed before connecting.
            if (status != 204 && response.trimmed() != QByteArrayLiteral("Ok."))
            {
                fail(tr("Login failed. Check your username and password."));
                return;
            }
            requestVersion();
        }, {}, true);
    });
}

void RemoteSession::checkReply(
    QNetworkReply *reply,
    const std::function<void(const QByteArray &)> &onSuccess,
    const std::function<void(const QString &)> &onFailure,
    bool allowNoContent)
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
        // Missing/expired sessions and operation-specific permissions can
        // both produce 403. Only a failed core poll ends an active session.
        if (m_connected && !onFailure)
            reject(tr("Server synchronization denied (HTTP 403). The session may have expired or access was revoked. Reconnect to continue."));
        else if (m_connected)
            reject(tr("Access denied (HTTP 403). Permission or session validity could not be confirmed."));
        else
            reject(tr("Access denied (HTTP 403). Check server access restrictions or an authentication IP ban."));
        return;
    }
    if (status == 401)
    {
        if (m_connected && !onFailure)
            reject(tr("Server synchronization rejected (HTTP 401). The session is no longer authorized. Reconnect to continue."));
        else
            reject(tr("Authentication was rejected (HTTP 401)."));
        return;
    }
    if (status == 404)
    {
        reject(tr("The server could not find the requested resource (HTTP 404)."));
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
    if (status != 200 && !(allowNoContent && status == 204))
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
            m_capabilities = ServerCapabilities::fromVersions(version, apiVersion);
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
            QJsonObject payload = json.object();
            // 4.1.0–4.1.2 send a list of category names (including
            // additions in incremental updates). From 4.1.3 onward the
            // category map carries definitions. Normalize the wire shape
            // once, preserving absence of savePath and other unknown fields.
            const auto categories = payload.value(QStringLiteral("categories"));
            if (categories.isArray())
            {
                QJsonObject names;
                for (const auto &entry : categories.toArray())
                {
                    if (!entry.isString() || entry.toString().isEmpty())
                    {
                        fail(tr("Invalid category name in /sync/maindata response."));
                        return;
                    }
                    names.insert(entry.toString(), QJsonObject{});
                }
                payload.insert(QStringLiteral("categories"), names);
            }
            else if (!categories.isUndefined() && !categories.isObject())
            {
                fail(tr("Invalid categories in /sync/maindata response."));
                return;
            }
            // Validate the complete envelope before committing the response
            // ID or exposing any part of the patch to the GUI. Missing
            // optional fields are legitimate (especially on old servers).
            // Do not constrain unknown fields or future torrent states.
            const auto validNames = [](const QJsonValue &value) {
                if (!value.isArray()) return false;
                for (const auto &item : value.toArray())
                    if (!item.isString() || item.toString().isEmpty()) return false;
                return true;
            };
            const auto validMap = [&](const QString &key, bool membership) {
                const auto value = payload.value(key);
                if (value.isUndefined()) return true;
                if (!value.isObject()) return false;
                const auto map = value.toObject();
                for (auto it = map.constBegin(); it != map.constEnd(); ++it)
                    if (it.key().isEmpty() || (membership ? !validNames(it.value()) : !it.value().isObject()))
                        return false;
                return true;
            };
            const qint64 rid = payload.value(QStringLiteral("rid")).toInteger(-1);
            const auto full = payload.value(QStringLiteral("full_update"));
            const auto state = payload.value(QStringLiteral("server_state"));
            bool valid = rid > 0 && (!m_capabilities.hasKnownRelease() || rid <= 1000000)
                && (full.isUndefined() || full.isBool())
                && (m_syncRid != 0 || full.toBool())
                && (state.isUndefined() || state.isObject())
                && validMap(QStringLiteral("torrents"), false)
                && validMap(QStringLiteral("categories"), false)
                && validMap(QStringLiteral("trackers"), true);
            for (const auto *key : {"torrents_removed", "categories_removed", "tags",
                                    "tags_removed", "trackers_removed"})
            {
                const auto value = payload.value(QLatin1String(key));
                if (!value.isUndefined() && !validNames(value)) valid = false;
            }
            if (!valid)
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
                                   std::function<void(const QString &)> onRejected,
                                   bool reportOperation)
{
    if (!m_connected || !m_address)
    {
        const QString reason = tr("Server is not connected.");
        if (reportOperation)
            emit operationFailed(description, reason);
        if (onRejected)
            onRejected(reason);
        return;
    }
    QueuedOperation operation{endpoint, body, contentType, description, checkAccepted};
    operation.onAccepted = std::move(onAccepted);
    operation.onRejected = std::move(onRejected);
    operation.reportOperation = reportOperation;
    m_operations.enqueue(std::move(operation));
    pumpOperations();
}

void RemoteSession::setSessionPaused(bool paused)
{
    // A true libtorrent session pause preserves the HTTP API and individual
    // torrent states. Never emulate it with torrents/stop or torrents/start.
    queueOperation(paused ? QStringLiteral("api/v2/transfer/pauseSession")
                          : QStringLiteral("api/v2/transfer/resumeSession"),
                   {}, QByteArrayLiteral("application/x-www-form-urlencoded"),
                   paused ? tr("Pause session") : tr("Resume session"), true);
}

void RemoteSession::setAlternativeSpeedLimitsEnabled(bool enabled)
{
    // Use an explicit mode, not the toggle endpoint: retrying or switching
    // active server must never invert the wrong session's speed mode.
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("mode"), enabled ? QStringLiteral("1") : QStringLiteral("0"));
    queueOperation(QStringLiteral("api/v2/transfer/setSpeedLimitsMode"),
                   form.toString(QUrl::FullyEncoded).toUtf8(),
                   QByteArrayLiteral("application/x-www-form-urlencoded"),
                   tr("Set alternative speed mode"), true);
}

void RemoteSession::requestPreferences(PreferencesResult received, PreferencesError rejected)
{
    if (!m_connected || !m_address)
    {
        const QString reason = tr("The selected server is not connected.");
        if (rejected)
            rejected(reason);
        else if (!received)
            emit preferencesFailed(reason);
        return;
    }
    QueuedOperation request;
    request.endpoint = QStringLiteral("api/v2/app/preferences");
    request.description = tr("Read server preferences");
    request.jsonObjectRequest = true;
    request.reportOperation = !received && !rejected;
    request.onJsonObject = std::move(received);
    request.onRejected = std::move(rejected);
    m_operations.enqueue(std::move(request));
    pumpOperations();
}

void RemoteSession::rotateWebUIAPIKey(PreferencesResult received, PreferencesError rejected)
{
    if (!m_connected || !m_address)
    {
        if (rejected) rejected(tr("The selected server is not connected."));
        return;
    }
    QueuedOperation request;
    request.endpoint = QStringLiteral("api/v2/app/rotateAPIKey");
    request.description = tr("Rotate server API key");
    request.contentType = QByteArrayLiteral("application/x-www-form-urlencoded");
    request.jsonObjectRequest = true;
    request.jsonObjectPost = true;
    request.reportOperation = false;
    request.onJsonObject = std::move(received);
    request.onRejected = std::move(rejected);
    m_operations.enqueue(std::move(request));
    pumpOperations();
}

void RemoteSession::deleteWebUIAPIKey(std::function<void()> accepted, PreferencesError rejected)
{
    queueOperation(QStringLiteral("api/v2/app/deleteAPIKey"), {},
        QByteArrayLiteral("application/x-www-form-urlencoded"),
        tr("Delete server API key"), true, std::move(accepted), std::move(rejected), false);
}

void RemoteSession::requestBuildInfo(PreferencesResult received, PreferencesError rejected)
{
    if (!m_connected || !m_address)
    {
        if (rejected)
            rejected(tr("The selected server is not connected."));
        return;
    }
    // Known absent read endpoints need no speculative request. An unknown
    // version may still expose this read-only information, so allow probing.
    if (m_capabilities.status(ServerCapabilities::Feature::BuildInfo)
        == ServerCapabilities::Status::Unsupported)
    {
        if (rejected) rejected(m_capabilities.unavailableReason(ServerCapabilities::Feature::BuildInfo));
        return;
    }
    QueuedOperation request;
    request.endpoint = QStringLiteral("api/v2/app/buildInfo");
    request.description = tr("Read server build information");
    request.jsonObjectRequest = true;
    request.reportOperation = false;
    request.onJsonObject = std::move(received);
    request.onRejected = std::move(rejected);
    m_operations.enqueue(std::move(request));
    pumpOperations();
}

void RemoteSession::requestProcessInfo(PreferencesResult received, PreferencesError rejected)
{
    if (!m_connected || !m_address)
    {
        if (rejected)
            rejected(tr("The selected server is not connected."));
        return;
    }
    // Known absent read endpoints need no speculative request. An unknown
    // version may still expose this read-only information, so allow probing.
    if (m_capabilities.status(ServerCapabilities::Feature::ProcessInfo)
        == ServerCapabilities::Status::Unsupported)
    {
        if (rejected) rejected(m_capabilities.unavailableReason(ServerCapabilities::Feature::ProcessInfo));
        return;
    }
    QueuedOperation request;
    request.endpoint = QStringLiteral("api/v2/app/processInfo");
    request.description = tr("Read server process information");
    request.jsonObjectRequest = true;
    request.reportOperation = false;
    request.onJsonObject = std::move(received);
    request.onRejected = std::move(rejected);
    m_operations.enqueue(std::move(request));
    pumpOperations();
}

void RemoteSession::requestLog(bool peers, int lastKnownId, ArrayResult received, PreferencesError rejected)
{
    if (!m_connected || !m_address)
    {
        if (rejected) rejected(tr("The selected server is not connected."));
        return;
    }
    QueuedOperation request;
    request.endpoint = (peers ? QStringLiteral("api/v2/log/peers") : QStringLiteral("api/v2/log/main"))
        + QStringLiteral("?last_known_id=") + QString::number(lastKnownId);
    request.description = tr("Read remote execution log");
    request.jsonArrayRequest = true;
    request.reportOperation = false;
    request.onJsonArray = std::move(received);
    request.onRejected = std::move(rejected);
    m_operations.enqueue(std::move(request));
    pumpOperations();
}

void RemoteSession::requestDirectoryContent(const QString &path, bool directories, ArrayResult received, PreferencesError rejected)
{
    if (!m_connected || !m_address || path.isEmpty() || path.contains(QChar::Null))
    {
        if (rejected) rejected(tr("The selected server or directory is unavailable."));
        return;
    }
    QueuedOperation request;
    request.endpoint = QStringLiteral("api/v2/app/getDirectoryContent?dirPath=")
        + QString::fromLatin1(QUrl::toPercentEncoding(path))
        + (directories ? QStringLiteral("&mode=dirs") : QStringLiteral("&mode=files"))
        + QStringLiteral("&withMetadata=true");
    request.description = tr("Read remote directory");
    request.jsonArrayRequest = true;
    request.reportOperation = false;
    request.onJsonArray = std::move(received);
    request.onRejected = std::move(rejected);
    m_operations.enqueue(std::move(request));
    pumpOperations();
}

void RemoteSession::requestPathFreeSpace(const QString &path, std::function<void(qint64)> received,
    PreferencesError rejected)
{
    if (!supportsPathFreeSpace())
    {
        if (rejected) rejected(m_capabilities.unavailableReason(ServerCapabilities::Feature::PathFreeSpace));
        return;
    }
    if (path.isEmpty() || path.contains(QChar::Null))
    {
        if (rejected) rejected(tr("Path-specific free space is unavailable."));
        return;
    }
    QueuedOperation request;
    request.endpoint = QStringLiteral("api/v2/app/getFreeSpaceAtPath?path=")
        + QString::fromLatin1(QUrl::toPercentEncoding(path));
    request.description = tr("Read available remote disk space");
    request.bytesRequest = true;
    request.reportOperation = false;
    request.onBytes = [received = std::move(received), rejected](const QByteArray &response) {
        bool ok = false;
        const qint64 bytes = response.trimmed().toLongLong(&ok);
        if (!ok || bytes < 0)
        {
            if (rejected) rejected(tr("Available disk space is unknown."));
        }
        else if (received) received(bytes);
    };
    request.onRejected = std::move(rejected);
    m_operations.enqueue(std::move(request));
    pumpOperations();
}

void RemoteSession::requestCookies(ArrayResult received, PreferencesError rejected)
{
    if (!m_connected || !m_address)
    {
        if (rejected) rejected(tr("The selected server is not connected."));
        return;
    }
    QueuedOperation request;
    request.endpoint = QStringLiteral("api/v2/app/cookies");
    request.description = tr("Read remote download cookies");
    request.jsonArrayRequest = true;
    request.reportOperation = false;
    request.onJsonArray = std::move(received);
    request.onRejected = std::move(rejected);
    m_operations.enqueue(std::move(request));
    pumpOperations();
}

void RemoteSession::setCookies(const QJsonArray &cookies, std::function<void()> accepted, PreferencesError rejected)
{
    const QByteArray body = QByteArrayLiteral("cookies=") + QUrl::toPercentEncoding(
        QString::fromUtf8(QJsonDocument(cookies).toJson(QJsonDocument::Compact)));
    queueOperation(QStringLiteral("api/v2/app/setCookies"), body,
        QByteArrayLiteral("application/x-www-form-urlencoded"), tr("Update remote download cookies"),
        true, std::move(accepted), std::move(rejected), false);
}

void RemoteSession::requestNetworkInterfaces(ArrayResult received, PreferencesError rejected)
{
    if (!m_connected || !m_address)
    {
        if (rejected) rejected(tr("The selected server is not connected."));
        return;
    }
    QueuedOperation request;
    request.endpoint = QStringLiteral("api/v2/app/networkInterfaceList");
    request.description = tr("Read remote network interfaces");
    request.jsonArrayRequest = true;
    request.reportOperation = false;
    request.onJsonArray = std::move(received);
    request.onRejected = std::move(rejected);
    m_operations.enqueue(std::move(request));
    pumpOperations();
}

void RemoteSession::requestNetworkAddresses(const QString &iface, ArrayResult received, PreferencesError rejected)
{
    if (!m_connected || !m_address)
    {
        if (rejected) rejected(tr("The selected server is not connected."));
        return;
    }
    QueuedOperation request;
    request.endpoint = QStringLiteral("api/v2/app/networkInterfaceAddressList?iface=")
        + QString::fromLatin1(QUrl::toPercentEncoding(iface));
    request.description = tr("Read remote interface addresses");
    request.jsonArrayRequest = true;
    request.reportOperation = false;
    request.onJsonArray = std::move(received);
    request.onRejected = std::move(rejected);
    m_operations.enqueue(std::move(request));
    pumpOperations();
}

namespace
{
QByteArray rssForm(const QMap<QString, QString> &params)
{
    QByteArray result;
    for (auto it = params.cbegin(); it != params.cend(); ++it)
    {
        if (!result.isEmpty()) result += '&';
        result += QUrl::toPercentEncoding(it.key()) + '=' + QUrl::toPercentEncoding(it.value());
    }
    return result;
}
}

void RemoteSession::createTorrentTask(const QMap<QString, QString> &params,
                                     PreferencesResult received, PreferencesError rejected)
{
    if (!m_connected || !m_address)
    {
        if (rejected) rejected(tr("The selected server is not connected."));
        return;
    }
    QueuedOperation request;
    request.endpoint = QStringLiteral("api/v2/torrentcreator/addTask");
    request.body = rssForm(params);
    request.contentType = QByteArrayLiteral("application/x-www-form-urlencoded");
    request.description = tr("Create torrent on server");
    request.jsonObjectRequest = true;
    request.jsonObjectPost = true;
    request.reportOperation = false;
    request.onJsonObject = std::move(received);
    request.onRejected = std::move(rejected);
    m_operations.enqueue(std::move(request));
    pumpOperations();
}

void RemoteSession::requestTorrentCreationStatus(const QString &taskId,
                                                ArrayResult received, PreferencesError rejected)
{
    if (!m_connected || !m_address)
    {
        if (rejected) rejected(tr("The selected server is not connected."));
        return;
    }
    QueuedOperation request;
    request.endpoint = QStringLiteral("api/v2/torrentcreator/status");
    if (!taskId.isEmpty()) request.endpoint += QStringLiteral("?taskID=") + QString::fromLatin1(QUrl::toPercentEncoding(taskId));
    request.description = tr("Read torrent creation status");
    request.jsonArrayRequest = true;
    request.reportOperation = false;
    request.onJsonArray = std::move(received);
    request.onRejected = std::move(rejected);
    m_operations.enqueue(std::move(request));
    pumpOperations();
}

void RemoteSession::downloadCreatedTorrent(const QString &taskId,
                                           BytesResult received, PreferencesError rejected)
{
    if (!m_connected || !m_address || taskId.isEmpty())
    {
        if (rejected) rejected(tr("The torrent creation task is unavailable."));
        return;
    }
    QueuedOperation request;
    request.endpoint = QStringLiteral("api/v2/torrentcreator/torrentFile?taskID=")
        + QString::fromLatin1(QUrl::toPercentEncoding(taskId));
    request.description = tr("Retrieve created torrent");
    request.bytesRequest = true;
    request.reportOperation = false;
    request.onBytes = std::move(received);
    request.onRejected = std::move(rejected);
    m_operations.enqueue(std::move(request));
    pumpOperations();
}

void RemoteSession::deleteTorrentCreationTask(const QString &taskId,
                                               std::function<void()> accepted, PreferencesError rejected)
{
    if (taskId.isEmpty())
    {
        if (rejected) rejected(tr("No torrent creation task was selected."));
        return;
    }
    queueOperation(QStringLiteral("api/v2/torrentcreator/deleteTask"),
        rssForm({{QStringLiteral("taskID"), taskId}}),
        QByteArrayLiteral("application/x-www-form-urlencoded"), tr("Remove torrent creation task"),
        true, std::move(accepted), std::move(rejected), false);
}

void RemoteSession::requestRss(const QString &action, const QMap<QString, QString> &params,
                               PreferencesResult received, PreferencesError rejected)
{
    if (!m_connected || !m_address
        || !QStringList{QStringLiteral("items"), QStringLiteral("rules"),
                         QStringLiteral("matchingArticles")}.contains(action))
    {
        if (rejected) rejected(tr("RSS request is unavailable."));
        return;
    }
    QueuedOperation request;
    request.endpoint = QStringLiteral("api/v2/rss/") + action;
    if (!params.isEmpty()) request.endpoint += QLatin1Char('?') + QString::fromLatin1(rssForm(params));
    request.description = tr("Read RSS data");
    request.jsonObjectRequest = true;
    request.reportOperation = false;
    request.onJsonObject = std::move(received);
    request.onRejected = std::move(rejected);
    m_operations.enqueue(std::move(request));
    pumpOperations();
}

void RemoteSession::changeRss(const QString &action, const QMap<QString, QString> &params,
                              std::function<void()> accepted, PreferencesError rejected)
{
    const QStringList allowed{QStringLiteral("addFeed"), QStringLiteral("addFolder"),
        QStringLiteral("setFeedURL"), QStringLiteral("moveItem"), QStringLiteral("removeItem"),
        QStringLiteral("refreshItem"), QStringLiteral("markAsRead"), QStringLiteral("setRule"),
        QStringLiteral("renameRule"), QStringLiteral("removeRule"), QStringLiteral("setFeedRefreshInterval")};
    if (!allowed.contains(action))
    {
        if (rejected) rejected(tr("Unsupported RSS operation."));
        return;
    }
    queueOperation(QStringLiteral("api/v2/rss/") + action, rssForm(params),
        QByteArrayLiteral("application/x-www-form-urlencoded"), tr("Update RSS"),
        true, std::move(accepted), std::move(rejected), false);
}

void RemoteSession::setPreferences(const QJsonObject &changes,
                                   std::function<void()> accepted, PreferencesError rejected)
{
    if (changes.isEmpty())
    {
        if (accepted)
            accepted();
        return;
    }
    // Form encoding must preserve literal '+', '&', '%' and Unicode
    // (especially future SMTP/proxy credentials), not interpret '+' as space.
    const QByteArray body = QByteArrayLiteral("json=") + QUrl::toPercentEncoding(
        QString::fromUtf8(QJsonDocument(changes).toJson(QJsonDocument::Compact)));
    const bool reportOperation = !accepted && !rejected;
    queueOperation(QStringLiteral("api/v2/app/setPreferences"),
        body,
        QByteArrayLiteral("application/x-www-form-urlencoded"),
        tr("Update server preferences"), true, std::move(accepted),
        std::move(rejected), reportOperation);
}

void RemoteSession::sendTestEmail(std::function<void()> accepted, PreferencesError rejected)
{
    queueOperation(QStringLiteral("api/v2/app/sendTestEmail"), {},
        QByteArrayLiteral("application/x-www-form-urlencoded"),
        tr("Send test email"), true, std::move(accepted), std::move(rejected), false);
}

void RemoteSession::pumpOperations()
{
    // The existing secure TLS/checkReply path tracks one outstanding reply.
    // Serialize all operations with /sync/maindata so no request can overwrite
    // another reply's TLS verification context or session error state.
    if (!m_connected || m_pendingReply || m_operations.isEmpty())
        return;

    const QueuedOperation operation = m_operations.dequeue();
    const bool peerRequest = operation.detailsRequest && operation.detailTab == 2;
    QNetworkRequest request = requestFor(operation.endpoint
        + (peerRequest ? QStringLiteral("&rid=%1").arg(m_peerRid) : QString()));
    if (!operation.detailsRequest && !operation.exportRequest && !operation.jsonArrayRequest && !operation.bytesRequest
        && (!operation.jsonObjectRequest || operation.jsonObjectPost))
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QString::fromLatin1(operation.contentType));
    QNetworkReply *reply = (operation.detailsRequest || operation.exportRequest
                            || operation.bytesRequest || operation.jsonArrayRequest || (operation.jsonObjectRequest && !operation.jsonObjectPost))
        ? m_network.get(request) : m_network.post(request, operation.body);
    armReply(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, operation]() {
        checkReply(reply, [this, operation](const QByteArray &response) {
            if (operation.exportRequest)
            {
                if (response.isEmpty() || response.size() > 32 * 1024 * 1024)
                {
                    const QString reason = tr("The server returned an invalid or oversized torrent file.");
                    emit operationFailed(operation.description, reason);
                    emit torrentExportFailed(operation.exportHash, reason);
                }
                else
                    emit torrentExportReceived(operation.exportHash, response);
            }
            else if (operation.bytesRequest)
            {
                if (response.isEmpty() || response.size() > 32 * 1024 * 1024)
                {
                    if (operation.onRejected) operation.onRejected(tr("Invalid or oversized torrent file returned by the server."));
                }
                else if (operation.onBytes) operation.onBytes(response);
            }
            else if (operation.jsonArrayRequest)
            {
                QJsonParseError error;
                const auto json = QJsonDocument::fromJson(response, &error);
                if (error.error != QJsonParseError::NoError || !json.isArray())
                {
                    if (operation.onRejected) operation.onRejected(tr("Invalid list returned by the server."));
                }
                else if (operation.onJsonArray) operation.onJsonArray(json.array());
            }
            else if (operation.jsonObjectRequest)
            {
                QJsonParseError error;
                const QJsonDocument json = QJsonDocument::fromJson(response, &error);
                if (error.error != QJsonParseError::NoError || !json.isObject())
                {
                    const QString reason = operation.endpoint == QLatin1String("api/v2/app/buildInfo")
                        ? tr("Invalid build information returned by the server.")
                        : operation.endpoint == QLatin1String("api/v2/app/processInfo")
                            ? tr("Invalid process information returned by the server.")
                        : operation.endpoint.startsWith(QLatin1String("api/v2/rss/"))
                            ? tr("Invalid RSS data returned by the server.")
                            : operation.endpoint.startsWith(QLatin1String("api/v2/torrentcreator/"))
                                ? tr("Invalid torrent creation response returned by the server.")
                            : operation.endpoint == QLatin1String("api/v2/torrents/addPeers")
                                ? tr("Invalid peer-addition response returned by the server.")
                            : operation.jsonObjectPost
                                ? tr("Invalid API key response returned by the server.")
                                : tr("Invalid preferences returned by the server.");
                    if (operation.onRejected)
                        operation.onRejected(reason);
                    else if (operation.reportOperation)
                        emit preferencesFailed(reason);
                }
                else
                {
                    if (operation.endpoint == QLatin1String("api/v2/app/preferences"))
                        emit dhtPreferenceObserved(json.object().value(QStringLiteral("dht")));
                    if (operation.onJsonObject)
                        operation.onJsonObject(json.object());
                    else if (operation.reportOperation)
                        emit preferencesReceived(json.object());
                }
            }
            else if (operation.detailsRequest)
            {
                if (operation.detailGeneration != m_detailGeneration) return;
                QJsonParseError error;
                const QJsonDocument json = QJsonDocument::fromJson(response, &error);
                const bool expectsObject = (operation.detailTab == 0 || operation.detailTab == 2 || operation.detailTab == 8);
                if (error.error != QJsonParseError::NoError
                    || (expectsObject && !json.isObject())
                    || (!expectsObject && !json.isArray()))
                {
                    if (operation.detailTab == 2) m_peerRid = 0;
                    if (operation.detailTab == 8) m_peerPrivacyKnown = false;
                    if (operation.detailTab == 1) m_detailTrackersKnown = false;
                    emit torrentDetailsFailed(operation.detailHash, operation.detailTab,
                                              tr("Unexpected torrent-details response."));
                    return;
                }
                if (operation.detailTab == 1)
                {
                    m_detailTrackerUrls.clear();
                    for (const auto &record : json.array())
                    {
                        const auto url = record.toObject().value(QStringLiteral("url")).toString();
                        if (!url.isEmpty() && !url.startsWith(QLatin1String("** ["))
                            && !m_detailTrackerUrls.contains(url)) m_detailTrackerUrls.append(url);
                    }
                    m_detailTrackersKnown = true;
                }
                if (operation.detailTab == 8)
                {
                    const auto privacy = json.object().value(QStringLiteral("private"));
                    m_peerPrivacyKnown = privacy.isBool();
                    m_peerPrivate = !m_peerPrivacyKnown || privacy.toBool();
                }
                if (operation.detailTab == 2)
                {
                    const auto peers = json.object();
                    const int rid = peers.value(QStringLiteral("rid")).toInt(-1);
                    const auto updates = peers.value(QStringLiteral("peers"));
                    const auto removed = peers.value(QStringLiteral("peers_removed"));
                    bool valid = rid > 0 && rid <= 1000000
                        && (m_peerRid != 0 || peers.value(QStringLiteral("full_update")).toBool())
                        && (updates.isUndefined() || updates.isObject())
                        && (removed.isUndefined() || removed.isArray());
                    const auto changes = updates.toObject();
                    for (auto it = changes.constBegin(); it != changes.constEnd(); ++it)
                        valid = valid && !it.key().isEmpty() && it.value().isObject();
                    for (const auto &key : removed.toArray())
                        valid = valid && key.isString();
                    if (!valid)
                    {
                        m_peerRid = 0;
                        emit torrentDetailsFailed(operation.detailHash, operation.detailTab,
                            tr("Unexpected peer synchronization response."));
                        return;
                    }
                    m_peerRid = rid;
                }
                emit torrentDetailsReceived(operation.detailHash, operation.detailTab,
                                             expectsObject ? QJsonValue(json.object())
                                                           : QJsonValue(json.array()));
            }
            else if (operation.checkAccepted && response.trimmed() == QByteArrayLiteral("Fails."))
            {
                const QString reason = tr("The server rejected the request.");
                if (operation.reportOperation)
                    emit operationFailed(operation.description, reason);
                if (operation.onRejected)
                    operation.onRejected(reason);
            }
            else
            {
                if (operation.reportOperation)
                    emit operationSucceeded(operation.description);
                if (operation.onAccepted)
                    operation.onAccepted();
            }
        }, [this, operation](const QString &reason) {
            if (operation.detailsRequest)
            {
                if (operation.detailGeneration == m_detailGeneration)
                {
                    if (operation.detailTab == 2) m_peerRid = 0;
                    if (operation.detailTab == 8) m_peerPrivacyKnown = false;
                    if (operation.detailTab == 1) m_detailTrackersKnown = false;
                    emit torrentDetailsFailed(operation.detailHash, operation.detailTab, reason);
                }
            }
            else if (operation.jsonObjectRequest || operation.jsonArrayRequest)
            {
                if (operation.onRejected)
                    operation.onRejected(reason);
                else if (operation.reportOperation)
                    emit preferencesFailed(reason);
            }
            else
            {
                if (operation.reportOperation)
                    emit operationFailed(operation.description, reason);
                if (operation.exportRequest)
                    emit torrentExportFailed(operation.exportHash, reason);
                if (operation.onRejected)
                    operation.onRejected(reason);
            }
        }, !operation.detailsRequest && !operation.exportRequest && !operation.jsonObjectRequest && !operation.jsonArrayRequest && !operation.bytesRequest);
        // Continue the queue even when an individual command was rejected.
        if (m_connected && !m_pendingReply)
            QTimer::singleShot(0, this, &RemoteSession::pumpOperations);
    });
}

void RemoteSession::setRefreshInterval(int milliseconds)
{
    const int interval = qBound(30, milliseconds, 99999);
    if (m_syncTimer.interval() != interval) m_syncTimer.setInterval(interval);
}

void RemoteSession::requestTorrentDetails(const QString &hash, int tab, bool availability)
{
    static const QRegularExpression validHash(
        QStringLiteral("^(?:[a-fA-F0-9]{40}|[a-fA-F0-9]{64})$"));
    const bool valid = m_connected && m_address && validHash.match(hash).hasMatch()
        && tab >= 0 && tab <= 4;
    const QString selected = valid ? hash : QString();
    const int page = valid ? tab : -1;
    availability = valid && page == 0 && availability && m_capabilities.pieceAvailability();
    if (m_detailHash != selected || m_detailPage != page || m_detailAvailability != availability)
    {
        ++m_detailGeneration;
        m_peerRid = 0;
        m_peerPrivacyKnown = false;
        m_peerPrivate = true;
        m_detailTrackersKnown = false;
        m_detailTrackerUrls.clear();
        m_detailHash = selected;
        m_detailPage = page;
        m_detailAvailability = availability;
    }
    for (int i = m_operations.size() - 1; i >= 0; --i)
        if (m_operations.at(i).detailsRequest)
            m_operations.removeAt(i);
    if (!valid) return;
    static const QStringList endpoints {
        QStringLiteral("torrents/properties"),
        QStringLiteral("torrents/trackers"),
        QStringLiteral("sync/torrentPeers"),
        QStringLiteral("torrents/webseeds"),
        QStringLiteral("torrents/files")
    };
    const auto enqueue = [&](const QString &endpoint, int kind) {
        const QString url = QStringLiteral("api/v2/%1?hash=%2")
            .arg(endpoint, QString::fromLatin1(QUrl::toPercentEncoding(hash)));
        QueuedOperation request {url, {}, {}, {}, false};
        request.detailsRequest = true;
        request.detailHash = hash;
        request.detailTab = kind;
        request.detailGeneration = m_detailGeneration;
        m_operations.enqueue(std::move(request));
    };
    if ((tab == 1 || tab == 2) && !m_peerPrivacyKnown)
        enqueue(QStringLiteral("torrents/properties"), 8);
    enqueue(endpoints.at(tab), tab);
    if (tab == 0)
    {
        enqueue(QStringLiteral("torrents/pieceStates"), 6);
        if (availability) enqueue(QStringLiteral("torrents/pieceAvailability"), 7);
    }
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
    if (!validSelectedTorrentHashes(hashes))
    {
        emit operationFailed(tr("Create torrent category"), tr("Invalid torrent selection."));
        return;
    }
    saveCategory(category, {{QStringLiteral("savePath"), QString()}}, false, hashes);
}

void RemoteSession::saveCategory(const QString &category, const QJsonObject &fields,
                                 bool edit, const QStringList &assignHashes)
{
    // Upstream Session::isValidCategoryName rules, plus Web API delimiters.
    if (category.trimmed().isEmpty() || category.contains(QLatin1Char('\\'))
        || category.startsWith(QLatin1Char('/')) || category.endsWith(QLatin1Char('/'))
        || category.contains(QStringLiteral("//")) || category.contains(QLatin1Char('\n'))
        || category.contains(QLatin1Char('\r')) || category.contains(QChar::Null)
        || (!assignHashes.isEmpty() && !validSelectedTorrentHashes(assignHashes)))
    {
        emit operationFailed(tr("Save torrent category"), tr("Invalid category name or torrent selection."));
        return;
    }
    if (edit && fields.isEmpty())
        return;
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("category"), category);
    for (auto it = fields.constBegin(); it != fields.constEnd(); ++it)
    {
        QString value = it.value().toVariant().toString();
        if (value.isNull()) value = QStringLiteral("");
        form.addQueryItem(it.key(), value);
    }
    queueOperation(edit ? QStringLiteral("api/v2/torrents/editCategory")
                        : QStringLiteral("api/v2/torrents/createCategory"),
                   form.toString(QUrl::FullyEncoded).toUtf8(),
                   QByteArrayLiteral("application/x-www-form-urlencoded"),
                   edit ? tr("Edit torrent category") : tr("Create torrent category"), true,
                   [this, assignHashes, category]() {
                       if (!assignHashes.isEmpty()) setTorrentCategory(assignHashes, category);
                   });
}

void RemoteSession::removeCategories(const QStringList &categories)
{
    if (categories.isEmpty())
        return;
    for (const auto &name : categories)
        if (name.isEmpty() || name.contains(QLatin1Char('\n')) || name.contains(QLatin1Char('\r')))
            return;
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("categories"), categories.join(QLatin1Char('\n')));
    queueOperation(QStringLiteral("api/v2/torrents/removeCategories"),
                   form.toString(QUrl::FullyEncoded).toUtf8(),
                   QByteArrayLiteral("application/x-www-form-urlencoded"), tr("Remove categories"), true);
}

void RemoteSession::manageTagDefinitions(const QStringList &tags, bool create)
{
    if (!validSelectedTags(tags))
    {
        emit operationFailed(tr("Manage tags"), tr("Invalid tag names."));
        return;
    }
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("tags"), tags.join(QLatin1Char(',')));
    queueOperation(create ? QStringLiteral("api/v2/torrents/createTags")
                          : QStringLiteral("api/v2/torrents/deleteTags"),
                   form.toString(QUrl::FullyEncoded).toUtf8(),
                   QByteArrayLiteral("application/x-www-form-urlencoded"),
                   create ? tr("Add tags") : tr("Remove tags"), true);
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

void RemoteSession::requestTorrentExport(const QString &hash)
{
    static const QRegularExpression validHash(
        QStringLiteral("^(?:[a-fA-F0-9]{40}|[a-fA-F0-9]{64})$"));
    if (!m_connected || !m_address || !validHash.match(hash).hasMatch())
    {
        const QString reason = tr("Server is disconnected or torrent hash is invalid.");
        emit operationFailed(tr("Export .torrent"), reason);
        emit torrentExportFailed(hash, reason);
        return;
    }
    QueuedOperation request {
        QStringLiteral("api/v2/torrents/export?hash=")
            + QString::fromLatin1(QUrl::toPercentEncoding(hash)),
        {}, {}, tr("Export .torrent"), false
    };
    request.exportRequest = true;
    request.exportHash = hash;
    m_operations.enqueue(std::move(request));
    pumpOperations();
}

void RemoteSession::renameTorrentContent(const QString &hash, const QString &oldPath,
                                         const QString &newPath)
{
    renameTorrentContentItem(hash, oldPath, newPath, false);
}

void RemoteSession::renameTorrentContentItem(const QString &hash, const QString &oldPath,
                                             const QString &newPath, bool folder)
{
    const QString description = folder ? tr("Rename torrent folder") : tr("Rename torrent file");
    static const QRegularExpression validHash(
        QStringLiteral("^(?:[a-fA-F0-9]{40}|[a-fA-F0-9]{64})$"));
    const auto validRelative = [](const QString &path) {
        if (path.isEmpty() || path.startsWith(QLatin1Char('/'))
            || path.contains(QLatin1Char('\\'))
            || path.contains(QLatin1Char('\r'))
            || path.contains(QLatin1Char('\n')) || path.contains(QChar(0)))
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
        emit operationFailed(description, tr("Invalid torrent or content path."));
        emit torrentContentRenameFinished(hash, oldPath, false, tr("Invalid file path."));
        return;
    }
    const QByteArray form = QByteArrayLiteral("hash=") + QUrl::toPercentEncoding(hash)
        + QByteArrayLiteral("&oldPath=") + QUrl::toPercentEncoding(oldPath)
        + QByteArrayLiteral("&newPath=") + QUrl::toPercentEncoding(newPath);
    queueOperation(folder ? QStringLiteral("api/v2/torrents/renameFolder")
                          : QStringLiteral("api/v2/torrents/renameFile"),
        form,
        QByteArrayLiteral("application/x-www-form-urlencoded"),
        description, true,
        [this, hash, oldPath]() {
            emit torrentContentRenameFinished(hash, oldPath, true, {});
        },
        [this, hash, oldPath](const QString &reason) {
            emit torrentContentRenameFinished(hash, oldPath, false, reason);
        });
}

void RemoteSession::setTorrentSavePath(const QStringList &hashes, const QString &path)
{
    if (!validSelectedTorrentHashes(hashes) || path.isEmpty()
        || path == QLatin1String("/") || !path.startsWith(QLatin1Char('/'))
        || path.contains(QLatin1Char('\\')) || path.contains(QLatin1Char('\r'))
        || path.contains(QLatin1Char('\n')))
    {
        emit operationFailed(tr("Set torrent save path"), tr("Invalid torrent or server path."));
        return;
    }
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("id"), hashes.join(QLatin1Char('|')));
    form.addQueryItem(QStringLiteral("path"), path);
    queueOperation(QStringLiteral("api/v2/torrents/setSavePath"),
        form.toString(QUrl::FullyEncoded).toUtf8(),
        QByteArrayLiteral("application/x-www-form-urlencoded"),
        tr("Set torrent save path"), true);
}

void RemoteSession::setTorrentDownloadPath(const QStringList &hashes, const QString &path)
{
    if (!validSelectedTorrentHashes(hashes)
        || (!path.isEmpty() && (path == QLatin1String("/")
            || !path.startsWith(QLatin1Char('/'))
            || path.contains(QLatin1Char('\\'))
            || path.contains(QLatin1Char('\r'))
            || path.contains(QLatin1Char('\n')))))
    {
        emit operationFailed(tr("Set incomplete torrent path"), tr("Invalid torrent or server path."));
        return;
    }
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("id"), hashes.join(QLatin1Char('|')));
    form.addQueryItem(QStringLiteral("path"), path);
    queueOperation(QStringLiteral("api/v2/torrents/setDownloadPath"),
        form.toString(QUrl::FullyEncoded).toUtf8(),
        QByteArrayLiteral("application/x-www-form-urlencoded"),
        tr("Set incomplete torrent path"), true);
}

void RemoteSession::setTorrentRateLimit(const QStringList &hashes, bool upload, qint64 bytesPerSec)
{
    if (!validSelectedTorrentHashes(hashes) || bytesPerSec < 0
        || bytesPerSec > 2000000LL * 1024)
    {
        emit operationFailed(tr("Set torrent speed limit"), tr("Invalid selection or speed."));
        return;
    }
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("hashes"), hashes.join(QLatin1Char('|')));
    form.addQueryItem(QStringLiteral("limit"), QString::number(bytesPerSec));
    queueOperation(QStringLiteral("api/v2/torrents/")
        + (upload ? QStringLiteral("setUploadLimit") : QStringLiteral("setDownloadLimit")),
        form.toString(QUrl::FullyEncoded).toUtf8(),
        QByteArrayLiteral("application/x-www-form-urlencoded"),
        tr("Set torrent speed limit"), true);
}

void RemoteSession::setTorrentShareLimits(const QString &hash, const QJsonObject &share)
{
    static const QRegularExpression hashPattern(
        QStringLiteral("^(?:[a-fA-F0-9]{40}|[a-fA-F0-9]{64})$"));
    const QStringList fields {
        QStringLiteral("ratio_limit"), QStringLiteral("seeding_time_limit"),
        QStringLiteral("inactive_seeding_time_limit"),
        QStringLiteral("share_limits_mode"), QStringLiteral("share_limit_action")
    };
    if (!hashPattern.match(hash).hasMatch()
        || std::any_of(fields.cbegin(), fields.cend(), [&share](const QString &key) {
            return !share.contains(key);
        }))
    {
        emit operationFailed(tr("Set torrent share limits"),
                             tr("The server has not supplied all share limit settings."));
        return;
    }
    for (int i = 0; i != 3; ++i)
        if (!share.value(fields.at(i)).isDouble())
        {
            emit operationFailed(tr("Set torrent share limits"), tr("Invalid share limit."));
            return;
        }
    const QString mode = share.value(QStringLiteral("share_limits_mode")).toString();
    const QString action = share.value(QStringLiteral("share_limit_action")).toString();
    if (!QStringList{QStringLiteral("Default"), QStringLiteral("MatchAny"),
                     QStringLiteral("MatchAll")}.contains(mode)
        || !QStringList{QStringLiteral("Default"), QStringLiteral("Stop"),
                        QStringLiteral("Remove"), QStringLiteral("RemoveWithContent"),
                        QStringLiteral("EnableSuperSeeding")}.contains(action))
    {
        emit operationFailed(tr("Set torrent share limits"), tr("Invalid share limit mode."));
        return;
    }
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("hashes"), hash);
    form.addQueryItem(QStringLiteral("ratioLimit"),
        QString::number(share.value(QStringLiteral("ratio_limit")).toDouble(), 'g', 14));
    form.addQueryItem(QStringLiteral("seedingTimeLimit"),
        QString::number(share.value(QStringLiteral("seeding_time_limit")).toInt()));
    form.addQueryItem(QStringLiteral("inactiveSeedingTimeLimit"),
        QString::number(share.value(QStringLiteral("inactive_seeding_time_limit")).toInt()));
    form.addQueryItem(QStringLiteral("shareLimitsMode"), mode);
    form.addQueryItem(QStringLiteral("shareLimitAction"), action);
    queueOperation(QStringLiteral("api/v2/torrents/setShareLimits"),
        form.toString(QUrl::FullyEncoded).toUtf8(),
        QByteArrayLiteral("application/x-www-form-urlencoded"),
        tr("Set torrent share limits"), true);
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

void RemoteSession::addTorrentPeers(const QString &hash, const QStringList &peers)
{
    const auto endpoints = normalizedPeerEndpoints(peers);
    const auto reject = [this, hash](const QString &reason) {
        emit torrentPeerActionFinished(hash, {});
        emit operationFailed(tr("Add peers"), reason);
    };
    if (!m_connected || !m_address || hash.isEmpty() || hash != m_detailHash
        || m_detailPage != 2 || !m_peerPrivacyKnown || m_peerPrivate || endpoints.isEmpty())
    {
        reject(tr("Peer addition requires a selected non-private torrent and valid IP:port addresses."));
        return;
    }
    QueuedOperation request {};
    request.endpoint = QStringLiteral("api/v2/torrents/addPeers");
    request.contentType = QByteArrayLiteral("application/x-www-form-urlencoded");
    request.body = QByteArrayLiteral("hashes=") + QUrl::toPercentEncoding(hash)
        + QByteArrayLiteral("&peers=") + QUrl::toPercentEncoding(endpoints.join(QLatin1Char('|')));
    request.description = tr("Add peers");
    request.jsonObjectRequest = true;
    request.jsonObjectPost = true;
    request.reportOperation = false;
    request.onRejected = reject;
    request.onJsonObject = [this, hash, count = endpoints.size(), reject](const QJsonObject &result) {
        const auto entry = result.value(hash).toObject();
        const int added = entry.value(QStringLiteral("added")).toInt(-1);
        const int failed = entry.value(QStringLiteral("failed")).toInt(-1);
        if (added < 0 || failed < 0 || static_cast<qint64>(added) + failed != count)
        {
            reject(tr("The server did not return valid peer-addition results."));
            return;
        }
        emit torrentPeerActionFinished(hash, tr("Peers added: %1; failed: %2.").arg(added).arg(failed));
    };
    m_operations.enqueue(std::move(request));
    pumpOperations();
}

void RemoteSession::banTorrentPeers(const QString &hash, const QStringList &peers)
{
    const auto endpoints = normalizedPeerEndpoints(peers);
    if (!m_connected || hash.isEmpty() || hash != m_detailHash
        || m_detailPage != 2 || endpoints.isEmpty())
    {
        emit torrentPeerActionFinished(hash, {});
        emit operationFailed(tr("Ban peers"), tr("Invalid peer selection."));
        return;
    }
    const QByteArray body = QByteArrayLiteral("peers=")
        + QUrl::toPercentEncoding(endpoints.join(QLatin1Char('|')));
    queueOperation(QStringLiteral("api/v2/transfer/banPeers"), body,
        QByteArrayLiteral("application/x-www-form-urlencoded"), tr("Ban peers"), true,
        [this, hash]() { emit torrentPeerActionFinished(hash, tr("Peer ban accepted by the server.")); },
        [this, hash](const QString &) { emit torrentPeerActionFinished(hash, {}); });
}

void RemoteSession::changeWebSeeds(const QString &hash, WebSeedAction action,
                                   const QStringList &urls, const QString &replacement)
{
    const auto reject = [this, hash]() {
        emit webSeedActionFinished(hash);
        emit operationFailed(tr("Change web seeds"), tr("Invalid torrent selection or web seed URL."));
    };
    if (!m_connected || hash.isEmpty() || hash != m_detailHash || m_detailPage != 3
        || urls.isEmpty() || !std::all_of(urls.cbegin(), urls.cend(), validWebSeedUrl)
        || (action == WebSeedAction::Edit && (urls.size() != 1 || !validWebSeedUrl(replacement))))
    {
        reject();
        return;
    }
    QString endpoint;
    QByteArray body = QByteArrayLiteral("hash=") + QUrl::toPercentEncoding(hash);
    switch (action)
    {
    case WebSeedAction::Add: endpoint = QStringLiteral("api/v2/torrents/addWebSeeds"); break;
    case WebSeedAction::Remove: endpoint = QStringLiteral("api/v2/torrents/removeWebSeeds"); break;
    case WebSeedAction::Edit: endpoint = QStringLiteral("api/v2/torrents/editWebSeed"); break;
    default: reject(); return;
    }
    if (action == WebSeedAction::Edit)
        body += QByteArrayLiteral("&origUrl=") + encodeWebSeedUrls(urls)
            + QByteArrayLiteral("&newUrl=") + encodeWebSeedUrls({replacement});
    else
        body += QByteArrayLiteral("&urls=") + encodeWebSeedUrls(urls);
    queueOperation(endpoint, body, QByteArrayLiteral("application/x-www-form-urlencoded"),
        tr("Change web seeds"), true,
        [this, hash]() { emit webSeedActionFinished(hash); },
        [this, hash](const QString &) { emit webSeedActionFinished(hash); });
}

void RemoteSession::setTorrentComment(const QString &hash, const QString &comment)
{
    static const QRegularExpression validHash(
        QStringLiteral("^(?:[a-fA-F0-9]{40}|[a-fA-F0-9]{64})$"));
    if (!supportsCommentEditing())
    {
        emit torrentCommentEditFinished(hash);
        emit operationFailed(tr("Edit torrent comment"),
            m_capabilities.unavailableReason(ServerCapabilities::Feature::CommentEditing));
        return;
    }
    if (!validHash.match(hash).hasMatch())
    {
        emit torrentCommentEditFinished(hash);
        emit operationFailed(tr("Edit torrent comment"),
            tr("Comment editing is unavailable or the torrent selection is invalid."));
        return;
    }
    // Explicit form encoding preserves literal plus, percent, Unicode and
    // newlines. An empty comment is a valid request to clear the comment.
    const QByteArray form = QByteArrayLiteral("hashes=") + hash.toLatin1()
        + QByteArrayLiteral("&comment=") + QUrl::toPercentEncoding(comment.trimmed());
    queueOperation(QStringLiteral("api/v2/torrents/setComment"), form,
        QByteArrayLiteral("application/x-www-form-urlencoded"), tr("Edit torrent comment"), true,
        [this, hash]() { emit torrentCommentEditFinished(hash); },
        [this, hash](const QString &) { emit torrentCommentEditFinished(hash); });
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
    // When the desktop options dialog is skipped, omit every override.
    if (options.useServerDefaults)
        return;
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
    if (options.disableDownloadPath && !options.autoTMM)
        form.addQueryItem(QStringLiteral("useDownloadPath"), QStringLiteral("false"));
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
    form.addQueryItem(QStringLiteral("addToTopOfQueue"), options.queueTop
        ? QStringLiteral("true") : QStringLiteral("false"));
    if (options.stopCondition == QLatin1String("None")
        || options.stopCondition == QLatin1String("MetadataReceived")
        || options.stopCondition == QLatin1String("FilesChecked"))
        form.addQueryItem(QStringLiteral("stopCondition"), options.stopCondition);
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
    appendAddOptions(form, options);
    QByteArray body = QByteArrayLiteral("urls=") + QUrl::toPercentEncoding(checked.join(QLatin1Char('\n')));
    if (!form.isEmpty()) body += '&' + form.toString(QUrl::FullyEncoded).toUtf8();
    queueOperation(QStringLiteral("api/v2/torrents/add"),
                   body,
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
    if (!supportsTrackerUrlEditing())
    {
        const auto reason = m_capabilities.unavailableReason(ServerCapabilities::Feature::TrackerUrlEditing);
        emit torrentTrackerEditFinished(hash, oldUrl, false, reason);
        emit operationFailed(tr("Edit torrent tracker"), reason);
        return;
    }
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
    queueOperation(QStringLiteral("api/v2/torrents/editTracker"),
                   trackerUrlEditBody(m_capabilities.trackerEditSyntax(), hash, oldUrl, newUrl),
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

void RemoteSession::changeDetailTrackers(const QString &hash, TrackerDetailAction action,
                                         const QStringList &urls, const QString &replacement)
{
    const auto reject = [this, hash](const QString &reason) {
        emit trackerTierEditFinished(hash);
        emit operationFailed(tr("Change trackers"), reason);
    };
    const bool all = action == TrackerDetailAction::ReannounceAll;
    if (!m_connected || hash.isEmpty() || hash != m_detailHash || m_detailPage != 1
        || !m_detailTrackersKnown || (!all && urls.isEmpty()) || urls.size() > 128
        || !std::all_of(urls.cbegin(), urls.cend(), validEditableTrackerUrl))
    {
        reject(tr("Invalid torrent selection or tracker list."));
        return;
    }
    QStringList unique = urls;
    unique.removeDuplicates();
    if (action == TrackerDetailAction::Add)
    {
        if ((!m_peerPrivacyKnown || m_peerPrivate)
            && (!m_detailTrackerUrls.isEmpty() || unique.size() != 1))
        {
            reject(tr("Adding additional trackers to private or unknown torrents is blocked."));
            return;
        }
        for (const auto &url : unique)
            if (m_detailTrackerUrls.contains(url))
            {
                reject(tr("This tracker is already in the list."));
                return;
            }
    }
    else if (!all)
    {
        for (const auto &url : unique)
            if (!m_detailTrackerUrls.contains(url))
            {
                reject(tr("The tracker selection changed."));
                return;
            }
    }
    if (action == TrackerDetailAction::Edit
        && (unique.size() != 1 || !validEditableTrackerUrl(replacement)
            || m_detailTrackerUrls.contains(replacement)))
    {
        reject(tr("Invalid or duplicate replacement tracker."));
        return;
    }
    if (action == TrackerDetailAction::Edit && !supportsTrackerUrlEditing())
    {
        reject(m_capabilities.unavailableReason(ServerCapabilities::Feature::TrackerUrlEditing));
        return;
    }
    if (action == TrackerDetailAction::ReannounceSelected && !supportsSelectedTrackerReannounce())
    {
        reject(m_capabilities.unavailableReason(ServerCapabilities::Feature::SelectedTrackerReannounce));
        return;
    }
    QString endpoint;
    QByteArray body;
    if (action == TrackerDetailAction::Add || action == TrackerDetailAction::Edit
        || action == TrackerDetailAction::Remove)
        body = QByteArrayLiteral("hash=") + QUrl::toPercentEncoding(hash);
    else body = QByteArrayLiteral("hashes=") + QUrl::toPercentEncoding(hash);
    switch (action)
    {
    case TrackerDetailAction::Add:
        endpoint = QStringLiteral("addTrackers");
        body += QByteArrayLiteral("&urls=") + QUrl::toPercentEncoding(unique.join(QLatin1Char('\n')));
        break;
    case TrackerDetailAction::Edit:
        endpoint = QStringLiteral("editTracker");
        body = trackerUrlEditBody(m_capabilities.trackerEditSyntax(), hash, unique.first(), replacement);
        break;
    case TrackerDetailAction::Remove:
    case TrackerDetailAction::ReannounceSelected:
        endpoint = action == TrackerDetailAction::Remove ? QStringLiteral("removeTrackers") : QStringLiteral("reannounce");
        // These endpoints split on '|' then percent-decode each URL.
        body += QByteArrayLiteral("&urls=") + encodeWebSeedUrls(unique);
        break;
    case TrackerDetailAction::ReannounceAll:
        endpoint = QStringLiteral("reannounce");
        break;
    default: reject(tr("Invalid tracker action.")); return;
    }
    m_detailTrackersKnown = false;
    queueOperation(QStringLiteral("api/v2/torrents/") + endpoint, body,
        QByteArrayLiteral("application/x-www-form-urlencoded"), tr("Change trackers"), true,
        [this, hash]() { emit torrentTrackersChanged(hash); emit trackerTierEditFinished(hash); },
        [this, hash](const QString &) { emit trackerTierEditFinished(hash); });
}

void RemoteSession::setTrackerTiers(const QString &hash, const QJsonObject &tiers)
{
    static const QRegularExpression validHash(
        QStringLiteral("^(?:[a-fA-F0-9]{40}|[a-fA-F0-9]{64})$"));
    const auto reject = [this, hash](const QString &reason) {
        emit trackerTierEditFinished(hash);
        emit operationFailed(tr("Change tracker tiers"), reason);
    };
    if (!supportsTrackerTierEditing())
    {
        reject(m_capabilities.unavailableReason(ServerCapabilities::Feature::TrackerTierEditing));
        return;
    }
    if (!validHash.match(hash).hasMatch() || tiers.isEmpty())
    {
        reject(tr("Tracker tier editing is unavailable or the selection is invalid."));
        return;
    }
    for (auto it = tiers.constBegin(); it != tiers.constEnd(); ++it)
    {
        const int tier = it.value().toInt(-1);
        if (!validEditableTrackerUrl(it.key()) || tier < 0 || tier > 255)
        {
            reject(tr("Invalid tracker URL or tier."));
            return;
        }
    }

    // The Web API edits one tracker per request; it has no atomic batch
    // endpoint. Retain the explicit hash/URL, serialize with the existing
    // queue, report partial failure, and refresh only after the whole batch.
    struct Batch { qsizetype remaining; qsizetype failed = 0; };
    auto batch = std::make_shared<Batch>(Batch{tiers.size()});
    const auto finish = [this, hash, batch](bool ok) {
        if (!ok) ++batch->failed;
        if (--batch->remaining != 0) return;
        emit torrentTrackersChanged(hash);
        emit trackerTierEditFinished(hash);
        if (batch->failed)
            emit operationFailed(tr("Change tracker tiers"),
                tr("%1 tracker edit(s) failed. The server's current tiers will be refreshed.")
                    .arg(batch->failed));
        else
            emit operationSucceeded(tr("Change tracker tiers"));
    };
    for (auto it = tiers.constBegin(); it != tiers.constEnd(); ++it)
    {
        // Encode each value exactly once, including literal plus signs and
        // existing percent escapes in private tracker passkeys.
        const QByteArray body = QByteArrayLiteral("hash=") + QUrl::toPercentEncoding(hash)
            + QByteArrayLiteral("&url=") + QUrl::toPercentEncoding(it.key())
            + QByteArrayLiteral("&tier=") + QByteArray::number(it.value().toInt());
        queueOperation(QStringLiteral("api/v2/torrents/editTracker"), body,
            QByteArrayLiteral("application/x-www-form-urlencoded"),
            tr("Change tracker tiers"), true,
            [finish]() { finish(true); },
            [finish](const QString &) { finish(false); }, false);
    }
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
            if (!supportsTrackerUrlEditing())
            {
                emit operationFailed(tr("Replace private torrent tracker"),
                    m_capabilities.unavailableReason(ServerCapabilities::Feature::TrackerUrlEditing));
                return;
            }
            const auto syntax = m_capabilities.trackerEditSyntax();
            queueOperation(QStringLiteral("api/v2/torrents/editTracker"),
                trackerUrlEditBody(syntax, hash, removed, added),
                QByteArrayLiteral("application/x-www-form-urlencoded"),
                tr("Replace private torrent tracker"), true,
                [this, hash, existing, removed, added, removeThenAdd, syntax]() {
                    QStringList currentlyOnServer = existing;
                    currentlyOnServer.replace(currentlyOnServer.indexOf(removed), added);
                    const auto undoEdit = [this, hash, removed, added, syntax]() {
                        queueOperation(QStringLiteral("api/v2/torrents/editTracker"),
                            trackerUrlEditBody(syntax, hash, added, removed),
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
                                    const AddTorrentOptions &options,
                                    std::function<void()> accepted, PreferencesError rejected)
{
    const QString description = files.size() == 1
        ? tr("Add torrent file") : tr("Add %1 torrent files").arg(files.size());
    // Bound the total request before allocating the multipart body. Metadata
    // uploads only; never upload downloaded content or local mount paths.
    constexpr qint64 maxSingleBytes = 32LL * 1024 * 1024;
    constexpr qint64 maxBatchBytes = 128LL * 1024 * 1024;
    if (files.isEmpty() || files.size() > 256)
    {
        const QString reason = tr("Select between 1 and 256 torrent files.");
        if (rejected) rejected(reason);
        else emit operationFailed(description, reason);
        return;
    }
    qint64 total = 0;
    for (const TorrentUpload &file : files)
    {
        if (file.contents.isEmpty() || file.contents.size() > maxSingleBytes
            || total > maxBatchBytes - file.contents.size())
        {
            const QString reason = tr("Torrent metadata must be nonempty, at most 32 MiB each and 128 MiB total.");
            if (rejected) rejected(reason);
            else emit operationFailed(description, reason);
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
    const bool reportOperation = !rejected;
    queueOperation(QStringLiteral("api/v2/torrents/add"), body,
                   QByteArrayLiteral("multipart/form-data; boundary=") + boundary,
                   description, true, std::move(accepted), std::move(rejected), reportOperation);
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
    m_capabilities = {};
    emit disconnected();
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
    m_capabilities = {};
    emit failed(reason);
}

} // namespace Remote
