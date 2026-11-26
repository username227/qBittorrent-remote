// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDialog>
#include <QJsonObject>
#include <memory>

namespace Ui { class RemoteStatisticsDialog; }
namespace Remote { class RemoteSession; }

class RemoteStatisticsDialog final : public QDialog
{
    Q_OBJECT
public:
    explicit RemoteStatisticsDialog(const QString &serverName, Remote::RemoteSession *session,
        const QJsonObject &state, const QString &version, const QString &apiVersion,
        QWidget *parent = nullptr);
    ~RemoteStatisticsDialog() override;
    void setServerName(const QString &name);
    void updateStatistics(const QJsonObject &state);
private:
    void updateUptime();
    qint64 m_launchTime = -1;
    std::unique_ptr<Ui::RemoteStatisticsDialog> m_ui;
};
