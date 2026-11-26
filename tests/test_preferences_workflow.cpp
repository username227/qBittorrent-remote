// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/RemotePreferencesDialog.h"
#include "gui/MainWindow.h"
#include "api/PathMappingStore.h"
#include "api/ServerProfile.h"

#include <QAction>
#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QGuiApplication>
#include <QScreen>
#include <QHeaderView>
#include <QLineEdit>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTreeWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <limits>
#include <QScrollBar>
#include <QtTest/QtTest>

class PreferencesWorkflowTest final : public QObject
{
    Q_OBJECT
private slots:
    void pathMappingsArePerServerAndUseDirectoryBoundaries()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QSettings settings(tmp.filePath(QStringLiteral("preferences.ini")),
                           QSettings::IniFormat);
        Remote::PathMappingStore maps(settings);

        const QString local = QDir(tmp.path()).filePath(QStringLiteral("qnap"));
        const QString nested = QDir(local).filePath(QStringLiteral("music"));
        const QString destination = QStringLiteral("/share/CACHEDEV1_DATA/Downloads");
        const QList<Remote::PathMapping> first {
            {local, destination},
            {nested, QStringLiteral("/share/CACHEDEV1_DATA/Music")}
        };
        QString error;
        QVERIFY2(maps.replace(QStringLiteral("server-a"), first, &error),
                 qPrintable(error));
        QVERIFY2(maps.replace(QStringLiteral("server-b"),
                              {{local, QStringLiteral("/share/Other")}}, &error),
                 qPrintable(error));
        QCOMPARE(maps.mappings(QStringLiteral("server-a")).size(), 2);
        QCOMPARE(maps.mappings(QStringLiteral("server-b")).size(), 1);

        const QString file = QDir(nested).filePath(QStringLiteral("album/song.flac"));
        QCOMPARE(Remote::PathMappingStore::toRemote(file, first),
                 QStringLiteral("/share/CACHEDEV1_DATA/Music/album/song.flac"));
        QCOMPARE(Remote::PathMappingStore::toLocal(
                     QStringLiteral("/share/CACHEDEV1_DATA/Music/album/song.flac"), first),
                 QDir::cleanPath(QDir::fromNativeSeparators(file)));
        QVERIFY(Remote::PathMappingStore::toRemote(
            local + QStringLiteral("-elsewhere/file"), first).isEmpty());
        QVERIFY(Remote::PathMappingStore::toLocal(
            QStringLiteral("/share/CACHEDEV1_DATA/DownloadsExtra/file"), first).isEmpty());
        QVERIFY(Remote::PathMappingStore::toRemote(
            QStringLiteral("/some/unmapped/dir"), first).isEmpty());

        QVERIFY(!maps.replace(QStringLiteral("server-a"),
                             {{local, destination}, {nested, destination + QStringLiteral("/")}},
                             &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!maps.replace(QStringLiteral("server-a"),
                             {{QStringLiteral("not/absolute"), destination}}, &error));
        QVERIFY(!maps.replace(QStringLiteral("server-a"),
                             {{local, QStringLiteral("relative/remote")}}, &error));
        QCOMPARE(maps.mappings(QStringLiteral("server-a")).size(), 2);

        settings.sync();
        QSettings reread(tmp.filePath(QStringLiteral("preferences.ini")),
                         QSettings::IniFormat);
        QCOMPARE(Remote::PathMappingStore(reread).mappings(
                     QStringLiteral("server-a")).size(), 2);

        maps.remove(QStringLiteral("server-a"));
        QVERIFY(maps.mappings(QStringLiteral("server-a")).isEmpty());
        QCOMPARE(maps.mappings(QStringLiteral("server-b")).size(), 1);
    }

#ifdef Q_OS_WIN
    void windowsDriveRootMappingPreservesAbsoluteRoot()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QSettings settings(tmp.filePath(QStringLiteral("windows.ini")),
                           QSettings::IniFormat);
        Remote::PathMappingStore maps(settings);
        const QString profile = QStringLiteral("Windows drive root");
        const QString remote = QStringLiteral("/share/CACHEDEV1_DATA/Public");
        QString error;
        QVERIFY2(maps.replace(profile, {{QStringLiteral("Z:\\"), remote}}, &error),
                 qPrintable(error));
        const auto saved = maps.mappings(profile);
        QCOMPARE(saved.size(), 1);
        QCOMPARE(saved.first().localRoot, QStringLiteral("Z:/"));
        QCOMPARE(Remote::PathMappingStore::toRemote(
            QStringLiteral("Z:/"), saved), remote);
        QCOMPARE(Remote::PathMappingStore::toRemote(
            QStringLiteral("Z:/Music/track.flac"), saved),
            remote + QStringLiteral("/Music/track.flac"));
        QCOMPARE(Remote::PathMappingStore::toLocal(
            remote, saved), QStringLiteral("Z:/"));
        QCOMPARE(Remote::PathMappingStore::toLocal(
            remote + QStringLiteral("/Music/track.flac"), saved),
            QStringLiteral("Z:/Music/track.flac"));

        // Older builds stored the root as drive-relative "Z:". The next
        // read should repair it without requiring the user to re-enter it.
        QString key;
        for (const QString &candidate : settings.allKeys())
            if (candidate.startsWith(QStringLiteral("RemotePaths/Servers/")))
                key = candidate;
        QVERIFY(!key.isEmpty());
        QVariantList rows = settings.value(key).toList();
        QCOMPARE(rows.size(), 1);
        QVariantMap row = rows.at(0).toMap();
        row[QStringLiteral("local")] = QStringLiteral("Z:");
        rows[0] = row;
        settings.setValue(key, rows);

        const auto repaired = maps.mappings(profile);
        QCOMPARE(repaired.size(), 1);
        QCOMPARE(repaired.first().localRoot, QStringLiteral("Z:/"));
        QCOMPARE(Remote::PathMappingStore::toRemote(
            QStringLiteral("Z:/Music"), repaired),
            remote + QStringLiteral("/Music"));

        // Replacing the repaired record persists the absolute root again.
        QVERIFY2(maps.replace(profile, repaired, &error), qPrintable(error));
        QCOMPARE(settings.value(key).toList().first().toMap()
                     .value(QStringLiteral("local")).toString(),
                 QStringLiteral("Z:/"));
    }

    void windowsPreferencesDisplayLocalDriveRootNatively()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QSettings settings(tmp.filePath(QStringLiteral("windows-ui.ini")),
                           QSettings::IniFormat);
        Remote::ProfileStore profiles(settings);
        Remote::ServerProfile server{
            {}, QStringLiteral("Windows NAS"), QStringLiteral("http://127.0.0.1:6364"),
            QStringLiteral("demo"), false, false
        };
        QVERIFY(profiles.save(server));
        Remote::PathMappingStore maps(settings);
        QString error;
        QVERIFY2(maps.replace(server.id, {{
            QStringLiteral("Z:/"), QStringLiteral("/share/CACHEDEV1_DATA/Public")}}, &error),
            qPrintable(error));
        RemotePreferencesDialog dialog(settings, server.id);
        auto *table = dialog.findChild<QTableWidget *>(
            QStringLiteral("remotePathMappings"));
        QVERIFY(table);
        QCOMPARE(table->rowCount(), 1);
        QCOMPARE(table->item(0, 0)->text(), QStringLiteral("Z:\\"));
        QCOMPARE(table->item(0, 1)->text(),
                 QStringLiteral("/share/CACHEDEV1_DATA/Public"));
        // The Edit dialog must display a native drive root too.
        table->selectRow(0);
        bool opened = false;
        QTimer::singleShot(0, [&]() {
            auto *modal = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!modal || modal->objectName() != QStringLiteral("mappingEditor"))
                return;
            auto *local = modal->findChild<QLineEdit *>(
                QStringLiteral("mappingLocalPath"));
            auto *remote = modal->findChild<QLineEdit *>(
                QStringLiteral("mappingRemotePath"));
            if (local && remote)
            {
                opened = true;
                QCOMPARE(local->text(), QStringLiteral("Z:\\"));
                QCOMPARE(remote->text(), QStringLiteral("/share/CACHEDEV1_DATA/Public"));
            }
            modal->reject();
        });
        auto *edit = dialog.findChild<QPushButton *>(
            QStringLiteral("editPathMapping"));
        QVERIFY(edit && edit->isEnabled());
        edit->click();
        QVERIFY(opened);
    }
#endif

    void changingSavedServerEndpointInvalidatesOldFilesystemMappings()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QSettings settings(tmp.filePath(QStringLiteral("preferences.ini")),
                           QSettings::IniFormat);
        Remote::ProfileStore profiles(settings);
        Remote::ServerProfile server{
            {}, QStringLiteral("QNAP"), QStringLiteral("https://127.0.0.1:6363"),
            QStringLiteral("demo"), false, false
        };
        QVERIFY(profiles.save(server));
        Remote::PathMappingStore mappings(settings);
        const QString local = QDir(tmp.path()).filePath(QStringLiteral("mounted"));
        QVERIFY(mappings.replace(server.id,
             {{local, QStringLiteral("/share/CACHEDEV1_DATA/Downloads")}}));
        QCOMPARE(mappings.mappings(server.id).size(), 1);

        server.name = QStringLiteral("Renamed QNAP");
        QVERIFY(profiles.save(server));
        QCOMPARE(mappings.mappings(server.id).size(), 1);

        server.url = QStringLiteral("https://127.0.0.1:7777");
        QVERIFY(profiles.save(server));
        QVERIFY(mappings.mappings(server.id).isEmpty());
    }

    void nativeOptionsContainsAllPagesAndDisablesUnimplementedControls()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QSettings settings(tmp.filePath(QStringLiteral("preferences.ini")),
                           QSettings::IniFormat);
        RemotePreferencesDialog dialog(settings);
        dialog.show();
        QTRY_VERIFY(dialog.isVisible());

        auto *navigation = dialog.findChild<QListWidget *>(
            QStringLiteral("preferencesSections"));
        auto *pages = dialog.findChild<QStackedWidget *>(
            QStringLiteral("preferencesPages"));
        QVERIFY(navigation && pages);
        QCOMPARE(navigation->count(), 10);
        QCOMPARE(pages->count(), 10);
        QCOMPARE(navigation->iconSize(), QSize(28, 28));
        // Match upstream's *actual* blue speedometer resource, rather
        // than the green/blue bar-chart icon previously shown for Speed.
        const auto nativeSpeedometer =
            QIcon(QStringLiteral(":/icons/speedometer.svg")).pixmap(28, 28).toImage();
        const auto speedIcon = navigation->item(4)->icon().pixmap(28, 28).toImage();
        QVERIFY(!nativeSpeedometer.isNull());
        QCOMPARE(speedIcon, nativeSpeedometer);
        const QStringList nativePages {
            QStringLiteral("Servers"), QStringLiteral("Behavior"),
            QStringLiteral("Downloads"), QStringLiteral("Connection"),
            QStringLiteral("Speed"), QStringLiteral("BitTorrent"),
            QStringLiteral("Search"), QStringLiteral("RSS"),
            QStringLiteral("WebUI"), QStringLiteral("Advanced")
        };
        QCOMPARE(navigation->currentRow(), 0);
        QVERIFY(dialog.height() >= 480);
        // Match upstream's item sizing: every icon tile spans the whole
        // navigation viewport, rather than being clipped to a fixed grid.
        // This is what actually centers icons, not setTextAlignment alone.
        for (int i = 0; i < navigation->count(); ++i)
        {
            QCOMPARE(navigation->item(i)->sizeHint().width(),
                     std::numeric_limits<int>::max());
            const QRect rect = navigation->visualItemRect(navigation->item(i));
            QVERIFY2(rect.width() >= navigation->viewport()->width() - 5,
                     "Native navigation tiles should use the full sidebar width.");
        }
        // At the preferred initial size the tenth page stays visible.
        QTRY_VERIFY(navigation->verticalScrollBar()->maximum() == 0);
        QVERIFY(navigation->visualItemRect(navigation->item(0)).top() >= 0);
        QVERIFY(navigation->visualItemRect(navigation->item(9)).bottom()
                <= navigation->viewport()->height());
        for (int i = 0; i < nativePages.size(); ++i)
        {
            QCOMPARE(navigation->item(i)->text(), nativePages.at(i));
            navigation->setCurrentRow(i);
            QCOMPARE(pages->currentIndex(), i);
            QVERIFY(!navigation->item(i)->icon().isNull());
            QVERIFY(navigation->item(i)->textAlignment() & Qt::AlignHCenter);
        }

        // The upstream Advanced section is dynamically assembled in C++,
        // rather than present in its Designer XML. Keep all named rows.
        auto *advanced = dialog.findChild<QTableWidget *>(
            QStringLiteral("advancedSettingsTable"));
        QVERIFY(advanced);
        QCOMPARE(advanced->rowCount(), 92);
        QVERIFY(!advanced->horizontalHeader()->highlightSections());
        QCOMPARE(advanced->item(0, 0)->text(), QStringLiteral("qBittorrent Section"));
        QCOMPARE(advanced->item(35, 0)->text(), QStringLiteral("libtorrent Section"));
        QCOMPARE(advanced->item(advanced->rowCount() - 1, 0)->data(Qt::UserRole).toString(), QStringLiteral("I2P outbound length variance"));
        // An embedded label paints over the table delegate. Any nonempty
        // DisplayRole would draw the setting twice through that widget.
        for (int r = 0; r < advanced->rowCount(); ++r)
        {
            if (r == 0 || r == 35) continue;
            QVERIFY(advanced->item(r, 0));
            QVERIFY(advanced->item(r, 0)->data(Qt::DisplayRole).toString().isEmpty());
            QVERIFY(!advanced->item(r, 0)->data(Qt::UserRole).toString().isEmpty());
            auto *label = qobject_cast<QLabel *>(advanced->cellWidget(r, 0));
            QVERIFY(label);
            QCOMPARE(label->font().weight(), QFont::Normal);
        }
        QVERIFY(qobject_cast<QLabel *>(advanced->cellWidget(0, 1))->openExternalLinks());
        QVERIFY(qobject_cast<QLabel *>(advanced->cellWidget(35, 1))->openExternalLinks());
        auto *threads = advanced->findChild<QSpinBox *>("advanced_ASYNC_IO_THREADS");
        auto *storage = advanced->findChild<QComboBox *>("advanced_RESUME_DATA_STORAGE");
        auto *recheck = advanced->findChild<QCheckBox *>("advanced_CONFIRM_RECHECK_TORRENT");
        auto *tags = advanced->findChild<QCheckBox *>("advanced_CONFIRM_REMOVE_ALL_TAGS");
        auto *tracker = advanced->findChild<QCheckBox *>("advanced_CONFIRM_REMOVE_TRACKER_FROM_ALL_TORRENTS");
        QVERIFY(threads && storage && recheck && tags && tracker);
        QVERIFY(!threads->isEnabled() && !storage->isEnabled());
        QCOMPARE(threads->minimum(), 1);
        QCOMPARE(threads->maximum(), 1024);
        QCOMPARE(storage->currentIndex(), -1);
        QCOMPARE(advanced->textElideMode(), Qt::ElideNone);
        QVERIFY(recheck->isEnabled() && tags->isEnabled() && tracker->isEnabled());
        QVERIFY(!recheck->isChecked() && tags->isChecked() && tracker->isChecked());
        // Both columns must remain visible without horizontal scrolling at
        // ordinary dialog widths, even with long linked setting names.
        for (int width : {650, 820, 1000})
        {
            dialog.resize(width, dialog.height());
            QTRY_VERIFY(advanced->columnViewportPosition(1) >= 120);
            QTRY_VERIFY(advanced->columnViewportPosition(1) + advanced->columnWidth(1)
                        <= advanced->viewport()->width() + 1);
            QCOMPARE(advanced->horizontalScrollBar()->maximum(), 0);
            auto *longLabel = advanced->findChild<QLabel *>("advancedLabel_PEER_DSCP");
            QVERIFY(longLabel && longLabel->wordWrap());
            // Use the stable cell widget rather than relying on the current
            // vertical scroll position for a row that may be offscreen.
            int labelRow = -1;
            for (int r = 0; r < advanced->rowCount(); ++r)
                if (advanced->cellWidget(r, 0) == longLabel) labelRow = r;
            QVERIFY(labelRow >= 0);
            QTRY_VERIFY(advanced->rowHeight(labelRow) >= longLabel->heightForWidth(advanced->columnWidth(0) - 2));
        }
        auto *linked = advanced->findChild<QLabel *>("advancedLabel_ASYNC_IO_THREADS");
        auto *plain = advanced->findChild<QLabel *>("advancedLabel_SAVE_RESUME_DATA_INTERVAL");
        auto *localLabel = advanced->findChild<QLabel *>("advancedLabel_CONFIRM_RECHECK_TORRENT");
        QVERIFY(linked && plain && localLabel);
        QCOMPARE(linked->font(), plain->font());
        QCOMPARE(linked->font(), localLabel->font());
        QVERIFY(linked->text().contains(linked->palette().color(QPalette::Disabled, QPalette::Text).name()));
        QVERIFY(plain->text().contains(plain->palette().color(QPalette::Disabled, QPalette::Text).name()));
        QVERIFY(localLabel->text().contains(localLabel->palette().color(QPalette::Active, QPalette::Text).name()));
        QVERIFY(linked->openExternalLinks() && linked->text().contains("href="));
        dialog.resize(820, dialog.height());
        auto *applyAdvanced = dialog.findChild<QPushButton *>("preferencesApply");
        QVERIFY(applyAdvanced);
        recheck->setChecked(true);
        tags->setChecked(false);
        tracker->setChecked(false);
        QVERIFY(applyAdvanced->isEnabled());
        applyAdvanced->click();
        QVERIFY(settings.value("Preferences/Advanced/ConfirmTorrentRecheck").toBool());
        QVERIFY(!settings.value("Preferences/Advanced/ConfirmRemoveAllTags", true).toBool());
        QVERIFY(!settings.value("Preferences/Advanced/ConfirmRemoveTrackerFromAllTorrents", true).toBool());
        recheck->setChecked(false);
        QVERIFY(applyAdvanced->isEnabled());
        recheck->setChecked(true);
        QVERIFY(!applyAdvanced->isEnabled());

        auto *confirm = dialog.findChild<QCheckBox *>(
            QStringLiteral("confirmDeletion"));
        auto *limit = dialog.findChild<QSpinBox *>(
            QStringLiteral("spinDownloadLimit"));
        QVERIFY(confirm && limit);
        QVERIFY(confirm->isEnabled());
        QVERIFY(!limit->isEnabled());
        // Behavior's local controls are functional; obsolete remote-engine
        // controls have been removed, not left as disabled placeholders.
        QVERIFY(!dialog.findChild<QCheckBox *>(QStringLiteral("checkShowSplash")));
        QVERIFY(!dialog.findChild<QCheckBox *>(QStringLiteral("checkProgramAutoExitConfirm")));
        QVERIFY(!dialog.findChild<QCheckBox *>(QStringLiteral("checkProgramUpdates")));
        QVERIFY(!dialog.findChild<QCheckBox *>(
            QStringLiteral("checkPreventFromSuspendWhenDownloading")));
        auto *downloadAction = dialog.findChild<QComboBox *>(
            QStringLiteral("actionTorrentDlOnDblClBox"));
        QVERIFY(downloadAction);
        QVERIFY(downloadAction->isEnabled());
        QVERIFY(downloadAction->view()->isEnabled());
        // The original native dialog has no persistent warning strip:
        // unsupported control tooltips communicate the same limitation.
        QVERIFY(!dialog.findChild<QLabel *>(QStringLiteral("unimplementedNotice")));
        QVERIFY(navigation->toolTip().contains(QStringLiteral("remain disabled")));
        dialog.resize(765, 550); // keep the native dialog freely resizable
        QCoreApplication::processEvents();
        QVERIFY(dialog.height() < 660);

        auto *selector = dialog.findChild<QComboBox *>(
            QStringLiteral("remotePathsServer"));
        auto *add = dialog.findChild<QPushButton *>(
            QStringLiteral("addPathMapping"));
        auto *apply = dialog.findChild<QPushButton *>(
            QStringLiteral("preferencesApply"));
        QVERIFY(selector && add && apply);
        auto *mappings = dialog.findChild<QTableWidget *>(
            QStringLiteral("remotePathMappings"));
        QVERIFY(mappings);
        QVERIFY(!mappings->horizontalHeader()->highlightSections());
        QCOMPARE(selector->count(), 0);
        QVERIFY(!add->isEnabled());
        QVERIFY(!apply->isEnabled());
        dialog.reject();
        QCOMPARE(settings.value(QStringLiteral("Preferences/LastPage")).toInt(), 9);
    }

    void migrationKeepsLastSelectedPageWhenServersMoveFirst()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QSettings settings(tmp.filePath(QStringLiteral("preferences.ini")),
                           QSettings::IniFormat);

        // Earlier PR #9 builds saved Remote Paths as the last page.
        settings.setValue(QStringLiteral("Preferences/LastPage"), 9);
        {
            RemotePreferencesDialog dialog(settings);
            auto *nav = dialog.findChild<QListWidget *>(
                QStringLiteral("preferencesSections"));
            QVERIFY(nav);
            QCOMPARE(nav->currentRow(), 0); // now Servers
            dialog.reject();
        }
        QCOMPARE(settings.value(QStringLiteral("Preferences/LastPage")).toInt(), 0);
        QCOMPARE(settings.value(QStringLiteral("Preferences/NavOrderVersion")).toInt(), 2);
        {
            RemotePreferencesDialog dialog(settings);
            auto *nav = dialog.findChild<QListWidget *>(
                QStringLiteral("preferencesSections"));
            QVERIFY(nav);
            QCOMPARE(nav->currentRow(), 0); // idempotent across restarts
            nav->setCurrentRow(1); // Behavior in the new order
            dialog.reject();
        }
        QCOMPARE(settings.value(QStringLiteral("Preferences/LastPage")).toInt(), 1);
        {
            RemotePreferencesDialog dialog(settings);
            auto *nav = dialog.findChild<QListWidget *>(
                QStringLiteral("preferencesSections"));
            QVERIFY(nav);
            QCOMPARE(nav->currentRow(), 1);
            dialog.reject();
        }

        // A previously selected native page keeps its meaning too.
        settings.remove(QStringLiteral("Preferences/NavOrderVersion"));
        settings.setValue(QStringLiteral("Preferences/LastPage"), 0);
        {
            RemotePreferencesDialog dialog(settings);
            auto *nav = dialog.findChild<QListWidget *>(
                QStringLiteral("preferencesSections"));
            QVERIFY(nav);
            QCOMPARE(nav->currentRow(), 1); // former Behavior
            dialog.reject();
        }
    }

    void longMappingPathsCanBeScrolledWithoutWideningPreferences()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QSettings settings(tmp.filePath("preferences.ini"), QSettings::IniFormat);
        Remote::ProfileStore profiles(settings);
        Remote::ServerProfile first{{}, "Long paths", "http://127.0.0.1:6363", "demo", false, false};
        Remote::ServerProfile second{{}, "Short paths", "http://127.0.0.1:6364", "demo", false, false};
        QVERIFY(profiles.save(first));
        QVERIFY(profiles.save(second));
        const QString local = QDir(tmp.path()).filePath("mount");
        const QString remote = QStringLiteral("/share/") + QStringLiteral("long-directory/").repeated(20) + QStringLiteral("end-of-path");
        Remote::PathMappingStore maps(settings);
        QVERIFY(maps.replace(first.id, {{local, remote}}));
        QVERIFY(maps.replace(second.id, {{local, QStringLiteral("/share/Public")}}));
        RemotePreferencesDialog dialog(settings, first.id);
        dialog.show();
        auto *nav = dialog.findChild<QListWidget *>("preferencesSections");
        auto *selector = dialog.findChild<QComboBox *>("remotePathsServer");
        auto *table = dialog.findChild<QTableWidget *>("remotePathMappings");
        QVERIFY(nav && selector && table);
        nav->setCurrentRow(0);
        dialog.resize(700, 600);
        QTRY_VERIFY(table->isVisible());
        auto *scroll = table->horizontalScrollBar();
        QTRY_VERIFY(scroll->isVisible() && scroll->maximum() > 0);
        QCOMPARE(table->item(0, 1)->text(), remote);
        QCOMPARE(table->item(0, 1)->toolTip(), remote);
        const int width = dialog.width();
        QVERIFY(table->columnWidth(1) >= table->fontMetrics().horizontalAdvance(remote));
        scroll->setValue(scroll->maximum());
        QTRY_COMPARE(table->horizontalHeader()->offset(), scroll->maximum());
        QVERIFY(table->visualItemRect(table->item(0, 1)).right() <= table->viewport()->rect().right());
        QCOMPARE(dialog.width(), width);
        const int longColumnWidth = table->columnWidth(1);
        selector->setCurrentIndex(selector->findData(second.id));
        QTRY_COMPARE(table->item(0, 1)->text(), QStringLiteral("/share/Public"));
        QTRY_VERIFY(table->columnWidth(1) < longColumnWidth);
        // Fonts, theme metrics and temporary directory lengths differ by
        // platform. Give the short mappings sufficient actual viewport space,
        // rather than assuming they fit a particular dialog width.
        QTRY_VERIFY(table->horizontalHeader()->length() < longColumnWidth);
        const int extraWidth = qMax(0, table->horizontalHeader()->length() - table->viewport()->width());
        dialog.resize(dialog.width() + extraWidth + 40, dialog.height());
        QTRY_COMPARE(scroll->maximum(), 0);
        QTRY_VERIFY(!scroll->isVisible());
        dialog.resize(width, dialog.height());
        selector->setCurrentIndex(selector->findData(first.id));
        QTRY_VERIFY(scroll->isVisible() && scroll->maximum() > 0);
        QCOMPARE(table->item(0, 1)->text(), remote);
    }

    void mappingEditorStagesByServerAndCancelDoesNotPersist()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QSettings settings(tmp.filePath(QStringLiteral("preferences.ini")),
                           QSettings::IniFormat);
        Remote::ProfileStore profiles(settings);
        Remote::ServerProfile first{
            {}, QStringLiteral("QNAP default"),
            QStringLiteral("https://127.0.0.1:6363"), QStringLiteral("demo"), false, false
        };
        Remote::ServerProfile second{
            {}, QStringLiteral("RED"),
            QStringLiteral("http://127.0.0.1:6364"), QStringLiteral("demo"), false, false
        };
        QVERIFY(profiles.save(first));
        QVERIFY(profiles.save(second));
        const QString local = QDir(tmp.path()).filePath(QStringLiteral("mounted"));
        const QString remote = QStringLiteral("/share/CACHEDEV1_DATA/Downloads");

        const QString alias = QStringLiteral("/share/Public/Downloads");
        const auto addViaDialog = [&](RemotePreferencesDialog &dialog, const QString &remotePath) {
            auto *button = dialog.findChild<QPushButton *>(
                QStringLiteral("addPathMapping"));
            QVERIFY(button);
            bool editorOpened = false;
            QTimer::singleShot(0, [&]() {
                auto *modal = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                if (!modal || modal->objectName() != QStringLiteral("mappingEditor"))
                    return;
                auto *localField = modal->findChild<QLineEdit *>(
                    QStringLiteral("mappingLocalPath"));
                auto *remoteField = modal->findChild<QLineEdit *>(
                    QStringLiteral("mappingRemotePath"));
                auto *buttons = modal->findChild<QDialogButtonBox *>();
                if (!localField || !remoteField || !buttons)
                    return;
                localField->setText(local);
                remoteField->setText(remotePath);
                editorOpened = true;
                buttons->button(QDialogButtonBox::Ok)->click();
            });
            button->click();
            QVERIFY(editorOpened);
        };

        {
            RemotePreferencesDialog dialog(settings, first.id);
            dialog.show();
            QTRY_VERIFY(dialog.isVisible());
            auto *nav = dialog.findChild<QListWidget *>(
                QStringLiteral("preferencesSections"));
            auto *selector = dialog.findChild<QComboBox *>(
                QStringLiteral("remotePathsServer"));
            auto *table = dialog.findChild<QTableWidget *>(
                QStringLiteral("remotePathMappings"));
            auto *apply = dialog.findChild<QPushButton *>(
                QStringLiteral("preferencesApply"));
            QVERIFY(nav && selector && table && apply);
            nav->setCurrentRow(0);
            QCOMPARE(nav->item(0)->text(), QStringLiteral("Servers"));
            QCOMPARE(selector->currentData().toString(), first.id);
            addViaDialog(dialog, remote);
            QCOMPARE(table->rowCount(), 1);
            // Local paths follow the client's native style; remote paths
            // remain POSIX on Windows as well as Linux.
            QCOMPARE(table->item(0, 0)->text(), QDir::toNativeSeparators(local));
            QCOMPARE(table->item(0, 1)->text(), remote);
            QVERIFY(apply->isEnabled());
            selector->setCurrentIndex(selector->findData(second.id));
            QCOMPARE(table->rowCount(), 0);
            selector->setCurrentIndex(selector->findData(first.id));
            QCOMPARE(table->rowCount(), 1);
            QVERIFY(Remote::PathMappingStore(settings).mappings(first.id).isEmpty());
            dialog.reject();
        }
        QVERIFY(Remote::PathMappingStore(settings).mappings(first.id).isEmpty());

        {
            RemotePreferencesDialog dialog(settings, first.id);
            dialog.show();
            QTRY_VERIFY(dialog.isVisible());
            auto *table = dialog.findChild<QTableWidget *>(
                QStringLiteral("remotePathMappings"));
            QVERIFY(table);
            addViaDialog(dialog, remote);
            addViaDialog(dialog, alias);
            QCOMPARE(table->rowCount(), 2);
            QCOMPARE(table->item(0, 1)->text(), remote);
            QCOMPARE(table->item(1, 1)->text(), alias);
            QCOMPARE(table->item(0, 0)->text(), table->item(1, 0)->text());
            auto *prefer = dialog.findChild<QPushButton *>(QStringLiteral("preferPathMapping"));
            QVERIFY(prefer);
            table->selectRow(1);
            QVERIFY(prefer->isEnabled());
            prefer->click();
            QCOMPARE(table->item(0, 1)->text(), alias);
            QCOMPARE(table->item(1, 1)->text(), remote);
            auto *apply = dialog.findChild<QPushButton *>(
                QStringLiteral("preferencesApply"));
            apply->click();
            QVERIFY(!apply->isEnabled());
            QVERIFY(dialog.isVisible());
            dialog.reject();
        }
        const auto stored = Remote::PathMappingStore(settings).mappings(first.id);
        QCOMPARE(stored.size(), 2);
        QCOMPARE(stored.first().remoteRoot, alias);
        QCOMPARE(stored.last().remoteRoot, remote);
        QVERIFY(Remote::PathMappingStore(settings).mappings(second.id).isEmpty());
        // Reopening the real page retains the user's preferred ordering.
        {
            RemotePreferencesDialog dialog(settings, first.id);
            auto *table = dialog.findChild<QTableWidget *>(QStringLiteral("remotePathMappings"));
            QVERIFY(table);
            QCOMPARE(table->rowCount(), 2);
            QCOMPARE(table->item(0, 1)->text(), alias);
            QCOMPARE(table->item(1, 1)->text(), remote);
        }

        // Deleting a saved profile must not leave orphaned path mappings.
        QVERIFY(profiles.remove(first.id));
        QVERIFY(Remote::PathMappingStore(settings).mappings(first.id).isEmpty());
    }

    void optionsActionOpensNativePreferencesWithoutServerConnection()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QSettings settings(tmp.filePath(QStringLiteral("prefs.ini")), QSettings::IniFormat);
        MainWindow window(nullptr, &settings);
        window.show();
        QTRY_VERIFY(window.isVisible());
        auto *action = window.findChild<QAction *>(QStringLiteral("actionOptions"));
        QVERIFY(action);
        QVERIFY(action->isEnabled());

        bool opened = false;
        QTimer::singleShot(0, [&]() {
            auto *dialog = qobject_cast<RemotePreferencesDialog *>(
                QApplication::activeModalWidget());
            if (dialog)
            {
                opened = true;
                // Dialogs should center over their main window, not at
                // arbitrary offsets. Offscreen CI deliberately has no
                // window manager and cannot guarantee global positioning.
                const QPoint parentCenter = window.mapToGlobal(window.rect().center());
                const QScreen *screen = QGuiApplication::screenAt(parentCenter);
                if (QGuiApplication::platformName() != QLatin1String("offscreen")
                    && screen)
                {
                    const QRect desired(
                        parentCenter.x() - dialog->width() / 2,
                        parentCenter.y() - dialog->height() / 2,
                        dialog->width(), dialog->height());
                    if (screen->availableGeometry().contains(desired))
                    {
                        const QPoint delta = dialog->geometry().center() - parentCenter;
                        QVERIFY(qAbs(delta.x()) <= 40);
                        QVERIFY(qAbs(delta.y()) <= 40);
                    }
                }
                dialog->reject();
            }
        });
        action->trigger();
        QVERIFY(opened);
        QCOMPARE(settings.value(
                     QStringLiteral("Layout/Dialogs/Preferences/CenterVersion")).toInt(), 1);
        QVERIFY(!settings.value(
                     QStringLiteral("Layout/Dialogs/Preferences")).toByteArray().isEmpty());
    }
};

// Preserve QtTest's native QApplication and test setup for this suite.
// The shared dispatcher selects this entry point before Qt is initialized.
#define main runPreferencesWorkflow
QTEST_MAIN(PreferencesWorkflowTest)
#undef main
#include "test_preferences_workflow.moc"
