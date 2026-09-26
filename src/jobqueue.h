#ifndef JOBQUEUE_H
#define JOBQUEUE_H

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

/// The executor of requested calculations. It holds at most the running job
/// and the one job its caller has chosen to run next. It keeps no order of
/// arrival and no list; the caller decides what runs next. Jobs run one at a
/// time (kMaxRunningJobs) on the application's only worker thread, which runs
/// at below-normal priority. A job is one explicit calculation for one session.
///
/// LIFECYCLE. Every job ends in exactly one end state:
///
///   offer() --> Queued --> Running --> Succeeded   the result was published (a rejection or
///    (the chosen   |           |                    a failed result included: see resultStatus)
///     next job)    |           +------> Cancelled   cancel() / shutdown(); nothing published
///                  |           +------> Superseded  the engine refused the result (or was
///                  |           |                    certain to: see STALE WHILE RUNNING), or
///                  |           |                    the session went away; nothing published
///                  |           +------> Failed      worker could not start / out of memory;
///                  |                                nothing published, nothing cached
///                  +--> Cancelled | Superseded      ended before it ever ran (startedAt invalid)
///
/// A Queued job ends Cancelled when it is replaced by another offer, withdrawn
/// (withdrawChosenNext()), cancel()ed or shut down, and Superseded when it goes
/// stale at start or its session goes.
///
/// A job runs with the engine's three steps: CalculationEngine::prepare() when
/// it STARTS (inputs are captured then, not at offer()),
/// PreparedCalculation::compute() on the worker, PreparedCalculation::publish()
/// back here. The engine, not the executor, decides staleness. The executor
/// never offers on its own: whoever still wants a result sees it missing and
/// offers it again.
///
/// THREADING. Everything here is main thread, including every signal. One
/// worker thread (64 MiB stack) exists per running job, from its start until it
/// has been joined; it executes compute() and nothing else, and sees the
/// ticket's captured inputs and the progress/cancel facility only. Two things
/// cross threads: progress text, posted to the main thread as a queued
/// invocation, and the cancel request, the one atomic flag of the facility.
/// There is no lock.
///
/// STALE WHILE RUNNING. The engine marks the running job's ticket the moment
/// its result can no longer be installed: a declared input changed, the
/// session's data was replaced or removed, the registration went away, the
/// caches were cleared. The executor asks the ticket
/// (PreparedCalculation::willBeRefused()) on the main-thread signals that follow
/// such a change - SessionModel::dependencyChanged / modelChanged / dataChanged
/// / modelReset / rowsRemoved / destroyed and the registry's observer call;
/// there is no timer - and then asks the compute function to stop through the
/// cancel flag. The job's end is recorded as what publish() would have
/// reported: Superseded, with the same reason ("Inputs changed", ...). From
/// that moment the job is a running job that was asked to cancel
/// (jobCancelRequested is emitted, activeJob() no longer returns it), so
/// whoever wants the result sees it missing, and a new offer becomes the
/// chosen next job behind it; the new job starts only after the old worker has
/// been joined. Nothing is published or cached for the stopped job. A compute
/// function that ignores the request, or a staleness that none of those
/// signals announces, still ends Superseded at publish.
///
/// A PENDING END (cancel, stale ticket, session gone, shutdown) is decided once:
/// the first writer wins. A job the user cancelled ends Cancelled even if its
/// inputs change afterwards; a job stopped because it went stale ends
/// Superseded even if the user cancels it afterwards (cancel() still returns
/// true). shutdown() follows the same rule.
///
/// The executor NEVER LOADS A SESSION: it looks sessions up with
/// SessionModel::loadedSession() only, and a job whose session is not loaded
/// cannot be created or started. Every active job pins its session
/// (SessionModel::pinSession) so that the LRU does not unload it; removing the
/// session, merging into it, or repopulating the model is not prevented and
/// ends the job Superseded. The executor never touches the idle scheduler:
/// saves, loads and column work continue while a job runs.
///
/// JobModel is the store of the job records and the single source of truth
/// about work in progress; the executor keeps no job list of its own (the
/// chosen next job is the one record in state Queued).
///
/// ORDER OF A JOB'S END. (1) the model's end transition (one dataChanged);
/// (2) for a publication, SessionModel::publishCalculationInvalidation() -
/// dependencyChanged listeners already see the job finished, and
/// publishingJob() names the job for exactly this step; (3) jobFinished,
/// jobsChanged; (4) the session is unpinned; (5) the model trims its finished
/// rows; (6) idle(), or the chosen next job, which a slot may have offered
/// during (3), is scheduled. The next job never starts synchronously: always
/// from the event loop. A job that ends at its start (for any of the reasons
/// of docs/CALCULATIONS.md 15.2) ends that attempt, and the next is tried at
/// the next turn of the event loop: one attempt per turn at most. A chosen
/// next job that an offer replaces skips (6), so that no idle() falls between
/// it and its replacement.
///
/// Slots connected to the executor's signals may call offer(),
/// withdrawChosenNext(), cancel() and shutdown().
///
/// PRECONDITION of offer(), withdrawChosenNext(), cancel() and shutdown(): main
/// thread; not from inside a calculation or an engine callback (they inspect
/// or publish to engines).
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

    /// Requested calculations that may run at once. startNext() and the demand
    /// layer's load bound (kMaxHeldSessions, calculationdemand.h) are written in
    /// terms of it.
    static constexpr int kMaxRunningJobs = 1;

    struct OfferResult {
        enum class Kind {
            Created,            ///< a new chosen next job (`job`); a different chosen next job was replaced
            AlreadyActive,      ///< equal to the chosen next job, or to the running job not asked to stop (`job`); nothing changed
            SessionNotLoaded,   ///< no such session, or it is not loaded; the executor does not load sessions
            UnknownCalculation, ///< no such calculation / the name is not of that family
            MissingInput,       ///< a declared input is unavailable: there is nothing to compute
            Blocked,            ///< other explicit calculations must run first (engine.readiness())
            NothingToDo,        ///< already computed (a cached rejection or failure included), or not explicit
            ShuttingDown        ///< shutdown() was called
        };
        Kind  kind = Kind::SessionNotLoaded;
        JobId job = 0;          ///< non-zero for Created and AlreadyActive only
        bool created() const { return kind == Kind::Created; }
    };
    /// Makes one explicit calculation for one loaded session the chosen next
    /// job, unless an equal job is active (see activeJob()) or the engine's
    /// readiness() says there is nothing to run. A different chosen next job is
    /// replaced: it ends Cancelled ("No longer needed") without ever running,
    /// and no idle() is emitted between it and the new one. A refused offer
    /// changes nothing. Never prepares and never starts anything
    /// synchronously: the job is Queued when this returns.
    OfferResult offer(const QString &sessionId, const CalculationBlocker &calculation);
    /// A plain (non-family) calculation by id.
    OfferResult offer(const QString &sessionId, const CalculationId &plainCalculationId);
    /// Ends the chosen next job Cancelled ("No longer needed"); idle() follows
    /// when nothing runs. False when there is no chosen next job.
    bool withdrawChosenNext();

    // ---- queries -------------------------------------------------------------
    /// The job an offer of (sessionId, instanceId) is compared with: the chosen
    /// next or running job with that key, EXCEPT a running job that has been
    /// asked to stop (cancelled, or stale: see STALE WHILE RUNNING) - that one
    /// is winding down, and a new offer becomes the chosen next job behind it.
    /// 0 if none.
    JobId        activeJob(const QString &sessionId, const QString &instanceId) const;
    /// The running job, if any (one winding down included), then the chosen
    /// next job, if any: never more than two ids.
    QList<JobId> activeJobs() const;
    JobId        runningJob() const;    ///< 0 if none
    JobId        chosenNextJob() const; ///< the one Queued job, 0 if none
    /// The job whose end is at step (2) of ORDER OF A JOB'S END (its
    /// invalidations are being published); 0 at every other moment.
    JobId        publishingJob() const { return m_publishingJob; }
    JobRecord    job(JobId id) const;   ///< == model()->record(id)
    bool         isIdle() const;        ///< no chosen next job, nothing running
    bool         isShutDown() const { return m_shutDown; }

    // ---- cancellation ----------------------------------------------------------
    /// The chosen next job ends Cancelled at once. The running job is asked to
    /// stop and ends Cancelled when its compute function returns - even if that
    /// returned a complete result: once cancellation was requested nothing is
    /// published. The next job does not start before then. False: unknown or
    /// already finished.
    bool cancel(JobId id);
    /// Cancels everything ("Application closing") and waits - without a
    /// timeout - until the worker has returned; afterwards no worker and no
    /// ticket exists and offer() returns ShuttingDown. The wait is bounded by
    /// the compute function's cancellation boundaries; abandoning a live thread
    /// instead would be a crash. Idempotent.
    void shutdown();

    /// Test seam: the next `count` worker starts are treated as failed (thread
    /// creation failure cannot be provoked portably).
    void failNextWorkerStarts(int count) { m_failWorkerStarts = count; }

signals:
    /// After the model's rowsInserted for a new chosen next job. A job that a
    /// slot connected to rowsInserted has already ended (cancel, shutdown) or
    /// removed is not announced as queued: jobFinished was its only signal.
    /// offer() still returns Created for it.
    void jobQueued(FlySight::JobId id);
    void jobStarted(FlySight::JobId id);
    void jobProgress(FlySight::JobId id, const QString &text);
    void jobCancelRequested(FlySight::JobId id);    ///< the running job was asked to stop (cancel, or stale)
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
    /// Step (6) of ORDER OF A JOB'S END: done, or skipped for a replaced
    /// chosen next job (its replacement follows at once).
    enum class AfterEnd { ScheduleOrIdle, Nothing };

    void scheduleStart();
    void startNext();
    void onWorkerFinished(JobId id);
    void finishRun();
    void endJob(JobId id, JobState state, const QString &reason,
                std::optional<ResultStatus> resultStatus = std::nullopt,
                const QSet<DependencyKey> &invalidated = {},
                AfterEnd afterEnd = AfterEnd::ScheduleOrIdle);
    void requestStop(const PendingEnd &end);
    void stopRunIfRefused();
    void onProgress(JobId id, const QString &text);
    void onSessionRowsChanged();
    bool isSessionLoaded(const QString &sessionId) const;
    int runningCount() const { return m_run ? 1 : 0; }
    void announceIdleIfIdle();

    QPointer<SessionModel> m_sessionModel;
    JobModel *m_model;                  // child object
    JobId m_nextId = 1;
    bool m_shutDown = false;
    bool m_startQueued = false;         // a queued startNext() is pending
    bool m_idleAnnounced = true;        // idle() was emitted since the last job was created
    int m_failWorkerStarts = 0;
    int m_registryObserver = 0;         // CalculationRegistry::addObserver() token
    JobId m_publishingJob = 0;          // see publishingJob()
    std::unique_ptr<Run> m_run;         // non-null from a job's start until its worker was joined
};

} // namespace FlySight

#endif // JOBQUEUE_H
