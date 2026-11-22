// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/ServerProfile.h"

#include <QDialog>

class QComboBox;
class QLineEdit;
class QPushButton;

class ConnectionDialog final : public QDialog
{
public:
    explicit ConnectionDialog(QSettings &settings, QWidget *parent = nullptr);

    Remote::ServerProfile selectedProfile() const;
    QString password() const;

private:
    void refreshProfiles(const QString &selectId = {});
    void loadSelectedProfile();
    bool saveCurrentProfile();
    void removeCurrentProfile();
    void clearForNewProfile();

    Remote::ProfileStore m_store;
    Remote::ServerProfile m_selected;
    QComboBox *m_profiles = nullptr;
    QLineEdit *m_name = nullptr;
    QLineEdit *m_url = nullptr;
    QLineEdit *m_username = nullptr;
    QLineEdit *m_password = nullptr;
    QPushButton *m_removeButton = nullptr;
};

