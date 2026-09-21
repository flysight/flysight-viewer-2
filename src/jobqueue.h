#ifndef JOBQUEUE_H
#define JOBQUEUE_H

#include <functional>
#include <memory>
#include <optional>

#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>

#include "engine/blockerreport.h"
#include "jobmodel.h"

namespace FlySight {

class SessionModel;

/// The application-wide queue of background calculation work. A job is one
/// explicit calculation for one session; jobs run one at a time, first come
/// first served, on the application's only worker thread.
///
/// LIFECYCLE. Every job ends in exactly one end state:
///
///   request() --> Queued --> Running --> Succeeded   the result was published (a rejection or
///                   |           |                    a failed result included: see resultStatus)
///                   |           +------> Cancelled   cancel() / shutdown(); nothing published
///                   |           +------> Superseded  the engine refused the result, or the
///                   |           |                    session went away; nothing published
///                   |           +------> Failed      worker could not start / out of memory;
///                   |                                nothing published, nothing cached
///                   +--> Cancelled | Superseded      ended before it ever ran (startedAt invalid)
///
/// A job runs with the engine's three steps: CalculationEngine::prepare() when
/// it STARTS (inputs are captured then, not at request()),
/// PreparedCalculation::compute() on the worker, PreparedCalculation::publish()
/// back here. The engine, not the queue, decides staleness. The queue never
/// re-requests on its own: whoever still wants a result sees it missing and
/// asks again.
///
/// THREADING. Everything here is main thread, including every signal. One
/// worker thread (64 MiB stack) exists per running job, from its start until it
/// has been joined; it executes compute() and nothing else, and sees the
/// ticket's captured inputs and the progress/cancel facility only. Two things
/// cross threads: progress text, posted to the main thread as a queued
/// invocation, and the cancel request, the one atomic flag of the facility.
/// There is no lock.
///
/// The queue NEVER LOADS A SESSION: it looks sessions up with
/// SessionModel::loadedSession() only, and a job whose session is not loaded
/// cannot be created or started. Every active job pins its session
/// (SessionModel::pinSession) so that the LRU does not unload it; removing the
/// session, merging into it, or repopulating the model is not prevented and
/// ends the job Superseded. The queue never touches the idle scheduler: saves,
/// loads and column work continue while a job runs.
///
/// JobModel is the store of the job records and the single source of truth
/// about work in progress; the queue keeps no job list of its own.
///
/// ORDER OF A JOB'S END. (1) the model's end transition (one dataChanged);
/// (2) for a publication, SessionModel::publishCalculationInvalidation() -
/// dependencyChanged listeners already see the job finished; (3) jobFinished,
/// jobsChanged; (4) the session is unpinned; (5) the model trims its finished
/// rows; (6) idle(), or the next job is scheduled. The next job never starts
/// synchronously: always from the event loop.
///
/// Slots connected to the queue's signals may call request(), cancel*() and
/// shutdown().
///
/// PRECONDITION of request(), cancel(), cancelSession(), cancelAll(),
/// cancelUnwantedQueued() and shutdown(): main thread; not from inside a
/// calculation or an engine callback (they inspect or publish to engines).
class JobQueue : public QObject
{
    Q_OBJECT
public:
    /// A factor-graph solver's elimination trees overflow default thread stacks,
    /// so every job gets this much, whichever calculation it runs. Reserved
    /// address space, committed lazily, and only while a job runs.
    static constexpr qsizetype kWorkerStackSize = 64 * 1024 * 1024;

    /// `sessionModel` is held weakly: the queue may outlive it (every job then
    /// ends Superseded), although shutdown() should be called first.
    explicit JobQueue(SessionModel *sessionModel, QObject *parent = nullptr);
    ~JobQueue() override;       ///< calls shutdown()

    JobModel *model() const { return m_model; }

    struct RequestResult {
        enum class Kind {
            Created,            ///< a new job was queued (`job`)
            AlreadyActive,      ///< an equal job is queued or running (`job`); nothing was added
            SessionNotLoaded,   ///< no such session, or it is not loaded; the queue does not load sessions
            UnknownCalculation, ///< no such calculation / the name is not of that family
            MissingInput,       ///< a declared input is unavailable: there is nothing to compute
            Blocked,            ///< other explicit calculations must be requested first (engine.readiness())
            NothingToDo,        ///< already computed (a cached rejection or failure included), or not explicit
            ShuttingDown        ///< shutdown() was called
        };
        Kind  kind = Kind::SessionNotLoaded;
        JobId job = 0;          ///< non-zero for Created and AlreadyActive only
        bool created() const { return kind == Kind::Created; }
    };
    /// Queues one explicit calculation for one loaded session, unless an equal
    /// job is active (see activeJob()) or the engine's readiness() says there
    /// is nothing to run. Never prepares and never starts anything
    /// synchronously: the job is Queued when this returns.
    RequestResult request(const QString &sessionId, const CalculationBlocker &calculation);
    /// A plain (non-family) calculation by id.
    RequestResult request(const QString &sessionId, const CalculationId &plainCalculationId);

    // ---- queries -------------------------------------------------------------
    /// The job a request for (sessionId, instanceId) would be deduplicated
    /// against: the queued or running job with that key, EXCEPT a running job
    /// that has been asked to cancel - that one is winding down, and a new
    /// request creates a new job behind it. 0 if none.
    JobId        activeJob(const QString &sessionId, const QString &instanceId) const;
    QList<JobId> activeJobs() const;    ///< request order; the running job, if any, is first
    JobId        runningJob() const;    ///< 0 if none
    JobRecord    job(JobId id) const;   ///< == model()->record(id)
    bool         isIdle() const;        ///< nothing queued, nothing running
    bool         isShutDown() const { return m_shutDown; }

    // ---- cancellation ----------------------------------------------------------
    /// A queued job ends Cancelled at once. The running job is asked to stop
    /// and ends Cancelled when its compute function returns - even if that
    /// returned a complete result: once cancellation was requested nothing is
    /// published. The next job does not start before then. False: unknown or
    /// already finished.
    bool cancel(JobId id);
    /// cancel() for every active job of the session / of the queue. Returns the
    /// number of jobs newly cancelled or asked to stop (a running job that had
    /// been asked before is not counted again).
    int  cancelSession(const QString &sessionId);
    int  cancelAll();
    /// Ends every QUEUED job for which `isWanted` returns false as Cancelled
    /// ("No longer needed"). The running job is never offered to the predicate:
    /// its result is valid and worth keeping. Returns the number cancelled.
    int  cancelUnwantedQueued(const std::function<bool(const JobRecord &)> &isWanted);
    /// Cancels everything ("Application closing") and waits - without a
    /// timeout - until the worker has returned; afterwards no worker and no
    /// ticket exists and request() returns ShuttingDown. The wait is bounded by
    /// the compute function's cancellation boundaries; abandoning a live thread
    /// instead would be a crash. Idempotent.
    void shutdown();

    /// Test seam: the next `count` worker starts are treated as failed (thread
    /// creation failure cannot be provoked portably).
    void failNextWorkerStarts(int count) { m_failWorkerStarts = count; }

signals:
    void jobQueued(FlySight::JobId id);
    void jobStarted(FlySight::JobId id);
    void jobProgress(FlySight::JobId id, const QString &text);
    void jobCancelRequested(FlySight::JobId id);    ///< the running job was asked to stop
    void jobFinished(FlySight::JobId id, FlySight::JobState state);
    void jobsChanged();                             ///< after every one of the above except jobProgress
    void idle();                                    ///< the last active job ended

private:
    class JobProgress;      // the CalculationProgress handed to compute()
    class JobWorker;        // the thread of one running job
    struct PendingEnd {
        JobState state;
        QString reason;
    };
    struct Run;             // the running job: ticket, worker, pending end

    void scheduleStart();
    void startNext();
    void onWorkerFinished(JobId id);
    void finishRun();
    void endJob(JobId id, JobState state, const QString &reason,
                std::optional<ResultStatus> resultStatus = std::nullopt,
                const QSet<DependencyKey> &invalidated = {});
    void requestStop(const PendingEnd &end);
    void onProgress(JobId id, const QString &text);
    void onSessionRowsChanged();
    bool isSessionLoaded(const QString &sessionId) const;
    JobId oldestQueued() const;

    QPointer<SessionModel> m_sessionModel;
    JobModel *m_model;                  // child object
    JobId m_nextId = 1;
    bool m_shutDown = false;
    bool m_startQueued = false;         // a queued startNext() is pending
    bool m_idleAnnounced = true;        // idle() was emitted since the last job was created
    int m_failWorkerStarts = 0;
    std::unique_ptr<Run> m_run;         // non-null from a job's start until its worker was joined
};

} // namespace FlySight

#endif // JOBQUEUE_H
