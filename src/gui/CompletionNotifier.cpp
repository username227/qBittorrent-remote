// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/CompletionNotifier.h"

#include <QApplication>
#include <QSoundEffect>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QUrl>
#ifdef QBT_REMOTE_WITH_DBUS
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QVariantMap>
#endif

CompletionNotifier::CompletionNotifier(QSystemTrayIcon *tray, QObject *parent)
    : QObject(parent), m_tray(tray)
{
    setObjectName(QStringLiteral("completionNotifier"));
    Q_INIT_RESOURCE(completion_sounds);
    connect(this, &CompletionNotifier::notificationRequested,
            this, &CompletionNotifier::showDesktopNotification);
    connect(this, &CompletionNotifier::soundRequested,
            this, &CompletionNotifier::playCompletionSound);
}

void CompletionNotifier::deliver(const QString &server, const QStringList &names,
                                 bool notify, bool sound)
{
    if (names.isEmpty())
        return;
    if (notify)
    {
        const QString title = names.size() == 1
            ? tr("Torrent download completed — %1").arg(server)
            : tr("%n torrent(s) completed — %1", nullptr, names.size()).arg(server);
        QString body = names.mid(0, 3).join(QLatin1Char('\n'));
        if (names.size() > 3)
            body += tr("\n…and %n more", nullptr, names.size() - 3);
        emit notificationRequested(title, body);
    }
    if (sound)
        emit soundRequested();
}

void CompletionNotifier::showDesktopNotification(const QString &title, const QString &body)
{
#ifdef QBT_REMOTE_WITH_DBUS
    // Native freedesktop notifications do not require a visible tray icon.
    // Suppress notification-server sounds: the separate checkbox controls
    // the application's own bundled sound.
    auto bus = QDBusConnection::sessionBus();
    if (bus.isConnected())
    {
        QDBusMessage message = QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.Notifications"),
            QStringLiteral("/org/freedesktop/Notifications"),
            QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("Notify"));
        const QVariantMap hints{{QStringLiteral("suppress-sound"), true}};
        message << QStringLiteral("qBittorrent Remote") << uint(0)
                << QStringLiteral("dialog-information") << title << body.toHtmlEscaped()
                << QStringList{} << hints << int(-1);
        auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(message, 5000), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, title, body](QDBusPendingCallWatcher *finished) {
                const QDBusPendingReply<uint> reply = *finished;
                finished->deleteLater();
                if (reply.isError())
                    showTrayNotification(title, body);
            });
        return;
    }
#endif
    showTrayNotification(title, body);
}

void CompletionNotifier::showTrayNotification(const QString &title, const QString &body)
{
    if (!QSystemTrayIcon::isSystemTrayAvailable() || !QSystemTrayIcon::supportsMessages())
        return;
    if (m_tray && m_tray->isVisible())
    {
        m_tray->showMessage(title, body, QSystemTrayIcon::Information, 5000);
        return;
    }
    // Windows/other tray-based notification implementations require a
    // registered tray icon. Use a transient one without changing the user's
    // persistent tray preference or the application's minimize/close behavior.
    if (!m_temporaryTray)
    {
        m_temporaryTray = new QSystemTrayIcon(QApplication::windowIcon(), this);
        m_temporaryTray->setToolTip(tr("qBittorrent Remote"));
        m_hideTrayTimer = new QTimer(this);
        m_hideTrayTimer->setSingleShot(true);
        connect(m_hideTrayTimer, &QTimer::timeout, m_temporaryTray, &QSystemTrayIcon::hide);
    }
    m_temporaryTray->show();
    m_temporaryTray->showMessage(title, body, QSystemTrayIcon::Information, 5000);
    m_hideTrayTimer->start(10000);
}

void CompletionNotifier::playCompletionSound()
{
    m_soundPending = true;
    if (!m_sound)
    {
        m_sound = new QSoundEffect(this);
        m_sound->setVolume(0.45f);
        connect(m_sound, &QSoundEffect::statusChanged,
                this, &CompletionNotifier::playPendingSound);
        m_sound->setSource(QUrl(QStringLiteral("qrc:/sounds/completion.wav")));
    }
    playPendingSound();
}

void CompletionNotifier::playPendingSound()
{
    if (!m_soundPending || !m_sound)
        return;
    if (m_sound->status() == QSoundEffect::Ready)
    {
        m_soundPending = false;
        // A burst of completions should not restart a chime mid-note.
        if (!m_sound->isPlaying())
            m_sound->play();
    }
    else if (m_sound->status() == QSoundEffect::Error)
    {
        m_soundPending = false;
        QApplication::beep();
    }
}
