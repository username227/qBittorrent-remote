// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QMainWindow>
#include <QSettings>

#include <memory>

class FilterSidebar;
class QAction;
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
    explicit MainWindow(QWidget *parent = nullptr, QSettings *settings = nullptr);
    ~MainWindow() override;

private:
    void setupMainArea();
    void setupActions();
    void connectToProfile(const Remote::ServerProfile &profile, const QString &password);
    void disconnectCurrentServer();

    std::unique_ptr<Ui::MainWindow> m_ui;
    FilterSidebar *m_filterSidebar = nullptr;
    QSettings m_defaultSettings;
    QSettings *m_settings = nullptr;
    Remote::RemoteSession *m_session = nullptr;
    QAction *m_connectAction = nullptr;
    QAction *m_disconnectAction = nullptr;
    QString m_currentServerName;
};
