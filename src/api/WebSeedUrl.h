// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QUrl>
#include <QStringList>

namespace Remote
{
enum class WebSeedAction { Add, Edit, Remove };

inline bool validWebSeedUrl(const QString &text)
{
    if (text.isEmpty() || text != text.trimmed()) return false;
    for (const auto ch : text)
        if (ch.category() == QChar::Other_Control) return false;
    const QUrl url(text, QUrl::StrictMode);
    return url.isValid() && !url.host().isEmpty()
        && (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https")
            || url.scheme() == QLatin1String("ftp"));
}

// Upstream splits the decoded form value on '|' BEFORE explicitly
// percent-decoding each URL. Match the official WebUI's two encoding layers.
inline QByteArray encodeWebSeedUrls(const QStringList &urls)
{
    QByteArray joined;
    for (const auto &url : urls)
    {
        if (!joined.isEmpty()) joined += '|';
        joined += QUrl::toPercentEncoding(url);
    }
    return QUrl::toPercentEncoding(QString::fromLatin1(joined));
}
}
