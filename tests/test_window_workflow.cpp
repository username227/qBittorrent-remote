// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/MainWindow.h"

#include <QAction>
#include <QMenu>
#include <QMenuBar>
#include <QtTest/QtTest>

// Functional smoke test: exercise the actual Qt window and the user's
// File > Quit workflow. Do not assert internal algorithms or sample API inputs.
class WindowWorkflowTest final : public QObject
{
    Q_OBJECT

private slots:
    void opensAndQuitsThroughTheMenu()
    {
        MainWindow window;
        window.show();

        QTRY_VERIFY(window.isVisible());

        // Discover the displayed Quit action rather than invoking a method
        // on MainWindow directly. This tests the menu action's real behavior.
        QAction *quitAction = nullptr;
        for (QAction *menuAction : window.menuBar()->actions())
        {
            QMenu *menu = menuAction->menu();
            if (!menu)
                continue;

            for (QAction *action : menu->actions())
            {
                QString text = action->text();
                text.remove(QLatin1Char('&'));
                if (text == QStringLiteral("Quit"))
                {
                    quitAction = action;
                    break;
                }
            }
            if (quitAction)
                break;
        }

        QVERIFY2(quitAction, "The application must offer a Quit menu action.");
        quitAction->trigger();

        QTRY_VERIFY(!window.isVisible());
    }
};

QTEST_MAIN(WindowWorkflowTest)
#include "test_window_workflow.moc"
