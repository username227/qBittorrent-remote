// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/RemotePreferencesDialog.h"
#include "gui/DesktopAutostart.h"
#include "api/ServerProfile.h"
#include "api/RemoteSession.h"
#include "ui_optionsdialog.h"

#include <QAbstractButton>
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
#include <limits>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTreeWidget>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QVBoxLayout>
#include <QHBoxLayout>

namespace
{
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
                                                 Remote::RemoteSession *session)
    : QDialog(parent)
    , m_settings(settings)
    , m_mappingStore(settings)
    , m_ui(std::make_unique<Ui::OptionsDialog>())
    , m_remoteSession(session)
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
    loadServerBehaviorControls();
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

    // Preserve the upstream two-part dialog: settings and buttons only.
    // Unsupported controls already carry the same explanatory tooltip;
    // a permanent footer warning reduced the height available to settings.
    m_ui->tabSelection->setToolTip(tr(
        "Unimplemented controls remain disabled. Local Behavior controls "
        "use desktop settings; NAS log controls load actual values from the "
        "connected server."));

    m_applyButton = m_ui->buttonBox->button(QDialogButtonBox::Apply);
    m_applyButton->setObjectName(QStringLiteral("preferencesApply"));
    QObject::connect(m_applyButton, &QPushButton::clicked, this,
                     [this]() { applyChanges(); });
    QObject::connect(m_ui->buttonBox, &QDialogButtonBox::accepted, this, [this]() {
        if (applyChanges())
            accept();
    });
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
    m_ui->checkProgressBarFollowsTextColor->setEnabled(
        m_ui->checkUseTorrentStatesColors->isChecked());
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
    connect(m_ui->comboTrayIcon, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [dirty](int) { dirty(); });
    connect(m_ui->confirmDeletion, &QCheckBox::toggled, this, dirty);
    connect(m_ui->checkProgramExitConfirm, &QCheckBox::toggled, this, dirty);
    connect(m_ui->checkTorrentContentDrag, &QCheckBox::toggled, this, dirty);
    connect(m_ui->checkUseTorrentStatesColors, &QCheckBox::toggled, this,
            [this, dirty](bool enabled) {
                m_ui->checkProgressBarFollowsTextColor->setEnabled(enabled);
                dirty();
            });
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

void RemotePreferencesDialog::loadServerBehaviorControls()
{
    // Server log files are on the NAS, never in this desktop client's
    // filesystem. No server controls are activated without actual API data.
    if (!m_remoteSession || !m_remoteSession->isConnected())
        return;

    const auto onChanged = [this]() {
        if (!m_loadingRemotePreferences)
        {
            m_serverBehaviorDirty = true;
            updateButtons();
        }
    };

    connect(m_remoteSession, &Remote::RemoteSession::preferencesReceived,
            this, [this, onChanged](const QJsonObject &preferences) {
        m_loadingRemotePreferences = true;
        m_remotePreferences = preferences;
        m_remotePreferencesLoaded = true;

        const auto unlock = [](QWidget *widget, const QString &tooltip = {}) {
            widget->setEnabled(true);
            widget->setToolTip(tooltip);
            if (auto *combo = qobject_cast<QComboBox *>(widget))
                combo->view()->setEnabled(true);
        };
        unlock(m_ui->checkFileLog);
        unlock(m_ui->textFileLogPath, tr("Directory on the remote server (NAS), not this desktop."));
        unlock(m_ui->checkFileLogBackup);
        unlock(m_ui->spinFileLogSize);
        unlock(m_ui->checkFileLogDelete);
        unlock(m_ui->spinFileLogAge);
        unlock(m_ui->comboFileLogAgeType);
        unlock(m_ui->checkBoxPerformanceWarning);

        m_ui->checkFileLog->setChecked(
            preferences.value(QStringLiteral("file_log_enabled")).toBool());
        m_ui->textFileLogPath->setText(
            preferences.value(QStringLiteral("file_log_path")).toString());
        m_ui->checkFileLogBackup->setChecked(
            preferences.value(QStringLiteral("file_log_backup_enabled")).toBool());
        m_ui->spinFileLogSize->setValue(
            preferences.value(QStringLiteral("file_log_max_size")).toInt());
        m_ui->checkFileLogDelete->setChecked(
            preferences.value(QStringLiteral("file_log_delete_old")).toBool());
        m_ui->spinFileLogAge->setValue(
            preferences.value(QStringLiteral("file_log_age")).toInt());
        m_ui->comboFileLogAgeType->setCurrentIndex(qBound(0,
            preferences.value(QStringLiteral("file_log_age_type")).toInt(),
            m_ui->comboFileLogAgeType->count() - 1));
        m_ui->checkBoxPerformanceWarning->setChecked(
            preferences.value(QStringLiteral("performance_warning")).toBool());

        m_ui->spinFileLogSize->setEnabled(m_ui->checkFileLogBackup->isChecked());
        m_ui->spinFileLogAge->setEnabled(m_ui->checkFileLogDelete->isChecked());
        m_ui->comboFileLogAgeType->setEnabled(m_ui->checkFileLogDelete->isChecked());
        m_loadingRemotePreferences = false;
        m_serverBehaviorDirty = false;
        updateButtons();
    });
    connect(m_remoteSession, &Remote::RemoteSession::preferencesFailed,
            this, [this](const QString &reason) {
        QMessageBox::warning(this, tr("Cannot read server preferences"),
            tr("The server's log settings remain unavailable: %1").arg(reason));
    });

    connect(m_ui->checkFileLog, &QGroupBox::toggled, this,
            [onChanged](bool) { onChanged(); });
    connect(m_ui->textFileLogPath, &QLineEdit::textChanged, this,
            [onChanged](const QString &) { onChanged(); });
    for (QCheckBox *box : {m_ui->checkFileLogBackup, m_ui->checkFileLogDelete,
                           m_ui->checkBoxPerformanceWarning})
        connect(box, &QCheckBox::toggled, this,
                [onChanged](bool) { onChanged(); });
    for (QSpinBox *spin : {m_ui->spinFileLogSize, m_ui->spinFileLogAge})
        connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), this,
                [onChanged](int) { onChanged(); });
    connect(m_ui->comboFileLogAgeType,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [onChanged](int) { onChanged(); });
    connect(m_ui->checkFileLogBackup, &QCheckBox::toggled,
            m_ui->spinFileLogSize, &QWidget::setEnabled);
    connect(m_ui->checkFileLogDelete, &QCheckBox::toggled,
            m_ui->spinFileLogAge, &QWidget::setEnabled);
    connect(m_ui->checkFileLogDelete, &QCheckBox::toggled,
            m_ui->comboFileLogAgeType, &QWidget::setEnabled);

    m_remoteSession->requestPreferences();
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
        m_applyButton->setEnabled(!m_changedProfiles.isEmpty()
                                  || m_behaviorDirty || m_serverBehaviorDirty);
}

bool RemotePreferencesDialog::applyChanges()
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
    if (m_serverBehaviorDirty)
    {
        if (!m_remoteSession || !m_remoteSession->isConnected()
            || !m_remotePreferencesLoaded)
        {
            QMessageBox::warning(this, tr("Server disconnected"),
                                 tr("Cannot save the NAS log settings."));
            return false;
        }
        QJsonObject changes;
        const auto put = [this, &changes](const char *key, const QJsonValue &value) {
            const QString name = QString::fromLatin1(key);
            if (m_remotePreferences.value(name) != value)
                changes.insert(name, value);
        };
        put("file_log_enabled", m_ui->checkFileLog->isChecked());
        put("file_log_path", m_ui->textFileLogPath->text());
        put("file_log_backup_enabled", m_ui->checkFileLogBackup->isChecked());
        put("file_log_max_size", m_ui->spinFileLogSize->value());
        put("file_log_delete_old", m_ui->checkFileLogDelete->isChecked());
        put("file_log_age", m_ui->spinFileLogAge->value());
        put("file_log_age_type", m_ui->comboFileLogAgeType->currentIndex());
        put("performance_warning", m_ui->checkBoxPerformanceWarning->isChecked());
        if (!changes.isEmpty())
            m_remoteSession->setPreferences(changes);
        for (auto it = changes.constBegin(); it != changes.constEnd(); ++it)
            m_remotePreferences.insert(it.key(), it.value());
        m_serverBehaviorDirty = false;
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
    // The selected settings page is UI state, not a preference mutation.
    m_settings.setValue(QStringLiteral("Preferences/LastPage"),
                        m_ui->tabSelection->currentRow());
    m_settings.setValue(QStringLiteral("Preferences/NavOrderVersion"), 2);
    QDialog::done(result);
}
