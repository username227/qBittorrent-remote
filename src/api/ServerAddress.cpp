// SPDX-License-Identifier: GPL-3.0-or-later
#include "api/ServerAddress.h"

#include <utility>

namespace Remote
{

ServerAddress::ServerAddress(QUrl baseUrl)
    : m_baseUrl(std::move(baseUrl))
{
}

std::optional<ServerAddress> ServerAddress::parse(const QString &input)
{
    // Require an explicit HTTP(S) scheme: never silently choose HTTP.
    QUrl url(input.trimmed(), QUrl::StrictMode);

    if (!url.isValid() || url.host().isEmpty()
        || (url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https"))
        || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()
        || url.port(-1) == 0)
    {
        return std::nullopt;
    }

    QString basePath = url.path();
    if (!basePath.endsWith(QLatin1Char('/')))
        basePath += QLatin1Char('/');
    url.setPath(basePath);

    return ServerAddress(std::move(url));
}

const QUrl &ServerAddress::baseUrl() const noexcept
{
    return m_baseUrl;
}

QUrl ServerAddress::apiUrl(const QString &endpoint) const
{
    QString path = endpoint;
    while (path.startsWith(QLatin1Char('/')))
        path.remove(0, 1);

    // A relative URL preserves the reverse proxy's path prefix.
    return m_baseUrl.resolved(QUrl(path));
}

} // namespace Remote
