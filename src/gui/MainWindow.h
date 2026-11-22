// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QMainWindow>

class MainWindow final : public QMainWindow
{
public:
    explicit MainWindow(QWidget *parent = nullptr);

private:
    void setupMenus();
    void setupMainArea();
};
