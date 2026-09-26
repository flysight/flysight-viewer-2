// The demand layer (CalculationDemand) on a real PlotModel, executor
// (JobQueue), SessionModel, real SessionData engines, and the global registry,
// with the synthetic plots of plotfixture.h over the explicit calculations of
// jobfixture.h. No widgets.
// Plot demand: checking a plot, showing a session, loading a visible session
// start work with no other call; unchecking and hiding drop the waiting pair;
// priority (the focused session, then row order, upstream first); the
// input-settle wait; the memory of job-level failures and not-applicable pairs;
// the per-plot state and its signals.
// Column demand: every enabled logbook column over a requested calculation,
// for every session, loaded or not; the column fill's hidden loads, the bound
// on holds and their release; settlements; tier (c); the per-column state and
// the pending cells; the ordering against saves and bulk edits.
//
// Synchronization: Gate::waitEntered() proves the worker is inside a compute
// function; QTRY_*, waitIdle() and waitDemandIdle() spin the event loop for
// main-thread effects; CalculationDemand::flush() runs a pending pass before a
// state is read; settle() ends the input-settle waits. There are no sleeps.

#include <memory>

#include <QFile>
#include <QJsonObject>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QtTest>

#include "builtinfixture.h"
#include "calculationdemand.h"
#include "engine/calculationdescriptor.h"
#include "engine/calculationengine.h"
#include "engine/calculationregistry.h"
#include "engine/calculationresult.h"
#include "engine/evaluationcontext.h"
#include "fakesessionstate.h"
#include "fixturebuilder.h"
#include "jobfixture.h"
#include "jobmodel.h"
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
#include "testmain.h"
#include "testutil.h"

using namespace FlySight;
using namespace FlySightTest;

using Kind = JobQueue::OfferResult::Kind;

namespace {

const char kNoLongerNeeded[] = "No longer needed";

/// A (remaining, total) progress report.
QPair<int, int> progressOf(int remaining, int total)
{
    return {remaining, total};
}

} // namespace

class CalculationDemandTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void plotIdMatchesPlotModelRole();
    void ordinaryPlotsAreNeverInspected();
    void uncheckedPlotsAreNeverInspected();
    void hiddenAndStubRowsAreNotTracks();
    void failedLoadPlaceholderIsNotATrack();
    void rowScript();
    void chainedBlockersContinue();
    void heldChainContinues();
    void chainCompletesAfterFirstJobDoesNotSucceed_data();
    void chainCompletesAfterFirstJobDoesNotSucceed();
    void chainStopsForHiddenTrackOrUncheckedPlot_data();
    void chainStopsForHiddenTrackOrUncheckedPlot();
    void programmaticCheckCreatesDemand();
    void profileStyleApplyCreatesDemand();
    void startupRestoreWithHiddenSessionsStartsNothing();
    void showingASessionStartsIt();
    void hidingASessionDropsItsWaitingPair();
    void uncheckingDropsWaitingPairsKeepsRunning();
    void waitingPairNeededByAnotherPlotSurvives();
    void loadingAVisibleSessionStartsIt();
    void mergeCreatesDemandForShownSessions();
    void inputBurstRunsOneJob();
    void staleRunningJobIsWaitingAtOnce();
    void supersededJobIsRunAgainAfterInputsSettle();
    void sessionWithoutInputIsNeverListed();
    void onlyRequestableCalculationsAreOffered();
    void resultAppearingWhileWaitingDropsThePair();
    void inputDeterminedFailureIsStoredBadgedNeverRerun();
    void jobLevelFailureIsBadgedNotRerunUntilRestart_data();
    void jobLevelFailureIsBadgedNotRerunUntilRestart();
    void failuresListedWhileWorking();
    void merelyUncomputedIsNotWorthAWarning();
    void sharedJobSameProgress();
    void plotCheckedDuringAJobJoinsIt();
    void focusedSessionFirstThenRowOrder();
    void changingDemandReplacesChosenNext();
    void executorHoldsAtMostRunningAndChosenNext();
    void tooltipText();
    void changeSignalsAreMinimal();
    void dependencyBurstIsCoalesced();
    void progressUpdatesWithoutInspection();
    void removedSessionLeavesNoTrace();
    void registryChangeReclassifies();
    void survivesExecutorShutdown();
    void nullCollaborators();

    // Column demand
    void columnIdIsTheDefinitionKey();
    void ordinaryColumnsCreateNoDemand();
    void enablingColumnFillsEveryUnloadedSession_data();
    void enablingColumnFillsEveryUnloadedSession();
    void loadedHiddenSessionsNeedNoLoad();
    void sessionShownDuringColumnDemandRunsNext();
    void columnPriorityFollowsRowOrderAfterPlots();
    void visibleSessionsFirstWithinColumnDemand();
    void chainedColumnWithUpstreamRecordIsCompleted();
    void storedRejectionIsBadgedAfterRestartWithoutLoad();
    void fillTaskReportsProgressWhileWaiting();
    void storedResultsCreateNoJob();
    void notApplicableSessionIsSettledWithoutAJob();
    void columnFailuresAreBadgedNotReloaded();
    void columnJobLevelFailureIsNotReloadedUntilRestart();
    void columnOfferRefusalIsNotLeftPending();
    void unloadableSessionIsSettledAsFailed();
    void visibleFailedLoadIsSettledAsFailed();
    void chainedColumnKeepsItsHold();
    void disablingColumnReleasesHeldSessions();
    void heldSessionShownStaysLoaded();
    void removedOrRepopulatedHeldSessionIsReleased();
    void identityStubIsOfferedUnderItsRealId();
    void columnStateCountsAndPendingCells();
    void profileStyleColumnsCreateDemand();
    void startupWithEnabledColumnLoadsAfterColumnWorker();
    void savesAndBulkEditsPrecedeLoadStep();
    void bulkEditMakesSettledSessionApplicable();
    void fillTaskIsLowestAndNotCancellable();
    void noLoadsAfterExecutorShutdown();
    void demandDestroyedReleasesHoldsAndTask();
    void passOverManyStubsReadsEachRecordSetOnce();

    // Presentation
    void workingIdsFollowStates();
    void toolTipListsAtMostTenFailures();

private:
    SessionData &session(const QString &id) { return m_model->sessionRef(m_model->getSessionRow(id)); }
    CalculationEngine &engine(const QString &id) { return session(id).calculationEngine(); }
    bool isLoaded(const QString &id) const
    {
        const int row = m_model->getSessionRow(id);
        return row >= 0 && std::as_const(*m_model).rowAt(row).isLoaded();
    }
    Gate &gate() { return m_world->gate(); }

    void show(const QStringList &ids, bool visible = true) { PlotFixture::show(*m_model, ids, visible); }
    /// The application's edit path, for each session. False as soon as the
    /// model refuses one (the rest is then left alone); a test function checks
    /// it with QVERIFY, so that a failure ends the function.
    [[nodiscard]] bool giveInput(const QStringList &ids, const char *key, double value)
    {
        for (const QString &id : ids) {
            if (!PlotFixture::giveInput(*m_model, id, QString::fromLatin1(key), value)) {
                qWarning().noquote() << "giveInput: the model refused" << key << "for session" << id;
                return false;
            }
        }
        return true;
    }
    /// setPlotEnabled(): what a profile, the start-up restore and the Plots
    /// menu do. A click on the row's check box writes the same model.
    void check(const char *measurement, bool enabled = true)
    {
        m_plots->setPlotEnabled(QStringLiteral("Syn"), QString::fromLatin1(measurement), enabled);
    }
    /// The current state: a pending pass runs first.
    DemandState row(const char *plotId)
    {
        m_demand->flush();
        return m_demand->plotState(QString::fromLatin1(plotId));
    }
    QVector<double> values(const QString &sessionId, const char *measurement)
    {
        return session(sessionId).getMeasurement(QStringLiteral("Syn"), QString::fromLatin1(measurement));
    }
    /// The newest job of a session for a calculation; a default record when none.
    JobRecord jobOf(const QString &sessionId, const char *calculationId) const
    {
        const JobModel *jobs = m_queue->model();
        for (int r = jobs->rowCount() - 1; r >= 0; --r) {
            const JobRecord record = jobs->record(r);
            if (record.sessionId == sessionId && record.calculationId == QLatin1String(calculationId))
                return record;
        }
        return JobRecord();
    }
    int jobCount(const char *calculationId) const
    {
        int count = 0;
        const JobModel *jobs = m_queue->model();
        for (int r = 0; r < jobs->rowCount(); ++r) {
            if (jobs->record(r).calculationId == QLatin1String(calculationId))
                ++count;
        }
        return count;
    }
    JobState stateOf(JobId id) const { return m_queue->job(id).state; }
    /// The end states of every job, in offer order.
    QList<JobState> history() const
    {
        QList<JobState> states;
        const JobModel *jobs = m_queue->model();
        for (int r = 0; r < jobs->rowCount(); ++r)
            states.append(jobs->record(r).state);
        return states;
    }
    /// The executor's chosen next job, as a record (id 0 when none).
    JobRecord chosenNext() const { return m_queue->job(m_queue->chosenNextJob()); }
    JobRecord running() const { return m_queue->job(m_queue->runningJob()); }
    /// Two turns of the event loop and a flush: whatever was going to start by
    /// itself has started.
    void spin() { PlotFixture::spin(m_demand.get()); }
    /// Every session's input-settle wait ends now, and the pass runs.
    void settle()
    {
        m_demand->endInputSettleWaits();
        m_demand->flush();
    }
    [[nodiscard]] bool waitDemandIdle(int timeoutMs = 30000)
    {
        return FlySightTest::waitDemandIdle(*m_queue, *m_demand, timeoutMs);
    }
    /// A new demand layer over the same models and executor: what the next
    /// start of the application has (nothing is remembered).
    void restartDemand()
    {
        m_demand.reset();
        m_demand = std::make_unique<CalculationDemand>(m_model.get(), m_plots.get(), m_queue.get());
    }
    static bool stored(const QString &sessionId, const char *calculationId)
    {
        return LogbookManager::instance().knownCalculationRecords(sessionId).contains(QString::fromLatin1(calculationId));
    }
    int totalRuns()
    {
        int runs = 0;
        for (const char *id : {"s1", "s2", "s3", "s4"})
            runs += engine(id).totalRunCount();
        return runs;
    }
    /// A session "s5" (a new session for the model) with G_IN = 4 in its own
    /// data, so that no edit announces the input.
    static QList<SessionData> newSessionWithInput(const char *id)
    {
        QList<SessionData> sessions = JobWorld::sessions({QString::fromLatin1(id)});
        sessions.first().setAttribute(QStringLiteral("G_IN"), 4);
        return sessions;
    }


    // ---- Column demand ------------------------------------------------------
    static QString colId(const char *key) { return CalculationDemand::columnId(attributeColumn(QString::fromLatin1(key))); }
    /// The current state of the column over attribute `key`: a pending pass runs first.
    DemandState col(const char *key)
    {
        m_demand->flush();
        return m_demand->columnState(colId(key));
    }
    /// The column index of the attribute column over `key` in m_model; -1 when none.
    int section(const char *key) const
    {
        for (int c = 0; c < m_model->columnCount(); ++c) {
            const LogbookColumn &column = m_model->column(c);
            if (column.type == ColumnType::SessionAttribute && column.attributeKey == QLatin1String(key))
                return c;
        }
        return -1;
    }
    int rowOf(const QString &id) const { return m_model->getSessionRow(id); }
    const SessionRow &rowState(const QString &id) const { return std::as_const(*m_model).rowAt(rowOf(id)); }
    /// Whether the cell of (session, column over `key`) is pending: a pending pass runs first.
    bool isCellPending(const QString &id, const char *key)
    {
        m_demand->flush();
        return m_demand->isCellPending(rowOf(id), section(key));
    }
    /// Forgets the gate's entries so far: the next waitEntered() waits for a new one.
    void drainEntered()
    {
        while (gate().entered.tryAcquire(1)) {
        }
    }
    /// The description column plus attribute columns over `keys`, through
    /// LogbookColumnStore::setColumns(): the path applyProfile() step 7 takes.
    static void enableColumns(const QStringList &keys)
    {
        QVector<LogbookColumn> columns{descriptionColumn()};
        for (const QString &key : keys)
            columns.append(attributeColumn(key));
        LogbookColumnStore::instance().setColumns(columns);
    }
    /// Hides `ids` and evicts every hidden row (capacity 0), then sets the
    /// capacity back to m_capacity. False (with a warning) when one of `ids`
    /// is still loaded.
    [[nodiscard]] bool makeStubs(const QStringList &ids = {"s1", "s2", "s3", "s4"})
    {
        show(ids, false);
        PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 0);
        PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, m_capacity);
        for (const QString &id : ids) {
            if (isLoaded(id)) {
                qWarning().noquote() << "makeStubs:" << id << "is still loaded";
                return false;
            }
        }
        return true;
    }
    /// Loaded, hidden rows that are pinned.
    QStringList heldHidden() const
    {
        QStringList ids;
        for (int r = 0; r < m_model->rowCount(); ++r) {
            const SessionRow &sr = std::as_const(*m_model).rowAt(r);
            if (sr.isLoaded() && !sr.visible && m_model->isSessionPinned(sr.sessionId))
                ids.append(sr.sessionId);
        }
        return ids;
    }
    /// What breaks the bound on holds right now; empty when nothing does.
    /// `pinnedHidden`: also every pinned hidden row is a hold or the session
    /// of an active job (not at every moment of a job's end: the executor
    /// unpins after jobFinished).
    QString holdViolation(bool pinnedHidden) const
    {
        if (!m_demand || !m_model || !m_queue)
            return QString();
        const QStringList held = m_demand->heldSessionIds();
        if (held.size() > CalculationDemand::kMaxHeldSessions)
            return QStringLiteral("held: ") + held.join(QLatin1Char(','));
        for (const QString &id : held) {
            if (!m_model->isSessionPinned(id))
                return id + QStringLiteral(" held but not pinned");
        }
        if (!pinnedHidden)
            return QString();
        QSet<QString> allowed(held.cbegin(), held.cend());
        for (const JobId id : m_queue->activeJobs())
            allowed.insert(m_queue->job(id).sessionId);
        for (const QString &id : heldHidden()) {
            if (!allowed.contains(id))
                return id + QStringLiteral(" pinned and hidden without a hold or an active job");
        }
        return QString();
    }
    /// Checks holdViolation() on every load, job start, jobsChanged and column
    /// state change while `scope` lives; the first violation lands in `violation`.
    void watchHolds(QObject *scope, QString *violation)
    {
        const auto check = [this, violation](bool pinnedHidden) {
            if (violation->isEmpty())
                *violation = holdViolation(pinnedHidden);
        };
        connect(m_model.get(), &SessionModel::sessionLoaded, scope, [check](const QString &) { check(true); });
        connect(m_queue.get(), &JobQueue::jobStarted, scope, [check](JobId) { check(true); });
        connect(m_queue.get(), &JobQueue::jobsChanged, scope, [check] { check(false); });
        connect(m_demand.get(), &CalculationDemand::columnStateChanged, scope, [check](const QString &) { check(false); });
    }
    /// The logbook's cached value of (session, column over `key`) as the row holds it.
    QVariant cachedValue(const QString &id, const char *key) const
    {
        return rowState(id).cachedValues.value(section(key));
    }
    bool isCachedUnavailable(const QString &id, const char *key) const
    {
        return rowState(id).cachedValues.contains(section(key)) && !cachedValue(id, key).isValid();
    }
    /// The number of sessionLoaded emissions of `id` in `spy`.
    static int loadsOf(const QSignalSpy &spy, const char *id)
    {
        int count = 0;
        for (const QList<QVariant> &arguments : spy) {
            if (arguments.at(0).toString() == QLatin1String(id))
                ++count;
        }
        return count;
    }
    /// A simulated application restart: the index flushed, then new logbook
    /// state, a new model of stubs from the index, a new executor and a new
    /// demand layer over the same plot model. Runs no event-loop pass.
    void restartApplication()
    {
        if (m_model)
            m_model->flushDirtySessions();
        m_demand.reset();
        if (m_queue)
            m_queue->shutdown();
        m_queue.reset();
        m_model.reset();
        LogbookManager &logbook = LogbookManager::instance();
        TestEnvironment::instance().reopenLogbook();
        logbook.initialize();
        m_model = std::make_unique<SessionModel>();
        m_model->populateFromIndex(logbook.cachedColumnValues(LogbookColumnStore::instance().enabledColumns()),
                                   logbook.lastAccessedMap());
        m_queue = std::make_unique<JobQueue>(m_model.get());
        m_demand = std::make_unique<CalculationDemand>(m_model.get(), m_plots.get(), m_queue.get());
    }
    /// Whether the session file of `id` on disk has the line.
    static bool fileHas(const QString &id, const QByteArray &line)
    {
        QFile file(sessionFilePath(id));
        return file.open(QIODevice::ReadOnly) && file.readAll().contains(line);
    }

    std::unique_ptr<JobWorld> m_world;
    std::unique_ptr<PlotFixture> m_fixture;
    std::unique_ptr<ExtraRegistrations> m_extra;    // a test's own calculations
    int m_capacity = 50;                            // LogbookCacheSize that makeStubs() leaves
    std::unique_ptr<SessionModel> m_model;
    std::unique_ptr<JobQueue> m_queue;
    std::unique_ptr<PlotModel> m_plots;
    std::unique_ptr<CalculationDemand> m_demand;
    QStringList m_registryBefore;
};

void CalculationDemandTest::initTestCase()
{
    TestEnvironment::instance().registerBuiltIns();

    // One logbook column that reads stored data only (see tst_jobqueue)
    PreferencesManager::instance().registerPreference(PreferenceKeys::LogbookColumnsVersion, 0);
    LogbookColumnStore::instance().setColumns({descriptionColumn()});
}

// Four loaded, hidden, saved sessions "s1".."s4" named "Jump 1".."Jump 4" (so
// that published results are stored). None has an input of any synthetic
// calculation: a test adds them. No plot is checked, no session is focused.
void CalculationDemandTest::init()
{
    TestEnvironment &env = TestEnvironment::instance();
    env.useFreshLogbook();
    env.resetPreferencesToDefaults();
    LogbookManager::instance().initialize();
    m_registryBefore = CalculationRegistry::instance().registeredIds();

    m_world = std::make_unique<JobWorld>();
    m_fixture = std::make_unique<PlotFixture>();
    m_extra = std::make_unique<ExtraRegistrations>();
    m_capacity = 50;
    m_model = std::make_unique<SessionModel>();
    m_model->mergeSessions(JobWorld::sessions({"s1", "s2", "s3", "s4"}));
    QCOMPARE(m_model->rowCount(), 4);
    QVERIFY(waitForIdle(*m_model));
    for (int i = 1; i <= 4; ++i) {
        m_model->updateAttribute(QStringLiteral("s%1").arg(i), QString::fromLatin1(SessionKeys::Description),
                                 QStringLiteral("Jump %1").arg(i));
    }
    m_model->flushPendingInvalidations();

    m_queue = std::make_unique<JobQueue>(m_model.get());
    m_plots = std::make_unique<PlotModel>();
    m_plots->setPlots(PlotFixture::plots());
    m_demand = std::make_unique<CalculationDemand>(m_model.get(), m_plots.get(), m_queue.get());
}

// Note what is to be checked, tear everything down, and only then check: a
// failing check returns from cleanup(), and whatever were still alive then
// would be alive under the next init() (see tst_jobqueue). The demand layer
// goes before the executor, as in the application, and before the pin check:
// it releases its holds when destroyed, so the check covers both releases.
void CalculationDemandTest::cleanup()
{
    if (m_queue)
        m_queue->shutdown();        // let nothing linger inside a compute function
    m_demand.reset();
    QStringList stillPinned;
    if (m_model) {
        for (const char *id : {"s1", "s2", "s3", "s4", "s5"}) {
            if (m_model->isSessionPinned(id))
                stillPinned.append(QString::fromLatin1(id));
        }
    }

    m_plots.reset();
    m_queue.reset();
    m_model.reset();
    m_extra.reset();
    // The column store and the cache capacity are process-wide
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

    QCOMPARE(stillPinned, QStringList());
    // The fixture removed exactly what it added
    QCOMPARE(afterFixture, withoutFixture);
    QCOMPARE(CalculationRegistry::instance().registeredIds(), m_registryBefore);
    QCOMPARE(CalculationRegistry::instance().enrolledEngineCount(), 0);
}

// ---- Vocabulary ---------------------------------------------------------------------

void CalculationDemandTest::plotIdMatchesPlotModelRole()
{
    int plots = 0;
    for (int c = 0; c < m_plots->rowCount(); ++c) {
        const QModelIndex category = m_plots->index(c, 0);
        for (int p = 0; p < m_plots->rowCount(category); ++p) {
            const QModelIndex plot = m_plots->index(p, 0, category);
            QCOMPARE(CalculationDemand::plotId(plot.data(PlotModel::SensorIDRole).toString(),
                                               plot.data(PlotModel::MeasurementIDRole).toString()),
                     plot.data(PlotModel::PlotValueIdRole).toString());
            ++plots;
        }
    }
    QCOMPARE(plots, 8);
    QCOMPARE(PlotFixture::plots().size(), 8);

    PlotValue value;
    value.sensorID = QStringLiteral("Syn");
    value.measurementID = QStringLiteral("g");
    QCOMPARE(CalculationDemand::plotId(value), QStringLiteral("Syn/g"));
}

// ---- Which plots are inspected ------------------------------------------------------------

// A plot over a stored attribute is not requested: it is never inspected
// (inspecting Syn/plain would run the plotPlain bridge), never announced, and
// it creates no demand.
void CalculationDemandTest::ordinaryPlotsAreNeverInspected()
{
    QVERIFY(giveInput({"s1", "s2", "s3"}, "P_IN", 3));
    QVERIFY(giveInput({"s1", "s2", "s3"}, "G_IN", 4));
    show({"s1", "s2", "s3"});

    QSignalSpy changedSpy(m_demand.get(), &CalculationDemand::plotStateChanged);
    QSignalSpy anySpy(m_demand.get(), &CalculationDemand::statesChanged);
    const Quiet quiet(*m_queue);
    const int runsBefore = totalRuns();

    check("plain");
    for (int pass = 0; pass < 5; ++pass) {
        show({"s1"}, pass % 2 == 1);        // every change of visibility schedules a pass
        QVERIFY(m_demand->hasPendingUpdate());
        m_demand->flush();
    }
    spin();
    QVERIFY(m_demand->passCount() >= 5);
    QCOMPARE(totalRuns(), runsBefore);
    QCOMPARE(changedSpy.count(), 0);
    QCOMPARE(anySpy.count(), 0);

    const DemandState state = row("Syn/plain");
    QVERIFY(state == DemandState());
    QVERIFY(!state.requested);
    QVERIFY(state.isPlain());
    QVERIFY(quiet.holds());
    QVERIFY(m_queue->isIdle());

    // The plot itself still reads normally
    QCOMPARE(values("s2", "plain"), QVector<double>({3.0}));
}

// An unchecked plot is not inspected either, requested or not: with G_OUT
// published, inspecting Syn/g would run the plotG bridge.
void CalculationDemandTest::uncheckedPlotsAreNeverInspected()
{
    QVERIFY(giveInput({"s1"}, "G_IN", 4));
    show({"s1", "s2"});
    gate().open(1);
    QCOMPARE(engine("s1").request(QStringLiteral("gated")).status, ResultStatus::Ok);
    QVERIFY(gate().waitEntered());

    QSignalSpy changedSpy(m_demand.get(), &CalculationDemand::plotStateChanged);
    const Quiet quiet(*m_queue);
    const int runsBefore = totalRuns();
    for (int pass = 0; pass < 5; ++pass) {
        show({"s2"}, pass % 2 == 1);
        m_demand->flush();
    }
    spin();
    QCOMPARE(totalRuns(), runsBefore);
    QCOMPARE(changedSpy.count(), 0);
    QVERIFY(row("Syn/g") == DemandState());
    QVERIFY(row("No/such") == DemandState());
    QVERIFY(quiet.holds());
}

// Tracks are the rows the plot widget draws. A hidden session and a stub are
// not tracks, and a pass never loads a session.
void CalculationDemandTest::hiddenAndStubRowsAreNotTracks()
{
    QVERIFY(giveInput({"s1", "s2", "s3"}, "G_IN", 4));
    QVERIFY(waitForIdle(*m_model));     // clean rows: eviction has nothing to save
    show({"s3", "s4"});
    session("s1");
    session("s2");                      // s1 is the least recently used hidden row

    const auto restoreCapacity = qScopeGuard([] {
        PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 50);
    });
    PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 1);
    QVERIFY(!isLoaded("s1"));
    QVERIFY(isLoaded("s2"));
    QVERIFY(waitForIdle(*m_model));

    QSignalSpy loadedSpy(m_model.get(), &SessionModel::sessionLoaded);
    check("g");
    QVERIFY(gate().waitEntered());      // s3 is the only wanted track: s4 has no input
    DemandState state = row("Syn/g");
    QCOMPARE(sessionIdsOf(state.running), QStringList({"s3"}));
    QCOMPARE(state.wantedCount, 1);
    QCOMPARE(state.waitingCount, 0);
    QCOMPARE(m_queue->model()->rowCount(), 1);

    // More passes, none of which touches the LRU
    const int passes = m_demand->passCount();
    for (int i = 0; i < 4; ++i) {
        check("plain", i % 2 == 0);
        state = row("Syn/g");
    }
    QCOMPARE(m_demand->passCount(), passes + 4);
    QCOMPARE(state.wantedCount, 1);
    QCOMPARE(loadedSpy.count(), 0);
    QVERIFY(!isLoaded("s1"));
    QVERIFY(isLoaded("s2"));            // had a pass touched the LRU, s2 would be gone
    QCOMPARE(m_queue->model()->rowCount(), 1);

    gate().open(1);
    QVERIFY(waitDemandIdle());
    QVERIFY(row("Syn/g").isPlain());
    QCOMPARE(loadedSpy.count(), 0);
    QVERIFY(!isLoaded("s1"));
}

// A row whose session file could not be loaded holds an empty placeholder
// (loadFailed). It is loaded and may be visible, but it is not a track: it is
// in no count, and nothing is offered for it.
void CalculationDemandTest::failedLoadPlaceholderIsNotATrack()
{
    QVERIFY(giveInput({"s1", "s2"}, "G_IN", 4));
    QVERIFY(waitForIdle(*m_model));     // clean rows: eviction has nothing to save
    session("s1");
    session("s2");                      // s1 is the least recently used hidden row
    {
        const auto restoreCapacity = qScopeGuard([] {
            PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 50);
        });
        PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 1);
        QVERIFY(!isLoaded("s1"));
    }

    // The stub's file is damaged; whoever looks at the session gets the placeholder
    const QString csvPath = sessionFilePath(QStringLiteral("s1"));
    QVERIFY(!csvPath.isEmpty());
    QVERIFY(writeFile(csvPath, "corrupt"));
    const int s1Row = m_model->getSessionRow("s1");
    QVERIFY(m_model->sessionRef(s1Row).attributeKeys().isEmpty());
    QVERIFY(std::as_const(*m_model).rowAt(s1Row).isLoaded());
    QVERIFY(std::as_const(*m_model).rowAt(s1Row).loadFailed);

    // An empty placeholder has no input, so inspecting it would classify it
    // as not applicable anyway; what shows that it is not inspected at all is
    // that no engine is ever created for it.
    const int enginesBefore = CalculationRegistry::instance().enrolledEngineCount();
    show({"s1", "s2"});
    QVERIFY(std::as_const(*m_model).rowAt(s1Row).visible);
    check("g");
    QVERIFY(gate().waitEntered());
    DemandState state = row("Syn/g");
    QCOMPARE(sessionIdsOf(state.running), QStringList({"s2"}));
    QCOMPARE(state.wantedCount, 1);
    QCOMPARE(state.failedCount, 0);
    QCOMPARE(CalculationRegistry::instance().enrolledEngineCount(), enginesBefore);
    QCOMPARE(m_queue->model()->rowCount(), 1);
    QCOMPARE(jobOf("s1", "gated").id, JobId(0));

    gate().open(1);
    QVERIFY(waitDemandIdle());
    QVERIFY(row("Syn/g").isPlain());
    QCOMPARE(m_queue->model()->rowCount(), 1);
    QVERIFY(std::as_const(*m_model).rowAt(s1Row).loadFailed);
}

// ---- Checking, showing, unchecking, hiding (spec section 13) -------------------------------------

void CalculationDemandTest::rowScript()
{
    const QString plot = QStringLiteral("Syn/g");
    QVERIFY(giveInput({"s1", "s2", "s3", "s4"}, "G_IN", 4));

    // 1. Three visible tracks; checking the plot starts the first with no other
    //    call, and the second is the chosen next job behind it
    show({"s1", "s2", "s3"});
    check("g");
    QVERIFY(gate().waitEntered());
    const JobId job1 = jobOf("s1", "gated").id;
    QVERIFY(job1 != 0);
    QCOMPARE(stateOf(job1), JobState::Running);
    QTRY_COMPARE(m_queue->job(job1).progressText, QStringLiteral("step 1"));
    DemandState state = row("Syn/g");
    const JobId job2 = jobOf("s2", "gated").id;
    QVERIFY(job2 != 0);
    QCOMPARE(m_queue->chosenNextJob(), job2);
    QCOMPARE(m_queue->activeJobs().size(), 2);
    QVERIFY(state.requested);
    QCOMPARE(state.sourceId, plot);
    QCOMPARE(state.wantedCount, 3);
    QCOMPARE(state.doneCount, 0);
    QCOMPARE(state.runningCount, 1);
    QCOMPARE(state.waitingCount, 2);
    QCOMPARE(state.failedCount, 0);
    QVERIFY(state.isWorking());
    QVERIFY(!state.showsWarning());
    QCOMPARE(sessionIdsOf(state.running), QStringList({"s1"}));
    QCOMPARE(sessionIdsOf(state.waiting), QStringList({"s2", "s3"}));
    QCOMPARE(state.running.at(0).job, job1);
    QCOMPARE(state.running.at(0).calculationTitles, QStringList({"Gated"}));
    QCOMPARE(state.waiting.at(0).job, job2);            // the chosen next job is s2's
    QCOMPARE(state.waiting.at(1).job, JobId(0));
    QVERIFY(!state.waiting.at(0).settling);
    QCOMPARE(state.progressLabel, QStringLiteral("0 of 3"));
    QCOMPARE(state.toolTip, QStringLiteral("Computing: 0 of 3 done\n"
                                           "  Jump 1 - Gated: step 1"));

    // 2. Each end starts the next
    gate().open(1);
    QTRY_COMPARE(stateOf(job1), JobState::Succeeded);
    QVERIFY(stored("s1", "gated"));
    QVERIFY(gate().waitEntered());
    QCOMPARE(stateOf(job2), JobState::Running);
    state = row("Syn/g");
    const JobId job3 = jobOf("s3", "gated").id;
    QVERIFY(job3 != 0);
    QCOMPARE(m_queue->chosenNextJob(), job3);
    QCOMPARE(state.doneCount, 1);
    QCOMPARE(state.wantedCount, 3);
    QCOMPARE(state.progressLabel, QStringLiteral("1 of 3"));
    QCOMPARE(values("s1", "g"), QVector<double>({5.0}));

    // 3. Unchecking drops the waiting pair at once and lets the running job finish
    check("g", false);
    QCOMPARE(stateOf(job3), JobState::Cancelled);
    QCOMPARE(m_queue->job(job3).reason, QString::fromLatin1(kNoLongerNeeded));
    QVERIFY(!m_queue->job(job3).startedAt.isValid());
    QCOMPARE(stateOf(job2), JobState::Running);
    QVERIFY(!m_queue->job(job2).cancelRequested);
    QVERIFY(row("Syn/g") == DemandState());

    gate().open(1);
    QVERIFY(waitDemandIdle());
    QCOMPARE(stateOf(job2), JobState::Succeeded);
    QVERIFY(stored("s2", "gated"));
    QCOMPARE(values("s2", "g"), QVector<double>({5.0}));
    QCOMPARE(m_queue->model()->rowCount(), 3);

    // 4. Checked again: only s3 is missing, and it starts with no other call
    check("g");
    QVERIFY(gate().waitEntered());
    const JobId job4 = jobOf("s3", "gated").id;
    QVERIFY(job4 != job3);
    QCOMPARE(stateOf(job4), JobState::Running);
    state = row("Syn/g");
    QCOMPARE(state.progressLabel, QStringLiteral("2 of 3"));

    // 5. A fourth track shown while s3 runs becomes the chosen next job
    show({"s4"});
    state = row("Syn/g");
    const JobId job5 = jobOf("s4", "gated").id;
    QVERIFY(job5 != 0);
    QCOMPARE(m_queue->chosenNextJob(), job5);
    QCOMPARE(state.wantedCount, 4);
    QCOMPARE(state.progressLabel, QStringLiteral("2 of 4"));
    QCOMPARE(state.waiting.at(0).sessionName, QStringLiteral("Jump 4"));
    gate().open(2);
    QVERIFY(waitDemandIdle());
    state = row("Syn/g");
    QVERIFY(state.isPlain());
    QVERIFY(state.toolTip.isEmpty());
    QVERIFY(state.progressLabel.isEmpty());
    QCOMPARE(state.doneCount, 4);
    QCOMPARE(values("s4", "g"), QVector<double>({5.0}));

    // 6. The history
    QCOMPARE(history(), QList<JobState>({JobState::Succeeded, JobState::Succeeded, JobState::Cancelled,
                                         JobState::Succeeded, JobState::Succeeded}));
    QCOMPARE(gate().maxRunning.load(), 1);
}

// ---- Chained blockers ---------------------------------------------------------------------------

void CalculationDemandTest::chainedBlockersContinue()
{
    const QString plot = QStringLiteral("Syn/db");
    QVERIFY(giveInput({"s1"}, "EA_IN", 4));
    QVERIFY(giveInput({"s1"}, "EB_IN", 10));
    show({"s1"});

    QSignalSpy idleSpy(m_queue.get(), &JobQueue::idle);
    // Every announced state until the last is working
    QList<DemandState> announced;
    QObject scope;      // owns the connection: it cannot outlive `announced`
    connect(m_demand.get(), &CalculationDemand::plotStateChanged, &scope, [&](const QString &id) {
        if (id == plot)
            announced.append(m_demand->plotState(id));
    });

    check("db");
    QVERIFY(waitDemandIdle());
    const DemandState state = row("Syn/db");

    const JobModel *jobs = m_queue->model();
    QCOMPARE(jobs->rowCount(), 2);
    QCOMPARE(jobs->record(0).calculationId, QStringLiteral("expA"));
    QCOMPARE(jobs->record(0).state, JobState::Succeeded);
    QCOMPARE(jobs->record(1).calculationId, QStringLiteral("expB"));
    QCOMPARE(jobs->record(1).state, JobState::Succeeded);
    // The executor was never idle between the links
    QCOMPARE(idleSpy.count(), 1);

    QCOMPARE(values("s1", "db"), QVector<double>({19.0}));
    QCOMPARE(engine("s1").runCount("expA"), 1);
    QCOMPARE(engine("s1").runCount("expB"), 1);
    QVERIFY(state.isPlain());
    QVERIFY(!m_demand->isSettling("s1"));       // a publication is not an input change

    QVERIFY(announced.size() >= 2);
    for (int i = 0; i < announced.size() - 1; ++i) {
        QVERIFY(announced.at(i).isWorking());
        QCOMPARE(announced.at(i).wantedCount, 1);
        QCOMPARE(announced.at(i).progressLabel, QStringLiteral("0 of 1"));
    }
    QVERIFY(announced.last().isPlain());
}

void CalculationDemandTest::heldChainContinues()
{
    QVERIFY(giveInput({"s1"}, "G_IN", 4));
    show({"s1"});
    QSignalSpy idleSpy(m_queue.get(), &JobQueue::idle);

    check("h");
    QVERIFY(gate().waitEntered());
    DemandState state = row("Syn/h");
    QCOMPARE(state.runningCount, 1);
    QCOMPARE(state.running.at(0).calculationTitles, QStringList({"Gated"}));
    QCOMPARE(state.running.at(0).condition, DemandCondition::Running);
    QCOMPARE(state.running.at(0).job, jobOf("s1", "gated").id);
    QCOMPARE(jobCount("afterG"), 0);

    gate().open(1);
    QVERIFY(waitDemandIdle());
    QCOMPARE(jobCount("gated"), 1);
    QCOMPARE(jobCount("afterG"), 1);
    QCOMPARE(jobOf("s1", "afterG").state, JobState::Succeeded);
    QCOMPARE(idleSpy.count(), 1);
    QCOMPARE(values("s1", "h"), QVector<double>({6.0}));
    QVERIFY(row("Syn/h").isPlain());
}

void CalculationDemandTest::chainCompletesAfterFirstJobDoesNotSucceed_data()
{
    QTest::addColumn<QString>("action");
    QTest::addColumn<int>("endState");
    QTest::addColumn<double>("finalValue");
    QTest::newRow("cancelled from outside") << "executorCancel" << int(JobState::Cancelled) << 6.0;
    QTest::newRow("superseded") << "editInput" << int(JobState::Superseded) << 9.0;
}

// A first link that ends without success is still in demand: the chain
// completes with no other call.
void CalculationDemandTest::chainCompletesAfterFirstJobDoesNotSucceed()
{
    QFETCH(QString, action);
    QFETCH(int, endState);
    QFETCH(double, finalValue);

    QVERIFY(giveInput({"s1"}, "G_IN", 4));
    show({"s1"});
    check("h");
    QVERIFY(gate().waitEntered());
    const JobId first = jobOf("s1", "gated").id;

    if (action == QLatin1String("executorCancel")) {
        // Still in demand: offered again at once, as a new job behind the one
        // that winds down
        QVERIFY(m_queue->cancel(first));
        const DemandState state = row("Syn/h");
        QCOMPARE(state.runningCount, 0);
        QCOMPARE(state.waitingCount, 1);
        const JobId again = m_queue->chosenNextJob();
        QVERIFY(again != 0 && again != first);
        QTRY_COMPARE(int(stateOf(first)), endState);
        QVERIFY(gate().waitEntered());
        QCOMPARE(stateOf(again), JobState::Running);
    } else {
        m_demand->setInputSettleDelay(60000);
        QVERIFY(giveInput({"s1"}, "G_IN", 7));
        // Still running, but the engine has marked the ticket and the executor
        // has asked the job to stop: the track waits for its inputs to settle
        QCOMPARE(stateOf(first), JobState::Running);
        const DemandState state = row("Syn/h");
        QCOMPARE(state.runningCount, 0);
        QCOMPARE(state.waitingCount, 1);
        QVERIFY(state.waiting.at(0).settling);
        QTRY_COMPARE(int(stateOf(first)), endState);
        spin();
        QCOMPARE(m_queue->model()->rowCount(), 1);      // nothing offered during the wait
        settle();
        QVERIFY(gate().waitEntered());
    }

    gate().open(1);
    QVERIFY(waitDemandIdle());
    QCOMPARE(jobCount("gated"), 2);
    QCOMPARE(jobCount("afterG"), 1);
    QCOMPARE(values("s1", "h"), QVector<double>({finalValue}));
    QVERIFY(row("Syn/h").isPlain());
}

void CalculationDemandTest::chainStopsForHiddenTrackOrUncheckedPlot_data()
{
    QTest::addColumn<bool>("hide");
    QTest::newRow("track hidden") << true;
    QTest::newRow("plot unchecked") << false;
}

// The first link finishes and is stored; the rest of the chain is not started
// while nothing wants it, and starts by itself once something does again.
void CalculationDemandTest::chainStopsForHiddenTrackOrUncheckedPlot()
{
    QFETCH(bool, hide);

    QVERIFY(giveInput({"s1"}, "G_IN", 4));
    show({"s1"});
    check("h");
    QVERIFY(gate().waitEntered());
    const JobId first = jobOf("s1", "gated").id;

    if (hide)
        show({"s1"}, false);
    else
        check("h", false);
    QCOMPARE(stateOf(first), JobState::Running);        // the running job is left to finish
    QVERIFY(!m_queue->job(first).cancelRequested);

    gate().open(1);
    QVERIFY(waitDemandIdle());
    QCOMPARE(stateOf(first), JobState::Succeeded);
    QVERIFY(stored("s1", "gated"));
    QCOMPARE(session("s1").getAttribute("G_OUT"), QVariant(5));
    spin();
    QCOMPARE(jobCount("afterG"), 0);

    // Wanted again: the rest of the chain starts with no other action
    if (hide)
        show({"s1"});
    else
        check("h");
    QVERIFY(waitDemandIdle());
    QCOMPARE(jobCount("afterG"), 1);
    QCOMPARE(jobOf("s1", "afterG").state, JobState::Succeeded);
    QCOMPARE(jobCount("gated"), 1);
    QCOMPARE(values("s1", "h"), QVector<double>({6.0}));
    QVERIFY(row("Syn/h").isPlain());
}

// ---- Every way of checking is the same (item 116, as amended) --------------------------------------

// setPlotEnabled (profiles), togglePlot (the Plots menu and its shortcuts) and
// setData(CheckStateRole) (what the view's check box writes) create the same
// demand: the visible sessions without a result are started.
void CalculationDemandTest::programmaticCheckCreatesDemand()
{
    QVERIFY(giveInput({"s1", "s2", "s3"}, "G_IN", 4));
    show({"s1", "s2", "s3"});

    QModelIndex index;
    const QModelIndex category = m_plots->index(0, 0);
    for (int p = 0; p < m_plots->rowCount(category); ++p) {
        if (m_plots->index(p, 0, category).data(PlotModel::PlotValueIdRole).toString() == QLatin1String("Syn/g"))
            index = m_plots->index(p, 0, category);
    }
    QVERIFY(index.isValid());

    const QStringList paths = {"setPlotEnabled", "togglePlot", "setData"};
    for (int i = 0; i < paths.size(); ++i) {
        const QString path = paths.at(i);
        const auto setChecked = [&](bool checked) {
            if (path == QLatin1String("setPlotEnabled"))
                check("g", checked);
            else if (path == QLatin1String("togglePlot"))
                m_plots->togglePlot("Syn", "g");
            else
                m_plots->setData(index, checked ? Qt::Checked : Qt::Unchecked, Qt::CheckStateRole);
        };

        setChecked(true);
        QVERIFY2(m_plots->isPlotEnabled("Syn", "g"), qPrintable(path));
        QVERIFY2(gate().waitEntered(), qPrintable(path));
        m_demand->flush();
        QCOMPARE(running().sessionId, QStringLiteral("s1"));
        const JobRecord next = chosenNext();
        QCOMPARE(next.sessionId, QStringLiteral("s2"));
        QCOMPARE(row("Syn/g").wantedCount, 3);

        // Unchecking in between drops the waiting pair
        setChecked(false);
        QVERIFY2(!m_plots->isPlotEnabled("Syn", "g"), qPrintable(path));
        QCOMPARE(stateOf(next.id), JobState::Cancelled);
        QCOMPARE(m_queue->job(next.id).reason, QString::fromLatin1(kNoLongerNeeded));
        QVERIFY(row("Syn/g") == DemandState());

        gate().open(1);
        QVERIFY(waitDemandIdle());
        QCOMPARE(jobOf("s1", "gated").state, JobState::Succeeded);

        // A new input for the next round; the plot is not checked, so the edit
        // is not an input change of a demanded session
        QVERIFY(giveInput({"s1", "s2", "s3"}, "G_IN", 10 + i));
        QVERIFY(!m_demand->hasSettlingSessions());
    }
    QCOMPARE(jobCount("gated"), 6);
    QCOMPARE(gate().maxRunning.load(), 1);
}

// The loop of applyProfile(): setPlotEnabled over all plots.
void CalculationDemandTest::profileStyleApplyCreatesDemand()
{
    QVERIFY(giveInput({"s1", "s2"}, "G_IN", 4));
    QVERIFY(giveInput({"s1", "s2"}, "EA_IN", 4));
    QVERIFY(giveInput({"s1", "s2"}, "EB_IN", 10));
    show({"s1", "s2"});
    gate().open(2);

    const QSet<QString> profile = {"Syn/g", "Syn/g2", "Syn/db", "Syn/h", "Syn/plain"};
    const QVector<PlotValue> plots = PlotFixture::plots();
    for (const PlotValue &plot : plots)
        m_plots->setPlotEnabled(plot.sensorID, plot.measurementID, profile.contains(CalculationDemand::plotId(plot)));
    QVERIFY(waitDemandIdle());

    for (const char *id : {"Syn/g", "Syn/g2", "Syn/db", "Syn/h"}) {
        const DemandState state = row(id);
        QVERIFY2(state.requested, id);
        QVERIFY2(state.isPlain(), id);
        QCOMPARE(state.wantedCount, 2);
        QCOMPARE(state.doneCount, 2);
    }
    QVERIFY(row("Syn/plain") == DemandState());
    QVERIFY(row("Syn/ea") == DemandState());       // unchecked by the profile
    for (const char *id : {"s1", "s2"}) {
        QCOMPARE(values(id, "g"), QVector<double>({5.0}));
        QCOMPARE(values(id, "h"), QVector<double>({6.0}));
        QCOMPARE(values(id, "db"), QVector<double>({19.0}));
    }
    // Each pair ran once. The choice follows plot-model order within a session
    // (db before h), so a chosen next job of a later plot can be replaced by an
    // earlier plot's next link: it ends Cancelled without ever running.
    QHash<QString, int> succeeded;
    for (const JobRecord &record : m_queue->model()->records()) {
        if (record.state == JobState::Succeeded) {
            ++succeeded[record.sessionId + QLatin1Char('/') + record.calculationId];
        } else {
            QCOMPARE(record.state, JobState::Cancelled);
            QCOMPARE(record.reason, QString::fromLatin1(kNoLongerNeeded));
            QVERIFY(!record.startedAt.isValid());
        }
    }
    for (const char *id : {"s1", "s2"}) {
        for (const char *calculation : {"gated", "afterG", "expA", "expB"})
            QCOMPARE(succeeded.value(QString::fromLatin1(id) + QLatin1Char('/') + QString::fromLatin1(calculation)), 1);
    }
    QCOMPARE(succeeded.size(), 8);
}

// Plots restored as checked from the settings come up through modelReset. At
// start-up every session is hidden: no job, a plain row. Work starts when a
// session is shown.
void CalculationDemandTest::startupRestoreWithHiddenSessionsStartsNothing()
{
    QVERIFY(giveInput({"s1", "s2", "s3"}, "G_IN", 4));

    const QString path = TestEnvironment::instance().newTempDir(QStringLiteral("plots")) + QStringLiteral("/plots.ini");
    QSettings settings(path, QSettings::IniFormat);
    settings.setValue(QStringLiteral("state/plots/Syn/g"), true);
    // Whichever way this function is left, no PlotModel keeps pointing at `settings`
    const auto detachSettings = qScopeGuard([this] {
        if (m_plots)
            m_plots->setSettings(nullptr);
    });

    {
        const Quiet quiet(*m_queue);

        // As the application starts: the demand layer exists before the plots do
        m_demand.reset();
        m_plots = std::make_unique<PlotModel>();
        m_plots->setSettings(&settings);
        m_demand = std::make_unique<CalculationDemand>(m_model.get(), m_plots.get(), m_queue.get());
        m_plots->setPlots(PlotFixture::plots());
        QVERIFY(m_plots->isPlotEnabled("Syn", "g"));
        spin();

        DemandState state = row("Syn/g");
        QVERIFY(state.requested);
        QVERIFY(state.isPlain());
        QCOMPARE(state.wantedCount, 0);
        QVERIFY(quiet.holds());

        // ... and the other order: the plots are there when the layer is created
        m_demand.reset();
        m_demand = std::make_unique<CalculationDemand>(m_model.get(), m_plots.get(), m_queue.get());
        QVERIFY(m_demand->hasPendingUpdate());      // the initial pass needs no event
        spin();
        state = row("Syn/g");
        QVERIFY(state.isPlain());
        QVERIFY(quiet.holds());
        QVERIFY(m_queue->isIdle());
    }

    // Showing a session starts it with no other action
    show({"s1"});
    QVERIFY(gate().waitEntered());
    QCOMPARE(running().sessionId, QStringLiteral("s1"));
    gate().open(1);
    QVERIFY(waitDemandIdle());
    QCOMPARE(values("s1", "g"), QVector<double>({5.0}));
    QCOMPARE(m_queue->model()->rowCount(), 1);

    // The settings object must outlive the model that writes to it
    m_demand.reset();
    m_plots.reset();
}

void CalculationDemandTest::showingASessionStartsIt()
{
    QVERIFY(giveInput({"s1", "s2"}, "G_IN", 4));
    check("g");
    spin();
    QVERIFY(row("Syn/g").isPlain());        // no track at all
    QCOMPARE(m_queue->model()->rowCount(), 0);

    show({"s1"});
    QVERIFY(gate().waitEntered());
    QCOMPARE(running().sessionId, QStringLiteral("s1"));

    // Shown while s1 runs: the chosen next job, with no other call
    show({"s2"});
    m_demand->flush();
    QCOMPARE(chosenNext().sessionId, QStringLiteral("s2"));
    QCOMPARE(row("Syn/g").wantedCount, 2);

    gate().open(2);
    QVERIFY(waitDemandIdle());
    QCOMPARE(history(), QList<JobState>({JobState::Succeeded, JobState::Succeeded}));
    QVERIFY(stored("s1", "gated"));
    QVERIFY(stored("s2", "gated"));
    QVERIFY(row("Syn/g").isPlain());
}

void CalculationDemandTest::hidingASessionDropsItsWaitingPair()
{
    QVERIFY(giveInput({"s1", "s2"}, "G_IN", 4));
    show({"s1", "s2"});
    check("g");
    QVERIFY(gate().waitEntered());
    m_demand->flush();
    const JobId runningJob = jobOf("s1", "gated").id;
    const JobId waitingJob = jobOf("s2", "gated").id;
    QCOMPARE(m_queue->chosenNextJob(), waitingJob);

    // At once, before any turn of the event loop
    show({"s2"}, false);
    QCOMPARE(stateOf(waitingJob), JobState::Cancelled);
    QCOMPARE(m_queue->job(waitingJob).reason, QString::fromLatin1(kNoLongerNeeded));
    QVERIFY(!m_queue->job(waitingJob).startedAt.isValid());
    QVERIFY(!m_model->isSessionPinned("s2"));

    // Hiding the running job's session does not stop it
    show({"s1"}, false);
    QCOMPARE(stateOf(runningJob), JobState::Running);
    QVERIFY(!m_queue->job(runningJob).cancelRequested);
    QCOMPARE(m_queue->chosenNextJob(), JobId(0));

    gate().open(1);
    QVERIFY(waitDemandIdle());
    QCOMPARE(stateOf(runningJob), JobState::Succeeded);
    QVERIFY(stored("s1", "gated"));
    QCOMPARE(m_queue->model()->rowCount(), 2);
    QVERIFY(row("Syn/g").isPlain());
}

void CalculationDemandTest::uncheckingDropsWaitingPairsKeepsRunning()
{
    QVERIFY(giveInput({"s1", "s2", "s3"}, "G_IN", 4));
    show({"s1", "s2", "s3"});
    check("g");
    QVERIFY(gate().waitEntered());
    m_demand->flush();
    const JobId runningJob = jobOf("s1", "gated").id;
    const JobId waitingJob = m_queue->chosenNextJob();
    QCOMPARE(m_queue->job(waitingJob).sessionId, QStringLiteral("s2"));

    QVERIFY(!m_plots->togglePlot("Syn", "g"));
    QCOMPARE(stateOf(waitingJob), JobState::Cancelled);
    QCOMPARE(m_queue->job(waitingJob).reason, QString::fromLatin1(kNoLongerNeeded));
    QCOMPARE(stateOf(runningJob), JobState::Running);
    QVERIFY(!m_queue->job(runningJob).cancelRequested);
    QVERIFY(row("Syn/g") == DemandState());

    gate().open(1);
    QVERIFY(waitDemandIdle());
    QCOMPARE(stateOf(runningJob), JobState::Succeeded);
    QVERIFY(stored("s1", "gated"));
    QCOMPARE(values("s1", "g"), QVector<double>({5.0}));
    QCOMPARE(m_queue->model()->rowCount(), 2);
    QVERIFY(row("Syn/g") == DemandState());
}

void CalculationDemandTest::waitingPairNeededByAnotherPlotSurvives()
{
    QVERIFY(giveInput({"s1", "s2"}, "G_IN", 4));
    show({"s1", "s2"});
    check("g");
    check("g2");                            // needs the same jobs
    QVERIFY(gate().waitEntered());
    m_demand->flush();
    const JobId waitingJob = m_queue->chosenNextJob();
    QCOMPARE(m_queue->job(waitingJob).sessionId, QStringLiteral("s2"));

    check("g", false);
    QCOMPARE(stateOf(waitingJob), JobState::Queued);
    QCOMPARE(m_queue->chosenNextJob(), waitingJob);
    QCOMPARE(row("Syn/g2").waitingCount, 1);

    check("g2", false);                     // now nothing needs it
    QCOMPARE(stateOf(waitingJob), JobState::Cancelled);
    QCOMPARE(jobOf("s1", "gated").state, JobState::Running);

    gate().open(1);
    QVERIFY(waitDemandIdle());
    QCOMPARE(jobOf("s1", "gated").state, JobState::Succeeded);
}

// A visible stub becomes a track when the model loads it, and is started.
void CalculationDemandTest::loadingAVisibleSessionStartsIt()
{
    QVERIFY(giveInput({"s1", "s2"}, "G_IN", 4));
    QVERIFY(waitForIdle(*m_model));     // saved: the stub reloads with its input
    show({"s2"});
    session("s1");
    session("s3");
    session("s4");                      // s1 is the least recently used hidden row

    const auto restoreCapacity = qScopeGuard([] {
        PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 50);
    });
    PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 1);
    QVERIFY(!isLoaded("s1"));
    QVERIFY(waitForIdle(*m_model));

    gate().open(1);
    check("g");
    QVERIFY(waitDemandIdle());
    QVERIFY(gate().waitEntered());          // s2's run, which the open gate let through
    QCOMPARE(sessionIdsOf(row("Syn/g").running), QStringList());
    QCOMPARE(jobOf("s2", "gated").state, JobState::Succeeded);
    QCOMPARE(m_queue->model()->rowCount(), 1);

    QSignalSpy loadedSpy(m_model.get(), &SessionModel::sessionLoaded);
    show({"s1"});                       // the model loads the stub
    QVERIFY(isLoaded("s1"));
    QVERIFY(loadedSpy.count() >= 1);
    QVERIFY(gate().waitEntered());
    QCOMPARE(running().sessionId, QStringLiteral("s1"));
    gate().open(1);
    QVERIFY(waitDemandIdle());
    QCOMPARE(values("s1", "g"), QVector<double>({5.0}));
    QVERIFY(row("Syn/g").isPlain());
}

// A merge into a session whose names are all irrelevant to the checked plots
// starts no settle wait; a new session, shown, is started.
void CalculationDemandTest::mergeCreatesDemandForShownSessions()
{
    QVERIFY(giveInput({"s1"}, "G_IN", 4));
    check("g");
    m_demand->flush();

    // Into an existing session
    SessionData sensorOnly = DescentFixture::loadSensorOnly("s1");
    sensorOnly.setMeasurement("IMU", "wx", {30.0, 60.0, 90.0});
    const QList<MergeResult> merged = m_model->mergeSessions({sensorOnly});
    QCOMPARE(merged.size(), 1);
    QCOMPARE(merged.at(0).outcome, MergeResult::Outcome::Merged);
    QVERIFY(!m_demand->isSettling("s1"));
    QVERIFY(!m_demand->hasSettlingSessions());

    // A new session, with its input in its own data
    const QList<MergeResult> created = m_model->mergeSessions(newSessionWithInput("s5"));
    QCOMPARE(created.at(0).outcome, MergeResult::Outcome::Created);
    QVERIFY(!m_demand->hasSettlingSessions());

    show({"s1", "s5"});
    QVERIFY(gate().waitEntered());
    QCOMPARE(running().sessionId, QStringLiteral("s1"));      // no wait for the merge
    m_demand->flush();
    QCOMPARE(chosenNext().sessionId, QStringLiteral("s5"));
    gate().open(2);
    QVERIFY(waitDemandIdle());
    QCOMPARE(values("s1", "g"), QVector<double>({5.0}));
    QCOMPARE(values("s5", "g"), QVector<double>({5.0}));
    QVERIFY(row("Syn/g").isPlain());
    QVERIFY(!m_model->isSessionPinned("s5"));
}

// ---- The input-settle wait ---------------------------------------------------------------------------

void CalculationDemandTest::inputBurstRunsOneJob()
{
    QCOMPARE(CalculationDemand::kInputSettleMs, 1000);
    QCOMPARE(m_demand->inputSettleDelay(), CalculationDemand::kInputSettleMs);

    QVERIFY(giveInput({"s1"}, "G_IN", 4));
    show({"s1"});
    gate().open(1);
    check("g");
    QVERIFY(waitDemandIdle());
    QCOMPARE(jobCount("gated"), 1);
    QVERIFY(row("Syn/g").isPlain());

    m_demand->setInputSettleDelay(60000);
    {
        const Quiet quiet(*m_queue);
        QVERIFY(giveInput({"s1"}, "G_IN", 5));
        // In demand from the first change: working, but not started
        QVERIFY(m_demand->isSettling("s1"));
        DemandState state = row("Syn/g");
        QVERIFY(state.isWorking());
        QCOMPARE(state.waitingCount, 1);
        QVERIFY(state.waiting.at(0).settling);
        QCOMPARE(state.waiting.at(0).job, JobId(0));
        QCOMPARE(state.progressLabel, QStringLiteral("0 of 1"));
        spin();
        QVERIFY(giveInput({"s1"}, "G_IN", 6));
        spin();
        QVERIFY(giveInput({"s1"}, "G_IN", 7));
        spin();
        state = row("Syn/g");
        QVERIFY(state.waiting.at(0).settling);
        QVERIFY(quiet.holds());
    }

    gate().open(1);
    settle();
    QVERIFY(waitDemandIdle());
    QCOMPARE(jobCount("gated"), 2);         // exactly one new job
    QCOMPARE(values("s1", "g"), QVector<double>({8.0}));
    QVERIFY(row("Syn/g").isPlain());
}

// The executor stops a running job whose inputs went stale. From that moment
// its track is Waiting (settling) - before the worker has returned. Nothing is
// offered until the inputs settle; then one job computes the new value.
void CalculationDemandTest::staleRunningJobIsWaitingAtOnce()
{
    QVERIFY(giveInput({"s1"}, "G_IN", 4));
    show({"s1"});
    check("g");
    check("g2");                            // a second row waiting on the same job
    QVERIFY(gate().waitEntered());
    const JobId stale = jobOf("s1", "gated").id;
    QCOMPARE(row("Syn/g").runningCount, 1);

    m_demand->setInputSettleDelay(60000);
    QVERIFY(giveInput({"s1"}, "G_IN", 7));

    // Before the worker has returned; the gate is never opened for this job
    QCOMPARE(stateOf(stale), JobState::Running);
    QVERIFY(m_queue->job(stale).cancelRequested);
    for (const char *id : {"Syn/g", "Syn/g2"}) {
        const DemandState state = row(id);
        QCOMPARE(state.runningCount, 0);
        QCOMPARE(state.waitingCount, 1);
        QVERIFY(state.waiting.at(0).settling);
        QVERIFY(state.isWorking());
    }
    QCOMPARE(stateOf(stale), JobState::Running);
    QCOMPARE(m_queue->model()->rowCount(), 1);      // nothing offered

    QTRY_COMPARE(stateOf(stale), JobState::Superseded);
    QCOMPARE(m_queue->job(stale).reason, QStringLiteral("Inputs changed"));
    spin();
    QCOMPARE(m_queue->model()->rowCount(), 1);

    gate().open(1);
    settle();
    QVERIFY(waitDemandIdle());
    const JobId again = jobOf("s1", "gated").id;
    QVERIFY(again != stale);
    QCOMPARE(stateOf(again), JobState::Succeeded);
    QVERIFY(row("Syn/g").isPlain());
    QVERIFY(row("Syn/g2").isPlain());
    QCOMPARE(values("s1", "g"), QVector<double>({8.0}));
    QCOMPARE(m_queue->model()->rowCount(), 2);
}

// The real timer: nothing is offered while the wait runs; once it has passed,
// the job runs with no other call. The wait that is checked for "nothing yet"
// is long, so that no delay of the test's own thread can end it early.
void CalculationDemandTest::supersededJobIsRunAgainAfterInputsSettle()
{
    QVERIFY(giveInput({"s1"}, "G_IN", 4));
    show({"s1"});
    check("g");
    QVERIFY(gate().waitEntered());
    const JobId job = jobOf("s1", "gated").id;

    m_demand->setInputSettleDelay(60000);
    QVERIFY(giveInput({"s1"}, "G_IN", 7));
    QTRY_COMPARE(stateOf(job), JobState::Superseded);
    spin();
    spin();
    QCOMPARE(m_queue->model()->rowCount(), 1);      // nothing during the wait
    QVERIFY(m_demand->isSettling("s1"));

    // The next change restarts the wait, with a short delay this time: the
    // long wait is replaced, the real timer ends the new one, and the job is
    // offered by itself
    m_demand->setInputSettleDelay(50);
    gate().open(1);
    QVERIFY(giveInput({"s1"}, "G_IN", 8));
    QTRY_COMPARE_WITH_TIMEOUT(m_queue->model()->rowCount(), 2, 30000);
    QVERIFY(!m_demand->isSettling("s1"));
    QVERIFY(waitDemandIdle());
    QCOMPARE(jobOf("s1", "gated").state, JobState::Succeeded);
    QCOMPARE(values("s1", "g"), QVector<double>({9.0}));
}

// ---- What is offered -----------------------------------------------------------------------------------

void CalculationDemandTest::sessionWithoutInputIsNeverListed()
{
    QVERIFY(giveInput({"s1", "s2"}, "G_IN", 4));      // s3 has no input: nothing to compute
    show({"s1", "s2", "s3"});

    const auto neverListed = [this](const char *plotId) {
        const DemandState state = row(plotId);
        return !sessionIdsOf(state.running).contains("s3") && !sessionIdsOf(state.waiting).contains("s3")
            && !sessionIdsOf(state.failed).contains("s3");
    };

    check("g");
    check("g2");
    QVERIFY(gate().waitEntered());
    QCOMPARE(row("Syn/g").wantedCount, 2);
    QVERIFY(neverListed("Syn/g"));
    QVERIFY(neverListed("Syn/g2"));

    gate().open(1);
    QVERIFY(gate().waitEntered());
    QVERIFY(neverListed("Syn/g"));
    gate().open(1);
    QVERIFY(waitDemandIdle());

    QVERIFY(row("Syn/g").isPlain());
    QVERIFY(row("Syn/g2").isPlain());
    QVERIFY(neverListed("Syn/g"));
    QCOMPARE(m_queue->model()->rowCount(), 2);
    QCOMPARE(jobOf("s3", "gated").id, JobId(0));

    // No job can be created for it
    QCOMPARE(m_queue->offer("s3", QStringLiteral("gated")).kind, Kind::MissingInput);
    QCOMPARE(jobOf("s3", "gated").id, JobId(0));
    QVERIFY(values("s3", "g").isEmpty());
}

// The engine only reports requestable blockers, and a pair with a result is
// not in demand: only s1 is offered.
void CalculationDemandTest::onlyRequestableCalculationsAreOffered()
{
    QVERIFY(giveInput({"s1", "s3"}, "EA_IN", 4));    // s1: requestable; s3: computed below
    QVERIFY(giveInput({"s4"}, "EA_IN", -1));         // rejected below; s2 has no input
    QCOMPARE(engine("s3").request(QStringLiteral("expA")).status, ResultStatus::Ok);
    QCOMPARE(engine("s4").request(QStringLiteral("expA")).status, ResultStatus::Ok);

    show({"s1", "s2", "s3", "s4"});
    QSignalSpy queuedSpy(m_queue.get(), &JobQueue::jobQueued);
    check("ea");
    QVERIFY(waitDemandIdle());
    QCOMPARE(queuedSpy.count(), 1);
    QCOMPARE(m_queue->model()->rowCount(), 1);
    QCOMPARE(jobOf("s1", "expA").state, JobState::Succeeded);

    const DemandState state = row("Syn/ea");
    QVERIFY(state.running.isEmpty());
    QVERIFY(state.waiting.isEmpty());
    QCOMPARE(sessionIdsOf(state.failed), QStringList({"s4"}));
    QCOMPARE(state.wantedCount, 3);
    QCOMPARE(state.doneCount, 3);
    QCOMPARE(values("s1", "ea"), QVector<double>({5.0}));
    QCOMPARE(values("s3", "ea"), QVector<double>({5.0}));
}

// Spec section 6: a pair whose result appeared by other means while it waits
// is dropped before it starts.
void CalculationDemandTest::resultAppearingWhileWaitingDropsThePair()
{
    QVERIFY(giveInput({"s1"}, "G_IN", 4));
    QVERIFY(giveInput({"s2"}, "EA_IN", 4));
    show({"s1", "s2"});
    QSignalSpy startedSpy(m_queue.get(), &JobQueue::jobStarted);
    check("g");
    check("ea");
    QVERIFY(gate().waitEntered());
    m_demand->flush();
    const JobId held = jobOf("s1", "gated").id;
    const JobId waiting = m_queue->chosenNextJob();
    QCOMPARE(m_queue->job(waiting).sessionId, QStringLiteral("s2"));
    QCOMPARE(m_queue->job(waiting).calculationId, QStringLiteral("expA"));
    DemandState state = row("Syn/ea");
    QCOMPARE(sessionIdsOf(state.waiting), QStringList({"s2"}));
    QCOMPARE(state.waiting.at(0).job, waiting);

    // Installed without the demand layer or the executor; ungated, and the
    // held worker is not touched
    QCOMPARE(engine("s2").request(QStringLiteral("expA")).status, ResultStatus::Ok);
    QCOMPARE(stateOf(waiting), JobState::Queued);

    gate().open(1);
    QVERIFY(waitDemandIdle());
    QCOMPARE(stateOf(held), JobState::Succeeded);
    QCOMPARE(stateOf(waiting), JobState::Cancelled);
    QCOMPARE(m_queue->job(waiting).reason, QString::fromLatin1(kNoLongerNeeded));
    QVERIFY(!m_queue->job(waiting).startedAt.isValid());
    QCOMPARE(engine("s2").runCount("expA"), 1);         // the synchronous run only
    for (const QList<QVariant> &started : std::as_const(startedSpy))
        QVERIFY(m_queue->job(started.at(0).value<JobId>()).sessionId != QLatin1String("s2"));
    QVERIFY(!m_model->isSessionPinned("s2"));

    state = row("Syn/ea");
    QCOMPARE(state.wantedCount, 1);
    QCOMPARE(state.doneCount, 1);
    QVERIFY(state.isPlain());
}

// ---- Failures -----------------------------------------------------------------------------------------

void CalculationDemandTest::inputDeterminedFailureIsStoredBadgedNeverRerun()
{
    QVERIFY(giveInput({"s1"}, "EA_IN", -1));
    show({"s1"});
    check("ea");
    QVERIFY(waitDemandIdle());
    QCOMPARE(jobOf("s1", "expA").state, JobState::Succeeded);      // a rejection is a result
    QVERIFY(stored("s1", "expA"));

    DemandState state = row("Syn/ea");
    QCOMPARE(state.failedCount, 1);
    QVERIFY(state.showsWarning());
    QVERIFY(!state.isWorking());
    QVERIFY(!state.isPlain());
    QCOMPARE(state.doneCount, 1);
    QCOMPARE(state.failed.at(0).sessionId, QStringLiteral("s1"));
    QCOMPARE(state.failed.at(0).calculationTitles, QStringList({"Explicit A"}));
    QCOMPARE(state.failed.at(0).reason, QStringLiteral("Explicit A: negative input"));
    QVERIFY(!state.failed.at(0).jobFailure);
    QCOMPARE(state.toolTip, QStringLiteral("Could not be computed:\n"
                                           "  Jump 1 - Explicit A: negative input"));

    // Never run again with the same inputs
    {
        const Quiet quiet(*m_queue);
        for (int i = 0; i < 3; ++i)
            spin();
        QVERIFY(quiet.holds());
    }

    // Its inputs change: offered once they settle, and available
    QVERIFY(giveInput({"s1"}, "EA_IN", 4));
    state = row("Syn/ea");
    QCOMPARE(state.failedCount, 0);
    QCOMPARE(state.waitingCount, 1);
    settle();
    QVERIFY(waitDemandIdle());
    QVERIFY(row("Syn/ea").isPlain());
    QCOMPARE(values("s1", "ea"), QVector<double>({5.0}));
    QCOMPARE(jobCount("expA"), 2);

    // An exception is an engine-cached failed result: badged, not offered
    // again in this run, and never stored
    QVERIFY(giveInput({"s2"}, "T_IN", 1));
    show({"s2"});
    check("t");
    QVERIFY(waitDemandIdle());
    QCOMPARE(jobOf("s2", "thrower").state, JobState::Succeeded);
    QCOMPARE(jobOf("s2", "thrower").resultStatus, std::optional<ResultStatus>(ResultStatus::Failed));
    state = row("Syn/t");
    QCOMPARE(state.failedCount, 1);
    QCOMPARE(state.failed.at(0).sessionId, QStringLiteral("s2"));
    QCOMPARE(state.failed.at(0).reason, QStringLiteral("Thrower: synthetic failure"));
    QVERIFY(state.showsWarning());
    {
        const Quiet quiet(*m_queue);
        for (int i = 0; i < 3; ++i)
            spin();
        QVERIFY(quiet.holds());
    }
    QVERIFY(!stored("s2", "thrower"));
}

void CalculationDemandTest::jobLevelFailureIsBadgedNotRerunUntilRestart_data()
{
    QTest::addColumn<QString>("plot");
    QTest::addColumn<QString>("calculation");
    QTest::addColumn<QString>("input");
    QTest::addColumn<bool>("failWorkerStart");
    QTest::addColumn<QString>("reason");
    QTest::addColumn<double>("value");
    QTest::newRow("worker start failure") << "ea" << "expA" << "EA_IN" << true
                                          << "Explicit A: The worker thread could not be started" << 5.0;
    QTest::newRow("out of memory") << "x" << "exhausted" << "X_IN" << false
                                   << "Exhausted: Out of memory" << 5.0;
}

// A failure of the job itself is not a result: it is badged, not offered again
// in this run and not stored. The next start (a new demand layer) tries again.
void CalculationDemandTest::jobLevelFailureIsBadgedNotRerunUntilRestart()
{
    QFETCH(QString, plot);
    QFETCH(QString, calculation);
    QFETCH(QString, input);
    QFETCH(bool, failWorkerStart);
    QFETCH(QString, reason);
    QFETCH(double, value);
    const QByteArray plotId = QByteArray("Syn/") + plot.toLatin1();
    const QByteArray calculationId = calculation.toLatin1();

    QVERIFY(giveInput({"s1"}, input.toLatin1().constData(), 4));
    show({"s1"});
    if (failWorkerStart)
        m_queue->failNextWorkerStarts(1);
    check(plot.toLatin1().constData());
    QVERIFY(waitDemandIdle());
    const JobRecord failed = jobOf("s1", calculationId.constData());
    QCOMPARE(failed.state, JobState::Failed);

    DemandState state = row(plotId.constData());
    QCOMPARE(state.failedCount, 1);
    QVERIFY(state.showsWarning());
    QVERIFY(state.failed.at(0).jobFailure);
    QCOMPARE(state.failed.at(0).reason, reason);
    QCOMPARE(state.toolTip, QStringLiteral("Could not be computed:\n  Jump 1 - ") + reason);
    {
        const Quiet quiet(*m_queue);
        for (int i = 0; i < 3; ++i)
            spin();
        QVERIFY(quiet.holds());
    }
    QVERIFY(!stored("s1", calculationId.constData()));

    // A restart: nothing is remembered, and the pair is offered again
    restartDemand();
    QVERIFY(waitDemandIdle());
    QCOMPARE(jobOf("s1", calculationId.constData()).state, JobState::Succeeded);
    QCOMPARE(jobCount(calculationId.constData()), 2);
    state = row(plotId.constData());
    QVERIFY(state.isPlain());
    QCOMPARE(values("s1", plot.toLatin1().constData()), QVector<double>({value}));
    QVERIFY(stored("s1", calculationId.constData()));
}

// s1 was rejected, s2 waits behind another plot's held job: while working the
// indicator shows and the failure is listed in the tooltip only; afterwards
// the badge shows.
void CalculationDemandTest::failuresListedWhileWorking()
{
    QVERIFY(giveInput({"s1"}, "EA_IN", -1));
    QVERIFY(giveInput({"s2"}, "EA_IN", 4));
    QVERIFY(giveInput({"s3"}, "G_IN", 4));
    show({"s1"});
    check("ea");
    QVERIFY(waitDemandIdle());
    QCOMPARE(row("Syn/ea").failedCount, 1);

    show({"s3"});
    check("g");                             // holds the worker
    QVERIFY(gate().waitEntered());
    show({"s2"});
    DemandState state = row("Syn/ea");
    QCOMPARE(chosenNext().sessionId, QStringLiteral("s2"));
    QVERIFY(state.isWorking());
    QCOMPARE(state.failedCount, 1);
    QCOMPARE(state.waitingCount, 1);
    QCOMPARE(state.runningCount, 0);
    QCOMPARE(state.wantedCount, 2);
    QCOMPARE(state.doneCount, 1);
    QVERIFY(!state.showsWarning());
    QCOMPARE(state.progressLabel, QStringLiteral("1 of 2"));
    QCOMPARE(state.toolTip, QStringLiteral("Computing: 1 of 2 done\n"
                                           "Could not be computed:\n"
                                           "  Jump 1 - Explicit A: negative input"));

    gate().open(1);
    QVERIFY(waitDemandIdle());
    state = row("Syn/ea");
    QCOMPARE(state.failedCount, 1);
    QVERIFY(!state.isWorking());
    QVERIFY(state.showsWarning());
    QCOMPARE(state.doneCount, 2);
    QCOMPARE(values("s2", "ea"), QVector<double>({5.0}));
}

// CalculationDemand::isMerelyUncomputed() decides whether the plot widget
// keeps quiet about a track it could not draw ("No data available for plot"):
// yes for a value that waits on a requested calculation or was rejected by one,
// no for everything else. One case per BlockerReport state. It inspects only:
// no explicit calculation runs and no job appears, however often it is asked.
void CalculationDemandTest::merelyUncomputedIsNotWorthAWarning()
{
    const auto merelyUncomputed = [this](const char *sessionId, const char *sensor, const char *measurement) {
        return CalculationDemand::isMerelyUncomputed(session(QString::fromLatin1(sessionId)),
                                                     QString::fromLatin1(sensor), QString::fromLatin1(measurement));
    };
    const auto explicitRuns = [this](const char *sessionId) {
        int runs = 0;
        for (const char *id : {"expA", "expB", "gated", "thrower", "afterG"})
            runs += engine(QString::fromLatin1(sessionId)).runCount(QString::fromLatin1(id));
        return runs;
    };

    QVERIFY(giveInput({"s1"}, "EA_IN", 4));
    QVERIFY(giveInput({"s1"}, "EB_IN", 10));
    QVERIFY(giveInput({"s2"}, "EA_IN", -1));
    QVERIFY(giveInput({"s2"}, "T_IN", 1));
    QVERIFY(giveInput({"s4"}, "P_IN", 7));
    // s3 has no input of any synthetic calculation

    // Blocked: not computed yet. Directly behind the explicit calculation, and
    // at the end of a chain of two. The value is empty and nobody is warned.
    {
        const Quiet quiet(*m_queue);
        for (int i = 0; i < 3; ++i) {
            QVERIFY(values("s1", "ea").isEmpty());
            QVERIFY(merelyUncomputed("s1", "Syn", "ea"));
            QVERIFY(merelyUncomputed("s1", "Syn", "db"));
        }
        QCOMPARE(engine("s1").blockers(Synthetic::measKey("Syn", "ea")).state, BlockerReport::State::Blocked);
        QCOMPARE(explicitRuns("s1"), 0);
        QVERIFY(quiet.holds());
    }

    // NotApplicable: a missing input, a plot over stored data that is not
    // there, a sensor nobody knows. These keep their warning.
    {
        const Quiet quiet(*m_queue);
        QVERIFY(values("s3", "ea").isEmpty());
        QVERIFY(!merelyUncomputed("s3", "Syn", "ea"));
        QVERIFY(!merelyUncomputed("s3", "Syn", "db"));
        QVERIFY(!merelyUncomputed("s3", "Syn", "plain"));
        QVERIFY(!merelyUncomputed("s3", "NoSuchSensor", "nothing"));
        QVERIFY(!merelyUncomputed("s1", "NoSuchSensor", "nothing"));
        QCOMPARE(engine("s3").blockers(Synthetic::measKey("Syn", "ea")).state,
                 BlockerReport::State::NotApplicable);
        QCOMPARE(explicitRuns("s3"), 0);
        QVERIFY(quiet.holds());
    }

    // Available: ordinary data ...
    QCOMPARE(values("s4", "plain"), QVector<double>({7.0}));
    QVERIFY(!merelyUncomputed("s4", "Syn", "plain"));

    // NotProduced: computed, and rejected its input (a result), or threw (a
    // cached failure). The row carries the badge; the plot widget is silent.
    QCOMPARE(engine("s2").request(QStringLiteral("expA")).status, ResultStatus::Ok);
    QCOMPARE(engine("s2").request(QStringLiteral("thrower")).status, ResultStatus::Failed);
    // ... and Available: computed and installed
    QCOMPARE(engine("s1").request(QStringLiteral("expA")).status, ResultStatus::Ok);
    {
        const Quiet quiet(*m_queue);
        const int runsS1 = explicitRuns("s1"), runsS2 = explicitRuns("s2");
        QCOMPARE(runsS1, 1);
        QCOMPARE(runsS2, 2);
        for (int i = 0; i < 3; ++i) {
            QVERIFY(values("s2", "ea").isEmpty());
            QVERIFY(merelyUncomputed("s2", "Syn", "ea"));
            QVERIFY(values("s2", "t").isEmpty());
            QVERIFY(merelyUncomputed("s2", "Syn", "t"));

            QCOMPARE(values("s1", "ea"), QVector<double>({5.0}));
            QVERIFY(!merelyUncomputed("s1", "Syn", "ea"));
            // The chain's second link is still not computed
            QVERIFY(values("s1", "db").isEmpty());
            QVERIFY(merelyUncomputed("s1", "Syn", "db"));
        }
        QCOMPARE(engine("s2").blockers(Synthetic::measKey("Syn", "ea")).state, BlockerReport::State::NotProduced);
        QCOMPARE(engine("s1").blockers(Synthetic::measKey("Syn", "ea")).state, BlockerReport::State::Available);
        QCOMPARE(explicitRuns("s1"), runsS1);
        QCOMPARE(explicitRuns("s2"), runsS2);
        QVERIFY(quiet.holds());
    }
}

// ---- Plots that share jobs --------------------------------------------------------------------------------

void CalculationDemandTest::sharedJobSameProgress()
{
    QVERIFY(giveInput({"s1", "s2"}, "G_IN", 4));
    show({"s1", "s2"});
    check("g");
    check("g2");
    QVERIFY(gate().waitEntered());
    QTRY_COMPARE(row("Syn/g").running.value(0).progressText, QStringLiteral("step 1"));
    QCOMPARE(m_queue->model()->rowCount(), 2);      // one job per session

    const DemandState g = row("Syn/g");
    const DemandState g2 = row("Syn/g2");
    QCOMPARE(g.runningCount, 1);
    QCOMPARE(g.waitingCount, 1);
    QCOMPARE(g2.runningCount, g.runningCount);
    QCOMPARE(g2.waitingCount, g.waitingCount);
    QCOMPARE(g2.wantedCount, g.wantedCount);
    QCOMPARE(g2.progressLabel, g.progressLabel);
    QCOMPARE(g.progressLabel, QStringLiteral("0 of 2"));
    QCOMPARE(g2.running.at(0).progressText, QStringLiteral("step 1"));
    QCOMPARE(g2.toolTip, g.toolTip);
    QCOMPARE(g2.running.at(0).job, g.running.at(0).job);
    QCOMPARE(g2.waiting.at(0).job, g.waiting.at(0).job);

    gate().open(2);
    QVERIFY(waitDemandIdle());
    QVERIFY(row("Syn/g").isPlain());
    QVERIFY(row("Syn/g2").isPlain());
    QCOMPARE(values("s1", "g2"), QVector<double>({10.0}));
    QCOMPARE(m_queue->model()->rowCount(), 2);
}

// A plot checked while another plot's job runs waits on it too, and its own
// chain continues with no other call.
void CalculationDemandTest::plotCheckedDuringAJobJoinsIt()
{
    QVERIFY(giveInput({"s1"}, "G_IN", 4));
    show({"s1"});
    check("g");
    QVERIFY(gate().waitEntered());
    const JobId gated = jobOf("s1", "gated").id;

    check("h");
    const DemandState state = row("Syn/h");
    QCOMPARE(state.runningCount, 1);
    QCOMPARE(state.running.at(0).job, gated);
    QCOMPARE(state.progressLabel, QStringLiteral("0 of 1"));
    QCOMPARE(m_queue->model()->rowCount(), 1);

    gate().open(1);
    QVERIFY(waitDemandIdle());
    QCOMPARE(jobCount("gated"), 1);
    QCOMPARE(jobCount("afterG"), 1);
    QVERIFY(row("Syn/g").isPlain());
    QVERIFY(row("Syn/h").isPlain());
    QCOMPARE(values("s1", "h"), QVector<double>({6.0}));
}

// ---- Priority (spec section 7) ------------------------------------------------------------------------------

void CalculationDemandTest::focusedSessionFirstThenRowOrder()
{
    for (int i = 1; i <= 4; ++i)
        QVERIFY(giveInput({QStringLiteral("s%1").arg(i)}, "G_IN", i));
    show({"s1", "s2", "s3", "s4"});
    m_model->setFocusedSessionId(QStringLiteral("s3"));
    gate().open(4);
    check("g");
    QVERIFY(waitDemandIdle());
    QCOMPARE(gate().startOrder(), QList<int>({3, 1, 2, 4}));
    QCOMPARE(history(), QList<JobState>(4, JobState::Succeeded));
    QVERIFY(row("Syn/g").isPlain());
}

// While a job runs, the chosen next job is the demand layer's current choice:
// a change of demand replaces it.
void CalculationDemandTest::changingDemandReplacesChosenNext()
{
    QVERIFY(giveInput({"s1", "s3", "s4"}, "G_IN", 4));
    show({"s1", "s3", "s4"});
    m_model->setFocusedSessionId(QStringLiteral("s3"));
    check("g");
    QVERIFY(gate().waitEntered());
    m_demand->flush();
    QCOMPARE(running().sessionId, QStringLiteral("s3"));
    const JobRecord s1Job = chosenNext();
    QCOMPARE(s1Job.sessionId, QStringLiteral("s1"));

    m_model->setFocusedSessionId(QStringLiteral("s4"));
    m_demand->flush();
    QCOMPARE(chosenNext().sessionId, QStringLiteral("s4"));
    QCOMPARE(stateOf(s1Job.id), JobState::Cancelled);
    QCOMPARE(m_queue->job(s1Job.id).reason, QString::fromLatin1(kNoLongerNeeded));
    QVERIFY(!m_queue->job(s1Job.id).startedAt.isValid());
    QCOMPARE(running().sessionId, QStringLiteral("s3"));        // never preempted

    gate().open(3);
    QVERIFY(waitDemandIdle());
    QCOMPARE(gate().startOrder(), QList<int>({4, 4, 4}));       // every input is 4
    for (const char *id : {"s1", "s3", "s4"})
        QCOMPARE(jobOf(id, "gated").state, JobState::Succeeded);
    QVERIFY(row("Syn/g").isPlain());
}

// Over the whole flow of focusedSessionFirstThenRowOrder: never more than the
// running job and one chosen next job.
void CalculationDemandTest::executorHoldsAtMostRunningAndChosenNext()
{
    int maxActive = 0;
    int maxQueued = 0;
    QObject scope;
    connect(m_queue.get(), &JobQueue::jobsChanged, &scope, [&] {
        int active = 0, queued = 0;
        for (const JobRecord &record : m_queue->model()->records()) {
            if (record.isActive())
                ++active;
            if (record.state == JobState::Queued)
                ++queued;
        }
        maxActive = qMax(maxActive, active);
        maxQueued = qMax(maxQueued, queued);
    });

    for (int i = 1; i <= 4; ++i)
        QVERIFY(giveInput({QStringLiteral("s%1").arg(i)}, "G_IN", i));
    show({"s1", "s2", "s3", "s4"});
    m_model->setFocusedSessionId(QStringLiteral("s3"));
    check("g");
    for (int i = 0; i < 4; ++i) {
        QVERIFY(gate().waitEntered());
        m_demand->flush();
        QVERIFY(m_queue->activeJobs().size() <= 2);
        gate().open(1);
    }
    QVERIFY(waitDemandIdle());
    QCOMPARE(gate().startOrder(), QList<int>({3, 1, 2, 4}));
    QCOMPARE(maxActive, 2);
    QCOMPARE(maxQueued, 1);
    QCOMPARE(JobQueue::kMaxRunningJobs, 1);
}

// ---- Tooltip, signals, coalescing ------------------------------------------------------------------------

void CalculationDemandTest::tooltipText()
{
    // The live tooltip: s1 running, s2 waiting
    QVERIFY(giveInput({"s1", "s2"}, "G_IN", 4));
    show({"s1", "s2"});
    check("g");
    QVERIFY(gate().waitEntered());
    QTRY_COMPARE(row("Syn/g").toolTip, QStringLiteral("Computing: 0 of 2 done\n"
                                                      "  Jump 1 - Gated: step 1"));
    gate().open(2);
    QVERIFY(waitDemandIdle());
    QVERIFY(row("Syn/g").toolTip.isEmpty());

    // The pure function
    DemandState state;
    QCOMPARE(CalculationDemand::buildToolTip(state), QString());

    // A running track without progress text, several titles
    DemandTrack withoutText;
    withoutText.sessionName = QStringLiteral("Morning");
    withoutText.condition = DemandCondition::Running;
    withoutText.calculationTitles = {QStringLiteral("Sensor fusion"), QStringLiteral("Other")};
    DemandTrack withText = withoutText;
    withText.sessionName = QStringLiteral("Noon");
    withText.calculationTitles = {QStringLiteral("Sensor fusion")};
    withText.progressText = QStringLiteral("iteration 3");
    state.running = {withoutText, withText};
    state.runningCount = 2;
    state.wantedCount = 5;
    state.doneCount = 3;
    QCOMPARE(CalculationDemand::buildToolTip(state),
             QStringLiteral("Computing: 3 of 5 done\n"
                            "  Morning - Sensor fusion, Other: running\n"
                            "  Noon - Sensor fusion: iteration 3"));

    // A job-level failure, while working and afterwards
    DemandTrack failed;
    failed.sessionName = QStringLiteral("Evening");
    failed.condition = DemandCondition::Failed;
    failed.jobFailure = true;
    failed.calculationTitles = {QStringLiteral("Sensor fusion")};
    failed.reason = QStringLiteral("Sensor fusion: Out of memory");
    state.failed = {failed};
    state.failedCount = 1;
    QCOMPARE(CalculationDemand::buildToolTip(state),
             QStringLiteral("Computing: 3 of 5 done\n"
                            "  Morning - Sensor fusion, Other: running\n"
                            "  Noon - Sensor fusion: iteration 3\n"
                            "Could not be computed:\n"
                            "  Evening - Sensor fusion: Out of memory"));
    DemandState finished;
    finished.wantedCount = 1;
    finished.doneCount = 1;
    finished.failed = {failed};
    finished.failedCount = 1;
    QVERIFY(finished.showsWarning());
    QCOMPARE(CalculationDemand::buildToolTip(finished),
             QStringLiteral("Could not be computed:\n"
                            "  Evening - Sensor fusion: Out of memory"));
}

void CalculationDemandTest::changeSignalsAreMinimal()
{
    QVERIFY(giveInput({"s1"}, "G_IN", 4));
    QVERIFY(giveInput({"s1"}, "P_IN", 3));
    show({"s1"});
    m_demand->flush();

    QSignalSpy changedSpy(m_demand.get(), &CalculationDemand::plotStateChanged);
    QSignalSpy anySpy(m_demand.get(), &CalculationDemand::statesChanged);

    // Only requested plots are announced, once each, in one pass
    check("plain");
    check("g");
    check("g2");
    m_demand->flush();
    QCOMPARE(changedSpy.count(), 2);
    QCOMPARE(changedSpy.at(0).at(0).toString(), QStringLiteral("Syn/g"));
    QCOMPARE(changedSpy.at(1).at(0).toString(), QStringLiteral("Syn/g2"));
    QCOMPARE(anySpy.count(), 1);
    QVERIFY(m_queue->chosenNextJob() != 0);         // offered, not started yet

    // A pass that changes nothing announces nothing (s2 has no input)
    show({"s2"});
    QVERIFY(m_demand->hasPendingUpdate());
    m_demand->flush();
    QCOMPARE(changedSpy.count(), 2);
    QCOMPARE(anySpy.count(), 1);

    // Unchecked: reset to the default state, announced once; the waiting job
    // stays, the other plot still needs it
    const JobId waiting = m_queue->chosenNextJob();
    check("g2", false);
    m_demand->flush();
    QCOMPARE(changedSpy.count(), 3);
    QCOMPARE(changedSpy.last().at(0).toString(), QStringLiteral("Syn/g2"));
    QCOMPARE(anySpy.count(), 2);
    m_demand->flush();
    QCOMPARE(changedSpy.count(), 3);
    QCOMPARE(m_queue->chosenNextJob(), waiting);

    gate().open(1);
    QVERIFY(waitDemandIdle());
    for (const QList<QVariant> &emission : std::as_const(changedSpy))
        QVERIFY(emission.at(0).toString() != QLatin1String("Syn/plain"));
    QVERIFY(row("Syn/plain") == DemandState());
}

void CalculationDemandTest::dependencyBurstIsCoalesced()
{
    QVERIFY(giveInput({"s1", "s2", "s3"}, "G_IN", 4));
    show({"s1", "s2", "s3"});
    gate().open(3);
    check("g");
    QVERIFY(waitDemandIdle());
    QVERIFY(row("Syn/g").isPlain());
    m_demand->setInputSettleDelay(60000);

    // An irrelevant name schedules nothing and starts no wait
    const int passes = m_demand->passCount();
    QVERIFY(giveInput({"s1"}, "UNRELATED_KEY", 1));
    QVERIFY(!m_demand->hasPendingUpdate());
    QVERIFY(m_model->updateAttribute("s1", QString::fromLatin1(SessionKeys::Description), QStringLiteral("Renamed")));
    QVERIFY(!m_demand->hasPendingUpdate());
    QVERIFY(!m_demand->hasSettlingSessions());

    // Many relevant ones in one event-loop pass cause exactly one pass
    const Quiet quiet(*m_queue);
    QVERIFY(giveInput({"s1", "s2", "s3"}, "G_IN", 6));
    QVERIFY(giveInput({"s1", "s2", "s3"}, "G_IN", 8));
    QVERIFY(m_demand->hasPendingUpdate());
    QCOMPARE(m_demand->passCount(), passes);
    QTRY_VERIFY(!m_demand->hasPendingUpdate());
    QCOMPARE(m_demand->passCount(), passes + 1);
    QVERIFY(quiet.holds());                         // settling: nothing offered

    // The name is read live at the next pass
    const DemandState state = row("Syn/g");
    QCOMPARE(state.waitingCount, 3);
    QCOMPARE(state.waiting.at(0).sessionName, QStringLiteral("Renamed"));
    QVERIFY(state.waiting.at(0).settling);
}

void CalculationDemandTest::progressUpdatesWithoutInspection()
{
    QVERIFY(giveInput({"s1", "s2"}, "G_IN", 4));
    show({"s1", "s2"});
    check("g");
    check("g2");
    QVERIFY(gate().waitEntered());
    const JobId runningJob = jobOf("s1", "gated").id;
    QTRY_COMPARE(m_queue->job(runningJob).progressText, QStringLiteral("step 1"));
    spin();

    QSignalSpy changedSpy(m_demand.get(), &CalculationDemand::plotStateChanged);
    const int passes = m_demand->passCount();
    const int runs = totalRuns();

    // The executor's own signal, delivered by hand: the text alone changes
    emit m_queue->jobProgress(runningJob, QStringLiteral("iteration 7"));
    QVERIFY(!m_demand->hasPendingUpdate());
    QCOMPARE(m_demand->passCount(), passes);
    QCOMPARE(totalRuns(), runs);
    QCOMPARE(changedSpy.count(), 2);
    for (const char *id : {"Syn/g", "Syn/g2"}) {
        const DemandState state = m_demand->plotState(QString::fromLatin1(id));
        QCOMPARE(state.running.at(0).progressText, QStringLiteral("iteration 7"));
        QCOMPARE(state.toolTip, QStringLiteral("Computing: 0 of 2 done\n"
                                               "  Jump 1 - Gated: iteration 7"));
    }

    // The same text again announces nothing; a job no plot waits on neither
    emit m_queue->jobProgress(runningJob, QStringLiteral("iteration 7"));
    emit m_queue->jobProgress(JobId(999), QStringLiteral("elsewhere"));
    QCOMPARE(changedSpy.count(), 2);
    QCOMPARE(m_demand->passCount(), passes);

    gate().open(2);
    QVERIFY(waitDemandIdle());
}

// ---- Robustness ----------------------------------------------------------------------------------------------

void CalculationDemandTest::removedSessionLeavesNoTrace()
{
    // A remembered job failure of a session that is removed is forgotten: the
    // session that comes back under the same id is offered again
    QVERIFY(giveInput({"s4"}, "G_IN", 4));
    show({"s4"});
    m_queue->failNextWorkerStarts(1);
    check("g");
    QVERIFY(waitDemandIdle());
    QCOMPARE(jobOf("s4", "gated").state, JobState::Failed);
    QCOMPARE(row("Syn/g").failedCount, 1);
    QVERIFY(m_model->removeSessions({"s4"}));
    QVERIFY(row("Syn/g").isPlain());
    const QList<MergeResult> created = m_model->mergeSessions(newSessionWithInput("s4"));
    QCOMPARE(created.at(0).outcome, MergeResult::Outcome::Created);
    show({"s4"});
    QVERIFY(gate().waitEntered());
    QCOMPARE(running().sessionId, QStringLiteral("s4"));
    gate().open(1);
    QVERIFY(waitDemandIdle());
    QCOMPARE(jobOf("s4", "gated").state, JobState::Succeeded);
    QVERIFY(row("Syn/g").isPlain());

    // The chosen next session is removed: the executor supersedes its job and
    // the demand layer offers the next
    QVERIFY(giveInput({"s1", "s2", "s3"}, "G_IN", 4));
    settle();                               // g is checked: the edits were input changes
    show({"s1", "s2", "s3"});
    QVERIFY(gate().waitEntered());
    m_demand->flush();
    const JobId runningJob = jobOf("s1", "gated").id;
    QCOMPARE(running().id, runningJob);
    const JobId queued = jobOf("s2", "gated").id;
    QCOMPARE(m_queue->chosenNextJob(), queued);

    QVERIFY(m_model->removeSessions({"s2"}));
    QCOMPARE(stateOf(queued), JobState::Superseded);
    DemandState state = row("Syn/g");
    QCOMPARE(chosenNext().sessionId, QStringLiteral("s3"));
    QCOMPARE(sessionIdsOf(state.running), QStringList({"s1"}));
    QCOMPARE(sessionIdsOf(state.waiting), QStringList({"s3"}));

    // The running session is removed: its job is abandoned, and s3 runs next
    QVERIFY(m_model->removeSessions({"s1"}));
    state = row("Syn/g");
    QCOMPARE(sessionIdsOf(state.running), QStringList());
    QCOMPARE(sessionIdsOf(state.waiting), QStringList({"s3"}));
    QTRY_COMPARE(stateOf(runningJob), JobState::Superseded);

    QVERIFY(gate().waitEntered());                  // s3 runs next
    gate().open(1);
    QVERIFY(waitDemandIdle());
    QVERIFY(row("Syn/g").isPlain());
    QCOMPARE(values("s3", "g"), QVector<double>({5.0}));
}

// "Requested" is a function of the registrations, with or without sessions.
void CalculationDemandTest::registryChangeReclassifies()
{
    // A registration's broadcast invalidation reaches the model as an input
    // change; this test is about the registry, not the wait
    m_demand->setInputSettleDelay(0);

    // Syn/db is two on-demand levels above an explicit output (plotDB <- derivB <- expB)
    check("db");
    check("plain");
    QVERIFY(row("Syn/db").requested);
    QVERIFY(!row("Syn/plain").requested);

    // A plot of the test's own: Syn/rx <- RX_OUT <- regX (explicit)
    CalculationDescriptor regX;
    regX.id = QStringLiteral("regX");
    regX.title = QStringLiteral("Reg X");
    regX.policy = EvaluationPolicy::Explicit;
    regX.inputs = {CalcInput::attribute(QStringLiteral("RX_IN"))};
    regX.outputs = {DependencyKey::attribute(QStringLiteral("RX_OUT"))};
    regX.compute = [](const EvaluationContext &ctx) {
        return CalculationResult().setAttribute(QStringLiteral("RX_OUT"), ctx.attribute(QStringLiteral("RX_IN")).toInt() + 1);
    };
    CalculationDescriptor plotRX;
    plotRX.id = QStringLiteral("plotRX");
    plotRX.inputs = {CalcInput::attribute(QStringLiteral("RX_OUT"))};
    plotRX.outputs = {DependencyKey::measurement(QStringLiteral("Syn"), QStringLiteral("rx"))};
    plotRX.compute = [](const EvaluationContext &ctx) {
        return CalculationResult().setMeasurement(QStringLiteral("Syn"), QStringLiteral("rx"),
                                                  {ctx.attribute(QStringLiteral("RX_OUT")).toDouble()});
    };
    CalculationRegistry &registry = CalculationRegistry::instance();
    QVERIFY(registry.registerCalculation(plotRX));
    const auto unregister = qScopeGuard([&registry] {
        registry.unregister(QStringLiteral("regX"), CalculationRegistry::Removal::Change);
        registry.unregister(QStringLiteral("plotRX"), CalculationRegistry::Removal::Change);
    });

    QVector<PlotValue> plots = PlotFixture::plots();
    PlotValue rx = plots.first();
    rx.plotName = QStringLiteral("rx");
    rx.measurementID = QStringLiteral("rx");
    plots.append(rx);
    m_plots->setPlots(plots);
    m_plots->setPlotEnabled("Syn", "rx", true);
    QVERIFY(giveInput({"s1"}, "RX_IN", 1));
    show({"s1"});

    // Not requested yet: nothing to compute, nothing offered
    QVERIFY(!row("Syn/rx").requested);
    spin();
    QCOMPARE(jobCount("regX"), 0);

    // Registered: requested, and started with no other call (regX is not gated)
    QVERIFY(registry.registerCalculation(regX));
    QVERIFY(m_demand->hasPendingUpdate());
    QVERIFY(row("Syn/rx").requested);
    QVERIFY(waitDemandIdle());
    QCOMPARE(jobCount("regX"), 1);
    QCOMPARE(jobOf("s1", "regX").state, JobState::Succeeded);
    QVERIFY(row("Syn/rx").isPlain());
    QCOMPARE(values("s1", "rx"), QVector<double>({2.0}));

    // Unregistered: the default state, announced once, and nothing offered
    QSignalSpy changedSpy(m_demand.get(), &CalculationDemand::plotStateChanged);
    {
        const Quiet quiet(*m_queue);
        QVERIFY(registry.unregister(QStringLiteral("regX"), CalculationRegistry::Removal::Change));
        QVERIFY(row("Syn/rx") == DemandState());
        QCOMPARE(changedSpy.count(), 1);
        QCOMPARE(changedSpy.at(0).at(0).toString(), QStringLiteral("Syn/rx"));
        spin();
        QVERIFY(quiet.holds());
    }

    // Registered again: the result went with the registration, so there is
    // demand again
    QVERIFY(registry.registerCalculation(regX));
    QVERIFY(row("Syn/rx").requested);
    QVERIFY(waitDemandIdle());
    QCOMPARE(jobCount("regX"), 2);
    QVERIFY(row("Syn/rx").isPlain());
    m_model->flushPendingInvalidations();
}

void CalculationDemandTest::survivesExecutorShutdown()
{
    QVERIFY(giveInput({"s1", "s2"}, "G_IN", 4));
    show({"s1", "s2"});
    check("g");
    QVERIFY(gate().waitEntered());

    m_queue->shutdown();
    QVERIFY(m_queue->isIdle());
    DemandState state = row("Syn/g");
    QCOMPARE(state.runningCount, 0);
    QCOMPARE(state.waitingCount, 2);        // still wanted: waiting, never offered

    {
        const Quiet quiet(*m_queue);
        check("g", false);
        QVERIFY(row("Syn/g") == DemandState());
        check("g");
        show({"s1"}, false);
        spin();
        state = row("Syn/g");
        QCOMPARE(state.waitingCount, 1);
        show({"s1"});
        spin();
        QVERIFY(quiet.holds());
    }

    // The component outlives the executor and becomes inert
    m_queue.reset();
    show({"s1"}, false);                    // schedules a pass
    m_demand->flush();
    QVERIFY(m_demand->plotState(QStringLiteral("Syn/g")) == DemandState());
    show({"s1"});
    m_demand->flush();
    QVERIFY(m_demand->plotState(QStringLiteral("Syn/g")) == DemandState());
}

void CalculationDemandTest::nullCollaborators()
{
    // Only the demand layers built here exist while the executor is watched
    m_demand.reset();
    QVERIFY(giveInput({"s1"}, "G_IN", 4));
    show({"s1"});
    check("g");

    // A QVERIFY in here returns from the lambda only: each call is followed by
    // a check of QTest::currentTestFailed()
    const auto verifyInert = [](CalculationDemand &demand) {
        demand.flush();
        QTest::qWait(0);
        demand.flush();
        QVERIFY(!demand.hasPendingUpdate());
        QVERIFY(demand.plotState(QStringLiteral("Syn/g")) == DemandState());
        demand.setInputSettleDelay(10);
        QCOMPARE(demand.inputSettleDelay(), 10);
        demand.endInputSettleWaits();
        demand.flush();
        QVERIFY(!demand.isSettling(QStringLiteral("s1")));
        QVERIFY(!demand.hasSettlingSessions());
        QVERIFY(demand.plotState(QStringLiteral("Syn/g")) == DemandState());
    };

    const Quiet quiet(*m_queue);
    {
        CalculationDemand demand(nullptr, m_plots.get(), m_queue.get());
        verifyInert(demand);
        if (QTest::currentTestFailed())
            return;
    }
    {
        CalculationDemand demand(m_model.get(), nullptr, m_queue.get());
        verifyInert(demand);
        if (QTest::currentTestFailed())
            return;
    }
    {
        CalculationDemand demand(m_model.get(), m_plots.get(), nullptr);
        verifyInert(demand);
        if (QTest::currentTestFailed())
            return;
    }
    {
        CalculationDemand demand(nullptr, nullptr, nullptr);
        verifyInert(demand);
        if (QTest::currentTestFailed())
            return;
    }
    QTest::qWait(0);
    QVERIFY(quiet.holds());
    QVERIFY(m_queue->isIdle());

    // A real one works
    restartDemand();
    QVERIFY(gate().waitEntered());
    QCOMPARE(row("Syn/g").runningCount, 1);
    gate().open(1);
    QVERIFY(waitDemandIdle());
    QVERIFY(row("Syn/g").isPlain());
}

// ---- Column demand ----------------------------------------------------------------

// A column's id is its definition key. A column that is not enabled, not
// requested or unknown has the default state, and no cell of it is pending.
void CalculationDemandTest::columnIdIsTheDefinitionKey()
{
    const LogbookColumn g = attributeColumn(QStringLiteral("G_OUT"));
    QCOMPARE(CalculationDemand::columnId(g), logbookColumnDefinitionKey(g));
    QCOMPARE(CalculationDemand::columnId(descriptionColumn()), logbookColumnDefinitionKey(descriptionColumn()));

    m_demand->flush();
    QVERIFY(m_demand->columnState(CalculationDemand::columnId(descriptionColumn())) == DemandState());
    QVERIFY(m_demand->columnState(colId("G_OUT")) == DemandState());      // not enabled
    QVERIFY(m_demand->columnState(QStringLiteral("nope")) == DemandState());
    QVERIFY(!m_demand->isCellPending(-1, 0));
    QVERIFY(!m_demand->isCellPending(0, 99));
    QVERIFY(!m_demand->isCellPending(0, section("_DESCRIPTION")));
    QVERIFY(!m_demand->isCellPending(QStringLiteral("s1"), colId("G_OUT")));
}

// The description column reads stored data only: no row is walked, nothing is
// loaded, and nothing is announced.
void CalculationDemandTest::ordinaryColumnsCreateNoDemand()
{
    QVERIFY(giveInput({"s1", "s2"}, "G_IN", 4));
    QVERIFY(makeStubs());
    QVERIFY(waitForIdle(*m_model));

    QSignalSpy loadedSpy(m_model.get(), &SessionModel::sessionLoaded);
    QSignalSpy columnSpy(m_demand.get(), &CalculationDemand::columnStateChanged);
    const Quiet quiet(*m_queue);
    enableColumns({});
    spin();
    QVERIFY(waitForIdle(*m_model));
    spin();

    QVERIFY(quiet.holds());
    QCOMPARE(loadedSpy.count(), 0);
    QCOMPARE(m_demand->recordSetLookups(), 0);
    QCOMPARE(columnSpy.count(), 0);
    QVERIFY(!m_demand->hasFillWork());
    QVERIFY(m_demand->columnState(CalculationDemand::columnId(descriptionColumn())) == DemandState());
}

void CalculationDemandTest::enablingColumnFillsEveryUnloadedSession_data()
{
    QTest::addColumn<int>("capacity");
    QTest::newRow("capacity 0") << 0;
    QTest::newRow("capacity 1") << 1;
    QTest::newRow("capacity 50") << 50;
}

// Spec 13: enabling a column over a requested output wants every session
// without a result, loads the stubs a bounded number at a time as hidden,
// pinned sessions, fills the column, and ends with every session computed or
// not applicable; the sessions leave the pool by ordinary eviction.
void CalculationDemandTest::enablingColumnFillsEveryUnloadedSession()
{
    QFETCH(int, capacity);
    m_capacity = capacity;
    QVERIFY(giveInput({"s1"}, "G_IN", 1));
    QVERIFY(giveInput({"s3"}, "G_IN", 3));
    QVERIFY(giveInput({"s4"}, "G_IN", 4));      // s2 has none
    QVERIFY(makeStubs());
    QVERIFY(waitForIdle(*m_model));
    gate().open(3);

    QObject scope;
    QString violation;
    watchHolds(&scope, &violation);
    QSignalSpy loadedSpy(m_model.get(), &SessionModel::sessionLoaded);

    enableColumns({"G_OUT"});
    DemandState state = col("G_OUT");
    QVERIFY(state.requested);
    QCOMPARE(state.sourceId, colId("G_OUT"));
    QCOMPARE(state.wantedCount, 4);
    QCOMPARE(state.waitingCount, 4);
    QVERIFY(state.waiting.isEmpty());       // counted, not listed
    for (const char *id : {"s1", "s2", "s3", "s4"})
        QVERIFY2(isCellPending(QString::fromLatin1(id), "G_OUT"), id);

    QVERIFY(waitDemandIdle());
    QVERIFY(waitForIdle(*m_model));
    spin();
    QVERIFY2(violation.isEmpty(), qPrintable(violation));

    QCOMPARE(gate().startOrder(), QList<int>({1, 3, 4}));
    for (const char *id : {"s1", "s3", "s4"}) {
        QCOMPARE(jobOf(QString::fromLatin1(id), "gated").state, JobState::Succeeded);
        QVERIFY2(stored(QString::fromLatin1(id), "gated"), id);
    }
    QCOMPARE(jobOf(QStringLiteral("s2"), "gated").id, JobId(0));
    QCOMPARE(m_queue->model()->rowCount(), 3);

    m_model->flushDirtySessions();          // the index as the model knows it
    const QJsonObject index = readIndex();
    const LogbookColumn g = attributeColumn(QStringLiteral("G_OUT"));
    for (const auto &[id, value] : {std::pair{"s1", 2.0}, std::pair{"s3", 4.0}, std::pair{"s4", 5.0}}) {
        QCOMPARE(cachedValue(QString::fromLatin1(id), "G_OUT").toDouble(), value);
        QCOMPARE(indexValue(index, QString::fromLatin1(id), g), QJsonValue(value));
    }
    QVERIFY(isCachedUnavailable(QStringLiteral("s2"), "G_OUT"));

    state = col("G_OUT");
    QCOMPARE(state.wantedCount, 3);
    QCOMPARE(state.doneCount, 3);
    QVERIFY(state.isPlain());
    for (const char *id : {"s1", "s2", "s3", "s4"})
        QVERIFY2(!isCellPending(QString::fromLatin1(id), "G_OUT"), id);
    QCOMPARE(m_demand->heldSessionIds(), QStringList());
    for (const char *id : {"s1", "s2", "s3", "s4"})
        QVERIFY2(!m_model->isSessionPinned(QString::fromLatin1(id)), id);

    // Once per session, s2 included
    QCOMPARE(loadedSpy.count(), 4);
    for (const char *id : {"s1", "s2", "s3", "s4"})
        QCOMPARE(loadsOf(loadedSpy, id), 1);

    // Ordinary eviction
    if (capacity == 0) {
        for (const char *id : {"s1", "s2", "s3", "s4"})
            QVERIFY2(!isLoaded(QString::fromLatin1(id)), id);
    } else {
        if (capacity >= 4) {
            for (const char *id : {"s1", "s2", "s3", "s4"})
                QVERIFY2(isLoaded(QString::fromLatin1(id)) && !rowState(QString::fromLatin1(id)).visible, id);
        }
        PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 0);
        for (const char *id : {"s1", "s2", "s3", "s4"})
            QVERIFY2(!isLoaded(QString::fromLatin1(id)), id);
    }
    spin();
    QCOMPARE(loadedSpy.count(), 4);
    QVERIFY(!m_demand->hasFillWork());
}

// Sessions in the hidden pool are offered directly, in row order: nothing is
// loaded and nothing is held.
void CalculationDemandTest::loadedHiddenSessionsNeedNoLoad()
{
    for (int i = 1; i <= 4; ++i)
        QVERIFY(giveInput({QStringLiteral("s%1").arg(i)}, "G_IN", i));
    QVERIFY(waitForIdle(*m_model));
    gate().open(4);

    QSignalSpy loadedSpy(m_model.get(), &SessionModel::sessionLoaded);
    QObject scope;
    bool everHeld = false;
    connect(m_queue.get(), &JobQueue::jobsChanged, &scope, [this, &everHeld] {
        everHeld = everHeld || !m_demand->heldSessionIds().isEmpty();
    });
    enableColumns({"G_OUT"});
    QCOMPARE(col("G_OUT").waitingCount, 4);
    QVERIFY(waitDemandIdle());

    QCOMPARE(gate().startOrder(), QList<int>({1, 2, 3, 4}));
    QVERIFY(!everHeld);
    QCOMPARE(m_demand->heldSessionIds(), QStringList());
    QCOMPARE(loadedSpy.count(), 0);
    QCOMPARE(col("G_OUT").doneCount, 4);
    QVERIFY(col("G_OUT").isPlain());
}

// Spec 13: a session made visible while column demand is being worked through
// is the next job; the chosen next job it replaces ends "No longer needed".
void CalculationDemandTest::sessionShownDuringColumnDemandRunsNext()
{
    for (int i = 1; i <= 4; ++i)
        QVERIFY(giveInput({QStringLiteral("s%1").arg(i)}, "G_IN", i));
    QVERIFY(makeStubs());
    QVERIFY(waitForIdle(*m_model));

    QObject scope;
    QString violation;
    watchHolds(&scope, &violation);
    check("g");                 // no session is visible: no plot demand
    enableColumns({"G_OUT"});
    QVERIFY(gate().waitEntered());
    QCOMPARE(running().sessionId, QStringLiteral("s1"));
    QTRY_COMPARE(m_demand->heldSessionIds(), QStringList({"s1", "s2"}));
    m_demand->flush();
    QTRY_COMPARE(chosenNext().sessionId, QStringLiteral("s2"));
    const JobId s2Job = chosenNext().id;

    show({"s4"});
    m_demand->flush();
    QCOMPARE(chosenNext().sessionId, QStringLiteral("s4"));
    QCOMPARE(stateOf(s2Job), JobState::Cancelled);
    QCOMPARE(m_queue->job(s2Job).reason, QString::fromLatin1(kNoLongerNeeded));
    QCOMPARE(m_demand->heldSessionIds(), QStringList({"s1", "s2"}));
    QVERIFY(!m_demand->heldSessionIds().contains(QStringLiteral("s4")));    // shown, not held

    gate().open(4);
    QVERIFY(waitDemandIdle());
    QCOMPARE(gate().startOrder(), QList<int>({1, 4, 2, 3}));
    QVERIFY2(violation.isEmpty(), qPrintable(violation));
    QVERIFY(col("G_OUT").isPlain());
    QCOMPARE(col("G_OUT").doneCount, 4);
}

// Within column demand, loaded sessions are offered in row order and sessions
// that are not loaded join as the fill loads them: a load for an earlier row
// replaces a later chosen next job. Plot demand comes first.
void CalculationDemandTest::columnPriorityFollowsRowOrderAfterPlots()
{
    for (int i = 1; i <= 4; ++i)
        QVERIFY(giveInput({QStringLiteral("s%1").arg(i)}, "G_IN", i));
    QVERIFY(makeStubs());
    session("s2");
    session("s4");                          // loaded, hidden: the pool
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(!isLoaded("s1"));
    QVERIFY(!isLoaded("s3"));

    enableColumns({"G_OUT"});
    m_demand->flush();
    QCOMPARE(chosenNext().sessionId, QStringLiteral("s2"));     // the first loaded candidate
    QVERIFY(gate().waitEntered());
    QCOMPARE(running().sessionId, QStringLiteral("s2"));
    m_demand->flush();
    if (!m_demand->heldSessionIds().contains(QStringLiteral("s1")))
        QCOMPARE(chosenNext().sessionId, QStringLiteral("s4"));
    QTRY_COMPARE(m_demand->heldSessionIds().size(), 2);
    m_demand->flush();
    QCOMPARE(m_demand->heldSessionIds(), QStringList({"s1", "s3"}));
    QCOMPARE(chosenNext().sessionId, QStringLiteral("s1"));
    const JobRecord s4First = jobOf(QStringLiteral("s4"), "gated");
    if (s4First.id != 0) {
        QCOMPARE(s4First.state, JobState::Cancelled);
        QCOMPARE(s4First.reason, QString::fromLatin1(kNoLongerNeeded));
        QVERIFY(!s4First.startedAt.isValid());
    }

    gate().open(4);
    QVERIFY(waitDemandIdle());
    QCOMPARE(gate().startOrder(), QList<int>({2, 1, 3, 4}));

    // Plot demand first: the focused session, then the other visible one,
    // then the column demand of the hidden ones
    check("g");
    for (int i = 1; i <= 4; ++i)
        QVERIFY(giveInput({QStringLiteral("s%1").arg(i)}, "G_IN", 10 + i));
    show({"s3", "s4"});
    m_model->setFocusedSessionId(QStringLiteral("s4"));
    drainEntered();
    settle();
    QVERIFY(gate().waitEntered());
    gate().open(4);
    QVERIFY(waitDemandIdle());
    QCOMPARE(gate().startOrder().mid(4), QList<int>({14, 13, 11, 12}));
}

// Spec 13: within column demand, visible sessions come before hidden loaded
// ones. A chosen next job whose session only changes tier stays chosen.
void CalculationDemandTest::visibleSessionsFirstWithinColumnDemand()
{
    for (int i = 1; i <= 4; ++i)
        QVERIFY(giveInput({QStringLiteral("s%1").arg(i)}, "G_IN", i));
    show({"s3", "s4"});
    QVERIFY(waitForIdle(*m_model));

    enableColumns({"G_OUT"});
    QVERIFY(gate().waitEntered());
    QCOMPARE(running().sessionId, QStringLiteral("s3"));
    m_demand->flush();
    QCOMPARE(chosenNext().sessionId, QStringLiteral("s4"));
    gate().open(4);
    QVERIFY(waitDemandIdle());
    QCOMPARE(gate().startOrder(), QList<int>({3, 4, 1, 2}));
    QCOMPARE(m_demand->heldSessionIds(), QStringList());

    // Only s3 and s4 in demand again: hiding s4 while s3 runs keeps its
    // chosen next job, which is still the first candidate
    drainEntered();
    QVERIFY(giveInput({"s3"}, "G_IN", 13));
    QVERIFY(giveInput({"s4"}, "G_IN", 14));
    settle();
    QVERIFY(gate().waitEntered());
    m_demand->flush();
    QCOMPARE(running().sessionId, QStringLiteral("s3"));
    const JobId s4Job = chosenNext().id;
    QCOMPARE(m_queue->job(s4Job).sessionId, QStringLiteral("s4"));
    show({"s4"}, false);
    m_demand->flush();
    QCOMPARE(m_queue->chosenNextJob(), s4Job);
    QCOMPARE(stateOf(s4Job), JobState::Queued);
    gate().open(2);
    QVERIFY(waitDemandIdle());
    QCOMPARE(stateOf(s4Job), JobState::Succeeded);
    QCOMPARE(gate().startOrder().mid(4), QList<int>({13, 14}));
}

// Spec 13: a column over a chain of requested calculations whose upstream
// record alone is stored is completed: the cell is in demand, and only the
// downstream link runs.
void CalculationDemandTest::chainedColumnWithUpstreamRecordIsCompleted()
{
    QVERIFY(giveInput({"s1"}, "G_IN", 4));
    gate().open(1);
    QCOMPARE(engine("s1").request(QStringLiteral("gated")).status, ResultStatus::Ok);
    QVERIFY(stored("s1", "gated"));
    QVERIFY(!stored("s1", "afterG"));
    QVERIFY(makeStubs());
    QVERIFY(waitForIdle(*m_model));

    enableColumns({"H_OUT"});
    DemandState state = col("H_OUT");
    QCOMPARE(state.waitingCount, 4);
    QCOMPARE(state.doneCount, 0);
    QVERIFY(isCellPending(QStringLiteral("s1"), "H_OUT"));

    QVERIFY(waitDemandIdle());
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(jobCount("gated"), 0);
    QCOMPARE(jobCount("afterG"), 1);
    QCOMPARE(jobOf(QStringLiteral("s1"), "afterG").state, JobState::Succeeded);
    QVERIFY(stored("s1", "gated"));
    QVERIFY(stored("s1", "afterG"));
    QCOMPARE(cachedValue(QStringLiteral("s1"), "H_OUT").toInt(), 6);
    state = col("H_OUT");
    QVERIFY(state.isPlain());
    QCOMPARE(state.wantedCount, 1);
    QCOMPARE(state.doneCount, 1);
}

// Spec 13: a stored rejection of a session that is not loaded is listed as
// failed with its reason, in this run and after a restart, without a load.
void CalculationDemandTest::storedRejectionIsBadgedAfterRestartWithoutLoad()
{
    QVERIFY(giveInput({"s2"}, "EA_IN", -1));
    enableColumns({"EA1"});
    QVERIFY(waitDemandIdle());
    QCOMPARE(jobOf(QStringLiteral("s2"), "expA").state, JobState::Succeeded);
    QVERIFY(stored("s2", "expA"));
    QCOMPARE(LogbookManager::instance().calculationRecordReason(QStringLiteral("s2"), QStringLiteral("expA")),
             QStringLiteral("negative input"));

    const auto verifyBadged = [this] {
        const DemandState state = col("EA1");
        QCOMPARE(sessionIdsOf(state.failed), QStringList({"s2"}));
        QCOMPARE(state.failed.at(0).reason, QStringLiteral("Explicit A: negative input"));
        QCOMPARE(state.failed.at(0).calculationTitles, QStringList({"Explicit A"}));
        QVERIFY(!state.failed.at(0).jobFailure);
        QCOMPARE(state.failed.at(0).sessionName, QStringLiteral("Jump 2"));
        QVERIFY(!isCellPending(QStringLiteral("s2"), "EA1"));
    };

    // Evicted, and a new demand layer: nothing is remembered, the record's
    // reason is known
    QVERIFY(makeStubs());
    QVERIFY(waitForIdle(*m_model));
    restartDemand();
    {
        const Quiet quiet(*m_queue);
        QSignalSpy loadedSpy(m_model.get(), &SessionModel::sessionLoaded);
        verifyBadged();
        if (QTest::currentTestFailed())
            return;
        QVERIFY(waitDemandIdle());     // s1, s3 and s4 are loaded to find they do not apply
        verifyBadged();
        if (QTest::currentTestFailed())
            return;
        QVERIFY(col("EA1").showsWarning());
        QCOMPARE(loadsOf(loadedSpy, "s2"), 0);
        QVERIFY(quiet.holds());
    }

    // A full restart: from index.json's "recordReasons"
    QVERIFY(waitForIdle(*m_model));
    restartApplication();
    QVERIFY(!isLoaded("s2"));
    {
        const Quiet quiet(*m_queue);
        QSignalSpy loadedSpy(m_model.get(), &SessionModel::sessionLoaded);
        verifyBadged();
        if (QTest::currentTestFailed())
            return;
        QVERIFY(waitDemandIdle());
        verifyBadged();
        if (QTest::currentTestFailed())
            return;
        QCOMPARE(loadsOf(loadedSpy, "s2"), 0);
        QVERIFY(quiet.holds());
    }

    // An index without "recordReasons" (an earlier build): Done until the
    // column worker's copy restores the record, after which the reason is in
    // the index and the next pass lists s2 as failed
    QVERIFY(waitForIdle(*m_model));
    m_model->flushDirtySessions();
    m_demand.reset();
    m_queue->shutdown();
    m_queue.reset();
    m_model.reset();
    QJsonObject root = readIndex();
    QJsonObject sessions = root[QStringLiteral("sessions")].toObject();
    QJsonObject entry = sessions[QStringLiteral("s2")].toObject();
    QVERIFY(entry.contains(QStringLiteral("recordReasons")));
    entry.remove(QStringLiteral("recordReasons"));
    // ... and without the value over the record, so that the worker restores it
    QJsonObject values = entry[QStringLiteral("values")].toObject();
    values.remove(indexColumnId(root, attributeColumn(QStringLiteral("EA1"))));
    entry[QStringLiteral("values")] = values;
    sessions[QStringLiteral("s2")] = entry;
    root[QStringLiteral("sessions")] = sessions;
    QVERIFY(writeIndex(root));
    TestEnvironment::instance().reopenLogbook();
    LogbookManager &logbook = LogbookManager::instance();
    logbook.initialize();
    QCOMPARE(logbook.calculationRecordReason(QStringLiteral("s2"), QStringLiteral("expA")), QString());
    m_model = std::make_unique<SessionModel>();
    m_model->populateFromIndex(logbook.cachedColumnValues(LogbookColumnStore::instance().enabledColumns()),
                               logbook.lastAccessedMap());
    m_queue = std::make_unique<JobQueue>(m_model.get());
    m_demand = std::make_unique<CalculationDemand>(m_model.get(), m_plots.get(), m_queue.get());
    {
        const Quiet quiet(*m_queue);
        QSignalSpy loadedSpy(m_model.get(), &SessionModel::sessionLoaded);
        DemandState state = col("EA1");
        QCOMPARE(state.failedCount, 0);
        QCOMPARE(state.doneCount, 1);
        m_model->startColumnWorker();
        QTRY_COMPARE(logbook.calculationRecordReason(QStringLiteral("s2"), QStringLiteral("expA")),
                     QStringLiteral("negative input"));
        QVERIFY(waitDemandIdle());
        verifyBadged();
        if (QTest::currentTestFailed())
            return;
        QCOMPARE(loadsOf(loadedSpy, "s2"), 0);
        QVERIFY(quiet.holds());
    }
}

// Spec 13: the progress line reports the fill for its whole duration, and the
// scheduler rests while the fill waits on a job with both holds taken.
void CalculationDemandTest::fillTaskReportsProgressWhileWaiting()
{
    for (int i = 1; i <= 4; ++i)
        QVERIFY(giveInput({QStringLiteral("s%1").arg(i)}, "G_IN", i));
    QVERIFY(makeStubs());
    QVERIFY(waitForIdle(*m_model));

    IdleScheduler &scheduler = m_model->scheduler();
    QList<QPair<int, int>> fillProgress;      // (remaining, total) of ColumnFillTask
    QStringList events;                         // "P r/t", "I" (idle), "L <id>"
    QObject scope;
    connect(&scheduler, &IdleScheduler::progressChanged, &scope, [&](int id, int remaining, int total) {
        if (id != SessionModel::ColumnFillTask)
            return;
        fillProgress.append({remaining, total});
        events.append(QStringLiteral("P %1/%2").arg(remaining).arg(total));
    });
    connect(&scheduler, &IdleScheduler::schedulerIdle, &scope, [&] { events.append(QStringLiteral("I")); });
    connect(m_model.get(), &SessionModel::sessionLoaded, &scope,
            [&](const QString &id) { events.append(QStringLiteral("L ") + id); });

    enableColumns({"G_OUT"});
    QVERIFY(gate().waitEntered());
    QTRY_COMPARE(m_demand->heldSessionIds().size(), 2);
    QTRY_VERIFY(!scheduler.isTicking());
    QVERIFY(fillProgress.contains(progressOf(4, 4)));
    QVERIFY(m_demand->hasFillWork());
    QVERIFY(!m_demand->canLoad());

    // Resting: no tick, no idle, no load
    const int eventsBefore = int(events.size());
    const int progressBefore = int(fillProgress.size());
    QTest::qWait(200);
    QVERIFY(!scheduler.isTicking());
    QCOMPARE(int(events.size()), eventsBefore);
    QCOMPARE(int(fillProgress.size()), progressBefore);

    // A wake reports once and rests again
    scheduler.wake();
    QTRY_VERIFY(!scheduler.isTicking());
    QTest::qWait(50);
    QCOMPARE(int(fillProgress.size()), progressBefore + 1);
    QCOMPARE(fillProgress.last(), progressOf(4, 4));

    // One job ends: its release wakes the scheduler, which reports and loads
    gate().open(1);
    QTRY_VERIFY(fillProgress.contains(progressOf(3, 4)));
    QTRY_VERIFY(events.contains(QStringLiteral("L s3")));
    QVERIFY(!events.contains(QStringLiteral("I")));

    gate().open(3);
    QVERIFY(waitDemandIdle());
    QTRY_VERIFY(events.contains(QStringLiteral("I")));
    const int end = int(events.lastIndexOf(QStringLiteral("P 0/4")));
    QVERIFY(end >= 0);
    QCOMPARE(events.indexOf(QStringLiteral("I"), end), end + 1);
    QVERIFY(!events.mid(0, end).contains(QStringLiteral("I")));
    QCOMPARE(events.count(QStringLiteral("L s1")) + events.count(QStringLiteral("L s2"))
                 + events.count(QStringLiteral("L s3")) + events.count(QStringLiteral("L s4")), 4);
    // The remaining count never rose within the fill, and the total was 4 throughout
    for (int i = 0; i < fillProgress.size(); ++i) {
        QCOMPARE(fillProgress.at(i).second, 4);
        if (i > 0)
            QVERIFY(fillProgress.at(i).first <= fillProgress.at(i - 1).first);
    }

    // The next fill starts its own count. Its job is held, so the fill waits on
    // it and must report. (A job that is let through at once may end before
    // the fill's first tick, because the save of the edit takes the ticks
    // before it; that fill then reports only its end, "0 / 1".)
    QCOMPARE(gate().proceed.available(), 0);
    fillProgress.clear();
    QVERIFY(giveInput({"s2"}, "G_IN", 20));
    settle();
    QTRY_VERIFY(!fillProgress.isEmpty());
    QCOMPARE(fillProgress.first(), progressOf(1, 1));
    gate().open(1);
    QVERIFY(waitDemandIdle());
    QCOMPARE(fillProgress.last(), progressOf(0, 1));
    for (const QPair<int, int> &progress : std::as_const(fillProgress))
        QCOMPARE(progress.second, 1);
}

// Spec 13: stored results are restored, not recomputed: enabling the column on
// a logbook whose sessions all have stored results creates no job and loads
// nothing, now and after a restart.
void CalculationDemandTest::storedResultsCreateNoJob()
{
    for (int i = 1; i <= 4; ++i)
        QVERIFY(giveInput({QStringLiteral("s%1").arg(i)}, "G_IN", i));
    gate().open(4);
    for (int i = 1; i <= 4; ++i)
        QCOMPARE(engine(QStringLiteral("s%1").arg(i)).request(QStringLiteral("gated")).status, ResultStatus::Ok);
    for (const char *id : {"s1", "s2", "s3", "s4"})
        QVERIFY2(stored(QString::fromLatin1(id), "gated"), id);
    QVERIFY(makeStubs());
    QVERIFY(waitForIdle(*m_model));

    const auto verifyNothingRuns = [this] {
        const Quiet quiet(*m_queue);
        QSignalSpy loadedSpy(m_model.get(), &SessionModel::sessionLoaded);
        DemandState state = col("G_OUT");
        QCOMPARE(state.doneCount, 4);
        QCOMPARE(state.wantedCount, 4);
        QVERIFY(state.isPlain());
        QVERIFY(!m_demand->hasFillWork());
        spin();
        QVERIFY(waitForIdle(*m_model));     // the worker's copies may read the records
        spin();
        QCOMPARE(loadedSpy.count(), 0);
        QVERIFY(quiet.holds());
        QVERIFY(col("G_OUT").isPlain());
    };

    enableColumns({"G_OUT"});
    verifyNothingRuns();
    if (QTest::currentTestFailed())
        return;

    restartApplication();
    verifyNothingRuns();
    if (QTest::currentTestFailed())
        return;

    // A loaded session with its record restored reads Done as well
    session("s1");
    QCOMPARE(col("G_OUT").doneCount, 4);
    QVERIFY(col("G_OUT").isPlain());
    QCOMPARE(m_queue->model()->rowCount(), 0);
}

// A session whose calculation turns out not to apply once loaded is settled
// without a job and not loaded again in the run.
void CalculationDemandTest::notApplicableSessionIsSettledWithoutAJob()
{
    QVERIFY(makeStubs());
    QVERIFY(waitForIdle(*m_model));
    QSignalSpy loadedSpy(m_model.get(), &SessionModel::sessionLoaded);
    const Quiet quiet(*m_queue);

    enableColumns({"G_OUT"});
    QVERIFY(isCellPending(QStringLiteral("s2"), "G_OUT"));
    QVERIFY(waitDemandIdle());
    QCOMPARE(loadsOf(loadedSpy, "s2"), 1);
    QVERIFY(!isCellPending(QStringLiteral("s2"), "G_OUT"));
    QCOMPARE(m_demand->heldSessionIds(), QStringList());
    QVERIFY(!m_model->isSessionPinned(QStringLiteral("s2")));
    DemandState state = col("G_OUT");
    QCOMPARE(state.wantedCount, 0);
    QVERIFY(state.isPlain());

    // Evicted: the settlement keeps it from being loaded again
    QVERIFY(makeStubs());
    for (int i = 0; i < 3; ++i)
        spin();
    QCOMPARE(loadsOf(loadedSpy, "s2"), 1);
    QVERIFY(!m_demand->hasFillWork());
    QCOMPARE(col("G_OUT").wantedCount, 0);
    QVERIFY(quiet.holds());
}

// A Failed-status result (not stored) and a stored rejection are badged, and
// neither session is loaded again after its eviction.
void CalculationDemandTest::columnFailuresAreBadgedNotReloaded()
{
    QVERIFY(giveInput({"s1"}, "T_IN", 1));
    QVERIFY(giveInput({"s2"}, "EA_IN", -1));
    QVERIFY(makeStubs());
    QVERIFY(waitForIdle(*m_model));
    QSignalSpy loadedSpy(m_model.get(), &SessionModel::sessionLoaded);

    enableColumns({"T_OUT", "EA1"});
    QVERIFY(waitDemandIdle());
    QCOMPARE(jobOf(QStringLiteral("s1"), "thrower").state, JobState::Succeeded);
    QCOMPARE(jobOf(QStringLiteral("s1"), "thrower").resultStatus, std::optional<ResultStatus>(ResultStatus::Failed));
    QVERIFY(!stored("s1", "thrower"));
    QCOMPARE(jobOf(QStringLiteral("s2"), "expA").state, JobState::Succeeded);
    QVERIFY(stored("s2", "expA"));

    const auto verifyBadged = [this] {
        DemandState t = col("T_OUT");
        QCOMPARE(sessionIdsOf(t.failed), QStringList({"s1"}));
        QCOMPARE(t.failed.at(0).reason, QStringLiteral("Thrower: synthetic failure"));
        QCOMPARE(t.failed.at(0).sessionName, QStringLiteral("Jump 1"));
        QVERIFY(t.showsWarning());
        DemandState ea = col("EA1");
        QCOMPARE(sessionIdsOf(ea.failed), QStringList({"s2"}));
        QCOMPARE(ea.failed.at(0).reason, QStringLiteral("Explicit A: negative input"));
        QCOMPARE(ea.failed.at(0).sessionName, QStringLiteral("Jump 2"));
        QVERIFY(ea.showsWarning());
    };
    verifyBadged();
    if (QTest::currentTestFailed())
        return;

    const int jobs = m_queue->model()->rowCount();
    QVERIFY(makeStubs());
    for (int i = 0; i < 3; ++i)
        spin();
    QVERIFY(waitDemandIdle());
    verifyBadged();
    if (QTest::currentTestFailed())
        return;
    QCOMPARE(loadsOf(loadedSpy, "s1"), 1);
    QCOMPARE(loadsOf(loadedSpy, "s2"), 1);
    QCOMPARE(m_queue->model()->rowCount(), jobs);
}

// A job-level failure is badged, the session released and not loaded again
// in the run; the next start (a new demand layer) computes it.
void CalculationDemandTest::columnJobLevelFailureIsNotReloadedUntilRestart()
{
    QVERIFY(giveInput({"s1"}, "X_IN", 4));
    QVERIFY(makeStubs());
    QVERIFY(waitForIdle(*m_model));
    QSignalSpy loadedSpy(m_model.get(), &SessionModel::sessionLoaded);

    enableColumns({"X_OUT"});
    QVERIFY(waitDemandIdle());
    QCOMPARE(jobOf(QStringLiteral("s1"), "exhausted").state, JobState::Failed);
    DemandState state = col("X_OUT");
    QCOMPARE(sessionIdsOf(state.failed), QStringList({"s1"}));
    QVERIFY(state.failed.at(0).jobFailure);
    QCOMPARE(state.failed.at(0).reason, QStringLiteral("Exhausted: Out of memory"));
    QVERIFY(state.showsWarning());
    QCOMPARE(m_demand->heldSessionIds(), QStringList());

    QVERIFY(makeStubs());
    for (int i = 0; i < 3; ++i)
        spin();
    QCOMPARE(loadsOf(loadedSpy, "s1"), 1);
    QCOMPARE(jobCount("exhausted"), 1);
    QVERIFY(col("X_OUT").failed.at(0).jobFailure);

    restartDemand();
    QVERIFY(waitDemandIdle());
    QCOMPARE(loadsOf(loadedSpy, "s1"), 2);
    QCOMPARE(jobOf(QStringLiteral("s1"), "exhausted").state, JobState::Succeeded);
    QVERIFY(stored("s1", "exhausted"));
    state = col("X_OUT");
    QVERIFY(state.isPlain());
    QCOMPARE(state.doneCount, 1);
}

// A column pair the executor refuses at offer time is remembered, and a pass
// follows it: the cell, classified before the offers, is not left pending.
// Engine inspection and readiness agree at any one moment, so the refusal
// needs a column report older than a change no signal announces: a
// preference that loses its value under a calculation that never ran (the
// engine has no edge from it, so nothing is invalidated).
void CalculationDemandTest::columnOfferRefusalIsNotLeftPending()
{
    const QString key = QStringLiteral("test/demand/probePreference");
    PreferencesManager &preferences = PreferencesManager::instance();
    preferences.registerPreference(key, 1.0);
    preferences.setValue(key, 1.0);
    const auto restore = qScopeGuard([key] { PreferencesManager::instance().setValue(key, 1.0); });

    CalculationDescriptor probe;
    probe.id = QStringLiteral("test.demand.pref");
    probe.title = QStringLiteral("Preference probe");
    probe.policy = EvaluationPolicy::Explicit;
    probe.inputs = {CalcInput::attribute(QStringLiteral("PP_IN")), CalcInput::preference(key)};
    probe.outputs = {DependencyKey::attribute(QStringLiteral("PP_OUT"))};
    probe.compute = [](const EvaluationContext &ctx) {
        return CalculationResult().setAttribute(QStringLiteral("PP_OUT"), ctx.attribute(QStringLiteral("PP_IN")).toInt() + 1);
    };
    QVERIFY(m_extra->add(probe));

    // s1, loaded and hidden, waits inside its input-settle wait: its report
    // is inspected (and kept), nothing is offered
    m_demand->setInputSettleDelay(60000);
    enableColumns({"PP_OUT"});
    QVERIFY(giveInput({"s1"}, "PP_IN", 1));
    DemandState state = col("PP_OUT");
    QCOMPARE(state.wantedCount, 1);
    QCOMPARE(state.waitingCount, 1);
    QVERIFY(m_demand->isSettling(QStringLiteral("s1")));
    QVERIFY(isCellPending(QStringLiteral("s1"), "PP_OUT"));

    const Quiet quiet(*m_queue);
    preferences.setValue(key, QVariant());
    QCOMPARE(engine("s1").readiness(QStringLiteral("test.demand.pref")).state,
             CalculationReadiness::State::MissingInput);

    // The wait ends: the pass offers s1's pair and the executor refuses it
    m_demand->endInputSettleWaits();
    m_demand->flush();
    QVERIFY(quiet.holds());
    spin();

    state = col("PP_OUT");
    QCOMPARE(state.waitingCount, 0);
    QCOMPARE(state.wantedCount, 0);
    QVERIFY(state.isPlain());
    QVERIFY(!isCellPending(QStringLiteral("s1"), "PP_OUT"));
    // The fill's last step reports it complete, and then it has no work
    QTRY_VERIFY(!m_demand->hasFillWork());
    QVERIFY(quiet.holds());
}

// A stub whose session file cannot be loaded is settled failed by the load
// step: not held, no job, not loaded again.
void CalculationDemandTest::unloadableSessionIsSettledAsFailed()
{
    for (int i = 1; i <= 4; ++i)
        QVERIFY(giveInput({QStringLiteral("s%1").arg(i)}, "G_IN", i));
    QVERIFY(makeStubs());
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(QFile::remove(sessionFilePath(QStringLiteral("s3"))));
    gate().open(3);

    QSignalSpy loadedSpy(m_model.get(), &SessionModel::sessionLoaded);
    QObject scope;
    bool s3Held = false;
    connect(m_model.get(), &SessionModel::sessionLoaded, &scope, [this, &s3Held](const QString &) {
        s3Held = s3Held || m_demand->heldSessionIds().contains(QStringLiteral("s3"));
    });
    enableColumns({"G_OUT"});
    QVERIFY(waitDemandIdle());

    DemandState state = col("G_OUT");
    QCOMPARE(sessionIdsOf(state.failed), QStringList({"s3"}));
    QVERIFY(state.failed.at(0).jobFailure);
    QCOMPARE(state.failed.at(0).reason, QStringLiteral("The session file could not be loaded"));
    QCOMPARE(state.failed.at(0).sessionName, QStringLiteral("Jump 3"));
    QCOMPARE(state.doneCount, 4);
    QVERIFY(state.showsWarning());
    QVERIFY(!s3Held);
    QVERIFY(!m_demand->heldSessionIds().contains(QStringLiteral("s3")));
    QVERIFY(!m_model->isSessionPinned(QStringLiteral("s3")));
    QCOMPARE(jobOf(QStringLiteral("s3"), "gated").id, JobId(0));
    QCOMPARE(loadsOf(loadedSpy, "s3"), 1);

    QVERIFY(makeStubs());
    for (int i = 0; i < 3; ++i)
        spin();
    QCOMPARE(loadsOf(loadedSpy, "s3"), 1);
    QVERIFY(!m_demand->hasFillWork());
    QCOMPARE(sessionIdsOf(col("G_OUT").failed), QStringList({"s3"}));
}

// A visible failed-load placeholder is settled failed as well: it is never
// pending, and the fill does not keep working for it.
void CalculationDemandTest::visibleFailedLoadIsSettledAsFailed()
{
    for (int i = 1; i <= 4; ++i)
        QVERIFY(giveInput({QStringLiteral("s%1").arg(i)}, "G_IN", i));
    QVERIFY(makeStubs());
    QVERIFY(waitForIdle(*m_model));
    gate().open(3);

    QObject scope;
    bool s2Held = false;
    const auto noteHolds = [this, &s2Held] {
        s2Held = s2Held || m_demand->heldSessionIds().contains(QStringLiteral("s2"));
    };
    connect(m_model.get(), &SessionModel::sessionLoaded, &scope, [noteHolds](const QString &) { noteHolds(); });
    connect(m_queue.get(), &JobQueue::jobsChanged, &scope, noteHolds);

    enableColumns({"G_OUT"});
    QVERIFY(QFile::remove(sessionFilePath(QStringLiteral("s2"))));
    show({"s2"});
    QVERIFY(rowState(QStringLiteral("s2")).isLoaded());
    QVERIFY(rowState(QStringLiteral("s2")).loadFailed);
    QVERIFY(rowState(QStringLiteral("s2")).visible);

    QVERIFY(waitDemandIdle());
    QVERIFY(waitForIdle(*m_model));
    DemandState state = col("G_OUT");
    QCOMPARE(sessionIdsOf(state.failed), QStringList({"s2"}));
    QVERIFY(state.failed.at(0).jobFailure);
    QCOMPARE(state.failed.at(0).reason, QStringLiteral("The session file could not be loaded"));
    QCOMPARE(state.failed.at(0).sessionName, QStringLiteral("Jump 2"));
    QVERIFY(state.toolTip.contains(QStringLiteral("  Jump 2 - The session file could not be loaded")));
    QVERIFY(!m_demand->isCellPending(rowOf(QStringLiteral("s2")), section("G_OUT")));
    QCOMPARE(state.waitingCount, 0);
    QCOMPARE(state.runningCount, 0);
    QVERIFY(!state.isWorking());
    QVERIFY(state.showsWarning());
    QCOMPARE(state.failedCount, 1);
    QCOMPARE(state.doneCount, 4);
    for (const char *id : {"s1", "s3", "s4"})
        QCOMPARE(jobOf(QString::fromLatin1(id), "gated").state, JobState::Succeeded);
    QCOMPARE(jobOf(QStringLiteral("s2"), "gated").id, JobId(0));
    QVERIFY(!s2Held);
    for (int i = 0; i < 3; ++i)
        spin();
    QVERIFY(!m_demand->hasFillWork());

    // Hidden and evicted: still failed, not loaded again
    QSignalSpy loadedSpy(m_model.get(), &SessionModel::sessionLoaded);
    QVERIFY(makeStubs({"s2"}));
    for (int i = 0; i < 3; ++i)
        spin();
    QCOMPARE(loadsOf(loadedSpy, "s2"), 0);
    QCOMPARE(sessionIdsOf(col("G_OUT").failed), QStringList({"s2"}));
    QVERIFY(!m_demand->hasFillWork());
}

// A chain keeps its hold from the load to the end of its last link.
void CalculationDemandTest::chainedColumnKeepsItsHold()
{
    QVERIFY(giveInput({"s1"}, "G_IN", 4));
    QVERIFY(makeStubs());
    session("s2");
    session("s3");
    session("s4");                          // in the pool: not held
    QVERIFY(waitForIdle(*m_model));
    gate().open(1);

    QSignalSpy loadedSpy(m_model.get(), &SessionModel::sessionLoaded);
    QObject scope;
    QStringList heldAtEnds;                 // "<calculation>:<held>" at each job's end
    connect(m_queue.get(), &JobQueue::jobFinished, &scope, [this, &heldAtEnds](JobId id, JobState) {
        heldAtEnds.append(m_queue->job(id).calculationId + QLatin1Char(':')
                          + m_demand->heldSessionIds().join(QLatin1Char(',')));
    });
    QStringList heldAtStarts;
    connect(m_queue.get(), &JobQueue::jobStarted, &scope, [this, &heldAtStarts](JobId id) {
        heldAtStarts.append(m_queue->job(id).calculationId + QLatin1Char(':')
                            + m_demand->heldSessionIds().join(QLatin1Char(',')));
    });

    enableColumns({"H_OUT"});
    QVERIFY(waitDemandIdle());
    QVERIFY(waitForIdle(*m_model));

    QCOMPARE(loadedSpy.count(), 1);
    QCOMPARE(loadsOf(loadedSpy, "s1"), 1);
    QCOMPARE(heldAtStarts, QStringList({"gated:s1", "afterG:s1"}));
    // The demand layer's own slot runs first: after gated, the cell still waits
    // for afterG and the hold stays; after afterG it is released
    QCOMPARE(heldAtEnds, QStringList({"gated:s1", "afterG:"}));
    QCOMPARE(jobCount("gated"), 1);
    QCOMPARE(jobCount("afterG"), 1);
    QVERIFY(stored("s1", "gated"));
    QVERIFY(stored("s1", "afterG"));
    QCOMPARE(cachedValue(QStringLiteral("s1"), "H_OUT").toInt(), 6);
    QCOMPARE(m_demand->heldSessionIds(), QStringList());
}

// Disabling the column drops the chosen next job and releases the holds; the
// running job finishes and is stored.
void CalculationDemandTest::disablingColumnReleasesHeldSessions()
{
    m_capacity = 0;
    for (int i = 1; i <= 4; ++i)
        QVERIFY(giveInput({QStringLiteral("s%1").arg(i)}, "G_IN", i));
    QVERIFY(makeStubs());
    QVERIFY(waitForIdle(*m_model));

    enableColumns({"G_OUT"});
    QVERIFY(gate().waitEntered());
    QTRY_COMPARE(m_demand->heldSessionIds(), QStringList({"s1", "s2"}));
    m_demand->flush();
    QTRY_COMPARE(chosenNext().sessionId, QStringLiteral("s2"));
    const JobId s1Job = running().id;
    const JobId s2Job = chosenNext().id;

    enableColumns({});
    m_demand->flush();
    QCOMPARE(stateOf(s2Job), JobState::Cancelled);
    QCOMPARE(m_queue->job(s2Job).reason, QString::fromLatin1(kNoLongerNeeded));
    QCOMPARE(m_demand->heldSessionIds(), QStringList());
    QVERIFY(!m_model->isSessionPinned(QStringLiteral("s2")));
    QVERIFY(m_model->isSessionPinned(QStringLiteral("s1")));       // the executor's pin
    QCOMPARE(stateOf(s1Job), JobState::Running);
    QVERIFY(!m_queue->job(s1Job).cancelRequested);

    gate().open(1);
    QTRY_COMPARE(stateOf(s1Job), JobState::Succeeded);
    QVERIFY(stored("s1", "gated"));
    QTRY_VERIFY(!m_model->isSessionPinned(QStringLiteral("s1")));
    spin();
    QVERIFY(!isLoaded("s1"));
    QVERIFY(!isLoaded("s2"));
    QVERIFY(!m_demand->hasFillWork());
    QCOMPARE(m_queue->model()->rowCount(), 2);
}

// A held session that is shown stays loaded after its release: it is visible.
void CalculationDemandTest::heldSessionShownStaysLoaded()
{
    m_capacity = 0;
    QVERIFY(giveInput({"s1"}, "G_IN", 1));
    QVERIFY(makeStubs());
    QVERIFY(waitForIdle(*m_model));

    enableColumns({"G_OUT"});
    QVERIFY(gate().waitEntered());
    QCOMPARE(running().sessionId, QStringLiteral("s1"));
    QVERIFY(m_demand->heldSessionIds().contains(QStringLiteral("s1")));
    show({"s1"});
    gate().open(1);
    QVERIFY(waitDemandIdle());
    QVERIFY(!m_demand->heldSessionIds().contains(QStringLiteral("s1")));
    QVERIFY(!m_model->isSessionPinned(QStringLiteral("s1")));
    spin();
    QVERIFY(isLoaded("s1"));
    QVERIFY(rowState(QStringLiteral("s1")).visible);

    show({"s1"}, false);
    spin();
    QVERIFY(!isLoaded("s1"));
}

// A held session whose row is removed, or that a repopulation turns back into
// a stub, is released at once.
void CalculationDemandTest::removedOrRepopulatedHeldSessionIsReleased()
{
    for (int i = 1; i <= 4; ++i)
        QVERIFY(giveInput({QStringLiteral("s%1").arg(i)}, "G_IN", i));
    QVERIFY(makeStubs());
    QVERIFY(waitForIdle(*m_model));

    enableColumns({"G_OUT"});
    QVERIFY(gate().waitEntered());
    QTRY_COMPARE(m_demand->heldSessionIds(), QStringList({"s1", "s2"}));
    const JobId s1Job = running().id;

    QVERIFY(m_model->removeSessions({"s2"}));
    m_demand->flush();
    QVERIFY(!m_demand->heldSessionIds().contains(QStringLiteral("s2")));
    QVERIFY(!m_model->isSessionPinned(QStringLiteral("s2")));
    QCOMPARE(m_demand->heldSessionIds(), QStringList({"s1"}));

    LogbookManager &logbook = LogbookManager::instance();
    m_model->populateFromIndex(logbook.cachedColumnValues(LogbookColumnStore::instance().enabledColumns()),
                               logbook.lastAccessedMap());
    m_demand->flush();
    QVERIFY(!m_demand->heldSessionIds().contains(QStringLiteral("s1")));
    // No more loads: the executor's end of the running job is what is left
    enableColumns({});
    m_demand->flush();
    QCOMPARE(m_demand->heldSessionIds(), QStringList());
    QTRY_COMPARE(stateOf(s1Job), JobState::Superseded);
    QTRY_VERIFY(!m_model->isSessionPinned(QStringLiteral("s1")));
    QVERIFY(!m_model->isSessionPinned(QStringLiteral("s2")));
}

// A row known by its file stem (a deferred scan) is loaded, corrected, held and
// offered under its real id only; nothing is pinned under the stem.
void CalculationDemandTest::identityStubIsOfferedUnderItsRealId()
{
    for (int i = 1; i <= 4; ++i)
        QVERIFY(giveInput({QStringLiteral("s%1").arg(i)}, "G_IN", i));
    QVERIFY(waitForIdle(*m_model));
    m_model->flushDirtySessions();
    m_demand.reset();
    m_queue->shutdown();
    m_queue.reset();
    m_model.reset();

    TestEnvironment &env = TestEnvironment::instance();
    QVERIFY(QFile::remove(env.indexPath()));
    env.reopenLogbook();
    LogbookManager &logbook = LogbookManager::instance();
    logbook.initialize();
    QVERIFY(logbook.hasDeferredScan());
    m_model = std::make_unique<SessionModel>();
    m_model->populateFromUuids(logbook.scannedUuids());
    m_queue = std::make_unique<JobQueue>(m_model.get());
    restartDemand();
    enableColumns({"G_OUT"});
    m_demand->flush();

    // Before any event-loop turn: the column worker has not remapped the row
    const QString stem = std::as_const(*m_model).rowAt(0).sessionId;
    QVERIFY(logbook.isIdentityEntry(stem));
    QVERIFY(m_demand->canLoad());
    m_demand->runLoadStep();
    const QString realId = m_model->rowAt(0).sessionId;
    QVERIFY(QStringList({"s1", "s2", "s3", "s4"}).contains(realId));
    QVERIFY(realId != stem);
    QCOMPARE(m_demand->heldSessionIds(), QStringList({realId}));
    QVERIFY(m_model->isSessionPinned(realId));
    QVERIFY(!m_model->isSessionPinned(stem));

    gate().open(4);
    QVERIFY(waitDemandIdle());
    QCOMPARE(jobOf(realId, "gated").state, JobState::Succeeded);
    QVERIFY(stored(realId, "gated"));
    for (const JobRecord &record : m_queue->model()->records())
        QVERIFY2(QStringList({"s1", "s2", "s3", "s4"}).contains(record.sessionId), qPrintable(record.sessionId));
    QVERIFY(!m_model->isSessionPinned(stem));
}

// The per-column state and the pending cells while a column fills.
void CalculationDemandTest::columnStateCountsAndPendingCells()
{
    QVERIFY(giveInput({"s1"}, "G_IN", 1));
    QVERIFY(giveInput({"s3"}, "G_IN", 3));
    QVERIFY(giveInput({"s4"}, "G_IN", 4));
    QVERIFY(makeStubs());
    QVERIFY(waitForIdle(*m_model));

    QSignalSpy columnSpy(m_demand.get(), &CalculationDemand::columnStateChanged);
    enableColumns({"G_OUT"});
    DemandState state = col("G_OUT");
    QCOMPARE(state.wantedCount, 4);
    QCOMPARE(state.waitingCount, 4);
    QCOMPARE(state.progressLabel, QStringLiteral("0 of 4"));

    QVERIFY(gate().waitEntered());
    QTRY_COMPARE(col("G_OUT").running.value(0).progressText, QStringLiteral("step 1"));
    state = col("G_OUT");
    QCOMPARE(state.runningCount, 1);
    QCOMPARE(sessionIdsOf(state.running), QStringList({"s1"}));
    QCOMPARE(state.running.at(0).sessionName, QStringLiteral("Jump 1"));
    QVERIFY(state.running.at(0).job != 0);
    QVERIFY(state.toolTip.startsWith(QStringLiteral("Computing: 0 of ")));
    QVERIFY(state.toolTip.contains(QStringLiteral("  Jump 1 - Gated: step 1")));
    QCOMPARE(state.waitingCount, state.wantedCount - 1);
    QVERIFY(state.wantedCount == 4 || state.wantedCount == 3);
    QVERIFY(state.waiting.isEmpty());
    QVERIFY(isCellPending(QStringLiteral("s1"), "G_OUT"));
    QVERIFY(isCachedUnavailable(QStringLiteral("s1"), "G_OUT"));

    // s2 is loaded and found not applicable while s1 runs
    QTRY_COMPARE(col("G_OUT").wantedCount, 3);
    QVERIFY(!isCellPending(QStringLiteral("s2"), "G_OUT"));

    gate().open(3);
    QVERIFY(waitDemandIdle());
    state = col("G_OUT");
    QVERIFY(state.progressLabel.isEmpty());
    QVERIFY(state.isPlain());
    QCOMPARE(state.doneCount, 3);
    for (const QList<QVariant> &arguments : std::as_const(columnSpy))
        QCOMPARE(arguments.at(0).toString(), colId("G_OUT"));
    QVERIFY(!columnSpy.isEmpty());

    // A pass that changes nothing announces nothing
    columnSpy.clear();
    check("plain");
    m_demand->flush();
    check("plain", false);
    m_demand->flush();
    QCOMPARE(columnSpy.count(), 0);
}

// Spec 13: a profile's column list (applyProfile step 7: a new list, some
// columns disabled, the requested one enabled) creates the same demand.
void CalculationDemandTest::profileStyleColumnsCreateDemand()
{
    for (int i = 1; i <= 4; ++i)
        QVERIFY(giveInput({QStringLiteral("s%1").arg(i)}, "G_IN", i));
    QVERIFY(makeStubs());
    QVERIFY(waitForIdle(*m_model));
    gate().open(4);

    LogbookColumn exitTime = exitTimeColumn();
    exitTime.enabled = false;
    LogbookColumn g = attributeColumn(QStringLiteral("G_OUT"));
    g.enabled = true;
    g.customLabel = QStringLiteral("G out");
    LogbookColumnStore::instance().setColumns({descriptionColumn(), exitTime, g});

    QCOMPARE(col("G_OUT").waitingCount, 4);
    QVERIFY(waitDemandIdle());
    for (const char *id : {"s1", "s2", "s3", "s4"}) {
        QCOMPARE(jobOf(QString::fromLatin1(id), "gated").state, JobState::Succeeded);
        QVERIFY2(stored(QString::fromLatin1(id), "gated"), id);
    }
    QCOMPARE(m_queue->model()->rowCount(), 4);
    QVERIFY(col("G_OUT").isPlain());
}

// At start-up with an enabled column over a requested calculation, demand
// exists at once, and the fill's loads wait for the column worker's pass.
void CalculationDemandTest::startupWithEnabledColumnLoadsAfterColumnWorker()
{
    for (int i = 1; i <= 4; ++i)
        QVERIFY(giveInput({QStringLiteral("s%1").arg(i)}, "G_IN", i));
    m_demand.reset();                       // nothing runs before the restart
    enableColumns({"G_OUT"});
    QVERIFY(waitForIdle(*m_model));
    m_model->flushDirtySessions();
    m_queue->shutdown();
    m_queue.reset();
    m_model.reset();

    // Some cached values are gone: the column worker has a start-up pass
    QJsonObject root = readIndex();
    QJsonObject sessions = root[QStringLiteral("sessions")].toObject();
    const QString descriptionId = indexColumnId(root, descriptionColumn());
    for (const char *id : {"s2", "s4"}) {
        QJsonObject entry = sessions[QString::fromLatin1(id)].toObject();
        QJsonObject values = entry[QStringLiteral("values")].toObject();
        QVERIFY(values.contains(descriptionId));
        values.remove(descriptionId);
        entry[QStringLiteral("values")] = values;
        sessions[QString::fromLatin1(id)] = entry;
    }
    root[QStringLiteral("sessions")] = sessions;
    QVERIFY(writeIndex(root));

    TestEnvironment::instance().reopenLogbook();
    LogbookManager &logbook = LogbookManager::instance();
    logbook.initialize();
    m_model = std::make_unique<SessionModel>();
    m_model->populateFromIndex(logbook.cachedColumnValues(LogbookColumnStore::instance().enabledColumns()),
                               logbook.lastAccessedMap());
    m_queue = std::make_unique<JobQueue>(m_model.get());

    // As MainWindow: the column worker is started, then the demand layer made
    QStringList order;                      // "C" a column worker step, "F" the fill activated, "L" a load
    bool passDoneAtFill = false;            // at the fill's first activation
    int copiesAtFill = -1;
    QObject scope;
    connect(&m_model->scheduler(), &IdleScheduler::progressChanged, &scope, [&order](int id, int, int) {
        if (id == SessionModel::ColumnTask)
            order.append(QStringLiteral("C"));
    });
    connect(&m_model->scheduler(), &IdleScheduler::activeTaskChanged, &scope, [&, this](int id, bool) {
        if (id != SessionModel::ColumnFillTask)
            return;
        if (!order.contains(QStringLiteral("F"))) {
            const int description = section("_DESCRIPTION");
            passDoneAtFill = rowState(QStringLiteral("s2")).cachedValues.contains(description)
                && rowState(QStringLiteral("s4")).cachedValues.contains(description);
            copiesAtFill = m_model->columnWorkStats().sessionsLoaded;
        }
        order.append(QStringLiteral("F"));
    });
    connect(m_model.get(), &SessionModel::sessionLoaded, &scope,
            [&order](const QString &) { order.append(QStringLiteral("L")); });
    m_model->startColumnWorker();
    m_demand = std::make_unique<CalculationDemand>(m_model.get(), m_plots.get(), m_queue.get());

    const DemandState state = col("G_OUT");
    QVERIFY(state.requested);
    QCOMPARE(state.waitingCount, 4);
    QVERIFY(order.isEmpty());

    gate().open(4);
    QVERIFY(waitDemandIdle());
    QVERIFY(order.contains(QStringLiteral("C")));
    const int firstFill = int(order.indexOf(QStringLiteral("F")));
    const int firstLoad = int(order.indexOf(QStringLiteral("L")));
    QVERIFY(firstFill >= 0);
    QVERIFY(firstLoad > firstFill);
    // The start-up pass was complete when the fill was first activated: its
    // values are cached, from the temporary copies of s2 and s4 ...
    QVERIFY(passDoneAtFill);
    QCOMPARE(copiesAtFill, 2);
    // ... and the column worker made no temporary copy afterwards. It may step
    // again once a load has happened: a job's record drops the loaded row's
    // cached value, and the worker computes it when its tick comes before the
    // queued loaded-row refresh (the event loop's order decides).
    QCOMPARE(m_model->columnWorkStats().sessionsLoaded, copiesAtFill);
    const int laterColumnStep = int(order.indexOf(QStringLiteral("C"), firstFill));
    QVERIFY(laterColumnStep < 0 || laterColumnStep > firstLoad);
    QCOMPARE(order.count(QStringLiteral("L")), 4);
    QCOMPARE(m_queue->model()->rowCount(), 4);
}

namespace {

/// test.demand.desc: DESC_OUT = "x:" + _DESCRIPTION, explicit.
CalculationDescriptor descriptionCalculation()
{
    CalculationDescriptor d;
    d.id = QStringLiteral("test.demand.desc");
    d.title = QStringLiteral("Description");
    d.policy = EvaluationPolicy::Explicit;
    d.inputs = {CalcInput::attribute(QString::fromLatin1(SessionKeys::Description))};
    d.outputs = {DependencyKey::attribute(QStringLiteral("DESC_OUT"))};
    d.compute = [](const EvaluationContext &ctx) {
        return CalculationResult().setAttribute(
            QStringLiteral("DESC_OUT"),
            QStringLiteral("x:") + ctx.attribute(QString::fromLatin1(SessionKeys::Description)).toString());
    };
    return d;
}

} // namespace

// Spec 13: saves and bulk edits precede the fill's loads, so no result is
// computed from a file that is about to be rewritten.
void CalculationDemandTest::savesAndBulkEditsPrecedeLoadStep()
{
    QVERIFY(m_extra->add(descriptionCalculation()));
    m_demand->setInputSettleDelay(0);
    QVERIFY(makeStubs());
    QVERIFY(waitForIdle(*m_model));

    QStringList order;              // "S"/"B" save/bulk-edit progress, "F" the fill activated
    QStringList violations;
    QObject scope;
    connect(&m_model->scheduler(), &IdleScheduler::progressChanged, &scope, [&order](int id, int, int) {
        if (id == SessionModel::SaveTask)
            order.append(QStringLiteral("S"));
        else if (id == SessionModel::BulkEditTask)
            order.append(QStringLiteral("B"));
    });
    connect(&m_model->scheduler(), &IdleScheduler::activeTaskChanged, &scope, [&order](int id, bool cancellable) {
        if (id == SessionModel::ColumnFillTask)
            order.append(cancellable ? QStringLiteral("F!") : QStringLiteral("F"));
    });
    const QByteArray bulkLine = "$VAR,_DESCRIPTION,bulk";
    connect(m_queue.get(), &JobQueue::jobQueued, &scope, [this, &violations, bulkLine](JobId id) {
        const QString sessionId = m_queue->job(id).sessionId;
        if (sessionId != QLatin1String("s1") && !fileHas(sessionId, bulkLine))
            violations.append(QStringLiteral("job queued for ") + sessionId + QStringLiteral(" before its bulk edit"));
    });

    // In one turn: the column, a bulk edit of every row, and an edit that
    // loads s1 and makes it dirty
    enableColumns({"DESC_OUT"});
    QList<int> rows;
    for (int r = 0; r < m_model->rowCount(); ++r)
        rows.append(r);
    m_model->startBulkEdit(rows, section("_DESCRIPTION"), QStringLiteral("bulk"));
    QVERIFY(m_model->updateAttribute(QStringLiteral("s1"), QString::fromLatin1(SessionKeys::Description),
                                     QStringLiteral("edited")));
    connect(m_model.get(), &SessionModel::sessionLoaded, &scope, [&violations, bulkLine](const QString &id) {
        if (!fileHas(id, bulkLine))
            violations.append(id + QStringLiteral(" loaded before its bulk edit was saved"));
    });

    QVERIFY(waitDemandIdle());
    QVERIFY(waitForIdle(*m_model));
    QVERIFY2(violations.isEmpty(), qPrintable(violations.join(QStringLiteral("; "))));
    const int firstFill = int(order.indexOf(QStringLiteral("F")));
    QVERIFY(firstFill >= 0);
    QVERIFY(!order.contains(QStringLiteral("F!")));
    QVERIFY(!order.mid(firstFill).contains(QStringLiteral("S")));
    QVERIFY(!order.mid(firstFill).contains(QStringLiteral("B")));

    for (const char *id : {"s2", "s3", "s4"}) {
        QVERIFY2(fileHas(QString::fromLatin1(id), bulkLine), id);
        QCOMPARE(cachedValue(QString::fromLatin1(id), "DESC_OUT").toString(), QStringLiteral("x:bulk"));
        QVERIFY2(stored(QString::fromLatin1(id), "test.demand.desc"), id);
    }
    const QString s1Description = session("s1").getAttribute(SessionKeys::Description).toString();
    QVERIFY(s1Description == QLatin1String("bulk") || s1Description == QLatin1String("edited"));
    QVERIFY(fileHas(QStringLiteral("s1"), "$VAR,_DESCRIPTION," + s1Description.toUtf8()));
    QCOMPARE(cachedValue(QStringLiteral("s1"), "DESC_OUT").toString(), QStringLiteral("x:") + s1Description);
    QVERIFY(col("DESC_OUT").isPlain());
}

// A settled session becomes applicable by a bulk edit of its stub: the
// single-row change clears the settlement and the session is loaded again.
void CalculationDemandTest::bulkEditMakesSettledSessionApplicable()
{
    QVERIFY(m_extra->add(descriptionCalculation()));
    QVERIFY(m_model->removeAttribute(QStringLiteral("s2"), QString::fromLatin1(SessionKeys::Description)));
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(makeStubs());
    QVERIFY(waitForIdle(*m_model));

    QSignalSpy loadedSpy(m_model.get(), &SessionModel::sessionLoaded);
    enableColumns({"DESC_OUT"});
    QVERIFY(waitDemandIdle());
    QCOMPARE(loadsOf(loadedSpy, "s2"), 1);
    QCOMPARE(jobOf(QStringLiteral("s2"), "test.demand.desc").id, JobId(0));
    QCOMPARE(col("DESC_OUT").wantedCount, 3);

    QVERIFY(makeStubs());
    for (int i = 0; i < 3; ++i)
        spin();
    QCOMPARE(loadsOf(loadedSpy, "s2"), 1);
    QVERIFY(!m_demand->hasFillWork());

    m_model->startBulkEdit({rowOf(QStringLiteral("s2"))}, section("_DESCRIPTION"), QStringLiteral("bulk"));
    QTRY_COMPARE(loadsOf(loadedSpy, "s2"), 2);
    QVERIFY(waitDemandIdle());
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(jobOf(QStringLiteral("s2"), "test.demand.desc").state, JobState::Succeeded);
    QVERIFY(stored("s2", "test.demand.desc"));
    QCOMPARE(cachedValue(QStringLiteral("s2"), "DESC_OUT").toString(), QStringLiteral("x:bulk"));
    QCOMPARE(col("DESC_OUT").wantedCount, 4);
    QVERIFY(col("DESC_OUT").isPlain());
}

// The fill is the lowest-priority task, active once per fill, not cancellable;
// its remaining count falls by one per finished session; a cancel() changes
// nothing.
void CalculationDemandTest::fillTaskIsLowestAndNotCancellable()
{
    for (int i = 1; i <= 4; ++i)
        QVERIFY(giveInput({QStringLiteral("s%1").arg(i)}, "G_IN", i));
    QVERIFY(makeStubs());
    QVERIFY(waitForIdle(*m_model));

    IdleScheduler &scheduler = m_model->scheduler();
    QList<QPair<int, bool>> activations;
    QList<QPair<int, int>> fillProgress;
    QObject scope;
    connect(&scheduler, &IdleScheduler::activeTaskChanged, &scope,
            [&activations](int id, bool cancellable) { activations.append({id, cancellable}); });
    connect(&scheduler, &IdleScheduler::progressChanged, &scope, [&fillProgress](int id, int remaining, int total) {
        if (id == SessionModel::ColumnFillTask)
            fillProgress.append({remaining, total});
    });

    enableColumns({"G_OUT"});
    QVERIFY(gate().waitEntered());
    QTRY_COMPARE(m_demand->heldSessionIds().size(), 2);

    // Before the cancel: the fill was activated once, after the column
    // worker's steps, and it is not cancellable
    int fills = 0;
    int lastColumnTask = -1;
    for (int i = 0; i < activations.size(); ++i) {
        if (activations.at(i).first == SessionModel::ColumnFillTask) {
            ++fills;
            QVERIFY(!activations.at(i).second);
            QVERIFY(i > lastColumnTask);
        } else if (activations.at(i).first == SessionModel::ColumnTask) {
            lastColumnTask = i;
            QCOMPARE(fills, 0);
        } else {
            QCOMPARE(fills, 0);         // save, load, bulk edit: never while the fill is active
        }
    }
    QCOMPARE(fills, 1);

    scheduler.cancel(SessionModel::ColumnFillTask);
    QVERIFY(m_demand->hasFillWork());
    // One session at a time, so that every count is reported. The scheduler
    // reports the active task once per tick: with every job let through, a
    // job can end before the fill's next tick (the column worker's step for
    // the record just written may take the tick before it), and the count
    // then falls by two between reports.
    for (int left = 3; left >= 0; --left) {
        gate().open(1);
        QTRY_VERIFY2(fillProgress.contains(progressOf(left, 4)), qPrintable(QString::number(left)));
    }
    QVERIFY(waitDemandIdle());
    for (int i = 1; i <= 4; ++i)
        QCOMPARE(jobOf(QStringLiteral("s%1").arg(i), "gated").state, JobState::Succeeded);
    QVERIFY(col("G_OUT").isPlain());

    // The total is the fill's high-water mark; the remaining count falls by
    // one per finished session
    QList<int> remaining;
    for (const QPair<int, int> &progress : std::as_const(fillProgress)) {
        QCOMPARE(progress.second, 4);
        if (remaining.isEmpty() || remaining.last() != progress.first)
            remaining.append(progress.first);
    }
    QCOMPARE(remaining, QList<int>({4, 3, 2, 1, 0}));
}

// Nothing is loaded after the executor has shut down, the scheduler goes idle,
// and the holds are released when the demand layer is destroyed.
void CalculationDemandTest::noLoadsAfterExecutorShutdown()
{
    for (int i = 1; i <= 4; ++i)
        QVERIFY(giveInput({QStringLiteral("s%1").arg(i)}, "G_IN", i));
    QVERIFY(makeStubs());
    QVERIFY(waitForIdle(*m_model));

    enableColumns({"G_OUT"});
    QVERIFY(gate().waitEntered());
    QTRY_COMPARE(m_demand->heldSessionIds(), QStringList({"s1", "s2"}));

    QSignalSpy loadedSpy(m_model.get(), &SessionModel::sessionLoaded);
    QSignalSpy idleSpy(&m_model->scheduler(), &IdleScheduler::schedulerIdle);
    m_queue->shutdown();
    m_demand->flush();
    QVERIFY(!m_demand->hasFillWork());
    QVERIFY(!m_demand->canLoad());
    QVERIFY(waitForIdle(*m_model));
    for (int i = 0; i < 3; ++i)
        spin();
    QCOMPARE(loadedSpy.count(), 0);
    QVERIFY(idleSpy.count() > 0);
    QCOMPARE(m_demand->heldSessionIds(), QStringList({"s1", "s2"}));

    m_demand.reset();
    QVERIFY(!m_model->isSessionPinned(QStringLiteral("s1")));
    QVERIFY(!m_model->isSessionPinned(QStringLiteral("s2")));
}

// The destructor unregisters the fill and releases every hold.
void CalculationDemandTest::demandDestroyedReleasesHoldsAndTask()
{
    for (int i = 1; i <= 4; ++i)
        QVERIFY(giveInput({QStringLiteral("s%1").arg(i)}, "G_IN", i));
    QVERIFY(makeStubs());
    QVERIFY(waitForIdle(*m_model));

    enableColumns({"G_OUT"});
    QVERIFY(gate().waitEntered());
    QTRY_COMPARE(m_demand->heldSessionIds(), QStringList({"s1", "s2"}));
    m_queue->shutdown();                    // the executor's own pins go
    QVERIFY(m_model->isSessionPinned(QStringLiteral("s1")));
    QVERIFY(m_model->isSessionPinned(QStringLiteral("s2")));

    QObject scope;
    int fillActivations = 0;
    connect(&m_model->scheduler(), &IdleScheduler::activeTaskChanged, &scope, [&fillActivations](int id, bool) {
        if (id == SessionModel::ColumnFillTask)
            ++fillActivations;
    });
    m_demand.reset();
    QVERIFY(!m_model->isSessionPinned(QStringLiteral("s1")));
    QVERIFY(!m_model->isSessionPinned(QStringLiteral("s2")));
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(fillActivations, 0);
}

// A pass over thousands of stubs asks the logbook manager for each record set
// once; later passes ask again only for a session whose records changed.
void CalculationDemandTest::passOverManyStubsReadsEachRecordSetOnce()
{
    m_demand.reset();
    QMap<QString, QMap<int, QVariant>> stubs;
    for (int i = 0; i < 2000; ++i)
        stubs.insert(QStringLiteral("many%1").arg(i, 4, 10, QLatin1Char('0')), {});
    m_model->populateFromIndex(stubs, {});
    QCOMPARE(m_model->rowCount(), 2000);
    enableColumns({"G_OUT"});
    m_model->resetStoredResultStats();
    QSignalSpy loadedSpy(m_model.get(), &SessionModel::sessionLoaded);

    m_demand = std::make_unique<CalculationDemand>(m_model.get(), m_plots.get(), m_queue.get());
    m_demand->flush();
    QCOMPARE(m_demand->recordSetLookups(), 2000);
    const DemandState state = m_demand->columnState(colId("G_OUT"));
    QCOMPARE(state.waitingCount, 2000);
    QVERIFY(state.waiting.isEmpty());
    QCOMPARE(m_model->storedResultStats().recordsRead, 0);
    QCOMPARE(loadedSpy.count(), 0);

    check("plain");                         // an unrelated trigger
    QVERIFY(m_demand->hasPendingUpdate());
    m_demand->flush();
    QCOMPARE(m_demand->recordSetLookups(), 2000);

    emit LogbookManager::instance().calculationRecordsChanged(QStringLiteral("many0007"), QStringLiteral("gated"));
    QVERIFY(m_demand->hasPendingUpdate());
    m_demand->flush();
    QCOMPARE(m_demand->recordSetLookups(), 2001);
    QCOMPARE(m_model->storedResultStats().recordsRead, 0);
    QCOMPARE(loadedSpy.count(), 0);

    m_demand.reset();                       // before any event-loop turn
}

// ---- Presentation -------------------------------------------------------------------

// The ids the views' clocks follow: exactly the plots and columns whose state
// is working, and statesChanged() says when they change.
void CalculationDemandTest::workingIdsFollowStates()
{
    m_demand->flush();
    QVERIFY(m_demand->workingPlotIds().isEmpty());
    QVERIFY(m_demand->workingColumnIds().isEmpty());

    // Every statesChanged(): whether anything is working then
    QObject scope;
    QList<bool> working;
    connect(m_demand.get(), &CalculationDemand::statesChanged, &scope, [this, &working] {
        working.append(!m_demand->workingPlotIds().isEmpty() || !m_demand->workingColumnIds().isEmpty());
    });

    QVERIFY(giveInput({"s1", "s2"}, "G_IN", 4));
    show({"s1", "s2"});
    check("g");
    QVERIFY(gate().waitEntered());
    QVERIFY(row("Syn/g").isWorking());
    QCOMPARE(m_demand->workingPlotIds(), QStringList({"Syn/g"}));
    QVERIFY(m_demand->workingColumnIds().isEmpty());

    enableColumns({"G_OUT"});
    m_demand->flush();
    QVERIFY(col("G_OUT").isWorking());
    QCOMPARE(m_demand->workingColumnIds(), QStringList({colId("G_OUT")}));
    QCOMPARE(m_demand->workingPlotIds(), QStringList({"Syn/g"}));

    // Unchecked: at once
    check("g", false);
    m_demand->flush();
    QVERIFY(m_demand->workingPlotIds().isEmpty());
    QCOMPARE(m_demand->workingColumnIds(), QStringList({colId("G_OUT")}));

    gate().open(4);
    QVERIFY(waitDemandIdle());
    QVERIFY(m_demand->workingPlotIds().isEmpty());
    QVERIFY(m_demand->workingColumnIds().isEmpty());
    QVERIFY(working.contains(true));
    QCOMPARE(working.last(), false);
}

// A tooltip lists at most kToolTipListLimit tracks per section; the state's
// own lists stay complete.
void CalculationDemandTest::toolTipListsAtMostTenFailures()
{
    QCOMPARE(CalculationDemand::kToolTipListLimit, 10);
    const auto failures = [](int n) {
        QList<DemandTrack> tracks;
        for (int k = 1; k <= n; ++k) {
            DemandTrack track;
            track.sessionName = QStringLiteral("Jump %1").arg(k);
            track.condition = DemandCondition::Failed;
            track.calculationTitles = {QStringLiteral("Explicit A")};
            track.reason = QStringLiteral("r");
            tracks.append(track);
        }
        return tracks;
    };
    const auto finished = [&failures](int n) {
        DemandState state;
        state.requested = true;
        state.failed = failures(n);
        state.failedCount = n;
        state.wantedCount = n;
        state.doneCount = n;
        return state;
    };
    QStringList tenLines{QStringLiteral("Could not be computed:")};
    for (int k = 1; k <= 10; ++k)
        tenLines.append(QStringLiteral("  Jump %1 - r").arg(k));

    // Twelve: ten, then how many more
    DemandState twelve = finished(12);
    QVERIFY(twelve.showsWarning());
    QCOMPARE(CalculationDemand::buildToolTip(twelve),
             (tenLines + QStringList{QStringLiteral("  and 2 more")}).join(QLatin1Char('\n')));
    QCOMPARE(twelve.failed.size(), 12);

    // Exactly ten: no "more" line
    QCOMPARE(CalculationDemand::buildToolTip(finished(10)), tenLines.join(QLatin1Char('\n')));

    // Working with twelve failures: the "Computing" line first, then the capped section
    DemandState working = twelve;
    DemandTrack running;
    running.sessionName = QStringLiteral("Noon");
    running.condition = DemandCondition::Running;
    running.calculationTitles = {QStringLiteral("Sensor fusion")};
    running.progressText = QStringLiteral("iteration 3");
    working.running = {running};
    working.runningCount = 1;
    working.wantedCount = 13;
    QVERIFY(working.isWorking());
    QCOMPARE(CalculationDemand::buildToolTip(working),
             (QStringList{QStringLiteral("Computing: 12 of 13 done"),
                          QStringLiteral("  Noon - Sensor fusion: iteration 3")}
              + tenLines + QStringList{QStringLiteral("  and 2 more")})
                 .join(QLatin1Char('\n')));

    // The running list is capped the same way
    DemandState manyRunning;
    for (int k = 1; k <= 11; ++k) {
        DemandTrack track = running;
        track.sessionName = QStringLiteral("Run %1").arg(k);
        manyRunning.running.append(track);
    }
    manyRunning.runningCount = 11;
    manyRunning.wantedCount = 11;
    const QStringList lines = CalculationDemand::buildToolTip(manyRunning).split(QLatin1Char('\n'));
    QCOMPARE(lines.size(), 1 + 10 + 1);
    QCOMPARE(lines.at(10), QStringLiteral("  Run 10 - Sensor fusion: iteration 3"));
    QCOMPARE(lines.last(), QStringLiteral("  and 1 more"));
}

FLYSIGHT_TEST_MAIN(CalculationDemandTest)
#include "tst_calculation_demand.moc"
