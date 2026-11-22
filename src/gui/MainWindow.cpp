// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/MainWindow.h"

#include <QAction>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSplitter>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableView>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(tr("qBittorrent Remote"));
    resize(1280, 800);

    setupMenus();
    setupMainArea();
    statusBar()->showMessage(tr("Not connected to a server"));
}

void MainWindow::setupMenus()
{
    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
    QAction *quitAction = fileMenu->addAction(tr("&Quit"));
    connect(quitAction, &QAction::triggered, this, [this]() { close(); });

    QMenu *helpMenu = menuBar()->addMenu(tr("&Help"));
    QAction *aboutAction = helpMenu->addAction(tr("&About"));
    connect(aboutAction, &QAction::triggered, this, [this]() {
        QMessageBox::about(this, tr("About qBittorrent Remote"),
            tr("A standalone Qt desktop client for remote qBittorrent instances.\n"
               "This bootstrap does not yet connect to a server."));
    });
}

void MainWindow::setupMainArea()
{
    auto *outerSplitter = new QSplitter(Qt::Horizontal, this);
    auto *statusList = new QListWidget(outerSplitter);
    statusList->addItems({
        tr("All"), tr("Downloading"), tr("Seeding"), tr("Completed"),
        tr("Paused"), tr("Stalled"), tr("Errored")
    });
    statusList->setCurrentRow(0);
    statusList->setMinimumWidth(150);
    statusList->setMaximumWidth(250);

    auto *verticalSplitter = new QSplitter(Qt::Vertical, outerSplitter);
    auto *torrentTable = new QTableView(verticalSplitter);
    auto *emptyModel = new QStandardItemModel(torrentTable);
    emptyModel->setHorizontalHeaderLabels({
        tr("Name"), tr("Size"), tr("Done"), tr("Status"),
        tr("Seeds"), tr("Peers"), tr("DL Speed"), tr("UP Speed"),
        tr("Ratio"), tr("ETA"), tr("Category"), tr("Tags")
    });
    torrentTable->setModel(emptyModel);
    torrentTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    torrentTable->setSelectionMode(QAbstractItemView::SingleSelection);
    torrentTable->setSortingEnabled(true);
    torrentTable->horizontalHeader()->setStretchLastSection(false);
    torrentTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);

    auto *details = new QTabWidget(verticalSplitter);
    for (const QString &tab : {
        tr("General"), tr("Trackers"), tr("Peers"), tr("Content")
    })
    {
        auto *placeholder = new QLabel(tr("Select a torrent to view details."), details);
        placeholder->setAlignment(Qt::AlignCenter);
        details->addTab(placeholder, tab);
    }

    verticalSplitter->addWidget(torrentTable);
    verticalSplitter->addWidget(details);
    verticalSplitter->setStretchFactor(0, 3);
    verticalSplitter->setStretchFactor(1, 1);

    outerSplitter->addWidget(statusList);
    outerSplitter->addWidget(verticalSplitter);
    outerSplitter->setStretchFactor(0, 0);
    outerSplitter->setStretchFactor(1, 1);

    setCentralWidget(outerSplitter);
}
