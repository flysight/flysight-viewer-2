#include "jobqueue.h"

#include <atomic>

#include <QDateTime>
#include <QMetaObject>
#include <QStringList>
#include <QThread>

#include "engine/calculationengine.h"
#include "engine/calculationregistry.h"
#include "sessiondata.h"
#include "sessionmodel.h"

namespace FlySight {

// ---- The facility and the worker -------------------------------------------

/// What the compute function sees besides its captured inputs. report() and
/// isCancelled() run on the worker; requestCancel() on the main thread.
class JobQueue::JobProgress : public CalculationProgress
{
public:
    JobProgress(JobQueue *queue, JobId jobId) : m_queue(queue), m_jobId(jobId) {}

    void report(const QString &text) override
    {
        // Posted through Qt's thread-safe event queue with the text copied by
        // value: nothing is shared. The queue is the context object and joins
        // the worker before it dies, so the pointer is valid here and events
        // still pending at its destruction are dropped by Qt. Must not throw:
        // if the copy cannot be allocated the text is dropped.
        try {
            JobQueue *queue = m_queue;
            const JobId jobId = m_jobId;
            QMetaObject::invokeMethod(queue, [queue, jobId, text] { queue->onProgress(jobId, text); },
                                      Qt::QueuedConnection);
        } catch (...) {
        }
    }

    bool isCancelled() const override { return m_cancelled.load(std::memory_order_relaxed); }

    void requestCancel() { m_cancelled.store(true, std::memory_order_relaxed); }

private:
    JobQueue *const m_queue;
    const JobId m_jobId;
    // The cancellation request of the facility: written by the main thread,
    // read by the worker. The only atomic of the executor; it guards nothing.
    std::atomic<bool> m_cancelled{false};
};

/// The thread of one running job. Everything is handed over before start()
/// and read back only after wait(): those two are the happens-before edges, so
/// no field is accessed concurrently (the facility's flag aside).
class JobQueue::JobWorker : public QThread
{
public:
    JobWorker(JobQueue *queue, JobId jobId, PreparedCalculation *ticket)
        : m_ticket(ticket), m_progress(queue, jobId)
    {
        // Must precede start(). On Windows the value passes through an
        // unsigned parameter; 64 MiB fits.
        setStackSize(uint(JobQueue::kWorkerStackSize));
    }

    JobProgress &progress() { return m_progress; }
    ComputedCalculation takeResult() { return std::move(m_result); }    // after wait() only

protected:
    void run() override { m_result = m_ticket->compute(&m_progress); }  // never throws

private:
    PreparedCalculation *const m_ticket;    // owned by the queue's Run
    JobProgress m_progress;
    ComputedCalculation m_result;
};

struct JobQueue::Run {
    JobId jobId = 0;
    // Created, published and destroyed on the main thread; only compute()
    // crosses. Never destroyed while the worker may still be inside compute().
    std::unique_ptr<PreparedCalculation> ticket;
    std::unique_ptr<JobWorker> worker;
    // An end decided while the job runs (cancel, a stale ticket, abandonment,
    // shutdown, a failed start). First writer wins; when set, nothing is
    // published.
    std::optional<PendingEnd> pendingEnd;
};

// The implementation has one Run slot (m_run); raising the bound means turning
// it into a list of runs.
static_assert(JobQueue::kMaxRunningJobs == 1, "JobQueue implements exactly one running job");

namespace {

QString blockerTitles(const QList<CalculationBlocker> &blockers)
{
    QStringList titles;
    for (const CalculationBlocker &blocker : blockers)
        titles.append(blocker.title.isEmpty() ? blocker.instanceId : blocker.title);
    return titles.join(QStringLiteral(", "));
}

/// The reason text of a job whose result the engine refuses. The one
/// vocabulary for a refusal, whether it is met at publish or seen coming.
/// SessionGone says only that the engine died. That happens when the session's
/// row is removed or unloaded, and also when the row's SessionData is replaced
/// by another object (a move-assignment brings its own engine). The session
/// model tells the two apart: `sessionStillLoaded` is its answer at the moment
/// the refusal is mapped (a model that is being destroyed has no loaded row).
QString refusalText(PublishOutcome::Reason reason, bool sessionStillLoaded)
{
    switch (reason) {
    case PublishOutcome::Reason::AlreadyPublished:    return JobQueue::tr("Result is already available");
    case PublishOutcome::Reason::RegistrationRemoved: return JobQueue::tr("Calculation is no longer registered");
    case PublishOutcome::Reason::SessionGone:
        return sessionStillLoaded ? JobQueue::tr("Session data replaced")
                                  : JobQueue::tr("Session removed or unloaded");
    default:                                          return JobQueue::tr("Inputs changed");
    }
}

} // namespace

// ---- Construction ---------------------------------------------------------

JobQueue::JobQueue(SessionModel *sessionModel, QObject *parent)
    : QObject(parent)
    , m_sessionModel(sessionModel)
    , m_model(new JobModel(this))
{
    qRegisterMetaType<JobId>("FlySight::JobId");
    qRegisterMetaType<JobState>("FlySight::JobState");

    // A session that disappears takes its jobs with it. Eviction cannot cause
    // this (active jobs pin their session); removal and repopulation do, and
    // SessionModel reports both as a reset.
    if (sessionModel) {
        connect(sessionModel, &QAbstractItemModel::modelReset, this, &JobQueue::onSessionRowsChanged);
        connect(sessionModel, &QAbstractItemModel::rowsRemoved, this, &JobQueue::onSessionRowsChanged);

        // Whatever makes the engine mark the running job's ticket is followed
        // by one of these on the main thread: an edit or a merge announces the
        // changed names, a bulk edit and an environment change repaint rows,
        // and a model that dies has destroyed its engines by then. The ticket
        // is asked again each time (stopRunIfRefused() is a flag read).
        connect(sessionModel, &SessionModel::dependencyChanged, this, &JobQueue::stopRunIfRefused);
        connect(sessionModel, &SessionModel::modelChanged, this, &JobQueue::stopRunIfRefused);
        connect(sessionModel, &QAbstractItemModel::dataChanged, this, &JobQueue::stopRunIfRefused);
        connect(sessionModel, &QObject::destroyed, this, &JobQueue::stopRunIfRefused);
    }

    // A registration that goes away reaches no model signal synchronously. The
    // registry calls its observers after the engines were notified.
    m_registryObserver = CalculationRegistry::instance().addObserver([this] { stopRunIfRefused(); });
}

JobQueue::~JobQueue()
{
    CalculationRegistry::instance().removeObserver(m_registryObserver);
    shutdown();
}

// ---- Offer ------------------------------------------------------------------

JobQueue::OfferResult JobQueue::offer(const QString &sessionId, const CalculationId &plainCalculationId)
{
    CalculationBlocker calculation;
    calculation.registrationId = plainCalculationId;
    calculation.instanceId = plainCalculationId;
    calculation.title = CalculationRegistry::instance().title(plainCalculationId);
    return offer(sessionId, calculation);
}

JobQueue::OfferResult JobQueue::offer(const QString &sessionId, const CalculationBlocker &calculation)
{
    using Kind = OfferResult::Kind;

    if (m_shutDown)
        return {Kind::ShuttingDown, 0};

    // Equal to the chosen next job, or to the running job that was not asked
    // to stop: nothing changes, not even a chosen next job of another key
    if (const JobId existing = activeJob(sessionId, calculation.instanceId))
        return {Kind::AlreadyActive, existing};

    // A plain read of a loaded session: never sessionRef(), which would load.
    // The guard is released before anything is emitted.
    CalculationReadiness readiness;
    QString sessionName;
    bool loaded = false;
    if (m_sessionModel) {
        const SessionModel::RowStabilityGuard guard(*m_sessionModel);
        if (const SessionData *session = m_sessionModel->loadedSession(sessionId)) {
            loaded = true;
            readiness = session->calculationEngine().readiness(calculation.registrationId,
                                                               calculation.instanceOutput);
            sessionName = session->getAttribute(SessionKeys::Description).toString();
        }
    }
    // A refusal changes nothing, the chosen next job included
    if (!loaded)
        return {Kind::SessionNotLoaded, 0};

    switch (readiness.state) {
    case CalculationReadiness::State::Unknown:      return {Kind::UnknownCalculation, 0};
    case CalculationReadiness::State::MissingInput: return {Kind::MissingInput, 0};
    case CalculationReadiness::State::Blocked:      return {Kind::Blocked, 0};
    case CalculationReadiness::State::Done:         return {Kind::NothingToDo, 0};
    case CalculationReadiness::State::Ready:        break;
    }

    // Replace the chosen next job. Its end skips step (6), so that neither
    // idle() nor a start falls between it and the new one. Slots of the end
    // may shut down or offer: validate again after every end.
    while (const JobId previous = chosenNextJob()) {
        endJob(previous, JobState::Cancelled, tr("No longer needed"), std::nullopt, {}, AfterEnd::Nothing);
        if (m_shutDown) {
            announceIdleIfIdle();       // the step (6) that was skipped
            return {Kind::ShuttingDown, 0};
        }
        if (const JobId existing = activeJob(sessionId, calculation.instanceId))
            return {Kind::AlreadyActive, existing};
    }

    JobRecord record;
    record.id = m_nextId++;
    record.sessionId = sessionId;
    record.sessionName = sessionName.isEmpty() ? sessionId : sessionName;
    record.calculationId = calculation.registrationId;
    record.instanceOutput = calculation.instanceOutput;
    record.instanceId = calculation.instanceId;
    record.calculationTitle = calculation.title.isEmpty() ? calculation.instanceId : calculation.title;
    record.state = JobState::Queued;
    record.queuedAt = QDateTime::currentDateTimeUtc();

    // Pin and open the busy period BEFORE the row appears: append() emits
    // rowsInserted, and a slot on it may cancel the new job at once. endJob()
    // must then find the pin it releases and announce idle() for this period.
    m_idleAnnounced = false;
    m_sessionModel->pinSession(sessionId);      // one pin per job; released in endJob()
    const JobId id = m_model->append(record);

    // A rowsInserted slot may have ended the job (cancel, shutdown) and even
    // removed its row. jobFinished was emitted then, and a job is not
    // announced as queued after it has ended.
    const JobRecord appended = m_model->record(id);
    if (appended.id == 0 || appended.isFinished())
        return {Kind::Created, id};

    emit jobQueued(id);
    emit jobsChanged();
    scheduleStart();

    return {Kind::Created, id};
}

bool JobQueue::withdrawChosenNext()
{
    const JobId id = chosenNextJob();
    if (id == 0)
        return false;
    endJob(id, JobState::Cancelled, tr("No longer needed"));
    return true;
}

// ---- Queries ------------------------------------------------------------------

JobId JobQueue::activeJob(const QString &sessionId, const QString &instanceId) const
{
    for (const JobRecord &job : m_model->records()) {
        if (job.isActive() && !job.cancelRequested
            && job.sessionId == sessionId && job.instanceId == instanceId)
            return job.id;
    }
    return 0;
}

QList<JobId> JobQueue::activeJobs() const
{
    QList<JobId> ids;
    if (const JobId running = runningJob())
        ids.append(running);
    if (const JobId next = chosenNextJob())
        ids.append(next);
    return ids;
}

JobId JobQueue::runningJob() const
{
    return m_run ? m_run->jobId : 0;
}

JobId JobQueue::chosenNextJob() const
{
    // Derived from the store: by construction at most one record is Queued
    JobId found = 0;
    for (const JobRecord &job : m_model->records()) {
        if (job.state != JobState::Queued)
            continue;
        Q_ASSERT_X(found == 0, "JobQueue", "more than one chosen next job");
        found = job.id;
    }
    return found;
}

JobRecord JobQueue::job(JobId id) const
{
    return m_model->record(id);
}

bool JobQueue::isIdle() const
{
    return !m_run && chosenNextJob() == 0;
}

bool JobQueue::isSessionLoaded(const QString &sessionId) const
{
    if (!m_sessionModel)
        return false;
    const SessionModel::RowStabilityGuard guard(*m_sessionModel);
    return m_sessionModel->loadedSession(sessionId) != nullptr;
}

// ---- Starting -------------------------------------------------------------------

void JobQueue::scheduleStart()
{
    if (m_startQueued || m_shutDown)
        return;
    m_startQueued = true;
    QMetaObject::invokeMethod(this, &JobQueue::startNext, Qt::QueuedConnection);
}

void JobQueue::startNext()
{
    m_startQueued = false;

    // The running count is the ordering guarantee: the slot is cleared only in
    // finishRun(), after the worker has been joined.
    while (!m_shutDown && runningCount() < kMaxRunningJobs) {
        const JobId id = chosenNextJob();
        if (id == 0)
            return;
        const JobRecord record = m_model->record(id);

        // Inputs are captured now, not at offer(). The guard covers the
        // session pointer only and is gone before anything is emitted.
        bool loaded = false;
        CalculationEngine::PrepareOutcome outcome;
        if (m_sessionModel) {
            const SessionModel::RowStabilityGuard guard(*m_sessionModel);
            if (const SessionData *session = m_sessionModel->loadedSession(record.sessionId)) {
                loaded = true;
                outcome = session->calculationEngine().prepare(record.calculationId, record.instanceOutput);
            }
        }
        // A job that ends before it starts ends this attempt: endJob() queues
        // the next start (the chosen next job that the end's slots put in
        // place), so a disagreement between an offer's readiness and prepare()
        // costs one attempt per event-loop turn at most, never a loop.
        if (!loaded) {
            endJob(id, JobState::Superseded, tr("Session removed or unloaded"));
            return;
        }

        using Kind = CalculationEngine::PrepareOutcome::Kind;
        switch (outcome.kind) {
        case Kind::NotFound:
            endJob(id, JobState::Superseded, tr("Calculation is no longer registered"));
            return;
        case Kind::NotExplicit:
            endJob(id, JobState::Superseded, tr("Calculation is no longer requested explicitly"));
            return;
        case Kind::AlreadyValid:
            endJob(id, JobState::Superseded, tr("Result is already available"));
            return;
        case Kind::NothingToRun:
            // prepare() cached "nothing to run" as readiness() would; the names
            // it dropped are ours to pass on.
            endJob(id, JobState::Superseded, tr("Inputs changed: nothing to compute"),
                   std::nullopt, outcome.invalidated);
            return;
        case Kind::Blocked:
            endJob(id, JobState::Superseded,
                   tr("Inputs changed: waiting for %1").arg(blockerTitles(outcome.blockers)),
                   std::nullopt, outcome.invalidated);
            return;
        case Kind::Ready:
            break;      // `invalidated` is informational: no value changed
        }

        m_run = std::make_unique<Run>();
        m_run->jobId = id;
        m_run->ticket = std::move(outcome.ticket);
        m_run->worker = std::make_unique<JobWorker>(this, id, m_run->ticket.get());
        // Queued explicitly: the signal comes from the worker thread, shortly
        // before it has exited. The id is captured, never the run or worker.
        connect(m_run->worker.get(), &QThread::finished, this,
                [this, id] { onWorkerFinished(id); }, Qt::QueuedConnection);

        m_model->markRunning(id, QDateTime::currentDateTimeUtc());
        emit jobStarted(id);
        emit jobsChanged();

        // A slot may have shut the executor down (the run is consumed) or
        // cancelled the job (an end is pending): then no thread is needed.
        if (!m_run || m_run->jobId != id)
            continue;
        if (m_run->pendingEnd) {
            finishRun();
            continue;
        }

        bool started = false;
        if (m_failWorkerStarts > 0) {
            --m_failWorkerStarts;
        } else {
            // Below normal: the user interface stays responsive and the
            // machine usable while a long calculation runs
            m_run->worker->start(QThread::LowPriority);
            // A thread that could not be created is neither running nor finished
            started = m_run->worker->isRunning() || m_run->worker->isFinished();
        }
        if (!started) {
            // The ticket is destroyed unpublished: nothing is cached and the
            // calculation stays requestable.
            m_run->pendingEnd = PendingEnd{JobState::Failed, tr("The worker thread could not be started")};
            finishRun();
            continue;
        }
    }
}

void JobQueue::onProgress(JobId id, const QString &text)
{
    if (!m_run || m_run->jobId != id)
        return;     // a late post of a job that has ended
    m_model->setProgress(id, text);
    emit jobProgress(id, text);
}

// ---- Ending -----------------------------------------------------------------------

void JobQueue::onWorkerFinished(JobId id)
{
    if (!m_run || m_run->jobId != id)
        return;     // shutdown() already consumed the run
    m_run->worker->wait();      // finished() is emitted slightly before the thread has exited
    finishRun();
}

void JobQueue::finishRun()
{
    // Precondition: the worker never started or has been joined. Taking the
    // run out frees the running slot; nothing is emitted until endJob().
    const std::unique_ptr<Run> run = std::move(m_run);
    Q_ASSERT(run && !run->worker->isRunning());

    JobState state = JobState::Superseded;
    QString reason;
    std::optional<ResultStatus> resultStatus;
    QSet<DependencyKey> invalidated;

    if (run->pendingEnd) {
        // Cancel wins over a completed compute: the ticket goes unpublished
        state = run->pendingEnd->state;
        reason = run->pendingEnd->reason;
    } else {
        // Always published when no end is pending, a Cancelled or
        // ResourceExhausted compute included: the engine reports staleness
        // before it discards, so a superseded job is reported as superseded.
        PublishOutcome outcome = run->ticket->publish(run->worker->takeResult());

        using Kind = PublishOutcome::Kind;
        using Reason = PublishOutcome::Reason;
        switch (outcome.kind) {
        case Kind::Published:
            state = JobState::Succeeded;
            resultStatus = outcome.status;
            invalidated = outcome.invalidated;
            if (outcome.status == ResultStatus::Ok) {
                reason = outcome.detail;    // a rejection's reason; empty for a plain success
            } else {
                QString what = outcome.detail;
                if (what.isEmpty()) {
                    what = outcome.status == ResultStatus::UndeclaredRead ? tr("undeclared read")
                         : outcome.status == ResultStatus::InvalidOutput  ? tr("invalid output")
                                                                          : tr("failed");
                }
                reason = tr("Calculation failed: %1").arg(what);
            }
            break;
        case Kind::RefusedStale:
        case Kind::RefusedGone:
            state = JobState::Superseded;
            reason = refusalText(outcome.reason, isSessionLoaded(m_model->record(run->jobId).sessionId));
            break;
        case Kind::Discarded:
            if (outcome.reason == Reason::ResourceExhausted) {
                state = JobState::Failed;
                reason = tr("Out of memory");
            } else {
                // The compute function threw CalculationCancelled although
                // nobody asked it to stop
                state = JobState::Cancelled;
                reason = tr("Cancelled by the calculation");
            }
            break;
        }
    }

    const JobId id = run->jobId;
    run->ticket.reset();
    run->worker.reset();

    endJob(id, state, reason, resultStatus, invalidated);
}

void JobQueue::endJob(JobId id, JobState state, const QString &reason,
                      std::optional<ResultStatus> resultStatus, const QSet<DependencyKey> &invalidated,
                      AfterEnd afterEnd)
{
    // A copy: a slot may remove the finished row
    const QString sessionId = m_model->record(id).sessionId;

    m_model->markFinished(id, state, reason, resultStatus, QDateTime::currentDateTimeUtc());

    if (m_sessionModel && !invalidated.isEmpty()) {
        m_publishingJob = id;
        m_sessionModel->publishCalculationInvalidation(sessionId, invalidated);
        m_publishingJob = 0;
    }

    emit jobFinished(id, state);
    emit jobsChanged();

    if (m_sessionModel)
        m_sessionModel->unpinSession(sessionId);

    m_model->trimFinished();

    if (afterEnd == AfterEnd::Nothing)
        return;     // a replaced chosen next job: its replacement follows at once
    if (!isIdle())
        scheduleStart();
    else
        announceIdleIfIdle();
}

void JobQueue::announceIdleIfIdle()
{
    if (isIdle() && !m_idleAnnounced) {
        m_idleAnnounced = true;     // once per busy period, whatever slots did before
        emit idle();
    }
}

// ---- Cancellation ---------------------------------------------------------------------

void JobQueue::requestStop(const PendingEnd &end)
{
    if (!m_run)
        return;
    if (!m_run->pendingEnd)
        m_run->pendingEnd = end;        // first writer wins
    m_run->worker->progress().requestCancel();

    const JobId id = m_run->jobId;
    if (!m_model->record(id).cancelRequested) {
        m_model->markCancelRequested(id);
        emit jobCancelRequested(id);
        emit jobsChanged();
    }
}

bool JobQueue::cancel(JobId id)
{
    const JobRecord record = m_model->record(id);
    if (record.id == 0 || record.isFinished())
        return false;

    if (record.state == JobState::Queued) {
        // No longer the chosen next job; the record stays as a finished entry
        endJob(id, JobState::Cancelled, tr("Cancelled"));
        return true;
    }

    if (m_run && m_run->jobId == id)
        requestStop({JobState::Cancelled, tr("Cancelled")});
    return true;
}

// The engine marks a ticket the moment its result can no longer be installed;
// the worker cannot see that and would compute to the end - minutes, for a fit
// - only to be refused. Asked here, on the main thread, whenever something
// happened that can have marked it. The engine still decides: this reads its
// verdict and publishes nothing. The pending end is what publish() would have
// reported, so the job ends Superseded with the same reason, only sooner; and
// from now on it is "a running job that was asked to cancel" for activeJob(),
// so a new offer of the same calculation becomes the chosen next job behind it. A staleness no
// signal announces, and a compute function that ignores the request, end
// Superseded at publish as before.
void JobQueue::stopRunIfRefused()
{
    // An end is pending: the job was asked to stop already, and first writer wins
    if (!m_run || m_run->pendingEnd || !m_run->ticket->willBeRefused())
        return;

    const bool stillLoaded = isSessionLoaded(m_model->record(m_run->jobId).sessionId);
    requestStop({JobState::Superseded, refusalText(m_run->ticket->refusalReason(), stillLoaded)});
}

void JobQueue::onSessionRowsChanged()
{
    // A chosen next job whose session is gone ends now; nothing promised to load it
    QList<JobId> orphaned;
    for (const JobRecord &job : m_model->records()) {
        if (job.state == JobState::Queued && !isSessionLoaded(job.sessionId))
            orphaned.append(job.id);
    }
    for (const JobId id : std::as_const(orphaned)) {
        if (m_model->record(id).state == JobState::Queued)
            endJob(id, JobState::Superseded, tr("Session removed or unloaded"));
    }

    // The running job is abandoned, so that a long fit for a deleted session
    // does not hold the executor. What happened is still "superseded".
    if (m_run && !isSessionLoaded(m_model->record(m_run->jobId).sessionId))
        requestStop({JobState::Superseded, tr("Session removed or unloaded")});

    // A reset that replaced the session's contents (a merge) marked the ticket
    stopRunIfRefused();
}

void JobQueue::shutdown()
{
    if (m_shutDown)
        return;
    m_shutDown = true;

    QList<JobId> queued;
    for (const JobRecord &job : m_model->records()) {
        if (job.state == JobState::Queued)
            queued.append(job.id);
    }
    for (const JobId id : std::as_const(queued)) {
        if (m_model->record(id).state == JobState::Queued)
            endJob(id, JobState::Cancelled, tr("Application closing"));
    }

    if (m_run) {
        requestStop({JobState::Cancelled, tr("Application closing")});
        // No timeout: the worker may be inside a solver step, and everything it
        // touches must outlive it. The wait ends as soon as compute() returns.
        if (m_run) {
            m_run->worker->wait();
            finishRun();
        }
    }
}

} // namespace FlySight
