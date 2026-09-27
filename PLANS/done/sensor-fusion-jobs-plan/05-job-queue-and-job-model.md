# Phase 5: Job queue and job model

## Overview

One application-wide `JobQueue` owns all background explicit-calculation work
(spec section 8). A job is one explicit calculation for one session; the queue
deduplicates requests, runs jobs one at a time in request order on the
application's only worker thread (64 MiB stack), and ends every job in exactly
one of succeeded / cancelled / superseded / failed, using Phase 4's
prepare / compute / publish so that the engine, not the queue, decides
staleness. Its records are presented by `JobModel`, a Qt table model complete
enough that a future jobs dock is a pure view. Both classes live in
`flysight_core` (Qt Core + Gui, no Widgets, no GTSAM), are driven entirely from
the main thread, and are tested with synthetic explicit calculations.

## Dependencies

- **Depends on:** Phase 4 (Engine: asynchronous request and blocker inspection).
- **Blocks:** Phase 6 (Plot request logic), Phase 9 (Fusion calculation), Phase 10.
- **Assumptions:**
  - Phase 4's API exists exactly as in `04-engine-async-request-and-blockers.md`:
    `CalculationEngine::prepare()` / `PrepareOutcome`, `PreparedCalculation::compute()`
    / `publish()`, `ComputedCalculation`, `PublishOutcome`, `CalculationProgress`,
    `CalculationCancelled`, `readiness()`, `blockers()`, `resultDetail()`,
    `CalculationBlocker`, `CalculationRegistry::title()`, and in the tests
    `tests/support/asyncdriver.h` and `Synthetic::registerExplicitWorld()`.
  - A ticket (`PreparedCalculation`) is created, published and destroyed on the
    main thread; only `compute()` crosses to the worker; destroying the engine
    nulls the ticket's engine pointer, so a ticket may safely outlive its
    session (Phase 4 "Staleness").
  - `SessionData` engines are bound to `CalculationRegistry::instance()`
    (`src/sessiondata.h` line 179-182), so queue tests register their synthetic
    calculations on the global registry and remove them in `cleanup()`.
  - `SessionModel` is as on `master`: `evictSession()` may already return
    `false` for a row that must stay loaded, and `evictIfNeeded()` passes over
    such rows (`src/sessionmodel.cpp` lines 1619-1689). Rows that are visible or
    focused are not in the LRU list at all.
  - Shared files (`src/CMakeLists.txt`, `tests/CMakeLists.txt`,
    `docs/CALCULATIONS.md`, `tests/README.md`, `tests/acceptance_map.txt`) get
    small, additive, separate hunks (overview Decision 10).
  - Commits are made by the orchestrator only (overview "Commit Policy").

## Design summary (read before the tasks)

### Classes and files

| Type | File | Role |
|---|---|---|
| `JobId`, `JobState`, `JobRecord` | `src/jobmodel.h` | The vocabulary: id, the six states, one job's complete record. |
| `JobModel : QAbstractTableModel` | `src/jobmodel.h` / `.cpp` | Holds the records (it is the store, not a copy) and presents them. Mutated only by `JobQueue` (`friend`). |
| `JobQueue : QObject` | `src/jobqueue.h` / `.cpp` | Requests, dedup, ordering, the worker, publish, end states, cancellation, shutdown. Owns its `JobModel`. |
| `JobQueue`-private `JobWorker : QThread`, `JobProgress : CalculationProgress` | `src/jobqueue.cpp` only | One thread per running job; the facility implementation. |
| `SessionModel::pinSession()` / `unpinSession()` / `publishCalculationInvalidation()` | `src/sessionmodel.h` / `.cpp` | The narrow hooks: eviction deferral and publication of engine-returned invalidations. |

File and class names follow `master`'s flat lower-case convention
(`sessionmodel.cpp`, `momentmodel.cpp`, `idlescheduler.cpp`). Everything is in
`namespace FlySight`.

### Public API (part of the contract for phases 6, 7, 9)

```cpp
// jobmodel.h -----------------------------------------------------------------
using JobId = quint64;                       // 0 = "no job"; ids start at 1 and are never reused

enum class JobState { Queued, Running, Succeeded, Cancelled, Superseded, Failed };

struct JobRecord {
    JobId          id = 0;
    QString        sessionId;
    QString        sessionName;              // snapshot at request: _DESCRIPTION, else the session id
    CalculationId  calculationId;            // registration id
    DependencyKey  instanceOutput = DependencyKey::attribute(QString());   // empty name: plain calculation
    QString        instanceId;               // == calculationId for a plain calculation
    QString        calculationTitle;         // CalculationBlocker::title
    JobState       state = JobState::Queued;
    bool           cancelRequested = false;  // Running only: asked to stop, not stopped yet
    QString        progressText;             // latest text; kept after the job ends
    QDateTime      queuedAt, startedAt, finishedAt;   // UTC; startedAt invalid if compute never started
    QString        reason;                   // outcome reason; empty for a plain success
    std::optional<ResultStatus> resultStatus;         // Succeeded only: the published status
    bool isFinished() const;                 // state is one of the four end states
    bool isActive() const;                   // Queued or Running
};

class JobModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { SessionColumn, CalculationColumn, StateColumn, ProgressColumn,
                  QueuedColumn, StartedColumn, FinishedColumn, ReasonColumn, ColumnCount };
    enum Roles { JobIdRole = Qt::UserRole + 1, SessionIdRole, SessionNameRole,
                 CalculationIdRole, InstanceIdRole, CalculationTitleRole,
                 StateRole, CancelRequestedRole, ProgressTextRole,
                 QueuedTimeRole, StartedTimeRole, FinishedTimeRole,
                 ReasonRole, ResultStatusRole, IsFinishedRole };

    explicit JobModel(QObject *parent = nullptr);
    // QAbstractTableModel: rowCount, columnCount, data, headerData, flags, roleNames, removeRows
    int rowOf(JobId id) const;                        // -1 when unknown (or already removed)
    JobRecord record(int row) const;                  // by value
    JobRecord record(JobId id) const;                 // default record (id 0) when unknown
    static QString stateText(JobState state);         // tr(): "Queued", "Running", ...

    bool removeFinished(JobId id);                    // false for an active or unknown job
    int  clearFinished();                             // number removed
    int  finishedLimit() const;                       // default 200
    void setFinishedLimit(int limit);                 // >= 0; trims at once, oldest finished first
private:
    friend class JobQueue;                            // append / mutate / finish: see Task 5.2
};

// jobqueue.h -----------------------------------------------------------------
class JobQueue : public QObject {
    Q_OBJECT
public:
    static constexpr qsizetype kWorkerStackSize = 64 * 1024 * 1024;

    explicit JobQueue(SessionModel *sessionModel, QObject *parent = nullptr);
    ~JobQueue() override;                             // calls shutdown()

    JobModel *model() const;

    struct RequestResult {
        enum class Kind { Created, AlreadyActive,      // job != 0
                          SessionNotLoaded, UnknownCalculation, MissingInput,
                          Blocked, NothingToDo, ShuttingDown };   // job == 0
        Kind  kind = Kind::SessionNotLoaded;
        JobId job = 0;
        bool created() const { return kind == Kind::Created; }
    };
    RequestResult request(const QString &sessionId, const CalculationBlocker &calculation);
    RequestResult request(const QString &sessionId, const CalculationId &plainCalculationId);

    // Queries (all O(number of active jobs))
    JobId        activeJob(const QString &sessionId, const QString &instanceId) const;  // 0 if none; see "Deduplication"
    QList<JobId> activeJobs() const;                  // request order; the running job, if any, is first
    JobId        runningJob() const;                  // 0 if none
    JobRecord    job(JobId id) const;                 // == model()->record(id)
    bool         isIdle() const;                      // nothing queued, nothing running
    bool         isShutDown() const;

    // Cancellation
    bool cancel(JobId id);                            // false: unknown or already finished
    int  cancelSession(const QString &sessionId);     // every active job of the session
    int  cancelAll();
    int  cancelUnwantedQueued(const std::function<bool(const JobRecord &)> &isWanted);   // spec 9.5; never the running job
    void shutdown();                                  // cancel everything, wait for the worker; idempotent

    // Test seam
    void failNextWorkerStarts(int count);             // the next `count` worker starts are treated as failed

signals:
    void jobQueued(FlySight::JobId id);
    void jobStarted(FlySight::JobId id);
    void jobProgress(FlySight::JobId id, const QString &text);
    void jobCancelRequested(FlySight::JobId id);      // a running job was asked to stop
    void jobFinished(FlySight::JobId id, FlySight::JobState state);
    void jobsChanged();                               // after every one of the above except jobProgress
    void idle();                                      // the last active job ended
};

// sessionmodel.h (additions) ---------------------------------------------------
void pinSession(const QString &sessionId);            // counted; a pinned loaded row is never evicted
void unpinSession(const QString &sessionId);          // at zero: schedules one deferred eviction pass
bool isSessionPinned(const QString &sessionId) const;
void publishCalculationInvalidation(const QString &sessionId, const QSet<DependencyKey> &keys);
```

### How a job identifies and finds its session

A job stores the **session id** only, never a row index, a `SessionData *`, or
an engine pointer (rows move on sort and on growth; `ROW STABILITY` in
`src/sessionmodel.h` lines 96-105). Each time the queue needs the session
(at `request()` and when the job starts) it takes a `RowStabilityGuard`
(`m_sessionModel->stableRows()`), calls `loadedSession(sessionId)`
(`src/sessionmodel.cpp` lines 1015-1025), uses
`session->calculationEngine()` (a const accessor that returns the mutable
engine) inside the guard, and releases the guard **before** emitting anything
or calling `publishCalculationInvalidation()`. `loadedSession()` is a plain
read: it never loads, never touches the LRU. **The queue never calls
`sessionRef()`** - that is the only way to honour "the queue does not load
sessions" (spec 8.1).

`publish()` needs no session lookup: the ticket carries its own engine pointer
and the engine decides whether the session still exists.

### A job whose session is not loaded (spec 8.1) - decision

- `request()` for a session that has no row or is not loaded returns
  `SessionNotLoaded` and creates nothing.
- A **queued** job whose session stops being loaded ends at once as
  **Superseded**, reason "Session removed or unloaded". It does not stay
  queued. Justification: the queue cannot cause the session to load, so a
  waiting job would wait on something nobody promised; the job model would
  show "queued" (and phase 6 "pending") indefinitely; and spec 8.2 lists
  "session removed or unloaded" under superseded together with "whoever still
  wants the result sees it as missing and asks again". The check happens
  eagerly on `SessionModel`'s `modelReset` / `rowsRemoved` and again, as the
  authoritative check, when the job reaches the head of the queue.
- In practice eviction never causes this, because every active job pins its
  session (next section); removal, and a wholesale repopulation of the model
  (`populateFromIndex` / `populateFromUuids`), do.

### Eviction deferral (spec 8.2, overview risk) - decision: pin, with superseded as the natural fallback

`SessionModel` gains a counted pin per session id. `evictSession()` returns
`false` for a pinned row at the same place, and with the same consequences, as
the existing "save failed: must stay loaded" case: the row keeps its place in
the LRU list, `evictIfNeeded()` passes over it, and the cache may exceed its
capacity by the number of pinned rows. This is clean because the eviction path
already has exactly this shape; no new invariant is introduced.

- The queue pins a session when a job for it is **created** and unpins when
  that job **ends** (one pin per job, so counts balance by construction).
  Pinning queued jobs too - not only the running one - costs nothing in
  practice: phase 6 removes queued jobs for hidden tracks (spec 9.5), and
  visible rows are not in the LRU anyway.
- `unpinSession()` reaching zero schedules one deferred `evictIfNeeded()`
  (queued invocation), so the cache returns to capacity without the queue ever
  running eviction from inside one of its own signal emissions and without
  tripping `assertRowsMutable()`.
- Pins never block `removeSessions()`, a merge, or a repopulation. Those
  destroy or change the session under the job; the engine then refuses the
  ticket and the job ends **Superseded** - the spec's fallback, which is
  correct there because the user asked for the session to go or change.
- `SessionModel` stays ignorant of the queue: it knows "pinned ids", nothing
  else. Unpinning an id that has no row is a no-op on rows.

### The worker

- **One `JobWorker` (a private `QThread` subclass) per running job**, created
  when the job starts, `setStackSize(JobQueue::kWorkerStackSize)`, started,
  joined (`wait()`) and deleted on the main thread when the job ends. At most
  one exists at a time, so it is still "the application's only worker thread"
  (overview Decision 4). Chosen over a long-lived thread because: "the worker
  could not start" is then a per-job fact that maps directly to **Failed** and
  is retryable; no worker-side event loop, `moveToThread`, or cross-thread
  `QObject` is needed; shutdown joins exactly the thread that runs compute.
  Thread creation cost is irrelevant next to a fit. The 64 MiB is reserved
  address space, committed lazily, and exists only while a job runs.
- `JobWorker` holds three things, handed over before `start()` and read back
  only after `wait()`: a raw `PreparedCalculation *` (the queue keeps the
  owning `std::unique_ptr`), a `JobProgress`, and a `ComputedCalculation`
  result slot. `run()` is one statement:
  `m_result = m_ticket->compute(&m_progress);`. `compute()` never throws. The
  happens-before edges are `QThread::start()` and `QThread::wait()`; there is
  no concurrently accessed field and therefore no lock.
- **The ticket never leaves the main thread** in the ownership sense: it is
  created by `prepare()`, held by the queue's run state, published and
  destroyed by the queue. Only the `compute()` call crosses.
- **Progress** (`JobProgress::report(text)`, called on the worker): posts to
  the main thread with
  `QMetaObject::invokeMethod(queue, [queue, jobId, text] { queue->onProgress(jobId, text); }, Qt::QueuedConnection)`.
  `QString` is copied by value into the closure; the post is Qt's thread-safe
  event queue, not shared state. `onProgress()` ignores ids that are not the
  running job. Because the queue is the context object, events still pending
  when the queue is destroyed are dropped by Qt. Posts from one thread are
  delivered in order and `QThread::finished` is posted after them, so the last
  progress text is recorded before the job ends.
- **Cancellation** is a `std::atomic<bool>` inside `JobProgress`, written by
  the main thread (`requestCancel()`), read by `isCancelled()` on the worker.
  This atomic *is* the facility of spec section 6, not a lock protecting shared
  state; it is the only atomic in the queue.
- **Completion**: `QThread::finished` is connected (explicitly
  `Qt::QueuedConnection`) to a queue slot capturing the job id. The slot
  returns immediately unless that job is still the current run (shutdown may
  already have consumed it), then calls `wait()` (the signal is emitted
  slightly before the thread has fully exited), then finishes the job.
- **Worker could not start**: after `start()`, a thread that is neither
  `isRunning()` nor `isFinished()` was not created (Qt warns and leaves both
  false). The job ends **Failed**, reason "The worker thread could not be
  started"; the ticket is destroyed unpublished, so nothing is cached and the
  calculation stays requestable. `failNextWorkerStarts(n)` makes the queue
  skip `start()` and take this path; it exists because thread-creation failure
  cannot be provoked portably.
- The queue **never pauses, wakes, or registers with `IdleScheduler`**
  (`src/idlescheduler.h`); saves, loads and column work continue during a job
  (acceptance 19, 20).

### Request, deduplication, ordering

`request(sessionId, calculation)`, main thread, never from inside an engine
evaluation (it calls `readiness()`, which asserts that no evaluation is open):

1. Shut down: `ShuttingDown`.
2. An active job with the same **dedup key `(sessionId, instanceId)`** that is
   not cancel-requested exists: `AlreadyActive` with its id. (`instanceId`
   already embeds the registration id - `"<familyId>#<key>"` for a family
   instance - so the triple the spec talks about is this pair.) A running job
   that has been asked to cancel does **not** count: a new request while the
   old one is still winding down creates a new queued job, which cannot start
   before the old one has ended. Without this rule a refresh pressed right
   after a cancel would be silently lost.
3. Session not loaded: `SessionNotLoaded`.
4. `engine.readiness(calculation.registrationId, calculation.instanceOutput)`:
   `Unknown` -> `UnknownCalculation`; `MissingInput` -> `MissingInput`
   (**acceptance 11: no job can be created for a session without the inputs**);
   `Blocked` -> `Blocked` (the caller requests the blockers instead; chaining is
   phase 6's, spec 9.3); `Done` -> `NothingToDo` (already computed, including a
   cached rejection or failure, or not an explicit calculation); `Ready` ->
   continue.
5. Create the record (`Queued`, `queuedAt` now, `sessionName` =
   `_DESCRIPTION` attribute, else the session id - the rule of
   `src/ui/docks/legend/LegendPresenter.cpp` lines 360-362), append it to the
   model, pin the session, emit `jobQueued`, `jobsChanged`, and schedule
   `startNext()` with a queued invocation. Return `Created`.

`request()` never prepares and never starts a job synchronously: inputs are
captured when the job **starts**, not when it is requested (a job queued behind
a five-minute fit must see the session as it is five minutes later), and the
caller can queue several jobs and observe them all as `Queued`.

The plain-id overload builds the `CalculationBlocker` itself
(`instanceOutput` = empty attribute name, `instanceId` = the id, `title` =
`CalculationRegistry::instance().title(id)`).

`startNext()` (always from the event loop, never re-entrantly):

1. Return if shut down or a job is running. **This check is the ordering
   guarantee**: the running slot is cleared only in `finishRun()`, after the
   worker has been joined, so the next job cannot start until the running one
   has ended (spec 8.3).
2. Take the oldest `Queued` job (first come, first served). None: return.
3. Session not loaded -> end Superseded ("Session removed or unloaded"); loop to 2.
4. `prepare()` under the guard; release the guard; if the outcome is
   `NothingToRun` or `Blocked`, publish `outcome.invalidated` through
   `SessionModel::publishCalculationInvalidation()` (Phase 4: "the caller
   treats it like `RequestOutcome::invalidated`"). For `Ready`,
   `invalidated` is informational and is not published.
5. Map the outcome (table below). Anything but `Ready` ends the job and loops
   to 2 - iteratively, not recursively.
6. `Ready`: state `Running`, `startedAt` now, `progressText` cleared, emit
   `jobStarted`, `jobsChanged`; create and start the worker (or fail, see
   above).

### End-state mapping (requirement from Phase 4)

At start (`prepare()`), no compute ran, `startedAt` stays invalid:

| `PrepareOutcome::Kind` | End state | Reason text |
|---|---|---|
| `NotFound` | Superseded | "Calculation is no longer registered" |
| `NotExplicit` | Superseded | "Calculation is no longer requested explicitly" |
| `AlreadyValid` | Superseded | "Result is already available" |
| `NothingToRun` | Superseded | "Inputs changed: nothing to compute" |
| `Blocked` | Superseded | "Inputs changed: waiting for %1" (titles of the blockers, comma separated) |
| `Ready` | (runs) | |

When the worker has returned (`finishRun()`), in this order:

| Condition | Action | End state | Reason text / extras |
|---|---|---|---|
| an end was already decided for the run (`pendingEnd`, see cancellation) | destroy the ticket **without** `publish()` | the pending state | the pending reason |
| `publish()` -> `Published`, status `Ok` | publish `invalidated` via `SessionModel` | **Succeeded** | `reason` = `PublishOutcome::detail` (the rejection reason, empty for a plain success); `resultStatus` = `Ok` |
| `Published`, any other status (`Failed`, `UndeclaredRead`, `InvalidOutput`) | same | **Succeeded** | `reason` = "Calculation failed: %1" with `detail` (or the status name when `detail` is empty); `resultStatus` = that status. This is spec 8.2's "published and cached as a failed result, and the job record says so" |
| `RefusedStale / InputsChanged` | | Superseded | "Inputs changed" |
| `RefusedStale / AlreadyPublished` | | Superseded | "Result is already available" |
| `RefusedGone / SessionGone` | | Superseded | "Session removed or unloaded" |
| `RefusedGone / RegistrationRemoved` | | Superseded | "Calculation is no longer registered" |
| `Discarded / ResourceExhausted` | | **Failed** | "Out of memory" |
| `Discarded / Cancelled` (the compute function threw `CalculationCancelled` although the queue never asked) | | Cancelled | "Cancelled by the calculation" |

`publish()` is always called when no end is pending, including for
`Cancelled` / `ResourceExhausted` computes: the engine reports staleness before
discarding (Phase 4 "Publish" step 4), so a superseded job is reported as
superseded.

Every end, whatever the path, goes through one private `endJob(id, state,
reason, resultStatus)` that performs, in this order: (1) the model transition
(`finishedAt` now; one `dataChanged`); (2) for a publication, 
`SessionModel::publishCalculationInvalidation(sessionId, invalidated)` - so a
listener of `dependencyChanged` that looks at the job model already sees the
job finished; (3) `jobFinished`, `jobsChanged`; (4) `unpinSession`;
(5) retention trimming in the model; (6) `idle()` if nothing is active, else a
queued `startNext()`.

### Cancellation, abandonment, shutdown

- `cancel(id)` on a **queued** job ends it at once: Cancelled, reason
  "Cancelled". (Spec 8.3 "removes it" means removed from the queue; the record
  stays in the model as a finished entry, which acceptance 18 requires.)
- `cancel(id)` on the **running** job sets `pendingEnd = {Cancelled,
  "Cancelled"}` (first writer wins), sets `cancelRequested`, calls
  `JobProgress::requestCancel()`, emits `jobCancelRequested`, `jobsChanged`.
  The job stays `Running` until the worker returns. **Decision (Phase 4's open
  question):** a job for which cancel was requested ends Cancelled and
  publishes nothing even if its compute function returned a complete result -
  spec 8.3 says "the job ends as cancelled when it does" and 8.2 says
  "nothing is published"; it also makes the outcome independent of a race the
  user cannot see.
- `cancelUnwantedQueued(isWanted)` ends every **queued** job for which the
  predicate returns false as Cancelled, reason "No longer needed". The running
  job is never offered to the predicate (spec 9.5). Collect ids first, then
  end them: slots connected to `jobFinished` may call back into the queue.
- **Abandonment.** On `SessionModel`'s `modelReset` and `rowsRemoved`, every
  queued job whose session is no longer loaded ends Superseded, and if the
  *running* job's session is no longer loaded the queue sets `pendingEnd =
  {Superseded, "Session removed or unloaded"}` and requests cancellation, so a
  multi-minute fit for a deleted session does not hold the queue. The state is
  still Superseded (what happened), not Cancelled. No attempt is made to abort
  early on a mere input change: the ticket does not expose staleness before
  publish and Phase 4's API is not extended.
- `shutdown()`: sets the shut-down flag (later requests return
  `ShuttingDown`); ends every queued job Cancelled, reason "Application
  closing"; for the running job sets `pendingEnd` (unless one is set), requests
  cancellation, **`wait()`s for the worker without a timeout**, and runs
  `finishRun()` inline. Idempotent; called by the destructor. After it returns
  no worker exists and no ticket is alive.
- **Why no timeout.** Spec 8.3 requires both "waits for the worker to stop
  before tearing down anything the worker could touch" and "must not crash".
  Abandoning a live thread that is inside a solver and letting static
  destruction proceed is a crash. The bound "no longer than one solver step"
  is therefore delivered by the compute function's cancellation boundaries
  (spec section 6; phases 8-9), and this phase proves the queue adds nothing on
  top: the wait ends as soon as compute returns, whatever compute returns.
- **Teardown order is not load-bearing for memory safety.** The queue holds
  `QPointer<SessionModel>`. If the model dies first, its engines null the
  tickets' engine pointers (Phase 4), publish reports `SessionGone`, and the
  pin / publication calls are skipped through the null `QPointer`. Phase 7
  should still call `shutdown()` in `MainWindow::closeEvent()` before anything
  else is torn down (note that `QObject` deletes children in creation order,
  so parenting alone would destroy the session model before the queue).

### The model (spec 8.4)

- One row per job, in request order (ascending `JobId`); new rows are appended;
  a job keeps its row for life, so a row index changes only when earlier rows
  are removed.
- Columns (Display role): session name; calculation title; `stateText()`;
  progress text; queued, started, finished as local-time text
  (`QLocale().toString(dt.toLocalTime(), QLocale::ShortFormat)`, empty when
  invalid); reason. Every custom role is answered on every column. Time roles
  return `QDateTime` (UTC). `StateRole` returns `int(JobState)`.
  `ResultStatusRole` returns `int(ResultStatus)` or an invalid `QVariant`.
  `headerData()` gives `tr()` titles for horizontal Display. `flags()` is
  `ItemIsEnabled | ItemIsSelectable`. `roleNames()` exposes the roles in
  camelCase (pattern: `MarkerModel::roleNames`, `src/markermodel.cpp`).
- **Every transition is its own signal**, never coalesced: `rowsInserted`
  (Queued); `dataChanged(row, 0 .. ColumnCount-1)` with the affected roles for
  Queued->Running, for cancel-requested, and for the end transition;
  `dataChanged` on `ProgressColumn` with `{Qt::DisplayRole, ProgressTextRole}`
  for progress; `rowsRemoved` for removal and trimming. No `modelReset` is ever
  emitted after construction.
- Removal: `removeFinished(id)`, `clearFinished()`, and `removeRows()` (so a
  view's standard deletion works) remove **finished** rows only; `removeRows`
  over a range that contains an active job removes nothing and returns false.
- Retention: finished rows are kept for the life of the application up to
  `finishedLimit()` (default 200). When a job finishes and the count exceeds
  the limit, the oldest finished rows are removed (`rowsRemoved`) after the
  end transition has been signalled. Active rows are never trimmed. Nothing is
  written to settings or disk.

## Tasks

### Task 5.1: `SessionModel` hooks - pinning and publication of engine-returned invalidations

**Purpose:** Give the queue the two narrow things it needs from the session model: "do not evict this while a job needs it" and "tell consumers these names changed" (Phase 4 note; spec 9.6).

**Files to modify:**
- `src/sessionmodel.h` - public `pinSession`, `unpinSession`, `isSessionPinned`, `publishCalculationInvalidation`; private `QHash<QString, int> m_pinnedSessions` and `bool m_evictionPassQueued = false`. Extend the class comment with a short paragraph "PINNED SESSIONS" next to the LRU description: who pins (the job queue), what a pin does and does not prevent.
- `src/sessionmodel.cpp` - the four functions; one check in `evictSession()`.

**Technical Approach:**
- `evictSession()` (lines 1630-1689): after the "already a stub" early return (line 1644-1647) and **before** the `saveFailed` check, add `if (m_pinnedSessions.value(sessionId) > 0) return false;` with a comment mirroring the `saveFailed` one ("a job is queued or running for it; it stays loaded and in the LRU list"). Nothing is saved or filled for a pinned row in that pass. Update the comment of `evictIfNeeded()` (lines 1621-1623) to mention pinned rows alongside failed saves.
- `pinSession(id)`: ignore an empty id; increment. `unpinSession(id)`: decrement, remove the entry at zero (a call without a matching pin is a no-op with a `qWarning`); at zero, if `!m_evictionPassQueued`, set it and `QMetaObject::invokeMethod(this, [this] { m_evictionPassQueued = false; evictIfNeeded(); }, Qt::QueuedConnection)` - the same deferral style as `queueInvalidation()` (lines 1251-1262). Never call `evictIfNeeded()` synchronously from `unpinSession()`.
- Do not touch pins in `removeSessions()`, `populateFrom*()`, or `setRowSessionId()`: pins are keyed by id and balanced by the pinner. (`setRowSessionId()` only renames rows that are not loaded, and a pinned session is loaded.)
- `publishCalculationInvalidation(sessionId, keys)`: return if `keys` is empty, the id has no row, or the row is not loaded; otherwise `publishInvalidation(row, keys)` (lines 1426-1436) followed by `emit modelChanged()` - the same pair `flushPendingInvalidations()` emits (lines 1272-1282), but **immediately**, like `updateAttribute()` does for its own edits (line 1194). It asserts `assertRowsMutable()`-style that no `RowStabilityGuard` is held (it emits). It must **not** call `invalidateColumns()`, `scheduleSave()`, or mark anything dirty: publishing a calculation result is not a persistent change (class comment, lines 71-94). Loaded rows display live values, so the logbook repaints from the `dataChanged` it emits.

**Acceptance Criteria:**
- [ ] With cache capacity 1 and two hidden, unfocused, loaded, clean sessions, pinning the least recently used one keeps it loaded through `evictIfNeeded()` (triggered by setting `LogbookCacheSize`); after `unpinSession()` and one event-loop pass it is evicted (`tst_session_model_engine::pinnedSessionIsNotEvicted`).
- [ ] Pins are counted: two pins need two unpins. `isSessionPinned()` reflects the count. Unpinning an unknown id changes nothing and does not crash.
- [ ] `removeSessions()` removes a pinned session.
- [ ] `publishCalculationInvalidation("s1", {attribute X})` emits exactly one `dependencyChanged("s1", X)`, one publishing `dataChanged` for the row, and one `modelChanged`; for an unknown id, an unloaded row, or an empty set it emits nothing; it marks nothing dirty and schedules no save (`tst_session_model_engine::calculationInvalidationIsPublished`).
- [ ] All existing tests pass unchanged, in particular `tst_column_cache` (failed-save eviction) and `tst_session_model_engine`.

**Complexity:** S

---

### Task 5.2: Job vocabulary and `JobModel`

**Purpose:** The single source of truth about work in progress and work done (spec 8.4, section 12), as a Qt item model.

**Files to create:**
- `src/jobmodel.h` / `src/jobmodel.cpp` - `JobId`, `JobState`, `JobRecord`, `JobModel` as in the design summary.

**Files to modify:**
- `src/CMakeLists.txt` - add `jobmodel.cpp jobmodel.h` to `flysight_core` directly after the `idlescheduler` line (line 275); one hunk shared with Task 5.3's line.

**Technical Approach:**
- Storage: `QVector<JobRecord> m_jobs` in request order plus `QHash<JobId, int>` rebuilt after removals (pattern: `MomentModel::m_indexById` / `rebuildIndex()`, `src/momentmodel.h` lines 121-126).
- Private mutators used by `JobQueue` only, each emitting exactly the signals listed under "The model": `JobId append(JobRecord)` (assigns nothing; the queue assigns ids), `void markRunning(JobId, QDateTime)`, `void markCancelRequested(JobId)`, `void setProgress(JobId, QString)`, `void markFinished(JobId, JobState, QString reason, std::optional<ResultStatus>, QDateTime)`, `void trimFinished()`. `markFinished` asserts the job is active and the state is an end state; a second call is ignored with an assert (exactly-one-end-state is enforced here, in one place).
- `Q_DECLARE_METATYPE(FlySight::JobState)` in the header (outside the namespace); `JobQueue`'s constructor calls `qRegisterMetaType<JobId>("FlySight::JobId")` and `qRegisterMetaType<JobState>("FlySight::JobState")` so `QSignalSpy` and queued connections work.
- Header includes `engine/calctypes.h` (for `CalculationId`, `ResultStatus`) and `dependencykey.h`; no widget header. `QDateTime::currentDateTimeUtc()` is called by the queue, not the model, so the model is a pure container.
- Comment density and header layout as `src/sessionmodel.h`; class comment states the row-order, signal, removal and retention rules verbatim from the design summary because views will rely on them.

**Acceptance Criteria:**
- [ ] `QAbstractItemModelTester` (QtTest failure mode) reports nothing through append, every transition, progress, removal, clearing and trimming.
- [ ] `data()` returns the documented value for every role on every column, and an invalid `QVariant` for an out-of-range index; `StartedTimeRole` / `FinishedTimeRole` are invalid `QDateTime`s until set.
- [ ] `removeFinished`, `clearFinished`, `removeRows` never remove an active job; `rowOf()` is correct after removals.
- [ ] With `setFinishedLimit(3)`, finishing a fourth job removes the oldest finished row only, after the fourth job's end transition was signalled; an active job older than all finished ones is kept.
- [ ] No `modelReset` is emitted at any time after construction.
- [ ] `flysight_core` still links Qt Core and Gui only.

**Complexity:** M

---

### Task 5.3: `JobQueue` - request, deduplication, ordering, worker, publish, end states

**Purpose:** Run explicit calculations one at a time in the background with Phase 4's three steps, ending each job in exactly one state (spec 8.1, 8.2).

**Files to create:**
- `src/jobqueue.h` / `src/jobqueue.cpp`.

**Files to modify:**
- `src/CMakeLists.txt` - `jobqueue.cpp jobqueue.h` in `flysight_core`, same hunk as Task 5.2.

**Technical Approach:**
- Members: `QPointer<SessionModel> m_sessionModel`; `JobModel *m_model` (child object); `JobId m_nextId = 1`; `bool m_shutDown = false`; `bool m_startQueued = false`; `int m_failWorkerStarts = 0`; and the run state, a private struct held by `std::unique_ptr<Run> m_run`: `JobId jobId`, `std::unique_ptr<PreparedCalculation> ticket`, `std::unique_ptr<JobWorker> worker`, `std::optional<PendingEnd> pendingEnd` (`JobState` + reason). The queued jobs are not stored separately: "oldest `Queued` record in the model" is the queue (single source of truth).
- `JobProgress` and `JobWorker` are defined in an anonymous namespace / as private nested types in `jobqueue.cpp`, without `Q_OBJECT` (no new signals or slots; `QThread::finished` suffices), so no `.moc` include is needed. `JobProgress::report()` and `isCancelled()` must not throw (Phase 4 contract): the `invokeMethod` call is wrapped so that a `std::bad_alloc` while copying the text is swallowed (the text is dropped).
- Implement `request()`, `startNext()`, `finishRun()`, `endJob()` exactly as numbered in the design summary; the two mapping tables are normative, including the reason texts (all through `tr()`; tests compare the untranslated literals).
- `startNext()` is scheduled with `QMetaObject::invokeMethod(this, &JobQueue::startNext, Qt::QueuedConnection)` guarded by `m_startQueued`, the coalescing pattern of `SessionModel::queueInvalidation()`.
- Connect, in the constructor, `SessionModel::modelReset` and `rowsRemoved` to the abandonment check of Task 5.4 (both are `QAbstractItemModel` signals; `removeSessions()` uses a reset, lines 924-967).
- `publish()` and `prepare()` are only ever reached from the event loop (queued slot) or from `shutdown()` / `request()` called at top level, which satisfies the engine's "no evaluation open" assertion. State this precondition in the header for `request()`, `cancel*()` and `shutdown()`: "main thread; not from inside a calculation or an engine callback".
- Class comment: the lifecycle diagram (queued -> running -> four ends), the threading rule in three sentences, "never loads a session", "never touches the idle scheduler", and the signal order of `endJob()`.

**Acceptance Criteria:**
- [ ] A requested job runs on a thread other than the main thread whose `stackSize()` is 64 MiB, publishes all outputs together, and ends Succeeded; names read while unrequested (`DA`, `EA1`) are emitted as `SessionModel::dependencyChanged` for that session and then read their literal values (105, 5 for `EA_IN = 4`).
- [ ] A compute function that touches a 16 MiB stack buffer succeeds on the worker.
- [ ] Duplicate `request()` while queued or running returns `AlreadyActive` with the first job's id and adds no row (acceptance 14).
- [ ] With five jobs over three sessions the instrumented compute function never observes more than one concurrent run, and start order equals request order (acceptance 14).
- [ ] `request()` returns `MissingInput` and creates nothing for a session without the declared input; `SessionNotLoaded` for a stub row and for an unknown id; `Blocked` for B while A is unrequested; `NothingToDo` after publication, after a cached rejection, and for an on-demand id; `UnknownCalculation` for an unknown id (acceptance 11).
- [ ] The queue never loads a session: `SessionModel::sessionLoaded` is never emitted by any queue operation.
- [ ] Changing a declared input while the job runs ends it Superseded ("Inputs changed"), emits no `dependencyChanged` for the outputs, leaves `readiness()` `Ready`, and a new request computes the new literal value (acceptance 8, queue half).
- [ ] Input rejection: Succeeded, `resultStatus == Ok`, reason "negative input". Ordinary exception: Succeeded, `resultStatus == Failed`, reason "Calculation failed: synthetic failure", re-request `NothingToDo`. `std::bad_alloc`: Failed, "Out of memory", nothing cached, re-request `Created` and succeeds. Worker start failure: Failed, requestable again, and the next queued job runs.
- [ ] A job whose inputs changed between request and start ends Superseded without a worker being created (`startedAt` invalid), and the queue proceeds to the next job.
- [ ] `IdleScheduler` is not referenced by `jobqueue.cpp` / `jobmodel.cpp`.

**Complexity:** L

---

### Task 5.4: Cancellation, pruning, session removal, and shutdown

**Purpose:** Spec 8.3, 9.5 (queue side), and acceptance 10 and 17.

**Files to modify:**
- `src/jobqueue.h` / `src/jobqueue.cpp`.

**Technical Approach:**
- Implement `cancel`, `cancelSession`, `cancelAll`, `cancelUnwantedQueued`, the abandonment slot, and `shutdown()` as in "Cancellation, abandonment, shutdown". All multi-job operations snapshot the ids first and re-check each job's state before acting (a slot connected to `jobFinished` may have changed it).
- `shutdown()` must be safe when called from a slot connected to one of the queue's own signals, twice, with no jobs, and after the session model has been destroyed.
- The queued `QThread::finished` slot and queued progress lambdas that arrive after `shutdown()` consumed the run must find `m_run` empty (or a different `jobId`) and return.
- `~JobQueue()` calls `shutdown()`; `m_model` is a child and dies with the queue.

**Acceptance Criteria:**
- [ ] Cancelling the running job (held inside compute by the test) ends it Cancelled at the compute function's next cancellation check, publishes nothing (outputs unavailable, no `dependencyChanged`, `resultStatus()` of the calculation not set), leaves it requestable (`readiness() == Ready`), and the next queued job then starts and succeeds (acceptance 10). The next job's `startedAt` is not earlier than the cancelled job's `finishedAt`, and the instrumented concurrency maximum stays 1.
- [ ] A compute function that notices cancellation, performs one more finite step, and **returns a complete result** still ends Cancelled with nothing published.
- [ ] A request for the same calculation while the cancelled job is still running returns `Created` (new queued job), which runs after the old one ended.
- [ ] Cancelling a queued job ends it Cancelled without ever creating a worker; its row remains.
- [ ] `cancelUnwantedQueued` ends exactly the queued jobs the predicate rejects, with reason "No longer needed", and never the running job even if the predicate would reject it.
- [ ] Removing a session with a queued job: the job ends Superseded during `removeSessions()`. Removing a session with the running job: cancellation is requested, the job ends Superseded ("Session removed or unloaded"), nothing is published, no crash; the next job runs (acceptance 17).
- [ ] With cache capacity 1, a hidden session with a queued or running job is not evicted while the job is active and is evicted after it ends (acceptance 17, deferral). Repopulating the model (`populateFromUuids`) with a queued and a running job ends both Superseded without a crash.
- [ ] `shutdown()` with one running and two queued jobs returns; all three are Cancelled ("Application closing"); nothing was published; a later `request()` returns `ShuttingDown`; destroying queue then model, and model then queue (separate test functions), neither crashes nor hangs (acceptance 17).
- [ ] Each job in every scenario has exactly one end transition in the model.

**Complexity:** M

---

### Task 5.5: Test support - controllable explicit calculations on real sessions

**Purpose:** Let queue and model tests (and phase 6) hold a worker inside compute, observe it, and release or cancel it, without GTSAM and without sleeps.

**Files to create:**
- `tests/support/jobfixture.h` / `tests/support/jobfixture.cpp`.

**Files to modify:**
- `tests/CMakeLists.txt` - add the two files to `flysight_test_support` (one line, after `asyncdriver.h` from Phase 4).

**Technical Approach:**
- `namespace FlySightTest`. `class JobWorld` - constructed in a test's `init()`, destroyed in `cleanup()` **after** the `JobQueue` and `SessionModel` are gone (tests/README.md section 8: destroy the model before unregistering). It registers on `CalculationRegistry::instance()` and unregisters in its destructor:
  - Phase 4's `Synthetic::registerExplicitWorld(CalculationRegistry::instance())` (`expA`, `derivA`, `derivA2`, `expB`, `derivB`), remembering the ids it added.
  - Gated calculations, all Explicit, all with a title, each with input attribute `<X>_IN` and output attribute `<X>_OUT = <X>_IN + 1`:

    | Id / prefix | Behavior of compute |
    |---|---|
    | `gated` / `G` | records entry (see `Gate`), reports progress "step 1", waits for the gate while checking `progress().throwIfCancelled()`, reports "step 2", returns |
    | `stubborn` / `S` | as `gated`, but on cancellation does one more finite step (sums 1e6 doubles), then **returns normally** |
    | `thrower` / `T` | throws `std::runtime_error("synthetic failure")` |
    | `exhausted` / `X` | throws `std::bad_alloc` on the first call only (test-local `std::atomic<int>`; comment that real compute functions hold no state) |
    | `deepstack` / `D` | fills and sums a 16 MiB local `volatile` buffer, page by page |

  - `struct Gate`: `QSemaphore entered`, `QSemaphore proceed`, `std::atomic<int> running{0}`, `std::atomic<int> maxRunning{0}`, and a main-thread-readable start-order list guarded by being appended only before `entered.release()` and read only after `entered.acquire()`. Waiting inside compute is `while (!proceed.tryAcquire(1, 1)) ctx.progress().throwIfCancelled();` - a timed poll of the cancel flag, which is what a real compute function does at its solver boundaries; it is not a sleep used for synchronization. `Gate::open(n)` releases `n` permits; `Gate::waitEntered(timeoutMs = 5000)` is `entered.tryAcquire(1, timeoutMs)`.
  - `static QList<SessionData> sessions(const QStringList &ids)` - `DescentFixture::load(id)` per id (they have no `EA_IN` / `G_IN`: the "missing input" sessions); the test adds inputs through `SessionModel::updateAttribute(id, "G_IN", 4.0)`, the application's edit path.
  - `bool waitIdle(JobQueue &, int timeoutMs = 5000)` - spins the event loop (`QTRY`-style, like `waitForIdle` in `testenvironment.h` line 75) until `isIdle()`.
- Test-local synchronization is fine; the no-locks rule is about the library (Phase 4 Task 4.6 says the same).

**Acceptance Criteria:**
- [ ] After `JobWorld` is destroyed, `CalculationRegistry::instance().registeredIds()` equals its value before construction.
- [ ] The fixture contains no `QThread::sleep`, `QTest::qSleep`, `QTest::qWait(n)` used as a delay, or `std::this_thread::sleep_for`.
- [ ] `jobfixture.*` compiles into `flysight_test_support` without Widgets.

**Complexity:** M

---

### Task 5.6: `tst_jobqueue`

**Purpose:** Prove acceptance 8 (queue half), 10, 11 (queue half), 14, 17 and every criterion of Tasks 5.3 and 5.4 on a real `SessionModel`.

**Files to create:**
- `tests/tst_jobqueue.cpp` - class `JobQueueTest`; fixture modelled on `tests/tst_session_model_engine.cpp` lines 97-141 (`registerBuiltIns`, one stored-data logbook column, `useFreshLogbook`, `resetPreferencesToDefaults`, registry snapshot compared in `cleanup()`).

**Files to modify:**
- `tests/CMakeLists.txt` - under the `tst_session_model_engine` line (line 118): a comment line "Job queue and job model (synthetic explicit calculations; no GTSAM)" and `flysight_add_test(tst_jobqueue SOURCES tst_jobqueue.cpp LIBS Threads::Threads)` (`find_package(Threads)` comes from Phase 4).
- `tests/tst_session_model_engine.cpp` - `pinnedSessionIsNotEvicted`, `calculationInvalidationIsPublished` (Task 5.1), following `forEachLoadedSessionIsAPlainRead` (lines 550-585) for the capacity-1 setup and its `qScopeGuard` restore.

**Technical Approach:**
- Test functions: `runsAndPublishes`, `publishesInvalidationsThroughSessionModel`, `workerIsNotMainThreadAndHasLargeStack`, `duplicateRequestsCreateNoDuplicates`, `oneAtATimeInRequestOrder`, `refusesMissingInput`, `refusesUnloadedAndUnknownSession`, `refusesBlockedAndDone`, `neverLoadsASession`, `inputChangeWhileRunningSupersedes`, `inputChangeWhileQueuedSupersedesAtStart`, `registrationRemovedSupersedes`, `rejectionSucceedsWithReason`, `exceptionSucceedsAsFailedResult`, `resourceExhaustionFails`, `workerStartFailureFails`, `cancelRunningThenNextStarts`, `cancelIgnoredForOneStepStillCancelled`, `requestWhileCancellingCreatesNewJob`, `cancelQueued`, `cancelSessionAndCancelAll`, `cancelUnwantedQueuedSparesRunning`, `removeSessionWithQueuedJob`, `removeSessionWithRunningJob`, `evictionDeferredWhileJobActive`, `repopulateWithJobs`, `mergeIntoSessionWithRunningJobSupersedes` (a `mergeSessions` that changes a declared input; pattern `mergeEmitsDependencyChanged`, lines 325-343), `sortWhileRunningStillPublishes` (rows move; the ticket survives), `shutdownWithQueuedAndRunning`, `shutdownIsIdempotentAndRefusesRequests`, `modelDestroyedBeforeQueue`, `idleSchedulerKeepsWorking` (an attribute edit made while a job is held in compute is saved by the idle saver: `waitForIdle(model)` returns true before the gate is opened).
- Synchronization: `gate.waitEntered()` proves the worker is inside compute; `QTRY_COMPARE(queue.job(id).state, ...)` and `QSignalSpy::wait()` spin the event loop for main-thread effects. No sleeps.
- "Nothing published" is asserted three ways each time: the output attribute is unavailable, a `dependencyChanged` spy has no entry for the outputs, and `calculationEngine().resultStatus(id)` has no value or is `NotRequested`.
- Expected values are literals (tests/README.md section 8).
- The worker-thread identity check reads `QThread::currentThread()` and its `stackSize()` inside a test-local compute function into atomics / values read after the job ended.

**Acceptance Criteria:**
- [ ] Every function above exists and passes in Debug and Release on Windows, 50 consecutive runs, well inside the 120 s timeout.
- [ ] The test links `flysight_test_support` and `Threads::Threads` only; `cleanup()` verifies the global registry and `enrolledEngineCount() == 0`.
- [ ] Each acceptance item 8 (queue half), 10, 11, 14, 17 is demonstrated by at least one named function recorded in the acceptance map (Task 5.8).

**Complexity:** L

---

### Task 5.7: `tst_jobmodel` - model contract and the history view

**Purpose:** Acceptance 18: every transition arrives through model signals, finished jobs are retained with state, timing and reason, and a view can render the whole history from the model alone.

**Files to create:**
- `tests/tst_jobmodel.cpp` - class `JobModelTest`.

**Files to modify:**
- `tests/CMakeLists.txt` - `flysight_add_test(tst_jobmodel SOURCES tst_jobmodel.cpp LIBS Threads::Threads)` next to `tst_jobqueue`.

**Technical Approach:**
- A test-local `HistoryView : QObject` that is given **only** a `QAbstractItemModel *`. It connects to `rowsInserted`, `rowsRemoved`, `dataChanged`, `modelReset` (which must never fire) and reads exclusively through `index()` / `data()`. It keeps a mirror list of rows (all roles) and, per `JobIdRole`, the ordered list of distinct states it has seen and every progress text. It also counts rows in state Running after every signal.
- A `QAbstractItemModelTester(model, FailureReportingMode::QtTest)` is attached in `init()` for every function.
- Test functions: `rolesAndColumns` (every role on every column, headers, flags, `roleNames`), `historyFromSignalsAlone` (a scripted session: success with two progress texts, rejection, exception, cancel while running, cancel while queued, superseded by an input change, superseded at start, failed by exhaustion, failed worker start; afterwards the mirror equals `queue.job(id)` for every job field, and the per-job state sequences equal literal expectations such as `[Queued, Running, Succeeded]`, `[Queued, Cancelled]`, `[Queued, Superseded]`), `neverMoreThanOneRunningRow`, `timestampsAreOrdered` (`queuedAt <= startedAt <= finishedAt`, all UTC, `startedAt` invalid for jobs that never ran), `progressIsItsOwnSignal` (roles and column of the `dataChanged`), `cancelRequestedIsVisible`, `removeFinishedAndClear`, `removeRowsRefusesActive`, `retentionBound` (`setFinishedLimit(3)`), `nothingIsPersisted` (the test's `QSettings` and logbook folder are byte-identical before and after a scripted session; pattern: `logbookprobe.h`).

**Acceptance Criteria:**
- [ ] `HistoryView` includes no queue header and calls no `JobQueue` / `JobModel`-specific method (only `QAbstractItemModel` API and the role enum values).
- [ ] All functions pass in Debug and Release; the model tester reports nothing.
- [ ] Every state transition of every job in `historyFromSignalsAlone` was observed as a separate signal (the recorded sequences contain `Running` for every job that ran).

**Complexity:** M

---

### Task 5.8: Documentation and traceability

**Purpose:** Keep the engine documentation, test README and acceptance map in step, with append-only edits to shared files.

**Files to modify:**
- `docs/CALCULATIONS.md` - append one section (own hunk), numbered 15 whether or not Phase 4's 12-14 have landed yet (overview, Integration Note 2): **15. Background jobs** - what a job is, the lifecycle and the four end states with the mapping tables of this document, the dedup rule (including the cancel-requested exception), "the queue never loads a session", pinning, the one-thread-per-job worker and the two marshalling mechanisms (queued invocation for progress, the facility's atomic for cancel), `shutdown()` and why it has no timeout, the model's columns / roles / signal guarantees, and a note that reads never start jobs.
- `tests/README.md` - executable count (add 2 to the current count, whatever it is), two rows in the test table, the `tst_session_model_engine` row text, one sentence on `jobfixture.h` in section 8.
- `tests/acceptance_map.txt` - append comment-only lines to Phase 4's `# SFJ` block (Phase 4's convention: comment lines that `tests/audit/cleanup_audit.cmake` skips):

  ```
  # SFJ 8  tst_jobqueue inputChangeWhileRunningSupersedes
  # SFJ 10 tst_jobqueue cancelRunningThenNextStarts
  # SFJ 11 tst_jobqueue refusesMissingInput
  # SFJ 14 tst_jobqueue oneAtATimeInRequestOrder
  # SFJ 14 tst_jobqueue duplicateRequestsCreateNoDuplicates
  # SFJ 17 tst_jobqueue removeSessionWithRunningJob
  # SFJ 17 tst_jobqueue removeSessionWithQueuedJob
  # SFJ 17 tst_jobqueue evictionDeferredWhileJobActive
  # SFJ 17 tst_jobqueue shutdownWithQueuedAndRunning
  # SFJ 18 tst_jobmodel historyFromSignalsAlone
  # SFJ 18 tst_jobmodel retentionBound
  ```

  **Discrepancy for Phase 10 to reconcile:** Phase 2's document numbers this specification's items `100 + n` as real (audited) map lines, while Phase 4 and this phase use `# SFJ n` comment lines. Do not convert either here; if Phase 4's block is absent because Phase 4's map edit is not yet committed, create the block header exactly as Phase 4's document gives it. The `# SFJ n` comment lines are converted to audited `100 + n` lines by Phase 10, and the block header follows Phase 2's form (02 Task 2.7), which Phase 4's document reproduces (overview, Integration Note 1).

**Acceptance Criteria:**
- [ ] `audit_cleanup` passes.
- [ ] Every public member of `JobQueue`, `JobModel`, and the three `SessionModel` additions is named in `docs/CALCULATIONS.md` section 15 with its threading rule.
- [ ] Edits to the three shared files are append-only apart from the README count and rows.

**Complexity:** S

## Testing Requirements

### Unit Tests
- New: `tst_jobqueue` (Task 5.6), `tst_jobmodel` (Task 5.7), support `jobfixture` (Task 5.5).
- Extended: `tst_session_model_engine` (`pinnedSessionIsNotEvicted`, `calculationInvalidationIsPublished`).
- Unchanged and passing: everything else, especially `tst_column_cache` (eviction with failed saves), `tst_session_oracle`, `tst_workflow`, and Phase 4's `tst_calcengine_async` / `tst_calcengine_blockers`.

### Integration Tests
- `tst_jobqueue` is itself the integration of queue + `SessionModel` + real `SessionData` engines + the global registry: removal, eviction, merge, sort, repopulation, and shutdown are exercised through the public `SessionModel` API only.
- Build the application target: it links `flysight_core` and must compile although nothing constructs a `JobQueue` yet (Phase 7 wires it).

### Manual Verification
- `cmake --build build --config Release`, then `ctest --test-dir build -C Release -L core --output-on-failure`.
- Run `tst_jobqueue` and `tst_jobmodel` 50 times in a loop in Debug and Release.
- `git grep -n "QMutex\|std::mutex\|QReadWriteLock\|QWaitCondition" src/jobqueue.cpp src/jobmodel.cpp src/sessionmodel.cpp` finds nothing; `git grep -n "std::atomic" src/jobqueue.cpp` finds exactly the cancel flag.
- `git grep -n "idlescheduler\|IdleScheduler\|QtWidgets\|QWidget\|gtsam" src/jobqueue.* src/jobmodel.*` finds nothing.

## Notes for Implementer

### Gotchas
- Release the `RowStabilityGuard` before any emit, before `publishCalculationInvalidation()`, and before `endJob()`; never hold it across `start()` of the worker. `loadedSession()` asserts that a guard *is* held.
- `QThread::finished` is emitted from the worker shortly before the thread has exited: always `wait()` before reading the result slot or deleting the `JobWorker`.
- `QThread::setStackSize` must be called before `start()`. On Windows the value passes through an `unsigned` parameter; 64 MiB fits - do not "round up" to anything near 4 GiB.
- Queued `std::function`/lambda invocations need the queue as context object so that they are dropped when it dies. Do not capture the `Run` or the worker in them; capture the `JobId` and look it up.
- Do not destroy a ticket while its worker may still be inside `compute()`; every path that resets `m_run` goes through `wait()` first.
- Slots connected to the queue's signals may call `request()`, `cancel*()` and even `shutdown()`. Keep `endJob()`'s order, snapshot ids before loops, and never start a job synchronously from `request()` or `endJob()`.
- `PrepareOutcome::invalidated` must be published for `NothingToRun` / `Blocked`, not for `Ready`. `PublishOutcome::invalidated` must be published for `Published` only.
- `publish()` must be called even when compute reported `Cancelled` / `ResourceExhausted` and no end is pending, so that staleness wins over discarding (Phase 4 publish step order).
- Tests: destroy the `JobQueue`, then the `SessionModel`, then `JobWorld` (unregisters), except in the test that deliberately reverses the first two. A live model schedules an environment check when the registry changes.
- A registry change made while a job is running (a test registering something) can mark the ticket stale if it reaches an input's resolution; register everything in `init()` before requesting.
- Cached logbook columns: `publishCalculationInvalidation()` deliberately does not invalidate `SessionRow::cachedValues`. Loaded rows are displayed live, so the logbook is right on screen. See Open Questions for the interaction with `index.json`.
- `JobRecord::sessionName` is a snapshot; do not try to keep it live.

### Decisions Made
- **`JobQueue` + `JobModel` as two classes, the model being the store.** The queue has no job list of its own, which makes "the job model is the single source of truth" (spec 12) true by construction.
- **One thread per job**, not one long-lived thread: per-job start failure, no worker event loop, simplest join at shutdown; still only one worker exists at a time (Decision 4).
- **Inputs are captured at start, not at request**; request-time checking uses `readiness()` only.
- **Every active job pins its session** through a counted pin in `SessionModel`, reusing the existing "cannot evict" path; removal, merge and repopulation are not blocked and end the job Superseded (the spec's fallback).
- **A queued job whose session is not loaded ends Superseded immediately** rather than waiting (rationale above).
- **Cancel wins over a completed compute**: nothing is published once cancellation was requested.
- **A cancel-requested running job does not deduplicate new requests.**
- **The running job of a removed session is abandoned** (cancellation requested, ends Superseded) so it cannot hold the queue for minutes.
- **`AlreadyValid` / `AlreadyPublished` map to Superseded**, not Succeeded: this job published nothing, and "succeeded" is defined as "the result was published" by it.
- **Published failures are Succeeded + `resultStatus`**, keeping exactly the spec's four end states; the view distinguishes them by `ResultStatusRole` and the reason text.
- **`shutdown()` waits without a timeout**; the one-solver-step bound belongs to the compute function's cancellation boundaries.
- **Immediate, not coalesced, publication of invalidations** (`publishCalculationInvalidation`), like model edits: a job end is a single event, and phase 6 and the tests get deterministic ordering (job finished, then `dependencyChanged`, then `jobFinished`).
- **Retention bound 200**, adjustable through `setFinishedLimit()` (also the test seam).
- **`failNextWorkerStarts()`** is a documented test seam, in the spirit of `SessionModel::ColumnWorkStats`; thread-creation failure cannot be provoked portably.
- **Acceptance map lines are comments** following Phase 4; the `100 + n` scheme of Phase 2 is left for Phase 10 to reconcile.

### Open Questions
- None blocking. For Phase 9 / 10 to consider: a logbook column that reads an explicit output (for example an interpolated `Fusion/...` value) may have its cached value computed while the result is published and written to `index.json`, although results are not persisted and the value reads unavailable after a restart until fusion is requested again. This phase deliberately leaves cached columns alone (publication is not a persistent change); whether such columns should be excluded from the cache is a product decision outside spec section 8. **Answered by Phase 9 Task 9.5: such columns are cached as unavailable** (overview, Integration Note 12).

## Definition of Done

This phase is complete when:
1. All tasks have passing acceptance criteria
2. All tests pass (`ctest -L core`, plus `audit_cleanup`), in Debug and Release on Windows, including 50-run loops of the two new tests
3. Code follows patterns established in reference files (`src/sessionmodel.*`, `src/momentmodel.*`, `src/idlescheduler.*`; test conventions of `tests/README.md` section 8)
4. No TODOs or placeholder code remains
5. `flysight_core` links Qt Core + Gui only; no lock exists in the new code; the only atomic is the cancellation flag of the progress facility
6. `src/engine/*`, `src/idlescheduler.*`, `src/mainwindow.*`, and `src/ui/*` are untouched (application wiring is Phase 7)
