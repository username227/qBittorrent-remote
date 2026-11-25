// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QLocale>
#include <QString>
#include <QtMath>

// Display rounding must never imply completion when the server's fraction
// is below one. Keep the raw fraction untouched for sorting and filtering.
namespace ProgressDisplay
{
inline int roundedValue(double fraction, int maximum)
{
    return qBound(0, qRound(qBound(0.0, fraction, 1.0) * maximum),
                  fraction < 1.0 ? maximum - 1 : maximum);
}

inline QString percent(double fraction, int decimals = 1)
{
    const int scale = decimals == 0 ? 1 : 10;
    return QLocale().toString(roundedValue(fraction, 100 * scale)
                                  / static_cast<double>(scale), 'f', decimals)
        + QLatin1Char('%');
}
} // namespace ProgressDisplay
