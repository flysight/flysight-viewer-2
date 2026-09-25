// The plot-row script with the REAL fusion plots: PlotModel + CalculationDemand +
// the executor + SessionModel + Fusion::registerFusionCalculations, with real
// fits on the executor's 64 MiB worker. Sensor-fusion-jobs acceptance 15 end to
// end, and the real-plot halves of 9, 11 and 19. No widgets.
//
// Work follows demand: a checked fusion plot with visible sessions starts their
// fits, one after the other, with no other call.
//
// DETERMINISM WITHOUT A GATE (as in tst_fusion_jobs). A real fit cannot be held
// by a semaphore. Progress posts reach the main thread in order and BEFORE the
// end of the same job is processed, so a slot that acts on the FIRST progress
// text of a job runs while the executor still considers that job Running,
// whatever the worker has done meanwhile. The demand layer's pass on jobFinished
// runs synchronously, before the executor starts the next job (from a queued
// call), so a slot on jobFinished connected after the demand layer sees each
// publication in the state. waitDemandIdle() waits until everything the demand
// layer wanted has run. There are no sleeps.
//
// Expected values are literals and the committed goldens of the kernel.

#include <functional>
#include <memory>

#include <QSignalSpy>
#include <QtTest>

#include "calculationdemand.h"
#include "engine/calculationengine.h"
#include "engine/calculationregistry.h"
#include "fusion/fusionregistration.h"
#include "fusionfixtures.h"
#include "fusiongolden.h"
#include "fusionsessions.h"
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

Q_DECLARE_METATYPE(FlySight::DependencyKey)

namespace {

const QString kFit = QString::fromLatin1(Fusion::FitCalculationId);     // "builtin.fusion.fit"
const QString kAccH = QStringLiteral("builtin.fusion.accH");
const QString kTitle = QStringLiteral("Sensor fusion");
const QString kRoll = QStringLiteral("Fusion/roll");

constexpr int kFitTimeoutMs = 120000;

/// The six "GNSS (Local frame)" plots: ordinary, on demand, never a job.
QVector<PlotValue> localFramePlots()
{
    QVector<PlotValue> plots;
    for (const char *measurement : {"north", "east", "down", "velN", "velE", "velD"}) {
        PlotValue plot;
        plot.category = QStringLiteral("GNSS (Local frame)");
        plot.plotName = QString::fromLatin1(measurement);
        plot.sensorID = QStringLiteral("Local");
        plot.measurementID = QString::fromLatin1(measurement);
        plots.append(plot);
    }
    return plots;
}

} // namespace

class FusionRowsTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void allSeventeenFusionPlotsAreExplicitBacked();
    void realRowScript();
    void rollPitchYawShareOneJob();
    void accHRowIsBlockedByFusion();
    void noImuSessionIsNeverCounted();
    void rejectedTrackShowsBadge();
    void editsAndVisibilityDuringFit();

private:
    /// Into the (empty) model, as the application adds them; empty when that
    /// worked (fusionsessions.h). Check it with QCOMPARE in the test function.
    [[nodiscard]] QString addSessions(const QList<SessionData> &sessions)
    {
        return FlySightTest::addSessions(*m_model, sessions);
    }
    SessionData &session(const QString &id) { return m_model->sessionRef(m_model->getSessionRow(id)); }
    CalculationEngine &engine(const QString &id) { return session(id).calculationEngine(); }
    QVector<double> fusion(const QString &id, const QString &name)
    {
        return session(id).getMeasurement(QStringLiteral("Fusion"), name);
    }
    void show(const QStringList &ids, bool visible = true) { PlotFixture::show(*m_model, ids, visible); }
    /// A programmatic check: the same demand as a click, the Plots menu or a
    /// profile.
    void check(const QString &measurement, bool enabled = true)
    {
        m_plots->setPlotEnabled(QStringLiteral("Fusion"), measurement, enabled);
    }
    void checkAllFusionPlots()
    {
        for (const PlotValue &plot : fusionPlots())
            check(plot.measurementID);
    }
    /// The current plot state: a pending pass runs first.
    DemandState row(const QString &plotId)
    {
        m_demand->flush();
        return m_demand->plotState(plotId);
    }
    /// The newest fit job of a session; a default record (id 0) when none.
    JobRecord fitJobOf(const QString &sessionId) const
    {
        const JobModel *jobs = m_queue->model();
        for (int r = jobs->rowCount() - 1; r >= 0; --r) {
            const JobRecord record = jobs->record(r);
            if (record.sessionId == sessionId && record.calculationId == kFit)
                return record;
        }
        return JobRecord();
    }
    /// The states of every job in the job model, in creation order.
    QList<JobState> history() const
    {
        QList<JobState> states;
        for (int r = 0; r < m_queue->model()->rowCount(); ++r)
            states.append(m_queue->model()->record(r).state);
        return states;
    }
    /// Empty when Fusion/<every channel> of the session matches the golden.
    QString goldenDifference(const QString &id, const QString &goldenName);
    /// Empty when the session `absent` is in no list of any fusion row and no
    /// row counts more than one track; else the first offence.
    QString offenceInRows(const QString &absent);

    std::unique_ptr<SessionModel> m_model;
    std::unique_ptr<JobQueue> m_queue;
    std::unique_ptr<PlotModel> m_plots;
    std::unique_ptr<CalculationDemand> m_demand;
    QStringList m_registryBefore;
};

void FusionRowsTest::initTestCase()
{
    // As the application does: every registration before the logbook and the
    // model exist (a live model reacts to registry changes).
    TestEnvironment::instance().registerBuiltIns();
    registerFusionOnce();

    // One logbook column that reads stored data only
    PreferencesManager::instance().registerPreference(PreferenceKeys::LogbookColumnsVersion, 0);
    LogbookColumnStore::instance().setColumns({descriptionColumn()});

    qRegisterMetaType<DependencyKey>();
}

void FusionRowsTest::init()
{
    TestEnvironment &env = TestEnvironment::instance();
    env.useFreshLogbook();
    env.resetPreferencesToDefaults();
    LogbookManager::instance().initialize();
    m_registryBefore = CalculationRegistry::instance().registeredIds();

    m_model = std::make_unique<SessionModel>();
    m_queue = std::make_unique<JobQueue>(m_model.get());
    m_plots = std::make_unique<PlotModel>();
    m_plots->setPlots(fusionPlots() + localFramePlots());
    m_demand = std::make_unique<CalculationDemand>(m_model.get(), m_plots.get(), m_queue.get());
}

// Note what is to be checked, tear everything down, and only then check: a
// failing check returns from cleanup(), and whatever were still alive then
// would be alive under the next init() (see tst_jobqueue).
void FusionRowsTest::cleanup()
{
    if (m_queue)
        m_queue->shutdown();
    QStringList stillPinned;
    if (m_model) {
        for (int r = 0; r < m_model->rowCount(); ++r) {
            const QString id = std::as_const(*m_model).rowAt(r).sessionId;
            if (m_model->isSessionPinned(id))
                stillPinned.append(id);
        }
    }

    // Reverse order of construction: the demand layer before the executor
    m_demand.reset();
    m_plots.reset();
    m_queue.reset();
    m_model.reset();

    QCOMPARE(stillPinned, QStringList());
    QCOMPARE(CalculationRegistry::instance().registeredIds(), m_registryBefore);
    QCOMPARE(CalculationRegistry::instance().enrolledEngineCount(), 0);
}

QString FusionRowsTest::goldenDifference(const QString &id, const QString &goldenName)
{
    return FlySightTest::goldenDifference(session(id), loadFusionGolden(goldenName));
}

QString FusionRowsTest::offenceInRows(const QString &absent)
{
    for (const PlotValue &plot : fusionPlots()) {
        const QString id = CalculationDemand::plotId(plot);
        const DemandState state = row(id);
        const QStringList listed = sessionIdsOf(state.running) + sessionIdsOf(state.waiting)
            + sessionIdsOf(state.failed);
        if (listed.contains(absent))
            return id + QStringLiteral(" lists ") + absent;
        if (state.toolTip.contains(absent))
            return id + QStringLiteral(" names it in the tooltip");
        if (state.wantedCount > 1 || state.doneCount > 1 || state.waitingCount > 1
            || state.runningCount > 1 || state.failedCount > 1)
            return id + QStringLiteral(" counts more than one track");
    }
    return QString();
}

// Every real fusion plot is requested; the local-frame plots are ordinary.
// Checking all of them with one visible session starts ONE fit, which every
// fusion row waits on and then shows running; the ordinary rows stay plain.
void FusionRowsTest::allSeventeenFusionPlotsAreExplicitBacked()
{
    QCOMPARE(fusionPlots().size(), 17);
    QCOMPARE(addSessions({sessionFromFixture(fusionFixture(QStringLiteral("coarse_linear")), QStringLiteral("s2"))}),
             QString());
    show({"s2"});

    checkAllFusionPlots();
    for (const PlotValue &plot : localFramePlots())
        m_plots->setPlotEnabled(plot.sensorID, plot.measurementID, true);

    // The first pass chose the fit; it starts from the event loop
    m_demand->flush();
    QCOMPARE(m_queue->model()->rowCount(), 1);
    const JobId job = fitJobOf("s2").id;
    QVERIFY(job != 0);
    QCOMPARE(m_queue->chosenNextJob(), job);
    QCOMPARE(m_queue->job(job).calculationTitle, kTitle);

    for (const PlotValue &plot : fusionPlots()) {
        const QString id = CalculationDemand::plotId(plot);
        const DemandState state = row(id);
        QVERIFY2(state.requested, qPrintable(id));
        QCOMPARE(state.sourceId, id);
        QCOMPARE(state.wantedCount, 1);
        QCOMPARE(state.doneCount, 0);
        QCOMPARE(state.waitingCount, 1);
        QCOMPARE(state.runningCount, 0);
        QCOMPARE(state.failedCount, 0);
        QVERIFY(state.isWorking());
        QCOMPARE(state.progressLabel, QStringLiteral("0 of 1"));
        QCOMPARE(state.waiting.at(0).sessionId, QStringLiteral("s2"));
        QCOMPARE(state.waiting.at(0).job, job);
        QCOMPARE(state.waiting.at(0).calculationTitles, QStringList({kTitle}));
    }
    for (const PlotValue &plot : localFramePlots()) {
        const QString id = CalculationDemand::plotId(plot);
        QVERIFY2(row(id) == DemandState(), qPrintable(id));
    }
    // The ordinary plots still read normally, next to the fit that has not run
    QVERIFY(!session("s2").getMeasurement(QStringLiteral("Local"), QStringLiteral("north")).isEmpty());

    // While the fit runs, every fusion row shows it running: the same job
    QString offenceDuring = QStringLiteral("the job reported no progress");
    QObject scope;      // owns the connection: it cannot outlive what the slot captures
    onFirstProgress(*m_queue, &scope, job, [&] {
        offenceDuring.clear();
        for (const PlotValue &plot : fusionPlots()) {
            const QString id = CalculationDemand::plotId(plot);
            const DemandState state = row(id);
            if (state.runningCount != 1 || state.waitingCount != 0 || state.running.at(0).job != job) {
                offenceDuring = id + QStringLiteral(" is not running the fit");
                return;
            }
        }
    });
    QVERIFY(waitDemandIdle(*m_queue, *m_demand, kFitTimeoutMs));
    QVERIFY2(offenceDuring.isEmpty(), qPrintable(offenceDuring));

    QCOMPARE(m_queue->job(job).state, JobState::Succeeded);
    for (const PlotValue &plot : fusionPlots()) {
        const QString id = CalculationDemand::plotId(plot);
        const DemandState state = row(id);
        QVERIFY2(state.isPlain(), qPrintable(id));
        QVERIFY2(state.requested && state.wantedCount == 1 && state.doneCount == 1, qPrintable(id));
    }
    for (const PlotValue &plot : localFramePlots()) {
        const QString id = CalculationDemand::plotId(plot);
        QVERIFY2(row(id) == DemandState(), qPrintable(id));
    }
    QCOMPARE(m_queue->model()->rowCount(), 1);
    QCOMPARE(engine("s2").runCount(kFit), 1);
}

// Acceptance 15, on the real names with real fits: checking the plot computes
// every visible track, one after the other, with no other call; unchecking
// drops the waiting track and lets the running one finish; checking again and
// showing another track compute what is missing.
void FusionRowsTest::realRowScript()
{
    QCOMPARE(addSessions({sessionFromFixture(fusionFixture(QStringLiteral("coarse_maneuver")), QStringLiteral("s1")),
                          sessionFromFixture(fusionFixture(QStringLiteral("coarse_linear")), QStringLiteral("s2")),
                          sessionFromFixture(fusionFixture(QStringLiteral("coarse_linear")), QStringLiteral("s3")),
                          sessionFromFixture(fusionFixture(QStringLiteral("coarse_linear")), QStringLiteral("s4"))}),
             QString());

    // Records, on every job end, what the row shows at that moment (the
    // demand layer's synchronous pass on jobFinished has run: it was
    // connected first)
    struct Seen { JobId job; JobState state; DemandState row; };
    QList<Seen> seen;
    QObject seenScope;      // owns the connection: it cannot outlive `seen`
    connect(m_queue.get(), &JobQueue::jobFinished, &seenScope, [this, &seen](JobId id, JobState state) {
        seen.append({id, state, m_demand->plotState(kRoll)});
    });

    // 1. Three visible fusable tracks, the plot checked programmatically: the
    //    first track's fit is the chosen next job at once, and the row works
    show({"s1", "s2", "s3"});
    check(QStringLiteral("roll"));
    DemandState state = row(kRoll);
    QVERIFY(state.requested);
    QCOMPARE(state.wantedCount, 3);
    QCOMPARE(state.doneCount, 0);
    QCOMPARE(state.waitingCount, 3);
    QCOMPARE(state.runningCount, 0);
    QCOMPARE(state.progressLabel, QStringLiteral("0 of 3"));
    QCOMPARE(sessionIdsOf(state.waiting), QStringList({"s1", "s2", "s3"}));
    QCOMPARE(m_queue->model()->rowCount(), 1);
    const JobId job1 = fitJobOf("s1").id;
    QVERIFY(job1 != 0);
    QCOMPARE(m_queue->chosenNextJob(), job1);
    QCOMPARE(m_queue->job(job1).calculationTitle, kTitle);

    // 2 (armed now, acts later). On the first progress text of s2's fit the
    //    plot is unchecked: s3's chosen next job goes at once, s2's running
    //    job stays. A pending pass runs first, so that s3's job is the chosen
    //    next job whichever event came first after s2's start.
    bool uncheckedWhileRunning = false;
    JobId job2 = 0;
    JobId job3 = 0;
    JobRecord job3AfterUncheck;
    bool job2CancelRequested = true;
    DemandState afterUncheck;
    QObject job2Scope;      // owns the connection: it cannot outlive what the slot captures
    connect(m_queue.get(), &JobQueue::jobProgress, &job2Scope, [&](JobId id, const QString &) {
        if (job2 != 0 || m_queue->job(id).sessionId != QStringLiteral("s2"))
            return;
        job2 = id;
        m_demand->flush();
        job3 = m_queue->chosenNextJob();
        uncheckedWhileRunning = m_queue->job(job2).state == JobState::Running
            && m_queue->job(job3).sessionId == QStringLiteral("s3");
        check(QStringLiteral("roll"), false);
        job3AfterUncheck = m_queue->job(job3);          // at once, before any event-loop turn
        job2CancelRequested = m_queue->job(job2).cancelRequested;
        afterUncheck = m_demand->plotState(kRoll);
        uncheckedWhileRunning = uncheckedWhileRunning && m_queue->job(job2).state == JobState::Running;
    });

    QVERIFY(waitDemandIdle(*m_queue, *m_demand, kFitTimeoutMs));

    // s1 and then s2 ran with no call
    QCOMPARE(m_queue->job(job1).state, JobState::Succeeded);
    QString difference = goldenDifference("s1", QStringLiteral("coarse_maneuver"));
    QVERIFY2(difference.isEmpty(), qPrintable(difference));

    // Unchecking mid-way dropped the chosen next job and let the running one finish
    QVERIFY(job2 != 0 && job3 != 0);
    QVERIFY(uncheckedWhileRunning);
    QCOMPARE(job3AfterUncheck.state, JobState::Cancelled);
    QCOMPARE(job3AfterUncheck.reason, QStringLiteral("No longer needed"));
    QVERIFY(!job3AfterUncheck.startedAt.isValid());
    QVERIFY(!job2CancelRequested);
    QVERIFY(afterUncheck == DemandState());
    QCOMPARE(m_queue->job(job2).state, JobState::Succeeded);
    difference = goldenDifference("s2", QStringLiteral("coarse_linear"));
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
    QVERIFY(fusion("s3", QStringLiteral("roll")).isEmpty());
    QCOMPARE(engine("s3").runCount(kFit), 0);
    QVERIFY(row(kRoll) == DemandState());

    // 3. Checked again: only s3 is missing, and its fit is chosen at once
    check(QStringLiteral("roll"));
    state = row(kRoll);
    QCOMPARE(state.wantedCount, 3);
    QCOMPARE(state.doneCount, 2);
    QCOMPARE(state.waitingCount, 1);
    QCOMPARE(state.progressLabel, QStringLiteral("2 of 3"));
    const JobId job4 = fitJobOf("s3").id;
    QVERIFY(job4 != 0 && job4 != job3);
    QCOMPARE(m_queue->chosenNextJob(), job4);
    QCOMPARE(state.waiting.at(0).job, job4);

    // 4. A fourth track shown while s3's fit runs becomes the chosen next job
    //    with no other call
    JobId job5 = 0;
    DemandState whileS3Runs;
    QObject job4Scope;      // owns the connection: it cannot outlive what the slot captures
    onFirstProgress(*m_queue, &job4Scope, job4, [&] {
        show({"s4"});
        whileS3Runs = row(kRoll);
        job5 = m_queue->chosenNextJob();
    });
    QVERIFY(waitDemandIdle(*m_queue, *m_demand, kFitTimeoutMs));
    QVERIFY(job5 != 0);
    QCOMPARE(m_queue->job(job5).sessionId, QStringLiteral("s4"));
    QCOMPARE(whileS3Runs.wantedCount, 4);
    QCOMPARE(whileS3Runs.doneCount, 2);
    QCOMPARE(whileS3Runs.runningCount, 1);
    QCOMPARE(whileS3Runs.waitingCount, 1);
    QCOMPARE(whileS3Runs.progressLabel, QStringLiteral("2 of 4"));
    QCOMPARE(whileS3Runs.running.at(0).sessionId, QStringLiteral("s3"));
    QCOMPARE(whileS3Runs.running.at(0).job, job4);
    QCOMPARE(whileS3Runs.waiting.at(0).sessionId, QStringLiteral("s4"));
    QCOMPARE(whileS3Runs.waiting.at(0).job, job5);
    QCOMPARE(whileS3Runs.waiting.at(0).calculationTitles, QStringList({kTitle}));

    QCOMPARE(m_queue->job(job4).state, JobState::Succeeded);
    QCOMPARE(m_queue->job(job5).state, JobState::Succeeded);
    QVERIFY(row(kRoll).isPlain());
    QCOMPARE(row(kRoll).wantedCount, 4);
    QCOMPARE(row(kRoll).doneCount, 4);
    difference = goldenDifference("s3", QStringLiteral("coarse_linear"));
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
    difference = goldenDifference("s4", QStringLiteral("coarse_linear"));
    QVERIFY2(difference.isEmpty(), qPrintable(difference));

    // What the row showed at each end: the done count rose with every
    // publication, before the next job started (unchecked in between)
    QCOMPARE(seen.size(), 5);
    QCOMPARE(seen.at(0).job, job1);
    QCOMPARE(seen.at(0).state, JobState::Succeeded);
    QCOMPARE(seen.at(0).row.doneCount, 1);
    QCOMPARE(seen.at(0).row.wantedCount, 3);
    QCOMPARE(seen.at(0).row.progressLabel, QStringLiteral("1 of 3"));
    QCOMPARE(seen.at(1).job, job3);
    QCOMPARE(seen.at(1).state, JobState::Cancelled);
    QCOMPARE(seen.at(2).job, job2);
    QCOMPARE(seen.at(2).state, JobState::Succeeded);
    QVERIFY(seen.at(2).row == DemandState());           // unchecked
    QCOMPARE(seen.at(3).job, job4);
    QCOMPARE(seen.at(3).state, JobState::Succeeded);
    QCOMPARE(seen.at(3).row.doneCount, 3);
    QCOMPARE(seen.at(3).row.wantedCount, 4);
    QCOMPARE(seen.at(3).row.progressLabel, QStringLiteral("3 of 4"));
    QCOMPARE(seen.at(4).job, job5);
    QCOMPARE(seen.at(4).state, JobState::Succeeded);
    QCOMPARE(seen.at(4).row.doneCount, 4);
    QCOMPARE(seen.at(4).row.wantedCount, 4);
    QVERIFY(seen.at(4).row.isPlain());

    // The whole history, from the job model alone
    QCOMPARE(history(), QList<JobState>({JobState::Succeeded, JobState::Succeeded, JobState::Cancelled,
                                         JobState::Succeeded, JobState::Succeeded}));
    for (const QString &id : {QStringLiteral("s1"), QStringLiteral("s2"), QStringLiteral("s3"), QStringLiteral("s4")})
        QCOMPARE(engine(id).runCount(kFit), 1);
}

// Three rows, one fit: roll, pitch and yaw wait on the same job and show the
// same progress.
void FusionRowsTest::rollPitchYawShareOneJob()
{
    QCOMPARE(addSessions({sessionFromFixture(fusionFixture(QStringLiteral("coarse_linear")), QStringLiteral("s2"))}),
             QString());
    show({"s2"});
    const QStringList rows = {kRoll, QStringLiteral("Fusion/pitch"), QStringLiteral("Fusion/yaw")};
    check(QStringLiteral("roll"));
    check(QStringLiteral("pitch"));
    check(QStringLiteral("yaw"));
    QCOMPARE(row(kRoll).waitingCount, 1);

    QCOMPARE(m_queue->model()->rowCount(), 1);
    const JobId job = fitJobOf("s2").id;
    QVERIFY(job != 0);
    for (const QString &id : rows)
        QCOMPARE(row(id).waiting.at(0).job, job);

    QList<DemandState> during;
    QObject scope;      // owns the connection: it cannot outlive what the slot captures
    onFirstProgress(*m_queue, &scope, job, [&] {
        m_demand->flush();
        for (const QString &id : rows)
            during.append(m_demand->plotState(id));
    });
    QVERIFY(waitDemandIdle(*m_queue, *m_demand, kFitTimeoutMs));

    QCOMPARE(during.size(), 3);
    for (const DemandState &state : std::as_const(during)) {
        QCOMPARE(state.runningCount, 1);
        QCOMPARE(state.waitingCount, 0);
        QCOMPARE(state.progressLabel, QStringLiteral("0 of 1"));
        QCOMPARE(state.running.at(0).job, job);
        QVERIFY(!state.running.at(0).progressText.isEmpty());
        QCOMPARE(state.running.at(0).progressText, during.at(0).running.at(0).progressText);
        QCOMPARE(state.toolTip, during.at(0).toolTip);
    }

    QCOMPARE(m_queue->job(job).state, JobState::Succeeded);
    for (const QString &id : rows)
        QVERIFY2(row(id).isPlain(), qPrintable(id));
    QCOMPARE(m_queue->model()->rowCount(), 1);
    QCOMPARE(engine("s2").runCount(kFit), 1);
}

// Fusion/accH is on demand; the row sees through it and the fit is started.
void FusionRowsTest::accHRowIsBlockedByFusion()
{
    QCOMPARE(addSessions({sessionFromFixture(fusionFixture(QStringLiteral("coarse_linear")), QStringLiteral("s2"))}),
             QString());
    show({"s2"});
    const QString accH = QStringLiteral("Fusion/accH");
    check(QStringLiteral("accH"));

    const DemandState state = row(accH);
    QVERIFY(state.requested);
    QCOMPARE(state.wantedCount, 1);
    QCOMPARE(state.waitingCount, 1);
    QCOMPARE(state.waiting.at(0).calculationTitles, QStringList({kTitle}));

    QCOMPARE(m_queue->model()->rowCount(), 1);
    QCOMPARE(m_queue->model()->record(0).calculationId, kFit);
    QVERIFY(waitDemandIdle(*m_queue, *m_demand, kFitTimeoutMs));
    QCOMPARE(m_queue->model()->rowCount(), 1);
    QCOMPARE(m_queue->model()->record(0).state, JobState::Succeeded);
    // An on-demand calculation can never be a job
    QVERIFY(!m_queue->offer("s2", kAccH).created());
    QCOMPARE(m_queue->model()->rowCount(), 1);

    QVERIFY(row(accH).isPlain());
    const QVector<double> values = fusion("s2", QStringLiteral("accH"));
    QCOMPARE(values.size(), 160);           // coarse_linear: 160 output samples
    QCOMPARE(values.size(), fusion("s2", QStringLiteral("_time")).size());
    QCOMPARE(engine("s2").runCount(kAccH), 1);
    QCOMPARE(engine("s2").runCount(kFit), 1);
}

// Acceptance 11 on all seventeen real rows: a session without IMU data is
// never waiting, running or failed, is never counted, and cannot have a job.
void FusionRowsTest::noImuSessionIsNeverCounted()
{
    QCOMPARE(addSessions({sessionFromFixture(fusionFixture(QStringLiteral("coarse_linear")), QStringLiteral("s2")),
                          sessionWithoutImu(fusionFixture(QStringLiteral("coarse_linear")), QStringLiteral("n1"))}),
             QString());
    show({"s2", "n1"});
    checkAllFusionPlots();

    // Before
    QString offence = offenceInRows(QStringLiteral("n1"));
    QVERIFY2(offence.isEmpty(), qPrintable(offence));
    QCOMPARE(row(kRoll).wantedCount, 1);
    QCOMPARE(row(kRoll).waitingCount, 1);
    QCOMPARE(row(kRoll).waiting.at(0).sessionId, QStringLiteral("s2"));

    // s2's fit is started by demand; n1 has none
    QCOMPARE(m_queue->model()->rowCount(), 1);
    const JobId job = fitJobOf("s2").id;
    QVERIFY(job != 0);
    QCOMPARE(fitJobOf("n1").id, JobId(0));

    // During
    QString offenceDuring = QStringLiteral("the job reported no progress");
    int runningDuring = -1;
    QObject scope;      // owns the connection: it cannot outlive what the slot captures
    onFirstProgress(*m_queue, &scope, job, [&] {
        offenceDuring = offenceInRows(QStringLiteral("n1"));
        runningDuring = row(QStringLiteral("Fusion/qw")).runningCount;
    });
    QVERIFY(waitDemandIdle(*m_queue, *m_demand, kFitTimeoutMs));
    QVERIFY2(offenceDuring.isEmpty(), qPrintable(offenceDuring));
    QCOMPARE(runningDuring, 1);             // every row waits on the one job of s2

    // After
    offence = offenceInRows(QStringLiteral("n1"));
    QVERIFY2(offence.isEmpty(), qPrintable(offence));
    for (const PlotValue &plot : fusionPlots())
        QVERIFY2(row(CalculationDemand::plotId(plot)).isPlain(), qPrintable(plot.measurementID));

    QCOMPARE(m_queue->offer("n1", kFit).kind, Kind::MissingInput);
    QCOMPARE(m_queue->model()->rowCount(), 1);
    QCOMPARE(engine("n1").runCount(kFit), 0);
}

// Acceptance 9 seen from the row: a recording the model rejects shows the
// warning badge with the reason and is not run again; when its inputs change
// it waits for them to settle and is then computed.
void FusionRowsTest::rejectedTrackShowsBadge()
{
    QCOMPARE(addSessions({sessionFromFixture(fusionFixture(QStringLiteral("reject_origin")), QStringLiteral("r1"))}),
             QString());
    show({"r1"});
    check(QStringLiteral("roll"));
    QVERIFY(waitDemandIdle(*m_queue, *m_demand, kFitTimeoutMs));
    QCOMPARE(m_queue->model()->rowCount(), 1);
    QCOMPARE(fitJobOf("r1").state, JobState::Succeeded);        // a rejection is a result

    DemandState state = row(kRoll);
    QCOMPARE(state.failedCount, 1);
    QVERIFY(!state.isWorking());
    QVERIFY(state.showsWarning());
    QCOMPARE(state.failed.at(0).sessionId, QStringLiteral("r1"));
    QVERIFY(!state.failed.at(0).jobFailure);
    QCOMPARE(state.failed.at(0).reason,
             QStringLiteral("Sensor fusion: Local origin index outside GNSS samples"));
    QCOMPARE(state.toolTip, QStringLiteral("Could not be computed:\n"
                                           "  r1 - Sensor fusion: Local origin index outside GNSS samples"));

    // No rerun: the same inputs give the same answer
    {
        const Quiet quiet(*m_queue);
        for (int i = 0; i < 3; ++i)
            PlotFixture::spin(m_demand.get());
        QVERIFY(quiet.holds());
        QVERIFY(row(kRoll).showsWarning());
    }

    // reject_origin is coarse_linear with origin index 9: a valid index makes
    // the track wait (settling) at once, and nothing starts during the wait
    m_demand->setInputSettleDelay(60000);
    {
        const Quiet quiet(*m_queue);
        QVERIFY(m_model->updateAttribute("r1", "_LOCAL_ORIGIN_INDEX", QVariant::fromValue(qlonglong(0))));
        state = row(kRoll);
        QCOMPARE(state.failedCount, 0);
        QCOMPARE(state.waitingCount, 1);
        QVERIFY(state.waiting.at(0).settling);
        QVERIFY(state.isWorking());
        PlotFixture::spin(m_demand.get());
        QVERIFY(quiet.holds());
    }

    // Once the inputs have settled, the fit runs again with no other call
    m_demand->endInputSettleWaits();
    QVERIFY(waitDemandIdle(*m_queue, *m_demand, kFitTimeoutMs));
    QCOMPARE(m_queue->model()->rowCount(), 2);
    QCOMPARE(fitJobOf("r1").state, JobState::Succeeded);
    QVERIFY(row(kRoll).isPlain());
    const QString difference = goldenDifference("r1", QStringLiteral("coarse_linear"));
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
    QCOMPARE(engine("r1").runCount(kFit), 2);
}

// Acceptance 19, the half that needs no widget: while a real fit runs, other
// sessions are edited, tracks are hidden and shown, and other values are read,
// all from the main thread; the fit is not disturbed, and the other tracks are
// computed after it with no other action.
void FusionRowsTest::editsAndVisibilityDuringFit()
{
    QCOMPARE(addSessions({sessionFromFixture(fusionFixture(QStringLiteral("coarse_maneuver")), QStringLiteral("s1")),
                          sessionFromFixture(fusionFixture(QStringLiteral("coarse_linear")), QStringLiteral("s2")),
                          sessionFromFixture(fusionFixture(QStringLiteral("coarse_linear")), QStringLiteral("s3"))}),
             QString());
    show({"s1"});
    check(QStringLiteral("roll"));
    QCOMPARE(row(kRoll).waitingCount, 1);
    const JobId job = fitJobOf("s1").id;
    QVERIFY(job != 0);
    QCOMPARE(m_queue->chosenNextJob(), job);

    // More tracks shown before the fit starts wait behind it: s1 stays chosen
    show({"s2", "s3"});
    QCOMPARE(row(kRoll).wantedCount, 3);
    QCOMPARE(row(kRoll).waitingCount, 3);
    QCOMPARE(m_queue->chosenNextJob(), job);
    QCOMPARE(m_queue->model()->rowCount(), 1);

    QSignalSpy dependencySpy(m_model.get(), &SessionModel::dependencyChanged);
    bool running = false, edited = false, settling = true, hidden = false, shownAgain = false;
    qsizetype northSamples = 0;
    int wantedWhileHidden = -1;
    int wantedShownAgain = -1;
    QObject scope;      // owns the connection: it cannot outlive what the slot captures
    onFirstProgress(*m_queue, &scope, job, [&] {
        running = m_queue->job(job).state == JobState::Running;
        edited = m_model->updateAttribute("s2", QString::fromLatin1(SessionKeys::Description),
                                          QStringLiteral("edited"));
        m_model->flushPendingInvalidations();
        settling = m_demand->isSettling(QStringLiteral("s2"));     // an irrelevant name: no wait
        show({"s3"}, false);
        hidden = !session("s3").isVisible();
        wantedWhileHidden = row(kRoll).wantedCount;
        show({"s3"});
        shownAgain = session("s3").isVisible();
        wantedShownAgain = row(kRoll).wantedCount;
        northSamples = session("s2").getMeasurement(QStringLiteral("Local"), QStringLiteral("north")).size();
    });
    QVERIFY(waitDemandIdle(*m_queue, *m_demand, kFitTimeoutMs));

    QVERIFY(running);
    QVERIFY(edited);
    QVERIFY(!settling);
    QVERIFY(hidden);
    QCOMPARE(wantedWhileHidden, 2);         // s1 and s2, while s3 was hidden
    QVERIFY(shownAgain);
    QCOMPARE(wantedShownAgain, 3);
    QCOMPARE(northSamples, qsizetype(9));   // coarse_linear: nine GNSS fixes
    QVERIFY(spyHasAttribute(dependencySpy, "s2", QString::fromLatin1(SessionKeys::Description)));

    // Unrelated edits do not supersede: the fit of s1 ends well and is right
    QCOMPARE(m_queue->job(job).state, JobState::Succeeded);
    QVERIFY2(m_queue->job(job).reason.isEmpty(), qPrintable(m_queue->job(job).reason));
    QString difference = goldenDifference("s1", QStringLiteral("coarse_maneuver"));
    QVERIFY2(difference.isEmpty(), qPrintable(difference));

    // The other tracks were computed after it with no other action
    QCOMPARE(history(), QList<JobState>({JobState::Succeeded, JobState::Succeeded, JobState::Succeeded}));
    QCOMPARE(m_queue->job(fitJobOf("s2").id).state, JobState::Succeeded);
    QCOMPARE(m_queue->job(fitJobOf("s3").id).state, JobState::Succeeded);
    difference = goldenDifference("s2", QStringLiteral("coarse_linear"));
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
    difference = goldenDifference("s3", QStringLiteral("coarse_linear"));
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
    QVERIFY(row(kRoll).isPlain());
    QCOMPARE(row(kRoll).doneCount, 3);

    // The edit was saved by the ordinary idle saver
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(session("s2").getAttribute(SessionKeys::Description).toString(), QStringLiteral("edited"));
    QVERIFY(!std::as_const(*m_model).rowAt(m_model->getSessionRow("s2")).dirty);
    QCOMPARE(indexValue(QStringLiteral("s2"), descriptionColumn()).toString(), QStringLiteral("edited"));
}

FLYSIGHT_TEST_MAIN(FusionRowsTest)
#include "tst_fusion_rows.moc"
