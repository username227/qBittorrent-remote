// SPDX-License-Identifier: GPL-3.0-or-later
// Remote adaptation of upstream ExecutionLogWidget (2011 Christophe Dumez).
#include "gui/RemoteExecutionLogWidget.h"
#include "ui_remoteexecutionlogwidget.h"
#include <QListWidget>
#include <QLabel>
#include <QDateTime>
#include <QLocale>
#include <QMenu>
#include <QClipboard>
#include <QApplication>
#include <QShortcut>
#include <QScrollBar>
#include <QPalette>
#include <QColor>
#include <QJsonObject>
#include <algorithm>

RemoteExecutionLogWidget::RemoteExecutionLogWidget(Remote::RemoteSession *session, QWidget *parent)
    : QWidget(parent), m_ui(std::make_unique<Ui::RemoteExecutionLogWidget>()), m_session(session)
{
    m_ui->setupUi(this);
    setObjectName("remoteExecutionLogWidget");
    m_general = new QListWidget(m_ui->tabGeneral);
    m_general->setObjectName("remoteGeneralLog");
    m_peers = new QListWidget(m_ui->tabBan);
    m_peers->setObjectName("remotePeerLog");
    m_ui->tabGeneralLayout->addWidget(m_general);
    m_ui->tabBanLayout->addWidget(m_peers);
    configureList(m_general);
    configureList(m_peers);
    m_status = new QLabel(tr("Waiting for server log…"), this);
    m_status->setObjectName("remoteLogStatus");
    m_status->setTextFormat(Qt::PlainText);
    m_ui->verticalLayout->addWidget(m_status);
    m_timer.setInterval(1000);
    connect(&m_timer, &QTimer::timeout, this, &RemoteExecutionLogWidget::poll);
    m_timer.start();
    connect(m_ui->tabConsole, &QTabWidget::currentChanged, this, [this] { poll(); });
}
RemoteExecutionLogWidget::~RemoteExecutionLogWidget() = default;

void RemoteExecutionLogWidget::configureList(QListWidget *list)
{
    list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list->setUniformItemSizes(true);
    list->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    list->setTextElideMode(Qt::ElideNone);
    const auto copy = [list] {
        QStringList lines;
        for (auto *item : list->selectedItems()) if (!item->isHidden()) lines.append(item->text());
        if (!lines.isEmpty()) QApplication::clipboard()->setText(lines.join('\n'));
    };
    auto *shortcut = new QShortcut(QKeySequence::Copy, list);
    shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(shortcut, &QShortcut::activated, list, copy);
    list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(list, &QWidget::customContextMenuRequested, list, [list, copy](const QPoint &position) {
        QMenu menu(list);
        auto *copyAction = menu.addAction(tr("Copy"), list, copy);
        copyAction->setEnabled(!list->selectedItems().isEmpty());
        menu.addAction(tr("Clear"), list, &QListWidget::clear);
        menu.exec(list->viewport()->mapToGlobal(position));
    });
}
void RemoteExecutionLogWidget::setMessageTypes(int mask)
{
    m_types = mask;
    for (int i = 0; i < m_general->count(); ++i)
    {
        auto *item = m_general->item(i);
        item->setHidden(!(item->data(Qt::UserRole).toInt() & m_types));
    }
}
void RemoteExecutionLogWidget::poll()
{
    if (!isVisible() || !m_session || !m_session->isConnected() || m_pending) return;
    const bool peers = m_readPeersNext;
    m_readPeersNext = !m_readPeersNext;
    m_pending = true;
    const QPointer<RemoteExecutionLogWidget> self(this);
    m_session->requestLog(peers, peers ? m_peerId : m_mainId, [self, peers](const QJsonArray &entries) {
        if (!self) return;
        self->m_pending = false;
        self->append(peers, entries);
        self->m_status->clear();
    }, [self](const QString &error) {
        if (!self) return;
        self->m_pending = false;
        self->m_status->setText(self->tr("Cannot read server log: %1").arg(error));
    });
}
void RemoteExecutionLogWidget::append(bool peers, const QJsonArray &entries)
{
    auto *list = peers ? m_peers : m_general;
    int &cursor = peers ? m_peerId : m_mainId;
    QList<QJsonObject> ordered;
    for (const auto &entry : entries)
        if (entry.isObject()) ordered.append(entry.toObject());
    std::sort(ordered.begin(), ordered.end(), [](const QJsonObject &a, const QJsonObject &b) {
        return a.value("id").toInt(-1) < b.value("id").toInt(-1);
    });
    const bool atTop = list->verticalScrollBar()->value() == 0;
    for (const auto &o : ordered)
    {
        const int id = o.value("id").toInt(-1);
        if (id <= cursor) continue;
        if (!o.value("timestamp").isDouble() || (peers ? !o.value("ip").isString() : !o.value("message").isString())) continue;
        const qint64 timestamp = o.value("timestamp").toInteger();
        const QDateTime date = timestamp > 100000000000LL
            ? QDateTime::fromMSecsSinceEpoch(timestamp) : QDateTime::fromSecsSinceEpoch(timestamp);
        const QString time = QLocale().toString(date, QLocale::ShortFormat);
        const int type = peers ? 1 : o.value("type").toInt(1);
        const QString msg = peers
            ? (o.value("blocked").toBool() ? tr("%1 was blocked. Reason: %2.").arg(o.value("ip").toString(), o.value("reason").toString())
                : tr("%1 was banned").arg(o.value("ip").toString()))
            : o.value("message").toString();
        auto *item = new QListWidgetItem(time + QStringLiteral(" - ") + msg);
        item->setData(Qt::UserRole, type);
        const bool dark = list->palette().color(QPalette::Base).lightness() < 128;
        if (peers || type == 8) item->setForeground(QColor(dark ? "#ff8585" : "#b00020"));
        else if (type == 4) item->setForeground(QColor(dark ? "#f0c36c" : "#9a5800"));
        else if (type == 2) item->setForeground(QColor(dark ? "#8dbdff" : "#175aa0"));
        list->insertItem(0, item);
        if (!peers) item->setHidden(!(type & m_types));
        cursor = id;
        while (list->count() > 20000) delete list->takeItem(list->count()-1);
    }
    if (atTop) list->scrollToTop();
}
