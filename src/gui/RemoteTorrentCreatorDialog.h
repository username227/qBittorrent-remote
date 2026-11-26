// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "api/PathMappingStore.h"
#include "api/RemoteSession.h"
#include <QDialog>
#include <QPointer>
#include <QTimer>
#include <memory>

namespace Ui { class RemoteTorrentCreatorDialog; }
class QDragEnterEvent;
class QDropEvent;
class QLineEdit;
class QPushButton;
class QLabel;

class RemoteTorrentCreatorDialog final : public QDialog
{
    Q_OBJECT
public:
    struct SeedTarget
    {
        Remote::ServerProfile profile;
        QPointer<Remote::RemoteSession> session;
    };
    RemoteTorrentCreatorDialog(QSettings &settings, const Remote::ServerProfile &profile,
        Remote::RemoteSession *session, const QString &version, QWidget *parent = nullptr,
        const QList<SeedTarget> &seedTargets = {});
    ~RemoteTorrentCreatorDialog() override;
    void reject() override;
protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
private:
    enum class State { Loading, Ready, Submitting, Running, Finished, Retrieving, Seeding, Cancelling, Failed, Saved, Uncertain, Disconnected };
    void setState(State state, const QString &message);
    void loadSettings();
    void saveSettings();
    void updateSeedTarget(bool resetPath = false);
    void seedOnTarget(const QString &filename, const QByteArray &bytes, const QString &torrentName);
    void finishCrossSeed(const QString &message);
    void browseSource(bool directory);
    bool selectMappedSource(const QString &local);
    void chooseOutput();
    void createOrRetry();
    void poll();
    void retrieve();
    void removeTask(bool saved);
    void disconnected();
    QString suggestedFilename() const;
    QStringList urlLines(const QString &text, bool tiers, bool *valid) const;
    std::unique_ptr<Ui::RemoteTorrentCreatorDialog> m_ui;
    QPointer<Remote::RemoteSession> m_session;
    QSettings &m_settings;
    QString m_settingsKey;
    bool m_settingsLoaded = false;
    QString m_lastSaveDirectory;
    QList<SeedTarget> m_seedTargets;
    bool m_seedPathEdited = false;
    int m_seedTargetIndex = 0;
    QString m_seedTargetPath;
    QString m_seedResult;
    QList<Remote::PathMapping> m_mappings;
    QTimer m_poll;
    State m_state = State::Loading;
    QString m_taskId;
    QString m_source;
    bool m_modern = false;
    bool m_lt2 = false;
    bool m_seed = false;
    bool m_pollPending = false;
    bool m_finishedTask = false;
    bool m_fileSaved = false;
    QLineEdit *m_output = nullptr;
    QPushButton *m_browseOutput = nullptr;
    QLabel *m_status = nullptr;
};
