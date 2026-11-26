// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/RemoteAddTorrentDialog.h"
#include "gui/RemoteFileSystemPathComboEdit.h"
#include "gui/RemoteTorrentContentWidget.h"

#include "ui_addnewtorrentdialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QToolButton>
#include <QStyle>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QSplitter>
#include <QShowEvent>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <utility>

namespace
{
QString displaySize(qint64 bytes)
{
    return QLocale().formattedDataSize(bytes);
}

bool validLink(const QString &line)
{
    const QUrl url(line, QUrl::StrictMode);
    if (!url.isValid())
        return false;
    const QString scheme = url.scheme().toLower();
    return scheme == QLatin1String("magnet")
        || ((scheme == QLatin1String("http") || scheme == QLatin1String("https"))
            && !url.host().isEmpty());
}
}

RemoteAddTorrentDialog::RemoteAddTorrentDialog(QSettings &settings,
                                               const Remote::ServerProfile &server,
                                               Source source, QString initialLinks,
                                               QString fileName, QByteArray fileContents,
                                               QWidget *parent,
                                               QList<Remote::ServerProfile> availableServers,
                                               std::function<bool(const QString &)> serverIsConnected,
                                               QList<Remote::TorrentUpload> batchFiles,
                                               std::function<Remote::RemoteSession *(const QString &)> sessionForServer)
    : QDialog(settings.value(QStringLiteral("Preferences/Advanced/AttachAddTorrentDialog"), true).toBool()
              ? parent : nullptr)
    , m_settings(settings)
    , m_server(server)
    , m_pathStore(settings)
    , m_ui(std::make_unique<Ui::AddNewTorrentDialog>())
    , m_source(source)
    , m_fileName(std::move(fileName))
    , m_fileContents(std::move(fileContents))
    , m_batchFiles(std::move(batchFiles))
    , m_availableServers(std::move(availableServers))
    , m_serverIsConnected(std::move(serverIsConnected))
    , m_sessionForServer(std::move(sessionForServer))
{
    m_ui->setupUi(this);
    if (!settings.value(QStringLiteral("Preferences/Advanced/AttachAddTorrentDialog"), true).toBool())
        setWindowFlags(Qt::Window);
    setObjectName(QStringLiteral("remoteAddTorrentDialog"));
    setWindowTitle(tr("Add Torrent — %1").arg(server.name));
    setMinimumSize(700, 480);
    // Preserve original upstream dialog proportions and its two-column
    // split rather than pushing the native file view into a narrow gutter.
    resize(900, 680);
    m_ui->splitter->setSizes({510, 380});
    m_ui->scrollArea->viewport()->setAutoFillBackground(false);
    m_ui->scrollAreaWidgetContents->setAutoFillBackground(false);

    if (m_availableServers.isEmpty())
        m_availableServers.append(server);
    else if (std::none_of(m_availableServers.cbegin(), m_availableServers.cend(),
                          [&server](const auto &profile) { return profile.id == server.id; }))
        m_availableServers.prepend(server);

    setupPaths();
    setupOptions();
    setupServerSelector();
    adaptOptionsGridToFont();
    setupSource(std::move(initialLinks));

    connect(m_ui->buttonBox, &QDialogButtonBox::accepted,
            this, &RemoteAddTorrentDialog::accept);
    connect(m_ui->buttonBox, &QDialogButtonBox::rejected, this, [this]() {
        m_explicitlyCancelled = true;
        reject();
    });

    // Qt can select the native server path without claiming that the local
    // machine or the Web API can browse an unmounted server filesystem.
    connect(m_ui->comboTMM, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) {
        m_ui->groupBoxSavePath->setEnabled(index == 0);
    });

    const QByteArray splitter = settings.value(
        QStringLiteral("Layout/Dialogs/AddTorrent/Splitter")).toByteArray();
    // The old generic-tree implementation saved a left-heavy splitter.
    // Migrate that layout once, then respect any proportions the user
    // deliberately chooses in this native-adapter version.
    if (settings.value(QStringLiteral("Layout/Dialogs/AddTorrent/SplitterVersion"),
                       0).toInt() >= 2 && !splitter.isEmpty())
        m_ui->splitter->restoreState(splitter);
    loadServerDefaults();
}

RemoteAddTorrentDialog::~RemoteAddTorrentDialog() = default;

void RemoteAddTorrentDialog::setupServerSelector()
{
    if (m_availableServers.size() < 2)
        return; // Exactly the original upstream dialog with one connection.

    // A compact Qt-native row in the left options pane, without any new
    // group box, explanatory strip, or change to the main splitter.
    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    auto *label = new QLabel(tr("Server:"), m_ui->torrentoptionsFrame);
    m_serverSelector = new QComboBox(m_ui->torrentoptionsFrame);
    m_serverSelector->setObjectName(QStringLiteral("addTorrentServerSelector"));
    m_serverSelector->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_serverSelector->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_serverSelector->setMaximumWidth(240);
    for (const auto &profile : m_availableServers)
        m_serverSelector->addItem(profile.name, profile.id);
    m_serverSelector->setCurrentIndex(m_serverSelector->findData(m_server.id));
    row->addWidget(label);
    row->addWidget(m_serverSelector);
    row->addStretch(1);
    m_ui->mainlayout_addui->insertLayout(0, row);

    connect(m_serverSelector, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) {
        if (index >= 0)
            switchServer(m_serverSelector->itemData(index).toString());
    });
}

void RemoteAddTorrentDialog::clearTargetFields()
{
    // Switching even *back* to a former server does not restore old inputs.
    // Each destination needs an explicit new choice, or can remain empty
    // when the user intends to use that server's own default path.
    populatePathChoices();
    m_ui->savePath->setEditText(QString());
    m_ui->downloadPath->setEditText(QString());
    m_ui->categoryComboBox->setEditText(QString());
    m_ui->tagsLineEdit->clear();
    m_ui->checkBoxRememberLastSavePath->setChecked(false);
    m_ui->groupBoxDownloadPath->setChecked(false);
    m_ui->comboTMM->setCurrentIndex(0);
    m_ui->groupBoxSavePath->setEnabled(true);
    m_ui->savePath->setToolTip(tr(
        "Absolute path on %1, not on this computer. Leave empty to use "
        "the qBittorrent server's default download directory.").arg(m_server.name));
    m_ui->downloadPath->setToolTip(
        tr("Absolute incomplete-download path on %1.").arg(m_server.name));
}

void RemoteAddTorrentDialog::switchServer(const QString &id)
{
    if (id == m_server.id)
        return;
    const auto it = std::find_if(m_availableServers.cbegin(), m_availableServers.cend(),
                                 [&id](const auto &server) { return server.id == id; });
    if (it == m_availableServers.cend())
        return;
    m_server = *it;
    clearTargetFields();
    loadServerDefaults();
    if (m_source == Source::Links)
        setWindowTitle(tr("Add Torrent — %1").arg(m_server.name));
    else if (m_source == Source::TorrentFiles)
        setWindowTitle(tr("Add %1 torrents — %2")
            .arg(m_batchFiles.size()).arg(m_server.name));
}

void RemoteAddTorrentDialog::adaptOptionsGridToFont()
{
    // At ordinary font sizes retain upstream's exact checkbox placement.
    // At large sizes the two long labels force an excessively wide left
    // pane, squeezing the native content tree into an unusable gutter.
    // Reflow only that row instead of constraining the user's font.
    const int needed = m_ui->sequentialCheckBox->sizeHint().width()
        + m_ui->firstLastCheckBox->sizeHint().width()
        + m_ui->gridLayout->horizontalSpacing();
    if (needed <= 450)
        return;

    m_ui->gridLayout->removeWidget(m_ui->firstLastCheckBox);
    m_ui->gridLayout->removeWidget(m_ui->doNotDeleteTorrentCheckBox);
    m_ui->gridLayout->addWidget(m_ui->firstLastCheckBox, 3, 0, 1, 2);
    m_ui->gridLayout->addWidget(m_ui->doNotDeleteTorrentCheckBox, 4, 0, 1, 2);
    // The upstream right-side spacer is redundant in the reflowed layout.
    if (auto *item = m_ui->gridLayout->itemAtPosition(0, 2))
        if (auto *spacer = item->spacerItem())
            spacer->changeSize(0, 0, QSizePolicy::Minimum, QSizePolicy::Minimum);
    m_ui->gridLayout->invalidate();
}

void RemoteAddTorrentDialog::setupPaths()
{
    m_ui->savePath->setEditable(true);
    m_ui->downloadPath->setEditable(true);
    m_ui->savePath->setInsertPolicy(QComboBox::NoInsert);
    m_ui->downloadPath->setInsertPolicy(QComboBox::NoInsert);
    m_ui->savePath->comboBox()->setObjectName(QStringLiteral("remoteSavePath"));
    m_ui->downloadPath->comboBox()->setObjectName(QStringLiteral("remoteIncompletePath"));
    m_ui->savePath->setToolTip(tr(
        "Absolute path on %1, not on this computer. Leave empty to use "
        "the qBittorrent server's default download directory.").arg(m_server.name));
    m_ui->downloadPath->setToolTip(tr(
        "Absolute incomplete-download path on %1.").arg(m_server.name));
    m_ui->savePath->lineEdit()->setPlaceholderText(tr("Server default download folder"));
    m_ui->downloadPath->lineEdit()->setPlaceholderText(
        tr("Remote incomplete-download directory"));

    populatePathChoices();
    const QString remembered = m_settings.value(savedPathKey()).toString();
    if (!remembered.isEmpty())
    {
        m_ui->savePath->setEditText(remembered);
        m_ui->checkBoxRememberLastSavePath->setChecked(true);
    }

    connect(m_ui->savePath, &RemoteFileSystemPathComboEdit::browseRequested,
            this, [this]() { browseMappedPath(m_ui->savePath); });
    connect(m_ui->downloadPath, &RemoteFileSystemPathComboEdit::browseRequested,
            this, [this]() { browseMappedPath(m_ui->downloadPath); });
    m_ui->groupBoxDownloadPath->setChecked(false);
}

void RemoteAddTorrentDialog::setupOptions()
{
    m_ui->comboTMM->setCurrentIndex(0);
    m_ui->categoryComboBox->setEditable(true);
    m_ui->categoryComboBox->setInsertPolicy(QComboBox::NoInsert);
    m_ui->categoryComboBox->setCurrentText(QString());
    m_ui->tagsLineEdit->setReadOnly(false);
    m_ui->tagsLineEdit->setPlaceholderText(tr("Comma-separated server tags (optional)"));

    m_ui->startTorrentCheckBox->setChecked(true);
    m_ui->stopConditionComboBox->addItem(tr("None"), QStringLiteral("None"));
    m_ui->stopConditionComboBox->setEnabled(false);
    m_ui->stopConditionLabel->setEnabled(false);
    connect(m_ui->startTorrentCheckBox, &QCheckBox::toggled,
            this, [this]() { refreshDefaultControls(); });
    m_ui->addToQueueTopCheckBox->setChecked(false);
    m_ui->seedModeCheckBox->setChecked(false);
    m_ui->sequentialCheckBox->setChecked(false);
    m_ui->firstLastCheckBox->setChecked(false);

    // The Web API does not manage this local GUI behavior.
    m_ui->defaultCategoryCheckbox->setChecked(false);
    m_ui->defaultCategoryCheckbox->setEnabled(false);
    m_ui->defaultCategoryCheckbox->setToolTip(tr("Not implemented in qBittorrent Remote."));
    const bool allowDelete = m_source != Source::Links && m_settings.value(
        QStringLiteral("Preferences/Downloads/DeleteOriginalTorrent"), false).toBool();
    m_ui->doNotDeleteTorrentCheckBox->setChecked(false);
    m_ui->doNotDeleteTorrentCheckBox->setEnabled(allowDelete);
    m_ui->doNotDeleteTorrentCheckBox->setVisible(allowDelete);
    m_ui->doNotDeleteTorrentCheckBox->setToolTip(tr(
        "Keep all original local .torrent files for this addition, including if cancelled."));
    m_ui->checkBoxNeverShow->setChecked(false);
    m_ui->checkBoxNeverShow->setEnabled(true);
    m_ui->checkBoxNeverShow->setToolTip(tr("Skip the Add Torrent options dialog for future additions. Change this in Preferences → Downloads."));
    m_ui->tagsEditButton->setEnabled(false);
    m_ui->tagsEditButton->setToolTip(tr(
        "Enter tags separated by commas above. Native tag editing is planned."));

    // Upstream inserts a filename filter beside Select All/None. Keep it
    // in the same contentFilterLayout rather than adding a new dialog row.
    auto *filter = new QLineEdit(this);
    filter->setObjectName(QStringLiteral("torrentContentFilter"));
    filter->setPlaceholderText(tr("Filter…"));
    filter->setClearButtonEnabled(true);
    filter->setMaximumWidth(130);
    filter->addAction(QIcon(QStringLiteral(":/icons/edit-find.svg")),
                      QLineEdit::LeadingPosition);
    m_ui->contentFilterLayout->addWidget(filter);
    connect(filter, &QLineEdit::textChanged,
            m_ui->contentTreeView, &RemoteTorrentContentWidget::setFilterText);

    // Older Web API releases, including 2.11.4 on the user's QNAP, cannot
    // accept file priorities with an uploaded torrent in /torrents/add.
    // Do NOT display fake selectable checkboxes which get silently ignored.
    m_ui->buttonSelectAll->setEnabled(false);
    m_ui->buttonSelectNone->setEnabled(false);
    for (auto *button : {m_ui->buttonSelectAll, m_ui->buttonSelectNone})
        button->setToolTip(tr(
            "File priorities must be changed after adding the torrent "
            "using the bottom Content panel (server Web API limitation)."));
}

void RemoteAddTorrentDialog::refreshDefaultControls()
{
    const bool editable = !m_defaultsLoading;
    m_ui->comboTMM->setEnabled(editable);
    m_ui->contentLayoutComboBox->setEnabled(editable);
    m_ui->startTorrentCheckBox->setEnabled(editable);
    m_ui->addToQueueTopCheckBox->setEnabled(editable);
    const bool stopAvailable = editable && m_stopConditionSupported
        && m_ui->startTorrentCheckBox->isChecked();
    m_ui->stopConditionComboBox->setEnabled(stopAvailable);
    m_ui->stopConditionLabel->setEnabled(stopAvailable);
    m_ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(editable);
}

void RemoteAddTorrentDialog::loadServerDefaults()
{
    const auto generation = ++m_defaultsGeneration;
    m_defaultsLoading = false;
    m_stopConditionSupported = false;
    m_mergeTrackersDefault = QJsonValue(QJsonValue::Undefined);
    m_ui->comboTMM->setCurrentIndex(0);
    m_ui->contentLayoutComboBox->setCurrentIndex(0);
    m_ui->startTorrentCheckBox->setChecked(true);
    m_ui->addToQueueTopCheckBox->setChecked(false);
    m_ui->stopConditionComboBox->clear();
    m_ui->stopConditionComboBox->addItem(tr("None"), QStringLiteral("None"));
    m_ui->stopConditionComboBox->setToolTip(tr("Stop-condition support is not available from this server."));
    auto *session = m_sessionForServer ? m_sessionForServer(m_server.id) : nullptr;
    if (!session || !session->isConnected())
    {
        refreshDefaultControls();
        return;
    }
    m_defaultsLoading = true;
    m_ui->buttonBox->button(QDialogButtonBox::Ok)->setToolTip(tr("Reading the selected server's torrent defaults…"));
    refreshDefaultControls();

    const QPointer<RemoteAddTorrentDialog> dialog(this);
    const auto finish = [dialog, generation](const QJsonObject &values) {
        if (!dialog || dialog->m_defaultsGeneration != generation || !dialog->m_defaultsLoading)
            return;
        const auto boolean = [&values](const char *key, bool fallback) {
            const auto value = values.value(QLatin1String(key));
            return value.isBool() ? value.toBool() : fallback;
        };
        const auto merge = values.value(QStringLiteral("merge_trackers"));
        if (merge.isBool()) dialog->m_mergeTrackersDefault = merge;
        auto &ui = *dialog->m_ui;
        ui.comboTMM->setCurrentIndex(boolean("auto_tmm_enabled", false) ? 1 : 0);
        const QStringList layouts{QStringLiteral("Original"), QStringLiteral("Subfolder"), QStringLiteral("NoSubfolder")};
        const int layout = layouts.indexOf(values.value(QStringLiteral("torrent_content_layout")).toString());
        ui.contentLayoutComboBox->setCurrentIndex(layout >= 0 ? layout : 0);
        ui.addToQueueTopCheckBox->setChecked(boolean("add_to_top_of_queue", false));
        const QString stop = values.value(QStringLiteral("torrent_stop_condition")).toString();
        dialog->m_stopConditionSupported = stop == QLatin1String("None")
            || stop == QLatin1String("MetadataReceived") || stop == QLatin1String("FilesChecked");
        bool stopped = boolean("add_stopped_enabled", false);
        if (dialog->m_stopConditionSupported)
        {
            if (dialog->m_source == Source::Links)
                ui.stopConditionComboBox->addItem(dialog->tr("Metadata received"), QStringLiteral("MetadataReceived"));
            ui.stopConditionComboBox->addItem(dialog->tr("Files checked"), QStringLiteral("FilesChecked"));
            // Native qBittorrent treats metadata-already-present torrents
            // with a MetadataReceived default as stopped at addition.
            if (stop == QLatin1String("MetadataReceived") && dialog->m_source != Source::Links)
                stopped = true;
            const int index = ui.stopConditionComboBox->findData(stop);
            ui.stopConditionComboBox->setCurrentIndex(index >= 0 ? index : 0);
            ui.stopConditionComboBox->setToolTip(QString());
        }
        ui.startTorrentCheckBox->setChecked(!stopped);
        dialog->m_defaultsLoading = false;
        ui.buttonBox->button(QDialogButtonBox::Ok)->setToolTip(QString());
        dialog->refreshDefaultControls();
    };
    const auto failed = [finish](const QString &) { finish({}); };
    connect(session, &Remote::RemoteSession::failed, this, failed);
    connect(session, &Remote::RemoteSession::disconnected, this, [finish]() { finish({}); });
    connect(session, &QObject::destroyed, this, [finish]() { finish({}); });
    session->requestPreferences(finish, failed);
}

void RemoteAddTorrentDialog::setupSource(QString initialLinks)
{
    if (m_source == Source::Links)
    {
        auto *group = new QGroupBox(tr("Torrent link(s)"), m_ui->torrentoptionsFrame);
        group->setObjectName(QStringLiteral("torrentLinksGroup"));
        auto *layout = new QVBoxLayout(group);
        m_linkInput = new QPlainTextEdit(group);
        m_linkInput->setObjectName(QStringLiteral("torrentLinks"));
        m_linkInput->setPlaceholderText(tr("Paste a magnet, HTTP or HTTPS link, one per line"));
        m_linkInput->setPlainText(std::move(initialLinks));
        m_linkInput->setMaximumHeight(100);
        layout->addWidget(m_linkInput);
        m_ui->mainlayout_addui->insertWidget(m_serverSelector ? 1 : 0, group);
        m_ui->labelSizeData->setText(tr("Not available until metadata is downloaded"));
        m_ui->labelDateData->setText(tr("Not available"));
        m_ui->labelInfohash1Data->setText(tr("Not available"));
        m_ui->labelInfohash2Data->setText(tr("Not available"));
        m_ui->labelCommentData->setText(tr(
            "Magnet metadata is retrieved by the remote server after adding."));
        m_ui->lblMetaLoading->setText(tr(
            "File selection is available after adding, in the Content tab."));
        m_ui->contentTreeView->setEnabled(false);
        m_ui->buttonSave->setEnabled(false);
    }
    else if (m_source == Source::TorrentFiles)
    {
        // The content tree and metadata labels describe one torrent only.
        // Don't show a misleading preview of just the first file.
        auto *group = new QGroupBox(
            tr("Selected torrent files (%1)").arg(m_batchFiles.size()),
            m_ui->torrentoptionsFrame);
        group->setObjectName(QStringLiteral("torrentBatchGroup"));
        auto *layout = new QVBoxLayout(group);
        auto *names = new QPlainTextEdit(group);
        names->setObjectName(QStringLiteral("torrentBatchFileList"));
        names->setReadOnly(true);
        names->setMaximumHeight(125);
        QStringList labels;
        for (const auto &file : m_batchFiles)
            labels.append(file.filename);
        names->setPlainText(labels.join(QLatin1Char('\n')));
        layout->addWidget(names);
        m_ui->mainlayout_addui->insertWidget(m_serverSelector ? 1 : 0, group);
        setWindowTitle(tr("Add %1 torrents — %2")
            .arg(m_batchFiles.size()).arg(m_server.name));
        for (auto *label : {
                m_ui->labelSizeData, m_ui->labelDateData,
                m_ui->labelInfohash1Data, m_ui->labelInfohash2Data
             })
            label->setText(tr("Multiple torrents"));
        m_ui->labelCommentData->setText(
            tr("Shared options apply to all selected torrent files."));
        m_ui->lblMetaLoading->setText(tr(
            "Individual torrent files can be managed in the Content tab after adding."));
        m_ui->contentTreeView->setEnabled(false);
        m_ui->buttonSave->setEnabled(false);
    }
    else
    {
        m_ui->lblMetaLoading->clear();
        m_ui->buttonSave->setEnabled(true);
        connect(m_ui->buttonSave, &QPushButton::clicked, this, [this]() {
            const QString filename = QFileDialog::getSaveFileName(
                this, tr("Save .torrent metadata"), m_fileName,
                tr("Torrent files (*.torrent);;All files (*)"));
            if (filename.isEmpty())
                return;
            QFile output(filename);
            if (!output.open(QIODevice::WriteOnly)
                || output.write(m_fileContents) != m_fileContents.size())
                QMessageBox::warning(this, tr("Save failed"),
                                     tr("Could not write the .torrent metadata."));
        });
        displayMetadata();
    }
    m_ui->progMetaLoading->hide();
}

void RemoteAddTorrentDialog::displayMetadata()
{
    QString error;
    if (!Remote::TorrentMetadataReader::read(m_fileContents, m_metadata, &error))
    {
        m_ui->labelSizeData->setText(tr("Unavailable"));
        m_ui->labelDateData->setText(tr("Unavailable"));
        m_ui->lblMetaLoading->setText(tr(
            "Could not preview metadata; the server will validate the file."));
        m_ui->contentTreeView->setEnabled(false);
        return;
    }

    m_ui->labelSizeData->setText(displaySize(m_metadata.totalSize));
    m_ui->labelDateData->setText(m_metadata.created.isValid()
        ? QLocale().toString(m_metadata.created.toLocalTime(), QLocale::ShortFormat)
        : tr("Not available"));
    m_ui->labelInfohash1Data->setText(
        m_metadata.infoHashV1.isEmpty() ? tr("Not applicable") : m_metadata.infoHashV1);
    m_ui->labelInfohash2Data->setText(
        m_metadata.infoHashV2.isEmpty() ? tr("Not applicable") : m_metadata.infoHashV2);
    m_ui->labelCommentData->setText(m_metadata.comment.toHtmlEscaped());

    // The remote content adapter uses the same QTreeView + model structure
    // as native qBittorrent, including folder/file icons and check states.
    m_ui->contentTreeView->setTorrentFiles(m_metadata.files);
    setWindowTitle(m_metadata.name);
}

QString RemoteAddTorrentDialog::savedPathKey() const
{
    const QString hash = QString::fromLatin1(QCryptographicHash::hash(
        m_server.id.toUtf8(), QCryptographicHash::Sha256).toHex());
    return QStringLiteral("AddTorrent/LastRemoteSavePath/") + hash;
}

QString RemoteAddTorrentDialog::pathHistoryKey(bool incomplete) const
{
    return QStringLiteral("AddTorrent/RemotePathHistory/")
        + savedPathKey().section(QLatin1Char('/'), -1)
        + (incomplete ? QStringLiteral("/Incomplete") : QStringLiteral("/Save"));
}

void RemoteAddTorrentDialog::populatePathChoices()
{
    const int length = qBound(0, m_settings.value(QStringLiteral("Preferences/Advanced/SavePathHistoryLength"), 8).toInt(), 99);
    const auto fill = [&](RemoteFileSystemPathComboEdit *combo, bool incomplete) {
        combo->comboBox()->clear();
        QStringList paths = m_settings.value(pathHistoryKey(incomplete)).toStringList().mid(0, length);
        for (const auto &entry : m_pathStore.mappings(m_server.id))
            if (!paths.contains(entry.remoteRoot)) paths.append(entry.remoteRoot);
        for (const auto &path : paths) combo->addItem(path);
        combo->setCurrentIndex(-1);
        combo->setEditText(QString());
    };
    fill(m_ui->savePath, false);
    fill(m_ui->downloadPath, true);
}

void RemoteAddTorrentDialog::rememberPath(const QString &path, bool incomplete)
{
    const int length = qBound(0, m_settings.value(QStringLiteral("Preferences/Advanced/SavePathHistoryLength"), 8).toInt(), 99);
    const QString key = pathHistoryKey(incomplete);
    if (length == 0) { m_settings.remove(key); return; }
    if (path.isEmpty()) return;
    QStringList history = m_settings.value(key).toStringList();
    history.removeAll(path);
    history.prepend(path);
    m_settings.setValue(key, history.mid(0, length));
}

void RemoteAddTorrentDialog::browseMappedPath(RemoteFileSystemPathComboEdit *target)
{
    const auto entries = m_pathStore.mappings(m_server.id);
    if (entries.isEmpty())
    {
        QMessageBox::information(this, tr("No path mappings"),
            tr("Configure a local NAS mount in Preferences → Servers first, "
               "or type an absolute remote path manually."));
        return;
    }
    const QString local = QFileDialog::getExistingDirectory(
        this, tr("Choose mapped local NAS directory"),
        entries.constFirst().localRoot);
    if (local.isEmpty()) return;
    const QString remote = Remote::PathMappingStore::toRemote(local, entries);
    if (remote.isEmpty())
    {
        QMessageBox::warning(this, tr("Unmapped local directory"),
            tr("The chosen directory is not covered by a mapping for %1. "
               "No save path has been changed.").arg(m_server.name));
        return;
    }
    target->setEditText(remote);
}

bool RemoteAddTorrentDialog::setLocalSaveDirectory(const QString &local)
{
    const QString path = Remote::PathMappingStore::toRemote(
        local, m_pathStore.mappings(m_server.id));
    if (path.isEmpty()) return false;
    m_ui->savePath->setEditText(path);
    return true;
}

bool RemoteAddTorrentDialog::setLocalIncompleteDirectory(const QString &local)
{
    const QString path = Remote::PathMappingStore::toRemote(
        local, m_pathStore.mappings(m_server.id));
    if (path.isEmpty()) return false;
    m_ui->groupBoxDownloadPath->setChecked(true);
    m_ui->downloadPath->setEditText(path);
    return true;
}

QString RemoteAddTorrentDialog::resolveServerPath(const QString &text) const
{
    const QString value = text.trimmed();
    if (value.isEmpty())
        return {};
    const QString mapped = Remote::PathMappingStore::toRemote(
        value, m_pathStore.mappings(m_server.id));
    return mapped.isEmpty() ? value : mapped;
}

bool RemoteAddTorrentDialog::validateServerPath(RemoteFileSystemPathComboEdit *combo, QString *resolved,
                                                QString *error) const
{
    *resolved = resolveServerPath(combo->currentText());
    if (resolved->isEmpty()) return true;
    if (!resolved->startsWith(QLatin1Char('/'))
        || resolved->contains(QLatin1Char('\\'))
        || resolved->contains(QLatin1Char('\n'))
        || resolved->contains(QLatin1Char('\r')))
    {
        if (error)
            *error = tr("Enter an absolute POSIX path on %1, or use Browse "
                        "with a configured local-to-remote mapping.").arg(m_server.name);
        return false;
    }
    return true;
}

Remote::AddTorrentOptions RemoteAddTorrentDialog::options() const
{
    Remote::AddTorrentOptions result;
    result.autoTMM = m_ui->comboTMM->currentIndex() == 1;
    if (!result.autoTMM)
    {
        result.savePath = resolveServerPath(m_ui->savePath->currentText());
        result.useDownloadPath = m_ui->groupBoxDownloadPath->isChecked();
        if (result.useDownloadPath)
            result.downloadPath = resolveServerPath(m_ui->downloadPath->currentText());
    }
    result.category = m_ui->categoryComboBox->currentText().trimmed();
    result.tags = m_ui->tagsLineEdit->text().trimmed();
    result.stopped = !m_ui->startTorrentCheckBox->isChecked();
    result.queueTop = m_ui->addToQueueTopCheckBox->isChecked();
    if (m_stopConditionSupported)
        result.stopCondition = m_ui->stopConditionComboBox->currentData().toString();
    result.seedMode = m_ui->seedModeCheckBox->isChecked();
    result.sequential = m_ui->sequentialCheckBox->isChecked();
    result.firstLastPiece = m_ui->firstLastCheckBox->isChecked();
    const QStringList layouts {
        QStringLiteral("Original"),
        QStringLiteral("Subfolder"),
        QStringLiteral("NoSubfolder")
    };
    result.contentLayout = layouts.value(m_ui->contentLayoutComboBox->currentIndex(),
                                          QStringLiteral("Original"));
    return result;
}

bool RemoteAddTorrentDialog::keepOriginalTorrentFiles() const
{
    return m_ui->doNotDeleteTorrentCheckBox->isChecked();
}

QString RemoteAddTorrentDialog::links() const
{
    return m_linkInput ? m_linkInput->toPlainText().trimmed() : QString();
}

void RemoteAddTorrentDialog::accept()
{
    if (m_defaultsLoading)
        return;
    // A connection can disappear while this modal dialog is open; never
    // silently fall back to whichever main-window tab is selected now.
    if (m_serverIsConnected && !m_serverIsConnected(m_server.id))
    {
        QMessageBox::warning(this, tr("Server disconnected"),
                             tr("%1 is no longer connected. Select a connected "
                                "server or reconnect before adding this torrent.")
                                 .arg(m_server.name));
        return;
    }

    if (m_source == Source::Links)
    {
        QStringList urls;
        for (const QString &line : links().split(QLatin1Char('\n')))
        {
            if (line.trimmed().isEmpty()) continue;
            if (!validLink(line.trimmed()))
            {
                QMessageBox::warning(this, tr("Invalid torrent link"),
                    tr("Provide valid magnet, HTTP or HTTPS URLs."));
                return;
            }
            urls.append(line.trimmed());
        }
        if (urls.isEmpty())
        {
            QMessageBox::warning(this, tr("No torrent links"),
                                 tr("Enter at least one torrent link."));
            return;
        }
    }
    else if (m_source == Source::TorrentFiles && m_batchFiles.size() < 2)
    {
        QMessageBox::warning(this, tr("Invalid torrent files"),
                             tr("Select at least two .torrent files for a batch."));
        return;
    }
    else if (m_source == Source::TorrentFile
             && (m_fileContents.isEmpty() || m_fileContents.size() > 32 * 1024 * 1024))
    {
        QMessageBox::warning(this, tr("Invalid torrent file"),
                             tr("The selected torrent metadata is empty or exceeds 32 MiB."));
        return;
    }

    if (m_ui->comboTMM->currentIndex() == 0)
    {
        QString path, error;
        if (!validateServerPath(m_ui->savePath, &path, &error))
        {
            QMessageBox::warning(this, tr("Invalid remote destination"), error);
            return;
        }
        if (m_ui->groupBoxDownloadPath->isChecked())
        {
            if (!validateServerPath(m_ui->downloadPath, &path, &error)
                || path.isEmpty())
            {
                QMessageBox::warning(this, tr("Invalid remote incomplete path"),
                                     error.isEmpty()
                                         ? tr("Enter the remote incomplete-download folder.")
                                         : error);
                return;
            }
        }
    }

    if (m_ui->comboTMM->currentIndex() == 0)
    {
        // Unchecking Remember must clear any previously stored path;
        // otherwise a later dialog silently prefills an obsolete destination.
        const QString remote = resolveServerPath(m_ui->savePath->currentText());
        rememberPath(remote, false);
        if (m_ui->groupBoxDownloadPath->isChecked())
            rememberPath(resolveServerPath(m_ui->downloadPath->currentText()), true);
        if (!m_ui->checkBoxRememberLastSavePath->isChecked() || remote.isEmpty())
            m_settings.remove(savedPathKey());
        else
            m_settings.setValue(savedPathKey(), remote);
    }
    if (m_ui->checkBoxNeverShow->isChecked())
        m_settings.setValue(QStringLiteral("Preferences/Downloads/ShowAdditionDialog"), false);
    QDialog::accept();
}

void RemoteAddTorrentDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    if (m_settings.value(QStringLiteral("Preferences/Downloads/AdditionDialogFront"), true).toBool())
    {
        activateWindow();
        raise();
    }
}

void RemoteAddTorrentDialog::done(int result)
{
    m_settings.setValue(QStringLiteral("Layout/Dialogs/AddTorrent/Splitter"),
                        m_ui->splitter->saveState());
    m_settings.setValue(QStringLiteral("Layout/Dialogs/AddTorrent/SplitterVersion"), 2);
    QDialog::done(result);
}
