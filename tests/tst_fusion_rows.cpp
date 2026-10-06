// The plot-row script with the REAL fusion plots, the fifteen rows of the
// application's "Sensor fusion" category (fusionPlots()): PlotModel +
// CalculationDemand + the executor + SessionModel +
// Fusion::registerFusionCalculations, with real fits on the executor's 64 MiB
// worker. Sensor-fusion-jobs acceptance 15 end to end, and the real-plot halves
// of 9, 11 and 19. Also what reads a fit channel that has no plot: a logbook
// column kept from before, and the fit's stored record. No widgets.
//
// A row that creates demand here is the Roll row (Fusion/bodyRoll): a check of
// a measurement the model has no row for creates none. The fixture sessions of
// fixtureSession() carry what the attitude reads besides the fit.
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
// publication in the demand layer's progress. waitDemandIdle() waits until
// everything the demand layer wanted has run. There are no sleeps.
//
// What a row is doing is asserted through what the demand layer presents
// (progress and failures) and the executor's jobs.
//
// Expected values are literals and the committed goldens of the kernel.

#include <functional>
#include <memory>

#include <QScopeGuard>
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

using BlockerState = BlockerReport::State;

Q_DECLARE_METATYPE(FlySight::DependencyKey)

namespace {

const QString kFit = QString::fromLatin1(Fusion::FitCalculationId);     // "builtin.fusion.fit"
const QString kVelH = QStringLiteral("builtin.fusion.velH");
const QString kVel = QStringLiteral("builtin.fusion.vel");
const QString kAccH = QStringLiteral("builtin.fusion.accH");
const QString kTitle = QStringLiteral("Sensor fusion");
const QString kRoll = QStringLiteral("bodyRoll");        // the Roll row

constexpr int kFitTimeoutMs = 120000;

/// <sensor>/<measurement> at the exit marker, of the given measurement type.
LogbookColumn atExitColumn(const QString &sensor, const QString &measurement, const QString &type)
{
    LogbookColumn column;
    column.type = ColumnType::MeasurementAtMarker;
    column.sensorID = sensor;
    column.measurementID = measurement;
    column.measurementType = type;
    column.markerAttributeKey = QString::fromLatin1(SessionKeys::ExitTime);
    return column;
}

} // namespace

class FusionRowsTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void allFusionPlotsAreExplicitBacked();
    void accuracyPlotsAreAbsentWithoutAFit();
    void realRowScript();
    void headingPitchRollShareOneJob();
    void accHRowIsBlockedByFusion();
    void totalSpeedRowIsBlockedByFusion();
    void noImuSessionIsNeverCounted();
    void rejectedTrackShowsBadge();
    void editsAndVisibilityDuringFit();
    void removedPlotMeasurementsStayAvailable();

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
    /// The current progress: a pending pass runs first.
    DemandProgress progressNow()
    {
        m_demand->flush();
        return m_demand->progress();
    }
    /// Nothing to compute and nothing failed: a pending pass runs first.
    bool nothingToShow()
    {
        return progressNow().count == 0 && m_demand->failures().isEmpty();
    }
    /// Whether the fusion value of the plot waits on (or was rejected by) a
    /// requested calculation for the session: the demand layer's own
    /// inspection, which runs nothing.
    bool merelyUncomputed(const QString &id, const PlotValue &plot)
    {
        return CalculationDemand::isMerelyUncomputed(session(id), plot.sensorID, plot.measurementID);
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
    /// Empty when the session `absent` is never the recording being computed,
    /// has no failure, and at most one session is counted; else the first
    /// offence.
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

    // The application's fusion rows, so that column labels resolve as there
    // (nothing else in this executable reads the plot registry)
    for (const PlotValue &plot : fusionPlots())
        PlotRegistry::instance().registerPlot(plot);

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
    m_plots->setPlots(fusionPlots());
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
    const DemandProgress progress = progressNow();
    if (progress.sessionName == absent)
        return QStringLiteral("the progress names ") + absent;
    if (!m_demand->sessionFailures(absent).calculations.isEmpty())
        return absent + QStringLiteral(" is listed among the failures");
    if (progress.count > 1 || progress.highWater > 1)
        return QStringLiteral("the progress counts more than one session");
    return QString();
}

// Every one of the fifteen fusion plots is requested: its value waits on the
// fit, which is its only requested calculation. Checking all fifteen with one
// visible session (with a ground elevation, which Elevation needs) starts ONE
// fit, which every value waits on; the computations count that one session.
// After it every row has a value on the fit's time axis.
void FusionRowsTest::allFusionPlotsAreExplicitBacked()
{
    // The mirror of the application's rows, literally
    struct Row { const char *name; const char *units; const char *measurement; const char *type; };
    const Row expected[] = {
        {"Elevation",                "m",     "z",             "altitude"},
        {"Horizontal speed",         "m/s",   "velH",          "speed"},
        {"Vertical speed",           "m/s",   "velD",          "vertical_speed"},
        {"Total speed",              "m/s",   "vel",           "speed"},
        {"Horizontal acceleration",  "m/s^2", "accH",          "acceleration"},
        {"Vertical acceleration",    "m/s^2", "accD",          "acceleration"},
        {"Along-track acceleration", "m/s^2", "accAlongTrack", "acceleration"},
        {"Cross-track acceleration", "m/s^2", "accCrossTrack", "acceleration"},
        {"Heading",                  "deg",   "bodyHeading",   "angle"},
        {"Pitch",                    "deg",   "bodyPitch",     "angle"},
        {"Roll",                     "deg",   "bodyRoll",      "angle"},
        {"Heading accuracy",                 "deg",   "headingAcc", "angle"},
        {"Tilt accuracy",                    "deg",   "tiltAcc",    "angle"},
        {"Horizontal acceleration accuracy", "m/s^2", "accHAcc",    "acceleration_accuracy"},
        {"Vertical acceleration accuracy",   "m/s^2", "accDAcc",    "acceleration_accuracy"},
    };
    const QVector<PlotValue> plots = fusionPlots();
    QCOMPARE(plots.size(), 15);
    for (int i = 0; i < plots.size(); ++i) {
        const PlotValue &plot = plots.at(i);
        QCOMPARE(plot.category, QStringLiteral("Sensor fusion"));
        QCOMPARE(plot.plotName, QString::fromLatin1(expected[i].name));
        QCOMPARE(plot.plotUnits, QString::fromLatin1(expected[i].units));
        QCOMPARE(plot.sensorID, QStringLiteral("Fusion"));
        QCOMPARE(plot.measurementID, QString::fromLatin1(expected[i].measurement));
        QCOMPARE(plot.measurementType, QString::fromLatin1(expected[i].type));
        QVERIFY(plot.role == PlotRole::Dependent);
    }

    // The registry: each row's one requested calculation is the fit
    for (const PlotValue &plot : plots) {
        QCOMPARE(CalculationRegistry::instance().explicitDependencies(fusionKey(plot.measurementID)),
                 QStringList({kFit}));
    }

    SessionData s2 = fixtureSession(QStringLiteral("coarse_linear"), QStringLiteral("s2"));
    s2.setAttribute(SessionKeys::GroundElev, 100.0);
    QCOMPARE(addSessions({s2}), QString());
    show({"s2"});

    checkAllFusionPlots();

    // The first pass chose the fit; it starts from the event loop
    m_demand->flush();
    QCOMPARE(m_queue->model()->rowCount(), 1);
    const JobId job = fitJobOf("s2").id;
    QVERIFY(job != 0);
    QCOMPARE(m_queue->chosenNextJob(), job);
    QCOMPARE(m_queue->job(job).calculationTitle, kTitle);
    QCOMPARE(progressNow().count, 1);
    QVERIFY(m_demand->failures().isEmpty());

    // Every fusion value waits on a requested calculation
    for (const PlotValue &plot : plots)
        QVERIFY2(merelyUncomputed(QStringLiteral("s2"), plot), qPrintable(plot.measurementID));
    // The local frame, which has no plot, still reads normally next to the
    // fit that has not run
    QVERIFY(!session("s2").getMeasurement(QStringLiteral("Local"), QStringLiteral("north")).isEmpty());

    // While the fit runs, the computations are that one job
    QString offenceDuring = QStringLiteral("the job reported no progress");
    QObject scope;      // owns the connection: it cannot outlive what the slot captures
    onFirstProgress(*m_queue, &scope, job, [&] {
        const DemandProgress progress = progressNow();
        if (progress.count != 1 || progress.sessionName != QLatin1String("s2") || m_queue->runningJob() != job)
            offenceDuring = QStringLiteral("the progress is not the fit of s2");
        else
            offenceDuring.clear();
    });
    QVERIFY(waitDemandIdle(*m_queue, *m_demand, kFitTimeoutMs));
    QVERIFY2(offenceDuring.isEmpty(), qPrintable(offenceDuring));

    QCOMPARE(m_queue->job(job).state, JobState::Succeeded);
    QVERIFY(nothingToShow());
    const qsizetype samples = fusion("s2", QStringLiteral("_time")).size();
    QVERIFY(samples > 0);
    for (const PlotValue &plot : plots) {
        QVERIFY2(!merelyUncomputed(QStringLiteral("s2"), plot), qPrintable(plot.measurementID));
        QVERIFY2(fusion("s2", plot.measurementID).size() == samples, qPrintable(plot.measurementID));
    }
    QCOMPARE(m_queue->model()->rowCount(), 1);
    QCOMPARE(engine("s2").runCount(kFit), 1);
}

// The four accuracy plots are absent, like any unavailable value, where the
// fit did not compute them. Before the fit they merely wait on it; for a
// recording the model rejects they are not produced, and the recording is
// listed once, with the fit's reason, however many of the four are checked;
// for a session without IMU data none of the fifteen applies, silently.
void FusionRowsTest::accuracyPlotsAreAbsentWithoutAFit()
{
    QVector<PlotValue> accuracies;
    for (const PlotValue &plot : fusionPlots()) {
        if (plot.plotName.endsWith(QStringLiteral(" accuracy")))
            accuracies.append(plot);
    }
    QCOMPARE(accuracies.size(), 4);

    QCOMPARE(addSessions({fixtureSession(QStringLiteral("coarse_linear"), QStringLiteral("s2")),
                          fixtureSession(QStringLiteral("reject_origin"), QStringLiteral("r1")),
                          sessionWithoutImu(fusionFixture(QStringLiteral("coarse_linear")), QStringLiteral("n1"))}),
             QString());
    show({"s2", "r1", "n1"});
    for (const PlotValue &plot : accuracies)
        check(plot.measurementID);

    // Before the fits: the two fusable recordings' accuracies wait on them;
    // the session without IMU data is never counted
    QCOMPARE(progressNow().count, 2);
    QVERIFY(m_demand->failures().isEmpty());
    for (const PlotValue &plot : accuracies) {
        for (const QString &id : {QStringLiteral("s2"), QStringLiteral("r1")}) {
            QVERIFY2(merelyUncomputed(id, plot), qPrintable(id + QLatin1Char('/') + plot.measurementID));
            QVERIFY2(CalculationDemand::isNotYetComputed(session(id), plot.sensorID, plot.measurementID),
                     qPrintable(id + QLatin1Char('/') + plot.measurementID));
            QVERIFY2(fusion(id, plot.measurementID).isEmpty(), qPrintable(id + QLatin1Char('/') + plot.measurementID));
        }
    }
    for (const PlotValue &plot : fusionPlots()) {
        const BlockerReport report = engine("n1").blockers(fusionKey(plot.measurementID));
        QVERIFY2(report.state == BlockerState::NotApplicable, qPrintable(plot.measurementID));
        QVERIFY(report.blockers.isEmpty());
        QVERIFY(!merelyUncomputed(QStringLiteral("n1"), plot));
    }

    QVERIFY(waitDemandIdle(*m_queue, *m_demand, kFitTimeoutMs));
    QCOMPARE(fitJobOf("s2").state, JobState::Succeeded);
    QCOMPARE(fitJobOf("r1").state, JobState::Succeeded);        // a rejection is a result
    QCOMPARE(fitJobOf("n1").id, JobId(0));

    // The success has them; the rejection does not, and is listed once
    const qsizetype samples = fusion("s2", QStringLiteral("_time")).size();
    QVERIFY(samples > 0);
    for (const PlotValue &plot : accuracies) {
        QCOMPARE(fusion("s2", plot.measurementID).size(), samples);
        QVERIFY2(fusion("r1", plot.measurementID).isEmpty(), qPrintable(plot.measurementID));
        // a failure, not a value on its way
        QVERIFY2(!CalculationDemand::isNotYetComputed(session("r1"), plot.sensorID, plot.measurementID),
                 qPrintable(plot.measurementID));
        const BlockerReport report = engine("r1").blockers(fusionKey(plot.measurementID));
        QVERIFY2(report.state == BlockerState::NotProduced, qPrintable(plot.measurementID));
    }
    m_demand->flush();
    const QList<SessionFailures> failures = m_demand->failures();
    QCOMPARE(failures.size(), 1);
    QCOMPARE(failures.at(0).sessionId, QStringLiteral("r1"));
    const FailedCalculation expected{kFit, kTitle, QStringLiteral("Local origin index outside GNSS samples"), false};
    QVERIFY(failures.at(0).calculations == QList<FailedCalculation>({expected}));
    QCOMPARE(progressNow().count, 0);

    // The session without IMU data: still nothing, and no fit
    for (const PlotValue &plot : fusionPlots())
        QVERIFY2(engine("n1").blockers(fusionKey(plot.measurementID)).state == BlockerState::NotApplicable,
                 qPrintable(plot.measurementID));
    const QString offence = offenceInRows(QStringLiteral("n1"));
    QVERIFY2(offence.isEmpty(), qPrintable(offence));
    QCOMPARE(engine("n1").runCount(kFit), 0);
    QCOMPARE(engine("s2").runCount(kFit), 1);
    QCOMPARE(engine("r1").runCount(kFit), 1);
}

// Acceptance 15, on the real names with real fits: checking the plot computes
// every visible track, one after the other, with no other call; unchecking
// drops the waiting track and lets the running one finish; checking again and
// showing another track compute what is missing.
void FusionRowsTest::realRowScript()
{
    QCOMPARE(addSessions({fixtureSession(QStringLiteral("coarse_maneuver"), QStringLiteral("s1")),
                          fixtureSession(QStringLiteral("coarse_linear"), QStringLiteral("s2")),
                          fixtureSession(QStringLiteral("coarse_linear"), QStringLiteral("s3")),
                          fixtureSession(QStringLiteral("coarse_linear"), QStringLiteral("s4"))}),
             QString());

    // Records, on every job end, the progress at that moment (the demand
    // layer's synchronous pass on jobFinished has run: it was connected first)
    struct Seen { JobId job; JobState state; DemandProgress progress; };
    QList<Seen> seen;
    QObject seenScope;      // owns the connection: it cannot outlive `seen`
    connect(m_queue.get(), &JobQueue::jobFinished, &seenScope, [this, &seen](JobId id, JobState state) {
        seen.append({id, state, m_demand->progress()});
    });

    // 1. Three visible fusable tracks, the plot checked programmatically: the
    //    first track's fit is the chosen next job at once, and all three are
    //    to compute
    show({"s1", "s2", "s3"});
    check(kRoll);
    QCOMPARE(progressNow().count, 3);
    QCOMPARE(progressNow().highWater, 3);
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
    DemandProgress afterUncheck;
    QObject job2Scope;      // owns the connection: it cannot outlive what the slot captures
    connect(m_queue.get(), &JobQueue::jobProgress, &job2Scope, [&](JobId id, const QString &) {
        if (job2 != 0 || m_queue->job(id).sessionId != QStringLiteral("s2"))
            return;
        job2 = id;
        m_demand->flush();
        job3 = m_queue->chosenNextJob();
        uncheckedWhileRunning = m_queue->job(job2).state == JobState::Running
            && m_queue->job(job3).sessionId == QStringLiteral("s3");
        check(kRoll, false);
        job3AfterUncheck = m_queue->job(job3);          // at once, before any event-loop turn
        job2CancelRequested = m_queue->job(job2).cancelRequested;
        afterUncheck = m_demand->progress();
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
    QCOMPARE(afterUncheck.count, 0);
    QCOMPARE(m_queue->job(job2).state, JobState::Succeeded);
    difference = goldenDifference("s2", QStringLiteral("coarse_linear"));
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
    QVERIFY(fusion("s3", QStringLiteral("roll")).isEmpty());
    QCOMPARE(engine("s3").runCount(kFit), 0);
    QVERIFY(nothingToShow());

    // 3. Checked again: only s3 is missing, and its fit is chosen at once
    check(kRoll);
    QCOMPARE(progressNow().count, 1);
    QCOMPARE(progressNow().highWater, 1);
    const JobId job4 = fitJobOf("s3").id;
    QVERIFY(job4 != 0 && job4 != job3);
    QCOMPARE(m_queue->chosenNextJob(), job4);

    // 4. A fourth track shown while s3's fit runs becomes the chosen next job
    //    with no other call
    JobId job5 = 0;
    JobId runningWhileS3Runs = 0;
    DemandProgress whileS3Runs;
    QObject job4Scope;      // owns the connection: it cannot outlive what the slot captures
    onFirstProgress(*m_queue, &job4Scope, job4, [&] {
        show({"s4"});
        whileS3Runs = progressNow();
        job5 = m_queue->chosenNextJob();
        runningWhileS3Runs = m_queue->runningJob();
    });
    QVERIFY(waitDemandIdle(*m_queue, *m_demand, kFitTimeoutMs));
    QVERIFY(job5 != 0);
    QCOMPARE(m_queue->job(job5).sessionId, QStringLiteral("s4"));
    QCOMPARE(whileS3Runs.count, 2);             // s3 running, s4 waiting
    QCOMPARE(whileS3Runs.highWater, 2);
    QCOMPARE(whileS3Runs.sessionName, QStringLiteral("s3"));
    QCOMPARE(runningWhileS3Runs, job4);
    QCOMPARE(m_queue->job(runningWhileS3Runs).sessionId, QStringLiteral("s3"));
    QCOMPARE(m_queue->job(job5).calculationTitle, kTitle);

    QCOMPARE(m_queue->job(job4).state, JobState::Succeeded);
    QCOMPARE(m_queue->job(job5).state, JobState::Succeeded);
    QVERIFY(nothingToShow());
    difference = goldenDifference("s3", QStringLiteral("coarse_linear"));
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
    difference = goldenDifference("s4", QStringLiteral("coarse_linear"));
    QVERIFY2(difference.isEmpty(), qPrintable(difference));

    // The progress at each end: the sessions still to compute fell with every
    // publication, before the next job started (unchecked in between)
    QCOMPARE(seen.size(), 5);
    QCOMPARE(seen.at(0).job, job1);
    QCOMPARE(seen.at(0).state, JobState::Succeeded);
    QCOMPARE(seen.at(0).progress.count, 2);
    QCOMPARE(seen.at(0).progress.highWater, 3);
    QCOMPARE(seen.at(1).job, job3);
    QCOMPARE(seen.at(1).state, JobState::Cancelled);
    QCOMPARE(seen.at(2).job, job2);
    QCOMPARE(seen.at(2).state, JobState::Succeeded);
    QCOMPARE(seen.at(2).progress.count, 0);             // unchecked
    QCOMPARE(seen.at(3).job, job4);
    QCOMPARE(seen.at(3).state, JobState::Succeeded);
    QCOMPARE(seen.at(3).progress.count, 1);
    QCOMPARE(seen.at(3).progress.highWater, 2);
    QCOMPARE(seen.at(4).job, job5);
    QCOMPARE(seen.at(4).state, JobState::Succeeded);
    QVERIFY(seen.at(4).progress == DemandProgress());

    // The whole history, from the job model alone
    QCOMPARE(history(), QList<JobState>({JobState::Succeeded, JobState::Succeeded, JobState::Cancelled,
                                         JobState::Succeeded, JobState::Succeeded}));
    for (const QString &id : {QStringLiteral("s1"), QStringLiteral("s2"), QStringLiteral("s3"), QStringLiteral("s4")})
        QCOMPARE(engine(id).runCount(kFit), 1);
}

// Three rows, one fit: Heading, Pitch and Roll wait on the same job, and the
// computations are that one session and its progress.
void FusionRowsTest::headingPitchRollShareOneJob()
{
    QCOMPARE(addSessions({fixtureSession(QStringLiteral("coarse_linear"), QStringLiteral("s2"))}), QString());
    show({"s2"});
    check(QStringLiteral("bodyHeading"));
    check(QStringLiteral("bodyPitch"));
    check(kRoll);
    QCOMPARE(progressNow().count, 1);

    QCOMPARE(m_queue->model()->rowCount(), 1);
    const JobId job = fitJobOf("s2").id;
    QVERIFY(job != 0);
    QCOMPARE(m_queue->chosenNextJob(), job);

    DemandProgress during;
    JobId runningDuring = 0;
    QObject scope;      // owns the connection: it cannot outlive what the slot captures
    onFirstProgress(*m_queue, &scope, job, [&] {
        during = progressNow();
        runningDuring = m_queue->runningJob();
    });
    QVERIFY(waitDemandIdle(*m_queue, *m_demand, kFitTimeoutMs));
    QCOMPARE(runningDuring, job);

    // One value for the three plots: one session, its name and a progress text
    QCOMPARE(during.count, 1);
    QCOMPARE(during.sessionName, QStringLiteral("s2"));
    QVERIFY(!during.progressText.isEmpty());

    QCOMPARE(m_queue->job(job).state, JobState::Succeeded);
    QVERIFY(nothingToShow());
    for (const char *name : {"bodyHeading", "bodyPitch", "bodyRoll"})
        QVERIFY2(!fusion("s2", QString::fromLatin1(name)).isEmpty(), name);
    QCOMPARE(m_queue->model()->rowCount(), 1);
    QCOMPARE(engine("s2").runCount(kFit), 1);
}

// Fusion/accH is on demand; the row sees through it and the fit is started.
void FusionRowsTest::accHRowIsBlockedByFusion()
{
    QCOMPARE(addSessions({sessionFromFixture(fusionFixture(QStringLiteral("coarse_linear")), QStringLiteral("s2"))}),
             QString());
    show({"s2"});
    check(QStringLiteral("accH"));

    // In demand: the value waits on the fit
    QCOMPARE(progressNow().count, 1);
    QVERIFY(CalculationDemand::isMerelyUncomputed(session("s2"), QStringLiteral("Fusion"), QStringLiteral("accH")));

    QCOMPARE(m_queue->model()->rowCount(), 1);
    QCOMPARE(m_queue->model()->record(0).calculationTitle, kTitle);
    QCOMPARE(m_queue->model()->record(0).calculationId, kFit);
    QVERIFY(waitDemandIdle(*m_queue, *m_demand, kFitTimeoutMs));
    QCOMPARE(m_queue->model()->rowCount(), 1);
    QCOMPARE(m_queue->model()->record(0).state, JobState::Succeeded);
    // An on-demand calculation can never be a job
    QVERIFY(!m_queue->offer("s2", kAccH).created());
    QCOMPARE(m_queue->model()->rowCount(), 1);

    QVERIFY(nothingToShow());
    const QVector<double> values = fusion("s2", QStringLiteral("accH"));
    QCOMPARE(values.size(), 160);           // coarse_linear: 160 output samples
    QCOMPARE(values.size(), fusion("s2", QStringLiteral("_time")).size());
    QCOMPARE(engine("s2").runCount(kAccH), 1);
    QCOMPARE(engine("s2").runCount(kFit), 1);
}

// Fusion/vel is on demand and reads Fusion/velH, also on demand: the Total
// speed row alone sees through both to the fit, one job is created for it,
// and after it the speed lies on the fit's time axis, each calculation run
// once.
void FusionRowsTest::totalSpeedRowIsBlockedByFusion()
{
    QCOMPARE(addSessions({sessionFromFixture(fusionFixture(QStringLiteral("coarse_linear")), QStringLiteral("s2"))}),
             QString());
    show({"s2"});
    check(QStringLiteral("vel"));

    // In demand: the value waits on the fit
    QCOMPARE(progressNow().count, 1);
    QVERIFY(CalculationDemand::isMerelyUncomputed(session("s2"), QStringLiteral("Fusion"), QStringLiteral("vel")));

    QCOMPARE(m_queue->model()->rowCount(), 1);
    QCOMPARE(m_queue->model()->record(0).calculationTitle, kTitle);
    QCOMPARE(m_queue->model()->record(0).calculationId, kFit);
    QVERIFY(waitDemandIdle(*m_queue, *m_demand, kFitTimeoutMs));
    QCOMPARE(m_queue->model()->rowCount(), 1);
    QCOMPARE(m_queue->model()->record(0).state, JobState::Succeeded);
    // An on-demand calculation can never be a job
    QVERIFY(!m_queue->offer("s2", kVel).created());
    QCOMPARE(m_queue->model()->rowCount(), 1);

    QVERIFY(nothingToShow());
    const QVector<double> values = fusion("s2", QStringLiteral("vel"));
    QCOMPARE(values.size(), fusion("s2", QStringLiteral("_time")).size());
    QVERIFY(!values.isEmpty());
    QCOMPARE(engine("s2").runCount(kFit), 1);
    QCOMPARE(engine("s2").runCount(kVelH), 1);
    QCOMPARE(engine("s2").runCount(kVel), 1);
}

// Acceptance 11 on all fifteen real rows: a session without IMU data is
// never counted in progress nor listed among failures, and cannot have a job.
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
    QCOMPARE(progressNow().count, 1);

    // s2's fit is started by demand; n1 has none
    QCOMPARE(m_queue->model()->rowCount(), 1);
    const JobId job = fitJobOf("s2").id;
    QVERIFY(job != 0);
    QCOMPARE(fitJobOf("n1").id, JobId(0));

    // During
    QString offenceDuring = QStringLiteral("the job reported no progress");
    QString runningDuring;
    QObject scope;      // owns the connection: it cannot outlive what the slot captures
    onFirstProgress(*m_queue, &scope, job, [&] {
        offenceDuring = offenceInRows(QStringLiteral("n1"));
        runningDuring = progressNow().sessionName;
    });
    QVERIFY(waitDemandIdle(*m_queue, *m_demand, kFitTimeoutMs));
    QVERIFY2(offenceDuring.isEmpty(), qPrintable(offenceDuring));
    QCOMPARE(runningDuring, QStringLiteral("s2"));      // every row waits on the one job of s2

    // After
    offence = offenceInRows(QStringLiteral("n1"));
    QVERIFY2(offence.isEmpty(), qPrintable(offence));
    QVERIFY(nothingToShow());

    QCOMPARE(m_queue->offer("n1", kFit).kind, Kind::MissingInput);
    QCOMPARE(m_queue->model()->rowCount(), 1);
    QCOMPARE(engine("n1").runCount(kFit), 0);
}

// Acceptance 9 seen from the row: a recording the model rejects is listed
// among the failures with the reason and is not run again; when its inputs
// change it waits for them to settle and is then computed.
void FusionRowsTest::rejectedTrackShowsBadge()
{
    QCOMPARE(addSessions({fixtureSession(QStringLiteral("reject_origin"), QStringLiteral("r1"))}), QString());
    show({"r1"});
    check(kRoll);
    QVERIFY(waitDemandIdle(*m_queue, *m_demand, kFitTimeoutMs));
    QCOMPARE(m_queue->model()->rowCount(), 1);
    QCOMPARE(fitJobOf("r1").state, JobState::Succeeded);        // a rejection is a result

    // A stored rejection: not tried again at the next start
    const auto verifyListed = [this] {
        m_demand->flush();
        const QList<SessionFailures> failures = m_demand->failures();
        QCOMPARE(failures.size(), 1);
        QCOMPARE(failures.at(0).sessionId, QStringLiteral("r1"));
        QCOMPARE(failures.at(0).sessionName, QStringLiteral("r1"));
        const FailedCalculation expected{kFit, kTitle, QStringLiteral("Local origin index outside GNSS samples"),
                                         false};
        QVERIFY(failures.at(0).calculations == QList<FailedCalculation>({expected}));
        QCOMPARE(m_demand->sessionFailures(QStringLiteral("r1")).text(),
                 QStringLiteral("Sensor fusion: Local origin index outside GNSS samples"));
        QCOMPARE(m_demand->progress().count, 0);
    };
    verifyListed();
    if (QTest::currentTestFailed())
        return;

    // No rerun: the same inputs give the same answer
    {
        const Quiet quiet(*m_queue);
        for (int i = 0; i < 3; ++i)
            PlotFixture::spin(m_demand.get());
        QVERIFY(quiet.holds());
        verifyListed();
        if (QTest::currentTestFailed())
            return;
    }

    // reject_origin is coarse_linear with origin index 9: a valid index makes
    // the track wait (settling) at once, and nothing starts during the wait
    m_demand->setInputSettleDelay(60000);
    {
        const Quiet quiet(*m_queue);
        QVERIFY(m_model->updateAttribute("r1", "_LOCAL_ORIGIN_INDEX", QVariant::fromValue(qlonglong(0))));
        QCOMPARE(progressNow().count, 1);
        QVERIFY(m_demand->failures().isEmpty());
        QVERIFY(m_demand->isSettling(QStringLiteral("r1")));
        PlotFixture::spin(m_demand.get());
        QVERIFY(quiet.holds());
    }

    // Once the inputs have settled, the fit runs again with no other call
    m_demand->endInputSettleWaits();
    QVERIFY(waitDemandIdle(*m_queue, *m_demand, kFitTimeoutMs));
    QCOMPARE(m_queue->model()->rowCount(), 2);
    QCOMPARE(fitJobOf("r1").state, JobState::Succeeded);
    QVERIFY(nothingToShow());
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
    QCOMPARE(addSessions({fixtureSession(QStringLiteral("coarse_maneuver"), QStringLiteral("s1")),
                          fixtureSession(QStringLiteral("coarse_linear"), QStringLiteral("s2")),
                          fixtureSession(QStringLiteral("coarse_linear"), QStringLiteral("s3"))}),
             QString());
    show({"s1"});
    check(kRoll);
    QCOMPARE(progressNow().count, 1);
    const JobId job = fitJobOf("s1").id;
    QVERIFY(job != 0);
    QCOMPARE(m_queue->chosenNextJob(), job);

    // More tracks shown before the fit starts wait behind it: s1 stays chosen
    show({"s2", "s3"});
    QCOMPARE(progressNow().count, 3);
    QCOMPARE(m_queue->chosenNextJob(), job);
    QCOMPARE(m_queue->model()->rowCount(), 1);

    QSignalSpy dependencySpy(m_model.get(), &SessionModel::dependencyChanged);
    bool running = false, edited = false, settling = true, hidden = false, shownAgain = false;
    qsizetype northSamples = 0;
    int countWhileHidden = -1;
    int countShownAgain = -1;
    QObject scope;      // owns the connection: it cannot outlive what the slot captures
    onFirstProgress(*m_queue, &scope, job, [&] {
        running = m_queue->job(job).state == JobState::Running;
        edited = m_model->updateAttribute("s2", QString::fromLatin1(SessionKeys::Description),
                                          QStringLiteral("edited"));
        m_model->flushPendingInvalidations();
        settling = m_demand->isSettling(QStringLiteral("s2"));     // an irrelevant name: no wait
        show({"s3"}, false);
        hidden = !session("s3").isVisible();
        countWhileHidden = progressNow().count;
        show({"s3"});
        shownAgain = session("s3").isVisible();
        countShownAgain = progressNow().count;
        northSamples = session("s2").getMeasurement(QStringLiteral("Local"), QStringLiteral("north")).size();
    });
    QVERIFY(waitDemandIdle(*m_queue, *m_demand, kFitTimeoutMs));

    QVERIFY(running);
    QVERIFY(edited);
    QVERIFY(!settling);
    QVERIFY(hidden);
    QCOMPARE(countWhileHidden, 2);          // s1 and s2, while s3 was hidden
    QVERIFY(shownAgain);
    QCOMPARE(countShownAgain, 3);
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
    QVERIFY(nothingToShow());

    // The edit was saved by the ordinary idle saver
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(session("s2").getAttribute(SessionKeys::Description).toString(), QStringLiteral("edited"));
    QVERIFY(!std::as_const(*m_model).rowAt(m_model->getSessionRow("s2")).dirty);
    QCOMPARE(indexValue(QStringLiteral("s2"), descriptionColumn()).toString(), QStringLiteral("edited"));
}

// What reads a measurement behind a removed plot (the fit's own roll, the
// local frame): a logbook column kept from before computes, labelled with the
// measurement's name since no plot names it, while a column over the Roll row
// takes the row's name; and the fit's stored record, the one place a fit
// channel leaves the process, carries every fit channel the fit publishes and
// no derived calculation produces, plotted (velD) or not.
void FusionRowsTest::removedPlotMeasurementsStayAvailable()
{
    const auto restore = qScopeGuard([] { LogbookColumnStore::instance().setColumns({descriptionColumn()}); });

    const LogbookColumn roll = atExitColumn(QStringLiteral("Fusion"), QStringLiteral("roll"), QStringLiteral("angle"));
    const LogbookColumn north = atExitColumn(QStringLiteral("Local"), QStringLiteral("north"), QStringLiteral("distance"));
    const LogbookColumn bodyRoll = atExitColumn(QStringLiteral("Fusion"), kRoll, QStringLiteral("angle"));
    const QString at = QStringLiteral(" @ ");
    const QString rollLabel = logbookColumnLabel(roll);
    const QString northLabel = logbookColumnLabel(north);
    const QString bodyRollLabel = logbookColumnLabel(bodyRoll);
    QVERIFY2(rollLabel.startsWith(QStringLiteral("Fusion/roll") + at), qPrintable(rollLabel));
    QVERIFY2(northLabel.startsWith(QStringLiteral("Local/north") + at), qPrintable(northLabel));
    QVERIFY2(bodyRollLabel.startsWith(QStringLiteral("Roll") + at), qPrintable(bodyRollLabel));
    // The same marker name after " @ " in all three
    const QString markerPart = bodyRollLabel.mid(bodyRollLabel.indexOf(at));
    QVERIFY2(rollLabel.endsWith(markerPart), qPrintable(rollLabel));
    QVERIFY2(northLabel.endsWith(markerPart), qPrintable(northLabel));

    // One session, not shown, no plot checked
    QCOMPARE(addSessions({fixtureSession(QStringLiteral("coarse_linear"), QStringLiteral("s2"))}), QString());
    QVERIFY(m_plots->enabledPlots().isEmpty());

    // Column demand fits it
    LogbookColumnStore::instance().setColumns({descriptionColumn(), roll, north});
    QVERIFY(waitDemandIdle(*m_queue, *m_demand, kFitTimeoutMs));
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(nothingToShow());
    QCOMPARE(m_queue->model()->rowCount(), 1);
    QCOMPARE(fitJobOf("s2").state, JobState::Succeeded);

    const auto cell = [this](const LogbookColumn &column) {
        const QString id = CalculationDemand::columnId(column);
        for (int c = 0; c < m_model->columnCount(); ++c) {
            if (CalculationDemand::columnId(m_model->column(c)) == id)
                return std::as_const(*m_model).rowAt(m_model->getSessionRow("s2")).cachedValues.value(c);
        }
        return QVariant();
    };
    const QVariant rollCell = cell(roll);
    const QVariant northCell = cell(north);
    QCOMPARE(rollCell.typeId(), int(QMetaType::Double));
    QCOMPARE(northCell.typeId(), int(QMetaType::Double));
    const QVariant rollAtExit = session("s2").getAttribute(fusionRollAtExit());
    QVERIFY(rollAtExit.isValid());
    QVERIFY(sameBits(rollCell.toDouble(), rollAtExit.toDouble()));

    // The fit's stored record carries every fit channel the fit publishes and
    // no derived calculation produces, plotted (velD) or not, with the samples
    // the session reads
    const CalculationRecordRead read = LogbookManager::instance().readCalculationRecord(QStringLiteral("s2"), kFit);
    QCOMPARE(read.status, CalculationRecordStatus::Ok);
    QVERIFY(read.record.has_value());
    const CalculationResult &bundle = read.record->result.bundle;
    for (const char *name : {"north", "east", "down", "velN", "velE", "velD", "accN", "accE",
                             "roll", "pitch", "yaw", "qx", "qy", "qz", "qw"}) {
        const QString channel = QString::fromLatin1(name);
        QVERIFY2(bundle.isAvailable(fusionKey(channel)), name);
        QVERIFY2(sameBitsEverywhere(bundle.measurementValues(QStringLiteral("Fusion"), channel), fusion("s2", channel)),
                 name);
    }
    QCOMPARE(engine("s2").runCount(kFit), 1);
}

FLYSIGHT_TEST_MAIN(FusionRowsTest)
#include "tst_fusion_rows.moc"
