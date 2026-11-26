// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/RemoteFileSystemPathComboEdit.h"
#include "gui/RemoteFileDialog.h"
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QPointer>
#include <QUrl>

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
    , m_browse(new RemoteFilesystemBrowseButton(this))
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
        if (!m_browse->menu()) emit browseRequested();
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
    m_browse->configure(settings, [profileId] { return profileId; }, std::move(availability),
        [this](BrowseMode mode) { emit filesystemBrowseRequested(mode); });
}

void RemoteFileSystemPathComboEdit::refreshFilesystemBrowse() { m_browse->refresh(); }

RemoteFilesystemBrowseButton::RemoteFilesystemBrowseButton(QWidget *parent) : QToolButton(parent)
{
    connect(this, &QToolButton::clicked, this, [this] {
        if (!m_browseMenu) return;
        refresh();
        if (m_remoteBrowse->isEnabled() || m_mountedBrowse->isEnabled())
            m_chosen(m_effectiveMode);
        else showMenu();
    });
}

QString RemoteFilesystemBrowseButton::preferenceKey() const
{
    return QStringLiteral("Layout/RemoteFilesystemBrowseMode/") + QString::fromLatin1(
        QCryptographicHash::hash(m_profileId().toUtf8(), QCryptographicHash::Sha256).toHex());
}

void RemoteFilesystemBrowseButton::configure(QSettings &settings,
    std::function<QString()> profileId, std::function<BrowseAvailability()> availability,
    std::function<void(BrowseMode)> chosen)
{
    m_settings = &settings;
    m_profileId = std::move(profileId);
    m_availability = std::move(availability);
    m_chosen = std::move(chosen);
    if (!m_browseMenu)
    {
        m_browseMenu = new QMenu(this);
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
        connect(m_remoteBrowse, &QAction::triggered, this, [this] { choose(BrowseMode::Remote); });
        connect(m_mountedBrowse, &QAction::triggered, this, [this] { choose(BrowseMode::Mounted); });
        connect(m_openMapped, &QAction::triggered, this, [this] { m_chosen(BrowseMode::OpenMapped); });
        connect(m_browseMenu, &QMenu::aboutToShow, this, &RemoteFilesystemBrowseButton::refresh);
        setMenu(m_browseMenu);
        setPopupMode(QToolButton::MenuButtonPopup);
        // Allow the native style to reserve space for its split arrow.
        setMaximumWidth(QWIDGETSIZE_MAX);
        setMinimumWidth(0);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }
    refresh();
}

void RemoteFilesystemBrowseButton::refresh()
{
    if (!m_browseMenu) return;
    const auto available = m_availability();
    const bool preferMounted = m_settings->value(preferenceKey()).toString() == QStringLiteral("mounted");
    m_remoteBrowse->setEnabled(available.remote);
    m_mountedBrowse->setEnabled(available.mounted);
    m_openMapped->setEnabled(available.openMapped);
    // A temporary unavailable method does not overwrite the saved choice.
    const bool useMounted = (preferMounted && available.mounted) || (!available.remote && available.mounted);
    m_effectiveMode = useMounted ? BrowseMode::Mounted : BrowseMode::Remote;
    m_remoteBrowse->setChecked(!useMounted && available.remote);
    m_mountedBrowse->setChecked(useMounted);
    setIcon(style()->standardIcon(useMounted ? QStyle::SP_DirOpenIcon : QStyle::SP_DriveNetIcon));
    setToolTip(useMounted ? tr("Browse locally mounted filesystem")
        : available.remote ? tr("Browse remote filesystem") : tr("No filesystem browser currently available"));
    updateGeometry();
}

void RemoteFilesystemBrowseButton::choose(BrowseMode mode)
{
    const auto available = m_availability();
    if ((mode == BrowseMode::Remote && !available.remote)
        || (mode == BrowseMode::Mounted && !available.mounted))
    {
        refresh();
        return;
    }
    m_settings->setValue(preferenceKey(),
        mode == BrowseMode::Mounted ? QStringLiteral("mounted") : QStringLiteral("remote"));
    refresh();
    emit preferenceChanged();
    m_chosen(mode);
}

void RemoteFilesystemBrowseButton::configurePath(QSettings &settings,
    std::function<PathContext()> context, std::function<void(const QString &)> selected)
{
    configure(settings, [context] { return context().profileId; }, [context] {
        const auto ctx = context();
        const QString local = Remote::PathMappingStore::toLocal(ctx.current, ctx.mappings);
        return BrowseAvailability{ctx.session && ctx.session->isConnected(), !ctx.mappings.isEmpty(),
            !local.isEmpty() && QFileInfo(local).exists()};
    }, [this, &settings, context, selected](BrowseMode mode) {
        const auto ctx = context();
        const QPointer<Remote::RemoteSession> session(ctx.session);
        const QString mapped = Remote::PathMappingStore::toLocal(ctx.current, ctx.mappings);
        QString remote;
        if (mode == BrowseMode::OpenMapped)
        {
            const QFileInfo info(mapped);
            const QString dir = info.isDir() ? mapped : info.isFile() ? info.absolutePath() : QString();
            if (!mapped.isEmpty() && !dir.isEmpty() && !QDesktopServices::openUrl(QUrl::fromLocalFile(dir)))
                QMessageBox::warning(window(), tr("Cannot open directory"), tr("The desktop could not open the mapped directory."));
            return;
        }
        if (mode == BrowseMode::Remote)
        {
            if (!session || !session->isConnected()) return;
            const QString initial = ctx.directory ? ctx.current : RemoteFileDialog::parentDirectory(ctx.current);
            RemoteFileDialog picker(session.data(), ctx.serverName, ctx.profileId, settings, initial,
                ctx.directory ? RemoteFileDialog::Mode::Directory : RemoteFileDialog::Mode::File, window());
            if (picker.exec() != QDialog::Accepted) return;
            remote = picker.selectedPath();
        }
        else
        {
            if (ctx.mappings.isEmpty()) return;
            const QFileInfo info(mapped);
            const QString initial = !mapped.isEmpty() && (info.isDir() || (!ctx.directory && info.isFile()))
                ? mapped : ctx.mappings.first().localRoot;
            const QString local = ctx.directory
                ? QFileDialog::getExistingDirectory(window(), tr("Choose mapped local NAS directory"), initial)
                : QFileDialog::getOpenFileName(window(), tr("Choose mapped local NAS file"), initial, ctx.fileFilter);
            if (local.isEmpty()) return;
            remote = Remote::PathMappingStore::toRemote(local, ctx.mappings);
            if (remote.isEmpty())
            {
                QMessageBox::warning(window(), ctx.directory ? tr("Unmapped local directory") : tr("Unmapped local file"),
                    tr("The selected path is not covered by a mapping for this connected server. The server path has not been changed."));
                return;
            }
        }
        if (context().profileId == ctx.profileId) selected(remote);
    });
}

RemoteFilesystemBrowseButton *RemoteFilesystemBrowseButton::attachToLineEdit(QLineEdit *line)
{
    auto *button = new RemoteFilesystemBrowseButton(line);
    button->setObjectName(line->objectName() + QStringLiteral("Browse"));
    button->setAutoRaise(true);
    auto *layout = new QHBoxLayout(line);
    layout->setContentsMargins(0, 0, 2, 0);
    layout->setSpacing(0);
    layout->addStretch();
    layout->addWidget(button, 0, Qt::AlignVCenter);
    return button;
}

void RemoteFileSystemPathComboEdit::configureDirectoryBrowse(QSettings &settings,
    const QString &profileId, const QString &serverName, Remote::RemoteSession *session,
    const QList<Remote::PathMapping> &mappings, bool translateTypedLocalPath)
{
    const QPointer<Remote::RemoteSession> connection(session);
    const auto currentRemote = [this, mappings, translateTypedLocalPath] {
        const QString input = currentText().trimmed();
        if (!translateTypedLocalPath || property("explicitRemotePath").toString() == input)
            return input;
        const QString mapped = Remote::PathMappingStore::toRemote(input, mappings);
        return mapped.isEmpty() ? input : mapped;
    };
    m_browse->configurePath(settings, [connection, mappings, currentRemote, profileId, serverName] {
        return RemoteFilesystemBrowseButton::PathContext{profileId, serverName, connection.data(), mappings, currentRemote(), true, {}};
    }, [this](const QString &remote) {
        setEditText(remote);
        setProperty("explicitRemotePath", remote);
    });
}
