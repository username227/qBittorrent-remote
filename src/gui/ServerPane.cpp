// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/ServerPane.h"
#include "gui/FilterSidebar.h"
#include "gui/TorrentModel.h"
#include "gui/upstream/proptabbar.h"
#include "gui/upstream/transferlistdelegate.h"

#include <QAbstractItemView>
#include <QAction>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLocale>
#include <QMenu>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableView>
#include <QVBoxLayout>

ServerPane::ServerPane(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *outer = new QSplitter(Qt::Horizontal, this);
    outer->setChildrenCollapsible(false);
    layout->addWidget(outer);

    m_sidebar = new FilterSidebar(outer);
    m_sidebar->setMinimumWidth(185);
    m_sidebar->setMaximumWidth(320);

    auto *vertical = new QSplitter(Qt::Vertical, outer);
    vertical->setChildrenCollapsible(false);

    m_table = new QTableView(vertical);
    m_table->setObjectName(QStringLiteral("torrentList"));
    m_model = new TorrentModel(m_table);
    m_proxy = new QSortFilterProxyModel(m_table);
    m_proxy->setSourceModel(m_model);
    m_proxy->setSortRole(Qt::UserRole);
    m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_proxy->setFilterKeyColumn(0);
    m_table->setModel(m_proxy);
    m_table->setItemDelegate(new TransferListDelegate(m_table));
    m_table->setAlternatingRowColors(true);
    m_table->setShowGrid(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setSortingEnabled(true);
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->verticalHeader()->hide();

    for (int col = 13; col < m_model->columnCount(); ++col)
        m_table->hideColumn(col);

    auto *header = m_table->horizontalHeader();
    header->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(header, &QWidget::customContextMenuRequested, m_table,
        [this, header](const QPoint &position)
        {
            QMenu menu(header);
            for (int column = 0; column < m_model->columnCount(); ++column)
            {
                QAction *action = menu.addAction(
                    m_model->headerData(column, Qt::Horizontal).toString());
                action->setCheckable(true);
                action->setChecked(!m_table->isColumnHidden(column));
                if (column == 0)
                    action->setEnabled(false);
                connect(action, &QAction::toggled, m_table,
                    [this, column](bool checked) {
                        m_table->setColumnHidden(column, !checked);
                    });
            }
            menu.exec(header->mapToGlobal(position));
        });

    auto *details = new QWidget(vertical);
    auto *detailsLayout = new QVBoxLayout(details);
    detailsLayout->setContentsMargins(0, 0, 0, 0);
    detailsLayout->setSpacing(0);

    auto *tabBar = new PropTabBar;
    detailsLayout->addLayout(tabBar);
    auto *propertyPages = new QStackedWidget(details);
    for (int i = 0; i < 6; ++i)
        propertyPages->addWidget(new QWidget(propertyPages));
    detailsLayout->addWidget(propertyPages);

    connect(tabBar, &PropTabBar::tabChanged,
            propertyPages, &QStackedWidget::setCurrentIndex);
    connect(tabBar, &PropTabBar::visibilityToggled,
            propertyPages, &QWidget::setVisible);
    tabBar->setCurrentIndex(0);

    vertical->addWidget(m_table);
    vertical->addWidget(details);
    vertical->setStretchFactor(0, 3);
    vertical->setStretchFactor(1, 0);
    vertical->setSizes({650, 75});

    outer->addWidget(m_sidebar);
    outer->addWidget(vertical);
    outer->setStretchFactor(0, 0);
    outer->setStretchFactor(1, 1);
}

void ServerPane::applySync(const QJsonObject &payload)
{
    if (payload.value(QStringLiteral("full_update")).toBool())
        m_serverState = {};

    const QJsonObject changes = payload.value(QStringLiteral("server_state")).toObject();
    for (auto it = changes.constBegin(); it != changes.constEnd(); ++it)
        m_serverState.insert(it.key(), it.value());

    m_model->applySync(payload);
    m_sidebar->updateCounts(m_model->torrents());
}

QString ServerPane::rateText() const
{
    const auto rate = [](double bytes) {
        if (bytes < 0)
            return QStringLiteral("—");
        static const char *units[] = {"B/s", "KiB/s", "MiB/s", "GiB/s"};
        int unit = 0;
        while (bytes >= 1024 && unit < 3)
        {
            bytes /= 1024;
            ++unit;
        }
        return QLocale().toString(bytes, 'f', unit == 0 ? 0 : 1)
            + QLatin1Char(' ') + QString::fromLatin1(units[unit]);
    };

    const double down = m_serverState.value(QStringLiteral("dl_info_speed")).toDouble(-1);
    const double up = m_serverState.value(QStringLiteral("up_info_speed")).toDouble(-1);
    return tr("D: %1    U: %2").arg(rate(down), rate(up));
}

void ServerPane::setSearchTerm(const QString &text)
{
    m_search = text;
    m_proxy->setFilterFixedString(text);
}

void ServerPane::setSearchColumn(int index)
{
    m_searchColumn = index;
    m_proxy->setFilterKeyColumn(index == 1 ? 25 : index == 2 ? 32 : 0);
}
