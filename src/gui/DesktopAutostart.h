// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Per-user desktop-session autostart. No daemon is started on boot and no
// remote qBittorrent server preference is modified.
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QString>

namespace DesktopAutostart
{
inline bool available()
{
#if defined(Q_OS_WIN) || (defined(Q_OS_UNIX) && !defined(Q_OS_MACOS))
    return true;
#else
    return false;
#endif
}

#if defined(Q_OS_WIN)
inline QSettings registry()
{
    return QSettings(QStringLiteral(
        "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
        QSettings::NativeFormat);
}
inline QString registryValue() { return QStringLiteral("qBittorrent Remote"); }
#else
inline QString entryPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
        + QStringLiteral("/autostart/qbittorrent-remote.desktop");
}
inline bool ours(const QByteArray &bytes)
{
    return bytes.contains("X-qBittorrentRemote-Managed=true");
}
#endif

inline bool isEnabled()
{
#if defined(Q_OS_WIN)
    QSettings s = registry();
    return !s.value(registryValue()).toString().isEmpty();
#elif defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    QFile file(entryPath());
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray bytes = file.readAll();
    return ours(bytes) && !bytes.contains("Hidden=true");
#else
    return false;
#endif
}

inline bool setEnabled(bool enabled, QString *error)
{
    if (!available())
    {
        if (error) *error = QStringLiteral("Desktop autostart is unavailable on this platform.");
        return !enabled;
    }
#if defined(Q_OS_WIN)
    QSettings s = registry();
    if (enabled)
    {
        QString executable = QCoreApplication::applicationFilePath();
        executable.replace(QLatin1Char('"'), QString());
        s.setValue(registryValue(), QLatin1Char('"') + executable + QLatin1Char('"'));
    }
    else
    {
        s.remove(registryValue());
    }
    s.sync();
    if (s.status() != QSettings::NoError)
    {
        if (error) *error = QStringLiteral("Windows could not update your user startup registry.");
        return false;
    }
    return true;
#elif defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    const QString path = entryPath();
    QFile prior(path);
    if (prior.exists())
    {
        if (!prior.open(QIODevice::ReadOnly))
        {
            if (error) *error = prior.errorString();
            return false;
        }
        const QByteArray bytes = prior.readAll();
        prior.close();
        if (!ours(bytes))
        {
            if (error) *error = QStringLiteral(
                "An existing, unmanaged autostart entry uses the same filename. "
                "It was left untouched.");
            return false;
        }
    }
    if (!enabled)
    {
        if (!QFile::exists(path) || QFile::remove(path))
            return true;
        if (error) *error = QStringLiteral("Could not remove the desktop autostart entry.");
        return false;
    }
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
    {
        if (error) *error = QStringLiteral("Could not create the desktop autostart directory.");
        return false;
    }
    // Desktop Entry Exec quoting: escape special characters inside double
    // quotes. QCoreApplication::applicationFilePath() is an absolute path.
    QString executable = QCoreApplication::applicationFilePath();
    executable.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    executable.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    executable.replace(QLatin1Char('$'), QStringLiteral("\\$"));
    executable.replace(QLatin1Char('`'), QStringLiteral("\\`"));
    QByteArray content =
        "[Desktop Entry]\nType=Application\nVersion=1.0\n"
        "Name=qBittorrent Remote\n"
        "Comment=Native remote qBittorrent desktop client\n"
        "Terminal=false\nStartupNotify=false\n"
        "X-GNOME-Autostart-enabled=true\n"
        "X-qBittorrentRemote-Managed=true\n"
        "Exec=\"" + executable.toUtf8() + "\"\n";
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly))
    {
        if (error) *error = out.errorString();
        return false;
    }
    if (out.write(content) != content.size() || !out.commit())
    {
        if (error) *error = out.errorString();
        return false;
    }
    return true;
#else
    return false;
#endif
}
} // namespace DesktopAutostart
