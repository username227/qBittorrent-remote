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
#include <QScopedValueRollback>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QTimer>

#include <algorithm>
#include <utility>

RemoteTorrentContentLayoutDialog::RemoteTorrentContentLayoutDialog(
    Remote::RemoteSession *session, QString hash,
    std::function<bool()> selectionStillValid, QWidget *parent, QSet<int> selectedIds)
    : QDialog(parent)
    , m_ui(std::make_unique<Ui::TorrentContentLayoutDialog>())
    , m_session(session)
    , m_hash(std::move(hash))
    , m_selectionStillValid(std::move(selectionStillValid))
    , m_model(new QStandardItemModel(this))
    , m_selectedIds(std::move(selectedIds))
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
        [this](QStandardItem *) {
            if (m_loading || m_pending) return;
            updateCommonPath();
            updateApply();
        });
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
        const auto lost = [this]() {
            m_pending = 0;
            m_pendingPaths.clear();
            reject();
        };
        connect(m_session, &Remote::RemoteSession::disconnected, this, lost);
        connect(m_session, &Remote::RemoteSession::failed, this, lost);
        connect(m_session, &QObject::destroyed, this, lost);
    }
    QTimer::singleShot(0, this, &RemoteTorrentContentLayoutDialog::reload);
}

RemoteTorrentContentLayoutDialog::~RemoteTorrentContentLayoutDialog() = default;

void RemoteTorrentContentLayoutDialog::reject()
{
    // All requests were explicitly authorized by Apply. Keep the dialog
    // modal until their results arrive, unless the session has disconnected.
    if (m_pending && m_session && m_session->isConnected()) return;
    QDialog::reject();
}

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
    m_ui->pathListView->setEnabled(false);
    m_ui->commonPathEdit->setEnabled(false);
    m_ui->buttonBox->button(QDialogButtonBox::Apply)->setEnabled(false);
    m_session->requestTorrentDetails(m_hash, 4);
}

void RemoteTorrentContentLayoutDialog::received(
    const QString &hash, int tab, const QJsonArray &files)
{
    if (tab != 4 || hash != m_hash || !m_loading)
        return;
    // The view MUST receive begin/end reset and row insertion signals.
    // Suppressing model signals here leaves QTreeView holding invalid
    // QModelIndexes after clear(), which crashes during paint.
    // m_loading keeps itemChanged from treating population as user edits.
    if (!m_initialSelection)
    {
        m_selectedIds.clear();
        for (const QModelIndex &selected : m_ui->pathListView->selectionModel()->selectedRows(0))
            m_selectedIds.insert(selected.data().toInt());
    }
    const bool preselect = !m_selectedIds.isEmpty() || !m_initialSelection;
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
    auto *selection = m_ui->pathListView->selectionModel();
    if (preselect)
    {
        for (int row = 0; row < m_model->rowCount(); ++row)
            if (m_selectedIds.contains(m_model->item(row, 0)->text().toInt()))
                selection->select(m_model->index(row, 0),
                    QItemSelectionModel::Select | QItemSelectionModel::Rows);
    }
    else if (m_model->rowCount() > 0)
        selection->setCurrentIndex(m_model->index(0, 1),
            QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    m_initialSelection = false;
    m_ui->pathListView->setEnabled(true);
    m_loading = false;
    updateCommonPath();
    updateApply();
}

void RemoteTorrentContentLayoutDialog::updateApply()
{
    if (m_loading || m_pending)
        return;
    // Allow Qt's dataChanged notifications to reach the view, while
    // preventing itemChanged from recursively recalculating appearance.
    const QScopedValueRollback<bool> renderingGuard(m_loading, true);
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
    m_commonSuffixes.clear();
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
    for (const QModelIndex &index : selected)
    {
        QString suffix = index.data(Qt::EditRole).toString().mid(m_commonPrefix.size());
        if (!m_commonPrefix.isEmpty() && suffix.startsWith(QLatin1Char('/'))) suffix.remove(0, 1);
        m_commonSuffixes.insert(m_model->item(index.row(), 0)->text().toInt(), suffix);
    }
    QSignalBlocker blocker(m_ui->commonPathEdit);
    m_ui->commonPathEdit->setText(m_commonPrefix);
    m_ui->commonPathEdit->setEnabled(!m_loading && !m_pending);
}

void RemoteTorrentContentLayoutDialog::editCommonPath(const QString &newPrefix)
{
    const QModelIndexList selected =
        m_ui->pathListView->selectionModel()->selectedRows(1);
    if (selected.isEmpty() || m_loading || m_pending)
        return;
    {
        const QScopedValueRollback<bool> guard(m_loading, true);
        for (const QModelIndex &index : selected)
        {
            const QString suffix = m_commonSuffixes.value(m_model->item(index.row(), 0)->text().toInt());
            const QString separator = newPrefix.isEmpty() || suffix.isEmpty()
                || newPrefix.endsWith(QLatin1Char('/')) ? QString() : QStringLiteral("/");
            m_model->setData(index, newPrefix + separator + suffix);
        }
    }
    m_commonPrefix = newPrefix;
    updateApply();
}

void RemoteTorrentContentLayoutDialog::apply()
{
    if (!selectionValid() || m_loading || m_pending)
        return;
    // The Web API has no atomic batch rename. Reject conflicting targets
    // before any request rather than risking a partial destructive sequence.
    // A destination occupied by a different original file is not safe even
    // if that file is also scheduled to move (swaps need a temporary name).
    QList<QPair<QString, QString>> changes;
    QSet<QString> originals, destinations;
    for (const QString &path : std::as_const(m_original)) originals.insert(path);
    QStringList originalPaths = originals.values();
    std::sort(originalPaths.begin(), originalPaths.end());
    const auto conflicts = [](const QString &path, const QSet<QString> &occupied,
                               const QStringList &sorted) {
        if (occupied.contains(path)) return true;
        for (int slash = path.indexOf(QLatin1Char('/')); slash >= 0;
             slash = path.indexOf(QLatin1Char('/'), slash + 1))
            if (occupied.contains(path.left(slash))) return true;
        const QString prefix = path + QLatin1Char('/');
        const auto it = std::lower_bound(sorted.cbegin(), sorted.cend(), prefix);
        return it != sorted.cend() && it->startsWith(prefix);
    };
    for (int row = 0; row < m_model->rowCount(); ++row)
    {
        const int index = m_model->item(row, 0)->text().toInt();
        const QString before = m_original.value(index);
        const QString after = m_model->item(row, 1)->text();
        if (destinations.contains(after))
        {
            QMessageBox::warning(this, tr("Rename error"), tr("More than one file would use the same path."));
            return;
        }
        destinations.insert(after);
        if (before == after)
            continue;
        if (!validTorrentRelativePath(after))
        {
            QMessageBox::warning(this, tr("Rename error"),
                tr("Invalid torrent-relative file path: %1").arg(after));
            return;
        }
        if (conflicts(after, originals, originalPaths))
        {
            QMessageBox::warning(this, tr("Rename error"),
                tr("Destination conflicts with an existing torrent file: %1").arg(after));
            return;
        }
        changes.append({before, after});
    }
    if (changes.isEmpty())
        return;
    for (const QString &path : std::as_const(destinations))
        for (int slash = path.indexOf(QLatin1Char('/')); slash >= 0;
             slash = path.indexOf(QLatin1Char('/'), slash + 1))
            if (destinations.contains(path.left(slash)))
            {
                QMessageBox::warning(this, tr("Rename error"), tr("A file would also be used as a directory."));
                return;
            }
    m_pendingPaths.clear();
    for (const auto &change : std::as_const(changes)) m_pendingPaths.insert(change.first);
    m_pending = changes.size();
    m_ui->pathListView->setEnabled(false);
    m_ui->commonPathEdit->setEnabled(false);
    m_ui->buttonBox->button(QDialogButtonBox::Close)->setEnabled(false);
    m_ui->buttonBox->button(QDialogButtonBox::Apply)->setEnabled(false);
    for (const auto &change : std::as_const(changes))
        m_session->renameTorrentContent(m_hash, change.first, change.second);
}

void RemoteTorrentContentLayoutDialog::fileRenameFinished(
    const QString &hash, const QString &oldPath, bool success, const QString &reason)
{
    if (hash != m_hash || !m_pending || !m_pendingPaths.remove(oldPath))
        return;
    if (!success)
        QMessageBox::warning(this, tr("Rename error"), reason);
    if (--m_pending == 0)
    {
        m_ui->buttonBox->button(QDialogButtonBox::Close)->setEnabled(true);
        reload();
    }
}
