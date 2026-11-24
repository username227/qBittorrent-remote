// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/MainWindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("qBittorrent Remote"));
#ifdef Q_OS_LINUX
    // Match the installed desktop entry, including the Wayland app_id.
    app.setDesktopFileName(QStringLiteral("qbittorrent-remote"));
#endif
    app.setOrganizationName(QStringLiteral("qBittorrent Remote"));

    MainWindow window;
    window.showForStartup();

    return app.exec();
}
