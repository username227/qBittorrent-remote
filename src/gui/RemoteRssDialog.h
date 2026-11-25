// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QDialog>
#include <QJsonObject>
#include <QMap>
#include <QPointer>
#include <functional>

class QLabel;
class QTreeWidget;
class QTreeWidgetItem;
class QListWidget;
class QPlainTextEdit;
class QTabWidget;
class QPushButton;
namespace Remote { class RemoteSession; }

class RemoteRssWidget final : public QWidget
{
    Q_OBJECT
public:
    explicit RemoteRssWidget(Remote::RemoteSession *session, QWidget *parent = nullptr,
                             bool rulesFirst = false);
protected:
    void showEvent(QShowEvent *event) override;
private:
    void reload();
    void showItems();
    void showArticles();
    void showArticle();
    void showRules();
    void setBusy(bool busy);
    void failed();
    void updateActions();
    void mutate(const QString &action, const QMap<QString, QString> &params);
    void addItem(bool folder);
    void editFeed();
    void moveItem();
    void removeItem();
    void refreshItem();
    void markRead(bool article);
    void downloadArticle();
    void editRule(bool create);
    void renameRule();
    void removeRule();
    void matchingArticles();
    void checkedRuleMutation(const QString &name, const QJsonValue &baseline,
                             const QString &action, const QMap<QString, QString> &params,
                             const QString &newName = {});
    QString selectedPath() const;
    QString selectedRule() const;
    QJsonObject selectedArticle() const;
    QPointer<Remote::RemoteSession> m_session;
    QTabWidget *m_tabs = nullptr;
    QLabel *m_status = nullptr;
    QTreeWidget *m_feeds = nullptr;
    QTreeWidget *m_articles = nullptr;
    QPlainTextEdit *m_article = nullptr;
    QListWidget *m_ruleList = nullptr;
    QPlainTextEdit *m_ruleSummary = nullptr;
    QPushButton *m_reload = nullptr, *m_addFeed = nullptr, *m_addFolder = nullptr;
    QPushButton *m_editFeed = nullptr, *m_move = nullptr, *m_remove = nullptr;
    QPushButton *m_refresh = nullptr, *m_markItem = nullptr, *m_markArticle = nullptr, *m_download = nullptr;
    QPushButton *m_newRule = nullptr, *m_editRule = nullptr, *m_renameRule = nullptr;
    QPushButton *m_removeRule = nullptr, *m_match = nullptr;
    QJsonObject m_items, m_rules;
    QMap<QString, QJsonObject> m_feedData;
    bool m_busy = false, m_loaded = false, m_rulesLoaded = false;
};

// Preferences retains an independently scoped modal rule editor.
class RemoteRssDialog final : public QDialog
{
public:
    explicit RemoteRssDialog(Remote::RemoteSession *session, const QString &name,
                             QWidget *parent = nullptr, bool rulesFirst = false);
};
