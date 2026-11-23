// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/RemoteSpeedLimitsDialog.h"
#include "api/RemoteSession.h"
#include "ui_speedlimitdialog.h"

#include <QDialogButtonBox>
#include <QIcon>
#include <QMessageBox>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QJsonValue>
#include <QJsonObject>
#include <QtGlobal>

RemoteSpeedLimitsDialog::RemoteSpeedLimitsDialog(Remote::RemoteSession *session,
                                                 QWidget *parent)
    : QDialog(parent)
    , m_ui(std::make_unique<Ui::SpeedLimitDialog>())
    , m_session(session)
{
    m_ui->setupUi(this); // The original qBittorrent 5.3.0 .ui, not a replica.
    setObjectName(QStringLiteral("remoteSpeedLimitsDialog"));
    m_ui->labelGlobalSpeedIcon->setPixmap(
        QIcon(QStringLiteral(":/icons/slow_off.svg")).pixmap(22, 22));
    m_ui->labelAltGlobalSpeedIcon->setPixmap(
        QIcon(QStringLiteral(":/icons/slow.svg")).pixmap(22, 22));
    m_ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(false);
    connect(m_ui->buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_ui->buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    const auto bindPair = [](QSlider *slider, QSpinBox *spin) {
        slider->setMaximum(10000);
        connect(slider, &QSlider::valueChanged, spin, &QSpinBox::setValue);
        connect(spin, QOverload<int>::of(&QSpinBox::valueChanged),
                slider, [slider](int value) {
                    if (value > slider->maximum()) slider->setMaximum(value);
                    slider->setValue(value);
                });
    };
    bindPair(m_ui->sliderUploadLimit, m_ui->spinUploadLimit);
    bindPair(m_ui->sliderDownloadLimit, m_ui->spinDownloadLimit);
    bindPair(m_ui->sliderAltUploadLimit, m_ui->spinAltUploadLimit);
    bindPair(m_ui->sliderAltDownloadLimit, m_ui->spinAltDownloadLimit);

    if (!m_session || !m_session->isConnected())
    {
        reportLoadError(tr("The selected server is disconnected."));
        return;
    }
    connect(m_session, &Remote::RemoteSession::preferencesReceived,
            this, &RemoteSpeedLimitsDialog::loadLimits);
    connect(m_session, &Remote::RemoteSession::preferencesFailed,
            this, &RemoteSpeedLimitsDialog::reportLoadError);
    m_session->requestPreferences();
}

RemoteSpeedLimitsDialog::~RemoteSpeedLimitsDialog() = default;

void RemoteSpeedLimitsDialog::loadLimits(const QJsonObject &values)
{
    const struct {const char *key; QSpinBox *spin; QSlider *slider;} fields[] = {
        {"up_limit", m_ui->spinUploadLimit, m_ui->sliderUploadLimit},
        {"dl_limit", m_ui->spinDownloadLimit, m_ui->sliderDownloadLimit},
        {"alt_up_limit", m_ui->spinAltUploadLimit, m_ui->sliderAltUploadLimit},
        {"alt_dl_limit", m_ui->spinAltDownloadLimit, m_ui->sliderAltDownloadLimit}
    };
    for (const auto &field : fields)
    {
        const QJsonValue raw = values.value(QString::fromLatin1(field.key));
        if (!raw.isDouble() || raw.toDouble() < 0
            || raw.toDouble() > 2000000.0 * 1024.0)
        {
            reportLoadError(tr("The server did not supply a valid %1 rate limit.")
                .arg(QString::fromLatin1(field.key)));
            return;
        }
        const int rateKiB = static_cast<int>(raw.toDouble() / 1024);
        field.slider->setMaximum(qMax(10000, rateKiB));
        field.spin->setValue(rateKiB);
        m_initialKiB.insert(QString::fromLatin1(field.key), rateKiB);
    }
    m_ready = true;
    m_ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(true);
}

void RemoteSpeedLimitsDialog::reportLoadError(const QString &message)
{
    m_ready = false;
    m_ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(false);
    QMessageBox::warning(this, tr("Cannot read remote speed limits"), message);
    reject();
}

void RemoteSpeedLimitsDialog::accept()
{
    if (!m_ready || !m_session || !m_session->isConnected())
    {
        QMessageBox::warning(this, tr("Cannot change remote speed limits"),
                             tr("The selected server is no longer connected."));
        return;
    }
    const struct {const char *key; QSpinBox *spin;} fields[] = {
        {"up_limit", m_ui->spinUploadLimit},
        {"dl_limit", m_ui->spinDownloadLimit},
        {"alt_up_limit", m_ui->spinAltUploadLimit},
        {"alt_dl_limit", m_ui->spinAltDownloadLimit}
    };
    QJsonObject changes;
    for (const auto &field : fields)
    {
        const QString key = QString::fromLatin1(field.key);
        if (field.spin->value() != m_initialKiB.value(key))
            changes.insert(key, field.spin->value() * 1024);
    }
    if (!changes.isEmpty())
        m_session->setPreferences(changes); // Only user-edited fields, on this NAS.
    QDialog::accept();
}
