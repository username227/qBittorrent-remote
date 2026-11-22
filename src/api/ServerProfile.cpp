// SPDX-License-Identifier: GPL-3.0-or-later
#include "api/ServerProfile.h"
#include "api/ServerAddress.h"

#include <QUuid>

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
    for (ServerProfile &item : items)
    {
        if (item.id == profile.id)
        {
            item = profile;
            updated = true;
            break;
        }
    }
    if (!updated)
        items.append(profile);
    return write(items);
}

bool ProfileStore::remove(const QString &id)
{
    QList<ServerProfile> items = profiles();
    for (qsizetype i = 0; i < items.size(); ++i)
    {
        if (items.at(i).id == id)
        {
            items.removeAt(i);
            return write(items);
        }
    }
    return false;
}

} // namespace Remote
