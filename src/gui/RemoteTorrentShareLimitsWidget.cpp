// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/RemoteTorrentShareLimitsWidget.h"
#include "ui_remotetorrentsharelimitswidget.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QSignalBlocker>

RemoteTorrentShareLimitsWidget::RemoteTorrentShareLimitsWidget(QWidget *parent)
    : QWidget(parent), m_ui(std::make_unique<Ui::TorrentShareLimitsWidget>())
{
    m_ui->setupUi(this);
    const auto setupLimit = [this](QComboBox *box, QAbstractSpinBox *value, const QString &key) {
        box->addItems({tr("Default"), tr("Unlimited"), tr("Set to")});
        connect(box, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, value, key](int index) {
                value->setEnabled(index == 2);
                m_changed.insert(key);
            });
    };
    setupLimit(m_ui->comboBoxRatioMode, m_ui->spinBoxRatioValue,
        QStringLiteral("ratio_limit"));
    setupLimit(m_ui->comboBoxSeedingTimeMode, m_ui->spinBoxSeedingTimeValue,
        QStringLiteral("seeding_time_limit"));
    setupLimit(m_ui->comboBoxInactiveSeedingTimeMode, m_ui->spinBoxInactiveSeedingTimeValue,
        QStringLiteral("inactive_seeding_time_limit"));
    m_ui->spinBoxRatioValue->setMaximum(999999999);
    m_ui->comboBoxMode->addItem(tr("Default"), QStringLiteral("Default"));
    m_ui->comboBoxMode->addItem(tr("Match any limit"), QStringLiteral("MatchAny"));
    m_ui->comboBoxMode->addItem(tr("Match all the limits"), QStringLiteral("MatchAll"));
    m_ui->comboBoxAction->addItem(tr("Default"), QStringLiteral("Default"));
    m_ui->comboBoxAction->addItem(tr("Stop torrent"), QStringLiteral("Stop"));
    m_ui->comboBoxAction->addItem(tr("Remove torrent"), QStringLiteral("Remove"));
    m_ui->comboBoxAction->addItem(tr("Remove torrent and its content"),
                                  QStringLiteral("RemoveWithContent"));
    m_ui->comboBoxAction->addItem(tr("Enable super seeding for torrent"),
                                  QStringLiteral("EnableSuperSeeding"));
    connect(m_ui->comboBoxMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
        [this](int) { m_changed.insert(QStringLiteral("share_limits_mode")); });
    connect(m_ui->comboBoxAction, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
        [this](int) { m_changed.insert(QStringLiteral("share_limit_action")); });
    connect(m_ui->spinBoxRatioValue, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
        [this](double) { m_changed.insert(QStringLiteral("ratio_limit")); });
    connect(m_ui->spinBoxSeedingTimeValue, QOverload<int>::of(&QSpinBox::valueChanged), this,
        [this](int) { m_changed.insert(QStringLiteral("seeding_time_limit")); });
    connect(m_ui->spinBoxInactiveSeedingTimeValue, QOverload<int>::of(&QSpinBox::valueChanged), this,
        [this](int) { m_changed.insert(QStringLiteral("inactive_seeding_time_limit")); });
}

RemoteTorrentShareLimitsWidget::~RemoteTorrentShareLimitsWidget() = default;

void RemoteTorrentShareLimitsWidget::load(const QJsonObject &torrent)
{
    const auto loadLimit = [](QComboBox *box, QDoubleSpinBox *ratio, QSpinBox *minutes,
                              const QJsonValue &value) {
        if (!value.isDouble())
        {
            box->setCurrentIndex(-1);
            return;
        }
        const double n = value.toDouble();
        box->setCurrentIndex(n == -2 ? 0 : (n == -1 ? 1 : 2));
        if (n >= 0)
        {
            if (ratio) ratio->setValue(n);
            if (minutes) minutes->setValue(static_cast<int>(n));
        }
    };
    loadLimit(m_ui->comboBoxRatioMode, m_ui->spinBoxRatioValue, nullptr,
        torrent.value(QStringLiteral("ratio_limit")));
    loadLimit(m_ui->comboBoxSeedingTimeMode, nullptr, m_ui->spinBoxSeedingTimeValue,
        torrent.value(QStringLiteral("seeding_time_limit")));
    loadLimit(m_ui->comboBoxInactiveSeedingTimeMode, nullptr,
        m_ui->spinBoxInactiveSeedingTimeValue,
        torrent.value(QStringLiteral("inactive_seeding_time_limit")));
    for (const auto &field : {
        qMakePair(m_ui->comboBoxMode, QStringLiteral("share_limits_mode")),
        qMakePair(m_ui->comboBoxAction, QStringLiteral("share_limit_action"))})
    {
        const int index = field.first->findData(torrent.value(field.second).toString());
        field.first->setCurrentIndex(index >= 0 ? index : -1);
    }
    m_changed.clear();
}

QJsonObject RemoteTorrentShareLimitsWidget::changes() const
{
    QJsonObject result;
    const auto number = [](QComboBox *mode, double configured) {
        return mode->currentIndex() == 0 ? -2.0
             : mode->currentIndex() == 1 ? -1.0 : configured;
    };
    if (m_changed.contains(QStringLiteral("ratio_limit"))
        && m_ui->comboBoxRatioMode->currentIndex() >= 0)
        result.insert(QStringLiteral("ratio_limit"), number(m_ui->comboBoxRatioMode,
            m_ui->spinBoxRatioValue->value()));
    if (m_changed.contains(QStringLiteral("seeding_time_limit"))
        && m_ui->comboBoxSeedingTimeMode->currentIndex() >= 0)
        result.insert(QStringLiteral("seeding_time_limit"), static_cast<int>(
            number(m_ui->comboBoxSeedingTimeMode, m_ui->spinBoxSeedingTimeValue->value())));
    if (m_changed.contains(QStringLiteral("inactive_seeding_time_limit"))
        && m_ui->comboBoxInactiveSeedingTimeMode->currentIndex() >= 0)
        result.insert(QStringLiteral("inactive_seeding_time_limit"), static_cast<int>(
            number(m_ui->comboBoxInactiveSeedingTimeMode,
                m_ui->spinBoxInactiveSeedingTimeValue->value())));
    for (const auto &field : {
        qMakePair(m_ui->comboBoxMode, QStringLiteral("share_limits_mode")),
        qMakePair(m_ui->comboBoxAction, QStringLiteral("share_limit_action"))})
        if (m_changed.contains(field.second) && field.first->currentIndex() >= 0)
            result.insert(field.second, field.first->currentData().toString());
    return result;
}
