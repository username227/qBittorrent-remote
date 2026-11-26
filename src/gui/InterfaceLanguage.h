// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QLocale>
#include <QSettings>
#include <QTranslator>
namespace InterfaceLanguage
{
inline QString selection(const QSettings &settings)
{
    const auto value = settings.value(QStringLiteral("Appearance/Language"),
        QStringLiteral("en")).toString();
    return (value == QLatin1String("system") || value == QLatin1String("de"))
        ? value : QStringLiteral("en");
}
inline bool load(QTranslator &translator, const QSettings &settings)
{
    auto language = selection(settings);
    if (language == QLatin1String("system"))
    {
        language = QStringLiteral("en");
        for (const auto &name : QLocale::system().uiLanguages())
        {
            const auto candidate = QLocale(name).language();
            if (candidate == QLocale::English) break;
            if (candidate == QLocale::German)
            {
                language = QStringLiteral("de");
                break;
            }
        }
    }
    return language == QLatin1String("de")
        && translator.load(QStringLiteral(":/i18n/qbittorrent_remote_de.qm"));
}
} // namespace InterfaceLanguage
