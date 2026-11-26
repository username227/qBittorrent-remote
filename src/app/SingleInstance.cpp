// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/SingleInstance.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QThread>
#include <QTimer>
#include <QUrl>

namespace
{
constexpr qsizetype MaxMessage = 1024 * 1024;
constexpr int MaxArguments = 256;
constexpr int Timeout = 5000;

bool validArgument(const QString &arg)
{
    if (arg.isEmpty() || arg.contains(QChar::Null)) return false;
    if (arg.startsWith(QStringLiteral("magnet:"), Qt::CaseInsensitive))
        return QUrl(arg, QUrl::StrictMode).isValid();
    return QDir::isAbsolutePath(arg) && arg.endsWith(QStringLiteral(".torrent"), Qt::CaseInsensitive);
}
}

SingleInstance::SingleInstance(const QString &directory) : m_directory(directory) {}

bool SingleInstance::normalizeArguments(const QStringList &arguments, QStringList &normalized, QString &error)
{
    normalized.clear();
    if (arguments.size() > MaxArguments)
    {
        error = tr("Select no more than 256 torrent files or magnet links at once.");
        return false;
    }
    for (const auto &argument : arguments)
    {
        QString value = argument;
        if (value.startsWith(QStringLiteral("file:"), Qt::CaseInsensitive))
        {
            const QUrl url(value, QUrl::StrictMode);
            if (!url.isValid() || !url.isLocalFile())
            {
                error = tr("Invalid local file URL.");
                return false;
            }
            value = url.toLocalFile();
        }
        else if (!value.startsWith(QStringLiteral("magnet:"), Qt::CaseInsensitive))
            value = QFileInfo(value).absoluteFilePath();

        if (!validArgument(value))
        {
            error = tr("Only local .torrent files and magnet links can be opened.");
            return false;
        }
        normalized.append(value);
    }
    return true;
}

SingleInstance::Result SingleInstance::start(const QStringList &arguments, Receiver receiver, QString &error)
{
    if (m_directory.isEmpty() || !QDir().mkpath(m_directory))
    {
        error = tr("Cannot create the application data directory.");
        return Result::Error;
    }
    const QString directory = QFileInfo(m_directory).canonicalFilePath();
    if (directory.isEmpty())
    {
        error = tr("Cannot resolve the application data directory.");
        return Result::Error;
    }
    m_name = QStringLiteral("qbittorrent-remote-") + QString::fromLatin1(
        QCryptographicHash::hash(directory.toUtf8(), QCryptographicHash::Sha256).toHex().left(40));
    m_lock = std::make_unique<QLockFile>(QDir(directory).filePath(QStringLiteral("desktop-instance.lock")));
    // Never expire a lock just because a healthy application has been open
    // for a long time. QLockFile still detects a dead owner.
    m_lock->setStaleLockTime(0);
    const QByteArray message = QJsonDocument(QJsonArray::fromStringList(arguments)).toJson(QJsonDocument::Compact) + '\n';
    if (message.size() > MaxMessage || arguments.size() > MaxArguments)
    {
        error = tr("The file-opening request is too large.");
        return Result::Error;
    }
    if (!m_lock->tryLock(0))
    {
        if (m_lock->error() != QLockFile::LockFailedError)
        {
            error = tr("Cannot acquire the desktop instance lock.");
            return Result::Error;
        }
        return forward(message, error) ? Result::Forwarded : Result::Error;
    }

    // Only the lock owner may remove a stale endpoint.
    QLocalServer::removeServer(m_name);
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    m_receiver = std::move(receiver);
    connect(&m_server, &QLocalServer::newConnection, this, &SingleInstance::acceptConnections);
    if (!m_server.listen(m_name))
    {
        error = tr("Cannot start the local file-opening service: %1").arg(m_server.errorString());
        return Result::Error;
    }
    return Result::Primary;
}

bool SingleInstance::forward(const QByteArray &message, QString &error)
{
    // The primary might hold its lock before listen() has completed. Retry
    // connection only; never retransmit a request after it might be accepted.
    QElapsedTimer elapsed;
    elapsed.start();
    QLocalSocket socket;
    while (elapsed.elapsed() < Timeout)
    {
        socket.abort();
        socket.connectToServer(m_name);
        if (socket.state() == QLocalSocket::ConnectedState || socket.waitForConnected(200)) break;
        QThread::msleep(50);
    }
    if (socket.state() != QLocalSocket::ConnectedState)
    {
        error = tr("The existing qBittorrent Remote instance is not responding. No second instance was opened.");
        return false;
    }
    if (socket.write(message) != message.size())
    {
        error = tr("Cannot send the file-opening request.");
        return false;
    }
    elapsed.restart();
    while (socket.bytesToWrite() > 0 && elapsed.elapsed() < Timeout)
        if (!socket.waitForBytesWritten(qMax(1, Timeout - int(elapsed.elapsed())))) break;
    QByteArray reply;
    while (elapsed.elapsed() < Timeout)
    {
        reply += socket.readAll();
        if (reply.contains('\n')) break;
        if (reply.size() > 32 || !socket.waitForReadyRead(qMax(1, Timeout - int(elapsed.elapsed()))))
        {
            reply += socket.readAll();
            break;
        }
    }
    if (reply == "OK\n") return true;
    error = tr("The existing instance did not acknowledge the file-opening request. Check its window before retrying.");
    return false;
}

void SingleInstance::acceptConnections()
{
    while (auto *socket = m_server.nextPendingConnection())
    {
        socket->setReadBufferSize(MaxMessage + 1);
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        QTimer::singleShot(Timeout, socket, [socket] { socket->abort(); });
        auto buffer = std::make_shared<QByteArray>();
        auto finished = std::make_shared<bool>(false);
        const auto read = [this, socket, buffer, finished] {
            if (*finished) return;
            buffer->append(socket->readAll());
            if (buffer->size() > MaxMessage)
            {
                *finished = true;
                socket->abort();
                return;
            }
            const auto newline = buffer->indexOf('\n');
            if (newline < 0) return;
            *finished = true;
            QJsonParseError parseError;
            const auto document = QJsonDocument::fromJson(buffer->left(newline), &parseError);
            QStringList args;
            bool valid = parseError.error == QJsonParseError::NoError && document.isArray()
                && document.array().size() <= MaxArguments && newline == buffer->size() - 1;
            if (valid)
                for (const auto &value : document.array())
                {
                    if (!value.isString() || !validArgument(value.toString()))
                    {
                        valid = false;
                        break;
                    }
                    args.append(value.toString());
                }
            if (valid && m_receiver) m_receiver(args);
            socket->write(valid ? "OK\n" : "ERROR\n");
            socket->disconnectFromServer();
        };
        connect(socket, &QLocalSocket::readyRead, this, read);
        if (socket->bytesAvailable()) read();
    }
}
