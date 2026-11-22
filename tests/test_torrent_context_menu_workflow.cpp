// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the *actual* torrent-list Qt context menu with simulated sync data.
// Never contact a live server or make filesystem modifications.
#include "gui/ServerPane.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QResource>
#include <QHeaderView>
#include <QHash>
#include <QIcon>
#include <QImage>
#include <QJsonDocument>
#include <QMenu>
#include <QSignalSpy>
#include <QTableView>
#include <QTimer>
#include <QtTest/QtTest>

namespace
{
QJsonObject payload(const QByteArray &text)
{
    return QJsonDocument::fromJson(text).object();
}
QModelIndex indexForName(QTableView *table, const QString &name)
{
    for (int row = 0; row < table->model()->rowCount(); ++row)
    {
        const QModelIndex index = table->model()->index(row, 0);
        if (index.data(Qt::DisplayRole).toString() == name)
            return index;
    }
    return {};
}
QAction *actionNamed(QMenu *menu, const QString &name)
{
    for (QAction *action : menu->actions())
        if (action->objectName() == name)
            return action;
    return nullptr;
}
QMenu *copySubmenu(QMenu *menu)
{
    for (QAction *action : menu->actions())
        if (action->menu()
            && action->menu()->objectName() == QLatin1String("torrentContextCopyMenu"))
            return action->menu();
    return nullptr;
}
} // namespace

class TorrentContextMenuWorkflowTest final : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        Q_INIT_RESOURCE(qbittorrent_icons);
    }
    void contextMenuUsesNativeQbittorrentIcons();
    void rightClickUnselectedRowTargetsOnlyThatTorrent();
    void rightClickSelectedRowPreservesMultipleSelectionAndCopiesNames();
    void mixedSelectionOffersBothStartAndStop();
    void copySubmenuCopiesRealRemoteFieldsAndDisablesUnavailableMetadata();
};

void TorrentContextMenuWorkflowTest::contextMenuUsesNativeQbittorrentIcons()
{
    // Validate the real resource paths in the linked Qt static GUI target,
    // not just the presence of icon file names in source.
    const QStringList nativeIcons = {
        QStringLiteral("torrent-start"), QStringLiteral("torrent-stop"),
        QStringLiteral("list-remove"), QStringLiteral("edit-copy"),
        QStringLiteral("name"), QStringLiteral("hash"),
        QStringLiteral("torrent-magnet"), QStringLiteral("help-about"),
        QStringLiteral("directory")
    };
    for (const QString &name : nativeIcons)
    {
        const QString resource = QStringLiteral(":/icons/") + name + QStringLiteral(".svg");
        QVERIFY2(QFile::exists(resource), qPrintable(resource));
    }

    // Check each QAction against upstream qBittorrent's precise icon
    // mapping, not merely that some valid SVG happens to be assigned.
    const QHash<QString, QString> expectedIcon = {
        {QStringLiteral("torrentContextStart"), QStringLiteral("torrent-start")},
        {QStringLiteral("torrentContextStop"), QStringLiteral("torrent-stop")},
        {QStringLiteral("torrentContextRemove"), QStringLiteral("list-remove")},
        {QStringLiteral("torrentContextCopyMenu"), QStringLiteral("edit-copy")},
        {QStringLiteral("torrentCopyName"), QStringLiteral("name")},
        {QStringLiteral("torrentCopyHashV1"), QStringLiteral("hash")},
        {QStringLiteral("torrentCopyHashV2"), QStringLiteral("hash")},
        {QStringLiteral("torrentCopyMagnet"), QStringLiteral("torrent-magnet")},
        {QStringLiteral("torrentCopyId"), QStringLiteral("help-about")},
        {QStringLiteral("torrentCopyComment"), QStringLiteral("edit-copy")},
        {QStringLiteral("torrentCopyContentPath"), QStringLiteral("directory")}
    };

    ServerPane pane;
    pane.resize(1200, 740);
    pane.applySync(payload(R"JSON({
      "full_update":true,
      "torrents":{
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa":{
          "name":"Stopped album","state":"stoppedUP",
          "infohash_v1":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        },
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb":{
          "name":"Running album","state":"uploading",
          "infohash_v1":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
        }
      }
    })JSON"));
    pane.show();
    auto *table = pane.table();

    for (const auto &row : {
        qMakePair(QStringLiteral("Stopped album"), QStringLiteral("torrentContextStart")),
        qMakePair(QStringLiteral("Running album"), QStringLiteral("torrentContextStop"))
    })
    {
        const QModelIndex index = indexForName(table, row.first);
        QVERIFY(index.isValid());
        QString error;
        QTimer::singleShot(0, &pane, [&]() {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu) { error = QStringLiteral("Missing native context menu"); return; }
            QList<QAction *> actions;
            actions.append(actionNamed(menu, row.second));
            actions.append(actionNamed(menu, QStringLiteral("torrentContextRemove")));
            QMenu *copy = copySubmenu(menu);
            actions.append(copy ? copy->menuAction() : nullptr);
            if (copy)
            {
                for (const char *name : {
                    "torrentCopyName", "torrentCopyHashV1",
                    "torrentCopyHashV2", "torrentCopyMagnet",
                    "torrentCopyId", "torrentCopyComment",
                    "torrentCopyContentPath"
                })
                    actions.append(actionNamed(copy, QString::fromLatin1(name)));
            }
            for (QAction *action : actions)
            {
                if (!action || action->icon().isNull()
                    || !action->isIconVisibleInMenu()
                    || action->icon().pixmap(16, 16).isNull())
                {
                    error = QStringLiteral("Menu action has no visible native SVG icon: ")
                        + (action ? action->objectName() : QStringLiteral("<missing>"));
                    break;
                }
                const QString expected = expectedIcon.value(action->objectName());
                const QPixmap expectedPixmap = QIcon(
                    QStringLiteral(":/icons/") + expected + QStringLiteral(".svg"))
                    .pixmap(16, 16);
                if (expected.isEmpty() || expectedPixmap.isNull()
                    || action->icon().pixmap(16, 16).toImage()
                        != expectedPixmap.toImage())
                {
                    error = QStringLiteral("Incorrect upstream icon for menu action: ")
                        + action->objectName();
                    break;
                }
            }
            menu->close();
        });
        QVERIFY(QMetaObject::invokeMethod(table, "customContextMenuRequested",
            Qt::DirectConnection, Q_ARG(QPoint, table->visualRect(index).center())));
        QVERIFY2(error.isEmpty(), qPrintable(error));
    }
}

void TorrentContextMenuWorkflowTest::rightClickUnselectedRowTargetsOnlyThatTorrent()
{
    ServerPane pane;
    pane.resize(1200, 740);
    pane.applySync(payload(R"JSON({
      "full_update":true,
      "torrents":{
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa":{
          "name":"Album 2","state":"stoppedUP","infohash_v1":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        },
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb":{
          "name":"Album 10","state":"uploading","infohash_v1":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
        }
      }
    })JSON"));
    pane.show();
    QTRY_VERIFY(pane.isVisible());
    auto *table = pane.table();
    const QModelIndex album2 = indexForName(table, QStringLiteral("Album 2"));
    const QModelIndex album10 = indexForName(table, QStringLiteral("Album 10"));
    QVERIFY(album2.isValid() && album10.isValid());
    table->selectionModel()->select(album2, QItemSelectionModel::ClearAndSelect
        | QItemSelectionModel::Rows);
    QCOMPARE(table->selectionModel()->selectedRows().size(), 1);

    int called = 0;
    ServerPane::TorrentContextAction emitted = ServerPane::TorrentContextAction::Remove;
    connect(&pane, &ServerPane::torrentContextActionRequested, &pane,
        [&](ServerPane::TorrentContextAction action) {
            ++called;
            emitted = action;
        });

    QString error;
    QTimer::singleShot(0, &pane, [&]() {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu) { error = QStringLiteral("No visible torrent context menu"); return; }
        if (menu->objectName() != QLatin1String("torrentListContextMenu"))
        {
            error = QStringLiteral("Unexpected context menu");
            menu->close();
            return;
        }
        QAction *stop = actionNamed(menu, QStringLiteral("torrentContextStop"));
        QAction *start = actionNamed(menu, QStringLiteral("torrentContextStart"));
        QAction *remove = actionNamed(menu, QStringLiteral("torrentContextRemove"));
        if (stop || !start || !remove || !start->isEnabled())
        {
            error = QStringLiteral("Stopped torrent must show Start and Remove, but no Stop");
            menu->close();
            return;
        }
        start->trigger();
        menu->close();
    });
    // Invoke the actual widget signal to open its native QMenu. This avoids
    // relying on platform-specific right-button context-menu event timing.
    QVERIFY(QMetaObject::invokeMethod(table, "customContextMenuRequested",
        Qt::DirectConnection, Q_ARG(QPoint, table->visualRect(album2).center())));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(called, 1);
    QCOMPARE(emitted, ServerPane::TorrentContextAction::Start);
    QCOMPARE(table->selectionModel()->selectedRows().size(), 1);

    // Clicking the other row switches the target before running Stop.
    QTimer::singleShot(0, &pane, [&]() {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu) { error = QStringLiteral("No context menu on second row"); return; }
        QAction *stop = actionNamed(menu, QStringLiteral("torrentContextStop"));
        QAction *start = actionNamed(menu, QStringLiteral("torrentContextStart"));
        if (!stop || start || !stop->isEnabled())
            error = QStringLiteral("Running torrent must show Stop, but no Start");
        if (stop) stop->trigger();
        menu->close();
    });
    QVERIFY(QMetaObject::invokeMethod(table, "customContextMenuRequested",
        Qt::DirectConnection, Q_ARG(QPoint, table->visualRect(album10).center())));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(called, 2);
    QCOMPARE(emitted, ServerPane::TorrentContextAction::Stop);
    QCOMPARE(table->selectionModel()->selectedRows().size(), 1);
    QCOMPARE(table->model()->index(table->selectionModel()->selectedRows().first().row(), 0)
                 .data(Qt::DisplayRole).toString(), QStringLiteral("Album 10"));
}

void TorrentContextMenuWorkflowTest::rightClickSelectedRowPreservesMultipleSelectionAndCopiesNames()
{
    ServerPane pane;
    pane.resize(1200, 740);
    pane.applySync(payload(R"JSON({
      "full_update":true,
      "torrents":{
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa":{"name":"Album 2","state":"uploading"},
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb":{"name":"Album 10","state":"uploading"}
      }
    })JSON"));
    pane.show();
    auto *table = pane.table();
    const QModelIndex first = indexForName(table, QStringLiteral("Album 2"));
    const QModelIndex second = indexForName(table, QStringLiteral("Album 10"));
    QVERIFY(first.isValid() && second.isValid());
    auto *selection = table->selectionModel();
    selection->select(first, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    selection->select(second, QItemSelectionModel::Select | QItemSelectionModel::Rows);
    QCOMPARE(selection->selectedRows().size(), 2);

    QString error;
    QTimer::singleShot(0, &pane, [&]() {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu) { error = QStringLiteral("No multi-select context menu"); return; }
        if (actionNamed(menu, QStringLiteral("torrentContextStart"))
            || !actionNamed(menu, QStringLiteral("torrentContextStop")))
            error = QStringLiteral("All-running selection must show Stop only");
        QMenu *copy = copySubmenu(menu);
        QAction *name = copy ? actionNamed(copy, QStringLiteral("torrentCopyName")) : nullptr;
        if (!name || !name->isEnabled())
            error = QStringLiteral("Copy Name not enabled for selected rows");
        if (name) name->trigger();
        menu->close();
    });
    QVERIFY(QMetaObject::invokeMethod(table, "customContextMenuRequested",
        Qt::DirectConnection, Q_ARG(QPoint, table->visualRect(first).center())));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(selection->selectedRows().size(), 2);
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("Album 2\nAlbum 10"));
}

void TorrentContextMenuWorkflowTest::mixedSelectionOffersBothStartAndStop()
{
    ServerPane pane;
    pane.resize(1200, 740);
    pane.applySync(payload(R"JSON({
      "full_update":true,
      "torrents":{
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa":{"name":"Stopped track","state":"stoppedUP"},
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb":{"name":"Running track","state":"uploading"}
      }
    })JSON"));
    pane.show();
    auto *table = pane.table();
    const QModelIndex stopped = indexForName(table, QStringLiteral("Stopped track"));
    const QModelIndex running = indexForName(table, QStringLiteral("Running track"));
    QVERIFY(stopped.isValid() && running.isValid());
    auto *selection = table->selectionModel();
    selection->select(stopped, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    selection->select(running, QItemSelectionModel::Select | QItemSelectionModel::Rows);
    QCOMPARE(selection->selectedRows().size(), 2);

    QString error;
    QTimer::singleShot(0, &pane, [&]() {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu) { error = QStringLiteral("No mixed-selection context menu"); return; }
        QAction *start = actionNamed(menu, QStringLiteral("torrentContextStart"));
        QAction *stop = actionNamed(menu, QStringLiteral("torrentContextStop"));
        if (!start || !stop || !start->isEnabled() || !stop->isEnabled())
            error = QStringLiteral("Mixed running/stopped selection must offer both actions");
        menu->close();
    });
    QVERIFY(QMetaObject::invokeMethod(table, "customContextMenuRequested",
        Qt::DirectConnection, Q_ARG(QPoint, table->visualRect(stopped).center())));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(selection->selectedRows().size(), 2);

    // Both stopped: only Start should be available for the selected group.
    pane.applySync(payload(R"JSON({"torrents":{
      "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb":{"state":"stoppedDL"}
    }})JSON"));
    QTimer::singleShot(0, &pane, [&]() {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu) { error = QStringLiteral("No all-stopped context menu"); return; }
        QAction *start = actionNamed(menu, QStringLiteral("torrentContextStart"));
        QAction *stop = actionNamed(menu, QStringLiteral("torrentContextStop"));
        if (!start || stop || !start->isEnabled())
            error = QStringLiteral("All-stopped selection must offer Start only");
        menu->close();
    });
    QVERIFY(QMetaObject::invokeMethod(table, "customContextMenuRequested",
        Qt::DirectConnection,
        Q_ARG(QPoint, table->visualRect(indexForName(table, QStringLiteral("Stopped track"))).center())));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(selection->selectedRows().size(), 2);
}

void TorrentContextMenuWorkflowTest::copySubmenuCopiesRealRemoteFieldsAndDisablesUnavailableMetadata()
{
    ServerPane pane;
    pane.resize(1200, 740);
    const QString hash = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    pane.applySync(payload(R"JSON({
      "full_update":true,
      "torrents":{
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa":{
          "name":"My music","state":"uploading",
          "infohash_v1":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
          "content_path":"/share/Music/My music",
          "magnet_uri":"magnet:?xt=urn:btih:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        }
      }
    })JSON"));
    pane.show();
    auto *table = pane.table();
    const QModelIndex row = indexForName(table, QStringLiteral("My music"));
    QVERIFY(row.isValid());
    const auto openAndCopy = [&](const QString &actionName, bool shouldExist) {
        QString error;
        bool observedEnabled = false;
        QTimer::singleShot(0, &pane, [&]() {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu) { error = QStringLiteral("Context menu did not open"); return; }
            QMenu *copy = copySubmenu(menu);
            QAction *action = copy ? actionNamed(copy, actionName) : nullptr;
            if (!action)
                error = QStringLiteral("Missing copy action: ") + actionName;
            else
            {
                observedEnabled = action->isEnabled();
                if (observedEnabled)
                    action->trigger();
            }
            menu->close();
        });
        const bool invoked = QMetaObject::invokeMethod(table, "customContextMenuRequested",
            Qt::DirectConnection, Q_ARG(QPoint, table->visualRect(row).center()));
        QVERIFY(invoked);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(observedEnabled, shouldExist);
    };

    openAndCopy(QStringLiteral("torrentCopyId"), true);
    QCOMPARE(QApplication::clipboard()->text(), hash);
    openAndCopy(QStringLiteral("torrentCopyContentPath"), true);
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("/share/Music/My music"));
    openAndCopy(QStringLiteral("torrentCopyMagnet"), true);
    QCOMPARE(QApplication::clipboard()->text(),
             QStringLiteral("magnet:?xt=urn:btih:") + hash);

    // The sync API did not supply v2 or comment: do NOT manufacture values.
    QApplication::clipboard()->setText(QStringLiteral("previous clipboard"));
    openAndCopy(QStringLiteral("torrentCopyHashV2"), false);
    openAndCopy(QStringLiteral("torrentCopyComment"), false);
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("previous clipboard"));

    // The context menu's Remove action exists; actual deletion must still
    // route through the existing MainWindow confirmation and remote session.
    QString error;
    bool foundRemove = false;
    QTimer::singleShot(0, &pane, [&]() {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu) { error = QStringLiteral("Context menu did not open"); return; }
        QAction *remove = actionNamed(menu, QStringLiteral("torrentContextRemove"));
        foundRemove = remove && remove->isEnabled();
        menu->close();
    });
    QVERIFY(QMetaObject::invokeMethod(table, "customContextMenuRequested",
        Qt::DirectConnection, Q_ARG(QPoint, table->visualRect(row).center())));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(foundRemove);
}

QTEST_MAIN(TorrentContextMenuWorkflowTest)
#include "test_torrent_context_menu_workflow.moc"
