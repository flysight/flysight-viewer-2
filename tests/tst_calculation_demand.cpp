// The demand layer (CalculationDemand) on a real PlotModel, executor
// (JobQueue), SessionModel, real SessionData engines, and the global registry,
// with the synthetic plots of plotfixture.h over the explicit calculations of
// jobfixture.h. No widgets.
// Plot demand: checking a plot, showing a session, loading a visible session
// start work with no other call; unchecking and hiding drop the waiting pair;
// priority (the focused session, then row order, upstream first); the
// input-settle wait; the memory of job-level failures and not-applicable pairs;
// the per-plot state and its signals.
//
// Synchronization: Gate::waitEntered() proves the worker is inside a compute
// function; QTRY_*, waitIdle() and waitDemandIdle() spin the event loop for
// main-thread effects; CalculationDemand::flush() runs a pending pass before a
// state is read; settle() ends the input-settle waits. There are no sleeps.

#include <memory>

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
    [[nodiscard]] bool waitDemandIdle(int timeoutMs = 5000)
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

    std::unique_ptr<JobWorld> m_world;
    std::unique_ptr<PlotFixture> m_fixture;
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
// goes before the executor, as in the application.
void CalculationDemandTest::cleanup()
{
    if (m_queue)
        m_queue->shutdown();        // let nothing linger inside a compute function
    QStringList stillPinned;
    if (m_model) {
        for (const char *id : {"s1", "s2", "s3", "s4", "s5"}) {
            if (m_model->isSessionPinned(id))
                stillPinned.append(QString::fromLatin1(id));
        }
    }

    m_demand.reset();
    m_plots.reset();
    m_queue.reset();
    m_model.reset();

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
    QVERIFY(waitDemandIdle(10000));

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
// the job runs with no other call.
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

    // A shorter wait for the next change; the change restarts the wait
    m_demand->setInputSettleDelay(200);
    QVERIFY(giveInput({"s1"}, "G_IN", 8));
    QVERIFY(m_demand->isSettling("s1"));
    spin();
    QCOMPARE(m_queue->model()->rowCount(), 1);
    gate().open(1);
    QTRY_COMPARE(m_queue->model()->rowCount(), 2);  // the wait passed: offered by itself
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

FLYSIGHT_TEST_MAIN(CalculationDemandTest)
#include "tst_calculation_demand.moc"
