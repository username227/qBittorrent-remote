// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/MainWindow.h"

#include <QAction>
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
