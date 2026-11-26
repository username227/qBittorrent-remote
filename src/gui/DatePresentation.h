// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDateTime>
#include <QLocale>
#include <QStringList>

namespace DatePresentation
{
// Equivalent Qt patterns for upstream WebUI's date-format choices.
inline const QStringList &patterns()
{
    static const QStringList values{QString(), QStringLiteral("MM/dd/yyyy, h:mm:ss AP"),
        QStringLiteral("MM/dd/yyyy, HH:mm:ss"), QStringLiteral("dd/MM/yyyy, HH:mm:ss"),
        QStringLiteral("yyyy-MM-dd HH:mm:ss"), QStringLiteral("yyyy/MM/dd HH:mm:ss"),
        QStringLiteral("dd.MM.yyyy, HH:mm:ss"), QStringLiteral("MMM dd, yyyy, h:mm:ss AP"),
        QStringLiteral("dd MMM yyyy, HH:mm:ss")};
    return values;
}

inline QString format(qint64 seconds, const QString &pattern)
{
    if (seconds <= 0) return QStringLiteral("—");
    const auto date = QDateTime::fromSecsSinceEpoch(seconds);
    return patterns().contains(pattern) && !pattern.isEmpty()
        ? QLocale().toString(date, pattern) : QLocale().toString(date, QLocale::ShortFormat);
}
}
