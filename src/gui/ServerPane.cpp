// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/ServerPane.h"
#include "gui/FilterSidebar.h"
#include "gui/TorrentModel.h"
#include "gui/TorrentDetailsWidget.h"
#include "gui/upstream/proptabbar.h"
#include "gui/upstream/transferlistdelegate.h"

#include <QAbstractItemView>
#include <QAction>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QSettings>
#include <QTreeWidget>
#include <QTableWidget>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLocale>
#include <QMenu>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QSplitterHandle>
#include <QTableView>
#include <QVBoxLayout>

// One composable proxy per remote server. Filtering never mutates the remote
// torrent model, so changing selections does not affect the other servers.
namespace
{
class TorrentFilterProxyModel final : public QSortFilterProxyModel
{
public:
    explicit TorrentFilterProxyModel(FilterSidebar *sidebar, QObject *parent)
        : QSortFilterProxyModel(parent), m_sidebar(sidebar) {}

    void setSearch(const QString &text)
    {
        if (m_search == text) return;
        m_search = text;
        invalidateFilter();
    }
    void setSearchField(int column)
    {
        if (m_column == column) return;
        m_column = column;
        invalidateFilter();
    }
    void refresh() { invalidateFilter(); }

protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override
    {
        const auto *model = qobject_cast<const TorrentModel *>(sourceModel());
        if (!model || parent.isValid()) return false;
        const QString hash = model->hashForRow(row);
        if (hash.isEmpty() || !m_sidebar->matches(hash, model->torrents().value(hash)))
            return false;
        if (m_search.isEmpty())
            return true;
        const int field = m_column == 1 ? 25 : m_column == 2 ? 32 : 0;
        return model->data(model->index(row, field), Qt::UserRole).toString()
            .contains(m_search, Qt::CaseInsensitive);
    }
private:
    FilterSidebar *m_sidebar;
    QString m_search;
    int m_column = 0;
};
} // namespace

ServerPane::ServerPane(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *outer = new QSplitter(Qt::Horizontal, this);
    outer->setObjectName(QStringLiteral("serverHorizontalSplitter"));
    m_outerSplitter = outer;
    outer->setChildrenCollapsible(false);
    layout->addWidget(outer);

    m_sidebar = new FilterSidebar(outer);
    m_sidebar->setMinimumWidth(185);
    m_sidebar->setMaximumWidth(320);

    auto *vertical = new QSplitter(Qt::Vertical, outer);
    vertical->setObjectName(QStringLiteral("torrentDetailsSplitter"));
    m_detailsSplitter = vertical;
    vertical->setChildrenCollapsible(false);

    m_table = new QTableView(vertical);
    m_table->setObjectName(QStringLiteral("torrentList"));
    m_model = new TorrentModel(m_table);
    auto *combined = new TorrentFilterProxyModel(m_sidebar, m_table);
    m_proxy = combined;
    m_proxy->setSourceModel(m_model);
    m_proxy->setSortRole(Qt::UserRole);
    connect(m_sidebar, &FilterSidebar::filtersChanged, combined,
            [combined]() { combined->refresh(); });
    m_table->setModel(m_proxy);
    m_table->setItemDelegate(new TransferListDelegate(m_table));
    m_table->setAlternatingRowColors(true);
    m_table->setShowGrid(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setSortingEnabled(true);
    m_table->horizontalHeader()->setStretchLastSection(false);
    // Qt highlights selected columns by default; some desktop styles
    // render that state as intermittent bold header labels.
    m_table->horizontalHeader()->setHighlightSections(false);
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
            [this, vertical, details](bool show) {
                auto *handle = vertical->handle(1);
                if (!show)
                {
                    // Save the EXPANDED splitter geometry, not the compressed
                    // tab-row-only geometry, for the next launch/reopen.
                    m_expandedDetailsState = vertical->saveState();
                    m_originalDetailsHandleWidth = vertical->handleWidth();
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
                    if (m_originalDetailsHandleWidth >= 0)
                        vertical->setHandleWidth(m_originalDetailsHandleWidth);
                    if (handle)
                        handle->setVisible(true);
                    m_details->show();
                    if (!m_expandedDetailsState.isEmpty())
                        vertical->restoreState(m_expandedDetailsState);
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

    // /sync/maindata sends incremental changes for categories, tags and
    // tracker-to-hash memberships, not complete lists on every request.
    if (payload.value(QStringLiteral("full_update")).toBool())
    {
        m_categories.clear();
        m_tags.clear();
        m_trackers.clear();
    }
    const QJsonObject categories = payload.value(QStringLiteral("categories")).toObject();
    for (auto it = categories.constBegin(); it != categories.constEnd(); ++it)
        m_categories.insert(it.key());
    for (const QJsonValue &value : payload.value(QStringLiteral("categories_removed")).toArray())
        m_categories.remove(value.toString());
    for (const QJsonValue &value : payload.value(QStringLiteral("tags")).toArray())
        m_tags.insert(value.toString());
    for (const QJsonValue &value : payload.value(QStringLiteral("tags_removed")).toArray())
        m_tags.remove(value.toString());
    const QJsonObject trackers = payload.value(QStringLiteral("trackers")).toObject();
    for (auto it = trackers.constBegin(); it != trackers.constEnd(); ++it)
    {
        QSet<QString> hashes;
        for (const QJsonValue &hash : it.value().toArray())
            hashes.insert(hash.toString());
        m_trackers.insert(it.key(), hashes);
    }
    for (const QJsonValue &value : payload.value(QStringLiteral("trackers_removed")).toArray())
        m_trackers.remove(value.toString());

    m_model->applySync(payload);
    m_sidebar->updateCounts(m_model->torrents(), m_categories, m_tags, m_trackers);
    static_cast<TorrentFilterProxyModel *>(m_proxy)->refresh();
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
    static_cast<TorrentFilterProxyModel *>(m_proxy)->setSearch(text);
}

void ServerPane::setSearchColumn(int index)
{
    m_searchColumn = index;
    static_cast<TorrentFilterProxyModel *>(m_proxy)->setSearchField(index);
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

void ServerPane::refreshFontMetrics()
{
    m_sidebar->refreshFontMetrics();

    // The original upstream delegate caches the name-column sizeHint for
    // fast scrolling; that cache must be invalidated for font changes.
    if (auto *delegate = qobject_cast<TransferListDelegate *>(m_table->itemDelegate()))
        delegate->invalidateCachedRowHeight();

    // Native QTableView row sections otherwise retain their old height on
    // some Qt platform themes, clipping the enlarged text.
    const int suggested = qMax(m_table->fontMetrics().height() + 8,
                                m_table->iconSize().height() + 4);
    m_table->verticalHeader()->setDefaultSectionSize(suggested);
    m_table->doItemsLayout();
    m_table->viewport()->update();
}

namespace
{
QString serverLayoutPrefix(const QString &id)
{
    // The opaque profile ID is hashed so arbitrary text in an imported
    // profile cannot introduce '/' separators into QSettings paths.
    return QStringLiteral("Layout/Servers/")
        + QString::fromLatin1(QCryptographicHash::hash(
            id.toUtf8(), QCryptographicHash::Sha256).toHex());
}
} // namespace

void ServerPane::saveLayout(QSettings &settings, const QString &profileId) const
{
    if (profileId.isEmpty())
        return;

    const QString root = serverLayoutPrefix(profileId);
    settings.setValue(root + QStringLiteral("/horizontal"), m_outerSplitter->saveState());
    const QByteArray expandedState = m_detailTabs->currentIndex() >= 0
        ? m_detailsSplitter->saveState() : m_expandedDetailsState;
    if (!expandedState.isEmpty())
        settings.setValue(root + QStringLiteral("/verticalExpanded"), expandedState);

    settings.setValue(root + QStringLiteral("/torrentHeader"),
                      m_table->horizontalHeader()->saveState());
    settings.setValue(root + QStringLiteral("/sortColumn"),
                      m_table->horizontalHeader()->sortIndicatorSection());
    settings.setValue(root + QStringLiteral("/sortOrder"),
                      static_cast<int>(m_table->horizontalHeader()->sortIndicatorOrder()));
    settings.setValue(root + QStringLiteral("/detailsTab"), m_details->currentTab());
    settings.setValue(root + QStringLiteral("/detailsVisible"),
                      m_detailTabs->currentIndex() >= 0);

    const auto saveHeader = [&](const char *objectName, const char *key) {
        if (auto *view = m_details->findChild<QTreeWidget *>(
                QString::fromLatin1(objectName)))
            settings.setValue(root + QLatin1Char('/') + QString::fromLatin1(key),
                              view->header()->saveState());
    };
    saveHeader("torrentTrackers", "trackerHeader");
    saveHeader("torrentContent", "contentHeader");

    if (auto *peers = m_details->findChild<QTableWidget *>(QStringLiteral("torrentPeers")))
        settings.setValue(root + QStringLiteral("/peerHeader"),
                          peers->horizontalHeader()->saveState());
}

void ServerPane::restoreLayout(QSettings &settings, const QString &profileId)
{
    if (profileId.isEmpty())
        return;
    const QString root = serverLayoutPrefix(profileId);

    const auto restoreSplitter = [&settings, &root](QSplitter *splitter, const char *key) {
        const QByteArray data = settings.value(
            root + QLatin1Char('/') + QString::fromLatin1(key)).toByteArray();
        if (!data.isEmpty())
            splitter->restoreState(data);
    };
    restoreSplitter(m_outerSplitter, "horizontal");
    m_expandedDetailsState =
        settings.value(root + QStringLiteral("/verticalExpanded")).toByteArray();
    if (!m_expandedDetailsState.isEmpty())
        m_detailsSplitter->restoreState(m_expandedDetailsState);

    const QByteArray torrentHeader =
        settings.value(root + QStringLiteral("/torrentHeader")).toByteArray();
    if (!torrentHeader.isEmpty())
    {
        m_table->horizontalHeader()->restoreState(torrentHeader);
        const int col = settings.value(root + QStringLiteral("/sortColumn"), 0).toInt();
        const auto order = static_cast<Qt::SortOrder>(
            settings.value(root + QStringLiteral("/sortOrder"),
                           static_cast<int>(Qt::AscendingOrder)).toInt());
        if (col >= 0 && col < m_model->columnCount()
            && (order == Qt::AscendingOrder || order == Qt::DescendingOrder))
            m_table->sortByColumn(col, order);
    }

    const auto restoreHeader = [&](const char *objectName, const char *key) {
        if (auto *view = m_details->findChild<QTreeWidget *>(
                QString::fromLatin1(objectName)))
        {
            const QByteArray data = settings.value(
                root + QLatin1Char('/') + QString::fromLatin1(key)).toByteArray();
            if (!data.isEmpty())
                view->header()->restoreState(data);
        }
    };
    restoreHeader("torrentTrackers", "trackerHeader");
    restoreHeader("torrentContent", "contentHeader");
    if (auto *peers = m_details->findChild<QTableWidget *>(QStringLiteral("torrentPeers")))
    {
        const QByteArray data =
            settings.value(root + QStringLiteral("/peerHeader")).toByteArray();
        if (!data.isEmpty())
            peers->horizontalHeader()->restoreState(data);
    }

    const int tab = settings.value(root + QStringLiteral("/detailsTab"), 0).toInt();
    if (tab >= 0 && tab <= 5 && m_detailTabs->currentIndex() != tab)
        m_detailTabs->setCurrentIndex(tab);
    const bool visible = settings.value(root + QStringLiteral("/detailsVisible"), true).toBool();
    if (!visible)
    {
        // Collapse only after the first real layout pass; otherwise the
        // pre-show splitter size could overwrite the saved expanded height.
        QTimer::singleShot(0, this, [this]() {
            if (m_detailTabs->currentIndex() >= 0)
                m_detailTabs->setCurrentIndex(-1);
        });
    }
}
