// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/MainWindow.h"
#include "gui/ServerPane.h"
#include "gui/ConnectionDialog.h"
#include "gui/RemoteAddTorrentDialog.h"
#include "gui/upstream/transferlistdelegate.h"
#include "gui/upstream/proptabbar.h"
#include "api/CredentialVault.h"
#include "api/RemoteSession.h"
#include "api/TorrentMetadataReader.h"

#include <QAction>
#include <QApplication>
#include <QEvent>
#include <QDateTime>
#include <QCheckBox>
#include <QButtonGroup>
#include <QFile>
#include <QFileDialog>
#include <QFontInfo>
#include <QHash>
#include <QGroupBox>
#include <QComboBox>
#include <QHeaderView>
#include <QHostAddress>
#include <QLineEdit>
#include <QMessageBox>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QStatusBar>
#include <QTabWidget>
#include <QTabBar>
#include <QTableView>
#include <QAbstractItemModel>
#include <QListWidget>
#include <QLabel>
#include <QInputDialog>
#include <QPlainTextEdit>
#include <QDialogButtonBox>
#include <QImage>
#include <QProgressBar>
#include <QTableWidget>
#include <QTreeWidget>
#include <QPainter>
#include <QStyleOptionViewItem>
#include <QSplitter>
#include <QPair>
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
                            ++m_syncRequests;
                            const QUrlQuery query(QString::fromLatin1(path.mid(path.indexOf('?') + 1)));
                            const int rid = query.queryItemValue(QStringLiteral("rid")).toInt();
                            if (!m_existingTorrentHash.isEmpty())
                            {
                                if (rid == 0)
                                    response = QByteArrayLiteral("{\"rid\":1,\"full_update\":true,\"torrents\":{\"")
                                        + m_existingTorrentHash.toUtf8()
                                        + QByteArrayLiteral("\":{\"name\":\"Already present\",\"progress\":1,\"state\":\"uploading\",\"private\":")
                                        + (m_existingTorrentPrivate ? QByteArrayLiteral("true")
                                                                    : QByteArrayLiteral("false"))
                                        + QByteArrayLiteral("}}}");
                                else
                                    response = R"({"rid":2,"full_update":false,"server_state":{"dl_info_speed":0}})";
                            }
                            else if (m_demoNativePresentation)
                            {
                                // Two real server-originated states: completed seeding
                                // with unavailable swarm information, and an incomplete
                                // torrent with 2.5 distributed copies.
                                if (rid == 0)
                                    response = R"({"rid":1,"full_update":true,"torrents":{"0000000000000000000000000000000000000001":{"name":"Complete seed","size":1024,"progress":1.0,"state":"stalledUP","availability":-1},"0000000000000000000000000000000000000002":{"name":"Partial download","size":1024,"progress":0.5,"state":"downloading","availability":2.5}},"server_state":{"dl_info_speed":0,"up_info_speed":0}})";
                                else
                                    response = R"({"rid":2,"full_update":false,"server_state":{"dl_info_speed":0}})";
                            }
                            else if (!m_demoTorrents)
                                response = R"({"rid":1,"full_update":true,"torrents":{},"server_state":{"dl_info_speed":0,"up_info_speed":0}})";
                            else if (rid == 0)
                            {
                                response = R"({"rid":1,"full_update":true,"torrents":{"0123456789abcdef0123456789abcdef01234567":{"name":"Demo Linux ISO","size":1048576,"progress":0.25,"state":"downloading","dlspeed":2048,"upspeed":1024,"category":"linux"}},"server_state":{"dl_info_speed":2048,"up_info_speed":1024}})";
                                if (m_queueingEnabled)
                                    response.replace(QByteArrayLiteral("\"up_info_speed\":1024}"),
                                        QByteArrayLiteral("\"up_info_speed\":1024,\"queueing\":true}"));
                            }
                            else if (rid == 1)
                                response = R"({"rid":2,"full_update":false,"torrents":{"0123456789abcdef0123456789abcdef01234567":{"name":"Updated Linux ISO","progress":0.5,"dlspeed":4096}},"server_state":{"dl_info_speed":4096}})";
                            else if (m_keepDemoTorrents)
                                response = R"({"rid":3,"full_update":false,"server_state":{"dl_info_speed":0}})";
                            else
                                response = R"({"rid":3,"full_update":false,"torrents_removed":["0123456789abcdef0123456789abcdef01234567"],"server_state":{"dl_info_speed":0,"up_info_speed":0}})";
                        }
                        else if (m_demoDetails && hasCookie
                                 && (path.startsWith("/api/v2/torrents/properties?")
                                     || path.startsWith("/api/v2/torrents/trackers?")
                                     || path.startsWith("/api/v2/sync/torrentPeers?")
                                     || path.startsWith("/api/v2/torrents/webseeds?")
                                     || path.startsWith("/api/v2/torrents/files?")))
                        {
                            m_detailRequests.append(path);
                            if (path.startsWith("/api/v2/torrents/properties?"))
                                response = R"DEMO({"save_path":"/share/NAS/torrents","comment":"Get it at https://example.test/files?id=7&part=2 and <b>plain markup</b>; javascript:alert(1)","total_downloaded":1048576,"total_uploaded":2048,"total_size":1048576,"time_elapsed":3600,"eta":0,"share_ratio":1.25,"pieces_num":4,"seeds":2,"peers":1})DEMO";
                            else if (path.startsWith("/api/v2/torrents/trackers?"))
                            {
                                response = R"TRACK([
                                  {"url":"** [DHT] **","tier":-1,"status":0,"disabled_for_torrent":true,"num_peers":0,"num_seeds":0,"num_leeches":0,"num_downloaded":0,"msg":"This torrent is private"},
                                  {"url":"** [PeX] **","tier":-1,"status":0,"disabled_for_torrent":true,"num_peers":0,"num_seeds":0,"num_leeches":0,"num_downloaded":0,"msg":"This torrent is private"},
                                  {"url":"** [LSD] **","tier":-1,"status":0,"disabled_for_torrent":true,"num_peers":0,"num_seeds":0,"num_leeches":0,"num_downloaded":0,"msg":"This torrent is private"},
                                  {"url":"https://tracker.example/announce","tier":0,"status":2,"num_seeds":3,"num_peers":1,"num_leeches":2,"num_downloaded":4,"next_announce":NEXT_EPOCH,"min_announce":MIN_EPOCH,"msg":"Working",
                                    "endpoints":[{"name":"tracker.example:443","bt_version":1,"status":2,"num_seeds":3,"num_peers":1,"num_leeches":2,"num_downloaded":4,"next_announce":NEXT_EPOCH,"min_announce":MIN_EPOCH,"msg":"Announced"}]}
                                ])TRACK";
                                const qint64 now = QDateTime::currentSecsSinceEpoch();
                                response.replace("NEXT_EPOCH", QByteArray::number(now + 780));
                                response.replace("MIN_EPOCH", QByteArray::number(now + 540));
                            }
                            else if (path.startsWith("/api/v2/sync/torrentPeers?"))
                                response = R"({"rid":1,"full_update":true,"peers":{"192.0.2.1:61413":{"client":"qBittorrent","country_code":"US","progress":0.75,"dl_speed":1024,"up_speed":2048,"flags":"D","connection":"TCP"}}})";
                            else if (path.startsWith("/api/v2/torrents/webseeds?"))
                                response = R"([{"url":"https://mirror.example/file.iso"}])";
                            else
                                response = R"([{"index":0,"name":"data/file-one.bin","size":512,"progress":1,"priority":1},{"index":1,"name":"data/file-two.bin","size":512,"progress":0.5,"priority":6}])";
                        }
                        else if (path.startsWith("/api/v2/torrents/") && hasCookie)
                        {
                            m_commands.append({path, body});
                            code = m_rejectCommands ? 400 : 200;
                            response = m_rejectCommands ? QByteArrayLiteral("Rejected")
                                                        : QByteArrayLiteral("Ok.");
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
    int syncRequests() const noexcept { return m_syncRequests; }
    void enableTorrentSyncDemo() { m_demoTorrents = true; }
    void enableQueueingDemo() { m_queueingEnabled = true; }
    void setExistingTorrentHash(const QString &hash) { m_existingTorrentHash = hash; }
    void setExistingTorrentPrivate(bool value) { m_existingTorrentPrivate = value; }
    void enableNativePresentationDemo() { m_demoNativePresentation = true; }
    void enableDetailsDemo() { m_demoDetails = true; }
    int detailsFor(const QByteArray &pathPrefix) const
    {
        int count = 0;
        for (const QByteArray &requested : m_detailRequests)
            count += requested.startsWith(pathPrefix);
        return count;
    }
    QByteArray lastDetailRequest() const
    {
        return m_detailRequests.isEmpty() ? QByteArray() : m_detailRequests.last();
    }
    void keepDemoTorrents() { m_keepDemoTorrents = true; }
    void rejectTorrentCommands() { m_rejectCommands = true; }
    int commandsFor(const QByteArray &endpoint) const
    {
        int count = 0;
        for (const auto &request : m_commands)
            count += request.first == endpoint;
        return count;
    }
    QByteArray lastCommandBody(const QByteArray &endpoint) const
    {
        for (auto it = m_commands.crbegin(); it != m_commands.crend(); ++it)
        {
            if (it->first == endpoint)
                return it->second;
        }
        return {};
    }

private:
    QTcpServer m_server;
    bool m_acceptPassword;
    bool m_acceptBlank;
    int m_logouts = 0;
    bool m_demoTorrents = false;
    bool m_queueingEnabled = false;
    QString m_existingTorrentHash;
    bool m_existingTorrentPrivate = false;
    bool m_demoNativePresentation = false;
    bool m_demoDetails = false;
    QList<QByteArray> m_detailRequests;
    bool m_keepDemoTorrents = false;
    bool m_rejectCommands = false;
    int m_syncRequests = 0;
    QList<QPair<QByteArray, QByteArray>> m_commands;
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

class ConfirmTorrentRemoval final : public QObject
{
public:
    int prompts = 0;
protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (event->type() == QEvent::Show)
        {
            auto *dialog = qobject_cast<QMessageBox *>(object);
            if (dialog && dialog->windowTitle() == QStringLiteral("Remove torrents"))
            {
                ++prompts;
                QTimer::singleShot(0, dialog, [dialog]() {
                    if (auto *yes = dialog->button(QMessageBox::Yes))
                        yes->click();
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

// Operate the real file chooser, native Add Torrent dialog, and duplicate
// confirmation as a person would, against a disposable authenticated server.
class MergeDuplicateTorrentUi final : public QObject
{
public:
    explicit MergeDuplicateTorrentUi(QString fileName) : m_fileName(std::move(fileName)) {}
    int chosenFiles = 0;
    int addDialogs = 0;
    int mergePrompts = 0;
    int privateWarnings = 0;

protected:
    bool eventFilter(QObject *obj, QEvent *event) override
    {
        if (event->type() != QEvent::Show)
            return QObject::eventFilter(obj, event);
        if (auto *picker = qobject_cast<QFileDialog *>(obj);
            picker && picker->objectName() == QLatin1String("addTorrentFileDialog"))
        {
            ++chosenFiles;
            const QString path = m_fileName;
            QTimer::singleShot(0, picker, [picker, path]() {
                picker->selectFile(path);
                // QFileDialog::accept() is protected in Qt. Dispatch through
                // the public QDialog interface, preserving its override.
                static_cast<QDialog *>(picker)->accept();
            });
        }
        else if (auto *dialog = qobject_cast<RemoteAddTorrentDialog *>(obj))
        {
            ++addDialogs;
            QTimer::singleShot(0, dialog, [dialog]() {
                auto *buttons = dialog->findChild<QDialogButtonBox *>(
                    QStringLiteral("buttonBox"));
                if (buttons && buttons->button(QDialogButtonBox::Ok))
                    buttons->button(QDialogButtonBox::Ok)->click();
            });
        }
        else if (auto *prompt = qobject_cast<QMessageBox *>(obj))
        {
            if (prompt->objectName() == QLatin1String("duplicateTorrentTrackersDialog"))
            {
                ++mergePrompts;
                QTimer::singleShot(0, prompt, [prompt]() {
                    if (auto *yes = prompt->button(QMessageBox::Yes))
                        yes->click();
                });
            }
            else if (prompt->windowTitle() == QLatin1String("Torrent already exists")
                     && prompt->text().contains(QLatin1String("private torrent")))
            {
                ++privateWarnings;
                QTimer::singleShot(0, prompt, [prompt]() {
                    if (auto *ok = prompt->button(QMessageBox::Ok))
                        ok->click();
                });
            }
        }
        return QObject::eventFilter(obj, event);
    }
private:
    QString m_fileName;
};

// Drive the visible multi-select file picker and Add Torrent dialog.
// The assertions observe UI controls and the remote HTTP request, not an
// internal addTorrentFiles() call.
class AddBatchTorrentUi final : public QObject
{
public:
    AddBatchTorrentUi(QString directory, QStringList filenames)
        : m_directory(std::move(directory)), m_filenames(std::move(filenames)) {}

    int filePickers = 0;
    int addDialogs = 0;
    QString error;

protected:
    bool eventFilter(QObject *obj, QEvent *event) override
    {
        if (event->type() != QEvent::Show)
            return QObject::eventFilter(obj, event);
        if (auto *picker = qobject_cast<QFileDialog *>(obj);
            picker && picker->objectName() == QLatin1String("addTorrentFileDialog"))
        {
            ++filePickers;
            QTimer::singleShot(0, picker, [this, picker]() {
                if (picker->fileMode() != QFileDialog::ExistingFiles)
                {
                    error = QStringLiteral("File picker does not allow multiple torrent selection");
                    picker->reject();
                    return;
                }
                picker->setDirectory(m_directory);
                auto *line = picker->findChild<QLineEdit *>(
                    QStringLiteral("fileNameEdit"));
                if (!line)
                {
                    error = QStringLiteral("Native Qt filename input missing");
                    picker->reject();
                    return;
                }
                // Qt's ExistingFiles picker accepts quoted filenames from
                // its own filename input, exactly as in the visible UI.
                QStringList quoted;
                for (const QString &name : m_filenames)
                    quoted.append(QLatin1Char('"') + name + QLatin1Char('"'));
                line->setText(quoted.join(QLatin1Char(' ')));
                if (picker->selectedFiles().size() != m_filenames.size())
                {
                    error = QStringLiteral("Multi-select picker did not select all seven files");
                    picker->reject();
                    return;
                }
                static_cast<QDialog *>(picker)->accept();
            });
        }
        else if (auto *dialog = qobject_cast<RemoteAddTorrentDialog *>(obj))
        {
            ++addDialogs;
            QTimer::singleShot(0, dialog, [this, dialog]() {
                auto *group = dialog->findChild<QGroupBox *>(
                    QStringLiteral("torrentBatchGroup"));
                auto *names = dialog->findChild<QPlainTextEdit *>(
                    QStringLiteral("torrentBatchFileList"));
                auto *category = dialog->findChild<QComboBox *>(
                    QStringLiteral("categoryComboBox"));
                auto *save = dialog->findChild<QComboBox *>(
                    QStringLiteral("remoteSavePath"));
                auto *start = dialog->findChild<QCheckBox *>(
                    QStringLiteral("startTorrentCheckBox"));
                auto *buttons = dialog->findChild<QDialogButtonBox *>(
                    QStringLiteral("buttonBox"));
                if (!group || !names || !names->isReadOnly()
                    || names->toPlainText().split(QLatin1Char('\n')).size() != m_filenames.size()
                    || !category || !save || !start || !buttons
                    || !buttons->button(QDialogButtonBox::Ok))
                {
                    error = QStringLiteral("User did not see seven files and shared add options");
                    dialog->reject();
                    return;
                }
                for (const QString &name : m_filenames)
                    if (!names->toPlainText().contains(name))
                        error = QStringLiteral("Missing selected file: ") + name;
                if (!error.isEmpty()) { dialog->reject(); return; }
                category->setEditText(QStringLiteral("Queue Test"));
                save->setEditText(QStringLiteral("/share/QueueTest"));
                start->setChecked(true);
                buttons->button(QDialogButtonBox::Ok)->click();
            });
        }
        else if (auto *warning = qobject_cast<QMessageBox *>(obj))
        {
            error = QStringLiteral("Unexpected user-visible warning: ")
                + warning->windowTitle() + QLatin1String(": ") + warning->text();
            QTimer::singleShot(0, warning, &QMessageBox::reject);
        }
        return QObject::eventFilter(obj, event);
    }
private:
    QString m_directory;
    QStringList m_filenames;
};

class ConnectionWorkflowTest final : public QObject
{
    Q_OBJECT

private:
    void openConnectDialog(MainWindow &window, const QString &url,
                           bool remember = false, bool enterPassword = true,
                           bool createNew = false,
                           const QString &displayName = QStringLiteral("Demo seedbox"))
    {
        QTimer::singleShot(0, [url, remember, enterPassword, createNew, displayName]() {
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

            if (createNew)
            {
                auto *newButton = dialog->findChild<QPushButton *>(QStringLiteral("newServer"));
                if (!newButton)
                    return;
                QTest::mouseClick(newButton, Qt::LeftButton);
            }
            name->setText(displayName);
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
    void mainWindowAndEachServerRestoreNativeLayout()
    {
        DemoQbittorrentServer alpha(true, nullptr, true);
        DemoQbittorrentServer beta(true, nullptr, true);
        QVERIFY(alpha.start());
        QVERIFY(beta.start());

        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("layout.ini")), QSettings::IniFormat);
        DemoCredentialVault vault;
        Remote::ProfileStore store(settings);
        Remote::ServerProfile first {
            QStringLiteral("alpha-profile"), QStringLiteral("Alpha"),
            alpha.url(), QStringLiteral("demo"), false, true
        };
        Remote::ServerProfile second {
            QStringLiteral("beta-profile"), QStringLiteral("Beta"),
            beta.url(), QStringLiteral("demo"), false, true
        };
        QVERIFY(store.save(first));
        QVERIFY(store.save(second));
        QByteArray originalGeometry;

        {
            MainWindow window(nullptr, &settings, &vault);
            window.show();
            QTRY_VERIFY(window.isVisible());
            auto *servers = window.findChild<QTabWidget *>(QStringLiteral("serverTabs"));
            QVERIFY(servers);
            QTRY_COMPARE_WITH_TIMEOUT(servers->count(), 2, 8000);

            servers->setCurrentIndex(1);
            QVERIFY(qobject_cast<ServerPane *>(servers->currentWidget()));
            QCOMPARE(servers->tabText(servers->currentIndex()), QStringLiteral("Beta"));
            auto *pane = qobject_cast<ServerPane *>(servers->currentWidget());
            QVERIFY(pane);
            auto *torrentHeader = pane->table()->horizontalHeader();
            QVERIFY(torrentHeader);
            torrentHeader->resizeSection(1, 173);
            QCOMPARE(torrentHeader->sectionSize(1), 173);
            auto *tabs = pane->findChild<PropTabBar *>();
            QVERIFY(tabs);
            tabs->setCurrentIndex(4); // Content page
            QCOMPARE(tabs->currentIndex(), 4);
            tabs->setCurrentIndex(4); // collapse, keeping last page
            QCOMPARE(tabs->currentIndex(), -1);

            // Also change the other server's header to ensure layouts are
            // kept independently, not shared between session tabs.
            servers->setCurrentIndex(0);
            auto *firstPane = qobject_cast<ServerPane *>(servers->currentWidget());
            QVERIFY(firstPane);
            firstPane->table()->horizontalHeader()->resizeSection(1, 124);
            servers->setCurrentIndex(1);
            window.resize(1230, 790);
            QCoreApplication::processEvents();
            originalGeometry = window.saveGeometry();
            QVERIFY(window.close());
            settings.sync();
        }

        QVERIFY(settings.contains(QStringLiteral("Layout/MainWindow/Geometry")));
        QVERIFY(settings.contains(QStringLiteral("Layout/MainWindow/State")));
        QVERIFY(settings.contains(QStringLiteral("Layout/MainWindow/Maximized")));
        QVERIFY(settings.contains(QStringLiteral("Layout/MainWindow/Minimized")));
        QCOMPARE(settings.value(QStringLiteral("Layout/ActiveServer")).toString(),
                 QStringLiteral("beta-profile"));
        QVERIFY(!settings.value(QStringLiteral("Layout/MainWindow/Geometry"))
                       .toByteArray().isEmpty());

        {
            MainWindow window(nullptr, &settings, &vault);
            window.show();
            QTRY_VERIFY(window.isVisible());
            auto *servers = window.findChild<QTabWidget *>(QStringLiteral("serverTabs"));
            QVERIFY(servers);
            QTRY_COMPARE_WITH_TIMEOUT(servers->count(), 2, 8000);
            QCOMPARE(servers->tabText(servers->currentIndex()), QStringLiteral("Beta"));

            auto *pane = qobject_cast<ServerPane *>(servers->currentWidget());
            QVERIFY(pane);
            QTRY_COMPARE(pane->table()->horizontalHeader()->sectionSize(1), 173);
            auto *tabs = pane->findChild<PropTabBar *>();
            QVERIFY(tabs);
            QTRY_COMPARE(tabs->currentIndex(), -1);
            tabs->setCurrentIndex(4);
            QCOMPARE(tabs->currentIndex(), 4);
            QVERIFY(pane->findChild<QWidget *>(
                QStringLiteral("remoteTorrentProperties"))->isVisible());

            servers->setCurrentIndex(0);
            auto *firstPane = qobject_cast<ServerPane *>(servers->currentWidget());
            QVERIFY(firstPane);
            QCOMPARE(firstPane->table()->horizontalHeader()->sectionSize(1), 124);
            QVERIFY(window.close());
        }
    }

    void connectionDialogGeometryUsesSameQSettingsFile()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("layout.ini")), QSettings::IniFormat);
        DemoCredentialVault vault;
        {
            ConnectionDialog dialog(settings, nullptr, &vault);
            dialog.show();
            QTRY_VERIFY(dialog.isVisible());
            dialog.resize(620, 475);
            QCoreApplication::processEvents();
            dialog.reject();
            settings.sync();
        }
        const QByteArray stored = settings.value(
            QStringLiteral("Layout/Dialogs/Connection")).toByteArray();
        QVERIFY(!stored.isEmpty());
        {
            ConnectionDialog dialog(settings, nullptr, &vault);
            dialog.show();
            QTRY_VERIFY(dialog.isVisible());
            QVERIFY(dialog.size().width() >= 600);
            dialog.reject();
        }
    }

    void globalFontSizeKeepsTorrentListAsFloorAndPersists()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("profiles.ini")), QSettings::IniFormat);
        DemoCredentialVault vault;
        const QFont original = QApplication::font();
        int initialSize = -1;

        {
            MainWindow window(nullptr, &settings, &vault);
            window.show();
            QTRY_VERIFY(window.isVisible());

            auto *increase = window.findChild<QAction *>(QStringLiteral("fontSizeIncrease"));
            auto *decrease = window.findChild<QAction *>(QStringLiteral("fontSizeDecrease"));
            auto *reset = window.findChild<QAction *>(QStringLiteral("fontSizeReset"));
            auto *table = window.findChild<QTableView *>(QStringLiteral("torrentList"));
            auto *trackers = window.findChild<QTreeWidget *>(QStringLiteral("torrentTrackers"));
            auto *content = window.findChild<QTreeWidget *>(QStringLiteral("torrentContent"));
            auto *filterList = window.findChild<QListWidget *>(QStringLiteral("filterList"));
            QVERIFY(increase && decrease && reset);
            QVERIFY(table && trackers && content && filterList);
            initialSize = window.fontPointSize();
            QVERIFY(initialSize > 0);
            QCOMPARE(QApplication::font(), original);
            QVERIFY(increase->shortcuts().contains(QKeySequence(Qt::CTRL | Qt::Key_Plus)));
            QVERIFY(increase->shortcuts().contains(QKeySequence(Qt::CTRL | Qt::Key_Equal)));
            QCOMPARE(decrease->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_Minus));
            QCOMPARE(reset->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_0));

            QVERIFY(!decrease->isEnabled());
            QVERIFY(!reset->isEnabled());
            increase->trigger();
            QCOMPARE(window.fontPointSize(), initialSize + 1);
            QCOMPARE(QApplication::font().pointSize(), initialSize + 1);
            QCOMPARE(table->font().pointSize(), initialSize + 1);
            QCOMPARE(trackers->font().pointSize(), initialSize + 1);
            QCOMPARE(content->font().pointSize(), initialSize + 1);
            QCOMPARE(filterList->font().pointSize(), initialSize + 1);
            QVERIFY(filterList->height() >=
                    filterList->count() * filterList->fontMetrics().height());

            settings.sync();
            QCOMPARE(settings.value(QStringLiteral("Appearance/FontPointSize")).toInt(),
                     initialSize + 1);

            decrease->trigger();
            QCOMPARE(window.fontPointSize(), initialSize);
            QVERIFY(!decrease->isEnabled());
            decrease->trigger(); // cannot shrink below the torrent-list baseline
            QCOMPARE(window.fontPointSize(), initialSize);
            increase->trigger();
        }
        {
            MainWindow second(nullptr, &settings, &vault);
            second.show();
            QTRY_VERIFY(second.isVisible());
            QCOMPARE(second.fontPointSize(), initialSize + 1);
            QCOMPARE(second.findChild<QTableView *>(QStringLiteral("torrentList"))
                         ->font().pointSize(), initialSize + 1);
            auto *reset = second.findChild<QAction *>(QStringLiteral("fontSizeReset"));
            QVERIFY(reset);
            reset->trigger();
            QCOMPARE(second.fontPointSize(), initialSize);
            QCOMPARE(QApplication::font(), original);
            settings.sync();
            QVERIFY(!settings.contains(QStringLiteral("Appearance/FontPointSize")));
        }
        QApplication::setFont(original);
    }

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

    void nativeTransferPresentationShowsUpstreamProgressAndAvailability()
    {
        DemoQbittorrentServer server(true);
        server.enableNativePresentationDemo();
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
        QVERIFY(table && table->model());
        QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 2, 6000);

        // Selection and sorting must never temporarily embolden headings.
        auto *torrentHeader = table->horizontalHeader();
        QVERIFY(torrentHeader);
        QVERIFY(!torrentHeader->highlightSections());
        table->selectRow(0);
        QCoreApplication::processEvents();
        QVERIFY(!torrentHeader->highlightSections());
        table->sortByColumn(1, Qt::DescendingOrder);
        QCoreApplication::processEvents();
        QVERIFY(!torrentHeader->highlightSections());

        // This is the real native Qt painter installed in the live remote
        // table, not a plain-text-only progress cell or a mock item model.
        auto *delegate = qobject_cast<TransferListDelegate *>(table->itemDelegate());
        QVERIFY2(delegate, "Torrent progress must use the adapted upstream delegate.");

        const auto indexFor = [table](const QString &name, int column) {
            for (int row = 0; row < table->model()->rowCount(); ++row)
                if (table->model()->index(row, 0).data().toString() == name)
                    return table->model()->index(row, column);
            return QModelIndex();
        };

        const QModelIndex seed = indexFor(QStringLiteral("Complete seed"), 2);
        const QModelIndex partial = indexFor(QStringLiteral("Partial download"), 2);
        QVERIFY(seed.isValid() && partial.isValid());
        QCOMPARE(seed.data(Qt::UserRole).toDouble(), 1.0);
        QCOMPARE(partial.data(Qt::UserRole).toDouble(), 0.5);
        QVERIFY(seed.data().toString().endsWith(QLatin1Char('%')));
        QVERIFY(partial.data().toString().endsWith(QLatin1Char('%')));
        QCOMPARE(indexFor(QStringLiteral("Complete seed"), 3).data().toString(),
                 QStringLiteral("Seeding"));
        QCOMPARE(indexFor(QStringLiteral("Complete seed"), 12).data().toString(),
                 QStringLiteral("N/A"));

        const QString known = indexFor(QStringLiteral("Partial download"), 12).data().toString();
        QCOMPARE(QLocale().toDouble(known), 2.5);
        QVERIFY(!known.endsWith(QLatin1Char('%')));
        QCOMPARE(known, QLocale().toString(2.5, 'f', 3));

        // Exercise the actual item's paint path at two remotely supplied
        // completion levels: native Qt must produce different bar images.
        const auto paintProgress = [table, delegate](const QModelIndex &index) {
            QImage image(200, 24, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QStyleOptionViewItem option;
            option.rect = QRect(0, 0, 200, 24);
            option.widget = table;
            option.palette = table->palette();
            option.state = QStyle::State_Enabled | QStyle::State_Active;
            QPainter painter(&image);
            delegate->paint(&painter, option, index);
            painter.end();
            return image;
        };
        const QImage completeBar = paintProgress(seed);
        const QImage partialBar = paintProgress(partial);
        QVERIFY2(completeBar != partialBar, "100% and 50% must paint distinct progress bars.");
    }

    void nativeTorrentDetailsUseSelectedRemoteServerAndFilePriorityApis()
    {
        DemoQbittorrentServer server(true);
        server.enableNativePresentationDemo();
        server.enableDetailsDemo();
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
        QVERIFY(table);
        QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 2, 6000);

        int targetRow = -1;
        for (int i = 0; i < table->model()->rowCount(); ++i)
            if (table->model()->index(i, 0).data().toString() == QStringLiteral("Complete seed"))
                targetRow = i;
        QVERIFY(targetRow >= 0);
        table->selectRow(targetRow);
        const QString torrentHash = QStringLiteral("0000000000000000000000000000000000000001");

        auto *tabBar = window.findChild<PropTabBar *>();
        QVERIFY(tabBar);
        // The original qBittorrent bar lives at the BOTTOM of the splitter,
        // while the property pages occupy the area immediately above it.
        QCOMPARE(tabBar->currentIndex(), 0);
        auto *dock = window.findChild<QWidget *>(QStringLiteral("torrentDetailsDock"));
        auto *pages = window.findChild<QWidget *>(QStringLiteral("remoteTorrentProperties"));
        auto *splitter = window.findChild<QSplitter *>(QStringLiteral("torrentDetailsSplitter"));
        QVERIFY(dock && pages && splitter);
        QTRY_VERIFY(pages->isVisible());
        const int expandedHeight = dock->height();
        QVERIFY2(expandedHeight > 100, "Native properties must open above the bottom tabs.");
        QVERIFY2(qAbs(tabBar->geometry().bottom() - dock->height()) < 15,
                 "Tab bar should be bottom-anchored while the details are visible.");

        auto *buttons = tabBar->findChild<QButtonGroup *>();
        QVERIFY(buttons && buttons->button(0) && buttons->button(1));
        QTest::mouseClick(buttons->button(0), Qt::LeftButton);
        QTRY_VERIFY(!pages->isVisible());
        QCOMPARE(tabBar->currentIndex(), -1);
        QTRY_VERIFY_WITH_TIMEOUT(dock->height() < 100, 3000);
        QVERIFY2(qAbs(tabBar->geometry().bottom() - dock->height()) < 15,
                 "Bottom tabs must stay anchored after the panel collapses.");
        QTest::mouseClick(buttons->button(0), Qt::LeftButton);
        QTRY_VERIFY(pages->isVisible());
        QCOMPARE(tabBar->currentIndex(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(dock->height() >= expandedHeight - 25, 3000);
        QVERIFY2(qAbs(tabBar->geometry().bottom() - dock->height()) < 15,
                 "Bottom tabs must remain fixed after restoring the panel.");

        QTRY_VERIFY_WITH_TIMEOUT(server.detailsFor("/api/v2/torrents/properties?") > 0, 6000);
        auto *savePath = window.findChild<QLabel *>(QStringLiteral("labelSavePathVal"));
        QVERIFY(savePath);
        QTRY_COMPARE_WITH_TIMEOUT(savePath->text(), QStringLiteral("/share/NAS/torrents"), 6000);
        auto *propertiesBar = window.findChild<QProgressBar *>(QStringLiteral("detailsProgressBar"));
        QVERIFY(propertiesBar);
        QCOMPARE(propertiesBar->value(), 1000);
        auto *comment = window.findChild<QLabel *>(QStringLiteral("labelCommentVal"));
        QVERIFY(comment);
        QTRY_VERIFY_WITH_TIMEOUT(comment->text().contains(
            QStringLiteral("<a href=\"https://example.test/files?id=7&amp;part=2\">")), 6000);
        QVERIFY(comment->text().contains(QStringLiteral("&lt;b&gt;plain markup&lt;/b&gt;")));
        QVERIFY(!comment->text().contains(QStringLiteral("href=\"javascript:")));
        QCOMPARE(comment->textFormat(), Qt::RichText);
        QVERIFY(!comment->openExternalLinks());
        // Only the comment becomes rich text. Untrusted torrent metadata
        // elsewhere in General must remain plain text.
        QCOMPARE(savePath->textFormat(), Qt::PlainText);

        tabBar->setCurrentIndex(1);
        auto *trackers = window.findChild<QTreeWidget *>(QStringLiteral("torrentTrackers"));
        QVERIFY(trackers);
        QTRY_COMPARE_WITH_TIMEOUT(trackers->topLevelItemCount(), 4, 6000);
        QCOMPARE(trackers->columnCount(), 11);
        QVERIFY(!trackers->header()->highlightSections());
        QCOMPARE(trackers->headerItem()->text(0), QStringLiteral("URL/Announce Endpoint"));
        QCOMPARE(trackers->headerItem()->text(1), QStringLiteral("Tier"));
        QCOMPARE(trackers->headerItem()->text(2), QStringLiteral("BT Protocol"));
        QCOMPARE(trackers->headerItem()->text(7), QStringLiteral("Times Downloaded"));
        QCOMPARE(trackers->headerItem()->text(9), QStringLiteral("Next Announce"));
        QCOMPARE(trackers->headerItem()->text(10), QStringLiteral("Min Announce"));
        for (int row = 0; row < 3; ++row)
        {
            auto *protocol = trackers->topLevelItem(row);
            QVERIFY(protocol);
            QCOMPARE(protocol->text(1), QString());
            QCOMPARE(protocol->text(2), QString());
            QCOMPARE(protocol->text(3), QStringLiteral("Disabled for this torrent"));
            // Upstream GUI uses N/A for pseudo tracker peer/download counts,
            // even when the Web API sends zero for those fields.
            QCOMPARE(protocol->text(4), QStringLiteral("N/A"));
            QCOMPARE(protocol->text(5), QStringLiteral("0"));
            QCOMPARE(protocol->text(6), QStringLiteral("0"));
            QCOMPARE(protocol->text(7), QStringLiteral("N/A"));
            QCOMPARE(protocol->text(9), QString());
            QCOMPARE(protocol->text(10), QString());
            QCOMPARE(protocol->childCount(), 0);
        }
        QCOMPARE(trackers->topLevelItem(0)->text(0), QStringLiteral("** [DHT] **"));
        QCOMPARE(trackers->topLevelItem(1)->text(0), QStringLiteral("** [PeX] **"));
        QCOMPARE(trackers->topLevelItem(2)->text(0), QStringLiteral("** [LSD] **"));

        auto *tracker = trackers->topLevelItem(3);
        QVERIFY(tracker);
        QCOMPARE(tracker->text(0), QStringLiteral("https://tracker.example/announce"));
        QCOMPARE(tracker->text(1), QStringLiteral("0"));
        QCOMPARE(tracker->text(3), QStringLiteral("Working"));
        QCOMPARE(tracker->text(4), QStringLiteral("1"));
        QCOMPARE(tracker->text(5), QStringLiteral("3"));
        QCOMPARE(tracker->text(6), QStringLiteral("2"));
        QCOMPARE(tracker->text(7), QStringLiteral("4"));
        QVERIFY(!tracker->text(9).isEmpty());
        QVERIFY(!tracker->text(10).isEmpty());
        QVERIFY(!tracker->text(9).contains(QLatin1Char('/')));
        QCOMPARE(tracker->childCount(), 1);
        QCOMPARE(tracker->child(0)->text(0), QStringLiteral("tracker.example:443"));
        QCOMPARE(tracker->child(0)->text(1), QString());
        QCOMPARE(tracker->child(0)->text(2), QStringLiteral("v1"));
        QCOMPARE(tracker->child(0)->text(3), QStringLiteral("Working"));

        tracker->setExpanded(true);
        QCOMPARE(tracker->isExpanded(), true);
        // Re-polling must preserve native tree state, not flatten endpoints.
        const int previousTrackerQueries = server.detailsFor("/api/v2/torrents/trackers?");
        auto *serverPane = window.findChild<ServerPane *>();
        QVERIFY(serverPane);
        serverPane->refreshSelectedDetails();
        QTRY_VERIFY_WITH_TIMEOUT(
            server.detailsFor("/api/v2/torrents/trackers?") > previousTrackerQueries, 6000);
        QTRY_VERIFY_WITH_TIMEOUT(trackers->topLevelItem(3)->isExpanded(), 6000);
        QCOMPARE(trackers->topLevelItem(3)->childCount(), 1);

        tabBar->setCurrentIndex(2);
        auto *peers = window.findChild<QTableWidget *>(QStringLiteral("torrentPeers"));
        QVERIFY(peers);
        QTRY_COMPARE_WITH_TIMEOUT(peers->rowCount(), 1, 6000);
        QVERIFY(!peers->horizontalHeader()->highlightSections());
        QCOMPARE(peers->item(0, 2)->text(), QStringLiteral("qBittorrent"));

        tabBar->setCurrentIndex(3);
        auto *sources = window.findChild<QListWidget *>(QStringLiteral("torrentWebSeeds"));
        QVERIFY(sources);
        QTRY_COMPARE_WITH_TIMEOUT(sources->count(), 1, 6000);
        QCOMPARE(sources->item(0)->text(), QStringLiteral("https://mirror.example/file.iso"));

        tabBar->setCurrentIndex(4);
        auto *contents = window.findChild<QTreeWidget *>(QStringLiteral("torrentContent"));
        QVERIFY(contents);
        QTRY_VERIFY_WITH_TIMEOUT(contents->topLevelItemCount() > 0, 6000);
        QTRY_VERIFY_WITH_TIMEOUT(contents->topLevelItem(0)->childCount() == 2, 6000);
        QCOMPARE(contents->columnCount(), 6);
        QVERIFY(!contents->header()->highlightSections());
        QCOMPARE(contents->headerItem()->text(3), QStringLiteral("Download Priority"));
        auto *rootFolder = contents->topLevelItem(0);
        auto *fileOne = rootFolder->child(0);
        auto *fileTwo = rootFolder->child(1);
        QCOMPARE(fileOne->checkState(0), Qt::Checked);
        QCOMPARE(fileTwo->checkState(0), Qt::Checked);
        QCOMPARE(rootFolder->checkState(0), Qt::Checked);
        QCOMPARE(fileOne->text(2), QStringLiteral("100%"));
        QCOMPARE(fileTwo->text(2), QStringLiteral("50%"));
        QVERIFY(fileOne->icon(0).isNull() == false);

        auto *filter = window.findChild<QLineEdit *>(QStringLiteral("torrentFileFilter"));
        QVERIFY(filter);
        filter->setText(QStringLiteral("file-two"));
        QVERIFY(fileOne->isHidden());
        QVERIFY(!fileTwo->isHidden());
        filter->clear();
        QVERIFY(!fileOne->isHidden());

        // Click-to-exclude must send priority=0 for exactly this file;
        // the folder becomes partially checked, as in upstream qBittorrent.
        fileOne->setCheckState(0, Qt::Unchecked);
        QCOMPARE(fileOne->text(3), QStringLiteral("Do not download"));
        QCOMPARE(rootFolder->checkState(0), Qt::PartiallyChecked);
        QTRY_COMPARE_WITH_TIMEOUT(server.commandsFor("/api/v2/torrents/filePrio"), 1, 6000);
        const QUrlQuery excluded(QString::fromUtf8(
            server.lastCommandBody("/api/v2/torrents/filePrio")));
        QCOMPARE(excluded.queryItemValue(QStringLiteral("hash")), torrentHash);
        QCOMPARE(excluded.queryItemValue(QStringLiteral("id")), QStringLiteral("0"));
        QCOMPARE(excluded.queryItemValue(QStringLiteral("priority")), QStringLiteral("0"));

        auto *selectNone = window.findChild<QPushButton *>(QStringLiteral("selectNoneButton"));
        QVERIFY(selectNone);
        // This button excludes the entire torrent without deleting any
        // already-downloaded files. The server is the authority afterwards.
        QTest::mouseClick(selectNone, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(server.commandsFor("/api/v2/torrents/filePrio"), 2, 6000);
        const QUrlQuery priority(QString::fromUtf8(
            server.lastCommandBody("/api/v2/torrents/filePrio")));
        QCOMPARE(priority.queryItemValue(QStringLiteral("hash")), torrentHash);
        QCOMPARE(priority.queryItemValue(QStringLiteral("id")), QStringLiteral("0|1"));
        QCOMPARE(priority.queryItemValue(QStringLiteral("priority")), QStringLiteral("0"));

        tabBar->setCurrentIndex(5);
        auto *speedGraph = window.findChild<QWidget *>(QStringLiteral("remoteSpeedGraph"));
        QVERIFY(speedGraph);
        QVERIFY(speedGraph->isVisible());
        QVERIFY(server.detailsFor("/api/v2/torrents/files?") > 0);
        QVERIFY(server.detailsFor("/api/v2/sync/torrentPeers?") > 0);
        QVERIFY(!server.lastDetailRequest().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(server.syncRequests() >= 2, 6000);
    }

    void startupConnectsOnlyOptedInProfilesUsingProtectedCredentials()
    {
        DemoQbittorrentServer selected(true);
        DemoQbittorrentServer dormant(true);
        QVERIFY(selected.start());
        QVERIFY(dormant.start());

        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("profiles.ini")), QSettings::IniFormat);
        Remote::ProfileStore store(settings);
        DemoCredentialVault vault;

        Remote::ServerProfile automatic {
            QString(), QStringLiteral("Automatic seedbox"), selected.url(),
            QStringLiteral("demo"), true, true
        };
        Remote::ServerProfile manual {
            QString(), QStringLiteral("Manual seedbox"), dormant.url(),
            QStringLiteral("demo"), false, false
        };
        QVERIFY(store.save(automatic));
        QVERIFY(store.save(manual));

        vault.data.insert(vault.keyFor(automatic), QStringLiteral("demo-password"));
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        QTRY_VERIFY(window.isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(
            QStringLiteral("Connected to Automatic seedbox")), 6000);

        auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("serverTabs"));
        QVERIFY(tabs);
        QCOMPARE(tabs->count(), 1);
        QVERIFY(!tabs->tabBar()->isVisible());
        QCOMPARE(vault.reads, 1);
        QCOMPARE(dormant.syncRequests(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(selected.syncRequests() > 0, 6000);

        // No secret is stored in QSettings, and untouched profiles stay offline.
        settings.sync();
        QFile stored(temp.filePath(QStringLiteral("profiles.ini")));
        QVERIFY(stored.open(QIODevice::ReadOnly));
        QVERIFY(!stored.readAll().contains("demo-password"));
    }

    void simultaneousServersHaveIndependentTabsAndKeepPolling()
    {
        DemoQbittorrentServer first(true);
        DemoQbittorrentServer second(true);
        QVERIFY(first.start());
        QVERIFY(second.start());

        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("profiles.ini")), QSettings::IniFormat);
        DemoCredentialVault vault;

        MainWindow window(nullptr, &settings, &vault);
        window.show();
        QTRY_VERIFY(window.isVisible());

        openConnectDialog(window, first.url());
        auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("serverTabs"));
        QVERIFY(tabs);
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 6000);
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(
            QStringLiteral("Connected to Demo seedbox")), 6000);
        QVERIFY2(!tabs->tabBar()->isVisible(), "Single server must not show a tab strip.");

        openConnectDialog(window, second.url(), false, true, true,
                          QStringLiteral("Red seedbox"));
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 2, 6000);
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(
            QStringLiteral("Connected to Red seedbox")), 6000);
        QVERIFY2(tabs->tabBar()->isVisible(), "Two simultaneous servers need visible tabs.");

        auto *firstTable = tabs->widget(0)->findChild<QTableView *>(QStringLiteral("torrentList"));
        auto *secondTable = tabs->widget(1)->findChild<QTableView *>(QStringLiteral("torrentList"));
        QVERIFY(firstTable && secondTable);
        QVERIFY2(firstTable->model() != secondTable->model(),
                 "Each server must own independent torrent models and cookie jars.");

        auto *search = window.findChild<QLineEdit *>(QStringLiteral("torrentSearch"));
        QVERIFY(search);
        search->setText(QStringLiteral("red-isolated-filter"));
        tabs->setCurrentIndex(0);
        QCOMPARE(search->text(), QString());
        tabs->setCurrentIndex(1);
        QCOMPARE(search->text(), QStringLiteral("red-isolated-filter"));

        const int priorFirst = first.syncRequests();
        const int priorSecond = second.syncRequests();
        QTRY_VERIFY_WITH_TIMEOUT(first.syncRequests() > priorFirst, 6000);
        QTRY_VERIFY_WITH_TIMEOUT(second.syncRequests() > priorSecond, 6000);

        auto *disconnect = window.findChild<QAction *>(QStringLiteral("disconnectServer"));
        QVERIFY(disconnect);
        disconnect->trigger();
        QTRY_COMPARE(tabs->count(), 1);
        QVERIFY2(!tabs->tabBar()->isVisible(), "Returning to one server hides tabs.");
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(
            QStringLiteral("Connected to Demo seedbox")), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(second.logouts(), 1, 6000);

        const int beforeLastPoll = first.syncRequests();
        QTRY_VERIFY_WITH_TIMEOUT(first.syncRequests() > beforeLastPoll, 6000);
    }

    void torrentFileUploadPreservesBinaryMetadata()
    {
        DemoQbittorrentServer server(true);
        QVERIFY(server.start());

        Remote::RemoteSession session;
        Remote::ServerProfile profile {
            QString(), QStringLiteral("Temporary fixture"), server.url(),
            QStringLiteral("demo"), false, false
        };
        QSignalSpy connected(&session, &Remote::RemoteSession::connected);
        QSignalSpy accepted(&session, &Remote::RemoteSession::operationSucceeded);

        session.connectToServer(profile, QStringLiteral("demo-password"));
        QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 6000);

        const QByteArray metadata = QByteArray::fromHex("006465616462656566ff");
        session.addTorrentFile(QStringLiteral("test.torrent"), metadata);
        QTRY_COMPARE_WITH_TIMEOUT(server.commandsFor("/api/v2/torrents/add"), 1, 6000);
        QTRY_COMPARE_WITH_TIMEOUT(accepted.count(), 1, 6000);

        const QByteArray request = server.lastCommandBody("/api/v2/torrents/add");
        QVERIFY(request.contains("name=\"torrents\"; filename=\"test.torrent\""));
        QVERIFY(request.contains("Content-Type: application/x-bittorrent"));
        QVERIFY(request.contains(metadata));
        QVERIFY(request.startsWith("--qbtRemote-"));
    }

    void userCanSelectSevenTorrentFilesAndUploadOneAuthenticatedBatch()
    {
        DemoQbittorrentServer server(true);
        QVERIFY(server.start());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());

        QStringList fileNames;
        QList<QByteArray> metadata;
        for (int i = 1; i <= 7; ++i)
        {
            const QByteArray torrentName = QByteArrayLiteral("batch-")
                + QByteArray::number(i);
            const QByteArray encoded = QByteArrayLiteral("d4:infod6:lengthi")
                + QByteArray::number(100 + i) + QByteArrayLiteral("e4:name")
                + QByteArray::number(torrentName.size()) + ':'
                + torrentName + QByteArrayLiteral("ee");
            Remote::TorrentMetadata parsed;
            QString parseError;
            QVERIFY2(Remote::TorrentMetadataReader::read(encoded, parsed, &parseError),
                     qPrintable(parseError));
            const QString name = QStringLiteral("queue_%1.torrent").arg(i);
            QFile output(temp.filePath(name));
            QVERIFY(output.open(QIODevice::WriteOnly));
            QCOMPARE(output.write(encoded), qint64(encoded.size()));
            output.close();
            fileNames.append(name);
            metadata.append(encoded);
        }

        QSettings settings(temp.filePath(QStringLiteral("settings.ini")),
                           QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        QTRY_VERIFY(window.isVisible());
        openConnectDialog(window, server.url());
        QAction *add = window.findChild<QAction *>(QStringLiteral("actionOpen"));
        QVERIFY(add);
        QTRY_VERIFY_WITH_TIMEOUT(add->isEnabled(), 6000);

        const bool native = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        AddBatchTorrentUi interaction(temp.path(), fileNames);
        QApplication::instance()->installEventFilter(&interaction);
        QTimer watchdog;
        watchdog.setSingleShot(true);
        QObject::connect(&watchdog, &QTimer::timeout, &window, []() {
            for (QWidget *widget : QApplication::topLevelWidgets())
                if (auto *dialog = qobject_cast<QDialog *>(widget);
                    dialog && dialog->isVisible())
                    dialog->reject();
        });
        watchdog.start(10000);
        add->trigger(); // Real UI path: picker -> Add Torrent -> Web API.
        watchdog.stop();
        QApplication::instance()->removeEventFilter(&interaction);
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, native);
        QVERIFY2(interaction.error.isEmpty(), qPrintable(interaction.error));
        QCOMPARE(interaction.filePickers, 1);
        QCOMPARE(interaction.addDialogs, 1);

        QTRY_COMPARE_WITH_TIMEOUT(server.commandsFor("/api/v2/torrents/add"), 1, 6000);
        const QByteArray body = server.lastCommandBody("/api/v2/torrents/add");
        QCOMPARE(body.count(QByteArrayLiteral("name=\"torrents\"; filename=\"")), 7);
        for (int i = 0; i < fileNames.size(); ++i)
        {
            QVERIFY2(body.contains(QByteArrayLiteral("name=\"torrents\"; filename=\"")
                    + fileNames.at(i).toUtf8() + '"'),
                     qPrintable(fileNames.at(i)));
            QVERIFY(body.contains(metadata.at(i)));
        }
        QVERIFY(body.contains("name=\"savepath\"\r\n\r\n/share/QueueTest\r\n"));
        QVERIFY(body.contains("name=\"category\"\r\n\r\nQueue Test\r\n"));
        QVERIFY(body.contains("name=\"stopped\"\r\n\r\nfalse\r\n"));
        QCOMPARE(server.commandsFor("/api/v2/torrents/addTrackers"), 0);
    }

    void duplicateTorrentOffersTrackerMergeThroughNativeGui()
    {
        DemoQbittorrentServer server(true);
        QVERIFY(server.start());

        const QByteArray tracker("https://tracker.duplicate.example/announce");
        const QByteArray info("d6:lengthi123e4:name7:examplee");
        const QByteArray metadata = QByteArrayLiteral("d8:announce")
            + QByteArray::number(tracker.size()) + ':' + tracker
            + QByteArrayLiteral("4:info") + info + QByteArrayLiteral("e");
        Remote::TorrentMetadata parsed;
        QVERIFY(Remote::TorrentMetadataReader::read(metadata, parsed));
        QVERIFY(!parsed.infoHashV1.isEmpty());
        server.setExistingTorrentHash(parsed.infoHashV1);

        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString filename = temp.filePath(QStringLiteral("existing.torrent"));
        QFile file(filename);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(metadata), qint64(metadata.size()));
        file.close();

        QSettings settings(temp.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
        MainWindow window(nullptr, &settings);
        window.show();
        openConnectDialog(window, server.url());
        auto *table = window.findChild<QTableView *>(QStringLiteral("torrentList"));
        QVERIFY(table);
        QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 1, 6000);

        // Test the real Qt widget workflow with a deterministic non-native
        // file picker on headless Linux and Windows runners. A watchdog
        // closes unexpected modal dialogs instead of hanging CTest forever.
        const bool originallyNative = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        MergeDuplicateTorrentUi interaction(filename);
        QApplication::instance()->installEventFilter(&interaction);
        QTimer watchdog;
        watchdog.setSingleShot(true);
        QObject::connect(&watchdog, &QTimer::timeout, &window, []() {
            for (QWidget *widget : QApplication::topLevelWidgets())
                if (auto *modal = qobject_cast<QDialog *>(widget);
                    modal && modal->isVisible())
                    modal->reject();
        });
        auto *add = window.findChild<QAction *>(QStringLiteral("actionOpen"));
        QVERIFY(add && add->isEnabled());
        watchdog.start(10000);
        add->trigger();
        watchdog.stop();
        QApplication::instance()->removeEventFilter(&interaction);
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, originallyNative);

        QCOMPARE(interaction.chosenFiles, 1);
        QCOMPARE(interaction.addDialogs, 1);
        QCOMPARE(interaction.mergePrompts, 1);
        QCOMPARE(server.commandsFor("/api/v2/torrents/add"), 0);
        QTRY_COMPARE_WITH_TIMEOUT(server.commandsFor("/api/v2/torrents/addTrackers"), 1, 6000);
        QUrlQuery merged(QString::fromUtf8(
            server.lastCommandBody("/api/v2/torrents/addTrackers")));
        QCOMPARE(merged.queryItemValue(QStringLiteral("hash")), parsed.infoHashV1);
        QCOMPARE(merged.queryItemValue(QStringLiteral("urls")), QString::fromLatin1(tracker));
    }

    void privateTorrentsNeverOfferOrSendTrackerMerge()
    {
        // Block merging both when the *server's existing torrent* is private
        // and when only the incoming .torrent file carries the private flag.
        for (const bool incomingPrivate : {false, true})
        {
            DemoQbittorrentServer server(true);
            QVERIFY(server.start());
            const QByteArray tracker("https://tracker.duplicate.example/announce");
            const QByteArray info = incomingPrivate
                ? QByteArrayLiteral("d6:lengthi123e4:name7:example7:privatei1ee")
                : QByteArrayLiteral("d6:lengthi123e4:name7:examplee");
            const QByteArray metadata = QByteArrayLiteral("d8:announce")
                + QByteArray::number(tracker.size()) + ':' + tracker
                + QByteArrayLiteral("4:info") + info + QByteArrayLiteral("e");
            Remote::TorrentMetadata parsed;
            QVERIFY(Remote::TorrentMetadataReader::read(metadata, parsed));
            QCOMPARE(parsed.isPrivate, incomingPrivate);
            server.setExistingTorrentHash(parsed.infoHashV1);
            server.setExistingTorrentPrivate(!incomingPrivate);

            QTemporaryDir temp;
            QVERIFY(temp.isValid());
            const QString filename = temp.filePath(QStringLiteral("private.torrent"));
            QFile file(filename);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(metadata), qint64(metadata.size()));
            file.close();

            QSettings settings(temp.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
            MainWindow window(nullptr, &settings);
            window.show();
            openConnectDialog(window, server.url());
            auto *table = window.findChild<QTableView *>(QStringLiteral("torrentList"));
            QVERIFY(table);
            QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 1, 6000);

            const bool originallyNative =
                QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
            QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
            MergeDuplicateTorrentUi interaction(filename);
            QApplication::instance()->installEventFilter(&interaction);
            QTimer watchdog;
            watchdog.setSingleShot(true);
            QObject::connect(&watchdog, &QTimer::timeout, &window, []() {
                for (QWidget *widget : QApplication::topLevelWidgets())
                    if (auto *modal = qobject_cast<QDialog *>(widget);
                        modal && modal->isVisible())
                        modal->reject();
            });
            auto *add = window.findChild<QAction *>(QStringLiteral("actionOpen"));
            QVERIFY(add && add->isEnabled());
            watchdog.start(10000);
            add->trigger();
            watchdog.stop();
            QApplication::instance()->removeEventFilter(&interaction);
            QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, originallyNative);

            QCOMPARE(interaction.chosenFiles, 1);
            QCOMPARE(interaction.addDialogs, 1);
            QCOMPARE(interaction.privateWarnings, 1);
            QCOMPARE(interaction.mergePrompts, 0);
            QCOMPARE(server.commandsFor("/api/v2/torrents/add"), 0);
            QCOMPARE(server.commandsFor("/api/v2/torrents/addTrackers"), 0);
        }
    }

    void nativeAddTorrentOptionsUseAuthenticatedServerApi()
    {
        DemoQbittorrentServer server(true);
        QVERIFY(server.start());
        Remote::RemoteSession session;
        Remote::ServerProfile profile {
            {}, QStringLiteral("QNAP"), server.url(),
            QStringLiteral("demo"), false, false
        };
        QSignalSpy connected(&session, &Remote::RemoteSession::connected);
        QSignalSpy submitted(&session, &Remote::RemoteSession::operationSucceeded);
        session.connectToServer(profile, QStringLiteral("demo-password"));
        QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 6000);

        Remote::AddTorrentOptions options;
        options.savePath = QStringLiteral("/share/CACHEDEV1_DATA/Music");
        options.downloadPath = QStringLiteral("/share/CACHEDEV1_DATA/Incomplete");
        options.useDownloadPath = true;
        options.category = QStringLiteral("Music");
        options.tags = QStringLiteral("Classical,Dvorak");
        options.contentLayout = QStringLiteral("NoSubfolder");
        options.stopped = true;
        options.queueTop = true;
        options.firstLastPiece = true;
        session.addTorrentUrls(
            QStringLiteral("magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567"),
            options);
        QTRY_COMPARE_WITH_TIMEOUT(submitted.count(), 1, 6000);
        QCOMPARE(server.commandsFor("/api/v2/torrents/add"), 1);
        QUrlQuery form(QString::fromUtf8(
            server.lastCommandBody("/api/v2/torrents/add")));
        QCOMPARE(form.queryItemValue(QStringLiteral("savepath")), options.savePath);
        QCOMPARE(form.queryItemValue(QStringLiteral("downloadPath")), options.downloadPath);
        QCOMPARE(form.queryItemValue(QStringLiteral("useDownloadPath")), QStringLiteral("true"));
        QCOMPARE(form.queryItemValue(QStringLiteral("category")), options.category);
        QCOMPARE(form.queryItemValue(QStringLiteral("tags")), options.tags);
        QCOMPARE(form.queryItemValue(QStringLiteral("stopped")), QStringLiteral("true"));
        QCOMPARE(form.queryItemValue(QStringLiteral("addToTopOfQueue")), QStringLiteral("true"));
        QCOMPARE(form.queryItemValue(QStringLiteral("contentLayout")), QStringLiteral("NoSubfolder"));

        // File metadata goes through multipart, not URL-encoded binary data.
        const QByteArray metadata = QByteArray::fromHex("006465616462656566ff");
        session.addTorrentFile(QStringLiteral("music.torrent"), metadata, options);
        QTRY_COMPARE_WITH_TIMEOUT(submitted.count(), 2, 6000);
        QCOMPARE(server.commandsFor("/api/v2/torrents/add"), 2);
        const QByteArray body = server.lastCommandBody("/api/v2/torrents/add");
        QVERIFY(body.contains("name=\"savepath\"\r\n\r\n/share/CACHEDEV1_DATA/Music"));
        QVERIFY(body.contains("name=\"downloadPath\"\r\n\r\n/share/CACHEDEV1_DATA/Incomplete"));
        QVERIFY(body.contains("name=\"stopped\"\r\n\r\ntrue"));
        QVERIFY(body.contains("name=\"contentLayout\"\r\n\r\nNoSubfolder"));
        QVERIFY(body.contains("name=\"torrents\"; filename=\"music.torrent\""));
        QVERIFY(body.contains(metadata));
        QVERIFY(!body.contains("filePriorities")); // not supported for uploaded files

        session.addTorrentTrackers(
            QStringLiteral("0123456789abcdef0123456789abcdef01234567"),
            {QStringLiteral("https://first.example/announce"),
             QStringLiteral("udp://second.example:80/announce")});
        QTRY_COMPARE_WITH_TIMEOUT(submitted.count(), 3, 6000);
        const QUrlQuery merged(QString::fromUtf8(
            server.lastCommandBody("/api/v2/torrents/addTrackers")));
        QCOMPARE(merged.queryItemValue(QStringLiteral("hash")),
                 QStringLiteral("0123456789abcdef0123456789abcdef01234567"));
        QCOMPARE(merged.queryItemValue(QStringLiteral("urls"), QUrl::FullyDecoded),
                 QStringLiteral("https://first.example/announce\nudp://second.example:80/announce"));
    }



    void userQueueSubmenuSendsSelectedTorrentCommandsToAuthenticatedServer()
    {
        // The user opens the actual main window, logs into a disposable
        // qBittorrent Web API server, selects a torrent and picks each
        // visible item from the native Queue submenu.
        DemoQbittorrentServer server(true);
        server.enableTorrentSyncDemo();
        server.enableQueueingDemo();
        server.keepDemoTorrents();
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
        QVERIFY(table);
        QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 1, 6000);
        table->selectRow(0);
        QVERIFY(table->selectionModel()->hasSelection());

        const QString hash = QStringLiteral("0123456789abcdef0123456789abcdef01234567");
        const struct {const char *name; const char *endpoint;} commands[] = {
            {"torrentQueueTop", "/api/v2/torrents/topPrio"},
            {"torrentQueueUp", "/api/v2/torrents/increasePrio"},
            {"torrentQueueDown", "/api/v2/torrents/decreasePrio"},
            {"torrentQueueBottom", "/api/v2/torrents/bottomPrio"}
        };
        for (const auto &command : commands)
        {
            QString error;
            QTimer::singleShot(0, &window, [&]() {
                auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
                if (!menu || menu->objectName() != QLatin1String("torrentListContextMenu"))
                {
                    error = QStringLiteral("The native torrent context menu did not open");
                    if (menu) menu->close();
                    return;
                }
                QAction *submenuEntry = nullptr;
                for (QAction *action : menu->actions())
                    if (action->objectName() == QLatin1String("torrentContextQueueMenu"))
                        submenuEntry = action;
                auto *queue = submenuEntry ? submenuEntry->menu() : nullptr;
                QAction *action = nullptr;
                if (queue)
                    for (QAction *candidate : queue->actions())
                        if (candidate->objectName() == QString::fromLatin1(command.name))
                            action = candidate;
                if (!action || !action->isEnabled())
                    error = QStringLiteral("Queue submenu action missing or disabled: ")
                        + QString::fromLatin1(command.name);
                else
                    action->trigger();
                menu->close();
            });

            const QModelIndex selected = table->selectionModel()->selectedRows(0).value(0);
            QVERIFY(selected.isValid());
            QVERIFY(QMetaObject::invokeMethod(table, "customContextMenuRequested",
                Qt::DirectConnection, Q_ARG(QPoint, table->visualRect(selected).center())));
            QVERIFY2(error.isEmpty(), qPrintable(error));

            const QByteArray endpoint(command.endpoint);
            QTRY_COMPARE_WITH_TIMEOUT(server.commandsFor(endpoint), 1, 6000);
            const QUrlQuery request(QString::fromUtf8(server.lastCommandBody(endpoint)));
            QCOMPARE(request.queryItemValue(QStringLiteral("hashes")), hash);
        }

        // Verify command routing through the authenticated server, not
        // qBittorrent scheduling outcomes; the latter requires a separate
        // real-world saturated-queue test and is intentionally deferred.
        QVERIFY(table->selectionModel()->hasSelection());
    }

    void userForceActionsReachSelectedAuthenticatedRemoteServer()
    {
        // Run the real application window through login, torrent selection,
        // the native right-click menu, and a disposable HTTP Web API server.
        // Do not invoke RemoteSession methods to simulate user interaction.
        DemoQbittorrentServer server(true);
        server.enableTorrentSyncDemo();
        server.keepDemoTorrents();
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
        QVERIFY(table);
        QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 1, 6000);
        table->selectRow(0);
        QVERIFY(table->selectionModel()->hasSelection());

        const QString selectedHash = QStringLiteral("0123456789abcdef0123456789abcdef01234567");
        const struct {
            const char *action;
            const char *endpoint;
            bool hasValue;
        } operations[] = {
            {"torrentContextForceStart", "/api/v2/torrents/setForceStart", true},
            {"torrentContextForceRecheck", "/api/v2/torrents/recheck", false},
            {"torrentContextForceReannounce", "/api/v2/torrents/reannounce", false}
        };
        for (const auto &operation : operations)
        {
            QString error;
            QTimer::singleShot(0, &window, [&]() {
                auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
                if (!menu || menu->objectName() != QLatin1String("torrentListContextMenu"))
                {
                    error = QStringLiteral("User did not see the native torrent context menu");
                    if (menu) menu->close();
                    return;
                }

                QAction *action = nullptr;
                for (QAction *candidate : menu->actions())
                {
                    if (candidate->objectName() == QString::fromLatin1(operation.action))
                    {
                        action = candidate;
                        break;
                    }
                }
                if (!action || !action->isEnabled())
                {
                    error = QStringLiteral("Requested torrent menu action missing/disabled: ")
                        + QString::fromLatin1(operation.action);
                }
                else
                {
                    // Trigger exactly as if the user selected this visible action.
                    action->trigger();
                }
                menu->close();
            });
            const QModelIndex row = table->selectionModel()->selectedRows(0).value(0);
            QVERIFY(row.isValid());
            QVERIFY(QMetaObject::invokeMethod(table, "customContextMenuRequested",
                Qt::DirectConnection, Q_ARG(QPoint, table->visualRect(row).center())));
            QVERIFY2(error.isEmpty(), qPrintable(error));

            const QByteArray endpoint(operation.endpoint);
            QTRY_COMPARE_WITH_TIMEOUT(server.commandsFor(endpoint), 1, 6000);
            const QUrlQuery submitted(QString::fromUtf8(server.lastCommandBody(endpoint)));
            QCOMPARE(submitted.queryItemValue(QStringLiteral("hashes")), selectedHash);
            if (operation.hasValue)
                QCOMPARE(submitted.queryItemValue(QStringLiteral("value")),
                         QStringLiteral("true"));
            else
                QVERIFY(!submitted.hasQueryItem(QStringLiteral("value")));
        }

        // All three commands must go to the authenticated active server,
        // not an anonymous request or the local torrent engine.
        QCOMPARE(server.commandsFor("/api/v2/torrents/setForceStart"), 1);
        QCOMPARE(server.commandsFor("/api/v2/torrents/recheck"), 1);
        QCOMPARE(server.commandsFor("/api/v2/torrents/reannounce"), 1);
        QVERIFY(table->selectionModel()->hasSelection());
    }

    void rejectedTorrentActionDoesNotDisconnectServer()
    {
        DemoQbittorrentServer server(true);
        server.rejectTorrentCommands();
        QVERIFY(server.start());

        Remote::RemoteSession session;
        Remote::ServerProfile profile {
            QString(), QStringLiteral("Temporary fixture"), server.url(),
            QStringLiteral("demo"), false, false
        };
        QSignalSpy connected(&session, &Remote::RemoteSession::connected);
        QSignalSpy failedOperation(&session, &Remote::RemoteSession::operationFailed);

        session.connectToServer(profile, QStringLiteral("demo-password"));
        QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 6000);
        session.commandTorrents(Remote::RemoteSession::TorrentCommand::Start,
            {QStringLiteral("0123456789abcdef0123456789abcdef01234567")});
        QTRY_COMPARE_WITH_TIMEOUT(failedOperation.count(), 1, 6000);
        QVERIFY(session.isConnected());

        const int requestsBefore = server.syncRequests();
        QTRY_VERIFY_WITH_TIMEOUT(server.syncRequests() > requestsBefore, 6000);
    }

    void realTorrentActionsUseAuthenticatedServerApis()
    {
        DemoQbittorrentServer server(true);
        server.enableTorrentSyncDemo();
        server.keepDemoTorrents();
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
        QVERIFY(table);
        QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 1, 6000);
        table->selectRow(0);

        const auto action = [&window](const char *name) {
            return window.findChild<QAction *>(QString::fromLatin1(name));
        };
        QAction *start = action("actionStart");
        QAction *stop = action("actionStop");
        QAction *top = action("actionTopQueuePos");
        QAction *remove = action("actionDelete");
        QAction *link = action("actionDownloadFromURL");
        QVERIFY(start && stop && top && remove && link);
        QTRY_VERIFY(start->isEnabled());
        QVERIFY(stop->isEnabled());
        QVERIFY(top->isEnabled());
        QVERIFY(remove->isEnabled());

        start->trigger();
        stop->trigger();
        top->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(server.commandsFor("/api/v2/torrents/start"), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(server.commandsFor("/api/v2/torrents/stop"), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(server.commandsFor("/api/v2/torrents/topPrio"), 1, 5000);
        QCOMPARE(QUrlQuery(QString::fromUtf8(server.lastCommandBody(
            "/api/v2/torrents/start"))).queryItemValue(QStringLiteral("hashes")),
            QStringLiteral("0123456789abcdef0123456789abcdef01234567"));

        ConfirmTorrentRemoval confirmation;
        qApp->installEventFilter(&confirmation);
        remove->trigger();
        qApp->removeEventFilter(&confirmation);
        QCOMPARE(confirmation.prompts, 1);
        QTRY_COMPARE_WITH_TIMEOUT(server.commandsFor("/api/v2/torrents/delete"), 1, 5000);
        const QUrlQuery deletion(QString::fromUtf8(
            server.lastCommandBody("/api/v2/torrents/delete")));
        QCOMPARE(deletion.queryItemValue(QStringLiteral("deleteFiles")),
                 QStringLiteral("false"));

        QTimer::singleShot(0, []() {
            auto *dialog = qobject_cast<RemoteAddTorrentDialog *>(
                QApplication::activeModalWidget());
            if (!dialog)
                return;
            auto *links = dialog->findChild<QPlainTextEdit *>(
                QStringLiteral("torrentLinks"));
            auto *buttons = dialog->findChild<QDialogButtonBox *>(
                QStringLiteral("buttonBox"));
            if (links && buttons)
            {
                links->setPlainText(QStringLiteral(
                    "magnet:?xt=urn:btih:fedcba9876543210fedcba9876543210fedcba98"));
                buttons->button(QDialogButtonBox::Ok)->click();
            }
        });
        QVERIFY(link->isEnabled());
        link->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(server.commandsFor("/api/v2/torrents/add"), 1, 5000);
        const QUrlQuery addition(QString::fromUtf8(
            server.lastCommandBody("/api/v2/torrents/add")));
        QVERIFY(addition.queryItemValue(QStringLiteral("urls"))
                .startsWith(QStringLiteral("magnet:?xt=")));
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
