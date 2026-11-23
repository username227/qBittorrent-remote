// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/TorrentDetailsWidget.h"
#include "gui/MappedContentTreeWidget.h"
#include "gui/NaturalNameSort.h"
#include "ui_remotepropertieswidget.h"
#include "gui/upstream/progressbarpainter.h"

#include <QAction>
#include <QDateTime>
#include <QDesktopServices>
#include <QFileInfo>
#include <QIcon>
#include <QLineEdit>
#include <QMimeDatabase>
#include <QMimeType>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QLayout>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QPainter>
#include <QPalette>
#include <QProgressBar>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QStyle>
#include <QUrl>
#include <QTableWidget>
#include <QTreeWidget>
#include <QSet>
#include <QPushButton>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <functional>

namespace
{
QString bytesText(double amount, bool perSecond = false)
{
    if (amount < 0)
        return QStringLiteral("—");
    static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    int unit = 0;
    while (amount >= 1024 && unit < 5)
    {
        amount /= 1024;
        ++unit;
    }
    return QLocale().toString(amount, 'f', unit ? 1 : 0)
        + QLatin1Char(' ') + QString::fromLatin1(units[unit])
        + (perSecond ? QStringLiteral("/s") : QString());
}

QString numberText(const QJsonValue &value)
{
    return value.isDouble() ? QLocale().toString(value.toInteger()) : QStringLiteral("—");
}

QString sizeText(const QJsonValue &value)
{
    return value.isDouble() ? bytesText(value.toDouble()) : QStringLiteral("—");
}

QString speedText(const QJsonValue &value)
{
    return value.isDouble() ? bytesText(value.toDouble(), true) : QStringLiteral("—");
}

QString dateText(const QJsonValue &value)
{
    if (!value.isDouble() || value.toInteger() <= 0)
        return QStringLiteral("—");
    return QLocale().toString(QDateTime::fromSecsSinceEpoch(value.toInteger()),
                              QLocale::ShortFormat);
}

QString durationText(const QJsonValue &value)
{
    if (!value.isDouble() || value.toInteger() < 0 || value.toInteger() >= 8640000)
        return QStringLiteral("—");
    qint64 n = value.toInteger();
    const qint64 days = n / 86400;
    const qint64 hours = (n % 86400) / 3600;
    const qint64 mins = (n % 3600) / 60;
    if (days)
        return QStringLiteral("%1d %2h").arg(days).arg(hours);
    if (hours)
        return QStringLiteral("%1h %2m").arg(hours).arg(mins);
    return QStringLiteral("%1m").arg(mins);
}

QString availabilityText(const QJsonValue &value)
{
    if (!value.isDouble())
        return QStringLiteral("—");
    const double copies = value.toDouble();
    return copies < 0 ? QStringLiteral("N/A") : QLocale().toString(copies, 'f', 3);
}

QString priorityText(int priority)
{
    switch (priority)
    {
    case 0: return QStringLiteral("Do not download");
    case 6: return QStringLiteral("High");
    case 7: return QStringLiteral("Maximum");
    default: return QStringLiteral("Normal");
    }
}



QString trackerCount(const QJsonValue &v)
{
    return !v.isDouble() || v.toInteger() < 0
        ? QStringLiteral("N/A") : QLocale().toString(v.toInteger());
}

QString trackerAnnounceTime(const QJsonValue &v)
{
    // qBittorrent Web API returns Unix timestamps; native GUI shows a
    // remaining duration, not a timestamp or a raw number of seconds.
    if (!v.isDouble() || v.toInteger() <= 0)
        return {};
    const qint64 remaining = std::max<qint64>(
        0, v.toInteger() - QDateTime::currentSecsSinceEpoch());
    if (remaining < 60)
        return QObject::tr("< 1m");
    if (remaining < 3600)
        return QObject::tr("%1m").arg(remaining / 60);
    if (remaining < 86400)
        return QObject::tr("%1h %2m").arg(remaining / 3600).arg((remaining % 3600) / 60);
    return QObject::tr("%1d %2h").arg(remaining / 86400).arg((remaining % 86400) / 3600);
}

QString trackerStatus(const QJsonObject &tracker, bool pseudo)
{
    if (pseudo && tracker.value(QStringLiteral("disabled_for_torrent")).toBool(false))
        return QObject::tr("Disabled for this torrent");
    if (tracker.value(QStringLiteral("updating")).toBool(false))
        return QObject::tr("Updating...");
    const int status = tracker.value(QStringLiteral("status")).toInt(-1);
    switch (status)
    {
    case 0: return QObject::tr("Disabled");
    case 1: return QObject::tr("Not contacted yet");
    case 2: return QObject::tr("Working");
    case 3: return QObject::tr("Updating...");
    case 4: return QObject::tr("Not working");
    default: return QObject::tr("Unknown");
    }
}

QString trackerProtocol(const QJsonValue &v)
{
    if (!v.isDouble() || v.toInteger() <= 0)
        return {};
    return QStringLiteral("v%1").arg(v.toInteger());
}

bool isPseudoTracker(const QString &url)
{
    return url == QLatin1String("** [DHT] **")
        || url == QLatin1String("** [PeX] **")
        || url == QLatin1String("** [LSD] **");
}

QStringList nativeTrackerFields(const QJsonObject &record, bool pseudo, bool endpoint)
{
    // Native columns: URL / Tier / BT Protocol / Status / Peers / Seeds /
    // Leeches / Times Downloaded / Message / Next Announce / Min Announce.
    const int tier = record.value(QStringLiteral("tier")).toInt(-1);
    return {
        record.value(endpoint ? QStringLiteral("name") : QStringLiteral("url")).toString(),
        (pseudo || endpoint || tier < 0) ? QString() : QString::number(tier),
        endpoint ? trackerProtocol(record.value(QStringLiteral("bt_version"))) : QString(),
        trackerStatus(record, pseudo),
        pseudo ? QObject::tr("N/A") : trackerCount(record.value(QStringLiteral("num_peers"))),
        trackerCount(record.value(QStringLiteral("num_seeds"))),
        trackerCount(record.value(QStringLiteral("num_leeches"))),
        pseudo ? QObject::tr("N/A") : trackerCount(record.value(QStringLiteral("num_downloaded"))),
        record.value(QStringLiteral("msg")).toString(),
        pseudo ? QString() : trackerAnnounceTime(record.value(QStringLiteral("next_announce"))),
        pseudo ? QString() : trackerAnnounceTime(record.value(QStringLiteral("min_announce")))
    };
}

QString linkifyComment(const QString &comment)
{
    // Tracker metadata is untrusted: escape ALL non-URL text and permit
    // only absolute HTTP(S) anchors. Never parse embedded HTML/JS.
    static const QRegularExpression links(
        QStringLiteral(R"((?:https?://)[^\s<>"']+)"),
        QRegularExpression::CaseInsensitiveOption);
    QString html;
    qsizetype position = 0;
    const auto matches = links.globalMatch(comment);
    auto it = matches;
    while (it.hasNext())
    {
        const QRegularExpressionMatch match = it.next();
        QString candidate = match.captured();
        while (!candidate.isEmpty()
               && QStringLiteral(".,;!?)]}").contains(candidate.back()))
            candidate.chop(1);
        const QUrl url(candidate, QUrl::StrictMode);
        if (!url.isValid() || url.host().isEmpty()
            || (url.scheme().compare(QStringLiteral("http"), Qt::CaseInsensitive) != 0
                && url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0))
            continue;

        html += comment.mid(position, match.capturedStart() - position).toHtmlEscaped();
        html += QStringLiteral("<a href=\"%1\">%2</a>")
                    .arg(url.toString(QUrl::FullyEncoded).toHtmlEscaped(),
                         candidate.toHtmlEscaped());
        position = match.capturedStart() + candidate.size();
    }
    html += comment.mid(position).toHtmlEscaped();
    return html.replace(QLatin1Char('\n'), QStringLiteral("<br/>"));
}

class ContentProgressDelegate final : public QStyledItemDelegate
{
public:
    explicit ContentProgressDelegate(QObject *parent = nullptr)
        : QStyledItemDelegate(parent) {}

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        if (index.column() == 2 && index.data(Qt::UserRole).isValid())
        {
            const double fraction = index.data(Qt::UserRole).toDouble();
            m_painter.paint(painter, option, index.data(Qt::DisplayRole).toString(),
                            qBound(0, qRound(fraction * 100), 100));
            return;
        }
        QStyledItemDelegate::paint(painter, option, index);
    }

private:
    ProgressBarPainter m_painter;
};

// Sort the actual Qt detail widgets by raw API values, not formatted text.
// QTreeWidget automatically sorts each sibling group within its own parent,
// retaining the torrent folder and tracker endpoint hierarchy.
class SortableDetailTreeItem final : public QTreeWidgetItem
{
public:
    explicit SortableDetailTreeItem(QTreeWidget *parent, const QStringList &labels = {})
        : QTreeWidgetItem(parent, labels) {}
    explicit SortableDetailTreeItem(QTreeWidgetItem *parent, const QStringList &labels = {})
        : QTreeWidgetItem(parent, labels) {}

    bool operator<(const QTreeWidgetItem &other) const override
    {
        const int column = treeWidget() ? treeWidget()->sortColumn() : 0;
        if (column == 0)
        {
            // DHT, PeX and LSD are fixed native pseudo-trackers, not
            // ordinary announce URLs. Preserve their traditional order
            // instead of alphabetically moving LSD ahead of PeX.
            const auto pseudoRank = [](const QString &name) {
                if (name == QLatin1String("** [DHT] **")) return 0;
                if (name == QLatin1String("** [PeX] **")) return 1;
                if (name == QLatin1String("** [LSD] **")) return 2;
                return 3;
            };
            const int leftRank = pseudoRank(text(0));
            const int rightRank = pseudoRank(other.text(0));
            if (leftRank != rightRank)
                return leftRank < rightRank;
        }
        if (column > 0)
        {
            const QVariant left = data(column, Qt::UserRole);
            const QVariant right = other.data(column, Qt::UserRole);
            if (left.isValid() != right.isValid())
                return !left.isValid();
            if (left.isValid())
            {
                if (left.toDouble() != right.toDouble())
                    return left.toDouble() < right.toDouble();
                return RemoteGuiSort::naturalNameCompare(text(0), other.text(0)) < 0;
            }
        }
        const int result = RemoteGuiSort::naturalNameCompare(
            text(column), other.text(column));
        return result ? result < 0
            : RemoteGuiSort::naturalNameCompare(text(0), other.text(0)) < 0;
    }
};

class SortablePeerItem final : public QTableWidgetItem
{
public:
    explicit SortablePeerItem(const QString &text) : QTableWidgetItem(text) {}

    bool operator<(const QTableWidgetItem &other) const override
    {
        const QVariant left = data(Qt::UserRole);
        const QVariant right = other.data(Qt::UserRole);
        if (left.isValid() != right.isValid())
            return !left.isValid();
        if (left.isValid())
            return left.toDouble() < right.toDouble();
        return RemoteGuiSort::naturalNameCompare(text(), other.text()) < 0;
    }
};

void setNumericSortKey(QTreeWidgetItem *item, int column, const QJsonValue &value)
{
    if (value.isDouble() && value.toDouble() >= 0)
        item->setData(column, Qt::UserRole, value.toDouble());
}

void setTrackerSortKeys(QTreeWidgetItem *item, const QJsonObject &record,
                        bool pseudo, bool endpoint)
{
    if (!pseudo && !endpoint)
        setNumericSortKey(item, 1, record.value(QStringLiteral("tier")));
    if (endpoint)
        setNumericSortKey(item, 2, record.value(QStringLiteral("bt_version")));
    if (!pseudo)
    {
        setNumericSortKey(item, 4, record.value(QStringLiteral("num_peers")));
        setNumericSortKey(item, 7, record.value(QStringLiteral("num_downloaded")));
        setNumericSortKey(item, 9, record.value(QStringLiteral("next_announce")));
        setNumericSortKey(item, 10, record.value(QStringLiteral("min_announce")));
    }
    setNumericSortKey(item, 5, record.value(QStringLiteral("num_seeds")));
    setNumericSortKey(item, 6, record.value(QStringLiteral("num_leeches")));
}

// Server-specific rates from /sync/maindata, kept independently for each pane.
// Similar native Qt painting to upstream SpeedPlotView, without libtorrent.
class NativeSpeedGraph final : public QWidget
{
public:
    explicit NativeSpeedGraph(QWidget *parent = nullptr) : QWidget(parent)
    {
        setObjectName(QStringLiteral("remoteSpeedGraph"));
        setMinimumHeight(125);
    }

    void append(double down, double up)
    {
        m_down.append(std::max(0.0, down));
        m_up.append(std::max(0.0, up));
        if (m_down.size() > 150)
        {
            m_down.removeFirst();
            m_up.removeFirst();
        }
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF graph = QRectF(rect()).adjusted(60, 27, -14, -24);
        if (graph.width() < 20 || graph.height() < 20)
            return;

        const QColor text = palette().color(QPalette::Text);
        const QColor grid = palette().color(QPalette::Mid);
        const QColor down = palette().color(QPalette::Highlight);
        const QColor up = palette().color(QPalette::Link);
        double maximum = 1024.0;
        for (double speed : m_down)
            maximum = std::max(maximum, speed);
        for (double speed : m_up)
            maximum = std::max(maximum, speed);
        maximum *= 1.15;

        painter.setPen(text);
        painter.drawText(QRectF(graph.left(), 0, graph.width(), 22),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         tr("Download: %1     Upload: %2")
                            .arg(bytesText(m_down.isEmpty() ? 0 : m_down.last(), true),
                                 bytesText(m_up.isEmpty() ? 0 : m_up.last(), true)));
        painter.setPen(QPen(grid, 0.5));
        for (int i = 0; i <= 4; ++i)
        {
            const qreal y = graph.top() + graph.height() * i / 4.0;
            painter.drawLine(QPointF(graph.left(), y), QPointF(graph.right(), y));
            painter.setPen(text);
            painter.drawText(QRectF(0, y - 9, 56, 18),
                             Qt::AlignRight | Qt::AlignVCenter,
                             bytesText(maximum * (4 - i) / 4.0, true));
            painter.setPen(QPen(grid, 0.5));
        }
        const auto paintSeries = [&](const QVector<double> &series, const QColor &color) {
            if (series.size() < 2)
                return;
            painter.setPen(QPen(color, 1.8));
            for (int i = 1; i < series.size(); ++i)
            {
                const auto point = [&](int index) {
                    return QPointF(graph.left() + graph.width() * index / 149.0,
                                   graph.bottom() - graph.height() * series.at(index) / maximum);
                };
                painter.drawLine(point(i - 1), point(i));
            }
        };
        paintSeries(m_down, down);
        paintSeries(m_up, up);
    }

private:
    QVector<double> m_down;
    QVector<double> m_up;
};
} // namespace

class SpeedGraphWidget final : public QWidget
{
public:
    explicit SpeedGraphWidget(QWidget *parent = nullptr) : QWidget(parent)
    {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        m_graph = new NativeSpeedGraph(this);
        layout->addWidget(m_graph);
    }
    void append(double down, double up) { m_graph->append(down, up); }
private:
    NativeSpeedGraph *m_graph = nullptr;
};

TorrentDetailsWidget::TorrentDetailsWidget(QWidget *parent)
    : QWidget(parent), m_ui(new Ui::PropertiesWidget)
{
    setObjectName(QStringLiteral("remoteTorrentProperties"));
    m_ui->setupUi(this);

    m_error = new QLabel(this);
    m_error->setObjectName(QStringLiteral("detailsError"));
    m_error->setTextFormat(Qt::PlainText);
    m_error->setWordWrap(true);
    m_error->hide();
    m_ui->verticalLayout->insertWidget(0, m_error);

    // The original qBittorrent tracker widget is a native hierarchical
    // QTreeView: DHT / PeX / LSD are fixed, non-expandable top-level rows;
    // actual trackers can expand to individual announce endpoints. A Qt
    // QTreeWidget preserves the native tree rows without libtorrent models.
    m_trackers = new QTreeWidget(this);
    m_trackers->setObjectName(QStringLiteral("torrentTrackers"));
    m_trackers->setColumnCount(11);
    m_trackers->setHeaderLabels({
        tr("URL/Announce Endpoint"), tr("Tier"), tr("BT Protocol"),
        tr("Status"), tr("Peers"), tr("Seeds"), tr("Leeches"),
        tr("Times Downloaded"), tr("Message"), tr("Next Announce"),
        tr("Min Announce")
    });
    m_trackers->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_trackers->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_trackers->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_trackers->setAlternatingRowColors(true);
    m_trackers->setUniformRowHeights(true);
    m_trackers->setRootIsDecorated(true);
    m_trackers->setItemsExpandable(true);
    m_trackers->setSortingEnabled(true);
    m_trackers->sortItems(0, Qt::AscendingOrder);
    m_trackers->header()->setStretchLastSection(false);
    m_trackers->header()->setHighlightSections(false);
    m_trackers->header()->setSectionsMovable(true);
    m_trackers->setColumnWidth(0, 285);
    m_trackers->setColumnWidth(1, 52);
    m_trackers->setColumnWidth(2, 88);
    m_trackers->setColumnWidth(3, 175);
    m_trackers->setColumnWidth(4, 65);
    m_trackers->setColumnWidth(5, 65);
    m_trackers->setColumnWidth(6, 75);
    m_trackers->setColumnWidth(7, 115);
    m_trackers->setColumnWidth(8, 195);
    m_trackers->setColumnWidth(9, 115);
    m_trackers->setColumnWidth(10, 105);
    m_ui->hBoxLayoutTrackers->insertWidget(0, m_trackers, 1);
    // Upstream's arrow buttons adjust tracker tiers through its local
    // BitTorrent engine; the standard Web API exposes no tier-reordering
    // endpoint. Keep the native controls visually, but explicitly disabled.
    m_ui->trackerUpButton->setIcon(style()->standardIcon(QStyle::SP_ArrowUp));
    m_ui->trackerDownButton->setIcon(style()->standardIcon(QStyle::SP_ArrowDown));
    m_ui->trackerUpButton->setEnabled(false);
    m_ui->trackerDownButton->setEnabled(false);
    const QString tierMessage = tr("Changing tracker tiers is not supported by the remote Web API.");
    m_ui->trackerUpButton->setToolTip(tierMessage);
    m_ui->trackerDownButton->setToolTip(tierMessage);

    m_peers = new QTableWidget(this);
    m_peers->setObjectName(QStringLiteral("torrentPeers"));
    m_peers->setColumnCount(8);
    m_peers->setHorizontalHeaderLabels({
        tr("IP"), tr("Country"), tr("Client"), tr("Progress"), tr("Down Speed"),
        tr("Up Speed"), tr("Flags"), tr("Connection")
    });
    m_peers->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_peers->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_peers->setAlternatingRowColors(true);
    m_peers->horizontalHeader()->setStretchLastSection(true);
    m_peers->horizontalHeader()->setSectionsMovable(true);
    m_peers->horizontalHeader()->setHighlightSections(false);
    m_peers->setSortingEnabled(true);
    m_peers->sortItems(0, Qt::AscendingOrder);
    m_ui->vBoxLayoutPeerPage->addWidget(m_peers);

    m_ui->listWebSeeds->setObjectName(QStringLiteral("torrentWebSeeds"));

    auto *tree = m_ui->filesList;
    tree->setObjectName(QStringLiteral("torrentContent"));
    tree->setColumnCount(6);
    tree->setHeaderLabels({
        tr("Name"), tr("Total Size"), tr("Progress"),
        tr("Download Priority"), tr("Remaining"), tr("Availability")
    });
    tree->header()->setStretchLastSection(false);
    tree->header()->setSectionsMovable(true);
    tree->header()->setHighlightSections(false);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->setColumnWidth(1, 105);
    tree->setColumnWidth(2, 112);
    tree->setColumnWidth(3, 125);
    tree->setColumnWidth(4, 100);
    tree->setColumnWidth(5, 95);
    tree->setItemDelegate(new ContentProgressDelegate(tree));
    tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree->setDragEnabled(true); // Keep existing working drag behavior by default.
    tree->setDragDropMode(QAbstractItemView::DragOnly);
    tree->setDefaultDropAction(Qt::CopyAction);
    tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree->setAlternatingRowColors(true);
    tree->setUniformRowHeights(true);
    tree->setSortingEnabled(true);
    tree->sortItems(0, Qt::AscendingOrder);
    connect(tree, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        if (m_hash.isEmpty() || m_ui->filesList->selectedItems().isEmpty())
            return;
        QMenu menu(this);
        QMenu *priorityMenu = menu.addMenu(tr("Priority"));
        for (const auto option : {
                 qMakePair(tr("Do not download"), 0),
                 qMakePair(tr("Normal priority"), 1),
                 qMakePair(tr("High priority"), 6),
                 qMakePair(tr("Maximum priority"), 7) })
        {
            QAction *action = priorityMenu->addAction(option.first);
            connect(action, &QAction::triggered, this,
                    [this, option]() { chooseFilePriorities(option.second, false); });
        }
        menu.exec(m_ui->filesList->viewport()->mapToGlobal(pos));
    });
    connect(m_ui->selectAllButton, &QPushButton::clicked, this,
            [this]() { chooseFilePriorities(1, true); });
    connect(m_ui->selectNoneButton, &QPushButton::clicked, this,
            [this]() { chooseFilePriorities(0, true); });

    // Native qBittorrent checkboxes reflect download inclusion, not the
    // presence of already-downloaded files on the remote server.
    connect(tree, &QTreeWidget::itemChanged, this,
            [this, tree](QTreeWidgetItem *changed, int column) {
        if (column != 0 || m_hash.isEmpty())
            return;
        const int priority = changed->checkState(0) == Qt::Unchecked ? 0 : 1;
        QList<int> affected;
        QSignalBlocker blocker(tree);
        const auto apply = [&affected, priority](QTreeWidgetItem *item,
                                                 const auto &self) -> void {
            const QVariant fileId = item->data(0, Qt::UserRole);
            if (fileId.isValid())
            {
                const int previous = item->data(0, Qt::UserRole + 3).toInt();
                // Checking a folder does not downgrade files that already
                // have High or Maximum priority.
                const int next = priority == 1 && previous > 0 ? previous : priority;
                if (next != previous)
                    affected.append(fileId.toInt());
                item->setData(0, Qt::UserRole + 3, next);
                item->setCheckState(0, next == 0 ? Qt::Unchecked : Qt::Checked);
                item->setText(3, priorityText(next));
                item->setData(3, Qt::UserRole, next);
            }
            else
            {
                for (int i = 0; i < item->childCount(); ++i)
                    self(item->child(i), self);
            }
        };
        apply(changed, apply);
        updateFileFolders();
        if (!affected.isEmpty())
            emit filePriorityRequested(m_hash, affected, priority);
    });

    auto *fileFilter = new QLineEdit(m_ui->pageContents);
    fileFilter->setObjectName(QStringLiteral("torrentFileFilter"));
    fileFilter->setPlaceholderText(tr("Filter files..."));
    fileFilter->setClearButtonEnabled(true);
    fileFilter->setMaximumWidth(220);
    m_ui->contentFilterLayout->addWidget(fileFilter);
    connect(fileFilter, &QLineEdit::textChanged, this,
            [this](const QString &needle) {
        auto *view = m_ui->filesList;
        const auto filterItem = [&needle](QTreeWidgetItem *item,
                                          const auto &self) -> bool {
            bool hasMatch = item->text(0).contains(needle, Qt::CaseInsensitive);
            for (int i = 0; i < item->childCount(); ++i)
                hasMatch = self(item->child(i), self) || hasMatch;
            item->setHidden(!hasMatch);
            return hasMatch;
        };
        for (int i = 0; i < view->topLevelItemCount(); ++i)
            filterItem(view->topLevelItem(i), filterItem);
    });

    auto *commentLabel = findChild<QLabel *>(QStringLiteral("labelCommentVal"));
    if (commentLabel)
    {
        commentLabel->setTextInteractionFlags(Qt::TextBrowserInteraction
                                              | Qt::TextSelectableByMouse);
        commentLabel->setOpenExternalLinks(false);
        connect(commentLabel, &QLabel::linkActivated, this,
                [](const QString &link) {
            const QUrl url(link, QUrl::StrictMode);
            if (url.isValid() && !url.host().isEmpty()
                && (url.scheme().compare(QStringLiteral("http"), Qt::CaseInsensitive) == 0
                    || url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0))
                QDesktopServices::openUrl(url);
        });
    }

    m_speedGraph = new SpeedGraphWidget(m_ui->pageSpeed);
    m_ui->speedLayout->addWidget(m_speedGraph);

    // The upstream layout reserves a spot for its original piece bar.
    // Replace only that local-engine-dependent widget with Qt's native bar.
    m_progressBar = new QProgressBar(this);
    m_progressBar->setObjectName(QStringLiteral("detailsProgressBar"));
    m_progressBar->setRange(0, 1000);
    QLayout *layout = m_ui->tempProgressBarArea->parentWidget()->layout();
    if (QLayoutItem *removed = layout ? layout->replaceWidget(
            m_ui->tempProgressBarArea, m_progressBar) : nullptr)
    {
        delete removed;
        m_ui->tempProgressBarArea->hide();
    }
    else
        m_ui->verticalLayout->insertWidget(0, m_progressBar);
    // Piece availability requires a separate piece-level API request.
    // Do not display a meaningless placeholder bar for it.
    m_ui->labelPiecesAvailability->hide();
    m_ui->tempAvailabilityBarArea->hide();
    m_ui->labelAverageAvailabilityVal->hide();

    resetPages();
}

TorrentDetailsWidget::~TorrentDetailsWidget()
{
    delete m_ui;
}

void TorrentDetailsWidget::showTab(int tab)
{
    if (tab < 0 || tab > 5)
        return;
    m_currentTab = tab;
    m_ui->stackedProperties->setCurrentIndex(tab);
    m_error->hide();
}

void TorrentDetailsWidget::setValue(const char *name, const QString &text)
{
    if (auto *label = findChild<QLabel *>(QString::fromLatin1(name)))
    {
        if (QString::fromLatin1(name) == QLatin1String("labelCommentVal"))
        {
            label->setTextFormat(Qt::RichText);
            label->setText(linkifyComment(text));
        }
        else
        {
            label->setTextFormat(Qt::PlainText);
            label->setText(text);
        }
    }
}

void TorrentDetailsWidget::resetPages()
{
    for (const char *name : {
        "labelProgressVal", "labelElapsedVal", "labelETAVal", "labelConnectionsVal",
        "labelDlTotalVal", "labelUpTotalVal", "labelSeedsVal", "labelPeersVal",
        "labelDlSpeedVal", "labelUpSpeedVal", "labelDlLimitVal", "labelUpLimitVal",
        "labelWastedVal", "labelShareRatioVal", "labelReannounceInVal",
        "labelLastSeenCompleteVal", "labelPopularityVal", "labelTotalSizeVal",
        "labelTotalPiecesVal", "labelCreatedByVal", "labelAddedOnVal",
        "labelCompletedOnVal", "labelCreatedOnVal", "labelPrivateVal",
        "labelInfohash1Val", "labelInfohash2Val", "labelSavePathVal",
        "labelCommentVal"
    })
        setValue(name, QStringLiteral("—"));
    m_progressBar->setValue(0);
    m_trackers->clear();
    m_peers->setRowCount(0);
    m_ui->listWebSeeds->clear();
    m_ui->filesList->clear();
    m_error->hide();
    m_ui->selectAllButton->setEnabled(!m_hash.isEmpty());
    m_ui->selectNoneButton->setEnabled(!m_hash.isEmpty());
}

void TorrentDetailsWidget::setSelection(const QString &hash, const QJsonObject &summary)
{
    if (m_hash != hash)
    {
        m_hash = hash;
        resetPages();
    }
    m_summary = summary;
    m_ui->filesList->setRemoteRoots(summary.value(QStringLiteral("save_path")).toString(),
                                   summary.value(QStringLiteral("content_path")).toString());
    if (hash.isEmpty())
        return;
    const double progress = summary.value(QStringLiteral("progress")).toDouble();
    m_progressBar->setValue(qBound(0, qRound(progress * 1000.0), 1000));
    setValue("labelProgressVal",
             QLocale().toString(progress * 100.0, 'f', 1) + QLatin1Char('%'));
    setValue("labelInfohash1Val",
             summary.value(QStringLiteral("infohash_v1")).toString(hash));
    setValue("labelInfohash2Val",
             summary.value(QStringLiteral("infohash_v2")).toString(QStringLiteral("—")));
    setValue("labelSavePathVal",
             summary.value(QStringLiteral("save_path")).toString(QStringLiteral("—")));
}

void TorrentDetailsWidget::displayGeneral(const QJsonObject &p)
{
    setValue("labelElapsedVal", durationText(p.value(QStringLiteral("time_elapsed"))));
    setValue("labelETAVal", durationText(p.value(QStringLiteral("eta"))));
    setValue("labelConnectionsVal", numberText(p.value(QStringLiteral("nb_connections"))));
    setValue("labelDlTotalVal", sizeText(p.value(QStringLiteral("total_downloaded"))));
    setValue("labelUpTotalVal", sizeText(p.value(QStringLiteral("total_uploaded"))));
    setValue("labelSeedsVal", numberText(p.value(QStringLiteral("seeds"))));
    setValue("labelPeersVal", numberText(p.value(QStringLiteral("peers"))));
    setValue("labelDlSpeedVal", speedText(p.value(QStringLiteral("dl_speed"))));
    setValue("labelUpSpeedVal", speedText(p.value(QStringLiteral("up_speed"))));
    setValue("labelDlLimitVal", speedText(p.value(QStringLiteral("dl_limit"))));
    setValue("labelUpLimitVal", speedText(p.value(QStringLiteral("up_limit"))));
    setValue("labelWastedVal", sizeText(p.value(QStringLiteral("total_wasted"))));
    if (const auto v = p.value(QStringLiteral("share_ratio")); v.isDouble())
        setValue("labelShareRatioVal", QLocale().toString(v.toDouble(), 'f', 2));
    setValue("labelReannounceInVal", durationText(p.value(QStringLiteral("reannounce"))));
    setValue("labelLastSeenCompleteVal", dateText(p.value(QStringLiteral("last_seen"))));
    if (const auto v = p.value(QStringLiteral("popularity")); v.isDouble())
        setValue("labelPopularityVal", QLocale().toString(v.toDouble(), 'f', 2));
    setValue("labelTotalSizeVal", sizeText(p.value(QStringLiteral("total_size"))));
    setValue("labelTotalPiecesVal", numberText(p.value(QStringLiteral("pieces_num"))));
    setValue("labelCreatedByVal", p.value(QStringLiteral("created_by")).toString(QStringLiteral("—")));
    setValue("labelAddedOnVal", dateText(p.value(QStringLiteral("addition_date"))));
    setValue("labelCompletedOnVal", dateText(p.value(QStringLiteral("completion_date"))));
    setValue("labelCreatedOnVal", dateText(p.value(QStringLiteral("creation_date"))));
    if (const auto v = p.value(QStringLiteral("is_private")); v.isBool())
        setValue("labelPrivateVal", v.toBool() ? tr("Yes") : tr("No"));
    setValue("labelSavePathVal",
             p.value(QStringLiteral("save_path")).toString(
                 m_summary.value(QStringLiteral("save_path")).toString(QStringLiteral("—"))));
    setValue("labelCommentVal", p.value(QStringLiteral("comment")).toString(QStringLiteral("—")));
    // The native UI also has a separate per-piece availability strip. This
    // is deliberately hidden until piece-level data is implemented.
}

void TorrentDetailsWidget::displayTrackers(const QJsonArray &trackers)
{
    // Preserve user expansion and selected tracker/endpoint across refreshes.
    QSet<QString> expandedURLs;
    QSet<QString> selectedRows;
    for (int i = 0; i < m_trackers->topLevelItemCount(); ++i)
    {
        auto *item = m_trackers->topLevelItem(i);
        if (item->isExpanded())
            expandedURLs.insert(item->text(0));
        if (item->isSelected())
            selectedRows.insert(item->text(0));
        for (int j = 0; j < item->childCount(); ++j)
        {
            const auto *endpoint = item->child(j);
            if (endpoint->isSelected())
                selectedRows.insert(item->text(0) + QLatin1Char('\n') + endpoint->text(0));
        }
    }

    QSignalBlocker blocker(m_trackers);
    m_trackers->setSortingEnabled(false);
    m_trackers->setUpdatesEnabled(false);
    m_trackers->clear();
    const auto setAlignment = [](QTreeWidgetItem *item) {
        for (int column : {1, 2, 4, 5, 6, 7, 9, 10})
            item->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
    };

    for (const QJsonValue &value : trackers)
    {
        if (!value.isObject())
            continue;
        const QJsonObject record = value.toObject();
        const QString url = record.value(QStringLiteral("url")).toString();
        const bool pseudo = isPseudoTracker(url);

        auto *item = new SortableDetailTreeItem(m_trackers,
                                        nativeTrackerFields(record, pseudo, false));
        setTrackerSortKeys(item, record, pseudo, false);
        item->setFlags((item->flags() | Qt::ItemIsSelectable | Qt::ItemIsEnabled)
                       & ~Qt::ItemIsEditable);
        item->setToolTip(0, url);
        item->setToolTip(8, item->text(8));
        setAlignment(item);
        if (pseudo)
        {
            // Upstream renders fixed DHT / PeX / LSD rows in muted gray.
            const QColor muted = palette().color(QPalette::Disabled, QPalette::Text);
            for (int column = 0; column < m_trackers->columnCount(); ++column)
                item->setForeground(column, muted);
        }

        // The Web API already includes the same pseudo rows as native
        // qBittorrent. Never synthesize them and never fabricate their
        // missing tier/announce values.
        if (!pseudo)
        {
            for (const QJsonValue &endpointValue :
                 record.value(QStringLiteral("endpoints")).toArray())
            {
                if (!endpointValue.isObject())
                    continue;
                const QJsonObject endpoint = endpointValue.toObject();
                auto *child = new SortableDetailTreeItem(item,
                    nativeTrackerFields(endpoint, false, true));
                setTrackerSortKeys(child, endpoint, false, true);
                child->setFlags((child->flags() | Qt::ItemIsSelectable | Qt::ItemIsEnabled)
                                & ~Qt::ItemIsEditable);
                child->setToolTip(0, child->text(0));
                child->setToolTip(8, child->text(8));
                setAlignment(child);
                if (selectedRows.contains(url + QLatin1Char('\n') + child->text(0)))
                    child->setSelected(true);
            }
        }
        if (expandedURLs.contains(url))
            item->setExpanded(true);
        if (selectedRows.contains(url))
            item->setSelected(true);
    }
    m_trackers->setSortingEnabled(true);
    m_trackers->setUpdatesEnabled(true);
}

void TorrentDetailsWidget::displayPeers(const QJsonObject &reply)
{
    const QJsonObject peers = reply.value(QStringLiteral("peers")).toObject();
    m_peers->setSortingEnabled(false);
    m_peers->setRowCount(peers.size());
    int row = 0;
    for (auto it = peers.constBegin(); it != peers.constEnd(); ++it, ++row)
    {
        const QJsonObject p = it.value().toObject();
        const double progress = p.value(QStringLiteral("progress")).toDouble(-1);
        const QStringList values {
            it.key(), p.value(QStringLiteral("country_code")).toString(),
            p.value(QStringLiteral("client")).toString(),
            progress < 0 ? QStringLiteral("—")
                         : QLocale().toString(progress * 100, 'f', 1) + QLatin1Char('%'),
            speedText(p.value(QStringLiteral("dl_speed"))),
            speedText(p.value(QStringLiteral("up_speed"))),
            p.value(QStringLiteral("flags")).toString(),
            p.value(QStringLiteral("connection")).toString()
        };
        for (int col = 0; col < values.size(); ++col)
        {
            auto *item = new SortablePeerItem(values.at(col));
            const QJsonValue raw = col == 3 ? p.value(QStringLiteral("progress"))
                : col == 4 ? p.value(QStringLiteral("dl_speed"))
                : col == 5 ? p.value(QStringLiteral("up_speed")) : QJsonValue();
            if (raw.isDouble() && raw.toDouble() >= 0)
                item->setData(Qt::UserRole, raw.toDouble());
            m_peers->setItem(row, col, item);
        }
    }
    m_peers->setSortingEnabled(true);
}

void TorrentDetailsWidget::displayWebSeeds(const QJsonArray &sources)
{
    m_ui->listWebSeeds->clear();
    for (const QJsonValue &entry : sources)
    {
        const QString address = entry.isObject()
            ? entry.toObject().value(QStringLiteral("url")).toString()
            : entry.toString();
        if (!address.isEmpty())
            m_ui->listWebSeeds->addItem(address);
    }
}

void TorrentDetailsWidget::setContentPathMappings(
    const QList<Remote::PathMapping> &mappings)
{
    m_ui->filesList->setPathMappings(mappings);
}

void TorrentDetailsWidget::setContentDragEnabled(bool enabled)
{
    m_ui->filesList->setContentDragEnabled(enabled);
}

void TorrentDetailsWidget::displayFiles(const QJsonArray &files)
{
    auto *tree = m_ui->filesList;
    QSignalBlocker blocker(tree);
    tree->setSortingEnabled(false);
    tree->clear();
    QHash<QString, QTreeWidgetItem *> folders;
    const QMimeDatabase mimeDatabase;

    for (const QJsonValue &entry : files)
    {
        const QJsonObject file = entry.toObject();
        const int id = file.value(QStringLiteral("index")).toInt(-1);
        if (id < 0)
            continue;
        const QStringList parts = file.value(QStringLiteral("name")).toString()
                                      .split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (parts.isEmpty())
            continue;

        QString path;
        QTreeWidgetItem *parent = nullptr;
        for (int i = 0; i + 1 < parts.size(); ++i)
        {
            path += QLatin1Char('/') + parts.at(i);
            if (!folders.contains(path))
            {
                auto *folder = parent ? new SortableDetailTreeItem(parent)
                                      : new SortableDetailTreeItem(tree);
                folder->setText(0, parts.at(i));
                folder->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
                folder->setFlags(folder->flags() | Qt::ItemIsUserCheckable);
                folders.insert(path, folder);
            }
            parent = folders.value(path);
        }

        auto *item = parent ? new SortableDetailTreeItem(parent)
                            : new SortableDetailTreeItem(tree);
        item->setText(0, parts.last());
        // Determine icons strictly from filename extension: never stat or
        // navigate the server's arbitrary file paths on the local machine.
        const QMimeType mime = mimeDatabase.mimeTypeForFile(
            parts.last(), QMimeDatabase::MatchExtension);
        QIcon icon = QIcon::fromTheme(mime.iconName());
        if (icon.isNull())
            icon = QIcon::fromTheme(mime.genericIconName());
        if (icon.isNull())
            icon = style()->standardIcon(QStyle::SP_FileIcon);
        item->setIcon(0, icon);

        const qint64 size = file.value(QStringLiteral("size")).toInteger(0);
        const double fraction = qBound(0.0,
             file.value(QStringLiteral("progress")).toDouble(0.0), 1.0);
        const int priority = file.value(QStringLiteral("priority")).toInt(1);
        item->setText(1, bytesText(size));
        item->setData(1, Qt::UserRole, size);
        item->setText(2, QLocale().toString(fraction * 100.0, 'f', 0)
                             + QLatin1Char('%'));
        item->setData(2, Qt::UserRole, fraction);
        item->setText(3, priorityText(priority));
        item->setData(3, Qt::UserRole, priority);
        const double remaining = std::max(0.0, size * (1.0 - fraction));
        item->setText(4, bytesText(remaining));
        item->setData(4, Qt::UserRole, remaining);
        item->setText(5, availabilityText(file.value(QStringLiteral("availability"))));
        setNumericSortKey(item, 5, file.value(QStringLiteral("availability")));
        if (!file.contains(QStringLiteral("availability")))
            item->setText(5, QStringLiteral("N/A"));

        item->setData(0, Qt::UserRole, id);
        item->setData(0, Qt::UserRole + 1, size);
        item->setData(0, Qt::UserRole + 2, fraction);
        item->setData(0, Qt::UserRole + 3, priority);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, priority == 0 ? Qt::Unchecked : Qt::Checked);
    }
    updateFileFolders();
    tree->setSortingEnabled(true);
    tree->expandToDepth(0);
    // Preserve a user's active filename filter across server refreshes.
    if (auto *filter = findChild<QLineEdit *>(QStringLiteral("torrentFileFilter")))
    {
        const QString needle = filter->text();
        if (!needle.isEmpty())
        {
            const auto filterItem = [&needle](QTreeWidgetItem *item,
                                              const auto &self) -> bool {
                bool hasMatch = item->text(0).contains(needle, Qt::CaseInsensitive);
                for (int i = 0; i < item->childCount(); ++i)
                    hasMatch = self(item->child(i), self) || hasMatch;
                item->setHidden(!hasMatch);
                return hasMatch;
            };
            for (int i = 0; i < tree->topLevelItemCount(); ++i)
                filterItem(tree->topLevelItem(i), filterItem);
        }
    }
}

void TorrentDetailsWidget::updateFileFolders()
{
    auto *tree = m_ui->filesList;
    QSignalBlocker blocker(tree);
    const bool wasSorting = tree->isSortingEnabled();
    tree->setSortingEnabled(false);
    struct Summary
    {
        qint64 size = 0;
        double completed = 0;
        int selected = 0;
        int total = 0;
        int commonPriority = -1;
    };
    const auto visit = [](QTreeWidgetItem *node,
                          const auto &self) -> Summary {
        const QVariant fileId = node->data(0, Qt::UserRole);
        if (fileId.isValid())
        {
            const qint64 size = node->data(0, Qt::UserRole + 1).toLongLong();
            const double fraction = node->data(0, Qt::UserRole + 2).toDouble();
            const int priority = node->data(0, Qt::UserRole + 3).toInt();
            return {size, size * fraction, priority != 0 ? 1 : 0, 1, priority};
        }

        Summary summary;
        for (int i = 0; i < node->childCount(); ++i)
        {
            const Summary child = self(node->child(i), self);
            summary.size += child.size;
            summary.completed += child.completed;
            summary.selected += child.selected;
            summary.total += child.total;
            if (summary.commonPriority == -1)
                summary.commonPriority = child.commonPriority;
            else if (summary.commonPriority != child.commonPriority)
                summary.commonPriority = -2;
        }
        const double fraction = summary.size
            ? qBound(0.0, summary.completed / summary.size, 1.0) : 0.0;
        node->setText(1, bytesText(summary.size));
        node->setData(1, Qt::UserRole, summary.size);
        node->setText(2, QLocale().toString(fraction * 100.0, 'f', 0)
                             + QLatin1Char('%'));
        node->setData(2, Qt::UserRole, fraction);
        node->setText(3, summary.commonPriority >= 0
                         ? priorityText(summary.commonPriority) : QStringLiteral("Mixed"));
        node->setData(3, Qt::UserRole, summary.commonPriority >= 0
            ? QVariant(summary.commonPriority) : QVariant());
        const double remaining = std::max(0.0, summary.size - summary.completed);
        node->setText(4, bytesText(remaining));
        node->setData(4, Qt::UserRole, remaining);
        node->setText(5, QStringLiteral("N/A"));
        node->setCheckState(0, summary.selected == 0 ? Qt::Unchecked
            : (summary.selected == summary.total ? Qt::Checked : Qt::PartiallyChecked));
        return summary;
    };
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        visit(tree->topLevelItem(i), visit);
    tree->setSortingEnabled(wasSorting);
}

void TorrentDetailsWidget::chooseFilePriorities(int priority, bool allFiles)
{
    if (m_hash.isEmpty())
        return;
    auto *tree = m_ui->filesList;
    QList<int> ids;
    const auto gather = [&ids](QTreeWidgetItem *root, const auto &self) -> void {
        if (root->data(0, Qt::UserRole).isValid())
            ids.append(root->data(0, Qt::UserRole).toInt());
        for (int j = 0; j < root->childCount(); ++j)
            self(root->child(j), self);
    };

    if (allFiles)
    {
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
            gather(tree->topLevelItem(i), gather);
    }
    else
    {
        for (QTreeWidgetItem *item : tree->selectedItems())
            gather(item, gather);
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    if (ids.isEmpty())
        return;

    // Immediate native checkbox feedback; the authoritative remote state
    // will replace it when /torrents/files refreshes on server acceptance.
    {
        QSignalBlocker blocker(tree);
        const bool wasSorting = tree->isSortingEnabled();
        tree->setSortingEnabled(false);
        const auto update = [&ids, priority, allFiles](QTreeWidgetItem *node,
                                                       const auto &self) -> void {
            const QVariant fileId = node->data(0, Qt::UserRole);
            if (fileId.isValid())
            {
                if (std::binary_search(ids.cbegin(), ids.cend(), fileId.toInt()))
                {
                    const int current = node->data(0, Qt::UserRole + 3).toInt();
                    const int next = allFiles && priority == 1 && current > 0
                        ? current : priority;
                    node->setData(0, Qt::UserRole + 3, next);
                    node->setCheckState(0, next ? Qt::Checked : Qt::Unchecked);
                    node->setText(3, priorityText(next));
                    node->setData(3, Qt::UserRole, next);
                }
            }
            for (int j = 0; j < node->childCount(); ++j)
                self(node->child(j), self);
        };
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
            update(tree->topLevelItem(i), update);
        updateFileFolders();
        tree->setSortingEnabled(wasSorting);
    }
    emit filePriorityRequested(m_hash, ids, priority);
}

void TorrentDetailsWidget::displayDetails(const QString &hash, int tab,
                                         const QJsonValue &data)
{
    if (hash != m_hash || tab != m_currentTab)
        return;
    m_error->hide();
    switch (tab)
    {
    case 0: displayGeneral(data.toObject()); break;
    case 1: displayTrackers(data.toArray()); break;
    case 2: displayPeers(data.toObject()); break;
    case 3: displayWebSeeds(data.toArray()); break;
    case 4: displayFiles(data.toArray()); break;
    default: break;
    }
}

void TorrentDetailsWidget::displayError(const QString &hash, int tab, const QString &reason)
{
    if (hash != m_hash || tab != m_currentTab)
        return;
    m_error->setText(tr("Could not load torrent details: %1").arg(reason));
    m_error->show();
}

void TorrentDetailsWidget::appendRates(double downloadBytesPerSec,
                                       double uploadBytesPerSec)
{
    m_speedGraph->append(downloadBytesPerSec, uploadBytesPerSec);
}
