// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QUrl>
#include <QString>

#include <optional>

namespace Remote
{

// Server base URL used to construct qBittorrent Web API requests.
// A path prefix is retained for reverse-proxy installations.
class ServerAddress final
{
public:
    static std::optional<ServerAddress> parse(const QString &input);

    const QUrl &baseUrl() const noexcept;
    QUrl apiUrl(const QString &endpoint) const;

private:
    explicit ServerAddress(QUrl baseUrl);

    QUrl m_baseUrl;
};

} // namespace Remote
