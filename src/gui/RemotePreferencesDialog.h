// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/PathMappingStore.h"
#include "api/ServerProfile.h"

#include <QDialog>
#include <QHash>
#include <QJsonObject>
#include <QPointer>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <memory>
#include <functional>

class QAction;
class QComboBox;
class QCheckBox;
class QLineEdit;
class QLabel;
class QDialogButtonBox;
class QPushButton;
class QSettings;
class QTableWidget;
class QStandardItemModel;
class TorrentModel;
namespace Ui { class OptionsDialog; }
namespace Remote { class RemoteSession; }

// The complete native qBittorrent Preferences layout is retained for
// visual parity. Local preferences stay local; remote controls are enabled
// only from the authenticated session's actual, representable values.
class RemotePreferencesDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit RemotePreferencesDialog(QSettings &settings,
                                     const QString &preferredProfileId = {},
                                     QWidget *parent = nullptr,
                                     Remote::RemoteSession *session = nullptr,
                                     TorrentModel *torrentModel = nullptr,
                                     const QString &serverVersion = {});
    ~RemotePreferencesDialog() override;

signals:
    void localBehaviorApplied();
    void webUITransitionVerified(const QString &url, const QString &username,
                                 const QString &password, const QString &fingerprint, bool securityOnly);

private:
    void buildRemotePathsPage(const QString &preferredProfileId);
    void populateAdvancedPlaceholders();
    void disableUnimplementedControls();
    void loadLocalBehaviorControls();
    void loadLocalAdditionControls();
    void bindServerBehaviorControls();
    void bindConnectionListeningControls();
    void bindConnectionLimits();
    void refreshConnectionLimitControls();
    void bindI2PControls();
    void bindProxyControls();
    void refreshProxyControls();
    void bindIPFilterControls();
    void bindSpeedControls();
    void bindRSSControls();
    void bindWebUIControls();
    void refreshWebUIControls();
    bool validateWebUIChanges(const QJsonObject &changes);
    void applyWebUITransition(const QJsonObject &changes);
    void probeWebUITransition();
    void webUITransitionFailed(const QString &reason);
    bool validateRSSChanges(const QJsonObject &changes);
    void bindBitTorrentPrivacyControls();
    void bindBitTorrentQueueingControls();
    void bindBitTorrentSeedingLimits();
    void bindBitTorrentAutomaticTrackers();
    void refreshAutomaticTrackerControls();
    bool validateAutomaticTrackerChanges(const QJsonObject &changes);
    void refreshSeedingLimitControls();
    bool validateSeedingLimitChanges(const QJsonObject &changes);
    bool validateSpeedChanges(const QJsonObject &changes);
    bool validateIPFilterChanges(const QJsonObject &changes);
    void refreshI2PControls();
    void readI2PBuildInfo();
    void bindDownloadsSavingControls();
    void bindTorrentAdditionDefaults();
    void bindBackupAndWatchedFolders();
    void refreshBackupControls();
    void bindExcludedFileNames();
    void bindNotificationAndProgramControls();
    void refreshNotificationControls();
    void sendTestEmail();
    bool hasPendingMailChanges() const;
    void displayWatchedFolders();
    void refreshWatchedFolderButtons();
    void editWatchedFolder(bool existing);
    void sendRemoteChanges(const QJsonObject &changes);
    void bindStringCombo(const char *key, QComboBox *combo, const QStringList &values,
                         std::function<bool()> dependency = {});
    void bindBooleanCombo(const char *key, QComboBox *combo, int trueIndex,
                          const QString &tooltip);
    void bindRemotePath(const char *key, QLineEdit *line,
                        std::function<bool()> dependency = {});
    void browseRemotePath(QLineEdit *line, bool filterFile = false);
    bool validateDownloadChanges(const QJsonObject &changes);
    void remoteFieldChanged();
    // Register typed controls, including custom enum/compound bindings for
    // later tabs. A field is available only after a valid server value.
    struct RemoteField
    {
        QString key;
        QWidget *widget;
        std::function<QJsonValue()> read;
        std::function<void(const QJsonValue &)> write;
        std::function<bool(const QJsonValue &)> accepts;
        std::function<bool()> dependency;
        QString tooltip;
        QJsonValue baseline = QJsonValue(QJsonValue::Undefined);
    };
    void bindRemoteField(RemoteField field);
    void bindRemoteControl(const char *key, QWidget *widget,
                           std::function<bool()> dependency = {}, QString tooltip = {});
    bool remoteFieldAvailable(const char *key) const;
    void loadRemotePreferences();
    void populateRemoteFields(const QJsonObject &values, const QJsonObject &staged = {});
    void refreshRemoteControls();
    QJsonObject remoteChanges() const;
    void setRemoteBusy(bool busy);
    void remoteDisconnected();
    void remoteSaveFailed(const QString &reason);
    void verifyRemoteChanges(const QJsonObject &changes);
    void populateServerSelector(const QString &preferredProfileId);
    void showServerMappings();
    void stageCurrentMappings();
    void editMapping(bool existing);
    void removeMapping();
    void checkLocalMount();
    void applyChanges(bool closeAfter);
    bool validateLocalChanges();
    bool applyLocalChanges();
    void finishApply();
    void updateButtons();
    QString currentProfileId() const;
    QList<Remote::PathMapping> tableMappings() const;
    void displayMappings(const QList<Remote::PathMapping> &mappings);
    void done(int result) override;

    QSettings &m_settings;
    const QString m_remoteProfileId;
    Remote::PathMappingStore m_mappingStore;
    std::unique_ptr<Ui::OptionsDialog> m_ui;
    QLabel *m_remoteTargetLabel = nullptr;
    QString m_remoteTargetName;
    QComboBox *m_serverSelector = nullptr;
    QTableWidget *m_mappings = nullptr;
    QPushButton *m_addMapping = nullptr;
    QPushButton *m_editMapping = nullptr;
    QPushButton *m_removeMapping = nullptr;
    QPushButton *m_preferMapping = nullptr;
    QPushButton *m_checkMount = nullptr;
    QPushButton *m_applyButton = nullptr;
    QHash<QString, QList<Remote::PathMapping>> m_staged;
    QSet<QString> m_changedProfiles;
    bool m_behaviorDirty = false;
    bool m_rssImageBaseline = false;
    QString m_webUISubnets;
    Remote::ServerProfile m_webUIProfile, m_webUICandidate;
    QJsonObject m_webUIBaseline, m_webUIExpected;
    QString m_webUIPassword, m_webUIFingerprint;
    bool m_webUIDirect = false;
    bool m_webUITransitionInProgress = false;
    bool m_webUIProbeStarted = false;
    bool m_webUIPasswordChanged = false;
    bool m_webUIBypassPossible = false;
    bool m_webUIWriteAcknowledged = false;
    bool m_webUISecurityOnly = false;
    QPointer<Remote::RemoteSession> m_webUIProbe;
    bool m_additionDirty = false;
    QStandardItemModel *m_watchedFoldersModel = nullptr;
    QJsonObject m_watchedFolders;
    QList<RemoteField> m_remoteFields;
    QString m_bannedIPs;
    bool m_banRangesSupported = false;
    bool m_i2pBuildInfoFinished = false;
    bool m_i2pVersionSupportsConfig = false;
    bool m_i2pVersionSupportsPeX = false;
    QString m_i2pLibtorrentVersion;
    bool m_fetchedTrackersAvailable = false;
    bool m_remotePreferencesLoaded = false;
    bool m_loadingRemotePreferences = false;
    bool m_remoteBusy = false;
    bool m_testEmailPending = false;
    QCheckBox *m_legacySmtpSsl = nullptr;
    bool m_closeAfterApply = false;
    QPointer<Remote::RemoteSession> m_remoteSession;
    QPointer<TorrentModel> m_torrentModel;
};
