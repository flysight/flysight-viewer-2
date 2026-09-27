# Phase 4: Documentation, acceptance map and audit

## Overview

This phase brings every document, test catalogue and traceability record in
line with "Demand-driven requested calculations" as Phases 1-3 implement it.
It rewrites the developer note's sections on the executor (15) and on the
demand layer (16, which replaces "Plot-driven requests"), the user guide to
computed plots, the fusion document's usage and lifecycle text and the data
schema's column-cache and stored-result sections. It adds a new acceptance
range, 501-563 (one item per clause of the specification, item = 500 + clause
number), restates as amended the 1xx and 3xx items that still describe the
refresh gesture and the queue, and adds a `demand` audit group that keeps the
new boundaries in place: one demand layer that nothing below it knows, views
that only read it, a model and scheduler that know nothing of jobs, pending
cells that never reach the model or the index, one bound, a below-normal
worker, and no refresh, cancel, queue or `PlotRequests` anywhere. No behaviour
changes: the only edits outside documents, `tests/README.md`,
`tests/acceptance_map.txt` and `tests/audit/cleanup_audit.cmake` are the
comment-only fixes of Task 4.8.

## Dependencies

- **Depends on:** Phase 1 (Executor and plot demand), Phase 2 (Column demand),
  Phase 3 (Working indicator, hover detail and pending cells), all
  implemented and committed before this phase starts.
- **Blocks:** None.
- **Assumptions** (names fixed by the phase documents; if a committed phase
  deviated, follow the code as committed, point a map line at the committed
  name only if that function asserts the same clause, and report the
  difference; never delete a map line to make the audit pass):
  - **Executor (Phase 1):** `JobQueue` (class and files keep their names; docs
    call it "the executor"), `JobQueue::kMaxRunningJobs` (1),
    `offer(sessionId, CalculationBlocker)` / `offer(sessionId, CalculationId)`
    returning `OfferResult {kind, job, created()}` with the kinds `Created`,
    `AlreadyActive`, `SessionNotLoaded`, `UnknownCalculation`, `MissingInput`,
    `Blocked`, `NothingToDo`, `ShuttingDown`; `withdrawChosenNext()`,
    `chosenNextJob()`, `publishingJob()`; `cancel(JobId)` kept with no product
    caller; `request()`, `RequestResult`, `oldestQueued`, `cancelSession`,
    `cancelAll`, `cancelUnwantedQueued` removed; the worker started with
    `QThread::LowPriority`; a replaced or withdrawn chosen next job ends
    `Cancelled` with the reason "No longer needed".
  - **Demand layer (Phases 1-3):** `CalculationDemand` in
    `src/calculationdemand.{h,cpp}` (`flysight_core`); types
    `DemandCondition` (Done, Waiting, Running, Failed, NotApplicable),
    `DemandTrack`, `DemandState` (`isWorking()`, `showsWarning()`,
    `isPlain()`); constants `kInputSettleMs` (1000), `kMaxHeldSessions`
    (`JobQueue::kMaxRunningJobs + 1`), `kToolTipListLimit` (10); queries
    `plotId`, `columnId`, `plotState`, `columnState`, `isCellPending` (two
    overloads), `workingPlotIds`, `workingColumnIds`, static `buildToolTip`,
    static `isMerelyUncomputed`; signals `plotStateChanged`,
    `columnStateChanged`, `statesChanged`; test seams `flush`,
    `hasPendingUpdate`, `passCount`, `setInputSettleDelay`,
    `inputSettleDelay`, `endInputSettleWaits`, `isSettling`,
    `hasSettlingSessions`, `heldSessionIds`, `hasFillWork`, `runLoadStep`,
    `recordSetLookups`. `PlotRequests` and its files are gone.
  - **Model and scheduler (Phase 2):** `SessionModel::loadPinnedSession(id)`,
    `SessionModel::ColumnFillTask` (4, priority 5, registered by the demand
    layer, not cancellable, active for the whole fill, progress label
    "Computing columns: %v / %m", the same as the cheap column pass),
    `IdleScheduler::unregisterTask`, `registerTask` replacing an existing
    id, and `TaskDef::canStep` (a task with work it cannot step right now is
    reported active, not stepped, and the scheduler rests until woken);
    `LogbookManager::calculationRecordReason()` /
    `setCalculationRecordReason()` and the `"recordReasons"` object of each
    session entry in `index.json`, set by the result store at write and at
    restore. The column worker's code is
    unchanged. A failed-load placeholder (visible or hidden) is settled as a
    job-level failure with the reason "The session file could not be loaded".
  - **Views (Phases 1 and 3):** `PlotRowDelegate(CalculationDemand *, view)`
    with no `editorEvent`, `PlotRowLayout.h` without `controlHit`,
    `src/ui/docks/DemandIndicator.{h,cpp}` (`drawWorkingGlyph`,
    `drawWarningGlyph`, `WorkingAnimation`: 80 ms per frame, 12 frames per
    turn), `src/ui/docks/logbook/LogbookHeaderView.{h,cpp}`,
    `src/ui/docks/logbook/LogbookCellDelegate.{h,cpp}` (pending text "…",
    U+2026, tooltip "Pending: this value is being computed"),
    `LogbookView(SessionModel *, CalculationDemand *, QWidget *)`,
    `AppContext::calculationDemand`.
  - **Tests:** `tests/tst_calculation_demand.cpp` (Phase 1: 41 functions;
    Phase 2: 26 more; Phase 3: `workingIdsFollowStates`,
    `toolTipListsAtMostTenFailures`), `tests/tst_logbook_indicators.cpp`
    (Phase 3: 15 functions, labels `core;widgets`), the new and renamed
    functions of `tst_jobqueue`, `tst_plot_row_delegate`,
    `tst_plot_row_layout`, `tst_result_columns`, `tst_column_cache`,
    `tst_session_model_engine` and `tst_fusion_store` named in the three
    phase documents; `tests/tst_plot_requests.cpp` deleted. Phase 1 already
    repointed the map lines of items 108, 111, 113-116 and 306 and rewrote the
    `gestures` audit group (keeping its slug); Phases 1 and 3 extended
    `widget-free-core` to `src/calculationdemand.*`.
  - `docs/`, `README.md`, `python_plugins/README.md` and `tests/README.md` are
    untouched by Phases 1-3, so their line numbers below (taken at `b55869d`)
    hold. `tests/audit/cleanup_audit.cmake` was edited by Phases 1-2: locate
    its passages by the text quoted, not by line number.
  - `PLANS/` is untracked. Nothing written in this phase links to it or names
    a file in it. The specification is cited by its title, "Demand-driven
    requested calculations".
  - Build and test only in `build-phase1/`. **Never build `build/`.**

## Tasks

### Task 4.1: `docs/CALCULATIONS.md`: introduction, sections 8, 12, 13 wording and section 15 (the executor)

**Purpose:** The developer note must describe `JobQueue` as the executor that
holds at most the running job and one chosen next job (spec §12, §11, §14),
with its bound, its below-normal worker and its unchanged lifecycle and job
history, and must stop describing a queue, deduplication against a list,
pruning or `request()`.

**Files to modify:**
- `docs/CALCULATIONS.md`

**Technical Approach:**

1. **Introduction, lines 8-11.** "the job queue (15), plot-driven requests
   (16)" becomes "the executor of background jobs (15), the demand layer that
   decides what they compute (16)". Line 12 keeps the link to
   `COMPUTED_PLOTS.md` ("What the user sees of it").
2. **§8, lines 223-229** (the paragraph "An explicit calculation's outputs must
   have no other candidate ..."): after "(`SessionModel::settleExplicitColumns`,
   section 17)" add: "; while a column over such an output is enabled, the
   demand layer then has the calculation computed for that session (section
   16.8)". Nothing else in §8 changes.
3. **§12, line 357**: "the caller (the job queue) decides" becomes "the caller
   (the executor, section 15) decides". **Line 491**: "is the job queue's
   decision" becomes "is the executor's decision".
4. **§13, lines 620-621**: "An outstanding ticket does not change a report:
   "pending" is the job model's notion." becomes "An outstanding ticket does
   not change a report: "waiting" and "running" are the demand layer's notions
   (section 16.1)."
5. **§15 heading and introduction, lines 658-675.** Heading
   `## 15. Background jobs: the executor`. Replace the two paragraphs with:
   - Sections 12-14 are the engine's half; the other half is `JobQueue`
     (`src/jobqueue.h`), called **the executor** in this note, and its
     `JobModel` (`src/jobmodel.h`), in `flysight_core` next to `SessionModel`
     (Qt Core and Gui only, no widgets, no GTSAM). One per application; it owns
     the application's only worker thread, which runs below normal priority
     (15.5).
   - **It is not a queue.** It holds at most the running job and the one job
     its caller has chosen to run next. It keeps no order of arrival, no list
     to deduplicate against and prunes nothing. Which job runs next needs the
     plots, columns, focus and row order, which the executor must not know; the
     demand layer (section 16) decides and is the only product caller of
     `offer()` and `withdrawChosenNext()`. The executor names no caller.
   - **Reads never start jobs.** A job is created by `JobQueue::offer()` and by
     nothing else; the executor never offers on its own. Keep the existing
     sentences on the synchronous `request()` (tests only; audit group
     `gestures`), Python plugins (`pluginsNeverStartExplicitWork`) and loading
     (a stored result is restored, not requested: 15.8). Replace the last
     sentence with: a job that ends cancelled, superseded or failed leaves its
     result missing, and the demand layer offers it again while it is in demand
     - except a job-level failure, which it remembers for the rest of the run
     (16.7).
6. **§15.1, lines 686-689.** "**The queue never loads a session.**" becomes
   "**The executor never loads a session.**"; "A request for a session that has
   no row or is not loaded is refused (`SessionNotLoaded`), and a queued job
   whose session stops being loaded ends Superseded at once" becomes "An offer
   for a session that has no row or is not loaded is refused
   (`SessionNotLoaded`), and a chosen next job whose session stops being loaded
   ends Superseded at once". In 679-685 "the queue needs" becomes "the
   executor needs".
7. **§15.2, lines 691-772.** Rewrite in this order (the two outcome tables at
   726-735 and 737-764 and the paragraph 751-764 stay verbatim apart from
   "queue" -> "executor"):
   - Diagram:
     ```
     offer() --> Queued (the chosen next job) --> Running --> Succeeded | Cancelled | Superseded | Failed
                    |
                    +--> Cancelled | Superseded          (ended before it ever ran)
     ```
     A chosen next job ends Cancelled when another offer replaces it, when it
     is withdrawn, cancelled or shut down, and Superseded when it goes stale at
     start or its session goes. Every job ends in exactly one end state
     (`JobModel::markFinished`).
   - `offer()` checks, in this order: shut down (`ShuttingDown`); equal to the
     chosen next job or to the running job not asked to stop (`AlreadyActive`,
     with its id; nothing changes, and an offer equal to the running job leaves
     the chosen next job alone); session not loaded (`SessionNotLoaded`); then
     the readiness mapping of 702-710 unchanged (`Blocked` -> "offer the
     blockers instead: chaining is the caller's"). A refusal changes nothing,
     the current chosen next job included.
   - **Replacement.** A `Ready` offer whose (session, instance) differs from
     the chosen next job replaces it: the old one ends Cancelled "No longer
     needed" (it never started; its pin is released), with no `idle()` between
     the old job and the new one, and the executor re-validates after the end's
     signals (a slot may shut it down or offer). The new job is Queued, pinned,
     announced (`jobQueued`, `jobsChanged`) and started from the event loop,
     never synchronously. **Withdrawal:** `withdrawChosenNext()` ends the
     chosen next job Cancelled "No longer needed"; `idle()` follows when
     nothing runs.
   - **A running job asked to stop does not count** (replaces
     "Deduplication", 712-718): cancelled, or stopped because its inputs went
     stale (15.3), it is winding down, so an offer of the same (session,
     instance) becomes the chosen next job and starts only after the old worker
     has been joined, with the new inputs. `activeJob()` applies the same rule.
   - **Inputs are captured when a job starts** (720-724), not when it is
     offered. Jobs run one at a time (`kMaxRunningJobs`, 1; the implementation
     has one run slot and asserts it); the chosen next job starts when nothing
     runs, always from the event loop; the running slot is freed only after the
     worker thread has been joined.
   - **Order of a job's end** (766-772): step (2) gains "while it runs,
     `publishingJob()` returns the job's id, so a listener can tell a
     publication's `dependencyChanged` from an edit's"; step (6) becomes
     "`idle()`, or the chosen next job, which a slot may have offered during
     (3), is scheduled". Add: the demand layer runs its pass synchronously in
     its `jobFinished` slot, so the next link of a chain is chosen before step
     (6) and the executor reports no `idle()` between links (16.4). Last
     sentence: "Slots connected to the executor's signals may call `offer()`,
     `withdrawChosenNext()`, `cancel()` and `shutdown()`."
8. **§15.3, lines 774-826.**
   - Bullet 776-779 (`cancel(id)`): the chosen next job ends Cancelled
     ("Cancelled") at once; on the running job cancellation is requested, as
     now. Add: `cancel(JobId)` is kept for a later jobs dock; no product code
     calls it (audit group `gestures`, "no product code cancels a job"). A job
     cancelled from outside while its pair is still in demand is offered again
     at once by the demand layer; a jobs dock that cancels will need its own
     policy (for example remembering a user cancel like a job-level failure).
   - Bullet 783-803: "a new request for the same calculation is `Created` and
     runs after the old worker has been joined" becomes "a new offer for the
     same calculation becomes the chosen next job and runs after the old worker
     has been joined"; "the queue still re-requests nothing by itself" becomes
     "the executor still offers nothing by itself"; every other "queue" in the
     bullet becomes "executor".
   - Bullet 804-808: delete "(`cancel()` still returns true; `cancelSession()`
     and `cancelAll()` do not count it again)"; keep "`cancel()` still returns
     true".
   - Replace bullet 809-811 (`cancelUnwantedQueued`) with: **Nothing is
     pruned.** The chosen next job is replaced by a different offer or withdrawn
     by its caller (15.2), both ending it Cancelled "No longer needed". The
     running job is never ended because its caller no longer wants it: it
     finishes, and its result is published and stored.
   - Bullet 812-816: "queued jobs whose session is no longer loaded" becomes
     "the chosen next job, if its session is no longer loaded,"; "does not hold
     the queue" becomes "does not hold the worker".
   - Bullet 817-824: "`shutdown()` refuses later requests, ends queued jobs
     Cancelled" becomes "`shutdown()` refuses later offers (`ShuttingDown`),
     ends the chosen next job Cancelled"; "the queue adds nothing" becomes "the
     executor adds nothing".
   - Bullet 825-826: "the queue holds" becomes "the executor holds".
9. **§15.4, lines 828-847.** "Every job pins its session from creation to its
   end" stays (a job is the running or the chosen next job). Add after it:
   "The demand layer additionally pins each session it loads for column demand
   while that session has column demand left (16.8); pins are counted per
   session id, so both kinds coexist." In 844-847 "the queue's" is not
   present; keep the paragraph.
10. **§15.5, lines 849-872.** After "At most one exists at a time." add
    "(`JobQueue::kMaxRunningJobs`)". New paragraph after 858: **Priority.** The
    worker is started with `QThread::LowPriority`, so that the interface stays
    responsive and the machine usable while a whole-logbook column fills in.
    Windows and macOS lower the thread's scheduling priority. On Linux under
    the default `SCHED_OTHER` policy Qt applies no change (`QThread::priority()`
    still reports `LowPriority`, which is what `tst_jobqueue` checks): there the
    guarantee is only that the main thread never waits for the worker
    (`mainThreadIsNotBlockedByARunningJob`). Lines 871-872 become: "The
    executor never pauses, wakes, or registers with `IdleScheduler`: saves,
    loads, bulk edits and column work continue during a job. (The demand
    layer's hidden loads are an idle-scheduler task of its own, 16.8.)"
    Replace "the queue" by "the executor" in 862-869.
11. **§15.6, lines 874-907.** 876-878: "the queue keeps no job list" becomes
    "the executor keeps no job list"; "a jobs dock can be a pure view of it"
    stays. 880: "One row per job in request order" becomes "One row per job in
    offer order". 888: "(a snapshot at request" becomes "(a snapshot at the
    offer". New bullet before 903: a chosen next job that is replaced or
    withdrawn stays in the history as a Cancelled row with the reason "No
    longer needed"; while demand changes quickly such rows accumulate like any
    finished row and the retention bound trims them. The job history is the
    one the jobs dock will read; this feature does not change it.
12. **§15.7, lines 909-937.** Precondition sentence: "`offer()`,
    `withdrawChosenNext()`, `cancel()` and `shutdown()` must additionally not be
    called from inside a calculation or an engine callback". Table rows
    (replace 921, 922, 923, 924, 925, 926, 927; keep 918-920 and 928-934 with
    "queue" -> "executor"):

    | Member | Meaning |
    |---|---|
    | `JobQueue::kMaxRunningJobs` | 1: jobs that may run at once; the demand layer's load bound follows it (16.8) |
    | `offer(sessionId, CalculationBlocker)` / `offer(sessionId, plainCalculationId)` -> `OfferResult {kind, job, created()}` | 15.2; `job` is non-zero for `Created` and `AlreadyActive` |
    | `withdrawChosenNext()` | false when there is no chosen next job; else ends it Cancelled "No longer needed" |
    | `chosenNextJob()` | the one Queued job, 0 if none |
    | `publishingJob()` | the job whose publication is being delivered (15.2, step 2), 0 at every other moment |
    | `activeJob(sessionId, instanceId)` | the chosen next or running job an offer would be equal to, 0 if none; never a running job that was asked to stop |
    | `activeJobs()` | the running job (also one winding down), then the chosen next job: at most two ids |
    | `runningJob()`, `job(id)`, `isIdle()`, `isShutDown()` | queries; `isIdle()` is "nothing runs and no chosen next job"; `job()` returns a default record for an unknown or removed job |
    | `cancel(id)` | false: unknown or already finished. Kept for a jobs dock; no product caller |

    The signal row: "`jobQueued` follows the model's `rowsInserted` ... and
    `offer()` still returns `Created`". Tests line (936-937): "`tests/tst_jobqueue.cpp`
    (the executor), `tests/tst_jobmodel.cpp`, and the controllable calculations
    of `tests/support/jobfixture.h` (`waitStarted`, `waitIdle`, `Quiet`)".
13. **§15.8.** Line 952-954: "both install paths write: the queue's publish and
    a synchronous `request()`" becomes "both install paths write: the
    executor's publish and a synchronous `request()`". Lines 1034-1037: after
    "every explicit calculation reads as not requested," add "what is switched
    on computes it again (section 16),". Lines 1042-1043: "After a restore
    `readiness()` is `Done`, so `JobQueue::request()` answers `NothingToDo`"
    becomes "After a restore `readiness()` is `Done`, so `JobQueue::offer()`
    answers `NothingToDo` and the demand layer counts the pair as done".

**Acceptance Criteria:**
- [ ] `git grep -n -E "request\(\)|RequestResult|cancelSession|cancelAll|cancelUnwantedQueued|oldestQueued|PlotRequests|in request order" -- docs/CALCULATIONS.md` prints only the engine's synchronous `request()` mentions of sections 8, 12 and 15's introduction, and no `JobQueue::request`.
- [ ] §15 contains `kMaxRunningJobs`, `offer(`, `withdrawChosenNext()`, `chosenNextJob()`, `publishingJob()`, "No longer needed", `QThread::LowPriority` and the Linux `SCHED_OTHER` sentence, and the note on a job cancelled from outside.
- [ ] Subsection numbers 15.1-15.8 are unchanged.

**Complexity:** M

---

### Task 4.2: `docs/CALCULATIONS.md`: section 16 (the demand layer) and section 17

**Purpose:** Replace "Plot-driven requests" with the demand layer (spec §5-§12,
§14): demand, result existence, work follows demand, the settle wait,
priority, failures and not applicable, hidden loads, state publication, the
views, the one-way flow and the component contracts; and make §17 say that a
column over a requested result fills for the whole logbook.

**Files to modify:**
- `docs/CALCULATIONS.md`

**Technical Approach:**

Replace lines 1054-1409 (the whole of section 16, up to the line before
`## 17.`) with the section below. Subsection numbers 16.1 and 16.3 keep their
subjects, because §17 cites "section 16.1" (the time-axis rule) and "16.3"
(explicit-backed) and `src/ui/docks/plotselection/PlotRowDelegate.h` may cite
16.9 / 16.10 (Task 4.8 checks). Use the prose style of the existing section:
bold lead-ins, one table per vocabulary, member names in backticks.

**`## 16. The demand layer: what is computed, and when`** (introduction)
- Users think in plots and logbook columns, not calculations: a checked plot
  means "show this for the visible sessions", an enabled column means "this
  value for every session in the logbook". What is switched on is the request;
  there is no refresh and no cancel.
- `CalculationDemand` (`src/calculationdemand.h`), the **demand layer**, is
  widget-free, in `flysight_core` next to `SessionModel`, `PlotModel`,
  `LogbookColumnStore` and the executor. It derives **demand** - the pairs
  (session, requested calculation) that something switched on needs and that
  have no result - and keeps the executor's chosen next job equal to its first
  choice. It is the only product caller of `JobQueue::offer()` and
  `withdrawChosenNext()`; nothing calls into it except the views' read-only
  queries; it observes the models, the logbook's record changes, the registry
  and the executor's signals. Everything is tested without widgets
  (`tests/tst_calculation_demand.cpp`).
- The principles, as a list: intent is expressed through plots and columns;
  anything needed is wanted at once and anything no longer needed is dropped;
  finished work is never wasted (results are stored); background work must not
  degrade the application; a failure is shown, never retried in a loop; each
  component's contract can be stated without naming the others (16.11).

**`### 16.1 Demand and track conditions`**
- **Plot demand:** for every checked plot whose y name is requested (16.3),
  every **visible, loaded track** - a row with `isLoaded() && visible &&
  !loadFailed`, exactly the rows the plot widget draws - needs the requested
  calculations that block that name for that session.
- **Column demand:** for every enabled logbook column whose value depends on a
  requested output (16.3), every **session row of the logbook**, loaded or not,
  needs the requested calculations that block that value.
- **Where a result is looked up** - two sources only:
  - a loaded session (visible or in the hidden pool): blocker inspection
    (section 13) of the plot's y name, or of the column's names (one, or two
    for a `Delta` column; combined: any `NotApplicable` -> `NotApplicable`,
    else any `NotProduced` -> `NotProduced`, else any `Blocked` -> `Blocked`
    with the union of the blockers, else `Available`);
  - a session that is not loaded (a stub, or a failed-load placeholder): the
    logbook's known record names (`LogbookManager::knownCalculationRecords()`;
    no record is opened). A known record of one of the column's storable
    calculations counts as a result until something restores it and finds it
    stale: the column worker does so when it fills the session's column values
    from a temporary copy, deletes the stale record, and that record change
    moves the pair into demand. The demand layer performs no staleness check of
    its own. Explicit family instances are never stored, so they create no
    column demand for a session that is not loaded.
- **Conditions** (a table, columns "Report" and "Condition"): `Available` ->
  Done; `NotApplicable` -> NotApplicable (silently absent); `NotProduced` ->
  Failed (an input-determined failure, reason from the notes); `Blocked` and a
  blocker is the running job not asked to stop -> Running; `Blocked` and a
  blocker is remembered as a job-level failure -> Failed (job-level); `Blocked`
  and every blocker is remembered as not applicable -> NotApplicable;
  otherwise `Blocked` -> Waiting (with `settling` while the session's inputs
  settle, 16.5). `Blocked` wins over `NotProduced`. A session that is not
  loaded is decided in this order: no storable calculation -> NotApplicable; a
  settlement of this run (16.7) -> its verdict; a known record -> Done; a
  remembered job-level failure -> Failed; every calculation remembered not
  applicable -> NotApplicable; a failed-load placeholder -> Failed ("The
  session file could not be loaded", settled); otherwise Waiting.
- A session without the calculation's inputs is `NotApplicable`: never
  waiting, running or failed, never counted, never offered.
- The failure-reason paragraph of the old 16.1 (lines 1107-1109) verbatim.
- **Only the y name is inspected**: keep lines 1111-1122 verbatim, with "Like
  every inspection it starts nothing and loads nothing" unchanged.

**`### 16.2 Plot and column state`**
- `plotState(plotId)` and `columnState(columnId)` return a `DemandState`, a
  plain value; `plotId` is `"<sensorID>/<measurementID>"` (equal to
  `PlotModel::PlotValueIdRole`), `columnId` is
  `logbookColumnDefinitionKey(column)`. The default value ("plain") for a plot
  that is unchecked, not requested or unknown, and for a column that is not
  enabled, not requested or unknown.
- Fields (a table): `sourceId`; `requested`; `wantedCount` (tracks that are
  not NotApplicable); `doneCount` (Done plus Failed: nothing is left to
  compute); `waitingCount`; `runningCount` (0 or 1); `failedCount`
  (input-determined and job-level); `running`, `waiting`, `failed` (lists of
  `DemandTrack` in row order; a column state counts its waiting tracks without
  listing them); `progressLabel` ("<done> of <wanted>" while working);
  `toolTip`. `isWorking()` = anything waiting or running; `showsWarning()` =
  not working and something failed (the badge replaces the indicator);
  `isPlain()` = neither. There is no "episode": the counts are the truth of the
  moment.
- `DemandTrack`: `sessionId`, `sessionName` (the name the logbook shows: the
  live `_DESCRIPTION` of a loaded session; for a session that is not loaded or
  failed to load, the description cached in `index.json`; the session id only
  when no description exists; `tst_calculation_demand::visibleFailedLoadIsSettledAsFailed`
  asserts the name),
  `condition`, `calculationTitles`, `reason` (failed), `jobFailure`,
  `settling`, `job`, `progressText` (running).
- The tooltip, built by the pure `buildToolTip(state)`, shown as a block:
  ```
  Computing: <done> of <wanted> done
    <session name> - <titles>: <progress text | running>
  Could not be computed:
    <session name> - <reason>
    and <n> more
  ```
  Each list shows at most `kToolTipListLimit` (10) sessions and ends with
  "and <n> more" when there are more; the state's own lists stay complete.
  Empty for a plain state. No message box reports a calculation outcome.
- `isCellPending(sessionId, columnId)` / `isCellPending(row, column)`: true
  exactly for a Waiting or Running cell of a requested column; the row is
  mapped to its session on every call. `workingPlotIds()` /
  `workingColumnIds()`: the ids whose state is working (what the views' clocks
  follow).
- Signals: `plotStateChanged(plotId)` and `columnStateChanged(columnId)` (the
  state or, for a column, its set of pending cells changed), per id, then
  `statesChanged()` once per pass; a state is stored before it is announced.
  The running job's progress text updates the states without a pass and
  without inspection. A state may be one event-loop pass behind the models: a
  consumer that must not be (the plot widget's "no data" warning) asks the
  engine (16.10).

**`### 16.3 Requested plots and columns, and what inspection costs`**
- Keep lines 1181-1193 (explicit-backed via `dependsOnExplicit()` /
  `explicitDependencies()`, memoized per plot id, dropped by a registry
  observer) with "explicit-backed" introduced as "requested (explicit-backed)".
  Add: a column is requested when `logbookColumnExplicitCalculations()` is not
  empty (the same registry answer the column cache uses; section 17); the
  demand layer never tests `EvaluationPolicy::Explicit` itself.
- **What a pass costs.** Plots that are not requested are never inspected (one
  hash lookup). Blocker inspection runs only for (checked, requested plots) x
  (visible, loaded tracks), and for (enabled, requested columns) x (loaded
  rows whose report memo was dropped by an input change, a publication, a
  load, a record change or a single-row edit), through
  `SessionModel::loadedSession()` under one `RowStabilityGuard`: nothing is
  loaded, evicted or touched in the LRU inside a pass, and the guard is
  released before anything is offered, withdrawn, pinned or emitted. A session
  that is not loaded costs one `knownCalculationRecords()` call between changes
  to its records (a memo); a pass is O(rows) plus O(rows x requested columns)
  hash lookups, and no walk at all when no requested column is enabled.
- **When a pass runs.** Passes are coalesced to one per event-loop pass
  (zero-interval timer); a pass runs synchronously in the executor's
  `jobFinished` slot (16.4) and on an uncheck or hide while a chosen next job
  exists (so the waiting pair is withdrawn before it can start). A table of
  triggers: the plot model's check-state `dataChanged` and `modelReset`; the
  session model's `visibilityChanged`, `sessionLoaded`, `modelChanged`,
  `focusedSessionChanged`, `modelReset`, `dependencyChanged` of a relevant name
  (16.5) and a single-row display `dataChanged` (bulk edits and the column
  worker, which emit no `dependencyChanged`); `LogbookManager::
  calculationRecordsChanged`; the executor's `jobStarted`,
  `jobCancelRequested`, `jobFinished` and (text only) `jobProgress`; the
  registry observer; the end of a settle wait.

**`### 16.4 Work follows demand`**
- **No gesture.** A pair that enters demand is wanted at once, and the
  indicator shows it immediately. What creates demand (list): checking a plot
  in any way (a click on the check box, Space, the Plots menu, applying a
  profile, the start-up restore - the check state in `PlotModel` is the only
  thing read); showing a session while a plot is checked; a visible session
  finishing its load; enabling a column (the column editor, a profile:
  `LogbookColumnStore`); an input change that drops a demanded result (after
  the settle wait, 16.5); a record the column worker deletes as stale; a
  registry change that makes a plot or column requested.
- **Start-up.** Every session starts hidden, so restored checked plots create
  no demand until a session is shown; enabled columns create demand at once,
  and their hidden loads wait behind the start-up work by scheduler priority
  (16.8). Nothing about demand is persisted.
- **Profiles.** Applying a profile that carries a column over a requested
  output computes that calculation for every session of the logbook that lacks
  a result. That is intended, and it is why no default profile carries such a
  column; the audit (group `demand`) checks the profiles shipped in
  `src/resources/profiles/`.
- **What drops demand:** unchecking the plot, hiding the session, disabling the
  column, or the result appearing by other means. A waiting pair that leaves
  demand is withdrawn before it starts (the executor ends it Cancelled "No
  longer needed"). There is no queue of accepted requests to prune: demand is
  the only list of waiting work.
- **The running job is never stopped by the demand layer.** It finishes, and
  its result is published and stored, even when its pair left demand. It is
  stopped only as 15.3 says: its inputs changed, its session went away, or the
  application closes.
- **Offering.** Each pass walks the candidates in priority order (16.6) and
  offers the first one the executor accepts. A table of the executor's answers
  and the reaction: `Created` (the chosen next job; done); `MissingInput`,
  `NothingToDo`, `UnknownCalculation` (remembered as not applicable; next
  candidate); `Blocked`, `SessionNotLoaded`, `AlreadyActive` (next candidate,
  nothing remembered); `ShuttingDown` (stop). When no candidate is accepted,
  the demand layer withdraws the chosen next job if it offered it itself; a
  chosen next job it did not offer is replaced when it has a choice and
  otherwise left alone (the executor's own tests drive it directly).
- **Chained calculations.** When requested calculation B consumes requested A,
  demand covers both, upstream first, as blocker inspection orders them. The
  pass that runs synchronously in `jobFinished` (before the executor decides
  between `idle()` and the next start, 15.2) offers the next link, so the
  executor reports no `idle()` between links; a hold (16.8) lasts across the
  links.
- **After shutdown** nothing is offered and the states stay working until the
  demand layer is destroyed.

**`### 16.5 The input-settle wait`**
- `CalculationDemand::kInputSettleMs` (1000 ms). A `dependencyChanged` of a
  relevant name (the static closure of every checked requested plot and every
  enabled requested column) on a session is an input change, unless it is the
  delivery of a publication (`JobQueue::publishingJob()` is a job of that
  session). An input change forgets the session's remembered failures,
  not-applicable verdicts and settlements and restarts its wait.
- While the wait runs, the session's pairs are in demand and counted Waiting
  (with `settling`), so the indicator shows from the first change, but they are
  not offered; a burst of edits runs one job. The executor already stops a
  running job whose inputs changed (15.3); the wait decides only when the
  replacement starts.
- An irrelevant edit (a description) starts no wait and clears nothing.
  Showing, hiding, checking, enabling, profiles and loading take effect
  without a wait.

**`### 16.6 Priority`**
- At every pass, and so when a job ends and whenever demand changes, the
  candidates are, in order: (a) plot demand of the focused session (if it is
  one of the tracks); (b) plot demand of the other visible sessions in logbook
  row order; (c) column demand of every loaded session: the visible ones in
  logbook row order, then the hidden ones (the pool, and sessions the fill
  has loaded) in logbook row order, so that what the user is looking at is
  computed before what only the logbook shows. Within a session: plot-model
  order, then column order, then the blockers' order (upstream first); each
  (session, instance) once.
- A session that is not loaded enters (c) once the fill has loaded it (16.8),
  in row order.
- The choice is made from demand as it is at that moment, not from the order in
  which pairs entered it: a different first choice replaces the executor's
  chosen next job (Cancelled "No longer needed"). The running job is not
  preempted. So a session shown during a column fill is computed next, after
  the running job.

**`### 16.7 Failures, not applicable, settlements`**
- **Input-determined failures are results.** A rejection or solver failure
  (`NotProduced`) is stored, shown with the warning badge and its reason, and
  never offered again; an input change makes it `Blocked` again. A result the
  engine cached as `Failed` (a compute function that threw) is kept in memory,
  not stored: badged, not offered again in this run, computed again after a
  restart.
- **Job-level failures** (`JobState::Failed`: the worker could not be started,
  or the calculation ran out of memory) are remembered per (session,
  calculation instance) with the reason "<title>: <reason>" (for example
  "Sensor fusion: Out of memory"). The pair is badged (a job-level failure) and
  not offered again until the session's inputs change or the registry changes.
  The memory is not persisted, so the next start of the application tries
  again.
- **Not applicable.** An offer the executor refuses as `MissingInput`,
  `NothingToDo` or `UnknownCalculation` is remembered like a failure, the track
  reads NotApplicable, and the memory clears the same way.
- **Memory and resets.** A session-model reset (a sort resets the model)
  forgets only the entries of sessions that no longer have a row; a registry
  change forgets everything.
- **Settlements** (columns only): the last final verdict (Done, Failed,
  NotApplicable) of a session's cell, kept per (session, column) for the run,
  so that a session evicted after its verdict is not loaded again to find the
  same answer (a calculation that does not apply, a thrown exception, a failed
  record write, a job-level failure, a failed load). For a session that is not
  loaded a settlement takes precedence over the record names. Cleared by the
  session's input change, by a record change of a calculation of that column,
  by a single-row edit of the session while it is not loaded (a bulk edit), by
  the column leaving the enabled set, by a registry change, and for sessions
  whose row is gone.
- **A session whose file cannot be loaded** (a failed-load placeholder, visible
  or hidden) is settled as a job-level failure, "The session file could not be
  loaded": badged, never pending, not retried in this run unless its data
  change; the next start tries again.
- **There is no retry control.** Changing an input is how a failure is
  retried; restarting the application retries job-level failures.
- **Stored failures of sessions that are not loaded.** Records are never
  opened for such a session; the logbook index records each record's reason
  instead (`LogbookManager::calculationRecordReason()`, DATA_SCHEMA §11),
  learned by the result store when the record is written and when it is
  restored. A stored rejection or solver failure of a session that is not
  loaded is therefore a failed result with its reason, listed in the column's
  failure list and badge before and after a restart alike, without a load. A
  record written by an earlier build has no recorded reason until its next
  restore (the column worker's copy or a load) and counts as a success until
  then.

**`### 16.8 Sessions that are not loaded`**
- The executor needs its session loaded (15.1), so the demand layer, not the
  column worker, has the sessions of column demand loaded. **The load step** is
  an `IdleScheduler` task the demand layer registers through
  `SessionModel::scheduler()` under `SessionModel::ColumnFillTask` (4), at
  priority 5, below saving (1), loading visible sessions (2), bulk edits (3)
  and column work (4); it is unregistered when the demand layer is destroyed.
  The task is the column fill: it has work while any session has a waiting or
  running column cell, it can step (load) only while a hold is free and an
  unloaded session waits, and its progress is the sessions remaining of the
  fill's high-water mark, so the logbook's progress line shows "Computing
  columns: k / n" for the whole fill, as for the cheap column pass. For this
  the scheduler gained the generic notion of a task with work it cannot step
  right now (`TaskDef::canStep`): reported as active with its progress, not
  stepped, and the scheduler rests until woken instead of spinning. The
  scheduler learns nothing about jobs: the task is one more source of
  steps.
- **A step** takes the first session in row order that is not loaded, not
  visible (a visible stub belongs to the visible loader), not settling, not
  held and has a waiting cell, re-validates it, and calls
  `SessionModel::loadPinnedSession(id)`: the real load of showing it
  (`sessionRef()`: the session-id correction, the engine attached, its stored
  results restored, `sessionLoaded`, the LRU and its eviction pass) without
  making the row visible, pinned under the id the row has after the load. The
  session-id correction therefore always precedes the offer; no pair of a
  session that is not loaded is ever offered. A load that fails settles the
  session (16.7).
- **The bound.** At most `CalculationDemand::kMaxHeldSessions`
  (`JobQueue::kMaxRunningJobs + 1`, so 2: the running job's session and the
  chosen next) are held at a time; the next is loaded when one is released.
  Sessions that were already loaded are never held and do not count.
- **Holds.** A hold is the demand layer's own pin, begun when the load returns
  and released when the session has no Waiting or Running cell left (its job
  ended and the result was published, it was settled, its column was disabled,
  its result appeared by other means), when its row is gone or unloaded, when
  the demand layer is inert, and at its destruction. A hold lasts across the
  links of a chain. The executor's own pin keeps a running job's session loaded
  after a hold is released. Released sessions stay in the hidden pool until
  ordinary LRU eviction; the pool may exceed "Maximum cached sessions" by at
  most the holds, and a capacity of 0 works.
- **Ordering.** The scheduler runs the highest-priority task with work, so a
  load step never runs while a save, a visible load, a bulk edit or column work
  has work: a stub is never dirty, a bulk edit on a stub finishes (edit, save,
  index) before any load step, and a bulk edit on a held session takes the
  loaded path, so a job over the old value goes stale and is offered again. No
  result is computed from a file that is about to be rewritten.
- **Progress line.** The logbook's progress line shows "Loading sessions for
  columns: k / n" for a moment before each load, with no cancel button: the
  task is not cancellable, because a cancel that left the column enabled would
  be undone on the next tick; disabling the column is how the work is dropped.
- **The column worker is unchanged.** It settles a column over a requested
  output as unavailable when the session has no record, computes it from its
  temporary copy when there is one, and never knows a job exists. When a job
  writes the record, the record change drops the cached value and the
  loaded-row refresh computes the new one. Cheap column values never wait for
  a requested calculation.
- **Edge cases** (a list): a column disabled mid-load (the next pass releases
  the hold and withdraws its pair; a running job finishes and is stored); a
  held session shown by the user (the hold ends as usual; a visible row is not
  in the LRU and stays loaded); the logbook reopened or sessions removed (the
  model reset releases the holds); a calculation found not applicable after the
  load (settled without a job; the worker's cached "unavailable" stays); the
  executor shut down (nothing more is loaded).
- **A known cost.** A column whose value reaches a requested calculation only
  through one of several candidates of an on-demand name would load each
  session without a record once per run to find out. No registered column is
  like this today (every fusion output has one candidate); if one appears, the
  fix belongs to `CalculationRegistry::explicitDependencies()`, not to the
  demand layer.

**`### 16.9 API and threading rules`**
- Main thread only; no member may be called from inside a calculation or an
  engine callback. Create after the executor, destroy before the executor and
  before the session model (it registered a scheduler task). Collaborators are
  held weakly; a missing one makes the component inert (default states,
  nothing offered, every seam safe).
- A table of members: the constructor
  `CalculationDemand(SessionModel *, PlotModel *, JobQueue *, QObject *parent)`
  and destructor (removes the registry observer, unregisters the load step,
  releases the holds); `kInputSettleMs`, `kMaxHeldSessions`,
  `kToolTipListLimit`; `plotId(...)`, `columnId(column)`; `plotState`,
  `columnState`, `isCellPending` (both), `workingPlotIds`, `workingColumnIds`;
  static `buildToolTip`, static `isMerelyUncomputed(session, sensorId,
  measurementId)`; the three signals; the test seams (`flush`,
  `hasPendingUpdate`, `passCount`, `setInputSettleDelay`, `inputSettleDelay`,
  `endInputSettleWaits`, `isSettling`, `hasSettlingSessions`, `heldSessionIds`,
  `hasFillWork`, `runLoadStep`, `recordSetLookups`: tests only; the views never
  call them); `DemandCondition`, `DemandTrack`, `DemandState` (16.1, 16.2).
  Then the entries it uses below it: `SessionModel::loadPinnedSession(id)`,
  `SessionModel::ColumnFillTask`, `IdleScheduler::registerTask` (replaces an
  existing id) and `unregisterTask(id)`.
- Tests: `tests/tst_calculation_demand.cpp` (plot and column demand with the
  synthetic plots of `tests/support/plotfixture.h` over the calculations of
  `jobfixture.h`), `tests/tst_result_columns.cpp` (the column worker unchanged
  by demand), `tests/tst_column_cache.cpp` (`loadPinnedSession`),
  `tests/tst_session_model_engine.cpp` (`unregisterTask`).

**`### 16.10 The views and application wiring`**
- **Plot rows.** `PlotRowDelegate`, installed by `PlotSelectionDockFeature`,
  paints `DemandState` right-aligned in the row: while working, the working
  indicator (an open 270° arc in the row's text colour, turning about once a
  second) and `progressLabel` ("k of n"); once work is finished and some
  sessions failed, the warning badge with `failedCount`; never both. Plain rows
  are the unmodified `QStyledItemDelegate`, pixel for pixel; the name is elided,
  never the cluster; the row height never changes. The tooltip is
  `DemandState::toolTip` over the whole row. Geometry is the pure
  `layoutPlotRow()` (`PlotRowLayout.h`), which has no hit rectangle.
  **No gestures:** the delegate handles no event of its own; checking a row is
  the base class's write to `PlotModel`, which the demand layer observes like
  every other check change.
- **The shared glyphs.** `src/ui/docks/DemandIndicator.h` (Qt Core and Gui
  only): `drawWorkingGlyph()`, `drawWarningGlyph()` and `WorkingAnimation`, one
  clock per view, ticking (80 ms per frame, 30° per frame) only while its view
  has something working (`workingPlotIds()` / `workingColumnIds()`); it stops
  and returns to frame 0 when nothing is, and each frame repaints only the
  working rows or sections.
- **Logbook column headers.** `LogbookView` takes the demand layer and installs
  `LogbookHeaderView`: a requested column's header shows the same indicator or
  badge immediately right of its centred text, inside the label rect (clear of
  the sort arrow), the text elided per line to make room; no count in the
  header; hovering the section shows `columnState(id).toolTip`; double-clicking
  the section edge fits text and glyph; clicks sort exactly as before. The
  section-to-column mapping is computed per call (sorting and column changes
  reset the model).
- **Pending cells.** `LogbookCellDelegate` paints a cell whose pair is in
  demand (`isCellPending`) and whose model value is empty as a muted "…"
  (U+2026, the palette's placeholder colour), with the tooltip "Pending: this
  value is being computed". A value always wins. The three looks: a value;
  empty (unavailable, or a value over a record that could not be read, which is
  not cached); "…" (in demand). Pending is a presentation of demand: the model,
  its cached values, `pendingColumns`, `index.json` and `SessionModel::sort()`
  never see it; the cached value underneath stays unavailable until the record
  is written, so sorting treats a pending cell as unavailable. A
  `columnStateChanged` repaints that column's visible rect only. Cells are not
  animated.
- **The progress line** is unchanged in form; the load step has its label
  (16.8).
- **Ownership and order.** Keep the substance of lines 1362-1377 with: "the
  `JobQueue` and then the `CalculationDemand`"; `AppContext` carries
  `jobQueue` and `calculationDemand`; restored plots and a first-launch profile
  reach the demand layer as ordinary model changes and every session starts
  hidden, so start-up starts no job for plots, while an enabled column over a
  requested output does start its fill; `closeEvent()` calls
  `JobQueue::shutdown()` first ("an executor that was shut down refuses every
  later offer"); `~MainWindow()` deletes the demand layer, then the executor,
  explicitly and before everything else. The plot list's delegate and the
  logbook's header and cell delegate hold the demand layer weakly and turn
  plain once it is gone. No dialog reports a calculation outcome.
- **The "no data" warning.** Lines 1379-1391 with `PlotRequests::` replaced by
  `CalculationDemand::` (`src/calculationdemand.h`), "`rowState()`" by "the
  plot's state", "shown by the plot list" unchanged, and the test
  `tst_calculation_demand::merelyUncomputedIsNotWorthAWarning`.
- **Results appear through ordinary invalidation only.** Lines 1393-1401 with
  "the queue or the component" -> "the executor or the demand layer".
- Tests: `tests/tst_plot_row_layout.cpp` (geometry, no widgets),
  `tests/tst_plot_row_delegate.cpp` and `tests/tst_logbook_indicators.cpp`
  (the delegate, the header view and the cell delegate in offscreen views: the
  only tests that link Qt Widgets, behind `FLYSIGHT_BUILD_WIDGET_TESTS`), and
  the manual script in `tests/README.md` section 12 (steps M1-M9 and M23-M28).
  That nothing but the demand layer offers work, and that the views only read
  it, are rules of the cleanup audit (groups `gestures` and `demand`).

**`### 16.11 Component contracts and the one-way flow`**
- One sentence of contract per component, none naming another's workings:
  - **The demand layer:** from what is switched on and where results are, it
    derives demand, chooses by priority, has sessions loaded for column demand
    within its bound, offers the next pair, remembers this run's failures, and
    publishes the state the views present.
  - **The executor:** runs one requested calculation for one loaded session
    with the engine's prepare / compute / publish steps, holding at most the
    running job and one chosen next job; it never loads a session and knows no
    caller.
  - **The column worker:** keeps cached column values a function of what is on
    disk; it never requests, prepares or runs a requested calculation and never
    knows a job exists.
  - **The idle scheduler:** runs steps of registered tasks by priority; it
    knows nothing about jobs.
  - **The views:** present the demand layer's state; they never run a pass,
    offer, or write.
- **The flow is one way:** the demand layer chooses; the executor publishes
  into the loaded session and the engine's listener writes the record; the
  record change drops the cached column values over it and the loaded-row
  refresh recomputes them; the demand layer sees the result through the same
  blocker inspection and record names it always reads, and the pair leaves
  demand. No component asks another what it intends to do.
- **What stays true:** restoring a stored result is not requesting (only a
  missing result creates demand); the temporary copy used for cheap column
  values never writes a stored result and never requests; a restored result is
  indistinguishable from a published one; the engine's threading rules are
  unchanged; the plot widget's "no data" warning keeps asking the engine.
- The cleanup audit's group `demand` holds these boundaries as text rules
  (`tests/README.md`, section 10).

**§17 edits:**
1. **Plots paragraph, lines 1499-1502.** "They are explicit-backed (16.3), so
   they are computed from the plot list and nowhere else." becomes "They are
   requested (16.3): a checked fusion plot has the fit computed for the visible
   sessions, and nothing else about a plot starts one (section 16)."
2. **Logbook columns paragraph, lines 1521-1560.** After "...or unavailable
   when the calculation is not requested." insert: "While such a column is
   enabled, the demand layer has the calculation computed for every session of
   the logbook that has no result, loaded or not (section 16; sessions that are
   not loaded are loaded as hidden sessions, two at a time, 16.8); until the
   record is written the cached value stays unavailable and the view paints the
   cell as pending (16.10), and the record change then drops the value and the
   loaded-row refresh computes it." Replace "A value over a record the copy
   skipped stays pending (not cached, shown empty) until the session is
   loaded" with "A value over a record the copy skipped is not cached (the cell
   is empty, not the "…" of 16.10) until the session is loaded". Append at the
   end of the paragraph: "A stored rejection of a session that is not loaded
   is a failed result with the reason the index recorded for it (16.7): not
   computed again, listed in the column's failures before and after a restart
   alike, and the session is not loaded for it."
3. **Tests paragraph, lines 1562-1580.** "`tests/tst_fusion_jobs.cpp` (the job
   queue's worker on a real `SessionModel`)" -> "(the executor's worker on a
   real `SessionModel`)"; "`tests/tst_fusion_rows.cpp` (the plot rows of
   section 16 with the seventeen real plots and real fits)" -> "(the demand
   layer and the plot rows of section 16 with the seventeen real plots and real
   fits: fits start and are dropped with no gesture)"; in the
   `tst_fusion_store` parenthesis add "; a logbook column over roll filled for
   sessions that are not loaded, and not fitted again after a restart"; after
   "the column rule without GTSAM in" add
   "`tst_calculation_demand::enablingColumnFillsEveryUnloadedSession` (column
   demand), `tst_result_columns::columnWorkerIsUnchangedByDemand`,".

**Words to avoid in all of `docs/` and `README.md`** (audit groups `demand`,
`stored-results`, `result-validity`, `naming`): `PlotRequests`,
`plotrequests`, `PlotRowState`, `PlotTrackCondition`, `tst_plot_requests`,
`oldestQueued`, `cancelUnwantedQueued`, `cancelSession(`, `cancelAll(`,
`RequestResult`, "refresh icon", "refresh control", "refresh gesture",
"press refresh", "pressing refresh", "cancel control", "cancel icon",
"circled x"; also "Results are not saved", "kept in memory only", "makes every
record stale", `EKF`. Say "there is no refresh and no cancel" instead.

**Acceptance Criteria:**
- [ ] §16 has exactly the headings `## 16. The demand layer: what is computed, and when` and `### 16.1` to `### 16.11` with the titles above; `git grep -c "^### 16\." -- docs/CALCULATIONS.md` prints 11.
- [ ] §16 names `CalculationDemand`, `kInputSettleMs`, `kMaxHeldSessions`, `kToolTipListLimit`, `loadPinnedSession`, `ColumnFillTask`, "Computing columns", "No longer needed", "The session file could not be loaded", "Pending: this value is being computed", `src/resources/profiles`, and states the restart rule for stored rejections and the alternative-candidate cost.
- [ ] §17's references to "section 16.1" and "16.3" still point at the time-axis rule and at requested plots.
- [ ] `cmake -DREPO=. -P tests/audit/cleanup_audit.cmake` reports no `demand` violation in `docs/CALCULATIONS.md`.

**Complexity:** L

---

### Task 4.3: `docs/COMPUTED_PLOTS.md`

**Purpose:** The user guide must describe what the user sees (spec §10, §14):
no refresh and no cancel, the working indicator and its hover, the badge,
logbook columns that fill for the whole logbook with pending cells, and
failures.

**Files to modify:**
- `docs/COMPUTED_PLOTS.md` (replace the whole file, lines 1-161)

**Technical Approach:**

Title `# Plots and logbook columns that are computed in the background`. Table
of contents with the nine sections below (anchors from the headings). Plain
user language, no class names.

1. **Why some values need computing.** Most plots and columns are instant. A
   few take seconds to minutes per recording: today the "Sensor fusion" plots
   and any logbook column over a Sensor fusion value ([what they are](SENSOR_FUSION.md)).
   FlySight Viewer computes them in the background for what you have switched
   on: a checked plot for the tracks that are visible, and a logbook column for
   every recording in the logbook. There is nothing to press: switching a plot
   or a column on is the request, switching it off drops what has not started.
   A result, once computed, is kept with the recording (section 6).
2. **What a plot row shows.** Table "You see / It means": a turning arc with
   "k of n" (computing: k of the n visible tracks that can be computed are
   done); a warning triangle with a number (after computing, that many tracks
   could not be computed: section 7); nothing (everything that can be shown is
   shown, or the plot never needs computing: such a row looks exactly as it
   always has). The arc and the triangle are never shown together. Hover over
   the row, the arc or the triangle: "Computing: k of n done" with the track
   being worked on and its current step, and "Could not be computed:" with each
   track and its reason (at most ten of each, then "and N more"). Roll, pitch
   and yaw come from one computation per track, so they show the same progress
   and one computation fills them all. Only visible tracks count; a track whose
   recording lacks the needed sensor data never appears in a number or a
   tooltip.
3. **What starts a computation.** Anything that switches a value on: checking
   the plot (click, Space, the Plots menu, applying a profile), showing a track
   while the plot is checked, adding or enabling a logbook column over such a
   value (the column editor, a profile). Tracks already computed (section 6)
   are drawn at once. When you start FlySight Viewer every track is hidden, so
   checked plots start nothing until you show tracks; a logbook column over
   such a value continues filling at once (section 4). After you edit a
   recording's data, the track waits about a second after your last edit
   before it is computed again, so a series of edits costs one computation.
4. **Logbook columns over computed values.** Such a column is filled for every
   recording in the logbook, whether or not it is shown. Recordings that are
   not loaded are loaded in the background, at most two at a time, as hidden
   recordings; the progress line under the logbook shows "Loading sessions for
   columns: k / n" before each, with no cancel button. While a column fills,
   its header shows the turning arc at the right of its name; hovering the
   header shows the counts, the recording being computed and any failures.
   A cell whose value is still to come shows a grey "…"; a blank cell means the
   value does not exist for that recording (for sensor fusion, a recording
   without IMU data). Sorting by the column puts "…" and blank cells together
   at the bottom. Visible tracks go first: a track you show while a column
   fills is computed next. Removing or disabling the column stops further work
   (the computation that is running finishes and is kept). Applying a profile
   that includes such a column fills it for the whole logbook, which can take a
   long time on a large logbook; none of the profiles that come with FlySight
   Viewer includes one.
5. **When you stop needing a result.** Unchecking a plot, hiding a track, or
   removing a column drops the computations that have not started, at once.
   The one that is running is left to finish and its result is kept: hiding a
   track for a moment does not throw away minutes of work. There is no refresh
   and no cancel; quitting stops the running computation.
6. **When results go away.** Keep the bullets of the current section 6 (lines
   101-125) with these changes: the first bullet ends "...results that
   depended on the old data are discarded. If that happens while the track is
   being computed, that computation is stopped - its result could not be used.
   While the plot is checked and the track visible, or a column needs it, the
   track is computed again from the new data a moment after your last edit.";
   the second bullet "without the refresh icon and without computing" becomes
   "at once, without computing"; the `cache/` bullet adds "every such track is
   computed again as soon as something switched on needs it"; the unreadable
   bullet ends "...for that time only: the file is kept, and the result comes
   back the next time the track is loaded (while it cannot be read, a checked
   plot or a column computes the track again)".
7. **When a track cannot be computed.** Some recordings cannot be computed (for
   sensor fusion, one with a gap in its sensor data): once the work is done the
   plot row, or the column header, shows the warning triangle and the tooltip
   names the recording with the reason; no message box appears. Nothing offers
   to try again, because the same data give the same answer; when the data
   change the recording is computed again. A few failures are not about the
   data - the computer ran out of memory, or the recording's file could not be
   read: they are shown the same way, not tried again while FlySight Viewer
   runs, and tried again the next time it starts. A recording that lacks the
   needed sensor altogether is silently absent (keep lines 137-140).
8. **While computing.** The application stays fully usable (keep the list of
   line 144-146). Computations run one at a time, in the background at a lower
   priority than the rest of the application: first the track you are looking
   at (the focused one), then the other visible tracks from top to bottom, then
   what logbook columns need, from top to bottom. Quitting stops them; expect a
   short wait while the running one reaches a point where it can stop.
9. **Known limitations.** Bullets: there is no window that lists computations
   (the rows, the column headers and their tooltips report progress and
   failures); there is no switch that pauses background work (remove the
   column or uncheck the plot); on Linux the background computation does not
   run at a lower operating-system priority (the application stays
   responsive all the same).

**Acceptance Criteria:**
- [ ] The file has nine numbered sections with a working table of contents, and contains "Computing columns:", "…", "Computing:", "Could not be computed:" and "none of the profiles that come with FlySight Viewer".
- [ ] `git grep -n -i -E "refresh (icon|control)|press(ing)? (the )?refresh|cancel control|circled x|keyboard access" -- docs/COMPUTED_PLOTS.md` prints nothing.

**Complexity:** M

---

### Task 4.4: `docs/SENSOR_FUSION.md`, `docs/DATA_SCHEMA.md`, `python_plugins/README.md`, root `README.md`

**Purpose:** The fusion document (spec §14: section 2, and the refresh wording
of section 7), the data schema (sections 11 and 12, where a column over a
requested calculation "stays unavailable until requested from the plot list")
and the two READMEs must describe demand.

**Files to modify:**
- `docs/SENSOR_FUSION.md`, `docs/DATA_SCHEMA.md`, `python_plugins/README.md`, `README.md`

**Technical Approach:**

1. **SENSOR_FUSION §1, lines 22-25.** "so it never runs by itself: it runs only
   when you ask for it from the plot list, in the background, while the
   application stays usable. [Plots that are computed on request](COMPUTED_PLOTS.md)
   explains the controls." becomes "so it runs only for what you have switched
   on - a checked fusion plot for the visible recordings, a logbook column over
   a fusion value for every recording - in the background, while the
   application stays usable. [Plots and logbook columns that are computed in the background](COMPUTED_PLOTS.md)
   explains what starts and stops a fit and what the rows and columns show."
2. **SENSOR_FUSION §2, lines 42-55.** Bullet 42-44 becomes: check a fusion plot
   and every visible recording without a result is fitted, one after another; a
   logbook column over a fusion value (roll at the exit marker, say) fits every
   recording of the logbook in the background. A fit takes from seconds to
   several minutes; all seventeen plots and every fusion column of one
   recording come from the same fit, so it runs once. Bullet 45-49 unchanged
   except "only means that the fits have to be requested again" -> "only means
   that the recordings are fitted again when something switched on needs
   them". Bullet 50-55: "the plot then shows the refresh icon again" becomes
   "while a fusion plot is checked for it or a fusion column is enabled, the
   recording is fitted again a moment after the change"; "Nothing is recomputed
   on its own." becomes "Nothing is fitted again unless something switched on
   needs it."
3. **SENSOR_FUSION §7.**
   - Lines 336-339 ("Unavailable until requested."): keep; "no read ever starts
     it" stays.
   - Lines 341-345 ("The plot is the request."): heading "**What is switched on
     is the request.**" Checking a fusion plot in any way (a click, Space, a
     profile), showing a recording while one is checked, or enabling a logbook
     column over a fusion value creates one job per recording that lacks the
     result; unchecking, hiding or disabling drops the ones that have not
     started, and a running fit always finishes and is stored. At start-up every
     recording is hidden, so checked plots start nothing until recordings are
     shown; an enabled fusion column continues its fill at once. Loading a
     recording whose stored result is still valid restores that result: no job.
   - Lines 355-359: "the job queue asks the fit to stop there and then" -> "the
     executor asks the fit to stop there and then"; "and the row shows the
     refresh control again at once; a refresh queues a new fit, which starts
     when the old one has stopped" -> "and the recording counts as waiting at
     once; a new fit starts by itself once the inputs have been still for about
     a second and the old one has stopped".
   - Lines 369-374 ("Outcomes."): "the recording stays "not computed"" becomes
     "it is shown with the warning badge and its reason, not tried again while
     the application runs unless an input changes, and tried again at the next
     start".
   - Lines 395-397 ("One at a time."): "Jobs run one after another in the order
     requested." becomes "Fits run one after another, below normal priority:
     the focused recording first, then the other visible recordings from top to
     bottom, then the recordings fusion columns need (CALCULATIONS.md, section
     16.6)." Keep the quitting sentence.
   - Lines 399-402 ("Logbook columns"): after the first sentence add "While
     such a column is enabled, every recording without a stored result is
     fitted in the background (recordings that are not loaded are loaded two at
     a time, as hidden recordings); its cell shows "…" until then." Replace "A
     recording without a stored result shows none." with "A recording without
     IMU data shows none." Keep the last sentence.
4. **SENSOR_FUSION §8 table, lines 417-419.** `tst_fusion_jobs`: "through the job
   queue" -> "through the executor". `tst_fusion_rows`: "the plot rows with the
   real fusion plots, end to end" -> "the plot rows with the real fusion plots,
   end to end: fits started and dropped by what is checked and visible, with no
   gesture". `tst_fusion_store`: append "; a logbook column over roll filled for
   recordings that are not loaded, and nothing fitted again after a restart".
5. **SENSOR_FUSION words to avoid** (group `fusion-model`): `frozen`,
   `stationary window`, `candidate window`, `coarse initializer`,
   `bias shifts below`, `zero bias shift`, `twenty-one`,
   `sensor-fusion-clean-port`; plus the words of Task 4.2.
6. **DATA_SCHEMA §11.**
   - Lines 359-371: after "For a session that is not loaded and has no cached
     value, the column is unavailable when the session has no stored result."
     insert: "While such a column is enabled, FlySight Viewer computes the
     requested calculation in the background for every session that has no
     stored result, loading sessions that are not loaded two at a time without
     showing them ([COMPUTED_PLOTS.md](COMPUTED_PLOTS.md), section 4); the
     cached value stays unavailable until the record is written, which drops
     it, and the value is then computed from the result like any other. Until
     then the logbook shows the cell as pending ("…"): that is a presentation of
     the view, never a cached value, and never written to `index.json`."
   - Lines 379-381: "shows such a column empty (pending) until it is loaded
     again" becomes "shows such a column empty (not cached; this is not the "…"
     of a value being computed) until it is loaded again".
7. **DATA_SCHEMA §12.**
   - Lines 420-423: "At the next start every requested calculation reads as not
     requested until it is requested again from the plot list, and the logbook
     column values that came from a stored result are dropped (section 11) and
     show as unavailable." becomes "At the next start every requested
     calculation reads as not computed, and is computed again for whatever is
     switched on (a checked plot for the visible sessions, an enabled column
     over it for every session); the logbook column values that came from a
     stored result are dropped (section 11) and show as pending while they are
     computed again."
   - Line 442: "and the calculation has to be requested again" -> "and the
     calculation is computed again when something switched on needs it".
   - Lines 514-518: "A stale or missing record leaves the calculation not
     computed until it is requested again from the plot list." becomes "A stale
     or missing record leaves the calculation not computed until something
     switched on needs it (a checked plot over a visible session, or an enabled
     logbook column over it), which computes it in the background."
   - Lines 519-525: after "nothing is requested or computed again" add "(an
     enabled column's missing results are computed by the application's demand
     for them, not by the worker: section 11)".
   - Lines 526-531: after "the calculation reads as not requested, and a
     warning is logged." add "While a checked plot or an enabled column needs
     it for that loaded session, it is computed again; the new publish replaces
     the record once the file can be written."
   - New bullet after the unreadable-record bullet (before 542): **Failures that
     are not a function of the inputs** - running out of memory, a worker that
     could not be started, a session file that could not be loaded - are never
     stored: they are shown until the application closes and tried again at the
     next start. **A stored rejection or solver failure** is a result: its
     reason is also recorded in `index.json` (section 11, `"recordReasons"`)
     when the record is written and whenever it is restored, so that a session
     that is not loaded is listed as "could not be computed" with that reason
     before and after a restart alike, without opening the record or loading
     the session.
   - **DATA_SCHEMA §11**, after the `"records"` paragraph and its example
     (lines 341-350): a paragraph and an example for `"recordReasons"`: "Each
     session entry may also have `"recordReasons"`: an object mapping the
     calculation id of a stored result that did not produce its outputs (a
     rejection or solver failure) to its reason, as the application learned it
     when the record was written or last restored. It is derived, additive and
     optional: an index without it means no reason has been learned yet, and a
     record of an earlier build reads as a success until it is next restored.
     Removing the record removes the entry. Nothing else in the entry changes."
     Example: `"recordReasons": {"builtin.fusion.fit": "No stationary window found"}`.
   - Lines 545-549 ("Guarantees."): "and only means that the calculation has to
     be requested again" -> "and only means that the calculation is computed
     again when something switched on needs it".
8. **python_plugins/README.md.** Lines 253-254: "and must be requested again"
   -> "and is computed again when a checked plot or an enabled logbook column
   needs it". Lines 270-271: "(they are requested again from the plot list)"
   -> "(they are computed again in the background while a checked plot or an
   enabled logbook column needs them)". (This file is excluded from every audit
   search; its wording is checked by this task's criteria only.)
9. **Root README.md** (keep the tree's comment column at column 44 and the
   `│   │` continuation pattern):
   - 322-323: `COMPUTED_PLOTS.md` comment "# User guide: plots and logbook
     columns computed in the" / "#   background (working indicator, badge,
     pending cells)".
   - 329-330: `CALCULATIONS.md` comment "... background" / "#   execution, the
     executor, the demand layer".
   - 356: "job queue, plot request logic; Qt Core + Gui, no" -> "executor,
     demand layer; Qt Core + Gui, no".
   - 367-370: `jobqueue.*, jobmodel.*` "# The executor of requested
     calculations" / "#   (one worker thread) and its Qt item model";
     `calculationdemand.*` (replacing `plotrequests.*`) "# The demand layer:
     what checked plots and" / "#   enabled columns need computed, what runs
     next".
   - 374-375: "(ui/docks/plotselection/PlotRow*: the plot list's" / "refresh /
     progress / cancel / warning controls)" -> "(PlotRow*, DemandIndicator.*,
     logbook/LogbookHeaderView.*," / "#   LogbookCellDelegate.*: working
     indicator, badge, pending cells)".
   - 416: "- [docs/COMPUTED_PLOTS.md](docs/COMPUTED_PLOTS.md): plots and logbook
     columns that are computed in the background - what starts and stops a
     computation, the working indicator and its count, the warning badge,
     pending logbook cells".
   - 423: "the job queue, stored results of requested calculations, plot-driven
     requests" -> "the executor, stored results of requested calculations, the
     demand layer".

**Acceptance Criteria:**
- [ ] `git grep -n -i -E "refresh (icon|control)|press(ing)? (the )?refresh|requested (again )?from the plot list|job queue|plot-driven|plotrequests" -- docs/SENSOR_FUSION.md docs/DATA_SCHEMA.md README.md python_plugins/README.md` prints nothing.
- [ ] DATA_SCHEMA §11 says the pending "…" is a view presentation never written to `index.json` and documents `"recordReasons"`; §12 has the non-input failures bullet and the stored-failure bullet, and no sentence says a warning waits for a load.
- [ ] The root README tree renders aligned and names `calculationdemand.*` and no `plotrequests.*`.
- [ ] `audit_cleanup` passes (`fusion-model`, `naming`, `stored-results`, `result-validity`, `demand`).

**Complexity:** M

---

### Task 4.5: `tests/acceptance_map.txt`: the range 501-563 and the amended 1xx and 3xx items

**Purpose:** Every clause of "Demand-driven requested calculations" is
traceable to evidence, individually, and the items it supersedes say what now
holds.

**Files to modify:**
- `tests/acceptance_map.txt`

**Technical Approach:**

1. **Header (lines 1-39).** "Five specifications, five item ranges" -> "Six
   specifications, six item ranges". After the 401-442 entry add:
   ```
   #   501-563   demand-driven requested calculations (demand from checked plots
   #             and enabled logbook columns, the executor, priority, hidden
   #             loads, the working indicator and pending cells): item = 500 +
   #             clause number. Stated in full in tests/README.md, appendix F.
   #             It amends the specifications of 101-120 and 301-350: items 110,
   #             111, 113-117, 304, 306, 321 and 337 are stated as amended.
   ```
   "Every item must lie in one of the five ranges" -> "six ranges"; "every item
   101-120, 201-247, 301-350 or 401-442" -> "every item 101-120, 201-247,
   301-350, 401-442 or 501-563"; "sections 9.1 to 9.5" -> "9.1 to 9.6".
2. **Amend in place** (replace the `#` line; the numbered lines stay, and the
   listed lines are added after the item's existing lines):
   - `# 110 - (as amended) cancelling a running job through the executor stops it at the next solver boundary, publishes nothing, leaves the calculation requestable; the chosen next job then starts`
   - `# 111 - (as amended) a session without IMU data is never waiting, running or failed, nor counted; no job can be created for it`
   - `# 113 - (as amended) B consumes A: checking a plot of B's output runs A then B with no other action`
   - `# 114 - (as amended) never more than one job at a time; the executor holds at most the running job and one chosen next job, and an offer equal to either creates no duplicate` and add `114 tst_jobqueue holdsAtMostRunningAndChosenNext`
   - `# 115 - (as amended) row behaviour without widgets, then with the real fusion plots: checking computes the visible tracks one after another, the count rising; unchecking mid-way drops the waiting ones and lets the running one finish; checking again resumes; a fourth track shown afterwards is computed with no other action; what the user sees` and add `115 tst_calculation_demand showingASessionStartsIt` and `115 tst_calculation_demand uncheckingDropsWaitingPairsKeepsRunning`
   - `# 116 - (as amended) starting the application with fusion plots checked starts no job, because every track starts hidden; applying a profile, the Plots menu or any programmatic check creates demand exactly as a click on the check box does`
   - `# 117 - (as amended) removing or unloading a session with a running or chosen next job, and quitting with a job running and one chosen next: no crash, no hang, nothing stale`
   - `# 304 - (2, as amended) a stored result is a memory, not a request: stale or missing, the calculation reads not requested until something switched on (a checked plot over a visible session, an enabled column over it) has it computed` and add `304 tst_result_columns staleRecordDeletedByWorkerCreatesDemand`
   - `# 306 - (2, as amended) the kernel is unchanged, and plot rows and columns count a restored result as computed: no working indicator, no job` and add `306 tst_calculation_demand storedResultsCreateNoJob`
   - `# 321 - (5, as amended) plot rows count a restored result as computed: no working indicator, no job; a stale or absent result is in demand like any missing one` and add `321 tst_calculation_demand storedResultsCreateNoJob`
   - `# 337 - (8, as amended) test: a fusion fixture fitted, saved, unloaded, reloaded: seventeen channels and diagnostics bit-identical to the goldens, no job, the row plain`
3. **Append this block at the end of the file, exactly** (one blank line before
   it). Item numbers follow appendix F (Task 4.7).

```
# ---- Demand-driven requested calculations: item = 500 + clause number ----

# 501 - (2) the user expresses intent through plots and logbook columns, never through calculations: what is switched on is the request
501 tst_calculation_demand rowScript
501 tst_calculation_demand programmaticCheckCreatesDemand
501 tst_calculation_demand enablingColumnFillsEveryUnloadedSession
501 tst_fusion_rows realRowScript
501 audit gestures
501 manual M3

# 502 - (2) anything needed to complete what is switched on is wanted at once; anything no longer needed is dropped
502 tst_calculation_demand showingASessionStartsIt
502 tst_calculation_demand hidingASessionDropsItsWaitingPair
502 tst_calculation_demand uncheckingDropsWaitingPairsKeepsRunning
502 tst_calculation_demand disablingColumnReleasesHeldSessions
502 tst_calculation_demand changingDemandReplacesChosenNext

# 503 - (2) finished work is never wasted: results are stored
503 tst_calculation_demand uncheckingDropsWaitingPairsKeepsRunning
503 tst_calculation_demand disablingColumnReleasesHeldSessions
503 tst_calculation_demand storedResultsCreateNoJob
503 tst_fusion_store fusionColumnWithStoredFitsRunsNothing

# 504 - (2) background work must not degrade the rest of the application
504 tst_jobqueue workerRunsBelowNormalPriority
504 tst_jobqueue mainThreadIsNotBlockedByARunningJob
504 tst_jobqueue idleSchedulerKeepsWorking
504 tst_calculation_demand passOverManyStubsReadsEachRecordSetOnce
504 tst_calculation_demand enablingColumnFillsEveryUnloadedSession
504 manual M27

# 505 - (2) a failure is shown, never retried in a loop
505 tst_calculation_demand inputDeterminedFailureIsStoredBadgedNeverRerun
505 tst_calculation_demand jobLevelFailureIsBadgedNotRerunUntilRestart
505 tst_calculation_demand columnFailuresAreBadgedNotReloaded
505 tst_calculation_demand notApplicableSessionIsSettledWithoutAJob
505 tst_calculation_demand unloadableSessionIsSettledAsFailed

# 506 - (2) each component's contract can be stated without naming the others (section 12 gives the contracts)
506 tst_calculation_demand nullCollaborators
506 tst_result_columns columnWorkerIsUnchangedByDemand
506 tst_session_model_engine schedulerTaskCanBeUnregistered
506 audit demand

# 507 - (3) the fusion kernel, its outputs and the record format are unchanged; the job history the jobs dock will read stays as it is
507 tst_fusion_golden successFixturesMatchGolden
507 tst_result_records layoutIsPinned
507 tst_jobmodel historyFromSignalsAlone
507 tst_fusion_store restoredAfterRestartIsBitIdentical

# 508 - (3) nothing about demand or the job history is persisted across restarts, and no user-facing switch pauses or throttles background work
508 tst_jobmodel nothingIsPersisted
508 tst_calculation_demand jobLevelFailureIsBadgedNotRerunUntilRestart
508 tst_calculation_demand fillTaskIsLowestAndNotCancellable
508 audit demand

# 509 - (5) plot demand: for every checked plot whose value is a requested output, every visible session needs the requested calculations that block that output
509 tst_calculation_demand rowScript
509 tst_calculation_demand ordinaryPlotsAreNeverInspected
509 tst_calculation_demand hiddenAndStubRowsAreNotTracks
509 tst_calculation_demand failedLoadPlaceholderIsNotATrack
509 tst_fusion_rows allSeventeenFusionPlotsAreExplicitBacked
509 tst_calculation_demand plotIdMatchesPlotModelRole
509 tst_calculation_demand uncheckedPlotsAreNeverInspected

# 510 - (5) column demand: for every enabled logbook column whose value depends on a requested output, every session in the logbook needs the requested calculations that block that value
510 tst_calculation_demand enablingColumnFillsEveryUnloadedSession
510 tst_calculation_demand loadedHiddenSessionsNeedNoLoad
510 tst_calculation_demand ordinaryColumnsCreateNoDemand
510 tst_calculation_demand columnIdIsTheDefinitionKey
510 tst_fusion_store columnOverFusionFillsUnloadedSessions
510 manual M24

# 511 - (5) a pair is in demand only while it has no result; a result counts whether published in this run or restored, success or input-determined failure
511 tst_calculation_demand onlyRequestableCalculationsAreOffered
511 tst_calculation_demand storedResultsCreateNoJob
511 tst_calculation_demand inputDeterminedFailureIsStoredBadgedNeverRerun
511 tst_calculation_demand columnFailuresAreBadgedNotReloaded
511 tst_fusion_store restoredRejectionShowsBadge
511 tst_fusion_store restoredAfterRestartIsBitIdentical

# 512 - (5) a pair whose calculation cannot apply (a declared input missing) is never in demand and is not reported anywhere
512 tst_calculation_demand sessionWithoutInputIsNeverListed
512 tst_calculation_demand notApplicableSessionIsSettledWithoutAJob
512 tst_jobqueue refusesMissingInput
512 tst_fusion_rows noImuSessionIsNeverCounted
512 tst_fusion_store columnOverFusionFillsUnloadedSessions

# 513 - (5) whether a result exists: blocker inspection for a loaded session; for a session not loaded the logbook's record names and the reasons the index recorded for them, no record opened; a cell has a result only when every calculation it needs has a record; a record with a reason is a failed result before and after a restart alike; a known record counts until the column worker's restore deletes it as stale, which moves the pair into demand; the demand layer checks no staleness itself
513 tst_calculation_demand storedResultsCreateNoJob
513 tst_calculation_demand chainedColumnWithUpstreamRecordIsCompleted
513 tst_calculation_demand storedRejectionIsBadgedAfterRestartWithoutLoad
513 tst_logbook_index recordReasonsRoundTrip
513 tst_result_store recordReasonRecordedAtWriteAndRestore
513 tst_calculation_demand passOverManyStubsReadsEachRecordSetOnce
513 tst_calculation_demand columnStateCountsAndPendingCells
513 tst_result_columns staleRecordDeletedByWorkerCreatesDemand
513 audit demand

# 514 - (5) demand does not depend on how the state arose (a gesture, a profile, the start-up restore); at start-up every session is hidden, so plots create no demand until sessions are shown, while enabled columns do
514 tst_calculation_demand programmaticCheckCreatesDemand
514 tst_calculation_demand profileStyleApplyCreatesDemand
514 tst_calculation_demand startupRestoreWithHiddenSessionsStartsNothing
514 tst_calculation_demand profileStyleColumnsCreateDemand
514 tst_calculation_demand startupWithEnabledColumnLoadsAfterColumnWorker
514 tst_plot_row_delegate programmaticCheckIsTheSameAsAClick
514 tst_plot_row_delegate startupRestoreWithHiddenSessionsStartsNothingWithViewAttached
514 manual M1
514 manual M2

# 515 - (5) nothing about demand is persisted: it is derived again at the next start; a profile carrying a column over a requested output computes it for every session without a result, so no default profile carries such a column
515 tst_calculation_demand jobLevelFailureIsBadgedNotRerunUntilRestart
515 tst_calculation_demand profileStyleColumnsCreateDemand
515 audit demand
515 manual M2

# 516 - (5) when a requested calculation depends on another, the demand covers both, upstream first
516 tst_calculation_demand chainedBlockersContinue
516 tst_calculation_demand heldChainContinues
516 tst_calculation_demand chainCompletesAfterFirstJobDoesNotSucceed
516 tst_calculation_demand chainedColumnKeepsItsHold
516 tst_calcengine_blockers chainedBlockers

# 517 - (6) a pair that enters demand is wanted at once, with no gesture (checking, showing, enabling, an input change that drops a demanded result), and the indicator shows it immediately
517 tst_calculation_demand showingASessionStartsIt
517 tst_calculation_demand loadingAVisibleSessionStartsIt
517 tst_calculation_demand mergeCreatesDemandForShownSessions
517 tst_calculation_demand plotCheckedDuringAJobJoinsIt
517 tst_calculation_demand enablingColumnFillsEveryUnloadedSession
517 tst_calculation_demand inputBurstRunsOneJob
517 tst_plot_row_delegate checkBoxClickChecksThroughTheModel
517 tst_plot_row_delegate spaceKeyChecksThroughTheModel

# 518 - (6) a pair that leaves demand (plot unchecked, session hidden, column disabled, result appeared by other means) is dropped before it starts; there is no queue of accepted requests to prune
518 tst_calculation_demand hidingASessionDropsItsWaitingPair
518 tst_calculation_demand uncheckingDropsWaitingPairsKeepsRunning
518 tst_calculation_demand waitingPairNeededByAnotherPlotSurvives
518 tst_calculation_demand chainStopsForHiddenTrackOrUncheckedPlot
518 tst_calculation_demand disablingColumnReleasesHeldSessions
518 tst_calculation_demand onlyRequestableCalculationsAreOffered
518 tst_calculation_demand resultAppearingWhileWaitingDropsThePair
518 tst_jobqueue withdrawEndsChosenNext
518 tst_plot_row_delegate uncheckByClickDropsWaitingWork
518 audit demand
518 manual M6
518 manual M26

# 519 - (6) the running job is never stopped because its pair left demand: it finishes and its result is published and stored; it is stopped only when its inputs change, its session goes away, or the application closes
519 tst_calculation_demand uncheckingDropsWaitingPairsKeepsRunning
519 tst_calculation_demand hidingASessionDropsItsWaitingPair
519 tst_calculation_demand disablingColumnReleasesHeldSessions
519 tst_calculation_demand removedSessionLeavesNoTrace
519 tst_jobqueue staleRunningJobIsStoppedAtOnce
519 tst_jobqueue removeSessionWithRunningJob
519 tst_jobqueue shutdownWithQueuedAndRunning
519 tst_fusion_rows realRowScript
519 audit gestures
519 manual M6

# 520 - (6) after an input change of a demanded session the pair is in demand at once but starts only once the inputs have been still for a short moment, so a burst of edits runs one job; showing, hiding, checking and enabling take effect without the wait
520 tst_calculation_demand inputBurstRunsOneJob
520 tst_calculation_demand staleRunningJobIsWaitingAtOnce
520 tst_calculation_demand supersededJobIsRunAgainAfterInputsSettle
520 tst_calculation_demand dependencyBurstIsCoalesced
520 tst_calculation_demand mergeCreatesDemandForShownSessions
520 tst_fusion_rows rejectedTrackShowsBadge
520 manual M18

# 521 - (6) jobs run one at a time on the executor's worker thread
521 tst_jobqueue oneAtATimeInOfferOrder
521 tst_jobqueue holdsAtMostRunningAndChosenNext
521 tst_jobqueue workerIsNotMainThreadAndHasLargeStack
521 tst_jobmodel neverMoreThanOneRunningRow
521 audit one-worker

# 522 - (7) the next job is chosen when a job ends and whenever demand changes, from the demand as it is at that moment, not from the order in which pairs entered it
522 tst_calculation_demand changingDemandReplacesChosenNext
522 tst_calculation_demand focusedSessionFirstThenRowOrder
522 tst_calculation_demand columnPriorityFollowsRowOrderAfterPlots
522 tst_jobqueue offerReplacesChosenNext

# 523 - (7) plot demand first (the focused session, then the other visible sessions in logbook row order), then column demand: visible sessions, then the other loaded sessions, then sessions not loaded as the fill loads them, each in logbook row order
523 tst_calculation_demand focusedSessionFirstThenRowOrder
523 tst_calculation_demand visibleSessionsFirstWithinColumnDemand
523 tst_calculation_demand columnPriorityFollowsRowOrderAfterPlots
523 tst_calculation_demand enablingColumnFillsEveryUnloadedSession

# 524 - (7) a session made visible while column demand is worked through is computed next, waiting at most for the running job, which is not preempted
524 tst_calculation_demand sessionShownDuringColumnDemandRunsNext
524 tst_calculation_demand columnPriorityFollowsRowOrderAfterPlots
524 manual M26

# 525 - (8) an input-determined failure (rejection, solver failure) is a result: stored, shown with the warning badge and its reason, never run again
525 tst_calculation_demand inputDeterminedFailureIsStoredBadgedNeverRerun
525 tst_calculation_demand columnFailuresAreBadgedNotReloaded
525 tst_fusion_rows rejectedTrackShowsBadge
525 tst_fusion_store restoredRejectionShowsBadge
525 tst_fusion_store restoredSolverFailureShowsBadge
525 manual M7

# 526 - (8) a job-level failure (the worker could not start, out of memory, a failure not a function of the inputs) is badged with its reason, not started again in the run unless its inputs change, not stored, so tried again at the next start; the demand layer remembers it
526 tst_calculation_demand jobLevelFailureIsBadgedNotRerunUntilRestart
526 tst_calculation_demand columnJobLevelFailureIsNotReloadedUntilRestart
526 tst_calculation_demand unloadableSessionIsSettledAsFailed
526 tst_calculation_demand visibleFailedLoadIsSettledAsFailed
526 tst_logbook_indicators failedLoadSessionShowsBadgeNotPending
526 tst_jobqueue workerStartFailureFails
526 tst_jobqueue resourceExhaustionFails
526 manual M28

# 527 - (8) there is no retry control
527 tst_plot_row_delegate clickOnClusterIsAClickOnTheRow
527 tst_logbook_indicators clickOnIndicatorIsAClickOnTheSection
527 audit gestures
527 audit demand

# 528 - (9) for column demand the demand layer, not the column worker, has a session that is not loaded loaded the way showing it would, without making it visible: an ordinary hidden session, pinned from its load until no column it needs is still waiting or running, then left to ordinary eviction
528 tst_calculation_demand enablingColumnFillsEveryUnloadedSession
528 tst_calculation_demand chainedColumnKeepsItsHold
528 tst_calculation_demand heldSessionShownStaysLoaded
528 tst_calculation_demand removedOrRepopulatedHeldSessionIsReleased
528 tst_calculation_demand demandDestroyedReleasesHoldsAndTask
528 tst_column_cache loadPinnedSessionLoadsWithoutShowing
528 tst_column_cache loadPinnedSessionFailedLoadPinsNothing
528 tst_calculation_demand noLoadsAfterExecutorShutdown

# 529 - (9) at most a small fixed number of sessions is loaded for this purpose at a time, not smaller than the number of jobs that may run at once; the next is loaded when one has ended
529 tst_calculation_demand enablingColumnFillsEveryUnloadedSession
529 tst_calculation_demand sessionShownDuringColumnDemandRunsNext
529 tst_calculation_demand columnPriorityFollowsRowOrderAfterPlots
529 audit demand
529 manual M24

# 530 - (9) the fill is an idle-scheduler task below saving, loading visible sessions, bulk edits and column work, whose steps are the loads, so saves and bulk edits come first; it has work for the whole fill and steps only while it can load; the progress line reports the whole fill; the scheduler gains the generic notion of a task with work it cannot step right now, rests instead of spinning, and learns nothing about jobs
530 tst_calculation_demand savesAndBulkEditsPrecedeLoadStep
530 tst_calculation_demand startupWithEnabledColumnLoadsAfterColumnWorker
530 tst_calculation_demand fillTaskIsLowestAndNotCancellable
530 tst_calculation_demand fillTaskReportsProgressWhileWaiting
530 tst_session_model_engine schedulerWaitingTaskDoesNotSpin
530 tst_calculation_demand bulkEditMakesSettledSessionApplicable
530 tst_session_model_engine schedulerTaskCanBeUnregistered
530 audit demand
530 manual M24

# 531 - (9) the session-id correction of a first load happens before the pair is offered to the executor
531 tst_calculation_demand identityStubIsOfferedUnderItsRealId
531 tst_column_cache loadPinnedSessionFollowsIdentityRemap

# 532 - (9) a session whose requested calculation turns out not to apply once loaded is settled as not applicable for the run without a job; its column value stays unavailable
532 tst_calculation_demand notApplicableSessionIsSettledWithoutAJob
532 tst_calculation_demand enablingColumnFillsEveryUnloadedSession
532 tst_fusion_store columnOverFusionFillsUnloadedSessions

# 533 - (9) the column worker is unchanged and never knows a job exists; a record written later drops the cached value and the loaded-row refresh computes the new one; cheap column values never wait for a requested calculation
533 tst_result_columns columnWorkerIsUnchangedByDemand
533 tst_result_columns staleRecordDeletedByWorkerCreatesDemand
533 tst_calculation_demand enablingColumnFillsEveryUnloadedSession
533 tst_logbook_indicators pendingCellBecomesValueWhenRecordIsWritten
533 audit demand

# 534 - (10) no refresh and no cancel for requested calculations anywhere; unchecking a plot, hiding sessions or disabling a column is how the user changes what is wanted
534 tst_plot_row_delegate clickOnClusterIsAClickOnTheRow
534 tst_logbook_indicators clickOnIndicatorIsAClickOnTheSection
534 tst_logbook_indicators pendingCellBecomesValueWhenRecordIsWritten
534 audit gestures
534 audit demand
534 manual M4
534 manual M6

# 535 - (10) a plot row and a logbook column header show a small animated working indicator at the right of their name while any of their demand is waiting or running
535 tst_plot_row_delegate workingRowPaintsIndicator
535 tst_plot_row_delegate workingIndicatorAnimatesOnlyWhileWorking
535 tst_plot_row_delegate workingAnimationClock
535 tst_logbook_indicators workingColumnShowsIndicatorRightOfText
535 tst_logbook_indicators indicatorAnimatesOnlyWhileWorking
535 tst_logbook_indicators indicatorFollowsColumnWhenMovedHiddenOrReordered
535 tst_logbook_indicators indicatorClearsSortArrowAndNarrowSections
535 tst_calculation_demand workingIdsFollowStates
535 tst_plot_row_delegate plotStateChangeRepaintsRow
535 tst_logbook_indicators plainHeaderAndCellsAreIdenticalToBase
535 manual M3
535 manual M23

# 536 - (10) hovering the indicator, the row or the header shows how many sessions are done of how many are wanted, the session being computed with its progress text, and the sessions that could not be computed with their reasons
536 tst_calculation_demand tooltipText
536 tst_calculation_demand toolTipListsAtMostTenFailures
536 tst_calculation_demand sharedJobSameProgress
536 tst_calculation_demand progressUpdatesWithoutInspection
536 tst_calculation_demand columnStateCountsAndPendingCells
536 tst_plot_row_delegate hoverDetailFollowsDemandState
536 tst_plot_row_delegate toolTipComesFromPlotState
536 tst_logbook_indicators headerToolTipFollowsDemandState
536 tst_calculation_demand visibleFailedLoadIsSettledAsFailed
536 manual M23

# 537 - (10) the warning badge replaces the indicator once work is finished and some sessions could not be computed; its hover lists them with reasons
537 tst_calculation_demand failuresListedWhileWorking
537 tst_plot_row_delegate badgeReplacesIndicatorOnceFinished
537 tst_logbook_indicators badgeReplacesIndicatorWhenFinished
537 tst_fusion_rows rejectedTrackShowsBadge
537 tst_logbook_indicators failedLoadSessionShowsBadgeNotPending
537 manual M7

# 538 - (10) a column cell whose pair is in demand reads as pending, distinct from unavailable and from the unreadable-record pending state; pending is the view's presentation of demand, never a cached value, never in the index; the value underneath stays unavailable until the record is written, and sorting treats pending as unavailable
538 tst_logbook_indicators pendingCellsAreDistinctFromUnavailable
538 tst_logbook_indicators pendingCellBecomesValueWhenRecordIsWritten
538 tst_logbook_indicators sortingTreatsPendingAsUnavailable
538 tst_logbook_indicators unreadableRecordPendingIsNotDemandPending
538 tst_logbook_indicators columnStateChangeRepaintsOnlyThatColumn
538 tst_calculation_demand columnStateCountsAndPendingCells
538 audit demand
538 manual M25

# 539 - (10) the logbook's progress line for background work is unchanged in form and reports a column fill for its whole duration, as on-demand column values: remaining of wanted, no cancel
539 tst_calculation_demand fillTaskIsLowestAndNotCancellable
539 tst_calculation_demand fillTaskReportsProgressWhileWaiting
539 tst_logbook_indicators pendingCellBecomesValueWhenRecordIsWritten
539 manual M24

# 540 - (11) the executor's worker thread runs below normal priority, so that the user interface stays responsive
540 tst_jobqueue workerRunsBelowNormalPriority
540 tst_jobqueue mainThreadIsNotBlockedByARunningJob
540 audit demand
540 manual M27

# 541 - (11) the number of jobs that may run at once is a single bound of the executor (one today); the load bound of section 9 follows it, and raising it changes no contract of the demand layer or the column worker
541 tst_jobqueue holdsAtMostRunningAndChosenNext
541 tst_calculation_demand enablingColumnFillsEveryUnloadedSession
541 audit demand

# 542 - (12) one widget-free demand layer replaces the plot rows' request logic: it reads the plot model, the session model, the enabled columns, blocker reports and the record set, derives demand, applies the priority, has sessions loaded, offers the next pair, remembers this run's failures and not-applicable pairs, and publishes per-plot and per-column state
542 tst_calculation_demand nullCollaborators
542 tst_calculation_demand changeSignalsAreMinimal
542 tst_calculation_demand registryChangeReclassifies
542 tst_calculation_demand removedSessionLeavesNoTrace
542 audit widget-free-core
542 audit demand

# 543 - (12) the demand layer is the only caller of the executor; nothing calls back into it: it observes the models and the executor's signals
543 tst_calculation_demand survivesExecutorShutdown
543 tst_calculation_demand nullCollaborators
543 tst_plot_row_delegate survivesDemandDestroyedFirst
543 tst_logbook_indicators survivesDemandDestroyedFirst
543 audit gestures
543 audit demand

# 544 - (12) the executor stays the only place a requested calculation runs and never loads a session; it holds at most the running job and one chosen next job, with no order of arrival, no deduplication against a list and no pruning
544 tst_jobqueue holdsAtMostRunningAndChosenNext
544 tst_jobqueue offerReplacesChosenNext
544 tst_jobqueue withdrawEndsChosenNext
544 tst_jobqueue duplicateOffersCreateNoDuplicates
544 tst_jobqueue neverLoadsASession
544 tst_calculation_demand executorHoldsAtMostRunningAndChosenNext
544 audit gestures
544 audit demand

# 545 - (12) the executor's lifecycle (start, stale while running, cancel, supersede, fail, shutdown), its pinning, the publication and storing of results, and the job history are unchanged
545 tst_jobqueue runsAndPublishes
545 tst_jobqueue staleRunningJobIsStoppedAtOnce
545 tst_jobqueue offerWhileStaleJobWindsDown
545 tst_jobqueue cancelRunningThenNextStarts
545 tst_jobqueue evictionDeferredWhileJobActive
545 tst_jobqueue shutdownWithQueuedAndRunning
545 tst_jobmodel historyFromSignalsAlone
545 tst_result_store writesOnOkInstall
545 tst_fusion_jobs jobPublishesAllOutputsTogether
545 tst_jobqueue offerWhileCancellingCreatesNewJob
545 tst_jobqueue shutdownIsIdempotentAndRefusesOffers

# 546 - (12) the column worker and the idle scheduler keep their tasks and priorities; the column fill is a new lowest-priority task; the scheduler gains the notion of a task with work it cannot step right now and no knowledge of jobs; the manager and the store record each stored result's outcome in the index
546 tst_result_columns columnWorkerIsUnchangedByDemand
546 tst_calculation_demand fillTaskIsLowestAndNotCancellable
546 tst_session_model_engine schedulerTaskCanBeUnregistered
546 tst_session_model_engine schedulerWaitingTaskDoesNotSpin
546 tst_result_store recordReasonRecordedAtWriteAndRestore
546 audit branch-mechanisms
546 audit demand

# 547 - (12) the flow is one way: the demand layer chooses, the executor publishes, the listener writes the record, the record change drops cached column values, the loaded-row refresh recomputes them, and the demand layer sees the result through the inspection and record set it always reads
547 tst_result_columns staleRecordDeletedByWorkerCreatesDemand
547 tst_calculation_demand enablingColumnFillsEveryUnloadedSession
547 tst_logbook_indicators pendingCellBecomesValueWhenRecordIsWritten
547 tst_fusion_store columnOverFusionFillsUnloadedSessions
547 audit demand

# 548 - (12) what stays true: restoring is not requesting; the column worker's temporary copy never writes a stored result and never requests; a restored result is indistinguishable from a published one; the engine's threading rules; the plot widget's "no data" warning asks the engine directly
548 tst_calculation_demand storedResultsCreateNoJob
548 tst_calculation_demand merelyUncomputedIsNotWorthAWarning
548 tst_result_columns columnWorkerIsUnchangedByDemand
548 tst_fusion_store restoredAfterRestartIsBitIdentical
548 tst_fusion_session restoredFitIsIndistinguishable
548 audit stored-results
548 audit one-worker

# 549 - (13) test: checking a plot starts the visible sessions without a result; showing another session while it is checked starts it with no other action; hiding drops its waiting pair; unchecking drops all waiting pairs; the running job finishes and its result is stored
549 tst_calculation_demand rowScript
549 tst_calculation_demand showingASessionStartsIt
549 tst_calculation_demand hidingASessionDropsItsWaitingPair
549 tst_calculation_demand uncheckingDropsWaitingPairsKeepsRunning
549 tst_fusion_rows realRowScript

# 550 - (13) test: a session made visible during column demand is the next job to start; within column demand visible sessions come before hidden loaded ones, and those before sessions not yet loaded
550 tst_calculation_demand sessionShownDuringColumnDemandRunsNext
550 tst_calculation_demand visibleSessionsFirstWithinColumnDemand
550 tst_calculation_demand columnPriorityFollowsRowOrderAfterPlots

# 551 - (13) test: enabling a column over a requested output wants every session without a result, loads unloaded ones a bounded number at a time as hidden pinned sessions, fills the column, ends with every session computed, not applicable or failed, and they leave the pool by ordinary eviction
551 tst_calculation_demand enablingColumnFillsEveryUnloadedSession
551 tst_calculation_demand notApplicableSessionIsSettledWithoutAJob
551 tst_calculation_demand columnFailuresAreBadgedNotReloaded
551 tst_calculation_demand unloadableSessionIsSettledAsFailed
551 tst_fusion_store columnOverFusionFillsUnloadedSessions

# 552 - (13) test: the column worker's behaviour and statistics are unchanged by column demand, and a stale record it deletes moves the pair into demand
552 tst_result_columns columnWorkerIsUnchangedByDemand
552 tst_result_columns staleRecordDeletedByWorkerCreatesDemand

# 553 - (13) test: stored results are restored, not recomputed: enabling the column on a logbook whose sessions all have valid stored results creates no job; a stored rejection of a session not loaded is listed as failed with its reason after a restart without loading it; a column over a chain whose upstream record alone is stored is completed
553 tst_calculation_demand storedResultsCreateNoJob
553 tst_calculation_demand storedRejectionIsBadgedAfterRestartWithoutLoad
553 tst_calculation_demand chainedColumnWithUpstreamRecordIsCompleted
553 tst_fusion_store fusionColumnWithStoredFitsRunsNothing

# 554 - (13) test: an input-determined failure is stored, badged and never run again; a job-level failure is badged, not run again in the run, and tried again after a restart
554 tst_calculation_demand inputDeterminedFailureIsStoredBadgedNeverRerun
554 tst_calculation_demand jobLevelFailureIsBadgedNotRerunUntilRestart
554 tst_calculation_demand columnFailuresAreBadgedNotReloaded
554 tst_calculation_demand columnJobLevelFailureIsNotReloadedUntilRestart

# 555 - (13) test: a burst of input changes on a demanded session produces one job; the indicator shows from the first change
555 tst_calculation_demand inputBurstRunsOneJob
555 tst_calculation_demand staleRunningJobIsWaitingAtOnce

# 556 - (13) test: applying a profile with such a column creates demand; start-up with a checked plot and no visible sessions creates none
556 tst_calculation_demand profileStyleColumnsCreateDemand
556 tst_calculation_demand profileStyleApplyCreatesDemand
556 tst_calculation_demand startupRestoreWithHiddenSessionsStartsNothing
556 tst_plot_row_delegate startupRestoreWithHiddenSessionsStartsNothingWithViewAttached
556 manual M2

# 557 - (13) test: the working indicator and hover detail reflect waiting, running, done and failed counts on plot rows and column headers; no refresh or cancel control exists
557 tst_calculation_demand columnStateCountsAndPendingCells
557 tst_plot_row_delegate workingIndicatorAnimatesOnlyWhileWorking
557 tst_plot_row_delegate badgeReplacesIndicatorOnceFinished
557 tst_plot_row_delegate hoverDetailFollowsDemandState
557 tst_plot_row_delegate clickOnClusterIsAClickOnTheRow
557 tst_logbook_indicators workingColumnShowsIndicatorRightOfText
557 tst_logbook_indicators badgeReplacesIndicatorWhenFinished
557 tst_logbook_indicators headerToolTipFollowsDemandState
557 tst_logbook_indicators clickOnIndicatorIsAClickOnTheSection
557 tst_logbook_indicators failedLoadSessionShowsBadgeNotPending

# 558 - (13) test: a pending column cell is distinguishable from an unavailable one, is not written to the logbook index, and becomes the value when the record is written
558 tst_logbook_indicators pendingCellsAreDistinctFromUnavailable
558 tst_logbook_indicators pendingCellBecomesValueWhenRecordIsWritten
558 tst_logbook_indicators sortingTreatsPendingAsUnavailable
558 tst_logbook_indicators unreadableRecordPendingIsNotDemandPending

# 559 - (13) test: saves and bulk edits still precede the fill's loads; no result is computed from a file being rewritten; the progress line reports the fill from its first load to its last result, and the scheduler does not spin while the fill waits on a job
559 tst_calculation_demand savesAndBulkEditsPrecedeLoadStep
559 tst_calculation_demand startupWithEnabledColumnLoadsAfterColumnWorker

# 560 - (13) test: the executor never holds more than the running job and one chosen next job; changing demand replaces the chosen next job
560 tst_jobqueue holdsAtMostRunningAndChosenNext
560 tst_jobqueue offerReplacesChosenNext
560 tst_calculation_demand executorHoldsAtMostRunningAndChosenNext
560 tst_calculation_demand changingDemandReplacesChosenNext

# 561 - (13) test: the UI thread is not blocked by a running requested calculation, and the calculation runs below normal priority
561 tst_jobqueue mainThreadIsNotBlockedByARunningJob
561 tst_jobqueue workerRunsBelowNormalPriority

# 562 - (13) test: the existing tests of stored results and of the executor pass, those that asserted the refresh gesture, the queue order or the pruning of queued jobs rewritten to the demand rules
562 tst_jobqueue oneAtATimeInOfferOrder
562 tst_jobqueue duplicateOffersCreateNoDuplicates
562 tst_fusion_rows realRowScript
562 tst_fusion_store dependencyEditDropsRecord
562 tst_fusion_store codeStampChangeDropsRecordOnLoad
562 tst_plot_row_layout indicatorOnly
562 tst_plot_row_layout indicatorAndWarning
562 audit gestures

# 563 - (14) docs/ describe the executor, the demand layer, logbook columns over requested results, what the user sees (no refresh, the working indicator, pending cells, failures) and where a column over a requested calculation stays unavailable or pending
563 audit demand
```

4. Every function above is one that Phases 1-3 name, or one that already
   exists (checked at `b55869d`: `idleSchedulerKeepsWorking`,
   `refusesMissingInput`, `workerIsNotMainThreadAndHasLargeStack`,
   `staleRunningJobIsStoppedAtOnce`, `removeSessionWithRunningJob`,
   `shutdownWithQueuedAndRunning`, `workerStartFailureFails`,
   `resourceExhaustionFails`, `neverLoadsASession`, `runsAndPublishes`,
   `cancelRunningThenNextStarts`, `evictionDeferredWhileJobActive`
   (`tst_jobqueue`); `historyFromSignalsAlone`, `nothingIsPersisted`,
   `neverMoreThanOneRunningRow` (`tst_jobmodel`); the `tst_fusion_rows`,
   `tst_fusion_store`, `tst_fusion_jobs`, `tst_fusion_golden`,
   `tst_fusion_session`, `tst_result_records`, `tst_result_store` and
   `tst_calcengine_blockers` names cited). Data-driven functions are cited by
   their base name. If one does not exist under that name in the committed
   test file, report it under "Deviations"; never delete a line to make the
   audit pass.

**Acceptance Criteria:**
- [ ] The header describes six ranges, including 501-563, and names the amended items.
- [ ] Items 110, 111, 113-117, 304, 306, 321 and 337 carry the "as amended" `#` lines above, and the six added lines exist (114 x1, 115 x2, 304, 306, 321).
- [ ] `grep -c "^5[0-9][0-9] " tests/acceptance_map.txt` prints 344 (the lines of the block), and every item 501-563 has at least one test or audit line.
- [ ] Every new or renamed test function of Phases 1-3 appears in at least one 1xx, 3xx or 5xx line (Task 4.9 step 3 lists the check).
- [ ] No line lies outside 1-19, 101-120, 201-247, 301-350, 401-442 and 501-563; `audit_cleanup` passes.

**Complexity:** M

---

### Task 4.6: `tests/audit/cleanup_audit.cmake`: group `demand`, the widget-free glyphs, the range 501-563

**Purpose:** Keep the boundaries of spec §12 (and the §5 profile note, the §10
"no refresh, no cancel", the §11 bound and priority) in place with text rules
that pass on the Phase 1-3 code as documented and fail on today's code, and
extend the traceability checker to the new range.

**Files to modify:**
- `tests/audit/cleanup_audit.cmake`

**Technical Approach:**

1. **Header (lines 1-28).** After the result-validity bullet add:
   ```
   #   - what is switched on is the request: one widget-free demand layer is the
   #     only caller of the executor, nothing below it knows it, the views only
   #     read it, "pending" never reaches the model or the index, the executor
   #     has one bound of jobs on a below-normal worker, no default profile
   #     carries a column over a requested output, and no refresh, cancel, queue
   #     or plot-request logic remains (items 501-563).
   ```
2. **Group `widget-free-core`** (the one `expect_none` Phase 1 left over
   `"src/jobqueue.*" "src/jobmodel.*" "src/calculationdemand.*"
   "src/plotmodel.*" src/ui/docks/plotselection/PlotRowLayout.h`): extend the
   pattern to
   `QtWidgets|#include <Q(Widget|TreeView|AbstractItemView|StyledItemDelegate|Application|ToolTip|Style[A-Za-z]*|HeaderView)>`
   and add `"src/ui/docks/DemandIndicator.*"` to the pathspec. Rule count
   unchanged. (Phase 3: the glyphs are Qt Core and Gui only, no style option.)
3. **New block** after the `result-validity` group and before "leftover
   markers", in the style of the existing blocks (`=====` banner, `─────` group
   line, an `Allow:` comment on every rule):

```cmake
# =============================================================================
# Demand-driven requested calculations (acceptance items 501-563): what is
# switched on - checked plots for the visible sessions, enabled logbook columns
# for every session - is the request. One widget-free demand layer derives what
# to compute and is the only caller of the executor (group gestures). The rules
# below keep everything below it ignorant of it, the views read-only, the
# model, the index and the scheduler free of jobs and of "pending", one bound,
# a below-normal worker, the default profiles free of columns over requested
# outputs, and no refresh, cancel, queue or plot-request logic in the code or
# the documents.
# =============================================================================

# ─────────────────────────────── demand (items 506, 508, 513, 515, 518, 527, 529, 530, 533, 534, 538, 540-544, 546, 547, 563)
audit_group(demand)
# Allow: a new view that presents the demand layer is added to the allowed-file
# regex; nothing below the demand layer (the executor, the session model, the
# scheduler, the logbook) ever is. Elsewhere a comment says "the demand layer".
expect_only("only the application and its views know the demand layer" "CalculationDemand"
  "^src/calculationdemand\\.(cpp|h)$|^src/mainwindow\\.(cpp|h)$|^src/ui/docks/AppContext\\.h$|^src/ui/docks/plotselection/(PlotRowDelegate\\.(cpp|h)|PlotSelectionDockFeature\\.cpp)$|^src/ui/docks/logbook/(LogbookView|LogbookHeaderView|LogbookCellDelegate)\\.(cpp|h)$|^src/ui/docks/logbook/LogbookDockFeature\\.cpp$|^src/ui/docks/plot/PlotWidget\\.cpp$"
  src)
# Allow: none expected. The layers below the demand layer, the shared glyphs
# and the row layout never include it (so they can use none of its types).
expect_none("nothing below the demand layer includes it" "#include [\"<](\\.\\./)*calculationdemand\\.h"
  "src/jobqueue.*" "src/jobmodel.*" "src/sessionmodel.*" "src/idlescheduler.*" "src/logbookmanager.*"
  "src/logbookcolumn.*" "src/plotmodel.*" "src/profilestatebridge.*" "src/calculationresultstore.*"
  src/engine "src/ui/docks/DemandIndicator.*" src/ui/docks/plotselection/PlotRowLayout.h)
# Allow: none expected. The views read plotState, columnState, isCellPending
# and working*Ids; a pass, the load step and the settle seams belong to the
# demand layer and to tests.
expect_none("the views only read the demand layer" "[.>](flush|runLoadStep|endInputSettleWaits|setInputSettleDelay)\\("
  src/ui "src/mainwindow.*")
# Allow: none expected. Nothing the demand views paint is a control: the base
# classes handle every click and key (tooltips come from helpEvent /
# viewportEvent, which this rule does not name). Say "no event of its own" in
# a comment instead of naming a handler.
expect_none("the demand views handle no event of their own"
  "editorEvent|mouse(Press|Release|DoubleClick|Move)Event|keyPressEvent"
  "src/ui/docks/plotselection/PlotRowDelegate.*" "src/ui/docks/logbook/LogbookHeaderView.*"
  "src/ui/docks/logbook/LogbookCellDelegate.*" "src/ui/docks/DemandIndicator.*")
# Pending is a presentation of demand: the demand layer answers it and the cell
# delegate paints it; the model, its cached values and index.json never see it.
# Allow: none expected.
expect_only("pending is the view's presentation of demand" "isCellPending|showsPending|pendingText\\(|pendingToolTip\\("
  "^src/calculationdemand\\.(cpp|h)$|^src/ui/docks/logbook/LogbookCellDelegate\\.(cpp|h)$" src)
# Allow: none expected. The model knows pinned ids only; the logbook, the
# column store, the plot model and the scheduler know nothing about jobs.
# Comments say "the executor".
expect_none("the model, the logbook and the scheduler know nothing of the executor"
  "JobQueue|JobModel|jobqueue\\.h|jobmodel\\.h"
  "src/sessionmodel.*" "src/logbookmanager.*" "src/logbookcolumn.*" "src/idlescheduler.*" "src/plotmodel.*")
# Allow: none expected. The idle scheduler runs the steps of registered tasks;
# the load step is one more task and tells it nothing.
expect_none("the idle scheduler learns nothing about jobs or demand" "[Dd]emand|[Cc]alculation|[Jj]ob|[Ee]xecutor"
  src/idlescheduler.cpp src/idlescheduler.h)
# The load step is the demand layer's task: the enum names it, the progress
# line labels it, and the session model never registers or runs it.
# Allow: none expected.
expect_only("the load step is the demand layer's scheduler task" "ColumnFillTask"
  "^src/calculationdemand\\.(cpp|h)$|^src/sessionmodel\\.h$|^src/ui/docks/logbook/LogbookView\\.cpp$" src)
# Hidden loads for column demand go through the one entry that loads without
# showing and pins under the corrected id. Allow: none expected.
expect_only("hidden loads go through loadPinnedSession" "loadPinnedSession\\("
  "^src/calculationdemand\\.(cpp|h)$|^src/sessionmodel\\.(cpp|h)$" src)
# The demand layer reads record names only; loading, restoring and reading
# records belong to the session model and the result store. Allow: reword a
# comment that names one of these calls ("loaded the way showing it would").
expect_none("the demand layer never loads a session or reads a record itself"
  "sessionRef\\(|loadSession\\(|readCalculationRecord|calculationRecordIds\\(|restoreSession\\(|restoreStoredResults\\("
  "src/calculationdemand.*")
# One bound of simultaneous jobs, and the load bound follows it (spec 11).
# Allow: change the number only together with the executor's run slots.
expect_count("one bound of simultaneous jobs" "kMaxRunningJobs *=[^=]" 1 src)
expect_count("the load bound follows the executor's bound"
  "kMaxHeldSessions *= *JobQueue::kMaxRunningJobs *\\+ *1" 1 src/calculationdemand.h)
# Allow: none expected. The worker runs below normal priority (spec 11).
expect_count("the worker runs below normal priority" "start\\(QThread::LowPriority\\)" 1 src/jobqueue.cpp)
# The executor is not a queue. Allow: none expected (tests/README.md is not
# searched: its section 10 spells these names).
expect_none("the executor keeps no queue" "oldestQueued|cancelUnwantedQueued|cancelSession\\(|cancelAll\\(|RequestResult"
  src docs README.md)
expect_none("the plot request logic is gone"
  "PlotRequests|plotrequests|plotRequests|PlotRowState|PlotTrackCondition|tst_plot_requests"
  src tests docs cmake CMakeLists.txt README.md ":!tests/README.md")
# No refresh and no cancel for requested calculations anywhere (spec 10).
# Allow: say "there is no refresh and no cancel"; never name the old controls.
expect_none("no refresh or cancel control in code or documents"
  "[Rr]efresh (icon|control|gesture)|press(es|ed|ing)? (the )?refresh|[Cc]ancel (control|icon)|circled x"
  src docs README.md)
expect_none("the plot rows' controls are gone"
  "drawRefreshGlyph|drawCancelGlyph|Control::(Refresh|Cancel)|controlHit|controlCount\\(|controlRect\\("
  src tests ":!tests/README.md")
# Applying a profile that carries a column over a requested output computes it
# for the whole logbook, so no default profile carries one (spec 5). Allow: a
# new requested calculation adds its sensor or attribute names to the pattern.
expect_none("no default profile carries a column over a requested output"
  "\"(sensorID|attributeKey|markerAttributeKey|marker2AttributeKey)\": *\"(Fusion|_FUSION)"
  src/resources/profiles)
```

   Why each rule holds for the documented Phase 1-3 code, and fails on today's
   (`b55869d`) code:
   - **Who knows `CalculationDemand`.** Phase 1 names it in `mainwindow.{h,cpp}`,
     `AppContext.h`, `PlotRowDelegate.*`, `PlotWidget.cpp` and its own files;
     Phase 3 in `LogbookView.*`, `LogbookHeaderView.*`, `LogbookCellDelegate.*`.
     `PlotSelectionDockFeature.cpp` and `LogbookDockFeature.cpp` pass
     `ctx.calculationDemand` (lower-case c, no match) and are allowed anyway.
     `src/CMakeLists.txt` says `calculationdemand.cpp` (no match).
     `PlotRowLayout.h` says `DemandState (calculationdemand.h)`,
     `sessionmodel.h` and `DemandIndicator.h` say `calculationdemand.h` in
     words (no match). Today no file names the class; a regression (the
     executor or the model calling back) is a hit outside the list.
   - **Includes.** Only the demand layer's own `.cpp`, `mainwindow.cpp`,
     `PlotWidget.cpp`, `PlotRowDelegate.*` and the logbook views include it.
   - **Read-only views.** No `.flush(` / `->flush(` exists in `src/ui` or
     `mainwindow.*` today (checked); Phase 3's views call only the queries.
   - **Event handlers.** Today `PlotRowDelegate.{h,cpp}` define `editorEvent`
     (a hit); Phase 1 deletes it; Phase 3's header view overrides
     `paintSection`, `sectionSizeFromContents` and `viewportEvent`, and the
     cell delegate `paint` and `helpEvent` only.
   - **Pending API.** Declared and defined in `calculationdemand.*` (Phase 2)
     and `LogbookCellDelegate.*` (Phase 3); `SessionModel` has
     `pendingColumns`, which does not match.
   - **Executor names.** No `JobQueue` / `JobModel` in `sessionmodel.*`,
     `logbookmanager.*`, `logbookcolumn.*`, `idlescheduler.*` or `plotmodel.*`
     today (checked); Phase 2's comments there say "the executor".
   - **Scheduler words.** `idlescheduler.{h,cpp}` contain none of the four
     words today (checked); Phase 2's `unregisterTask` documentation is
     generic.
   - **`ColumnFillTask`.** Phase 2: the enum in `sessionmodel.h`, the
     registration in `calculationdemand.cpp`, the label in `LogbookView.cpp`;
     `SessionModel` never registers it (Phase 2 criterion).
   - **`loadPinnedSession(`.** Phase 2: declared and defined in
     `sessionmodel.{h,cpp}`, called from `calculationdemand.cpp`, named in the
     demand layer's comment.
   - **No own loads or record reads.** Phase 2: the demand layer uses
     `knownCalculationRecords()` and `loadPinnedSession()` only (its gotcha
     "the unloaded rule must never open a record").
   - **Bounds.** Phase 1: `static constexpr int kMaxRunningJobs = 1;` in
     `jobqueue.h` (the `static_assert(kMaxRunningJobs == 1, ...)` and
     `runningCount() < kMaxRunningJobs` do not match `*=[^=]`); Phase 2:
     `static constexpr int kMaxHeldSessions = JobQueue::kMaxRunningJobs + 1;`.
     Today both counts are 0 (hits).
   - **Priority.** Phase 1: `m_run->worker->start(QThread::LowPriority)`;
     today `start()` (count 0, a hit).
   - **Queue API, `PlotRequests`, refresh wording.** Today `jobqueue.*`,
     `plotrequests.*`, `PlotRowDelegate.*`, `PlotWidget.cpp:535`,
     `PlotSelectionDockFeature.cpp:28`, the map and the tests, and
     `docs/CALCULATIONS.md`, `docs/COMPUTED_PLOTS.md`,
     `docs/SENSOR_FUSION.md` and `README.md` hit; Phases 1-3 clean `src` and
     `tests` (Phase 1 criteria), Tasks 4.1-4.4 clean the documents.
   - **Profiles.** The four `.fvprofile` files have `sensorID` `""` or
     `"GNSS"` and no `_FUSION` attribute (checked); `enabledPlots` entries such
     as `"Fusion/roll"` would not match (a checked plot only creates demand for
     visible sessions, which is fine).

   If a rule trips on committed Phase 1-3 code anyway (a comment worded
   differently from its document), reword the **comment** (Task 4.8). Change a
   rule only when the committed code has a legitimate second user, and report
   that.
4. **Traceability checker.**
   - The comment above it: "... and 401-442 (stored results: validity that
     mirrors memory, item = 400 + clause number)" becomes "..., 401-442
     (stored results: validity that mirrors memory, item = 400 + clause
     number) and 501-563 (demand-driven requested calculations, item = 500 +
     clause number)".
   - The range check:
     ```cmake
     if(NOT ((item GREATER_EQUAL 1 AND item LESS_EQUAL 19) OR (item GREATER_EQUAL 101 AND item LESS_EQUAL 120)
             OR (item GREATER_EQUAL 201 AND item LESS_EQUAL 247) OR (item GREATER_EQUAL 301 AND item LESS_EQUAL 350)
             OR (item GREATER_EQUAL 401 AND item LESS_EQUAL 442) OR (item GREATER_EQUAL 501 AND item LESS_EQUAL 563)))
       _violation("[traceability] item ${item} is outside 1-19, 101-120, 201-247, 301-350, 401-442 and 501-563: ${line}")
     endif()
     ```
   - After the `foreach(item RANGE 401 442)` block, the same block for
     `RANGE 501 563`.

**Acceptance Criteria:**
- [ ] `cmake -DREPO=. -P tests/audit/cleanup_audit.cmake` from the repository root prints "cleanup audit passed", with the rule count raised by exactly 18 over the count before this phase.
- [ ] Planted-hit proof. Plant each in turn, run the script, see the violation named, revert. None of this is committed.
      - (a) `// CalculationDemand` in `src/jobqueue.h` -> "only the application and its views know the demand layer".
      - (b) `#include "calculationdemand.h"` in `src/ui/docks/DemandIndicator.cpp` -> "nothing below the demand layer includes it".
      - (c) `// m_demand->flush();` in `src/ui/docks/logbook/LogbookHeaderView.cpp` -> "the views only read the demand layer".
      - (d) `// mousePressEvent` in `src/ui/docks/logbook/LogbookHeaderView.h` -> "the demand views handle no event of their own".
      - (e) `// isCellPending(` in `src/sessionmodel.cpp` -> "pending is the view's presentation of demand".
      - (f) `// JobQueue` in `src/sessionmodel.cpp` -> "the model, the logbook and the scheduler know nothing of the executor".
      - (g) `// Job` in `src/idlescheduler.h` -> "the idle scheduler learns nothing about jobs or demand".
      - (h) `// ColumnFillTask` in `src/sessionmodel.cpp` -> "the load step is the demand layer's scheduler task".
      - (i) `// loadPinnedSession(` in `src/mainwindow.cpp` -> "hidden loads go through loadPinnedSession".
      - (j) `// sessionRef(` in `src/calculationdemand.cpp` -> "the demand layer never loads a session or reads a record itself".
      - (k) a second line `// kMaxRunningJobs = 2` in `src/jobqueue.cpp` -> "one bound of simultaneous jobs".
      - (l) `kMaxHeldSessions = 2;` in place of the real initializer in `src/calculationdemand.h` -> "the load bound follows the executor's bound".
      - (m) `start()` in place of `start(QThread::LowPriority)` in `src/jobqueue.cpp` -> "the worker runs below normal priority".
      - (n) `cancelAll()` in `docs/CALCULATIONS.md` -> "the executor keeps no queue".
      - (o) `PlotRequests` in `docs/COMPUTED_PLOTS.md` -> "the plot request logic is gone".
      - (p) "press refresh" in `docs/SENSOR_FUSION.md` -> "no refresh or cancel control in code or documents".
      - (q) `// controlHit` in `tests/tst_plot_row_layout.cpp` -> "the plot rows' controls are gone".
      - (r) `"sensorID": "Fusion"` in place of one `"sensorID": "GNSS"` of `src/resources/profiles/Canopy_Piloting.fvprofile` -> "no default profile carries a column over a requested output".
      - (s) `#include <QStyleOption>` in `src/ui/docks/DemandIndicator.h` -> "the logic components see no widget" (widget-free-core).
      - (t) a map line `564 audit demand` -> "outside ... 501-563".
      - (u) every `549` line commented out -> "acceptance item 549 has no resolving test or audit line".
      - (v) a map line `501 tst_logbook_indicators noSuchFunction` -> "has no test function".
- [ ] `git diff` of the script shows only the header bullet, the widget-free-core pattern and pathspec, the new block and the checker.

**Complexity:** M

---

### Task 4.7: `tests/README.md`

**Purpose:** The test README must describe the suite as it is after Phases 1-3
(counts, catalogue rows, helpers, the widget tests), the traceability matrices
(9.2 and 9.4 amended, a new 9.6), the audit groups, the manual steps without
refresh or cancel plus the new ones, and the clause lists (appendices B and D
amended, a new appendix F).

**Files to modify:**
- `tests/README.md`

**Technical Approach:**

1. **Table of contents (3-23).** After the 12.3 line add
   `    - 12.4 [Demand-driven calculations](#124-demand-driven-calculations)`;
   after the appendix E line add
   `[Appendix F. The acceptance items of demand-driven requested calculations (501-563)](#appendix-f-the-acceptance-items-of-demand-driven-requested-calculations-501-563)`.
2. **§1 introduction (27-35).** "job queue, plot request logic" -> "the
   executor, the demand layer"; "`tst_plot_row_delegate` links Qt Widgets" ->
   "`tst_plot_row_delegate` and `tst_logbook_indicators` link Qt Widgets".
3. **§1 counts (42-47).** Replace 48 / 49 / 56 with the numbers
   `ctest --test-dir build-phase1/FlySightViewer-build -C Release -N` shows.
   Expected: 49 executables (Phase 1 replaced `tst_plot_requests` by
   `tst_calculation_demand`; Phase 3 added `tst_logbook_indicators`), 50
   entries, 57 with the seven exact runs. If they differ, write the real ones
   and say why in the report.
4. **§1 catalogue.**
   - Heading line 83: "**The executor, the demand layer and the demand views**
     (synthetic explicit calculations; no GTSAM)".
   - `tst_jobqueue` (87): "The executor, `JobQueue`, ..." In the list replace
     "deduplication, one job at a time in request order" with "at most the
     running job and one chosen next job (`holdsAtMostRunningAndChosenNext`),
     an equal offer creating nothing, a different offer replacing the chosen
     next job, which ends Cancelled "No longer needed" without an `idle()` in
     between (`offerReplacesChosenNext`), withdrawing it
     (`withdrawEndsChosenNext`), one job at a time in offer order"; after "the
     64 MiB worker thread" add "at below-normal priority
     (`workerRunsBelowNormalPriority`), the main thread free while a job
     computes (`mainThreadIsNotBlockedByARunningJob`)"; "a new request behind
     it created rather than deduplicated" -> "a new offer behind it becoming the
     chosen next job"; delete "pruning of unwanted queued jobs,"; append to the
     acceptance list "; demand items 521, 540, 544, 560, 561".
   - `tst_jobmodel` (88): after "never more than one running row" add ", a
     replaced chosen next job recorded as Cancelled "No longer needed"".
   - Replace the `tst_plot_requests` row (89) by
     `| \`tst_calculation_demand\` | ... |` with this content: `CalculationDemand`,
     the widget-free demand layer, on a real `PlotModel`, executor,
     `SessionModel`, `LogbookColumnStore`, logbook, real session engines and
     the global registry, with the synthetic plots of `plotfixture.h` (no
     widgets, no GTSAM). **Plot demand:** checking a plot starts the visible
     sessions without a result, showing a session or loading a visible one
     starts it, hiding drops its waiting pair and unchecking all of them while
     the running job finishes and is stored (`rowScript`,
     `showingASessionStartsIt`, `hidingASessionDropsItsWaitingPair`,
     `uncheckingDropsWaitingPairsKeepsRunning`); a waiting pair whose result
     appears by other means dropped before it starts
     (`resultAppearingWhileWaitingDropsThePair`); a click, `setPlotEnabled`,
     `togglePlot`, `setData` and a profile create the same demand, the
     start-up restore with every session hidden none; chained calculations
     upstream first with no `idle()` between links; the focused session, then
     row order, the chosen next job replaced when demand changes, never more
     than the running and one chosen next job; the one-second input-settle wait
     (a burst runs one job, the indicator from the first change); failures (an
     input-determined one stored, badged and never re-run; a job-level one
     badged, not re-run in the run and re-run by a new demand layer, as after a
     restart); not-applicable sessions never listed; per-plot state, counts,
     tooltip (at most ten per list), change signals, coalesced passes,
     `isMerelyUncomputed()`; session removal, registry changes, executor
     shutdown and null collaborators. **Column demand:** enabling a column over
     a requested output fills every session of the logbook, loading sessions
     that are not loaded at most two at a time as hidden, pinned sessions that
     leave by ordinary eviction (`enablingColumnFillsEveryUnloadedSession`), a
     session shown meanwhile running next, stored results creating no job,
     settlements (not applicable, failures, a session file that cannot be
     loaded, visible or hidden) that are not loaded again, chains keeping their
     hold, holds released on disable, show, removal, repopulation and
     destruction, the identity stub offered under its real id, per-column state
     and pending cells, the load step below saves, bulk edits and column work
     and not cancellable, and a pass over 2000 stubs reading each record set
     once (sensor-fusion-jobs acceptance 11, 13, 15, 16; demand items 501-560).
   - `tst_plot_row_layout` (90): "`layoutPlotRow()` (...), the pure geometry of
     a plot-list row's indicator cluster, without widgets or a font: indicator
     only, indicator and warning, warning only, nothing shown, an empty label,
     right-to-left as the exact mirror image; there is no hit rectangle".
   - `tst_plot_row_delegate` (91): rewrite: `PlotRowDelegate` in an offscreen
     `QTreeView` on a real `CalculationDemand`, `PlotModel`, executor and
     `SessionModel`; one of the **two tests that link Qt Widgets**
     (`FLYSIGHT_BUILD_WIDGET_TESTS`, label `widgets`). Plain rows
     pixel-identical to the base delegate; the working indicator and "k of n"
     painted and the name elided rather than the cluster; the indicator's clock
     (`workingAnimationClock`) turning only while a plot is working and never
     repainting when idle; the badge replacing the indicator once finished;
     hover detail from `DemandState`; a click on the check box, Space and the
     programmatic paths all writing the model and creating the same demand; a
     click, right click or double click on the cluster being a click on the
     row; repaint on `plotStateChanged`; survival of a destroyed demand layer
     (sensor-fusion-jobs acceptance 16; demand items 514, 517, 534-537, 557).
     Keep the sentence on the offscreen platform's fonts.
   - New row after it: `| \`tst_logbook_indicators\` | ... |`: `LogbookView`
     with `LogbookHeaderView` and `LogbookCellDelegate` in an offscreen window
     beside a reference `QTreeView`, on a real demand layer, executor and
     `SessionModel` (the second test that links Qt Widgets, label `widgets`):
     plain headers and cells identical to the base classes; a requested
     column's header showing the indicator right of its text, clear of the sort
     arrow, following moved, hidden and reordered sections, animating only
     while working; the badge once finished (also for a session file that
     cannot be loaded); hover detail from `columnState`; a click on the glyph
     sorting as elsewhere; pending cells distinct from unavailable ones and
     from the unreadable-record state, never in the model or `index.json`,
     sorted as unavailable, and replaced by the value when the record is
     written; one column's repaint per state change; survival of a destroyed
     demand layer (demand items 527, 533-538, 557, 558).
   - `tst_session_model_engine` (78): after "counted session pins that defer LRU
     eviction (and nothing else)" add ", the idle scheduler's
     `unregisterTask` and replacing `registerTask`
     (`schedulerTaskCanBeUnregistered`)"; "the job queue's hooks" -> "the
     executor's hooks".
   - `tst_result_columns` (80): append "; the column worker's behaviour and
     statistics unchanged with column demand active, and a stale record it
     deletes moving the pair into demand (`columnWorkerIsUnchangedByDemand`,
     `staleRecordDeletedByWorkerCreatesDemand`)".
   - `tst_column_cache` (110): append ", and `loadPinnedSession()`: a hidden
     session loaded the way showing it would, pinned under its corrected id,
     nothing pinned for a file that cannot be loaded
     (`loadPinnedSessionLoadsWithoutShowing`,
     `loadPinnedSessionFollowsIdentityRemap`,
     `loadPinnedSessionFailedLoadPinsNothing`)".
   - `tst_fusion_jobs` (137): "The real fit through `JobQueue`" -> "The real fit
     through the executor"; "the queue gives the bits" -> "the executor gives
     the bits"; "the queue's 64 MiB worker" -> "the executor's 64 MiB worker";
     "which the queue delivers" -> "which the executor delivers".
   - `tst_fusion_rows` (140): rewrite the first part: "`PlotModel` +
     `CalculationDemand` + the executor + `SessionModel` + the fusion
     registration ..."; the row script clause becomes "the row script of
     acceptance 15 on three real tracks (checking fits them one after another
     with no other action, the count rising as each publishes; unchecking
     mid-way drops the waiting one and lets the running one finish; checking
     again resumes; a fourth track shown is fitted with no other action)"; "and
     becomes refreshable when its input changes (9)" -> "and is fitted again
     after its input changes and settles (9)"; "sessions are edited, tracks
     hidden and shown ... without disturbing it" add "and are fitted
     afterwards with no other action".
   - `tst_fusion_store` (141): "not requested (refresh offered, nothing run)
     after the `cache/` folder was deleted while closed" -> "not requested after
     the `cache/` folder was deleted while closed (one fit offered while roll is
     checked, dropped when unchecked, nothing run)"; append "; a logbook column
     over roll filled for sessions that are not loaded, and nothing fitted
     again after a restart (`columnOverFusionFillsUnloadedSessions`,
     `fusionColumnWithStoredFitsRunsNothing`)".
   - `audit_cleanup` (168): "gestures from the row delegate only, a widget-free
     core" -> "work started only by the demand layer through the executor,
     views that only read the demand layer, a model and scheduler that know no
     jobs, a widget-free core"; before "and every line of" add "and no refresh,
     cancel, queue or plot-request logic remains in code or documents".
5. **§3 (264, 275).** The `FLYSIGHT_BUILD_WIDGET_TESTS` row: "also build
   `tst_plot_row_delegate` and `tst_logbook_indicators`, the two tests that
   link Qt Widgets (they run offscreen views)". Line 275: "The Widgets tests
   also have `widgets`".
6. **§8.** Lines 527-528: "(`tst_plot_row_delegate` and
   `tst_logbook_indicators` are the only ones)". Lines 569-588: "`jobfixture.h`
   (`JobWorld`, `Gate`, `waitIdle`, `waitStarted`, `Quiet`, `onFirstProgress`)
   registers controllable explicit calculations on the global registry for
   tests of the executor and whatever sits on top of it: ..."; "destroy it
   after the `JobQueue`" -> "destroy it after the executor"; `plotfixture.h`:
   "adds synthetic plots (`Syn/...`, eight, the last over the `exhausted`
   calculation for an out-of-memory job) over those calculations ..., `spin()`
   (two turns of the event loop and a flush of the demand layer),
   `waitDemandIdle()` (the executor idle, no pass pending, no session settling
   and no load step with work; follow it with `waitForIdle(model)` when column
   values must be filled) and `sessionIdsOf()` for a state's track lists". In
   the `logbookprobe.h` list add `attributeColumn(key)` (a session-attribute
   column over `key`) after `exitTimeColumn`.
7. **§9 introduction (660-661).** "Five specifications, five ranges ... the
   five tables below" -> six.
8. **§9.2.** Introduction: add after the first paragraph "Items 110, 111, 113,
   114, 115, 116 and 117 are stated as amended by the specification
   "Demand-driven requested calculations" (9.6)." Rows (each Clause gets
   "(as amended)" where the item is amended; the Evidence column mirrors the
   map):
   - row 8 (761): evidence "... `tst_jobqueue::staleRunningJobIsStoppedAtOnce`,
     `offerWhileStaleJobWindsDown`, `tst_calculation_demand::staleRunningJobIsWaitingAtOnce`".
   - row 10 (764): clause "(as amended) cancel through the executor stops at
     the next boundary, publishes nothing, requestable; the chosen next job
     starts".
   - row 11 (765): clause "(as amended) no-IMU session: never waiting /
     running / failed, nor counted; no job possible"; `tst_plot_requests::` ->
     `tst_calculation_demand::sessionWithoutInputIsNeverListed`.
   - row 13 (767): clause "(as amended) B consumes A: checking the plot runs A
     then B"; `tst_calculation_demand::chainedBlockersContinue`.
   - row 14 (768): clause "(as amended) one job at a time; at most the running
     and one chosen next job; no duplicates"; evidence
     `tst_jobqueue::oneAtATimeInOfferOrder`, `duplicateOffersCreateNoDuplicates`,
     `holdsAtMostRunningAndChosenNext`.
   - row 15 (769): clause "(as amended) row script, without widgets,
     synthetic: checking computes the visible tracks, unchecking drops the
     waiting ones, a track shown is computed"; evidence
     `tst_calculation_demand::rowScript`, `showingASessionStartsIt`,
     `uncheckingDropsWaitingPairsKeepsRunning`.
   - row 16 (772): clause "(as amended) start-up restore with hidden tracks
     starts nothing; a profile or a programmatic check creates demand like a
     click (logic)"; evidence
     `tst_calculation_demand::startupRestoreWithHiddenSessionsStartsNothing`,
     `profileStyleApplyCreatesDemand`, `programmaticCheckCreatesDemand`.
   - row 16 (773): clause "... with the view attached; a click on the check box
     writes the model like any other check"; evidence
     `tst_plot_row_delegate::programmaticCheckIsTheSameAsAClick`,
     `startupRestoreWithHiddenSessionsStartsNothingWithViewAttached`,
     `checkBoxClickChecksThroughTheModel`.
   - row 16 (774): `tst_calculation_demand::merelyUncomputedIsNotWorthAWarning`.
   - row 17 (776): clause "(as amended) remove / unload with a running or
     chosen next job; shutdown".
9. **§9.4.** Introduction: add "Clauses 4, 6, 21 and 37 are stated as amended
   by the specification "Demand-driven requested calculations" (9.6)." Rows:
   304 (Section "2, as amended", Clause = the new `#` text, Evidence gains
   `tst_result_columns::staleRecordDeletedByWorkerCreatesDemand`); 306 (Section
   "2, as amended", Clause new, Evidence `tst_fusion_golden::successFixturesMatchGolden`;
   `tst_calculation_demand::rowScript`, `storedResultsCreateNoJob`;
   `tst_fusion_rows::realRowScript`; `audit gestures`); 321 (Section "5, as
   amended", Clause new, Evidence gains
   `tst_calculation_demand::storedResultsCreateNoJob`); 337 (Section "8, as
   amended", Clause new).
10. **New §9.6** after 9.5: `### 9.6 Demand-driven requested calculations (items 501-563)`.
    Introduction: the sixty-three clauses of the specification "Demand-driven
    requested calculations", stated in full in
    [appendix F](#appendix-f-the-acceptance-items-of-demand-driven-requested-calculations-501-563);
    item = 500 + clause number; the same four line forms as 9.2, and every item
    has at least one test or audit line; "Section" is the section of the
    specification; its sections 1 (motivation) and 4 (terms) have no item;
    clauses 49-62 are its section 13 tests, one per bullet, and clause 63 its
    section 14; the specification amends those of 9.2 and 9.4. Then a table
    `| # | Section | Clause | Evidence |` with one row per item 501-563:
    Section from the parenthesis of the item's `#` line in the map, Clause the
    rest of that line, Evidence rendering the item's map lines in the style of
    9.4 (`` `target::function` ``, further functions of the same target
    comma-separated, targets separated by `;`, then `` `audit <group>` `` and
    `` `manual M<k>` ``). The table and the map list the same evidence.
11. **§10.**
    - `one-worker` bullet: "or an atomic other than the queue's cancel flag
      does" -> "the executor's cancel flag".
    - `branch-mechanisms` bullet: "(or the idle scheduler in the job code)" ->
      "(or the idle scheduler in the executor's code)".
    - `solver-confinement` bullet: "session, engine, queue, preference or GUI
      header" -> "session, engine, executor, preference or GUI header";
      "session-model, queue or plot-request code" -> "session-model, executor
      or demand-layer code".
    - Replace the `gestures` bullet (1029-1041) with: **group `gestures`**
      (items 116, 306, 501, 519, 527, 534, 543, 544, 562): a gesture entry point
      (`plotCheckedByUser`, `refreshPressed`, `cancelPressed`) is named in
      `src` or `tests`; `prepare(` / `publish(` is called outside the executor
      and the engine; `request(` outside the engine; anything in `src` outside
      `src/engine` calls the engine's synchronous `request()` (spelled through
      `calculationEngine()`, `engine.` or `engine->`); `offer(` is called on
      other than exactly one line of `src`, or anywhere but
      `calculationdemand.cpp`, or `withdrawChosenNext(` anywhere but there (the
      demand layer is the only caller of the executor); product code cancels a
      job; the UI refers to the executor beyond `AppContext.h`; or
      `EvaluationPolicy::Explicit` is tested outside the engine (the rest of
      the old sentence on `explicitDependencies()` stays).
    - `widget-free-core` bullet: "the executor, the job model, the demand
      layer, the plot model, `PlotRowLayout.h` or the shared glyphs
      (`DemandIndicator.*`) include a widget header (a style option and a header
      view included)".
    - `fusion-tooling` bullet: "or the job queue" -> "or the executor".
    - `stored-results` bullet: "the store names the job queue, the plot
      requests or a request / prepare / publish call" -> "the store names the
      executor, the demand layer or a request / offer / prepare / publish
      call".
    - New bullet after `result-validity`: **group `demand`** (items 506, 508,
      513, 515, 518, 527, 529, 530, 533, 534, 538, 540-544, 546, 547, 563):
      `CalculationDemand` is named outside the demand layer, `MainWindow`,
      `AppContext.h`, the plot list's delegate and dock feature, the logbook
      view, header view, cell delegate and dock feature, and the plot widget;
      the executor, the job model, the session model, the scheduler, the
      logbook, the column store, the plot model, the profile bridge, the result
      store, the engine, the shared glyphs or the row layout includes
      `calculationdemand.h`; anything in `src/ui` or `MainWindow` runs a pass,
      the load step or a settle seam of the demand layer; the plot-row
      delegate, the header view, the cell delegate or the shared glyphs handle
      a mouse or key event of their own; the pending queries appear outside the
      demand layer and the cell delegate; the session model, the logbook, the
      column store, the scheduler or the plot model names the executor; the
      idle scheduler mentions demand, calculations, jobs or an executor;
      `ColumnFillTask` appears outside the demand layer, `sessionmodel.h` and
      the logbook view, or `loadPinnedSession(` outside the demand layer and
      the session model; the demand layer loads a session or reads a record
      itself; `kMaxRunningJobs` is assigned other than once, the load bound is
      not `JobQueue::kMaxRunningJobs + 1`, or the worker is not started with
      `QThread::LowPriority`; the removed queue API appears in `src`, `docs` or
      `README.md`; the plot request logic appears anywhere; a refresh or cancel
      control is named in `src`, `docs` or `README.md`, or its code in `src` or
      `tests`; or a profile in `src/resources/profiles` carries a column over a
      Sensor fusion value. This file is excluded from the text rules because
      this section spells the patterns;
    - The traceability bullet: "... 401-442" -> "... 401-442 and 501-563" in
      both places.
12. **§12.** Introduction: "Three scripts." -> "Four scripts.". Replace these
    steps (one paragraph each, opening with the bold id; ids and their place
    unchanged):
    - **M1 Startup (116, 321, 514).** Check "Sensor fusion > Roll" with three
      fusable tracks visible that have no stored fit: the three fits start one
      after another with no other action. As soon as the first track has
      published, uncheck Roll: the row turns plain at once, the running fit
      (the second track's) continues to its end, and no third fit starts. Quit
      and restart. After the restart the row is still checked. Track
      visibility is not kept across restarts, so no track is visible, the row
      is plain and no job starts (no progress, CPU idle). Show the three
      tracks: the first two tracks' roll is drawn at once (their stored
      results), and the third track's fit starts by itself: the row shows the
      turning arc and "2 of 3", then turns plain. The debug output contains no
      "No data available" line for the fusion plot.
    - **M2 Profile (116, 514, 515, 556).** Save a profile that checks Roll.
      Uncheck Roll, and show two fusable tracks without stored fits: nothing
      starts. Apply the profile: Roll is checked and the two fits start at
      once, one after the other, exactly as after a click on the check box
      (this is the real `applyProfile()` path, which no automated test can
      construct). Uncheck Roll and hide every track, apply the profile again:
      nothing starts (no track is visible); show one fusable track without a
      stored fit: its fit starts at once. In Manage Profiles restore the default
      profiles and apply each of them in turn: none adds a logbook column over
      a "Sensor fusion" value, and "Computing columns" never appears in the
      logbook's progress line while every track is hidden. (The Plots menu and its shortcuts list a
      fixed set of GNSS plots; no fusion plot can be toggled from there.)
    - **M3 Working indicator (115, 501, 535).** With three fusable tracks
      without stored fits visible, check Roll: the row shows the turning arc
      and "0 of 3" at the right of its name; Pitch and Yaw, if checked, show
      the same. The tooltip reads "Computing: 0 of 3 done" and "<name> - Sensor
      fusion: <progress text>". As each fit publishes, its graph appears
      without any further action, the label advances ("1 of 3", "2 of 3"),
      and finally the arc stops and the row is plain. The legend and any
      fusion logbook column fill in at the same moments.
    - **M4 No controls (115, 534).** On a fresh set of tracks, uncheck and
      re-check a fusion plot by clicking its check box: fits start. Do the same
      with Space. Click, right-click and double-click the arc and the "k of n"
      of a working row: the row is selected as by a click anywhere else in it;
      nothing is cancelled and nothing is toggled. No context menu, menu item
      or shortcut offers a refresh or a cancel for a calculation.
    - **M6 Hide and uncheck (115, 518, 519, 534).** With one fit running and two
      tracks waiting ("0 of 3"), hide one waiting track: the row shows "0 of 2"
      at once. Uncheck the plot: the row turns plain at once; the running fit
      continues (CPU busy) to its end, its record file appears in `cache/`, no
      further fit starts, and the CPU then goes idle. Check the plot again: the
      remaining visible track is fitted.
    - **M7 Fourth track and failure (115, 525, 537).** Show a fourth fusable
      track: it is fitted with no other action, after the running fit if one
      runs. Show the recording without IMU data: it never appears in any count
      or tooltip. For a rejected recording, once the other fits are done the
      row shows the warning badge with 1 instead of the arc, the tooltip gives
      the reason, nothing offers to try again, and no message box appears.
    - **M8 Remove and unload (117).** With one fit running and another track
      waiting, delete the waiting track's session, then the running one's: no
      crash, no hang, nothing published for them; the remaining rows' counts
      fall.
    - **M9 Quit (117).** "With one fit running and two queued" -> "With one fit
      running and two tracks waiting"; the rest unchanged.
    - The closing paragraph (1679-1680) becomes: "There is no refresh and no
      cancel for a calculation anywhere: unchecking a plot, hiding a track or
      removing a logbook column is how work is dropped."
    - **M16** (1741): "press refresh and let the three fits finish" -> "let the
      three fits finish (they start when Roll is checked)"; "the row is plain
      (no refresh icon, no count)" -> "the row is plain (no arc, no count)".
    - **M17** (1743): after the first sentence insert: "Every other recording
      of the logbook with IMU data and no stored fit is now fitted in the
      background too (M24); on a large logbook copy wait until the column's
      header shows no arc, or use a logbook copy that holds only the script's
      recordings." "With the three tracks of M16 fitted" -> "With the three
      tracks of M16 fitted and the column filled"; "and nothing loads (no
      progress in the status bar)" -> "nothing loads and no fit starts (no
      "Computing columns" in the progress line)".
    - **M18 Change an input (315, 520)** (1745): from "The track's roll
      disappears" on: "The track's roll disappears, the row shows the arc, the
      record file is gone and the track's cell in the column of M17 shows "…";
      about a second after the re-import a fit starts by itself, a record file
      appears again, roll is drawn and the cell shows the new number."
    - **M19** (1747): from "Quit, delete the whole `cache/` folder" on: "Quit,
      delete the whole `cache/` folder, and start again: every track is still in
      the logbook, the cells of the column of M17 show "…" and fill in again as
      the recordings are fitted in the background, and `cache/` reappears with
      the first finished fit. Remove the column, quit, delete `cache/` again and
      start: showing a track draws no roll until Roll is checked, which starts
      its fit, and no `cache/` folder appears until a fit finishes."
    - **M22** (1753): insert after "Windows only.": "Uncheck Roll and remove the
      column of M17 first: while either is on, a record that cannot be read
      reads as not computed and would be fitted again." Then "Start the
      application and show that track: its roll is not drawn, the row shows the
      refresh control with 1, the debug output ..." -> "Start the application
      and show that track: nothing starts, the debug output ..."; "and that
      track's cell in the column of M17 is empty. Do not press refresh." ->
      "Do not check Roll while the file is held."; "Quit, run `$f.Close()`,
      start again: the cell stays empty until the track is shown; show it: roll
      is drawn at once, the row is plain, no job starts, and the cell shows its
      number." -> "Quit, run `$f.Close()`, start again, check Roll and show the
      track: roll is drawn at once, the row is plain and no job starts."
13. **New §12.4** after 12.3's closing paragraph and before appendix A:
    `### 12.4 Demand-driven calculations`. Introduction: what the automated
    tests cannot show: the animated indicator, its hover and the pending cells
    in the real views and themes, a whole-logbook fill with real fits and its
    memory, the application staying usable while a fill runs, and a job-level
    failure tried again after a restart. Use the preamble of 12.1 (a COPY of a
    logbook) with a logbook of 20-50 recordings with IMU data, several of them
    never fitted, one without IMU data and one the model rejects; Task Manager
    (Details tab) and, for M27, Sysinternals Process Explorer. Then:
    - **M23 Working indicator and hover (535, 536).** Show two fusable tracks
      without stored fits and check Roll. The arc at the right of the row's
      name turns about once a second, "0 of 2" beside it. Hover the arc, then
      the row's name: both show "Computing: 0 of 2 done" and "<name> - Sensor
      fusion: <progress text>" (hover again to see the progress text advance).
      When both fits end the arc stops and the row is plain; with nothing
      computing, Task Manager shows FlySight Viewer at 0 % CPU (no repaint while
      idle). Repeat in the light and the dark theme and on a selected row: the
      arc and the text are readable in each.
    - **M24 A logbook column fills in the background (510, 529, 530, 539).**
      Hide every track. In the column editor add "roll at the exit marker" over
      Sensor fusion. At once its header shows the turning arc right of its
      name; hovering the header shows "Computing: 0 of <n> done" (n: the
      recordings with IMU data and a local origin) and the running recording
      with its progress; the progress line shows "Computing columns: k / n" for
      the whole fill, with no cancel button, k rising as fits finish. Values replace
      "…" row by row, from the top. Task Manager's memory for FlySight Viewer
      stays flat after the first loads (at most two recordings beyond
      "Maximum cached sessions" are held for the fill). The recording without
      IMU data stays blank and is not counted. When the fill ends, the arc stops
      (or the badge shows: M7's rejected recording, hover lists it), and
      `index.json` in the logbook folder holds the values and no "…", and the
      progress line has gone. While a fit runs, Task Manager shows no main-thread
      activity beyond the fit (the scheduler waits, it does not spin). Quit and
      start again: the column shows its values at once, and nothing is loaded or
      fitted for it.
    - **M25 Pending cells (538).** During a fill (M24's, or, once it has
      finished, after deleting a few records in `cache/` with the application
      closed and starting again with the column enabled): cells still to come
      show a grey "…" (a lighter one on a selected row); blank cells are
      recordings without IMU data; hovering a "…" cell shows "Pending: this
      value is being computed". Sort by the column ascending, then descending:
      numbers first, "…" and blank cells together at the bottom both ways.
      Drag the column elsewhere and narrow it: the arc stays at the right of its
      (elided) name and clear of the sort arrow; double-click the section's
      right edge: it widens to fit the name and the arc.
    - **M26 Visible first, then disabling (518, 524).** While a fill runs, check
      Roll and show a recording far down the list whose fit is missing: it is
      fitted next, right after the running fit, and its roll is drawn. Then
      remove the column in the column editor: the header and its arc are gone,
      the running fit finishes and its record appears in `cache/`, nothing else
      starts, and the progress line goes idle.
    - **M27 Background work does not get in the way (504, 540).** During a fill:
      pan and zoom plots, show and hide tracks, edit a description, sort and
      scroll the logbook: everything responds as without the fill. On Windows,
      in Process Explorer (FlySight Viewer > Properties > Threads) the thread
      that uses the CPU runs at "Below Normal" priority, the application's
      other threads at "Normal". (On Linux the worker's priority is not
      lowered: docs/CALCULATIONS.md, section 15.5; only the responsiveness is
      checked there.)
    - **M28 A failure is tried again after a restart (526).** Windows only.
      Quit. Pick a recording with IMU data and no stored fit (delete its
      `.fvresult` in `cache/` if it has one) and hold its session file open
      without sharing: `$f = [IO.File]::Open('<scratch>\FlySight Viewer\logbook\sessions\<uuid>.csv', 'Open', 'Read', 'None')`.
      Start the application with the column of M24 enabled. When the fill
      reaches that recording its load fails; once the fill is done the
      column's header shows the warning badge, and its tooltip lists
      "<description> - The session file could not be loaded", where
      <description> is the recording's description as the logbook shows it
      (the recording is named, not identified by its id). Wait a minute: that
      recording is not tried again (the progress line has gone, no fit). Quit,
      run `$f.Close()`, start again: the recording is
      loaded and fitted in the background, its cell gets its number and the
      badge is gone.

    Close with the usual "Pass / fail and a note per step go in the phase
    report. A step that fails is reported as it failed, not adjusted."
14. **Appendix B.** Introduction: after "verbatim." add "Clauses 10, 11, 13,
    14, 15, 16 and 17 are stated as amended by the specification
    "Demand-driven requested calculations" (appendix F)." Replace those
    clauses:
    - "10. (as amended) Cancelling a running job through the executor stops it
      at the next solver boundary, publishes nothing, and leaves the
      calculation requestable. The chosen next job then starts."
    - "11. (as amended) A session with no IMU data never appears as waiting,
      running or failed for any fusion plot, is never counted, and no job can
      be created for it."
    - "13. (as amended) With two explicit test calculations where B consumes A,
      checking a plot of B's output runs A then B with no other action."
    - "14. (as amended) Never more than one job runs at a time. The executor
      holds at most the running job and one chosen next job, and an offer equal
      to either creates no duplicate job."
    - "15. (as amended) Row behaviour, tested without widgets: checking a
      fusion plot with three visible fusable tracks computes them one after
      another, the count of done tracks rising as each publishes; unchecking
      mid-way drops the tracks that are waiting and lets the running one
      finish; checking again resumes; a fourth track shown afterwards is
      computed with no other action."
    - "16. (as amended) Starting the application with fusion plots checked
      starts no job, because every track starts hidden. Applying a profile that
      checks fusion plots, the Plots menu and any other programmatic check
      create demand exactly as a click on the check box does."
    - "17. (as amended) Removing or unloading a session with a running job or
      the chosen next job, and quitting with a job running and one chosen next,
      neither crash nor hang, and publish nothing stale."
15. **Appendix D.** Introduction: add "Clauses 4, 6, 21 and 37 are stated as
    amended by the specification "Demand-driven requested calculations"
    (appendix F)." Replace:
    - "4. (2, as amended) A stored result is a memory, not a request: when it
      is stale or missing, the calculation reads as not requested until
      something switched on - a checked plot over a visible session, or an
      enabled logbook column over it - has it computed."
    - "6. (2, as amended) The kernel is unchanged, and plot rows and logbook
      columns count a restored result as computed: no working indicator and no
      job."
    - "21. (5, as amended) Plot rows count a session with a restored result as
      computed: no working indicator, no job; a stale or absent result is in
      demand like any missing one."
    - "37. (8, as amended) Test: a fusion fixture fitted, saved, unloaded and
      reloaded yields the seventeen channels and the diagnostics bit-identical
      to the goldens, with no job created; the row is plain."
16. **Appendix F** after appendix E: `## Appendix F. The acceptance items of demand-driven requested calculations (501-563)`.
    Introduction: the clauses of the specification "Demand-driven requested
    calculations", which amends "Sensor fusion as an explicit calculation,
    with plot-driven background jobs" (appendix B) and "Storing requested
    calculation results with the session" (appendix D), one sentence each,
    with the specification's section number in front; items 501-563 of
    `tests/acceptance_map.txt` (item = 500 + the number below) and the rows of
    section 9.6; its sections 1 (motivation) and 4 (terms) have no item. Then
    63 numbered clauses, 1-63: clause k is the text of the map's `#` line for
    item 500 + k from its parenthesised section on, written as one sentence
    (capital letter after the parenthesis, full stop at the end), for example
    "1. (2) The user expresses intent through plots and logbook columns, never
    through calculations: what is switched on is the request."
17. Do not add a mention of `sensor-fusion-clean-port` (the audit pins its
    count in this file at 4) and do not link `PLANS/`.

**Acceptance Criteria:**
- [ ] The §1 counts equal `ctest -N`'s; `git grep -c -E "^\| \`tst_(calculation_demand|logbook_indicators)\` \|" -- tests/README.md` prints 2; `git grep -n "tst_plot_requests\|PlotRequests" -- tests/README.md` prints only lines of §10's `demand` bullet, if any.
- [ ] The catalogue rows named in step 4 contain `holdsAtMostRunningAndChosenNext`, `workerRunsBelowNormalPriority`, `columnWorkerIsUnchangedByDemand`, `loadPinnedSessionLoadsWithoutShowing`, `schedulerTaskCanBeUnregistered`, `columnOverFusionFillsUnloadedSessions`.
- [ ] 9.6 has 63 rows, 501-563, matching the map line for line; 9.2 rows 8, 10, 11, 13-17 and 9.4 rows 304, 306, 321, 337 are updated.
- [ ] Appendix F has 63 numbered clauses; appendix B's clauses 10, 11, 13-17 and appendix D's 4, 6, 21, 37 read "as amended".
- [ ] `**M23 ` to `**M28 ` exist in 12.4; `git grep -n -i -E "refresh (icon|control)|press(ed)? refresh|circled x|cancel control" -- tests/README.md` prints only lines of §10 (the rule descriptions), if any.
- [ ] §10 has the `demand` bullet, the rewritten `gestures` bullet and the 501-563 range.
- [ ] `audit_cleanup` passes.

**Complexity:** L

---

### Task 4.8: Cross-references and comment sweep

**Purpose:** Every reference to a moved document section, and every comment
that a new `demand` rule matches, is correct; nothing else in `src` changes.

**Files to modify** (comments only, and only as found below):
- whatever the checks report.

**Technical Approach:**

1. Doc cross-references: `git grep -n -E "section 16\.[0-9]+|CALCULATIONS\.md[^)]{0,40}section 16|16\.[1-9][0-9]?\)" -- src tests docs README.md python_plugins/README.md`.
   Each hit must point at the subsection that now holds its subject (16.1
   demand and conditions, 16.3 requested plots and costs, 16.8 hidden loads,
   16.10 the views, 16.11 contracts). Known candidate:
   `src/ui/docks/plotselection/PlotRowDelegate.h` (it cited "section 16.9",
   the old view subsection; the views are now 16.10) - fix the number in the
   comment if Phase 1 kept the reference.
2. Test comments with stale wording:
   `git grep -n -i -E "refresh (icon|control)|press(ed|es)? refresh|refreshPressed|cancelPressed|plotCheckedByUser|in request order|oldest queued|PlotRequests|the queue (prunes|queues)" -- tests ":!tests/README.md" ":!tests/audit"`
   (Phase 1 rewrote the tests that use gestures; this finds comments it
   missed, for example `tst_fusion_jobs.cpp:627` "no second gesture" is
   harmless and stays). Rewrite only what the `demand` or `gestures` rules
   match, and what says the opposite of the demand rules.
3. `src` comments: run `cmake -DREPO=. -P tests/audit/cleanup_audit.cmake`
   after Task 4.6. For each `demand` violation in a comment, reword the comment
   (the rule's `Allow:` text says how). Do not change code. Stale wording in
   `src` comments that no rule matches is **reported**, not edited.
4. List every changed file and line in the phase report.

**Acceptance Criteria:**
- [ ] The three checks print only correct references or nothing.
- [ ] `git diff --stat -- src tests/*.cpp tests/support tests/fusion` shows comment-only edits, each listed in the phase report (none is also acceptable).

**Complexity:** S

---

### Task 4.9: Verification

**Purpose:** Prove that the documents, map and audit agree with the code and
that nothing regressed.

**Technical Approach:**
1. `cmake --build build-phase1 --config Release` (only needed if Task 4.8 edited
   a source or test comment). **Never build `build/`.**
2. `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure -L audit`.
3. Coverage check of the map: every function name listed under Dependencies >
   Assumptions for Phases 1-3 (the new and renamed functions of
   `tst_calculation_demand`, `tst_jobqueue`, `tst_plot_row_delegate`,
   `tst_plot_row_layout`, `tst_logbook_indicators`, `tst_result_columns`,
   `tst_column_cache`, `tst_session_model_engine`, `tst_fusion_store`) occurs
   in `tests/acceptance_map.txt`
   (`grep -c " <name>$" tests/acceptance_map.txt` >= 1 for each). Expected: all
   of them (the block was built to cite every one).
4. The planted-hit proofs of Task 4.6 (a)-(v), each with
   `cmake -DREPO=. -P tests/audit/cleanup_audit.cmake` (no build needed).
   Revert every plant; `git status --short` afterwards shows only this phase's
   files and the pre-existing untracked paths.
5. `ctest --test-dir build-phase1/FlySightViewer-build -C Release -N` for the
   counts of Task 4.7 step 3.
6. Full suite: `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure`.
7. Render check: open the changed documents and `tests/README.md` in a
   Markdown preview. Tables keep the column counts of their header rows (9.6:
   four columns), the table-of-contents anchors resolve (12.4, appendix F,
   COMPUTED_PLOTS' nine sections), and the root README tree stays aligned.

**Acceptance Criteria:**
- [ ] Build (if needed) succeeds; `-L audit` passes; the full suite passes.
- [ ] Each planted hit (a)-(v) was reported by the script, and none remains.
- [ ] The phase report lists every changed file, every comment rewritten in Task 4.8, any stale `src` comment only reported, and any map line pointed at a committed name that differs from its phase document.

**Complexity:** S

## Testing Requirements

### Unit Tests
- None new: this phase adds evidence, not tests.
- The audit is the phase's test: the `demand` group, the extended
  `widget-free-core` rule, the extended checker and every map line must pass,
  and every new rule is proven by a planted hit (Task 4.6).

### Integration Tests
- The full suite passes unchanged in `build-phase1` (labels core, fusion,
  exact, python, audit, widgets). No test file changes apart from comments the
  Task 4.8 sweep finds.

### Manual Verification
- Michael runs M23-M28 of `tests/README.md` §12.4 and the rewritten M1-M4,
  M6-M9, M16-M19 and M22 on a development build from `build-phase1`, on a copy
  of a logbook (the 12.1 preamble). Pass / fail per step goes in the phase
  report.
- Read-through against the behaviour seen: `docs/CALCULATIONS.md` §15-17,
  `docs/COMPUTED_PLOTS.md`, `docs/SENSOR_FUSION.md` §1, §2, §7, §8,
  `docs/DATA_SCHEMA.md` §11-12, `python_plugins/README.md` §7-8, the root
  `README.md`.

## Notes for Implementer

### Gotchas
- **The checker finds a test function by the text `::<function>()`** in
  `tests/<target>.cpp`. A function defined inside its class body has no such
  text and its map line fails. `tst_logbook_indicators.cpp` is new: if Phase 3
  defined a function in the class body, move the definition out of line
  (`void LogbookIndicatorsTest::name()`; a test-file edit, listed in the
  report); never drop the line.
- **Data-driven tests** (`enablingColumnFillsEveryUnloadedSession`,
  `chainCompletesAfterFirstJobDoesNotSucceed`,
  `chainStopsForHiddenTrackOrUncheckedPlot`,
  `jobLevelFailureIsBadgedNotRerunUntilRestart`,
  `columnWorkerIsUnchangedByDemand`,
  `pendingCellBecomesValueWhenRecordIsWritten`,
  `unreadableRecordPendingIsNotDemandPending`, ...) are cited by their base
  name, never with `_data`.
- **Two targets have a `survivesDemandDestroyedFirst`** (`tst_plot_row_delegate`,
  `tst_logbook_indicators`); the map names the target, so both lines are
  distinct.
- **Audit patterns are case-sensitive and line-based, and they match
  comments.** In `docs/` and `README.md` never write the words of Task 4.2's
  list, not even to describe what was removed. In `src`, a comment that names
  `CalculationDemand` outside the allowed files, `JobQueue` in the model or the
  scheduler, or `sessionRef(` in the demand layer trips a rule: reword it.
- **`tests/README.md` is excluded** from the text rules that its section 10
  spells (`gestures`' first rule, `demand`'s `PlotRequests` and control
  rules); `python_plugins/README.md` is excluded from every search, so its two
  sentences are checked by Task 4.4's criteria only.
- **SENSOR_FUSION.md is under `fusion-model`'s text rule** (no `frozen`,
  `twenty-one`, `stationary window`, `candidate window`, `coarse
  initializer`, `bias shifts below`, `zero bias shift`, branch name).
- **The `sensor-fusion-clean-port` count in tests/README.md is pinned at 4.**
  Nothing added in this phase names it.
- **Item numbers are fixed by clause order**; 9.6, appendix F and the map use
  the same numbering (501 = clause 1). Do not renumber when adding evidence.
- **Do not link `PLANS/`.** Cite the specification by title.
- **The rule count** printed by the audit rises by exactly 18 (the `demand`
  group); `widget-free-core` changes its pattern and pathspec, not the count.
- **M28 assumes** that a session file held open without sharing makes the load
  fail on Windows (the load reads the file). If it does not, report it: the
  automated `unloadableSessionIsSettledAsFailed`,
  `visibleFailedLoadIsSettledAsFailed` and
  `failedLoadSessionShowsBadgeNotPending` still carry item 526, and the
  restart half is `columnJobLevelFailureIsNotReloadedUntilRestart` /
  `jobLevelFailureIsBadgedNotRerunUntilRestart`.
- **"k of n" counts failures as done**: n is the tracks (or sessions) that can
  be computed, and a failed one is done. Say so where the documents explain the
  label.
- **Line numbers in this document** are from `b55869d`; the documents and
  `tests/README.md` were not edited by Phases 1-3, the map was edited in place
  (same line count), and the audit script's line numbers moved.

### Decisions Made
- **A new range, 501-563 (item = 500 + clause number)**, as the two previous
  specifications did (401-442, 201-247): 6 principles (§2), 2 scope clauses
  (§3), 8 demand clauses (§5), 5 on work following demand (§6), 3 on priority
  (§7), 3 on failures (§8), 6 on sessions not loaded (§9), 6 on what the user
  sees (§10), 2 on degradation (§11), 7 on the architecture (§12), 14 tests
  (§13, one per bullet: 549-562) and 1 on documentation (§14: 563). §1
  (motivation) and §4 (terms) have none.
- **Superseded items are amended in place, not retired.** The map and the
  README have no retirement convention; the precedent is "as amended" (items
  310, 316, 317, 334, 341). Items 110, 111, 113-117 (the queue, the gestures,
  the refresh count) and 304, 306, 321, 337 (gestures and "no refresh count")
  keep their numbers, gain "(as amended)", and six evidence lines are added
  where the amended clause needs a demand test. Items 318 and 411 ("nothing is
  recomputed on its own") and 349 ("nothing starts on its own") stay: a load
  still computes nothing; demand does, because something is switched on.
- **The offer rules stay in `gestures`** (Phase 1 put them there and the map
  cites the slug); the new `demand` group holds the other boundaries. Items
  that need both cite both.
- **`DemandIndicator.*` joins `widget-free-core`** (Phase 3 recommendation), and
  that rule's include pattern gains `QStyle*` and `QHeaderView`, which none of
  the listed files includes today.
- **The default-profile check is an audit text rule** over
  `src/resources/profiles/*.fvprofile` (the only shipped profiles:
  `profiles.json` lists Basic_Flight, Canopy_Piloting, Speed_Skydiving,
  Wingsuit_Performance). A test would need product-resource loading and a
  fusion build for one fact; the text rule names the one requested
  calculation's outputs (sensor `Fusion`, attributes `_FUSION_...`) and says
  what to add for a new one. Checked plots in a profile are allowed (plot
  demand is bounded by the visible sessions).
- **§16's numbering**: 16.1 and 16.3 keep their subjects so §17's references
  hold; the rest is new (16.2 state, 16.4-16.8 behaviour, 16.9 API, 16.10
  views, 16.11 contracts). `COMPUTED_PLOTS.md` is renumbered freely (nothing
  links its sections). §15's heading gains ": the executor" (nothing links it);
  15.1-15.8 keep their numbers (15.2, 15.3 and 15.8 are cited widely).
- **Manual steps**: M1-M4, M6-M9, M16-M19 and M22 are rewritten in place (the
  map cites their ids); M23-M28 are new in a new §12.4. M22 now unchecks Roll
  and removes the column first, because under demand a skipped record would be
  fitted again at once. M28 provokes a job-level failure with a locked session
  file, the only such failure a person can cause on purpose.
- **Honest priority note**: `QThread::LowPriority` lowers the OS priority on
  Windows and macOS only; the documents say that on Linux the guarantee is the
  responsiveness of the main thread (Phase 1's open question).
- **The external-cancel note** goes in §15.3 (Phase 1's open question): the
  jobs dock will need a policy.
- **Restart and failures**: a stored rejection of a session that is not loaded
  is badged with its reason after a restart, from the index's
  `"recordReasons"` (CALCULATIONS §16.7, DATA_SCHEMA §11 and §12); the
  remaining Phase 2 open question (a column that reaches a requested
  calculation only through an alternative candidate would load each session
  once per run) is stated in CALCULATIONS §16.8 and §17.
- **`src` comments** are edited only where a new rule trips or a document
  reference moved (Task 4.8); other stale wording is reported. The phase
  assignment keeps product code out of this phase.

### Open Questions
- None blocking.

## Definition of Done

This phase is complete when:
1. Every task's acceptance criteria pass.
2. `ctest --test-dir build-phase1/FlySightViewer-build -C Release` passes,
   `-L audit` included (and `cmake --build build-phase1 --config Release`
   succeeds if Task 4.8 edited a comment).
3. The documents follow the style of their existing sections (numbered
   sections, bold lead-ins, tables for vocabularies, no class layouts in user
   documents, no links to `PLANS/`).
4. No TODOs or placeholder text remain. The only files changed are
   `docs/CALCULATIONS.md`, `docs/COMPUTED_PLOTS.md`, `docs/SENSOR_FUSION.md`,
   `docs/DATA_SCHEMA.md`, `python_plugins/README.md`, `README.md`,
   `tests/README.md`, `tests/acceptance_map.txt` and
   `tests/audit/cleanup_audit.cmake`, plus the comment-only edits of Task 4.8
   listed in the phase report. `PLANS/`, `experiments/` and `build*/` are
   untouched.

```
Phase 4 documentation complete.
- Tasks: 9
- Estimated complexity: 18 (M, L, M, M, M, M, L, S, S)
- Ready for implementation: Yes
```
