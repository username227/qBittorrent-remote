// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/MainWindow.h"
#include "gui/ServerPane.h"
#include "gui/TorrentModel.h"
#include "gui/FilterSidebar.h"
#include "gui/ConnectionDialog.h"
#include "api/RemoteSession.h"
#include "gui/upstream/hidabletabwidget.h"

#include "ui_mainwindow.h"

#include <QDialog>
#include <QCloseEvent>
#include <QApplication>
#include <QFont>
#include <QFontInfo>
#include <QGuiApplication>
#include <QScreen>
#include <QKeySequence>
#include <QMenu>
#include <QCheckBox>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
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
#include <QSslCertificate>
#include <QSizePolicy>
#include <QStatusBar>
#include <QToolBar>
#include <QWidget>
#include <QSignalBlocker>
#include <QTimer>
#include <QTabWidget>
#include <algorithm>

namespace
{

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
    setWindowIcon(upstreamIcon("qbittorrent-tray"));
    resize(1280, 800);

    setupMainArea();
    setupActions();
    updateFontActionStates();

    m_ui->statusBar->showMessage(tr("Not connected to a server"));
    m_ratesLabel = new QLabel(tr("D: —    U: —"), m_ui->statusBar);
    m_ui->statusBar->addPermanentWidget(m_ratesLabel);
    restoreWindowLayout();

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
        m_views->addTab(m_emptyPane, upstreamIcon("folder-remote"), tr("Transfers"));
        m_views->setCurrentWidget(m_emptyPane);
    }
}

void MainWindow::setupMainArea()
{
    m_views = new HidableTabWidget(m_ui->centralWidget);
    m_views->setObjectName(QStringLiteral("serverTabs"));
    m_ui->centralWidgetLayout->addWidget(m_views);
    showEmptyPane();
    if (m_emptyPane)
        m_emptyPane->restoreLayout(*m_settings, QStringLiteral("empty-pane"));

    connect(m_views, &QTabWidget::currentChanged, this,
            [this](int) { updateActiveUi(); });

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
        m_ui->actionOptions, m_ui->actionPauseSession,
        m_ui->actionResumeSession
    })
        action->setEnabled(false);

    m_ui->actionLock->setVisible(false);

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
    connect(m_ui->actionExit, &QAction::triggered, this, &QWidget::close);
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
        const auto server = activeConnection();
        const QStringList hashes = selectedTorrentHashes();
        if (!server || !server->connected || hashes.isEmpty())
            return;

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
    });

    connect(m_ui->actionDownloadFromURL, &QAction::triggered, this, [this]() {
        const auto server = activeConnection();
        if (!server || !server->connected)
            return;
        QInputDialog dialog(this);
        dialog.setObjectName(QStringLiteral("addTorrentUrlDialog"));
        dialog.setWindowTitle(tr("Add torrent link to %1").arg(server->profile.name));
        dialog.setLabelText(tr("Paste magnet or HTTP(S) links, one per line.\n"
                               "The remote server will use its own default download location."));
        dialog.setInputMode(QInputDialog::TextInput);
        dialog.setOption(QInputDialog::UsePlainTextEditForTextInput);
        trackDialogGeometry(dialog, QStringLiteral("AddTorrentURL"));
        if (dialog.exec() != QDialog::Accepted)
            return;
        const QString urls = dialog.textValue();
        if (!urls.trimmed().isEmpty() && containsConnection(server) && server->connected)
            server->session->addTorrentUrls(urls);
    });

    connect(m_ui->actionOpen, &QAction::triggered, this, [this]() {
        const auto server = activeConnection();
        if (!server || !server->connected)
            return;
        QFileDialog dialog(this, tr("Add .torrent to %1").arg(server->profile.name));
        dialog.setObjectName(QStringLiteral("addTorrentFileDialog"));
        dialog.setFileMode(QFileDialog::ExistingFile);
        dialog.setNameFilters({tr("Torrent files (*.torrent)"), tr("All files (*)")});
        trackDialogGeometry(dialog, QStringLiteral("AddTorrentFile"));
        if (dialog.exec() != QDialog::Accepted)
            return;
        const QString filename = dialog.selectedFiles().value(0);
        if (filename.isEmpty() || !containsConnection(server) || !server->connected)
            return;
        QFile file(filename);
        if (!file.open(QIODevice::ReadOnly))
        {
            QMessageBox::warning(this, tr("Cannot open torrent file"),
                                 tr("Cannot read the selected local .torrent file."));
            return;
        }
        if (file.size() > 32 * 1024 * 1024)
        {
            QMessageBox::warning(this, tr("Torrent file too large"),
                                 tr("The selected .torrent metadata exceeds 32 MiB."));
            return;
        }
        server->session->addTorrentFile(QFileInfo(filename).fileName(), file.readAll());
    });
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

ServerPane *MainWindow::activePane() const
{
    return m_views ? static_cast<ServerPane *>(m_views->currentWidget()) : nullptr;
}

void MainWindow::updateActiveUi()
{
    if (!m_searchEdit || !m_searchBy || !m_disconnectAction || !m_ratesLabel)
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
    m_ratesLabel->setText(connection && ready && pane
                          ? pane->rateText() : tr("D: —    U: —"));

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
    connection->pane->restoreLayout(*m_settings, profile.id);
    connection->session = new Remote::RemoteSession(this);
    auto *session = connection->session;
    m_connections.append(connection);

    // Properties always use the selected server's own authenticated session,
    // certificate pin and cookie jar. Inactive tabs remain independent.
    connect(connection->pane, &ServerPane::torrentDetailsRequested,
            session, &Remote::RemoteSession::requestTorrentDetails);
    connect(connection->pane, &ServerPane::filePriorityRequested,
            session, &Remote::RemoteSession::setFilePriority);
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
            if (activeConnection() == connection)
            {
                m_ratesLabel->setText(connection->pane->rateText());
                updateTorrentActionState();
            }
        });
    connect(session, &Remote::RemoteSession::operationSucceeded, this,
        [this, connection](const QString &description) {
            if (containsConnection(connection) && activeConnection() == connection)
                m_ui->statusBar->showMessage(
                    tr("%1: %2 request accepted; awaiting server synchronization.")
                        .arg(connection->profile.name, description));
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
                    tr("Connection to %1 failed: %2").arg(name, reason));
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
                                .arg(name, error));
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
    // Store geometry only; never the contents of dialogs or credentials.
    connect(&dialog, &QDialog::finished, &dialog, [this, &dialog, key](int) {
        m_settings->setValue(key, dialog.saveGeometry());
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
    m_ui->actionTopToolBar->setChecked(
        m_settings->value(QStringLiteral("Layout/ToolbarVisible"), true).toBool());
    m_ui->actionShowStatusbar->setChecked(
        m_settings->value(QStringLiteral("Layout/StatusbarVisible"), true).toBool());
    m_ui->actionShowFiltersSidebar->setChecked(
        m_settings->value(QStringLiteral("Layout/SidebarVisible"), true).toBool());

    if (m_settings->value(QStringLiteral("Layout/MainWindow/Minimized"), false).toBool())
        QTimer::singleShot(0, this, [this]() { showMinimized(); });
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    for (const auto &connection : m_connections)
        if (connection->pane)
            connection->pane->saveLayout(*m_settings, connection->profile.id);
    if (m_emptyPane)
        m_emptyPane->saveLayout(*m_settings, QStringLiteral("empty-pane"));

    m_settings->setValue(QStringLiteral("Layout/MainWindow/Geometry"), saveGeometry());
    m_settings->setValue(QStringLiteral("Layout/MainWindow/State"), saveState(1));
    m_settings->setValue(QStringLiteral("Layout/MainWindow/Minimized"), isMinimized());
    m_settings->setValue(QStringLiteral("Layout/MainWindow/Maximized"), isMaximized());
    m_settings->setValue(QStringLiteral("Layout/ToolbarVisible"), m_ui->toolBar->isVisible());
    m_settings->setValue(QStringLiteral("Layout/StatusbarVisible"), m_ui->statusBar->isVisible());
    m_settings->setValue(QStringLiteral("Layout/SidebarVisible"),
                         m_ui->actionShowFiltersSidebar->isChecked());
    if (!m_preferredServerId.isEmpty())
        m_settings->setValue(QStringLiteral("Layout/ActiveServer"), m_preferredServerId);
    m_settings->sync();
    QMainWindow::closeEvent(event);
}
