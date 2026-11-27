// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <QVersionNumber>

namespace Remote
{

// Snapshot of one authenticated server's version evidence. Supported means
// the current gate permits an operation, not that the entire release or
// all build/configuration-dependent semantics have been verified.
class ServerCapabilities final
{
public:
    enum class Status { Unknown, Unsupported, Supported };
    enum class Feature
    {
        TrackerUrlEditing, TrackerTierEditing, SelectedTrackerReannounce,
        PieceAvailability, CommentEditing, PathFreeSpace,
        AdvancedModernPreferences, BanRanges
    };
    enum class TrackerEditSyntax { Unknown, Unsupported, LegacyOrigUrl, ModernUrl };

    static ServerCapabilities fromVersions(const QString &daemon, const QString &api);

    const QString &daemonVersion() const noexcept { return m_daemonText; }
    const QString &apiVersion() const noexcept { return m_apiText; }
    bool hasKnownRelease() const noexcept { return m_knownRelease; }
    bool hasWebApiV2() const noexcept { return m_api.majorVersion() == 2; }
    Status status(Feature feature) const noexcept;
    TrackerEditSyntax trackerEditSyntax() const noexcept;
    QString unavailableReason(Feature feature) const;
    QString versionWarning() const;

    bool trackerUrlEditing() const noexcept { return status(Feature::TrackerUrlEditing) == Status::Supported; }
    bool trackerTierEditing() const noexcept { return status(Feature::TrackerTierEditing) == Status::Supported; }
    bool pathFreeSpace() const noexcept { return status(Feature::PathFreeSpace) == Status::Supported; }
    bool pieceAvailability() const noexcept { return status(Feature::PieceAvailability) == Status::Supported; }
    bool commentEditing() const noexcept { return status(Feature::CommentEditing) == Status::Supported; }
    bool selectedTrackerReannounce() const noexcept { return status(Feature::SelectedTrackerReannounce) == Status::Supported; }
    bool advancedModernPreferences() const noexcept { return status(Feature::AdvancedModernPreferences) == Status::Supported; }
    bool banRanges() const noexcept { return status(Feature::BanRanges) == Status::Supported; }

private:
    Status sinceDaemon(int major, int minor, int patch) const noexcept;
    Status sinceDaemonAndApi(int major, int minor, int patch,
                             int apiMinor, int apiPatch) const noexcept;

    QString m_daemonText;
    QString m_apiText;
    QVersionNumber m_daemon;
    QVersionNumber m_api;
    bool m_knownRelease = false;
};

} // namespace Remote
