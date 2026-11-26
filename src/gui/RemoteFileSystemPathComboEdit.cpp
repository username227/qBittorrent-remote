// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/RemoteFileSystemPathComboEdit.h"

#include <QAction>
#include <QCryptographicHash>
#include <QHBoxLayout>
#include <QMenu>
#include <QSettings>
#include <utility>
#include <QLineEdit>
#include <QStyle>
#include <QToolButton>

RemoteFileSystemPathComboEdit::RemoteFileSystemPathComboEdit(QWidget *parent)
    : QWidget(parent)
    , m_combo(new QComboBox(this))
    , m_browse(new QToolButton(this))
{
    // Matches the upstream FileSystemPathComboEdit's native, tight layout.
    // There must be no extra QGroupBox row or full-size "Browse…" button.
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    m_combo->setEditable(true);
    connect(m_combo, &QComboBox::editTextChanged, this, [this] { setProperty("explicitRemotePath", QVariant()); });
    m_combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_browse->setObjectName(QStringLiteral("browseFolderButton"));
    m_browse->setIcon(style()->standardIcon(QStyle::SP_DirOpenIcon));
    m_browse->setToolTip(tr("Browse remote or locally mounted filesystem"));
    m_browse->setAutoRaise(false);
    m_browse->setFixedSize(qMax(26, m_combo->sizeHint().height()),
                           m_combo->sizeHint().height());

    layout->addWidget(m_combo, 1);
    layout->addWidget(m_browse);
    connect(m_browse, &QToolButton::clicked, this, [this] {
        if (!m_browseMenu) { emit browseRequested(); return; }
        refreshFilesystemBrowse();
        if (m_remoteBrowse->isEnabled() || m_mountedBrowse->isEnabled())
            emit filesystemBrowseRequested(m_effectiveMode);
        else
            m_browse->showMenu();
    });
}

QLineEdit *RemoteFileSystemPathComboEdit::lineEdit() const
{
    return m_combo->lineEdit();
}
void RemoteFileSystemPathComboEdit::setEditable(bool editable)
{
    m_combo->setEditable(editable);
}
void RemoteFileSystemPathComboEdit::setInsertPolicy(QComboBox::InsertPolicy policy)
{
    m_combo->setInsertPolicy(policy);
}
void RemoteFileSystemPathComboEdit::setCurrentIndex(int index)
{
    m_combo->setCurrentIndex(index);
}
void RemoteFileSystemPathComboEdit::addItem(const QString &text)
{
    m_combo->addItem(text);
}
void RemoteFileSystemPathComboEdit::setEditText(const QString &text)
{
    m_combo->setEditText(text);
}
QString RemoteFileSystemPathComboEdit::currentText() const
{
    return m_combo->currentText();
}

void RemoteFileSystemPathComboEdit::configureFilesystemBrowse(QSettings &settings,
    const QString &profileId, std::function<BrowseAvailability()> availability)
{
    m_browseSettings = &settings;
    m_browseKey = QStringLiteral("Layout/RemoteFilesystemBrowseMode/") + QString::fromLatin1(
        QCryptographicHash::hash(profileId.toUtf8(), QCryptographicHash::Sha256).toHex());
    m_availability = std::move(availability);
    if (!m_browseMenu)
    {
        m_browseMenu = new QMenu(m_browse);
        m_browseMenu->setObjectName("serverPathBrowseMenu");
        m_remoteBrowse = m_browseMenu->addAction(tr("Browse remote filesystem…"));
        m_remoteBrowse->setObjectName("browseRemoteFilesystem");
        m_remoteBrowse->setCheckable(true);
        m_mountedBrowse = m_browseMenu->addAction(tr("Browse locally mounted filesystem…"));
        m_mountedBrowse->setObjectName("browseMountedFilesystem");
        m_mountedBrowse->setCheckable(true);
        m_browseMenu->addSeparator();
        m_openMapped = m_browseMenu->addAction(tr("Open in file manager"));
        m_openMapped->setObjectName("openMappedFilesystem");
        connect(m_remoteBrowse, &QAction::triggered, this, [this] { chooseBrowseMode(BrowseMode::Remote); });
        connect(m_mountedBrowse, &QAction::triggered, this, [this] { chooseBrowseMode(BrowseMode::Mounted); });
        connect(m_openMapped, &QAction::triggered, this, [this] { emit filesystemBrowseRequested(BrowseMode::OpenMapped); });
        connect(m_browseMenu, &QMenu::aboutToShow, this, &RemoteFileSystemPathComboEdit::refreshFilesystemBrowse);
        m_browse->setMenu(m_browseMenu);
        m_browse->setPopupMode(QToolButton::MenuButtonPopup);
        // Allow the native style to reserve space for its split arrow.
        m_browse->setMaximumWidth(QWIDGETSIZE_MAX);
        m_browse->setMinimumWidth(0);
        m_browse->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }
    refreshFilesystemBrowse();
}

void RemoteFileSystemPathComboEdit::refreshFilesystemBrowse()
{
    if (!m_browseMenu) return;
    const auto available = m_availability();
    const bool preferMounted = m_browseSettings->value(m_browseKey).toString() == QStringLiteral("mounted");
    m_remoteBrowse->setEnabled(available.remote);
    m_mountedBrowse->setEnabled(available.mounted);
    m_openMapped->setEnabled(available.openMapped);
    // A temporary unavailable method does not overwrite the saved choice.
    const bool useMounted = (preferMounted && available.mounted) || (!available.remote && available.mounted);
    m_effectiveMode = useMounted ? BrowseMode::Mounted : BrowseMode::Remote;
    m_remoteBrowse->setChecked(!useMounted && available.remote);
    m_mountedBrowse->setChecked(useMounted);
    m_browse->setIcon(style()->standardIcon(useMounted ? QStyle::SP_DirOpenIcon : QStyle::SP_DriveNetIcon));
    m_browse->setToolTip(useMounted ? tr("Browse locally mounted filesystem")
        : available.remote ? tr("Browse remote filesystem") : tr("No filesystem browser currently available"));
    m_browse->updateGeometry();
}

void RemoteFileSystemPathComboEdit::chooseBrowseMode(BrowseMode mode)
{
    const auto available = m_availability();
    if ((mode == BrowseMode::Remote && !available.remote)
        || (mode == BrowseMode::Mounted && !available.mounted))
    {
        refreshFilesystemBrowse();
        return;
    }
    m_browseSettings->setValue(m_browseKey,
        mode == BrowseMode::Mounted ? QStringLiteral("mounted") : QStringLiteral("remote"));
    refreshFilesystemBrowse();
    emit filesystemBrowseRequested(mode);
}
