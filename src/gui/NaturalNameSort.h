#pragma once

// The same deterministic, case-insensitive natural sorting is used by
// the torrent list and the Content/Peers/Trackers detail tables.
#include <QString>
#include <QChar>

namespace RemoteGuiSort
{
// QCollator's numeric mode depends on Qt's platform collation backend;
// some Linux builds silently sort "10" before "2". Compare digit runs
// ourselves so torrent-name ordering stays consistent on Windows and Linux.
// Compare digit sequences without converting to integers, avoiding overflow.
inline int naturalNameCompare(const QString &left, const QString &right)
{
    const bool leftBracket = left.startsWith(QLatin1Char('('))
                          || left.startsWith(QLatin1Char('['));
    const bool rightBracket = right.startsWith(QLatin1Char('('))
                           || right.startsWith(QLatin1Char('['));
    if (leftBracket != rightBracket)
        return leftBracket ? -1 : 1;

    qsizetype i = 0;
    qsizetype j = 0;
    while (i < left.size() && j < right.size())
    {
        const QChar l = left.at(i);
        const QChar r = right.at(j);
        if (l.isDigit() && r.isDigit())
        {
            const qsizetype startL = i;
            const qsizetype startR = j;
            while (i < left.size() && left.at(i).isDigit()) ++i;
            while (j < right.size() && right.at(j).isDigit()) ++j;

            // Compare significant digit counts and values, not lexicographic
            // digit strings. Supports arbitrarily long numeric suffixes.
            qsizetype sigL = startL;
            qsizetype sigR = startR;
            while (sigL < i && left.at(sigL).digitValue() == 0) ++sigL;
            while (sigR < j && right.at(sigR).digitValue() == 0) ++sigR;
            const qsizetype countL = i - sigL;
            const qsizetype countR = j - sigR;
            if (countL != countR)
                return countL < countR ? -1 : 1;
            for (qsizetype n = 0; n < countL; ++n)
            {
                const int digitL = left.at(sigL + n).digitValue();
                const int digitR = right.at(sigR + n).digitValue();
                if (digitL != digitR)
                    return digitL < digitR ? -1 : 1;
            }

            // Equal numeric values: put "2" before "02".
            const qsizetype lengthL = i - startL;
            const qsizetype lengthR = j - startR;
            if (lengthL != lengthR)
                return lengthL < lengthR ? -1 : 1;
            continue;
        }

        const QChar foldedL = l.toCaseFolded();
        const QChar foldedR = r.toCaseFolded();
        if (foldedL != foldedR)
            return foldedL < foldedR ? -1 : 1;
        ++i;
        ++j;
    }
    if (i == left.size() && j == right.size()) return 0;
    return i == left.size() ? -1 : 1;
}

} // namespace RemoteGuiSort
