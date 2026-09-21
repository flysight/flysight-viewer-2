// The plot-row script with the REAL fusion plots: PlotModel + PlotRequests +
// JobQueue + SessionModel + Fusion::registerFusionCalculations, with real fits
// on the queue's 64 MiB worker. Sensor-fusion-jobs acceptance 15 end to end,
// and the real-plot halves of 9, 11 and 19. No widgets.
//
// DETERMINISM WITHOUT A GATE (as in tst_fusion_jobs). A real fit cannot be held
// by a semaphore. Progress posts reach the main thread in order and BEFORE the
// end of the same job is processed, so a slot that acts on the FIRST progress
// text of a job runs while the queue still considers that job Running, whatever
// the worker has done meanwhile. A slot on jobFinished that flushes the request
// component observes each publication before the next job starts (the queue
// starts the next job from a queued call). There are no sleeps.
//
// Expected values are literals and the committed goldens of the kernel.

#include <functional>
#include <memory>

#include <QSignalSpy>
#include <QtTest>

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
    /// A programmatic check: never a gesture.
    void check(const QString &measurement, bool enabled = true)
    {
        m_plots->setPlotEnabled(QStringLiteral("Fusion"), measurement, enabled);
    }
    void checkAllFusionPlots()
    {
        for (const PlotValue &plot : fusionPlots())
            check(plot.measurementID);
    }
    /// The current row state: a pending pass runs first.
    PlotRowState row(const QString &plotId)
    {
        m_requests->flush();
        return m_requests->rowState(plotId);
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
    /// Empty when Fusion/<every channel> of the session matches the golden.
    QString goldenDifference(const QString &id, const QString &goldenName);
    /// Empty when the session `absent` is in no list of any fusion row and no
    /// row counts more than one track; else the first offence.
    QString offenceInRows(const QString &absent);

    std::unique_ptr<SessionModel> m_model;
    std::unique_ptr<JobQueue> m_queue;
    std::unique_ptr<PlotModel> m_plots;
    std::unique_ptr<PlotRequests> m_requests;
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
    m_requests = std::make_unique<PlotRequests>(m_model.get(), m_plots.get(), m_queue.get());
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

    // Reverse order of construction
    m_requests.reset();
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
        const QString id = PlotRequests::plotId(plot);
        const PlotRowState state = row(id);
        const QStringList listed = sessionIdsOf(state.pending) + sessionIdsOf(state.missing)
            + sessionIdsOf(state.failed);
        if (listed.contains(absent))
            return id + QStringLiteral(" lists ") + absent;
        if (state.toolTip.contains(absent))
            return id + QStringLiteral(" names it in the tooltip");
        if (state.pendingCount > 1 || state.missingCount > 1 || state.failedCount > 1
            || state.waitingTotal > 1)
            return id + QStringLiteral(" counts more than one track");
    }
    return QString();
}

// Every real fusion plot is explicit-backed; the local-frame plots are
// ordinary. Checking any of them programmatically starts nothing.
void FusionRowsTest::allSeventeenFusionPlotsAreExplicitBacked()
{
    QCOMPARE(fusionPlots().size(), 17);
    QCOMPARE(addSessions({sessionFromFixture(fusionFixture(QStringLiteral("coarse_linear")), QStringLiteral("s2"))}),
             QString());
    show({"s2"});

    const Quiet quiet(*m_queue);
    checkAllFusionPlots();
    for (const PlotValue &plot : localFramePlots())
        m_plots->setPlotEnabled(plot.sensorID, plot.measurementID, true);

    for (const PlotValue &plot : fusionPlots()) {
        const QString id = PlotRequests::plotId(plot);
        const PlotRowState state = row(id);
        QVERIFY2(state.explicitBacked, qPrintable(id));
        QCOMPARE(state.plotId, id);
        QCOMPARE(state.missingCount, 1);
        QCOMPARE(state.pendingCount, 0);
        QCOMPARE(state.failedCount, 0);
        QCOMPARE(state.control(), Control::Refresh);
        QCOMPARE(state.missing.at(0).calculationTitles, QStringList({kTitle}));
    }
    for (const PlotValue &plot : localFramePlots()) {
        const QString id = PlotRequests::plotId(plot);
        QVERIFY2(row(id) == PlotRowState(), qPrintable(id));
    }
    // The ordinary plots still read normally, next to the unrequested fit
    QVERIFY(!session("s2").getMeasurement(QStringLiteral("Local"), QStringLiteral("north")).isEmpty());

    QVERIFY(quiet.holds());
    QVERIFY(m_queue->isIdle());
    QCOMPARE(engine("s2").runCount(kFit), 0);
}

// Acceptance 15, on the real names with real fits.
void FusionRowsTest::realRowScript()
{
    QCOMPARE(addSessions({sessionFromFixture(fusionFixture(QStringLiteral("coarse_maneuver")), QStringLiteral("s1")),
                          sessionFromFixture(fusionFixture(QStringLiteral("coarse_linear")), QStringLiteral("s2")),
                          sessionFromFixture(fusionFixture(QStringLiteral("coarse_linear")), QStringLiteral("s3")),
                          sessionFromFixture(fusionFixture(QStringLiteral("coarse_linear")), QStringLiteral("s4"))}),
             QString());

    // 1. Three visible fusable tracks, the plot checked programmatically:
    //    nothing starts by itself
    show({"s1", "s2", "s3"});
    {
        const Quiet quiet(*m_queue);
        check(QStringLiteral("roll"));
        const PlotRowState state = row(kRoll);
        QVERIFY(state.explicitBacked);
        QCOMPARE(state.missingCount, 3);
        QCOMPARE(state.control(), Control::Refresh);
        QCOMPARE(state.controlCount(), 3);
        QVERIFY(quiet.holds());
    }

    // Records, on every job end, what the row shows at that moment
    struct Seen { JobId job; int pendingCount; QString progressLabel; };
    QList<Seen> seen;
    QObject seenScope;      // owns the connection: it cannot outlive `seen`
    connect(m_queue.get(), &JobQueue::jobFinished, &seenScope, [this, &seen](JobId id, JobState) {
        m_requests->flush();
        const PlotRowState state = m_requests->rowState(kRoll);
        seen.append({id, state.pendingCount, state.progressLabel});
    });

    // 2. The gesture queues three jobs
    QCOMPARE(m_requests->plotCheckedByUser(kRoll), 3);
    QCOMPARE(m_queue->model()->rowCount(), 3);
    const JobId job1 = fitJobOf("s1").id;
    const JobId job2 = fitJobOf("s2").id;
    const JobId job3 = fitJobOf("s3").id;
    QVERIFY(job1 != 0 && job2 != 0 && job3 != 0);
    for (JobId id : {job1, job2, job3}) {
        const JobRecord record = m_queue->job(id);
        QVERIFY(record.isActive());
        QCOMPARE(record.calculationId, kFit);
        QCOMPARE(record.calculationTitle, kTitle);
    }
    PlotRowState state = m_requests->rowState(kRoll);
    QCOMPARE(state.pendingCount, 3);
    QCOMPARE(state.progressLabel, QStringLiteral("0 of 3"));
    QCOMPARE(state.control(), Control::Cancel);

    // 4 (armed now, acts later). On the first progress text of s2's job the
    //    plot is unchecked: s3's queued job goes, s2's running job stays.
    bool uncheckedWhileRunning = false;
    JobState job3AfterUncheck = JobState::Queued;
    QString job3Reason;
    bool job2CancelRequested = true;
    QObject job2Scope;      // owns the connection: it cannot outlive what the slot captures
    onFirstProgress(*m_queue, &job2Scope, job2, [&] {
        uncheckedWhileRunning = m_queue->job(job2).state == JobState::Running;
        check(QStringLiteral("roll"), false);
        job3AfterUncheck = m_queue->job(job3).state;
        job3Reason = m_queue->job(job3).reason;
        job2CancelRequested = m_queue->job(job2).cancelRequested;
        uncheckedWhileRunning = uncheckedWhileRunning && m_queue->job(job2).state == JobState::Running;
    });

    QVERIFY(waitIdle(*m_queue, kFitTimeoutMs));

    // 3. The count fell when s1's job published, before the next job started
    QVERIFY(!seen.isEmpty());
    QCOMPARE(seen.at(0).job, job1);
    QCOMPARE(seen.at(0).pendingCount, 2);
    QCOMPARE(seen.at(0).progressLabel, QStringLiteral("1 of 3"));
    QCOMPARE(m_queue->job(job1).state, JobState::Succeeded);
    QString difference = goldenDifference("s1", QStringLiteral("coarse_maneuver"));
    QVERIFY2(difference.isEmpty(), qPrintable(difference));

    // 4. Unchecking mid-way removed the queued job and let the running one finish
    QVERIFY(uncheckedWhileRunning);
    QCOMPARE(job3AfterUncheck, JobState::Cancelled);
    QCOMPARE(job3Reason, QStringLiteral("No longer needed"));
    QVERIFY(!job2CancelRequested);
    QCOMPARE(m_queue->job(job2).state, JobState::Succeeded);
    difference = goldenDifference("s2", QStringLiteral("coarse_linear"));
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
    QVERIFY(fusion("s3", QStringLiteral("roll")).isEmpty());
    QCOMPARE(engine("s3").runCount(kFit), 0);
    QVERIFY(row(kRoll) == PlotRowState());

    // 5. Checked again by the user: only s3 is missing. Cancel leaves the plot
    //    checked and the track missing, and publishes nothing.
    QSignalSpy dependencySpy(m_model.get(), &SessionModel::dependencyChanged);
    check(QStringLiteral("roll"));
    QCOMPARE(m_requests->plotCheckedByUser(kRoll), 1);
    const JobId job4 = fitJobOf("s3").id;
    QVERIFY(job4 != job3);
    int cancelled = -1;
    bool checkedAfterCancel = false;
    PlotRowState afterCancel;
    QObject job4Scope;      // owns the connection: it cannot outlive what the slot captures
    onFirstProgress(*m_queue, &job4Scope, job4, [&] {
        cancelled = m_requests->cancelPressed(kRoll);
        checkedAfterCancel = m_plots->isPlotEnabled(QStringLiteral("Fusion"), QStringLiteral("roll"));
        afterCancel = m_requests->rowState(kRoll);      // at once
    });
    QVERIFY(waitIdle(*m_queue, kFitTimeoutMs));
    QCOMPARE(cancelled, 1);
    QVERIFY(checkedAfterCancel);
    QCOMPARE(afterCancel.pendingCount, 0);
    QCOMPARE(afterCancel.missingCount, 1);
    QCOMPARE(afterCancel.control(), Control::Refresh);
    QCOMPARE(m_queue->job(job4).state, JobState::Cancelled);
    QVERIFY(fusion("s3", QStringLiteral("roll")).isEmpty());
    QCOMPARE(engine("s3").runCount(kFit), 0);
    for (const QString &name : fusionMeasurementNames())
        QVERIFY2(!spyHasMeasurement(dependencySpy, "s3", QStringLiteral("Fusion"), name), qPrintable(name));
    QVERIFY(!spyHasAttribute(dependencySpy, "s3", QStringLiteral("_FUSION_DIAGNOSTICS")));
    QVERIFY(m_plots->isPlotEnabled(QStringLiteral("Fusion"), QStringLiteral("roll")));
    QCOMPARE(row(kRoll).missingCount, 1);

    // 6. Refresh computes it
    QCOMPARE(m_requests->refreshPressed(kRoll), 1);
    QVERIFY(waitIdle(*m_queue, kFitTimeoutMs));
    QVERIFY(row(kRoll).isPlain());
    difference = goldenDifference("s3", QStringLiteral("coarse_linear"));
    QVERIFY2(difference.isEmpty(), qPrintable(difference));

    // 7. A fourth track shown afterwards is missing, with a count of one, and
    //    starts nothing
    {
        const Quiet quiet(*m_queue);
        show({"s4"});
        state = row(kRoll);
        QCOMPARE(state.missingCount, 1);
        QCOMPARE(state.control(), Control::Refresh);
        QCOMPARE(state.missing.at(0).sessionId, QStringLiteral("s4"));
        QCOMPARE(state.missing.at(0).calculationTitles, QStringList({kTitle}));
        PlotFixture::spin(m_requests.get());
        QVERIFY(quiet.holds());
        QVERIFY(m_queue->isIdle());
        QCOMPARE(row(kRoll).missingCount, 1);
        QCOMPARE(engine("s4").runCount(kFit), 0);
    }

    // 8. Pressing refresh computes it
    QCOMPARE(m_requests->refreshPressed(kRoll), 1);
    QVERIFY(waitIdle(*m_queue, kFitTimeoutMs));
    QVERIFY(row(kRoll).isPlain());
    difference = goldenDifference("s4", QStringLiteral("coarse_linear"));
    QVERIFY2(difference.isEmpty(), qPrintable(difference));

    // The whole history, from the job model alone
    QList<JobState> history;
    for (int r = 0; r < m_queue->model()->rowCount(); ++r)
        history.append(m_queue->model()->record(r).state);
    QCOMPARE(history, QList<JobState>({JobState::Succeeded, JobState::Succeeded, JobState::Cancelled,
                                       JobState::Cancelled, JobState::Succeeded, JobState::Succeeded}));
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
    QCOMPARE(row(kRoll).missingCount, 1);

    QCOMPARE(m_requests->plotCheckedByUser(QStringLiteral("Fusion/pitch")), 1);
    QCOMPARE(m_requests->plotCheckedByUser(QStringLiteral("Fusion/yaw")), 0);     // the job exists
    QCOMPARE(m_queue->model()->rowCount(), 1);
    const JobId job = fitJobOf("s2").id;
    QVERIFY(job != 0);

    QList<PlotRowState> during;
    QObject scope;      // owns the connection: it cannot outlive what the slot captures
    onFirstProgress(*m_queue, &scope, job, [&] {
        m_requests->flush();
        for (const QString &id : rows)
            during.append(m_requests->rowState(id));
    });
    QVERIFY(waitIdle(*m_queue, kFitTimeoutMs));

    QCOMPARE(during.size(), 3);
    for (const PlotRowState &state : std::as_const(during)) {
        QCOMPARE(state.pendingCount, 1);
        QCOMPARE(state.control(), Control::Cancel);
        QCOMPARE(state.pending.at(0).job, job);
        QVERIFY(!state.jobProgressText.isEmpty());
        QCOMPARE(state.jobProgressText, during.at(0).jobProgressText);
    }

    QCOMPARE(m_queue->job(job).state, JobState::Succeeded);
    for (const QString &id : rows)
        QVERIFY2(row(id).isPlain(), qPrintable(id));
    QCOMPARE(m_queue->model()->rowCount(), 1);
    QCOMPARE(engine("s2").runCount(kFit), 1);
}

// Fusion/accH is on demand; the row sees through it and asks for the fit.
void FusionRowsTest::accHRowIsBlockedByFusion()
{
    QCOMPARE(addSessions({sessionFromFixture(fusionFixture(QStringLiteral("coarse_linear")), QStringLiteral("s2"))}),
             QString());
    show({"s2"});
    const QString accH = QStringLiteral("Fusion/accH");
    check(QStringLiteral("accH"));

    PlotRowState state = row(accH);
    QVERIFY(state.explicitBacked);
    QCOMPARE(state.missingCount, 1);
    QCOMPARE(state.missing.at(0).calculationTitles, QStringList({kTitle}));

    QCOMPARE(m_requests->plotCheckedByUser(accH), 1);
    QCOMPARE(m_queue->model()->rowCount(), 1);
    QCOMPARE(m_queue->model()->record(0).calculationId, kFit);
    // An on-demand calculation can never be a job
    QVERIFY(!m_queue->request("s2", kAccH).created());
    QVERIFY(waitIdle(*m_queue, kFitTimeoutMs));
    QCOMPARE(m_queue->model()->rowCount(), 1);
    QCOMPARE(m_queue->model()->record(0).state, JobState::Succeeded);

    QVERIFY(row(accH).isPlain());
    const QVector<double> values = fusion("s2", QStringLiteral("accH"));
    QCOMPARE(values.size(), 160);           // coarse_linear: 160 output samples
    QCOMPARE(values.size(), fusion("s2", QStringLiteral("_time")).size());
    QCOMPARE(engine("s2").runCount(kAccH), 1);
    QCOMPARE(engine("s2").runCount(kFit), 1);
}

// Acceptance 11 on all seventeen real rows: a session without IMU data is
// never missing, pending or failed, and cannot have a job.
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
    QCOMPARE(row(kRoll).missingCount, 1);
    QCOMPARE(row(kRoll).missing.at(0).sessionId, QStringLiteral("s2"));

    QCOMPARE(m_requests->plotCheckedByUser(kRoll), 1);
    QCOMPARE(m_queue->model()->rowCount(), 1);
    const JobId job = fitJobOf("s2").id;
    QVERIFY(job != 0);
    QCOMPARE(fitJobOf("n1").id, JobId(0));

    // During
    QString offenceDuring = QStringLiteral("the job reported no progress");
    int pendingDuring = -1;
    QObject scope;      // owns the connection: it cannot outlive what the slot captures
    onFirstProgress(*m_queue, &scope, job, [&] {
        offenceDuring = offenceInRows(QStringLiteral("n1"));
        pendingDuring = m_requests->rowState(QStringLiteral("Fusion/qw")).pendingCount;
    });
    QVERIFY(waitIdle(*m_queue, kFitTimeoutMs));
    QVERIFY2(offenceDuring.isEmpty(), qPrintable(offenceDuring));
    QCOMPARE(pendingDuring, 1);             // every row waits on the one job of s2

    // After
    offence = offenceInRows(QStringLiteral("n1"));
    QVERIFY2(offence.isEmpty(), qPrintable(offence));
    for (const PlotValue &plot : fusionPlots())
        QVERIFY2(row(PlotRequests::plotId(plot)).isPlain(), qPrintable(plot.measurementID));

    QCOMPARE(m_queue->request("n1", kFit).kind, Kind::MissingInput);
    QCOMPARE(m_queue->model()->rowCount(), 1);
    QCOMPARE(engine("n1").runCount(kFit), 0);
}

// Acceptance 9 seen from the row: a recording the model rejects shows the
// warning badge with the reason, offers no retry, and becomes refreshable when
// its inputs change.
void FusionRowsTest::rejectedTrackShowsBadge()
{
    QCOMPARE(addSessions({sessionFromFixture(fusionFixture(QStringLiteral("reject_origin")), QStringLiteral("r1"))}),
             QString());
    show({"r1"});
    check(QStringLiteral("roll"));
    QCOMPARE(m_requests->plotCheckedByUser(kRoll), 1);
    QVERIFY(waitIdle(*m_queue, kFitTimeoutMs));
    QCOMPARE(fitJobOf("r1").state, JobState::Succeeded);        // a rejection is a result

    PlotRowState state = row(kRoll);
    QCOMPARE(state.failedCount, 1);
    QVERIFY(state.showsWarning());
    QCOMPARE(state.control(), Control::None);
    QCOMPARE(state.failed.at(0).sessionId, QStringLiteral("r1"));
    QCOMPARE(state.failed.at(0).reason,
             QStringLiteral("Sensor fusion: Local origin index outside GNSS samples"));
    QCOMPARE(state.toolTip, QStringLiteral("Could not be computed:\n"
                                           "  r1 - Sensor fusion: Local origin index outside GNSS samples"));

    // No retry: the same inputs give the same answer
    {
        const Quiet quiet(*m_queue);
        QCOMPARE(m_requests->refreshPressed(kRoll), 0);
        QCOMPARE(m_requests->plotCheckedByUser(kRoll), 0);
        QVERIFY(quiet.holds());
    }

    // reject_origin is coarse_linear with origin index 9: a valid index makes
    // the track missing, and therefore refreshable
    QVERIFY(m_model->updateAttribute("r1", "_LOCAL_ORIGIN_INDEX", QVariant::fromValue(qlonglong(0))));
    state = row(kRoll);
    QCOMPARE(state.failedCount, 0);
    QCOMPARE(state.missingCount, 1);
    QCOMPARE(state.control(), Control::Refresh);
    QCOMPARE(m_requests->refreshPressed(kRoll), 1);
    QVERIFY(waitIdle(*m_queue, kFitTimeoutMs));
    QVERIFY(row(kRoll).isPlain());
    const QString difference = goldenDifference("r1", QStringLiteral("coarse_linear"));
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
}

// Acceptance 19, the half that needs no widget: while a real fit runs, other
// sessions are edited, tracks are hidden and shown, and other values are read,
// all from the main thread; the fit is not disturbed.
void FusionRowsTest::editsAndVisibilityDuringFit()
{
    QCOMPARE(addSessions({sessionFromFixture(fusionFixture(QStringLiteral("coarse_maneuver")), QStringLiteral("s1")),
                          sessionFromFixture(fusionFixture(QStringLiteral("coarse_linear")), QStringLiteral("s2")),
                          sessionFromFixture(fusionFixture(QStringLiteral("coarse_linear")), QStringLiteral("s3"))}),
             QString());
    show({"s1"});
    check(QStringLiteral("roll"));
    QCOMPARE(m_requests->plotCheckedByUser(kRoll), 1);
    const JobId job = fitJobOf("s1").id;
    QVERIFY(job != 0);

    // Showing more tracks starts nothing for them
    {
        const Quiet quiet(*m_queue);
        show({"s2", "s3"});
        QCOMPARE(row(kRoll).missingCount, 2);
        QVERIFY(quiet.holds());
    }

    QSignalSpy dependencySpy(m_model.get(), &SessionModel::dependencyChanged);
    bool running = false, edited = false, hidden = false, shownAgain = false;
    qsizetype northSamples = 0;
    int missingWhileHidden = -1;
    QObject scope;      // owns the connection: it cannot outlive what the slot captures
    onFirstProgress(*m_queue, &scope, job, [&] {
        running = m_queue->job(job).state == JobState::Running;
        edited = m_model->updateAttribute("s2", QString::fromLatin1(SessionKeys::Description),
                                          QStringLiteral("edited"));
        show({"s3"}, false);
        hidden = !session("s3").isVisible();
        missingWhileHidden = row(kRoll).missingCount;
        show({"s3"});
        shownAgain = session("s3").isVisible();
        northSamples = session("s2").getMeasurement(QStringLiteral("Local"), QStringLiteral("north")).size();
    });
    QVERIFY(waitIdle(*m_queue, kFitTimeoutMs));

    QVERIFY(running);
    QVERIFY(edited);
    QVERIFY(hidden);
    QCOMPARE(missingWhileHidden, 1);        // s2 only, while s3 was hidden
    QVERIFY(shownAgain);
    QCOMPARE(northSamples, qsizetype(9));   // coarse_linear: nine GNSS fixes
    m_model->flushPendingInvalidations();
    QVERIFY(spyHasAttribute(dependencySpy, "s2", QString::fromLatin1(SessionKeys::Description)));

    // Unrelated edits do not supersede: the fit of s1 ends well and is right
    QCOMPARE(m_queue->job(job).state, JobState::Succeeded);
    QVERIFY2(m_queue->job(job).reason.isEmpty(), qPrintable(m_queue->job(job).reason));
    const QString difference = goldenDifference("s1", QStringLiteral("coarse_maneuver"));
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
    QCOMPARE(m_queue->model()->rowCount(), 1);      // nothing else ever started
    QCOMPARE(row(kRoll).missingCount, 2);

    // The edit was saved by the ordinary idle saver
    QVERIFY(waitForIdle(*m_model));
    QCOMPARE(session("s2").getAttribute(SessionKeys::Description).toString(), QStringLiteral("edited"));
    QVERIFY(!std::as_const(*m_model).rowAt(m_model->getSessionRow("s2")).dirty);
    QCOMPARE(indexValue(QStringLiteral("s2"), descriptionColumn()).toString(), QStringLiteral("edited"));
}

FLYSIGHT_TEST_MAIN(FusionRowsTest)
#include "tst_fusion_rows.moc"
