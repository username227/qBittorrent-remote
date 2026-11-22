// SPDX-License-Identifier: GPL-3.0-or-later
#include "api/RemoteSession.h"

#include <QNetworkCookieJar>
#include <QNetworkRequest>
#include <QUrlQuery>

#include <functional>

namespace Remote
{

RemoteSession::RemoteSession(QObject *parent)
    : QObject(parent)
    , m_network(this)
{
    // Do not share cookie jars or authentication between server instances.
    m_network.setCookieJar(new QNetworkCookieJar(&m_network));
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

void RemoteSession::connectToServer(const ServerProfile &profile, const QString &password)
{
    if (m_pendingReply || m_connected)
    {
        fail(tr("This connection is already active."));
        return;
    }

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
    // The password is not retained in this object or in QSettings.

    QNetworkReply *reply = m_network.post(request, body);
    m_pendingReply = reply;
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

void RemoteSession::checkReply(QNetworkReply *reply,
                               const std::function<void(const QByteArray &)> &onSuccess)
{
    if (m_pendingReply != reply)
    {
        reply->deleteLater();
        return;
    }
    m_pendingReply.clear();

    const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto redirect = reply->attribute(QNetworkRequest::RedirectionTargetAttribute);
    const QNetworkReply::NetworkError error = reply->error();
    const QString errorText = reply->errorString();
    const QByteArray response = reply->readAll();
    reply->deleteLater();

    if (redirect.isValid())
    {
        fail(tr("The server redirected the API request. Redirects are not followed for security."));
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
            fail(tr("TLS certificate verification failed. Insecure HTTPS is not allowed."));
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
    m_pendingReply = reply;
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
    QNetworkReply *reply = m_network.get(requestFor(QStringLiteral("api/v2/app/webapiVersion")));
    m_pendingReply = reply;
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
        });
    });
}

bool RemoteSession::isConnected() const noexcept
{
    return m_connected;
}

void RemoteSession::disconnectFromServer()
{
    // The visible UI disconnects immediately. Best-effort remote logout
    // completes asynchronously so the server session cookie is invalidated
    // without freezing the Qt event loop.
    if (!m_connected || !m_address)
    {
        deleteLater();
        return;
    }
    m_connected = false;
    QNetworkReply *reply = m_network.post(requestFor(QStringLiteral("api/v2/auth/logout")), QByteArray());
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        deleteLater();
    });
}

void RemoteSession::fail(const QString &reason)
{
    m_connected = false;
    emit failed(reason);
}

} // namespace Remote
