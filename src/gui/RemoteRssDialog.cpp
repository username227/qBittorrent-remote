// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/RemoteRssDialog.h"
#include "api/RemoteSession.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTextDocumentFragment>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QTimer>
#include <QShowEvent>
#include <QSignalBlocker>
#include <limits>

namespace
{
constexpr int PathRole = Qt::UserRole;
constexpr int ArticleRole = Qt::UserRole + 1;
const QChar sep = QLatin1Char('\\');
QString joinPath(const QString &parent, const QString &name)
{
    return parent.isEmpty() ? name : parent + sep + name;
}
bool validPath(const QString &path)
{
    if (path.isEmpty()) return false;
    for (const QString &part : path.split(sep))
        if (part.trimmed().isEmpty() || part.contains(QLatin1Char('\n')) || part.contains(QLatin1Char('\r')))
            return false;
    return true;
}
bool validUrl(const QString &text, bool magnet = false)
{
    if (text.contains(QLatin1Char('\n')) || text.contains(QLatin1Char('\r'))) return false;
    const QUrl url(text, QUrl::StrictMode);
    return url.isValid() && ((magnet && url.scheme() == QLatin1String("magnet"))
        || ((url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https"))
            && !url.host().isEmpty()));
}
QString stringValue(const QJsonObject &o, const char *key)
{
    return o.value(QLatin1String(key)).toString();
}
QStringList feedUrls(const QJsonValue &v)
{
    QStringList result;
    if (v.isString()) result << v.toString();
    else for (const auto &item : v.toArray()) if (item.isString()) result << item.toString();
    return result;
}
}

RemoteRssWidget::RemoteRssWidget(Remote::RemoteSession *session, QWidget *parent, bool rulesFirst)
    : QWidget(parent), m_session(session)
{
    setObjectName(QStringLiteral("remoteRssWidget"));
    auto *layout = new QVBoxLayout(this);
    auto *note = new QLabel(tr("RSS subscriptions and rules belong to this server. Changes take effect immediately."), this);
    note->setWordWrap(true);
    layout->addWidget(note);
    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName(QStringLiteral("rssTabs"));
    layout->addWidget(m_tabs, 1);
    auto *reader = new QWidget(m_tabs);
    auto *readerLayout = new QVBoxLayout(reader);
    auto *tools = new QHBoxLayout;
    readerLayout->addLayout(tools);
    auto button = [this](QHBoxLayout *row, const QString &label, const char *name, auto slot) {
        auto *b = new QPushButton(label, this);
        b->setObjectName(QString::fromLatin1(name));
        row->addWidget(b);
        connect(b, &QPushButton::clicked, this, slot);
        return b;
    };
    m_addFeed = button(tools, tr("New subscription..."), "rssAddFeed", [this] { addItem(false); });
    m_addFolder = button(tools, tr("New folder..."), "rssAddFolder", [this] { addItem(true); });
    m_editFeed = button(tools, tr("Edit URL..."), "rssEditFeed", [this] { editFeed(); });
    m_move = button(tools, tr("Rename / move..."), "rssMoveItem", [this] { moveItem(); });
    m_remove = button(tools, tr("Remove"), "rssRemoveItem", [this] { removeItem(); });
    tools->addStretch();
    auto *split = new QSplitter(reader);
    m_feeds = new QTreeWidget(split);
    m_feeds->setObjectName(QStringLiteral("rssFeeds"));
    m_feeds->setHeaderLabels({tr("Subscriptions"), tr("Status")});
    m_feeds->setColumnWidth(0, 200);
    auto *right = new QSplitter(Qt::Vertical, split);
    m_articles = new QTreeWidget(right);
    m_articles->setObjectName(QStringLiteral("rssArticles"));
    m_articles->setHeaderLabels({tr("Article"), tr("Date"), tr("Feed")});
    m_articles->setColumnWidth(0, 350);
    m_article = new QPlainTextEdit(right);
    m_article->setObjectName(QStringLiteral("rssArticlePreview"));
    m_article->setReadOnly(true);
    split->setStretchFactor(1, 1);
    right->setStretchFactor(0, 2);
    readerLayout->addWidget(split, 1);
    auto *articleTools = new QHBoxLayout;
    readerLayout->addLayout(articleTools);
    m_refresh = button(articleTools, tr("Refresh feed(s)"), "rssRefreshItem", [this] { refreshItem(); });
    m_markItem = button(articleTools, tr("Mark feed(s) read"), "rssMarkItem", [this] { markRead(false); });
    m_markArticle = button(articleTools, tr("Mark article read"), "rssMarkArticle", [this] { markRead(true); });
    m_download = button(articleTools, tr("Download torrent..."), "rssDownload", [this] { downloadArticle(); });
    articleTools->addStretch();
    connect(m_feeds, &QTreeWidget::currentItemChanged, this, [this] { showArticles(); });
    connect(m_articles, &QTreeWidget::currentItemChanged, this, [this] { showArticle(); });
    m_tabs->addTab(reader, tr("RSS reader"));

    auto *rules = new QWidget(m_tabs);
    auto *ruleLayout = new QVBoxLayout(rules);
    auto *ruleTools = new QHBoxLayout;
    ruleLayout->addLayout(ruleTools);
    m_newRule = button(ruleTools, tr("New rule..."), "rssNewRule", [this] { editRule(true); });
    m_editRule = button(ruleTools, tr("Edit rule..."), "rssEditRule", [this] { editRule(false); });
    m_renameRule = button(ruleTools, tr("Rename..."), "rssRenameRule", [this] { renameRule(); });
    m_removeRule = button(ruleTools, tr("Remove"), "rssRemoveRule", [this] { removeRule(); });
    m_match = button(ruleTools, tr("Matching articles"), "rssMatchingArticles", [this] { matchingArticles(); });
    ruleTools->addStretch();
    auto *ruleSplit = new QSplitter(rules);
    m_ruleList = new QListWidget(ruleSplit);
    m_ruleList->setObjectName(QStringLiteral("rssRules"));
    m_ruleSummary = new QPlainTextEdit(ruleSplit);
    m_ruleSummary->setObjectName(QStringLiteral("rssRuleSummary"));
    m_ruleSummary->setReadOnly(true);
    ruleSplit->setStretchFactor(1, 1);
    ruleLayout->addWidget(ruleSplit, 1);
    connect(m_ruleList, &QListWidget::currentItemChanged, this, [this] {
        const QJsonObject rule = m_rules.value(selectedRule()).toObject();
        m_ruleSummary->setPlainText(rule.isEmpty() ? QString() :
            tr("Enabled: %1\nMust contain: %2\nMust not contain: %3\nEpisode filter: %4\nFeeds: %5")
                .arg(rule.value("enabled").toBool() ? tr("Yes") : tr("No"),
                     stringValue(rule, "mustContain"), stringValue(rule, "mustNotContain"),
                     stringValue(rule, "episodeFilter"), QString::number(feedUrls(rule.value("affectedFeeds")).size())));
        updateActions();
    });
    m_tabs->addTab(rules, tr("Automatic downloading rules"));
    m_tabs->setCurrentIndex(rulesFirst ? 1 : 0);
    auto *footer = new QHBoxLayout;
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("rssStatus"));
    m_status->setWordWrap(true);
    footer->addWidget(m_status, 1);
    m_reload = button(footer, tr("Reload from server"), "rssReload", [this] { reload(); });
    layout->addLayout(footer);
    if (session)
    {
        auto disconnected = [this] {
            m_loaded = m_rulesLoaded = false;
            m_status->setText(tr("Server disconnected. Reopen RSS after reconnecting."));
            setBusy(false);
        };
        connect(session, &Remote::RemoteSession::disconnected, this, disconnected);
        connect(session, &Remote::RemoteSession::failed, this, disconnected);
        connect(session, &QObject::destroyed, this, disconnected);
    }
    updateActions();
}

void RemoteRssWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    QTimer::singleShot(0, this, [this] {
        if (isVisible() && !m_busy && !m_loaded && m_session && m_session->isConnected())
            reload();
    });
}

RemoteRssDialog::RemoteRssDialog(Remote::RemoteSession *session, const QString &name,
                                 QWidget *parent, bool rulesFirst)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("remoteRssDialog"));
    setWindowTitle(tr("RSS — %1").arg(name));
    resize(1000, 650);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new RemoteRssWidget(session, this, rulesFirst), 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void RemoteRssWidget::setBusy(bool busy)
{
    m_busy = busy;
    updateActions();
}
void RemoteRssWidget::failed()
{
    // Server error bodies can contain a private feed URL. Avoid echoing them.
    m_loaded = m_rulesLoaded = false;
    m_status->setText(tr("RSS request failed or could not be confirmed. Reload from the server."));
    setBusy(false);
}
void RemoteRssWidget::reload()
{
    if (!m_session || !m_session->isConnected()) { failed(); return; }
    setBusy(true);
    m_status->setText(tr("Reading server RSS data..."));
    const QPointer<RemoteRssWidget> self(this);
    m_session->requestRss(QStringLiteral("items"), {{"withData", "true"}},
        [self](const QJsonObject &items) {
            if (!self) return;
            self->m_items = items;
            self->m_loaded = true;
            self->showItems();
            if (!self->m_session) { self->failed(); return; }
            self->m_session->requestRss(QStringLiteral("rules"), {},
                [self](const QJsonObject &rules) {
                    if (!self) return;
                    self->m_rules = rules;
                    self->m_rulesLoaded = true;
                    self->showRules();
                    self->m_status->setText(tr("Server snapshot loaded. Feed refreshes may finish asynchronously."));
                    self->setBusy(false);
                }, [self](const QString &) { if (self) { self->m_rulesLoaded = false; self->failed(); } });
        }, [self](const QString &) { if (self) { self->m_loaded = self->m_rulesLoaded = false; self->failed(); } });
}
QString RemoteRssWidget::selectedPath() const
{
    return m_feeds->currentItem() ? m_feeds->currentItem()->data(0, PathRole).toString() : QString();
}
QString RemoteRssWidget::selectedRule() const
{
    return m_ruleList->currentItem() ? m_ruleList->currentItem()->text() : QString();
}
QJsonObject RemoteRssWidget::selectedArticle() const
{
    return m_articles->currentItem() ? m_articles->currentItem()->data(0, ArticleRole).toJsonObject() : QJsonObject();
}
void RemoteRssWidget::showItems()
{
    const QString selected = selectedPath();
    const QSignalBlocker blocker(m_feeds);
    m_feeds->clear();
    m_feedData.clear();
    auto *root = new QTreeWidgetItem(m_feeds, {tr("All feeds")});
    root->setData(0, PathRole, QString());
    QTreeWidgetItem *restore = root;
    std::function<void(const QJsonObject &, QTreeWidgetItem *, const QString &, int)> visit;
    visit = [&](const QJsonObject &object, QTreeWidgetItem *parent, const QString &prefix, int depth) {
        if (depth > 64) return;
        for (auto it = object.begin(); it != object.end(); ++it)
        {
            if (!it.value().isObject()) continue;
            const QJsonObject value = it.value().toObject();
            const QString path = joinPath(prefix, it.key());
            auto *item = new QTreeWidgetItem(parent, {it.key()});
            item->setData(0, PathRole, path);
            if (path == selected) restore = item;
            if (value.value("uid").isString() && value.value("url").isString())
            {
                m_feedData.insert(path, value);
                item->setText(1, value.value("hasError").toBool() ? tr("Error")
                    : value.value("isLoading").toBool() ? tr("Loading")
                    : tr("%1 article(s)").arg(value.value("articles").toArray().size()));
            }
            else visit(value, item, path, depth + 1);
        }
    };
    visit(m_items, root, {}, 0);
    m_feeds->expandAll();
    m_feeds->setCurrentItem(restore);
    showArticles();
}
void RemoteRssWidget::showArticles()
{
    const auto previous = selectedArticle();
    const QString oldPath = m_articles->currentItem()
        ? m_articles->currentItem()->data(0, PathRole).toString() : QString();
    const QSignalBlocker blocker(m_articles);
    m_articles->clear();
    const QString path = selectedPath();
    for (auto it = m_feedData.cbegin(); it != m_feedData.cend(); ++it)
    {
        if (!path.isEmpty() && it.key() != path && !it.key().startsWith(path + sep)) continue;
        for (const auto &entry : it.value().value("articles").toArray())
        {
            if (!entry.isObject()) continue;
            const auto article = entry.toObject();
            auto *row = new QTreeWidgetItem(m_articles, {stringValue(article, "title"),
                stringValue(article, "date"), it.key()});
            row->setData(0, PathRole, it.key());
            row->setData(0, ArticleRole, article);
            if (!previous.isEmpty() && it.key() == oldPath
                && stringValue(previous, "id") == stringValue(article, "id"))
                m_articles->setCurrentItem(row);
            if (!article.value("isRead").toBool())
            {
                auto font = row->font(0);
                font.setBold(true);
                row->setFont(0, font);
            }
        }
    }
    showArticle();
}
void RemoteRssWidget::showArticle()
{
    const auto a = selectedArticle();
    // Plain text only: feed HTML must never fetch resources on the desktop.
    m_article->setPlainText(a.isEmpty() ? QString() : stringValue(a, "title") + QStringLiteral("\n")
        + stringValue(a, "date") + QStringLiteral("\n\n")
        + QTextDocumentFragment::fromHtml(stringValue(a, "description")).toPlainText());
    updateActions();
}
void RemoteRssWidget::showRules()
{
    const QString selected = selectedRule();
    m_ruleList->clear();
    for (auto it = m_rules.begin(); it != m_rules.end(); ++it)
        if (it.value().isObject())
        {
            auto *item = new QListWidgetItem(it.key(), m_ruleList);
            if (it.key() == selected) m_ruleList->setCurrentItem(item);
        }
    if (!m_ruleList->currentItem() && m_ruleList->count()) m_ruleList->setCurrentRow(0);
}
void RemoteRssWidget::updateActions()
{
    const bool online = m_session && m_session->isConnected() && !m_busy;
    const bool feeds = online && m_loaded;
    const bool selected = feeds && !selectedPath().isEmpty();
    const bool feed = selected && m_feedData.contains(selectedPath());
    const bool article = feeds && m_articles->currentItem();
    m_reload->setEnabled(online);
    m_feeds->setEnabled(feeds);
    m_articles->setEnabled(feeds);
    m_addFeed->setEnabled(feeds);
    m_addFolder->setEnabled(feeds);
    m_editFeed->setEnabled(feed);
    m_move->setEnabled(selected);
    m_remove->setEnabled(selected);
    m_refresh->setEnabled(feeds);
    m_markItem->setEnabled(feeds);
    m_markArticle->setEnabled(article && !stringValue(selectedArticle(), "id").isEmpty());
    m_download->setEnabled(article && validUrl(stringValue(selectedArticle(), "torrentURL"), true));
    const bool rules = online && m_loaded && m_rulesLoaded;
    m_ruleList->setEnabled(rules);
    m_newRule->setEnabled(rules);
    const bool rule = rules && !selectedRule().isEmpty();
    for (auto *b : {m_editRule, m_renameRule, m_removeRule, m_match}) b->setEnabled(rule);
}
void RemoteRssWidget::mutate(const QString &action, const QMap<QString, QString> &params)
{
    if (!m_session || !m_session->isConnected()) { failed(); return; }
    setBusy(true);
    m_status->setText(tr("Sending RSS change to server..."));
    const QPointer<RemoteRssWidget> self(this);
    m_session->changeRss(action, params, [self] { if (self) self->reload(); },
        [self](const QString &) { if (self) self->failed(); });
}
void RemoteRssWidget::addItem(bool folder)
{
    QString prefix = selectedPath();
    if (m_feedData.contains(prefix)) { const auto pos = prefix.lastIndexOf(sep); prefix = pos < 0 ? QString() : prefix.left(pos); }
    QDialog dialog(this);
    dialog.setWindowTitle(folder ? tr("New RSS folder") : tr("New RSS subscription"));
    auto *form = new QFormLayout(&dialog);
    auto *path = new QLineEdit(prefix.isEmpty() ? QString() : prefix + sep, &dialog);
    path->setObjectName(QStringLiteral("rssItemPath"));
    form->addRow(folder ? tr("Folder name:") : tr("Subscription name:"), path);
    auto *url = new QLineEdit(&dialog);
    url->setObjectName(QStringLiteral("rssFeedUrl"));
    if (!folder) form->addRow(tr("URL:"), url);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    const QString dest = path->text().trimmed();
    if (!validPath(dest) || (!folder && !validUrl(url->text().trimmed())))
    {
        QMessageBox::warning(this, tr("Invalid subscription"), tr("Enter a name/path and, for a feed, a valid HTTP or HTTPS URL. Use backslashes between folders."));
        return;
    }
    QMap<QString, QString> params{{"path", dest}};
    if (!folder) params.insert("url", url->text().trimmed());
    mutate(folder ? QStringLiteral("addFolder") : QStringLiteral("addFeed"), params);
}
void RemoteRssWidget::editFeed()
{
    const QString path = selectedPath();
    bool ok = false;
    const QString url = QInputDialog::getText(this, tr("Edit feed URL"), tr("Feed URL:"),
        QLineEdit::Normal, stringValue(m_feedData.value(path), "url"), &ok).trimmed();
    if (ok && validUrl(url)) mutate(QStringLiteral("setFeedURL"), {{"path", path}, {"url", url}});
    else if (ok) QMessageBox::warning(this, tr("Invalid feed URL"), tr("Use a valid HTTP or HTTPS URL."));
}
void RemoteRssWidget::moveItem()
{
    const QString path = selectedPath();
    bool ok = false;
    const QString dest = QInputDialog::getText(this, tr("Rename or move subscription"), tr("Destination path (backslash separates folders):"),
        QLineEdit::Normal, path, &ok).trimmed();
    if (ok && validPath(dest) && dest != path) mutate(QStringLiteral("moveItem"), {{"itemPath", path}, {"destPath", dest}});
}
void RemoteRssWidget::removeItem()
{
    const QString path = selectedPath();
    if (!path.isEmpty() && QMessageBox::question(this, tr("Remove RSS subscription"),
        tr("Remove the selected subscription or folder and its articles from this server? Existing torrents will not be removed."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
        mutate(QStringLiteral("removeItem"), {{"path", path}});
}
void RemoteRssWidget::refreshItem()
{
    mutate(QStringLiteral("refreshItem"), {{"itemPath", selectedPath()}});
}
void RemoteRssWidget::markRead(bool article)
{
    QMap<QString, QString> params{{"itemPath", article ? m_articles->currentItem()->data(0, PathRole).toString() : selectedPath()}};
    if (article)
    {
        const auto id = stringValue(selectedArticle(), "id");
        if (id.isEmpty()) return;
        params.insert("articleId", id);
    }
    mutate(QStringLiteral("markAsRead"), params);
}
void RemoteRssWidget::downloadArticle()
{
    const QString url = stringValue(selectedArticle(), "torrentURL");
    if (!m_session || !m_session->isConnected() || !validUrl(url, true)) return;
    if (QMessageBox::question(this, tr("Download RSS torrent"),
        tr("Add this article's torrent to the selected server using its default torrent settings?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    if (!m_session || !m_session->isConnected()) { failed(); return; }
    Remote::AddTorrentOptions options;
    options.useServerDefaults = true;
    m_session->addTorrentUrls(url, options);
}
void RemoteRssWidget::editRule(bool create)
{
    QString name = selectedRule();
    if (create)
    {
        bool ok = false;
        name = QInputDialog::getText(this, tr("New RSS rule"), tr("Rule name:"), QLineEdit::Normal, {}, &ok).trimmed();
        if (!ok || name.isEmpty()) return;
        if (m_rules.contains(name)) { QMessageBox::warning(this, tr("RSS rule"), tr("A rule with that name already exists.")); return; }
    }
    const QJsonValue baseline = m_rules.value(name);
    QJsonObject rule = baseline.toObject();
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("rssRuleEditor"));
    dialog.setWindowTitle(tr("RSS rule — %1").arg(name));
    dialog.resize(550, 500);
    auto *form = new QFormLayout(&dialog);
    auto *enabled = new QCheckBox(&dialog);
    enabled->setObjectName(QStringLiteral("rssRuleEnabled"));
    enabled->setChecked(rule.value("enabled").toBool(false));
    form->addRow(tr("Enabled"), enabled);
    auto *regex = new QCheckBox(&dialog); regex->setChecked(rule.value("useRegex").toBool());
    form->addRow(tr("Use regular expressions"), regex);
    auto *must = new QLineEdit(stringValue(rule, "mustContain"), &dialog);
    must->setObjectName(QStringLiteral("rssRuleMustContain"));
    form->addRow(tr("Must contain"), must);
    auto *exclude = new QLineEdit(stringValue(rule, "mustNotContain"), &dialog);
    form->addRow(tr("Must not contain"), exclude);
    auto *episode = new QLineEdit(stringValue(rule, "episodeFilter"), &dialog);
    form->addRow(tr("Episode filter"), episode);
    auto *smart = new QCheckBox(&dialog); smart->setChecked(rule.value("smartFilter").toBool());
    form->addRow(tr("Smart episode filter"), smart);
    auto *days = new QSpinBox(&dialog); days->setRange(0, std::numeric_limits<int>::max());
    days->setValue(rule.value("ignoreDays").toInt());
    form->addRow(tr("Ignore subsequent matches for days"), days);
    auto *priority = new QSpinBox(&dialog); priority->setRange(std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
    priority->setValue(rule.value("priority").toInt());
    form->addRow(tr("Priority"), priority);
    auto *feeds = new QListWidget(&dialog);
    feeds->setObjectName(QStringLiteral("rssRuleFeeds"));
    const QStringList oldFeeds = feedUrls(rule.value("affectedFeeds"));
    QStringList seen;
    auto addFeed = [&](const QString &label, const QString &url) {
        if (seen.contains(url)) return;
        seen << url;
        auto *item = new QListWidgetItem(label, feeds);
        item->setData(Qt::UserRole, url);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(oldFeeds.contains(url) ? Qt::Checked : Qt::Unchecked);
    };
    for (auto it = m_feedData.cbegin(); it != m_feedData.cend(); ++it)
        addFeed(it.key(), stringValue(it.value(), "url"));
    int missing = 0;
    for (const QString &url : oldFeeds)
        if (!seen.contains(url)) addFeed(tr("Unavailable feed %1").arg(++missing), url);
    form->addRow(tr("Affected feeds"), feeds);
    auto *note = new QLabel(tr("New rules use server torrent defaults. Existing torrent options and match history are preserved."), &dialog);
    note->setWordWrap(true);
    form->addRow(note);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    if (regex->isChecked() && (!QRegularExpression(must->text()).isValid() || !QRegularExpression(exclude->text()).isValid()))
    { QMessageBox::warning(this, tr("Invalid rule"), tr("Check the regular expressions.")); return; }
    QJsonArray affected;
    for (int i = 0; i < feeds->count(); ++i)
        if (feeds->item(i)->checkState() == Qt::Checked) affected.append(feeds->item(i)->data(Qt::UserRole).toString());
    rule.insert("enabled", enabled->isChecked());
    rule.insert("useRegex", regex->isChecked());
    rule.insert("mustContain", must->text());
    rule.insert("mustNotContain", exclude->text());
    rule.insert("episodeFilter", episode->text());
    rule.insert("smartFilter", smart->isChecked());
    rule.insert("ignoreDays", days->value());
    rule.insert("priority", priority->value());
    rule.insert("affectedFeeds", affected);
    if (rule == baseline.toObject()) return;
    if (enabled->isChecked() && QMessageBox::question(this, tr("Enable RSS rule"),
        tr("This rule may automatically add matching torrents on the server. Save it?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    checkedRuleMutation(name, baseline, QStringLiteral("setRule"), {{"ruleName", name},
        {"ruleDef", QString::fromUtf8(QJsonDocument(rule).toJson(QJsonDocument::Compact))}});
}
void RemoteRssWidget::checkedRuleMutation(const QString &name, const QJsonValue &baseline,
        const QString &action, const QMap<QString, QString> &params, const QString &newName)
{
    if (!m_session || !m_session->isConnected()) { failed(); return; }
    setBusy(true);
    const QPointer<RemoteRssWidget> self(this);
    m_session->requestRss(QStringLiteral("rules"), {},
        [self, name, baseline, action, params, newName](const QJsonObject &fresh) {
            if (!self) return;
            if (fresh.value(name) != baseline || (!newName.isEmpty() && fresh.contains(newName)))
            {
                self->m_rules = fresh;
                self->showRules();
                self->m_status->setText(tr("Rules changed on the server. Review the updated list and retry."));
                self->setBusy(false);
                return;
            }
            self->mutate(action, params);
        }, [self](const QString &) { if (self) self->failed(); });
}
void RemoteRssWidget::renameRule()
{
    const QString old = selectedRule();
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Rename RSS rule"), tr("New name:"), QLineEdit::Normal, old, &ok).trimmed();
    if (!ok || name.isEmpty() || name == old) return;
    if (m_rules.contains(name)) { QMessageBox::warning(this, tr("RSS rule"), tr("That name already exists.")); return; }
    checkedRuleMutation(old, m_rules.value(old), QStringLiteral("renameRule"), {{"ruleName", old}, {"newRuleName", name}}, name);
}
void RemoteRssWidget::removeRule()
{
    const QString name = selectedRule();
    if (QMessageBox::question(this, tr("Remove RSS rule"), tr("Remove this automatic downloading rule from the server?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
        checkedRuleMutation(name, m_rules.value(name), QStringLiteral("removeRule"), {{"ruleName", name}});
}
void RemoteRssWidget::matchingArticles()
{
    const QString name = selectedRule();
    if (!m_session || !m_session->isConnected()) return;
    setBusy(true);
    const QPointer<RemoteRssWidget> self(this);
    m_session->requestRss(QStringLiteral("matchingArticles"), {{"ruleName", name}},
        [self](const QJsonObject &matches) {
            if (!self) return;
            QStringList lines;
            for (auto it = matches.begin(); it != matches.end(); ++it)
            {
                lines << it.key();
                for (const auto &title : it.value().toArray()) lines << QStringLiteral("  ") + title.toString();
            }
            self->m_ruleSummary->setPlainText(lines.isEmpty() ? tr("No matching articles.") : lines.join(QLatin1Char('\n')));
            self->setBusy(false);
        }, [self](const QString &) { if (self) self->failed(); });
}
