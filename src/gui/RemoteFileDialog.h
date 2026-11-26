// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/RemoteSession.h"
#include <QDialog>
#include <QPointer>
#include <QStringList>

class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;
class QLabel;
class QPushButton;
class QStringListModel;
class QSettings;

class RemoteFileDialog final : public QDialog
{
    Q_OBJECT
public:
    enum class Mode { Directory, File };
    RemoteFileDialog(Remote::RemoteSession *session, const QString &serverName,
        const QString &profileId, QSettings &settings, const QString &initial,
        Mode mode, QWidget *parent = nullptr);
    QString selectedPath() const { return m_selected; }
    static bool isAbsoluteServerPath(const QString &path);
private:
    void navigate(const QString &path);
    void readEntries(const QString &path, bool directories, quint64 generation);
    void addEntries(const QJsonArray &entries, const QString &path, bool directories);
    void finishListing();
    void updateSelection();
    void disconnected();
    void accept() override;
    QPointer<Remote::RemoteSession> m_session;
    QSettings &m_settings;
    QString m_historyKey, m_directory, m_selected;
    Mode m_mode;
    QLineEdit *m_path = nullptr;
    QTreeWidget *m_entries = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_space = nullptr;
    QPushButton *m_ok = nullptr;
    QPushButton *m_up = nullptr;
    QStringListModel *m_completions = nullptr;
    quint64 m_generation = 0;
    bool m_loading = false;
    bool m_valid = false;
    bool m_requestPending = false;
    QString m_nextDirectory;
};
