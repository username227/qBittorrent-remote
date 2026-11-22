// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/ServerPane.h"
#include "gui/FilterSidebar.h"
#include "gui/TorrentModel.h"
#include "gui/TorrentDetailsWidget.h"
#include "gui/upstream/proptabbar.h"
#include "gui/upstream/transferlistdelegate.h"

#include <QAbstractItemView>
#include <QAction>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLocale>
#include <QMenu>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QSplitterHandle>
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
    vertical->setObjectName(QStringLiteral("torrentDetailsSplitter"));
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
    details->setObjectName(QStringLiteral("torrentDetailsDock"));
    auto *detailsLayout = new QVBoxLayout(details);
    detailsLayout->setContentsMargins(0, 0, 0, 0);
    detailsLayout->setSpacing(0);

    // Match upstream PropertiesWidget: the information occupies the area
    // ABOVE the bottom-anchored tabs. A second click collapses the information,
    // never the persistent bottom tab row.
    m_details = new TorrentDetailsWidget(details);
    detailsLayout->addWidget(m_details, 1);
    m_detailTabs = new PropTabBar;
    m_detailTabs->setContentsMargins(0, 4, 0, 4);
    detailsLayout->addLayout(m_detailTabs);

    connect(m_detailTabs, &PropTabBar::tabChanged,
            m_details, &TorrentDetailsWidget::showTab);
    connect(m_detailTabs, &PropTabBar::tabChanged, this,
            [this](int) { requestSelectedDetails(); });
    connect(m_details, &TorrentDetailsWidget::filePriorityRequested,
            this, &ServerPane::filePriorityRequested);
    m_detailTabs->setCurrentIndex(0);

    connect(m_table->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, [this]() { updateSelectedTorrent(); });
    connect(m_model, &QAbstractItemModel::modelReset,
            this, [this]() { updateSelectedTorrent(); });
    m_detailsRefresh.setInterval(10000);
    connect(&m_detailsRefresh, &QTimer::timeout, this,
            &ServerPane::requestSelectedDetails);
    m_detailsRefresh.start();

    vertical->addWidget(m_table);
    vertical->addWidget(details);
    vertical->setStretchFactor(0, 3);
    vertical->setStretchFactor(1, 1);
    vertical->setSizes({550, 300});

    // Upstream keeps the tab bar at the very bottom when the properties
    // pane is collapsed, without wasting a large blank splitter area.
    // Preserve the exact user-adjusted splitter sizes for reopening.
    connect(m_detailTabs, &PropTabBar::visibilityToggled, this,
            [this, vertical, details, expandedSizes = QList<int>{},
             originalHandleWidth = -1](bool show) mutable {
                auto *handle = vertical->handle(1);
                if (!show)
                {
                    expandedSizes = vertical->sizes();
                    originalHandleWidth = vertical->handleWidth();
                    m_details->hide();
                    const int tabHeight = m_detailTabs->sizeHint().height()
                        + m_detailTabs->contentsMargins().top()
                        + m_detailTabs->contentsMargins().bottom();
                    details->setMaximumHeight(tabHeight);
                    vertical->setSizes({qMax(0, vertical->height() - tabHeight), tabHeight});
                    if (handle)
                        handle->setVisible(false);
                    vertical->setHandleWidth(0);
                }
                else
                {
                    details->setMaximumHeight(QWIDGETSIZE_MAX);
                    if (originalHandleWidth >= 0)
                        vertical->setHandleWidth(originalHandleWidth);
                    if (handle)
                        handle->setVisible(true);
                    m_details->show();
                    if (!expandedSizes.isEmpty())
                        vertical->setSizes(expandedSizes);
                }
            });

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
    m_details->appendRates(
        m_serverState.value(QStringLiteral("dl_info_speed")).toDouble(),
        m_serverState.value(QStringLiteral("up_info_speed")).toDouble());
    if (!m_selectedHash.isEmpty())
    {
        if (m_model->torrents().contains(m_selectedHash))
            m_details->setSelection(m_selectedHash, m_model->torrents().value(m_selectedHash));
        else
            updateSelectedTorrent();
    }
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

void ServerPane::updateSelectedTorrent()
{
    const auto selected = m_table->selectionModel()->selectedRows();
    QString hash;
    if (selected.size() == 1)
        hash = m_model->hashForRow(m_proxy->mapToSource(selected.first()).row());
    if (hash == m_selectedHash)
        return;
    m_selectedHash = hash;
    m_details->setSelection(hash, m_model->torrents().value(hash));
    requestSelectedDetails();
}

void ServerPane::requestSelectedDetails()
{
    if (!m_selectedHash.isEmpty() && m_details->isVisible()
        && m_details->currentTab() >= 0 && m_details->currentTab() < 5)
        emit torrentDetailsRequested(m_selectedHash, m_details->currentTab());
}

void ServerPane::refreshSelectedDetails()
{
    requestSelectedDetails();
}

void ServerPane::displayTorrentDetails(const QString &hash, int tab, const QJsonValue &data)
{
    m_details->displayDetails(hash, tab, data);
}

void ServerPane::displayTorrentDetailsError(const QString &hash, int tab,
                                            const QString &reason)
{
    m_details->displayError(hash, tab, reason);
}
