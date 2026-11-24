// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/RemotePreferencesDialog.h"
#include "gui/RemoteBanListDialog.h"
#include <QFileInfo>
#include <QToolButton>
#include "gui/DesktopAutostart.h"
#include "gui/TorrentModel.h"
#include "api/ServerProfile.h"
#include "api/RemoteSession.h"
#include "ui_optionsdialog.h"

#include <QAbstractButton>
#include <QAction>
#include <QStyle>
#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFontMetrics>
#include <QFormLayout>
#include <QGroupBox>
#include <QJsonValue>
#include <QIcon>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QScreen>
#include <QSystemTrayIcon>
#include <QGuiApplication>
#include <QPushButton>
#include <QRandomGenerator>
#include <limits>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QItemSelectionModel>
#include <QTreeView>
#include <QTableWidget>
#include <QTreeWidget>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QVBoxLayout>
#include <QVersionNumber>
#include <QHBoxLayout>

namespace
{
// Both supported upstream generations normalize nonpositive connection
// and upload-slot limits to -1. Normalize only integral JSON numbers,
// never missing, fractional, out-of-range or nonnumeric values.
QJsonValue normalizedConnectionLimit(const QString &key, QJsonValue value)
{
    if ((key == QLatin1String("max_connec")
            || key == QLatin1String("max_connec_per_torrent")
            || key == QLatin1String("max_uploads")
            || key == QLatin1String("max_uploads_per_torrent"))
        && value.isDouble() && value.toDouble() == value.toInt()
        && value.toInt() <= 0)
        return QJsonValue(-1);
    return value;
}

QIcon icon(const char *name)
{
    return QIcon(QStringLiteral(":/icons/")
                 + QString::fromLatin1(name) + QStringLiteral(".svg"));
}

bool mappingEditor(QWidget *parent, Remote::PathMapping &mapping)
{
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("mappingEditor"));
    dialog.setWindowTitle(QObject::tr("Local / Remote Directory Mapping"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *local = new QLineEdit(&dialog);
    local->setObjectName(QStringLiteral("mappingLocalPath"));
    local->setText(QDir::toNativeSeparators(mapping.localRoot));
    local->setPlaceholderText(QObject::tr("Locally mounted NAS directory"));

    auto *localRow = new QWidget(&dialog);
    auto *localLayout = new QHBoxLayout(localRow);
    localLayout->setContentsMargins(0, 0, 0, 0);
    localLayout->addWidget(local, 1);
    auto *browse = new QPushButton(QObject::tr("Browse…"), localRow);
    browse->setObjectName(QStringLiteral("mappingBrowseLocal"));
    localLayout->addWidget(browse);
    QObject::connect(browse, &QPushButton::clicked, &dialog, [&dialog, local]() {
        const QString dir = QFileDialog::getExistingDirectory(
            &dialog, QObject::tr("Choose locally mounted NAS directory"), local->text());
        if (!dir.isEmpty())
            local->setText(QDir::toNativeSeparators(dir));
    });
    form->addRow(QObject::tr("Local directory:"), localRow);

    auto *remote = new QLineEdit(&dialog);
    remote->setObjectName(QStringLiteral("mappingRemotePath"));
    remote->setText(mapping.remoteRoot);
    remote->setPlaceholderText(QStringLiteral("/share/CACHEDEV1_DATA/..."));
    form->addRow(QObject::tr("Server directory:"), remote);
    layout->addLayout(form);

    auto *note = new QLabel(QObject::tr(
        "The local directory is a mount on THIS computer. The server directory "
        "must be an absolute path visible to qBittorrent on the selected server. "
        "The Web API does not provide remote filesystem browsing."), &dialog);
    note->setWordWrap(true);
    layout->addWidget(note);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected,
                     &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog,
                     [&dialog, local, remote, &mapping]() {
        const Remote::PathMapping candidate {
            QDir::fromNativeSeparators(local->text()), remote->text()};
        QString error;
        if (!Remote::PathMappingStore::validate({candidate}, &error))
        {
            QMessageBox::warning(&dialog, QObject::tr("Invalid mapping"), error);
            return;
        }
        mapping = candidate;
        dialog.accept();
    });
    return dialog.exec() == QDialog::Accepted;
}

void setUnsupported(QWidget *widget)
{
    widget->setEnabled(false);
    const QString existing = widget->toolTip();
    const QString reason = QObject::tr(
        "Not implemented in qBittorrent Remote. Displayed defaults are not "
        "live values from the selected server.");
    widget->setToolTip(existing.isEmpty() ? reason : existing + QLatin1Char('\n') + reason);
}
}

RemotePreferencesDialog::RemotePreferencesDialog(QSettings &settings,
                                                 const QString &preferredProfileId,
                                                 QWidget *parent,
                                                 Remote::RemoteSession *session,
                                                 TorrentModel *torrentModel,
                                                 const QString &serverVersion)
    : QDialog(parent)
    , m_settings(settings)
    , m_remoteProfileId(preferredProfileId)
    , m_mappingStore(settings)
    , m_ui(std::make_unique<Ui::OptionsDialog>())
    , m_remoteSession(session)
    , m_torrentModel(torrentModel)
{
    m_ui->setupUi(this);
    setObjectName(QStringLiteral("remotePreferencesDialog"));
    setWindowTitle(tr("Preferences"));
    // The original native dialog is freely resizable. Allow compact sizes
    // too, and use a taller initial height for our tenth navigation page.
    // MainWindow restores whatever size the user subsequently chooses.
    setMinimumSize(650, 480);
    const QScreen *screen = QGuiApplication::screenAt(
        parent ? parent->mapToGlobal(parent->rect().center()) : QPoint());
    const int available = screen ? screen->availableGeometry().height() : 900;
    resize(820, qMax(minimumHeight(), qMin(710, available - 60)));

    // These nine pages are the actual upstream Qt Designer preferences
    // layout (GPL-3.0-or-later). Only local-engine-only custom path widgets
    // were substituted with stock QLineEdit widgets.
    disableUnimplementedControls();
    loadLocalBehaviorControls();
    loadLocalAdditionControls();
    bindServerBehaviorControls();
    bindDownloadsSavingControls();
    bindTorrentAdditionDefaults();
    bindBackupAndWatchedFolders();
    bindExcludedFileNames();
    bindNotificationAndProgramControls();
    bindConnectionListeningControls();
    bindConnectionLimits();
    bindI2PControls();
    bindProxyControls();
    // The range API was introduced in 5.3.0; 5.1/5.2 silently discard
    // ranges. Use the authenticated daemon's version, not the GUI version.
    QString numeric = serverVersion;
    if (numeric.startsWith(QLatin1Char('v'))) numeric.remove(0, 1);
    const auto version = QVersionNumber::fromString(numeric);
    m_banRangesSupported = version.segmentCount() >= 3
        && version >= QVersionNumber(5, 3, 0);
    bindIPFilterControls();
    populateAdvancedPlaceholders();
    buildRemotePathsPage(preferredProfileId);

    const char *icons[] = {
        "folder-remote", "preferences-desktop", "download", "network-connect",
        "speedometer", "preferences-bittorrent", "edit-find", "application-rss",
        "preferences-webui", "preferences-advanced"
    };
    for (int i = 0; i < m_ui->tabSelection->count(); ++i)
    {
        auto *item = m_ui->tabSelection->item(i);
        item->setIcon(icon(icons[i]));
        item->setTextAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    }
    m_ui->tabSelection->setObjectName(QStringLiteral("preferencesSections"));
    m_ui->tabOption->setObjectName(QStringLiteral("preferencesPages"));
    // Native qBittorrent uses the blue speedometer icon. A modest size
    // increase matches the original preferences navigation more closely,
    // without changing font sizes or expanding row spacing arbitrarily.
    m_ui->tabSelection->setIconSize(QSize(28, 28));
    // Match upstream optionsdialog.cpp: give each item effectively
    // unlimited requested width so IconMode lays it out across the
    // entire viewport, naturally centering BOTH the icon and its label.
    // setTextAlignment() alone does not center the icon. A fixed gridSize
    // was the source of the visibly left-anchored presentation.
    m_ui->tabSelection->setGridSize(QSize());
    const int tileHeight = qMax(47,
        m_ui->tabSelection->iconSize().height()
        + m_ui->tabSelection->fontMetrics().height() + 5);
    for (int i = 0; i < m_ui->tabSelection->count(); ++i)
        m_ui->tabSelection->item(i)->setSizeHint(
            QSize(std::numeric_limits<int>::max(), tileHeight));
    m_ui->tabSelection->setMinimumWidth(132);
    m_ui->tabSelection->setMaximumWidth(154);
    m_ui->tabSelection->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_ui->tabSelection->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_ui->hsplitter->setSizes({140, 660});
    QObject::connect(m_ui->tabSelection, &QListWidget::currentRowChanged,
                     m_ui->tabOption, &QStackedWidget::setCurrentIndex);

    // A single compact header identifies the fixed remote target on every
    // page, independently of the saved-profile selector for local mappings.
    m_ui->tabSelection->setToolTip(tr(
        "Desktop preferences are local. Remote preferences load from the "
        "server selected when this dialog opens. Unsupported controls remain disabled."));
    if (m_remoteSession)
    {
        for (const auto &profile : Remote::ProfileStore(m_settings).profiles())
            if (profile.id == preferredProfileId)
            {
                m_remoteTargetName = profile.name;
                break;
            }
        if (!m_remoteTargetName.isEmpty())
            setWindowTitle(tr("Preferences — %1").arg(m_remoteTargetName));
    }
    m_remoteTargetLabel = new QLabel(this);
    m_remoteTargetLabel->setObjectName(QStringLiteral("preferencesRemoteTarget"));
    m_remoteTargetLabel->setTextFormat(Qt::PlainText);
    m_remoteTargetLabel->setWordWrap(true);
    QFont targetFont = m_remoteTargetLabel->font();
    targetFont.setBold(true);
    m_remoteTargetLabel->setFont(targetFont);
    m_remoteTargetLabel->setToolTip(tr("Remote settings apply only to this server. Desktop settings apply to the application. The local path-mapping selector does not change the remote target."));
    m_remoteTargetLabel->setText(m_remoteSession
        ? tr("Server settings: %1").arg(m_remoteTargetName.isEmpty() ? tr("Connected server") : m_remoteTargetName)
        : tr("No connected server — local settings only"));
    // Keep the full height of the native navigation column. The target
    // belongs above the settings pages, not above the entire splitter.
    auto *settingsPane = new QWidget;
    auto *settingsLayout = new QVBoxLayout(settingsPane);
    settingsLayout->setContentsMargins(0, 0, 0, 0);
    auto *pages = m_ui->hsplitter->replaceWidget(
        m_ui->hsplitter->indexOf(m_ui->tabOption), settingsPane);
    settingsLayout->addWidget(m_remoteTargetLabel);
    settingsLayout->addWidget(pages, 1);
    m_ui->hsplitter->setCollapsible(1, false);
    m_ui->hsplitter->setSizes({140, 660});

    m_applyButton = m_ui->buttonBox->button(QDialogButtonBox::Apply);
    m_applyButton->setObjectName(QStringLiteral("preferencesApply"));
    QObject::connect(m_applyButton, &QPushButton::clicked, this,
                     [this]() { applyChanges(false); });
    QObject::connect(m_ui->buttonBox, &QDialogButtonBox::accepted, this,
                     [this]() { applyChanges(true); });
    QObject::connect(m_ui->buttonBox, &QDialogButtonBox::rejected,
                     this, &QDialog::reject);

    // Previously Remote Paths was last (index 9). Moving the page first
    // must preserve whichever page the user last selected, not reopen the
    // wrong page. Only migrate settings written by the earlier PR #9 build.
    const bool reordered = m_settings.value(
        QStringLiteral("Preferences/NavOrderVersion"), 0).toInt() >= 2;
    const int savedPage = m_settings.value(
        QStringLiteral("Preferences/LastPage"), 0).toInt();
    const int selection = reordered
        ? savedPage
        : (m_settings.contains(QStringLiteral("Preferences/LastPage"))
            ? (savedPage == 9 ? 0 : savedPage + 1)
            : 0);
    m_ui->tabSelection->setCurrentRow(
        qBound(0, selection, m_ui->tabSelection->count() - 1));
    updateButtons();
    loadRemotePreferences();
}

RemotePreferencesDialog::~RemotePreferencesDialog() = default;

void RemotePreferencesDialog::disableUnimplementedControls()
{
    // Disable only *interactive* widgets, not entire pages: users must still
    // be able to scroll the complete original settings to review progress.
    for (int pageIndex = 0; pageIndex < m_ui->tabOption->count(); ++pageIndex)
    {
        QWidget *page = m_ui->tabOption->widget(pageIndex);
        for (QWidget *widget : page->findChildren<QWidget *>())
        {
            // QComboBox owns a popup QAbstractItemView. Disabling its view
            // separately leaves every popup item visually disabled even after
            // we re-enable the combo in Behavior. Treat the combo as one
            // control and never disable its internal widgets independently.
            bool insideComboBox = false;
            for (QWidget *ancestor = widget->parentWidget();
                 ancestor && ancestor != page;
                 ancestor = ancestor->parentWidget())
            {
                if (qobject_cast<QComboBox *>(ancestor))
                {
                    insideComboBox = true;
                    break;
                }
            }
            if (insideComboBox)
                continue;

            if (auto *group = qobject_cast<QGroupBox *>(widget))
            {
                if (group->isCheckable())
                    setUnsupported(group);
            }
            else if (qobject_cast<QAbstractButton *>(widget)
                     || qobject_cast<QComboBox *>(widget)
                     || qobject_cast<QAbstractSpinBox *>(widget)
                     || qobject_cast<QLineEdit *>(widget)
                     || qobject_cast<QTextEdit *>(widget)
                     || qobject_cast<QPlainTextEdit *>(widget)
                     || qobject_cast<QAbstractItemView *>(widget))
                setUnsupported(widget);
        }
    }
}

void RemotePreferencesDialog::loadLocalBehaviorControls()
{
    // These native controls affect this remote desktop application, not the
    // qBittorrent daemon. They must not modify any server preferences.
    const auto unlock = [](QWidget *widget, const QString &tooltip = {}) {
        widget->setEnabled(true);
        widget->setToolTip(tooltip);
        // Also repair popups disabled by earlier initialization; enabling a
        // QComboBox alone does not undo an explicit disable on its view.
        if (auto *combo = qobject_cast<QComboBox *>(widget))
            combo->view()->setEnabled(true);
    };
    unlock(m_ui->confirmDeletion);
    unlock(m_ui->checkTorrentContentDrag,
        tr("Allow dragging files from the Content tab into a file manager. "
           "Hold Alt to invert drag versus selection behavior. "
           "The files must be accessible through this server's path mappings."));
    unlock(m_ui->checkUseTorrentStatesColors);
    unlock(m_ui->checkProgressBarFollowsTextColor);
    unlock(m_ui->checkBoxUseSeparateTrackerStatusFilter);
    unlock(m_ui->checkProgramExitConfirm);
    unlock(m_ui->checkAltRowColors);
    unlock(m_ui->checkHideZero);
    unlock(m_ui->comboHideZero);
    unlock(m_ui->actionTorrentDlOnDblClBox);
    unlock(m_ui->actionTorrentFnOnDblClBox);
    unlock(m_ui->checkBoxFreeDiskSpaceStatusBar);
    unlock(m_ui->checkBoxHideZeroStatusFilters);
    unlock(m_ui->windowStateComboBox);
    if (DesktopAutostart::available())
        unlock(m_ui->checkStartup);
    if (QSystemTrayIcon::isSystemTrayAvailable())
        unlock(m_ui->checkShowSystray);
    unlock(m_ui->checkMinimizeToSysTray);
    unlock(m_ui->checkCloseToSystray);
    unlock(m_ui->comboTrayIcon);
    unlock(m_ui->checkNotifyTorrentCompletion);
    unlock(m_ui->checkSoundTorrentCompletion);
    m_ui->checkNotifyTorrentCompletion->setChecked(m_settings.value(
        QStringLiteral("Preferences/Behavior/NotifyTorrentCompletion"), true).toBool());
    m_ui->checkSoundTorrentCompletion->setChecked(m_settings.value(
        QStringLiteral("Preferences/Behavior/SoundTorrentCompletion"), false).toBool());

    m_ui->checkStartup->setText(tr("Start qBittorrent Remote when system starts"));
    m_ui->checkStartup->setToolTip(tr(
        "Start when you sign in to your desktop session (Linux), "
        "or when you sign in to Windows. This does not start a system daemon."));
    m_ui->checkStartup->setChecked(DesktopAutostart::isEnabled());
    m_ui->windowStateComboBox->clear();
    m_ui->windowStateComboBox->addItem(tr("Normal"), QStringLiteral("normal"));
    m_ui->windowStateComboBox->addItem(tr("Minimized"), QStringLiteral("minimized"));
    m_ui->windowStateComboBox->addItem(tr("Hidden"), QStringLiteral("hidden"));
    const QString startupWindowState = m_settings.value(
        QStringLiteral("Preferences/Behavior/StartupWindowState"),
        QStringLiteral("normal")).toString();
    const int startupIndex = m_ui->windowStateComboBox->findData(startupWindowState);
    m_ui->windowStateComboBox->setCurrentIndex(startupIndex >= 0 ? startupIndex : 0);
    m_ui->checkShowSystray->setChecked(m_settings.value(
        QStringLiteral("Preferences/Behavior/EnableTray"), false).toBool()
        && QSystemTrayIcon::isSystemTrayAvailable());
    m_ui->checkMinimizeToSysTray->setChecked(m_settings.value(
        QStringLiteral("Preferences/Behavior/MinimizeToTray"), false).toBool());
    m_ui->checkCloseToSystray->setChecked(m_settings.value(
        QStringLiteral("Preferences/Behavior/CloseToTray"), false).toBool());
    m_ui->comboTrayIcon->setCurrentIndex(qBound(0, m_settings.value(
        QStringLiteral("Preferences/Behavior/TrayIconStyle"), 0).toInt(),
        m_ui->comboTrayIcon->count() - 1));
    const auto updateTrayInputs = [this]() {
        const bool active = m_ui->checkShowSystray->isEnabled()
            && m_ui->checkShowSystray->isChecked();
        m_ui->checkMinimizeToSysTray->setEnabled(active);
        m_ui->checkCloseToSystray->setEnabled(active);
        m_ui->comboTrayIcon->setEnabled(active);
    };
    updateTrayInputs();

    m_ui->confirmDeletion->setChecked(m_settings.value(
        QStringLiteral("Preferences/Behavior/ConfirmTorrentDeletion"), true).toBool());
    m_ui->checkProgramExitConfirm->setChecked(m_settings.value(
        QStringLiteral("Preferences/Behavior/ConfirmExit"), false).toBool());
    m_ui->checkTorrentContentDrag->setChecked(m_settings.value(
        QStringLiteral("Preferences/Behavior/TorrentContentDragEnabled"), true).toBool());
    m_ui->checkUseTorrentStatesColors->setChecked(m_settings.value(
        QStringLiteral("Preferences/Behavior/StateColors"), true).toBool());
    m_ui->checkProgressBarFollowsTextColor->setChecked(m_settings.value(
        QStringLiteral("Preferences/Behavior/ProgressFollowsState"), false).toBool());

    m_ui->checkBoxUseSeparateTrackerStatusFilter->setChecked(m_settings.value(
        QStringLiteral("Preferences/Behavior/SeparateTrackerStatus"), false).toBool());
    m_ui->checkAltRowColors->setChecked(m_settings.value(
        QStringLiteral("Preferences/Behavior/AlternatingRows"), true).toBool());
    const bool hideZero = m_settings.value(
        QStringLiteral("Preferences/Behavior/HideZero"), false).toBool();
    m_ui->checkHideZero->setChecked(hideZero);
    m_ui->comboHideZero->setCurrentIndex(qBound(0, m_settings.value(
        QStringLiteral("Preferences/Behavior/HideZeroMode"), 0).toInt(), 1));
    m_ui->comboHideZero->setEnabled(hideZero);

    // Preview requires a separate remote-access and playback milestone.
    // Do not offer an action that would be a no-op or open a wrong local file.
    for (QComboBox *box : {m_ui->actionTorrentDlOnDblClBox,
                           m_ui->actionTorrentFnOnDblClBox})
    {
        box->removeItem(2);
        for (int i = 0; i < box->count(); ++i)
        {
            static const char *const actionIds[] = {
                "toggle", "folder", "options", "none"
            };
            box->setItemData(i, QString::fromLatin1(actionIds[i]));
        }
    }
    const auto loadAction = [this](QComboBox *box, const QString &key,
                                   const QString &fallback) {
        const QString selected = m_settings.value(key, fallback).toString();
        const int index = box->findData(selected);
        box->setCurrentIndex(index >= 0 ? index : box->findData(fallback));
    };
    loadAction(m_ui->actionTorrentDlOnDblClBox,
        QStringLiteral("Preferences/Behavior/DoubleClickDownloading"),
        QStringLiteral("toggle"));
    loadAction(m_ui->actionTorrentFnOnDblClBox,
        QStringLiteral("Preferences/Behavior/DoubleClickCompleted"),
        QStringLiteral("folder"));
    m_ui->checkBoxHideZeroStatusFilters->setChecked(m_settings.value(
        QStringLiteral("Preferences/Behavior/HideZeroStatusFilters"), false).toBool());
    m_ui->checkBoxFreeDiskSpaceStatusBar->setChecked(m_settings.value(
        QStringLiteral("Preferences/Behavior/ShowServerFreeSpace"), false).toBool());
    m_ui->checkBoxFreeDiskSpaceStatusBar->setToolTip(tr(
        "Show available space on the connected server's default save drive, "
        "as reported by its Web API. This is not your desktop's free space."));

    const auto dirty = [this]() {
        m_behaviorDirty = true;
        updateButtons();
    };
    connect(m_ui->checkStartup, &QCheckBox::toggled, this, dirty);
    connect(m_ui->windowStateComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [dirty](int) { dirty(); });
    connect(m_ui->checkShowSystray, &QGroupBox::toggled, this,
            [updateTrayInputs, dirty](bool) { updateTrayInputs(); dirty(); });
    connect(m_ui->checkMinimizeToSysTray, &QCheckBox::toggled, this, dirty);
    connect(m_ui->checkCloseToSystray, &QCheckBox::toggled, this, dirty);
    connect(m_ui->checkNotifyTorrentCompletion, &QCheckBox::toggled, this, dirty);
    connect(m_ui->checkSoundTorrentCompletion, &QCheckBox::toggled, this, dirty);
    connect(m_ui->comboTrayIcon, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [dirty](int) { dirty(); });
    connect(m_ui->confirmDeletion, &QCheckBox::toggled, this, dirty);
    connect(m_ui->checkProgramExitConfirm, &QCheckBox::toggled, this, dirty);
    connect(m_ui->checkTorrentContentDrag, &QCheckBox::toggled, this, dirty);
    connect(m_ui->checkUseTorrentStatesColors, &QCheckBox::toggled, this, dirty);
    connect(m_ui->checkProgressBarFollowsTextColor, &QCheckBox::toggled,
            this, dirty);
    connect(m_ui->checkBoxUseSeparateTrackerStatusFilter, &QCheckBox::toggled,
            this, dirty);
    connect(m_ui->checkAltRowColors, &QCheckBox::toggled, this, dirty);
    connect(m_ui->checkHideZero, &QCheckBox::toggled, this, [this, dirty](bool value) {
        m_ui->comboHideZero->setEnabled(value);
        dirty();
    });
    connect(m_ui->comboHideZero, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [dirty](int) { dirty(); });
    for (QComboBox *box : {m_ui->actionTorrentDlOnDblClBox,
                           m_ui->actionTorrentFnOnDblClBox})
        connect(box, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [dirty](int) { dirty(); });
    connect(m_ui->checkBoxFreeDiskSpaceStatusBar, &QCheckBox::toggled,
            this, dirty);
    connect(m_ui->checkBoxHideZeroStatusFilters, &QCheckBox::toggled,
            this, dirty);
}

void RemotePreferencesDialog::bindRemoteField(RemoteField field)
{
    m_remoteFields.append(std::move(field));
}

void RemotePreferencesDialog::bindRemoteControl(const char *key, QWidget *widget,
    std::function<bool()> dependency, QString tooltip)
{
    RemoteField field{QString::fromLatin1(key), widget, {}, {}, {},
                      std::move(dependency), std::move(tooltip)};
    const auto changed = [this]() { remoteFieldChanged(); };
    const auto boolean = [&field](auto *control) {
        field.read = [control]() { return QJsonValue(control->isChecked()); };
        field.write = [control](const QJsonValue &value) { control->setChecked(value.toBool()); };
        field.accepts = [](const QJsonValue &value) { return value.isBool(); };
    };
    if (auto *group = qobject_cast<QGroupBox *>(widget))
    {
        boolean(group);
        connect(group, &QGroupBox::toggled, this, changed);
    }
    else if (auto *box = qobject_cast<QCheckBox *>(widget))
    {
        boolean(box);
        connect(box, &QCheckBox::toggled, this, changed);
    }
    else if (auto *line = qobject_cast<QLineEdit *>(widget))
    {
        field.read = [line]() { return QJsonValue(line->text()); };
        field.write = [line](const QJsonValue &value) { line->setText(value.toString()); };
        field.accepts = [](const QJsonValue &value) { return value.isString(); };
        connect(line, &QLineEdit::textChanged, this, changed);
    }
    else if (auto *editor = qobject_cast<QPlainTextEdit *>(widget))
    {
        field.read = [editor]() { return QJsonValue(editor->toPlainText()); };
        field.write = [editor](const QJsonValue &value) { editor->setPlainText(value.toString()); };
        field.accepts = [](const QJsonValue &value) { return value.isString(); };
        connect(editor, &QPlainTextEdit::textChanged, this, changed);
    }
    else if (auto *spin = qobject_cast<QSpinBox *>(widget))
    {
        field.read = [spin]() { return QJsonValue(spin->value()); };
        field.write = [spin](const QJsonValue &value) { spin->setValue(value.toInt()); };
        field.accepts = [spin](const QJsonValue &value) {
            return value.isDouble() && value.toDouble() >= spin->minimum()
                && value.toDouble() <= spin->maximum() && value.toDouble() == value.toInt();
        };
        connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), this, changed);
    }
    else if (auto *combo = qobject_cast<QComboBox *>(widget))
    {
        field.read = [combo]() { return QJsonValue(combo->currentIndex()); };
        field.write = [combo](const QJsonValue &value) { combo->setCurrentIndex(value.toInt()); };
        field.accepts = [combo](const QJsonValue &value) {
            return value.isDouble() && value.toDouble() >= 0
                && value.toDouble() < combo->count() && value.toDouble() == value.toInt();
        };
        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, changed);
    }
    else
        return;
    bindRemoteField(std::move(field));
}

void RemotePreferencesDialog::bindConnectionListeningControls()
{
    // Both upstream API generations use the same BTProtocol enumeration:
    // Both=0, TCP=1, UTP=2. The native combo follows that exact order.
    bindRemoteControl("bittorrent_protocol", m_ui->comboProtocol, {},
        tr("Peer connection protocol used by the connected server."));
    bindRemoteControl("listen_port", m_ui->spinPort, {},
        tr("Incoming BitTorrent port on the connected server, not its Web API port. "
           "Set to 0 (Any) to let the server select an unused port."));
    bindRemoteControl("upnp", m_ui->checkUPnP, {},
        tr("Request UPnP / NAT-PMP forwarding for the connected server's BitTorrent port."));
    connect(m_ui->randomButton, &QPushButton::clicked, this, [this]() {
        if (!m_remoteBusy && remoteFieldAvailable("listen_port")
            && m_ui->spinPort->isEnabled())
            m_ui->spinPort->setValue(QRandomGenerator::global()->bounded(1024, 65536));
    });
    // random_port is a deprecated alias for listen_port == 0. Never
    // write both keys, or a stale alias could override an explicit port.
}

void RemotePreferencesDialog::bindConnectionLimits()
{
    const auto bind = [this](const char *key, QCheckBox *check, QSpinBox *spin) {
        // Upstream setters interpret 0 as unlimited, not zero slots.
        // Avoid a checked-but-unlimited value in the native upload controls.
        spin->setMinimum(qMax(1, spin->minimum()));
        bindRemoteField(RemoteField{
            QString::fromLatin1(key), check,
            [check, spin]() { return QJsonValue(check->isChecked() ? spin->value() : -1); },
            [check, spin](const QJsonValue &value) {
                const int limit = value.toInt();
                check->setChecked(limit > 0);
                // Retain the last in-dialog numeric value when unchecked,
                // just like native qBittorrent; never persist it locally.
                if (limit > 0)
                    spin->setValue(limit);
            },
            [spin](const QJsonValue &value) {
                return value.isDouble() && value.toDouble() == value.toInt()
                    && (value.toInt() == -1
                        || (value.toInt() >= spin->minimum() && value.toInt() <= spin->maximum()));
            },
            {}, tr("Limit on the selected server. Uncheck for unlimited.")
        });
        connect(check, &QCheckBox::toggled, this, [this]() { remoteFieldChanged(); });
        connect(spin, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [this]() { remoteFieldChanged(); });
    };
    bind("max_connec", m_ui->checkMaxConnections, m_ui->spinMaxConnec);
    bind("max_connec_per_torrent", m_ui->checkMaxConnectionsPerTorrent, m_ui->spinMaxConnecPerTorrent);
    bind("max_uploads", m_ui->checkMaxUploads, m_ui->spinMaxUploads);
    bind("max_uploads_per_torrent", m_ui->checkMaxUploadsPerTorrent, m_ui->spinMaxUploadsPerTorrent);
}

void RemotePreferencesDialog::refreshConnectionLimitControls()
{
    const auto refresh = [this](const char *key, QCheckBox *check, QSpinBox *spin) {
        const bool supported = remoteFieldAvailable(key);
        spin->setEnabled(supported && check->isChecked());
        spin->setToolTip(supported ? check->toolTip() : tr(
            "This setting is unavailable from the connected server. Displayed values are not live."));
    };
    refresh("max_connec", m_ui->checkMaxConnections, m_ui->spinMaxConnec);
    refresh("max_connec_per_torrent", m_ui->checkMaxConnectionsPerTorrent, m_ui->spinMaxConnecPerTorrent);
    refresh("max_uploads", m_ui->checkMaxUploads, m_ui->spinMaxUploads);
    refresh("max_uploads_per_torrent", m_ui->checkMaxUploadsPerTorrent, m_ui->spinMaxUploadsPerTorrent);
}

void RemotePreferencesDialog::bindI2PControls()
{
    // Upstream app/preferences advertises these fields even when the
    // engine was compiled without TORRENT_USE_I2P. buildInfo can rule
    // out incompatible versions, but cannot establish the compile flag.
    bindRemoteControl("i2p_enabled", m_ui->groupI2P, [this]() {
        return m_i2pVersionSupportsConfig;
    });
    const auto enabled = [this]() {
        return m_i2pVersionSupportsConfig && remoteFieldAvailable("i2p_enabled")
            && m_ui->groupI2P->isChecked();
    };
    bindRemoteControl("i2p_address", m_ui->textI2PHost, enabled,
        tr("SAM bridge host as seen by the connected server, not this desktop."));
    bindRemoteControl("i2p_port", m_ui->spinI2PPort, enabled,
        tr("SAM bridge port on the connected server's network."));
    bindRemoteControl("i2p_mixed_mode", m_ui->checkI2PMixed, enabled,
        tr("Mixed mode allows I2P torrents to discover and connect to regular IP peers. "
           "This does not provide anonymization."));
    bindRemoteControl("i2p_pex_enabled", m_ui->checkI2PPeX, [this, enabled]() {
        return enabled() && m_i2pVersionSupportsPeX;
    }, tr("Enable I2P peer exchange on the server. Requires libtorrent 2.1 or later "
          "and a server restart; changing this preference does not restart the server."));
}

void RemotePreferencesDialog::refreshI2PControls()
{
    m_ui->labelI2PHost->setEnabled(m_ui->textI2PHost->isEnabled());
    m_ui->labelI2PPort->setEnabled(m_ui->spinI2PPort->isEnabled());
    // Keep the native layout. Explain unavailable controls only on hover.
    if (remoteFieldAvailable("i2p_enabled"))
    {
        if (!m_i2pBuildInfoFinished)
            m_ui->groupI2P->setToolTip(tr("Checking server libtorrent version..."));
        else if (!m_i2pVersionSupportsConfig)
            m_ui->groupI2P->setToolTip(m_i2pLibtorrentVersion.isEmpty()
                ? tr("Server libtorrent version could not be verified.")
                : tr("Unavailable for server libtorrent %1.").arg(m_i2pLibtorrentVersion));
    }
    if (!m_i2pVersionSupportsPeX && remoteFieldAvailable("i2p_pex_enabled"))
        m_ui->checkI2PPeX->setToolTip(tr("I2P PeX requires server libtorrent 2.1 or later."));
}

void RemotePreferencesDialog::readI2PBuildInfo()
{
    if (!m_remoteSession || !m_remoteSession->isConnected()
        || !remoteFieldAvailable("i2p_enabled"))
        return;

    const QPointer<RemotePreferencesDialog> dialog(this);
    m_remoteSession->requestBuildInfo([dialog](const QJsonObject &info) {
        if (!dialog || !dialog->m_remotePreferencesLoaded || !dialog->m_remoteSession
            || !dialog->m_remoteSession->isConnected())
            return;
        const QString reported = info.value(QStringLiteral("libtorrent")).toString();
        // Parse the numeric version prefix (upstream lt::version() can
        // include a build suffix). Require at least major and minor.
        const QVersionNumber version = QVersionNumber::fromString(reported);
        if (version.segmentCount() >= 2)
        {
            dialog->m_i2pLibtorrentVersion = version.toString();
            dialog->m_i2pVersionSupportsConfig = version.majorVersion() == 2;
            dialog->m_i2pVersionSupportsPeX = version.majorVersion() == 2
                && version.minorVersion() >= 1;
        }
        dialog->m_i2pBuildInfoFinished = true;
        dialog->refreshRemoteControls();
        dialog->updateButtons();
    }, [dialog](const QString &) {
        if (!dialog || !dialog->m_remotePreferencesLoaded)
            return;
        // Optional build information must never prevent unrelated settings.
        dialog->m_i2pBuildInfoFinished = true;
        dialog->refreshRemoteControls();
        dialog->updateButtons();
    });
}

void RemotePreferencesDialog::bindProxyControls()
{
    // Match the native enum order and labels, not the WebUI's numeric
    // legacy representation. Both upstream versions expose string keys.
    m_ui->comboProxyType->addItems({tr("(None)"), tr("SOCKS4"), tr("SOCKS5"), tr("HTTP")});
    bindStringCombo("proxy_type", m_ui->comboProxyType,
        {QStringLiteral("None"), QStringLiteral("SOCKS4"), QStringLiteral("SOCKS5"), QStringLiteral("HTTP")});
    const auto active = [this]() {
        return remoteFieldAvailable("proxy_type") && m_ui->comboProxyType->currentIndex() > 0;
    };
    const auto modern = [this, active]() {
        return active() && m_ui->comboProxyType->currentIndex() >= 2;
    };
    const auto auth = [this, modern]() {
        return modern() && remoteFieldAvailable("proxy_auth_enabled")
            && m_ui->checkProxyAuth->isChecked();
    };
    bindRemoteControl("proxy_ip", m_ui->textProxyIP, active,
        tr("Proxy host as seen by the connected server."));
    bindRemoteControl("proxy_port", m_ui->spinProxyPort, active);
    bindRemoteControl("proxy_hostname_lookup", m_ui->checkProxyHostnameLookup, modern,
        tr("If checked, hostname lookups are done via the proxy."));
    bindRemoteControl("proxy_auth_enabled", m_ui->checkProxyAuth, modern);
    bindRemoteControl("proxy_username", m_ui->textProxyUsername, auth);
    m_ui->textProxyPassword->setEchoMode(QLineEdit::Password);
    bindRemoteControl("proxy_password", m_ui->textProxyPassword, auth);
    bindRemoteControl("proxy_bittorrent", m_ui->checkProxyBitTorrent, active);
    bindRemoteControl("proxy_peer_connections", m_ui->checkProxyPeerConnections,
        [this, active]() {
            return active() && remoteFieldAvailable("proxy_bittorrent")
                && m_ui->checkProxyBitTorrent->isChecked();
        }, tr("Otherwise, the proxy server is only used for tracker connections."));
    bindRemoteControl("proxy_rss", m_ui->checkProxyRSS, modern,
        tr("RSS feeds will use proxy."));
    bindRemoteControl("proxy_misc", m_ui->checkProxyMisc, modern,
        tr("Search engine, software updates or anything else on the server will use proxy."));
}

void RemotePreferencesDialog::refreshProxyControls()
{
    m_ui->labelProxyTypeUnavailable->setVisible(remoteFieldAvailable("proxy_type")
        && m_ui->comboProxyType->currentIndex() == 1);
    m_ui->lblProxyIP->setEnabled(m_ui->textProxyIP->isEnabled());
    m_ui->lblProxyPort->setEnabled(m_ui->spinProxyPort->isEnabled());
    m_ui->lblProxyUsername->setEnabled(m_ui->textProxyUsername->isEnabled());
    m_ui->lblProxyPassword->setEnabled(m_ui->textProxyPassword->isEnabled());
}

void RemotePreferencesDialog::bindIPFilterControls()
{
    bindRemoteControl("ip_filter_enabled", m_ui->checkIPFilter);
    bindRemoteControl("ip_filter_path", m_ui->textFilterPath, [this]() {
        return remoteFieldAvailable("ip_filter_enabled") && m_ui->checkIPFilter->isChecked();
    }, tr("Absolute IP filter file path on the connected server. Browse a mapped NAS file or enter the server path."));
    auto *browse = m_ui->textFilterPath->addAction(style()->standardIcon(QStyle::SP_DirOpenIcon),
                                                   QLineEdit::TrailingPosition);
    browse->setObjectName(QStringLiteral("textFilterPathBrowse"));
    browse->setToolTip(tr("Choose a mapped local NAS filter file"));
    connect(browse, &QAction::triggered, this, [this]() {
        if (m_ui->textFilterPath->isEnabled())
            browseRemotePath(m_ui->textFilterPath, true);
    });
    bindRemoteControl("ip_filter_trackers", m_ui->checkIpFilterTrackers);
    m_ui->IpFilterRefreshBtn->setIcon(icon("view-refresh"));
    m_ui->IpFilterRefreshBtn->setEnabled(false);
    m_ui->IpFilterRefreshBtn->setToolTip(tr(
        "The Web API has no explicit filter reload operation. Reload on the server itself."));
    bindRemoteField(RemoteField{
        QStringLiteral("banned_IPs"), m_ui->banListButton,
        [this]() { return QJsonValue(m_bannedIPs); },
        [this](const QJsonValue &value) { m_bannedIPs = value.toString(); },
        [this](const QJsonValue &value) {
            return value.isString() && RemoteBanListDialog::validList(value.toString(), m_banRangesSupported);
        },
        {}, tr("Edit manually banned addresses on the selected server. Changes are staged until Apply or OK.")
    });
    connect(m_ui->banListButton, &QPushButton::clicked, this, [this]() {
        if (m_remoteBusy || !remoteFieldAvailable("banned_IPs"))
            return;
        RemoteBanListDialog dialog(m_bannedIPs, m_banRangesSupported, this);
        if (dialog.exec() == QDialog::Accepted && remoteFieldAvailable("banned_IPs"))
        {
            m_bannedIPs = dialog.entries();
            remoteFieldChanged();
        }
    });
}

bool RemotePreferencesDialog::validateIPFilterChanges(const QJsonObject &changes)
{
    if (!changes.contains(QStringLiteral("ip_filter_path"))
        && !changes.value(QStringLiteral("ip_filter_enabled")).toBool())
        return true;
    if (!remoteFieldAvailable("ip_filter_path"))
    {
        QMessageBox::warning(this, tr("Unavailable filter path"),
            tr("The server has not reported a usable IP filter path."));
        return false;
    }
    const QString path = m_ui->textFilterPath->text();
    // Empty dormant paths are allowed; enabling a filter requires a file.
    if (path.isEmpty() && !m_ui->checkIPFilter->isChecked())
        return true;
    if (!path.startsWith(QLatin1Char('/')) || path.endsWith(QLatin1Char('/'))
        || path.contains(QLatin1Char('\\')) || path.contains(QLatin1Char('\n'))
        || path.contains(QLatin1Char('\r')) || path.contains(QChar::Null))
    {
        QMessageBox::warning(this, tr("Invalid filter path"),
            tr("Enter an absolute server file path, or choose a mapped local NAS filter file."));
        return false;
    }
    return true;
}

void RemotePreferencesDialog::bindServerBehaviorControls()
{
    bindRemoteControl("file_log_enabled", m_ui->checkFileLog);
    bindRemoteControl("file_log_path", m_ui->textFileLogPath, {},
        tr("Directory on the connected server, not this desktop."));
    bindRemoteControl("file_log_backup_enabled", m_ui->checkFileLogBackup);
    bindRemoteControl("file_log_max_size", m_ui->spinFileLogSize, [this]() {
        return remoteFieldAvailable("file_log_backup_enabled") && m_ui->checkFileLogBackup->isChecked();
    });
    bindRemoteControl("file_log_delete_old", m_ui->checkFileLogDelete);
    const auto deleteOld = [this]() {
        return remoteFieldAvailable("file_log_delete_old") && m_ui->checkFileLogDelete->isChecked();
    };
    bindRemoteControl("file_log_age", m_ui->spinFileLogAge, deleteOld);
    bindRemoteControl("file_log_age_type", m_ui->comboFileLogAgeType, deleteOld);
    bindRemoteControl("performance_warning", m_ui->checkBoxPerformanceWarning);
}

void RemotePreferencesDialog::remoteFieldChanged()
{
    if (!m_loadingRemotePreferences)
    {
        refreshRemoteControls();
        updateButtons();
    }
}

void RemotePreferencesDialog::bindBooleanCombo(const char *key, QComboBox *combo,
    int trueIndex, const QString &tooltip)
{
    bindRemoteField(RemoteField{
        QString::fromLatin1(key), combo,
        [combo, trueIndex]() { return QJsonValue(combo->currentIndex() == trueIndex); },
        [combo, trueIndex](const QJsonValue &value) {
            combo->setCurrentIndex(value.toBool() ? trueIndex : 1 - trueIndex);
        },
        [](const QJsonValue &value) { return value.isBool(); },
        {}, tooltip
    });
    connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this]() { remoteFieldChanged(); });
}

void RemotePreferencesDialog::bindStringCombo(const char *key, QComboBox *combo,
    const QStringList &values, std::function<bool()> dependency)
{
    bindRemoteField(RemoteField{
        QString::fromLatin1(key), combo,
        [combo, values]() { return QJsonValue(values.value(combo->currentIndex())); },
        [combo, values](const QJsonValue &value) {
            combo->setCurrentIndex(values.indexOf(value.toString()));
        },
        [values](const QJsonValue &value) {
            return value.isString() && values.contains(value.toString());
        },
        std::move(dependency), {}
    });
    connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this]() { remoteFieldChanged(); });
}

void RemotePreferencesDialog::loadLocalAdditionControls()
{
    const auto show = m_settings.value(QStringLiteral("Preferences/Downloads/ShowAdditionDialog"), true).toBool();
    m_ui->checkAdditionDialog->setChecked(show);
    m_ui->checkAdditionDialog->setEnabled(true);
    m_ui->checkAdditionDialog->setToolTip(tr("Show the desktop Add Torrent options dialog. Otherwise add to the active server using its defaults."));
    m_ui->checkAdditionDialogFront->setChecked(m_settings.value(
        QStringLiteral("Preferences/Downloads/AdditionDialogFront"), true).toBool());
    m_ui->checkAdditionDialogFront->setToolTip(tr("Request activation of the Add Torrent window. The desktop compositor may decline."));
    m_ui->checkConfirmMergeTrackers->setChecked(m_settings.value(
        QStringLiteral("Preferences/Downloads/ConfirmMergeTrackers"), true).toBool());
    m_ui->checkConfirmMergeTrackers->setToolTip(tr("Ask before merging trackers for a manually added duplicate. When unchecked, use the selected server's merge setting."));
    m_ui->deleteTorrentBox->setEnabled(true);
    m_ui->deleteTorrentBox->setChecked(m_settings.value(
        QStringLiteral("Preferences/Downloads/DeleteOriginalTorrent"), false).toBool());
    m_ui->deleteCancelledTorrentBox->setChecked(m_settings.value(
        QStringLiteral("Preferences/Downloads/DeleteCancelledTorrent"), false).toBool());
    m_ui->deleteTorrentWarningIcon->setPixmap(style()->standardIcon(
        QStyle::SP_MessageBoxCritical).pixmap(16, 16));
    const QString warning = tr("Warning: this permanently deletes the original .torrent "
        "files selected on this computer, not downloaded content. Files are removed after "
        "the selected server confirms their addition. Cancelled files are removed only "
        "if you explicitly cancel the Add Torrent dialog and allow deletion. "
        "Use Do not delete .torrent file to retain the originals.");
    m_ui->deleteTorrentBox->setToolTip(warning);
    m_ui->deleteCancelledTorrentBox->setToolTip(warning);
    m_ui->deleteTorrentWarningLabel->setToolTip(warning);
    const auto refresh = [this]() {
        const bool deleteOriginal = m_ui->deleteTorrentBox->isChecked();
        m_ui->deleteCancelledTorrentBox->setEnabled(deleteOriginal);
        m_ui->deleteTorrentWarningIcon->setVisible(deleteOriginal);
        m_ui->deleteTorrentWarningLabel->setVisible(deleteOriginal);
        const bool show = m_ui->checkAdditionDialog->isChecked();
        m_ui->checkAdditionDialogFront->setEnabled(show);
        m_ui->checkConfirmMergeTrackers->setEnabled(show);
    };
    refresh();
    const auto changed = [this, refresh]() {
        refresh();
        m_additionDirty = true;
        updateButtons();
    };
    connect(m_ui->checkAdditionDialog, &QGroupBox::toggled, this, changed);
    connect(m_ui->checkAdditionDialogFront, &QCheckBox::toggled, this, changed);
    connect(m_ui->checkConfirmMergeTrackers, &QCheckBox::toggled, this, changed);
    connect(m_ui->deleteTorrentBox, &QGroupBox::toggled, this, changed);
    connect(m_ui->deleteCancelledTorrentBox, &QCheckBox::toggled, this, changed);
}

void RemotePreferencesDialog::bindTorrentAdditionDefaults()
{
    bindRemoteControl("merge_trackers", m_ui->checkMergeTrackers, {},
        tr("Server default for duplicate torrents. Private torrents are never merged."));
    bindStringCombo("torrent_content_layout", m_ui->contentLayoutComboBox,
        {QStringLiteral("Original"), QStringLiteral("Subfolder"), QStringLiteral("NoSubfolder")});
    bindRemoteControl("add_to_top_of_queue", m_ui->checkAddToQueueTop);
    bindRemoteControl("add_stopped_enabled", m_ui->checkAddStopped);
    bindStringCombo("torrent_stop_condition", m_ui->stopConditionComboBox,
        {QStringLiteral("None"), QStringLiteral("MetadataReceived"), QStringLiteral("FilesChecked")},
        [this]() {
            return remoteFieldAvailable("add_stopped_enabled") && !m_ui->checkAddStopped->isChecked();
        });
}

void RemotePreferencesDialog::bindExcludedFileNames()
{
    bindRemoteControl("excluded_file_names_enabled", m_ui->groupExcludedFileNames);
    const auto enabled = [this]() {
        return remoteFieldAvailable("excluded_file_names_enabled")
            && m_ui->groupExcludedFileNames->isChecked();
    };
    // The Web API uses a newline-separated string, not an array. Leave
    // wildcard interpretation to the server, and preserve order and spelling.
    bindRemoteControl("excluded_file_names", m_ui->textExcludedFileNames, enabled,
        tr("Files matching any filter will have their priority automatically set to \"Do not download\".\n"
           "Use newlines to separate entries. Wildcards: * (zero or more characters), "
           "? (one character), [...] (character set).\n"
           "Examples: *.exe, readme.txt, ?.txt, readme[0-9].txt."));
    bindRemoteControl("exclude_files_without_extension", m_ui->checkExcludeFilesWithoutExtension, enabled,
        tr("Files whose name has no extension (e.g. 'readme', 'setup.') will have "
           "their priority automatically set to \"Do not download\"."));
}

void RemotePreferencesDialog::bindNotificationAndProgramControls()
{
    bindRemoteControl("mail_notification_enabled", m_ui->groupMailNotification);
    const auto mailEnabled = [this]() {
        return remoteFieldAvailable("mail_notification_enabled")
            && m_ui->groupMailNotification->isChecked();
    };
    bindRemoteControl("mail_notification_sender", m_ui->senderEmailTxt, mailEnabled);
    bindRemoteControl("mail_notification_email", m_ui->lineEditDestEmail, mailEnabled);
    bindRemoteControl("mail_notification_smtp", m_ui->lineEditSMTPServer, mailEnabled,
        tr("SMTP host or host:port, accessed from the connected server."));
    // The legacy Boolean cannot represent STARTTLS. Keep its native
    // checkbox separate from the modern string-valued encryption selector.
    auto *encryption = m_ui->comboSMTPEncryption;
    encryption->addItems({tr("None"), tr("STARTTLS"), tr("SMTPS")});
    bindStringCombo("mail_notification_encryption_type", encryption,
        {QStringLiteral("None"), QStringLiteral("STARTTLS"), QStringLiteral("SMTPS")}, mailEnabled);
    m_legacySmtpSsl = new QCheckBox(tr("Use SSL connection"), m_ui->groupMailNotification);
    m_legacySmtpSsl->setObjectName(QStringLiteral("checkSmtpSSL"));
    m_ui->layoutSMTPEncryption->insertWidget(1, m_legacySmtpSsl);
    m_legacySmtpSsl->hide();
    bindRemoteControl("mail_notification_ssl_enabled", m_legacySmtpSsl, [this, mailEnabled]() {
        return mailEnabled() && !remoteFieldAvailable("mail_notification_encryption_type");
    });
    bindRemoteControl("mail_notification_auth_enabled", m_ui->groupMailNotifAuth, mailEnabled);
    const auto authEnabled = [this, mailEnabled]() {
        return mailEnabled() && remoteFieldAvailable("mail_notification_auth_enabled")
            && m_ui->groupMailNotifAuth->isChecked();
    };
    bindRemoteControl("mail_notification_username", m_ui->mailNotifUsername, authEnabled);
    // The API returns the password as a string. It remains in this
    // dialog's in-memory transaction, never in desktop QSettings.
    m_ui->mailNotifPassword->setEchoMode(QLineEdit::Password);
    bindRemoteControl("mail_notification_password", m_ui->mailNotifPassword, authEnabled);
    connect(m_ui->sendTestEmail, &QPushButton::clicked, this, &RemotePreferencesDialog::sendTestEmail);

    bindRemoteControl("autorun_on_torrent_added_enabled", m_ui->groupBoxRunOnAdded);
    bindRemoteControl("autorun_on_torrent_added_program", m_ui->lineEditRunOnAdded, [this]() {
        return remoteFieldAvailable("autorun_on_torrent_added_enabled")
            && m_ui->groupBoxRunOnAdded->isChecked();
    });
    bindRemoteControl("autorun_enabled", m_ui->groupBoxRunOnFinished);
    bindRemoteControl("autorun_program", m_ui->lineEditRunOnFinished, [this]() {
        return remoteFieldAvailable("autorun_enabled") && m_ui->groupBoxRunOnFinished->isChecked();
    });
    auto *note = new QLabel(tr("Programs run on the connected server, not this desktop, "
        "even when qBittorrent Remote is closed."), m_ui->autoRunBox);
    note->setWordWrap(true);
    m_ui->verticalLayout_19->insertWidget(0, note);
    m_ui->labelAutoRunParam->setText(QStringList{
        tr("Supported parameters (case sensitive):"),
        tr("    %N: Torrent name"), tr("    %L: Category"), tr("    %G: Tags (separated by comma)"),
        tr("    %F: Content path (same as root path for multifile torrent)"),
        tr("    %R: Root path (first torrent subdirectory path)"), tr("    %D: Save path"),
        tr("    %C: Number of files"), tr("    %Z: Torrent size (bytes)"),
        tr("    %T: Current tracker"), tr("    %I: Info hash v1 (or '-' if unavailable)"),
        tr("    %J: Info hash v2 (or '-' if unavailable)"),
        tr("    %K: Torrent ID (either sha-1 info hash for v1 torrent or truncated sha-256 info hash for v2/hybrid torrent)"),
        tr("    %M: Comment (on servers supporting this parameter)"),
        tr("Tip: Encapsulate parameter with quotation marks to avoid text being cut off at whitespace (e.g., \"%N\")")
    }.join(QLatin1Char('\n')));
}

bool RemotePreferencesDialog::hasPendingMailChanges() const
{
    const auto changes = remoteChanges();
    for (auto it = changes.constBegin(); it != changes.constEnd(); ++it)
        if (it.key().startsWith(QLatin1String("mail_notification_")))
            return true;
    return false;
}

void RemotePreferencesDialog::refreshNotificationControls()
{
    const bool modern = remoteFieldAvailable("mail_notification_encryption_type");
    const bool legacy = remoteFieldAvailable("mail_notification_ssl_enabled") && !modern;
    m_legacySmtpSsl->setVisible(legacy);
    m_ui->comboSMTPEncryption->setVisible(!legacy);
    const int index = m_ui->comboSMTPEncryption->currentIndex();
    const int port = legacy ? (m_legacySmtpSsl->isChecked() ? 465 : 25)
        : (index == 1 ? 587 : (index == 2 ? 465 : 25));
    m_ui->labelSMTPEncryptionPortInfo->setText(tr("Default port: %1").arg(port));
    m_ui->labelSMTPEncryptionPortInfo->setVisible(modern || legacy);
    const bool dirty = hasPendingMailChanges();
    const bool available = m_remoteSession && m_remoteSession->isConnected()
        && remoteFieldAvailable("mail_notification_enabled") && m_ui->groupMailNotification->isChecked()
        && remoteFieldAvailable("mail_notification_sender") && remoteFieldAvailable("mail_notification_email")
        && remoteFieldAvailable("mail_notification_smtp");
    m_ui->sendTestEmail->setEnabled(available && !m_remoteBusy && !dirty);
    m_ui->sendTestEmail->setToolTip(dirty
        ? tr("Apply the changed email settings before sending a test email.")
        : tr("Ask the connected server to send a test email using its saved settings."));
}

void RemotePreferencesDialog::sendTestEmail()
{
    if (!m_remoteSession || !m_remoteSession->isConnected() || m_remoteBusy
        || !m_ui->sendTestEmail->isEnabled() || hasPendingMailChanges())
        return;
    m_testEmailPending = true;
    setRemoteBusy(true);
    const QPointer<RemotePreferencesDialog> dialog(this);
    m_remoteSession->sendTestEmail([dialog]() {
        if (!dialog || !dialog->m_testEmailPending) return;
        dialog->m_testEmailPending = false;
        dialog->setRemoteBusy(false);
        QMessageBox::information(dialog, dialog->tr("Test email"),
            dialog->tr("The server accepted the request to send a test email. "
                       "Check the recipient's inbox and the server log; delivery is not confirmed."));
    }, [dialog](const QString &) {
        if (!dialog || !dialog->m_testEmailPending) return;
        dialog->m_testEmailPending = false;
        dialog->setRemoteBusy(false);
        QMessageBox::warning(dialog, dialog->tr("Test email"),
            dialog->tr("The test email request could not be confirmed by the server."));
    });
}

void RemotePreferencesDialog::bindBackupAndWatchedFolders()
{
    bindRemoteControl("torrent_files_backup_enabled", m_ui->backupDirCheckBox);
    bindRemotePath("torrent_files_backup_dir", m_ui->backupDirPathEdit, [this]() {
        return remoteFieldAvailable("torrent_files_backup_enabled") && m_ui->backupDirCheckBox->isChecked();
    });
    bindRemoteControl("torrent_files_finished_backup_dir_enabled", m_ui->finishedBackupDirCheckBox);
    bindRemotePath("torrent_files_finished_backup_dir", m_ui->finishedBackupDirPathEdit, [this]() {
        return remoteFieldAvailable("torrent_files_finished_backup_dir_enabled")
            && m_ui->finishedBackupDirCheckBox->isChecked();
    });
    bindRemoteControl("remove_torrent_file_backup", m_ui->removeBackupTorrentFileCheckBox, {},
        tr("Remove the server's backup .torrent metadata when a torrent is removed."));

    // Legacy APIs encode both the enable flag and destination in one
    // string: empty disables copying, a nonempty path enables it.
    // Reuse the same native controls and mapped browse action. The modern
    // fields and legacy pair are mutually exclusive for each destination.
    const auto bindLegacyExport = [this](const char *key, QCheckBox *enabled, QLineEdit *path) {
        bindRemoteField(RemoteField{
            QString::fromLatin1(key), enabled,
            [enabled, path]() -> QJsonValue {
                if (!enabled->isChecked()) return QJsonValue(QString());
                // Distinguish a checked empty destination from disabling.
                if (path->text().trimmed().isEmpty()) return QJsonValue(QJsonValue::Null);
                return QJsonValue(path->text());
            },
            [enabled, path](const QJsonValue &value) {
                const QString directory = value.toString();
                enabled->setChecked(!directory.isEmpty());
                path->setText(directory);
            },
            [](const QJsonValue &value) { return value.isString(); },
            {}, tr("Legacy server export: uncheck and Apply to clear the server's saved destination.")
        });
        // The checkbox and path already have change connections via the
        // modern bindings, including the existing mapped Browse action.
    };
    bindLegacyExport("export_dir", m_ui->backupDirCheckBox, m_ui->backupDirPathEdit);
    bindLegacyExport("export_dir_fin", m_ui->finishedBackupDirCheckBox, m_ui->finishedBackupDirPathEdit);

    auto *view = m_ui->scanFoldersView;
    m_watchedFoldersModel = new QStandardItemModel(this);
    view->setModel(m_watchedFoldersModel);
    view->setRootIsDecorated(false);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    bindRemoteField(RemoteField{
        QStringLiteral("scan_dirs"), view,
        [this]() { return QJsonValue(m_watchedFolders); },
        [this](const QJsonValue &value) { m_watchedFolders = value.toObject(); displayWatchedFolders(); },
        [](const QJsonValue &value) {
            if (!value.isObject()) return false;
            const auto folders = value.toObject();
            const auto validPath = [](const QString &path) {
                return path.startsWith(QLatin1Char('/')) && !path.contains(QLatin1Char('\\'))
                    && !path.contains(QLatin1Char('\n')) && !path.contains(QLatin1Char('\r'))
                    && !path.contains(QChar::Null);
            };
            for (auto it = folders.begin(); it != folders.end(); ++it)
                if (!validPath(it.key()) || !(it.value() == QJsonValue(0) || it.value() == QJsonValue(1)
                    || (it.value().isString() && validPath(it.value().toString()))))
                    return false;
            return true;
        }, {},
        tr("Server watched folders. The Web API exposes only monitored directories and save destinations.")
    });
    displayWatchedFolders();
    connect(view->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, [this]() { refreshWatchedFolderButtons(); });
    connect(m_ui->addWatchedFolderButton, &QPushButton::clicked, this, [this]() { editWatchedFolder(false); });
    connect(m_ui->editWatchedFolderButton, &QPushButton::clicked, this, [this]() { editWatchedFolder(true); });
    connect(view, &QTreeView::doubleClicked, this, [this](const QModelIndex &) { editWatchedFolder(true); });
    connect(m_ui->removeWatchedFolderButton, &QPushButton::clicked, this, [this]() {
        if (!m_ui->removeWatchedFolderButton->isEnabled()) return;
        const auto selected = m_ui->scanFoldersView->selectionModel()->selectedRows();
        for (const auto &index : selected)
            m_watchedFolders.remove(index.data().toString());
        displayWatchedFolders();
        remoteFieldChanged();
    });
}

void RemotePreferencesDialog::refreshBackupControls()
{
    // A legacy field and its modern counterpart share the checkbox. Resolve
    // their final enabled state here instead of allowing iteration order
    // to disable a valid binding. The modern path remains independently
    // capability-gated.
    const auto refresh = [this](const char *legacy, const char *modernEnabled,
        const char *modernPath, QCheckBox *box, QLineEdit *line,
        const QString &modernLabel, const QString &legacyLabel) {
        const bool old = remoteFieldAvailable(legacy);
        const bool enabled = remoteFieldAvailable(modernEnabled);
        const bool pathAvailable = remoteFieldAvailable(modernPath);
        box->setText(old ? legacyLabel : modernLabel);
        box->setEnabled(old || enabled);
        line->setEnabled((old || (enabled && pathAvailable)) && box->isChecked());
        if (old)
        {
            box->setToolTip(tr("Legacy server export: disabling and applying clears the saved server destination."));
            line->setToolTip(tr("Absolute directory on the connected server, or choose a mapped local NAS directory."));
        }
        else if (enabled)
            box->setToolTip(QString());
    };
    refresh("export_dir", "torrent_files_backup_enabled", "torrent_files_backup_dir",
        m_ui->backupDirCheckBox, m_ui->backupDirPathEdit,
        tr("Store backup in:"), tr("Copy .torrent files to:"));
    refresh("export_dir_fin", "torrent_files_finished_backup_dir_enabled", "torrent_files_finished_backup_dir",
        m_ui->finishedBackupDirCheckBox, m_ui->finishedBackupDirPathEdit,
        tr("When torrent finished move backup to:"), tr("Copy .torrent files for finished downloads to:"));
}

void RemotePreferencesDialog::displayWatchedFolders()
{
    m_watchedFoldersModel->clear();
    m_watchedFoldersModel->setHorizontalHeaderLabels({tr("Watched Folder"), tr("Download destination")});
    for (auto it = m_watchedFolders.begin(); it != m_watchedFolders.end(); ++it)
    {
        const auto value = it.value();
        const QString destination = value == QJsonValue(0) ? tr("Watched folder")
            : value == QJsonValue(1) ? tr("Default save path") : value.toString();
        m_watchedFoldersModel->appendRow({new QStandardItem(it.key()), new QStandardItem(destination)});
    }
    refreshWatchedFolderButtons();
}

void RemotePreferencesDialog::refreshWatchedFolderButtons()
{
    if (!m_watchedFoldersModel) return;
    const bool available = remoteFieldAvailable("scan_dirs") && !m_remoteBusy;
    const int count = m_ui->scanFoldersView->selectionModel()->selectedRows().size();
    m_ui->addWatchedFolderButton->setEnabled(available);
    m_ui->editWatchedFolderButton->setEnabled(available && count == 1);
    m_ui->removeWatchedFolderButton->setEnabled(available && count > 0);
    for (auto *button : {m_ui->addWatchedFolderButton, m_ui->editWatchedFolderButton, m_ui->removeWatchedFolderButton})
        button->setToolTip(available ? tr("Only server paths and download destinations are available through the Web API.")
            : tr("Watched folder settings are unavailable from the selected server."));
}

void RemotePreferencesDialog::editWatchedFolder(bool existing)
{
    if (!remoteFieldAvailable("scan_dirs") || m_remoteBusy) return;
    const auto selected = m_ui->scanFoldersView->selectionModel()->selectedRows();
    if (existing && selected.size() != 1) return;
    const QString original = existing ? selected.first().data().toString() : QString();
    const QJsonValue previous = existing ? m_watchedFolders.value(original) : QJsonValue(1);

    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("watchedFolderOptionsDialog"));
    dialog.setWindowTitle(existing ? tr("Watched folder options") : tr("Select folder to monitor"));
    auto *layout = new QFormLayout(&dialog);
    auto *folder = new QLineEdit(original, &dialog);
    folder->setObjectName(QStringLiteral("watchedFolderPath"));
    folder->setReadOnly(existing);
    auto *mode = new QComboBox(&dialog);
    mode->setObjectName(QStringLiteral("watchedFolderDestinationMode"));
    mode->addItem(tr("Default save path"), 1);
    mode->addItem(tr("Watched folder"), 0);
    mode->addItem(tr("Custom save path"), 2);
    auto *destination = new QLineEdit(previous.toString(), &dialog);
    destination->setObjectName(QStringLiteral("watchedFolderDestinationPath"));
    const auto addBrowse = [this](QLineEdit *line) {
        auto *action = line->addAction(style()->standardIcon(QStyle::SP_DirOpenIcon), QLineEdit::TrailingPosition);
        action->setToolTip(tr("Choose mapped local NAS directory"));
        connect(action, &QAction::triggered, this, [this, line]() {
            if (line->isEnabled() && !line->isReadOnly()) browseRemotePath(line);
        });
    };
    if (!existing) addBrowse(folder);
    addBrowse(destination);
    mode->setCurrentIndex(previous == QJsonValue(1) ? 0 : previous == QJsonValue(0) ? 1 : 2);
    // In upstream's deprecated setter, 1 does not clear an existing custom
    // save path. Never silently promise a reset the API cannot perform.
    if (existing && previous != QJsonValue(1))
    {
        auto *model = qobject_cast<QStandardItemModel *>(mode->model());
        model->item(0)->setEnabled(false);
        mode->setToolTip(tr("The server's legacy Web API cannot reset an existing explicit destination to the default save path."));
    }
    destination->setEnabled(mode->currentData().toInt() == 2);
    connect(mode, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog,
        [mode, destination]() { destination->setEnabled(mode->currentData().toInt() == 2); });
    layout->addRow(tr("Watched folder:"), folder);
    layout->addRow(tr("Download destination:"), mode);
    layout->addRow(tr("Custom save path:"), destination);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    const auto cleanPath = [](QString path) {
        while (path.size() > 1 && path.endsWith(QLatin1Char('/'))) path.chop(1);
        return path;
    };
    const auto valid = [](const QString &path) {
        return path.startsWith(QLatin1Char('/')) && !path.contains(QLatin1Char('\\'))
            && !path.contains(QLatin1Char('\n')) && !path.contains(QLatin1Char('\r'))
            && !path.contains(QChar::Null)
            && QDir::cleanPath(path) == path;
    };
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
        const QString path = existing ? original : cleanPath(folder->text());
        const QString custom = cleanPath(destination->text());
        const int choice = mode->currentData().toInt();
        if ((!existing && (!valid(path) || m_watchedFolders.contains(path)))
            || (choice == 2 && !valid(custom)))
        {
            QMessageBox::warning(&dialog, tr("Invalid server directory"),
                tr("Enter an absolute POSIX directory without relative components, and do not duplicate an existing watched folder."));
            return;
        }
        if (existing && previous != QJsonValue(1) && choice == 1) return;
        if (!remoteFieldAvailable("scan_dirs")) { dialog.reject(); return; }
        m_watchedFolders.insert(path, choice == 2
            ? (custom == path ? QJsonValue(0) : QJsonValue(custom)) : QJsonValue(choice));
        dialog.accept();
    });
    if (dialog.exec() == QDialog::Accepted)
    {
        displayWatchedFolders();
        remoteFieldChanged();
    }
}

void RemotePreferencesDialog::bindRemotePath(const char *key, QLineEdit *line,
    std::function<bool()> dependency)
{
    // The text always represents a server path. Browsing is explicitly a
    // translation from a locally mounted directory, not remote filesystem I/O.
    bindRemoteControl(key, line, std::move(dependency),
        tr("Absolute directory on the connected server. Type a server path, "
           "or use the folder button to select a mapped local NAS directory."));
    auto *browse = line->addAction(style()->standardIcon(QStyle::SP_DirOpenIcon),
                                   QLineEdit::TrailingPosition);
    browse->setObjectName(line->objectName() + QStringLiteral("Browse"));
    browse->setToolTip(tr("Choose mapped local NAS directory"));
    connect(browse, &QAction::triggered, this, [this, line]() {
        if (line->isEnabled())
            browseRemotePath(line);
    });
}

void RemotePreferencesDialog::browseRemotePath(QLineEdit *line, bool filterFile)
{
    // The Servers page selector edits local mappings only. Always use the
    // authenticated profile captured when this Preferences dialog opened.
    const auto entries = m_staged.contains(m_remoteProfileId)
        ? m_staged.value(m_remoteProfileId) : m_mappingStore.mappings(m_remoteProfileId);
    if (entries.isEmpty())
    {
        QMessageBox::information(this, tr("No path mappings"),
            tr("Configure a local NAS mount in Preferences → Servers first, "
               "or type the absolute server path manually."));
        return;
    }
    QString initial = Remote::PathMappingStore::toLocal(line->text(), entries);
    if (initial.isEmpty() || (!QDir(initial).exists() && !(filterFile && QFileInfo(initial).isFile())))
        initial = entries.constFirst().localRoot;
    const QString local = filterFile
        ? QFileDialog::getOpenFileName(this, tr("Choose an IP filter file"), initial,
            tr("All supported filters (*.dat *.p2p *.p2b);;.dat (*.dat);;.p2p (*.p2p);;.p2b (*.p2b)"))
        : QFileDialog::getExistingDirectory(this, tr("Choose mapped local NAS directory"), initial);
    if (local.isEmpty())
        return;
    const QString remote = Remote::PathMappingStore::toRemote(local, entries);
    if (remote.isEmpty())
    {
        QMessageBox::warning(this, filterFile ? tr("Unmapped local file") : tr("Unmapped local directory"),
            tr("The selected path is not covered by a mapping for this connected server. "
               "The server path has not been changed."));
        return;
    }
    line->setText(remote);
}

void RemotePreferencesDialog::bindDownloadsSavingControls()
{
    bindRemoteControl("preallocate_all", m_ui->checkPreallocateAll, {},
        tr("Pre-allocate files on the connected server's disk before downloading."));
    bindRemoteControl("incomplete_files_ext", m_ui->checkAppendqB, {},
        tr("The server appends .!qB to incomplete files. Changing this may rename existing incomplete files."));
    bindRemoteControl("use_unwanted_folder", m_ui->checkUnwantedFolder, {},
        tr("Keep unselected files in the server's .unwanted folder."));

    bindBooleanCombo("auto_tmm_enabled", m_ui->comboSavingMode, 1,
        tr("Default management mode for newly added torrents on the server."));
    bindBooleanCombo("torrent_changed_tmm_enabled", m_ui->comboTorrentCategoryChanged, 0,
        tr("When a torrent category changes, relocate it or switch it to Manual Mode."));
    bindBooleanCombo("save_path_changed_tmm_enabled", m_ui->comboCategoryDefaultPathChanged, 0,
        tr("When a default save or incomplete path changes, relocate affected torrents "
           "or switch them to Manual Mode."));
    bindBooleanCombo("category_changed_tmm_enabled", m_ui->comboCategoryChanged, 0,
        tr("When a category save path changes, relocate affected torrents "
           "or switch them to Manual Mode."));
    bindRemoteControl("use_category_paths_in_manual_mode", m_ui->checkUseCategoryPaths, {},
        tr("Resolve relative save paths against the category path instead of the default path."));
    bindRemotePath("save_path", m_ui->textSavePath);
    bindRemoteControl("temp_path_enabled", m_ui->checkUseDownloadPath);
    bindRemotePath("temp_path", m_ui->textDownloadPath, [this]() {
        return remoteFieldAvailable("temp_path_enabled") && m_ui->checkUseDownloadPath->isChecked();
    });
}

bool RemotePreferencesDialog::validateDownloadChanges(const QJsonObject &changes)
{
    // A saved program is executed by the NAS on future torrent events.
    // Confirm all command edits conservatively, even if currently disabled:
    // another client may have enabled the hook since the preferences read.
    const bool commandEdit = changes.contains(QStringLiteral("autorun_on_torrent_added_program"))
        || changes.contains(QStringLiteral("autorun_program"));
    const bool enableAdded = changes.value(QStringLiteral("autorun_on_torrent_added_enabled")).toBool();
    const bool enableFinished = changes.value(QStringLiteral("autorun_enabled")).toBool();
    if ((enableAdded && (!remoteFieldAvailable("autorun_on_torrent_added_program")
            || m_ui->lineEditRunOnAdded->text().trimmed().isEmpty()))
        || (enableFinished && (!remoteFieldAvailable("autorun_program")
            || m_ui->lineEditRunOnFinished->text().trimmed().isEmpty())))
    {
        QMessageBox::warning(this, tr("Missing server command"),
            tr("Enter a supported, non-empty server command before enabling an external program."));
        return false;
    }
    if ((commandEdit || enableAdded || enableFinished)
        && QMessageBox::question(this, tr("Change server external programs"),
            tr("These commands will run on the connected server when the configured torrent events "
               "occur, even when this application is closed. Verify the commands and server paths. "
               "Apply these changes?"), QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
            != QMessageBox::Yes)
        return false;

    // This client supports absolute POSIX NAS paths. Do not resolve relative
    // text against the desktop filesystem, guess a mapping, or require a mount.
    for (const QString &key : {QStringLiteral("save_path"), QStringLiteral("temp_path"),
         QStringLiteral("torrent_files_backup_dir"), QStringLiteral("torrent_files_finished_backup_dir"),
         QStringLiteral("export_dir"), QStringLiteral("export_dir_fin")})
    {
        if (!changes.contains(key))
            continue;
        const QJsonValue value = changes.value(key);
        const QString path = value.toString();
        const bool legacyExport = key == QLatin1String("export_dir") || key == QLatin1String("export_dir_fin");
        if (legacyExport && value.isString() && path.isEmpty())
            continue; // Unchecked: disable the legacy copy destination.
        if (!path.startsWith(QLatin1Char('/')) || path.contains(QLatin1Char('\\'))
            || path.contains(QLatin1Char('\n')) || path.contains(QLatin1Char('\r'))
            || path.contains(QChar::Null))
        {
            QMessageBox::warning(this, tr("Invalid server directory"),
                tr("Enter an absolute POSIX directory on the connected server, "
                   "or choose a mapped local NAS directory."));
            return false;
        }
    }
    if (changes.contains(QStringLiteral("scan_dirs")))
    {
        QJsonObject oldFolders;
        for (const auto &field : m_remoteFields)
            if (field.key == QLatin1String("scan_dirs")) oldFolders = field.baseline.toObject();
        const auto newFolders = changes.value(QStringLiteral("scan_dirs")).toObject();
        for (auto it = newFolders.begin(); it != newFolders.end(); ++it)
        {
            if (oldFolders.contains(it.key()) && oldFolders.value(it.key()) != QJsonValue(1)
                && it.value() == QJsonValue(1))
            {
                QMessageBox::warning(this, tr("Unsupported watched folder change"),
                    tr("The legacy server API cannot reset an existing explicit destination to the default save path."));
                return false;
            }
        }
    }
    if (changes.contains(QStringLiteral("scan_dirs"))
        && QMessageBox::question(this, tr("Change watched folders"),
            tr("The selected server will start monitoring new folders and stop monitoring removed folders. "
               "Existing .torrent files in monitored folders may be added automatically. "
               "Only folder paths and save destinations are exposed by the Web API. Apply these changes?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return false;

    // Upstream's path setters can relocate Auto TMM torrents or switch
    // them to Manual Mode. Existing Manual Mode torrents retain their paths;
    // use_category_paths_in_manual_mode only changes suggested paths.
    if (!changes.contains(QStringLiteral("save_path"))
        && !changes.contains(QStringLiteral("temp_path"))
        && !changes.contains(QStringLiteral("temp_path_enabled")))
        return true;

    // Reuse the captured server's synchronized, unfiltered torrent model.
    // Unknown or incomplete state is not proof that all torrents are manual.
    if (m_torrentModel && m_torrentModel->hasFullSync())
    {
        bool allManual = true;
        const auto &torrents = m_torrentModel->torrents();
        for (const auto &torrent : torrents)
        {
            const auto mode = torrent.value(QStringLiteral("auto_tmm"));
            if (!mode.isBool() || mode.toBool())
            {
                allManual = false;
                break;
            }
        }
        if (allManual)
            return true;
    }

    return QMessageBox::question(this, tr("Change server download locations"),
        tr("These settings affect the connected server. Depending on its torrent "
           "management policies, existing torrents may be relocated or switched "
           "to Manual Mode. The server performs those changes, even if this "
           "application is closed. Apply these settings?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
}

bool RemotePreferencesDialog::remoteFieldAvailable(const char *key) const
{
    for (const auto &field : m_remoteFields)
        if (field.key == QLatin1String(key))
            return m_remotePreferencesLoaded && !field.baseline.isUndefined();
    return false;
}

void RemotePreferencesDialog::refreshRemoteControls()
{
    for (const auto &field : m_remoteFields)
    {
        const bool supported = m_remotePreferencesLoaded && !field.baseline.isUndefined();
        field.widget->setEnabled(supported && (!field.dependency || field.dependency()));
        field.widget->setToolTip(supported ? field.tooltip : tr(
            "This setting is unavailable from the connected server. Displayed values are not live."));
        if (auto *combo = qobject_cast<QComboBox *>(field.widget))
            combo->view()->setEnabled(supported);
    }
    m_ui->stopConditionLabel->setEnabled(m_ui->stopConditionComboBox->isEnabled());
    refreshWatchedFolderButtons();
    refreshNotificationControls();
    refreshBackupControls();
    refreshConnectionLimitControls();
    refreshI2PControls();
    refreshProxyControls();
    const bool portAvailable = remoteFieldAvailable("listen_port");
    m_ui->randomButton->setEnabled(portAvailable);
    m_ui->randomButton->setToolTip(portAvailable
        ? tr("Choose a random port between 1024 and 65535. Apply to change the server.")
        : tr("The connected server does not expose a supported listening port."));
}

void RemotePreferencesDialog::populateRemoteFields(const QJsonObject &values,
                                                    const QJsonObject &staged)
{
    m_loadingRemotePreferences = true;
    m_remotePreferencesLoaded = true;
    for (auto &field : m_remoteFields)
    {
        QJsonValue value = normalizedConnectionLimit(field.key, values.value(field.key));
        // Prefer modern keys if the server advertises them, even on a
        // transitional server exposing deprecated export aliases as well.
        if ((field.key == QLatin1String("export_dir")
                && (values.contains(QStringLiteral("torrent_files_backup_enabled"))
                    || values.contains(QStringLiteral("torrent_files_backup_dir"))))
            || (field.key == QLatin1String("export_dir_fin")
                && (values.contains(QStringLiteral("torrent_files_finished_backup_dir_enabled"))
                    || values.contains(QStringLiteral("torrent_files_finished_backup_dir")))))
            value = QJsonValue(QJsonValue::Undefined);
        field.baseline = field.accepts(value) ? value : QJsonValue(QJsonValue::Undefined);
        if (!field.baseline.isUndefined())
        {
            field.write(value);
            // Reject any value the widget cannot faithfully represent.
            if (field.read() != value)
                field.baseline = QJsonValue(QJsonValue::Undefined);
            else if (staged.contains(field.key) && field.accepts(staged.value(field.key)))
                field.write(staged.value(field.key));
        }
    }
    m_loadingRemotePreferences = false;
    refreshRemoteControls();
    updateButtons();
}

QJsonObject RemotePreferencesDialog::remoteChanges() const
{
    QJsonObject changes;
    const auto withoutTrailingSlash = [](QString path) {
        while (path.size() > 1 && path.endsWith(QLatin1Char('/')))
            path.chop(1);
        return path;
    };
    for (const auto &field : m_remoteFields)
    {
        if (field.baseline.isUndefined())
            continue;
        const auto value = field.read();
        if (value == field.baseline)
            continue;
        if ((field.key == QLatin1String("save_path") || field.key == QLatin1String("temp_path")
            || field.key == QLatin1String("torrent_files_backup_dir")
            || field.key == QLatin1String("torrent_files_finished_backup_dir")
            || field.key == QLatin1String("export_dir") || field.key == QLatin1String("export_dir_fin"))
            && value.isString() && field.baseline.isString()
            && value.toString().startsWith(QLatin1Char('/'))
            && field.baseline.toString().startsWith(QLatin1Char('/'))
            && withoutTrailingSlash(value.toString()) == withoutTrailingSlash(field.baseline.toString()))
            continue;
        changes.insert(field.key, value);
    }
    return changes;
}

void RemotePreferencesDialog::loadRemotePreferences()
{
    if (!m_remoteSession || !m_remoteSession->isConnected())
    {
        refreshRemoteControls();
        return;
    }
    connect(m_remoteSession, &Remote::RemoteSession::failed,
            this, [this](const QString &) { remoteDisconnected(); });
    connect(m_remoteSession, &Remote::RemoteSession::disconnected,
            this, &RemotePreferencesDialog::remoteDisconnected);
    connect(m_remoteSession, &QObject::destroyed,
            this, &RemotePreferencesDialog::remoteDisconnected);
    const QPointer<RemotePreferencesDialog> dialog(this);
    m_remoteSession->requestPreferences(
        [dialog](const QJsonObject &values) {
            if (dialog)
            {
                dialog->populateRemoteFields(values);
                dialog->readI2PBuildInfo();
            }
        },
        [dialog](const QString &reason) {
            if (!dialog)
                return;
            dialog->refreshRemoteControls();
            QMessageBox::warning(dialog, dialog->tr("Cannot read server preferences"), reason);
        });
}

void RemotePreferencesDialog::setRemoteBusy(bool busy)
{
    m_remoteBusy = busy;
    m_ui->tabOption->setEnabled(!busy);
    m_ui->tabSelection->setEnabled(!busy);
    m_ui->buttonBox->setEnabled(!busy);
    refreshWatchedFolderButtons();
    updateButtons();
}

void RemotePreferencesDialog::remoteDisconnected()
{
    m_remoteTargetLabel->setText(tr("Server settings: %1 (disconnected)")
        .arg(m_remoteTargetName.isEmpty() ? tr("Server") : m_remoteTargetName));
    m_remotePreferencesLoaded = false;
    refreshRemoteControls();
    if (m_testEmailPending)
    {
        m_testEmailPending = false;
        setRemoteBusy(false);
        QMessageBox::warning(this, tr("Test email"),
            tr("The server disconnected. The test email request could not be confirmed."));
    }
    else if (m_remoteBusy)
        remoteSaveFailed(tr("The selected server disconnected. The update could not be confirmed."));
    else
        updateButtons();
}

void RemotePreferencesDialog::remoteSaveFailed(const QString &reason)
{
    m_closeAfterApply = false;
    setRemoteBusy(false);
    QMessageBox::warning(this, tr("Cannot confirm server preferences"), reason);
}

void RemotePreferencesDialog::verifyRemoteChanges(const QJsonObject &changes)
{
    if (!m_remoteSession || !m_remoteSession->isConnected())
    {
        remoteDisconnected();
        return;
    }
    const QPointer<RemotePreferencesDialog> dialog(this);
    m_remoteSession->requestPreferences(
        [dialog, changes](const QJsonObject &values) {
            if (!dialog || !dialog->m_remoteBusy)
                return;
            QJsonObject unapplied;
            for (auto it = changes.constBegin(); it != changes.constEnd(); ++it)
            {
                const QJsonValue returned = normalizedConnectionLimit(it.key(), values.value(it.key()));
                // qBittorrent can canonicalize directory spelling (for
                // example, remove a trailing slash). Accept equivalent POSIX
                // paths without claiming that the remote filesystem exists.
                const bool pathKey = it.key() == QLatin1String("save_path")
                    || it.key() == QLatin1String("temp_path")
                    || it.key() == QLatin1String("torrent_files_backup_dir")
                    || it.key() == QLatin1String("torrent_files_finished_backup_dir")
                    || it.key() == QLatin1String("export_dir") || it.key() == QLatin1String("export_dir_fin");
                const bool equivalentPath = pathKey && returned.isString() && it.value().isString()
                    && returned.toString().startsWith(QLatin1Char('/'))
                    && it.value().toString().startsWith(QLatin1Char('/'))
                    && QDir::cleanPath(returned.toString()) == QDir::cleanPath(it.value().toString());
                if (returned != it.value() && !equivalentPath)
                    unapplied.insert(it.key(), it.value());
            }
            // Use authoritative read-back as the new baseline. Keep any
            // representable, unconfirmed edits staged for an explicit retry.
            dialog->populateRemoteFields(values, unapplied);
            if (!unapplied.isEmpty())
            {
                dialog->remoteSaveFailed(dialog->tr(
                    "The server did not confirm every requested value. "
                    "Some settings may have changed; review the remaining edits before retrying."));
                return;
            }
            dialog->setRemoteBusy(false);
            dialog->finishApply();
        },
        [dialog](const QString &reason) {
            if (dialog && dialog->m_remoteBusy)
                dialog->remoteSaveFailed(dialog->tr(
                    "The write was accepted but its result could not be read back. "
                    "Some settings may already have changed. %1").arg(reason));
        });
}

void RemotePreferencesDialog::populateAdvancedPlaceholders()
{
    // Upstream qBittorrent's Designer leaves Advanced empty and populates
    // this table in advancedsettings.cpp. Retain every named setting from
    // that original list, with non-editable grey values until supported by
    // the authenticated remote Web API.
    auto *notice = new QLabel(tr(
        "Advanced server settings are not yet connected to the Web API. "
        "These labels are shown for functional parity tracking, not as "
        "current server values."), m_ui->tabAdvancedPage);
    notice->setWordWrap(true);
    m_ui->advPageLayout->addWidget(notice);

    auto *table = new QTreeWidget(m_ui->tabAdvancedPage);
    table->setObjectName(QStringLiteral("advancedSettingsPreview"));
    table->setColumnCount(2);
    table->setHeaderLabels({tr("Setting"), tr("Value")});
    table->setRootIsDecorated(false);
    table->setAlternatingRowColors(true);
    table->setUniformRowHeights(true);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->header()->setStretchLastSection(true);
    table->header()->setHighlightSections(false);
    table->header()->resizeSection(0, 415);
    const QStringList upstreamSettings {
        tr("qBittorrent Section"),
        tr("libtorrent Section"),
        tr("Resume data storage type (requires restart)"),
        tr("Torrent content removing mode"),
        tr("Physical memory (RAM) usage limit"),
        tr("Process memory priority"),
        tr("Bdecode depth limit"),
        tr("Bdecode token limit"),
        tr("Maximum directory depth when loading torrents"),
        tr("Asynchronous I/O threads"),
        tr("Hashing threads"),
        tr("File pool size"),
        tr("Outstanding memory when checking torrents"),
        tr("Disk cache"),
        tr("Disk cache expiry interval"),
        tr("Disk queue size"),
        tr("Disk IO type (requires restart)"),
        tr("Disk IO read mode"),
        tr("Disk IO write mode"),
        tr("Coalesce reads & writes"),
        tr("Use piece extent affinity"),
        tr("Send upload piece suggestions"),
        tr("Send buffer watermark"),
        tr("Send buffer low watermark"),
        tr("Send buffer watermark factor"),
        tr("Outgoing connections per second"),
        tr("Allow outgoing connections when seeding"),
        tr("Socket send buffer size [0: system default]"),
        tr("Socket receive buffer size [0: system default]"),
        tr("Socket backlog size"),
        tr("Save resume data interval [0: disabled]"),
        tr("Save statistics interval [0: disabled]"),
        tr(".torrent file size limit"),
        tr("Outgoing ports (Min) [0: disabled]"),
        tr("Outgoing ports (Max) [0: disabled]"),
        tr("UPnP lease duration [0: permanent lease]"),
        tr("Differentiated Services Code Point (DSCP) for connections to peers"),
        tr("uTP-TCP mixed mode algorithm"),
        tr("Internal hostname resolver cache expiry interval"),
        tr("Support internationalized domain name (IDN)"),
        tr("Allow multiple connections from the same IP address"),
        tr("Allow multiple connections from the same Peer ID"),
        tr("Validate HTTPS tracker certificates"),
        tr("Server-side request forgery (SSRF) mitigation"),
        tr("Disallow connection to peers on privileged ports"),
        tr("Recheck torrents on completion"),
        tr("Customize application instance name"),
        tr("Refresh interval"),
        tr("Resolve peer countries"),
        tr("Resolve peer host names"),
        tr("Network interface"),
        tr("Optional IP address to bind to"),
        tr("IP address reported to trackers (requires restart)"),
        tr("Port reported to trackers (requires restart) [0: listening port]"),
        tr("Max concurrent HTTP announces"),
        tr("Stop tracker timeout [0: disabled]"),
        tr("Display notifications"),
        tr("Display notifications for added torrents"),
        tr("Notification timeout [0: infinite, -1: system default]"),
        tr("Reannounce to all trackers when IP or port changed"),
        tr("Download tracker's favicon"),
        tr("Save path history length"),
        tr("Enable speed graphs"),
        tr("Enable icons in menus"),
        tr("Attach \"Add new torrent\" dialog to main window"),
        tr("Enable embedded tracker"),
        tr("Embedded tracker port"),
        tr("Enable port forwarding for embedded tracker"),
        tr("Ignore SSL errors"),
        tr("Python executable path (may require restart)"),
        tr("Start BitTorrent session in paused state"),
        tr("BitTorrent session shutdown timeout [-1: unlimited]"),
        tr("Upload slots behavior"),
        tr("Upload choking algorithm"),
        tr("Confirm torrent recheck"),
        tr("Confirm removal of all tags"),
        tr("Confirm removal of tracker from all torrents"),
        tr("Always announce to all trackers in a tier"),
        tr("Always announce to all tiers"),
        tr("Peer turnover disconnect percentage"),
        tr("Peer turnover threshold percentage"),
        tr("Peer turnover disconnect interval"),
        tr("Maximum outstanding requests to a single peer"),
        tr("Maximum outstanding block requests from a peer"),
        tr("DHT bootstrap nodes"),
        tr("STUN server for WebTorrent NAT traversal"),
        tr("I2P inbound quantity"),
        tr("I2P outbound quantity"),
        tr("I2P inbound length"),
        tr("I2P outbound length"),
        tr("I2P inbound length variance"),
        tr("I2P outbound length variance")
    };
    const QColor disabledText = palette().color(QPalette::Disabled, QPalette::Text);
    for (int i = 0; i < upstreamSettings.size(); ++i)
    {
        const bool sectionHeader = i < 2;
        auto *item = new QTreeWidgetItem(table, {
            upstreamSettings.at(i),
            sectionHeader ? QString() : tr("Not implemented")
        });
        item->setFlags(Qt::ItemIsEnabled);
        if (sectionHeader)
        {
            QFont bold = font();
            bold.setBold(true);
            item->setFont(0, bold);
        }
        else
        {
            item->setForeground(0, disabledText);
            item->setForeground(1, disabledText);
        }
    }
    m_ui->advPageLayout->addWidget(table, 1);
}

void RemotePreferencesDialog::buildRemotePathsPage(const QString &preferredProfileId)
{
    auto *page = new QWidget(m_ui->tabOption);
    page->setObjectName(QStringLiteral("tabRemotePathsPage"));
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(6, 6, 6, 6);
    outer->setSpacing(6);

    auto *group = new QGroupBox(tr("Server and Remote Paths"), page);
    auto *groupLayout = new QVBoxLayout(group);
    groupLayout->setSpacing(6);
    auto *form = new QFormLayout;
    m_serverSelector = new QComboBox(group);
    m_serverSelector->setObjectName(QStringLiteral("remotePathsServer"));
    form->addRow(tr("Saved server:"), m_serverSelector);
    groupLayout->addLayout(form);

    auto *intro = new QLabel(tr(
        "Map local NAS mounts to directories on the selected server. "
        "Each server has its own mappings."), group);
    intro->setWordWrap(true);
    groupLayout->addWidget(intro);

    m_mappings = new QTableWidget(0, 2, group);
    m_mappings->setObjectName(QStringLiteral("remotePathMappings"));
    m_mappings->setHorizontalHeaderLabels({tr("Local mounted directory"),
                                            tr("Remote server directory")});
    m_mappings->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_mappings->horizontalHeader()->setHighlightSections(false);
    m_mappings->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_mappings->setSelectionMode(QAbstractItemView::SingleSelection);
    m_mappings->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_mappings->setAlternatingRowColors(true);
    groupLayout->addWidget(m_mappings, 1);

    auto *row = new QHBoxLayout;
    m_addMapping = new QPushButton(tr("Add…"), group);
    m_addMapping->setObjectName(QStringLiteral("addPathMapping"));
    m_editMapping = new QPushButton(tr("Edit…"), group);
    m_editMapping->setObjectName(QStringLiteral("editPathMapping"));
    m_removeMapping = new QPushButton(tr("Remove"), group);
    m_removeMapping->setObjectName(QStringLiteral("removePathMapping"));
    m_checkMount = new QPushButton(tr("Check Local Mount"), group);
    m_checkMount->setObjectName(QStringLiteral("checkLocalMount"));
    for (QPushButton *button : {m_addMapping, m_editMapping,
                                 m_removeMapping, m_checkMount})
        row->addWidget(button);
    row->addStretch();
    groupLayout->addLayout(row);

    auto *note = new QLabel(tr(
        "Browse picks a local folder; enter the remote path manually. "
        "Mount check is local only."), group);
    note->setToolTip(tr(
        "A local mount check cannot verify the server filesystem. "
        "Filesystem mapping never alters or deletes any files."));
    note->setWordWrap(true);
    groupLayout->addWidget(note);
    outer->addWidget(group, 1);

    // Exactly one remote-client-specific page, shown BEFORE Behavior.
    // Keep the native nine sections and their relative order unchanged.
    m_ui->tabOption->insertWidget(0, page);
    m_ui->tabSelection->insertItem(0, tr("Servers"));

    QObject::connect(m_serverSelector,
                     QOverload<int>::of(&QComboBox::currentIndexChanged),
                     this, [this](int) { showServerMappings(); });
    QObject::connect(m_mappings, &QTableWidget::itemSelectionChanged,
                     this, [this]() { updateButtons(); });
    QObject::connect(m_addMapping, &QPushButton::clicked,
                     this, [this]() { editMapping(false); });
    QObject::connect(m_editMapping, &QPushButton::clicked,
                     this, [this]() { editMapping(true); });
    QObject::connect(m_removeMapping, &QPushButton::clicked,
                     this, [this]() { removeMapping(); });
    QObject::connect(m_checkMount, &QPushButton::clicked,
                     this, [this]() { checkLocalMount(); });
    QObject::connect(m_mappings, &QTableWidget::cellDoubleClicked,
                     this, [this](int, int) { editMapping(true); });

    populateServerSelector(preferredProfileId);
}

void RemotePreferencesDialog::populateServerSelector(const QString &preferredProfileId)
{
    const Remote::ProfileStore store(m_settings);
    const auto profiles = store.profiles();
    for (const auto &profile : profiles)
        m_serverSelector->addItem(profile.name, profile.id);

    const int index = m_serverSelector->findData(preferredProfileId);
    if (index >= 0)
        m_serverSelector->setCurrentIndex(index);
    else if (m_serverSelector->count() > 0)
        m_serverSelector->setCurrentIndex(0);
    else
        showServerMappings();
}

QString RemotePreferencesDialog::currentProfileId() const
{
    return m_serverSelector->currentData().toString();
}

QList<Remote::PathMapping> RemotePreferencesDialog::tableMappings() const
{
    QList<Remote::PathMapping> entries;
    for (int i = 0; i < m_mappings->rowCount(); ++i)
        entries.append({
            QDir::fromNativeSeparators(m_mappings->item(i, 0)->text()),
            m_mappings->item(i, 1)->text()
        });
    return entries;
}

void RemotePreferencesDialog::displayMappings(
    const QList<Remote::PathMapping> &entries)
{
    QSignalBlocker blocker(m_mappings);
    m_mappings->setRowCount(0);
    for (const Remote::PathMapping &entry : entries)
    {
        const int row = m_mappings->rowCount();
        m_mappings->insertRow(row);
        m_mappings->setItem(row, 0, new QTableWidgetItem(
            QDir::toNativeSeparators(entry.localRoot)));
        m_mappings->setItem(row, 1, new QTableWidgetItem(entry.remoteRoot));
    }
    m_mappings->clearSelection();
    updateButtons();
}

void RemotePreferencesDialog::showServerMappings()
{
    const QString profileId = currentProfileId();
    if (profileId.isEmpty())
        displayMappings({});
    else
        displayMappings(m_staged.contains(profileId)
                        ? m_staged.value(profileId)
                        : m_mappingStore.mappings(profileId));
    updateButtons();
}

void RemotePreferencesDialog::stageCurrentMappings()
{
    const QString profileId = currentProfileId();
    if (!profileId.isEmpty())
    {
        m_staged.insert(profileId, tableMappings());
        m_changedProfiles.insert(profileId);
    }
    updateButtons();
}

void RemotePreferencesDialog::editMapping(bool existing)
{
    const QString profileId = currentProfileId();
    if (profileId.isEmpty())
        return;

    QList<Remote::PathMapping> entries = tableMappings();
    const int selected = m_mappings->currentRow();
    if (existing && (selected < 0 || selected >= entries.size()))
        return;
    Remote::PathMapping value = existing
        ? entries.at(selected) : Remote::PathMapping{};
    if (!mappingEditor(this, value))
        return;

    if (existing)
        entries[selected] = value;
    else
        entries.append(value);
    QString error;
    if (!Remote::PathMappingStore::validate(entries, &error))
    {
        QMessageBox::warning(this, tr("Invalid filesystem mapping"), error);
        return;
    }
    displayMappings(entries);
    stageCurrentMappings();
}

void RemotePreferencesDialog::removeMapping()
{
    QList<Remote::PathMapping> entries = tableMappings();
    const int selected = m_mappings->currentRow();
    if (selected < 0 || selected >= entries.size())
        return;
    entries.removeAt(selected);
    displayMappings(entries);
    stageCurrentMappings();
}

void RemotePreferencesDialog::checkLocalMount()
{
    const int row = m_mappings->currentRow();
    if (row < 0)
        return;
    const QString path = m_mappings->item(row, 0)->text();
    const bool exists = QDir(path).exists();
    QMessageBox::information(this, tr("Local mount check"), exists
        ? tr("The local directory is currently accessible: %1\n"
             "This does not verify that the remote server can access its mapping.").arg(path)
        : tr("The local directory is not accessible: %1\n"
             "You can keep the mapping for a share that is mounted later.").arg(path));
}

void RemotePreferencesDialog::updateButtons()
{
    const bool hasProfile = !currentProfileId().isEmpty();
    const bool selected = hasProfile && m_mappings->currentRow() >= 0;
    m_addMapping->setEnabled(hasProfile);
    m_editMapping->setEnabled(selected);
    m_removeMapping->setEnabled(selected);
    m_checkMount->setEnabled(selected);
    if (m_applyButton)
        m_applyButton->setEnabled(!m_remoteBusy && (!m_changedProfiles.isEmpty()
                                  || m_behaviorDirty || m_additionDirty || !remoteChanges().isEmpty()));
    refreshNotificationControls();
}

void RemotePreferencesDialog::applyChanges(bool closeAfter)
{
    if (m_remoteBusy || !validateLocalChanges())
        return;
    m_closeAfterApply = closeAfter;
    const QJsonObject changes = remoteChanges();
    if (changes.isEmpty())
    {
        finishApply();
        return;
    }
    if (!m_remoteSession || !m_remoteSession->isConnected() || !m_remotePreferencesLoaded)
    {
        remoteSaveFailed(tr("The server is no longer available. Reopen Preferences after reconnecting."));
        return;
    }
    if (!validateDownloadChanges(changes) || !validateIPFilterChanges(changes))
    {
        m_closeAfterApply = false;
        return;
    }
    // A modal confirmation can run the event loop while a server disconnects.
    if (!m_remoteSession || !m_remoteSession->isConnected() || !m_remotePreferencesLoaded)
    {
        remoteSaveFailed(tr("The server is no longer available. Reopen Preferences after reconnecting."));
        return;
    }
    setRemoteBusy(true);
    const QPointer<RemotePreferencesDialog> dialog(this);
    // Replacement collections must not overwrite a newer list observed
    // on the server. The API has no atomic compare-and-set.
    QJsonObject replacementBaselines;
    for (const auto &field : m_remoteFields)
        if ((field.key == QLatin1String("scan_dirs") || field.key == QLatin1String("banned_IPs"))
            && changes.contains(field.key))
            replacementBaselines.insert(field.key, field.baseline);
    if (!replacementBaselines.isEmpty())
    {
        m_remoteSession->requestPreferences([dialog, changes, replacementBaselines](const QJsonObject &values) {
            if (!dialog || !dialog->m_remoteBusy) return;
            for (auto it = replacementBaselines.constBegin(); it != replacementBaselines.constEnd(); ++it)
            {
                if (values.value(it.key()) != it.value())
                {
                    dialog->remoteSaveFailed(it.key() == QLatin1String("scan_dirs")
                        ? dialog->tr("Watched folders changed on the server. Reopen Preferences to review the new list before editing.")
                        : dialog->tr("Banned IP addresses changed on the server. Reopen Preferences to review the new list before editing."));
                    return;
                }
            }
            dialog->sendRemoteChanges(changes);
        }, [dialog](const QString &reason) {
            if (dialog && dialog->m_remoteBusy) dialog->remoteSaveFailed(reason);
        });
        return;
    }
    sendRemoteChanges(changes);
}

void RemotePreferencesDialog::sendRemoteChanges(const QJsonObject &changes)
{
    if (!m_remoteSession || !m_remoteSession->isConnected())
    {
        remoteDisconnected();
        return;
    }
    const QPointer<RemotePreferencesDialog> dialog(this);
    m_remoteSession->setPreferences(changes,
        [dialog, changes]() {
            if (dialog && dialog->m_remoteBusy)
                dialog->verifyRemoteChanges(changes);
        },
        [dialog](const QString &reason) {
            if (dialog && dialog->m_remoteBusy)
                dialog->remoteSaveFailed(reason);
        });
}

bool RemotePreferencesDialog::validateLocalChanges()
{
    if (m_behaviorDirty && m_ui->windowStateComboBox->currentData().toString()
            == QLatin1String("hidden")
        && !(m_ui->checkShowSystray->isChecked() && QSystemTrayIcon::isSystemTrayAvailable()))
    {
        QMessageBox::warning(this, tr("Tray required"),
            tr("A hidden startup window requires an enabled, available system tray. "
               "Choose Normal or Minimized instead."));
        return false;
    }
    for (const QString &profileId : m_changedProfiles)
    {
        QString error;
        if (!Remote::PathMappingStore::validate(m_staged.value(profileId), &error))
        {
            QMessageBox::warning(this, tr("Cannot save Remote Paths"), error);
            return false;
        }
    }
    return true;
}

void RemotePreferencesDialog::finishApply()
{
    const bool closeAfter = m_closeAfterApply;
    m_closeAfterApply = false;
    if (applyLocalChanges() && closeAfter)
        accept();
}

bool RemotePreferencesDialog::applyLocalChanges()
{
    for (const QString &profileId : m_changedProfiles)
    {
        QString error;
        if (!m_mappingStore.replace(profileId, m_staged.value(profileId), &error))
        {
            QMessageBox::warning(this, tr("Cannot save Remote Paths"), error);
            return false;
        }
    }
    m_changedProfiles.clear();
    if (m_additionDirty)
    {
        m_settings.setValue(QStringLiteral("Preferences/Downloads/ShowAdditionDialog"),
            m_ui->checkAdditionDialog->isChecked());
        m_settings.setValue(QStringLiteral("Preferences/Downloads/AdditionDialogFront"),
            m_ui->checkAdditionDialogFront->isChecked());
        m_settings.setValue(QStringLiteral("Preferences/Downloads/ConfirmMergeTrackers"),
            m_ui->checkConfirmMergeTrackers->isChecked());
        m_settings.setValue(QStringLiteral("Preferences/Downloads/DeleteOriginalTorrent"),
            m_ui->deleteTorrentBox->isChecked());
        m_settings.setValue(QStringLiteral("Preferences/Downloads/DeleteCancelledTorrent"),
            m_ui->deleteTorrentBox->isChecked() && m_ui->deleteCancelledTorrentBox->isChecked());
        m_additionDirty = false;
    }
    if (m_behaviorDirty)
    {
        const bool tray = m_ui->checkShowSystray->isChecked()
            && QSystemTrayIcon::isSystemTrayAvailable();
        if (m_ui->windowStateComboBox->currentData().toString()
                == QLatin1String("hidden") && !tray)
        {
            QMessageBox::warning(this, tr("Tray required"),
                tr("A hidden startup window requires an enabled, available system tray. "
                   "Choose Normal or Minimized instead."));
            return false;
        }
        const bool startAtLogin = m_ui->checkStartup->isChecked();
        if (DesktopAutostart::available()
            && startAtLogin != DesktopAutostart::isEnabled())
        {
            QString error;
            if (!DesktopAutostart::setEnabled(startAtLogin, &error))
            {
                QMessageBox::warning(this, tr("Could not change autostart"), error);
                return false;
            }
        }
        m_settings.setValue(QStringLiteral("Preferences/Behavior/StartupWindowState"),
            m_ui->windowStateComboBox->currentData().toString());
        m_settings.setValue(QStringLiteral("Preferences/Behavior/EnableTray"), tray);
        m_settings.setValue(QStringLiteral("Preferences/Behavior/MinimizeToTray"),
            m_ui->checkMinimizeToSysTray->isChecked());
        m_settings.setValue(QStringLiteral("Preferences/Behavior/CloseToTray"),
            m_ui->checkCloseToSystray->isChecked());
        m_settings.setValue(QStringLiteral("Preferences/Behavior/NotifyTorrentCompletion"),
            m_ui->checkNotifyTorrentCompletion->isChecked());
        m_settings.setValue(QStringLiteral("Preferences/Behavior/SoundTorrentCompletion"),
            m_ui->checkSoundTorrentCompletion->isChecked());
        m_settings.setValue(QStringLiteral("Preferences/Behavior/TrayIconStyle"),
            m_ui->comboTrayIcon->currentIndex());
        m_settings.setValue(QStringLiteral("Preferences/Behavior/ConfirmTorrentDeletion"),
            m_ui->confirmDeletion->isChecked());
        m_settings.setValue(QStringLiteral("Preferences/Behavior/ConfirmExit"),
            m_ui->checkProgramExitConfirm->isChecked());
        m_settings.setValue(QStringLiteral("Preferences/Behavior/TorrentContentDragEnabled"),
            m_ui->checkTorrentContentDrag->isChecked());
        m_settings.setValue(QStringLiteral("Preferences/Behavior/StateColors"),
            m_ui->checkUseTorrentStatesColors->isChecked());
        m_settings.setValue(QStringLiteral("Preferences/Behavior/ProgressFollowsState"),
            m_ui->checkProgressBarFollowsTextColor->isChecked());
        m_settings.setValue(QStringLiteral("Preferences/Behavior/SeparateTrackerStatus"),
            m_ui->checkBoxUseSeparateTrackerStatusFilter->isChecked());
        m_settings.setValue(QStringLiteral("Preferences/Behavior/AlternatingRows"),
            m_ui->checkAltRowColors->isChecked());
        m_settings.setValue(QStringLiteral("Preferences/Behavior/HideZero"),
            m_ui->checkHideZero->isChecked());
        m_settings.setValue(QStringLiteral("Preferences/Behavior/HideZeroMode"),
            m_ui->comboHideZero->currentIndex());
        m_settings.setValue(QStringLiteral("Preferences/Behavior/DoubleClickDownloading"),
            m_ui->actionTorrentDlOnDblClBox->currentData().toString());
        m_settings.setValue(QStringLiteral("Preferences/Behavior/DoubleClickCompleted"),
            m_ui->actionTorrentFnOnDblClBox->currentData().toString());
        m_settings.setValue(QStringLiteral("Preferences/Behavior/ShowServerFreeSpace"),
            m_ui->checkBoxFreeDiskSpaceStatusBar->isChecked());
        m_settings.setValue(QStringLiteral("Preferences/Behavior/HideZeroStatusFilters"),
            m_ui->checkBoxHideZeroStatusFilters->isChecked());
        m_behaviorDirty = false;
        emit localBehaviorApplied();
    }
    updateButtons();
    return true;
}

void RemotePreferencesDialog::done(int result)
{
    // An in-flight server write cannot be cancelled. Do not imply it can.
    if (m_remoteBusy)
        return;
    // The selected settings page is UI state, not a preference mutation.
    m_settings.setValue(QStringLiteral("Preferences/LastPage"),
                        m_ui->tabSelection->currentRow());
    m_settings.setValue(QStringLiteral("Preferences/NavOrderVersion"), 2);
    QDialog::done(result);
}
