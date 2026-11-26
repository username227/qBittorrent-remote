// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QLocalServer>
#include <QLockFile>
#include <QStringList>
#include <functional>
#include <memory>

// One desktop instance per user and application data directory. The lock
// protects startup and stale socket cleanup; the local socket transports
// only launch arguments, never Web API credentials or session cookies.
class SingleInstance final : public QObject
{
public:
    enum class Result { Primary, Forwarded, Error };
    using Receiver = std::function<void(const QStringList &)>;
    explicit SingleInstance(const QString &directory);
    Result start(const QStringList &arguments, Receiver receiver, QString &error);
    static bool normalizeArguments(const QStringList &arguments, QStringList &normalized, QString &error);
private:
    void acceptConnections();
    bool forward(const QByteArray &message, QString &error);
    QString m_directory;
    QString m_name;
    std::unique_ptr<QLockFile> m_lock;
    QLocalServer m_server;
    Receiver m_receiver;
};
