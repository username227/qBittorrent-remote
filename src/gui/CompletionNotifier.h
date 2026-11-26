// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QIcon>
#include <QStringList>

class QSoundEffect;
class QSystemTrayIcon;
class QTimer;

// Local desktop delivery only. No network commands or server preferences.
class CompletionNotifier final : public QObject
{
    Q_OBJECT
public:
    explicit CompletionNotifier(QSystemTrayIcon *tray, QObject *parent = nullptr);
    void deliver(const QString &server, const QStringList &names, bool notify, bool sound);
    void deliverAdded(const QString &server, const QStringList &names);
    void setTimeout(int milliseconds) { m_timeoutMs = qMax(-1, milliseconds); }

signals:
    // Delivery requests, not acknowledgement that the OS displayed or played them.
    void notificationRequested(const QString &title, const QString &body);
    void soundRequested();

private:
    void showDesktopNotification(const QString &title, const QString &body);
    void showTrayNotification(const QString &title, const QString &body);
    void playCompletionSound();
    void playPendingSound();

    QSystemTrayIcon *m_tray;
    QSystemTrayIcon *m_temporaryTray = nullptr;
    QTimer *m_hideTrayTimer = nullptr;
    QSoundEffect *m_sound = nullptr;
    bool m_soundPending = false;
    int m_timeoutMs = -1;
};
