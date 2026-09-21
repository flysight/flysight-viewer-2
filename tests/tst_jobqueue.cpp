// The job queue on a real SessionModel, real SessionData engines, and the
// global registry, with the synthetic explicit calculations of jobfixture.h.
// Sensor-fusion-jobs acceptance 8 (queue half), 10, 11 (queue half), 14, 17.
//
// Synchronization: Gate::waitEntered() proves the worker is inside a compute
// function; QTRY_* and waitIdle() spin the event loop for main-thread effects.
// There are no sleeps.

#include <atomic>
#include <memory>

#include <QHash>
#include <QSignalSpy>
#include <QThread>
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

using Kind = JobQueue::RequestResult::Kind;

Q_DECLARE_METATYPE(FlySight::DependencyKey)

namespace {

const char kRemoved[] = "Session removed or unloaded";
const char kReplaced[] = "Session data replaced";

// Written by the probe's compute function on the worker, read after the job ended
std::atomic<quintptr> g_probeThread{0};
std::atomic<uint> g_probeStackSize{0};

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
    void duplicateRequestsCreateNoDuplicates();
    void oneAtATimeInRequestOrder();
    void refusesMissingInput();
    void refusesUnloadedAndUnknownSession();
    void refusesBlockedAndDone();
    void neverLoadsASession();
    void inputChangeWhileRunningSupersedes();
    void inputChangeWhileQueuedSupersedesAtStart();
    void registrationRemovedSupersedes();
    void staleRunningJobIsStoppedAtOnce();
    void requestWhileStaleJobWindsDown();
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
    void requestWhileCancellingCreatesNewJob();
    void cancelQueued();
    void cancelFromRowsInsertedLeavesNoPin();
    void cancelSessionAndCancelAll();
    void cancelUnwantedQueuedSparesRunning();
    void removeSessionWithQueuedJob();
    void removeSessionWithRunningJob();
    void evictionDeferredWhileJobActive();
    void repopulateWithJobs();
    void mergeIntoSessionWithRunningJobSupersedes();
    void sessionDataReplacedWithRunningJobSupersedes();
    void sortWhileRunningStillPublishes();
    void shutdownWithQueuedAndRunning();
    void shutdownIsIdempotentAndRefusesRequests();
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
    void setInput(const QString &sessionId, const char *key, int value)
    {
        QVERIFY(m_model->updateAttribute(sessionId, QString::fromLatin1(key), value));
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
    void verifyEndTransitions(JobModel *model);

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
    LogbookColumn description;
    description.type = ColumnType::SessionAttribute;
    description.attributeKey = QString::fromLatin1(SessionKeys::Description);
    LogbookColumnStore::instance().setColumns({description});

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

void JobQueueTest::cleanup()
{
    if (m_queue) {
        // Let nothing linger inside a compute function
        m_queue->shutdown();
        verifyEndTransitions(m_queue->model());
    }
    if (m_model) {
        for (const char *id : {"s1", "s2", "s3"})
            QVERIFY2(!m_model->isSessionPinned(id), id);
    }

    // Queue, then model, then the registrations (a live model reacts to
    // registry changes)
    m_queue.reset();
    m_model.reset();
    m_world.reset();
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

void JobQueueTest::verifyEndTransitions(JobModel *model)
{
    for (int row = 0; row < model->rowCount(); ++row) {
        const JobRecord job = model->record(row);
        QCOMPARE(m_endTransitions.value(job.id), job.isFinished() ? 1 : 0);
    }
}

// ---- Running and publishing ---------------------------------------------------------

void JobQueueTest::runsAndPublishes()
{
    setInput("s1", "EA_IN", 4);
    QVERIFY(m_model->updateAttribute("s1", "_DESCRIPTION", QStringLiteral("First jump")));
    m_model->removeAttribute("s2", "_DESCRIPTION");
    setInput("s2", "EA_IN", 4);

    QSignalSpy queuedSpy(m_queue.get(), &JobQueue::jobQueued);
    QSignalSpy startedSpy(m_queue.get(), &JobQueue::jobStarted);
    QSignalSpy finishedSpy(m_queue.get(), &JobQueue::jobFinished);
    QSignalSpy idleSpy(m_queue.get(), &JobQueue::idle);

    const JobQueue::RequestResult result = m_queue->request("s1", QStringLiteral("expA"));
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
    QCOMPARE(m_queue->activeJobs(), QList<JobId>({1}));
    QCOMPARE(m_queue->activeJob("s1", "expA"), JobId(1));
    QVERIFY(m_model->isSessionPinned("s1"));
    QCOMPARE(queuedSpy.count(), 1);
    QCOMPARE(startedSpy.count(), 0);

    QVERIFY(waitIdle(*m_queue));
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
    const JobQueue::RequestResult second = m_queue->request("s2", QStringLiteral("expA"));
    QCOMPARE(second.kind, Kind::Created);
    QCOMPARE(second.job, JobId(2));
    QCOMPARE(m_queue->job(second.job).sessionName, QStringLiteral("s2"));
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(second.job), JobState::Succeeded);
    QCOMPARE(idleSpy.count(), 2);
}

// Names read while the calculation was unrequested are re-announced through
// the session model, after the model says the job finished and before
// jobFinished.
void JobQueueTest::publishesInvalidationsThroughSessionModel()
{
    setInput("s1", "EA_IN", 4);
    QVERIFY(!session("s1").getAttribute("DA").isValid());       // read while unrequested
    QVERIFY(!session("s1").getAttribute("EA1").isValid());

    QSignalSpy dependencySpy(m_model.get(), &SessionModel::dependencyChanged);
    QSignalSpy modelSpy(m_model.get(), &SessionModel::modelChanged);
    QSignalSpy finishedSpy(m_queue.get(), &JobQueue::jobFinished);

    QList<JobState> stateWhenAnnounced;
    QList<int> finishedSignalsWhenAnnounced;
    connect(m_model.get(), &SessionModel::dependencyChanged, this,
            [&](const QString &, const DependencyKey &) {
        stateWhenAnnounced.append(m_queue->job(1).state);
        finishedSignalsWhenAnnounced.append(int(finishedSpy.count()));
    });

    QCOMPARE(m_queue->request("s1", QStringLiteral("expA")).kind, Kind::Created);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(1), JobState::Succeeded);

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
    const auto unregister = qScopeGuard([&registry] { registry.unregister(QStringLiteral("threadprobe")); });
    m_model->flushPendingInvalidations();

    g_probeThread.store(0);
    g_probeStackSize.store(0);
    setInput("s1", "P_IN", 4);
    QCOMPARE(m_queue->request("s1", QStringLiteral("threadprobe")).kind, Kind::Created);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(1), JobState::Succeeded);
    QCOMPARE(session("s1").getAttribute("P_OUT"), QVariant(5));

    QVERIFY(g_probeThread.load() != 0);
    QVERIFY(g_probeThread.load() != quintptr(QCoreApplication::instance()->thread()));
    QCOMPARE(g_probeStackSize.load(), uint(64 * 1024 * 1024));
    QCOMPARE(JobQueue::kWorkerStackSize, qsizetype(67108864));

    // 16 MiB of locals would overflow a default thread stack
    setInput("s1", "D_IN", 4);
    QCOMPARE(m_queue->request("s1", QStringLiteral("deepstack")).kind, Kind::Created);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(2), JobState::Succeeded);
    QCOMPARE(m_queue->job(2).resultStatus, std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(session("s1").getAttribute("D_OUT"), QVariant(5));
}

// Acceptance 14
void JobQueueTest::duplicateRequestsCreateNoDuplicates()
{
    setInput("s1", "G_IN", 4);
    setInput("s2", "G_IN", 6);
    setInput("s1", "EA_IN", 4);

    const JobQueue::RequestResult first = m_queue->request("s1", QStringLiteral("gated"));
    QCOMPARE(first.kind, Kind::Created);

    // While queued
    JobQueue::RequestResult again = m_queue->request("s1", QStringLiteral("gated"));
    QCOMPARE(again.kind, Kind::AlreadyActive);
    QCOMPARE(again.job, first.job);
    QVERIFY(!again.created());
    QCOMPARE(m_queue->model()->rowCount(), 1);

    // While running
    QVERIFY(gate().waitEntered());
    QCOMPARE(stateOf(first.job), JobState::Running);
    again = m_queue->request("s1", QStringLiteral("gated"));
    QCOMPARE(again.kind, Kind::AlreadyActive);
    QCOMPARE(again.job, first.job);
    QCOMPARE(m_queue->model()->rowCount(), 1);
    QVERIFY(m_model->isSessionPinned("s1"));

    // Another session, or another calculation, is another job
    QCOMPARE(m_queue->request("s2", QStringLiteral("gated")).kind, Kind::Created);
    QCOMPARE(m_queue->request("s1", QStringLiteral("expA")).kind, Kind::Created);
    QCOMPARE(m_queue->request("s2", QStringLiteral("gated")).kind, Kind::AlreadyActive);
    QCOMPARE(m_queue->model()->rowCount(), 3);

    gate().open(2);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(engine("s1").runCount("gated"), 1);
    QCOMPARE(engine("s2").runCount("gated"), 1);
    QCOMPARE(session("s1").getAttribute("G_OUT"), QVariant(5));
    QCOMPARE(session("s2").getAttribute("G_OUT"), QVariant(7));
    QCOMPARE(gate().maxRunning.load(), 1);
}

// Acceptance 14: five jobs over three sessions, one at a time, first come
// first served.
void JobQueueTest::oneAtATimeInRequestOrder()
{
    setInput("s1", "G_IN", 1);
    setInput("s2", "G_IN", 2);
    setInput("s3", "G_IN", 3);
    setInput("s1", "S_IN", 4);
    setInput("s2", "S_IN", 5);

    QList<JobId> ids;
    ids.append(m_queue->request("s1", QStringLiteral("gated")).job);
    ids.append(m_queue->request("s2", QStringLiteral("gated")).job);
    ids.append(m_queue->request("s1", QStringLiteral("stubborn")).job);
    ids.append(m_queue->request("s3", QStringLiteral("gated")).job);
    ids.append(m_queue->request("s2", QStringLiteral("stubborn")).job);
    QCOMPARE(ids, QList<JobId>({1, 2, 3, 4, 5}));
    QCOMPARE(m_queue->activeJobs(), ids);

    for (int i = 0; i < ids.size(); ++i) {
        QVERIFY(gate().waitEntered());
        QCOMPARE(gate().running.load(), 1);
        QCOMPARE(m_queue->runningJob(), ids.at(i));
        QCOMPARE(m_queue->activeJobs(), ids.mid(i));
        for (int j = 0; j < ids.size(); ++j) {
            const JobState expected = j < i ? JobState::Succeeded
                                    : j == i ? JobState::Running : JobState::Queued;
            QCOMPARE(stateOf(ids.at(j)), expected);
        }
        gate().open(1);
        QTRY_COMPARE(stateOf(ids.at(i)), JobState::Succeeded);
    }
    QVERIFY(waitIdle(*m_queue));

    QCOMPARE(gate().maxRunning.load(), 1);
    QCOMPARE(gate().startOrder(), QList<int>({1, 2, 4, 3, 5}));
    for (int i = 1; i < ids.size(); ++i)
        QVERIFY(m_queue->job(ids.at(i)).startedAt >= m_queue->job(ids.at(i - 1)).finishedAt);
    QCOMPARE(session("s3").getAttribute("G_OUT"), QVariant(4));
    QCOMPARE(session("s2").getAttribute("S_OUT"), QVariant(6));
}

// ---- Refusals ---------------------------------------------------------------------------

// Acceptance 11 (queue half): no job can be created for a session without the inputs.
void JobQueueTest::refusesMissingInput()
{
    QSignalSpy queuedSpy(m_queue.get(), &JobQueue::jobQueued);
    for (const char *id : {"gated", "expA", "deepstack"}) {
        const JobQueue::RequestResult result = m_queue->request("s1", QString::fromLatin1(id));
        QCOMPARE(result.kind, Kind::MissingInput);
        QCOMPARE(result.job, JobId(0));
    }
    QCOMPARE(m_queue->model()->rowCount(), 0);
    QCOMPARE(queuedSpy.count(), 0);
    QVERIFY(m_queue->isIdle());
    QVERIFY(!m_model->isSessionPinned("s1"));
    QCOMPARE(engine("s1").totalRunCount(), 0);

    // Control: with the input the same request is accepted
    setInput("s1", "EA_IN", 4);
    QCOMPARE(m_queue->request("s1", QStringLiteral("expA")).kind, Kind::Created);
    QVERIFY(waitIdle(*m_queue));
}

void JobQueueTest::refusesUnloadedAndUnknownSession()
{
    QCOMPARE(m_queue->request("no-such-session", QStringLiteral("expA")).kind, Kind::SessionNotLoaded);
    QCOMPARE(m_queue->request(QString(), QStringLiteral("expA")).kind, Kind::SessionNotLoaded);

    // s1 becomes a stub: room for one, and s3 was used last
    setInput("s1", "EA_IN", 4);
    QVERIFY(waitForIdle(*m_model));
    session("s3");
    const auto restoreCapacity = qScopeGuard([] {
        PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 50);
    });
    PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 1);
    QVERIFY(!isLoaded("s1"));

    QSignalSpy loadedSpy(m_model.get(), &SessionModel::sessionLoaded);
    const JobQueue::RequestResult result = m_queue->request("s1", QStringLiteral("expA"));
    QCOMPARE(result.kind, Kind::SessionNotLoaded);
    QCOMPARE(result.job, JobId(0));
    QVERIFY(!isLoaded("s1"));
    QCOMPARE(loadedSpy.count(), 0);
    QCOMPARE(m_queue->model()->rowCount(), 0);
}

void JobQueueTest::refusesBlockedAndDone()
{
    setInput("s1", "EA_IN", 4);
    setInput("s1", "EB_IN", 10);
    setInput("s2", "EA_IN", -1);

    QCOMPARE(m_queue->request("s1", QStringLiteral("no-such-calculation")).kind, Kind::UnknownCalculation);

    // B consumes an output of A, which has not been requested
    QCOMPARE(m_queue->request("s1", QStringLiteral("expB")).kind, Kind::Blocked);
    QCOMPARE(m_queue->model()->rowCount(), 0);

    QCOMPARE(m_queue->request("s1", QStringLiteral("expA")).kind, Kind::Created);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(m_queue->request("s1", QStringLiteral("expA")).kind, Kind::NothingToDo);     // published
    QCOMPARE(m_queue->request("s1", QStringLiteral("derivA")).kind, Kind::NothingToDo);   // on demand
    QCOMPARE(m_queue->model()->rowCount(), 1);

    // Now B can run. The blocker form of request() is the one chains use.
    const BlockerReport report = engine("s1").blockers(DependencyKey::attribute(QStringLiteral("DB")));
    QCOMPARE(report.state, BlockerReport::State::Blocked);
    QCOMPARE(report.blockers.size(), 1);
    const JobQueue::RequestResult b = m_queue->request("s1", report.blockers.first());
    QCOMPARE(b.kind, Kind::Created);
    QCOMPARE(m_queue->job(b.job).calculationTitle, QStringLiteral("Explicit B"));
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(session("s1").getAttribute("EB1"), QVariant(18));
    QCOMPARE(session("s1").getAttribute("DB"), QVariant(19));

    // A cached rejection is done too: the same inputs give the same answer
    QCOMPARE(m_queue->request("s2", QStringLiteral("expA")).kind, Kind::Created);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(m_queue->request("s2", QStringLiteral("expA")).kind, Kind::NothingToDo);
    QCOMPARE(engine("s2").runCount("expA"), 1);
}

void JobQueueTest::neverLoadsASession()
{
    setInput("s1", "G_IN", 4);
    setInput("s2", "EA_IN", 4);
    setInput("s3", "EA_IN", 4);
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
    QCOMPARE(m_queue->request("s3", QStringLiteral("expA")).kind, Kind::SessionNotLoaded);
    QCOMPARE(m_queue->request("s1", QStringLiteral("gated")).kind, Kind::Created);
    QCOMPARE(m_queue->request("s2", QStringLiteral("expA")).kind, Kind::Created);
    QVERIFY(gate().waitEntered());
    QCOMPARE(m_queue->activeJob("s3", "expA"), JobId(0));
    m_queue->cancelSession("s3");
    m_queue->cancelUnwantedQueued([](const JobRecord &) { return true; });
    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(m_queue->request("s3", QStringLiteral("expA")).kind, Kind::SessionNotLoaded);
    m_queue->cancelAll();
    m_queue->shutdown();

    QCOMPARE(loadedSpy.count(), 0);
    QVERIFY(!isLoaded("s3"));
}

// ---- Superseded ---------------------------------------------------------------------------

// Acceptance 8 (queue half)
void JobQueueTest::inputChangeWhileRunningSupersedes()
{
    setInput("s1", "G_IN", 4);
    QVERIFY(!session("s1").getAttribute("G_OUT").isValid());    // read while unrequested

    const JobId first = m_queue->request("s1", QStringLiteral("gated")).job;
    QVERIFY(gate().waitEntered());

    // A declared input changes under the running job. (The edit itself
    // announces G_OUT, whose cached "unavailable" depended on G_IN; what must
    // not happen is an announcement when the job ends.)
    setInput("s1", "G_IN", 7);
    QSignalSpy dependencySpy(m_model.get(), &SessionModel::dependencyChanged);
    QVERIFY(!session("s1").getAttribute("G_OUT").isValid());

    gate().open(1);
    QTRY_COMPARE(stateOf(first), JobState::Superseded);
    QCOMPARE(m_queue->job(first).reason, QStringLiteral("Inputs changed"));
    QVERIFY(!m_queue->job(first).resultStatus.has_value());
    QVERIFY(m_queue->job(first).startedAt.isValid());
    QCOMPARE(publishedTrace(dependencySpy, "s1", "gated", "G_OUT"), QString());
    QVERIFY(waitIdle(*m_queue));        // the queue does not re-request on its own
    QCOMPARE(m_queue->model()->rowCount(), 1);

    // Still requestable, and a new request computes the new value
    QCOMPARE(engine("s1").readiness("gated").state, CalculationReadiness::State::Ready);
    const JobQueue::RequestResult second = m_queue->request("s1", QStringLiteral("gated"));
    QCOMPARE(second.kind, Kind::Created);
    QVERIFY(gate().waitEntered());
    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(second.job), JobState::Succeeded);
    QCOMPARE(session("s1").getAttribute("G_OUT"), QVariant(8));
    QVERIFY(spyHasAttribute(dependencySpy, "s1", "G_OUT"));
}

// Inputs are captured when a job starts. A job whose inputs went away while it
// was queued ends without a worker, and the queue moves on.
void JobQueueTest::inputChangeWhileQueuedSupersedesAtStart()
{
    setInput("s1", "G_IN", 4);
    setInput("s2", "EA_IN", 4);
    setInput("s3", "EA_IN", 4);
    setInput("s3", "EB_IN", 10);

    // Preparation: A is published in s3, so that B is ready there
    QCOMPARE(m_queue->request("s3", QStringLiteral("expA")).kind, Kind::Created);
    QVERIFY(waitIdle(*m_queue));

    QSignalSpy startedSpy(m_queue.get(), &JobQueue::jobStarted);
    const JobId held = m_queue->request("s1", QStringLiteral("gated")).job;
    QVERIFY(gate().waitEntered());

    const JobId missing = m_queue->request("s2", QStringLiteral("expA")).job;
    const JobId blocked = m_queue->request("s3", QStringLiteral("expB")).job;
    const JobId valid = m_queue->request("s2", QStringLiteral("expA")).job;     // same job: deduplicated
    QCOMPARE(valid, missing);
    setInput("s1", "EA_IN", 4);
    const JobId alreadyValid = m_queue->request("s1", QStringLiteral("expA")).job;
    setInput("s1", "T_IN", 4);
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("synthetic failure")));
    const JobId last = m_queue->request("s1", QStringLiteral("thrower")).job;
    QVERIFY(missing != 0 && blocked != 0 && alreadyValid != 0 && last != 0);

    // While they wait: s2 loses the input; s3's A result is dropped by an
    // input change, which blocks B; s1's A is computed synchronously.
    QVERIFY(m_model->removeAttribute("s2", "EA_IN"));
    setInput("s3", "EA_IN", 5);
    QCOMPARE(engine("s1").request("expA").status, ResultStatus::Ok);

    gate().open(1);
    QVERIFY(waitIdle(*m_queue));

    QCOMPARE(stateOf(held), JobState::Succeeded);
    QCOMPARE(stateOf(missing), JobState::Superseded);
    QCOMPARE(m_queue->job(missing).reason, QStringLiteral("Inputs changed: nothing to compute"));
    QCOMPARE(stateOf(blocked), JobState::Superseded);
    QCOMPARE(m_queue->job(blocked).reason, QStringLiteral("Inputs changed: waiting for Explicit A"));
    QCOMPARE(stateOf(alreadyValid), JobState::Superseded);
    QCOMPARE(m_queue->job(alreadyValid).reason, QStringLiteral("Result is already available"));
    for (const JobId id : {missing, blocked, alreadyValid}) {
        QVERIFY(!m_queue->job(id).startedAt.isValid());
        QVERIFY(m_queue->job(id).finishedAt.isValid());
    }

    // No worker for them; the queue went on to the last job
    QList<JobId> started;
    for (const QList<QVariant> &args : startedSpy)
        started.append(args.at(0).toULongLong());
    QCOMPARE(started, QList<JobId>({held, last}));
    QCOMPARE(stateOf(last), JobState::Succeeded);
    QVERIFY(!session("s3").getAttribute("EB1").isValid());
}

void JobQueueTest::registrationRemovedSupersedes()
{
    CalculationRegistry &registry = CalculationRegistry::instance();
    setInput("s1", "G_IN", 4);
    setInput("s2", "T_IN", 4);
    setInput("s3", "EA_IN", 4);
    QVERIFY(!session("s1").getAttribute("G_OUT").isValid());

    const JobId running = m_queue->request("s1", QStringLiteral("gated")).job;
    const JobId queued = m_queue->request("s2", QStringLiteral("thrower")).job;
    const JobId next = m_queue->request("s3", QStringLiteral("expA")).job;
    QVERIFY(gate().waitEntered());

    // Both go away; the fixture gets them back at the end so that its own
    // clean-up stays balanced.
    const CalculationDescriptor gatedDescriptor = *registry.instance(QStringLiteral("gated"))->descriptor;
    const CalculationDescriptor throwerDescriptor = *registry.instance(QStringLiteral("thrower"))->descriptor;
    QVERIFY(registry.unregister(QStringLiteral("gated")));
    QVERIFY(registry.unregister(QStringLiteral("thrower")));
    m_model->flushPendingInvalidations();
    QSignalSpy dependencySpy(m_model.get(), &SessionModel::dependencyChanged);

    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(running), JobState::Superseded);
    QCOMPARE(m_queue->job(running).reason, QStringLiteral("Calculation is no longer registered"));
    QCOMPARE(stateOf(queued), JobState::Superseded);
    QCOMPARE(m_queue->job(queued).reason, QStringLiteral("Calculation is no longer registered"));
    QVERIFY(!m_queue->job(queued).startedAt.isValid());
    QCOMPARE(stateOf(next), JobState::Succeeded);
    QVERIFY(!spyHasAttribute(dependencySpy, "s1", "G_OUT"));
    QVERIFY(!session("s1").getAttribute("G_OUT").isValid());
    QCOMPARE(m_queue->request("s1", QStringLiteral("gated")).kind, Kind::UnknownCalculation);

    QVERIFY(registry.registerCalculation(gatedDescriptor));
    QVERIFY(registry.registerCalculation(throwerDescriptor));
    m_model->flushPendingInvalidations();
}

// ---- Stale while running: stopped early, ended Superseded ---------------------------------------

// Acceptance 8 (queue half), sooner: the engine marked the ticket when the
// input changed, and the queue asks the compute function to stop there and
// then. The gate is never opened, so the run cannot have reached its end.
void JobQueueTest::staleRunningJobIsStoppedAtOnce()
{
    setInput("s1", "G_IN", 4);
    setInput("s2", "EA_IN", 4);
    QVERIFY(!session("s1").getAttribute("G_OUT").isValid());
    QSignalSpy cancelSpy(m_queue.get(), &JobQueue::jobCancelRequested);
    QSignalSpy progressSpy(m_queue.get(), &JobQueue::jobProgress);

    const JobId first = m_queue->request("s1", QStringLiteral("gated")).job;
    const JobId next = m_queue->request("s2", QStringLiteral("expA")).job;
    QVERIFY(gate().waitEntered());
    QCOMPARE(m_queue->activeJob("s1", "gated"), first);

    // An edit of another session, or of a name the job does not depend on,
    // stops nothing
    setInput("s2", "G_IN", 1);
    setInput("s1", "S_IN", 1);
    QVERIFY(!m_queue->job(first).cancelRequested);
    QCOMPARE(cancelSpy.count(), 0);

    setInput("s1", "G_IN", 7);
    QSignalSpy dependencySpy(m_model.get(), &SessionModel::dependencyChanged);

    // Synchronously with the edit: asked to stop, still running, no longer
    // what a new request is deduplicated against
    QCOMPARE(cancelSpy.count(), 1);
    QCOMPARE(cancelSpy.at(0).at(0).toULongLong(), first);
    QVERIFY(m_queue->job(first).cancelRequested);
    QCOMPARE(stateOf(first), JobState::Running);
    QCOMPARE(m_queue->runningJob(), first);
    QCOMPARE(m_queue->activeJob("s1", "gated"), JobId(0));
    QCOMPARE(stateOf(next), JobState::Queued);

    // A second change asks nothing twice
    setInput("s1", "G_IN", 8);
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

    // The worker is free for the next job, and nothing is re-requested
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(next), JobState::Succeeded);
    QVERIFY(m_queue->job(next).startedAt >= m_queue->job(first).finishedAt);
    QCOMPARE(m_queue->model()->rowCount(), 2);
}

// A refresh pressed while the stale job is still winding down is a new job,
// which runs after the old worker has returned, with the new inputs.
void JobQueueTest::requestWhileStaleJobWindsDown()
{
    setInput("s1", "G_IN", 4);
    const JobId first = m_queue->request("s1", QStringLiteral("gated")).job;
    QVERIFY(gate().waitEntered());
    QCOMPARE(m_queue->request("s1", QStringLiteral("gated")).kind, Kind::AlreadyActive);

    setInput("s1", "G_IN", 7);
    QCOMPARE(stateOf(first), JobState::Running);        // still winding down

    const JobQueue::RequestResult second = m_queue->request("s1", QStringLiteral("gated"));
    QCOMPARE(second.kind, Kind::Created);
    QVERIFY(second.job != first);
    QCOMPARE(stateOf(second.job), JobState::Queued);
    QCOMPARE(m_queue->activeJob("s1", "gated"), second.job);
    QCOMPARE(m_queue->request("s1", QStringLiteral("gated")).kind, Kind::AlreadyActive);
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
    setInput("s1", "S_IN", 4);
    QVERIFY(!session("s1").getAttribute("S_OUT").isValid());

    const JobId id = m_queue->request("s1", QStringLiteral("stubborn")).job;
    QVERIFY(gate().waitEntered());
    setInput("s1", "S_IN", 7);
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
    setInput("s1", "G_IN", 4);
    const JobId running = m_queue->request("s1", QStringLiteral("gated")).job;
    QVERIFY(gate().waitEntered());

    // An unrelated registration change stops nothing
    const CalculationDescriptor throwerDescriptor = *registry.instance(QStringLiteral("thrower"))->descriptor;
    QVERIFY(registry.unregister(QStringLiteral("thrower")));
    QVERIFY(!m_queue->job(running).cancelRequested);

    const CalculationDescriptor gatedDescriptor = *registry.instance(QStringLiteral("gated"))->descriptor;
    QVERIFY(registry.unregister(QStringLiteral("gated")));
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
    setInput("s1", "G_IN", 4);
    const JobId running = m_queue->request("s1", QStringLiteral("gated")).job;
    QVERIFY(gate().waitEntered());

    m_model.reset();
    QVERIFY(m_queue->job(running).cancelRequested);

    QTRY_COMPARE(stateOf(running), JobState::Superseded);       // the gate is never opened
    QCOMPARE(m_queue->job(running).reason, QString::fromLatin1(kRemoved));
    QVERIFY(waitIdle(*m_queue));

    verifyEndTransitions(m_queue->model());
    m_queue.reset();
}

// The pending end is decided once: the first writer wins.
void JobQueueTest::userCancelThenStaleEndsCancelled()
{
    setInput("s1", "G_IN", 4);
    QSignalSpy cancelSpy(m_queue.get(), &JobQueue::jobCancelRequested);
    const JobId id = m_queue->request("s1", QStringLiteral("gated")).job;
    QVERIFY(gate().waitEntered());

    QVERIFY(m_queue->cancel(id));
    setInput("s1", "G_IN", 7);
    QCOMPARE(cancelSpy.count(), 1);

    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(id), JobState::Cancelled);
    QCOMPARE(m_queue->job(id).reason, QStringLiteral("Cancelled"));
    QVERIFY(!session("s1").getAttribute("G_OUT").isValid());
}

void JobQueueTest::staleThenUserCancelEndsSuperseded()
{
    setInput("s1", "G_IN", 4);
    QSignalSpy cancelSpy(m_queue.get(), &JobQueue::jobCancelRequested);
    const JobId id = m_queue->request("s1", QStringLiteral("gated")).job;
    QVERIFY(gate().waitEntered());

    setInput("s1", "G_IN", 7);
    QVERIFY(m_queue->cancel(id));           // accepted: the job is still active
    QCOMPARE(m_queue->cancelAll(), 0);      // but it had been asked to stop before
    QCOMPARE(cancelSpy.count(), 1);

    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(id), JobState::Superseded);
    QCOMPARE(m_queue->job(id).reason, QStringLiteral("Inputs changed"));
    QVERIFY(!session("s1").getAttribute("G_OUT").isValid());
}

// ---- Results that are functions of the inputs, and failures that are not ----------------------

void JobQueueTest::rejectionSucceedsWithReason()
{
    setInput("s1", "EA_IN", -1);
    const JobId id = m_queue->request("s1", QStringLiteral("expA")).job;
    QVERIFY(waitIdle(*m_queue));

    const JobRecord job = m_queue->job(id);
    QCOMPARE(job.state, JobState::Succeeded);
    QCOMPARE(job.resultStatus, std::optional<ResultStatus>(ResultStatus::Ok));
    QCOMPARE(job.reason, QStringLiteral("negative input"));
    QVERIFY(!session("s1").getAttribute("EA1").isValid());
    QCOMPARE(session("s1").getAttribute("EA_DIAG"), QVariant(QStringLiteral("rejected")));

    QCOMPARE(m_queue->request("s1", QStringLiteral("expA")).kind, Kind::NothingToDo);
    QCOMPARE(engine("s1").runCount("expA"), 1);
}

void JobQueueTest::exceptionSucceedsAsFailedResult()
{
    setInput("s1", "T_IN", 4);
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("synthetic failure")));
    const JobId id = m_queue->request("s1", QStringLiteral("thrower")).job;
    QVERIFY(waitIdle(*m_queue));

    // Published and cached as a failed result, and the record says so
    const JobRecord job = m_queue->job(id);
    QCOMPARE(job.state, JobState::Succeeded);
    QCOMPARE(job.resultStatus, std::optional<ResultStatus>(ResultStatus::Failed));
    QCOMPARE(job.reason, QStringLiteral("Calculation failed: synthetic failure"));
    QCOMPARE(engine("s1").resultStatus("thrower"), std::optional<ResultStatus>(ResultStatus::Failed));
    QVERIFY(!session("s1").getAttribute("T_OUT").isValid());

    QCOMPARE(m_queue->request("s1", QStringLiteral("thrower")).kind, Kind::NothingToDo);
    QCOMPARE(m_queue->model()->rowCount(), 1);
}

void JobQueueTest::resourceExhaustionFails()
{
    setInput("s1", "X_IN", 4);
    QVERIFY(!session("s1").getAttribute("X_OUT").isValid());
    QSignalSpy dependencySpy(m_model.get(), &SessionModel::dependencyChanged);

    const JobId first = m_queue->request("s1", QStringLiteral("exhausted")).job;
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(first), JobState::Failed);
    QCOMPARE(m_queue->job(first).reason, QStringLiteral("Out of memory"));
    QVERIFY(!m_queue->job(first).resultStatus.has_value());

    // Nothing published, nothing cached: it can be requested again
    QCOMPARE(publishedTrace(dependencySpy, "s1", "exhausted", "X_OUT"), QString());
    QCOMPARE(engine("s1").readiness("exhausted").state, CalculationReadiness::State::Ready);

    const JobQueue::RequestResult second = m_queue->request("s1", QStringLiteral("exhausted"));
    QCOMPARE(second.kind, Kind::Created);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(second.job), JobState::Succeeded);
    QCOMPARE(session("s1").getAttribute("X_OUT"), QVariant(5));
}

void JobQueueTest::workerStartFailureFails()
{
    setInput("s1", "EA_IN", 4);
    setInput("s2", "EA_IN", 4);
    QVERIFY(!session("s1").getAttribute("EA1").isValid());
    QSignalSpy dependencySpy(m_model.get(), &SessionModel::dependencyChanged);

    m_queue->failNextWorkerStarts(1);
    const JobId first = m_queue->request("s1", QStringLiteral("expA")).job;
    const JobId second = m_queue->request("s2", QStringLiteral("expA")).job;
    QVERIFY(waitIdle(*m_queue));

    QCOMPARE(stateOf(first), JobState::Failed);
    QCOMPARE(m_queue->job(first).reason, QStringLiteral("The worker thread could not be started"));
    QCOMPARE(publishedTrace(dependencySpy, "s1", "expA", "EA1"), QString());
    QCOMPARE(engine("s1").runCount("expA"), 0);

    // The next job ran, and the failed one is requestable
    QCOMPARE(stateOf(second), JobState::Succeeded);
    QCOMPARE(session("s2").getAttribute("EA1"), QVariant(5));
    const JobQueue::RequestResult retry = m_queue->request("s1", QStringLiteral("expA"));
    QCOMPARE(retry.kind, Kind::Created);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(retry.job), JobState::Succeeded);
    QCOMPARE(session("s1").getAttribute("EA1"), QVariant(5));
}

// ---- Cancellation ----------------------------------------------------------------------------

// Acceptance 10
void JobQueueTest::cancelRunningThenNextStarts()
{
    setInput("s1", "G_IN", 4);
    setInput("s2", "G_IN", 6);
    QVERIFY(!session("s1").getAttribute("G_OUT").isValid());
    QSignalSpy dependencySpy(m_model.get(), &SessionModel::dependencyChanged);
    QSignalSpy cancelSpy(m_queue.get(), &JobQueue::jobCancelRequested);

    const JobId first = m_queue->request("s1", QStringLiteral("gated")).job;
    const JobId second = m_queue->request("s2", QStringLiteral("gated")).job;
    QVERIFY(gate().waitEntered());

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

    // Then the next queued job starts, and succeeds
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
    setInput("s1", "S_IN", 4);
    QVERIFY(!session("s1").getAttribute("S_OUT").isValid());
    QSignalSpy dependencySpy(m_model.get(), &SessionModel::dependencyChanged);

    const JobId id = m_queue->request("s1", QStringLiteral("stubborn")).job;
    QVERIFY(gate().waitEntered());
    QVERIFY(m_queue->cancel(id));
    QVERIFY(waitIdle(*m_queue));

    QCOMPARE(stateOf(id), JobState::Cancelled);
    QCOMPARE(m_queue->job(id).reason, QStringLiteral("Cancelled"));
    QCOMPARE(publishedTrace(dependencySpy, "s1", "stubborn", "S_OUT"), QString());
    QCOMPARE(engine("s1").runCount("stubborn"), 0);
    QCOMPARE(engine("s1").readiness("stubborn").state, CalculationReadiness::State::Ready);
}

// A refresh pressed right after a cancel must not be lost.
void JobQueueTest::requestWhileCancellingCreatesNewJob()
{
    setInput("s1", "G_IN", 4);
    const JobId first = m_queue->request("s1", QStringLiteral("gated")).job;
    QVERIFY(gate().waitEntered());
    QVERIFY(m_queue->cancel(first));
    QCOMPARE(stateOf(first), JobState::Running);        // still winding down
    QCOMPARE(m_queue->activeJob("s1", "gated"), JobId(0));

    const JobQueue::RequestResult second = m_queue->request("s1", QStringLiteral("gated"));
    QCOMPARE(second.kind, Kind::Created);
    QVERIFY(second.job != first);
    QCOMPARE(stateOf(second.job), JobState::Queued);
    QCOMPARE(m_queue->activeJob("s1", "gated"), second.job);
    QCOMPARE(m_queue->request("s1", QStringLiteral("gated")).kind, Kind::AlreadyActive);
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
    setInput("s1", "G_IN", 4);
    setInput("s2", "EA_IN", 4);
    QSignalSpy startedSpy(m_queue.get(), &JobQueue::jobStarted);
    QSignalSpy finishedSpy(m_queue.get(), &JobQueue::jobFinished);

    const JobId held = m_queue->request("s1", QStringLiteral("gated")).job;
    const JobId queued = m_queue->request("s2", QStringLiteral("expA")).job;
    QVERIFY(gate().waitEntered());

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
    QCOMPARE(m_queue->request("s2", QStringLiteral("expA")).kind, Kind::Created);
    QVERIFY(waitIdle(*m_queue));
}

// A view of the job model may cancel a job the moment its row appears. The
// session is pinned before the row is announced, so the cancel releases that
// pin: no "session is not pinned" warning and no pin left behind.
void JobQueueTest::cancelFromRowsInsertedLeavesNoPin()
{
    setInput("s1", "EA_IN", 4);
    QTest::failOnWarning(QRegularExpression(QStringLiteral("unpinSession")));
    QSignalSpy idleSpy(m_queue.get(), &JobQueue::idle);
    QSignalSpy queuedSpy(m_queue.get(), &JobQueue::jobQueued);
    QSignalSpy finishedSpy(m_queue.get(), &JobQueue::jobFinished);

    JobModel *jobs = m_queue->model();
    bool pinnedWhenAnnounced = false;
    bool cancelled = false;
    const QMetaObject::Connection connection = connect(
        jobs, &QAbstractItemModel::rowsInserted, this, [&](const QModelIndex &, int first, int) {
            pinnedWhenAnnounced = m_model->isSessionPinned("s1");
            cancelled = m_queue->cancel(jobs->record(first).id);
        });

    const JobQueue::RequestResult result = m_queue->request("s1", QStringLiteral("expA"));
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
    QCOMPARE(m_queue->request("s1", QStringLiteral("expA")).kind, Kind::Created);
    QVERIFY(waitIdle(*m_queue));
    QVERIFY(!m_model->isSessionPinned("s1"));
}

void JobQueueTest::cancelSessionAndCancelAll()
{
    setInput("s1", "G_IN", 4);
    setInput("s1", "EA_IN", 4);
    setInput("s2", "EA_IN", 4);
    setInput("s3", "EA_IN", 4);

    const JobId running = m_queue->request("s1", QStringLiteral("gated")).job;
    const JobId queuedSame = m_queue->request("s1", QStringLiteral("expA")).job;
    const JobId queued2 = m_queue->request("s2", QStringLiteral("expA")).job;
    const JobId queued3 = m_queue->request("s3", QStringLiteral("expA")).job;
    QVERIFY(gate().waitEntered());

    QCOMPARE(m_queue->cancelSession("no-such-session"), 0);
    QCOMPARE(m_queue->cancelSession("s1"), 2);
    QCOMPARE(stateOf(queuedSame), JobState::Cancelled);
    QVERIFY(m_queue->job(running).cancelRequested);
    QCOMPARE(stateOf(queued2), JobState::Queued);

    // The running job was asked already: it is not counted twice
    QCOMPARE(m_queue->cancelAll(), 2);
    QCOMPARE(stateOf(queued2), JobState::Cancelled);
    QCOMPARE(stateOf(queued3), JobState::Cancelled);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(running), JobState::Cancelled);
    QCOMPARE(m_queue->cancelAll(), 0);
    QCOMPARE(engine("s2").totalRunCount(), 0);
}

// Spec 9.5 (queue side): queued jobs nobody wants go; the running job stays.
void JobQueueTest::cancelUnwantedQueuedSparesRunning()
{
    setInput("s1", "G_IN", 4);
    setInput("s2", "G_IN", 6);
    setInput("s2", "EA_IN", 4);
    setInput("s3", "EA_IN", 4);

    const JobId running = m_queue->request("s1", QStringLiteral("gated")).job;
    const JobId unwantedA = m_queue->request("s2", QStringLiteral("expA")).job;
    const JobId wanted = m_queue->request("s3", QStringLiteral("expA")).job;
    const JobId unwantedB = m_queue->request("s2", QStringLiteral("gated")).job;
    QVERIFY(gate().waitEntered());

    QList<JobId> offered;
    const int cancelled = m_queue->cancelUnwantedQueued([&offered](const JobRecord &job) {
        offered.append(job.id);
        return job.sessionId == QLatin1String("s3");    // would reject the running job too
    });
    QCOMPARE(cancelled, 2);
    QCOMPARE(offered, QList<JobId>({unwantedA, wanted, unwantedB}));
    for (const JobId id : {unwantedA, unwantedB}) {
        QCOMPARE(stateOf(id), JobState::Cancelled);
        QCOMPARE(m_queue->job(id).reason, QStringLiteral("No longer needed"));
    }
    QCOMPARE(stateOf(running), JobState::Running);
    QVERIFY(!m_queue->job(running).cancelRequested);
    QCOMPARE(stateOf(wanted), JobState::Queued);

    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(running), JobState::Succeeded);
    QCOMPARE(stateOf(wanted), JobState::Succeeded);
    QCOMPARE(session("s1").getAttribute("G_OUT"), QVariant(5));
    QCOMPARE(gate().startOrder(), QList<int>({4}));
}

// ---- Sessions that go away or change (acceptance 17) ------------------------------------------

void JobQueueTest::removeSessionWithQueuedJob()
{
    setInput("s1", "G_IN", 4);
    setInput("s2", "EA_IN", 4);
    const JobId held = m_queue->request("s1", QStringLiteral("gated")).job;
    const JobId queued = m_queue->request("s2", QStringLiteral("expA")).job;
    QVERIFY(gate().waitEntered());

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
    setInput("s1", "G_IN", 4);
    setInput("s2", "EA_IN", 4);
    QVERIFY(!session("s1").getAttribute("G_OUT").isValid());
    const JobId running = m_queue->request("s1", QStringLiteral("gated")).job;
    const JobId next = m_queue->request("s2", QStringLiteral("expA")).job;
    QVERIFY(gate().waitEntered());

    QSignalSpy dependencySpy(m_model.get(), &SessionModel::dependencyChanged);
    QVERIFY(m_model->removeSessions({QStringLiteral("s1")}));

    // Abandoned: asked to stop, so that it does not hold the queue. The gate
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
    QCOMPARE(m_queue->request("s1", QStringLiteral("gated")).kind, Kind::SessionNotLoaded);
}

// Deferral: a hidden session with an active job is not unloaded by the LRU.
void JobQueueTest::evictionDeferredWhileJobActive()
{
    setInput("s1", "G_IN", 4);
    setInput("s2", "EA_IN", 4);
    QVERIFY(waitForIdle(*m_model));     // clean rows: eviction has nothing to save
    session("s1");
    session("s2");
    session("s3");                      // most recently used: the one a plain LRU would keep

    const JobId running = m_queue->request("s1", QStringLiteral("gated")).job;
    const JobId queued = m_queue->request("s2", QStringLiteral("expA")).job;
    QVERIFY(gate().waitEntered());

    const auto restoreCapacity = qScopeGuard([] {
        PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 50);
    });
    PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 1);
    QVERIFY(isLoaded("s1"));            // running job
    QVERIFY(isLoaded("s2"));            // queued job
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
    setInput("s1", "G_IN", 4);
    setInput("s2", "EA_IN", 4);
    const JobId running = m_queue->request("s1", QStringLiteral("gated")).job;
    const JobId queued = m_queue->request("s2", QStringLiteral("expA")).job;
    QVERIFY(gate().waitEntered());

    m_model->populateFromUuids({QStringLiteral("u1"), QStringLiteral("u2")});
    QCOMPARE(stateOf(queued), JobState::Superseded);
    QCOMPARE(m_queue->job(queued).reason, QString::fromLatin1(kRemoved));
    QVERIFY(m_queue->job(running).cancelRequested);

    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(running), JobState::Superseded);
    QCOMPARE(m_queue->job(running).reason, QString::fromLatin1(kRemoved));
    QCOMPARE(m_model->rowCount(), 2);
}

// A merge that changes a declared input is an input change like any other.
// The row stays loaded but its SessionData becomes another object: the old
// engine dies with its ticket (SessionGone), and the reason says what happened
// instead of "removed or unloaded". The application's merge never does this to
// a loaded row (it merges in place: "Inputs changed", below); an assignment
// through sessionRef() does.
void JobQueueTest::sessionDataReplacedWithRunningJobSupersedes()
{
    // Refused at publish: nothing announces the replacement
    setInput("s1", "G_IN", 4);
    const JobId first = m_queue->request("s1", QStringLiteral("gated")).job;
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
    setInput("s1", "G_IN", 6);
    const JobId second = m_queue->request("s1", QStringLiteral("gated")).job;
    QVERIFY(second != 0);
    QVERIFY(gate().waitEntered());

    {
        // A move-assignment: the row gets the other object's engine
        SessionData replacement = JobWorld::sessions({"s1"}).first();
        session("s1") = std::move(replacement);
    }
    setInput("s2", "EA_IN", 1);
    QVERIFY(m_queue->job(second).cancelRequested);

    QTRY_COMPARE(stateOf(second), JobState::Superseded);        // the gate is never opened
    QCOMPARE(m_queue->job(second).reason, QString::fromLatin1(kReplaced));
    QVERIFY(waitIdle(*m_queue));
    QVERIFY(!m_model->isSessionPinned("s1"));

    verifyEndTransitions(m_queue->model());
}

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
    const auto unregister = qScopeGuard([&registry] { registry.unregister(QStringLiteral("mergeprobe")); });
    m_model->flushPendingInvalidations();

    const JobId first = m_queue->request("s1", QStringLiteral("mergeprobe")).job;
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
    const JobId second = m_queue->request("s1", QStringLiteral("mergeprobe")).job;
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
    setInput("s1", "G_IN", 4);
    setInput("s3", "EA_IN", 4);
    const JobId running = m_queue->request("s1", QStringLiteral("gated")).job;
    const JobId queued = m_queue->request("s3", QStringLiteral("expA")).job;
    QVERIFY(gate().waitEntered());

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
    setInput("s1", "G_IN", 4);
    setInput("s2", "EA_IN", 4);
    setInput("s3", "EA_IN", 4);
    QVERIFY(!session("s1").getAttribute("G_OUT").isValid());
    QVERIFY(!session("s2").getAttribute("EA1").isValid());
    QVERIFY(!session("s3").getAttribute("EA1").isValid());
    QSignalSpy dependencySpy(m_model.get(), &SessionModel::dependencyChanged);
    QSignalSpy idleSpy(m_queue.get(), &JobQueue::idle);

    const JobId running = m_queue->request("s1", QStringLiteral("gated")).job;
    const JobId queued2 = m_queue->request("s2", QStringLiteral("expA")).job;
    const JobId queued3 = m_queue->request("s3", QStringLiteral("expA")).job;
    QVERIFY(gate().waitEntered());

    // Returns although the gate is never opened: the job is asked to stop and
    // the wait ends when its compute function returns.
    m_queue->shutdown();

    for (const JobId id : {running, queued2, queued3}) {
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
    QCOMPARE(publishedTrace(dependencySpy, "s3", "expA", "EA1"), QString());

    const JobQueue::RequestResult refused = m_queue->request("s2", QStringLiteral("expA"));
    QCOMPARE(refused.kind, Kind::ShuttingDown);
    QCOMPARE(refused.job, JobId(0));

    // Late queued events (the worker's finished signal, progress posts) find nothing
    QCoreApplication::processEvents();
    QCOMPARE(stateOf(running), JobState::Cancelled);
    QCOMPARE(m_queue->model()->rowCount(), 3);
}

void JobQueueTest::shutdownIsIdempotentAndRefusesRequests()
{
    setInput("s1", "EA_IN", 4);
    QSignalSpy idleSpy(m_queue.get(), &JobQueue::idle);

    QVERIFY(!m_queue->isShutDown());
    m_queue->shutdown();                // with no jobs
    m_queue->shutdown();
    QVERIFY(m_queue->isShutDown());
    QVERIFY(m_queue->isIdle());
    QCOMPARE(idleSpy.count(), 0);       // nothing had been active

    QCOMPARE(m_queue->request("s1", QStringLiteral("expA")).kind, Kind::ShuttingDown);
    QCOMPARE(m_queue->model()->rowCount(), 0);
    QCOMPARE(m_queue->cancelAll(), 0);
    QVERIFY(!m_model->isSessionPinned("s1"));
}

// Slots connected to the queue's own signals may shut it down.
void JobQueueTest::shutdownFromSlots()
{
    setInput("s1", "EA_IN", 4);
    setInput("s2", "EA_IN", 4);
    setInput("s3", "EA_IN", 4);
    QVERIFY(!session("s3").getAttribute("EA1").isValid());

    // From jobFinished: the first job succeeded, the second never runs
    {
        JobQueue queue(m_model.get());
        connect(&queue, &JobQueue::jobFinished, &queue, [&queue] { queue.shutdown(); });
        const JobId first = queue.request("s1", QStringLiteral("expA")).job;
        const JobId second = queue.request("s2", QStringLiteral("expA")).job;
        QVERIFY(waitIdle(queue));
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
        const JobId id = queue.request("s3", QStringLiteral("expA")).job;
        QVERIFY(waitIdle(queue));
        QCOMPARE(queue.job(id).state, JobState::Cancelled);
        QCOMPARE(queue.job(id).reason, QStringLiteral("Application closing"));
        QCOMPARE(publishedTrace(dependencySpy, "s3", "expA", "EA1"), QString());
    }
    for (const char *id : {"s1", "s2", "s3"})
        QVERIFY(!m_model->isSessionPinned(id));
}

// Quitting with jobs queued and running: the queue goes first (the intended order).
void JobQueueTest::queueDestroyedBeforeModel()
{
    setInput("s1", "G_IN", 4);
    setInput("s2", "EA_IN", 4);
    QVERIFY(!session("s1").getAttribute("G_OUT").isValid());
    QSignalSpy dependencySpy(m_model.get(), &SessionModel::dependencyChanged);

    QVERIFY(m_queue->request("s1", QStringLiteral("gated")).created());
    QVERIFY(m_queue->request("s2", QStringLiteral("expA")).created());
    QVERIFY(gate().waitEntered());

    verifyEndTransitions(m_queue->model());
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
// tickets, and the queue holds the model weakly.
void JobQueueTest::modelDestroyedBeforeQueue()
{
    setInput("s1", "G_IN", 4);
    setInput("s2", "EA_IN", 4);
    setInput("s3", "S_IN", 4);
    const JobId running = m_queue->request("s1", QStringLiteral("gated")).job;
    const JobId queued = m_queue->request("s2", QStringLiteral("expA")).job;
    QVERIFY(gate().waitEntered());

    m_model.reset();

    // The compute function returns a complete result for a session that no
    // longer exists
    gate().open(1);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(stateOf(running), JobState::Superseded);
    QCOMPARE(m_queue->job(running).reason, QString::fromLatin1(kRemoved));
    QCOMPARE(stateOf(queued), JobState::Superseded);
    QCOMPARE(m_queue->job(queued).reason, QString::fromLatin1(kRemoved));
    QCOMPARE(m_queue->request("s3", QStringLiteral("stubborn")).kind, Kind::SessionNotLoaded);

    verifyEndTransitions(m_queue->model());
    m_queue.reset();
}

// Acceptance 19 / 20 (queue side): the queue never pauses the idle scheduler.
// An edit made while a job is held inside compute is saved by the idle saver.
void JobQueueTest::idleSchedulerKeepsWorking()
{
    setInput("s1", "G_IN", 4);
    QVERIFY(waitForIdle(*m_model));
    const JobId running = m_queue->request("s1", QStringLiteral("gated")).job;
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
