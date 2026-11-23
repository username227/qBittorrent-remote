// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDialog>
#include <QHash>
#include <QJsonObject>
#include <QPointer>
#include <memory>

class QString;
namespace Remote { class RemoteSession; }
namespace Ui { class SpeedLimitDialog; }

// The original qBittorrent speed-limit UI, with values fetched from the
// authenticated remote qBittorrent instance. Never writes local defaults.
class RemoteSpeedLimitsDialog final : public QDialog
{
    Q_OBJECT
public:
    explicit RemoteSpeedLimitsDialog(Remote::RemoteSession *session,
                                     QWidget *parent = nullptr);
    ~RemoteSpeedLimitsDialog() override;

protected:
    void accept() override;

private:
    void loadLimits(const QJsonObject &preferences);
    void reportLoadError(const QString &message);

    std::unique_ptr<Ui::SpeedLimitDialog> m_ui;
    QPointer<Remote::RemoteSession> m_session;
    QHash<QString, int> m_initialKiB;
    bool m_ready = false;
};
