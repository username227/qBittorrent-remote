// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/ServerAddress.h"
#include "api/ServerProfile.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QPointer>

#include <functional>
#include <optional>

namespace Remote
{

// A separately owned QObject for each remote server. Every instance has an
// independent network manager and cookie jar; no cross-server auth state.
class RemoteSession final : public QObject
{
    Q_OBJECT

public:
    explicit RemoteSession(QObject *parent = nullptr);

    void connectToServer(const ServerProfile &profile, const QString &password);
    void disconnectFromServer();
    bool isConnected() const noexcept;

signals:
    void connected(const QString &version, const QString &apiVersion);
    void failed(const QString &reason);

private:
    QNetworkRequest requestFor(const QString &endpoint) const;
    void requestVersion();
    void requestApiVersion(const QString &version);
    void checkReply(QNetworkReply *reply, const std::function<void(const QByteArray &)> &onSuccess);
    void fail(const QString &reason);

    QNetworkAccessManager m_network;
    std::optional<ServerAddress> m_address;
    QPointer<QNetworkReply> m_pendingReply;
    bool m_connected = false;
};

} // namespace Remote
