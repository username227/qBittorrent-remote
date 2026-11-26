// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/RemoteFileDialog.h"
#include "gui/BytePresentation.h"

#include <QCompleter>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStringListModel>
#include <QStyle>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace
{
bool windowsPath(const QString &p)
{
    static const QRegularExpression drive(QStringLiteral("^[A-Za-z]:[/\\\\]"));
    return drive.match(p).hasMatch() || p.startsWith(QStringLiteral("//")) || p.startsWith(QStringLiteral("\\\\"));
}
QString normalized(QString path)
{
    if (windowsPath(path)) path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    // Keep drive and UNC roots intact. Never use the desktop filesystem
    // to canonicalize, resolve symlinks or interpret remote names.
    while (path.size() > 1 && path.endsWith('/') && !(path.size() == 3 && path.at(1) == ':'))
        path.chop(1);
    return path;
}
QString rootOf(const QString &p)
{
    if (p.startsWith("//"))
    {
        const auto parts = p.mid(2).split('/', Qt::SkipEmptyParts);
        return parts.size() >= 2 ? QStringLiteral("//") + parts[0] + '/' + parts[1] : QString();
    }
    if (p.size() >= 3 && p.at(1) == ':' && p.at(2) == '/') return p.left(3);
    return p.startsWith('/') ? QStringLiteral("/") : QString();
}
QString joinPath(const QString &parent, const QString &name)
{
    return parent + (parent.endsWith('/') ? QString() : QStringLiteral("/")) + name;
}
QString parentOf(const QString &p)
{
    const QString root = rootOf(p);
    if (root.isEmpty() || p == root) return {};
    const int slash = p.lastIndexOf('/');
    if (slash < root.size()) return root;
    return p.left(slash);
}
bool samePath(const QString &a, const QString &b)
{
    return a.compare(b, windowsPath(a) ? Qt::CaseInsensitive : Qt::CaseSensitive) == 0;
}
}

bool RemoteFileDialog::isAbsoluteServerPath(const QString &path)
{
    if (path.contains(QChar::Null) || path.contains('\r') || path.contains('\n')) return false;
    const QString p = normalized(path);
    const QString root = rootOf(p);
    if (root.isEmpty()) return false;
    if (p.size() >= 2 && p.at(1) == ':' && !p.at(0).isLetter()) return false;
    if (!windowsPath(p) && p.contains('\\')) return false;
    for (const auto &part : p.split('/', Qt::SkipEmptyParts))
        if (part == "." || part == "..") return false;
    return true;
}

QString RemoteFileDialog::parentDirectory(const QString &path)
{
    if (!isAbsoluteServerPath(path)) return {};
    const QString p = normalized(path);
    const QString parent = parentOf(p);
    return parent.isEmpty() ? p : parent;
}

RemoteFileDialog::RemoteFileDialog(Remote::RemoteSession *session, const QString &serverName,
    const QString &profileId, QSettings &settings, const QString &initial, Mode mode, QWidget *parent)
    : QDialog(parent), m_session(session), m_settings(settings),
      m_historyKey(QStringLiteral("Layout/RemoteFileBrowser/") + QString::fromLatin1(
          QCryptographicHash::hash(profileId.toUtf8(), QCryptographicHash::Sha256).toHex())),
      m_mode(mode)
{
    setObjectName("remoteFileDialog");
    setWindowTitle((mode == Mode::Directory ? tr("Select remote directory — %1") : tr("Select remote file — %1")).arg(serverName));
    resize(650, 440);
    auto *layout = new QVBoxLayout(this);
    auto *bar = new QHBoxLayout;
    bar->addWidget(new QLabel(tr("Look in:"), this));
    m_path = new QLineEdit(this);
    m_path->setObjectName("remoteBrowserPath");
    m_completions = new QStringListModel(this);
    auto *completer = new QCompleter(m_completions, m_path);
    completer->setCompletionMode(QCompleter::PopupCompletion);
    completer->setCaseSensitivity(Qt::CaseSensitive);
    m_path->setCompleter(completer);
    bar->addWidget(m_path, 1);
    auto *go = new QPushButton(tr("Go"), this);
    go->setAutoDefault(false);
    bar->addWidget(go);
    m_up = new QPushButton(style()->standardIcon(QStyle::SP_FileDialogToParent), QString(), this);
    m_up->setObjectName("remoteBrowserUp");
    m_up->setToolTip(tr("Parent directory"));
    m_up->setAutoDefault(false);
    bar->addWidget(m_up);
    auto *refresh = new QPushButton(style()->standardIcon(QStyle::SP_BrowserReload), QString(), this);
    refresh->setToolTip(tr("Refresh"));
    refresh->setAutoDefault(false);
    bar->addWidget(refresh);
    layout->addLayout(bar);
    m_entries = new QTreeWidget(this);
    m_entries->setObjectName("remoteBrowserEntries");
    m_entries->setHeaderLabels({tr("Name"), tr("Size"), tr("Date modified")});
    m_entries->setRootIsDecorated(false);
    m_entries->setUniformRowHeights(true);
    m_entries->setAlternatingRowColors(true);
    m_entries->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_entries->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_entries->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    layout->addWidget(m_entries, 1);
    m_status = new QLabel(this);
    m_status->setObjectName("remoteBrowserStatus");
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    m_space = new QLabel(this);
    m_space->setObjectName("remoteBrowserFreeSpace");
    layout->addWidget(m_space);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_ok = buttons->button(QDialogButtonBox::Ok);
    m_ok->setText(mode == Mode::Directory ? tr("Select folder") : tr("Select file"));
    m_ok->setEnabled(false);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &RemoteFileDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(go, &QPushButton::clicked, this, [this] { navigate(m_path->text()); });
    connect(m_path, &QLineEdit::returnPressed, this, [this] { navigate(m_path->text()); });
    connect(m_path, &QLineEdit::textEdited, this, [this] { updateSelection(); });
    connect(m_up, &QPushButton::clicked, this, [this] { navigate(parentOf(m_directory)); });
    connect(refresh, &QPushButton::clicked, this, [this] { navigate(m_path->text()); });
    connect(m_entries, &QTreeWidget::itemSelectionChanged, this, &RemoteFileDialog::updateSelection);
    connect(m_entries, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        if (item->data(0, Qt::UserRole+1).toBool()) navigate(item->data(0, Qt::UserRole).toString());
        else if (m_mode == Mode::File) accept();
    });
    if (session)
    {
        connect(session, &Remote::RemoteSession::disconnected, this, &RemoteFileDialog::disconnected);
        connect(session, &Remote::RemoteSession::failed, this, [this] { disconnected(); });
        connect(session, &QObject::destroyed, this, &RemoteFileDialog::disconnected);
    }
    QString start = initial;
    if (!isAbsoluteServerPath(start)) start = settings.value(m_historyKey).toString();
    if (!isAbsoluteServerPath(start)) start = QStringLiteral("/");
    QTimer::singleShot(0, this, [this, start] { navigate(start); });
}

void RemoteFileDialog::disconnected()
{
    ++m_generation;
    m_loading = m_requestPending = m_valid = false;
    m_nextDirectory.clear();
    m_entries->clear();
    m_space->clear();
    m_status->setText(tr("The selected server is no longer connected."));
    updateSelection();
}

void RemoteFileDialog::navigate(const QString &path)
{
    if (!m_session || !m_session->isConnected()) { disconnected(); return; }
    if (!isAbsoluteServerPath(path))
    {
        ++m_generation;
        m_nextDirectory.clear();
        m_loading = m_valid = false;
        m_entries->clear();
        m_space->clear();
        m_completions->setStringList({});
        m_status->setText(tr("Enter an absolute server directory path without dot segments."));
        updateSelection();
        return;
    }
    const QString target = normalized(path);
    ++m_generation;
    m_directory = target;
    m_path->setText(target);
    m_entries->clear();
    m_completions->setStringList({});
    m_space->clear();
    m_loading = true;
    m_valid = false;
    m_status->setText(tr("Reading remote directory…"));
    updateSelection();
    if (m_requestPending) { m_nextDirectory = target; return; }
    readEntries(target, true, m_generation);
}

void RemoteFileDialog::readEntries(const QString &path, bool directories, quint64 generation)
{
    if (!m_session || !m_session->isConnected()) { disconnected(); return; }
    m_requestPending = true;
    const QPointer<RemoteFileDialog> self(this);
    const auto advance = [self, generation]() {
        if (!self) return false;
        self->m_requestPending = false;
        if (generation == self->m_generation) return true;
        const QString next = self->m_nextDirectory;
        self->m_nextDirectory.clear();
        if (!next.isEmpty()) self->readEntries(next, true, self->m_generation);
        return false;
    };
    m_session->requestDirectoryContent(path, directories, [self, path, directories, generation, advance](const QJsonArray &entries) {
        if (!advance()) return;
        self->addEntries(entries, path, directories);
        if (directories && self->m_mode == Mode::File)
            self->readEntries(path, false, generation);
        else
            self->finishListing();
    }, [self, advance](const QString &error) {
        if (!advance()) return;
        self->m_loading = false;
        self->m_valid = false;
        self->m_entries->clear();
        self->m_status->setText(error);
        self->updateSelection();
    });
}

void RemoteFileDialog::addEntries(const QJsonArray &entries, const QString &path, bool directories)
{
    QStringList completions = m_completions->stringList();
    for (const auto &v : entries)
    {
        QString full, name;
        const auto metadata = v.toObject();
        if (v.isString())
        {
            full = normalized(v.toString());
            name = full.mid(full.lastIndexOf('/')+1);
            if (!samePath(parentOf(full), path)) continue;
        }
        else if (v.isObject())
        {
            name = metadata.value("name").toString();
            if (metadata.value("type").toString() != (directories ? "dir" : "file")) continue;
            full = joinPath(path, name);
        }
        else continue;
        if (name.isEmpty() || name == "." || name == ".." || name.contains('/')
            || (windowsPath(path) && name.contains('\\'))
            || !isAbsoluteServerPath(full) || !samePath(parentOf(full), path)) continue;
        auto *item = new QTreeWidgetItem(m_entries);
        item->setText(0, name);
        item->setData(0, Qt::UserRole, full);
        item->setData(0, Qt::UserRole+1, directories);
        item->setIcon(0, style()->standardIcon(directories ? QStyle::SP_DirIcon : QStyle::SP_FileIcon));
        if (!directories && metadata.value("size").isDouble())
            item->setText(1, BytePresentation::dataSize(metadata.value("size").toInteger()));
        if (metadata.value("last_modification_date").isDouble())
        {
            const auto seconds = metadata.value("last_modification_date").toInteger();
            if (seconds > 0) item->setText(2, QLocale().toString(QDateTime::fromSecsSinceEpoch(seconds), QLocale::ShortFormat));
        }
        if (directories) completions.append(full);
    }
    m_completions->setStringList(completions);
}

void RemoteFileDialog::finishListing()
{
    m_entries->sortItems(0, Qt::AscendingOrder);
    m_loading = false;
    m_valid = true;
    m_status->setText(tr("%1 entries").arg(m_entries->topLevelItemCount()));
    updateSelection();
    if (!m_session || !m_session->supportsPathFreeSpace()) return;
    const auto generation = m_generation;
    const QPointer<RemoteFileDialog> self(this);
    m_session->requestPathFreeSpace(m_directory, [self, generation](qint64 bytes) {
        if (self && generation == self->m_generation && bytes >= 0)
            self->m_space->setText(self->tr("Free space: %1").arg(BytePresentation::dataSize(bytes)));
    }, {});
}

void RemoteFileDialog::updateSelection()
{
    const bool ready = m_valid && !m_loading && m_session && m_session->isConnected()
        && samePath(normalized(m_path->text()), m_directory);
    const auto *item = m_entries->currentItem();
    const bool selected = item && item->isSelected();
    const bool correctType = selected && item->data(0, Qt::UserRole+1).toBool() == (m_mode == Mode::Directory);
    m_ok->setEnabled(ready && (m_mode == Mode::Directory || correctType));
    m_up->setEnabled(!m_directory.isEmpty() && !parentOf(m_directory).isEmpty() && m_session && m_session->isConnected());
}

void RemoteFileDialog::accept()
{
    if (!m_ok->isEnabled()) return;
    const auto *item = m_entries->currentItem();
    m_selected = item && item->isSelected() ? item->data(0, Qt::UserRole).toString() : m_directory;
    m_settings.setValue(m_historyKey, m_directory);
    QDialog::accept();
}
