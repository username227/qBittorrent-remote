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
class HidableTabWidget;
class ServerPane;

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
    void updateTrayActionState();
    void refreshTrayDisconnectMenu();
    void updateTraySettings();
    void toggleWindowFromTray();
    void quitCompletely();
    void connectToProfile(const Remote::ServerProfile &profile, const QString &password);
    void disconnectCurrentServer();
    void removeConnection(const std::shared_ptr<SessionContext> &connection);
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
    void updateTransferRates(const ServerPane *pane);
    QStringList selectedTorrentHashes() const;
    void openSelectedTorrentDestinations();
    void removeRemoteTorrents(const std::shared_ptr<SessionContext> &server, const QStringList &hashes);
    void setSelectedTorrentLocation();
    void refreshContentPathMappings();

    std::unique_ptr<Ui::MainWindow> m_ui;
    QSettings m_defaultSettings;
    QSettings *m_settings = nullptr;
    Remote::CredentialVault m_defaultVault;
    Remote::CredentialVault *m_vault = nullptr;

    HidableTabWidget *m_views = nullptr;
    ServerPane *m_emptyPane = nullptr;
    QList<std::shared_ptr<SessionContext>> m_connections;
    QLabel *m_torrentCountLabel = nullptr;
    QLabel *m_downloadRateLabel = nullptr;
    QLabel *m_uploadRateLabel = nullptr;
    QLabel *m_freeSpaceSeparator = nullptr;
    QLabel *m_serverFreeSpaceLabel = nullptr;
    QSystemTrayIcon *m_trayIcon = nullptr;
    QMenu *m_trayMenu = nullptr;
    QMenu *m_trayDisconnectMenu = nullptr;
    QAction *m_trayToggleAction = nullptr;
    bool m_forceExit = false;
    bool m_wasMaximizedBeforeTray = false;
    QLineEdit *m_searchEdit = nullptr;
    QComboBox *m_searchBy = nullptr;
    QAction *m_connectAction = nullptr;
    QAction *m_disconnectAction = nullptr;
    QAction *m_increaseFontAction = nullptr;
    QAction *m_decreaseFontAction = nullptr;
    QAction *m_resetFontAction = nullptr;
    int m_baseFontPointSize = 10;
    int m_fontPointSize = 10;
    QString m_preferredServerId;
    bool m_restoringStartup = false;
    std::function<void(const QString &)> m_addLinks;
    std::function<void(const QStringList &)> m_addTorrentFiles;
};
