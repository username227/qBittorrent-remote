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
#include <QShortcut>
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
    void forceActionsRespectTorrentStatesAndSelections();
    void queueSubmenuReflectsRemoteQueueingAndIncompleteSelections();
    void renameAppearsOnlyForExactlyOneSelectedTorrent();
    void copySubmenuCopiesRealRemoteFieldsAndDisablesUnavailableMetadata();
    void categoryTagsAutoManagementMatchUpstreamOrderAndSelection();
    void editTrackersFollowsRenameAndRespectsSelection();
    void remainingNativeContextActionsRespectSelectionAndModes();
};

void TorrentContextMenuWorkflowTest::contextMenuUsesNativeQbittorrentIcons()
{
    // Validate the real resource paths in the linked Qt static GUI target,
    // not just the presence of icon file names in source.
    const QStringList nativeIcons = {
        QStringLiteral("torrent-start"), QStringLiteral("torrent-stop"),
        QStringLiteral("list-remove"), QStringLiteral("edit-copy"),
        QStringLiteral("torrent-start-forced"), QStringLiteral("force-recheck"),
        QStringLiteral("reannounce"), QStringLiteral("name"), QStringLiteral("hash"),
        QStringLiteral("torrent-magnet"), QStringLiteral("help-about"),
        QStringLiteral("directory"), QStringLiteral("edit-rename")
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
        {QStringLiteral("torrentContextForceStart"), QStringLiteral("torrent-start-forced")},
        {QStringLiteral("torrentContextForceRecheck"), QStringLiteral("force-recheck")},
        {QStringLiteral("torrentContextForceReannounce"), QStringLiteral("reannounce")},
        {QStringLiteral("torrentContextRemove"), QStringLiteral("list-remove")},
        {QStringLiteral("torrentContextRename"), QStringLiteral("edit-rename")},
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
            actions.append(actionNamed(menu, QStringLiteral("torrentContextRename")));
            actions.append(actionNamed(menu, QStringLiteral("torrentContextForceStart")));
            actions.append(actionNamed(menu, QStringLiteral("torrentContextForceRecheck")));
            actions.append(actionNamed(menu, QStringLiteral("torrentContextForceReannounce")));
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


void TorrentContextMenuWorkflowTest::forceActionsRespectTorrentStatesAndSelections()
{
    ServerPane pane;
    pane.resize(1200, 740);
    pane.applySync(payload(R"JSON({
      "full_update":true,
      "torrents":{
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa":{"name":"Stopped","state":"stoppedUP","force_start":false},
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb":{"name":"Downloading","state":"downloading","force_start":false},
        "cccccccccccccccccccccccccccccccccccccccc":{"name":"Forced","state":"forcedUP","force_start":true},
        "dddddddddddddddddddddddddddddddddddddddd":{"name":"Queued","state":"queuedDL","force_start":false},
        "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee":{"name":"Checking","state":"checkingUP","force_start":false},
        "ffffffffffffffffffffffffffffffffffffffff":{"name":"Metadata","state":"metaDL","force_start":false,"has_metadata":false},
        "1111111111111111111111111111111111111111":{"name":"Missing","state":"missingFiles","force_start":false}
      }
    })JSON"));
    pane.show();
    auto *table = pane.table();
    QList<ServerPane::TorrentContextAction> emitted;
    connect(&pane, &ServerPane::torrentContextActionRequested, &pane,
        [&](ServerPane::TorrentContextAction action) { emitted.append(action); });

    struct Case {
        const char *name;
        bool forceStart;
        bool recheck;
        bool reannounceEnabled;
    };
    const QList<Case> cases {
        {"Stopped", true, true, false},
        {"Downloading", true, true, true},
        {"Forced", false, true, true},
        {"Queued", true, true, false},
        {"Checking", true, true, false},
        {"Metadata", true, false, true},
        {"Missing", true, true, false}
    };
    for (const Case &item : cases)
    {
        const QModelIndex index = indexForName(table, QString::fromLatin1(item.name));
        QVERIFY(index.isValid());
        table->selectionModel()->setCurrentIndex(index,
            QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        QString error;
        QTimer::singleShot(0, &pane, [&]() {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu) { error = QStringLiteral("Missing context menu"); return; }
            auto *force = actionNamed(menu, QStringLiteral("torrentContextForceStart"));
            auto *recheck = actionNamed(menu, QStringLiteral("torrentContextForceRecheck"));
            auto *reannounce = actionNamed(menu, QStringLiteral("torrentContextForceReannounce"));
            if (static_cast<bool>(force) != item.forceStart
                || static_cast<bool>(recheck) != item.recheck
                || !reannounce || reannounce->isEnabled() != item.reannounceEnabled)
                error = QStringLiteral("Incorrect force-action availability for ")
                    + QString::fromLatin1(item.name);
            if (reannounce && !item.reannounceEnabled
                && reannounce->toolTip().isEmpty())
                error = QStringLiteral("Disabled reannounce needs an explanation");
            if (QString::fromLatin1(item.name) == QLatin1String("Downloading")
                && error.isEmpty())
            {
                // Trigger actual menu actions and check the signals, not a
                // synthetic enum dispatch detached from the real QMenu.
                force->trigger();
                recheck->trigger();
                reannounce->trigger();
            }
            menu->close();
        });
        QVERIFY(QMetaObject::invokeMethod(table, "customContextMenuRequested",
            Qt::DirectConnection, Q_ARG(QPoint, table->visualRect(index).center())));
        QVERIFY2(error.isEmpty(), qPrintable(error));
    }
    QCOMPARE(emitted.size(), 3);
    QCOMPARE(emitted.at(0), ServerPane::TorrentContextAction::ForceStart);
    QCOMPARE(emitted.at(1), ServerPane::TorrentContextAction::ForceRecheck);
    QCOMPARE(emitted.at(2), ServerPane::TorrentContextAction::ForceReannounce);

    // Mixed selection: a forced torrent must not suppress Force Start for
    // an ordinary torrent, and a queued torrent must not disable reannounce
    // when another selected torrent is eligible.
    auto *selection = table->selectionModel();
    const QModelIndex forced = indexForName(table, QStringLiteral("Forced"));
    const QModelIndex queued = indexForName(table, QStringLiteral("Queued"));
    const QModelIndex downloading = indexForName(table, QStringLiteral("Downloading"));
    selection->select(forced, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    selection->select(queued, QItemSelectionModel::Select | QItemSelectionModel::Rows);
    selection->select(downloading, QItemSelectionModel::Select | QItemSelectionModel::Rows);
    QString error;
    QTimer::singleShot(0, &pane, [&]() {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu) { error = QStringLiteral("No mixed-selection menu"); return; }
        auto *force = actionNamed(menu, QStringLiteral("torrentContextForceStart"));
        auto *reannounce = actionNamed(menu, QStringLiteral("torrentContextForceReannounce"));
        if (!force || !reannounce || !force->isEnabled() || !reannounce->isEnabled())
            error = QStringLiteral("Mixed states must enable force and reannounce");
        menu->close();
    });
    QVERIFY(QMetaObject::invokeMethod(table, "customContextMenuRequested",
        Qt::DirectConnection, Q_ARG(QPoint, table->visualRect(forced).center())));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(selection->selectedRows().size(), 3);
}


void TorrentContextMenuWorkflowTest::queueSubmenuReflectsRemoteQueueingAndIncompleteSelections()
{
    // Verify the actual native right-click menu against the server's sync
    // information, not a hand-built QAction or an internal helper function.
    ServerPane pane;
    pane.resize(1200, 740);
    pane.applySync(payload(R"JSON({
      "full_update":true,
      "server_state":{"queueing":true},
      "torrents":{
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa":{
          "name":"Incomplete album","state":"downloading","progress":0.4
        },
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb":{
          "name":"Completed album","state":"uploading","progress":1
        }
      }
    })JSON"));
    pane.show();
    auto *table = pane.table();
    QList<ServerPane::TorrentContextAction> triggered;
    connect(&pane, &ServerPane::torrentContextActionRequested, &pane,
        [&](ServerPane::TorrentContextAction action) { triggered.append(action); });

    const auto checkMenu = [&](const QModelIndex &index,
                               bool expectedVisible, const char *invoke = nullptr) {
        QString error;
        QTimer::singleShot(0, &pane, [&]() {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu)
            {
                error = QStringLiteral("Right click did not open the native torrent menu");
                return;
            }
            QAction *queueEntry = actionNamed(menu, QStringLiteral("torrentContextQueueMenu"));
            QMenu *queue = queueEntry ? queueEntry->menu() : nullptr;
            if (static_cast<bool>(queue) != expectedVisible)
                error = QStringLiteral("Queue menu visibility disagrees with remote server state");
            if (queue && error.isEmpty())
            {
                if (queue->objectName() != QLatin1String("torrentContextQueueMenu")
                    || queue->title() != QStringLiteral("Queue"))
                    error = QStringLiteral("Not the original native Queue submenu");

                const QHash<QString, QString> expectedIcons {
                    {QStringLiteral("torrentContextQueueMenu"), QStringLiteral("queued")},
                    {QStringLiteral("torrentQueueTop"), QStringLiteral("go-top")},
                    {QStringLiteral("torrentQueueUp"), QStringLiteral("go-up")},
                    {QStringLiteral("torrentQueueDown"), QStringLiteral("go-down")},
                    {QStringLiteral("torrentQueueBottom"), QStringLiteral("go-bottom")}
                };
                const QStringList expectedActions {
                    QStringLiteral("torrentQueueTop"), QStringLiteral("torrentQueueUp"),
                    QStringLiteral("torrentQueueDown"), QStringLiteral("torrentQueueBottom")
                };
                if (queue->actions().size() != 4)
                    error = QStringLiteral("Queue submenu does not contain exactly four commands");
                for (int i = 0; i < expectedActions.size() && error.isEmpty(); ++i)
                {
                    QAction *a = actionNamed(queue, expectedActions.at(i));
                    const QString expectedIcon = expectedIcons.value(expectedActions.at(i));
                    const QPixmap actual = a ? a->icon().pixmap(16, 16) : QPixmap();
                    const QPixmap expected = QIcon(QStringLiteral(":/icons/") + expectedIcon
                        + QStringLiteral(".svg")).pixmap(16, 16);
                    if (!a || queue->actions().at(i) != a || !a->isEnabled()
                        || !a->isIconVisibleInMenu() || actual.isNull()
                        || expected.isNull() || actual.toImage() != expected.toImage())
                        error = QStringLiteral("Incorrect Queue action, order or native icon: ")
                            + expectedActions.at(i);
                }
                const QPixmap queueIcon = queueEntry->icon().pixmap(16, 16);
                const QPixmap expectedQueueIcon = QIcon(
                    QStringLiteral(":/icons/queued.svg")).pixmap(16, 16);
                if (queueIcon.isNull() || expectedQueueIcon.isNull()
                    || queueIcon.toImage() != expectedQueueIcon.toImage()
                    || !queueEntry->isIconVisibleInMenu())
                    error = QStringLiteral("Incorrect Queue submenu icon");

                if (invoke && error.isEmpty())
                {
                    QAction *action = actionNamed(queue, QString::fromLatin1(invoke));
                    if (action) action->trigger();
                }
            }
            menu->close();
        });
        if (!QMetaObject::invokeMethod(table, "customContextMenuRequested",
                Qt::DirectConnection, Q_ARG(QPoint, table->visualRect(index).center())))
            error = QStringLiteral("Could not open torrent context menu");
        return error;
    };

    const QModelIndex incomplete = indexForName(table, QStringLiteral("Incomplete album"));
    const QModelIndex completed = indexForName(table, QStringLiteral("Completed album"));
    QVERIFY(incomplete.isValid());
    QVERIFY(completed.isValid());

    table->selectionModel()->select(incomplete,
        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QVERIFY2(checkMenu(incomplete, true, "torrentQueueTop").isEmpty(),
        "Enabled server with an incomplete torrent must expose working Queue actions.");
    QCOMPARE(triggered.size(), 1);
    QCOMPARE(triggered.last(), ServerPane::TorrentContextAction::QueueTop);

    table->selectionModel()->select(completed,
        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    const QString completedError = checkMenu(completed, false);
    QVERIFY2(completedError.isEmpty(), qPrintable(completedError));

    // A mixed selection must preserve both rows and offer queue operations.
    table->selectionModel()->select(incomplete,
        QItemSelectionModel::Select | QItemSelectionModel::Rows);
    QCOMPARE(table->selectionModel()->selectedRows().size(), 2);
    const QString mixedError = checkMenu(completed, true, "torrentQueueBottom");
    QVERIFY2(mixedError.isEmpty(), qPrintable(mixedError));
    QCOMPARE(triggered.last(), ServerPane::TorrentContextAction::QueueBottom);
    QCOMPARE(table->selectionModel()->selectedRows().size(), 2);

    // A live /sync/maindata change must hide the menu immediately, without
    // changing selection, reconnecting or restarting the application.
    pane.applySync(payload(R"JSON({
      "full_update":false,"server_state":{"queueing":false}
    })JSON"));
    const QString disabledError = checkMenu(completed, false);
    QVERIFY2(disabledError.isEmpty(), qPrintable(disabledError));

    pane.applySync(payload(R"JSON({
      "full_update":false,"server_state":{"queueing":true}
    })JSON"));
    const QString enabledAgainError = checkMenu(completed, true, "torrentQueueUp");
    QVERIFY2(enabledAgainError.isEmpty(), qPrintable(enabledAgainError));
    QCOMPARE(triggered.last(), ServerPane::TorrentContextAction::QueueUp);
}

void TorrentContextMenuWorkflowTest::renameAppearsOnlyForExactlyOneSelectedTorrent()
{
    ServerPane pane;
    pane.resize(1200, 740);
    pane.applySync(payload(R"JSON({
      "full_update":true,
      "torrents":{
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa":{"name":"Album 1","state":"downloading"},
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb":{"name":"Album 2","state":"uploading"}
      }
    })JSON"));
    pane.show();
    auto *table = pane.table();
    const QModelIndex first = indexForName(table, QStringLiteral("Album 1"));
    const QModelIndex second = indexForName(table, QStringLiteral("Album 2"));
    QVERIFY(first.isValid() && second.isValid());

    int emitted = 0;
    connect(&pane, &ServerPane::torrentContextActionRequested, &pane,
        [&](ServerPane::TorrentContextAction action) {
            if (action == ServerPane::TorrentContextAction::Rename)
                ++emitted;
        });

    // Single selection: native icon and action must be present and usable.
    table->selectionModel()->select(first,
        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QString error;
    QTimer::singleShot(0, &pane, [&]() {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu) { error = QStringLiteral("No native context menu"); return; }
        QAction *rename = actionNamed(menu, QStringLiteral("torrentContextRename"));
        const QPixmap expected = QIcon(
            QStringLiteral(":/icons/edit-rename.svg")).pixmap(16, 16);
        if (!rename || !rename->isEnabled() || !rename->isIconVisibleInMenu()
            || expected.isNull() || rename->icon().pixmap(16, 16).toImage()
                != expected.toImage())
            error = QStringLiteral("Rename is missing, disabled or has the wrong native icon");
        else
            rename->trigger();
        menu->close();
    });
    QVERIFY(QMetaObject::invokeMethod(table, "customContextMenuRequested",
        Qt::DirectConnection, Q_ARG(QPoint, table->visualRect(first).center())));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(emitted, 1);

    // The shortcut is registered to the table, not to the window. The
    // offscreen CI plugin does not reliably activate keyboard focus after a
    // QMenu; invoke the actual registered QShortcut activation directly.
    auto *renameShortcut = table->findChild<QShortcut *>(
        QStringLiteral("torrentRenameShortcut"));
    QVERIFY(renameShortcut);
    QCOMPARE(renameShortcut->key(), QKeySequence(Qt::Key_F2));
    QCOMPARE(renameShortcut->context(), Qt::WidgetWithChildrenShortcut);
    QCOMPARE(renameShortcut->parent(), table);
    QVERIFY(QMetaObject::invokeMethod(renameShortcut, "activated",
                                     Qt::DirectConnection));
    QCOMPARE(emitted, 2);

    // Multi-selection: upstream hides Rename rather than picking a random
    // row and possibly changing the wrong torrent.
    table->selectionModel()->select(second,
        QItemSelectionModel::Select | QItemSelectionModel::Rows);
    QCOMPARE(table->selectionModel()->selectedRows().size(), 2);
    QVERIFY(QMetaObject::invokeMethod(renameShortcut, "activated",
                                     Qt::DirectConnection));
    QCOMPARE(emitted, 2); // No ambiguous rename on multi-selection.
    error.clear();
    QTimer::singleShot(0, &pane, [&]() {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu) { error = QStringLiteral("No native multi-selection menu"); return; }
        if (actionNamed(menu, QStringLiteral("torrentContextRename")))
            error = QStringLiteral("Rename must not appear for multiple selected torrents");
        menu->close();
    });
    QVERIFY(QMetaObject::invokeMethod(table, "customContextMenuRequested",
        Qt::DirectConnection, Q_ARG(QPoint, table->visualRect(first).center())));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(emitted, 2);
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


void TorrentContextMenuWorkflowTest::categoryTagsAutoManagementMatchUpstreamOrderAndSelection()
{
    ServerPane pane;
    pane.resize(1200, 740);
    pane.applySync(payload(R"JSON({
      "full_update":true,
      "categories":{"Music":{"name":"Music"},"Movies":{"name":"Movies"}},
      "tags":["favorite","review"],
      "torrents":{
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa":{
          "name":"First album","state":"uploading","category":"Music",
          "tags":"favorite","auto_tmm":true
        },
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb":{
          "name":"Second album","state":"uploading","category":"Movies",
          "tags":"review","auto_tmm":false
        }
      }
    })JSON"));
    pane.show();
    auto *table = pane.table();
    const QModelIndex first = indexForName(table, QStringLiteral("First album"));
    const QModelIndex second = indexForName(table, QStringLiteral("Second album"));
    QVERIFY(first.isValid() && second.isValid());
    table->selectionModel()->select(first,
        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    table->selectionModel()->select(second,
        QItemSelectionModel::Select | QItemSelectionModel::Rows);

    QSignalSpy category(&pane, &ServerPane::torrentCategoryRequested);
    QSignalSpy tag(&pane, &ServerPane::torrentTagRequested);
    QSignalSpy autoManagement(&pane, &ServerPane::torrentAutoManagementRequested);
    QString error;
    QTimer::singleShot(0, &pane, [&]() {
        QMenu *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu) { error = QStringLiteral("No visible torrent context menu"); return; }
        const auto actions = menu->actions();
        auto indexOf = [&](const QString &name) {
            for (int i = 0; i < actions.size(); ++i)
                if (actions.at(i)->objectName() == name) return i;
            return -1;
        };
        const int rename = indexOf(QStringLiteral("torrentContextSetLocation"));
        const int categories = indexOf(QStringLiteral("torrentContextCategoryMenu"));
        const int tags = indexOf(QStringLiteral("torrentContextTagsMenu"));
        const int automatic = indexOf(QStringLiteral("torrentContextAutoManagement"));
        const int recheck = indexOf(QStringLiteral("torrentContextForceRecheck"));
        if (!(rename >= 0 && categories > rename && tags > categories
              && automatic > tags && recheck > automatic))
            error = QStringLiteral("Category/Tags/Auto TMM are out of upstream order");
        QMenu *categoryMenu = actionNamed(menu, QStringLiteral("torrentContextCategoryMenu"))
            ? actionNamed(menu, QStringLiteral("torrentContextCategoryMenu"))->menu() : nullptr;
        QMenu *tagsMenu = actionNamed(menu, QStringLiteral("torrentContextTagsMenu"))
            ? actionNamed(menu, QStringLiteral("torrentContextTagsMenu"))->menu() : nullptr;
        QAction *autoTMM = actionNamed(menu, QStringLiteral("torrentContextAutoManagement"));
        if (!categoryMenu || !tagsMenu || !autoTMM)
        {
            error = QStringLiteral("Context submenus or Auto TMM checkbox missing");
            menu->close(); return;
        }
        QAction *music = nullptr;
        for (QAction *action : categoryMenu->actions())
            if (action->data().toString() == QStringLiteral("Music"))
                music = action;
        QAction *favorite = nullptr;
        for (QAction *action : tagsMenu->actions())
            if (action->data().toString() == QStringLiteral("favorite"))
                favorite = action;
        if (!music || !favorite || music->isChecked() || favorite->isChecked()
            || autoTMM->isChecked())
            error = QStringLiteral("Mixed selection checkbox/category state incorrect");
        else
        {
            music->trigger();
            favorite->trigger();
            autoTMM->trigger();
        }
        menu->close();
    });
    QVERIFY(QMetaObject::invokeMethod(table, "customContextMenuRequested",
        Qt::DirectConnection, Q_ARG(QPoint, table->visualRect(first).center())));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(category.size(), 1);
    QCOMPARE(category.first().at(0).toString(), QStringLiteral("Music"));
    QCOMPARE(tag.size(), 1);
    QCOMPARE(tag.first().at(0).toString(), QStringLiteral("favorite"));
    QCOMPARE(tag.first().at(1).toBool(), true);
    QCOMPARE(autoManagement.size(), 1);
    QCOMPARE(autoManagement.first().at(0).toBool(), true);

    // A uniform selection must show the checked tag/category/auto state and
    // offer removal rather than blindly adding the same tag again.
    table->selectionModel()->select(first,
        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTimer::singleShot(0, &pane, [&]() {
        QMenu *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu) { error = QStringLiteral("No single-selection menu"); return; }
        QMenu *categoryMenu = actionNamed(menu, QStringLiteral("torrentContextCategoryMenu"))->menu();
        QMenu *tagsMenu = actionNamed(menu, QStringLiteral("torrentContextTagsMenu"))->menu();
        QAction *favorite = nullptr;
        QAction *music = nullptr;
        for (QAction *action : tagsMenu->actions())
            if (action->data().toString() == QStringLiteral("favorite")) favorite = action;
        for (QAction *action : categoryMenu->actions())
            if (action->data().toString() == QStringLiteral("Music")) music = action;
        QAction *autoTMM = actionNamed(menu, QStringLiteral("torrentContextAutoManagement"));
        if (!favorite || !music || !favorite->isChecked()
            || !music->isChecked() || !autoTMM->isChecked())
            error = QStringLiteral("Single selection check state incorrect");
        else
        {
            favorite->trigger();
            autoTMM->trigger();
        }
        menu->close();
    });
    QVERIFY(QMetaObject::invokeMethod(table, "customContextMenuRequested",
        Qt::DirectConnection, Q_ARG(QPoint, table->visualRect(first).center())));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(tag.size(), 2);
    QCOMPARE(tag.last().at(1).toBool(), false);
    QCOMPARE(autoManagement.size(), 2);
    QCOMPARE(autoManagement.last().at(0).toBool(), false);
}


void TorrentContextMenuWorkflowTest::editTrackersFollowsRenameAndRespectsSelection()
{
    ServerPane pane;
    pane.resize(1200, 740);
    pane.applySync(payload(R"JSON({
        "full_update":true,
        "torrents":{
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa":{
                "name":"Alpha","state":"uploading"
            },
            "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb":{
                "name":"Beta","state":"uploading"
            }
        }
    })JSON"));
    pane.show();
    auto *table = pane.table();
    const QModelIndex first = indexForName(table, QStringLiteral("Alpha"));
    const QModelIndex second = indexForName(table, QStringLiteral("Beta"));
    QVERIFY(first.isValid() && second.isValid());
    QSignalSpy emitted(&pane, &ServerPane::torrentContextActionRequested);
    table->selectionModel()->select(first,
        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QString error;
    QTimer::singleShot(0, &pane, [&]() {
        QMenu *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu) { error = QStringLiteral("Missing context menu"); return; }
        QAction *rename = actionNamed(menu, QStringLiteral("torrentContextRename"));
        QAction *manage = actionNamed(menu, QStringLiteral("torrentContextManageContent"));
        QAction *trackers = actionNamed(menu, QStringLiteral("torrentContextEditTrackers"));
        QAction *category = actionNamed(menu, QStringLiteral("torrentContextCategoryMenu"));
        const auto actions = menu->actions();
        if (!rename || !manage || !trackers || !category || !trackers->isEnabled()
            || actions.indexOf(manage) != actions.indexOf(rename) + 1
            || actions.indexOf(trackers) != actions.indexOf(manage) + 1
            || actions.indexOf(category) != actions.indexOf(trackers) + 1
            || trackers->icon().pixmap(16, 16).toImage()
                != QIcon(QStringLiteral(":/icons/edit-rename.svg"))
                    .pixmap(16, 16).toImage())
            error = QStringLiteral("Edit trackers icon/order/state incorrect");
        else
            trackers->trigger();
        menu->close();
    });
    QVERIFY(QMetaObject::invokeMethod(table, "customContextMenuRequested",
        Qt::DirectConnection, Q_ARG(QPoint, table->visualRect(first).center())));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(emitted.size(), 1);
    QCOMPARE(emitted.first().first().value<ServerPane::TorrentContextAction>(),
             ServerPane::TorrentContextAction::EditTrackers);

    table->selectionModel()->select(second,
        QItemSelectionModel::Select | QItemSelectionModel::Rows);
    QTimer::singleShot(0, &pane, [&]() {
        QMenu *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu) { error = QStringLiteral("Missing multi-selection menu"); return; }
        QAction *trackers = actionNamed(menu, QStringLiteral("torrentContextEditTrackers"));
        if (!trackers || !trackers->isEnabled())
            error = QStringLiteral("Bulk tracker edit must be enabled for multiple torrents");
        else
            trackers->trigger();
        menu->close();
    });
    QVERIFY(QMetaObject::invokeMethod(table, "customContextMenuRequested",
        Qt::DirectConnection, Q_ARG(QPoint, table->visualRect(first).center())));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(emitted.size(), 2);
    QCOMPARE(emitted.last().first().value<ServerPane::TorrentContextAction>(),
             ServerPane::TorrentContextAction::EditTrackers);
}


void TorrentContextMenuWorkflowTest::remainingNativeContextActionsRespectSelectionAndModes()
{
    ServerPane pane;
    pane.resize(1200, 740);
    pane.applySync(payload(R"JSON({
        "full_update":true,
        "torrents":{
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa":{
                "name":"Completed seed", "state":"uploading", "progress":1.0,
                "super_seeding":false, "seq_dl":false, "f_l_piece_prio":false
            },
            "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb":{
                "name":"Incomplete download", "state":"downloading", "progress":0.6,
                "super_seeding":false, "seq_dl":false, "f_l_piece_prio":false
            }
        }
    })JSON"));
    pane.show();
    auto *table = pane.table();
    QVERIFY(table);
    const QModelIndex seed = indexForName(table, QStringLiteral("Completed seed"));
    const QModelIndex download = indexForName(table, QStringLiteral("Incomplete download"));
    QVERIFY(seed.isValid() && download.isValid());
    QSignalSpy mode(&pane, &ServerPane::torrentModeRequested);
    QSignalSpy action(&pane, &ServerPane::torrentContextActionRequested);
    QString error;

    table->selectionModel()->select(seed,
        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTimer::singleShot(0, &pane, [&]() {
        QMenu *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu) { error = QStringLiteral("Context menu did not open"); return; }
        QAction *options = actionNamed(menu, QStringLiteral("torrentContextTorrentOptions"));
        QAction *manage = actionNamed(menu, QStringLiteral("torrentContextManageContent"));
        QAction *super = actionNamed(menu, QStringLiteral("torrentContextSuperSeeding"));
        QAction *sequential = actionNamed(menu, QStringLiteral("torrentContextSequentialDownload"));
        QAction *exportFile = actionNamed(menu, QStringLiteral("torrentContextExportTorrent"));
        if (!options || !manage || !super || !exportFile || sequential
            || !super->isEnabled() || super->isChecked())
            error = QStringLiteral("Completed torrent has incorrect context actions");
        else
            super->trigger();
        menu->close();
    });
    QVERIFY(QMetaObject::invokeMethod(table, "customContextMenuRequested",
        Qt::DirectConnection, Q_ARG(QPoint, table->visualRect(seed).center())));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(mode.size(), 1);
    QCOMPARE(mode.first().at(0).value<ServerPane::TorrentContextAction>(),
             ServerPane::TorrentContextAction::SuperSeeding);
    QCOMPARE(mode.first().at(1).toBool(), true);

    table->selectionModel()->select(download,
        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTimer::singleShot(0, &pane, [&]() {
        QMenu *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu) { error = QStringLiteral("Download context menu did not open"); return; }
        QAction *sequential = actionNamed(menu, QStringLiteral("torrentContextSequentialDownload"));
        QAction *firstLast = actionNamed(menu, QStringLiteral("torrentContextFirstLastPieces"));
        QAction *super = actionNamed(menu, QStringLiteral("torrentContextSuperSeeding"));
        QAction *options = actionNamed(menu, QStringLiteral("torrentContextTorrentOptions"));
        QAction *exportFile = actionNamed(menu, QStringLiteral("torrentContextExportTorrent"));
        if (!sequential || !firstLast || super || !options || !exportFile)
            error = QStringLiteral("Incomplete torrent has incorrect context actions");
        else
            sequential->trigger();
        menu->close();
    });
    QVERIFY(QMetaObject::invokeMethod(table, "customContextMenuRequested",
        Qt::DirectConnection, Q_ARG(QPoint, table->visualRect(download).center())));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(mode.size(), 2);
    QCOMPARE(mode.last().at(0).value<ServerPane::TorrentContextAction>(),
             ServerPane::TorrentContextAction::SequentialDownload);
    QCOMPARE(mode.last().at(1).toBool(), true);

    // Bulk selections must not offer individual file management, but still
    // provide the working multi-torrent options and export actions.
    table->selectionModel()->select(seed,
        QItemSelectionModel::Select | QItemSelectionModel::Rows);
    QTimer::singleShot(0, &pane, [&]() {
        QMenu *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu) { error = QStringLiteral("Bulk context menu did not open"); return; }
        QAction *manage = actionNamed(menu, QStringLiteral("torrentContextManageContent"));
        QAction *options = actionNamed(menu, QStringLiteral("torrentContextTorrentOptions"));
        QAction *exportFile = actionNamed(menu, QStringLiteral("torrentContextExportTorrent"));
        if (manage || !options || !exportFile)
            error = QStringLiteral("Bulk selection has incorrect context actions");
        else
            options->trigger();
        menu->close();
    });
    QVERIFY(QMetaObject::invokeMethod(table, "customContextMenuRequested",
        Qt::DirectConnection, Q_ARG(QPoint, table->visualRect(download).center())));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(action.size(), 1);
    QCOMPARE(action.first().at(0).value<ServerPane::TorrentContextAction>(),
             ServerPane::TorrentContextAction::TorrentOptions);
}

// Preserve QtTest's native QApplication and test setup for this suite.
// The shared dispatcher selects this entry point before Qt is initialized.
#define main runTorrentContextMenuWorkflow
QTEST_MAIN(TorrentContextMenuWorkflowTest)
#undef main
#include "test_torrent_context_menu_workflow.moc"
