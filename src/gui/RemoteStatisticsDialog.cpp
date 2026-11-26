// SPDX-License-Identifier: GPL-3.0-or-later
// Remote adaptation of qBittorrent's statsdialog (Copyright 2013 Nick Tiskov).
#include "gui/RemoteStatisticsDialog.h"
#include "gui/BytePresentation.h"
#include "api/RemoteSession.h"
#include "ui_remotestatisticsdialog.h"

#include <QDateTime>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLocale>
#include <QPointer>
#include <QVersionNumber>
#include <cmath>

namespace
{
QString bytesText(const QJsonValue &value)
{
    return BytePresentation::format(value.toDouble(-1), 2);
}

QString countText(const QJsonValue &value)
{
    if (!value.isDouble() || value.toDouble() < 0 || !std::isfinite(value.toDouble()))
        return QStringLiteral("—");
    return QLocale().toString(value.toDouble(), 'f', 0);
}

QString ratioText(const QJsonValue &value, bool percent)
{
    // Upstream sends locale-independent decimal strings for these metrics.
    bool ok = false;
    const double number = value.isDouble() ? value.toDouble()
        : QLocale::c().toDouble(value.toString(), &ok);
    if (value.isDouble()) ok = true;
    if (!ok || !std::isfinite(number) || number < 0)
        return QStringLiteral("—");
    return QLocale().toString(number, 'f', percent && number == 0 ? 0 : 2)
        + (percent ? QStringLiteral("%") : QString());
}
}

RemoteStatisticsDialog::RemoteStatisticsDialog(const QString &serverName,
    Remote::RemoteSession *session, const QJsonObject &state, const QString &version,
    const QString &apiVersion, QWidget *parent)
    : QDialog(parent), m_ui(std::make_unique<Ui::RemoteStatisticsDialog>())
{
    m_ui->setupUi(this);
    setObjectName(QStringLiteral("remoteStatisticsDialog"));
    setServerName(serverName);
    const auto setVersion = [](QLabel *label, const QString &value) {
        label->setText(value.trimmed().isEmpty() ? QStringLiteral("—") : value);
    };
    setVersion(m_ui->labelServerVersion, version);
    setVersion(m_ui->labelApiVersion, apiVersion);
    connect(m_ui->buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    // libtorrent 2 does not expose meaningful read-cache hits even though
    // the Web API retains the legacy key. Follow the native dialog.
    m_ui->labelCacheHitsText->hide();
    m_ui->labelCacheHits->hide();
    updateStatistics(state);
    if (session)
    {
        const QPointer<RemoteStatisticsDialog> self(this);
        session->requestBuildInfo([self](const QJsonObject &info) {
            if (!self) return;
            const auto libtorrent = info.value("libtorrent").toString().trimmed();
            const auto qt = info.value("qt").toString().trimmed();
            self->m_ui->labelLibtorrentVersion->setText(libtorrent.isEmpty() ? QStringLiteral("—") : libtorrent);
            self->m_ui->labelQtVersion->setText(qt.isEmpty() ? QStringLiteral("—") : qt);
            const auto version = QVersionNumber::fromString(libtorrent);
            if (version.majorVersion() == 1 && version.segmentCount() >= 2)
            {
                self->m_ui->labelCacheHitsText->show();
                self->m_ui->labelCacheHits->show();
            }
        }, [](const QString &) {});
        // Probe this optional read once per dialog. Older servers may return 404.
        // The launch timestamp is server-provided; the duration is a local estimate.
        session->requestProcessInfo([self](const QJsonObject &info) {
            if (!self) return;
            const auto launch = info.value("launch_time").toInteger(-1);
            if (launch <= 0 || launch > QDateTime::currentSecsSinceEpoch()) return;
            const auto time = QDateTime::fromSecsSinceEpoch(launch);
            if (!time.isValid()) return;
            self->m_launchTime = launch;
            self->m_ui->labelLaunchTime->setText(QLocale().toString(time, QLocale::ShortFormat));
            self->updateUptime();
        }, [](const QString &) {});
    }
}

RemoteStatisticsDialog::~RemoteStatisticsDialog() = default;

void RemoteStatisticsDialog::setServerName(const QString &name)
{
    setWindowTitle(tr("Statistics — %1").arg(name));
}

void RemoteStatisticsDialog::updateUptime()
{
    const qint64 seconds = QDateTime::currentSecsSinceEpoch() - m_launchTime;
    if (m_launchTime <= 0 || seconds < 0)
    {
        m_ui->labelUptime->setText(QStringLiteral("—"));
        return;
    }
    const qint64 days = seconds / 86400;
    const auto clock = QStringLiteral("%1:%2:%3")
        .arg((seconds / 3600) % 24, 2, 10, QLatin1Char('0'))
        .arg((seconds / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(seconds % 60, 2, 10, QLatin1Char('0'));
    m_ui->labelUptime->setText(days > 0 ? tr("%1 d %2").arg(days).arg(clock) : clock);
}

void RemoteStatisticsDialog::updateStatistics(const QJsonObject &state)
{
    updateUptime();
    const auto bytes = [&](const char *key) { return bytesText(state.value(QLatin1String(key))); };
    const auto count = [&](const char *key) { return countText(state.value(QLatin1String(key))); };
    const auto percent = [&](const char *key) { return ratioText(state.value(QLatin1String(key)), true); };
    const auto time = [&](const char *key) {
        const auto value = state.value(QLatin1String(key));
        return value.isDouble() && value.toDouble() >= 0 && std::isfinite(value.toDouble())
            ? tr("%1 μs").arg(countText(value)) : QStringLiteral("—");
    };
    m_ui->labelAlltimeUL->setText(bytes("alltime_ul"));
    m_ui->labelAlltimeDL->setText(bytes("alltime_dl"));
    m_ui->labelGlobalRatio->setText(ratioText(state.value("global_ratio"), false));
    m_ui->labelWaste->setText(bytes("total_wasted_session"));
    m_ui->labelPeers->setText(count("total_peer_connections"));
    m_ui->labelCacheHits->setText(percent("read_cache_hits"));
    m_ui->labelTotalBuf->setText(bytes("total_buffers_size"));
    m_ui->labelWriteStarve->setText(percent("write_cache_overload"));
    m_ui->labelReadStarve->setText(percent("read_cache_overload"));
    m_ui->labelQueuedJobs->setText(count("queued_io_jobs"));
    m_ui->labelJobsTime->setText(time("average_time_queue"));
    m_ui->labelQueuedBytes->setText(bytes("total_queued_size"));
    m_ui->labelRequestLatency->setText(time("request_latency"));
    m_ui->labelQueuedTrackerAnnounces->setText(count("queued_tracker_announces"));
}
