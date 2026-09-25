// The executor on a real SessionModel, real SessionData engines, and the
// global registry, with the synthetic explicit calculations of jobfixture.h.
// Sensor-fusion-jobs acceptance 8 (executor half), 10, 11 (executor half), 14,
// 17. The executor holds at most the running job and one chosen next job, so
// a second job exists here only as the chosen next job, offered after the
// first has started (Gate::waitEntered() or waitStarted()); a second offer in
// the same event-loop turn replaces the first. There is no demand layer here:
// the tests drive the executor directly.
//
// Synchronization: Gate::waitEntered() proves the worker is inside a compute
// function; QTRY_* and waitIdle() spin the event loop for main-thread effects.
// There are no sleeps.

#include <atomic>
#include <memory>

#include <QHash>
#include <QSignalSpy>
#include <QThread>
#include <QTimer>
#include <QtTest>

#include "builtinfixture.h"
#include "engine/calculationengine.h"
#include "engine/calculationregistry.h"
#include "jobfixture.h"
#include "jobmodel.h"
#include "jobqueue.h"
#include "logbookcolumn.h"
#include "logbookprobe.h"
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

const char kRemoved[] = "Session removed or unloaded";
const char kReplaced[] = "Session data replaced";

// Written by the probe's compute function on the worker, read after the job ended
std::atomic<quintptr> g_probeThread{0};
std::atomic<uint> g_probeStackSize{0};
std::atomic<int> g_probePriority{-1};       // a QThread::Priority

} // namespace

class JobQueueTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void runsAndPublishes();
    void publishesInvalidationsThroughSessionModel();
    void workerIsNotMainThreadAndHasLargeStack();
    void workerRunsBelowNormalPriority();
    void mainThreadIsNotBlockedByARunningJob();
    void duplicateOffersCreateNoDuplicates();
    void oneAtATimeInOfferOrder();
    void holdsAtMostRunningAndChosenNext();
    void offerReplacesChosenNext();
    void withdrawEndsChosenNext();
    void refusesMissingInput();
    void refusesUnloadedAndUnknownSession();
    void refusesBlockedAndDone();
    void neverLoadsASession();
    void inputChangeWhileRunningSupersedes();
    void inputChangeWhileQueuedSupersedesAtStart();
    void registrationRemovedSupersedes();
    void staleRunningJobIsStoppedAtOnce();
    void offerWhileStaleJobWindsDown();
    void staleJobThatReturnsAResultIsStillSuperseded();
    void registrationRemovedStopsRunningJobAtOnce();
    void modelDestroyedStopsRunningJobAtOnce();
    void userCancelThenStaleEndsCancelled();
    void staleThenUserCancelEndsSuperseded();
    void rejectionSucceedsWithReason();
    void exceptionSucceedsAsFailedResult();
    void resourceExhaustionFails();
    void workerStartFailureFails();
    void cancelRunningThenNextStarts();
    void cancelIgnoredForOneStepStillCancelled();
    void offerWhileCancellingCreatesNewJob();
    void cancelQueued();
    void cancelFromRowsInsertedLeavesNoPin();
    void removeSessionWithQueuedJob();
    void removeSessionWithRunningJob();
    void evictionDeferredWhileJobActive();
    void repopulateWithJobs();
    void mergeIntoSessionWithRunningJobSupersedes();
    void sessionDataReplacedWithRunningJobSupersedes();
    void sortWhileRunningStillPublishes();
    void shutdownWithQueuedAndRunning();
    void shutdownIsIdempotentAndRefusesOffers();
    void shutdownFromSlots();
    void queueDestroyedBeforeModel();
    void modelDestroyedBeforeQueue();
    void idleSchedulerKeepsWorking();

private:
    SessionData &session(const QString &id) { return m_model->sessionRef(m_model->getSessionRow(id)); }
    CalculationEngine &engine(const QString &id) { return session(id).calculationEngine(); }
    bool isLoaded(const QString &id) const
    {
        const int row = m_model->getSessionRow(id);
        return row >= 0 && std::as_const(*m_model).rowAt(row).isLoaded();
    }
    /// The application's edit path. False when the model refused; a test
    /// function checks it with QVERIFY, so that a failure ends the function.
    [[nodiscard]] bool setInput(const QString &sessionId, const char *key, int value)
    {
        return m_model->updateAttribute(sessionId, QString::fromLatin1(key), value);
    }
    JobState stateOf(JobId id) const { return m_queue->job(id).state; }
    Gate &gate() { return m_world->gate(); }

    /// Empty when nothing of `calculationId` was published in `sessionId`:
    /// the output is unavailable, no dependencyChanged named it, and the
    /// calculation has no result. Otherwise what was found.
    QString publishedTrace(const QSignalSpy &dependencySpy, const QString &sessionId,
                           const QString &calculationId, const char *outputKey)
    {
        const QString key = QString::fromLatin1(outputKey);
        if (session(sessionId).getAttribute(key).isValid())
            return key + QStringLiteral(" is available");
        if (spyHasAttribute(dependencySpy, sessionId, key))
            return QStringLiteral("dependencyChanged was emitted for ") + key;
        const std::optional<ResultStatus> status = engine(sessionId).resultStatus(calculationId);
        if (status.has_value() && *status != ResultStatus::NotRequested)
            return calculationId + QStringLiteral(" has a cached result");
        if (engine(sessionId).preparedCount() != 0)
            return QStringLiteral("a ticket is still outstanding");
        return QString();
    }

    void watchEndTransitions(JobModel *model);
    /// Empty when each job of the model has had exactly one end transition if
    /// it is finished and none otherwise; else the jobs for which that is not so.
    QString endTransitionErrors(JobModel *model) const;

    std::unique_ptr<JobWorld> m_world;
    std::unique_ptr<SessionModel> m_model;
    std::unique_ptr<JobQueue> m_queue;
    QStringList m_registryBefore;
    QHash<JobId, int> m_endTransitions;
};

void JobQueueTest::initTestCase()
{
    TestEnvironment::instance().registerBuiltIns();

    // One logbook column that reads stored data only, so that the model has
    // valid indexes to report without warming any calculation.
    PreferencesManager::instance().registerPreference(PreferenceKeys::LogbookColumnsVersion, 0);
    LogbookColumnStore::instance().setColumns({descriptionColumn()});

    qRegisterMetaType<DependencyKey>();
}

// Three loaded, hidden, unfocused sessions "s1", "s2", "s3" from the descent
// fixture. None has an input of any synthetic calculation: a test adds them.
void JobQueueTest::init()
{
    TestEnvironment &env = TestEnvironment::instance();
    env.useFreshLogbook();
    env.resetPreferencesToDefaults();
    m_registryBefore = CalculationRegistry::instance().registeredIds();

    m_world = std::make_unique<JobWorld>();
    m_model = std::make_unique<SessionModel>();
    m_model->mergeSessions(JobWorld::sessions({"s1", "s2", "s3"}));
    QCOMPARE(m_model->rowCount(), 3);
    m_model->flushPendingInvalidations();

    m_queue = std::make_unique<JobQueue>(m_model.get());
    m_endTransitions.clear();
    watchEndTransitions(m_queue->model());
}

// Note what is to be checked, tear everything down, and only then check: a
// failing check returns from cleanup(), and whatever were still alive then
// would be alive under the next init() (a second JobWorld registering the same
// ids on top of the first, and every later test failing for that reason).
void JobQueueTest::cleanup()
{
    QString endTransitions;
    QStringList stillPinned;
    if (m_queue) {
        // Let nothing linger inside a compute function
        m_queue->shutdown();
        endTransitions = endTransitionErrors(m_queue->model());
    }
    if (m_model) {
        for (const char *id : {"s1", "s2", "s3"}) {
            if (m_model->isSessionPinned(id))
                stillPinned.append(QString::fromLatin1(id));
        }
    }

    // Queue, then model, then the registrations (a live model reacts to
    // registry changes)
    m_queue.reset();
    m_model.reset();
    m_world.reset();

    QCOMPARE(endTransitions, QString());
    QCOMPARE(stillPinned, QStringList());
    QCOMPARE(CalculationRegistry::instance().registeredIds(), m_registryBefore);
    QCOMPARE(CalculationRegistry::instance().enrolledEngineCount(), 0);
}

// Each job has exactly one end transition in the model.
void JobQueueTest::watchEndTransitions(JobModel *model)
{
    connect(model, &QAbstractItemModel::dataChanged, this,
            [this, model](const QModelIndex &topLeft, const QModelIndex &, const QList<int> &roles) {
        if (roles.contains(JobModel::IsFinishedRole))
            ++m_endTransitions[topLeft.data(JobModel::JobIdRole).toULongLong()];
    });
}

QString JobQueueTest::endTransitionErrors(JobModel *model) const
{
    QStringList errors;
    for (int row = 0; row < model->rowCount(); ++row) {
        const JobRecord job = model->record(row);
        const int expected = job.isFinished() ? 1 : 0;
        if (m_endTransitions.value(job.id) != expected) {
            errors.append(QStringLiteral("job %1: %2 end transitions, expected %3")
                              .arg(job.id).arg(m_endTransitions.value(job.id)).arg(expected));
        }
    }
    return errors.join(QStringLiteral("; "));
}

// ---- Running and publishing ---------------------------------------------------------

void JobQueueTest::runsAndPublishes()
{
    QVERIFY(setInput("s1", "EA_IN", 4));
    QVERIFY(m_model->updateAttribute("s1", "_DESCRIPTION", QStringLiteral("First jump")));
    m_model->removeAttribute("s2", "_DESCRIPTION");
    QVERIFY(setInput("s2", "EA_IN", 4));

    QSignalSpy queuedSpy(m_queue.get(), &JobQueue::jobQueued);
    QSignalSpy startedSpy(m_queue.get(), &JobQueue::jobStarted);
    QSignalSpy finishedSpy(m_queue.get(), &JobQueue::jobFinished);
    QSignalSpy idleSpy(m_queue.get(), &JobQueue::idle);

    const JobQueue::OfferResult result = m_queue->offer("s1", QStringLiteral("expA"));
    QCOMPARE(result.kind, Kind::Created);
    QVERIFY(result.created());
    QCOMPARE(result.job, JobId(1));

    // Nothing starts synchronously
    JobRecord job = m_queue->job(result.job);
    QCOMPARE(job.state, JobState::Queued);
    QCOMPARE(job.sessionId, QStringLiteral("s1"));
    QCOMPARE(job.sessionName, QStringLiteral("First jump"));
    QCOMPARE(job.calculationId, QStringLiteral("expA"));
    QCOMPARE(job.instanceId, QStringLiteral("expA"));
    QCOMPARE(job.calculationTitle, QStringLiteral("Explicit A"));
    QVERIFY(job.queuedAt.isValid());
    QVERIFY(!job.startedAt.isValid());
    QVERIFY(!m_queue->isIdle());
    QCOMPARE(m_queue->runningJob(), JobId(0));
    QCOMPARE(m_queue->chosenNextJob(), JobId(1));
    QCOMPARE(m_queue->activeJobs(), QList<JobId>({1}));
    QCOMPARE(m_queue->activeJob("s1", "expA"), JobId(1));
    QVERIFY(m_model->isSessionPinned("s1"));
    QCOMPARE(queuedSpy.count(), 1);
    QCOMPARE(startedSpy.count(), 0);

    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(m_queue->chosenNextJob(), JobId(0));
    job = m_queue->job(result.job);
    QCOMPARE(job.state, JobState::Succeeded);
    QVERIFY(job.reason.isEmpty());
    QCOMPARE(job.resultStatus, std::optional<ResultStatus>(ResultStatus::Ok));
    QVERIFY(job.isFinished());
    QCOMPARE(startedSpy.count(), 1);
    QCOMPARE(finishedSpy.count(), 1);
    QCOMPARE(finishedSpy.at(0).at(0).toULongLong(), 1ULL);
    QCOMPARE(finishedSpy.at(0).at(1).value<JobState>(), JobState::Succeeded);
    QCOMPARE(idleSpy.count(), 1);
    QVERIFY(!m_model->isSessionPinned("s1"));
    QCOMPARE(m_queue->activeJob("s1", "expA"), JobId(0));

    // All outputs together, once
    QCOMPARE(session("s1").getAttribute("EA1"), QVariant(5));
    QCOMPARE(session("s1").getAttribute("EA2"), QVariant(8));
    QCOMPARE(session("s1").getAttribute("EA_DIAG"), QVariant(QStringLiteral("ok")));
    QCOMPARE(engine("s1").runCount("expA"), 1);
    QCOMPARE(engine("s1").preparedCount(), 0);

    // The name of a session without a description is its id
    const JobQueue::OfferResult second = m_queue->offer("s2", QStringLiteral("expA"));
    QCOMPARE(second.kind, Kind::Created);
    QCOMPARE(second.job, JobId(2));
    QCOMPARE(m_queue->job(second.job).sessionName, QStringLiteral("s2"));
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(second.job), JobState::Succeeded);
    QCOMPARE(idleSpy.count(), 2);
}

// Names read while the calculation was unrequested are re-announced through
// the session model, after the model says the job finished and before
// jobFinished. publishingJob() names the job for exactly that step.
void JobQueueTest::publishesInvalidationsThroughSessionModel()
{
    QVERIFY(setInput("s1", "EA_IN", 4));
    QVERIFY(!session("s1").getAttribute("DA").isValid());       // read while unrequested
    QVERIFY(!session("s1").getAttribute("EA1").isValid());

    QSignalSpy dependencySpy(m_model.get(), &SessionModel::dependencyChanged);
    QSignalSpy modelSpy(m_model.get(), &SessionModel::modelChanged);
    QSignalSpy finishedSpy(m_queue.get(), &JobQueue::jobFinished);

    QList<JobState> stateWhenAnnounced;
    QList<int> finishedSignalsWhenAnnounced;
    QList<JobId> publishingWhenAnnounced;
    QObject scope;      // owns the connection: it ends with this function, as what the slot captures does
    connect(m_model.get(), &SessionModel::dependencyChanged, &scope,
            [&](const QString &, const DependencyKey &) {
        stateWhenAnnounced.append(m_queue->job(1).state);
        finishedSignalsWhenAnnounced.append(int(finishedSpy.count()));
        publishingWhenAnnounced.append(m_queue->publishingJob());
    });

    QCOMPARE(m_queue->publishingJob(), JobId(0));
    QCOMPARE(m_queue->offer("s1", QStringLiteral("expA")).kind, Kind::Created);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(1), JobState::Succeeded);
    QCOMPARE(m_queue->publishingJob(), JobId(0));       // only during the publication
    QVERIFY(!publishingWhenAnnounced.isEmpty());
    for (const JobId publishing : std::as_const(publishingWhenAnnounced))
        QCOMPARE(publishing, JobId(1));

    QVERIFY(spyHasAttribute(dependencySpy, "s1", "DA"));
    QVERIFY(spyHasAttribute(dependencySpy, "s1", "EA1"));
    QVERIFY(!spyHasAttribute(dependencySpy, "s2", "DA"));
    QVERIFY(modelSpy.count() >= 1);
    QVERIFY(!stateWhenAnnounced.isEmpty());
    for (const JobState state : std::as_const(stateWhenAnnounced))
        QCOMPARE(state, JobState::Succeeded);
    for (const int count : std::as_const(finishedSignalsWhenAnnounced))
        QCOMPARE(count, 0);

    QCOMPARE(session("s1").getAttribute("DA"), QVariant(105));
    QCOMPARE(session("s1").getAttribute("EA1"), QVariant(5));
}

void JobQueueTest::workerIsNotMainThreadAndHasLargeStack()
{
    CalculationDescriptor probe;
    probe.id = QStringLiteral("threadprobe");
    probe.title = QStringLiteral("Thread probe");
    probe.policy = EvaluationPolicy::Explicit;
    probe.inputs = {CalcInput::attribute(QStringLiteral("P_IN"))};
    probe.outputs = {DependencyKey::attribute(QStringLiteral("P_OUT"))};
    probe.compute = [](const EvaluationContext &ctx) {
        g_probeThread.store(quintptr(QThread::currentThread()));
        g_probeStackSize.store(QThread::currentThread()->stackSize());
        return CalculationResult().setAttribute(QStringLiteral("P_OUT"),
                                                ctx.attribute(QStringLiteral("P_IN")).toInt() + 1);
    };
    CalculationRegistry &registry = CalculationRegistry::instance();
    QVERIFY(registry.registerCalculation(probe));
    const auto unregister = qScopeGuard([&registry] {
        registry.unregister(QStringLiteral("threadprobe"), CalculationRegistry::Removal::Change);
    });
    m_model->flushPendingInvalidations();

    g_probeThread.store(0);
    g_probeStackSize.store(0);
    QVERIFY(setInput("s1", "P_IN", 4));
    QCOMPARE(m_queue->offer("s1", QStringLiteral("threadprobe")).kind, Kind::Created);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(1), JobState::Succeeded);
    QCOMPARE(session("s1").getAttribute("P_OUT"), QVariant(5));

    QVERIFY(g_probeThread.load() != 0);
    QVERIFY(g_probeThread.load() != quintptr(QCoreApplication::instance()->thread()));
    QCOMPARE(g_probeStackSize.load(), uint(64 * 1024 * 1024));
    QCOMPARE(JobQueue::kWorkerStackSize, qsizetype(67108864));

    // 16 MiB of locals would overflow a default thread stack
    QVERIFY(setInput("s1", "D_IN", 4));
    QCOMPARE(m_queue->offer("s1", QStringLiteral("deepstack")).kind, Kind::Created);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(2), JobState::Succeeded);
    QCOMPARE(m_queue->job(2).resultStatus, std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(session("s1").getAttribute("D_OUT"), QVariant(5));
}

// Below normal: the user interface stays responsive and the machine usable
// while a long calculation runs. (On Linux the OS ignores it under
// SCHED_OTHER; QThread still reports what it was started with.)
void JobQueueTest::workerRunsBelowNormalPriority()
{
    CalculationDescriptor probe;
    probe.id = QStringLiteral("priorityprobe");
    probe.title = QStringLiteral("Priority probe");
    probe.policy = EvaluationPolicy::Explicit;
    probe.inputs = {CalcInput::attribute(QStringLiteral("P_IN"))};
    probe.outputs = {DependencyKey::attribute(QStringLiteral("PR_OUT"))};
    probe.compute = [](const EvaluationContext &ctx) {
        g_probePriority.store(int(QThread::currentThread()->priority()));
        return CalculationResult().setAttribute(QStringLiteral("PR_OUT"),
                                                ctx.attribute(QStringLiteral("P_IN")).toInt() + 1);
    };
    CalculationRegistry &registry = CalculationRegistry::instance();
    QVERIFY(registry.registerCalculation(probe));
    const auto unregister = qScopeGuard([&registry] {
        registry.unregister(QStringLiteral("priorityprobe"), CalculationRegistry::Removal::Change);
    });
    m_model->flushPendingInvalidations();

    g_probePriority.store(-1);
    QVERIFY(setInput("s1", "P_IN", 4));
    QCOMPARE(m_queue->offer("s1", QStringLiteral("priorityprobe")).kind, Kind::Created);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(1), JobState::Succeeded);
    QCOMPARE(session("s1").getAttribute("PR_OUT"), QVariant(5));
    QCOMPARE(QThread::Priority(g_probePriority.load()), QThread::LowPriority);
}

// A job held inside its compute function holds nothing on the main thread:
// the event loop keeps turning, and an edit of another session returns while
// the job is still inside the gate (were it to wait for the job, it would
// never return, since the gate is opened only afterwards).
void JobQueueTest::mainThreadIsNotBlockedByARunningJob()
{
    QVERIFY(setInput("s1", "G_IN", 4));
    const JobId held = m_queue->offer("s1", QStringLiteral("gated")).job;
    QVERIFY(gate().waitEntered());

    int ticks = 0;
    QTimer timer;
    timer.setInterval(0);
    connect(&timer, &QTimer::timeout, &timer, [&ticks] { ++ticks; });
    timer.start();
    QTRY_VERIFY(ticks >= 50);
    timer.stop();
    QCOMPARE(stateOf(held), JobState::Running);
    QCOMPARE(gate().running.load(), 1);

    QVERIFY(m_model->updateAttribute("s2", "EA_IN", 4));
    QCOMPARE(session("s2").getAttribute("EA_IN"), QVariant(4));
    QCOMPARE(stateOf(held), JobState::Running);         // the gate is still closed
    QVERIFY(!m_queue->job(held).cancelRequested);
    QCOMPARE(gate().running.load(), 1);

    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(held), JobState::Succeeded);
}

// Acceptance 14
void JobQueueTest::duplicateOffersCreateNoDuplicates()
{
    QVERIFY(setInput("s1", "G_IN", 4));
    QVERIFY(setInput("s2", "G_IN", 6));
    QVERIFY(setInput("s1", "EA_IN", 4));

    const JobQueue::OfferResult first = m_queue->offer("s1", QStringLiteral("gated"));
    QCOMPARE(first.kind, Kind::Created);

    // While it is the chosen next job
    JobQueue::OfferResult again = m_queue->offer("s1", QStringLiteral("gated"));
    QCOMPARE(again.kind, Kind::AlreadyActive);
    QCOMPARE(again.job, first.job);
    QVERIFY(!again.created());
    QCOMPARE(m_queue->chosenNextJob(), first.job);
    QCOMPARE(m_queue->model()->rowCount(), 1);

    // While running
    QVERIFY(gate().waitEntered());
    QCOMPARE(stateOf(first.job), JobState::Running);
    QCOMPARE(m_queue->chosenNextJob(), JobId(0));
    again = m_queue->offer("s1", QStringLiteral("gated"));
    QCOMPARE(again.kind, Kind::AlreadyActive);
    QCOMPARE(again.job, first.job);
    QCOMPARE(m_queue->chosenNextJob(), JobId(0));
    QCOMPARE(m_queue->model()->rowCount(), 1);
    QVERIFY(m_model->isSessionPinned("s1"));

    // Another session is another job: the chosen next one
    const JobQueue::OfferResult other = m_queue->offer("s2", QStringLiteral("gated"));
    QCOMPARE(other.kind, Kind::Created);
    QCOMPARE(m_queue->chosenNextJob(), other.job);
    QVERIFY(m_model->isSessionPinned("s2"));

    // Equal to the running job: nothing changes, the chosen next job included
    again = m_queue->offer("s1", QStringLiteral("gated"));
    QCOMPARE(again.kind, Kind::AlreadyActive);
    QCOMPARE(again.job, first.job);
    QCOMPARE(m_queue->chosenNextJob(), other.job);
    QCOMPARE(stateOf(other.job), JobState::Queued);

    // Another calculation is another job, and it replaces the chosen next job
    const JobQueue::OfferResult replacing = m_queue->offer("s1", QStringLiteral("expA"));
    QCOMPARE(replacing.kind, Kind::Created);
    QCOMPARE(m_queue->chosenNextJob(), replacing.job);
    QCOMPARE(stateOf(other.job), JobState::Cancelled);
    QCOMPARE(m_queue->job(other.job).reason, QStringLiteral("No longer needed"));
    QVERIFY(!m_queue->job(other.job).startedAt.isValid());
    QVERIFY(!m_model->isSessionPinned("s2"));
    QCOMPARE(m_queue->offer("s1", QStringLiteral("expA")).kind, Kind::AlreadyActive);
    QCOMPARE(m_queue->activeJobs(), QList<JobId>({first.job, replacing.job}));
    QCOMPARE(m_queue->model()->rowCount(), 3);

    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(first.job), JobState::Succeeded);
    QCOMPARE(stateOf(replacing.job), JobState::Succeeded);
    QCOMPARE(engine("s1").runCount("gated"), 1);
    QCOMPARE(engine("s2").runCount("gated"), 0);
    QCOMPARE(engine("s1").runCount("expA"), 1);
    QCOMPARE(session("s1").getAttribute("G_OUT"), QVariant(5));
    QCOMPARE(session("s1").getAttribute("EA1"), QVariant(5));
    QVERIFY(!session("s2").getAttribute("G_OUT").isValid());
    QCOMPARE(gate().maxRunning.load(), 1);
}

// Acceptance 14: five jobs over three sessions, one at a time, each offered
// as the chosen next job while the one before it runs; they start in the
// order they were offered.
void JobQueueTest::oneAtATimeInOfferOrder()
{
    QVERIFY(setInput("s1", "G_IN", 1));
    QVERIFY(setInput("s2", "G_IN", 2));
    QVERIFY(setInput("s3", "G_IN", 3));
    QVERIFY(setInput("s1", "S_IN", 4));
    QVERIFY(setInput("s2", "S_IN", 5));

    const QList<QPair<QString, QString>> script = {
        {QStringLiteral("s1"), QStringLiteral("gated")},
        {QStringLiteral("s2"), QStringLiteral("gated")},
        {QStringLiteral("s1"), QStringLiteral("stubborn")},
        {QStringLiteral("s3"), QStringLiteral("gated")},
        {QStringLiteral("s2"), QStringLiteral("stubborn")}};

    QList<JobId> ids;
    ids.append(m_queue->offer(script.at(0).first, script.at(0).second).job);
    QCOMPARE(m_queue->activeJobs(), ids);

    for (int i = 0; i < script.size(); ++i) {
        QVERIFY(gate().waitEntered());
        QCOMPARE(gate().running.load(), 1);
        QCOMPARE(m_queue->runningJob(), ids.at(i));
        QCOMPARE(m_queue->chosenNextJob(), JobId(0));
        if (i + 1 < script.size()) {
            const JobQueue::OfferResult next = m_queue->offer(script.at(i + 1).first, script.at(i + 1).second);
            QCOMPARE(next.kind, Kind::Created);
            ids.append(next.job);
            QCOMPARE(m_queue->chosenNextJob(), next.job);
            QCOMPARE(m_queue->activeJobs(), QList<JobId>({ids.at(i), next.job}));
        } else {
            QCOMPARE(m_queue->activeJobs(), QList<JobId>({ids.at(i)}));
        }
        for (int j = 0; j < ids.size(); ++j) {
            const JobState expected = j < i ? JobState::Succeeded
                                    : j == i ? JobState::Running : JobState::Queued;
            QCOMPARE(stateOf(ids.at(j)), expected);
        }
        gate().open(1);
        QTRY_COMPARE(stateOf(ids.at(i)), JobState::Succeeded);
    }
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(ids, QList<JobId>({1, 2, 3, 4, 5}));

    QCOMPARE(gate().maxRunning.load(), 1);
    QCOMPARE(gate().startOrder(), QList<int>({1, 2, 4, 3, 5}));
    for (int i = 1; i < ids.size(); ++i)
        QVERIFY(m_queue->job(ids.at(i)).startedAt >= m_queue->job(ids.at(i - 1)).finishedAt);
    QCOMPARE(session("s3").getAttribute("G_OUT"), QVariant(4));
    QCOMPARE(session("s2").getAttribute("S_OUT"), QVariant(6));
}

// The executor's bound, observed at every announcement: at most the running
// job and one chosen next job are active, through offers, a replacement, a
// withdrawal, a cancel, starts, ends and an input change.
void JobQueueTest::holdsAtMostRunningAndChosenNext()
{
    QCOMPARE(JobQueue::kMaxRunningJobs, 1);

    QVERIFY(setInput("s1", "G_IN", 4));
    QVERIFY(setInput("s1", "EA_IN", 4));
    QVERIFY(setInput("s2", "EA_IN", 4));
    QVERIFY(setInput("s3", "EA_IN", 4));

    int maxActive = 0;
    int maxQueued = 0;
    int observations = 0;
    const auto observe = [&] {
        int active = 0;
        int queued = 0;
        for (const JobRecord &job : m_queue->model()->records()) {
            active += job.isActive() ? 1 : 0;
            queued += job.state == JobState::Queued ? 1 : 0;
        }
        maxActive = qMax(maxActive, active);
        maxQueued = qMax(maxQueued, queued);
        ++observations;
    };
    QObject scope;      // owns the connections: they cannot outlive what the slots capture
    connect(m_queue.get(), &JobQueue::jobsChanged, &scope, observe);
    connect(m_queue->model(), &QAbstractItemModel::rowsInserted, &scope, observe);
    connect(m_queue->model(), &QAbstractItemModel::dataChanged, &scope, observe);

    // Offer, and a replacement in the same event-loop turn
    const JobId replacedBeforeStart = m_queue->offer("s2", QStringLiteral("expA")).job;
    const JobId held = m_queue->offer("s1", QStringLiteral("gated")).job;
    QCOMPARE(stateOf(replacedBeforeStart), JobState::Cancelled);
    QVERIFY(gate().waitEntered());                      // start

    // A replacement while a job runs
    const JobId replaced = m_queue->offer("s2", QStringLiteral("expA")).job;
    const JobId withdrawn = m_queue->offer("s3", QStringLiteral("expA")).job;
    QCOMPARE(stateOf(replaced), JobState::Cancelled);

    // Withdraw, then cancel
    QVERIFY(m_queue->withdrawChosenNext());
    QCOMPARE(stateOf(withdrawn), JobState::Cancelled);
    const JobId cancelled = m_queue->offer("s2", QStringLiteral("expA")).job;
    QVERIFY(m_queue->cancel(cancelled));
    QCOMPARE(stateOf(cancelled), JobState::Cancelled);

    // An input change stops the running job; the chosen next job runs after it
    const JobId next = m_queue->offer("s3", QStringLiteral("expA")).job;
    QVERIFY(setInput("s1", "G_IN", 7));
    QVERIFY(m_queue->job(held).cancelRequested);
    QVERIFY(waitStarted(*m_queue, next));
    QVERIFY(waitIdle(*m_queue));                        // ends
    QCOMPARE(stateOf(held), JobState::Superseded);
    QCOMPARE(stateOf(next), JobState::Succeeded);

    // Offered again with the new input, run to its end
    const JobId again = m_queue->offer("s1", QStringLiteral("gated")).job;
    QVERIFY(gate().waitEntered());
    const JobId last = m_queue->offer("s1", QStringLiteral("expA")).job;
    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(again), JobState::Succeeded);
    QCOMPARE(stateOf(last), JobState::Succeeded);
    QCOMPARE(session("s1").getAttribute("G_OUT"), QVariant(8));

    QVERIFY(observations > 0);
    QCOMPARE(maxActive, 2);
    QCOMPARE(maxQueued, 1);
    QCOMPARE(gate().maxRunning.load(), 1);
}

// An offer that differs from the chosen next job replaces it: the old job
// ends Cancelled ("No longer needed") without ever running, its session is
// unpinned, and no idle() falls between the two.
void JobQueueTest::offerReplacesChosenNext()
{
    QVERIFY(setInput("s1", "G_IN", 4));
    QVERIFY(setInput("s1", "EA_IN", 4));
    QVERIFY(setInput("s2", "EA_IN", 4));
    QVERIFY(setInput("s3", "EA_IN", 4));
    QSignalSpy idleSpy(m_queue.get(), &JobQueue::idle);
    QSignalSpy startedSpy(m_queue.get(), &JobQueue::jobStarted);

    // With a held running job
    const JobId held = m_queue->offer("s1", QStringLiteral("gated")).job;
    QVERIFY(gate().waitEntered());
    const JobQueue::OfferResult a = m_queue->offer("s2", QStringLiteral("expA"));
    QCOMPARE(a.kind, Kind::Created);
    QCOMPARE(m_queue->chosenNextJob(), a.job);
    QVERIFY(m_model->isSessionPinned("s2"));

    const JobQueue::OfferResult b = m_queue->offer("s3", QStringLiteral("expA"));
    QCOMPARE(b.kind, Kind::Created);
    QCOMPARE(stateOf(a.job), JobState::Cancelled);
    QCOMPARE(m_queue->job(a.job).reason, QStringLiteral("No longer needed"));
    QVERIFY(!m_queue->job(a.job).startedAt.isValid());
    QVERIFY(m_queue->job(a.job).finishedAt.isValid());
    QVERIFY(!m_model->isSessionPinned("s2"));
    QVERIFY(m_model->isSessionPinned("s3"));
    QCOMPARE(m_queue->chosenNextJob(), b.job);
    QCOMPARE(m_queue->activeJobs(), QList<JobId>({held, b.job}));
    QCOMPARE(stateOf(held), JobState::Running);
    QVERIFY(!m_queue->job(held).cancelRequested);

    const JobQueue::OfferResult bAgain = m_queue->offer("s3", QStringLiteral("expA"));
    QCOMPARE(bAgain.kind, Kind::AlreadyActive);
    QCOMPARE(bAgain.job, b.job);
    QCOMPARE(m_queue->model()->rowCount(), 3);
    QCOMPARE(idleSpy.count(), 0);

    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(held), JobState::Succeeded);
    QCOMPARE(stateOf(b.job), JobState::Succeeded);
    QCOMPARE(engine("s2").runCount("expA"), 0);
    QCOMPARE(idleSpy.count(), 1);

    // With nothing running: A then B in the same event-loop turn. B runs, and
    // idle() comes once, at the very end.
    idleSpy.clear();
    startedSpy.clear();
    QList<JobState> bStateAtIdle;
    QObject scope;      // owns the connection: it cannot outlive what the slot captures
    JobId bId = 0;
    connect(m_queue.get(), &JobQueue::idle, &scope, [&] { bStateAtIdle.append(stateOf(bId)); });

    const JobQueue::OfferResult a2 = m_queue->offer("s2", QStringLiteral("expA"));
    QCOMPARE(a2.kind, Kind::Created);
    const JobQueue::OfferResult b2 = m_queue->offer("s1", QStringLiteral("expA"));
    QCOMPARE(b2.kind, Kind::Created);
    bId = b2.job;
    QCOMPARE(stateOf(a2.job), JobState::Cancelled);
    QCOMPARE(m_queue->job(a2.job).reason, QStringLiteral("No longer needed"));
    QCOMPARE(stateOf(b2.job), JobState::Queued);
    QCOMPARE(m_queue->chosenNextJob(), b2.job);
    QCOMPARE(idleSpy.count(), 0);
    QVERIFY(!m_model->isSessionPinned("s2"));

    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(b2.job), JobState::Succeeded);
    QVERIFY(!m_queue->job(a2.job).startedAt.isValid());
    QCOMPARE(startedSpy.count(), 1);
    QCOMPARE(startedSpy.at(0).at(0).value<JobId>(), b2.job);
    QCOMPARE(idleSpy.count(), 1);
    QCOMPARE(bStateAtIdle.size(), 1);
    QCOMPARE(bStateAtIdle.first(), JobState::Succeeded);
    QCOMPARE(engine("s2").runCount("expA"), 0);
    QCOMPARE(session("s1").getAttribute("EA1"), QVariant(5));
}

// withdrawChosenNext() ends the chosen next job as a replacement would, and
// leaves the running job alone.
void JobQueueTest::withdrawEndsChosenNext()
{
    QVERIFY(setInput("s1", "G_IN", 4));
    QVERIFY(setInput("s2", "EA_IN", 4));
    QSignalSpy idleSpy(m_queue.get(), &JobQueue::idle);
    QSignalSpy startedSpy(m_queue.get(), &JobQueue::jobStarted);

    // None
    QVERIFY(!m_queue->withdrawChosenNext());
    QCOMPARE(m_queue->model()->rowCount(), 0);
    QCOMPARE(idleSpy.count(), 0);

    // One, behind a running job
    const JobId held = m_queue->offer("s1", QStringLiteral("gated")).job;
    QVERIFY(gate().waitEntered());
    const JobId next = m_queue->offer("s2", QStringLiteral("expA")).job;
    QVERIFY(m_model->isSessionPinned("s2"));
    QVERIFY(m_queue->withdrawChosenNext());
    QCOMPARE(stateOf(next), JobState::Cancelled);
    QCOMPARE(m_queue->job(next).reason, QStringLiteral("No longer needed"));
    QVERIFY(!m_queue->job(next).startedAt.isValid());
    QVERIFY(!m_model->isSessionPinned("s2"));
    QCOMPARE(m_queue->chosenNextJob(), JobId(0));
    QCOMPARE(m_queue->runningJob(), held);
    QCOMPARE(stateOf(held), JobState::Running);
    QVERIFY(!m_queue->job(held).cancelRequested);
    QCOMPARE(idleSpy.count(), 0);                       // the held job still runs
    QVERIFY(!m_queue->withdrawChosenNext());

    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(held), JobState::Succeeded);
    QCOMPARE(idleSpy.count(), 1);

    // One, with nothing running: idle() at once
    const JobId alone = m_queue->offer("s2", QStringLiteral("expA")).job;
    QVERIFY(!m_queue->isIdle());
    QVERIFY(m_queue->withdrawChosenNext());
    QCOMPARE(stateOf(alone), JobState::Cancelled);
    QCOMPARE(m_queue->job(alone).reason, QStringLiteral("No longer needed"));
    QVERIFY(m_queue->isIdle());
    QCOMPARE(idleSpy.count(), 2);
    QVERIFY(!m_model->isSessionPinned("s2"));

    // Neither withdrawn job ever ran
    QCoreApplication::processEvents();
    QCOMPARE(startedSpy.count(), 1);
    QCOMPARE(startedSpy.at(0).at(0).value<JobId>(), held);
    QCOMPARE(engine("s2").runCount("expA"), 0);
    QVERIFY(!m_queue->job(alone).startedAt.isValid());
}

// ---- Refusals ---------------------------------------------------------------------------

// Acceptance 11 (executor half): no job can be created for a session without the inputs.
void JobQueueTest::refusesMissingInput()
{
    QSignalSpy queuedSpy(m_queue.get(), &JobQueue::jobQueued);
    for (const char *id : {"gated", "expA", "deepstack"}) {
        const JobQueue::OfferResult result = m_queue->offer("s1", QString::fromLatin1(id));
        QCOMPARE(result.kind, Kind::MissingInput);
        QCOMPARE(result.job, JobId(0));
    }
    QCOMPARE(m_queue->model()->rowCount(), 0);
    QCOMPARE(queuedSpy.count(), 0);
    QVERIFY(m_queue->isIdle());
    QVERIFY(!m_model->isSessionPinned("s1"));
    QCOMPARE(engine("s1").totalRunCount(), 0);

    // Control: with the input the same offer is accepted. It is the chosen
    // next job behind a held one, and a refused offer leaves it untouched.
    QVERIFY(setInput("s1", "G_IN", 4));
    QVERIFY(setInput("s1", "EA_IN", 4));
    const JobId held = m_queue->offer("s1", QStringLiteral("gated")).job;
    QVERIFY(gate().waitEntered());
    const JobQueue::OfferResult next = m_queue->offer("s1", QStringLiteral("expA"));
    QCOMPARE(next.kind, Kind::Created);
    QCOMPARE(m_queue->chosenNextJob(), next.job);

    const JobQueue::OfferResult refused = m_queue->offer("s2", QStringLiteral("expA"));
    QCOMPARE(refused.kind, Kind::MissingInput);
    QCOMPARE(refused.job, JobId(0));
    QCOMPARE(m_queue->chosenNextJob(), next.job);
    QCOMPARE(stateOf(next.job), JobState::Queued);
    QCOMPARE(m_queue->activeJobs(), QList<JobId>({held, next.job}));
    QCOMPARE(m_queue->model()->rowCount(), 2);
    QVERIFY(!m_model->isSessionPinned("s2"));

    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(held), JobState::Succeeded);
    QCOMPARE(stateOf(next.job), JobState::Succeeded);
    QCOMPARE(engine("s2").totalRunCount(), 0);
}

void JobQueueTest::refusesUnloadedAndUnknownSession()
{
    QCOMPARE(m_queue->offer("no-such-session", QStringLiteral("expA")).kind, Kind::SessionNotLoaded);
    QCOMPARE(m_queue->offer(QString(), QStringLiteral("expA")).kind, Kind::SessionNotLoaded);

    // s1 becomes a stub: room for one, and s3 was used last
    QVERIFY(setInput("s1", "EA_IN", 4));
    QVERIFY(waitForIdle(*m_model));
    session("s3");
    const auto restoreCapacity = qScopeGuard([] {
        PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 50);
    });
    PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 1);
    QVERIFY(!isLoaded("s1"));

    QSignalSpy loadedSpy(m_model.get(), &SessionModel::sessionLoaded);
    const JobQueue::OfferResult result = m_queue->offer("s1", QStringLiteral("expA"));
    QCOMPARE(result.kind, Kind::SessionNotLoaded);
    QCOMPARE(result.job, JobId(0));
    QVERIFY(!isLoaded("s1"));
    QCOMPARE(loadedSpy.count(), 0);
    QCOMPARE(m_queue->model()->rowCount(), 0);
}

void JobQueueTest::refusesBlockedAndDone()
{
    QVERIFY(setInput("s1", "EA_IN", 4));
    QVERIFY(setInput("s1", "EB_IN", 10));
    QVERIFY(setInput("s2", "EA_IN", -1));

    QCOMPARE(m_queue->offer("s1", QStringLiteral("no-such-calculation")).kind, Kind::UnknownCalculation);

    // B consumes an output of A, which has not been requested
    QCOMPARE(m_queue->offer("s1", QStringLiteral("expB")).kind, Kind::Blocked);
    QCOMPARE(m_queue->model()->rowCount(), 0);

    QCOMPARE(m_queue->offer("s1", QStringLiteral("expA")).kind, Kind::Created);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(m_queue->offer("s1", QStringLiteral("expA")).kind, Kind::NothingToDo);     // published
    QCOMPARE(m_queue->offer("s1", QStringLiteral("derivA")).kind, Kind::NothingToDo);   // on demand
    QCOMPARE(m_queue->model()->rowCount(), 1);

    // Now B can run. The blocker form of offer() is the one chains use.
    const BlockerReport report = engine("s1").blockers(DependencyKey::attribute(QStringLiteral("DB")));
    QCOMPARE(report.state, BlockerReport::State::Blocked);
    QCOMPARE(report.blockers.size(), 1);
    const JobQueue::OfferResult b = m_queue->offer("s1", report.blockers.first());
    QCOMPARE(b.kind, Kind::Created);
    QCOMPARE(m_queue->job(b.job).calculationTitle, QStringLiteral("Explicit B"));
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(session("s1").getAttribute("EB1"), QVariant(18));
    QCOMPARE(session("s1").getAttribute("DB"), QVariant(19));

    // A cached rejection is done too: the same inputs give the same answer
    QCOMPARE(m_queue->offer("s2", QStringLiteral("expA")).kind, Kind::Created);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(m_queue->offer("s2", QStringLiteral("expA")).kind, Kind::NothingToDo);
    QCOMPARE(engine("s2").runCount("expA"), 1);
}

void JobQueueTest::neverLoadsASession()
{
    QVERIFY(setInput("s1", "G_IN", 4));
    QVERIFY(setInput("s2", "EA_IN", 4));
    QVERIFY(setInput("s3", "EA_IN", 4));
    QVERIFY(waitForIdle(*m_model));

    // s3 is a stub with everything a job would need
    session("s3");
    session("s2");
    session("s1");
    const auto restoreCapacity = qScopeGuard([] {
        PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 50);
    });
    PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 2);
    QVERIFY(!isLoaded("s3"));
    QVERIFY(isLoaded("s1") && isLoaded("s2"));

    QSignalSpy loadedSpy(m_model.get(), &SessionModel::sessionLoaded);
    QCOMPARE(m_queue->offer("s3", QStringLiteral("expA")).kind, Kind::SessionNotLoaded);
    const JobQueue::OfferResult held = m_queue->offer("s1", QStringLiteral("gated"));
    QCOMPARE(held.kind, Kind::Created);
    QVERIFY(gate().waitEntered());
    QCOMPARE(m_queue->offer("s2", QStringLiteral("expA")).kind, Kind::Created);
    QCOMPARE(m_queue->offer("s3", QStringLiteral("expA")).kind, Kind::SessionNotLoaded);
    QCOMPARE(m_queue->activeJob("s3", "expA"), JobId(0));
    QVERIFY(m_queue->withdrawChosenNext());
    QVERIFY(m_queue->cancel(held.job));
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(m_queue->offer("s3", QStringLiteral("expA")).kind, Kind::SessionNotLoaded);
    m_queue->shutdown();

    QCOMPARE(loadedSpy.count(), 0);
    QVERIFY(!isLoaded("s3"));
}

// ---- Superseded ---------------------------------------------------------------------------

// Acceptance 8 (executor half)
void JobQueueTest::inputChangeWhileRunningSupersedes()
{
    QVERIFY(setInput("s1", "G_IN", 4));
    QVERIFY(!session("s1").getAttribute("G_OUT").isValid());    // read while unrequested

    const JobId first = m_queue->offer("s1", QStringLiteral("gated")).job;
    QVERIFY(gate().waitEntered());

    // A declared input changes under the running job. (The edit itself
    // announces G_OUT, whose cached "unavailable" depended on G_IN; what must
    // not happen is an announcement when the job ends.)
    QVERIFY(setInput("s1", "G_IN", 7));
    QSignalSpy dependencySpy(m_model.get(), &SessionModel::dependencyChanged);
    QVERIFY(!session("s1").getAttribute("G_OUT").isValid());

    gate().open(1);
    QTRY_COMPARE(stateOf(first), JobState::Superseded);
    QCOMPARE(m_queue->job(first).reason, QStringLiteral("Inputs changed"));
    QVERIFY(!m_queue->job(first).resultStatus.has_value());
    QVERIFY(m_queue->job(first).startedAt.isValid());
    QCOMPARE(publishedTrace(dependencySpy, "s1", "gated", "G_OUT"), QString());
    QVERIFY(waitIdle(*m_queue));        // the executor does not offer again on its own
    QCOMPARE(m_queue->model()->rowCount(), 1);

    // Still requestable, and a new offer computes the new value
    QCOMPARE(engine("s1").readiness("gated").state, CalculationReadiness::State::Ready);
    const JobQueue::OfferResult second = m_queue->offer("s1", QStringLiteral("gated"));
    QCOMPARE(second.kind, Kind::Created);
    QVERIFY(gate().waitEntered());
    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(second.job), JobState::Succeeded);
    QCOMPARE(session("s1").getAttribute("G_OUT"), QVariant(8));
    QVERIFY(spyHasAttribute(dependencySpy, "s1", "G_OUT"));
}

// Inputs are captured when a job starts. A chosen next job whose inputs went
// away while it waited ends without a worker, and the executor moves on. Three
// rounds, one per case; each holds a gated job, offers the case's job as the
// chosen next job behind it, applies the change, and lets the held job end.
void JobQueueTest::inputChangeWhileQueuedSupersedesAtStart()
{
    QVERIFY(setInput("s1", "G_IN", 4));
    QVERIFY(setInput("s2", "G_IN", 5));
    QVERIFY(setInput("s3", "G_IN", 6));
    QVERIFY(setInput("s1", "EA_IN", 4));
    QVERIFY(setInput("s2", "EA_IN", 4));
    QVERIFY(setInput("s3", "EA_IN", 4));
    QVERIFY(setInput("s3", "EB_IN", 10));

    // Preparation: A is published in s3, so that B is ready there
    QCOMPARE(m_queue->offer("s3", QStringLiteral("expA")).kind, Kind::Created);
    QVERIFY(waitIdle(*m_queue));

    QSignalSpy startedSpy(m_queue.get(), &JobQueue::jobStarted);

    // 1. Missing input: s2 loses the input while its job waits
    const JobId held1 = m_queue->offer("s1", QStringLiteral("gated")).job;
    QVERIFY(gate().waitEntered());
    const JobId missing = m_queue->offer("s2", QStringLiteral("expA")).job;
    const JobId valid = m_queue->offer("s2", QStringLiteral("expA")).job;     // same job: deduplicated
    QCOMPARE(valid, missing);
    QVERIFY(m_model->removeAttribute("s2", "EA_IN"));
    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(held1), JobState::Succeeded);
    QCOMPARE(stateOf(missing), JobState::Superseded);
    QCOMPARE(m_queue->job(missing).reason, QStringLiteral("Inputs changed: nothing to compute"));

    // 2. Blocked by an input change: s3's A result is dropped, which blocks B
    const JobId held2 = m_queue->offer("s2", QStringLiteral("gated")).job;
    QVERIFY(gate().waitEntered());
    const JobId blocked = m_queue->offer("s3", QStringLiteral("expB")).job;
    QVERIFY(setInput("s3", "EA_IN", 5));
    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(held2), JobState::Succeeded);
    QCOMPARE(stateOf(blocked), JobState::Superseded);
    QCOMPARE(m_queue->job(blocked).reason, QStringLiteral("Inputs changed: waiting for Explicit A"));

    // 3. Already valid: s1's A is computed synchronously while its job waits
    const JobId held3 = m_queue->offer("s3", QStringLiteral("gated")).job;
    QVERIFY(gate().waitEntered());
    const JobId alreadyValid = m_queue->offer("s1", QStringLiteral("expA")).job;
    QCOMPARE(engine("s1").request("expA").status, ResultStatus::Ok);
    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(held3), JobState::Succeeded);
    QCOMPARE(stateOf(alreadyValid), JobState::Superseded);
    QCOMPARE(m_queue->job(alreadyValid).reason, QStringLiteral("Result is already available"));

    QVERIFY(missing != 0 && blocked != 0 && alreadyValid != 0);
    for (const JobId id : {missing, blocked, alreadyValid}) {
        QVERIFY(!m_queue->job(id).startedAt.isValid());
        QVERIFY(m_queue->job(id).finishedAt.isValid());
    }

    // The executor goes on to the next offer
    QVERIFY(setInput("s1", "T_IN", 4));
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("synthetic failure")));
    const JobId last = m_queue->offer("s1", QStringLiteral("thrower")).job;
    QVERIFY(last != 0);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(last), JobState::Succeeded);

    // No worker for the three
    QList<JobId> started;
    for (const QList<QVariant> &args : startedSpy)
        started.append(args.at(0).toULongLong());
    QCOMPARE(started, QList<JobId>({held1, held2, held3, last}));
    QVERIFY(!session("s3").getAttribute("EB1").isValid());
}

void JobQueueTest::registrationRemovedSupersedes()
{
    CalculationRegistry &registry = CalculationRegistry::instance();
    QVERIFY(setInput("s1", "G_IN", 4));
    QVERIFY(setInput("s2", "T_IN", 4));
    QVERIFY(setInput("s3", "EA_IN", 4));
    QVERIFY(!session("s1").getAttribute("G_OUT").isValid());

    const JobId running = m_queue->offer("s1", QStringLiteral("gated")).job;
    QVERIFY(gate().waitEntered());
    const JobId queued = m_queue->offer("s2", QStringLiteral("thrower")).job;
    QCOMPARE(m_queue->chosenNextJob(), queued);

    // Both go away; the fixture gets them back at the end so that its own
    // clean-up stays balanced.
    const CalculationDescriptor gatedDescriptor = *registry.instance(QStringLiteral("gated"))->descriptor;
    const CalculationDescriptor throwerDescriptor = *registry.instance(QStringLiteral("thrower"))->descriptor;
    QVERIFY(registry.unregister(QStringLiteral("gated"), CalculationRegistry::Removal::Change));
    QVERIFY(registry.unregister(QStringLiteral("thrower"), CalculationRegistry::Removal::Change));
    m_model->flushPendingInvalidations();
    QSignalSpy dependencySpy(m_model.get(), &SessionModel::dependencyChanged);

    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(running), JobState::Superseded);
    QCOMPARE(m_queue->job(running).reason, QStringLiteral("Calculation is no longer registered"));
    QCOMPARE(stateOf(queued), JobState::Superseded);
    QCOMPARE(m_queue->job(queued).reason, QStringLiteral("Calculation is no longer registered"));
    QVERIFY(!m_queue->job(queued).startedAt.isValid());
    QVERIFY(!spyHasAttribute(dependencySpy, "s1", "G_OUT"));
    QVERIFY(!session("s1").getAttribute("G_OUT").isValid());
    QCOMPARE(m_queue->offer("s1", QStringLiteral("gated")).kind, Kind::UnknownCalculation);

    // What is still registered still runs
    const JobQueue::OfferResult next = m_queue->offer("s3", QStringLiteral("expA"));
    QCOMPARE(next.kind, Kind::Created);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(next.job), JobState::Succeeded);

    QVERIFY(registry.registerCalculation(gatedDescriptor));
    QVERIFY(registry.registerCalculation(throwerDescriptor));
    m_model->flushPendingInvalidations();
}

// ---- Stale while running: stopped early, ended Superseded ---------------------------------------

// Acceptance 8 (executor half), sooner: the engine marked the ticket when the
// input changed, and the executor asks the compute function to stop there and
// then. The gate is never opened, so the run cannot have reached its end.
void JobQueueTest::staleRunningJobIsStoppedAtOnce()
{
    QVERIFY(setInput("s1", "G_IN", 4));
    QVERIFY(setInput("s2", "EA_IN", 4));
    QVERIFY(!session("s1").getAttribute("G_OUT").isValid());
    QSignalSpy cancelSpy(m_queue.get(), &JobQueue::jobCancelRequested);
    QSignalSpy progressSpy(m_queue.get(), &JobQueue::jobProgress);

    const JobId first = m_queue->offer("s1", QStringLiteral("gated")).job;
    QVERIFY(gate().waitEntered());
    const JobId next = m_queue->offer("s2", QStringLiteral("expA")).job;
    QCOMPARE(m_queue->activeJob("s1", "gated"), first);

    // An edit of another session, or of a name the job does not depend on,
    // stops nothing
    QVERIFY(setInput("s2", "G_IN", 1));
    QVERIFY(setInput("s1", "S_IN", 1));
    QVERIFY(!m_queue->job(first).cancelRequested);
    QCOMPARE(cancelSpy.count(), 0);

    QVERIFY(setInput("s1", "G_IN", 7));
    QSignalSpy dependencySpy(m_model.get(), &SessionModel::dependencyChanged);

    // Synchronously with the edit: asked to stop, still running, no longer
    // what a new offer is compared with
    QCOMPARE(cancelSpy.count(), 1);
    QCOMPARE(cancelSpy.at(0).at(0).toULongLong(), first);
    QVERIFY(m_queue->job(first).cancelRequested);
    QCOMPARE(stateOf(first), JobState::Running);
    QCOMPARE(m_queue->runningJob(), first);
    QCOMPARE(m_queue->activeJob("s1", "gated"), JobId(0));
    QCOMPARE(stateOf(next), JobState::Queued);

    // A second change asks nothing twice
    QVERIFY(setInput("s1", "G_IN", 8));
    QCOMPARE(cancelSpy.count(), 1);

    QTRY_COMPARE(stateOf(first), JobState::Superseded);
    QCOMPARE(m_queue->job(first).reason, QStringLiteral("Inputs changed"));
    QVERIFY(!m_queue->job(first).cancelRequested);
    QVERIFY(!m_queue->job(first).resultStatus.has_value());
    QCOMPARE(gate().proceed.available(), 0);        // nobody let it through
    for (const QList<QVariant> &args : progressSpy)
        QVERIFY(args.at(1).toString() != QStringLiteral("step 2"));
    QCOMPARE(publishedTrace(dependencySpy, "s1", "gated", "G_OUT"), QString());
    QCOMPARE(engine("s1").runCount("gated"), 0);
    QCOMPARE(engine("s1").readiness("gated").state, CalculationReadiness::State::Ready);

    // The worker is free for the chosen next job, and nothing is offered again
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(next), JobState::Succeeded);
    QVERIFY(m_queue->job(next).startedAt >= m_queue->job(first).finishedAt);
    QCOMPARE(m_queue->model()->rowCount(), 2);
}

// An offer made while the stale job is still winding down is a new job, the
// chosen next job behind it, which runs after the old worker has returned,
// with the new inputs.
void JobQueueTest::offerWhileStaleJobWindsDown()
{
    QVERIFY(setInput("s1", "G_IN", 4));
    const JobId first = m_queue->offer("s1", QStringLiteral("gated")).job;
    QVERIFY(gate().waitEntered());
    QCOMPARE(m_queue->offer("s1", QStringLiteral("gated")).kind, Kind::AlreadyActive);

    QVERIFY(setInput("s1", "G_IN", 7));
    QCOMPARE(stateOf(first), JobState::Running);        // still winding down

    const JobQueue::OfferResult second = m_queue->offer("s1", QStringLiteral("gated"));
    QCOMPARE(second.kind, Kind::Created);
    QVERIFY(second.job != first);
    QCOMPARE(stateOf(second.job), JobState::Queued);
    QCOMPARE(m_queue->activeJob("s1", "gated"), second.job);
    QCOMPARE(m_queue->offer("s1", QStringLiteral("gated")).kind, Kind::AlreadyActive);
    QCOMPARE(m_queue->activeJobs(), QList<JobId>({first, second.job}));

    QVERIFY(gate().waitEntered());                      // the new job: the old one has ended
    QCOMPARE(stateOf(first), JobState::Superseded);
    QCOMPARE(m_queue->job(first).reason, QStringLiteral("Inputs changed"));
    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(second.job), JobState::Succeeded);
    QCOMPARE(session("s1").getAttribute("G_OUT"), QVariant(8));
    QVERIFY(m_queue->job(second.job).startedAt >= m_queue->job(first).finishedAt);
    QCOMPARE(gate().maxRunning.load(), 1);
    QCOMPARE(gate().startOrder(), QList<int>({4, 7}));
}

// A compute function that answers the request with a complete result changes
// nothing: the job is superseded, and the result goes nowhere.
void JobQueueTest::staleJobThatReturnsAResultIsStillSuperseded()
{
    QVERIFY(setInput("s1", "S_IN", 4));
    QVERIFY(!session("s1").getAttribute("S_OUT").isValid());

    const JobId id = m_queue->offer("s1", QStringLiteral("stubborn")).job;
    QVERIFY(gate().waitEntered());
    QVERIFY(setInput("s1", "S_IN", 7));
    QSignalSpy dependencySpy(m_model.get(), &SessionModel::dependencyChanged);
    QVERIFY(m_queue->job(id).cancelRequested);
    QVERIFY(waitIdle(*m_queue));

    QCOMPARE(stateOf(id), JobState::Superseded);
    QCOMPARE(m_queue->job(id).reason, QStringLiteral("Inputs changed"));
    QCOMPARE(publishedTrace(dependencySpy, "s1", "stubborn", "S_OUT"), QString());
    QCOMPARE(engine("s1").runCount("stubborn"), 0);
    QCOMPARE(engine("s1").readiness("stubborn").state, CalculationReadiness::State::Ready);
}

void JobQueueTest::registrationRemovedStopsRunningJobAtOnce()
{
    CalculationRegistry &registry = CalculationRegistry::instance();
    QVERIFY(setInput("s1", "G_IN", 4));
    const JobId running = m_queue->offer("s1", QStringLiteral("gated")).job;
    QVERIFY(gate().waitEntered());

    // An unrelated registration change stops nothing
    const CalculationDescriptor throwerDescriptor = *registry.instance(QStringLiteral("thrower"))->descriptor;
    QVERIFY(registry.unregister(QStringLiteral("thrower"), CalculationRegistry::Removal::Change));
    QVERIFY(!m_queue->job(running).cancelRequested);

    const CalculationDescriptor gatedDescriptor = *registry.instance(QStringLiteral("gated"))->descriptor;
    QVERIFY(registry.unregister(QStringLiteral("gated"), CalculationRegistry::Removal::Change));
    QVERIFY(m_queue->job(running).cancelRequested);     // from the registry's notification
    QCOMPARE(stateOf(running), JobState::Running);

    // Registered again at once: the old run is refused all the same
    QVERIFY(registry.registerCalculation(gatedDescriptor));
    QVERIFY(registry.registerCalculation(throwerDescriptor));
    m_model->flushPendingInvalidations();

    QTRY_COMPARE(stateOf(running), JobState::Superseded);       // the gate is never opened
    QCOMPARE(m_queue->job(running).reason, QStringLiteral("Calculation is no longer registered"));
    QVERIFY(!session("s1").getAttribute("G_OUT").isValid());
    QCOMPARE(engine("s1").preparedCount(), 0);
    QVERIFY(waitIdle(*m_queue));
}

void JobQueueTest::modelDestroyedStopsRunningJobAtOnce()
{
    QVERIFY(setInput("s1", "G_IN", 4));
    const JobId running = m_queue->offer("s1", QStringLiteral("gated")).job;
    QVERIFY(gate().waitEntered());

    m_model.reset();
    QVERIFY(m_queue->job(running).cancelRequested);

    QTRY_COMPARE(stateOf(running), JobState::Superseded);       // the gate is never opened
    QCOMPARE(m_queue->job(running).reason, QString::fromLatin1(kRemoved));
    QVERIFY(waitIdle(*m_queue));

    QCOMPARE(endTransitionErrors(m_queue->model()), QString());
    m_queue.reset();
}

// The pending end is decided once: the first writer wins.
void JobQueueTest::userCancelThenStaleEndsCancelled()
{
    QVERIFY(setInput("s1", "G_IN", 4));
    QSignalSpy cancelSpy(m_queue.get(), &JobQueue::jobCancelRequested);
    const JobId id = m_queue->offer("s1", QStringLiteral("gated")).job;
    QVERIFY(gate().waitEntered());

    QVERIFY(m_queue->cancel(id));
    QVERIFY(setInput("s1", "G_IN", 7));
    QCOMPARE(cancelSpy.count(), 1);

    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(id), JobState::Cancelled);
    QCOMPARE(m_queue->job(id).reason, QStringLiteral("Cancelled"));
    QVERIFY(!session("s1").getAttribute("G_OUT").isValid());
}

void JobQueueTest::staleThenUserCancelEndsSuperseded()
{
    QVERIFY(setInput("s1", "G_IN", 4));
    QSignalSpy cancelSpy(m_queue.get(), &JobQueue::jobCancelRequested);
    const JobId id = m_queue->offer("s1", QStringLiteral("gated")).job;
    QVERIFY(gate().waitEntered());

    QVERIFY(setInput("s1", "G_IN", 7));
    QVERIFY(m_queue->cancel(id));           // accepted: the job is still active
    QVERIFY(m_queue->cancel(id));           // and again, but it had been asked to stop before
    QCOMPARE(cancelSpy.count(), 1);

    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(id), JobState::Superseded);
    QCOMPARE(m_queue->job(id).reason, QStringLiteral("Inputs changed"));
    QVERIFY(!session("s1").getAttribute("G_OUT").isValid());
}

// ---- Results that are functions of the inputs, and failures that are not ----------------------

void JobQueueTest::rejectionSucceedsWithReason()
{
    QVERIFY(setInput("s1", "EA_IN", -1));
    const JobId id = m_queue->offer("s1", QStringLiteral("expA")).job;
    QVERIFY(waitIdle(*m_queue));

    const JobRecord job = m_queue->job(id);
    QCOMPARE(job.state, JobState::Succeeded);
    QCOMPARE(job.resultStatus, std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(job.reason, QStringLiteral("negative input"));
    QVERIFY(!session("s1").getAttribute("EA1").isValid());
    QCOMPARE(session("s1").getAttribute("EA_DIAG"), QVariant(QStringLiteral("rejected")));

    QCOMPARE(m_queue->offer("s1", QStringLiteral("expA")).kind, Kind::NothingToDo);
    QCOMPARE(engine("s1").runCount("expA"), 1);
}

void JobQueueTest::exceptionSucceedsAsFailedResult()
{
    QVERIFY(setInput("s1", "T_IN", 4));
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("synthetic failure")));
    const JobId id = m_queue->offer("s1", QStringLiteral("thrower")).job;
    QVERIFY(waitIdle(*m_queue));

    // Published and cached as a failed result, and the record says so
    const JobRecord job = m_queue->job(id);
    QCOMPARE(job.state, JobState::Succeeded);
    QCOMPARE(job.resultStatus, std::optional<ResultStatus>(ResultStatus::Failed));
    QCOMPARE(job.reason, QStringLiteral("Calculation failed: synthetic failure"));
    QCOMPARE(engine("s1").resultStatus("thrower"), std::optional<ResultStatus>(ResultStatus::Failed));
    QVERIFY(!session("s1").getAttribute("T_OUT").isValid());

    QCOMPARE(m_queue->offer("s1", QStringLiteral("thrower")).kind, Kind::NothingToDo);
    QCOMPARE(m_queue->model()->rowCount(), 1);
}

void JobQueueTest::resourceExhaustionFails()
{
    QVERIFY(setInput("s1", "X_IN", 4));
    QVERIFY(!session("s1").getAttribute("X_OUT").isValid());
    QSignalSpy dependencySpy(m_model.get(), &SessionModel::dependencyChanged);

    const JobId first = m_queue->offer("s1", QStringLiteral("exhausted")).job;
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(first), JobState::Failed);
    QCOMPARE(m_queue->job(first).reason, QStringLiteral("Out of memory"));
    QVERIFY(!m_queue->job(first).resultStatus.has_value());

    // Nothing published, nothing cached: it can be requested again
    QCOMPARE(publishedTrace(dependencySpy, "s1", "exhausted", "X_OUT"), QString());
    QCOMPARE(engine("s1").readiness("exhausted").state, CalculationReadiness::State::Ready);

    const JobQueue::OfferResult second = m_queue->offer("s1", QStringLiteral("exhausted"));
    QCOMPARE(second.kind, Kind::Created);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(second.job), JobState::Succeeded);
    QCOMPARE(session("s1").getAttribute("X_OUT"), QVariant(5));
}

void JobQueueTest::workerStartFailureFails()
{
    QVERIFY(setInput("s1", "EA_IN", 4));
    QVERIFY(setInput("s2", "EA_IN", 4));
    QVERIFY(!session("s1").getAttribute("EA1").isValid());
    QSignalSpy dependencySpy(m_model.get(), &SessionModel::dependencyChanged);

    m_queue->failNextWorkerStarts(1);
    const JobId first = m_queue->offer("s1", QStringLiteral("expA")).job;
    QVERIFY(waitIdle(*m_queue));

    QCOMPARE(stateOf(first), JobState::Failed);
    QCOMPARE(m_queue->job(first).reason, QStringLiteral("The worker thread could not be started"));
    QCOMPARE(publishedTrace(dependencySpy, "s1", "expA", "EA1"), QString());
    QCOMPARE(engine("s1").runCount("expA"), 0);

    // The next job runs, and the failed one is requestable
    const JobId second = m_queue->offer("s2", QStringLiteral("expA")).job;
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(second), JobState::Succeeded);
    QCOMPARE(session("s2").getAttribute("EA1"), QVariant(5));
    const JobQueue::OfferResult retry = m_queue->offer("s1", QStringLiteral("expA"));
    QCOMPARE(retry.kind, Kind::Created);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(retry.job), JobState::Succeeded);
    QCOMPARE(session("s1").getAttribute("EA1"), QVariant(5));
}

// ---- Cancellation ----------------------------------------------------------------------------

// Acceptance 10
void JobQueueTest::cancelRunningThenNextStarts()
{
    QVERIFY(setInput("s1", "G_IN", 4));
    QVERIFY(setInput("s2", "G_IN", 6));
    QVERIFY(!session("s1").getAttribute("G_OUT").isValid());
    QSignalSpy dependencySpy(m_model.get(), &SessionModel::dependencyChanged);
    QSignalSpy cancelSpy(m_queue.get(), &JobQueue::jobCancelRequested);

    const JobId first = m_queue->offer("s1", QStringLiteral("gated")).job;
    QVERIFY(gate().waitEntered());
    const JobId second = m_queue->offer("s2", QStringLiteral("gated")).job;

    // Asked to stop, not stopped yet; the next job has not started
    QVERIFY(m_queue->cancel(first));
    QCOMPARE(stateOf(first), JobState::Running);
    QVERIFY(m_queue->job(first).cancelRequested);
    QCOMPARE(cancelSpy.count(), 1);
    QCOMPARE(stateOf(second), JobState::Queued);
    QCOMPARE(m_queue->runningJob(), first);

    // It stops at its next cancellation check: the gate was never opened
    QTRY_COMPARE(stateOf(first), JobState::Cancelled);
    QCOMPARE(m_queue->job(first).reason, QStringLiteral("Cancelled"));
    QVERIFY(!m_queue->job(first).cancelRequested);
    QVERIFY(!m_queue->job(first).resultStatus.has_value());
    QCOMPARE(publishedTrace(dependencySpy, "s1", "gated", "G_OUT"), QString());
    QCOMPARE(engine("s1").readiness("gated").state, CalculationReadiness::State::Ready);
    QVERIFY(!m_queue->cancel(first));           // already finished
    QVERIFY(!m_queue->cancel(JobId(999)));      // unknown

    // Then the chosen next job starts, and succeeds
    QVERIFY(gate().waitEntered());
    QCOMPARE(m_queue->runningJob(), second);
    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(second), JobState::Succeeded);
    QCOMPARE(session("s2").getAttribute("G_OUT"), QVariant(7));
    QVERIFY(m_queue->job(second).startedAt >= m_queue->job(first).finishedAt);
    QCOMPARE(gate().maxRunning.load(), 1);
    QCOMPARE(gate().startOrder(), QList<int>({4, 6}));
}

// Cancel wins over a completed compute: the function notices the request,
// finishes one more step, and returns a complete result. Nothing is published.
void JobQueueTest::cancelIgnoredForOneStepStillCancelled()
{
    QVERIFY(setInput("s1", "S_IN", 4));
    QVERIFY(!session("s1").getAttribute("S_OUT").isValid());
    QSignalSpy dependencySpy(m_model.get(), &SessionModel::dependencyChanged);

    const JobId id = m_queue->offer("s1", QStringLiteral("stubborn")).job;
    QVERIFY(gate().waitEntered());
    QVERIFY(m_queue->cancel(id));
    QVERIFY(waitIdle(*m_queue));

    QCOMPARE(stateOf(id), JobState::Cancelled);
    QCOMPARE(m_queue->job(id).reason, QStringLiteral("Cancelled"));
    QCOMPARE(publishedTrace(dependencySpy, "s1", "stubborn", "S_OUT"), QString());
    QCOMPARE(engine("s1").runCount("stubborn"), 0);
    QCOMPARE(engine("s1").readiness("stubborn").state, CalculationReadiness::State::Ready);
}

// An offer made right after a cancel must not be lost.
void JobQueueTest::offerWhileCancellingCreatesNewJob()
{
    QVERIFY(setInput("s1", "G_IN", 4));
    const JobId first = m_queue->offer("s1", QStringLiteral("gated")).job;
    QVERIFY(gate().waitEntered());
    QVERIFY(m_queue->cancel(first));
    QCOMPARE(stateOf(first), JobState::Running);        // still winding down
    QCOMPARE(m_queue->activeJob("s1", "gated"), JobId(0));

    const JobQueue::OfferResult second = m_queue->offer("s1", QStringLiteral("gated"));
    QCOMPARE(second.kind, Kind::Created);
    QVERIFY(second.job != first);
    QCOMPARE(stateOf(second.job), JobState::Queued);
    QCOMPARE(m_queue->activeJob("s1", "gated"), second.job);
    QCOMPARE(m_queue->offer("s1", QStringLiteral("gated")).kind, Kind::AlreadyActive);
    QCOMPARE(m_queue->activeJobs(), QList<JobId>({first, second.job}));

    QVERIFY(gate().waitEntered());                      // the new job: the old one has ended
    QCOMPARE(stateOf(first), JobState::Cancelled);
    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(second.job), JobState::Succeeded);
    QCOMPARE(session("s1").getAttribute("G_OUT"), QVariant(5));
    QVERIFY(m_queue->job(second.job).startedAt >= m_queue->job(first).finishedAt);
    QCOMPARE(gate().maxRunning.load(), 1);
}

void JobQueueTest::cancelQueued()
{
    QVERIFY(setInput("s1", "G_IN", 4));
    QVERIFY(setInput("s2", "EA_IN", 4));
    QSignalSpy startedSpy(m_queue.get(), &JobQueue::jobStarted);
    QSignalSpy finishedSpy(m_queue.get(), &JobQueue::jobFinished);

    const JobId held = m_queue->offer("s1", QStringLiteral("gated")).job;
    QVERIFY(gate().waitEntered());
    const JobId queued = m_queue->offer("s2", QStringLiteral("expA")).job;

    // At once, and the record stays as a finished entry
    QVERIFY(m_queue->cancel(queued));
    QCOMPARE(stateOf(queued), JobState::Cancelled);
    QCOMPARE(m_queue->job(queued).reason, QStringLiteral("Cancelled"));
    QVERIFY(!m_queue->job(queued).startedAt.isValid());
    QVERIFY(m_queue->job(queued).finishedAt.isValid());
    QCOMPARE(m_queue->model()->rowCount(), 2);
    QCOMPARE(finishedSpy.count(), 1);
    QVERIFY(!m_model->isSessionPinned("s2"));
    QVERIFY(m_model->isSessionPinned("s1"));
    QVERIFY(!m_queue->cancel(queued));

    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(held), JobState::Succeeded);
    QCOMPARE(startedSpy.count(), 1);                    // never a worker for the cancelled job
    QCOMPARE(engine("s2").runCount("expA"), 0);
    QCOMPARE(m_queue->offer("s2", QStringLiteral("expA")).kind, Kind::Created);
    QVERIFY(waitIdle(*m_queue));
}

// A view of the job model may cancel a job the moment its row appears. The
// session is pinned before the row is announced, so the cancel releases that
// pin: no "session is not pinned" warning and no pin left behind.
void JobQueueTest::cancelFromRowsInsertedLeavesNoPin()
{
    QVERIFY(setInput("s1", "EA_IN", 4));
    QTest::failOnWarning(QRegularExpression(QStringLiteral("unpinSession")));
    QSignalSpy idleSpy(m_queue.get(), &JobQueue::idle);
    QSignalSpy queuedSpy(m_queue.get(), &JobQueue::jobQueued);
    QSignalSpy finishedSpy(m_queue.get(), &JobQueue::jobFinished);

    JobModel *jobs = m_queue->model();
    bool pinnedWhenAnnounced = false;
    bool cancelled = false;
    QObject scope;      // owns the connection: it cannot outlive what the slot captures
    const QMetaObject::Connection connection = connect(
        jobs, &QAbstractItemModel::rowsInserted, &scope, [&](const QModelIndex &, int first, int) {
            pinnedWhenAnnounced = m_model->isSessionPinned("s1");
            cancelled = m_queue->cancel(jobs->record(first).id);
        });

    const JobQueue::OfferResult result = m_queue->offer("s1", QStringLiteral("expA"));
    disconnect(connection);

    QVERIFY(pinnedWhenAnnounced);
    QVERIFY(cancelled);
    QCOMPARE(result.kind, Kind::Created);
    QCOMPARE(stateOf(result.job), JobState::Cancelled);
    QVERIFY(!m_model->isSessionPinned("s1"));
    QVERIFY(m_queue->isIdle());
    QCOMPARE(idleSpy.count(), 1);

    // A job that has ended is not announced as queued afterwards
    QCOMPARE(finishedSpy.count(), 1);
    QCOMPARE(finishedSpy.at(0).at(0).value<JobId>(), result.job);
    QCOMPARE(queuedSpy.count(), 0);

    // Nothing runs for it, and the calculation is requestable as before
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(engine("s1").runCount("expA"), 0);
    QCOMPARE(m_queue->offer("s1", QStringLiteral("expA")).kind, Kind::Created);
    QVERIFY(waitIdle(*m_queue));
    QVERIFY(!m_model->isSessionPinned("s1"));
}

// ---- Sessions that go away or change (acceptance 17) ------------------------------------------

void JobQueueTest::removeSessionWithQueuedJob()
{
    QVERIFY(setInput("s1", "G_IN", 4));
    QVERIFY(setInput("s2", "EA_IN", 4));
    const JobId held = m_queue->offer("s1", QStringLiteral("gated")).job;
    QVERIFY(gate().waitEntered());
    const JobId queued = m_queue->offer("s2", QStringLiteral("expA")).job;

    QVERIFY(m_model->removeSessions({QStringLiteral("s2")}));
    QCOMPARE(stateOf(queued), JobState::Superseded);    // during removeSessions()
    QCOMPARE(m_queue->job(queued).reason, QString::fromLatin1(kRemoved));
    QVERIFY(!m_queue->job(queued).startedAt.isValid());
    QVERIFY(!m_model->isSessionPinned("s2"));
    QCOMPARE(stateOf(held), JobState::Running);
    QVERIFY(!m_queue->job(held).cancelRequested);

    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(held), JobState::Succeeded);
}

void JobQueueTest::removeSessionWithRunningJob()
{
    QVERIFY(setInput("s1", "G_IN", 4));
    QVERIFY(setInput("s2", "EA_IN", 4));
    QVERIFY(!session("s1").getAttribute("G_OUT").isValid());
    const JobId running = m_queue->offer("s1", QStringLiteral("gated")).job;
    QVERIFY(gate().waitEntered());
    const JobId next = m_queue->offer("s2", QStringLiteral("expA")).job;

    QSignalSpy dependencySpy(m_model.get(), &SessionModel::dependencyChanged);
    QVERIFY(m_model->removeSessions({QStringLiteral("s1")}));

    // Abandoned: asked to stop, so that it does not hold the executor. The gate
    // is never opened.
    QVERIFY(m_queue->job(running).cancelRequested);
    QTRY_COMPARE(stateOf(running), JobState::Superseded);
    QCOMPARE(m_queue->job(running).reason, QString::fromLatin1(kRemoved));
    QVERIFY(!m_queue->job(running).resultStatus.has_value());
    QVERIFY(!spyHasAttribute(dependencySpy, "s1", "G_OUT"));
    QCOMPARE(m_model->getSessionRow("s1"), -1);
    QVERIFY(!m_model->isSessionPinned("s1"));

    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(next), JobState::Succeeded);
    QCOMPARE(session("s2").getAttribute("EA1"), QVariant(5));
    QCOMPARE(m_queue->offer("s1", QStringLiteral("gated")).kind, Kind::SessionNotLoaded);
}

// Deferral: a hidden session with an active job is not unloaded by the LRU.
void JobQueueTest::evictionDeferredWhileJobActive()
{
    QVERIFY(setInput("s1", "G_IN", 4));
    QVERIFY(setInput("s2", "EA_IN", 4));
    QVERIFY(waitForIdle(*m_model));     // clean rows: eviction has nothing to save
    session("s1");
    session("s2");
    session("s3");                      // most recently used: the one a plain LRU would keep

    const JobId running = m_queue->offer("s1", QStringLiteral("gated")).job;
    QVERIFY(gate().waitEntered());
    const JobId queued = m_queue->offer("s2", QStringLiteral("expA")).job;

    const auto restoreCapacity = qScopeGuard([] {
        PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 50);
    });
    PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 1);
    QVERIFY(isLoaded("s1"));            // running job
    QVERIFY(isLoaded("s2"));            // chosen next job
    QVERIFY(!isLoaded("s3"));
    QCoreApplication::processEvents();
    QVERIFY(isLoaded("s1") && isLoaded("s2"));

    QSignalSpy dependencySpy(m_model.get(), &SessionModel::dependencyChanged);
    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(running), JobState::Succeeded);
    QCOMPARE(stateOf(queued), JobState::Succeeded);

    // With the jobs over the cache returns to its capacity
    const auto loadedCount = [this] {
        return int(isLoaded("s1")) + int(isLoaded("s2")) + int(isLoaded("s3"));
    };
    QTRY_COMPARE(loadedCount(), 1);
}

void JobQueueTest::repopulateWithJobs()
{
    QVERIFY(setInput("s1", "G_IN", 4));
    QVERIFY(setInput("s2", "EA_IN", 4));
    const JobId running = m_queue->offer("s1", QStringLiteral("gated")).job;
    QVERIFY(gate().waitEntered());
    const JobId queued = m_queue->offer("s2", QStringLiteral("expA")).job;

    m_model->populateFromUuids({QStringLiteral("u1"), QStringLiteral("u2")});
    QCOMPARE(stateOf(queued), JobState::Superseded);
    QCOMPARE(m_queue->job(queued).reason, QString::fromLatin1(kRemoved));
    QVERIFY(m_queue->job(running).cancelRequested);

    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(running), JobState::Superseded);
    QCOMPARE(m_queue->job(running).reason, QString::fromLatin1(kRemoved));
    QCOMPARE(m_model->rowCount(), 2);
}

// The row stays loaded but its SessionData becomes another object: the old
// engine dies with its ticket (SessionGone), and the reason says what happened
// instead of "removed or unloaded". The application's merge never does this to
// a loaded row (it merges in place: "Inputs changed", below); an assignment
// through sessionRef() does.
void JobQueueTest::sessionDataReplacedWithRunningJobSupersedes()
{
    // Refused at publish: nothing announces the replacement
    QVERIFY(setInput("s1", "G_IN", 4));
    const JobId first = m_queue->offer("s1", QStringLiteral("gated")).job;
    QVERIFY(first != 0);
    QVERIFY(gate().waitEntered());

    {
        // A move-assignment: the row gets the other object's engine
        SessionData replacement = JobWorld::sessions({"s1"}).first();
        session("s1") = std::move(replacement);
    }
    QVERIFY(isLoaded("s1"));
    QSignalSpy dependencySpy(m_model.get(), &SessionModel::dependencyChanged);

    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(first), JobState::Superseded);
    QCOMPARE(m_queue->job(first).reason, QString::fromLatin1(kReplaced));
    QVERIFY(!m_queue->job(first).resultStatus.has_value());
    QCOMPARE(publishedTrace(dependencySpy, "s1", "gated", "G_OUT"), QString());
    QVERIFY(!m_model->isSessionPinned("s1"));

    // Seen coming: the next model signal (an edit of another session) finds
    // the ticket marked, and the job is stopped with the same reason
    QVERIFY(setInput("s1", "G_IN", 6));
    const JobId second = m_queue->offer("s1", QStringLiteral("gated")).job;
    QVERIFY(second != 0);
    QVERIFY(gate().waitEntered());

    {
        // A move-assignment: the row gets the other object's engine
        SessionData replacement = JobWorld::sessions({"s1"}).first();
        session("s1") = std::move(replacement);
    }
    QVERIFY(setInput("s2", "EA_IN", 1));
    QVERIFY(m_queue->job(second).cancelRequested);

    QTRY_COMPARE(stateOf(second), JobState::Superseded);        // the gate is never opened
    QCOMPARE(m_queue->job(second).reason, QString::fromLatin1(kReplaced));
    QVERIFY(waitIdle(*m_queue));
    QVERIFY(!m_model->isSessionPinned("s1"));

    QCOMPARE(endTransitionErrors(m_queue->model()), QString());
}

// A merge that changes a declared input is an input change like any other.
void JobQueueTest::mergeIntoSessionWithRunningJobSupersedes()
{
    // MP_OUT = the first effective sample of IMU/wx, held at the fixture's gate
    Gate *const sharedGate = &gate();
    CalculationDescriptor probe;
    probe.id = QStringLiteral("mergeprobe");
    probe.title = QStringLiteral("Merge probe");
    probe.policy = EvaluationPolicy::Explicit;
    probe.inputs = {CalcInput::measurement(QStringLiteral("IMU"), QStringLiteral("wx"))};
    probe.outputs = {DependencyKey::attribute(QStringLiteral("MP_OUT"))};
    probe.compute = [sharedGate](const EvaluationContext &ctx) {
        const double first = ctx.measurement(QStringLiteral("IMU"), QStringLiteral("wx")).value(0);
        sharedGate->enter(0);
        while (!sharedGate->proceed.tryAcquire(1, 1)) {
            if (ctx.progress().isCancelled()) {
                sharedGate->leave();
                throw CalculationCancelled();
            }
        }
        sharedGate->leave();
        return CalculationResult().setAttribute(QStringLiteral("MP_OUT"), first);
    };
    CalculationRegistry &registry = CalculationRegistry::instance();
    QVERIFY(registry.registerCalculation(probe));
    const auto unregister = qScopeGuard([&registry] {
        registry.unregister(QStringLiteral("mergeprobe"), CalculationRegistry::Removal::Change);
    });
    m_model->flushPendingInvalidations();

    const JobId first = m_queue->offer("s1", QStringLiteral("mergeprobe")).job;
    QVERIFY(first != 0);
    QVERIFY(gate().waitEntered());

    SessionData sensorOnly = DescentFixture::loadSensorOnly("s1");
    sensorOnly.setMeasurement("IMU", "wx", {30.0, 60.0, 90.0});
    m_model->mergeSessions({sensorOnly});
    QSignalSpy dependencySpy(m_model.get(), &SessionModel::dependencyChanged);
    QVERIFY(m_queue->job(first).cancelRequested);       // stale: asked to stop during the merge

    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(first), JobState::Superseded);
    QCOMPARE(m_queue->job(first).reason, QStringLiteral("Inputs changed"));
    QCOMPARE(publishedTrace(dependencySpy, "s1", "mergeprobe", "MP_OUT"), QString());

    // Asked again, it computes from the merged data: 30 x 1.14688 (the fixture
    // declares no SCHEMA_VER, so the gyro is legacy-corrected)
    const JobId second = m_queue->offer("s1", QStringLiteral("mergeprobe")).job;
    QVERIFY(second != 0);
    QVERIFY(gate().waitEntered());
    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(second), JobState::Succeeded);
    QVERIFY(isNear(session("s1").getAttribute("MP_OUT").toDouble(), 34.4064));
}

// Rows move; a job knows its session by id and the ticket travels with the engine.
void JobQueueTest::sortWhileRunningStillPublishes()
{
    QVERIFY(setInput("s1", "G_IN", 4));
    QVERIFY(setInput("s3", "EA_IN", 4));
    const JobId running = m_queue->offer("s1", QStringLiteral("gated")).job;
    QVERIFY(gate().waitEntered());
    const JobId queued = m_queue->offer("s3", QStringLiteral("expA")).job;

    m_model->sort(0, Qt::DescendingOrder);
    m_model->sort(0, Qt::AscendingOrder);
    m_model->sort(0, Qt::DescendingOrder);
    QVERIFY(!m_queue->job(running).cancelRequested);
    QCOMPARE(stateOf(queued), JobState::Queued);

    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(running), JobState::Succeeded);
    QCOMPARE(stateOf(queued), JobState::Succeeded);
    QCOMPARE(session("s1").getAttribute("G_OUT"), QVariant(5));
    QCOMPARE(session("s3").getAttribute("EA1"), QVariant(5));
}

// ---- Shutdown (acceptance 17) -------------------------------------------------------------------

void JobQueueTest::shutdownWithQueuedAndRunning()
{
    QVERIFY(setInput("s1", "G_IN", 4));
    QVERIFY(setInput("s2", "EA_IN", 4));
    QVERIFY(!session("s1").getAttribute("G_OUT").isValid());
    QVERIFY(!session("s2").getAttribute("EA1").isValid());
    QSignalSpy dependencySpy(m_model.get(), &SessionModel::dependencyChanged);
    QSignalSpy idleSpy(m_queue.get(), &JobQueue::idle);

    const JobId running = m_queue->offer("s1", QStringLiteral("gated")).job;
    QVERIFY(gate().waitEntered());
    const JobId queued = m_queue->offer("s2", QStringLiteral("expA")).job;
    QCOMPARE(m_queue->activeJobs(), QList<JobId>({running, queued}));

    // Returns although the gate is never opened: the job is asked to stop and
    // the wait ends when its compute function returns.
    m_queue->shutdown();

    for (const JobId id : {running, queued}) {
        QCOMPARE(stateOf(id), JobState::Cancelled);
        QCOMPARE(m_queue->job(id).reason, QStringLiteral("Application closing"));
    }
    QVERIFY(m_queue->isShutDown());
    QVERIFY(m_queue->isIdle());
    QCOMPARE(m_queue->runningJob(), JobId(0));
    QCOMPARE(idleSpy.count(), 1);
    QCOMPARE(gate().running.load(), 0);

    QCOMPARE(publishedTrace(dependencySpy, "s1", "gated", "G_OUT"), QString());
    QCOMPARE(publishedTrace(dependencySpy, "s2", "expA", "EA1"), QString());

    const JobQueue::OfferResult refused = m_queue->offer("s2", QStringLiteral("expA"));
    QCOMPARE(refused.kind, Kind::ShuttingDown);
    QCOMPARE(refused.job, JobId(0));

    // Late queued events (the worker's finished signal, progress posts) find nothing
    QCoreApplication::processEvents();
    QCOMPARE(stateOf(running), JobState::Cancelled);
    QCOMPARE(m_queue->model()->rowCount(), 2);
}

void JobQueueTest::shutdownIsIdempotentAndRefusesOffers()
{
    QVERIFY(setInput("s1", "EA_IN", 4));
    QSignalSpy idleSpy(m_queue.get(), &JobQueue::idle);

    QVERIFY(!m_queue->isShutDown());
    m_queue->shutdown();                // with no jobs
    m_queue->shutdown();
    QVERIFY(m_queue->isShutDown());
    QVERIFY(m_queue->isIdle());
    QCOMPARE(idleSpy.count(), 0);       // nothing had been active

    QCOMPARE(m_queue->offer("s1", QStringLiteral("expA")).kind, Kind::ShuttingDown);
    QCOMPARE(m_queue->model()->rowCount(), 0);
    QVERIFY(!m_queue->withdrawChosenNext());
    QVERIFY(!m_model->isSessionPinned("s1"));
}

// Slots connected to the executor's own signals may shut it down.
void JobQueueTest::shutdownFromSlots()
{
    QVERIFY(setInput("s1", "EA_IN", 4));
    QVERIFY(setInput("s2", "EA_IN", 4));
    QVERIFY(setInput("s3", "EA_IN", 4));
    QVERIFY(!session("s3").getAttribute("EA1").isValid());

    // From jobFinished: the first job succeeded, the second - the chosen next
    // job, offered once the first had started - never runs
    {
        JobQueue queue(m_model.get());
        JobId first = 0;
        JobId second = 0;
        connect(&queue, &JobQueue::jobStarted, &queue, [&queue, &first, &second](JobId id) {
            if (id == first)
                second = queue.offer("s2", QStringLiteral("expA")).job;
        });
        connect(&queue, &JobQueue::jobFinished, &queue, [&queue] { queue.shutdown(); });
        first = queue.offer("s1", QStringLiteral("expA")).job;
        QVERIFY(waitIdle(queue));
        QVERIFY(second != 0);
        QCOMPARE(queue.job(first).state, JobState::Succeeded);
        QCOMPARE(queue.job(second).state, JobState::Cancelled);
        QCOMPARE(queue.job(second).reason, QStringLiteral("Application closing"));
        QVERIFY(queue.isShutDown());
    }
    QCOMPARE(session("s1").getAttribute("EA1"), QVariant(5));
    QCOMPARE(engine("s2").runCount("expA"), 0);

    // From jobStarted: before the worker exists
    {
        JobQueue queue(m_model.get());
        QSignalSpy dependencySpy(m_model.get(), &SessionModel::dependencyChanged);
        connect(&queue, &JobQueue::jobStarted, &queue, [&queue] { queue.shutdown(); });
        const JobId id = queue.offer("s3", QStringLiteral("expA")).job;
        QVERIFY(waitIdle(queue));
        QCOMPARE(queue.job(id).state, JobState::Cancelled);
        QCOMPARE(queue.job(id).reason, QStringLiteral("Application closing"));
        QCOMPARE(publishedTrace(dependencySpy, "s3", "expA", "EA1"), QString());
    }
    for (const char *id : {"s1", "s2", "s3"})
        QVERIFY(!m_model->isSessionPinned(id));
}

// Quitting with a chosen next job and a running job: the executor goes first
// (the intended order).
void JobQueueTest::queueDestroyedBeforeModel()
{
    QVERIFY(setInput("s1", "G_IN", 4));
    QVERIFY(setInput("s2", "EA_IN", 4));
    QVERIFY(!session("s1").getAttribute("G_OUT").isValid());
    QSignalSpy dependencySpy(m_model.get(), &SessionModel::dependencyChanged);

    QVERIFY(m_queue->offer("s1", QStringLiteral("gated")).created());
    QVERIFY(gate().waitEntered());
    QVERIFY(m_queue->offer("s2", QStringLiteral("expA")).created());

    QCOMPARE(endTransitionErrors(m_queue->model()), QString());
    m_queue.reset();                    // shutdown() inside; the gate is never opened
    QCOMPARE(gate().running.load(), 0);
    QVERIFY(!m_model->isSessionPinned("s1"));
    QVERIFY(!m_model->isSessionPinned("s2"));
    QCOMPARE(publishedTrace(dependencySpy, "s1", "gated", "G_OUT"), QString());
    QCOMPARE(publishedTrace(dependencySpy, "s2", "expA", "EA1"), QString());

    m_model.reset();
    QCoreApplication::processEvents();
}

// The other order is not load-bearing for memory safety: the engines null the
// tickets, and the executor holds the model weakly.
void JobQueueTest::modelDestroyedBeforeQueue()
{
    QVERIFY(setInput("s1", "G_IN", 4));
    QVERIFY(setInput("s2", "EA_IN", 4));
    QVERIFY(setInput("s3", "S_IN", 4));
    const JobId running = m_queue->offer("s1", QStringLiteral("gated")).job;
    QVERIFY(gate().waitEntered());
    const JobId queued = m_queue->offer("s2", QStringLiteral("expA")).job;

    m_model.reset();

    // The compute function returns a complete result for a session that no
    // longer exists
    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(running), JobState::Superseded);
    QCOMPARE(m_queue->job(running).reason, QString::fromLatin1(kRemoved));
    QCOMPARE(stateOf(queued), JobState::Superseded);
    QCOMPARE(m_queue->job(queued).reason, QString::fromLatin1(kRemoved));
    QCOMPARE(m_queue->offer("s3", QStringLiteral("stubborn")).kind, Kind::SessionNotLoaded);

    QCOMPARE(endTransitionErrors(m_queue->model()), QString());
    m_queue.reset();
}

// Acceptance 19 / 20 (executor side): the executor never pauses the idle scheduler.
// An edit made while a job is held inside compute is saved by the idle saver.
void JobQueueTest::idleSchedulerKeepsWorking()
{
    QVERIFY(setInput("s1", "G_IN", 4));
    QVERIFY(waitForIdle(*m_model));
    const JobId running = m_queue->offer("s1", QStringLiteral("gated")).job;
    QVERIFY(gate().waitEntered());

    QVERIFY(m_model->updateAttribute("s2", "_DESCRIPTION", QStringLiteral("edited during a job")));
    QVERIFY(std::as_const(*m_model).rowAt(m_model->getSessionRow("s2")).dirty);
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(!std::as_const(*m_model).rowAt(m_model->getSessionRow("s2")).dirty);
    QCOMPARE(stateOf(running), JobState::Running);      // the gate is still closed

    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(running), JobState::Succeeded);
}

FLYSIGHT_TEST_MAIN(JobQueueTest)
#include "tst_jobqueue.moc"
