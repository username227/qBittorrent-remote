// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/PathMappingStore.h"

#include <QDialog>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <memory>

class QComboBox;
class QDialogButtonBox;
class QPushButton;
class QSettings;
class QTableWidget;
namespace Ui { class OptionsDialog; }

// The complete native qBittorrent Preferences layout is retained for
// visual parity, with unsupported controls explicitly disabled. Client-side
// server/path mappings are the only editable page until other settings are
// backed by the selected remote server's authenticated API.
class RemotePreferencesDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit RemotePreferencesDialog(QSettings &settings,
                                     const QString &preferredProfileId = {},
                                     QWidget *parent = nullptr);
    ~RemotePreferencesDialog() override;

private:
    void buildRemotePathsPage(const QString &preferredProfileId);
    void populateAdvancedPlaceholders();
    void disableUnimplementedControls();
    void populateServerSelector(const QString &preferredProfileId);
    void showServerMappings();
    void stageCurrentMappings();
    void editMapping(bool existing);
    void removeMapping();
    void checkLocalMount();
    bool applyChanges();
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
};
