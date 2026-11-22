// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QMainWindow>
#include <QSettings>
#include <QJsonObject>
#include "api/CredentialVault.h"

#include <memory>

class FilterSidebar;
class QAction;
class QLabel;
class QLineEdit;
class QComboBox;
class QSortFilterProxyModel;
class TorrentModel;
namespace Remote
{
class RemoteSession;
struct ServerProfile;
}

namespace Ui
{
class MainWindow;
}

// Qt Designer's upstream qBittorrent main-window chrome is intentionally
// kept separate from our remote-specific central content and future sessions.
class MainWindow final : public QMainWindow
{
public:
    explicit MainWindow(QWidget *parent = nullptr, QSettings *settings = nullptr,
                        Remote::CredentialVault *vault = nullptr);
    ~MainWindow() override;

private:
    void setupMainArea();
    void setupActions();
    void connectToProfile(const Remote::ServerProfile &profile, const QString &password);
    void disconnectCurrentServer();
    void applyServerUpdate(const QJsonObject &payload);

    std::unique_ptr<Ui::MainWindow> m_ui;
    FilterSidebar *m_filterSidebar = nullptr;
    TorrentModel *m_torrentModel = nullptr;
    QSortFilterProxyModel *m_torrentProxy = nullptr;
    QLabel *m_ratesLabel = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    QComboBox *m_searchBy = nullptr;
    QJsonObject m_serverState;
    QSettings m_defaultSettings;
    QSettings *m_settings = nullptr;
    Remote::CredentialVault m_defaultVault;
    Remote::CredentialVault *m_vault = nullptr;
    Remote::RemoteSession *m_session = nullptr;
    QAction *m_connectAction = nullptr;
    QAction *m_disconnectAction = nullptr;
    QString m_currentServerName;
};
