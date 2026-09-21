// The plot request logic (PlotRequests) on a real PlotModel, JobQueue,
// SessionModel, real SessionData engines, and the global registry, with the
// synthetic plots of plotfixture.h over the explicit calculations of
// jobfixture.h. No widgets.
// Sensor-fusion-jobs acceptance 11 (row half), 13, 15, 16 (logic).
//
// Synchronization: Gate::waitEntered() proves the worker is inside a compute
// function; QTRY_* and waitIdle() spin the event loop for main-thread effects;
// PlotRequests::flush() runs a pending pass before a row state is read. There
// are no sleeps.

#include <memory>

#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QtTest>

#include "builtinfixture.h"
#include "engine/calculationdescriptor.h"
#include "engine/calculationengine.h"
#include "engine/calculationregistry.h"
#include "engine/calculationresult.h"
#include "engine/evaluationcontext.h"
#include "jobfixture.h"
#include "jobmodel.h"
#include "jobqueue.h"
#include "logbookcolumn.h"
#include "plotfixture.h"
#include "plotmodel.h"
#include "plotrequests.h"
#include "preferences/preferencekeys.h"
#include "preferences/preferencesmanager.h"
#include "sessiondata.h"
#include "sessionmodel.h"
#include "testenvironment.h"
#include "testmain.h"
#include "testutil.h"

using namespace FlySight;
using namespace FlySightTest;

using Kind = JobQueue::RequestResult::Kind;
using Control = PlotRowState::Control;

namespace {

const char kNoLongerNeeded[] = "No longer needed";

/// "Nothing started": no jobQueued signal and no new row in the job model.
class Quiet {
public:
    explicit Quiet(JobQueue &queue)
        : m_queue(queue), m_spy(&queue, &JobQueue::jobQueued), m_rows(queue.model()->rowCount())
    {
    }
    bool holds() const { return m_spy.isEmpty() && m_queue.model()->rowCount() == m_rows; }

private:
    JobQueue &m_queue;
    QSignalSpy m_spy;
    int m_rows;
};

QStringList sessionIdsOf(const QList<PlotTrackState> &tracks)
{
    QStringList ids;
    for (const PlotTrackState &track : tracks)
        ids.append(track.sessionId);
    return ids;
}

} // namespace

class PlotRequestsTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void plotIdMatchesPlotModelRole();
    void ordinaryPlotsAreNeverInspected();
    void uncheckedPlotsAreNeverInspected();
    void hiddenAndStubRowsAreNotTracks();
    void rowScript();
    void chainedBlockersContinue();
    void heldChainContinues();
    void chainStopsWhenFirstJobDoesNotSucceed_data();
    void chainStopsWhenFirstJobDoesNotSucceed();
    void chainNotContinuedForHiddenTrackOrUncheckedPlot_data();
    void chainNotContinuedForHiddenTrackOrUncheckedPlot();
    void programmaticCheckStartsNothing();
    void profileStyleApplyStartsNothing();
    void startupRestoreStartsNothing();
    void showingTrackStartsNothing();
    void loadingSessionStartsNothing();
    void mergeStartsNothing();
    void inputInvalidationStartsNothing();
    void supersededJobStartsNothing();
    void sessionWithoutInputIsNeverListed();
    void gestureNeverRequestsTheUnrequestable();
    void failedBadgeAndReason();
    void failedAndPendingTogether();
    void sharedJobSameProgress();
    void rowCheckedProgrammaticallyDuringJobShowsPending();
    void cancelThenRefreshWhileWindingDown();
    void uncheckPrunesQueuedKeepsRunning();
    void hidePrunesOnlyThatTrack();
    void queuedJobNeededByOtherPlotSurvives();
    void tooltipText();
    void changeSignalsAreMinimal();
    void dependencyBurstIsCoalesced();
    void progressUpdatesWithoutInspection();
    void removedSessionLeavesNoTrace();
    void registryChangeReclassifies();
    void survivesQueueShutdown();
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
    void giveInput(const QStringList &ids, const char *key, double value)
    {
        for (const QString &id : ids)
            QVERIFY2(PlotFixture::giveInput(*m_model, id, QString::fromLatin1(key), value), qPrintable(id));
    }
    /// A programmatic check: never a gesture.
    void check(const char *measurement, bool enabled = true)
    {
        m_plots->setPlotEnabled(QStringLiteral("Syn"), QString::fromLatin1(measurement), enabled);
    }
    /// What the view does for a direct check of the row: write the model, then say so.
    int checkByUser(const char *measurement)
    {
        check(measurement);
        return m_requests->plotCheckedByUser(QStringLiteral("Syn/") + QString::fromLatin1(measurement));
    }
    /// The current row state: a pending pass runs first.
    PlotRowState row(const char *plotId)
    {
        m_requests->flush();
        return m_requests->rowState(QString::fromLatin1(plotId));
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
    /// Two turns of the event loop and a flush: whatever was going to start by
    /// itself has started.
    void spin()
    {
        QTest::qWait(0);
        QTest::qWait(0);
        m_requests->flush();
    }
    int totalRuns()
    {
        int runs = 0;
        for (const char *id : {"s1", "s2", "s3", "s4"})
            runs += engine(id).totalRunCount();
        return runs;
    }

    std::unique_ptr<JobWorld> m_world;
    std::unique_ptr<PlotFixture> m_fixture;
    std::unique_ptr<SessionModel> m_model;
    std::unique_ptr<JobQueue> m_queue;
    std::unique_ptr<PlotModel> m_plots;
    std::unique_ptr<PlotRequests> m_requests;
    QStringList m_registryBefore;
};

void PlotRequestsTest::initTestCase()
{
    TestEnvironment::instance().registerBuiltIns();

    // One logbook column that reads stored data only (see tst_jobqueue)
    PreferencesManager::instance().registerPreference(PreferenceKeys::LogbookColumnsVersion, 0);
    LogbookColumn description;
    description.type = ColumnType::SessionAttribute;
    description.attributeKey = QString::fromLatin1(SessionKeys::Description);
    LogbookColumnStore::instance().setColumns({description});
}

// Four loaded, hidden sessions "s1".."s4" named "Jump 1".."Jump 4". None has an
// input of any synthetic calculation: a test adds them. No plot is checked.
void PlotRequestsTest::init()
{
    TestEnvironment &env = TestEnvironment::instance();
    env.useFreshLogbook();
    env.resetPreferencesToDefaults();
    m_registryBefore = CalculationRegistry::instance().registeredIds();

    m_world = std::make_unique<JobWorld>();
    m_fixture = std::make_unique<PlotFixture>();
    m_model = std::make_unique<SessionModel>();
    m_model->mergeSessions(JobWorld::sessions({"s1", "s2", "s3", "s4"}));
    QCOMPARE(m_model->rowCount(), 4);
    for (int i = 1; i <= 4; ++i) {
        m_model->updateAttribute(QStringLiteral("s%1").arg(i), QString::fromLatin1(SessionKeys::Description),
                                 QStringLiteral("Jump %1").arg(i));
    }
    m_model->flushPendingInvalidations();

    m_queue = std::make_unique<JobQueue>(m_model.get());
    m_plots = std::make_unique<PlotModel>();
    m_plots->setPlots(PlotFixture::plots());
    m_requests = std::make_unique<PlotRequests>(m_model.get(), m_plots.get(), m_queue.get());
}

void PlotRequestsTest::cleanup()
{
    if (m_queue)
        m_queue->shutdown();        // let nothing linger inside a compute function
    if (m_model) {
        for (const char *id : {"s1", "s2", "s3", "s4"})
            QVERIFY2(!m_model->isSessionPinned(id), id);
    }

    m_requests.reset();
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
    // The fixture removed exactly what it added
    QCOMPARE(CalculationRegistry::instance().registeredIds(), withoutFixture);
    m_world.reset();
    QCOMPARE(CalculationRegistry::instance().registeredIds(), m_registryBefore);
    QCOMPARE(CalculationRegistry::instance().enrolledEngineCount(), 0);
}

// ---- Vocabulary ---------------------------------------------------------------------

void PlotRequestsTest::plotIdMatchesPlotModelRole()
{
    int plots = 0;
    for (int c = 0; c < m_plots->rowCount(); ++c) {
        const QModelIndex category = m_plots->index(c, 0);
        for (int p = 0; p < m_plots->rowCount(category); ++p) {
            const QModelIndex plot = m_plots->index(p, 0, category);
            QCOMPARE(PlotRequests::plotId(plot.data(PlotModel::SensorIDRole).toString(),
                                          plot.data(PlotModel::MeasurementIDRole).toString()),
                     plot.data(PlotModel::PlotValueIdRole).toString());
            ++plots;
        }
    }
    QCOMPARE(plots, 7);

    PlotValue value;
    value.sensorID = QStringLiteral("Syn");
    value.measurementID = QStringLiteral("g");
    QCOMPARE(PlotRequests::plotId(value), QStringLiteral("Syn/g"));
}

// ---- Which plots are inspected ------------------------------------------------------------

// A plot over a stored attribute is not explicit-backed: it is never inspected
// (inspecting Syn/plain would run the plotPlain bridge), never announced, and
// a gesture on it requests nothing.
void PlotRequestsTest::ordinaryPlotsAreNeverInspected()
{
    giveInput({"s1", "s2", "s3"}, "P_IN", 3);
    giveInput({"s1", "s2", "s3"}, "G_IN", 4);
    show({"s1", "s2", "s3"});

    QSignalSpy changedSpy(m_requests.get(), &PlotRequests::rowStateChanged);
    QSignalSpy anySpy(m_requests.get(), &PlotRequests::rowStatesChanged);
    const Quiet quiet(*m_queue);
    const int runsBefore = totalRuns();

    check("plain");
    for (int pass = 0; pass < 5; ++pass) {
        show({"s1"}, pass % 2 == 1);        // every change of visibility schedules a pass
        QVERIFY(m_requests->hasPendingUpdate());
        m_requests->flush();
    }
    QVERIFY(m_requests->passCount() >= 5);
    QCOMPARE(totalRuns(), runsBefore);
    QCOMPARE(changedSpy.count(), 0);
    QCOMPARE(anySpy.count(), 0);

    const PlotRowState state = row("Syn/plain");
    QVERIFY(state == PlotRowState());
    QVERIFY(!state.explicitBacked);
    QVERIFY(state.isPlain());

    QCOMPARE(m_requests->plotCheckedByUser(QStringLiteral("Syn/plain")), 0);
    QCOMPARE(m_requests->refreshPressed(QStringLiteral("Syn/plain")), 0);
    QCOMPARE(m_requests->cancelPressed(QStringLiteral("Syn/plain")), 0);
    QCOMPARE(totalRuns(), runsBefore);
    QVERIFY(quiet.holds());

    // The plot itself still reads normally
    QCOMPARE(values("s2", "plain"), QVector<double>({3.0}));
}

// An unchecked plot is not inspected either, explicit-backed or not: with
// G_OUT published, inspecting Syn/g would run the plotG bridge.
void PlotRequestsTest::uncheckedPlotsAreNeverInspected()
{
    giveInput({"s1"}, "G_IN", 4);
    show({"s1", "s2"});
    gate().open(1);
    QCOMPARE(m_queue->request("s1", QStringLiteral("gated")).kind, Kind::Created);
    QVERIFY(waitIdle(*m_queue));
    QVERIFY(gate().waitEntered());

    QSignalSpy changedSpy(m_requests.get(), &PlotRequests::rowStateChanged);
    const int runsBefore = totalRuns();
    for (int pass = 0; pass < 5; ++pass) {
        show({"s2"}, pass % 2 == 1);
        m_requests->flush();
    }
    QCOMPARE(totalRuns(), runsBefore);
    QCOMPARE(changedSpy.count(), 0);
    QVERIFY(row("Syn/g") == PlotRowState());

    // Not checked: not a gesture
    const Quiet quiet(*m_queue);
    QCOMPARE(m_requests->plotCheckedByUser(QStringLiteral("Syn/g")), 0);
    QCOMPARE(m_requests->refreshPressed(QStringLiteral("Syn/g")), 0);
    QCOMPARE(m_requests->plotCheckedByUser(QStringLiteral("No/such")), 0);
    QVERIFY(quiet.holds());
}

// Tracks are the rows the plot widget draws. A hidden session and a stub are
// not tracks, and a pass never loads a session.
void PlotRequestsTest::hiddenAndStubRowsAreNotTracks()
{
    giveInput({"s1", "s2", "s3"}, "G_IN", 4);
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
    const Quiet quiet(*m_queue);
    check("g");
    PlotRowState state = row("Syn/g");
    QCOMPARE(sessionIdsOf(state.missing), QStringList({"s3"}));    // s4 has no input: not applicable
    QCOMPARE(state.missingCount, 1);

    // More passes (an input edit of a visible row does not touch the LRU)
    const int passes = m_requests->passCount();
    giveInput({"s3"}, "G_IN", 6);
    state = row("Syn/g");
    giveInput({"s3"}, "G_IN", 8);
    state = row("Syn/g");
    QCOMPARE(m_requests->passCount(), passes + 2);
    QCOMPARE(state.missingCount, 1);
    QCOMPARE(loadedSpy.count(), 0);
    QVERIFY(!isLoaded("s1"));
    QVERIFY(isLoaded("s2"));            // had a pass touched the LRU, s2 would be gone
    QVERIFY(quiet.holds());
}

// ---- Acceptance 15 -----------------------------------------------------------------------

void PlotRequestsTest::rowScript()
{
    const QString plot = QStringLiteral("Syn/g");
    giveInput({"s1", "s2", "s3", "s4"}, "G_IN", 4);

    // 1. Checked, three visible fusable tracks: nothing starts by itself
    show({"s1", "s2", "s3"});
    {
        const Quiet quiet(*m_queue);
        check("g");
        const PlotRowState state = row("Syn/g");
        QVERIFY(state.explicitBacked);
        QCOMPARE(state.plotId, plot);
        QCOMPARE(state.missingCount, 3);
        QCOMPARE(state.pendingCount, 0);
        QCOMPARE(state.control(), Control::Refresh);
        QCOMPARE(state.controlCount(), 3);
        QVERIFY(state.progressLabel.isEmpty());
        QVERIFY(quiet.holds());
    }

    //    The gesture queues three jobs
    QCOMPARE(m_requests->plotCheckedByUser(plot), 3);
    QVERIFY(!m_requests->hasPendingUpdate());       // the pass ran before the call returned
    QCOMPARE(m_queue->model()->rowCount(), 3);
    PlotRowState state = m_requests->rowState(plot);
    QCOMPARE(state.pendingCount, 3);
    QCOMPARE(state.missingCount, 0);
    QCOMPARE(state.waitingTotal, 3);
    QCOMPARE(state.waitingDone, 0);
    QCOMPARE(state.progressLabel, QStringLiteral("0 of 3"));
    QCOMPARE(state.control(), Control::Cancel);
    QCOMPARE(state.controlCount(), 3);

    const JobId job1 = jobOf("s1", "gated").id;
    const JobId job2 = jobOf("s2", "gated").id;
    const JobId job3 = jobOf("s3", "gated").id;
    QVERIFY(job1 != 0 && job2 != 0 && job3 != 0);
    QVERIFY(gate().waitEntered());
    QCOMPARE(stateOf(job1), JobState::Running);
    QCOMPARE(stateOf(job2), JobState::Queued);
    QCOMPARE(stateOf(job3), JobState::Queued);

    //    A second gesture while the jobs are active changes nothing
    {
        const Quiet quiet(*m_queue);
        QCOMPARE(m_requests->refreshPressed(plot), 0);
        QCOMPARE(m_requests->plotCheckedByUser(plot), 0);
        QVERIFY(quiet.holds());
        state = m_requests->rowState(plot);
        QCOMPARE(state.pendingCount, 3);
        QCOMPARE(state.progressLabel, QStringLiteral("0 of 3"));
    }

    // 2. The count falls as each job publishes
    gate().open(1);
    QTRY_COMPARE(stateOf(job1), JobState::Succeeded);
    QVERIFY(gate().waitEntered());
    QCOMPARE(stateOf(job2), JobState::Running);
    QTRY_COMPARE(m_queue->job(job2).progressText, QStringLiteral("step 1"));
    state = row("Syn/g");
    QCOMPARE(state.pendingCount, 2);
    QCOMPARE(state.waitingTotal, 3);
    QCOMPARE(state.waitingDone, 1);
    QCOMPARE(state.progressLabel, QStringLiteral("1 of 3"));
    QCOMPARE(state.control(), Control::Cancel);
    QCOMPARE(state.jobProgressText, QStringLiteral("step 1"));
    QCOMPARE(state.toolTip, QStringLiteral("Computing (1 of 3 done):\n"
                                           "  Jump 2 - Gated: step 1\n"
                                           "  Jump 3 - Gated: queued"));
    QCOMPARE(values("s1", "g"), QVector<double>({5.0}));

    // 3. Unchecking removes the queued job and lets the running one finish
    check("g", false);
    QCOMPARE(stateOf(job3), JobState::Cancelled);
    QCOMPARE(m_queue->job(job3).reason, QString::fromLatin1(kNoLongerNeeded));
    QCOMPARE(stateOf(job2), JobState::Running);
    QVERIFY(!m_queue->job(job2).cancelRequested);
    QVERIFY(row("Syn/g") == PlotRowState());

    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(job2), JobState::Succeeded);
    QCOMPARE(values("s2", "g"), QVector<double>({5.0}));
    QCOMPARE(m_queue->model()->rowCount(), 3);

    // 4. Checked again by the user: only s3 is missing. Cancel leaves the plot
    //    checked and the track missing.
    QCOMPARE(checkByUser("g"), 1);
    state = m_requests->rowState(plot);
    QCOMPARE(state.pendingCount, 1);
    QCOMPARE(state.progressLabel, QStringLiteral("0 of 1"));
    const JobId job4 = jobOf("s3", "gated").id;
    QVERIFY(job4 != job3);
    QVERIFY(gate().waitEntered());

    QCOMPARE(m_requests->cancelPressed(plot), 1);
    QVERIFY(m_plots->isPlotEnabled("Syn", "g"));
    state = m_requests->rowState(plot);             // at once: the worker has not returned yet
    QCOMPARE(stateOf(job4), JobState::Running);
    QCOMPARE(state.pendingCount, 0);
    QCOMPARE(state.missingCount, 1);
    QCOMPARE(state.control(), Control::Refresh);

    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(job4), JobState::Cancelled);
    QVERIFY(values("s3", "g").isEmpty());
    QVERIFY(!session("s3").getAttribute("G_OUT").isValid());     // nothing was published
    QVERIFY(m_plots->isPlotEnabled("Syn", "g"));
    QCOMPARE(row("Syn/g").missingCount, 1);

    // 5. Refresh computes it
    QCOMPARE(m_requests->refreshPressed(plot), 1);
    QVERIFY(gate().waitEntered());
    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    state = row("Syn/g");
    QVERIFY(state.isPlain());
    QVERIFY(state.toolTip.isEmpty());
    QCOMPARE(state.control(), Control::None);
    QCOMPARE(values("s3", "g"), QVector<double>({5.0}));

    // 6. A fourth track shown afterwards is missing, with a count of one, and
    //    starts nothing
    {
        const Quiet quiet(*m_queue);
        show({"s4"});
        state = row("Syn/g");
        QCOMPARE(state.missingCount, 1);
        QCOMPARE(state.control(), Control::Refresh);
        QCOMPARE(state.controlCount(), 1);
        QCOMPARE(state.missing.at(0).sessionId, QStringLiteral("s4"));
        QCOMPARE(state.missing.at(0).sessionName, QStringLiteral("Jump 4"));
        QCOMPARE(state.missing.at(0).calculationTitles, QStringList({"Gated"}));
        spin();
        QVERIFY(quiet.holds());
        QVERIFY(m_queue->isIdle());
        QCOMPARE(row("Syn/g").missingCount, 1);
    }

    // 7. Pressing refresh computes it
    QCOMPARE(m_requests->refreshPressed(plot), 1);
    QVERIFY(gate().waitEntered());
    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QVERIFY(row("Syn/g").isPlain());
    QCOMPARE(values("s4", "g"), QVector<double>({5.0}));
    QCOMPARE(m_queue->model()->rowCount(), 6);
    QCOMPARE(gate().maxRunning.load(), 1);
}

// ---- Acceptance 13: chained blockers ---------------------------------------------------------

void PlotRequestsTest::chainedBlockersContinue()
{
    const QString plot = QStringLiteral("Syn/db");
    giveInput({"s1"}, "EA_IN", 4);
    giveInput({"s1"}, "EB_IN", 10);
    show({"s1"});
    check("db");
    PlotRowState state = row("Syn/db");
    QCOMPARE(state.missingCount, 1);
    QCOMPARE(state.missing.at(0).calculationTitles, QStringList({"Explicit A"}));

    QSignalSpy idleSpy(m_queue.get(), &JobQueue::idle);
    // Every announced state between the gesture and the end is "pending"
    QList<PlotRowState> announced;
    connect(m_requests.get(), &PlotRequests::rowStateChanged, this, [&](const QString &id) {
        if (id == plot)
            announced.append(m_requests->rowState(id));
    });

    QCOMPARE(m_requests->plotCheckedByUser(plot), 1);      // one gesture, one job: expA
    QVERIFY(waitIdle(*m_queue));
    state = row("Syn/db");

    const JobModel *jobs = m_queue->model();
    QCOMPARE(jobs->rowCount(), 2);
    QCOMPARE(jobs->record(0).calculationId, QStringLiteral("expA"));
    QCOMPARE(jobs->record(0).state, JobState::Succeeded);
    QCOMPARE(jobs->record(1).calculationId, QStringLiteral("expB"));
    QCOMPARE(jobs->record(1).state, JobState::Succeeded);
    // The queue was never idle between the links
    QCOMPARE(idleSpy.count(), 1);

    QCOMPARE(values("s1", "db"), QVector<double>({19.0}));
    QCOMPARE(engine("s1").runCount("expA"), 1);
    QCOMPARE(engine("s1").runCount("expB"), 1);
    QVERIFY(state.isPlain());

    QVERIFY(announced.size() >= 2);
    for (int i = 0; i < announced.size() - 1; ++i) {
        QCOMPARE(announced.at(i).pendingCount, 1);
        QCOMPARE(announced.at(i).missingCount, 0);
        QCOMPARE(announced.at(i).progressLabel, QStringLiteral("0 of 1"));
    }
    QVERIFY(announced.last().isPlain());
}

void PlotRequestsTest::heldChainContinues()
{
    const QString plot = QStringLiteral("Syn/h");
    giveInput({"s1"}, "G_IN", 4);
    show({"s1"});
    QSignalSpy idleSpy(m_queue.get(), &JobQueue::idle);

    QCOMPARE(checkByUser("h"), 1);
    QVERIFY(gate().waitEntered());
    PlotRowState state = row("Syn/h");
    QCOMPARE(state.pendingCount, 1);
    QCOMPARE(state.pending.at(0).calculationTitles, QStringList({"Gated"}));
    QCOMPARE(state.pending.at(0).jobState, JobState::Running);
    QCOMPARE(state.pending.at(0).job, jobOf("s1", "gated").id);
    QCOMPARE(jobCount("afterG"), 0);

    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(jobCount("gated"), 1);
    QCOMPARE(jobCount("afterG"), 1);
    QCOMPARE(jobOf("s1", "afterG").state, JobState::Succeeded);
    QCOMPARE(idleSpy.count(), 1);
    QCOMPARE(values("s1", "h"), QVector<double>({6.0}));
    QVERIFY(row("Syn/h").isPlain());
}

void PlotRequestsTest::chainStopsWhenFirstJobDoesNotSucceed_data()
{
    QTest::addColumn<QString>("action");
    QTest::addColumn<int>("endState");
    QTest::addColumn<double>("finalValue");
    QTest::newRow("cancel pressed") << "cancelPressed" << int(JobState::Cancelled) << 6.0;
    QTest::newRow("cancelled from outside") << "queueCancel" << int(JobState::Cancelled) << 6.0;
    QTest::newRow("superseded") << "editInput" << int(JobState::Superseded) << 9.0;
}

void PlotRequestsTest::chainStopsWhenFirstJobDoesNotSucceed()
{
    QFETCH(QString, action);
    QFETCH(int, endState);
    QFETCH(double, finalValue);
    const QString plot = QStringLiteral("Syn/h");

    giveInput({"s1"}, "G_IN", 4);
    show({"s1"});
    QCOMPARE(checkByUser("h"), 1);
    QVERIFY(gate().waitEntered());
    const JobId first = jobOf("s1", "gated").id;

    if (action == QLatin1String("cancelPressed")) {
        QCOMPARE(m_requests->cancelPressed(plot), 1);
    } else if (action == QLatin1String("queueCancel")) {
        QVERIFY(m_queue->cancel(first));
    } else {
        giveInput({"s1"}, "G_IN", 7);
        QCOMPARE(row("Syn/h").pendingCount, 1);     // still running: the engine decides at publish
        gate().open(1);
    }
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(int(stateOf(first)), endState);

    // Nothing continued and nothing was requested again
    QCOMPARE(jobCount("afterG"), 0);
    QCOMPARE(m_queue->model()->rowCount(), 1);
    spin();
    QCOMPARE(m_queue->model()->rowCount(), 1);
    PlotRowState state = row("Syn/h");
    QCOMPARE(state.pendingCount, 0);
    QCOMPARE(state.missingCount, 1);
    QCOMPARE(state.control(), Control::Refresh);
    QVERIFY(m_plots->isPlotEnabled("Syn", "h"));

    // A later refresh completes the whole chain
    QCOMPARE(m_requests->refreshPressed(plot), 1);
    QVERIFY(gate().waitEntered());
    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(jobCount("afterG"), 1);
    QCOMPARE(values("s1", "h"), QVector<double>({finalValue}));
    QVERIFY(row("Syn/h").isPlain());
}

void PlotRequestsTest::chainNotContinuedForHiddenTrackOrUncheckedPlot_data()
{
    QTest::addColumn<bool>("hide");
    QTest::newRow("track hidden") << true;
    QTest::newRow("plot unchecked") << false;
}

void PlotRequestsTest::chainNotContinuedForHiddenTrackOrUncheckedPlot()
{
    QFETCH(bool, hide);

    giveInput({"s1"}, "G_IN", 4);
    show({"s1"});
    QCOMPARE(checkByUser("h"), 1);
    QVERIFY(gate().waitEntered());
    const JobId first = jobOf("s1", "gated").id;

    if (hide)
        show({"s1"}, false);
    else
        check("h", false);
    QCOMPARE(stateOf(first), JobState::Running);        // the running job is left to finish
    QVERIFY(!m_queue->job(first).cancelRequested);

    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(first), JobState::Succeeded);
    QCOMPARE(session("s1").getAttribute("G_OUT"), QVariant(5));
    QCOMPARE(jobCount("afterG"), 0);

    // Back again, programmatically: the rest of the chain is missing
    const Quiet quiet(*m_queue);
    if (hide)
        show({"s1"});
    else
        check("h");
    spin();
    const PlotRowState state = row("Syn/h");
    QCOMPARE(state.missingCount, 1);
    QCOMPARE(state.missing.at(0).calculationTitles, QStringList({"After G"}));
    QCOMPARE(state.control(), Control::Refresh);
    QVERIFY(quiet.holds());
}

// ---- Acceptance 16 (logic) and the list of spec 9.3: what is NOT a gesture ---------------------

void PlotRequestsTest::programmaticCheckStartsNothing()
{
    giveInput({"s1", "s2", "s3"}, "G_IN", 4);
    show({"s1", "s2", "s3"});
    const Quiet quiet(*m_queue);

    // setPlotEnabled (profiles)
    check("g");
    spin();
    QCOMPARE(row("Syn/g").missingCount, 3);
    QCOMPARE(row("Syn/g").control(), Control::Refresh);
    check("g", false);
    QVERIFY(row("Syn/g") == PlotRowState());

    // togglePlot (the Plots menu and its shortcuts)
    QVERIFY(m_plots->togglePlot("Syn", "g"));
    spin();
    QCOMPARE(row("Syn/g").missingCount, 3);
    QVERIFY(!m_plots->togglePlot("Syn", "g"));

    // setData(CheckStateRole): what the view's checkbox writes. The write is
    // not the gesture; the view's explicit call is.
    QModelIndex index;
    const QModelIndex category = m_plots->index(0, 0);
    for (int p = 0; p < m_plots->rowCount(category); ++p) {
        if (m_plots->index(p, 0, category).data(PlotModel::PlotValueIdRole).toString() == QLatin1String("Syn/g"))
            index = m_plots->index(p, 0, category);
    }
    QVERIFY(index.isValid());
    QVERIFY(m_plots->setData(index, Qt::Checked, Qt::CheckStateRole));
    spin();
    QCOMPARE(row("Syn/g").missingCount, 3);

    QVERIFY(quiet.holds());
    QVERIFY(m_queue->isIdle());
}

// The loop of applyProfile(): setPlotEnabled over all plots.
void PlotRequestsTest::profileStyleApplyStartsNothing()
{
    giveInput({"s1", "s2"}, "G_IN", 4);
    giveInput({"s1", "s2"}, "EA_IN", 4);
    giveInput({"s1", "s2"}, "EB_IN", 10);
    show({"s1", "s2"});
    const Quiet quiet(*m_queue);

    const QSet<QString> profile = {"Syn/g", "Syn/g2", "Syn/db", "Syn/h", "Syn/plain"};
    const QVector<PlotValue> plots = PlotFixture::plots();
    for (const PlotValue &plot : plots)
        m_plots->setPlotEnabled(plot.sensorID, plot.measurementID, profile.contains(PlotRequests::plotId(plot)));
    spin();

    for (const char *id : {"Syn/g", "Syn/g2", "Syn/db", "Syn/h"}) {
        QCOMPARE(row(id).missingCount, 2);
        QCOMPARE(row(id).control(), Control::Refresh);
    }
    QVERIFY(row("Syn/plain") == PlotRowState());
    QVERIFY(row("Syn/ea") == PlotRowState());       // unchecked by the profile
    QVERIFY(quiet.holds());
    QVERIFY(m_queue->isIdle());
}

// Plots restored as checked from the settings come up through modelReset.
void PlotRequestsTest::startupRestoreStartsNothing()
{
    giveInput({"s1", "s2", "s3"}, "G_IN", 4);
    show({"s1", "s2", "s3"});
    const Quiet quiet(*m_queue);

    const QString path = TestEnvironment::instance().newTempDir(QStringLiteral("plots")) + QStringLiteral("/plots.ini");
    QSettings settings(path, QSettings::IniFormat);
    settings.setValue(QStringLiteral("state/plots/Syn/g"), true);

    // As the application starts: the component exists before the plots do
    m_requests.reset();
    m_plots = std::make_unique<PlotModel>();
    m_plots->setSettings(&settings);
    m_requests = std::make_unique<PlotRequests>(m_model.get(), m_plots.get(), m_queue.get());
    m_plots->setPlots(PlotFixture::plots());
    QVERIFY(m_plots->isPlotEnabled("Syn", "g"));
    spin();

    PlotRowState state = row("Syn/g");
    QCOMPARE(state.missingCount, 3);
    QCOMPARE(state.control(), Control::Refresh);
    QVERIFY(quiet.holds());

    // ... and the other order: the plots are there when the component is created
    m_requests.reset();
    m_requests = std::make_unique<PlotRequests>(m_model.get(), m_plots.get(), m_queue.get());
    QVERIFY(m_requests->hasPendingUpdate());        // the initial pass needs no event
    spin();
    state = row("Syn/g");
    QCOMPARE(state.missingCount, 3);
    QVERIFY(quiet.holds());
    QVERIFY(m_queue->isIdle());

    // The settings object must outlive the model that writes to it
    m_requests.reset();
    m_plots.reset();
}

void PlotRequestsTest::showingTrackStartsNothing()
{
    giveInput({"s1", "s2"}, "G_IN", 4);
    check("g");
    const Quiet quiet(*m_queue);

    QVERIFY(row("Syn/g").isPlain());        // no track at all
    show({"s1"});
    QCOMPARE(row("Syn/g").missingCount, 1);
    show({"s2"});
    QCOMPARE(row("Syn/g").missingCount, 2);
    show({"s1"}, false);
    QCOMPARE(sessionIdsOf(row("Syn/g").missing), QStringList({"s2"}));
    spin();
    QVERIFY(quiet.holds());
}

// A visible stub becomes a track when the model loads it.
void PlotRequestsTest::loadingSessionStartsNothing()
{
    giveInput({"s1", "s2"}, "G_IN", 4);
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

    check("g");
    QCOMPARE(sessionIdsOf(row("Syn/g").missing), QStringList({"s2"}));

    const Quiet quiet(*m_queue);
    QSignalSpy loadedSpy(m_model.get(), &SessionModel::sessionLoaded);
    show({"s1"});                       // the model loads the stub
    QVERIFY(isLoaded("s1"));
    QVERIFY(loadedSpy.count() >= 1);
    const PlotRowState state = row("Syn/g");
    QCOMPARE(state.missingCount, 2);
    QCOMPARE(state.control(), Control::Refresh);
    spin();
    QVERIFY(quiet.holds());
    QVERIFY(m_queue->isIdle());
}

void PlotRequestsTest::mergeStartsNothing()
{
    giveInput({"s1"}, "G_IN", 4);
    show({"s1"});
    check("g");
    QCOMPARE(row("Syn/g").missingCount, 1);
    const Quiet quiet(*m_queue);

    // Into an existing, visible session
    SessionData sensorOnly = DescentFixture::loadSensorOnly("s1");
    sensorOnly.setMeasurement("IMU", "wx", {30.0, 60.0, 90.0});
    const QList<MergeResult> merged = m_model->mergeSessions({sensorOnly});
    QCOMPARE(merged.size(), 1);
    QCOMPARE(merged.at(0).outcome, MergeResult::Outcome::Merged);
    spin();
    QCOMPARE(row("Syn/g").missingCount, 1);

    // A new session
    const QList<MergeResult> created = m_model->mergeSessions(JobWorld::sessions({"s5"}));
    QCOMPARE(created.at(0).outcome, MergeResult::Outcome::Created);
    giveInput({"s5"}, "G_IN", 4);
    show({"s5"});
    spin();
    QCOMPARE(sessionIdsOf(row("Syn/g").missing), QStringList({"s1", "s5"}));
    QVERIFY(quiet.holds());
    QVERIFY(m_queue->isIdle());

    // cleanup() checks the pins of s1..s4 only; s5 was never pinned
    QVERIFY(!m_model->isSessionPinned("s5"));
}

// A published result invalidated by an input change is simply missing again.
void PlotRequestsTest::inputInvalidationStartsNothing()
{
    giveInput({"s1"}, "G_IN", 4);
    show({"s1"});
    gate().open(1);
    QCOMPARE(checkByUser("g"), 1);
    QVERIFY(waitIdle(*m_queue));
    QVERIFY(gate().waitEntered());
    QVERIFY(row("Syn/g").isPlain());
    QCOMPARE(values("s1", "g"), QVector<double>({5.0}));

    const Quiet quiet(*m_queue);
    giveInput({"s1"}, "G_IN", 7);
    QVERIFY(m_requests->hasPendingUpdate());
    spin();
    const PlotRowState state = row("Syn/g");
    QCOMPARE(state.missingCount, 1);
    QCOMPARE(state.pendingCount, 0);
    QCOMPARE(state.control(), Control::Refresh);
    QVERIFY(values("s1", "g").isEmpty());
    QVERIFY(quiet.holds());
    QVERIFY(m_queue->isIdle());
}

void PlotRequestsTest::supersededJobStartsNothing()
{
    giveInput({"s1"}, "G_IN", 4);
    show({"s1"});
    QCOMPARE(checkByUser("g"), 1);
    QVERIFY(gate().waitEntered());
    const JobId job = jobOf("s1", "gated").id;

    giveInput({"s1"}, "G_IN", 7);
    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(job), JobState::Superseded);

    spin();
    const PlotRowState state = row("Syn/g");
    QCOMPARE(state.missingCount, 1);
    QCOMPARE(state.control(), Control::Refresh);
    QCOMPARE(m_queue->model()->rowCount(), 1);      // the queue did not ask again, and neither did the row
    QVERIFY(values("s1", "g").isEmpty());
}

// ---- Acceptance 11 (row half) ---------------------------------------------------------------------

void PlotRequestsTest::sessionWithoutInputIsNeverListed()
{
    const QString plot = QStringLiteral("Syn/g");
    giveInput({"s1", "s2"}, "G_IN", 4);      // s3 has no input: nothing to compute
    show({"s1", "s2", "s3"});
    check("g");
    check("g2");

    const auto neverListed = [this](const char *plotId) {
        const PlotRowState state = row(plotId);
        return !sessionIdsOf(state.pending).contains("s3") && !sessionIdsOf(state.missing).contains("s3")
            && !sessionIdsOf(state.failed).contains("s3");
    };

    // Before
    QCOMPARE(row("Syn/g").missingCount, 2);
    QVERIFY(neverListed("Syn/g"));
    QVERIFY(neverListed("Syn/g2"));

    // During
    QCOMPARE(m_requests->plotCheckedByUser(plot), 2);
    QCOMPARE(m_queue->model()->rowCount(), 2);
    QVERIFY(gate().waitEntered());
    QCOMPARE(row("Syn/g").pendingCount, 2);
    QCOMPARE(row("Syn/g").waitingTotal, 2);
    QVERIFY(neverListed("Syn/g"));
    QVERIFY(neverListed("Syn/g2"));

    // No job can be created for it
    QCOMPARE(m_queue->request("s3", QStringLiteral("gated")).kind, Kind::MissingInput);
    QCOMPARE(jobOf("s3", "gated").id, JobId(0));

    gate().open(1);
    QVERIFY(gate().waitEntered());
    QVERIFY(neverListed("Syn/g"));
    gate().open(1);
    QVERIFY(waitIdle(*m_queue));

    // After
    QVERIFY(row("Syn/g").isPlain());
    QVERIFY(row("Syn/g2").isPlain());
    QVERIFY(neverListed("Syn/g"));
    QCOMPARE(m_requests->refreshPressed(plot), 0);
    QCOMPARE(m_queue->model()->rowCount(), 2);
    QVERIFY(values("s3", "g").isEmpty());
}

// The engine only reports requestable blockers, so a gesture asks the queue
// for exactly the requestable tracks: every request creates a job.
void PlotRequestsTest::gestureNeverRequestsTheUnrequestable()
{
    giveInput({"s1", "s3"}, "EA_IN", 4);    // s1: requestable; s3: computed below
    giveInput({"s4"}, "EA_IN", -1);         // rejected below; s2 has no input
    QCOMPARE(m_queue->request("s3", QStringLiteral("expA")).kind, Kind::Created);
    QCOMPARE(m_queue->request("s4", QStringLiteral("expA")).kind, Kind::Created);
    QVERIFY(waitIdle(*m_queue));

    show({"s1", "s2", "s3", "s4"});
    check("ea");
    PlotRowState state = row("Syn/ea");
    QCOMPARE(sessionIdsOf(state.missing), QStringList({"s1"}));
    QCOMPARE(sessionIdsOf(state.failed), QStringList({"s4"}));

    QSignalSpy queuedSpy(m_queue.get(), &JobQueue::jobQueued);
    QCOMPARE(m_requests->plotCheckedByUser(QStringLiteral("Syn/ea")), 1);
    QCOMPARE(queuedSpy.count(), 1);
    QCOMPARE(jobOf("s1", "expA").state, JobState::Queued);
    QVERIFY(waitIdle(*m_queue));

    state = row("Syn/ea");
    QVERIFY(state.pending.isEmpty());
    QVERIFY(state.missing.isEmpty());
    QCOMPARE(sessionIdsOf(state.failed), QStringList({"s4"}));
    QCOMPARE(values("s1", "ea"), QVector<double>({5.0}));
    QCOMPARE(values("s3", "ea"), QVector<double>({5.0}));
    QCOMPARE(m_queue->model()->rowCount(), 3);
}

// ---- Failures ----------------------------------------------------------------------------------

void PlotRequestsTest::failedBadgeAndReason()
{
    const QString plot = QStringLiteral("Syn/ea");
    giveInput({"s1"}, "EA_IN", -1);
    show({"s1"});
    QCOMPARE(checkByUser("ea"), 1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(jobOf("s1", "expA").state, JobState::Succeeded);      // a rejection is a result

    PlotRowState state = row("Syn/ea");
    QCOMPARE(state.failedCount, 1);
    QVERIFY(state.showsWarning());
    QCOMPARE(state.control(), Control::None);
    QCOMPARE(state.controlCount(), 0);
    QVERIFY(!state.isPlain());
    QCOMPARE(state.failed.at(0).sessionId, QStringLiteral("s1"));
    QCOMPARE(state.failed.at(0).calculationTitles, QStringList({"Explicit A"}));
    QCOMPARE(state.failed.at(0).reason, QStringLiteral("Explicit A: negative input"));
    QCOMPARE(state.toolTip, QStringLiteral("Could not be computed:\n"
                                           "  Jump 1 - Explicit A: negative input"));

    // No retry: the same inputs give the same failure
    {
        const Quiet quiet(*m_queue);
        QCOMPARE(m_requests->refreshPressed(plot), 0);
        QCOMPARE(m_requests->plotCheckedByUser(plot), 0);
        QVERIFY(quiet.holds());
    }

    // Its inputs change: missing, and therefore refreshable
    giveInput({"s1"}, "EA_IN", 4);
    state = row("Syn/ea");
    QCOMPARE(state.failedCount, 0);
    QVERIFY(!state.showsWarning());
    QCOMPARE(state.missingCount, 1);
    QCOMPARE(state.control(), Control::Refresh);
    QCOMPARE(m_requests->refreshPressed(plot), 1);
    QVERIFY(waitIdle(*m_queue));
    QVERIFY(row("Syn/ea").isPlain());
    QCOMPARE(values("s1", "ea"), QVector<double>({5.0}));

    // An exception is a cached failed result
    giveInput({"s2"}, "T_IN", 1);
    show({"s2"});
    QCOMPARE(checkByUser("t"), 1);
    QVERIFY(waitIdle(*m_queue));
    state = row("Syn/t");
    QCOMPARE(state.failedCount, 1);
    QCOMPARE(state.failed.at(0).sessionId, QStringLiteral("s2"));
    QCOMPARE(state.failed.at(0).reason, QStringLiteral("Thrower: synthetic failure"));
    QCOMPARE(state.control(), Control::None);
    QCOMPARE(m_requests->refreshPressed(QStringLiteral("Syn/t")), 0);
}

// s1 failed, s2 waits behind a held job of another row: badge and cancel together.
void PlotRequestsTest::failedAndPendingTogether()
{
    giveInput({"s1"}, "EA_IN", -1);
    giveInput({"s3"}, "G_IN", 4);
    show({"s1", "s2", "s3"});
    QCOMPARE(checkByUser("ea"), 1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(row("Syn/ea").failedCount, 1);

    QCOMPARE(checkByUser("g"), 1);          // holds the worker
    QVERIFY(gate().waitEntered());
    giveInput({"s2"}, "EA_IN", 4);
    QCOMPARE(row("Syn/ea").missingCount, 1);
    QCOMPARE(m_requests->refreshPressed(QStringLiteral("Syn/ea")), 1);

    PlotRowState state = row("Syn/ea");
    QCOMPARE(state.failedCount, 1);
    QCOMPARE(state.pendingCount, 1);
    QCOMPARE(state.missingCount, 0);
    QVERIFY(state.showsWarning());
    QCOMPARE(state.control(), Control::Cancel);
    QCOMPARE(state.progressLabel, QStringLiteral("0 of 1"));
    QVERIFY(state.jobProgressText.isEmpty());       // the running job is not this row's
    QCOMPARE(state.pending.at(0).jobState, JobState::Queued);
    QCOMPARE(state.toolTip, QStringLiteral("Computing (0 of 1 done):\n"
                                           "  Jump 2 - Explicit A: queued\n"
                                           "Could not be computed:\n"
                                           "  Jump 1 - Explicit A: negative input"));

    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    state = row("Syn/ea");
    QCOMPARE(state.failedCount, 1);
    QCOMPARE(state.pendingCount, 0);
    QCOMPARE(state.control(), Control::None);
    QVERIFY(state.showsWarning());
    QCOMPARE(values("s2", "ea"), QVector<double>({5.0}));
}

// ---- Rows that share jobs ---------------------------------------------------------------------------

void PlotRequestsTest::sharedJobSameProgress()
{
    giveInput({"s1", "s2"}, "G_IN", 4);
    show({"s1", "s2"});
    QCOMPARE(checkByUser("g"), 2);
    QCOMPARE(checkByUser("g2"), 0);         // the jobs exist: one per session
    QCOMPARE(m_queue->model()->rowCount(), 2);
    QVERIFY(gate().waitEntered());
    QTRY_COMPARE(row("Syn/g").jobProgressText, QStringLiteral("step 1"));

    PlotRowState g = row("Syn/g");
    PlotRowState g2 = row("Syn/g2");
    QCOMPARE(g.pendingCount, 2);
    QCOMPARE(g2.pendingCount, g.pendingCount);
    QCOMPARE(g2.progressLabel, g.progressLabel);
    QCOMPARE(g.progressLabel, QStringLiteral("0 of 2"));
    QCOMPARE(g2.jobProgressText, QStringLiteral("step 1"));
    QCOMPARE(g2.toolTip, g.toolTip);
    QCOMPARE(g2.pending.at(0).job, g.pending.at(0).job);

    // Cancel on one row changes both the same way; both stay checked
    QCOMPARE(m_requests->cancelPressed(QStringLiteral("Syn/g2")), 2);
    g = m_requests->rowState(QStringLiteral("Syn/g"));
    g2 = m_requests->rowState(QStringLiteral("Syn/g2"));
    QCOMPARE(g.pendingCount, 0);
    QCOMPARE(g.missingCount, 2);
    QCOMPARE(g.control(), Control::Refresh);
    QCOMPARE(g2.pendingCount, 0);
    QCOMPARE(g2.missingCount, 2);
    QCOMPARE(g2.control(), Control::Refresh);
    QVERIFY(m_plots->isPlotEnabled("Syn", "g"));
    QVERIFY(m_plots->isPlotEnabled("Syn", "g2"));

    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(m_queue->model()->rowCount(), 2);
    QCOMPARE(row("Syn/g").missingCount, 2);
    QCOMPARE(row("Syn/g2").missingCount, 2);

    // The gesture of the second row adopted the pending tracks: checked by the
    // user during another row's job, it completes its own work too
    QCOMPARE(m_requests->refreshPressed(QStringLiteral("Syn/g")), 2);
    gate().open(2);
    QVERIFY(waitIdle(*m_queue));
    QVERIFY(row("Syn/g").isPlain());
    QVERIFY(row("Syn/g2").isPlain());
    QCOMPARE(values("s1", "g2"), QVector<double>({10.0}));
}

// A row that did not ask waits on another row's job for progress, and
// continues nothing.
void PlotRequestsTest::rowCheckedProgrammaticallyDuringJobShowsPending()
{
    giveInput({"s1"}, "G_IN", 4);
    show({"s1"});
    QCOMPARE(checkByUser("g"), 1);
    QVERIFY(gate().waitEntered());

    check("h");                             // programmatic: Syn/h waits for gated, then afterG
    PlotRowState state = row("Syn/h");
    QCOMPARE(state.pendingCount, 1);
    QCOMPARE(state.waitingTotal, 1);
    QCOMPARE(state.progressLabel, QStringLiteral("0 of 1"));
    QCOMPARE(state.control(), Control::Cancel);
    QCOMPARE(state.pending.at(0).job, jobOf("s1", "gated").id);

    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    spin();
    QCOMPARE(jobCount("afterG"), 0);
    QVERIFY(row("Syn/g").isPlain());
    state = row("Syn/h");
    QCOMPARE(state.missingCount, 1);
    QCOMPARE(state.control(), Control::Refresh);
    QCOMPARE(state.missing.at(0).calculationTitles, QStringList({"After G"}));

    // The user's own check of that row, made while the job runs, continues
    giveInput({"s2"}, "G_IN", 4);
    show({"s2"});
    QCOMPARE(m_requests->refreshPressed(QStringLiteral("Syn/g")), 1);
    QVERIFY(gate().waitEntered());
    check("h", false);
    spin();
    QCOMPARE(checkByUser("h"), 1);          // s1: afterG is created; s2: pending, adopted
    QCOMPARE(jobOf("s1", "afterG").state, JobState::Queued);
    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(jobCount("afterG"), 2);
    QVERIFY(row("Syn/h").isPlain());
    QCOMPARE(values("s2", "h"), QVector<double>({6.0}));
}

// ---- Cancel and pruning --------------------------------------------------------------------------------

void PlotRequestsTest::cancelThenRefreshWhileWindingDown()
{
    const QString plot = QStringLiteral("Syn/g");
    giveInput({"s1", "s2", "s3"}, "G_IN", 4);
    show({"s1", "s2", "s3"});
    check("g2");                            // a second row waiting on the same jobs
    QCOMPARE(checkByUser("g"), 3);
    QVERIFY(gate().waitEntered());
    const JobId running = jobOf("s1", "gated").id;
    const JobId queued2 = jobOf("s2", "gated").id;
    const JobId queued3 = jobOf("s3", "gated").id;

    QCOMPARE(m_requests->cancelPressed(plot), 3);
    // Before the worker has returned
    QCOMPARE(stateOf(queued2), JobState::Cancelled);
    QCOMPARE(stateOf(queued3), JobState::Cancelled);
    QCOMPARE(stateOf(running), JobState::Running);
    QVERIFY(m_queue->job(running).cancelRequested);
    for (const char *id : {"Syn/g", "Syn/g2"}) {
        const PlotRowState state = m_requests->rowState(QString::fromLatin1(id));
        QCOMPARE(state.pendingCount, 0);
        QCOMPARE(state.missingCount, 3);
        QCOMPARE(state.control(), Control::Refresh);
        QVERIFY(state.progressLabel.isEmpty());
    }
    QVERIFY(m_plots->isPlotEnabled("Syn", "g"));
    QVERIFY(m_plots->isPlotEnabled("Syn", "g2"));

    // A refresh while the cancelled job winds down: cancel-requested does not
    // deduplicate, so s1 gets a new job behind it
    QCOMPARE(stateOf(running), JobState::Running);
    QCOMPARE(m_requests->refreshPressed(plot), 3);
    const JobId again = jobOf("s1", "gated").id;
    QVERIFY(again != running);
    PlotRowState state = m_requests->rowState(plot);
    QCOMPARE(state.pendingCount, 3);
    QCOMPARE(state.pending.at(0).sessionId, QStringLiteral("s1"));
    QCOMPARE(state.pending.at(0).job, again);
    QCOMPARE(state.pending.at(0).jobState, JobState::Queued);

    QTRY_COMPARE(stateOf(running), JobState::Cancelled);
    gate().open(3);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(again), JobState::Succeeded);
    QVERIFY(row("Syn/g").isPlain());
    QVERIFY(row("Syn/g2").isPlain());
    QCOMPARE(values("s1", "g"), QVector<double>({5.0}));
}

void PlotRequestsTest::uncheckPrunesQueuedKeepsRunning()
{
    giveInput({"s1", "s2", "s3"}, "G_IN", 4);
    show({"s1", "s2", "s3"});
    QCOMPARE(checkByUser("g"), 3);
    QVERIFY(gate().waitEntered());
    const JobId running = jobOf("s1", "gated").id;

    QVERIFY(!m_plots->togglePlot("Syn", "g"));      // programmatically: pruning needs no gesture
    QCOMPARE(stateOf(running), JobState::Running);
    QVERIFY(!m_queue->job(running).cancelRequested);
    for (const char *id : {"s2", "s3"}) {
        QCOMPARE(jobOf(id, "gated").state, JobState::Cancelled);
        QCOMPARE(jobOf(id, "gated").reason, QString::fromLatin1(kNoLongerNeeded));
    }

    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(running), JobState::Succeeded);
    QCOMPARE(session("s1").getAttribute("G_OUT"), QVariant(5));
    QCOMPARE(values("s1", "g"), QVector<double>({5.0}));
    QCOMPARE(m_queue->model()->rowCount(), 3);
    QVERIFY(row("Syn/g") == PlotRowState());
}

void PlotRequestsTest::hidePrunesOnlyThatTrack()
{
    giveInput({"s1", "s2", "s3"}, "G_IN", 4);
    show({"s1", "s2", "s3"});
    QCOMPARE(checkByUser("g"), 3);
    QVERIFY(gate().waitEntered());
    const JobId running = jobOf("s1", "gated").id;

    show({"s3"}, false);
    QCOMPARE(jobOf("s3", "gated").state, JobState::Cancelled);
    QCOMPARE(jobOf("s3", "gated").reason, QString::fromLatin1(kNoLongerNeeded));
    QCOMPARE(jobOf("s2", "gated").state, JobState::Queued);
    PlotRowState state = row("Syn/g");
    QCOMPARE(state.pendingCount, 2);
    QCOMPARE(state.waitingTotal, 2);

    // Hiding the track of the running job does not cancel it
    show({"s1"}, false);
    QCOMPARE(stateOf(running), JobState::Running);
    QVERIFY(!m_queue->job(running).cancelRequested);
    QCOMPARE(jobOf("s2", "gated").state, JobState::Queued);
    state = row("Syn/g");
    QCOMPARE(sessionIdsOf(state.pending), QStringList({"s2"}));
    QCOMPARE(state.progressLabel, QStringLiteral("0 of 1"));

    gate().open(2);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(running), JobState::Succeeded);
    QCOMPARE(jobOf("s2", "gated").state, JobState::Succeeded);
    QVERIFY(row("Syn/g").isPlain());

    // Shown again, s3 is missing and s1 is simply there
    show({"s1", "s3"});
    QCOMPARE(sessionIdsOf(row("Syn/g").missing), QStringList({"s3"}));
}

void PlotRequestsTest::queuedJobNeededByOtherPlotSurvives()
{
    giveInput({"s1", "s2", "s3"}, "G_IN", 4);
    show({"s1", "s2", "s3"});
    QCOMPARE(checkByUser("g"), 3);
    check("g2");                            // needs the same jobs, asked for none
    QVERIFY(gate().waitEntered());

    check("g", false);                      // the plot that requested them
    for (const char *id : {"s2", "s3"})
        QCOMPARE(jobOf(id, "gated").state, JobState::Queued);
    QCOMPARE(row("Syn/g2").pendingCount, 3);

    check("g2", false);                     // now nobody needs them
    for (const char *id : {"s2", "s3"})
        QCOMPARE(jobOf(id, "gated").state, JobState::Cancelled);
    QCOMPARE(jobOf("s1", "gated").state, JobState::Running);

    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(jobOf("s1", "gated").state, JobState::Succeeded);
}

// ---- Tooltip, signals, coalescing ----------------------------------------------------------------------

void PlotRequestsTest::tooltipText()
{
    // Three sections from a live row: s1 failed, s2 queued behind a held job
    // of another row, s3 not asked for
    giveInput({"s1"}, "EA_IN", -1);
    giveInput({"s4"}, "G_IN", 4);
    show({"s1", "s2", "s3", "s4"});
    QCOMPARE(checkByUser("ea"), 1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(checkByUser("g"), 1);
    QVERIFY(gate().waitEntered());
    giveInput({"s2"}, "EA_IN", 4);
    QCOMPARE(m_requests->refreshPressed(QStringLiteral("Syn/ea")), 1);
    giveInput({"s3"}, "EA_IN", 4);

    QCOMPARE(row("Syn/ea").toolTip, QStringLiteral("Computing (0 of 1 done):\n"
                                                   "  Jump 2 - Explicit A: queued\n"
                                                   "Not computed (press refresh to compute):\n"
                                                   "  Jump 3\n"
                                                   "Could not be computed:\n"
                                                   "  Jump 1 - Explicit A: negative input"));
    gate().open(1);
    QVERIFY(waitIdle(*m_queue));

    // The pure function: a running job without progress text, several titles
    PlotRowState state;
    QCOMPARE(PlotRequests::buildToolTip(state), QString());
    PlotTrackState running;
    running.sessionName = QStringLiteral("Morning");
    running.condition = PlotTrackCondition::Pending;
    running.calculationTitles = {QStringLiteral("Sensor fusion"), QStringLiteral("Other")};
    running.jobState = JobState::Running;
    PlotTrackState progressing = running;
    progressing.sessionName = QStringLiteral("Noon");
    progressing.calculationTitles = {QStringLiteral("Sensor fusion")};
    progressing.jobProgressText = QStringLiteral("iteration 3");
    state.pending = {running, progressing};
    state.pendingCount = 2;
    state.waitingTotal = 5;
    state.waitingDone = 3;
    QCOMPARE(PlotRequests::buildToolTip(state),
             QStringLiteral("Computing (3 of 5 done):\n"
                            "  Morning - Sensor fusion, Other: running\n"
                            "  Noon - Sensor fusion: iteration 3"));
}

void PlotRequestsTest::changeSignalsAreMinimal()
{
    giveInput({"s1"}, "G_IN", 4);
    giveInput({"s1"}, "P_IN", 3);
    show({"s1"});
    m_requests->flush();

    QSignalSpy changedSpy(m_requests.get(), &PlotRequests::rowStateChanged);
    QSignalSpy anySpy(m_requests.get(), &PlotRequests::rowStatesChanged);

    check("plain");
    check("g");
    check("g2");
    m_requests->flush();
    QCOMPARE(changedSpy.count(), 2);
    QCOMPARE(changedSpy.at(0).at(0).toString(), QStringLiteral("Syn/g"));
    QCOMPARE(changedSpy.at(1).at(0).toString(), QStringLiteral("Syn/g2"));
    QCOMPARE(anySpy.count(), 1);

    // A pass that changes nothing announces nothing
    giveInput({"s1"}, "G_IN", 6);
    QVERIFY(m_requests->hasPendingUpdate());
    m_requests->flush();
    QCOMPARE(changedSpy.count(), 2);
    QCOMPARE(anySpy.count(), 1);

    // A gesture on one row changes both rows that wait on the job, once each,
    // in one pass
    QCOMPARE(m_requests->plotCheckedByUser(QStringLiteral("Syn/g")), 1);
    QCOMPARE(changedSpy.count(), 4);
    QCOMPARE(anySpy.count(), 2);

    // Unchecked: reset to the default state, announced once
    QVERIFY(gate().waitEntered());
    m_requests->flush();
    const int before = int(changedSpy.count());
    check("g2", false);
    m_requests->flush();
    QCOMPARE(int(changedSpy.count()), before + 1);
    QCOMPARE(changedSpy.last().at(0).toString(), QStringLiteral("Syn/g2"));
    m_requests->flush();
    QCOMPARE(int(changedSpy.count()), before + 1);

    for (const QList<QVariant> &emission : changedSpy)
        QVERIFY(emission.at(0).toString() != QLatin1String("Syn/plain"));
    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
}

void PlotRequestsTest::dependencyBurstIsCoalesced()
{
    giveInput({"s1", "s2", "s3"}, "G_IN", 4);
    show({"s1", "s2", "s3"});
    check("g");
    m_requests->flush();
    QVERIFY(!m_requests->hasPendingUpdate());

    // An irrelevant name schedules nothing
    int passes = m_requests->passCount();
    giveInput({"s1"}, "UNRELATED_KEY", 1);
    QVERIFY(!m_requests->hasPendingUpdate());
    QVERIFY(m_model->updateAttribute("s1", QString::fromLatin1(SessionKeys::Description), QStringLiteral("Renamed")));
    QVERIFY(!m_requests->hasPendingUpdate());

    // Many relevant ones in one event-loop pass cause exactly one pass
    giveInput({"s1", "s2", "s3"}, "G_IN", 6);
    giveInput({"s1", "s2", "s3"}, "G_IN", 8);
    QVERIFY(m_requests->hasPendingUpdate());
    QCOMPARE(m_requests->passCount(), passes);
    QTRY_VERIFY(!m_requests->hasPendingUpdate());
    QCOMPARE(m_requests->passCount(), passes + 1);

    // The name is read live at the next pass
    QCOMPARE(row("Syn/g").missing.at(0).sessionName, QStringLiteral("Renamed"));
}

void PlotRequestsTest::progressUpdatesWithoutInspection()
{
    giveInput({"s1", "s2"}, "G_IN", 4);
    show({"s1", "s2"});
    QCOMPARE(checkByUser("g"), 2);
    check("g2");
    QVERIFY(gate().waitEntered());
    const JobId running = jobOf("s1", "gated").id;
    QTRY_COMPARE(m_queue->job(running).progressText, QStringLiteral("step 1"));
    m_requests->flush();

    QSignalSpy changedSpy(m_requests.get(), &PlotRequests::rowStateChanged);
    const int passes = m_requests->passCount();
    const int runs = totalRuns();

    // The queue's own signal, delivered by hand: the text alone changes
    emit m_queue->jobProgress(running, QStringLiteral("iteration 7"));
    QVERIFY(!m_requests->hasPendingUpdate());
    QCOMPARE(m_requests->passCount(), passes);
    QCOMPARE(totalRuns(), runs);
    QCOMPARE(changedSpy.count(), 2);
    for (const char *id : {"Syn/g", "Syn/g2"}) {
        const PlotRowState state = m_requests->rowState(QString::fromLatin1(id));
        QCOMPARE(state.jobProgressText, QStringLiteral("iteration 7"));
        QCOMPARE(state.pending.at(0).jobProgressText, QStringLiteral("iteration 7"));
        QCOMPARE(state.toolTip, QStringLiteral("Computing (0 of 2 done):\n"
                                               "  Jump 1 - Gated: iteration 7\n"
                                               "  Jump 2 - Gated: queued"));
    }

    // The same text again announces nothing; a job nobody waits on neither
    emit m_queue->jobProgress(running, QStringLiteral("iteration 7"));
    emit m_queue->jobProgress(JobId(999), QStringLiteral("elsewhere"));
    QCOMPARE(changedSpy.count(), 2);
    QCOMPARE(m_requests->passCount(), passes);

    gate().open(2);
    QVERIFY(waitIdle(*m_queue));
}

// ---- Robustness ----------------------------------------------------------------------------------------------

void PlotRequestsTest::removedSessionLeavesNoTrace()
{
    giveInput({"s1", "s2", "s3"}, "G_IN", 4);
    show({"s1", "s2", "s3"});
    QCOMPARE(checkByUser("g"), 3);
    QVERIFY(gate().waitEntered());
    const JobId running = jobOf("s1", "gated").id;
    const JobId queued = jobOf("s2", "gated").id;

    QVERIFY(m_model->removeSessions({"s2"}));
    QCOMPARE(stateOf(queued), JobState::Superseded);
    PlotRowState state = row("Syn/g");
    QCOMPARE(sessionIdsOf(state.pending), QStringList({"s1", "s3"}));
    QCOMPARE(state.waitingTotal, 2);
    QCOMPARE(state.progressLabel, QStringLiteral("0 of 2"));

    QVERIFY(m_model->removeSessions({"s1"}));       // the running job is abandoned
    state = row("Syn/g");
    QCOMPARE(sessionIdsOf(state.pending), QStringList({"s3"}));
    QCOMPARE(state.progressLabel, QStringLiteral("0 of 1"));
    QTRY_COMPARE(stateOf(running), JobState::Superseded);

    QVERIFY(gate().waitEntered());                  // s3 runs next
    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QVERIFY(row("Syn/g").isPlain());
    QCOMPARE(values("s3", "g"), QVector<double>({5.0}));
    QCOMPARE(m_queue->model()->rowCount(), 3);
}

// "Explicit-backed" is a function of the registrations, with or without sessions.
void PlotRequestsTest::registryChangeReclassifies()
{
    // Syn/db is two on-demand levels above an explicit output (plotDB <- derivB <- expB)
    check("db");
    check("plain");
    QVERIFY(row("Syn/db").explicitBacked);
    QVERIFY(!row("Syn/plain").explicitBacked);

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
        registry.unregister(QStringLiteral("regX"));
        registry.unregister(QStringLiteral("plotRX"));
    });

    QVector<PlotValue> plots = PlotFixture::plots();
    PlotValue rx = plots.first();
    rx.plotName = QStringLiteral("rx");
    rx.measurementID = QStringLiteral("rx");
    plots.append(rx);
    m_plots->setPlots(plots);
    m_plots->setPlotEnabled("Syn", "rx", true);

    // No session is visible; none is needed
    QVERIFY(!row("Syn/rx").explicitBacked);
    QVERIFY(registry.registerCalculation(regX));
    QVERIFY(m_requests->hasPendingUpdate());
    QVERIFY(row("Syn/rx").explicitBacked);
    QVERIFY(row("Syn/rx").isPlain());

    giveInput({"s1"}, "RX_IN", 1);
    show({"s1"});
    QCOMPARE(row("Syn/rx").missingCount, 1);
    QCOMPARE(row("Syn/rx").missing.at(0).calculationTitles, QStringList({"Reg X"}));

    QSignalSpy changedSpy(m_requests.get(), &PlotRequests::rowStateChanged);
    const Quiet quiet(*m_queue);
    QVERIFY(registry.unregister(QStringLiteral("regX")));
    QVERIFY(row("Syn/rx") == PlotRowState());
    QCOMPARE(changedSpy.count(), 1);
    QCOMPARE(changedSpy.at(0).at(0).toString(), QStringLiteral("Syn/rx"));
    QCOMPARE(m_requests->refreshPressed(QStringLiteral("Syn/rx")), 0);

    QVERIFY(registry.registerCalculation(regX));
    QCOMPARE(row("Syn/rx").missingCount, 1);
    QVERIFY(quiet.holds());
    m_model->flushPendingInvalidations();
}

void PlotRequestsTest::survivesQueueShutdown()
{
    giveInput({"s1", "s2"}, "G_IN", 4);
    show({"s1", "s2"});
    QCOMPARE(checkByUser("g"), 2);
    QVERIFY(gate().waitEntered());

    m_queue->shutdown();
    QVERIFY(m_queue->isIdle());
    PlotRowState state = row("Syn/g");
    QCOMPARE(state.pendingCount, 0);
    QCOMPARE(state.missingCount, 2);

    const Quiet quiet(*m_queue);
    QCOMPARE(m_requests->refreshPressed(QStringLiteral("Syn/g")), 0);
    QCOMPARE(m_requests->plotCheckedByUser(QStringLiteral("Syn/g")), 0);
    QCOMPARE(m_requests->cancelPressed(QStringLiteral("Syn/g")), 0);
    check("g", false);
    check("g");
    show({"s1"}, false);
    state = row("Syn/g");
    QCOMPARE(state.missingCount, 1);        // refused by a closing queue is not "refused": still missing
    QVERIFY(quiet.holds());

    // The component outlives the queue and the models
    m_queue.reset();
    QCOMPARE(m_requests->refreshPressed(QStringLiteral("Syn/g")), 0);
    show({"s1"});                           // schedules a pass: without a queue the component is inert
    m_requests->flush();
    QVERIFY(m_requests->rowState(QStringLiteral("Syn/g")) == PlotRowState());
}

void PlotRequestsTest::nullCollaborators()
{
    giveInput({"s1"}, "G_IN", 4);
    show({"s1"});
    check("g");

    const auto verifyInert = [](PlotRequests &requests) {
        requests.flush();
        QVERIFY(!requests.hasPendingUpdate());
        QVERIFY(requests.rowState(QStringLiteral("Syn/g")) == PlotRowState());
        QCOMPARE(requests.plotCheckedByUser(QStringLiteral("Syn/g")), 0);
        QCOMPARE(requests.refreshPressed(QStringLiteral("Syn/g")), 0);
        QCOMPARE(requests.cancelPressed(QStringLiteral("Syn/g")), 0);
    };

    const Quiet quiet(*m_queue);
    {
        PlotRequests requests(nullptr, m_plots.get(), m_queue.get());
        verifyInert(requests);
    }
    {
        PlotRequests requests(m_model.get(), nullptr, m_queue.get());
        verifyInert(requests);
    }
    {
        PlotRequests requests(m_model.get(), m_plots.get(), nullptr);
        verifyInert(requests);
    }
    {
        PlotRequests requests(nullptr, nullptr, nullptr);
        verifyInert(requests);
    }
    QVERIFY(quiet.holds());

    // The real one is unaffected
    QCOMPARE(row("Syn/g").missingCount, 1);
}

FLYSIGHT_TEST_MAIN(PlotRequestsTest)
#include "tst_plot_requests.moc"
