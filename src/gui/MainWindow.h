// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/CredentialVault.h"
#include "api/ServerProfile.h"

#include <QList>
#include <QMainWindow>
#include <QSettings>
#include <memory>
#include <functional>

class QAction;
class QCloseEvent;
class QDialog;
class QEvent;
class QMimeData;
class QSystemTrayIcon;
class QMenu;
class QLabel;
class QLineEdit;
class QComboBox;
class QPushButton;
class QTimer;
class HidableTabWidget;
class ServerPane;
class CompletionNotifier;

namespace Remote { class RemoteSession; }
namespace Ui { class MainWindow; }

class MainWindow final : public QMainWindow
{
public:
    explicit MainWindow(QWidget *parent = nullptr, QSettings *settings = nullptr,
                        Remote::CredentialVault *vault = nullptr);
    ~MainWindow() override;
    int fontPointSize() const noexcept { return m_fontPointSize; }
    void showForStartup();
    void enqueueExternalOpen(const QStringList &arguments);

private:
    struct SessionContext;

    void setupMainArea();
    void setupActions();
    void applyFontPointSize(int points);
    void updateFontActionStates();
    void restoreWindowLayout();
    void trackDialogGeometry(QDialog &dialog, const QString &name);
    void showEmptyPane();
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void enableTorrentDrops(QWidget *widget);
    void setupTray();
    void observeTorrentCompletions(SessionContext &connection, const QJsonObject &payload);
    void updateTrayActionState();
    void refreshTrayDisconnectMenu();
    void updateTraySettings();
    void toggleWindowFromTray();
    void quitCompletely();
    void connectToProfile(const Remote::ServerProfile &profile, const QString &password,
                          const Remote::ServerProfile &previousWebUIProfile = {}, bool preserveCredential = false);
    void disconnectCurrentServer();
    bool showConnectionDialog(const QString &profileId = {});
    void processExternalOpen();
    QString disconnectedStatus(const SessionContext &connection) const;
    void updateServerTab(const std::shared_ptr<SessionContext> &connection);
    void disconnectConnection(const std::shared_ptr<SessionContext> &connection, const QString &reason);
    void removeConnection(const std::shared_ptr<SessionContext> &connection, bool showPlaceholder = true);
    bool containsConnection(const std::shared_ptr<SessionContext> &connection) const;
    std::shared_ptr<SessionContext> activeConnection() const;
    std::shared_ptr<SessionContext> connectedById(const QString &id) const;
    QList<Remote::ServerProfile> connectedServerProfiles() const;
    ServerPane *activePane() const;
    void updateActiveUi();
    void updateTorrentCount();
    void updateTorrentActionState();
    void applyLocalBehaviorPreferences();
    void updateServerFreeSpace();
    void updateStatusBarFieldWidths();
    void updateStatusBarIndicators();
    void updateTransferRates(const ServerPane *pane);
    void refreshWindowTitle();
    QStringList selectedTorrentHashes() const;
    void openSelectedTorrentDestinations();
    void removeRemoteTorrents(const std::shared_ptr<SessionContext> &server, const QStringList &hashes);
    void setSelectedTorrentLocation();
    void refreshContentPathMappings();
    void openPreferences(const std::shared_ptr<SessionContext> &server);

    std::unique_ptr<Ui::MainWindow> m_ui;
    QSettings m_defaultSettings;
    QSettings *m_settings = nullptr;
    Remote::CredentialVault m_defaultVault;
    Remote::CredentialVault *m_vault = nullptr;

    HidableTabWidget *m_views = nullptr;
    ServerPane *m_emptyPane = nullptr;
    QList<std::shared_ptr<SessionContext>> m_connections;
    QLabel *m_torrentCountLabel = nullptr;
    QLabel *m_connectionStatusLabel = nullptr;
    QPushButton *m_alternativeSpeedButton = nullptr;
    QLabel *m_downloadRateLabel = nullptr;
    QLabel *m_uploadRateLabel = nullptr;
    QLabel *m_freeSpaceSeparator = nullptr;
    QLabel *m_serverFreeSpaceLabel = nullptr;
    QSystemTrayIcon *m_trayIcon = nullptr;
    CompletionNotifier *m_completionNotifier = nullptr;
    QMenu *m_trayMenu = nullptr;
    QMenu *m_trayDisconnectMenu = nullptr;
    QAction *m_trayToggleAction = nullptr;
    bool m_forceExit = false;
    bool m_wasMaximizedBeforeTray = false;
    QLineEdit *m_searchEdit = nullptr;
    QComboBox *m_searchBy = nullptr;
    QList<QAction *> m_queueToolbarActions;
    QAction *m_queueToolbarSeparator = nullptr;
    QAction *m_rssAction = nullptr;
    QAction *m_connectAction = nullptr;
    QAction *m_disconnectAction = nullptr;
    QAction *m_reconnectAction = nullptr;
    QAction *m_closeTabAction = nullptr;
    QAction *m_increaseFontAction = nullptr;
    QAction *m_decreaseFontAction = nullptr;
    QAction *m_resetFontAction = nullptr;
    int m_baseFontPointSize = 10;
    int m_fontPointSize = 10;
    QString m_preferredServerId;
    bool m_restoringStartup = false;
    bool m_startupComplete = false;
    bool m_externalOpenBusy = false;
    bool m_externalConnectionPrompted = false;
    bool m_forceExternalAdditionDialog = false;
    QList<QStringList> m_externalOpenQueue;
    QTimer *m_externalOpenTimer = nullptr;
    std::function<void(const QString &)> m_addLinks;
    std::function<void(const QStringList &)> m_addTorrentFiles;
};
