// SPDX-License-Identifier: GPL-3.0-or-later
#include "api/ServerProfile.h"
#include "gui/ConnectionDialog.h"
#include "gui/MainWindow.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslServer>
#include <QSslSocket>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest/QtTest>

#include <memory>

// Certificates and private keys are generated in a temporary directory by
// OpenSSL at test runtime. No reusable TLS private key is committed.
class EphemeralCertificate final
{
public:
    bool generate(const QString &dir, const QString &name, const QString &san,
                  bool expired = false)
    {
        const QString pemPath = dir + QLatin1Char('/') + name + QStringLiteral(".crt");
        const QString keyPath = dir + QLatin1Char('/') + name + QStringLiteral(".key");
        QProcess generator;
        QStringList args {
            QStringLiteral("req"), QStringLiteral("-x509"),
            QStringLiteral("-newkey"), QStringLiteral("rsa:2048"),
            QStringLiteral("-nodes"),
            QStringLiteral("-keyout"), keyPath,
            QStringLiteral("-out"), pemPath,
            QStringLiteral("-days"), QStringLiteral("1"),
            QStringLiteral("-subj"), QStringLiteral("/CN=127.0.0.1"),
            QStringLiteral("-addext"), QStringLiteral("basicConstraints=critical,CA:FALSE"),
            QStringLiteral("-addext"), QStringLiteral("keyUsage=critical,digitalSignature,keyEncipherment"),
            QStringLiteral("-addext"), QStringLiteral("extendedKeyUsage=serverAuth")
        };
        if (!san.isEmpty())
            args << QStringLiteral("-addext") << QStringLiteral("subjectAltName=") + san;
        generator.start(QStringLiteral("openssl"), args);
        if (!generator.waitForFinished(15000)
            || generator.exitStatus() != QProcess::NormalExit
            || generator.exitCode() != 0)
            return false;

        // Use explicit historical validity dates, rather than -days 0
        // (rejected by some OpenSSL versions and timing-dependent on others).
        // openssl ca supports these dates on older and newer OpenSSL builds.
        const QString finalPath = expired ? pemPath + QStringLiteral(".expired") : pemPath;
        if (expired)
        {
            const auto write = [&dir](const QString &filename, const QByteArray &data) {
                QFile file(dir + QLatin1Char('/') + filename);
                return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
                    && file.write(data) == data.size() && file.flush();
            };
            const QByteArray config = QByteArrayLiteral(
                "[ca]\n"
                "default_ca=local\n"
                "[local]\n"
                "database=index.txt\n"
                "serial=serial\n"
                "new_certs_dir=.\n"
                "default_md=sha256\n"
                "policy=policy\n"
                "[policy]\n"
                "commonName=supplied\n"
                "[server]\n"
                "basicConstraints=critical,CA:FALSE\n"
                "keyUsage=critical,digitalSignature,keyEncipherment\n"
                "extendedKeyUsage=serverAuth\n")
                + (san.isEmpty() ? QByteArray() : QByteArrayLiteral("subjectAltName=")
                    + san.toUtf8() + '\n');
            if (!write(QStringLiteral("ca.cnf"), config)
                || !write(QStringLiteral("index.txt"), QByteArray())
                || !write(QStringLiteral("serial"), QByteArrayLiteral("01\n")))
                return false;
            generator.setWorkingDirectory(dir);
            const QString csr = name + QStringLiteral(".csr");
            const auto run = [&generator](const QStringList &arguments) {
                generator.start(QStringLiteral("openssl"), arguments);
                const bool ok = generator.waitForFinished(15000)
                    && generator.exitStatus() == QProcess::NormalExit
                    && generator.exitCode() == 0;
                if (!ok)
                    qWarning().noquote() << generator.readAllStandardError();
                return ok;
            };
            if (!run({QStringLiteral("x509"), QStringLiteral("-x509toreq"),
                    QStringLiteral("-in"), name + QStringLiteral(".crt"),
                    QStringLiteral("-signkey"), name + QStringLiteral(".key"),
                    QStringLiteral("-out"), csr})
                || !run({QStringLiteral("ca"), QStringLiteral("-selfsign"),
                    QStringLiteral("-batch"), QStringLiteral("-config"), QStringLiteral("ca.cnf"),
                    QStringLiteral("-keyfile"), name + QStringLiteral(".key"),
                    QStringLiteral("-in"), csr,
                    QStringLiteral("-out"), name + QStringLiteral(".crt.expired"),
                    QStringLiteral("-notext"), QStringLiteral("-startdate"), QStringLiteral("20000101000000Z"),
                    QStringLiteral("-enddate"), QStringLiteral("20010101000000Z"),
                    QStringLiteral("-extensions"), QStringLiteral("server")}))
                return false;
        }
        QFile certificateFile(finalPath);
        QFile keyFile(keyPath);
        if (!certificateFile.open(QIODevice::ReadOnly)
            || !keyFile.open(QIODevice::ReadOnly))
            return false;
        certificate = QSslCertificate(&certificateFile, QSsl::Pem);
        key = QSslKey(&keyFile, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
        return !certificate.isNull() && !key.isNull();
    }

    QSslCertificate certificate;
    QSslKey key;
};

class DemoHttpsServer final : public QObject
{
public:
    explicit DemoHttpsServer(QObject *parent = nullptr)
        : QObject(parent)
    {
        // QSslServer exposes a socket in its pending queue only after the
        // TLS handshake has completed. newConnection() can fire earlier.
        // pendingConnectionAvailable() is the documented signal for
        // nextPendingConnection() with QSslServer.
        connect(&m_server, &QTcpServer::pendingConnectionAvailable, this, [this]() {
            while (m_server.hasPendingConnections())
            {
                auto *socket = qobject_cast<QSslSocket *>(m_server.nextPendingConnection());
                if (!socket)
                    continue;
                auto input = std::make_shared<QByteArray>();
                connect(socket, &QSslSocket::readyRead, this,
                    [this, socket, input]() {
                        input->append(socket->readAll());
                        const int split = input->indexOf("\r\n\r\n");
                        if (split < 0)
                            return;
                        const QByteArray headers = input->left(split);
                        int bodyLength = 0;
                        for (const QByteArray &line : headers.split('\n'))
                        {
                            if (line.trimmed().toLower().startsWith("content-length:"))
                                bodyLength = line.mid(line.indexOf(':') + 1).trimmed().toInt();
                        }
                        if (input->size() < split + 4 + bodyLength)
                            return;

                        const QByteArray path = headers.split('\n').first().split(' ').value(1);
                        const bool authenticated = headers.contains("QBT_SID=local-test");
                        QByteArray result;
                        QByteArray additionalHeaders;
                        int status = 200;
                        if (path == "/api/v2/auth/login")
                        {
                            ++m_loginRequests;
                            result = "Ok.";
                            additionalHeaders = "Set-Cookie: QBT_SID=local-test; Path=/; HttpOnly\r\n";
                        }
                        else if (path == "/api/v2/app/version" && authenticated)
                            result = "v5.1.4";
                        else if (path == "/api/v2/app/webapiVersion" && authenticated)
                            result = "2.11.4";
                        else if (path.startsWith("/api/v2/sync/maindata?rid=") && authenticated)
                            result = R"({"rid":1,"full_update":true,"torrents":{},"server_state":{"dl_info_speed":0,"up_info_speed":0}})";
                        else if (path == "/api/v2/auth/logout" && authenticated)
                            result = "Ok.";
                        else
                        {
                            result = "Forbidden";
                            status = 403;
                        }

                        const QByteArray response = "HTTP/1.1 "
                            + QByteArray::number(status)
                            + (status == 200 ? " OK\r\n" : " Forbidden\r\n")
                            + "Content-Length: " + QByteArray::number(result.size())
                            + "\r\nContent-Type: text/plain\r\n"
                            + additionalHeaders
                            + "Connection: close\r\n\r\n"
                            + result;
                        socket->write(response);
                        socket->disconnectFromHost();
                    });
                connect(socket, &QSslSocket::disconnected,
                        socket, &QSslSocket::deleteLater);
            }
        });
    }

    void useCertificate(const EphemeralCertificate &cert)
    {
        QSslConfiguration config = QSslConfiguration::defaultConfiguration();
        config.setLocalCertificate(cert.certificate);
        config.setPrivateKey(cert.key);
        config.setPeerVerifyMode(QSslSocket::VerifyNone);
        m_server.setSslConfiguration(config);
    }

    int loginRequests() const noexcept { return m_loginRequests; }
    bool start() { return m_server.listen(QHostAddress::LocalHost, 0); }
    QString url() const
    {
        return QStringLiteral("https://127.0.0.1:%1").arg(m_server.serverPort());
    }

private:
    QSslServer m_server;
    int m_loginRequests = 0;
};

// Observes real modal dialogs as a GUI user would. It can approve or reject a
// certificate; when no prompt is expected it rejects unexpected prompts so a
// regression cannot stall CTest indefinitely.
class CertificateDialogDriver final : public QObject
{
public:
    enum class Decision { Approve, Reject };
    explicit CertificateDialogDriver(Decision decision, QObject *parent = nullptr)
        : QObject(parent), m_decision(decision) {}

    int prompts() const noexcept { return m_prompts; }
    bool sawChangedCertificate() const noexcept { return m_sawChanged; }
    bool sawIdentityMismatch() const noexcept { return m_sawMismatch; }
    QString certificateText() const { return m_lastText; }

protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (event->type() != QEvent::Show
            || object->objectName() != QStringLiteral("certificateTrustDialog"))
            return QObject::eventFilter(object, event);

        auto *dialog = qobject_cast<QMessageBox *>(object);
        if (!dialog)
            return false;

        ++m_prompts;
        m_sawChanged |= dialog->windowTitle().contains(QStringLiteral("Changed"));
        m_lastText = dialog->informativeText();
        m_sawMismatch |= m_lastText.contains(QStringLiteral("Certificate identity mismatch"));
        const Decision decision = m_decision;
        QTimer::singleShot(0, dialog, [dialog, decision]() {
            for (QAbstractButton *button : dialog->buttons())
            {
                const bool isTrust = button->text().contains(QStringLiteral("Trust This Certificate"));
                if (isTrust == (decision == Decision::Approve))
                {
                    button->click();
                    return;
                }
            }
            dialog->reject();
        });
        return false;
    }

private:
    Decision m_decision;
    int m_prompts = 0;
    bool m_sawChanged = false;
    bool m_sawMismatch = false;
    QString m_lastText;
};

class TlsTrustWorkflowTest final : public QObject
{
    Q_OBJECT

private:
    static void connectThroughMenu(MainWindow &window, const QString &url,
                                   const QString &name = QStringLiteral("TLS test seedbox"),
                                   bool newProfile = false)
    {
        QTimer::singleShot(0, [url, name, newProfile]() {
            auto *dialog = qobject_cast<ConnectionDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            if (newProfile)
                dialog->findChild<QPushButton *>(QStringLiteral("newServer"))->click();
            else
            {
                auto *profiles = dialog->findChild<QComboBox *>(QStringLiteral("savedServers"));
                const int existing = profiles->findText(name);
                if (existing >= 0)
                    profiles->setCurrentIndex(existing);
            }
            dialog->findChild<QLineEdit *>(QStringLiteral("serverName"))->setText(name);
            dialog->findChild<QLineEdit *>(QStringLiteral("serverUrl"))->setText(url);
            dialog->findChild<QLineEdit *>(QStringLiteral("serverUsername"))->setText(
                QStringLiteral("demo"));
            dialog->findChild<QLineEdit *>(QStringLiteral("serverPassword"))->setText(
                QStringLiteral("demo-password"));
            QTest::mouseClick(
                dialog->findChild<QPushButton *>(QStringLiteral("connectServer")),
                Qt::LeftButton);
        });
        auto *action = window.findChild<QAction *>(QStringLiteral("connectToServer"));
        if (action)
            action->trigger();
    }

private:
    static bool waitForStatus(MainWindow &window, const QString &expected)
    {
        QElapsedTimer elapsed;
        elapsed.start();
        while (elapsed.elapsed() < 7000)
        {
            const QString message = window.statusBar()->currentMessage();
            if (message.contains(expected))
                return true;
            // An unexpected terminal failure is already conclusive:
            // report the actual status instead of waiting for the timeout.
            if (message.contains(QStringLiteral(" failed:")))
                return false;
            QTest::qWait(20);
        }
        return false;
    }

private slots:
    void initTestCase()
    {
        if (!QSslSocket::supportsSsl())
            QSKIP("Qt Network has no TLS backend.");
        if (QStandardPaths::findExecutable(QStringLiteral("openssl")).isEmpty())
            QSKIP("OpenSSL command-line tool unavailable for ephemeral test certificates.");
    }

    void userApprovesAndReconnectsWithoutReprompt()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        EphemeralCertificate certificate;
        QVERIFY(certificate.generate(temp.path(), QStringLiteral("first"),
            QStringLiteral("IP:127.0.0.1")));
        DemoHttpsServer server;
        server.useCertificate(certificate);
        QVERIFY(server.start());

        QSettings settings(temp.filePath(QStringLiteral("profiles.ini")), QSettings::IniFormat);
        {
            MainWindow window(nullptr, &settings);
            window.show();
            CertificateDialogDriver driver(CertificateDialogDriver::Decision::Approve);
            qApp->installEventFilter(&driver);
            connectThroughMenu(window, server.url());
            QVERIFY2(waitForStatus(window, QStringLiteral("Connected to TLS test seedbox")),
                qPrintable(window.statusBar()->currentMessage()));
            QCOMPARE(driver.prompts(), 1);
            QVERIFY(driver.certificateText().contains(QStringLiteral("SHA-256")));
            qApp->removeEventFilter(&driver);
        }

        Remote::ProfileStore store(settings);
        const auto items = store.profiles();
        QCOMPARE(items.size(), 1);
        const QString fingerprint = store.trustedCertificateFingerprint(items.constFirst());
        QCOMPARE(fingerprint, QString::fromLatin1(
            certificate.certificate.digest(QCryptographicHash::Sha256).toHex()));

        // A new session must load the saved, endpoint-scoped pin. No second
        // trust prompt is necessary for the exact same valid certificate.
        MainWindow reopened(nullptr, &settings);
        reopened.show();
        CertificateDialogDriver driver(CertificateDialogDriver::Decision::Reject);
        qApp->installEventFilter(&driver);
        connectThroughMenu(reopened, server.url());
        QVERIFY2(waitForStatus(reopened, QStringLiteral("Connected to TLS test seedbox")),
                qPrintable(reopened.statusBar()->currentMessage()));
        QCOMPARE(driver.prompts(), 0);
        qApp->removeEventFilter(&driver);
    }

    void userApprovesLegacySelfSignedWithoutSan()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        EphemeralCertificate certificate;
        QVERIFY(certificate.generate(temp.path(), QStringLiteral("legacy-no-san"), QString()));
        QVERIFY(certificate.certificate.subjectAlternativeNames().isEmpty());
        QVERIFY(certificate.certificate.subjectInfo(QSslCertificate::CommonName)
            .contains(QStringLiteral("127.0.0.1")));

        DemoHttpsServer server;
        server.useCertificate(certificate);
        QVERIFY(server.start());
        QSettings settings(temp.filePath(QStringLiteral("profiles.ini")), QSettings::IniFormat);

        {
            MainWindow window(nullptr, &settings);
            window.show();
            CertificateDialogDriver driver(CertificateDialogDriver::Decision::Approve);
            qApp->installEventFilter(&driver);
            connectThroughMenu(window, server.url());
            QVERIFY2(waitForStatus(window, QStringLiteral("Connected to TLS test seedbox")),
                     qPrintable(window.statusBar()->currentMessage()));
            QCOMPARE(driver.prompts(), 1);
            QVERIFY(driver.certificateText().contains(QStringLiteral("Legacy certificate")));
            qApp->removeEventFilter(&driver);
        }

        Remote::ProfileStore store(settings);
        const auto saved = store.profiles();
        QCOMPARE(saved.size(), 1);
        QCOMPARE(store.trustedCertificateFingerprint(saved.first()),
            QString::fromLatin1(certificate.certificate.digest(
                QCryptographicHash::Sha256).toHex()));
    }

    void userRejectsUnexpectedCertificateReplacement()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        EphemeralCertificate original, replacement;
        QVERIFY(original.generate(temp.path(), QStringLiteral("original"),
            QStringLiteral("IP:127.0.0.1")));
        QVERIFY(replacement.generate(temp.path(), QStringLiteral("replacement"),
            QStringLiteral("IP:127.0.0.1")));

        DemoHttpsServer server;
        server.useCertificate(original);
        QVERIFY(server.start());
        QSettings settings(temp.filePath(QStringLiteral("profiles.ini")), QSettings::IniFormat);

        {
            MainWindow window(nullptr, &settings);
            window.show();
            CertificateDialogDriver driver(CertificateDialogDriver::Decision::Approve);
            qApp->installEventFilter(&driver);
            connectThroughMenu(window, server.url());
            QVERIFY2(waitForStatus(window, QStringLiteral("Connected to TLS test seedbox")),
                qPrintable(window.statusBar()->currentMessage()));
            qApp->removeEventFilter(&driver);
        }
        Remote::ProfileStore store(settings);
        const auto saved = store.profiles();
        QCOMPARE(saved.size(), 1);
        const QString originalFingerprint = store.trustedCertificateFingerprint(saved.first());

        server.useCertificate(replacement);
        {
            MainWindow window(nullptr, &settings);
            window.show();
            CertificateDialogDriver driver(CertificateDialogDriver::Decision::Reject);
            qApp->installEventFilter(&driver);
            connectThroughMenu(window, server.url());
            QVERIFY2(waitForStatus(window, QStringLiteral("trust declined")),
                qPrintable(window.statusBar()->currentMessage()));
            QCOMPARE(driver.prompts(), 1);
            QVERIFY(driver.sawChangedCertificate());
            QVERIFY(driver.certificateText().contains(originalFingerprint.toUpper()));
            qApp->removeEventFilter(&driver);
        }
        QCOMPARE(store.trustedCertificateFingerprint(saved.first()), originalFingerprint);

        // A new, *explicit* approval replaces the remembered fingerprint.
        MainWindow approved(nullptr, &settings);
        approved.show();
        CertificateDialogDriver driver(CertificateDialogDriver::Decision::Approve);
        qApp->installEventFilter(&driver);
        connectThroughMenu(approved, server.url());
        QVERIFY2(waitForStatus(approved, QStringLiteral("Connected to TLS test seedbox")),
                qPrintable(approved.statusBar()->currentMessage()));
        QCOMPARE(driver.prompts(), 1);
        QCOMPARE(store.trustedCertificateFingerprint(saved.first()),
                 QString::fromLatin1(replacement.certificate.digest(
                     QCryptographicHash::Sha256).toHex()));
        qApp->removeEventFilter(&driver);
    }

    void mismatchedCertificateRequiresExplicitEndpointTrust()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        EphemeralCertificate wrongHost, replacement;
        QVERIFY(wrongHost.generate(temp.path(), QStringLiteral("wrong-host"),
            QStringLiteral("DNS:not-localhost.example")));
        QVERIFY(replacement.generate(temp.path(), QStringLiteral("replacement-wrong-host"),
            QStringLiteral("DNS:another-host.example")));
        // The legacy CN matches the IP, but a conflicting SAN takes
        // precedence. Only explicit fingerprint trust may waive this.
        QVERIFY(wrongHost.certificate.subjectInfo(QSslCertificate::CommonName)
            .contains(QStringLiteral("127.0.0.1")));
        QVERIFY(wrongHost.certificate.subjectAlternativeNames()
            .values(QSsl::IpAddressEntry).isEmpty());
        DemoHttpsServer server;
        server.useCertificate(wrongHost);
        QVERIFY(server.start());
        QSettings settings(temp.filePath(QStringLiteral("profiles.ini")), QSettings::IniFormat);
        Remote::ProfileStore store(settings);

        {
            MainWindow window(nullptr, &settings);
            window.show();
            CertificateDialogDriver driver(CertificateDialogDriver::Decision::Reject);
            qApp->installEventFilter(&driver);
            connectThroughMenu(window, server.url());
            QVERIFY2(waitForStatus(window, QStringLiteral("trust declined")),
                qPrintable(window.statusBar()->currentMessage()));
            QCOMPARE(driver.prompts(), 1);
            QVERIFY(driver.sawIdentityMismatch());
            QCOMPARE(server.loginRequests(), 0);
            QCOMPARE(store.profiles().size(), 1);
            QVERIFY(store.trustedCertificateFingerprint(store.profiles().first()).isEmpty());
        }
        {
            MainWindow window(nullptr, &settings);
            window.show();
            CertificateDialogDriver driver(CertificateDialogDriver::Decision::Approve);
            qApp->installEventFilter(&driver);
            connectThroughMenu(window, server.url());
            QVERIFY2(waitForStatus(window, QStringLiteral("Connected to TLS test seedbox")),
                qPrintable(window.statusBar()->currentMessage()));
            QCOMPARE(driver.prompts(), 1);
            QVERIFY(driver.sawIdentityMismatch());
            QCOMPARE(server.loginRequests(), 1);
        }
        const auto profile = store.profiles().first();
        const QString pin = QString::fromLatin1(wrongHost.certificate.digest(
            QCryptographicHash::Sha256).toHex());
        QCOMPARE(store.trustedCertificateFingerprint(profile), pin);
        {
            MainWindow window(nullptr, &settings);
            window.show();
            CertificateDialogDriver driver(CertificateDialogDriver::Decision::Reject);
            qApp->installEventFilter(&driver);
            connectThroughMenu(window, server.url());
            QVERIFY2(waitForStatus(window, QStringLiteral("Connected to TLS test seedbox")),
                qPrintable(window.statusBar()->currentMessage()));
            QCOMPARE(driver.prompts(), 0);
            QCOMPARE(server.loginRequests(), 2);
        }
        // A different endpoint must not inherit trust, even when presenting
        // the same certificate (e.g. packages shipping a common default).
        DemoHttpsServer other;
        other.useCertificate(wrongHost);
        QVERIFY(other.start());
        {
            MainWindow window(nullptr, &settings);
            window.show();
            CertificateDialogDriver driver(CertificateDialogDriver::Decision::Reject);
            qApp->installEventFilter(&driver);
            connectThroughMenu(window, other.url(), QStringLiteral("Other TLS seedbox"), true);
            QVERIFY2(waitForStatus(window, QStringLiteral("trust declined")),
                qPrintable(window.statusBar()->currentMessage()));
            QCOMPARE(driver.prompts(), 1);
            QVERIFY(driver.sawIdentityMismatch());
            QCOMPARE(other.loginRequests(), 0);
        }
        QCOMPARE(store.profiles().size(), 2);
        for (const auto &saved : store.profiles())
            if (saved.id != profile.id)
                QVERIFY(store.trustedCertificateFingerprint(saved).isEmpty());
        QCOMPARE(store.trustedCertificateFingerprint(profile), pin);
        server.useCertificate(replacement);
        {
            MainWindow window(nullptr, &settings);
            window.show();
            CertificateDialogDriver driver(CertificateDialogDriver::Decision::Reject);
            qApp->installEventFilter(&driver);
            connectThroughMenu(window, server.url());
            QVERIFY2(waitForStatus(window, QStringLiteral("trust declined")),
                qPrintable(window.statusBar()->currentMessage()));
            QCOMPARE(driver.prompts(), 1);
            QVERIFY(driver.sawChangedCertificate() && driver.sawIdentityMismatch());
            QVERIFY(driver.certificateText().contains(pin.toUpper()));
            QCOMPARE(server.loginRequests(), 2);
        }
        QCOMPARE(store.trustedCertificateFingerprint(profile), pin);
    }

    void expiredCertificateCannotBeApprovedEvenWithStoredPin()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        EphemeralCertificate expired;
        QVERIFY(expired.generate(temp.path(), QStringLiteral("expired"),
            QStringLiteral("DNS:not-localhost.example"), true));
        QTRY_VERIFY_WITH_TIMEOUT(expired.certificate.expiryDate() < QDateTime::currentDateTimeUtc(), 3000);
        DemoHttpsServer server;
        server.useCertificate(expired);
        QVERIFY(server.start());
        QSettings settings(temp.filePath(QStringLiteral("profiles.ini")), QSettings::IniFormat);
        {
            MainWindow window(nullptr, &settings);
            window.show();
            CertificateDialogDriver driver(CertificateDialogDriver::Decision::Approve);
            qApp->installEventFilter(&driver);
            connectThroughMenu(window, server.url());
            QVERIFY2(waitForStatus(window, QStringLiteral("expired")),
                qPrintable(window.statusBar()->currentMessage()));
            QCOMPARE(driver.prompts(), 0);
            QCOMPARE(server.loginRequests(), 0);
        }
        Remote::ProfileStore store(settings);
        QCOMPARE(store.profiles().size(), 1);
        const auto profile = store.profiles().first();
        QVERIFY(store.trustCertificate(profile, QString::fromLatin1(
            expired.certificate.digest(QCryptographicHash::Sha256).toHex())));
        {
            MainWindow window(nullptr, &settings);
            window.show();
            CertificateDialogDriver driver(CertificateDialogDriver::Decision::Approve);
            qApp->installEventFilter(&driver);
            connectThroughMenu(window, server.url());
            QVERIFY2(waitForStatus(window, QStringLiteral("expired")),
                qPrintable(window.statusBar()->currentMessage()));
            QCOMPARE(driver.prompts(), 0);
            QCOMPARE(server.loginRequests(), 0);
        }
    }

    void userCanCancelAnUnresponsiveConnection()
    {
        QTcpServer unresponsive;
        QVERIFY(unresponsive.listen(QHostAddress::LocalHost, 0));
        // Keep accepted sockets open but send no response to login.
        connect(&unresponsive, &QTcpServer::newConnection, &unresponsive,
            [&unresponsive]() {
                while (unresponsive.hasPendingConnections())
                    unresponsive.nextPendingConnection();
            });

        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("profiles.ini")), QSettings::IniFormat);
        MainWindow window(nullptr, &settings);
        window.show();
        const QString address = QStringLiteral("http://127.0.0.1:%1")
            .arg(unresponsive.serverPort());
        connectThroughMenu(window, address);
        QTRY_VERIFY(window.statusBar()->currentMessage()
            .contains(QStringLiteral("Connecting")));
        auto *disconnect = window.findChild<QAction *>(QStringLiteral("disconnectServer"));
        QVERIFY(disconnect && disconnect->isEnabled());
        disconnect->trigger();
        QTRY_VERIFY(window.statusBar()->currentMessage()
            .contains(QStringLiteral("Not connected")));
        QTest::qWait(100);
        QVERIFY(!window.statusBar()->currentMessage()
            .contains(QStringLiteral("Connection to")));
    }
};

// Preserve QtTest's native QApplication and test setup for this suite.
// The shared dispatcher selects this entry point before Qt is initialized.
#define main runTlsTrustWorkflow
QTEST_MAIN(TlsTrustWorkflowTest)
#undef main
#include "test_tls_trust_workflow.moc"
