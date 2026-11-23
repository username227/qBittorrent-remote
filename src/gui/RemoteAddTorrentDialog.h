// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/PathMappingStore.h"
#include "api/RemoteSession.h"
#include "api/ServerProfile.h"
#include "api/TorrentMetadataReader.h"

#include <QByteArray>
#include <QDialog>
#include <QList>
#include <QSettings>
#include <functional>
#include <memory>

class QComboBox;
class RemoteFileSystemPathComboEdit;

namespace Ui { class AddNewTorrentDialog; }

// Adapted upstream desktop add-torrent UI, not a WebUI imitation.
// A compact server chooser appears only with multiple authenticated sessions.
// LOCAL mount directories translate only through the chosen server's mappings;
// all values sent to the Web API are server-visible paths.
class RemoteAddTorrentDialog final : public QDialog
{
    Q_OBJECT
public:
    enum class Source { Links, TorrentFile, TorrentFiles };

    RemoteAddTorrentDialog(QSettings &settings,
                           const Remote::ServerProfile &server,
                           Source source,
                           QString initialLinks = {},
                           QString fileName = {},
                           QByteArray fileContents = {},
                           QWidget *parent = nullptr,
                           QList<Remote::ServerProfile> availableServers = {},
                           std::function<bool(const QString &)> serverIsConnected = {},
                           QList<Remote::TorrentUpload> batchFiles = {});
    ~RemoteAddTorrentDialog() override;

    Remote::AddTorrentOptions options() const;
    QString selectedServerId() const { return m_server.id; }
    QString links() const;
    QString fileName() const { return m_fileName; }
    QByteArray fileContents() const { return m_fileContents; }
    const QList<Remote::TorrentUpload> &batchFiles() const { return m_batchFiles; }

    // Testable noninteractive path translation used by native Browse buttons.
    bool setLocalSaveDirectory(const QString &path);
    bool setLocalIncompleteDirectory(const QString &path);

protected:
    void accept() override;

private:
    void setupServerSelector();
    void switchServer(const QString &id);
    void clearTargetFields();
    void adaptOptionsGridToFont();
    void setupPaths();
    void setupOptions();
    void setupSource(QString initialLinks);
    void displayMetadata();
    void browseMappedPath(RemoteFileSystemPathComboEdit *target);
    bool validateServerPath(RemoteFileSystemPathComboEdit *combo, QString *resolved, QString *error) const;
    QString resolveServerPath(const QString &text) const;
    QString savedPathKey() const;
    void done(int result) override;

    QSettings &m_settings;
    Remote::ServerProfile m_server;
    Remote::PathMappingStore m_pathStore;
    std::unique_ptr<Ui::AddNewTorrentDialog> m_ui;
    Source m_source;
    QString m_fileName;
    QByteArray m_fileContents;
    QList<Remote::TorrentUpload> m_batchFiles;
    Remote::TorrentMetadata m_metadata;
    class QPlainTextEdit *m_linkInput = nullptr;
    QList<Remote::ServerProfile> m_availableServers;
    QComboBox *m_serverSelector = nullptr;
    std::function<bool(const QString &)> m_serverIsConnected;
};
