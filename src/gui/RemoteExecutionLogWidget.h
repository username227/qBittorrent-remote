// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "api/RemoteSession.h"
#include <QWidget>
#include <QPointer>
#include <QTimer>
#include <memory>
class QListWidget;
class QLabel;
namespace Ui { class RemoteExecutionLogWidget; }
class RemoteExecutionLogWidget final : public QWidget
{
    Q_OBJECT
public:
    explicit RemoteExecutionLogWidget(Remote::RemoteSession *session, QWidget *parent = nullptr);
    ~RemoteExecutionLogWidget() override;
    void setMessageTypes(int mask);
private:
    void poll();
    void append(bool peers, const QJsonArray &entries);
    void configureList(QListWidget *list);
    std::unique_ptr<Ui::RemoteExecutionLogWidget> m_ui;
    QPointer<Remote::RemoteSession> m_session;
    QTimer m_timer;
    QListWidget *m_general = nullptr;
    QListWidget *m_peers = nullptr;
    QLabel *m_status = nullptr;
    int m_mainId = -1;
    int m_peerId = -1;
    int m_types = 15;
    bool m_pending = false;
    bool m_readPeersNext = false;
};
