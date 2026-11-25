// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/ServerPane.h"
#include "gui/FilterSidebar.h"
#include "gui/TorrentModel.h"

#include <algorithm>

#include <QApplication>
#include <QFont>
#include <QHeaderView>
#include <QJsonDocument>
#include <QListWidget>
#include <QMenu>
#include <QTimer>
#include <QTreeWidget>
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
    void torrentColumnHeaderStaysRegularAfterSelectionAndFontChanges()
    {
        ServerPane pane;
        pane.resize(1200, 650);
        pane.applySync(payload(R"JSON({
          "full_update": true,
          "torrents": {
            "aa": {"name":"Album 2","state":"uploading","progress":1},
            "bb": {"name":"Album 10","state":"uploading","progress":1}
          }
        })JSON"));
        pane.show();
        auto *table = pane.table();
        auto *header = table->horizontalHeader();
        QVERIFY(!header->highlightSections());
        QCOMPARE(header->font().weight(), QFont::Normal);
        QVERIFY2(header->font().resolveMask() & QFont::WeightResolved,
                 "Qt's selected headers may turn bold unless weight is explicit");

        const QModelIndex first = table->model()->index(0, 0);
        const QRect rowRect = table->visualRect(first);
        QVERIFY(rowRect.isValid());
        QTest::mouseClick(table->viewport(), Qt::LeftButton,
                          Qt::NoModifier, rowRect.center());
        QVERIFY(table->selectionModel()->hasSelection());
        table->sortByColumn(0, Qt::DescendingOrder);
        QCOMPARE(header->font().weight(), QFont::Normal);
        QVERIFY(header->font().resolveMask() & QFont::WeightResolved);

        // The real View -> Font Size route updates the QApplication font,
        // then calls each ServerPane::refreshFontMetrics(). Verify that
        // the explicit weight does not freeze the old font size.
        const QFont originalFont = QApplication::font();
        if (originalFont.pointSize() > 0)
        {
            QFont enlarged = originalFont;
            enlarged.setPointSize(originalFont.pointSize() + 2);
            QApplication::setFont(enlarged);
            pane.refreshFontMetrics();
            const int newSize = header->font().pointSize();
            const QFont::Weight newWeight = header->font().weight();
            QApplication::setFont(originalFont);
            pane.refreshFontMetrics();
            QCOMPARE(newSize, enlarged.pointSize());
            QCOMPARE(newWeight, QFont::Normal);
            QCOMPARE(header->font().pointSize(), originalFont.pointSize());
        }
    }

    void restoringLegacyHeaderLayoutCannotReenableBoldSelectionHeadings()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QSettings settings(temporary.filePath(QStringLiteral("layout.ini")),
                           QSettings::IniFormat);
        const QString profile = QStringLiteral("legacy-highlighted-transfer-header");

        // Simulate real persisted layouts created while highlighting was on.
        // QHeaderView::saveState() stores highlightSections, not just widths.
        ServerPane legacy;
        auto *legacyHeader = legacy.table()->horizontalHeader();
        legacyHeader->setHighlightSections(true);
        QVERIFY(legacyHeader->highlightSections());
        legacyHeader->resizeSection(0, 430);
        legacy.saveLayout(settings, profile);

        ServerPane restored;
        restored.resize(1300, 700);
        restored.restoreLayout(settings, profile);
        auto *header = restored.table()->horizontalHeader();
        QVERIFY2(!header->highlightSections(),
                 "Restoring the saved header must not reactivate bold selected-column labels.");
        QCOMPARE(header->font().weight(), QFont::Normal);
        QCOMPARE(header->sectionSize(0), 430);

        restored.applySync(payload(R"JSON({
          "full_update": true,
          "torrents": {
            "aa": {"name":"Album 2","state":"uploading","progress":1},
            "bb": {"name":"Album 10","state":"uploading","progress":1}
          }
        })JSON"));
        restored.show();
        auto *table = restored.table();
        QTRY_VERIFY(table->isVisible());

        const QRect selectedRect = table->visualRect(table->model()->index(0, 0));
        QVERIFY(selectedRect.isValid());
        QTest::mouseClick(table->viewport(), Qt::LeftButton,
                          Qt::NoModifier, selectedRect.center());
        QVERIFY(table->selectionModel()->hasSelection());
        table->sortByColumn(0, Qt::DescendingOrder);
        QVERIFY(!header->highlightSections());
        QCOMPARE(header->font().weight(), QFont::Normal);

        // Re-saving must migrate the old preference so reconnects/restarts
        // cannot resurrect the original problem, even for this same profile.
        restored.saveLayout(settings, profile);
        ServerPane reopened;
        reopened.restoreLayout(settings, profile);
        QVERIFY(!reopened.table()->horizontalHeader()->highlightSections());
        QCOMPARE(reopened.table()->horizontalHeader()->sectionSize(0), 430);
    }

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

    void nameSortingMatchesNativeCaseInsensitiveNaturalOrdering()
    {
        ServerPane pane;
        pane.applySync(payload(R"JSON({
          "full_update":true,
          "torrents":{
            "a":{"name":"Album 10","state":"uploading","progress":1},
            "b":{"name":"album 2","state":"uploading","progress":1},
            "c":{"name":"Apple","state":"uploading","progress":1},
            "d":{"name":"banana","state":"uploading","progress":1},
            "e":{"name":"BANJO","state":"uploading","progress":1},
            "f":{"name":"Track 10","state":"uploading","progress":1},
            "g":{"name":"track 2","state":"uploading","progress":1},
            "h":{"name":"(2008) Time Flies","state":"uploading","progress":1},
            "i":{"name":"[2010] Concert","state":"uploading","progress":1},
            "j":{"name":"Album 02","state":"uploading","progress":1},
            "k":{"name":"Track 999999999999999999999","state":"uploading","progress":1}
          }
        })JSON"));
        pane.table()->sortByColumn(0, Qt::AscendingOrder);
        QStringList names;
        for (int i = 0; i < pane.proxy()->rowCount(); ++i)
            names.append(pane.proxy()->index(i, 0).data(Qt::UserRole).toString());
        // ( and [ must appear contiguously, without any alphabetic
        // names between them, irrespective of host platform locale.
        QCOMPARE(names.size(), 11);
        QVERIFY(names.at(0).startsWith(QLatin1Char('('))
             || names.at(0).startsWith(QLatin1Char('[')));
        QVERIFY(names.at(1).startsWith(QLatin1Char('('))
             || names.at(1).startsWith(QLatin1Char('[')));
        QCOMPARE(names.mid(2), (QStringList{
            QStringLiteral("album 2"), QStringLiteral("Album 02"),
            QStringLiteral("Album 10"),
            QStringLiteral("Apple"), QStringLiteral("banana"),
            QStringLiteral("BANJO"), QStringLiteral("track 2"),
            QStringLiteral("Track 10"),
            QStringLiteral("Track 999999999999999999999")
        }));

        pane.table()->sortByColumn(0, Qt::DescendingOrder);
        QStringList reversed;
        for (int i = 0; i < pane.proxy()->rowCount(); ++i)
            reversed.append(pane.proxy()->index(i, 0).data(Qt::UserRole).toString());
        QStringList expected = names;
        std::reverse(expected.begin(), expected.end());
        QCOMPARE(reversed, expected);

        // Only Name gets natural text sorting; other columns keep numeric
        // Qt::UserRole comparisons, not lexicographic formatted strings.
        pane.table()->sortByColumn(1, Qt::AscendingOrder);
        QCOMPARE(pane.proxy()->sortColumn(), 1);
    }

    void queueStatesRemainInNativeSidebarFilters()
    {
        ServerPane pane;
        pane.resize(1300, 840);
        pane.show();
        QTRY_VERIFY(pane.isVisible());
        pane.applySync(payload(R"JSON({
          "full_update":true,
          "torrents":{
            "a":{"name":"Idle seed","state":"stalledUP","progress":1},
            "b":{"name":"Queued seed","state":"queuedUP","progress":1},
            "c":{"name":"Checking seed","state":"checkingUP","progress":1},
            "d":{"name":"Stopped seed","state":"stoppedUP","progress":1},
            "e":{"name":"Queued download","state":"queuedDL","progress":0.5},
            "f":{"name":"Checking download","state":"checkingDL","progress":0.5},
            "g":{"name":"Stopped download","state":"stoppedDL","progress":0.5},
            "h":{"name":"Forced metadata","state":"forcedMetaDL","progress":0},
            "i":{"name":"Active seed","state":"uploading","progress":1,"upspeed":4096},
            "j":{"name":"Moving torrent","state":"moving","progress":1},
            "k":{"name":"Legacy paused download","state":"pausedDL","progress":0.2},
            "l":{"name":"Legacy paused seed","state":"pausedUP","progress":1},
            "m":{"name":"Checking resume data","state":"checkingResumeData","progress":1},
            "n":{"name":"Active download","state":"downloading","progress":0.2,"dlspeed":2048}
          }
        })JSON"));
        auto *status = list(pane, "filterList");
        QVERIFY(status);
        const auto check = [&](const QString &key, const QString &label, const QStringList &hashes) {
            const int index = row(status, key);
            QVERIFY(index >= 0);
            QCOMPARE(status->item(index)->text(),
                QStringLiteral("%1 (%2)").arg(label).arg(hashes.size()));
            click(status, key);
            QTRY_COMPARE(visible(pane), hashes);
        };
        check("seeding", "Seeding", {"a","b","c","i"});
        check("downloading", "Downloading", {"e","f","g","h","k","n"});
        check("running", "Running", {"a","b","c","e","f","h","i","j","m","n"});
        check("stopped", "Stopped", {"d","g","k","l"});
        check("active", "Active", {"i","n"});
        check("inactive", "Inactive", {"a","b","c","d","e","f","g","h","j","k","l","m"});
        check("stalledUP", "Stalled Uploading", {"a"});
        check("checking", "Checking", {"c","f","m"});

        // Queueing an idle seed must not remove it from the Seeding
        // filter; stopping it must. Verify counts and the selected table
        // both react to ordinary incremental synchronization.
        check("seeding", "Seeding", {"a","b","c","i"});
        pane.applySync(payload(R"JSON({"torrents":{"a":{"state":"queuedUP"}}})JSON"));
        QTRY_COMPARE(visible(pane), (QStringList{"a","b","c","i"}));
        QCOMPARE(status->item(row(status,"seeding"))->text(), QStringLiteral("Seeding (4)"));
        QCOMPARE(status->item(row(status,"stalledUP"))->text(), QStringLiteral("Stalled Uploading (0)"));
        pane.applySync(payload(R"JSON({"torrents":{"a":{"state":"stoppedUP"}}})JSON"));
        QTRY_COMPARE(visible(pane), (QStringList{"b","c","i"}));
        QCOMPARE(status->item(row(status,"seeding"))->text(), QStringLiteral("Seeding (3)"));
        check("running", "Running", {"b","c","e","f","h","i","j","m","n"});
    }

    void combineStatusCategoryTagTrackerAndSearch();
    void independentServerTabsKeepIndependentSidebarStates();
    void locationTreeFiltersTogetherWithNativeSidebarAndPersists();
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

void SidebarWorkflowTest::locationTreeFiltersTogetherWithNativeSidebarAndPersists()
{
    const auto data = payload(R"JSON({
      "full_update":true,
      "torrents":{
        "a":{"name":"Alpha","state":"downloading","progress":0.4,
             "category":"Music","tags":"FLAC","save_path":"/share/Public/Music","size":1024},
        "b":{"name":"Beta","state":"uploading","progress":1.0,
             "category":"Music","tags":"FLAC","save_path":"/share/Public/Music/Country","size":2048},
        "c":{"name":"Charlie","state":"uploading","progress":1.0,
             "category":"Music","tags":"MP3","save_path":"/share/Public/Music/Country","size":4096},
        "d":{"name":"Delta","state":"downloading","progress":0.2,
             "category":"Books","tags":"PDF","save_path":"/share/Public/Books","size":8192},
        "e":{"name":"Echo","state":"uploading","progress":1.0,
             "category":"Other","tags":"FLAC","save_path":"/share/Public","size":512},
        "f":{"name":"Foxtrot","state":"uploading","progress":1.0,
             "category":"Music","tags":"FLAC","save_path":"/share/CACHEDEV1_DATA/Public","size":256},
        "g":{"name":"Golf","state":"stoppedUP","progress":1.0,
             "category":"Other","tags":"","size":128}
      }
    })JSON");

    ServerPane pane;
    pane.resize(1350, 1000);
    pane.show();
    QTRY_VERIFY(pane.isVisible());
    pane.applySync(data);
    auto *tree = pane.sidebar()->findChild<QTreeWidget *>(
        QStringLiteral("directoryFilterTree"));
    auto *status = list(pane, "filterList");
    auto *categories = list(pane, "categoryFilterList");
    auto *tags = list(pane, "tagFilterList");
    auto *trackers = list(pane, "trackerFilterList");
    QVERIFY(tree && status && categories && tags && trackers);
    QVERIFY(pane.sidebar()->findChild<QWidget *>(
        QStringLiteral("directoryFilterSection")));
    QVERIFY(tree->header()->isHidden());
    QCOMPARE(tree->columnCount(), 2);
    QCOMPARE(tree->verticalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
    QCOMPARE(tree->horizontalScrollBarPolicy(), Qt::ScrollBarAsNeeded);
    QVERIFY(!tree->header()->stretchLastSection());
    QCOMPARE(visible(pane), (QStringList{"a","b","c","d","e","f","g"}));

    const auto locate = [](QTreeWidget *view, const QString &path)
        -> QTreeWidgetItem * {
        for (QTreeWidgetItemIterator it(view); *it; ++it)
            if ((*it)->data(0, Qt::UserRole).toString() == path)
                return *it;
        return nullptr;
    };
    const auto choose = [&locate](ServerPane &server, const QString &path) {
        auto *view = server.sidebar()->findChild<QTreeWidget *>(
            QStringLiteral("directoryFilterTree"));
        QVERIFY(view);
        auto *node = locate(view, path);
        QVERIFY2(node, qPrintable(QStringLiteral("Missing directory: ") + path));
        for (auto *ancestor = node->parent(); ancestor; ancestor = ancestor->parent())
            ancestor->setExpanded(true);
        server.sidebar()->ensureWidgetVisible(view);
        view->scrollToItem(node);
        const QRect bounds = view->visualItemRect(node);
        QVERIFY(bounds.isValid());
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier,
                          bounds.center());
        QCOMPARE(view->currentItem(), node);
    };

    auto *music = locate(tree, QStringLiteral("/share/Public/Music"));
    auto *publicDir = locate(tree, QStringLiteral("/share/Public"));
    auto *alias = locate(tree, QStringLiteral("/share/CACHEDEV1_DATA/Public"));
    QVERIFY(music && publicDir && alias);
    QCOMPARE(music->text(0), QStringLiteral("Music (1)"));
    QCOMPARE(publicDir->text(0), QStringLiteral("Public (1)"));
    QCOMPARE(tree->topLevelItem(0)->text(0), QStringLiteral("All (7)"));
    QVERIFY(publicDir->isExpanded()); // Deep paths are visible on first load.
    QVERIFY(music->isExpanded());
    QCOMPARE(alias->text(0), QStringLiteral("Public (1)"));
    QVERIFY(!music->text(1).isEmpty()); // Size in right-hand column.
    QCOMPARE(locate(tree, QStringLiteral("/share/Public/Music/Country"))
                 ->text(0), QStringLiteral("Country (2)"));

    // With no other filters, exact directory sees only one save_path.
    choose(pane, QStringLiteral("/share/Public/Music"));
    QCOMPARE(visible(pane), (QStringList{"a"}));

    // This right-click is on the actual directory widget, and invokes
    // a visible native context menu rather than setting an internal flag.
    QString menuError;
    QTimer::singleShot(0, tree, [&]() {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu || menu->objectName() != QLatin1String("directoryFilterOptions"))
        {
            menuError = QStringLiteral("Directories context menu did not open");
            if (menu) menu->close();
            return;
        }
        QAction *toggle = nullptr;
        for (auto *action : menu->actions())
            if (action->objectName() == QLatin1String("directoryIncludeSubfolders"))
                toggle = action;
        if (!toggle || !toggle->isCheckable() || toggle->isChecked())
            menuError = QStringLiteral("Include subfolders should default off");
        else
            toggle->trigger();
        menu->close();
    });
    // Offscreen QtTest cannot reproduce the window manager's context-menu
    // event reliably. Invoke the native view's documented right-click
    // signal: the actual menu and QAction callback are then exercised.
    const QPoint point = tree->visualItemRect(music).center();
    QVERIFY(QMetaObject::invokeMethod(tree, "customContextMenuRequested",
                                     Qt::DirectConnection, Q_ARG(QPoint, point)));
    QVERIFY2(menuError.isEmpty(), qPrintable(menuError));
    QCOMPARE(music->text(0), QStringLiteral("Music (3)"));
    QCOMPARE(publicDir->text(0), QStringLiteral("Public (5)"));
    QCOMPARE(alias->text(0), QStringLiteral("Public (1)"));
    QCOMPARE(visible(pane), (QStringList{"a","b","c"}));

    // AND across all sections. Counts are global per server and must not
    // disappear or shrink when other filters are selected.
    click(categories, "category:Music");
    click(tags, "tag:FLAC");
    click(status, "seeding");
    QCOMPARE(visible(pane), (QStringList{"b"}));
    QCOMPARE(music->text(0), QStringLiteral("Music (3)"));
    pane.setSearchTerm(QStringLiteral("Alpha"));
    QCOMPARE(visible(pane), QStringList());
    pane.setSearchTerm(QStringLiteral("Beta"));
    QCOMPARE(visible(pane), (QStringList{"b"}));
    pane.setSearchTerm(QString());
    click(status, "all");

    // Path prefixes must be component-aware; never conflate the symlink
    // /share/Public with /share/CACHEDEV1_DATA/Public.
    choose(pane, QStringLiteral("/share/CACHEDEV1_DATA/Public"));
    QCOMPARE(visible(pane), (QStringList{"f"}));
    choose(pane, QString()); // "All" directory clears only this condition.
    QCOMPARE(visible(pane), (QStringList{"a","b","f"}));

    // Selection, filtering mode, and tree expansion persist per server.
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    QSettings settings(temp.filePath(QStringLiteral("profile.ini")), QSettings::IniFormat);
    choose(pane, QStringLiteral("/share/Public/Music"));
    music->setExpanded(true);
    pane.saveLayout(settings, QStringLiteral("QNAP-A"));
    settings.sync();
    ServerPane restored;
    restored.resize(1350, 1000);
    restored.restoreLayout(settings, QStringLiteral("QNAP-A"));
    restored.applySync(data);
    restored.show();
    auto *restoredTree = restored.sidebar()->findChild<QTreeWidget *>(
        QStringLiteral("directoryFilterTree"));
    QVERIFY(restoredTree);
    auto *restoredMusic = locate(restoredTree, QStringLiteral("/share/Public/Music"));
    QVERIFY(restoredMusic);
    QCOMPARE(restoredMusic->text(0), QStringLiteral("Music (3)"));
    QCOMPARE(restoredTree->currentItem(), restoredMusic);
    QVERIFY(restoredMusic->isExpanded());

    ServerPane second;
    second.resize(900, 800);
    second.applySync(payload(R"({"full_update":true,"torrents":{
       "z":{"name":"Other server","save_path":"/mnt/Public/Music","size":512}
    }})"));
    auto *secondTree = second.sidebar()->findChild<QTreeWidget *>(
        QStringLiteral("directoryFilterTree"));
    QVERIFY(secondTree);
    QVERIFY(locate(secondTree, QStringLiteral("/mnt/Public/Music")));
    QVERIFY(!locate(secondTree, QStringLiteral("/share/Public/Music")));
    QCOMPARE(visible(second), (QStringList{"z"}));
    QCOMPARE(restoredMusic->text(0), QStringLiteral("Music (3)"));
}

// Preserve QtTest's native QApplication and test setup for this suite.
// The shared dispatcher selects this entry point before Qt is initialized.
#define main runSidebarFilterWorkflow
QTEST_MAIN(SidebarWorkflowTest)
#undef main
#include "test_sidebar_filter_workflow.moc"
