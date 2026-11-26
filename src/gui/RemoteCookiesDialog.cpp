// SPDX-License-Identifier: GPL-3.0-or-later
// Native form adapted from qBittorrent, Copyright 2016 Vladimir Golovnev.
#include "gui/RemoteCookiesDialog.h"
#include "ui_remotecookiesdialog.h"
#include <QStandardItemModel>
#include <QDateTime>
#include <QSettings>
#include <QLabel>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QToolButton>
#include <QIcon>
#include <algorithm>
#include <QSet>

RemoteCookiesDialog::RemoteCookiesDialog(Remote::RemoteSession *session, const QString &name,
    QSettings &settings, QWidget *parent)
    : QDialog(parent), m_ui(std::make_unique<Ui::RemoteCookiesDialog>()),
      m_session(session), m_settings(settings)
{
    m_ui->setupUi(this);
    setObjectName("remoteCookiesDialog");
    setWindowTitle(tr("Manage Cookies — %1").arg(name));
    m_model = new QStandardItemModel(0, 5, this);
    m_model->setHorizontalHeaderLabels({tr("Domain"), tr("Path"), tr("Name"), tr("Value"), tr("Expiration Date")});
    m_ui->treeView->setModel(m_model);
    m_ui->treeView->setRootIsDecorated(false);
    m_ui->treeView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_ui->treeView->header()->setSectionsMovable(true);
    m_ui->treeView->header()->restoreState(settings.value("Layout/Dialogs/CookiesHeader").toByteArray());
    m_status = new QLabel(tr("Loading server download cookies…"), this);
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    m_ui->verticalLayout_2->insertWidget(1, m_status);
    m_ui->buttonAdd->setText("+");
    m_ui->buttonDelete->setText("−");
    m_ui->buttonAdd->setIcon(QIcon::fromTheme("list-add"));
    m_ui->buttonDelete->setIcon(QIcon::fromTheme("list-remove"));
    m_ui->buttonAdd->setToolTip(tr("Add cookie"));
    m_ui->buttonDelete->setToolTip(tr("Remove selected cookies"));
    connect(m_ui->buttonBox, &QDialogButtonBox::accepted, this, &RemoteCookiesDialog::accept);
    connect(m_ui->buttonBox, &QDialogButtonBox::rejected, this, &RemoteCookiesDialog::reject);
    connect(m_ui->buttonAdd, &QToolButton::clicked, this, [this] {
        QList<QStandardItem *> row;
        for (int i = 0; i < 5; ++i) row.append(new QStandardItem);
        row[1]->setText("/");
        row[4]->setData(QDateTime::currentDateTime().addYears(2), Qt::EditRole);
        m_model->appendRow(row);
        m_ui->treeView->setCurrentIndex(m_model->index(m_model->rowCount()-1, 0));
        m_ui->treeView->edit(m_ui->treeView->currentIndex());
    });
    connect(m_ui->buttonDelete, &QToolButton::clicked, this, [this] {
        auto rows = m_ui->treeView->selectionModel()->selectedRows();
        std::sort(rows.begin(), rows.end(), [](const QModelIndex &a, const QModelIndex &b) { return a.row() > b.row(); });
        for (const auto &row : rows) m_model->removeRow(row.row());
    });
    setBusy(true);
    const QPointer<RemoteCookiesDialog> self(this);
    if (!session) { fail(tr("Server disconnected.")); return; }
    connect(session, &Remote::RemoteSession::disconnected, this, [this] {
        m_loaded = false; fail(tr("Server disconnected. No further changes can be saved."));
    });
    connect(session, &Remote::RemoteSession::failed, this, [this](const QString &) {
        m_loaded = false; fail(tr("Server disconnected. No further changes can be saved."));
    });
    session->requestCookies([self](const QJsonArray &cookies) {
        if (!self) return;
        for (const auto &v : cookies)
        {
            const auto o = v.toObject();
            if (!v.isObject() || !o.value("domain").isString() || !o.value("path").isString()
                || !o.value("name").isString() || !o.value("value").isString() || !o.value("expirationDate").isDouble())
            { self->fail(tr("Unsupported cookie response. Editing disabled.")); return; }
        }
        self->m_original = cookies;
        self->populate(cookies);
        self->m_loaded = true;
        self->m_status->setText(tr("Cookies used by the server's download manager, not Web API login cookies."));
        self->setBusy(false);
    }, [self](const QString &error) { if (self) self->fail(error); });
}
RemoteCookiesDialog::~RemoteCookiesDialog()
{
    m_settings.setValue("Layout/Dialogs/CookiesHeader", m_ui->treeView->header()->saveState());
}
void RemoteCookiesDialog::populate(const QJsonArray &cookies)
{
    for (const auto &v : cookies)
    {
        const auto o = v.toObject();
        QList<QStandardItem *> row;
        for (const auto key : {"domain", "path", "name", "value"})
            row.append(new QStandardItem(o.value(key).toString()));
        auto *expiry = new QStandardItem;
        const qint64 sec = o.value("expirationDate").toInteger(-1);
        expiry->setData(sec == -1 ? QDateTime() : QDateTime::fromSecsSinceEpoch(sec), Qt::EditRole);
        row.append(expiry);
        m_model->appendRow(row);
    }
}
void RemoteCookiesDialog::setBusy(bool busy)
{
    m_busy = busy;
    const bool editable = !busy && m_loaded;
    m_ui->treeView->setEnabled(editable);
    m_ui->buttonAdd->setEnabled(editable);
    m_ui->buttonDelete->setEnabled(editable);
    m_ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(editable);
    m_ui->buttonBox->button(QDialogButtonBox::Cancel)->setEnabled(!busy);
}
void RemoteCookiesDialog::fail(const QString &message)
{
    m_status->setText(message);
    setBusy(false);
}
void RemoteCookiesDialog::reject()
{
    if (!m_busy) QDialog::reject();
}
void RemoteCookiesDialog::accept()
{
    if (!m_loaded || m_busy || !m_session || !m_session->isConnected()) return;
    QJsonArray changed;
    QSet<QString> identities;
    for (int row = 0; row < m_model->rowCount(); ++row)
    {
        const auto value = [&](int col) { return m_model->index(row, col).data(Qt::EditRole).toString(); };
        const QString domain = value(0).trimmed(), path = value(1), name = value(2), contents = value(3);
        const QString key = domain.toLower() + QChar::Null + path + QChar::Null + name;
        if (domain.isEmpty() || name.isEmpty() || identities.contains(key)
            || domain.contains(QChar::Null) || path.contains(QChar::Null) || name.contains(QChar::Null)
            || contents.contains(QChar::Null) || contents.contains('\n') || contents.contains('\r'))
        { fail(tr("Every cookie needs a domain and name, with no duplicate domain/path/name or invalid characters.")); return; }
        identities.insert(key);
        const auto expiry = m_model->index(row, 4).data(Qt::EditRole).toDateTime();
        changed.append(QJsonObject{{"domain", domain}, {"path", path}, {"name", name},
            {"value", contents}, {"expirationDate", expiry.isValid() ? expiry.toSecsSinceEpoch() : qint64(-1)}});
    }
    if (changed == m_original) { QDialog::accept(); return; }
    setBusy(true);
    m_status->setText(tr("Checking for changes made elsewhere…"));
    const QPointer<RemoteCookiesDialog> self(this);
    m_session->requestCookies([self, changed](const QJsonArray &current) {
        if (!self || !self->m_loaded || !self->m_session) return;
        if (current != self->m_original)
        { self->m_loaded = false; self->fail(tr("Cookies changed on the server. Reopen this dialog before editing.")); return; }
        self->m_status->setText(tr("Saving cookies…"));
        self->m_session->setCookies(changed, [self] {
            if (self) { self->m_busy = false; self->QDialog::accept(); }
        }, [self](const QString &error) { if (self) self->fail(error); });
    }, [self](const QString &error) { if (self) self->fail(error); });
}
