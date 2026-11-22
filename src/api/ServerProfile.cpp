// SPDX-License-Identifier: GPL-3.0-or-later
#include "api/ServerProfile.h"
#include "api/ServerAddress.h"

#include <QCryptographicHash>
#include <QRegularExpression>
#include <QUuid>

namespace
{
QString trustKey(const QString &id)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha256).toHex());
}
QString canonicalAddress(const Remote::ServerProfile &profile)
{
    const auto address = Remote::ServerAddress::parse(profile.url);
    return address ? address->baseUrl().toString(QUrl::FullyEncoded) : QString();
}
bool validFingerprint(const QString &s)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{64}$"));
    return pattern.match(s).hasMatch();
}
}

namespace Remote
{

ProfileStore::ProfileStore(QSettings &settings)
    : m_settings(settings)
{
}

QList<ServerProfile> ProfileStore::profiles() const
{
    QList<ServerProfile> result;
    const int count = m_settings.beginReadArray(QStringLiteral("servers"));
    for (int i = 0; i < count; ++i)
    {
        m_settings.setArrayIndex(i);
        ServerProfile p {
            m_settings.value(QStringLiteral("id")).toString(),
            m_settings.value(QStringLiteral("name")).toString(),
            m_settings.value(QStringLiteral("url")).toString(),
            m_settings.value(QStringLiteral("username")).toString()
        };
        if (!p.id.isEmpty() && !p.name.isEmpty() && ServerAddress::parse(p.url))
            result.append(p);
    }
    m_settings.endArray();
    return result;
}

bool ProfileStore::write(const QList<ServerProfile> &profiles)
{
    m_settings.remove(QStringLiteral("servers"));
    m_settings.beginWriteArray(QStringLiteral("servers"));
    for (int i = 0; i < profiles.size(); ++i)
    {
        m_settings.setArrayIndex(i);
        const ServerProfile &p = profiles.at(i);
        m_settings.setValue(QStringLiteral("id"), p.id);
        m_settings.setValue(QStringLiteral("name"), p.name);
        m_settings.setValue(QStringLiteral("url"), p.url);
        m_settings.setValue(QStringLiteral("username"), p.username);
    }
    m_settings.endArray();
    m_settings.sync();
    return m_settings.status() == QSettings::NoError;
}

bool ProfileStore::save(ServerProfile &profile)
{
    if (profile.name.trimmed().isEmpty() || !ServerAddress::parse(profile.url))
        return false;

    if (profile.id.isEmpty())
        profile.id = QUuid::createUuid().toString(QUuid::WithoutBraces);

    QList<ServerProfile> items = profiles();
    bool updated = false;
    bool addressChanged = false;
    for (ServerProfile &item : items)
    {
        if (item.id == profile.id)
        {
            addressChanged = canonicalAddress(item) != canonicalAddress(profile);
            item = profile;
            updated = true;
            break;
        }
    }
    if (!updated)
        items.append(profile);
    if (!write(items))
        return false;

    // A profile redirected to a new server must never reuse the original
    // fingerprint, even if it is subsequently edited back to the old URL.
    if (addressChanged)
    {
        m_settings.remove(QStringLiteral("certificateTrust/") + trustKey(profile.id));
        m_settings.sync();
        return m_settings.status() == QSettings::NoError;
    }
    return true;
}

bool ProfileStore::remove(const QString &id)
{
    QList<ServerProfile> items = profiles();
    for (qsizetype i = 0; i < items.size(); ++i)
    {
        if (items.at(i).id == id)
        {
            items.removeAt(i);
            if (!write(items))
                return false;
            m_settings.remove(QStringLiteral("certificateTrust/") + trustKey(id));
            m_settings.sync();
            return m_settings.status() == QSettings::NoError;
        }
    }
    return false;
}


QString ProfileStore::trustedCertificateFingerprint(const ServerProfile &profile) const
{
    const auto address = ServerAddress::parse(profile.url);
    if (profile.id.isEmpty() || !address
        || address->baseUrl().scheme() != QStringLiteral("https"))
        return {};

    const QString prefix = QStringLiteral("certificateTrust/") + trustKey(profile.id);
    if (m_settings.value(prefix + QStringLiteral("/url")).toString()
        != canonicalAddress(profile))
        return {};

    const QString sha256 = m_settings.value(prefix + QStringLiteral("/sha256")).toString();
    return validFingerprint(sha256) ? sha256 : QString();
}

bool ProfileStore::trustCertificate(const ServerProfile &profile, const QString &sha256)
{
    const auto address = ServerAddress::parse(profile.url);
    if (profile.id.isEmpty() || !address
        || address->baseUrl().scheme() != QStringLiteral("https")
        || !validFingerprint(sha256))
        return false;

    bool matchesSavedProfile = false;
    for (const ServerProfile &item : profiles())
    {
        if (item.id == profile.id
            && canonicalAddress(item) == canonicalAddress(profile))
        {
            matchesSavedProfile = true;
            break;
        }
    }
    if (!matchesSavedProfile)
        return false;

    const QString prefix = QStringLiteral("certificateTrust/") + trustKey(profile.id);
    m_settings.setValue(prefix + QStringLiteral("/url"), canonicalAddress(profile));
    m_settings.setValue(prefix + QStringLiteral("/sha256"), sha256);
    m_settings.sync();
    return m_settings.status() == QSettings::NoError;
}

} // namespace Remote
