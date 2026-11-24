// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/ConnectionDialog.h"

#include <QAbstractButton>
#include <QApplication>
#include <QEvent>
#include <QComboBox>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest/QtTest>

// Confirm only the actual Remove-server modal. Qt may process a zero-delay
// timer before the modal exists, so observing the Show event is deterministic.
class ConfirmRemoveServer final : public QObject
{
public:
    int confirmations = 0;

protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (event->type() == QEvent::Show)
        {
            auto *dialog = qobject_cast<QMessageBox *>(object);
            if (dialog && dialog->windowTitle() == QStringLiteral("Remove server"))
            {
                ++confirmations;
                QTimer::singleShot(0, dialog, [dialog]() {
                    if (auto *yes = dialog->button(QMessageBox::Yes))
                        yes->click();
                });
            }
        }
        return QObject::eventFilter(object, event);
    }
};

// Functional workflow: save a server via the real GUI, reopen the dialog,
// verify the password is not stored, and remove the saved profile.
class ProfileWorkflowTest final : public QObject
{
    Q_OBJECT

private slots:
    void userCanSaveReopenAndRemoveServer()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("servers.ini")), QSettings::IniFormat);

        {
            ConnectionDialog dialog(settings);
            dialog.show();
            QTRY_VERIFY(dialog.isVisible());

            auto *name = dialog.findChild<QLineEdit *>(QStringLiteral("serverName"));
            auto *url = dialog.findChild<QLineEdit *>(QStringLiteral("serverUrl"));
            auto *user = dialog.findChild<QLineEdit *>(QStringLiteral("serverUsername"));
            auto *pass = dialog.findChild<QLineEdit *>(QStringLiteral("serverPassword"));
            auto *save = dialog.findChild<QPushButton *>(QStringLiteral("saveServer"));
            auto *combo = dialog.findChild<QComboBox *>(QStringLiteral("savedServers"));
            QVERIFY(name && url && user && pass && save && combo);
            name->setText(QStringLiteral("Home seedbox"));
            url->setText(QStringLiteral("http://127.0.0.1:8080"));
            user->setText(QStringLiteral("test-user"));
            pass->setText(QStringLiteral("not-a-real-password"));
            QTest::mouseClick(save, Qt::LeftButton);
            QCOMPARE(combo->count(), 1);
            QCOMPARE(combo->currentText(), QStringLiteral("Home seedbox"));
        }

        {
            ConnectionDialog dialog(settings);
            dialog.show();
            QTRY_VERIFY(dialog.isVisible());
            auto *combo = dialog.findChild<QComboBox *>(QStringLiteral("savedServers"));
            auto *pass = dialog.findChild<QLineEdit *>(QStringLiteral("serverPassword"));
            auto *remove = dialog.findChild<QPushButton *>(QStringLiteral("removeServer"));
            QVERIFY(combo && pass && remove);
            QCOMPARE(combo->count(), 1);
            QVERIFY(pass->text().isEmpty());

            ConfirmRemoveServer confirm;
            qApp->installEventFilter(&confirm);
            QTest::mouseClick(remove, Qt::LeftButton);
            qApp->removeEventFilter(&confirm);
            QCOMPARE(confirm.confirmations, 1);
            QTRY_COMPARE(combo->count(), 0);
        }

        // User-visible removal persists across application sessions.
        ConnectionDialog reopened(settings);
        auto *combo = reopened.findChild<QComboBox *>(QStringLiteral("savedServers"));
        QVERIFY(combo);
        QCOMPARE(combo->count(), 0);
    }
};

// Preserve QtTest's native QApplication and test setup for this suite.
// The shared dispatcher selects this entry point before Qt is initialized.
#define main runProfileWorkflow
QTEST_MAIN(ProfileWorkflowTest)
#undef main
#include "test_profile_workflow.moc"
