#include "RemoteBuildVersion.h"
// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/MainWindow.h"
#include "app/SingleInstance.h"
#include <QProcess>
#include "gui/RemoteBanListDialog.h"
#include <QToolButton>
#include "gui/CompletionNotifier.h"
#include "gui/ServerPane.h"
#include "gui/RemotePreferencesDialog.h"
#include "gui/RemoteFileSystemPathComboEdit.h"
#include "gui/RemoteFileDialog.h"
#include "gui/RemoteSpeedLimitsDialog.h"
#include "gui/MappedContentTreeWidget.h"
#include "gui/TorrentModel.h"
#include "gui/ConnectionDialog.h"
#include "gui/RemoteAddTorrentDialog.h"
#include "gui/RemoteTorrentCreatorDialog.h"
#include "gui/RemoteStatisticsDialog.h"
#include <QTextEdit>
#include "gui/RemoteTrackerEntriesDialog.h"
#include "gui/RemoteTorrentContentLayoutDialog.h"
#include "gui/RemoteTorrentOptionsDialog.h"
#include "gui/upstream/transferlistdelegate.h"
#include "gui/upstream/proptabbar.h"
#include "api/CredentialVault.h"
#include "api/RemoteSession.h"
#include "api/PeerEndpoint.h"
#include "api/TorrentMetadataReader.h"
#include "api/PathMappingStore.h"

#include <iterator>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QDateTime>
#include <QJsonDocument>
#include <QAbstractButton>
#include <QCheckBox>
#include <QColor>
#include <QPalette>
#include <QSignalSpy>
#include <QButtonGroup>
#include <QFile>
#include <QFileInfo>
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
#include <QScopeGuard>
#include <QScrollBar>
#include <QShortcut>
#include <QStatusBar>
#include <QTabWidget>
#include <QTabBar>
#include <QTableView>
#include <QToolBar>
#include <QAbstractItemModel>
#include <QListWidget>
#include <QLabel>
#include <QLocale>
#include <QInputDialog>
#include <QPlainTextEdit>
#include <QTextBrowser>
#include <QStandardItemModel>
#include <QSpinBox>
#include <QTimeEdit>
#include <QDoubleSpinBox>
#include <QRadioButton>
#include <QDialogButtonBox>
#include <QImage>
#include <QProgressBar>
#include <QTableWidget>
#include <QTreeWidget>
#include <QTreeView>
#include <QItemSelectionModel>
#include <QTreeWidgetItemIterator>
#include <QPainter>
#include <QStyleOptionViewItem>
#include <QSplitter>
#include <QPair>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QWheelEvent>
#include <QUrlQuery>
#include <QtTest/QtTest>

#include <memory>

// A local, disposable HTTP server providing observable qBittorrent API
// behavior; it never contacts a NAS or uses any real credentials.
class DemoQbittorrentServer final : public QObject
{
public:
    // Independent, deliberately scoped historical contracts. Other
    // fixture responses remain synthetic until their subsystem audit.
    enum class CapabilityProfile { Synthetic, V410, V514, V520, V530 };
    void setCapabilityProfile(CapabilityProfile profile)
    {
        m_capabilityProfile = profile;
        switch (profile)
        {
        case CapabilityProfile::V410: setReportedVersions("v4.1.0", "2.0"); break;
        case CapabilityProfile::V514: setReportedVersions("v5.1.4", "2.11.4"); break;
        case CapabilityProfile::V520: setReportedVersions("v5.2.0", "2.15.1"); break;
        case CapabilityProfile::V530: setReportedVersions("v5.3.0", "2.16.2"); break;
        case CapabilityProfile::Synthetic: break;
        }
    }
    int prohibitedCapabilityRequests() const noexcept { return m_prohibitedCapabilityRequests; }

    // Scope: login/logout, session cookie and category sync shape only.
    // Not a complete historical server fixture.
    void setHistoricalConnectionProfile(const QString &version, const QString &api,
                                        bool modernLogin, bool listCategories)
    {
        setReportedVersions("v" + version, api);
        m_historicalConnection = true;
        m_modernResponses = modernLogin;
        m_listCategories = listCategories;
        m_sessionCookie = modernLogin ? QByteArrayLiteral("QBT_SID_fixture")
                                     : QByteArrayLiteral("SID");
    }
    int invalidHistoricalSyncReplies() const { return m_invalidHistoricalSyncReplies; }

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
                        const bool hasCookie = headers.contains(m_sessionCookie + "=demo-session")
                            && (!m_historicalConnection || m_historicalAuthenticated);

                        int code = 200;
                        QByteArray response;
                        QByteArray cookieHeader;
                        if (hasCookie && rejectsCapabilityRequest(path, fields.value(0), body, code))
                        {
                            ++m_prohibitedCapabilityRequests;
                            response = "Outside historical fixture contract";
                        }
                        else if (path == "/api/v2/auth/login")
                        {
                            const QUrlQuery data(QString::fromUtf8(body));
                            // Upstream creates a bypass session before loginAction,
                            // which then succeeds even with incorrect credentials.
                            const bool bypass = m_webUITransitionDemo
                                && (m_preferences.value("bypass_local_auth").toBool()
                                    || m_preferences.value("bypass_auth_subnet_whitelist_enabled").toBool());
                            const bool good = bypass || (m_acceptPassword
                                && data.queryItemValue(QStringLiteral("username"), QUrl::FullyDecoded) == m_loginUsername
                                && (data.queryItemValue(QStringLiteral("password"), QUrl::FullyDecoded)
                                        == m_loginPassword
                                    || (m_acceptBlank && data.queryItemValue(
                                        QStringLiteral("password"), QUrl::FullyDecoded).isEmpty())));
                            if (m_modernResponses)
                                code = good ? 204 : 401;
                            else
                                response = good ? QByteArrayLiteral("Ok.") : QByteArrayLiteral("Fails.");
                            if (good)
                            {
                                m_historicalAuthenticated = true;
                                cookieHeader = "Set-Cookie: " + m_sessionCookie
                                    + "=demo-session; Path=/; HttpOnly\r\n";
                            }
                        }
                        else if (path == "/api/v2/app/version" && hasCookie)
                        {
                            response = m_reportedQbtVersion;
                        }
                        else if (path == "/api/v2/app/webapiVersion" && hasCookie)
                        {
                            response = m_reportedApiVersion;
                        }
                        else if (path == "/api/v2/app/networkInterfaceList" && hasCookie)
                        {
                            if (m_networkInterfaces.isEmpty()) { code = 404; response = "Unavailable"; }
                            else response = QJsonDocument(m_networkInterfaces).toJson(QJsonDocument::Compact);
                        }
                        else if (path.startsWith("/api/v2/app/networkInterfaceAddressList?") && hasCookie)
                        {
                            const QUrlQuery query(QString::fromLatin1(path.mid(path.indexOf('?') + 1)));
                            const QString iface = query.queryItemValue("iface", QUrl::FullyDecoded);
                            if (!m_networkAddresses.contains(iface)) { code = 404; response = "Unavailable"; }
                            else response = QJsonDocument(m_networkAddresses.value(iface)).toJson(QJsonDocument::Compact);
                        }
                        else if (path == "/api/v2/torrentcreator/status" && hasCookie && m_creatorDemo)
                        {
                            response = "[]";
                        }
                        else if (path == "/api/v2/torrentcreator/addTask" && hasCookie && m_creatorDemo)
                        {
                            m_commands.append({path, body});
                            m_creatorActive = true;
                            m_creatorPolls = 0;
                            response = QJsonDocument(QJsonObject{{"taskID", "creator&task"}}).toJson(QJsonDocument::Compact);
                        }
                        else if (path.startsWith("/api/v2/torrentcreator/status?") && hasCookie && m_creatorDemo)
                        {
                            const QUrlQuery query(QString::fromLatin1(path.mid(path.indexOf('?') + 1)));
                            if (!m_creatorActive || query.queryItemValue("taskID", QUrl::FullyDecoded) != "creator&task")
                            { code = 404; response = "Not found"; }
                            else
                            {
                                ++m_creatorPolls;
                                const QJsonObject task{{"taskID", "creator&task"},
                                    {"status", m_creatorHold || m_creatorPolls == 1 ? "Running" : "Finished"},
                                    {"progress", 43}};
                                response = QJsonDocument(QJsonArray{task}).toJson(QJsonDocument::Compact);
                            }
                        }
                        else if (path.startsWith("/api/v2/torrentcreator/torrentFile?") && hasCookie && m_creatorDemo)
                        {
                            const QUrlQuery query(QString::fromLatin1(path.mid(path.indexOf('?') + 1)));
                            if (!m_creatorActive || m_creatorPolls < 2 || m_creatorHold
                                || query.queryItemValue("taskID", QUrl::FullyDecoded) != "creator&task")
                            { code = 409; response = "Not ready"; }
                            else
                            {
                                ++m_creatorDownloads;
                                response = m_exportedTorrent;
                            }
                        }
                        else if (path == "/api/v2/torrentcreator/deleteTask" && hasCookie && m_creatorDemo)
                        {
                            m_commands.append({path, body});
                            const QUrlQuery query(QString::fromUtf8(body));
                            if (!m_creatorActive || query.queryItemValue("taskID", QUrl::FullyDecoded) != "creator&task")
                            { code = 404; response = "Not found"; }
                            else m_creatorActive = false;
                        }
                        else if (path == "/api/v2/app/processInfo" && hasCookie)
                        {
                            ++m_processInfoRequests;
                            if (m_processInfo.isEmpty())
                            { code = 404; response = "Unavailable"; }
                            else response = QJsonDocument(m_processInfo).toJson(QJsonDocument::Compact);
                        }
                        else if (path == "/api/v2/app/buildInfo" && hasCookie)
                        {
                            if (m_buildInfo.isEmpty())
                            {
                                code = 404;
                                response = "Unavailable";
                            }
                            else
                                response = QJsonDocument(m_buildInfo).toJson(QJsonDocument::Compact);
                        }
                        else if (path.startsWith("/api/v2/sync/maindata?rid=") && hasCookie)
                        {
                            ++m_syncRequests;
                            const QUrlQuery query(QString::fromLatin1(path.mid(path.indexOf('?') + 1)));
                            const int rid = query.queryItemValue(QStringLiteral("rid")).toInt();
                            if (m_rejectSync)
                            {
                                code = 403;
                                response = "Forbidden";
                            }
                            else if (m_completionSyncDemo)
                            {
                                QJsonObject snapshot;
                                if (rid == 0)
                                {
                                    snapshot = m_completionSnapshot;
                                    snapshot.insert(QStringLiteral("full_update"), true);
                                }
                                else if (!m_completionPatches.isEmpty())
                                    snapshot = m_completionPatches.takeFirst();
                                snapshot.insert(QStringLiteral("rid"), rid + 1);
                                // Fail fixture mistakes rather than silently
                                // supplying a modern category map to 4.1.0.
                                if (m_historicalConnection && snapshot.contains("categories")
                                    && (m_listCategories ? !snapshot.value("categories").isArray()
                                                         : !snapshot.value("categories").isObject()))
                                {
                                    ++m_invalidHistoricalSyncReplies;
                                    code = 500;
                                    response = "Invalid fixture category shape";
                                }
                                else
                                    response = QJsonDocument(snapshot).toJson(QJsonDocument::Compact);
                            }
                            else if (!m_sidebarSnapshot.isEmpty())
                            {
                                QJsonObject snapshot = m_sidebarSnapshot;
                                snapshot.insert(QStringLiteral("rid"), rid + 1);
                                snapshot.insert(QStringLiteral("full_update"), true);
                                response = QJsonDocument(snapshot).toJson(QJsonDocument::Compact);
                            }
                            else if (m_statusRatesDemo)
                            {
                                response = QJsonDocument(QJsonObject{
                                    {QStringLiteral("rid"), rid + 1},
                                    {QStringLiteral("full_update"), rid == 0},
                                    {QStringLiteral("torrents"), QJsonObject()},
                                    {QStringLiteral("server_state"), m_statusRates}
                                }).toJson(QJsonDocument::Compact);
                            }
                            else if (m_sessionPauseDemo)
                            {
                                response = QJsonDocument(QJsonObject{
                                    {"rid", rid + 1}, {"full_update", rid == 0},
                                    {"torrents", QJsonObject()},
                                    {"server_state", QJsonObject{{"session_state", m_sessionPaused},
                                        {"dl_info_speed", m_sessionPaused ? 0 : 2048},
                                        {"up_info_speed", m_sessionPaused ? 0 : 1024}}}
                                }).toJson(QJsonDocument::Compact);
                            }
                            else if (m_trayDemo)
                            {
                                // Stateful authenticated sync/maindata for the
                                // selected NAS alternative speed setting.
                                response = QJsonDocument(QJsonObject{
                                    {QStringLiteral("rid"), rid + 1},
                                    {QStringLiteral("full_update"), rid == 0},
                                    {QStringLiteral("torrents"), QJsonObject()},
                                    {QStringLiteral("server_state"), QJsonObject{
                                        {QStringLiteral("use_alt_speed_limits"), m_trayAltMode},
                                        {QStringLiteral("connection_status"), m_trayConnectionStatus},
                                        {QStringLiteral("dl_info_speed"), 0},
                                        {QStringLiteral("up_info_speed"), 0}
                                    }}
                                }).toJson(QJsonDocument::Compact);
                            }
                            else if (m_savingManagementTorrent.isBool())
                            {
                                const QJsonObject torrents{
                                    {QStringLiteral("0123456789abcdef0123456789abcdef01234567"),
                                        QJsonObject{
                                            {QStringLiteral("name"), QStringLiteral("Existing download")},
                                            {QStringLiteral("state"), QStringLiteral("uploading")},
                                            {QStringLiteral("progress"), 1},
                                            {QStringLiteral("auto_tmm"), m_savingManagementTorrent}
                                        }}
                                };
                                response = QJsonDocument(QJsonObject{
                                    {QStringLiteral("rid"), rid + 1},
                                    {QStringLiteral("full_update"), rid == 0},
                                    {QStringLiteral("torrents"), rid == 0 ? torrents : QJsonObject()}
                                }).toJson(QJsonDocument::Compact);
                            }
                            else if (!m_existingTorrentHash.isEmpty())
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
                                // Authenticated sync/maindata drives the actual
                                // visible Qt torrent table; no local torrent data.
                                if (rid == 0)
                                    response = R"({"rid":1,"full_update":true,"torrents":{"0000000000000000000000000000000000000001":{"name":"Complete seed","tracker":"https://tracker.example.test/announce/private-passkey","added_on":1700000000,"size":2460000,"total_size":4194304,"progress":1.0,"state":"stalledUP","availability":-1,"num_seeds":0,"num_complete":3,"num_leechs":0,"num_incomplete":0},"0000000000000000000000000000000000000002":{"name":"Partial download","size":1024,"progress":0.5,"state":"downloading","availability":2.5,"num_seeds":2,"num_complete":5,"num_leechs":4,"num_incomplete":8}},"server_state":{"dl_info_speed":0,"up_info_speed":0}})";
                                else if (m_pendingSwarmCountsUpdate)
                                {
                                    m_pendingSwarmCountsUpdate = false;
                                    response = QByteArrayLiteral("{\"rid\":")
                                        + QByteArray::number(rid + 1)
                                        + QByteArrayLiteral(R"(,"full_update":false,"torrents":{"0000000000000000000000000000000000000001":{"num_seeds":10,"num_complete":14,"num_leechs":3,"num_incomplete":6},"0000000000000000000000000000000000000002":{"num_complete":11}}})");
                                }
                                else
                                    response = QByteArrayLiteral("{\"rid\":")
                                        + QByteArray::number(rid + 1)
                                        + QByteArrayLiteral(R"(,"full_update":false,"server_state":{"dl_info_speed":0}})");
                            }
                            else if (!m_demoTorrents)
                                response = R"({"rid":1,"full_update":true,"torrents":{},"server_state":{"dl_info_speed":0,"up_info_speed":0}})";
                            else if (rid == 0)
                            {
                                response = R"({"rid":1,"full_update":true,"torrents":{"0123456789abcdef0123456789abcdef01234567":{"name":"Demo Linux ISO","size":1048576,"progress":0.25,"state":"downloading","dlspeed":2048,"upspeed":1024,"category":"linux"}},"server_state":{"dl_info_speed":2048,"up_info_speed":1024}})";
                                if (m_mappedFolderDemo)
                                    response.replace(
                                        QByteArrayLiteral("\"state\":\"downloading\",\"dlspeed\""),
                                        QByteArrayLiteral("\"state\":\"downloading\","
                                            "\"save_path\":\"/share/NAS/Music\","
                                            "\"content_path\":\"/share/NAS/Music/recording.flac\","
                                            "\"dlspeed\""));
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
                                     || path.startsWith("/api/v2/torrents/pieceStates?")
                                     || path.startsWith("/api/v2/torrents/pieceAvailability?")
                                     || path.startsWith("/api/v2/torrents/trackers?")
                                     || path.startsWith("/api/v2/sync/torrentPeers?")
                                     || path.startsWith("/api/v2/torrents/webseeds?")
                                     || path.startsWith("/api/v2/torrents/files?")))
                        {
                            m_detailRequests.append(path);
                            if (path.startsWith("/api/v2/torrents/trackers?")
                                && m_statefulTrackerDemo)
                            {
                                const QString hash = QUrlQuery(QString::fromLatin1(
                                    path.mid(path.indexOf('?') + 1)))
                                    .queryItemValue(QStringLiteral("hash"));
                                response = QJsonDocument(m_trackerEntries.value(hash))
                                    .toJson(QJsonDocument::Compact);
                            }
                            else if (path.startsWith("/api/v2/torrents/files?")
                                     && m_statefulContentDemo)
                            {
                                const QString hash = QUrlQuery(QString::fromLatin1(
                                    path.mid(path.indexOf('?') + 1)))
                                    .queryItemValue(QStringLiteral("hash"));
                                response = QJsonDocument(m_contentEntries.value(hash))
                                    .toJson(QJsonDocument::Compact);
                            }
                            else if (path.startsWith("/api/v2/torrents/pieceStates?"))
                            {
                                const auto hash = QUrlQuery(QString::fromLatin1(
                                    path.mid(path.indexOf('?') + 1))).queryItemValue(QStringLiteral("hash"));
                                response = hash == QLatin1String("0000000000000000000000000000000000000002")
                                    ? "[2,0,1,0]" : "[2,2,2,2]";
                            }
                            else if (path.startsWith("/api/v2/torrents/pieceAvailability?"))
                                response = "[0,2,4,2]";
                            else if (path.startsWith("/api/v2/torrents/properties?"))
                                {
                                response = R"DEMO({"save_path":"/share/NAS/torrents","comment":"Get it at https://example.test/files?id=7&part=2 and <b>plain markup</b>; javascript:alert(1)","total_downloaded":1048576,"total_uploaded":2048,"total_size":1048576,"time_elapsed":3600,"eta":0,"share_ratio":1.25,"pieces_num":4,"piece_size":262144,"pieces_have":2,"availability":2.5,"seeds":2,"peers":1})DEMO";
                                const auto hash = QUrlQuery(QString::fromLatin1(
                                    path.mid(path.indexOf('?') + 1))).queryItemValue(QStringLiteral("hash"));
                                auto properties = QJsonDocument::fromJson(response).object();
                                properties.insert(QStringLiteral("private"), hash.endsWith(QLatin1Char('2')));
                                if (m_torrentComments.contains(hash))
                                    properties.insert(QStringLiteral("comment"), m_torrentComments.value(hash));
                                response = QJsonDocument(properties).toJson(QJsonDocument::Compact);
                            }
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
                                if (m_distinctTrackerPasskeys)
                                {
                                    const QString hash = QUrlQuery(QString::fromUtf8(
                                        path.mid(path.indexOf('?') + 1)))
                                        .queryItemValue(QStringLiteral("hash"));
                                    const QByteArray oldKey = hash.startsWith(QLatin1Char('a'))
                                        ? QByteArrayLiteral("OLD_PASSKEY_ALPHA")
                                        : QByteArrayLiteral("OLD_PASSKEY_BETA");
                                    response.replace(
                                        QByteArrayLiteral("https://tracker.example/announce"),
                                        QByteArrayLiteral("https://tracker.example/announce/")
                                            + oldKey);
                                }
                            }
                            else if (path.startsWith("/api/v2/sync/torrentPeers?"))
                            {
                                const auto rid = QUrlQuery(QString::fromLatin1(
                                    path.mid(path.indexOf('?') + 1))).queryItemValue(QStringLiteral("rid"));
                                if (rid == QLatin1String("1"))
                                    response = R"({"rid":2,"peers":{"192.0.2.1:61413":{"dl_speed":4096}}})";
                                else if (rid == QLatin1String("2"))
                                    response = R"({"rid":3})";
                                else
                                    response = R"({"rid":1,"full_update":true,"peers":{"192.0.2.1:61413":{"client":"qBittorrent","country_code":"US","progress":0.75,"dl_speed":1024,"up_speed":2048,"flags":"D","connection":"TCP","ip":"192.0.2.1","port":61413,"downloaded":1048576,"uploaded":1024,"relevance":0.25,"files":"data/file.bin"}}})";
                            }
                            else if (path.startsWith("/api/v2/torrents/webseeds?"))
                            {
                                const auto hash = QUrlQuery(QString::fromLatin1(
                                    path.mid(path.indexOf('?') + 1))).queryItemValue(QStringLiteral("hash"));
                                QJsonArray seeds;
                                for (const auto &url : m_webSeeds.value(hash, {QStringLiteral("https://mirror.example/file.iso")}))
                                    seeds.append(QJsonObject{{"url", url}});
                                response = QJsonDocument(seeds).toJson(QJsonDocument::Compact);
                            }
                            else
                                response = R"([{"index":0,"name":"data/file-one.bin","size":512,"progress":1,"priority":1},{"index":1,"name":"data/file-two.bin","size":512,"progress":0.5,"priority":6}])";
                        }
                        else if ((path == "/api/v2/torrents/addWebSeeds" || path == "/api/v2/torrents/editWebSeed"
                                  || path == "/api/v2/torrents/removeWebSeeds") && hasCookie && fields.value(0) == "POST")
                        {
                            m_commands.append({path, body});
                            const QUrlQuery form(QString::fromUtf8(body));
                            const auto hash = form.queryItemValue(QStringLiteral("hash"), QUrl::FullyDecoded);
                            auto seeds = m_webSeeds.value(hash, {QStringLiteral("https://mirror.example/file.iso")});
                            const auto decode = [](const QString &value) { return QUrl::fromPercentEncoding(value.toLatin1()); };
                            bool valid = !hash.isEmpty() && !m_rejectCommands;
                            if (path == "/api/v2/torrents/editWebSeed")
                            {
                                const auto old = decode(form.queryItemValue(QStringLiteral("origUrl"), QUrl::FullyDecoded));
                                const auto replacement = decode(form.queryItemValue(QStringLiteral("newUrl"), QUrl::FullyDecoded));
                                valid = valid && seeds.contains(old) && Remote::validWebSeedUrl(replacement);
                                if (valid) seeds[seeds.indexOf(old)] = replacement;
                            }
                            else
                            {
                                for (const auto &encoded : form.queryItemValue(QStringLiteral("urls"), QUrl::FullyDecoded).split(QLatin1Char('|')))
                                {
                                    const auto url = decode(encoded);
                                    valid = valid && Remote::validWebSeedUrl(url);
                                    if (path == "/api/v2/torrents/addWebSeeds" && !seeds.contains(url)) seeds.append(url);
                                    else if (path == "/api/v2/torrents/removeWebSeeds") seeds.removeAll(url);
                                }
                            }
                            if (valid) { m_webSeeds.insert(hash, seeds); code = 204; }
                            else { code = 400; response = "Invalid web seed"; }
                        }
                        else if ((path == "/api/v2/torrents/addPeers" || path == "/api/v2/transfer/banPeers")
                                 && hasCookie && fields.value(0) == "POST")
                        {
                            m_commands.append({path, body});
                            const QUrlQuery form(QString::fromUtf8(body));
                            const auto hash = form.queryItemValue(QStringLiteral("hashes"), QUrl::FullyDecoded);
                            const auto peers = form.queryItemValue(QStringLiteral("peers"), QUrl::FullyDecoded).split(QLatin1Char('|'));
                            if (m_rejectCommands || Remote::normalizedPeerEndpoints(peers).isEmpty())
                            {
                                code = 400;
                                response = "Invalid peers";
                            }
                            else if (path == "/api/v2/torrents/addPeers")
                            {
                                response = QJsonDocument(QJsonObject{{hash, QJsonObject{
                                    {"added", 1}, {"failed", static_cast<int>(peers.size()) - 1}}}})
                                    .toJson(QJsonDocument::Compact);
                            }
                            else
                                code = 204;
                        }
                        else if (path == "/api/v2/torrents/setComment" && hasCookie && fields.value(0) == "POST")
                        {
                            m_commands.append({path, body});
                            const QUrlQuery form(QString::fromUtf8(body));
                            const auto hash = form.queryItemValue(QStringLiteral("hashes"), QUrl::FullyDecoded);
                            if (m_rejectCommands || !form.hasQueryItem(QStringLiteral("comment"))
                                || hash.isEmpty() || hash.contains(QLatin1Char('|')))
                            {
                                code = 400;
                                response = "Invalid comment";
                            }
                            else
                            {
                                m_torrentComments.insert(hash,
                                    form.queryItemValue(QStringLiteral("comment"), QUrl::FullyDecoded).trimmed());
                                code = 204;
                            }
                        }
                        else if (path.startsWith("/api/v2/torrents/export?") && hasCookie)
                        {
                            const QString hash = QUrlQuery(QString::fromLatin1(
                                path.mid(path.indexOf('?') + 1)))
                                .queryItemValue(QStringLiteral("hash"));
                            if (m_statefulOptionsDemo && m_optionStates.contains(hash))
                                response = m_exportedTorrent;
                            else
                            {
                                code = 404;
                                response = "Unknown torrent";
                            }
                        }
                        else if (m_rssDemo && hasCookie && path.startsWith("/api/v2/rss/"))
                        {
                            if (path.startsWith("/api/v2/rss/items?"))
                                response = QJsonDocument(m_rssItems).toJson(QJsonDocument::Compact);
                            else if (path == "/api/v2/rss/rules")
                                response = QJsonDocument(m_rssRules).toJson(QJsonDocument::Compact);
                            else if (path.startsWith("/api/v2/rss/matchingArticles?"))
                                response = QJsonDocument(QJsonObject{{"News", QJsonArray{"Example article"}}}).toJson(QJsonDocument::Compact);
                            else if (fields.value(0) == "POST")
                            {
                                m_commands.append({path, body});
                                const QUrlQuery form(QString::fromUtf8(body));
                                const auto param = [&](const char *key) {
                                    return form.queryItemValue(QString::fromLatin1(key), QUrl::FullyDecoded);
                                };
                                if (path == "/api/v2/rss/addFeed")
                                    m_rssItems.insert(param("path"), QJsonObject{{"uid", "new-id"}, {"url", param("url")}, {"articles", QJsonArray{}}});
                                else if (path == "/api/v2/rss/markAsRead")
                                {
                                    QJsonObject feed = m_rssItems.value(param("itemPath")).toObject();
                                    QJsonArray articles = feed.value("articles").toArray();
                                    for (qsizetype i = 0; i < articles.size(); ++i)
                                    {
                                        QJsonObject article = articles.at(i).toObject();
                                        if (article.value("id").toString() == param("articleId"))
                                        {
                                            article.insert("isRead", true);
                                            articles.replace(i, article);
                                        }
                                    }
                                    feed.insert("articles", articles);
                                    m_rssItems.insert(param("itemPath"), feed);
                                }
                                else if (path == "/api/v2/rss/setRule")
                                    m_rssRules.insert(param("ruleName"), QJsonDocument::fromJson(param("ruleDef").toUtf8()).object());
                                else if (path != "/api/v2/rss/refreshItem")
                                    code = 400;
                                if (code == 200 && m_modernResponses) code = 204;
                            }
                            else code = 404;
                        }
                        else if (m_preferencesDemo && hasCookie
                                 && path == "/api/v2/app/preferences")
                        {
                            ++m_preferenceReads;
                            response = QJsonDocument(m_preferences).toJson(QJsonDocument::Compact);
                        }
                        else if (m_preferencesDemo && hasCookie
                                 && path == "/api/v2/app/setPreferences")
                        {
                            m_commands.append({path, body});
                            const QUrlQuery form(QString::fromUtf8(body));
                            const QJsonDocument parsed = QJsonDocument::fromJson(
                                form.queryItemValue(QStringLiteral("json"), QUrl::FullyDecoded).toUtf8());
                            if (!parsed.isObject() || m_rejectPreferences)
                            {
                                code = 400;
                                response = "Rejected";
                            }
                            else
                            {
                                if (!m_ignorePreferences)
                                {
                                    const auto updates = parsed.object();
                                    if (m_webUITransitionDemo)
                                    {
                                        if (updates.contains("web_ui_username"))
                                            m_loginUsername = updates.value("web_ui_username").toString();
                                        if (updates.contains("web_ui_password"))
                                            m_loginPassword = updates.value("web_ui_password").toString();
                                        if (updates.contains("web_ui_port"))
                                        {
                                            const auto port = quint16(updates.value("web_ui_port").toInt());
                                            m_server.close();
                                            m_server.listen(QHostAddress::LocalHost, port);
                                        }
                                    }
                                    for (auto it = updates.constBegin(); it != updates.constEnd(); ++it)
                                        if (m_preferences.contains(it.key()))
                                        {
                                            const QString key = it.key();
                                            const bool fromTime = key == QLatin1String("schedule_from_hour")
                                                || key == QLatin1String("schedule_from_min");
                                            const bool toTime = key == QLatin1String("schedule_to_hour")
                                                || key == QLatin1String("schedule_to_min");
                                            if ((fromTime && (!updates.contains(QStringLiteral("schedule_from_hour"))
                                                    || !updates.contains(QStringLiteral("schedule_from_min"))))
                                                || (toTime && (!updates.contains(QStringLiteral("schedule_to_hour"))
                                                    || !updates.contains(QStringLiteral("schedule_to_min")))))
                                                continue;
                                            const bool directory = it.key() == QLatin1String("save_path")
                                                || it.key() == QLatin1String("temp_path")
                                                || ((it.key() == QLatin1String("export_dir")
                                                    || it.key() == QLatin1String("export_dir_fin"))
                                                    && !it.value().toString().isEmpty());
                                            m_preferences.insert(it.key(), directory && it.value().isString()
                                                ? QJsonValue(QDir::cleanPath(it.value().toString()))
                                                : it.value());
                                            if (it.key() == QLatin1String("listen_port")
                                                && m_preferences.contains(QStringLiteral("random_port")))
                                                m_preferences.insert(QStringLiteral("random_port"), it.value().toInt() == 0);
                                        }
                                }
                                response = "Ok.";
                            }
                        }
                        else if (m_preferencesDemo && hasCookie && fields.value(0) == "POST"
                                 && (path == "/api/v2/app/rotateAPIKey" || path == "/api/v2/app/deleteAPIKey"))
                        {
                            m_commands.append({path, body});
                            if (!m_preferences.value("web_ui_api_key").isString() || m_rejectPreferences)
                            {
                                code = 400;
                                response = "Rejected";
                            }
                            else if (path == "/api/v2/app/rotateAPIKey")
                            {
                                const QString key = QStringLiteral("test-key-%1").arg(commandsFor(path));
                                if (!m_ignorePreferences) m_preferences.insert("web_ui_api_key", key);
                                response = QJsonDocument(QJsonObject{{"apiKey", key}}).toJson(QJsonDocument::Compact);
                            }
                            else
                            {
                                if (!m_ignorePreferences) m_preferences.insert("web_ui_api_key", QString());
                                code = 204;
                            }
                        }
                        else if (m_preferencesDemo && hasCookie && fields.value(0) == "POST"
                                 && path == "/api/v2/app/sendTestEmail")
                        {
                            m_commands.append({path, body});
                            if (m_rejectPreferences)
                            {
                                code = 400;
                                response = "Rejected";
                            }
                            else
                                response = "Ok.";
                        }
                        else if (m_trayDemo && hasCookie
                                 && path == "/api/v2/app/preferences")
                        {
                            response = QJsonDocument(m_traySpeedLimits)
                                .toJson(QJsonDocument::Compact);
                        }
                        else if (m_trayDemo && hasCookie
                                 && path == "/api/v2/app/setPreferences")
                        {
                            m_commands.append({path, body});
                            const QUrlQuery form(QString::fromUtf8(body));
                            const QJsonDocument parsed = QJsonDocument::fromJson(
                                form.queryItemValue(QStringLiteral("json"), QUrl::FullyDecoded).toUtf8());
                            if (!parsed.isObject() || m_rejectCommands)
                            {
                                code = 400;
                                response = "Rejected";
                            }
                            else
                            {
                                const QJsonObject updates = parsed.object();
                                for (auto it = updates.constBegin();
                                     it != updates.constEnd(); ++it)
                                    m_traySpeedLimits.insert(it.key(), it.value());
                                response = "Ok.";
                            }
                        }
                        else if ((path.startsWith("/api/v2/log/main?") || path.startsWith("/api/v2/log/peers?")) && hasCookie)
                        {
                            m_commands.append({path, body});
                            const QUrlQuery query(QString::fromLatin1(path.mid(path.indexOf('?') + 1)));
                            const int last = query.queryItemValue("last_known_id").toInt();
                            const auto entries = path.startsWith("/api/v2/log/main?") ? m_generalLog : m_peerLog;
                            QJsonArray result;
                            for (const auto &entry : entries)
                                if (entry.toObject().value("id").toInt() > last) result.append(entry);
                            response = QJsonDocument(result).toJson(QJsonDocument::Compact);
                        }
                        else if (path.startsWith("/api/v2/app/getDirectoryContent?") && hasCookie)
                        {
                            m_commands.append({path, body});
                            const QUrlQuery query(QString::fromLatin1(path.mid(path.indexOf('?') + 1)));
                            const QString dir = query.queryItemValue("dirPath", QUrl::FullyDecoded);
                            const bool dirs = query.queryItemValue("mode") == "dirs";
                            QJsonArray entries;
                            if (dir == "/browse" || dir == "/browse/Space & 100%")
                            {
                                if (dirs && dir == "/browse")
                                {
                                    if (m_modernResponses) entries.append(QJsonObject{{"name", "Space & 100%"}, {"type", "dir"}, {"last_modification_date", 1700000000}});
                                    else entries.append("/browse/Space & 100%");
                                }
                                if (!dirs && dir != "/browse")
                                {
                                    if (m_modernResponses) entries.append(QJsonObject{{"name", "sample file.txt"}, {"type", "file"}, {"size", 4096}, {"last_modification_date", 1700000000}});
                                    else entries.append("/browse/Space & 100%/sample file.txt");
                                }
                                response = QJsonDocument(entries).toJson(QJsonDocument::Compact);
                            }
                            else { code = 404; response = "Directory does not exist"; }
                        }
                        else if (path.startsWith("/api/v2/app/getFreeSpaceAtPath?") && hasCookie)
                        {
                            m_commands.append({path, body});
                            response = "1073741824";
                        }
                        else if (path == "/api/v2/app/cookies" && hasCookie)
                        {
                            response = QJsonDocument(m_downloadCookies).toJson(QJsonDocument::Compact);
                        }
                        else if (path == "/api/v2/app/setCookies" && hasCookie)
                        {
                            m_commands.append({path, body});
                            const QUrlQuery form(QString::fromUtf8(body));
                            const auto json = QJsonDocument::fromJson(form.queryItemValue("cookies", QUrl::FullyDecoded).toUtf8());
                            if (!json.isArray()) { code = 400; response = "Invalid"; }
                            else { m_downloadCookies = json.array(); code = 204; }
                        }
                        else if (m_sessionPauseDemo && hasCookie
                                 && (path == "/api/v2/transfer/pauseSession" || path == "/api/v2/transfer/resumeSession"))
                        {
                            m_commands.append({path, body});
                            m_sessionPaused = path == "/api/v2/transfer/pauseSession";
                            code = 204;
                        }
                        else if (m_trayDemo && hasCookie
                                 && path.startsWith("/api/v2/transfer/"))
                        {
                            m_commands.append({path, body});
                            if (m_rejectCommands)
                            {
                                code = 400;
                                response = "Rejected";
                            }
                            else
                            {
                                if (path == "/api/v2/transfer/setSpeedLimitsMode")
                                {
                                    const QUrlQuery form(QString::fromUtf8(body));
                                    m_trayAltMode = form.queryItemValue(
                                        QStringLiteral("mode")) == QStringLiteral("1");
                                }
                                else
                                {
                                    code = 404;
                                    response = "Unknown transfer operation";
                                }
                            }
                        }
                        else if (path.startsWith("/api/v2/torrents/") && hasCookie)
                        {
                            m_commands.append({path, body});
                            code = m_rejectCommands ? 400 : 200;
                            response = m_rejectCommands ? QByteArrayLiteral("Rejected")
                                                        : QByteArrayLiteral("Ok.");

                            // Real user-visible effects of content renames
                            // and options changes, not protocol implementation
                            // details such as exact API request counts.
                            const QUrlQuery form(QString::fromUtf8(body));
                            if (!m_sidebarSnapshot.isEmpty() && !m_rejectCommands)
                            {
                                auto categories = m_sidebarSnapshot.value(QStringLiteral("categories")).toObject();
                                auto tags = m_sidebarSnapshot.value(QStringLiteral("tags")).toArray();
                                auto torrents = m_sidebarSnapshot.value(QStringLiteral("torrents")).toObject();
                                auto trackers = m_sidebarSnapshot.value(QStringLiteral("trackers")).toObject();
                                const auto get = [&form](const QString &key) {
                                    return form.queryItemValue(key, QUrl::FullyDecoded);
                                };
                                if (path == "/api/v2/torrents/createCategory" || path == "/api/v2/torrents/editCategory")
                                {
                                    const QString name = get(QStringLiteral("category"));
                                    auto definition = categories.value(name).toObject();
                                    definition.insert(QStringLiteral("name"), name);
                                    if (form.hasQueryItem(QStringLiteral("savePath")))
                                        definition.insert(QStringLiteral("savePath"), get(QStringLiteral("savePath")));
                                    categories.insert(name, definition);
                                }
                                else if (path == "/api/v2/torrents/createTags")
                                {
                                    for (const auto &tag : get(QStringLiteral("tags")).split(QLatin1Char(','), Qt::SkipEmptyParts))
                                        if (!tags.contains(QJsonValue(tag))) tags.append(tag);
                                }
                                else if (path == "/api/v2/torrents/deleteTags")
                                {
                                    QJsonArray remaining;
                                    const auto remove = get(QStringLiteral("tags")).split(QLatin1Char(','));
                                    for (const auto &tag : tags)
                                        if (!remove.contains(tag.toString())) remaining.append(tag);
                                    tags = remaining;
                                }
                                else if (path == "/api/v2/torrents/stop"
                                         || path == "/api/v2/torrents/start"
                                         || path == "/api/v2/torrents/setForceStart")
                                {
                                    for (const auto &hash : get(QStringLiteral("hashes")).split(QLatin1Char('|')))
                                    {
                                        auto torrent = torrents.value(hash).toObject();
                                        if (torrent.isEmpty()) continue;
                                        torrent.insert(QStringLiteral("state"), path == "/api/v2/torrents/stop"
                                            ? QStringLiteral("stoppedUP") : path == "/api/v2/torrents/setForceStart"
                                            ? QStringLiteral("forcedUP") : QStringLiteral("uploading"));
                                        torrents.insert(hash, torrent);
                                    }
                                }
                                else if (path == "/api/v2/torrents/delete")
                                {
                                    for (const auto &hash : get(QStringLiteral("hashes")).split(QLatin1Char('|')))
                                        torrents.remove(hash);
                                }
                                else if (path == "/api/v2/torrents/removeTrackers")
                                {
                                    const auto hash = get(QStringLiteral("hash"));
                                    for (const auto &encoded : get(QStringLiteral("urls")).split(QLatin1Char('|')))
                                    {
                                        const auto url = QUrl::fromPercentEncoding(encoded.toUtf8());
                                        QJsonArray remaining;
                                        for (const auto &member : trackers.value(url).toArray())
                                            if (member.toString() != hash) remaining.append(member);
                                        if (remaining.isEmpty()) trackers.remove(url);
                                        else trackers.insert(url, remaining);
                                    }
                                    auto torrent = torrents.value(hash).toObject();
                                    torrent.insert(QStringLiteral("tracker"), QStringLiteral(""));
                                    torrents.insert(hash, torrent);
                                }
                                m_sidebarSnapshot.insert(QStringLiteral("categories"), categories);
                                m_sidebarSnapshot.insert(QStringLiteral("tags"), tags);
                                m_sidebarSnapshot.insert(QStringLiteral("torrents"), torrents);
                                m_sidebarSnapshot.insert(QStringLiteral("trackers"), trackers);
                            }
                            if (m_statefulContentDemo && !m_rejectCommands
                                && path == "/api/v2/torrents/filePrio")
                            {
                                const QString hash = form.queryItemValue("hash");
                                const auto ids = form.queryItemValue("id", QUrl::FullyDecoded).split(QLatin1Char('|'));
                                const int priority = form.queryItemValue("priority").toInt();
                                QJsonArray updated;
                                for (const auto &value : m_contentEntries.value(hash))
                                {
                                    auto row = value.toObject();
                                    if (ids.contains(QString::number(row.value("index").toInt())))
                                        row.insert("priority", priority);
                                    updated.append(row);
                                }
                                m_contentEntries.insert(hash, updated);
                            }
                            if (m_statefulContentDemo && !m_rejectCommands
                                && (path == "/api/v2/torrents/renameFile"
                                    || path == "/api/v2/torrents/renameFolder"))
                            {
                                const bool folder = path == "/api/v2/torrents/renameFolder";
                                const QString hash = form.queryItemValue(QStringLiteral("hash"));
                                const QString before = form.queryItemValue(
                                    QStringLiteral("oldPath"), QUrl::FullyDecoded);
                                const QString after = form.queryItemValue(
                                    QStringLiteral("newPath"), QUrl::FullyDecoded);
                                QJsonArray updated;
                                bool found = false;
                                for (const QJsonValue &v : m_contentEntries.value(hash))
                                {
                                    QJsonObject row = v.toObject();
                                    const QString name = row.value(QStringLiteral("name")).toString();
                                    if ((!folder && name == before)
                                        || (folder && name.startsWith(before + QLatin1Char('/'))))
                                    {
                                        row.insert(QStringLiteral("name"), after + name.mid(before.size()));
                                        found = true;
                                    }
                                    updated.append(row);
                                }
                                if (!found)
                                {
                                    code = 409;
                                    response = "File not found";
                                }
                                else
                                    m_contentEntries.insert(hash, updated);
                            }
                            if (m_statefulOptionsDemo && !m_rejectCommands)
                            {
                                const QByteArray op = path.mid(QByteArrayLiteral("/api/v2/torrents/").size());
                                const QString hashes = form.queryItemValue(QStringLiteral("hashes"));
                                for (const QString &hash : hashes.split(QLatin1Char('|'), Qt::SkipEmptyParts))
                                {
                                    if (!m_optionStates.contains(hash))
                                        continue;
                                    QJsonObject state = m_optionStates.value(hash);
                                    if (op == "setUploadLimit")
                                        state.insert(QStringLiteral("up_limit"),
                                            form.queryItemValue(QStringLiteral("limit")).toInt());
                                    else if (op == "setDownloadLimit")
                                        state.insert(QStringLiteral("dl_limit"),
                                            form.queryItemValue(QStringLiteral("limit")).toInt());
                                    else if (op == "toggleSequentialDownload")
                                        state.insert(QStringLiteral("seq_dl"),
                                            !state.value(QStringLiteral("seq_dl")).toBool());
                                    else if (op == "toggleFirstLastPiecePrio")
                                        state.insert(QStringLiteral("f_l_piece_prio"),
                                            !state.value(QStringLiteral("f_l_piece_prio")).toBool());
                                    else if (op == "setSuperSeeding")
                                        state.insert(QStringLiteral("super_seeding"),
                                            form.queryItemValue(QStringLiteral("value")) == QStringLiteral("true"));
                                    else if (op == "setAutoManagement")
                                        state.insert(QStringLiteral("auto_tmm"),
                                            form.queryItemValue(QStringLiteral("enable")) == QStringLiteral("true"));
                                    else if (op == "setSavePath")
                                        state.insert(QStringLiteral("save_path"),
                                            form.queryItemValue(QStringLiteral("path")));
                                    else if (op == "setDownloadPath")
                                        state.insert(QStringLiteral("download_path"),
                                            form.queryItemValue(QStringLiteral("path")));
                                    else if (op == "setCategory")
                                        state.insert(QStringLiteral("category"),
                                            form.queryItemValue(QStringLiteral("category")));
                                    else if (op == "setShareLimits")
                                    {
                                        state.insert(QStringLiteral("ratio_limit"),
                                            form.queryItemValue(QStringLiteral("ratioLimit")).toDouble());
                                        state.insert(QStringLiteral("seeding_time_limit"),
                                            form.queryItemValue(QStringLiteral("seedingTimeLimit")).toInt());
                                        state.insert(QStringLiteral("inactive_seeding_time_limit"),
                                            form.queryItemValue(QStringLiteral("inactiveSeedingTimeLimit")).toInt());
                                        state.insert(QStringLiteral("share_limits_mode"),
                                            form.queryItemValue(QStringLiteral("shareLimitsMode")));
                                        state.insert(QStringLiteral("share_limit_action"),
                                            form.queryItemValue(QStringLiteral("shareLimitAction")));
                                    }
                                    m_optionStates.insert(hash, state);
                                }
                            }

                            // For the functional tracker workflow, model the
                            // actual server-side result of Web API mutations.
                            // Decode HTTP form data once, as qBittorrent does,
                            // then observe the final list/tier state via GET.
                            if (m_statefulTrackerDemo && !m_rejectCommands
                                && (path == "/api/v2/torrents/removeTrackers"
                                    || path == "/api/v2/torrents/addTrackers"
                                    || path == "/api/v2/torrents/editTracker"))
                            {
                                const QUrlQuery form(QString::fromUtf8(body));
                                const QString hash =
                                    form.queryItemValue(QStringLiteral("hash"));
                                const QString urls =
                                    form.queryItemValue(QStringLiteral("urls"),
                                                        QUrl::FullyDecoded);
                                QJsonArray current = m_trackerEntries.value(hash);
                                if (path == "/api/v2/torrents/editTracker")
                                {
                                    const QString before = form.queryItemValue(
                                        m_reportedQbtVersion == "v5.1.4" ? QStringLiteral("origUrl") : QStringLiteral("url"),
                                        QUrl::FullyDecoded);
                                    const QString after = form.queryItemValue(
                                        QStringLiteral("newUrl"), QUrl::FullyDecoded);
                                    QJsonArray edited;
                                    bool found = false;
                                    for (QJsonValue item : current)
                                    {
                                        QJsonObject row = item.toObject();
                                        if (row.value(QStringLiteral("url")).toString() == before)
                                        {
                                            if (form.hasQueryItem(QStringLiteral("newUrl")))
                                                row.insert(QStringLiteral("url"), after);
                                            if (form.hasQueryItem(QStringLiteral("tier")))
                                                row.insert(QStringLiteral("tier"), form.queryItemValue(
                                                    QStringLiteral("tier")).toInt());
                                            item = row;
                                            found = true;
                                        }
                                        edited.append(item);
                                    }
                                    if (found)
                                    {
                                        m_trackerEntries.insert(hash, edited);
                                        // Upstream private-torrent replaceTrackers()
                                        // clears old peers on the remote server.
                                        m_peerConnections.insert(hash, 0);
                                    }
                                    else
                                    {
                                        code = 409;
                                        response = QByteArrayLiteral("Tracker not found");
                                    }
                                }
                                else if (path == "/api/v2/torrents/removeTrackers")
                                {
                                    QSet<QString> removed;
                                    for (const QString &part : urls.split(QLatin1Char('|'),
                                                                           Qt::SkipEmptyParts))
                                        removed.insert(QUrl::fromPercentEncoding(
                                            part.toUtf8()));
                                    QJsonArray kept;
                                    for (const QJsonValue &item : current)
                                        if (!removed.contains(item.toObject()
                                                .value(QStringLiteral("url")).toString()))
                                            kept.append(item);
                                    m_trackerEntries.insert(hash, kept);
                                }
                                else
                                {
                                    int tier = 0;
                                    bool afterGroup = false;
                                    for (const QString &raw : urls.split(QLatin1Char('\n')))
                                    {
                                        const QString line = raw.trimmed();
                                        if (line.isEmpty())
                                        {
                                            if (!current.isEmpty())
                                                afterGroup = true;
                                            continue;
                                        }
                                        if (afterGroup)
                                        {
                                            ++tier;
                                            afterGroup = false;
                                        }
                                        // Readable tracker URLs must survive the HTTP
                                        // form decoding as complete individual entries.
                                        current.append(QJsonObject{
                                            {QStringLiteral("url"), line},
                                            {QStringLiteral("tier"), tier}});
                                    }
                                    m_trackerEntries.insert(hash, current);
                                }
                            }
                        }
                        else if (path == "/api/v2/auth/logout" && hasCookie)
                        {
                            ++m_logouts;
                            if (m_historicalConnection)
                            {
                                m_historicalAuthenticated = false;
                                cookieHeader = "Set-Cookie: " + m_sessionCookie
                                    + "=; Path=/; Max-Age=0\r\n";
                                // Historical logoutAction ends the session;
                                // do not fabricate a login-style Ok. body.
                                response.clear();
                            }
                            else response = "Ok.";
                        }
                        else
                        {
                            code = 403;
                            response = "Forbidden";
                        }

                        if (m_modernResponses && code == 200 && fields.value(0) == "POST"
                            && response == "Ok.")
                        {
                            code = 204;
                            response.clear();
                        }
                        QByteArray result = "HTTP/1.1 " + QByteArray::number(code) +
                            (code == 200 ? " OK\r\n" : code == 204 ? " No Content\r\n"
                                : code == 401 ? " Unauthorized\r\n" : " Forbidden\r\n");
                        result += "Content-Type: text/plain\r\n";
                        result += "Content-Length: " + QByteArray::number(response.size()) + "\r\n";
                        result += cookieHeader;
                        result += "Connection: close\r\n\r\n";
                        result += response;
                        if (m_preferencesDemo && (path == "/api/v2/app/preferences"
                                || path == "/api/v2/app/setPreferences"))
                        {
                            // A delayed response lets GUI tests observe the
                            // in-flight state rather than optimistic success.
                            QTimer::singleShot(150, socket, [socket, result]() {
                                socket->write(result);
                                socket->disconnectFromHost();
                            });
                        }
                        else
                        {
                            socket->write(result);
                            socket->disconnectFromHost();
                        }
                    });
                connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
            }
        });
    }

    bool start() { return m_server.listen(QHostAddress::LocalHost, 0); }
    QString url() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()); }
    int logouts() const noexcept { return m_logouts; }
    int syncRequests() const noexcept { return m_syncRequests; }
    void rejectSync(bool reject) { m_rejectSync = reject; }
    void enableTorrentSyncDemo() { m_demoTorrents = true; }
    void setSidebarSnapshot(const QJsonObject &snapshot) { m_sidebarSnapshot = snapshot; }
    void setCompletionSnapshot(const QJsonObject &snapshot)
    {
        m_completionSyncDemo = true;
        m_completionSnapshot = snapshot;
    }
    void queueCompletionPatch(const QJsonObject &patch) { m_completionPatches.append(patch); }
    QJsonObject sidebarSnapshot() const { return m_sidebarSnapshot; }
    void setStatusRates(const QJsonObject &rates)
    {
        m_statusRatesDemo = true;
        m_statusRates = rates;
    }
    void enableNativeTrayDemo(bool alternative = false)
    {
        m_trayDemo = true;
        m_trayAltMode = alternative;
        m_traySpeedLimits = QJsonObject{
            {QStringLiteral("up_limit"), 50 * 1024},
            {QStringLiteral("dl_limit"), 100 * 1024},
            {QStringLiteral("alt_up_limit"), 20 * 1024},
            {QStringLiteral("alt_dl_limit"), 30 * 1024}
        };
    }
    void enableWebUITransitionDemo() { m_webUITransitionDemo = true; }
    void setServerPreferences(const QJsonObject &values)
    {
        m_preferencesDemo = true;
        m_preferences = values;
    }
    QJsonObject serverPreferences() const { return m_preferences; }
    void setRssData(const QJsonObject &items, const QJsonObject &rules)
    {
        m_rssDemo = true;
        m_rssItems = items;
        m_rssRules = rules;
    }
    QJsonObject rssItems() const { return m_rssItems; }
    QJsonObject rssRules() const { return m_rssRules; }
    void setBuildInfo(const QJsonObject &info) { m_buildInfo = info; }
    void setProcessInfo(const QJsonObject &info) { m_processInfo = info; }
    int processInfoRequests() const noexcept { return m_processInfoRequests; }
    void enableCreatorDemo(bool hold = false) { m_creatorDemo = true; m_creatorHold = hold; }
    int creatorDownloads() const { return m_creatorDownloads; }
    bool creatorActive() const { return m_creatorActive; }
    void setNetworkLists(const QJsonArray &interfaces, const QHash<QString, QJsonArray> &addresses)
    { m_networkInterfaces = interfaces; m_networkAddresses = addresses; }
    int preferenceReads() const { return m_preferenceReads; }
    void rejectPreferenceWrites(bool reject) { m_rejectPreferences = reject; }
    void ignorePreferenceWrites(bool ignore) { m_ignorePreferences = ignore; }
    bool trayAlternative() const { return m_trayAltMode; }
    void setLogEntries(const QJsonArray &general, const QJsonArray &peers) { m_generalLog = general; m_peerLog = peers; }
    void setDownloadCookies(const QJsonArray &cookies) { m_downloadCookies = cookies; }
    QJsonArray downloadCookies() const { return m_downloadCookies; }
    void enableSessionPauseDemo() { m_sessionPauseDemo = true; }
    void setSessionPaused(bool paused) { m_sessionPaused = paused; }
    bool sessionPaused() const { return m_sessionPaused; }
    void setTrayConnectionStatus(const QString &status) { m_trayConnectionStatus = status; }
    QJsonObject trayLimits() const { return m_traySpeedLimits; }
    void enableModernResponses()
    {
        m_modernResponses = true;
        setReportedVersions(QStringLiteral("v5.3.0"), QStringLiteral("2.16.0"));
    }
    void setReportedVersions(const QString &qbtVersion, const QString &apiVersion)
    {
        m_reportedQbtVersion = qbtVersion.toUtf8();
        m_reportedApiVersion = apiVersion.toUtf8();
    }
    QString reportedQbtVersion() const { return QString::fromUtf8(m_reportedQbtVersion); }
    QString reportedApiVersion() const { return QString::fromUtf8(m_reportedApiVersion); }
    void enableQueueingDemo() { m_queueingEnabled = true; }
    void setSavingManagementTorrent(bool automatic) { m_savingManagementTorrent = automatic; }
    void setExistingTorrentHash(const QString &hash) { m_existingTorrentHash = hash; }
    void setExistingTorrentPrivate(bool value) { m_existingTorrentPrivate = value; }
    void enableNativePresentationDemo() { m_demoNativePresentation = true; }
    void queueSwarmCountsUpdate() { m_pendingSwarmCountsUpdate = true; }
    void enableDetailsDemo() { m_demoDetails = true; }
    void enableDistinctTrackerPasskeyDemo() { m_distinctTrackerPasskeys = true; }
    void enableRemoteContentAndOptionsDemo()
    {
        m_statefulContentDemo = true;
        m_statefulOptionsDemo = true;
        const QString first(40, QLatin1Char('a'));
        const QString second(40, QLatin1Char('b'));
        m_contentEntries.insert(first, QJsonArray{
            QJsonObject{{QStringLiteral("index"), 0},
                        {QStringLiteral("name"), QStringLiteral("music/song-one.flac")},
                        {QStringLiteral("size"), 1024}},
            QJsonObject{{QStringLiteral("index"), 1},
                        {QStringLiteral("name"), QStringLiteral("music/song-two.flac")},
                        {QStringLiteral("size"), 2048}}
        });
        QJsonObject firstState{
            {QStringLiteral("name"), QStringLiteral("Album one")},
            {QStringLiteral("auto_tmm"), false},
            {QStringLiteral("save_path"), QStringLiteral("/share/NAS/torrents")},
            {QStringLiteral("download_path"), QString()},
            {QStringLiteral("category"), QStringLiteral("music")},
            {QStringLiteral("up_limit"), 512 * 1024},
            {QStringLiteral("dl_limit"), 1024 * 1024},
            {QStringLiteral("ratio_limit"), -2},
            {QStringLiteral("seeding_time_limit"), -2},
            {QStringLiteral("inactive_seeding_time_limit"), -2},
            {QStringLiteral("share_limits_mode"), QStringLiteral("Default")},
            {QStringLiteral("share_limit_action"), QStringLiteral("Default")},
            {QStringLiteral("seq_dl"), false},
            {QStringLiteral("f_l_piece_prio"), false},
            {QStringLiteral("super_seeding"), false},
            {QStringLiteral("private"), false}
        };
        m_optionStates.insert(first, firstState);
        firstState.insert(QStringLiteral("ratio_limit"), 2.0);
        firstState.insert(QStringLiteral("up_limit"), 256 * 1024);
        m_optionStates.insert(second, firstState);
    }
    QJsonArray contentEntries(const QString &hash) const
    {
        return m_contentEntries.value(hash);
    }
    void setContentEntries(const QString &hash, const QJsonArray &files)
    {
        m_contentEntries.insert(hash, files);
    }
    QJsonObject optionState(const QString &hash) const
    {
        return m_optionStates.value(hash);
    }
    QByteArray exportedTorrent() const { return m_exportedTorrent; }

    void enableStatefulTrackerEditingDemo()
    {
        m_statefulTrackerDemo = true;
        const QString first(40, QLatin1Char('a'));
        const QString second(40, QLatin1Char('b'));
        m_trackerEntries.insert(first, QJsonArray{QJsonObject{
            {QStringLiteral("url"), QStringLiteral(
                "https://tracker.example/announce/OLD_PASSKEY_ALPHA")},
            {QStringLiteral("tier"), 0}}});
        m_trackerEntries.insert(second, QJsonArray{QJsonObject{
            {QStringLiteral("url"), QStringLiteral(
                "https://tracker.example/announce/OLD_PASSKEY_BETA")},
            {QStringLiteral("tier"), 0}}});
    }
    QJsonArray trackerEntries(const QString &hash) const
    {
        return m_trackerEntries.value(hash);
    }
    void setTrackerEntries(const QString &hash, const QJsonArray &entries)
    {
        m_trackerEntries.insert(hash, entries);
    }
    void setPeerConnections(const QString &hash, int count)
    {
        m_peerConnections.insert(hash, count);
    }
    int peerConnections(const QString &hash) const
    {
        return m_peerConnections.value(hash);
    }
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
    void enableMappedFolderDemo() { m_mappedFolderDemo = true; }
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
    bool rejectsCapabilityRequest(const QByteArray &path, const QByteArray &method,
                                  const QByteArray &body, int &status) const
    {
        if (m_capabilityProfile == CapabilityProfile::Synthetic) return false;
        // Independent upstream-derived oracle. Old reannounce ignores urls;
        // the fixture deliberately flags that prohibited client request
        // rather than modelling a broader reannounce as a success.
        const bool earliest = m_capabilityProfile == CapabilityProfile::V410;
        const bool modernTracker = m_capabilityProfile == CapabilityProfile::V520
            || m_capabilityProfile == CapabilityProfile::V530;
        const bool pathSpace = m_capabilityProfile == CapabilityProfile::V530;
        const QUrlQuery form(QString::fromUtf8(body));
        const auto reject = [&status](int code) { status = code; return true; };
        if (path == "/api/v2/torrents/editTracker")
        {
            if (earliest) return reject(404);
            if (method != "POST" || !form.hasQueryItem("hash")) return reject(400);
            if (modernTracker)
            {
                if (!form.hasQueryItem("url") || form.hasQueryItem("origUrl")
                    || (!form.hasQueryItem("newUrl") && !form.hasQueryItem("tier")))
                    return reject(400);
            }
            else if (!form.hasQueryItem("origUrl") || !form.hasQueryItem("newUrl")
                     || form.hasQueryItem("url") || form.hasQueryItem("tier"))
                return reject(400);
        }
        else if (path == "/api/v2/torrents/setComment")
        {
            if (!modernTracker) return reject(404);
            if (method != "POST" || !form.hasQueryItem("hashes") || !form.hasQueryItem("comment"))
                return reject(400);
        }
        else if (path == "/api/v2/torrents/reannounce")
        {
            if (earliest) return reject(404);
            if (method != "POST" || !form.hasQueryItem("hashes")
                || (!modernTracker && form.hasQueryItem("urls"))) return reject(400);
        }
        else if (path.startsWith("/api/v2/torrents/pieceAvailability?"))
        {
            if (!modernTracker) return reject(404);
            const QUrlQuery query(QString::fromUtf8(path.mid(path.indexOf('?') + 1)));
            if (method != "GET" || !query.hasQueryItem("hash")) return reject(400);
        }
        else if (path.startsWith("/api/v2/app/getFreeSpaceAtPath?"))
        {
            if (!pathSpace) return reject(404);
            const QUrlQuery query(QString::fromUtf8(path.mid(path.indexOf('?') + 1)));
            if (method != "GET" || !query.hasQueryItem("path")) return reject(400);
        }
        return false;
    }

    CapabilityProfile m_capabilityProfile = CapabilityProfile::Synthetic;
    int m_prohibitedCapabilityRequests = 0;
    QTcpServer m_server;
    bool m_acceptPassword;
    bool m_acceptBlank;
    bool m_modernResponses = false;
    bool m_historicalConnection = false;
    bool m_historicalAuthenticated = false;
    bool m_listCategories = false;
    int m_invalidHistoricalSyncReplies = 0;
    QByteArray m_sessionCookie = QByteArrayLiteral("QBT_SID");
    QByteArray m_reportedQbtVersion = QByteArrayLiteral("v5.1.4");
    QByteArray m_reportedApiVersion = QByteArrayLiteral("2.11.3");
    int m_logouts = 0;
    bool m_demoTorrents = false;
    QJsonObject m_sidebarSnapshot;
    bool m_completionSyncDemo = false;
    QJsonObject m_completionSnapshot;
    QList<QJsonObject> m_completionPatches;
    bool m_statusRatesDemo = false;
    QJsonObject m_statusRates;
    bool m_trayDemo = false;
    bool m_trayAltMode = false;
    QJsonArray m_downloadCookies;
    QJsonArray m_generalLog, m_peerLog;
    bool m_sessionPauseDemo = false;
    bool m_sessionPaused = false;
    QString m_trayConnectionStatus = QStringLiteral("connected");
    QJsonObject m_traySpeedLimits;
    bool m_rssDemo = false;
    QJsonObject m_rssItems, m_rssRules;
    bool m_webUITransitionDemo = false;
    QString m_loginUsername = QStringLiteral("demo");
    QString m_loginPassword = QStringLiteral("demo-password");
    bool m_preferencesDemo = false;
    bool m_rejectPreferences = false;
    bool m_ignorePreferences = false;
    int m_preferenceReads = 0;
    QJsonObject m_preferences;
    QJsonObject m_buildInfo;
    QJsonObject m_processInfo;
    int m_processInfoRequests = 0;
    bool m_creatorDemo = false;
    bool m_creatorHold = false;
    bool m_creatorActive = false;
    int m_creatorPolls = 0;
    int m_creatorDownloads = 0;
    QJsonArray m_networkInterfaces;
    QHash<QString, QJsonArray> m_networkAddresses;
    bool m_queueingEnabled = false;
    bool m_mappedFolderDemo = false;
    QJsonValue m_savingManagementTorrent;
    QString m_existingTorrentHash;
    bool m_existingTorrentPrivate = false;
    bool m_demoNativePresentation = false;
    bool m_pendingSwarmCountsUpdate = false;
    bool m_demoDetails = false;
    bool m_distinctTrackerPasskeys = false;
    bool m_statefulTrackerDemo = false;
    bool m_statefulContentDemo = false;
    bool m_statefulOptionsDemo = false;
    QHash<QString, QJsonArray> m_contentEntries;
    QHash<QString, QJsonObject> m_optionStates;
    QByteArray m_exportedTorrent = QByteArrayLiteral(
        "d4:infod6:lengthi1e4:name4:testee");
    QHash<QString, QJsonArray> m_trackerEntries;
    QHash<QString, int> m_peerConnections;
    QList<QByteArray> m_detailRequests;
    QHash<QString, QString> m_torrentComments;
    QHash<QString, QStringList> m_webSeeds;
    bool m_keepDemoTorrents = false;
    bool m_rejectCommands = false;
    int m_syncRequests = 0;
    bool m_rejectSync = false;
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
                const auto cleanup = qScopeGuard([dialog]() {
                    if (dialog->isVisible()) dialog->reject();
                });
                QVERIFY(buttons && buttons->button(QDialogButtonBox::Ok));
                QTRY_VERIFY_WITH_TIMEOUT(buttons->button(QDialogButtonBox::Ok)->isEnabled(), 6000);
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
                const auto cleanup = qScopeGuard([dialog]() {
                    if (dialog->isVisible()) dialog->reject();
                });
                QTRY_VERIFY_WITH_TIMEOUT(buttons->button(QDialogButtonBox::Ok)->isEnabled(), 6000);
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

// Drive the real Add Torrent options dialog without a file picker. The
// drag gesture should always use the same authenticated native dialog.
class DroppedTorrentUi final : public QObject
{
public:
    int opened = 0;
    int batches = 0;
    int links = 0;
    QString unexpected;
protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (event->type() != QEvent::Show)
            return QObject::eventFilter(object, event);
        if (auto *picker = qobject_cast<QFileDialog *>(object);
            picker && picker->objectName() == QLatin1String("addTorrentFileDialog"))
            unexpected = QStringLiteral("A file picker opened during a desktop drop");
        if (auto *dialog = qobject_cast<RemoteAddTorrentDialog *>(object))
        {
            ++opened;
            QTimer::singleShot(0, dialog, [this, dialog]() {
                if (dialog->findChild<QGroupBox *>(QStringLiteral("torrentBatchGroup")))
                    ++batches;
                if (dialog->findChild<QPlainTextEdit *>(QStringLiteral("torrentLinks")))
                    ++links;
                auto *buttons = dialog->findChild<QDialogButtonBox *>(
                    QStringLiteral("buttonBox"));
                if (!buttons || !buttons->button(QDialogButtonBox::Ok))
                {
                    unexpected = QStringLiteral("Native Add Torrent confirmation missing");
                    dialog->reject();
                    return;
                }
                const auto cleanup = qScopeGuard([dialog]() {
                    if (dialog->isVisible()) dialog->reject();
                });
                QTRY_VERIFY_WITH_TIMEOUT(buttons->button(QDialogButtonBox::Ok)->isEnabled(), 6000);
                buttons->button(QDialogButtonBox::Ok)->click();
            });
        }
        if (auto *message = qobject_cast<QMessageBox *>(object))
        {
            unexpected = QStringLiteral("Unexpected warning during drop: ")
                + message->windowTitle() + QLatin1String(": ") + message->text();
            QTimer::singleShot(0, message, &QMessageBox::reject);
        }
        return QObject::eventFilter(object, event);
    }
};

class ConnectionWorkflowTest final : public QObject
{
    Q_OBJECT

private:
    QList<QUrl> m_fileOpenRequests;

private slots:
    void recordFileOpenRequest(const QUrl &url) { m_fileOpenRequests.append(url); }

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
    void lockedReleaseCapabilityPolicy()
    {
        using Caps = Remote::ServerCapabilities;
        using Feature = Caps::Feature;
        using Status = Caps::Status;
        using Syntax = Caps::TrackerEditSyntax;
        QFile file(QString::fromUtf8(QBT_COMPATIBILITY_RELEASE_LOCK));
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.errorString()));
        const auto releases = QJsonDocument::fromJson(file.readAll()).object().value("releases").toArray();
        QCOMPARE(releases.size(), qsizetype(68));
        // Independent expected stable-release boundaries, not resolver output.
        QSet<QString> seen;
        for (const auto &entry : releases)
        {
            const auto release = entry.toObject();
            const QString version = release.value("version").toString();
            QVERIFY(!seen.contains(version));
            seen.insert(version);
            QString api = release.value("declared_api_version").toString();
            if (version == "4.1.0") api = "2.0"; // observed live wire value
            const auto caps = Caps::fromVersions("v" + version, api);
            QVERIFY2(caps.hasKnownRelease() && caps.hasWebApiV2(), qPrintable(version));
            QVERIFY2(caps.versionWarning().isEmpty(), qPrintable(version));
            const bool modern = version.startsWith("5.2.") || version.startsWith("5.3.");
            const bool newest = version.startsWith("5.3.");
            const bool beforeTrackerEdit = version == "4.1.0" || version == "4.1.1"
                || version == "4.1.2" || version == "4.1.3" || version == "4.1.4";
            QCOMPARE(caps.trackerEditSyntax(), beforeTrackerEdit ? Syntax::Unsupported
                : modern ? Syntax::ModernUrl : Syntax::LegacyOrigUrl);
            QCOMPARE(caps.status(Feature::TrackerUrlEditing), beforeTrackerEdit ? Status::Unsupported : Status::Supported);
            for (auto feature : {Feature::TrackerTierEditing, Feature::SelectedTrackerReannounce,
                                 Feature::PieceAvailability, Feature::CommentEditing})
                QCOMPARE(caps.status(feature), modern ? Status::Supported : Status::Unsupported);
            // Preferences are regression policy, not yet audited per-setting contracts.
            for (auto feature : {Feature::PathFreeSpace, Feature::AdvancedModernPreferences, Feature::BanRanges})
                QCOMPARE(caps.status(feature), newest ? Status::Supported : Status::Unsupported);
        }
        QCOMPARE(seen.size(), qsizetype(68));

        for (const auto *daemon : {"v5.3.0-beta1", "v5.3.0junk", "v5.3.2", "v5.2.99", "v4.0.4", "v5.3", ""})
        {
            const auto caps = Caps::fromVersions(QString::fromLatin1(daemon), "2.16.2");
            QCOMPARE(caps.status(Feature::CommentEditing), Status::Unknown);
            QCOMPARE(caps.trackerEditSyntax(), Syntax::Unknown);
            QVERIFY(!caps.versionWarning().isEmpty());
        }
        for (const auto *api : {"2.16.2junk", "2.16.2.1", "3.0.0", "2", "", "2.16.99999999999999999999999999"})
        {
            const auto caps = Caps::fromVersions("v5.3.0", QString::fromLatin1(api));
            QCOMPARE(caps.status(Feature::TrackerTierEditing), Status::Unknown);
            QCOMPARE(caps.status(Feature::PathFreeSpace), Status::Unknown);
            QCOMPARE(caps.trackerEditSyntax(), Syntax::Unknown);
        }
        const auto contradictory = Caps::fromVersions("v5.1.4", "2.16.2");
        QCOMPARE(contradictory.status(Feature::TrackerTierEditing), Status::Unknown);
        QCOMPARE(contradictory.trackerEditSyntax(), Syntax::Unknown);
        QVERIFY(!contradictory.versionWarning().isEmpty());
        const auto shortApi = Caps::fromVersions("v5.3.0", "2.16");
        QCOMPARE(shortApi.apiVersion(), QStringLiteral("2.16"));
        QCOMPARE(shortApi.status(Feature::PathFreeSpace), Status::Supported);
        const auto empty = Caps{};
        QCOMPARE(empty.status(Feature::CommentEditing), Status::Unknown);
        QVERIFY(!empty.unavailableReason(Feature::CommentEditing).isEmpty());
    }

    void mixedHistoricalCapabilityDispatch()
    {
        using Caps = Remote::ServerCapabilities;
        using Feature = Caps::Feature;
        using Status = Caps::Status;
        using Profile = DemoQbittorrentServer::CapabilityProfile;
        DemoQbittorrentServer oldest(true), older(true), boundary(true), current(true), unknown(true);
        oldest.setCapabilityProfile(Profile::V410);
        older.setCapabilityProfile(Profile::V514);
        boundary.setCapabilityProfile(Profile::V520);
        current.setCapabilityProfile(Profile::V530);
        unknown.setReportedVersions("v5.3.0-beta1", "2.16.2");
        const QString hash(40, QLatin1Char('a'));
        const QString original = "https://tracker.example/announce?token=a%2Bb";
        const QString replacement = "https://replacement.example/announce?x=+";
        const QJsonArray initial{QJsonObject{{"url", original}, {"tier", 0}}};
        QObject lifetime;
        auto connectServer = [&](DemoQbittorrentServer &server) -> Remote::RemoteSession * {
            server.enableStatefulTrackerEditingDemo();
            server.setTrackerEntries(hash, initial);
            if (!server.start()) return nullptr;
            auto *session = new Remote::RemoteSession(&lifetime);
            Remote::ServerProfile profile;
            profile.url = server.url();
            profile.username = "demo";
            profile.name = "Compatibility";
            session->connectToServer(profile, "demo-password");
            return session;
        };
        auto *old = connectServer(oldest);
        auto *legacy = connectServer(older);
        auto *modern = connectServer(boundary);
        auto *latest = connectServer(current);
        auto *unrecognized = connectServer(unknown);
        QVERIFY(old && legacy && modern && latest && unrecognized);
        QTRY_VERIFY_WITH_TIMEOUT(old->isConnected() && legacy->isConnected()
            && modern->isConnected() && latest->isConnected() && unrecognized->isConnected(), 10000);

        QCOMPARE(old->capabilities().apiVersion(), QStringLiteral("2.0"));
        QCOMPARE(old->capabilities().status(Feature::TrackerUrlEditing), Status::Unsupported);
        QCOMPARE(legacy->capabilities().status(Feature::TrackerTierEditing), Status::Unsupported);
        QCOMPARE(modern->capabilities().status(Feature::TrackerTierEditing), Status::Supported);
        QCOMPARE(modern->capabilities().status(Feature::PathFreeSpace), Status::Unsupported);
        QCOMPARE(latest->capabilities().status(Feature::PathFreeSpace), Status::Supported);
        QCOMPARE(unrecognized->capabilities().status(Feature::TrackerUrlEditing), Status::Unknown);

        QSignalSpy oldErrors(old, &Remote::RemoteSession::operationFailed);
        QSignalSpy legacyErrors(legacy, &Remote::RemoteSession::operationFailed);
        QSignalSpy unknownErrors(unrecognized, &Remote::RemoteSession::operationFailed);
        old->editTorrentTracker(hash, original, replacement);
        old->setTorrentComment(hash, "blocked");
        legacy->setTrackerTiers(hash, QJsonObject{{original, 1}});
        legacy->setTorrentComment(hash, "blocked");
        unrecognized->editTorrentTracker(hash, original, replacement);
        unrecognized->setTorrentComment(hash, "blocked");
        unrecognized->replaceTorrentTrackers(hash, original, replacement, true);
        QCOMPARE(oldErrors.size(), 2);
        QCOMPARE(legacyErrors.size(), 2);
        QCOMPARE(unknownErrors.size(), 3);
        QVERIFY(oldErrors.first().at(1).toString().contains("not supported"));
        QVERIFY(unknownErrors.first().at(1).toString().contains("unknown"));

        const QByteArray editRoute("/api/v2/torrents/editTracker");
        const QByteArray commentRoute("/api/v2/torrents/setComment");
        legacy->editTorrentTracker(hash, original, replacement);
        modern->editTorrentTracker(hash, original, replacement);
        modern->setTorrentComment(hash, "Modern only");
        QTRY_COMPARE_WITH_TIMEOUT(older.commandsFor(editRoute), 1, 6000);
        QTRY_COMPARE_WITH_TIMEOUT(boundary.commandsFor(editRoute), 1, 6000);
        QTRY_COMPARE_WITH_TIMEOUT(boundary.commandsFor(commentRoute), 1, 6000);
        const QUrlQuery legacyForm(QString::fromUtf8(older.lastCommandBody(editRoute)));
        const QUrlQuery modernForm(QString::fromUtf8(boundary.lastCommandBody(editRoute)));
        QCOMPARE(legacyForm.queryItemValue("origUrl", QUrl::FullyDecoded), original);
        QCOMPARE(modernForm.queryItemValue("url", QUrl::FullyDecoded), original);
        QVERIFY(!legacyForm.hasQueryItem("url") && !modernForm.hasQueryItem("origUrl"));
        QTRY_COMPARE_WITH_TIMEOUT(older.trackerEntries(hash).first().toObject().value("url").toString(), replacement, 6000);
        QTRY_COMPARE_WITH_TIMEOUT(boundary.trackerEntries(hash).first().toObject().value("url").toString(), replacement, 6000);

        bool refused = false;
        modern->requestPathFreeSpace("/browse", {}, [&](const QString &) { refused = true; });
        QVERIFY(refused);
        qint64 freeBytes = -1;
        latest->requestPathFreeSpace("/browse", [&](qint64 bytes) { freeBytes = bytes; }, {});
        QTRY_COMPARE_WITH_TIMEOUT(freeBytes, qint64(1073741824), 6000);
        QCOMPARE(oldest.commandsFor(editRoute), 0);
        QCOMPARE(oldest.commandsFor(commentRoute), 0);
        QCOMPARE(older.commandsFor(commentRoute), 0);
        QCOMPARE(unknown.commandsFor(editRoute), 0);
        QCOMPARE(unknown.commandsFor(commentRoute), 0);
        QCOMPARE(unknown.commandsFor("/api/v2/torrents/removeTrackers"), 0);
        QCOMPARE(unknown.commandsFor("/api/v2/torrents/addTrackers"), 0);
        for (auto *server : {&oldest, &older, &boundary, &current})
            QCOMPARE(server->prohibitedCapabilityRequests(), 0);

        QPointer<Remote::RemoteSession> oldGuard(old);
        old->disconnectFromServer();
        if (oldGuard)
            QCOMPARE(oldGuard->capabilities().status(Feature::TrackerUrlEditing), Status::Unknown);
        QTRY_VERIFY_WITH_TIMEOUT(oldGuard.isNull(), 6000);
        QVERIFY(modern->isConnected() && latest->isConnected());
        QCOMPARE(modern->capabilities().status(Feature::TrackerTierEditing), Status::Supported);
    }

    void nativeTrayMenuControlsRemoteSessionAndSpeedLimitsWhileHidden()
    {
        DemoQbittorrentServer server(true);
        server.enableNativeTrayDemo();
        QVERIFY(server.start());

        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("tray-actions.ini")),
                           QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        QTRY_VERIFY(window.isVisible());

        auto *menu = window.findChild<QMenu *>(QStringLiteral("remoteTrayMenu"));
        auto *show = window.findChild<QAction *>(QStringLiteral("trayShowWindow"));
        auto *file = window.findChild<QAction *>(QStringLiteral("actionOpen"));
        auto *link = window.findChild<QAction *>(QStringLiteral("actionDownloadFromURL"));
        auto *alternate = window.findChild<QAction *>(
            QStringLiteral("actionUseAlternativeSpeedLimits"));
        auto *limits = window.findChild<QAction *>(
            QStringLiteral("actionSetGlobalSpeedLimits"));
        auto *disconnectMenu = window.findChild<QMenu *>(QStringLiteral("trayDisconnectMenu"));
        auto *exit = window.findChild<QAction *>(QStringLiteral("actionExit"));
        QVERIFY(menu && show && file && link && alternate && limits && disconnectMenu && exit);
        QVERIFY(!disconnectMenu->isEnabled());

        // Open the actual Qt menu used by the system-tray icon. The native
        // .ui actions are reused, rather than unrelated fake tray callbacks.
        menu->popup(QPoint(25, 25));
        QTRY_VERIFY(menu->isVisible());
        QCOMPARE(menu->actions().first(), show);
        QVERIFY(menu->actions().contains(file));
        QVERIFY(menu->actions().contains(link));
        QVERIFY(menu->actions().contains(alternate));
        QVERIFY(menu->actions().contains(limits));
        QVERIFY(menu->actions().contains(disconnectMenu->menuAction()));
        QVERIFY(!menu->actions().contains(window.findChild<QAction *>(
            QStringLiteral("actionPauseSession"))));
        QVERIFY(!menu->actions().contains(window.findChild<QAction *>(
            QStringLiteral("actionResumeSession"))));
        QCOMPARE(menu->actions().last(), exit);
        QVERIFY(!file->icon().isNull());
        QVERIFY(!link->icon().isNull());
        QVERIFY(!limits->icon().isNull());
        QVERIFY(!disconnectMenu->icon().isNull());
        menu->close();

        openConnectDialog(window, server.url());
        QTRY_VERIFY_WITH_TIMEOUT(alternate->isEnabled() && limits->isEnabled()
            && disconnectMenu->isEnabled(), 7000);
        QVERIFY(!alternate->isChecked());

        // The system-tray entry points remain fully functional when the main
        // window is hidden. In CI, menu popup is simulated without an OS tray.
        window.hide();
        QVERIFY(!window.isVisible());

        QTimer::singleShot(0, &window, []() {
            if (auto *dlg = qobject_cast<RemoteAddTorrentDialog *>(
                    QApplication::activeModalWidget()))
                dlg->reject();
        });
        link->trigger();
        QVERIFY(!window.isVisible());

        QTimer::singleShot(0, &window, []() {
            if (auto *picker = qobject_cast<QFileDialog *>(
                    QApplication::activeModalWidget()))
                picker->reject();
        });
        file->trigger();
        QVERIFY(!window.isVisible());

        alternate->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(server.trayAlternative(), 7000);
        QTRY_VERIFY_WITH_TIMEOUT(alternate->isChecked(), 7000);
        QCOMPARE(server.commandsFor("/api/v2/transfer/setSpeedLimitsMode"), 1);

        // The original qBittorrent 5.3.0 Speed Limit dialog must read all
        // four server settings before Save becomes enabled. Edit one value,
        // then assert only that one change reached the NAS fake server.
        QTimer poll;
        poll.setInterval(20);
        QString dialogError;
        int opened = 0;
        connect(&poll, &QTimer::timeout, &window, [&]() {
            auto *dlg = qobject_cast<RemoteSpeedLimitsDialog *>(
                QApplication::activeModalWidget());
            if (!dlg) return;
            auto *buttons = dlg->findChild<QDialogButtonBox *>(
                QStringLiteral("buttonBox"));
            auto *upload = dlg->findChild<QSpinBox *>(
                QStringLiteral("spinUploadLimit"));
            auto *download = dlg->findChild<QSpinBox *>(
                QStringLiteral("spinDownloadLimit"));
            if (!buttons || !upload || !download) {
                dialogError = QStringLiteral("Original speed dialog widgets missing");
                dlg->reject();
                poll.stop();
                return;
            }
            if (!buttons->button(QDialogButtonBox::Ok)->isEnabled())
                return; // Remote preferences still loading.
            ++opened;
            if (upload->value() != 50 || download->value() != 100)
                dialogError = QStringLiteral("Speed limits did not load from the server");
            else
                upload->setValue(57);
            buttons->button(QDialogButtonBox::Ok)->click();
            poll.stop();
        });
        QTimer watchdog;
        watchdog.setSingleShot(true);
        connect(&watchdog, &QTimer::timeout, &window, [&]() {
            dialogError = QStringLiteral("Speed limit dialog timed out");
            if (auto *dlg = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
                dlg->reject();
        });
        poll.start();
        watchdog.start(8000);
        limits->trigger();
        watchdog.stop();
        poll.stop();
        QVERIFY2(dialogError.isEmpty(), qPrintable(dialogError));
        QCOMPARE(opened, 1);
        QTRY_COMPARE_WITH_TIMEOUT(server.trayLimits().value(QStringLiteral("up_limit")).toInt(),
                                  57 * 1024, 7000);
        QCOMPARE(server.trayLimits().value(QStringLiteral("dl_limit")).toInt(), 100 * 1024);
        QCOMPARE(server.trayLimits().value(QStringLiteral("alt_up_limit")).toInt(), 20 * 1024);
        QCOMPARE(server.commandsFor("/api/v2/app/setPreferences"), 1);
        const QUrlQuery changed(QString::fromUtf8(server.lastCommandBody(
            "/api/v2/app/setPreferences")));
        const QJsonObject changes = QJsonDocument::fromJson(changed.queryItemValue(
            QStringLiteral("json"), QUrl::FullyDecoded).toUtf8()).object();
        QCOMPARE(changes.size(), 1);
        QCOMPARE(changes.value(QStringLiteral("up_limit")).toInt(), 57 * 1024);
        // Open the real tray submenu while the main window is hidden.
        // Entries are sourced from currently connected NAS profiles.
        disconnectMenu->popup(QPoint(35, 35));
        QTRY_VERIFY(disconnectMenu->isVisible());
        QCOMPARE(disconnectMenu->actions().size(), 1);
        QAction *serverEntry = disconnectMenu->actions().first();
        QCOMPARE(serverEntry->text(), QStringLiteral("Demo seedbox"));
        QVERIFY(serverEntry->isEnabled());
        disconnectMenu->close();
        serverEntry->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(server.logouts(), 1, 7000);
        QVERIFY(!disconnectMenu->isEnabled());
        QVERIFY(!window.isVisible());
        // Disconnect is local only. Nothing pauses the NAS or torrents.
        QCOMPARE(server.commandsFor("/api/v2/transfer/pauseSession"), 0);
        QCOMPARE(server.commandsFor("/api/v2/transfer/resumeSession"), 0);
        QVERIFY(server.trayAlternative());
        show->trigger();
        QTRY_VERIFY(window.isVisible());

        // Exit is the existing safe "Quit Completely" action. Declining the
        // actual confirmation must keep the application and NAS running.
        settings.setValue(QStringLiteral("Preferences/Behavior/ConfirmExit"), true);
        QTimer::singleShot(0, &window, []() {
            if (auto *question = qobject_cast<QMessageBox *>(
                    QApplication::activeModalWidget()))
                question->done(QMessageBox::No);
        });
        exit->trigger();
        QVERIFY(window.isVisible());
        QCOMPARE(server.logouts(), 1);
        settings.setValue(QStringLiteral("Preferences/Behavior/ConfirmExit"), false);
    }

    void nativeTrayMenuTracksTheSelectedServerWithoutCrossServerCommands()
    {
        DemoQbittorrentServer alpha(true, nullptr, true);
        DemoQbittorrentServer beta(true, nullptr, true);
        alpha.enableNativeTrayDemo(false);
        beta.enableNativeTrayDemo(true);
        beta.setTrayConnectionStatus(QStringLiteral("firewalled"));
        QVERIFY(alpha.start());
        QVERIFY(beta.start());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("two-trays.ini")),
                           QSettings::IniFormat);
        Remote::ProfileStore profiles(settings);
        Remote::ServerProfile first{
            QStringLiteral("alpha-server"), QStringLiteral("Alpha"), alpha.url(),
            QStringLiteral("demo"), false, true};
        Remote::ServerProfile second{
            QStringLiteral("beta-server"), QStringLiteral("Beta"), beta.url(),
            QStringLiteral("demo"), false, true};
        QVERIFY(profiles.save(first));
        QVERIFY(profiles.save(second));
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("serverTabs"));
        auto *alternative = window.findChild<QAction *>(
            QStringLiteral("actionUseAlternativeSpeedLimits"));
        auto *disconnectMenu = window.findChild<QMenu *>(QStringLiteral("trayDisconnectMenu"));
        auto *connectionStatus = window.statusBar()->findChild<QLabel *>(
            QStringLiteral("remoteConnectionStatus"));
        auto *speedMode = window.statusBar()->findChild<QPushButton *>(
            QStringLiteral("remoteAlternativeSpeedLimits"));
        QVERIFY(tabs && alternative && disconnectMenu && connectionStatus && speedMode);
        QVERIFY(!speedMode->isEnabled());
        QVERIFY(connectionStatus->toolTip().contains(QStringLiteral("Not connected")));
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 2, 7000);
        tabs->setCurrentIndex(0);
        QTRY_VERIFY_WITH_TIMEOUT(alternative->isEnabled() && disconnectMenu->isEnabled(), 7000);
        QCOMPARE(alternative->isChecked(), false);
        QTRY_VERIFY_WITH_TIMEOUT(connectionStatus->toolTip().contains(QStringLiteral("Online")), 7000);
        QVERIFY(speedMode->isVisible() && speedMode->isEnabled() && !speedMode->isChecked());
        QVERIFY(!connectionStatus->pixmap().isNull() && !speedMode->icon().isNull());
        QVERIFY(speedMode->toolTip().contains(QStringLiteral("alternative speed limits")));
        tabs->setCurrentIndex(1);
        QTRY_VERIFY_WITH_TIMEOUT(alternative->isEnabled() && alternative->isChecked(), 7000);
        QTRY_VERIFY_WITH_TIMEOUT(connectionStatus->toolTip().contains(QStringLiteral("No direct connections")), 7000);
        QVERIFY(speedMode->isChecked());
        QVERIFY(speedMode->toolTip().contains(QStringLiteral("regular speed limits")));

        // The bottom-row speed button changes only the selected server.
        QTest::mouseClick(speedMode, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!beta.trayAlternative() && !speedMode->isChecked(), 7000);
        QVERIFY(!alternative->isChecked());
        QCOMPARE(beta.commandsFor("/api/v2/transfer/setSpeedLimitsMode"), 1);
        QCOMPARE(alpha.commandsFor("/api/v2/transfer/setSpeedLimitsMode"), 0);

        // Incoming connectivity is distinct from Web API connectivity.
        beta.setTrayConnectionStatus(QStringLiteral("disconnected"));
        QTRY_VERIFY_WITH_TIMEOUT(connectionStatus->toolTip().contains(QStringLiteral("Offline")), 7000);
        QVERIFY(speedMode->isEnabled());
        beta.setTrayConnectionStatus(QStringLiteral("unknown"));
        QTRY_VERIFY_WITH_TIMEOUT(connectionStatus->toolTip().contains(QStringLiteral("has not been reported")), 7000);

        // Disconnect Beta while Alpha is selected. It must not act on the
        // current tab by accident or modify the NAS daemon's torrent state.
        tabs->setCurrentIndex(0);
        QCOMPARE(alternative->isChecked(), false);
        QVERIFY(connectionStatus->toolTip().contains(QStringLiteral("Online")));
        QTest::mouseClick(speedMode, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(alpha.trayAlternative() && speedMode->isChecked(), 7000);
        QVERIFY(alternative->isChecked());
        QCOMPARE(alpha.commandsFor("/api/v2/transfer/setSpeedLimitsMode"), 1);
        QCOMPARE(beta.commandsFor("/api/v2/transfer/setSpeedLimitsMode"), 1);
        disconnectMenu->popup(QPoint(40, 40));
        QTRY_COMPARE(disconnectMenu->actions().size(), 2);
        QAction *betaEntry = nullptr;
        for (QAction *entry : disconnectMenu->actions())
            if (entry->text() == QStringLiteral("Beta"))
                betaEntry = entry;
        QVERIFY(betaEntry);
        QCOMPARE(betaEntry->data().toString(), QStringLiteral("beta-server"));
        disconnectMenu->close();
        betaEntry->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(beta.logouts(), 1, 7000);
        QCOMPARE(alpha.logouts(), 0);
        QTRY_COMPARE(tabs->count(), 2);
        QCOMPARE(tabs->tabText(0), QStringLiteral("Alpha"));
        QCOMPARE(tabs->tabText(1), QStringLiteral("Beta"));
        QVERIFY(!tabs->widget(1)->isEnabled());
        QVERIFY(tabs->tabToolTip(1).contains(QStringLiteral("Disconnected manually")));
        QVERIFY(disconnectMenu->isEnabled());
        QCOMPARE(alpha.commandsFor("/api/v2/transfer/pauseSession"), 0);
        QCOMPARE(beta.commandsFor("/api/v2/transfer/pauseSession"), 0);

        // After refresh, the list has only Alpha. It can be disconnected
        // without showing the main window or stopping any NAS transfers.
        window.hide();
        disconnectMenu->popup(QPoint(40, 40));
        QTRY_COMPARE(disconnectMenu->actions().size(), 1);
        QAction *alphaEntry = disconnectMenu->actions().first();
        QCOMPARE(alphaEntry->text(), QStringLiteral("Alpha"));
        disconnectMenu->close();
        alphaEntry->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(alpha.logouts(), 1, 7000);
        QTRY_VERIFY(!disconnectMenu->isEnabled());
        QVERIFY(!speedMode->isEnabled());
        QVERIFY(connectionStatus->toolTip().contains(QStringLiteral("Not connected")));
        QCOMPARE(tabs->count(), 2);
        QVERIFY(!tabs->widget(0)->isEnabled() && !tabs->widget(1)->isEnabled());
        QVERIFY(!window.isVisible());
    }

    void toolbarAndStatusbarRemainEnabledAfterHiddenWindowExit()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("tray-layout.ini")),
                           QSettings::IniFormat);
        DemoCredentialVault vault;

        {
            MainWindow window(nullptr, &settings, &vault);
            window.show();
            QTRY_VERIFY(window.isVisible());
            auto *toolbar = window.findChild<QToolBar *>(QStringLiteral("toolBar"));
            auto *statusbar = window.findChild<QStatusBar *>(QStringLiteral("statusBar"));
            auto *toolbarAction = window.findChild<QAction *>(
                QStringLiteral("actionTopToolBar"));
            auto *statusAction = window.findChild<QAction *>(
                QStringLiteral("actionShowStatusbar"));
            QVERIFY(toolbar && statusbar && toolbarAction && statusAction);
            QVERIFY(toolbar->isVisible());
            QVERIFY(statusbar->isVisible());
            QVERIFY(toolbarAction->isChecked());
            QVERIFY(statusAction->isChecked());

            // A tray-hidden top-level widget makes isVisible() false for
            // children, without changing the View menu toggles.
            window.hide();
            QVERIFY(!toolbar->isVisible());
            QVERIFY(!statusbar->isVisible());
            QVERIFY(window.close());
            settings.sync();
        }
        QCOMPARE(settings.value(QStringLiteral("Layout/ToolbarVisible")).toBool(), true);
        QCOMPARE(settings.value(QStringLiteral("Layout/StatusbarVisible")).toBool(), true);

        {
            MainWindow window(nullptr, &settings, &vault);
            window.show();
            QTRY_VERIFY(window.isVisible());
            auto *toolbar = window.findChild<QToolBar *>(QStringLiteral("toolBar"));
            auto *statusbar = window.findChild<QStatusBar *>(QStringLiteral("statusBar"));
            QVERIFY(toolbar && statusbar);
            QTRY_VERIFY(toolbar->isVisible());
            QVERIFY(statusbar->isVisible());
            QVERIFY(window.close());
        }
    }

    void corruptedOldToolbarSettingsRecoverOnce()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("legacy-layout.ini")),
                           QSettings::IniFormat);
        settings.setValue(QStringLiteral("Layout/ToolbarVisible"), false);
        settings.setValue(QStringLiteral("Layout/StatusbarVisible"), false);
        DemoCredentialVault vault;
        {
            MainWindow window(nullptr, &settings, &vault);
            window.show();
            QTRY_VERIFY(window.isVisible());
            auto *toolbar = window.findChild<QToolBar *>(QStringLiteral("toolBar"));
            auto *statusbar = window.findChild<QStatusBar *>(QStringLiteral("statusBar"));
            QVERIFY(toolbar && statusbar);
            QVERIFY(toolbar->isVisible());
            QVERIFY(statusbar->isVisible());
            QVERIFY(window.close());
        }
        QCOMPARE(settings.value(QStringLiteral("Layout/ViewVisibilityVersion")).toInt(), 2);
        // Subsequent deliberate changes are respected, not migrated again.
        settings.setValue(QStringLiteral("Layout/ToolbarVisible"), false);
        settings.setValue(QStringLiteral("Layout/StatusbarVisible"), false);
        {
            MainWindow window(nullptr, &settings, &vault);
            window.show();
            QTRY_VERIFY(window.isVisible());
            auto *toolbar = window.findChild<QToolBar *>(QStringLiteral("toolBar"));
            auto *statusbar = window.findChild<QStatusBar *>(QStringLiteral("statusBar"));
            QVERIFY(toolbar && statusbar);
            QVERIFY(!toolbar->isVisible());
            QVERIFY(!statusbar->isVisible());
            QVERIFY(window.close());
        }
    }

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
            // Tabs are created before asynchronous authentication completes.
            // A real header drag requires the pane to be enabled for input.
            QTRY_VERIFY_WITH_TIMEOUT(servers->widget(0)->isEnabled()
                && servers->widget(1)->isEnabled(), 8000);

            servers->setCurrentIndex(1);
            QVERIFY(qobject_cast<ServerPane *>(servers->currentWidget()));
            QCOMPARE(servers->tabText(servers->currentIndex()), QStringLiteral("Beta"));
            auto *pane = qobject_cast<ServerPane *>(servers->currentWidget());
            QVERIFY(pane);
            auto *torrentHeader = pane->table()->horizontalHeader();
            QVERIFY(torrentHeader);
            torrentHeader->resizeSection(1, 173);
            QCOMPARE(torrentHeader->sectionSize(1), 173);
            QVERIFY(torrentHeader->sectionsMovable());
            QCOMPARE(torrentHeader->visualIndex(1), 1);
            // Real mouse gesture on the native header: the Size column is
            // dragged ahead of Name, not merely reordered through an API.
            auto *headerViewport = torrentHeader->viewport();
            const QPoint from(torrentHeader->sectionViewportPosition(1) + 38,
                              torrentHeader->height() / 2);
            const QPoint to(torrentHeader->sectionViewportPosition(0) + 20,
                            torrentHeader->height() / 2);
            QTest::mousePress(headerViewport, Qt::LeftButton, Qt::NoModifier, from);
            // QTest::mouseMove() reports NoButton; QHeaderView requires a
            // MouseMove *with* LeftButton held to begin a genuine section drag.
            const auto moveWithLeftHeld = [headerViewport](const QPoint &point) {
                QMouseEvent move(QEvent::MouseMove, QPointF(point),
                                 Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(headerViewport, &move);
            };
            moveWithLeftHeld(from - QPoint(22, 0));
            moveWithLeftHeld(to);
            QTest::mouseRelease(headerViewport, Qt::LeftButton, Qt::NoModifier, to);
            QTRY_COMPARE(torrentHeader->visualIndex(1), 0);
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
            // Reorder only the second connection: a different server must
            // retain its own independent logical/visual column mapping.
            QCOMPARE(firstPane->table()->horizontalHeader()->visualIndex(1), 1);
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
            // Tabs are created before asynchronous authentication completes.
            // A real header drag requires the pane to be enabled for input.
            QTRY_VERIFY_WITH_TIMEOUT(servers->widget(0)->isEnabled()
                && servers->widget(1)->isEnabled(), 8000);
            QCOMPARE(servers->tabText(servers->currentIndex()), QStringLiteral("Beta"));

            auto *pane = qobject_cast<ServerPane *>(servers->currentWidget());
            QVERIFY(pane);
            QTRY_COMPARE(pane->table()->horizontalHeader()->sectionSize(1), 173);
            QVERIFY(pane->table()->horizontalHeader()->sectionsMovable());
            QCOMPARE(pane->table()->horizontalHeader()->visualIndex(1), 0);
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
            QVERIFY(firstPane->table()->horizontalHeader()->sectionsMovable());
            QCOMPARE(firstPane->table()->horizontalHeader()->visualIndex(1), 1);
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
            QVERIFY(increase->shortcuts().contains(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Plus)));
            QVERIFY(increase->shortcuts().contains(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Equal)));
            QCOMPARE(decrease->shortcut(), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Minus));
            QCOMPARE(reset->shortcut(), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_0));

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

    void historicalCategorySynchronization_data()
    {
        QTest::addColumn<QString>("version");
        QTest::addColumn<QString>("api");
        QTest::addColumn<bool>("listCategories");
        QTest::addColumn<bool>("modernLogin");
        QTest::newRow("4.1.0") << QString("4.1.0") << QString("2.0") << true << false;
        QTest::newRow("4.1.1") << QString("4.1.1") << QString("2.0.1") << true << false;
        QTest::newRow("4.1.2") << QString("4.1.2") << QString("2.0.2") << true << false;
        QTest::newRow("4.1.3") << QString("4.1.3") << QString("2.1.0") << false << false;
        QTest::newRow("5.1.4") << QString("5.1.4") << QString("2.11.4") << false << false;
        QTest::newRow("5.2.0") << QString("5.2.0") << QString("2.15.1") << false << true;
        QTest::newRow("5.3.1") << QString("5.3.1") << QString("2.16.2") << false << true;
    }

    void historicalCategorySynchronization()
    {
        QFETCH(QString, version);
        QFETCH(QString, api);
        QFETCH(bool, listCategories);
        QFETCH(bool, modernLogin);
        DemoQbittorrentServer server(true);
        server.setHistoricalConnectionProfile(version, api, modernLogin, listCategories);
        const QString hash(40, QLatin1Char('a'));
        const auto categories = [listCategories](const QStringList &names) -> QJsonValue {
            if (listCategories) return QJsonArray::fromStringList(names);
            QJsonObject result;
            for (const auto &name : names)
                result.insert(name, QJsonObject{{"name", name}, {"savePath", "/share/" + name}});
            return result;
        };
        // An empty category is not inferable from any torrent's category field.
        server.setCompletionSnapshot({
            {"categories", categories({"Music", "Empty category"})},
            {"torrents", QJsonObject{{hash, QJsonObject{{"name", "Test torrent"},
                {"state", "uploading"}, {"progress", 1}, {"category", "Music"}}}}},
            {"server_state", QJsonObject{{"queueing", false}, {"dl_info_speed", 0}}}
        });
        QVERIFY(server.start());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath("historical-sync.ini"), QSettings::IniFormat);
        settings.setValue("Preferences/Advanced/RefreshInterval", 100);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        openConnectDialog(window, server.url());
        auto *tabs = window.findChild<QTabWidget *>("serverTabs");
        QVERIFY(tabs);
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 6000);
        auto *pane = qobject_cast<ServerPane *>(tabs->widget(0));
        QVERIFY(pane);
        auto *sidebar = pane->findChild<QListWidget *>("categoryFilterList");
        QVERIFY(sidebar);
        const auto hasCategory = [sidebar](const QString &name) {
            const QString key = QStringLiteral("category:") + name;
            for (int i = 0; i < sidebar->count(); ++i)
                if (sidebar->item(i)->data(Qt::UserRole).toString() == key) return true;
            return false;
        };
        // Separate the connection/sync assertions from the visible row
        // assertions so a failure identifies the actual layer.
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(
            QStringLiteral("Connected to Demo seedbox")), 6000);
        QTRY_VERIFY_WITH_TIMEOUT(pane->hasFullSync(), 6000);
        QVERIFY(pane->categories().contains("Empty category"));
        QVERIFY(pane->categories().contains("Music"));
        QVERIFY(hasCategory("Empty category"));
        QVERIFY(hasCategory("Music"));
        QCOMPARE(pane->table()->model()->rowCount(), 1);
        QVERIFY(window.statusBar()->currentMessage().contains("v" + version));
        if (listCategories)
            QVERIFY(!pane->categoryDefinitions().value("Music").toObject().contains("savePath"));
        else
            QCOMPARE(pane->categoryDefinitions().value("Music").toObject().value("savePath").toString(),
                     QString("/share/Music"));

        // In both wire contracts the incremental collection is an addition,
        // not a replacement. Removals are transmitted independently.
        server.queueCompletionPatch({{"categories", categories({"New empty"})},
                                     {"categories_removed", QJsonArray{"Empty category"}}});
        QTRY_VERIFY_WITH_TIMEOUT(hasCategory("New empty") && !hasCategory("Empty category"), 6000);
        QVERIFY(hasCategory("Music"));
        QCOMPARE(pane->table()->model()->rowCount(), 1);
        if (!listCategories)
        {
            server.queueCompletionPatch({{"categories", QJsonObject{
                {"Music", QJsonObject{{"savePath", "/changed"}}}}}});
            QTRY_COMPARE_WITH_TIMEOUT(pane->categoryDefinitions().value("Music").toObject()
                .value("savePath").toString(), QString("/changed"), 6000);
            QCOMPARE(pane->categoryDefinitions().value("Music").toObject()
                .value("name").toString(), QString("Music"));
        }

        // A replacement snapshot after server-side sync state loss must
        // discard old category names/definitions and preserve no fake paths.
        server.queueCompletionPatch({{"full_update", true}, {"torrents", QJsonObject{}},
                                     {"categories", categories({"Replacement"})}});
        QTRY_VERIFY_WITH_TIMEOUT(hasCategory("Replacement") && !hasCategory("Music")
            && !hasCategory("New empty"), 6000);
        QCOMPARE(pane->table()->model()->rowCount(), 0);
        QVERIFY(!pane->categoryDefinitions().contains("Music"));
        QCOMPARE(server.invalidHistoricalSyncReplies(), 0);

        auto *disconnect = window.findChild<QAction *>("disconnectServer");
        QVERIFY(disconnect && disconnect->isEnabled());
        disconnect->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(server.logouts(), 1, 6000);
        QTRY_VERIFY_WITH_TIMEOUT(!pane->isEnabled(), 6000);
    }

    void historicalLoginFailures()
    {
        for (const bool modern : {false, true})
        {
            DemoQbittorrentServer server(false);
            server.setHistoricalConnectionProfile(modern ? "5.2.0" : "5.1.4",
                modern ? "2.15.1" : "2.11.4", modern, false);
            QVERIFY(server.start());
            QTemporaryDir temp;
            QVERIFY(temp.isValid());
            QSettings settings(temp.filePath("historical-login.ini"), QSettings::IniFormat);
            DemoCredentialVault vault;
            MainWindow window(nullptr, &settings, &vault);
            window.show();
            openConnectDialog(window, server.url());
            QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(
                modern ? "HTTP 401" : "Login failed"), 6000);
            auto *tabs = window.findChild<QTabWidget *>("serverTabs");
            QVERIFY(tabs && tabs->count() == 1 && !tabs->widget(0)->isEnabled());
            QCOMPARE(server.syncRequests(), 0);
            QCOMPARE(vault.writes, 0);
        }
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

        auto *count = window.statusBar()->findChild<QLabel *>(
            QStringLiteral("visibleTorrentCount"));
        QVERIFY(count);
        QCOMPARE(count->text(), QStringLiteral("Torrents: —"));
        openConnectDialog(window, server.url());
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage()
            .contains(QStringLiteral("Connected to Demo seedbox")), 6000);
        QVERIFY(window.windowTitle().contains(QStringLiteral("Demo seedbox")));
        QTRY_COMPARE(count->text(), QStringLiteral("Torrents: 0 / 0"));

        auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("serverTabs"));
        auto *connectAction = window.findChild<QAction *>(QStringLiteral("connectToServer"));
        auto *disconnect = window.findChild<QAction *>(QStringLiteral("disconnectServer"));
        auto *reconnect = window.findChild<QAction *>(QStringLiteral("reconnectServer"));
        auto *close = window.findChild<QAction *>(QStringLiteral("closeServerTab"));
        QVERIFY(tabs && connectAction && disconnect && reconnect && close);
        QVERIFY(disconnect->isEnabled() && close->isEnabled() && !reconnect->isEnabled());

        // Saving a rename without pressing Connect changes the live label
        // immediately, even while the management dialog is still open.
        bool renamed = false;
        QTimer::singleShot(0, &window, [&]() {
            auto *dialog = qobject_cast<ConnectionDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            const auto cleanup = qScopeGuard([dialog]() { dialog->reject(); });
            auto *name = dialog->findChild<QLineEdit *>(QStringLiteral("serverName"));
            auto *save = dialog->findChild<QPushButton *>(QStringLiteral("saveServer"));
            QVERIFY(name && save);
            name->setText(QStringLiteral("Renamed seedbox"));
            save->click();
            QCOMPARE(tabs->tabText(0), QStringLiteral("Renamed seedbox"));
            QVERIFY(window.windowTitle().contains(QStringLiteral("Renamed seedbox")));
            QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("Connected to Renamed seedbox")));
            renamed = true;
        });
        connectAction->trigger();
        QVERIFY(renamed);
        QCOMPARE(server.logouts(), 0);

        disconnect->trigger();
        QTRY_VERIFY(window.statusBar()->currentMessage()
            .contains(QStringLiteral("Disconnected manually")));
        QCOMPARE(tabs->count(), 1);
        QCOMPARE(tabs->tabText(0), QStringLiteral("Renamed seedbox"));
        QVERIFY(!tabs->widget(0)->isEnabled());
        QVERIFY(!disconnect->isEnabled() && reconnect->isEnabled() && close->isEnabled());
        QCOMPARE(count->text(), QStringLiteral("Torrents: —"));
        QTRY_COMPARE_WITH_TIMEOUT(server.logouts(), 1, 6000);
        auto *start = window.findChild<QAction *>(QStringLiteral("actionStart"));
        QVERIFY(start && !start->isEnabled());

        bool selectedProfile = false;
        QTimer::singleShot(0, &window, [&]() {
            auto *dialog = qobject_cast<ConnectionDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            const auto cleanup = qScopeGuard([dialog]() { if (dialog->isVisible()) dialog->reject(); });
            auto *profiles = dialog->findChild<QComboBox *>(QStringLiteral("savedServers"));
            auto *password = dialog->findChild<QLineEdit *>(QStringLiteral("serverPassword"));
            auto *button = dialog->findChild<QPushButton *>(QStringLiteral("connectServer"));
            QVERIFY(profiles && password && button);
            QCOMPARE(profiles->currentText(), QStringLiteral("Renamed seedbox"));
            password->setText(QStringLiteral("demo-password"));
            selectedProfile = true;
            button->click();
        });
        reconnect->trigger();
        QVERIFY(selectedProfile);
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage()
            .contains(QStringLiteral("Connected to Renamed seedbox")), 6000);
        QCOMPARE(tabs->count(), 1);
        QVERIFY(tabs->widget(0)->isEnabled());
        QVERIFY(disconnect->isEnabled() && !reconnect->isEnabled());
        QTRY_COMPARE(count->text(), QStringLiteral("Torrents: 0 / 0"));
        close->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(server.logouts(), 2, 6000);
        QCOMPARE(tabs->count(), 1); // only the empty Transfers placeholder
        QCOMPARE(tabs->tabText(0), QStringLiteral("Transfers"));
        QVERIFY(!close->isEnabled() && !reconnect->isEnabled());
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

    void completionNotificationsFollowLiveServerTransitionsAndLocalPreferences()
    {
        DemoQbittorrentServer first(true), second(true);
        const QString hash = QStringLiteral("0000000000000000000000000000000000000001");
        const QString old = QStringLiteral("0000000000000000000000000000000000000002");
        const QString added = QStringLiteral("0000000000000000000000000000000000000003");
        const QString fresh = QStringLiteral("0000000000000000000000000000000000000004");
        const auto record = [](const QString &name, bool complete, int stamp) {
            return QJsonObject{
                {QStringLiteral("name"), name},
                {QStringLiteral("state"), complete ? QStringLiteral("uploading") : QStringLiteral("downloading")},
                {QStringLiteral("progress"), complete ? 1.0 : 0.5},
                {QStringLiteral("amount_left"), complete ? 0 : 100},
                {QStringLiteral("completion_on"), stamp}
            };
        };
        const auto snapshot = [&](const QString &name) {
            return QJsonObject{{QStringLiteral("torrents"), QJsonObject{
                {hash, record(name, false, -1)},
                {old, record(QStringLiteral("Old completed torrent"), true, 100)}
            }}};
        };
        first.setCompletionSnapshot(snapshot(QStringLiteral("First download")));
        second.setCompletionSnapshot(snapshot(QStringLiteral("Second download")));
        first.setServerPreferences({{QStringLiteral("upnp"), false}});
        second.setServerPreferences({{QStringLiteral("upnp"), true}});
        QVERIFY(first.start() && second.start());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("completion.ini")), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        auto *notifier = window.findChild<CompletionNotifier *>();
        auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("serverTabs"));
        auto *prefs = window.findChild<QAction *>(QStringLiteral("actionOptions"));
        QVERIFY(notifier && tabs && prefs);
        // Leave the real sync, preference and delivery-request workflow
        // intact, but disconnect the notifier's own OS/audio delivery slots.
        // Offscreen Qt alone does not isolate the desktop DBus or audio session.
        // Never pop up notifications or play sounds during CTest, even when
        // makepkg runs it inside an active desktop session.
        QVERIFY(QObject::disconnect(notifier, nullptr, notifier, nullptr));
        QSignalSpy notices(notifier, &CompletionNotifier::notificationRequested);
        QSignalSpy sounds(notifier, &CompletionNotifier::soundRequested);
        QVERIFY(notices.isValid() && sounds.isValid());
        openConnectDialog(window, first.url());
        QTRY_VERIFY_WITH_TIMEOUT(first.syncRequests() > 0, 6000);
        openConnectDialog(window, second.url(), false, true, true, QStringLiteral("Second seedbox"));
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 2, 6000);
        auto *firstPane = qobject_cast<ServerPane *>(tabs->widget(0));
        auto *secondPane = qobject_cast<ServerPane *>(tabs->widget(1));
        QVERIFY(firstPane && secondPane);
        QTRY_VERIFY_WITH_TIMEOUT(firstPane->model()->hasFullSync()
            && secondPane->model()->hasFullSync(), 6000);
        QCOMPARE(notices.size(), 0);
        QCOMPARE(sounds.size(), 0);
        tabs->setCurrentIndex(1);

        const auto patch = [](DemoQbittorrentServer &server, const QString &id, const QJsonObject &fields) {
            server.queueCompletionPatch({{QStringLiteral("torrents"), QJsonObject{{id, fields}}}});
        };
        // A timestamp may arrive before the progress delta. Inactive tabs
        // must continue to dispatch, with the correct server name.
        patch(first, hash, {{QStringLiteral("completion_on"), 200}});
        QTRY_COMPARE_WITH_TIMEOUT(firstPane->model()->torrents().value(hash)
            .value(QStringLiteral("completion_on")).toInt(), 200, 6000);
        QCOMPARE(notices.size(), 0);
        patch(first, hash, {{QStringLiteral("progress"), 1.0},
            {QStringLiteral("amount_left"), 0}, {QStringLiteral("state"), QStringLiteral("uploading")}});
        QTRY_COMPARE_WITH_TIMEOUT(notices.size(), 1, 6000);
        QVERIFY(notices.at(0).at(0).toString().contains(QStringLiteral("Demo seedbox")));
        QCOMPARE(notices.at(0).at(1).toString(), QStringLiteral("First download"));
        QCOMPARE(sounds.size(), 0);
        QCOMPARE(tabs->currentIndex(), 1);

        const auto changePreferences = [&](bool beforeNotify, bool beforeSound,
                                           bool notify, bool sound, bool save) {
            bool finished = false;
            QTimer::singleShot(0, &window, [&]() {
                auto *dlg = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget());
                QVERIFY(dlg);
                const auto cleanup = qScopeGuard([dlg]() { if (dlg->isVisible()) dlg->reject(); });
                auto *sections = dlg->findChild<QListWidget *>(QStringLiteral("preferencesSections"));
                auto *notification = dlg->findChild<QCheckBox *>(QStringLiteral("checkNotifyTorrentCompletion"));
                auto *audio = dlg->findChild<QCheckBox *>(QStringLiteral("checkSoundTorrentCompletion"));
                auto *tray = dlg->findChild<QGroupBox *>(QStringLiteral("checkShowSystray"));
                auto *apply = dlg->findChild<QPushButton *>(QStringLiteral("preferencesApply"));
                QVERIFY(sections && notification && audio && tray && apply);
                sections->setCurrentRow(1);
                QVERIFY(notification->isEnabled() && audio->isEnabled() && !tray->isChecked());
                QCOMPARE(notification->isChecked(), beforeNotify);
                QCOMPARE(audio->isChecked(), beforeSound);
                notification->setChecked(notify);
                audio->setChecked(sound);
                QVERIFY(apply->isEnabled());
                if (save)
                {
                    apply->click();
                    QTRY_VERIFY_WITH_TIMEOUT(!apply->isEnabled(), 6000);
                }
                finished = true;
            });
            prefs->trigger();
            QVERIFY(finished);
        };
        changePreferences(true, false, false, true, false);
        changePreferences(true, false, false, true, true);
        QCOMPARE(settings.value(QStringLiteral("Preferences/Behavior/NotifyTorrentCompletion")).toBool(), false);
        QCOMPARE(settings.value(QStringLiteral("Preferences/Behavior/SoundTorrentCompletion")).toBool(), true);

        // Same infohash on a different server must complete independently.
        // This time progress arrives first; sound-only still dispatches once.
        patch(second, hash, {{QStringLiteral("progress"), 1.0},
            {QStringLiteral("amount_left"), 0}, {QStringLiteral("state"), QStringLiteral("uploading")}});
        QTRY_COMPARE_WITH_TIMEOUT(secondPane->model()->torrents().value(hash)
            .value(QStringLiteral("progress")).toDouble(), 1.0, 6000);
        QCOMPARE(sounds.size(), 0);
        patch(second, hash, {{QStringLiteral("completion_on"), 300}});
        QTRY_COMPARE_WITH_TIMEOUT(sounds.size(), 1, 6000);
        QCOMPARE(notices.size(), 1);

        changePreferences(false, true, true, true, true);
        // A newly discovered complete torrent is not an observed download.
        patch(second, added, record(QStringLiteral("Already present"), true, 400));
        QTRY_VERIFY_WITH_TIMEOUT(secondPane->model()->torrents().contains(added), 6000);
        QCOMPARE(notices.size(), 1);
        QCOMPARE(sounds.size(), 1);
        // Checking progress of a completed torrent must not arm an event.
        patch(second, old, {{QStringLiteral("state"), QStringLiteral("checkingUP")},
            {QStringLiteral("progress"), 0.25}});
        QTRY_COMPARE_WITH_TIMEOUT(secondPane->model()->torrents().value(old)
            .value(QStringLiteral("progress")).toDouble(), 0.25, 6000);
        patch(second, old, record(QStringLiteral("Old completed torrent"), true, 100));
        QTRY_COMPARE_WITH_TIMEOUT(secondPane->model()->torrents().value(old)
            .value(QStringLiteral("progress")).toDouble(), 1.0, 6000);
        QCOMPARE(notices.size(), 1);
        QCOMPARE(sounds.size(), 1);

        patch(second, fresh, record(QStringLiteral("New download"), false, -1));
        QTRY_VERIFY_WITH_TIMEOUT(secondPane->model()->torrents().contains(fresh), 6000);
        patch(second, fresh, record(QStringLiteral("New download"), true, 500));
        QTRY_COMPARE_WITH_TIMEOUT(notices.size(), 2, 6000);
        QTRY_COMPARE_WITH_TIMEOUT(sounds.size(), 2, 6000);
        QVERIFY(notices.at(1).at(0).toString().contains(QStringLiteral("Second seedbox")));
        QCOMPARE(notices.at(1).at(1).toString(), QStringLiteral("New download"));

        patch(second, fresh, {{QStringLiteral("completion_on"), 500}});
        const int beforeRepeat = second.syncRequests();
        QTRY_VERIFY_WITH_TIMEOUT(second.syncRequests() > beforeRepeat + 1, 7000);
        QCOMPARE(notices.size(), 2);
        QCOMPARE(sounds.size(), 2);

        // A full sync after a gap establishes a silent baseline.
        second.queueCompletionPatch({{QStringLiteral("full_update"), true},
            {QStringLiteral("torrents"), QJsonObject{
                {hash, record(QStringLiteral("Second download"), true, 300)},
                {fresh, record(QStringLiteral("New download"), true, 500)}
            }}});
        QTRY_VERIFY_WITH_TIMEOUT(!secondPane->model()->torrents().contains(old), 6000);
        QCOMPARE(notices.size(), 2);
        QCOMPARE(sounds.size(), 2);
        QCOMPARE(first.commandsFor("/api/v2/app/setPreferences"), 0);
        QCOMPARE(second.commandsFor("/api/v2/app/setPreferences"), 0);
    }

    void liveTorrentRowsFollowFullUpdatesPatchesAndRemovals()
    {
        DemoQbittorrentServer server(true);
        server.enableTorrentSyncDemo();
        server.enableMappedFolderDemo(); // Live remote save_path drives directory filtering.
        server.setReportedVersions(QStringLiteral("v7.5.30"), QStringLiteral("4.9.2"));
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
        auto *count = window.statusBar()->findChild<QLabel *>(
            QStringLiteral("visibleTorrentCount"));
        auto *down = window.statusBar()->findChild<QLabel *>(
            QStringLiteral("remoteDownloadRate"));
        auto *up = window.statusBar()->findChild<QLabel *>(
            QStringLiteral("remoteUploadRate"));
        QVERIFY(count && down && up);
        QCOMPARE(count->text(), QStringLiteral("Torrents: 1 / 1"));
        QVERIFY(count->geometry().right() < down->geometry().left());
        QVERIFY(down->geometry().right() < up->geometry().left());
        const QString identity = window.statusBar()->currentMessage();
        QVERIFY(identity.contains(QStringLiteral("Demo seedbox")));
        QVERIFY(identity.contains(server.reportedQbtVersion()));
        QVERIFY(identity.contains(server.reportedApiVersion()));

        auto *search = window.findChild<QLineEdit *>(QStringLiteral("torrentSearch"));
        QVERIFY2(search && search->isEnabled(), "Live torrent search should be enabled.");
        search->setText(QStringLiteral("does-not-exist"));
        QTRY_COMPARE(table->model()->rowCount(), 0);
        QTRY_COMPARE(count->text(), QStringLiteral("Torrents: 0 / 1"));
        search->clear();
        QTRY_COMPARE(table->model()->rowCount(), 1);
        QTRY_COMPARE(count->text(), QStringLiteral("Torrents: 1 / 1"));

        // Exercise the real Directories Qt tree on synchronized save_path:
        // selecting an ancestor with no direct torrents hides the row,
        // while selecting the leaf restores it. Total remains server-wide.
        auto *directories = window.findChild<QTreeWidget *>(
            QStringLiteral("directoryFilterTree"));
        QVERIFY(directories);
        QTreeWidgetItem *ancestor = nullptr;
        QTreeWidgetItem *leaf = nullptr;
        for (QTreeWidgetItemIterator it(directories); *it; ++it)
        {
            const QString path = (*it)->data(0, Qt::UserRole).toString();
            if (path == QLatin1String("/share/NAS"))
                ancestor = *it;
            else if (path == QLatin1String("/share/NAS/Music"))
                leaf = *it;
        }
        QVERIFY(ancestor && leaf);
        directories->setCurrentItem(ancestor);
        QTRY_COMPARE(table->model()->rowCount(), 0);
        QTRY_COMPARE(count->text(), QStringLiteral("Torrents: 0 / 1"));
        directories->setCurrentItem(leaf);
        QTRY_COMPARE(table->model()->rowCount(), 1);
        QTRY_COMPARE(count->text(), QStringLiteral("Torrents: 1 / 1"));
        directories->setCurrentItem(directories->topLevelItem(0));
        QTRY_COMPARE(table->model()->rowCount(), 1);

        QTRY_COMPARE_WITH_TIMEOUT(table->model()->index(0, 0).data().toString(),
                                  QStringLiteral("Updated Linux ISO"), 6000);
        QCOMPARE(count->text(), QStringLiteral("Torrents: 1 / 1"));
        QCOMPARE(table->model()->index(0, 2).data().toString().endsWith(QLatin1Char('%')), true);
        QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 0, 9000);
        QTRY_COMPARE(count->text(), QStringLiteral("Torrents: 0 / 0"));
        // Qt clears a timed status tip; the default server identity returns.
        window.statusBar()->showMessage(QStringLiteral("Temporary status"), 60);
        QCOMPARE(window.statusBar()->currentMessage(), QStringLiteral("Temporary status"));
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage()
            .contains(server.reportedQbtVersion()), 1500);
        QVERIFY(window.statusBar()->currentMessage().contains(server.reportedApiVersion()));
        QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("Demo seedbox")));
    }

    void nativeSidebarMenusManageSelectedServerAndVisibleTorrents()
    {
        DemoQbittorrentServer first(true), second(true);
        const QString hashA = QStringLiteral("0000000000000000000000000000000000000001");
        const QString hashB = QStringLiteral("0000000000000000000000000000000000000002");
        const QString trackerUrl = QStringLiteral("https://tracker.example/announce");
        const QJsonObject snapshot{
            {QStringLiteral("categories"), QJsonObject{
                {QStringLiteral("Music"), QJsonObject{{QStringLiteral("name"), QStringLiteral("Music")},
                    {QStringLiteral("savePath"), QStringLiteral("/share/Music")}}}}},
            {QStringLiteral("tags"), QJsonArray{QStringLiteral("keep"), QStringLiteral("unused")}},
            {QStringLiteral("trackers"), QJsonObject{{trackerUrl, QJsonArray{hashA, hashB}}}},
            {QStringLiteral("torrents"), QJsonObject{
                {hashA, QJsonObject{{QStringLiteral("name"), QStringLiteral("Music torrent")},
                    {QStringLiteral("state"), QStringLiteral("uploading")},
                    {QStringLiteral("progress"), 1}, {QStringLiteral("auto_tmm"), false},
                    {QStringLiteral("category"), QStringLiteral("Music")},
                    {QStringLiteral("save_path"), QStringLiteral("/share/Music")},
                    {QStringLiteral("tags"), QStringLiteral("keep")}, {QStringLiteral("tracker"), trackerUrl}}},
                {hashB, QJsonObject{{QStringLiteral("name"), QStringLiteral("Other torrent")},
                    {QStringLiteral("state"), QStringLiteral("uploading")},
                    {QStringLiteral("progress"), 1}, {QStringLiteral("auto_tmm"), false},
                    {QStringLiteral("category"), QStringLiteral("")},
                    {QStringLiteral("save_path"), QStringLiteral("/share/Music/Classical")},
                    {QStringLiteral("tags"), QStringLiteral("")}, {QStringLiteral("tracker"), trackerUrl}}}}}
        };
        first.setSidebarSnapshot(snapshot);
        second.setSidebarSnapshot(snapshot);
        QVERIFY(first.start() && second.start());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("sidebar.ini")), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        openConnectDialog(window, first.url());
        auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("serverTabs"));
        QVERIFY(tabs);
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(
            QStringLiteral("Connected to Demo seedbox")), 6000);
        openConnectDialog(window, second.url(), false, true, true, QStringLiteral("Second seedbox"));
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 2, 6000);
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(
            QStringLiteral("Connected to Second seedbox")), 6000);
        tabs->setCurrentIndex(0);
        auto *pane = qobject_cast<ServerPane *>(tabs->currentWidget());
        QVERIFY(pane);
        QTRY_COMPARE_WITH_TIMEOUT(pane->table()->model()->rowCount(), 2, 6000);
        auto *categories = pane->findChild<QListWidget *>(QStringLiteral("categoryFilterList"));
        auto *tags = pane->findChild<QListWidget *>(QStringLiteral("tagFilterList"));
        auto *status = pane->findChild<QListWidget *>(QStringLiteral("filterList"));
        auto *trackers = pane->findChild<QListWidget *>(QStringLiteral("trackerFilterList"));
        QVERIFY(categories && tags && status && trackers);
        const auto findRow = [](QListWidget *list, const QString &key) -> QListWidgetItem * {
            for (int i = 0; i < list->count(); ++i)
                if (list->item(i)->data(Qt::UserRole).toString() == key) return list->item(i);
            return nullptr;
        };
        using Drive = std::function<void(QDialog *)>;
        const auto invokeMenu = [&](QWidget *view, const QPoint &position, const QString &actionName,
                                    const Drive &drive) {
            bool invoked = false, opened = false, timedOut = false;
            QTimer watchdog;
            connect(&watchdog, &QTimer::timeout, &window, [&]() {
                timedOut = true;
                if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject();
                if (auto *menu = QApplication::activePopupWidget()) menu->close();
            });
            watchdog.start(6000);
            QTimer::singleShot(0, &window, [&]() {
                auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
                if (!menu) return;
                for (auto *action : menu->actions())
                {
                    if (action->objectName() != actionName) continue;
                    invoked = true;
                    if (drive)
                        QTimer::singleShot(0, &window, [&]() {
                            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                            if (!dialog) return;
                            opened = true;
                            const auto cleanup = qScopeGuard([dialog]() {
                                if (dialog->isVisible()) dialog->reject();
                            });
                            drive(dialog);
                        });
                    action->trigger();
                    break;
                }
                menu->close();
            });
            QVERIFY(QMetaObject::invokeMethod(view, "customContextMenuRequested", Qt::DirectConnection,
                Q_ARG(QPoint, position)));
            QVERIFY(invoked && !timedOut && (!drive || opened));
        };
        const auto menuAction = [&](QListWidget *list, const QString &key, const QString &actionName,
                                    const Drive &drive = {}) {
            auto *item = findRow(list, key);
            QVERIFY(item);
            invokeMenu(list, list->visualItemRect(item).center(), actionName, drive);
        };
        const auto setCategory = [&](QDialog *dialog, const QString &name, const QString &path, bool edit) {
            auto *categoryName = dialog->findChild<QLineEdit *>(QStringLiteral("textCategoryName"));
            auto *savePath = dialog->findChild<RemoteFileSystemPathComboEdit *>(QStringLiteral("comboSavePath"));
            auto *buttons = dialog->findChild<QDialogButtonBox *>(QStringLiteral("buttonBox"));
            QVERIFY(categoryName && savePath && buttons);
            auto *browse = savePath->findChild<QToolButton *>("browseFolderButton");
            QVERIFY(browse && browse->menu());
            QCOMPARE(browse->popupMode(), QToolButton::MenuButtonPopup);
            QVERIFY(browse->menu()->findChild<QAction *>("browseRemoteFilesystem")->isEnabled());
            if (edit)
            {
                QCOMPARE(categoryName->text(), name);
                QCOMPARE(savePath->currentText(), QStringLiteral("/share/Classical"));
                QVERIFY(!categoryName->isEnabled());
            }
            else
            {
                categoryName->setText(name);
                savePath->setEditText("/browse");
                QTimer drive;
                connect(&drive, &QTimer::timeout, &drive, [&] {
                    auto *picker = qobject_cast<RemoteFileDialog *>(QApplication::activeModalWidget());
                    if (!picker) return;
                    auto *box = picker->findChild<QDialogButtonBox *>();
                    if (box && box->button(QDialogButtonBox::Ok)->isEnabled())
                        box->button(QDialogButtonBox::Ok)->click();
                });
                QTimer timeout;
                timeout.setSingleShot(true);
                connect(&timeout, &QTimer::timeout, &timeout, [] {
                    if (auto *picker = qobject_cast<RemoteFileDialog *>(QApplication::activeModalWidget()))
                        picker->reject();
                    if (auto *popup = QApplication::activePopupWidget()) popup->close();
                });
                drive.start(10);
                timeout.start(4000);
                browse->click();
                QCOMPARE(savePath->property("explicitRemotePath").toString(), QStringLiteral("/browse"));
            }
            savePath->setEditText(path);
            buttons->button(QDialogButtonBox::Ok)->click();
        };
        menuAction(categories, QStringLiteral("all"), QStringLiteral("sidebarGroupAdd"), [&](QDialog *dialog) {
            setCategory(dialog, QStringLiteral("Classical"), QStringLiteral("/share/Classical"), false);
        });
        QTRY_VERIFY_WITH_TIMEOUT(findRow(categories, QStringLiteral("category:Classical")), 6000);
        menuAction(categories, QStringLiteral("category:Classical"), QStringLiteral("sidebarGroupEdit"), [&](QDialog *dialog) {
            setCategory(dialog, QStringLiteral("Classical"), QStringLiteral("/share/New Classical"), true);
        });
        QTRY_COMPARE_WITH_TIMEOUT(first.sidebarSnapshot().value(QStringLiteral("categories")).toObject()
            .value(QStringLiteral("Classical")).toObject().value(QStringLiteral("savePath")).toString(),
            QStringLiteral("/share/New Classical"), 6000);
        // Cancel does not replace the server's category settings.
        menuAction(categories, QStringLiteral("category:Classical"), QStringLiteral("sidebarGroupEdit"), [&](QDialog *dialog) {
            auto *path = dialog->findChild<RemoteFileSystemPathComboEdit *>(QStringLiteral("comboSavePath"));
            QVERIFY(path);
            path->setEditText(QStringLiteral("/never-saved"));
            dialog->reject();
        });
        QCOMPARE(first.sidebarSnapshot().value(QStringLiteral("categories")).toObject()
            .value(QStringLiteral("Classical")).toObject().value(QStringLiteral("savePath")).toString(),
            QStringLiteral("/share/New Classical"));
        menuAction(tags, QStringLiteral("all"), QStringLiteral("sidebarGroupAdd"), [&](QDialog *dialog) {
            auto *input = qobject_cast<QInputDialog *>(dialog);
            QVERIFY(input);
            input->setTextValue(QStringLiteral("new tag"));
            input->accept();
        });
        QTRY_VERIFY_WITH_TIMEOUT(findRow(tags, QStringLiteral("tag:new tag")), 6000);
        menuAction(tags, QStringLiteral("all"), QStringLiteral("sidebarGroupRemoveUnused"));
        QTRY_VERIFY_WITH_TIMEOUT(!findRow(tags, QStringLiteral("tag:unused"))
            && !findRow(tags, QStringLiteral("tag:new tag")), 6000);
        QVERIFY(findRow(tags, QStringLiteral("tag:keep")));
        // Status actions apply to all visible torrents, including other filters.
        categories->setCurrentItem(findRow(categories, QStringLiteral("category:Music")));
        QTRY_COMPARE(pane->table()->model()->rowCount(), 1);
        menuAction(status, QStringLiteral("all"), QStringLiteral("sidebarGroupStop"));
        QTRY_COMPARE_WITH_TIMEOUT(first.sidebarSnapshot().value(QStringLiteral("torrents")).toObject()
            .value(hashA).toObject().value(QStringLiteral("state")).toString(), QStringLiteral("stoppedUP"), 6000);
        QCOMPARE(first.sidebarSnapshot().value(QStringLiteral("torrents")).toObject()
            .value(hashB).toObject().value(QStringLiteral("state")).toString(), QStringLiteral("uploading"));
        // Removing a tracker host is global on this server, unlike visible-row actions.
        menuAction(trackers, QStringLiteral("tracker:tracker.example"), QStringLiteral("sidebarGroupRemove"),
            [](QDialog *dialog) {
                auto *confirm = qobject_cast<QMessageBox *>(dialog);
                QVERIFY(confirm);
                confirm->button(QMessageBox::Yes)->click();
            });
        QTRY_VERIFY_WITH_TIMEOUT(!first.sidebarSnapshot().value(QStringLiteral("trackers")).toObject().contains(trackerUrl), 6000);
        QTRY_VERIFY_WITH_TIMEOUT(!findRow(trackers, QStringLiteral("tracker:tracker.example")), 6000);

        // Directories shares the same real bulk actions. Right-click selects
        // the directory and uses the exact-path/subfolder filter and other
        // active filters, not the selected rows in the transfer list.
        categories->setCurrentItem(findRow(categories, QStringLiteral("all")));
        trackers->setCurrentItem(findRow(trackers, QStringLiteral("all")));
        auto *directories = pane->findChild<QTreeWidget *>(QStringLiteral("directoryFilterTree"));
        QVERIFY(directories);
        const auto directoryAction = [&](const QString &action, const Drive &drive = {}) {
            QTreeWidgetItem *music = nullptr;
            for (QTreeWidgetItemIterator it(directories); *it; ++it)
                if ((*it)->data(0, Qt::UserRole).toString() == QStringLiteral("/share/Music"))
                {
                    music = *it;
                    break;
                }
            QVERIFY(music);
            directories->scrollToItem(music);
            invokeMenu(directories, directories->visualItemRect(music).center(), action, drive);
        };
        const auto state = [&](const QString &hash) {
            return first.sidebarSnapshot().value(QStringLiteral("torrents")).toObject()
                .value(hash).toObject().value(QStringLiteral("state")).toString();
        };
        directoryAction(QStringLiteral("sidebarGroupStart"));
        QTRY_COMPARE_WITH_TIMEOUT(state(hashA), QStringLiteral("uploading"), 6000);
        QCOMPARE(pane->table()->model()->rowCount(), 1);
        directoryAction(QStringLiteral("sidebarGroupForceStart"));
        QTRY_COMPARE_WITH_TIMEOUT(state(hashA), QStringLiteral("forcedUP"), 6000);
        QCOMPARE(state(hashB), QStringLiteral("uploading"));

        directoryAction(QStringLiteral("directoryIncludeSubfolders"));
        QTRY_COMPARE(pane->table()->model()->rowCount(), 2);
        directoryAction(QStringLiteral("sidebarGroupStop"));
        QTRY_VERIFY_WITH_TIMEOUT(state(hashA) == QLatin1String("stoppedUP")
            && state(hashB) == QLatin1String("stoppedUP"), 6000);

        categories->setCurrentItem(findRow(categories, QStringLiteral("category:Music")));
        QTRY_COMPARE(pane->table()->model()->rowCount(), 1);
        directoryAction(QStringLiteral("sidebarGroupRemoveTorrents"), [](QDialog *dialog) {
            auto *confirm = qobject_cast<QMessageBox *>(dialog);
            QVERIFY(confirm);
            confirm->button(QMessageBox::Cancel)->click();
        });
        QCOMPARE(first.sidebarSnapshot().value(QStringLiteral("torrents")).toObject().size(), 2);
        directoryAction(QStringLiteral("sidebarGroupRemoveTorrents"), [](QDialog *dialog) {
            auto *confirm = qobject_cast<QMessageBox *>(dialog);
            QVERIFY(confirm && confirm->checkBox());
            QVERIFY(!confirm->checkBox()->isChecked());
            confirm->button(QMessageBox::Yes)->click();
        });
        QTRY_VERIFY_WITH_TIMEOUT(!first.sidebarSnapshot().value(QStringLiteral("torrents")).toObject().contains(hashA), 6000);
        QVERIFY(first.sidebarSnapshot().value(QStringLiteral("torrents")).toObject().contains(hashB));
        QCOMPARE(QUrlQuery(QString::fromUtf8(first.lastCommandBody("/api/v2/torrents/delete")))
            .queryItemValue(QStringLiteral("deleteFiles")), QStringLiteral("false"));
        QCOMPARE(second.sidebarSnapshot(), snapshot);
    }

    void remotePreferencesReadApplyVerifyAndCancelOnSelectedServer()
    {
        DemoQbittorrentServer first(true);
        DemoQbittorrentServer second(true);
        first.setServerPreferences({
            {QStringLiteral("file_log_enabled"), true},
            {QStringLiteral("file_log_path"), QStringLiteral("/share/First/log")},
            {QStringLiteral("file_log_backup_enabled"), true},
            {QStringLiteral("file_log_max_size"), 65},
            {QStringLiteral("file_log_delete_old"), true},
            {QStringLiteral("file_log_age"), 10},
            {QStringLiteral("file_log_age_type"), 0},
            {QStringLiteral("performance_warning"), true},
            {QStringLiteral("mail_notification_password"), QStringLiteral("server-only-secret")}
        });
        // An older/different server can lack fields or return values this
        // client cannot represent. None of those may be sent as defaults.
        second.setServerPreferences({
            {QStringLiteral("file_log_enabled"), true},
            {QStringLiteral("file_log_path"), QStringLiteral("/share/Second/log")},
            {QStringLiteral("file_log_backup_enabled"), true},
            {QStringLiteral("file_log_max_size"), QStringLiteral("unsupported")},
            {QStringLiteral("file_log_delete_old"), true},
            {QStringLiteral("file_log_age_type"), 99},
            {QStringLiteral("performance_warning"), false}
        });
        QVERIFY(first.start() && second.start());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("prefs.ini")), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        QTRY_VERIFY(window.isVisible());
        openConnectDialog(window, first.url());
        auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("serverTabs"));
        QVERIFY(tabs);
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 6000);
        openConnectDialog(window, second.url(), false, true, true, QStringLiteral("Second seedbox"));
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 2, 6000);
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(
            QStringLiteral("Connected to Second seedbox")), 6000);

        auto *options = window.findChild<QAction *>(QStringLiteral("actionOptions"));
        QVERIFY(options);
        // Exercise the real menu and modal dialog rather than calling the
        // preference transport or the field binding helpers directly.
        const auto withDialog = [&](const std::function<void(RemotePreferencesDialog *)> &drive,
                                    bool background = false) {
            bool opened = false;
            QString clickedPreferencesLabel;
            const auto driveDialog = [&]() {
                auto *dialog = qobject_cast<RemotePreferencesDialog *>(
                    QApplication::activeModalWidget());
                if (!dialog)
                    return;
                opened = true;
                const auto cleanup = qScopeGuard([dialog]() {
                    if (dialog->isVisible())
                        dialog->reject();
                });
                auto *navigation = dialog->findChild<QListWidget *>(QStringLiteral("preferencesSections"));
                QVERIFY(navigation);
                navigation->setCurrentRow(1);
                drive(dialog);
            };
            // Dismiss a dialog only on a test timeout, never pretend it passed.
            QTimer watchdog;
            bool timedOut = false;
            connect(&watchdog, &QTimer::timeout, &window, [&]() {
                timedOut = true;
                if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
                    dialog->reject();
            });
            watchdog.start(12000);
            if (background)
            {
                auto *bar = tabs->tabBar();
                QTimer::singleShot(0, &window, [&]() {
                    auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
                    if (!menu) return;
                    for (auto *action : menu->actions())
                        if (action->objectName() == QLatin1String("serverTabPreferences") && action->isEnabled())
                        {
                            clickedPreferencesLabel = action->text();
                            QTimer::singleShot(0, &window, driveDialog);
                            action->trigger();
                            break;
                        }
                    menu->close();
                });
                QVERIFY(QMetaObject::invokeMethod(bar, "customContextMenuRequested",
                    Qt::DirectConnection, Q_ARG(QPoint, bar->tabRect(0).center())));
            }
            else
            {
                QTimer::singleShot(0, &window, driveDialog);
                options->trigger();
            }
            watchdog.stop();
            QVERIFY(opened);
            QVERIFY(!timedOut);
            if (background)
                QCOMPARE(clickedPreferencesLabel, QStringLiteral("Demo seedbox Preferences..."));
        };
        const QString newPath = QStringLiteral("/share/Second/Logs + & 100% ü");
        const auto changedBody = [](const DemoQbittorrentServer &server) {
            const QUrlQuery form(QString::fromUtf8(server.lastCommandBody("/api/v2/app/setPreferences")));
            return QJsonDocument::fromJson(form.queryItemValue(
                QStringLiteral("json"), QUrl::FullyDecoded).toUtf8()).object();
        };
        withDialog([&](RemotePreferencesDialog *dialog) {
            auto *path = dialog->findChild<QLineEdit *>(QStringLiteral("textFileLogPath"));
            auto *size = dialog->findChild<QSpinBox *>(QStringLiteral("spinFileLogSize"));
            auto *age = dialog->findChild<QSpinBox *>(QStringLiteral("spinFileLogAge"));
            auto *ageType = dialog->findChild<QComboBox *>(QStringLiteral("comboFileLogAgeType"));
            auto *buttons = dialog->findChild<QDialogButtonBox *>(QStringLiteral("buttonBox"));
            QVERIFY(path && size && age && ageType && buttons);
            auto *apply = buttons->button(QDialogButtonBox::Apply);
            QVERIFY(!path->isEnabled()); // No fictitious editable values before GET.
            QTRY_VERIFY_WITH_TIMEOUT(path->isEnabled(), 6000);
            QCOMPARE(path->text(), QStringLiteral("/share/Second/log"));
            auto *target = dialog->findChild<QLabel *>(QStringLiteral("preferencesRemoteTarget"));
            QVERIFY(target && target->isVisible());
            QVERIFY(target->text().contains(QStringLiteral("Second seedbox")));
            QVERIFY(!size->isEnabled() && !age->isEnabled() && !ageType->isEnabled());
            QVERIFY(!apply->isEnabled());
            path->setText(QStringLiteral("unsaved"));
            QVERIFY(apply->isEnabled());
            path->setText(QStringLiteral("/share/Second/log"));
            QVERIFY(!apply->isEnabled());
            path->setText(newPath);
            const int reads = second.preferenceReads();
            apply->click();
            QVERIFY(!buttons->isEnabled()); // Wait for POST plus authoritative GET.
            QVERIFY(dialog->isVisible());
            QTRY_VERIFY_WITH_TIMEOUT(buttons->isEnabled(), 6000);
            QVERIFY(!apply->isEnabled());
            QCOMPARE(second.preferenceReads(), reads + 1);
            QCOMPARE(second.commandsFor("/api/v2/app/setPreferences"), 1);
            QCOMPARE(changedBody(second), QJsonObject({{QStringLiteral("file_log_path"), newPath}}));
            QCOMPARE(second.serverPreferences().value(QStringLiteral("file_log_path")).toString(), newPath);
            QCOMPARE(first.commandsFor("/api/v2/app/setPreferences"), 0);
            // Cancel discards only the new unapplied edit, not the applied one.
            path->setText(QStringLiteral("/share/Second/discard"));
            buttons->button(QDialogButtonBox::Cancel)->click();
        });
        QCOMPARE(second.commandsFor("/api/v2/app/setPreferences"), 1);
        QCOMPARE(second.serverPreferences().value(QStringLiteral("file_log_path")).toString(), newPath);

        // Leave the second server active and configure the first through its
        // background tab. The active transfer view must not be replaced.
        QCOMPARE(tabs->currentIndex(), 1);
        // Simulate an external WebUI edit before reopening Preferences.
        auto updated = first.serverPreferences();
        updated.insert(QStringLiteral("file_log_path"), QStringLiteral("/share/First/changed-externally"));
        first.setServerPreferences(updated);
        withDialog([&](RemotePreferencesDialog *dialog) {
            auto *path = dialog->findChild<QLineEdit *>(QStringLiteral("textFileLogPath"));
            auto *size = dialog->findChild<QSpinBox *>(QStringLiteral("spinFileLogSize"));
            auto *backup = dialog->findChild<QCheckBox *>(QStringLiteral("checkFileLogBackup"));
            auto *buttons = dialog->findChild<QDialogButtonBox *>(QStringLiteral("buttonBox"));
            QVERIFY(path && size && backup && buttons);
            QTRY_VERIFY_WITH_TIMEOUT(path->isEnabled(), 6000);
            QCOMPARE(path->text(), QStringLiteral("/share/First/changed-externally"));
            auto *target = dialog->findChild<QLabel *>(QStringLiteral("preferencesRemoteTarget"));
            QVERIFY(target && target->isVisible());
            QVERIFY(target->text().contains(QStringLiteral("Demo seedbox")));
            QCOMPARE(tabs->currentIndex(), 1);
            QCOMPARE(size->value(), 65);
            QVERIFY(size->isEnabled());
            backup->setChecked(false);
            QVERIFY(!size->isEnabled());
            backup->setChecked(true);
            QVERIFY(size->isEnabled());
            QVERIFY(!buttons->button(QDialogButtonBox::Apply)->isEnabled());
            size->setValue(128);
            buttons->button(QDialogButtonBox::Ok)->click();
            QVERIFY(dialog->isVisible());
            QVERIFY(!buttons->isEnabled());
            QTRY_VERIFY_WITH_TIMEOUT(!dialog->isVisible(), 6000);
        }, true);
        QCOMPARE(tabs->currentIndex(), 1);
        QCOMPARE(first.commandsFor("/api/v2/app/setPreferences"), 1);
        QCOMPARE(changedBody(first), QJsonObject({{QStringLiteral("file_log_max_size"), 128}}));
        QCOMPARE(first.serverPreferences().value(QStringLiteral("file_log_max_size")).toInt(), 128);
        QCOMPARE(second.commandsFor("/api/v2/app/setPreferences"), 1);
        settings.sync();
        QFile saved(settings.fileName());
        QVERIFY(saved.open(QIODevice::ReadOnly));
        const QByteArray stored = saved.readAll();
        QVERIFY(!stored.contains("server-only-secret"));
        QVERIFY(!stored.contains("file_log_path"));
        QVERIFY(!stored.contains("file_log_max_size"));
    }

    void rejectedAndIgnoredRemotePreferenceWritesKeepEditsStaged()
    {
        DemoQbittorrentServer server(true);
        server.setServerPreferences({
            {QStringLiteral("file_log_enabled"), true},
            {QStringLiteral("file_log_path"), QStringLiteral("/share/log")},
            {QStringLiteral("performance_warning"), false}
        });
        QVERIFY(server.start());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("prefs.ini")), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        openConnectDialog(window, server.url());
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(
            QStringLiteral("Connected to Demo seedbox")), 6000);
        auto *options = window.findChild<QAction *>(QStringLiteral("actionOptions"));
        QVERIFY(options);
        int warnings = 0;
        QTimer dismissWarning;
        connect(&dismissWarning, &QTimer::timeout, &window, [&]() {
            auto *warning = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!warning || warning->windowTitle() != QStringLiteral("Cannot confirm server preferences"))
                return;
            ++warnings;
            warning->accept();
        });
        dismissWarning.start(20);
        bool opened = false;
        QTimer::singleShot(0, &window, [&]() {
            auto *dialog = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget());
            if (!dialog) return;
            opened = true;
            const auto cleanup = qScopeGuard([dialog]() {
                if (dialog->isVisible())
                    dialog->reject();
            });
            auto *nav = dialog->findChild<QListWidget *>(QStringLiteral("preferencesSections"));
            auto *path = dialog->findChild<QLineEdit *>(QStringLiteral("textFileLogPath"));
            auto *buttons = dialog->findChild<QDialogButtonBox *>(QStringLiteral("buttonBox"));
            QVERIFY(nav && path && buttons);
            nav->setCurrentRow(1);
            QTRY_VERIFY_WITH_TIMEOUT(path->isEnabled(), 6000);
            QCOMPARE(path->text(), QStringLiteral("/share/log"));
            path->setText(QStringLiteral("/share/new"));
            server.rejectPreferenceWrites(true);
            const int reads = server.preferenceReads();
            buttons->button(QDialogButtonBox::Ok)->click();
            QTRY_COMPARE_WITH_TIMEOUT(warnings, 1, 6000);
            QVERIFY(dialog->isVisible() && buttons->isEnabled());
            QVERIFY(buttons->button(QDialogButtonBox::Apply)->isEnabled());
            QCOMPARE(path->text(), QStringLiteral("/share/new"));
            QCOMPARE(server.preferenceReads(), reads);
            QCOMPARE(server.serverPreferences().value(QStringLiteral("file_log_path")).toString(),
                     QStringLiteral("/share/log"));

            server.rejectPreferenceWrites(false);
            server.ignorePreferenceWrites(true);
            buttons->button(QDialogButtonBox::Ok)->click();
            QTRY_COMPARE_WITH_TIMEOUT(warnings, 2, 6000);
            QVERIFY(dialog->isVisible() && buttons->isEnabled());
            QVERIFY(buttons->button(QDialogButtonBox::Apply)->isEnabled());
            QCOMPARE(path->text(), QStringLiteral("/share/new"));
            QCOMPARE(server.preferenceReads(), reads + 1);
            QCOMPARE(server.serverPreferences().value(QStringLiteral("file_log_path")).toString(),
                     QStringLiteral("/share/log"));

            server.ignorePreferenceWrites(false);
            buttons->button(QDialogButtonBox::Ok)->click();
            QTRY_VERIFY_WITH_TIMEOUT(!dialog->isVisible(), 6000);
        });
        QTimer watchdog;
        bool timedOut = false;
        connect(&watchdog, &QTimer::timeout, &window, [&]() {
            timedOut = true;
            if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
                dialog->reject();
        });
        watchdog.start(15000);
        options->trigger();
        watchdog.stop();
        dismissWarning.stop();
        QVERIFY(opened && !timedOut);
        QCOMPARE(warnings, 2);
        QCOMPARE(server.commandsFor("/api/v2/app/setPreferences"), 3);
        QCOMPARE(server.serverPreferences().value(QStringLiteral("file_log_path")).toString(),
                 QStringLiteral("/share/new"));
    }

    void downloadsSavingManagementUsesSelectedServerAndMappedPaths()
    {
        DemoQbittorrentServer first(true), second(true);
        const QJsonObject initial{
            {QStringLiteral("preallocate_all"), false},
            {QStringLiteral("incomplete_files_ext"), true},
            {QStringLiteral("use_unwanted_folder"), false},
            {QStringLiteral("auto_tmm_enabled"), false},
            {QStringLiteral("torrent_changed_tmm_enabled"), true},
            {QStringLiteral("save_path_changed_tmm_enabled"), false},
            {QStringLiteral("category_changed_tmm_enabled"), true},
            {QStringLiteral("use_category_paths_in_manual_mode"), false},
            {QStringLiteral("save_path"), QStringLiteral("/share/First/downloads")},
            {QStringLiteral("temp_path_enabled"), false},
            {QStringLiteral("temp_path"), QStringLiteral("/share/First/incomplete")},
            {QStringLiteral("mail_notification_enabled"), true}
        };
        const QJsonObject other{
            {QStringLiteral("preallocate_all"), true},
            {QStringLiteral("auto_tmm_enabled"), true},
            {QStringLiteral("save_path"), QStringLiteral("/share/Second/downloads")},
            {QStringLiteral("temp_path_enabled"), false},
            {QStringLiteral("temp_path"), QStringLiteral("/share/Second/incomplete")}
        };
        first.setServerPreferences(initial);
        second.setServerPreferences(other);
        // Defaults deliberately disagree with existing torrent modes.
        first.setSavingManagementTorrent(true);
        second.setSavingManagementTorrent(false);
        QVERIFY(first.start() && second.start());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QVERIFY(QDir(temp.path()).mkpath(QStringLiteral("mounted/finished")));
        QVERIFY(QDir(temp.path()).mkpath(QStringLiteral("unmapped")));
        const QString mount = temp.filePath(QStringLiteral("mounted"));
        const QString mapped = temp.filePath(QStringLiteral("mounted/finished"));
        QSettings settings(temp.filePath(QStringLiteral("prefs.ini")), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        openConnectDialog(window, first.url());
        auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("serverTabs"));
        QVERIFY(tabs);
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(
            QStringLiteral("Connected to Demo seedbox")), 6000);
        openConnectDialog(window, second.url(), false, true, true, QStringLiteral("Second seedbox"));
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(
            QStringLiteral("Connected to Second seedbox")), 6000);
        // Wait for authenticated sync to populate both real transfer views.
        for (int i = 0; i < tabs->count(); ++i)
        {
            auto *table = tabs->widget(i)->findChild<QTableView *>(QStringLiteral("torrentList"));
            QVERIFY(table && table->model());
            QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 1, 6000);
        }
        const auto profiles = Remote::ProfileStore(settings).profiles();
        QCOMPARE(profiles.size(), 2);
        QString firstId, secondId;
        for (const auto &profile : profiles)
        {
            if (profile.name == QStringLiteral("Demo seedbox"))
                firstId = profile.id;
            else if (profile.name == QStringLiteral("Second seedbox"))
                secondId = profile.id;
        }
        QVERIFY(!firstId.isEmpty() && !secondId.isEmpty());
        Remote::PathMappingStore mappings(settings);
        // Browsing must use the first of equally specific local aliases.
        QVERIFY(mappings.replace(firstId, {
            {mount, QStringLiteral("/share/First")},
            {mount, QStringLiteral("/share/FirstAlias")}
        }));
        QVERIFY(mappings.replace(secondId, {{mount, QStringLiteral("/share/Second")}}));
        tabs->setCurrentIndex(0);
        QTRY_VERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("Connected to Demo seedbox")));
        auto *options = window.findChild<QAction *>(QStringLiteral("actionOptions"));
        QVERIFY(options);

        const bool originalNative = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        const auto restoreNative = qScopeGuard([originalNative]() {
            QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, originalNative);
        });

        int confirmationCount = 0, invalidCount = 0, unmappedCount = 0;
        bool approve = false;
        QString nextFolder;
        int pickers = 0;
        QTimer modalDriver;
        connect(&modalDriver, &QTimer::timeout, &window, [&]() {
            if (auto *warning = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
            {
                if (warning->windowTitle() == QStringLiteral("Change server download locations"))
                {
                    ++confirmationCount;
                    warning->button(approve ? QMessageBox::Yes : QMessageBox::No)->click();
                }
                else if (warning->windowTitle() == QStringLiteral("Invalid server directory"))
                {
                    ++invalidCount;
                    warning->accept();
                }
                else if (warning->windowTitle() == QStringLiteral("Unmapped local directory"))
                {
                    ++unmappedCount;
                    warning->accept();
                }
            }
            else if (auto *picker = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
                     picker && !nextFolder.isEmpty())
            {
                ++pickers;
                picker->setDirectory(nextFolder);
                nextFolder.clear();
                static_cast<QDialog *>(picker)->accept();
            }
        });
        modalDriver.start(20);
        bool opened = false;
        QTimer::singleShot(0, &window, [&]() {
            auto *dialog = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            opened = true;
            const auto cleanup = qScopeGuard([dialog]() {
                if (dialog->isVisible())
                    dialog->reject();
            });
            auto *nav = dialog->findChild<QListWidget *>(QStringLiteral("preferencesSections"));
            auto *selector = dialog->findChild<QComboBox *>(QStringLiteral("remotePathsServer"));
            auto *save = dialog->findChild<QLineEdit *>(QStringLiteral("textSavePath"));
            auto *incomplete = dialog->findChild<QLineEdit *>(QStringLiteral("textDownloadPath"));
            auto *useIncomplete = dialog->findChild<QCheckBox *>(QStringLiteral("checkUseDownloadPath"));
            auto *preallocate = dialog->findChild<QCheckBox *>(QStringLiteral("checkPreallocateAll"));
            auto *extension = dialog->findChild<QCheckBox *>(QStringLiteral("checkAppendqB"));
            auto *unwanted = dialog->findChild<QCheckBox *>(QStringLiteral("checkUnwantedFolder"));
            auto *mode = dialog->findChild<QComboBox *>(QStringLiteral("comboSavingMode"));
            auto *category = dialog->findChild<QComboBox *>(QStringLiteral("comboTorrentCategoryChanged"));
            auto *defaultPath = dialog->findChild<QComboBox *>(QStringLiteral("comboCategoryDefaultPathChanged"));
            auto *categoryPath = dialog->findChild<QComboBox *>(QStringLiteral("comboCategoryChanged"));
            auto *manualPaths = dialog->findChild<QCheckBox *>(QStringLiteral("checkUseCategoryPaths"));
            auto *buttons = dialog->findChild<QDialogButtonBox *>(QStringLiteral("buttonBox"));
            auto *browse = dialog->findChild<QToolButton *>(QStringLiteral("textSavePathBrowse"));
            QVERIFY(nav && selector && save && incomplete && useIncomplete && preallocate
                    && extension && unwanted && mode && category && defaultPath && categoryPath
                    && manualPaths && buttons && browse);
            // Deliberately select the other server in the *mapping* page.
            // Download settings must remain bound to the authenticated first server.
            nav->setCurrentRow(0);
            const int otherIndex = selector->findData(secondId);
            QVERIFY(otherIndex >= 0);
            selector->setCurrentIndex(otherIndex);
            nav->setCurrentRow(2);
            auto *apply = buttons->button(QDialogButtonBox::Apply);
            QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(), 6000);
            QCOMPARE(save->text(), QStringLiteral("/share/First/downloads"));
            QCOMPARE(incomplete->text(), QStringLiteral("/share/First/incomplete"));
            QVERIFY(!useIncomplete->isChecked() && !incomplete->isEnabled());
            QVERIFY(!preallocate->isChecked() && extension->isChecked() && !unwanted->isChecked());
            QCOMPARE(mode->currentText(), QStringLiteral("Manual"));
            QCOMPARE(category->currentText(), QStringLiteral("Relocate torrent"));
            QCOMPARE(defaultPath->currentText(), QStringLiteral("Switch affected torrents to Manual Mode"));
            QCOMPARE(categoryPath->currentText(), QStringLiteral("Relocate affected torrents"));
            QVERIFY(!manualPaths->isChecked() && !apply->isEnabled());

            // Browse the remote settings target, independently of the
            // mapping page's other-server selection.
            auto *remoteBrowse = browse->menu()->findChild<QAction *>("browseRemoteFilesystem");
            QVERIFY(remoteBrowse);
            save->setText("/browse");
            {
                QTimer pick;
                connect(&pick, &QTimer::timeout, &pick, [&] {
                    auto *picker = qobject_cast<RemoteFileDialog *>(QApplication::activeModalWidget());
                    if (!picker) return;
                    auto *box = picker->findChild<QDialogButtonBox *>();
                    auto *tree = picker->findChild<QTreeWidget *>("remoteBrowserEntries");
                    if (box && tree && tree->topLevelItemCount() && box->button(QDialogButtonBox::Ok)->isEnabled())
                    {
                        tree->setCurrentItem(tree->topLevelItem(0));
                        box->button(QDialogButtonBox::Ok)->click();
                    }
                });
                QTimer deadline;
                deadline.setSingleShot(true);
                connect(&deadline, &QTimer::timeout, &deadline, [] {
                    if (auto *picker = qobject_cast<RemoteFileDialog *>(QApplication::activeModalWidget())) picker->reject();
                    if (auto *popup = QApplication::activePopupWidget()) popup->close();
                });
                pick.start(10);
                deadline.start(4000);
                remoteBrowse->trigger();
                QCOMPARE(save->text(), QStringLiteral("/browse/Space & 100%"));
            }
            save->setText(QStringLiteral("relative/path"));
            apply->click();
            QCOMPARE(invalidCount, 1);
            QCOMPARE(first.serverPreferences(), initial);

            QVERIFY(browse->menu() && browse->popupMode() == QToolButton::MenuButtonPopup);
            auto *mounted = browse->menu()->findChild<QAction *>("browseMountedFilesystem");
            QVERIFY(mounted && mounted->isEnabled());
            nextFolder = temp.filePath(QStringLiteral("unmapped"));
            mounted->trigger();
            QCOMPARE(unmappedCount, 1);
            QCOMPARE(save->text(), QStringLiteral("relative/path"));
            nextFolder = mapped;
            browse->click();
            QCOMPARE(pickers, 2);
            QCOMPARE(save->text(), QStringLiteral("/share/First/finished"));

            useIncomplete->setChecked(true);
            QVERIFY(incomplete->isEnabled());
            incomplete->setText(QStringLiteral("/share/First/new + incomplete/"));
            preallocate->setChecked(true);
            extension->setChecked(false);
            unwanted->setChecked(true);
            mode->setCurrentIndex(mode->findText(QStringLiteral("Automatic")));
            category->setCurrentIndex(category->findText(QStringLiteral("Switch torrent to Manual Mode")));
            defaultPath->setCurrentIndex(defaultPath->findText(QStringLiteral("Relocate affected torrents")));
            categoryPath->setCurrentIndex(categoryPath->findText(QStringLiteral("Switch affected torrents to Manual Mode")));
            manualPaths->setChecked(true);
            apply->click(); // Decline the relocation warning.
            QCOMPARE(confirmationCount, 1);
            QCOMPARE(first.serverPreferences(), initial);
            QVERIFY(apply->isEnabled());

            approve = true;
            apply->click();
            QCOMPARE(confirmationCount, 2);
            QTRY_VERIFY_WITH_TIMEOUT(buttons->isEnabled(), 6000);
            QVERIFY(!apply->isEnabled());
            QCOMPARE(incomplete->text(), QStringLiteral("/share/First/new + incomplete"));
            QCOMPARE(save->text(), QStringLiteral("/share/First/finished"));
            QCOMPARE(second.serverPreferences(), other);
            const QJsonObject expected{
                {QStringLiteral("preallocate_all"), true},
                {QStringLiteral("incomplete_files_ext"), false},
                {QStringLiteral("use_unwanted_folder"), true},
                {QStringLiteral("auto_tmm_enabled"), true},
                {QStringLiteral("torrent_changed_tmm_enabled"), false},
                {QStringLiteral("save_path_changed_tmm_enabled"), true},
                {QStringLiteral("category_changed_tmm_enabled"), false},
                {QStringLiteral("use_category_paths_in_manual_mode"), true},
                {QStringLiteral("save_path"), QStringLiteral("/share/First/finished")},
                {QStringLiteral("temp_path_enabled"), true},
                {QStringLiteral("temp_path"), QStringLiteral("/share/First/new + incomplete")},
                {QStringLiteral("mail_notification_enabled"), true}
            };
            QCOMPARE(first.serverPreferences(), expected);
            // A trailing slash is the same path, even with Auto TMM torrents.
            save->setText(QStringLiteral("/share/First/finished/"));
            QVERIFY(!apply->isEnabled());
            // The manual category-path preference changes future suggestions,
            // not existing torrent locations, even on this Auto TMM server.
            manualPaths->setChecked(false);
            apply->click();
            QTRY_VERIFY_WITH_TIMEOUT(buttons->isEnabled(), 6000);
            QVERIFY(!apply->isEnabled());
            QCOMPARE(confirmationCount, 2);
            QVERIFY(!first.serverPreferences().value(QStringLiteral("use_category_paths_in_manual_mode")).toBool());
            // Cancel a new edit after a confirmed Apply.
            save->setText(QStringLiteral("/share/First/never-apply"));
            buttons->button(QDialogButtonBox::Cancel)->click();
        });
        QTimer watchdog;
        bool timedOut = false;
        connect(&watchdog, &QTimer::timeout, &window, [&]() {
            timedOut = true;
            if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
                dialog->reject();
        });
        watchdog.start(16000);
        options->trigger();
        watchdog.stop();
        QVERIFY(opened && !timedOut);
        QCOMPARE(first.commandsFor("/api/v2/app/setPreferences"), 2);
        QCOMPARE(first.serverPreferences().value(QStringLiteral("save_path")).toString(),
                 QStringLiteral("/share/First/finished"));

        tabs->setCurrentIndex(1);
        QTRY_VERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("Connected to Second seedbox")));
        opened = false;
        QTimer::singleShot(0, &window, [&]() {
            auto *dialog = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            opened = true;
            const auto cleanup = qScopeGuard([dialog]() {
                if (dialog->isVisible()) dialog->reject();
            });
            auto *nav = dialog->findChild<QListWidget *>(QStringLiteral("preferencesSections"));
            auto *save = dialog->findChild<QLineEdit *>(QStringLiteral("textSavePath"));
            auto *extension = dialog->findChild<QCheckBox *>(QStringLiteral("checkAppendqB"));
            auto *mode = dialog->findChild<QComboBox *>(QStringLiteral("comboSavingMode"));
            auto *policy = dialog->findChild<QComboBox *>(QStringLiteral("comboTorrentCategoryChanged"));
            auto *buttons = dialog->findChild<QDialogButtonBox *>(QStringLiteral("buttonBox"));
            QVERIFY(nav && save && extension && mode && policy && buttons);
            nav->setCurrentRow(2);
            QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(), 6000);
            QCOMPARE(save->text(), QStringLiteral("/share/Second/downloads"));
            QCOMPARE(mode->currentText(), QStringLiteral("Automatic"));
            QVERIFY(!extension->isEnabled() && !policy->isEnabled());
            // A manually typed remote path need not exist locally or match a mount.
            save->setText(QStringLiteral("/remote-only/Second + data"));
            buttons->button(QDialogButtonBox::Ok)->click();
            QTRY_VERIFY_WITH_TIMEOUT(!dialog->isVisible(), 6000);
        });
        watchdog.start(16000);
        options->trigger();
        watchdog.stop();
        modalDriver.stop();
        QVERIFY(opened && !timedOut);
        QCOMPARE(confirmationCount, 2); // Manual torrents: no relocation warning.
        QCOMPARE(second.commandsFor("/api/v2/app/setPreferences"), 1);
        QJsonObject expectedOther = other;
        expectedOther.insert(QStringLiteral("save_path"), QStringLiteral("/remote-only/Second + data"));
        QCOMPARE(second.serverPreferences(), expectedOther);
        QCOMPARE(first.commandsFor("/api/v2/app/setPreferences"), 2);
    }

    void localOriginalTorrentDeletionRequiresConfirmedAddition()
    {
        DemoQbittorrentServer server(true), other(true);
        server.enableModernResponses();
        QJsonObject snapshot{{QStringLiteral("torrents"), QJsonObject()}};
        server.setSidebarSnapshot(snapshot);
        other.setSidebarSnapshot(snapshot);
        QVERIFY(server.start() && other.start());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("prefs.ini")), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        openConnectDialog(window, server.url());
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(QStringLiteral("v5.3.0")), 6000);
        openConnectDialog(window, other.url(), false, true, true, QStringLiteral("Other seedbox"));
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(QStringLiteral("Connected to Other seedbox")), 6000);
        auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("serverTabs"));
        auto *prefs = window.findChild<QAction *>(QStringLiteral("actionOptions"));
        QVERIFY(tabs && prefs);
        tabs->setCurrentIndex(0);
        bool configured = false;
        QTimer::singleShot(0, &window, [&]() {
            auto *dlg = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget());
            QVERIFY(dlg);
            const auto cleanup = qScopeGuard([dlg]() { if (dlg->isVisible()) dlg->reject(); });
            auto *sections = dlg->findChild<QListWidget *>(QStringLiteral("preferencesSections"));
            auto *remove = dlg->findChild<QGroupBox *>(QStringLiteral("deleteTorrentBox"));
            auto *cancel = dlg->findChild<QCheckBox *>(QStringLiteral("deleteCancelledTorrentBox"));
            auto *apply = dlg->findChild<QPushButton *>(QStringLiteral("preferencesApply"));
            QVERIFY(sections && remove && cancel && apply);
            sections->setCurrentRow(2);
            QVERIFY(remove->isEnabled() && !remove->isChecked() && !cancel->isEnabled());
            remove->setChecked(true);
            QVERIFY(cancel->isEnabled());
            cancel->setChecked(true);
            apply->click();
            QVERIFY(!apply->isEnabled());
            configured = true;
        });
        // Provide a successful preferences endpoint without server deletion keys.
        server.setServerPreferences(QJsonObject{});
        other.setServerPreferences(QJsonObject{});
        prefs->trigger();
        QVERIFY(configured);
        QVERIFY(settings.value(QStringLiteral("Preferences/Downloads/DeleteOriginalTorrent")).toBool());
        QVERIFY(settings.value(QStringLiteral("Preferences/Downloads/DeleteCancelledTorrent")).toBool());
        QCOMPARE(server.commandsFor("/api/v2/app/setPreferences"), 0);
        auto *pane = qobject_cast<ServerPane *>(tabs->currentWidget());
        QVERIFY(pane);
        auto *table = pane->table();
        auto *model = pane->model();
        QVERIFY(table && model);
        QTRY_VERIFY_WITH_TIMEOUT(model->hasFullSync(), 6000);

        QJsonObject records;
        QHash<QString, QString> hashByPath;
        for (int i = 1; i <= 8; ++i)
        {
            const QByteArray name = QByteArrayLiteral("deletion-") + QByteArray::number(i);
            const QByteArray bytes = QByteArrayLiteral("d4:infod6:lengthi")
                + QByteArray::number(100 + i) + QByteArrayLiteral("e4:name")
                + QByteArray::number(name.size()) + ':' + name + QByteArrayLiteral("ee");
            const QString path = temp.filePath(QStringLiteral("original-%1.torrent").arg(i));
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(bytes), qint64(bytes.size()));
            file.close();
            Remote::TorrentMetadata parsed;
            QVERIFY(Remote::TorrentMetadataReader::read(bytes, parsed));
            QVERIFY(!parsed.infoHashV1.isEmpty());
            hashByPath.insert(path, parsed.infoHashV1);
        }
        auto path = [&](int i) { return temp.filePath(QStringLiteral("original-%1.torrent").arg(i)); };
        auto publish = [&](DemoQbittorrentServer &target, const QStringList &paths) {
            QJsonObject torrents;
            for (const auto &p : paths)
                torrents.insert(hashByPath.value(p), QJsonObject{
                    {QStringLiteral("name"), QFileInfo(p).fileName()},
                    {QStringLiteral("state"), QStringLiteral("stoppedDL")}});
            target.setSidebarSnapshot(QJsonObject{{QStringLiteral("torrents"), torrents}});
        };
        auto drop = [&](const QStringList &paths, bool cancel, bool keep, bool options = true) {
            QMimeData mime;
            QList<QUrl> urls;
            for (const auto &p : paths) urls.append(QUrl::fromLocalFile(p));
            mime.setUrls(urls);
            auto *viewport = table->viewport();
            QDragEnterEvent enter(QPoint(10, 10), Qt::CopyAction, &mime,
                Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(viewport, &enter);
            QVERIFY(enter.isAccepted());
            bool handled = false;
            if (options)
            {
                QTimer::singleShot(0, &window, [&]() {
                    auto *dlg = qobject_cast<RemoteAddTorrentDialog *>(QApplication::activeModalWidget());
                    QVERIFY(dlg);
                    const auto cleanup = qScopeGuard([dlg]() { if (dlg->isVisible()) dlg->reject(); });
                    auto *box = dlg->findChild<QCheckBox *>(QStringLiteral("doNotDeleteTorrentCheckBox"));
                    auto *buttons = dlg->findChild<QDialogButtonBox *>(QStringLiteral("buttonBox"));
                    QVERIFY(box && buttons && box->isEnabled() && box->isVisible());
                    box->setChecked(keep);
                    QTRY_VERIFY_WITH_TIMEOUT(buttons->button(QDialogButtonBox::Ok)->isEnabled(), 6000);
                    buttons->button(cancel ? QDialogButtonBox::Cancel : QDialogButtonBox::Ok)->click();
                    handled = true;
                });
            }
            QDropEvent event(QPointF(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(viewport, &event);
            QVERIFY(event.isAccepted());
            QVERIFY(!options || handled);
        };
        drop({path(1)}, true, true);
        QVERIFY(QFile::exists(path(1)));
        drop({path(2)}, true, false);
        QVERIFY(!QFile::exists(path(2)));
        QCOMPARE(server.commandsFor("/api/v2/torrents/add"), 0);

        drop({path(1)}, false, true);
        QTRY_COMPARE_WITH_TIMEOUT(server.commandsFor("/api/v2/torrents/add"), 1, 6000);
        publish(server, {path(1)});
        QTRY_VERIFY_WITH_TIMEOUT(model->torrents().contains(hashByPath.value(path(1))), 6000);
        QVERIFY(QFile::exists(path(1)));

        // Even HTTP 204 and another server's matching hash cannot delete a file.
        drop({path(3), path(4)}, false, false);
        QTRY_COMPARE_WITH_TIMEOUT(server.commandsFor("/api/v2/torrents/add"), 2, 6000);
        publish(other, {path(3), path(4)});
        publish(server, {path(1), path(3)});
        QTRY_VERIFY_WITH_TIMEOUT(model->torrents().contains(hashByPath.value(path(3))), 6000);
        QVERIFY(QFile::exists(path(3)) && QFile::exists(path(4)));
        publish(server, {path(1), path(3), path(4)});
        QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(path(3)) && !QFile::exists(path(4)), 6000);
        QCOMPARE(other.commandsFor("/api/v2/torrents/add"), 0);

        // No-dialog path follows the same confirmation rule, and a changed
        // local file must never be removed even after its original hash appears.
        settings.setValue(QStringLiteral("Preferences/Downloads/ShowAdditionDialog"), false);
        drop({path(5)}, false, false, false);
        QTRY_COMPARE_WITH_TIMEOUT(server.commandsFor("/api/v2/torrents/add"), 3, 6000);
        QFile replaced(path(5));
        QVERIFY(replaced.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(replaced.write("replacement"), qint64(11));
        replaced.close();
        publish(server, {path(1), path(3), path(4), path(5)});
        QTRY_VERIFY_WITH_TIMEOUT(model->torrents().contains(hashByPath.value(path(5))), 6000);
        QVERIFY(QFile::exists(path(5)));
        drop({path(6)}, false, false, false);
        QTRY_COMPARE_WITH_TIMEOUT(server.commandsFor("/api/v2/torrents/add"), 4, 6000);
        publish(server, {path(1), path(3), path(4), path(5), path(6)});
        QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(path(6)), 6000);

        // A rejected upload never authorizes deletion, even if another
        // client subsequently adds that same infohash on the target server.
        server.rejectTorrentCommands();
        bool rejected = false;
        QTimer warningDriver;
        connect(&warningDriver, &QTimer::timeout, &window, [&]() {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
            {
                rejected = box->windowTitle() == QStringLiteral("Remote torrent operation failed");
                box->reject();
            }
        });
        warningDriver.start(20);
        drop({path(7)}, false, false, false);
        QTRY_VERIFY_WITH_TIMEOUT(rejected, 6000);
        warningDriver.stop();
        publish(server, {path(1), path(3), path(4), path(5), path(6), path(7)});
        QTRY_VERIFY_WITH_TIMEOUT(model->torrents().contains(hashByPath.value(path(7))), 6000);
        QVERIFY(QFile::exists(path(7)));

        // The local preference is independent of the legacy server's API.
        tabs->setCurrentIndex(1);
        settings.setValue(QStringLiteral("Preferences/Downloads/DeleteOriginalTorrent"), false);
        // A disabled preference must retain the original after a successful
        // addition. Reuse the actual viewport of the selected legacy pane.
        auto *legacyPane = qobject_cast<ServerPane *>(tabs->currentWidget());
        QVERIFY(legacyPane);
        table = legacyPane->table();
        model = legacyPane->model();
        QTRY_VERIFY_WITH_TIMEOUT(model->hasFullSync(), 6000);
        drop({path(8)}, false, false, false);
        QTRY_COMPARE_WITH_TIMEOUT(other.commandsFor("/api/v2/torrents/add"), 1, 6000);
        publish(other, {path(3), path(4), path(8)});
        QTRY_VERIFY_WITH_TIMEOUT(model->torrents().contains(hashByPath.value(path(8))), 6000);
        QVERIFY(QFile::exists(path(8)));
        QCOMPARE(server.commandsFor("/api/v2/app/setPreferences"), 0);
        QCOMPARE(other.commandsFor("/api/v2/app/setPreferences"), 0);
    }

    void downloadsNotificationsAndProgramsFollowSelectedServer()
    {
        DemoQbittorrentServer modern(true), legacy(true);
        modern.enableModernResponses();
        QJsonObject initial{
            {QStringLiteral("mail_notification_enabled"), true},
            {QStringLiteral("mail_notification_sender"), QStringLiteral("sender@example.invalid")},
            {QStringLiteral("mail_notification_email"), QStringLiteral("recipient@example.invalid")},
            {QStringLiteral("mail_notification_smtp"), QStringLiteral("smtp.example.invalid")},
            {QStringLiteral("mail_notification_encryption_type"), QStringLiteral("SMTPS")},
            {QStringLiteral("mail_notification_auth_enabled"), true},
            {QStringLiteral("mail_notification_username"), QStringLiteral("demo")},
            {QStringLiteral("mail_notification_password"), QStringLiteral("fake-smtp-secret")},
            {QStringLiteral("autorun_on_torrent_added_enabled"), false},
            {QStringLiteral("autorun_on_torrent_added_program"), QStringLiteral("/server/on-added")},
            {QStringLiteral("autorun_enabled"), false},
            {QStringLiteral("autorun_program"), QStringLiteral("/server/on-finished")}
        };
        QJsonObject other = initial;
        other.remove(QStringLiteral("mail_notification_encryption_type"));
        other.insert(QStringLiteral("mail_notification_ssl_enabled"), false);
        modern.setServerPreferences(initial);
        legacy.setServerPreferences(other);
        QVERIFY(modern.start() && legacy.start());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("prefs.ini")), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        openConnectDialog(window, modern.url());
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(QStringLiteral("v5.3.0")), 6000);
        openConnectDialog(window, legacy.url(), false, true, true, QStringLiteral("Legacy seedbox"));
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(QStringLiteral("Connected to Legacy seedbox")), 6000);
        auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("serverTabs"));
        auto *prefs = window.findChild<QAction *>(QStringLiteral("actionOptions"));
        QVERIFY(tabs && prefs);

        const auto workflow = [&](bool modernServer, bool save) {
            tabs->setCurrentIndex(modernServer ? 0 : 1);
            bool finished = false;
            QTimer::singleShot(0, &window, [&]() {
                auto *dlg = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget());
                QVERIFY(dlg);
                const auto cleanup = qScopeGuard([dlg]() { if (dlg->isVisible()) dlg->reject(); });
                auto *sections = dlg->findChild<QListWidget *>(QStringLiteral("preferencesSections"));
                auto *mail = dlg->findChild<QGroupBox *>(QStringLiteral("groupMailNotification"));
                auto *auth = dlg->findChild<QGroupBox *>(QStringLiteral("groupMailNotifAuth"));
                auto *password = dlg->findChild<QLineEdit *>(QStringLiteral("mailNotifPassword"));
                auto *encryption = dlg->findChild<QComboBox *>(QStringLiteral("comboSMTPEncryption"));
                auto *ssl = dlg->findChild<QCheckBox *>(QStringLiteral("checkSmtpSSL"));
                auto *send = dlg->findChild<QPushButton *>(QStringLiteral("sendTestEmail"));
                auto *added = dlg->findChild<QGroupBox *>(QStringLiteral("groupBoxRunOnAdded"));
                auto *program = dlg->findChild<QLineEdit *>(QStringLiteral("lineEditRunOnAdded"));
                auto *apply = dlg->findChild<QPushButton *>(QStringLiteral("preferencesApply"));
                QVERIFY(sections && mail && auth && password && encryption && ssl && send && added && program && apply);
                sections->setCurrentRow(2);
                QTRY_VERIFY_WITH_TIMEOUT(send->isEnabled() && added->isEnabled(), 6000);
                QCOMPARE(password->echoMode(), QLineEdit::Password);
                QCOMPARE(password->text(), QStringLiteral("fake-smtp-secret"));
                auth->setChecked(false);
                QVERIFY(!password->isEnabled() && !send->isEnabled());
                auth->setChecked(true);
                if (modernServer)
                {
                    QVERIFY(encryption->isVisible() && !ssl->isVisible());
                    QCOMPARE(encryption->currentIndex(), 2);
                    encryption->setCurrentIndex(1);
                    password->setText(QStringLiteral("fake+new&%secret"));
                    QVERIFY(!program->isEnabled());
                    added->setChecked(true);
                    QVERIFY(program->isEnabled());
                    program->setText(QStringLiteral("/server/new-hook \"%N\""));
                }
                else
                {
                    QVERIFY(ssl->isVisible() && !encryption->isVisible());
                    QVERIFY(!ssl->isChecked());
                    ssl->click();
                }
                QVERIFY(apply->isEnabled() && !send->isEnabled());
                if (save)
                {
                    bool confirm = false;
                    bool accepted = false;
                    bool notified = false;
                    QTimer driver;
                    connect(&driver, &QTimer::timeout, dlg, [&]() {
                        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                        {
                            if (box->windowTitle() == QStringLiteral("Change server external programs"))
                            {
                                confirm = true;
                                box->button(accepted ? QMessageBox::Yes : QMessageBox::No)->click();
                            }
                            else if (box->windowTitle() == QStringLiteral("Test email")
                                     && box->icon() == QMessageBox::Information)
                            {
                                notified = true;
                                box->accept();
                            }
                            else
                            {
                                QTest::qFail("Unexpected modal during notification workflow", __FILE__, __LINE__);
                                box->reject();
                            }
                        }
                    });
                    driver.start(20);
                    if (modernServer)
                    {
                        apply->click();
                        QVERIFY(confirm && apply->isEnabled() && !send->isEnabled());
                        QCOMPARE(modern.commandsFor("/api/v2/app/setPreferences"), 0);
                        confirm = false;
                    }
                    accepted = true;
                    apply->click();
                    if (modernServer) QVERIFY(confirm);
                    QTRY_VERIFY_WITH_TIMEOUT(send->isEnabled() && !apply->isEnabled(), 6000);
                    send->click();
                    QTRY_VERIFY_WITH_TIMEOUT(notified && send->isEnabled(), 6000);
                }
                finished = true;
            });
            prefs->trigger();
            QVERIFY(finished);
        };

        workflow(true, false);
        QCOMPARE(modern.serverPreferences(), initial);
        QCOMPARE(modern.commandsFor("/api/v2/app/setPreferences"), 0);
        QCOMPARE(modern.commandsFor("/api/v2/app/sendTestEmail"), 0);
        workflow(true, true);
        QJsonObject expected = initial;
        expected.insert(QStringLiteral("mail_notification_encryption_type"), QStringLiteral("STARTTLS"));
        expected.insert(QStringLiteral("mail_notification_password"), QStringLiteral("fake+new&%secret"));
        expected.insert(QStringLiteral("autorun_on_torrent_added_enabled"), true);
        expected.insert(QStringLiteral("autorun_on_torrent_added_program"), QStringLiteral("/server/new-hook \"%N\""));
        QCOMPARE(modern.serverPreferences(), expected);
        QCOMPARE(modern.commandsFor("/api/v2/app/setPreferences"), 1);
        QCOMPARE(modern.commandsFor("/api/v2/app/sendTestEmail"), 1);
        QCOMPARE(legacy.serverPreferences(), other);
        QCOMPARE(legacy.commandsFor("/api/v2/app/sendTestEmail"), 0);
        workflow(false, true);
        other.insert(QStringLiteral("mail_notification_ssl_enabled"), true);
        QCOMPARE(legacy.serverPreferences(), other);
        QCOMPARE(legacy.commandsFor("/api/v2/app/setPreferences"), 1);
        QCOMPARE(legacy.commandsFor("/api/v2/app/sendTestEmail"), 1);
        QCOMPARE(modern.serverPreferences(), expected);
        QCOMPARE(modern.commandsFor("/api/v2/app/sendTestEmail"), 1);
        settings.sync();
        QFile persisted(settings.fileName());
        QVERIFY(persisted.open(QIODevice::ReadOnly));
        const auto contents = persisted.readAll();
        QVERIFY(!contents.contains("fake-smtp-secret") && !contents.contains("fake+new&%secret")
            && !contents.contains("fake%2Bnew") && !contents.contains("mail_notification_password"));
    }

    void downloadsExclusionsFollowSelectedServerCapabilities()
    {
        DemoQbittorrentServer modern(true), legacy(true);
        modern.enableModernResponses();
        // Synthetic future-capability server: 5.3.0 itself does not expose
        // this key. Capability-driven controls must not rely on the label.
        modern.setReportedVersions(QStringLiteral("v5.3.0"), QStringLiteral("2.17.0"));
        const QJsonObject initialModern{
            {QStringLiteral("excluded_file_names_enabled"), false},
            {QStringLiteral("excluded_file_names"), QStringLiteral("*.exe\nreadme.txt")},
            {QStringLiteral("exclude_files_without_extension"), false}
        };
        const QJsonObject initialLegacy{
            {QStringLiteral("excluded_file_names_enabled"), true},
            {QStringLiteral("excluded_file_names"), QStringLiteral("*.nfo")}
        };
        modern.setServerPreferences(initialModern);
        legacy.setServerPreferences(initialLegacy);
        QVERIFY(modern.start() && legacy.start());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("prefs.ini")), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        openConnectDialog(window, modern.url());
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(
            QStringLiteral("v5.3.0")), 6000);
        openConnectDialog(window, legacy.url(), false, true, true, QStringLiteral("Legacy seedbox"));
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(
            QStringLiteral("Connected to Legacy seedbox")), 6000);
        auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("serverTabs"));
        auto *prefs = window.findChild<QAction *>(QStringLiteral("actionOptions"));
        QVERIFY(tabs && prefs);
        tabs->setCurrentIndex(0);

        const QString patterns = QStringLiteral("*.exe\n?.txt\nreadme[0-9].txt");
        const auto editModern = [&](bool save) {
            bool finished = false;
            QTimer::singleShot(0, &window, [&]() {
                auto *dlg = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget());
                QVERIFY(dlg);
                const auto cleanup = qScopeGuard([dlg]() { if (dlg->isVisible()) dlg->reject(); });
                auto *sections = dlg->findChild<QListWidget *>(QStringLiteral("preferencesSections"));
                auto *group = dlg->findChild<QGroupBox *>(QStringLiteral("groupExcludedFileNames"));
                auto *list = dlg->findChild<QPlainTextEdit *>(QStringLiteral("textExcludedFileNames"));
                auto *extension = dlg->findChild<QCheckBox *>(QStringLiteral("checkExcludeFilesWithoutExtension"));
                auto *apply = dlg->findChild<QPushButton *>(QStringLiteral("preferencesApply"));
                QVERIFY(sections && group && list && extension && apply);
                sections->setCurrentRow(2);
                QTRY_VERIFY_WITH_TIMEOUT(group->isEnabled(), 6000);
                QVERIFY(!group->isChecked());
                QCOMPARE(list->toPlainText(), QStringLiteral("*.exe\nreadme.txt"));
                QVERIFY(!list->isEnabled() && !extension->isEnabled());
                group->setChecked(true);
                QVERIFY(list->isEnabled() && extension->isEnabled());
                list->setPlainText(patterns);
                extension->click();
                QVERIFY(apply->isEnabled());
                if (save)
                {
                    apply->click();
                    QTRY_VERIFY_WITH_TIMEOUT(group->isEnabled() && !apply->isEnabled(), 6000);
                    QCOMPARE(list->toPlainText(), patterns);
                    QVERIFY(extension->isChecked());
                    // Disabling exclusions retains the user's rules for later reuse.
                    group->setChecked(false);
                    QVERIFY(!list->isEnabled() && !extension->isEnabled());
                    apply->click();
                    QTRY_VERIFY_WITH_TIMEOUT(group->isEnabled() && !apply->isEnabled(), 6000);
                    QCOMPARE(list->toPlainText(), patterns);
                    QVERIFY(extension->isChecked());
                }
                finished = true;
            });
            prefs->trigger();
            QVERIFY(finished);
        };
        editModern(false);
        QCOMPARE(modern.serverPreferences(), initialModern);
        QCOMPARE(modern.commandsFor("/api/v2/app/setPreferences"), 0);
        editModern(true);
        const QJsonObject expectedModern{
            {QStringLiteral("excluded_file_names_enabled"), false},
            {QStringLiteral("excluded_file_names"), patterns},
            {QStringLiteral("exclude_files_without_extension"), true}
        };
        QCOMPARE(modern.serverPreferences(), expectedModern);
        QCOMPARE(modern.commandsFor("/api/v2/app/setPreferences"), 2);
        QCOMPARE(legacy.serverPreferences(), initialLegacy);

        tabs->setCurrentIndex(1);
        bool finished = false;
        QTimer::singleShot(0, &window, [&]() {
            auto *dlg = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget());
            QVERIFY(dlg);
            const auto cleanup = qScopeGuard([dlg]() { if (dlg->isVisible()) dlg->reject(); });
            auto *sections = dlg->findChild<QListWidget *>(QStringLiteral("preferencesSections"));
            auto *group = dlg->findChild<QGroupBox *>(QStringLiteral("groupExcludedFileNames"));
            auto *list = dlg->findChild<QPlainTextEdit *>(QStringLiteral("textExcludedFileNames"));
            auto *extension = dlg->findChild<QCheckBox *>(QStringLiteral("checkExcludeFilesWithoutExtension"));
            auto *apply = dlg->findChild<QPushButton *>(QStringLiteral("preferencesApply"));
            QVERIFY(sections && group && list && extension && apply);
            sections->setCurrentRow(2);
            QTRY_VERIFY_WITH_TIMEOUT(list->isEnabled(), 6000);
            QVERIFY(group->isChecked());
            QCOMPARE(list->toPlainText(), QStringLiteral("*.nfo"));
            QVERIFY(!extension->isEnabled());
            group->setChecked(false);
            group->setChecked(true);
            QVERIFY(list->isEnabled() && !extension->isEnabled());
            // An empty list is a valid server preference, not an absent value.
            list->clear();
            QVERIFY(apply->isEnabled());
            apply->click();
            QTRY_VERIFY_WITH_TIMEOUT(list->isEnabled() && !apply->isEnabled(), 6000);
            QVERIFY(!extension->isEnabled());
            finished = true;
        });
        prefs->trigger();
        QVERIFY(finished);
        QJsonObject expectedLegacy = initialLegacy;
        expectedLegacy.insert(QStringLiteral("excluded_file_names"), QString());
        QCOMPARE(legacy.serverPreferences(), expectedLegacy);
        QCOMPARE(legacy.commandsFor("/api/v2/app/setPreferences"), 1);
        const QUrlQuery form(QString::fromUtf8(legacy.lastCommandBody("/api/v2/app/setPreferences")));
        const QJsonObject delta = QJsonDocument::fromJson(
            form.queryItemValue(QStringLiteral("json"), QUrl::FullyDecoded).toUtf8()).object();
        const QJsonObject expectedDelta{{QStringLiteral("excluded_file_names"), QString()}};
        QCOMPARE(delta, expectedDelta);
        QCOMPARE(modern.serverPreferences(), expectedModern);
        QCOMPARE(modern.commandsFor("/api/v2/app/setPreferences"), 2);
    }

    void connectionProtocolAndListeningPortFollowSelectedServer()
    {
        DemoQbittorrentServer legacy(true), modern(true);
        modern.enableModernResponses();
        const QJsonObject initialLegacy{
            {QStringLiteral("bittorrent_protocol"), 0},
            {QStringLiteral("listen_port"), 0},
            {QStringLiteral("random_port"), true},
            {QStringLiteral("upnp"), false},
            {QStringLiteral("web_ui_port"), 6363}
        };
        const QJsonObject initialModern{
            {QStringLiteral("bittorrent_protocol"), 1},
            {QStringLiteral("listen_port"), 51413},
            {QStringLiteral("random_port"), false},
            {QStringLiteral("upnp"), true},
            {QStringLiteral("web_ui_port"), 6364}
        };
        legacy.setServerPreferences(initialLegacy);
        modern.setServerPreferences(initialModern);
        QVERIFY(legacy.start() && modern.start());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("prefs.ini")), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        openConnectDialog(window, legacy.url());
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(QStringLiteral("v5.1.4")), 6000);
        openConnectDialog(window, modern.url(), false, true, true, QStringLiteral("Modern seedbox"));
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(QStringLiteral("v5.3.0")), 6000);
        auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("serverTabs"));
        auto *prefs = window.findChild<QAction *>(QStringLiteral("actionOptions"));
        QVERIFY(tabs && prefs);
        tabs->setCurrentIndex(0);

        // Exercise the actual native controls, including Random and Cancel.
        for (const bool save : {false, true})
        {
            bool finished = false;
            QTimer::singleShot(0, &window, [&]() {
                auto *dlg = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget());
                QVERIFY(dlg);
                const auto cleanup = qScopeGuard([dlg]() { if (dlg->isVisible()) dlg->reject(); });
                auto *sections = dlg->findChild<QListWidget *>(QStringLiteral("preferencesSections"));
                auto *protocol = dlg->findChild<QComboBox *>(QStringLiteral("comboProtocol"));
                auto *port = dlg->findChild<QSpinBox *>(QStringLiteral("spinPort"));
                auto *random = dlg->findChild<QPushButton *>(QStringLiteral("randomButton"));
                auto *upnp = dlg->findChild<QCheckBox *>(QStringLiteral("checkUPnP"));
                auto *apply = dlg->findChild<QPushButton *>(QStringLiteral("preferencesApply"));
                QVERIFY(sections && protocol && port && random && upnp && apply);
                sections->setCurrentRow(3);
                QTRY_VERIFY_WITH_TIMEOUT(protocol->isEnabled() && port->isEnabled()
                    && random->isEnabled() && upnp->isEnabled(), 6000);
                QCOMPARE(protocol->count(), 3);
                QCOMPARE(protocol->currentIndex(), 0);
                QCOMPARE(port->value(), 0);
                QCOMPARE(port->text(), QStringLiteral("Any"));
                QVERIFY(!upnp->isChecked() && !apply->isEnabled());
                random->click();
                QVERIFY(port->value() >= 1024 && port->value() <= 65535);
                QVERIFY(apply->isEnabled());
                QCOMPARE(legacy.commandsFor("/api/v2/app/setPreferences"), 0);
                protocol->setCurrentIndex(2);
                port->setValue(49152);
                upnp->click();
                if (save)
                {
                    apply->click();
                    QTRY_VERIFY_WITH_TIMEOUT(port->isEnabled() && !apply->isEnabled(), 6000);
                    QCOMPARE(port->value(), 49152);
                    QCOMPARE(protocol->currentIndex(), 2);
                    QVERIFY(upnp->isChecked());
                }
                finished = true;
            });
            prefs->trigger();
            QVERIFY(finished);
            if (!save)
                QCOMPARE(legacy.serverPreferences(), initialLegacy);
        }
        QJsonObject expectedLegacy = initialLegacy;
        expectedLegacy.insert(QStringLiteral("bittorrent_protocol"), 2);
        expectedLegacy.insert(QStringLiteral("listen_port"), 49152);
        expectedLegacy.insert(QStringLiteral("random_port"), false);
        expectedLegacy.insert(QStringLiteral("upnp"), true);
        QCOMPARE(legacy.serverPreferences(), expectedLegacy);
        QCOMPARE(modern.serverPreferences(), initialModern);
        QCOMPARE(legacy.commandsFor("/api/v2/app/setPreferences"), 1);
        const auto delta = [](DemoQbittorrentServer &server) {
            const QUrlQuery form(QString::fromUtf8(server.lastCommandBody("/api/v2/app/setPreferences")));
            return QJsonDocument::fromJson(form.queryItemValue(
                QStringLiteral("json"), QUrl::FullyDecoded).toUtf8()).object();
        };
        const QJsonObject expectedLegacyDelta{
            {QStringLiteral("bittorrent_protocol"), 2},
            {QStringLiteral("listen_port"), 49152},
            {QStringLiteral("upnp"), true}
        };
        QCOMPARE(delta(legacy), expectedLegacyDelta);

        // HTTP 204 mutations on the modern server use the same binding.
        tabs->setCurrentIndex(1);
        bool finished = false;
        QTimer::singleShot(0, &window, [&]() {
            auto *dlg = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget());
            QVERIFY(dlg);
            const auto cleanup = qScopeGuard([dlg]() { if (dlg->isVisible()) dlg->reject(); });
            auto *sections = dlg->findChild<QListWidget *>(QStringLiteral("preferencesSections"));
            auto *protocol = dlg->findChild<QComboBox *>(QStringLiteral("comboProtocol"));
            auto *port = dlg->findChild<QSpinBox *>(QStringLiteral("spinPort"));
            auto *upnp = dlg->findChild<QCheckBox *>(QStringLiteral("checkUPnP"));
            auto *apply = dlg->findChild<QPushButton *>(QStringLiteral("preferencesApply"));
            QVERIFY(sections && protocol && port && upnp && apply);
            sections->setCurrentRow(3);
            QTRY_VERIFY_WITH_TIMEOUT(port->isEnabled(), 6000);
            QCOMPARE(protocol->currentIndex(), 1);
            QCOMPARE(port->value(), 51413);
            QVERIFY(upnp->isChecked());
            port->setValue(0);
            apply->click();
            QTRY_VERIFY_WITH_TIMEOUT(port->isEnabled() && !apply->isEnabled(), 6000);
            QCOMPARE(port->text(), QStringLiteral("Any"));
            finished = true;
        });
        prefs->trigger();
        QVERIFY(finished);
        QJsonObject expectedModern = initialModern;
        expectedModern.insert(QStringLiteral("listen_port"), 0);
        expectedModern.insert(QStringLiteral("random_port"), true);
        QCOMPARE(modern.serverPreferences(), expectedModern);
        const QJsonObject expectedModernDelta{{QStringLiteral("listen_port"), 0}};
        QCOMPARE(delta(modern), expectedModernDelta);
        QCOMPARE(legacy.serverPreferences(), expectedLegacy);

        // Invalid or absent values remain disabled, even when a deprecated
        // random_port alias is present.
        modern.setServerPreferences(QJsonObject{
            {QStringLiteral("bittorrent_protocol"), 99},
            {QStringLiteral("listen_port"), 65536},
            {QStringLiteral("random_port"), true}
        });
        finished = false;
        const int reads = modern.preferenceReads();
        QTimer::singleShot(0, &window, [&]() {
            auto *dlg = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget());
            QVERIFY(dlg);
            const auto cleanup = qScopeGuard([dlg]() { if (dlg->isVisible()) dlg->reject(); });
            auto *sections = dlg->findChild<QListWidget *>(QStringLiteral("preferencesSections"));
            auto *protocol = dlg->findChild<QComboBox *>(QStringLiteral("comboProtocol"));
            auto *port = dlg->findChild<QSpinBox *>(QStringLiteral("spinPort"));
            auto *random = dlg->findChild<QPushButton *>(QStringLiteral("randomButton"));
            auto *upnp = dlg->findChild<QCheckBox *>(QStringLiteral("checkUPnP"));
            auto *apply = dlg->findChild<QPushButton *>(QStringLiteral("preferencesApply"));
            QVERIFY(sections && protocol && port && random && upnp && apply);
            sections->setCurrentRow(3);
            // Wait until the server preferences read occurs, including the
            // reply dispatch, before checking unavailable states.
            QTRY_VERIFY_WITH_TIMEOUT(modern.preferenceReads() > reads, 6000);
            QTest::qWait(100);
            QVERIFY(!protocol->isEnabled() && !port->isEnabled()
                && !random->isEnabled() && !upnp->isEnabled() && !apply->isEnabled());
            finished = true;
        });
        prefs->trigger();
        QVERIFY(finished);
        QCOMPARE(modern.commandsFor("/api/v2/app/setPreferences"), 1);
    }

    void connectionLimitsFollowSelectedServer()
    {
        DemoQbittorrentServer legacy(true), modern(true);
        modern.enableModernResponses();
        const QJsonObject initialLegacy{
            {QStringLiteral("max_connec"), 500},
            {QStringLiteral("max_connec_per_torrent"), -1},
            {QStringLiteral("max_uploads"), 8},
            {QStringLiteral("max_uploads_per_torrent"), 4}
        };
        const QJsonObject initialModern{
            {QStringLiteral("max_connec"), 0},
            {QStringLiteral("max_connec_per_torrent"), 75},
            {QStringLiteral("max_uploads"), -1},
            {QStringLiteral("max_uploads_per_torrent"), 3}
        };
        legacy.setServerPreferences(initialLegacy);
        modern.setServerPreferences(initialModern);
        QVERIFY(legacy.start() && modern.start());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("prefs.ini")), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        openConnectDialog(window, legacy.url());
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(QStringLiteral("v5.1.4")), 6000);
        openConnectDialog(window, modern.url(), false, true, true, QStringLiteral("Modern seedbox"));
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(QStringLiteral("v5.3.0")), 6000);
        auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("serverTabs"));
        auto *prefs = window.findChild<QAction *>(QStringLiteral("actionOptions"));
        QVERIFY(tabs && prefs);

        const auto withLimits = [&](auto exercise) {
            bool finished = false;
            QTimer::singleShot(0, &window, [&]() {
                auto *dlg = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget());
                QVERIFY(dlg);
                const auto cleanup = qScopeGuard([dlg]() { if (dlg->isVisible()) dlg->reject(); });
                auto *sections = dlg->findChild<QListWidget *>(QStringLiteral("preferencesSections"));
                auto *apply = dlg->findChild<QPushButton *>(QStringLiteral("preferencesApply"));
                QCheckBox *checks[] = {
                    dlg->findChild<QCheckBox *>(QStringLiteral("checkMaxConnections")),
                    dlg->findChild<QCheckBox *>(QStringLiteral("checkMaxConnectionsPerTorrent")),
                    dlg->findChild<QCheckBox *>(QStringLiteral("checkMaxUploads")),
                    dlg->findChild<QCheckBox *>(QStringLiteral("checkMaxUploadsPerTorrent"))
                };
                QSpinBox *spins[] = {
                    dlg->findChild<QSpinBox *>(QStringLiteral("spinMaxConnec")),
                    dlg->findChild<QSpinBox *>(QStringLiteral("spinMaxConnecPerTorrent")),
                    dlg->findChild<QSpinBox *>(QStringLiteral("spinMaxUploads")),
                    dlg->findChild<QSpinBox *>(QStringLiteral("spinMaxUploadsPerTorrent"))
                };
                QVERIFY(sections && apply);
                for (int i = 0; i < 4; ++i)
                    QVERIFY(checks[i] && spins[i]);
                sections->setCurrentRow(3);
                // The last field is deliberately valid in every fixture,
                // giving a deterministic signal that the read completed.
                QTRY_VERIFY_WITH_TIMEOUT(checks[3]->isEnabled(), 6000);
                exercise(checks, spins, apply);
                finished = true;
            });
            prefs->trigger();
            QVERIFY(finished);
        };
        const auto delta = [](DemoQbittorrentServer &server) {
            const QUrlQuery form(QString::fromUtf8(server.lastCommandBody("/api/v2/app/setPreferences")));
            return QJsonDocument::fromJson(form.queryItemValue(
                QStringLiteral("json"), QUrl::FullyDecoded).toUtf8()).object();
        };

        tabs->setCurrentIndex(0);
        for (bool save : {false, true})
        {
            withLimits([&](QCheckBox **checks, QSpinBox **spins, QPushButton *apply) {
                for (int i = 0; i < 4; ++i)
                    QVERIFY(checks[i]->isEnabled());
                QVERIFY(checks[0]->isChecked() && spins[0]->isEnabled());
                QCOMPARE(spins[0]->value(), 500);
                QVERIFY(!checks[1]->isChecked() && !spins[1]->isEnabled());
                QVERIFY(checks[2]->isChecked() && spins[2]->isEnabled());
                QCOMPARE(spins[2]->value(), 8);
                QVERIFY(checks[3]->isChecked() && spins[3]->isEnabled());
                QCOMPARE(spins[3]->value(), 4);
                QVERIFY(!apply->isEnabled());
                checks[0]->click();
                QVERIFY(!spins[0]->isEnabled());
                checks[0]->click();
                QCOMPARE(spins[0]->value(), 500);
                QVERIFY(!apply->isEnabled());
                checks[0]->click();
                checks[1]->click();
                QVERIFY(spins[1]->isEnabled());
                spins[1]->setValue(120);
                spins[2]->setValue(12);
                checks[3]->click();
                QVERIFY(!spins[3]->isEnabled());
                QVERIFY(apply->isEnabled());
                QCOMPARE(legacy.commandsFor("/api/v2/app/setPreferences"), 0);
                if (save)
                {
                    apply->click();
                    QTRY_VERIFY_WITH_TIMEOUT(checks[0]->isEnabled() && !apply->isEnabled(), 6000);
                    QVERIFY(!checks[0]->isChecked() && !spins[0]->isEnabled());
                    QCOMPARE(spins[1]->value(), 120);
                    QCOMPARE(spins[2]->value(), 12);
                    QVERIFY(!checks[3]->isChecked() && !spins[3]->isEnabled());
                }
            });
            if (!save)
                QCOMPARE(legacy.serverPreferences(), initialLegacy);
        }
        const QJsonObject expectedLegacy{
            {QStringLiteral("max_connec"), -1},
            {QStringLiteral("max_connec_per_torrent"), 120},
            {QStringLiteral("max_uploads"), 12},
            {QStringLiteral("max_uploads_per_torrent"), -1}
        };
        QCOMPARE(legacy.serverPreferences(), expectedLegacy);
        QCOMPARE(delta(legacy), expectedLegacy);
        QCOMPARE(legacy.commandsFor("/api/v2/app/setPreferences"), 1);
        QCOMPARE(modern.serverPreferences(), initialModern);

        tabs->setCurrentIndex(1);
        withLimits([](QCheckBox **checks, QSpinBox **spins, QPushButton *apply) {
            QVERIFY(checks[0]->isEnabled() && !checks[0]->isChecked() && !spins[0]->isEnabled());
            QVERIFY(checks[1]->isChecked() && spins[1]->isEnabled());
            QCOMPARE(spins[1]->value(), 75);
            QVERIFY(checks[2]->isEnabled() && !checks[2]->isChecked() && !spins[2]->isEnabled());
            QVERIFY(checks[3]->isChecked());
            QCOMPARE(spins[3]->value(), 3);
            QVERIFY(!apply->isEnabled()); // 0 is semantically unlimited, not a pending edit.
            spins[1]->setValue(80);
            apply->click();
            QTRY_VERIFY_WITH_TIMEOUT(checks[1]->isEnabled() && !apply->isEnabled(), 6000);
            QCOMPARE(spins[1]->value(), 80);
        });
        QJsonObject expectedModern = initialModern;
        expectedModern.insert(QStringLiteral("max_connec_per_torrent"), 80);
        QCOMPARE(modern.serverPreferences(), expectedModern);
        const QJsonObject expectedModernDelta{{QStringLiteral("max_connec_per_torrent"), 80}};
        QCOMPARE(delta(modern), expectedModernDelta);
        QCOMPARE(modern.commandsFor("/api/v2/app/setPreferences"), 1);
        QCOMPARE(legacy.serverPreferences(), expectedLegacy);

        // A positive value below the native minimum is not silently clamped.
        // Missing and malformed fields must not become fabricated limits.
        modern.setServerPreferences({
            {QStringLiteral("max_connec"), 1},
            {QStringLiteral("max_connec_per_torrent"), 2.5},
            {QStringLiteral("max_uploads_per_torrent"), 6}
        });
        withLimits([](QCheckBox **checks, QSpinBox **spins, QPushButton *apply) {
            for (int i = 0; i < 3; ++i)
                QVERIFY(!checks[i]->isEnabled() && !spins[i]->isEnabled());
            QVERIFY(checks[3]->isChecked() && spins[3]->isEnabled());
            QCOMPARE(spins[3]->value(), 6);
            QVERIFY(!apply->isEnabled());
        });
        QCOMPARE(modern.commandsFor("/api/v2/app/setPreferences"), 1);

        tabs->setCurrentIndex(0);
        withLimits([](QCheckBox **checks, QSpinBox **spins, QPushButton *apply) {
            QVERIFY(!checks[0]->isChecked() && !spins[0]->isEnabled());
            QCOMPARE(spins[1]->value(), 120);
            QCOMPARE(spins[2]->value(), 12);
            QVERIFY(!checks[3]->isChecked() && !spins[3]->isEnabled());
            QVERIFY(!apply->isEnabled());
        });
    }

    void i2pConfigurationFollowsSelectedServerAndBuild()
    {
        DemoQbittorrentServer legacy(true), modern(true), incompatible(true);
        modern.enableModernResponses();
        incompatible.enableModernResponses();
        const QJsonObject initialLegacy{
            {QStringLiteral("i2p_enabled"), false},
            {QStringLiteral("i2p_address"), QStringLiteral("127.0.0.1")},
            {QStringLiteral("i2p_port"), 7656},
            {QStringLiteral("i2p_mixed_mode"), false},
            {QStringLiteral("i2p_inbound_quantity"), 3},
            {QStringLiteral("upnp"), false}
        };
        const QJsonObject initialModern{
            {QStringLiteral("i2p_enabled"), true},
            {QStringLiteral("i2p_address"), QStringLiteral("router.internal")},
            {QStringLiteral("i2p_port"), 7657},
            {QStringLiteral("i2p_mixed_mode"), false},
            {QStringLiteral("i2p_pex_enabled"), false},
            {QStringLiteral("i2p_inbound_length_variance"), 0},
            {QStringLiteral("upnp"), true}
        };
        legacy.setServerPreferences(initialLegacy);
        modern.setServerPreferences(initialModern);
        incompatible.setServerPreferences(initialModern);
        legacy.setBuildInfo({{QStringLiteral("libtorrent"), QStringLiteral("2.0.11.0")}});
        modern.setBuildInfo({{QStringLiteral("libtorrent"), QStringLiteral("2.1.0.0")}});
        incompatible.setBuildInfo({{QStringLiteral("libtorrent"), QStringLiteral("1.2.20.0")}});
        QVERIFY(legacy.start() && modern.start() && incompatible.start());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("prefs.ini")), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        openConnectDialog(window, legacy.url());
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(QStringLiteral("v5.1.4")), 6000);
        openConnectDialog(window, modern.url(), false, true, true, QStringLiteral("Modern seedbox"));
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(QStringLiteral("v5.3.0")), 6000);
        openConnectDialog(window, incompatible.url(), false, true, true, QStringLiteral("Incompatible seedbox"));
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(QStringLiteral("v5.3.0")), 6000);
        auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("serverTabs"));
        auto *prefs = window.findChild<QAction *>(QStringLiteral("actionOptions"));
        QVERIFY(tabs && prefs);
        QTRY_COMPARE(tabs->count(), 3);

        const auto withI2P = [&](const QString &ready, auto exercise) {
            bool finished = false;
            QTimer::singleShot(0, &window, [&]() {
                auto *dlg = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget());
                QVERIFY(dlg);
                const auto cleanup = qScopeGuard([dlg]() { if (dlg->isVisible()) dlg->reject(); });
                auto *sections = dlg->findChild<QListWidget *>(QStringLiteral("preferencesSections"));
                auto *group = dlg->findChild<QGroupBox *>(QStringLiteral("groupI2P"));
                auto *host = dlg->findChild<QLineEdit *>(QStringLiteral("textI2PHost"));
                auto *port = dlg->findChild<QSpinBox *>(QStringLiteral("spinI2PPort"));
                auto *mixed = dlg->findChild<QCheckBox *>(QStringLiteral("checkI2PMixed"));
                auto *pex = dlg->findChild<QCheckBox *>(QStringLiteral("checkI2PPeX"));
                auto *apply = dlg->findChild<QPushButton *>(QStringLiteral("preferencesApply"));
                auto *upnp = dlg->findChild<QCheckBox *>(QStringLiteral("checkUPnP"));
                QVERIFY(sections && group && host && port && mixed && pex && apply && upnp);
                sections->setCurrentRow(3);
                QCOMPARE(group->title(), QStringLiteral("I2P (experimental)"));
                QVERIFY(!dlg->findChild<QLabel *>(QStringLiteral("labelI2PBuildSupport")));
                // A supported group becomes enabled only after the build-info
                // read; disabled cases expose their specific reason on hover.
                if (ready.isEmpty())
                    QTRY_VERIFY_WITH_TIMEOUT(group->isEnabled(), 6000);
                else
                    QTRY_VERIFY_WITH_TIMEOUT(group->toolTip().contains(ready), 6000);
                QVERIFY(upnp->isEnabled());
                exercise(group, host, port, mixed, pex, apply);
                finished = true;
            });
            prefs->trigger();
            QVERIFY(finished);
        };
        const auto delta = [](DemoQbittorrentServer &server) {
            const QUrlQuery form(QString::fromUtf8(server.lastCommandBody("/api/v2/app/setPreferences")));
            return QJsonDocument::fromJson(form.queryItemValue(
                QStringLiteral("json"), QUrl::FullyDecoded).toUtf8()).object();
        };

        tabs->setCurrentIndex(0);
        for (bool save : {false, true})
        {
            withI2P(QString(), [&](QGroupBox *group, QLineEdit *host,
                QSpinBox *port, QCheckBox *mixed, QCheckBox *pex, QPushButton *apply) {
                QVERIFY(group->isEnabled() && !group->isChecked());
                QVERIFY(!host->isEnabled() && !port->isEnabled() && !mixed->isEnabled() && !pex->isEnabled());
                QVERIFY(!apply->isEnabled());
                group->setChecked(true);
                QVERIFY(host->isEnabled() && port->isEnabled() && mixed->isEnabled() && !pex->isEnabled());
                QCOMPARE(host->text(), QStringLiteral("127.0.0.1"));
                QCOMPARE(port->value(), 7656);
                host->setText(QStringLiteral("sam.internal"));
                port->setValue(7660);
                mixed->click();
                QVERIFY(mixed->toolTip().contains(QStringLiteral("regular IP")));
                QVERIFY(apply->isEnabled());
                QCOMPARE(legacy.commandsFor("/api/v2/app/setPreferences"), 0);
                if (save)
                {
                    apply->click();
                    QTRY_VERIFY_WITH_TIMEOUT(group->isEnabled() && !apply->isEnabled(), 6000);
                    QVERIFY(group->isChecked() && mixed->isChecked());
                    QCOMPARE(host->text(), QStringLiteral("sam.internal"));
                    QCOMPARE(port->value(), 7660);
                    QVERIFY(!pex->isEnabled());
                }
            });
            if (!save)
                QCOMPARE(legacy.serverPreferences(), initialLegacy);
        }
        const QJsonObject legacyDelta{
            {QStringLiteral("i2p_enabled"), true},
            {QStringLiteral("i2p_address"), QStringLiteral("sam.internal")},
            {QStringLiteral("i2p_port"), 7660},
            {QStringLiteral("i2p_mixed_mode"), true}
        };
        QJsonObject expectedLegacy = initialLegacy;
        for (auto it = legacyDelta.constBegin(); it != legacyDelta.constEnd(); ++it)
            expectedLegacy.insert(it.key(), it.value());
        QCOMPARE(delta(legacy), legacyDelta);
        QCOMPARE(legacy.serverPreferences(), expectedLegacy);
        QCOMPARE(modern.serverPreferences(), initialModern);

        tabs->setCurrentIndex(1);
        withI2P(QString(), [](QGroupBox *group, QLineEdit *host,
            QSpinBox *port, QCheckBox *mixed, QCheckBox *pex, QPushButton *apply) {
            QVERIFY(group->isEnabled() && group->isChecked() && pex->isEnabled());
            QCOMPARE(host->text(), QStringLiteral("router.internal"));
            QCOMPARE(port->value(), 7657);
            QVERIFY(!mixed->isChecked() && !pex->isChecked());
            QVERIFY(pex->text().contains(QStringLiteral("requires restart")));
            pex->click();
            QVERIFY(apply->isEnabled());
            apply->click();
            QTRY_VERIFY_WITH_TIMEOUT(group->isEnabled() && !apply->isEnabled(), 6000);
            QVERIFY(pex->isChecked());
        });
        const QJsonObject modernDelta{{QStringLiteral("i2p_pex_enabled"), true}};
        QJsonObject expectedModern = initialModern;
        expectedModern.insert(QStringLiteral("i2p_pex_enabled"), true);
        QCOMPARE(delta(modern), modernDelta);
        QCOMPARE(modern.serverPreferences(), expectedModern);
        QCOMPARE(legacy.serverPreferences(), expectedLegacy);

        // All five advertised keys are insufficient on a 1.x build.
        tabs->setCurrentIndex(2);
        withI2P(QStringLiteral("Unavailable for server libtorrent 1.2.20"),
            [](QGroupBox *group, QLineEdit *host, QSpinBox *port, QCheckBox *mixed,
               QCheckBox *pex, QPushButton *apply) {
                QVERIFY(!group->isEnabled() && !host->isEnabled() && !port->isEnabled()
                    && !mixed->isEnabled() && !pex->isEnabled() && !apply->isEnabled());
            });
        QCOMPARE(incompatible.commandsFor("/api/v2/app/setPreferences"), 0);

        // Missing build info must not block ordinary Connection settings.
        incompatible.setBuildInfo({});
        withI2P(QStringLiteral("version could not be verified"),
            [](QGroupBox *group, QLineEdit *host, QSpinBox *port, QCheckBox *mixed,
               QCheckBox *pex, QPushButton *apply) {
                QVERIFY(!group->isEnabled() && !host->isEnabled() && !port->isEnabled()
                    && !mixed->isEnabled() && !pex->isEnabled() && !apply->isEnabled());
            });
        QCOMPARE(incompatible.commandsFor("/api/v2/app/setPreferences"), 0);

        // PeX is separately gated by libtorrent >= 2.1, even when the key
        // is present in a newer qBittorrent built against libtorrent 2.0.
        modern.setBuildInfo({{QStringLiteral("libtorrent"), QStringLiteral("2.0.11.0")}});
        tabs->setCurrentIndex(1);
        withI2P(QString(), [](QGroupBox *group, QLineEdit *host,
            QSpinBox *port, QCheckBox *mixed, QCheckBox *pex, QPushButton *apply) {
            QVERIFY(group->isEnabled() && group->isChecked());
            QVERIFY(host->isEnabled() && port->isEnabled() && mixed->isEnabled() && !pex->isEnabled());
            QVERIFY(!apply->isEnabled());
        });
        QCOMPARE(modern.commandsFor("/api/v2/app/setPreferences"), 1);

        // Individual malformed/missing fields remain unavailable.
        modern.setBuildInfo({{QStringLiteral("libtorrent"), QStringLiteral("2.1.0.0")}});
        modern.setServerPreferences({
            {QStringLiteral("i2p_enabled"), true},
            {QStringLiteral("i2p_address"), false},
            {QStringLiteral("i2p_port"), 65536},
            {QStringLiteral("upnp"), true}
        });
        withI2P(QString(), [](QGroupBox *group, QLineEdit *host,
            QSpinBox *port, QCheckBox *mixed, QCheckBox *pex, QPushButton *apply) {
            QVERIFY(group->isEnabled() && group->isChecked());
            QVERIFY(!host->isEnabled() && !port->isEnabled() && !mixed->isEnabled() && !pex->isEnabled());
            QVERIFY(!apply->isEnabled());
        });
        QCOMPARE(modern.commandsFor("/api/v2/app/setPreferences"), 1);
        tabs->setCurrentIndex(0);
        withI2P(QString(), [](QGroupBox *group, QLineEdit *host,
            QSpinBox *port, QCheckBox *mixed, QCheckBox *pex, QPushButton *apply) {
            QVERIFY(group->isChecked() && mixed->isChecked() && !pex->isEnabled());
            QCOMPARE(host->text(), QStringLiteral("sam.internal"));
            QCOMPARE(port->value(), 7660);
            QVERIFY(!apply->isEnabled());
        });
    }

    void proxyConfigurationFollowsSelectedServer()
    {
        DemoQbittorrentServer legacy(true), modern(true);
        modern.enableModernResponses();
        const QJsonObject initialLegacy{
            {QStringLiteral("proxy_type"), QStringLiteral("None")},
            {QStringLiteral("proxy_ip"), QStringLiteral("proxy.old")},
            {QStringLiteral("proxy_port"), 1080},
            {QStringLiteral("proxy_auth_enabled"), false},
            {QStringLiteral("proxy_username"), QStringLiteral("old-user")},
            {QStringLiteral("proxy_password"), QStringLiteral("old-secret")},
            {QStringLiteral("proxy_hostname_lookup"), false},
            {QStringLiteral("proxy_bittorrent"), false},
            {QStringLiteral("proxy_peer_connections"), false},
            {QStringLiteral("proxy_rss"), false},
            {QStringLiteral("proxy_misc"), false}
        };
        QJsonObject initialModern = initialLegacy;
        initialModern.insert(QStringLiteral("proxy_type"), QStringLiteral("SOCKS4"));
        initialModern.insert(QStringLiteral("proxy_ip"), QStringLiteral("proxy.modern"));
        initialModern.insert(QStringLiteral("proxy_bittorrent"), true);
        initialModern.insert(QStringLiteral("proxy_peer_connections"), true);
        legacy.setServerPreferences(initialLegacy);
        modern.setServerPreferences(initialModern);
        QVERIFY(legacy.start() && modern.start());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("prefs.ini")), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        openConnectDialog(window, legacy.url());
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(QStringLiteral("v5.1.4")), 6000);
        openConnectDialog(window, modern.url(), false, true, true, QStringLiteral("Modern seedbox"));
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(QStringLiteral("v5.3.0")), 6000);
        auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("serverTabs"));
        auto *prefs = window.findChild<QAction *>(QStringLiteral("actionOptions"));
        QVERIFY(tabs && prefs);

        struct Controls
        {
            QComboBox *type;
            QLineEdit *host;
            QSpinBox *port;
            QGroupBox *auth;
            QLineEdit *user;
            QLineEdit *password;
            QCheckBox *lookup;
            QGroupBox *bt;
            QCheckBox *peer;
            QCheckBox *rss;
            QCheckBox *misc;
            QLabel *unavailable;
            QPushButton *apply;
            QCheckBox *upnp;
        };
        const auto withProxy = [&](bool supported, auto exercise) {
            bool finished = false;
            QTimer::singleShot(0, &window, [&]() {
                auto *dlg = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget());
                QVERIFY(dlg);
                const auto cleanup = qScopeGuard([dlg]() { if (dlg->isVisible()) dlg->reject(); });
                auto *sections = dlg->findChild<QListWidget *>(QStringLiteral("preferencesSections"));
                const Controls c{
                    dlg->findChild<QComboBox *>(QStringLiteral("comboProxyType")),
                    dlg->findChild<QLineEdit *>(QStringLiteral("textProxyIP")),
                    dlg->findChild<QSpinBox *>(QStringLiteral("spinProxyPort")),
                    dlg->findChild<QGroupBox *>(QStringLiteral("checkProxyAuth")),
                    dlg->findChild<QLineEdit *>(QStringLiteral("textProxyUsername")),
                    dlg->findChild<QLineEdit *>(QStringLiteral("textProxyPassword")),
                    dlg->findChild<QCheckBox *>(QStringLiteral("checkProxyHostnameLookup")),
                    dlg->findChild<QGroupBox *>(QStringLiteral("checkProxyBitTorrent")),
                    dlg->findChild<QCheckBox *>(QStringLiteral("checkProxyPeerConnections")),
                    dlg->findChild<QCheckBox *>(QStringLiteral("checkProxyRSS")),
                    dlg->findChild<QCheckBox *>(QStringLiteral("checkProxyMisc")),
                    dlg->findChild<QLabel *>(QStringLiteral("labelProxyTypeUnavailable")),
                    dlg->findChild<QPushButton *>(QStringLiteral("preferencesApply")),
                    dlg->findChild<QCheckBox *>(QStringLiteral("checkUPnP"))
                };
                QVERIFY(sections && c.type && c.host && c.port && c.auth && c.user && c.password
                    && c.lookup && c.bt && c.peer && c.rss && c.misc && c.unavailable && c.apply && c.upnp);
                sections->setCurrentRow(3);
                if (supported)
                    QTRY_VERIFY_WITH_TIMEOUT(c.type->isEnabled(), 6000);
                else
                    QTRY_VERIFY_WITH_TIMEOUT(c.upnp->isEnabled(), 6000);
                QCOMPARE(c.type->count(), 4);
                QCOMPARE(c.type->itemText(0), QStringLiteral("(None)"));
                QCOMPARE(c.password->echoMode(), QLineEdit::Password);
                exercise(c);
                finished = true;
            });
            prefs->trigger();
            QVERIFY(finished);
        };
        const auto delta = [](DemoQbittorrentServer &server) {
            const QUrlQuery form(QString::fromUtf8(server.lastCommandBody("/api/v2/app/setPreferences")));
            return QJsonDocument::fromJson(form.queryItemValue(
                QStringLiteral("json"), QUrl::FullyDecoded).toUtf8()).object();
        };

        tabs->setCurrentIndex(0);
        for (bool save : {false, true})
        {
            withProxy(true, [&](const Controls &c) {
                QCOMPARE(c.type->currentIndex(), 0);
                QVERIFY(!c.host->isEnabled() && !c.port->isEnabled() && !c.auth->isEnabled()
                    && !c.lookup->isEnabled() && !c.bt->isEnabled() && !c.peer->isEnabled()
                    && !c.rss->isEnabled() && !c.misc->isEnabled() && c.unavailable->isHidden());
                QVERIFY(!c.apply->isEnabled());
                c.type->setCurrentIndex(1);
                QVERIFY(c.host->isEnabled() && c.port->isEnabled() && c.bt->isEnabled());
                QVERIFY(!c.auth->isEnabled() && !c.lookup->isEnabled() && !c.rss->isEnabled()
                    && !c.misc->isEnabled() && !c.peer->isEnabled() && !c.unavailable->isHidden());
                c.type->setCurrentIndex(2);
                QVERIFY(c.auth->isEnabled() && c.lookup->isEnabled() && c.rss->isEnabled()
                    && c.misc->isEnabled() && c.unavailable->isHidden());
                QVERIFY(!c.user->isEnabled() && !c.password->isEnabled());
                c.type->setCurrentIndex(3);
                QVERIFY(c.auth->isEnabled() && c.lookup->isEnabled() && c.rss->isEnabled()
                    && c.misc->isEnabled());
                c.type->setCurrentIndex(2);
                c.host->setText(QStringLiteral("proxy.new"));
                c.port->setValue(1081);
                c.auth->setChecked(true);
                QVERIFY(c.user->isEnabled() && c.password->isEnabled());
                c.user->setText(QStringLiteral("new-user"));
                c.password->setText(QStringLiteral("secret+&%"));
                c.lookup->click();
                c.bt->setChecked(true);
                QVERIFY(c.peer->isEnabled());
                c.peer->click();
                c.rss->click();
                c.misc->click();
                QVERIFY(c.apply->isEnabled());
                QCOMPARE(legacy.commandsFor("/api/v2/app/setPreferences"), 0);
                if (save)
                {
                    c.apply->click();
                    QTRY_VERIFY_WITH_TIMEOUT(c.type->isEnabled() && !c.apply->isEnabled(), 6000);
                    QCOMPARE(c.host->text(), QStringLiteral("proxy.new"));
                    QCOMPARE(c.port->value(), 1081);
                    QVERIFY(c.auth->isChecked() && c.bt->isChecked() && c.peer->isChecked());
                }
            });
            if (!save)
                QCOMPARE(legacy.serverPreferences(), initialLegacy);
        }
        const QJsonObject expectedLegacy{
            {QStringLiteral("proxy_type"), QStringLiteral("SOCKS5")},
            {QStringLiteral("proxy_ip"), QStringLiteral("proxy.new")},
            {QStringLiteral("proxy_port"), 1081},
            {QStringLiteral("proxy_auth_enabled"), true},
            {QStringLiteral("proxy_username"), QStringLiteral("new-user")},
            {QStringLiteral("proxy_password"), QStringLiteral("secret+&%")},
            {QStringLiteral("proxy_hostname_lookup"), true},
            {QStringLiteral("proxy_bittorrent"), true},
            {QStringLiteral("proxy_peer_connections"), true},
            {QStringLiteral("proxy_rss"), true},
            {QStringLiteral("proxy_misc"), true}
        };
        QCOMPARE(delta(legacy), expectedLegacy);
        QCOMPARE(legacy.serverPreferences(), expectedLegacy);
        QCOMPARE(modern.serverPreferences(), initialModern);

        tabs->setCurrentIndex(1);
        withProxy(true, [](const Controls &c) {
            QCOMPARE(c.type->currentIndex(), 1);
            QCOMPARE(c.host->text(), QStringLiteral("proxy.modern"));
            QVERIFY(c.bt->isChecked() && c.peer->isEnabled() && c.peer->isChecked());
            QVERIFY(!c.auth->isEnabled() && !c.lookup->isEnabled() && !c.rss->isEnabled()
                && !c.misc->isEnabled() && !c.unavailable->isHidden());
            c.bt->setChecked(false);
            QVERIFY(!c.peer->isEnabled());
            c.bt->setChecked(true);
            QVERIFY(c.peer->isEnabled() && c.peer->isChecked());
            QVERIFY(!c.apply->isEnabled());
            c.type->setCurrentIndex(0);
            QVERIFY(!c.host->isEnabled() && !c.peer->isEnabled());
            c.apply->click();
            QTRY_VERIFY_WITH_TIMEOUT(c.type->isEnabled() && !c.apply->isEnabled(), 6000);
        });
        const QJsonObject modernDelta{{QStringLiteral("proxy_type"), QStringLiteral("None")}};
        QCOMPARE(delta(modern), modernDelta);
        QJsonObject expectedModern = initialModern;
        expectedModern.insert(QStringLiteral("proxy_type"), QStringLiteral("None"));
        QCOMPARE(modern.serverPreferences(), expectedModern);
        QCOMPARE(legacy.serverPreferences(), expectedLegacy);
        settings.sync();
        QFile settingsFile(temp.filePath(QStringLiteral("prefs.ini")));
        QVERIFY(settingsFile.open(QIODevice::ReadOnly));
        const QByteArray saved = settingsFile.readAll();
        QVERIFY(!saved.contains("secret+&%") && !saved.contains("old-secret"));
        settingsFile.close();

        // An unknown type must not become None and must not enable dependent controls.
        modern.setServerPreferences({
            {QStringLiteral("proxy_type"), QStringLiteral("Other")},
            {QStringLiteral("proxy_ip"), QStringLiteral("host")},
            {QStringLiteral("proxy_port"), 1080},
            {QStringLiteral("proxy_bittorrent"), true},
            {QStringLiteral("upnp"), true}
        });
        withProxy(false, [](const Controls &c) {
            QVERIFY(!c.type->isEnabled() && !c.host->isEnabled() && !c.port->isEnabled()
                && !c.bt->isEnabled() && !c.auth->isEnabled() && !c.peer->isEnabled());
            QVERIFY(!c.apply->isEnabled());
        });
        modern.setServerPreferences({
            {QStringLiteral("proxy_type"), QStringLiteral("HTTP")},
            {QStringLiteral("proxy_ip"), false},
            {QStringLiteral("proxy_port"), 65536},
            {QStringLiteral("proxy_auth_enabled"), true},
            {QStringLiteral("proxy_username"), QStringLiteral("name")},
            {QStringLiteral("proxy_peer_connections"), true}
        });
        withProxy(true, [](const Controls &c) {
            QCOMPARE(c.type->currentIndex(), 3);
            QVERIFY(!c.host->isEnabled() && !c.port->isEnabled() && c.auth->isEnabled());
            QVERIFY(c.user->isEnabled() && !c.password->isEnabled());
            QVERIFY(!c.bt->isEnabled() && !c.peer->isEnabled() && !c.lookup->isEnabled()
                && !c.rss->isEnabled() && !c.misc->isEnabled());
            QVERIFY(!c.apply->isEnabled());
        });
        QCOMPARE(modern.commandsFor("/api/v2/app/setPreferences"), 1);

        tabs->setCurrentIndex(0);
        withProxy(true, [](const Controls &c) {
            QCOMPARE(c.type->currentIndex(), 2);
            QCOMPARE(c.host->text(), QStringLiteral("proxy.new"));
            QCOMPARE(c.port->value(), 1081);
            QVERIFY(c.auth->isChecked() && c.lookup->isChecked() && c.bt->isChecked()
                && c.peer->isChecked() && c.rss->isChecked() && c.misc->isChecked());
            QVERIFY(!c.apply->isEnabled());
        });
    }

    void rssReaderAndRulesFollowSelectedServer()
    {
        DemoQbittorrentServer legacy(true), modern(true);
        modern.enableModernResponses();
        const QString feedUrl = QStringLiteral("https://feed.example/rss?key=a+b%2Fc&x=1");
        const QJsonObject article{{"id", "article-id"}, {"title", "Example article"},
            {"date", "Wed, 25 Nov 2026 12:00:00 +0000"}, {"description", "<b>Example description</b><img src='https://feed.example/image.png' width='1200' height='900'><img src='file:///tmp/private.png'>"},
            {"link", "https://feed.example/news/story"}, {"torrentURL", "https://feed.example/download?key=a+b%2Fc&x=1"}, {"isRead", false}};
        const QJsonObject feed{{"uid", "id1"}, {"url", feedUrl}, {"articles", QJsonArray{article}}};
        const QJsonObject rule{{"enabled", false}, {"priority", 0}, {"useRegex", false},
            {"mustContain", "Old"}, {"mustNotContain", ""}, {"episodeFilter", ""},
            {"ignoreDays", 0}, {"smartFilter", false}, {"affectedFeeds", QJsonArray{feedUrl}},
            {"torrentParams", QJsonObject{{"category", "Music"}}}, {"previouslyMatchedEpisodes", QJsonArray{"1x01"}}};
        const QJsonObject legacyItems{{"News", feed}};
        const QJsonObject legacyRules{{"Existing", rule}};
        const QJsonObject modernItems{{"Other", QJsonObject{{"uid", "id2"}, {"url", "https://other.example/rss"}, {"articles", QJsonArray{}}}}};
        legacy.setRssData(legacyItems, legacyRules);
        modern.setRssData(modernItems, {});
        QVERIFY(legacy.start() && modern.start());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("prefs.ini")), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        openConnectDialog(window, legacy.url());
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(QStringLiteral("v5.1.4")), 6000);
        openConnectDialog(window, modern.url(), false, true, true, QStringLiteral("Modern seedbox"));
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(QStringLiteral("v5.3.0")), 6000);
        auto *tabs = window.findChild<QTabWidget *>("serverTabs");
        auto *action = window.findChild<QAction *>("actionRemoteRSS");
        QVERIFY(tabs && action && action->isEnabled());

        const auto withReader = [&](auto exercise) {
            auto *sections = tabs->currentWidget()->findChild<QTabWidget *>("serverSectionTabs");
            QVERIFY(sections && sections->isTabVisible(1));
            sections->setCurrentIndex(1);
            auto *reader = sections->currentWidget();
            QVERIFY(reader && reader->objectName() == QStringLiteral("remoteRssWidget"));
            QVERIFY(reader->isVisible());
            auto *add = reader->findChild<QPushButton *>("rssAddFeed");
            QVERIFY(add);
            QTRY_VERIFY_WITH_TIMEOUT(add->isEnabled(), 6000);
            exercise(reader);
        };
        tabs->setCurrentIndex(0);
        QVERIFY(action->isCheckable() && !action->isChecked());
        action->trigger();
        QVERIFY(action->isChecked());
        withReader([&](QWidget *dlg) {
            auto *feeds = dlg->findChild<QTreeWidget *>("rssFeeds");
            auto *articles = dlg->findChild<QTreeWidget *>("rssArticles");
            auto *preview = dlg->findChild<QTextBrowser *>("rssArticlePreview");
            auto *mark = dlg->findChild<QPushButton *>("rssMarkArticle");
            auto *add = dlg->findChild<QPushButton *>("rssAddFeed");
            auto *downloader = dlg->findChild<QPushButton *>("rssDownloader");
            QVERIFY(feeds && articles && preview && mark && add && downloader);
            QCOMPARE(feeds->topLevelItem(0)->childCount(), 1);
            QCOMPARE(feeds->topLevelItem(0)->child(0)->text(0), QStringLiteral("News (1)"));
            QCOMPARE(articles->topLevelItemCount(), 1);
            articles->setCurrentItem(articles->topLevelItem(0));
            QVERIFY(preview->toPlainText().contains(QStringLiteral("Example description")));
            QVERIFY(preview->toHtml().contains(QStringLiteral("href=\"https://feed.example/news/story\"")));
            QVERIFY(!preview->toHtml().contains(QStringLiteral("<img"), Qt::CaseInsensitive));
            QVERIFY(!preview->openExternalLinks());
            QTRY_VERIFY_WITH_TIMEOUT(add->isEnabled(), 6000);
            QCOMPARE(legacy.commandsFor("/api/v2/rss/markAsRead"), 1);
            auto *download = dlg->findChild<QPushButton *>("rssDownload");
            QVERIFY(download && download->isEnabled());
            QTimer::singleShot(0, dlg, [&]() {
                auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                if (box) box->button(QMessageBox::Yes)->click();
            });
            download->click();
            QTRY_COMPARE_WITH_TIMEOUT(legacy.commandsFor("/api/v2/torrents/add"), 1, 6000);
            const QByteArray wire = legacy.lastCommandBody("/api/v2/torrents/add");
            const QUrlQuery decoded(QString::fromUtf8(QByteArray(wire).replace('+', ' ')));
            QCOMPARE(decoded.queryItemValue(QStringLiteral("urls"), QUrl::FullyDecoded),
                     article.value("torrentURL").toString());
            QCOMPARE(modern.commandsFor("/api/v2/torrents/add"), 0);
            QTRY_VERIFY_WITH_TIMEOUT(add->isEnabled(), 6000);
            QCOMPARE(legacy.commandsFor("/api/v2/rss/markAsRead"), 1);
            QVERIFY(legacy.rssItems().value("News").toObject().value("articles").toArray().at(0).toObject().value("isRead").toBool());

            QTimer::singleShot(0, dlg, [&]() {
                auto *sub = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                if (!sub || sub == dlg) return;
                const auto cleanup = qScopeGuard([sub]() { if (sub->isVisible()) sub->reject(); });
                auto *path = sub->findChild<QLineEdit *>("rssItemPath");
                auto *url = sub->findChild<QLineEdit *>("rssFeedUrl");
                auto *box = sub->findChild<QDialogButtonBox *>();
                QVERIFY(path && url && box);
                path->setText(QStringLiteral("New feed"));
                url->setText(feedUrl + QStringLiteral("&new=2"));
                box->button(QDialogButtonBox::Ok)->click();
            });
            add->click();
            QTRY_VERIFY_WITH_TIMEOUT(add->isEnabled(), 6000);
            QCOMPARE(legacy.rssItems().value("New feed").toObject().value("url").toString(), feedUrl + QStringLiteral("&new=2"));
            QCOMPARE(feeds->topLevelItem(0)->childCount(), 2);

            bool edited = false;
            QTimer::singleShot(0, dlg, [&]() {
                auto *ruleDialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                QVERIFY(ruleDialog && ruleDialog->objectName() == QStringLiteral("remoteRssDialog"));
                const auto cleanup = qScopeGuard([ruleDialog]() { if (ruleDialog->isVisible()) ruleDialog->reject(); });
                auto *rules = ruleDialog->findChild<QListWidget *>("rssRules");
                auto *editRule = ruleDialog->findChild<QPushButton *>("rssEditRule");
                QVERIFY(rules && editRule);
                QTRY_VERIFY_WITH_TIMEOUT(editRule->isEnabled(), 6000);
                QCOMPARE(rules->count(), 1);
                rules->setCurrentRow(0);
                QTimer::singleShot(0, ruleDialog, [&]() {
                auto *editor = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                if (!editor || editor == ruleDialog) return;
                const auto cleanup = qScopeGuard([editor]() { if (editor->isVisible()) editor->reject(); });
                auto *must = editor->findChild<QLineEdit *>("rssRuleMustContain");
                auto *box = editor->findChild<QDialogButtonBox *>();
                QVERIFY(must && box);
                must->setText(QStringLiteral("New match"));
                box->button(QDialogButtonBox::Ok)->click();
            });
            editRule->click();
            QTRY_VERIFY_WITH_TIMEOUT(editRule->isEnabled(), 6000);
            const auto changed = legacy.rssRules().value("Existing").toObject();
            QCOMPARE(changed.value("mustContain").toString(), QStringLiteral("New match"));
            QCOMPARE(changed.value("torrentParams"), rule.value("torrentParams"));
            QCOMPARE(changed.value("previouslyMatchedEpisodes"), rule.value("previouslyMatchedEpisodes"));
            QCOMPARE(changed.value("affectedFeeds"), rule.value("affectedFeeds"));
                QCOMPARE(modern.commandsFor("/api/v2/rss/setRule"), 0);
                edited = true;
            });
            downloader->click();
            QVERIFY(edited);
            QTRY_VERIFY_WITH_TIMEOUT(add->isEnabled(), 6000);
        });
        auto *legacyReader = tabs->currentWidget()->findChild<QWidget *>("remoteRssWidget");
        tabs->setCurrentIndex(1);
        withReader([&](QWidget *dlg) {
            auto *feeds = dlg->findChild<QTreeWidget *>("rssFeeds");
            auto *rules = dlg->findChild<QListWidget *>("rssRules");
            QVERIFY(feeds && rules);
            QCOMPARE(feeds->topLevelItem(0)->childCount(), 1);
            QCOMPARE(feeds->topLevelItem(0)->child(0)->text(0), QStringLiteral("Other"));
            QCOMPARE(rules->count(), 0);
        });
        tabs->setCurrentIndex(0);
        auto *sections = tabs->currentWidget()->findChild<QTabWidget *>("serverSectionTabs");
        QVERIFY(sections && sections->currentWidget() == legacyReader);
        QCOMPARE(legacyReader->findChild<QTabWidget *>("rssTabs")->currentIndex(), 0);
        action->trigger();
        QVERIFY(!action->isChecked());
        QCOMPARE(sections->currentIndex(), 0);
        QVERIFY(!sections->isTabVisible(1));
        action->trigger();
        QCOMPARE(sections->currentWidget(), legacyReader);
        QCOMPARE(modern.rssItems(), modernItems);
        QCOMPARE(modern.commandsFor("/api/v2/rss/addFeed"), 0);
        QCOMPARE(legacy.commandsFor("/api/v2/rss/addFeed"), 1);
    }

    void directWebUIConnectionTransition_data()
    {
        QTest::addColumn<bool>("modern");
        QTest::addColumn<bool>("bypassLocal");
        QTest::addColumn<bool>("bypassSubnet");
        QTest::newRow("modern-authenticated") << true << false << false;
        QTest::newRow("legacy-local-bypass") << false << true << false;
        QTest::newRow("modern-subnet-bypass") << true << false << true;
    }

    void directWebUIConnectionTransition()
    {
        QFETCH(bool, modern);
        QFETCH(bool, bypassLocal);
        QFETCH(bool, bypassSubnet);
        DemoQbittorrentServer server(true);
        if (modern) server.enableModernResponses();
        const QString version = modern ? QStringLiteral("v5.3.0") : QStringLiteral("v5.1.4");
        server.enableWebUITransitionDemo();
        QVERIFY(server.start());
        const QString oldUrl = server.url();
        server.setServerPreferences({
            {"web_ui_address", "*"}, {"web_ui_port", QUrl(oldUrl).port()},
            {"use_https", false}, {"web_ui_https_cert_path", ""}, {"web_ui_https_key_path", ""},
            {"web_ui_username", "demo"}, {"bypass_local_auth", bypassLocal},
            {"bypass_auth_subnet_whitelist_enabled", bypassSubnet}, {"web_ui_reverse_proxy_enabled", false}
        });
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath("profiles.ini"), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        openConnectDialog(window, oldUrl, true);
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(version), 6000);
        const auto original = Remote::ProfileStore(settings).profiles().first();
        const QString originalId = original.id;
        const QList<Remote::PathMapping> mappings{{temp.path(), QStringLiteral("/share/downloads")}};
        QVERIFY(Remote::PathMappingStore(settings).replace(originalId, mappings));
        QTcpServer reserve;
        QVERIFY(reserve.listen(QHostAddress::LocalHost, 0));
        const int newPort = reserve.serverPort();
        reserve.close();
        auto *prefs = window.findChild<QAction *>("actionOptions");
        QVERIFY(prefs);
        const QString newPassword = QStringLiteral("changed+&%password");
        bool confirmed = false, passwordEntered = false, edited = false;
        bool expectFailure = false, failureReported = false, verificationLimited = false;
        QString unexpected;
        QTimer modalDriver;
        modalDriver.setInterval(20);
        connect(&modalDriver, &QTimer::timeout, &window, [&] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
            {
                if (box->windowTitle() == QStringLiteral("Change server connection"))
                {
                    confirmed = true;
                    box->button(QMessageBox::Yes)->click();
                }
                else if (box->windowTitle() == QStringLiteral("Password verification limited"))
                {
                    verificationLimited = true;
                    box->accept();
                }
                else if (expectFailure && box->windowTitle() == QStringLiteral("Cannot verify connection change"))
                {
                    failureReported = true;
                    box->accept();
                }
                else
                {
                    unexpected = box->windowTitle() + QStringLiteral(": ") + box->text();
                    box->reject();
                }
            }
            else if (auto *input = qobject_cast<QInputDialog *>(QApplication::activeModalWidget()))
            {
                if (input->windowTitle() == QStringLiteral("Verify server login"))
                {
                    passwordEntered = true;
                    input->setTextValue(newPassword);
                    input->accept();
                }
            }
        });
        modalDriver.start();
        QTimer::singleShot(0, &window, [&] {
            auto *dlg = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget());
            QVERIFY(dlg);
            auto *nav = dlg->findChild<QListWidget *>("preferencesSections");
            auto *port = dlg->findChild<QSpinBox *>("spinWebUIPort");
            auto *user = dlg->findChild<QLineEdit *>("textWebUIUsername");
            auto *password = dlg->findChild<QLineEdit *>("textWebUIPassword");
            auto *apply = dlg->findChild<QPushButton *>("preferencesApply");
            QVERIFY(nav && port && user && password && apply);
            nav->setCurrentRow(8);
            QTRY_VERIFY_WITH_TIMEOUT(port->isEnabled() && password->isEnabled(), 6000);
            QCOMPARE(port->value(), QUrl(oldUrl).port());
            QVERIFY(password->text().isEmpty());
            auto *https = dlg->findChild<QGroupBox *>("checkWebUIHttps");
            auto *certificate = dlg->findChild<QLineEdit *>("textWebUIHttpsCert");
            auto *keyPath = dlg->findChild<QLineEdit *>("textWebUIHttpsKey");
            auto *certificateLink = dlg->findChild<QLabel *>("lblWebUICertInfo");
            QVERIFY(https && certificate && keyPath && certificateLink);
            QVERIFY(!https->isChecked() && !certificate->isEnabled() && !keyPath->isEnabled());
            QVERIFY(certificateLink->isEnabled());
            https->setChecked(true);
            QVERIFY(certificate->isEnabled() && keyPath->isEnabled() && certificateLink->isEnabled());
            https->setChecked(false);
            QVERIFY(!certificate->isEnabled() && !keyPath->isEnabled() && certificateLink->isEnabled());
            port->setValue(newPort);
            user->setText("new-user");
            password->setText(newPassword);
            QVERIFY(apply->isEnabled());
            edited = true;
            apply->click();
        });
        // Fail rather than hang indefinitely if a regression leaves the modal
        // open. The watchdog only dismisses an idle dialog; active requests
        // still use the normal transport timeout.
        QTimer watchdog;
        watchdog.setSingleShot(true);
        connect(&watchdog, &QTimer::timeout, &window, [&] {
            if (auto *dlg = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget()))
                dlg->reject();
        });
        watchdog.start(35000);
        prefs->trigger();
        watchdog.stop();
        modalDriver.stop();
        QVERIFY2(unexpected.isEmpty(), qPrintable(unexpected));
        QVERIFY(edited && confirmed && !passwordEntered);
        QCOMPARE(verificationLimited, bypassLocal || bypassSubnet);
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(version), 12000);
        const auto saved = Remote::ProfileStore(settings).profiles();
        QCOMPARE(saved.size(), 1);
        QCOMPARE(saved.first().id, originalId);
        QCOMPARE(QUrl(saved.first().url).port(), newPort);
        QCOMPARE(saved.first().username, QStringLiteral("new-user"));
        QTRY_COMPARE_WITH_TIMEOUT(vault.data.value(vault.keyFor(saved.first())), newPassword, 6000);
        QVERIFY(!vault.data.contains(vault.keyFor(original)));
        QCOMPARE(Remote::PathMappingStore(settings).mappings(originalId), mappings);
        QCOMPARE(server.commandsFor("/api/v2/app/setPreferences"), 1);
        QCOMPARE(server.serverPreferences().value("web_ui_port").toInt(), newPort);
        QCOMPARE(server.serverPreferences().value("web_ui_username").toString(), QStringLiteral("new-user"));
        QVERIFY(!server.serverPreferences().contains("web_ui_password"));
        QCOMPARE(server.serverPreferences().value("bypass_local_auth").toBool(), bypassLocal);
        QCOMPARE(server.serverPreferences().value("bypass_auth_subnet_whitelist_enabled").toBool(), bypassSubnet);
        settings.sync();
        QFile file(settings.fileName());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(!file.readAll().contains(newPassword.toUtf8()));
        file.close();

        // A write can return success even if the listener remains unchanged.
        // The proposed endpoint must not replace the verified saved profile.
        QVERIFY(reserve.listen(QHostAddress::LocalHost, 0));
        const int unavailablePort = reserve.serverPort();
        reserve.close();
        server.ignorePreferenceWrites(true);
        expectFailure = true;
        modalDriver.start();
        watchdog.start(35000);
        QTimer::singleShot(0, &window, [&] {
            auto *dlg = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget());
            QVERIFY(dlg);
            auto *port = dlg->findChild<QSpinBox *>("spinWebUIPort");
            auto *apply = dlg->findChild<QPushButton *>("preferencesApply");
            QVERIFY(port && apply);
            QTRY_VERIFY_WITH_TIMEOUT(port->isEnabled(), 6000);
            port->setValue(unavailablePort);
            apply->click();
        });
        prefs->trigger();
        watchdog.stop();
        modalDriver.stop();
        QVERIFY2(unexpected.isEmpty(), qPrintable(unexpected));
        QVERIFY(failureReported);
        QVERIFY(passwordEntered);
        const auto retained = Remote::ProfileStore(settings).profiles().first();
        QCOMPARE(retained.url, saved.first().url);
        QCOMPARE(retained.username, saved.first().username);
        QCOMPARE(server.commandsFor("/api/v2/app/setPreferences"), 2);
        QCOMPARE(Remote::PathMappingStore(settings).mappings(originalId), mappings);
    }

    void advancedServerCapabilitiesWorkflow()
    {
        DemoQbittorrentServer legacy(true), modern(true);
        modern.enableModernResponses();
        QVERIFY(legacy.start() && modern.start());
        legacy.setBuildInfo({{"libtorrent", "1.2.20.0"}, {"platform", "linux"}, {"bitness", 32}});
        modern.setBuildInfo({{"libtorrent", "2.1.0.0"}, {"platform", "windows"}, {"bitness", 64}});
        const QJsonObject values{
            {"current_network_interface", "nas0"}, {"current_interface_address", "10.1.0.2"},
            {"resume_data_storage_type", "Legacy"}, {"disk_io_type", 0}, {"disk_io_write_mode", 1},
            {"checking_memory_use", 64}, {"hashing_threads", 4}, {"disk_cache", 512},
            {"disk_cache_ttl", 60}, {"enable_coalesce_read_write", false},
            {"memory_working_set_limit", 256}, {"mark_of_the_web", false},
            {"announce_ip", ""}, {"announce_port", 0}, {"start_paused", false},
            {"shutdown_timeout", -1}, {"python_executable_path", ""},
            {"resolve_peer_countries", false},
            {"listen_port", 6881}, {"proxy_type", "None"}
        };
        legacy.setServerPreferences(values);
        QJsonObject modernValues = values;
        modernValues.insert("resolve_peer_host_names", false);
        modern.setServerPreferences(modernValues);
        const QJsonArray ifaces{QJsonObject{{"name", "NAS interface"}, {"value", "nas0"}},
            QJsonObject{{"name", "VPN tunnel"}, {"value", "vpn&1"}}};
        const QHash<QString, QJsonArray> addresses{{"nas0", QJsonArray{"10.1.0.2"}},
            {"vpn&1", QJsonArray{"10.2.0.3"}}, {"", QJsonArray{"10.1.0.2", "10.2.0.3"}}};
        legacy.setNetworkLists(ifaces, addresses);
        modern.setNetworkLists(ifaces, addresses);
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath("profiles.ini"), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        openConnectDialog(window, legacy.url());
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains("v5.1.4"), 6000);
        openConnectDialog(window, modern.url(), false, true, true, "Modern");
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains("v5.3.0"), 6000);
        auto *tabs = window.findChild<QTabWidget *>("serverTabs");
        auto *prefs = window.findChild<QAction *>("actionOptions");
        QVERIFY(tabs && prefs);
        for (const bool newer : {false, true})
        {
            tabs->setCurrentIndex(newer ? 1 : 0);
            bool finished = false;
            QTimer::singleShot(0, &window, [&] {
                auto *dlg = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget());
                QVERIFY(dlg);
                const auto cleanup = qScopeGuard([dlg] { if (dlg->isVisible()) dlg->reject(); });
                auto *nav = dlg->findChild<QListWidget *>("preferencesSections");
                auto *apply = dlg->findChild<QPushButton *>("preferencesApply");
                auto *iface = dlg->findChild<QComboBox *>("advanced_NETWORK_IFACE");
                auto *addr = dlg->findChild<QComboBox *>("advanced_NETWORK_IFACE_ADDRESS");
                auto *storage = dlg->findChild<QComboBox *>("advanced_RESUME_DATA_STORAGE");
                auto *disk = dlg->findChild<QComboBox *>("advanced_DISK_IO_TYPE");
                auto *write = dlg->findChild<QComboBox *>("advanced_DISK_IO_WRITE_MODE");
                auto *cache = dlg->findChild<QSpinBox *>("advanced_DISK_CACHE");
                auto *mem = dlg->findChild<QSpinBox *>("advanced_CHECKING_MEM_USAGE");
                auto *hash = dlg->findChild<QSpinBox *>("advanced_HASHING_THREADS");
                auto *motw = dlg->findChild<QCheckBox *>("advanced_ENABLE_MARK_OF_THE_WEB");
                QVERIFY(nav && apply && iface && addr && storage && disk && write && cache && mem && hash && motw);
                nav->setCurrentRow(9);
                QTRY_VERIFY_WITH_TIMEOUT(iface->isEnabled() && addr->isEnabled(), 6000);
                auto *refresh = dlg->findChild<QSpinBox *>("advanced_LIST_REFRESH");
                auto *graphs = dlg->findChild<QCheckBox *>("advanced_ENABLE_SPEED_WIDGET");
                auto *notifications = dlg->findChild<QCheckBox *>("advanced_PROGRAM_NOTIFICATIONS");
                auto *added = dlg->findChild<QCheckBox *>("advanced_TORRENT_ADDED_NOTIFICATIONS");
                auto *timeout = dlg->findChild<QSpinBox *>("advanced_NOTIFICATION_TIMEOUT");
                auto *menuIcons = dlg->findChild<QCheckBox *>("advanced_ENABLE_ICONS_IN_MENUS");
                auto *history = dlg->findChild<QSpinBox *>("advanced_SAVE_PATH_HISTORY_LENGTH");
                auto *attached = dlg->findChild<QCheckBox *>("advanced_USE_ATTACHED_ADD_NEW_TORRENT_DIALOG");
                QVERIFY(refresh && graphs && notifications && added && timeout && menuIcons && history && attached);
                QVERIFY(refresh->isEnabled() && graphs->isEnabled() && notifications->isEnabled() && added->isEnabled()
                    && timeout->isEnabled() && menuIcons->isEnabled() && history->isEnabled() && attached->isEnabled());
                QCOMPARE(refresh->value(), 2000);
                QVERIFY(graphs->isChecked() && notifications->isChecked() && attached->isChecked() && menuIcons->isChecked());
                QVERIFY(!added->isChecked());
                refresh->setValue(500);
                graphs->setChecked(false);
                notifications->setChecked(false);
                added->setChecked(true);
                timeout->setValue(3000);
                menuIcons->setChecked(false);
                history->setValue(2);
                attached->setChecked(false);
                QVERIFY(apply->isEnabled());
                refresh->setValue(2000);
                graphs->setChecked(true);
                notifications->setChecked(true);
                added->setChecked(false);
                timeout->setValue(-1);
                menuIcons->setChecked(true);
                history->setValue(8);
                attached->setChecked(true);
                QCOMPARE(iface->currentData().toString(), QStringLiteral("nas0"));
                QCOMPARE(addr->currentData().toString(), QStringLiteral("10.1.0.2"));
                QCOMPARE(mem->maximum(), newer ? 1024 : 128);
                QCOMPARE(disk->isEnabled(), newer);
                QCOMPARE(hash->isEnabled(), newer);
                QCOMPARE(motw->isEnabled(), newer);
                QCOMPARE(cache->isEnabled(), !newer);
                QCOMPARE(write->count(), newer ? 3 : 2);
                auto *python = dlg->findChild<QLineEdit *>("advanced_PYTHON_EXECUTABLE_PATH");
                QVERIFY(python && python->isEnabled());
                QVERIFY(python->text().isEmpty() && python->placeholderText().isEmpty());
                QVERIFY(!apply->isEnabled());
                // Wheel events over real enabled editors, including focused
                // editors, must not change values or stage remote writes.
                const auto wheel = [](QWidget *editor, int delta = -120) {
                    editor->setFocus();
                    const QPoint local = editor->rect().center();
                    QWheelEvent event(QPointF(local), QPointF(editor->mapToGlobal(local)),
                        QPoint(), QPoint(0, delta), Qt::NoButton, Qt::NoModifier,
                        Qt::NoScrollPhase, false);
                    QApplication::sendEvent(editor, &event);
                };
                const int memoryBefore = mem->value();
                const int storageBefore = storage->currentIndex();
                auto *advanced = dlg->findChild<QTableWidget *>("advancedSettingsTable");
                QVERIFY(advanced);
                advanced->scrollToTop();
                QTRY_VERIFY(advanced->verticalScrollBar()->maximum() > 0);
                wheel(storage);
                QVERIFY(advanced->verticalScrollBar()->value() > 0);
                wheel(mem);
                QCOMPARE(mem->value(), memoryBefore);
                QCOMPARE(storage->currentIndex(), storageBefore);
                QVERIFY(!apply->isEnabled());

                // Designer-created editors outside Advanced use the same
                // application policy; this is not a special table workaround.
                nav->setCurrentRow(3);
                auto *proxy = dlg->findChild<QComboBox *>("comboProxyType");
                auto *listen = dlg->findChild<QSpinBox *>("spinPort");
                QVERIFY(proxy && listen && proxy->isEnabled() && listen->isEnabled());
                const int proxyBefore = proxy->currentIndex();
                const int listenBefore = listen->value();
                wheel(proxy);
                wheel(listen);
                QCOMPARE(proxy->currentIndex(), proxyBefore);
                QCOMPARE(listen->value(), listenBefore);
                QVERIFY(!apply->isEnabled());
                nav->setCurrentRow(9);
                iface->setCurrentIndex(iface->findData(QStringLiteral("vpn&1")));
                QTRY_VERIFY_WITH_TIMEOUT(addr->isEnabled() && addr->findData(QStringLiteral("10.2.0.3")) >= 0, 6000);
                QCOMPARE(addr->findData(QStringLiteral("10.1.0.2")), -1);
                addr->setCurrentIndex(addr->findData(QStringLiteral("10.2.0.3")));
                storage->setCurrentIndex(1);
                auto *countries = dlg->findChild<QCheckBox *>("advanced_RESOLVE_COUNTRIES");
                auto *hosts = dlg->findChild<QCheckBox *>("advanced_RESOLVE_HOSTS");
                QVERIFY(countries && hosts && countries->isEnabled());
                QCOMPARE(hosts->isEnabled(), newer);
                countries->setChecked(true);
                if (newer) { disk->setCurrentIndex(4); hosts->setChecked(true); }
                else cache->setValue(768);
                refresh->setValue(500);
                graphs->setChecked(false);
                notifications->setChecked(false);
                added->setChecked(true);
                timeout->setValue(3000);
                menuIcons->setChecked(false);
                history->setValue(2);
                attached->setChecked(false);
                QVERIFY(apply->isEnabled());
                if (newer)
                {
                    QTimer driver;
                    connect(&driver, &QTimer::timeout, dlg, [] {
                        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                        {
                            if (box->windowTitle() == "Change Advanced server settings") box->button(QMessageBox::Yes)->click();
                            else box->reject();
                        }
                    });
                    driver.start(20);
                    apply->click();
                    QTRY_VERIFY_WITH_TIMEOUT(nav->isEnabled() && !apply->isEnabled(), 6000);
                }
                finished = true;
            });
            prefs->trigger();
            QVERIFY(finished);
        }
        QCOMPARE(legacy.serverPreferences(), values);
        QCOMPARE(legacy.commandsFor("/api/v2/app/setPreferences"), 0);
        QCOMPARE(settings.value("Preferences/Advanced/RefreshInterval").toInt(), 500);
        QVERIFY(!settings.value("Preferences/Advanced/EnableSpeedGraphs").toBool());
        QVERIFY(!settings.value("Preferences/Advanced/DisplayNotifications").toBool());
        QVERIFY(settings.value("Preferences/Advanced/NotifyTorrentAdded").toBool());
        QCOMPARE(settings.value("Preferences/Advanced/NotificationTimeout").toInt(), 3000);
        QCOMPARE(settings.value("Preferences/Advanced/SavePathHistoryLength").toInt(), 2);
        QVERIFY(!settings.value("Preferences/Advanced/AttachAddTorrentDialog").toBool());
        QVERIFY(QCoreApplication::testAttribute(Qt::AA_DontShowIconsInMenus));
        auto *speedButton = tabs->currentWidget()->findChild<QPushButton *>("remoteSpeedTabButton");
        QVERIFY(speedButton && speedButton->isHidden());
        const int before = modern.syncRequests();
        QTRY_VERIFY_WITH_TIMEOUT(modern.syncRequests() >= before + 2, 2500);
        QCoreApplication::setAttribute(Qt::AA_DontShowIconsInMenus, false);
        const QJsonObject delta{{"current_network_interface", "vpn&1"}, {"current_interface_address", "10.2.0.3"},
            {"resume_data_storage_type", "SQLite"}, {"disk_io_type", 4},
            {"resolve_peer_countries", true}, {"resolve_peer_host_names", true}};
        const QUrlQuery form(QString::fromUtf8(modern.lastCommandBody("/api/v2/app/setPreferences")));
        QCOMPARE(QJsonDocument::fromJson(form.queryItemValue("json", QUrl::FullyDecoded).toUtf8()).object(), delta);
        QCOMPARE(modern.commandsFor("/api/v2/app/setPreferences"), 1);
    }

    void advancedRemoteSettingsBulkWorkflow()
    {
        struct Case { const char *widget; const char *key; QJsonValue before; QJsonValue after; int scale; bool modern; };
        const QList<Case> cases{
            {"advanced_TORRENT_CONTENT_REMOVE_OPTION", "torrent_content_remove_option", QJsonValue("MoveToTrash"), QJsonValue("Delete"), 1, false},
            {"advanced_SAVE_RESUME_DATA_INTERVAL", "save_resume_data_interval", QJsonValue(60), QJsonValue(45), 1, false},
            {"advanced_SAVE_STATISTICS_INTERVAL", "save_statistics_interval", QJsonValue(15), QJsonValue(20), 1, false},
            {"advanced_TORRENT_FILE_SIZE_LIMIT", "torrent_file_size_limit", QJsonValue(104857600), QJsonValue(115343360), 1048576, false},
            {"advanced_RECHECK_COMPLETED", "recheck_completed_torrents", QJsonValue(false), QJsonValue(true), 1, false},
            {"advanced_REANNOUNCE_WHEN_ADDRESS_CHANGED", "reannounce_when_address_changed", QJsonValue(false), QJsonValue(true), 1, false},
            {"advanced_TRACKER_STATUS", "enable_embedded_tracker", QJsonValue(false), QJsonValue(true), 1, false},
            {"advanced_TRACKER_PORT", "embedded_tracker_port", QJsonValue(9000), QJsonValue(9001), 1, false},
            {"advanced_TRACKER_PORT_FORWARDING", "embedded_tracker_port_forwarding", QJsonValue(false), QJsonValue(true), 1, false},
            {"advanced_IGNORE_SSL_ERRORS", "ignore_ssl_errors", QJsonValue(false), QJsonValue(true), 1, false},
            {"advanced_BDECODE_DEPTH_LIMIT", "bdecode_depth_limit", QJsonValue(100), QJsonValue(110), 1, false},
            {"advanced_BDECODE_TOKEN_LIMIT", "bdecode_token_limit", QJsonValue(1000000), QJsonValue(1100000), 1, false},
            {"advanced_LOAD_TORRENT_MAX_DIRECTORY_DEPTH", "load_torrent_max_directory_depth", QJsonValue(100), QJsonValue(120), 1, true},
            {"advanced_ASYNC_IO_THREADS", "async_io_threads", QJsonValue(4), QJsonValue(8), 1, false},
            {"advanced_FILE_POOL_SIZE", "file_pool_size", QJsonValue(40), QJsonValue(50), 1, false},
            {"advanced_DISK_QUEUE_SIZE", "disk_queue_size", QJsonValue(1048576), QJsonValue(2097152), 1024, false},
            {"advanced_PIECE_EXTENT_AFFINITY", "enable_piece_extent_affinity", QJsonValue(false), QJsonValue(true), 1, false},
            {"advanced_SUGGEST_MODE", "enable_upload_suggestions", QJsonValue(false), QJsonValue(true), 1, false},
            {"advanced_SEND_BUF_WATERMARK", "send_buffer_watermark", QJsonValue(500), QJsonValue(600), 1, false},
            {"advanced_SEND_BUF_LOW_WATERMARK", "send_buffer_low_watermark", QJsonValue(10), QJsonValue(20), 1, false},
            {"advanced_SEND_BUF_WATERMARK_FACTOR", "send_buffer_watermark_factor", QJsonValue(50), QJsonValue(60), 1, false},
            {"advanced_CONNECTION_SPEED", "connection_speed", QJsonValue(30), QJsonValue(40), 1, false},
            {"advanced_SEEDING_OUTGOING_CONNECTIONS", "seeding_outgoing_connections", QJsonValue(true), QJsonValue(false), 1, true},
            {"advanced_SOCKET_SEND_BUFFER_SIZE", "socket_send_buffer_size", QJsonValue(0), QJsonValue(65536), 1024, false},
            {"advanced_SOCKET_RECEIVE_BUFFER_SIZE", "socket_receive_buffer_size", QJsonValue(0), QJsonValue(131072), 1024, false},
            {"advanced_SOCKET_BACKLOG_SIZE", "socket_backlog_size", QJsonValue(30), QJsonValue(40), 1, false},
            {"advanced_OUTGOING_PORT_MIN", "outgoing_ports_min", QJsonValue(0), QJsonValue(40000), 1, false},
            {"advanced_OUTGOING_PORT_MAX", "outgoing_ports_max", QJsonValue(0), QJsonValue(41000), 1, false},
            {"advanced_UPNP_LEASE_DURATION", "upnp_lease_duration", QJsonValue(0), QJsonValue(3600), 1, false},
            {"advanced_PEER_DSCP", "peer_tos", QJsonValue(0), QJsonValue(32), 1, false},
            {"advanced_UTP_MIX_MODE", "utp_tcp_mixed_mode", QJsonValue(0), QJsonValue(1), 1, false},
            {"advanced_HOSTNAME_CACHE_TTL", "hostname_cache_ttl", QJsonValue(60), QJsonValue(120), 1, true},
            {"advanced_IDN_SUPPORT", "idn_support_enabled", QJsonValue(true), QJsonValue(false), 1, false},
            {"advanced_MULTI_CONNECTIONS_PER_IP", "enable_multi_connections_from_same_ip", QJsonValue(false), QJsonValue(true), 1, false},
            {"advanced_MULTI_CONNECTIONS_PER_PEER_ID", "enable_multi_connections_from_same_peer_id", QJsonValue(false), QJsonValue(true), 1, true},
            {"advanced_VALIDATE_HTTPS_TRACKER_CERTIFICATE", "validate_https_tracker_certificate", QJsonValue(true), QJsonValue(false), 1, false},
            {"advanced_SSRF_MITIGATION", "ssrf_mitigation", QJsonValue(true), QJsonValue(false), 1, false},
            {"advanced_BLOCK_PEERS_ON_PRIVILEGED_PORTS", "block_peers_on_privileged_ports", QJsonValue(true), QJsonValue(false), 1, false},
            {"advanced_CHOKING_ALGORITHM", "upload_slots_behavior", QJsonValue(0), QJsonValue(1), 1, false},
            {"advanced_SEED_CHOKING_ALGORITHM", "upload_choking_algorithm", QJsonValue(0), QJsonValue(2), 1, false},
            {"advanced_ANNOUNCE_ALL_TRACKERS", "announce_to_all_trackers", QJsonValue(false), QJsonValue(true), 1, false},
            {"advanced_ANNOUNCE_ALL_TIERS", "announce_to_all_tiers", QJsonValue(false), QJsonValue(true), 1, false},
            {"advanced_MAX_CONCURRENT_HTTP_ANNOUNCES", "max_concurrent_http_announces", QJsonValue(50), QJsonValue(60), 1, false},
            {"advanced_STOP_TRACKER_TIMEOUT", "stop_tracker_timeout", QJsonValue(5), QJsonValue(10), 1, false},
            {"advanced_PEER_TURNOVER", "peer_turnover", QJsonValue(4), QJsonValue(5), 1, false},
            {"advanced_PEER_TURNOVER_CUTOFF", "peer_turnover_cutoff", QJsonValue(90), QJsonValue(85), 1, false},
            {"advanced_PEER_TURNOVER_INTERVAL", "peer_turnover_interval", QJsonValue(300), QJsonValue(360), 1, false},
            {"advanced_REQUEST_QUEUE_SIZE", "request_queue_size", QJsonValue(500), QJsonValue(600), 1, false},
            {"advanced_MAX_OUTSTANDING_BLOCK_REQUESTS", "max_outstanding_block_requests", QJsonValue(500), QJsonValue(600), 1, true},
            {"advanced_DHT_BOOTSTRAP_NODES", "dht_bootstrap_nodes", QJsonValue("router.bittorrent.com:6881"), QJsonValue("router.utorrent.com:6881"), 1, false},
            {"advanced_WEBTORRENT_STUN_SERVER", "webtorrent_stun_server", QJsonValue("stun.example.test:3478"), QJsonValue("stun2.example.test:3478"), 1, true}
        };
        DemoQbittorrentServer legacy(true), modern(true);
        modern.enableModernResponses();
        QVERIFY(legacy.start() && modern.start());
        QJsonObject first, second;
        for (const auto &c : cases)
        {
            if (!c.modern) first.insert(c.key, c.before);
            second.insert(c.key, c.before);
        }
        // STUN is only supported by libtorrent 2.1 or later.
        modern.setBuildInfo({{"libtorrent", "2.1.0.0"}, {"platform", "linux"}, {"bitness", 64}});
        // Deliberately malformed value cannot be represented by a KiB editor.
        first.insert("disk_queue_size", 1025);
        legacy.setServerPreferences(first);
        modern.setServerPreferences(second);
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath("profiles.ini"), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        openConnectDialog(window, legacy.url());
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains("v5.1.4"), 6000);
        openConnectDialog(window, modern.url(), false, true, true, "Modern");
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains("v5.3.0"), 6000);
        auto *tabs = window.findChild<QTabWidget *>("serverTabs");
        auto *prefs = window.findChild<QAction *>("actionOptions");
        QVERIFY(tabs && prefs);
        const auto exercise = [&](bool newer, bool save, bool ignored) {
            tabs->setCurrentIndex(newer ? 1 : 0);
            bool finished = false;
            QTimer::singleShot(0, &window, [&] {
                auto *dlg = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget());
                QVERIFY(dlg);
                const auto cleanup = qScopeGuard([dlg] { if (dlg->isVisible()) dlg->reject(); });
                auto *nav = dlg->findChild<QListWidget *>("preferencesSections");
                auto *apply = dlg->findChild<QPushButton *>("preferencesApply");
                auto *flag = dlg->findChild<QCheckBox *>("advanced_TRACKER_STATUS");
                auto *port = dlg->findChild<QSpinBox *>("advanced_TRACKER_PORT");
                QVERIFY(nav && apply && flag && port);
                nav->setCurrentRow(9);
                QTRY_VERIFY_WITH_TIMEOUT(flag->isEnabled(), 6000);
                QVERIFY(!port->isEnabled() && !flag->isChecked());
                QVERIFY(!apply->isEnabled());
                for (const auto &c : cases)
                {
                    auto *widget = dlg->findChild<QWidget *>(QString::fromLatin1(c.widget));
                    QVERIFY2(widget, c.widget);
                    if (!newer && (c.modern || QByteArray(c.key) == "disk_queue_size"))
                    {
                        QVERIFY2(!widget->isEnabled(), c.widget);
                        continue;
                    }
                    QTRY_VERIFY_WITH_TIMEOUT(widget->isEnabled(), 6000);
                    if (auto *box = qobject_cast<QCheckBox *>(widget))
                    {
                        QVERIFY(!box->isTristate());
                        QCOMPARE(box->isChecked(), c.before.toBool());
                        box->setChecked(c.after.toBool());
                    }
                    else if (auto *spin = qobject_cast<QSpinBox *>(widget))
                    {
                        QCOMPARE(double(spin->value()) * c.scale, c.before.toDouble());
                        spin->setValue(int(c.after.toDouble() / c.scale));
                    }
                    else if (auto *combo = qobject_cast<QComboBox *>(widget))
                    {
                        if (c.before.isString())
                        {
                            QCOMPARE(combo->currentIndex(), 1);
                            combo->setCurrentIndex(0);
                        }
                        else
                        {
                            QCOMPARE(combo->currentIndex(), c.before.toInt());
                            combo->setCurrentIndex(c.after.toInt());
                        }
                    }
                    else if (auto *line = qobject_cast<QLineEdit *>(widget))
                    {
                        QCOMPARE(line->text(), c.before.toString());
                        line->setText(c.after.toString());
                    }
                    else QFAIL("Unknown Advanced editor");
                }
                QVERIFY(apply->isEnabled());
                bool confirmed = false, failure = false;
                QString unexpected;
                QTimer driver;
                driver.setInterval(20);
                connect(&driver, &QTimer::timeout, dlg, [&] {
                    if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                    {
                        if (box->windowTitle() == QStringLiteral("Change Advanced server settings"))
                        {
                            confirmed = true;
                            box->button(QMessageBox::Yes)->click();
                        }
                        else if (ignored && box->windowTitle() == QStringLiteral("Cannot confirm server preferences"))
                        {
                            failure = true;
                            box->accept();
                        }
                        else { unexpected = box->windowTitle(); box->reject(); }
                    }
                });
                driver.start();
                if (save)
                {
                    apply->click();
                    QTRY_VERIFY_WITH_TIMEOUT(nav->isEnabled() && (ignored ? failure : !apply->isEnabled()), 6000);
                    QCOMPARE(failure, ignored);
                    QVERIFY(confirmed);
                }
                driver.stop();
                QVERIFY2(unexpected.isEmpty(), qPrintable(unexpected));
                finished = true;
            });
            prefs->trigger();
            QVERIFY(finished);
        };
        exercise(false, false, false);
        QCOMPARE(legacy.serverPreferences(), first);
        QCOMPARE(legacy.commandsFor("/api/v2/app/setPreferences"), 0);
        legacy.ignorePreferenceWrites(true);
        exercise(false, true, true);
        QCOMPARE(legacy.serverPreferences(), first);
        QCOMPARE(modern.serverPreferences(), second);
        exercise(true, true, false);
        QJsonObject delta;
        for (const auto &c : cases) delta.insert(c.key, c.after);
        const QUrlQuery form(QString::fromUtf8(modern.lastCommandBody("/api/v2/app/setPreferences")));
        QCOMPARE(QJsonDocument::fromJson(form.queryItemValue("json", QUrl::FullyDecoded).toUtf8()).object(), delta);
        QCOMPARE(modern.serverPreferences(), delta);
        QCOMPARE(legacy.serverPreferences(), first);
        QCOMPARE(modern.commandsFor("/api/v2/app/setPreferences"), 1);
        QCOMPARE(legacy.commandsFor("/api/v2/app/setPreferences"), 1);
    }

    void webUIDynamicDNSAndAPIKeyManagement()
    {
        DemoQbittorrentServer legacy(true), modern(true);
        modern.enableModernResponses();
        QVERIFY(legacy.start() && modern.start());
        const QJsonObject initial{
            {"dyndns_enabled", false}, {"dyndns_service", 0}, {"dyndns_domain", "old.example.test"},
            {"dyndns_username", "old-user"}, {"dyndns_password", "old-password"}
        };
        QJsonObject other = initial;
        other.insert("web_ui_api_key", QString());
        legacy.setServerPreferences(initial);
        modern.setServerPreferences(other);
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath("profiles.ini"), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        openConnectDialog(window, legacy.url());
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains("v5.1.4"), 6000);
        openConnectDialog(window, modern.url(), false, true, true, "Modern");
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains("v5.3.0"), 6000);
        auto *tabs = window.findChild<QTabWidget *>("serverTabs");
        auto *prefs = window.findChild<QAction *>("actionOptions");
        QVERIFY(tabs && prefs);
        const auto exercise = [&](bool modernServer, bool applyDNS) {
            tabs->setCurrentIndex(modernServer ? 1 : 0);
            bool finished = false;
            QTimer::singleShot(0, &window, [&] {
                auto *dlg = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget());
                QVERIFY(dlg);
                const auto cleanup = qScopeGuard([dlg] { if (dlg->isVisible()) dlg->reject(); });
                auto *nav = dlg->findChild<QListWidget *>("preferencesSections");
                auto *dns = dlg->findChild<QGroupBox *>("checkDynDNS");
                auto *service = dlg->findChild<QComboBox *>("comboDNSService");
                auto *domain = dlg->findChild<QLineEdit *>("domainNameTxt");
                auto *user = dlg->findChild<QLineEdit *>("DNSUsernameTxt");
                auto *password = dlg->findChild<QLineEdit *>("DNSPasswordTxt");
                auto *registration = dlg->findChild<QPushButton *>("registerDNSBtn");
                auto *group = dlg->findChild<QGroupBox *>("groupWebUIAPIKey");
                auto *key = dlg->findChild<QLineEdit *>("textWebUIAPIKey");
                auto *copy = dlg->findChild<QPushButton *>("btnWebUIAPIKeyCopy");
                auto *rotate = dlg->findChild<QPushButton *>("btnWebUIAPIKeyRotate");
                auto *remove = dlg->findChild<QPushButton *>("btnWebUIAPIKeyDelete");
                auto *apply = dlg->findChild<QPushButton *>("preferencesApply");
                QVERIFY(nav && dns && service && domain && user && password && registration && group && key && copy && rotate && remove && apply);
                nav->setCurrentRow(8);
                QTRY_VERIFY_WITH_TIMEOUT(dns->isEnabled(), 6000);
                QCOMPARE(group->isVisible(), modernServer);
                QVERIFY(!dns->isChecked() && !domain->isEnabled() && !registration->isEnabled());
                QCOMPARE(password->echoMode(), QLineEdit::Password);
                dns->setChecked(true);
                QVERIFY(domain->isEnabled() && registration->isEnabled() && password->isEnabled());
                service->setCurrentIndex(1);
                domain->setText("new.example.test");
                user->setText("new-user");
                password->setText("dns+&%password");
                QVERIFY(apply->isEnabled());
                bool allow = true;
                QString expectedTitle, unexpected;
                QTimer modalDriver;
                modalDriver.setInterval(20);
                connect(&modalDriver, &QTimer::timeout, dlg, [&] {
                    if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                    {
                        if (box->windowTitle() == expectedTitle && box->button(QMessageBox::Yes))
                            box->button(allow ? QMessageBox::Yes : QMessageBox::No)->click();
                        else if (box->windowTitle() == expectedTitle)
                            box->accept();
                        else
                        {
                            unexpected = box->windowTitle();
                            box->reject();
                        }
                    }
                });
                modalDriver.start();
                const auto stop = qScopeGuard([&] { modalDriver.stop(); });
                if (modernServer)
                {
                    QVERIFY(key->text().isEmpty() && !copy->isEnabled() && !remove->isEnabled() && rotate->isEnabled());
                    expectedTitle = "Generate API key";
                    allow = false;
                    rotate->click();
                    QCOMPARE(modern.commandsFor("/api/v2/app/rotateAPIKey"), 0);
                    allow = true;
                    rotate->click();
                    QTRY_COMPARE_WITH_TIMEOUT(key->text(), QStringLiteral("test-key-1"), 6000);
                    QTRY_VERIFY(nav->isEnabled());
                    QVERIFY(copy->isEnabled() && remove->isEnabled() && key->isReadOnly());
                    QCOMPARE(key->echoMode(), QLineEdit::Password);
                    copy->click();
                    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("test-key-1"));
                    QApplication::clipboard()->clear();
                    QVERIFY(apply->isEnabled()); // staged DNS edits survive immediate key action
                    expectedTitle = "Rotate API key";
                    rotate->click();
                    QTRY_COMPARE_WITH_TIMEOUT(key->text(), QStringLiteral("test-key-2"), 6000);
                    QTRY_VERIFY(nav->isEnabled());
                    expectedTitle = "Delete API key";
                    remove->click();
                    QTRY_VERIFY_WITH_TIMEOUT(key->text().isEmpty() && nav->isEnabled(), 6000);
                    QVERIFY(!copy->isEnabled() && !remove->isEnabled() && rotate->isEnabled());
                    QCOMPARE(modern.commandsFor("/api/v2/app/rotateAPIKey"), 2);
                    QCOMPARE(modern.commandsFor("/api/v2/app/deleteAPIKey"), 1);
                    QCOMPARE(modern.commandsFor("/api/v2/app/setPreferences"), 0);
                    QCOMPARE(domain->text(), QStringLiteral("new.example.test"));
                }
                if (applyDNS)
                {
                    expectedTitle = "Change Dynamic DNS settings";
                    apply->click();
                    QTRY_VERIFY_WITH_TIMEOUT(nav->isEnabled() && !apply->isEnabled(), 6000);
                }
                QVERIFY2(unexpected.isEmpty(), qPrintable(unexpected));
                finished = true;
            });
            prefs->trigger();
            QVERIFY(finished);
        };
        exercise(false, false);
        QCOMPARE(legacy.serverPreferences(), initial);
        QCOMPARE(legacy.commandsFor("/api/v2/app/setPreferences"), 0);
        exercise(true, true);
        const QJsonObject delta{{"dyndns_enabled", true}, {"dyndns_service", 1}, {"dyndns_domain", "new.example.test"},
            {"dyndns_username", "new-user"}, {"dyndns_password", "dns+&%password"}};
        const QUrlQuery form(QString::fromUtf8(modern.lastCommandBody("/api/v2/app/setPreferences")));
        QCOMPARE(QJsonDocument::fromJson(form.queryItemValue("json", QUrl::FullyDecoded).toUtf8()).object(), delta);
        for (auto it = delta.constBegin(); it != delta.constEnd(); ++it) other.insert(it.key(), it.value());
        QCOMPARE(modern.serverPreferences(), other);
        QCOMPARE(legacy.serverPreferences(), initial);
        QCOMPARE(legacy.commandsFor("/api/v2/app/rotateAPIKey"), 0);
        settings.sync();
        QFile file(settings.fileName());
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto persisted = file.readAll();
        QVERIFY(!persisted.contains("dns+&%password") && !persisted.contains("test-key-"));
    }

    void webUISecurityPolicyTransitions()
    {
        DemoQbittorrentServer legacy(true), modern(true);
        modern.enableModernResponses();
        QVERIFY(legacy.start() && modern.start());
        const QJsonObject initial{
            {"web_ui_address", "*"}, {"use_https", false},
            {"web_ui_https_cert_path", ""}, {"web_ui_https_key_path", ""},
            {"web_ui_username", "demo"}, {"bypass_local_auth", false},
            {"bypass_auth_subnet_whitelist_enabled", false},
            {"web_ui_secure_cookie_enabled", false},
            {"web_ui_host_header_validation_enabled", true}, {"web_ui_domain_list", "*"},
            {"web_ui_use_custom_http_headers_enabled", false}, {"web_ui_custom_http_headers", ""},
            {"web_ui_reverse_proxy_enabled", false}, {"web_ui_reverse_proxies_list", ""}
        };
        QJsonObject older = initial, newer = initial;
        older.insert("web_ui_port", QUrl(legacy.url()).port());
        newer.insert("web_ui_port", QUrl(modern.url()).port());
        legacy.setServerPreferences(older);
        modern.setServerPreferences(newer);
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath("profiles.ini"), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        openConnectDialog(window, legacy.url(), true);
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains("v5.1.4"), 6000);
        openConnectDialog(window, modern.url(), true, true, true, "Modern");
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains("v5.3.0"), 6000);
        auto *tabs = window.findChild<QTabWidget *>("serverTabs");
        auto *prefs = window.findChild<QAction *>("actionOptions");
        QVERIFY(tabs && prefs);
        const auto profiles = Remote::ProfileStore(settings).profiles();
        QCOMPARE(profiles.size(), 2);
        for (const auto &profile : profiles)
            QTRY_COMPARE_WITH_TIMEOUT(vault.data.value(vault.keyFor(profile)), QStringLiteral("demo-password"), 6000);
        const auto exercise = [&](bool modernServer, bool save, bool fail) {
            tabs->setCurrentIndex(modernServer ? 1 : 0);
            bool edited = false, confirmed = false, entered = false, failure = false;
            QString unexpected;
            QTimer modalDriver;
            modalDriver.setInterval(20);
            connect(&modalDriver, &QTimer::timeout, &window, [&] {
                if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                {
                    if (box->windowTitle() == QStringLiteral("Change server connection"))
                    {
                        confirmed = true;
                        box->button(QMessageBox::Yes)->click();
                    }
                    else if (fail && box->windowTitle() == QStringLiteral("Cannot verify connection change"))
                    {
                        failure = true;
                        box->accept();
                    }
                    else
                    {
                        unexpected = box->windowTitle() + QStringLiteral(": ") + box->text();
                        box->reject();
                    }
                }
                else if (auto *input = qobject_cast<QInputDialog *>(QApplication::activeModalWidget()))
                {
                    if (input->windowTitle() == QStringLiteral("Verify server login"))
                    {
                        entered = true;
                        input->setTextValue(QStringLiteral("demo-password"));
                        input->accept();
                    }
                }
            });
            QTimer watchdog;
            watchdog.setSingleShot(true);
            connect(&watchdog, &QTimer::timeout, &window, [&] {
                if (auto *dialog = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget()))
                    dialog->reject();
            });
            QTimer::singleShot(0, &window, [&] {
                auto *dlg = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget());
                QVERIFY(dlg);
                auto *nav = dlg->findChild<QListWidget *>("preferencesSections");
                auto *secure = dlg->findChild<QCheckBox *>("checkSecureCookie");
                auto *host = dlg->findChild<QGroupBox *>("groupHostHeaderValidation");
                auto *domains = dlg->findChild<QLineEdit *>("textServerDomains");
                auto *headers = dlg->findChild<QGroupBox *>("groupWebUIAddCustomHTTPHeaders");
                auto *text = dlg->findChild<QPlainTextEdit *>("textWebUICustomHTTPHeaders");
                auto *proxy = dlg->findChild<QGroupBox *>("groupEnableReverseProxySupport");
                auto *trusted = dlg->findChild<QLineEdit *>("textTrustedReverseProxiesList");
                auto *link = dlg->findChild<QLabel *>("labelReverseProxyLink");
                auto *apply = dlg->findChild<QPushButton *>("preferencesApply");
                QVERIFY(nav && secure && host && domains && headers && text && proxy && trusted && link && apply);
                nav->setCurrentRow(8);
                QTRY_VERIFY_WITH_TIMEOUT(secure->isEnabled() && host->isEnabled() && proxy->isEnabled(), 6000);
                QVERIFY(!text->isEnabled() && !trusted->isEnabled() && link->isEnabled());
                QVERIFY(domains->isEnabled());
                host->setChecked(false);
                QVERIFY(!domains->isEnabled());
                host->setChecked(true);
                domains->setText("localhost;*.example.test");
                // Change the host flag while preserving the staged domain list.
                host->setChecked(false);
                secure->setChecked(true);
                headers->setChecked(true);
                proxy->setChecked(true);
                QVERIFY(text->isEnabled() && trusted->isEnabled() && link->isEnabled());
                text->setPlainText("X-Test: yes:two");
                trusted->setText("127.0.0.1;192.0.2.0/24");
                QVERIFY(apply->isEnabled());
                edited = true;
                if (save) apply->click();
                else dlg->reject();
            });
            modalDriver.start();
            watchdog.start(25000);
            prefs->trigger();
            watchdog.stop();
            modalDriver.stop();
            QVERIFY2(unexpected.isEmpty(), qPrintable(unexpected));
            QVERIFY(edited);
            QCOMPARE(confirmed, save);
            QCOMPARE(entered, save);
            QCOMPARE(failure, fail);
            if (save && !fail)
                QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(modernServer ? "v5.3.0" : "v5.1.4"), 12000);
        };
        exercise(false, false, false);
        QCOMPARE(legacy.serverPreferences(), older);
        exercise(true, true, false);
        QJsonObject delta{
            {"web_ui_secure_cookie_enabled", true},
            {"web_ui_host_header_validation_enabled", false}, {"web_ui_domain_list", "localhost;*.example.test"},
            {"web_ui_use_custom_http_headers_enabled", true}, {"web_ui_custom_http_headers", "X-Test: yes:two"},
            {"web_ui_reverse_proxy_enabled", true}, {"web_ui_reverse_proxies_list", "127.0.0.1;192.0.2.0/24"}
        };
        const QUrlQuery form(QString::fromUtf8(modern.lastCommandBody("/api/v2/app/setPreferences")));
        QCOMPARE(QJsonDocument::fromJson(form.queryItemValue("json", QUrl::FullyDecoded).toUtf8()).object(), delta);
        for (auto it = delta.constBegin(); it != delta.constEnd(); ++it) newer.insert(it.key(), it.value());
        QCOMPARE(modern.serverPreferences(), newer);
        QCOMPARE(legacy.serverPreferences(), older);
        legacy.ignorePreferenceWrites(true);
        exercise(false, true, true);
        QCOMPARE(legacy.serverPreferences(), older);
        QCOMPARE(modern.serverPreferences(), newer);
        QCOMPARE(legacy.commandsFor("/api/v2/app/setPreferences"), 1);
        QCOMPARE(modern.commandsFor("/api/v2/app/setPreferences"), 1);
        const auto after = Remote::ProfileStore(settings).profiles();
        QCOMPARE(after.size(), profiles.size());
        for (const auto &profile : profiles)
        {
            bool found = false;
            for (const auto &saved : after)
                if (saved.id == profile.id)
                {
                    found = true;
                    QCOMPARE(saved.url, profile.url);
                    QCOMPARE(saved.username, profile.username);
                }
            QVERIFY(found);
            QCOMPARE(vault.data.value(vault.keyFor(profile)), QStringLiteral("demo-password"));
        }
    }

    void webUIPreferencesFollowSelectedServer()
    {
        DemoQbittorrentServer legacy(true), modern(true);
        modern.enableModernResponses();
        const QJsonObject initial{
            {"web_ui_address", "*"}, {"web_ui_port", 8080}, {"use_https", false},
            {"web_ui_https_cert_path", ""}, {"web_ui_https_key_path", ""},
            {"web_ui_username", "legacy-user"}, {"web_ui_upnp", false},
            {"bypass_local_auth", false}, {"bypass_auth_subnet_whitelist_enabled", true},
            {"bypass_auth_subnet_whitelist", "192.0.2.0/24"},
            {"web_ui_max_auth_fail_count", 5}, {"web_ui_ban_duration", 3600},
            {"web_ui_session_timeout", 1800}, {"alternative_webui_enabled", false},
            {"alternative_webui_path", "/share/web"}, {"web_ui_clickjacking_protection_enabled", true},
            {"web_ui_csrf_protection_enabled", true}, {"web_ui_secure_cookie_enabled", true},
            {"web_ui_host_header_validation_enabled", true}, {"web_ui_domain_list", "example.test"},
            {"web_ui_use_custom_http_headers_enabled", false}, {"web_ui_custom_http_headers", ""},
            {"web_ui_reverse_proxy_enabled", false}, {"web_ui_reverse_proxies_list", ""},
            {"dyndns_enabled", false}, {"dyndns_service", 0}, {"dyndns_domain", "example.test"},
            {"dyndns_username", "dns-user"}, {"dyndns_password", "dns-secret"}
        };
        QJsonObject other = initial;
        other.insert("web_ui_port", 9000);
        other.insert("web_ui_username", "modern-user");
        other.insert("web_ui_sessions_count_limit", 10);
        other.insert("web_ui_api_key", "secret-test-key");
        legacy.setServerPreferences(initial);
        modern.setServerPreferences(other);
        QVERIFY(legacy.start() && modern.start());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath("prefs.ini"), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        openConnectDialog(window, legacy.url());
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains("v5.1.4"), 6000);
        openConnectDialog(window, modern.url(), false, true, true, "Modern seedbox");
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains("v5.3.0"), 6000);
        auto *tabs = window.findChild<QTabWidget *>("serverTabs");
        auto *prefs = window.findChild<QAction *>("actionOptions");
        QVERIFY(tabs && prefs);
        const auto exercise = [&](bool modernServer, bool save) {
            bool finished = false;
            QTimer::singleShot(0, &window, [&] {
                auto *dlg = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget());
                QVERIFY(dlg);
                const auto cleanup = qScopeGuard([dlg] { if (dlg->isVisible()) dlg->reject(); });
                auto *nav = dlg->findChild<QListWidget *>("preferencesSections");
                QVERIFY(nav);
                nav->setCurrentRow(8);
                auto *group = dlg->findChild<QGroupBox *>("checkWebUI");
                auto *port = dlg->findChild<QSpinBox *>("spinWebUIPort");
                auto *user = dlg->findChild<QLineEdit *>("textWebUIUsername");
                auto *password = dlg->findChild<QLineEdit *>("textWebUIPassword");
                auto *key = dlg->findChild<QLineEdit *>("textWebUIAPIKey");
                auto *upnp = dlg->findChild<QCheckBox *>("checkWebUIUPnP");
                auto *counter = dlg->findChild<QSpinBox *>("spinBanCounter");
                auto *duration = dlg->findChild<QSpinBox *>("spinBanDuration");
                auto *sessions = dlg->findChild<QSpinBox *>("spinBoxWebUISessionsCountLimit");
                auto *apply = dlg->findChild<QPushButton *>("preferencesApply");
                QVERIFY(group && port && user && password && key && upnp && counter && duration && sessions && apply);
                QTRY_VERIFY_WITH_TIMEOUT(upnp->isEnabled(), 6000);
                QVERIFY(!group->isCheckable());
                QVERIFY(group->isEnabled());
                QCOMPARE(port->value(), modernServer ? 9000 : 8080);
                QCOMPARE(user->text(), modernServer ? QStringLiteral("modern-user") : QStringLiteral("legacy-user"));
                QVERIFY(!port->isEnabled() && !user->isEnabled() && !password->isEnabled());
                QVERIFY(password->text().isEmpty());
                QCOMPARE(sessions->isVisible(), modernServer);
                QCOMPARE(key->isVisible(), modernServer);
                auto *https = dlg->findChild<QGroupBox *>("checkWebUIHttps");
                auto *certificate = dlg->findChild<QLineEdit *>("textWebUIHttpsCert");
                auto *keyPath = dlg->findChild<QLineEdit *>("textWebUIHttpsKey");
                auto *certificateLink = dlg->findChild<QLabel *>("lblWebUICertInfo");
                QVERIFY(https && certificate && keyPath && certificateLink);
                QVERIFY(!https->isChecked() && !certificate->isEnabled() && !keyPath->isEnabled());
                QVERIFY(certificateLink->isEnabled() && certificateLink->openExternalLinks());
                QVERIFY(certificateLink->textInteractionFlags().testFlag(Qt::LinksAccessibleByMouse));
                QVERIFY(certificateLink->textInteractionFlags().testFlag(Qt::LinksAccessibleByKeyboard));
                auto *alternative = dlg->findChild<QGroupBox *>("groupAltWebUI");
                auto *alternativePath = dlg->findChild<QLineEdit *>("textWebUIRootFolder");
                auto *alternativeLink = dlg->findChild<QLabel *>("lblAltWebUIList");
                QVERIFY(alternative && alternativePath && alternativeLink);
                QVERIFY(!alternative->isChecked() && !alternativePath->isEnabled());
                QVERIFY(alternativeLink->isEnabled() && alternativeLink->openExternalLinks());
                QVERIFY(alternativeLink->toolTip().contains("qBittorrent Remote"));
                QVERIFY(alternative->toolTip().contains("qBittorrent Remote"));
                alternative->setChecked(true);
                QVERIFY(alternativePath->isEnabled() && alternativeLink->isEnabled());
                alternative->setChecked(false);
                QVERIFY(!alternativePath->isEnabled() && alternativeLink->isEnabled());
                if (modernServer)
                {
                    QCOMPARE(sessions->value(), 10);
                    QVERIFY(sessions->isEnabled());
                    QCOMPARE(key->echoMode(), QLineEdit::Password);
                    QCOMPARE(key->text(), QStringLiteral("secret-test-key"));
                }
                QVERIFY(!apply->isEnabled());
                counter->setValue(0);
                QVERIFY(!duration->isEnabled());
                counter->setValue(5);
                QVERIFY(duration->isEnabled());
                QVERIFY(!apply->isEnabled());
                upnp->setChecked(true);
                QVERIFY(apply->isEnabled());
                if (save)
                {
                    bool confirmed = false;
                    QString unexpectedDialog;
                    QTimer modalDriver;
                    modalDriver.setInterval(20);
                    connect(&modalDriver, &QTimer::timeout, dlg, [&] {
                        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                        if (!box) return;
                        if (box->windowTitle() == QStringLiteral("Change server WebUI settings")
                            && box->button(QMessageBox::Yes))
                        {
                            confirmed = true;
                            box->button(QMessageBox::Yes)->click();
                        }
                        else
                        {
                            unexpectedDialog = box->windowTitle() + QStringLiteral(": ") + box->text();
                            box->reject();
                        }
                    });
                    modalDriver.start();
                    apply->click();
                    // Apply is disabled as soon as the asynchronous write starts.
                    // Wait for the page to be unlocked after readback, otherwise
                    // the scope guard's reject() is ignored while remoteBusy.
                    QTRY_VERIFY_WITH_TIMEOUT(nav->isEnabled(), 6000);
                    modalDriver.stop();
                    QVERIFY2(unexpectedDialog.isEmpty(), qPrintable(unexpectedDialog));
                    QVERIFY(confirmed);
                    QVERIFY(!apply->isEnabled());
                    QCOMPARE(modern.commandsFor("/api/v2/app/setPreferences"), 1);
                }
                finished = true;
            });
            prefs->trigger();
            QVERIFY(finished);
        };
        tabs->setCurrentIndex(0);
        exercise(false, false);
        QCOMPARE(legacy.serverPreferences(), initial);
        QCOMPARE(legacy.commandsFor("/api/v2/app/setPreferences"), 0);
        tabs->setCurrentIndex(1);
        exercise(true, true);
        auto expected = other;
        expected.insert("web_ui_upnp", true);
        QCOMPARE(modern.serverPreferences(), expected);
        QCOMPARE(legacy.serverPreferences(), initial);
        QCOMPARE(modern.commandsFor("/api/v2/app/setPreferences"), 1);
        const QUrlQuery form(QString::fromUtf8(modern.lastCommandBody("/api/v2/app/setPreferences")));
        const QJsonObject delta = QJsonDocument::fromJson(form.queryItemValue("json", QUrl::FullyDecoded).toUtf8()).object();
        QCOMPARE(delta, (QJsonObject{{"web_ui_upnp", true}}));
        settings.sync();
        QFile file(settings.fileName());
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto bytes = file.readAll();
        QVERIFY(!bytes.contains("dns-secret") && !bytes.contains("secret-test-key"));
    }

    void rssPreferencesFollowSelectedServer()
    {
        DemoQbittorrentServer legacy(true), modern(true);
        modern.enableModernResponses();
        const QString patterns = QStringLiteral("s(\\\\d+)e(\\\\d+)\n(\\\\d+)x(\\\\d+)");
        const QString revised = patterns + QStringLiteral("\nEP(\\\\d+)");
        const QJsonObject initialLegacy{
            {"rss_processing_enabled", false}, {"rss_refresh_interval", 5},
            {"rss_fetch_delay", 2}, {"rss_max_articles_per_feed", 100},
            {"rss_auto_downloading_enabled", false},
            {"rss_download_repack_proper_episodes", true},
            {"rss_smart_episode_filters", patterns}
        };
        const QJsonObject initialModern{
            {"rss_processing_enabled", true}, {"rss_refresh_interval", 30},
            {"rss_fetch_delay", 0}, {"rss_max_articles_per_feed", 500},
            {"rss_auto_downloading_enabled", true},
            {"rss_download_repack_proper_episodes", false},
            {"rss_smart_episode_filters", QStringLiteral("other")}
        };
        legacy.setServerPreferences(initialLegacy);
        modern.setServerPreferences(initialModern);
        QVERIFY(legacy.start() && modern.start());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("prefs.ini")), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        openConnectDialog(window, legacy.url());
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(QStringLiteral("v5.1.4")), 6000);
        openConnectDialog(window, modern.url(), false, true, true, QStringLiteral("Modern seedbox"));
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(QStringLiteral("v5.3.0")), 6000);
        auto *tabs = window.findChild<QTabWidget *>("serverTabs");
        auto *prefs = window.findChild<QAction *>("actionOptions");
        QVERIFY(tabs && prefs);

        struct Controls {
            QCheckBox *reader, *autoDownload, *repacks, *images;
            QSpinBox *interval, *delay, *articles;
            QPlainTextEdit *filters;
            QPushButton *rules, *apply;
        };
        const auto withRSS = [&](auto exercise) {
            bool finished = false;
            QTimer::singleShot(0, &window, [&]() {
                auto *dlg = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget());
                QVERIFY(dlg);
                const auto cleanup = qScopeGuard([dlg]() { if (dlg->isVisible()) dlg->reject(); });
                auto *sections = dlg->findChild<QListWidget *>("preferencesSections");
                const Controls c{
                    dlg->findChild<QCheckBox *>("checkRSSEnable"),
                    dlg->findChild<QCheckBox *>("checkRSSAutoDownloaderEnable"),
                    dlg->findChild<QCheckBox *>("checkSmartFilterDownloadRepacks"),
                    dlg->findChild<QCheckBox *>("checkRSSLoadExternalImages"),
                    dlg->findChild<QSpinBox *>("spinRSSRefreshInterval"),
                    dlg->findChild<QSpinBox *>("spinRSSFetchDelay"),
                    dlg->findChild<QSpinBox *>("spinRSSMaxArticlesPerFeed"),
                    dlg->findChild<QPlainTextEdit *>("textSmartEpisodeFilters"),
                    dlg->findChild<QPushButton *>("btnEditRules"),
                    dlg->findChild<QPushButton *>("preferencesApply")
                };
                QVERIFY(sections && c.reader && c.autoDownload && c.repacks && c.images && c.interval
                    && c.delay && c.articles && c.filters && c.rules && c.apply);
                sections->setCurrentRow(7);
                QTRY_VERIFY_WITH_TIMEOUT(c.reader->isEnabled(), 6000);
                QVERIFY(c.rules->isEnabled());
                QVERIFY(!c.apply->isEnabled());
                exercise(c);
                finished = true;
            });
            prefs->trigger();
            QVERIFY(finished);
        };
        const auto delta = [](DemoQbittorrentServer &server) {
            const QUrlQuery form(QString::fromUtf8(server.lastCommandBody("/api/v2/app/setPreferences")));
            return QJsonDocument::fromJson(form.queryItemValue(
                QStringLiteral("json"), QUrl::FullyDecoded).toUtf8()).object();
        };

        tabs->setCurrentIndex(0);
        withRSS([&](const Controls &c) {
            QVERIFY(c.images->isEnabled() && !c.images->isChecked());
            c.images->setChecked(true);
            QVERIFY(c.apply->isEnabled());
            c.images->setChecked(false);
            QVERIFY(!c.apply->isEnabled());
            c.images->setChecked(true);
        });
        QVERIFY(!settings.value(QStringLiteral("Preferences/RSS/LoadExternalImages"), false).toBool());
        withRSS([&](const Controls &c) {
            c.images->setChecked(true);
            c.apply->click();
            QTRY_VERIFY_WITH_TIMEOUT(!c.apply->isEnabled(), 6000);
        });
        QVERIFY(settings.value(QStringLiteral("Preferences/RSS/LoadExternalImages")).toBool());
        QCOMPARE(legacy.commandsFor("/api/v2/app/setPreferences"), 0);
        tabs->setCurrentIndex(1);
        withRSS([&](const Controls &c) {
            QVERIFY(c.images->isChecked());
            c.images->setChecked(false);
            c.apply->click();
            QTRY_VERIFY_WITH_TIMEOUT(!c.apply->isEnabled(), 6000);
        });
        QVERIFY(!settings.value(QStringLiteral("Preferences/RSS/LoadExternalImages")).toBool());
        QCOMPARE(modern.commandsFor("/api/v2/app/setPreferences"), 0);
        tabs->setCurrentIndex(0);
        for (bool save : {false, true})
        {
            withRSS([&](const Controls &c) {
                QVERIFY(!c.reader->isChecked() && !c.autoDownload->isChecked());
                QVERIFY(c.interval->isEnabled() && c.delay->isEnabled() && c.articles->isEnabled());
                QVERIFY(c.filters->isEnabled() && c.repacks->isEnabled());
                QCOMPARE(c.interval->value(), 5);
                QCOMPARE(c.delay->value(), 2);
                QCOMPARE(c.articles->value(), 100);
                QCOMPARE(c.filters->toPlainText(), patterns);
                QVERIFY(c.repacks->isChecked());
                for (auto *spin : {c.interval, c.delay, c.articles})
                    QVERIFY(spin->findChild<QLineEdit *>()->isEnabled());
                c.reader->setChecked(true);
                c.reader->setChecked(false);
                QVERIFY(!c.apply->isEnabled());
                c.reader->setChecked(true);
                c.autoDownload->setChecked(true);
                c.interval->setValue(17);
                c.delay->setValue(0);
                c.articles->setValue(0);
                c.repacks->setChecked(false);
                c.filters->setPlainText(revised);
                QVERIFY(c.apply->isEnabled());
                QCOMPARE(legacy.commandsFor("/api/v2/app/setPreferences"), 0);
                if (save)
                {
                    const auto answer = [&](QMessageBox::StandardButton response) {
                        QTimer::singleShot(0, &window, [response]() {
                            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                                box->button(response)->click();
                        });
                        c.apply->click();
                    };
                    answer(QMessageBox::No);
                    QCOMPARE(legacy.serverPreferences(), initialLegacy);
                    QVERIFY(c.apply->isEnabled());
                    answer(QMessageBox::Yes);
                    QTRY_VERIFY_WITH_TIMEOUT(c.reader->isEnabled() && !c.apply->isEnabled(), 6000);
                }
            });
            if (!save) QCOMPARE(legacy.serverPreferences(), initialLegacy);
        }
        const QJsonObject expectedLegacy{
            {"rss_processing_enabled", true}, {"rss_refresh_interval", 17},
            {"rss_fetch_delay", 0}, {"rss_max_articles_per_feed", 0},
            {"rss_auto_downloading_enabled", true},
            {"rss_download_repack_proper_episodes", false},
            {"rss_smart_episode_filters", revised}
        };
        QCOMPARE(delta(legacy), expectedLegacy);
        QCOMPARE(legacy.serverPreferences(), expectedLegacy);
        QCOMPARE(modern.serverPreferences(), initialModern);
        withRSS([&](const Controls &c) {
            QVERIFY(c.reader->isChecked() && c.autoDownload->isChecked());
            QCOMPARE(c.interval->value(), 17);
            QCOMPARE(c.delay->value(), 0);
            QCOMPARE(c.articles->value(), 0);
            QCOMPARE(c.filters->toPlainText(), revised);
        });

        tabs->setCurrentIndex(1);
        withRSS([](const Controls &c) {
            QVERIFY(c.reader->isChecked() && c.autoDownload->isChecked());
            QCOMPARE(c.interval->value(), 30);
            QCOMPARE(c.articles->value(), 500);
            QCOMPARE(c.filters->toPlainText(), QStringLiteral("other"));
            c.autoDownload->setChecked(false);
            c.reader->setChecked(false);
            QVERIFY(c.interval->isEnabled() && c.filters->isEnabled());
            c.apply->click();
            QTRY_VERIFY_WITH_TIMEOUT(c.reader->isEnabled() && !c.apply->isEnabled(), 6000);
        });
        const QJsonObject disableOnly{
            {"rss_processing_enabled", false}, {"rss_auto_downloading_enabled", false}
        };
        QCOMPARE(delta(modern), disableOnly);
        QJsonObject expectedModern = initialModern;
        for (auto it = disableOnly.constBegin(); it != disableOnly.constEnd(); ++it)
            expectedModern.insert(it.key(), it.value());
        QCOMPARE(modern.serverPreferences(), expectedModern);

        // Missing and malformed settings must not leak placeholder values.
        expectedModern.remove("rss_refresh_interval");
        expectedModern.insert("rss_fetch_delay", -1);
        expectedModern.insert("rss_max_articles_per_feed", 1.5);
        expectedModern.insert("rss_auto_downloading_enabled", "false");
        expectedModern.insert("rss_smart_episode_filters", QJsonArray{});
        modern.setServerPreferences(expectedModern);
        withRSS([](const Controls &c) {
            QVERIFY(!c.interval->isEnabled() && !c.delay->isEnabled() && !c.articles->isEnabled());
            QVERIFY(!c.autoDownload->isEnabled() && !c.filters->isEnabled());
            QVERIFY(c.repacks->isEnabled());
            c.repacks->setChecked(true);
            c.apply->click();
            QTRY_VERIFY_WITH_TIMEOUT(c.reader->isEnabled() && !c.apply->isEnabled(), 6000);
        });
        const QJsonObject repacksOnly{{"rss_download_repack_proper_episodes", true}};
        QCOMPARE(delta(modern), repacksOnly);
        expectedModern.insert("rss_download_repack_proper_episodes", true);
        QCOMPARE(modern.serverPreferences(), expectedModern);
        QCOMPARE(legacy.serverPreferences(), expectedLegacy);
    }

    void bitTorrentPrivacyFollowsSelectedServer()
    {
        DemoQbittorrentServer legacy(true), modern(true);
        modern.enableModernResponses();
        const QJsonObject initialLegacy{
            {QStringLiteral("dht"), true},
            {QStringLiteral("pex"), false},
            {QStringLiteral("lsd"), true},
            {QStringLiteral("encryption"), 0},
            {QStringLiteral("anonymous_mode"), false},
            {QStringLiteral("max_active_checking_torrents"), 1}
        };
        const QJsonObject initialModern{
            {QStringLiteral("dht"), false},
            {QStringLiteral("pex"), true},
            {QStringLiteral("lsd"), false},
            {QStringLiteral("encryption"), 1},
            {QStringLiteral("anonymous_mode"), true},
            {QStringLiteral("max_active_checking_torrents"), 2}
        };
        legacy.setServerPreferences(initialLegacy);
        modern.setServerPreferences(initialModern);
        QVERIFY(legacy.start() && modern.start());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("prefs.ini")), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        openConnectDialog(window, legacy.url());
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(QStringLiteral("v5.1.4")), 6000);
        openConnectDialog(window, modern.url(), false, true, true, QStringLiteral("Modern seedbox"));
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(QStringLiteral("v5.3.0")), 6000);
        auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("serverTabs"));
        auto *prefs = window.findChild<QAction *>(QStringLiteral("actionOptions"));
        QVERIFY(tabs && prefs);

        struct Controls
        {
            QCheckBox *dht, *pex, *lsd, *anonymous;
            QComboBox *encryption;
            QPushButton *apply;
        };
        const auto withPrivacy = [&](auto exercise) {
            bool finished = false;
            QTimer::singleShot(0, &window, [&]() {
                auto *dlg = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget());
                QVERIFY(dlg);
                const auto cleanup = qScopeGuard([dlg]() { if (dlg->isVisible()) dlg->reject(); });
                auto *sections = dlg->findChild<QListWidget *>(QStringLiteral("preferencesSections"));
                const Controls c{
                    dlg->findChild<QCheckBox *>(QStringLiteral("checkDHT")),
                    dlg->findChild<QCheckBox *>(QStringLiteral("checkPeX")),
                    dlg->findChild<QCheckBox *>(QStringLiteral("checkLSD")),
                    dlg->findChild<QCheckBox *>(QStringLiteral("checkAnonymousMode")),
                    dlg->findChild<QComboBox *>(QStringLiteral("comboEncryption")),
                    dlg->findChild<QPushButton *>(QStringLiteral("preferencesApply"))
                };
                auto *info = dlg->findChild<QLabel *>(QStringLiteral("label_anonymous"));
                auto *checking = dlg->findChild<QSpinBox *>(QStringLiteral("spinBoxMaxActiveCheckingTorrents"));
                QVERIFY(sections && c.dht && c.pex && c.lsd && c.anonymous
                    && c.encryption && c.apply && info && checking);
                sections->setCurrentRow(5);
                // Anonymous mode is present and valid even in the malformed fixture.
                QTRY_VERIFY_WITH_TIMEOUT(c.anonymous->isEnabled(), 6000);
                QCOMPARE(c.encryption->count(), 3);
                QVERIFY(info->openExternalLinks());
                exercise(c);
                finished = true;
            });
            prefs->trigger();
            QVERIFY(finished);
        };
        const auto delta = [](DemoQbittorrentServer &server) {
            const QUrlQuery form(QString::fromUtf8(server.lastCommandBody("/api/v2/app/setPreferences")));
            return QJsonDocument::fromJson(form.queryItemValue(
                QStringLiteral("json"), QUrl::FullyDecoded).toUtf8()).object();
        };
        tabs->setCurrentIndex(0);
        for (bool save : {false, true})
        {
            withPrivacy([&](const Controls &c) {
                QVERIFY(c.dht->isEnabled() && c.pex->isEnabled() && c.lsd->isEnabled()
                    && c.encryption->isEnabled());
                QVERIFY(c.dht->isChecked() && !c.pex->isChecked() && c.lsd->isChecked()
                    && !c.anonymous->isChecked());
                QCOMPARE(c.encryption->currentIndex(), 0);
                QVERIFY(!c.apply->isEnabled());
                c.encryption->setCurrentIndex(1);
                QVERIFY(c.apply->isEnabled());
                c.encryption->setCurrentIndex(0);
                QVERIFY(!c.apply->isEnabled());
                c.dht->click();
                c.pex->click();
                c.lsd->click();
                c.anonymous->click();
                c.encryption->setCurrentIndex(2);
                QVERIFY(c.apply->isEnabled());
                QCOMPARE(legacy.commandsFor("/api/v2/app/setPreferences"), 0);
                if (save)
                {
                    c.apply->click();
                    QTRY_VERIFY_WITH_TIMEOUT(c.anonymous->isEnabled() && !c.apply->isEnabled(), 6000);
                }
            });
            if (!save)
                QCOMPARE(legacy.serverPreferences(), initialLegacy);
        }
        const QJsonObject expectedDelta{
            {QStringLiteral("dht"), false},
            {QStringLiteral("pex"), true},
            {QStringLiteral("lsd"), false},
            {QStringLiteral("encryption"), 2},
            {QStringLiteral("anonymous_mode"), true}
        };
        QJsonObject expectedLegacy = expectedDelta;
        expectedLegacy.insert(QStringLiteral("max_active_checking_torrents"), 1);
        QCOMPARE(delta(legacy), expectedDelta);
        QCOMPARE(legacy.serverPreferences(), expectedLegacy);
        QCOMPARE(modern.serverPreferences(), initialModern);
        withPrivacy([](const Controls &c) {
            QVERIFY(!c.dht->isChecked() && c.pex->isChecked() && !c.lsd->isChecked()
                && c.anonymous->isChecked());
            QCOMPARE(c.encryption->currentIndex(), 2);
            QVERIFY(!c.apply->isEnabled());
        });

        tabs->setCurrentIndex(1);
        withPrivacy([&](const Controls &c) {
            QCOMPARE(c.encryption->currentIndex(), 1);
            QVERIFY(!c.dht->isChecked() && c.pex->isChecked() && !c.lsd->isChecked()
                && c.anonymous->isChecked());
            c.encryption->setCurrentIndex(0);
            c.apply->click();
            QTRY_VERIFY_WITH_TIMEOUT(c.encryption->isEnabled() && !c.apply->isEnabled(), 6000);
        });
        const QJsonObject onlyEncryption{{QStringLiteral("encryption"), 0}};
        QCOMPARE(delta(modern), onlyEncryption);
        QJsonObject expectedModern = initialModern;
        expectedModern.insert(QStringLiteral("encryption"), 0);
        QCOMPARE(modern.serverPreferences(), expectedModern);
        QCOMPARE(legacy.serverPreferences(), expectedLegacy);

        // Missing, mistyped and out-of-range settings must never be sent
        // as native Designer defaults when another valid field changes.
        QJsonObject malformed = expectedModern;
        malformed.remove(QStringLiteral("dht"));
        malformed.insert(QStringLiteral("pex"), 1);
        malformed.insert(QStringLiteral("lsd"), QStringLiteral("true"));
        malformed.insert(QStringLiteral("encryption"), 3);
        modern.setServerPreferences(malformed);
        withPrivacy([&](const Controls &c) {
            QVERIFY(!c.dht->isEnabled() && !c.pex->isEnabled() && !c.lsd->isEnabled()
                && !c.encryption->isEnabled());
            QVERIFY(c.anonymous->isChecked() && !c.apply->isEnabled());
            c.anonymous->click();
            c.apply->click();
            QTRY_VERIFY_WITH_TIMEOUT(c.anonymous->isEnabled() && !c.apply->isEnabled(), 6000);
        });
        const QJsonObject onlyAnonymous{{QStringLiteral("anonymous_mode"), false}};
        QCOMPARE(delta(modern), onlyAnonymous);
        malformed.insert(QStringLiteral("anonymous_mode"), false);
        QCOMPARE(modern.serverPreferences(), malformed);
        QCOMPARE(legacy.serverPreferences(), expectedLegacy);
    }

    void bitTorrentQueueingFollowsSelectedServer()
    {
        DemoQbittorrentServer legacy(true), modern(true);
        modern.enableModernResponses();
        const QJsonObject initialLegacy{
            {QStringLiteral("max_active_checking_torrents"), 1},
            {QStringLiteral("queueing_enabled"), false},
            {QStringLiteral("max_active_downloads"), 3},
            {QStringLiteral("max_active_uploads"), 4},
            {QStringLiteral("max_active_torrents"), 5},
            {QStringLiteral("dont_count_slow_torrents"), false},
            {QStringLiteral("slow_torrent_dl_rate_threshold"), 2},
            {QStringLiteral("slow_torrent_ul_rate_threshold"), 3},
            {QStringLiteral("slow_torrent_inactive_timer"), 60}
        };
        const QJsonObject initialModern{
            {QStringLiteral("max_active_checking_torrents"), -1},
            {QStringLiteral("queueing_enabled"), true},
            {QStringLiteral("max_active_downloads"), 0},
            {QStringLiteral("max_active_uploads"), -1},
            {QStringLiteral("max_active_torrents"), 8},
            {QStringLiteral("dont_count_slow_torrents"), true},
            {QStringLiteral("slow_torrent_dl_rate_threshold"), 12},
            {QStringLiteral("slow_torrent_ul_rate_threshold"), 0},
            {QStringLiteral("slow_torrent_inactive_timer"), 120}
        };
        legacy.setServerPreferences(initialLegacy);
        modern.setServerPreferences(initialModern);
        QVERIFY(legacy.start() && modern.start());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("prefs.ini")), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        openConnectDialog(window, legacy.url());
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(QStringLiteral("v5.1.4")), 6000);
        openConnectDialog(window, modern.url(), false, true, true, QStringLiteral("Modern seedbox"));
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(QStringLiteral("v5.3.0")), 6000);
        auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("serverTabs"));
        auto *prefs = window.findChild<QAction *>(QStringLiteral("actionOptions"));
        QVERIFY(tabs && prefs);

        struct Controls
        {
            QSpinBox *checking, *downloads, *uploads, *total, *dlRate, *ulRate, *timer;
            QGroupBox *queue, *slow;
            QPushButton *apply;
        };
        const auto withQueue = [&](auto exercise) {
            bool finished = false;
            QTimer::singleShot(0, &window, [&]() {
                auto *dlg = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget());
                QVERIFY(dlg);
                const auto cleanup = qScopeGuard([dlg]() { if (dlg->isVisible()) dlg->reject(); });
                auto *sections = dlg->findChild<QListWidget *>(QStringLiteral("preferencesSections"));
                const Controls c{
                    dlg->findChild<QSpinBox *>(QStringLiteral("spinBoxMaxActiveCheckingTorrents")),
                    dlg->findChild<QSpinBox *>(QStringLiteral("spinMaxActiveDownloads")),
                    dlg->findChild<QSpinBox *>(QStringLiteral("spinMaxActiveUploads")),
                    dlg->findChild<QSpinBox *>(QStringLiteral("spinMaxActiveTorrents")),
                    dlg->findChild<QSpinBox *>(QStringLiteral("spinDownloadRateForSlowTorrents")),
                    dlg->findChild<QSpinBox *>(QStringLiteral("spinUploadRateForSlowTorrents")),
                    dlg->findChild<QSpinBox *>(QStringLiteral("spinSlowTorrentsInactivityTimer")),
                    dlg->findChild<QGroupBox *>(QStringLiteral("checkEnableQueueing")),
                    dlg->findChild<QGroupBox *>(QStringLiteral("checkIgnoreSlowTorrentsForQueueing")),
                    dlg->findChild<QPushButton *>(QStringLiteral("preferencesApply"))
                };
                QVERIFY(sections && c.checking && c.downloads && c.uploads && c.total
                    && c.dlRate && c.ulRate && c.timer && c.queue && c.slow && c.apply);
                sections->setCurrentRow(5);
                QTRY_VERIFY_WITH_TIMEOUT(c.checking->isEnabled(), 6000);
                auto *editor = c.checking->findChild<QLineEdit *>();
                QVERIFY(editor && editor->isEnabled());
                exercise(c);
                finished = true;
            });
            prefs->trigger();
            QVERIFY(finished);
        };
        const auto delta = [](DemoQbittorrentServer &server) {
            const QUrlQuery form(QString::fromUtf8(server.lastCommandBody("/api/v2/app/setPreferences")));
            return QJsonDocument::fromJson(form.queryItemValue(
                QStringLiteral("json"), QUrl::FullyDecoded).toUtf8()).object();
        };

        tabs->setCurrentIndex(0);
        for (bool save : {false, true})
        {
            withQueue([&](const Controls &c) {
                QVERIFY(c.queue->isEnabled() && !c.queue->isChecked());
                QVERIFY(!c.downloads->isEnabled() && !c.uploads->isEnabled()
                    && !c.total->isEnabled() && !c.slow->isEnabled());
                QCOMPARE(c.checking->value(), 1);
                QCOMPARE(c.downloads->value(), 3);
                QCOMPARE(c.uploads->value(), 4);
                QCOMPARE(c.total->value(), 5);
                QCOMPARE(c.dlRate->value(), 2);
                QCOMPARE(c.ulRate->value(), 3);
                QCOMPARE(c.timer->value(), 60);
                QVERIFY(!c.apply->isEnabled());
                c.queue->setChecked(true);
                QVERIFY(c.downloads->isEnabled() && c.uploads->isEnabled()
                    && c.total->isEnabled() && c.slow->isEnabled());
                QVERIFY(c.slow->toolTip().contains(QStringLiteral("hard cap")));
                QVERIFY(!c.dlRate->isEnabled() && !c.ulRate->isEnabled() && !c.timer->isEnabled());
                c.queue->setChecked(false);
                QVERIFY(!c.apply->isEnabled());
                c.queue->setChecked(true);
                c.slow->setChecked(true);
                QVERIFY(c.dlRate->isEnabled() && c.ulRate->isEnabled() && c.timer->isEnabled());
                for (auto *spin : {c.downloads, c.uploads, c.total, c.dlRate, c.ulRate, c.timer})
                {
                    auto *editor = spin->findChild<QLineEdit *>();
                    QVERIFY(editor && editor->isEnabled());
                }
                c.checking->setValue(-1);
                QCOMPARE(c.checking->text(), QStringLiteral("∞"));
                c.downloads->setValue(0);
                c.uploads->setValue(-1);
                c.total->setValue(2147483647);
                c.dlRate->setValue(2000000);
                c.ulRate->setValue(0);
                c.timer->setValue(999999);
                c.slow->setChecked(false);
                QVERIFY(!c.dlRate->isEnabled());
                QCOMPARE(c.dlRate->value(), 2000000);
                c.slow->setChecked(true);
                c.queue->setChecked(false);
                QVERIFY(!c.slow->isEnabled() && !c.dlRate->isEnabled());
                QVERIFY(c.slow->isChecked());
                c.queue->setChecked(true);
                QVERIFY(c.slow->isEnabled() && c.dlRate->isEnabled());
                QCOMPARE(c.timer->value(), 999999);
                QVERIFY(c.apply->isEnabled());
                QCOMPARE(legacy.commandsFor("/api/v2/app/setPreferences"), 0);
                if (save)
                {
                    c.apply->click();
                    QTRY_VERIFY_WITH_TIMEOUT(c.checking->isEnabled() && !c.apply->isEnabled(), 6000);
                }
            });
            if (!save)
                QCOMPARE(legacy.serverPreferences(), initialLegacy);
        }
        const QJsonObject expectedLegacy{
            {QStringLiteral("max_active_checking_torrents"), -1},
            {QStringLiteral("queueing_enabled"), true},
            {QStringLiteral("max_active_downloads"), 0},
            {QStringLiteral("max_active_uploads"), -1},
            {QStringLiteral("max_active_torrents"), 2147483647},
            {QStringLiteral("dont_count_slow_torrents"), true},
            {QStringLiteral("slow_torrent_dl_rate_threshold"), 2000000},
            {QStringLiteral("slow_torrent_ul_rate_threshold"), 0},
            {QStringLiteral("slow_torrent_inactive_timer"), 999999}
        };
        QCOMPARE(delta(legacy), expectedLegacy);
        QCOMPARE(legacy.serverPreferences(), expectedLegacy);
        QCOMPARE(modern.serverPreferences(), initialModern);
        withQueue([](const Controls &c) {
            QVERIFY(c.queue->isChecked() && c.slow->isChecked() && c.timer->isEnabled());
            QCOMPARE(c.checking->value(), -1);
            QCOMPARE(c.downloads->value(), 0);
            QCOMPARE(c.uploads->value(), -1);
            QCOMPARE(c.total->value(), 2147483647);
            QCOMPARE(c.dlRate->value(), 2000000);
            QCOMPARE(c.ulRate->value(), 0);
            QCOMPARE(c.timer->value(), 999999);
            QVERIFY(!c.apply->isEnabled());
        });

        tabs->setCurrentIndex(1);
        withQueue([&](const Controls &c) {
            QVERIFY(c.queue->isChecked() && c.slow->isChecked());
            QCOMPARE(c.total->value(), 8);
            QCOMPARE(c.dlRate->value(), 12);
            QCOMPARE(c.timer->value(), 120);
            c.queue->setChecked(false);
            QVERIFY(c.checking->isEnabled());
            QVERIFY(!c.slow->isEnabled() && !c.downloads->isEnabled() && !c.timer->isEnabled());
            c.apply->click();
            QTRY_VERIFY_WITH_TIMEOUT(c.checking->isEnabled() && !c.apply->isEnabled(), 6000);
        });
        const QJsonObject disableQueue{{QStringLiteral("queueing_enabled"), false}};
        QCOMPARE(delta(modern), disableQueue);
        QJsonObject expectedModern = initialModern;
        expectedModern.insert(QStringLiteral("queueing_enabled"), false);
        QCOMPARE(modern.serverPreferences(), expectedModern);
        withQueue([&](const Controls &c) {
            QVERIFY(!c.queue->isChecked() && c.slow->isChecked());
            QCOMPARE(c.dlRate->value(), 12);
            c.queue->setChecked(true);
            QVERIFY(c.timer->isEnabled());
            c.slow->setChecked(false);
            QVERIFY(!c.timer->isEnabled());
            c.apply->click();
            QTRY_VERIFY_WITH_TIMEOUT(c.checking->isEnabled() && !c.apply->isEnabled(), 6000);
        });
        const QJsonObject enableQueueDisableSlow{
            {QStringLiteral("queueing_enabled"), true},
            {QStringLiteral("dont_count_slow_torrents"), false}
        };
        QCOMPARE(delta(modern), enableQueueDisableSlow);
        expectedModern.insert(QStringLiteral("queueing_enabled"), true);
        expectedModern.insert(QStringLiteral("dont_count_slow_torrents"), false);
        QCOMPARE(modern.serverPreferences(), expectedModern);

        // Missing and unrepresentable settings must not leak defaults.
        QJsonObject malformed = expectedModern;
        malformed.remove(QStringLiteral("queueing_enabled"));
        malformed.insert(QStringLiteral("max_active_downloads"), -2);
        malformed.insert(QStringLiteral("slow_torrent_dl_rate_threshold"), 2000001);
        malformed.insert(QStringLiteral("slow_torrent_ul_rate_threshold"), QStringLiteral("2"));
        malformed.insert(QStringLiteral("slow_torrent_inactive_timer"), 0);
        modern.setServerPreferences(malformed);
        withQueue([&](const Controls &c) {
            QVERIFY(!c.queue->isEnabled() && !c.slow->isEnabled() && !c.downloads->isEnabled());
            QVERIFY(c.checking->isEnabled());
            c.checking->setValue(2);
            c.apply->click();
            QTRY_VERIFY_WITH_TIMEOUT(c.checking->isEnabled() && !c.apply->isEnabled(), 6000);
        });
        const QJsonObject checkingOnly{{QStringLiteral("max_active_checking_torrents"), 2}};
        QCOMPARE(delta(modern), checkingOnly);
        malformed.insert(QStringLiteral("max_active_checking_torrents"), 2);
        QCOMPARE(modern.serverPreferences(), malformed);
        malformed.insert(QStringLiteral("queueing_enabled"), true);
        malformed.insert(QStringLiteral("dont_count_slow_torrents"), true);
        modern.setServerPreferences(malformed);
        withQueue([&](const Controls &c) {
            QVERIFY(c.queue->isEnabled() && c.queue->isChecked() && c.slow->isEnabled()
                && c.slow->isChecked());
            QVERIFY(!c.downloads->isEnabled() && !c.dlRate->isEnabled()
                && !c.ulRate->isEnabled() && !c.timer->isEnabled());
            QVERIFY(c.uploads->isEnabled());
            c.uploads->setValue(7);
            c.apply->click();
            QTRY_VERIFY_WITH_TIMEOUT(c.checking->isEnabled() && !c.apply->isEnabled(), 6000);
        });
        const QJsonObject uploadsOnly{{QStringLiteral("max_active_uploads"), 7}};
        QCOMPARE(delta(modern), uploadsOnly);
        malformed.insert(QStringLiteral("max_active_uploads"), 7);
        QCOMPARE(modern.serverPreferences(), malformed);
        QCOMPARE(legacy.serverPreferences(), expectedLegacy);
    }

    void bitTorrentSeedingLimitsFollowSelectedServer()
    {
        DemoQbittorrentServer legacy(true), modern(true);
        modern.enableModernResponses();
        const QJsonObject initialLegacy{
            {"max_ratio_enabled", false}, {"max_ratio", -1},
            {"max_seeding_time_enabled", true}, {"max_seeding_time", 1440},
            {"max_inactive_seeding_time_enabled", false}, {"max_inactive_seeding_time", -1},
            {"max_ratio_act", 2}
        };
        const QJsonObject initialModern{
            {"max_ratio_enabled", true}, {"max_ratio", 2.5},
            {"max_seeding_time_enabled", false}, {"max_seeding_time", -1},
            {"max_inactive_seeding_time_enabled", true}, {"max_inactive_seeding_time", 60},
            {"max_ratio_act", 0}, {"share_limits_mode", "MatchAll"}
        };
        legacy.setServerPreferences(initialLegacy);
        modern.setServerPreferences(initialModern);
        QVERIFY(legacy.start() && modern.start());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("prefs.ini")), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        openConnectDialog(window, legacy.url());
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(QStringLiteral("v5.1.4")), 6000);
        openConnectDialog(window, modern.url(), false, true, true, QStringLiteral("Modern seedbox"));
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(QStringLiteral("v5.3.0")), 6000);
        auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("serverTabs"));
        auto *prefs = window.findChild<QAction *>(QStringLiteral("actionOptions"));
        QVERIFY(tabs && prefs);

        struct Controls {
            QCheckBox *ratio, *time, *inactive;
            QDoubleSpinBox *ratioValue;
            QSpinBox *timeValue, *inactiveValue;
            QRadioButton *any, *all;
            QComboBox *action;
            QPushButton *apply;
        };
        const auto withLimits = [&](auto exercise) {
            bool finished = false;
            QTimer::singleShot(0, &window, [&]() {
                auto *dlg = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget());
                QVERIFY(dlg);
                const auto cleanup = qScopeGuard([dlg]() { if (dlg->isVisible()) dlg->reject(); });
                auto *sections = dlg->findChild<QListWidget *>(QStringLiteral("preferencesSections"));
                const Controls c{
                    dlg->findChild<QCheckBox *>("checkMaxRatio"),
                    dlg->findChild<QCheckBox *>("checkMaxSeedingMinutes"),
                    dlg->findChild<QCheckBox *>("checkMaxInactiveSeedingMinutes"),
                    dlg->findChild<QDoubleSpinBox *>("spinMaxRatio"),
                    dlg->findChild<QSpinBox *>("spinMaxSeedingMinutes"),
                    dlg->findChild<QSpinBox *>("spinMaxInactiveSeedingMinutes"),
                    dlg->findChild<QRadioButton *>("radioButtonShareLimitsModeAny"),
                    dlg->findChild<QRadioButton *>("radioButtonShareLimitsModeAll"),
                    dlg->findChild<QComboBox *>("comboRatioLimitAct"),
                    dlg->findChild<QPushButton *>("preferencesApply")
                };
                QVERIFY(sections && c.ratio && c.time && c.inactive && c.ratioValue
                    && c.timeValue && c.inactiveValue && c.any && c.all && c.action && c.apply);
                sections->setCurrentRow(5);
                QTRY_VERIFY_WITH_TIMEOUT(c.time->isEnabled(), 6000);
                exercise(c);
                finished = true;
            });
            prefs->trigger();
            QVERIFY(finished);
        };
        const auto delta = [](DemoQbittorrentServer &server) {
            const QUrlQuery form(QString::fromUtf8(server.lastCommandBody("/api/v2/app/setPreferences")));
            return QJsonDocument::fromJson(form.queryItemValue(
                QStringLiteral("json"), QUrl::FullyDecoded).toUtf8()).object();
        };

        tabs->setCurrentIndex(0);
        for (bool save : {false, true})
        {
            withLimits([&](const Controls &c) {
                QVERIFY(!c.ratio->isChecked() && !c.ratioValue->isEnabled());
                QVERIFY(c.time->isChecked() && c.timeValue->isEnabled());
                QCOMPARE(c.timeValue->value(), 1440);
                QCOMPARE(c.action->currentIndex(), 3); // API 2 = super seeding
                QVERIFY(!c.any->isEnabled() && !c.all->isEnabled());
                QVERIFY(!c.apply->isEnabled());
                c.ratio->setChecked(true);
                QVERIFY(c.ratioValue->isEnabled());
                QVERIFY(c.ratioValue->findChild<QLineEdit *>()->isEnabled());
                c.ratioValue->setValue(1.75);
                c.ratio->setChecked(false);
                QVERIFY(!c.apply->isEnabled());
                c.ratio->setChecked(true);
                QCOMPARE(c.ratioValue->value(), 1.75);
                c.timeValue->setValue(2880); // enabled flag still required in legacy write
                c.inactive->setChecked(true);
                c.inactiveValue->setValue(0);
                c.action->setCurrentIndex(0);
                QVERIFY(c.apply->isEnabled());
                QCOMPARE(legacy.commandsFor("/api/v2/app/setPreferences"), 0);
                if (save)
                {
                    c.apply->click();
                    QTRY_VERIFY_WITH_TIMEOUT(c.time->isEnabled() && !c.apply->isEnabled(), 6000);
                }
            });
            if (!save) QCOMPARE(legacy.serverPreferences(), initialLegacy);
        }
        const QJsonObject expectedLegacy{
            {"max_ratio_enabled", true}, {"max_ratio", 1.75},
            {"max_seeding_time_enabled", true}, {"max_seeding_time", 2880},
            {"max_inactive_seeding_time_enabled", true}, {"max_inactive_seeding_time", 0},
            {"max_ratio_act", 0}
        };
        QCOMPARE(delta(legacy), expectedLegacy);
        QCOMPARE(legacy.serverPreferences(), expectedLegacy);
        QCOMPARE(modern.serverPreferences(), initialModern);
        withLimits([](const Controls &c) {
            QVERIFY(c.ratio->isChecked() && c.time->isChecked() && c.inactive->isChecked());
            QCOMPARE(c.ratioValue->value(), 1.75);
            QCOMPARE(c.timeValue->value(), 2880);
            QCOMPARE(c.inactiveValue->value(), 0);
            QVERIFY(!c.apply->isEnabled());
        });

        tabs->setCurrentIndex(1);
        withLimits([&](const Controls &c) {
            QVERIFY(c.all->isEnabled() && c.all->isChecked());
            QCOMPARE(c.ratioValue->value(), 2.5);
            QCOMPARE(c.inactiveValue->value(), 60);
            c.ratio->setChecked(false);
            c.inactive->setChecked(false);
            QVERIFY(!c.action->isEnabled());
            c.time->setChecked(true);
            c.timeValue->setValue(720);
            QVERIFY(c.action->isEnabled());
            c.any->setChecked(true);
            c.action->setCurrentIndex(2); // native delete files = API 3

            // Declining the destructive-action confirmation leaves the
            // server unchanged and the edits staged.
            const auto answer = [&](QMessageBox::StandardButton response) {
                QTimer::singleShot(0, &window, [response]() {
                    if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                        box->button(response)->click();
                });
                c.apply->click();
            };
            answer(QMessageBox::No);
            QCOMPARE(modern.serverPreferences(), initialModern);
            QVERIFY(c.apply->isEnabled());
            answer(QMessageBox::Yes);
            QTRY_VERIFY_WITH_TIMEOUT(c.time->isEnabled() && !c.apply->isEnabled(), 6000);
        });
        const QJsonObject expectedModern{
            {"max_ratio_enabled", false}, {"max_ratio", -1},
            {"max_seeding_time_enabled", true}, {"max_seeding_time", 720},
            {"max_inactive_seeding_time_enabled", false}, {"max_inactive_seeding_time", -1},
            {"max_ratio_act", 3}, {"share_limits_mode", "MatchAny"}
        };
        QCOMPARE(delta(modern), expectedModern);
        QCOMPARE(modern.serverPreferences(), expectedModern);
        QCOMPARE(legacy.serverPreferences(), expectedLegacy);
        withLimits([](const Controls &c) {
            QVERIFY(c.any->isChecked() && c.any->isEnabled());
            QCOMPARE(c.action->currentIndex(), 2);
            QCOMPARE(c.timeValue->value(), 720);
            QVERIFY(!c.ratioValue->isEnabled() && !c.inactiveValue->isEnabled());
        });

        QJsonObject malformed = expectedModern;
        malformed.insert("max_ratio_enabled", true); // contradicts -1
        malformed.insert("max_inactive_seeding_time", 12); // contradicts false
        malformed.insert("share_limits_mode", "Unknown");
        malformed.insert("max_ratio_act", 99);
        modern.setServerPreferences(malformed);
        withLimits([](const Controls &c) {
            QVERIFY(!c.ratio->isEnabled() && !c.ratioValue->isEnabled());
            QVERIFY(!c.inactive->isEnabled() && !c.inactiveValue->isEnabled());
            QVERIFY(!c.any->isEnabled() && !c.all->isEnabled() && !c.action->isEnabled());
            QVERIFY(c.time->isEnabled() && c.timeValue->isEnabled());
            QVERIFY(!c.apply->isEnabled());
        });
        QCOMPARE(modern.serverPreferences(), malformed);
    }

    void bitTorrentAutomaticTrackersFollowSelectedServer()
    {
        DemoQbittorrentServer legacy(true), modern(true);
        modern.enableModernResponses();
        const QString manual = QStringLiteral("udp://one.example:6969/announce\n\nhttps://two.example/announce");
        const QString oldUrl = QStringLiteral("https://lists.example/old.txt");
        const QString newUrl = QStringLiteral("https://lists.example/new.txt?format=plain&v=1");
        const QString fetched = QStringLiteral("https://fetched.example/announce\n");
        const QJsonObject initialLegacy{
            {"add_trackers_enabled", false}, {"add_trackers", manual},
            {"add_trackers_from_url_enabled", true}, {"add_trackers_url", oldUrl},
            {"add_trackers_url_list", fetched}
        };
        const QJsonObject initialModern{
            {"add_trackers_enabled", true}, {"add_trackers", "https://modern.example/announce"},
            {"add_trackers_from_url_enabled", false}, {"add_trackers_url", ""},
            {"add_trackers_url_list", ""}
        };
        legacy.setServerPreferences(initialLegacy);
        modern.setServerPreferences(initialModern);
        QVERIFY(legacy.start() && modern.start());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("prefs.ini")), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        openConnectDialog(window, legacy.url());
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(QStringLiteral("v5.1.4")), 6000);
        openConnectDialog(window, modern.url(), false, true, true, QStringLiteral("Modern seedbox"));
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(QStringLiteral("v5.3.0")), 6000);
        auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("serverTabs"));
        auto *prefs = window.findChild<QAction *>(QStringLiteral("actionOptions"));
        QVERIFY(tabs && prefs);

        struct Controls {
            QGroupBox *manualEnabled, *urlEnabled;
            QPlainTextEdit *manual, *fetched;
            QLineEdit *url;
            QPushButton *apply;
        };
        const auto withTrackers = [&](auto exercise) {
            bool finished = false;
            QTimer::singleShot(0, &window, [&]() {
                auto *dlg = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget());
                QVERIFY(dlg);
                const auto cleanup = qScopeGuard([dlg]() { if (dlg->isVisible()) dlg->reject(); });
                auto *sections = dlg->findChild<QListWidget *>("preferencesSections");
                const Controls c{
                    dlg->findChild<QGroupBox *>("checkEnableAddTrackers"),
                    dlg->findChild<QGroupBox *>("checkAddTrackersFromURL"),
                    dlg->findChild<QPlainTextEdit *>("textTrackers"),
                    dlg->findChild<QPlainTextEdit *>("textTrackersFromURL"),
                    dlg->findChild<QLineEdit *>("textTrackersURL"),
                    dlg->findChild<QPushButton *>("preferencesApply")
                };
                QVERIFY(sections && c.manualEnabled && c.urlEnabled && c.manual && c.fetched && c.url && c.apply);
                sections->setCurrentRow(5);
                QTRY_VERIFY_WITH_TIMEOUT(c.manualEnabled->isEnabled() || c.urlEnabled->isEnabled(), 6000);
                QVERIFY(c.fetched->isReadOnly());
                exercise(c);
                finished = true;
            });
            prefs->trigger();
            QVERIFY(finished);
        };
        const auto delta = [](DemoQbittorrentServer &server) {
            const QUrlQuery form(QString::fromUtf8(server.lastCommandBody("/api/v2/app/setPreferences")));
            return QJsonDocument::fromJson(form.queryItemValue(
                QStringLiteral("json"), QUrl::FullyDecoded).toUtf8()).object();
        };
        const QString revised = manual + QStringLiteral("\n\nhttps://three.example/announce?key=a+b%2Fc");
        tabs->setCurrentIndex(0);
        for (bool save : {false, true})
        {
            withTrackers([&](const Controls &c) {
                QVERIFY(!c.manualEnabled->isChecked() && !c.manual->isEnabled());
                QCOMPARE(c.manual->toPlainText(), manual);
                QVERIFY(c.urlEnabled->isChecked() && c.url->isEnabled() && c.fetched->isEnabled());
                QCOMPARE(c.url->text(), oldUrl);
                QCOMPARE(c.fetched->toPlainText(), fetched);
                QVERIFY(!c.apply->isEnabled());
                c.manualEnabled->setChecked(true);
                QVERIFY(c.manual->isEnabled());
                c.manualEnabled->setChecked(false);
                QVERIFY(!c.apply->isEnabled());
                c.manualEnabled->setChecked(true);
                c.manual->setPlainText(revised);
                c.manualEnabled->setChecked(false);
                QCOMPARE(c.manual->toPlainText(), revised);
                c.manualEnabled->setChecked(true);
                c.url->setText(QStringLiteral("  ") + newUrl + QStringLiteral("  "));
                // The fetched snapshot is not synthesized from the edited URL.
                QCOMPARE(c.fetched->toPlainText(), fetched);
                QVERIFY(c.apply->isEnabled());
                QCOMPARE(legacy.commandsFor("/api/v2/app/setPreferences"), 0);
                if (save)
                {
                    c.apply->click();
                    QTRY_VERIFY_WITH_TIMEOUT(c.manualEnabled->isEnabled() && !c.apply->isEnabled(), 6000);
                    QCOMPARE(c.url->text(), newUrl);
                }
            });
            if (!save) QCOMPARE(legacy.serverPreferences(), initialLegacy);
        }
        const QJsonObject expectedDelta{
            {"add_trackers_enabled", true}, {"add_trackers", revised}, {"add_trackers_url", newUrl}
        };
        QCOMPARE(delta(legacy), expectedDelta);
        QJsonObject expectedLegacy = initialLegacy;
        for (auto it = expectedDelta.constBegin(); it != expectedDelta.constEnd(); ++it)
            expectedLegacy.insert(it.key(), it.value());
        QCOMPARE(legacy.serverPreferences(), expectedLegacy);
        QCOMPARE(modern.serverPreferences(), initialModern);
        // Simulate completion of the daemon's asynchronous fetch. Reopening
        // must show the actual new server value, not the old cached list.
        expectedLegacy.insert("add_trackers_url_list", "udp://new.example:6969/announce");
        legacy.setServerPreferences(expectedLegacy);
        withTrackers([&](const Controls &c) {
            QCOMPARE(c.manual->toPlainText(), revised);
            QCOMPARE(c.url->text(), newUrl);
            QCOMPARE(c.fetched->toPlainText(), QStringLiteral("udp://new.example:6969/announce"));
            QVERIFY(!c.apply->isEnabled());
        });

        tabs->setCurrentIndex(1);
        withTrackers([&](const Controls &c) {
            QVERIFY(c.manualEnabled->isChecked() && c.manual->isEnabled());
            QCOMPARE(c.manual->toPlainText(), QStringLiteral("https://modern.example/announce"));
            QVERIFY(!c.urlEnabled->isChecked() && !c.url->isEnabled() && !c.fetched->isEnabled());
            c.manualEnabled->setChecked(false);
            c.urlEnabled->setChecked(true);
            QVERIFY(c.url->isEnabled() && c.fetched->isEnabled());
            c.url->setText(newUrl);
            c.apply->click();
            QTRY_VERIFY_WITH_TIMEOUT(c.urlEnabled->isEnabled() && !c.apply->isEnabled(), 6000);
        });
        const QJsonObject modernDelta{
            {"add_trackers_enabled", false}, {"add_trackers_from_url_enabled", true}, {"add_trackers_url", newUrl}
        };
        QCOMPARE(delta(modern), modernDelta);
        QJsonObject expectedModern = initialModern;
        for (auto it = modernDelta.constBegin(); it != modernDelta.constEnd(); ++it)
            expectedModern.insert(it.key(), it.value());
        QCOMPARE(modern.serverPreferences(), expectedModern);
        QCOMPARE(legacy.serverPreferences(), expectedLegacy);
        // Unavailable fields remain disabled and must never be written as
        // empty defaults. The valid enable flag can still disable a list.
        expectedModern.remove("add_trackers");
        expectedModern.insert("add_trackers_url_list", 42);
        modern.setServerPreferences(expectedModern);
        withTrackers([](const Controls &c) {
            QVERIFY(!c.manual->isEnabled());
            QVERIFY(c.urlEnabled->isChecked() && !c.fetched->isEnabled());
            QVERIFY(c.fetched->toPlainText().isEmpty());
            QVERIFY(!c.apply->isEnabled());
            c.urlEnabled->setChecked(false);
            c.apply->click();
            QTRY_VERIFY_WITH_TIMEOUT(c.urlEnabled->isEnabled() && !c.apply->isEnabled(), 6000);
        });
        const QJsonObject disableOnly{{"add_trackers_from_url_enabled", false}};
        QCOMPARE(delta(modern), disableOnly);
        expectedModern.insert("add_trackers_from_url_enabled", false);
        QCOMPARE(modern.serverPreferences(), expectedModern);
        QCOMPARE(legacy.serverPreferences(), expectedLegacy);
    }

    void speedPreferencesFollowSelectedServer()
    {
        DemoQbittorrentServer legacy(true), modern(true);
        modern.enableModernResponses();
        const QJsonObject initialLegacy{
            {QStringLiteral("up_limit"), 100 * 1024},
            {QStringLiteral("dl_limit"), 0},
            {QStringLiteral("alt_up_limit"), 10 * 1024},
            {QStringLiteral("alt_dl_limit"), 20 * 1024},
            {QStringLiteral("scheduler_enabled"), false},
            {QStringLiteral("schedule_from_hour"), 8},
            {QStringLiteral("schedule_from_min"), 15},
            {QStringLiteral("schedule_to_hour"), 20},
            {QStringLiteral("schedule_to_min"), 30},
            {QStringLiteral("scheduler_days"), 0},
            {QStringLiteral("limit_utp_rate"), true},
            {QStringLiteral("limit_tcp_overhead"), false},
            {QStringLiteral("limit_lan_peers"), true}
        };
        QJsonObject initialModern = initialLegacy;
        initialModern.insert(QStringLiteral("up_limit"), 200 * 1024);
        initialModern.insert(QStringLiteral("scheduler_enabled"), true);
        initialModern.insert(QStringLiteral("scheduler_days"), 9);
        legacy.setServerPreferences(initialLegacy);
        modern.setServerPreferences(initialModern);
        QVERIFY(legacy.start() && modern.start());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("prefs.ini")), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        openConnectDialog(window, legacy.url());
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(QStringLiteral("v5.1.4")), 6000);
        openConnectDialog(window, modern.url(), false, true, true, QStringLiteral("Modern seedbox"));
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(QStringLiteral("v5.3.0")), 6000);
        auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("serverTabs"));
        auto *prefs = window.findChild<QAction *>(QStringLiteral("actionOptions"));
        QVERIFY(tabs && prefs);

        struct Controls
        {
            QSpinBox *up, *down, *altUp, *altDown;
            QGroupBox *schedule;
            QTimeEdit *from, *to;
            QComboBox *days;
            QCheckBox *utp, *overhead, *lan;
            QPushButton *apply;
        };
        const auto withSpeed = [&](auto exercise) {
            bool finished = false;
            QTimer::singleShot(0, &window, [&]() {
                auto *dlg = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget());
                QVERIFY(dlg);
                const auto cleanup = qScopeGuard([dlg]() { if (dlg->isVisible()) dlg->reject(); });
                auto *sections = dlg->findChild<QListWidget *>(QStringLiteral("preferencesSections"));
                const Controls c{
                    dlg->findChild<QSpinBox *>(QStringLiteral("spinUploadLimit")),
                    dlg->findChild<QSpinBox *>(QStringLiteral("spinDownloadLimit")),
                    dlg->findChild<QSpinBox *>(QStringLiteral("spinUploadLimitAlt")),
                    dlg->findChild<QSpinBox *>(QStringLiteral("spinDownloadLimitAlt")),
                    dlg->findChild<QGroupBox *>(QStringLiteral("groupBoxSchedule")),
                    dlg->findChild<QTimeEdit *>(QStringLiteral("timeEditScheduleFrom")),
                    dlg->findChild<QTimeEdit *>(QStringLiteral("timeEditScheduleTo")),
                    dlg->findChild<QComboBox *>(QStringLiteral("comboBoxScheduleDays")),
                    dlg->findChild<QCheckBox *>(QStringLiteral("checkLimituTPConnections")),
                    dlg->findChild<QCheckBox *>(QStringLiteral("checkLimitTransportOverhead")),
                    dlg->findChild<QCheckBox *>(QStringLiteral("checkLimitLocalPeerRate")),
                    dlg->findChild<QPushButton *>(QStringLiteral("preferencesApply"))
                };
                QVERIFY(sections && c.up && c.down && c.altUp && c.altDown && c.schedule
                    && c.from && c.to && c.days && c.utp && c.overhead && c.lan && c.apply);
                sections->setCurrentRow(4);
                QTRY_VERIFY_WITH_TIMEOUT(c.lan->isEnabled(), 6000);
                QCOMPARE(c.days->count(), 10);
                exercise(c);
                finished = true;
            });
            prefs->trigger();
            QVERIFY(finished);
        };
        const auto delta = [](DemoQbittorrentServer &server) {
            const QUrlQuery form(QString::fromUtf8(server.lastCommandBody("/api/v2/app/setPreferences")));
            return QJsonDocument::fromJson(form.queryItemValue(
                QStringLiteral("json"), QUrl::FullyDecoded).toUtf8()).object();
        };

        tabs->setCurrentIndex(0);
        for (bool save : {false, true})
        {
            withSpeed([&](const Controls &c) {
                QVERIFY(c.up->isEnabled() && c.down->isEnabled() && c.altUp->isEnabled() && c.altDown->isEnabled());
                QCOMPARE(c.up->value(), 100);
                QCOMPARE(c.down->value(), 0);
                QCOMPARE(c.altUp->value(), 10);
                QCOMPARE(c.altDown->value(), 20);
                QVERIFY(c.schedule->isEnabled() && !c.schedule->isChecked());
                QVERIFY(!c.from->isEnabled() && !c.to->isEnabled() && !c.days->isEnabled());
                QVERIFY(c.utp->isChecked() && !c.overhead->isChecked() && c.lan->isChecked());
                QVERIFY(!c.apply->isEnabled());
                c.up->setValue(0);
                c.down->setValue(2000000);
                c.altUp->setValue(32);
                c.altDown->setValue(0);
                c.schedule->setChecked(true);
                QVERIFY(c.from->isEnabled() && c.to->isEnabled() && c.days->isEnabled());
                QCOMPARE(c.from->time(), QTime(8, 15));
                QCOMPARE(c.to->time(), QTime(20, 30));
                // Only minutes of From and hours of To change. The API still
                // requires the complete hour/minute pair for each.
                c.from->setTime(QTime(8, 45));
                c.to->setTime(QTime(7, 30)); // Overnight is valid.
                c.days->setCurrentIndex(2);
                c.utp->click();
                c.overhead->click();
                c.lan->click();
                QVERIFY(c.apply->isEnabled());
                QCOMPARE(legacy.commandsFor("/api/v2/app/setPreferences"), 0);
                if (save)
                {
                    // Native equal-time validation must keep all edits staged.
                    c.to->setTime(c.from->time());
                    bool warned = false;
                    QTimer closer;
                    closer.setInterval(20);
                    connect(&closer, &QTimer::timeout, &window, [&]() {
                        if (auto *warning = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                        {
                            warned = true;
                            warning->accept();
                        }
                    });
                    closer.start();
                    c.apply->click();
                    closer.stop();
                    QVERIFY(warned && c.apply->isEnabled());
                    QCOMPARE(legacy.commandsFor("/api/v2/app/setPreferences"), 0);
                    c.to->setTime(QTime(7, 30));
                    c.apply->click();
                    QTRY_VERIFY_WITH_TIMEOUT(c.schedule->isEnabled() && !c.apply->isEnabled(), 6000);
                }
            });
            if (!save) QCOMPARE(legacy.serverPreferences(), initialLegacy);
        }
        const QJsonObject expectedLegacy{
            {QStringLiteral("up_limit"), 0},
            {QStringLiteral("dl_limit"), 2000000 * 1024},
            {QStringLiteral("alt_up_limit"), 32 * 1024},
            {QStringLiteral("alt_dl_limit"), 0},
            {QStringLiteral("scheduler_enabled"), true},
            {QStringLiteral("schedule_from_hour"), 8},
            {QStringLiteral("schedule_from_min"), 45},
            {QStringLiteral("schedule_to_hour"), 7},
            {QStringLiteral("schedule_to_min"), 30},
            {QStringLiteral("scheduler_days"), 2},
            {QStringLiteral("limit_utp_rate"), false},
            {QStringLiteral("limit_tcp_overhead"), true},
            {QStringLiteral("limit_lan_peers"), false}
        };
        QCOMPARE(delta(legacy), expectedLegacy);
        QCOMPARE(legacy.serverPreferences(), expectedLegacy);
        QCOMPARE(modern.serverPreferences(), initialModern);
        withSpeed([](const Controls &c) {
            QCOMPARE(c.down->value(), 2000000);
            QCOMPARE(c.from->time(), QTime(8, 45));
            QCOMPARE(c.to->time(), QTime(7, 30));
            QCOMPARE(c.days->currentIndex(), 2);
            QVERIFY(!c.apply->isEnabled());
        });

        tabs->setCurrentIndex(1);
        withSpeed([&](const Controls &c) {
            QCOMPARE(c.up->value(), 200);
            QVERIFY(c.schedule->isChecked());
            QCOMPARE(c.days->currentIndex(), 9);
            c.schedule->setChecked(false);
            QVERIFY(!c.from->isEnabled() && !c.to->isEnabled() && !c.days->isEnabled());
            c.schedule->setChecked(true);
            QCOMPARE(c.from->time(), QTime(8, 15));
            QCOMPARE(c.days->currentIndex(), 9);
            QVERIFY(!c.apply->isEnabled());
            c.schedule->setChecked(false);
            c.apply->click();
            QTRY_VERIFY_WITH_TIMEOUT(c.schedule->isEnabled() && !c.apply->isEnabled(), 6000);
        });
        const QJsonObject disabledSchedule{{QStringLiteral("scheduler_enabled"), false}};
        QCOMPARE(delta(modern), disabledSchedule);
        initialModern.insert(QStringLiteral("scheduler_enabled"), false);
        QCOMPARE(modern.serverPreferences(), initialModern);
        QCOMPARE(legacy.serverPreferences(), expectedLegacy);

        // Unsupported or non-representable values must not become defaults.
        QJsonObject malformed = initialModern;
        malformed.insert(QStringLiteral("up_limit"), 511);
        malformed.remove(QStringLiteral("alt_dl_limit"));
        malformed.insert(QStringLiteral("schedule_from_hour"), 24);
        malformed.remove(QStringLiteral("schedule_to_min"));
        malformed.insert(QStringLiteral("scheduler_days"), 10);
        malformed.insert(QStringLiteral("limit_utp_rate"), QStringLiteral("true"));
        modern.setServerPreferences(malformed);
        withSpeed([&](const Controls &c) {
            QVERIFY(!c.up->isEnabled() && !c.altDown->isEnabled() && !c.utp->isEnabled());
            QVERIFY(c.down->isEnabled() && c.altUp->isEnabled() && c.schedule->isEnabled());
            c.schedule->setChecked(true);
            QVERIFY(!c.from->isEnabled() && !c.to->isEnabled() && !c.days->isEnabled());
            c.schedule->setChecked(false);
            QVERIFY(!c.apply->isEnabled());
            c.down->setValue(128);
            c.apply->click();
            QTRY_VERIFY_WITH_TIMEOUT(c.down->isEnabled() && !c.apply->isEnabled(), 6000);
        });
        const QJsonObject onlyDown{{QStringLiteral("dl_limit"), 128 * 1024}};
        QCOMPARE(delta(modern), onlyDown);
        malformed.insert(QStringLiteral("dl_limit"), 128 * 1024);
        QCOMPARE(modern.serverPreferences(), malformed);
        QCOMPARE(legacy.serverPreferences(), expectedLegacy);
    }

    void ipFilteringFollowsSelectedServer()
    {
        DemoQbittorrentServer legacy(true), modern(true);
        modern.enableModernResponses();
        const QJsonObject initialLegacy{
            {QStringLiteral("ip_filter_enabled"), false},
            {QStringLiteral("ip_filter_path"), QStringLiteral("/share/old.dat")},
            {QStringLiteral("ip_filter_trackers"), true},
            {QStringLiteral("banned_IPs"), QStringLiteral("192.0.2.1")}
        };
        const QJsonObject initialModern{
            {QStringLiteral("ip_filter_enabled"), true},
            {QStringLiteral("ip_filter_path"), QStringLiteral("/share/modern.p2p")},
            {QStringLiteral("ip_filter_trackers"), false},
            {QStringLiteral("banned_IPs"), QStringLiteral("198.51.100.1 - 198.51.100.9")}
        };
        legacy.setServerPreferences(initialLegacy);
        modern.setServerPreferences(initialModern);
        QVERIFY(legacy.start() && modern.start());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("prefs.ini")), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        openConnectDialog(window, legacy.url());
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(QStringLiteral("v5.1.4")), 6000);
        openConnectDialog(window, modern.url(), false, true, true, QStringLiteral("Modern seedbox"));
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(QStringLiteral("v5.3.0")), 6000);
        auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("serverTabs"));
        auto *prefs = window.findChild<QAction *>(QStringLiteral("actionOptions"));
        QVERIFY(tabs && prefs);

        struct Controls
        {
            QCheckBox *enabled;
            QLineEdit *path;
            QCheckBox *trackers;
            QPushButton *ban;
            QToolButton *reload;
            QPushButton *apply;
        };
        const auto withFilter = [&](auto exercise) {
            bool finished = false;
            QTimer::singleShot(0, &window, [&]() {
                auto *dlg = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget());
                QVERIFY(dlg);
                const auto cleanup = qScopeGuard([dlg]() { if (dlg->isVisible()) dlg->reject(); });
                auto *sections = dlg->findChild<QListWidget *>(QStringLiteral("preferencesSections"));
                const Controls c{
                    dlg->findChild<QCheckBox *>(QStringLiteral("checkIPFilter")),
                    dlg->findChild<QLineEdit *>(QStringLiteral("textFilterPath")),
                    dlg->findChild<QCheckBox *>(QStringLiteral("checkIpFilterTrackers")),
                    dlg->findChild<QPushButton *>(QStringLiteral("banListButton")),
                    dlg->findChild<QToolButton *>(QStringLiteral("IpFilterRefreshBtn")),
                    dlg->findChild<QPushButton *>(QStringLiteral("preferencesApply"))
                };
                QVERIFY(sections && c.enabled && c.path && c.trackers && c.ban && c.reload && c.apply);
                sections->setCurrentRow(3);
                QTRY_VERIFY_WITH_TIMEOUT(c.trackers->isEnabled(), 6000);
                QVERIFY(!c.reload->isEnabled());
                exercise(c);
                finished = true;
            });
            prefs->trigger();
            QVERIFY(finished);
        };
        const auto editBanList = [&](QPushButton *button, const QString &initial,
                                     const QString &entry, bool ranges, bool accept, bool removeOld) {
            bool finished = false;
            QTimer::singleShot(0, &window, [&]() {
                auto *dialog = qobject_cast<RemoteBanListDialog *>(QApplication::activeModalWidget());
                QVERIFY(dialog);
                const auto cleanup = qScopeGuard([dialog]() { if (dialog->isVisible()) dialog->reject(); });
                auto *list = dialog->findChild<QTreeView *>(QStringLiteral("bannedIPList"));
                auto *ip = dialog->findChild<QLineEdit *>(QStringLiteral("txtIP"));
                auto *add = dialog->findChild<QPushButton *>(QStringLiteral("buttonBanIP"));
                auto *del = dialog->findChild<QPushButton *>(QStringLiteral("buttonDeleteIP"));
                auto *buttons = dialog->findChild<QDialogButtonBox *>(QStringLiteral("buttonBox"));
                QVERIFY(list && ip && add && del && buttons);
                QCOMPARE(list->model()->rowCount(), 1);
                QCOMPARE(list->model()->index(0, 0).data().toString(), initial);
                ip->setText(QStringLiteral("invalid"));
                QVERIFY(!add->isEnabled());
                ip->setText(QStringLiteral("203.0.113.0/24"));
                QCOMPARE(add->isEnabled(), ranges);
                ip->setText(entry);
                QVERIFY(add->isEnabled());
                if (removeOld)
                {
                    list->selectionModel()->select(list->model()->index(0, 0),
                        QItemSelectionModel::ClearAndSelect);
                    QVERIFY(del->isEnabled());
                    del->click();
                }
                add->click();
                QCOMPARE(list->model()->rowCount(), removeOld ? 1 : 2);
                buttons->button(accept ? QDialogButtonBox::Ok : QDialogButtonBox::Cancel)->click();
                finished = true;
            });
            button->click();
            QVERIFY(finished);
        };
        tabs->setCurrentIndex(0);
        for (bool save : {false, true})
        {
            withFilter([&](const Controls &c) {
                QVERIFY(c.enabled->isEnabled() && c.ban->isEnabled());
                QVERIFY(!c.enabled->isChecked() && !c.path->isEnabled() && c.trackers->isChecked());
                QCOMPARE(c.path->text(), QStringLiteral("/share/old.dat"));
                QVERIFY(!c.apply->isEnabled());
                editBanList(c.ban, QStringLiteral("192.0.2.1"), QStringLiteral("2001:0db8::1"),
                    false, false, false);
                QVERIFY(!c.apply->isEnabled());
                editBanList(c.ban, QStringLiteral("192.0.2.1"), QStringLiteral("2001:0db8::1"),
                    false, true, true);
                c.enabled->setChecked(true);
                QVERIFY(c.path->isEnabled() && !c.reload->isEnabled());
                c.path->setText(QStringLiteral("/share/new filter.dat"));
                c.trackers->setChecked(false);
                QVERIFY(c.apply->isEnabled());
                QCOMPARE(legacy.commandsFor("/api/v2/app/setPreferences"), 0);
                if (save)
                {
                    c.apply->click();
                    QTRY_VERIFY_WITH_TIMEOUT(c.enabled->isEnabled() && !c.apply->isEnabled(), 6000);
                }
            });
            if (!save) QCOMPARE(legacy.serverPreferences(), initialLegacy);
        }
        const QJsonObject expectedLegacy{
            {QStringLiteral("ip_filter_enabled"), true},
            {QStringLiteral("ip_filter_path"), QStringLiteral("/share/new filter.dat")},
            {QStringLiteral("ip_filter_trackers"), false},
            {QStringLiteral("banned_IPs"), QStringLiteral("2001:db8::1")}
        };
        QCOMPARE(legacy.serverPreferences(), expectedLegacy);
        QCOMPARE(modern.serverPreferences(), initialModern);
        const auto delta = [](DemoQbittorrentServer &server) {
            const QUrlQuery form(QString::fromUtf8(server.lastCommandBody("/api/v2/app/setPreferences")));
            return QJsonDocument::fromJson(form.queryItemValue(
                QStringLiteral("json"), QUrl::FullyDecoded).toUtf8()).object();
        };
        QCOMPARE(delta(legacy), expectedLegacy);
        tabs->setCurrentIndex(1);
        withFilter([&](const Controls &c) {
            QVERIFY(c.enabled->isChecked() && c.path->isEnabled() && c.ban->isEnabled());
            QCOMPARE(c.path->text(), QStringLiteral("/share/modern.p2p"));
            editBanList(c.ban, QStringLiteral("198.51.100.1 - 198.51.100.9"),
                QStringLiteral("203.0.113.0/24"), true, true, false);
            c.apply->click();
            QTRY_VERIFY_WITH_TIMEOUT(c.ban->isEnabled() && !c.apply->isEnabled(), 6000);
        });
        const QJsonObject modernDelta{{QStringLiteral("banned_IPs"),
            QStringLiteral("198.51.100.1 - 198.51.100.9\n203.0.113.0 - 203.0.113.255")}};
        QCOMPARE(delta(modern), modernDelta);
        QCOMPARE(legacy.serverPreferences(), expectedLegacy);
        withFilter([&](const Controls &c) {
            QVERIFY(c.ban->isEnabled());
            // A stale replacement must be rejected before any POST.
            bool edited = false;
            QTimer::singleShot(0, &window, [&]() {
                auto *dialog = qobject_cast<RemoteBanListDialog *>(QApplication::activeModalWidget());
                QVERIFY(dialog);
                const auto cleanup = qScopeGuard([dialog]() { if (dialog->isVisible()) dialog->reject(); });
                auto *list = dialog->findChild<QTreeView *>(QStringLiteral("bannedIPList"));
                auto *del = dialog->findChild<QPushButton *>(QStringLiteral("buttonDeleteIP"));
                auto *buttons = dialog->findChild<QDialogButtonBox *>(QStringLiteral("buttonBox"));
                QVERIFY(list && del && buttons);
                QCOMPARE(list->model()->rowCount(), 2);
                list->selectionModel()->select(list->model()->index(0, 0),
                    QItemSelectionModel::ClearAndSelect);
                del->click();
                buttons->button(QDialogButtonBox::Ok)->click();
                edited = true;
            });
            c.ban->click();
            QVERIFY(edited && c.apply->isEnabled());
            QJsonObject concurrent = modern.serverPreferences();
            concurrent.insert(QStringLiteral("banned_IPs"), QStringLiteral("192.0.2.55"));
            modern.setServerPreferences(concurrent);
            const int writes = modern.commandsFor("/api/v2/app/setPreferences");
            QTimer closer;
            closer.setInterval(20);
            connect(&closer, &QTimer::timeout, &window, []() {
                if (auto *warning = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                    warning->accept();
            });
            closer.start();
            c.apply->click();
            QTRY_VERIFY_WITH_TIMEOUT(c.apply->isEnabled(), 6000);
            closer.stop();
            QCOMPARE(modern.commandsFor("/api/v2/app/setPreferences"), writes);
            QCOMPARE(modern.serverPreferences(), concurrent);
        });
        modern.setServerPreferences({
            {QStringLiteral("ip_filter_enabled"), true},
            {QStringLiteral("ip_filter_path"), false},
            {QStringLiteral("ip_filter_trackers"), true},
            {QStringLiteral("banned_IPs"), QStringLiteral("invalid")}
        });
        withFilter([](const Controls &c) {
            QVERIFY(c.enabled->isEnabled() && c.trackers->isEnabled());
            QVERIFY(!c.path->isEnabled() && !c.ban->isEnabled() && !c.reload->isEnabled());
            QVERIFY(!c.apply->isEnabled());
        });
    }

    void legacyBackupDestinationsUseNativeControls()
    {
        DemoQbittorrentServer legacy(true), modern(true);
        modern.enableModernResponses();
        const QJsonObject initialLegacy{
            {QStringLiteral("export_dir"), QStringLiteral("/share/old")},
            {QStringLiteral("export_dir_fin"), QString()}
        };
        const QJsonObject initialModern{
            {QStringLiteral("torrent_files_backup_enabled"), false},
            {QStringLiteral("torrent_files_backup_dir"), QStringLiteral("/share/modern")},
            {QStringLiteral("torrent_files_finished_backup_dir_enabled"), true},
            {QStringLiteral("torrent_files_finished_backup_dir"), QStringLiteral("/share/completed")},
            {QStringLiteral("remove_torrent_file_backup"), false},
            // A transitional server exposing both families must use modern.
            {QStringLiteral("export_dir"), QStringLiteral("/share/deprecated")},
            {QStringLiteral("export_dir_fin"), QStringLiteral("/share/deprecated-finished")}
        };
        legacy.setServerPreferences(initialLegacy);
        modern.setServerPreferences(initialModern);
        QVERIFY(legacy.start() && modern.start());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("prefs.ini")), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        openConnectDialog(window, legacy.url());
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(QStringLiteral("v5.1.4")), 6000);
        openConnectDialog(window, modern.url(), false, true, true, QStringLiteral("Modern seedbox"));
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(QStringLiteral("v5.3.0")), 6000);
        auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("serverTabs"));
        auto *prefs = window.findChild<QAction *>(QStringLiteral("actionOptions"));
        QVERIFY(tabs && prefs);
        tabs->setCurrentIndex(0);

        const auto editLegacy = [&](bool save) {
            bool finished = false;
            QTimer::singleShot(0, &window, [&]() {
                auto *dlg = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget());
                QVERIFY(dlg);
                const auto cleanup = qScopeGuard([dlg]() { if (dlg->isVisible()) dlg->reject(); });
                auto *sections = dlg->findChild<QListWidget *>(QStringLiteral("preferencesSections"));
                auto *backup = dlg->findChild<QCheckBox *>(QStringLiteral("backupDirCheckBox"));
                auto *path = dlg->findChild<QLineEdit *>(QStringLiteral("backupDirPathEdit"));
                auto *complete = dlg->findChild<QCheckBox *>(QStringLiteral("finishedBackupDirCheckBox"));
                auto *dest = dlg->findChild<QLineEdit *>(QStringLiteral("finishedBackupDirPathEdit"));
                auto *remove = dlg->findChild<QCheckBox *>(QStringLiteral("removeBackupTorrentFileCheckBox"));
                auto *apply = dlg->findChild<QPushButton *>(QStringLiteral("preferencesApply"));
                QVERIFY(sections && backup && path && complete && dest && remove && apply);
                sections->setCurrentRow(2);
                QTRY_VERIFY_WITH_TIMEOUT(backup->isEnabled() && complete->isEnabled(), 6000);
                QCOMPARE(backup->text(), QStringLiteral("Copy .torrent files to:"));
                QCOMPARE(complete->text(), QStringLiteral("Copy .torrent files for finished downloads to:"));
                QVERIFY(backup->isChecked() && path->isEnabled());
                QCOMPARE(path->text(), QStringLiteral("/share/old"));
                QVERIFY(!complete->isChecked() && !dest->isEnabled() && !remove->isEnabled());
                backup->click();
                QVERIFY(!path->isEnabled());
                complete->click();
                QVERIFY(dest->isEnabled() && apply->isEnabled());
                if (save)
                {
                    // A checked but empty path must not silently disable.
                    bool warned = false;
                    QTimer::singleShot(0, dlg, [&]() {
                        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                        {
                            warned = box->windowTitle() == QStringLiteral("Invalid server directory");
                            box->accept();
                        }
                    });
                    apply->click();
                    QVERIFY(warned);
                    QCOMPARE(legacy.commandsFor("/api/v2/app/setPreferences"), 0);
                }
                dest->setText(QStringLiteral("/share/new-finished/"));
                if (save)
                {
                    apply->click();
                    QTRY_VERIFY_WITH_TIMEOUT(backup->isEnabled() && !apply->isEnabled(), 6000);
                    QVERIFY(!backup->isChecked() && path->text().isEmpty());
                    QCOMPARE(dest->text(), QStringLiteral("/share/new-finished"));
                    QVERIFY(complete->isChecked() && !remove->isEnabled());
                }
                finished = true;
            });
            prefs->trigger();
            QVERIFY(finished);
        };
        editLegacy(false);
        QCOMPARE(legacy.serverPreferences(), initialLegacy);
        QCOMPARE(legacy.commandsFor("/api/v2/app/setPreferences"), 0);
        editLegacy(true);
        const QJsonObject expectedLegacy{
            {QStringLiteral("export_dir"), QString()},
            {QStringLiteral("export_dir_fin"), QStringLiteral("/share/new-finished")}
        };
        QCOMPARE(legacy.serverPreferences(), expectedLegacy);
        QCOMPARE(legacy.commandsFor("/api/v2/app/setPreferences"), 1);
        QCOMPARE(modern.serverPreferences(), initialModern);

        tabs->setCurrentIndex(1);
        bool finished = false;
        QTimer::singleShot(0, &window, [&]() {
            auto *dlg = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget());
            QVERIFY(dlg);
            const auto cleanup = qScopeGuard([dlg]() { if (dlg->isVisible()) dlg->reject(); });
            auto *sections = dlg->findChild<QListWidget *>(QStringLiteral("preferencesSections"));
            auto *backup = dlg->findChild<QCheckBox *>(QStringLiteral("backupDirCheckBox"));
            auto *path = dlg->findChild<QLineEdit *>(QStringLiteral("backupDirPathEdit"));
            auto *remove = dlg->findChild<QCheckBox *>(QStringLiteral("removeBackupTorrentFileCheckBox"));
            auto *apply = dlg->findChild<QPushButton *>(QStringLiteral("preferencesApply"));
            QVERIFY(sections && backup && path && remove && apply);
            sections->setCurrentRow(2);
            QTRY_VERIFY_WITH_TIMEOUT(backup->isEnabled() && remove->isEnabled(), 6000);
            QCOMPARE(backup->text(), QStringLiteral("Store backup in:"));
            QVERIFY(!backup->isChecked() && !path->isEnabled());
            QCOMPARE(path->text(), QStringLiteral("/share/modern"));
            backup->click();
            QVERIFY(path->isEnabled());
            remove->click();
            apply->click();
            QTRY_VERIFY_WITH_TIMEOUT(backup->isEnabled() && !apply->isEnabled(), 6000);
            finished = true;
        });
        prefs->trigger();
        QVERIFY(finished);
        QJsonObject expectedModern = initialModern;
        expectedModern.insert(QStringLiteral("torrent_files_backup_enabled"), true);
        expectedModern.insert(QStringLiteral("remove_torrent_file_backup"), true);
        QCOMPARE(modern.serverPreferences(), expectedModern);
        QCOMPARE(modern.commandsFor("/api/v2/app/setPreferences"), 1);
        QCOMPARE(legacy.serverPreferences(), expectedLegacy);
    }

    void downloadsBackupsAndWatchedFoldersUseSelectedServer()
    {
        DemoQbittorrentServer first(true), second(true);
        const QJsonObject watched{
            {QStringLiteral("/share/watch-existing"), 0},
            {QStringLiteral("/share/watch-remove"), 1}
        };
        const QJsonObject initial{
            {QStringLiteral("torrent_files_backup_enabled"), false},
            {QStringLiteral("torrent_files_backup_dir"), QStringLiteral("/share/old-backup")},
            {QStringLiteral("torrent_files_finished_backup_dir_enabled"), false},
            {QStringLiteral("torrent_files_finished_backup_dir"), QStringLiteral("/share/finished")},
            {QStringLiteral("remove_torrent_file_backup"), false},
            {QStringLiteral("scan_dirs"), watched}
        };
        const QJsonObject other{{QStringLiteral("scan_dirs"), QJsonObject{
            {QStringLiteral("/other/watch"), 1}}}};
        first.setServerPreferences(initial);
        second.setServerPreferences(other);
        QVERIFY(first.start() && second.start());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("prefs.ini")), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        openConnectDialog(window, first.url());
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(
            QStringLiteral("Connected to Demo seedbox")), 6000);
        openConnectDialog(window, second.url(), false, true, true, QStringLiteral("Second seedbox"));
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(
            QStringLiteral("Connected to Second seedbox")), 6000);
        auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("serverTabs"));
        auto *prefs = window.findChild<QAction *>(QStringLiteral("actionOptions"));
        QVERIFY(tabs && prefs);
        tabs->setCurrentIndex(0);

        const auto edit = [&](bool save) {
            bool finished = false;
            QTimer::singleShot(0, &window, [&]() {
                auto *dlg = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget());
                QVERIFY(dlg);
                const auto cleanup = qScopeGuard([dlg]() { if (dlg->isVisible()) dlg->reject(); });
                auto *sections = dlg->findChild<QListWidget *>(QStringLiteral("preferencesSections"));
                auto *backup = dlg->findChild<QCheckBox *>(QStringLiteral("backupDirCheckBox"));
                auto *backupPath = dlg->findChild<QLineEdit *>(QStringLiteral("backupDirPathEdit"));
                auto *complete = dlg->findChild<QCheckBox *>(QStringLiteral("finishedBackupDirCheckBox"));
                auto *completePath = dlg->findChild<QLineEdit *>(QStringLiteral("finishedBackupDirPathEdit"));
                auto *removeBackup = dlg->findChild<QCheckBox *>(QStringLiteral("removeBackupTorrentFileCheckBox"));
                auto *view = dlg->findChild<QTreeView *>(QStringLiteral("scanFoldersView"));
                auto *add = dlg->findChild<QPushButton *>(QStringLiteral("addWatchedFolderButton"));
                auto *options = dlg->findChild<QPushButton *>(QStringLiteral("editWatchedFolderButton"));
                auto *remove = dlg->findChild<QPushButton *>(QStringLiteral("removeWatchedFolderButton"));
                auto *apply = dlg->findChild<QPushButton *>(QStringLiteral("preferencesApply"));
                QVERIFY(sections && backup && backupPath && complete && completePath && removeBackup
                    && view && add && options && remove && apply);
                sections->setCurrentRow(2);
                QTRY_VERIFY_WITH_TIMEOUT(backup->isEnabled() && add->isEnabled(), 6000);
                QCOMPARE(view->model()->rowCount(), 2);
                QVERIFY(!backupPath->isEnabled() && !completePath->isEnabled());
                backup->click();
                complete->click();
                QVERIFY(backupPath->isEnabled() && completePath->isEnabled());
                backupPath->setText(QStringLiteral("/share/new-backup"));
                completePath->setText(QStringLiteral("/share/new-finished"));
                removeBackup->click();

                // Add a server-side watch folder with an explicit destination.
                bool edited = false;
                QTimer::singleShot(0, dlg, [&]() {
                    auto *editor = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                    QVERIFY(editor && editor != dlg);
                    const auto closeEditor = qScopeGuard([editor]() { if (editor->isVisible()) editor->reject(); });
                    auto *path = editor->findChild<QLineEdit *>(QStringLiteral("watchedFolderPath"));
                    auto *mode = editor->findChild<QComboBox *>(QStringLiteral("watchedFolderDestinationMode"));
                    auto *dest = editor->findChild<QLineEdit *>(QStringLiteral("watchedFolderDestinationPath"));
                    auto *buttons = editor->findChild<QDialogButtonBox *>();
                    QVERIFY(path && mode && dest && buttons);
                    path->setText(QStringLiteral("/share/watch-new"));
                    mode->setCurrentIndex(mode->findData(2));
                    dest->setText(QStringLiteral("/share/downloads"));
                    buttons->button(QDialogButtonBox::Ok)->click();
                    edited = true;
                });
                add->click();
                QVERIFY(edited);
                QCOMPARE(view->model()->rowCount(), 3);
                for (int row = 0; row < view->model()->rowCount(); ++row)
                    if (view->model()->index(row, 0).data().toString() == QStringLiteral("/share/watch-remove"))
                        view->selectionModel()->select(view->model()->index(row, 0),
                            QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
                QVERIFY(remove->isEnabled() && options->isEnabled());
                remove->click();
                QCOMPARE(view->model()->rowCount(), 2);
                if (save)
                {
                    bool confirmed = false;
                    QTimer modalDriver;
                    connect(&modalDriver, &QTimer::timeout, dlg, [&]() {
                        if (auto *question = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                        {
                            if (question->windowTitle() == QStringLiteral("Change watched folders"))
                            {
                                confirmed = true;
                                question->button(QMessageBox::Yes)->click();
                            }
                            else
                            {
                                QTest::qFail(qPrintable(question->text()), __FILE__, __LINE__);
                                question->reject();
                            }
                        }
                    });
                    modalDriver.start(20);
                    QVERIFY(apply->isEnabled());
                    apply->click();
                    QVERIFY(confirmed);
                    QTRY_VERIFY_WITH_TIMEOUT(backup->isEnabled() && !apply->isEnabled(), 6000);
                }
                finished = true;
            });
            prefs->trigger();
            QVERIFY(finished);
        };
        edit(false);
        QCOMPARE(first.serverPreferences(), initial);
        QCOMPARE(first.commandsFor("/api/v2/app/setPreferences"), 0);
        edit(true);
        QJsonObject expected = initial;
        expected.insert(QStringLiteral("torrent_files_backup_enabled"), true);
        expected.insert(QStringLiteral("torrent_files_backup_dir"), QStringLiteral("/share/new-backup"));
        expected.insert(QStringLiteral("torrent_files_finished_backup_dir_enabled"), true);
        expected.insert(QStringLiteral("torrent_files_finished_backup_dir"), QStringLiteral("/share/new-finished"));
        expected.insert(QStringLiteral("remove_torrent_file_backup"), true);
        expected.insert(QStringLiteral("scan_dirs"), QJsonObject{
            {QStringLiteral("/share/watch-existing"), 0},
            {QStringLiteral("/share/watch-new"), QStringLiteral("/share/downloads")}});
        QCOMPARE(first.serverPreferences(), expected);
        QCOMPARE(first.commandsFor("/api/v2/app/setPreferences"), 1);
        QCOMPARE(second.serverPreferences(), other);
        QCOMPARE(second.commandsFor("/api/v2/app/setPreferences"), 0);
        tabs->setCurrentIndex(1);
        bool checked = false;
        QTimer::singleShot(0, &window, [&]() {
            auto *dlg = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget());
            QVERIFY(dlg);
            const auto cleanup = qScopeGuard([dlg]() { if (dlg->isVisible()) dlg->reject(); });
            auto *view = dlg->findChild<QTreeView *>(QStringLiteral("scanFoldersView"));
            auto *backup = dlg->findChild<QCheckBox *>(QStringLiteral("backupDirCheckBox"));
            QVERIFY(view && backup);
            QTRY_VERIFY_WITH_TIMEOUT(view->isEnabled(), 6000);
            QVERIFY(!backup->isEnabled());
            QCOMPARE(view->model()->rowCount(), 1);
            QCOMPARE(view->model()->index(0, 0).data().toString(), QStringLiteral("/other/watch"));
            checked = true;
        });
        prefs->trigger();
        QVERIFY(checked);
    }

    void downloadsAdditionDefaultsFollowSelectedServer()
    {
        DemoQbittorrentServer first(true), second(true);
        const QJsonObject initial{
            {QStringLiteral("torrent_content_layout"), QStringLiteral("Original")},
            {QStringLiteral("add_to_top_of_queue"), false},
            {QStringLiteral("add_stopped_enabled"), true},
            {QStringLiteral("torrent_stop_condition"), QStringLiteral("None")},
            {QStringLiteral("auto_tmm_enabled"), false}
        };
        const QJsonObject other{
            {QStringLiteral("torrent_content_layout"), QStringLiteral("Subfolder")},
            {QStringLiteral("add_to_top_of_queue"), true},
            {QStringLiteral("add_stopped_enabled"), false},
            {QStringLiteral("torrent_stop_condition"), QStringLiteral("FilesChecked")},
            {QStringLiteral("auto_tmm_enabled"), true}
        };
        first.setServerPreferences(initial);
        second.setServerPreferences(other);
        QVERIFY(first.start() && second.start());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("prefs.ini")), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        openConnectDialog(window, first.url());
        auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("serverTabs"));
        QVERIFY(tabs);
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(
            QStringLiteral("Connected to Demo seedbox")), 6000);
        openConnectDialog(window, second.url(), false, true, true, QStringLiteral("Second seedbox"));
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(
            QStringLiteral("Connected to Second seedbox")), 6000);
        tabs->setCurrentIndex(0);
        auto *prefs = window.findChild<QAction *>(QStringLiteral("actionOptions"));
        auto *link = window.findChild<QAction *>(QStringLiteral("actionDownloadFromURL"));
        QVERIFY(prefs && link);
        bool saved = false;
        QTimer::singleShot(0, &window, [&]() {
            auto *dialog = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            const auto cleanup = qScopeGuard([dialog]() {
                if (dialog->isVisible()) dialog->reject();
            });
            auto *sections = dialog->findChild<QListWidget *>(QStringLiteral("preferencesSections"));
            auto *layout = dialog->findChild<QComboBox *>(QStringLiteral("contentLayoutComboBox"));
            auto *top = dialog->findChild<QCheckBox *>(QStringLiteral("checkAddToQueueTop"));
            auto *stopped = dialog->findChild<QCheckBox *>(QStringLiteral("checkAddStopped"));
            auto *stop = dialog->findChild<QComboBox *>(QStringLiteral("stopConditionComboBox"));
            auto *apply = dialog->findChild<QPushButton *>(QStringLiteral("preferencesApply"));
            QVERIFY(sections && layout && top && stopped && stop && apply);
            sections->setCurrentRow(2);
            QTRY_VERIFY_WITH_TIMEOUT(layout->isEnabled(), 6000);
            QCOMPARE(layout->currentText(), QStringLiteral("Original"));
            QVERIFY(!top->isChecked() && stopped->isChecked());
            QVERIFY(!stop->isEnabled());
            stopped->click();
            QVERIFY(stop->isEnabled());
            layout->setCurrentIndex(2);
            top->click();
            stop->setCurrentIndex(1);
            QVERIFY(apply->isEnabled());
            apply->click();
            QTRY_VERIFY_WITH_TIMEOUT(layout->isEnabled() && !apply->isEnabled(), 6000);
            saved = true;
        });
        prefs->trigger();
        QVERIFY(saved);
        QJsonObject expected = initial;
        expected.insert(QStringLiteral("torrent_content_layout"), QStringLiteral("NoSubfolder"));
        expected.insert(QStringLiteral("add_to_top_of_queue"), true);
        expected.insert(QStringLiteral("add_stopped_enabled"), false);
        expected.insert(QStringLiteral("torrent_stop_condition"), QStringLiteral("MetadataReceived"));
        QCOMPARE(first.serverPreferences(), expected);
        QCOMPARE(second.serverPreferences(), other);
        QCOMPARE(second.commandsFor("/api/v2/app/setPreferences"), 0);

        const auto add = [&](bool switchTarget) {
            bool accepted = false;
            QTimer::singleShot(0, &window, [&]() {
                auto *dialog = qobject_cast<RemoteAddTorrentDialog *>(QApplication::activeModalWidget());
                QVERIFY(dialog);
                const auto cleanup = qScopeGuard([dialog]() {
                    if (dialog->isVisible()) dialog->reject();
                });
                auto *buttons = dialog->findChild<QDialogButtonBox *>(QStringLiteral("buttonBox"));
                auto *layout = dialog->findChild<QComboBox *>(QStringLiteral("contentLayoutComboBox"));
                auto *top = dialog->findChild<QCheckBox *>(QStringLiteral("addToQueueTopCheckBox"));
                auto *start = dialog->findChild<QCheckBox *>(QStringLiteral("startTorrentCheckBox"));
                auto *stop = dialog->findChild<QComboBox *>(QStringLiteral("stopConditionComboBox"));
                auto *tmm = dialog->findChild<QComboBox *>(QStringLiteral("comboTMM"));
                auto *selector = dialog->findChild<QComboBox *>(QStringLiteral("addTorrentServerSelector"));
                auto *links = dialog->findChild<QPlainTextEdit *>(QStringLiteral("torrentLinks"));
                QVERIFY(buttons && layout && top && start && stop && tmm && selector && links);
                auto *ok = buttons->button(QDialogButtonBox::Ok);
                QVERIFY(ok);
                QTRY_VERIFY_WITH_TIMEOUT(ok->isEnabled(), 6000);
                QCOMPARE(layout->currentIndex(), 2);
                QVERIFY(top->isChecked() && start->isChecked() && stop->isEnabled());
                QCOMPARE(stop->currentData().toString(), QStringLiteral("MetadataReceived"));
                QCOMPARE(tmm->currentIndex(), 0);
                if (switchTarget)
                {
                    selector->setCurrentIndex(selector->findText(QStringLiteral("Second seedbox")));
                    QTRY_VERIFY_WITH_TIMEOUT(ok->isEnabled(), 6000);
                    QCOMPARE(layout->currentIndex(), 1);
                    QVERIFY(top->isChecked() && start->isChecked());
                    QCOMPARE(stop->currentData().toString(), QStringLiteral("FilesChecked"));
                    QCOMPARE(tmm->currentIndex(), 1);
                    // A user override must win over the server default.
                    top->click();
                    stop->setCurrentIndex(stop->findData(QStringLiteral("None")));
                }
                links->setPlainText(QStringLiteral(
                    "magnet:?xt=urn:btih:fedcba9876543210fedcba9876543210fedcba98"));
                ok->click();
                accepted = !dialog->isVisible();
            });
            link->trigger();
            QVERIFY(accepted);
        };
        add(false);
        QTRY_COMPARE_WITH_TIMEOUT(first.commandsFor("/api/v2/torrents/add"), 1, 6000);
        QCOMPARE(second.commandsFor("/api/v2/torrents/add"), 0);
        QUrlQuery form(QString::fromUtf8(first.lastCommandBody("/api/v2/torrents/add")));
        QCOMPARE(form.queryItemValue(QStringLiteral("contentLayout")), QStringLiteral("NoSubfolder"));
        QCOMPARE(form.queryItemValue(QStringLiteral("addToTopOfQueue")), QStringLiteral("true"));
        QCOMPARE(form.queryItemValue(QStringLiteral("stopped")), QStringLiteral("false"));
        QCOMPARE(form.queryItemValue(QStringLiteral("stopCondition")), QStringLiteral("MetadataReceived"));
        add(true);
        QTRY_COMPARE_WITH_TIMEOUT(second.commandsFor("/api/v2/torrents/add"), 1, 6000);
        QCOMPARE(first.commandsFor("/api/v2/torrents/add"), 1);
        form = QUrlQuery(QString::fromUtf8(second.lastCommandBody("/api/v2/torrents/add")));
        QCOMPARE(form.queryItemValue(QStringLiteral("contentLayout")), QStringLiteral("Subfolder"));
        QCOMPARE(form.queryItemValue(QStringLiteral("addToTopOfQueue")), QStringLiteral("false"));
        QCOMPARE(form.queryItemValue(QStringLiteral("stopCondition")), QStringLiteral("None"));
        QCOMPARE(form.queryItemValue(QStringLiteral("autoTMM")), QStringLiteral("true"));
        QCOMPARE(second.serverPreferences(), other);
    }

    void statusBarFieldsStayFixedDuringAuthenticatedRateUpdates()
    {
        DemoQbittorrentServer server(true);
        server.setStatusRates({
            {QStringLiteral("dl_info_speed"), 0},
            {QStringLiteral("up_info_speed"), 0},
            {QStringLiteral("free_space_on_disk"), 1024.0 * 1024 * 1024}
        });
        QVERIFY(server.start());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("status.ini")), QSettings::IniFormat);
        settings.setValue(QStringLiteral("Preferences/Behavior/ShowServerFreeSpace"), true);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.resize(1400, 800);
        window.show();
        QTRY_VERIFY(window.isVisible());
        auto *bar = window.statusBar();
        auto *count = bar->findChild<QLabel *>(QStringLiteral("visibleTorrentCount"));
        auto *down = bar->findChild<QLabel *>(QStringLiteral("remoteDownloadRate"));
        auto *up = bar->findChild<QLabel *>(QStringLiteral("remoteUploadRate"));
        auto *free = bar->findChild<QLabel *>(QStringLiteral("remoteServerFreeSpace"));
        auto *sep1 = bar->findChild<QLabel *>(QStringLiteral("torrentCountSeparator"));
        auto *sep2 = bar->findChild<QLabel *>(QStringLiteral("transferRateSeparator"));
        auto *sep3 = bar->findChild<QLabel *>(QStringLiteral("freeSpaceSeparator"));
        QVERIFY(count && down && up && free && sep1 && sep2 && sep3);
        QCOMPARE(down->text(), QStringLiteral("D: —"));
        QCOMPARE(up->text(), QStringLiteral("U: —"));
        openConnectDialog(window, server.url());
        QTRY_COMPARE_WITH_TIMEOUT(down->text(), QStringLiteral("D: 0 B/s"), 6000);
        QCOMPARE(up->text(), QStringLiteral("U: 0 B/s"));
        QTRY_VERIFY(free->text().endsWith(QStringLiteral(" GiB")));
        QCoreApplication::processEvents();
        const QList<QLabel *> fields{count, sep1, down, sep2, up, sep3, free};
        // The requirement is stable horizontal positions and reserved widths.
        // Qt can recalculate the native status bar's height after a font
        // change; that vertical adjustment is not a transfer-rate shift.
        const auto horizontalGeometry = [](const QLabel *label) {
            return QPair<int, int>{label->x(), label->width()};
        };
        QList<QPair<int, int>> initial;
        for (QLabel *label : fields)
        {
            QVERIFY(label->isVisible());
            initial.append(horizontalGeometry(label));
        }
        for (QLabel *sep : {sep1, sep2, sep3})
            QCOMPARE(sep->text(), QStringLiteral("|"));

        server.setStatusRates({
            {QStringLiteral("dl_info_speed"), 1023.5 * 1024 * 1024},
            {QStringLiteral("up_info_speed"), 42.1 * 1024},
            {QStringLiteral("free_space_on_disk"), 5.9 * 1024 * 1024 * 1024 * 1024}
        });
        const QLocale locale;
        QTRY_COMPARE_WITH_TIMEOUT(down->text(),
            QStringLiteral("D: %1 MiB/s").arg(locale.toString(1023.5, 'f', 1)), 6000);
        QCOMPARE(up->text(), QStringLiteral("U: %1 KiB/s").arg(locale.toString(42.1, 'f', 1)));
        QCOMPARE(free->text(), QStringLiteral("Free: %1 TiB").arg(locale.toString(5.9, 'f', 1)));
        QCoreApplication::processEvents();
        for (int i = 0; i < fields.size(); ++i)
            QCOMPARE(horizontalGeometry(fields.at(i)), initial.at(i));
        for (QLabel *label : {down, up, free})
            QVERIFY(label->fontMetrics().horizontalAdvance(label->text()) <= label->width());

        // Delta synchronization of only the download speed must preserve the
        // upload and free-space values and all label positions.
        server.setStatusRates({{QStringLiteral("dl_info_speed"), 1}});
        QTRY_COMPARE_WITH_TIMEOUT(down->text(), QStringLiteral("D: 1 B/s"), 6000);
        QCOMPARE(up->text(), QStringLiteral("U: %1 KiB/s").arg(locale.toString(42.1, 'f', 1)));
        QCOMPARE(free->text(), QStringLiteral("Free: %1 TiB").arg(locale.toString(5.9, 'f', 1)));
        QCoreApplication::processEvents();
        for (int i = 0; i < fields.size(); ++i)
            QCOMPARE(horizontalGeometry(fields.at(i)), initial.at(i));

        auto *increase = window.findChild<QAction *>(QStringLiteral("fontSizeIncrease"));
        QVERIFY(increase);
        if (increase->isEnabled())
        {
            const int oldPointSize = window.fontPointSize();
            increase->trigger();
            QCOMPARE(window.fontPointSize(), oldPointSize + 1);
            // Font metrics can differ across pixel-sized and point-sized
            // platform fonts. Check actual readable capacity, not an assumed
            // monotonic pixel width when the font preference changes.
            QTRY_VERIFY(down->width() >= down->fontMetrics().horizontalAdvance(
                QStringLiteral("D: %1 MiB/s").arg(locale.toString(1023.5, 'f', 1))));
            QTRY_VERIFY(up->width() >= up->fontMetrics().horizontalAdvance(
                QStringLiteral("U: %1 MiB/s").arg(locale.toString(1023.5, 'f', 1))));
            QCoreApplication::processEvents();
            QList<QPair<int, int>> enlarged;
            for (QLabel *label : fields)
                enlarged.append(horizontalGeometry(label));

            server.setStatusRates({
                {QStringLiteral("dl_info_speed"), 1023.5 * 1024 * 1024},
                {QStringLiteral("up_info_speed"), 512.5 * 1024 * 1024}
            });
            QTRY_COMPARE_WITH_TIMEOUT(down->text(),
                QStringLiteral("D: %1 MiB/s").arg(locale.toString(1023.5, 'f', 1)), 6000);
            QCOMPARE(up->text(), QStringLiteral("U: %1 MiB/s").arg(locale.toString(512.5, 'f', 1)));
            QCoreApplication::processEvents();
            for (int i = 0; i < fields.size(); ++i)
                QCOMPARE(horizontalGeometry(fields.at(i)), enlarged.at(i));
            for (QLabel *label : {down, up, free})
                QVERIFY(label->fontMetrics().horizontalAdvance(label->text()) <= label->width());
            auto *reset = window.findChild<QAction *>(QStringLiteral("fontSizeReset"));
            QVERIFY(reset);
            reset->trigger();
        }
    }

    void nativeStatusBarShowsSelectedServerTotalsLimitsAndExternalAddresses()
    {
        DemoQbittorrentServer first(true), second(true);
        first.setStatusRates({{"dl_info_speed", 2048}, {"up_info_speed", 1024},
            {"dl_rate_limit", 4096}, {"up_rate_limit", 0},
            {"dl_info_data", 1048576}, {"up_info_data", 2097152},
            {"dht_nodes", 42}, {"last_external_address_v4", "192.0.2.10"},
            {"last_external_address_v6", "2001:db8::10"}});
        first.setServerPreferences({{"dht", true}});
        second.setServerPreferences({{"dht", false}});
        second.setStatusRates({{"dl_info_speed", 0}, {"up_info_speed", 0}, {"dht_nodes", 0}});
        QVERIFY(first.start() && second.start());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath("native-status.ini"), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.resize(1600, 850);
        window.show();
        auto *bar = window.statusBar();
        auto *down = bar->findChild<QLabel *>("remoteDownloadRate");
        auto *up = bar->findChild<QLabel *>("remoteUploadRate");
        auto *dht = bar->findChild<QLabel *>("remoteDhtNodes");
        auto *ip = bar->findChild<QLabel *>("remoteExternalIP");
        auto *options = window.findChild<QAction *>("actionOptions");
        QVERIFY(down && up && dht && ip && options);
        QVERIFY(ip->isHidden() && dht->isHidden());
        openConnectDialog(window, first.url());
        QTRY_VERIFY_WITH_TIMEOUT(down->text().contains("MiB"), 6000);
        const QLocale locale;
        QCOMPARE(down->text(), QStringLiteral("D: %1 KiB/s [%2 KiB/s] (%3 MiB)")
            .arg(locale.toString(2.0, 'f', 1), locale.toString(4.0, 'f', 1), locale.toString(1.0, 'f', 1)));
        QCOMPARE(up->text(), QStringLiteral("U: %1 KiB/s (%2 MiB)")
            .arg(locale.toString(1.0, 'f', 1), locale.toString(2.0, 'f', 1)));
        QVERIFY(up->toolTip().contains("Unlimited"));
        QCOMPARE(dht->text(), QStringLiteral("DHT: 42 nodes"));
        QVERIFY(dht->isVisible() && ip->isHidden());
        const auto configureIp = [&](bool checked, bool apply) {
            bool exercised = false;
            QTimer::singleShot(0, &window, [&] {
                auto *dialog = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget());
                if (!dialog) return;
                const auto cleanup = qScopeGuard([dialog] { if (dialog->isVisible()) dialog->reject(); });
                auto *field = dialog->findChild<QCheckBox *>("checkBoxExternalIPStatusBar");
                auto *nav = dialog->findChild<QListWidget *>("preferencesSections");
                auto *buttons = dialog->findChild<QDialogButtonBox *>("buttonBox");
                QVERIFY(field && nav && buttons);
                nav->setCurrentRow(1);
                QVERIFY(field->isEnabled());
                field->setChecked(checked);
                if (apply)
                {
                    QVERIFY(buttons->button(QDialogButtonBox::Apply)->isEnabled());
                    buttons->button(QDialogButtonBox::Apply)->click();
                    QVERIFY(!buttons->button(QDialogButtonBox::Apply)->isEnabled());
                }
                exercised = true;
                dialog->reject();
            });
            options->trigger();
            QVERIFY(exercised);
        };
        configureIp(true, false);
        QVERIFY(ip->isHidden());
        configureIp(true, true);
        QVERIFY(ip->isVisible());
        QCOMPARE(ip->text(), QStringLiteral("External IPs: 192.0.2.10, 2001:db8::10"));
        QVERIFY(settings.value("Preferences/Behavior/ShowExternalIP").toBool());
        QCoreApplication::processEvents();
        const int width = down->width();
        first.setStatusRates({{"dl_info_speed", 4096}, {"dl_info_data", 3145728}});
        QTRY_VERIFY_WITH_TIMEOUT(down->text().endsWith(locale.toString(3.0, 'f', 1) + " MiB)"), 6000);
        QCOMPARE(down->width(), width);
        // Do not reserve an entire invisible limit slot for unlimited
        // rates. Explicit limit-mode changes may reflow the layout once;
        // subsequent speed changes must leave the new layout stationary.
        const int upWidth = up->width();
        first.setStatusRates({{"dl_rate_limit", 0}, {"up_rate_limit", 8192}});
        QTRY_VERIFY_WITH_TIMEOUT(up->text().contains("["), 6000);
        QVERIFY(!down->text().contains("["));
        QVERIFY(down->width() < width);
        QVERIFY(up->width() > upWidth);
        QVERIFY(down->toolTip().contains("Unlimited"));
        QCoreApplication::processEvents();
        const int compactDownWidth = down->width(), limitedUpWidth = up->width();
        const int downX = down->x(), upX = up->x();
        first.setStatusRates({{"dl_info_speed", 8192}, {"up_info_speed", 4096}});
        QTRY_VERIFY_WITH_TIMEOUT(down->text().startsWith(QStringLiteral("D: %1 KiB/s").arg(locale.toString(8.0, 'f', 1))), 6000);
        QCoreApplication::processEvents();
        QCOMPARE(down->width(), compactDownWidth);
        QCOMPARE(up->width(), limitedUpWidth);
        QCOMPARE(down->x(), downX);
        QCOMPARE(up->x(), upX);
        QCOMPARE(dht->text(), QStringLiteral("DHT: 42 nodes"));
        QCOMPARE(ip->text(), QStringLiteral("External IPs: 192.0.2.10, 2001:db8::10"));
        openConnectDialog(window, second.url(), false, true, true, "Other status server");
        QTRY_COMPARE_WITH_TIMEOUT(down->text(), QStringLiteral("D: 0 B/s"), 6000);
        QCOMPARE(up->text(), QStringLiteral("U: 0 B/s"));
        QCOMPARE(ip->text(), QStringLiteral("External IP: N/A"));
        QTRY_VERIFY_WITH_TIMEOUT(dht->isHidden(), 6000);
        auto *tabs = window.findChild<QTabWidget *>("serverTabs");
        QVERIFY(tabs && tabs->count() == 2);
        tabs->setCurrentIndex(0);
        QTRY_COMPARE(ip->text(), QStringLiteral("External IPs: 192.0.2.10, 2001:db8::10"));
        QVERIFY(dht->isVisible());
        configureIp(false, true);
        QVERIFY(ip->isHidden());
        first.setStatusRates({{"dht_nodes", 0}});
        QTRY_COMPARE_WITH_TIMEOUT(dht->text(), QStringLiteral("DHT: 0 nodes"), 6000);
        QVERIFY(dht->isVisible());
        // An enabled session with no nodes remains visible. Disabling DHT
        // through the real Preferences workflow hides the field and separator
        // only after the server confirms the setting.
        bool changed = false;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            const auto cleanup = qScopeGuard([dialog] { if (dialog->isVisible()) dialog->reject(); });
            auto *nav = dialog->findChild<QListWidget *>("preferencesSections");
            auto *field = dialog->findChild<QCheckBox *>("checkDHT");
            auto *buttons = dialog->findChild<QDialogButtonBox *>("buttonBox");
            QVERIFY(nav && field && buttons);
            nav->setCurrentRow(5);
            QTRY_VERIFY_WITH_TIMEOUT(field->isEnabled(), 6000);
            QVERIFY(field->isChecked());
            field->setChecked(false);
            QVERIFY(dht->isVisible());
            buttons->button(QDialogButtonBox::Apply)->click();
            QTRY_VERIFY_WITH_TIMEOUT(dht->isHidden(), 6000);
            QVERIFY(bar->findChild<QLabel *>("dhtSeparator")->isHidden());
            QTRY_VERIFY_WITH_TIMEOUT(buttons->isEnabled(), 6000);
            changed = true;
        });
        options->trigger();
        QVERIFY(changed);
        QCOMPARE(first.serverPreferences().value("dht").toBool(), false);
    }

    void liveTorrentSwarmCountsHonorHideZeroAndIncrementalSync()
    {
        DemoQbittorrentServer server(true);
        server.enableNativePresentationDemo();
        QVERIFY(server.start());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("swarm-view.ini")),
                           QSettings::IniFormat);
        // Apply the actual native Behavior option through the live MainWindow,
        // not a fabricated model object or a formatting-helper unit test.
        settings.setValue(QStringLiteral("Preferences/Behavior/HideZero"), true);
        settings.setValue(QStringLiteral("Preferences/Behavior/HideZeroMode"), 0);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        QTRY_VERIFY(window.isVisible());
        openConnectDialog(window, server.url());

        auto *table = window.findChild<QTableView *>(QStringLiteral("torrentList"));
        QVERIFY(table && table->model());
        QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 2, 6000);
        const auto rowFor = [table](const QString &name) {
            for (int row = 0; row < table->model()->rowCount(); ++row)
                if (table->model()->index(row, 0).data().toString() == name)
                    return row;
            return -1;
        };
        const auto shown = [table, &rowFor](const QString &name, int column) {
            const int row = rowFor(name);
            return row < 0 ? QStringLiteral("<missing torrent>")
                : table->model()->index(row, column).data().toString();
        };
        const QString complete = QStringLiteral("Complete seed");
        const QString partial = QStringLiteral("Partial download");
        QCOMPARE(shown(complete, 4), QStringLiteral("0 (3)"));
        QCOMPARE(shown(complete, 5), QString()); // Both counts are zero.
        QCOMPARE(shown(partial, 4), QStringLiteral("2 (5)"));
        QCOMPARE(shown(partial, 5), QStringLiteral("4 (8)"));

        // Apply the real local presentation preferences and verify that
        // Cancel leaves both the live model and saved settings unchanged.
        const QString fullUrl = QStringLiteral("https://tracker.example.test/announce/private-passkey");
        QCOMPARE(shown(complete, 16), QStringLiteral("tracker.example.test"));
        QCOMPARE(table->model()->index(rowFor(complete), 16).data(Qt::ToolTipRole).toString(),
            QStringLiteral("tracker.example.test"));
        const QString originalDate = shown(complete, 13);
        auto *prefs = window.findChild<QAction *>("actionOptions");
        QVERIFY(prefs);
        const auto changePresentation = [&](bool applyChanges, bool full) {
            bool completed = false;
            QTimer::singleShot(0, &window, [&] {
                auto *dialog = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget());
                QVERIFY(dialog);
                const auto cleanup = qScopeGuard([dialog] { if (dialog->isVisible()) dialog->reject(); });
                auto *nav = dialog->findChild<QListWidget *>("preferencesSections");
                auto *tracker = dialog->findChild<QCheckBox *>("checkBoxFullTrackerURL");
                auto *decimal = dialog->findChild<QCheckBox *>("checkDecimalUnits");
                auto *format = dialog->findChild<QComboBox *>("comboDateFormat");
                auto *scheme = dialog->findChild<QComboBox *>("comboColorScheme");
                auto *apply = dialog->findChild<QPushButton *>("preferencesApply");
                QVERIFY(nav && tracker && decimal && format && scheme && apply);
                nav->setCurrentRow(1);
                QVERIFY(tracker->isEnabled());
                QCOMPARE(tracker->isChecked(), settings.value("Preferences/Behavior/FullTrackerURL", false).toBool());
                QVERIFY(format->isEnabled());
                tracker->setChecked(full);
                QVERIFY(decimal->isEnabled());
                QCOMPARE(decimal->isChecked(), settings.value("Preferences/Behavior/DecimalUnits", false).toBool());
                decimal->setChecked(full);
                const int iso = format->findData(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
                QVERIFY(iso > 0);
                format->setCurrentIndex(iso);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
                QVERIFY(scheme->isEnabled());
                scheme->setCurrentIndex(scheme->findData(QStringLiteral("dark")));
#endif
                if (applyChanges)
                {
                    QVERIFY(apply->isEnabled());
                    apply->click();
                    QTRY_VERIFY(!apply->isEnabled());
                }
                completed = true;
            });
            prefs->trigger();
            QVERIFY(completed);
        };
        const QString binarySize = QLocale().toString(2460000.0 / (1024.0 * 1024.0), 'f', 2)
            + QStringLiteral(" MiB");
        const QString decimalSize = QLocale().toString(2.46, 'f', 2) + QStringLiteral(" MB");
        QCOMPARE(shown(complete, 1), binarySize);
        changePresentation(false, true);
        QCOMPARE(shown(complete, 16), QStringLiteral("tracker.example.test"));
        QCOMPARE(shown(complete, 13), originalDate);
        QCOMPARE(shown(complete, 1), binarySize);
        QVERIFY(!settings.value("Preferences/Behavior/DecimalUnits", false).toBool());
        changePresentation(true, true);
        QCOMPARE(shown(complete, 16), fullUrl);
        QVERIFY(settings.value("Preferences/Behavior/FullTrackerURL").toBool());
        QVERIFY(settings.value("Preferences/Behavior/DecimalUnits").toBool());
        QCOMPARE(shown(complete, 1), decimalSize);
        QCOMPARE(table->model()->index(rowFor(complete), 1).data(Qt::UserRole).toLongLong(), qint64(2460000));
        changePresentation(true, false);
        QCOMPARE(shown(complete, 1), binarySize);
        QVERIFY(!settings.value("Preferences/Behavior/DecimalUnits").toBool());
        QCOMPARE(shown(complete, 16), QStringLiteral("tracker.example.test"));
        QCOMPARE(shown(complete, 13), QLocale().toString(QDateTime::fromSecsSinceEpoch(1700000000),
            QStringLiteral("yyyy-MM-dd HH:mm:ss")));
        const auto trackerIndex = table->model()->index(rowFor(complete), 16);
        QCOMPARE(trackerIndex.data(Qt::ToolTipRole).toString(), QStringLiteral("tracker.example.test"));
        QCOMPARE(trackerIndex.data(Qt::UserRole).toString(), fullUrl);
        QCOMPARE(table->model()->index(rowFor(complete), 13).data(Qt::UserRole).toLongLong(), qint64(1700000000));
        QCOMPARE(server.commandsFor("/api/v2/app/setPreferences"), 0);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        QCOMPARE(settings.value(QStringLiteral("Appearance/ColorScheme")).toString(), QStringLiteral("dark"));
#endif

        // Change counts at the HTTP server, then observe the *live* Qt table
        // after a genuine incremental /sync/maindata response is processed.
        server.queueSwarmCountsUpdate();
        QTRY_COMPARE_WITH_TIMEOUT(shown(complete, 4), QStringLiteral("10 (14)"), 7000);
        QCOMPARE(shown(complete, 5), QStringLiteral("3 (6)"));
        QCOMPARE(shown(partial, 4), QStringLiteral("2 (11)")); // Total-only patch.

        // The display string must not leak into numeric sorting: 10 > 2,
        // despite "2 (...)" sorting lexically after "10 (...)".
        table->sortByColumn(4, Qt::DescendingOrder);
        QTRY_COMPARE(table->model()->index(0, 0).data().toString(), complete);
        QCOMPARE(table->model()->index(0, 4).data(Qt::UserRole).toInt(), 10);
        QCOMPARE(table->model()->index(1, 4).data(Qt::UserRole).toInt(), 2);

        // Start with a narrow table configuration that fits completely,
        // then hold the real native header grip against the trailing edge.
        // A single column must be able to exceed the viewport width.
        auto *header = table->horizontalHeader();
        auto *scroll = table->horizontalScrollBar();
        for (int col = 0; col < table->model()->columnCount(); ++col)
            table->setColumnHidden(col, col != 0 && col != 1);
        header->resizeSection(0, 180);
        header->resizeSection(1, 80);
        QTRY_COMPARE(scroll->maximum(), 0);
        QVERIFY(header->sectionsClickable());
        QCOMPARE(table->horizontalScrollMode(), QAbstractItemView::ScrollPerPixel);
        QCOMPARE(header->sectionResizeMode(0), QHeaderView::Interactive);
        auto *viewport = header->viewport();
        const QPoint grip(header->sectionViewportPosition(0) + header->sectionSize(0) - 1,
            viewport->height() / 2);
        const QPoint edge(viewport->width() - 2, grip.y());
        QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, grip);
        QMouseEvent move(QEvent::MouseMove, QPointF(edge), Qt::NoButton,
            Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewport, &move);
        bool released = false;
        const auto release = qScopeGuard([&] {
            if (!released) QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, edge);
        });
        QTRY_VERIFY_WITH_TIMEOUT(header->sectionSize(0) > viewport->width() + 20, 2500);
        QTRY_VERIFY(scroll->maximum() > 0 && scroll->isVisible());
        QTRY_VERIFY(scroll->value() > 0);
        QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, edge);
        released = true;
        const int width = header->sectionSize(0);
        QTest::qWait(120);
        QCOMPARE(header->sectionSize(0), width);
        scroll->setValue(scroll->maximum());
        QTRY_VERIFY(header->sectionViewportPosition(1) < viewport->width());
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
        auto *count = window.statusBar()->findChild<QLabel *>(
            QStringLiteral("visibleTorrentCount"));
        QVERIFY(count);
        QTRY_COMPARE(count->text(), QStringLiteral("Torrents: 2 / 2"));
        // Apply real, composable Status + text search filters and observe the
        // permanent status label, not merely a proxy's internal rowCount.
        auto *statuses = window.findChild<QListWidget *>(QStringLiteral("filterList"));
        auto *search = window.findChild<QLineEdit *>(QStringLiteral("torrentSearch"));
        QVERIFY(statuses && search);
        const auto chooseStatus = [statuses](const QString &key) {
            for (int row = 0; row < statuses->count(); ++row)
                if (statuses->item(row)->data(Qt::UserRole).toString() == key)
                {
                    statuses->setCurrentRow(row);
                    return true;
                }
            return false;
        };
        QVERIFY(chooseStatus(QStringLiteral("seeding")));
        QTRY_COMPARE(table->model()->rowCount(), 1);
        QTRY_COMPARE(count->text(), QStringLiteral("Torrents: 1 / 2"));
        search->setText(QStringLiteral("Partial download"));
        QTRY_COMPARE(table->model()->rowCount(), 0);
        QTRY_COMPARE(count->text(), QStringLiteral("Torrents: 0 / 2"));
        QVERIFY(chooseStatus(QStringLiteral("downloading")));
        QTRY_COMPARE(table->model()->rowCount(), 1);
        QTRY_COMPARE(count->text(), QStringLiteral("Torrents: 1 / 2"));
        search->clear();
        QVERIFY(chooseStatus(QStringLiteral("all")));
        QTRY_COMPARE(table->model()->rowCount(), 2);
        QTRY_COMPARE(count->text(), QStringLiteral("Torrents: 2 / 2"));

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

        // The live Size column rounds to two places, retaining raw bytes
        // for sorting; Total Size deliberately keeps its existing precision.
        const QModelIndex seedSize = indexFor(QStringLiteral("Complete seed"), 1);
        const QModelIndex partialSize = indexFor(QStringLiteral("Partial download"), 1);
        QVERIFY(seedSize.isValid() && partialSize.isValid());
        QCOMPARE(seedSize.data().toString(),
                 QLocale().toString(2.35, 'f', 2) + QStringLiteral(" MiB"));
        QCOMPARE(partialSize.data().toString(),
                 QLocale().toString(1.0, 'f', 2) + QStringLiteral(" KiB"));
        QCOMPARE(seedSize.data(Qt::UserRole).toLongLong(), qint64(2460000));
        QCOMPARE(partialSize.data(Qt::UserRole).toLongLong(), qint64(1024));
        QCOMPARE(indexFor(QStringLiteral("Complete seed"), 31).data().toString(),
                 QLocale().toString(4.0, 'f', 1) + QStringLiteral(" MiB"));
        QCOMPARE(table->model()->index(0, 0).data().toString(),
                 QStringLiteral("Complete seed")); // Descending raw-byte sort.

        const QModelIndex seed = indexFor(QStringLiteral("Complete seed"), 2);
        const QModelIndex partial = indexFor(QStringLiteral("Partial download"), 2);
        QVERIFY(seed.isValid() && partial.isValid());
        QCOMPARE(seed.data(Qt::UserRole).toDouble(), 1.0);
        QCOMPARE(partial.data(Qt::UserRole).toDouble(), 0.5);
        QVERIFY(seed.data().toString().endsWith(QLatin1Char('%')));
        QVERIFY(partial.data().toString().endsWith(QLatin1Char('%')));
        QCOMPARE(indexFor(QStringLiteral("Complete seed"), 3).data().toString(),
                 QStringLiteral("Seeding"));
        // End-to-end rendering from the authenticated sync/maindata fixture:
        // connected count followed by the swarm total (upstream v5.3.0).
        QCOMPARE(indexFor(QStringLiteral("Complete seed"), 4).data().toString(),
                 QStringLiteral("0 (3)"));
        QCOMPARE(indexFor(QStringLiteral("Complete seed"), 5).data().toString(),
                 QStringLiteral("0 (0)"));
        QCOMPARE(indexFor(QStringLiteral("Partial download"), 4).data().toString(),
                 QStringLiteral("2 (5)"));
        QCOMPARE(indexFor(QStringLiteral("Partial download"), 5).data().toString(),
                 QStringLiteral("4 (8)"));
        QCOMPARE(indexFor(QStringLiteral("Partial download"), 4)
                 .data(Qt::UserRole).toInt(), 2);
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

        // Exercise the real Preferences dialog and the existing delegate.
        // The partial download's green bar must be independent of text colors.
        auto *prefs = window.findChild<QAction *>(QStringLiteral("actionOptions"));
        QVERIFY(prefs);
        const auto setColors = [&](bool textColors, bool progressColors, bool save) {
            bool edited = false;
            QTimer::singleShot(0, &window, [&]() {
                auto *dlg = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget());
                QVERIFY(dlg);
                const auto cleanup = qScopeGuard([dlg]() { if (dlg->isVisible()) dlg->reject(); });
                auto *sections = dlg->findChild<QListWidget *>(QStringLiteral("preferencesSections"));
                auto *text = dlg->findChild<QCheckBox *>(QStringLiteral("checkUseTorrentStatesColors"));
                auto *bars = dlg->findChild<QCheckBox *>(QStringLiteral("checkProgressBarFollowsTextColor"));
                auto *apply = dlg->findChild<QPushButton *>(QStringLiteral("preferencesApply"));
                QVERIFY(sections && text && bars && apply);
                sections->setCurrentRow(1);
                QVERIFY(text->isEnabled() && bars->isEnabled());
                QCOMPARE(bars->text(), QStringLiteral("Use different progress bar colors by torrent states"));
                QCOMPARE(text->isChecked(), settings.value(
                    QStringLiteral("Preferences/Behavior/StateColors"), true).toBool());
                QCOMPARE(bars->isChecked(), settings.value(
                    QStringLiteral("Preferences/Behavior/ProgressFollowsState"), false).toBool());
                text->setChecked(textColors);
                QVERIFY(bars->isEnabled());
                bars->setChecked(progressColors);
                if (save)
                {
                    QVERIFY(apply->isEnabled());
                    apply->click();
                    QTRY_VERIFY(!apply->isEnabled());
                }
                edited = true;
            });
            prefs->trigger();
            QVERIFY(edited);
        };
        const auto progressIndex = [&]() { return indexFor(QStringLiteral("Partial download"), 2); };
        const auto nameIndex = [&]() { return indexFor(QStringLiteral("Partial download"), 0); };

        setColors(true, true, true);
        QVERIFY(nameIndex().data(Qt::ForegroundRole).value<QColor>().isValid());
        const QImage coloredBar = paintProgress(progressIndex());
        QVERIFY2(coloredBar != partialBar, "State-colored bar must differ from the native palette.");

        setColors(false, true, true);
        QVERIFY(!nameIndex().data(Qt::ForegroundRole).isValid());
        QCOMPARE(paintProgress(progressIndex()), coloredBar);
        QCOMPARE(settings.value(QStringLiteral("Preferences/Behavior/StateColors")).toBool(), false);
        QCOMPARE(settings.value(QStringLiteral("Preferences/Behavior/ProgressFollowsState")).toBool(), true);

        setColors(false, false, false); // Cancel must retain the colorful bars.
        QCOMPARE(paintProgress(progressIndex()), coloredBar);
        QVERIFY(!nameIndex().data(Qt::ForegroundRole).isValid());
        setColors(false, false, true);
        QVERIFY(!nameIndex().data(Qt::ForegroundRole).isValid());
        QCOMPARE(paintProgress(progressIndex()), partialBar);

        setColors(true, false, true);
        QVERIFY(nameIndex().data(Qt::ForegroundRole).value<QColor>().isValid());
        QCOMPARE(paintProgress(progressIndex()), partialBar);
        QCOMPARE(server.commandsFor("/api/v2/app/setPreferences"), 0);
    }

    void nearCompleteTorrentNeverDisplaysFullProgress()
    {
        DemoQbittorrentServer server(true);
        server.enableDetailsDemo();
        const QString hash(40, QLatin1Char('a'));
        const QString completeHash(40, QLatin1Char('b'));
        const auto record = [](const QString &name, double progress, const QString &state) {
            return QJsonObject{{QStringLiteral("name"), name},
                {QStringLiteral("progress"), progress},
                {QStringLiteral("state"), state},
                {QStringLiteral("size"), 1000000}};
        };
        server.setCompletionSnapshot({{QStringLiteral("torrents"), QJsonObject{
            {hash, record(QStringLiteral("Almost complete"), 0.99999, QStringLiteral("stoppedDL"))},
            {completeHash, record(QStringLiteral("Complete"), 1.0, QStringLiteral("stoppedUP"))}
        }}});
        QVERIFY(server.start());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("progress.ini")), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        openConnectDialog(window, server.url());
        auto *table = window.findChild<QTableView *>(QStringLiteral("torrentList"));
        auto *bar = window.findChild<QProgressBar *>(QStringLiteral("detailsProgressBar"));
        auto *label = window.findChild<QLabel *>(QStringLiteral("labelProgressVal"));
        QVERIFY(table && bar && label);
        QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 2, 6000);
        table->sortByColumn(2, Qt::AscendingOrder);
        QTRY_COMPARE(table->model()->index(0, 0).data().toString(), QStringLiteral("Almost complete"));
        const QString partial = QLocale().toString(99.9, 'f', 1) + QLatin1Char('%');
        const QString complete = QLocale().toString(100.0, 'f', 1) + QLatin1Char('%');
        QCOMPARE(table->model()->index(0, 2).data().toString(), partial);
        QCOMPARE(table->model()->index(1, 2).data().toString(), complete);
        table->selectRow(0);
        QTRY_COMPARE(label->text(), partial);
        QVERIFY(bar->value() < bar->maximum());
        QVERIFY(bar->value() > 0);
        // The live server's final delta, not display rounding, marks completion.
        server.queueCompletionPatch({{QStringLiteral("torrents"), QJsonObject{
            {hash, QJsonObject{{QStringLiteral("progress"), 1.0},
                               {QStringLiteral("state"), QStringLiteral("stoppedUP")}}}
        }}});
        QTRY_COMPARE_WITH_TIMEOUT(label->text(), complete, 6000);
        QCOMPARE(bar->value(), bar->maximum());
        QCOMPARE(table->model()->index(0, 2).data().toString(), complete);
        QCOMPARE(table->model()->index(1, 2).data().toString(), complete);
    }

    void contentRefreshPreservesUserFolderExpansion()
    {
        DemoQbittorrentServer server(true);
        server.enableNativePresentationDemo();
        server.enableDetailsDemo();
        server.enableRemoteContentAndOptionsDemo();
        const QString first = QStringLiteral("0000000000000000000000000000000000000001");
        const QString second = QStringLiteral("0000000000000000000000000000000000000002");
        QJsonArray files = QJsonDocument::fromJson(R"JSON([
          {"index":0,"name":"Album/Disc/song.flac","size":1024,"progress":0.25,"priority":1},
          {"index":1,"name":"Bonus/Disc/song.flac","size":2048,"progress":0.5,"priority":1}
        ])JSON").array();
        // A long file tree must remain scrollable throughout a recheck.
        for (int i = 2; i < 160; ++i)
            files.append(QJsonObject{
                {QStringLiteral("index"), i},
                {QStringLiteral("name"), QStringLiteral("Album/Disc/track-%1.flac").arg(i, 3, 10, QLatin1Char('0'))},
                {QStringLiteral("size"), 1024},
                {QStringLiteral("progress"), 0.25},
                {QStringLiteral("priority"), 1}
            });
        server.setContentEntries(first, files);
        server.setContentEntries(second, files);
        QVERIFY(server.start());

        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("prefs.ini")), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        openConnectDialog(window, server.url());
        auto *table = window.findChild<QTableView *>(QStringLiteral("torrentList"));
        auto *tabs = window.findChild<PropTabBar *>();
        auto *tree = window.findChild<MappedContentTreeWidget *>(QStringLiteral("torrentContent"));
        auto *pane = window.findChild<ServerPane *>();
        QVERIFY(table && tabs && tree && pane);
        QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 2, 6000);
        const auto selectTorrent = [&](const QString &name) {
            for (int i = 0; i < table->model()->rowCount(); ++i)
            {
                if (table->model()->index(i, 0).data().toString() == name)
                {
                    table->selectRow(i);
                    return;
                }
            }
            QFAIL("Torrent not found");
        };
        selectTorrent(QStringLiteral("Complete seed"));
        tabs->setCurrentIndex(4);
        QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(), 2, 6000);
        const auto root = [tree](const QString &name) -> QTreeWidgetItem * {
            for (int i = 0; i < tree->topLevelItemCount(); ++i)
                if (tree->topLevelItem(i)->text(0) == name)
                    return tree->topLevelItem(i);
            return nullptr;
        };
        const QString album = QStringLiteral("Album");
        const QString bonus = QStringLiteral("Bonus");
        QVERIFY(root(album) && root(bonus));
        QVERIFY(root(album)->isExpanded() && root(bonus)->isExpanded());
        QVERIFY(!root(album)->child(0)->isExpanded());
        tree->expandItem(root(album)->child(0));
        tree->collapseItem(root(bonus));
        auto *selected = root(album)->child(0)->child(0);
        tree->setCurrentItem(selected);
        selected->setSelected(true);
        tree->doItemsLayout();
        QTRY_VERIFY(tree->verticalScrollBar()->maximum() > 20);
        tree->verticalScrollBar()->setValue(tree->verticalScrollBar()->maximum() / 2);
        const int scrollPosition = tree->verticalScrollBar()->value();
        auto *anchor = tree->itemAt(0, 0);
        QVERIFY(anchor && anchor != selected);
        bool anchorIdValid = false;
        const int anchorId = anchor->data(0, Qt::UserRole).toInt(&anchorIdValid);
        QVERIFY(anchorIdValid && anchorId >= 0);
        const int anchorOffset = tree->visualItemRect(anchor).top();

        // Drive the actual periodic-refresh request path, not displayFiles
        // directly. Changing progress proves the new reply reached the UI.
        QJsonArray updated = files;
        QJsonObject track = updated.at(0).toObject();
        track.insert(QStringLiteral("progress"), 0.75);
        updated.replace(0, track);
        server.setContentEntries(first, updated);
        pane->refreshSelectedDetails();
        QTRY_COMPARE_WITH_TIMEOUT(root(album)->child(0)->child(0)
            ->data(2, Qt::UserRole).toDouble(), 0.75, 6000);
        QVERIFY(root(album)->isExpanded() && root(album)->child(0)->isExpanded());
        QVERIFY(!root(bonus)->isExpanded() && !root(bonus)->child(0)->isExpanded());
        QCOMPARE(tree->selectedItems().size(), 1);
        QCOMPARE(tree->selectedItems().first(), root(album)->child(0)->child(0));
        QCOMPARE(tree->currentItem(), root(album)->child(0)->child(0));
        // Let queued view/layout events run before checking the position.
        QTest::qWait(50);
        QCOMPARE(tree->verticalScrollBar()->value(), scrollPosition);
        QVERIFY(tree->itemAt(0, 0));
        bool restoredIdValid = false;
        const int restoredId = tree->itemAt(0, 0)->data(0, Qt::UserRole).toInt(&restoredIdValid);
        QVERIFY(restoredIdValid);
        QCOMPARE(restoredId, anchorId);
        QCOMPARE(tree->visualItemRect(tree->itemAt(0, 0)).top(), anchorOffset);

        // A deliberately collapsed nested folder must stay collapsed too.
        tree->collapseItem(root(album)->child(0));
        track.insert(QStringLiteral("progress"), 0.99999);
        updated.replace(0, track);
        server.setContentEntries(first, updated);
        pane->refreshSelectedDetails();
        QTRY_COMPARE_WITH_TIMEOUT(root(album)->child(0)->child(0)
            ->data(2, Qt::UserRole).toDouble(), 0.99999, 6000);
        QCOMPARE(root(album)->child(0)->child(0)->text(2),
                 QLocale().toString(99.0, 'f', 0) + QLatin1Char('%'));
        QVERIFY(!root(album)->child(0)->isExpanded());
        QVERIFY(!root(bonus)->isExpanded());

        // Identical names/file IDs on another torrent must not inherit the
        // prior torrent's expansion or selected file.
        selectTorrent(QStringLiteral("Partial download"));
        QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(), 2, 6000);
        QVERIFY(root(album) && root(bonus));
        QTRY_COMPARE_WITH_TIMEOUT(root(album)->child(0)->child(0)
            ->data(2, Qt::UserRole).toDouble(), 0.25, 6000);
        QVERIFY(root(album)->isExpanded() && root(bonus)->isExpanded());
        QVERIFY(!root(album)->child(0)->isExpanded());
        QVERIFY(tree->selectedItems().isEmpty());
        QCOMPARE(server.commandsFor("/api/v2/torrents/filePrio"), 0);

        // Exercise the real native Rename dialog on the second torrent.
        // No local mount is configured; the rename belongs to the server.
        const auto rename = [&](const QString &name, bool accept) {
            bool menuFound = false, dialogFound = false;
            QTimer watchdog;
            watchdog.setSingleShot(true);
            connect(&watchdog, &QTimer::timeout, &window, []() {
                if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
                    dialog->reject();
                if (auto *popup = QApplication::activePopupWidget()) popup->close();
            });
            watchdog.start(6000);
            QTimer::singleShot(0, &window, [&]() {
                auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
                if (!menu) return;
                for (auto *action : menu->actions())
                {
                    if (action->objectName() != QLatin1String("contentRenameItem")) continue;
                    menuFound = action->isEnabled();
                    if (menuFound)
                    {
                        QTimer::singleShot(0, &window, [&]() {
                            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                            if (!dialog || dialog->objectName() != QLatin1String("contentRenameDialog")) return;
                            auto *editor = dialog->findChild<QLineEdit *>(QStringLiteral("contentRenameName"));
                            auto *buttons = dialog->findChild<QDialogButtonBox *>(QStringLiteral("contentRenameButtons"));
                            if (!editor || !buttons) { dialog->reject(); return; }
                            dialogFound = true;
                            QVERIFY(dialog->width() >= 500);
                            QVERIFY(dialog->isSizeGripEnabled());
                            const QString original = editor->text();
                            const bool file = original.endsWith(QStringLiteral(".flac"));
                            QCOMPARE(editor->selectedText(), file ? original.left(original.lastIndexOf(QLatin1Char('.'))) : original);
                            editor->setText(name);
                            buttons->button(accept ? QDialogButtonBox::Ok : QDialogButtonBox::Cancel)->click();
                        });
                        action->trigger();
                    }
                    break;
                }
                menu->close();
            });
            QVERIFY(QMetaObject::invokeMethod(tree, "customContextMenuRequested",
                Qt::DirectConnection, Q_ARG(QPoint, QPoint(10, 10))));
            QVERIFY(menuFound && dialogFound);
        };
        tree->expandItem(root(bonus)->child(0));
        tree->setCurrentItem(root(bonus)->child(0)->child(0));
        rename(QStringLiteral("cancelled.flac"), false);
        QCOMPARE(server.commandsFor("/api/v2/torrents/renameFile"), 0);
        QCOMPARE(server.contentEntries(second), files);
        const QString renamed = QStringLiteral("new + 100%.flac");
        rename(renamed, true);
        QTRY_COMPARE_WITH_TIMEOUT(server.contentEntries(second).at(1).toObject()
            .value(QStringLiteral("name")).toString(),
            QStringLiteral("Bonus/Disc/") + renamed, 6000);
        QTRY_COMPARE_WITH_TIMEOUT(root(bonus)->child(0)->child(0)->text(0), renamed, 6000);
        tree->clearSelection();
        tree->setCurrentItem(root(bonus));
        const QString newFolder = QStringLiteral("Extras + 100%");
        rename(newFolder, true);
        QTRY_COMPARE_WITH_TIMEOUT(server.contentEntries(second).at(1).toObject()
            .value(QStringLiteral("name")).toString(),
            newFolder + QStringLiteral("/Disc/") + renamed, 6000);
        QTRY_VERIFY_WITH_TIMEOUT(root(newFolder), 6000);
        QCOMPARE(server.contentEntries(first), updated);
        QCOMPARE(server.commandsFor("/api/v2/torrents/renameFile"), 1);
        QCOMPARE(server.commandsFor("/api/v2/torrents/renameFolder"), 1);

        // Batch rename follows the native Manage Content layout editor:
        // selected folders contribute descendants, overlapping selection
        // is deduplicated, and unrelated files remain visible.
        QVERIFY(root(newFolder) && root(album));
        tree->clearSelection();
        tree->expandItem(root(newFolder));
        tree->expandItem(root(newFolder)->child(0));
        tree->expandItem(root(album)->child(0));
        root(newFolder)->setSelected(true);
        root(newFolder)->child(0)->child(0)->setSelected(true);
        root(album)->child(0)->child(0)->setSelected(true);
        bool opened = false, applied = false;
        QTimer watchdog;
        watchdog.setSingleShot(true);
        connect(&watchdog, &QTimer::timeout, &window, []() {
            if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
                dialog->reject();
            if (auto *popup = QApplication::activePopupWidget()) popup->close();
        });
        watchdog.start(12000);
        QTimer::singleShot(0, &window, [&]() {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu) return;
            for (auto *action : menu->actions())
            {
                if (action->objectName() != QLatin1String("contentBatchRename")
                    || !action->isEnabled()) continue;
                QTimer::singleShot(0, &window, [&]() {
                    auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                    if (!dialog || dialog->objectName() != QLatin1String("remoteTorrentContentLayoutDialog")) return;
                    opened = true;
                    auto *paths = dialog->findChild<QTreeView *>(QStringLiteral("pathListView"));
                    auto *common = dialog->findChild<QLineEdit *>(QStringLiteral("commonPathEdit"));
                    auto *buttons = dialog->findChild<QDialogButtonBox *>(QStringLiteral("buttonBox"));
                    QVERIFY(paths && common && buttons);
                    QTRY_COMPARE_WITH_TIMEOUT(paths->model()->rowCount(), files.size(), 6000);
                    QTRY_VERIFY_WITH_TIMEOUT(paths->isEnabled(), 6000);
                    QSet<int> ids;
                    for (const auto &index : paths->selectionModel()->selectedRows(0))
                        ids.insert(index.data().toInt());
                    QCOMPARE(ids, (QSet<int>{0, 1}));
                    QVERIFY(common->isEnabled());
                    QCOMPARE(common->text(), QString());
                    QTest::keyClicks(common, QStringLiteral("Moved"));
                    auto *model = qobject_cast<QStandardItemModel *>(paths->model());
                    QVERIFY(model);
                    QCOMPARE(model->item(0, 1)->text(), QStringLiteral("Moved/Album/Disc/song.flac"));
                    QCOMPARE(model->item(1, 1)->text(), QStringLiteral("Moved/") + newFolder
                        + QStringLiteral("/Disc/") + renamed);
                    QCOMPARE(model->item(2, 1)->text(), files.at(2).toObject().value("name").toString());
                    // A conflicting target must be rejected before sending
                    // any mutations, even if another row is also edited.
                    const QString target = model->item(0, 1)->text();
                    model->item(0, 1)->setText(model->item(2, 1)->text());
                    bool warned = false;
                    QTimer::singleShot(0, dialog, [&]() {
                        if (auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                        {
                            warned = true;
                            message->accept();
                        }
                    });
                    buttons->button(QDialogButtonBox::Apply)->click();
                    QVERIFY(warned);
                    QCOMPARE(server.commandsFor("/api/v2/torrents/renameFile"), 1);
                    model->item(0, 1)->setText(target);
                    buttons->button(QDialogButtonBox::Apply)->click();
                    QTRY_COMPARE_WITH_TIMEOUT(server.contentEntries(second).at(0).toObject()
                        .value("name").toString(), target, 6000);
                    QTRY_COMPARE_WITH_TIMEOUT(server.contentEntries(second).at(1).toObject()
                        .value("name").toString(), QStringLiteral("Moved/") + newFolder
                        + QStringLiteral("/Disc/") + renamed, 6000);
                    QTRY_VERIFY_WITH_TIMEOUT(paths->isEnabled()
                        && buttons->button(QDialogButtonBox::Close)->isEnabled(), 6000);
                    QCOMPARE(model->item(0, 1)->text(), target);
                    applied = true;
                    buttons->button(QDialogButtonBox::Close)->click();
                });
                action->trigger();
                break;
            }
            menu->close();
        });
        QVERIFY(QMetaObject::invokeMethod(tree, "customContextMenuRequested",
            Qt::DirectConnection, Q_ARG(QPoint, QPoint(10, 10))));
        QVERIFY(opened && applied);
        QTRY_VERIFY_WITH_TIMEOUT(root(QStringLiteral("Moved")), 6000);
        QCOMPARE(server.commandsFor("/api/v2/torrents/renameFile"), 3);
        QCOMPARE(server.contentEntries(first), updated);
        for (int i = 2; i < files.size(); ++i)
            QCOMPARE(server.contentEntries(second).at(i), files.at(i));

        // Assign priorities using the actual descending displayed order,
        // selecting in the opposite order. Four rows yield Max, High,
        // Normal, Normal. The neighboring torrent and other IDs stay intact.
        tree->clearSelection();
        tree->sortItems(0, Qt::DescendingOrder);
        QTRY_VERIFY(root(album));
        auto *disc = root(album)->child(0);
        tree->expandItem(root(album));
        tree->expandItem(disc);
        QList<int> order;
        for (int i = 3; i >= 0; --i) disc->child(i)->setSelected(true);
        for (int i = 0; i < 4; ++i)
            order.append(disc->child(i)->data(0, Qt::UserRole).toInt());
        QVERIFY(order.at(0) > order.at(1) && order.at(1) > order.at(2));
        bool priorityTriggered = false;
        QTimer::singleShot(0, &window, [&]() {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu) return;
            for (auto *action : menu->actions())
            {
                if (!action->menu()) continue;
                for (auto *sub : action->menu()->actions())
                {
                    if (sub->objectName() != QLatin1String("contentPriorityByOrder")) continue;
                    priorityTriggered = sub->isEnabled();
                    if (priorityTriggered) sub->trigger();
                }
            }
            menu->close();
        });
        QVERIFY(QMetaObject::invokeMethod(tree, "customContextMenuRequested",
            Qt::DirectConnection, Q_ARG(QPoint, QPoint(10, 10))));
        QVERIFY(priorityTriggered);
        QTRY_COMPARE_WITH_TIMEOUT(server.commandsFor("/api/v2/torrents/filePrio"), 3, 6000);
        const auto priorityAt = [&](int id) {
            return server.contentEntries(second).at(id).toObject().value("priority").toInt();
        };
        QTRY_COMPARE_WITH_TIMEOUT(priorityAt(order.at(0)), 7, 6000);
        QCOMPARE(priorityAt(order.at(1)), 6);
        QCOMPARE(priorityAt(order.at(2)), 1);
        QCOMPARE(priorityAt(order.at(3)), 1);
        QTRY_COMPARE_WITH_TIMEOUT(disc->child(0)->data(3, Qt::UserRole).toInt(), 7, 6000);
        QCOMPARE(disc->child(1)->data(3, Qt::UserRole).toInt(), 6);
        QCOMPARE(server.contentEntries(first), updated);
        QCOMPARE(priorityAt(0), 1);
        QCOMPARE(priorityAt(1), 1);
    }

    void torrentCommentsUseSelectedServerAndRespectCapability()
    {
        DemoQbittorrentServer legacy(true);
        DemoQbittorrentServer modern(true);
        legacy.setCapabilityProfile(DemoQbittorrentServer::CapabilityProfile::V514);
        modern.enableModernResponses();
        modern.setCapabilityProfile(DemoQbittorrentServer::CapabilityProfile::V520);
        for (auto *server : {&legacy, &modern})
        {
            server->enableNativePresentationDemo();
            server->enableDetailsDemo();
            QVERIFY(server->start());
        }
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("profiles.ini")), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        QTRY_VERIFY(window.isVisible());
        openConnectDialog(window, legacy.url());
        auto *tabs = window.findChild<QTabWidget *>("serverTabs");
        QVERIFY(tabs);
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 6000);
        auto *legacyTable = tabs->widget(0)->findChild<QTableView *>("torrentList");
        auto *legacyEdit = tabs->widget(0)->findChild<QPushButton *>("editCommentButton");
        auto *legacyComment = tabs->widget(0)->findChild<QLabel *>("labelCommentVal");
        QVERIFY(legacyTable && legacyEdit && legacyComment);
        QTRY_COMPARE_WITH_TIMEOUT(legacyTable->model()->rowCount(), 2, 6000);
        legacyTable->selectRow(0);
        QTRY_VERIFY_WITH_TIMEOUT(legacyComment->text().contains("plain markup"), 6000);
        QVERIFY(!legacyEdit->isEnabled());

        openConnectDialog(window, modern.url(), false, true, true, QStringLiteral("Modern seedbox"));
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 2, 6000);
        auto *table = tabs->widget(1)->findChild<QTableView *>("torrentList");
        auto *edit = tabs->widget(1)->findChild<QPushButton *>("editCommentButton");
        auto *label = tabs->widget(1)->findChild<QLabel *>("labelCommentVal");
        QVERIFY(table && edit && label);
        QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 2, 6000);
        QVERIFY(!edit->isEnabled());
        int row = -1;
        for (int i = 0; i < table->model()->rowCount(); ++i)
            if (table->model()->index(i, 0).data().toString() == QStringLiteral("Complete seed"))
                row = i;
        QVERIFY(row >= 0);
        table->selectRow(row);
        QTRY_VERIFY_WITH_TIMEOUT(edit->isEnabled(), 6000);
        const QString original = QStringLiteral("Get it at https://example.test/files?id=7&part=2 and <b>plain markup</b>; javascript:alert(1)");
        const auto endpoint = QByteArrayLiteral("/api/v2/torrents/setComment");
        edit->click();
        auto *dialog = window.findChild<QDialog *>("remoteEditCommentDialog");
        QVERIFY(dialog);
        auto *editor = dialog->findChild<QPlainTextEdit *>("remoteCommentEditor");
        auto *buttons = dialog->findChild<QDialogButtonBox *>();
        QVERIFY(editor && buttons);
        QCOMPARE(editor->toPlainText(), original);
        editor->setPlainText(QStringLiteral("Discard this"));
        buttons->button(QDialogButtonBox::Cancel)->click();
        QTRY_VERIFY(!window.findChild<QDialog *>("remoteEditCommentDialog"));
        QCOMPARE(modern.commandsFor(endpoint), 0);

        edit->click();
        dialog = window.findChild<QDialog *>("remoteEditCommentDialog");
        QVERIFY(dialog);
        editor = dialog->findChild<QPlainTextEdit *>("remoteCommentEditor");
        buttons = dialog->findChild<QDialogButtonBox *>();
        QVERIFY(editor && buttons);
        const QString changed = QStringLiteral("New + 100% & <b>literal</b>\nSecond line");
        editor->setPlainText(changed);
        buttons->button(QDialogButtonBox::Ok)->click();
        QTRY_COMPARE_WITH_TIMEOUT(modern.commandsFor(endpoint), 1, 6000);
        const QUrlQuery form(QString::fromUtf8(modern.lastCommandBody(endpoint)));
        QCOMPARE(form.queryItemValue(QStringLiteral("hashes")), QStringLiteral("0000000000000000000000000000000000000001"));
        QCOMPARE(form.queryItemValue(QStringLiteral("comment"), QUrl::FullyDecoded), changed);
        QVERIFY(modern.lastCommandBody(endpoint).contains("%2B"));
        QTRY_VERIFY_WITH_TIMEOUT(label->text().contains("&lt;b&gt;literal&lt;/b&gt;<br/>Second line"), 6000);
        QTRY_VERIFY_WITH_TIMEOUT(edit->isEnabled(), 6000);
        QCOMPARE(legacy.commandsFor(endpoint), 0);
        QVERIFY(legacyComment->text().contains("plain markup"));

        edit->click();
        dialog = window.findChild<QDialog *>("remoteEditCommentDialog");
        QVERIFY(dialog);
        editor = dialog->findChild<QPlainTextEdit *>("remoteCommentEditor");
        buttons = dialog->findChild<QDialogButtonBox *>();
        QVERIFY(editor && buttons);
        QCOMPARE(editor->toPlainText(), changed);
        editor->clear();
        buttons->button(QDialogButtonBox::Ok)->click();
        QTRY_COMPARE_WITH_TIMEOUT(modern.commandsFor(endpoint), 2, 6000);
        QTRY_VERIFY_WITH_TIMEOUT(label->text().isEmpty(), 6000);
        QVERIFY(modern.lastCommandBody(endpoint).endsWith("&comment="));
        QCOMPARE(legacy.commandsFor(endpoint), 0);
    }

    void trackerTabActionsRespectServerVersionAndPrivateMetadata()
    {
        for (const bool modern : {false, true})
        {
            DemoQbittorrentServer server(true);
            if (modern) server.enableModernResponses();
            server.setCapabilityProfile(modern ? DemoQbittorrentServer::CapabilityProfile::V520
                : DemoQbittorrentServer::CapabilityProfile::V514);
            server.enableNativePresentationDemo();
            server.enableDetailsDemo();
            server.enableStatefulTrackerEditingDemo();
            const QString publicHash = QStringLiteral("0000000000000000000000000000000000000001");
            const QString privateHash = QStringLiteral("0000000000000000000000000000000000000002");
            const QString original = QStringLiteral("https://tracker.example/announce?token=a%2Bb");
            const QString added = QStringLiteral("https://backup.example/announce");
            const QString replacement = QStringLiteral("https://replacement.example/announce?x=+");
            const QJsonArray initial{QJsonObject{{"url", original}, {"tier", 1}}};
            server.setTrackerEntries(publicHash, initial);
            server.setTrackerEntries(privateHash, initial);
            QVERIFY(server.start());
            QTemporaryDir temp;
            QVERIFY(temp.isValid());
            QSettings settings(temp.filePath(QStringLiteral("profiles.ini")), QSettings::IniFormat);
            DemoCredentialVault vault;
            MainWindow window(nullptr, &settings, &vault);
            window.show();
            openConnectDialog(window, server.url());
            auto *table = window.findChild<QTableView *>("torrentList");
            auto *tabs = window.findChild<PropTabBar *>();
            auto *trackers = window.findChild<QTreeWidget *>("torrentTrackers");
            QVERIFY(table && tabs && trackers);
            QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 2, 6000);
            int publicRow = -1, privateRow = -1;
            for (int i = 0; i < table->model()->rowCount(); ++i)
            {
                const auto name = table->model()->index(i, 0).data().toString();
                if (name == QLatin1String("Complete seed")) publicRow = i;
                if (name == QLatin1String("Partial download")) privateRow = i;
            }
            QVERIFY(publicRow >= 0 && privateRow >= 0);
            table->selectRow(publicRow);
            tabs->setCurrentIndex(1);
            QTRY_COMPARE_WITH_TIMEOUT(trackers->topLevelItemCount(), 1, 6000);

            const auto menuAction = [&](const QString &name, bool expectedEnabled,
                                        const QString &value = {}, bool input = false, bool inspect = false) {
                bool found = false, enabled = false, opened = false, timedOut = false;
                QTimer watchdog;
                watchdog.setSingleShot(true);
                connect(&watchdog, &QTimer::timeout, &window, [&]() {
                    timedOut = true;
                    if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject();
                    if (auto *popup = QApplication::activePopupWidget()) popup->close();
                });
                watchdog.start(6000);
                QTimer::singleShot(0, &window, [&]() {
                    auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
                    if (!menu) return;
                    for (auto *action : menu->actions())
                    {
                        if (action->objectName() != name) continue;
                        found = true;
                        enabled = action->isEnabled();
                        if (enabled && !inspect)
                        {
                            if (input)
                                QTimer::singleShot(0, &window, [&]() {
                                    auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
                                    if (!dialog) return;
                                    opened = true;
                                    dialog->setTextValue(value);
                                    dialog->accept();
                                });
                            action->trigger();
                        }
                        break;
                    }
                    menu->close();
                });
                QVERIFY(QMetaObject::invokeMethod(trackers, "customContextMenuRequested", Qt::DirectConnection,
                    Q_ARG(QPoint, QPoint(10, 10))));
                QVERIFY(found && !timedOut);
                QCOMPARE(enabled, expectedEnabled);
                QVERIFY(!input || opened);
            };
            trackers->topLevelItem(0)->setSelected(true);
            menuAction(QStringLiteral("copyDetailTrackers"), true);
            QCOMPARE(QApplication::clipboard()->text(), original);
            menuAction(QStringLiteral("addDetailTrackers"), true, added, true);
            QTRY_COMPARE_WITH_TIMEOUT(trackers->topLevelItemCount(), 2, 6000);
            QCOMPARE(server.commandsFor("/api/v2/torrents/addTrackers"), 1);
            trackers->clearSelection();
            auto rows = trackers->findItems(original, Qt::MatchExactly, 0);
            QCOMPARE(rows.size(), 1);
            rows.first()->setSelected(true);
            menuAction(QStringLiteral("editDetailTracker"), true, replacement, true);
            QTRY_COMPARE_WITH_TIMEOUT(trackers->findItems(replacement, Qt::MatchExactly, 0).size(), 1, 6000);
            const auto form = QUrlQuery(QString::fromUtf8(server.lastCommandBody("/api/v2/torrents/editTracker")));
            QCOMPARE(form.queryItemValue(modern ? "url" : "origUrl", QUrl::FullyDecoded), original);
            QCOMPARE(form.queryItemValue("newUrl", QUrl::FullyDecoded), replacement);
            trackers->clearSelection();
            trackers->findItems(replacement, Qt::MatchExactly, 0).first()->setSelected(true);
            menuAction(QStringLiteral("reannounceSelectedTrackers"), modern, {}, false, !modern);
            if (modern)
            {
                QTRY_COMPARE_WITH_TIMEOUT(server.commandsFor("/api/v2/torrents/reannounce"), 1, 6000);
                const auto announce = QUrlQuery(QString::fromUtf8(server.lastCommandBody("/api/v2/torrents/reannounce")));
                QCOMPARE(QUrl::fromPercentEncoding(announce.queryItemValue("urls", QUrl::FullyDecoded).toLatin1()), replacement);
                QCOMPARE(announce.queryItemValue("hashes"), publicHash);
                // Wait for authoritative readback after this action.
                auto *pane = window.findChild<ServerPane *>();
                QVERIFY(pane);
                const int reads = server.detailsFor("/api/v2/torrents/trackers?");
                pane->refreshSelectedDetails();
                QTRY_VERIFY_WITH_TIMEOUT(server.detailsFor("/api/v2/torrents/trackers?") > reads, 6000);
                QTest::qWait(100);
            }
            else QCOMPARE(server.commandsFor("/api/v2/torrents/reannounce"), 0);
            menuAction(QStringLiteral("reannounceAllTrackers"), true);
            QTRY_COMPARE_WITH_TIMEOUT(server.commandsFor("/api/v2/torrents/reannounce"), modern ? 2 : 1, 6000);
            QVERIFY(!QUrlQuery(QString::fromUtf8(server.lastCommandBody("/api/v2/torrents/reannounce"))).hasQueryItem("urls"));
            // Switch torrent: the same tracker URL must never be enough to
            // permit an addition to a private torrent.
            table->selectRow(privateRow);
            QTRY_COMPARE_WITH_TIMEOUT(trackers->topLevelItemCount(), 1, 6000);
            menuAction(QStringLiteral("addDetailTrackers"), false, {}, false, true);
            trackers->topLevelItem(0)->setSelected(true);
            menuAction(QStringLiteral("removeDetailTrackers"), true);
            QTRY_COMPARE_WITH_TIMEOUT(trackers->topLevelItemCount(), 0, 6000);
            QCOMPARE(server.commandsFor("/api/v2/torrents/removeTrackers"), 1);
            // Recovery with exactly one tracker is allowed.
            menuAction(QStringLiteral("addDetailTrackers"), true, original, true);
            QTRY_COMPARE_WITH_TIMEOUT(trackers->topLevelItemCount(), 1, 6000);
            QCOMPARE(server.commandsFor("/api/v2/torrents/addTrackers"), 2);
            QCOMPARE(server.trackerEntries(privateHash).first().toObject().value("url").toString(), original);
            QCOMPARE(server.prohibitedCapabilityRequests(), 0);
        }
    }

    void webSeedsUseNativeActionsAndServerReadback()
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
        auto *table = window.findChild<QTableView *>("torrentList");
        auto *tabs = window.findChild<PropTabBar *>();
        auto *sources = window.findChild<QListWidget *>("torrentWebSeeds");
        QVERIFY(table && tabs && sources);
        QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 2, 6000);
        int row = -1;
        for (int i = 0; i < table->model()->rowCount(); ++i)
            if (table->model()->index(i, 0).data().toString() == QStringLiteral("Complete seed")) row = i;
        QVERIFY(row >= 0);
        table->selectRow(row);
        tabs->setCurrentIndex(3);
        QTRY_COMPARE_WITH_TIMEOUT(sources->count(), 1, 6000);

        // Exercise actual popup actions and the nested native input dialog.
        const auto menuAction = [&](const QString &name, const QString &value, bool accept, bool inputExpected) {
            bool invoked = false, opened = false, timedOut = false;
            QTimer watchdog;
            watchdog.setSingleShot(true);
            connect(&watchdog, &QTimer::timeout, &window, [&]() {
                timedOut = true;
                if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject();
                if (auto *popup = QApplication::activePopupWidget()) popup->close();
            });
            watchdog.start(6000);
            QTimer::singleShot(0, &window, [&]() {
                auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
                if (!menu) return;
                for (auto *action : menu->actions())
                {
                    if (action->objectName() != name || !action->isEnabled()) continue;
                    invoked = true;
                    if (inputExpected)
                        QTimer::singleShot(0, &window, [&]() {
                            auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
                            if (!dialog) return;
                            opened = true;
                            dialog->setTextValue(value);
                            if (accept) dialog->accept(); else dialog->reject();
                        });
                    action->trigger();
                    break;
                }
                menu->close();
            });
            QVERIFY(QMetaObject::invokeMethod(sources, "customContextMenuRequested", Qt::DirectConnection,
                Q_ARG(QPoint, QPoint(10, 10))));
            QVERIFY(invoked && !timedOut && (!inputExpected || opened));
        };
        const QByteArray add = "/api/v2/torrents/addWebSeeds";
        const QByteArray edit = "/api/v2/torrents/editWebSeed";
        const QByteArray remove = "/api/v2/torrents/removeWebSeeds";
        const QString url = QStringLiteral("https://mirror.example/a%20b?x=a%7Cb&y=+");
        const QString replacement = QStringLiteral("https://other.example/new%2Ffile?x=%25");
        menuAction(QStringLiteral("addWebSeed"), url, false, true);
        QCOMPARE(server.commandsFor(add), 0);
        menuAction(QStringLiteral("addWebSeed"), url, true, true);
        QTRY_COMPARE_WITH_TIMEOUT(server.commandsFor(add), 1, 6000);
        QTRY_COMPARE_WITH_TIMEOUT(sources->count(), 2, 6000);
        QVERIFY(server.lastCommandBody(add).contains("%252520"));
        QCOMPARE(QUrlQuery(QString::fromUtf8(server.lastCommandBody(add))).queryItemValue("hash"),
            QStringLiteral("0000000000000000000000000000000000000001"));
        auto matches = sources->findItems(url, Qt::MatchExactly);
        QCOMPARE(matches.size(), 1);
        sources->clearSelection();
        matches.first()->setSelected(true);
        menuAction(QStringLiteral("copyWebSeeds"), {}, false, false);
        QCOMPARE(QApplication::clipboard()->text(), url);
        menuAction(QStringLiteral("editWebSeed"), replacement, true, true);
        QTRY_COMPARE_WITH_TIMEOUT(server.commandsFor(edit), 1, 6000);
        QTRY_COMPARE_WITH_TIMEOUT(sources->findItems(replacement, Qt::MatchExactly).size(), 1, 6000);
        QVERIFY(sources->findItems(url, Qt::MatchExactly).isEmpty());
        sources->selectAll();
        menuAction(QStringLiteral("removeWebSeeds"), {}, true, false);
        QTRY_COMPARE_WITH_TIMEOUT(server.commandsFor(remove), 1, 6000);
        QTRY_COMPARE_WITH_TIMEOUT(sources->count(), 0, 6000);
    }

    void nativeTorrentDetailsUseSelectedRemoteServerAndFilePriorityApis()
    {
        DemoQbittorrentServer server(true);
        server.setReportedVersions(QStringLiteral("v5.3.0"), QStringLiteral("2.16.0"));
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
        QTRY_COMPARE_WITH_TIMEOUT(propertiesBar->property("pieceCount").toInt(), 4, 6000);
        auto *availabilityBar = window.findChild<QProgressBar *>("detailsAvailabilityBar");
        auto *availabilityLabel = window.findChild<QLabel *>("labelAverageAvailabilityVal");
        QVERIFY(availabilityBar && availabilityLabel && !availabilityBar->isVisible());
        QCOMPARE(server.detailsFor("/api/v2/torrents/pieceAvailability?"), 0);
        int partialRow = -1;
        for (int i = 0; i < table->model()->rowCount(); ++i)
            if (table->model()->index(i, 0).data().toString() == QStringLiteral("Partial download"))
                partialRow = i;
        QVERIFY(partialRow >= 0);
        table->selectRow(partialRow);
        QTRY_VERIFY_WITH_TIMEOUT(availabilityBar->isVisible(), 6000);
        QCOMPARE(availabilityBar->property("pieceCount").toInt(), 4);
        QCOMPARE(availabilityLabel->text(), QLocale().toString(2.5, 'f', 3));
        QTRY_COMPARE_WITH_TIMEOUT(propertiesBar->property("pieceCount").toInt(), 4, 6000);
        // Use a deterministic contrasting palette and wait for the real
        // visible layout before comparing exact rendered piece colors.
        const QColor baseColor(240, 240, 240);
        const QColor completeColor(0, 85, 170);
        auto palette = propertiesBar->palette();
        palette.setColor(QPalette::Base, baseColor);
        palette.setColor(QPalette::Highlight, completeColor);
        propertiesBar->setPalette(palette);
        QTRY_VERIFY(propertiesBar->isVisible() && propertiesBar->width() >= 64);
        const auto colorAt = [propertiesBar](int eighth) {
            const auto pixels = propertiesBar->grab().toImage();
            if (pixels.isNull()) return QColor();
            return pixels.pixelColor(pixels.width() * eighth / 8, pixels.height() / 2);
        };
        QTRY_COMPARE_WITH_TIMEOUT(colorAt(1), completeColor, 6000);
        QCOMPARE(colorAt(3), baseColor);
        QCOMPARE(colorAt(7), baseColor);
        QVERIFY(colorAt(5) != baseColor && colorAt(5) != completeColor);
        table->selectRow(targetRow);
        QTRY_VERIFY(!availabilityBar->isVisible());
        QTRY_COMPARE_WITH_TIMEOUT(propertiesBar->property("pieceCount").toInt(), 4, 6000);
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
        QVERIFY(peers->horizontalHeader()->sectionsMovable());
        QCOMPARE(peers->item(0, 2)->text(), QStringLiteral("qBittorrent"));
        QCOMPARE(peers->columnCount(), 15);
        QCOMPARE(peers->item(0, 8)->data(Qt::UserRole).toInt(), 61413);
        QCOMPARE(peers->item(0, 8)->text(), QStringLiteral("61413"));
        QCOMPARE(peers->item(0, 9)->data(Qt::UserRole).toInt(), 1048576);
        peers->selectRow(0);
        auto *peerItem = peers->item(0, 0);
        serverPane->refreshSelectedDetails();
        QTRY_COMPARE_WITH_TIMEOUT(peers->item(0, 4)->data(Qt::UserRole).toInt(), 4096, 6000);
        QCOMPARE(peers->item(0, 0), peerItem);
        QCOMPARE(peers->item(0, 2)->text(), QStringLiteral("qBittorrent"));
        QCOMPARE(peers->selectionModel()->selectedRows().size(), 1);
        const int previousPeerQueries = server.detailsFor("/api/v2/sync/torrentPeers?");
        serverPane->refreshSelectedDetails();
        QTRY_VERIFY_WITH_TIMEOUT(server.detailsFor("/api/v2/sync/torrentPeers?") > previousPeerQueries, 6000);
        QTRY_VERIFY_WITH_TIMEOUT(server.lastDetailRequest().endsWith("&rid=2"), 6000);
        QCOMPARE(peers->rowCount(), 1);

        // Inspect and drive the real Peers context menu and dialogs.
        const auto addEndpoint = QByteArrayLiteral("/api/v2/torrents/addPeers");
        const auto banEndpoint = QByteArrayLiteral("/api/v2/transfer/banPeers");
        QCOMPARE(Remote::normalizedPeerEndpoint(QStringLiteral("[2001:db8::1]:6881")),
            QStringLiteral("[2001:db8::1]:6881"));
        QVERIFY(Remote::normalizedPeerEndpoint(QStringLiteral("2001:db8::1:6881")).isEmpty());
        QVERIFY(Remote::normalizedPeerEndpoint(QStringLiteral("192.0.2.1:65536")).isEmpty());
        QVERIFY(Remote::normalizedPeerEndpoint(QStringLiteral("example.test:80")).isEmpty());

        bool menuSeen = false, enabled = false, dialogSeen = false;
        QString resultText;
        QTimer driver;
        driver.setInterval(20);
        connect(&driver, &QTimer::timeout, &window, [&]() {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
            {
                if (!box->standardButtons().testFlag(QMessageBox::Yes))
                {
                    resultText = box->text();
                    box->button(QMessageBox::Ok)->click();
                }
            }
        });
        auto menuAction = [&](const QString &name, bool accept, bool inspectOnly) {
            menuSeen = enabled = dialogSeen = false;
            resultText.clear();
            bool timedOut = false;
            QTimer watchdog;
            watchdog.setSingleShot(true);
            connect(&watchdog, &QTimer::timeout, &window, [&]() {
                timedOut = true;
                if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
                    dialog->reject();
                if (auto *menu = QApplication::activePopupWidget()) menu->close();
            });
            watchdog.start(6000);
            QTimer::singleShot(0, &window, [&]() {
                auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
                if (!menu) return;
                menuSeen = true;
                for (auto *action : menu->actions())
                {
                    if (action->objectName() != name) continue;
                    enabled = action->isEnabled();
                    if (enabled && !inspectOnly)
                    {
                        // A separate queued callback answers the nested modal
                        // loop; a repeating timer cannot reenter its own slot.
                        QTimer::singleShot(0, &window, [&]() {
                            if (auto *input = qobject_cast<QInputDialog *>(QApplication::activeModalWidget()))
                            {
                                dialogSeen = true;
                                input->setTextValue(QStringLiteral("192.0.2.9:6881\n[2001:db8::1]:6882"));
                                if (accept) input->accept(); else input->reject();
                            }
                            else if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                            {
                                dialogSeen = true;
                                auto *button = box->button(accept ? QMessageBox::Yes : QMessageBox::No);
                                if (button) button->click(); else box->reject();
                            }
                        });
                        action->trigger();
                    }
                    break;
                }
                menu->close();
            });
            driver.start();
            QVERIFY(QMetaObject::invokeMethod(peers, "customContextMenuRequested", Qt::DirectConnection,
                Q_ARG(QPoint, QPoint(10, 10))));
            QVERIFY2(!timedOut, "Peer menu or modal dialog timed out.");
        };
        menuAction(QStringLiteral("addTorrentPeers"), false, false);
        QVERIFY(menuSeen && enabled && dialogSeen);
        QCOMPARE(server.commandsFor(addEndpoint), 0);
        driver.stop();
        menuAction(QStringLiteral("addTorrentPeers"), true, false);
        QVERIFY(menuSeen && enabled && dialogSeen);
        QTRY_COMPARE_WITH_TIMEOUT(server.commandsFor(addEndpoint), 1, 6000);
        QTRY_VERIFY_WITH_TIMEOUT(resultText.contains("added: 1; failed: 1"), 6000);
        driver.stop();
        const QUrlQuery added(QString::fromUtf8(server.lastCommandBody(addEndpoint)));
        QCOMPARE(added.queryItemValue(QStringLiteral("hashes")), torrentHash);
        QCOMPARE(added.queryItemValue(QStringLiteral("peers"), QUrl::FullyDecoded),
            QStringLiteral("192.0.2.9:6881|[2001:db8::1]:6882"));

        peers->selectRow(0);
        menuAction(QStringLiteral("banTorrentPeers"), false, false);
        QVERIFY(menuSeen && enabled && dialogSeen);
        QCOMPARE(server.commandsFor(banEndpoint), 0);
        driver.stop();
        menuAction(QStringLiteral("banTorrentPeers"), true, false);
        QVERIFY(menuSeen && enabled && dialogSeen);
        QTRY_COMPARE_WITH_TIMEOUT(server.commandsFor(banEndpoint), 1, 6000);
        QTRY_VERIFY_WITH_TIMEOUT(resultText.contains("ban accepted"), 6000);
        driver.stop();
        QCOMPARE(QUrlQuery(QString::fromUtf8(server.lastCommandBody(banEndpoint)))
            .queryItemValue(QStringLiteral("peers"), QUrl::FullyDecoded), QStringLiteral("192.0.2.1:61413"));

        // The other torrent has authoritative private metadata.
        table->selectRow(partialRow);
        QTRY_COMPARE_WITH_TIMEOUT(peers->rowCount(), 1, 6000);
        menuAction(QStringLiteral("addTorrentPeers"), false, true);
        QVERIFY(menuSeen && !enabled);
        driver.stop();
        QCOMPARE(server.commandsFor(addEndpoint), 1);
        table->selectRow(targetRow);

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
        QVERIFY(contents->header()->sectionsMovable());
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
        filter->setText(QStringLiteral("FILE-t?o.*"));
        QVERIFY(fileOne->isHidden());
        QVERIFY(!fileTwo->isHidden());
        const auto chooseFormat = [&](const QString &format) {
            bool found = false;
            QTimer::singleShot(0, &window, [&]() {
                auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
                if (!menu) return;
                for (auto *action : menu->actions())
                {
                    if (!action->menu()) continue;
                    for (auto *sub : action->menu()->actions())
                        if (sub->objectName() == QStringLiteral("contentFilter_") + format)
                        { found = true; sub->trigger(); }
                }
                menu->close();
            });
            QVERIFY(QMetaObject::invokeMethod(filter, "customContextMenuRequested",
                Qt::DirectConnection, Q_ARG(QPoint, QPoint(5, 5))));
            QVERIFY(found);
        };
        chooseFormat(QStringLiteral("regex"));
        filter->setText(QStringLiteral("^file-one[.]bin$"));
        QVERIFY(!fileOne->isHidden() && fileTwo->isHidden());
        filter->setText(QStringLiteral("["));
        QVERIFY(fileOne->isHidden() && fileTwo->isHidden());
        QVERIFY(!filter->toolTip().isEmpty());
        chooseFormat(QStringLiteral("plain"));
        filter->setText(QStringLiteral("file-two"));
        QVERIFY(fileOne->isHidden() && !fileTwo->isHidden());
        filter->clear();
        QVERIFY(!fileOne->isHidden() && !fileTwo->isHidden());
        chooseFormat(QStringLiteral("wildcards"));

        auto *contentHeader = contents->header();
        const auto headerAction = [&](const QString &name, bool enabled) {
            bool found = false, actualEnabled = false;
            QTimer::singleShot(0, &window, [&]() {
                auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
                if (!menu) return;
                for (auto *action : menu->actions())
                    if (action->objectName() == name)
                    {
                        found = true;
                        actualEnabled = action->isEnabled();
                        if (actualEnabled) action->trigger();
                    }
                menu->close();
            });
            QVERIFY(QMetaObject::invokeMethod(contentHeader, "customContextMenuRequested",
                Qt::DirectConnection, Q_ARG(QPoint, QPoint(10, 10))));
            QVERIFY(found);
            QCOMPARE(actualEnabled, enabled);
        };
        headerAction(QStringLiteral("contentColumn0"), false);
        QVERIFY(!contents->isColumnHidden(0));
        headerAction(QStringLiteral("contentColumn5"), true);
        QVERIFY(contents->isColumnHidden(5));
        headerAction(QStringLiteral("resizeContentColumns"), true);
        QVERIFY(contents->isColumnHidden(5));
        headerAction(QStringLiteral("contentColumn5"), true);
        QVERIFY(!contents->isColumnHidden(5));

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
        first.enableTorrentSyncDemo();
        first.keepDemoTorrents();
        second.enableNativePresentationDemo();
        // Distinct fake versions catch cross-server label leakage; assertions
        // use each server's actual configured responses, not fixed UI strings.
        first.setReportedVersions(QStringLiteral("v9.15-alpha"),
                                  QStringLiteral("3.1"));
        second.setReportedVersions(QStringLiteral("v8.6-beta"),
                                   QStringLiteral("7.4"));
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
        auto *count = window.statusBar()->findChild<QLabel *>(
            QStringLiteral("visibleTorrentCount"));
        QVERIFY(count);
        QTRY_COMPARE_WITH_TIMEOUT(count->text(), QStringLiteral("Torrents: 1 / 1"), 6000);
        QVERIFY(window.statusBar()->currentMessage().contains(first.reportedQbtVersion()));
        QVERIFY(window.statusBar()->currentMessage().contains(first.reportedApiVersion()));
        QVERIFY(window.statusBar()->currentMessage().contains("Unrecognized stable release"));

        openConnectDialog(window, second.url(), false, true, true,
                          QStringLiteral("Red seedbox"));
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 2, 6000);
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(
            QStringLiteral("Connected to Red seedbox")), 6000);
        QVERIFY2(tabs->tabBar()->isVisible(), "Two simultaneous servers need visible tabs.");
        QTRY_COMPARE_WITH_TIMEOUT(count->text(), QStringLiteral("Torrents: 2 / 2"), 6000);
        QVERIFY(window.statusBar()->currentMessage().contains(second.reportedQbtVersion()));
        QVERIFY(window.statusBar()->currentMessage().contains(second.reportedApiVersion()));
        QVERIFY(!window.statusBar()->currentMessage().contains(first.reportedQbtVersion()));

        auto *firstTable = tabs->widget(0)->findChild<QTableView *>(QStringLiteral("torrentList"));
        auto *secondTable = tabs->widget(1)->findChild<QTableView *>(QStringLiteral("torrentList"));
        QVERIFY(firstTable && secondTable);
        QVERIFY2(firstTable->model() != secondTable->model(),
                 "Each server must own independent torrent models and cookie jars.");

        auto *search = window.findChild<QLineEdit *>(QStringLiteral("torrentSearch"));
        QVERIFY(search);
        search->setText(QStringLiteral("red-isolated-filter"));
        QTRY_COMPARE_WITH_TIMEOUT(count->text(), QStringLiteral("Torrents: 0 / 2"), 2000);
        tabs->setCurrentIndex(0);
        QCOMPARE(search->text(), QString());
        QTRY_COMPARE(count->text(), QStringLiteral("Torrents: 1 / 1"));
        QVERIFY(window.statusBar()->currentMessage().contains(first.reportedQbtVersion()));
        QVERIFY(window.statusBar()->currentMessage().contains(first.reportedApiVersion()));
        QVERIFY(!window.statusBar()->currentMessage().contains(second.reportedQbtVersion()));
        tabs->setCurrentIndex(1);
        QCOMPARE(search->text(), QStringLiteral("red-isolated-filter"));
        QTRY_COMPARE(count->text(), QStringLiteral("Torrents: 0 / 2"));
        QVERIFY(window.statusBar()->currentMessage().contains(second.reportedQbtVersion()));

        const int priorFirst = first.syncRequests();
        const int priorSecond = second.syncRequests();
        QTRY_VERIFY_WITH_TIMEOUT(first.syncRequests() > priorFirst, 6000);
        QTRY_VERIFY_WITH_TIMEOUT(second.syncRequests() > priorSecond, 6000);

        // Disconnect the background server through its own tab, without
        // activating it or logging out of the currently selected server.
        tabs->setCurrentIndex(0);
        auto *bar = tabs->tabBar();
        bool disconnectedFromMenu = false;
        QTimer::singleShot(0, &window, [&]() {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu)
                return;
            for (auto *action : menu->actions())
                if (action->objectName() == QLatin1String("serverTabDisconnect"))
                {
                    disconnectedFromMenu = true;
                    action->trigger();
                    break;
                }
            menu->close();
        });
        QVERIFY(QMetaObject::invokeMethod(bar, "customContextMenuRequested",
            Qt::DirectConnection, Q_ARG(QPoint, bar->tabRect(1).center())));
        QVERIFY(disconnectedFromMenu);
        QTRY_COMPARE(tabs->count(), 2);
        QVERIFY(tabs->tabBar()->isVisible());
        QVERIFY(!tabs->widget(1)->isEnabled());
        QVERIFY(tabs->tabToolTip(1).contains(QStringLiteral("Disconnected manually")));
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(
            QStringLiteral("Connected to Demo seedbox")), 3000);
        QTRY_COMPARE(count->text(), QStringLiteral("Torrents: 1 / 1"));
        QVERIFY(window.statusBar()->currentMessage().contains(first.reportedQbtVersion()));
        QVERIFY(!window.statusBar()->currentMessage().contains(second.reportedQbtVersion()));
        QTRY_COMPARE_WITH_TIMEOUT(second.logouts(), 1, 6000);
        QCOMPARE(first.logouts(), 0);
        const int beforeLastPoll = first.syncRequests();
        QTRY_VERIFY_WITH_TIMEOUT(first.syncRequests() > beforeLastPoll, 6000);

        // The red close button removes only the disconnected tab, with no
        // second logout. A single active tab keeps the existing hidden strip.
        auto *closeButton = qobject_cast<QToolButton *>(bar->tabButton(1, QTabBar::RightSide));
        QVERIFY(closeButton && closeButton->isVisible());
        QCOMPARE(closeButton->text(), QStringLiteral("×"));
        QTest::mouseClick(closeButton, Qt::LeftButton);
        QTRY_COMPARE(tabs->count(), 1);
        QVERIFY(!tabs->tabBar()->isVisible());
        QCOMPARE(second.logouts(), 1);

        // Reopen the same saved server, then reject authenticated sync.
        // This is a real failure after a populated torrent snapshot.
        bool choseSecond = false;
        auto *connectAction = window.findChild<QAction *>(QStringLiteral("connectToServer"));
        QVERIFY(connectAction);
        QTimer::singleShot(0, &window, [&]() {
            auto *dialog = qobject_cast<ConnectionDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            const auto cleanup = qScopeGuard([dialog]() { if (dialog->isVisible()) dialog->reject(); });
            auto *profiles = dialog->findChild<QComboBox *>(QStringLiteral("savedServers"));
            auto *password = dialog->findChild<QLineEdit *>(QStringLiteral("serverPassword"));
            auto *button = dialog->findChild<QPushButton *>(QStringLiteral("connectServer"));
            QVERIFY(profiles && password && button);
            const int index = profiles->findText(QStringLiteral("Red seedbox"));
            QVERIFY(index >= 0);
            profiles->setCurrentIndex(index);
            password->setText(QStringLiteral("demo-password"));
            choseSecond = true;
            button->click();
        });
        connectAction->trigger();
        QVERIFY(choseSecond);
        QTRY_COMPARE(tabs->count(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(
            QStringLiteral("Connected to Red seedbox")), 6000);
        auto *reopenedTable = tabs->widget(1)->findChild<QTableView *>(QStringLiteral("torrentList"));
        QVERIFY(reopenedTable);
        // Reopening preserves the saved filter. Clear it to see the snapshot.
        search->clear();
        QTRY_COMPARE_WITH_TIMEOUT(reopenedTable->model()->rowCount(), 2, 6000);
        second.rejectSync(true);
        tabs->setCurrentIndex(0);
        QTRY_VERIFY_WITH_TIMEOUT(!tabs->widget(1)->isEnabled(), 6000);
        QCOMPARE(tabs->count(), 2);
        QVERIFY(tabs->tabToolTip(1).contains(QStringLiteral("stale")));
        QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("Connected to Demo seedbox")));
        tabs->setCurrentIndex(1);
        QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("Not connected to Red seedbox")));
        QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("stale")));
        QCOMPARE(reopenedTable->model()->rowCount(), 2);
        auto *reconnect = window.findChild<QAction *>(QStringLiteral("reconnectServer"));
        QVERIFY(reconnect && reconnect->isEnabled());

        // Close a connected background tab using its own red button.
        auto *firstClose = qobject_cast<QToolButton *>(bar->tabButton(0, QTabBar::RightSide));
        QVERIFY(firstClose && firstClose->isVisible());
        QTest::mouseClick(firstClose, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(first.logouts(), 1, 6000);
        QTRY_COMPARE(tabs->count(), 1);
        QCOMPARE(tabs->tabText(0), QStringLiteral("Red seedbox"));
        QVERIFY(!tabs->widget(0)->isEnabled());
    }

    void nativeMenuActionsFollowServerSessionState()
    {
        DemoQbittorrentServer legacy(true), modern(true);
        modern.setReportedVersions("v5.3.1", "2.16.2");
        modern.enableSessionPauseDemo();
        QVERIFY(legacy.start() && modern.start());
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QSettings settings(tmp.filePath("menu-parity.ini"), QSettings::IniFormat);
        settings.setValue("Preferences/Advanced/RefreshInterval", 100);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        auto *pause = window.findChild<QAction *>("actionPauseSession");
        auto *resume = window.findChild<QAction *>("actionResumeSession");
        auto *speed = window.findChild<QAction *>("actionSpeedInTitleBar");
        auto *docs = window.findChild<QAction *>("actionDocumentation");
        auto *about = window.findChild<QAction *>("actionAbout");
        QVERIFY(pause && resume && speed && docs && about);
        QVERIFY(pause->isVisible() && !pause->isEnabled() && !resume->isVisible());
        QCOMPARE(docs->shortcut(), QKeySequence(QKeySequence::HelpContents));
        QCOMPARE(window.findChild<QAction *>("actionCreateTorrent")->shortcut(),
                 QKeySequence(Qt::CTRL | Qt::Key_N));
        QCOMPARE(window.findChild<QAction *>("actionIncreaseQueuePos")->shortcut(),
                 QKeySequence(Qt::CTRL | Qt::Key_Plus));
        QVERIFY(!window.findChild<QMenu *>("menuHelp")->actions().contains(
            window.findChild<QAction *>("actionDonateMoney")));
        openConnectDialog(window, legacy.url());
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains("Connected to"), 6000);
        QVERIFY(pause->isVisible() && !pause->isEnabled() && !resume->isVisible());
        openConnectDialog(window, modern.url(), false, true, true, "Modern");
        QTRY_VERIFY_WITH_TIMEOUT(pause->isVisible() && pause->isEnabled(), 6000);
        QVERIFY(!resume->isVisible());
        speed->setChecked(true);
        QVERIFY(window.windowTitle().contains("D:") && window.windowTitle().contains("Modern"));
        QCOMPARE(settings.value("Layout/SpeedInTitleBar").toBool(), true);
        const int before = modern.syncRequests();
        pause->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(modern.sessionPaused() && resume->isVisible(), 6000);
        QVERIFY(window.windowTitle().contains("[PAUSED]"));
        QTRY_VERIFY_WITH_TIMEOUT(modern.syncRequests() > before, 6000);
        QCOMPARE(modern.commandsFor("/api/v2/transfer/pauseSession"), 1);
        QCOMPARE(legacy.commandsFor("/api/v2/transfer/pauseSession"), 0);
        QVERIFY(window.statusBar()->currentMessage().contains("Connected to Modern"));
        auto *tabs = window.findChild<QTabWidget *>("serverTabs");
        QVERIFY(tabs);
        tabs->setCurrentIndex(0);
        QVERIFY(pause->isVisible() && !pause->isEnabled() && !resume->isVisible());
        QVERIFY(!window.windowTitle().contains("[PAUSED]"));
        tabs->setCurrentIndex(1);
        QVERIFY(resume->isEnabled() && resume->isVisible());
        resume->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!modern.sessionPaused() && pause->isVisible(), 6000);
        QCOMPARE(modern.commandsFor("/api/v2/transfer/resumeSession"), 1);
        QCOMPARE(legacy.commandsFor("/api/v2/transfer/resumeSession"), 0);
        QCOMPARE(modern.commandsFor("/api/v2/torrents/stop"), 0);
        QCOMPARE(modern.commandsFor("/api/v2/torrents/start"), 0);
        modern.setSessionPaused(true);
        QTRY_VERIFY_WITH_TIMEOUT(resume->isVisible(), 6000);
        modern.setSessionPaused(false);
        QTRY_VERIFY_WITH_TIMEOUT(pause->isVisible(), 6000);
        speed->setChecked(false);
        QVERIFY(!window.windowTitle().contains("D:"));
        bool aboutChecked = false;
        QTimer::singleShot(0, &window, [&] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            QVERIFY(box);
            const auto guard = qScopeGuard([box] { box->accept(); });
            QCOMPARE(box->objectName(), QStringLiteral("aboutDialog"));
            QVERIFY(box->text().contains(QStringLiteral(QBT_REMOTE_DISPLAY_VERSION))
                    && box->text().contains("GPL-3.0-or-later"));
            QVERIFY(box->text().contains("Qt") && box->text().contains("qBittorrent-remote"));
            aboutChecked = true;
        });
        about->trigger();
        QVERIFY(aboutChecked);
    }

    void optionalTabCloseButtonsAndServerOrderStaySynchronized()
    {
        DemoQbittorrentServer alpha(true), beta(true);
        QVERIFY(alpha.start() && beta.start());
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QSettings settings(tmp.filePath("tab-order.ini"), QSettings::IniFormat);
        Remote::ProfileStore store(settings);
        Remote::ServerProfile first{QString(), "First", alpha.url(), "demo", true, true};
        Remote::ServerProfile second{QString(), "Second", beta.url(), "demo", true, true};
        QVERIFY(store.save(first) && store.save(second));
        DemoCredentialVault vault;
        vault.data.insert(vault.keyFor(first), "demo-password");
        vault.data.insert(vault.keyFor(second), "demo-password");
        settings.setValue("Layout/ActiveServer", second.id);
        settings.setValue("Layout/ShowRSSReader", true);
        settings.setValue("Layout/ShowExecutionLog", true);
        {
            MainWindow window(nullptr, &settings, &vault);
            window.show();
            auto *tabs = window.findChild<QTabWidget *>("serverTabs");
            QVERIFY(tabs && tabs->isMovable());
            QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 2, 6000);
            QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains("Connected to Second"), 6000);
            QWidget *selected = tabs->currentWidget();
            tabs->tabBar()->moveTab(1, 0);
            QCOMPARE(tabs->widget(0), selected);
            QCOMPARE(tabs->currentWidget(), selected);
            QCOMPARE(settings.value("Layout/ServerTabOrder").toStringList(), QStringList({second.id, first.id}));
            auto *rss = window.findChild<QAction *>("actionRemoteRSS");
            auto *log = window.findChild<QAction *>("actionExecutionLogs");
            auto *sections = selected->findChild<QTabWidget *>("serverSectionTabs");
            QVERIFY(rss && log && sections);
            QTRY_VERIFY_WITH_TIMEOUT(selected->findChild<QToolButton *>("rssTabCloseButton"), 6000);
            QTRY_VERIFY_WITH_TIMEOUT(selected->findChild<QToolButton *>("logTabCloseButton"), 6000);
            QVERIFY(!sections->tabBar()->tabButton(0, QTabBar::RightSide));
            auto *pane = qobject_cast<ServerPane *>(selected);
            QVERIFY(pane);
            pane->showRss();
            selected->findChild<QToolButton *>("rssTabCloseButton")->click();
            QVERIFY(!rss->isChecked());
            QVERIFY(pane->isTransfersSelected());
            QVERIFY(!settings.value("Layout/ShowRSSReader").toBool());
            pane->showLog();
            selected->findChild<QToolButton *>("logTabCloseButton")->click();
            QTRY_VERIFY(!log->isChecked());
            QVERIFY(pane->isTransfersSelected());
            QVERIFY(!settings.value("Layout/ShowExecutionLog").toBool());
            QVERIFY(!sections->tabBar()->isVisible());
            rss->setChecked(true);
            log->setChecked(true);
            QVERIFY(sections->tabBar()->isVisible());
            QVERIFY(selected->findChild<QToolButton *>("rssTabCloseButton"));
            QVERIFY(selected->findChild<QToolButton *>("logTabCloseButton"));
            QVERIFY(window.close());
        }
        MainWindow reopened(nullptr, &settings, &vault);
        reopened.show();
        auto *tabs = reopened.findChild<QTabWidget *>("serverTabs");
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 2, 6000);
        QCOMPARE(tabs->tabText(0), QStringLiteral("Second"));
        QCOMPARE(tabs->tabText(1), QStringLiteral("First"));
        QTRY_VERIFY_WITH_TIMEOUT(reopened.statusBar()->currentMessage().contains("Connected to Second"), 6000);
    }

    void nativeExecutionLogUsesIndependentCursorsAndFilters()
    {
        DemoQbittorrentServer alpha(true), beta(true);
        const auto msg = [](int id, int type, const QString &message) {
            return QJsonObject{{"id", id}, {"type", type}, {"timestamp", 1700000000}, {"message", message}};
        };
        const QJsonObject peer{{"id", 0}, {"timestamp", 1700000000}, {"ip", "192.0.2.5"},
            {"blocked", true}, {"reason", "test"}};
        alpha.setLogEntries({msg(0, 1, "First normal"), msg(1, 4, "First warning")}, {peer});
        beta.setLogEntries({msg(0, 2, "Second info")}, {});
        QVERIFY(alpha.start() && beta.start());
        QTemporaryDir tmp;
        QSettings settings(tmp.filePath("log.ini"), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        openConnectDialog(window, alpha.url());
        QTRY_VERIFY(window.statusBar()->currentMessage().contains("Connected to"));
        auto *show = window.findChild<QAction *>("actionExecutionLogs");
        auto *warnings = window.findChild<QAction *>("actionWarningMessages");
        QVERIFY(show && warnings && !show->isChecked());
        show->setChecked(true);
        auto *tabs = window.findChild<QTabWidget *>("serverTabs");
        QVERIFY(tabs);
        auto *first = tabs->currentWidget();
        auto *general = first->findChild<QListWidget *>("remoteGeneralLog");
        auto *peers = first->findChild<QListWidget *>("remotePeerLog");
        QVERIFY(general && peers);
        QTRY_COMPARE_WITH_TIMEOUT(general->count(), 2, 6000);
        QTRY_COMPARE_WITH_TIMEOUT(peers->count(), 1, 6000);
        QVERIFY(general->item(0)->text().contains("First warning"));
        QVERIFY(peers->item(0)->text().contains("192.0.2.5"));
        warnings->setChecked(false);
        QVERIFY(general->item(0)->isHidden());
        warnings->setChecked(true);
        QVERIFY(!general->item(0)->isHidden());
        alpha.setLogEntries({msg(0, 1, "First normal"), msg(1, 4, "First warning"), msg(2, 8, "First critical")}, {peer});
        QTRY_COMPARE_WITH_TIMEOUT(general->count(), 3, 6000);
        QVERIFY(general->item(0)->text().contains("First critical"));
        openConnectDialog(window, beta.url(), false, true, true, "Second");
        QTRY_VERIFY(window.statusBar()->currentMessage().contains("Connected to Second"));
        auto *second = qobject_cast<ServerPane *>(tabs->currentWidget());
        QVERIFY(second && second != first);
        second->showLog();
        auto *secondLog = second->findChild<QListWidget *>("remoteGeneralLog");
        QVERIFY(secondLog);
        QTRY_COMPARE_WITH_TIMEOUT(secondLog->count(), 1, 6000);
        QVERIFY(secondLog->item(0)->text().contains("Second info"));
        QCOMPARE(general->count(), 3);
        show->setChecked(false);
        QVERIFY(!first->findChild<QListWidget *>("remoteGeneralLog"));
        QVERIFY(!second->findChild<QListWidget *>("remoteGeneralLog"));
    }

    void nativeCookiesEditOnlySelectedServer()
    {
        DemoQbittorrentServer alpha(true), beta(true);
        const QJsonArray initial{QJsonObject{{"domain", "example.test"}, {"path", "/"},
            {"name", "test"}, {"value", "a+b&c%20"}, {"expirationDate", -1}}};
        alpha.setDownloadCookies(initial);
        beta.setDownloadCookies(initial);
        QVERIFY(alpha.start() && beta.start());
        QTemporaryDir tmp;
        QSettings settings(tmp.filePath("cookies.ini"), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        auto *action = window.findChild<QAction *>("actionManageCookies");
        QVERIFY(action && !action->isEnabled());
        openConnectDialog(window, alpha.url());
        QTRY_VERIFY(action->isEnabled());
        openConnectDialog(window, beta.url(), false, true, true, "Second");
        QTRY_VERIFY(window.statusBar()->currentMessage().contains("Connected to Second"));
        for (bool save : {false, true})
        {
            bool edited = false;
            QTimer::singleShot(0, &window, [&] {
                auto *dlg = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                QVERIFY(dlg);
                const auto guard = qScopeGuard([dlg] { if (dlg->isVisible()) dlg->QDialog::reject(); });
                QCOMPARE(dlg->objectName(), QStringLiteral("remoteCookiesDialog"));
                auto *view = dlg->findChild<QTreeView *>("treeView");
                auto *buttons = dlg->findChild<QDialogButtonBox *>("buttonBox");
                QVERIFY(view && buttons);
                auto *ok = buttons->button(QDialogButtonBox::Ok);
                QTRY_VERIFY_WITH_TIMEOUT(ok->isEnabled(), 6000);
                QCOMPARE(view->model()->rowCount(), 1);
                QCOMPARE(view->model()->index(0,3).data().toString(), QStringLiteral("a+b&c%20"));
                QVERIFY(view->model()->setData(view->model()->index(0,3), "new+&%25", Qt::EditRole));
                if (save)
                {
                    ok->click();
                    QTRY_VERIFY_WITH_TIMEOUT(!dlg->isVisible(), 6000);
                }
                else buttons->button(QDialogButtonBox::Cancel)->click();
                edited = true;
            });
            action->trigger();
            QVERIFY(edited);
            QCOMPARE(beta.commandsFor("/api/v2/app/setCookies"), save ? 1 : 0);
        }
        QCOMPARE(alpha.downloadCookies(), initial);
        QCOMPARE(alpha.commandsFor("/api/v2/app/setCookies"), 0);
        QCOMPARE(beta.downloadCookies().first().toObject().value("value").toString(), QStringLiteral("new+&%25"));
    }

    void queueControlsFollowSelectedServerCapability()
    {
        DemoQbittorrentServer alpha(true), beta(true);
        const QString hash = "0123456789abcdef0123456789abcdef01234567";
        const auto snapshot = [&](bool enabled) {
            return QJsonObject{{"server_state", QJsonObject{{"queueing", enabled}}},
                {"torrents", QJsonObject{{hash, QJsonObject{{"name", "Test download"},
                    {"progress", 0.5}, {"state", "downloading"}, {"priority", 1}}}}}};
        };
        alpha.setCompletionSnapshot(snapshot(true));
        beta.setCompletionSnapshot(snapshot(false));
        QVERIFY(alpha.start() && beta.start());
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QSettings settings(tmp.filePath("queue-controls.ini"), QSettings::IniFormat);
        settings.setValue("Preferences/Advanced/RefreshInterval", 100);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        auto *menuAction = window.findChild<QAction *>("actionTopQueuePos");
        auto *toolbarAction = window.findChild<QAction *>("actionTopQueuePosToolbar");
        auto *separator = window.findChild<QAction *>("queueToolbarSeparator");
        QVERIFY(menuAction && toolbarAction && separator);
        QVERIFY(menuAction->isVisible() && !menuAction->isEnabled());
        QVERIFY(!toolbarAction->isVisible() && !separator->isVisible());
        openConnectDialog(window, alpha.url());
        QTRY_VERIFY_WITH_TIMEOUT(toolbarAction->isVisible(), 6000);
        QVERIFY(separator->isVisible());
        auto *tabs = window.findChild<QTabWidget *>("serverTabs");
        QVERIFY(tabs);
        auto *first = qobject_cast<ServerPane *>(tabs->currentWidget());
        QVERIFY(first);
        const int column = first->model()->columnCount() - 1;
        QVERIFY(!first->table()->isColumnHidden(column));
        first->table()->selectRow(0);
        QTRY_VERIFY(menuAction->isEnabled() && toolbarAction->isEnabled());
        toolbarAction->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(alpha.commandsFor("/api/v2/torrents/topPrio"), 1, 6000);
        openConnectDialog(window, beta.url(), false, true, true, "Second");
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains("Connected to Second"), 6000);
        auto *second = qobject_cast<ServerPane *>(tabs->currentWidget());
        QVERIFY(second && second != first);
        QTRY_COMPARE(second->model()->rowCount(), 1);
        second->table()->selectRow(0);
        QVERIFY(second->table()->isColumnHidden(column));
        QVERIFY(menuAction->isVisible() && !menuAction->isEnabled());
        QVERIFY(!toolbarAction->isVisible() && !separator->isVisible());
        menuAction->trigger();
        QCOMPARE(beta.commandsFor("/api/v2/torrents/topPrio"), 0);
        beta.queueCompletionPatch({{"server_state", QJsonObject{{"queueing", true}}}});
        QTRY_VERIFY_WITH_TIMEOUT(toolbarAction->isVisible() && toolbarAction->isEnabled(), 6000);
        QVERIFY(!second->table()->isColumnHidden(column));
        beta.queueCompletionPatch({{"server_state", QJsonObject{{"queueing", false}}}});
        QTRY_VERIFY_WITH_TIMEOUT(!toolbarAction->isVisible(), 6000);
        QVERIFY(second->table()->isColumnHidden(column));
        tabs->setCurrentWidget(first);
        QVERIFY(toolbarAction->isVisible() && !first->table()->isColumnHidden(column));
        QVERIFY(menuAction->isEnabled());
        QCOMPARE(beta.commandsFor("/api/v2/torrents/topPrio"), 0);
    }

    void nativeStatisticsTracksIndependentServerSnapshots()
    {
        DemoQbittorrentServer legacy(true), modern(true);
        modern.enableModernResponses();
        legacy.setBuildInfo({{"libtorrent", "1.2.20.0"}, {"qt", "6.8.1"}});
        modern.setBuildInfo({{"libtorrent", "2.1.0.0"}, {"qt", "6.10.1"}});
        const qint64 launched = QDateTime::currentSecsSinceEpoch() - 90061;
        modern.setProcessInfo({{"launch_time", QJsonValue(launched)}});
        const QJsonObject first{
            {"alltime_ul", 4096}, {"alltime_dl", 1024}, {"global_ratio", "4.00"},
            {"total_wasted_session", 64}, {"total_peer_connections", 8},
            {"read_cache_hits", "12.50"}, {"total_buffers_size", 16384},
            {"write_cache_overload", "25.00"}, {"read_cache_overload", "0"},
            {"queued_io_jobs", 3}, {"average_time_queue", 12}, {"total_queued_size", 2048},
            {"request_latency", 17}, {"queued_tracker_announces", 5}
        };
        QJsonObject second = first;
        second.insert("alltime_ul", 8192);
        second.insert("total_peer_connections", 0);
        second.insert("queued_tracker_announces", 99);
        legacy.setCompletionSnapshot({{"server_state", first}});
        modern.setCompletionSnapshot({{"server_state", second}});
        QVERIFY(legacy.start() && modern.start());
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QSettings settings(tmp.filePath("statistics.ini"), QSettings::IniFormat);
        settings.setValue("Preferences/Advanced/RefreshInterval", 100);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        auto *action = window.findChild<QAction *>("actionStatistics");
        QVERIFY(action && !action->isEnabled() && !action->icon().isNull());
        openConnectDialog(window, legacy.url());
        QTRY_VERIFY_WITH_TIMEOUT(action->isEnabled(), 6000);
        openConnectDialog(window, modern.url(), false, true, true, "Modern");
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains("v5.3.0"), 6000);
        auto *tabs = window.findChild<QTabWidget *>("serverTabs");
        QVERIFY(tabs);
        tabs->setCurrentIndex(0);
        action->trigger();
        auto *alpha = window.findChild<RemoteStatisticsDialog *>();
        QVERIFY(alpha && alpha->isVisible() && !alpha->isModal());
        const auto label = [](RemoteStatisticsDialog *dialog, const char *name) {
            return dialog->findChild<QLabel *>(QLatin1String(name));
        };
        QTRY_COMPARE_WITH_TIMEOUT(label(alpha, "labelPeers")->text(), QStringLiteral("8"), 6000);
        QTRY_VERIFY_WITH_TIMEOUT(label(alpha, "labelCacheHits")->isVisible(), 6000);
        QCOMPARE(label(alpha, "labelServerVersion")->text(), legacy.reportedQbtVersion());
        QCOMPARE(label(alpha, "labelApiVersion")->text(), legacy.reportedApiVersion());
        QCOMPARE(label(alpha, "labelLibtorrentVersion")->text(), QStringLiteral("1.2.20.0"));
        QCOMPARE(label(alpha, "labelQtVersion")->text(), QStringLiteral("6.8.1"));
        QTRY_COMPARE_WITH_TIMEOUT(legacy.processInfoRequests(), 1, 6000);
        const auto legacySyncAfterProbe = legacy.syncRequests();
        QTRY_VERIFY_WITH_TIMEOUT(legacy.syncRequests() > legacySyncAfterProbe, 6000);
        QCOMPARE(label(alpha, "labelLaunchTime")->text(), QStringLiteral("—"));
        QCOMPARE(label(alpha, "labelUptime")->text(), QStringLiteral("—"));
        QCOMPARE(label(alpha, "labelAlltimeUL")->text(), QLocale().toString(4.0, 'f', 2) + " KiB");
        QCOMPARE(label(alpha, "labelGlobalRatio")->text(), QLocale().toString(4.0, 'f', 2));
        QCOMPARE(label(alpha, "labelCacheHits")->text(), QLocale().toString(12.5, 'f', 2) + "%");
        QCOMPARE(label(alpha, "labelWaste")->text(), QStringLiteral("64 B"));
        QCOMPARE(label(alpha, "labelTotalBuf")->text(), QLocale().toString(16.0, 'f', 2) + " KiB");
        QCOMPARE(label(alpha, "labelWriteStarve")->text(), QLocale().toString(25.0, 'f', 2) + "%");
        QCOMPARE(label(alpha, "labelReadStarve")->text(), QStringLiteral("0%"));
        QCOMPARE(label(alpha, "labelQueuedJobs")->text(), QStringLiteral("3"));
        QCOMPARE(label(alpha, "labelJobsTime")->text(), QStringLiteral("12 μs"));
        QCOMPARE(label(alpha, "labelQueuedBytes")->text(), QLocale().toString(2.0, 'f', 2) + " KiB");
        QCOMPARE(label(alpha, "labelRequestLatency")->text(), QStringLiteral("17 μs"));
        QCOMPARE(label(alpha, "labelQueuedTrackerAnnounces")->text(), QStringLiteral("5"));
        action->trigger();
        QCOMPARE(window.findChildren<RemoteStatisticsDialog *>().size(), 1);
        tabs->setCurrentIndex(1);
        action->trigger();
        RemoteStatisticsDialog *beta = nullptr;
        for (auto *dialog : window.findChildren<RemoteStatisticsDialog *>())
            if (dialog != alpha) beta = dialog;
        QVERIFY(beta && beta->isVisible());
        QVERIFY(beta->windowTitle().contains("Modern"));
        QTRY_COMPARE_WITH_TIMEOUT(label(beta, "labelPeers")->text(), QStringLiteral("0"), 6000);
        QCOMPARE(label(beta, "labelQueuedTrackerAnnounces")->text(), QStringLiteral("99"));
        QVERIFY(!label(beta, "labelCacheHits")->isVisible());
        QCOMPARE(label(beta, "labelServerVersion")->text(), modern.reportedQbtVersion());
        QCOMPARE(label(beta, "labelApiVersion")->text(), modern.reportedApiVersion());
        QTRY_COMPARE_WITH_TIMEOUT(label(beta, "labelQtVersion")->text(), QStringLiteral("6.10.1"), 6000);
        QCOMPARE(label(beta, "labelLibtorrentVersion")->text(), QStringLiteral("2.1.0.0"));
        QTRY_COMPARE_WITH_TIMEOUT(label(beta, "labelLaunchTime")->text(),
            QLocale().toString(QDateTime::fromSecsSinceEpoch(launched), QLocale::ShortFormat), 6000);
        QVERIFY(label(beta, "labelUptime")->text().startsWith(QStringLiteral("1 d 01:")));
        QCOMPARE(modern.processInfoRequests(), 1);
        QCOMPARE(label(alpha, "labelLaunchTime")->text(), QStringLiteral("—"));
        QCOMPARE(label(alpha, "labelQtVersion")->text(), QStringLiteral("6.8.1"));
        legacy.queueCompletionPatch({{"server_state", QJsonObject{{"queued_tracker_announces", 6}}}});
        QTRY_COMPARE_WITH_TIMEOUT(label(alpha, "labelQueuedTrackerAnnounces")->text(), QStringLiteral("6"), 6000);
        QCOMPARE(label(alpha, "labelPeers")->text(), QStringLiteral("8"));
        QCOMPARE(label(beta, "labelQueuedTrackerAnnounces")->text(), QStringLiteral("99"));
        modern.queueCompletionPatch({{"full_update", true}, {"server_state", QJsonObject{{"total_peer_connections", 2}}}});
        QTRY_COMPARE_WITH_TIMEOUT(label(beta, "labelPeers")->text(), QStringLiteral("2"), 6000);
        QCOMPARE(label(beta, "labelQueuedTrackerAnnounces")->text(), QStringLiteral("—"));
        QCOMPARE(label(alpha, "labelQueuedTrackerAnnounces")->text(), QStringLiteral("6"));
        // Reopening after an invalid response must not reuse another dialog\'s data.
        beta->close();
        QTRY_COMPARE(window.findChildren<RemoteStatisticsDialog *>().size(), 1);
        modern.setBuildInfo({{"libtorrent", false}, {"qt", 123}});
        modern.setProcessInfo({{"launch_time", "not a timestamp"}});
        action->trigger();
        beta = nullptr;
        for (auto *dialog : window.findChildren<RemoteStatisticsDialog *>())
            if (dialog != alpha) beta = dialog;
        QVERIFY(beta && beta->isVisible());
        QTRY_COMPARE_WITH_TIMEOUT(modern.processInfoRequests(), 2, 6000);
        const auto modernSyncAfterProbe = modern.syncRequests();
        QTRY_VERIFY_WITH_TIMEOUT(modern.syncRequests() > modernSyncAfterProbe, 6000);
        QCOMPARE(label(beta, "labelPeers")->text(), QStringLiteral("2"));
        QCOMPARE(label(beta, "labelLibtorrentVersion")->text(), QStringLiteral("—"));
        QCOMPARE(label(beta, "labelQtVersion")->text(), QStringLiteral("—"));
        QCOMPARE(label(beta, "labelLaunchTime")->text(), QStringLiteral("—"));
        QCOMPARE(label(beta, "labelUptime")->text(), QStringLiteral("—"));
        // Closing one server closes only its statistics window.
        auto *disconnect = window.findChild<QAction *>("disconnectServer");
        QVERIFY(disconnect && disconnect->isEnabled());
        disconnect->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(window.findChildren<RemoteStatisticsDialog *>().size(), 1, 6000);
        QVERIFY(alpha->isVisible());
        QVERIFY(!action->isEnabled());
        tabs->setCurrentIndex(0);
        QVERIFY(action->isEnabled());
        alpha->close();
        QTRY_COMPARE(window.findChildren<RemoteStatisticsDialog *>().size(), 0);
    }

    void nativeTorrentCreatorUsesSelectedRemoteServer()
    {
        DemoQbittorrentServer legacy(true), modern(true);
        modern.enableModernResponses();
        legacy.enableCreatorDemo();
        modern.enableCreatorDemo();
        legacy.setBuildInfo({{"libtorrent", "1.2.20.0"}});
        modern.setBuildInfo({{"libtorrent", "2.1.0.0"}});
        QVERIFY(legacy.start() && modern.start());
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QSettings settings(tmp.filePath("profiles.ini"), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        auto *action = window.findChild<QAction *>("actionCreateTorrent");
        QVERIFY(action && !action->isEnabled());
        openConnectDialog(window, legacy.url());
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains("v5.1.4"), 6000);
        openConnectDialog(window, modern.url(), false, true, true, "Modern");
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains("v5.3.0"), 6000);
        auto *tabs = window.findChild<QTabWidget *>("serverTabs");
        QVERIFY(tabs && action->isEnabled());
        const QString tracker = QStringLiteral("https://tracker.example/announce?a=x%2Fy&b=1+2");
        for (const bool newer : {false, true})
        {
            tabs->setCurrentIndex(newer ? 1 : 0);
            const QString destination = tmp.filePath(newer ? "modern.torrent" : "legacy.torrent");
            bool finished = false;
            QTimer::singleShot(0, &window, [&] {
                auto *dlg = qobject_cast<RemoteTorrentCreatorDialog *>(QApplication::activeModalWidget());
                QVERIFY(dlg);
                const auto cleanup = qScopeGuard([dlg] { if (dlg->isVisible()) dlg->QDialog::reject(); });
                auto *buttons = dlg->findChild<QDialogButtonBox *>("buttonBox");
                auto *input = dlg->findChild<QLineEdit *>("textInputPath");
                auto *output = dlg->findChild<QLineEdit *>("creatorLocalOutput");
                auto *format = dlg->findChild<QComboBox *>("comboTorrentFormat");
                auto *piece = dlg->findChild<QComboBox *>("comboPieceSize");
                auto *priv = dlg->findChild<QCheckBox *>("checkPrivate");
                auto *seed = dlg->findChild<QCheckBox *>("checkStartSeeding");
                auto *dotfiles = dlg->findChild<QCheckBox *>("checkIgnoreDotfiles");
                auto *alignment = dlg->findChild<QGroupBox *>("checkOptimizeAlignment");
                auto *padding = dlg->findChild<QSpinBox *>("spinPaddedFileSizeLimit");
                auto *trackers = dlg->findChild<QTextEdit *>("trackersList");
                auto *seeds = dlg->findChild<QTextEdit *>("URLSeedsList");
                auto *comment = dlg->findChild<QTextEdit *>("txtComment");
                auto *source = dlg->findChild<QLineEdit *>("lineEditSource");
                auto *progress = dlg->findChild<QProgressBar *>("progressBar");
                auto *status = dlg->findChild<QLabel *>("creatorTaskStatus");
                QVERIFY(buttons && input && output && format && piece && priv && seed && dotfiles
                    && alignment && padding && trackers && seeds && comment && source && progress && status);
                auto *seedServer = dlg->findChild<QComboBox *>("seedServerCombo");
                QVERIFY(seedServer);
                QCOMPARE(seedServer->count(), 2);
                QCOMPARE(seedServer->currentText(), newer ? QStringLiteral("Modern") : QStringLiteral("Demo seedbox"));
                QVERIFY(trackers->toPlainText().isEmpty() && source->text().isEmpty());
                auto *create = buttons->button(QDialogButtonBox::Ok);
                QTRY_VERIFY_WITH_TIMEOUT(create->isEnabled(), 6000);
                QCOMPARE(format->isVisible(), newer);
                QCOMPARE(dotfiles->isVisible(), newer);
                QCOMPARE(alignment->isVisible(), !newer);
                auto *folderBrowse = dlg->findChild<QToolButton *>("addFolderButton");
                auto *fileBrowse = dlg->findChild<QToolButton *>("addFileButton");
                QVERIFY(folderBrowse && fileBrowse && folderBrowse->menu() && fileBrowse->menu());
                QCOMPARE(folderBrowse->popupMode(), QToolButton::MenuButtonPopup);
                QCOMPARE(fileBrowse->popupMode(), QToolButton::MenuButtonPopup);
                input->setText("/browse/Space & 100%/sample file.txt");
                {
                    QTimer pick;
                    connect(&pick, &QTimer::timeout, &pick, [] {
                        auto *picker = qobject_cast<RemoteFileDialog *>(QApplication::activeModalWidget());
                        if (!picker) return;
                        auto *tree = picker->findChild<QTreeWidget *>("remoteBrowserEntries");
                        auto *box = picker->findChild<QDialogButtonBox *>();
                        if (!tree || !box) return;
                        for (int i = 0; i < tree->topLevelItemCount(); ++i)
                            if (tree->topLevelItem(i)->text(0) == QStringLiteral("sample file.txt"))
                                tree->setCurrentItem(tree->topLevelItem(i));
                        if (box->button(QDialogButtonBox::Ok)->isEnabled())
                            box->button(QDialogButtonBox::Ok)->click();
                    });
                    QTimer deadline;
                    deadline.setSingleShot(true);
                    connect(&deadline, &QTimer::timeout, &deadline, [] {
                        if (auto *picker = qobject_cast<RemoteFileDialog *>(QApplication::activeModalWidget())) picker->reject();
                        if (auto *popup = QApplication::activePopupWidget()) popup->close();
                    });
                    pick.start(10);
                    deadline.start(4000);
                    fileBrowse->click();
                    QCOMPARE(input->property("explicitRemotePath").toString(), QStringLiteral("/browse/Space & 100%/sample file.txt"));
                }
                input->setText("/browse");
                {
                QTimer pick;
                connect(&pick, &QTimer::timeout, &pick, [&] {
                    auto *picker = qobject_cast<RemoteFileDialog *>(QApplication::activeModalWidget());
                    if (!picker) return;
                    auto *box = picker->findChild<QDialogButtonBox *>();
                    if (box && box->button(QDialogButtonBox::Ok)->isEnabled())
                        box->button(QDialogButtonBox::Ok)->click();
                });
                QTimer deadline;
                deadline.setSingleShot(true);
                connect(&deadline, &QTimer::timeout, &deadline, [] {
                    if (auto *picker = qobject_cast<RemoteFileDialog *>(QApplication::activeModalWidget())) picker->reject();
                    if (auto *popup = QApplication::activePopupWidget()) popup->close();
                });
                pick.start(10);
                deadline.start(4000);
                    folderBrowse->click();
                    QCOMPARE(input->property("explicitRemotePath").toString(), QStringLiteral("/browse"));
                }
                input->setText("/share/Media & Music/Album");
                output->setText(destination);
                piece->setCurrentIndex(piece->findData(262144));
                priv->setChecked(true);
                seed->setChecked(newer);
                if (newer) { format->setCurrentIndex(format->findData("v1")); dotfiles->setChecked(false); }
                else { alignment->setChecked(true); padding->setValue(64); }
                trackers->setPlainText(tracker + "\n\nudp://tracker2.example:80/announce");
                seeds->setPlainText("https://seed.example/file%20name");
                comment->setPlainText("A comment & more");
                source->setText("TEST");
                create->click();
                QTRY_VERIFY_WITH_TIMEOUT(progress->value() == 43 && progress->maximum() == 100, 6000);
                // Wait for the server to acknowledge completed-task cleanup, not just the local save.
                QTRY_COMPARE_WITH_TIMEOUT(status->text(), newer
                    ? QStringLiteral("Torrent saved locally. Immediate seeding was requested on the server.")
                    : QStringLiteral("Torrent saved locally. Seeding was not requested."), 10000);
                QVERIFY(create->isEnabled());
                QFile file(destination);
                QVERIFY(file.open(QIODevice::ReadOnly));
                QCOMPARE(file.readAll(), newer ? modern.exportedTorrent() : legacy.exportedTorrent());
                QCOMPARE(progress->value(), 100);
                create->click();
                finished = true;
            });
            action->trigger();
            QVERIFY(finished);
            auto &server = newer ? modern : legacy;
            QCOMPARE(server.commandsFor("/api/v2/torrentcreator/addTask"), 1);
            QCOMPARE(server.commandsFor("/api/v2/torrentcreator/deleteTask"), 1);
            QCOMPARE(server.creatorDownloads(), 1);
            QVERIFY(!server.creatorActive());
            const QUrlQuery request(QString::fromUtf8(server.lastCommandBody("/api/v2/torrentcreator/addTask")));
            const auto value = [&](const QString &key) { return request.queryItemValue(key, QUrl::FullyDecoded); };
            QCOMPARE(value("sourcePath"), QStringLiteral("/share/Media & Music/Album"));
            QCOMPARE(value("private"), QStringLiteral("true"));
            QCOMPARE(value("startSeeding"), newer ? QStringLiteral("true") : QStringLiteral("false"));
            QCOMPARE(value("pieceSize"), QStringLiteral("262144"));
            QCOMPARE(value("comment"), QStringLiteral("A comment & more"));
            QCOMPARE(value("source"), QStringLiteral("TEST"));
            QVERIFY(!request.hasQueryItem("torrentFilePath"));
            const QString expected = tracker + "||udp://tracker2.example:80/announce";
            QStringList urls = value("trackers").split('|');
            if (newer)
                for (auto &url : urls) url = QUrl::fromPercentEncoding(url.toLatin1());
            QCOMPARE(urls.join('|'), expected);
            if (newer)
            {
                QCOMPARE(value("format"), QStringLiteral("v1"));
                QCOMPARE(value("ignoreDotfiles"), QStringLiteral("false"));
                QCOMPARE(QUrl::fromPercentEncoding(value("urlSeeds").toLatin1()), QStringLiteral("https://seed.example/file%20name"));
                QVERIFY(!request.hasQueryItem("optimizeAlignment"));
            }
            else
            {
                QCOMPARE(value("optimizeAlignment"), QStringLiteral("true"));
                QCOMPARE(value("paddedFileSizeLimit"), QStringLiteral("65536"));
                QVERIFY(!request.hasQueryItem("format") && !request.hasQueryItem("ignoreDotfiles"));
                QCOMPARE(value("urlSeeds"), QStringLiteral("https://seed.example/file%20name"));
            }
        }

        // Reopen each creator after saving: reusable inputs persist but filenames
        // are blank, and edits to one profile must not leak into the other.
        for (const bool newer : {false, true})
        {
            tabs->setCurrentIndex(newer ? 1 : 0);
            bool restored = false;
            QTimer::singleShot(0, &window, [&] {
                auto *dlg = qobject_cast<RemoteTorrentCreatorDialog *>(QApplication::activeModalWidget());
                QVERIFY(dlg);
                const auto cleanup = qScopeGuard([dlg] { if (dlg->isVisible()) dlg->QDialog::reject(); });
                auto *create = dlg->findChild<QDialogButtonBox *>("buttonBox")->button(QDialogButtonBox::Ok);
                QTRY_VERIFY_WITH_TIMEOUT(create->isEnabled(), 6000);
                QCOMPARE(dlg->findChild<QTextEdit *>("trackersList")->toPlainText(), tracker + "\n\nudp://tracker2.example:80/announce");
                QCOMPARE(dlg->findChild<QTextEdit *>("URLSeedsList")->toPlainText(), QStringLiteral("https://seed.example/file%20name"));
                QCOMPARE(dlg->findChild<QTextEdit *>("txtComment")->toPlainText(), QStringLiteral("A comment & more"));
                QCOMPARE(dlg->findChild<QLineEdit *>("lineEditSource")->text(), QStringLiteral("TEST"));
                QCOMPARE(dlg->findChild<QComboBox *>("comboPieceSize")->currentData().toInt(), 262144);
                QVERIFY(dlg->findChild<QCheckBox *>("checkPrivate")->isChecked());
                QCOMPARE(dlg->findChild<QCheckBox *>("checkStartSeeding")->isChecked(), newer);
                QVERIFY(dlg->findChild<QLineEdit *>("textInputPath")->text().isEmpty());
                QVERIFY(dlg->findChild<QLineEdit *>("creatorLocalOutput")->text().isEmpty());
                if (newer)
                    QCOMPARE(dlg->findChild<QComboBox *>("comboTorrentFormat")->currentData().toString(), QStringLiteral("v1"));
                else
                    dlg->findChild<QLineEdit *>("lineEditSource")->setText("LEGACY ONLY");
                dlg->reject();
                restored = true;
            });
            action->trigger();
            QVERIFY(restored);
        }

        // Cancel a live task without downloading or creating another torrent.
        modern.enableCreatorDemo(true);
        tabs->setCurrentIndex(1);
        bool cancelled = false;
        QTimer::singleShot(0, &window, [&] {
            auto *dlg = qobject_cast<RemoteTorrentCreatorDialog *>(QApplication::activeModalWidget());
            QVERIFY(dlg);
            const auto cleanup = qScopeGuard([dlg] { if (dlg->isVisible()) dlg->QDialog::reject(); });
            auto *buttons = dlg->findChild<QDialogButtonBox *>("buttonBox");
            auto *create = buttons->button(QDialogButtonBox::Ok);
            QTRY_VERIFY_WITH_TIMEOUT(create->isEnabled(), 6000);
            dlg->findChild<QLineEdit *>("textInputPath")->setText("/share/cancel");
            dlg->findChild<QLineEdit *>("creatorLocalOutput")->setText(tmp.filePath("cancel.torrent"));
            create->click();
            QTRY_VERIFY_WITH_TIMEOUT(modern.creatorActive(), 6000);
            auto *progress = dlg->findChild<QProgressBar *>("progressBar");
            QTRY_VERIFY_WITH_TIMEOUT(progress->value() == 43, 6000);
            QTimer driver;
            connect(&driver, &QTimer::timeout, dlg, [] {
                if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                    if (box->windowTitle() == "Remove torrent creation task")
                        box->button(QMessageBox::Yes)->click();
            });
            driver.start(20);
            buttons->button(QDialogButtonBox::Cancel)->click();
            QTRY_VERIFY_WITH_TIMEOUT(!dlg->isVisible(), 6000);
            cancelled = true;
        });
        action->trigger();
        QVERIFY(cancelled);
        QCOMPARE(modern.commandsFor("/api/v2/torrentcreator/addTask"), 2);
        QCOMPARE(modern.commandsFor("/api/v2/torrentcreator/deleteTask"), 2);
        QCOMPARE(modern.creatorDownloads(), 1);
        QCOMPARE(legacy.commandsFor("/api/v2/torrentcreator/addTask"), 1);
        QVERIFY(!QFileInfo::exists(tmp.filePath("cancel.torrent")));

        // Hash on legacy, seed on the independently authenticated modern
        // instance using its mapped data path. Never add to the hashing server.
        const auto profiles = Remote::ProfileStore(settings).profiles();
        QCOMPARE(profiles.size(), 2);
        for (const auto &profile : profiles)
            QVERIFY(Remote::PathMappingStore(settings).replace(profile.id,
                {{tmp.path(), profile.name == "Modern" ? QStringLiteral("/share/modern") : QStringLiteral("/share/legacy")}}));
        tabs->setCurrentIndex(0);
        bool crossSeeded = false;
        const QString crossFile = tmp.filePath("cross.torrent");
        QTimer::singleShot(0, &window, [&] {
            auto *dlg = qobject_cast<RemoteTorrentCreatorDialog *>(QApplication::activeModalWidget());
            QVERIFY(dlg);
            const auto cleanup = qScopeGuard([dlg] { if (dlg->isVisible()) dlg->QDialog::reject(); });
            auto *create = dlg->findChild<QDialogButtonBox *>("buttonBox")->button(QDialogButtonBox::Ok);
            QTRY_VERIFY_WITH_TIMEOUT(create->isEnabled(), 6000);
            QCOMPARE(dlg->findChild<QLineEdit *>("lineEditSource")->text(), QStringLiteral("LEGACY ONLY"));
            dlg->findChild<QLineEdit *>("textInputPath")->setText("/share/legacy/test");
            dlg->findChild<QLineEdit *>("creatorLocalOutput")->setText(crossFile);
            auto *seed = dlg->findChild<QCheckBox *>("checkStartSeeding");
            auto *server = dlg->findChild<QComboBox *>("seedServerCombo");
            auto *path = dlg->findChild<QLineEdit *>("seedTargetPath");
            QVERIFY(seed && server && path && !server->isEnabled());
            seed->setChecked(true);
            server->setCurrentIndex(server->findText("Modern"));
            QCOMPARE(path->text(), QStringLiteral("/share/modern/test"));
            auto *targetBrowse = dlg->findChild<QToolButton *>("seedBrowseFolder");
            auto *targetFile = dlg->findChild<QToolButton *>("seedBrowseFile");
            QVERIFY(targetBrowse && targetFile && targetBrowse->menu() && targetFile->menu());
            QCOMPARE(targetBrowse->popupMode(), QToolButton::MenuButtonPopup);
            path->setText("/browse");
            {
                bool correctTarget = false;
                QTimer pick;
                connect(&pick, &QTimer::timeout, &pick, [&] {
                    auto *picker = qobject_cast<RemoteFileDialog *>(QApplication::activeModalWidget());
                    if (!picker) return;
                    correctTarget = picker->windowTitle().contains("Modern");
                    auto *box = picker->findChild<QDialogButtonBox *>();
                    auto *tree = picker->findChild<QTreeWidget *>("remoteBrowserEntries");
                    if (box && tree && tree->topLevelItemCount() && box->button(QDialogButtonBox::Ok)->isEnabled())
                    {
                        tree->setCurrentItem(tree->topLevelItem(0));
                        box->button(QDialogButtonBox::Ok)->click();
                    }
                });
                QTimer deadline;
                deadline.setSingleShot(true);
                connect(&deadline, &QTimer::timeout, &deadline, [] {
                    if (auto *picker = qobject_cast<RemoteFileDialog *>(QApplication::activeModalWidget())) picker->reject();
                    if (auto *popup = QApplication::activePopupWidget()) popup->close();
                });
                pick.start(10);
                deadline.start(4000);
                targetBrowse->click();
                QVERIFY(correctTarget);
                QCOMPARE(path->text(), QStringLiteral("/browse/Space & 100%"));
            }
            path->setText("/share/modern/test");
            QTimer driver;
            connect(&driver, &QTimer::timeout, dlg, [] {
                if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                    if (box->windowTitle() == "Seed on another server")
                        box->button(QMessageBox::Yes)->click();
            });
            driver.start(20);
            create->click();
            driver.stop();
            auto *status = dlg->findChild<QLabel *>("creatorTaskStatus");
            QTRY_COMPARE_WITH_TIMEOUT(status->text(), QStringLiteral("Torrent saved locally. Immediate seeding was requested on Modern."), 10000);
            QVERIFY(create->isEnabled());
            create->click();
            crossSeeded = true;
        });
        action->trigger();
        QVERIFY(crossSeeded);
        QCOMPARE(legacy.commandsFor("/api/v2/torrentcreator/addTask"), 2);
        QCOMPARE(legacy.commandsFor("/api/v2/torrentcreator/deleteTask"), 2);
        QCOMPARE(modern.commandsFor("/api/v2/torrentcreator/addTask"), 2);
        QCOMPARE(legacy.commandsFor("/api/v2/torrents/add"), 0);
        QCOMPARE(modern.commandsFor("/api/v2/torrents/add"), 1);
        const QUrlQuery crossRequest(QString::fromUtf8(legacy.lastCommandBody("/api/v2/torrentcreator/addTask")));
        QCOMPARE(crossRequest.queryItemValue("startSeeding"), QStringLiteral("false"));
        const auto multipart = modern.lastCommandBody("/api/v2/torrents/add");
        QVERIFY(multipart.contains("name=\"savepath\"\r\n\r\n/share/modern\r\n"));
        QVERIFY(multipart.contains("name=\"seedMode\"\r\n\r\ntrue\r\n"));
        QVERIFY(multipart.contains("name=\"useDownloadPath\"\r\n\r\nfalse\r\n"));
        QVERIFY(multipart.contains(legacy.exportedTorrent()));
        QFile saved(crossFile);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(saved.readAll(), legacy.exportedTorrent());
    }

    void remoteBrowserNavigatesAuthenticatedServerPaths()
    {
        DemoQbittorrentServer legacy(true), modern(true);
        modern.enableModernResponses();
        QVERIFY(legacy.start() && modern.start());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath("browser.ini"), QSettings::IniFormat);
        for (auto *server : {&legacy, &modern})
        {
            Remote::RemoteSession session;
            const bool newer = server == &modern;
            const QString id = newer ? "modern" : "legacy";
            Remote::ServerProfile profile{id, id, server->url(), "demo", false, false};
            QSignalSpy connected(&session, &Remote::RemoteSession::connected);
            session.connectToServer(profile, "demo-password");
            QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 6000);
            RemoteFileDialog directory(&session, id, id, settings, "/browse",
                RemoteFileDialog::Mode::Directory);
            directory.show();
            auto *tree = directory.findChild<QTreeWidget *>("remoteBrowserEntries");
            auto *buttons = directory.findChild<QDialogButtonBox *>();
            auto *path = directory.findChild<QLineEdit *>("remoteBrowserPath");
            QVERIFY(tree && buttons && path);
            QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(), 1, 6000);
            QCOMPARE(tree->topLevelItem(0)->text(0), QStringLiteral("Space & 100%"));
            tree->setCurrentItem(tree->topLevelItem(0));
            tree->itemDoubleClicked(tree->currentItem(), 0);
            QTRY_COMPARE(path->text(), QStringLiteral("/browse/Space & 100%"));
            QTRY_VERIFY_WITH_TIMEOUT(buttons->button(QDialogButtonBox::Ok)->isEnabled(), 6000);
            if (newer)
                QTRY_VERIFY_WITH_TIMEOUT(!directory.findChild<QLabel *>("remoteBrowserFreeSpace")->text().isEmpty(), 6000);
            buttons->button(QDialogButtonBox::Ok)->click();
            QCOMPARE(directory.result(), int(QDialog::Accepted));
            QCOMPARE(directory.selectedPath(), QStringLiteral("/browse/Space & 100%"));
            RemoteFileDialog files(&session, id, id, settings, {},
                RemoteFileDialog::Mode::File);
            files.show();
            auto *fileTree = files.findChild<QTreeWidget *>("remoteBrowserEntries");
            auto *fileButtons = files.findChild<QDialogButtonBox *>();
            QVERIFY(fileTree && fileButtons);
            QTRY_COMPARE_WITH_TIMEOUT(fileTree->topLevelItemCount(), 1, 6000);
            QCOMPARE(fileTree->topLevelItem(0)->text(0), QStringLiteral("sample file.txt"));
            if (newer) QVERIFY(!fileTree->topLevelItem(0)->text(1).isEmpty());
            else QVERIFY(fileTree->topLevelItem(0)->text(1).isEmpty());
            fileTree->setCurrentItem(fileTree->topLevelItem(0));
            QTRY_VERIFY(fileButtons->button(QDialogButtonBox::Ok)->isEnabled());
            fileButtons->button(QDialogButtonBox::Ok)->click();
            QCOMPARE(files.selectedPath(), QStringLiteral("/browse/Space & 100%/sample file.txt"));
            RemoteFileDialog unavailable(&session, id, id, settings, "/missing", RemoteFileDialog::Mode::Directory);
            unavailable.show();
            auto *missingStatus = unavailable.findChild<QLabel *>("remoteBrowserStatus");
            auto *missingButtons = unavailable.findChild<QDialogButtonBox *>();
            QVERIFY(missingStatus && missingButtons);
            QTRY_VERIFY_WITH_TIMEOUT(missingStatus->text().contains("Cannot reach the server"), 6000);
            QVERIFY(!missingButtons->button(QDialogButtonBox::Ok)->isEnabled());
            unavailable.reject();

            // Exercise the actual Add Torrent split button, including reopening
            // the dialog and keeping the other profile's default independent.
            QVERIFY(Remote::PathMappingStore(settings).replace(id, {{temp.path(), QStringLiteral("/browse")}}));
            const bool native = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
            QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
            const auto restoreNative = qScopeGuard([native] {
                QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, native);
            });
            const auto checkBrowse = [&](QToolButton *button, QAction *action, bool expectRemote) {
                bool sawRemote = false, sawMounted = false;
                QTimer driver;
                driver.setSingleShot(true);
                connect(&driver, &QTimer::timeout, &driver, [&] {
                    auto *modal = QApplication::activeModalWidget();
                    sawRemote = qobject_cast<RemoteFileDialog *>(modal) != nullptr;
                    sawMounted = qobject_cast<QFileDialog *>(modal) != nullptr;
                    if (auto *dlg = qobject_cast<QDialog *>(modal)) dlg->reject();
                    if (auto *popup = QApplication::activePopupWidget()) popup->close();
                });
                driver.start(0);
                if (action) action->trigger();
                else button->click();
                driver.stop();
                QCOMPARE(sawRemote, expectRemote);
                QCOMPARE(sawMounted, !expectRemote);
            };
            const auto newAddDialog = [&] {
                return std::make_unique<RemoteAddTorrentDialog>(settings, profile,
                    RemoteAddTorrentDialog::Source::Links, QStringLiteral("magnet:?xt=urn:btih:0123456789012345678901234567890123456789"),
                    QString(), QByteArray(), nullptr, QList<Remote::ServerProfile>{profile},
                    [](const QString &) { return true; }, QList<Remote::TorrentUpload>{},
                    [&](const QString &requested) { return requested == id ? &session : nullptr; });
            };
            {
                auto add = newAddDialog();
                auto *pathEdit = add->findChild<RemoteFileSystemPathComboEdit *>("savePath");
                QVERIFY(pathEdit);
                auto *button = pathEdit->findChild<QToolButton *>("browseFolderButton");
                QVERIFY(button && button->menu());
                QCOMPARE(button->popupMode(), QToolButton::MenuButtonPopup);
                checkBrowse(button, nullptr, true);
                if (!newer)
                {
                    auto *mounted = button->menu()->findChild<QAction *>("browseMountedFilesystem");
                    QVERIFY(mounted && mounted->isEnabled());
                    checkBrowse(button, mounted, false);
                    checkBrowse(button, nullptr, false);
                }
            }
            settings.sync();
            {
                auto add = newAddDialog();
                auto *pathEdit = add->findChild<RemoteFileSystemPathComboEdit *>("savePath");
                QVERIFY(pathEdit);
                auto *button = pathEdit->findChild<QToolButton *>("browseFolderButton");
                QVERIFY(button && button->menu());
                checkBrowse(button, nullptr, newer);
                if (!newer)
                {
                    auto *remote = button->menu()->findChild<QAction *>("browseRemoteFilesystem");
                    QVERIFY(remote && remote->isEnabled());
                    checkBrowse(button, remote, true);
                    checkBrowse(button, nullptr, true);
                    // Restore mounted preference so the modern iteration proves isolation.
                    auto *mounted = button->menu()->findChild<QAction *>("browseMountedFilesystem");
                    QVERIFY(mounted);
                    checkBrowse(button, mounted, false);
                }
            }

            // Mapping creation must browse the explicitly selected profile,
            // never inherit the Add Torrent local/remote preference.
            QVERIFY(Remote::ProfileStore(settings).save(profile));
            Remote::ServerProfile offline{"offline", "Offline", "http://127.0.0.1:65534", "demo", false, false};
            QVERIFY(Remote::ProfileStore(settings).save(offline));
            RemotePreferencesDialog prefs(settings, id, nullptr, &session, nullptr, {},
                [&](const QString &requested) -> Remote::RemoteSession * {
                    return requested == id ? &session : nullptr;
                });
            prefs.show();
            auto *profiles = prefs.findChild<QComboBox *>("remotePathsServer");
            auto *addMapping = prefs.findChild<QPushButton *>("addPathMapping");
            auto *sections = prefs.findChild<QListWidget *>("preferencesSections");
            QVERIFY(profiles && addMapping && sections);
            sections->setCurrentRow(0);
            profiles->setCurrentIndex(profiles->findData(id));
            bool verified = false;
            QTimer::singleShot(0, &prefs, [&] {
                auto *editor = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                if (!editor || editor->objectName() != "mappingEditor") return;
                const auto cleanup = qScopeGuard([editor] { if (editor->isVisible()) editor->reject(); });
                auto *local = editor->findChild<QLineEdit *>("mappingLocalPath");
                auto *remote = editor->findChild<QLineEdit *>("mappingRemotePath");
                auto *localButton = editor->findChild<QPushButton *>("mappingBrowseLocal");
                auto *remoteButton = editor->findChild<QPushButton *>("mappingBrowseRemote");
                QVERIFY(local && remote && localButton && remoteButton);
                QVERIFY(localButton->isEnabled() && remoteButton->isEnabled() && !remoteButton->menu());
                local->setText(temp.path());
                remote->setText("/browse");
                QTimer drive;
                connect(&drive, &QTimer::timeout, &drive, [] {
                    auto *picker = qobject_cast<RemoteFileDialog *>(QApplication::activeModalWidget());
                    if (!picker) return;
                    auto *box = picker->findChild<QDialogButtonBox *>();
                    if (box && box->button(QDialogButtonBox::Ok)->isEnabled())
                    {
                        auto *tree = picker->findChild<QTreeWidget *>("remoteBrowserEntries");
                        if (tree && tree->topLevelItemCount()) tree->setCurrentItem(tree->topLevelItem(0));
                        box->button(QDialogButtonBox::Ok)->click();
                    }
                });
                QTimer timeout;
                timeout.setSingleShot(true);
                connect(&timeout, &QTimer::timeout, &timeout, [] {
                    if (auto *picker = qobject_cast<QDialog *>(QApplication::activeModalWidget())) picker->reject();
                });
                drive.start(10);
                timeout.start(4000);
                remoteButton->click();
                QCOMPARE(remote->text(), QStringLiteral("/browse/Space & 100%"));
                QCOMPARE(local->text(), temp.path());
                verified = true;
            });
            addMapping->click();
            QVERIFY(verified);
            profiles->setCurrentIndex(profiles->findData(offline.id));
            bool offlineVerified = false;
            QTimer::singleShot(0, &prefs, [&] {
                auto *editor = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                if (!editor || editor->objectName() != "mappingEditor") return;
                const auto cleanup = qScopeGuard([editor] { editor->reject(); });
                auto *remote = editor->findChild<QLineEdit *>("mappingRemotePath");
                auto *remoteButton = editor->findChild<QPushButton *>("mappingBrowseRemote");
                QVERIFY(remote && remoteButton && !remoteButton->isEnabled() && remote->isEnabled());
                offlineVerified = true;
            });
            addMapping->click();
            QVERIFY(offlineVerified);
            prefs.reject();
            QCOMPARE(server->commandsFor("/api/v2/torrents/add"), 0);
        }
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

    void upstreamTorrentStateColorsSurviveStalledAndQueuedStates()
    {
        // Regression: stalledUP/stalledDL previously fell back to the plain
        // desktop Text color, rendering nearly all idle seedboxes black.
        // Exercise the real model with actual qBittorrent Web API state names.
        struct Case
        {
            const char *state;
            const char *lightColor;
            const char *darkColor;
        };
        const Case cases[] = {
            {"downloading", "#1a7f37", "#3fb950"},
            {"forcedDL", "#1a7f37", "#3fb950"},
            {"metaDL", "#1a7f37", "#3fb950"},
            {"forcedMetaDL", "#1a7f37", "#3fb950"},
            {"stalledDL", "#2da44e", "#238636"},
            {"uploading", "#0969da", "#58a6ff"},
            {"forcedUP", "#0969da", "#58a6ff"},
            {"stalledUP", "#0969da", "#1f6feb"},
            {"queuedDL", "#7d4e00", "#845306"},
            {"queuedUP", "#7d4e00", "#845306"},
            {"checkingDL", "#1a7f37", "#3fb950"},
            {"checkingUP", "#1a7f37", "#3fb950"},
            {"checkingResumeData", "#1a7f37", "#3fb950"},
            {"moving", "#1a7f37", "#3fb950"},
            {"stoppedDL", "#57606a", "#8b949e"},
            {"pausedDL", "#57606a", "#8b949e"},
            {"stoppedUP", "#8250df", "#a371f7"},
            {"pausedUP", "#8250df", "#a371f7"},
            {"error", "#cf222e", "#f85149"},
            {"missingFiles", "#cf222e", "#f85149"}
        };
        const bool dark = QApplication::palette()
            .color(QPalette::Base).lightness() < 128;
        QJsonObject torrents;
        for (int i = 0; i < int(std::size(cases)); ++i)
        {
            const QString hash = QString::number(i + 1).rightJustified(40, QLatin1Char('0'));
            torrents.insert(hash, QJsonObject{
                {QStringLiteral("name"), QString::fromLatin1(cases[i].state)},
                {QStringLiteral("state"), QString::fromLatin1(cases[i].state)},
                {QStringLiteral("progress"), 0.5}
            });
        }
        TorrentModel model;
        model.applySync(QJsonObject{
            {QStringLiteral("rid"), 1},
            {QStringLiteral("full_update"), true},
            {QStringLiteral("torrents"), torrents}
        });
        QCOMPARE(model.rowCount(), int(std::size(cases)));
        model.setUseStateColors(true);
        for (int i = 0; i < int(std::size(cases)); ++i)
        {
            const QModelIndex row = model.index(i, 0);
            const QColor expected(QString::fromLatin1(
                dark ? cases[i].darkColor : cases[i].lightColor));
            const QColor actual = row.data(Qt::ForegroundRole).value<QColor>();
            QVERIFY2(actual.isValid(), cases[i].state);
            QCOMPARE(actual, expected);
            QCOMPARE(model.index(i, 2).data(Qt::ForegroundRole).value<QColor>(), expected);
        }

        // When the setting is disabled, the native text palette wins.
        model.setUseStateColors(false);
        QVERIFY(!model.index(7, 0).data(Qt::ForegroundRole).isValid());
        QVERIFY(model.index(7, 2).data(TorrentModel::StateColorRole).value<QColor>().isValid());

        // A state transition must invalidate the ForegroundRole as well as
        // text/decoration, so the visible row and progress repaint together.
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        const QString hash = QString::number(8).rightJustified(40, QLatin1Char('0'));
        model.setUseStateColors(true);
        changed.clear();
        model.applySync(QJsonObject{
            {QStringLiteral("rid"), 2},
            {QStringLiteral("torrents"), QJsonObject{
                {hash, QJsonObject{{QStringLiteral("state"), QStringLiteral("stoppedUP")}}}
            }}
        });
        QVERIFY(!changed.isEmpty());
        const QList<int> roles = changed.constLast().at(2).value<QList<int>>();
        QVERIFY(roles.contains(Qt::ForegroundRole));
        QVERIFY(roles.contains(TorrentModel::StateColorRole));
        QCOMPARE(model.index(7, 0).data(Qt::ForegroundRole).value<QColor>(),
                 QColor(dark ? QStringLiteral("#a371f7") : QStringLiteral("#8250df")));

        // Both server-reported announce error classes override the normal
        // palette, but warnings alone and absent flags do not. Exercise the
        // actual painter, text preference, and incremental recovery.
        const QColor normal(dark ? "#a371f7" : "#8250df");
        const QColor red(dark ? "#f85149" : "#cf222e");
        const auto patch = [&](const QJsonObject &flags) {
            model.applySync(QJsonObject{{"rid", 3}, {"torrents", QJsonObject{{hash, flags}}}});
        };
        const auto progress = model.index(7, 2);
        const auto name = model.index(7, 0);
        TransferListDelegate delegate;
        const auto paint = [&] {
            QImage image(200, 24, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QStyleOptionViewItem option;
            option.rect = QRect(0, 0, 200, 24);
            option.palette = QApplication::palette();
            option.state = QStyle::State_Enabled | QStyle::State_Active;
            QPainter painter(&image);
            delegate.paint(&painter, option, progress);
            painter.end();
            return image;
        };
        const auto nativeBar = paint();
        delegate.setProgressFollowsStateColor(true);
        const auto normalBar = paint();
        QCOMPARE(progress.data(TorrentModel::StateColorRole).value<QColor>(), normal);
        patch(QJsonObject{{"has_tracker_warning", true}});
        QCOMPARE(name.data(Qt::ForegroundRole).value<QColor>(), normal);
        QCOMPARE(paint(), normalBar);
        patch(QJsonObject{{"has_tracker_error", true}});
        QCOMPARE(name.data(Qt::ForegroundRole).value<QColor>(), red);
        QCOMPARE(progress.data(TorrentModel::StateColorRole).value<QColor>(), red);
        const auto errorBar = paint();
        QVERIFY(errorBar != normalBar);
        model.setUseStateColors(false);
        QVERIFY(!name.data(Qt::ForegroundRole).isValid());
        QCOMPARE(paint(), errorBar);
        delegate.setProgressFollowsStateColor(false);
        QCOMPARE(paint(), nativeBar);
        model.setUseStateColors(true);
        QCOMPARE(name.data(Qt::ForegroundRole).value<QColor>(), red);
        delegate.setProgressFollowsStateColor(true);
        patch(QJsonObject{{"has_tracker_error", false}, {"has_other_announce_error", true}});
        QCOMPARE(name.data(Qt::ForegroundRole).value<QColor>(), red);
        QCOMPARE(paint(), errorBar);
        patch(QJsonObject{{"has_other_announce_error", false}});
        QCOMPARE(name.data(Qt::ForegroundRole).value<QColor>(), normal);
        QCOMPARE(paint(), normalBar);
        QCOMPARE(name.siblingAtColumn(3).data(Qt::UserRole).toString(), QStringLiteral("stoppedUP"));
    }

    void externalFileOpenReusesExistingWindowAndWaitsForAuthentication()
    {
        DemoQbittorrentServer server(true);
        QVERIFY(server.start());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath("external-open.ini"), QSettings::IniFormat);
        Remote::ProfileStore store(settings);
        Remote::ServerProfile profile{QString(), "External open server", server.url(), "demo", true, true};
        QVERIFY(store.save(profile));
        DemoCredentialVault vault;
        vault.data.insert(vault.keyFor(profile), "demo-password");

        const auto createTorrent = [&](const QString &path, const QByteArray &name) {
            const QByteArray metadata = QByteArrayLiteral("d4:infod6:lengthi100e4:name")
                + QByteArray::number(name.size()) + ':' + name + QByteArrayLiteral("ee");
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly) || file.write(metadata) != metadata.size()) return QByteArray();
            return metadata;
        };
        const QString first = temp.filePath("cold launch.torrent");
        const QString second = temp.filePath("second & percent %20.torrent");
        const auto firstBytes = createTorrent(first, "first");
        const auto secondBytes = createTorrent(second, "second");
        QVERIFY(!firstBytes.isEmpty() && !secondBytes.isEmpty());

        MainWindow window(nullptr, &settings, &vault);
        DroppedTorrentUi driver;
        qApp->installEventFilter(&driver);
        const auto cleanup = qScopeGuard([&] { qApp->removeEventFilter(&driver); });
        const QString instanceDirectory = temp.filePath("ipc");
        SingleInstance primary(instanceDirectory);
        QString error;
        QCOMPARE(primary.start({}, [&window](const QStringList &args) { window.enqueueExternalOpen(args); }, error),
                 SingleInstance::Result::Primary);

        // Simulate an initial file-manager launch before auto-connect has run.
        QStringList initial;
        QVERIFY(SingleInstance::normalizeArguments({first}, initial, error));
        window.enqueueExternalOpen(initial);
        QTRY_COMPARE_WITH_TIMEOUT(server.commandsFor("/api/v2/torrents/add"), 1, 10000);
        QCOMPARE(driver.opened, 1);
        QVERIFY(server.lastCommandBody("/api/v2/torrents/add").contains(firstBytes));

        // A separate OS process sends a file URI, exits after acknowledgement,
        // and the original authenticated window handles the actual addition.
        window.hide();
        QProcess secondary;
        secondary.start(QCoreApplication::applicationFilePath(),
            {"forward-launch", instanceDirectory, QUrl::fromLocalFile(second).toString(QUrl::FullyEncoded)});
        QTRY_COMPARE_WITH_TIMEOUT(secondary.state(), QProcess::NotRunning, 12000);
        QCOMPARE(secondary.exitStatus(), QProcess::NormalExit);
        QCOMPARE(secondary.exitCode(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(server.commandsFor("/api/v2/torrents/add"), 2, 10000);
        QCOMPARE(driver.opened, 2);
        QVERIFY(window.isVisible());
        QVERIFY(server.lastCommandBody("/api/v2/torrents/add").contains(secondBytes));

        const QString magnet = "magnet:?xt=urn:btih:fedcba9876543210fedcba9876543210fedcba98";
        QProcess link;
        link.start(QCoreApplication::applicationFilePath(), {"forward-launch", instanceDirectory, magnet});
        QTRY_COMPARE_WITH_TIMEOUT(link.state(), QProcess::NotRunning, 12000);
        QCOMPARE(link.exitCode(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(server.commandsFor("/api/v2/torrents/add"), 3, 10000);
        QCOMPARE(driver.opened, 3);
        QCOMPARE(driver.links, 1);
        QCOMPARE(QUrlQuery(QString::fromUtf8(server.lastCommandBody("/api/v2/torrents/add")))
            .queryItemValue("urls", QUrl::FullyDecoded), magnet);
        QVERIFY2(driver.unexpected.isEmpty(), qPrintable(driver.unexpected));
    }

    void userDropsTorrentFilesAndMagnetIntoNativeTransferView()
    {
        DemoQbittorrentServer server(true);
        QVERIFY(server.start());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("drag.ini")), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        QTRY_VERIFY(window.isVisible());
        openConnectDialog(window, server.url());
        auto *table = window.findChild<QTableView *>(QStringLiteral("torrentList"));
        QVERIFY(table);
        auto *add = window.findChild<QAction *>(QStringLiteral("actionOpen"));
        QVERIFY(add);
        QTRY_VERIFY_WITH_TIMEOUT(add->isEnabled(), 6000);

        QList<QByteArray> payloads;
        QList<QUrl> urls;
        for (int i = 1; i <= 2; ++i)
        {
            const QByteArray torrentName = QByteArrayLiteral("drag") + QByteArray::number(i);
            const QByteArray metadata = QByteArrayLiteral("d4:infod6:lengthi")
                + QByteArray::number(100 + i) + QByteArrayLiteral("e4:name")
                + QByteArray::number(torrentName.size()) + ':'
                + torrentName + QByteArrayLiteral("ee");
            Remote::TorrentMetadata decoded;
            QVERIFY(Remote::TorrentMetadataReader::read(metadata, decoded));
            const QString path = temp.filePath(QStringLiteral("dropped_%1.torrent").arg(i));
            QFile output(path);
            QVERIFY(output.open(QIODevice::WriteOnly));
            QCOMPARE(output.write(metadata), qint64(metadata.size()));
            output.close();
            payloads.append(metadata);
            urls.append(QUrl::fromLocalFile(path));
        }

        QMimeData localMime;
        localMime.setUrls(urls);
        auto *viewport = table->viewport();
        QDragEnterEvent enter(QPoint(10, 10), Qt::CopyAction, &localMime,
                              Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewport, &enter);
        QVERIFY(enter.isAccepted());

        DroppedTorrentUi driver;
        QApplication::instance()->installEventFilter(&driver);
        QTimer watchdog;
        watchdog.setSingleShot(true);
        QObject::connect(&watchdog, &QTimer::timeout, &window, [&]() {
            driver.unexpected = QStringLiteral("Drop confirmation timed out");
            for (QWidget *top : QApplication::topLevelWidgets())
                if (auto *dialog = qobject_cast<QDialog *>(top); dialog && dialog->isVisible())
                    dialog->reject();
        });
        watchdog.start(10000);
        QDropEvent drop(QPointF(10, 10), Qt::CopyAction, &localMime,
                        Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewport, &drop);
        watchdog.stop();
        QApplication::instance()->removeEventFilter(&driver);
        QVERIFY(drop.isAccepted());
        QVERIFY2(driver.unexpected.isEmpty(), qPrintable(driver.unexpected));
        QCOMPARE(driver.opened, 1);
        QCOMPARE(driver.batches, 1);
        QTRY_COMPARE_WITH_TIMEOUT(server.commandsFor("/api/v2/torrents/add"), 1, 6000);
        QByteArray body = server.lastCommandBody("/api/v2/torrents/add");
        QCOMPARE(body.count(QByteArrayLiteral("name=\"torrents\"; filename=\"")), 2);
        for (const QByteArray &metadata : payloads)
            QVERIFY(body.contains(metadata));

        const QString magnet = QStringLiteral(
            "magnet:?xt=urn:btih:fedcba9876543210fedcba9876543210fedcba98");
        QMimeData magnetMime;
        magnetMime.setText(magnet);
        QDragEnterEvent magnetEnter(QPoint(10, 10), Qt::CopyAction, &magnetMime,
                                    Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewport, &magnetEnter);
        QVERIFY(magnetEnter.isAccepted());
        DroppedTorrentUi linkDriver;
        QApplication::instance()->installEventFilter(&linkDriver);
        watchdog.start(10000);
        QDropEvent magnetDrop(QPointF(10, 10), Qt::CopyAction, &magnetMime,
                              Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewport, &magnetDrop);
        watchdog.stop();
        QApplication::instance()->removeEventFilter(&linkDriver);
        QVERIFY(magnetDrop.isAccepted());
        QVERIFY2(linkDriver.unexpected.isEmpty(), qPrintable(linkDriver.unexpected));
        QCOMPARE(linkDriver.opened, 1);
        QCOMPARE(linkDriver.links, 1);
        QTRY_COMPARE_WITH_TIMEOUT(server.commandsFor("/api/v2/torrents/add"), 2, 6000);
        QCOMPARE(QUrlQuery(QString::fromUtf8(server.lastCommandBody(
            "/api/v2/torrents/add"))).queryItemValue(QStringLiteral("urls"), QUrl::FullyDecoded), magnet);

        QMimeData irrelevant;
        irrelevant.setUrls({QUrl::fromLocalFile(temp.filePath(QStringLiteral("notes.txt")))});
        QDragEnterEvent rejected(QPoint(10, 10), Qt::CopyAction, &irrelevant,
                                 Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewport, &rejected);
        QVERIFY(!rejected.isAccepted());
        QCOMPARE(server.commandsFor("/api/v2/torrents/add"), 2);
    }

    void contentDragPreferenceIsEnabledAndControlsNativeDragMode()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("drag-preferences.ini")),
                           QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        QTRY_VERIFY(window.isVisible());

        auto *content = window.findChild<MappedContentTreeWidget *>(
            QStringLiteral("torrentContent"));
        auto *preferences = window.findChild<QAction *>(QStringLiteral("actionOptions"));
        QVERIFY(content && preferences);
        // Preserve the existing working drag-out behavior on upgrade.
        QVERIFY(content->dragEnabled());

        QString error;
        const auto changePreference = [&](bool enabled) {
            QTimer::singleShot(0, &window, [&]() {
                auto *dialog = qobject_cast<RemotePreferencesDialog *>(
                    QApplication::activeModalWidget());
                if (!dialog)
                {
                    error = QStringLiteral("Preferences did not open");
                    return;
                }
                auto *check = dialog->findChild<QCheckBox *>(
                    QStringLiteral("checkTorrentContentDrag"));
                auto *buttons = dialog->findChild<QDialogButtonBox *>(
                    QStringLiteral("buttonBox"));
                if (!check || !check->isEnabled() || !buttons
                    || !buttons->button(QDialogButtonBox::Ok))
                {
                    error = QStringLiteral("Native Content drag option remains disabled");
                    dialog->reject();
                    return;
                }
                check->setChecked(enabled);
                buttons->button(QDialogButtonBox::Ok)->click();
            });
            preferences->trigger();
        };

        changePreference(false);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(settings.value(QStringLiteral(
            "Preferences/Behavior/TorrentContentDragEnabled")).toBool(), false);
        QVERIFY(!content->dragEnabled());

        // Upstream's Alt inversion: unchecked prefers selection, Alt drags.
        auto *viewport = content->viewport();
        QTest::mousePress(viewport, Qt::LeftButton, Qt::AltModifier, QPoint(3, 3));
        QVERIFY(content->dragEnabled());
        QTest::mouseRelease(viewport, Qt::LeftButton, Qt::AltModifier, QPoint(3, 3));
        QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, QPoint(3, 3));
        QVERIFY(!content->dragEnabled());
        QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, QPoint(3, 3));

        changePreference(true);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(settings.value(QStringLiteral(
            "Preferences/Behavior/TorrentContentDragEnabled")).toBool(), true);
        QVERIFY(content->dragEnabled());

        // Checked prefers dragging, Alt gives multi-selection again.
        QTest::mousePress(viewport, Qt::LeftButton, Qt::AltModifier, QPoint(3, 3));
        QVERIFY(!content->dragEnabled());
        QTest::mouseRelease(viewport, Qt::LeftButton, Qt::AltModifier, QPoint(3, 3));
        QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, QPoint(3, 3));
        QVERIFY(content->dragEnabled());
        QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, QPoint(3, 3));

        // Reopening the dialog must reflect the saved choice.
        RemotePreferencesDialog reopened(settings);
        auto *check = reopened.findChild<QCheckBox *>(
            QStringLiteral("checkTorrentContentDrag"));
        QVERIFY(check && check->isEnabled() && check->isChecked());
    }

    void userDragsOnlyExistingMappedContentAsCopyUris()
    {
        DemoQbittorrentServer server(true);
        server.enableTorrentSyncDemo();
        server.enableMappedFolderDemo();
        server.enableDetailsDemo();
        server.keepDemoTorrents();
        QVERIFY(server.start());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString mount = temp.filePath(QStringLiteral("NAS"));
        const QString data = temp.filePath(QStringLiteral("NAS/Music/data"));
        QVERIFY(QDir().mkpath(data));
        QStringList local;
        for (int i = 1; i <= 2; ++i)
        {
            const QString name = QStringLiteral("file-%1.bin").arg(i == 1
                ? QStringLiteral("one") : QStringLiteral("two"));
            const QString path = QDir(data).filePath(name);
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("test", 4), qint64(4));
            file.close();
            local.append(path);
        }

        QSettings settings(temp.filePath(QStringLiteral("paths.ini")), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        QTRY_VERIFY(window.isVisible());
        openConnectDialog(window, server.url());
        auto *table = window.findChild<QTableView *>(QStringLiteral("torrentList"));
        QVERIFY(table);
        QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 1, 6000);
        const auto profiles = Remote::ProfileStore(settings).profiles();
        QCOMPARE(profiles.size(), 1);
        Remote::PathMappingStore mappings(settings);
        QVERIFY(mappings.replace(profiles.first().id, {{mount, QStringLiteral("/share/NAS")}}));
        // Opening/closing Preferences reloads mappings for every live server.
        QTimer::singleShot(0, &window, []() {
            if (auto *modal = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
                modal->reject();
        });
        auto *preferences = window.findChild<QAction *>(QStringLiteral("actionOptions"));
        QVERIFY(preferences);
        preferences->trigger();

        table->selectRow(0);
        auto *tabs = window.findChild<PropTabBar *>();
        QVERIFY(tabs);
        tabs->setCurrentIndex(4);
        auto *content = window.findChild<MappedContentTreeWidget *>(
            QStringLiteral("torrentContent"));
        QVERIFY(content);
        QTRY_VERIFY_WITH_TIMEOUT(content->topLevelItemCount() > 0, 6000);
        QTRY_COMPARE_WITH_TIMEOUT(content->topLevelItem(0)->childCount(), 2, 6000);

        auto *first = content->topLevelItem(0)->child(0);
        auto *second = content->topLevelItem(0)->child(1);
        first->setSelected(true);
        {
            std::unique_ptr<QMimeData> mime(content->mappedSelectionMimeData());
            QVERIFY(mime);
            QCOMPARE(mime->urls(), (QList<QUrl>{
                QUrl::fromLocalFile(QFileInfo(local.at(0)).canonicalFilePath())}));
        }
        // Drive the actual native Content context menu and capture the
        // desktop file-open request without starting external programs.
        struct RestoreContentOpen {
            ~RestoreContentOpen() { QDesktopServices::unsetUrlHandler(QStringLiteral("file")); }
        } restoreHandler;
        m_fileOpenRequests.clear();
        QDesktopServices::setUrlHandler(QStringLiteral("file"), this, "recordFileOpenRequest");
        const auto menuAction = [&](const QString &name, bool enabled, bool trigger = true) {
            bool found = false, actualEnabled = false;
            QTimer::singleShot(0, &window, [&]() {
                auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
                if (!menu) return;
                if (menu->objectName() == QLatin1String("torrentContentContextMenu"))
                {
                    for (auto *action : menu->actions())
                    {
                        if (action->objectName() != name) continue;
                        found = true;
                        actualEnabled = action->isEnabled();
                        if (actualEnabled && trigger) action->trigger();
                        break;
                    }
                }
                menu->close();
            });
            QVERIFY(QMetaObject::invokeMethod(content, "customContextMenuRequested",
                Qt::DirectConnection, Q_ARG(QPoint, QPoint(10, 10))));
            QVERIFY(found);
            QCOMPARE(actualEnabled, enabled);
        };
        const QString firstPath = QFileInfo(local.at(0)).canonicalFilePath();
        const QString parentPath = QFileInfo(data).canonicalFilePath();
        menuAction(QStringLiteral("contentOpenMappedItem"), true);
        QCOMPARE(m_fileOpenRequests.size(), 1);
        QCOMPARE(m_fileOpenRequests.last(), QUrl::fromLocalFile(firstPath));
        menuAction(QStringLiteral("contentOpenMappedParent"), true);
        QCOMPARE(m_fileOpenRequests.size(), 2);
        QCOMPARE(m_fileOpenRequests.last(), QUrl::fromLocalFile(parentPath));
        menuAction(QStringLiteral("contentCopyMappedPath"), true);
        QCOMPARE(QApplication::clipboard()->text(), QDir::toNativeSeparators(firstPath));
        content->clearSelection();
        content->topLevelItem(0)->setSelected(true);
        menuAction(QStringLiteral("contentOpenMappedItem"), true);
        QCOMPARE(m_fileOpenRequests.size(), 3);
        QCOMPARE(m_fileOpenRequests.last(), QUrl::fromLocalFile(parentPath));
        content->clearSelection();
        first->setSelected(true);
        second->setSelected(true);
        menuAction(QStringLiteral("contentOpenMappedItem"), false);
        {
            std::unique_ptr<QMimeData> mime(content->mappedSelectionMimeData());
            QVERIFY(mime);
            QCOMPARE(mime->urls().size(), 2);
        }
        // Native Properties defaults: double-click and Enter open a single
        // mapped item. The local preferences can switch double-click to
        // remote rename and disable opening with either Enter key.
        const auto setInteraction = [&](const QString &mode, bool enter) {
            bool found = false;
            QTimer::singleShot(0, &window, [&]() {
                auto *dialog = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget());
                if (!dialog) return;
                auto *combo = dialog->findChild<QComboBox *>("comboContentDoubleClick");
                auto *check = dialog->findChild<QCheckBox *>("checkContentOpenByEnter");
                auto *buttons = dialog->findChild<QDialogButtonBox *>("buttonBox");
                if (!combo || !check || !buttons || !combo->isEnabled() || !check->isEnabled())
                { dialog->reject(); return; }
                found = true;
                combo->setCurrentIndex(combo->findData(mode));
                check->setChecked(enter);
                buttons->button(QDialogButtonBox::Ok)->click();
            });
            preferences->trigger();
            QVERIFY(found);
            QCOMPARE(settings.value("Preferences/Behavior/ContentDoubleClick").toString(), mode);
            QCOMPARE(settings.value("Preferences/Behavior/ContentOpenByEnter").toBool(), enter);
        };
        content->clearSelection();
        content->expandItem(content->topLevelItem(0));
        content->setCurrentItem(first);
        content->setFocus();
        content->scrollToItem(first);
        QTRY_VERIFY(content->visualItemRect(first).isValid());
        const auto doubleClick = [&]() {
            const QRect rect = content->visualItemRect(first);
            // Use the filename cell (not its checkbox or disclosure arrow).
            const QPoint pos(qMin(rect.right() - 2, rect.left() + 80), rect.center().y());
            QTest::mouseClick(content->viewport(), Qt::LeftButton, Qt::NoModifier, pos);
            QTest::mouseDClick(content->viewport(), Qt::LeftButton, Qt::NoModifier, pos);
        };
        doubleClick();
        QTRY_COMPARE(m_fileOpenRequests.size(), 4);
        QCOMPARE(m_fileOpenRequests.last(), QUrl::fromLocalFile(firstPath));
        QTest::keyClick(content, Qt::Key_Return);
        QTRY_COMPARE(m_fileOpenRequests.size(), 5);
        QTest::keyClick(content, Qt::Key_Enter);
        QTRY_COMPARE(m_fileOpenRequests.size(), 6);
        setInteraction(QStringLiteral("rename"), false);
        content->setCurrentItem(first);
        content->setFocus();
        QTest::keyClick(content, Qt::Key_Return);
        QTest::keyClick(content, Qt::Key_Enter);
        QCOMPARE(m_fileOpenRequests.size(), 6);
        bool renameOpened = false;
        QTimer renameDriver;
        connect(&renameDriver, &QTimer::timeout, &window, [&]() {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (dialog && dialog->objectName() == QLatin1String("contentRenameDialog"))
            {
                renameOpened = true;
                dialog->reject();
            }
        });
        renameDriver.start(10);
        doubleClick();
        QTRY_VERIFY_WITH_TIMEOUT(renameOpened, 3000);
        renameDriver.stop();
        QCOMPARE(server.commandsFor("/api/v2/torrents/renameFile"), 0);
        QCOMPARE(m_fileOpenRequests.size(), 6);
        setInteraction(QStringLiteral("open"), true);
        content->clearSelection();
        first->setSelected(true);
        second->setSelected(true);
        QTest::keyClick(content, Qt::Key_Return);
        QCOMPARE(m_fileOpenRequests.size(), 6);
        // No partial drag if one selected file vanished from the mount.
        QVERIFY(QFile::remove(local.at(1)));
        QVERIFY(content->mappedSelectionMimeData() == nullptr);
        first->setSelected(false);
        menuAction(QStringLiteral("contentOpenMappedItem"), false);
        menuAction(QStringLiteral("contentCopyMappedPath"), false);
        second->setSelected(false);
        first->setSelected(true);
        // An unmapped content selection offers no misleading local URI.
        content->setPathMappings({});
        QVERIFY(content->mappedSelectionMimeData() == nullptr);
        menuAction(QStringLiteral("contentOpenMappedItem"), false);
        menuAction(QStringLiteral("contentOpenMappedParent"), false);
        menuAction(QStringLiteral("contentCopyMappedPath"), false);
        QTest::keyClick(content, Qt::Key_Return);
        QCOMPARE(m_fileOpenRequests.size(), 6);
        QCOMPARE(server.commandsFor("/api/v2/torrents/add"), 0);
        QCOMPARE(server.commandsFor("/api/v2/torrents/setLocation"), 0);
    }

    void incompletePreviewUsesActualMappedStorage()
    {
        DemoQbittorrentServer server(true);
        server.enableDetailsDemo();
        server.enableRemoteContentAndOptionsDemo();
        const QString hash(40, QLatin1Char('a'));
        server.setSidebarSnapshot(QJsonObject{{"torrents", QJsonObject{
            {hash, QJsonObject{{"name", "Preview demo"}, {"size", 4096},
                {"progress", 0.5}, {"state", "downloading"},
                {"save_path", "/share/NAS/Finished"},
                {"download_path", "/share/NAS/Incomplete"},
                {"content_path", "/share/NAS/Incomplete/Album"}}}}},
            {"server_state", QJsonObject{}}});
        server.setContentEntries(hash, QJsonArray{
            QJsonObject{{"index", 0}, {"name", "Album/track.flac"}, {"size", 2048},
                {"progress", 0.5}, {"priority", 1}},
            QJsonObject{{"index", 1}, {"name", "Album/complete.mp4"}, {"size", 1024},
                {"progress", 1.0}, {"priority", 1}},
            QJsonObject{{"index", 2}, {"name", "Album/notes.txt"}, {"size", 1024},
                {"progress", 0.5}, {"priority", 1}}});
        QVERIFY(server.start());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString mount = temp.filePath("NAS");
        const QString unfinished = mount + "/Incomplete/Album";
        const QString finished = mount + "/Finished/Album";
        QVERIFY(QDir().mkpath(unfinished));
        QVERIFY(QDir().mkpath(finished));
        const QString plain = unfinished + "/track.flac";
        const QString partial = plain + ".!qB";
        const auto create = [](const QString &path) {
            QFile file(path);
            return file.open(QIODevice::WriteOnly) && file.write("fixture", 7) == 7;
        };
        QVERIFY(create(partial));
        QVERIFY(create(finished + "/track.flac")); // Never open a stale destination copy.
        QVERIFY(create(unfinished + "/complete.mp4"));
        QVERIFY(create(unfinished + "/notes.txt"));
        QSettings settings(temp.filePath("profiles.ini"), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        openConnectDialog(window, server.url());
        auto *table = window.findChild<QTableView *>("torrentList");
        auto *tabs = window.findChild<PropTabBar *>();
        auto *tree = window.findChild<MappedContentTreeWidget *>("torrentContent");
        auto *preferences = window.findChild<QAction *>("actionOptions");
        QVERIFY(table && tabs && tree && preferences);
        QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 1, 6000);
        const auto profiles = Remote::ProfileStore(settings).profiles();
        QCOMPARE(profiles.size(), 1);
        QVERIFY(Remote::PathMappingStore(settings).replace(profiles.first().id,
            {{mount, QStringLiteral("/share/NAS")}}));
        QTimer::singleShot(0, &window, []() {
            if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
                dialog->reject();
        });
        preferences->trigger();
        table->selectRow(0);
        tabs->setCurrentIndex(4);
        QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(), 1, 6000);
        QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItem(0)->childCount(), 3, 6000);
        auto *album = tree->topLevelItem(0);
        const auto find = [&](const QString &name) {
            for (int i = 0; i < album->childCount(); ++i)
                if (album->child(i)->text(0) == name) return album->child(i);
            return static_cast<QTreeWidgetItem *>(nullptr);
        };
        auto *track = find("track.flac");
        auto *complete = find("complete.mp4");
        auto *notes = find("notes.txt");
        QVERIFY(track && complete && notes);
        struct RestorePreviewOpen {
            ~RestorePreviewOpen() { QDesktopServices::unsetUrlHandler(QStringLiteral("file")); }
        } restoreHandler;
        m_fileOpenRequests.clear();
        QDesktopServices::setUrlHandler(QStringLiteral("file"), this, "recordFileOpenRequest");
        const auto preview = [&](bool enabled, bool trigger = true) {
            bool found = false, actualEnabled = false;
            QTimer::singleShot(0, &window, [&]() {
                auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
                if (!menu) return;
                for (auto *action : menu->actions())
                {
                    if (action->objectName() != QLatin1String("contentPreviewIncomplete")) continue;
                    found = true;
                    actualEnabled = action->isEnabled();
                    if (actualEnabled && trigger) action->trigger();
                    break;
                }
                menu->close();
            });
            QVERIFY(QMetaObject::invokeMethod(tree, "customContextMenuRequested",
                Qt::DirectConnection, Q_ARG(QPoint, QPoint(10, 10))));
            QVERIFY(found);
            QCOMPARE(actualEnabled, enabled);
        };
        tree->setCurrentItem(track);
        preview(true);
        QCOMPARE(m_fileOpenRequests.size(), 1);
        QCOMPARE(m_fileOpenRequests.last(), QUrl::fromLocalFile(QFileInfo(partial).canonicalFilePath()));
        QVERIFY(create(plain));
        preview(false); // Two plausible names, neither can be chosen safely.
        QCOMPARE(m_fileOpenRequests.size(), 1);
        QVERIFY(QFile::remove(partial));
        preview(true);
        QCOMPARE(m_fileOpenRequests.size(), 2);
        QCOMPARE(m_fileOpenRequests.last(), QUrl::fromLocalFile(QFileInfo(plain).canonicalFilePath()));
        QVERIFY(QFile::remove(plain));
        preview(false); // The finished-directory copy is not the actual file.
        QVERIFY(create(partial));
        tree->setCurrentItem(complete);
        preview(false);
        tree->setCurrentItem(notes);
        preview(false);
        tree->setCurrentItem(album);
        preview(false);
        tree->clearSelection();
        track->setSelected(true);
        complete->setSelected(true);
        preview(false);
        tree->setCurrentItem(track);
        tree->setPathMappings({});
        preview(false);
        QCOMPARE(m_fileOpenRequests.size(), 2);
        QCOMPARE(server.commandsFor("/api/v2/torrents/filePrio"), 0);
        QCOMPARE(server.commandsFor("/api/v2/torrents/renameFile"), 0);
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

    void downloadsDialogBehaviorAndDuplicateDefaults()
    {
        DemoQbittorrentServer server(true);
        server.setServerPreferences({
            {QStringLiteral("merge_trackers"), false},
            {QStringLiteral("add_stopped_enabled"), true}
        });
        const QByteArray tracker("https://tracker.duplicate.example/announce");
        const auto makeMetadata = [&tracker](const QByteArray &name) {
            return QByteArrayLiteral("d8:announce") + QByteArray::number(tracker.size()) + ':'
                + tracker + QByteArrayLiteral("4:infod6:lengthi123e4:name")
                + QByteArray::number(name.size()) + ':' + name + QByteArrayLiteral("ee");
        };
        const QByteArray duplicate = makeMetadata("example");
        const QByteArray fresh = makeMetadata("new-example");
        Remote::TorrentMetadata parsed;
        QVERIFY(Remote::TorrentMetadataReader::read(duplicate, parsed));
        server.setExistingTorrentHash(parsed.infoHashV1);
        QVERIFY(server.start());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString duplicatePath = temp.filePath(QStringLiteral("duplicate.torrent"));
        const QString freshPath = temp.filePath(QStringLiteral("fresh.torrent"));
        for (const auto &fileData : QList<QPair<QString, QByteArray>>{
                 {duplicatePath, duplicate}, {freshPath, fresh}})
        {
            QFile file(fileData.first);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(fileData.second), qint64(fileData.second.size()));
        }
        QSettings settings(temp.filePath(QStringLiteral("prefs.ini")), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        openConnectDialog(window, server.url());
        auto *table = window.findChild<QTableView *>(QStringLiteral("torrentList"));
        QVERIFY(table);
        QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 1, 6000);
        auto *prefs = window.findChild<QAction *>(QStringLiteral("actionOptions"));
        auto *add = window.findChild<QAction *>(QStringLiteral("actionOpen"));
        QVERIFY(prefs && add);

        // Drive the visible controls, including their dependency on showing
        // the options dialog. Cancel must not persist either local or remote edits.
        const auto edit = [&](bool show, bool merge, bool ask, bool save) {
            bool finished = false;
            QTimer::singleShot(0, &window, [&]() {
                auto *dialog = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget());
                QVERIFY(dialog);
                const auto cleanup = qScopeGuard([dialog]() {
                    if (dialog->isVisible()) dialog->reject();
                });
                auto *sections = dialog->findChild<QListWidget *>(QStringLiteral("preferencesSections"));
                auto *display = dialog->findChild<QGroupBox *>(QStringLiteral("checkAdditionDialog"));
                auto *front = dialog->findChild<QCheckBox *>(QStringLiteral("checkAdditionDialogFront"));
                auto *mergeBox = dialog->findChild<QCheckBox *>(QStringLiteral("checkMergeTrackers"));
                auto *confirm = dialog->findChild<QCheckBox *>(QStringLiteral("checkConfirmMergeTrackers"));
                auto *apply = dialog->findChild<QPushButton *>(QStringLiteral("preferencesApply"));
                QVERIFY(sections && display && front && mergeBox && confirm && apply);
                sections->setCurrentRow(2);
                QTRY_VERIFY_WITH_TIMEOUT(mergeBox->isEnabled(), 6000);
                QCOMPARE(mergeBox->isChecked(), server.serverPreferences()
                    .value(QStringLiteral("merge_trackers")).toBool());
                display->setChecked(true);
                QVERIFY(front->isEnabled() && confirm->isEnabled());
                confirm->setChecked(ask);
                front->setChecked(false);
                mergeBox->setChecked(merge);
                display->setChecked(show);
                QCOMPARE(front->isEnabled(), show);
                QCOMPARE(confirm->isEnabled(), show);
                if (save)
                {
                    QVERIFY(apply->isEnabled());
                    apply->click();
                    QTRY_VERIFY_WITH_TIMEOUT(mergeBox->isEnabled() && !apply->isEnabled(), 6000);
                }
                finished = true;
            });
            prefs->trigger();
            QVERIFY(finished);
        };
        const bool originalNative = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        const auto restoreNative = qScopeGuard([originalNative]() {
            QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, originalNative);
        });
        const auto upload = [&](const QString &filename, int dialogs, int prompts) {
            MergeDuplicateTorrentUi interaction(filename);
            QApplication::instance()->installEventFilter(&interaction);
            const auto removeFilter = qScopeGuard([&]() {
                QApplication::instance()->removeEventFilter(&interaction);
            });
            QTimer watchdog;
            watchdog.setSingleShot(true);
            connect(&watchdog, &QTimer::timeout, &window, []() {
                for (QWidget *widget : QApplication::topLevelWidgets())
                    if (auto *modal = qobject_cast<QDialog *>(widget); modal && modal->isVisible())
                        modal->reject();
            });
            watchdog.start(10000);
            add->trigger();
            watchdog.stop();
            QCOMPARE(interaction.chosenFiles, 1);
            QCOMPARE(interaction.addDialogs, dialogs);
            QCOMPARE(interaction.mergePrompts, prompts);
        };

        edit(false, true, false, false);
        QCOMPARE(server.commandsFor("/api/v2/app/setPreferences"), 0);
        upload(duplicatePath, 1, 1); // Cancel kept the confirmation.
        QTRY_COMPARE_WITH_TIMEOUT(server.commandsFor("/api/v2/torrents/addTrackers"), 1, 6000);

        edit(true, true, false, true);
        QVERIFY(server.serverPreferences().value(QStringLiteral("merge_trackers")).toBool());
        upload(duplicatePath, 1, 0); // Server permits merge; no prompt.
        QTRY_COMPARE_WITH_TIMEOUT(server.commandsFor("/api/v2/torrents/addTrackers"), 2, 6000);

        edit(false, true, false, true);
        upload(duplicatePath, 0, 0); // Direct addition uses the server merge default.
        QTRY_COMPARE_WITH_TIMEOUT(server.commandsFor("/api/v2/torrents/addTrackers"), 3, 6000);

        edit(false, false, false, true);
        QVERIFY(!server.serverPreferences().value(QStringLiteral("merge_trackers")).toBool());
        upload(duplicatePath, 0, 0); // Neither options nor merge confirmation.
        upload(freshPath, 0, 0);
        QTRY_COMPARE_WITH_TIMEOUT(server.commandsFor("/api/v2/torrents/add"), 1, 6000);
        QCOMPARE(server.commandsFor("/api/v2/torrents/addTrackers"), 3);
        // A skipped dialog must leave the NAS's chosen defaults intact,
        // not send the desktop's previously hard-coded values.
        const QByteArray body = server.lastCommandBody("/api/v2/torrents/add");
        QVERIFY(body.contains(fresh));
        for (const QByteArray &field : {QByteArray("stopped"), QByteArray("autoTMM"),
             QByteArray("contentLayout"), QByteArray("addToTopOfQueue"), QByteArray("stopCondition"),
             QByteArray("savepath")})
            QVERIFY(!body.contains("name=\"" + field + "\""));
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




    void userOpensMappedRemoteTorrentLocationInDesktopFileManager()
    {
        DemoQbittorrentServer server(true);
        server.enableTorrentSyncDemo();
        server.enableMappedFolderDemo();
        server.keepDemoTorrents();
        QVERIFY(server.start());

        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString root = temp.filePath(QStringLiteral("NAS"));
        const QString music = temp.filePath(QStringLiteral("NAS/Music"));
        QVERIFY(QDir().mkpath(music));
        QFile localFile(QDir(music).filePath(QStringLiteral("recording.flac")));
        QVERIFY(localFile.open(QIODevice::WriteOnly));
        QCOMPARE(localFile.write(QByteArrayLiteral("metadata-free test file")), qint64(23));
        localFile.close();

        QSettings settings(temp.filePath(QStringLiteral("profiles.ini")), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        QTRY_VERIFY(window.isVisible());
        openConnectDialog(window, server.url());

        auto *table = window.findChild<QTableView *>(QStringLiteral("torrentList"));
        QVERIFY(table);
        QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 1, 6000);
        const auto saved = Remote::ProfileStore(settings).profiles();
        QCOMPARE(saved.size(), 1);
        Remote::PathMappingStore mapping(settings);
        // The preferred destination differs from the torrent's existing
        // server path. Opening must still resolve the secondary alias.
        QVERIFY(mapping.replace(saved.first().id, {
            {root, QStringLiteral("/share/Public/NAS")},
            {root, QStringLiteral("/share/NAS")}
        }));

        table->selectRow(0);
        const QModelIndex row = table->selectionModel()->selectedRows(0).value(0);
        QVERIFY(row.isValid());

        // Intercept the actual desktop file-open request without spawning a
        // local file manager during CI. The user action and mapping remain real.
        struct RestoreDesktopOpen {
            ~RestoreDesktopOpen() { QDesktopServices::unsetUrlHandler(QStringLiteral("file")); }
        } restoreHandler;
        m_fileOpenRequests.clear();
        QDesktopServices::setUrlHandler(QStringLiteral("file"),
                                      this, "recordFileOpenRequest");

        QString error;
        QTimer::singleShot(0, &window, [&]() {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu || menu->objectName() != QLatin1String("torrentListContextMenu"))
            {
                error = QStringLiteral("Native torrent context menu did not open");
                if (menu) menu->close();
                return;
            }
            QAction *open = nullptr;
            for (QAction *action : menu->actions())
                if (action->objectName() == QLatin1String("torrentContextOpenDestinationFolder"))
                    open = action;
            const QPixmap expected = QIcon(
                QStringLiteral(":/icons/directory.svg")).pixmap(16, 16);
            if (!open || !open->isEnabled() || !open->isIconVisibleInMenu()
                || open->icon().pixmap(16, 16).toImage() != expected.toImage())
                error = QStringLiteral("Open destination folder has wrong Qt menu or icon");
            else
                open->trigger();
            menu->close();
        });

        QVERIFY(QMetaObject::invokeMethod(table, "customContextMenuRequested",
            Qt::DirectConnection, Q_ARG(QPoint, table->visualRect(row).center())));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(m_fileOpenRequests.size(), 1);
        QCOMPARE(m_fileOpenRequests.first(), QUrl::fromLocalFile(music));
        // Opening a local folder must NOT send any torrent mutation.
        QCOMPARE(server.commandsFor("/api/v2/torrents/setLocation"), 0);
    }


    void userSetsRemoteLocationUsingActualTorrentContextMenu()
    {
        DemoQbittorrentServer server(true);
        server.enableTorrentSyncDemo();
        server.enableMappedFolderDemo();
        server.keepDemoTorrents();
        QVERIFY(server.start());

        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QVERIFY(QDir().mkpath(temp.filePath(QStringLiteral("NAS/New Location"))));
        QSettings settings(temp.filePath(QStringLiteral("profiles.ini")), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        QTRY_VERIFY(window.isVisible());
        openConnectDialog(window, server.url());

        auto *table = window.findChild<QTableView *>(QStringLiteral("torrentList"));
        QVERIFY(table);
        QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 1, 6000);
        const auto saved = Remote::ProfileStore(settings).profiles();
        QCOMPARE(saved.size(), 1);
        Remote::PathMappingStore mappings(settings);
        QVERIFY(mappings.replace(saved.first().id,
            {{temp.filePath(QStringLiteral("NAS")), QStringLiteral("/share/NAS")}}));

        table->selectRow(0);
        const QModelIndex row = table->selectionModel()->selectedRows(0).value(0);
        QVERIFY(row.isValid());
        QString error;
        int locationDialogs = 0;

        // Exercise the actual native menu and dialog with its path widget,
        // not a direct call to the backend or an internal signal.
        class LocationDialogDriver final : public QObject {
        public:
            int count = 0;
            QString error;
            bool eventFilter(QObject *obj, QEvent *event) override
            {
                if (event->type() != QEvent::Show)
                    return QObject::eventFilter(obj, event);
                auto *dialog = qobject_cast<QDialog *>(obj);
                if (!dialog || dialog->objectName() != QLatin1String("setTorrentLocationDialog"))
                    return QObject::eventFilter(obj, event);
                ++count;
                QTimer::singleShot(0, dialog, [this, dialog]() {
                    auto *selector = dialog->findChild<QComboBox *>(
                        QStringLiteral("remoteSetLocationPath"));
                    auto *buttons = dialog->findChild<QDialogButtonBox *>(
                        QStringLiteral("setTorrentLocationButtons"));
                    if (!selector || !buttons || !buttons->button(QDialogButtonBox::Ok)
                        || selector->currentText() != QStringLiteral("/share/NAS/Music"))
                    {
                        error = QStringLiteral("Set Location dialog did not reflect remote save path");
                        dialog->reject();
                        return;
                    }
                    auto *edit = dialog->findChild<RemoteFileSystemPathComboEdit *>("setTorrentLocationSelector");
                    auto *browse = edit ? edit->findChild<QToolButton *>("browseFolderButton") : nullptr;
                    if (!browse || !browse->menu() || browse->popupMode() != QToolButton::MenuButtonPopup)
                    {
                        error = QStringLiteral("Set Location does not provide the shared split browse button");
                        dialog->reject();
                        return;
                    }
                    selector->setEditText("/browse");
                    QTimer drive;
                    QObject::connect(&drive, &QTimer::timeout, &drive, [] {
                        auto *picker = qobject_cast<RemoteFileDialog *>(QApplication::activeModalWidget());
                        if (!picker) return;
                        auto *box = picker->findChild<QDialogButtonBox *>();
                        if (box && box->button(QDialogButtonBox::Ok)->isEnabled())
                            box->button(QDialogButtonBox::Ok)->click();
                    });
                    QTimer timeout;
                    timeout.setSingleShot(true);
                    QObject::connect(&timeout, &QTimer::timeout, &timeout, [] {
                        if (auto *picker = qobject_cast<RemoteFileDialog *>(QApplication::activeModalWidget()))
                            picker->reject();
                        if (auto *popup = QApplication::activePopupWidget()) popup->close();
                    });
                    drive.start(10);
                    timeout.start(4000);
                    browse->click();
                    if (edit->property("explicitRemotePath").toString() != QStringLiteral("/browse"))
                    {
                        error = QStringLiteral("Remote browsing did not select the server directory");
                        dialog->reject();
                        return;
                    }
                    // Manual local path translation must still work after browsing.
                    selector->setEditText(property("localPath").toString());
                    buttons->button(QDialogButtonBox::Ok)->click();
                });
                return QObject::eventFilter(obj, event);
            }
        } driver;
        QApplication::instance()->installEventFilter(&driver);
        const QString localLocation = temp.filePath(QStringLiteral("NAS/New Location"));
        // Use per-instance data on the driver to avoid static global paths.
        driver.setProperty("localPath", localLocation);
        QTimer::singleShot(0, &window, [&]() {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu || menu->objectName() != QLatin1String("torrentListContextMenu"))
            {
                error = QStringLiteral("No native torrent context menu");
                if (menu) menu->close();
                return;
            }
            QAction *action = nullptr;
            for (QAction *a : menu->actions())
                if (a->objectName() == QLatin1String("torrentContextSetLocation"))
                    action = a;
            const QPixmap expected = QIcon(
                QStringLiteral(":/icons/set-location.svg")).pixmap(16, 16);
            if (!action || !action->isEnabled() || !action->isIconVisibleInMenu()
                || action->icon().pixmap(16, 16).toImage() != expected.toImage())
                error = QStringLiteral("Native Set Location action is absent or has wrong icon");
            else
                action->trigger();
            menu->close();
        });
        QVERIFY(QMetaObject::invokeMethod(table, "customContextMenuRequested",
            Qt::DirectConnection, Q_ARG(QPoint, table->visualRect(row).center())));
        QApplication::instance()->removeEventFilter(&driver);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY2(driver.error.isEmpty(), qPrintable(driver.error));
        QCOMPARE(driver.count, 1);

        QTRY_COMPARE_WITH_TIMEOUT(server.commandsFor("/api/v2/torrents/setLocation"), 1, 6000);
        const QUrlQuery post(QString::fromUtf8(
            server.lastCommandBody("/api/v2/torrents/setLocation")));
        QCOMPARE(post.queryItemValue(QStringLiteral("hashes")),
                 QStringLiteral("0123456789abcdef0123456789abcdef01234567"));
        QCOMPARE(post.queryItemValue(QStringLiteral("location")),
                 QStringLiteral("/share/NAS/New Location"));
        QCOMPARE(server.commandsFor("/api/v2/torrents/rename"), 0);
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

    void userRenamesSelectedTorrentThroughActualContextMenuAndDialog()
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
        QVERIFY(table->selectionModel()->hasSelection());

        // The server can synchronize the displayed name between polls.
        // Read the name the person actually sees, rather than asserting a
        // stale fixture value that is no longer on screen.
        const QString original = table->model()->index(0, 0).data(Qt::DisplayRole).toString();
        QVERIFY(!original.isEmpty());
        const QString renamed = QStringLiteral("My & Dvořák + Test / 2026");
        QString error;
        // Observe the real modal rename dialog, not a synthetic signal
        // or direct RemoteSession method invocation.
        class RenameDialogDriver final : public QObject {
        public:
            QString initial;
            QString wanted;
            QString problem;
            int dialogs = 0;
            int firstWidth = 0;
            bool eventFilter(QObject *obj, QEvent *event) override {
                if (event->type() != QEvent::Show)
                    return QObject::eventFilter(obj, event);
                auto *dialog = qobject_cast<QInputDialog *>(obj);
                if (!dialog || dialog->objectName() != QLatin1String("renameTorrentDialog"))
                    return QObject::eventFilter(obj, event);
                ++dialogs;
                const int pass = dialogs;
                QTimer::singleShot(0, dialog, [this, dialog, pass]() {
                    if (dialog->windowTitle() != QStringLiteral("Rename")
                        || dialog->labelText() != QStringLiteral("New name:"))
                        problem = QStringLiteral("Rename dialog heading or label is incorrect");
                    if (pass == 1)
                    {
                        if (dialog->textValue() != initial || dialog->width() < 480)
                            problem = QStringLiteral("Rename dialog has wrong name or default size");
                        dialog->resize(710, 185);
                        firstWidth = dialog->width();
                        dialog->setTextValue(wanted);
                        dialog->accept();
                    }
                    else
                    {
                        // The next invocation must restore the person's
                        // resized dialog. Cancel must not issue another POST.
                        if (dialog->width() != firstWidth)
                            problem = QStringLiteral("Resized rename geometry was not restored");
                        dialog->reject();
                    }
                });
                return QObject::eventFilter(obj, event);
            }
        } driver;
        driver.initial = original;
        driver.wanted = renamed;
        QApplication::instance()->installEventFilter(&driver);
        QTimer::singleShot(0, &window, [&]() {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu || menu->objectName() != QLatin1String("torrentListContextMenu"))
            {
                error = QStringLiteral("Right click did not show torrent context menu");
                if (menu) menu->close();
                return;
            }
            QAction *rename = nullptr;
            for (QAction *a : menu->actions())
                if (a->objectName() == QLatin1String("torrentContextRename"))
                    rename = a;
            if (!rename || !rename->isEnabled())
                error = QStringLiteral("Rename action is absent or disabled");
            else
                rename->trigger();
            menu->close();
        });
        const QModelIndex row = table->selectionModel()->selectedRows(0).value(0);
        QVERIFY(row.isValid());
        QVERIFY(QMetaObject::invokeMethod(table, "customContextMenuRequested",
            Qt::DirectConnection, Q_ARG(QPoint, table->visualRect(row).center())));
        QApplication::instance()->removeEventFilter(&driver);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY2(driver.problem.isEmpty(), qPrintable(driver.problem));
        QCOMPARE(driver.dialogs, 1);

        QTRY_COMPARE_WITH_TIMEOUT(server.commandsFor("/api/v2/torrents/rename"), 1, 6000);
        const QUrlQuery form(QString::fromUtf8(
            server.lastCommandBody("/api/v2/torrents/rename")));
        QCOMPARE(form.queryItemValue(QStringLiteral("hash")),
                 QStringLiteral("0123456789abcdef0123456789abcdef01234567"));
        QCOMPARE(form.queryItemValue(QStringLiteral("name")), renamed);
        QVERIFY(driver.firstWidth >= 700);
        QVERIFY(!settings.value(QStringLiteral("Layout/Dialogs/RenameTorrent"))
                     .toByteArray().isEmpty());

        // Use keyboard F2 to reopen the SAME Rename dialog, check the
        // persisted geometry, then cancel without sending another POST.
        QApplication::instance()->installEventFilter(&driver);
        const QModelIndex selectedAgain = table->selectionModel()->selectedRows(0).value(0);
        QVERIFY(selectedAgain.isValid());
        auto *renameShortcut = table->findChild<QShortcut *>(
            QStringLiteral("torrentRenameShortcut"));
        QVERIFY(renameShortcut);
        QCOMPARE(renameShortcut->key(), QKeySequence(Qt::Key_F2));
        QCOMPARE(renameShortcut->context(), Qt::WidgetWithChildrenShortcut);
        QCOMPARE(renameShortcut->parent(), table);
        // Offscreen QtTest cannot reliably activate table focus following a
        // modal menu; invoke the registered F2 action to test real routing
        // to the MainWindow dialog and authenticated server.
        QVERIFY(QMetaObject::invokeMethod(renameShortcut, "activated",
                                         Qt::DirectConnection));
        QApplication::instance()->removeEventFilter(&driver);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY2(driver.problem.isEmpty(), qPrintable(driver.problem));
        QCOMPARE(driver.dialogs, 2);
        QCOMPARE(server.commandsFor("/api/v2/torrents/rename"), 1);

        // F2 in the toolbar's search field must not open a Rename dialog.
        auto *search = window.findChild<QLineEdit *>(QStringLiteral("torrentSearch"));
        QVERIFY(search);
        search->setFocus();
        QTest::keyClick(search, Qt::Key_F2);
        QCOMPARE(driver.dialogs, 2);

        // Changing a display name must never rename the remote files,
        // move the save path, or change the selected torrent's queue slot.
        QCOMPARE(server.commandsFor("/api/v2/torrents/setLocation"), 0);
        QCOMPARE(server.commandsFor("/api/v2/torrents/renameFile"), 0);
        QCOMPARE(server.commandsFor("/api/v2/torrents/increasePrio"), 0);
    }

    void realTorrentActionsUseAuthenticatedServerApis()
    {
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
        // HTTP operation acceptance must not displace the default identity.
        // Wait for the actual acknowledged requests, then inspect the status.
        QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("Demo seedbox")));
        QVERIFY(window.statusBar()->currentMessage().contains(server.reportedQbtVersion()));
        QVERIFY(window.statusBar()->currentMessage().contains(server.reportedApiVersion()));
        QVERIFY(!window.statusBar()->currentMessage().contains(
            QStringLiteral("awaiting server synchronization")));
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

        auto *prefs = action("actionOptions");
        QVERIFY(prefs);
        const auto setDefault = [&](bool confirm, bool content, bool applyChanges) {
            bool exercised = false;
            QTimer::singleShot(0, &window, [&] {
                auto *dialog = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget());
                if (!dialog) return;
                const auto cleanup = qScopeGuard([dialog] { if (dialog->isVisible()) dialog->reject(); });
                auto *nav = dialog->findChild<QListWidget *>("preferencesSections");
                auto *confirmBox = dialog->findChild<QCheckBox *>("confirmDeletion");
                auto *defaultBox = dialog->findChild<QCheckBox *>("checkDefaultDeleteTorrentContent");
                auto *apply = dialog->findChild<QPushButton *>("preferencesApply");
                QVERIFY(nav && confirmBox && defaultBox && apply);
                nav->setCurrentRow(1);
                QCOMPARE(defaultBox->isChecked(), settings.value("Preferences/Behavior/DefaultDeleteTorrentContent", false).toBool());
                confirmBox->setChecked(true);
                QVERIFY(defaultBox->isEnabled());
                defaultBox->setChecked(content);
                confirmBox->setChecked(confirm);
                QCOMPARE(defaultBox->isEnabled(), confirm);
                if (applyChanges)
                {
                    QVERIFY(apply->isEnabled());
                    apply->click();
                    QTRY_VERIFY(!apply->isEnabled());
                }
                exercised = true;
            });
            prefs->trigger();
            QVERIFY(exercised);
        };
        const auto removeWithChoice = [&](bool expectedDefault, bool content, bool accept) {
            bool exercised = false;
            table->selectRow(0);
            QTimer::singleShot(0, &window, [&] {
                auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                if (!dialog) return;
                const auto cleanup = qScopeGuard([dialog] { if (dialog->isVisible()) dialog->reject(); });
                QVERIFY(dialog->objectName() == QStringLiteral("removeTorrentsDialog"));
                QVERIFY(dialog->checkBox());
                QCOMPARE(dialog->checkBox()->isChecked(), expectedDefault);
                QCOMPARE(dialog->defaultButton(), dialog->button(QMessageBox::Cancel));
                QCOMPARE(dialog->button(QMessageBox::Yes)->text(), expectedDefault
                    ? QStringLiteral("Remove torrent and content") : QStringLiteral("Remove torrent"));
                dialog->checkBox()->setChecked(content);
                QCOMPARE(dialog->button(QMessageBox::Yes)->text(), content
                    ? QStringLiteral("Remove torrent and content") : QStringLiteral("Remove torrent"));
                exercised = true;
                dialog->button(accept ? QMessageBox::Yes : QMessageBox::Cancel)->click();
            });
            remove->trigger();
            QVERIFY(exercised);
        };
        setDefault(true, true, false);
        QVERIFY(!settings.value("Preferences/Behavior/DefaultDeleteTorrentContent", false).toBool());
        removeWithChoice(false, true, false);
        QCOMPARE(server.commandsFor("/api/v2/torrents/delete"), 1);
        setDefault(true, true, true);
        QVERIFY(settings.value("Preferences/Behavior/DefaultDeleteTorrentContent").toBool());
        removeWithChoice(true, false, true);
        QTRY_COMPARE_WITH_TIMEOUT(server.commandsFor("/api/v2/torrents/delete"), 2, 5000);
        QCOMPARE(QUrlQuery(QString::fromUtf8(server.lastCommandBody("/api/v2/torrents/delete")))
            .queryItemValue("deleteFiles"), QStringLiteral("false"));
        removeWithChoice(true, true, false);
        QCOMPARE(server.commandsFor("/api/v2/torrents/delete"), 2);
        removeWithChoice(true, true, true);
        QTRY_COMPARE_WITH_TIMEOUT(server.commandsFor("/api/v2/torrents/delete"), 3, 5000);
        QCOMPARE(QUrlQuery(QString::fromUtf8(server.lastCommandBody("/api/v2/torrents/delete")))
            .queryItemValue("deleteFiles"), QStringLiteral("true"));
        // A remembered destructive default must never turn a non-confirmed
        // deletion into a remote content deletion.
        setDefault(false, true, true);
        QVERIFY(settings.value("Preferences/Behavior/DefaultDeleteTorrentContent").toBool());
        QVERIFY(!settings.value("Preferences/Behavior/ConfirmTorrentDeletion").toBool());
        table->selectRow(0);
        remove->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(server.commandsFor("/api/v2/torrents/delete"), 4, 5000);
        QCOMPARE(QUrlQuery(QString::fromUtf8(server.lastCommandBody("/api/v2/torrents/delete")))
            .queryItemValue("deleteFiles"), QStringLiteral("false"));
        QCOMPARE(server.commandsFor("/api/v2/app/setPreferences"), 0);

        QTimer::singleShot(0, []() {
            auto *dialog = qobject_cast<RemoteAddTorrentDialog *>(
                QApplication::activeModalWidget());
            if (!dialog)
                return;
            auto *links = dialog->findChild<QPlainTextEdit *>(
                QStringLiteral("torrentLinks"));
            auto *buttons = dialog->findChild<QDialogButtonBox *>(
                QStringLiteral("buttonBox"));
            const auto cleanup = qScopeGuard([dialog]() {
                if (dialog->isVisible()) dialog->reject();
            });
            QVERIFY(links && buttons && buttons->button(QDialogButtonBox::Ok));
            QTRY_VERIFY_WITH_TIMEOUT(buttons->button(QDialogButtonBox::Ok)->isEnabled(), 6000);
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
        QVERIFY(addition.queryItemValue(QStringLiteral("urls"), QUrl::FullyDecoded)
                .startsWith(QStringLiteral("magnet:?xt=")));
    }


    void automaticTorrentManagementRequiresUserConsent()
    {
        DemoQbittorrentServer server(true);
        server.enableTorrentSyncDemo();
        server.keepDemoTorrents();
        QVERIFY(server.start());

        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("atm-warning.ini")),
                           QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        QTRY_VERIFY(window.isVisible());
        openConnectDialog(window, server.url());

        auto *pane = window.findChild<ServerPane *>();
        QVERIFY(pane);
        QTRY_COMPARE_WITH_TIMEOUT(pane->table()->model()->rowCount(), 1, 6000);
        pane->table()->selectRow(0);

        // Cancel is the safe default. No server command may be sent.
        bool sawWarning = false;
        QTimer::singleShot(0, &window, [&]() {
            auto *warning = qobject_cast<QMessageBox *>(
                QApplication::activeModalWidget());
            if (!warning)
                return;
            sawWarning = warning->objectName()
                == QStringLiteral("enableAutomaticTorrentManagementWarning")
                && warning->text().contains(QStringLiteral("1 selected torrent"))
                && warning->text().contains(QStringLiteral("Demo seedbox"));
            QCOMPARE(warning->defaultButton()->text(), QStringLiteral("Cancel"));
            warning->reject();
        });
        QVERIFY(QMetaObject::invokeMethod(
            pane, "torrentAutoManagementRequested", Qt::DirectConnection,
            Q_ARG(bool, true)));
        QVERIFY(sawWarning);
        QCOMPARE(server.commandsFor("/api/v2/torrents/setAutoManagement"), 0);

        // Explicit Enable sends the command to this server, not the local
        // filesystem. Disabling later has no modal warning.
        bool acceptedWarning = false;
        QTimer::singleShot(0, &window, [&]() {
            auto *warning = qobject_cast<QMessageBox *>(
                QApplication::activeModalWidget());
            if (!warning)
                return;
            const auto buttons = warning->buttons();
            for (QAbstractButton *button : buttons)
                if (button->text() == QStringLiteral("Enable"))
                {
                    acceptedWarning = true;
                    button->click();
                    break;
                }
        });
        QVERIFY(QMetaObject::invokeMethod(
            pane, "torrentAutoManagementRequested", Qt::DirectConnection,
            Q_ARG(bool, true)));
        QVERIFY(acceptedWarning);
        QTRY_COMPARE_WITH_TIMEOUT(
            server.commandsFor("/api/v2/torrents/setAutoManagement"), 1, 5000);
        const QUrlQuery enabled(QString::fromUtf8(server.lastCommandBody(
            "/api/v2/torrents/setAutoManagement")));
        QCOMPARE(enabled.queryItemValue(QStringLiteral("enable")),
                 QStringLiteral("true"));

        QVERIFY(QMetaObject::invokeMethod(
            pane, "torrentAutoManagementRequested", Qt::DirectConnection,
            Q_ARG(bool, false)));
        QTRY_COMPARE_WITH_TIMEOUT(
            server.commandsFor("/api/v2/torrents/setAutoManagement"), 2, 5000);
        const QUrlQuery disabled(QString::fromUtf8(server.lastCommandBody(
            "/api/v2/torrents/setAutoManagement")));
        QCOMPARE(disabled.queryItemValue(QStringLiteral("enable")),
                 QStringLiteral("false"));
    }


    void nativeTrackerTierArrowsFollowServerCapability()
    {
        DemoQbittorrentServer modern(true), legacy(true);
        modern.enableModernResponses();
        modern.setCapabilityProfile(DemoQbittorrentServer::CapabilityProfile::V520);
        legacy.setCapabilityProfile(DemoQbittorrentServer::CapabilityProfile::V514);
        const QString hash = QStringLiteral("0000000000000000000000000000000000000001");
        const QString first = QStringLiteral("https://tracker.example/announce?key=a+b%2Fc");
        const QString second = QStringLiteral("udp://backup.example:6969/announce");
        const QJsonArray initial{
            QJsonObject{{"url", "** [DHT] **"}, {"tier", -1}},
            QJsonObject{{"url", first}, {"tier", 0}, {"endpoints", QJsonArray{
                QJsonObject{{"name", "tracker.example:443"}}}}},
            QJsonObject{{"url", second}, {"tier", 2}}
        };
        for (auto *server : {&modern, &legacy})
        {
            server->enableNativePresentationDemo();
            server->enableDetailsDemo();
            server->enableStatefulTrackerEditingDemo();
            server->setTrackerEntries(hash, initial);
            QVERIFY(server->start());
        }
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("tiers.ini")), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        openConnectDialog(window, modern.url(), false, true, false, QStringLiteral("Modern"));
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(
            QStringLiteral("Connected to Modern")), 6000);
        openConnectDialog(window, legacy.url(), false, true, true, QStringLiteral("Legacy"));
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(
            QStringLiteral("Connected to Legacy")), 6000);
        auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("serverTabs"));
        QVERIFY(tabs);
        QTRY_COMPARE(tabs->count(), 2);
        for (int index : {1, 0})
        {
            tabs->setCurrentIndex(index);
            auto *pane = tabs->currentWidget();
            auto *table = pane->findChild<QTableView *>(QStringLiteral("torrentList"));
            auto *bar = pane->findChild<PropTabBar *>();
            auto *tree = pane->findChild<QTreeWidget *>(QStringLiteral("torrentTrackers"));
            auto *up = pane->findChild<QPushButton *>(QStringLiteral("trackerUpButton"));
            auto *down = pane->findChild<QPushButton *>(QStringLiteral("trackerDownButton"));
            QVERIFY(table && bar && tree && up && down);
            QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 2, 6000);
            int target = -1;
            for (int row = 0; row < table->model()->rowCount(); ++row)
                if (table->model()->index(row, 0).data().toString() == QStringLiteral("Complete seed"))
                    target = row;
            QVERIFY(target >= 0);
            table->selectRow(target);
            bar->setCurrentIndex(1);
            QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(), 3, 6000);
            const auto find = [&](const QString &url) -> QTreeWidgetItem * {
                for (int row = 0; row < tree->topLevelItemCount(); ++row)
                    if (tree->topLevelItem(row)->text(0) == url)
                        return tree->topLevelItem(row);
                return nullptr;
            };
            QVERIFY(find(first) && find(second) && find("** [DHT] **"));
            find("** [DHT] **")->setSelected(true);
            QVERIFY(!up->isEnabled() && !down->isEnabled());
            tree->clearSelection();
            find(first)->setSelected(true);
            QVERIFY(!up->isEnabled());
            if (index == 1)
            {
                QVERIFY(!down->isEnabled());
                QCOMPARE(legacy.trackerEntries(hash), initial);
                continue;
            }
            QTRY_VERIFY(down->isEnabled());
            find(second)->setSelected(true);
            QTRY_VERIFY(up->isEnabled());
            QTest::mouseClick(down, Qt::LeftButton);
            QVERIFY(!up->isEnabled() && !down->isEnabled());
            QTRY_VERIFY_WITH_TIMEOUT(find(first) && find(second)
                && find(first)->text(1) == QStringLiteral("1")
                && find(second)->text(1) == QStringLiteral("3")
                && up->isEnabled(), 7000);
            QCOMPARE(modern.trackerEntries(hash).at(1).toObject().value("url").toString(), first);
            QCOMPARE(modern.trackerEntries(hash).at(1).toObject().value("tier").toInt(), 1);
            QCOMPARE(modern.trackerEntries(hash).at(2).toObject().value("tier").toInt(), 3);
            QTest::mouseClick(up, Qt::LeftButton);
            QTRY_VERIFY_WITH_TIMEOUT(find(first) && find(second)
                && find(first)->text(1) == QStringLiteral("0")
                && find(second)->text(1) == QStringLiteral("2"), 7000);
            QCOMPARE(modern.trackerEntries(hash), initial);
            QCOMPARE(legacy.trackerEntries(hash), initial);
            QCOMPARE(legacy.commandsFor("/api/v2/torrents/editTracker"), 0);
        }
    }

    void nativeTrackerListEditorReplacesAllSelectedTorrents();
    void privateTrackerAdditionPolicyThroughEditor();
    void nativeManageContentAndTorrentOptionsChangeServerState();
    void remoteTorrentExportReturnsActualMetadata();

    void modernServerLoginAndPreferencesAcceptNoContent()
    {
        DemoQbittorrentServer server(true);
        server.enableModernResponses();
        server.enableTorrentSyncDemo();
        server.setServerPreferences({{QStringLiteral("add_stopped_enabled"), false}});
        QVERIFY(server.start());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings settings(temp.filePath(QStringLiteral("profiles.ini")), QSettings::IniFormat);
        DemoCredentialVault vault;
        MainWindow window(nullptr, &settings, &vault);
        window.show();
        openConnectDialog(window, server.url());
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(
            QStringLiteral("v5.3.0")), 6000);
        QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("2.16.0")));
        auto *table = window.findChild<QTableView *>(QStringLiteral("torrentList"));
        QVERIFY(table);
        QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 1, 6000);
        auto *prefs = window.findChild<QAction *>(QStringLiteral("actionOptions"));
        QVERIFY(prefs);
        bool saved = false;
        QTimer::singleShot(0, &window, [&]() {
            auto *dialog = qobject_cast<RemotePreferencesDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            const auto cleanup = qScopeGuard([dialog]() { if (dialog->isVisible()) dialog->reject(); });
            auto *sections = dialog->findChild<QListWidget *>(QStringLiteral("preferencesSections"));
            auto *stopped = dialog->findChild<QCheckBox *>(QStringLiteral("checkAddStopped"));
            auto *apply = dialog->findChild<QPushButton *>(QStringLiteral("preferencesApply"));
            QVERIFY(sections && stopped && apply);
            sections->setCurrentRow(2);
            QTRY_VERIFY_WITH_TIMEOUT(stopped->isEnabled(), 6000);
            QVERIFY(!stopped->isChecked());
            stopped->click();
            QVERIFY(apply->isEnabled());
            apply->click();
            QTRY_VERIFY_WITH_TIMEOUT(stopped->isEnabled() && !apply->isEnabled(), 6000);
            saved = true;
        });
        prefs->trigger();
        QVERIFY(saved);
        QCOMPARE(server.serverPreferences().value(QStringLiteral("add_stopped_enabled")), QJsonValue(true));
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
        QVERIFY(window.windowTitle().contains(QStringLiteral("Demo seedbox")));
        auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("serverTabs"));
        QVERIFY(tabs && tabs->count() == 1);
        QCOMPARE(tabs->tabText(0), QStringLiteral("Demo seedbox"));
        QVERIFY(!tabs->widget(0)->isEnabled());
        QVERIFY(tabs->tabToolTip(0).contains(QStringLiteral("Login failed")));
        auto *reconnect = window.findChild<QAction *>(QStringLiteral("reconnectServer"));
        QVERIFY(reconnect && reconnect->isEnabled());
        QCOMPARE(vault.reads, 0);
        QCOMPARE(vault.writes, 0);
        QCOMPARE(vault.erases, 0);
    }
};

void ConnectionWorkflowTest::nativeTrackerListEditorReplacesAllSelectedTorrents()
{
    // Exercise the actual native editor against a disposable server that
    // maintains tracker lists across requests. Verify the torrent's visible
    // resulting state, not HTTP serialization or numbers of API calls.
    DemoQbittorrentServer server(true);
    server.enableDetailsDemo();
    server.enableStatefulTrackerEditingDemo();
    QVERIFY(server.start());

    Remote::RemoteSession session;
    Remote::ServerProfile profile {
        {}, QStringLiteral("Music NAS"), server.url(),
        QStringLiteral("demo"), false, false
    };
    QSignalSpy connected(&session, &Remote::RemoteSession::connected);
    session.connectToServer(profile, QStringLiteral("demo-password"));
    QTRY_COMPARE_WITH_TIMEOUT(connected.size(), 1, 6000);

    const QString first(40, QLatin1Char('a'));
    const QString second(40, QLatin1Char('b'));
    const QJsonArray firstOriginal = server.trackerEntries(first);
    const QJsonArray secondOriginal = server.trackerEntries(second);
    QVERIFY(!firstOriginal.isEmpty());
    QVERIFY(!secondOriginal.isEmpty());

    // Editing one torrent displays its full original tracker and passkey.
    RemoteTrackerEntriesDialog single(&session, {first}, [] { return true; });
    single.show();
    auto *singleText = single.findChild<QPlainTextEdit *>(
        QStringLiteral("plainTextEdit"));
    auto *singleButtons = single.findChild<QDialogButtonBox *>(
        QStringLiteral("buttonBox"));
    QVERIFY(singleText && singleButtons);
    QTRY_VERIFY_WITH_TIMEOUT(singleButtons->button(QDialogButtonBox::Ok)->isEnabled(),
                             7000);
    QCOMPARE(singleText->toPlainText().trimmed(),
             firstOriginal.at(0).toObject().value(QStringLiteral("url")).toString());
    singleButtons->button(QDialogButtonBox::Cancel)->click();

    RemoteTrackerEntriesDialog bulk(&session, {first, second}, [] { return true; });
    bulk.show();
    auto *text = bulk.findChild<QPlainTextEdit *>(QStringLiteral("plainTextEdit"));
    auto *buttons = bulk.findChild<QDialogButtonBox *>(QStringLiteral("buttonBox"));
    QVERIFY(text && buttons);
    QTRY_VERIFY_WITH_TIMEOUT(buttons->button(QDialogButtonBox::Ok)->isEnabled(),
                             7000);
    // When the two selected torrents share no complete URL at the same
    // tier, the upstream multi-torrent editor must start blank.
    QCOMPARE(text->toPlainText(), QString());
    buttons->button(QDialogButtonBox::Cancel)->click();
    // The user's Cancel must leave each torrent completely unchanged.
    QCOMPARE(server.trackerEntries(first), firstOriginal);
    QCOMPARE(server.trackerEntries(second), secondOriginal);

    RemoteTrackerEntriesDialog replace(&session, {first, second}, [] { return true; });
    replace.show();
    auto *editor = replace.findChild<QPlainTextEdit *>(QStringLiteral("plainTextEdit"));
    auto *actions = replace.findChild<QDialogButtonBox *>(
        QStringLiteral("buttonBox"));
    QVERIFY(editor && actions);
    QTRY_VERIFY_WITH_TIMEOUT(actions->button(QDialogButtonBox::Ok)->isEnabled(),
                             7000);
    const QString primary = QStringLiteral(
        "https://new.example/announce?passkey=NEW_KEY");
    const QString backup = QStringLiteral("udp://backup.example:6969/announce");
    editor->setPlainText(primary + QStringLiteral("\n\n") + backup);
    actions->button(QDialogButtonBox::Ok)->click();

    // Observe the effect on BOTH selected torrents. The old passkeys are
    // gone and both replacement trackers have the right tier assignments.
    const QJsonArray expected {
        QJsonObject{{QStringLiteral("url"), primary}, {QStringLiteral("tier"), 0}},
        QJsonObject{{QStringLiteral("url"), backup}, {QStringLiteral("tier"), 1}}
    };
    QTRY_VERIFY_WITH_TIMEOUT(server.trackerEntries(first) == expected
                             && server.trackerEntries(second) == expected, 7000);
    QCOMPARE(server.trackerEntries(first), expected);
    QCOMPARE(server.trackerEntries(second), expected);

    // Reopening after the change must show the actual new remote list.
    RemoteTrackerEntriesDialog after(&session, {first, second}, [] { return true; });
    after.show();
    auto *afterText = after.findChild<QPlainTextEdit *>(QStringLiteral("plainTextEdit"));
    auto *afterActions = after.findChild<QDialogButtonBox *>(QStringLiteral("buttonBox"));
    QVERIFY(afterText && afterActions);
    QTRY_VERIFY_WITH_TIMEOUT(afterActions->button(QDialogButtonBox::Ok)->isEnabled(),
                             7000);
    const QStringList displayed = afterText->toPlainText().split(QLatin1Char('\n'));
    QVERIFY(displayed.contains(primary));
    QVERIFY(displayed.contains(backup));
    QVERIFY(displayed.contains(QString()));
    afterActions->button(QDialogButtonBox::Cancel)->click();
}


void ConnectionWorkflowTest::privateTrackerAdditionPolicyThroughEditor()
{
    // Test the real Edit trackers dialog against a stateful remote server:
    // private torrents cannot gain URLs, but replacement, removal, and
    // recovery from a trackerless state still work.
    DemoQbittorrentServer server(true);
    server.enableDetailsDemo();
    server.enableStatefulTrackerEditingDemo();
    QVERIFY(server.start());

    Remote::RemoteSession session;
    Remote::ServerProfile profile {
        {}, QStringLiteral("Music NAS"), server.url(),
        QStringLiteral("demo"), false, false
    };
    QSignalSpy connected(&session, &Remote::RemoteSession::connected);
    session.connectToServer(profile, QStringLiteral("demo-password"));
    QTRY_COMPARE_WITH_TIMEOUT(connected.size(), 1, 6000);

    const QString privateHash(40, QLatin1Char('a'));
    const QString publicHash(40, QLatin1Char('b'));
    const QString oldUrl = QStringLiteral("https://tracker.example/announce/OLD_PASSKEY_ALPHA");
    const QString replacementUrl = QStringLiteral("https://tracker.example/announce/NEW_PASSKEY");
    const QString backup = QStringLiteral("https://backup.example/announce");
    const QString third = QStringLiteral("udp://another.example:6969/announce");
    const auto entry = [](const QString &url, int tier = 0) {
        return QJsonObject {{QStringLiteral("url"), url},
                            {QStringLiteral("tier"), tier}};
    };
    const QJsonArray original {entry(oldUrl)};
    const QJsonArray two {entry(oldUrl), entry(backup, 1)};
    const QJsonArray replacement {entry(replacementUrl)};
    const QJsonArray twoReplaced {entry(replacementUrl), entry(backup, 1)};

    // Each invocation opens the visible editor, enters text and clicks OK.
    // A blocked edit shows a real warning and leaves the editor open;
    // permitted edits complete and update the server's observable state.
    const auto edit = [&](const QStringList &hashes,
                          const QSet<QString> &privateHashes,
                          const QString &text, bool expectBlocked) {
        QSignalSpy completed(&session, &Remote::RemoteSession::torrentTrackersChanged);
        RemoteTrackerEntriesDialog dialog(&session, hashes,
                                          [] { return true; }, nullptr, privateHashes);
        dialog.show();
        auto *editor = dialog.findChild<QPlainTextEdit *>(
            QStringLiteral("plainTextEdit"));
        auto *buttons = dialog.findChild<QDialogButtonBox *>(
            QStringLiteral("buttonBox"));
        QVERIFY(editor && buttons);
        QTRY_VERIFY_WITH_TIMEOUT(buttons->button(QDialogButtonBox::Ok)->isEnabled(),
                                 7000);
        editor->setPlainText(text);
        // Observe the actual warning being shown rather than relying on
        // which modal window a particular platform calls "active" at the
        // moment a zero-delay timer fires.
        class BlockedWarningObserver final : public QObject
        {
        public:
            bool appeared = false;
            bool hasSafetyExplanation = false;
        protected:
            bool eventFilter(QObject *object, QEvent *event) override
            {
                if (event->type() != QEvent::Show)
                    return QObject::eventFilter(object, event);
                auto *message = qobject_cast<QMessageBox *>(object);
                if (!message || message->objectName()
                        != QLatin1String("privateTorrentTrackerAdditionBlocked"))
                    return QObject::eventFilter(object, event);
                appeared = true;
                hasSafetyExplanation = message->text().contains(
                    QStringLiteral("does not allow adding additional trackers"))
                    && message->text().contains(QStringLiteral("Web UI"));
                QTimer::singleShot(0, message, [message]() { message->accept(); });
                return QObject::eventFilter(object, event);
            }
        } warningObserver;
        qApp->installEventFilter(&warningObserver);
        buttons->button(QDialogButtonBox::Ok)->click();
        qApp->removeEventFilter(&warningObserver);
        QCOMPARE(warningObserver.appeared, expectBlocked);
        if (expectBlocked)
            QVERIFY(warningObserver.hasSafetyExplanation);
        QCOMPARE(dialog.result() == QDialog::Accepted, !expectBlocked);
        if (expectBlocked)
        {
            buttons->button(QDialogButtonBox::Cancel)->click();
            QCOMPARE(completed.size(), 0);
        }
        else
        {
            // A URL edit may make the desired tracker list visible BEFORE
            // the client's entire remove/add sequence completes. Only the
            // final server confirmation tells us it is safe to continue to
            // the next user action without overlapping pending mutations.
            QTRY_VERIFY_WITH_TIMEOUT(completed.size() >= hashes.size(), 7000);
        }
    };

    // 1 -> 2: blocked; none of the original tracker URLs is lost.
    edit({privateHash}, {privateHash}, oldUrl + QLatin1Char('\n') + backup, true);
    QCOMPARE(server.trackerEntries(privateHash), original);

    // 1 -> 1 passkey/URL replacement is allowed without a warning and
    // triggers qBittorrent's native peer cleanup on the remote server.
    server.setPeerConnections(privateHash, 3);
    edit({privateHash}, {privateHash}, replacementUrl, false);
    QTRY_COMPARE_WITH_TIMEOUT(server.trackerEntries(privateHash), replacement, 7000);
    QCOMPARE(server.peerConnections(privateHash), 0);

    // 2 -> 3 is blocked, even if a backup tracker is currently inactive.
    server.setTrackerEntries(privateHash, two);
    edit({privateHash}, {privateHash},
         oldUrl + QStringLiteral("\n\n") + backup + QLatin1Char('\n') + third, true);
    QCOMPARE(server.trackerEntries(privateHash), two);

    // 2 -> 2: legitimate replacement with the backup tier preserved.
    server.setPeerConnections(privateHash, 4);
    edit({privateHash}, {privateHash},
         replacementUrl + QStringLiteral("\n\n") + backup, false);
    QTRY_COMPARE_WITH_TIMEOUT(server.trackerEntries(privateHash), twoReplaced, 7000);
    QCOMPARE(server.peerConnections(privateHash), 0);

    // 2 -> 1: removals remain permitted.
    server.setTrackerEntries(privateHash, two);
    edit({privateHash}, {privateHash}, oldUrl, false);
    QTRY_COMPARE_WITH_TIMEOUT(server.trackerEntries(privateHash), original, 7000);

    // 0 -> 2 is not recovery: block any attempt to introduce more than one.
    server.setTrackerEntries(privateHash, {});
    edit({privateHash}, {privateHash}, oldUrl + QLatin1Char('\n') + backup, true);
    QCOMPARE(server.trackerEntries(privateHash), QJsonArray{});

    // 0 -> 1: the user can repair an accidentally trackerless private torrent.
    edit({privateHash}, {privateHash}, oldUrl, false);
    QTRY_COMPARE_WITH_TIMEOUT(server.trackerEntries(privateHash), original, 7000);

    // Mixed bulk edits must not partially modify the public torrent if a
    // private torrent would gain an additional tracker.
    const QJsonArray publicOriginal = server.trackerEntries(publicHash);
    edit({privateHash, publicHash}, {privateHash},
         oldUrl + QLatin1Char('\n') + backup, true);
    QCOMPARE(server.trackerEntries(privateHash), original);
    QCOMPARE(server.trackerEntries(publicHash), publicOriginal);

    // Public torrents remain free to add additional trackers.
    edit({publicHash}, {}, oldUrl + QStringLiteral("\n\n") + backup, false);
    const QJsonArray publicExpected {entry(oldUrl), entry(backup, 1)};
    QTRY_COMPARE_WITH_TIMEOUT(server.trackerEntries(publicHash), publicExpected, 7000);
}


void ConnectionWorkflowTest::nativeManageContentAndTorrentOptionsChangeServerState()
{
    DemoQbittorrentServer server(true);
    server.enableDetailsDemo();
    server.enableRemoteContentAndOptionsDemo();
    QVERIFY(server.start());

    Remote::RemoteSession session;
    Remote::ServerProfile profile {{}, QStringLiteral("NAS"), server.url(),
                                    QStringLiteral("demo"), false, false};
    QSignalSpy connected(&session, &Remote::RemoteSession::connected);
    session.connectToServer(profile, QStringLiteral("demo-password"));
    QTRY_COMPARE_WITH_TIMEOUT(connected.size(), 1, 6000);

    const QString first(40, QLatin1Char('a'));
    const QString second(40, QLatin1Char('b'));
    const QJsonArray initialFiles = server.contentEntries(first);

    // User opens the native Manage Content dialog and changes a file path.
    RemoteTorrentContentLayoutDialog manage(&session, first, [] { return true; });
    manage.show();
    auto *tree = manage.findChild<QTreeView *>(QStringLiteral("pathListView"));
    auto *buttons = manage.findChild<QDialogButtonBox *>(QStringLiteral("buttonBox"));
    QVERIFY(tree && buttons);
    auto *model = qobject_cast<QStandardItemModel *>(tree->model());
    QVERIFY(model);
    QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), 2, 7000);
    QCOMPARE(model->item(1, 1)->text(), QStringLiteral("music/song-two.flac"));
    QCOMPARE(server.contentEntries(first), initialFiles);

    model->item(1, 1)->setText(QStringLiteral("music/renamed-song.flac"));
    QVERIFY(buttons->button(QDialogButtonBox::Apply)->isEnabled());
    buttons->button(QDialogButtonBox::Apply)->click();
    QTRY_COMPARE_WITH_TIMEOUT(server.contentEntries(first).at(1).toObject()
        .value(QStringLiteral("name")).toString(),
        QStringLiteral("music/renamed-song.flac"), 7000);
    QTRY_VERIFY_WITH_TIMEOUT(!buttons->button(QDialogButtonBox::Apply)->isEnabled(), 7000);
    manage.reject();

    // The remote file list survives closing and reopening the native editor.
    RemoteTorrentContentLayoutDialog reopened(&session, first, [] { return true; });
    reopened.show();
    auto *reopenedTree = reopened.findChild<QTreeView *>(QStringLiteral("pathListView"));
    QVERIFY(reopenedTree);
    QTRY_COMPARE_WITH_TIMEOUT(reopenedTree->model()->rowCount(), 2, 7000);
    QCOMPARE(reopenedTree->model()->index(1, 1).data().toString(),
             QStringLiteral("music/renamed-song.flac"));
    reopened.reject();

    const QJsonObject initialFirst = server.optionState(first);
    const QJsonObject initialSecond = server.optionState(second);
    // Open and Cancel must not change anything on either torrent.
    {
        RemoteTorrentOptionsDialog dialog(&session,
            {{first, initialFirst}, {second, initialSecond}},
            {QStringLiteral("music")}, {}, [] { return true; });
        dialog.show();
        dialog.reject();
    }
    QCOMPARE(server.optionState(first), initialFirst);
    QCOMPARE(server.optionState(second), initialSecond);

    // Mixed selection: changing only the upload limit must preserve each
    // torrent's existing ratio/other options and not overwrite with a default.
    {
        RemoteTorrentOptionsDialog dialog(&session,
            {{first, initialFirst}, {second, initialSecond}},
            {QStringLiteral("music")}, {}, [] { return true; });
        dialog.show();
        auto *upload = dialog.findChild<QSpinBox *>(QStringLiteral("spinUploadLimit"));
        QVERIFY(upload);
        upload->setValue(768);
        auto *actions = dialog.findChild<QDialogButtonBox *>(QStringLiteral("buttonBox"));
        QVERIFY(actions);
        actions->button(QDialogButtonBox::Ok)->click();
    }
    QTRY_COMPARE_WITH_TIMEOUT(server.optionState(first).value(QStringLiteral("up_limit"))
        .toInt(), 768 * 1024, 7000);
    QTRY_COMPARE_WITH_TIMEOUT(server.optionState(second).value(QStringLiteral("up_limit"))
        .toInt(), 768 * 1024, 7000);
    QCOMPARE(server.optionState(first).value(QStringLiteral("ratio_limit")),
             initialFirst.value(QStringLiteral("ratio_limit")));
    QCOMPARE(server.optionState(second).value(QStringLiteral("ratio_limit")),
             initialSecond.value(QStringLiteral("ratio_limit")));

    // Reopen with actual server state: changing sequential download and
    // an explicit ratio must update only the requested settings.
    {
        RemoteTorrentOptionsDialog dialog(&session,
            {{first, server.optionState(first)}}, {QStringLiteral("music")},
            {}, [] { return true; });
        dialog.show();
        auto *sequential = dialog.findChild<QCheckBox *>(QStringLiteral("checkSequential"));
        auto *ratioMode = dialog.findChild<QComboBox *>(QStringLiteral("comboBoxRatioMode"));
        auto *ratioValue = dialog.findChild<QDoubleSpinBox *>(
            QStringLiteral("spinBoxRatioValue"));
        auto *actions = dialog.findChild<QDialogButtonBox *>(QStringLiteral("buttonBox"));
        QVERIFY(sequential && ratioMode && ratioValue && actions);
        sequential->click();
        ratioMode->setCurrentIndex(2);
        ratioValue->setValue(1.5);
        actions->button(QDialogButtonBox::Ok)->click();
    }
    QTRY_VERIFY_WITH_TIMEOUT(server.optionState(first).value(QStringLiteral("seq_dl"))
        .toBool(), 7000);
    QTRY_COMPARE_WITH_TIMEOUT(server.optionState(first).value(QStringLiteral("ratio_limit"))
        .toDouble(), 1.5, 7000);
    QCOMPARE(server.optionState(second).value(QStringLiteral("ratio_limit")),
             initialSecond.value(QStringLiteral("ratio_limit")));
}

void ConnectionWorkflowTest::remoteTorrentExportReturnsActualMetadata()
{
    // Exercise the native right-click Export .torrent command all the way
    // through the authenticated server and a real local save-folder picker.
    DemoQbittorrentServer server(true);
    server.enableRemoteContentAndOptionsDemo();
    const QString hash(40, QLatin1Char('a'));
    server.setExistingTorrentHash(hash);
    QVERIFY(server.start());

    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    QSettings settings(temp.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    MainWindow window(nullptr, &settings);
    window.show();
    openConnectDialog(window, server.url());
    auto *table = window.findChild<QTableView *>(QStringLiteral("torrentList"));
    QVERIFY(table);
    QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 1, 7000);

    // Use a non-native QFileDialog in CI. Select a real temporary directory,
    // and assert the resulting .torrent bytes saved on the local filesystem.
    class ChooseExportFolder final : public QObject
    {
    public:
        QString folder;
        int opened = 0;
    protected:
        bool eventFilter(QObject *object, QEvent *event) override
        {
            if (event->type() != QEvent::Show)
                return QObject::eventFilter(object, event);
            if (auto *picker = qobject_cast<QFileDialog *>(object))
            {
                ++opened;
                const QString dir = folder;
                QTimer::singleShot(0, picker, [picker, dir]() {
                    picker->setDirectory(dir);
                    static_cast<QDialog *>(picker)->accept();
                });
            }
            return QObject::eventFilter(object, event);
        }
    } choose;
    choose.folder = temp.path();
    const bool originallyNative = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    QApplication::instance()->installEventFilter(&choose);

    QString error;
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, &window, [&]() {
        for (QWidget *widget : QApplication::topLevelWidgets())
            if (auto *dialog = qobject_cast<QDialog *>(widget);
                dialog && dialog->isVisible())
                dialog->reject();
        error = QStringLiteral("Export folder dialog did not complete.");
    });
    watchdog.start(10000);
    const QModelIndex first = table->model()->index(0, 0);
    table->selectionModel()->select(first, QItemSelectionModel::ClearAndSelect
        | QItemSelectionModel::Rows);
    QTimer::singleShot(0, &window, [&]() {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu)
        {
            error = QStringLiteral("Torrent context menu did not open.");
            return;
        }
        QAction *exportFile = nullptr;
        for (QAction *item : menu->actions())
            if (item->objectName() == QLatin1String("torrentContextExportTorrent"))
                exportFile = item;
        if (!exportFile)
            error = QStringLiteral("Export .torrent is absent.");
        else
            exportFile->trigger();
        menu->close();
    });
    QVERIFY(QMetaObject::invokeMethod(table, "customContextMenuRequested",
        Qt::DirectConnection, Q_ARG(QPoint, table->visualRect(first).center())));
    watchdog.stop();
    QApplication::instance()->removeEventFilter(&choose);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, originallyNative);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(choose.opened, 1);

    const QString destination = temp.filePath(QStringLiteral("Already present.torrent"));
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(destination), 7000);
    QFile saved(destination);
    QVERIFY(saved.open(QIODevice::ReadOnly));
    QCOMPARE(saved.readAll(), server.exportedTorrent());
}

// Preserve QtTest's native QApplication and test setup for this suite.
// The shared dispatcher selects this entry point before Qt is initialized.
#define main runConnectionWorkflow
QTEST_MAIN(ConnectionWorkflowTest)
#undef main
#include "test_connection_workflow.moc"
