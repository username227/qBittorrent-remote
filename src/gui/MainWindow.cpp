// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/MainWindow.h"
#include "gui/ServerPane.h"
#include "gui/TorrentModel.h"
#include "gui/FilterSidebar.h"
#include "gui/ConnectionDialog.h"
#include "gui/RemotePreferencesDialog.h"
#include "gui/RemoteCategoryDialog.h"
#include "gui/RemoteSpeedLimitsDialog.h"
#include "gui/RemoteAddTorrentDialog.h"
#include "gui/RemoteTrackerEntriesDialog.h"
#include "gui/RemoteTorrentContentLayoutDialog.h"
#include "gui/RemoteTorrentOptionsDialog.h"
#include "gui/RemoteFileSystemPathComboEdit.h"
#include "api/RemoteSession.h"
#include "api/TorrentMetadataReader.h"
#include "api/PathMappingStore.h"
#include "gui/upstream/hidabletabwidget.h"

#include "ui_mainwindow.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QCloseEvent>
#include <QGuiApplication>
#include <QScreen>
#include <QApplication>
#include <QSystemTrayIcon>
#include <QWindowStateChangeEvent>
#include <QFont>
#include <QFontInfo>
#include <QFontMetrics>
#include <QLocale>
#include <QGuiApplication>
#include <QScreen>
#include <QKeySequence>
#include <QMenu>
#include <QCheckBox>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QHash>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QSortFilterProxyModel>
#include <QSet>
#include <QTableView>
#include <QAction>
#include <QComboBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QPointer>
#include <QRegularExpression>
#include <QSslCertificate>
#include <QUrl>
#include <QUrlQuery>
#include <QSizePolicy>
#include <QStatusBar>
#include <QStyle>
#include <QToolBar>
#include <QWidget>
#include <QSignalBlocker>
#include <QTimer>
#include <QTabWidget>
#include <QTabBar>
#include <algorithm>

namespace
{
struct DroppedTorrentSources
{
    QStringList files;
    QStringList links;
    bool valid() const { return !files.isEmpty() || !links.isEmpty(); }
};

DroppedTorrentSources droppedTorrentSources(const QMimeData *mime)
{
    DroppedTorrentSources sources;
    if (!mime) return sources;
    QStringList inputs;
    if (mime->hasUrls())
    {
        for (const QUrl &url : mime->urls())
            inputs.append(url.toString(QUrl::FullyEncoded));
    }
    else if (mime->hasText())
        inputs = mime->text().split(QRegularExpression(QStringLiteral("[\\r\\n]+")),
                                   Qt::SkipEmptyParts);
    if (inputs.isEmpty() || inputs.size() > 256) return {};
    QSet<QString> seen;
    for (const QString &input : inputs)
    {
        const QUrl url(input.trimmed(), QUrl::StrictMode);
        if (!url.isValid()) return {};
        if (url.isLocalFile())
        {
            const QString local = url.toLocalFile();
            if (!local.endsWith(QLatin1String(".torrent"), Qt::CaseInsensitive))
                return {};
            if (!seen.contains(local))
            {
                seen.insert(local);
                sources.files.append(local);
            }
        }
        else if (url.scheme().compare(QLatin1String("magnet"), Qt::CaseInsensitive) == 0)
        {
            const QString link = url.toString(QUrl::FullyEncoded);
            if (!seen.contains(link))
            {
                seen.insert(link);
                sources.links.append(link);
            }
        }
        else return {}; // Reject unrelated dropped web links and file types.
    }
    if (!sources.files.isEmpty() && !sources.links.isEmpty()) return {};
    return sources;
}

struct TorrentIdentity
{
    QStringList hashes;
    QStringList trackers;
    bool isPrivate = false;
};

QString base32Btih(const QString &raw)
{
    // Legacy magnet links may encode the 20-byte BTIH as 32 Base32 chars.
    // Decode only this exact bounded shape; malformed values cannot match.
    if (raw.size() != 32) return {};
    QByteArray decoded;
    quint32 bits = 0;
    int bitCount = 0;
    for (QChar c : raw.toUpper())
    {
        const ushort u = c.unicode();
        int value = -1;
        if (u >= 'A' && u <= 'Z') value = u - 'A';
        else if (u >= '2' && u <= '7') value = u - '2' + 26;
        if (value < 0) return {};
        bits = (bits << 5) | static_cast<quint32>(value);
        bitCount += 5;
        if (bitCount >= 8)
        {
            bitCount -= 8;
            decoded.append(static_cast<char>((bits >> bitCount) & 0xff));
        }
    }
    return decoded.size() == 20 ? QString::fromLatin1(decoded.toHex()) : QString();
}

TorrentIdentity magnetIdentity(const QString &links)
{
    TorrentIdentity result;
    const QStringList sources = links.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (sources.size() != 1)
        return result; // Avoid claiming a single duplicate for multiple links.
    const QUrl url(sources.first().trimmed(), QUrl::StrictMode);
    if (!url.isValid() || url.scheme().compare(QLatin1String("magnet"),
                                               Qt::CaseInsensitive) != 0)
        return result;
    const QUrlQuery query(url);
    static const QRegularExpression hex40(QStringLiteral("^[0-9a-fA-F]{40}$"));
    static const QRegularExpression hex64(QStringLiteral("^[0-9a-fA-F]{64}$"));
    for (const auto &part : query.queryItems(QUrl::FullyDecoded))
    {
        if (part.first == QLatin1String("xt"))
        {
            const QString xt = part.second;
            if (xt.startsWith(QLatin1String("urn:btih:"), Qt::CaseInsensitive))
            {
                const QString hash = xt.mid(9);
                if (hex40.match(hash).hasMatch())
                    result.hashes.append(hash.toLower());
                else
                {
                    const QString decoded = base32Btih(hash);
                    if (!decoded.isEmpty()) result.hashes.append(decoded);
                }
            }
            else if (xt.startsWith(QLatin1String("urn:btmh:1220"), Qt::CaseInsensitive))
            {
                const QString hash = xt.mid(13);
                if (hex64.match(hash).hasMatch())
                    result.hashes.append(hash.toLower());
            }
        }
        else if (part.first == QLatin1String("tr") && result.trackers.size() < 256)
        {
            const QUrl tracker(part.second, QUrl::StrictMode);
            const QString scheme = tracker.scheme().toLower();
            if (tracker.isValid() && !tracker.host().isEmpty()
                && (scheme == QLatin1String("http") || scheme == QLatin1String("https")
                    || scheme == QLatin1String("udp"))
                && !part.second.contains(QLatin1Char('\n'))
                && !part.second.contains(QLatin1Char('\r'))
                && !result.trackers.contains(part.second))
                result.trackers.append(part.second);
        }
    }
    return result;
}

QString existingHash(const TorrentModel &model, const QStringList &candidateHashes)
{
    if (candidateHashes.isEmpty()) return {};
    for (auto it = model.torrents().cbegin(); it != model.torrents().cend(); ++it)
    {
        const QJsonObject record = it.value();
        for (const QString &hash : {it.key(),
                 record.value(QStringLiteral("infohash_v1")).toString(),
                 record.value(QStringLiteral("infohash_v2")).toString()})
            if (!hash.isEmpty() && candidateHashes.contains(hash, Qt::CaseInsensitive))
                return it.key(); // Server's canonical, authenticated torrent ID.
    }
    return {};
}

QIcon upstreamIcon(const char *name)
{
    return QIcon(QStringLiteral(":/icons/") + QString::fromLatin1(name) + QStringLiteral(".svg"));
}

// Capture the original system Qt font once, before an application-level
// preference can overwrite QApplication::font(). Both the initial floor
// and Reset always refer to this original torrent-list baseline.
const QFont &originalSystemFont()
{
    static const QFont font = QApplication::font();
    return font;
}

int defaultFontPointSize()
{
    const QFontInfo info(originalSystemFont());
    if (info.pointSize() > 0)
        return info.pointSize();

    // Qt styles may specify fonts in physical pixels rather than points.
    // Round *up* to prevent a new point-size preference from reducing
    // the perceived baseline on any desktop DPI setting.
    const QScreen *screen = QGuiApplication::primaryScreen();
    const qreal dpi = screen ? screen->logicalDotsPerInchY() : 96.0;
    return qMax(1, qCeil(info.pixelSize() * 72.0 / dpi));
}

constexpr int maxFontSize = 28;
constexpr auto fontSettingsKey = "Appearance/FontPointSize";

} // namespace

struct MainWindow::SessionContext
{
    Remote::ServerProfile profile;
    Remote::RemoteSession *session = nullptr;
    ServerPane *pane = nullptr;
    QString version;
    QString apiVersion;
    bool connected = false;
    bool altSpeedModeKnown = false;
    bool altSpeedMode = false;
};

MainWindow::MainWindow(QWidget *parent, QSettings *settings,
                       Remote::CredentialVault *vault)
    : QMainWindow(parent)
    , m_ui(std::make_unique<Ui::MainWindow>())
    , m_settings(settings ? settings : &m_defaultSettings)
    , m_vault(vault ? vault : &m_defaultVault)
{
    // Required when SVG resources live in a linked static library.
    Q_INIT_RESOURCE(qbittorrent_icons);

    // Preserve the existing torrent-list font as the smallest permitted
    // point size. All widgets share the Qt application font, never a
    // per-table stylesheet or a smaller imitation of upstream's text.
    m_baseFontPointSize = defaultFontPointSize();
    const int configured = m_settings->value(
        QLatin1String(fontSettingsKey), m_baseFontPointSize).toInt();
    applyFontPointSize(configured);

    // Actual, unchanged upstream Qt Designer layout: menus, actions,
    // original toolbar and status bar, not a visual imitation.
    m_ui->setupUi(this);
    setWindowTitle(tr("qBittorrent Remote"));
    const QIcon applicationIcon = upstreamIcon("qbittorrent-tray");
    QApplication::setWindowIcon(applicationIcon);
    setWindowIcon(applicationIcon);
    resize(1280, 800);

    setupMainArea();
    setupActions();
    setupTray();
    updateFontActionStates();

    // Retain server identity/version as the default left-hand status text.
    // Qt briefly replaces status messages with hovered action tips or
    // time-limited connection errors; restore the default when they expire.
    connect(m_ui->statusBar, &QStatusBar::messageChanged, this,
            [this](const QString &message) {
        if (!message.isEmpty())
            return;
        QTimer::singleShot(0, this, [this]() {
            if (m_ui->statusBar->currentMessage().isEmpty())
                updateActiveUi();
        });
    });
    m_ui->statusBar->showMessage(tr("Not connected to a server"));
    // Permanently visible widgets are inserted left to right. The displayed
    // torrent count stays left of the connectivity and alternative-speed
    // indicators, followed by the existing transfer rates.
    m_torrentCountLabel = new QLabel(tr("Torrents: —"), m_ui->statusBar);
    m_torrentCountLabel->setObjectName(QStringLiteral("visibleTorrentCount"));
    m_torrentCountLabel->setToolTip(tr("Torrents visible after all filters / total on the selected server"));
    m_ui->statusBar->addPermanentWidget(m_torrentCountLabel);
    const auto separator = [this](const char *name) {
        auto *label = new QLabel(QStringLiteral("|"), m_ui->statusBar);
        label->setObjectName(QString::fromLatin1(name));
        m_ui->statusBar->addPermanentWidget(label);
        return label;
    };
    separator("torrentCountSeparator");
    // The globe/flame reports the selected daemon's BitTorrent connectivity,
    // not whether the desktop can reach the authenticated Web API.
    m_connectionStatusLabel = new QLabel(m_ui->statusBar);
    m_connectionStatusLabel->setObjectName(QStringLiteral("remoteConnectionStatus"));
    m_connectionStatusLabel->setAccessibleName(tr("BitTorrent connection status"));
    m_connectionStatusLabel->setAlignment(Qt::AlignCenter);
    m_ui->statusBar->addPermanentWidget(m_connectionStatusLabel);
    separator("connectionStatusSeparator");

    m_alternativeSpeedButton = new QPushButton(m_ui->statusBar);
    m_alternativeSpeedButton->setObjectName(QStringLiteral("remoteAlternativeSpeedLimits"));
    m_alternativeSpeedButton->setAccessibleName(tr("Alternative speed limits"));
    m_alternativeSpeedButton->setFlat(true);
    m_alternativeSpeedButton->setFocusPolicy(Qt::NoFocus);
    m_alternativeSpeedButton->setCursor(Qt::PointingHandCursor);
    m_alternativeSpeedButton->setCheckable(true);
    m_ui->statusBar->addPermanentWidget(m_alternativeSpeedButton);
    connect(m_alternativeSpeedButton, &QPushButton::clicked, this, [this]() {
        // Both entry points use the same authenticated selected-server action.
        m_ui->actionUseAlternativeSpeedLimits->trigger();
        updateTrayActionState();
    });
    separator("alternativeSpeedSeparator");
    m_downloadRateLabel = new QLabel(tr("D: —"), m_ui->statusBar);
    m_downloadRateLabel->setObjectName(QStringLiteral("remoteDownloadRate"));
    m_ui->statusBar->addPermanentWidget(m_downloadRateLabel);
    separator("transferRateSeparator");
    m_uploadRateLabel = new QLabel(tr("U: —"), m_ui->statusBar);
    m_uploadRateLabel->setObjectName(QStringLiteral("remoteUploadRate"));
    m_ui->statusBar->addPermanentWidget(m_uploadRateLabel);
    m_freeSpaceSeparator = separator("freeSpaceSeparator");
    m_serverFreeSpaceLabel = new QLabel(tr("Free: —"), m_ui->statusBar);
    m_serverFreeSpaceLabel->setObjectName(QStringLiteral("remoteServerFreeSpace"));
    m_serverFreeSpaceLabel->setToolTip(tr(
        "Free space on the selected server's default save drive, not this computer."));
    m_ui->statusBar->addPermanentWidget(m_serverFreeSpaceLabel);
    for (auto *label : {m_downloadRateLabel, m_uploadRateLabel, m_serverFreeSpaceLabel})
        label->installEventFilter(this);
    updateStatusBarFieldWidths();
    updateTrayActionState();
    restoreWindowLayout();
    applyLocalBehaviorPreferences();

    // Opt-in startup connections run after the event loop begins. Each
    // profile independently uses a blank-password login or secure vault
    // retrieval; failures never prevent other selected profiles from opening.
    QTimer::singleShot(0, this, [this]() {
        m_restoringStartup = true;
        Remote::ProfileStore store(*m_settings);
        for (const auto &profile : store.profiles())
            if (profile.autoConnect)
                connectToProfile(profile, QString());
        m_restoringStartup = false;
        for (const auto &connection : m_connections)
            if (connection->profile.id == m_preferredServerId)
            {
                // Switching tabs is allowed. Starting a connection just
                // because it was selected last session is NOT allowed.
                m_views->setCurrentWidget(connection->pane);
                break;
            }
        updateActiveUi();
    });
}

MainWindow::~MainWindow() = default;

void MainWindow::showEmptyPane()
{
    if (!m_emptyPane && m_views && m_connections.isEmpty())
    {
        m_emptyPane = new ServerPane(m_views);
        m_emptyPane->setContentDragEnabled(m_settings->value(
            QStringLiteral("Preferences/Behavior/TorrentContentDragEnabled"), true).toBool());
        m_views->addTab(m_emptyPane, upstreamIcon("folder-remote"), tr("Transfers"));
        m_views->setCurrentWidget(m_emptyPane);
        enableTorrentDrops(m_emptyPane);
        enableTorrentDrops(m_emptyPane->table()->viewport());
    }
}

void MainWindow::setupMainArea()
{
    m_views = new HidableTabWidget(m_ui->centralWidget);
    m_views->setObjectName(QStringLiteral("serverTabs"));
    m_ui->centralWidgetLayout->addWidget(m_views);
    enableTorrentDrops(m_ui->centralWidget);
    enableTorrentDrops(m_views);
    showEmptyPane();
    if (m_emptyPane)
        m_emptyPane->restoreLayout(*m_settings, QStringLiteral("empty-pane"));

    connect(m_views, &QTabWidget::currentChanged, this,
            [this](int) { updateActiveUi(); });

    auto *tabBar = m_views->tabBar();
    tabBar->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tabBar, &QTabBar::customContextMenuRequested, this,
        [this, tabBar](const QPoint &position) {
            const int index = tabBar->tabAt(position);
            if (index < 0)
                return;
            const auto *pane = m_views->widget(index);
            const auto it = std::find_if(m_connections.cbegin(), m_connections.cend(),
                [pane](const auto &connection) { return connection->pane == pane; });
            if (it == m_connections.cend())
                return;
            // Capture the actual clicked connection, not its tab index or
            // the active server. Sync/disconnect events can run in the menu.
            const auto connection = *it;
            QMenu menu(this);
            menu.setObjectName(QStringLiteral("serverTabContextMenu"));
            QString serverName = connection->profile.name;
            serverName.replace(QLatin1Char('&'), QStringLiteral("&&"));
            auto *preferences = menu.addAction(upstreamIcon("configure"),
                tr("%1 Preferences...").arg(serverName));
            preferences->setObjectName(QStringLiteral("serverTabPreferences"));
            preferences->setEnabled(connection->connected);
            connect(preferences, &QAction::triggered, this, [this, connection]() {
                if (containsConnection(connection) && connection->connected)
                    openPreferences(connection);
            });
            menu.addSeparator();
            auto *disconnect = menu.addAction(upstreamIcon("disconnected"), tr("Disconnect"));
            disconnect->setObjectName(QStringLiteral("serverTabDisconnect"));
            connect(disconnect, &QAction::triggered, this, [this, connection]() {
                if (containsConnection(connection))
                    removeConnection(connection);
            });
            menu.exec(tabBar->mapToGlobal(position));
        });

    // Keep the original, shared native qBittorrent toolbar search.
    // Each server pane keeps its own search string and filter column.
    auto *filterPanel = new QWidget(m_ui->toolBar);
    auto *filterLayout = new QHBoxLayout(filterPanel);
    filterLayout->setContentsMargins(0, 0, 0, 0);
    filterLayout->addStretch();

    m_searchEdit = new QLineEdit(filterPanel);
    m_searchEdit->setObjectName(QStringLiteral("torrentSearch"));
    m_searchEdit->setPlaceholderText(tr("Filter torrents..."));
    m_searchEdit->setFixedWidth(200);
    m_searchEdit->setEnabled(false);
    filterLayout->addWidget(m_searchEdit);
    filterLayout->addWidget(new QLabel(tr("Filter by:"), filterPanel));

    m_searchBy = new QComboBox(filterPanel);
    m_searchBy->addItems({tr("Name"), tr("Save Path"), tr("Info Hash v1")});
    m_searchBy->setEnabled(false);
    filterLayout->addWidget(m_searchBy);

    connect(m_searchEdit, &QLineEdit::textChanged, this,
            [this](const QString &text) {
                if (auto *pane = activePane())
                    pane->setSearchTerm(text);
            });
    connect(m_searchBy, &QComboBox::currentIndexChanged, this,
            [this](int index) {
                if (auto *pane = activePane())
                    pane->setSearchColumn(index);
            });

    filterPanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_ui->toolBar->insertWidget(m_ui->actionLock, filterPanel);
}

void MainWindow::setupActions()
{
    const struct
    {
        QAction *action;
        const char *iconName;
    } iconAssignments[] = {
        {m_ui->actionOpen, "list-add"},
        {m_ui->actionDownloadFromURL, "insert-link"},
        {m_ui->actionDelete, "list-remove"},
        {m_ui->actionStart, "torrent-start"},
        {m_ui->actionStop, "torrent-stop"},
        {m_ui->actionOpenDestinationFolder, "directory"},
        {m_ui->actionTopQueuePos, "go-top"},
        {m_ui->actionIncreaseQueuePos, "go-up"},
        {m_ui->actionDecreaseQueuePos, "go-down"},
        {m_ui->actionBottomQueuePos, "go-bottom"},
        {m_ui->actionCreateTorrent, "torrent-creator"},
        {m_ui->actionOptions, "configure"},
        {m_ui->actionSetGlobalSpeedLimits, "speedometer"},
        {m_ui->actionExit, "application-exit"},
        {m_ui->actionAbout, "help-about"}
    };
    for (const auto &assignment : iconAssignments)
        assignment.action->setIcon(upstreamIcon(assignment.iconName));

    // Match native qBittorrent's toolbar sizing: let the active Qt style
    // choose icon dimensions instead of imposing a fixed smaller size.

    // Preserve the original actions but never offer a misleading, working
    // control before the remote backend and its safety checks exist.
    for (QAction *action : {
        m_ui->actionOpen, m_ui->actionDownloadFromURL,
        m_ui->actionDelete, m_ui->actionStart, m_ui->actionStop,
        m_ui->actionOpenDestinationFolder, m_ui->actionTopQueuePos,
        m_ui->actionIncreaseQueuePos, m_ui->actionDecreaseQueuePos,
        m_ui->actionBottomQueuePos, m_ui->actionCreateTorrent,
        m_ui->actionOptions, m_ui->actionUseAlternativeSpeedLimits,
        m_ui->actionSetGlobalSpeedLimits
    })
        action->setEnabled(false);

    m_ui->actionLock->setVisible(false);
    // The original native tray actions are scoped to the currently selected
    // connected server. They are not local torrent-engine actions.
    connect(m_ui->actionUseAlternativeSpeedLimits, &QAction::triggered, this,
            [this](bool enabled) {
                const auto server = activeConnection();
                if (!server || !server->connected || !server->altSpeedModeKnown)
                {
                    updateTrayActionState();
                    return;
                }
                server->session->setAlternativeSpeedLimitsEnabled(enabled);
                // A command acceptance is not authoritative state. Keep
                // both entry points on the last synchronized value.
                updateTrayActionState();
            });
    connect(m_ui->actionSetGlobalSpeedLimits, &QAction::triggered, this, [this]() {
        const auto server = activeConnection();
        if (!server || !server->connected)
            return;
        RemoteSpeedLimitsDialog dialog(server->session, this);
        trackDialogGeometry(dialog, QStringLiteral("GlobalSpeedLimits"));
        dialog.exec();
    });

    // The original toolbar action and the native context menu share the same
    // selected-server behavior. Never open raw remote POSIX paths locally.
    connect(m_ui->actionOpenDestinationFolder, &QAction::triggered,
            this, [this]() { openSelectedTorrentDestinations(); });

    // Keep the relevant original menus; remove local-engine-only and
    // otherwise unimplemented actions until the remote features exist.
    m_ui->menuPlugins->menuAction()->setVisible(false);

    m_ui->menuFile->clear();
    m_connectAction = m_ui->menuFile->addAction(tr("Connect to Server..."));
    m_connectAction->setObjectName(QStringLiteral("connectToServer"));
    m_disconnectAction = m_ui->menuFile->addAction(tr("Disconnect"));
    m_disconnectAction->setObjectName(QStringLiteral("disconnectServer"));
    m_disconnectAction->setEnabled(false);
    m_ui->menuFile->addSeparator();
    m_ui->menuFile->addAction(m_ui->actionOpen);
    m_ui->menuFile->addAction(m_ui->actionDownloadFromURL);
    m_ui->menuFile->addSeparator();
    m_ui->menuFile->addAction(m_ui->actionExit);

    m_ui->menuEdit->clear();
    m_ui->menuEdit->addActions({
        m_ui->actionStart, m_ui->actionStop, m_ui->actionDelete,
        m_ui->actionTopQueuePos, m_ui->actionIncreaseQueuePos,
        m_ui->actionDecreaseQueuePos, m_ui->actionBottomQueuePos
    });

    m_ui->menuView->clear();
    m_ui->menuView->addAction(m_ui->actionTopToolBar);
    m_ui->menuView->addAction(m_ui->actionShowStatusbar);
    m_ui->menuView->addAction(m_ui->actionShowFiltersSidebar);
    m_ui->menuView->addSeparator();

    // Local usability enhancement, absent from upstream qBittorrent.
    // Native Qt fonts retain the active desktop theme and DPI scaling.
    QMenu *fontMenu = m_ui->menuView->addMenu(tr("Font Size"));
    fontMenu->setObjectName(QStringLiteral("fontSizeMenu"));
    m_increaseFontAction = fontMenu->addAction(tr("Increase Font Size"));
    m_increaseFontAction->setObjectName(QStringLiteral("fontSizeIncrease"));
    m_increaseFontAction->setShortcuts({
        QKeySequence(Qt::CTRL | Qt::Key_Plus),
        QKeySequence(Qt::CTRL | Qt::Key_Equal)
    });
    m_decreaseFontAction = fontMenu->addAction(tr("Decrease Font Size"));
    m_decreaseFontAction->setObjectName(QStringLiteral("fontSizeDecrease"));
    m_decreaseFontAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Minus));
    fontMenu->addSeparator();
    m_resetFontAction = fontMenu->addAction(tr("Reset Font Size"));
    m_resetFontAction->setObjectName(QStringLiteral("fontSizeReset"));
    m_resetFontAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));

    connect(m_increaseFontAction, &QAction::triggered, this,
            [this]() { applyFontPointSize(m_fontPointSize + 1); });
    connect(m_decreaseFontAction, &QAction::triggered, this,
            [this]() { applyFontPointSize(m_fontPointSize - 1); });
    connect(m_resetFontAction, &QAction::triggered, this,
            [this]() { applyFontPointSize(m_baseFontPointSize); });

    m_ui->menuOptions->clear();
    m_ui->menuOptions->addAction(m_ui->actionCreateTorrent);
    m_ui->menuOptions->addAction(m_ui->actionOptions);
    m_ui->actionOptions->setEnabled(true);
    m_ui->actionOptions->setText(tr("Preferences..."));

    // Native vertical Preferences pages are always reachable, even without
    // a currently connected server: local path mappings belong to saved
    // server profiles and never require an authenticated session.
    connect(m_ui->actionOptions, &QAction::triggered, this, [this]() {
        openPreferences(activeConnection());
    });

    m_ui->menuHelp->clear();
    m_ui->menuHelp->addAction(m_ui->actionAbout);

    m_ui->actionTopToolBar->setChecked(true);
    m_ui->actionShowStatusbar->setChecked(true);
    m_ui->actionShowFiltersSidebar->setChecked(true);
    connect(m_ui->actionTopToolBar, &QAction::toggled,
        m_ui->toolBar, &QWidget::setVisible);
    connect(m_ui->actionShowStatusbar, &QAction::toggled,
        m_ui->statusBar, &QWidget::setVisible);
    connect(m_ui->actionShowFiltersSidebar, &QAction::toggled, this,
        [this](bool visible) {
            if (auto *pane = activePane())
                pane->sidebar()->setVisible(visible);
        });

    connect(m_connectAction, &QAction::triggered, this, [this]() {
        ConnectionDialog dialog(*m_settings, this, m_vault);
        if (dialog.exec() == QDialog::Accepted)
            connectToProfile(dialog.selectedProfile(), dialog.password());
    });
    connect(m_disconnectAction, &QAction::triggered, this,
        &MainWindow::disconnectCurrentServer);
    connect(m_ui->actionExit, &QAction::triggered, this, &MainWindow::quitCompletely);
    connect(m_ui->actionAbout, &QAction::triggered, this, [this]()
    {
        QMessageBox dialog(QMessageBox::Information, tr("About qBittorrent Remote"),
            tr("A native Qt desktop client for remote qBittorrent servers.\n"
               "Live torrent synchronization uses each connected server's Web API."),
            QMessageBox::Ok, this);
        dialog.setObjectName(QStringLiteral("aboutDialog"));
        trackDialogGeometry(dialog, QStringLiteral("About"));
        dialog.exec();
    });
    const auto bindCommand = [this](QAction *action,
                                   Remote::RemoteSession::TorrentCommand command)
    {
        connect(action, &QAction::triggered, this, [this, command]() {
            if (const auto server = activeConnection())
                server->session->commandTorrents(command, selectedTorrentHashes());
        });
    };
    bindCommand(m_ui->actionStart, Remote::RemoteSession::TorrentCommand::Start);
    bindCommand(m_ui->actionStop, Remote::RemoteSession::TorrentCommand::Stop);
    bindCommand(m_ui->actionTopQueuePos, Remote::RemoteSession::TorrentCommand::TopPriority);
    bindCommand(m_ui->actionIncreaseQueuePos, Remote::RemoteSession::TorrentCommand::IncreasePriority);
    bindCommand(m_ui->actionDecreaseQueuePos, Remote::RemoteSession::TorrentCommand::DecreasePriority);
    bindCommand(m_ui->actionBottomQueuePos, Remote::RemoteSession::TorrentCommand::BottomPriority);

    connect(m_ui->actionDelete, &QAction::triggered, this, [this]() {
        removeRemoteTorrents(activeConnection(), selectedTorrentHashes());
    });

    // A duplicate is identified by its actual infohash in the chosen
    // server's synchronized model. Never mistake an arbitrary Web API failure
    // for a duplicate, and never route tracker changes to another server.
    const auto offerTrackerMerge = [this](const std::shared_ptr<SessionContext> &server,
                                          const TorrentIdentity &identity,
                                          bool confirm, const QJsonValue &mergeDefault) {
        if (!server || !server->connected) return false;
        const QString hash = existingHash(*server->pane->model(), identity.hashes);
        if (hash.isEmpty()) return false;

        const QJsonObject record = server->pane->model()->torrents().value(hash);
        const QJsonValue privateValue = record.value(QStringLiteral("private"));
        const QJsonValue legacyPrivate = record.value(QStringLiteral("is_private"));
        if (identity.isPrivate || privateValue.toBool() || legacyPrivate.toBool())
        {
            QMessageBox::information(this, tr("Torrent already exists"),
                tr("This torrent is already present on %1. "
                   "Trackers cannot be merged because it is a private torrent.")
                    .arg(server->profile.name));
            return true;
        }
        // A magnet's privacy is unknown until its metadata arrives.
        // Never send a tracker-changing request unless the selected server
        // explicitly reports that its existing torrent is non-private.
        if (!privateValue.isBool() && !legacyPrivate.isBool())
        {
            QMessageBox::information(this, tr("Torrent already exists"),
                tr("This torrent is already present on %1, but its private status "
                   "is not available. Trackers were not merged.")
                    .arg(server->profile.name));
            return true;
        }

        if (identity.trackers.isEmpty())
        {
            QMessageBox::information(this, tr("Torrent already exists"),
                tr("This torrent is already present on %1. "
                   "No trackers were provided to merge.").arg(server->profile.name));
            return true;
        }

        // Automatic merging is allowed only by a known true server default.
        // Recheck the selected connection and privacy after any modal prompt
        // or asynchronous preference read.
        const QPointer<MainWindow> owner(this);
        const auto merge = [owner, server, hash, identity]() {
            if (!owner || !owner->containsConnection(server) || !server->connected)
                return;
            const auto record = server->pane->model()->torrents().value(hash);
            const auto privacy = record.value(QStringLiteral("private"));
            const auto legacy = record.value(QStringLiteral("is_private"));
            if (identity.isPrivate || privacy.toBool() || legacy.toBool()
                || (!privacy.isBool() && !legacy.isBool()))
                return;
            server->session->addTorrentTrackers(hash, identity.trackers);
        };
        if (!confirm)
        {
            if (mergeDefault.isBool())
            {
                if (mergeDefault.toBool()) merge();
            }
            else
            {
                // Direct additions have no options dialog to read defaults.
                server->session->requestPreferences(
                    [merge](const QJsonObject &values) {
                        const auto setting = values.value(QStringLiteral("merge_trackers"));
                        if (setting.isBool() && setting.toBool()) merge();
                    }, [](const QString &) {});
            }
            return true;
        }

        QMessageBox prompt(QMessageBox::Question, tr("Torrent already exists"),
            tr("This torrent is already present on %1. "
               "Do you want to merge its trackers?").arg(server->profile.name),
            QMessageBox::Yes | QMessageBox::No, this);
        prompt.setObjectName(QStringLiteral("duplicateTorrentTrackersDialog"));
        prompt.setInformativeText(tr(
            "Tracker URLs from the new torrent will be added to the existing "
            "torrent on this server. The torrent will not be added again."));
        prompt.setDefaultButton(mergeDefault.isBool() && mergeDefault.toBool()
            ? QMessageBox::Yes : QMessageBox::No);
        if (prompt.exec() == QMessageBox::Yes)
            merge();
        return true; // Duplicate was handled even when the user declined.
    };

    // Menu and desktop drops share the actual server-selecting Add Torrent
    // dialog, duplicate safeguards and authenticated remote Web API workflow.
    m_addLinks = [this, offerTrackerMerge](const QString &initialLinks) {
        const auto server = activeConnection();
        if (!server || !server->connected)
            return;
        const bool showOptions = m_settings->value(
            QStringLiteral("Preferences/Downloads/ShowAdditionDialog"), true).toBool();
        if (!showOptions)
        {
            QString links = initialLinks;
            if (links.trimmed().isEmpty())
            {
                bool accepted = false;
                links = QInputDialog::getMultiLineText(this, tr("Download from URLs — %1").arg(server->profile.name),
                    tr("Torrent links (one per line):"), {}, &accepted);
                if (!accepted || links.trimmed().isEmpty()) return;
            }
            if (!containsConnection(server) || !server->connected) return;
            // Filter known magnet duplicates individually even in direct
            // multi-link additions; unknown URL metadata stays server-side.
            QStringList pending;
            for (const auto &source : links.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
                if (!source.trimmed().isEmpty()
                    && !offerTrackerMerge(server, magnetIdentity(source.trimmed()), false, {}))
                    pending.append(source.trimmed());
            if (!pending.isEmpty() && containsConnection(server) && server->connected)
            {
                Remote::AddTorrentOptions defaults;
                defaults.useServerDefaults = true;
                server->session->addTorrentUrls(pending.join(QLatin1Char('\n')), defaults);
            }
            return;
        }
        RemoteAddTorrentDialog dialog(*m_settings, server->profile,
            RemoteAddTorrentDialog::Source::Links, initialLinks, {}, {}, this,
            connectedServerProfiles(),
            [this](const QString &id) { return static_cast<bool>(connectedById(id)); }, {},
            [this](const QString &id) -> Remote::RemoteSession * {
                const auto target = connectedById(id);
                return target ? target->session : nullptr;
            });
        trackDialogGeometry(dialog, QStringLiteral("AddTorrent"));
        if (dialog.exec() != QDialog::Accepted)
            return;
        const auto target = connectedById(dialog.selectedServerId());
        if (!target)
        {
            QMessageBox::warning(this, tr("Server disconnected"),
                                 tr("The selected server disconnected before the torrent "
                                    "could be submitted. Nothing was added."));
            return;
        }
        const bool confirm = m_settings->value(
            QStringLiteral("Preferences/Downloads/ConfirmMergeTrackers"), true).toBool();
        QStringList pending;
        for (const auto &source : dialog.links().split(QLatin1Char('\n'), Qt::SkipEmptyParts))
            if (!source.trimmed().isEmpty()
                && !offerTrackerMerge(target, magnetIdentity(source.trimmed()), confirm,
                                      dialog.mergeTrackersDefault()))
                pending.append(source.trimmed());
        if (!pending.isEmpty() && containsConnection(target) && target->connected)
            target->session->addTorrentUrls(pending.join(QLatin1Char('\n')), dialog.options());
    };
    connect(m_ui->actionDownloadFromURL, &QAction::triggered,
            this, [this]() { m_addLinks({}); });

    m_addTorrentFiles = [this, offerTrackerMerge](const QStringList &filenames) {
        const auto server = activeConnection();
        if (!server || !server->connected || filenames.isEmpty()) return;

        QList<Remote::TorrentUpload> uploads;
        uploads.reserve(filenames.size());
        constexpr qint64 maxSingleBytes = 32LL * 1024 * 1024;
        constexpr qint64 maxBatchBytes = 128LL * 1024 * 1024;
        qint64 totalBytes = 0;
        for (const QString &filename : filenames)
        {
            QFile file(filename);
            if (!file.open(QIODevice::ReadOnly))
            {
                QMessageBox::warning(this, tr("Cannot open torrent file"),
                    tr("Cannot read %1: %2").arg(filename, file.errorString()));
                return;
            }
            const qint64 size = file.size();
            if (size <= 0 || size > maxSingleBytes
                || totalBytes > maxBatchBytes - size)
            {
                QMessageBox::warning(this, tr("Invalid torrent metadata"),
                    tr("%1 is empty, exceeds 32 MiB, or exceeds the 128 MiB batch limit.")
                        .arg(filename));
                return; // Reject the whole batch, no partial submissions.
            }
            const QByteArray contents = file.readAll();
            if (contents.size() != size)
            {
                QMessageBox::warning(this, tr("Cannot read torrent file"),
                    tr("Could not read all the metadata in %1.").arg(filename));
                return;
            }
            totalBytes += size;
            uploads.append({QFileInfo(filename).fileName(), contents});
        }
        if (uploads.size() > 256)
        {
            QMessageBox::warning(this, tr("Too many torrent files"),
                tr("Select no more than 256 .torrent files at once."));
            return;
        }

        if (!containsConnection(server) || !server->connected) return;
        const bool batch = uploads.size() > 1;
        const bool showOptions = m_settings->value(
            QStringLiteral("Preferences/Downloads/ShowAdditionDialog"), true).toBool();
        const bool confirm = showOptions && m_settings->value(
            QStringLiteral("Preferences/Downloads/ConfirmMergeTrackers"), true).toBool();
        auto target = server;
        Remote::AddTorrentOptions options;
        QJsonValue mergeDefault(QJsonValue::Undefined);
        if (showOptions)
        {
            RemoteAddTorrentDialog dialog(*m_settings, server->profile,
                batch ? RemoteAddTorrentDialog::Source::TorrentFiles
                      : RemoteAddTorrentDialog::Source::TorrentFile,
                {}, batch ? QString() : uploads.first().filename,
                batch ? QByteArray() : uploads.first().contents,
                this, connectedServerProfiles(),
                [this](const QString &id) { return static_cast<bool>(connectedById(id)); },
                batch ? uploads : QList<Remote::TorrentUpload>{},
                [this](const QString &id) -> Remote::RemoteSession * {
                    const auto target = connectedById(id);
                    return target ? target->session : nullptr;
                });
            trackDialogGeometry(dialog, QStringLiteral("AddTorrent"));
            if (dialog.exec() != QDialog::Accepted)
                return;
            target = connectedById(dialog.selectedServerId());
            if (!target)
            {
                QMessageBox::warning(this, tr("Server disconnected"),
                    tr("The selected server disconnected before the torrents could be submitted. Nothing was added."));
                return;
            }
            options = dialog.options();
            mergeDefault = dialog.mergeTrackersDefault();
        }
        else
            options.useServerDefaults = true;

        if (!batch)
        {
            // Preserve the single-file duplicate tracker-merge workflow.
            Remote::TorrentMetadata metadata;
            if (Remote::TorrentMetadataReader::read(uploads.first().contents, metadata))
            {
                TorrentIdentity identity;
                if (!metadata.infoHashV1.isEmpty()) identity.hashes.append(metadata.infoHashV1);
                if (!metadata.infoHashV2.isEmpty()) identity.hashes.append(metadata.infoHashV2);
                identity.trackers = metadata.trackers;
                identity.isPrivate = metadata.isPrivate;
                if (offerTrackerMerge(target, identity, confirm, mergeDefault))
                    return;
            }
            target->session->addTorrentFile(uploads.first().filename, uploads.first().contents, options);
            return;
        }

        // Batch submission is one request with shared options. Validate all
        // metadata first so one malformed file cannot reject the entire POST.
        // The server can change in the dialog, so detect duplicates only
        // against the *chosen* remote session after the user accepts.
        QList<Remote::TorrentUpload> toUpload;
        QSet<QString> hashesSeen;
        QStringList skipped;
        for (const Remote::TorrentUpload &upload : uploads)
        {
            Remote::TorrentMetadata metadata;
            QString error;
            if (!Remote::TorrentMetadataReader::read(upload.contents, metadata, &error)
                || (metadata.infoHashV1.isEmpty() && metadata.infoHashV2.isEmpty()))
            {
                QMessageBox::warning(this, tr("Invalid torrent file"),
                    tr("%1 could not be decoded: %2. No files were uploaded.")
                        .arg(upload.filename, error));
                return;
            }
            TorrentIdentity identity;
            if (!metadata.infoHashV1.isEmpty()) identity.hashes.append(metadata.infoHashV1);
            if (!metadata.infoHashV2.isEmpty()) identity.hashes.append(metadata.infoHashV2);
            bool duplicate = !existingHash(*target->pane->model(), identity.hashes).isEmpty();
            for (const QString &hash : identity.hashes)
                duplicate = duplicate || hashesSeen.contains(hash);
            if (duplicate)
            {
                skipped.append(upload.filename);
                continue;
            }
            for (const QString &hash : identity.hashes)
                hashesSeen.insert(hash);
            toUpload.append(upload);
        }
        if (!skipped.isEmpty())
        {
            const QString explanation = tr(
                "%1 torrent(s) already exist on %2 or are duplicates in this batch. "
                "They will not be uploaded or have trackers modified.")
                .arg(skipped.size()).arg(target->profile.name);
            if (toUpload.isEmpty())
            {
                QMessageBox::information(this, tr("No new torrents"), explanation);
                return;
            }
            QMessageBox::information(this, tr("Skipping duplicate torrents"),
                explanation + tr("\n\nUploading the remaining %1 torrent(s).")
                    .arg(toUpload.size()));
        }
        if (!containsConnection(target) || !target->connected)
            return;
        target->session->addTorrentFiles(toUpload, options);
    };
    connect(m_ui->actionOpen, &QAction::triggered, this, [this]() {
        if (!activeConnection()) return;
        QFileDialog picker(this, tr("Select local .torrent metadata"));
        picker.setObjectName(QStringLiteral("addTorrentFileDialog"));
        picker.setFileMode(QFileDialog::ExistingFiles);
        picker.setNameFilters({tr("Torrent files (*.torrent)"), tr("All files (*)")});
        trackDialogGeometry(picker, QStringLiteral("SelectTorrentFile"));
        if (picker.exec() == QDialog::Accepted)
            m_addTorrentFiles(picker.selectedFiles());
    });
}



void MainWindow::enableTorrentDrops(QWidget *widget)
{
    widget->setAcceptDrops(true);
    widget->installEventFilter(this);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if ((watched == m_downloadRateLabel || watched == m_uploadRateLabel
            || watched == m_serverFreeSpaceLabel)
        && (event->type() == QEvent::FontChange
            || event->type() == QEvent::StyleChange
            || event->type() == QEvent::LocaleChange))
    {
        // Parent font events can arrive before propagation to child labels.
        // Measure after the labels have processed their own change events.
        QTimer::singleShot(0, this, [this]() { updateStatusBarFieldWidths(); });
    }
    if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove
        || event->type() == QEvent::Drop)
    {
        const QMimeData *mime = nullptr;
        if (event->type() == QEvent::DragEnter)
            mime = static_cast<QDragEnterEvent *>(event)->mimeData();
        else if (event->type() == QEvent::DragMove)
            mime = static_cast<QDragMoveEvent *>(event)->mimeData();
        else
            mime = static_cast<QDropEvent *>(event)->mimeData();
        const DroppedTorrentSources sources = droppedTorrentSources(mime);
        const auto server = activeConnection();
        if (server && server->connected && sources.valid())
        {
            if (event->type() == QEvent::DragEnter)
                static_cast<QDragEnterEvent *>(event)->acceptProposedAction();
            else if (event->type() == QEvent::DragMove)
                static_cast<QDragMoveEvent *>(event)->acceptProposedAction();
            else
            {
                auto *drop = static_cast<QDropEvent *>(event);
                drop->setDropAction(Qt::CopyAction);
                drop->accept();
                if (!sources.files.isEmpty() && m_addTorrentFiles)
                    m_addTorrentFiles(sources.files);
                else if (!sources.links.isEmpty() && m_addLinks)
                    m_addLinks(sources.links.join(QLatin1Char('\n')));
            }
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::openPreferences(const std::shared_ptr<SessionContext> &server)
{
    if (server && !containsConnection(server))
        return;
    const QString profileId = server ? server->profile.id : m_preferredServerId;
    RemotePreferencesDialog dialog(*m_settings, profileId, this,
                                  server && server->connected ? server->session : nullptr,
                                  server && server->connected ? server->pane->model() : nullptr);
    connect(&dialog, &RemotePreferencesDialog::localBehaviorApplied,
            this, &MainWindow::applyLocalBehaviorPreferences);
    trackDialogGeometry(dialog, QStringLiteral("Preferences"));
    dialog.exec();
    refreshContentPathMappings();
}

void MainWindow::refreshContentPathMappings()
{
    const Remote::PathMappingStore store(*m_settings);
    for (const auto &connection : m_connections)
        connection->pane->setContentPathMappings(store.mappings(connection->profile.id));
}

void MainWindow::setSelectedTorrentLocation()
{
    const auto connection = activeConnection();
    if (!connection || !connection->connected)
        return;
    const QStringList hashes = selectedTorrentHashes();
    if (hashes.isEmpty())
        return;

    // Present only a destination belonging to this server. Several torrents
    // with different old paths intentionally have no misleading default.
    QString previousPath;
    bool first = true;
    for (const QString &hash : hashes)
    {
        const QString path = connection->pane->model()->torrents().value(hash)
            .value(QStringLiteral("save_path")).toString();
        if (first)
        {
            previousPath = path;
            first = false;
        }
        else if (previousPath != path)
        {
            previousPath.clear();
            break;
        }
    }

    const Remote::PathMappingStore store(*m_settings);
    const auto mappings = store.mappings(connection->profile.id);

    // Unlike the original local qBittorrent, we cannot browse the NAS's
    // filesystem directly. Accept an absolute server path, or browse a
    // configured local mount and translate it to a remote POSIX path.
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("setTorrentLocationDialog"));
    dialog.setWindowTitle(tr("Set Location"));
    dialog.setMinimumWidth(480);
    auto *layout = new QVBoxLayout(&dialog);
    auto *label = new QLabel(tr("New save path on %1:")
        .arg(connection->profile.name), &dialog);
    layout->addWidget(label);
    auto *destination = new RemoteFileSystemPathComboEdit(&dialog);
    destination->setObjectName(QStringLiteral("setTorrentLocationSelector"));
    destination->comboBox()->setObjectName(QStringLiteral("remoteSetLocationPath"));
    destination->setInsertPolicy(QComboBox::NoInsert);
    destination->setToolTip(tr("Enter an absolute path on the remote server "
        "(for example /share/Downloads), or browse a mapped local NAS mount."));
    for (const auto &mapping : mappings)
        destination->addItem(mapping.remoteRoot);
    destination->setCurrentIndex(-1);
    destination->setEditText(previousPath);
    layout->addWidget(destination);
    auto *warning = new QLabel(tr(
        "qBittorrent will move the selected torrents' data on the remote "
        "server. The destination must be writable on that server."), &dialog);
    warning->setWordWrap(true);
    layout->addWidget(warning);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->setObjectName(QStringLiteral("setTorrentLocationButtons"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(destination, &RemoteFileSystemPathComboEdit::browseRequested,
            &dialog, [&, this]() {
        if (mappings.isEmpty())
        {
            QMessageBox::information(&dialog, tr("No path mappings"),
                tr("Configure a local NAS mount in Preferences → Servers, "
                   "or enter an absolute remote server path."));
            return;
        }
        const QString local = QFileDialog::getExistingDirectory(
            &dialog, tr("Choose mapped local NAS directory"),
            mappings.first().localRoot);
        if (local.isEmpty())
            return;
        const QString remote = Remote::PathMappingStore::toRemote(local, mappings);
        if (remote.isEmpty())
        {
            QMessageBox::warning(&dialog, tr("Unmapped directory"),
                tr("The chosen directory is not covered by this server's path mappings."));
            return;
        }
        destination->setEditText(remote);
    });

    QString chosenLocation;
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&, this]() {
        const QString input = destination->currentText().trimmed();
        const QString mapped = Remote::PathMappingStore::toRemote(input, mappings);
        const QString remote = mapped.isEmpty() ? input : mapped;
        if (remote.isEmpty() || remote == QLatin1String("/")
            || !remote.startsWith(QLatin1Char('/'))
            || remote.contains(QLatin1Char('\\'))
            || remote.contains(QLatin1Char('\n'))
            || remote.contains(QLatin1Char('\r')))
        {
            QMessageBox::warning(&dialog, tr("Invalid remote location"),
                tr("Enter a non-root absolute POSIX path on %1, or browse a mapped "
                   "local NAS folder.").arg(connection->profile.name));
            return;
        }
        chosenLocation = remote;
        dialog.accept();
    });
    if (dialog.exec() != QDialog::Accepted || chosenLocation == previousPath)
        return;

    // The user may have changed tabs or disconnected while the modal dialog
    // was open. Never relocate torrents on a different remote server.
    if (!containsConnection(connection) || !connection->connected
        || activeConnection() != connection || selectedTorrentHashes() != hashes)
        return;
    connection->session->setTorrentLocation(hashes, chosenLocation);
}

void MainWindow::openSelectedTorrentDestinations()
{
    const auto connection = activeConnection();
    if (!connection || !connection->connected)
        return;
    const QStringList hashes = selectedTorrentHashes();
    if (hashes.isEmpty())
        return;

    const Remote::PathMappingStore store(*m_settings);
    const auto mappings = store.mappings(connection->profile.id);
    QSet<QString> opened;
    QStringList unavailable;

    // A qBittorrent remote path is not directly accessible on the desktop.
    // Open only an existing local mount covered by this server's mappings.
    for (const QString &hash : hashes)
    {
        const QJsonObject record = connection->pane->model()->torrents().value(hash);
        if (record.isEmpty())
            continue;

        const QString content = record.value(QStringLiteral("content_path")).toString();
        const QString save = record.value(QStringLiteral("save_path")).toString();
        QStringList candidates;
        if (!content.isEmpty()) candidates.append(content);
        if (!save.isEmpty() && save != content) candidates.append(save);

        QString destination;
        for (const QString &remote : candidates)
        {
            const QString local = Remote::PathMappingStore::toLocal(remote, mappings);
            if (local.isEmpty())
                continue;
            const QFileInfo localInfo(local);
            if (localInfo.isDir())
                destination = localInfo.absoluteFilePath();
            else if (localInfo.isFile())
                destination = localInfo.absolutePath(); // Reveal single-file location.
            if (!destination.isEmpty())
                break;
        }

        if (destination.isEmpty())
        {
            unavailable.append(record.value(QStringLiteral("name")).toString(hash));
            continue;
        }
        const QString normalized = QDir::cleanPath(destination);
        if (opened.contains(normalized))
            continue;
        opened.insert(normalized);
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(normalized)))
            unavailable.append(record.value(QStringLiteral("name")).toString(hash));
    }

    if (!unavailable.isEmpty())
        QMessageBox::information(this, tr("Cannot open destination folder"),
            tr("No accessible local mapped folder was found for:\n%1\n\n"
               "Check the mounted NAS share and its mapping under "
               "Preferences → Servers. The remote torrent location was not changed.")
                .arg(unavailable.join(QLatin1Char('\n'))));
}

QStringList MainWindow::selectedTorrentHashes() const
{
    const auto server = activeConnection();
    if (!server || !server->connected)
        return {};

    const ServerPane *pane = server->pane;
    auto *selection = pane->table()->selectionModel();
    if (!selection)
        return {};

    QSet<QString> seen;
    QStringList hashes;
    for (const QModelIndex &selected : selection->selectedRows())
    {
        const QModelIndex source = pane->proxy()->mapToSource(selected);
        const QString hash = pane->model()->hashForRow(source.row());
        if (!hash.isEmpty() && !seen.contains(hash))
        {
            seen.insert(hash);
            hashes.append(hash);
        }
    }
    return hashes;
}

void MainWindow::updateTorrentActionState()
{
    const auto server = activeConnection();
    const bool ready = server && server->connected;
    const bool selected = ready && !selectedTorrentHashes().isEmpty();

    m_ui->actionOpen->setEnabled(ready);
    m_ui->actionDownloadFromURL->setEnabled(ready);
    m_ui->actionOpenDestinationFolder->setEnabled(selected);
    for (QAction *action : {
        m_ui->actionDelete, m_ui->actionStart, m_ui->actionStop,
        m_ui->actionTopQueuePos, m_ui->actionIncreaseQueuePos,
        m_ui->actionDecreaseQueuePos, m_ui->actionBottomQueuePos
    })
        action->setEnabled(selected);
}

std::shared_ptr<MainWindow::SessionContext> MainWindow::activeConnection() const
{
    if (!m_views)
        return {};
    QWidget *current = m_views->currentWidget();
    for (const auto &connection : m_connections)
    {
        if (connection->pane == current)
            return connection;
    }
    return {};
}

bool MainWindow::containsConnection(const std::shared_ptr<SessionContext> &connection) const
{
    return m_connections.contains(connection);
}

std::shared_ptr<MainWindow::SessionContext> MainWindow::connectedById(const QString &id) const
{
    for (const auto &connection : m_connections)
        if (connection->connected && connection->profile.id == id)
            return connection;
    return {};
}

QList<Remote::ServerProfile> MainWindow::connectedServerProfiles() const
{
    QList<Remote::ServerProfile> profiles;
    for (const auto &connection : m_connections)
        if (connection->connected)
            profiles.append(connection->profile);
    return profiles;
}

ServerPane *MainWindow::activePane() const
{
    return m_views ? static_cast<ServerPane *>(m_views->currentWidget()) : nullptr;
}

void MainWindow::updateTorrentCount()
{
    if (!m_torrentCountLabel)
        return;
    const auto connection = activeConnection();
    if (!connection || !connection->connected || !connection->pane)
    {
        m_torrentCountLabel->setText(tr("Torrents: —"));
        return;
    }
    const auto *pane = connection->pane;
    // The proxy applies directory/status/category/tag/tracker/search filters;
    // the source owns every synchronized torrent, even if currently hidden.
    m_torrentCountLabel->setText(tr("Torrents: %1 / %2")
        .arg(pane->proxy()->rowCount())
        .arg(pane->model()->rowCount()));
}

void MainWindow::applyLocalBehaviorPreferences()
{
    updateTraySettings();
    const bool contentDrag = m_settings->value(
        QStringLiteral("Preferences/Behavior/TorrentContentDragEnabled"), true).toBool();
    const bool stateColors = m_settings->value(
        QStringLiteral("Preferences/Behavior/StateColors"), true).toBool();
    const bool progressColor = m_settings->value(
        QStringLiteral("Preferences/Behavior/ProgressFollowsState"), false).toBool();
    const bool separateTracker = m_settings->value(
        QStringLiteral("Preferences/Behavior/SeparateTrackerStatus"), false).toBool();
    const bool alternating = m_settings->value(
        QStringLiteral("Preferences/Behavior/AlternatingRows"), true).toBool();
    const bool hideZero = m_settings->value(
        QStringLiteral("Preferences/Behavior/HideZero"), false).toBool();
    const int mode = m_settings->value(
        QStringLiteral("Preferences/Behavior/HideZeroMode"), 0).toInt();

    const bool hideZeroStatus = m_settings->value(
        QStringLiteral("Preferences/Behavior/HideZeroStatusFilters"), false).toBool();
    const auto apply = [alternating, hideZero, mode, hideZeroStatus,
                        stateColors, progressColor, separateTracker, contentDrag](ServerPane *pane) {
        if (!pane)
            return;
        pane->table()->setAlternatingRowColors(alternating);
        pane->model()->setHideZeroValues(hideZero, mode);
        pane->sidebar()->setHideZeroStatusFilters(hideZeroStatus);
        pane->model()->setUseStateColors(stateColors);
        pane->setProgressFollowsStateColors(progressColor && stateColors);
        pane->setContentDragEnabled(contentDrag);
        pane->setSeparateTrackerStatusFilter(separateTracker);
    };
    apply(m_emptyPane);
    for (const auto &connection : m_connections)
        apply(connection->pane);
    updateServerFreeSpace();
}

void MainWindow::setupTray()
{
    m_trayIcon = new QSystemTrayIcon(this);
    m_trayIcon->setObjectName(QStringLiteral("remoteTrayIcon"));
    m_trayMenu = new QMenu(this);
    m_trayMenu->setObjectName(QStringLiteral("remoteTrayMenu"));
    // This is the upstream 5.3.0 tray layout, using the exact native
    // .ui QActions and their icons. The active remote profile supplies each
    // session operation; a hidden window must still be able to use the menu.
    m_trayToggleAction = m_trayMenu->addAction(tr("Show"));
    m_trayToggleAction->setObjectName(QStringLiteral("trayShowWindow"));
    connect(m_trayToggleAction, &QAction::triggered, this, [this]() {
        if (!isVisible() || isMinimized())
        {
            if (m_wasMaximizedBeforeTray) showMaximized();
            else showNormal();
        }
        raise();
        activateWindow();
    });
    m_trayMenu->addSeparator();
    m_trayMenu->addAction(m_ui->actionOpen);
    m_trayMenu->addAction(m_ui->actionDownloadFromURL);
    m_trayMenu->addSeparator();
    m_trayMenu->addAction(m_ui->actionUseAlternativeSpeedLimits);
    m_trayMenu->addAction(m_ui->actionSetGlobalSpeedLimits);
    m_trayMenu->addSeparator();
    // Remote-safe substitute for upstream Pause Session: close one desktop
    // connection without pausing or altering any torrent on the NAS.
    m_trayDisconnectMenu = m_trayMenu->addMenu(
        upstreamIcon("disconnected"), tr("Disconnect from Server"));
    m_trayDisconnectMenu->setObjectName(QStringLiteral("trayDisconnectMenu"));
    connect(m_trayDisconnectMenu, &QMenu::aboutToShow,
            this, &MainWindow::refreshTrayDisconnectMenu);
    m_trayMenu->addSeparator();
    m_trayMenu->addAction(m_ui->actionExit);
    m_trayIcon->setContextMenu(m_trayMenu);
    updateTrayActionState();
    m_trayIcon->setToolTip(tr("qBittorrent Remote"));
    connect(m_trayIcon, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger
                    || reason == QSystemTrayIcon::DoubleClick)
                    toggleWindowFromTray();
            });
}

void MainWindow::updateTrayActionState()
{
    if (!m_ui || !m_ui->actionUseAlternativeSpeedLimits)
        return;
    const auto server = activeConnection();
    const bool ready = server && server->connected;
    const bool altKnown = ready && server->altSpeedModeKnown;
    m_ui->actionSetGlobalSpeedLimits->setEnabled(ready);
    m_ui->actionUseAlternativeSpeedLimits->setEnabled(altKnown);
    {
        QSignalBlocker blocker(m_ui->actionUseAlternativeSpeedLimits);
        m_ui->actionUseAlternativeSpeedLimits->setChecked(
            altKnown && server->altSpeedMode);
    }
    updateStatusBarIndicators();
    if (m_trayDisconnectMenu)
    {
        const bool anyConnected = std::any_of(m_connections.cbegin(), m_connections.cend(),
            [](const auto &connection) { return connection && connection->connected; });
        m_trayDisconnectMenu->setEnabled(anyConnected);
    }
}

void MainWindow::updateStatusBarIndicators()
{
    if (!m_connectionStatusLabel || !m_alternativeSpeedButton)
        return;
    const auto server = activeConnection();
    const bool ready = server && server->connected;
    const QString status = ready ? server->pane->connectionStatus() : QString();
    const char *connectionIcon = "disconnected";
    QString explanation;
    if (!ready)
        explanation = tr("Not connected to a server");
    else if (status == QLatin1String("connected"))
    {
        connectionIcon = "connected";
        explanation = tr("Online");
    }
    else if (status == QLatin1String("firewalled"))
    {
        connectionIcon = "firewalled";
        explanation = tr("No direct connections. This may indicate network configuration problems.");
    }
    else if (status == QLatin1String("disconnected"))
        explanation = tr("Offline. This usually means that qBittorrent failed to listen on the selected port for incoming connections.");
    else
        explanation = tr("BitTorrent connectivity has not been reported by the selected server.");
    const int small = style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this);
    m_connectionStatusLabel->setPixmap(upstreamIcon(connectionIcon).pixmap(QSize(small, small), devicePixelRatioF()));
    m_connectionStatusLabel->setToolTip(tr("<b>Connection Status:</b><br>%1").arg(explanation));
    m_connectionStatusLabel->setAccessibleDescription(explanation);

    const bool known = ready && server->altSpeedModeKnown;
    const bool alternative = known && server->altSpeedMode;
    m_alternativeSpeedButton->setEnabled(known);
    m_alternativeSpeedButton->setChecked(alternative);
    m_alternativeSpeedButton->setIcon(upstreamIcon(alternative ? "slow" : "slow_off"));
    m_alternativeSpeedButton->setToolTip(!known
        ? tr("Alternative speed limit mode is unavailable from the selected server.")
        : alternative ? tr("Click to switch to regular speed limits")
                      : tr("Click to switch to alternative speed limits"));
}

void MainWindow::refreshTrayDisconnectMenu()
{
    if (!m_trayDisconnectMenu)
        return;
    // Populate on submenu open, not during QAction::triggered. Destroying the
    // triggered action while Qt is dispatching it would be unsafe.
    m_trayDisconnectMenu->clear();
    for (const auto &connection : m_connections)
    {
        if (!connection || !connection->connected)
            continue;
        const QString profileId = connection->profile.id;
        auto *action = m_trayDisconnectMenu->addAction(connection->profile.name);
        action->setData(profileId);
        connect(action, &QAction::triggered, this, [this, profileId]() {
            if (const auto live = connectedById(profileId))
                removeConnection(live);
            // This is strictly a client-side disconnect + Web API logout;
            // downloading and seeding on the server keep running.
        });
    }
}

void MainWindow::updateTraySettings()
{
    if (!m_trayIcon)
        return;
    // Use upstream's transparent symbolic artwork, not an opaque disc.
    const bool monochrome = m_settings->value(
        QStringLiteral("Preferences/Behavior/TrayIconStyle"), 0).toInt() == 1;
    const bool dark = palette().color(QPalette::Window).lightness() < 128;
    m_trayIcon->setIcon(monochrome
        ? upstreamIcon(dark ? "dark/qbittorrent-tray-mono" : "qbittorrent-tray-mono")
        : windowIcon());
    const bool enabled = m_settings->value(
        QStringLiteral("Preferences/Behavior/EnableTray"), false).toBool()
        && QSystemTrayIcon::isSystemTrayAvailable();
    if (!enabled)
    {
        // Never strand a hidden application after its tray icon is disabled.
        if (!isVisible() && m_trayIcon->isVisible())
            showNormal();
        m_trayIcon->hide();
        return;
    }
    m_trayIcon->show();
}

void MainWindow::toggleWindowFromTray()
{
    if (isVisible() && !isMinimized())
    {
        m_wasMaximizedBeforeTray = isMaximized();
        hide();
        return;
    }
    if (m_wasMaximizedBeforeTray)
        showMaximized();
    else
        showNormal();
    raise();
    activateWindow();
}

void MainWindow::quitCompletely()
{
    m_forceExit = true;
    // closeEvent() may reject exit when the user declines confirmation.
    // QApplication::quit() must never bypass that decision.
    const bool accepted = close();
    m_forceExit = false;
    if (accepted)
        QApplication::quit();
}

void MainWindow::showForStartup()
{
    const QString state = m_settings->value(
        QStringLiteral("Preferences/Behavior/StartupWindowState"),
        QStringLiteral("normal")).toString();
    const bool trayAvailable = m_trayIcon && m_trayIcon->isVisible();
    if (state == QLatin1String("hidden") && trayAvailable)
    {
        hide();
        return;
    }
    if (state == QLatin1String("minimized"))
        showMinimized();
    else
        show();
}

void MainWindow::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::WindowStateChange
        && isMinimized() && m_trayIcon && m_trayIcon->isVisible()
        && m_settings->value(
            QStringLiteral("Preferences/Behavior/MinimizeToTray"), false).toBool())
    {
        const auto *state = static_cast<QWindowStateChangeEvent *>(event);
        m_wasMaximizedBeforeTray = state->oldState().testFlag(Qt::WindowMaximized);
        QTimer::singleShot(0, this, [this]() {
            if (isMinimized() && m_trayIcon && m_trayIcon->isVisible())
                hide();
        });
    }
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::PaletteChange
        || event->type() == QEvent::ApplicationPaletteChange)
        updateTraySettings();
    if (event->type() == QEvent::FontChange
        || event->type() == QEvent::ApplicationFontChange
        || event->type() == QEvent::StyleChange)
        updateStatusBarFieldWidths();
}

void MainWindow::updateStatusBarFieldWidths()
{
    if (!m_downloadRateLabel || !m_uploadRateLabel || !m_serverFreeSpaceLabel)
        return;

    if (m_connectionStatusLabel && m_alternativeSpeedButton)
    {
        const int small = style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this);
        m_connectionStatusLabel->setFixedSize(small, small);
        m_alternativeSpeedButton->setIconSize(QSize(small, small));
        updateStatusBarIndicators();
    }

    // Reserve space using the actual label font and locale, not padded
    // strings (which shift with proportional fonts) or hard-coded pixels.
    // All rates have at most four integer digits before unit promotion.
    // The free-space PiB unit also covers the entire signed 64-bit range.
    const QLocale locale;
    const auto reserve = [&locale](QLabel *label, const QString &pattern,
                                 const QStringList &units) {
        const QFontMetrics metrics = label->fontMetrics();
        int width = metrics.horizontalAdvance(label->text());
        for (int digit = 1; digit <= 9; ++digit)
        {
            const QString number = locale.toString(digit * 1111.1, 'f', 1);
            for (const QString &unit : units)
                width = qMax(width, metrics.horizontalAdvance(
                    pattern.arg(number + QLatin1Char(' ') + unit)));
        }
        // QLabel's own margins are normally zero; allow for its style/frame.
        label->setFixedWidth(width + 2 * label->margin() + 2 * label->frameWidth() + 2);
        label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    };
    const QStringList speedUnits{QStringLiteral("B/s"), QStringLiteral("KiB/s"),
        QStringLiteral("MiB/s"), QStringLiteral("GiB/s"), QStringLiteral("TiB/s"),
        QStringLiteral("PiB/s"), QStringLiteral("EiB/s")};
    reserve(m_downloadRateLabel, tr("D: %1"), speedUnits);
    reserve(m_uploadRateLabel, tr("U: %1"), speedUnits);
    reserve(m_serverFreeSpaceLabel, tr("Free: %1"),
        {QStringLiteral("B"), QStringLiteral("KiB"), QStringLiteral("MiB"),
         QStringLiteral("GiB"), QStringLiteral("TiB"), QStringLiteral("PiB")});
}

void MainWindow::updateTransferRates(const ServerPane *pane)
{
    m_downloadRateLabel->setText(tr("D: %1").arg(
        pane ? pane->downloadRateText() : QStringLiteral("—")));
    m_uploadRateLabel->setText(tr("U: %1").arg(
        pane ? pane->uploadRateText() : QStringLiteral("—")));
}

void MainWindow::updateServerFreeSpace()
{
    if (!m_serverFreeSpaceLabel)
        return;
    const bool show = m_settings->value(
        QStringLiteral("Preferences/Behavior/ShowServerFreeSpace"), false).toBool();
    m_serverFreeSpaceLabel->setVisible(show);
    m_freeSpaceSeparator->setVisible(show);
    if (show)
    {
        const auto connection = activeConnection();
        m_serverFreeSpaceLabel->setText(connection && connection->connected
            ? connection->pane->freeSpaceText() : tr("Free: —"));
    }
}

void MainWindow::updateActiveUi()
{
    if (!m_searchEdit || !m_searchBy || !m_disconnectAction
        || !m_downloadRateLabel || !m_uploadRateLabel || !m_torrentCountLabel)
        return;

    const auto connection = activeConnection();
    auto *pane = activePane();

    // Tab switches must not overwrite another server's search filter.
    {
        QSignalBlocker searchBlocker(m_searchEdit);
        QSignalBlocker fieldBlocker(m_searchBy);
        m_searchEdit->setText(pane ? pane->searchTerm() : QString());
        m_searchBy->setCurrentIndex(pane ? pane->searchColumn() : 0);
    }

    const bool ready = connection && connection->connected;
    m_searchEdit->setEnabled(ready);
    m_searchBy->setEnabled(ready);
    m_disconnectAction->setEnabled(static_cast<bool>(connection));
    if (pane)
        pane->sidebar()->setVisible(m_ui->actionShowFiltersSidebar->isChecked());
    updateTorrentActionState();
    updateTrayActionState();
    updateTorrentCount();
    updateTransferRates(connection && ready ? pane : nullptr);
    updateServerFreeSpace();

    if (connection && !m_restoringStartup && !connection->profile.id.isEmpty())
    {
        m_preferredServerId = connection->profile.id;
        m_settings->setValue(QStringLiteral("Layout/ActiveServer"), m_preferredServerId);
    }

    if (!connection)
    {
        setWindowTitle(tr("qBittorrent Remote"));
        m_ui->statusBar->showMessage(tr("Not connected to a server"));
    }
    else
    {
        setWindowTitle(tr("qBittorrent Remote — %1").arg(connection->profile.name));
        if (ready)
            m_ui->statusBar->showMessage(
                tr("Connected to %1 — qBittorrent %2 (Web API %3).")
                    .arg(connection->profile.name, connection->version, connection->apiVersion));
        else
            m_ui->statusBar->showMessage(
                tr("Connecting to %1...").arg(connection->profile.name));
    }
}

void MainWindow::removeRemoteTorrents(const std::shared_ptr<SessionContext> &server, const QStringList &hashes)
{
    if (!server || !server->connected || hashes.isEmpty())
        return;

    const bool confirmDeletion = m_settings->value(
        QStringLiteral("Preferences/Behavior/ConfirmTorrentDeletion"), true).toBool();
    if (!confirmDeletion)
    {
        // With confirmations disabled, perform metadata-only deletion.
        // Never silently delete downloaded NAS content.
        server->session->commandTorrents(
            Remote::RemoteSession::TorrentCommand::Delete, hashes, false);
        return;
    }

    QMessageBox prompt(this);
    prompt.setObjectName(QStringLiteral("removeTorrentsDialog"));
    prompt.setIcon(QMessageBox::Warning);
    prompt.setWindowTitle(tr("Remove torrents"));
    prompt.setText(tr("Remove %1 selected torrent(s) from %2?")
                       .arg(hashes.size()).arg(server->profile.name));
    prompt.setInformativeText(tr("Removing a torrent does not delete its downloaded files "
                                  "unless you explicitly select the option below."));
    auto *deleteFiles = new QCheckBox(
        tr("Also delete downloaded files from the REMOTE server"), &prompt);
    prompt.setCheckBox(deleteFiles);
    prompt.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
    prompt.setDefaultButton(QMessageBox::Cancel);
    if (prompt.exec() != QMessageBox::Yes || !containsConnection(server)
        || !server->connected)
        return;

    server->session->commandTorrents(
        Remote::RemoteSession::TorrentCommand::Delete,
        hashes, deleteFiles->isChecked());
}

void MainWindow::removeConnection(const std::shared_ptr<SessionContext> &connection)
{
    if (!containsConnection(connection))
        return;

    if (connection->pane)
        connection->pane->saveLayout(*m_settings, connection->profile.id);
    const int tabIndex = m_views->indexOf(connection->pane);
    if (tabIndex >= 0)
        m_views->removeTab(tabIndex);

    m_connections.removeOne(connection);
    if (connection->session)
        connection->session->disconnectFromServer();
    if (connection->pane)
        connection->pane->deleteLater();

    showEmptyPane();
    updateActiveUi();
}

void MainWindow::connectToProfile(const Remote::ServerProfile &profile,
                                  const QString &password)
{
    // Re-selecting a live profile switches to its tab; edited endpoints must
    // reconnect instead of retaining credentials/cookies from an old URL.
    for (const auto &existing : m_connections)
    {
        if (existing->profile.id != profile.id)
            continue;
        if (existing->profile.url == profile.url
            && existing->profile.username == profile.username)
        {
            m_views->setCurrentWidget(existing->pane);
            return;
        }
        removeConnection(existing);
        break;
    }

    if (m_emptyPane)
    {
        const int index = m_views->indexOf(m_emptyPane);
        if (index >= 0)
            m_views->removeTab(index);
        m_emptyPane->saveLayout(*m_settings, QStringLiteral("empty-pane"));
        delete m_emptyPane;
        m_emptyPane = nullptr;
    }

    auto connection = std::make_shared<SessionContext>();
    connection->profile = profile;
    connection->pane = new ServerPane(m_views);
    connection->pane->setContentDragEnabled(m_settings->value(
        QStringLiteral("Preferences/Behavior/TorrentContentDragEnabled"), true).toBool());
    connection->pane->setContentPathMappings(
        Remote::PathMappingStore(*m_settings).mappings(profile.id));
    enableTorrentDrops(connection->pane);
    enableTorrentDrops(connection->pane->table()->viewport());
    connection->pane->restoreLayout(*m_settings, profile.id);
    connection->pane->table()->setAlternatingRowColors(m_settings->value(
        QStringLiteral("Preferences/Behavior/AlternatingRows"), true).toBool());
    connection->pane->model()->setHideZeroValues(m_settings->value(
        QStringLiteral("Preferences/Behavior/HideZero"), false).toBool(),
        m_settings->value(QStringLiteral("Preferences/Behavior/HideZeroMode"), 0).toInt());
    connection->pane->sidebar()->setHideZeroStatusFilters(m_settings->value(
        QStringLiteral("Preferences/Behavior/HideZeroStatusFilters"), false).toBool());
    connection->pane->model()->setUseStateColors(m_settings->value(
        QStringLiteral("Preferences/Behavior/StateColors"), true).toBool());
    connection->pane->setProgressFollowsStateColors(m_settings->value(
        QStringLiteral("Preferences/Behavior/ProgressFollowsState"), false).toBool()
        && m_settings->value(QStringLiteral("Preferences/Behavior/StateColors"), true).toBool());
    connection->pane->setSeparateTrackerStatusFilter(m_settings->value(
        QStringLiteral("Preferences/Behavior/SeparateTrackerStatus"), false).toBool());
    connection->session = new Remote::RemoteSession(this);
    auto *session = connection->session;
    m_connections.append(connection);

    // The user may choose separate double-click actions for downloads and
    // completed torrents. All actions use this exact remote session.
    connect(connection->pane->table(), &QTableView::doubleClicked,
            this, [this, connection](const QModelIndex &index) {
        if (!index.isValid() || !containsConnection(connection)
            || !connection->connected || activeConnection() != connection)
            return;
        const QStringList selected = selectedTorrentHashes();
        if (selected.size() != 1)
            return;
        const QJsonObject torrent = connection->pane->model()->torrents()
            .value(selected.first());
        if (torrent.isEmpty())
            return;
        const bool complete = torrent.value(QStringLiteral("progress")).toDouble() >= 1.0;
        const QString key = complete
            ? QStringLiteral("Preferences/Behavior/DoubleClickCompleted")
            : QStringLiteral("Preferences/Behavior/DoubleClickDownloading");
        const QString action = m_settings->value(key,
            complete ? QStringLiteral("folder") : QStringLiteral("toggle")).toString();
        if (action == QLatin1String("folder"))
            openSelectedTorrentDestinations();
        else if (action == QLatin1String("options"))
            emit connection->pane->torrentContextActionRequested(
                ServerPane::TorrentContextAction::TorrentOptions);
        else if (action == QLatin1String("toggle"))
        {
            const QString state = torrent.value(QStringLiteral("state")).toString();
            const bool stopped = state.startsWith(QLatin1String("stopped"))
                || state.startsWith(QLatin1String("paused"));
            connection->session->commandTorrents(
                stopped ? Remote::RemoteSession::TorrentCommand::Start
                        : Remote::RemoteSession::TorrentCommand::Stop, selected);
        }
        // "none" deliberately does nothing; the preview choice is absent.
    });

    // Properties always use the selected server's own authenticated session,
    // certificate pin and cookie jar. Inactive tabs remain independent.
    connect(connection->pane, &ServerPane::torrentDetailsRequested,
            session, &Remote::RemoteSession::requestTorrentDetails);
    connect(connection->pane, &ServerPane::filePriorityRequested,
            session, &Remote::RemoteSession::setFilePriority);
    connect(connection->pane, &ServerPane::torrentModeRequested,
            this, [this, connection](ServerPane::TorrentContextAction action, bool enabled) {
        if (!containsConnection(connection) || !connection->connected
            || activeConnection() != connection)
            return;
        const QStringList selected = selectedTorrentHashes();
        if (selected.isEmpty())
            return;
        if (action == ServerPane::TorrentContextAction::SuperSeeding)
        {
            connection->session->setSuperSeeding(selected, enabled);
            return;
        }
        const QString property = (action == ServerPane::TorrentContextAction::SequentialDownload)
            ? QStringLiteral("seq_dl") : QStringLiteral("f_l_piece_prio");
        if (action != ServerPane::TorrentContextAction::SequentialDownload
            && action != ServerPane::TorrentContextAction::FirstLastPieces)
            return;
        QStringList changed;
        const auto &torrents = connection->pane->model()->torrents();
        for (const QString &hash : selected)
        {
            const QJsonValue value = torrents.value(hash).value(property);
            if (value.isBool() && value.toBool() != enabled)
                changed.append(hash);
        }
        if (!changed.isEmpty())
            connection->session->toggleTorrentMode(
                changed, action == ServerPane::TorrentContextAction::SequentialDownload);
    });
    connect(connection->pane, &ServerPane::torrentContextActionRequested,
            this, [this, connection](ServerPane::TorrentContextAction action) {
        // Context-menu operations must never cross remote server tabs.
        // Reuse the existing toolbar QAction implementations, especially
        // the destructive Remove confirmation and delete-files checkbox.
        if (!containsConnection(connection) || !connection->connected
            || activeConnection() != connection)
            return;
        switch (action)
        {
        case ServerPane::TorrentContextAction::Start:
            m_ui->actionStart->trigger();
            break;
        case ServerPane::TorrentContextAction::Stop:
            m_ui->actionStop->trigger();
            break;
        case ServerPane::TorrentContextAction::ForceStart:
            connection->session->commandTorrents(
                Remote::RemoteSession::TorrentCommand::ForceStart, selectedTorrentHashes());
            break;
        case ServerPane::TorrentContextAction::ForceRecheck:
            connection->session->commandTorrents(
                Remote::RemoteSession::TorrentCommand::ForceRecheck, selectedTorrentHashes());
            break;
        case ServerPane::TorrentContextAction::ForceReannounce:
            connection->session->commandTorrents(
                Remote::RemoteSession::TorrentCommand::ForceReannounce, selectedTorrentHashes());
            break;
        case ServerPane::TorrentContextAction::Remove:
            m_ui->actionDelete->trigger();
            break;
        case ServerPane::TorrentContextAction::QueueTop:
            m_ui->actionTopQueuePos->trigger();
            break;
        case ServerPane::TorrentContextAction::QueueUp:
            m_ui->actionIncreaseQueuePos->trigger();
            break;
        case ServerPane::TorrentContextAction::QueueDown:
            m_ui->actionDecreaseQueuePos->trigger();
            break;
        case ServerPane::TorrentContextAction::QueueBottom:
            m_ui->actionBottomQueuePos->trigger();
            break;
        case ServerPane::TorrentContextAction::Rename:
        {
            const QStringList selected = selectedTorrentHashes();
            if (selected.size() != 1)
                break;
            const QString hash = selected.first();
            const QJsonObject record = connection->pane->model()->torrents().value(hash);
            if (record.isEmpty())
                break;
            const QString current = record.value(QStringLiteral("name")).toString();

            QInputDialog dialog(this);
            dialog.setObjectName(QStringLiteral("renameTorrentDialog"));
            dialog.setWindowTitle(tr("Rename"));
            dialog.setLabelText(tr("New name:"));
            dialog.setInputMode(QInputDialog::TextInput);
            dialog.setTextValue(current);
            // Torrent names can be much longer than QInputDialog's compact
            // default width. Keep the user's chosen size and position using
            // the same geometry tracker as other native application dialogs.
            dialog.setMinimumWidth(480);
            dialog.resize(620, qMax(dialog.sizeHint().height(), 125));
            dialog.setSizeGripEnabled(true);
            trackDialogGeometry(dialog, QStringLiteral("RenameTorrent"));
            if (dialog.exec() != QDialog::Accepted)
                break;
            const QString name = dialog.textValue().replace(
                QRegularExpression(QStringLiteral("\\r?\\n|\\r")),
                QStringLiteral(" "));
            if (name.trimmed().isEmpty() || name == current)
                break;
            // A session or selection can change while the dialog is open.
            // Never rename a torrent from another connected server.
            if (!containsConnection(connection) || !connection->connected
                || activeConnection() != connection
                || !connection->pane->model()->torrents().contains(hash)
                || selectedTorrentHashes() != QStringList{hash})
                break;
            connection->session->renameTorrent(hash, name);
            break;
        }
        case ServerPane::TorrentContextAction::ManageContent:
        {
            const QStringList hashes = selectedTorrentHashes();
            if (hashes.size() != 1)
                break;
            const QString hash = hashes.first();
            const auto selectionValid = [this, connection, hash]() {
                return containsConnection(connection) && connection->connected
                    && activeConnection() == connection
                    && selectedTorrentHashes() == QStringList{hash}
                    && connection->pane->model()->torrents().contains(hash);
            };
            if (!selectionValid())
                break;
            RemoteTorrentContentLayoutDialog dialog(
                connection->session, hash, selectionValid, this);
            trackDialogGeometry(dialog, QStringLiteral("ManageTorrentContent"));
            dialog.exec();
            break;
        }
        case ServerPane::TorrentContextAction::TorrentOptions:
        {
            const QStringList selected = selectedTorrentHashes();
            if (selected.isEmpty())
                break;
            const auto valid = [this, connection, selected]() {
                if (!containsConnection(connection) || !connection->connected
                    || activeConnection() != connection
                    || selectedTorrentHashes() != selected)
                    return false;
                const auto &all = connection->pane->model()->torrents();
                return std::all_of(selected.cbegin(), selected.cend(),
                    [&all](const QString &hash) { return all.contains(hash); });
            };
            if (!valid())
                break;
            QVector<QPair<QString, QJsonObject>> torrents;
            const auto &all = connection->pane->model()->torrents();
            for (const QString &hash : selected)
                torrents.append({hash, all.value(hash)});
            const Remote::PathMappingStore paths(*m_settings);
            RemoteTorrentOptionsDialog dialog(connection->session, torrents,
                connection->pane->categories(), paths.mappings(connection->profile.id),
                valid, this);
            trackDialogGeometry(dialog, QStringLiteral("TorrentOptions"));
            dialog.exec();
            break;
        }
        case ServerPane::TorrentContextAction::ExportTorrent:
        {
            const QStringList selected = selectedTorrentHashes();
            if (selected.isEmpty())
                break;
            const QString folder = QFileDialog::getExistingDirectory(
                this, tr("Choose folder to save exported .torrent files"));
            if (folder.isEmpty() || !containsConnection(connection)
                || !connection->connected || activeConnection() != connection
                || selectedTorrentHashes() != selected)
                break;

            QHash<QString, QString> filenames;
            for (const QString &hash : selected)
            {
                QString name = connection->pane->model()->torrents().value(hash)
                    .value(QStringLiteral("name")).toString();
                name.replace(QRegularExpression(QStringLiteral(R"([<>:"/\\|?*\x00-\x1F])")),
                             QStringLiteral("_"));
                name = name.trimmed();
                if (name.isEmpty() || name == QLatin1String(".")
                    || name == QLatin1String(".."))
                    name = hash;
                filenames.insert(hash, name);
            }
            const auto remaining = std::make_shared<QSet<QString>>(
                selected.cbegin(), selected.cend());
            auto done = std::make_shared<QMetaObject::Connection>();
            auto failed = std::make_shared<QMetaObject::Connection>();
            const auto finish = [remaining, done, failed](const QString &hash) {
                remaining->remove(hash);
                if (remaining->isEmpty())
                {
                    QObject::disconnect(*done);
                    QObject::disconnect(*failed);
                }
            };
            *done = connect(connection->session, &Remote::RemoteSession::torrentExportReceived,
                this, [this, folder, filenames, remaining, finish](
                    const QString &hash, const QByteArray &data) {
                    if (!remaining->contains(hash))
                        return;
                    const QDir dir(folder);
                    const QString base = filenames.value(hash, hash);
                    QString name = base + QStringLiteral(".torrent");
                    int number = 0;
                    while (QFileInfo::exists(dir.filePath(name)))
                        name = QStringLiteral("%1 (%2).torrent").arg(base).arg(++number);
                    QSaveFile file(dir.filePath(name));
                    if (!file.open(QIODevice::WriteOnly)
                        || file.write(data) != data.size() || !file.commit())
                        QMessageBox::warning(this, tr("Export .torrent"),
                            tr("Could not save exported torrent: %1").arg(name));
                    finish(hash);
                });
            *failed = connect(connection->session, &Remote::RemoteSession::torrentExportFailed,
                this, [this, remaining, finish](const QString &hash, const QString &reason) {
                    if (!remaining->contains(hash))
                        return;
                    QMessageBox::warning(this, tr("Export .torrent"), reason);
                    finish(hash);
                });
            for (const QString &hash : selected)
                connection->session->requestTorrentExport(hash);
            break;
        }
        case ServerPane::TorrentContextAction::SuperSeeding:
        case ServerPane::TorrentContextAction::SequentialDownload:
        case ServerPane::TorrentContextAction::FirstLastPieces:
            // Completed in subsequent commits on this branch.
            break;
        case ServerPane::TorrentContextAction::EditTrackers:
        {
            const QStringList hashes = selectedTorrentHashes();
            if (hashes.isEmpty() || !containsConnection(connection)
                || !connection->connected || activeConnection() != connection)
                break;
            // Match qBittorrent's native Edit trackers dialog for one or
            // multiple torrents. Only the backing tracker data comes from
            // the authenticated remote server instead of local libtorrent.
            const auto selectionValid = [this, connection, hashes]() {
                if (!containsConnection(connection) || !connection->connected
                    || activeConnection() != connection
                    || selectedTorrentHashes() != hashes)
                    return false;
                const auto &all = connection->pane->model()->torrents();
                return std::all_of(hashes.cbegin(), hashes.cend(),
                    [&all](const QString &hash) { return all.contains(hash); });
            };
            if (!selectionValid())
                break;
            // The private flag comes from the authenticated server's sync
            // state, never from tracker URL heuristics. If the server has
            // not yet established a torrent's privacy, protect it too.
            QSet<QString> protectedHashes;
            for (const QString &hash : hashes)
            {
                const QJsonObject torrent = connection->pane->model()->torrents().value(hash);
                const QJsonValue privacy = torrent.value(QStringLiteral("private"));
                const QJsonValue legacy = torrent.value(QStringLiteral("is_private"));
                if (privacy.isBool())
                {
                    if (privacy.toBool())
                        protectedHashes.insert(hash);
                }
                else if (legacy.isBool())
                {
                    if (legacy.toBool())
                        protectedHashes.insert(hash);
                }
                else
                    protectedHashes.insert(hash); // unknown: fail closed
            }
            RemoteTrackerEntriesDialog dialog(connection->session, hashes,
                                              selectionValid, this, protectedHashes);
            trackDialogGeometry(dialog, QStringLiteral("EditTorrentTrackers"));
            dialog.exec();
            break;
        }
        case ServerPane::TorrentContextAction::OpenDestinationFolder:
            m_ui->actionOpenDestinationFolder->trigger();
            break;
        case ServerPane::TorrentContextAction::SetLocation:
            setSelectedTorrentLocation();
            break;
        }
    });

    // Category and tag menus operate only on the active server selection.
    // Never cache selected hashes across a modal dialog or use another tab.
    const auto selectedForContext = [this, connection]() -> QStringList {
        if (!containsConnection(connection) || !connection->connected
            || activeConnection() != connection)
            return {};
        return selectedTorrentHashes();
    };
    connect(connection->pane, &ServerPane::torrentCategoryRequested, this,
        [session, selectedForContext](const QString &category) {
            const QStringList hashes = selectedForContext();
            if (!hashes.isEmpty())
                session->setTorrentCategory(hashes, category);
        });
    const auto groupValid = [this, connection]() {
        return containsConnection(connection) && connection->connected && activeConnection() == connection
            && connection->pane->hasFullSync();
    };
    const auto categoryEditor = [this, connection, session, groupValid](QString name, bool edit, bool assign) {
        if (!groupValid()) return;
        const auto &definitions = connection->pane->categoryDefinitions();
        QJsonObject definition = definitions.value(name).toObject();
        if (edit && !definition.value(QStringLiteral("savePath")).isString())
        {
            QMessageBox::information(this, tr("Category unavailable"), tr("The server has not supplied this category's save path."));
            return;
        }
        const QStringList initialSelection = assign ? selectedTorrentHashes() : QStringList();
        if (assign && initialSelection.isEmpty()) return;
        const auto names = connection->pane->categories();
        QSet<QString> existing(names.cbegin(), names.cend());
        QJsonObject capabilities;
        for (auto it = definitions.constBegin(); it != definitions.constEnd(); ++it)
        {
            const auto record = it.value().toObject();
            for (auto field = record.constBegin(); field != record.constEnd(); ++field)
                capabilities.insert(field.key(), field.value());
        }
        if (edit) capabilities = definition;
        else definition = {};
        Remote::PathMappingStore store(*m_settings);
        RemoteCategoryDialog dialog(name, edit, definition, capabilities, existing,
                                    store.mappings(connection->profile.id), this);
        trackDialogGeometry(dialog, QStringLiteral("TorrentCategory"));
        if (dialog.exec() != QDialog::Accepted || !groupValid()) return;
        const auto fields = dialog.fields();
        const auto &torrents = connection->pane->model()->torrents();
        if (assign && (selectedTorrentHashes() != initialSelection
            || std::any_of(initialSelection.cbegin(), initialSelection.cend(),
                [&torrents](const QString &hash) { return !torrents.contains(hash); })))
            return;
        if (edit && (fields.contains(QStringLiteral("savePath")) || fields.contains(QStringLiteral("downloadPathEnabled"))))
        {
            bool risk = false;
            for (const auto &torrent : torrents)
            {
                const auto cat = torrent.value(QStringLiteral("category"));
                const auto mode = torrent.value(QStringLiteral("auto_tmm"));
                if ((!cat.isString() || cat.toString() == name || cat.toString().startsWith(name + QLatin1Char('/')))
                    && (!mode.isBool() || mode.toBool()))
                {
                    risk = true;
                    break;
                }
            }
            if (risk && QMessageBox::question(this, tr("Change category save path"),
                tr("Automatically managed torrents may be relocated or switched to Manual Mode on the server. Continue?"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
                return;
        }
        if (groupValid())
            session->saveCategory(dialog.categoryName(), fields, edit, initialSelection);
    };
    connect(connection->pane, &ServerPane::torrentNewCategoryRequested, this,
        [this, categoryEditor]() { categoryEditor(tr("New Category"), false, true); });

    connect(connection->pane->sidebar(), &FilterSidebar::groupActionRequested, this,
        [this, connection, session, groupValid, categoryEditor](FilterSidebar::Group group, const QString &key,
                                                              FilterSidebar::GroupAction action) {
        using Group = FilterSidebar::Group;
        using Action = FilterSidebar::GroupAction;
        using Command = Remote::RemoteSession::TorrentCommand;
        if (!groupValid()) return;
        const bool category = group == Group::Categories;
        const bool tagGroup = group == Group::Tags;
        const bool tracker = group == Group::Trackers;
        const bool named = category ? key.startsWith(QLatin1String("category:"))
            : tagGroup ? key.startsWith(QLatin1String("tag:"))
            : tracker && key.startsWith(QLatin1String("tracker:"));
        const QString name = named ? key.mid(category ? 9 : tagGroup ? 4 : 8) : QString();
        if (category && (action == Action::Add || action == Action::AddSubcategory || action == Action::Edit))
        {
            categoryEditor(action == Action::Edit ? name : action == Action::AddSubcategory
                ? name + QLatin1Char('/') + tr("New Category") : tr("New Category"),
                action == Action::Edit, false);
            return;
        }
        if (tagGroup && action == Action::Add)
        {
            bool ok = false;
            const QString tag = QInputDialog::getText(this, tr("Add tag"), tr("Tag name:"),
                                                      QLineEdit::Normal, {}, &ok).trimmed();
            if (!ok || tag.isEmpty() || !groupValid()) return;
            if (tag.contains(QLatin1Char(',')) || tag.contains(QLatin1Char('\n')) || tag.contains(QLatin1Char('\r')))
            {
                QMessageBox::warning(this, tr("Invalid tag name"), tr("Tag names cannot contain commas or line breaks."));
                return;
            }
            if (connection->pane->tagDefinitions().contains(tag))
            {
                QMessageBox::warning(this, tr("Tag exists"), tr("Tag name already exists."));
                return;
            }
            session->manageTagDefinitions({tag}, true);
            return;
        }
        const auto &torrents = connection->pane->model()->torrents();
        if ((category || tagGroup) && (action == Action::Remove || action == Action::RemoveUnused))
        {
            QStringList definitions = category ? connection->pane->categories() : connection->pane->tagDefinitions().values();
            QStringList remove;
            if (action == Action::Remove)
            {
                if (named && definitions.contains(name)) remove.append(name);
            }
            else
            {
                QSet<QString> used;
                for (const auto &torrent : torrents)
                {
                    const auto membership = torrent.value(category ? QStringLiteral("category") : QStringLiteral("tags"));
                    if (!membership.isString())
                    {
                        QMessageBox::information(this, tr("Membership unavailable"), tr("The server has not supplied all torrent memberships."));
                        return;
                    }
                    if (category)
                    {
                        QString cat = membership.toString();
                        while (!cat.isEmpty())
                        {
                            used.insert(cat);
                            const auto slash = cat.lastIndexOf(QLatin1Char('/'));
                            if (slash < 0) break;
                            cat.truncate(slash);
                        }
                    }
                    else for (const auto &tag : membership.toString().split(QLatin1Char(','), Qt::SkipEmptyParts))
                        used.insert(tag.trimmed());
                }
                for (const auto &entry : definitions)
                    if (!used.contains(entry)) remove.append(entry);
            }
            if (remove.isEmpty()) return;
            if (category && action == Action::Remove
                && QMessageBox::question(this, tr("Remove category"),
                    tr("Remove this category and its subcategories? Existing torrents may change category or location on the server."),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
                return;
            if (!groupValid()) return;
            if (category) session->removeCategories(remove);
            else for (qsizetype i = 0; i < remove.size(); i += 128)
                session->manageTagDefinitions(remove.mid(i, 128), false);
            return;
        }
        if (tracker && action == Action::Remove && named)
        {
            if (name == QLatin1String("(unknown tracker)")) return;
            if (QMessageBox::question(this, tr("Removal confirmation"),
                tr("Are you sure you want to remove tracker \"%1\" from all torrents on %2?")
                    .arg(name, connection->profile.name),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes
                || !groupValid())
                return;
            QHash<QString, QStringList> removals;
            const auto &trackers = connection->pane->trackerDefinitions();
            for (auto it = trackers.constBegin(); it != trackers.constEnd(); ++it)
            {
                if (QUrl(it.key()).host().compare(name, Qt::CaseInsensitive) != 0) continue;
                for (const auto &hash : it.value())
                    if (connection->pane->model()->torrents().contains(hash))
                        removals[hash].append(it.key());
            }
            if (removals.isEmpty())
            {
                QMessageBox::information(this, tr("Tracker unavailable"), tr("The server has not supplied the tracker memberships needed for safe removal."));
                return;
            }
            for (auto it = removals.constBegin(); it != removals.constEnd(); ++it)
                for (qsizetype i = 0; i < it.value().size(); i += 128)
                    session->removeTorrentTrackers(it.key(), it.value().mid(i, 128));
            return;
        }
        // Upstream's bulk actions target all visible rows, not selected rows.
        QStringList hashes;
        auto *proxy = connection->pane->proxy();
        for (int row = 0; row < proxy->rowCount(); ++row)
        {
            const auto hash = connection->pane->model()->hashForRow(proxy->mapToSource(proxy->index(row, 0)).row());
            if (!hash.isEmpty()) hashes.append(hash);
        }
        if (hashes.isEmpty()) return;
        switch (action)
        {
        case Action::Start: session->commandTorrents(Command::Start, hashes); break;
        case Action::ForceStart: session->commandTorrents(Command::ForceStart, hashes); break;
        case Action::Stop: session->commandTorrents(Command::Stop, hashes); break;
        case Action::RemoveTorrents: removeRemoteTorrents(connection, hashes); break;
        default: break;
        }
    });
    connect(connection->pane, &ServerPane::torrentTagRequested, this,
        [session, selectedForContext](const QString &tag, bool add) {
            const QStringList hashes = selectedForContext();
            if (!hashes.isEmpty())
                session->changeTorrentTags(hashes, {tag}, add);
        });
    connect(connection->pane, &ServerPane::torrentNewTagsRequested, this,
        [this, session, selectedForContext]() {
            QInputDialog dialog(this);
            dialog.setObjectName(QStringLiteral("newTorrentTagsDialog"));
            dialog.setWindowTitle(tr("Add tags"));
            dialog.setLabelText(tr("Tag names (comma separated):"));
            dialog.setInputMode(QInputDialog::TextInput);
            dialog.setMinimumWidth(440);
            trackDialogGeometry(dialog, QStringLiteral("NewTorrentTags"));
            if (dialog.exec() != QDialog::Accepted)
                return;
            QStringList tags;
            for (QString value : dialog.textValue().split(QLatin1Char(','), Qt::SkipEmptyParts))
            {
                value = value.trimmed();
                if (!value.isEmpty() && !tags.contains(value))
                    tags.append(value);
            }
            const QStringList hashes = selectedForContext();
            if (!tags.isEmpty() && !hashes.isEmpty())
                session->createAndAddTorrentTags(hashes, tags);
        });
    connect(connection->pane, &ServerPane::torrentClearTagsRequested, this,
        [this, session, selectedForContext]() {
            const QStringList hashes = selectedForContext();
            if (hashes.isEmpty())
                return;
            const auto decision = QMessageBox::question(this, tr("Remove all tags"),
                tr("Remove all tags from %1 selected torrent(s) on this server? "
                   "Global tag definitions will be kept.").arg(hashes.size()),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
            if (decision == QMessageBox::Yes && selectedForContext() == hashes)
                session->changeTorrentTags(hashes, {}, false);
        });
    connect(connection->pane, &ServerPane::torrentAutoManagementRequested, this,
        [this, session, selectedForContext, connection](bool enabled) {
            const QStringList hashes = selectedForContext();
            if (hashes.isEmpty())
                return;
            if (enabled)
            {
                // Enabling Auto TMM can cause the *remote server* to move
                // existing data under its own Saving Management rules.
                // Require an explicit opt-in before sending anything.
                QMessageBox warning(this);
                warning.setObjectName(QStringLiteral(
                    "enableAutomaticTorrentManagementWarning"));
                warning.setIcon(QMessageBox::Warning);
                warning.setWindowTitle(tr("Enable Automatic Torrent Management?"));
                warning.setText(tr("qBittorrent may move downloaded files for "
                    "%1 selected torrent(s) on server %2.")
                    .arg(hashes.size()).arg(connection->profile.name));
                warning.setInformativeText(tr(
                    "The remote server controls where files are stored, "
                    "including category-based paths and its Saving Management "
                    "settings. Enabling this option may relocate existing data."));
                QPushButton *enable = warning.addButton(
                    tr("Enable"), QMessageBox::AcceptRole);
                QPushButton *cancel = warning.addButton(QMessageBox::Cancel);
                warning.setDefaultButton(cancel);
                warning.exec();
                // A connection or selection may have changed during a modal
                // dialog; never apply a stale command to another server.
                if (warning.clickedButton() != enable
                    || selectedForContext() != hashes)
                    return;
            }
            session->setTorrentAutoManagement(hashes, enabled);
        });
    connect(session, &Remote::RemoteSession::torrentDetailsReceived,
            connection->pane, &ServerPane::displayTorrentDetails);
    connect(session, &Remote::RemoteSession::torrentDetailsFailed,
            connection->pane, &ServerPane::displayTorrentDetailsError);

    connect(connection->pane->table()->selectionModel(),
            &QItemSelectionModel::selectionChanged, this,
            [this, connection]() {
                if (activeConnection() == connection)
                    updateTorrentActionState();
            });

    // Listen to the real Qt source and filtered proxy models, not to a
    // separate count derived from individual sidebar labels. Qt emits these
    // signals for live sync additions/removals and combined filter changes.
    const auto updateCount = [this, connection]() {
        if (activeConnection() == connection)
            updateTorrentCount();
    };
    for (auto *model : {static_cast<QAbstractItemModel *>(connection->pane->model()),
                        static_cast<QAbstractItemModel *>(connection->pane->proxy())})
    {
        connect(model, &QAbstractItemModel::rowsInserted, this, updateCount);
        connect(model, &QAbstractItemModel::rowsRemoved, this, updateCount);
        connect(model, &QAbstractItemModel::modelReset, this, updateCount);
        connect(model, &QAbstractItemModel::layoutChanged, this, updateCount);
    }

    m_views->addTab(connection->pane, upstreamIcon("folder-remote"), profile.name);
    m_views->setCurrentWidget(connection->pane);
    updateActiveUi();

    // Inspect the actual server certificate before permitting TLS errors.
    // This callback runs synchronously from QNetworkReply::sslErrors.
    Remote::ProfileStore trustStore(*m_settings);
    session->setCertificateTrust(
        trustStore.trustedCertificateFingerprint(profile),
        [this, profile](const Remote::CertificateChallenge &challenge) {
            const bool changed = !challenge.previousFingerprint.isEmpty();
            QMessageBox prompt(this);
            prompt.setObjectName(QStringLiteral("certificateTrustDialog"));
            prompt.setWindowTitle(changed
                ? tr("Server Certificate Changed")
                : tr("Trust Server Certificate"));
            prompt.setIcon(QMessageBox::Warning);
            prompt.setTextFormat(Qt::PlainText);
            prompt.setText(changed
                ? tr("The saved server presents a different HTTPS certificate.")
                : tr("This server's HTTPS certificate is not trusted by the system."));

            const QSslCertificate &certificate = challenge.certificate;
            const QString subject = certificate.subjectInfo(
                QSslCertificate::CommonName).join(QStringLiteral(", "));
            const QString issuer = certificate.issuerInfo(
                QSslCertificate::CommonName).join(QStringLiteral(", "));
            QString description = tr(
                "Server: %1\n"
                "Subject: %2\n"
                "Issuer: %3\n"
                "Valid from: %4\n"
                "Valid until: %5\n\n"
                "Presented SHA-256 fingerprint:\n%6")
                .arg(challenge.endpoint,
                     subject,
                     issuer,
                     certificate.effectiveDate().toString(Qt::ISODate),
                     certificate.expiryDate().toString(Qt::ISODate),
                     challenge.presentedFingerprint.toUpper());

            if (changed)
                description += tr("\n\nPreviously trusted SHA-256 fingerprint:\n%1")
                    .arg(challenge.previousFingerprint.toUpper());

            if (!challenge.verificationErrors.isEmpty())
                description += tr("\n\nVerification issues:\n%1")
                    .arg(challenge.verificationErrors.join(QLatin1Char('\n')));

            description += tr(
                "\n\nOnly approve if you've verified this fingerprint "
                "against the certificate installed on your server.");
            prompt.setInformativeText(description);
            prompt.setDetailedText(certificate.toText());
            auto *rejectButton = prompt.addButton(tr("Reject"), QMessageBox::RejectRole);
            auto *trustButton = prompt.addButton(
                tr("Trust This Certificate"), QMessageBox::AcceptRole);
            prompt.setDefaultButton(qobject_cast<QPushButton *>(rejectButton));
            prompt.exec();
            if (prompt.clickedButton() != trustButton)
                return false;

            Remote::ProfileStore store(*m_settings);
            if (!store.trustCertificate(profile, challenge.presentedFingerprint))
            {
                QMessageBox::warning(this, tr("Cannot Save Certificate Trust"),
                    tr("Certificate trust could not be saved. Connection remains blocked."));
                return false;
            }
            return true;
        });

    connect(session, &Remote::RemoteSession::connected, this,
        [this, connection, profile, password](const QString &version,
                                               const QString &apiVersion) {
            if (!containsConnection(connection))
                return;
            connection->connected = true;
            connection->version = version;
            connection->apiVersion = apiVersion;
            if (activeConnection() == connection)
                updateActiveUi();

            if (profile.rememberPassword && !password.isEmpty() && m_vault->available())
            {
                QPointer<MainWindow> safeWindow(this);
                m_vault->write(profile, password, [safeWindow](const QString &error) {
                    if (safeWindow && !error.isEmpty())
                        QMessageBox::warning(safeWindow,
                            QObject::tr("Cannot Remember Password"),
                            QObject::tr("Connected successfully, but the password was not saved: %1")
                                .arg(error));
                });
            }
        });
    connect(session, &Remote::RemoteSession::mainDataUpdated, this,
        [this, connection](const QJsonObject &payload) {
            if (!containsConnection(connection))
                return;
            connection->pane->applySync(payload);
            const QJsonObject state = payload.value(QStringLiteral("server_state")).toObject();
            const QJsonValue alt = state.value(QStringLiteral("use_alt_speed_limits"));
            if (payload.value(QStringLiteral("full_update")).toBool()
                || state.contains(QStringLiteral("use_alt_speed_limits")))
            {
                connection->altSpeedModeKnown = alt.isBool();
                connection->altSpeedMode = alt.toBool();
            }
            if (activeConnection() == connection)
            {
                updateTransferRates(connection->pane);
                updateServerFreeSpace();
                updateTorrentCount();
                updateTorrentActionState();
                updateTrayActionState();
            }
        });
    connect(session, &Remote::RemoteSession::operationSucceeded, this,
        [this, connection](const QString &description) {
            // HTTP request acceptance is not completion. Do not leave stale
            // "awaiting synchronization" banners over the server/version label.
            // A future activity log can show each server operation.
            if (containsConnection(connection)
                && description == tr("Set file priorities"))
            {
                QPointer<ServerPane> pane(connection->pane);
                QTimer::singleShot(500, this, [pane]() {
                    if (pane)
                        pane->refreshSelectedDetails();
                });
            }
        });
    connect(session, &Remote::RemoteSession::operationFailed, this,
        [this, connection](const QString &description, const QString &reason) {
            if (!containsConnection(connection))
                return;
            QMessageBox::warning(this, tr("Remote torrent operation failed"),
                tr("%1 on %2 failed: %3")
                    .arg(description, connection->profile.name, reason));
            // Restore server-authoritative priorities when a checkbox action
            // was rejected. Never leave optimistic selection state misleading.
            if (description == tr("Set file priorities"))
                connection->pane->refreshSelectedDetails();
        });
    connect(session, &Remote::RemoteSession::failed, this,
        [this, connection](const QString &reason) {
            if (!containsConnection(connection))
                return;
            const bool wasActive = activeConnection() == connection;
            const QString name = connection->profile.name;
            removeConnection(connection);
            if (wasActive)
                m_ui->statusBar->showMessage(
                    tr("Connection to %1 failed: %2").arg(name, reason), 8000);
        });

    if (profile.rememberPassword && password.isEmpty() && m_vault->available())
    {
        QPointer<MainWindow> safeWindow(this);
        std::weak_ptr<SessionContext> weakConnection(connection);
        m_vault->read(profile,
            [safeWindow, weakConnection, profile](const QString &savedPassword,
                                                   bool found, const QString &error) {
                const auto live = weakConnection.lock();
                if (!safeWindow || !live || !safeWindow->containsConnection(live))
                    return;
                if (!error.isEmpty())
                {
                    const bool wasActive = safeWindow->activeConnection() == live;
                    const QString name = live->profile.name;
                    safeWindow->removeConnection(live);
                    if (wasActive)
                        safeWindow->m_ui->statusBar->showMessage(
                            QObject::tr("Connection to %1 failed: Could not read saved password: %2")
                                .arg(name, error), 8000);
                    return;
                }
                live->session->connectToServer(profile,
                                                found ? savedPassword : QString());
            });
    }
    else
        session->connectToServer(profile, password);
}

void MainWindow::disconnectCurrentServer()
{
    if (auto connection = activeConnection())
        removeConnection(connection);
}

void MainWindow::applyFontPointSize(int points)
{
    const int ceiling = qMax(m_baseFontPointSize, maxFontSize);
    m_fontPointSize = qBound(m_baseFontPointSize, points, ceiling);

    QFont font = originalSystemFont();
    if (m_fontPointSize != m_baseFontPointSize)
        font.setPointSize(m_fontPointSize);
    // At the minimum, use the exact original font, including its pixel-size
    // specification if that is how the Qt theme defined it.

    // QApplication's font is inherited by Qt widgets (including menus,
    // dialogs and dynamically constructed per-server panes). Keep the
    // original font family, weight, style and desktop theme untouched.
    QApplication::setFont(font);
    setFont(font);

    if (m_settings)
    {
        if (m_fontPointSize == m_baseFontPointSize)
            m_settings->remove(QLatin1String(fontSettingsKey));
        else
            m_settings->setValue(QLatin1String(fontSettingsKey), m_fontPointSize);
    }

    // The sidebar's fixed-height lists and the upstream cached name-column
    // delegate require layout invalidation after a global font change.
    if (m_views)
        for (auto *pane : findChildren<ServerPane *>())
            pane->refreshFontMetrics();

    updateStatusBarFieldWidths();
    updateFontActionStates();
}

void MainWindow::updateFontActionStates()
{
    if (m_increaseFontAction)
        m_increaseFontAction->setEnabled(
            m_fontPointSize < qMax(m_baseFontPointSize, maxFontSize));
    if (m_decreaseFontAction)
        m_decreaseFontAction->setEnabled(m_fontPointSize > m_baseFontPointSize);
    if (m_resetFontAction)
        m_resetFontAction->setEnabled(m_fontPointSize != m_baseFontPointSize);
}

void MainWindow::trackDialogGeometry(QDialog &dialog, const QString &name)
{
    const QString key = QStringLiteral("Layout/Dialogs/") + name;
    const QByteArray saved = m_settings->value(key).toByteArray();
    if (!saved.isEmpty())
        dialog.restoreGeometry(saved);

    // Native qBittorrent centers Preferences over the parent window.
    // Migrate the old offset Preferences placement once, then preserve
    // the position the user deliberately chooses on subsequent launches.
    const QString centeredKey = key + QStringLiteral("/CenterVersion");
    const bool migratePreferences = name == QLatin1String("Preferences")
        && m_settings->value(centeredKey, 0).toInt() < 1;
    if (saved.isEmpty() || migratePreferences)
    {
        const QPoint parentCenter = mapToGlobal(rect().center());
        QPoint topLeft = parentCenter - QPoint(dialog.width() / 2, dialog.height() / 2);
        const QScreen *screen = QGuiApplication::screenAt(parentCenter);
        if (!screen)
            screen = QGuiApplication::primaryScreen();
        if (screen)
        {
            const QRect available = screen->availableGeometry();
            const int maxX = qMax(available.left(), available.right() + 1 - dialog.width());
            const int maxY = qMax(available.top(), available.bottom() + 1 - dialog.height());
            topLeft.setX(qBound(available.left(), topLeft.x(), maxX));
            topLeft.setY(qBound(available.top(), topLeft.y(), maxY));
        }
        dialog.move(topLeft);
    }

    // Only geometry is stored, never credentials or dialog contents.
    connect(&dialog, &QDialog::finished, &dialog,
            [this, &dialog, key, centeredKey, name](int) {
        m_settings->setValue(key, dialog.saveGeometry());
        if (name == QLatin1String("Preferences"))
            m_settings->setValue(centeredKey, 1);
    });
}

void MainWindow::restoreWindowLayout()
{
    m_preferredServerId =
        m_settings->value(QStringLiteral("Layout/ActiveServer")).toString();
    const QByteArray geometry =
        m_settings->value(QStringLiteral("Layout/MainWindow/Geometry")).toByteArray();
    if (!geometry.isEmpty())
        restoreGeometry(geometry);
    const QByteArray state =
        m_settings->value(QStringLiteral("Layout/MainWindow/State")).toByteArray();
    if (!state.isEmpty())
        restoreState(state, 1);

    if (m_settings->value(QStringLiteral("Layout/MainWindow/Maximized"), false).toBool()
        && !isMaximized())
        setWindowState(windowState() | Qt::WindowMaximized);
    // QMainWindow::restoreState() also restores toolbar visibility. Always
    // explicitly apply our saved View menu choices after it: setChecked()
    // alone emits nothing when an action is already checked, even if the
    // restored widget is unexpectedly hidden.
    // Previous versions recorded false while the entire window was hidden
    // to tray. Recover that poisoned persisted state once, not on every run.
    // After migration, intentionally hidden toolbars still stay hidden.
    if (m_settings->value(QStringLiteral("Layout/ViewVisibilityVersion"), 0).toInt() < 2)
    {
        m_settings->setValue(QStringLiteral("Layout/ToolbarVisible"), true);
        m_settings->setValue(QStringLiteral("Layout/StatusbarVisible"), true);
        m_settings->setValue(QStringLiteral("Layout/ViewVisibilityVersion"), 2);
    }
    const bool toolbarVisible = m_settings->value(
        QStringLiteral("Layout/ToolbarVisible"), true).toBool();
    const bool statusbarVisible = m_settings->value(
        QStringLiteral("Layout/StatusbarVisible"), true).toBool();
    m_ui->actionTopToolBar->setChecked(toolbarVisible);
    m_ui->toolBar->setVisible(toolbarVisible);
    m_ui->actionShowStatusbar->setChecked(statusbarVisible);
    m_ui->statusBar->setVisible(statusbarVisible);
    m_ui->actionShowFiltersSidebar->setChecked(
        m_settings->value(QStringLiteral("Layout/SidebarVisible"), true).toBool());

    // Explicit Behavior startup state takes precedence over the prior
    // window's last minimized state; showForStartup() applies it once.
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!m_forceExit && m_trayIcon && m_trayIcon->isVisible()
        && m_settings->value(
            QStringLiteral("Preferences/Behavior/CloseToTray"), false).toBool())
    {
        m_wasMaximizedBeforeTray = isMaximized();
        event->ignore();
        hide();
        return;
    }
    if (m_settings->value(
            QStringLiteral("Preferences/Behavior/ConfirmExit"), false).toBool())
    {
        const auto response = QMessageBox::question(this,
            tr("Quit qBittorrent Remote?"),
            tr("Close qBittorrent Remote? Remote server torrents will continue running."),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (response != QMessageBox::Yes)
        {
            m_forceExit = false;
            event->ignore();
            return;
        }
    }
    for (const auto &connection : m_connections)
        if (connection->pane)
            connection->pane->saveLayout(*m_settings, connection->profile.id);
    if (m_emptyPane)
        m_emptyPane->saveLayout(*m_settings, QStringLiteral("empty-pane"));

    m_settings->setValue(QStringLiteral("Layout/MainWindow/Geometry"), saveGeometry());
    m_settings->setValue(QStringLiteral("Layout/MainWindow/State"), saveState(1));
    m_settings->setValue(QStringLiteral("Layout/MainWindow/Minimized"), isMinimized());
    m_settings->setValue(QStringLiteral("Layout/MainWindow/Maximized"), isMaximized());
    // isVisible() returns false for a visible toolbar/statusbar whenever
    // their parent window is hidden to the system tray. Save the user's
    // explicit View menu choices instead, not ancestor visibility.
    m_settings->setValue(QStringLiteral("Layout/ToolbarVisible"),
                         m_ui->actionTopToolBar->isChecked());
    m_settings->setValue(QStringLiteral("Layout/StatusbarVisible"),
                         m_ui->actionShowStatusbar->isChecked());
    m_settings->setValue(QStringLiteral("Layout/SidebarVisible"),
                         m_ui->actionShowFiltersSidebar->isChecked());
    if (!m_preferredServerId.isEmpty())
        m_settings->setValue(QStringLiteral("Layout/ActiveServer"), m_preferredServerId);
    m_settings->sync();
    QMainWindow::closeEvent(event);
}
