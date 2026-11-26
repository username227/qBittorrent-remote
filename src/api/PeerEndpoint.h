// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QHostAddress>
#include <QRegularExpression>
#include <QStringList>
namespace Remote
{
inline QString normalizedPeerEndpoint(const QString &input)
{
    static const QRegularExpression syntax(QStringLiteral(
        R"(^(?:\[([^\]]+)\]|([^:]+)):([0-9]{1,5})$)"));
    const auto match = syntax.match(input.trimmed());
    if (!match.hasMatch()) return {};
    const bool bracketed = !match.captured(1).isEmpty();
    QHostAddress ip;
    if (!ip.setAddress(bracketed ? match.captured(1) : match.captured(2))
        || ip.isNull() || !ip.scopeId().isEmpty()
        || (bracketed && ip.protocol() != QAbstractSocket::IPv6Protocol)
        || (!bracketed && ip.protocol() != QAbstractSocket::IPv4Protocol))
        return {};
    bool ok = false;
    const auto port = match.captured(3).toUInt(&ok);
    if (!ok || port == 0 || port > 65535) return {};
    return (bracketed ? QStringLiteral("[%1]").arg(ip.toString()) : ip.toString())
        + QLatin1Char(':') + QString::number(port);
}
inline QStringList normalizedPeerEndpoints(const QStringList &inputs)
{
    QStringList result;
    for (const auto &input : inputs)
    {
        const auto endpoint = normalizedPeerEndpoint(input);
        if (endpoint.isEmpty()) return {};
        if (!result.contains(endpoint)) result.append(endpoint);
    }
    return result;
}
}
