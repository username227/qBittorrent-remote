// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/ServerPane.h"
#include "gui/FilterSidebar.h"
#include "gui/TorrentModel.h"

#include <QHeaderView>
#include <QJsonDocument>
#include <QListWidget>
#include <QSettings>
#include <QTemporaryDir>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QtTest/QtTest>

// Exercise actual clickable native sidebar and its visible transfer table.
// Deliberately use disposable /sync/maindata payloads: never touch a NAS.
namespace
{
static QJsonObject payload(const char *json)
{
    return QJsonDocument::fromJson(QByteArray(json)).object();
}
static QListWidget *list(ServerPane &pane, const char *name)
{
    return pane.sidebar()->findChild<QListWidget *>(QString::fromLatin1(name));
}
static int row(QListWidget *list, const QString &key)
{
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->data(Qt::UserRole).toString() == key)
            return i;
    return -1;
}
static void click(QListWidget *list, const QString &key)
{
    const int index = row(list, key);
    QVERIFY2(index >= 0, qPrintable(QStringLiteral("Missing sidebar item: ") + key));
    list->scrollToItem(list->item(index));
    QTest::mouseClick(list->viewport(), Qt::LeftButton,
                      Qt::NoModifier, list->visualItemRect(list->item(index)).center());
    QTRY_COMPARE(list->currentRow(), index);
}
static QStringList visible(ServerPane &pane)
{
    QStringList hashes;
    for (int row = 0; row < pane.proxy()->rowCount(); ++row)
    {
        const QModelIndex source = pane.proxy()->mapToSource(pane.proxy()->index(row, 0));
        hashes.append(pane.model()->hashForRow(source.row()));
    }
    hashes.sort();
    return hashes;
}

} // namespace

class SidebarWorkflowTest final : public QObject
{
    Q_OBJECT
private slots:
    void torrentNameColumnIsResizableAndRemembersUserWidth()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QSettings settings(temporary.filePath(QStringLiteral("layout.ini")),
                           QSettings::IniFormat);
        const QString profile = QStringLiteral("windows-name-width");

        ServerPane pane;
        pane.resize(1350, 700);
        pane.show();
        auto *header = pane.table()->horizontalHeader();
        QCOMPARE(header->sectionResizeMode(0), QHeaderView::Interactive);
        QVERIFY2(header->sectionSize(0) >= 320,
                 "Name must not collapse when other columns fill the window.");

        // A real manual section width must survive per-server layout restore.
        header->resizeSection(0, 450);
        QCOMPARE(header->sectionSize(0), 450);
        pane.saveLayout(settings, profile);

        ServerPane restored;
        restored.resize(1350, 700);
        restored.restoreLayout(settings, profile);
        auto *restoredHeader = restored.table()->horizontalHeader();
        QCOMPARE(restoredHeader->sectionResizeMode(0), QHeaderView::Interactive);
        QCOMPARE(restoredHeader->sectionSize(0), 450);

        // Users may intentionally shrink the column after this fix.
        restoredHeader->resizeSection(0, 170);
        restored.saveLayout(settings, profile);
        ServerPane reopened;
        reopened.restoreLayout(settings, profile);
        QCOMPARE(reopened.table()->horizontalHeader()->sectionResizeMode(0),
                 QHeaderView::Interactive);
        QCOMPARE(reopened.table()->horizontalHeader()->sectionSize(0), 170);
    }

    void narrowLegacyTorrentNameColumnMigratesOnce()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QSettings settings(temporary.filePath(QStringLiteral("old.ini")),
                           QSettings::IniFormat);
        const QString profile = QStringLiteral("pre-fix-layout");

        ServerPane legacy;
        auto *oldHeader = legacy.table()->horizontalHeader();
        oldHeader->resizeSection(0, 38);
        legacy.saveLayout(settings, profile);
        // Simulate header data written by the old, stretch-based release,
        // before the one-time migration flag was introduced.
        for (const QString &key : settings.allKeys())
            if (key.endsWith(QStringLiteral("/nameColumnResizableV1")))
                settings.remove(key);

        ServerPane migrated;
        migrated.restoreLayout(settings, profile);
        auto *newHeader = migrated.table()->horizontalHeader();
        QCOMPARE(newHeader->sectionResizeMode(0), QHeaderView::Interactive);
        QVERIFY2(newHeader->sectionSize(0) >= 320,
                 "Legacy Windows layout should receive a usable Name width.");
    }

    void combineStatusCategoryTagTrackerAndSearch();
    void independentServerTabsKeepIndependentSidebarStates();
};

void SidebarWorkflowTest::combineStatusCategoryTagTrackerAndSearch()
    {
        ServerPane pane;
        pane.resize(1300, 840);
        pane.show();
        QTRY_VERIFY(pane.isVisible());
        pane.applySync(payload(R"JSON({
          "full_update":true,
          "categories":{"Music":{},"Movies":{},"Unused":{}},
          "tags":["test","jazz","film","unused"],
          "trackers":{"https://tracker.music.example/announce/passkey":["a","c"],
                      "udp://tracker.music.example:1337/announce":["a"],
                      "https://tracker.movies.example/announce":["b"]},
          "torrents":{
            "a":{"name":"Alpha","state":"downloading","progress":0.5,"dlspeed":200,
                 "upspeed":0,"category":"Music","tags":"test, jazz",
                 "tracker":"https://tracker.music.example/announce/passkey"},
            "b":{"name":"Beta","state":"uploading","progress":1,"dlspeed":0,
                 "upspeed":0,"category":"Movies","tags":"film",
                 "tracker":"https://tracker.movies.example/announce"},
            "c":{"name":"Charlie","state":"stalledDL","progress":0.3,"dlspeed":0,
                 "upspeed":0,"category":"Music","tags":"test",
                 "tracker":"https://tracker.music.example/announce/passkey",
                 "has_tracker_warning":true},
            "d":{"name":"Delta","state":"stoppedUP","progress":1,"dlspeed":0,
                 "upspeed":0,"category":"","tags":"","tracker":"",
                 "has_tracker_error":true}
          }
        })JSON"));
        auto *status = list(pane, "filterList");
        auto *categories = list(pane, "categoryFilterList");
        auto *tags = list(pane, "tagFilterList");
        auto *trackers = list(pane, "trackerFilterList");
        QVERIFY(status && categories && tags && trackers);
        QCOMPARE(pane.proxy()->rowCount(), 4);
        QCOMPARE(status->item(row(status,"active"))->text(), QStringLiteral("Active (1)"));
        QCOMPARE(categories->item(row(categories,"category:Music"))->text(),
                 QStringLiteral("Music (2)"));
        QCOMPARE(tags->item(row(tags,"tag:test"))->text(), QStringLiteral("test (2)"));
        QCOMPARE(trackers->item(row(trackers,"tracker:tracker.music.example"))->text(),
                 QStringLiteral("tracker.music.example (2)"));
        QVERIFY(row(trackers, "tracker:https://tracker.music.example/announce/passkey") < 0);

        click(status, "active");
        QCOMPARE(visible(pane), QStringList({"a"}));
        click(categories, "category:Music");
        click(tags, "tag:test");
        click(trackers, "tracker:tracker.music.example");
        QCOMPARE(visible(pane), QStringList({"a"})); // all four conditions
        click(trackers, "tracker:tracker.movies.example");
        QCOMPARE(pane.proxy()->rowCount(), 0); // AND, not union
        click(trackers, "tracker:tracker.music.example");
        pane.setSearchTerm(QStringLiteral("Beta"));
        QCOMPARE(pane.proxy()->rowCount(), 0);
        pane.setSearchTerm(QStringLiteral("Alpha"));
        QCOMPARE(visible(pane), QStringList({"a"}));
        pane.setSearchTerm(QString());

        pane.applySync(payload(R"JSON({
          "torrents":{"a":{"dlspeed":0,"tags":"jazz","category":"Movies"}},
          "trackers":{"https://tracker.music.example/announce/passkey":["c"],
                      "udp://tracker.music.example:1337/announce":[]}
        })JSON"));
        QCOMPARE(pane.proxy()->rowCount(), 0);
        QCOMPARE(status->item(row(status,"active"))->text(), QStringLiteral("Active (0)"));
        QCOMPARE(tags->item(row(tags,"tag:test"))->text(), QStringLiteral("test (1)"));
        click(status,"all");
        click(categories,"all");
        click(tags,"all");
        click(trackers,"tracker:tracker.music.example");
        QCOMPARE(visible(pane), QStringList({"a","c"})); // primary tracker fallback

        pane.applySync(payload(R"JSON({
          "trackers_removed":["https://tracker.music.example/announce/passkey",
                              "udp://tracker.music.example:1337/announce"],
          "torrents":{"a":{"tracker":""},"c":{"tracker":""}}
        })JSON"));
        QVERIFY(row(trackers,"tracker:tracker.music.example") < 0);
        QCOMPARE(pane.proxy()->rowCount(), 4); // disappeared choice resets to All
        click(trackers,"trackerless");
        QCOMPARE(visible(pane), QStringList({"a","c","d"}));
        click(trackers,"tracker-error");
        QCOMPARE(visible(pane), QStringList({"d"}));
        click(trackers,"warning");
        QCOMPARE(visible(pane), QStringList({"c"}));

        pane.applySync(payload(R"JSON({"categories_removed":["Unused"],
          "tags_removed":["unused"],"torrents_removed":["c"]})JSON"));
        QVERIFY(row(categories,"category:Unused") < 0);
        QVERIFY(row(tags,"tag:unused") < 0);
        QCOMPARE(pane.model()->rowCount(), 3);
    }


void SidebarWorkflowTest::independentServerTabsKeepIndependentSidebarStates()
    {
        ServerPane first, second;
        first.resize(1100,700);
        second.resize(1100,700);
        first.show();
        second.show();
        QTRY_VERIFY(first.isVisible());
        QTRY_VERIFY(second.isVisible());
        first.applySync(payload(R"({"full_update":true,"torrents":{
          "a":{"name":"Alpha","state":"downloading","progress":0.5,"dlspeed":10},
          "b":{"name":"Beta","state":"stoppedUP","progress":1,"dlspeed":0}}})"));
        second.applySync(payload(R"({"full_update":true,"torrents":{
          "x":{"name":"Xray","state":"downloading","progress":0.5,"dlspeed":0},
          "y":{"name":"Yankee","state":"uploading","progress":1,"upspeed":10}}})"));
        click(list(first,"filterList"),"active");
        QCOMPARE(visible(first), QStringList({"a"}));
        QCOMPARE(visible(second), (QStringList{"x","y"}));
        click(list(second,"filterList"),"inactive");
        QCOMPARE(visible(second), QStringList({"x"}));
        QCOMPARE(visible(first), QStringList({"a"}));
        first.applySync(payload(R"({"torrents":{"a":{"dlspeed":0}}})"));
        QCOMPARE(first.proxy()->rowCount(), 0);
        QCOMPARE(visible(second), QStringList({"x"}));
    }
QTEST_MAIN(SidebarWorkflowTest)
#include "test_sidebar_filter_workflow.moc"
