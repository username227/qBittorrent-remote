// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/ServerPane.h"
#include "gui/FilterSidebar.h"
#include "gui/TorrentModel.h"
#include "gui/NaturalNameSort.h"
#include "gui/TorrentDetailsWidget.h"
#include "gui/upstream/proptabbar.h"
#include "gui/upstream/transferlistdelegate.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QAction>
#include <QClipboard>
#include <QGuiApplication>
#include <QIcon>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QSettings>
#include <QTreeWidget>
#include <QTableWidget>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <algorithm>
#include <functional>
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
// The Name column must remain readable even when the other, numerous
// qBittorrent transfer columns extend past the viewport. Users can scroll
// horizontally instead of being left with an unresizable 20-pixel Name cell.
int sensibleNameColumnWidth(const QTableView *table)
{
    return qMax(320, table->fontMetrics().horizontalAdvance(
        QStringLiteral("A descriptive torrent file name")) + 32);
}

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
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override
    {
        if (left.column() == 0 && right.column() == 0)
        {
            return RemoteGuiSort::naturalNameCompare(
                sourceModel()->data(left, Qt::UserRole).toString(),
                sourceModel()->data(right, Qt::UserRole).toString()) < 0;
        }
        // Preserve Qt's numeric comparison for the transfer-data columns.
        return QSortFilterProxyModel::lessThan(left, right);
    }

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
    // Native Qt may still bold labels of selected header sections even when
    // highlighting is disabled. An explicitly resolved Normal weight prevents
    // QCommonStyle::CE_HeaderLabel from synthesizing bold, without styling
    // or replacing the native header.
    m_table->horizontalHeader()->setHighlightSections(false);
    QFont headerFont = QApplication::font();
    headerFont.setWeight(QFont::Normal);
    m_table->horizontalHeader()->setFont(headerFont);
    // Stretch disables user resizing. On Windows it also squeezed Name to
    // nearly nothing because all other visible columns took the viewport.
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_table->horizontalHeader()->resizeSection(0, sensibleNameColumnWidth(m_table));
    m_table->verticalHeader()->hide();
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table, &QWidget::customContextMenuRequested,
            this, &ServerPane::showTorrentContextMenu);

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

void ServerPane::showTorrentContextMenu(const QPoint &position)
{
    const QModelIndex hit = m_table->indexAt(position);
    if (!hit.isValid())
        return; // Empty whitespace is not a selected torrent.

    auto *selection = m_table->selectionModel();
    if (!selection->isRowSelected(hit.row(), QModelIndex()))
    {
        // Right-clicking an unselected row acts on that row; right-clicking
        // an already selected row preserves the existing multi-selection.
        selection->setCurrentIndex(hit, QItemSelectionModel::ClearAndSelect
                                     | QItemSelectionModel::Rows);
    }

    const QModelIndexList selected = selection->selectedRows(0);
    if (selected.isEmpty())
        return;

    struct SelectedTorrent
    {
        QString hash;
        QJsonObject record;
    };
    QList<SelectedTorrent> torrents;
    for (const QModelIndex &row : selected)
    {
        const QModelIndex source = m_proxy->mapToSource(row);
        const QString hash = m_model->hashForRow(source.row());
        if (!hash.isEmpty())
            torrents.append({hash, m_model->torrents().value(hash)});
    }
    if (torrents.isEmpty())
        return;

    QMenu menu(m_table);
    menu.setObjectName(QStringLiteral("torrentListContextMenu"));
    const auto icon = [](const char *name) {
        // Reuse the original qBittorrent SVG icons already embedded in the
        // native GUI resource collection. No dependency on an OS icon theme.
        return QIcon(QStringLiteral(":/icons/") + QString::fromLatin1(name)
                     + QStringLiteral(".svg"));
    };
    const auto stopped = [](const SelectedTorrent &entry) {
        const QString state = entry.record.value(QStringLiteral("state")).toString();
        return state.startsWith(QLatin1String("stopped"))
            || state.startsWith(QLatin1String("paused"));
    };
    const bool anyStopped = std::any_of(torrents.cbegin(), torrents.cend(), stopped);
    const bool anyRunning = std::any_of(torrents.cbegin(), torrents.cend(),
                                        [&stopped](const SelectedTorrent &t) {
                                            return !stopped(t);
                                        });
    const auto isErrored = [](const SelectedTorrent &t) {
        const QString state = t.record.value(QStringLiteral("state")).toString();
        return state == QLatin1String("error")
            || state == QLatin1String("missingFiles");
    };
    const auto isChecking = [](const SelectedTorrent &t) {
        return t.record.value(QStringLiteral("state")).toString()
            .startsWith(QLatin1String("checking"));
    };
    const auto isQueued = [](const SelectedTorrent &t) {
        return t.record.value(QStringLiteral("state")).toString()
            .startsWith(QLatin1String("queued"));
    };
    const bool needsForce = std::any_of(torrents.cbegin(), torrents.cend(),
        [&isErrored](const SelectedTorrent &t) {
            return !t.record.value(QStringLiteral("force_start")).toBool()
                || isErrored(t);
        });
    // The sync feed does not always provide has_metadata. Exclude the
    // explicit metadata-download states instead of assuming their files
    // can be rechecked.
    const bool hasMetadata = std::any_of(torrents.cbegin(), torrents.cend(),
        [](const SelectedTorrent &t) {
            const QString state = t.record.value(QStringLiteral("state")).toString();
            const QJsonValue metadata = t.record.value(QStringLiteral("has_metadata"));
            if (metadata.isBool())
                return metadata.toBool();
            return !state.isEmpty()
                && state != QLatin1String("metaDL")
                && state != QLatin1String("forcedMetaDL")
                && state != QLatin1String("unknown");
        });
    const bool canReannounce = std::any_of(torrents.cbegin(), torrents.cend(),
        [&stopped, &isErrored, &isChecking, &isQueued](const SelectedTorrent &t) {
            const QString state = t.record.value(QStringLiteral("state")).toString();
            return !state.isEmpty() && state != QLatin1String("unknown")
                && !stopped(t) && !isErrored(t) && !isChecking(t) && !isQueued(t);
        });

    // Match native qBittorrent for uniform selections: only show the
    // applicable action. For a mixed selection both actions are useful.
    if (anyStopped)
    {
        QAction *start = menu.addAction(icon("torrent-start"), tr("Start"));
        start->setIconVisibleInMenu(true);
        start->setObjectName(QStringLiteral("torrentContextStart"));
        connect(start, &QAction::triggered, this, [this]() {
            emit torrentContextActionRequested(TorrentContextAction::Start);
        });
    }
    if (anyRunning)
    {
        QAction *stop = menu.addAction(icon("torrent-stop"), tr("Stop"));
        stop->setIconVisibleInMenu(true);
        stop->setObjectName(QStringLiteral("torrentContextStop"));
        connect(stop, &QAction::triggered, this, [this]() {
            emit torrentContextActionRequested(TorrentContextAction::Stop);
        });
    }
    if (needsForce)
    {
        QAction *force = menu.addAction(icon("torrent-start-forced"), tr("Force Start"));
        force->setIconVisibleInMenu(true);
        force->setObjectName(QStringLiteral("torrentContextForceStart"));
        connect(force, &QAction::triggered, this, [this]() {
            emit torrentContextActionRequested(TorrentContextAction::ForceStart);
        });
    }
    menu.addSeparator();
    QAction *remove = menu.addAction(icon("list-remove"), tr("Remove"));
    remove->setIconVisibleInMenu(true);
    remove->setObjectName(QStringLiteral("torrentContextRemove"));
    connect(remove, &QAction::triggered, this, [this]() {
        emit torrentContextActionRequested(TorrentContextAction::Remove);
    });
    menu.addSeparator();

    // Upstream places these after category/options controls, which can be
    // inserted above them by subsequent focused context-menu PRs.
    if (hasMetadata)
    {
        QAction *recheck = menu.addAction(icon("force-recheck"), tr("Force Recheck"));
        recheck->setObjectName(QStringLiteral("torrentContextForceRecheck"));
        recheck->setIconVisibleInMenu(true);
        connect(recheck, &QAction::triggered, this, [this]() {
            emit torrentContextActionRequested(TorrentContextAction::ForceRecheck);
        });
    }
    QAction *reannounce = menu.addAction(icon("reannounce"), tr("Force Reannounce"));
    reannounce->setObjectName(QStringLiteral("torrentContextForceReannounce"));
    reannounce->setIconVisibleInMenu(true);
    reannounce->setEnabled(canReannounce);
    if (!canReannounce)
        reannounce->setToolTip(tr("Cannot reannounce stopped, queued, errored or checking torrents."));
    connect(reannounce, &QAction::triggered, this, [this]() {
        emit torrentContextActionRequested(TorrentContextAction::ForceReannounce);
    });
    menu.addSeparator();

    // Original qBittorrent shows Queue only when server-side queueing is
    // enabled and the selection includes at least one incomplete torrent.
    // /sync/maindata reports queueing in server_state, and progress on each
    // torrent; never infer an enabled queueing system from local preferences.
    const bool hasIncompleteTorrent = std::any_of(torrents.cbegin(), torrents.cend(),
        [](const SelectedTorrent &t) {
            const QJsonValue progress = t.record.value(QStringLiteral("progress"));
            return progress.isDouble() && progress.toDouble() < 1.0;
        });
    if (m_serverState.value(QStringLiteral("queueing")).toBool() && hasIncompleteTorrent)
    {
        QMenu *queue = menu.addMenu(icon("queued"), tr("Queue"));
        queue->setObjectName(QStringLiteral("torrentContextQueueMenu"));
        queue->menuAction()->setObjectName(QStringLiteral("torrentContextQueueMenu"));
        queue->menuAction()->setIconVisibleInMenu(true);

        const auto queueAction = [this, queue, &icon](
            const QString &title, const char *objectName,
            const char *iconName, TorrentContextAction command) {
            QAction *action = queue->addAction(icon(iconName), title);
            action->setObjectName(QString::fromLatin1(objectName));
            action->setIconVisibleInMenu(true);
            connect(action, &QAction::triggered, this, [this, command]() {
                emit torrentContextActionRequested(command);
            });
        };
        queueAction(tr("Move to top"), "torrentQueueTop", "go-top",
                    TorrentContextAction::QueueTop);
        queueAction(tr("Move up"), "torrentQueueUp", "go-up",
                    TorrentContextAction::QueueUp);
        queueAction(tr("Move down"), "torrentQueueDown", "go-down",
                    TorrentContextAction::QueueDown);
        queueAction(tr("Move to bottom"), "torrentQueueBottom", "go-bottom",
                    TorrentContextAction::QueueBottom);
        menu.addSeparator();
    }

    QMenu *copy = menu.addMenu(icon("edit-copy"), tr("Copy"));
    copy->menuAction()->setIconVisibleInMenu(true);
    copy->setObjectName(QStringLiteral("torrentContextCopyMenu"));
    copy->menuAction()->setObjectName(QStringLiteral("torrentContextCopyMenu"));
    const auto copyField = [this, copy, &torrents, &icon](
        const QString &label, const QString &objectName, const char *iconName,
        const std::function<QString(const SelectedTorrent &)> &extract) {
        QStringList values;
        for (const SelectedTorrent &torrent : torrents)
        {
            const QString value = extract(torrent);
            if (!value.isEmpty())
                values.append(value);
        }
        QAction *action = copy->addAction(icon(iconName), label);
        action->setIconVisibleInMenu(true);
        action->setObjectName(objectName);
        action->setEnabled(!values.isEmpty());
        connect(action, &QAction::triggered, this, [values]() {
            QGuiApplication::clipboard()->setText(values.join(QLatin1Char('\n')));
        });
    };

    copyField(tr("Name"), QStringLiteral("torrentCopyName"), "name",
        [](const SelectedTorrent &t) {
            return t.record.value(QStringLiteral("name")).toString();
        });
    copyField(tr("Info hash v1"), QStringLiteral("torrentCopyHashV1"), "hash",
        [](const SelectedTorrent &t) {
            const QString v1 = t.record.value(QStringLiteral("infohash_v1")).toString();
            return v1.isEmpty() && t.hash.size() == 40 ? t.hash : v1;
        });
    copyField(tr("Info hash v2"), QStringLiteral("torrentCopyHashV2"), "hash",
        [](const SelectedTorrent &t) {
            const QString v2 = t.record.value(QStringLiteral("infohash_v2")).toString();
            return v2.isEmpty() && t.hash.size() == 64 ? t.hash : v2;
        });
    copyField(tr("Magnet link"), QStringLiteral("torrentCopyMagnet"), "torrent-magnet",
        [](const SelectedTorrent &t) {
            return t.record.value(QStringLiteral("magnet_uri")).toString();
        });
    // The remote Web API identifies torrents by canonical hash.
    copyField(tr("Torrent ID"), QStringLiteral("torrentCopyId"), "help-about",
        [](const SelectedTorrent &t) { return t.hash; });
    copyField(tr("Comment"), QStringLiteral("torrentCopyComment"), "edit-copy",
        [](const SelectedTorrent &t) {
            return t.record.value(QStringLiteral("comment")).toString();
        });
    copyField(tr("Content Path"), QStringLiteral("torrentCopyContentPath"), "directory",
        [](const SelectedTorrent &t) {
            // Always copy the remote server's actual content path. A local
            // NAS mapping is only applicable to opening the path later.
            return t.record.value(QStringLiteral("content_path")).toString();
        });

    // Every displayed action works; values not present in /sync/maindata
    // remain disabled rather than guessing metadata or local file paths.
    menu.exec(m_table->viewport()->mapToGlobal(position));
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

    // The header's explicit weight must not freeze its inherited size.
    // The application adjusts the global Qt font via View -> Font Size;
    // re-resolve from QApplication rather than the cached table font, which
    // can still report the previous size during live style propagation.
    QFont headerFont = QApplication::font();
    headerFont.setWeight(QFont::Normal);
    m_table->horizontalHeader()->setFont(headerFont);

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
    // Existing layouts saved a non-resizable Name column. Mark layouts saved
    // after the migration so a user's future manual narrow width is honored.
    settings.setValue(root + QStringLiteral("/nameColumnResizableV1"), true);
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

    // QHeaderView::restoreState can bring back the old Stretch resize mode
    // and the minuscule Name column stored by affected Windows versions.
    // Fix it once per profile, leaving intentional sizes saved after this
    // change untouched on subsequent launches.
    auto *header = m_table->horizontalHeader();
    // Qt serializes highlightSections in QHeaderView::saveState(). A saved
    // layout from an older build can re-enable the selected-column highlight
    // even though the constructor disabled it, causing intermittent bold
    // torrent headings whenever rows are selected. Override that persisted
    // visual flag *after* restoring section geometry and sort state.
    header->setHighlightSections(false);
    header->setSectionResizeMode(0, QHeaderView::Interactive);
    if (!settings.value(root + QStringLiteral("/nameColumnResizableV1"), false).toBool())
        header->resizeSection(0, qMax(header->sectionSize(0),
                                      sensibleNameColumnWidth(m_table)));

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
