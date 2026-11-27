// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/TorrentDetailsWidget.h"
#include "gui/BytePresentation.h"
#include "gui/DatePresentation.h"
#include "gui/MappedContentTreeWidget.h"
#include "gui/NaturalNameSort.h"
#include "gui/ProgressDisplay.h"
#include "api/PeerEndpoint.h"
#include "ui_remotepropertieswidget.h"
#include "gui/upstream/progressbarpainter.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QShortcut>
#include <QKeySequence>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPlainTextEdit>
#include <QMessageBox>
#include <QInputDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QScreen>
#include <QIcon>
#include <QItemSelectionModel>
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
#include <QScrollBar>
#include <QScopedValueRollback>
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
#include <utility>

namespace
{
QString bytesText(double amount, bool perSecond = false)
{
    return BytePresentation::format(amount, 1, perSecond, 5);
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

QString dateText(const QJsonValue &value, const QString &pattern)
{
    return DatePresentation::format(value.toInteger(), pattern);
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
                            ProgressDisplay::roundedValue(fraction, 100));
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

// Remote adaptation of upstream DownloadedPiecesBar / PieceAvailabilityBar
// (Copyright 2006 Christophe Dumez; 2024 Vladimir Golovnev).
class RemotePiecesBar final : public QProgressBar
{
public:
    explicit RemotePiecesBar(bool availability, QWidget *parent)
        : QProgressBar(parent), m_availability(availability)
    {
        setTextVisible(false);
        setRange(0, 1000);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMinimumSize(64, 14);
        clearPieces();
    }

    void clearPieces()
    {
        if (!m_pieces.isEmpty()) m_pieces.clear();
        m_maximum = 0;
        setProperty("pieceCount", 0);
        setToolTip(m_availability ? tr("Piece availability is unavailable.")
            : tr("Overall progress (piece data unavailable)."));
        update();
    }

    bool setPieces(const QJsonArray &values)
    {
        QList<int> pieces;
        pieces.reserve(values.size());
        int maximum = 0;
        for (const auto &value : values)
        {
            const auto number = value.toInteger(-1);
            if (number < 0 || number > (m_availability ? 2147483647 : 2))
            {
                clearPieces();
                return false;
            }
            pieces.append(static_cast<int>(number));
            maximum = qMax(maximum, static_cast<int>(number));
        }
        if (pieces.isEmpty())
        {
            clearPieces();
            return false;
        }
        m_pieces = std::move(pieces);
        m_maximum = maximum;
        setProperty("pieceCount", m_pieces.size());
        const auto row = [](const QColor &color, const QString &text) {
            return QStringLiteral("<tr><td width='20' bgcolor='%1'></td><td>%2</td></tr>")
                .arg(color.name(), text.toHtmlEscaped());
        };
        const auto bg = palette().color(QPalette::Base);
        const auto highlight = palette().color(QPalette::Highlight);
        setToolTip(QStringLiteral("<table cellspacing='4'>")
            + (m_availability
                ? row(bg, tr("Unavailable pieces")) + row(highlight, tr("Available pieces"))
                : row(bg, tr("Missing pieces")) + row(partialColor(), tr("Partial pieces"))
                    + row(highlight, tr("Completed pieces")))
            + QStringLiteral("</table>"));
        update();
        return true;
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        if (m_pieces.isEmpty() && !m_availability)
        {
            QProgressBar::paintEvent(event);
            return;
        }
        QPainter painter(this);
        const QRect area = rect().adjusted(1, 1, -1, -1);
        if (area.isEmpty()) return;
        const QColor bg = palette().color(QPalette::Base);
        const QColor full = palette().color(QPalette::Highlight);
        const QColor partial = partialColor();
        painter.fillRect(rect(), palette().color(QPalette::Dark));
        painter.fillRect(area, bg);
        if (m_pieces.isEmpty()) return;
        // Weighted pixel coverage preserves relative distribution for large
        // piece arrays. Availability is normalized to its peak, as upstream.
        const double scale = static_cast<double>(m_pieces.size()) / area.width();
        for (int x = 0; x < area.width(); ++x)
        {
            const double from = x * scale;
            const double to = qMin(static_cast<double>(m_pieces.size()), (x + 1) * scale);
            double complete = 0, downloading = 0;
            for (qsizetype i = static_cast<qsizetype>(from); i < m_pieces.size() && i < to; ++i)
            {
                const double weight = qMax(0.0, qMin(to, static_cast<double>(i + 1))
                    - qMax(from, static_cast<double>(i)));
                const int piece = m_pieces.at(i);
                if (m_availability)
                    complete += m_maximum > 0 ? weight * piece / m_maximum : 0;
                else if (piece == 2) complete += weight;
                else if (piece == 1) downloading += weight;
            }
            const double a = qBound(0.0, complete / scale, 1.0);
            const double b = qBound(0.0, downloading / scale, 1.0 - a);
            const auto mix = [&](int base, int done, int pending) {
                return qRound(base * (1 - a - b) + done * a + pending * b);
            };
            const QColor color(mix(bg.red(), full.red(), partial.red()),
                mix(bg.green(), full.green(), partial.green()),
                mix(bg.blue(), full.blue(), partial.blue()));
            painter.fillRect(area.left() + x, area.top(), 1, area.height(), color);
        }
    }

private:
    QColor partialColor() const
    {
        const auto full = palette().color(QPalette::Highlight);
        return QColor::fromHsl(120, full.hslSaturation(), full.lightness());
    }
    const bool m_availability;
    QList<int> m_pieces;
    int m_maximum = 0;
};

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
    void refreshBytePresentation() { m_graph->update(); }
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
    m_trackers->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_trackers, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QMenu menu(this);
        menu.setToolTipsVisible(true);
        const auto selected = trackerUrls(true);
        const auto action = [&](const QString &name, const QString &label, const QString &icon,
                                bool enabled, auto handler) {
            auto *item = menu.addAction(QIcon(QStringLiteral(":/icons/") + icon + QStringLiteral(".svg")), label);
            item->setObjectName(name);
            item->setEnabled(enabled);
            connect(item, &QAction::triggered, this, handler);
            return item;
        };
        auto *add = action("addDetailTrackers", tr("Add trackers..."), "list-add", canAddTrackers(),
            [this]() { addTrackers(); });
        if (!canAddTrackers()) add->setToolTip(tr("Additional trackers are blocked on private or unknown torrents."));
        action("editDetailTracker", tr("Edit tracker URL..."), "edit-rename",
            canChangeTrackers() && selected.size() == 1, [this]() { editTracker(); });
        action("removeDetailTrackers", tr("Remove tracker"), "list-remove",
            canChangeTrackers() && !selected.isEmpty(), [this]() { removeTrackers(); });
        action("copyDetailTrackers", tr("Copy tracker URL"), "edit-copy",
            !selected.isEmpty(), [this]() { copyTrackers(); });
        auto *announce = action("reannounceSelectedTrackers", tr("Force reannounce to selected trackers"), "reannounce",
            canReannounceTrackers() && m_selectedTrackerReannounceSupported && !selected.isEmpty(),
            [this]() { reannounceTrackers(true); });
        if (!m_selectedTrackerReannounceSupported)
            announce->setToolTip(tr("Requires qBittorrent 5.2 or newer."));
        menu.addSeparator();
        action("reannounceAllTrackers", tr("Force reannounce to all trackers"), "reannounce",
            canReannounceTrackers(), [this]() { reannounceTrackers(false); });
        menu.exec(m_trackers->viewport()->mapToGlobal(pos));
    });
    const auto trackerShortcut = [this](const QKeySequence &key, auto slot) {
        auto *shortcut = new QShortcut(key, m_trackers);
        shortcut->setContext(Qt::WidgetShortcut);
        connect(shortcut, &QShortcut::activated, this, slot);
    };
    trackerShortcut(QKeySequence(Qt::Key_F2), &TorrentDetailsWidget::editTracker);
    trackerShortcut(QKeySequence::Delete, &TorrentDetailsWidget::removeTrackers);
    trackerShortcut(QKeySequence::Copy, &TorrentDetailsWidget::copyTrackers);
    connect(m_trackers, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
        if (item && !item->parent() && !isPseudoTracker(item->text(0))) editTracker();
    });
    auto *trackerHeader = m_trackers->header();
    trackerHeader->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(trackerHeader, &QWidget::customContextMenuRequested, this, [this, trackerHeader](const QPoint &pos) {
        QMenu menu(this);
        for (int col = 0; col < m_trackers->columnCount(); ++col)
        {
            auto *item = menu.addAction(m_trackers->headerItem()->text(col));
            item->setCheckable(true);
            item->setChecked(!m_trackers->isColumnHidden(col));
            if (item->isChecked() && trackerHeader->count() - trackerHeader->hiddenSectionCount() <= 1)
                item->setEnabled(false);
            connect(item, &QAction::triggered, this, [this, col](bool checked) {
                m_trackers->setColumnHidden(col, !checked);
                if (checked && m_trackers->columnWidth(col) <= 5) m_trackers->resizeColumnToContents(col);
            });
        }
        menu.addSeparator();
        auto *resize = menu.addAction(tr("Resize columns"));
        connect(resize, &QAction::triggered, this, [this]() {
            for (int col = 0; col < m_trackers->columnCount(); ++col)
                if (!m_trackers->isColumnHidden(col)) m_trackers->resizeColumnToContents(col);
        });
        menu.exec(trackerHeader->mapToGlobal(pos));
    });
    m_ui->hBoxLayoutTrackers->insertWidget(0, m_trackers, 1);
    m_ui->trackerUpButton->setIcon(style()->standardIcon(QStyle::SP_ArrowUp));
    m_ui->trackerDownButton->setIcon(style()->standardIcon(QStyle::SP_ArrowDown));
    connect(m_trackers, &QTreeWidget::itemSelectionChanged,
            this, &TorrentDetailsWidget::updateTrackerTierButtons);
    connect(m_ui->trackerUpButton, &QPushButton::clicked, this,
            [this]() { changeTrackerTiers(-1); });
    connect(m_ui->trackerDownButton, &QPushButton::clicked, this,
            [this]() { changeTrackerTiers(1); });
    updateTrackerTierButtons();

    m_peers = new QTableWidget(this);
    m_peers->setObjectName(QStringLiteral("torrentPeers"));
    m_peers->setColumnCount(15);
    m_peers->setHorizontalHeaderLabels({
        tr("IP/Address"), tr("Country/Region"), tr("Client"), tr("Progress"), tr("Down Speed"),
        tr("Up Speed"), tr("Flags"), tr("Connection"), tr("Port"), tr("Downloaded"),
        tr("Uploaded"), tr("Relevance"), tr("Contribution"), tr("Files"), tr("Peer ID Client")
    });
    m_peers->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_peers->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_peers->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_peers->verticalHeader()->hide();
    m_peers->setAlternatingRowColors(true);
    m_peers->horizontalHeader()->setStretchLastSection(true);
    m_peers->horizontalHeader()->setSectionsMovable(true);
    m_peers->horizontalHeader()->setHighlightSections(false);
    m_peers->setSortingEnabled(true);
    m_peers->sortItems(0, Qt::AscendingOrder);
    m_peers->horizontalHeader()->setStretchLastSection(false);
    m_peers->setColumnHidden(14, true); // upstream hides Peer ID Client by default
    m_peers->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_peers, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QMenu menu(this);
        auto *add = menu.addAction(QIcon(QStringLiteral(":/icons/peers-add.svg")), tr("Add peers..."));
        add->setObjectName(QStringLiteral("addTorrentPeers"));
        add->setEnabled(canAddPeers());
        add->setToolTip(tr("Requires a non-private torrent that is not queued or checking."));
        connect(add, &QAction::triggered, this, &TorrentDetailsWidget::addPeers);
        auto *copy = menu.addAction(QIcon(QStringLiteral(":/icons/edit-copy.svg")), tr("Copy IP:port"));
        copy->setObjectName(QStringLiteral("copySelectedPeers"));
        copy->setEnabled(!m_peers->selectionModel()->selectedRows().isEmpty());
        connect(copy, &QAction::triggered, this, &TorrentDetailsWidget::copySelectedPeers);
        menu.addSeparator();
        auto *ban = menu.addAction(QIcon(QStringLiteral(":/icons/peers-remove.svg")), tr("Ban peer permanently"));
        ban->setObjectName(QStringLiteral("banTorrentPeers"));
        ban->setEnabled(!m_hash.isEmpty() && m_pendingPeerHash.isEmpty()
            && !selectedPeerEndpoints().isEmpty());
        connect(ban, &QAction::triggered, this, &TorrentDetailsWidget::banSelectedPeers);
        menu.setToolTipsVisible(true);
        menu.exec(m_peers->viewport()->mapToGlobal(pos));
    });
    auto *copyShortcut = new QShortcut(QKeySequence::Copy, m_peers);
    copyShortcut->setContext(Qt::WidgetShortcut);
    connect(copyShortcut, &QShortcut::activated, this, &TorrentDetailsWidget::copySelectedPeers);
    auto *banShortcut = new QShortcut(QKeySequence::Delete, m_peers);
    banShortcut->setContext(Qt::WidgetShortcut);
    connect(banShortcut, &QShortcut::activated, this, &TorrentDetailsWidget::banSelectedPeers);
    auto *header = m_peers->horizontalHeader();
    header->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(header, &QWidget::customContextMenuRequested, this, [this, header](const QPoint &pos) {
        QMenu menu(this);
        for (int col = 0; col < m_peers->columnCount(); ++col)
        {
            auto *action = menu.addAction(m_peers->horizontalHeaderItem(col)->text());
            action->setCheckable(true);
            action->setChecked(!m_peers->isColumnHidden(col));
            if (action->isChecked() && header->count() - header->hiddenSectionCount() <= 1)
                action->setEnabled(false);
            connect(action, &QAction::triggered, this, [this, col](bool checked) {
                m_peers->setColumnHidden(col, !checked);
                if (checked && m_peers->columnWidth(col) <= 5)
                    m_peers->resizeColumnToContents(col);
            });
        }
        menu.addSeparator();
        auto *resize = menu.addAction(tr("Resize columns"));
        connect(resize, &QAction::triggered, this, [this]() {
            for (int col = 0; col < m_peers->columnCount(); ++col)
                if (!m_peers->isColumnHidden(col)) m_peers->resizeColumnToContents(col);
        });
        menu.exec(header->mapToGlobal(pos));
    });
    m_ui->vBoxLayoutPeerPage->addWidget(m_peers);

    auto *sources = m_ui->listWebSeeds;
    sources->setObjectName(QStringLiteral("torrentWebSeeds"));
    sources->setSelectionMode(QAbstractItemView::ExtendedSelection);
    sources->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(sources, &QWidget::customContextMenuRequested, this, [this, sources](const QPoint &pos) {
        QMenu menu(this);
        const auto selected = selectedWebSeeds();
        auto *add = menu.addAction(QIcon(QStringLiteral(":/icons/list-add.svg")), tr("Add web seed..."));
        add->setObjectName(QStringLiteral("addWebSeed"));
        add->setEnabled(canEditWebSeeds());
        connect(add, &QAction::triggered, this, &TorrentDetailsWidget::addWebSeed);
        auto *remove = menu.addAction(QIcon(QStringLiteral(":/icons/list-remove.svg")), tr("Remove web seed"));
        remove->setObjectName(QStringLiteral("removeWebSeeds"));
        remove->setEnabled(canEditWebSeeds() && !selected.isEmpty());
        connect(remove, &QAction::triggered, this, &TorrentDetailsWidget::removeWebSeeds);
        menu.addSeparator();
        auto *copy = menu.addAction(QIcon(QStringLiteral(":/icons/edit-copy.svg")), tr("Copy web seed URL"));
        copy->setObjectName(QStringLiteral("copyWebSeeds"));
        copy->setEnabled(!selected.isEmpty());
        connect(copy, &QAction::triggered, this, &TorrentDetailsWidget::copyWebSeeds);
        auto *edit = menu.addAction(QIcon(QStringLiteral(":/icons/edit-rename.svg")), tr("Edit web seed URL..."));
        edit->setObjectName(QStringLiteral("editWebSeed"));
        edit->setEnabled(canEditWebSeeds() && selected.size() == 1);
        connect(edit, &QAction::triggered, this, &TorrentDetailsWidget::editWebSeed);
        menu.exec(sources->viewport()->mapToGlobal(pos));
    });
    const auto shortcut = [this, sources](const QKeySequence &key, auto slot) {
        auto *action = new QShortcut(key, sources);
        action->setContext(Qt::WidgetShortcut);
        connect(action, &QShortcut::activated, this, slot);
    };
    shortcut(QKeySequence(Qt::Key_F2), &TorrentDetailsWidget::editWebSeed);
    shortcut(QKeySequence::Delete, &TorrentDetailsWidget::removeWebSeeds);
    shortcut(QKeySequence::Copy, &TorrentDetailsWidget::copyWebSeeds);
    connect(sources, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) { editWebSeed(); });

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
    tree->header()->setFirstSectionMovable(true);
    tree->header()->setSectionResizeMode(QHeaderView::Interactive);
    tree->setColumnWidth(0, 260);
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
    tree->setExpandsOnDoubleClick(false);
    tree->sortItems(0, Qt::AscendingOrder);
    auto *contentHeader = tree->header();
    contentHeader->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(contentHeader, &QWidget::customContextMenuRequested, this, [this, tree, contentHeader](const QPoint &pos) {
        QMenu menu(this);
        menu.setObjectName(QStringLiteral("contentHeaderMenu"));
        menu.setTitle(tr("Column visibility"));
        menu.setToolTipsVisible(true);
        for (int col = 0; col < tree->columnCount(); ++col)
        {
            auto *action = menu.addAction(tree->headerItem()->text(col));
            action->setObjectName(QStringLiteral("contentColumn%1").arg(col));
            action->setCheckable(true);
            action->setChecked(!tree->isColumnHidden(col));
            action->setEnabled(col != 0);
            connect(action, &QAction::triggered, this, [tree, col](bool visible) {
                tree->setColumnHidden(col, !visible);
                if (visible && tree->columnWidth(col) <= 5)
                    tree->resizeColumnToContents(col);
            });
        }
        menu.addSeparator();
        auto *resize = menu.addAction(tr("Resize columns"));
        resize->setObjectName(QStringLiteral("resizeContentColumns"));
        resize->setToolTip(tr("Resize all non-hidden columns to the size of their contents"));
        connect(resize, &QAction::triggered, this, [tree]() {
            for (int col = 0; col < tree->columnCount(); ++col)
                if (!tree->isColumnHidden(col)) tree->resizeColumnToContents(col);
        });
        menu.exec(contentHeader->mapToGlobal(pos));
    });
    connect(tree, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        if (m_hash.isEmpty() || m_ui->filesList->selectedItems().isEmpty())
            return;
        QMenu menu(this);
        menu.setObjectName(QStringLiteral("torrentContentContextMenu"));
        menu.setToolTipsVisible(true);
        m_ui->filesList->addMappedOpenActions(menu);
        auto *rename = menu.addAction(QIcon(QStringLiteral(":/icons/edit-rename.svg")), tr("Rename..."));
        rename->setObjectName(QStringLiteral("contentRenameItem"));
        rename->setIconVisibleInMenu(true);
        rename->setEnabled(!selectedContentPath().isEmpty());
        connect(rename, &QAction::triggered, this, &TorrentDetailsWidget::renameSelectedContent);
        auto *batchRename = menu.addAction(QIcon(QStringLiteral(":/icons/edit-rename.svg")), tr("Batch rename..."));
        batchRename->setObjectName(QStringLiteral("contentBatchRename"));
        batchRename->setIconVisibleInMenu(true);
        const QString hash = m_hash;
        const quint64 generation = m_contentSelectionGeneration;
        const QList<int> ids = selectedContentFileIds();
        batchRename->setEnabled(!ids.isEmpty());
        connect(batchRename, &QAction::triggered, this, [this, hash, generation, ids]() {
            if (hash == m_hash && generation == m_contentSelectionGeneration
                && ids == selectedContentFileIds())
                batchRenameSelectedContent();
        });
        menu.addSeparator();
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
        priorityMenu->addSeparator();
        auto *byOrder = priorityMenu->addAction(tr("By shown file order"));
        byOrder->setObjectName(QStringLiteral("contentPriorityByOrder"));
        const auto groups = selectedContentPriorityGroups();
        byOrder->setEnabled(!groups.isEmpty());
        connect(byOrder, &QAction::triggered, this, [this, hash, generation, groups]() {
            if (contentBatchSelectionValid(hash, generation)
                && groups == selectedContentPriorityGroups())
                chooseFilePrioritiesByOrder(groups);
        });
        menu.exec(m_ui->filesList->viewport()->mapToGlobal(pos));
    });
    auto *renameShortcut = new QShortcut(QKeySequence(Qt::Key_F2), tree);
    renameShortcut->setContext(Qt::WidgetShortcut);
    connect(renameShortcut, &QShortcut::activated, this, &TorrentDetailsWidget::renameSelectedContent);
    connect(tree, &QTreeWidget::itemDoubleClicked, this, [this, tree](QTreeWidgetItem *item, int) {
        if (!item || tree->selectedItems().size() != 1 || tree->selectedItems().first() != item
            || selectedContentPath().isEmpty()) return;
        if (m_contentDoubleClickRename) renameSelectedContent();
        else tree->openSelectedMappedItem();
    });
    connect(tree, &MappedContentTreeWidget::openByEnterRequested, this, [this, tree]() {
        if (!selectedContentPath().isEmpty()) tree->openSelectedMappedItem();
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
    fileFilter->setMaximumWidth(300);
    m_ui->contentFilterLayout->addWidget(fileFilter);
    fileFilter->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(fileFilter, &QLineEdit::textChanged, this,
            [this]() { applyContentFilter(true); });
    connect(fileFilter, &QWidget::customContextMenuRequested, this, [this, fileFilter](const QPoint &pos) {
        auto *menu = fileFilter->createStandardContextMenu();
        menu->setObjectName(QStringLiteral("contentFilterMenu"));
        menu->addSeparator();
        auto *formats = menu->addMenu(tr("Pattern format"));
        auto *group = new QActionGroup(formats);
        group->setExclusive(true);
        for (const auto &option : {
                qMakePair(QStringLiteral("plain"), tr("Plain text")),
                qMakePair(QStringLiteral("wildcards"), tr("Wildcards")),
                qMakePair(QStringLiteral("regex"), tr("Regular expression"))})
        {
            auto *action = formats->addAction(option.second);
            action->setObjectName(QStringLiteral("contentFilter_") + option.first);
            group->addAction(action);
            action->setCheckable(true);
            action->setChecked(m_contentFilterFormat == option.first);
            connect(action, &QAction::triggered, this, [this, option]() {
                setContentFilterFormat(option.first);
            });
        }
        menu->exec(fileFilter->mapToGlobal(pos));
        delete menu;
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

    connect(m_ui->editCommentButton, &QPushButton::clicked, this, &TorrentDetailsWidget::editComment);
    m_speedGraph = new SpeedGraphWidget(m_ui->pageSpeed);
    m_ui->speedLayout->addWidget(m_speedGraph);

    // Use the original native layout with remote, palette-aware piece strips.
    m_progressBar = new RemotePiecesBar(false, this);
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
    m_ui->groupBarLayout->setColumnStretch(1, 1);
    m_availabilityBar = new RemotePiecesBar(true, this);
    m_availabilityBar->setObjectName(QStringLiteral("detailsAvailabilityBar"));
    if (QLayoutItem *removed = layout ? layout->replaceWidget(
            m_ui->tempAvailabilityBarArea, m_availabilityBar) : nullptr)
        delete removed;
    m_ui->tempAvailabilityBarArea->hide();
    m_ui->labelPiecesAvailability->hide();
    m_availabilityBar->hide();
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
    if (m_currentTab != tab && tab == 1)
        m_trackerTiersKnown = false;
    if (m_currentTab != tab)
    {
        m_peerPrivacy = QJsonValue();
        m_webSeedsKnown = false;
        m_contentKnown = false;
        ++m_contentSelectionGeneration;
    }
    m_currentTab = tab;
    m_ui->stackedProperties->setCurrentIndex(tab);
    updateCommentEditor();
    updateTrackerTierButtons();
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
    m_commentKnown = false;
    updateCommentEditor();
    m_progressBar->setValue(0);
    m_progressBar->clearPieces();
    m_availabilityBar->clearPieces();
    m_ui->labelPiecesAvailability->hide();
    m_availabilityBar->hide();
    m_ui->labelAverageAvailabilityVal->hide();
    setValue("labelAverageAvailabilityVal", QStringLiteral("—"));
    m_trackerTiersKnown = false;
    m_trackers->clear();
    updateTrackerTierButtons();
    m_peerPrivacy = QJsonValue();
    m_peerRecords = {};
    m_peers->setRowCount(0);
    m_webSeedsKnown = false;
    m_ui->listWebSeeds->clear();
    m_ui->filesList->clear();
    m_contentKnown = false;
    ++m_contentSelectionGeneration;
    m_error->hide();
    m_ui->selectAllButton->setEnabled(!m_hash.isEmpty());
    m_ui->selectNoneButton->setEnabled(!m_hash.isEmpty());
}

void TorrentDetailsWidget::setSelection(const QString &hash, const QJsonObject &summary)
{
    if (m_hash != hash)
    {
        m_hash = hash;
        m_lastGeneral = {};
        resetPages();
    }
    m_summary = summary;
    if (!wantsPieceAvailability())
    {
        if (!m_availabilityBar->isHidden()) m_availabilityBar->clearPieces();
        m_availabilityBar->hide();
        m_ui->labelPiecesAvailability->hide();
        m_ui->labelAverageAvailabilityVal->hide();
    }
    m_ui->filesList->setRemoteRoots(summary.value(QStringLiteral("save_path")).toString(),
                                   summary.value(QStringLiteral("content_path")).toString());
    if (hash.isEmpty())
        return;
    const double progress = summary.value(QStringLiteral("progress")).toDouble();
    m_progressBar->setValue(ProgressDisplay::roundedValue(progress, 1000));
    setValue("labelProgressVal", ProgressDisplay::percent(progress));
    setValue("labelInfohash1Val",
             summary.value(QStringLiteral("infohash_v1")).toString(hash));
    setValue("labelInfohash2Val",
             summary.value(QStringLiteral("infohash_v2")).toString(QStringLiteral("—")));
    setValue("labelSavePathVal",
             summary.value(QStringLiteral("save_path")).toString(QStringLiteral("—")));
}

bool TorrentDetailsWidget::wantsPieceAvailability() const
{
    const auto state = m_summary.value(QStringLiteral("state")).toString();
    const auto progress = m_summary.value(QStringLiteral("progress"));
    return !m_hash.isEmpty() && progress.isDouble() && progress.toDouble() < 1.0
        && (state == QLatin1String("downloading") || state == QLatin1String("stalledDL")
            || state == QLatin1String("forcedDL"));
}

void TorrentDetailsWidget::refreshBytePresentation()
{
    if (!m_lastGeneral.isEmpty()) displayGeneral(m_lastGeneral);
    const bool peerSorting = m_peers->isSortingEnabled();
    m_peers->setSortingEnabled(false);
    for (int row = 0; row < m_peers->rowCount(); ++row)
        for (int col : {4, 5, 9, 10})
            if (auto *item = m_peers->item(row, col))
            {
                const auto raw = item->data(Qt::UserRole);
                item->setText(raw.isValid() ? bytesText(raw.toDouble(), col == 4 || col == 5)
                    : QStringLiteral("—"));
            }
    m_peers->setSortingEnabled(peerSorting);
    auto *tree = m_ui->filesList;
    QSignalBlocker blocker(tree);
    const bool contentSorting = tree->isSortingEnabled();
    tree->setSortingEnabled(false);
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
        for (int col : {1, 4})
        {
            const auto raw = (*it)->data(col, Qt::UserRole);
            if (raw.isValid()) (*it)->setText(col, bytesText(raw.toDouble()));
        }
    tree->setSortingEnabled(contentSorting);
    m_speedGraph->refreshBytePresentation();
}

void TorrentDetailsWidget::setDateFormat(const QString &pattern)
{
    if (m_dateFormat == pattern) return;
    m_dateFormat = pattern;
    if (!m_lastGeneral.isEmpty()) displayGeneral(m_lastGeneral);
}

void TorrentDetailsWidget::displayGeneral(const QJsonObject &p)
{
    m_lastGeneral = p;
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
    setValue("labelLastSeenCompleteVal", dateText(p.value(QStringLiteral("last_seen")), m_dateFormat));
    if (const auto v = p.value(QStringLiteral("popularity")); v.isDouble())
        setValue("labelPopularityVal", QLocale().toString(v.toDouble(), 'f', 2));
    setValue("labelTotalSizeVal", sizeText(p.value(QStringLiteral("total_size"))));
    const auto count = p.value(QStringLiteral("pieces_num"));
    const auto size = p.value(QStringLiteral("piece_size"));
    const auto have = p.value(QStringLiteral("pieces_have"));
    setValue("labelTotalPiecesVal", count.toInteger(-1) > 0 && size.toInteger(-1) > 0
        && have.toInteger(-1) >= 0 && have.toInteger() <= count.toInteger()
        ? tr("%1 x %2 (have %3)").arg(numberText(count), sizeText(size), numberText(have))
        : numberText(count));
    setValue("labelCreatedByVal", p.value(QStringLiteral("created_by")).toString(QStringLiteral("—")));
    setValue("labelAddedOnVal", dateText(p.value(QStringLiteral("addition_date")), m_dateFormat));
    setValue("labelCompletedOnVal", dateText(p.value(QStringLiteral("completion_date")), m_dateFormat));
    setValue("labelCreatedOnVal", dateText(p.value(QStringLiteral("creation_date")), m_dateFormat));
    if (const auto v = p.value(QStringLiteral("is_private")); v.isBool())
        setValue("labelPrivateVal", v.toBool() ? tr("Yes") : tr("No"));
    setValue("labelSavePathVal",
             p.value(QStringLiteral("save_path")).toString(
                 m_summary.value(QStringLiteral("save_path")).toString(QStringLiteral("—"))));
    const auto comment = p.value(QStringLiteral("comment"));
    m_commentKnown = comment.isString();
    setValue("labelCommentVal", comment.toString(QStringLiteral("—")));
    updateCommentEditor();
    const auto copies = p.value(QStringLiteral("availability"));
    setValue("labelAverageAvailabilityVal", copies.isDouble() && copies.toDouble() >= 0
        && std::isfinite(copies.toDouble()) ? QLocale().toString(copies.toDouble(), 'f', 3)
        : QStringLiteral("—"));
}

void TorrentDetailsWidget::setCommentEditingSupported(bool supported)
{
    m_commentEditingSupported = supported;
    updateCommentEditor();
}

void TorrentDetailsWidget::updateCommentEditor()
{
    m_ui->editCommentButton->setEnabled(m_commentEditingSupported
        && m_commentKnown && !m_hash.isEmpty() && m_pendingCommentHash.isEmpty()
        && m_currentTab == 0);
    m_ui->editCommentButton->setToolTip(!m_commentEditingSupported
        ? tr("Comment editing requires qBittorrent 5.2 or newer.")
        : tr("Edit the selected torrent's comment on this server."));
}

void TorrentDetailsWidget::editComment()
{
    if (!m_ui->editCommentButton->isEnabled()) return;
    const QString hash = m_hash;
    const QString original = m_lastGeneral.value(QStringLiteral("comment")).toString();
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("remoteEditCommentDialog"));
    dialog->setWindowTitle(tr("Edit torrent comment"));
    auto *layout = new QVBoxLayout(dialog);
    auto *name = new QLabel(m_summary.value(QStringLiteral("name")).toString(), dialog);
    name->setTextFormat(Qt::PlainText);
    name->setWordWrap(true);
    layout->addWidget(name);
    auto *editor = new QPlainTextEdit(dialog);
    editor->setObjectName(QStringLiteral("remoteCommentEditor"));
    editor->setPlainText(original);
    layout->addWidget(editor);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(dialog, &QDialog::accepted, this, [this, hash, original, editor]() {
        const QString edited = editor->toPlainText().trimmed(); // upstream trims too
        if (edited == original) return;
        // Polling may change metadata while a modal editor is open. Never
        // redirect an edit or overwrite a newly observed comment silently.
        if (hash != m_hash || !m_ui->editCommentButton->isEnabled()
            || m_lastGeneral.value(QStringLiteral("comment")).toString() != original)
        {
            QMessageBox::warning(this, tr("Edit torrent comment"),
                tr("The torrent selection or comment changed. Reopen the editor and try again."));
            return;
        }
        m_pendingCommentHash = hash;
        updateCommentEditor();
        emit commentEditRequested(hash, edited);
    });
    dialog->resize(500, 260);
    editor->setFocus();
    dialog->open();
}

void TorrentDetailsWidget::finishCommentEdit(const QString &hash)
{
    if (m_pendingCommentHash != hash) return;
    m_pendingCommentHash.clear();
    if (m_hash == hash)
    {
        // Do not enable editing against a stale pre-write snapshot.
        m_commentKnown = false;
        m_lastGeneral.remove(QStringLiteral("comment"));
    }
    updateCommentEditor();
}

void TorrentDetailsWidget::setTrackerTierEditingSupported(bool supported)
{
    m_trackerTierSupported = supported;
    updateTrackerTierButtons();
}

QJsonObject TorrentDetailsWidget::adjustedTrackerTiers(int delta) const
{
    QJsonObject changes;
    if (!m_trackerTierSupported || !m_trackerTiersKnown || m_hash.isEmpty()
        || !m_pendingTrackerTierHash.isEmpty() || m_currentTab != 1)
        return changes;
    for (auto *item : m_trackers->selectedItems())
    {
        // Fixed DHT/PeX/LSD and individual announce endpoints are not
        // trackers. Only selected real top-level URL rows are editable.
        if (item->parent() || isPseudoTracker(item->text(0))) continue;
        const QUrl url(item->text(0));
        if (!url.isValid() || url.host().isEmpty()) continue;
        bool ok = false;
        const int tier = item->text(1).toInt(&ok);
        if (ok && tier >= 0 && tier <= 255 && tier + delta >= 0 && tier + delta <= 255)
            changes.insert(item->text(0), tier + delta);
    }
    return changes;
}

void TorrentDetailsWidget::updateTrackerTierButtons()
{
    m_ui->trackerUpButton->setEnabled(!adjustedTrackerTiers(-1).isEmpty());
    m_ui->trackerDownButton->setEnabled(!adjustedTrackerTiers(1).isEmpty());
    const QString unavailable = tr("Changing tracker tiers requires remote Web API 2.13.0 or later.");
    m_ui->trackerUpButton->setToolTip(m_trackerTierSupported
        ? tr("Decrease selected tracker tiers") : unavailable);
    m_ui->trackerDownButton->setToolTip(m_trackerTierSupported
        ? tr("Increase selected tracker tiers") : unavailable);
}

void TorrentDetailsWidget::changeTrackerTiers(int delta)
{
    const auto changes = adjustedTrackerTiers(delta);
    if (changes.isEmpty()) return;
    m_pendingTrackerTierHash = m_hash;
    updateTrackerTierButtons();
    emit trackerTiersRequested(m_hash, changes);
}

void TorrentDetailsWidget::finishTrackerTierEdit(const QString &hash)
{
    if (m_pendingTrackerTierHash != hash) return;
    m_pendingTrackerTierHash.clear();
    m_trackerTiersKnown = false; // Re-enable only on authoritative readback.
    updateTrackerTierButtons();
}

void TorrentDetailsWidget::setSelectedTrackerReannounceSupported(bool supported)
{
    m_selectedTrackerReannounceSupported = supported;
}

QStringList TorrentDetailsWidget::trackerUrls(bool selectedOnly) const
{
    QStringList urls;
    for (int row = 0; row < m_trackers->topLevelItemCount(); ++row)
    {
        const auto *item = m_trackers->topLevelItem(row);
        if (isPseudoTracker(item->text(0)) || (selectedOnly && !item->isSelected())) continue;
        if (!urls.contains(item->text(0))) urls.append(item->text(0));
    }
    return urls;
}

bool TorrentDetailsWidget::canChangeTrackers() const
{
    return m_currentTab == 1 && !m_hash.isEmpty() && m_trackerTiersKnown
        && m_pendingTrackerTierHash.isEmpty();
}

bool TorrentDetailsWidget::canAddTrackers() const
{
    return canChangeTrackers() && ((m_peerPrivacy.isBool() && !m_peerPrivacy.toBool())
        || trackerUrls(false).isEmpty());
}

bool TorrentDetailsWidget::canReannounceTrackers() const
{
    const auto state = m_summary.value(QStringLiteral("state")).toString();
    return canChangeTrackers() && !state.isEmpty() && !state.startsWith(QLatin1String("stopped"))
        && !state.startsWith(QLatin1String("paused"));
}

void TorrentDetailsWidget::addTrackers()
{
    if (!canAddTrackers()) return;
    const QString hash = m_hash;
    bool ok = false;
    const auto text = QInputDialog::getMultiLineText(this, tr("Add trackers"),
        tr("Tracker URLs (one per line):"), {}, &ok);
    if (!ok || hash != m_hash || !canAddTrackers()) return;
    QStringList urls;
    const auto existing = trackerUrls(false);
    for (const auto &line : text.split(QLatin1Char('\n')))
    {
        const auto url = line.trimmed();
        if (url.isEmpty() || existing.contains(url) || urls.contains(url)) continue;
        urls.append(url);
    }
    if (urls.isEmpty()) return;
    if ((!m_peerPrivacy.isBool() || m_peerPrivacy.toBool()) && urls.size() != 1)
    {
        QMessageBox::warning(this, tr("Additional trackers blocked"),
            tr("Only one tracker may be restored to a private or unknown trackerless torrent."));
        return;
    }
    m_pendingTrackerTierHash = hash;
    updateTrackerTierButtons();
    emit trackerDetailActionRequested(hash, Remote::TrackerDetailAction::Add, urls, {});
}

void TorrentDetailsWidget::editTracker()
{
    const auto selected = trackerUrls(true);
    if (!canChangeTrackers() || selected.size() != 1) return;
    const QString hash = m_hash, original = selected.first();
    bool ok = false;
    const auto replacement = QInputDialog::getText(this, tr("Tracker editing"), tr("Tracker URL:"),
        QLineEdit::Normal, original, &ok).trimmed();
    if (!ok || hash != m_hash || !canChangeTrackers() || replacement == original
        || !trackerUrls(false).contains(original)) return;
    m_pendingTrackerTierHash = hash;
    updateTrackerTierButtons();
    emit trackerDetailActionRequested(hash, Remote::TrackerDetailAction::Edit, {original}, replacement);
}

void TorrentDetailsWidget::removeTrackers()
{
    const auto urls = trackerUrls(true);
    if (!canChangeTrackers() || urls.isEmpty()) return;
    m_pendingTrackerTierHash = m_hash;
    updateTrackerTierButtons();
    emit trackerDetailActionRequested(m_hash, Remote::TrackerDetailAction::Remove, urls, {});
}

void TorrentDetailsWidget::copyTrackers()
{
    const auto urls = trackerUrls(true);
    if (!urls.isEmpty()) QApplication::clipboard()->setText(urls.join(QLatin1Char('\n')));
}

void TorrentDetailsWidget::reannounceTrackers(bool selected)
{
    const auto urls = selected ? trackerUrls(true) : QStringList();
    if (!canReannounceTrackers()
        || (selected && (!m_selectedTrackerReannounceSupported || urls.isEmpty()))) return;
    m_pendingTrackerTierHash = m_hash;
    updateTrackerTierButtons();
    emit trackerDetailActionRequested(m_hash, selected ? Remote::TrackerDetailAction::ReannounceSelected
        : Remote::TrackerDetailAction::ReannounceAll, urls, {});
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
    m_trackerTiersKnown = true;
    updateTrackerTierButtons();
}

QStringList TorrentDetailsWidget::selectedPeerEndpoints() const
{
    QStringList raw;
    for (const auto &index : m_peers->selectionModel()->selectedRows())
        if (const auto *item = m_peers->item(index.row(), 0))
            raw.append(item->data(Qt::UserRole + 1).toString());
    return Remote::normalizedPeerEndpoints(raw);
}

bool TorrentDetailsWidget::canAddPeers() const
{
    const QString state = m_summary.value(QStringLiteral("state")).toString();
    return m_currentTab == 2 && !m_hash.isEmpty() && m_pendingPeerHash.isEmpty()
        && m_peerPrivacy.isBool() && !m_peerPrivacy.toBool()
        && !state.isEmpty() && !state.startsWith(QLatin1String("checking"))
        && !state.startsWith(QLatin1String("queued"));
}

void TorrentDetailsWidget::addPeers()
{
    if (!canAddPeers()) return;
    const QString hash = m_hash;
    bool ok = false;
    const QString text = QInputDialog::getMultiLineText(this, tr("Add peers"),
        tr("Enter numeric IP:port or [IPv6]:port addresses, one per line:"), {}, &ok);
    if (!ok || hash != m_hash || !canAddPeers()) return;
    const auto entries = text.split(QRegularExpression(QStringLiteral("[\\s,;|]+")), Qt::SkipEmptyParts);
    if (entries.isEmpty()) return;
    const auto endpoints = Remote::normalizedPeerEndpoints(entries);
    if (endpoints.isEmpty())
    {
        QMessageBox::warning(this, tr("Add peers"), tr("One or more peer addresses are invalid."));
        return;
    }
    m_pendingPeerHash = hash;
    emit addPeersRequested(hash, endpoints);
}

void TorrentDetailsWidget::banSelectedPeers()
{
    if (m_currentTab != 2 || m_hash.isEmpty() || !m_pendingPeerHash.isEmpty()) return;
    const QString hash = m_hash;
    const auto endpoints = selectedPeerEndpoints();
    if (endpoints.isEmpty()) return;
    QMessageBox confirm(QMessageBox::Question, tr("Ban peer permanently"),
        tr("Permanently ban the selected peer IP addresses on this server? This affects all torrents on this server."),
        QMessageBox::Yes | QMessageBox::No, this);
    confirm.setTextFormat(Qt::PlainText);
    confirm.setInformativeText(endpoints.join(QLatin1Char('\n')));
    confirm.setDefaultButton(QMessageBox::No);
    if (confirm.exec() != QMessageBox::Yes || m_hash != hash || m_currentTab != 2
        || !m_pendingPeerHash.isEmpty()) return;
    m_pendingPeerHash = hash;
    emit banPeersRequested(hash, endpoints);
}

void TorrentDetailsWidget::finishPeerAction(const QString &hash, const QString &message)
{
    if (m_pendingPeerHash != hash) return;
    m_pendingPeerHash.clear();
    if (m_hash == hash && isVisible() && !message.isEmpty())
    {
        auto *notice = new QMessageBox(QMessageBox::Information, tr("Peers"), message,
            QMessageBox::Ok, this);
        notice->setAttribute(Qt::WA_DeleteOnClose);
        notice->open();
    }
}

void TorrentDetailsWidget::copySelectedPeers()
{
    QStringList addresses;
    for (const auto &index : m_peers->selectionModel()->selectedRows())
        if (const auto *item = m_peers->item(index.row(), 0))
            addresses.append(item->data(Qt::UserRole + 1).toString());
    if (!addresses.isEmpty()) QApplication::clipboard()->setText(addresses.join(QLatin1Char('\n')));
}

void TorrentDetailsWidget::displayPeers(const QJsonObject &reply)
{
    const bool full = reply.value(QStringLiteral("full_update")).toBool()
        || !reply.contains(QStringLiteral("rid")); // direct full-snapshot consumers
    if (full) m_peerRecords = {};
    for (const auto &removed : reply.value(QStringLiteral("peers_removed")).toArray())
        m_peerRecords.remove(removed.toString());
    const QJsonObject changes = reply.value(QStringLiteral("peers")).toObject();
    for (auto it = changes.constBegin(); it != changes.constEnd(); ++it)
    {
        auto record = m_peerRecords.value(it.key()).toObject();
        const auto patch = it.value().toObject();
        for (auto field = patch.constBegin(); field != patch.constEnd(); ++field)
            record.insert(field.key(), field.value());
        m_peerRecords.insert(it.key(), record);
    }

    // Keep surviving table items and their selection; update fields in place.
    // Sorting is suspended so inserts/updates cannot change row identity.
    m_peers->setUpdatesEnabled(false);
    m_peers->setSortingEnabled(false);
    QHash<QString, int> rows;
    for (int row = m_peers->rowCount() - 1; row >= 0; --row)
    {
        const auto *item = m_peers->item(row, 0);
        if (!item || !m_peerRecords.contains(item->data(Qt::UserRole + 1).toString()))
            m_peers->removeRow(row);
    }
    for (int row = 0; row < m_peers->rowCount(); ++row)
        rows.insert(m_peers->item(row, 0)->data(Qt::UserRole + 1).toString(), row);
    const auto percentage = [](const QJsonValue &value) {
        return value.isDouble() && value.toDouble() >= 0
            ? QLocale().toString(value.toDouble() * 100, 'f', 1) + QLatin1Char('%')
            : QStringLiteral("—");
    };
    for (auto it = m_peerRecords.constBegin(); it != m_peerRecords.constEnd(); ++it)
    {
        const auto p = it.value().toObject();
        int row = rows.value(it.key(), -1);
        if (row < 0)
        {
            row = m_peers->rowCount();
            m_peers->insertRow(row);
        }
        const auto host = p.value(QStringLiteral("host_name")).toString();
        const auto ip = p.value(QStringLiteral("ip")).toString();
        const auto peerAddress = !host.isEmpty() ? host : !ip.isEmpty() ? ip : it.key();
        QString files = p.value(QStringLiteral("files")).toString();
        const QStringList values {
            peerAddress, p.value(QStringLiteral("country_code")).toString().toUpper(),
            p.value(QStringLiteral("client")).toString(),
            percentage(p.value(QStringLiteral("progress"))),
            speedText(p.value(QStringLiteral("dl_speed"))),
            speedText(p.value(QStringLiteral("up_speed"))),
            p.value(QStringLiteral("flags")).toString(),
            p.value(QStringLiteral("connection")).toString(),
            p.value(QStringLiteral("port")).isDouble()
                ? QString::number(p.value(QStringLiteral("port")).toInt()) : QStringLiteral("—"),
            sizeText(p.value(QStringLiteral("downloaded"))),
            sizeText(p.value(QStringLiteral("uploaded"))),
            percentage(p.value(QStringLiteral("relevance"))),
            percentage(p.value(QStringLiteral("contribution"))),
            QString(files).replace(QLatin1Char('\n'), QStringLiteral("; ")),
            p.value(QStringLiteral("peer_id_client")).toString()
        };
        const QHash<int, QString> numeric {
            {3, QStringLiteral("progress")}, {4, QStringLiteral("dl_speed")},
            {5, QStringLiteral("up_speed")}, {8, QStringLiteral("port")},
            {9, QStringLiteral("downloaded")}, {10, QStringLiteral("uploaded")},
            {11, QStringLiteral("relevance")}, {12, QStringLiteral("contribution")}
        };
        for (int col = 0; col < values.size(); ++col)
        {
            auto *item = m_peers->item(row, col);
            if (!item)
            {
                item = new SortablePeerItem(values.at(col));
                m_peers->setItem(row, col, item);
            }
            else if (item->text() != values.at(col)) item->setText(values.at(col));
            if (col == 0)
            {
                item->setData(Qt::UserRole + 1, it.key());
                item->setToolTip(it.key().toHtmlEscaped());
            }
            else if (col == 1) item->setToolTip(p.value(QStringLiteral("country")).toString().toHtmlEscaped());
            else if (col == 6) item->setToolTip(p.value(QStringLiteral("flags_desc")).toString().toHtmlEscaped());
            else if (col == 13) item->setToolTip(files.toHtmlEscaped());
            const auto raw = p.value(numeric.value(col));
            item->setData(Qt::UserRole, raw.isDouble() && raw.toDouble() >= 0
                ? QVariant(raw.toDouble()) : QVariant());
            if (numeric.contains(col)) item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        }
    }
    m_peers->setSortingEnabled(true);
    m_peers->setUpdatesEnabled(true);
}

QStringList TorrentDetailsWidget::selectedWebSeeds() const
{
    QStringList urls;
    for (const auto *item : m_ui->listWebSeeds->selectedItems())
        urls.append(item->text());
    return urls;
}

bool TorrentDetailsWidget::canEditWebSeeds() const
{
    return m_currentTab == 3 && !m_hash.isEmpty() && m_webSeedsKnown && m_pendingWebSeedHash.isEmpty();
}

void TorrentDetailsWidget::addWebSeed()
{
    if (!canEditWebSeeds()) return;
    const QString hash = m_hash;
    bool ok = false;
    const auto url = QInputDialog::getText(this, tr("Add web seed"), tr("Add web seed:"),
        QLineEdit::Normal, QStringLiteral("http://www."), &ok).trimmed();
    if (!ok || hash != m_hash || !canEditWebSeeds()) return;
    if (!Remote::validWebSeedUrl(url))
    {
        QMessageBox::warning(this, tr("Add web seed"), tr("Enter a valid HTTP, HTTPS or FTP URL."));
        return;
    }
    for (int row = 0; row < m_ui->listWebSeeds->count(); ++row)
        if (QUrl(m_ui->listWebSeeds->item(row)->text()) == QUrl(url))
        {
            QMessageBox::warning(this, tr("Add web seed"), tr("This web seed is already in the list."));
            return;
        }
    m_pendingWebSeedHash = hash;
    emit webSeedActionRequested(hash, Remote::WebSeedAction::Add, {url}, {});
}

void TorrentDetailsWidget::editWebSeed()
{
    const auto selected = selectedWebSeeds();
    if (!canEditWebSeeds() || selected.size() != 1) return;
    const QString hash = m_hash;
    const QString old = selected.first();
    bool ok = false;
    const QString replacement = QInputDialog::getText(this, tr("Web seed editing"),
        tr("Web seed URL:"), QLineEdit::Normal, old, &ok).trimmed();
    if (!ok || hash != m_hash || !canEditWebSeeds() || QUrl(old) == QUrl(replacement)) return;
    if (m_ui->listWebSeeds->findItems(old, Qt::MatchExactly).isEmpty()) return;
    if (!Remote::validWebSeedUrl(replacement))
    {
        QMessageBox::warning(this, tr("Web seed editing"), tr("Enter a valid HTTP, HTTPS or FTP URL."));
        return;
    }
    for (int row = 0; row < m_ui->listWebSeeds->count(); ++row)
        if (QUrl(m_ui->listWebSeeds->item(row)->text()) == QUrl(replacement))
        {
            QMessageBox::warning(this, tr("Web seed editing"), tr("This web seed is already in the list."));
            return;
        }
    m_pendingWebSeedHash = hash;
    emit webSeedActionRequested(hash, Remote::WebSeedAction::Edit, {old}, replacement);
}

void TorrentDetailsWidget::removeWebSeeds()
{
    const auto urls = selectedWebSeeds();
    if (!canEditWebSeeds() || urls.isEmpty()) return;
    m_pendingWebSeedHash = m_hash;
    emit webSeedActionRequested(m_hash, Remote::WebSeedAction::Remove, urls, {});
}

void TorrentDetailsWidget::copyWebSeeds()
{
    const auto urls = selectedWebSeeds();
    if (!urls.isEmpty()) QApplication::clipboard()->setText(urls.join(QLatin1Char('\n')));
}

void TorrentDetailsWidget::finishWebSeedAction(const QString &hash)
{
    if (m_pendingWebSeedHash != hash) return;
    m_pendingWebSeedHash.clear();
    if (m_hash == hash) m_webSeedsKnown = false;
}

void TorrentDetailsWidget::displayWebSeeds(const QJsonArray &sources)
{
    auto *list = m_ui->listWebSeeds;
    QStringList incoming;
    for (const QJsonValue &entry : sources)
    {
        const QString address = entry.isObject()
            ? entry.toObject().value(QStringLiteral("url")).toString() : entry.toString();
        if (!address.isEmpty() && !incoming.contains(address)) incoming.append(address);
    }
    // Keep existing items, selection and scroll position across polling.
    for (int row = list->count() - 1; row >= 0; --row)
        if (!incoming.contains(list->item(row)->text())) delete list->takeItem(row);
    for (const auto &url : incoming)
        if (list->findItems(url, Qt::MatchExactly).isEmpty()) list->addItem(url);
    m_webSeedsKnown = true;
}

void TorrentDetailsWidget::setContentPathMappings(
    const QList<Remote::PathMapping> &mappings)
{
    m_ui->filesList->setPathMappings(mappings);
}

void TorrentDetailsWidget::setContentInteraction(bool doubleClickRename, bool openByEnter)
{
    m_contentDoubleClickRename = doubleClickRename;
    m_ui->filesList->setOpenByEnter(openByEnter);
}

void TorrentDetailsWidget::setContentDragEnabled(bool enabled)
{
    m_ui->filesList->setContentDragEnabled(enabled);
}

QString TorrentDetailsWidget::selectedContentPath() const
{
    if (m_hash.isEmpty() || m_currentTab != 4 || !isVisible() || !m_contentKnown
        || !m_pendingContentHash.isEmpty() || m_contentRenameDialogOpen)
        return {};
    const auto selected = m_ui->filesList->selectedItems();
    if (selected.size() != 1) return {};
    QStringList parts;
    for (auto *item = selected.first(); item; item = item->parent())
    {
        const QString part = item->text(0);
        if (part.isEmpty() || part == QLatin1String(".") || part == QLatin1String("..")
            || part.contains(QLatin1Char('/')) || part.contains(QLatin1Char('\\'))
            || part.contains(QLatin1Char('\r')) || part.contains(QLatin1Char('\n'))
            || part.contains(QChar(0)))
            return {};
        parts.prepend(part);
    }
    return parts.join(QLatin1Char('/'));
}

bool TorrentDetailsWidget::contentBatchSelectionValid(const QString &hash, quint64 generation) const
{
    return !hash.isEmpty() && hash == m_hash && generation == m_contentSelectionGeneration
        && m_currentTab == 4 && isVisible();
}

QList<int> TorrentDetailsWidget::selectedContentFileIds() const
{
    if (!contentBatchSelectionValid(m_hash, m_contentSelectionGeneration)
        || !m_contentKnown || !m_pendingContentHash.isEmpty() || m_contentRenameDialogOpen)
        return {};
    QSet<int> ids;
    const auto collect = [&ids](QTreeWidgetItem *item, const auto &self) -> void {
        const QVariant id = item->data(0, Qt::UserRole);
        if (id.isValid())
        {
            bool ok = false;
            const int index = id.toInt(&ok);
            if (ok && index >= 0) ids.insert(index);
            return;
        }
        for (int i = 0; i < item->childCount(); ++i) self(item->child(i), self);
    };
    for (auto *item : m_ui->filesList->selectedItems()) collect(item, collect);
    QList<int> result = ids.values();
    std::sort(result.begin(), result.end());
    return result;
}

void TorrentDetailsWidget::batchRenameSelectedContent()
{
    const auto ids = selectedContentFileIds();
    if (ids.isEmpty()) return;
    // The originating pane opens the existing modal content-layout editor.
    // Guard the other rename actions for the duration of that editor.
    QScopedValueRollback<bool> guard(m_contentRenameDialogOpen, true);
    emit contentBatchRenameRequested(m_hash, ids, m_contentSelectionGeneration);
}

void TorrentDetailsWidget::renameSelectedContent()
{
    const QString oldPath = selectedContentPath();
    if (oldPath.isEmpty()) return;
    const QString hash = m_hash;
    const quint64 generation = m_contentSelectionGeneration;
    const bool folder = !m_ui->filesList->selectedItems().first()->data(0, Qt::UserRole).isValid();
    const int separator = oldPath.lastIndexOf(QLatin1Char('/'));
    const QString oldName = oldPath.mid(separator + 1);
    // QInputDialog can reset its minimum size when its internal layout
    // initializes. Own the layout so long filenames have room on first show.
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("contentRenameDialog"));
    dialog.setWindowTitle(folder ? tr("Rename folder") : tr("Rename file"));
    dialog.setSizeGripEnabled(true);
    auto *layout = new QVBoxLayout(&dialog);
    auto *label = new QLabel(tr("New name:"), &dialog);
    auto *editor = new QLineEdit(oldName, &dialog);
    editor->setObjectName(QStringLiteral("contentRenameName"));
    label->setBuddy(editor);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->setObjectName(QStringLiteral("contentRenameButtons"));
    layout->addWidget(label);
    layout->addWidget(editor);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    // Use logical pixels and the actual font, leaving space for decorations
    // on small displays. Longer names remain horizontally editable.
    const int available = screen() ? qMax(1, screen()->availableGeometry().width() - 64) : 1000;
    const int minimum = qMin(500, available);
    const int preferred = qBound(minimum,
        editor->fontMetrics().horizontalAdvance(oldName) + 80, available);
    dialog.setMinimumWidth(minimum);
    dialog.resize(preferred, dialog.sizeHint().height());
    editor->setFocus();
    const auto extension = oldName.lastIndexOf(QLatin1Char('.'));
    const auto selectedLength = !folder && extension > 0 ? extension : oldName.size();
    editor->setSelection(0, selectedLength);
    m_contentRenameDialogOpen = true;
    const int result = dialog.exec();
    m_contentRenameDialogOpen = false;
    if (result != QDialog::Accepted || generation != m_contentSelectionGeneration
        || hash != m_hash || selectedContentPath() != oldPath
        || folder != !m_ui->filesList->selectedItems().first()->data(0, Qt::UserRole).isValid())
        return;
    const QString name = editor->text();
    if (name == oldName) return;
    if (name.trimmed().isEmpty() || name == QLatin1String(".") || name == QLatin1String("..")
        || name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))
        || name.contains(QLatin1Char('\r')) || name.contains(QLatin1Char('\n'))
        || name.contains(QChar(0)))
    {
        QMessageBox::warning(this, tr("Invalid name"),
            tr("Enter a single file or folder name, not a path."));
        return;
    }
    m_pendingContentHash = hash;
    m_pendingContentPath = oldPath;
    emit contentRenameRequested(hash, oldPath, oldPath.left(separator + 1) + name, folder);
}

void TorrentDetailsWidget::finishContentRename(const QString &hash, const QString &oldPath)
{
    if (hash == m_pendingContentHash && oldPath == m_pendingContentPath)
    {
        m_pendingContentHash.clear();
        m_pendingContentPath.clear();
        if (hash == m_hash) m_contentKnown = false;
    }
}

void TorrentDetailsWidget::setContentFilterFormat(const QString &format)
{
    m_contentFilterFormat = (format == QLatin1String("plain") || format == QLatin1String("regex"))
        ? format : QStringLiteral("wildcards");
    applyContentFilter(true);
}

void TorrentDetailsWidget::applyContentFilter(bool adjustExpansion)
{
    auto *filter = findChild<QLineEdit *>(QStringLiteral("torrentFileFilter"));
    if (!filter) return;
    auto *tree = m_ui->filesList;
    const QString needle = filter->text();
    QString pattern = needle;
    if (m_contentFilterFormat == QLatin1String("plain"))
        pattern = QRegularExpression::escape(needle);
    else if (m_contentFilterFormat == QLatin1String("wildcards"))
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 1)
        pattern = QRegularExpression::wildcardToRegularExpression(needle,
            QRegularExpression::UnanchoredWildcardConversion | QRegularExpression::NonPathWildcardConversion);
#else
        pattern = QRegularExpression::wildcardToRegularExpression(needle,
            QRegularExpression::UnanchoredWildcardConversion);
#endif
    }
    const QRegularExpression re(pattern, QRegularExpression::CaseInsensitiveOption);
    filter->setToolTip(re.isValid() ? QString() : re.errorString());
    const auto visit = [&re, &needle](QTreeWidgetItem *item, const auto &self) -> bool {
        bool match = needle.isEmpty() || (re.isValid() && re.match(item->text(0)).hasMatch());
        for (int i = 0; i < item->childCount(); ++i)
            match = self(item->child(i), self) || match;
        item->setHidden(!match);
        if (!match) item->setSelected(false);
        return match;
    };
    for (int i = 0; i < tree->topLevelItemCount(); ++i) visit(tree->topLevelItem(i), visit);
    if (adjustExpansion)
    {
        if (needle.isEmpty())
        {
            tree->collapseAll();
            if (tree->topLevelItemCount()) tree->expandItem(tree->topLevelItem(0));
        }
        else tree->expandAll();
    }
}

void TorrentDetailsWidget::displayFiles(const QJsonArray &files)
{
    auto *tree = m_ui->filesList;
    QSignalBlocker blocker(tree);
    // Rechecks and ordinary transfers change file values, not the tree.
    // Reuse existing items unless the server's file IDs or paths change.
    // setSelection() clears the tree when the selected hash changes.
    const bool firstDisplay = tree->topLevelItemCount() == 0;
    const auto itemPath = [](const QTreeWidgetItem *item) {
        QStringList components;
        for (auto *node = item; node; node = node->parent())
            components.prepend(node->text(0));
        return components.join(QLatin1Char('/'));
    };
    const auto itemKey = [&itemPath](const QTreeWidgetItem *item) -> QString {
        const QVariant id = item->data(0, Qt::UserRole);
        return id.isValid() ? QStringLiteral("file:") + QString::number(id.toInt())
            : QStringLiteral("folder:") + itemPath(item);
    };
    QSet<QString> expandedFolders;
    QSet<QString> selectedItems;
    QString currentItem;
    const int currentColumn = tree->currentColumn();
    const int verticalPosition = tree->verticalScrollBar()->value();
    const int horizontalPosition = tree->horizontalScrollBar()->value();
    // Keep the actual visible row, independently of the selected/current
    // item (which may be far outside the viewport during a long recheck).
    QString anchorKey;
    int anchorOffset = 0;
    if (const auto *anchor = tree->itemAt(0, 0))
    {
        anchorKey = itemKey(anchor);
        anchorOffset = tree->visualItemRect(anchor).top();
    }
    QHash<int, QTreeWidgetItem *> existingFiles;
    if (const auto *current = tree->currentItem())
        currentItem = itemKey(current);
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
    {
        auto *item = *it;
        if (item->childCount() && item->isExpanded())
            expandedFolders.insert(itemKey(item));
        if (item->isSelected())
            selectedItems.insert(itemKey(item));
        const QVariant id = item->data(0, Qt::UserRole);
        if (id.isValid())
            existingFiles.insert(id.toInt(), item);
    }
    QSet<int> incomingIds;
    bool sameStructure = !firstDisplay;
    for (const QJsonValue &entry : files)
    {
        const auto file = entry.toObject();
        const int id = file.value(QStringLiteral("index")).toInt(-1);
        const auto parts = file.value(QStringLiteral("name")).toString()
            .split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (id < 0 || parts.isEmpty())
            continue;
        const auto *item = existingFiles.value(id);
        if (incomingIds.contains(id) || !item
            || itemPath(item) != parts.join(QLatin1Char('/')))
            sameStructure = false;
        incomingIds.insert(id);
    }
    sameStructure = sameStructure && incomingIds.size() == existingFiles.size();
    const bool wasSorting = tree->isSortingEnabled();
    tree->setSortingEnabled(false);
    if (!sameStructure)
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

        QTreeWidgetItem *item = sameStructure ? existingFiles.value(id) : nullptr;
        if (!item)
        {
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

            item = parent ? new SortableDetailTreeItem(parent)
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
        }

        const qint64 size = file.value(QStringLiteral("size")).toInteger(0);
        const double fraction = qBound(0.0,
             file.value(QStringLiteral("progress")).toDouble(0.0), 1.0);
        const int priority = file.value(QStringLiteral("priority")).toInt(1);
        item->setText(1, bytesText(size));
        item->setData(1, Qt::UserRole, size);
        item->setText(2, ProgressDisplay::percent(fraction, 0));
        item->setData(2, Qt::UserRole, fraction);
        item->setText(3, priorityText(priority));
        item->setData(3, Qt::UserRole, priority);
        const double remaining = std::max(0.0, size * (1.0 - fraction));
        item->setText(4, bytesText(remaining));
        item->setData(4, Qt::UserRole, remaining);
        item->setText(5, availabilityText(file.value(QStringLiteral("availability"))));
        item->setData(5, Qt::UserRole, QVariant());
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
    tree->setSortingEnabled(wasSorting);
    if (firstDisplay)
        tree->expandToDepth(0);
    // Reapply exactly the same matcher on live updates without disturbing
    // the user's current expansion or scroll position.
    applyContentFilter(false);

    if (!firstDisplay)
    {
        QTreeWidgetItem *restoredCurrent = nullptr;
        QTreeWidgetItem *restoredAnchor = nullptr;
        for (QTreeWidgetItemIterator it(tree); *it; ++it)
        {
            auto *item = *it;
            const QString key = itemKey(item);
            if (!sameStructure && selectedItems.contains(key))
                item->setSelected(true);
            if (key == currentItem)
                restoredCurrent = item;
            if (key == anchorKey)
                restoredAnchor = item;
        }
        if (!sameStructure && restoredCurrent)
            tree->setCurrentItem(restoredCurrent, currentColumn, QItemSelectionModel::NoUpdate);
        // Restoring the current index can scroll/expand its ancestors.
        // Reapply the user's explicit expansion choices last.
        if (!sameStructure)
            for (auto *folder : std::as_const(folders))
                folder->setExpanded(expandedFolders.contains(itemKey(folder)));
        // Complete pending sorting/layout before setting scroll positions.
        // If live sorting moves the visible row, keep that row anchored
        // where possible, rather than jumping to the selected file.
        tree->doItemsLayout();
        tree->verticalScrollBar()->setValue(verticalPosition);
        if (restoredAnchor && tree->visualItemRect(restoredAnchor).isValid()
            && tree->visualItemRect(restoredAnchor).top() != anchorOffset)
        {
            tree->scrollToItem(restoredAnchor, QAbstractItemView::PositionAtTop);
            if (tree->verticalScrollMode() == QAbstractItemView::ScrollPerPixel)
                tree->verticalScrollBar()->setValue(tree->verticalScrollBar()->value()
                    + tree->visualItemRect(restoredAnchor).top() - anchorOffset);
        }
        tree->horizontalScrollBar()->setValue(horizontalPosition);
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
        node->setText(2, ProgressDisplay::percent(fraction, 0));
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

QList<QList<int>> TorrentDetailsWidget::selectedContentPriorityGroups() const
{
    if (selectedContentFileIds().isEmpty()) return {};
    auto *tree = m_ui->filesList;
    QList<QList<int>> rows;
    QSet<int> seen;
    const auto gather = [&seen](QTreeWidgetItem *item, QList<int> &ids, const auto &self) -> void {
        const QVariant id = item->data(0, Qt::UserRole);
        if (id.isValid())
        {
            const int index = id.toInt();
            if (index >= 0 && !seen.contains(index))
            {
                seen.insert(index);
                ids.append(index);
            }
            return;
        }
        for (int i = 0; i < item->childCount(); ++i) self(item->child(i), ids, self);
    };
    // Walk the sorted tree, not the order in which the user selected rows.
    // A selected folder represents one row; its descendants inherit its
    // priority, even if also selected. No file receives conflicting writes.
    const auto visit = [&rows, &gather](QTreeWidgetItem *item, const auto &self) -> void {
        if (item->isSelected())
        {
            QList<int> ids;
            gather(item, ids, gather);
            if (!ids.isEmpty()) rows.append(ids);
            return;
        }
        for (int i = 0; i < item->childCount(); ++i) self(item->child(i), self);
    };
    for (int i = 0; i < tree->topLevelItemCount(); ++i) visit(tree->topLevelItem(i), visit);
    if (rows.isEmpty()) return {};
    const qsizetype groupSize = std::max<qsizetype>(rows.size() / 3, 1);
    QList<QList<int>> groups(3);
    for (qsizetype i = 0; i < rows.size(); ++i)
        groups[std::min<qsizetype>(i / groupSize, 2)].append(rows.at(i));
    return groups;
}

void TorrentDetailsWidget::chooseFilePrioritiesByOrder(const QList<QList<int>> &groups)
{
    if (groups.size() != 3 || groups != selectedContentPriorityGroups()) return;
    const QString hash = m_hash;
    // Use the existing serialized HTTP queue and authoritative readback.
    // At most three requests, irrespective of the number of selected files.
    const int priorities[] = {7, 6, 1};
    for (int i = 0; i < 3; ++i)
        if (!groups.at(i).isEmpty())
            emit filePriorityRequested(hash, groups.at(i), priorities[i]);
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
    if (hash != m_hash) return;
    if (tab == 8)
    {
        if (m_currentTab == 1 || m_currentTab == 2) m_peerPrivacy = data.toObject().value(QStringLiteral("private"));
        return;
    }
    if (tab == 6 || tab == 7)
    {
        if (m_currentTab != 0 || !isVisible()) return;
        if (tab == 6) m_progressBar->setPieces(data.toArray());
        else if (wantsPieceAvailability())
        {
            const bool available = m_availabilityBar->setPieces(data.toArray());
            m_availabilityBar->setVisible(available);
            m_ui->labelPiecesAvailability->setVisible(available);
            m_ui->labelAverageAvailabilityVal->setVisible(available);
        }
        return;
    }
    if (tab != m_currentTab) return;
    m_error->hide();
    switch (tab)
    {
    case 0: displayGeneral(data.toObject()); break;
    case 1: displayTrackers(data.toArray()); break;
    case 2: displayPeers(data.toObject()); break;
    case 3: displayWebSeeds(data.toArray()); break;
    case 4: m_contentKnown = data.isArray(); displayFiles(data.toArray()); break;
    default: break;
    }
}

void TorrentDetailsWidget::displayError(const QString &hash, int tab, const QString &reason)
{
    if (hash != m_hash) return;
    if (tab == 8)
    {
        m_peerPrivacy = QJsonValue();
        return;
    }
    if (tab == 6 || tab == 7)
    {
        if (m_currentTab != 0 || !isVisible()) return;
        if (tab == 6) m_progressBar->clearPieces();
        else
        {
            m_availabilityBar->clearPieces();
            m_availabilityBar->hide();
            m_ui->labelPiecesAvailability->hide();
            m_ui->labelAverageAvailabilityVal->hide();
        }
        return;
    }
    if (tab != m_currentTab) return;
    if (tab == 0)
    {
        m_commentKnown = false;
        updateCommentEditor();
    }
    if (tab == 1)
    {
        m_trackerTiersKnown = false;
        updateTrackerTierButtons();
    }
    if (tab == 3) m_webSeedsKnown = false;
    if (tab == 4) m_contentKnown = false;
    if (tab == 2)
    {
        m_peerRecords = {};
        m_peers->setRowCount(0);
    }
    m_error->setText(tr("Could not load torrent details: %1").arg(reason));
    m_error->show();
}

void TorrentDetailsWidget::appendRates(double downloadBytesPerSec,
                                       double uploadBytesPerSec)
{
    m_speedGraph->append(downloadBytesPerSec, uploadBytesPerSec);
}
