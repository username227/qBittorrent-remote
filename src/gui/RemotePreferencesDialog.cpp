// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/RemotePreferencesDialog.h"
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
                                                 Remote::RemoteSession *session,
                                                 TorrentModel *torrentModel)
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
    bindServerBehaviorControls();
    bindDownloadsSavingControls();
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
        "Desktop preferences are local. Remote preferences load from the "
        "server connected when this dialog opens. Unsupported controls remain disabled."));
    if (m_remoteSession)
    {
        for (const auto &profile : Remote::ProfileStore(m_settings).profiles())
            if (profile.id == preferredProfileId)
            {
                setWindowTitle(tr("Preferences — %1").arg(profile.name));
                break;
            }
    }

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

void RemotePreferencesDialog::browseRemotePath(QLineEdit *line)
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
    if (initial.isEmpty() || !QDir(initial).exists())
        initial = entries.constFirst().localRoot;
    const QString local = QFileDialog::getExistingDirectory(
        this, tr("Choose mapped local NAS directory"), initial);
    if (local.isEmpty())
        return;
    const QString remote = Remote::PathMappingStore::toRemote(local, entries);
    if (remote.isEmpty())
    {
        QMessageBox::warning(this, tr("Unmapped local directory"),
            tr("The selected folder is not covered by a mapping for this connected server. "
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
    // This client supports absolute POSIX NAS paths. Do not resolve relative
    // text against the desktop filesystem, guess a mapping, or require a mount.
    for (const QString &key : {QStringLiteral("save_path"), QStringLiteral("temp_path")})
    {
        if (!changes.contains(key))
            continue;
        const QString path = changes.value(key).toString();
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
}

void RemotePreferencesDialog::populateRemoteFields(const QJsonObject &values,
                                                    const QJsonObject &staged)
{
    m_loadingRemotePreferences = true;
    m_remotePreferencesLoaded = true;
    for (auto &field : m_remoteFields)
    {
        const QJsonValue value = values.value(field.key);
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
        if ((field.key == QLatin1String("save_path") || field.key == QLatin1String("temp_path"))
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
                dialog->populateRemoteFields(values);
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
    updateButtons();
}

void RemotePreferencesDialog::remoteDisconnected()
{
    m_remotePreferencesLoaded = false;
    refreshRemoteControls();
    if (m_remoteBusy)
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
                const QJsonValue returned = values.value(it.key());
                // qBittorrent can canonicalize directory spelling (for
                // example, remove a trailing slash). Accept equivalent POSIX
                // paths without claiming that the remote filesystem exists.
                const bool pathKey = it.key() == QLatin1String("save_path")
                    || it.key() == QLatin1String("temp_path");
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
                                  || m_behaviorDirty || !remoteChanges().isEmpty()));
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
    if (!validateDownloadChanges(changes))
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
    // An in-flight server write cannot be cancelled. Do not imply it can.
    if (m_remoteBusy)
        return;
    // The selected settings page is UI state, not a preference mutation.
    m_settings.setValue(QStringLiteral("Preferences/LastPage"),
                        m_ui->tabSelection->currentRow());
    m_settings.setValue(QStringLiteral("Preferences/NavOrderVersion"), 2);
    QDialog::done(result);
}
