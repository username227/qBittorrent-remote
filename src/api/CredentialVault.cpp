// SPDX-License-Identifier: GPL-3.0-or-later
#include "api/CredentialVault.h"
#include "api/ServerAddress.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QTimer>

#ifdef QBT_REMOTE_WITH_KEYCHAIN
#include <qt6keychain/keychain.h>
#endif

#include <utility>

namespace
{
const QString serviceName = QStringLiteral("qBittorrent Remote");

QString noVaultError()
{
    return QObject::tr("Secure password storage is unavailable. "
                       "Install QtKeychain and a supported system credential service.");
}
}

namespace Remote
{

// Keychain backend calls are asynchronous. A vault may belong to a short-lived
// connection dialog/window, so jobs must outlive that window while in flight.
// QCoreApplication owns the jobs; QtKeychain auto-deletes them when finished.
// The callback is connected with 'this' as context and is disconnected
// automatically if the vault is destroyed. Never delete an in-flight job with
// its owning window.
CredentialVault::CredentialVault(QObject *parent)
    : QObject(parent)
{
}

QString CredentialVault::keyFor(const ServerProfile &profile)
{
    const auto address = ServerAddress::parse(profile.url);
    if (profile.id.isEmpty() || !address)
        return {};

    // The key is bound to server identity, exact endpoint and username.
    // A changed address or username can never retrieve an old password.
    const QByteArray context = profile.id.toUtf8() + '\0'
        + address->baseUrl().toString(QUrl::FullyEncoded).toUtf8() + '\0'
        + profile.username.toUtf8();
    return QStringLiteral("server-")
        + QString::fromLatin1(QCryptographicHash::hash(
            context, QCryptographicHash::Sha256).toHex());
}

bool CredentialVault::available() const
{
#ifdef QBT_REMOTE_WITH_KEYCHAIN
    return true;
#else
    return false;
#endif
}

void CredentialVault::read(const ServerProfile &profile, ReadResult done)
{
    const QString key = keyFor(profile);
    if (key.isEmpty())
    {
        done({}, false, tr("Invalid saved server profile."));
        return;
    }
#ifdef QBT_REMOTE_WITH_KEYCHAIN
    auto *job = new QKeychain::ReadPasswordJob(serviceName, QCoreApplication::instance());
    job->setKey(key);
    job->setInsecureFallback(false);
    connect(job, &QKeychain::Job::finished, this, [done = std::move(done)](QKeychain::Job *j) {
        auto *readJob = static_cast<QKeychain::ReadPasswordJob *>(j);
        if (j->error() == QKeychain::EntryNotFound)
            done({}, false, {});
        else if (j->error() != QKeychain::NoError)
            done({}, false, j->errorString());
        else
            done(readJob->textData(), true, {});
    });
    job->start();
#else
    QTimer::singleShot(0, this, [done = std::move(done)] {
        done({}, false, noVaultError());
    });
#endif
}

void CredentialVault::write(const ServerProfile &profile,
                            const QString &password, Done done)
{
    const QString key = keyFor(profile);
    if (key.isEmpty())
    {
        done(tr("Invalid saved server profile."));
        return;
    }
#ifdef QBT_REMOTE_WITH_KEYCHAIN
    auto *job = new QKeychain::WritePasswordJob(serviceName, QCoreApplication::instance());
    job->setKey(key);
    job->setTextData(password);
    job->setInsecureFallback(false);
    connect(job, &QKeychain::Job::finished, this, [done = std::move(done)](QKeychain::Job *j) {
        done(j->error() == QKeychain::NoError ? QString() : j->errorString());
    });
    job->start();
#else
    QTimer::singleShot(0, this, [done = std::move(done)] { done(noVaultError()); });
#endif
}

void CredentialVault::erase(const ServerProfile &profile, Done done)
{
    const QString key = keyFor(profile);
    if (key.isEmpty())
    {
        done(tr("Invalid saved server profile."));
        return;
    }
#ifdef QBT_REMOTE_WITH_KEYCHAIN
    auto *job = new QKeychain::DeletePasswordJob(serviceName, QCoreApplication::instance());
    job->setKey(key);
    job->setInsecureFallback(false);
    connect(job, &QKeychain::Job::finished, this, [done = std::move(done)](QKeychain::Job *j) {
        // Deleting an absent key is equivalent to the desired outcome.
        const auto e = j->error();
        done(e == QKeychain::NoError || e == QKeychain::EntryNotFound
             ? QString() : j->errorString());
    });
    job->start();
#else
    QTimer::singleShot(0, this, [done = std::move(done)] { done(noVaultError()); });
#endif
}

} // namespace Remote
