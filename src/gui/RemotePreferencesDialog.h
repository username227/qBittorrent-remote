// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/PathMappingStore.h"

#include <QDialog>
#include <QHash>
#include <QJsonObject>
#include <QPointer>
#include <QList>
#include <QSet>
#include <QString>
#include <memory>
#include <functional>

class QComboBox;
class QDialogButtonBox;
class QPushButton;
class QSettings;
class QTableWidget;
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
                                     Remote::RemoteSession *session = nullptr);
    ~RemotePreferencesDialog() override;

signals:
    void localBehaviorApplied();

private:
    void buildRemotePathsPage(const QString &preferredProfileId);
    void populateAdvancedPlaceholders();
    void disableUnimplementedControls();
    void loadLocalBehaviorControls();
    void bindServerBehaviorControls();
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
    Remote::PathMappingStore m_mappingStore;
    std::unique_ptr<Ui::OptionsDialog> m_ui;
    QComboBox *m_serverSelector = nullptr;
    QTableWidget *m_mappings = nullptr;
    QPushButton *m_addMapping = nullptr;
    QPushButton *m_editMapping = nullptr;
    QPushButton *m_removeMapping = nullptr;
    QPushButton *m_checkMount = nullptr;
    QPushButton *m_applyButton = nullptr;
    QHash<QString, QList<Remote::PathMapping>> m_staged;
    QSet<QString> m_changedProfiles;
    bool m_behaviorDirty = false;
    QList<RemoteField> m_remoteFields;
    bool m_remotePreferencesLoaded = false;
    bool m_loadingRemotePreferences = false;
    bool m_remoteBusy = false;
    bool m_closeAfterApply = false;
    QPointer<Remote::RemoteSession> m_remoteSession;
};
