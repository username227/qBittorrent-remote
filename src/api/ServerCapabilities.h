// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <QVersionNumber>

namespace Remote
{

// Immutable-by-interface snapshot of one authenticated server's reported versions.
// Capability predicates are centralized here; protocol adapters remain in RemoteSession.
class ServerCapabilities final
{
public:
    static ServerCapabilities fromVersions(const QString &daemon, const QString &api);

    const QString &daemonVersion() const noexcept { return m_daemonText; }
    const QString &apiVersion() const noexcept { return m_apiText; }

    bool trackerTierEditing() const { return apiAtLeast(2, 13, 0); }
    bool pathFreeSpace() const { return apiAtLeast(2, 16, 0); }
    bool pieceAvailability() const { return daemonAtLeast(5, 2, 0); }
    bool commentEditing() const { return daemonAtLeast(5, 2, 0); }
    bool selectedTrackerReannounce() const { return daemonAtLeast(5, 2, 0); }
    bool advancedModernPreferences() const { return daemonAtLeast(5, 3, 0); }
    bool banRanges() const { return daemonAtLeast(5, 3, 0); }

private:
    bool daemonAtLeast(int major, int minor, int patch) const;
    bool apiAtLeast(int major, int minor, int patch) const;

    QString m_daemonText;
    QString m_apiText;
    QVersionNumber m_daemon;
    QVersionNumber m_api;
};

} // namespace Remote
