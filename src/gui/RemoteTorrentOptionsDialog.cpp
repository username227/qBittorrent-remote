// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/RemoteTorrentOptionsDialog.h"

#include "api/RemoteSession.h"
#include "gui/RemoteFileSystemPathComboEdit.h"
#include "gui/RemoteTorrentShareLimitsWidget.h"
#include "ui_remotetorrentoptionsdialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QJsonValue>
#include <QLineEdit>
#include <QMessageBox>
#include <QSlider>
#include <QSpinBox>
#include <algorithm>
#include <utility>

RemoteTorrentOptionsDialog::RemoteTorrentOptionsDialog(
    Remote::RemoteSession *session, QVector<QPair<QString, QJsonObject>> torrents,
    QStringList categories, QList<Remote::PathMapping> mappings,
    std::function<bool()> selectionStillValid, QWidget *parent)
    : QDialog(parent), m_ui(std::make_unique<Ui::TorrentOptionsDialog>())
    , m_session(session), m_torrents(std::move(torrents))
    , m_categories(std::move(categories)), m_mappings(std::move(mappings))
    , m_selectionStillValid(std::move(selectionStillValid))
{
    m_ui->setupUi(this); // Native upstream layout, minus unsupported DHT/PeX/LSD.
    setObjectName(QStringLiteral("remoteTorrentOptionsDialog"));
    connect(m_ui->buttonBox, &QDialogButtonBox::accepted,
        this, &RemoteTorrentOptionsDialog::accept);
    connect(m_ui->buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    if (m_torrents.isEmpty())
        return;

    const QJsonObject first = m_torrents.first().second;
    const auto allSame = [this, first](const QString &key) {
        return std::all_of(m_torrents.cbegin(), m_torrents.cend(),
            [first, key](const auto &torrent) {
                return torrent.second.value(key) == first.value(key);
            });
    };
    const auto check = [this, &first, &allSame](
        QCheckBox *widget, const QString &source, const QString &touch) {
        if (allSame(source) && first.value(source).isBool())
            widget->setChecked(first.value(source).toBool());
        else
        {
            widget->setTristate(true);
            widget->setCheckState(Qt::PartiallyChecked);
        }
        connect(widget, &QCheckBox::clicked, this,
            [this, touch](bool) { m_touched.insert(touch); });
    };
    check(m_ui->checkAutoTMM, QStringLiteral("auto_tmm"), QStringLiteral("auto"));
    check(m_ui->checkSequential, QStringLiteral("seq_dl"), QStringLiteral("seq"));
    check(m_ui->checkFirstLastPieces, QStringLiteral("f_l_piece_prio"),
          QStringLiteral("firstlast"));

    if (allSame(QStringLiteral("save_path")))
        m_ui->savePath->setEditText(first.value(QStringLiteral("save_path")).toString());
    if (allSame(QStringLiteral("download_path")))
        m_ui->downloadPath->setEditText(first.value(QStringLiteral("download_path")).toString());
    const bool usesDownloadPath = !first.value(QStringLiteral("download_path")).toString().isEmpty();
    if (allSame(QStringLiteral("download_path")))
        m_ui->checkUseDownloadPath->setChecked(usesDownloadPath);
    else
    {
        m_ui->checkUseDownloadPath->setTristate(true);
        m_ui->checkUseDownloadPath->setCheckState(Qt::PartiallyChecked);
    }
    m_ui->savePath->setToolTip(tr("Path on the remote server"));
    m_ui->downloadPath->setToolTip(tr("Incomplete-torrent path on the remote server"));
    for (const Remote::PathMapping &mapping : std::as_const(m_mappings))
    {
        m_ui->savePath->addItem(mapping.remoteRoot);
        m_ui->downloadPath->addItem(mapping.remoteRoot);
    }
    connect(m_ui->savePath->lineEdit(), &QLineEdit::textEdited, this,
        [this](const QString &) { m_touched.insert(QStringLiteral("save")); });
    connect(m_ui->downloadPath->lineEdit(), &QLineEdit::textEdited, this,
        [this](const QString &) { m_touched.insert(QStringLiteral("download")); });
    const auto browse = [this](RemoteFileSystemPathComboEdit *widget, const QString &touch) {
        if (m_mappings.isEmpty())
        {
            QMessageBox::information(this, tr("No path mappings"),
                tr("Enter a server path, or configure local NAS mappings in Preferences."));
            return;
        }
        const QString local = QFileDialog::getExistingDirectory(this,
            tr("Choose mapped local NAS directory"), m_mappings.first().localRoot);
        if (local.isEmpty())
            return;
        const QString path = Remote::PathMappingStore::toRemote(local, m_mappings);
        if (path.isEmpty())
        {
            QMessageBox::warning(this, tr("Unmapped directory"),
                tr("The selected folder is not mapped to the remote server."));
            return;
        }
        widget->setEditText(path);
        m_touched.insert(touch);
    };
    connect(m_ui->savePath, &RemoteFileSystemPathComboEdit::browseRequested, this,
        [this, browse]() { browse(m_ui->savePath, QStringLiteral("save")); });
    connect(m_ui->downloadPath, &RemoteFileSystemPathComboEdit::browseRequested, this,
        [this, browse]() { browse(m_ui->downloadPath, QStringLiteral("download")); });
    connect(m_ui->checkUseDownloadPath, &QCheckBox::clicked, this,
        [this](bool enabled) {
            m_touched.insert(QStringLiteral("downloadEnable"));
            m_ui->downloadPath->setEnabled(enabled && !m_ui->checkAutoTMM->isChecked());
        });
    connect(m_ui->checkAutoTMM, &QCheckBox::clicked, this, [this](bool enabled) {
        m_ui->savePath->setEnabled(!enabled);
        m_ui->checkUseDownloadPath->setEnabled(!enabled);
        m_ui->downloadPath->setEnabled(!enabled
            && m_ui->checkUseDownloadPath->isChecked());
    });
    const bool automatic = m_ui->checkAutoTMM->isChecked();
    m_ui->savePath->setEnabled(!automatic);
    m_ui->checkUseDownloadPath->setEnabled(!automatic);
    m_ui->downloadPath->setEnabled(!automatic && usesDownloadPath);

    m_ui->comboCategory->addItem(QString());
    for (const QString &category : std::as_const(m_categories))
        m_ui->comboCategory->addItem(category);
    if (allSame(QStringLiteral("category")))
        m_ui->comboCategory->setCurrentText(first.value(QStringLiteral("category")).toString());
    else
    {
        m_ui->comboCategory->setCurrentIndex(-1);
        m_ui->comboCategory->lineEdit()->setPlaceholderText(tr("Mixed categories"));
    }
    connect(m_ui->comboCategory, QOverload<int>::of(&QComboBox::activated), this,
        [this](int) { m_touched.insert(QStringLiteral("category")); });
    connect(m_ui->comboCategory->lineEdit(), &QLineEdit::textEdited, this,
        [this](const QString &) { m_touched.insert(QStringLiteral("category")); });

    const auto speed = [this, &first, &allSame](QSpinBox *spin, QSlider *slider,
                                                const QString &key, const QString &touch) {
        const int initial = qMax(0, first.value(key).toInt() / 1024);
        spin->setValue(initial);
        slider->setMaximum(qMax(10000, initial));
        slider->setValue(initial);
        if (!allSame(key))
        {
            spin->setMinimum(-1);
            spin->setSpecialValueText(QStringLiteral("≠"));
            spin->setValue(-1);
        }
        connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this, touch](int) { m_touched.insert(touch); });
        connect(spin, QOverload<int>::of(&QSpinBox::valueChanged),
            slider, &QSlider::setValue);
        connect(slider, &QSlider::valueChanged, spin, &QSpinBox::setValue);
    };
    speed(m_ui->spinUploadLimit, m_ui->sliderUploadLimit, QStringLiteral("up_limit"),
          QStringLiteral("up"));
    speed(m_ui->spinDownloadLimit, m_ui->sliderDownloadLimit, QStringLiteral("dl_limit"),
          QStringLiteral("down"));

    QJsonObject shared = first;
    for (const QString &key : {QStringLiteral("ratio_limit"),
        QStringLiteral("seeding_time_limit"), QStringLiteral("inactive_seeding_time_limit"),
        QStringLiteral("share_limits_mode"), QStringLiteral("share_limit_action")})
        if (!allSame(key))
            shared.remove(key);
    m_ui->torrentShareLimitsWidget->load(shared);
}

RemoteTorrentOptionsDialog::~RemoteTorrentOptionsDialog() = default;

void RemoteTorrentOptionsDialog::accept()
{
    if (!m_session || !m_session->isConnected() || !m_selectionStillValid
        || !m_selectionStillValid())
        return;
    QStringList hashes;
    for (const auto &torrent : std::as_const(m_torrents))
        hashes.append(torrent.first);

    const bool automatic = m_ui->checkAutoTMM->isChecked();
    const auto remote = [this](const QString &raw) {
        const QString translated = Remote::PathMappingStore::toRemote(raw, m_mappings);
        return translated.isEmpty() ? raw.trimmed() : translated;
    };
    const auto validPath = [](const QString &path) {
        return path.startsWith(QLatin1Char('/')) && path != QLatin1String("/")
            && !path.contains(QLatin1Char('\\')) && !path.contains(QLatin1Char('\r'))
            && !path.contains(QLatin1Char('\n'));
    };
    const QString save = remote(m_ui->savePath->currentText());
    const QString download = m_ui->checkUseDownloadPath->isChecked()
        ? remote(m_ui->downloadPath->currentText()) : QString();
    if (!automatic && ((m_touched.contains(QStringLiteral("save")) && !validPath(save))
        || ((m_touched.contains(QStringLiteral("download"))
             || m_touched.contains(QStringLiteral("downloadEnable")))
            && !download.isEmpty() && !validPath(download))))
    {
        QMessageBox::warning(this, tr("Invalid remote location"),
            tr("Enter a valid absolute directory on the remote server."));
        return;
    }
    if (m_touched.contains(QStringLiteral("auto")))
        m_session->setTorrentAutoManagement(hashes, automatic);
    if (!automatic)
    {
        if (m_touched.contains(QStringLiteral("save")))
            m_session->setTorrentSavePath(hashes, save);
        if (m_touched.contains(QStringLiteral("download"))
            || m_touched.contains(QStringLiteral("downloadEnable")))
            m_session->setTorrentDownloadPath(hashes, download);
    }
    if (m_touched.contains(QStringLiteral("category")))
    {
        const QString category = m_ui->comboCategory->currentText().trimmed();
        if (category.isEmpty() || m_categories.contains(category))
            m_session->setTorrentCategory(hashes, category);
        else
            m_session->createAndAssignTorrentCategory(hashes, category);
    }
    if (m_touched.contains(QStringLiteral("up")) && m_ui->spinUploadLimit->value() >= 0)
        m_session->setTorrentRateLimit(hashes, true, m_ui->spinUploadLimit->value() * 1024);
    if (m_touched.contains(QStringLiteral("down")) && m_ui->spinDownloadLimit->value() >= 0)
        m_session->setTorrentRateLimit(hashes, false, m_ui->spinDownloadLimit->value() * 1024);
    const QJsonObject shareChanges = m_ui->torrentShareLimitsWidget->changes();
    if (!shareChanges.isEmpty())
        for (const auto &torrent : std::as_const(m_torrents))
        {
            QJsonObject merged = torrent.second;
            for (auto it = shareChanges.constBegin(); it != shareChanges.constEnd(); ++it)
                merged.insert(it.key(), it.value());
            m_session->setTorrentShareLimits(torrent.first, merged);
        }
    for (const auto &mode : {
        qMakePair(QStringLiteral("seq"), QStringLiteral("seq_dl")),
        qMakePair(QStringLiteral("firstlast"), QStringLiteral("f_l_piece_prio"))})
    {
        if (!m_touched.contains(mode.first))
            continue;
        const bool enabled = (mode.first == QLatin1String("seq"))
            ? m_ui->checkSequential->isChecked()
            : m_ui->checkFirstLastPieces->isChecked();
        QStringList changed;
        for (const auto &torrent : std::as_const(m_torrents))
        {
            const QJsonValue existing = torrent.second.value(mode.second);
            if (existing.isBool() && existing.toBool() != enabled)
                changed.append(torrent.first);
        }
        if (!changed.isEmpty())
            m_session->toggleTorrentMode(changed, mode.first == QLatin1String("seq"));
    }
    QDialog::accept();
}
