// SPDX-License-Identifier: GPL-3.0-or-later
#include "api/ServerCapabilities.h"

#include <QRegularExpression>

namespace Remote
{

ServerCapabilities ServerCapabilities::fromVersions(const QString &daemon, const QString &api)
{
    ServerCapabilities result;
    result.m_daemonText = daemon.trimmed();
    result.m_apiText = api.trimmed();
    QString numeric = result.m_daemonText;
    if (numeric.startsWith(QLatin1Char('v')))
        numeric.remove(0, 1);
    // Preserve the existing daemon-prefix behavior for packaged builds.
    result.m_daemon = QVersionNumber::fromString(numeric);
    // Accept historical two-component API versions (2.0) without treating
    // arbitrary suffixes or partial numbers as evidence of a capability.
    static const QRegularExpression apiPattern(
        QStringLiteral("^\\d+\\.\\d+(?:\\.\\d+)?$"));
    if (apiPattern.match(result.m_apiText).hasMatch())
    {
        auto version = QVersionNumber::fromString(result.m_apiText);
        if (version.segmentCount() == 2)
            version = QVersionNumber(version.majorVersion(), version.minorVersion(), 0);
        result.m_api = version;
    }
    return result;
}

bool ServerCapabilities::daemonAtLeast(int major, int minor, int patch) const
{
    return m_daemon.segmentCount() >= 3 && m_daemon >= QVersionNumber(major, minor, patch);
}

bool ServerCapabilities::apiAtLeast(int major, int minor, int patch) const
{
    return m_api.segmentCount() == 3 && m_api >= QVersionNumber(major, minor, patch);
}

} // namespace Remote
