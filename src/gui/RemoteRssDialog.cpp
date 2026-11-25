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
#include <QApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QHeaderView>
#include <QLocale>
#include <QMenu>
#include <QScrollBar>
#include <QSettings>
#include <QSet>
#include <QStyle>
#include <QTabBar>
#include <QTextBrowser>
#include <QTextBlock>
#include <QTextFragment>
#include <QTextCursor>
#include <QTextImageFormat>
#include <QImageReader>
#include <QBuffer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTreeWidgetItemIterator>
#include <limits>

namespace
{
constexpr int PathRole = Qt::UserRole;
constexpr int ArticleRole = Qt::UserRole + 1;
constexpr int DateRole = Qt::UserRole + 2;

// The document itself never retrieves resources. Optional raster images are
// fetched explicitly, with a separate unauthenticated desktop network manager.
class ArticleBrowser final : public QTextBrowser
{
public:
    explicit ArticleBrowser(QWidget *parent) : QTextBrowser(parent), m_network(this) {}

    void render(const QString &html, const QUrl &base, bool loadImages)
    {
        ++m_generation;
        const auto pending = m_pending;
        m_pending.clear();
        for (auto *reply : pending) reply->abort();

        setUpdatesEnabled(false);
        setHtml(html);
        struct Embedded { int position; int length; QTextImageFormat format; };
        QList<Embedded> images;
        for (QTextBlock block = document()->begin(); block.isValid(); block = block.next())
            for (auto it = block.begin(); !it.atEnd(); ++it)
            {
                const auto fragment = it.fragment();
                if (fragment.isValid() && fragment.charFormat().isImageFormat())
                    images.append({fragment.position(), fragment.length(), fragment.charFormat().toImageFormat()});
            }

        // Walk backwards so removing objects never shifts an earlier cursor.
        // Even malformed HTML is handled by Qt's parser, not by a tag regex.
        int count = 0;
        for (auto it = images.crbegin(); it != images.crend(); ++it)
        {
            QTextCursor cursor(document());
            cursor.setPosition(it->position);
            cursor.setPosition(it->position + it->length, QTextCursor::KeepAnchor);
            QUrl url(it->format.name(), QUrl::StrictMode);
            if (url.isRelative() && base.isValid()) url = base.resolved(url);
            const bool allowed = loadImages && url.isValid() && !url.host().isEmpty()
                && url.userInfo().isEmpty()
                && (url.scheme() == QLatin1String("https") || url.scheme() == QLatin1String("http"))
                && count < 16;
            if (!allowed)
            {
                cursor.removeSelectedText();
                continue;
            }
            // Never retain the feed's arbitrary image dimensions or source.
            // A tiny transparent placeholder expands only after a valid image
            // is received. Synthetic names isolate old document resources.
            const QString key = QStringLiteral("rss-image:%1/%2").arg(m_generation).arg(++count);
            QImage blank(1, 1, QImage::Format_ARGB32);
            blank.fill(Qt::transparent);
            document()->addResource(QTextDocument::ImageResource, QUrl(key), blank);
            auto format = it->format;
            format.setName(key);
            format.setWidth(1);
            format.setHeight(1);
            cursor.setCharFormat(format);
            fetchImage(url, key);
        }
        setUpdatesEnabled(true);
        viewport()->update();
    }

protected:
    QVariant loadResource(int type, const QUrl &) override
    {
        // Qt may ask for images, stylesheets, local files or other resources.
        // No implicit request is ever permitted, even when images are enabled.
        if (type == QTextDocument::ImageResource)
        {
            QImage blank(1, 1, QImage::Format_ARGB32);
            blank.fill(Qt::transparent);
            return blank;
        }
        return {};
    }

private:
    void fetchImage(const QUrl &url, const QString &key)
    {
        QNetworkRequest request(url);
        request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
        // Never forward a request to a different origin or insecure redirect.
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        request.setTransferTimeout(10000);
        auto *reply = m_network.get(request);
        m_pending.insert(reply);
        const auto generation = m_generation;
        constexpr qint64 limit = 4 * 1024 * 1024;
        connect(reply, &QNetworkReply::readyRead, this, [reply] {
            if (reply->bytesAvailable() > limit) reply->abort();
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply, key, generation] {
            m_pending.remove(reply);
            const auto cleanup = [reply] { reply->deleteLater(); };
            if (generation != m_generation || reply->error() != QNetworkReply::NoError
                || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200
                || reply->bytesAvailable() > limit)
            { cleanup(); return; }
            QByteArray data = reply->readAll();
            cleanup();
            QBuffer buffer(&data);
            buffer.open(QIODevice::ReadOnly);
            QImageReader reader(&buffer);
            const auto format = reader.format().toLower();
            if (!QList<QByteArray>{"png", "jpeg", "jpg", "gif", "webp", "bmp"}.contains(format)) return;
            const QSize size = reader.size();
            if (!size.isValid() || qint64(size.width()) * size.height() > 20000000) return;
            const QSize scaled = size.scaled(QSize(800, 600), Qt::KeepAspectRatio);
            reader.setScaledSize(size.width() > 800 || size.height() > 600 ? scaled : size);
            const QImage image = reader.read();
            if (image.isNull()) return;
            document()->addResource(QTextDocument::ImageResource, QUrl(key), image);
            // Locate the still-current object by its synthetic identifier.
            for (QTextBlock block = document()->begin(); block.isValid(); block = block.next())
                for (auto it = block.begin(); !it.atEnd(); ++it)
                {
                    const auto fragment = it.fragment();
                    if (!fragment.isValid() || !fragment.charFormat().isImageFormat()) continue;
                    auto imageFormat = fragment.charFormat().toImageFormat();
                    if (imageFormat.name() != key) continue;
                    imageFormat.setWidth(image.width());
                    imageFormat.setHeight(image.height());
                    QTextCursor cursor(document());
                    cursor.setPosition(fragment.position());
                    cursor.setPosition(fragment.position() + fragment.length(), QTextCursor::KeepAnchor);
                    cursor.setCharFormat(imageFormat);
                    document()->markContentsDirty(fragment.position(), fragment.length());
                    return;
                }
        });
    }

    QNetworkAccessManager m_network;
    QSet<QNetworkReply *> m_pending;
    quint64 m_generation = 0;
};

class ArticleItem final : public QTreeWidgetItem
{
public:
    using QTreeWidgetItem::QTreeWidgetItem;
    bool operator<(const QTreeWidgetItem &other) const override
    {
        if (treeWidget() && treeWidget()->sortColumn() == 1)
            return data(1, DateRole).toDateTime() < other.data(1, DateRole).toDateTime();
        return QTreeWidgetItem::operator<(other);
    }
};
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
    : QWidget(parent), m_session(session), m_rulesOnly(rulesFirst)
{
    setObjectName(QStringLiteral("remoteRssWidget"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    if (rulesFirst)
    {
        auto *note = new QLabel(tr("Rule changes are applied immediately to this server."), this);
        note->setWordWrap(true);
        layout->addWidget(note);
    }
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
    m_refresh = button(tools, tr("Refresh"), "rssRefreshItem", [this] { refreshItem(); });
    tools->addStretch();
    m_downloader = button(tools, tr("RSS Downloader..."), "rssDownloader", [this] {
        if (!m_session || !m_session->isConnected()) return;
        RemoteRssDialog dialog(m_session, window()->windowTitle(), this, true);
        dialog.exec();
        if (m_session && m_session->isConnected()) reload();
    });
    auto *split = new QSplitter(reader);
    m_split = split;
    m_feeds = new QTreeWidget(split);
    m_feeds->setObjectName(QStringLiteral("rssFeeds"));
    m_feeds->setHeaderLabels({tr("Subscriptions"), tr("Status")});
    m_feeds->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_feeds->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_feeds->setMinimumWidth(240);
    m_feeds->setAlternatingRowColors(true);
    auto *right = new QSplitter(Qt::Vertical, split);
    m_right = right;
    m_articles = new QTreeWidget(right);
    m_articles->setObjectName(QStringLiteral("rssArticles"));
    m_articles->setHeaderLabels({tr("Article"), tr("Date"), tr("Feed")});
    m_articles->setRootIsDecorated(false);
    m_articles->setAlternatingRowColors(true);
    m_articles->header()->setStretchLastSection(false);
    m_articles->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_articles->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_articles->header()->setSectionResizeMode(2, QHeaderView::Interactive);
    m_articles->setColumnWidth(1, 165);
    m_articles->setColumnWidth(2, 150);
    m_articles->setSortingEnabled(true);
    m_articles->sortItems(1, Qt::DescendingOrder);
    m_article = new ArticleBrowser(right);
    m_article->setObjectName(QStringLiteral("rssArticlePreview"));
    m_article->setReadOnly(true);
    m_article->setOpenLinks(false);
    m_article->setOpenExternalLinks(false);
    connect(m_article, &QTextBrowser::anchorClicked, this, [](const QUrl &url) {
        if (validUrl(url.toString(QUrl::FullyEncoded)))
            QDesktopServices::openUrl(url);
    });
    split->setStretchFactor(1, 1);
    right->setStretchFactor(0, 2);
    split->setSizes({300, 850});
    right->setSizes({360, 240});
    readerLayout->addWidget(split, 1);
    auto *articleTools = new QHBoxLayout;
    readerLayout->addLayout(articleTools);
    m_markItem = button(articleTools, tr("Mark feed(s) read"), "rssMarkItem", [this] { markRead(false); });
    m_markArticle = button(articleTools, tr("Mark article read"), "rssMarkArticle", [this] { markRead(true); });
    m_download = button(articleTools, tr("Download torrent..."), "rssDownload", [this] { downloadArticle(); });
    articleTools->addStretch();
    connect(m_feeds, &QTreeWidget::currentItemChanged, this, [this] { showArticles(); });
    connect(m_articles, &QTreeWidget::currentItemChanged, this, [this] {
        showArticle();
        const auto a = selectedArticle();
        if (!m_busy && m_loaded && !a.value("isRead").toBool()
            && !stringValue(a, "id").isEmpty() && m_session && m_session->isConnected())
            markRead(true);
    });
    m_feeds->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_feeds, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &point) {
        if (auto *item = m_feeds->itemAt(point)) m_feeds->setCurrentItem(item);
        QMenu menu(this);
        for (auto *b : {m_addFeed, m_addFolder, m_editFeed, m_move, m_remove, m_refresh, m_markItem})
        {
            auto *action = menu.addAction(b->text());
            action->setEnabled(b->isEnabled());
            connect(action, &QAction::triggered, b, &QPushButton::click);
        }
        menu.exec(m_feeds->viewport()->mapToGlobal(point));
    });
    m_articles->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_articles, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &point) {
        if (!m_articles->itemAt(point)) return;
        m_articles->setCurrentItem(m_articles->itemAt(point));
        QMenu menu(this);
        for (auto *b : {m_markArticle, m_download})
        {
            auto *action = menu.addAction(b->text());
            action->setEnabled(b->isEnabled());
            connect(action, &QAction::triggered, b, &QPushButton::click);
        }
        menu.exec(m_articles->viewport()->mapToGlobal(point));
    });
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
    m_tabs->tabBar()->hide();
    auto *footer = new QHBoxLayout;
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("rssStatus"));
    m_status->setWordWrap(true);
    footer->addWidget(m_status, 1);
    m_reload = button(footer, tr("Reload"), "rssReload", [this] { reload(); });
    layout->addLayout(footer);
    auto *timer = new QTimer(this);
    timer->setInterval(5000);
    connect(timer, &QTimer::timeout, this, [this] {
        if (!m_rulesOnly && isVisible() && !m_busy && !QApplication::activeModalWidget()
            && m_session && m_session->isConnected())
            reload();
    });
    timer->start();
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
        if (!isVisible()) return;
        showArticle();
        if (!m_busy && !m_loaded && m_session && m_session->isConnected())
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
    if (!m_loaded) m_status->setText(tr("Reading server RSS data..."));
    const QPointer<RemoteRssWidget> self(this);
    m_session->requestRss(QStringLiteral("items"), {{"withData", "true"}},
        [self](const QJsonObject &items) {
            if (!self) return;
            const bool changed = !self->m_loaded || self->m_items != items;
            self->m_items = items;
            self->m_loaded = true;
            if (changed) self->showItems();
            if (!self->m_rulesOnly)
            {
                self->m_status->clear();
                self->setBusy(false);
                return;
            }
            if (!self->m_session) { self->failed(); return; }
            self->m_session->requestRss(QStringLiteral("rules"), {},
                [self](const QJsonObject &rules) {
                    if (!self) return;
                    self->m_rules = rules;
                    self->m_rulesLoaded = true;
                    self->showRules();
                    self->m_status->clear();
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
    const int scroll = m_feeds->verticalScrollBar()->value();
    QSet<QString> known, expanded;
    for (QTreeWidgetItemIterator it(m_feeds); *it; ++it)
    {
        const QString key = (*it)->data(0, PathRole).toString();
        known.insert(key);
        if ((*it)->isExpanded()) expanded.insert(key);
    }
    const QSignalBlocker blocker(m_feeds);
    m_feeds->clear();
    m_feedData.clear();
    auto *root = new QTreeWidgetItem(m_feeds, {tr("All feeds")});
    root->setData(0, PathRole, QString());
    root->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
    QTreeWidgetItem *restore = root;
    std::function<int(const QJsonObject &, QTreeWidgetItem *, const QString &, int)> visit;
    visit = [&](const QJsonObject &object, QTreeWidgetItem *parent, const QString &prefix, int depth) {
        if (depth > 64) return 0;
        int total = 0;
        for (auto it = object.begin(); it != object.end(); ++it)
        {
            if (!it.value().isObject()) continue;
            const QJsonObject value = it.value().toObject();
            const QString path = joinPath(prefix, it.key());
            auto *item = new QTreeWidgetItem(parent, {it.key()});
            item->setData(0, PathRole, path);
            if (path == selected) restore = item;
            int unread = 0;
            if (value.value("uid").isString() && value.value("url").isString())
            {
                m_feedData.insert(path, value);
                const bool error = value.value("hasError").toBool();
                const bool loading = value.value("isLoading").toBool();
                item->setIcon(0, style()->standardIcon(error ? QStyle::SP_MessageBoxWarning
                    : loading ? QStyle::SP_BrowserReload : QStyle::SP_FileIcon));
                item->setText(1, error ? tr("Error") : loading ? tr("Loading") : QString());
                for (const auto &article : value.value("articles").toArray())
                    if (article.isObject() && !article.toObject().value("isRead").toBool()) ++unread;
            }
            else
            {
                item->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
                unread = visit(value, item, path, depth + 1);
                item->setExpanded(!known.contains(path) || expanded.contains(path));
            }
            if (unread) item->setText(0, tr("%1 (%2)").arg(it.key()).arg(unread));
            total += unread;
        }
        return total;
    };
    const int unread = visit(m_items, root, {}, 0);
    if (unread) root->setText(0, tr("All feeds (%1)").arg(unread));
    root->setExpanded(!known.contains(QString()) || expanded.contains(QString()));
    m_feeds->setCurrentItem(restore);
    m_feeds->verticalScrollBar()->setValue(scroll);
    showArticles();
}
void RemoteRssWidget::showArticles()
{
    const auto previous = selectedArticle();
    const QString oldPath = m_articles->currentItem()
        ? m_articles->currentItem()->data(0, PathRole).toString() : QString();
    const int scroll = m_articles->verticalScrollBar()->value();
    const QSignalBlocker blocker(m_articles);
    const int sortColumn = m_articles->sortColumn();
    const auto sortOrder = m_articles->header()->sortIndicatorOrder();
    m_articles->setSortingEnabled(false);
    m_articles->clear();
    const QString path = selectedPath();
    for (auto it = m_feedData.cbegin(); it != m_feedData.cend(); ++it)
    {
        if (!path.isEmpty() && it.key() != path && !it.key().startsWith(path + sep)) continue;
        for (const auto &entry : it.value().value("articles").toArray())
        {
            if (!entry.isObject()) continue;
            const auto article = entry.toObject();
            const auto date = QDateTime::fromString(stringValue(article, "date"), Qt::RFC2822Date);
            auto *row = new ArticleItem(m_articles, {stringValue(article, "title"),
                date.isValid() ? QLocale().toString(date.toLocalTime().date(), QLocale::ShortFormat)
                               : stringValue(article, "date"), it.key()});
            row->setData(1, DateRole, date);
            row->setToolTip(0, stringValue(article, "title"));
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
    m_articles->setSortingEnabled(true);
    m_articles->sortItems(sortColumn, sortOrder);
    m_articles->verticalScrollBar()->setValue(scroll);
    showArticle();
}
void RemoteRssWidget::setLoadExternalImages(bool enabled)
{
    if (m_loadExternalImages == enabled) return;
    m_loadExternalImages = enabled;
    m_displayedArticle = {};
    // Disabling immediately cancels requests and strips rendered images;
    // enabling only fetches images in the currently visible article.
    if (!enabled || isVisible()) showArticle();
}

void RemoteRssWidget::showArticle()
{
    const auto a = selectedArticle();
    // Avoid resetting a reading position when only the read flag changes.
    QJsonObject visible = a;
    visible.remove("isRead");
    if (visible != m_displayedArticle)
    {
        m_displayedArticle = visible;
        if (a.isEmpty()) static_cast<ArticleBrowser *>(m_article)->render({}, {}, false);
        else
        {
            const QString link = stringValue(a, "link");
            QString html = QStringLiteral("<h3>%1</h3><p>%2</p>")
                .arg(stringValue(a, "title").toHtmlEscaped(), stringValue(a, "date").toHtmlEscaped());
            if (validUrl(link))
                html += QStringLiteral("<p><a href=\"%1\">%2</a></p>")
                    .arg(link.toHtmlEscaped(), tr("Open original article").toHtmlEscaped());
            html += stringValue(a, "description");
            static_cast<ArticleBrowser *>(m_article)->render(html, validUrl(link) ? QUrl(link) : QUrl(),
                m_loadExternalImages && isVisible());
        }
    }
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
    m_downloader->setEnabled(online && m_loaded);
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
    auto *path = new QLineEdit(&dialog);
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
    const QString name = path->text().trimmed();
    const QString dest = joinPath(prefix, name);
    if (name.isEmpty() || name.contains(sep) || !validPath(dest) || (!folder && !validUrl(url->text().trimmed())))
    {
        QMessageBox::warning(this, tr("Invalid subscription"), tr("Enter a name without backslashes and, for a feed, a valid HTTP or HTTPS URL."));
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

void RemoteRssWidget::saveLayout(QSettings &settings, const QString &prefix) const
{
    settings.setValue(prefix + "/rssHorizontal", m_split->saveState());
    settings.setValue(prefix + "/rssVertical", m_right->saveState());
    settings.setValue(prefix + "/rssArticleHeader", m_articles->header()->saveState());
}
void RemoteRssWidget::restoreLayout(QSettings &settings, const QString &prefix)
{
    const auto horizontal = settings.value(prefix + "/rssHorizontal").toByteArray();
    const auto vertical = settings.value(prefix + "/rssVertical").toByteArray();
    const auto header = settings.value(prefix + "/rssArticleHeader").toByteArray();
    if (!horizontal.isEmpty()) m_split->restoreState(horizontal);
    if (!vertical.isEmpty()) m_right->restoreState(vertical);
    if (!header.isEmpty())
    {
        m_articles->header()->restoreState(header);
        m_articles->sortItems(m_articles->header()->sortIndicatorSection(),
                             m_articles->header()->sortIndicatorOrder());
    }
}
