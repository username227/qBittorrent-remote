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
    RemoteTorrentCreatorDialog(QSettings &settings, const Remote::ServerProfile &profile,
        Remote::RemoteSession *session, const QString &version, QWidget *parent = nullptr);
    ~RemoteTorrentCreatorDialog() override;
    void reject() override;
protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
private:
    enum class State { Loading, Ready, Submitting, Running, Finished, Retrieving, Cancelling, Failed, Saved, Uncertain, Disconnected };
    void setState(State state, const QString &message);
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
