# Phase 1: Executor and plot demand

## Overview

This phase turns `JobQueue` from a first-come-first-served queue into an **executor**. The executor holds at most the running job and one **chosen next** job. It keeps no order of arrival, deduplicates against no list and prunes nothing, and it has a single `constexpr` bound on simultaneous jobs. Its worker runs at below-normal priority. The phase also replaces `PlotRequests` with the widget-free **demand layer** `CalculationDemand`. The demand layer:

- derives plot demand from the checked plots and the visible sessions;
- chooses the next job by priority: the focused session first, then row order;
- delays an input-changed session by the input-settle wait;
- remembers this run's job-level failures and not-applicable pairs;
- publishes a per-plot state for plot rows to present.

Plot rows lose their refresh and cancel controls and every gesture entry point. Checking a plot, like any other check change, goes through `PlotModel` and is enough to start work. Tests, audit rules and the acceptance map are brought in line so that everything is green at the end of the phase.

## Dependencies

- **Depends on:** None. The phase can begin immediately.
- **Blocks:** Phase 2 (column demand), Phase 3 (working indicator, hover detail and pending cells), Phase 4 (documentation, acceptance map and audit).
- **Assumptions:**
  - The working branch is `store-requested-calculations` at `b55869d` or later, with a clean tracked tree.
  - The rules in `PLANS/implementation-plan/00-overview.md` "Decisions & Constraints" hold, as refined under "Decisions Made" below.
  - Build only `build-phase1/`: `cmake --build build-phase1 --config Release`.
  - After adding `tests/tst_calculation_demand.cpp`, reconfigure the inner project once with `cmake build-phase1/FlySightViewer-build`.
  - Test with `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure`.
  - **Never build `build/`.**

---

## Tasks

### Task 1.1: Turn `JobQueue` into the executor (offer / chosen next / withdraw)

**Purpose:** Make the executor hold at most the running job plus one chosen next job, chosen by its caller, with one bound and a low-priority worker. Lifecycle, pinning, publication and storing stay as they are.

**Files to modify:**
- `src/jobqueue.h`: public API, class comment and private members, as described below.
- `src/jobqueue.cpp`: `offer`, the replacement path, `withdrawChosenNext`, `chosenNextJob`, the `startNext` loop, `isIdle`, `endJob` (the `publishingJob` bracket and the follow-up switch), the worker priority, and removal of the queue-only code.

**Technical Approach:**

1. **Remove**:
   - both `request()` overloads and `RequestResult`;
   - `oldestQueued()`;
   - `cancelSession()`, `cancelAll()` and `cancelUnwantedQueued()`;
   - the `<functional>` include, if nothing else needs it.

   Keep `cancel(JobId)` (the later jobs dock calls it; no product code does in this plan) and `shutdown()`. Keep `failNextWorkerStarts()`, `activeJob()`, `activeJobs()`, `runningJob()`, `job()`, `isIdle()` and `isShutDown()`, with the semantics below. Keep the class and file names (`JobQueue`, `jobqueue.{h,cpp}`); comments call it "the executor".

2. **Add the public API:**

   ```cpp
   /// Requested calculations that may run at once. startNext() and the demand
   /// layer's load bound (phase 2) are written in terms of it.
   static constexpr int kMaxRunningJobs = 1;

   struct OfferResult {
       enum class Kind {
           Created,            ///< a new chosen next job (`job`); a different chosen next job was replaced
           AlreadyActive,      ///< equal to the chosen next job, or to the running job not asked to stop (`job`); nothing changed
           SessionNotLoaded, UnknownCalculation, MissingInput, Blocked, NothingToDo,
           ShuttingDown        ///< shutdown() was called
       };
       Kind  kind = Kind::SessionNotLoaded;
       JobId job = 0;          ///< non-zero for Created and AlreadyActive only
       bool created() const { return kind == Kind::Created; }
   };
   OfferResult offer(const QString &sessionId, const CalculationBlocker &calculation);
   OfferResult offer(const QString &sessionId, const CalculationId &plainCalculationId);  // as request(id) did
   bool  withdrawChosenNext();          ///< false when there is no chosen next job
   JobId chosenNextJob() const;         ///< the one Queued job, 0 if none
   JobId publishingJob() const;         ///< see ORDER OF A JOB'S END, step (2); 0 at every other moment
   ```

   The refusal kinds mean exactly what the `RequestResult` kinds meant, and map readiness the same way (`jobqueue.cpp:200-206`). The equality key is `(sessionId, instanceId)`, as in `activeJob()` today.

3. **`offer()`** follows `request()` (`jobqueue.cpp:173-238`), with these changes:
   1. If shut down, return `ShuttingDown`.
   2. If `activeJob(sessionId, instanceId)` is non-zero, return `AlreadyActive`. That is the chosen next job, or the running job that has not been asked to stop. **Nothing else changes**: an existing chosen next job is not touched when the offer equals the running job.
   3. Run the readiness read under a `RowStabilityGuard`, unchanged. A refusal changes nothing, including the current chosen next job.
   4. When the calculation is `Ready` and a chosen next job with a different key exists, **replace** it:
      - end it `Cancelled` with `tr("No longer needed")`, the existing string of `cancelUnwantedQueued`, through `endJob`;
      - the replacement's end suppresses step (6) of ORDER OF A JOB'S END. Give `endJob` a private follow-up parameter (e.g. `enum class AfterEnd { ScheduleOrIdle, Nothing }`), so no `idle()` is emitted and no start is scheduled between the old job and the new one.
      - Re-validate after the end's signals, because slots may call `shutdown()` or `offer()`. If shut down, return `ShuttingDown`. If a Queued job exists again, replace it the same way. Loop until none remains.
   5. Create the job exactly as `request()` did:
      - set `m_idleAnnounced = false` and pin before `append`;
      - do not announce a job that a `rowsInserted` slot ended;
      - emit `jobQueued` and `jobsChanged`;
      - call `scheduleStart()`, so the job never starts synchronously.

4. **`withdrawChosenNext()`**: if a chosen next job exists, end it `Cancelled` with `tr("No longer needed")` through the normal `endJob`. Step (6) runs as usual: `idle()` when nothing runs. Return `true`. Otherwise return `false`.

5. **`chosenNextJob()`** is derived from the model: the one record in state `Queued`. The executor keeps no job list of its own (class comment, "JobModel is the store"). By construction at most one Queued record exists; assert that in debug builds.

6. **Starting** (`startNext`, `jobqueue.cpp:311`):
   - The loop condition becomes `!m_shutDown && runningCount() < kMaxRunningJobs`, where `runningCount()` is `m_run ? 1 : 0`. Add `static_assert(kMaxRunningJobs == 1, ...)` next to `struct Run` to record that the implementation has one Run slot.
   - Take `chosenNextJob()` instead of `oldestQueued()`. The prepare and `PrepareOutcome` handling is unchanged.
   - Start the worker with `m_run->worker->start(QThread::LowPriority)` (`jobqueue.cpp:391`).

7. **`isIdle()`** is `!m_run && chosenNextJob() == 0`.

8. **`activeJobs()`** returns the running job, if any (including one winding down), then the chosen next job. It never returns more than two ids.

9. **`publishingJob()`**: in `endJob`, set a member `m_publishingJob = id` immediately before `m_sessionModel->publishCalculationInvalidation(...)` and reset it to 0 right after (`jobqueue.cpp:498-499`). This covers both a publication and a prepare-time `invalidated` set. The demand layer uses it to tell a publication's `dependencyChanged` from an edit's (Task 1.3).

10. **Unchanged:** `onSessionRowsChanged()`, which ends a chosen next job whose session is gone as `Superseded`; `stopRunIfRefused()`; `requestStop()`; `finishRun()`; `shutdown()`, which cancels the chosen next job and the running job with "Application closing"; pinning; `JobModel` and `JobState`.

11. **Class comment** (`jobqueue.h:21-99`). Rewrite:
    - Opening paragraph: "The executor of requested calculations. It holds at most the running job and the one job its caller has chosen to run next. It keeps no order of arrival and no list; the caller decides what runs next. Jobs run one at a time (`kMaxRunningJobs`) on the application's only worker thread, which runs at below-normal priority."
    - LIFECYCLE diagram: `offer() --> Queued (the chosen next job) --> Running --> ...`. A Queued job ends `Cancelled` when it is replaced by another offer, withdrawn, `cancel()`ed or shut down, and `Superseded` when it goes stale at start or its session goes.
    - "Whoever still wants a result sees it missing and offers it again".
    - ORDER OF A JOB'S END, step (2): name `publishingJob()`.
    - ORDER OF A JOB'S END, step (6): "idle(), or the chosen next job, which a slot may have offered during (3), is scheduled".
    - The re-entrancy line: "Slots connected to the executor's signals may call offer(), withdrawChosenNext(), cancel() and shutdown()".
    - The PRECONDITION list: offer(), withdrawChosenNext(), cancel(), shutdown().
    - STALE WHILE RUNNING: "a new offer ... becomes the chosen next job behind it".

    Keep everything that is still true verbatim (THREADING, pinning, first-writer-wins, "never loads a session", "never touches the idle scheduler").

**Acceptance Criteria:**
- [ ] `grep` of `src/` finds no `RequestResult`, `oldestQueued`, `cancelSession`, `cancelAll` or `cancelUnwantedQueued`.
- [ ] `JobQueue::kMaxRunningJobs == 1`, and `startNext` uses it.
- [ ] After any sequence of offer, withdraw, cancel, start and end, the job model holds at most one `Queued` record and at most two active records (tested in 1.7).
- [ ] An offer that differs from the chosen next job ends the old job `Cancelled` "No longer needed" (it never started and its pin is released), and emits no `idle()` between the old job and the new one.
- [ ] An offer equal to the chosen next job, or to the running job not asked to stop, returns `AlreadyActive` and changes nothing.
- [ ] A refused offer leaves an existing chosen next job untouched.
- [ ] An offered job never starts synchronously.
- [ ] The worker is started with `QThread::LowPriority`.
- [ ] `publishingJob()` is non-zero only inside `publishCalculationInvalidation()` during `endJob`.
- [ ] Existing lifecycle behaviour is unchanged: stale-while-running, first writer wins, pinning, shutdown and `failNextWorkerStarts`.

**Complexity:** L

---

### Task 1.2: `CalculationDemand`: types, per-plot state, inspection and publication

**Purpose:** Create the widget-free demand layer's header and its read side. The read side covers which plots matter, the tracks, their conditions, the per-plot state with counts, lists and tooltip, and the change signals. It replaces everything `PlotRequests` computed.

**Files to create:**
- `src/calculationdemand.h`: types and class, as below.
- `src/calculationdemand.cpp`: implementation.

**Files to delete:**
- `src/plotrequests.h`, `src/plotrequests.cpp`. Their logic moves here under demand-layer names; see "What moves over" below.

**Technical Approach:**

1. **Types** (in `calculationdemand.h`, namespace `FlySight`, plain values with `operator==`/`!=`):

   ```cpp
   /// Where one track (a visible, loaded session) stands for one demand source (a plot; phase 2: a column).
   enum class DemandCondition {
       Done,           ///< the value is available (a result exists): nothing to compute
       Waiting,        ///< in demand, not running (inside the input-settle wait, chosen next, or behind other work)
       Running,        ///< the executor's running job, not asked to stop, is one of its calculations
       Failed,         ///< an input-determined failure (NotProduced) or a job-level failure remembered this run
       NotApplicable   ///< unavailable for ordinary reasons, or refused as not applicable: silently absent
   };

   struct DemandTrack {
       QString sessionId;
       QString sessionName;            ///< live _DESCRIPTION, else the session id
       DemandCondition condition = DemandCondition::NotApplicable;
       QStringList calculationTitles;  ///< Waiting/Running: the blockers' titles; Failed: what did not produce / what failed
       QString reason;                 ///< Failed only; never empty
       bool jobFailure = false;        ///< Failed only: the job failed (not stored; retried at the next start)
       bool settling = false;          ///< Waiting only: the session is inside its input-settle wait
       JobId job = 0;                  ///< Running: the running job; Waiting: the chosen next job if it is this track's, else 0
       QString progressText;           ///< Running only: the running job's latest progress text
   };

   /// Everything a plot row (phase 2: a column header) presents. The default value is "plain".
   struct DemandState {
       QString sourceId;               ///< plot id "<sensorID>/<measurementID>" (== PlotModel::PlotValueIdRole)
       bool requested = false;         ///< the source's value depends on a requested calculation; false: never inspected
       int wantedCount = 0;            ///< tracks that are not NotApplicable
       int doneCount = 0;              ///< ... of which nothing is left to compute: Done + Failed
       int waitingCount = 0;
       int runningCount = 0;           ///< 0 or 1 (JobQueue::kMaxRunningJobs)
       int failedCount = 0;            ///< input-determined + job-level
       QList<DemandTrack> running, waiting, failed;   ///< each in session-model row order
       QString progressLabel;          ///< "<doneCount> of <wantedCount>" while isWorking(); else empty
       QString toolTip;                ///< buildToolTip(*this); empty when isPlain()
       bool isWorking() const { return waitingCount + runningCount > 0; }
       bool showsWarning() const { return !isWorking() && failedCount > 0; }   ///< spec 10: the badge replaces the indicator
       bool isPlain() const { return !isWorking() && failedCount == 0; }
   };
   ```

   The following are Phase 3's hover detail inputs and must be kept:
   - the counts;
   - `running`, with its session name and `progressText`;
   - `failed`, with `reason` and `jobFailure`;
   - `showsWarning()`.

2. **Class** (`QObject`, main thread only). The public API is:

   ```cpp
   class CalculationDemand : public QObject {
       Q_OBJECT
   public:
       static constexpr int kInputSettleMs = 1000;      ///< input-settle wait (Task 1.3)
       CalculationDemand(SessionModel *sessionModel, PlotModel *plotModel, JobQueue *executor,
                         QObject *parent = nullptr);
       ~CalculationDemand() override;                   ///< removes the registry observer

       static QString plotId(const QString &sensorId, const QString &measurementId);
       static QString plotId(const PlotValue &plot);
       DemandState plotState(const QString &plotId) const;   ///< default for unchecked / not requested / unknown
       static QString buildToolTip(const DemandState &state);
       static bool isMerelyUncomputed(const SessionData &session, const QString &sensorId,
                                      const QString &measurementId);

       // test seams
       void flush();                                    ///< runs a pending pass now
       bool hasPendingUpdate() const;
       int  passCount() const;
       void setInputSettleDelay(int milliseconds);
       int  inputSettleDelay() const;
       void endInputSettleWaits();                      ///< every session's wait ends now; schedules a pass
       bool isSettling(const QString &sessionId) const;
       bool hasSettlingSessions() const;
   signals:
       void plotStateChanged(const QString &plotId);   ///< plotState(plotId) differs from what it was
       void statesChanged();                           ///< once per pass (or progress update) that changed a state
   };
   ```

   Collaborators are held by `QPointer`. A missing one makes the component inert: every state is the default, nothing is offered, and every test seam is safe. This is today's `isInert()`.

3. **What moves over from `plotrequests.cpp`, renamed and otherwise unchanged:**
   - `plotId` (`:103-111`);
   - `isExplicitBacked` → `isRequested`, memoizing `dependsOnExplicit` and `staticDependencies` per plot id (`:127-141`);
   - `syncCheckedSet` (`:145`), which returns whether a plot was unchecked or vanished;
   - `rebuildRelevantNames` (`:173`);
   - `inspectedPlots` (`:183`);
   - `isVisibleLoadedTrack` (`:46`);
   - `inspectUnderGuard`, including the debug x-axis check with the class name in its message (`:226-251`);
   - `inspect()`, the one guarded read, which returns plain values (`:258-286`);
   - `failureReason` (`:297-312`);
   - `scheduleUpdate`, `flush` and `hasPendingUpdate`, with the zero-interval single-shot `m_updateTimer` (`:376-392`);
   - `applyStates`, which stores before emitting, lists dropped plots once, and emits `plotStateChanged` per changed id and then `statesChanged` once (`:490-512`);
   - `isMerelyUncomputed` (`:216-223`). Its doc now says "waiting to be computed (the row shows it working) or rejected by a requested calculation (the warning badge)".

   Nothing of the waiting set (`m_waiting`), `continueAfter`, `stopContinuing`, `requestMissing`, `requestBlockers`, `pruneUnwantedQueued`, `forgetSessions`, `dropVanishedTracks` or `liveJobs` survives.

4. **Classification of one track**, given its `BlockerReport` for the plot's y name, plus the executor's running and chosen next jobs, the memory and the settle set (Task 1.3):

   | Report | Condition |
   |---|---|
   | `Available` | `Done` |
   | `NotApplicable` | `NotApplicable` |
   | `NotProduced` | `Failed`, `jobFailure = false`, reason from `failureReason(notProduced)`, titles from the notes |
   | `Blocked`, a blocker key equals the running job's key and that job has `!cancelRequested` | `Running` (`job`, `progressText` from the record) |
   | `Blocked`, otherwise, a blocker is in job-failure memory | `Failed`, `jobFailure = true`, reason = the memory's reason |
   | `Blocked`, otherwise, every blocker is in not-applicable memory | `NotApplicable` (titles cleared) |
   | `Blocked`, otherwise | `Waiting`: `settling = isSettling(session)`; `job` = chosen next id if its key is one of the blockers; titles of the blockers not in not-applicable memory |

   A Blocked report's notes are ignored, as today: Blocked wins. The pair key throughout is `(sessionId, blocker.instanceId)`.

5. **Building a plot's state:**
   - Run over the plot's tracks in row order.
   - Fill the counts and lists as defined in the struct comments. `doneCount` counts `Done` and `Failed`. `wantedCount` counts everything but `NotApplicable`.
   - `progressLabel = tr("%1 of %2").arg(doneCount).arg(wantedCount)` while working.
   - `toolTip = buildToolTip(state)`.

   There is no episode or waiting set: every count is a function of the current tracks.

6. **`buildToolTip`** (pure). Its lines are:
   - If `isWorking()`: `tr("Computing: %1 of %2 done")`. Then, for each running track, `"  " + tr("%1 - %2: %3")` (name, titles joined by ", ", `progressText` or else `tr("running")`).
   - If `failedCount > 0`: `tr("Could not be computed:")`. Then, for each failed track, `"  " + tr("%1 - %2")` (name, reason).
   - Lines are joined with `\n`. The result is empty when `isPlain()`.
   - The job-level failure reason is built when it is remembered (Task 1.3) as `"<title>: <JobRecord::reason>"`, e.g. `"Gated: The worker thread could not be started"`, `"Exhausted: Out of memory"`.

7. **Progress without inspection** is today's `onJobProgress` (`plotrequests.cpp:945-973`), keyed on `DemandTrack::job` of `running` tracks. It updates `progressText` and rebuilds `toolTip`, and emits for the changed plots only.

8. **Class comment.** Write a new one in the `plotrequests.h:113-192` style, with sections:
   - WHAT IS WANTED: checked, requested plots × visible, loaded, not-failed-load tracks; the result sources of spec §5 for loaded sessions.
   - TRACK CONDITIONS: the table above.
   - WHAT INSPECTION COSTS: unchanged in substance.
   - THE CHOICE: see 1.3.
   - THE INPUT-SETTLE WAIT: see 1.3.
   - MEMORY: see 1.3.
   - WHEN A PASS RUNS: see 1.3.
   - THE ONLY CALLER: the only product caller of `JobQueue::offer()` / `withdrawChosenNext()`; nothing calls back into it; it observes `PlotModel`, `SessionModel`, the registry and the executor's signals.
   - LIFETIME: main thread only; create after the executor and destroy before it.

   Do not name `QThread`, `IdleScheduler` or `QEventLoop` anywhere in these files; audit rules match comments.

**Acceptance Criteria:**
- [ ] `src/plotrequests.{h,cpp}` no longer exist; `src/calculationdemand.{h,cpp}` exist in `flysight_core`.
- [ ] `plotState()` of an unchecked, non-requested or unknown plot equals `DemandState()`. A non-requested plot is never inspected (no engine read) and never announced.
- [ ] Every state field matches the definitions above, verified in Task 1.8.
- [ ] `buildToolTip(DemandState())` is empty, and the tooltip strings are exactly as specified.
- [ ] `isMerelyUncomputed` returns true for Blocked and NotProduced, and false otherwise.

**Complexity:** L

---

### Task 1.3: `CalculationDemand`: choosing, offering, the input-settle wait, memory and pass triggers

**Purpose:** Make work follow demand. Every pass keeps the executor's chosen next job equal to the demand layer's current choice, by priority. It delays an input-changed session by the settle wait, and it never re-runs a job-level failure or a not-applicable pair in this run.

**Files to modify:**
- `src/calculationdemand.h` / `.cpp` (from Task 1.2).

**Technical Approach:**

1. **Private structure.** The names are guidance; the shape is binding, because Phase 2 extends it.
   - `using PairKey = QPair<QString, QString>;`, i.e. (session id, instance id).
   - `struct Memory { enum class Kind { NotApplicable, JobFailed } kind; QString reason; };` in `QHash<PairKey, Memory> m_memory`.
   - `struct Candidate { QString sessionId; CalculationBlocker calculation; };`
   - `QList<Candidate> plotCandidates(const Inspections &, const QString &focusedId) const;` returns tier (a), the focused session's pairs, followed by tier (b), the other visible sessions' pairs in row order. Within a session, the order is plot-model order and then the report's blocker order, which is upstream first. Pairs are deduplicated by instance id. The following are excluded:
     - pairs in memory;
     - the key of the running job, when it has not been asked to stop;
     - every pair of a settling session.

     Phase 2 appends tier (c), column demand, from its own `columnCandidates()`.
   - `void offerChoice(const QList<Candidate> &candidates);`. See step 3.
   - Settle state:
     - `QHash<QString, QDeadlineTimer> m_settleUntil`;
     - a single-shot `QTimer m_settleTimer`, armed for the earliest deadline;
     - `int m_settleDelayMs = kInputSettleMs`.
   - `JobId m_offeredJob = 0`: the id of the last offer this component made that the executor accepted (`Created`, or `AlreadyActive` with the chosen next job).
   - `bool m_reconciling = false`: the re-entrancy guard.

2. **The pass (`recompute()`)**. There is one function, and every path runs it:
   1. `m_updateTimer.stop()` and `++m_passCount`. Set `m_reconciling = true`, reset on every exit.
   2. Drop expired settle entries.
   3. `syncCheckedSet()`, `inspectedPlots()`.
   4. If the component is inert, or there are no inspected plots: publish default states and go to step 7.
   5. Run `inspect(plots)`: plain values, the only guarded read.
   6. If `!m_queue->isShutDown()`: `offerChoice(plotCandidates(inspections, focused))`. The focused id is `SessionModel::focusedSessionId()`, used only if it is one of the tracks.
   7. Build the states from the inspections and the executor's current running and chosen next jobs, after the offers, and from the memory. Then `applyStates`.

   If no inspected plot exists but the executor has a chosen next job equal to `m_offeredJob`, withdraw it; demand is empty.

3. **`offerChoice`.** Walk the candidates in order:
   - If the candidate's key equals `chosenNextJob()`'s key: done. Set `m_offeredJob` to that id.
   - Otherwise, `offer()` it and handle the result:

     | Result | Action |
     |---|---|
     | `Created` | set `m_offeredJob`; done |
     | `MissingInput` / `NothingToDo` / `UnknownCalculation` | remember `NotApplicable` for the pair; try the next candidate |
     | `Blocked` / `SessionNotLoaded` / `AlreadyActive` | try the next candidate; nothing is remembered |
     | `ShuttingDown` | stop |

   - If no candidate is accepted and `chosenNextJob() != 0 && chosenNextJob() == m_offeredJob`: `withdrawChosenNext()`.
   - A chosen next job that this component did not offer is replaced when it has a choice, and otherwise left alone. In the application nothing else offers (the audit keeps this the only caller). The rule makes tests that drive the executor directly next to a demand layer deterministic.

4. **Triggers.** Connect as `PlotRequests` did (`plotrequests.cpp:55-96`), with these changes:

   | Signal | Reaction |
   |---|---|
   | `PlotModel::dataChanged` (roles empty or `CheckStateRole`), `modelReset` | `syncCheckedSet()`; if a plot was unchecked **and** `chosenNextJob() != 0`, run `recompute()` now; otherwise schedule |
   | `SessionModel::visibilityChanged(shown, hidden)` | if `hidden` is non-empty and `chosenNextJob() != 0`, run `recompute()` now; otherwise schedule |
   | `SessionModel::sessionLoaded`, `modelChanged`, `focusedSessionChanged` | schedule |
   | `SessionModel::modelReset` | forget the memory and settle entries of session ids that no longer have a row; schedule |
   | `SessionModel::dependencyChanged(s, key)` | see step 5 |
   | `JobQueue::jobStarted`, `jobCancelRequested` | schedule. After a start the chosen-next slot is empty; the pass fills it. After a cancel request the running pair is waiting again. |
   | `JobQueue::jobFinished(id, state)` | see step 6 |
   | `JobQueue::jobProgress` | text update only (Task 1.2 item 7) |
   | registry observer | clear the memos, `m_relevantNames` and **all** memory; schedule |
   | `m_settleTimer` timeout | drop expired entries; re-arm for the earliest remaining; schedule |

   "Now" guarantees that unchecking or hiding drops the waiting pair before it can start. An offered job starts from the next turn of the event loop.

5. **Input changes and the settle wait.** In the `dependencyChanged(sessionId, key)` slot:
   - If `m_relevantNamesDirty`, rebuild. If `key` is not in `m_relevantNames`, return. An irrelevant edit schedules nothing and clears nothing, as today (`plotrequests.cpp:890-904`).
   - If `m_queue && m_queue->publishingJob() != 0 && m_queue->job(m_queue->publishingJob()).sessionId == sessionId`, it is a **publication**. Schedule and return: no settle, no memory change. The synchronous pass of step 6 continues a chain.
   - Otherwise it is an **input change**:
     - erase every memory entry of `sessionId`;
     - set `m_settleUntil[sessionId] = QDeadlineTimer(m_settleDelayMs)`, which restarts the wait on every change;
     - re-arm `m_settleTimer`;
     - schedule.

   A settling session's pairs are counted `Waiting`, with `settling = true`, from the first change, and are never candidates. Showing, hiding, checking, profile application and loading never start a wait.

   `setInputSettleDelay(ms)` affects later changes only. `endInputSettleWaits()` clears `m_settleUntil`, stops the timer and schedules.

6. **`jobFinished(id, state)`**:
   1. Read the record first, because it may be trimmed.
   2. If `state == JobState::Failed`, remember `JobFailed` for `(record.sessionId, record.instanceId)`, with reason `record.calculationTitle + ": " + record.reason`.
   3. If `m_reconciling` is set (the end came from this component's own offer or withdraw), schedule and return.
   4. Otherwise run `recompute()` **synchronously**. The executor emits `jobFinished` before it decides between `idle()` and starting the next job. The synchronous pass puts the next choice in place first. Chained calculations then continue without an idle period between links, and the executor never goes idle while demand remains.

   This covers every end, not only `Succeeded`:
   - `Superseded`: a stale job; its session is settling.
   - `Cancelled`: by `shutdown()` (the offer answers `ShuttingDown`), or by an external `cancel()` (still in demand, so it is offered again).
   - `Failed`: remembered, so it is not offered again.

7. **Start-up.** The constructor calls `syncCheckedSet()` and `scheduleUpdate()`, as today. At start-up every session is hidden, so the first pass has no tracks and offers nothing.

**Acceptance Criteria:**
- [ ] Checking a plot, making a session visible or loading a visible session starts the missing pairs with no other call. The `rowScript`, `showingASessionStartsIt` and `loadingAVisibleSessionStartsIt` tests pass.
- [ ] The choice order is the focused visible session, then the other visible sessions in row order, upstream first. `focusedSessionFirstThenRowOrder` passes.
- [ ] While a job runs, the executor's chosen next job is the demand layer's current choice. A change of demand replaces it, and the replaced job ends `Cancelled` "No longer needed".
- [ ] Unchecking or hiding drops the chosen next job before it starts. The running job is never stopped by the demand layer.
- [ ] A relevant input change starts that session's settle wait, and a burst of changes produces exactly one job. A publication never starts a wait.
- [ ] A job-level failure is not offered again until an input change of its session or a registry change. A fresh `CalculationDemand` (a "restart") offers it again.
- [ ] Not-applicable refusals are remembered and suppress the pair: the track is `NotApplicable`. The memory clears on the session's input change and on registry change.
- [ ] After `JobQueue::shutdown()` nothing is offered, and no pass loops.

**Complexity:** L

---

### Task 1.4: Wire the demand layer into the application; delete `PlotRequests`

**Purpose:** Construct the demand layer where `PlotRequests` was constructed, hand it to the plot list, and keep the plot widget's "no data" check working.

**Files to modify:**
- `src/CMakeLists.txt` (`:298-300`). Replace `plotrequests.cpp plotrequests.h` with `calculationdemand.cpp calculationdemand.h`. Rewrite the comment above it: "Plot check state (widget-free) and the demand layer: what requested calculations the checked plots need for the visible sessions, which runs next, the per-plot state". Update the jobqueue comment (`:290-293`) to "the executor of requested calculations: the running job and one chosen next job (owns the application's only worker thread) and the item model of its records".
- `src/mainwindow.h` (`:32`, `:136`):
  - forward declaration `class CalculationDemand;`;
  - member `CalculationDemand *m_calculationDemand = nullptr; // the demand layer: plot demand and row state; destroyed before the executor`.
- `src/mainwindow.cpp`:
  - `#include "calculationdemand.h"` replaces `plotrequests.h` (`:49`).
  - `:205-213`: `m_calculationDemand = new CalculationDemand(model, m_plotModel, m_jobQueue, this);`. Rewrite the comment: "The executor for requested calculations, then the demand layer that decides what it runs, in that order and here: the session model is populated, every calculation is registered, and no dock exists yet. Work follows what is switched on: the checked plots restored by setPlots() below and a first-launch applyProfile() create demand the same way a click does, but every session starts hidden, so starting the application starts no job; work starts when a session is shown."
  - `:225`: `ctx.calculationDemand = m_calculationDemand;`.
  - `:342-351` destructor: delete the demand layer first, then the executor. Update the comment wording ("the demand layer, then the executor").
  - `closeEvent` comment (`:356-363`): "cancel every background job" becomes "stop the executor". "refuses every later request" becomes "refuses every later offer".
- `src/ui/docks/AppContext.h` (`:16`, `:31-32`): forward declaration `class CalculationDemand;`; replace `plotRequests` with `CalculationDemand* calculationDemand = nullptr; // plot-list row state (may be null: rows are then plain)`. Keep the `jobQueue` comment.
- `src/ui/docks/plotselection/PlotSelectionDockFeature.cpp` (`:27-29`): `new PlotRowDelegate(ctx.calculationDemand, m_treeView)`. New comment: "Rows of plots over requested calculations show a working indicator and progress while their demand is computed, and a warning badge for sessions that could not be computed (null demand layer: plain rows)".
- `src/ui/docks/plot/PlotWidget.cpp`:
  - `#include "calculationdemand.h"` replaces `plotrequests.h` (`:33`);
  - `CalculationDemand::isMerelyUncomputed(...)` at `:538`;
  - rewrite the comment `:531-537`: "Silently absent when it is merely uncomputed: waiting on a requested calculation, or rejected by one. The plot list's row reports both (working indicator, warning badge) ...".
- `src/profilestatebridge.cpp`: no change. `applyProfile` reaches the demand layer through `PlotModel::setPlotEnabled` like any other check change.

**Acceptance Criteria:**
- [ ] `grep -rn "PlotRequests\|plotrequests\|plotRequests" src` returns nothing.
- [ ] The application builds (`build-phase1`). At start-up with checked plots and a populated logbook, no job is created before a session is shown (manual check M-P1-1 below).
- [ ] The demand layer is deleted before the executor, both in `~MainWindow` and after `closeEvent`.

**Complexity:** M

---

### Task 1.5: Plot rows lose refresh and cancel; show the demand state

**Purpose:** Remove every gesture and control from the plot list's row delegate. Paint the per-plot state: a static working indicator with "k of n" while working, or the warning badge with the failed count. Show the tooltip from the state.

**Files to modify:**
- `src/ui/docks/plotselection/PlotRowDelegate.h` / `.cpp`
- `src/ui/docks/plotselection/PlotRowLayout.h`

**Technical Approach:**

1. **`PlotRowLayout.h`.** Switch to indicator vocabulary and drop the hit rectangle.
   - The signature becomes `layoutPlotRow(itemRect, metrics, bool showsWarning, int warningCountWidth, bool showsIndicator, int progressLabelWidth, Qt::LayoutDirection)`.
   - `PlotRowGeometry` holds `warningIcon`, `warningCount`, `progressLabel`, `indicatorIcon` and `clusterWidth`. **`controlHit` is removed**: nothing in the cluster is clickable.
   - The geometry arithmetic, the right-to-left mirroring and the "label width without an indicator is ignored" rule are unchanged, so every number in the existing layout tests still holds.
   - Rewrite the top comment:
     - `[check] plot name (elided) ... [warn][failedCount] [label][indicator] |`;
     - "The indicator occupies the right-most slot";
     - "WHAT is shown is decided by DemandState (calculationdemand.h)";
     - the warning group and the indicator are never both shown by the delegate (spec §10), but the function lays out both.
2. **`PlotRowDelegate`**:
   - The constructor becomes `PlotRowDelegate(CalculationDemand *demand, QAbstractItemView *view)` and connects `plotStateChanged` to `onPlotStateChanged` (the old `onRowStateChanged`).
   - `stateFor()` returns `DemandState`: the default without a component or for a category.
   - **Delete** `editorEvent()`, `m_pressedIndex`, `m_pressedControl`, `drawRefreshGlyph` and `drawCancelGlyph`. The base class handles every event, so the check box, Space and selection behave exactly as `QStyledItemDelegate`.
   - `controlRect()` becomes `QRect clusterRect(const QModelIndex &) const`, for tests only: the union of the painted cluster rects (indicator and label, or badge and count), null for plain rows.
   - Painting (`paint()`, `:205-261`):
     - `isPlain()`: base class.
     - Otherwise: elide the name as today. Then, if `isWorking()`, draw `drawWorkingGlyph(painter, indicatorIcon, color, 0.0)` and `progressLabel`. Else, if `showsWarning()`, draw the badge and `failedCount`, as today.
     - `drawWorkingGlyph(QPainter*, const QRectF&, const QColor&, qreal rotationDegrees)` draws an open arc of 270° in the text colour, with the pen width of `glyphPenWidth`. It is static in this phase (rotation 0). Phase 3 animates the rotation.
   - `toolTipFor()` / `helpEvent()`: show `DemandState::toolTip` over the whole row, unchanged in mechanics.
   - Rewrite the class comment (`:17-63`):
     - it paints what `CalculationDemand` reports and decides nothing;
     - PAINTING: working indicator and "k of n" while any demand is waiting or running; the warning badge with the failed count once work is finished and some sessions could not be computed; plain rows are the base class's; the row height never changes;
     - NO GESTURES: the delegate handles no event of its own; checking a row is the base class's write to `PlotModel`, which the demand layer observes like every other check change (the Plots menu, a profile, the start-up restore);
     - TOOLTIP;
     - REPAINT: `plotStateChanged`; no timer in this phase;
     - without a component, the delegate behaves exactly as `QStyledItemDelegate`.

**Acceptance Criteria:**
- [ ] `PlotRowDelegate` no longer overrides `editorEvent`, and `src/` contains no `plotCheckedByUser`, `refreshPressed` or `cancelPressed`.
- [ ] Plain rows are pixel-identical to the base delegate.
- [ ] A working row paints the indicator and "k of n"; a finished row with failures paints the badge. The two never appear together.
- [ ] A click anywhere in the cluster behaves exactly as a click on a base-delegate row: no job created or cancelled, no check toggled.
- [ ] `PlotRowLayout.h` has no `controlHit`, and still includes no widget header (widget-free-core audit).

**Complexity:** M

---

### Task 1.6: Test support fixtures

**Purpose:** Give the tests the vocabulary of the new executor and demand layer.

**Files to modify:**
- `tests/support/jobfixture.h/.cpp`:
  - rewrite the header comment: "tests of the executor, the job model, and whatever sits on top of them";
  - add `bool waitStarted(FlySight::JobQueue &executor, FlySight::JobId job, int timeoutMs = 5000);`, which spins the event loop until the job is no longer `Queued`, for tests without a gate;
  - `waitIdle` and `Quiet` are unchanged. `Quiet` still watches `jobQueued`, which is emitted for an accepted offer.
- `tests/support/plotfixture.h/.cpp`:
  - `#include "calculationdemand.h"` replaces `plotrequests.h`;
  - `spin(FlySight::CalculationDemand *demand)`: two event-loop turns, then `flush()`;
  - `sessionIdsOf(const QList<FlySight::DemandTrack> &)`;
  - add a bridge `plotX`: input `X_OUT`, output `Syn/x` = {X_OUT}, behind `exhausted`. Add it to the table and to `plots()`, which now returns **eight** plots in table order ending with `x`. The comment's literal line gains `Syn/x {5}` for `X_IN = 4`, on the second run.
  - add `bool waitDemandIdle(FlySight::JobQueue &executor, FlySight::CalculationDemand &demand, int timeoutMs = 5000)`. On every poll it calls `demand.flush()`. It returns true once `executor.isIdle() && !demand.hasPendingUpdate() && !demand.hasSettlingSessions()`.
  - Replace "plot request logic" with "demand layer" in the comments.

**Acceptance Criteria:**
- [ ] The support library builds, and no support file names `PlotRequests`.
- [ ] `PlotFixture::plots().size() == 8`, and `plotX` is unregistered by the destructor like the others.

**Complexity:** S

---

### Task 1.7: Executor tests: `tst_jobqueue`, `tst_jobmodel`, `tst_fusion_jobs`, `tst_result_store`, `tst_result_columns`

**Purpose:** Prove the executor contract and rewrite every test that relied on queueing several jobs at once, on FIFO order or on pruning.

**Files to modify:**
- `tests/tst_jobqueue.cpp`:
  - `using Kind = JobQueue::OfferResult::Kind;`;
  - every `request(` becomes `offer(`;
  - header comment: "The executor ...".

  **Rule used throughout:** a second job exists only as the chosen next job, offered after the first has started (after `gate().waitEntered()`, or `waitStarted()`). Per function:

  | Function | Change |
  |---|---|
  | `runsAndPublishes` | Also assert `chosenNextJob() == 1` before the start and `0` after. |
  | `publishesInvalidationsThroughSessionModel` | Also record `m_queue->publishingJob()` inside the `dependencyChanged` slot (expect `1`) and assert it is `0` afterwards. |
  | `workerIsNotMainThreadAndHasLargeStack` | Rename only. |
  | `duplicateRequestsCreateNoDuplicates` → **`duplicateOffersCreateNoDuplicates`** | Equal offer while queued → `AlreadyActive`. Equal while running → `AlreadyActive`, and the chosen next job is untouched. After start: offer `s2 gated` → `Created` (chosen next); offer `s1 expA` → `Created`, replacing it (`s2 gated` ends `Cancelled` "No longer needed", never started, `s2` unpinned); offer `s1 expA` again → `AlreadyActive`. 3 rows, `maxRunning == 1`. |
  | `oneAtATimeInRequestOrder` → **`oneAtATimeInOfferOrder`** | Offer each next job while the previous one runs (after `waitEntered`). At every step `activeJobs() == {running, chosenNext}`. Start order = offer order; `maxRunning == 1`; `startedAt(i) >= finishedAt(i-1)`. |
  | `refusesMissingInput` | Add: while a gated job runs and a chosen next job exists, a `MissingInput` offer leaves `chosenNextJob()` unchanged. |
  | `refusesUnloadedAndUnknownSession`, `refusesBlockedAndDone` | Rename only (`offer`). |
  | `neverLoadsASession` | Remove the `cancelSession`, `cancelUnwantedQueued` and `cancelAll` calls. Offer `s2 expA` after `waitEntered`; use `withdrawChosenNext()` and `cancel()` instead. |
  | `inputChangeWhileQueuedSupersedesAtStart` | Split into three sequential rounds (missing input; blocked by an input change; already valid via a synchronous `engine.request`). Each round holds a gated job, offers the case's job as chosen next, applies the change, opens the gate, and asserts the reason at start with no worker. Then offer `thrower` last. `startedSpy` contains only the held jobs and the last one. |
  | `registrationRemovedSupersedes` | Running `gated` plus chosen next `thrower`. After unregistering both, both end `Superseded` "Calculation is no longer registered"; then offer `s3 expA` → `Succeeded`. |
  | `staleRunningJobIsStoppedAtOnce` | Offer `next` after `waitEntered`. |
  | `requestWhileStaleJobWindsDown` → **`offerWhileStaleJobWindsDown`** | Same semantics: an offer of the same key while the stale job winds down creates the chosen next job behind it; offering again → `AlreadyActive`. |
  | `staleJobThatReturnsAResultIsStillSuperseded`, `registrationRemovedStopsRunningJobAtOnce`, `modelDestroyedStopsRunningJobAtOnce`, `userCancelThenStaleEndsCancelled`, `rejectionSucceedsWithReason`, `exceptionSucceedsAsFailedResult`, `resourceExhaustionFails`, `cancelIgnoredForOneStepStillCancelled`, `cancelFromRowsInsertedLeavesNoPin`, `sessionDataReplacedWithRunningJobSupersedes`, `mergeIntoSessionWithRunningJobSupersedes`, `idleSchedulerKeepsWorking` | Rename only. |
  | `staleThenUserCancelEndsSuperseded` | Replace `cancelAll() == 0` with a second `cancel(id)` (true) and `cancelSpy.count() == 1`. |
  | `workerStartFailureFails` | Offer `first`, wait idle (`Failed`), then offer `second` (`Succeeded`), then retry `first` (`Succeeded`). |
  | `cancelRunningThenNextStarts`, `cancelQueued`, `removeSessionWithQueuedJob`, `removeSessionWithRunningJob`, `evictionDeferredWhileJobActive`, `repopulateWithJobs`, `sortWhileRunningStillPublishes`, `queueDestroyedBeforeModel`, `modelDestroyedBeforeQueue` | Offer the second job after `waitEntered`. |
  | `requestWhileCancellingCreatesNewJob` → `offerWhileCancellingCreatesNewJob` | Rename. |
  | `cancelSessionAndCancelAll`, `cancelUnwantedQueuedSparesRunning` | **Delete** (API removed). |
  | `shutdownWithQueuedAndRunning` | Running plus one chosen next job (drop `queued3`). Both end `Cancelled` "Application closing". An offer afterwards → `ShuttingDown`. 2 rows. |
  | `shutdownIsIdempotentAndRefusesRequests` → `shutdownIsIdempotentAndRefusesOffers` | `withdrawChosenNext() == false` replaces `cancelAll() == 0`. |
  | `shutdownFromSlots` | First block: offer `s2 expA` from a `jobStarted` slot of the first job, so it is chosen next when `jobFinished` shuts the executor down. Second block unchanged. |

  **New test functions:**
  - **`holdsAtMostRunningAndChosenNext`**: a `jobsChanged` slot records the maximum number of active records and of `Queued` records through a scripted sequence (offer, replace, withdraw, cancel, start, end, input change). The maxima are 2 and 1. Also assert `JobQueue::kMaxRunningJobs == 1`.
  - **`offerReplacesChosenNext`**:
    - With a held running job: offer A, then B. A ends `Cancelled` "No longer needed" with `startedAt` invalid, and its session is unpinned. B is chosen next. Offering B again → `AlreadyActive`.
    - With nothing running: offer A, then B in the same event-loop turn. A is `Cancelled`, B runs, and `idle` is emitted exactly once, at the very end.
  - **`withdrawEndsChosenNext`**: with none → `false`. With one → `true`, `Cancelled` "No longer needed", never started, unpinned; the running job is untouched. `idle()` is emitted when nothing else runs.
  - **`workerRunsBelowNormalPriority`**: a probe calculation (pattern of `workerIsNotMainThreadAndHasLargeStack`, `:347-388`) stores `QThread::currentThread()->priority()` in an atomic. Expect `QThread::LowPriority`.
  - **`mainThreadIsNotBlockedByARunningJob`**: while a gated job is held inside compute, a repeating 0-interval `QTimer` on the main thread fires at least 50 times (`QTRY_VERIFY`), and `SessionModel::updateAttribute` on another session returns at once.

- `tests/tst_jobmodel.cpp`:
  - the helper `request()` becomes `offer()`;
  - `cancelledJob()` is unchanged, and is only called when no chosen next job is pending.

  | Function | Change |
  |---|---|
  | `historyFromSignalsAlone` | Step 2: offer `rejection`, wait idle, offer `exception`, wait idle. Add a step 10, "replaced": with a gated job held, offer `s2 expA`, then `s3 expA`. The first ends `{Q, kCancelled}` with reason "No longer needed". Row count 11. Outcome check `QStringList({"Cancelled", "No longer needed"})`. |
  | `neverMoreThanOneRunningRow` | Offer the next gated job after each `waitEntered`. |
  | `timestampsAreOrdered` | After the first job is entered: `neverRan = cancelledJob(...)`, then `second = offer(...)`. |
  | `removeFinishedAndClear` | Order: `a`, `active`, then (after `waitEntered`) `b`, `c`, `d` via `cancelledJob`, and `queued` last. Updated `rowOf` expectations: `a` 0, `active` 1, `c` 2, `d` 3, `queued` 4 after removing `b`. `clearFinished()` still returns 3. |
  | `removeRowsRefusesActive` | Offer `queued` after `waitEntered`. |
  | `nothingIsPersisted` | Sequential: success (gate), rejection, failure, then cancelled (gate, `cancel`). Same outcomes and records. |
  | `rolesAndColumns`, `progressIsItsOwnSignal`, `cancelRequestedIsVisible`, `retentionBound` | Rename only. |

- `tests/tst_fusion_jobs.cpp`:
  - alias `JobQueue::OfferResult::Kind`, and `RequestResult` becomes `OfferResult`;
  - every `request(` becomes `offer(`.
  - `cancelDuringFitThenNextJobStarts`: offer `s2` inside `onFirstProgress(job1)`, before `cancel(job1)`, so it is the chosen next job.
  - `shutdownDuringFit`: offer `s2` inside `onFirstProgress(running)`, before `shutdown()`.
  - Header comment: "the executor".
- `tests/tst_result_store.cpp` (`:78`, `:523`, `:588`) and `tests/tst_result_columns.cpp` (`:458`): `OfferResult` and `offer(`. Single jobs only, so there is no other change.

**Acceptance Criteria:**
- [ ] Every function listed above compiles against the new API and passes.
- [ ] The new functions `holdsAtMostRunningAndChosenNext`, `offerReplacesChosenNext`, `withdrawEndsChosenNext`, `workerRunsBelowNormalPriority` and `mainThreadIsNotBlockedByARunningJob` exist and pass.
- [ ] No test file calls `request(` on a `JobQueue`.

**Complexity:** L

---

### Task 1.8: Demand tests: `tst_calculation_demand` (replaces `tst_plot_requests`)

**Purpose:** Prove plot demand, priority, the settle wait, failure and not-applicable memory, per-plot state and the one-way flow, without widgets. This covers spec §13's plot rows.

**Files to create:**
- `tests/tst_calculation_demand.cpp`.

**Files to delete:**
- `tests/tst_plot_requests.cpp`.

**Files to modify:**
- `tests/CMakeLists.txt` (`:145-146`):
  - `flysight_add_test(tst_calculation_demand SOURCES tst_calculation_demand.cpp)` with the comment "Demand layer (plot demand, priority, the input-settle wait, failure memory, per-plot state; no widgets)";
  - the `tst_jobqueue` comment (`:141`) becomes "Executor (job queue) and job model".

**Technical Approach:**

**Fixture.** Follow `tst_plot_requests.cpp:191-261`:
- `init()`:
  - the same four sessions `s1`–`s4` named "Jump 1"–"Jump 4", loaded and hidden;
  - also call `LogbookManager::instance().initialize()` and `QVERIFY(waitForIdle(*m_model))` after the merge, as `tst_result_store.cpp:392-407` does, so that stored records can be asserted;
  - `m_demand = std::make_unique<CalculationDemand>(...)`.
- Helpers:
  - `row(id)` = `flush()` + `plotState(id)`;
  - `settle()` = `endInputSettleWaits()` + `flush()`;
  - `spin()` = `PlotFixture::spin(m_demand.get())`;
  - `restartDemand()`: reset and re-create the demand layer over the same model and executor;
  - `stored(id, calc)` = `LogbookManager::instance().knownCalculationRecords(id).contains(calc)`.
- `cleanup()`: as today; destroy the demand layer before the executor.

A test that needs a result without the demand layer uses `CalculationEngine::request()` synchronously (open the gate first for `gated`). It must not offer to the executor while plots are checked.

**Functions**:

| Function | What it proves |
|---|---|
| `plotIdMatchesPlotModelRole` | As before, with **8** plots. |
| `ordinaryPlotsAreNeverInspected` | `Syn/plain` checked: no run, no announcement, default state, no job. |
| `uncheckedPlotsAreNeverInspected` | As before; `G_OUT` is published through a synchronous engine request. |
| `hiddenAndStubRowsAreNotTracks` | Only `s3` is wanted (running, held); `s4` has no input; `s1` is not loaded and `s2`'s LRU position is untouched across passes; no load. |
| `failedLoadPlaceholderIsNotATrack` | Checking `g` starts `s2` only; no engine is created for the placeholder. |
| `rowScript` (items 115, 306) | See the script below. |
| `chainedBlockersContinue` (item 113) | Check `db`: `expA` then `expB` with no other call; `idle` emitted once; every announced state until the last is working with "0 of 1"; the last is plain. |
| `heldChainContinues` | `h`: `gated` held (track Running, `job` = its id), then `afterG` automatically; `Syn/h` {6}. |
| `chainCompletesAfterFirstJobDoesNotSucceed_data/()` | Rows "cancelled from outside" (`m_queue->cancel(first)`: offered again at once, a new job) and "superseded" (input edit: waits to settle, then offered). The chain completes; `jobCount("afterG") == 1`; final value 6 or 9. |
| `chainStopsForHiddenTrackOrUncheckedPlot_data/()` | The first link finishes (`Succeeded`, stored). `afterG` is not started while hidden or unchecked. Shown or checked again, `afterG` starts with no other action. |
| `programmaticCheckCreatesDemand` (item 116) | `setPlotEnabled`, `togglePlot` and `setData(CheckStateRole)` each create the same demand: jobs for the visible sessions. Unchecking in between drops the waiting pair. |
| `profileStyleApplyCreatesDemand` (item 116) | The `applyProfile` loop (`setPlotEnabled` over all plots) creates demand for `g`, `g2`, `db`, `h` over `s1`, `s2`; everything completes; `ea` stays default. |
| `startupRestoreWithHiddenSessionsStartsNothing` (item 116) | All sessions hidden; `g` restored checked from settings, with the demand layer created before the plots and then after; no job (`Quiet`), plain state. Then `show({"s1"})` starts `s1` with no other action. |
| `showingASessionStartsIt` | `g` checked. Show `s1` → running. Show `s2` while `s1` runs → it becomes the chosen next job with no other call. Both complete. |
| `hidingASessionDropsItsWaitingPair` | `s1` running, `s2` chosen next. Hide `s2` → its job ends `Cancelled` "No longer needed" at once, before any event loop turn. Hide `s1` → the running job is not asked to stop, finishes and is stored. |
| `uncheckingDropsWaitingPairsKeepsRunning` | Three visible sessions: uncheck → the chosen next job `Cancelled`; the running job finishes `Succeeded` and `stored("s1","gated")`; the state is default. |
| `waitingPairNeededByAnotherPlotSurvives` | `g` and `g2` checked. Uncheck `g` → the chosen next job survives (`g2` needs it). Uncheck `g2` → withdrawn. |
| `loadingAVisibleSessionStartsIt` | A visible stub, once loaded by the model, starts. |
| `mergeCreatesDemandForShownSessions` | A merge into `s1` with an irrelevant key does not start a settle wait. A new session `s5`, shown, is started. |
| `inputBurstRunsOneJob` | `QCOMPARE(CalculationDemand::kInputSettleMs, 1000)`. After `g` on `s1` has completed, `setInputSettleDelay(60000)`; three `G_IN` edits separated by `spin()`. After the first edit the track is Waiting with `settling`, `isWorking()`, and there is no job (`Quiet`). `settle()` → exactly one new job; its value is the last input + 1. |
| `staleRunningJobIsWaitingAtOnce` (item 108) | An edit during a held run: the running job is asked to stop; the track is Waiting and settling at once, before the worker returns. No new job until settled. Then one job with the new value. |
| `supersededJobIsRunAgainAfterInputsSettle` | After the stale job ends `Superseded`, nothing is offered during the wait; after `settle()` it runs. |
| `sessionWithoutInputIsNeverListed` (item 111) | `s3` has no `G_IN`: it never appears in `running`, `waiting` or `failed`, and is never offered. At idle, `m_queue->offer("s3","gated")` → `MissingInput`. |
| `onlyRequestableCalculationsAreOffered` | `s3` already computed and `s4` rejected (synchronous engine requests); `s2` has no input. Checking `ea` offers `s1` only (`jobQueued` once). |
| `resultAppearingWhileWaitingDropsThePair` | Spec §6, "the result appeared by other means". Setup: `s1` gets `G_IN = 4` and no `EA_IN`; `s2` gets `EA_IN = 4` and no `G_IN`. Show both and check `g` and `ea`. `s1`'s `gated` job runs (held in the gate). After `flush()`, `s2`'s `expA` job is the chosen next job, and `ea`'s state has `s2` Waiting with `job` = that id. Then `QCOMPARE(engine("s2").request("expA").status, ResultStatus::Ok)` installs `s2`'s result without the demand layer or the executor; it is ungated and does not touch the held worker. Open the gate. `s1`'s end runs the synchronous pass, which finds `s2`'s blocker report Available and withdraws the chosen next job: it ends `Cancelled` "No longer needed" with `startedAt` invalid. `waitDemandIdle` holds. Afterwards: `engine("s2").runCount("expA") == 1` (only the synchronous run); no job for `s2` ran (`jobStarted` never names it); `s2` is unpinned; and `row("Syn/ea")` has `s2` Done (`doneCount == wantedCount == 1`, plain). |
| `inputDeterminedFailureIsStoredBadgedNeverRerun` | `EA_IN = -1`: a rejection, `Succeeded`, `stored("s1","expA")`; `failedCount 1`, `showsWarning()`, tooltip `"Could not be computed:\n  Jump 1 - Explicit A: negative input"`. `spin()` ×3 → no new job. Input change → settle → offered → Available. Also `thrower`: an engine-cached failed result, badge "Thrower: synthetic failure", not re-offered in the run, no record written. |
| `jobLevelFailureIsBadgedNotRerunUntilRestart_data/()` | Rows "worker start failure" (`failNextWorkerStarts(1)`, plot `ea`) and "out of memory" (plot `x`, the `exhausted` bridge). The job ends `Failed`; the track is Failed with `jobFailure`; the badge reason is `"Explicit A: The worker thread could not be started"` / `"Exhausted: Out of memory"`; `spin()` ×3 → no new job; nothing stored. `restartDemand()` → offered again → `Succeeded`, badge gone. |
| `failuresListedWhileWorking` | `s1` rejected, `s2` waiting behind another plot's held job: `isWorking()`, `failedCount 1`, `!showsWarning()`, and the tooltip lists "Computing: 1 of 2 done" and the failure. After the work: `showsWarning()`. |
| `merelyUncomputedIsNotWorthAWarning` (item 116) | As before, with the executor requests replaced by synchronous engine requests. |
| `sharedJobSameProgress` | `g` and `g2` share the jobs: equal counts, `progressLabel`, running `progressText` and tooltip. |
| `plotCheckedDuringAJobJoinsIt` | `h` checked while `gated` runs for `g`: `h`'s track is Running on the same job; `afterG` follows automatically. |
| `focusedSessionFirstThenRowOrder` | `G_IN` = 1..4 on `s1`..`s4`, all visible, focus `s3`, check `g`. `gate().startOrder() == {3, 1, 2, 4}`. |
| `changingDemandReplacesChosenNext` | `s3` running (focused), `s1` chosen next. Focus `s4` → the chosen next job is `s4`'s; `s1`'s ends `Cancelled` "No longer needed". |
| `executorHoldsAtMostRunningAndChosenNext` | Over the whole `focusedSessionFirstThenRowOrder` flow, a `jobsChanged` spy shows at most 2 active records and at most 1 `Queued`. |
| `tooltipText` | The live tooltip, plus the pure builder on hand-built states (running with no progress text → "running"; several titles; job-level failure). |
| `changeSignalsAreMinimal` | Signals only for requested plots; a pass that changes nothing announces nothing; unchecking resets once; `Syn/plain` is never announced. |
| `dependencyBurstIsCoalesced` | Irrelevant names schedule nothing and start no wait; a relevant burst in one event-loop turn causes exactly one pass; the name is read live. |
| `progressUpdatesWithoutInspection` | As before, on `running` tracks and the new tooltip format. |
| `removedSessionLeavesNoTrace` | Removing the chosen next session → the executor supersedes it and the demand layer offers the next; removing the running session → abandoned; the memory of removed ids is forgotten. |
| `registryChangeReclassifies` | `Syn/rx` becomes requested on registration and is then started (`regX` is not gated); unregistering → default state, withdrawn or no job; re-registering → demand again. |
| `survivesExecutorShutdown` | After `shutdown()` the tracks show waiting and nothing is offered (`Quiet`) through check, uncheck, show and hide; after the executor is destroyed the component is inert (default states). |
| `nullCollaborators` | Every null combination is inert: default state, `flush` safe, no job. |

**The `rowScript` script:**
1. `G_IN = 4` on all four sessions; show `s1`–`s3`; check `g`. After `waitEntered`, `s1` is running. After `flush`, `s2` is the chosen next job and `activeJobs().size() == 2`. The state is: `wanted 3`, `done 0`, `running 1`, `waiting 2`, `progressLabel "0 of 3"`, tooltip `"Computing: 0 of 3 done\n  Jump 1 - Gated: step 1"`.
2. Open the gate once. `s1` is `Succeeded` and stored, `s2` runs, `s3` is chosen next, and the state is "1 of 3".
3. Uncheck `g`. `s3`'s job ends `Cancelled` "No longer needed" at once; `s2` is not asked to stop. The state is default. Open the gate: `s2` is `Succeeded` and stored.
4. Check `g` again. `s3` starts with no other call.
5. Show `s4` while `s3` runs. It becomes the chosen next job with no other call. Open 2: plain.
6. The job history is `S, S, C, S, S`, and `maxRunning == 1`.

**Acceptance Criteria:**
- [ ] `tests/tst_plot_requests.cpp` is gone, and `tst_calculation_demand` is registered and passes.
- [ ] Every function above exists as `CalculationDemandTest::<name>()`.
- [ ] Every spec §13 plot row is covered:
  - check starts: `rowScript`;
  - show starts: `showingASessionStartsIt`;
  - hide drops: `hidingASessionDropsItsWaitingPair`;
  - uncheck drops, running finishes stored: `uncheckingDropsWaitingPairsKeepsRunning`;
  - a result that appears by other means while its pair waits drops the pair before it starts: `resultAppearingWhileWaitingDropsThePair`;
  - burst → one job, indicator from the first change: `inputBurstRunsOneJob`;
  - input-determined failure: `inputDeterminedFailureIsStoredBadgedNeverRerun`;
  - job-level failure: `jobLevelFailureIsBadgedNotRerunUntilRestart`;
  - profile: `profileStyleApplyCreatesDemand`;
  - start-up: `startupRestoreWithHiddenSessionsStartsNothing`;
  - focus then row order: `focusedSessionFirstThenRowOrder`;
  - executor bound and replacement: `executorHoldsAtMostRunningAndChosenNext`, `changingDemandReplacesChosenNext`.

**Complexity:** L

---

### Task 1.9: Widget and layout tests: `tst_plot_row_delegate`, `tst_plot_row_layout`

**Purpose:** Prove the delegate paints the demand state, handles no gesture, and leaves plain rows untouched.

**Files to modify:**
- `tests/tst_plot_row_delegate.cpp`. `init()` is unchanged: `s1`, `s2` visible with `G_IN 4`; `s3` hidden. `m_requests` becomes `m_demand`, and the helpers `row()` and `spin()` use it. Remove `makeRefreshRow`; add `makeWorkingRow()`: check `g`, wait for `s1` to enter the gate, flush; the row is working with "0 of 2".

  | Old | New |
  |---|---|
  | `plainRowsAreIdenticalToBaseDelegate` | Same name; `clusterRect` replaces `controlRect`. |
  | `missingRowPaintsControl` | **`workingRowPaintsIndicator`**: the indicator differs from the base delegate inside `clusterRect`; the check box and other rows are identical; the row height is unchanged. |
  | `longNameIsElidedNotTheCluster` | Same name; rows `g` and `g2` both working with identical clusters. |
  | `checkBoxClickIsGesture` | **`checkBoxClickChecksThroughTheModel`** (item 116): a click on the check box writes `Checked` to the model and `s1`, `s2` are started by the demand layer. A `setPlotEnabled` of another row gives the same result: the delegate adds no path of its own. |
  | `spaceKeyIsGesture` | **`spaceKeyChecksThroughTheModel`**. |
  | `uncheckIsNotAGesture` | **`uncheckByClickDropsWaitingWork`**: `s2`'s chosen next job is `Cancelled`; `s1` finishes. |
  | `programmaticCheckStartsNothingWithViewAttached` | **`programmaticCheckIsTheSameAsAClick`** (item 116): each of the three programmatic paths creates the same jobs as a click, with the view attached. |
  | `startupStyleRestoreStartsNothingWithViewAttached` | **`startupRestoreWithHiddenSessionsStartsNothingWithViewAttached`** (item 116): hide `s1`, `s2` first; the settings restore checks `g`; no job and a plain row; showing `s1` starts it. |
  | `refreshClickRequests`, `cancelClickCancelsAndLeavesChecked`, `controlClickDoesNotToggleOrSelect`, `pressInsideReleaseOutsideDoesNothing`, `pressOutsideReleaseInsideDoesNothing`, `rightClickDoesNothing`, `doubleClickOnRefreshRequestsOnceAndCancelsNothing` | **Delete.** |
  | `clickOnLabelOrBadgeDoesNothing` | **`clickOnClusterIsAClickOnTheRow`**: left, right and double clicks over the working cluster and over a badge row's cluster create or cancel no job and toggle no check. The current index and selection equal what the base delegate produces for the same clicks. |
  | `toolTipComesFromRowState` | **`toolTipComesFromPlotState`**: the working-row tooltip contains "Computing: 0 of 2 done" and "Jump 1 - Gated: step 1"; the failed-row tooltip contains "Explicit A: negative input"; plain rows, categories and invalid indexes are empty; `helpEvent` shows it over the whole row. |
  | `rowStateChangeRepaintsRow` | **`plotStateChangeRepaintsRow`**: showing `s3` → one `plotStateChanged("Syn/g")`, `wantedCount 3`, a repaint, and no `PlotModel` change. |
  | `survivesRequestsDestroyedFirst` | **`survivesDemandDestroyedFirst`**. |

- `tests/tst_plot_row_layout.cpp`: rename `rectsOf` fields (no `controlHit`).

  | Old | New |
  |---|---|
  | `controlOnly` | `indicatorOnly` (drop the `controlHit` line) |
  | `controlAndWarning` | `indicatorAndWarning` |
  | `warningOnly` | Drop the `controlHit.isNull()` line. |
  | `emptyLabelOmitsItsSpacing`, `rightToLeftIsMirrorImage_data/()` | Renamed fields and columns. |
  | `hitRectSpansRowHeightToRightEdge` | **Delete.** |

  Header comments of both files: replace `PlotRequests` with `CalculationDemand`, and drop "gesture".

**Acceptance Criteria:**
- [ ] Both widget-free and widget tests pass (`FLYSIGHT_BUILD_WIDGET_TESTS=ON`).
- [ ] No test file mentions `controlRect`, `Control::` or `PlotRowState`.

**Complexity:** M

---

### Task 1.10: Fusion row and store tests: `tst_fusion_rows`, `tst_fusion_store`

**Purpose:** Replace gestures with demand in the real-fit suites, and keep their stored-results guarantees ("restoring is not requesting").

**Files to modify:**
- `tests/tst_fusion_rows.cpp`:
  - `m_demand` replaces `m_requests`;
  - `row()` = `flush()` + `plotState()`;
  - `waitDemandIdle(*m_queue, *m_demand, kFitTimeoutMs)` replaces `waitIdle` wherever demand should start work;
  - `offenceInRows` uses `running + waiting + failed` and the counts;
  - header comment: "PlotModel + CalculationDemand + the executor ...".

  | Function | Change |
  |---|---|
  | `allSeventeenFusionPlotsAreExplicitBacked` | All 17 plots are `requested`; with `s2` visible, checking them starts **one** fit shared by every row (each state is working with the same job); local-frame plots are default; after `waitDemandIdle`, all plain; `runCount == 1`. |
  | `realRowScript` | The rowScript of Task 1.8 with real fits: check `roll` → three fits in sequence with no call; on the first progress of `s2`'s fit, uncheck → `s3`'s chosen next job `Cancelled` "No longer needed", `s2` not asked to stop; check again → `s3` runs; show `s4` → runs. History `S, S, C, S, S`; goldens for `s1`–`s4`; the `seen` log shows `doneCount` rising on each end. |
  | `rollPitchYawShareOneJob` | Checking all three starts one fit; during it, all three states are running with the same progress text. |
  | `accHRowIsBlockedByFusion` | Check `accH` → the fit starts automatically; at idle, `offer("s2", kAccH)` is not created. |
  | `noImuSessionIsNeverCounted` | `n1` is never listed or counted; `s2`'s fit starts automatically; `offer("n1", kFit)` → `MissingInput`. |
  | `rejectedTrackShowsBadge` | The auto-started fit rejects; badge and reason; `spin()` → no new job. Origin edit → `endInputSettleWaits()` → the fit reruns → golden. |
  | `editsAndVisibilityDuringFit` | During `s1`'s fit: edit `s2`'s `_DESCRIPTION` (irrelevant: no wait), hide and show `s3`. `s1` is undisturbed (golden). **Afterwards `s2` and `s3` are also computed with no other action** (three `Succeeded` jobs). The edit is saved. |

- `tests/tst_fusion_store.cpp`:
  - `m_demand` replaces `m_requests`, in `init()`, `cleanup()` and `restart()` (a new demand layer after a restart);
  - `row()` = `flush()` + `plotState()`;
  - `Kind` becomes `OfferResult::Kind`, and `request(` becomes `offer(`;
  - where a test checks `roll` with `a` visible, **no direct offer**: `waitDemandIdle` instead.

  | Function | Change |
  |---|---|
  | `restoredAfterEvictionIsBitIdentical` | The fit is started by demand; `state.isPlain()` stays; drop the `controlCount` line. |
  | `restoredAfterRestartIsBitIdentical` | Unchanged apart from `row()`. It is the "restoring is not requesting" check: `Quiet` holds after showing `a` with `roll` checked. |
  | `restoredRejectionShowsBadge`, `restoredSolverFailureShowsBadge` | Demand starts the fit; `showsWarning()`; no rerun after `spin()` (`Quiet`). |
  | `dependencyEditDropsRecord` | `setInputSettleDelay(60000)` before the edit. After it: `waitingCount == 1` with `settling`; `Quiet` holds through `spin()`. Then uncheck `roll` (nothing to drop). The rest is unchanged. |
  | `codeStampChangeDropsRecordOnLoad`, `lookupResolvingDifferentlyAtLoadDeletesFit`, `deletedCacheFolderReadsNotRequested` | After `show({"a"})`, `row(kRoll)` shows `waitingCount == 1`, and exactly one job was offered for `a`'s fit. `check("roll", false)` ends it `Cancelled` "No longer needed" before it started. `runCount == 0`; no other job. Replace `Quiet` with these assertions. |
  | `runtimeRegistryChangeDropsFitAndRecord` | The first fit is started by demand. After the provider removal: `waitingCount == 1`. Uncheck `roll` (drops the offered job, if any) before `spin()`. `runCount` stays 0 after reload. |
  | `unrelatedEditKeepsRecord`, `mergeIntoLoadedSessionDropsRecord`, `mergeIntoUnloadedSession`, `storedFitSurvivesUnrelatedChanges`, `sessionFileBytesUnaffectedByRecord`, `fittedBeforeFirstSaveIsRestored` | `offer(` renames only. Where `roll` is not checked, direct offers stay legal (the demand layer withdraws only its own offers). |

**Acceptance Criteria:**
- [ ] `tst_fusion_rows` and `tst_fusion_store` pass (label `fusion`), with every function name kept. The acceptance map lines citing them stay valid.
- [ ] No fusion test calls a gesture or `request(`.

**Complexity:** L

---

### Task 1.11: Audit rules and acceptance map

**Purpose:** `audit_cleanup` passes after this phase. The rules that `PlotRequests` or the queue broke are rewritten, and every map line points at an existing function.

**Files to modify:**
- `tests/audit/cleanup_audit.cmake`:
  - Header (`:11-15`): "gestures only from the plot list's row delegate" becomes "work started only by the demand layer, through the executor".
  - `FUSION_CORE` (`:266`): `"src/jobqueue.*" "src/jobmodel.*" "src/calculationdemand.*" src/fusion`, with the comment "the executor, its model, the demand layer, and the fusion library".
  - "jobs never touch the idle scheduler" (`:289`): the pathspec becomes `"src/jobqueue.*" "src/jobmodel.*" src/fusion`, with the comment "The executor and the kernel never touch the idle scheduler". Phase 2's demand layer registers a scheduler task (the column fill).
  - solver-confinement "the kernel is pure" (`:326`): `plotrequests` becomes `calculationdemand`. "nobody but the application references the fusion library" (`:332`): `"src/plotrequests.*"` becomes `"src/calculationdemand.*"`.
  - `gestures` group (`:366-404`). **Keep the slug** (the map cites it for items 116 and 306); rewrite the heading comment: "Only the demand layer starts requested calculations, and it derives what to start from what is switched on; nothing is a gesture". Rules:

    ```cmake
    expect_none("no gesture entry points" "plotCheckedByUser|refreshPressed|cancelPressed"
      src tests ":!tests/README.md")
    # prepare/publish rule: unchanged
    expect_only("no reader requests" "[.>]request\\(" "^src/engine/" src)
    # "no synchronous explicit request in product code": unchanged
    expect_count("one call of offer( in product code: the demand layer" "[.>]offer\\(" 1 src)
    expect_only("one call of offer( in product code: the demand layer" "[.>]offer\\("
      "^src/calculationdemand\\.cpp$" src)
    expect_only("the chosen next job is withdrawn by the demand layer only" "[.>]withdrawChosenNext\\("
      "^src/calculationdemand\\.cpp$" src)
    expect_none("no product code cancels a job" "([Jj]ob[Qq]ueue|m_queue|executor)(->|\\.)cancel\\(" src)
    # "no jobs window, no view of the queue" and "one authority: explicit-backed": unchanged
    ```

    - Rewrite the long comment above the request rules (`:377-387`): "... explicit work runs only as a job, through JobQueue::offer() from CalculationDemand ...".
    - `CalculationDemand` must therefore call `offer(` on exactly **one** source line and `withdrawChosenNext(` only in `calculationdemand.cpp`. Keep any comment that quotes them free of the `.`/`->` prefix.
  - widget-free-core (`:407-411`): `"src/plotrequests.*"` becomes `"src/calculationdemand.*"`.
  - stored-results "restoring is not requesting" (`:556`): the regex becomes `JobQueue|CalculationDemand|[.>](request|offer|prepare|publish)\\(`.
  - Plant a hit once for each new rule to prove it fires, then remove it (header rule, `:39-40`).
- `tests/acceptance_map.txt`. Edit these evidence lines only; comment lines stay:
  - `:280` `108 tst_jobqueue requestWhileStaleJobWindsDown` → `108 tst_jobqueue offerWhileStaleJobWindsDown`
  - `:281` `108 tst_plot_requests staleRunningJobShowsRefreshAtOnce` → `108 tst_calculation_demand staleRunningJobIsWaitingAtOnce`
  - `:301` → `111 tst_calculation_demand sessionWithoutInputIsNeverListed`
  - `:313` → `113 tst_calculation_demand chainedBlockersContinue`
  - `:316` → `114 tst_jobqueue oneAtATimeInOfferOrder`
  - `:317` → `114 tst_jobqueue duplicateOffersCreateNoDuplicates`
  - `:320` and `:636` → `115 tst_calculation_demand rowScript`, `306 tst_calculation_demand rowScript`
  - `:332` → `116 tst_calculation_demand startupRestoreWithHiddenSessionsStartsNothing`
  - `:333` → `116 tst_calculation_demand profileStyleApplyCreatesDemand`
  - `:334` → `116 tst_calculation_demand programmaticCheckCreatesDemand`
  - `:335` → `116 tst_plot_row_delegate programmaticCheckIsTheSameAsAClick`
  - `:336` → `116 tst_plot_row_delegate startupRestoreWithHiddenSessionsStartsNothingWithViewAttached`
  - `:337` → `116 tst_plot_row_delegate checkBoxClickChecksThroughTheModel`
  - `:338` → `116 tst_calculation_demand merelyUncomputedIsNotWorthAWarning`

  Every other map line cites a function whose name this phase keeps. Verify with the audit. The ctest `audit_cleanup` passing is the check: the map is checked at `cleanup_audit.cmake:667-760`, which needs `::<function>()` in `tests/<tst>.cpp`.

**Acceptance Criteria:**
- [ ] `ctest -L audit` passes.
- [ ] No map line cites `tst_plot_requests`.
- [ ] `grep -n "PlotRequests\|plotrequests" tests/audit/cleanup_audit.cmake` returns nothing.

**Complexity:** M

---

## Testing Requirements

### Unit Tests
- New:
  - `tests/tst_calculation_demand.cpp` (Task 1.8);
  - the new `tst_jobqueue` functions `holdsAtMostRunningAndChosenNext`, `offerReplacesChosenNext`, `withdrawEndsChosenNext`, `workerRunsBelowNormalPriority` and `mainThreadIsNotBlockedByARunningJob`.
- Rewritten:
  - `tst_jobqueue`, `tst_jobmodel`, `tst_fusion_jobs`, `tst_result_store`, `tst_result_columns` (Task 1.7);
  - `tst_plot_row_delegate`, `tst_plot_row_layout` (Task 1.9);
  - `tst_fusion_rows`, `tst_fusion_store` (Task 1.10).
- Deleted: `tests/tst_plot_requests.cpp`.
- Unchanged, and must still pass: `tst_session_model_engine`, `tst_workflow`, `tst_column_cache`, `tst_logbook_index`, `tst_result_records` and every other suite.

### Integration Tests
- Full `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure` over the labels core, fusion, exact, python and audit: all green.
- `audit_cleanup` passes, including acceptance traceability.

### Manual Verification
1. **M-P1-1, start-up.** Open a logbook with a fusion plot checked in the saved state and no sessions visible. No job appears (the jobs model is empty). Show a session with IMU data: the fit starts, and the roll row shows the working arc and "0 of 1".
2. **M-P1-2, showing while checked.** While the first fit runs, show a second session. When the first fit finishes, the second starts with no click. The row shows "1 of 2".
3. **M-P1-3, hide and uncheck.** With two sessions waiting, hide one: it is dropped at once and the running fit continues. Uncheck the plot: the row returns to plain; the running fit finishes and its record appears in the logbook's `cache/`.
4. **M-P1-4, burst.** Edit `_LOCAL_ORIGIN_INDEX` of a visible fused session several times within a second. The row shows working at once, and exactly one new fit starts about one second after the last edit.
5. **M-P1-5, no controls.** No plot row shows a refresh or cancel control. Clicking at the right of a working row only selects the row.
6. **M-P1-6, responsiveness.** During a long fit, pan and zoom plots and edit logbook cells. The UI stays responsive. On Windows or macOS, the worker thread shows below-normal priority in a process inspector.

## Notes for Implementer

### Gotchas
- **An offer is not a start.** A job starts only from the next turn of the event loop. Tests must spin (`waitEntered`, `waitStarted`, `waitDemandIdle`) before asserting `Running`. A second offer in the same turn replaces the first; that is the contract, not a bug.
- **`waitIdle(queue)` is not enough next to a demand layer.** Right after a check, before the pass has run, or while a session is settling, the executor is idle although demand exists. Use `waitDemandIdle` or `flush()`/`settle()` first.
- **The synchronous pass in `jobFinished` is load-bearing.** It must run before `endJob` reaches step (6), which it does because it runs inside the slot. Otherwise:
  - chains go idle between links (`chainedBlockersContinue` expects one `idle`);
  - the executor would start a stale chosen next job.
  Keep `m_reconciling` so the demand layer's own `offer`/`withdrawChosenNext` (which emit `jobFinished` for the replaced job) does not re-enter `recompute()`.
- **Replacement must not emit `idle()`** between the old and the new chosen next job. Tests count `idle` signals.
- **Publications are not input changes.** Without the `publishingJob()` check, every published result would start a one-second wait on its own session and delay chained calculations. The check compares session ids, so an edit of another session during the publication still counts.
- **Only relevant names** (the static closure of checked requested plots) start a wait or clear memory. An irrelevant edit, such as `_DESCRIPTION`, must neither delay nor retry anything.
- **Withdraw only your own offer** (`m_offeredJob`). Otherwise a demand layer with no checked plot would withdraw a test's direct offer before it starts.
- **`SessionModel::sort()` resets the model.** Clearing all memory on reset would retry job-level failures on every sort. Forget only ids that no longer have a row.
- `QThread::LowPriority` is honoured by Windows and macOS. On Linux, Qt maps it to nothing under `SCHED_OTHER`, but `QThread::priority()` still reports it, which is what the test checks.
- The audit's `one-worker` and `branch-mechanisms` rules match comments. Do not write `QThread`, `QEventLoop`, `processEvents` or `IdleScheduler` in `calculationdemand.*`.
- `JobRecord::reason` of a replaced or withdrawn job is exactly "No longer needed". Tests and the future jobs dock read it.
- The engine keeps an exception (`ResultStatus::Failed`) as an in-memory failed result: `NotProduced`, not stored. It is therefore shown as an ordinary failure, not re-run in this run, and computed again after a restart without any memory. Do not add it to the job-failure memory.
- A job that ends `Superseded` at start is offered again only if blocker inspection still names it. This relies on the engine's consistency between `blockers()`, `readiness()` and `prepare()`. Do not add a retry counter.

### Decisions Made
- **Executor API names:**
  - `offer()` (two overloads) returning `OfferResult` (same kinds as `RequestResult`);
  - `withdrawChosenNext()`, not `withdraw()`: `CalculationRegistry::withdraw` exists, and the audit rule must not collide;
  - `chosenNextJob()`;
  - `kMaxRunningJobs = 1`.
- **`cancel(JobId)` stays** for the planned jobs dock (`PLANS/jobs-dock-clean.md` cancels from a row). No product code calls it, and an audit rule enforces that. `cancelSession`, `cancelAll` and `cancelUnwantedQueued` are removed.
- **Refinement: `JobQueue::publishingJob()`.** A new read-only query, so the demand layer can tell a publication's `dependencyChanged` from an edit's. The overview says the wait is "restarted per session by each input change (`dependencyChanged`)". Taken literally, publications would count as input changes. This adds no lifecycle change and no callback.
- **Applying a profile is tested through the model calls it makes, not through `applyProfile()` itself.** `applyProfile(const Profile &, MainWindow *)` (`src/profilestatebridge.h:20`) takes a `MainWindow` and reads its `plotModel()`, `markerModel()` and `plotViewSettingsModel()`, and calls `applyDockLayout()`. `profilestatebridge.cpp` belongs to the application executable, not `flysight_core` (`src/CMakeLists.txt:407`). A test could call it only by constructing a `MainWindow`, which the harness cannot do (the `gestures` audit comment says so). The alternative is to refactor the bridge to take the models, which is outside this phase's scope and not worth the scaffolding.
  - `profileStyleApplyCreatesDemand` and `programmaticCheckIsTheSameAsAClick` replay the exact call `applyProfile` makes for plots: `PlotModel::setPlotEnabled` over every plot (`profilestatebridge.cpp:159-167`). The demand layer sees no other path.
  - Manual step M2 of `tests/README.md` covers the real menu path. Phase 4 keeps M2 on the list, restated for demand: loading a profile that checks a fusion plot while sessions are visible starts the fit.
- **Settle delay: 1000 ms** (`CalculationDemand::kInputSettleMs`). Only relevant non-publication changes start or restart it. Test seams: `setInputSettleDelay`, `endInputSettleWaits`, `isSettling`, `hasSettlingSessions`.
- **Refinement of "failure memory cleared wholly on session-model reset".** A reset forgets only the memory and settle entries of session ids that no longer have a row. `SessionModel::sort()` resets the model, and Phase 2 needs not-applicable memory to survive unloading and re-sorting, or it would reload sessions forever. A registry change still clears everything; an input change clears that session.
- **Synchronous passes.** A pass runs synchronously on `jobFinished` (not re-entrantly), and on uncheck or hide when a chosen next job exists; otherwise passes are coalesced. The two synchronous cases are what `PlotRequests` did synchronously (continuation, pruning).
- **Stateless counts.** `wanted` = applicable tracks; `done` = Done + Failed. There is no "episode" denominator: the row shows the truth of the moment.
- **Badge and indicator are exclusive** (spec §10). While working, failures appear only in the tooltip. Phase 1 paints a **static** working glyph (the first frame of Phase 3's animation) plus "k of n". No part of the row is clickable.
- **`PlotRowLayout` switches to indicator vocabulary in this phase** and drops `controlHit`. Keeping a dead hit rectangle until Phase 3 would leave unused code behind.
- **Audit group slug `gestures` is kept, with rewritten rules.** The map cites it; Phase 4 adds `demand`.
- **Map lines:** test names whose meaning survives are kept, so fewer map edits are needed; 14 lines are edited (Task 1.11).
- `PlotFixture` gains a `plotX` bridge (over `exhausted`), so the out-of-memory job failure can be tested through a plot.

### Open Questions
- **Acceptance statements 115 and 116** (and appendix B of `tests/README.md`) still describe the refresh/gesture model; item 116 says "applying a profile starts no job". Phase 1 repoints their evidence to the successor tests, but the statements themselves are superseded by the demand specification. **Recommendation:** Phase 4 restates or annotates items 115/116 (and the `# 115`/`# 116` comment lines of the map) as amended by `PLANS/demand-driven-requested-calculations.md`, next to the new 501+ items.
- **Linux priority.** `QThread::LowPriority` has no OS effect on Linux (`SCHED_OTHER`). The overview fixes `LowPriority`, and this plan keeps it. If an OS-level effect on Linux is wanted later, use a per-thread `nice` in the worker's `run()` or `QThread::IdlePriority` (`SCHED_IDLE`). Either would be a small, separate change.
- **External `cancel()` versus demand.** With a demand layer alive, a job cancelled from outside is offered again at once while it is still in demand. No product code cancels in this plan. The jobs dock's cancel (a later feature) will need a policy, such as remembering a user cancel like a job-level failure. That is out of scope here.

## Definition of Done

This phase is complete when:
1. All tasks have passing acceptance criteria.
2. All tests pass under `build-phase1`: the core, fusion, exact, python and audit labels.
3. Code follows the patterns of the reference files: the `PlotRequests` guarded-read and coalesced-pass discipline, and `JobQueue`'s end order and pinning.
4. No TODOs or placeholder code remain, and no source, comment or test names `PlotRequests`, `plotrequests`, `request(` on the executor, or a gesture entry point.
