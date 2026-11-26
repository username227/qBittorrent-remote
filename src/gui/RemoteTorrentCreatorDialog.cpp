// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/RemoteTorrentCreatorDialog.h"
#include "api/TorrentMetadataReader.h"
#include "ui_remotetorrentcreatordialog.h"

#include <QCryptographicHash>
#include <QVariantMap>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QSaveFile>
#include <QUrl>
#include <QVersionNumber>
#include <QHBoxLayout>
#include <QStandardPaths>
#include <limits>

namespace
{
QString serverPath(QString path)
{
    path = path.trimmed();
    if (path.size() >= 2 && path.at(1) == QLatin1Char(':'))
        path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return path.isEmpty() ? QString() : QDir::cleanPath(path);
}

bool absoluteServerPath(const QString &path)
{
    return !path.contains(QChar::Null) && (path.startsWith(QLatin1Char('/'))
        || (path.size() >= 3 && path.at(0).isLetter() && path.at(1) == QLatin1Char(':')
            && path.at(2) == QLatin1Char('/')));
}

QString serverParent(const QString &path)
{
    const qsizetype slash = path.lastIndexOf(QLatin1Char('/'));
    if (slash == 0 || (slash == 2 && path.at(1) == QLatin1Char(':')))
        return path.left(slash + 1);
    return slash > 0 ? path.left(slash) : QString();
}
}

RemoteTorrentCreatorDialog::RemoteTorrentCreatorDialog(QSettings &settings,
    const Remote::ServerProfile &profile, Remote::RemoteSession *session,
    const QString &version, QWidget *parent, const QList<SeedTarget> &seedTargets)
    : QDialog(parent), m_ui(std::make_unique<Ui::RemoteTorrentCreatorDialog>()),
      m_session(session), m_settings(settings),
      m_settingsKey(profile.id.isEmpty() ? QString() : QStringLiteral("TorrentCreator/Servers/")
          + QString::fromLatin1(QCryptographicHash::hash(profile.id.toUtf8(), QCryptographicHash::Sha256).toHex())),
      m_mappings(Remote::PathMappingStore(settings).mappings(profile.id))
{
    m_ui->setupUi(this);
    m_seedTargets.append(SeedTarget{profile, session});
    for (const auto &target : seedTargets)
        if (target.session && target.session != session && target.session->isConnected())
            m_seedTargets.append(target);
    for (qsizetype i = 0; i < m_seedTargets.size(); ++i)
    {
        const auto &target = m_seedTargets.at(i);
        m_ui->seedServerCombo->addItem(target.profile.name);
        if (i == 0 || !target.session) continue;
        const auto lost = [this, i] {
            if (m_state == State::Seeding && m_seedTargetIndex == i)
                finishCrossSeed(tr("Torrent saved locally. Seeding on %1 could not be confirmed: server disconnected.")
                    .arg(m_seedTargets.at(i).profile.name));
        };
        connect(target.session, &Remote::RemoteSession::disconnected, this, lost);
        connect(target.session, &Remote::RemoteSession::failed, this, [lost](const QString &) { lost(); });
        connect(target.session, &QObject::destroyed, this, lost);
    }
    connect(m_ui->seedServerCombo, &QComboBox::currentIndexChanged, this, [this](int) { updateSeedTarget(true); });
    connect(m_ui->checkStartSeeding, &QCheckBox::toggled, this, [this](bool) { updateSeedTarget(); });
    connect(m_ui->textInputPath, &QLineEdit::textChanged, this, [this] { updateSeedTarget(); });
    connect(m_ui->seedTargetPath, &QLineEdit::textEdited, this, [this] { m_seedPathEdited = true; });
    updateSeedTarget(true);
    setObjectName(QStringLiteral("remoteTorrentCreatorDialog"));
    setWindowTitle(tr("Torrent Creator — %1").arg(profile.name));
    QString numeric = version;
    if (numeric.startsWith(QLatin1Char('v'))) numeric.remove(0, 1);
    m_modern = QVersionNumber::fromString(numeric) >= QVersionNumber(5, 3, 0);
    m_ui->checkIgnoreDotfiles->setVisible(m_modern);
    m_ui->checkIgnoreShareLimits->hide(); // no creation API parameter
    m_ui->buttonCalcTotalPieces->hide(); // no remote piece-count API
    m_ui->labelTotalPieces->hide();
    m_ui->textInputPath->setPlaceholderText(tr("Absolute path on the selected server"));
    m_ui->label_2->setText(tr("Drop a locally mapped file or folder"));
    m_ui->comboPieceSize->addItem(tr("Auto"), 0);
    for (int i = 4; i <= 17; ++i)
    {
        const int size = 1024 << i;
        m_ui->comboPieceSize->addItem(i < 10 ? tr("%1 KiB").arg(size / 1024)
            : tr("%1 MiB").arg(size / (1024 * 1024)), size);
    }
    m_ui->comboTorrentFormat->setItemData(0, QStringLiteral("v2"));
    m_ui->comboTorrentFormat->setItemData(1, QStringLiteral("hybrid"));
    m_ui->comboTorrentFormat->setItemData(2, QStringLiteral("v1"));
    m_ui->comboTorrentFormat->setCurrentIndex(1);
    m_ui->spinPaddedFileSizeLimit->setMaximum(std::numeric_limits<int>::max() / 1024);
    auto *outputRow = new QHBoxLayout;
    outputRow->addWidget(new QLabel(tr("Save torrent locally:"), this));
    m_output = new QLineEdit(this);
    m_output->setObjectName(QStringLiteral("creatorLocalOutput"));
    m_output->setPlaceholderText(tr("Choose a local .torrent filename"));
    outputRow->addWidget(m_output, 1);
    m_browseOutput = new QPushButton(tr("Browse…"), this);
    outputRow->addWidget(m_browseOutput);
    m_ui->verticalLayout->insertLayout(1, outputRow);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("creatorTaskStatus"));
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    m_ui->verticalLayout->insertWidget(2, m_status);
    connect(m_browseOutput, &QPushButton::clicked, this, &RemoteTorrentCreatorDialog::chooseOutput);
    connect(m_ui->addFileButton, &QPushButton::clicked, this, [this] { browseSource(false); });
    connect(m_ui->addFolderButton, &QPushButton::clicked, this, [this] { browseSource(true); });
    connect(m_ui->buttonBox, &QDialogButtonBox::accepted, this, &RemoteTorrentCreatorDialog::createOrRetry);
    connect(m_ui->buttonBox, &QDialogButtonBox::rejected, this, &RemoteTorrentCreatorDialog::reject);
    m_poll.setSingleShot(true);
    m_poll.setInterval(750);
    connect(&m_poll, &QTimer::timeout, this, &RemoteTorrentCreatorDialog::poll);
    setState(State::Loading, tr("Checking server torrent creation support…"));
    if (!session || !session->isConnected()) { disconnected(); return; }
    connect(session, &Remote::RemoteSession::disconnected, this, &RemoteTorrentCreatorDialog::disconnected);
    connect(session, &Remote::RemoteSession::failed, this, [this](const QString &) { disconnected(); });
    connect(session, &QObject::destroyed, this, &RemoteTorrentCreatorDialog::disconnected);
    const QPointer<RemoteTorrentCreatorDialog> self(this);
    session->requestBuildInfo([self](const QJsonObject &info) {
        if (!self || self->m_state != State::Loading || !self->m_session) return;
        const auto lt = QVersionNumber::fromString(info.value("libtorrent").toString());
        if (lt.segmentCount() < 2 || (lt.majorVersion() != 1 && lt.majorVersion() != 2))
        {
            self->setState(State::Disconnected, tr("Cannot establish the server's torrent format capabilities."));
            return;
        }
        self->m_lt2 = lt.majorVersion() == 2;
        self->m_ui->widgetTorrentFormat->setVisible(self->m_lt2);
        self->m_ui->checkOptimizeAlignment->setVisible(!self->m_lt2);
        self->m_session->requestTorrentCreationStatus({}, [self](const QJsonArray &) {
            if (self && self->m_state == State::Loading)
            {
                self->loadSettings();
                self->setState(State::Ready, tr("Files will be hashed by the selected server."));
            }
        }, [self](const QString &error) {
            if (self && self->m_state == State::Loading)
                self->setState(State::Disconnected, tr("Torrent creation API unavailable: %1").arg(error));
        });
    }, [self](const QString &error) {
        if (self && self->m_state == State::Loading)
            self->setState(State::Disconnected, tr("Cannot read server build information: %1").arg(error));
    });
}

RemoteTorrentCreatorDialog::~RemoteTorrentCreatorDialog()
{
    saveSettings();
}

void RemoteTorrentCreatorDialog::loadSettings()
{
    if (m_settingsKey.isEmpty()) return;
    const QVariantMap saved = m_settings.value(m_settingsKey).toMap();
    m_ui->trackersList->setPlainText(saved.value("Trackers").toString());
    m_ui->URLSeedsList->setPlainText(saved.value("WebSeeds").toString());
    m_ui->txtComment->setPlainText(saved.value("Comment").toString());
    m_ui->lineEditSource->setText(saved.value("Source").toString());
    const int piece = m_ui->comboPieceSize->findData(saved.value("PieceSize", 0));
    if (piece >= 0) m_ui->comboPieceSize->setCurrentIndex(piece);
    m_ui->checkPrivate->setChecked(saved.value("Private", m_ui->checkPrivate->isChecked()).toBool());
    m_ui->checkStartSeeding->setChecked(saved.value("StartSeeding", m_ui->checkStartSeeding->isChecked()).toBool());
    if (m_modern)
        m_ui->checkIgnoreDotfiles->setChecked(saved.value("IgnoreDotfiles", m_ui->checkIgnoreDotfiles->isChecked()).toBool());
    if (m_lt2)
    {
        const int format = m_ui->comboTorrentFormat->findData(saved.value("Format", QStringLiteral("hybrid")));
        if (format >= 0) m_ui->comboTorrentFormat->setCurrentIndex(format);
    }
    else
    {
        m_ui->checkOptimizeAlignment->setChecked(saved.value("OptimizeAlignment", m_ui->checkOptimizeAlignment->isChecked()).toBool());
        m_ui->spinPaddedFileSizeLimit->setValue(saved.value("PaddedFileSizeLimit", m_ui->spinPaddedFileSizeLimit->value()).toInt());
    }
    m_lastSaveDirectory = saved.value("LastSaveDirectory").toString();
    m_settingsLoaded = true;
}

void RemoteTorrentCreatorDialog::saveSettings()
{
    if (!m_settingsLoaded) return;
    // Keep values for capabilities temporarily unavailable on this server.
    // Tracker URLs may include private passkeys; keep them local to this profile.
    QVariantMap saved = m_settings.value(m_settingsKey).toMap();
    saved.insert("Trackers", m_ui->trackersList->toPlainText());
    saved.insert("WebSeeds", m_ui->URLSeedsList->toPlainText());
    saved.insert("Comment", m_ui->txtComment->toPlainText());
    saved.insert("Source", m_ui->lineEditSource->text());
    saved.insert("PieceSize", m_ui->comboPieceSize->currentData());
    saved.insert("Private", m_ui->checkPrivate->isChecked());
    saved.insert("StartSeeding", m_ui->checkStartSeeding->isChecked());
    if (m_modern) saved.insert("IgnoreDotfiles", m_ui->checkIgnoreDotfiles->isChecked());
    if (m_lt2) saved.insert("Format", m_ui->comboTorrentFormat->currentData());
    else
    {
        saved.insert("OptimizeAlignment", m_ui->checkOptimizeAlignment->isChecked());
        saved.insert("PaddedFileSizeLimit", m_ui->spinPaddedFileSizeLimit->value());
    }
    saved.insert("LastSaveDirectory", m_lastSaveDirectory);
    m_settings.setValue(m_settingsKey, saved);
    m_settings.sync();
}

void RemoteTorrentCreatorDialog::updateSeedTarget(bool resetPath)
{
    const int index = m_ui->seedServerCombo->currentIndex();
    const bool enabled = m_ui->checkStartSeeding->isChecked();
    m_ui->seedServerCombo->setEnabled(enabled);
    const bool other = enabled && index > 0 && index < m_seedTargets.size();
    m_ui->seedTargetRow->setVisible(other);
    if (resetPath) m_seedPathEdited = false;
    if (!other || m_seedPathEdited) return;
    QString source = m_ui->textInputPath->text().trimmed();
    const QString mapped = Remote::PathMappingStore::toRemote(source, m_mappings);
    if (!mapped.isEmpty()) source = mapped;
    source = serverPath(source);
    QString destination;
    if (absoluteServerPath(source))
    {
        const auto &target = m_seedTargets.at(index).profile;
        const QString local = Remote::PathMappingStore::toLocal(source, m_mappings);
        if (!local.isEmpty())
            destination = Remote::PathMappingStore::toRemote(local,
                Remote::PathMappingStore(m_settings).mappings(target.id));
        // Separate instances on the same NAS commonly share absolute paths.
        // This is only an editable suggestion, not a claim of shared storage.
        if (destination.isEmpty() && QUrl(target.url).host() == QUrl(m_seedTargets.first().profile.url).host())
            destination = source;
    }
    m_ui->seedTargetPath->setText(destination);
}

void RemoteTorrentCreatorDialog::seedOnTarget(const QString &filename, const QByteArray &bytes,
    const QString &torrentName)
{
    setState(State::Seeding, tr("Torrent saved locally. Adding it to %1…")
        .arg(m_seedTargets.at(m_seedTargetIndex).profile.name));
    const auto &target = m_seedTargets.at(m_seedTargetIndex);
    const QString name = target.profile.name;
    if (!target.session || !target.session->isConnected())
    {
        finishCrossSeed(tr("Torrent saved locally. Seeding on %1 could not start: server disconnected.").arg(name));
        return;
    }
    const QString parent = serverParent(m_seedTargetPath);
    if (parent.isEmpty() || m_seedTargetPath.section(QLatin1Char('/'), -1) != torrentName)
    {
        finishCrossSeed(tr("Torrent saved locally. Seeding on %1 was not started: the target file or folder name does not match the torrent.").arg(name));
        return;
    }
    Remote::AddTorrentOptions options;
    options.savePath = parent;
    options.disableDownloadPath = true;
    options.seedMode = true;
    options.stopCondition = QStringLiteral("None");
    const QPointer<RemoteTorrentCreatorDialog> self(this);
    target.session->addTorrentFiles({{filename, bytes}}, options, [self, name] {
        if (self) self->finishCrossSeed(tr("Torrent saved locally. Immediate seeding was requested on %1.").arg(name));
    }, [self, name](const QString &error) {
        if (self) self->finishCrossSeed(tr("Torrent saved locally. Seeding on %1 could not be confirmed: %2").arg(name, error));
    });
}

void RemoteTorrentCreatorDialog::finishCrossSeed(const QString &message)
{
    if (m_state != State::Seeding) return;
    m_seedResult = message;
    removeTask(true);
}

void RemoteTorrentCreatorDialog::setState(State state, const QString &message)
{
    m_state = state;
    m_status->setText(message);
    const bool ready = state == State::Ready;
    m_ui->scrollArea->setEnabled(ready);
    const bool output = ready || state == State::Finished;
    m_output->setEnabled(output);
    m_browseOutput->setEnabled(output);
    auto *ok = m_ui->buttonBox->button(QDialogButtonBox::Ok);
    auto *cancel = m_ui->buttonBox->button(QDialogButtonBox::Cancel);
    ok->setEnabled(ready || state == State::Finished || state == State::Uncertain || state == State::Saved);
    ok->setText(state == State::Finished ? tr("Save Torrent") :
        state == State::Uncertain ? tr("Retry Status") : state == State::Saved ? tr("Close") : tr("Create Torrent"));
    if (state == State::Uncertain && m_taskId.isEmpty()) ok->setEnabled(false);
    cancel->setEnabled(state != State::Submitting && state != State::Retrieving && state != State::Seeding && state != State::Cancelling);
    cancel->setText(state == State::Running || state == State::Uncertain || state == State::Finished
        ? tr("Cancel Task") : tr("Close"));
    if (state == State::Running || state == State::Submitting || state == State::Loading)
        m_ui->progressBar->setRange(0, 0);
    else m_ui->progressBar->setRange(0, 100);
    if (state == State::Finished || state == State::Saved) m_ui->progressBar->setValue(100);
}

bool RemoteTorrentCreatorDialog::selectMappedSource(const QString &local)
{
    const QString remote = Remote::PathMappingStore::toRemote(local, m_mappings);
    if (remote.isEmpty())
    {
        m_status->setText(tr("The selected local path is not mapped to this server. Configure Remote Paths or type the server path."));
        return false;
    }
    m_ui->textInputPath->setText(remote);
    return true;
}

void RemoteTorrentCreatorDialog::browseSource(bool directory)
{
    if (m_state != State::Ready) return;
    const QString root = m_mappings.isEmpty() ? QString() : m_mappings.first().localRoot;
    const QString local = directory ? QFileDialog::getExistingDirectory(this, tr("Select mapped folder"), root)
        : QFileDialog::getOpenFileName(this, tr("Select mapped file"), root);
    if (!local.isEmpty()) selectMappedSource(local);
}

void RemoteTorrentCreatorDialog::dragEnterEvent(QDragEnterEvent *event)
{
    if (m_state == State::Ready && event->mimeData()->hasUrls()
        && event->mimeData()->urls().size() == 1 && event->mimeData()->urls().first().isLocalFile())
        event->acceptProposedAction();
}

void RemoteTorrentCreatorDialog::dropEvent(QDropEvent *event)
{
    if (m_state == State::Ready && event->mimeData()->hasUrls()
        && event->mimeData()->urls().size() == 1 && event->mimeData()->urls().first().isLocalFile()
        && selectMappedSource(event->mimeData()->urls().first().toLocalFile()))
        event->acceptProposedAction();
}

QString RemoteTorrentCreatorDialog::suggestedFilename() const
{
    QString source = m_ui->textInputPath->text().trimmed();
    source.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (source.endsWith(QLatin1Char('/'))) source.chop(1);
    QString name = source.section(QLatin1Char('/'), -1);
    if (name.isEmpty()) name = QStringLiteral("created");
    return name + QStringLiteral(".torrent");
}

void RemoteTorrentCreatorDialog::chooseOutput()
{
    QString initial = m_output->text();
    if (initial.isEmpty())
        initial = QDir(m_lastSaveDirectory.isEmpty()
            ? QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)
            : m_lastSaveDirectory).filePath(suggestedFilename());
    const QString path = QFileDialog::getSaveFileName(this, tr("Save torrent locally"), initial, tr("Torrent files (*.torrent)"));
    if (!path.isEmpty())
        m_output->setText(path.endsWith(QLatin1String(".torrent"), Qt::CaseInsensitive) ? path : path + QStringLiteral(".torrent"));
}

QStringList RemoteTorrentCreatorDialog::urlLines(const QString &text, bool tiers, bool *valid) const
{
    QStringList lines;
    for (const auto &raw : text.split(QLatin1Char('\n')))
    {
        const QString line = raw.trimmed();
        if (line.isEmpty())
        {
            if (tiers && !lines.isEmpty() && !lines.last().isEmpty()) lines.append(QString());
            continue;
        }
        const QUrl url(line, QUrl::StrictMode);
        const QString scheme = url.scheme().toLower();
        if (!url.isValid() || url.host().isEmpty() || line.contains(QLatin1Char('|'))
            || (scheme != QLatin1String("http") && scheme != QLatin1String("https")
                && (!tiers || scheme != QLatin1String("udp"))))
        {
            *valid = false;
            return {};
        }
        // 5.3 decodes each item once in addition to normal form decoding.
        lines.append(m_modern ? QString::fromLatin1(QUrl::toPercentEncoding(line)) : line);
    }
    if (!lines.isEmpty() && lines.last().isEmpty()) lines.removeLast();
    return lines;
}

void RemoteTorrentCreatorDialog::createOrRetry()
{
    if (m_state == State::Saved) { accept(); return; }
    if (m_state == State::Finished) { retrieve(); return; }
    if (m_state == State::Uncertain && !m_taskId.isEmpty()) { poll(); return; }
    if (m_state != State::Ready || !m_session || !m_session->isConnected()) return;
    QString source = m_ui->textInputPath->text().trimmed();
    // Typed input is a server path; local mapped paths are translated.
    const QString mapped = Remote::PathMappingStore::toRemote(source, m_mappings);
    if (!mapped.isEmpty()) source = mapped;
    const bool absolute = source.startsWith(QLatin1Char('/'))
        || (source.size() >= 3 && source.at(0).isLetter() && source.at(1) == QLatin1Char(':')
            && (source.at(2) == QLatin1Char('/') || source.at(2) == QLatin1Char('\\')));
    if (!absolute || source.contains(QChar::Null))
    {
        m_status->setText(tr("Enter an absolute path on the selected server."));
        return;
    }
    bool valid = true;
    const QStringList trackers = urlLines(m_ui->trackersList->toPlainText(), true, &valid);
    const QStringList seeds = urlLines(m_ui->URLSeedsList->toPlainText(), false, &valid);
    if (!valid) { m_status->setText(tr("Enter valid tracker and web seed URLs, one per line.")); return; }
    if (m_output->text().trimmed().isEmpty()) chooseOutput();
    if (m_output->text().trimmed().isEmpty()) return;
    if (!QDir::isAbsolutePath(m_output->text().trimmed()))
    {
        m_status->setText(tr("Choose an absolute local output filename."));
        return;
    }
    m_source = source;
    m_seed = m_ui->checkStartSeeding->isChecked();
    m_seedTargetIndex = m_seed ? m_ui->seedServerCombo->currentIndex() : 0;
    m_seedResult.clear();
    if (m_seedTargetIndex > 0)
    {
        const auto &target = m_seedTargets.at(m_seedTargetIndex);
        m_seedTargetPath = serverPath(m_ui->seedTargetPath->text());
        if (!target.session || !target.session->isConnected())
        {
            m_status->setText(tr("The selected seeding server is not connected."));
            return;
        }
        if (!absoluteServerPath(m_seedTargetPath) || serverParent(m_seedTargetPath).isEmpty()
            || m_seedTargetPath.section(QLatin1Char('/'), -1) != serverPath(source).section(QLatin1Char('/'), -1))
        {
            m_status->setText(tr("Enter the absolute existing file or folder path on the seeding server, with the same name as the source."));
            return;
        }
        if (QMessageBox::question(this, tr("Seed on another server"),
                tr("Start seeding on %1 using existing data at:\n%2\n\nOnly torrent metadata will be sent. Confirm that this is the same data and is accessible to that server.")
                    .arg(target.profile.name, m_seedTargetPath),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
    }
    const auto boolean = [](bool b) { return b ? QStringLiteral("true") : QStringLiteral("false"); };
    QMap<QString, QString> params{
        {"sourcePath", source}, {"startSeeding", boolean(m_seed && m_seedTargetIndex == 0)},
        {"private", boolean(m_ui->checkPrivate->isChecked())},
        {"pieceSize", m_ui->comboPieceSize->currentData().toString()},
        {"trackers", trackers.join(QLatin1Char('|'))}, {"urlSeeds", seeds.join(QLatin1Char('|'))},
        {"comment", m_ui->txtComment->toPlainText()}, {"source", m_ui->lineEditSource->text()}
    };
    if (m_modern) params.insert("ignoreDotfiles", boolean(m_ui->checkIgnoreDotfiles->isChecked()));
    if (m_lt2) params.insert("format", m_ui->comboTorrentFormat->currentData().toString());
    else
    {
        params.insert("optimizeAlignment", boolean(m_ui->checkOptimizeAlignment->isChecked()));
        const int size = m_ui->spinPaddedFileSizeLimit->value();
        params.insert("paddedFileSizeLimit", QString::number(size < 0 ? -1 : size * 1024));
    }
    setState(State::Submitting, tr("Submitting creation task to server…"));
    const QPointer<RemoteTorrentCreatorDialog> self(this);
    m_session->createTorrentTask(params, [self](const QJsonObject &reply) {
        if (!self || self->m_state != State::Submitting) return;
        self->m_taskId = reply.value("taskID").toString();
        if (self->m_taskId.isEmpty())
        {
            self->setState(State::Uncertain, tr("The server did not return a task ID. Check its Torrent Creator before resubmitting."));
            return;
        }
        self->poll();
    }, [self](const QString &error) {
        if (self && self->m_state == State::Submitting)
            self->setState(State::Uncertain, tr("Creation could not be confirmed: %1. Check the server before resubmitting.").arg(error));
    });
}

void RemoteTorrentCreatorDialog::poll()
{
    if (!m_session || !m_session->isConnected()) { disconnected(); return; }
    if (m_pollPending || m_taskId.isEmpty()) return;
    m_pollPending = true;
    if (m_state != State::Running)
        setState(State::Running, tr("Checking server creation task…"));
    const QPointer<RemoteTorrentCreatorDialog> self(this);
    m_session->requestTorrentCreationStatus(m_taskId, [self](const QJsonArray &reply) {
        if (!self) return;
        self->m_pollPending = false;
        if (self->m_state != State::Running) return;
        QJsonObject task;
        for (const auto &v : reply)
            if (v.toObject().value("taskID").toString() == self->m_taskId) { task = v.toObject(); break; }
        const QString state = task.value("status").toString();
        if (state == QLatin1String("Finished"))
        {
            self->m_finishedTask = true;
            self->setState(State::Finished, tr("Torrent created on the server. Retrieving metadata…"));
            self->retrieve();
        }
        else if (state == QLatin1String("Failed"))
        {
            self->m_finishedTask = true;
            self->setState(State::Failed, tr("Server creation failed: %1").arg(task.value("errorMessage").toString()));
        }
        else if (state == QLatin1String("Running") || state == QLatin1String("Queued"))
        {
            const auto progress = task.value("progress");
            if (progress.isDouble() && progress.toDouble() >= 0 && progress.toDouble() <= 100)
            {
                self->m_ui->progressBar->setRange(0, 100);
                self->m_ui->progressBar->setValue(progress.toInt());
            }
            self->m_status->setText(state == QLatin1String("Queued") ? tr("Queued on server…") : tr("Hashing on server…"));
            self->m_poll.start();
        }
        else self->setState(State::Uncertain, tr("Unrecognized task status. Task ID: %1").arg(self->m_taskId));
    }, [self](const QString &error) {
        if (!self) return;
        self->m_pollPending = false;
        if (self->m_state == State::Running)
            self->setState(State::Uncertain, tr("Cannot read task status: %1. Task ID: %2").arg(error, self->m_taskId));
    });
}

void RemoteTorrentCreatorDialog::retrieve()
{
    if (m_state != State::Finished || !m_session || !m_session->isConnected()) return;
    QString path = m_output->text().trimmed();
    if (path.isEmpty()) { chooseOutput(); path = m_output->text().trimmed(); }
    if (path.isEmpty() || !QDir::isAbsolutePath(path))
    {
        m_status->setText(tr("Choose an absolute local output filename."));
        return;
    }
    if (!path.endsWith(QLatin1String(".torrent"), Qt::CaseInsensitive)) path += QStringLiteral(".torrent");
    if (QFileInfo::exists(path) && QMessageBox::question(this, tr("Replace file?"),
            tr("Replace the existing local torrent file?"), QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    setState(State::Retrieving, tr("Downloading torrent metadata…"));
    const QPointer<RemoteTorrentCreatorDialog> self(this);
    m_session->downloadCreatedTorrent(m_taskId, [self, path](const QByteArray &bytes) {
        if (!self || self->m_state != State::Retrieving) return;
        Remote::TorrentMetadata metadata;
        QString error;
        if (!Remote::TorrentMetadataReader::read(bytes, metadata, &error))
        {
            self->setState(State::Finished, tr("Invalid torrent metadata: %1. The server task was retained.").arg(error));
            return;
        }
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        {
            self->setState(State::Finished, tr("Cannot save local torrent: %1. Choose another file and retry.").arg(file.errorString()));
            return;
        }
        self->m_fileSaved = true;
        self->m_lastSaveDirectory = QFileInfo(path).absolutePath();
        if (self->m_seed && self->m_seedTargetIndex > 0)
            self->seedOnTarget(QFileInfo(path).fileName(), bytes, metadata.name);
        else
            self->removeTask(true);
    }, [self](const QString &error) {
        if (self && self->m_state == State::Retrieving)
            self->setState(State::Finished, tr("Cannot retrieve torrent: %1. Retry saving.").arg(error));
    });
}

void RemoteTorrentCreatorDialog::removeTask(bool saved)
{
    m_poll.stop();
    if (!m_session || !m_session->isConnected()) { disconnected(); return; }
    setState(State::Cancelling, saved ? tr("Torrent saved locally. Removing completed task…") : tr("Removing server task…"));
    const QPointer<RemoteTorrentCreatorDialog> self(this);
    m_session->deleteTorrentCreationTask(m_taskId, [self, saved] {
        if (!self || self->m_state != State::Cancelling) return;
        self->m_taskId.clear();
        if (saved)
            self->setState(State::Saved, !self->m_seedResult.isEmpty() ? self->m_seedResult : self->m_seed
                ? tr("Torrent saved locally. Immediate seeding was requested on the server.")
                : tr("Torrent saved locally. Seeding was not requested."));
        else self->QDialog::reject();
    }, [self, saved](const QString &error) {
        if (!self || self->m_state != State::Cancelling) return;
        self->setState(saved ? State::Saved : State::Uncertain,
            tr("%1 Server task %2 could not be removed: %3").arg(saved ? (self->m_seedResult.isEmpty() ? tr("Torrent saved locally.") : self->m_seedResult)
                : tr("Cancellation not confirmed."),
                self->m_taskId, error));
    });
}

void RemoteTorrentCreatorDialog::reject()
{
    if (m_state == State::Submitting || m_state == State::Retrieving || m_state == State::Seeding || m_state == State::Cancelling) return;
    if (!m_taskId.isEmpty() && !m_fileSaved && m_session && m_session->isConnected() && m_state != State::Disconnected)
    {
        if (QMessageBox::question(this, tr("Remove torrent creation task"),
                tr("Remove this server task? If creation already finished, its torrent may already have been added for seeding."),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
            removeTask(false);
        return;
    }
    m_poll.stop();
    QDialog::reject();
}

void RemoteTorrentCreatorDialog::disconnected()
{
    m_poll.stop();
    setState(State::Disconnected, m_taskId.isEmpty()
        ? tr("The server disconnected. Any submitted task may still be running; check the server before retrying.")
        : tr("%1 Server disconnected; task %2 may still exist. Check the server before resubmitting.")
            .arg(m_fileSaved ? tr("Torrent saved locally.") : QString(), m_taskId));
}
