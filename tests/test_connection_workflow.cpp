// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/MainWindow.h"
#include "gui/ConnectionDialog.h"

#include <QAction>
#include <QApplication>
#include <QHostAddress>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QStatusBar>
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
    explicit DemoQbittorrentServer(bool acceptPassword, QObject *parent = nullptr)
        : QObject(parent)
        , m_acceptPassword(acceptPassword)
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
                                && data.queryItemValue(QStringLiteral("password")) == QStringLiteral("demo-password");
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

private:
    QTcpServer m_server;
    bool m_acceptPassword;
    int m_logouts = 0;
};

class ConnectionWorkflowTest final : public QObject
{
    Q_OBJECT

private:
    void openConnectDialog(MainWindow &window, const QString &url)
    {
        QTimer::singleShot(0, [url]() {
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
            password->setText(QStringLiteral("demo-password"));
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
        MainWindow window(nullptr, &settings);
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
    }

    void userSeesAuthenticationFailure()
    {
        DemoQbittorrentServer server(false);
        QVERIFY(server.start());

        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("profiles.ini")), QSettings::IniFormat);
        MainWindow window(nullptr, &settings);
        window.show();
        QTRY_VERIFY(window.isVisible());

        openConnectDialog(window, server.url());
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage()
            .contains(QStringLiteral("Login failed")), 6000);
        QVERIFY(!window.windowTitle().contains(QStringLiteral("Demo seedbox")));
    }
};

QTEST_MAIN(ConnectionWorkflowTest)
#include "test_connection_workflow.moc"
