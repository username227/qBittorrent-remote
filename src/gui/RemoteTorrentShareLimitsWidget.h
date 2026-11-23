// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonObject>
#include <QSet>
#include <QWidget>
#include <memory>

namespace Ui { class TorrentShareLimitsWidget; }

// Upstream TorrentShareLimitsWidget UI with remote-backed settings.
class RemoteTorrentShareLimitsWidget final : public QWidget
{
    Q_OBJECT
public:
    explicit RemoteTorrentShareLimitsWidget(QWidget *parent = nullptr);
    ~RemoteTorrentShareLimitsWidget() override;
    void load(const QJsonObject &torrent);
    QJsonObject changes() const;
private:
    std::unique_ptr<Ui::TorrentShareLimitsWidget> m_ui;
    QSet<QString> m_changed;
};
