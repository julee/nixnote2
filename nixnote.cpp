/*********************************************************************************
NixNote - An open-source client for the Evernote service.
Copyright (C) 2013 Randy Baumgarte

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
***********************************************************************************/

#include "nixnote.h"
#include "threads/syncrunner.h"
#include "gui/nwebview.h"
#include "watcher/filewatcher.h"
#include "dialog/accountdialog.h"
#include "dialog/preferences/preferencesdialog.h"
#include "sql/resourcetable.h"
#include "sql/nsqlquery.h"
#include "dialog/logviewer.h"
#include "filters/filtercriteria.h"
#include "filters/filterengine.h"
#include "dialog/faderdialog.h"
#include "dialog/shortcutdialog.h"
#include "utilities/noteindexer.h"

#include <QApplication>
#include <QThread>
#include <QLabel>
#include <QMessageBox>
#include <QFileDialog>
#include <QStringList>
#include <QDesktopServices>
#include <QPrintDialog>
#include <QPrintPreviewDialog>
#include <QStatusBar>
#include <QSlider>
#include <QPrinter>
#include <QDesktopWidget>
#include <QFileIconProvider>
#include <QSplashScreen>
#include <unistd.h>

#include "sql/notetable.h"
#include "dialog/screencapture.h"
#include "gui/ntabwidget.h"
#include "sql/notebooktable.h"
#include "sql/usertable.h"
#include "settings/startupconfig.h"
#include "dialog/logindialog.h"
#include "dialog/closenotebookdialog.h"
#include "gui/lineedit.h"
#include "gui/findreplace.h"
#include "gui/nattributetree.h"
#include "dialog/watchfolderdialog.h"
#include "dialog/notehistoryselect.h"
#include "gui/ntrashtree.h"
#include "html/attachmenticonbuilder.h"
#include "filters/filterengine.h"
#include "global.h"
#include "html/enmlformatter.h"
//#include "oauth/oauthwindow.h"
//#include "oauth/oauthtokenizer.h"
#include "dialog/databasestatus.h"
#include "dialog/adduseraccountdialog.h"
#include "dialog/accountmaintenancedialog.h"
#include "communication/communicationmanager.h"
#include "utilities/encrypt.h"

// Windows Check
#ifndef _WIN32
#include <boost/shared_ptr.hpp>
#endif

#include "cmdtools/cmdlinequery.h"
#include "cmdtools/alternote.h"


#include "gui/nmainmenubar.h"
#include "dialog/logindialog.h"
#include "xml/importdata.h"
#include "xml/importenex.h"
#include "xml/exportdata.h"
#include "dialog/aboutdialog.h"

#include "qevercloud/include/QEverCloudOAuth.h"

using namespace qevercloud;

// Windows Check
#ifndef _WIN32
using namespace boost;
#endif

extern Global global;
class SyncRunner;

//*************************************************
//* This is the main class that is used to start
//* everything else.
//*************************************************
NixNote::NixNote(QWidget *parent) : QMainWindow(parent)
{
    splashScreen = new QSplashScreen(this, global.getPixmapResource(":splashLogoImage"));
    global.settings->beginGroup("Appearance");
    if(global.settings->value("showSplashScreen", false).toBool()) {
        splashScreen->setWindowFlags( Qt::WindowStaysOnTopHint | Qt::SplashScreen | Qt::FramelessWindowHint );
        splashScreen->show();
        QTimer::singleShot(2500, splashScreen, SLOT(close()));
    }
    global.settings->endGroup();

#if QT_VERSION < 0x050000
    QTextCodec::setCodecForCStrings(QTextCodec::codecForName("UTF-8"));
#endif
    global.setDebugLevel();

    // Load any plugins
    this->loadPlugins();

    QTranslator *nixnoteTranslator = new QTranslator();
    QLOG_DEBUG() << "Looking for transaltions: " << global.fileManager.getTranslateFilePath("nixnote2_" + QLocale::system().name() + ".qm");
    nixnoteTranslator->load(global.fileManager.getTranslateFilePath("nixnote2_" + QLocale::system().name() + ".qm"));
    QApplication::instance()->installTranslator(nixnoteTranslator);

    connect(&syncThread, SIGNAL(started()), this, SLOT(syncThreadStarted()));
    connect(&counterThread, SIGNAL(started()), this, SLOT(counterThreadStarted()));
    connect(&indexThread, SIGNAL(started()), this, SLOT(indexThreadStarted()));
    counterThread.start(QThread::LowestPriority);
    syncThread.start(QThread::LowPriority);
    indexThread.start(QThread::LowestPriority);
    this->thread()->setPriority(QThread::HighestPriority);

    heartbeatTimer.setInterval(1000);
    heartbeatTimer.setSingleShot(false);
    connect(&heartbeatTimer, SIGNAL(timeout()), this, SLOT(heartbeatTimerTriggered()));
    heartbeatTimer.start();

    QFont f = this->font();
    //f.setPointSize(8);
    global.getGuiFont(f);
    this->setFont(f);

    db = new DatabaseConnection("nixnote");  // Startup the database

    // Setup the sync thread
    QLOG_TRACE() << "Setting up counter thread";
    connect(this, SIGNAL(updateCounts()), &counterRunner, SLOT(countAll()));

    // Setup the counter thread
    QLOG_TRACE() << "Setting up sync thread";
    connect(this,SIGNAL(syncRequested()),&syncRunner,SLOT(synchronize()));
    connect(&syncRunner, SIGNAL(setMessage(QString, int)), this, SLOT(setMessage(QString, int)));

    QLOG_TRACE() << "Setting up GUI";
    global.filterPosition = 0;
    this->setupGui();

    global.resourceWatcher = new QFileSystemWatcher(this);
    QLOG_TRACE() << "Connecting signals";
    connect(favoritesTreeView, SIGNAL(updateSelectionRequested()), this, SLOT(updateSelectionCriteria()));
    connect(tagTreeView, SIGNAL(updateSelectionRequested()), this, SLOT(updateSelectionCriteria()));
    connect(notebookTreeView, SIGNAL(updateSelectionRequested()), this, SLOT(updateSelectionCriteria()));
    connect(searchTreeView, SIGNAL(updateSelectionRequested()), this, SLOT(updateSelectionCriteria()));
    connect(attributeTree, SIGNAL(updateSelectionRequested()), this, SLOT(updateSelectionCriteria()));
    connect(trashTree, SIGNAL(updateSelectionRequested()), this, SLOT(updateSelectionCriteria()));
    connect(searchText, SIGNAL(updateSelectionRequested()), this, SLOT(updateSelectionCriteria()));
    connect(global.resourceWatcher, SIGNAL(fileChanged(QString)), this, SLOT(resourceExternallyUpdated(QString)));

    hammer = new Thumbnailer(global.db);
    hammer->startTimer();
    finalSync = false;


    // Setup reminders
    global.reminderManager = new ReminderManager();
    connect(global.reminderManager, SIGNAL(showMessage(QString,QString,int)), this, SLOT(showMessage(QString,QString,int)));
    global.reminderManager->reloadTimers();

    global.settings->beginGroup("Appearance");
    bool showMissed = global.settings->value("showMissedReminders", false).toBool();
    global.settings->endGroup();
    if (showMissed)
        QTimer::singleShot(5000, global.reminderManager, SLOT(timerPop()));
    else
        global.setLastReminderTime(QDateTime::currentMSecsSinceEpoch());


    // Check for Java and verify encryption works
    QString test = "Test Message";
    QString  result;
    EnCrypt encrypt;
    if (!encrypt.encrypt(result, test, test)) {
        if (!encrypt.decrypt(result, result, test)) {
            if (result == test)
                global.javaFound = true;
        }
    }

    // Check if Tidy is installed
    QProcess tidyProcess;
    tidyProcess.start("tidy -h");
    tidyProcess.waitForFinished();
    if (tidyProcess.exitCode()) {
        QMessageBox mb;
        mb.critical(this, tr("Tidy Not Found"), tr("Tidy is not found on this system.\nUntil tidy is installed you cannot save any notes."));
    }

    if (global.startupNewNote) {
        this->showMinimized();
        this->newExternalNote();
    }

    //this->openNote(false);

    // Init OAuth winwod
    //oauthWindow = NULL;

    //QDesktopServices::setUrlHandler("evernote", this, "showDesktopUrl");
    remoteQuery = new RemoteQuery();

    // Initialize pdfExportWindow to null. We don't fully set this up in case the person requests it.
    pdfExportWindow = NULL;

    // Setup file watcher
    importManager = new FileWatcherManager(this);
    connect(importManager, SIGNAL(fileImported(qint32,qint32)), this, SLOT(updateSelectionCriteria()));
    connect(importManager, SIGNAL(fileImported()), this, SLOT(updateSelectionCriteria()));
    importManager->setup();
    this->updateSelectionCriteria(true);  // This is only needed in case we imported something at statup.
    QLOG_DEBUG() << "Exiting NixNote constructor";
}





// Destructor to call when all done
NixNote::~NixNote()
{
    delete splashScreen;
    syncThread.quit();
    indexThread.quit();
    counterThread.quit();
    while (!syncThread.isFinished());
    while (!indexThread.isFinished());
    while(!counterThread.isFinished());

    // Cleanup any temporary files
    if (global.purgeTemporaryFilesOnShutdown) {
        QDir myDir(global.fileManager.getTmpDirPath());
        QStringList list = myDir.entryList();
        for (int i=0; i<list.size(); i++) {
            if (list[i] != "." && list[i] != "..") {
                QString file = global.fileManager.getTmpDirPath()+ list[i];
                myDir.remove(file);
            }
        }
    }
//    delete db;  // Free up memory used by the database connection
//    delete rightPanelSplitter;
//    delete leftPanelSplitter;
//    delete leftPanel;
}



//****************************************************************
//* Setup the user interface
//****************************************************************
void NixNote::setupGui() {
    // Setup the GUI
    //this->setStyleSheet("background-color: white;");
    //statusBar();    setWindowTitle(tr("NixNote 2"));
    setWindowIcon(QIcon(global.getIconResource(":windowIcon")));

    //QLOG_TRACE() << "Setting up menu bar";
    searchText = new LineEdit();
    menuBar = new NMainMenuBar(this);
    setMenuBar(menuBar);

    QLOG_TRACE() << "Setting up tool bar";
    toolBar = addToolBar(tr("ToolBar"));
    connect(toolBar, SIGNAL(visibilityChanged(bool)), this, SLOT(toolbarVisibilityChanged()));
    //menuBar = new NMainMenuBar(this);
    toolBar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toolBar->setObjectName("toolBar");
    //toolBar->addWidget(menuBar);
    menuBar->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
    toolBar->setFloatable(true);
    toolBar->setMovable(true);
    toolBar->setFont(global.getGuiFont(toolBar->font()));
    toolBar->setAllowedAreas(Qt::BottomToolBarArea | Qt::TopToolBarArea);
    //toolBar->addSeparator();
    leftArrowButton = toolBar->addAction(global.getIconResource(":leftArrowIcon"), tr("Back"));
    rightArrowButton = toolBar->addAction(global.getIconResource(":rightArrowIcon"), tr("Next"));

    leftArrowButton->setEnabled(false);
    leftArrowButton->setPriority(QAction::LowPriority);
    connect(leftArrowButton, SIGNAL(triggered(bool)),this, SLOT(leftButtonTriggered()));
    rightArrowButton->setEnabled(false);
    rightArrowButton->setPriority(QAction::LowPriority);
    connect(rightArrowButton, SIGNAL(triggered(bool)),this, SLOT(rightButtonTriggered()));
    toolBar->addSeparator();

    homeButton = toolBar->addAction(global.getIconResource(":homeIcon"), tr("All Notes"));
//    homeButton->setPriority(QAction::LowPriority);   Hide the text by the icon
    syncButton = toolBar->addAction(global.getIconResource(":synchronizeIcon"), tr("Sync"));
//  syncButton->setPriority(QAction::LowPriority);   // Hide the text by the icon
    toolBar->addSeparator();
    printNoteButton = toolBar->addAction(global.getIconResource(":printerIcon"), tr("Print"));
    printNoteButton->setToolTip(tr("Print the current note"));

    emailButton = toolBar->addAction(global.getIconResource(":emailIcon"), tr("Email"));
    emailButton->setToolTip(tr("Email the current note"));

    noteButton = new QToolButton();
    toolBar->addSeparator();
    newNoteButton = new QAction(noteButton);
    newNoteButton->setIcon(global.getIconResource(":newNoteIcon"));
    newNoteButton->setText(tr("New Text Note"));
    newWebcamNoteButton = new QAction(noteButton);
    newWebcamNoteButton->setIcon(global.getIconResource(":webcamIcon"));
    newWebcamNoteButton->setText(tr("New Webcam Note"));
    noteButton->addAction(newNoteButton);
    noteButton->addAction(newWebcamNoteButton);
    noteButton->setText(newNoteButton->text());
    noteButton->setIcon(newNoteButton->icon());
    noteButton->setProperty("currentNoteButton", NewTextNote);
    noteButton->setPopupMode(QToolButton::MenuButtonPopup);
    noteButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    connect(noteButton, SIGNAL(clicked()), this, SLOT(noteButtonClicked()));
    toolBar->addWidget(noteButton);
    toolBar->addSeparator();

    deleteNoteButton = toolBar->addAction(global.getIconResource(":deleteIcon"), tr("Delete"));

    toolBar->addSeparator();
    trunkButton = toolBar->addAction(global.getIconResource(":trunkIcon"), tr("Trunk"));
    trunkButton->setVisible(false);
    usageButton = toolBar->addAction(global.getIconResource(":usageIcon"), tr("Usage"));


    connect(syncButton,SIGNAL(triggered()), this, SLOT(synchronize()));
    connect(homeButton, SIGNAL(triggered()), this, SLOT(resetView()));
    connect(printNoteButton, SIGNAL(triggered()), this, SLOT(fastPrintNote()));
    connect(deleteNoteButton, SIGNAL(triggered()), this, SLOT(deleteCurrentNote()));
    connect(trunkButton, SIGNAL(triggered()), this, SLOT(openTrunk()));
    connect(newNoteButton, SIGNAL(triggered()), this, SLOT(newNote()));
    connect(newWebcamNoteButton, SIGNAL(triggered()), this, SLOT(newWebcamNote()));
    connect(newNoteButton, SIGNAL(triggered()), this, SLOT(noteButtonClicked()));
    connect(usageButton, SIGNAL(triggered()), this, SLOT(openAccount()));
    connect(emailButton, SIGNAL(triggered()), this, SLOT(emailNote()));

    QLOG_TRACE() << "Adding main splitter";
    mainSplitter = new QSplitter(Qt::Horizontal);
    setCentralWidget(mainSplitter);

    rightPanelSplitter = new QSplitter(Qt::Vertical);
    leftPanelSplitter = new QSplitter(Qt::Vertical);
    leftPanel = new WidgetPanel();

    this->setupNoteList();
    this->setupFavoritesTree();
    this->setupSynchronizedNotebookTree();
    this->setupTagTree();
    this->setupSearchTree();
    this->setupAttributeTree();
    this->setupTrashTree();
    this->setupTabWindow();
    leftPanel->vboxLayout->addStretch();

    connect(tagTreeView, SIGNAL(tagDeleted(qint32,QString)), favoritesTreeView, SLOT(itemExpunged(qint32, QString)));
    connect(searchTreeView, SIGNAL(searchDeleted(qint32)), favoritesTreeView, SLOT(itemExpunged(qint32)));
    connect(notebookTreeView, SIGNAL(notebookDeleted(qint32,QString)), favoritesTreeView, SLOT(itemExpunged(qint32, QString)));
    connect(tagTreeView, SIGNAL(tagRenamed(qint32,QString,QString)), favoritesTreeView, SLOT(itemRenamed(qint32, QString, QString)));
    connect(searchTreeView, SIGNAL(searchDeleted(qint32)), favoritesTreeView, SLOT(itemExpunged(qint32)));
    connect(notebookTreeView, SIGNAL(notebookRenamed(qint32,QString,QString)), favoritesTreeView, SLOT(itemRenamed(qint32, QString, QString)));
    connect(notebookTreeView, SIGNAL(stackDeleted(QString)), favoritesTreeView, SLOT(stackExpunged(QString)));
    connect(notebookTreeView, SIGNAL(stackRenamed(QString,QString)), favoritesTreeView, SLOT(stackRenamed(QString, QString)));
    connect(tabWindow, SIGNAL(updateNoteTitle(QString,qint32,QString)), favoritesTreeView, SLOT(updateShortcutName(QString,qint32,QString)));

    QLOG_TRACE() << "Setting up left panel";
    leftPanel->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
    leftScroll = new QScrollArea();
    leftScroll->setWidgetResizable(true);
    leftScroll->setWidget(leftPanel);
    leftScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    leftScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    mainSplitter->insertWidget(0,leftScroll);
    mainSplitter->addWidget(rightPanelSplitter);
    mainSplitter->setStretchFactor(0,1);
    mainSplitter->setStretchFactor(1,3);

    QLOG_TRACE() << "Resetting left side widgets";
    tagTreeView->resetSize();
    searchTreeView->resetSize();
    attributeTree->resetSize();
    trashTree->resetSize();

    // Restore the window state
    global.startMinimized = false;
    QLOG_TRACE() << "Restoring window state";
    global.settings->beginGroup("Appearance");
    int selectionBehavior = global.settings->value("startupNotebook", AppearancePreferences::UseLastViewedNotebook).toInt();
    global.startMinimized = global.settings->value("startMinimized", false).toBool();
    global.settings->endGroup();

    global.settings->beginGroup("SaveState");
    bool showStatusbar = global.settings->value("statusBar", true).toBool();
    if (showStatusbar) {
        menuBar->viewStatusbar->setChecked(showStatusbar);
        statusBar()->setVisible(true);
    } else {
        menuBar->viewStatusbar->setChecked(false);
        statusBar()->setVisible(false);
    }
    restoreState(global.settings->value("WindowState").toByteArray());
    restoreGeometry(global.settings->value("WindowGeometry").toByteArray());
    mainSplitter->restoreState(global.settings->value("mainSplitter", 0).toByteArray());
    rightPanelSplitter->restoreState(global.settings->value("rightSplitter", 0).toByteArray());
    if (global.settings->value("isMaximized", false).toBool())
        this->setWindowState(Qt::WindowMaximized);
    QString lidListString = global.settings->value("openTabs", "").toString().trimmed();
    bool value = global.settings->value("leftPanelVisible", true).toBool();
    if (!value) {
        menuBar->viewLeftPanel->setChecked(false);
        leftScroll->setVisible(false);
    }
    value = global.settings->value("noteListVisible", true).toBool();
    if (!value) {
        menuBar->viewNoteList->setChecked(false);
        topRightWidget->setVisible(false);
    }
    value = global.settings->value("tabWindowVisible", true).toBool();
    if (!value) {
        menuBar->viewNotePanel->setChecked(false);
        tabWindow->setVisible(false);
    }
    value = global.settings->value("favoritesTreeVisible", true).toBool();
    if (!value) {
        menuBar->viewFavoritesTree->setChecked(false);
        favoritesTreeView->setVisible(false);
    }
    value = global.settings->value("notebookTreeVisible", true).toBool();
    if (!value) {
        menuBar->viewNotebookTree->setChecked(false);
        notebookTreeView->setVisible(false);
    }
    value = global.settings->value("tagTreeVisible", true).toBool();
    if (!value) {
        menuBar->viewTagTree->setChecked(false);
        tagTreeView->setVisible(false);
    }
    value = global.settings->value("savedSearchTreeVisible", true).toBool();
    if (!value) {
        menuBar->viewSearchTree->setChecked(false);
        searchTreeView->setVisible(false);
    }
    value = global.settings->value("attributeTreeVisible", true).toBool();
    if (!value) {
        menuBar->viewAttributesTree->setChecked(false);
        attributeTree->setVisible(false);
    }
    value = global.settings->value("trashTreeVisible", true).toBool();
    if (!value) {
        menuBar->viewTrashTree->setChecked(false);
        trashTree->setVisible(false);
    }
    global.settings->endGroup();
    checkLeftPanelSeparators();

    if (rightPanelSplitter->orientation() == Qt::Vertical)
        viewNoteListWide();
    else
        viewNoteListNarrow();

    QStringList lidList = lidListString.split(' ');
    // If we have old notes we were viewing the last time
    if (lidList.size() > 0) {
        FilterCriteria *filter = global.filterCriteria[global.filterPosition];

        for (int i=0; i<lidList.size(); i++) {
            // if we are doing multiple notes, they each need
            // to be added to the selection criteria.
            if (i>0)
                filter = new FilterCriteria();
            int lid = lidList[i].toInt();
            QList<qint32> selectedLids;
            selectedLids.append(lid);
            filter->setSelectedNotes(selectedLids);
            filter->setLid(lid);
            if (i>0)
                global.filterCriteria.append(filter);
        }

        for (int i=0; i<lidList.size(); i++) {
            global.filterPosition = i;
            if(i==0)
                openNote(false);
            else
                openNote(true);
        }
    }

    NoteTable noteTable(global.db);
    if (global.startupNote > 0 && noteTable.exists(global.startupNote)) {
        openExternalNote(global.startupNote);
    }

    // Setup the tray icon
    closeFlag = false;
    minimizeToTray = global.minimizeToTray();
    closeToTray = global.closeToTray();
    trayIcon = new QSystemTrayIcon(global.getIconResource(":trayIcon"), this);
    trayIconContextMenu = new TrayMenu(this);
    trayIconContextMenu->addAction(newNoteButton);

    newExternalNoteButton = trayIconContextMenu->addAction(tr("Quick Note"));
    connect(newExternalNoteButton, SIGNAL(triggered()), this, SLOT(newExternalNote()));

    screenCaptureButton = new QAction(tr("Screen Capture"), this);
    trayIconContextMenu->addAction(screenCaptureButton);
    screenCaptureButton->setIcon(global.getIconResource(":screenCaptureIcon"));
    noteButton->addAction(screenCaptureButton);
    connect(screenCaptureButton, SIGNAL(triggered()), this, SLOT(screenCapture()));

    trayIconContextMenu->addSeparator();
    QMenu *favoritesMenu = trayIconContextMenu->addMenu(tr("Shortcut Notes"));
    trayIconContextMenu->setActionMenu(TrayMenu::FavoriteNotesMenu, favoritesMenu);
    QMenu *pinnedMenu = trayIconContextMenu->addMenu(tr("Pinned Notes"));
    trayIconContextMenu->setActionMenu(TrayMenu::PinnedMenu, pinnedMenu);
    QMenu *recentMenu = trayIconContextMenu->addMenu(tr("Recently Updated Notes"));
    trayIconContextMenu->setActionMenu(TrayMenu::RecentMenu, recentMenu);
    connect(trayIconContextMenu, SIGNAL(openNote(qint32)), this, SLOT(openExternalNote(qint32)));
    trayIconContextMenu->addSeparator();

    showAction = trayIconContextMenu->addAction(tr("Show/Hide"));
    QLOG_DEBUG() << "QSystemTrayIcon::isSystemTrayAvailable():" << QSystemTrayIcon::isSystemTrayAvailable();
    if (!QSystemTrayIcon::isSystemTrayAvailable() && global.forceSystemTrayAvailable) {
        QLOG_INFO() << "Overriding QSystemTrayIcon::isSystemTrayAvailable() per command line option.";
    }
    if (!global.showTrayIcon() || global.forceNoStartMimized || (!QSystemTrayIcon::isSystemTrayAvailable() && !global.forceSystemTrayAvailable)) {
        QLOG_DEBUG() << "Overriding close & minimize to tray because of command line or isSystemTrayAvailable";
        closeToTray = false;
        minimizeToTray = false;
    }

    trayIconContextMenu->addSeparator();
    closeAction = trayIconContextMenu->addAction(tr("Close"));
    connect(closeAction, SIGNAL(triggered()), this, SLOT(closeNixNote()));
    connect(showAction, SIGNAL(triggered()), this, SLOT(toggleVisible()));

    trayIcon->setContextMenu(trayIconContextMenu);
    trayIcon->setVisible(global.showTrayIcon());
    if (trayIcon->isVisible())
        trayIcon->show();
    connect(trayIcon, SIGNAL(activated(QSystemTrayIcon::ActivationReason)), this, SLOT(trayActivated(QSystemTrayIcon::ActivationReason)));


    // Setup timers
    QLOG_TRACE() << "Setting up timers";
    setSyncTimer();
    connect(&syncTimer, SIGNAL(timeout()), this, SLOT(syncTimerExpired()));
    connect(&syncButtonTimer, SIGNAL(timeout()), this, SLOT(updateSyncButton()));
    connect(&syncRunner, SIGNAL(syncComplete()), this, SLOT(syncButtonReset()));

    QLOG_TRACE() << "Setting up more connections for tab windows & threads";
    // Setup so we refresh whenever the sync is done.
//    connect(&syncRunner, SIGNAL(syncComplete()), this, SLOT(updateSelectionCriteria(bool)));
    connect(&syncRunner, SIGNAL(syncComplete()), this, SLOT(notifySyncComplete()));

    // connect so we refresh the note list and counts whenever a note has changed
    connect(tabWindow, SIGNAL(noteUpdated(qint32)), noteTableView, SLOT(refreshData()));
    connect(tabWindow, SIGNAL(noteUpdated(qint32)), &counterRunner, SLOT(countNotebooks()));
    connect(tabWindow, SIGNAL(noteUpdated(qint32)), &counterRunner, SLOT(countTags()));
    connect(tabWindow, SIGNAL(noteTagsUpdated(QString, qint32, QStringList)), noteTableView, SLOT(noteTagsUpdated(QString, qint32, QStringList)));
    connect(tabWindow, SIGNAL(noteNotebookUpdated(QString, qint32, QString)), noteTableView, SLOT(noteNotebookUpdated(QString, qint32, QString)));
    connect(tabWindow, SIGNAL(updateNoteList(qint32, int, QVariant)), noteTableView, SLOT(refreshCell(qint32, int, QVariant)));
    connect(noteTableView, SIGNAL(refreshNoteContent(qint32)), tabWindow, SLOT(refreshNoteContent(qint32)));
    connect(noteTableView, SIGNAL(saveAllNotes()), tabWindow, SLOT(saveAllNotes()));

    // connect so we refresh the tag tree when a new tag is added
    connect(tabWindow, SIGNAL(tagCreated(qint32)), tagTreeView, SLOT(addNewTag(qint32)));
    connect(tabWindow, SIGNAL(tagCreated(qint32)), &counterRunner, SLOT(countTags()));

    connect(tabWindow, SIGNAL(updateSelectionRequested()), this, SLOT(updateSelectionCriteria()));
    connect(tabWindow->tabBar, SIGNAL(currentChanged(int)), this, SLOT(checkReadOnlyNotebook()));

    // Finish by filtering & displaying the data
    //updateSelectionCriteria();

    // connect signal on a tag rename
    connect(tagTreeView, SIGNAL(tagRenamed(qint32,QString,QString)), this, SLOT(updateSelectionCriteria()));
    connect(notebookTreeView, SIGNAL(notebookRenamed(qint32,QString,QString)), this, SLOT(updateSelectionCriteria()));

    // Reload saved selection criteria
    if (selectionBehavior != AppearancePreferences::UseAllNotebooks) {
        bool criteriaFound = false;
        FilterCriteria *criteria = new FilterCriteria();

        // Restore whatever they were looking at in the past
        if (selectionBehavior == AppearancePreferences::UseLastViewedNotebook) {

            global.settings->beginGroup("SaveState");
            qint32 notebookLid = global.settings->value("selectedNotebook", 0).toInt();
            if (notebookLid > 0 && notebookTreeView->dataStore[notebookLid] != NULL) {
                criteria->setNotebook(*notebookTreeView->dataStore[notebookLid]);
                criteriaFound = true;
            } else {
                QString selectedStack = global.settings->value("selectedStack", "").toString();
                if (selectedStack != "" && notebookTreeView->stackStore[selectedStack] != NULL) {
                    criteria->setNotebook(*notebookTreeView->stackStore[selectedStack]);
                    criteriaFound = true;
                }
            }

            QString prevSearch = global.settings->value("searchString", "").toString();
            if (prevSearch != "") {
                searchText->setText(prevSearch);
                criteria->setSearchString(prevSearch);
                criteriaFound = true;
            }

            qint32 searchLid = global.settings->value("selectedSearch", 0).toInt();
            if (searchLid > 0 && searchTreeView->dataStore[searchLid] != NULL) {
                criteria->setSavedSearch(*searchTreeView->dataStore[searchLid]);
                criteriaFound = true;
            }

            QString selectedTags = global.settings->value("selectedTags", "").toString();
            if (selectedTags != "") {
                QStringList tags = selectedTags.split(" ");
                QList<QTreeWidgetItem *> items;
                for (int i=0; i<tags.size(); i++) {
                    if (tagTreeView->dataStore[tags[i].toInt()] != NULL)
                        items.append(tagTreeView->dataStore[tags[i].toInt()]);
                }
                criteriaFound = true;
                criteria->setTags(items);
            }

            global.settings->endGroup();
        }

        // Select the default notebook
        if (selectionBehavior == AppearancePreferences::UseDefaultNotebook) {
            NotebookTable ntable(global.db);
            qint32 lid = ntable.getDefaultNotebookLid();
            if (notebookTreeView->dataStore[lid] != NULL) {
                criteria->setNotebook(*notebookTreeView->dataStore[lid]);
                criteriaFound = true;
            }
        }



        // If we have some filter criteria, save it.  Otherwise delete
        // the unused memory.
        if (criteriaFound) {
            global.filterPosition++;
            global.appendFilter(criteria);
        } else
            delete criteria;
    }

    this->updateSelectionCriteria();
    // Set default focuse to the editor window
    tabWindow->currentBrowser()->editor->setFocus();

    QStringList accountNames = global.accountsManager->nameList();
    QList<int> ids = global.accountsManager->idList();
    for (int i=0; i<ids.size(); i++) {
        if (ids[i] == global.accountsManager->currentId) {
            setWindowTitle("NixNote - " +accountNames[i]);
            i=ids.size();
        }
    }

    // Determine if we should start minimized
    QLOG_DEBUG() << "isSystemTrayAvailable:" << QSystemTrayIcon::isSystemTrayAvailable();
    if (global.startMinimized && !global.forceNoStartMimized && (QSystemTrayIcon::isSystemTrayAvailable()||global.forceSystemTrayAvailable)) {
        this->setWindowState(Qt::WindowMinimized);
        if (minimizeToTray)
            QTimer::singleShot(100,this, SLOT(hide()));
    }
    if (global.forceStartMinimized) {
        this->setWindowState(Qt::WindowMinimized);
        if (minimizeToTray)
            QTimer::singleShot(100,this, SLOT(hide()));
    }

    // Restore expanded tags & stacks
    global.settings->beginGroup("SaveState");
    QString expandedTags = global.settings->value("expandedTags", "").toString();
    if (expandedTags != "") {
        QStringList tags = expandedTags.split(" ");
        for (int i=0; i<tags.size(); i++) {
            NTagViewItem *item;
            item = tagTreeView->dataStore[tags[i].toInt()];
            if (item != NULL)
                item->setExpanded(true);
        }
    }
    QString expandedNotebooks = global.settings->value("expandedStacks", "").toString();
    if (expandedNotebooks != "") {
        QStringList books = expandedNotebooks.split(" ");
        for (int i=0; i<books.size(); i++) {
            NNotebookViewItem *item;
            item = notebookTreeView->dataStore[books[i].toInt()];
            if (item != NULL && item->stack != "" && item->parent() != NULL) {
                item->parent()->setExpanded(true);
                //QLOG_DEBUG() << "Parent of " << books[i] << " expanded.";
            }
        }
    }

    searchTreeView->root->setExpanded(true);
    QString collapsedTrees = global.settings->value("collapsedTrees", "").toString();
    if (collapsedTrees != "") {
        QStringList trees = collapsedTrees.split(" ");
        for (int i=0; i<trees.size(); i++) {
            QString item = trees[i].toLower();
            if (item=="favorites")
                this->favoritesTreeView->root->setExpanded(false);
            if (item=="notebooks")
                this->notebookTreeView->root->setExpanded(false);
            if (item=="tags")
                this->tagTreeView->root->setExpanded(false);
            if (item=="attributes")
                this->attributeTree->root->setExpanded(false);
            if (item=="savedsearches")
                this->searchTreeView->root->setExpanded(false);
        }
    }
    global.settings->endGroup();


    // Setup application-wide shortcuts
    focusSearchShortcut = new QShortcut(this);
    focusSearchShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    this->setupShortcut(focusSearchShortcut, "Focus_Search");
    connect(focusSearchShortcut, SIGNAL(activated()), searchText, SLOT(setFocus()));

    fileSaveShortcut = new QShortcut(this);
    fileSaveShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    this->setupShortcut(fileSaveShortcut, "File_Save_Content");
    connect(fileSaveShortcut, SIGNAL(activated()), tabWindow, SLOT(saveAllNotes()));

    focusTitleShortcut = new QShortcut(this);
    focusTitleShortcut->setContext(Qt::WidgetShortcut);
    this->setupShortcut(focusTitleShortcut, "Focus_Title");
    connect(focusTitleShortcut, SIGNAL(activated()), &tabWindow->currentBrowser()->noteTitle, SLOT(setFocus()));

    focusNoteShortcut = new QShortcut(this);
    focusNoteShortcut->setContext(Qt::WidgetShortcut);
    this->setupShortcut(focusNoteShortcut, "Focus_Note");
    connect(focusNoteShortcut, SIGNAL(activated()), tabWindow->currentBrowser()->editor, SLOT(setFocus()));

    copyNoteUrlShortcut = new QShortcut(this);
    copyNoteUrlShortcut->setContext(Qt::WidgetShortcut);
    this->setupShortcut(copyNoteUrlShortcut, "Edit_Copy_Note_Url");
    connect(copyNoteUrlShortcut, SIGNAL(activated()), tabWindow->currentBrowser(), SLOT(copyNoteUrl()));

    focusTagShortcut = new QShortcut(this);
    focusTagShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    this->setupShortcut(focusTagShortcut, "Focus_Tag");
    connect(focusTagShortcut, SIGNAL(activated()), tabWindow->currentBrowser(), SLOT(newTagFocusShortcut()));

    focusUrlShortcut = new QShortcut(this);
    focusUrlShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    this->setupShortcut(focusUrlShortcut, "Focus_Url");
    connect(focusUrlShortcut, SIGNAL(activated()), tabWindow->currentBrowser(), SLOT(urlFocusShortcut()));

    focusAuthorShortcut = new QShortcut(this);
    focusAuthorShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    this->setupShortcut(focusAuthorShortcut, "Focus_Author");
    connect(focusAuthorShortcut, SIGNAL(activated()), tabWindow->currentBrowser(), SLOT(authorFocusShortcut()));

    focusNotebookShortcut = new QShortcut(this);
    focusNotebookShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    this->setupShortcut(focusNotebookShortcut, "Focus_Notebook");
    connect(focusNotebookShortcut, SIGNAL(activated()), tabWindow->currentBrowser(), SLOT(notebookFocusShortcut()));

    focusFontShortcut = new QShortcut(this);
    focusFontShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    this->setupShortcut(focusFontShortcut, "Focus_Font");
    connect(focusFontShortcut, SIGNAL(activated()), tabWindow->currentBrowser(), SLOT(fontFocusShortcut()));

    focusFontSizeShortcut = new QShortcut(this);
    focusFontSizeShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    this->setupShortcut(focusFontSizeShortcut, "Focus_Font_Size");
    connect(focusFontSizeShortcut, SIGNAL(activated()), tabWindow->currentBrowser(), SLOT(fontSizeFocusShortcut()));

    nextTabShortcut = new QShortcut(this);
    nextTabShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    this->setupShortcut(nextTabShortcut, "Next_Tab");
    connect(nextTabShortcut, SIGNAL(activated()), tabWindow, SLOT(nextTab()));

    prevTabShortcut = new QShortcut(this);
    prevTabShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    this->setupShortcut(prevTabShortcut, "Prev_Tab");
    connect(prevTabShortcut, SIGNAL(activated()), tabWindow, SLOT(prevTab()));

    closeTabShortcut = new QShortcut(this);
    closeTabShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    this->setupShortcut(closeTabShortcut, "Close_Tab");
    connect(closeTabShortcut, SIGNAL(activated()), tabWindow, SLOT(closeTab()));

    downNoteShortcut = new QShortcut(this);
    downNoteShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    this->setupShortcut(downNoteShortcut, "Down_Note");
    connect(downNoteShortcut, SIGNAL(activated()), noteTableView, SLOT(downNote()));

    upNoteShortcut = new QShortcut(this);
    upNoteShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    this->setupShortcut(upNoteShortcut, "Up_Note");
    connect(upNoteShortcut, SIGNAL(activated()), noteTableView, SLOT(upNote()));


    // Disable menu items when plugins are not available
    if (!webcamPluginAvailable) {
        newWebcamNoteButton->setVisible(false);
        this->menuBar->newWebcamNoteAction->setVisible(false);
        this->menuBar->newWebcamNoteAction->setEnabled(false);
    }

    // startup the index timer (if needed)
    if (global.enableIndexing) {
        indexTimer.setInterval(global.minimumThumbnailInterval);
        connect(&indexTimer, SIGNAL(timeout()), &indexRunner, SLOT(index()));
        connect(&indexRunner, SIGNAL(indexDone(bool)), this, SLOT(indexFinished(bool)));
        indexTimer.start();
    }
}


void NixNote::indexFinished(bool finished) {
    indexTimer.stop();
    if (!finished)
        indexTimer.setInterval(global.minIndexInterval);
    else
        indexTimer.setInterval(global.maxIndexInterval);
    indexTimer.start();
}





//**************************************************************
//* Move sync, couter, & index objects to their appropriate
//* thread.
//**************************************************************
void NixNote::counterThreadStarted() {
    counterRunner.moveToThread(&counterThread);
}




//***************************************************************
//* Signal received when the syncRunner thread has started
//***************************************************************
void NixNote::syncThreadStarted() {
    syncRunner.moveToThread(&syncThread);
    global.settings->beginGroup("Sync");
    bool syncOnStartup = global.settings->value("syncOnStartup", false).toBool();
    global.showGoodSyncMessagesInTray = global.settings->value("showGoodSyncMessagesInTray", true).toBool();
    global.settings->endGroup();
    if (syncOnStartup)
        synchronize();
}

void NixNote::indexThreadStarted() {
    indexRunner.moveToThread(&indexThread);
    indexRunner.initialize();
    global.indexRunner = &indexRunner;
}




//******************************************************************************
//* This function sets up the note list window.  This is what the users select
//* view a specific note
//******************************************************************************
void NixNote::setupNoteList() {
    QLOG_TRACE() << "Starting NixNote.setupNoteList()";

    // Setup a generic widget to hold the search & note table
    topRightWidget  = new QWidget(this);
    topRightLayout = new QVBoxLayout();
    topRightLayout->addWidget(searchText);
    topRightWidget->setLayout(topRightLayout);
    noteTableView = new NTableView();
    topRightLayout->addWidget(noteTableView);
    topRightLayout->setContentsMargins(QMargins(0,0,0,0));

    // Add the generic widget
    if (global.listView == Global::ListViewWide)
        rightPanelSplitter->addWidget(topRightWidget);
    else
        mainSplitter->addWidget(topRightWidget);

    connect(noteTableView, SIGNAL(newNote()), this, SLOT(newNote()));
    connect(noteTableView, SIGNAL(notesDeleted(QList<qint32>,bool)), this, SLOT(notesDeleted(QList<qint32>)));
    connect(noteTableView, SIGNAL(notesRestored(QList<qint32>)), this, SLOT(notesRestored(QList<qint32>)));
    connect(&syncRunner, SIGNAL(syncComplete()), noteTableView, SLOT(refreshData()));
    connect(&syncRunner, SIGNAL(noteSynchronized(qint32, bool)), this, SLOT(noteSynchronized(qint32, bool)));

    QLOG_TRACE() << "Leaving NixNote.setupNoteList()";
}



// Signal received when a note has been synchronized
void NixNote::noteSynchronized(qint32 lid, bool value) {
    noteTableView->refreshCell(lid, NOTE_TABLE_IS_DIRTY_POSITION, value);
}


//*****************************************************************************
//* This function sets up the user's search tree
//*****************************************************************************
void NixNote::setupSearchTree() {
    QLOG_TRACE() << "Starting NixNote.setupSearchTree()";

    leftSeparator3 = new QLabel();
    leftSeparator3->setTextFormat(Qt::RichText);
    leftSeparator3->setText("<hr>");
    leftPanel->addSeparator(leftSeparator3);

    searchTreeView = new NSearchView(leftPanel);
    leftPanel->addSearchView(searchTreeView);
    connect(&syncRunner, SIGNAL(searchUpdated(qint32, QString)), searchTreeView, SLOT(searchUpdated(qint32, QString)));
    connect(&syncRunner, SIGNAL(searchExpunged(qint32)), searchTreeView, SLOT(searchExpunged(qint32)));
    //connect(&syncRunner, SIGNAL(syncComplete()),searchTreeView, SLOT(re);
    QLOG_TRACE() << "Exiting NixNote.setupSearchTree()";
}


//*****************************************************************************
//* This function sets up the user's tag tree
//*****************************************************************************
void NixNote::setupTagTree() {
    QLOG_TRACE() << "Starting NixNote.setupTagTree()";

    leftSeparator2 = new QLabel();
    leftSeparator2->setTextFormat(Qt::RichText);
    leftSeparator2->setText("<hr>");
    leftPanel->addSeparator(leftSeparator2);

    tagTreeView = new NTagView(leftPanel);
    leftPanel->addTagView(tagTreeView);
    connect(&syncRunner, SIGNAL(tagUpdated(qint32, QString, QString, qint32)),tagTreeView, SLOT(tagUpdated(qint32, QString, QString, qint32)));
    connect(&syncRunner, SIGNAL(tagExpunged(qint32)), tagTreeView, SLOT(tagExpunged(qint32)));
    connect(&syncRunner, SIGNAL(syncComplete()),tagTreeView, SLOT(rebuildTree()));
    connect(&counterRunner, SIGNAL(tagTotals(qint32,qint32, qint32)), tagTreeView, SLOT(updateTotals(qint32,qint32, qint32)));
    connect(&counterRunner, SIGNAL(tagCountComplete()), tagTreeView, SLOT(hideUnassignedTags()));
    connect(notebookTreeView, SIGNAL(notebookSelectionChanged(qint32)), tagTreeView, SLOT(notebookSelectionChanged(qint32)));
    connect(tagTreeView, SIGNAL(updateNoteList(qint32,int,QVariant)), noteTableView, SLOT(refreshCell(qint32,int,QVariant)));
    connect(tagTreeView, SIGNAL(updateCounts()), &counterRunner, SLOT(countAll()));
    QLOG_TRACE() << "Exiting NixNote.setupTagTree()";
}



//*****************************************************************************
//* This function sets up the attribute search tree
//*****************************************************************************
void NixNote::setupAttributeTree() {
    QLOG_TRACE() << "Starting NixNote.setupAttributeTree()";

    leftseparator4 = new QLabel();
    leftseparator4->setTextFormat(Qt::RichText);
    leftseparator4->setText("<hr>");
    leftPanel->addSeparator(leftseparator4);

    attributeTree = new NAttributeTree(leftPanel);
    leftPanel->addAttributeTree(attributeTree);
    QLOG_TRACE() << "Exiting NixNote.setupAttributeTree()";
}


//*****************************************************************************
//* This function sets up the trash
//*****************************************************************************
void NixNote::setupTrashTree() {
    QLOG_TRACE() << "Starting NixNote.setupTrashTree()";

    leftSeparator5 = new QLabel();
    leftSeparator5->setTextFormat(Qt::RichText);
    leftSeparator5->setText("<hr>");
    leftPanel->addSeparator(leftSeparator5);

    trashTree = new NTrashTree(leftPanel);
    leftPanel->addTrashTree(trashTree);
    QLOG_TRACE() << "Exiting NixNote.setupTrashTree()";
    connect(&counterRunner, SIGNAL(trashTotals(qint32)), trashTree, SLOT(updateTotals(qint32)));
}



//*****************************************************************************
//* This function sets up the user's synchronized notebook tree
//*****************************************************************************
void NixNote::setupFavoritesTree() {
    QLOG_TRACE() << "Starting NixNote.setupFavoritesdNotebookTree()";
    favoritesTreeView = new FavoritesView(leftPanel);
    leftPanel->addFavoritesView(favoritesTreeView);

//    connect(&syncRunner, SIGNAL(notebookUpdated(qint32, QString,QString, bool, bool)),notebookTreeView, SLOT(notebookUpdated(qint32, QString, QString, bool, bool)));
    connect(&syncRunner, SIGNAL(notebookExpunged(qint32)), favoritesTreeView, SLOT(itemExpunged(qint32)));
    connect(&syncRunner, SIGNAL(tagExpunged(qint32)), favoritesTreeView, SLOT(itemExpunged(qint32)));
//    connect(&syncRunner, SIGNAL(noteUpdated(qint32)), notebookTreeView, SLOT(itemExpunged(qint32)));
    connect(&counterRunner, SIGNAL(notebookTotals(qint32,qint32, qint32)), favoritesTreeView, SLOT(updateTotals(qint32,qint32, qint32)));
    connect(&counterRunner, SIGNAL(tagTotals(qint32,qint32,qint32)), favoritesTreeView, SLOT(updateTotals(qint32,qint32, qint32)));
    connect(favoritesTreeView, SIGNAL(updateCounts()), &counterRunner, SLOT(countAll()));

    leftSeparator1 = new QLabel();
    leftSeparator1->setTextFormat(Qt::RichText);
    leftSeparator1->setText("<hr>");
    leftPanel->addSeparator(leftSeparator1);

    QLOG_TRACE() << "Exiting NixNote.setupFavoritesTree()";
}



//*****************************************************************************
//* This function sets up the user's synchronized notebook tree
//*****************************************************************************
void NixNote::setupSynchronizedNotebookTree() {
    QLOG_TRACE() << "Starting NixNote.setupSynchronizedNotebookTree()";
    notebookTreeView = new NNotebookView(leftPanel);
    leftPanel->addNotebookView(notebookTreeView);
    connect(&syncRunner, SIGNAL(notebookUpdated(qint32, QString,QString, bool, bool)),notebookTreeView, SLOT(notebookUpdated(qint32, QString, QString, bool, bool)));
    connect(&syncRunner, SIGNAL(syncComplete()),notebookTreeView, SLOT(rebuildTree()));
    connect(&syncRunner, SIGNAL(notebookExpunged(qint32)), notebookTreeView, SLOT(notebookExpunged(qint32)));
    connect(&counterRunner, SIGNAL(notebookTotals(qint32,qint32, qint32)), notebookTreeView, SLOT(updateTotals(qint32,qint32, qint32)));
    connect(notebookTreeView, SIGNAL(updateNoteList(qint32,int,QVariant)), noteTableView, SLOT(refreshCell(qint32,int,QVariant)));
    connect(notebookTreeView, SIGNAL(updateCounts()), &counterRunner, SLOT(countAll()));
    QLOG_TRACE() << "Exiting NixNote.setupSynchronizedNotebookTree()";
}



//*****************************************************************************
//* This function sets up the tab window that is used by the browser
//*****************************************************************************
void NixNote::setupTabWindow() {
    QLOG_TRACE() << "Starting NixNote.setupTabWindow()";
    tabWindow = new NTabWidget(this, &syncRunner, notebookTreeView, tagTreeView);
    QWidget *tabPanel = new QWidget(this);
    tabPanel->setLayout(new QVBoxLayout());
    tabPanel->layout()->addWidget(tabWindow);
    rightPanelSplitter->addWidget(tabPanel);

    NBrowserWindow *newBrowser = new NBrowserWindow(this);
    connect(&syncRunner, SIGNAL(syncComplete()), &newBrowser->notebookMenu, SLOT(reloadData()));
    connect(&syncRunner, SIGNAL(syncComplete()), &newBrowser->tagEditor, SLOT(reloadTags()));
    connect(&syncRunner, SIGNAL(noteUpdated(qint32)), newBrowser, SLOT(noteSyncUpdate(qint32)));
    tabWindow->addBrowser(newBrowser, "");
    rightPanelSplitter->setStretchFactor(1,10);

    connect(noteTableView, SIGNAL(openNote(bool)), this, SLOT(openNote(bool)));
    connect(noteTableView, SIGNAL(openNoteExternalWindow(qint32)), this, SLOT(openExternalNote(qint32)));
    connect(menuBar->viewSourceAction, SIGNAL(triggered()), tabWindow, SLOT(toggleSource()));
    connect(menuBar->viewHistoryAction, SIGNAL(triggered()), this, SLOT(viewNoteHistory()));
    connect(menuBar->viewPresentationModeAction, SIGNAL(triggered()), this, SLOT(presentationModeOn()));
    connect(tabWindow, SIGNAL(escapeKeyPressed()), this, SLOT(presentationModeOff()));

    connect(menuBar->undoAction, SIGNAL(triggered()), tabWindow, SLOT(undoButtonPressed()));
    connect(menuBar->redoAction, SIGNAL(triggered()), tabWindow, SLOT(redoButtonPressed()));
    connect(menuBar->cutAction, SIGNAL(triggered()), tabWindow, SLOT(cutButtonPressed()));
    connect(menuBar->copyAction, SIGNAL(triggered()), tabWindow, SLOT(copyButtonPressed()));
    connect(menuBar->pasteAction, SIGNAL(triggered()), tabWindow, SLOT(pasteButtonPressed()));
    connect(menuBar->pasteAsTextAction, SIGNAL(triggered()), tabWindow, SLOT(pasteAsTextButtonPressed()));
    connect(menuBar->selectAllAction, SIGNAL(triggered()), tabWindow, SLOT(selectAllButtonPressed()));
    connect(menuBar->viewExtendedInformation, SIGNAL(triggered()), tabWindow, SLOT(viewExtendedInformation()));


    QLOG_TRACE() << "Exiting NixNote.setupTabWindow()";
}



//****************************************************************************
//* Close NixNote.   This will force a close.  Even if "close to tray" is set.
//*****************************************************************************
void NixNote::closeNixNote() {
    closeFlag = true;
    closeToTray = false;
    close();
}


//*****************************************************************************
//* Close nixnote via the shortcut. If we have it set to close to the tray,
//* we just hide things.
//*****************************************************************************
void NixNote::closeShortcut() {
    if (closeToTray && isVisible())
        toggleVisible();
    else
        closeNixNote();

}



//*****************************************************************************
//* Save program contents on exit
//******************************************************************************
void NixNote::saveOnExit() {
    QLOG_DEBUG() << "saveOnExit called";

    QLOG_DEBUG() << "Saving contents";
    saveContents();

    QLOG_DEBUG() << "Shutting down threads";
    indexRunner.keepRunning = false;
    counterRunner.keepRunning = false;
    QCoreApplication::processEvents();

    QLOG_DEBUG() << "Saving window states";
    ConfigStore config(global.db);
    config.saveSetting(CONFIG_STORE_WINDOW_STATE, saveState());
    config.saveSetting(CONFIG_STORE_WINDOW_GEOMETRY, saveGeometry());

    QString lidList;
    QList<NBrowserWindow*> *browsers = tabWindow->browserList;
    for (int i=0;i<browsers->size(); i++) {
        lidList = lidList + QString::number(browsers->at(i)->lid) + QString(" ");
    }

    global.settings->beginGroup("SaveState");
    global.settings->setValue("WindowState", saveState());
    global.settings->setValue("WindowGeometry", saveGeometry());
    global.settings->setValue("isMaximized", isMaximized());
    global.settings->setValue("openTabs", lidList);
    global.settings->setValue("lastViewed", tabWindow->currentBrowser()->lid);
    global.settings->setValue("noteListWidth", noteTableView->width());
    global.settings->setValue("noteListHeight", noteTableView->height());
    global.settings->setValue("mainSplitter", mainSplitter->saveState());
    global.settings->setValue("rightSplitter", rightPanelSplitter->saveState());
    global.settings->setValue("listView", global.listView);

    global.settings->remove("selectedStack");
    global.settings->remove("selectedNotebook");
    global.settings->remove("expandedStacks");
    global.settings->remove("selectedTags");
    global.settings->remove("expandedTags");
    global.settings->remove("selectedSearch");
    global.settings->remove("searchString");

    // Save the current notebook/stack selection
    if (notebookTreeView->selectedItems().size() > 0) {
        NNotebookViewItem *item = (NNotebookViewItem*)notebookTreeView->selectedItems().at(0);
        qint32 saveLid = item->lid;
        if (saveLid > 0) {
            global.settings->setValue("selectedNotebook", saveLid);
        } else {
            QString saveStack = item->text(0);
            global.settings->setValue("selectedStack", saveStack);
        }
    }

    if (searchText->isSet()) {
        global.settings->setValue("searchString", searchText->text().trimmed());
    }

    if (searchTreeView->selectedItems().size() > 0) {
        NSearchViewItem *item = (NSearchViewItem*)searchTreeView->selectedItems().at(0);
        qint32 saveLid = item->data(0, Qt::UserRole).toInt();
        if (saveLid > 0) {
            global.settings->setValue("selectedSearch", saveLid);
        }
    }

    // Save any selected tags
    QString savedLids = "";
    if (tagTreeView->selectedItems().size() > 0) {
        for (int i=0; i<tagTreeView->selectedItems().size(); i++) {
            NTagViewItem *item = (NTagViewItem*)tagTreeView->selectedItems().at(i);
            qint32 saveLid = item->data(0, Qt::UserRole).toInt();
            savedLids = savedLids + QString::number(saveLid) + " ";
        }
        global.settings->setValue("selectedTags", savedLids.trimmed());
    }

    QHash<qint32, NTagViewItem*>::iterator iterator;
    savedLids = "";
    for (iterator=tagTreeView->dataStore.begin(); iterator!=tagTreeView->dataStore.end(); ++iterator) {
        if (iterator.value() != NULL) {
            qint32 saveLid = iterator.value()->data(0, Qt::UserRole).toInt();
            if (iterator.value()->isExpanded()) {
                savedLids = savedLids + QString::number(saveLid) + " ";
            }
        }
    }
    global.settings->setValue("expandedTags", savedLids.trimmed());

    QString collapsedTrees = "";
    global.settings->remove("collapsedTrees");
    if (!favoritesTreeView->root->isExpanded())
        collapsedTrees = "favorites ";
    if (!notebookTreeView->root->isExpanded())
        collapsedTrees= collapsedTrees + "notebooks ";
    if (!tagTreeView->root->isExpanded())
        collapsedTrees= collapsedTrees + "tags ";
    if (!attributeTree->root->isExpanded())
        collapsedTrees= collapsedTrees + "attributes ";
    if (!searchTreeView->root->isExpanded())
        collapsedTrees= collapsedTrees + "savedsearches ";
    global.settings->setValue("collapsedTrees", collapsedTrees.trimmed());

    QHash<qint32, NNotebookViewItem*>::iterator books;
    savedLids = "";
    for (books=notebookTreeView->dataStore.begin(); books!=notebookTreeView->dataStore.end(); ++books) {
        if (books.value() != NULL) {
            qint32 saveLid = books.value()->data(0, Qt::UserRole).toInt();
            if (books.value()->stack != "" && books.value()->parent()->isExpanded()) {
                savedLids = savedLids + QString::number(saveLid) + " ";
            }
        }
    }
    global.settings->setValue("expandedStacks", savedLids.trimmed());

    global.settings->endGroup();

    saveNoteColumnWidths();
    saveNoteColumnPositions();
    noteTableView->saveColumnsVisible();

    QLOG_DEBUG() << "Closing threads";
    indexThread.quit();
    counterThread.quit();

    QLOG_DEBUG() << "Exitng saveOnExit()";
}

//*****************************************************************************
//* Close the program
//*****************************************************************************
void NixNote::closeEvent(QCloseEvent *event) {
//    if (closeToTray && !closeFlag) {
//        event->ignore();
//        hide();
//        return;
//    }

    saveOnExit();

    global.settings->beginGroup("Sync");
    bool syncOnShutdown = global.settings->value("syncOnShutdown", false).toBool();
    global.settings->endGroup();
    if (syncOnShutdown && !finalSync && global.accountsManager->oauthTokenFound()) {
        finalSync = true;
        syncRunner.finalSync = true;
        hide();
        connect(&syncRunner, SIGNAL(syncComplete()), this, SLOT(close()));
        synchronize();
        event->ignore();
        return;
    }

    syncRunner.keepRunning = false;
    syncThread.quit();

    if (trayIcon->isVisible())
        trayIcon->hide();
    if (trayIcon != NULL)
        delete trayIcon;

    QMainWindow::closeEvent(event);
    QLOG_DEBUG() << "Quitting";
}



//*************************************************************
//* Function called on shutdown to save all of the note
//* table column positions.  These values are restored the
//* next time NixNote starts.
//**************************************************************
void NixNote::saveNoteColumnPositions() {
    int position = noteTableView->horizontalHeader()->visualIndex(NOTE_TABLE_ALTITUDE_POSITION);
    global.setColumnPosition("noteTableAltitudePosition", position);
    position = noteTableView->horizontalHeader()->visualIndex(NOTE_TABLE_AUTHOR_POSITION);
    global.setColumnPosition("noteTableAuthorPosition", position);
    position = noteTableView->horizontalHeader()->visualIndex(NOTE_TABLE_DATE_CREATED_POSITION);
    global.setColumnPosition("noteTableDateCreatedPosition", position);
    position = noteTableView->horizontalHeader()->visualIndex(NOTE_TABLE_DATE_DELETED_POSITION);
    global.setColumnPosition("noteTableDateDeletedPosition", position);
    position = noteTableView->horizontalHeader()->visualIndex(NOTE_TABLE_DATE_SUBJECT_POSITION);
    global.setColumnPosition("noteTableDateSubjectPosition", position);
    position = noteTableView->horizontalHeader()->visualIndex(NOTE_TABLE_DATE_UPDATED_POSITION);
    global.setColumnPosition("noteTableDateUpdatedosition", position);
    position = noteTableView->horizontalHeader()->visualIndex(NOTE_TABLE_HAS_ENCRYPTION_POSITION);
    global.setColumnPosition("noteTableHasEncryptionPosition", position);
    position = noteTableView->horizontalHeader()->visualIndex(NOTE_TABLE_HAS_TODO_POSITION);
    global.setColumnPosition("noteTableHasTodoPosition", position);
    position = noteTableView->horizontalHeader()->visualIndex(NOTE_TABLE_IS_DIRTY_POSITION);
    global.setColumnPosition("noteTableIsDirtyPosition", position);
    position = noteTableView->horizontalHeader()->visualIndex(NOTE_TABLE_LATITUDE_POSITION);
    global.setColumnPosition("noteTableLatitudePosition", position);
    position = noteTableView->horizontalHeader()->visualIndex(NOTE_TABLE_LONGITUDE_POSITION);
    global.setColumnPosition("noteTableLongitudePosition", position);
    position = noteTableView->horizontalHeader()->visualIndex(NOTE_TABLE_NOTEBOOK_LID_POSITION);
    global.setColumnPosition("noteTableNotebookLidPosition", position);
    position = noteTableView->horizontalHeader()->visualIndex(NOTE_TABLE_LID_POSITION);
    global.setColumnPosition("noteTableLidPosition", position);
    position = noteTableView->horizontalHeader()->visualIndex(NOTE_TABLE_NOTEBOOK_POSITION);
    global.setColumnPosition("noteTableNotebookPosition", position);
    position = noteTableView->horizontalHeader()->visualIndex(NOTE_TABLE_SIZE_POSITION);
    global.setColumnPosition("noteTableSizePosition", position);
    position = noteTableView->horizontalHeader()->visualIndex(NOTE_TABLE_SOURCE_APPLICATION_POSITION);
    global.setColumnPosition("noteTableSourceApplicationPosition", position);
    position = noteTableView->horizontalHeader()->visualIndex(NOTE_TABLE_SOURCE_POSITION);
    global.setColumnPosition("noteTableSourcePosition", position);
    position = noteTableView->horizontalHeader()->visualIndex(NOTE_TABLE_SOURCE_URL_POSITION);
    global.setColumnPosition("noteTableSourceUrlPosition", position);
    position = noteTableView->horizontalHeader()->visualIndex(NOTE_TABLE_TAGS_POSITION);
    global.setColumnPosition("noteTableTagsPosition", position);
    position = noteTableView->horizontalHeader()->visualIndex(NOTE_TABLE_TITLE_POSITION);
    global.setColumnPosition("noteTableTitlePosition", position);
    position = noteTableView->horizontalHeader()->visualIndex(NOTE_TABLE_REMINDER_TIME_POSITION);
    global.setColumnPosition("noteTableReminderTimePosition", position);
    position = noteTableView->horizontalHeader()->visualIndex(NOTE_TABLE_REMINDER_TIME_DONE_POSITION);
    global.setColumnPosition("noteTableReminderTimeDonePosition", position);
    position = noteTableView->horizontalHeader()->visualIndex(NOTE_TABLE_REMINDER_ORDER_POSITION);
    global.setColumnPosition("noteTableReminderOrderPosition", position);
}



//*************************************************************
//* Function called on shutdown to save all of the note
//* table column widths.  These values are restored the
//* next time NixNote starts.
//**************************************************************
void NixNote::saveNoteColumnWidths() {
    int width;
    width = noteTableView->columnWidth(NOTE_TABLE_ALTITUDE_POSITION);
    global.setColumnWidth("noteTableAltitudePosition", width);
    width = noteTableView->columnWidth(NOTE_TABLE_AUTHOR_POSITION);
    global.setColumnWidth("noteTableAuthorPosition", width);
    width = noteTableView->columnWidth(NOTE_TABLE_DATE_CREATED_POSITION);
    global.setColumnWidth("noteTableDateCreatedPosition", width);
    width = noteTableView->columnWidth(NOTE_TABLE_DATE_DELETED_POSITION);
    global.setColumnWidth("noteTableDateDeletedPosition", width);
    width = noteTableView->columnWidth(NOTE_TABLE_DATE_SUBJECT_POSITION);
    global.setColumnWidth("noteTableDateSubjectPosition", width);
    width = noteTableView->columnWidth(NOTE_TABLE_DATE_UPDATED_POSITION);
    global.setColumnWidth("noteTableDateUpdatedPosition", width);
    width = noteTableView->columnWidth(NOTE_TABLE_HAS_ENCRYPTION_POSITION);
    global.setColumnWidth("noteTableHasEncryptionPosition", width);
    width = noteTableView->columnWidth(NOTE_TABLE_HAS_TODO_POSITION);
    global.setColumnWidth("noteTableHasTodoPosition", width);
    width = noteTableView->columnWidth(NOTE_TABLE_IS_DIRTY_POSITION);
    global.setColumnWidth("noteTableIsDirtyPosition", width);
    width = noteTableView->columnWidth(NOTE_TABLE_LATITUDE_POSITION);
    global.setColumnWidth("noteTableLatitudePosition", width);
    width = noteTableView->columnWidth(NOTE_TABLE_LID_POSITION);
    global.setColumnWidth("noteTableLidPosition", width);
    width = noteTableView->columnWidth(NOTE_TABLE_LONGITUDE_POSITION);
    global.setColumnWidth("noteTableLongitudePosition", width);
    width = noteTableView->columnWidth(NOTE_TABLE_NOTEBOOK_LID_POSITION);
    global.setColumnWidth("noteTableNotebookLidPosition", width);
    width = noteTableView->columnWidth(NOTE_TABLE_NOTEBOOK_POSITION);
    global.setColumnWidth("noteTableNotebookPosition", width);
    width = noteTableView->columnWidth(NOTE_TABLE_SIZE_POSITION);
    global.setColumnWidth("noteTableSizePosition", width);
    width = noteTableView->columnWidth(NOTE_TABLE_SOURCE_APPLICATION_POSITION);
    global.setColumnWidth("noteTableSourceApplicationPosition", width);
    width = noteTableView->columnWidth(NOTE_TABLE_TAGS_POSITION);
    global.setColumnWidth("noteTableTagsPosition", width);
    width = noteTableView->columnWidth(NOTE_TABLE_TITLE_POSITION);
    global.setColumnWidth("noteTableTitlePosition", width);
    width = noteTableView->columnWidth(NOTE_TABLE_REMINDER_TIME_POSITION);
    global.setColumnWidth("noteTableReminderTimePosition", width);
    width = noteTableView->columnWidth(NOTE_TABLE_REMINDER_TIME_DONE_POSITION);
    global.setColumnWidth("noteTableReminderTimeDonePosition", width);
    width = noteTableView->columnWidth(NOTE_TABLE_REMINDER_ORDER_POSITION);
    global.setColumnWidth("noteTableReminderOrderPosition", width);
}




//*****************************************************************************
//* The sync timer has expired
//*****************************************************************************
void NixNote::syncTimerExpired() {
    pauseIndexing(true);
    // If we are already connected, we are already synchronizing so there is nothing more to do
    if (global.connected == true)
        return;
    if (!global.accountsManager->oauthTokenFound())
        return;
    tabWindow->saveAllNotes();
    emit(syncRequested());
}




//******************************************************************************
//* User synchronize was requested
//******************************************************************************
void NixNote::synchronize() {
    // If we are already connected, we are already synchronizing so there is nothing more to do
    if (global.connected == true)
        return;

    this->pauseIndexing(true);

    if (tabWindow->currentBrowser()->noteTitle.hasFocus())
        tabWindow->currentBrowser()->noteTitle.checkNoteTitleChange();

    if (!global.accountsManager->oauthTokenFound()) {
        QString consumerKey = "baumgarr-3523";
        QString consumerSecret = "8d5ee175f8a5d3ec";
        EvernoteOAuthDialog d(consumerKey, consumerSecret, global.server);
        d.setWindowTitle(tr("Log in to Evernote"));
        if(d.exec() != QDialog::Accepted) {
            QMessageBox::critical(0, tr("NixNote"), "Login failed.\n" + d.oauthError());
            return;
        }
        QString token = QString("oauth_token=") +d.oauthResult().authenticationToken +
                       QString("&oauth_token_secret=&edam_shard=")+d.oauthResult().shardId  +
                       QString("&edam_userId=") +QString::number(d.oauthResult().userId) +
                       QString("&edam_expires=") +QString::number(d.oauthResult().expires) +
                       QString("&edam_noteStoreUrl=") + d.oauthResult().noteStoreUrl +
                       QString("&edam_webApiUrlPrefix=") +d.oauthResult().webApiUrlPrefix;

        global.accountsManager->setOAuthToken(token);
    }

    this->saveContents();
    statusBar()->clearMessage();

    tabWindow->saveAllNotes();
    syncButtonTimer.start(3);
    emit syncRequested();
}



//********************************************************************************
//* Disconnect from Evernote
//********************************************************************************
void NixNote::disconnect() {
    global.connected = false;
    menuBar->disconnectAction->setEnabled(false);
    syncButtonTimer.stop();
    pauseIndexing(false);
}



//********************************************************
//* Function called when a sync has completed.  It stops
//* the spinning sync icon and resets it to the default
//* value.
//*********************************************************
void NixNote::syncButtonReset() {
    pauseIndexing(false);
    if (syncIcons.size() == 0)
        return;
    syncButtonTimer.stop();
    syncButton->setIcon(syncIcons[0]);

    // If we had an API rate limit exceeded, restart at the top of the hour.
    if (syncRunner.apiRateLimitExceeded) {
           global.settings->beginGroup("Sync");
           bool restart = global.settings->value("apiRateLimitAutoRestart", false).toBool();
           global.settings->endGroup();
           if (restart) {
               QTime t = QTime::currentTime();
               int minutes = 60-t.minute()+1; // Time to the top of the hour plus a padding.
               QTimer::singleShot(60*1000*minutes, this, SLOT(synchronize()));
           }
    }
}



//*****************************************************
//* Rotate the sync icon when we are connected to
//* Evernote and are transmitting & receiving info
//*****************************************************
void NixNote::updateSyncButton() {

    if (syncIcons.size() == 0) {
        double angle = 0.0;
        synchronizeIconAngle = 0;
        QPixmap pix(":synchronizeIcon");
        syncIcons.push_back(pix);
        for (qint32 i=0; i<=360; i++) {
            QPixmap rotatedPix(pix.size());
            QPainter p(&rotatedPix);
            rotatedPix.fill(toolBar->palette().color(QPalette::Background));
            QSize size = pix.size();
            p.translate(size.width()/2, size.height()/2);
            angle = angle+1.0;
            p.rotate(angle);
            p.setBackgroundMode( Qt::OpaqueMode);
            p.translate(-size.width()/2, -size.height()/2);
            p.drawPixmap(0,0, pix);
            p.end();
            syncIcons.push_back(rotatedPix);
        }
    }
    synchronizeIconAngle++;
    if (synchronizeIconAngle > 359)
            synchronizeIconAngle=0;
    syncButton->setIcon(syncIcons[synchronizeIconAngle]);
}




//************************************************************
//* Open a new note.  If newWindow is true it is an external
//* note request.
//************************************************************
void NixNote::openNote(bool newWindow) {
    saveContents();
    FilterCriteria *criteria = global.filterCriteria[global.filterPosition];
    qint32 lid;
    if (criteria->isLidSet()) {
        lid = criteria->getLid();
        if (newWindow)
            tabWindow->openNote(lid, NTabWidget::NewTab);
        else
            tabWindow->openNote(lid, NTabWidget::CurrentTab);
    } else {
        tabWindow->openNote(-1, NTabWidget::CurrentTab);
    }
    rightArrowButton->setEnabled(false);
    leftArrowButton->setEnabled(false);
    if (global.filterPosition+1 < global.filterCriteria.size())
        rightArrowButton->setEnabled(true);
    if (global.filterPosition > 0)
        leftArrowButton->setEnabled(true);
    checkReadOnlyNotebook();
}




//**************************************************************
//* Open a note in an external window.
//**************************************************************
void NixNote::openExternalNote(qint32 lid) {
    tabWindow->openNote(lid, NTabWidget::ExternalWindow);
}




//*****************************************************
//* Called when a user changes the selection criteria
//* (i.e. they select a notebook, tag, saved search...
//*****************************************************
void NixNote::updateSelectionCriteria(bool afterSync) {
    QLOG_DEBUG() << "starting NixNote.updateSelectionCriteria()";

    tabWindow->currentBrowser()->saveNoteContent();

    // Invalidate the cache
    QDir dir(global.fileManager.getTmpDirPath());
    QFileInfoList files = dir.entryInfoList();

    for (int i=0; i<files.size(); i++) {
        if (files[i].fileName().endsWith("_icon.png")) {
            QFile file(files[i].absoluteFilePath());
            file.remove();
        }
    }
    QList<qint32> keys = global.cache.keys();
    for (int i=0; i<keys.size(); i++) {
        global.cache.remove(keys[i]);
    }

    FilterEngine filterEngine;
    filterEngine.filter();

    QLOG_DEBUG() << "Refreshing data";

    noteTableView->refreshData();

    favoritesTreeView->updateSelection();
    tagTreeView->updateSelection();
    notebookTreeView->updateSelection();
    searchTreeView->updateSelection();
    attributeTree->updateSelection();
    trashTree->updateSelection();
    searchText->updateSelection();

    rightArrowButton->setEnabled(false);
    leftArrowButton->setEnabled(false);
    if (global.filterPosition+1 < global.filterCriteria.size())
        rightArrowButton->setEnabled(true);
    if (global.filterPosition > 0)
        leftArrowButton->setEnabled(true);

    QList<qint32> selectedNotes;
    global.filterCriteria[global.filterPosition]->getSelectedNotes(selectedNotes);
    if (selectedNotes.size() == 0) {
        tabWindow->currentBrowser()->clear();
        tabWindow->currentBrowser()->setReadOnly(true);
    }
    if (selectedNotes.size() > 0 && !afterSync) {
        //tabWindow->currentBrowser()->setContent(selectedNotes.at(0));  // <<<<<< This causes problems with multiple tabs after sync
        NBrowserWindow *window = NULL;
        tabWindow->findBrowser(window, selectedNotes.at(0));
        if (window!= NULL)
            window->setContent(selectedNotes.at(0));
        if (!afterSync)
            openNote(false);
    }

    if (global.filterCriteria[global.filterPosition]->isDeletedOnlySet() && global.filterCriteria[global.filterPosition]->getDeletedOnly())
        newNoteButton->setEnabled(false);
    else
        newNoteButton->setEnabled(true);

    emit updateCounts();
}



//******************************************************************
//* Check if the notebook selected is read-only.  With
//* read-only notes the editor and a lot of actions are disabled.
//******************************************************************
void NixNote::checkReadOnlyNotebook() {
    qint32 lid = tabWindow->currentBrowser()->lid;
    Note n;
    NoteTable ntable(global.db);
    NotebookTable btable(global.db);
    ntable.get(n, lid, false, false);
    qint32 notebookLid = 0;
    if (n.notebookGuid.isSet())
        notebookLid = btable.getLid(n.notebookGuid);
    if (btable.isReadOnly(notebookLid)) {
        newNoteButton->setEnabled(false);
        menuBar->deleteNoteAction->setEnabled(false);
    } else {
        newNoteButton->setEnabled(true);
        menuBar->deleteNoteAction->setEnabled(true);
    }
}




//*********************************************
//* User clicked the -> "forward" button
//* to go to the next history position.
//*********************************************
void NixNote::rightButtonTriggered() {
    global.filterPosition++;
    updateSelectionCriteria();
}




//*********************************************
//* User clicked the <- "back" button
//* to go to the previous history position.
//*********************************************
void NixNote::leftButtonTriggered() {
    global.filterPosition--;
    updateSelectionCriteria();
}




//**************************************************
// Import notes menu option chosen
//**************************************************
void NixNote::noteExport() {
    databaseBackup(false);
}



//**************************************************
//* Backup (or export) notes
//**************************************************
void NixNote::databaseBackup(bool backup) {
    QLOG_TRACE() << "Entering databaseBackup()";
    ExportData noteReader(backup);

    if (!backup) {
        noteTableView->getSelectedLids(noteReader.lids);
        if (noteReader.lids.size() == 0) {
            QMessageBox::critical(this, tr("Error"), tr("No notes selected."));
            return;
        }
    }

    QString caption, directory;
    if (backup)
        caption = tr("Backup Database");
    else
        caption = tr("Export Notes");
    if (saveLastPath == "")
        directory = QDir::homePath();
    else
        directory = saveLastPath;

    QFileDialog fd(0, caption, directory, tr("NixNote Export (*.nnex);;All Files (*.*)"));
    fd.setFileMode(QFileDialog::AnyFile);
    fd.setConfirmOverwrite(true);
    fd.setAcceptMode(QFileDialog::AcceptSave);

    if (fd.exec() == 0 || fd.selectedFiles().size() == 0) {
        QLOG_DEBUG() << "Database restore canceled in file dialog.";
        return;
    }

    waitCursor(true);
    QStringList fileNames;
    fileNames = fd.selectedFiles();
    saveLastPath = fileNames[0];
    int pos = saveLastPath.lastIndexOf("/");
    if (pos != -1)
        saveLastPath.truncate(pos);

    if (backup)
        setMessage(tr("Performing backup"));
    else
        setMessage(tr("Performing export"));

    if (!fileNames[0].endsWith(".nnex")) {
        fileNames[0].append(".nnex");
    }
        noteReader.backupData(fileNames[0]);

    if (noteReader.lastError != 0) {
        setMessage(noteReader.errorMessage);
        QLOG_ERROR() <<  "Backup problem: " << noteReader.errorMessage;
        QMessageBox::critical(this, tr("Error"), noteReader.errorMessage);
        waitCursor(false);
        return;
    }


    if (backup)
        setMessage(tr("Database backup complete."));
    else
        setMessage(tr("Note extract complete."));

    waitCursor(false);

}




//**************************************************
// Import notes menu option chosen
//**************************************************
void NixNote::noteImport() {
    databaseRestore(false);
}



//**************************************************
//* Restore (or import) notes
//**************************************************
void NixNote::databaseRestore(bool fullRestore) {
    QLOG_TRACE() << "Entering databaseRestore()";

    if (fullRestore) {
        QMessageBox msgBox;
        msgBox.setText(tr("This is used to restore a database from backups.\nIt is HIGHLY recommended that this only be used to populate\nan empty database.  Restoring into a database that\n already has data can cause problems.\n\nAre you sure you want to continue?"));
        msgBox.setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel);
        msgBox.setDefaultButton(QMessageBox::Ok);
        msgBox.setWindowTitle(tr("Confirm Restore"));
        int retval = msgBox.exec();

        switch (retval) {
        case QMessageBox::Cancel:    // Cancel was clicked, let's exit
            QLOG_DEBUG() << "Database restore has been canceled";
            return;
            break;
        default:  // Anything else we don't care
            break;
        }
    }

    QString caption, directory, filter;

    if (fullRestore) {
        caption = tr("Restore Database");
        filter = tr("NixNote Export (*.nnex);;All Files (*.*)");
    } else {
        caption = tr("Import Notes");
        filter = tr("NixNote Export (*.nnex);;Evernote Export (*.enex);;All Files (*.*)");
    }

    if (saveLastPath == "")
        directory = QDir::homePath();
    else
        directory = saveLastPath;

    QFileDialog fd(0, caption, directory, filter);
    fd.setFileMode(QFileDialog::ExistingFile);
    fd.setConfirmOverwrite(true);
    fd.setAcceptMode(QFileDialog::AcceptOpen);

    if (fd.exec() == 0 || fd.selectedFiles().size() == 0) {
        QLOG_DEBUG() << "Database restore canceled in file dialog.";
        return;
    }

    waitCursor(true);
    QStringList fileNames;
    fileNames = fd.selectedFiles();
    saveLastPath = fileNames[0];
    int pos = saveLastPath.lastIndexOf("/");
    if (pos != -1)
        saveLastPath.truncate(pos);

    if (fullRestore)
        setMessage(tr("Restoring database"));
    else
        setMessage(tr("Importing Notes"));

    if (fileNames[0].endsWith(".nnex") || fullRestore) {
        ImportData noteReader(fullRestore);
        noteReader.import(fileNames[0]);

        if (noteReader.lastError != 0) {
            setMessage(noteReader.getErrorMessage());
            QLOG_ERROR() <<  "Restore problem: " << noteReader.errorMessage;
            QMessageBox::critical(this, tr("Error"), noteReader.errorMessage);
            waitCursor(false);
            return;
        }
    } else {
        ImportEnex enexReader;
        fullRestore = false;
        enexReader.import(fileNames[0]);
        QLOG_DEBUG() << "Back from import";
    }

    // Finish by filtering & displaying the data
    updateSelectionCriteria();

    if (fullRestore || fileNames[0].endsWith(".enex")) {
        tagTreeView->rebuildTagTreeNeeded = true;
        tagTreeView->loadData();
        searchTreeView->loadData();
    }
    notebookTreeView->rebuildNotebookTreeNeeded = true;
    notebookTreeView->loadData();

    if (fullRestore)
        setMessage(tr("Database has been restored."));
    else
        setMessage(tr("Notes have been imported."));

    waitCursor(false);
}



//*********************************************************
//* Set wait cursor
//*********************************************************
void NixNote::waitCursor(bool value) {
    Q_UNUSED(value); /* suppress warning of unused */
}


// Show a message in the status bar
void NixNote::setMessage(QString text, int timeout) {
    QLOG_TRACE_IN();
    statusBar()->showMessage(text, timeout);
    QLOG_INFO() << text;
    QLOG_TRACE_OUT();
}




// Notification slot that the sync has completed.
void NixNote::notifySyncComplete() {
    updateSelectionCriteria(true);
    bool show;
    global.settings->beginGroup("Sync");
    show = global.settings->value("enableNotification", false).toBool();
    global.settings->endGroup();
    if (!show)
        return;
    if (syncRunner.error) {
        showMessage(tr("Sync Error"), tr("Sync completed with errors."));
        if (global.popupOnSyncError()) {
            QMessageBox::critical(0, tr("Sync Error"), tr("Sync error. See message log for details"));
        }
    } else
        if (global.showGoodSyncMessagesInTray)
            showMessage(tr("Sync Complete"), tr("Sync completed successfully."));
}



void NixNote::showMessage(QString title, QString msg, int timeout) {
    if (global.systemNotifier() == "notify-send") {
        QProcess notifyProcess;
        QStringList arguments;
        arguments  << title << msg << "-t" << QString::number(timeout);
        notifyProcess.start(QString("notify-send"), arguments, QIODevice::ReadWrite|QIODevice::Unbuffered);
        notifyProcess.waitForFinished();
        QLOG_DEBUG() << "notify-send completed: " << notifyProcess.waitForFinished()
                     << " Return Code: " << notifyProcess.state();
        return;
    }
    trayIcon->showMessage(title, msg);

}


//*******************************************************
//* Check for dirty notes and save the contents
//*******************************************************
void NixNote::saveContents() {
    for (int i=0; i<tabWindow->browserList->size(); i++) {
        qint32 lid = tabWindow->browserList->at(i)->lid;
        // Check if the note is dirty
        if (tabWindow->browserList->at(i)->editor->isDirty) {
            tabWindow->browserList->at(i)->saveNoteContent();
            noteTableView->refreshCell(lid, NOTE_TABLE_IS_DIRTY_POSITION, true);
        }

    }
}



//********************************************
//* Reset values back to the unset values
//********************************************
void NixNote::resetView() {
    FilterCriteria *criteria = new FilterCriteria();
    global.filterCriteria[global.filterPosition]->duplicate(*criteria);
    criteria->resetAttribute = true;
    criteria->resetDeletedOnly =true;
    criteria->resetFavorite = true;
    criteria->resetNotebook = true;
    criteria->resetSavedSearch = true;
    criteria->resetSearchString = true;
    criteria->resetTags = true;
    criteria->unsetFavorite();
    criteria->unsetNotebook();
    criteria->unsetDeletedOnly();
    criteria->unsetTags();
    criteria->unsetAttribute();
    criteria->unsetSavedSearch();
    criteria->unsetSearchString();
    global.filterCriteria.append(criteria);
    global.filterPosition++;
    updateSelectionCriteria();
}


//*****************************
// Create a new note
//*****************************
void NixNote::newNote() {
    if (noteButton->property("currentNoteButton").toInt() != NewTextNote) {
        noteButton->setText(newNoteButton->text());
        noteButton->setIcon(newNoteButton->icon());
        noteButton->setProperty("currentNoteButton", NewTextNote);
    }
    QString newNoteBody = QString("<?xml version=\"1.0\" encoding=\"UTF-8\"?>")+
           QString("<!DOCTYPE en-note SYSTEM \"http://xml.evernote.com/pub/enml2.dtd\">")+
           QString("<en-note style=\"word-wrap: break-word; -webkit-nbsp-mode: space; -webkit-line-break: after-white-space;\"><br/></en-note>");

    Note n;
    NotebookTable notebookTable(global.db);
    n.content = newNoteBody;
    n.title = tr("Untitled note");
    QString uuid = QUuid::createUuid().toString();
    uuid = uuid.mid(1);
    uuid.chop(1);
    n.guid = uuid;
    n.active = true;
    //QDateTime now;
    n.created = QDateTime::currentMSecsSinceEpoch();
    n.updated = n.created;
    n.updateSequenceNum = 0;
    if (notebookTreeView->selectedItems().size() > 0) {
        NNotebookViewItem *item = (NNotebookViewItem*)notebookTreeView->selectedItems().at(0);
        qint32 lid = item->lid;

        // If we have a stack, we find the first notebook (in alphabetical order) for the new note.
        if (lid == 0) {
            QString stackName = notebookTreeView->selectedItems().at(0)->data(0, Qt::DisplayRole).toString();
            QList<qint32> notebooks;
            notebookTable.getStack(notebooks, stackName);
            QString priorName;
            Notebook book;
            if (notebooks.size() > 0) {
                for (int i=0; i<notebooks.size(); i++) {
                    qint32 priorLid = notebooks[i];
                    notebookTable.get(book, priorLid);
                    QString currentName = "";
                    if (book.name.isSet())
                        currentName = book.name;
                    if (currentName.toUpper() < priorName.toUpper() || priorName == "") {
                        lid = notebooks[i];
                    }
                    priorLid = notebooks[i];
                    priorName = currentName;
                }
            }
        }
        QString notebookGuid;
        notebookTable.getGuid(notebookGuid, lid);
        n.notebookGuid = notebookGuid;
    } else {
        QList<QTreeWidgetItem *>items = favoritesTreeView->selectedItems();
        QString notebookGuid = notebookTable.getDefaultNotebookGuid();
        for (int i=0; i<items.size(); i++) {
            FavoritesViewItem *item = (FavoritesViewItem*)items[i];
            if (item->record.type == FavoritesRecord::LocalNotebook ||
                    item->record.type == FavoritesRecord::SynchronizedNotebook) {
                QString guid;
                notebookTable.getGuid(guid, item->record.target.toInt());
                if (guid != "") {
                    notebookGuid = guid;
                    i=items.size();
                }
            }
        }
            n.notebookGuid = notebookGuid;
    }
    if (global.full_username != "") {
        NoteAttributes na;
        if (n.attributes.isSet())
            na = n.attributes;
        na.author = global.full_username;
        n.attributes = na;
    }
    NoteTable table(global.db);
    qint32 lid = table.add(0,n,true);

    FilterCriteria *criteria = new FilterCriteria();
    global.filterCriteria[global.filterPosition]->duplicate(*criteria);
    criteria->unsetTags();
    criteria->unsetSearchString();
    criteria->setLid(lid);
    global.filterCriteria.append(criteria);
    global.filterPosition++;
    updateSelectionCriteria();
    openNote(false);

    if (global.newNoteFocusToTitle()) {
        tabWindow->currentBrowser()->noteTitle.setFocus();
        tabWindow->currentBrowser()->noteTitle.selectAll();
    } else
        tabWindow->currentBrowser()->editor->setFocus();
}



//**********************************************
//* Create a new note in an external window.
//**********************************************
void NixNote::newExternalNote() {
    QString newNoteBody = QString("<?xml version=\"1.0\" encoding=\"UTF-8\"?>")+
           QString("<!DOCTYPE en-note SYSTEM \"http://xml.evernote.com/pub/enml2.dtd\">")+
           QString("<en-note style=\"word-wrap: break-word; -webkit-nbsp-mode: space; -webkit-line-break: after-white-space;\"><br/></en-note>");

    Note n;
    NotebookTable notebookTable(global.db);
    n.content = newNoteBody;
    n.title = "Untitled note";
    QString uuid = QUuid::createUuid().toString();
    uuid = uuid.mid(1);
    uuid.chop(1);
    n.guid = uuid;
    n.active = true;
    //QDateTime now;
    n.created = QDateTime::currentMSecsSinceEpoch();
    n.updated = n.created;
    n.updateSequenceNum = 0;
    if (notebookTreeView->selectedItems().size() == 0) {
        n.notebookGuid = notebookTable.getDefaultNotebookGuid();
    } else {
        NNotebookViewItem *item = (NNotebookViewItem*)notebookTreeView->selectedItems().at(0);
        qint32 lid = item->lid;

        // If we have a stack, we find the first notebook (in alphabetical order) for the new note.
        if (lid == 0) {
            QString stackName = notebookTreeView->selectedItems().at(0)->data(0, Qt::DisplayRole).toString();
            QList<qint32> notebooks;
            notebookTable.getStack(notebooks, stackName);
            QString priorName;
            Notebook book;
            if (notebooks.size() > 0) {
                for (int i=0; i<notebooks.size(); i++) {
                    qint32 priorLid = notebooks[i];
                    notebookTable.get(book, priorLid);
                    QString currentName = "";
                    if (book.name.isSet())
                        currentName = book.name;
                    if (currentName.toUpper() < priorName.toUpper() || priorName == "") {
                        lid = notebooks[i];
                    }
                    priorLid = notebooks[i];
                    priorName = currentName;
                }
            }
        }
        QString notebookGuid;
        notebookTable.getGuid(notebookGuid, lid);
        n.notebookGuid = notebookGuid;
    }
    NoteTable table(global.db);
    qint32 lid = table.add(0,n,true);
    tabWindow->openNote(lid, NTabWidget::ExternalWindow);
    updateSelectionCriteria();

    // Find the position in the external window array & set the focus.
    int pos = -1;
    for (int i=0; i<tabWindow->externalList->size(); i++) {
        if (tabWindow->externalList->at(i)->browser->lid == lid) {
            pos = i;
            i = tabWindow->externalList->size();
        }
    }

    // This shouldn't happen, but just in case...
    if (pos < 0)
        return;

    // Set the focus
    if (global.newNoteFocusToTitle()) {
        tabWindow->externalList->at(pos)->browser->noteTitle.setFocus();
        tabWindow->externalList->at(pos)->browser->noteTitle.selectAll();
    } else
        tabWindow->externalList->at(pos)->browser->editor->setFocus();

}


// Slot for when notes have been deleted from the notes list.
void NixNote::notesDeleted(QList<qint32>) {
    updateSelectionCriteria();
}



// Slot for when notes have been deleted from the notes list.
void NixNote::notesRestored(QList<qint32>) {
    updateSelectionCriteria();
}




// Open the trunk web site
void NixNote::openTrunk() {
    QDesktopServices::openUrl(QUrl("http://www.evernote.com/trunk"));
}


// Open Evernote support
void NixNote::openEvernoteSupport() {
    QString server = "http://www.evernote.com/about/contact/support/";
    if (global.accountsManager->getServer() == "app.yinxiang.com")
        server = "https://support.yinxiang.com";
    QDesktopServices::openUrl(QUrl(server));
}



//*****************************************
//* Open the user account dialog box.
//*****************************************
void NixNote::openAccount() {
    AccountDialog dialog;
    dialog.exec();
}



//*******************************
//* Open Help/About dialog box.
//*******************************
void NixNote::openAbout() {
    AboutDialog about;
    about.exec();
}



//*******************************
//* Open About Qt dialog box.
//*******************************
void NixNote::openQtAbout() {
    QApplication::aboutQt();
}


//*******************************
//* Open the NixNote GitHub page.
//*******************************
void NixNote::openGithub() {
    QString server = "http://www.github.com/baumgarr/nixnote2";
    QDesktopServices::openUrl(QUrl(server));
}


//*********************************
//* Open Shortcut Keys Dialog
//*********************************
void NixNote::openShortcutsDialog() {
    ShortcutDialog *dialog = new ShortcutDialog();
    dialog->exec();
    delete dialog;
}


//**********************************************
//* Show/Hide the left display panel.
//**********************************************
void NixNote::toggleLeftPanel() {
    bool visible;
    if (leftPanel->isVisible()) {
        leftScroll->hide();
        visible = false;
    } else {
        visible = true;
        leftScroll->show();
    }
    global.settings->beginGroup("SaveState");
    global.settings->setValue("leftPanelVisible", visible);
    global.settings->endGroup();
}



//************************************************
//* Show/Hide the note table.
//************************************************
void NixNote::toggleNoteList() {
    bool value;
    if (topRightWidget->isVisible()) {
        topRightWidget->hide();
        value = false;
    } else {
        value = true;
        topRightWidget->show();
    }
    global.settings->beginGroup("SaveState");
    global.settings->setValue("noteListVisible", value);
    global.settings->endGroup();
}



//****************************************************
//* Show/hide the note editor/tab window.
//****************************************************
void NixNote::toggleTabWindow() {
    bool value;
    if (tabWindow->isVisible()) {
        tabWindow->hide();
        value = false;
    } else {
        tabWindow->show();
        value = true;
    }
    global.settings->beginGroup("SaveState");
    global.settings->setValue("tabWindowVisible", value);
    global.settings->endGroup();

}


//**************************************
//* Toggle the main window toolbar.
//**************************************
void NixNote::toggleToolbar() {
    if (toolBar->isVisible())
        toolBar->hide();
    else
        toolBar->show();
}



//*****************************************
//* Show/hide the window statusbar.
//*****************************************
void NixNote::toggleStatusbar() {
    if (statusBar()->isVisible())
        statusBar()->hide();
    else
        statusBar()->show();
    global.settings->beginGroup("saveState");
    global.settings->setValue("statusBar", statusBar()->isVisible());
    global.settings->endGroup();
}



//**********************************************
//* View the current note's history.
//**********************************************
void NixNote::viewNoteHistory() {
    this->saveContents();
    statusBar()->clearMessage();

    qint32 lid = this->tabWindow->currentBrowser()->lid;
    NoteTable ntable(global.db);
    Note n;
    ntable.get(n,lid,false,false);
    if (n.updateSequenceNum.isSet() && n.updateSequenceNum == 0) {
        QMessageBox::information(0,tr("Unsynchronized Note"), tr("This note has never been synchronized with Evernote"));
        return;
    }

    if (!global.accountsManager->oauthTokenFound()) {
        QString consumerKey = "baumgarr-3523";
        QString consumerSecret = "8d5ee175f8a5d3ec";
        EvernoteOAuthDialog d(consumerKey, consumerSecret, global.server);
        d.setWindowTitle(tr("Log in to Evernote"));
        if(d.exec() != QDialog::Accepted) {
            QMessageBox::critical(0, tr("NixNote"), "Login failed.\n" + d.oauthError());
            return;
        }
        QString token = QString("oauth_token=") +d.oauthResult().authenticationToken +
                       QString("&oauth_token_secret=&edam_shard=")+d.oauthResult().shardId  +
                       QString("&edam_userId=") +QString::number(d.oauthResult().userId) +
                       QString("&edam_expires=") +QString::number(d.oauthResult().expires) +
                       QString("&edam_noteStoreUrl=") + d.oauthResult().noteStoreUrl +
                       QString("&edam_webApiUrlPrefix=") +d.oauthResult().webApiUrlPrefix;

        global.accountsManager->setOAuthToken(token);
    }

    UserTable userTable(global.db);
    User user;
    userTable.getUser(user);
    bool normalUser = false;
    if (user.privilege == PrivilegeLevel::NORMAL)
        normalUser = true;

    NoteHistorySelect dialog;
    QString guid = ntable.getGuid(tabWindow->currentBrowser()->lid);
    QList<NoteVersionId> versions;

    CommunicationManager comm(global.db);
    if (comm.enConnect()) {
        QList<NoteVersionId> versions;
        NoteTable ntable(global.db);
        QString guid = ntable.getGuid(tabWindow->currentBrowser()->lid);
        if (!normalUser)
            comm.listNoteVersions(versions, guid);
    }
    dialog.loadData(versions);
    dialog.exec();
    if (!dialog.importPressed)
        return;
    Note note;
    if (dialog.usn > 0 && !comm.getNoteVersion(note, guid, dialog.usn)) {
        QMessageBox mbox;
        mbox.setText(tr("Error retrieving note."));
        mbox.setWindowTitle(tr("Error retrieving note"));
        mbox.exec();
        return;
    }
    if (dialog.usn <= 0 && !comm.getNote(note, guid,true,true,true)) {
        QMessageBox mbox;
        mbox.setText(tr("Error retrieving note."));
        mbox.setWindowTitle(tr("Error retrieving note"));
        mbox.exec();
        return;
    }
    if (!dialog.replaceCurrentNote()) {
        note.updateSequenceNum = 0;
        note.active = true;
        QUuid uuid;
        QString newGuid = uuid.createUuid().toString().replace("{", "").replace("}", "");
        note.guid = newGuid;
        QList<Resource> resources;
        if (note.resources.isSet())
            resources = note.resources;
        for (int i=0;i<resources.size(); i++) {
            Resource r = resources[i];
            r.updateSequenceNum = 0;
            newGuid = uuid.createUuid().toString().replace("{", "").replace("}", "");
            r.guid = newGuid;
            resources[i] = r;
        }
        note.resources = resources;
        qint32 newLid = ntable.add(0,note,true);
        tabWindow->currentBrowser()->setContent(newLid);
        QMessageBox::information(0,tr("Note Restored"), tr("A new copy has been restored."));
    } else {
        ntable.expunge(lid);
        bool dirty = true;
        if (dialog.usn <=0)
            dirty=false;
        ntable.add(lid,note,dirty);
        tabWindow->currentBrowser()->setContent(0);
        tabWindow->currentBrowser()->setContent(lid);
        QMessageBox::information(0,tr("Note Restored"), tr("Note successfully restored."));
    }
    updateSelectionCriteria();
    setMessage(tr("Note restored"));
}



//****************************************
//* Search for text within a note
//****************************************
void NixNote::findInNote() {
    tabWindow->currentBrowser()->findShortcut();
//    if (!findReplaceWindow->isVisible()) {
//        findReplaceWindow->showFind();
//    } else {
//        if (findReplaceWindow->findLine->hasFocus())
//            findReplaceWindow->hide();
//        else {
//            findReplaceWindow->showFind();
//            findReplaceWindow->findLine->setFocus();
//            findReplaceWindow->findLine->selectAll();
//        }
//    }
}



//*******************************************
//* Search for the next occurrence of text
//* in a note.
//*******************************************
void NixNote::findNextInNote() {
    tabWindow->currentBrowser()->findNextInNote();
}



//*******************************************
//* Search for the previous occurrence of
//* text in a note.
//*******************************************
void NixNote::findPrevInNote() {
    tabWindow->currentBrowser()->findPrevInNote();
}




//*******************************************
//* This just does a null find to reset the
//* text in a note so nothing is highlighted.
//* This is triggered when the find dialog
//* box is hidden.
//*******************************************
void NixNote::findReplaceWindowHidden() {
    tabWindow->currentBrowser()->findReplaceWindowHidden();
}



//**************************************
//* Show find & replace dialog box.
//**************************************
void NixNote::findReplaceInNote() {
    tabWindow->currentBrowser()->findReplaceShortcut();
}



//***************************************
//* Find/replace button pressed, so we
//* need to highlight all the occurrences
//* in a note.
//***************************************
void NixNote::findReplaceInNotePressed() {
   tabWindow->currentBrowser()->findReplaceInNotePressed();
}



//**************************************************
//* Temporarily disable all note editing
//**************************************************
void NixNote::disableEditing() {
   global.disableEditing = !global.disableEditing;
   for (int i=0; i<tabWindow->browserList->size(); i++) {
        NBrowserWindow *browser = tabWindow->browserList->at(i);
        browser->setReadOnly(global.disableEditing && browser->isReadOnly);
   }
   for (int i=0; i<tabWindow->externalList->size(); i++) {
        NBrowserWindow *browser = tabWindow->externalList->at(i)->browser;
        browser->setReadOnly(global.disableEditing && browser->isReadOnly);
   }
}



//*************************************************
//* Replace All button pressed.
//*************************************************
void NixNote::findReplaceAllInNotePressed() {
    tabWindow->currentBrowser()->findReplaceAllInNotePressed();
}




//**************************************************************
//* This queries she shared memory segment at occasional
//* intervals.  This is useful for cross-program communication.
//**************************************************************
void NixNote::heartbeatTimerTriggered() {
    QByteArray data = global.sharedMemory->read();
    if (data.startsWith("SYNCHRONIZE")) {
        QLOG_DEBUG() << "Sync requested by shared memory segment.";
        this->synchronize();
        return;
    }
    if (data.startsWith("IMMEDIATE_SHUTDOWN")) {
        QLOG_ERROR() << "Immediate shutdown requested by shared memory segment.";
        this->closeNixNote();
        return;
    }
    if (data.startsWith("SHOW_WINDOW")) {
        this->raise();
        this->showMaximized();
        return;
    }
    if (data.startsWith("QUERY:")) {
        QList<qint32> results;
        QString query = data.mid(6);
        QLOG_DEBUG() << query;
        FilterCriteria filter;
        filter.setSearchString(query);
        FilterEngine engine;
        engine.filter(&filter, &results);
        QString xmlString;
        QXmlStreamWriter dom(&xmlString);
        dom.setAutoFormatting(true);
        dom.writeStartDocument();
        dom.writeStartElement("response");
        NoteTable ntable(global.db);
        for (int i=0; i<results.size(); i++) {
            dom.writeStartElement("note");
            dom.writeStartElement("lid");
            dom.writeCharacters(QString::number(results[i]));
            dom.writeEndElement();
            Note n;
            ntable.get(n, results[i], false, false);
            if (n.title.isSet()) {
                dom.writeStartElement("title");
                dom.writeCharacters(n.title);
                dom.writeEndElement();
            }
            QString filename = global.fileManager.getThumbnailDirPath("")+QString::number(results[i])+".png";
            QFile file(filename);
            if (file.exists()) {
                dom.writeStartElement("preview");
                dom.writeCharacters(filename);
                dom.writeEndElement();
            }
            dom.writeEndElement();
        }
        dom.writeEndElement();
        dom.writeEndDocument();

        global.sharedMemory->write(xmlString);
    }
    if (data.startsWith("OPEN_NOTE:")) {
        QString number = data.mid(10);
        qint32 note = number.toInt();
        NoteTable noteTable(global.db);
        if (noteTable.exists(note))
            this->openExternalNote(note);
    }
    if (data.startsWith("NEW_NOTE")) {
        this->newExternalNote();
    }
    if (data.startsWith("CMDLINE_QUERY:")) {
        QString xml = data.mid(14);
        CmdLineQuery query;
        query.unwrap(xml.trimmed());
        QString tmpFile = global.fileManager.getTmpDirPath()+query.returnUuid+".txt";
        FilterCriteria *filter = new FilterCriteria();
        FilterEngine engine;
        filter->setSearchString(query.query);
        QList<qint32> lids;
        engine.filter(filter, &lids);
        query.write(lids, tmpFile);
    }
    if (data.startsWith("DELETE_NOTE:")) {
        qint32 lid = data.mid(12).toInt();
        NoteTable noteTable(global.db);
        noteTable.deleteNote(lid, true);
        updateSelectionCriteria();
    }
    if (data.startsWith("EMAIL_NOTE:")) {
        QString xml = data.mid(11);
        EmailNote email;
        email.unwrap(xml);
        email.sendEmail();
    }
    if (data.startsWith("ALTER_NOTE:")) {
        QString xml = data.mid(11);
        AlterNote alter;
        alter.unwrap(xml);
        alter.alterNote();
        updateSelectionCriteria();
    }
    if (data.startsWith("READ_NOTE:")) {
        QString xml = data.mid(10);
        ExtractNoteText data;
        data.unwrap(xml);
        NoteTable ntable(global.db);
        Note n;
        if (ntable.get(n, data.lid,false,false))
            data.text = data.stripTags(n.content);
        else
            data.text=tr("Note not found.");
        QString reply = data.wrap();
        CrossMemoryMapper responseMapper(data.returnUuid);
        if (!responseMapper.attach())
            return;
        responseMapper.write(reply);
        responseMapper.detach();
    }
    if (data.startsWith("SIGNAL_GUI:")) {
        QString cmd = data.mid(12);
        QLOG_DEBUG() << "COMMAND REQUESTED: " << cmd;
        if (cmd.startsWith("SYNCHRONIZE")) {
            this->synchronize();
        }
        if (cmd.startsWith("SCREENSHOT")) {
            this->screenCapture();
        }
        if (cmd.startsWith("SHUTDOWN")) {
            this->close();
        }
        if (cmd.startsWith("SHOW")) {
            if (!isVisible())
                this->toggleVisible();
            this->raise();
            this->activateWindow();
            this->showNormal();
            this->tabWindow->currentBrowser()->editor->setFocus();
        }
        if (cmd.startsWith("NEW_NOTE")) {
            this->newNote();
            if (!isVisible())
                this->toggleVisible();
            this->raise();
            this->activateWindow();
            this->showNormal();
            this->tabWindow->currentBrowser()->editor->setFocus();
        }
        if (cmd.startsWith("NEW_EXTERNAL_NOTE")) {
            this->newExternalNote();
            this->raise();
            this->activateWindow();
            if (tabWindow->lastExternal != NULL) {
                tabWindow->lastExternal->activateWindow();
                tabWindow->lastExternal->showNormal();
                tabWindow->lastExternal->browser->editor->setFocus();
            }
        }
        if (cmd.startsWith("OPEN_EXTERNAL_NOTE")) {
            cmd = cmd.mid(18);
            qint32 lid = cmd.toInt();
            this->openExternalNote(lid);
            if (tabWindow->lastExternal != NULL) {
                tabWindow->lastExternal->activateWindow();
                tabWindow->lastExternal->showNormal();
                tabWindow->lastExternal->browser->editor->setFocus();
            }
            return;
        }
        if (cmd.startsWith("OPEN_NOTE")) {
            bool newTab = false;

            if (!isVisible()) {
                this->toggleVisible();
            }

            if (cmd.startsWith("OPEN_NOTE_NEW_TAB")) {
                newTab = true;
                cmd = cmd.mid(18);
            } else {
                cmd = cmd.mid(10);
            }
            qint32 lid = cmd.toInt();
            QList<qint32> lids;
            lids.append(lid);

            // First, find out if we're already viewing history.  If we are we
            // chop off the end of the history & start a new one
            if (global.filterPosition+1 < global.filterCriteria.size()) {
                while (global.filterPosition+1 < global.filterCriteria.size())
                    delete global.filterCriteria.takeAt(global.filterCriteria.size()-1);
            }

            FilterCriteria *newFilter = new FilterCriteria();
            global.filterCriteria.at(global.filterPosition)->duplicate(*newFilter);

            newFilter->setSelectedNotes(lids);
            newFilter->setLid(lid);
            global.filterCriteria.push_back(newFilter);
            global.filterPosition++;
            this->openNote(newTab);
            this->raise();
            this->activateWindow();
            this->showNormal();
            this->tabWindow->currentBrowser()->editor->setFocus();
        }
    }

    //free(buffer); // Fixes memory leak
}




// Open the dialog status dialog box.
void NixNote::openDatabaseStatus() {
    DatabaseStatus dbstatus;
    dbstatus.exec();
}




// Open the dialog status dialog box.
void NixNote::openImportFolders() {
    WatchFolderDialog dialog;
    dialog.exec();
    if (dialog.okClicked) {
        importManager->reset();
        importManager->setup();
    }
}




// Print the current note
void NixNote::printNote() {
    tabWindow->currentBrowser()->fastPrint = false;
    tabWindow->currentBrowser()->printNote();
}



// Print the current note
void NixNote::emailNote() {
    tabWindow->currentBrowser()->emailNote();
}



// Print the current note
void NixNote::printPreviewNote() {
    tabWindow->currentBrowser()->printPreviewNote();
}



// Print the current note
void NixNote::fastPrintNote() {
    tabWindow->currentBrowser()->fastPrint = true;
    tabWindow->currentBrowser()->printNote();
}



//***********************************************************
//* Toggle the window visibility.  Used when closing to
//* the tray.
//************************************************************
void NixNote::toggleVisible() {
    if (minimizeToTray || closeToTray) {
        if (isMinimized() || !isVisible() || !isActiveWindow()) {
            setWindowState(Qt::WindowActive) ;
            this->show();
            this->raise();
            this->showNormal();
            this->activateWindow();
            this->setFocus();
            return;
        } else {
            showMinimized();
            this->setHidden(true);
            return;
        }
    } else {
        if (isMinimized() || !isActiveWindow()) {
            setWindowState(Qt::WindowActive);
            this->showNormal();
            this->setFocus();
            return;
        } else {
            this->showMinimized();
            return;
        }
    }
}





// The tray icon was activated.  If it was double clicked we restore the
// gui.
void NixNote::trayActivated(QSystemTrayIcon::ActivationReason reason) {
    int doNothing = 0;
    int showHide = 1;
    int newNote = 2;
    int newQuickNote = 3;
    int screenCapture = 4;

    if (reason == QSystemTrayIcon::DoubleClick) {
        global.settings->beginGroup("Appearance");
        int value = global.settings->value("trayDoubleClickAction", -1).toInt();
        global.settings->endGroup();
        if (value == doNothing)
            return;
        if (value == showHide)
            toggleVisible();
        if (value == newNote) {
            if (!isVisible())
                toggleVisible();
            this->newNote();
        }
        if (value == newQuickNote)
            this->newExternalNote();
        if (value == screenCapture)
            this->screenCapture();
    }
    if (reason == QSystemTrayIcon::MiddleClick) {
        global.settings->beginGroup("Appearance");
        int value = global.settings->value("trayMiddleClickAction", -1).toInt();
        global.settings->endGroup();
        if (value == doNothing)
            return;
        if (value == showHide)
            toggleVisible();
        if (value == newNote) {
            if (!isVisible())
                toggleVisible();
            this->newNote();
        }
        if (value == newQuickNote)
            this->newExternalNote();
        if (value == screenCapture)
            this->screenCapture();
    }
    if (reason == QSystemTrayIcon::Trigger) {
        global.settings->beginGroup("Appearance");
        int value = global.settings->value("traySingleClickAction", -1).toInt();
        global.settings->endGroup();
        if (value == doNothing)
            return;
        if (value == showHide)
            toggleVisible();
        if (value == newNote) {
            if (!isVisible())
                toggleVisible();
             this->newNote();
        }
        if (value == newQuickNote)
            this->newExternalNote();
        if (value == screenCapture)
            this->screenCapture();
    }
}



//*******************************************************
//* Event triggered when the window state is changing.
//* Useful when hiding & restoring from the tray.
//*******************************************************
//void NixNote::changeEvent(QEvent *e) {
//    return QMainWindow::changeEvent(e);
//}

bool NixNote::event(QEvent *event) {

    if (event->type() == QEvent::WindowStateChange && isMinimized()) {
        if (minimizeToTray) {
            hide();
            return false;
        }
    }

    if (event->type() == QEvent::Close) {
        if (closeToTray && isVisible())  {
            QLOG_DEBUG() << "overriding close event";
            this->toggleVisible();
            event->ignore();
            return false;
        }
    }
    return QMainWindow::event(event);
}



//*****************************************************
//* Open the Edit/Preferences dialog box.
//*****************************************************
void NixNote::openPreferences() {
    PreferencesDialog prefs;
    prefs.exec();
    if (prefs.okButtonPressed) {
        setSyncTimer();
        bool showTrayIcon = global.showTrayIcon();
        setWindowIcon(global.getIconResource(":windowIcon"));
        trayIcon->setIcon(global.getIconResource(":trayIcon"));
        if (!showTrayIcon) {
            //trayIconBehavior();
            if (!this->isVisible())
                this->show();
            trayIcon->setVisible(false);

        } else {
            trayIcon->setVisible(true);
            minimizeToTray = global.minimizeToTray();
            closeToTray = global.closeToTray();
            //trayIconBehavior();
        }
        indexRunner.officeFound = global.synchronizeAttachments();
    }
    global.setDebugLevel();
}





//**************************************************************
//* Set the automatic sync timer interval.
//**************************************************************
void NixNote::setSyncTimer() {
    global.settings->beginGroup("Sync");
    bool automaticSync = global.settings->value("syncAutomatically", false).toBool();
    int interval = global.settings->value("syncInterval", 15).toInt();
    if (interval < 15)
        interval = 15;
    global.settings->endGroup();
    syncTimer.blockSignals(true);
    syncTimer.stop();
    syncTimer.blockSignals(false);
    if (!automaticSync) {
        return;
    }
    syncTimer.setInterval(60*1000*interval);
    syncTimer.blockSignals(true);
    syncTimer.start();
    syncTimer.blockSignals(false);
}




//*********************************************************************
//* Switch user account.
//*********************************************************************
void NixNote::switchUser() {
    QAction *userSwitch;
    QList<int> checkedEntries;
    int currentAcctPos = 0;
    int newAcctPos = 0;
    for (int i=0; i<menuBar->userAccountActions.size(); i++) {
        userSwitch = menuBar->userAccountActions[i];
        int actionID = userSwitch->data().toInt();
        if (actionID == global.accountsManager->currentId)
            currentAcctPos = i;
        else
            if (userSwitch->isChecked())
                newAcctPos = i;
        if (userSwitch->isChecked()) {
            checkedEntries.append(i);
        }
    }

    // If nothing is checked, we recheck the old one or
    // if more than one is checked, we uncheck the old guy
    if(checkedEntries.size() == 0) {
        menuBar->blockSignals(true);
        menuBar->userAccountActions[currentAcctPos]->setChecked(true);
        menuBar->blockSignals(false);
        return;
    }
    if(checkedEntries.size() > 1) {
        menuBar->blockSignals(true);
        menuBar->userAccountActions[currentAcctPos]->setChecked(false);
        menuBar->blockSignals(false);
        global.accountsManager->currentId = menuBar->userAccountActions[newAcctPos]->data().toInt();
        global.globalSettings->beginGroup("SaveState");
        global.globalSettings->setValue("lastAccessedAccount", global.accountsManager->currentId);
        global.globalSettings->endGroup();
        closeAction->trigger();
        global.sharedMemory->detach();
        QProcess::startDetached(QCoreApplication::applicationFilePath());
        return;
    }
}



//*********************************************************************
//* Add another user account.
//*********************************************************************
void NixNote::addAnotherUser() {
    AddUserAccountDialog dialog;
    dialog.exec();
    if (!dialog.okPushed)
        return;
    QString name = dialog.newAccountName->text().trimmed();
    int six = dialog.newAccountServer->currentIndex();
    QString server = dialog.newAccountServer->itemData(six, Qt::UserRole).toString();
    int newid = global.accountsManager->addId(-1, name, "", server);
    QAction *newAction = new QAction(menuBar);
    newAction->setText(tr("Switch to ") +name);
    newAction->setCheckable(true);
    newAction->setData(newid);
    menuBar->addUserAccount(newAction);
    connect(newAction, SIGNAL(triggered()), this, SLOT(switchUser()));
}


//*********************************************************************
//* Edit a user account
//*********************************************************************
void NixNote::userMaintenance() {
    AccountMaintenanceDialog dialog(menuBar, this);
    dialog.exec();
}


//*********************************************************************
//* Show the note list in a wide view above the editor.
//*********************************************************************
void NixNote::viewNoteListWide() {
    menuBar->blockSignals(true);
    menuBar->viewNoteListNarrow->setChecked(false);
    menuBar->viewNoteListWide->setChecked(true);
    menuBar->blockSignals(false);

    saveNoteColumnPositions();
    saveNoteColumnWidths();
    noteTableView->saveColumnsVisible();

    rightPanelSplitter->setOrientation(Qt::Vertical);
    global.listView = Global::ListViewWide;
    noteTableView->setColumnsVisible();
    noteTableView->repositionColumns();
    noteTableView->resizeColumns();
}



//*********************************************************************
//* Show the note list in a narrow view between the editor & left panel.
//*********************************************************************
void NixNote::viewNoteListNarrow() {

    menuBar->blockSignals(true);
    menuBar->viewNoteListWide->setChecked(false);
    menuBar->viewNoteListNarrow->setChecked(true);
    menuBar->blockSignals(false);

    saveNoteColumnPositions();
    saveNoteColumnWidths();
    noteTableView->saveColumnsVisible();

    rightPanelSplitter->setOrientation(Qt::Horizontal);
    global.listView = Global::listViewNarrow;
    noteTableView->setColumnsVisible();
    noteTableView->repositionColumns();
    noteTableView->resizeColumns();
}



// This is called via global.resourceWatcher when a resource
// has been updated by an external program.  The file name is the
// resource file which starts with the lid.
void NixNote::resourceExternallyUpdated(QString resourceFile) {
    // We do a remove of the watcher at the beginning and a
    // re-add at the end, because some applications don't actually
    // update an existing file, but delete & re-add it. The delete
    // causes NN to stop watching and any later saves are lost.
    // This re-add at the end hopefully fixes it.
    global.resourceWatcher->removePath(resourceFile);
    QString shortName = resourceFile;
    QString dba = global.fileManager.getDbaDirPath();
    shortName.replace(dba, "");
    int pos = shortName.indexOf(".");
    if (pos != -1)
        shortName = shortName.mid(0,pos);
    qint32 lid = shortName.toInt();
    QFile file(resourceFile);
    file.open(QIODevice::ReadOnly);
    QByteArray ba = file.readAll();
    file.close();
    QByteArray newHash = QCryptographicHash::hash(ba, QCryptographicHash::Md5);
    ResourceTable resTable(global.db);
    QByteArray oldHash = resTable.getDataHash(lid);
    if (oldHash != newHash) {
        qint32 noteLid = resTable.getNoteLid(lid);
        resTable.updateResourceHash(lid, newHash);
        NoteTable noteTable(global.db);
        noteTable.updateEnmediaHash(noteLid, oldHash, newHash, true);
        tabWindow->updateResourceHash(noteLid, oldHash, newHash);
    }
    AttachmentIconBuilder icon;
    icon.buildIcon(lid,resourceFile);
    global.resourceWatcher->addPath(resourceFile);
}


//*********************************************************************
//* Screen capture request.
//*********************************************************************
void NixNote::screenCapture() {
    if (noteButton->property("currentNoteButton").toInt() != NewScreenNote) {
        noteButton->setText(screenCaptureButton->text());
        noteButton->setIcon(screenCaptureButton->icon());
        noteButton->setProperty("currentNoteButton", NewScreenNote);
    }
    this->hide();
    sleep(1);

    ScreenCapture sc;
    sc.exec();
    QPixmap pix = sc.getSelection();
    this->show();
    ConfigStore cs(global.db);
    qint32 lid = cs.incrementLidCounter();

    QCryptographicHash md5hash(QCryptographicHash::Md5);
    QByteArray data;
    QBuffer buffer(&data);
    buffer.open(QIODevice::WriteOnly);
    pix.save(&buffer, "PNG");

    QByteArray hash = md5hash.hash(data, QCryptographicHash::Md5);

    // * Start setting up the new note
    Note newNote;
    newNote.guid = QString::number(lid);
    newNote.title = tr("Screen Capture");

    NotebookTable bookTable(global.db);
    QString notebook;
    notebook = bookTable.getDefaultNotebookGuid();
    newNote.notebookGuid = notebook;

    QString newNoteBody = QString("<?xml version=\"1.0\" encoding=\"UTF-8\"?>")+
           QString("<!DOCTYPE en-note SYSTEM \"http://xml.evernote.com/pub/enml2.dtd\">")+
           QString("<en-note style=\"word-wrap: break-word; -webkit-nbsp-mode: space; -webkit-line-break: after-white-space;\">");

    QString mime = "image/png";
    QString enMedia =QString("<en-media hash=\"") +hash.toHex() +QString("\" border=\"0\"")
            +QString(" type=\"" +mime +"\" ")
            +QString("/>");
    newNoteBody.append(enMedia + QString("</en-note>"));
    newNote.content = newNoteBody;
    newNote.active = true;
    newNote.created = QDateTime::currentMSecsSinceEpoch();;
    newNote.updated = newNote.created;
    newNote.updateSequenceNum = 0;

    NoteTable ntable(global.db);
    ntable.add(lid, newNote, true);
    QString noteGuid = ntable.getGuid(lid);
    qint32 noteLid  = lid;
    lid = cs.incrementLidCounter();


    // Start creating the new resource
    Resource newRes;
    Data d;
    d.body = data;
    d.bodyHash = hash;
    d.size = data.size();
    newRes.mime = mime;
    ResourceAttributes attributes;
    attributes.attachment = false;
    newRes.active = true;
    newRes.guid = QString::number(lid);
    newRes.noteGuid = noteGuid;
    newRes.updateSequenceNum = 0;
    newRes.data = d;
    newRes.attributes = attributes;
    ResourceTable restable(global.db);
    restable.add(lid, newRes, true, noteLid);

    updateSelectionCriteria();
}



// Reindex all notes & resources
void NixNote::reindexDatabase() {

    int response = QMessageBox::question ( this, tr("Reindex Database"), tr("Reindex the entire database?"), QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (response != QMessageBox::Yes)
        return;

    NoteTable ntable(global.db);
    ResourceTable rtable(global.db);
    rtable.reindexAllResources();
    ntable.reindexAllNotes();

    setMessage(tr("Notes will be reindexed."));
}



// Open/Close selected notebooks
void NixNote::openCloseNotebooks() {
    CloseNotebookDialog dialog;
    dialog.exec();
    if (dialog.okPressed) {
        notebookTreeView->rebuildNotebookTreeNeeded = true;
        this->updateSelectionCriteria();
        notebookTreeView->rebuildTree();
    }
}



// Capture an image from the webcam and create a new note
void NixNote::newWebcamNote() {
    if (!webcamPluginAvailable) {
        QMessageBox::critical(this, tr("Plugin Error"), tr("Webcam plugin not found or could not be loaded"));
        return;
    }
    webcamInterface->initialize();

    if (noteButton->property("currentNoteButton") != NewWebcamNote) {
        noteButton->setText(newWebcamNoteButton->text());
        noteButton->setIcon(newWebcamNoteButton->icon());
        noteButton->setProperty("currentNoteButton", NewWebcamNote);
    }

    //WebcamCaptureDialog dialog;
    QMessageBox msgBox;
    msgBox.setText(tr("Unable to find webcam or capture image."));
    msgBox.setWindowTitle(tr("Webcam Error"));

    webcamInterface->initialize();
    // Check for error reading camera
    if (!webcamInterface->isWebcamReady()) {
        msgBox.exec();
        return;
    }

    webcamInterface->exec();

    if (!webcamInterface->okPressed()) {
        return;
    }

    QImage img;
    // Check for webcam error
    if (!webcamInterface->isWebcamReady() ||
            !webcamInterface->getImage(img)) {
        msgBox.exec();
        return;
    }

    //Start generating the new note
    Note newNote;
    NoteTable ntable(global.db);

    QByteArray data;
    QBuffer buffer(&data);
    buffer.open(QIODevice::WriteOnly);
    img.save(&buffer, "PNG");

    ConfigStore cs(global.db);
    qint32 lid = cs.incrementLidCounter();

    QCryptographicHash md5hash(QCryptographicHash::Md5);
    QByteArray hash = md5hash.hash(data, QCryptographicHash::Md5);

    // * Start setting up the new note
    newNote.guid = QString::number(lid);
    newNote.title = "Webcam Note";

    NotebookTable notebookTable(global.db);
    if (notebookTreeView->selectedItems().size() == 0) {
        newNote.notebookGuid = notebookTable.getDefaultNotebookGuid();
    } else {
        NNotebookViewItem *item = (NNotebookViewItem*)notebookTreeView->selectedItems().at(0);
        qint32 lid = item->lid;

        // If we have a stack, we find the first notebook (in alphabetical order) for the new note.
        if (lid == 0) {
            QString stackName = notebookTreeView->selectedItems().at(0)->data(0, Qt::DisplayRole).toString();
            QList<qint32> notebooks;
            notebookTable.getStack(notebooks, stackName);
            QString priorName;
            Notebook book;
            if (notebooks.size() > 0) {
                for (int i=0; i<notebooks.size(); i++) {
                    qint32 priorLid = notebooks[i];
                    notebookTable.get(book, priorLid);
                    QString currentName = "";
                    if (book.name.isSet())
                        currentName = book.name;
                    if (currentName.toUpper() < priorName.toUpper() || priorName == "") {
                        lid = notebooks[i];
                    }
                    priorLid = notebooks[i];
                    priorName = currentName;
                }
            }
        }
        QString notebookGuid;
        notebookTable.getGuid(notebookGuid, lid);
        newNote.notebookGuid = notebookGuid;
    }

    QString newNoteBody = QString("<?xml version=\"1.0\" encoding=\"UTF-8\"?>")+
           QString("<!DOCTYPE en-note SYSTEM \"http://xml.evernote.com/pub/enml2.dtd\">")+
           QString("<en-note style=\"word-wrap: break-word; -webkit-nbsp-mode: space; -webkit-line-break: after-white-space;\">");


    QString enMedia =QString("<en-media hash=\"") +hash.toHex() +QString("\" border=\"0\"")
            +QString(" type=\"image/png\" ")
            +QString("/>");
    newNoteBody.append(enMedia + QString("</en-note>"));
    newNote.content = newNoteBody;
    newNote.active = true;
    newNote.created = QDateTime::currentMSecsSinceEpoch();;
    newNote.updated = newNote.created;
    newNote.updateSequenceNum = 0;


    qint32 noteLid = lid;
    ntable.add(lid, newNote, true);
    QString noteGuid = ntable.getGuid(lid);
    lid = cs.incrementLidCounter();


    // Start creating the new resource
    Resource newRes;
    Data d;
    ResourceAttributes attributes;
    d.body = data;
    d.bodyHash = hash;
    d.size = data.size();
    newRes.mime = "image/png";
    attributes.fileName = "";
    attributes.attachment = false;
    newRes.active = true;
    newRes.guid = QString::number(lid);
    newRes.noteGuid = noteGuid;
    newRes.updateSequenceNum = 0;
    newRes.data = d;
    newRes.attributes = attributes;
    ResourceTable restable(global.db);
    restable.add(lid, newRes, true, noteLid);


    FilterCriteria *criteria = new FilterCriteria();
    global.filterCriteria[global.filterPosition]->duplicate(*criteria);
    criteria->setLid(noteLid);
    global.filterCriteria.append(criteria);
    global.filterPosition++;
    openNote(false);
    updateSelectionCriteria();

    if (global.newNoteFocusToTitle()) {
        tabWindow->currentBrowser()->noteTitle.setFocus();
        tabWindow->currentBrowser()->noteTitle.selectAll();
    } else
        tabWindow->currentBrowser()->editor->setFocus();

    return;
}



// Reindex the current note
void NixNote::reindexCurrentNote() {
    tabWindow->currentBrowser()->saveNoteContent();

    NoteIndexer indexer(global.db);
    indexer.indexNote(tabWindow->currentBrowser()->lid);

    ResourceTable rtable(global.db);
    QList<qint32> rlids;
    rtable.getResourceList(rlids, tabWindow->currentBrowser()->lid);
    for (int i=0; i<rlids.size(); i++) {
        indexer.indexResource(rlids[i]);
    }
    QMessageBox::information(0,tr("Note Reindexed"),"Reindex Complete");
}


// Delete the note we are currently viewing
void NixNote::deleteCurrentNote() {
    qint32 lid = tabWindow->currentBrowser()->lid;

    QString typeDelete;
    QString msg;
    FilterCriteria *f  = global.filterCriteria[global.filterPosition];
    bool expunged = false;
    typeDelete = tr("Delete ");

    if (f->isDeletedOnlySet() && f->getDeletedOnly()) {
        typeDelete = tr("Permanently delete ");
        expunged = true;
    }

    msg = typeDelete + tr("this note?");

    if (global.confirmDeletes()) {
        QMessageBox msgBox;
        msgBox.setWindowTitle(tr("Verify Delete"));
        msgBox.setText(msg);
        msgBox.setStandardButtons(QMessageBox::Yes|QMessageBox::No);
        msgBox.setIcon(QMessageBox::Question);
        msgBox.setDefaultButton(QMessageBox::Yes);
        int rc = msgBox.exec();
        if (rc != QMessageBox::Yes)
            return;
    }

    NoteTable ntable(global.db);
    NSqlQuery sql(global.db);
    sql.prepare("Delete from filter where lid=:lid");
    ntable.deleteNote(lid, true);
    if (expunged)
        ntable.expunge(lid);
    sql.bindValue(":lid", lid);
    sql.exec();
    sql.finish();
    delete global.cache[lid];
    global.cache.remove(lid);
    QList<qint32> lids;
    lids.append(lid);
    emit(notesDeleted(lids));
}




// Duplicate the current note
void NixNote::duplicateCurrentNote() {
    tabWindow->currentBrowser()->saveNoteContent();
    qint32 oldLid = tabWindow->currentBrowser()->lid;
    qint32 newLid;
    NoteTable ntable(global.db);
    newLid = ntable.duplicateNote(oldLid);

    FilterCriteria *criteria = new FilterCriteria();
    global.filterCriteria[global.filterPosition]->duplicate(*criteria);
    criteria->setLid(newLid);
    global.filterCriteria.append(criteria);
    global.filterPosition++;
    openNote(false);
    updateSelectionCriteria();
    tabWindow->currentBrowser()->editor->setFocus();
}


// "Pin" the current note.  This makes sure it appears in all searches
void NixNote::pinCurrentNote() {
    qint32 lid = tabWindow->currentBrowser()->lid;
    NoteTable ntable(global.db);
    ntable.pinNote(lid, true);
    updateSelectionCriteria();
}




// "Unpin" the current note so it doesn't appear in every search
void NixNote::unpinCurrentNote() {
    qint32 lid = tabWindow->currentBrowser()->lid;
    NoteTable ntable(global.db);
    ntable.pinNote(lid, false);
    updateSelectionCriteria();
}




// Run the spell checker
void NixNote::spellCheckCurrentNote() {
    tabWindow->currentBrowser()->spellCheckPressed();
}




// Pause/unpause indexing.
void NixNote::pauseIndexing(bool value) {
    if (menuBar->pauseIndexingAction->isChecked()) {
       indexRunner.pauseIndexing = true;
       return;
    }
    indexRunner.pauseIndexing = value;
}


// View the mesasge log.  These same messages show up in a terminal
void NixNote::openMessageLog() {
    LogViewer viewer;
    viewer.exec();
}



// Note button has been pressed, so we need to know what type of note to create
void NixNote::noteButtonClicked() {
    if (noteButton->property("currentNoteButton").toInt() == NewTextNote)
        newNote();
    if (noteButton->property("currentNoteButton").toInt() == NewWebcamNote)
        newWebcamNote();
    if (noteButton->property("currentNoteButton").toInt() == NewScreenNote)
        screenCapture();
}


// Show a url to the debugging log
void NixNote::showDesktopUrl(const QUrl &url) {
    QLOG_DEBUG() << url.toString();
}



// Reload the icons after a theme switch
void NixNote::reloadIcons() {
    QString newThemeName = "";
    global.settings->beginGroup("Appearance");
    QString currentTheme = global.settings->value("themeName", "").toString();
    global.settings->endGroup();

    QAction *themeSwitch;
    QList<int> checkedEntries;
    int currentThemePos = 0;
    int newThemePos = 0;
    for (int i=0; i<menuBar->themeActions.size(); i++) {
        themeSwitch = menuBar->themeActions[i];
        QString checkedTheme = themeSwitch->data().toString();
        if (checkedTheme == currentTheme)
            currentThemePos = i;
        else {
            if (themeSwitch->isChecked())
                newThemePos = i;
        }
        if (themeSwitch->isChecked()) {
            checkedEntries.append(i);
        }
    }

    // If nothing is checked, we recheck the old one or
    // if more than one is checked, we uncheck the old guy
    if(checkedEntries.size() == 0) {
        menuBar->blockSignals(true);
        menuBar->themeActions[currentThemePos]->setChecked(true);
        menuBar->blockSignals(false);
    }
    if(checkedEntries.size() > 0) {
        menuBar->blockSignals(true);
        menuBar->themeActions[currentThemePos]->setChecked(false);
        menuBar->blockSignals(false);
        global.settings->beginGroup("Appearance");
        newThemeName = menuBar->themeActions[newThemePos]->data().toString();
        if (newThemeName != "")
            global.settings->setValue("themeName", newThemeName);
        else
            global.settings->remove("themeName");
        global.settings->endGroup();
        global.loadTheme(global.resourceList,global.colorList,newThemeName);
    }

    setWindowIcon(QIcon(global.getIconResource(":windowIcon")));
    leftArrowButton->setIcon(global.getIconResource(":leftArrowIcon"));
    rightArrowButton->setIcon(global.getIconResource(":rightArrowIcon"));
    homeButton->setIcon(global.getIconResource(":homeIcon"));
    syncButton->setIcon(global.getIconResource(":synchronizeIcon"));
    printNoteButton->setIcon(global.getIconResource(":printerIcon"));
    newNoteButton->setIcon(global.getIconResource(":newNoteIcon"));
    newWebcamNoteButton->setIcon(global.getIconResource(":webcamIcon"));
    deleteNoteButton->setIcon(global.getIconResource(":deleteIcon"));
    usageButton->setIcon(global.getIconResource(":usageIcon"));
    trayIcon->setIcon(global.getIconResource(":trayIcon"));
    screenCaptureButton->setIcon(global.getIconResource(":screenCaptureIcon"));
    trunkButton->setIcon(global.getIconResource(":trunkIcon"));
    emailButton->setIcon(global.getIconResource(":emailIcon"));
    notebookTreeView->reloadIcons();
    tagTreeView->reloadIcons();
    attributeTree->reloadIcons();
    trashTree->reloadIcons();
    searchTreeView->reloadIcons();
    searchText->reloadIcons();
    favoritesTreeView->reloadIcons();
    tabWindow->reloadIcons();

    tabWindow->changeEditorStyle();

    QString themeInformation = global.getResourceFileName(global.resourceList, ":themeInformation");
    menuBar->themeInformationAction->setVisible(true);
    if (themeInformation.startsWith("http://", Qt::CaseInsensitive))
        return;
    if (themeInformation.startsWith("https://", Qt::CaseInsensitive))
        return;

    QFile f(themeInformation);
    if (!f.exists() && newThemeName != "")
        menuBar->themeInformationAction->setVisible(false);
}




// Show/Hide the favorites tree on the left side
void NixNote::toggleFavoritesTree() {
    bool visible = true;
    if (favoritesTreeView->isVisible())
        visible = false;
    global.settings->beginGroup("SaveState");
    global.settings->setValue("favoritesTreeVisible", visible);
    global.settings->endGroup();
    favoritesTreeView->setVisible(visible);
    checkLeftPanelSeparators();
}



// Show/Hide the notebook tree on the left side
void NixNote::toggleNotebookTree() {
    bool visible = true;
    if (notebookTreeView->isVisible())
        visible = false;
    global.settings->beginGroup("SaveState");
    global.settings->setValue("notebookTreeVisible", visible);
    global.settings->endGroup();
    notebookTreeView->setVisible(visible);
    checkLeftPanelSeparators();
}


// Show/Hide the tag tree on the left side
void NixNote::toggleTagTree() {
    bool visible = true;
    if (tagTreeView->isVisible())
        visible = false;
    global.settings->beginGroup("SaveState");
    global.settings->setValue("tagTreeVisible", visible);
    global.settings->endGroup();
    tagTreeView->setVisible(visible);
    checkLeftPanelSeparators();
}


// Show/Hide the saved search tree on the left side
void NixNote::toggleSavedSearchTree() {
    bool visible = true;
    if (searchTreeView->isVisible())
        visible = false;
    global.settings->beginGroup("SaveState");
    global.settings->setValue("savedSearchTreeVisible", visible);
    global.settings->endGroup();
    searchTreeView->setVisible(visible);
    checkLeftPanelSeparators();
}


// Show/Hide the attributes tree on the left side
void NixNote::toggleAttributesTree() {
    bool visible = true;
    if (attributeTree->isVisible())
        visible = false;
    global.settings->beginGroup("SaveState");
    global.settings->setValue("attributeTreeVisible", visible);
    global.settings->endGroup();
    attributeTree->setVisible(visible);
    checkLeftPanelSeparators();
}

// Show/Hide the trash tree on the left side
void NixNote::toggleTrashTree() {
    bool visible = true;
    if (trashTree->isVisible())
        visible = false;
    global.settings->beginGroup("SaveState");
    global.settings->setValue("trashTreeVisible", visible);
    global.settings->endGroup();
    trashTree->setVisible(visible);
    checkLeftPanelSeparators();
}



// This function will show/hide all of the separators between the trees on the left side
// of the gui.
void NixNote::checkLeftPanelSeparators() {
    bool s1 = false;
    bool s2 = false;
    bool s3 = false;
    bool s4 = false;
    bool s5 = false;

    bool tags;
    bool notebooks;
    bool favorites;
    bool searches;
    bool attributes;
    bool trash;

    global.settings->beginGroup("SaveState");
    favorites = global.settings->value("favoritesTreeVisible", true).toBool();
    notebooks = global.settings->value("notebookTreeVisible",true).toBool();
    tags = global.settings->value("tagTreeVisible", true).toBool();
    searches = global.settings->value("savedSearchTreeVisible", true).toBool();
    attributes = global.settings->value("attributeTreeVisible", true).toBool();
    trash = global.settings->value("trashTreeVisible", true).toBool();
    global.settings->endGroup();

    if (favorites && (notebooks || tags || searches || attributes || trash)) {
        s1=true;
    }
    if (notebooks && (tags || searches || attributes || trash)) {
        s2=true;
    }
    if (tags && (searches || attributes || trash)) {
        s3=true;
    }
    if (searches && (attributes || trash)) {
        s4=true;
    }
    if (attributes && trash) {
        s5=true;
    }

    leftSeparator1->setVisible(s1);
    leftSeparator2->setVisible(s2);
    leftSeparator3->setVisible(s3);
    leftseparator4->setVisible(s4);
    leftSeparator5->setVisible(s5);
}



// Load any shortcut keys
void NixNote::setupShortcut(QShortcut *action, QString text) {
    if (!global.shortcutKeys->containsAction(&text))
        return;
    QKeySequence key(global.shortcutKeys->getShortcut(&text));
    action->setKey(key);
}



// Make sure the toolbar checkbox & the menu match.
void NixNote::toolbarVisibilityChanged() {
    menuBar->viewToolbar->blockSignals(true);
    menuBar->viewToolbar->setChecked(toolBar->isVisible());
    menuBar->viewToolbar->blockSignals(false);
}


//Turn on presentation mode
void NixNote::presentationModeOn() {
    this->leftScroll->hide();
//    this->toggleLeftPanel();
//    this->toggleLeftPanel();
    this->menuBar->setVisible(false);
    this->topRightWidget->setVisible(false);
    this->toolBar->setVisible(false);
    this->statusBar()->setVisible(false);
    this->showFullScreen();
    global.isFullscreen=true;
    tabWindow->currentBrowser()->buttonBar->hide();

    FaderDialog *d = new FaderDialog();
    d->setText(tr("Press ESC to exit."));
    d->show();
}

//Turn off presentation mode
void NixNote::presentationModeOff() {
    if (!this->isFullScreen())
        return;
    if (menuBar->viewLeftPanel->isChecked())
        leftScroll->show();
    if (menuBar->viewNoteList->isChecked())
        topRightWidget->show();
    if (menuBar->viewStatusbar->isChecked())
        statusBar()->show();
    menuBar->show();
    toolBar->show();
    global.isFullscreen=false;
    if (!global.autoHideEditorToolbar)
        tabWindow->currentBrowser()->buttonBar->show();
    this->showMaximized();
}






// Check to see if plugins are avaialble and they match
// the correct version expected. Load them if possible.
void NixNote::loadPlugins() {
    webcamPluginAvailable = false;

    QStringList dirList;
    dirList.append(global.fileManager.getProgramDirPath(""));
    dirList.append(global.fileManager.getProgramDirPath("")+"/plugins");
    dirList.append("/usr/lib/nixnote2/");
    dirList.append("/usr/local/lib/nixnote2/");
    dirList.append("/usr/local/lib");
    dirList.append("/usr/lib");

    // Start loading plugins
    for (int i=0; i<dirList.size(); i++) {
        QDir pluginsDir(dirList[i]);
        QStringList filter;
        filter.append("libwebcamplugin.so");
        filter.append("libhunspellplugin.so");
        foreach (QString fileName, pluginsDir.entryList(filter)) {
            QPluginLoader pluginLoader(pluginsDir.absoluteFilePath(fileName));
            QObject *plugin = pluginLoader.instance();
            if (fileName == "libwebcamplugin.so") {
                if (plugin) {
                    webcamInterface = qobject_cast<WebCamInterface *>(plugin);
                    if (webcamInterface) {
                        webcamPluginAvailable = true;
                    }
                } else {
                    QLOG_ERROR() << tr("Error loading Webcam plugin: ") << pluginLoader.errorString();
                }
            }

            // The Hunspell plugin isn't actually used here. We just use this as a
            // check to be sure that the menu should be available.
            if (fileName == "libhunspellplugin.so") {
                if (plugin) {
                    HunspellInterface *hunspellInterface;
                    hunspellInterface = qobject_cast<HunspellInterface *>(plugin);
                    if (hunspellInterface) {
                        hunspellPluginAvailable = true;
                    }
                    delete hunspellInterface;
                } else {
                    QLOG_ERROR() << tr("Error loading Hunspell plugin: ") << pluginLoader.errorString();
                }
            }
        }
    }
}



// Export selected notes as PDF files.
void NixNote::exportAsPdf() {


    QList<qint32> lids;
    noteTableView->getSelectedLids(lids);

    if (pdfExportWindow == NULL) {
        pdfExportWindow = new QWebView();
        connect(pdfExportWindow, SIGNAL(loadFinished(bool)), this, SLOT(exportAsPdfReady(bool)));
    }


    if (lids.size() <= 0) {
        QList<qint32> lids;
        noteTableView->getSelectedLids(lids);

        QString file = QFileDialog::getSaveFileName(0,tr("PDF Export"), "","*.pdf");
        if (file == "")
            return;

        QPrinter printer;
        printer.setOutputFormat(QPrinter::PdfFormat);
        printer.setResolution(QPrinter::HighResolution);
        printer.setPaperSize(QPrinter::A4);
        printer.setOutputFileName(file);

        printer.setDocName(tabWindow->currentBrowser()->noteTitle.text());
        tabWindow->currentBrowser()->editor->print(&printer);
        QMessageBox::information(0, tr("NixNote"), tr("Export complete"));

        return;
    }

    NoteTable noteTable(global.db);
    QByteArray content;
    content.clear();
    NoteFormatter formatter;

    QProgressDialog *progress = new QProgressDialog(0);
    progress->setMinimum(0);
    progress->setWindowTitle(tr("Exporting Notes as PDF"));
    progress->setLabelText(tr("Exporting notes as PDF"));
    progress->setMaximum(lids.size());
    progress->setVisible(true);
    progress->setWindowModality(Qt::ApplicationModal);
    progress->setCancelButton(0);
    progress->show();
    for (int i=0; i<lids.size(); i++) {
        Note n;
        noteTable.get(n,lids[i],true,false);
        formatter.setNote(n,false);
        if (n.title.isSet())
            content.append("<h2>"+n.title+"</h2>");
        content.append(formatter.rebuildNoteHTML());
        if (i<lids.size()-1)
            content.append("<p style=\"page-break-after:always;\"></p>");
        progress->setValue(i);
    }

    progress->hide();
    delete progress;
    pdfExportWindow->setHtml(content);
    return;
}


// Slot called when notes that were exported as PDF files are ready to be printed
void NixNote::exportAsPdfReady(bool) {
    QString file = QFileDialog::getSaveFileName(0,tr("PDF Export"), "","*.pdf");

    if (file == "")
        return;

    QPrinter printer;
    printer.setOutputFormat(QPrinter::PdfFormat);
    printer.setResolution(QPrinter::HighResolution);
    printer.setPaperSize(QPrinter::A4);
    printer.setOutputFileName(file);
    pdfExportWindow->print(&printer);
    QMessageBox::information(0, tr("NixNote"), tr("Export complete"));
}
