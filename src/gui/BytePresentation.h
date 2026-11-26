// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QLocale>
#include <QString>
#include <cmath>

namespace BytePresentation
{
// Application-wide desktop presentation only, accessed on the GUI thread.
// Do not use this for server settings, input conversion or raw sort values.
inline bool decimalUnits = false;

inline QString format(double bytes, int precision = 1, bool perSecond = false, int maxUnit = 6)
{
    if (bytes < 0 || !std::isfinite(bytes)) return QStringLiteral("—");
    static constexpr const char *binary[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
    static constexpr const char *decimal[] = {"B", "KB", "MB", "GB", "TB", "PB", "EB"};
    const double base = decimalUnits ? 1000.0 : 1024.0;
    int unit = 0;
    while (bytes >= base && unit < maxUnit && unit < 6)
    {
        bytes /= base;
        ++unit;
    }
    return QLocale().toString(bytes, 'f', unit == 0 ? 0 : precision)
        + QLatin1Char(' ') + QString::fromLatin1((decimalUnits ? decimal : binary)[unit])
        + (perSecond ? QStringLiteral("/s") : QString());
}

inline QString dataSize(qint64 bytes)
{
    return format(static_cast<double>(bytes), 2);
}
}
