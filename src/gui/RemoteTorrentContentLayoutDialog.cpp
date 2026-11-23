// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/RemoteTorrentContentLayoutDialog.h"

#include "api/RemoteSession.h"
#include "ui_remotecontentlayoutdialog.h"

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QJsonObject>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QTimer>

#include <algorithm>
#include <utility>

RemoteTorrentContentLayoutDialog::RemoteTorrentContentLayoutDialog(
    Remote::RemoteSession *session, QString hash,
    std::function<bool()> selectionStillValid, QWidget *parent)
    : QDialog(parent)
    , m_ui(std::make_unique<Ui::TorrentContentLayoutDialog>())
    , m_session(session)
    , m_hash(std::move(hash))
    , m_selectionStillValid(std::move(selectionStillValid))
    , m_model(new QStandardItemModel(this))
{
    m_ui->setupUi(this); // Unchanged upstream Qt Designer layout.
    setObjectName(QStringLiteral("remoteTorrentContentLayoutDialog"));
    m_ui->pathListView->setModel(m_model);
    m_ui->pathListView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_ui->pathListView->setEditTriggers(QAbstractItemView::AllEditTriggers);
    m_ui->pathListView->header()->setStretchLastSection(true);
    m_ui->buttonBox->button(QDialogButtonBox::Apply)->setEnabled(false);
    m_ui->commonPathEdit->setEnabled(false);

    connect(m_ui->buttonBox, &QDialogButtonBox::clicked, this,
        [this](QAbstractButton *button) {
            if (button == m_ui->buttonBox->button(QDialogButtonBox::Apply))
                apply();
            else
                reject();
        });
    connect(m_ui->pathListView->selectionModel(),
        &QItemSelectionModel::selectionChanged, this,
        [this]() { updateCommonPath(); });
    connect(m_ui->commonPathEdit, &QLineEdit::textEdited, this,
        &RemoteTorrentContentLayoutDialog::editCommonPath);
    connect(m_model, &QStandardItemModel::itemChanged, this,
        [this](QStandardItem *) { updateApply(); });
    if (m_session)
    {
        connect(m_session, &Remote::RemoteSession::torrentDetailsReceived, this,
            [this](const QString &hash, int tab, const QJsonValue &value) {
                if (tab == 4)
                    received(hash, tab, value.toArray());
            });
        connect(m_session, &Remote::RemoteSession::torrentDetailsFailed, this,
            [this](const QString &hash, int tab, const QString &reason) {
                if (tab != 4 || hash != m_hash || !m_loading)
                    return;
                m_loading = false;
                QMessageBox::warning(this, tr("Manage Torrent Content"),
                    tr("Could not retrieve the remote torrent's files: %1").arg(reason));
            });
        connect(m_session, &Remote::RemoteSession::torrentContentRenameFinished,
            this, &RemoteTorrentContentLayoutDialog::fileRenameFinished);
    }
    QTimer::singleShot(0, this, &RemoteTorrentContentLayoutDialog::reload);
}

RemoteTorrentContentLayoutDialog::~RemoteTorrentContentLayoutDialog() = default;

bool RemoteTorrentContentLayoutDialog::selectionValid() const
{
    return m_session && m_session->isConnected()
        && m_selectionStillValid && m_selectionStillValid();
}

bool RemoteTorrentContentLayoutDialog::validTorrentRelativePath(const QString &path)
{
    if (path.isEmpty() || path.startsWith(QLatin1Char('/'))
        || path.contains(QLatin1Char('\\'))
        || path.contains(QLatin1Char('\r')) || path.contains(QLatin1Char('\n'))
        || path.contains(QChar(0)))
        return false;
    const QStringList parts = path.split(QLatin1Char('/'));
    return std::all_of(parts.cbegin(), parts.cend(),
        [](const QString &part) {
            return !part.isEmpty() && part != QLatin1String(".")
                && part != QLatin1String("..");
        });
}

void RemoteTorrentContentLayoutDialog::reload()
{
    if (!selectionValid())
    {
        reject();
        return;
    }
    m_loading = true;
    m_ui->buttonBox->button(QDialogButtonBox::Apply)->setEnabled(false);
    m_session->requestTorrentDetails(m_hash, 4);
}

void RemoteTorrentContentLayoutDialog::received(
    const QString &hash, int tab, const QJsonArray &files)
{
    if (tab != 4 || hash != m_hash || !m_loading)
        return;
    m_loading = false;
    const QSignalBlocker block(m_model);
    m_model->clear();
    m_model->setHorizontalHeaderLabels({tr("Index"), tr("Path")});
    m_original.clear();
    for (const QJsonValue &value : files)
    {
        const QJsonObject row = value.toObject();
        const int index = row.value(QStringLiteral("index")).toInt(-1);
        const QString path = row.value(QStringLiteral("name")).toString();
        if (index < 0 || path.isEmpty())
            continue;
        auto *number = new QStandardItem(QString::number(index));
        number->setEditable(false);
        auto *name = new QStandardItem(path);
        name->setEditable(true);
        m_model->appendRow({number, name});
        m_original.insert(index, path);
    }
    m_ui->pathListView->resizeColumnToContents(0);
    if (m_model->rowCount() > 0)
        m_ui->pathListView->selectionModel()->setCurrentIndex(
            m_model->index(0, 1),
            QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    updateCommonPath();
    updateApply();
}

void RemoteTorrentContentLayoutDialog::updateApply()
{
    if (m_loading || m_pending)
        return;
    bool modified = false;
    for (int row = 0; row < m_model->rowCount(); ++row)
    {
        const int index = m_model->item(row, 0)->text().toInt();
        const QString path = m_model->item(row, 1)->text();
        QStandardItem *item = m_model->item(row, 1);
        const bool changed = path != m_original.value(index);
        item->setFont([item, changed]() {
            QFont font = item->font();
            font.setItalic(changed);
            return font;
        }());
        modified |= changed;
    }
    m_ui->buttonBox->button(QDialogButtonBox::Apply)->setEnabled(modified);
}

void RemoteTorrentContentLayoutDialog::updateCommonPath()
{
    const QModelIndexList selected =
        m_ui->pathListView->selectionModel()->selectedRows(1);
    if (selected.isEmpty())
    {
        m_commonPrefix.clear();
        m_ui->commonPathEdit->setEnabled(false);
        m_ui->commonPathEdit->clear();
        return;
    }

    QStringList prefix = selected.first().data(Qt::EditRole)
        .toString().split(QLatin1Char('/'));
    for (const QModelIndex &index : selected)
    {
        const QStringList parts = index.data(Qt::EditRole)
            .toString().split(QLatin1Char('/'));
        int shared = 0;
        while (shared < prefix.size() && shared < parts.size()
               && prefix.at(shared) == parts.at(shared))
            ++shared;
        prefix = prefix.mid(0, shared);
    }
    m_commonPrefix = prefix.join(QLatin1Char('/'));
    QSignalBlocker blocker(m_ui->commonPathEdit);
    m_ui->commonPathEdit->setText(m_commonPrefix);
    m_ui->commonPathEdit->setEnabled(true);
}

void RemoteTorrentContentLayoutDialog::editCommonPath(const QString &newPrefix)
{
    const QModelIndexList selected =
        m_ui->pathListView->selectionModel()->selectedRows(1);
    if (selected.isEmpty())
        return;
    const QString oldPrefix = m_commonPrefix;
    for (const QModelIndex &index : selected)
    {
        const QString before = index.data(Qt::EditRole).toString();
        if (!before.startsWith(oldPrefix))
            continue;
        const QString suffix = before.mid(oldPrefix.size());
        const QString after = newPrefix + suffix;
        m_model->setData(index, after);
    }
    m_commonPrefix = newPrefix;
}

void RemoteTorrentContentLayoutDialog::apply()
{
    if (!selectionValid() || m_loading || m_pending)
        return;
    QList<QPair<QString, QString>> changes;
    for (int row = 0; row < m_model->rowCount(); ++row)
    {
        const int index = m_model->item(row, 0)->text().toInt();
        const QString before = m_original.value(index);
        const QString after = m_model->item(row, 1)->text();
        if (before == after)
            continue;
        if (!validTorrentRelativePath(after))
        {
            QMessageBox::warning(this, tr("Rename error"),
                tr("Invalid torrent-relative file path: %1").arg(after));
            return;
        }
        changes.append({before, after});
    }
    if (changes.isEmpty())
        return;
    m_pending = changes.size();
    m_ui->buttonBox->button(QDialogButtonBox::Apply)->setEnabled(false);
    for (const auto &change : std::as_const(changes))
        m_session->renameTorrentContent(m_hash, change.first, change.second);
}

void RemoteTorrentContentLayoutDialog::fileRenameFinished(
    const QString &hash, const QString &, bool success, const QString &reason)
{
    if (hash != m_hash || !m_pending)
        return;
    if (!success)
        QMessageBox::warning(this, tr("Rename error"), reason);
    if (--m_pending == 0)
        reload();
}
