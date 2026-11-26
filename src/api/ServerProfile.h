// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QSettings>
#include <QString>

namespace Remote
{

struct ServerProfile
{
    QString id;
    QString name;
    QString url;
    QString username;
    bool rememberPassword = false;
    // Opt-in only: saved servers do not connect automatically by default.
    bool autoConnect = false;
};

// Only nonsecret profile metadata is persisted. Passwords and session
// cookies always remain outside QSettings.
class ProfileStore final
{
public:
    explicit ProfileStore(QSettings &settings);

    QList<ServerProfile> profiles() const;
    bool save(ServerProfile &profile);
    // Only for an explicitly authorized and freshly verified same-host WebUI transition.
    bool saveVerifiedWebUITransition(ServerProfile &profile, const ServerProfile &previous);
    bool remove(const QString &id);

    // Trust is scoped to both the saved profile ID and its exact normalized
    // URL. Changes to the server endpoint never inherit the previous pin.
    QString trustedCertificateFingerprint(const ServerProfile &profile) const;
    bool trustCertificate(const ServerProfile &profile, const QString &sha256);

private:
    bool write(const QList<ServerProfile> &profiles);
    bool saveImpl(ServerProfile &profile, bool preservePaths);

    QSettings &m_settings;
};

} // namespace Remote
