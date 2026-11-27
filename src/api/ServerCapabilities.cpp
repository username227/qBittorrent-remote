// SPDX-License-Identifier: GPL-3.0-or-later
#include "api/ServerCapabilities.h"
#include "KnownServerReleases.h"

#include <QCoreApplication>
#include <QRegularExpression>
#include <QStringList>

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

    // Only exact identities from the stable release lock are recognized.
    // Do not silently interpret a prerelease or a suffix as a stable tag.
    for (const auto *release : KnownServerReleases)
    {
        if (numeric == QLatin1String(release))
        {
            result.m_daemon = QVersionNumber::fromString(numeric);
            result.m_knownRelease = true;
            break;
        }
    }

    // 4.1.0 reports "2.0". Normalize numeric components, retain wire text.
    static const QRegularExpression apiPattern(
        QStringLiteral("\\A[0-9]+\\.[0-9]+(?:\\.[0-9]+)?\\z"));
    if (apiPattern.match(result.m_apiText).hasMatch())
    {
        const auto parts = result.m_apiText.split(QLatin1Char('.'));
        bool majorOk = false, minorOk = false, patchOk = true;
        const int major = parts.at(0).toInt(&majorOk);
        const int minor = parts.at(1).toInt(&minorOk);
        const int patch = parts.size() == 3 ? parts.at(2).toInt(&patchOk) : 0;
        if (majorOk && minorOk && patchOk)
            result.m_api = QVersionNumber(major, minor, patch);
    }
    return result;
}

ServerCapabilities::Status ServerCapabilities::sinceDaemon(int major, int minor, int patch) const noexcept
{
    if (!m_knownRelease || !hasWebApiV2())
        return Status::Unknown;
    return m_daemon >= QVersionNumber(major, minor, patch)
        ? Status::Supported : Status::Unsupported;
}

ServerCapabilities::Status ServerCapabilities::sinceDaemonAndApi(
    int major, int minor, int patch, int apiMinor, int apiPatch) const noexcept
{
    const auto daemon = sinceDaemon(major, minor, patch);
    if (daemon == Status::Unknown)
        return daemon;
    const bool apiMatches = m_api >= QVersionNumber(2, apiMinor, apiPatch);
    // Conflicting daemon/API evidence is not proof of presence or absence.
    if ((daemon == Status::Supported) != apiMatches)
        return Status::Unknown;
    return daemon;
}

ServerCapabilities::TrackerEditSyntax ServerCapabilities::trackerEditSyntax() const noexcept
{
    switch (sinceDaemon(4, 1, 5))
    {
    case Status::Unsupported: return TrackerEditSyntax::Unsupported;
    case Status::Unknown: return TrackerEditSyntax::Unknown;
    case Status::Supported: break;
    }
    // The URL parameter change and selected reannounce are independent
    // contracts, even though both changed in stable 5.2.0.
    switch (sinceDaemonAndApi(5, 2, 0, 13, 0))
    {
    case Status::Supported: return TrackerEditSyntax::ModernUrl;
    case Status::Unsupported: return TrackerEditSyntax::LegacyOrigUrl;
    case Status::Unknown: return TrackerEditSyntax::Unknown;
    }
    return TrackerEditSyntax::Unknown;
}

ServerCapabilities::Status ServerCapabilities::status(Feature feature) const noexcept
{
    switch (feature)
    {
    case Feature::TrackerUrlEditing:
        switch (trackerEditSyntax())
        {
        case TrackerEditSyntax::LegacyOrigUrl:
        case TrackerEditSyntax::ModernUrl: return Status::Supported;
        case TrackerEditSyntax::Unsupported: return Status::Unsupported;
        case TrackerEditSyntax::Unknown: return Status::Unknown;
        }
        break;
    case Feature::TrackerTierEditing: return sinceDaemonAndApi(5, 2, 0, 13, 0);
    case Feature::PathFreeSpace: return sinceDaemonAndApi(5, 3, 0, 16, 0);
    case Feature::SelectedTrackerReannounce:
    case Feature::PieceAvailability:
    case Feature::CommentEditing:
        return sinceDaemon(5, 2, 0);
    // Preserve existing preferences policy; its individual setting/build
    // contracts still require the separate preferences audit.
    case Feature::AdvancedModernPreferences:
    case Feature::BanRanges:
        return sinceDaemon(5, 3, 0);
    }
    return Status::Unknown;
}

QString ServerCapabilities::unavailableReason(Feature feature) const
{
    switch (status(feature))
    {
    case Status::Supported: return {};
    case Status::Unsupported:
        return QCoreApplication::translate("ServerCapabilities",
            "This operation is not supported by qBittorrent %1.").arg(m_daemonText);
    case Status::Unknown:
        return QCoreApplication::translate("ServerCapabilities",
            "Compatibility is unknown for this operation on qBittorrent %1 (Web API %2).")
            .arg(m_daemonText.isEmpty() ? QStringLiteral("?") : m_daemonText,
                 m_apiText.isEmpty() ? QStringLiteral("?") : m_apiText);
    }
    return {};
}

QString ServerCapabilities::versionWarning() const
{
    if (!m_knownRelease || !hasWebApiV2())
        return QCoreApplication::translate("ServerCapabilities",
            "Unrecognized stable release or Web API version; some operations are unavailable.");
    if (status(Feature::TrackerTierEditing) == Status::Unknown
        || status(Feature::PathFreeSpace) == Status::Unknown)
        return QCoreApplication::translate("ServerCapabilities",
            "Daemon and Web API versions disagree on some capabilities; affected operations are unavailable.");
    return {};
}

} // namespace Remote
