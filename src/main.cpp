// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/MainWindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("qBittorrent Remote"));
    app.setOrganizationName(QStringLiteral("qBittorrent Remote"));

    MainWindow window;
    window.show();

    return app.exec();
}
