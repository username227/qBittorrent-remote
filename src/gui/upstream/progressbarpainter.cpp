/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2025  Vladimir Golovnev <glassez@yandex.ru>
 * Copyright (C) 2020  Prince Gupta <jagannatharjun11@gmail.com>
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

#include "gui/upstream/progressbarpainter.h"

#include <QPainter>
#include <QPalette>
#include <QStyle>
#include <QStyleOptionProgressBar>
#include <QStyleOptionViewItem>

#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
#include <QProxyStyle>
#endif

ProgressBarPainter::ProgressBarPainter()
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    // Match qBittorrent's platform-independent progress bar on Win/macOS.
    auto *fusionStyle = new QProxyStyle(QStringLiteral("fusion"));
    fusionStyle->setParent(&m_dummyProgressBar);
    m_dummyProgressBar.setStyle(fusionStyle);
#endif
}

void ProgressBarPainter::paint(QPainter *painter, const QStyleOptionViewItem &option,
                               const QString &text, int progress, const QColor &color) const
{
    QStyleOptionProgressBar styleOption;
    styleOption.initFrom(&m_dummyProgressBar);
    styleOption.minimum = 0;
    styleOption.maximum = 100;
    styleOption.progress = progress;
    styleOption.text = text;
    styleOption.textVisible = true;
    styleOption.rect = option.rect.adjusted(0, 1, 0, -1);
    // Required by Qt 6 for horizontal native progress bars.
    styleOption.state = option.state | QStyle::State_Horizontal;
    styleOption.palette = option.palette;
    styleOption.palette.setCurrentColorGroup(
        option.state.testFlag(QStyle::State_Enabled) ? QPalette::Active : QPalette::Disabled);
    if (color.isValid())
        styleOption.palette.setColor(QPalette::Highlight, color);

    painter->save();
    const QStyle *style = m_dummyProgressBar.style();
    style->drawPrimitive(QStyle::PE_PanelItemViewItem, &option, painter, option.widget);
    style->drawControl(QStyle::CE_ProgressBar, &styleOption, painter, &m_dummyProgressBar);
    painter->restore();
}
