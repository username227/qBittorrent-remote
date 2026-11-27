// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/RemotePreferencesDialog.h"
#include "gui/DatePresentation.h"
#include "gui/RemoteBanListDialog.h"
#include "gui/RemoteRssDialog.h"
#include <QFileInfo>
#include <QToolButton>
#include "gui/DesktopAutostart.h"
#include "gui/TorrentModel.h"
#include "api/ServerProfile.h"
#include "api/ServerAddress.h"
#include "api/RemoteSession.h"
#include "gui/RemoteFileDialog.h"
#include "gui/RemoteFileSystemPathComboEdit.h"
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
#include <QEvent>
#include <QResizeEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QScreen>
#include <QSystemTrayIcon>
#include <QGuiApplication>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <limits>
#include <type_traits>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QRadioButton>
#include <QTimeEdit>
#include <QLocale>
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
#include <QHostAddress>
#include <QClipboard>
#include <QDesktopServices>
#include <QInputDialog>
#include <QTimer>

namespace
{
// All ordinary rows use the same QLabel font and margins, regardless of
// whether a documentation link is present. An unavailable editor greys its
// label text, but its documentation link stays independently accessible.
class AdvancedSettingLabel final : public QLabel
{
public:
    AdvancedSettingLabel(const QString &name, const QString &url, QWidget *editor, QWidget *parent)
        : QLabel(parent), m_name(name), m_url(url), m_editor(editor)
    {
        QFont normal = parent->font();
        normal.setWeight(QFont::Normal);
        setFont(normal);
        setWordWrap(true);
        setMargin(1);
        setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        setTextFormat(Qt::RichText);
        setOpenExternalLinks(true);
        setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
        editor->installEventFilter(this);
        updateLabel();
    }

protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (object == m_editor && event->type() == QEvent::EnabledChange)
            updateLabel();
        return QLabel::eventFilter(object, event);
    }

    void changeEvent(QEvent *event) override
    {
        QLabel::changeEvent(event);
        if (event->type() == QEvent::PaletteChange)
            updateLabel();
    }

private:
    void updateLabel()
    {
        if (!m_editor) return;
        const QColor color = palette().color(m_editor->isEnabled() ? QPalette::Active : QPalette::Disabled,
                                               QPalette::Text);
        QString content = QStringLiteral("<span style=\"color:%1\">%2</span>")
            .arg(color.name(), m_name.toHtmlEscaped());
        if (!m_url.isEmpty())
            content += QStringLiteral(" <a href=\"%1\" style=\"color:%2\">(?)</a>")
                .arg(m_url.toHtmlEscaped(), palette().color(QPalette::Active, QPalette::Link).name());
        setText(content);
        setToolTip(m_name + QLatin1Char('\n') + m_editor->toolTip());
    }

    const QString m_name;
    const QString m_url;
    QPointer<QWidget> m_editor;
};

class AdvancedSettingsTable final : public QTableWidget
{
public:
    explicit AdvancedSettingsTable(QWidget *parent) : QTableWidget(parent)
    {
        connect(horizontalHeader(), &QHeaderView::sectionResized, this, [this] { scheduleRowHeights(); });
    }

    void scheduleRowHeights()
    {
        if (m_updatePending) return;
        m_updatePending = true;
        QTimer::singleShot(0, this, [this] {
            m_updatePending = false;
            for (int row = 0; row < rowCount(); ++row)
            {
                int height = fontMetrics().height() + 5;
                for (int col = 0; col < columnCount(); ++col)
                    if (auto *widget = cellWidget(row, col))
                    {
                        const int width = qMax(1, columnWidth(col) - 2);
                        const int hint = widget->hasHeightForWidth()
                            ? widget->heightForWidth(width) : widget->sizeHint().height();
                        height = qMax(height, hint + 1);
                    }
                if (rowHeight(row) != height) setRowHeight(row, height);
            }
        });
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QTableWidget::resizeEvent(event);
        scheduleRowHeights();
    }

private:
    bool m_updatePending = false;
};

const QSet<QString> webUISecurityKeys{
    "web_ui_secure_cookie_enabled", "web_ui_host_header_validation_enabled",
    "web_ui_domain_list", "web_ui_use_custom_http_headers_enabled",
    "web_ui_custom_http_headers", "web_ui_reverse_proxy_enabled",
    "web_ui_reverse_proxies_list"
};
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

// Both supported API generations require the two components of a time
// in the same setPreferences request, even if only one component changed.
struct ScheduleTimeKeys
{
    const char *hour;
    const char *minute;
};
constexpr ScheduleTimeKeys scheduleFrom{"schedule_from_hour", "schedule_from_min"};
constexpr ScheduleTimeKeys scheduleTo{"schedule_to_hour", "schedule_to_min"};

// The legacy API ignores a numeric limit unless its enable flag is also
// supplied. Both generations expose -1 when disabled; send the complete
// pair only when that logical limit changes.
struct SeedingLimitKeys
{
    const char *value;
    const char *enabled;
};
constexpr SeedingLimitKeys seedingRatio{"max_ratio", "max_ratio_enabled"};
constexpr SeedingLimitKeys seedingTime{"max_seeding_time", "max_seeding_time_enabled"};
constexpr SeedingLimitKeys seedingInactive{"max_inactive_seeding_time", "max_inactive_seeding_time_enabled"};
constexpr SeedingLimitKeys seedingLimits[] = {seedingRatio, seedingTime, seedingInactive};

bool validTimeComponent(const QJsonValue &value, int maximum)
{
    return value.isDouble() && value.toDouble() >= 0
        && value.toDouble() <= maximum && value.toDouble() == value.toInt();
}

QIcon icon(const char *name)
{
    return QIcon(QStringLiteral(":/icons/")
                 + QString::fromLatin1(name) + QStringLiteral(".svg"));
}

bool mappingEditor(QWidget *parent, Remote::PathMapping &mapping, QSettings &settings,
    const QString &profileId, const QString &serverName, Remote::RemoteSession *session)
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
    auto *remoteRow = new QWidget(&dialog);
    auto *remoteLayout = new QHBoxLayout(remoteRow);
    remoteLayout->setContentsMargins(0, 0, 0, 0);
    remoteLayout->addWidget(remote, 1);
    auto *remoteBrowse = new QPushButton(QObject::tr("Browse…"), remoteRow);
    remoteBrowse->setObjectName(QStringLiteral("mappingBrowseRemote"));
    remoteLayout->addWidget(remoteBrowse);
    const QPointer<Remote::RemoteSession> connection(session);
    remoteBrowse->setEnabled(connection && connection->isConnected());
    remoteBrowse->setToolTip(remoteBrowse->isEnabled()
        ? QObject::tr("Browse the selected server's filesystem")
        : QObject::tr("Connect to this saved server to browse its filesystem. Manual entry remains available."));
    if (connection)
    {
        QObject::connect(connection.data(), &QObject::destroyed, remoteBrowse, [remoteBrowse] {
            remoteBrowse->setEnabled(false);
        });
        QObject::connect(connection.data(), &Remote::RemoteSession::connected, remoteBrowse, [remoteBrowse] {
            remoteBrowse->setEnabled(true);
            remoteBrowse->setToolTip(QObject::tr("Browse the selected server's filesystem"));
        });
        QObject::connect(connection.data(), &Remote::RemoteSession::disconnected, remoteBrowse, [remoteBrowse] {
            remoteBrowse->setEnabled(false);
            remoteBrowse->setToolTip(QObject::tr("Connect to this saved server to browse its filesystem. Manual entry remains available."));
        });
    }
    QObject::connect(remoteBrowse, &QPushButton::clicked, &dialog,
        [&dialog, &settings, remote, connection, profileId, serverName, remoteBrowse] {
            if (!connection || !connection->isConnected())
            {
                remoteBrowse->setEnabled(false);
                return;
            }
            RemoteFileDialog picker(connection.data(), serverName, profileId, settings,
                remote->text(), RemoteFileDialog::Mode::Directory, &dialog);
            if (picker.exec() == QDialog::Accepted)
                remote->setText(picker.selectedPath());
        });
    form->addRow(QObject::tr("Server directory:"), remoteRow);
    layout->addLayout(form);

    auto *note = new QLabel(QObject::tr(
        "The local directory is a mount on THIS computer. The server directory "
        "must be an absolute path visible to qBittorrent on the selected server. "
        "Browse each filesystem independently or enter the paths manually."), &dialog);
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
                                                 const QString &serverVersion,
                                                 std::function<Remote::RemoteSession *(const QString &)> sessionForProfile)
    : QDialog(parent)
    , m_settings(settings)
    , m_remoteProfileId(preferredProfileId)
    , m_mappingStore(settings)
    , m_ui(std::make_unique<Ui::OptionsDialog>())
    , m_remoteSession(session)
    , m_sessionForProfile(std::move(sessionForProfile))
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
    for (const auto &profile : Remote::ProfileStore(m_settings).profiles())
        if (profile.id == preferredProfileId) { m_webUIProfile = profile; break; }
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
    const auto capabilities = session && session->isConnected()
        ? session->capabilities()
        : Remote::ServerCapabilities::fromVersions(serverVersion, {});
    m_advancedModernServer = capabilities.advancedModernPreferences();
    m_banRangesSupported = capabilities.banRanges();
    bindIPFilterControls();
    bindRSSControls();
    bindWebUIControls();
    bindSpeedControls();
    bindBitTorrentPrivacyControls();
    bindBitTorrentQueueingControls();
    bindBitTorrentSeedingLimits();
    bindBitTorrentAutomaticTrackers();
    buildAdvancedPage();
    bindAdvancedRemoteControls();
    bindAdvancedNetworkControls();
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
    unlock(m_ui->checkDefaultDeleteTorrentContent, tr("Preselect removal of remote downloaded files in the confirmation dialog. With confirmation disabled, only the torrent is removed."));
    unlock(m_ui->checkBoxFullTrackerURL, tr("Show the complete tracker URL in the transfer list. URLs may contain private tracker passkeys."));
    m_ui->checkBoxFullTrackerURL->setChecked(m_settings.value(
        QStringLiteral("Preferences/Behavior/FullTrackerURL"), false).toBool());
    unlock(m_ui->checkDecimalUnits, tr("Display sizes and transfer rates using powers of 1000 instead of 1024. Server settings and actual byte values are unchanged."));
    m_ui->checkDecimalUnits->setChecked(m_settings.value(
        QStringLiteral("Preferences/Behavior/DecimalUnits"), false).toBool());
    unlock(m_ui->comboDateFormat);
    m_ui->comboDateFormat->addItem(tr("System default"), QString());
    const auto example = QDateTime::currentSecsSinceEpoch();
    for (const auto &pattern : DatePresentation::patterns())
        if (!pattern.isEmpty())
            m_ui->comboDateFormat->addItem(DatePresentation::format(example, pattern), pattern);
    m_ui->comboDateFormat->setCurrentIndex(qMax(0, m_ui->comboDateFormat->findData(
        m_settings.value(QStringLiteral("Preferences/Behavior/DateFormat"), QString()).toString())));
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    unlock(m_ui->comboColorScheme, tr("Local desktop appearance. Requires application restart."));
    m_ui->comboColorScheme->addItem(tr("System"), QStringLiteral("system"));
    m_ui->comboColorScheme->addItem(tr("Light"), QStringLiteral("light"));
    m_ui->comboColorScheme->addItem(tr("Dark"), QStringLiteral("dark"));
    m_ui->comboColorScheme->setCurrentIndex(qMax(0, m_ui->comboColorScheme->findData(
        m_settings.value(QStringLiteral("Appearance/ColorScheme"), QStringLiteral("system")).toString())));
#else
    m_ui->comboColorScheme->setToolTip(tr("Requires Qt 6.8 or newer."));
#endif
    unlock(m_ui->comboContentDoubleClick, tr("Open accessible mapped content or rename the selected item on the server."));
    unlock(m_ui->checkContentOpenByEnter, tr("Open a single accessible file or folder through this server's local path mappings."));
    m_ui->comboContentDoubleClick->addItem(tr("Open"), QStringLiteral("open"));
    m_ui->comboContentDoubleClick->addItem(tr("Rename"), QStringLiteral("rename"));
    m_ui->comboContentDoubleClick->setCurrentIndex(qMax(0, m_ui->comboContentDoubleClick->findData(
        m_settings.value(QStringLiteral("Preferences/Behavior/ContentDoubleClick"), QStringLiteral("open")).toString())));
    m_ui->checkContentOpenByEnter->setChecked(m_settings.value(
        QStringLiteral("Preferences/Behavior/ContentOpenByEnter"), true).toBool());
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
    unlock(m_ui->checkBoxExternalIPStatusBar, tr("Show the last external IPv4/IPv6 addresses reported by the selected server. No external lookup is performed."));
    unlock(m_ui->checkBoxHideZeroStatusFilters);
    unlock(m_ui->windowStateComboBox);
    if (DesktopAutostart::available())
        unlock(m_ui->checkStartup);
    const bool trayAvailable = QSystemTrayIcon::isSystemTrayAvailable();
    if (trayAvailable)
        unlock(m_ui->checkShowSystray);
    else
        m_ui->checkShowSystray->setToolTip(tr("The system tray is unavailable in this desktop session. Your saved preference is retained."));
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
        QStringLiteral("Preferences/Behavior/EnableTray"), true).toBool());
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
        // Hidden startup is safe only when a usable tray can restore the window.
        const int hidden = m_ui->windowStateComboBox->findData(QStringLiteral("hidden"));
        if (auto *model = qobject_cast<QStandardItemModel *>(m_ui->windowStateComboBox->model()))
            if (auto *item = model->item(hidden))
                item->setEnabled(active);
        if (!active && m_ui->windowStateComboBox->currentIndex() == hidden)
            m_ui->windowStateComboBox->setCurrentIndex(0);
    };
    updateTrayInputs();

    m_ui->confirmDeletion->setChecked(m_settings.value(
        QStringLiteral("Preferences/Behavior/ConfirmTorrentDeletion"), true).toBool());
    m_ui->checkDefaultDeleteTorrentContent->setChecked(m_settings.value(
        QStringLiteral("Preferences/Behavior/DefaultDeleteTorrentContent"), false).toBool());
    m_ui->checkDefaultDeleteTorrentContent->setEnabled(m_ui->confirmDeletion->isChecked());
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
    m_ui->checkBoxExternalIPStatusBar->setChecked(m_settings.value(
        QStringLiteral("Preferences/Behavior/ShowExternalIP"), false).toBool());
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
    connect(m_ui->confirmDeletion, &QCheckBox::toggled, this, [this, dirty](bool enabled) {
        m_ui->checkDefaultDeleteTorrentContent->setEnabled(enabled);
        dirty();
    });
    connect(m_ui->checkDefaultDeleteTorrentContent, &QCheckBox::toggled, this, dirty);
    connect(m_ui->checkBoxFullTrackerURL, &QCheckBox::toggled, this, dirty);
    connect(m_ui->checkDecimalUnits, &QCheckBox::toggled, this, dirty);
    connect(m_ui->comboDateFormat, QOverload<int>::of(&QComboBox::currentIndexChanged),
        this, [dirty](int) { dirty(); });
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    connect(m_ui->comboColorScheme, QOverload<int>::of(&QComboBox::currentIndexChanged),
        this, [dirty](int) { dirty(); });
#endif
    connect(m_ui->checkProgramExitConfirm, &QCheckBox::toggled, this, dirty);
    connect(m_ui->checkTorrentContentDrag, &QCheckBox::toggled, this, dirty);
    connect(m_ui->comboContentDoubleClick, qOverload<int>(&QComboBox::currentIndexChanged), this, dirty);
    connect(m_ui->checkContentOpenByEnter, &QCheckBox::toggled, this, dirty);
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
    connect(m_ui->checkBoxExternalIPStatusBar, &QCheckBox::toggled, this, dirty);
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

void RemotePreferencesDialog::bindRSSControls()
{
    m_rssImageBaseline = m_settings.value(QStringLiteral("Preferences/RSS/LoadExternalImages"), false).toBool();
    m_ui->checkRSSLoadExternalImages->setEnabled(true);
    m_ui->checkRSSLoadExternalImages->setChecked(m_rssImageBaseline);
    m_ui->checkRSSLoadExternalImages->setToolTip(tr("Local setting for all servers. Images are fetched directly by this desktop, which may reveal your IP address to their hosts. Disabled by default."));
    connect(m_ui->checkRSSLoadExternalImages, &QCheckBox::toggled, this, [this] { updateButtons(); });
    // The native RSS page permits editing dormant reader and downloader
    // settings independently. Both upstream API generations use the same
    // keys and units: refresh minutes, same-host delay seconds, article count.
    bindRemoteControl("rss_processing_enabled", m_ui->checkRSSEnable, {},
        tr("Fetch RSS feeds on the connected server, even when this application is closed."));
    const auto bindSpin = [this](const char *key, QSpinBox *spin, const QString &tip) {
        if (auto *editor = spin->findChild<QLineEdit *>())
        {
            editor->setEnabled(true);
            editor->setToolTip(QString());
        }
        bindRemoteControl(key, spin, {}, tip);
    };
    bindSpin("rss_refresh_interval", m_ui->spinRSSRefreshInterval,
        tr("Default feed refresh interval in minutes on the connected server."));
    bindSpin("rss_fetch_delay", m_ui->spinRSSFetchDelay,
        tr("Delay between requests to the same host in seconds."));
    bindSpin("rss_max_articles_per_feed", m_ui->spinRSSMaxArticlesPerFeed,
        tr("Maximum number of stored articles per feed on the connected server."));
    bindRemoteControl("rss_auto_downloading_enabled", m_ui->checkRSSAutoDownloaderEnable, {},
        tr("Automatically add torrents matching existing RSS rules on the connected server."));
    bindRemoteControl("rss_download_repack_proper_episodes", m_ui->checkSmartFilterDownloadRepacks, {},
        tr("Allow REPACK and PROPER releases when a rule uses the smart episode filter."));
    bindRemoteControl("rss_smart_episode_filters", m_ui->textSmartEpisodeFilters, {},
        tr("Server smart episode filter patterns, one per line. Preserve the server's text format."));

    // Rule mutations are independent of staged global preferences.
    m_ui->btnEditRules->setEnabled(false);
    m_ui->btnEditRules->setToolTip(tr("Edit server RSS rules. Rule changes take effect immediately, independently of Preferences Apply/Cancel."));
    connect(m_ui->btnEditRules, &QPushButton::clicked, this, [this]() {
        if (!m_remoteSession || !m_remoteSession->isConnected() || m_remoteBusy) return;
        RemoteRssDialog dialog(m_remoteSession, m_remoteTargetName, this, true);
        dialog.exec();
    });
}

bool RemotePreferencesDialog::validateRSSChanges(const QJsonObject &changes)
{
    // Enabling processing can immediately evaluate existing server rules;
    // enabling the reader can also feed an already-enabled auto downloader.
    const bool startsAuto = changes.value(QStringLiteral("rss_auto_downloading_enabled")).toBool();
    const bool startsReader = changes.value(QStringLiteral("rss_processing_enabled")).toBool();
    const bool autoMayBeEnabled = !remoteFieldAvailable("rss_auto_downloading_enabled")
        || m_ui->checkRSSAutoDownloaderEnable->isChecked();
    if (!startsAuto && !(startsReader && autoMayBeEnabled))
        return true;
    return QMessageBox::question(this, tr("Enable server RSS downloads"),
        tr("The connected server may immediately evaluate existing RSS auto-download rules "
           "and add matching torrents. This can continue when qBittorrent Remote is closed. "
           "Apply these settings?"), QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
        == QMessageBox::Yes;
}

void RemotePreferencesDialog::bindWebUIControls()
{
    // A remote client must never offer to disable the server's Web API.
    m_ui->checkWebUI->setCheckable(false);
    m_ui->checkWebUI->setEnabled(true);
    m_ui->labelWebUIError->setWordWrap(true);
    const auto direct = [this] { return m_webUIDirect; };
    const auto directTip = tr("Changing this server setting requires a fresh connection and verification before updating the saved profile.");
    bindRemoteControl("web_ui_address", m_ui->textWebUIAddress, direct, directTip);
    if (auto *editor = m_ui->spinWebUIPort->findChild<QLineEdit *>()) editor->setEnabled(true);
    bindRemoteControl("web_ui_port", m_ui->spinWebUIPort, direct, directTip);
    bindRemoteControl("use_https", m_ui->checkWebUIHttps, direct, directTip);
    const auto tls = [this] { return m_webUIDirect && m_ui->checkWebUIHttps->isChecked(); };
    bindRemoteControl("web_ui_https_cert_path", m_ui->textWebUIHttpsCert, tls, directTip);
    bindRemoteControl("web_ui_https_key_path", m_ui->textWebUIHttpsKey, tls, directTip);
    installPathBrowser(m_ui->textWebUIHttpsCert, false);
    installPathBrowser(m_ui->textWebUIHttpsKey, false);
    bindRemoteControl("web_ui_username", m_ui->textWebUIUsername, direct, directTip);
    m_ui->textWebUIPassword->setToolTip(tr("Enter a new password only when changing it. The current password is never returned by the server. If authentication is bypassed, a fresh connection may not independently verify the new password."));
    m_ui->textWebUIPassword->clear();
    connect(m_ui->textWebUIPassword, &QLineEdit::textChanged, this, [this] { remoteFieldChanged(); });
    // The API key is a secret and its rotation/deletion uses separate actions.
    // Show only a masked existing key, never store it in QSettings.
    m_ui->textWebUIAPIKey->setEchoMode(QLineEdit::Password);
    bindRemoteControl("web_ui_api_key", m_ui->textWebUIAPIKey, {},
        tr("Server API key (masked). Use Copy to retrieve it. Key changes take effect immediately."));
    connect(m_ui->btnWebUIAPIKeyCopy, &QPushButton::clicked, this, [this] {
        if (!m_remoteBusy && remoteFieldAvailable("web_ui_api_key") && !m_ui->textWebUIAPIKey->text().isEmpty())
            QApplication::clipboard()->setText(m_ui->textWebUIAPIKey->text());
    });
    connect(m_ui->btnWebUIAPIKeyRotate, &QPushButton::clicked, this, [this] { changeWebUIAPIKey(false); });
    connect(m_ui->btnWebUIAPIKeyDelete, &QPushButton::clicked, this, [this] { changeWebUIAPIKey(true); });

    bindRemoteControl("web_ui_upnp", m_ui->checkWebUIUPnP, {},
        tr("Forward the WebUI port on the server's router. This may expose the WebUI beyond the local network."));
    bindRemoteControl("bypass_local_auth", m_ui->checkBypassLocalAuth, {},
        tr("Allow unauthenticated requests from the server's loopback interface."));
    bindRemoteControl("bypass_auth_subnet_whitelist_enabled", m_ui->checkBypassAuthSubnetWhitelist, {},
        tr("Allow unauthenticated requests from the configured server-side IP subnets."));
    bindRemoteField(RemoteField{
        QStringLiteral("bypass_auth_subnet_whitelist"), m_ui->IPSubnetWhitelistButton,
        [this] { return QJsonValue(m_webUISubnets); },
        [this](const QJsonValue &v) { m_webUISubnets = v.toString(); },
        [](const QJsonValue &v) { return v.isString(); },
        [this] { return remoteFieldAvailable("bypass_auth_subnet_whitelist_enabled")
            && m_ui->checkBypassAuthSubnetWhitelist->isChecked(); },
        tr("Edit the server's authentication bypass list. One IP address or CIDR subnet per line.")
    });
    connect(m_ui->IPSubnetWhitelistButton, &QPushButton::clicked, this, [this] {
        if (!m_ui->IPSubnetWhitelistButton->isEnabled() || m_remoteBusy) return;
        QDialog dialog(this);
        dialog.setWindowTitle(tr("IP subnet whitelist"));
        auto *layout = new QVBoxLayout(&dialog);
        auto *note = new QLabel(tr("Clients from these IP addresses or subnets may access the WebUI without authentication. Enter one per line."), &dialog);
        note->setWordWrap(true);
        layout->addWidget(note);
        auto *editor = new QPlainTextEdit(m_webUISubnets, &dialog);
        editor->setObjectName(QStringLiteral("webUISubnetWhitelist"));
        layout->addWidget(editor);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
            QStringList subnets;
            for (const QString &line : editor->toPlainText().split(QLatin1Char('\n')))
            {
                const QString input = line.trimmed();
                if (input.isEmpty()) continue;
                const auto subnet = QHostAddress::parseSubnet(input);
                if (subnet.first.isNull() || subnet.second < 0)
                {
                    QMessageBox::warning(&dialog, tr("Invalid subnet"), tr("Enter a valid IPv4 or IPv6 address or CIDR subnet on each line."));
                    return;
                }
                const QString value = subnet.first.toString() + QLatin1Char('/') + QString::number(subnet.second);
                if (!subnets.contains(value)) subnets.append(value);
            }
            m_webUISubnets = subnets.join(QLatin1Char('\n'));
            dialog.accept();
            remoteFieldChanged();
        });
        dialog.exec();
    });

    const auto bindSpin = [this](const char *key, QSpinBox *spin, std::function<bool()> dependency = {}) {
        if (auto *editor = spin->findChild<QLineEdit *>()) editor->setEnabled(true);
        bindRemoteControl(key, spin, std::move(dependency));
    };
    bindSpin("web_ui_max_auth_fail_count", m_ui->spinBanCounter);
    bindSpin("web_ui_ban_duration", m_ui->spinBanDuration, [this] {
        return remoteFieldAvailable("web_ui_max_auth_fail_count") && m_ui->spinBanCounter->value() > 0;
    });
    bindSpin("web_ui_session_timeout", m_ui->spinSessionTimeout);
    bindSpin("web_ui_sessions_count_limit", m_ui->spinBoxWebUISessionsCountLimit);

    const auto alternateTip = tr("Changes the connected server's native browser WebUI, not qBittorrent Remote.");
    bindRemoteControl("alternative_webui_enabled", m_ui->groupAltWebUI, {}, alternateTip);
    bindRemoteControl("alternative_webui_path", m_ui->textWebUIRootFolder, [this] {
        return remoteFieldAvailable("alternative_webui_enabled") && m_ui->groupAltWebUI->isChecked();
    }, tr("Directory on the connected server, not the desktop. Changes its native browser WebUI, not qBittorrent Remote."));
    installPathBrowser(m_ui->textWebUIRootFolder);
    m_ui->lblAltWebUIList->setToolTip(alternateTip);
    bindRemoteControl("web_ui_clickjacking_protection_enabled", m_ui->checkClickjacking);
    bindRemoteControl("web_ui_csrf_protection_enabled", m_ui->checkCSRFProtection);
    const auto security = [this] { return Remote::ServerAddress::parse(m_webUIProfile.url).has_value(); };
    bindRemoteControl("web_ui_secure_cookie_enabled", m_ui->checkSecureCookie, security,
        tr("Request secure session cookies on trustworthy origins (HTTPS or localhost). A fresh connection will verify continued access."));
    bindRemoteControl("web_ui_host_header_validation_enabled", m_ui->groupHostHeaderValidation, security,
        tr("Validate the server's HTTP Host header to protect against DNS rebinding."));
    bindRemoteControl("web_ui_domain_list", m_ui->textServerDomains, [this, security] {
        return security() && remoteFieldAvailable("web_ui_host_header_validation_enabled")
            && m_ui->groupHostHeaderValidation->isChecked();
    }, tr("Allowed server domain names, separated by semicolons. Wildcards (*) are supported."));
    bindRemoteControl("web_ui_use_custom_http_headers_enabled", m_ui->groupWebUIAddCustomHTTPHeaders, security,
        tr("Set custom response headers on the server. These may affect browser security or API connectivity."));
    bindRemoteControl("web_ui_custom_http_headers", m_ui->textWebUICustomHTTPHeaders, [this, security] {
        return security() && remoteFieldAvailable("web_ui_use_custom_http_headers_enabled")
            && m_ui->groupWebUIAddCustomHTTPHeaders->isChecked();
    }, tr("One Header: value pair per line."));
    bindRemoteControl("web_ui_reverse_proxy_enabled", m_ui->groupEnableReverseProxySupport, security,
        tr("Allow the server to use forwarded client addresses from explicitly trusted reverse proxies."));
    bindRemoteControl("web_ui_reverse_proxies_list", m_ui->textTrustedReverseProxiesList, [this, security] {
        return security() && remoteFieldAvailable("web_ui_reverse_proxy_enabled")
            && m_ui->groupEnableReverseProxySupport->isChecked();
    }, tr("Trusted reverse-proxy IP addresses or CIDR subnets, separated by semicolons. Only trust proxies you control."));

    const auto dnsEnabled = [this] {
        return remoteFieldAvailable("dyndns_enabled") && m_ui->checkDynDNS->isChecked();
    };
    bindRemoteControl("dyndns_enabled", m_ui->checkDynDNS, {},
        tr("The selected server updates its public DNS record, not the desktop client."));
    bindRemoteControl("dyndns_service", m_ui->comboDNSService, dnsEnabled);
    bindRemoteControl("dyndns_domain", m_ui->domainNameTxt, dnsEnabled);
    bindRemoteControl("dyndns_username", m_ui->DNSUsernameTxt, dnsEnabled);
    m_ui->DNSPasswordTxt->setEchoMode(QLineEdit::Password);
    bindRemoteControl("dyndns_password", m_ui->DNSPasswordTxt, dnsEnabled,
        tr("Dynamic DNS provider credential stored on the server only."));
    connect(m_ui->registerDNSBtn, &QPushButton::clicked, this, [this] {
        if (!m_ui->registerDNSBtn->isEnabled() || m_remoteBusy) return;
        const int service = m_ui->comboDNSService->currentIndex();
        if (service == 0 || service == 1)
            QDesktopServices::openUrl(QUrl(service == 0
                ? QStringLiteral("https://account.dyn.com/entrance/")
                : QStringLiteral("https://www.noip.com/remote-access")));
    });
}

void RemotePreferencesDialog::refreshWebUIControls()
{
    // A missing 5.3-only key is not an empty key on an older daemon.
    const bool apiAvailable = remoteFieldAvailable("web_ui_api_key");
    const bool hasKey = apiAvailable && !m_ui->textWebUIAPIKey->text().isEmpty();
    m_ui->groupWebUIAPIKey->setVisible(apiAvailable);
    m_ui->textWebUIAPIKey->setEnabled(hasKey);
    m_ui->btnWebUIAPIKeyCopy->setEnabled(hasKey && !m_remoteBusy);
    m_ui->btnWebUIAPIKeyRotate->setEnabled(apiAvailable && !m_remoteBusy);
    m_ui->btnWebUIAPIKeyRotate->setToolTip(hasKey ? tr("Rotate API key") : tr("Generate API key"));
    m_ui->btnWebUIAPIKeyDelete->setEnabled(hasKey && !m_remoteBusy);
    m_ui->registerDNSBtn->setEnabled(m_ui->comboDNSService->isEnabled());
    m_ui->labelWebUISessionsCountLimit->setVisible(remoteFieldAvailable("web_ui_sessions_count_limit"));
    m_ui->spinBoxWebUISessionsCountLimit->setVisible(remoteFieldAvailable("web_ui_sessions_count_limit"));
    m_ui->lblBanDuration->setEnabled(m_ui->spinBanDuration->isEnabled());
    const bool canChangePassword = m_remotePreferencesLoaded && m_webUIDirect;
    m_ui->textWebUIPassword->setEnabled(canChangePassword);
    m_ui->labelWebUIError->setVisible(m_remotePreferencesLoaded && !m_webUIDirect);
    m_ui->labelWebUIError->setText(tr("Connection changes are available for direct server connections only. For a proxy or a different externally mapped port, use independent server access."));
}

void RemotePreferencesDialog::changeWebUIAPIKey(bool remove)
{
    if (m_remoteBusy || !m_remoteSession || !m_remoteSession->isConnected()
        || !remoteFieldAvailable("web_ui_api_key")) return;
    const QString previous = m_ui->textWebUIAPIKey->text();
    if (remove && previous.isEmpty()) return;
    const QString title = remove ? tr("Delete API key")
        : previous.isEmpty() ? tr("Generate API key") : tr("Rotate API key");
    const QString message = remove
        ? tr("Delete the selected server's API key? It will immediately stop working.")
        : previous.isEmpty()
            ? tr("Generate an API key for the selected server?")
            : tr("Rotate the selected server's API key? The existing key will immediately stop working.");
    if (QMessageBox::question(this, title, message + tr("\n\nThis action takes effect immediately; Cancel in Preferences cannot undo it."),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    if (m_remoteBusy || !m_remoteSession || !m_remoteSession->isConnected()
        || !remoteFieldAvailable("web_ui_api_key")) return;
    setRemoteBusy(true);
    const QPointer<RemotePreferencesDialog> dialog(this);
    // On ambiguous failure do not allow further mutations or copying a stale
    // key until a fresh authoritative Preferences read.
    const auto finish = [dialog](const QJsonValue &value, const QString &warning) {
        if (!dialog || !dialog->m_remoteBusy) return;
        for (auto &field : dialog->m_remoteFields)
            if (field.key == QLatin1String("web_ui_api_key"))
            {
                field.baseline = value.isString() ? value : QJsonValue(QJsonValue::Undefined);
                const QSignalBlocker blocker(field.widget);
                field.write(value.isString() ? value : QJsonValue(QString()));
                break;
            }
        dialog->setRemoteBusy(false);
        dialog->refreshRemoteControls();
        dialog->updateButtons();
        if (!warning.isEmpty())
            QMessageBox::warning(dialog.data(), dialog->tr("Cannot confirm API key change"), warning);
    };
    m_remoteSession->requestPreferences([dialog, previous, remove, finish](const QJsonObject &values) {
        if (!dialog || !dialog->m_remoteBusy || !dialog->m_remoteSession || !dialog->m_remoteSession->isConnected()) return;
        const auto current = values.value("web_ui_api_key");
        if (!current.isString() || current.toString() != previous)
        {
            finish(current, dialog->tr("The server API key changed since Preferences was opened. No mutation was sent. Review the current value before trying again."));
            return;
        }
        const auto failed = [dialog, finish](const QString &) {
            if (dialog && dialog->m_remoteBusy)
                finish(QJsonValue(), dialog->tr("The API key operation could not be confirmed. Reopen Preferences to read the current key before trying again."));
        };
        const auto verify = [dialog, previous, remove, finish, failed](const QString &expected) {
            if (!dialog || !dialog->m_remoteBusy || !dialog->m_remoteSession || !dialog->m_remoteSession->isConnected()) return;
            if (!remove && (expected.isEmpty() || expected == previous))
            {
                failed(QString());
                return;
            }
            dialog->m_remoteSession->requestPreferences([dialog, expected, finish](const QJsonObject &updated) {
                if (!dialog || !dialog->m_remoteBusy) return;
                const auto key = updated.value("web_ui_api_key");
                if (!key.isString() || key.toString() != expected)
                    finish(key, dialog->tr("The server did not confirm the requested API key. Review the current state before retrying."));
                else
                    finish(key, QString());
            }, failed);
        };
        if (remove)
            dialog->m_remoteSession->deleteWebUIAPIKey([verify] { verify(QString()); }, failed);
        else
            dialog->m_remoteSession->rotateWebUIAPIKey([verify](const QJsonObject &result) {
                verify(result.value("apiKey").toString());
            }, failed);
    }, [dialog, finish](const QString &) {
        if (dialog && dialog->m_remoteBusy)
            finish(QJsonValue(), dialog->tr("Could not check the server's current API key. No mutation was sent. Reopen Preferences to retry."));
    });
}

bool RemotePreferencesDialog::validateWebUIChanges(const QJsonObject &changes)
{
    bool dnsChange = false;
    for (auto it = changes.constBegin(); it != changes.constEnd(); ++it)
        dnsChange |= it.key().startsWith(QLatin1String("dyndns_"));
    if (dnsChange && m_ui->checkDynDNS->isChecked())
    {
        // Use the same validation as the upstream 5.1.4/5.3.0 DNS updater.
        const QRegularExpression domain(QStringLiteral("^(?:(?!\\d|-)[a-zA-Z0-9\\-]{1,63}\\.)+[a-zA-Z]{2,}$"));
        if (!remoteFieldAvailable("dyndns_enabled") || !remoteFieldAvailable("dyndns_service")
            || !remoteFieldAvailable("dyndns_domain") || !remoteFieldAvailable("dyndns_username")
            || !remoteFieldAvailable("dyndns_password")
            || !domain.match(m_ui->domainNameTxt->text()).hasMatch()
            || m_ui->DNSUsernameTxt->text().size() < 4 || m_ui->DNSPasswordTxt->text().size() < 4)
        {
            QMessageBox::warning(this, tr("Invalid Dynamic DNS settings"),
                tr("Select a supported provider and enter a valid domain name, username and password (at least four characters each for the credentials)."));
            return false;
        }
    }
    if (dnsChange && QMessageBox::question(this, tr("Change Dynamic DNS settings"),
            tr("The selected server will manage these DNS settings. Updating its DNS record may affect access by hostname. The client does not change its saved server address or contact the DNS provider. Apply these changes?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return false;
    const auto enabled = [&changes](const char *key) { return changes.value(QLatin1String(key)).toBool(); };
    if ((enabled("alternative_webui_enabled") || changes.contains("alternative_webui_path"))
        && m_ui->groupAltWebUI->isChecked()
        && (!remoteFieldAvailable("alternative_webui_path") || m_ui->textWebUIRootFolder->text().trimmed().isEmpty()))
    {
        QMessageBox::warning(this, tr("Alternative WebUI"), tr("Enter the server directory containing the alternative WebUI."));
        return false;
    }
    // Authentication, router exposure and session policy deserve explicit
    // confirmation. Do not conflate a successful readback with proof of reachability.
    const bool securityChange = changes.contains("web_ui_upnp")
        || changes.contains("bypass_local_auth")
        || changes.contains("bypass_auth_subnet_whitelist_enabled")
        || changes.contains("bypass_auth_subnet_whitelist")
        || changes.contains("web_ui_max_auth_fail_count")
        || changes.contains("web_ui_ban_duration")
        || changes.contains("web_ui_session_timeout")
        || changes.contains("web_ui_sessions_count_limit")
        || changes.contains("web_ui_clickjacking_protection_enabled")
        || changes.contains("web_ui_csrf_protection_enabled")
        || changes.contains("alternative_webui_enabled")
        || changes.contains("alternative_webui_path");
    if (!securityChange) return true;
    return QMessageBox::question(this, tr("Change server WebUI settings"),
        tr("These settings affect access to the selected server. Authentication bypass and router forwarding may expose the WebUI; changes to session and security policy may interrupt existing clients. Ensure you have independent access to the server before applying. Continue?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
}

void RemotePreferencesDialog::bindSpeedControls()
{
    m_ui->labelGlobalRate->setPixmap(icon("slow_off").pixmap(22, 22));
    m_ui->labelAltRate->setPixmap(icon("slow").pixmap(22, 22));

    const auto bindRate = [this](const char *key, QSpinBox *spin) {
        // Native UI uses whole KiB/s; preferences API uses bytes/sec.
        // Do not turn a fractional KiB/s into an incorrect unlimited rate.
        if (auto *editor = spin->findChild<QLineEdit *>())
            editor->setEnabled(true);
        bindRemoteField(RemoteField{
            QString::fromLatin1(key), spin,
            [spin]() { return QJsonValue(spin->value() * 1024); },
            [spin](const QJsonValue &value) { spin->setValue(value.toInt() / 1024); },
            [spin](const QJsonValue &value) {
                return value.isDouble() && value.toDouble() >= 0
                    && value.toDouble() <= spin->maximum() * 1024.0
                    && value.toDouble() == value.toInt()
                    && value.toInt() % 1024 == 0;
            },
            {}, tr("Rate limit on the selected server in KiB/s; 0 means unlimited.")
        });
        connect(spin, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [this]() { remoteFieldChanged(); });
    };
    bindRate("up_limit", m_ui->spinUploadLimit);
    bindRate("dl_limit", m_ui->spinDownloadLimit);
    bindRate("alt_up_limit", m_ui->spinUploadLimitAlt);
    bindRate("alt_dl_limit", m_ui->spinDownloadLimitAlt);

    bindRemoteControl("scheduler_enabled", m_ui->groupBoxSchedule, {},
        tr("Schedule alternative rate limits using the connected server's local time."));
    const auto scheduleEnabled = [this]() {
        return remoteFieldAvailable("scheduler_enabled") && m_ui->groupBoxSchedule->isChecked();
    };
    const auto bindTime = [this, scheduleEnabled](ScheduleTimeKeys keys, QTimeEdit *editor) {
        if (auto *line = editor->findChild<QLineEdit *>())
            line->setEnabled(true);
        const auto available = [this, scheduleEnabled, keys]() {
            return scheduleEnabled() && remoteFieldAvailable(keys.hour)
                && remoteFieldAvailable(keys.minute);
        };
        const QString tooltip = tr("Time on the connected server, not this desktop.");
        bindRemoteField(RemoteField{
            QString::fromLatin1(keys.hour), editor,
            [editor]() { return QJsonValue(editor->time().hour()); },
            [editor](const QJsonValue &value) {
                editor->setTime(QTime(value.toInt(), editor->time().minute()));
            },
            [](const QJsonValue &value) { return validTimeComponent(value, 23); },
            available, tooltip
        });
        bindRemoteField(RemoteField{
            QString::fromLatin1(keys.minute), editor,
            [editor]() { return QJsonValue(editor->time().minute()); },
            [editor](const QJsonValue &value) {
                editor->setTime(QTime(editor->time().hour(), value.toInt()));
            },
            [](const QJsonValue &value) { return validTimeComponent(value, 59); },
            available, tooltip
        });
        connect(editor, &QTimeEdit::timeChanged, this, [this]() { remoteFieldChanged(); });
    };
    bindTime(scheduleFrom, m_ui->timeEditScheduleFrom);
    bindTime(scheduleTo, m_ui->timeEditScheduleTo);

    // The Designer form already contains Every day, Weekdays, Weekends.
    // Upstream appends Monday through Sunday, in that exact enum order.
    const QLocale locale;
    for (int day = 1; day <= 7; ++day)
        m_ui->comboBoxScheduleDays->addItem(locale.dayName(day, QLocale::LongFormat));
    bindRemoteControl("scheduler_days", m_ui->comboBoxScheduleDays, scheduleEnabled,
        tr("Days on which the connected server uses the alternative rate schedule."));
    bindRemoteControl("limit_utp_rate", m_ui->checkLimituTPConnections);
    bindRemoteControl("limit_tcp_overhead", m_ui->checkLimitTransportOverhead);
    bindRemoteControl("limit_lan_peers", m_ui->checkLimitLocalPeerRate);
}

bool RemotePreferencesDialog::validateSpeedChanges(const QJsonObject &changes)
{
    const bool timeChanged = changes.contains(QLatin1String(scheduleFrom.hour))
        || changes.contains(QLatin1String(scheduleTo.hour));
    const bool enabling = changes.value(QStringLiteral("scheduler_enabled")).toBool();
    const bool enabled = remoteFieldAvailable("scheduler_enabled")
        && m_ui->groupBoxSchedule->isChecked();
    const bool daysChanged = changes.contains(QStringLiteral("scheduler_days"));
    if (!timeChanged && !enabling && !(enabled && daysChanged))
        return true;
    const bool timesAvailable = remoteFieldAvailable(scheduleFrom.hour)
        && remoteFieldAvailable(scheduleFrom.minute)
        && remoteFieldAvailable(scheduleTo.hour)
        && remoteFieldAvailable(scheduleTo.minute);
    if (enabled && (!timesAvailable || !remoteFieldAvailable("scheduler_days")))
    {
        m_ui->tabSelection->setCurrentRow(4);
        QMessageBox::warning(this, tr("Unavailable schedule"),
            tr("The connected server has not provided a complete, valid alternative rate schedule."));
        return false;
    }
    if (timesAvailable && m_ui->timeEditScheduleFrom->time() == m_ui->timeEditScheduleTo->time())
    {
        m_ui->tabSelection->setCurrentRow(4);
        QMessageBox::warning(this, tr("Time Error"),
            tr("The start time and the end time can't be the same."));
        return false;
    }
    return true;
}

void RemotePreferencesDialog::bindBitTorrentPrivacyControls()
{
    bindRemoteControl("dht", m_ui->checkDHT, {}, m_ui->checkDHT->toolTip().section(QLatin1Char('\n'), 0, 0));
    bindRemoteControl("pex", m_ui->checkPeX, {}, m_ui->checkPeX->toolTip().section(QLatin1Char('\n'), 0, 0));
    bindRemoteControl("lsd", m_ui->checkLSD, {}, m_ui->checkLSD->toolTip().section(QLatin1Char('\n'), 0, 0));
    // Both supported upstream generations use the native combo indices:
    // 0 = allow, 1 = require, 2 = disable protocol encryption.
    bindRemoteControl("encryption", m_ui->comboEncryption, {},
        tr("Allow encryption: Connect to peers regardless of setting\n"
           "Require encryption: Only connect to peers with protocol encryption\n"
           "Disable encryption: Only connect to peers without protocol encryption"));
    bindRemoteControl("anonymous_mode", m_ui->checkAnonymousMode, {},
        tr("Reduce identifying information sent by the connected server. "
           "This does not enable a VPN or proxy or hide its IP address."));
}

void RemotePreferencesDialog::bindBitTorrentQueueingControls()
{
    // Unlike connection limits, zero is a distinct queue limit; only -1
    // is represented by the native infinity symbol. Slow-rate preferences
    // already use KiB/s, unlike the global and alternative speed limits.
    const auto bindSpin = [this](const char *key, QSpinBox *spin,
        std::function<bool()> dependency = {}, QString tooltip = {}) {
        // The placeholder setup separately disables spin-box editors.
        if (auto *editor = spin->findChild<QLineEdit *>())
        {
            editor->setEnabled(true);
            editor->setToolTip(QString());
        }
        bindRemoteControl(key, spin, std::move(dependency), std::move(tooltip));
    };
    const QString limitTip = tr("Limit on the connected server; -1 means unlimited.");
    bindSpin("max_active_checking_torrents", m_ui->spinBoxMaxActiveCheckingTorrents,
        {}, limitTip);
    bindRemoteControl("queueing_enabled", m_ui->checkEnableQueueing);
    const auto queueing = [this]() {
        return remoteFieldAvailable("queueing_enabled") && m_ui->checkEnableQueueing->isChecked();
    };
    bindSpin("max_active_downloads", m_ui->spinMaxActiveDownloads, queueing, limitTip);
    bindSpin("max_active_uploads", m_ui->spinMaxActiveUploads, queueing, limitTip);
    bindSpin("max_active_torrents", m_ui->spinMaxActiveTorrents, queueing, limitTip);
    bindRemoteControl("dont_count_slow_torrents",
        m_ui->checkIgnoreSlowTorrentsForQueueing, queueing,
        tr("Slow torrents are excluded from the active download and upload limits. "
           "The maximum active torrents limit remains a hard cap."));
    const auto ignoreSlow = [this, queueing]() {
        return queueing() && remoteFieldAvailable("dont_count_slow_torrents")
            && m_ui->checkIgnoreSlowTorrentsForQueueing->isChecked();
    };
    bindSpin("slow_torrent_dl_rate_threshold", m_ui->spinDownloadRateForSlowTorrents,
        ignoreSlow, tr("Download threshold in KiB/s on the connected server."));
    bindSpin("slow_torrent_ul_rate_threshold", m_ui->spinUploadRateForSlowTorrents,
        ignoreSlow, tr("Upload threshold in KiB/s on the connected server."));
    bindSpin("slow_torrent_inactive_timer", m_ui->spinSlowTorrentsInactivityTimer,
        ignoreSlow, tr("Inactivity timer in seconds on the connected server."));
}

void RemotePreferencesDialog::bindBitTorrentSeedingLimits()
{
    // Use the current upstream native ratio range and two-decimal precision.
    // Both API generations accept nonnegative ratios and times; -1 disables.
    m_ui->spinMaxRatio->setMaximum(std::numeric_limits<int>::max());
    const auto bind = [this](SeedingLimitKeys keys, QCheckBox *check, auto *spin) {
        using Spin = std::remove_pointer_t<decltype(spin)>;
        if (auto *editor = spin->template findChild<QLineEdit *>())
        {
            editor->setEnabled(true);
            editor->setToolTip(QString());
        }
        bindRemoteField(RemoteField{
            QString::fromLatin1(keys.value), check,
            [check, spin]() {
                return QJsonValue(check->isChecked() ? double(spin->value()) : -1.0);
            },
            [check, spin](const QJsonValue &value) {
                const double limit = value.toDouble();
                check->setChecked(limit >= 0);
                // Keep the previous in-dialog value when unchecked. The
                // daemon stores only -1, not a dormant numeric threshold.
                if (limit >= 0)
                    spin->setValue(static_cast<decltype(spin->value())>(limit));
            },
            [spin](const QJsonValue &value) {
                if (!value.isDouble()) return false;
                const double limit = value.toDouble();
                if (limit == -1) return true;
                if (limit < spin->minimum() || limit > spin->maximum()) return false;
                if constexpr (std::is_same_v<Spin, QSpinBox>)
                    return limit == value.toInt();
                return true; // populateRemoteFields also rejects rounding.
            },
            {}, tr("Global limit on the connected server; uncheck to disable.")
        });
        connect(check, &QCheckBox::toggled, this, [this]() { remoteFieldChanged(); });
        connect(spin, &Spin::valueChanged, this, [this]() { remoteFieldChanged(); });
    };
    bind(seedingRatio, m_ui->checkMaxRatio, m_ui->spinMaxRatio);
    bind(seedingTime, m_ui->checkMaxSeedingMinutes, m_ui->spinMaxSeedingMinutes);
    bind(seedingInactive, m_ui->checkMaxInactiveSeedingMinutes, m_ui->spinMaxInactiveSeedingMinutes);

    // Native dropdown order differs from the API enum: the two last
    // entries are reversed. Never bind this as a plain index.
    auto *action = m_ui->comboRatioLimitAct;
    const QList<int> actions{0, 1, 3, 2};
    bindRemoteField(RemoteField{
        QStringLiteral("max_ratio_act"), action,
        [action, actions]() { return QJsonValue(actions.value(action->currentIndex(), -1)); },
        [action, actions](const QJsonValue &v) { action->setCurrentIndex(actions.indexOf(v.toInt())); },
        [](const QJsonValue &v) {
            return v.isDouble() && v.toDouble() == v.toInt() && v.toInt() >= 0 && v.toInt() <= 3;
        },
        [this]() {
            return (remoteFieldAvailable(seedingRatio.value) && m_ui->checkMaxRatio->isChecked())
                || (remoteFieldAvailable(seedingTime.value) && m_ui->checkMaxSeedingMinutes->isChecked())
                || (remoteFieldAvailable(seedingInactive.value) && m_ui->checkMaxInactiveSeedingMinutes->isChecked());
        },
        tr("Action performed by the remote server when its configured seeding limits are reached.")
    });
    connect(action, &QComboBox::currentIndexChanged, this, [this]() { remoteFieldChanged(); });

    auto *any = m_ui->radioButtonShareLimitsModeAny;
    auto *all = m_ui->radioButtonShareLimitsModeAll;
    // 5.1.4 has implicit MatchAny and does not advertise this field.
    bindRemoteField(RemoteField{
        QStringLiteral("share_limits_mode"), any,
        [all]() { return QJsonValue(all->isChecked() ? QStringLiteral("MatchAll") : QStringLiteral("MatchAny")); },
        [any, all](const QJsonValue &v) {
            (v.toString() == QLatin1String("MatchAll") ? all : any)->setChecked(true);
        },
        [](const QJsonValue &v) {
            return v.isString() && (v.toString() == QLatin1String("MatchAny")
                || v.toString() == QLatin1String("MatchAll"));
        },
        {}, tr("Match any or all enabled seeding limits on the connected server.")
    });
    connect(any, &QRadioButton::toggled, this, [this]() { remoteFieldChanged(); });
    connect(all, &QRadioButton::toggled, this, [this]() { remoteFieldChanged(); });
}

void RemotePreferencesDialog::refreshSeedingLimitControls()
{
    const auto refresh = [this](SeedingLimitKeys keys, QCheckBox *check, QAbstractSpinBox *spin) {
        const bool supported = remoteFieldAvailable(keys.value);
        spin->setEnabled(supported && check->isChecked());
        spin->setToolTip(check->toolTip());
    };
    refresh(seedingRatio, m_ui->checkMaxRatio, m_ui->spinMaxRatio);
    refresh(seedingTime, m_ui->checkMaxSeedingMinutes, m_ui->spinMaxSeedingMinutes);
    refresh(seedingInactive, m_ui->checkMaxInactiveSeedingMinutes, m_ui->spinMaxInactiveSeedingMinutes);
    m_ui->labelRatioLimitAct->setEnabled(m_ui->comboRatioLimitAct->isEnabled());
    m_ui->radioButtonShareLimitsModeAll->setEnabled(remoteFieldAvailable("share_limits_mode"));
    m_ui->radioButtonShareLimitsModeAll->setToolTip(m_ui->radioButtonShareLimitsModeAny->toolTip());
}

bool RemotePreferencesDialog::validateSeedingLimitChanges(const QJsonObject &changes)
{
    const bool limitChanged = changes.contains(QLatin1String(seedingRatio.value))
        || changes.contains(QLatin1String(seedingTime.value))
        || changes.contains(QLatin1String(seedingInactive.value));
    const bool modeChanged = changes.contains(QStringLiteral("share_limits_mode"));
    const bool actionChanged = changes.contains(QStringLiteral("max_ratio_act"));
    if (!limitChanged && !modeChanged && !actionChanged) return true;

    bool enablingOrEditing = modeChanged || actionChanged;
    for (const auto keys : seedingLimits)
        if (changes.contains(QLatin1String(keys.value))
            && changes.value(QLatin1String(keys.value)).toDouble() >= 0)
            enablingOrEditing = true;

    if (enablingOrEditing && !remoteFieldAvailable("max_ratio_act"))
    {
        QMessageBox::warning(this, tr("Unknown seeding-limit action"),
            tr("The connected server has not supplied a valid action. "
               "Do not enable or change active limits until its action is known."));
        return false;
    }
    // Also warn when changing a threshold or mode while a destructive
    // action is already configured: this can affect existing torrents.
    const int index = m_ui->comboRatioLimitAct->currentIndex();
    if (remoteFieldAvailable("max_ratio_act") && (index == 1 || index == 2))
    {
        return QMessageBox::question(this, tr("Change automatic torrent removal"),
            tr("The connected server may automatically remove torrents%1 when these limits "
               "are met, including existing torrents using global limits. "
               "These actions can occur even when qBittorrent Remote is closed. Apply changes?")
                .arg(index == 2 ? tr(" and delete their downloaded files") : QString()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
    }
    return true;
}

void RemotePreferencesDialog::bindBitTorrentAutomaticTrackers()
{
    // The daemon owns both tracker lists, including their parsing and
    // application to future downloads. Never issue tracker-list HTTP
    // requests or append these trackers from the desktop client.
    bindRemoteControl("add_trackers_enabled", m_ui->checkEnableAddTrackers, {},
        tr("Append trackers to new downloads on the connected server. "
           "Known private torrents are excluded by the server, but trackers "
           "may be contacted during magnet metadata retrieval before privacy is known."));
    bindRemoteControl("add_trackers", m_ui->textTrackers, [this]() {
        return remoteFieldAvailable("add_trackers_enabled") && m_ui->checkEnableAddTrackers->isChecked();
    }, tr("One tracker URL per line. Preserve blank lines separating tiers."));

    bindRemoteControl("add_trackers_from_url_enabled", m_ui->checkAddTrackersFromURL, {},
        tr("The connected server fetches this list and appends trackers to new downloads."));
    auto *url = m_ui->textTrackersURL;
    bindRemoteField(RemoteField{
        QStringLiteral("add_trackers_url"), url,
        [url]() { return QJsonValue(url->text().trimmed()); },
        [url](const QJsonValue &value) { url->setText(value.toString()); },
        [](const QJsonValue &value) { return value.isString(); },
        [this]() {
            return remoteFieldAvailable("add_trackers_from_url_enabled")
                && m_ui->checkAddTrackersFromURL->isChecked();
        },
        tr("Tracker-list source fetched by the connected server, not this desktop.")
    });
    connect(url, &QLineEdit::textChanged, this, [this]() { remoteFieldChanged(); });

    // add_trackers_url_list is GET-only in both supported generations.
    // Keep it outside m_remoteFields so it can never become a write.
    m_ui->textTrackersFromURL->setReadOnly(true);
    m_ui->labeltrackersFromURL->setText(tr("Fetched trackers (last reported by server)"));
}

void RemotePreferencesDialog::refreshAutomaticTrackerControls()
{
    const bool available = m_remotePreferencesLoaded && m_fetchedTrackersAvailable;
    auto *preview = m_ui->textTrackersFromURL;
    preview->setEnabled(available && remoteFieldAvailable("add_trackers_from_url_enabled")
        && m_ui->checkAddTrackersFromURL->isChecked());
    if (!available)
        preview->clear();
    preview->setToolTip(available
        ? tr("Read-only list from the last server preferences response. "
             "The server fetches asynchronously; reopen Preferences for a fresh snapshot.")
        : tr("The connected server has not supplied a fetched tracker list."));
    m_ui->labeltrackersFromURL->setEnabled(preview->isEnabled());
    m_ui->labelCustomizeTrackersListUrl->setEnabled(m_ui->textTrackersURL->isEnabled());
}

bool RemotePreferencesDialog::validateAutomaticTrackerChanges(const QJsonObject &changes)
{
    // An explicitly empty list or URL is valid upstream; a missing or
    // unrepresentable field is not an empty setting.
    if ((changes.value(QStringLiteral("add_trackers_enabled")).toBool()
            && !remoteFieldAvailable("add_trackers"))
        || (changes.value(QStringLiteral("add_trackers_from_url_enabled")).toBool()
            && !remoteFieldAvailable("add_trackers_url")))
    {
        m_ui->tabSelection->setCurrentRow(5);
        QMessageBox::warning(this, tr("Unavailable tracker settings"),
            tr("The connected server has not provided the associated tracker list or source URL."));
        return false;
    }
    return true;
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
        )
        return;
    bool needed = remoteFieldAvailable("i2p_enabled");
    for (const auto &field : m_remoteFields)
        if (m_advancedCapabilityKeys.contains(field.key) && !field.baseline.isUndefined()) needed = true;
    if (!needed) return;

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
        dialog->configureAdvancedCapabilities(info);
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
    }, tr("Absolute IP filter file path on the connected server. Browse remotely, select a mapped NAS file, or enter the server path."));
    installPathBrowser(m_ui->textFilterPath, false,
        tr("All supported filters (*.dat *.p2p *.p2b);;.dat (*.dat);;.p2p (*.p2p);;.p2b (*.p2b)"));
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
    installPathBrowser(m_ui->textFileLogPath);
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
    if (!existing) installPathBrowser(folder);
    installPathBrowser(destination);
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
    bindRemoteControl(key, line, std::move(dependency),
        tr("Absolute directory on the connected server. Type a server path, browse remotely, or select a mapped local NAS directory."));
    installPathBrowser(line);
}

void RemotePreferencesDialog::installPathBrowser(QLineEdit *line, bool directory, const QString &fileFilter)
{
    using Button = RemoteFilesystemBrowseButton;
    auto *button = Button::attachToLineEdit(line);
    button->configurePath(m_settings, [this, line, directory, fileFilter] {
        const auto entries = m_staged.contains(m_remoteProfileId)
            ? m_staged.value(m_remoteProfileId) : m_mappingStore.mappings(m_remoteProfileId);
        return Button::PathContext{m_remoteProfileId, m_remoteTargetName, m_remoteSession.data(),
            entries, line->text(), directory, fileFilter};
    }, [line](const QString &path) {
        if (line->isEnabled() && !line->isReadOnly()) line->setText(path);
    });
    const auto margins = line->textMargins();
    line->setTextMargins(margins.left(), margins.top(),
        margins.right() + button->sizeHint().width() + 4, margins.bottom());
    connect(button, &Button::preferenceChanged, this, [this] {
        for (auto *other : findChildren<Button *>()) other->refresh();
    });
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
        // Clear unavailable Advanced previews on a missing/malformed read or
        // disconnect; never retain an apparently authoritative stale value.
        if (!supported && field.widget->property("advancedRemote").toBool())
        {
            const QSignalBlocker blocker(field.widget);
            if (auto *box = qobject_cast<QCheckBox *>(field.widget))
            {
                box->setTristate(true);
                box->setCheckState(Qt::PartiallyChecked);
            }
            else if (auto *spin = qobject_cast<QSpinBox *>(field.widget))
            {
                spin->setSpecialValueText(tr("Unavailable"));
                spin->setValue(spin->minimum());
            }
            else if (auto *combo = qobject_cast<QComboBox *>(field.widget))
                combo->setCurrentIndex(-1);
            else if (auto *line = qobject_cast<QLineEdit *>(field.widget))
                line->clear();
        }
        if (field.widget->property("advancedRemote").toBool())
            if (auto *line = qobject_cast<QLineEdit *>(field.widget))
                line->setPlaceholderText(supported ? QString() : tr("Unavailable"));
        field.widget->setEnabled(supported && (!field.dependency || field.dependency()));
        field.widget->setToolTip(supported && m_advancedCapabilityKeys.contains(field.key) && !advancedCapabilityAvailable(field.key)
            ? tr("Not supported or not verified for the connected server's build.")
            : supported ? field.tooltip : tr(
            "This setting is unavailable from the connected server. Displayed values are not live."));
        if (auto *combo = qobject_cast<QComboBox *>(field.widget))
            combo->view()->setEnabled(supported);
    }
    m_ui->btnEditRules->setEnabled(m_remotePreferencesLoaded && m_remoteSession && m_remoteSession->isConnected());
    m_ui->label_111->setEnabled(m_ui->spinRSSRefreshInterval->isEnabled());
    m_ui->labelRSSFetchDelay->setEnabled(m_ui->spinRSSFetchDelay->isEnabled());
    m_ui->label_12->setEnabled(m_ui->spinRSSMaxArticlesPerFeed->isEnabled());
    m_ui->label_5->setEnabled(m_ui->textSmartEpisodeFilters->isEnabled());
    m_ui->stopConditionLabel->setEnabled(m_ui->stopConditionComboBox->isEnabled());
    m_ui->lbl_encryption->setEnabled(m_ui->comboEncryption->isEnabled());
    m_ui->labelMaxActiveCheckingTorrents->setEnabled(m_ui->spinBoxMaxActiveCheckingTorrents->isEnabled());
    m_ui->label_max_active_dl->setEnabled(m_ui->spinMaxActiveDownloads->isEnabled());
    m_ui->label_max_active_up->setEnabled(m_ui->spinMaxActiveUploads->isEnabled());
    m_ui->maxActiveTorrents_lbl->setEnabled(m_ui->spinMaxActiveTorrents->isEnabled());
    m_ui->labelDownloadRateForSlowTorrents->setEnabled(m_ui->spinDownloadRateForSlowTorrents->isEnabled());
    m_ui->labelUploadRateForSlowTorrents->setEnabled(m_ui->spinUploadRateForSlowTorrents->isEnabled());
    m_ui->labelSlowTorrentInactivityTimer->setEnabled(m_ui->spinSlowTorrentsInactivityTimer->isEnabled());
    refreshWatchedFolderButtons();
    refreshNotificationControls();
    refreshBackupControls();
    refreshConnectionLimitControls();
    refreshSeedingLimitControls();
    refreshAutomaticTrackerControls();
    refreshI2PControls();
    refreshProxyControls();
    refreshWebUIControls();
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
    m_webUIBaseline = {};
    for (const char *key : {"web_ui_address", "web_ui_port", "use_https", "web_ui_https_cert_path",
             "web_ui_https_key_path", "web_ui_username", "bypass_local_auth",
             "bypass_auth_subnet_whitelist_enabled", "web_ui_reverse_proxy_enabled",
             "web_ui_secure_cookie_enabled", "web_ui_domain_list", "web_ui_host_header_validation_enabled",
             "web_ui_use_custom_http_headers_enabled", "web_ui_custom_http_headers",
             "web_ui_reverse_proxies_list", "bypass_auth_subnet_whitelist"})
        if (values.contains(QLatin1String(key))) m_webUIBaseline.insert(QLatin1String(key), values.value(QLatin1String(key)));
    const auto address = Remote::ServerAddress::parse(m_webUIProfile.url);
    m_webUIDirect = address && address->baseUrl().path() == QLatin1String("/")
        && values.value("web_ui_reverse_proxy_enabled") == QJsonValue(false)
        && values.value("use_https").isBool() && values.value("web_ui_port").isDouble()
        && values.value("web_ui_port").toDouble() == address->baseUrl().port(
            address->baseUrl().scheme() == QLatin1String("https") ? 443 : 80)
        && values.value("use_https").toBool() == (address->baseUrl().scheme() == QLatin1String("https"))
        && values.value("web_ui_address").isString() && values.value("web_ui_username").isString()
        && values.value("web_ui_https_cert_path").isString() && values.value("web_ui_https_key_path").isString()
        && values.value("bypass_local_auth").isBool() && values.value("bypass_auth_subnet_whitelist_enabled").isBool();
    const auto fetchedTrackers = values.value(QStringLiteral("add_trackers_url_list"));
    m_fetchedTrackersAvailable = fetchedTrackers.isString();
    m_ui->textTrackersFromURL->setPlainText(fetchedTrackers.toString());
    QJsonObject availableValues = values;
    // A missing or contradictory flag/value pair is not editable. Do not
    // silently substitute a Designer threshold or overwrite unknown state.
    for (const auto keys : seedingLimits)
    {
        const auto value = values.value(QLatin1String(keys.value));
        const auto enabled = values.value(QLatin1String(keys.enabled));
        if (!value.isDouble() || !enabled.isBool()
            || (enabled.toBool() ? value.toDouble() < 0 : value.toDouble() != -1))
            availableValues.remove(QLatin1String(keys.value));
    }
    // A half-present or invalid time is not an editable setting.
    for (const auto keys : {scheduleFrom, scheduleTo})
    {
        const QString hour = QString::fromLatin1(keys.hour);
        const QString minute = QString::fromLatin1(keys.minute);
        if (!validTimeComponent(values.value(hour), 23)
            || !validTimeComponent(values.value(minute), 59))
        {
            availableValues.remove(hour);
            availableValues.remove(minute);
        }
    }
    for (auto &field : m_remoteFields)
    {
        QJsonValue value = normalizedConnectionLimit(field.key, availableValues.value(field.key));
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
        if (field.key == QLatin1String("web_ui_api_key")) continue;
        if (m_advancedCapabilityKeys.contains(field.key) && !advancedCapabilityAvailable(field.key))
            continue;
        if (!m_webUIDirect && (field.key == QLatin1String("web_ui_address")
            || field.key == QLatin1String("web_ui_port") || field.key == QLatin1String("use_https")
            || field.key == QLatin1String("web_ui_https_cert_path") || field.key == QLatin1String("web_ui_https_key_path")
            || field.key == QLatin1String("web_ui_username"))) continue;
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
    const auto completeTime = [&changes, this](ScheduleTimeKeys keys, const QTime &time) {
        const QString hour = QString::fromLatin1(keys.hour);
        const QString minute = QString::fromLatin1(keys.minute);
        if (!changes.contains(hour) && !changes.contains(minute))
            return;
        if (remoteFieldAvailable(keys.hour) && remoteFieldAvailable(keys.minute))
        {
            changes.insert(hour, time.hour());
            changes.insert(minute, time.minute());
        }
        else
        {
            changes.remove(hour);
            changes.remove(minute);
        }
    };
    completeTime(scheduleFrom, m_ui->timeEditScheduleFrom->time());
    completeTime(scheduleTo, m_ui->timeEditScheduleTo->time());
    for (const auto keys : seedingLimits)
        if (changes.contains(QLatin1String(keys.value)))
            changes.insert(QLatin1String(keys.enabled), changes.value(QLatin1String(keys.value)).toDouble() >= 0);
    if (m_webUIDirect && !m_ui->textWebUIPassword->text().isEmpty())
        changes.insert("web_ui_password", m_ui->textWebUIPassword->text());
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
                dialog->readAdvancedNetworkInterfaces();
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
    refreshWebUIControls();
    updateButtons();
}

void RemotePreferencesDialog::remoteDisconnected()
{
    m_remoteTargetLabel->setText(tr("Server settings: %1 (disconnected)")
        .arg(m_remoteTargetName.isEmpty() ? tr("Server") : m_remoteTargetName));
    m_remotePreferencesLoaded = false;
    refreshRemoteControls();
    if (m_webUITransitionInProgress)
    {
        QTimer::singleShot(1200, this, &RemotePreferencesDialog::probeWebUITransition);
        return;
    }
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

void RemotePreferencesDialog::buildAdvancedPage()
{
    // Native qBittorrent uses two columns and an editor widget in each row.
    // Keep the upstream section order and the union of its conditional rows.
    // The stable row IDs and factories let subsequent API bindings reuse this
    // layout without rebuilding it or guessing the remote daemon's platform.
    auto *table = new AdvancedSettingsTable(m_ui->tabAdvancedPage);
    table->setObjectName(QStringLiteral("advancedSettingsTable"));
    table->setColumnCount(2);
    table->setHorizontalHeaderLabels({tr("Setting"), tr("Value")});
    table->verticalHeader()->hide();
    table->setAlternatingRowColors(true);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setWordWrap(false);
    table->setTextElideMode(Qt::ElideNone);
    table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    table->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    table->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
    table->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    table->horizontalHeader()->setHighlightSections(false);
    table->horizontalHeader()->setMinimumSectionSize(120);
    table->horizontalHeader()->setStretchLastSection(false);
    // Reserve a usable editor column; the setting column wraps instead of
    // forcing the whole value column out of the viewport.
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
    table->horizontalHeader()->resizeSection(1, qMax(180, table->fontMetrics().horizontalAdvance(tr("Open documentation")) + 22));
    table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    const auto unavailable = tr("Not yet available from the selected server. This is not a live value.");
    const auto section = [this, table](const QString &name, const char *url) {
        const int index = table->rowCount();
        table->insertRow(index);
        auto *title = new QTableWidgetItem(name);
        QFont bold = title->font();
        bold.setBold(true);
        title->setFont(bold);
        title->setTextAlignment(Qt::AlignCenter);
        title->setFlags(Qt::ItemIsEnabled);
        title->setBackground(palette().brush(QPalette::AlternateBase));
        table->setItem(index, 0, title);
        auto *link = new QLabel(QStringLiteral("<a href=\"%1\">%2</a>")
            .arg(QString::fromLatin1(url), tr("Open documentation")), table);
        link->setOpenExternalLinks(true);
        link->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
        link->setMargin(1);
        table->setCellWidget(index, 1, link);
    };
    const auto row = [this, table, unavailable](const char *id, const QString &label,
                                                QWidget *editor, const char *url = nullptr) {
        const int index = table->rowCount();
        table->insertRow(index);
        // A transparent cell widget does not suppress the delegate's text.
        // Store metadata outside DisplayRole so the QLabel is the sole
        // renderer of the setting name (including wrapped lines).
        auto *item = new QTableWidgetItem;
        item->setData(Qt::UserRole, label);
        item->setFlags(Qt::ItemIsEnabled);
        item->setToolTip(label);
        table->setItem(index, 0, item);
        editor->setObjectName(QStringLiteral("advanced_") + QString::fromLatin1(id));
        const bool local = editor->property("advancedLocal").toBool();
        editor->setEnabled(local);
        if (!local)
        {
            editor->setToolTip(unavailable);
            item->setToolTip(label + QLatin1Char('\n') + unavailable);
            item->setForeground(palette().brush(QPalette::Disabled, QPalette::Text));
        }
        else
        {
            editor->setToolTip(tr("Local desktop preference. Does not change any server settings."));
            item->setToolTip(editor->toolTip());
        }
        table->setCellWidget(index, 1, editor);
        auto *name = new AdvancedSettingLabel(label, url ? QString::fromLatin1(url) : QString(), editor, table);
        name->setObjectName(QStringLiteral("advancedLabel_") + QString::fromLatin1(id));
        table->setCellWidget(index, 0, name);
    };
    const auto check = [table] {
        auto *editor = new QCheckBox(table);
        editor->setTristate(true);
        editor->setCheckState(Qt::PartiallyChecked);
        return editor;
    };
    const auto spin = [table](int min, int max, const QString &suffix, const QString &special) {
        auto *editor = new QSpinBox(table);
        editor->setRange(min, max);
        editor->setSuffix(suffix);
        editor->setProperty("advancedNativeSpecialValueText", special);
        editor->setSpecialValueText(QObject::tr("Unavailable"));
        editor->setValue(min);
        return editor;
    };
    const auto combo = [table](const QStringList &options) {
        auto *editor = new QComboBox(table);
        editor->addItems(options);
        editor->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        editor->setMinimumContentsLength(10);
        editor->setPlaceholderText(QObject::tr("Unavailable"));
        editor->setCurrentIndex(-1);
        return editor;
    };
    const auto line = [table] {
        auto *editor = new QLineEdit(table);
        editor->setPlaceholderText(QObject::tr("Unavailable"));
        return editor;
    };
    const auto localCheck = [this, table](const char *name, bool fallback) {
        auto *editor = new QCheckBox(table);
        const QString key = QStringLiteral("Preferences/Advanced/") + QString::fromLatin1(name);
        const bool value = m_settings.value(key, fallback).toBool();
        editor->setChecked(value);
        editor->setProperty("advancedLocal", true);
        m_advancedLocalChecks.insert(key, editor);
        m_advancedLocalBaseline.insert(key, value);
        connect(editor, &QCheckBox::toggled, this, [this] { updateButtons(); });
        return editor;
    };

    const auto localNumber = [this, table](const char *name, int fallback, int minimum, int maximum,
                                         const QString &suffix) {
        auto *editor = new QSpinBox(table);
        editor->setRange(minimum, maximum);
        editor->setSuffix(suffix);
        const QString key = QStringLiteral("Preferences/Advanced/") + QString::fromLatin1(name);
        editor->setValue(m_settings.value(key, fallback).toInt());
        editor->setProperty("advancedLocal", true);
        m_advancedLocalNumbers.insert(key, editor);
        m_advancedLocalNumberBaseline.insert(key, editor->value());
        connect(editor, &QSpinBox::valueChanged, this, [this] { updateButtons(); });
        return editor;
    };

    section(tr("qBittorrent Section"), "https://github.com/qbittorrent/qBittorrent/wiki/Explanation-of-Options-in-qBittorrent#Advanced");
    row("RESUME_DATA_STORAGE", tr("Resume data storage type (requires restart)"), combo({tr("Fastresume files"), tr("SQLite database (experimental)")}));
    row("TORRENT_CONTENT_REMOVE_OPTION", tr("Torrent content removing mode"), combo({tr("Delete files permanently"), tr("Move files to trash (if possible)")}));
    row("MEMORY_WORKING_SET_LIMIT", tr("Physical memory (RAM) usage limit"), spin(1, std::numeric_limits<int>::max(), tr(" MiB"), tr("")), "https://wikipedia.org/wiki/Working_set");
    row("OS_MEMORY_PRIORITY", tr("Process memory priority"), combo({tr("Normal"), tr("Below normal"), tr("Medium"), tr("Low"), tr("Very low")}), "https://docs.microsoft.com/en-us/windows/win32/api/processthreadsapi/ns-processthreadsapi-memory_priority_information");
    row("NETWORK_IFACE", tr("Network interface"), combo({tr("Any interface")}));
    row("NETWORK_IFACE_ADDRESS", tr("Optional IP address to bind to"), combo({tr("All addresses"), tr("All IPv4 addresses"), tr("All IPv6 addresses")}));
    row("SAVE_RESUME_DATA_INTERVAL", tr("Save resume data interval [0: disabled]"), spin(0, std::numeric_limits<int>::max(), tr(" min"), tr("0 (disabled)")));
    row("SAVE_STATISTICS_INTERVAL", tr("Save statistics interval [0: disabled]"), spin(0, std::numeric_limits<int>::max(), tr(" min"), tr("0 (disabled)")));
    row("TORRENT_FILE_SIZE_LIMIT", tr(".torrent file size limit"), spin(1, std::numeric_limits<int>::max() / 1024 / 1024, tr(" MiB"), tr("")));
    row("CONFIRM_RECHECK_TORRENT", tr("Confirm torrent recheck"), localCheck("ConfirmTorrentRecheck", false));
    row("RECHECK_COMPLETED", tr("Recheck torrents on completion"), check());
    row("APP_INSTANCE_NAME", tr("Customize application instance name"), line());
    row("LIST_REFRESH", tr("Refresh interval"), localNumber("RefreshInterval", 2000, 30, 99999, tr(" ms")));
    row("RESOLVE_HOSTS", tr("Resolve peer host names"), check());
    row("RESOLVE_COUNTRIES", tr("Resolve peer countries"), check());
    row("PROGRAM_NOTIFICATIONS", tr("Display notifications"), localCheck("DisplayNotifications", true));
    row("TORRENT_ADDED_NOTIFICATIONS", tr("Display notifications for added torrents"), localCheck("NotifyTorrentAdded", false));
    row("NOTIFICATION_TIMEOUT", tr("Notification timeout [0: infinite, -1: system default]"), localNumber("NotificationTimeout", -1, -1, std::numeric_limits<int>::max(), tr(" ms")));
    row("CONFIRM_REMOVE_ALL_TAGS", tr("Confirm removal of all tags"), localCheck("ConfirmRemoveAllTags", true));
    row("CONFIRM_REMOVE_TRACKER_FROM_ALL_TORRENTS", tr("Confirm removal of tracker from all torrents"), localCheck("ConfirmRemoveTrackerFromAllTorrents", true));
    row("REANNOUNCE_WHEN_ADDRESS_CHANGED", tr("Reannounce to all trackers when IP or port changed"), check());
    row("DOWNLOAD_TRACKER_FAVICON", tr("Download tracker's favicon"), check());
    row("SAVE_PATH_HISTORY_LENGTH", tr("Save path history length"), localNumber("SavePathHistoryLength", 8, 0, 99, QString()));
    row("ENABLE_SPEED_WIDGET", tr("Enable speed graphs"), localCheck("EnableSpeedGraphs", true));
    row("ENABLE_ICONS_IN_MENUS", tr("Enable icons in menus"), localCheck("EnableMenuIcons", true));
    row("USE_ATTACHED_ADD_NEW_TORRENT_DIALOG", tr("Attach \"Add new torrent\" dialog to main window"), localCheck("AttachAddTorrentDialog", true));
    row("TRACKER_STATUS", tr("Enable embedded tracker"), check());
    row("TRACKER_PORT", tr("Embedded tracker port"), spin(1, 65535, tr(""), tr("")));
    row("TRACKER_PORT_FORWARDING", tr("Enable port forwarding for embedded tracker"), check());
    row("ENABLE_MARK_OF_THE_WEB", tr("Enable Mark-of-the-Web (MOTW) for downloaded files"), check());
    row("IGNORE_SSL_ERRORS", tr("Ignore SSL errors"), check());
    row("PYTHON_EXECUTABLE_PATH", tr("Python executable path (may require restart)"), line());
    row("START_SESSION_PAUSED", tr("Start BitTorrent session in paused state"), check());
    row("SESSION_SHUTDOWN_TIMEOUT", tr("BitTorrent session shutdown timeout [-1: unlimited]"), spin(-1, std::numeric_limits<int>::max(), tr(" sec"), tr("-1 (unlimited)")));
    section(tr("libtorrent Section"), "https://www.libtorrent.org/reference-Settings.html");
    row("BDECODE_DEPTH_LIMIT", tr("Bdecode depth limit"), spin(0, std::numeric_limits<int>::max(), tr(""), tr("")), "https://www.libtorrent.org/reference-Bdecoding.html#bdecode()");
    row("BDECODE_TOKEN_LIMIT", tr("Bdecode token limit"), spin(0, std::numeric_limits<int>::max(), tr(""), tr("")), "https://www.libtorrent.org/reference-Bdecoding.html#bdecode()");
    row("LOAD_TORRENT_MAX_DIRECTORY_DEPTH", tr("Maximum directory depth when loading torrents"), spin(0, std::numeric_limits<int>::max(), QString(), QString()), "https://www.libtorrent.org/reference-Torrent_Info.html#max_directory_depth");
    row("ASYNC_IO_THREADS", tr("Asynchronous I/O threads"), spin(1, 1024, tr(""), tr("")), "https://www.libtorrent.org/reference-Settings.html#aio_threads");
    row("HASHING_THREADS", tr("Hashing threads"), spin(1, 1024, tr(""), tr("")), "https://www.libtorrent.org/reference-Settings.html#hashing_threads");
    row("FILE_POOL_SIZE", tr("File pool size"), spin(1, std::numeric_limits<int>::max(), tr(""), tr("")), "https://www.libtorrent.org/reference-Settings.html#file_pool_size");
    row("CHECKING_MEM_USAGE", tr("Outstanding memory when checking torrents"), spin(1, 1024, tr(" MiB"), tr("")), "https://www.libtorrent.org/reference-Settings.html#checking_mem_usage");
    row("DISK_CACHE", tr("Disk cache"), spin(-1, 33554431, tr(" MiB"), tr("")), "https://www.libtorrent.org/reference-Settings.html#cache_size");
    row("DISK_CACHE_TTL", tr("Disk cache expiry interval"), spin(1, std::numeric_limits<int>::max(), tr(" s"), tr("")), "https://www.libtorrent.org/reference-Settings.html#cache_expiry");
    row("DISK_QUEUE_SIZE", tr("Disk queue size"), spin(1, std::numeric_limits<int>::max() / 1024, tr(" KiB"), tr("")), "https://www.libtorrent.org/reference-Settings.html#max_queued_disk_bytes");
    row("DISK_IO_TYPE", tr("Disk IO type (requires restart)"), combo({tr("Default"), tr("Memory mapped files"), tr("POSIX-compliant"), tr("Simple pread/pwrite"), tr("Pread/pwrite")}), "https://www.libtorrent.org/single-page-ref.html#default-disk-io-constructor");
    row("DISK_IO_READ_MODE", tr("Disk IO read mode"), combo({tr("Disable OS cache"), tr("Enable OS cache")}), "https://www.libtorrent.org/reference-Settings.html#disk_io_read_mode");
    row("DISK_IO_WRITE_MODE", tr("Disk IO write mode"), combo({tr("Disable OS cache"), tr("Enable OS cache"), tr("Write-through")}), "https://www.libtorrent.org/reference-Settings.html#disk_io_write_mode");
    row("COALESCE_RW", tr("Coalesce reads & writes"), check(), "https://www.libtorrent.org/reference-Settings.html#coalesce_reads");
    row("PIECE_EXTENT_AFFINITY", tr("Use piece extent affinity"), check(), "https://libtorrent.org/single-page-ref.html#piece_extent_affinity");
    row("SUGGEST_MODE", tr("Send upload piece suggestions"), check(), "https://www.libtorrent.org/reference-Settings.html#suggest_mode");
    row("SEND_BUF_WATERMARK", tr("Send buffer watermark"), spin(1, std::numeric_limits<int>::max(), tr(" KiB"), tr("")), "https://www.libtorrent.org/reference-Settings.html#send_buffer_watermark");
    row("SEND_BUF_LOW_WATERMARK", tr("Send buffer low watermark"), spin(1, std::numeric_limits<int>::max(), tr(" KiB"), tr("")), "https://www.libtorrent.org/reference-Settings.html#send_buffer_low_watermark");
    row("SEND_BUF_WATERMARK_FACTOR", tr("Send buffer watermark factor"), spin(1, std::numeric_limits<int>::max(), tr(" %"), tr("")), "https://www.libtorrent.org/reference-Settings.html#send_buffer_watermark_factor");
    row("CONNECTION_SPEED", tr("Outgoing connections per second"), spin(0, std::numeric_limits<int>::max(), tr(""), tr("")), "https://www.libtorrent.org/reference-Settings.html#connection_speed");
    row("SEEDING_OUTGOING_CONNECTIONS", tr("Allow outgoing connections when seeding"), check(), "https://www.libtorrent.org/reference-Settings.html#seeding_outgoing_connections");
    row("SOCKET_SEND_BUFFER_SIZE", tr("Socket send buffer size [0: system default]"), spin(0, std::numeric_limits<int>::max() / 1024, tr(" KiB"), tr("0 (system default)")), "https://www.libtorrent.org/reference-Settings.html#send_socket_buffer_size");
    row("SOCKET_RECEIVE_BUFFER_SIZE", tr("Socket receive buffer size [0: system default]"), spin(0, std::numeric_limits<int>::max() / 1024, tr(" KiB"), tr("0 (system default)")), "https://www.libtorrent.org/reference-Settings.html#recv_socket_buffer_size");
    row("SOCKET_BACKLOG_SIZE", tr("Socket backlog size"), spin(1, std::numeric_limits<int>::max(), tr(""), tr("")), "https://www.libtorrent.org/reference-Settings.html#listen_queue_size");
    row("OUTGOING_PORT_MIN", tr("Outgoing ports (Min) [0: disabled]"), spin(0, 65535, tr(""), tr("0 (disabled)")), "https://www.libtorrent.org/reference-Settings.html#outgoing_port");
    row("OUTGOING_PORT_MAX", tr("Outgoing ports (Max) [0: disabled]"), spin(0, 65535, tr(""), tr("0 (disabled)")), "https://www.libtorrent.org/reference-Settings.html#outgoing_port");
    row("UPNP_LEASE_DURATION", tr("UPnP lease duration [0: permanent lease]"), spin(0, std::numeric_limits<int>::max(), tr(" s"), tr("0 (permanent lease)")), "https://www.libtorrent.org/reference-Settings.html#upnp_lease_duration");
    row("PEER_DSCP", tr("Differentiated Services Code Point (DSCP) for connections to peers"), spin(0, 255, tr(""), tr("")), "https://www.libtorrent.org/reference-Settings.html#peer_dscp");
    row("UTP_MIX_MODE", tr("uTP-TCP mixed mode algorithm"), combo({tr("Prefer TCP"), tr("Peer proportional (throttles TCP)")}), "https://www.libtorrent.org/reference-Settings.html#mixed_mode_algorithm");
    row("HOSTNAME_CACHE_TTL", tr("Internal hostname resolver cache expiry interval"), spin(0, std::numeric_limits<int>::max(), tr(" s"), tr("")), "https://www.libtorrent.org/reference-Settings.html#resolver_cache_timeout");
    row("IDN_SUPPORT", tr("Support internationalized domain name (IDN)"), check(), "https://www.libtorrent.org/reference-Settings.html#allow_idna");
    row("MULTI_CONNECTIONS_PER_IP", tr("Allow multiple connections from the same IP address"), check(), "https://www.libtorrent.org/reference-Settings.html#allow_multiple_connections_per_ip");
    row("MULTI_CONNECTIONS_PER_PEER_ID", tr("Allow multiple connections from the same Peer ID"), check(), "https://www.libtorrent.org/reference-Settings.html#allow_multiple_connections_per_pid");
    row("VALIDATE_HTTPS_TRACKER_CERTIFICATE", tr("Validate HTTPS tracker certificates"), check(), "https://www.libtorrent.org/reference-Settings.html#validate_https_trackers");
    row("SSRF_MITIGATION", tr("Server-side request forgery (SSRF) mitigation"), check(), "https://www.libtorrent.org/reference-Settings.html#ssrf_mitigation");
    row("BLOCK_PEERS_ON_PRIVILEGED_PORTS", tr("Disallow connection to peers on privileged ports"), check(), "https://libtorrent.org/single-page-ref.html#no_connect_privileged_ports");
    row("CHOKING_ALGORITHM", tr("Upload slots behavior"), combo({tr("Fixed slots"), tr("Upload rate based")}), "https://www.libtorrent.org/reference-Settings.html#choking_algorithm");
    row("SEED_CHOKING_ALGORITHM", tr("Upload choking algorithm"), combo({tr("Round-robin"), tr("Fastest upload"), tr("Anti-leech")}), "https://www.libtorrent.org/reference-Settings.html#seed_choking_algorithm");
    row("ANNOUNCE_ALL_TRACKERS", tr("Always announce to all trackers in a tier"), check(), "https://www.libtorrent.org/reference-Settings.html#announce_to_all_trackers");
    row("ANNOUNCE_ALL_TIERS", tr("Always announce to all tiers"), check(), "https://www.libtorrent.org/reference-Settings.html#announce_to_all_tiers");
    row("ANNOUNCE_IP", tr("IP address reported to trackers (requires restart)"), line(), "https://www.libtorrent.org/reference-Settings.html#announce_ip");
    row("ANNOUNCE_PORT", tr("Port reported to trackers (requires restart) [0: listening port]"), spin(0, 65535, tr(""), tr("")), "https://www.libtorrent.org/reference-Settings.html#announce_port");
    row("MAX_CONCURRENT_HTTP_ANNOUNCES", tr("Max concurrent HTTP announces"), spin(0, std::numeric_limits<int>::max(), tr(""), tr("")), "https://www.libtorrent.org/reference-Settings.html#max_concurrent_http_announces");
    row("STOP_TRACKER_TIMEOUT", tr("Stop tracker timeout [0: disabled]"), spin(0, std::numeric_limits<int>::max(), tr(" s"), tr("0 (disabled)")), "https://www.libtorrent.org/reference-Settings.html#stop_tracker_timeout");
    row("PEER_TURNOVER", tr("Peer turnover disconnect percentage"), spin(0, 100, tr(" %"), tr("")), "https://www.libtorrent.org/reference-Settings.html#peer_turnover");
    row("PEER_TURNOVER_CUTOFF", tr("Peer turnover threshold percentage"), spin(0, 100, tr(" %"), tr("")), "https://www.libtorrent.org/reference-Settings.html#peer_turnover");
    row("PEER_TURNOVER_INTERVAL", tr("Peer turnover disconnect interval"), spin(30, 3600, tr(" s"), tr("")), "https://www.libtorrent.org/reference-Settings.html#peer_turnover");
    row("REQUEST_QUEUE_SIZE", tr("Maximum outstanding requests to a single peer"), spin(1, std::numeric_limits<int>::max(), tr(""), tr("")), "https://www.libtorrent.org/reference-Settings.html#max_out_request_queue");
    row("MAX_OUTSTANDING_BLOCK_REQUESTS", tr("Maximum outstanding block requests from a peer"), spin(1, std::numeric_limits<int>::max(), tr(""), tr("")), "https://www.libtorrent.org/reference-Settings.html#max_allowed_in_request_queue");
    row("DHT_BOOTSTRAP_NODES", tr("DHT bootstrap nodes"), line(), "https://www.libtorrent.org/reference-Settings.html#dht_bootstrap_nodes");
    row("WEBTORRENT_STUN_SERVER", tr("STUN server for WebTorrent NAT traversal"), line(), "https://www.libtorrent.org/reference-Settings.html#webtorrent_stun_server");
    row("I2P_INBOUND_QUANTITY", tr("I2P inbound quantity"), spin(1, 16, tr(""), tr("")), "https://www.libtorrent.org/reference-Settings.html#i2p_inbound_quantity");
    row("I2P_OUTBOUND_QUANTITY", tr("I2P outbound quantity"), spin(1, 16, tr(""), tr("")), "https://www.libtorrent.org/reference-Settings.html#i2p_outbound_quantity");
    row("I2P_INBOUND_LENGTH", tr("I2P inbound length"), spin(0, 7, tr(""), tr("")), "https://www.libtorrent.org/reference-Settings.html#i2p_inbound_length");
    row("I2P_OUTBOUND_LENGTH", tr("I2P outbound length"), spin(0, 7, tr(""), tr("")), "https://www.libtorrent.org/reference-Settings.html#i2p_outbound_length");
    row("I2P_INBOUND_LENGTH_VARIANCE", tr("I2P inbound length variance"), spin(-7, 7, tr(""), tr("")), "https://www.libtorrent.org/reference-Settings.html#i2p_inbound_length_variance");
    row("I2P_OUTBOUND_LENGTH_VARIANCE", tr("I2P outbound length variance"), spin(-7, 7, tr(""), tr("")), "https://www.libtorrent.org/reference-Settings.html#i2p_outbound_length_variance");

    // Keep the upstream row definitions for a future supported implementation,
    // but never present permanently nonfunctional controls as preferences.
    for (const char *id : {"advanced_OS_MEMORY_PRIORITY", "advanced_DOWNLOAD_TRACKER_FAVICON"})
    {
        auto *editor = table->findChild<QWidget *>(QString::fromLatin1(id));
        for (int row = 0; editor && row < table->rowCount(); ++row)
            if (table->cellWidget(row, 1) == editor) table->setRowHidden(row, true);
    }
    auto *timeout = table->findChild<QSpinBox *>("advanced_NOTIFICATION_TIMEOUT");
    timeout->setSpecialValueText(tr("-1 (system default)"));
    timeout->setToolTip(tr("Local notification duration in milliseconds. The desktop notification service controls the final display duration."));
    table->findChild<QSpinBox *>("advanced_LIST_REFRESH")->setToolTip(
        tr("Local desktop polling interval for all connected servers. Short intervals increase server and network load."));
    m_ui->advPageLayout->addWidget(table, 1);
    table->scheduleRowHeights();
}

void RemotePreferencesDialog::bindAdvancedRemoteControls()
{
    // API keys, widget IDs and wire units verified against both supported
    // upstream appcontroller.cpp versions. A typed missing/unrepresentable
    // value is never substituted with a local default.
    struct Binding { const char *id; const char *key; int scale; };
    static const Binding bindings[] = {
        {"TORRENT_CONTENT_REMOVE_OPTION", "torrent_content_remove_option", 1},
        {"SAVE_RESUME_DATA_INTERVAL", "save_resume_data_interval", 1},
        {"SAVE_STATISTICS_INTERVAL", "save_statistics_interval", 1},
        {"TORRENT_FILE_SIZE_LIMIT", "torrent_file_size_limit", 1048576},
        {"RECHECK_COMPLETED", "recheck_completed_torrents", 1},
        {"RESOLVE_HOSTS", "resolve_peer_host_names", 1},
        {"RESOLVE_COUNTRIES", "resolve_peer_countries", 1},
        {"REANNOUNCE_WHEN_ADDRESS_CHANGED", "reannounce_when_address_changed", 1},
        {"TRACKER_STATUS", "enable_embedded_tracker", 1},
        {"TRACKER_PORT", "embedded_tracker_port", 1},
        {"TRACKER_PORT_FORWARDING", "embedded_tracker_port_forwarding", 1},
        {"IGNORE_SSL_ERRORS", "ignore_ssl_errors", 1},
        {"BDECODE_DEPTH_LIMIT", "bdecode_depth_limit", 1},
        {"BDECODE_TOKEN_LIMIT", "bdecode_token_limit", 1},
        {"LOAD_TORRENT_MAX_DIRECTORY_DEPTH", "load_torrent_max_directory_depth", 1},
        {"ASYNC_IO_THREADS", "async_io_threads", 1},
        {"FILE_POOL_SIZE", "file_pool_size", 1},
        {"DISK_QUEUE_SIZE", "disk_queue_size", 1024},
        {"PIECE_EXTENT_AFFINITY", "enable_piece_extent_affinity", 1},
        {"SUGGEST_MODE", "enable_upload_suggestions", 1},
        {"SEND_BUF_WATERMARK", "send_buffer_watermark", 1},
        {"SEND_BUF_LOW_WATERMARK", "send_buffer_low_watermark", 1},
        {"SEND_BUF_WATERMARK_FACTOR", "send_buffer_watermark_factor", 1},
        {"CONNECTION_SPEED", "connection_speed", 1},
        {"SEEDING_OUTGOING_CONNECTIONS", "seeding_outgoing_connections", 1},
        {"SOCKET_SEND_BUFFER_SIZE", "socket_send_buffer_size", 1024},
        {"SOCKET_RECEIVE_BUFFER_SIZE", "socket_receive_buffer_size", 1024},
        {"SOCKET_BACKLOG_SIZE", "socket_backlog_size", 1},
        {"OUTGOING_PORT_MIN", "outgoing_ports_min", 1},
        {"OUTGOING_PORT_MAX", "outgoing_ports_max", 1},
        {"UPNP_LEASE_DURATION", "upnp_lease_duration", 1},
        {"PEER_DSCP", "peer_tos", 1},
        {"UTP_MIX_MODE", "utp_tcp_mixed_mode", 1},
        {"HOSTNAME_CACHE_TTL", "hostname_cache_ttl", 1},
        {"IDN_SUPPORT", "idn_support_enabled", 1},
        {"MULTI_CONNECTIONS_PER_IP", "enable_multi_connections_from_same_ip", 1},
        {"MULTI_CONNECTIONS_PER_PEER_ID", "enable_multi_connections_from_same_peer_id", 1},
        {"VALIDATE_HTTPS_TRACKER_CERTIFICATE", "validate_https_tracker_certificate", 1},
        {"SSRF_MITIGATION", "ssrf_mitigation", 1},
        {"BLOCK_PEERS_ON_PRIVILEGED_PORTS", "block_peers_on_privileged_ports", 1},
        {"CHOKING_ALGORITHM", "upload_slots_behavior", 1},
        {"SEED_CHOKING_ALGORITHM", "upload_choking_algorithm", 1},
        {"ANNOUNCE_ALL_TRACKERS", "announce_to_all_trackers", 1},
        {"ANNOUNCE_ALL_TIERS", "announce_to_all_tiers", 1},
        {"MAX_CONCURRENT_HTTP_ANNOUNCES", "max_concurrent_http_announces", 1},
        {"STOP_TRACKER_TIMEOUT", "stop_tracker_timeout", 1},
        {"PEER_TURNOVER", "peer_turnover", 1},
        {"PEER_TURNOVER_CUTOFF", "peer_turnover_cutoff", 1},
        {"PEER_TURNOVER_INTERVAL", "peer_turnover_interval", 1},
        {"REQUEST_QUEUE_SIZE", "request_queue_size", 1},
        {"MAX_OUTSTANDING_BLOCK_REQUESTS", "max_outstanding_block_requests", 1},
        {"DHT_BOOTSTRAP_NODES", "dht_bootstrap_nodes", 1},
        {"WEBTORRENT_STUN_SERVER", "webtorrent_stun_server", 1},
        {"RESUME_DATA_STORAGE", "resume_data_storage_type", 1},
        {"MEMORY_WORKING_SET_LIMIT", "memory_working_set_limit", 1},
        {"APP_INSTANCE_NAME", "app_instance_name", 1},
        {"ENABLE_MARK_OF_THE_WEB", "mark_of_the_web", 1},
        {"PYTHON_EXECUTABLE_PATH", "python_executable_path", 1},
        {"START_SESSION_PAUSED", "start_paused", 1},
        {"SESSION_SHUTDOWN_TIMEOUT", "shutdown_timeout", 1},
        {"HASHING_THREADS", "hashing_threads", 1},
        {"CHECKING_MEM_USAGE", "checking_memory_use", 1},
        {"DISK_CACHE", "disk_cache", 1},
        {"DISK_CACHE_TTL", "disk_cache_ttl", 1},
        {"DISK_IO_TYPE", "disk_io_type", 1},
        {"DISK_IO_READ_MODE", "disk_io_read_mode", 1},
        {"DISK_IO_WRITE_MODE", "disk_io_write_mode", 1},
        {"COALESCE_RW", "enable_coalesce_read_write", 1},
        {"ANNOUNCE_IP", "announce_ip", 1},
        {"ANNOUNCE_PORT", "announce_port", 1},
        {"I2P_INBOUND_QUANTITY", "i2p_inbound_quantity", 1},
        {"I2P_OUTBOUND_QUANTITY", "i2p_outbound_quantity", 1},
        {"I2P_INBOUND_LENGTH", "i2p_inbound_length", 1},
        {"I2P_OUTBOUND_LENGTH", "i2p_outbound_length", 1},
        {"I2P_INBOUND_LENGTH_VARIANCE", "i2p_inbound_length_variance", 1},
        {"I2P_OUTBOUND_LENGTH_VARIANCE", "i2p_outbound_length_variance", 1}
    };
    m_advancedCapabilityKeys = {
        "memory_working_set_limit", "mark_of_the_web", "hashing_threads", "checking_memory_use",
        "disk_cache", "disk_cache_ttl", "disk_io_type", "disk_io_write_mode", "enable_coalesce_read_write",
        "webtorrent_stun_server", "i2p_inbound_quantity", "i2p_outbound_quantity",
        "i2p_inbound_length", "i2p_outbound_length", "i2p_inbound_length_variance", "i2p_outbound_length_variance"
    };
    const auto trackerEnabled = [this] {
        auto *flag = findChild<QCheckBox *>(QStringLiteral("advanced_TRACKER_STATUS"));
        return remoteFieldAvailable("enable_embedded_tracker") && flag && flag->isChecked();
    };
    for (const auto &entry : bindings)
    {
        auto *widget = findChild<QWidget *>(QStringLiteral("advanced_") + QString::fromLatin1(entry.id));
        Q_ASSERT(widget);
        if (!widget) continue;
        widget->setProperty("advancedRemote", true);
        m_advancedRemoteKeys.insert(QString::fromLatin1(entry.key));
        const QString key = QString::fromLatin1(entry.key);
        std::function<bool()> dependency;
        if (key == QLatin1String("embedded_tracker_port") || key == QLatin1String("embedded_tracker_port_forwarding"))
            dependency = trackerEnabled;
        if (m_advancedCapabilityKeys.contains(key))
            dependency = [this, key] { return advancedCapabilityAvailable(key); };
        const auto tooltip = tr("Applies to the selected remote daemon, not this desktop. Use Apply to save.");
        if (auto *box = qobject_cast<QCheckBox *>(widget))
        {
            bindRemoteField(RemoteField{key, box,
                [box] { return QJsonValue(box->isChecked()); },
                [box](const QJsonValue &v) { box->setTristate(false); box->setChecked(v.toBool()); },
                [](const QJsonValue &v) { return v.isBool(); }, dependency, tooltip});
            connect(box, &QCheckBox::toggled, this, [this] { remoteFieldChanged(); });
        }
        else if (auto *spin = qobject_cast<QSpinBox *>(widget))
        {
            const int scale = entry.scale;
            bindRemoteField(RemoteField{key, spin,
                [spin, scale] { return QJsonValue(double(spin->value()) * scale); },
                [spin, scale](const QJsonValue &v) {
                    spin->setSpecialValueText(spin->property("advancedNativeSpecialValueText").toString());
                    spin->setValue(int(v.toDouble() / scale));
                },
                [spin, scale](const QJsonValue &v) {
                    if (!v.isDouble()) return false;
                    const double n = v.toDouble() / scale;
                    return n >= spin->minimum() && n <= spin->maximum() && n == int(n);
                }, dependency, tooltip});
            connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this] { remoteFieldChanged(); });
        }
        else if (key == QLatin1String("torrent_content_remove_option"))
            bindStringCombo(entry.key, qobject_cast<QComboBox *>(widget),
                {QStringLiteral("Delete"), QStringLiteral("MoveToTrash")}, dependency);
        else if (key == QLatin1String("resume_data_storage_type"))
            bindStringCombo(entry.key, qobject_cast<QComboBox *>(widget),
                {QStringLiteral("Legacy"), QStringLiteral("SQLite")}, dependency);
        else
        {
            bindRemoteControl(entry.key, widget, dependency, tooltip);
            if (key == QLatin1String("python_executable_path"))
                if (auto *line = qobject_cast<QLineEdit *>(widget)) installPathBrowser(line, false);
        }
    }
}

bool RemotePreferencesDialog::advancedCapabilityAvailable(const QString &key) const
{
    return m_advancedSupportedKeys.contains(key);
}

void RemotePreferencesDialog::configureAdvancedCapabilities(const QJsonObject &info)
{
    const auto version = QVersionNumber::fromString(info.value("libtorrent").toString());
    const bool known = version.segmentCount() >= 2;
    const bool lt1 = known && version.majorVersion() == 1;
    const bool lt2 = known && version.majorVersion() == 2;
    const bool lt21 = lt2 && version.minorVersion() >= 1;
    const QString platform = info.value("platform").toString();
    const int bits = info.value("bitness").toInt();
    const bool knownBits = bits == 32 || bits == 64;
    m_advancedSupportedKeys.clear();
    const auto permit = [this](const char *key, bool yes) {
        if (yes) m_advancedSupportedKeys.insert(QString::fromLatin1(key));
    };
    permit("hashing_threads", lt2);
    permit("checking_memory_use", knownBits && known);
    permit("disk_cache", lt1 && knownBits);
    permit("disk_cache_ttl", lt1);
    permit("enable_coalesce_read_write", lt1);
    permit("disk_io_type", lt2);
    permit("disk_io_write_mode", lt1 || lt2);
    permit("webtorrent_stun_server", lt21);
    permit("memory_working_set_limit", lt2 && (platform == QLatin1String("windows")
        || (!m_advancedModernServer && platform == QLatin1String("linux"))));
    permit("mark_of_the_web", platform == QLatin1String("windows") || platform == QLatin1String("macos"));
    // The Web API does not disclose TORRENT_USE_I2P. Match the existing
    // I2P preferences rule: libtorrent 2 and a real enabled I2P setting.
    // A positive enable flag is stronger evidence than advertised defaults.
    const auto *i2p = m_ui->groupI2P;
    for (const auto &key : m_advancedCapabilityKeys)
        if (key.startsWith(QLatin1String("i2p_")) && lt2
            && remoteFieldAvailable("i2p_enabled") && i2p->isChecked())
            m_advancedSupportedKeys.insert(key);

    const bool wasLoading = m_loadingRemotePreferences;
    m_loadingRemotePreferences = true;
    for (auto &field : m_remoteFields)
    {
        if (!m_advancedCapabilityKeys.contains(field.key)) continue;
        const QSignalBlocker blocker(field.widget);
        if (field.key == QLatin1String("checking_memory_use") && knownBits)
            qobject_cast<QSpinBox *>(field.widget)->setMaximum(bits == 32 ? 128 : 1024);
        if (field.key == QLatin1String("disk_cache") && knownBits)
            qobject_cast<QSpinBox *>(field.widget)->setMaximum(bits == 32 ? 1536 : 33554431);
        if (field.key == QLatin1String("disk_io_type"))
        {
            auto *combo = qobject_cast<QComboBox *>(field.widget);
            if ((!lt21 || !m_advancedModernServer) && combo->count() == 5) combo->removeItem(4);
        }
        if (field.key == QLatin1String("disk_io_write_mode") && lt1)
        {
            auto *combo = qobject_cast<QComboBox *>(field.widget);
            if (combo->count() == 3) combo->removeItem(2);
        }
        if (!field.baseline.isUndefined())
        {
            if (!field.accepts(field.baseline))
                field.baseline = QJsonValue(QJsonValue::Undefined);
            else
                field.write(field.baseline);
        }
        // Hide only positively incompatible engine/platform rows. Unknown
        // build details remain visibly disabled instead of guessing.
        const bool incompatible = (lt1 && (field.key == QLatin1String("hashing_threads")
                || field.key == QLatin1String("disk_io_type") || field.key.startsWith(QLatin1String("i2p_"))))
            || (lt2 && (field.key == QLatin1String("disk_cache") || field.key == QLatin1String("disk_cache_ttl")
                || field.key == QLatin1String("enable_coalesce_read_write")))
            || (known && !lt21 && field.key == QLatin1String("webtorrent_stun_server"))
            || (!platform.isEmpty() && platform != QLatin1String("unknown")
                && (field.key == QLatin1String("memory_working_set_limit") || field.key == QLatin1String("mark_of_the_web"))
                && !advancedCapabilityAvailable(field.key));
        auto *table = findChild<QTableWidget *>("advancedSettingsTable");
        for (int row = 0; table && row < table->rowCount(); ++row)
            if (table->cellWidget(row, 1) == field.widget) table->setRowHidden(row, incompatible);
    }
    m_loadingRemotePreferences = wasLoading;
}

void RemotePreferencesDialog::bindAdvancedNetworkControls()
{
    auto *iface = findChild<QComboBox *>("advanced_NETWORK_IFACE");
    auto *address = findChild<QComboBox *>("advanced_NETWORK_IFACE_ADDRESS");
    const auto bind = [this](const char *key, QComboBox *combo, std::function<bool()> dependency) {
        combo->clear();
        combo->setProperty("advancedRemote", true);
        m_advancedRemoteKeys.insert(QString::fromLatin1(key));
        bindRemoteField(RemoteField{QString::fromLatin1(key), combo,
            [combo] { return QJsonValue(combo->currentData().toString()); },
            [combo](const QJsonValue &v) {
                const QString value = v.toString();
                int index = combo->findData(value);
                if (index < 0)
                {
                    combo->addItem(value.isEmpty() ? QObject::tr("All / Any") : value, value);
                    index = combo->count() - 1;
                }
                combo->setCurrentIndex(index);
            },
            [](const QJsonValue &v) { return v.isString(); }, std::move(dependency),
            tr("Network binding on the selected server. Changes may interrupt torrent connectivity.")});
    };
    bind("current_network_interface", iface, [this] { return m_advancedInterfacesReady; });
    bind("current_interface_address", address, [this] { return m_advancedAddressesReady && m_advancedInterfacesReady; });
    connect(iface, &QComboBox::currentIndexChanged, this, [this] {
        if (m_loadingRemotePreferences) return;
        readAdvancedNetworkAddresses();
        remoteFieldChanged();
    });
    connect(address, &QComboBox::currentIndexChanged, this, [this] { remoteFieldChanged(); });
}

void RemotePreferencesDialog::readAdvancedNetworkInterfaces()
{
    if (!m_remoteSession || !remoteFieldAvailable("current_network_interface")
        || !remoteFieldAvailable("current_interface_address")) return;
    const QPointer<RemotePreferencesDialog> dialog(this);
    m_remoteSession->requestNetworkInterfaces([dialog](const QJsonArray &values) {
        if (!dialog || !dialog->m_remotePreferencesLoaded) return;
        auto *combo = dialog->findChild<QComboBox *>("advanced_NETWORK_IFACE");
        const QString selected = combo->currentData().toString();
        const QSignalBlocker blocker(combo);
        combo->clear();
        combo->addItem(dialog->tr("Any interface"), QString());
        dialog->m_advancedInterfaceValues = {QString()};
        for (const auto &value : values)
        {
            const auto item = value.toObject();
            const QString id = item.value("value").toString();
            const QString name = item.value("name").toString();
            if (id.isEmpty() || name.isEmpty() || dialog->m_advancedInterfaceValues.contains(id)) continue;
            dialog->m_advancedInterfaceValues.insert(id);
            combo->addItem(name, id);
        }
        if (combo->findData(selected) < 0) combo->addItem(dialog->tr("%1 (current, unavailable)").arg(selected), selected);
        combo->setCurrentIndex(combo->findData(selected));
        dialog->m_advancedInterfacesReady = true;
        dialog->readAdvancedNetworkAddresses();
    }, [dialog](const QString &) {
        if (dialog) { dialog->m_advancedInterfacesReady = false; dialog->refreshRemoteControls(); }
    });
}

void RemotePreferencesDialog::readAdvancedNetworkAddresses()
{
    if (!m_remoteSession || !m_advancedInterfacesReady || !m_remotePreferencesLoaded) return;
    auto *iface = findChild<QComboBox *>("advanced_NETWORK_IFACE");
    auto *combo = findChild<QComboBox *>("advanced_NETWORK_IFACE_ADDRESS");
    const QString selected = iface->currentData().toString();
    const bool changed = m_advancedAddressGeneration > 0 && selected != m_advancedAddressInterface;
    const QString prior = changed ? QString() : combo->currentData().toString();
    m_advancedAddressInterface = selected;
    if (changed)
    {
        const QSignalBlocker blocker(combo);
        combo->clear();
        combo->addItem(tr("All addresses"), QString());
        combo->setCurrentIndex(0);
    }
    m_advancedAddressesReady = false;
    const quint64 generation = ++m_advancedAddressGeneration;
    refreshRemoteControls();
    const QPointer<RemotePreferencesDialog> dialog(this);
    m_remoteSession->requestNetworkAddresses(selected, [dialog, generation, selected, prior](const QJsonArray &values) {
        if (!dialog || !dialog->m_remotePreferencesLoaded || dialog->m_advancedAddressGeneration != generation) return;
        auto *combo = dialog->findChild<QComboBox *>("advanced_NETWORK_IFACE_ADDRESS");
        const QSignalBlocker blocker(combo);
        combo->clear();
        combo->addItem(dialog->tr("All addresses"), QString());
        combo->addItem(dialog->tr("All IPv4 addresses"), QStringLiteral("0.0.0.0"));
        combo->addItem(dialog->tr("All IPv6 addresses"), QStringLiteral("::"));
        dialog->m_advancedAddressValues = {QString(), QStringLiteral("0.0.0.0"), QStringLiteral("::")};
        for (const auto &value : values)
        {
            const QString ip = value.toString();
            if (QHostAddress(ip).isNull() || dialog->m_advancedAddressValues.contains(ip)) continue;
            dialog->m_advancedAddressValues.insert(ip);
            combo->addItem(ip, ip);
        }
        if (combo->findData(prior) < 0) combo->addItem(dialog->tr("%1 (current, unavailable)").arg(prior), prior);
        combo->setCurrentIndex(combo->findData(prior));
        dialog->m_advancedAddressInterface = selected;
        dialog->m_advancedAddressesReady = true;
        dialog->remoteFieldChanged();
    }, [dialog, generation](const QString &) {
        if (!dialog || dialog->m_advancedAddressGeneration != generation) return;
        // Keep interface selection available after a failed address fetch.
        // Validation prevents an unverified binding from being applied.
        dialog->m_advancedAddressesReady = false;
        dialog->refreshRemoteControls();
        dialog->updateButtons();
    });
}

bool RemotePreferencesDialog::validateAdvancedChanges(const QJsonObject &changes)
{
    bool advanced = false;
    for (auto it = changes.constBegin(); it != changes.constEnd(); ++it)
        advanced |= m_advancedRemoteKeys.contains(it.key());
    if (!advanced) return true;
    if (changes.contains("current_network_interface") || changes.contains("current_interface_address"))
    {
        const QString iface = findChild<QComboBox *>("advanced_NETWORK_IFACE")->currentData().toString();
        const QString address = findChild<QComboBox *>("advanced_NETWORK_IFACE_ADDRESS")->currentData().toString();
        if (!m_advancedInterfacesReady || !m_advancedAddressesReady || m_advancedAddressInterface != iface
            || !m_advancedInterfaceValues.contains(iface) || !m_advancedAddressValues.contains(address))
        {
            QMessageBox::warning(this, tr("Invalid server network binding"),
                tr("Choose a verified server interface and address before applying."));
            return false;
        }
    }
    if (changes.contains("outgoing_ports_min") || changes.contains("outgoing_ports_max"))
    {
        const int low = findChild<QSpinBox *>("advanced_OUTGOING_PORT_MIN")->value();
        const int high = findChild<QSpinBox *>("advanced_OUTGOING_PORT_MAX")->value();
        if (!remoteFieldAvailable("outgoing_ports_min") || !remoteFieldAvailable("outgoing_ports_max")
            || (low > 0 && high > 0 && low > high))
        {
            QMessageBox::warning(this, tr("Invalid outgoing ports"),
                tr("The server must expose both outgoing port limits and the minimum must not exceed the maximum."));
            return false;
        }
    }
    return QMessageBox::question(this, tr("Change Advanced server settings"),
        tr("These settings modify the selected server's torrent engine and may affect performance, security or tracker connectivity. Ensure you have independent server access. Settings marked as requiring restart must be activated by restarting the daemon separately; this application will not restart it. Apply the changes?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
}

bool RemotePreferencesDialog::hasAdvancedLocalChanges() const
{
    for (auto it = m_advancedLocalChecks.constBegin(); it != m_advancedLocalChecks.constEnd(); ++it)
        if (it.value()->isChecked() != m_advancedLocalBaseline.value(it.key()))
            return true;
    for (auto it = m_advancedLocalNumbers.constBegin(); it != m_advancedLocalNumbers.constEnd(); ++it)
        if (it.value()->value() != m_advancedLocalNumberBaseline.value(it.key()))
            return true;
    return false;
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
    // Stretching both columns to the viewport elides long paths and leaves
    // nothing to scroll. Size to the complete paths, without growing the dialog.
    m_mappings->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_mappings->horizontalHeader()->setResizeContentsPrecision(-1);
    m_mappings->horizontalHeader()->setHighlightSections(false);
    m_mappings->setWordWrap(false);
    m_mappings->setTextElideMode(Qt::ElideNone);
    m_mappings->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_mappings->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_mappings->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
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
    m_preferMapping = new QPushButton(tr("Make Preferred"), group);
    m_preferMapping->setObjectName(QStringLiteral("preferPathMapping"));
    m_preferMapping->setToolTip(tr("Move this mapping to the top. If multiple mappings match the same local directory, the first equally specific mapping is used for remote destinations."));
    m_checkMount = new QPushButton(tr("Check Local Mount"), group);
    m_checkMount->setObjectName(QStringLiteral("checkLocalMount"));
    for (QPushButton *button : {m_addMapping, m_editMapping,
                                 m_removeMapping, m_preferMapping, m_checkMount})
        row->addWidget(button);
    row->addStretch();
    groupLayout->addLayout(row);

    auto *note = new QLabel(tr(
        "Multiple remote directories may share a local mount. The most specific "
        "mapping wins; for equal local roots, the first listed mapping is preferred. "
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
    QObject::connect(m_preferMapping, &QPushButton::clicked,
                     this, [this]() {
        auto entries = tableMappings();
        const int row = m_mappings->currentRow();
        if (row <= 0 || row >= entries.size() || currentProfileId().isEmpty())
            return;
        entries.prepend(entries.takeAt(row));
        displayMappings(entries);
        m_mappings->selectRow(0);
        stageCurrentMappings();
    });
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
        auto *local = new QTableWidgetItem(QDir::toNativeSeparators(entry.localRoot));
        auto *remote = new QTableWidgetItem(entry.remoteRoot);
        local->setToolTip(local->text());
        remote->setToolTip(remote->text());
        m_mappings->setItem(row, 0, local);
        m_mappings->setItem(row, 1, remote);
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
    auto *session = m_sessionForProfile ? m_sessionForProfile(profileId)
        : (profileId == m_remoteProfileId ? m_remoteSession.data() : nullptr);
    if (!mappingEditor(this, value, m_settings, profileId, m_serverSelector->currentText(), session))
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
    m_preferMapping->setEnabled(selected && m_mappings->currentRow() > 0);
    m_checkMount->setEnabled(selected);
    if (m_applyButton)
        m_applyButton->setEnabled(!m_remoteBusy && (!m_changedProfiles.isEmpty()
                                  || m_behaviorDirty || m_additionDirty || hasAdvancedLocalChanges()
                                  || m_ui->checkRSSLoadExternalImages->isChecked() != m_rssImageBaseline
                                  || !remoteChanges().isEmpty()));
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
    const QSet<QString> transitionKeys{"web_ui_address", "web_ui_port", "use_https",
        "web_ui_https_cert_path", "web_ui_https_key_path", "web_ui_username", "web_ui_password"};
    for (auto it = changes.constBegin(); it != changes.constEnd(); ++it)
        if (transitionKeys.contains(it.key()) || webUISecurityKeys.contains(it.key()))
        {
            applyWebUITransition(changes);
            return;
        }
    if (!validateDownloadChanges(changes) || !validateIPFilterChanges(changes)
        || !validateSpeedChanges(changes) || !validateSeedingLimitChanges(changes)
        || !validateAutomaticTrackerChanges(changes) || !validateRSSChanges(changes)
        || !validateWebUIChanges(changes) || !validateAdvancedChanges(changes))
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
        if ((field.key == QLatin1String("scan_dirs") || field.key == QLatin1String("banned_IPs")
                || field.key == QLatin1String("bypass_auth_subnet_whitelist"))
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
                        : it.key() == QLatin1String("banned_IPs")
                            ? dialog->tr("Banned IP addresses changed on the server. Reopen Preferences to review the new list before editing.")
                            : dialog->tr("The authentication bypass whitelist changed on the server. Reopen Preferences before editing it."));
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

void RemotePreferencesDialog::applyWebUITransition(const QJsonObject &changes)
{
    const QSet<QString> allowed{"web_ui_address", "web_ui_port", "use_https",
        "web_ui_https_cert_path", "web_ui_https_key_path", "web_ui_username", "web_ui_password"};
    bool securityOnly = false;
    for (auto it = changes.constBegin(); it != changes.constEnd(); ++it)
        securityOnly |= webUISecurityKeys.contains(it.key());
    for (auto it = changes.constBegin(); it != changes.constEnd(); ++it)
        if (!(securityOnly ? webUISecurityKeys : allowed).contains(it.key()))
        {
            QMessageBox::warning(this, tr("Apply connection settings separately"),
                tr("Revert the other remote edits and apply them separately from connection changes."));
            return;
        }
    if (!m_changedProfiles.isEmpty() || m_behaviorDirty || m_additionDirty || hasAdvancedLocalChanges()
        || m_ui->checkRSSLoadExternalImages->isChecked() != m_rssImageBaseline)
    {
        QMessageBox::warning(this, tr("Apply connection settings separately"),
            tr("Apply or revert local settings separately before changing the connection."));
        return;
    }
    if ((!securityOnly && !m_webUIDirect) || !m_remoteSession || !m_remoteSession->isConnected()) return;
    const auto address = Remote::ServerAddress::parse(m_webUIProfile.url);
    if (!address) return;
    if (securityOnly)
    {
        const auto touches = [&changes](const char *flag, const char *value) {
            return changes.contains(QLatin1String(flag)) || changes.contains(QLatin1String(value));
        };
        const auto invalid = [this](const QString &message) {
            QMessageBox::warning(this, tr("Invalid WebUI security settings"), message);
        };
        if (touches("web_ui_host_header_validation_enabled", "web_ui_domain_list")
            && m_ui->groupHostHeaderValidation->isChecked())
        {
            if (!remoteFieldAvailable("web_ui_domain_list"))
            {
                invalid(tr("The server does not expose its allowed domain list."));
                return;
            }
            const auto domains = m_ui->textServerDomains->text().split(QLatin1Char(';'), Qt::SkipEmptyParts);
            for (const QString &domain : domains)
            {
                const QString entry = domain.trimmed();
                if (entry.contains(QRegularExpression(QStringLiteral("[\\s/\\\\?#]"))))
                {
                    invalid(tr("Enter domain names or wildcard patterns separated by semicolons, not URLs."));
                    return;
                }
            }
            // A direct hostname is not the listener's literal IP. Avoid
            // knowingly excluding it before sending a potentially locking write.
            QHostAddress literal;
            const QString host = address->baseUrl().host();
            if (m_webUIDirect && !literal.setAddress(host))
            {
                bool match = false;
                for (const QString &domain : domains)
                {
                    const QRegularExpression pattern(
                        QRegularExpression::wildcardToRegularExpression(domain.trimmed(), QRegularExpression::NonPathWildcardConversion),
                        QRegularExpression::CaseInsensitiveOption);
                    match |= pattern.match(host).hasMatch();
                }
                if (!match)
                {
                    invalid(tr("The allowed domains do not include the current server hostname (%1).").arg(host));
                    return;
                }
            }
        }
        if (touches("web_ui_use_custom_http_headers_enabled", "web_ui_custom_http_headers")
            && m_ui->groupWebUIAddCustomHTTPHeaders->isChecked())
        {
            if (!remoteFieldAvailable("web_ui_custom_http_headers"))
            {
                invalid(tr("The server does not expose its custom HTTP headers."));
                return;
            }
            const QRegularExpression name(QStringLiteral("^[!#$%&'*+.^_`|~0-9A-Za-z-]+$"));
            for (const QString &line : m_ui->textWebUICustomHTTPHeaders->toPlainText().split(QLatin1Char('\n')))
            {
                if (line.trimmed().isEmpty()) continue;
                const qsizetype colon = line.indexOf(QLatin1Char(':'));
                if (colon <= 0 || !name.match(line.left(colon).trimmed()).hasMatch()
                    || line.mid(colon + 1).contains(QRegularExpression(QStringLiteral("[\\x00-\\x08\\x0b-\\x1f\\x7f]"))))
                {
                    invalid(tr("Enter a valid HTTP header name and value separated by a colon on each line."));
                    return;
                }
            }
        }
        if (touches("web_ui_reverse_proxy_enabled", "web_ui_reverse_proxies_list")
            && m_ui->groupEnableReverseProxySupport->isChecked())
        {
            bool any = false;
            if (!remoteFieldAvailable("web_ui_reverse_proxies_list"))
            {
                invalid(tr("The server does not expose its trusted proxy list."));
                return;
            }
            for (const QString &value : m_ui->textTrustedReverseProxiesList->text().split(QLatin1Char(';'), Qt::SkipEmptyParts))
            {
                const QString entry = value.trimmed();
                if (entry.isEmpty()) continue;
                QHostAddress ip;
                const auto subnet = QHostAddress::parseSubnet(entry);
                if (!(ip.setAddress(entry) || (entry.contains(QLatin1Char('/')) && !subnet.first.isNull() && subnet.second >= 0)))
                {
                    invalid(tr("Enter valid proxy IP addresses or CIDR subnets separated by semicolons."));
                    return;
                }
                any = true;
            }
            if (!any)
            {
                invalid(tr("Specify at least one trusted proxy IP address or subnet."));
                return;
            }
        }
    }
    const QString binding = m_ui->textWebUIAddress->text();
    QHostAddress ip;
    const QString username = securityOnly ? m_webUIProfile.username : m_ui->textWebUIUsername->text();
    const bool https = securityOnly ? address->baseUrl().scheme() == QLatin1String("https") : m_ui->checkWebUIHttps->isChecked();
    const bool changePassword = changes.contains("web_ui_password");
    const QString newPassword = changes.value("web_ui_password").toString();
    if (!securityOnly && ((binding != QLatin1String("*") && !ip.setAddress(binding))
        || username.size() < 3 || username.contains(QLatin1Char(':')) || username.contains(QChar::Null)
        || (changePassword && (newPassword.size() < 6 || newPassword.contains(QChar::Null)))
        || (https && (m_ui->textWebUIHttpsCert->text().trimmed().isEmpty()
                     || m_ui->textWebUIHttpsKey->text().trimmed().isEmpty()))))
    {
        QMessageBox::warning(this, tr("Invalid connection settings"),
            tr("Enter a valid bind IP or *, a username of at least three characters without a colon, a new password of at least six characters if changing it, and server certificate/key paths for HTTPS."));
        return;
    }
    QUrl candidate = address->baseUrl();
    if (!securityOnly)
    {
        candidate.setScheme(https ? QStringLiteral("https") : QStringLiteral("http"));
        candidate.setPort(m_ui->spinWebUIPort->value());
    }
    if (QMessageBox::question(this, tr("Change server connection"),
        (securityOnly
            ? tr("These WebUI security settings can interrupt access or affect other clients. Trust only proxies you control. A fresh login at %1 will verify continued access; the saved endpoint and credentials will not be changed. Ensure you have independent recovery access.%2\n\nContinue?")
            : tr("This will change the selected server's WebUI settings and close Preferences after a verified reconnect. "
                 "The new connection will use %1. Ensure this is the same server and that you have independent access to restore its settings if the change fails. "
                 "The saved profile will not be changed until a fresh connection confirms the requested values.%2\n\nContinue?"))
            .arg(candidate.toString(), https ? QString() : tr("\n\nHTTP transmits login credentials without TLS protection.")),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    const bool bypassPossible = m_webUIBaseline.value("bypass_local_auth").toBool()
        || m_webUIBaseline.value("bypass_auth_subnet_whitelist_enabled").toBool();
    QString password = newPassword;
    if (!changePassword)
    {
        bool entered = false;
        password = QInputDialog::getText(this, tr("Verify server login"),
            tr("Enter the password for the new connection (blank only if authentication is bypassed):"),
            QLineEdit::Password, QString(), &entered);
        if (!entered) return;
        if (password.isEmpty() && !bypassPossible)
        {
            QMessageBox::warning(this, tr("Password required"), tr("Enter the password for the new connection."));
            return;
        }
    }
    if (!m_remoteSession || !m_remoteSession->isConnected() || !m_remotePreferencesLoaded) return;
    m_webUICandidate = m_webUIProfile;
    if (!securityOnly)
    {
        m_webUICandidate.url = candidate.toString(QUrl::FullyEncoded);
        m_webUICandidate.username = username;
    }
    m_webUISecurityOnly = securityOnly;
    m_webUIExpected = m_webUIBaseline;
    for (auto it = changes.constBegin(); it != changes.constEnd(); ++it)
        if (it.key() != QLatin1String("web_ui_password")) m_webUIExpected.insert(it.key(), it.value());
    m_webUIPassword = password;
    m_webUIPasswordChanged = changePassword;
    m_webUIBypassPossible = bypassPossible;
    m_webUIWriteAcknowledged = false;
    setRemoteBusy(true);
    const QPointer<RemotePreferencesDialog> dialog(this);
    // Check the original endpoint/credentials/security baseline before the
    // single mutation. There is no atomic compare-and-set in qBittorrent.
    m_remoteSession->requestPreferences([dialog, changes](const QJsonObject &values) {
        if (!dialog || !dialog->m_remoteBusy) return;
        const auto baseline = dialog->m_webUIBaseline;
        for (auto it = baseline.constBegin(); it != baseline.constEnd(); ++it)
            if (values.value(it.key()) != it.value())
            {
                dialog->webUITransitionFailed(dialog->tr("The server connection settings changed since Preferences was opened. No update was sent."));
                return;
            }
        if (!dialog->m_remoteSession || !dialog->m_remoteSession->isConnected())
        {
            dialog->webUITransitionFailed(dialog->tr("The original connection is unavailable. No update was sent."));
            return;
        }
        dialog->m_webUITransitionInProgress = true;
        const auto attempt = [dialog] {
            if (dialog) QTimer::singleShot(1200, dialog.data(), &RemotePreferencesDialog::probeWebUITransition);
        };
        // A listener restart may drop the HTTP response after the server has
        // applied the settings. Never blindly retry; probe exactly once.
        dialog->m_remoteSession->setPreferences(changes, [dialog, attempt] {
            if (dialog) dialog->m_webUIWriteAcknowledged = true;
            attempt();
        }, [attempt](const QString &) { attempt(); });
    }, [dialog](const QString &reason) {
        if (dialog && dialog->m_remoteBusy) dialog->webUITransitionFailed(reason);
    });
}

void RemotePreferencesDialog::probeWebUITransition()
{
    if (!m_webUITransitionInProgress || m_webUIProbeStarted) return;
    m_webUIProbeStarted = true;
    auto *probe = new Remote::RemoteSession(this);
    m_webUIProbe = probe;
    Remote::ProfileStore store(m_settings);
    m_webUIFingerprint = store.trustedCertificateFingerprint(m_webUICandidate);
    probe->setCertificateTrust(m_webUIFingerprint, [this](const Remote::CertificateChallenge &challenge) {
        QMessageBox prompt(QMessageBox::Warning, tr("Verify new WebUI certificate"),
            tr("Verify this certificate independently before sending credentials to the changed endpoint."),
            QMessageBox::Yes | QMessageBox::No, this);
        prompt.setTextFormat(Qt::PlainText);
        prompt.setInformativeText(tr("Endpoint: %1\nSHA-256: %2\n%3")
            .arg(challenge.endpoint, challenge.presentedFingerprint,
                 challenge.verificationErrors.join(QLatin1Char('\n'))));
        prompt.setDetailedText(challenge.certificate.toText());
        prompt.setDefaultButton(QMessageBox::No);
        if (prompt.exec() != QMessageBox::Yes) return false;
        m_webUIFingerprint = challenge.presentedFingerprint;
        return true;
    });
    connect(probe, &Remote::RemoteSession::failed, this, &RemotePreferencesDialog::webUITransitionFailed);
    connect(probe, &Remote::RemoteSession::connected, this, [this, probe] {
        const QPointer<RemotePreferencesDialog> dialog(this);
        probe->requestPreferences([dialog, probe](const QJsonObject &values) {
            if (!dialog || !dialog->m_webUITransitionInProgress) return;
            const auto expected = dialog->m_webUIExpected;
            for (auto it = expected.constBegin(); it != expected.constEnd(); ++it)
                if (values.value(it.key()) != it.value())
                {
                    dialog->webUITransitionFailed(dialog->tr("The new endpoint did not confirm the requested server settings."));
                    return;
                }
            // The password is write-only. Both upstream server generations
            // can establish a bypass session before evaluating login credentials.
            // If bypass may apply, require an acknowledged write as well as
            // endpoint/readback verification, and do not claim password proof.
            if (dialog->m_webUIPasswordChanged && dialog->m_webUIBypassPossible)
            {
                if (!dialog->m_webUIWriteAcknowledged)
                {
                    dialog->webUITransitionFailed(dialog->tr("Authentication bypass may have accepted the new connection without checking the password, and the server did not acknowledge the write."));
                    return;
                }
                QMessageBox::information(dialog.data(), dialog->tr("Password verification limited"),
                    dialog->tr("The server accepted the password change and the connection was re-established. Authentication bypass is enabled, so the new password could not be independently verified. Test the password from a client that requires authentication if needed."));
            }
            dialog->m_ui->textWebUIPassword->clear();
            emit dialog->webUITransitionVerified(dialog->m_webUICandidate.url,
                dialog->m_webUICandidate.username, dialog->m_webUIPassword, dialog->m_webUIFingerprint,
                dialog->m_webUISecurityOnly);
            dialog->m_webUIPassword.clear();
            // Logout the temporary session before the main window reconnects.
            QObject::disconnect(probe, nullptr, dialog.data(), nullptr);
            QObject::connect(probe, &QObject::destroyed, dialog.data(), [dialog] {
                if (!dialog) return;
                dialog->m_webUITransitionInProgress = false;
                dialog->setRemoteBusy(false);
                dialog->accept();
            });
            probe->disconnectFromServer();
        }, [dialog](const QString &reason) {
            if (dialog && dialog->m_webUITransitionInProgress) dialog->webUITransitionFailed(reason);
        });
    });
    probe->connectToServer(m_webUICandidate, m_webUIPassword);
}

void RemotePreferencesDialog::webUITransitionFailed(const QString &reason)
{
    // A failed verification is not a rollback. Keep the old saved profile,
    // do not retry credentials, and require independent recovery if needed.
    m_webUITransitionInProgress = false;
    m_webUIPassword.clear();
    m_ui->textWebUIPassword->clear();
    if (m_webUIProbe)
    {
        QObject::disconnect(m_webUIProbe.data(), nullptr, this, nullptr);
        m_webUIProbe->disconnectFromServer();
        m_webUIProbe.clear();
    }
    m_remotePreferencesLoaded = false;
    refreshRemoteControls();
    setRemoteBusy(false);
    QMessageBox::warning(this, tr("Cannot verify connection change"),
        tr("%1\n\nThe saved profile is unchanged. The server may already have applied some changes. Use independent server access or File → Reconnect to recover; do not assume the old settings were restored.").arg(reason));
    reject();
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
        // Save the preference independently of transient platform availability.
        const bool tray = m_ui->checkShowSystray->isChecked();
        if (m_ui->windowStateComboBox->currentData().toString()
                == QLatin1String("hidden")
            && !(tray && QSystemTrayIcon::isSystemTrayAvailable()))
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
        m_settings.setValue(QStringLiteral("Preferences/Behavior/DefaultDeleteTorrentContent"),
            m_ui->checkDefaultDeleteTorrentContent->isChecked());
        m_settings.setValue(QStringLiteral("Preferences/Behavior/FullTrackerURL"),
            m_ui->checkBoxFullTrackerURL->isChecked());
        m_settings.setValue(QStringLiteral("Preferences/Behavior/DecimalUnits"),
            m_ui->checkDecimalUnits->isChecked());
        m_settings.setValue(QStringLiteral("Preferences/Behavior/DateFormat"),
            m_ui->comboDateFormat->currentData().toString());
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        m_settings.setValue(QStringLiteral("Appearance/ColorScheme"), m_ui->comboColorScheme->currentData().toString());
#endif
        m_settings.setValue(QStringLiteral("Preferences/Behavior/ConfirmExit"),
            m_ui->checkProgramExitConfirm->isChecked());
        m_settings.setValue(QStringLiteral("Preferences/Behavior/TorrentContentDragEnabled"),
            m_ui->checkTorrentContentDrag->isChecked());
        m_settings.setValue(QStringLiteral("Preferences/Behavior/ContentDoubleClick"),
            m_ui->comboContentDoubleClick->currentData().toString());
        m_settings.setValue(QStringLiteral("Preferences/Behavior/ContentOpenByEnter"),
            m_ui->checkContentOpenByEnter->isChecked());
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
        m_settings.setValue(QStringLiteral("Preferences/Behavior/ShowExternalIP"),
            m_ui->checkBoxExternalIPStatusBar->isChecked());
        m_settings.setValue(QStringLiteral("Preferences/Behavior/ShowServerFreeSpace"),
            m_ui->checkBoxFreeDiskSpaceStatusBar->isChecked());
        m_settings.setValue(QStringLiteral("Preferences/Behavior/HideZeroStatusFilters"),
            m_ui->checkBoxHideZeroStatusFilters->isChecked());
        m_behaviorDirty = false;
        emit localBehaviorApplied();
    }
    const bool advancedChanged = hasAdvancedLocalChanges();
    for (auto it = m_advancedLocalChecks.constBegin(); it != m_advancedLocalChecks.constEnd(); ++it)
    {
        const bool checked = it.value()->isChecked();
        if (checked != m_advancedLocalBaseline.value(it.key()))
        {
            m_settings.setValue(it.key(), checked);
            m_advancedLocalBaseline.insert(it.key(), checked);
        }
    }
    for (auto it = m_advancedLocalNumbers.constBegin(); it != m_advancedLocalNumbers.constEnd(); ++it)
    {
        const int value = it.value()->value();
        if (value != m_advancedLocalNumberBaseline.value(it.key()))
        {
            m_settings.setValue(it.key(), value);
            m_advancedLocalNumberBaseline.insert(it.key(), value);
            if (it.key() == QLatin1String("Preferences/Advanced/SavePathHistoryLength"))
            {
                m_settings.beginGroup(QStringLiteral("AddTorrent/RemotePathHistory"));
                for (const auto &key : m_settings.allKeys())
                {
                    const auto history = m_settings.value(key).toStringList().mid(0, value);
                    if (history.isEmpty()) m_settings.remove(key);
                    else m_settings.setValue(key, history);
                }
                m_settings.endGroup();
            }
        }
    }
    if (advancedChanged) emit localBehaviorApplied();
    if (m_ui->checkRSSLoadExternalImages->isChecked() != m_rssImageBaseline)
    {
        m_rssImageBaseline = m_ui->checkRSSLoadExternalImages->isChecked();
        m_settings.setValue(QStringLiteral("Preferences/RSS/LoadExternalImages"), m_rssImageBaseline);
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
