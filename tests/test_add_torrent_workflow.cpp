// SPDX-License-Identifier: GPL-3.0-or-later
#include "api/TorrentMetadataReader.h"
#include "api/PathMappingStore.h"
#include "gui/RemoteAddTorrentDialog.h"
#include "gui/RemoteTorrentContentWidget.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QEvent>
#include <QMessageBox>
#include <QTimer>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSet>
#include <QTemporaryDir>
#include <QTreeView>
#include <QToolButton>
#include <QLineEdit>
#include <QtTest/QtTest>

class AddTorrentWorkflowTest final : public QObject
{
    Q_OBJECT

private:
    QStringList m_unexpectedWarnings;

    bool eventFilter(QObject *obj, QEvent *event) override
    {
        if (event->type() == QEvent::Show)
            if (auto *warning = qobject_cast<QMessageBox *>(obj))
            {
                m_unexpectedWarnings.append(warning->windowTitle()
                                            + QStringLiteral(": ") + warning->text());
                QTimer::singleShot(0, warning, &QMessageBox::reject);
            }
        return QObject::eventFilter(obj, event);
    }

private slots:
    void initTestCase()
    {
        QApplication::instance()->installEventFilter(this);
    }

    void init()
    {
        m_unexpectedWarnings.clear();
    }

    void cleanup()
    {
        QVERIFY2(m_unexpectedWarnings.isEmpty(),
                 qPrintable(m_unexpectedWarnings.join(QLatin1Char('\n'))));
    }

    void cleanupTestCase()
    {
        QApplication::instance()->removeEventFilter(this);
    }


    void torrentMetadataExposesAnnounceUrlsForDuplicateMerge()
    {
        const QByteArray first("https://tracker.one.example/announce/secret");
        const QByteArray second("udp://tracker.two.example:80/announce");
        const auto encode = [](const QByteArray &s) {
            return QByteArray::number(s.size()) + ':' + s;
        };
        // Canonical bencoded announce and two announce-list tiers. The
        // duplicated first URL must appear only once in the merge list.
        const QByteArray metadata = QByteArrayLiteral("d8:announce")
            + encode(first) + QByteArrayLiteral("13:announce-listll")
            + encode(first) + QByteArrayLiteral("el")
            + encode(second) + QByteArrayLiteral("ee4:info")
            + QByteArrayLiteral("d6:lengthi123e4:name7:examplee")
            + QByteArrayLiteral("e");
        Remote::TorrentMetadata decoded;
        QString error;
        QVERIFY2(Remote::TorrentMetadataReader::read(metadata, decoded, &error),
                 qPrintable(error));
        QCOMPARE(decoded.name, QStringLiteral("example"));
        QCOMPARE(decoded.trackers, (QStringList{
            QString::fromLatin1(first), QString::fromLatin1(second)}));
        QCOMPARE(decoded.infoHashV1.size(), 40);
        QVERIFY(!decoded.isPrivate);

        const QByteArray privateTorrent = QByteArrayLiteral("d8:announce")
            + encode(first) + QByteArrayLiteral("4:info")
            + QByteArrayLiteral("d6:lengthi123e4:name7:example7:privatei1eee");
        QVERIFY(Remote::TorrentMetadataReader::read(privateTorrent, decoded, &error));
        QVERIFY(decoded.isPrivate);
        QCOMPARE(decoded.trackers, QStringList{QString::fromLatin1(first)});
    }

    void parseLocalTorrentMetadataWithoutContactingNas()
    {
        const QByteArray metadata(
            "d4:infod6:lengthi123e4:name7:exampleee");
        Remote::TorrentMetadata result;
        QString error;
        QVERIFY2(Remote::TorrentMetadataReader::read(metadata, result, &error),
                 qPrintable(error));
        QCOMPARE(result.name, QStringLiteral("example"));
        QCOMPARE(result.totalSize, qint64(123));
        QCOMPARE(result.files.size(), 1);
        QCOMPARE(result.files.first().path, QStringLiteral("example"));
        QCOMPARE(result.infoHashV1.size(), 40);
        QVERIFY(result.infoHashV2.isEmpty());
        QVERIFY(!Remote::TorrentMetadataReader::read(
            QByteArrayLiteral("not a torrent"), result, &error));
        QVERIFY(!Remote::TorrentMetadataReader::read(
            QByteArrayLiteral("d4:infod6:lengthi-1e4:name7:exampleee"), result, &error));
    }

    void nativeFileDialogUsesServerMappingForBothSaveLocations()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("config.ini")), QSettings::IniFormat);
        const QString mount = QDir(temp.path()).filePath(QStringLiteral("QNAP"));
        QVERIFY(QDir().mkpath(mount));
        Remote::ServerProfile profile {
            QStringLiteral("server-a"), QStringLiteral("QNAP"),
            QStringLiteral("http://127.0.0.1:6363"),
            QStringLiteral("demo"), false, false
        };
        Remote::PathMappingStore paths(settings);
        QVERIFY(paths.replace(profile.id, {{
            mount, QStringLiteral("/share/CACHEDEV1_DATA/Downloads")
        }}));

        const QByteArray metadata(
            "d4:infod6:lengthi123e4:name7:exampleee");
        RemoteAddTorrentDialog dialog(settings, profile,
            RemoteAddTorrentDialog::Source::TorrentFile, {},
            QStringLiteral("example.torrent"), metadata);
        dialog.show();
        QTRY_VERIFY(dialog.isVisible());
        // The native .torrent dialog uses the torrent's name as its title.
        QCOMPARE(dialog.windowTitle(), QStringLiteral("example"));
        auto *tree = dialog.findChild<RemoteTorrentContentWidget *>(
            QStringLiteral("contentTreeView"));
        QVERIFY(tree);
        QCOMPARE(tree->model()->columnCount(), 3);
        QCOMPARE(tree->model()->rowCount(), 1);
        const QModelIndex first = tree->model()->index(0, 0);
        QCOMPARE(first.data().toString(), QStringLiteral("example"));
        QCOMPARE(first.data(Qt::CheckStateRole).toInt(), int(Qt::Checked));
        QVERIFY(!first.data(Qt::DecorationRole).value<QIcon>().isNull());
        QVERIFY(!(tree->model()->flags(first) & Qt::ItemIsUserCheckable));
        QVERIFY(!tree->header()->highlightSections());
        auto *filter = dialog.findChild<QLineEdit *>(
            QStringLiteral("torrentContentFilter"));
        QVERIFY(filter);
        filter->setText(QStringLiteral("nonmatching file"));
        QVERIFY(tree->isRowHidden(0, QModelIndex()));
        filter->clear();
        QVERIFY(!tree->isRowHidden(0, QModelIndex()));
        QCOMPARE(dialog.findChildren<QToolButton *>(
                     QStringLiteral("browseFolderButton")).size(), 2);
        QCOMPARE(dialog.findChild<QGroupBox *>(
                     QStringLiteral("groupBoxSavePath"))->title(),
                 QStringLiteral("Save at"));
        QVERIFY(!dialog.findChild<QLabel *>(QStringLiteral("remotePathExplanation")));

        auto *all = dialog.findChild<QPushButton *>(QStringLiteral("buttonSelectAll"));
        QVERIFY(all && !all->isEnabled());
        auto *save = dialog.findChild<QComboBox *>(QStringLiteral("remoteSavePath"));
        auto *incomplete = dialog.findChild<QComboBox *>(QStringLiteral("remoteIncompletePath"));
        QVERIFY(save && incomplete);
        QVERIFY(dialog.setLocalSaveDirectory(mount + QStringLiteral("/Music")));
        QCOMPARE(save->currentText(),
                 QStringLiteral("/share/CACHEDEV1_DATA/Downloads/Music"));
        QVERIFY(dialog.setLocalIncompleteDirectory(mount + QStringLiteral("/Incomplete")));
        QCOMPARE(incomplete->currentText(),
                 QStringLiteral("/share/CACHEDEV1_DATA/Downloads/Incomplete"));
        QVERIFY(!dialog.setLocalSaveDirectory(QStringLiteral("/unmapped/directory")));

        auto *category = dialog.findChild<QComboBox *>(QStringLiteral("categoryComboBox"));
        auto *remember = dialog.findChild<QCheckBox *>(
            QStringLiteral("checkBoxRememberLastSavePath"));
        QVERIFY(category && remember);
        category->setEditText(QStringLiteral("Music"));
        remember->setChecked(true);
        auto *start = dialog.findChild<QCheckBox *>(
            QStringLiteral("startTorrentCheckBox"));
        QVERIFY(start);
        start->setChecked(false);
        auto *layout = dialog.findChild<QComboBox *>(
            QStringLiteral("contentLayoutComboBox"));
        QVERIFY(layout);
        layout->setCurrentIndex(2);
        const auto options = dialog.options();
        QCOMPARE(options.savePath,
                 QStringLiteral("/share/CACHEDEV1_DATA/Downloads/Music"));
        QCOMPARE(options.downloadPath,
                 QStringLiteral("/share/CACHEDEV1_DATA/Downloads/Incomplete"));
        QVERIFY(options.useDownloadPath);
        QCOMPARE(options.category, QStringLiteral("Music"));
        QVERIFY(options.stopped);
        QCOMPARE(options.contentLayout, QStringLiteral("NoSubfolder"));
        QVERIFY(!options.autoTMM);

        auto *buttons = dialog.findChild<QDialogButtonBox *>(
            QStringLiteral("buttonBox"));
        QVERIFY(buttons);
        buttons->button(QDialogButtonBox::Ok)->click();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QCOMPARE(settings.value(
            QStringLiteral("Layout/Dialogs/AddTorrent/SplitterVersion")).toInt(), 2);
        settings.sync();

        RemoteAddTorrentDialog reopened(settings, profile,
            RemoteAddTorrentDialog::Source::TorrentFile, {},
            QStringLiteral("example.torrent"), metadata);
        auto *reopenedSave = reopened.findChild<QComboBox *>(
            QStringLiteral("remoteSavePath"));
        QVERIFY(reopenedSave);
        QCOMPARE(reopenedSave->currentText(),
                 QStringLiteral("/share/CACHEDEV1_DATA/Downloads/Music"));
        reopened.show();
        QTRY_VERIFY(reopened.isVisible());
        auto *reopenedRemember = reopened.findChild<QCheckBox *>(
            QStringLiteral("checkBoxRememberLastSavePath"));
        QVERIFY(reopenedRemember);
        reopenedRemember->setChecked(false);
        auto *reopenedButtons = reopened.findChild<QDialogButtonBox *>(
            QStringLiteral("buttonBox"));
        reopenedButtons->button(QDialogButtonBox::Ok)->click();
        QCOMPARE(reopened.result(), int(QDialog::Accepted));
        RemoteAddTorrentDialog forgotten(settings, profile,
            RemoteAddTorrentDialog::Source::TorrentFile, {},
            QStringLiteral("example.torrent"), metadata);
        QVERIFY(forgotten.findChild<QComboBox *>(
            QStringLiteral("remoteSavePath"))->currentText().isEmpty());

        Remote::ServerProfile other = profile;
        other.id = QStringLiteral("server-b");
        RemoteAddTorrentDialog isolated(settings, other,
            RemoteAddTorrentDialog::Source::TorrentFile, {},
            QStringLiteral("example.torrent"), metadata);
        QVERIFY(!isolated.setLocalSaveDirectory(mount + QStringLiteral("/Music")));
        QVERIFY(isolated.findChild<QComboBox *>(
            QStringLiteral("remoteSavePath"))->currentText().isEmpty());
    }

    void switchingConnectedServersAlwaysClearsTargetSpecificFields()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("config.ini")), QSettings::IniFormat);
        const QString mountA = temp.filePath(QStringLiteral("A"));
        const QString mountB = temp.filePath(QStringLiteral("B"));
        QVERIFY(QDir().mkpath(mountA));
        QVERIFY(QDir().mkpath(mountB));
        Remote::ServerProfile a {
            QStringLiteral("server-a"), QStringLiteral("NAS A"),
            QStringLiteral("http://127.0.0.1:6363"), QStringLiteral("demo"), false, false
        };
        Remote::ServerProfile b {
            QStringLiteral("server-b"), QStringLiteral("NAS B"),
            QStringLiteral("http://127.0.0.1:6364"), QStringLiteral("demo"), false, false
        };
        Remote::PathMappingStore mapping(settings);
        QVERIFY(mapping.replace(a.id, {{mountA, QStringLiteral("/srv/a")}}));
        QVERIFY(mapping.replace(b.id, {{mountB, QStringLiteral("/srv/b")}}));
        QSet<QString> connected {a.id, b.id};
        const QByteArray metadata("d4:infod6:lengthi123e4:name7:exampleee");
        RemoteAddTorrentDialog dialog(settings, a,
            RemoteAddTorrentDialog::Source::TorrentFile, {},
            QStringLiteral("example.torrent"), metadata, nullptr, {a, b},
            [&connected](const QString &id) { return connected.contains(id); });
        dialog.show();
        QTRY_VERIFY(dialog.isVisible());
        auto *selector = dialog.findChild<QComboBox *>(
            QStringLiteral("addTorrentServerSelector"));
        QVERIFY(selector);
        QCOMPARE(selector->count(), 2);
        QCOMPARE(selector->currentData().toString(), a.id);
        QCOMPARE(dialog.selectedServerId(), a.id);
        auto *save = dialog.findChild<QComboBox *>(QStringLiteral("remoteSavePath"));
        auto *incomplete = dialog.findChild<QComboBox *>(QStringLiteral("remoteIncompletePath"));
        auto *category = dialog.findChild<QComboBox *>(QStringLiteral("categoryComboBox"));
        auto *tags = dialog.findChild<QLineEdit *>(QStringLiteral("tagsLineEdit"));
        auto *remember = dialog.findChild<QCheckBox *>(
            QStringLiteral("checkBoxRememberLastSavePath"));
        auto *downloadGroup = dialog.findChild<QGroupBox *>(
            QStringLiteral("groupBoxDownloadPath"));
        QVERIFY(save && incomplete && category && tags && remember && downloadGroup);
        QVERIFY(dialog.setLocalSaveDirectory(mountA + QStringLiteral("/Music")));
        QVERIFY(dialog.setLocalIncompleteDirectory(mountA + QStringLiteral("/Tmp")));
        category->setEditText(QStringLiteral("music-a"));
        tags->setText(QStringLiteral("tag-a"));
        remember->setChecked(true);

        selector->setCurrentIndex(1);
        QCOMPARE(dialog.selectedServerId(), b.id);
        QVERIFY(save->currentText().isEmpty());
        QVERIFY(incomplete->currentText().isEmpty());
        QVERIFY(category->currentText().isEmpty());
        QVERIFY(tags->text().isEmpty());
        QVERIFY(!remember->isChecked());
        QVERIFY(!downloadGroup->isChecked());
        QVERIFY(!dialog.setLocalSaveDirectory(mountA + QStringLiteral("/Music")));
        QVERIFY(dialog.setLocalSaveDirectory(mountB + QStringLiteral("/Films")));
        QCOMPARE(save->currentText(), QStringLiteral("/srv/b/Films"));
        category->setEditText(QStringLiteral("films-b"));
        tags->setText(QStringLiteral("tag-b"));

        selector->setCurrentIndex(0);
        QCOMPARE(dialog.selectedServerId(), a.id);
        QVERIFY(save->currentText().isEmpty());
        QVERIFY(incomplete->currentText().isEmpty());
        QVERIFY(category->currentText().isEmpty());
        QVERIFY(tags->text().isEmpty());
        QVERIFY(dialog.options().savePath.isEmpty()); // server default is valid
        selector->setCurrentIndex(1);
        QVERIFY(save->currentText().isEmpty()); // no draft restored on return
        QVERIFY(category->currentText().isEmpty());
        QVERIFY(tags->text().isEmpty());

        connected.remove(b.id);
        auto *buttons = dialog.findChild<QDialogButtonBox *>(
            QStringLiteral("buttonBox"));
        QVERIFY(buttons);
        buttons->button(QDialogButtonBox::Ok)->click();
        QVERIFY(dialog.result() != QDialog::Accepted);
        QVERIFY(!m_unexpectedWarnings.isEmpty());
        QVERIFY(m_unexpectedWarnings.constLast().contains(QStringLiteral("disconnected"),
                                                           Qt::CaseInsensitive));
        m_unexpectedWarnings.clear();

        selector->setCurrentIndex(0);
        buttons->button(QDialogButtonBox::Ok)->click();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QCOMPARE(dialog.selectedServerId(), a.id);
        QVERIFY(dialog.options().savePath.isEmpty());
    }

    void largeFontReflowsCrowdedNativeCheckboxesWithoutRestrictingFont()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("config.ini")), QSettings::IniFormat);
        const Remote::ServerProfile profile {
            QStringLiteral("server-a"), QStringLiteral("NAS A"),
            QStringLiteral("http://127.0.0.1:6363"), QStringLiteral("demo"), false, false
        };
        const QFont originalFont = QApplication::font();
        QFont largeFont = originalFont;
        largeFont.setPointSize(24);
        QApplication::setFont(largeFont);
        {
            RemoteAddTorrentDialog dialog(settings, profile,
                RemoteAddTorrentDialog::Source::TorrentFile, {},
                QStringLiteral("example.torrent"),
                QByteArrayLiteral("d4:infod6:lengthi123e4:name7:exampleee"));
            auto *grid = dialog.findChild<QGridLayout *>(QStringLiteral("gridLayout"));
            auto *firstLast = dialog.findChild<QCheckBox *>(
                QStringLiteral("firstLastCheckBox"));
            auto *doNotDelete = dialog.findChild<QCheckBox *>(
                QStringLiteral("doNotDeleteTorrentCheckBox"));
            QVERIFY(grid && firstLast && doNotDelete);
            QCOMPARE(grid->itemAtPosition(3, 0)->widget(),
                     static_cast<QWidget *>(firstLast));
            QCOMPARE(grid->itemAtPosition(4, 0)->widget(),
                     static_cast<QWidget *>(doNotDelete));
            QVERIFY(!dialog.findChild<QComboBox *>(
                QStringLiteral("addTorrentServerSelector")));
            QCOMPARE(QApplication::font().pointSize(), 24);
        }
        QApplication::setFont(originalFont);
    }

    void nativeMagnetDialogRetainsUpstreamOptions()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("config.ini")), QSettings::IniFormat);
        Remote::ServerProfile profile {
            QStringLiteral("server-a"), QStringLiteral("QNAP"),
            QStringLiteral("http://127.0.0.1:6363"),
            QStringLiteral("demo"), false, false
        };
        const QString magnet = QStringLiteral(
            "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567");
        RemoteAddTorrentDialog dialog(settings, profile,
            RemoteAddTorrentDialog::Source::Links, magnet);
        dialog.show();
        QTRY_VERIFY(dialog.isVisible());
        auto *links = dialog.findChild<QPlainTextEdit *>(
            QStringLiteral("torrentLinks"));
        QVERIFY(links);
        QCOMPARE(dialog.links(), magnet);
        QVERIFY(!dialog.findChild<RemoteTorrentContentWidget *>(
            QStringLiteral("contentTreeView"))->isEnabled());
        QVERIFY(!dialog.findChild<QPushButton *>(
            QStringLiteral("buttonSave"))->isEnabled());

        auto *tmm = dialog.findChild<QComboBox *>(QStringLiteral("comboTMM"));
        auto *saveGroup = dialog.findChild<QGroupBox *>(
            QStringLiteral("groupBoxSavePath"));
        QVERIFY(tmm && saveGroup);
        tmm->setCurrentIndex(1);
        QVERIFY(!saveGroup->isEnabled());
        QVERIFY(dialog.options().autoTMM);
        auto *buttons = dialog.findChild<QDialogButtonBox *>(QStringLiteral("buttonBox"));
        QVERIFY(buttons);
        buttons->button(QDialogButtonBox::Ok)->click();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
    }
};

QTEST_MAIN(AddTorrentWorkflowTest)
#include "test_add_torrent_workflow.moc"
