// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/MainWindow.h"
#include "gui/ConnectionDialog.h"
#include "api/CredentialVault.h"

#include <QAction>
#include <QApplication>
#include <QEvent>
#include <QCheckBox>
#include <QFile>
#include <QHash>
#include <QHostAddress>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStatusBar>
#include <QTableView>
#include <QAbstractItemModel>
#include <QListWidget>
#include <QLabel>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrlQuery>
#include <QtTest/QtTest>

#include <memory>

// A local, disposable HTTP server providing observable qBittorrent API
// behavior; it never contacts a NAS or uses any real credentials.
class DemoQbittorrentServer final : public QObject
{
public:
    explicit DemoQbittorrentServer(bool acceptPassword, QObject *parent = nullptr,
                                   bool acceptBlank = false)
        : QObject(parent)
        , m_acceptPassword(acceptPassword)
        , m_acceptBlank(acceptBlank)
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            while (m_server.hasPendingConnections())
            {
                auto *socket = m_server.nextPendingConnection();
                auto incoming = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::readyRead, this,
                    [this, socket, incoming]() {
                        incoming->append(socket->readAll());
                        const int split = incoming->indexOf("\r\n\r\n");
                        if (split < 0)
                            return;

                        const QByteArray headers = incoming->left(split);
                        int length = 0;
                        for (const QByteArray &line : headers.split('\n'))
                        {
                            if (line.trimmed().toLower().startsWith("content-length:"))
                                length = line.mid(line.indexOf(':') + 1).trimmed().toInt();
                        }
                        if (incoming->size() < split + 4 + length)
                            return;

                        const QByteArray body = incoming->mid(split + 4, length);
                        const QByteArray requestLine = headers.split('\n').first().trimmed();
                        const QList<QByteArray> fields = requestLine.split(' ');
                        const QByteArray path = fields.value(1);
                        const bool hasCookie = headers.contains("QBT_SID=demo-session");

                        int code = 200;
                        QByteArray response;
                        QByteArray cookieHeader;
                        if (path == "/api/v2/auth/login")
                        {
                            const QUrlQuery data(QString::fromUtf8(body));
                            const bool good = m_acceptPassword
                                && data.queryItemValue(QStringLiteral("username")) == QStringLiteral("demo")
                                && (data.queryItemValue(QStringLiteral("password"))
                                        == QStringLiteral("demo-password")
                                    || (m_acceptBlank && data.queryItemValue(
                                        QStringLiteral("password")).isEmpty()));
                            response = good ? QByteArrayLiteral("Ok.") : QByteArrayLiteral("Fails.");
                            if (good)
                                cookieHeader = "Set-Cookie: QBT_SID=demo-session; Path=/; HttpOnly\r\n";
                        }
                        else if (path == "/api/v2/app/version" && hasCookie)
                        {
                            response = "v5.1.4";
                        }
                        else if (path == "/api/v2/app/webapiVersion" && hasCookie)
                        {
                            response = "2.11.3";
                        }
                        else if (path.startsWith("/api/v2/sync/maindata?rid=") && hasCookie)
                        {
                            const QUrlQuery query(QString::fromLatin1(path.mid(path.indexOf('?') + 1)));
                            const int rid = query.queryItemValue(QStringLiteral("rid")).toInt();
                            if (!m_demoTorrents)
                                response = R"({"rid":1,"full_update":true,"torrents":{},"server_state":{"dl_info_speed":0,"up_info_speed":0}})";
                            else if (rid == 0)
                                response = R"({"rid":1,"full_update":true,"torrents":{"0123456789abcdef0123456789abcdef01234567":{"name":"Demo Linux ISO","size":1048576,"progress":0.25,"state":"downloading","dlspeed":2048,"upspeed":1024,"category":"linux"}},"server_state":{"dl_info_speed":2048,"up_info_speed":1024}})";
                            else if (rid == 1)
                                response = R"({"rid":2,"full_update":false,"torrents":{"0123456789abcdef0123456789abcdef01234567":{"name":"Updated Linux ISO","progress":0.5,"dlspeed":4096}},"server_state":{"dl_info_speed":4096}})";
                            else
                                response = R"({"rid":3,"full_update":false,"torrents_removed":["0123456789abcdef0123456789abcdef01234567"],"server_state":{"dl_info_speed":0,"up_info_speed":0}})";
                        }
                        else if (path == "/api/v2/auth/logout" && hasCookie)
                        {
                            ++m_logouts;
                            response = "Ok.";
                        }
                        else
                        {
                            code = 403;
                            response = "Forbidden";
                        }

                        QByteArray result = "HTTP/1.1 " + QByteArray::number(code) +
                            (code == 200 ? " OK\r\n" : " Forbidden\r\n");
                        result += "Content-Type: text/plain\r\n";
                        result += "Content-Length: " + QByteArray::number(response.size()) + "\r\n";
                        result += cookieHeader;
                        result += "Connection: close\r\n\r\n";
                        result += response;
                        socket->write(result);
                        socket->disconnectFromHost();
                    });
                connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
            }
        });
    }

    bool start() { return m_server.listen(QHostAddress::LocalHost, 0); }
    QString url() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()); }
    int logouts() const noexcept { return m_logouts; }
    void enableTorrentSyncDemo() { m_demoTorrents = true; }

private:
    QTcpServer m_server;
    bool m_acceptPassword;
    bool m_acceptBlank;
    int m_logouts = 0;
    bool m_demoTorrents = false;
};


// Respond to the actual confirmation message when it is shown. A zero-delay
// timer sent before modal activation is not reliable under offscreen Qt.
class ConfirmProfileRemoval final : public QObject
{
public:
    int prompts = 0;

protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (event->type() == QEvent::Show)
        {
            auto *dialog = qobject_cast<QMessageBox *>(object);
            if (dialog && dialog->windowTitle() == QStringLiteral("Remove server"))
            {
                ++prompts;
                QTimer::singleShot(0, dialog, [dialog]() {
                    if (auto *button = dialog->button(QMessageBox::Yes))
                        button->click();
                });
            }
        }
        return QObject::eventFilter(object, event);
    }
};

// Disposable secret-store stand-in for testing the *real* dialog-to-network
// workflow without writing credentials into the OS keychain on CI machines.
class DemoCredentialVault final : public Remote::CredentialVault
{
public:
    using CredentialVault::keyFor;

    bool available() const override { return true; }

    void read(const Remote::ServerProfile &profile, ReadResult done) override
    {
        ++reads;
        const QString key = keyFor(profile);
        done(data.value(key), data.contains(key), {});
    }

    void write(const Remote::ServerProfile &profile, const QString &password,
               Done done) override
    {
        ++writes;
        data.insert(keyFor(profile), password);
        done({});
    }

    void erase(const Remote::ServerProfile &profile, Done done) override
    {
        ++erases;
        data.remove(keyFor(profile));
        done({});
    }

    QHash<QString, QString> data;
    int reads = 0;
    int writes = 0;
    int erases = 0;
};

class ConnectionWorkflowTest final : public QObject
{
    Q_OBJECT

private:
    void openConnectDialog(MainWindow &window, const QString &url,
                           bool remember = false, bool enterPassword = true)
    {
        QTimer::singleShot(0, [url, remember, enterPassword]() {
            auto *dialog = qobject_cast<ConnectionDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;

            auto *name = dialog->findChild<QLineEdit *>(QStringLiteral("serverName"));
            auto *endpoint = dialog->findChild<QLineEdit *>(QStringLiteral("serverUrl"));
            auto *username = dialog->findChild<QLineEdit *>(QStringLiteral("serverUsername"));
            auto *password = dialog->findChild<QLineEdit *>(QStringLiteral("serverPassword"));
            auto *connectButton = dialog->findChild<QPushButton *>(QStringLiteral("connectServer"));
            if (!name || !endpoint || !username || !password || !connectButton)
                return;

            name->setText(QStringLiteral("Demo seedbox"));
            endpoint->setText(url);
            username->setText(QStringLiteral("demo"));
            password->setText(enterPassword ? QStringLiteral("demo-password") : QString());
            auto *rememberBox = dialog->findChild<QCheckBox *>(
                QStringLiteral("rememberPassword"));
            if (rememberBox && rememberBox->isEnabled())
                rememberBox->setChecked(remember);
            QTest::mouseClick(connectButton, Qt::LeftButton);
        });
        if (auto *action = window.findChild<QAction *>(QStringLiteral("connectToServer")))
            action->trigger();
    }

private slots:
    void userCanConnectAndDisconnect()
    {
        DemoQbittorrentServer server(true);
        QVERIFY(server.start());

        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("profiles.ini")), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        QTRY_VERIFY(window.isVisible());

        openConnectDialog(window, server.url());
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage()
            .contains(QStringLiteral("Connected to Demo seedbox")), 6000);
        QVERIFY(window.windowTitle().contains(QStringLiteral("Demo seedbox")));

        QAction *disconnect = window.findChild<QAction *>(QStringLiteral("disconnectServer"));
        QVERIFY(disconnect && disconnect->isEnabled());
        disconnect->trigger();
        QTRY_VERIFY(window.statusBar()->currentMessage()
            .contains(QStringLiteral("Not connected")));
        QTRY_COMPARE_WITH_TIMEOUT(server.logouts(), 1, 6000);
        // Manual login must never launch any system-keyring operations.
        QCOMPARE(vault.reads, 0);
        QCOMPARE(vault.writes, 0);
        QCOMPARE(vault.erases, 0);
    }

    void userCanRememberAndReusePasswordWithoutSavingPlaintext()
    {
        DemoQbittorrentServer server(true);
        QVERIFY(server.start());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString settingsPath = temp.filePath(QStringLiteral("profiles.ini"));
        QSettings settings(settingsPath, QSettings::IniFormat);
        DemoCredentialVault vault;

        {
            MainWindow first(nullptr, &settings, &vault);
            first.show();
            QTRY_VERIFY(first.isVisible());
            openConnectDialog(first, server.url(), true, true);
            QTRY_VERIFY_WITH_TIMEOUT(first.statusBar()->currentMessage()
                .contains(QStringLiteral("Connected to Demo seedbox")), 6000);
            QCOMPARE(vault.writes, 1);

            settings.sync();
            QFile stored(settingsPath);
            QVERIFY(stored.open(QIODevice::ReadOnly));
            QVERIFY(!stored.readAll().contains("demo-password"));
        }

        // A new app window with the same profile automatically retrieves the
        // securely remembered password if its password field is left empty.
        {
            MainWindow next(nullptr, &settings, &vault);
            next.show();
            QTRY_VERIFY(next.isVisible());
            openConnectDialog(next, server.url(), true, false);
            QTRY_VERIFY_WITH_TIMEOUT(next.statusBar()->currentMessage()
                .contains(QStringLiteral("Connected to Demo seedbox")), 6000);
            QCOMPARE(vault.reads, 1);
            QCOMPARE(vault.writes, 1);
        }

        // Opting out removes the remembered credential when the profile is saved.
        {
            MainWindow manual(nullptr, &settings, &vault);
            manual.show();
            QTRY_VERIFY(manual.isVisible());
            openConnectDialog(manual, server.url(), false, true);
            QTRY_VERIFY_WITH_TIMEOUT(manual.statusBar()->currentMessage()
                .contains(QStringLiteral("Connected to Demo seedbox")), 6000);
            QCOMPARE(vault.erases, 1);
            QVERIFY(vault.data.isEmpty());
        }
    }

    void removingSavedServerForgetsItsProtectedCredential()
    {
        DemoQbittorrentServer server(true);
        QVERIFY(server.start());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("profiles.ini")), QSettings::IniFormat);
        DemoCredentialVault vault;

        {
            MainWindow window(nullptr, &settings, &vault);
            window.show();
            QTRY_VERIFY(window.isVisible());
            openConnectDialog(window, server.url(), true, true);
            QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage()
                .contains(QStringLiteral("Connected to Demo seedbox")), 6000);
            QCOMPARE(vault.writes, 1);
            QCOMPARE(vault.data.size(), 1);
        }

        // Remove a saved profile through the connection manager, including
        // its keyring entry. It must not affect another server's credentials.
        {
            ConnectionDialog dialog(settings, nullptr, &vault);
            dialog.show();
            QTRY_VERIFY(dialog.isVisible());

            auto *remove = dialog.findChild<QPushButton *>(
                QStringLiteral("removeServer"));
            QVERIFY(remove && remove->isEnabled());
            ConfirmProfileRemoval confirm;
            qApp->installEventFilter(&confirm);
            QTest::mouseClick(remove, Qt::LeftButton);
            qApp->removeEventFilter(&confirm);
            QCOMPARE(confirm.prompts, 1);
            QCOMPARE(vault.erases, 1);
            QVERIFY(vault.data.isEmpty());
        }
    }

    void userCanConnectWithBlankPasswordWhenServerPermitsIt()
    {
        DemoQbittorrentServer server(true, nullptr, true);
        QVERIFY(server.start());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("profiles.ini")), QSettings::IniFormat);

        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        QTRY_VERIFY(window.isVisible());
        openConnectDialog(window, server.url(), false, false);
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage()
            .contains(QStringLiteral("Connected to Demo seedbox")), 6000);
        QCOMPARE(vault.reads, 0);
        QCOMPARE(vault.writes, 0);
        QCOMPARE(vault.erases, 0);
    }

    void liveTorrentRowsFollowFullUpdatesPatchesAndRemovals()
    {
        DemoQbittorrentServer server(true);
        server.enableTorrentSyncDemo();
        QVERIFY(server.start());

        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("profiles.ini")), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        QTRY_VERIFY(window.isVisible());

        openConnectDialog(window, server.url());
        auto *table = window.findChild<QTableView *>(QStringLiteral("torrentList"));
        QVERIFY2(table && table->model(), "The native torrent table must exist.");

        QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 1, 6000);
        QCOMPARE(table->model()->index(0, 0).data().toString(), QStringLiteral("Demo Linux ISO"));
        QVERIFY(window.statusBar()->findChildren<QLabel *>().size() > 0);

        auto *search = window.findChild<QLineEdit *>(QStringLiteral("torrentSearch"));
        QVERIFY2(search && search->isEnabled(), "Live torrent search should be enabled.");
        search->setText(QStringLiteral("does-not-exist"));
        QTRY_COMPARE(table->model()->rowCount(), 0);
        search->clear();
        QTRY_COMPARE(table->model()->rowCount(), 1);

        QTRY_COMPARE_WITH_TIMEOUT(table->model()->index(0, 0).data().toString(),
                                  QStringLiteral("Updated Linux ISO"), 6000);
        QCOMPARE(table->model()->index(0, 2).data().toString().endsWith(QLatin1Char('%')), true);
        QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 0, 9000);
    }

    void userSeesAuthenticationFailure()
    {
        DemoQbittorrentServer server(false);
        QVERIFY(server.start());

        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("profiles.ini")), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        QTRY_VERIFY(window.isVisible());

        openConnectDialog(window, server.url());
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage()
            .contains(QStringLiteral("Login failed")), 6000);
        QVERIFY(!window.windowTitle().contains(QStringLiteral("Demo seedbox")));
        QCOMPARE(vault.reads, 0);
        QCOMPARE(vault.writes, 0);
        QCOMPARE(vault.erases, 0);
    }
};

QTEST_MAIN(ConnectionWorkflowTest)
#include "test_connection_workflow.moc"
