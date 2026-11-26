// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "api/RemoteSession.h"
#include <QDialog>
#include <QJsonArray>
#include <QPointer>
#include <memory>
class QStandardItemModel;
class QSettings;
class QLabel;
namespace Ui { class RemoteCookiesDialog; }
class RemoteCookiesDialog final : public QDialog
{
    Q_OBJECT
public:
    RemoteCookiesDialog(Remote::RemoteSession *session, const QString &name, QSettings &settings, QWidget *parent = nullptr);
    ~RemoteCookiesDialog() override;
    void reject() override;
private:
    void accept() override;
    void setBusy(bool busy);
    void fail(const QString &message);
    void populate(const QJsonArray &cookies);
    std::unique_ptr<Ui::RemoteCookiesDialog> m_ui;
    QPointer<Remote::RemoteSession> m_session;
    QSettings &m_settings;
    QStandardItemModel *m_model = nullptr;
    QLabel *m_status = nullptr;
    QJsonArray m_original;
    bool m_loaded = false;
    bool m_busy = false;
};
