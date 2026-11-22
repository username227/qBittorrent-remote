// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/MainWindow.h"
#include "gui/FilterSidebar.h"
#include "gui/ConnectionDialog.h"
#include "api/RemoteSession.h"
#include "gui/upstream/hidabletabwidget.h"
#include "gui/upstream/proptabbar.h"

#include "ui_mainwindow.h"

#include <QAbstractItemView>
#include <QDialog>
#include <QAction>
#include <QComboBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QPointer>
#include <QSslCertificate>
#include <QSizePolicy>
#include <QSplitter>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QStackedWidget>
#include <QTableView>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWidget>

namespace
{

QIcon upstreamIcon(const char *name)
{
    return QIcon(QStringLiteral(":/icons/") + QString::fromLatin1(name) + QStringLiteral(".svg"));
}

} // namespace

MainWindow::MainWindow(QWidget *parent, QSettings *settings,
                       Remote::CredentialVault *vault)
    : QMainWindow(parent)
    , m_ui(std::make_unique<Ui::MainWindow>())
    , m_settings(settings ? settings : &m_defaultSettings)
    , m_vault(vault ? vault : &m_defaultVault)
{
    // Required when SVG resources live in a linked static library.
    Q_INIT_RESOURCE(qbittorrent_icons);

    // Actual, unchanged upstream Qt Designer layout: menus, actions,
    // original toolbar and status bar, not a visual imitation.
    m_ui->setupUi(this);
    setWindowTitle(tr("qBittorrent Remote"));
    setWindowIcon(upstreamIcon("qbittorrent-tray"));
    resize(1280, 800);

    setupMainArea();
    setupActions();

    m_ui->statusBar->showMessage(tr("Not connected to a server"));
    auto *rates = new QLabel(tr("D: —    U: —"), m_ui->statusBar);
    m_ui->statusBar->addPermanentWidget(rates);
}

MainWindow::~MainWindow() = default;

void MainWindow::setupMainArea()
{
    // No server tab bar yet. Later, connected instances will own dynamic
    // views; one connection will still display without a tab strip.
    auto *outer = new QSplitter(Qt::Horizontal, m_ui->centralWidget);
    outer->setChildrenCollapsible(false);

    // Actual qBittorrent-style filter sections with upstream icons.
    m_filterSidebar = new FilterSidebar(outer);
    m_filterSidebar->setMinimumWidth(185);
    m_filterSidebar->setMaximumWidth(320);

    auto *vertical = new QSplitter(Qt::Vertical, outer);
    vertical->setChildrenCollapsible(false);

    auto *torrentTable = new QTableView(vertical);
    torrentTable->setObjectName(QStringLiteral("torrentList"));
    auto *emptyModel = new QStandardItemModel(torrentTable);
    // Expose all remote-compatible column labels; show default columns.
    emptyModel->setHorizontalHeaderLabels({
        tr("Name"), tr("Size"), tr("Progress"), tr("Status"), tr("Seeds"),
        tr("Peers"), tr("Down Speed"), tr("Up Speed"), tr("ETA"), tr("Ratio"),
        tr("Category"), tr("Tags"), tr("Availability"), tr("Added On"), tr("Completed On"),
        tr("Created On"), tr("Tracker"), tr("Down Limit"), tr("Up Limit"), tr("Downloaded"),
        tr("Uploaded"), tr("Session Downloaded"), tr("Session Uploaded"), tr("Remaining"), tr("Time Active"),
        tr("Save Path"), tr("Incomplete Save Path"), tr("Completed"), tr("Ratio Limit"), tr("Last Seen Complete"),
        tr("Last Activity"), tr("Total Size"), tr("Info Hash v1"), tr("Info Hash v2"), tr("Reannounce In"),
        tr("Private"), tr("Popularity"), tr("Queue Position")
    });
    torrentTable->setModel(emptyModel);
    torrentTable->setAlternatingRowColors(true);
    torrentTable->setShowGrid(false);
    torrentTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    torrentTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    torrentTable->setSortingEnabled(true);
    torrentTable->horizontalHeader()->setStretchLastSection(false);
    torrentTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    torrentTable->verticalHeader()->hide();

    for (int column = 13; column < emptyModel->columnCount(); ++column)
        torrentTable->hideColumn(column);

    auto *header = torrentTable->horizontalHeader();
    header->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(header, &QWidget::customContextMenuRequested, torrentTable,
        [torrentTable, header, emptyModel](const QPoint &position)
        {
            QMenu menu(header);
            for (int column = 0; column < emptyModel->columnCount(); ++column)
            {
                QAction *action = menu.addAction(
                    emptyModel->headerData(column, Qt::Horizontal).toString());
                action->setCheckable(true);
                action->setChecked(!torrentTable->isColumnHidden(column));
                if (column == 0)
                    action->setEnabled(false);
                QObject::connect(action, &QAction::toggled, torrentTable,
                    [torrentTable, column](bool checked)
                    {
                        torrentTable->setColumnHidden(column, !checked);
                    });
            }
            menu.exec(header->mapToGlobal(position));
        });

    auto *details = new QWidget(vertical);
    auto *detailsLayout = new QVBoxLayout(details);
    detailsLayout->setContentsMargins(0, 0, 0, 0);
    detailsLayout->setSpacing(0);

    // Upstream qBittorrent properties-tab bar with right-aligned Speed.
    auto *tabBar = new PropTabBar;
    detailsLayout->addLayout(tabBar);

    auto *propertyPages = new QStackedWidget(details);
    for (int index = 0; index < 6; ++index)
        propertyPages->addWidget(new QWidget(propertyPages));
    detailsLayout->addWidget(propertyPages);

    connect(tabBar, &PropTabBar::tabChanged,
        propertyPages, &QStackedWidget::setCurrentIndex);
    connect(tabBar, &PropTabBar::visibilityToggled,
        propertyPages, &QWidget::setVisible);
    tabBar->setCurrentIndex(0);

    vertical->addWidget(torrentTable);
    vertical->addWidget(details);
    vertical->setStretchFactor(0, 3);
    vertical->setStretchFactor(1, 0);
    vertical->setSizes({650, 75});

    outer->addWidget(m_filterSidebar);
    outer->addWidget(vertical);
    outer->setStretchFactor(0, 0);
    outer->setStretchFactor(1, 1);

    // qBittorrent's HidableTabWidget retains the pane border when the
    // only view (Transfers) has its tab label hidden. The frame cleanly
    // separates transfer controls from the status bar below.
    auto *views = new HidableTabWidget(m_ui->centralWidget);
    views->addTab(outer, upstreamIcon("folder-remote"), tr("Transfers"));
    m_ui->centralWidgetLayout->addWidget(views);

    // Original qBittorrent toolbar's right-aligned torrent filter.
    auto *filterPanel = new QWidget(m_ui->toolBar);
    auto *filterLayout = new QHBoxLayout(filterPanel);
    filterLayout->setContentsMargins(0, 0, 0, 0);
    filterLayout->addStretch();

    auto *searchEdit = new QLineEdit(filterPanel);
    searchEdit->setPlaceholderText(tr("Filter torrents..."));
    searchEdit->setFixedWidth(200);
    searchEdit->setEnabled(false);
    filterLayout->addWidget(searchEdit);

    filterLayout->addWidget(new QLabel(tr("Filter by:"), filterPanel));
    auto *filterBy = new QComboBox(filterPanel);
    filterBy->addItems({tr("Name"), tr("Save Path"), tr("Info Hash")});
    filterBy->setEnabled(false);
    filterLayout->addWidget(filterBy);

    // The original lock action is the final toolbar action.
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
    connect(m_ui->actionShowFiltersSidebar, &QAction::toggled,
        m_filterSidebar, &QWidget::setVisible);

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
        QMessageBox::about(this, tr("About qBittorrent Remote"),
            tr("A native Qt desktop client for remote qBittorrent servers.\n"
               "Server connections are supported; torrent synchronization is coming next."));
    });
}

void MainWindow::connectToProfile(const Remote::ServerProfile &profile,
                                  const QString &password)
{
    // One visible connection during the foundation milestone. Each session
    // has its own network manager; server-tab management comes in a later PR.
    if (m_session)
        m_session->disconnectFromServer();

    m_session = new Remote::RemoteSession(this);
    auto *session = m_session;

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

    m_currentServerName = profile.name;
    m_disconnectAction->setEnabled(true);
    m_ui->statusBar->showMessage(tr("Connecting to %1...").arg(profile.name));

    connect(session, &Remote::RemoteSession::connected, this,
        [this, session, profile, password](
            const QString &version, const QString &apiVersion) {
            if (m_session != session)
                return;
            m_ui->statusBar->showMessage(
                tr("Connected to %1 — qBittorrent %2 (Web API %3).")
                    .arg(m_currentServerName, version, apiVersion));
            setWindowTitle(tr("qBittorrent Remote — %1").arg(m_currentServerName));

            if (!m_vault->available())
                return;

            // Opting out and profile removal erase credentials in the
            // profile editor. Never launch a second asynchronous deletion
            // on every successful passwordless or manual login.
            if (profile.rememberPassword && !password.isEmpty())
            {
                // Only store a newly supplied password *after* successful
                // server authentication. A blank password needs no secret.
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
    connect(session, &Remote::RemoteSession::failed, this,
        [this, session](const QString &reason) {
            if (m_session != session)
                return;
            m_ui->statusBar->showMessage(
                tr("Connection to %1 failed: %2").arg(m_currentServerName, reason));
            m_disconnectAction->setEnabled(false);
        });
    if (profile.rememberPassword && password.isEmpty() && m_vault->available())
    {
        // An empty input means 'use remembered password if one exists',
        // otherwise send an empty password for servers permitting it.
        QPointer<MainWindow> safeWindow(this);
        QPointer<Remote::RemoteSession> safeSession(session);
        m_vault->read(profile,
            [safeWindow, safeSession, profile](const QString &savedPassword,
                                               bool found, const QString &error) {
                // A keyring operation can outlive a closed window or a
                // disconnected session; never use stale QObject pointers.
                if (!safeWindow || !safeSession
                    || safeWindow->m_session != safeSession)
                    return;
                if (!error.isEmpty())
                {
                    safeWindow->m_disconnectAction->setEnabled(false);
                    safeWindow->m_ui->statusBar->showMessage(
                        QObject::tr("Could not read saved password: %1").arg(error));
                    return;
                }
                safeSession->connectToServer(profile,
                                             found ? savedPassword : QString());
            });
    }
    else
        session->connectToServer(profile, password);
}

void MainWindow::disconnectCurrentServer()
{
    if (!m_session)
        return;
    m_session->disconnectFromServer();
    m_session = nullptr;
    m_currentServerName.clear();
    m_disconnectAction->setEnabled(false);
    setWindowTitle(tr("qBittorrent Remote"));
    m_ui->statusBar->showMessage(tr("Not connected to a server"));
}
