// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/MainWindow.h"
#include "gui/ServerPane.h"
#include "gui/RemotePreferencesDialog.h"
#include "gui/RemoteFileSystemPathComboEdit.h"
#include "gui/RemoteSpeedLimitsDialog.h"
#include "gui/MappedContentTreeWidget.h"
#include "gui/TorrentModel.h"
#include "gui/ConnectionDialog.h"
#include "gui/RemoteAddTorrentDialog.h"
#include "gui/RemoteTrackerEntriesDialog.h"
#include "gui/RemoteTorrentContentLayoutDialog.h"
#include "gui/RemoteTorrentOptionsDialog.h"
#include "gui/upstream/transferlistdelegate.h"
#include "gui/upstream/proptabbar.h"
#include "api/CredentialVault.h"
#include "api/RemoteSession.h"
#include "api/TorrentMetadataReader.h"
#include "api/PathMappingStore.h"

#include <iterator>
#include <QAction>
#include <QApplication>
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
#include <QStandardItemModel>
#include <QSpinBox>
#include <QDoubleSpinBox>
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
                            if (m_modernResponses)
                                code = good ? 204 : 401;
                            else
                                response = good ? QByteArrayLiteral("Ok.") : QByteArrayLiteral("Fails.");
                            if (good)
                                cookieHeader = "Set-Cookie: QBT_SID=demo-session; Path=/; HttpOnly\r\n";
                        }
                        else if (path == "/api/v2/app/version" && hasCookie)
                        {
                            response = m_reportedQbtVersion;
                        }
                        else if (path == "/api/v2/app/webapiVersion" && hasCookie)
                        {
                            response = m_reportedApiVersion;
                        }
                        else if (path.startsWith("/api/v2/sync/maindata?rid=") && hasCookie)
                        {
                            ++m_syncRequests;
                            const QUrlQuery query(QString::fromLatin1(path.mid(path.indexOf('?') + 1)));
                            const int rid = query.queryItemValue(QStringLiteral("rid")).toInt();
                            if (!m_sidebarSnapshot.isEmpty())
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
                                    response = R"({"rid":1,"full_update":true,"torrents":{"0000000000000000000000000000000000000001":{"name":"Complete seed","size":1024,"progress":1.0,"state":"stalledUP","availability":-1,"num_seeds":0,"num_complete":3,"num_leechs":0,"num_incomplete":0},"0000000000000000000000000000000000000002":{"name":"Partial download","size":1024,"progress":0.5,"state":"downloading","availability":2.5,"num_seeds":2,"num_complete":5,"num_leechs":4,"num_incomplete":8}},"server_state":{"dl_info_speed":0,"up_info_speed":0}})";
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
                            else if (path.startsWith("/api/v2/torrents/properties?"))
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
                                response = R"({"rid":1,"full_update":true,"peers":{"192.0.2.1:61413":{"client":"qBittorrent","country_code":"US","progress":0.75,"dl_speed":1024,"up_speed":2048,"flags":"D","connection":"TCP"}}})";
                            else if (path.startsWith("/api/v2/torrents/webseeds?"))
                                response = R"([{"url":"https://mirror.example/file.iso"}])";
                            else
                                response = R"([{"index":0,"name":"data/file-one.bin","size":512,"progress":1,"priority":1},{"index":1,"name":"data/file-two.bin","size":512,"progress":0.5,"priority":6}])";
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
                                    for (auto it = updates.constBegin(); it != updates.constEnd(); ++it)
                                        if (m_preferences.contains(it.key()))
                                        {
                                            const bool directory = it.key() == QLatin1String("save_path")
                                                || it.key() == QLatin1String("temp_path");
                                            m_preferences.insert(it.key(), directory && it.value().isString()
                                                ? QJsonValue(QDir::cleanPath(it.value().toString()))
                                                : it.value());
                                        }
                                }
                                response = "Ok.";
                            }
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
                                && path == "/api/v2/torrents/renameFile")
                            {
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
                                    if (row.value(QStringLiteral("name")).toString() == before)
                                    {
                                        row.insert(QStringLiteral("name"), after);
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
                                        QStringLiteral("url"), QUrl::FullyDecoded);
                                    const QString after = form.queryItemValue(
                                        QStringLiteral("newUrl"), QUrl::FullyDecoded);
                                    QJsonArray edited;
                                    bool found = false;
                                    for (QJsonValue item : current)
                                    {
                                        QJsonObject row = item.toObject();
                                        if (row.value(QStringLiteral("url")).toString() == before)
                                        {
                                            row.insert(QStringLiteral("url"), after);
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
                            response = "Ok.";
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
    void enableTorrentSyncDemo() { m_demoTorrents = true; }
    void setSidebarSnapshot(const QJsonObject &snapshot) { m_sidebarSnapshot = snapshot; }
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
    void setServerPreferences(const QJsonObject &values)
    {
        m_preferencesDemo = true;
        m_preferences = values;
    }
    QJsonObject serverPreferences() const { return m_preferences; }
    int preferenceReads() const { return m_preferenceReads; }
    void rejectPreferenceWrites(bool reject) { m_rejectPreferences = reject; }
    void ignorePreferenceWrites(bool ignore) { m_ignorePreferences = ignore; }
    bool trayAlternative() const { return m_trayAltMode; }
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
    QTcpServer m_server;
    bool m_acceptPassword;
    bool m_acceptBlank;
    bool m_modernResponses = false;
    QByteArray m_reportedQbtVersion = QByteArrayLiteral("v5.1.4");
    QByteArray m_reportedApiVersion = QByteArrayLiteral("2.11.3");
    int m_logouts = 0;
    bool m_demoTorrents = false;
    QJsonObject m_sidebarSnapshot;
    bool m_statusRatesDemo = false;
    QJsonObject m_statusRates;
    bool m_trayDemo = false;
    bool m_trayAltMode = false;
    QString m_trayConnectionStatus = QStringLiteral("connected");
    QJsonObject m_traySpeedLimits;
    bool m_preferencesDemo = false;
    bool m_rejectPreferences = false;
    bool m_ignorePreferences = false;
    int m_preferenceReads = 0;
    QJsonObject m_preferences;
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
        QTRY_COMPARE(tabs->count(), 1);
        QCOMPARE(tabs->tabText(0), QStringLiteral("Alpha"));
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

        auto *count = window.statusBar()->findChild<QLabel *>(
            QStringLiteral("visibleTorrentCount"));
        QVERIFY(count);
        QCOMPARE(count->text(), QStringLiteral("Torrents: —"));
        openConnectDialog(window, server.url());
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage()
            .contains(QStringLiteral("Connected to Demo seedbox")), 6000);
        QVERIFY(window.windowTitle().contains(QStringLiteral("Demo seedbox")));
        QTRY_COMPARE(count->text(), QStringLiteral("Torrents: 0 / 0"));

        QAction *disconnect = window.findChild<QAction *>(QStringLiteral("disconnectServer"));
        QVERIFY(disconnect && disconnect->isEnabled());
        disconnect->trigger();
        QTRY_VERIFY(window.statusBar()->currentMessage()
            .contains(QStringLiteral("Not connected")));
        QCOMPARE(count->text(), QStringLiteral("Torrents: —"));
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
            if (edit)
            {
                QCOMPARE(categoryName->text(), name);
                QCOMPARE(savePath->currentText(), QStringLiteral("/share/Classical"));
                QVERIFY(!categoryName->isEnabled());
            }
            else categoryName->setText(name);
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
        QVERIFY(mappings.replace(firstId, {{mount, QStringLiteral("/share/First")}}));
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
            auto *browse = dialog->findChild<QAction *>(QStringLiteral("textSavePathBrowse"));
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

            save->setText(QStringLiteral("relative/path"));
            apply->click();
            QCOMPARE(invalidCount, 1);
            QCOMPARE(first.serverPreferences(), initial);

            nextFolder = temp.filePath(QStringLiteral("unmapped"));
            browse->trigger();
            QCOMPARE(unmappedCount, 1);
            QCOMPARE(save->text(), QStringLiteral("relative/path"));
            nextFolder = mapped;
            browse->trigger();
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
        QVERIFY(peers->horizontalHeader()->sectionsMovable());
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
        QTRY_COMPARE(tabs->count(), 1);
        QVERIFY2(!tabs->tabBar()->isVisible(), "Returning to one server hides tabs.");
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().contains(
            QStringLiteral("Connected to Demo seedbox")), 3000);
        QTRY_COMPARE(count->text(), QStringLiteral("Torrents: 1 / 1"));
        QVERIFY(window.statusBar()->currentMessage().contains(first.reportedQbtVersion()));
        QVERIFY(!window.statusBar()->currentMessage().contains(second.reportedQbtVersion()));
        QTRY_COMPARE_WITH_TIMEOUT(second.logouts(), 1, 6000);
        QCOMPARE(first.logouts(), 0);
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
        QCOMPARE(model.index(7, 0).data(Qt::ForegroundRole).value<QColor>(),
                 QColor(dark ? QStringLiteral("#a371f7") : QStringLiteral("#8250df")));
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
            "/api/v2/torrents/add"))).queryItemValue(QStringLiteral("urls")), magnet);

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
        second->setSelected(true);
        {
            std::unique_ptr<QMimeData> mime(content->mappedSelectionMimeData());
            QVERIFY(mime);
            QCOMPARE(mime->urls().size(), 2);
        }
        // No partial drag if one selected file vanished from the mount.
        QVERIFY(QFile::remove(local.at(1)));
        QVERIFY(content->mappedSelectionMimeData() == nullptr);
        second->setSelected(false);
        // An unmapped content selection offers no misleading local URI.
        content->setPathMappings({});
        QVERIFY(content->mappedSelectionMimeData() == nullptr);
        QCOMPARE(server.commandsFor("/api/v2/torrents/add"), 0);
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
        QVERIFY(mapping.replace(saved.first().id, {{root, QStringLiteral("/share/NAS")}}));

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
                    // User enters a mapped local folder; the application
                    // must translate it to the selected NAS's remote path.
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
        QVERIFY(addition.queryItemValue(QStringLiteral("urls"))
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
        QVERIFY(!window.windowTitle().contains(QStringLiteral("Demo seedbox")));
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
