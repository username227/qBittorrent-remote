// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise actual bottom-panel Qt widgets with realistic qBittorrent Web API
// replies. These tests verify displayed ordering after user column sorts.
#include "gui/TorrentDetailsWidget.h"

#include <QHeaderView>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTreeWidget>
#include <QtTest/QtTest>

namespace
{
QJsonArray fixtureArray(const QByteArray &json)
{
    return QJsonDocument::fromJson(json).array();
}

QTreeWidgetItem *childNamed(QTreeWidgetItem *parent, const QString &name)
{
    if (!parent) return nullptr;
    for (int i = 0; i < parent->childCount(); ++i)
        if (parent->child(i)->text(0) == name)
            return parent->child(i);
    return nullptr;
}

QTreeWidgetItem *topNamed(QTreeWidget *tree, const QString &name)
{
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        if (tree->topLevelItem(i)->text(0) == name)
            return tree->topLevelItem(i);
    return nullptr;
}

QStringList topNames(QTreeWidget *tree)
{
    QStringList names;
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        names.append(tree->topLevelItem(i)->text(0));
    return names;
}

QStringList childNames(QTreeWidgetItem *parent)
{
    QStringList names;
    for (int i = 0; i < parent->childCount(); ++i)
        names.append(parent->child(i)->text(0));
    return names;
}

QStringList tableValues(QTableWidget *table, int column)
{
    QStringList values;
    for (int i = 0; i < table->rowCount(); ++i)
        values.append(table->item(i, column)->text());
    return values;
}
}

class DetailsSortingWorkflowTest final : public QObject
{
    Q_OBJECT
private slots:
    void contentSortsNaturalNamesAndNestedFolders();
    void peersSortNumbersRatherThanFormattedPercentagesAndSpeeds();
    void trackersSortStatusAndNumericColumnsWithoutFlatteningEndpoints();
};

void DetailsSortingWorkflowTest::contentSortsNaturalNamesAndNestedFolders()
{
        TorrentDetailsWidget widget;
        widget.setSelection(QStringLiteral("test-hash"));
        widget.showTab(4);
        const auto files = fixtureArray(R"JSON([
          {"index":0,"name":"Disc 10/Track 10.flac","size":3000,"progress":0.25,"priority":6,"availability":2.3},
          {"index":1,"name":"Disc 10/track 2.flac","size":4000,"progress":0.75,"priority":1,"availability":1.2},
          {"index":2,"name":"Disc 2/Track 20.flac","size":9000,"progress":0.9,"priority":7,"availability":8},
          {"index":3,"name":"Disc 2/track 3.flac","size":7000,"progress":0.1,"priority":1,"availability":0.5},
          {"index":4,"name":"Disc 2/Live/track 10.flac","size":500,"progress":0.4,"priority":1,"availability":3},
          {"index":5,"name":"Disc 2/Live/track 2.flac","size":700,"progress":0.3,"priority":1,"availability":5},
          {"index":6,"name":"Album 10.flac","size":2048,"progress":0.25,"priority":1,"availability":0.75},
          {"index":7,"name":"album 2.flac","size":512,"progress":0.75,"priority":7,"availability":4.25},
          {"index":8,"name":"Album 1.flac","size":1024,"progress":0.5,"priority":6,"availability":1.25}
        ])JSON");
        widget.displayDetails(QStringLiteral("test-hash"), 4, files);
        auto *tree = widget.findChild<QTreeWidget *>(QStringLiteral("torrentContent"));
        QVERIFY(tree);
        QVERIFY(tree->isSortingEnabled());
        QCOMPARE(tree->sortColumn(), 0);

        tree->sortItems(0, Qt::AscendingOrder);
        const QStringList names = topNames(tree);
        QVERIFY(names.indexOf(QStringLiteral("Album 1.flac")) <
                names.indexOf(QStringLiteral("album 2.flac")));
        QVERIFY(names.indexOf(QStringLiteral("album 2.flac")) <
                names.indexOf(QStringLiteral("Album 10.flac")));
        QVERIFY(names.indexOf(QStringLiteral("Disc 2")) <
                names.indexOf(QStringLiteral("Disc 10")));

        auto *disc10 = topNamed(tree, QStringLiteral("Disc 10"));
        auto *disc2 = topNamed(tree, QStringLiteral("Disc 2"));
        QVERIFY(disc10 && disc2);
        QCOMPARE(childNames(disc10), (QStringList{
            QStringLiteral("track 2.flac"), QStringLiteral("Track 10.flac")
        }));
        auto *live = childNamed(disc2, QStringLiteral("Live"));
        QVERIFY(live);
        QCOMPARE(childNames(live), (QStringList{
            QStringLiteral("track 2.flac"), QStringLiteral("track 10.flac")
        }));

        // Real user size sort: entire folder totals, then each folder's
        // individual children, both sorted using raw byte counts.
        tree->sortItems(1, Qt::DescendingOrder);
        QCOMPARE(tree->topLevelItem(0)->text(0), QStringLiteral("Disc 2"));
        QCOMPARE(tree->topLevelItem(1)->text(0), QStringLiteral("Disc 10"));
        QCOMPARE(disc10->child(0)->text(0), QStringLiteral("track 2.flac"));

        // Progress and download priority must use their real values.
        tree->sortItems(2, Qt::AscendingOrder);
        QCOMPARE(disc10->child(0)->text(0), QStringLiteral("Track 10.flac"));
        tree->sortItems(3, Qt::DescendingOrder);
        QCOMPARE(disc10->child(0)->text(0), QStringLiteral("Track 10.flac"));
        tree->sortItems(4, Qt::DescendingOrder);
        QCOMPARE(disc10->child(0)->text(0), QStringLiteral("Track 10.flac"));
        tree->sortItems(5, Qt::AscendingOrder);
        QCOMPARE(disc10->child(0)->text(0), QStringLiteral("track 2.flac"));

        // User's chosen column/direction must survive a details refresh.
        widget.displayDetails(QStringLiteral("test-hash"), 4, files);
        QCOMPARE(tree->sortColumn(), 5);
        QCOMPARE(tree->header()->sortIndicatorOrder(), Qt::AscendingOrder);
        disc10 = topNamed(tree, QStringLiteral("Disc 10"));
        QVERIFY(disc10);
        QCOMPARE(disc10->child(0)->text(0), QStringLiteral("track 2.flac"));

        // Sorting must not change the real API file id attached to a checkbox.
        QSignalSpy prioritySpy(&widget, &TorrentDetailsWidget::filePriorityRequested);
        auto *track2 = childNamed(disc10, QStringLiteral("track 2.flac"));
        QVERIFY(track2);
        QCOMPARE(track2->data(0, Qt::UserRole).toInt(), 1);
        track2->setCheckState(0, Qt::Unchecked);
        QCOMPARE(prioritySpy.size(), 1);
        QCOMPARE(prioritySpy.at(0).at(1).value<QList<int>>(), QList<int>{1});
}

void DetailsSortingWorkflowTest::peersSortNumbersRatherThanFormattedPercentagesAndSpeeds()
{
        TorrentDetailsWidget widget;
        widget.setSelection(QStringLiteral("test-hash"));
        widget.showTab(2);
        const QJsonObject reply = QJsonDocument::fromJson(R"JSON({
          "peers": {
            "10.0.0.10:6881":{"country_code":"US","client":"Alpha","progress":0.1,"dl_speed":10240,"up_speed":1024,"flags":"U","connection":"TCP"},
            "10.0.0.2:6881":{"country_code":"CA","client":"Zeta","progress":0.9,"dl_speed":900,"up_speed":2048,"flags":"D","connection":"uTP"},
            "10.0.0.3:6881":{"country_code":"GB","client":"beta","progress":0.5,"dl_speed":512,"up_speed":8192,"flags":"DU","connection":"TCP"}
          }
        })JSON").object();
        widget.displayDetails(QStringLiteral("test-hash"), 2, reply);
        auto *table = widget.findChild<QTableWidget *>(QStringLiteral("torrentPeers"));
        QVERIFY(table);
        QVERIFY(table->isSortingEnabled());

        table->sortItems(0, Qt::AscendingOrder);
        QCOMPARE(tableValues(table, 0), (QStringList{
            QStringLiteral("10.0.0.2:6881"), QStringLiteral("10.0.0.3:6881"),
            QStringLiteral("10.0.0.10:6881")
        }));
        table->sortItems(3, Qt::AscendingOrder);
        QCOMPARE(tableValues(table, 3), (QStringList{
            QStringLiteral("10.0%"), QStringLiteral("50.0%"), QStringLiteral("90.0%")
        }));
        table->sortItems(4, Qt::AscendingOrder);
        QCOMPARE(tableValues(table, 0), (QStringList{
            QStringLiteral("10.0.0.3:6881"), QStringLiteral("10.0.0.2:6881"),
            QStringLiteral("10.0.0.10:6881")
        }));
        table->sortItems(5, Qt::DescendingOrder);
        QCOMPARE(table->item(0, 0)->text(), QStringLiteral("10.0.0.3:6881"));

        // Keep the selected sort across a live peer refresh.
        widget.displayDetails(QStringLiteral("test-hash"), 2, reply);
        QCOMPARE(table->horizontalHeader()->sortIndicatorSection(), 5);
        QCOMPARE(table->horizontalHeader()->sortIndicatorOrder(), Qt::DescendingOrder);
        QCOMPARE(table->item(0, 0)->text(), QStringLiteral("10.0.0.3:6881"));
}

void DetailsSortingWorkflowTest::trackersSortStatusAndNumericColumnsWithoutFlatteningEndpoints()
{
        TorrentDetailsWidget widget;
        widget.setSelection(QStringLiteral("test-hash"));
        widget.showTab(1);
        const auto reply = fixtureArray(R"JSON([
          {"url":"https://working.example/announce","tier":10,"status":2,"num_peers":10,"num_seeds":7,"num_leeches":5,"num_downloaded":90,
           "endpoints":[
             {"name":"server 10","status":4,"num_seeds":8},
             {"name":"server 2","status":2,"num_seeds":12}
           ]},
          {"url":"https://notworking.example/announce","tier":2,"status":4,"num_peers":2,"num_seeds":100,"num_leeches":50,"num_downloaded":5},
          {"url":"https://new.example/announce","tier":1,"status":1,"num_peers":30,"num_seeds":20,"num_leeches":2,"num_downloaded":20}
        ])JSON");
        widget.displayDetails(QStringLiteral("test-hash"), 1, reply);
        auto *tree = widget.findChild<QTreeWidget *>(QStringLiteral("torrentTrackers"));
        QVERIFY(tree);
        QVERIFY(tree->isSortingEnabled());

        // Most useful with large public-torrent tracker lists: one click on
        // Status should group Working / Not working / Not contacted yet.
        tree->sortItems(3, Qt::AscendingOrder);
        QCOMPARE(topNames(tree), (QStringList{
            QStringLiteral("https://new.example/announce"),
            QStringLiteral("https://notworking.example/announce"),
            QStringLiteral("https://working.example/announce")
        }));
        tree->sortItems(5, Qt::AscendingOrder);
        QCOMPARE(topNames(tree), (QStringList{
            QStringLiteral("https://working.example/announce"),
            QStringLiteral("https://new.example/announce"),
            QStringLiteral("https://notworking.example/announce")
        }));
        tree->sortItems(1, Qt::AscendingOrder);
        QCOMPARE(topNames(tree), (QStringList{
            QStringLiteral("https://new.example/announce"),
            QStringLiteral("https://notworking.example/announce"),
            QStringLiteral("https://working.example/announce")
        }));

        auto *working = topNamed(tree, QStringLiteral("https://working.example/announce"));
        QVERIFY(working);
        QCOMPARE(working->childCount(), 2);
        tree->sortItems(0, Qt::AscendingOrder);
        QCOMPARE(childNames(working), (QStringList{
            QStringLiteral("server 2"), QStringLiteral("server 10")
        }));
        working->setExpanded(true);
        tree->sortItems(3, Qt::DescendingOrder);
        // Status descending puts Working before Not working, regardless
        // of each endpoint's filename-like number.
        QCOMPARE(childNames(working), (QStringList{
            QStringLiteral("server 2"), QStringLiteral("server 10")
        }));
        widget.displayDetails(QStringLiteral("test-hash"), 1, reply);
        QCOMPARE(tree->sortColumn(), 3);
        QCOMPARE(tree->header()->sortIndicatorOrder(), Qt::DescendingOrder);
        working = topNamed(tree, QStringLiteral("https://working.example/announce"));
        QVERIFY(working);
        QVERIFY(working->isExpanded());
        QCOMPARE(childNames(working), (QStringList{
            QStringLiteral("server 2"), QStringLiteral("server 10")
        }));
}

// Preserve QtTest's native QApplication and test setup for this suite.
// The shared dispatcher selects this entry point before Qt is initialized.
#define main runDetailsSortingWorkflow
QTEST_MAIN(DetailsSortingWorkflowTest)
#undef main
#include "test_details_sorting_workflow.moc"
