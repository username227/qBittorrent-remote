// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/MainWindow.h"

#include <QAction>
#include <QIcon>
#include <QImage>
#include <QPalette>
#include <QSettings>
#include <QSystemTrayIcon>
#include <QTemporaryDir>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QToolBar>
#include <QtTest/QtTest>

namespace
{
QString label(QAction *action)
{
    QString text = action->text();
    text.remove(QLatin1Char('&'));
    return text;
}

// Locate menu items as a user sees them, without invoking internal methods.
QAction *menuAction(MainWindow &window, const QString &menuName, const QString &actionName)
{
    for (QAction *top : window.menuBar()->actions())
    {
        if (label(top) != menuName || !top->menu())
            continue;

        for (QAction *child : top->menu()->actions())
        {
            if (label(child) == actionName)
                return child;
        }
    }
    return nullptr;
}
} // namespace

class WindowWorkflowTest final : public QObject
{
    Q_OBJECT

private slots:
    void opensAndExitsThroughTheMenu()
    {
        MainWindow window;
        window.show();
        QTRY_VERIFY(window.isVisible());

        QAction *exit = menuAction(window, QStringLiteral("File"), QStringLiteral("Exit"));
        QVERIFY2(exit && exit->isEnabled(), "File > Exit must be available.");
        exit->trigger();

        QTRY_VERIFY(!window.isVisible());
    }

    void bundledSvgIconsRenderOnToolbarAndSidebar()
    {
        MainWindow window;
        window.show();
        QTRY_VERIFY(window.isVisible());

        // Merely checking QIcon::isNull() is insufficient: a missing SVG
        // engine can leave a non-null QIcon that renders no pixmap.
        QAction *open = menuAction(window, QStringLiteral("File"),
                                   QStringLiteral("Add Torrent File..."));
        QVERIFY2(open, "File > Open should retain the native toolbar action.");
        QVERIFY2(!open->icon().pixmap(QSize(24, 24)).isNull(),
                 "The original Open SVG toolbar icon must render, including on Windows.");

        const auto lists = window.findChildren<QListWidget *>(QStringLiteral("filterList"));
        QVERIFY2(!lists.isEmpty() && lists.first()->count() > 0,
                 "The native filters sidebar must contain its status entries.");
        QVERIFY2(!lists.first()->item(0)->icon().pixmap(QSize(16, 16)).isNull(),
                 "The original filter status SVG icon must render, including on Windows.");

        QVERIFY2(!window.windowIcon().pixmap(QSize(32, 32)).isNull(),
                 "The native application window SVG icon must render.");
    }

#ifdef Q_OS_WIN
    void windowsSidebarUsesCompactIconAndFontAwareRows()
    {
        MainWindow window;
        window.show();
        QTRY_VERIFY(window.isVisible());

        const auto lists = window.findChildren<QListWidget *>(QStringLiteral("filterList"));
        QVERIFY2(!lists.isEmpty(), "Expected Windows sidebar filter lists.");

        for (QListWidget *list : lists)
        {
            QVERIFY2(list->count() > 0, "Each filter section must have entries.");
            const int minimumHeight = qMax(list->fontMetrics().height(),
                                            list->iconSize().height());
            const int expectedHeight = minimumHeight + 4;
            for (int i = 0; i < list->count(); ++i)
            {
                QCOMPARE(list->item(i)->sizeHint().height(), expectedHeight);
                QCOMPARE(list->sizeHintForRow(i), expectedHeight);
            }
            QCOMPARE(list->height(),
                     list->count() * expectedHeight + 2 * list->frameWidth());
        }
    }
#endif

    void savedTrayStyleRendersReadableArtworkAcrossThemes()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QSettings settings(tmp.filePath(QStringLiteral("icons.ini")), QSettings::IniFormat);
        const QString style = QStringLiteral("Preferences/Behavior/TrayIconStyle");
        // Inspect the actual tray widget, even in CI without a native tray host.
        settings.setValue(style, 0);
        {
            MainWindow window(nullptr, &settings);
            window.show();
            QTRY_VERIFY(window.isVisible());
            auto *tray = window.findChild<QSystemTrayIcon *>(QStringLiteral("remoteTrayIcon"));
            QVERIFY(tray);
            QCOMPARE(tray->isVisible(), QSystemTrayIcon::isSystemTrayAvailable());
            const QImage normal = tray->icon().pixmap(32, 32).toImage();
            QVERIFY(!normal.isNull());
            const QColor background = normal.pixelColor(16, 5);
            QVERIFY(background.alpha() > 200);
            QVERIFY(background.green() > background.red() + 30);
            QVERIFY(background.green() >= background.blue());
            QCOMPARE(window.windowIcon().pixmap(32, 32).toImage(), normal);
        }
        // An explicit opt-out must override the enabled-by-default setting.
        settings.setValue(QStringLiteral("Preferences/Behavior/EnableTray"), false);
        // Reopen with saved Monochrome, then change the live window palette.
        settings.setValue(style, 1);
        MainWindow window(nullptr, &settings);
        window.show();
        auto *tray = window.findChild<QSystemTrayIcon *>(QStringLiteral("remoteTrayIcon"));
        QVERIFY(tray);
        QVERIFY(!tray->isVisible());
        const auto checkSymbolic = [tray](bool lightInk) {
            const QImage img = tray->icon().pixmap(32, 32).toImage();
            QVERIFY(!img.isNull());
            int opaqueInterior = 0;
            int transparentInterior = 0;
            int appropriateInk = 0;
            for (int y = 5; y < 27; ++y)
                for (int x = 5; x < 27; ++x)
                {
                    if ((x - 16) * (x - 16) + (y - 16) * (y - 16) > 100)
                        continue;
                    const QColor pixel = img.pixelColor(x, y);
                    if (pixel.alpha() < 20) ++transparentInterior;
                    if (pixel.alpha() > 180)
                    {
                        ++opaqueInterior;
                        if (lightInk ? pixel.lightness() > 180 : pixel.lightness() < 100)
                            ++appropriateInk;
                    }
                }
            // A solid disc had no transparent interior; require real letter strokes.
            QVERIFY(opaqueInterior > 15);
            QVERIFY(transparentInterior > 40);
            QVERIFY(appropriateInk > 15);
        };
        QPalette light = window.palette();
        light.setColor(QPalette::Window, QColor(Qt::white));
        light.setColor(QPalette::WindowText, QColor(Qt::black));
        window.setPalette(light);
        QCoreApplication::processEvents();
        checkSymbolic(false);
        QPalette dark = light;
        dark.setColor(QPalette::Window, QColor(QStringLiteral("#202020")));
        dark.setColor(QPalette::WindowText, QColor(Qt::white));
        window.setPalette(dark);
        QCoreApplication::processEvents();
        checkSymbolic(true);
        const QColor windowColor = window.windowIcon().pixmap(32, 32)
            .toImage().pixelColor(16, 5);
        QVERIFY(windowColor.green() > windowColor.red() + 30);
    }

    void userCanHideAndRestoreFiltersSidebar()
    {
        MainWindow window;
        window.show();
        QTRY_VERIFY(window.isVisible());

        QAction *toggle = menuAction(window, QStringLiteral("View"), QStringLiteral("Filters Sidebar"));
        QWidget *sidebar = window.findChild<QWidget *>(QStringLiteral("filterSidebar"));
        QVERIFY2(toggle && sidebar, "View must offer a working Filters Sidebar toggle.");
        QTRY_VERIFY(sidebar->isVisible());

        toggle->trigger();
        QTRY_VERIFY(!sidebar->isVisible());

        toggle->trigger();
        QTRY_VERIFY(sidebar->isVisible());
    }

    void userCanHideAndRestoreToolbar()
    {
        MainWindow window;
        window.show();
        QTRY_VERIFY(window.isVisible());

        QAction *toggle = menuAction(window, QStringLiteral("View"), QStringLiteral("Top Toolbar"));
        QToolBar *toolbar = window.findChild<QToolBar *>();
        QVERIFY2(toggle && toolbar, "The toolbar must be available from View.");
        QTRY_VERIFY(toolbar->isVisible());

        toggle->trigger();
        QTRY_VERIFY(!toolbar->isVisible());

        toggle->trigger();
        QTRY_VERIFY(toolbar->isVisible());
    }
};

// Preserve QtTest's native QApplication and test setup for this suite.
// The shared dispatcher selects this entry point before Qt is initialized.
#define main runWindowWorkflow
QTEST_MAIN(WindowWorkflowTest)
#undef main
#include "test_window_workflow.moc"
