// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QMainWindow>

#include <memory>

class FilterSidebar;

namespace Ui
{
class MainWindow;
}

// Qt Designer's upstream qBittorrent main-window chrome is intentionally
// kept separate from our remote-specific central content and future sessions.
class MainWindow final : public QMainWindow
{
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private:
    void setupMainArea();
    void setupActions();

    std::unique_ptr<Ui::MainWindow> m_ui;
    FilterSidebar *m_filterSidebar = nullptr;
};
