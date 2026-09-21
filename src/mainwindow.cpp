#include "mainwindow.h"
#include "./ui_mainwindow.h"

#include <QApplication>
#include <QTreeView>
#include <QFileDialog>
#include <QProgressDialog>
#include <QMessageBox>
#include <QInputDialog>
#include <QDirIterator>
#include <QMouseEvent>
#include <QStandardPaths>
#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QMediaFormat>
#include <QMimeType>
#include <QDateTime>
#include <optional>
#include <kddockwidgets/LayoutSaver.h>

#include "version.h"
#include "sessionimport.h"
#include "dependencykey.h"
#include "pluginhost.h"
#include "ui/docks/DockRegistry.h"
#include "ui/docks/DockFeature.h"
#include "ui/docks/AppContext.h"
#include "ui/docks/logbook/LogbookDockFeature.h"
#include "ui/docks/plot/PlotDockFeature.h"
#include "ui/docks/plotselection/PlotSelectionDockFeature.h"
#include "ui/docks/video/VideoDockFeature.h"
#include "ui/docks/plot/PlotWidget.h"
#include "ui/docks/logbook/LogbookView.h"
#include "ui/docks/video/VideoWidget.h"
#include "preferences/preferencesdialog.h"
#include "preferences/preferencesmanager.h"
#include "preferences/preferencekeys.h"
#include "sessiondata.h"
#include "plotviewsettingsmodel.h"
#include "plotmodel.h"
#include "markermodel.h"
#include "markerregistry.h"
#include "momentmodel.h"
#include "plotrangemodel.h"
#include "measuremodel.h"
#include "jobqueue.h"
#include "plotrequests.h"
#include "units/unitconverter.h"
#include "calculations/builtincalculations.h"
#include "fusion/fusionregistration.h"
#include "preferences/enginepreferenceprovider.h"
#include "calculations/attributeregistration.h"
#include "altitudemarkerfeature.h"
#include "logbookcolumn.h"
#include "logbookmanager.h"
#include "profile.h"
#include "profilemanager.h"
#include "profilestatebridge.h"
#include "manageprofilesdialog.h"

namespace {

QSet<QString> supportedVideoSuffixes()
{
    QSet<QString> exts;
    QMediaFormat defaultFmt;
    for (auto fmt : defaultFmt.supportedFileFormats(QMediaFormat::Decode)) {
        QMediaFormat mf;
        mf.setFileFormat(fmt);
        QMimeType mime = mf.mimeType();
        if (mime.name().startsWith(QLatin1String("video/"))) {
            for (const QString &s : mime.suffixes())
                exts.insert(s.toLower());
        }
    }
    return exts;
}

} // anonymous namespace

namespace FlySight {

MainWindow::MainWindow(QWidget *parent)
    : KDDockWidgets::QtWidgets::MainWindow(
          QStringLiteral("MainWindow"),
          KDDockWidgets::MainWindowOptions{
              KDDockWidgets::MainWindowOption_ManualInit
          },
          parent)
    , m_settings(new QSettings(this))
    , m_plotViewSettingsModel(new PlotViewSettingsModel(m_settings, this))
    , ui(new Ui::MainWindow)
    , model(new SessionModel(this))
    , m_plotModel (new PlotModel(this))
    , m_markerModel (new MarkerModel(this))
{
    ui->setupUi(this);
    manualInit();

    // Give models access to QSettings for persisting enabled state
    m_plotModel->setSettings(m_settings);
    m_markerModel->setSettings(m_settings);

    // Create MomentModel and wire it to SessionModel for dependency tracking
    m_momentModel = new MomentModel(this);
    m_momentModel->setSessionModel(model);

    // Give MarkerRegistry and MarkerModel access to MomentModel so that
    // marker registration and enablement changes propagate to the moment system.
    MarkerRegistry::instance()->setMomentModel(m_momentModel);
    m_markerModel->setMomentModel(m_momentModel);

    // Register the mouse cursor as a moment (always enabled, always visible)
    {
        MomentTraits mouseTraits;
        mouseTraits.positionSource = PositionSource::MouseInput;
        mouseTraits.interaction = Interaction::None;
        mouseTraits.plotPresentation = PlotPresentation::VerticalLine;
        mouseTraits.plotBubble = PlotBubble::None;
        mouseTraits.mapPresentation = MapPresentation::LargeDot;
        mouseTraits.legendVisibility = LegendVisibility::Visible;
        m_momentModel->registerMoment(QStringLiteral("mouse"), tr("Mouse"), mouseTraits);
    }

    // Register the video cursor as a moment
    {
        MomentTraits videoTraits;
        videoTraits.positionSource = PositionSource::External;
        videoTraits.interaction = Interaction::PlaybackAndDrag;
        videoTraits.plotPresentation = PlotPresentation::VerticalLine;
        videoTraits.plotBubble = PlotBubble::None;
        videoTraits.mapPresentation = MapPresentation::LargeDot;
        videoTraits.legendVisibility = LegendVisibility::Visible;
        m_momentModel->registerMoment(QStringLiteral("video"), tr("Video"), videoTraits);
    }

    // Register built-in plots and markers BEFORE initializing preferences
    // so that we can dynamically register per-plot and per-marker preferences
    registerBuiltInPlots();
    registerBuiltInMarkers();

    // Register built-in session attributes (e.g., Description, StartTime, Duration)
    registerBuiltInAttributes();

    // Ensure default profiles exist on first launch (returns true if first launch)
    m_isFirstLaunch = ProfileManager::instance().ensureDefaultProfilesExist();

    // Load persisted logbook column configuration (or defaults on first launch).
    // This emits columnsChanged(), which triggers SessionModel::rebuildColumns().
    LogbookColumnStore::instance().load();

    // Initialize plugins (may register additional plots/markers)
#ifdef Q_OS_MACOS
    QString defaultDir = QCoreApplication::applicationDirPath() + "/../Resources/python_plugins";
#else
    QString defaultDir = QCoreApplication::applicationDirPath() + "/python_plugins";
#endif
    QString pluginDir = qEnvironmentVariable("FLYSIGHT_PLUGINS", defaultDir);
    PluginHost::instance().initialise(pluginDir);

    // Initialize preferences (must come AFTER plots and markers are registered)
    initializePreferences();

    // Let calculations read the preferences they declare as inputs, and have
    // preference changes invalidate their results (after the preferences are
    // registered).
    EnginePreferenceProvider::install();

    // Register the built-in calculations. Plugins registered theirs above, so
    // a plugin that declares a built-in output is tried first.
    registerBuiltInCalculations();
    // Sensor fusion lives in its own library (the only one that links GTSAM);
    // it is explicit, so registering it costs nothing until a plot asks for it.
    Fusion::registerFusionCalculations();
    registerBuiltInCalculationMetadata();

    // Instantiate and register altitude markers (must come after calculations are registered)
    m_altitudeMarkerManager = new AltitudeMarkerManager(this);
    m_altitudeMarkerManager->refresh();

    // Bring up the logbook. Every session starts as a stub - also those of a
    // legacy flat index, which knows real SESSION_IDs but caches no column
    // values: the idle column worker fills them in and its completion flush
    // writes the extended index. No session file is parsed here and none is
    // ever rewritten by starting the application.
    LogbookManager &logbook = LogbookManager::instance();
    logbook.initialize();

    if (logbook.hasDeferredScan()) {
        model->populateFromUuids(logbook.scannedUuids());
    } else {
        QVector<LogbookColumn> liveColumns = LogbookColumnStore::instance().enabledColumns();
        model->populateFromIndex(logbook.cachedColumnValues(liveColumns), logbook.lastAccessedMap());
    }
    model->startColumnWorker();

    // Create range model for synchronizing plot x-axis range with other docks
    m_rangeModel = new PlotRangeModel(this);

    // Create measure model for measure tool data
    m_measureModel = new MeasureModel(this);

    // The job queue for background (explicit) calculations, then the component
    // that turns plot-list gestures into jobs - in that order, and here: the
    // session model is populated and every calculation is registered, and no
    // dock exists yet. Nothing at start-up is a gesture: the checked plots
    // restored by setPlots() below and a first-launch applyProfile() reach
    // PlotRequests as ordinary model changes, so starting the application
    // starts no job.
    m_jobQueue = new JobQueue(model, this);
    m_plotRequests = new PlotRequests(model, m_plotModel, m_jobQueue, this);

    // Create all docks via registry
    AppContext ctx;
    ctx.sessionModel = model;
    ctx.plotModel = m_plotModel;
    ctx.markerModel = m_markerModel;
    ctx.momentModel = m_momentModel;
    ctx.rangeModel = m_rangeModel;
    ctx.plotViewSettings = m_plotViewSettingsModel;
    ctx.measureModel = m_measureModel;
    ctx.jobQueue = m_jobQueue;
    ctx.plotRequests = m_plotRequests;
    ctx.settings = m_settings;

    m_features = DockRegistry::createAll(ctx, this);

    for (auto* feature : m_features) {
        addDockWidget(feature->dock(), feature->defaultLocation());
    }

    // Find feature instances for signal connections
    auto* logbookFeature = findFeature<LogbookDockFeature>();
    auto* plotFeature = findFeature<PlotDockFeature>();
    auto* videoFeature = findFeature<VideoDockFeature>();

    // Connect logbook signals
    if (logbookFeature) {
        connect(logbookFeature, &LogbookDockFeature::showSelectedRequested,
                this, &MainWindow::on_action_ShowSelected_triggered);
        connect(logbookFeature, &LogbookDockFeature::hideSelectedRequested,
                this, &MainWindow::on_action_HideSelected_triggered);
        connect(logbookFeature, &LogbookDockFeature::hideOthersRequested,
                this, &MainWindow::on_action_HideOthers_triggered);
        connect(logbookFeature, &LogbookDockFeature::deleteRequested,
                this, &MainWindow::on_action_Delete_triggered);
        connect(logbookFeature, &LogbookDockFeature::focusSessionRequested,
                this, [this](int row) {
            // Show only the double-clicked session, hide all others
            QMap<int, bool> visibility;
            for (int i = 0; i < model->rowCount(); ++i) {
                visibility.insert(i, i == row);
            }
            model->setRowsVisibility(visibility);

            // Zoom to extent for the now-visible session
            auto* pf = findFeature<PlotDockFeature>();
            if (pf && pf->plotWidget()) {
                pf->plotWidget()->zoomToExtent();
            }
        });
    }

    // Connect plot signals
    if (plotFeature) {
        auto* plotWidget = plotFeature->plotWidget();
        if (plotWidget) {
            connect(plotFeature, &PlotDockFeature::toolChanged, this, &MainWindow::onPlotWidgetToolChanged);

            // Cross-dock connections
            if (logbookFeature && logbookFeature->logbookView()) {
                connect(plotFeature, &PlotDockFeature::sessionsSelected,
                        logbookFeature->logbookView(), &LogbookView::selectSessions);
            }
        }
    }

    // Connect video widget drop signal
    if (videoFeature && videoFeature->videoWidget()) {
        connect(videoFeature->videoWidget(), &VideoWidget::urlsDropped,
                this, &MainWindow::handleDroppedUrls);
    }

    // Let PlotModel own the category/plot tree (must be done after plugin initialization)
    if (m_plotModel) {
        m_plotModel->setPlots(PlotRegistry::instance().dependentPlots());
    }

    // Restore the previous dock layout (includes visibility/open/closed state).
    restoreDockLayout();

    // Initialize the Window menu (dock visibility)
    initializeWindowMenu();

    // Initialize the Plots menu
    initializeXAxisMenu();
    initializePlotsMenu();

    // Initialize the Profiles menu
    initializeProfilesMenu();

    // On first launch, apply the designated default profile
    if (m_isFirstLaunch) {
        const QString defaultId = ProfileManager::defaultProfileId();
        if (!defaultId.isEmpty()) {
            auto profile = ProfileManager::instance().loadProfile(defaultId);
            if (profile.has_value())
                applyProfile(profile.value(), this);
        }
    }

    // Setup plot tools
    setupPlotTools();

    // Video step shortcuts (comma = back, period = forward, matching YouTube)
    auto *stepBackAction = new QAction(tr("Step Video Backward"), this);
    stepBackAction->setShortcut(QKeySequence(Qt::Key_Comma));
    connect(stepBackAction, &QAction::triggered, this, [this]() {
        auto *vf = findFeature<VideoDockFeature>();
        if (vf && vf->videoWidget())
            vf->videoWidget()->stepBackward();
    });
    addAction(stepBackAction);

    auto *stepForwardAction = new QAction(tr("Step Video Forward"), this);
    stepForwardAction->setShortcut(QKeySequence(Qt::Key_Period));
    connect(stepForwardAction, &QAction::triggered, this, [this]() {
        auto *vf = findFeature<VideoDockFeature>();
        if (vf && vf->videoWidget())
            vf->videoWidget()->stepForward();
    });
    addAction(stepForwardAction);

    // Accept file drops from the OS
    setAcceptDrops(true);
}

MainWindow::~MainWindow()
{
    // QObject deletes children in creation order, which would destroy the
    // session model (created first) under a queue that may still hold a worker.
    // So: the request component, then the queue (its destructor shuts it down
    // and joins the worker), then everything else - also when closeEvent() never ran.
    delete m_plotRequests;
    m_plotRequests = nullptr;
    delete m_jobQueue;
    m_jobQueue = nullptr;

    delete ui;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // First of all: cancel every background job and wait for the worker to
    // stop, before anything it could touch is saved or torn down. The wait
    // lasts at most one solver step. Nothing below can veto the close; a
    // future veto must be decided BEFORE this call, because a queue that has
    // been shut down refuses every later request.
    if (m_jobQueue) {
        const bool busy = !m_jobQueue->isIdle();
        if (busy)
            QApplication::setOverrideCursor(Qt::WaitCursor);
        m_jobQueue->shutdown();
        if (busy)
            QApplication::restoreOverrideCursor();
    }

    model->flushDirtySessions();
    saveDockLayout();
    KDDockWidgets::QtWidgets::MainWindow::closeEvent(event);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    handleDroppedUrls(event->mimeData()->urls());
    event->acceptProposedAction();
}

void MainWindow::handleDroppedUrls(const QList<QUrl> &urls)
{
    if (urls.isEmpty())
        return;

    const QSet<QString> videoExts = supportedVideoSuffixes();

    QStringList trackFiles;
    QString lastVideoFile;

    for (const QUrl &url : urls) {
        if (!url.isLocalFile())
            continue;

        const QString path = url.toLocalFile();
        const QString suffix = QFileInfo(path).suffix().toLower();

        if (suffix == QLatin1String("csv")) {
            trackFiles.append(path);
        } else if (videoExts.contains(suffix)) {
            lastVideoFile = path;
        }
    }

    // Import track files
    if (!trackFiles.isEmpty()) {
        importFiles(trackFiles, trackFiles.size() > 5);
    }

    // Load the last video file dropped
    if (!lastVideoFile.isEmpty()) {
        auto *videoFeature = findFeature<VideoDockFeature>();
        if (videoFeature && videoFeature->dock() && !videoFeature->dock()->isVisible()) {
            videoFeature->dock()->show();
        }
        if (videoFeature && videoFeature->videoWidget()) {
            videoFeature->videoWidget()->loadVideo(lastVideoFile);
        }
    }
}

void MainWindow::restoreDockLayout()
{
    if (!m_settings)
        return;

    const QByteArray layout = m_settings->value(QStringLiteral("ui/dockLayout")).toByteArray();
    if (layout.isEmpty())
        return;

    KDDockWidgets::LayoutSaver saver;
    if (!saver.restoreLayout(layout)) {
        qWarning() << "MainWindow::restoreDockLayout: Failed to restore dock layout, using defaults.";
        m_settings->remove(QStringLiteral("ui/dockLayout"));
    }
}

void MainWindow::saveDockLayout()
{
    if (!m_settings)
        return;

    KDDockWidgets::LayoutSaver saver;
    const QByteArray layout = saver.serializeLayout();
    if (layout.isEmpty())
        return;

    m_settings->setValue(QStringLiteral("ui/dockLayout"), layout);
}

PlotModel* MainWindow::plotModel() const
{
    return m_plotModel;
}

MarkerModel* MainWindow::markerModel() const
{
    return m_markerModel;
}

PlotViewSettingsModel* MainWindow::plotViewSettingsModel() const
{
    return m_plotViewSettingsModel;
}

QByteArray MainWindow::captureDockLayout() const
{
    KDDockWidgets::LayoutSaver saver;
    return saver.serializeLayout();
}

bool MainWindow::applyDockLayout(const QByteArray &layout)
{
    if (layout.isEmpty())
        return false;

    KDDockWidgets::LayoutSaver saver;
    return saver.restoreLayout(layout);
}

void MainWindow::on_action_Import_triggered()
{
    // Get files to import
    QStringList fileNames = QFileDialog::getOpenFileNames(
        this,
        tr("Import Tracks"),
        m_settings->value("folder").toString(),
        tr("CSV Files (*.csv *.CSV)")
        );

    if (fileNames.isEmpty()) {
        return;
    }

    // Determine the base directory (the initial directory in the dialog)
    QString baseDir = m_settings->value("folder").toString();

    // Call the helper function
    importFiles(fileNames, fileNames.size() > 5, baseDir);

    // Update last used folder
    QString lastUsedFolder = QFileInfo(fileNames.last()).absolutePath();
    m_settings->setValue("folder", lastUsedFolder);
}

void MainWindow::on_action_ImportFolder_triggered()
{
    // Open the file dialog
    QFileDialog dialog(this, tr("Select Folder to Import"));
    dialog.setFileMode(QFileDialog::Directory);
    dialog.setDirectory(m_settings->value("folder").toString());

    // Show dialog and get the selected directory
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    // The path returned
    QString selectedDir;
    const QList<QString> selectedFiles = dialog.selectedFiles();
    if (!selectedFiles.isEmpty()) {
        selectedDir = selectedFiles.first();
    }

    // Directory the user navigated to
    QString enteredDir = dialog.directory().absolutePath();

    // Store the appropriate directory based on user interaction
    if (selectedDir != enteredDir) {
        // User selected a specific folder
        m_settings->setValue("folder", QFileInfo(selectedDir).absolutePath());
    } else {
        // User just confirmed the navigated directory
        m_settings->setValue("folder", enteredDir);
    }

    // Define file filters (adjust according to your file types)
    QStringList nameFilters;
    nameFilters << "*.csv" << "*.CSV";

    // Use QDirIterator to iterate through the folder and its subdirectories
    QDirIterator it(selectedDir, nameFilters, QDir::Files, QDirIterator::Subdirectories);

    // Collect all files to import
    QStringList filesToImport;
    while (it.hasNext()) {
        it.next();
        filesToImport << it.filePath();
    }

    // If no files found, inform the user and exit
    if (filesToImport.isEmpty()) {
        QMessageBox::information(this, tr("No Files Found"), tr("No files matching the filter were found in the selected folder."));
        return;
    }

    // Call the helper function
    importFiles(filesToImport, filesToImport.size() > 5, selectedDir);
}

void MainWindow::on_action_ImportVideo_triggered()
{
    const QString defaultDir = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    const QString startDir = m_settings->value("videoFolder", defaultDir).toString();

    // Build filter from runtime-supported video formats
    QStringList globPatterns;
    for (const QString &ext : supportedVideoSuffixes())
        globPatterns.append(QStringLiteral("*.") + ext);
    globPatterns.sort(Qt::CaseInsensitive);

    QString videoFilter = tr("Video Files") + QStringLiteral(" (")
                          + globPatterns.join(QLatin1Char(' '))
                          + QStringLiteral(")");
    QString filter = videoFilter + QStringLiteral(";;") + tr("All Files (*)");

    const QString fileName = QFileDialog::getOpenFileName(
        this,
        tr("Import Video"),
        startDir,
        filter
        );

    if (fileName.isEmpty()) {
        return;
    }

    // Update last used folder
    m_settings->setValue("videoFolder", QFileInfo(fileName).absolutePath());

    // Ensure the Video dock is visible when a video is imported.
    auto* videoFeature = findFeature<VideoDockFeature>();
    if (videoFeature && videoFeature->dock() && !videoFeature->dock()->isVisible()) {
        videoFeature->dock()->show();
    }

    // Load/replace the video in the widget
    if (videoFeature && videoFeature->videoWidget()) {
        videoFeature->videoWidget()->loadVideo(fileName);
    }
}

void MainWindow::importFiles(
    const QStringList &fileNames,
    bool showProgress,
    const QString &baseDir
    )
{
    if (fileNames.isEmpty()) {
        return;
    }

    // Parse every file and hand the batch to the model, which decides per file
    // between creating a session and merging into an existing one.
    SessionImport::BatchResult result;
    if (showProgress) {
        QProgressDialog progressDialog(tr("Importing files..."), tr("Cancel"), 0, fileNames.size(), this);
        progressDialog.setWindowModality(Qt::WindowModal);
        progressDialog.setMinimumDuration(0);

        result = SessionImport::importFiles(*model, fileNames, [&progressDialog](int current, int /*total*/) {
            progressDialog.setValue(current);
            return !progressDialog.wasCanceled();
        });
        progressDialog.setValue(fileNames.size());
    } else {
        result = SessionImport::importFiles(*model, fileNames);
    }

    // Make imported sessions visible; optionally hide all others. A session
    // the file changed nothing in is shown too: the user asked for it.
    const QStringList importedIdList = result.importedSessionIds();
    if (!importedIdList.isEmpty()) {
        const QSet<QString> importedIds(importedIdList.begin(), importedIdList.end());

        bool hideOthers = PreferencesManager::instance()
                              .getValue(PreferenceKeys::ImportHideOthersOnImport).toBool();

        QMap<int, bool> visibilityMap;
        for (int row = 0; row < model->rowCount(); ++row) {
            const SessionRow &sr = model->rowAt(row);
            if (importedIds.contains(sr.sessionId)) {
                visibilityMap.insert(row, true);
            } else if (hideOthers) {
                visibilityMap.insert(row, false);
            }
        }
        model->setRowsVisibility(visibilityMap);

        // Zoom to the extent of the imported sessions: the merged session, not
        // the single file, is what should be framed.
        auto* pf = findFeature<PlotDockFeature>();
        if (pf && pf->plotWidget()) {
            // By id: the plot reads the model's live sessions (a copy would
            // have a cold cache) and skips ids that are not loaded.
            pf->plotWidget()->zoomToExtent(importedIdList);
        }
    }

    // Report failures, each with its reason
    const QString message = SessionImport::failureMessage(result.failures(), baseDir);
    if (!message.isEmpty()) {
        QMessageBox::warning(this, tr("Import Completed with Some Failures"), message);
    }
}

void MainWindow::on_action_Pan_triggered()
{
    auto* plotFeature = findFeature<PlotDockFeature>();
    if (plotFeature && plotFeature->plotWidget()) {
        plotFeature->plotWidget()->setCurrentTool(PlotWidget::Tool::Pan);
        qDebug() << "Switched to Pan tool";
    }
}

void MainWindow::on_action_Zoom_triggered()
{
    auto* plotFeature = findFeature<PlotDockFeature>();
    if (plotFeature && plotFeature->plotWidget()) {
        plotFeature->plotWidget()->setCurrentTool(PlotWidget::Tool::Zoom);
        qDebug() << "Switched to Zoom tool";
    }
}

void MainWindow::on_action_Measure_triggered()
{
    auto* plotFeature = findFeature<PlotDockFeature>();
    if (plotFeature && plotFeature->plotWidget()) {
        plotFeature->plotWidget()->setCurrentTool(PlotWidget::Tool::Measure);
        qDebug() << "Switched to Measure tool";
    }
}

void MainWindow::on_action_Select_triggered()
{
    auto* plotFeature = findFeature<PlotDockFeature>();
    if (plotFeature && plotFeature->plotWidget()) {
        plotFeature->plotWidget()->setCurrentTool(PlotWidget::Tool::Select);
        qDebug() << "Switched to Select tool";
    }
}

void MainWindow::on_action_SetExit_triggered()
{
    auto* plotFeature = findFeature<PlotDockFeature>();
    if (plotFeature && plotFeature->plotWidget()) {
        plotFeature->plotWidget()->setCurrentTool(PlotWidget::Tool::SetExit);
        qDebug() << "Switched to Set Exit tool";
    }
}

void MainWindow::on_action_SetSync_triggered()
{
    auto* plotFeature = findFeature<PlotDockFeature>();
    if (plotFeature && plotFeature->plotWidget()) {
        plotFeature->plotWidget()->setCurrentTool(PlotWidget::Tool::SetSync);
    }
}

void MainWindow::on_action_SetGround_triggered()
{
    auto* plotFeature = findFeature<PlotDockFeature>();
    if (plotFeature && plotFeature->plotWidget()) {
        plotFeature->plotWidget()->setCurrentTool(PlotWidget::Tool::SetGround);
        qDebug() << "Switched to Set Ground tool";
    }
}

void MainWindow::on_action_SetCourse_triggered()
{
    auto* plotFeature = findFeature<PlotDockFeature>();
    if (plotFeature && plotFeature->plotWidget()) {
        plotFeature->plotWidget()->setCurrentTool(PlotWidget::Tool::SetCourse);
    }
}

void MainWindow::on_action_ShowSelected_triggered()
{
    setSelectedTrackCheckState(Qt::Checked);
}

void MainWindow::on_action_HideSelected_triggered()
{
    setSelectedTrackCheckState(Qt::Unchecked);
}

void MainWindow::on_action_HideOthers_triggered()
{
    auto* logbookFeature = findFeature<LogbookDockFeature>();
    if (!logbookFeature || !logbookFeature->logbookView())
        return;

    QList<QModelIndex> selectedRows = logbookFeature->logbookView()->selectedRows();
    if (selectedRows.isEmpty())
        return;

    // Hide all rows, then remove selected rows from the map
    QMap<int, bool> visibility;
    int totalRows = model->rowCount();
    for (int i = 0; i < totalRows; ++i) {
        visibility.insert(i, false);
    }
    for (const QModelIndex &idx : selectedRows) {
        visibility.remove(idx.row());
    }
    model->setRowsVisibility(visibility);
}

void MainWindow::on_action_Delete_triggered()
{
    auto* logbookFeature = findFeature<LogbookDockFeature>();
    if (!logbookFeature || !logbookFeature->logbookView())
        return;

    // Get selected rows from the logbook view
    QList<QModelIndex> selectedRows = logbookFeature->logbookView()->selectedRows();

    if (selectedRows.isEmpty()) {
        QMessageBox::information(this, tr("Delete Tracks"), tr("No tracks selected for deletion."));
        return;
    }

    // Collect SESSION_IDs of the selected sessions
    QList<QString> sessionIdsToRemove;
    for (const QModelIndex &index : selectedRows) {
        if (!index.isValid())
            continue;

        // Assuming the SESSION_ID is stored as an attribute in SessionData
        // You might need to adjust this based on your actual implementation
        const SessionRow &sr = model->rowAt(index.row());
        QString sessionId = sr.sessionId;
        sessionIdsToRemove.append(sessionId);
    }

    // Confirm deletion with the user
    QMessageBox::StandardButton reply;
    reply = QMessageBox::question(
        this,
        tr("Delete Tracks"),
        tr("Are you sure you want to delete the selected %1 track(s)?").arg(sessionIdsToRemove.size()),
        QMessageBox::Yes | QMessageBox::No
        );

    if (reply != QMessageBox::Yes) {
        return; // User canceled the deletion
    }

    // Proceed to remove the sessions from the model
    bool success = model->removeSessions(sessionIdsToRemove);

    if (success) {
        // Remove corresponding logbook files from disk
        for (const QString &sessionId : sessionIdsToRemove) {
            LogbookManager::instance().removeSession(sessionId);
        }
        LogbookManager::instance().flushIndex();
    } else {
        QMessageBox::warning(
            this,
            tr("Delete Tracks"),
            tr("Failed to delete the selected track(s).")
            );
    }
}

void MainWindow::on_action_Preferences_triggered()
{
    PreferencesDialog dlg(this);
    if (dlg.exec() == QDialog::Accepted) {
        // User clicked OK
    }
}

void MainWindow::on_action_Exit_triggered()
{
    close();  // Close the main window
}

void MainWindow::on_action_About_triggered()
{
    QMessageBox::about(this, tr("About FlySight Viewer"),
        tr("<h3>FlySight Viewer %1</h3>"
           "<p>Data analysis and visualization for FlySight</p>"
           "<p><a href=\"https://flysight.ca\">flysight.ca</a></p>")
        .arg(QStringLiteral(FLYSIGHT_VERSION_STRING)));
}

void MainWindow::onPlotWidgetToolChanged(PlotWidget::Tool t)
{
    switch (t) {
    case PlotWidget::Tool::Pan:
        ui->action_Pan->setChecked(true);
        break;
    case PlotWidget::Tool::Zoom:
        ui->action_Zoom->setChecked(true);
        break;
    case PlotWidget::Tool::Measure:
        ui->action_Measure->setChecked(true);
        break;
    case PlotWidget::Tool::Select:
        ui->action_Select->setChecked(true);
        break;
    case PlotWidget::Tool::SetExit:
        ui->action_SetExit->setChecked(true);
        break;
    case PlotWidget::Tool::SetSync:
        ui->action_SetSync->setChecked(true);
        break;
    case PlotWidget::Tool::SetGround:
        ui->action_SetGround->setChecked(true);
        break;
    case PlotWidget::Tool::SetCourse:
        ui->action_SetCourse->setChecked(true);
        break;
    }
}

void MainWindow::registerBuiltInMarkers()
{
    QVector<MarkerDefinition> defaults = {
        // Category: Reference                                                                                                               groupId  defaultEnabled
        {"Reference", "Exit",                    "Exit",   QColor(34, 131, 196),  SessionKeys::ExitTime,           {}, true,  {}, true},
        {"Reference", "Video sync",              "Sync",   QColor(0, 175, 175),   SessionKeys::SyncTime,           {}, true,  {}, true},
        {"Reference", "Manoeuvre start",         "Mvr",     QColor(213, 136, 43),  SessionKeys::ManoeuvreStartTime, {}, true,  {}, true},
        {"Reference", "Landing",                 "Land",   QColor(153, 102, 51),  SessionKeys::LandingTime,        {}, true,  {}, true},
        {"Reference", "Analysis start",          "AS",     QColor(179, 41, 179),  SessionKeys::AnalysisStartTime,  {}, true,  {}, true},
        {"Reference", "Analysis end",            "AE",     QColor(179, 41, 179),  SessionKeys::AnalysisEndTime,    {}, true,  {}, true},
        {"Reference", "Course",                  "Crs",    QColor(0, 255, 255),   SessionKeys::CourseRef,          {}, true,  {}, true},

        {"Reference", "Flare start",            "FlrS",   QColor(76, 153, 76),   SessionKeys::FlareStartTime,     {}, false, {}, true},
        {"Reference", "Flare end",              "FlrE",   QColor(76, 153, 76),   SessionKeys::FlareEndTime,       {}, false, {}, true},

        // Category: Analysis  (colours match the corresponding GNSS plots)
        {"Analysis",  "Maximum vertical speed",   "MaxV", QColor::fromHsl(120, 170, 115), SessionKeys::MaxVelDTime, {{"GNSS", "_time", "velD"}}, false},
        {"Analysis",  "Maximum horizontal speed", "MaxH", QColor::fromHsl(  0, 170, 128), SessionKeys::MaxVelHTime, {{"GNSS", "_time", "velH"}}, false},
    };

    for (auto &md : defaults)
        MarkerRegistry::instance()->registerMarker(md);
}

void MainWindow::registerBuiltInPlots()
{
    // Angular spread for grouped colours
    const int group_a = 40;

    // ── Muted palette: comfortable on both light and dark backgrounds ──
    const int S    = 170;      // Standard saturation  (was 255)
    const int S_dk = 150;      // Deep / accuracy-plot saturation

    // Per-hue-family lightness (equalises perceived brightness)
    const int L_w  = 128;      // Warm hues:  red, orange, pink     (H ~ 320-50)
    const int L_c  = 115;      // Cool hues:  green, teal           (H ~ 60-180)
    const int L_b  = 145;      // Blue hues:  blue, violet, magenta (H ~ 200-300)

    // Deep variant lightness (was ~64 — invisible on dark backgrounds)
    const int L_dw = 100;      // Deep warm
    const int L_dc =  90;      // Deep cool
    const int L_db = 130;      // Deep blue

    // Neutral grays
    const int L_g  = 120;      // Primary gray  (was 64)
    const int L_gl = 145;      // Lighter gray  (was 128)

    QVector<PlotValue> defaults = {
        // Category: GNSS (Basic)  (hues preserved from original Qt named colours)
        {"GNSS (Basic)", "Elevation",             "m",     QColor::fromHsl(  0, 0,    135),  "GNSS", "z",             "altitude"},
        {"GNSS (Basic)", "Horizontal speed",      "m/s",   QColor::fromHsl(  0, S,    L_w),  "GNSS", "velH",          "speed"},
        {"GNSS (Basic)", "Vertical speed",        "m/s",   QColor::fromHsl(120, S,    L_c),  "GNSS", "velD",          "vertical_speed"},
        {"GNSS (Basic)", "Total speed",           "m/s",   QColor::fromHsl(240, S,    L_b),  "GNSS", "vel",           "speed"},
        {"GNSS (Basic)", "Course",                "deg",   Qt::cyan,                          "GNSS", "course",        "angle"},
        {"GNSS (Basic)", "Course rate",           "deg/s", Qt::darkCyan,                      "GNSS", "courseRate",     "rotation"},
        {"GNSS (Basic)", "Glide ratio",           "",      Qt::darkCyan,                      "GNSS", "glideRatio",    "ratio"},
        {"GNSS (Basic)", "Dive angle",            "deg",   Qt::magenta,                       "GNSS", "diveAngle",     "angle"},
        {"GNSS (Basic)", "Dive angle rate",       "deg/s", Qt::darkYellow,                    "GNSS", "diveAngleRate", "rotation"},
        {"GNSS (Basic)", "Horizontal accuracy",   "m",     QColor::fromHsl(  0, S_dk, L_dw), "GNSS", "hAcc",          "distance"},
        {"GNSS (Basic)", "Vertical accuracy",     "m",     QColor::fromHsl(120, S_dk, L_dc), "GNSS", "vAcc",          "distance"},
        {"GNSS (Basic)", "Speed accuracy",        "m/s",   QColor::fromHsl(240, S_dk, L_db), "GNSS", "sAcc",          "speed"},
        {"GNSS (Basic)", "Number of satellites",  "",      QColor::fromHsl(300, S_dk, L_db), "GNSS", "numSV",         "count"},

        // Category: GNSS (Advanced)
        {"GNSS (Advanced)", "Horizontal acceleration",         "m/s^2", QColor::fromHsl( 30, S,    L_w),  "GNSS", "accH",              "acceleration"},
        {"GNSS (Advanced)", "Vertical acceleration",           "m/s^2", QColor::fromHsl(120, S,    L_c),  "GNSS", "accD",              "acceleration"},
        {"GNSS (Advanced)", "Wind-corrected horizontal speed", "m/s",   QColor::fromHsl(200, S,    L_b),  "GNSS", "wcVelH",            "speed"},
        {"GNSS (Advanced)", "Along-track acceleration",        "m/s^2", QColor::fromHsl( 60, S,    L_c),  "GNSS", "accAlongTrack",     "acceleration"},
        {"GNSS (Advanced)", "Cross-track acceleration",        "m/s^2", QColor::fromHsl(270, S,    L_b),  "GNSS", "accCrossTrack",     "acceleration"},
        {"GNSS (Advanced)", "Lift coefficient",                "",      Qt::darkGreen,                     "GNSS", "lift",              "coefficient"},
        {"GNSS (Advanced)", "Drag coefficient",                "",      Qt::darkBlue,                      "GNSS", "drag",              "coefficient"},
        {"GNSS (Advanced)", "Specific energy",                 "kJ/kg", Qt::darkGreen,                     "GNSS", "specificEnergy",    "specific_energy"},
        {"GNSS (Advanced)", "Specific energy rate",            "W/kg",  Qt::darkBlue,                      "GNSS", "specificEnergyRate","specific_power"},

        // Category: GNSS (Local frame) - recording-wide north/east/down frame
        // (red/green/blue per axis; positions deep, velocities standard)
        {"GNSS (Local frame)", "North position", "m",   QColor::fromHsl(  0, S_dk, L_dw), "Local", "north", "distance"},
        {"GNSS (Local frame)", "East position",  "m",   QColor::fromHsl(120, S_dk, L_dc), "Local", "east",  "distance"},
        {"GNSS (Local frame)", "Down position",  "m",   QColor::fromHsl(240, S_dk, L_db), "Local", "down",  "distance"},
        {"GNSS (Local frame)", "North velocity", "m/s", QColor::fromHsl(  0, S,    L_w),  "Local", "velN",  "speed"},
        {"GNSS (Local frame)", "East velocity",  "m/s", QColor::fromHsl(120, S,    L_c),  "Local", "velE",  "speed"},
        {"GNSS (Local frame)", "Down velocity",  "m/s", QColor::fromHsl(240, S,    L_b),  "Local", "velD",  "vertical_speed"},

        // Category: IMU · Acceleration (red group, H ≈ 0°)
        {"IMU", "Acceleration X",     "g", QColor::fromHsl(360 - group_a, S, L_w), "IMU", "ax",     "acceleration"},
        {"IMU", "Acceleration Y",     "g", QColor::fromHsl(  0,           S, L_w), "IMU", "ay",     "acceleration"},
        {"IMU", "Acceleration Z",     "g", QColor::fromHsl(group_a,       S, L_w), "IMU", "az",     "acceleration"},
        {"IMU", "Total acceleration", "g", QColor::fromHsl(  0,           S, L_w), "IMU", "aTotal", "acceleration"},

        // Category: IMU · Rotation (green group, H ≈ 120°)
        {"IMU", "Rotation X",     "deg/s", QColor::fromHsl(120 - group_a, S, L_c), "IMU", "wx",     "rotation"},
        {"IMU", "Rotation Y",     "deg/s", QColor::fromHsl(120,           S, L_c), "IMU", "wy",     "rotation"},
        {"IMU", "Rotation Z",     "deg/s", QColor::fromHsl(120 + group_a, S, L_c), "IMU", "wz",     "rotation"},
        {"IMU", "Total rotation", "deg/s", QColor::fromHsl(120,           S, L_c), "IMU", "wTotal", "rotation"},

        {"IMU", "Temperature", QString::fromUtf8("\302\260C"), QColor::fromHsl(45, S, L_w), "IMU", "temperature", "temperature"},

        // Category: Sensor fusion (explicit: computed on request from the plot list; same fixed NED frame as "GNSS (Local frame)")
        {"Sensor fusion", "North position",          "m",     QColor::fromHsl(  0, S_dk, L_dw), "Fusion", "north", "distance"},
        {"Sensor fusion", "East position",           "m",     QColor::fromHsl(120, S_dk, L_dc), "Fusion", "east",  "distance"},
        {"Sensor fusion", "Down position",           "m",     QColor::fromHsl(240, S_dk, L_db), "Fusion", "down",  "distance"},
        {"Sensor fusion", "North velocity",          "m/s",   QColor::fromHsl(  0, S,    L_w),  "Fusion", "velN",  "speed"},
        {"Sensor fusion", "East velocity",           "m/s",   QColor::fromHsl(120, S,    L_c),  "Fusion", "velE",  "speed"},
        {"Sensor fusion", "Down velocity",           "m/s",   QColor::fromHsl(240, S,    L_b),  "Fusion", "velD",  "vertical_speed"},
        {"Sensor fusion", "North acceleration",      "m/s^2", QColor::fromHsl(320, S,    L_w),  "Fusion", "accN",  "acceleration"},
        {"Sensor fusion", "East acceleration",       "m/s^2", QColor::fromHsl(  0, S,    L_w),  "Fusion", "accE",  "acceleration"},
        {"Sensor fusion", "Down acceleration",       "m/s^2", QColor::fromHsl( 40, S,    L_w),  "Fusion", "accD",  "acceleration"},
        {"Sensor fusion", "Horizontal acceleration", "m/s^2", QColor::fromHsl( 20, S,    L_w),  "Fusion", "accH",  "acceleration"},
        {"Sensor fusion", "Roll",                    "deg",   QColor::fromHsl(  0, S_dk, L_dw), "Fusion", "roll",  "angle"},
        {"Sensor fusion", "Pitch",                   "deg",   QColor::fromHsl(120, S_dk, L_dc), "Fusion", "pitch", "angle"},
        {"Sensor fusion", "Yaw",                     "deg",   QColor::fromHsl(240, S_dk, L_db), "Fusion", "yaw",   "angle"},
        {"Sensor fusion", "Quaternion X",            "",      QColor::fromHsl(  0, S,    L_w),  "Fusion", "qx",    "ratio"},
        {"Sensor fusion", "Quaternion Y",            "",      QColor::fromHsl(120, S,    L_c),  "Fusion", "qy",    "ratio"},
        {"Sensor fusion", "Quaternion Z",            "",      QColor::fromHsl(240, S,    L_b),  "Fusion", "qz",    "ratio"},
        {"Sensor fusion", "Quaternion W",            "",      QColor::fromHsl(  0, 0,    L_g),  "Fusion", "qw",    "ratio"},

        // Category: Magnetometer (blue group, H ≈ 240°)
        {"Magnetometer", "Magnetic field X",     "gauss", QColor::fromHsl(240 - group_a, S, L_b), "MAG", "x",     "magnetic_field"},
        {"Magnetometer", "Magnetic field Y",     "gauss", QColor::fromHsl(240,           S, L_b), "MAG", "y",     "magnetic_field"},
        {"Magnetometer", "Magnetic field Z",     "gauss", QColor::fromHsl(240 + group_a, S, L_b), "MAG", "z",     "magnetic_field"},
        {"Magnetometer", "Total magnetic field", "gauss", QColor::fromHsl(240,           S, L_b), "MAG", "total", "magnetic_field"},

        {"Magnetometer", "Temperature", QString::fromUtf8("\302\260C"), QColor::fromHsl(135, S, L_c), "MAG", "temperature", "temperature"},

        // Category: Barometer
        {"Barometer", "Air pressure", "Pa",                              QColor::fromHsl(  0, 0, L_g), "BARO", "pressure",    "pressure"},
        {"Barometer", "Temperature",  QString::fromUtf8("\302\260C"),    QColor::fromHsl(225, S, L_b), "BARO", "temperature", "temperature"},

        // Category: Humidity
        {"Humidity", "Humidity",    "%",                                 QColor::fromHsl(  0, 0,  L_gl), "HUM", "humidity",    "percentage"},
        {"Humidity", "Temperature", QString::fromUtf8("\302\260C"),      QColor::fromHsl(315, S,  L_b),  "HUM", "temperature", "temperature"},

        // Category: Battery
        {"Battery", "Battery voltage", "V", QColor::fromHsl(30, S, L_w), "VBAT", "voltage", "voltage"},

        // Category: GNSS time
        {"GNSS time", "Time of week", "s", QColor::fromHsl(0, 0, L_g),  "TIME", "tow",  "time"},
        {"GNSS time", "Week number",  "",  QColor::fromHsl(0, 0, L_gl), "TIME", "week", "count"},

        // Independent variables (x-axis)
        {"Time", "UTC time",    "s", QColor(128, 128, 128), "GNSS", "_time",        "time", PlotRole::Independent},
        {"Time", "System time", "s", QColor(128, 128, 128), "GNSS", "_system_time", "time", PlotRole::Independent},

    };

    for (auto &pv : defaults)
        PlotRegistry::instance().registerPlot(pv);
}


void MainWindow::initializePreferences()
{
    PreferencesManager &prefs = PreferencesManager::instance();

    // ========================================================================
    // General Preferences
    // ========================================================================
    prefs.registerPreference(PreferenceKeys::GeneralUnits, QStringLiteral("Metric"));
    prefs.registerPreference(PreferenceKeys::GeneralLogbookFolder,
                             QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));

    // ========================================================================
    // Import Preferences
    // ========================================================================
    prefs.registerPreference(PreferenceKeys::ImportGroundReferenceMode, QStringLiteral("Automatic"));
    prefs.registerPreference(PreferenceKeys::ImportFixedElevation, 0.0);
    prefs.registerPreference(PreferenceKeys::ImportDescentPauseSeconds, 30.0);
    prefs.registerPreference(PreferenceKeys::ImportHideOthersOnImport, false);

    // ========================================================================
    // Global Plot Preferences
    // ========================================================================
    prefs.registerPreference(PreferenceKeys::PlotsLineThickness, 1.0);
    prefs.registerPreference(PreferenceKeys::PlotsTextSize, 9);
    prefs.registerPreference(PreferenceKeys::PlotsCrosshairColor, QVariant::fromValue(QColor(Qt::gray)));
    prefs.registerPreference(PreferenceKeys::PlotsCrosshairThickness, 1.0);
    prefs.registerPreference(PreferenceKeys::PlotsYAxisPadding, 0.05);

    // ========================================================================
    // Per-Plot Preferences (dynamically registered from PlotRegistry)
    // ========================================================================
    const QVector<PlotValue> allPlots = PlotRegistry::instance().allPlots();
    for (const PlotValue &pv : allPlots) {
        // Color preference (default from PlotValue.defaultColor)
        prefs.registerPreference(
            PreferenceKeys::plotColorKey(pv.sensorID, pv.measurementID),
            QVariant::fromValue(pv.defaultColor)
        );

        // Y-axis mode preference (auto, fixed)
        prefs.registerPreference(
            PreferenceKeys::plotYAxisModeKey(pv.sensorID, pv.measurementID),
            QStringLiteral("auto")
        );

        // Y-axis min/max values (used when mode is "fixed")
        prefs.registerPreference(
            PreferenceKeys::plotYAxisMinKey(pv.sensorID, pv.measurementID),
            0.0
        );
        prefs.registerPreference(
            PreferenceKeys::plotYAxisMaxKey(pv.sensorID, pv.measurementID),
            100.0
        );
    }

    // ========================================================================
    // Per-Marker Preferences (dynamically registered from MarkerRegistry)
    // ========================================================================
    const QVector<MarkerDefinition> allMarkers = MarkerRegistry::instance()->allMarkers();
    for (const MarkerDefinition &md : allMarkers) {
        // Color preference (default from MarkerDefinition.color)
        prefs.registerPreference(
            PreferenceKeys::markerColorKey(md.attributeKey),
            QVariant::fromValue(md.color)
        );
    }

    // ========================================================================
    // Legend Preferences
    // ========================================================================
    prefs.registerPreference(PreferenceKeys::LegendTextSize, 9);

    // ========================================================================
    // Map Preferences
    // ========================================================================
    prefs.registerPreference(PreferenceKeys::MapLineThickness, 3.0);
    prefs.registerPreference(PreferenceKeys::MapLargeDotSize, 10);
    prefs.registerPreference(PreferenceKeys::MapSmallDotSize, 6);
    prefs.registerPreference(PreferenceKeys::MapTrackOpacity, 0.85);

    // ========================================================================
    // Zoom Preferences
    // ========================================================================
    prefs.registerPreference(PreferenceKeys::ZoomExtentMode, QStringLiteral("markerRange"));
    prefs.registerPreference(PreferenceKeys::ZoomExtentStartMarker, QString::fromLatin1(SessionKeys::AnalysisStartTime));
    prefs.registerPreference(PreferenceKeys::ZoomExtentEndMarker,   QString::fromLatin1(SessionKeys::AnalysisEndTime));
    prefs.registerPreference(PreferenceKeys::ZoomExtentMarginPct, 10.0);

    // ========================================================================
    // Analysis Preferences
    // ========================================================================
    prefs.registerPreference(PreferenceKeys::AnalysisMethod, QStringLiteral("Wingsuit Performance"));

    // ========================================================================
    // Aerodynamics Preferences
    // ========================================================================
    prefs.registerPreference(PreferenceKeys::AeroMass, 1.0);
    prefs.registerPreference(PreferenceKeys::AeroArea, 1.0);
}



void MainWindow::initializeXAxisMenu()
{
    QMenu *plotsMenu = ui->menuPlots;
    Q_ASSERT(plotsMenu);

    QMenu *xAxisMenu = plotsMenu->addMenu(tr("Independent Variable"));
    plotsMenu->addSeparator();

    QActionGroup *axisGroup = new QActionGroup(this);
    axisGroup->setExclusive(true);

    // Read the persisted x-variable so the checked action matches QSettings
    const QString currentXVar = m_plotViewSettingsModel
        ? m_plotViewSettingsModel->xVariable()
        : SessionKeys::Time;

    // Populate from registered independent variables
    const QVector<PlotValue> indepPlots = PlotRegistry::instance().independentPlots();
    for (const PlotValue &ipv : indepPlots) {
        QAction *action = xAxisMenu->addAction(ipv.plotName);
        action->setCheckable(true);
        action->setChecked(currentXVar == ipv.measurementID);
        axisGroup->addAction(action);

        const QString measID = ipv.measurementID;
        connect(action, &QAction::triggered, this, [this, measID]() {
            if (m_plotViewSettingsModel)
                m_plotViewSettingsModel->setXVariable(measID);
        });
    }
}

void MainWindow::initializePlotsMenu()
{
    // Access the 'Plots' menu from the UI
    QMenu *plotsMenu = ui->menuPlots;

    // Define the list of plots to include in the 'Plots' menu, including separators
    QVector<PlotMenuItem> plotsMenuItems = {
        PlotMenuItem("Elevation", QKeySequence(Qt::Key_E), "GNSS", "z"),

        PlotMenuItem(PlotMenuItemType::Separator),

        PlotMenuItem("Horizontal Speed", QKeySequence(Qt::Key_H), "GNSS", "velH"),
        PlotMenuItem("Vertical Speed", QKeySequence(Qt::Key_V), "GNSS", "velD"),
        PlotMenuItem("Total Speed", QKeySequence(Qt::Key_S), "GNSS", "vel"),

        PlotMenuItem(PlotMenuItemType::Separator),

        PlotMenuItem("Course", QKeySequence(Qt::Key_C), "GNSS", "course"),
        PlotMenuItem("Course Rate", QKeySequence(Qt::SHIFT | Qt::Key_C), "GNSS", "courseRate"),

        PlotMenuItem(PlotMenuItemType::Separator),

        PlotMenuItem("Glide Ratio", QKeySequence(Qt::Key_G), "GNSS", "glideRatio"),
        PlotMenuItem("Dive Angle", QKeySequence(Qt::Key_A), "GNSS", "diveAngle"),
        PlotMenuItem("Dive Angle Rate", QKeySequence(Qt::SHIFT | Qt::Key_A), "GNSS", "diveAngleRate"),

        PlotMenuItem(PlotMenuItemType::Separator),

        PlotMenuItem("Horizontal Accuracy", QKeySequence(Qt::SHIFT | Qt::Key_H), "GNSS", "hAcc"),
        PlotMenuItem("Vertical Accuracy", QKeySequence(Qt::SHIFT | Qt::Key_V), "GNSS", "vAcc"),
        PlotMenuItem("Speed Accuracy", QKeySequence(Qt::SHIFT | Qt::Key_S), "GNSS", "sAcc"),

        PlotMenuItem(PlotMenuItemType::Separator),

        PlotMenuItem("Number of Satellites", QKeySequence(Qt::SHIFT | Qt::Key_N), "GNSS", "numSV"),

        PlotMenuItem(PlotMenuItemType::Separator),

        PlotMenuItem("Lift Coefficient", QKeySequence(Qt::Key_L), "GNSS", "lift"),
        PlotMenuItem("Drag Coefficient", QKeySequence(Qt::Key_D), "GNSS", "drag"),
    };

    // Iterate over the list and create corresponding actions
    for(const PlotMenuItem &item : plotsMenuItems){
        if(item.type == PlotMenuItemType::Separator){
            plotsMenu->addSeparator();
            continue; // Move to the next item
        }

        QAction *action = new QAction(item.menuText, this);
        action->setShortcut(item.shortcut);

        // Combine sensorID and measurementID into a single string for data storage
        QString actionData = item.sensorID + "|" + item.measurementID;
        action->setData(actionData);

        // Connect each action to a lambda that calls togglePlot with appropriate parameters
        connect(action, &QAction::triggered, this, [this, action]() {
            QString data = action->data().toString();
            QStringList parts = data.split('|');
            if(parts.size() == 2){
                QString sensorID = parts.at(0);
                QString measurementID = parts.at(1);
                togglePlot(sensorID, measurementID);
            }
        });

        // Add the action to the 'Plots' menu
        plotsMenu->addAction(action);
    }

    // Add Zoom to Extent action at the top of Tools menu
    QAction *zoomToExtentAction = new QAction(tr("Zoom to Extent"), this);
    zoomToExtentAction->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_Z));
    connect(zoomToExtentAction, &QAction::triggered, this, [this]() {
        auto* plotFeature = findFeature<PlotDockFeature>();
        if (plotFeature && plotFeature->plotWidget()) {
            plotFeature->plotWidget()->zoomToExtent();
        }
    });
    ui->menu_Tools->insertAction(ui->action_Pan, zoomToExtentAction);
    ui->menu_Tools->insertSeparator(ui->action_Pan);

    // Add separator and units toggle at the end
    ui->menu_Tools->addSeparator();
    QAction *toggleUnitsAction = new QAction(tr("Toggle Units"), this);
    toggleUnitsAction->setShortcut(QKeySequence(Qt::Key_U));
    connect(toggleUnitsAction, &QAction::triggered,
            this, &MainWindow::on_action_ToggleUnits_triggered);
    ui->menu_Tools->addAction(toggleUnitsAction);
}

void MainWindow::initializeWindowMenu()
{
    QMenu *windowMenu = ui->menuWindow;
    Q_ASSERT(windowMenu);

    for (auto* feature : m_features) {
        QAction* a = feature->toggleAction();
        if (a) {
            a->setText(feature->title());
            windowMenu->addAction(a);
        }
    }
}

void MainWindow::initializeProfilesMenu()
{
    rebuildProfilesMenu();

    connect(&ProfileManager::instance(), &ProfileManager::profilesChanged,
            this, &MainWindow::rebuildProfilesMenu);
}

void MainWindow::rebuildProfilesMenu()
{
    QMenu *menu = ui->menuProfiles;
    menu->clear();

    // Get all profiles and the user-defined order
    const QVector<Profile> allProfiles = ProfileManager::instance().listProfiles();
    const QStringList orderedIds = ProfileManager::instance().profileOrder();

    // Build a map from id to profile for quick lookup
    QHash<QString, Profile> profileMap;
    for (const Profile &p : allProfiles) {
        profileMap.insert(p.id, p);
    }

    // Collect profiles in order: first those in orderedIds, then remaining alphabetically
    QVector<Profile> ordered;
    QSet<QString> added;

    for (const QString &id : orderedIds) {
        if (profileMap.contains(id)) {
            ordered.append(profileMap.value(id));
            added.insert(id);
        }
    }

    // Remaining profiles sorted alphabetically by displayName
    QVector<Profile> remaining;
    for (const Profile &p : allProfiles) {
        if (!added.contains(p.id)) {
            remaining.append(p);
        }
    }
    std::sort(remaining.begin(), remaining.end(),
              [](const Profile &a, const Profile &b) {
                  return a.displayName.toLower() < b.displayName.toLower();
              });
    ordered.append(remaining);

    // Add a menu action for each profile
    for (const Profile &profile : ordered) {
        QString profileId = profile.id;
        QAction *action = new QAction(profile.displayName, menu);
        action->setData(profileId);

        connect(action, &QAction::triggered, this, [this, profileId]() {
            auto result = QMessageBox::question(
                this,
                tr("Load Profile"),
                tr("Your current analysis state will be replaced. Continue?"),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No);

            if (result == QMessageBox::Yes) {
                auto profile = ProfileManager::instance().loadProfile(profileId);
                if (profile.has_value()) {
                    applyProfile(profile.value(), this);
                }
            }
        });

        menu->addAction(action);
    }

    // Add separator only if there are profiles
    if (!ordered.isEmpty()) {
        menu->addSeparator();
    }

    menu->addAction(ui->action_AddProfile);
    menu->addAction(ui->action_ManageProfiles);
}

void MainWindow::on_action_AddProfile_triggered()
{
    bool ok = false;
    QString name = QInputDialog::getText(
        this,
        tr("Add Profile"),
        tr("Profile name:"),
        QLineEdit::Normal,
        QString(),
        &ok);

    if (!ok) return;

    name = name.trimmed();
    if (name.isEmpty()) return;

    Profile profile = captureCurrentState(this);
    profile.displayName = name;
    ProfileManager::instance().saveProfile(profile);
}

void MainWindow::on_action_ManageProfiles_triggered()
{
    ManageProfilesDialog dlg(this);
    dlg.exec();
}

void MainWindow::togglePlot(const QString &sensorID, const QString &measurementID)
{
    if (!m_plotModel) {
        return;
    }

    m_plotModel->togglePlot(sensorID, measurementID);

    // Ensure the plot selection view is visible
    auto* plotSelectionFeature = findFeature<PlotSelectionDockFeature>();
    if (plotSelectionFeature && plotSelectionFeature->dock()) {
        if (!plotSelectionFeature->dock()->isVisible()) {
            plotSelectionFeature->dock()->show();
        }
    }
}

void MainWindow::setSelectedTrackCheckState(Qt::CheckState state)
{
    auto* logbookFeature = findFeature<LogbookDockFeature>();
    if (!logbookFeature || !logbookFeature->logbookView())
        return;

    // Get all selected rows from the logbook view
    QList<QModelIndex> selectedRows = logbookFeature->logbookView()->selectedRows();
    if (selectedRows.isEmpty())
        return;

    QMap<int, bool> visibility;
    for (const QModelIndex &idx : selectedRows) {
        visibility.insert(idx.row(), state == Qt::Checked);
    }
    model->setRowsVisibility(visibility);
}

void MainWindow::setupPlotTools()
{
    // Set up the tool action group
    toolActionGroup = new QActionGroup(this);
    toolActionGroup->setExclusive(true);

    // Add tool actions to the group
    toolActionGroup->addAction(ui->action_Pan);
    toolActionGroup->addAction(ui->action_Zoom);
    toolActionGroup->addAction(ui->action_Measure);
    toolActionGroup->addAction(ui->action_Select);
    toolActionGroup->addAction(ui->action_SetExit);
    toolActionGroup->addAction(ui->action_SetSync);
    toolActionGroup->addAction(ui->action_SetGround);
    toolActionGroup->addAction(ui->action_SetCourse);

    // Set Pan as the default checked tool
    ui->action_Pan->setChecked(true);
}


void MainWindow::on_action_ToggleUnits_triggered()
{
    UnitConverter &converter = UnitConverter::instance();
    QStringList systems = converter.availableSystems();

    if (systems.isEmpty())
        return;

    // Find current system index
    QString current = converter.currentSystem();
    int currentIndex = systems.indexOf(current);

    // Cycle to next system (wrap around)
    int nextIndex = (currentIndex + 1) % systems.size();
    QString nextSystem = systems.at(nextIndex);

    // Apply the change
    converter.setSystem(nextSystem);
}

} // namespace FlySight
