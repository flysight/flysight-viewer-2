// The main window's status bar (StatusBarFeature) in the status bar of a real
// QMainWindow, offscreen, over a real SessionModel with its idle scheduler, a
// real executor (JobQueue) and CalculationDemand, with the synthetic
// calculations of jobfixture.h and the plots of plotfixture.h.
//
// What is proved here is what the status bar owns: the activity area's items
// (the scheduler's four tasks under their labels and counts, the computations
// as one item, the fill never an item), the one rule for the shown item, the
// hover, the cancel button, the warning with its count and hover, the
// constant height, and the end of the demand layer before the bar. The
// values it presents are the scheduler's reports and the demand layer's
// progress() and failures(), which tst_session_model_engine and
// tst_calculation_demand prove.
//
// The tests read what the user sees: the label's text, the bar's value and
// range, the tooltips, visibility and the bar's height. They find the widgets
// by object name, click the real buttons with QTest, and emit the scheduler's
// signals by hand where a state is easier made than waited for.
//
// Synchronization: Gate::waitEntered() proves the worker is inside a compute
// function; QTRY_*, waitDemandIdle() and waitForIdle() spin the event loop;
// CalculationDemand::flush() runs a pending pass before a value is read.

#include <functional>
#include <memory>

#include <QApplication>
#include <QHashFunctions>
#include <QJsonObject>
#include <QLabel>
#include <QMainWindow>
#include <QProgressBar>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QStatusBar>
#include <QStyle>
#include <QStyleFactory>
#include <QToolButton>
#include <QToolTip>
#include <QtTest>

#include "calculationdemand.h"
#include "demandstate.h"
#include "engine/calculationregistry.h"
#include "idlescheduler.h"
#include "jobfixture.h"
#include "jobqueue.h"
#include "logbookcolumn.h"
#include "logbookmanager.h"
#include "logbookprobe.h"
#include "plotfixture.h"
#include "plotmodel.h"
#include "preferences/preferencekeys.h"
#include "preferences/preferencesmanager.h"
#include "sessiondata.h"
#include "sessionmodel.h"
#include "testenvironment.h"
#include "ui/docks/AppContext.h"
#include "ui/statusbar/StatusBarFeature.h"

using namespace FlySight;
using namespace FlySightTest;

class StatusBarTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void schedulerTasksShowTheirLabelsAndCounts();
    void computationsAreOneItem();
    void taskShownOverComputationsAndHoverListsBoth();
    void cancelOnlyForACancellableShownTask();
    void warningBesideComputationsThenAlone();
    void warningCountsRecordingsAndListsThem();
    void warningListsAtMostTenRecordings();
    void warningAbsentWhenNothingFailedAndNotDismissable();
    void warningAfterRestart();
    void heightNeverChanges();
    void activityLeftAndWarningRight();
    void survivesDemandDestroyedFirst();

private:
    Gate &gate() { return m_world->gate(); }
    IdleScheduler &scheduler() { return m_model->scheduler(); }

    /// A window whose status bar the component fills, over the current model
    /// and demand layer (which may be null), shown. With `waitExposed`, the
    /// event loop runs until the window is exposed; false when it never is.
    [[nodiscard]] bool buildUi(bool waitExposed = true);
    void destroyUi();
    /// A simulated application restart: the index flushed (and edited by
    /// `editIndex`, when given), everything torn down, then new logbook state,
    /// a new model of stubs from the index, a new executor, demand layer and
    /// status bar. Runs no event loop (so no pass and no scheduler tick) and
    /// does not start the column worker.
    [[nodiscard]] bool restart(const std::function<void(QJsonObject &)> &editIndex = {});

    // ---- What the user sees -----------------------------------------------------
    QStatusBar *statusBar() const { return m_window->statusBar(); }
    template <class T>
    T *part(const char *name) const { return statusBar()->findChild<T *>(QString::fromLatin1(name)); }
    QLabel *label() const { return part<QLabel>("statusActivityLabel"); }
    QProgressBar *bar() const { return part<QProgressBar>("statusActivityBar"); }
    QToolButton *cancelButton() const { return part<QToolButton>("statusCancelButton"); }
    QWidget *warning() const { return part<QWidget>("statusWarning"); }
    QLabel *warningText() const { return part<QLabel>("statusWarningText"); }
    QLabel *warningIcon() const { return part<QLabel>("statusWarningIcon"); }
    /// The shown item's text; empty when the activity area shows nothing.
    QString shownText() const { return label()->isVisible() ? label()->text() : QString(); }
    /// The activity area's hover (the label's and the bar's).
    QString hoverText() const { return label()->toolTip(); }
    bool warningShown() const { return warning()->isVisible(); }
    /// The warning's text while shown; empty otherwise.
    QString warningShownText() const { return warningShown() ? warningText()->text() : QString(); }
    /// The activity area shows nothing: no text, no bar, no button, no hover.
    bool activityEmpty() const
    {
        return !label()->isVisible() && label()->text().isEmpty() && !bar()->isVisible()
            && !cancelButton()->isVisible() && label()->toolTip().isEmpty() && bar()->toolTip().isEmpty();
    }
    /// The label and the bar agree: both shown or both hidden, one hover, and
    /// a shown label's "<done> / <total>" is the bar's value and range.
    bool consistent() const
    {
        if (label()->isVisible() != bar()->isVisible() || label()->toolTip() != bar()->toolTip())
            return false;
        if (!label()->isVisible())
            return true;
        static const QRegularExpression count(QStringLiteral(": (\\d+) / (\\d+)$"));
        const QRegularExpressionMatch match = count.match(label()->text());
        return match.hasMatch() && bar()->minimum() == 0 && bar()->value() == match.captured(1).toInt()
            && bar()->maximum() == match.captured(2).toInt();
    }
    /// The status bar's height once pending layout requests are processed.
    int barHeight()
    {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
        QApplication::processEvents();
        return statusBar()->height();
    }
    /// A widget's geometry in the bar's coordinates, once the bar is laid out.
    QRect inBar(QWidget *widget)
    {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
        QApplication::processEvents();
        return QRect(widget->mapTo(statusBar(), QPoint(0, 0)), widget->size());
    }

    // ---- Driving ---------------------------------------------------------------
    /// The description column plus attribute columns over `keys`, through
    /// LogbookColumnStore::setColumns(): the path of a profile and the editor.
    static void enableColumns(const QStringList &keys)
    {
        QVector<LogbookColumn> columns{descriptionColumn()};
        for (const QString &key : keys)
            columns.append(attributeColumn(key));
        LogbookColumnStore::instance().setColumns(columns);
    }
    void check(const char *measurement, bool enabled = true)
    {
        m_plots->setPlotEnabled(QStringLiteral("Syn"), QString::fromLatin1(measurement), enabled);
    }
    /// The application's edit path for each session; false when the model refused one.
    [[nodiscard]] bool giveInput(const QStringList &ids, const char *key, double value)
    {
        for (const QString &id : ids) {
            if (!PlotFixture::giveInput(*m_model, id, QString::fromLatin1(key), value))
                return false;
        }
        return true;
    }
    /// Every session's input-settle wait ends now, and the pass runs.
    void settle()
    {
        m_demand->endInputSettleWaits();
        m_demand->flush();
    }
    void spin()
    {
        PlotFixture::spin(m_demand.get());
        QApplication::processEvents();
    }
    [[nodiscard]] bool waitDemandIdle(int timeoutMs = 30000)
    {
        return FlySightTest::waitDemandIdle(*m_queue, *m_demand, timeoutMs);
    }
    bool isLoaded(const QString &id) const
    {
        const int row = m_model->getSessionRow(id);
        return row >= 0 && std::as_const(*m_model).rowAt(row).isLoaded();
    }
    bool isVisibleRow(const QString &id) const
    {
        const int row = m_model->getSessionRow(id);
        return row >= 0 && std::as_const(*m_model).rowAt(row).visible;
    }
    QStringList sessionIds() const
    {
        QStringList ids;
        for (int r = 0; r < m_model->rowCount(); ++r)
            ids.append(std::as_const(*m_model).rowAt(r).sessionId);
        return ids;
    }
    /// Hides every row and evicts every hidden row (capacity 0), then sets the
    /// capacity back to 50. False when a row is still loaded.
    [[nodiscard]] bool makeStubs()
    {
        const QStringList ids = sessionIds();
        PlotFixture::show(*m_model, ids, false);
        PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 0);
        PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 50);
        for (const QString &id : ids) {
            if (isLoaded(id))
                return false;
        }
        return true;
    }
    /// Sessions "s<first>".."s<last>" added to the model, each named "Jump <k>".
    [[nodiscard]] bool addSessions(int first, int last)
    {
        QStringList ids;
        for (int k = first; k <= last; ++k)
            ids.append(QStringLiteral("s%1").arg(k));
        m_model->mergeSessions(JobWorld::sessions(ids));
        if (!waitForIdle(*m_model))
            return false;
        for (int k = first; k <= last; ++k) {
            if (!m_model->updateAttribute(QStringLiteral("s%1").arg(k), QString::fromLatin1(SessionKeys::Description),
                                          QStringLiteral("Jump %1").arg(k)))
                return false;
        }
        return waitForIdle(*m_model);
    }
    static bool stored(const char *sessionId, const char *calculationId)
    {
        return LogbookManager::instance()
            .knownCalculationRecords(QString::fromLatin1(sessionId))
            .contains(QString::fromLatin1(calculationId));
    }
    static int loadsOf(const QSignalSpy &spy, const char *id)
    {
        int count = 0;
        for (const QList<QVariant> &arguments : spy) {
            if (arguments.at(0).toString() == QLatin1String(id))
                ++count;
        }
        return count;
    }
    /// A task's activation and its report, as a tick emits them.
    void reportTask(int id, bool cancellable, int remaining, int total)
    {
        emit scheduler().activeTaskChanged(id, cancellable);
        emit scheduler().progressChanged(id, remaining, total);
    }

    std::unique_ptr<JobWorld> m_world;
    std::unique_ptr<PlotFixture> m_fixture;
    std::unique_ptr<SessionModel> m_model;
    std::unique_ptr<JobQueue> m_queue;
    std::unique_ptr<PlotModel> m_plots;
    std::unique_ptr<CalculationDemand> m_demand;
    std::unique_ptr<QMainWindow> m_window;
    StatusBarFeature *m_feature = nullptr;      // a child of the window, as in MainWindow
    QStringList m_registryBefore;
};

void StatusBarTest::initTestCase()
{
    TestEnvironment::instance().registerBuiltIns();

    PreferencesManager::instance().registerPreference(PreferenceKeys::LogbookColumnsVersion, 0);
    LogbookColumnStore::instance().setColumns({descriptionColumn()});
}

// Four loaded, hidden, saved and indexed sessions "s1".."s4" named "Jump 1"..
// "Jump 4", in that row order; G_IN is 1, 2 and 4 on s1, s2 and s4 (s3 has
// none: not applicable). No plot is checked and no column over a requested
// calculation is enabled.
void StatusBarTest::init()
{
    TestEnvironment &env = TestEnvironment::instance();
    env.useFreshLogbook();
    env.resetPreferencesToDefaults();
    LogbookManager::instance().initialize();
    m_registryBefore = CalculationRegistry::instance().registeredIds();

    m_world = std::make_unique<JobWorld>();
    m_fixture = std::make_unique<PlotFixture>();
    m_model = std::make_unique<SessionModel>();
    m_model->mergeSessions(JobWorld::sessions({"s1", "s2", "s3", "s4"}));
    QCOMPARE(m_model->rowCount(), 4);
    QVERIFY(waitForIdle(*m_model));
    for (int i = 1; i <= 4; ++i) {
        QVERIFY(m_model->updateAttribute(QStringLiteral("s%1").arg(i), QString::fromLatin1(SessionKeys::Description),
                                         QStringLiteral("Jump %1").arg(i)));
    }
    QVERIFY(giveInput({"s1"}, "G_IN", 1));
    QVERIFY(giveInput({"s2"}, "G_IN", 2));
    QVERIFY(giveInput({"s4"}, "G_IN", 4));
    m_model->flushPendingInvalidations();
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(sessionIds(), QStringList({"s1", "s2", "s3", "s4"}));

    m_plots = std::make_unique<PlotModel>();
    m_plots->setPlots(PlotFixture::plots());
    m_queue = std::make_unique<JobQueue>(m_model.get());
    m_demand = std::make_unique<CalculationDemand>(m_model.get(), m_plots.get(), m_queue.get());
    QVERIFY(buildUi());
}

bool StatusBarTest::buildUi(bool waitExposed)
{
    m_window = std::make_unique<QMainWindow>();
    m_window->setCentralWidget(new QWidget(m_window.get()));
    AppContext ctx;
    ctx.sessionModel = m_model.get();
    ctx.calculationDemand = m_demand.get();
    m_feature = new StatusBarFeature(ctx, m_window->statusBar(), m_window.get());
    m_window->resize(800, 240);
    m_window->show();
    if (waitExposed) {
        if (!QTest::qWaitForWindowExposed(m_window.get()))
            return false;
        QApplication::processEvents();
    }
    return label() && bar() && cancelButton() && warning() && warningText() && warningIcon();
}

void StatusBarTest::destroyUi()
{
    QToolTip::hideText();
    m_feature = nullptr;
    m_window.reset();
}

bool StatusBarTest::restart(const std::function<void(QJsonObject &)> &editIndex)
{
    m_model->flushDirtySessions();
    destroyUi();
    m_demand.reset();
    m_queue->shutdown();
    m_queue.reset();
    m_model.reset();
    if (editIndex) {
        // The index as the manager holds it, so that nothing it still holds
        // is written over the edit
        LogbookManager::instance().flushIndex();
        QJsonObject root = readIndex();
        editIndex(root);
        if (!writeIndex(root))
            return false;
    }
    LogbookManager &logbook = LogbookManager::instance();
    TestEnvironment::instance().reopenLogbook();
    logbook.initialize();
    m_model = std::make_unique<SessionModel>();
    m_model->populateFromIndex(logbook.cachedColumnValues(LogbookColumnStore::instance().enabledColumns()),
                               logbook.lastAccessedMap());
    m_queue = std::make_unique<JobQueue>(m_model.get());
    m_demand = std::make_unique<CalculationDemand>(m_model.get(), m_plots.get(), m_queue.get());
    return buildUi(false);
}

// The status bar goes before the demand layer, as in MainWindow's children.
void StatusBarTest::cleanup()
{
    if (m_world)
        gate().open(16);
    bool becameIdle = true;
    if (m_queue) {
        becameIdle = m_demand ? waitDemandIdle() : waitIdle(*m_queue);
        m_queue->shutdown();
    }

    destroyUi();
    m_demand.reset();
    QStringList stillPinned;
    if (m_model) {
        for (const QString &id : sessionIds()) {
            if (m_model->isSessionPinned(id))
                stillPinned.append(id);
        }
    }
    m_plots.reset();
    m_queue.reset();
    m_model.reset();
    LogbookColumnStore::instance().setColumns({descriptionColumn()});
    PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 50);

    const QStringList withoutFixture = [this] {
        QStringList ids = CalculationRegistry::instance().registeredIds();
        for (const QString &id : m_fixture ? m_fixture->registeredIds() : QStringList())
            ids.removeAll(id);
        return ids;
    }();
    m_fixture.reset();
    const QStringList afterFixture = CalculationRegistry::instance().registeredIds();
    m_world.reset();

    QVERIFY(becameIdle);
    QCOMPARE(stillPinned, QStringList());
    QCOMPARE(afterFixture, withoutFixture);
    QCOMPARE(CalculationRegistry::instance().registeredIds(), m_registryBefore);
    QCOMPARE(CalculationRegistry::instance().enrolledEngineCount(), 0);
}

// ---- The activity area -------------------------------------------------------------

// Spec 5 and 11: each scheduler task under its label, with the scheduler's
// count for that id as "<label>: <done> / <total>" on the label and the bar;
// a report for another id changes nothing; idle empties the area.
void StatusBarTest::schedulerTasksShowTheirLabelsAndCounts()
{
    QVERIFY(activityEmpty());
    QVERIFY(!warningShown());

    struct Expected {
        int id;
        bool cancellable;
        const char *text;
    };
    const QList<Expected> expected = {
        {SessionModel::SaveTask, false, "Saving sessions: 1 / 3"},
        {SessionModel::LoadTask, true, "Loading sessions: 1 / 3"},
        {SessionModel::BulkEditTask, true, "Updating sessions: 1 / 3"},
        {SessionModel::ColumnTask, true, "Computing columns: 1 / 3"},
    };
    for (const Expected &e : expected) {
        reportTask(e.id, e.cancellable, 2, 3);
        QCOMPARE(shownText(), QString::fromLatin1(e.text));
        QVERIFY2(bar()->isVisible(), e.text);
        QCOMPARE(bar()->value(), 1);
        QCOMPARE(bar()->maximum(), 3);
        QCOMPARE(cancelButton()->isVisible(), e.cancellable);
        QCOMPARE(hoverText(), QString::fromLatin1(e.text));
        QVERIFY2(consistent(), e.text);
    }

    // A report for another id than the active one changes nothing
    emit scheduler().progressChanged(SessionModel::SaveTask, 0, 5);
    emit scheduler().progressChanged(SessionModel::ColumnFillTask, 0, 5);
    QCOMPARE(shownText(), QStringLiteral("Computing columns: 1 / 3"));
    QCOMPARE(bar()->value(), 1);
    QCOMPARE(bar()->maximum(), 3);
    QCOMPARE(hoverText(), QStringLiteral("Computing columns: 1 / 3"));

    // The scheduler idle: empty; a late report changes nothing
    emit scheduler().schedulerIdle();
    QVERIFY(activityEmpty());
    emit scheduler().progressChanged(SessionModel::ColumnTask, 1, 3);
    QVERIFY(activityEmpty());

    // An id that is not an item (a task outside the four and the fill)
    reportTask(96, true, 1, 2);
    QVERIFY(activityEmpty());
    emit scheduler().schedulerIdle();

    // Live: a real bulk edit of the four rows, seen at each of its reports
    // (connected after the component, so the label is the one it just set)
    QObject scope;
    QStringList seen;
    connect(&scheduler(), &IdleScheduler::progressChanged, &scope, [&](int id, int, int) {
        if (id == SessionModel::BulkEditTask) {
            seen.append(shownText());
            QVERIFY(consistent());
            QVERIFY(cancelButton()->isVisible());
        }
    });
    m_model->startBulkEdit({0, 1, 2, 3}, 0, QStringLiteral("edited"));
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(!seen.isEmpty());
    int done = 0;
    for (const QString &text : std::as_const(seen)) {
        const QRegularExpressionMatch match =
            QRegularExpression(QStringLiteral("^Updating sessions: (\\d) / 4$")).match(text);
        QVERIFY2(match.hasMatch(), qPrintable(text));
        QVERIFY2(match.captured(1).toInt() >= done, qPrintable(text));
        done = match.captured(1).toInt();
    }
    QVERIFY(seen.contains(QStringLiteral("Updating sessions: 1 / 4")));
    QCOMPARE(seen.last(), QStringLiteral("Updating sessions: 4 / 4"));
    QVERIFY(activityEmpty());
}

// Spec 5: the computations are one item, counting sessions with a waiting or
// running track in plots and columns together, each once, out of the
// high-water mark; the fill is never shown and never reports; a new burst
// starts its own total.
void StatusBarTest::computationsAreOneItem()
{
    // s1 and s3 shown (loaded, visible), s2 and s4 stubs. (A stub without
    // the input would count as well until the fill has loaded it.)
    QVERIFY(makeStubs());
    PlotFixture::show(*m_model, {"s1", "s3"});
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(isLoaded(QStringLiteral("s1")));
    QVERIFY(isLoaded(QStringLiteral("s3")));
    QVERIFY(!isLoaded(QStringLiteral("s2")));
    QVERIFY(!isLoaded(QStringLiteral("s4")));

    QObject scope;
    QStringList fillTexts;                  // what is shown at each activation of the fill
    bool fillButton = false;
    int fillReports = 0;
    connect(&scheduler(), &IdleScheduler::activeTaskChanged, &scope, [&](int id, bool) {
        if (id != SessionModel::ColumnFillTask)
            return;
        fillTexts.append(shownText());
        fillButton = fillButton || cancelButton()->isVisible();
    });
    connect(&scheduler(), &IdleScheduler::progressChanged, &scope, [&](int id, int, int) {
        if (id == SessionModel::ColumnFillTask)
            ++fillReports;
    });
    QStringList computing;                  // each new computations line of the hover
    connect(m_demand.get(), &CalculationDemand::progressChanged, &scope, [&] {
        for (const QString &line : hoverText().split(QLatin1Char('\n'))) {
            if (line.startsWith(QStringLiteral("Computing results: "))
                && (computing.isEmpty() || computing.last() != line))
                computing.append(line);
        }
    });

    // The plot Syn/g over s1 and s3 and the column G_OUT over every session:
    // s1 (both), s2 and s4 (the column; s3 has no input) - three sessions
    check("g");
    enableColumns({QStringLiteral("G_OUT")});
    QVERIFY(gate().waitEntered());
    QTRY_COMPARE(shownText(), QStringLiteral("Computing results: 0 / 3"));
    QVERIFY(!cancelButton()->isVisible());
    QVERIFY(consistent());
    QCOMPARE(m_demand->progress().count, 3);
    QCOMPARE(m_queue->job(m_queue->runningJob()).sessionId, QStringLiteral("s1"));

    // Falling as the gate opens, across both sources
    gate().open(1);
    QTRY_COMPARE(shownText(), QStringLiteral("Computing results: 1 / 3"));
    QVERIFY(gate().waitEntered());
    gate().open(1);
    QTRY_COMPARE(shownText(), QStringLiteral("Computing results: 2 / 3"));
    QVERIFY(gate().waitEntered());
    gate().open(1);
    QVERIFY(waitDemandIdle());
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(activityEmpty());
    const qsizetype from = computing.indexOf(QStringLiteral("Computing results: 0 / 3"));
    QVERIFY2(from >= 0, qPrintable(computing.join(QLatin1String(" | "))));
    QCOMPARE(computing.mid(from), QStringList({"Computing results: 0 / 3", "Computing results: 1 / 3",
                                               "Computing results: 2 / 3"}));

    // The fill was active, and the computations were shown for it, with no
    // cancel button; it reported nothing of its own
    QVERIFY(!fillTexts.isEmpty());
    for (const QString &text : std::as_const(fillTexts))
        QVERIFY2(text.startsWith(QStringLiteral("Computing results: ")), qPrintable(text));
    QVERIFY(!fillButton);
    QCOMPARE(fillReports, 0);

    // A later burst starts its own total
    computing.clear();
    QVERIFY(giveInput({"s2"}, "G_IN", 20));
    settle();
    QTRY_COMPARE(shownText(), QStringLiteral("Computing results: 0 / 1"));
    QVERIFY(gate().waitEntered());
    QCOMPARE(computing.first(), QStringLiteral("Computing results: 0 / 1"));
    gate().open(1);
    QVERIFY(waitDemandIdle());
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(activityEmpty());
    QCOMPARE(fillReports, 0);
}

// Spec 6: with a task and computations in progress, the task is shown and the
// hover lists both, the computations with the recording and its step; when
// the task ends, the computations return while they continue.
void StatusBarTest::taskShownOverComputationsAndHoverListsBoth()
{
    PlotFixture::show(*m_model, {"s1"});
    check("g");
    QVERIFY(gate().waitEntered());
    QTRY_COMPARE(m_demand->progress().progressText, QStringLiteral("step 1"));
    const QString computations = QStringLiteral("Computing results: 0 / 1\n  Jump 1: step 1");
    QTRY_COMPARE(hoverText(), computations);
    QCOMPARE(shownText(), QStringLiteral("Computing results: 0 / 1"));

    // A bulk edit of the three other rows, seen at each of its reports
    QObject scope;
    QStringList texts;
    QStringList hovers;
    bool buttonEverHidden = false;
    connect(&scheduler(), &IdleScheduler::progressChanged, &scope, [&](int id, int, int) {
        if (id != SessionModel::BulkEditTask)
            return;
        texts.append(shownText());
        hovers.append(hoverText());
        buttonEverHidden = buttonEverHidden || !cancelButton()->isVisible();
    });
    m_model->startBulkEdit({1, 2, 3}, 0, QStringLiteral("edited"));
    QVERIFY(waitForIdle(*m_model));

    QVERIFY(!texts.isEmpty());
    for (qsizetype i = 0; i < texts.size(); ++i) {
        QVERIFY2(texts.at(i).startsWith(QStringLiteral("Updating sessions: ")), qPrintable(texts.at(i)));
        QCOMPARE(hovers.at(i), texts.at(i) + QLatin1Char('\n') + computations);
    }
    QVERIFY(hovers.contains(QStringLiteral("Updating sessions: 1 / 3\n"
                                           "Computing results: 0 / 1\n"
                                           "  Jump 1: step 1")));
    QVERIFY(!buttonEverHidden);

    // The task ended: the computations return, and they did not stop
    QCOMPARE(shownText(), QStringLiteral("Computing results: 0 / 1"));
    QCOMPARE(hoverText(), computations);
    QVERIFY(!cancelButton()->isVisible());
    QVERIFY(consistent());
    QCOMPARE(m_queue->job(m_queue->runningJob()).sessionId, QStringLiteral("s1"));
    QCOMPARE(m_demand->progress().count, 1);

    gate().open(1);
    QVERIFY(waitDemandIdle());
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(activityEmpty());
}

// Spec 6: the cancel button shows exactly while the shown item is a task the
// scheduler reported cancellable (never for saving, the fill or the
// computations), and a click cancels that task.
void StatusBarTest::cancelOnlyForACancellableShownTask()
{
    // By hand: every task, as registered
    const QList<std::pair<int, bool>> tasks = {
        {SessionModel::SaveTask, false},
        {SessionModel::LoadTask, true},
        {SessionModel::BulkEditTask, true},
        {SessionModel::ColumnTask, true},
    };
    for (const auto &[id, cancellable] : tasks) {
        reportTask(id, cancellable, 1, 2);
        QVERIFY(label()->isVisible());
        QCOMPARE(cancelButton()->isVisible(), cancellable);
    }
    // The fill, with nothing to compute: nothing shown
    emit scheduler().activeTaskChanged(SessionModel::ColumnFillTask, false);
    QVERIFY(activityEmpty());
    emit scheduler().schedulerIdle();

    // The computations alone, under the fill, and under a task
    PlotFixture::show(*m_model, {"s1"});
    check("g");
    QVERIFY(gate().waitEntered());
    QTRY_COMPARE(shownText(), QStringLiteral("Computing results: 0 / 1"));
    QVERIFY(!cancelButton()->isVisible());
    emit scheduler().activeTaskChanged(SessionModel::ColumnFillTask, false);
    QCOMPARE(shownText(), QStringLiteral("Computing results: 0 / 1"));
    QVERIFY(!cancelButton()->isVisible());
    reportTask(SessionModel::LoadTask, true, 1, 2);
    QCOMPARE(shownText(), QStringLiteral("Loading sessions: 1 / 2"));
    QVERIFY(cancelButton()->isVisible());
    reportTask(SessionModel::SaveTask, false, 1, 2);
    QCOMPARE(shownText(), QStringLiteral("Saving sessions: 1 / 2"));
    QVERIFY(!cancelButton()->isVisible());
    emit scheduler().schedulerIdle();
    QCOMPARE(shownText(), QStringLiteral("Computing results: 0 / 1"));
    QVERIFY(!cancelButton()->isVisible());
    check("g", false);
    gate().open(1);
    QVERIFY(waitDemandIdle());
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(activityEmpty());

    // Live: a visible load of ten stubs, cancelled by a click on the button
    // once the first has loaded. The click is posted from the report, so that
    // it arrives from the event loop between two ticks, as a user's does.
    QVERIFY(addSessions(5, 10));
    QVERIFY(makeStubs());
    const QStringList ids = sessionIds();
    QCOMPARE(ids.size(), 10);
    QObject scope;
    bool clickPosted = false;
    bool clicked = false;
    QString shownAtClick;
    connect(&scheduler(), &IdleScheduler::progressChanged, &scope, [&](int id, int remaining, int total) {
        if (id != SessionModel::LoadTask || clickPosted || remaining == total)
            return;
        clickPosted = true;
        QMetaObject::invokeMethod(&scope, [&] {
            shownAtClick = shownText();
            if (cancelButton()->isVisible()) {
                QTest::mouseClick(cancelButton(), Qt::LeftButton);
                clicked = true;
            }
        }, Qt::QueuedConnection);
    });
    PlotFixture::show(*m_model, ids);
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(clickPosted);
    QVERIFY2(clicked, qPrintable(shownAtClick));
    QVERIFY2(shownAtClick.startsWith(QStringLiteral("Loading sessions: ")), qPrintable(shownAtClick));

    // Cancelled: the rows loaded before the click stay shown, the rows still
    // queued are unticked and not loaded
    int shown = 0;
    int unticked = 0;
    for (const QString &id : ids) {
        if (isVisibleRow(id)) {
            QVERIFY2(isLoaded(id), qPrintable(id));
            ++shown;
        } else {
            QVERIFY2(!isLoaded(id), qPrintable(id));
            ++unticked;
        }
    }
    QVERIFY(shown >= 1);
    QVERIFY(unticked >= 1);
    QVERIFY(activityEmpty());
}

// ---- The warning --------------------------------------------------------------------

// Spec 7: the warning is shown beside the computations while they continue,
// and alone once they end.
void StatusBarTest::warningBesideComputationsThenAlone()
{
    // A stored rejection, completed first: EA1 over s1 at -1
    QVERIFY(giveInput({"s1"}, "EA_IN", -1));
    m_model->flushPendingInvalidations();
    enableColumns({QStringLiteral("EA1")});
    QVERIFY(waitDemandIdle());
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(stored("s1", "expA"));
    QCOMPARE(warningShownText(), QStringLiteral("1 session could not be computed"));
    QVERIFY(activityEmpty());

    // G_OUT with the gate held: the warning beside "Computing results"
    enableColumns({QStringLiteral("EA1"), QStringLiteral("G_OUT")});
    QVERIFY(gate().waitEntered());
    QTRY_VERIFY(shownText().startsWith(QStringLiteral("Computing results: ")));
    QVERIFY(warningShown());
    QCOMPARE(warningText()->text(), QStringLiteral("1 session could not be computed"));
    QVERIFY(label()->isVisible() && bar()->isVisible());

    // The gate opens: the warning alone
    gate().open(16);
    QVERIFY(waitDemandIdle());
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(activityEmpty());
    QCOMPARE(warningShownText(), QStringLiteral("1 session could not be computed"));
    QCOMPARE(warning()->toolTip(), SessionFailures::listText(m_demand->failures()));
}

// Spec 7: the warning counts recordings, not pairs, shows the standard
// warning icon, and its hover is the capped list of what failed and why.
void StatusBarTest::warningCountsRecordingsAndListsThem()
{
    // s1 fails two calculations: a stored rejection and a computation that
    // throws (tried again at the next start)
    QVERIFY(giveInput({"s1"}, "EA_IN", -1));
    QVERIFY(giveInput({"s1"}, "T_IN", 1));
    m_model->flushPendingInvalidations();
    enableColumns({QStringLiteral("EA1"), QStringLiteral("T_OUT")});
    QVERIFY(waitDemandIdle());
    QVERIFY(waitForIdle(*m_model));
    m_demand->flush();
    QCOMPARE(m_demand->failures().size(), 1);
    QCOMPARE(m_demand->failures().at(0).calculations.size(), 2);
    QCOMPARE(warningShownText(), QStringLiteral("1 session could not be computed"));
    QCOMPARE(warning()->toolTip(), SessionFailures::listText(m_demand->failures()));
    QCOMPARE(warning()->toolTip(), QStringLiteral("Jump 1\n"
                                                  "  Explicit A: negative input\n"
                                                  "  Thrower: synthetic failure (tried again at the next start)"));

    // The style's standard warning icon, at the small icon size
    QStyle *style = statusBar()->style();
    const int extent = style->pixelMetric(QStyle::PM_SmallIconSize, nullptr, warningIcon());
    const QPixmap expected = style->standardIcon(QStyle::SP_MessageBoxWarning, nullptr, warningIcon())
                                 .pixmap(QSize(extent, extent), warningIcon()->devicePixelRatioF());
    QVERIFY(!expected.isNull());
    QVERIFY(warningIcon()->isVisible());
    QCOMPARE(warningIcon()->pixmap().toImage(), expected.toImage());

    // A second recording: two
    QVERIFY(giveInput({"s2"}, "EA_IN", -1));
    settle();
    QVERIFY(waitDemandIdle());
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(warningShownText(), QStringLiteral("2 sessions could not be computed"));
    QCOMPARE(warning()->toolTip(), SessionFailures::listText(m_demand->failures()));
    QVERIFY(warning()->toolTip().startsWith(QStringLiteral("Jump 1\n")));
    QVERIFY(warning()->toolTip().endsWith(QStringLiteral("\nJump 2\n  Explicit A: negative input")));
    QVERIFY(warning()->toolTip().contains(QStringLiteral("(tried again at the next start)")));
}

// Spec 7: the hover lists ten recordings at most and then says how many more.
void StatusBarTest::warningListsAtMostTenRecordings()
{
    QVERIFY(addSessions(5, 12));
    QStringList ids = sessionIds();
    QCOMPARE(ids.size(), 12);
    QVERIFY(giveInput(ids, "EA_IN", -1));
    m_model->flushPendingInvalidations();
    enableColumns({QStringLiteral("EA1")});
    QVERIFY(waitDemandIdle());
    QVERIFY(waitForIdle(*m_model));

    m_demand->flush();
    QCOMPARE(m_demand->failures().size(), 12);
    QCOMPARE(warningShownText(), QStringLiteral("12 sessions could not be computed"));
    const QString hover = warning()->toolTip();
    QCOMPARE(hover, SessionFailures::listText(m_demand->failures()));
    QVERIFY2(hover.endsWith(QStringLiteral("\nand 2 more")), qPrintable(hover));
    QCOMPARE(hover.split(QLatin1Char('\n')).size(), 2 * SessionFailures::kListLimit + 1);
}

// Spec 7: no warning when nothing failed; a click on it changes nothing; the
// last source over the calculation switched off hides it.
void StatusBarTest::warningAbsentWhenNothingFailedAndNotDismissable()
{
    QVERIFY(!warningShown());

    // A column computed without a failure: still none
    gate().open(16);
    enableColumns({QStringLiteral("G_OUT")});
    QVERIFY(waitDemandIdle());
    QVERIFY(waitForIdle(*m_model));
    m_demand->flush();
    QVERIFY(m_demand->failures().isEmpty());
    QVERIFY(!warningShown());
    QVERIFY(warningText()->text().isEmpty());
    QVERIFY(warning()->toolTip().isEmpty());

    // A failure: shown
    QVERIFY(giveInput({"s1"}, "EA_IN", -1));
    m_model->flushPendingInvalidations();
    enableColumns({QStringLiteral("G_OUT"), QStringLiteral("EA1")});
    QVERIFY(waitDemandIdle());
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(warningShown());
    const QList<SessionFailures> failures = m_demand->failures();
    QCOMPARE(failures.size(), 1);

    // A click, a double click and a click on its text change nothing, and start nothing
    {
        const Quiet quiet(*m_queue);
        QTest::mouseClick(warning(), Qt::LeftButton);
        QTest::mouseDClick(warning(), Qt::LeftButton);
        QTest::mouseClick(warningText(), Qt::LeftButton);
        spin();
        QVERIFY(quiet.holds());
    }
    QVERIFY(warningShown());
    QVERIFY(m_demand->failures() == failures);
    QCOMPARE(warningShownText(), QStringLiteral("1 session could not be computed"));

    // The last source over the calculation disabled: gone
    enableColumns({QStringLiteral("G_OUT")});
    m_demand->flush();
    QVERIFY(m_demand->failures().isEmpty());
    QVERIFY(!warningShown());
}

// Spec 7: after a restart a stored rejection wanted by a restored column is
// counted at the demand layer's first pass, without loading the session and
// without a job; a failure that is not stored is absent until its retry fails
// again. From an index without "recordReasons" (an earlier build) the
// rejection is counted once the column worker's pass has reported its reason,
// still without a load.
void StatusBarTest::warningAfterRestart()
{
    // A stored rejection of s1 (the column EA1), and an unstored failure of s2
    // (the plot Syn/t over the visible s2: the computation throws)
    QVERIFY(giveInput({"s1"}, "EA_IN", -1));
    QVERIFY(giveInput({"s2"}, "T_IN", 1));
    m_model->flushPendingInvalidations();
    enableColumns({QStringLiteral("EA1")});
    PlotFixture::show(*m_model, {"s2"});
    check("t");
    QVERIFY(waitDemandIdle());
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(stored("s1", "expA"));
    QVERIFY(!stored("s2", "thrower"));
    QCOMPARE(warningShownText(), QStringLiteral("2 sessions could not be computed"));

    // The next start, the plot still checked and every session hidden: the
    // first pass counts the stored rejection only
    QVERIFY(restart());
    QVERIFY(!isLoaded(QStringLiteral("s1")));
    QVERIFY(!isVisibleRow(QStringLiteral("s2")));
    {
        QSignalSpy loadedSpy(m_model.get(), &SessionModel::sessionLoaded);
        {
            const Quiet quiet(*m_queue);
            m_demand->flush();
            QCOMPARE(warningShownText(), QStringLiteral("1 session could not be computed"));
            QCOMPARE(warning()->toolTip(), QStringLiteral("Jump 1\n  Explicit A: negative input"));
            QCOMPARE(warning()->toolTip(), SessionFailures::listText(m_demand->failures()));
            QCOMPARE(loadedSpy.count(), 0);

            // The column worker's pass, then the fill, which loads the other
            // stubs to learn that EA1 does not apply to them
            m_model->startColumnWorker();
            QVERIFY(waitDemandIdle());
            QVERIFY(waitForIdle(*m_model));
            QCOMPARE(warningShownText(), QStringLiteral("1 session could not be computed"));
            QVERIFY(quiet.holds());
        }

        // s2 shown: its retry fails again
        PlotFixture::show(*m_model, {"s2"});
        QVERIFY(waitDemandIdle());
        QVERIFY(waitForIdle(*m_model));
        QCOMPARE(warningShownText(), QStringLiteral("2 sessions could not be computed"));
        QCOMPARE(warning()->toolTip(), SessionFailures::listText(m_demand->failures()));
        QCOMPARE(loadsOf(loadedSpy, "s1"), 0);
    }

    // Only the stored rejection is wanted; then a start from an index
    // without "recordReasons" and without s1's value of the column, so that
    // the column worker's copy restores s1's record
    check("t", false);
    PlotFixture::show(*m_model, {"s2"}, false);
    QVERIFY(waitDemandIdle());
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(warningShownText(), QStringLiteral("1 session could not be computed"));
    bool edited = false;
    QVERIFY(restart([&edited](QJsonObject &root) {
        QJsonObject sessions = root[QStringLiteral("sessions")].toObject();
        QJsonObject entry = sessions[QStringLiteral("s1")].toObject();
        edited = entry.contains(QStringLiteral("recordReasons"));
        entry.remove(QStringLiteral("recordReasons"));
        QJsonObject values = entry[QStringLiteral("values")].toObject();
        values.remove(indexColumnId(root, attributeColumn(QStringLiteral("EA1"))));
        entry[QStringLiteral("values")] = values;
        sessions[QStringLiteral("s1")] = entry;
        root[QStringLiteral("sessions")] = sessions;
    }));
    QVERIFY(edited);
    QCOMPARE(LogbookManager::instance().calculationRecordReason(QStringLiteral("s1"), QStringLiteral("expA")),
             QString());
    {
        QSignalSpy loadedSpy(m_model.get(), &SessionModel::sessionLoaded);
        const Quiet quiet(*m_queue);
        m_demand->flush();
        QVERIFY(!warningShown());
        m_model->startColumnWorker();
        QTRY_COMPARE(warningShownText(), QStringLiteral("1 session could not be computed"));
        QVERIFY(waitDemandIdle());
        QVERIFY(waitForIdle(*m_model));
        QCOMPARE(warningShownText(), QStringLiteral("1 session could not be computed"));
        QCOMPARE(warning()->toolTip(), QStringLiteral("Jump 1\n  Explicit A: negative input"));
        QCOMPARE(loadsOf(loadedSpy, "s1"), 0);
        QVERIFY(quiet.holds());
    }
}

// ---- The bar ------------------------------------------------------------------------

// Spec 5 and 11: the activity sits at the left of the bar, label then a
// compact bar then the cancel button, and the warning at the right; nothing
// keeps its width while hidden, so no gap is left where a hidden widget was.
void StatusBarTest::activityLeftAndWarningRight()
{
    QVERIFY(activityEmpty());
    QVERIFY(!warningShown());
    QVERIFY(!warning()->sizePolicy().retainSizeWhenHidden());
    QVERIFY(!cancelButton()->sizePolicy().retainSizeWhenHidden());

    // Wide enough for the label and the warning text at any font
    m_window->resize(1600, 240);
    reportTask(SessionModel::LoadTask, true, 1, 3);
    QVERIFY(cancelButton()->isVisible());
    const int width = statusBar()->width();
    QVERIFY(width >= 1500);
    const QRect labelRect = inBar(label());
    const QRect barRect = inBar(bar());
    const QRect buttonRect = inBar(cancelButton());
    const QRect activityRect = inBar(part<QWidget>("statusActivity"));
    const QString geometry = QStringLiteral("width %1 label %2-%3 bar %4-%5 button %6-%7 activity %8-%9")
                                 .arg(width).arg(labelRect.left()).arg(labelRect.right()).arg(barRect.left())
                                 .arg(barRect.right()).arg(buttonRect.left()).arg(buttonRect.right())
                                 .arg(activityRect.left()).arg(activityRect.right());
    QVERIFY2(labelRect.left() < width / 8, qPrintable(geometry));
    QVERIFY2(barRect.left() > labelRect.right(), qPrintable(geometry));
    QVERIFY2(barRect.left() - labelRect.right() < barRect.width(), qPrintable(geometry));   // beside its label
    QVERIFY2(barRect.width() <= 160, qPrintable(geometry));
    QVERIFY2(buttonRect.left() > barRect.right(), qPrintable(geometry));
    // The activity is as wide as its widgets, not stretched across the bar
    QVERIFY2(activityRect.width() <= label()->sizeHint().width() + barRect.width() + buttonRect.width() + 3 * 20,
             qPrintable(geometry));

    // The warning at the right, and the activity where it was
    QVERIFY(giveInput({"s1"}, "EA_IN", -1));
    m_model->flushPendingInvalidations();
    enableColumns({QStringLiteral("EA1")});
    QVERIFY(waitDemandIdle());
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(warningShown());
    reportTask(SessionModel::LoadTask, true, 1, 3);
    const QRect warningRect = inBar(warning());
    QVERIFY2(warningRect.left() > buttonRect.right() + 200, qPrintable(QString::number(warningRect.left())));
    QVERIFY2(width - warningRect.right() < 60, qPrintable(QString::number(width - warningRect.right())));
    QCOMPARE(inBar(label()), labelRect);
    QCOMPARE(inBar(bar()), barRect);

    // Idle again: the activity's widgets and container take no width
    emit scheduler().schedulerIdle();
    QVERIFY(activityEmpty());
    QCOMPARE(inBar(part<QWidget>("statusActivity")).width(), 0);
}

// Decision 7: the bar has one height idle, with a task, with the cancel
// button, with the warning, and with the warning beside a task and beside
// the computations.
void StatusBarTest::heightNeverChanges()
{
    const int idle = barHeight();
    QVERIFY(idle > 0);
    QVERIFY(activityEmpty());
    QVERIFY(!warningShown());

    reportTask(SessionModel::SaveTask, false, 2, 3);
    QVERIFY(label()->isVisible() && !cancelButton()->isVisible());
    QCOMPARE(barHeight(), idle);

    reportTask(SessionModel::LoadTask, true, 2, 3);
    QVERIFY(cancelButton()->isVisible());
    QCOMPARE(barHeight(), idle);

    emit scheduler().schedulerIdle();
    QVERIFY(activityEmpty());
    QCOMPARE(barHeight(), idle);

    // The warning alone
    QVERIFY(giveInput({"s1"}, "EA_IN", -1));
    m_model->flushPendingInvalidations();
    enableColumns({QStringLiteral("EA1")});
    QVERIFY(waitDemandIdle());
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(warningShown());
    QVERIFY(activityEmpty());
    QCOMPARE(barHeight(), idle);

    // Both: the warning beside a task with its cancel button ...
    reportTask(SessionModel::ColumnTask, true, 2, 3);
    QVERIFY(warningShown() && cancelButton()->isVisible());
    QCOMPARE(barHeight(), idle);
    emit scheduler().schedulerIdle();

    // ... and beside the computations
    enableColumns({QStringLiteral("EA1"), QStringLiteral("G_OUT")});
    QVERIFY(gate().waitEntered());
    QTRY_VERIFY(shownText().startsWith(QStringLiteral("Computing results: ")));
    QVERIFY(warningShown());
    QCOMPARE(barHeight(), idle);

    gate().open(16);
    QVERIFY(waitDemandIdle());
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(barHeight(), idle);
}

// Decision 6: MainWindow destroys the demand layer before its children. The
// computations and the warning go at once, a task is still shown, and a
// component over a null demand layer shows tasks only.
void StatusBarTest::survivesDemandDestroyedFirst()
{
    QVERIFY(giveInput({"s1"}, "EA_IN", -1));
    m_model->flushPendingInvalidations();
    enableColumns({QStringLiteral("EA1")});
    QVERIFY(waitDemandIdle());
    QVERIFY(waitForIdle(*m_model));
    enableColumns({QStringLiteral("EA1"), QStringLiteral("G_OUT")});
    QVERIFY(gate().waitEntered());
    QTRY_VERIFY(shownText().startsWith(QStringLiteral("Computing results: ")));
    QVERIFY(warningShown());

    m_demand.reset();
    QVERIFY(activityEmpty());
    QVERIFY(!warningShown());
    QVERIFY(warning()->toolTip().isEmpty());
    QApplication::processEvents();

    // A task is still shown, with no computations in its hover
    reportTask(SessionModel::LoadTask, true, 1, 4);
    QCOMPARE(shownText(), QStringLiteral("Loading sessions: 3 / 4"));
    QCOMPARE(hoverText(), QStringLiteral("Loading sessions: 3 / 4"));
    QVERIFY(cancelButton()->isVisible());
    QVERIFY(!warningShown());
    emit scheduler().schedulerIdle();
    QVERIFY(activityEmpty());

    // A component made over a null demand layer: tasks only
    destroyUi();
    QVERIFY(buildUi());
    QVERIFY(activityEmpty());
    QVERIFY(!warningShown());
    reportTask(SessionModel::SaveTask, false, 0, 2);
    QCOMPARE(shownText(), QStringLiteral("Saving sessions: 2 / 2"));
    QVERIFY(!cancelButton()->isVisible());
    QVERIFY(consistent());
    emit scheduler().schedulerIdle();
    QVERIFY(activityEmpty());
}

// A Widgets test writes its own main() (tests/README.md section 8): the same
// order as FLYSIGHT_TEST_MAIN, with a QApplication and the application's style.
int main(int argc, char **argv)
{
    QHashSeed::setDeterministicGlobalSeed();
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    FlySightTest::TestEnvironment env(QStringLiteral("StatusBarTest"));
    StatusBarTest tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_status_bar.moc"
