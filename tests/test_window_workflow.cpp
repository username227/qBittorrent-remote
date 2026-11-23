// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QMessageBox>
#include <QTimer>
#include <QIcon>
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

    void modalWindowsRetainTheNativeApplicationIcon()
    {
        MainWindow window;
        window.show();
        QTRY_VERIFY(window.isVisible());
        // This is what Wayland uses to associate a Qt toplevel (including
        // secondary dialogs) with an installed desktop-entry icon.
        QCOMPARE(QApplication::desktopFileName(), QStringLiteral("qbittorrent-remote"));

        // A parented Rename, Set Location, or Add Torrent dialog must retain
        // the same native application icon as the main window. Also verify
        // the QApplication fallback for a separate top-level dialog.
        const QPixmap mainIcon = window.windowIcon().pixmap(QSize(32, 32));
        QVERIFY2(!mainIcon.isNull(), "Main window app icon must render");
        QVERIFY2(!qApp->windowIcon().pixmap(QSize(32, 32)).isNull(),
                 "Application-wide default icon must render");
        QDialog child(&window);
        QVERIFY2(!child.windowIcon().pixmap(QSize(32, 32)).isNull(),
                 "Parented dialog must inherit the application icon");
        QDialog independent;
        QVERIFY2(!independent.windowIcon().pixmap(QSize(32, 32)).isNull(),
                 "Other top-level dialogs must use the application icon");

        // Exercise the actual Help > About modal, not only a synthetic
        // QDialog, and verify the Qt window icon on its real top-level window.
        QAction *about = menuAction(window, QStringLiteral("Help"), QStringLiteral("About"));
        QVERIFY2(about, "About action should exist");
        bool observed = false;
        bool renderedIcon = false;
        QTimer::singleShot(0, &window, [&]() {
            for (QWidget *top : QApplication::topLevelWidgets())
            {
                auto *dialog = qobject_cast<QMessageBox *>(top);
                if (dialog && dialog->objectName() == QLatin1String("aboutDialog"))
                {
                    observed = true;
                    renderedIcon = !dialog->windowIcon().pixmap(QSize(32, 32)).isNull();
                    dialog->accept();
                    break;
                }
            }
        });
        about->trigger();
        QVERIFY2(observed, "Help > About should open its real modal dialog");
        QVERIFY2(renderedIcon, "About dialog must retain the native application icon");
        QVERIFY2(!window.windowIcon().pixmap(QSize(32, 32)).isNull(),
                 "Closing the dialog must not clear the main window icon");
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

QTEST_MAIN(WindowWorkflowTest)
#include "test_window_workflow.moc"
