// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from qBittorrent's torrentcategorydialog.cpp (Vladimir Golovnev, 2017-2026).
#include "gui/RemoteCategoryDialog.h"
#include "gui/RemoteFileSystemPathComboEdit.h"
#include "gui/RemoteTorrentShareLimitsWidget.h"
#include "ui_remotetorrentcategorydialog.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QHash>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>

RemoteCategoryDialog::RemoteCategoryDialog(const QString &name, bool edit,
    const QJsonObject &definition, const QJsonObject &capabilities, const QSet<QString> &existing,
    const QList<Remote::PathMapping> &mappings, QWidget *parent)
    : QDialog(parent), m_ui(std::make_unique<Ui::TorrentCategoryDialog>()),
      m_edit(edit), m_original(definition), m_existing(existing), m_mappings(mappings)
{
    m_ui->setupUi(this);
    setObjectName(QStringLiteral("remoteCategoryDialog"));
    m_ui->textCategoryName->setText(name);
    m_ui->textCategoryName->setEnabled(!edit);
    const qsizetype start = name.lastIndexOf(QLatin1Char('/')) + 1;
    m_ui->textCategoryName->setSelection(start, name.size() - start);
    for (auto *field : {m_ui->comboSavePath, m_ui->comboDownloadPath})
    {
        field->setInsertPolicy(QComboBox::NoInsert);
        connect(field, &RemoteFileSystemPathComboEdit::browseRequested, this, [this, field]() { browse(field); });
        field->lineEdit()->setToolTip(tr("Path on the remote server. Empty uses the inherited default; relative paths are resolved by qBittorrent."));
    }
    m_ui->comboSavePath->setEditText(definition.value(QStringLiteral("savePath")).toString());
    m_ui->comboSavePath->lineEdit()->setPlaceholderText(tr("Default"));
    const auto download = definition.value(QStringLiteral("download_path"));
    const bool downloadSupported = capabilities.contains(QStringLiteral("download_path"));
    m_ui->groupBox->setEnabled(downloadSupported);
    if (!downloadSupported)
        m_ui->groupBox->setToolTip(tr("The connected server has not advertised category-specific incomplete paths."));
    m_ui->comboUseDownloadPath->setCurrentIndex(download.isString() || (download.isBool() && download.toBool())
        ? 1 : (download.isBool() ? 2 : 0));
    m_lastDownloadPath = download.toString();
    m_ui->comboDownloadPath->setEditText(m_lastDownloadPath);
    const auto updateDownload = [this](int index) {
        if (!m_ui->comboDownloadPath->currentText().isEmpty())
            m_lastDownloadPath = m_ui->comboDownloadPath->currentText();
        m_ui->comboDownloadPath->setEnabled(index == 1);
        m_ui->labelDownloadPath->setEnabled(index == 1);
        m_ui->comboDownloadPath->setEditText(index == 1 ? m_lastDownloadPath : QString());
    };
    connect(m_ui->comboUseDownloadPath, &QComboBox::currentIndexChanged, this, updateDownload);
    updateDownload(m_ui->comboUseDownloadPath->currentIndex());
    const QJsonObject defaults{
        {QStringLiteral("ratio_limit"), -2}, {QStringLiteral("seeding_time_limit"), -2},
        {QStringLiteral("inactive_seeding_time_limit"), -2},
        {QStringLiteral("share_limits_mode"), QStringLiteral("Default")},
        {QStringLiteral("share_limit_action"), QStringLiteral("Default")}
    };
    bool sharesSupported = true;
    for (auto it = defaults.constBegin(); it != defaults.constEnd(); ++it)
        sharesSupported &= capabilities.contains(it.key());
    m_ui->torrentShareLimitsBox->setEnabled(sharesSupported);
    if (!sharesSupported)
        m_ui->torrentShareLimitsBox->setToolTip(tr("The connected server has not advertised category share limits."));
    m_ui->torrentShareLimitsWidget->load(edit ? definition : defaults);
    auto *ok = m_ui->buttonBox->button(QDialogButtonBox::Ok);
    ok->setEnabled(!name.isEmpty());
    connect(m_ui->textCategoryName, &QLineEdit::textChanged, this,
            [ok](const QString &value) { ok->setEnabled(!value.isEmpty()); });
    connect(m_ui->buttonBox, &QDialogButtonBox::accepted, this, &RemoteCategoryDialog::accept);
    connect(m_ui->buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

RemoteCategoryDialog::~RemoteCategoryDialog() = default;
QString RemoteCategoryDialog::categoryName() const { return m_ui->textCategoryName->text(); }

QJsonObject RemoteCategoryDialog::fields() const
{
    QJsonObject result;
    const QString save = m_ui->comboSavePath->currentText();
    if (!m_edit || save != m_original.value(QStringLiteral("savePath")).toString())
        result.insert(QStringLiteral("savePath"), save);
    if (m_ui->groupBox->isEnabled())
    {
        const int mode = m_ui->comboUseDownloadPath->currentIndex();
        const QJsonValue desired = mode == 0 ? QJsonValue(QJsonValue::Null)
            : mode == 1 ? QJsonValue(m_ui->comboDownloadPath->currentText()) : QJsonValue(false);
        if (desired != m_original.value(QStringLiteral("download_path"))
            && (m_edit || mode != 0))
        {
            result.insert(QStringLiteral("downloadPathEnabled"),
                mode == 0 ? QStringLiteral("") : mode == 1 ? QStringLiteral("true") : QStringLiteral("false"));
            if (mode == 1)
                result.insert(QStringLiteral("downloadPath"), desired.toString());
        }
    }
    if (m_ui->torrentShareLimitsBox->isEnabled())
    {
        const auto changes = m_ui->torrentShareLimitsWidget->changes();
        const QHash<QString, QString> names{
            {QStringLiteral("ratio_limit"), QStringLiteral("ratioLimit")},
            {QStringLiteral("seeding_time_limit"), QStringLiteral("seedingTimeLimit")},
            {QStringLiteral("inactive_seeding_time_limit"), QStringLiteral("inactiveSeedingTimeLimit")},
            {QStringLiteral("share_limits_mode"), QStringLiteral("shareLimitsMode")},
            {QStringLiteral("share_limit_action"), QStringLiteral("shareLimitAction")}
        };
        for (auto it = changes.constBegin(); it != changes.constEnd(); ++it)
            if (!m_edit || it.value() != m_original.value(it.key()))
                result.insert(names.value(it.key()), it.value());
    }
    return result;
}

void RemoteCategoryDialog::accept()
{
    const QString name = categoryName();
    if (name.trimmed().isEmpty() || name.contains(QLatin1Char('\\'))
        || name.startsWith(QLatin1Char('/')) || name.endsWith(QLatin1Char('/'))
        || name.contains(QStringLiteral("//")) || name.contains(QLatin1Char('\r'))
        || name.contains(QLatin1Char('\n')) || name.contains(QChar::Null))
    {
        QMessageBox::critical(this, tr("Invalid category name"),
            tr("Category name cannot contain '\\', start/end with '/', or contain '//'."));
        return;
    }
    if (!m_edit && m_existing.contains(name))
    {
        QMessageBox::critical(this, tr("Category creation error"),
            tr("Category with the given name already exists. Please choose a different name."));
        return;
    }
    const auto values = fields();
    for (const auto &key : {QStringLiteral("savePath"), QStringLiteral("downloadPath")})
    {
        if (!values.contains(key)) continue;
        const QString path = values.value(key).toString();
        if (path.contains(QLatin1Char('\\')) || path.contains(QLatin1Char('\r'))
            || path.contains(QLatin1Char('\n')) || path.contains(QChar::Null)
            || (path.size() >= 2 && path.at(0).isLetter() && path.at(1) == QLatin1Char(':')))
        {
            QMessageBox::warning(this, tr("Invalid server path"), tr("Enter a remote POSIX path, or browse a mapped directory."));
            return;
        }
    }
    if (values.value(QStringLiteral("shareLimitAction")).toString() == QLatin1String("RemoveWithContent")
        && QMessageBox::warning(this, tr("Remove torrent content"),
            tr("The server may automatically delete downloaded files when these share limits are reached. Continue?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    QDialog::accept();
}

void RemoteCategoryDialog::browse(RemoteFileSystemPathComboEdit *field)
{
    if (m_mappings.isEmpty())
    {
        QMessageBox::information(this, tr("No path mappings"), tr("Configure a mapped NAS folder or type a server path."));
        return;
    }
    QString initial = Remote::PathMappingStore::toLocal(field->currentText(), m_mappings);
    if (initial.isEmpty() || !QDir(initial).exists())
        initial = m_mappings.first().localRoot;
    const auto local = QFileDialog::getExistingDirectory(this, tr("Choose mapped local NAS directory"), initial);
    if (local.isEmpty()) return;
    const auto remote = Remote::PathMappingStore::toRemote(local, m_mappings);
    if (remote.isEmpty())
        QMessageBox::warning(this, tr("Unmapped directory"), tr("The selected folder is not mapped to this remote server."));
    else
        field->setEditText(remote);
}
