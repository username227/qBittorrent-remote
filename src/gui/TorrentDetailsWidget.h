// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/PathMappingStore.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QWidget>

class QLabel;
class QProgressBar;
class QTableWidget;
class QTreeWidget;
class SpeedGraphWidget;
namespace Ui { class PropertiesWidget; }

// Original qBittorrent property-panel layout, adapted for Web API-only data.
// There are no local torrent or filesystem objects in this widget.
class TorrentDetailsWidget final : public QWidget
{
    Q_OBJECT
public:
    explicit TorrentDetailsWidget(QWidget *parent = nullptr);
    ~TorrentDetailsWidget() override;

    void showTab(int tab);
    int currentTab() const noexcept { return m_currentTab; }
    void setSelection(const QString &hash, const QJsonObject &summary = {});
    void displayDetails(const QString &hash, int tab, const QJsonValue &data);
    void displayError(const QString &hash, int tab, const QString &reason);
    void appendRates(double downloadBytesPerSec, double uploadBytesPerSec);
    void setContentPathMappings(const QList<Remote::PathMapping> &mappings);

signals:
    void filePriorityRequested(const QString &hash, const QList<int> &ids, int priority);

private:
    void displayGeneral(const QJsonObject &properties);
    void displayTrackers(const QJsonArray &trackers);
    void displayPeers(const QJsonObject &peers);
    void displayWebSeeds(const QJsonArray &sources);
    void displayFiles(const QJsonArray &files);
    void chooseFilePriorities(int priority, bool allFiles);
    void updateFileFolders();
    void resetPages();
    void setValue(const char *labelObjectName, const QString &text);

    Ui::PropertiesWidget *m_ui = nullptr;
    QTreeWidget *m_trackers = nullptr;
    QTableWidget *m_peers = nullptr;
    SpeedGraphWidget *m_speedGraph = nullptr;
    QProgressBar *m_progressBar = nullptr;
    QLabel *m_error = nullptr;
    QString m_hash;
    QJsonObject m_summary;
    int m_currentTab = 0;
};
