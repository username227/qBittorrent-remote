// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/MainWindow.h"
#include "gui/InterfaceLanguage.h"
#include "app/SingleInstance.h"
#include "RemoteBuildVersion.h"

#include <QApplication>
#include <QMessageBox>
#include <QStandardPaths>
#include <QSettings>
#include <QStyleHints>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("qBittorrent Remote"));
    app.setApplicationVersion(QStringLiteral(QBT_REMOTE_DISPLAY_VERSION));
#ifdef Q_OS_LINUX
    // Match the installed desktop entry, including the Wayland app_id.
    app.setDesktopFileName(QStringLiteral("qbittorrent-remote"));
#endif
    app.setOrganizationName(QStringLiteral("qBittorrent Remote"));

    QTranslator interfaceTranslator;
    if (InterfaceLanguage::load(interfaceTranslator, QSettings()))
        app.installTranslator(&interfaceTranslator);

    QStringList arguments;
    QString error;
    if (!SingleInstance::normalizeArguments(app.arguments().mid(1), arguments, error))
    {
        QMessageBox::warning(nullptr, QObject::tr("Cannot open torrent"), error);
        return 1;
    }
    SingleInstance instance(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation));
    MainWindow *mainWindow = nullptr;
    QList<QStringList> earlyRequests;
    const auto role = instance.start(arguments, [&](const QStringList &args) {
        if (mainWindow) mainWindow->enqueueExternalOpen(args);
        else earlyRequests.append(args);
    }, error);
    if (role == SingleInstance::Result::Forwarded) return 0;
    if (role == SingleInstance::Result::Error)
    {
        QMessageBox::warning(nullptr, QObject::tr("qBittorrent Remote"), error);
        return 1;
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    const auto scheme = QSettings().value(QStringLiteral("Appearance/ColorScheme"),
        QStringLiteral("system")).toString();
    if (scheme == QLatin1String("light"))
        app.styleHints()->setColorScheme(Qt::ColorScheme::Light);
    else if (scheme == QLatin1String("dark"))
        app.styleHints()->setColorScheme(Qt::ColorScheme::Dark);
#endif
    MainWindow window;
    mainWindow = &window;
    window.showForStartup();
    if (!arguments.isEmpty()) window.enqueueExternalOpen(arguments);
    for (const auto &request : earlyRequests) window.enqueueExternalOpen(request);
    earlyRequests.clear();
    const int result = app.exec();
    mainWindow = nullptr;
    return result;
}
