// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/ServerProfile.h"

#include <QObject>
#include <QString>

#include <functional>

namespace Remote
{

// Credentials are managed by the operating system's secure credential store.
// The default implementation uses QtKeychain where available. Virtual methods
// allow CTest to exercise complete GUI workflows with a disposable vault.
class CredentialVault : public QObject
{
public:
    using ReadResult = std::function<void(const QString &password, bool found,
                                          const QString &error)>;
    using Done = std::function<void(const QString &error)>;

    explicit CredentialVault(QObject *parent = nullptr);
    ~CredentialVault() override = default;

    virtual bool available() const;
    virtual void read(const ServerProfile &profile, ReadResult done);
    virtual void write(const ServerProfile &profile, const QString &password, Done done);
    virtual void erase(const ServerProfile &profile, Done done);

protected:
    static QString keyFor(const ServerProfile &profile);
};

} // namespace Remote
