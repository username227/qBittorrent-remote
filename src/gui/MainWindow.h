// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/CredentialVault.h"
#include "api/ServerProfile.h"

#include <QList>
#include <QMainWindow>
#include <QSettings>
#include <memory>

class QAction;
class QCloseEvent;
class QDialog;
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
    QStringList selectedTorrentHashes() const;
    void openSelectedTorrentDestinations();
    void setSelectedTorrentLocation();

    std::unique_ptr<Ui::MainWindow> m_ui;
    QSettings m_defaultSettings;
    QSettings *m_settings = nullptr;
    Remote::CredentialVault m_defaultVault;
    Remote::CredentialVault *m_vault = nullptr;

    HidableTabWidget *m_views = nullptr;
    ServerPane *m_emptyPane = nullptr;
    QList<std::shared_ptr<SessionContext>> m_connections;
    QLabel *m_torrentCountLabel = nullptr;
    QLabel *m_ratesLabel = nullptr;
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
};
