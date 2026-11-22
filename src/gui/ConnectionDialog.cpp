// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/ConnectionDialog.h"

#include "api/ServerAddress.h"

#include <QComboBox>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

ConnectionDialog::ConnectionDialog(QSettings &settings, QWidget *parent,
                                   Remote::CredentialVault *vault)
    : QDialog(parent)
    , m_store(settings)
    , m_vault(vault)
{
    setWindowTitle(tr("Connect to qBittorrent Server"));
    setMinimumWidth(440);

    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;

    m_profiles = new QComboBox(this);
    m_profiles->setObjectName(QStringLiteral("savedServers"));
    form->addRow(tr("Saved servers:"), m_profiles);

    auto *buttons = new QWidget(this);
    auto *management = new QHBoxLayout(buttons);
    management->setContentsMargins(0, 0, 0, 0);
    auto *newButton = new QPushButton(tr("New"), buttons);
    newButton->setObjectName(QStringLiteral("newServer"));
    auto *saveButton = new QPushButton(tr("Save"), buttons);
    saveButton->setObjectName(QStringLiteral("saveServer"));
    m_removeButton = new QPushButton(tr("Remove"), buttons);
    m_removeButton->setObjectName(QStringLiteral("removeServer"));
    management->addWidget(newButton);
    management->addWidget(saveButton);
    management->addWidget(m_removeButton);
    management->addStretch();
    form->addRow(QString(), buttons);

    m_name = new QLineEdit(this);
    m_name->setObjectName(QStringLiteral("serverName"));
    form->addRow(tr("Name:"), m_name);

    m_url = new QLineEdit(this);
    m_url->setObjectName(QStringLiteral("serverUrl"));
    m_url->setPlaceholderText(tr("http://server:8080 or https://server:8080"));
    form->addRow(tr("Server URL:"), m_url);

    m_username = new QLineEdit(this);
    m_username->setObjectName(QStringLiteral("serverUsername"));
    m_username->setText(QStringLiteral("admin"));
    form->addRow(tr("Username:"), m_username);

    m_password = new QLineEdit(this);
    m_password->setObjectName(QStringLiteral("serverPassword"));
    m_password->setEchoMode(QLineEdit::Password);
    form->addRow(tr("Password:"), m_password);

    m_remember = new QCheckBox(tr("Remember password securely"), this);
    m_remember->setObjectName(QStringLiteral("rememberPassword"));
    m_remember->setEnabled(m_vault && m_vault->available());
    if (!m_remember->isEnabled())
        m_remember->setToolTip(tr("QtKeychain secure storage support was not built in."));
    form->addRow(QString(), m_remember);

    layout->addLayout(form);
    auto *note = new QLabel(
        tr("Passwords are never stored in profile settings. When Remember "
           "password is selected, the operating system's protected credential "
           "store is used. A blank password is allowed for servers configured "
           "for passwordless access. HTTPS certificate trust remains separate."), this);
    note->setWordWrap(true);
    layout->addWidget(note);

    auto *dialogButtons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    auto *connectButton = dialogButtons->addButton(tr("Connect"), QDialogButtonBox::AcceptRole);
    connectButton->setObjectName(QStringLiteral("connectServer"));
    layout->addWidget(dialogButtons);

    connect(m_profiles, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { loadSelectedProfile(); });
    connect(newButton, &QPushButton::clicked, this,
            [this]() { clearForNewProfile(); });
    connect(saveButton, &QPushButton::clicked, this,
            [this]() { saveCurrentProfile(); });
    connect(m_removeButton, &QPushButton::clicked, this,
            [this]() { removeCurrentProfile(); });
    connect(dialogButtons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(connectButton, &QPushButton::clicked, this, [this]() {
        if (saveCurrentProfile())
            accept();
    });

    refreshProfiles();
}

Remote::ServerProfile ConnectionDialog::selectedProfile() const
{
    return m_selected;
}

QString ConnectionDialog::password() const
{
    return m_password->text();
}

bool ConnectionDialog::rememberPassword() const
{
    return m_remember->isEnabled() && m_remember->isChecked();
}

void ConnectionDialog::refreshProfiles(const QString &selectId)
{
    const QList<Remote::ServerProfile> profiles = m_store.profiles();
    m_profiles->blockSignals(true);
    m_profiles->clear();
    for (const auto &profile : profiles)
        m_profiles->addItem(profile.name, profile.id);
    const int index = selectId.isEmpty() ? (profiles.isEmpty() ? -1 : 0)
                                         : m_profiles->findData(selectId);
    m_profiles->setCurrentIndex(index);
    m_profiles->blockSignals(false);
    loadSelectedProfile();
}

void ConnectionDialog::loadSelectedProfile()
{
    m_selected = {};
    const QString id = m_profiles->currentData().toString();
    for (const auto &profile : m_store.profiles())
    {
        if (profile.id == id)
        {
            m_selected = profile;
            break;
        }
    }
    m_name->setText(m_selected.name);
    m_url->setText(m_selected.url);
    m_username->setText(m_selected.username.isEmpty()
                        ? QStringLiteral("admin") : m_selected.username);
    m_password->clear();
    m_remember->setChecked(m_selected.rememberPassword && m_remember->isEnabled());
    m_removeButton->setEnabled(!m_selected.id.isEmpty());
}

void ConnectionDialog::clearForNewProfile()
{
    m_profiles->setCurrentIndex(-1);
    m_selected = {};
    m_name->clear();
    m_url->clear();
    m_username->setText(QStringLiteral("admin"));
    m_password->clear();
    m_remember->setChecked(false);
    m_removeButton->setEnabled(false);
    m_name->setFocus();
}

bool ConnectionDialog::saveCurrentProfile()
{
    const QString name = m_name->text().trimmed();
    const QString url = m_url->text().trimmed();
    const auto address = Remote::ServerAddress::parse(url);
    if (name.isEmpty() || !address)
    {
        QMessageBox::warning(this, tr("Invalid server"),
                             tr("Provide a name and a valid http:// or https:// server URL."));
        return false;
    }

    Remote::ServerProfile edited {
        m_selected.id, name, address->baseUrl().toString(),
        m_username->text().trimmed(), rememberPassword()
    };
    const Remote::ServerProfile previous = m_selected;
    if (!m_store.save(edited))
    {
        QMessageBox::warning(this, tr("Cannot save server"),
                             tr("The server profile could not be saved."));
        return false;
    }

    // A changed endpoint or username must not leave an orphaned credential.
    // Explicitly opting out also forgets the old password immediately,
    // rather than waiting for another successful connection.
    if (m_vault && m_vault->available() && !previous.id.isEmpty()
        && (previous.url != edited.url || previous.username != edited.username
            || (previous.rememberPassword && !edited.rememberPassword)))
    {
        QPointer<ConnectionDialog> self(this);
        m_vault->erase(previous, [self](const QString &error) {
            if (self && !error.isEmpty())
                QMessageBox::warning(self, QObject::tr("Cannot Forget Password"), error);
        });
    }

    // Selecting a newly saved profile must not wipe the entered password.
    const QString typedPassword = m_password->text();
    refreshProfiles(edited.id);
    m_password->setText(typedPassword);
    m_selected = edited;
    return true;
}

void ConnectionDialog::removeCurrentProfile()
{
    if (m_selected.id.isEmpty())
        return;
    if (QMessageBox::question(this, tr("Remove server"),
                              tr("Remove the saved server \"%1\"?").arg(m_selected.name))
        != QMessageBox::Yes)
        return;

    const Remote::ServerProfile removed = m_selected;
    if (!m_store.remove(m_selected.id))
    {
        QMessageBox::warning(this, tr("Cannot remove server"),
                             tr("The saved server could not be removed."));
        return;
    }
    if (m_vault && m_vault->available())
    {
        QPointer<ConnectionDialog> self(this);
        m_vault->erase(removed, [self](const QString &error) {
            if (self && !error.isEmpty())
                QMessageBox::warning(self, QObject::tr("Cannot Forget Password"), error);
        });
    }
    refreshProfiles();
}
