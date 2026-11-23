/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2025  Vladimir Golovnev <glassez@yandex.ru>
 * Copyright (C) 2006  Christophe Dumez <chris@qbittorrent.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * In addition, as a special exception, the copyright holders give permission to
 * link this program with the OpenSSL project's "OpenSSL" library (or with
 * modified versions of it that use the same license as the "OpenSSL" library),
 * and distribute the linked executables. You must obey the GNU General Public
 * License in all respects for all of the code used other than "OpenSSL".  If you
 * modify file(s), you may extend this exception to your version of the file(s),
 * but you are not obligated to do so. If you do not wish to do so, delete this
 * exception statement from your version.
 */

#include "gui/upstream/transferlistdelegate.h"

#include <QModelIndex>
#include <QtMath>

#include <algorithm>

TransferListDelegate::TransferListDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{
}

QSize TransferListDelegate::sizeHint(const QStyleOptionViewItem &option,
                                     const QModelIndex &index) const
{
    // Preserve the upstream row height when the icon-bearing Name column
    // scrolls offscreen.
    if (m_nameColHeight == -1)
    {
        const QModelIndex nameIndex = index.siblingAtColumn(0);
        m_nameColHeight = QStyledItemDelegate::sizeHint(option, nameIndex).height();
    }
    QSize size = QStyledItemDelegate::sizeHint(option, index);
    size.setHeight(std::max(m_nameColHeight, size.height()));
    return size;
}

void TransferListDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                                 const QModelIndex &index) const
{
    if (index.column() != 2) // Progress
    {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    // sync/maindata progress is a fraction [0, 1]; native qBittorrent's
    // painter takes an integer [0, 100]. Never infer local torrent state.
    const int progress = qBound(0, qRound(index.data(Qt::UserRole).toDouble() * 100.0), 100);
    const QString state = index.siblingAtColumn(3).data(Qt::UserRole).toString();
    const bool enabled = !state.isEmpty()
        && state != QLatin1String("error")
        && state != QLatin1String("missingFiles")
        && state != QLatin1String("stoppedDL");

    QStyleOptionViewItem customOption {option};
    customOption.state.setFlag(QStyle::State_Enabled, enabled);
    const QColor stateColor = m_progressFollowsTextColor
        ? index.data(Qt::ForegroundRole).value<QColor>() : QColor();
    m_progressBarPainter.paint(painter, customOption,
                               index.data().toString(), progress, stateColor);
}
