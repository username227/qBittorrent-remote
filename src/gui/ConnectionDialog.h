// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/ServerProfile.h"
#include "api/CredentialVault.h"

#include <QDialog>

class QComboBox;
class QLineEdit;
class QCheckBox;
class QPushButton;

class ConnectionDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit ConnectionDialog(QSettings &settings, QWidget *parent = nullptr,
                              Remote::CredentialVault *vault = nullptr);

    Remote::ServerProfile selectedProfile() const;
    QString password() const;
    bool rememberPassword() const;
    void selectProfile(const QString &id);

signals:
    void profileSaved(const Remote::ServerProfile &profile);

protected:
    void done(int result) override;

private:
    void refreshProfiles(const QString &selectId = {});
    void loadSelectedProfile();
    bool saveCurrentProfile();
    void removeCurrentProfile();
    void clearForNewProfile();

    QSettings *m_settings = nullptr;
    Remote::ProfileStore m_store;
    Remote::CredentialVault *m_vault = nullptr;
    Remote::ServerProfile m_selected;
    QComboBox *m_profiles = nullptr;
    QLineEdit *m_name = nullptr;
    QLineEdit *m_url = nullptr;
    QLineEdit *m_username = nullptr;
    QLineEdit *m_password = nullptr;
    QCheckBox *m_remember = nullptr;
    QCheckBox *m_autoConnect = nullptr;
    QPushButton *m_removeButton = nullptr;
};

