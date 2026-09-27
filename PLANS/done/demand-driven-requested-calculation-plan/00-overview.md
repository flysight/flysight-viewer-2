# Implementation Plan: Demand-driven requested calculations

## Feature Specification

The complete specification (`PLANS/demand-driven-requested-calculations.md`,
dated 2026-09-25) follows unabridged, headings demoted by one level. Its Commit
Policy section is reproduced as its own top-level section at the end of this
document and is not repeated here. The two specifications it amends,
`PLANS/done/store-requested-calculations.md` and
`PLANS/done/stored-results-validity.md`, are implemented on the working branch
(head `b55869d`); their plans are archived in
`PLANS/done/store-requested-calculations-plan/` and
`PLANS/done/stored-results-validity-plan/`. The committed code is
authoritative over those archived plans.

## Demand-driven requested calculations

Date: 2026-09-25 (revised after review, and again after the plan review, the same day)
Status: specification for planning. Not an implementation plan.
Baseline: branch `store-requested-calculations`, which implements
`PLANS/store-requested-calculations.md` and `PLANS/stored-results-validity.md`.
Where this document and those two disagree, this one wins.
Related: `docs/CALCULATIONS.md` (sections 13, 15, 16, 17),
`docs/COMPUTED_PLOTS.md`, `docs/DATA_SCHEMA.md` (sections 11, 12),
`docs/SENSOR_FUSION.md`, `PLANS/jobs-dock-clean.md` (a later feature that
reads the job history this document leaves in place).

This document says what the change must do and the boundaries it must
respect. It avoids class layouts and function signatures unless a name is
part of the observable contract. The planning agent decides phasing; the
implementation agent decides code structure.

### 1. Motivation

A requested calculation is one that is too slow to compute on the fly;
sensor fusion is the only one today. Its work starts only from a gesture:
checking a plot queues it for the visible sessions that lack a result, and
afterwards a refresh control on the plot row queues whatever is still
missing. In use this misses the mark. Making another session visible while
the plot is checked does not compute it until the user presses refresh;
after a cancel the user must refresh again; a user who does not know the
underlying mechanism is surprised that a refresh is needed at all. The
user's intent is already expressed by what is switched on: a checked plot
means "show this for the visible sessions", and nothing else is needed to
say it again.

Logbook columns over a requested calculation have the opposite problem:
nothing ever requests their results. A column is meant to be "just there",
a value per session that can be sorted and compared across the whole
logbook, without the user thinking about computation. Today such a column
shows a value only for sessions whose result was requested through a plot.

### 2. Principles

- The user expresses intent through plots and logbook columns, never
  through calculations. What is switched on is the request.
- Anything needed to complete what the user has switched on is wanted at
  once; anything no longer needed is dropped.
- Finished work is never wasted: results are stored.
- Background work must not degrade the rest of the application.
- A failure is shown, never retried in a loop.
- Each component's contract can be stated without naming the others.
  Section 12 gives the contracts; a change that makes one component need
  to know how another works is a sign the boundary is in the wrong place.

### 3. Scope

In scope:

- Deriving what to compute from checked plots, visible sessions and enabled
  logbook columns, and keeping the background work equal to it.
- Removing the refresh and cancel controls of plot rows; a working
  indicator and hover detail on plot rows and column headers.
- Filling logbook columns over requested calculations for every session in
  the logbook, loaded or not.
- Priority between plot demand and column demand.
- Tests and documentation.

Out of scope:

- Any change to the fusion kernel, its outputs, or the record format.
- The jobs dock (`PLANS/jobs-dock-clean.md`). The job history it will read
  stays as it is.
- Running more than one requested calculation at a time (section 11 says
  what must not be precluded).
- Persisting demand or job history across restarts.
- A user-facing switch that pauses or throttles background work.

### 4. Terms

- **Requested calculation**: a calculation registered with
  `EvaluationPolicy::Explicit`.
- **Requested output**: an output that can only be produced, directly or
  through on-demand calculations, by one or more requested calculations
  (the engine's blocker inspection names them).
- **Result**: a requested calculation's outputs for one session, in memory
  or stored beside the logbook as a record.
- **Pair**: one (session, requested calculation).
- **Demand**: the set of pairs that something the user has switched on
  needs and that have no result.
- **Demand layer**: the one component that derives demand and decides what
  runs next (section 12).
- **Executor**: the component that runs one requested calculation for one
  loaded session on the worker thread (today `JobQueue`).
- **Column worker**: the idle scheduler's lowest-priority task, which fills
  missing cached column values (today the column task of `SessionModel`).

### 5. Demand

Demand is derived continuously from the application's state:

1. **Plot demand.** For every checked plot whose value is a requested
   output, every visible session needs the requested calculations that
   block that output for that session.
2. **Column demand.** For every enabled logbook column whose value depends
   on a requested output, every session in the logbook needs the requested
   calculations that block that column's value for that session.

A pair is in demand only while it has no result. A result counts whether it
was published in this run or restored from storage, and whether it is a
success or an input-determined failure (a rejection or solver failure),
which is a result like any other. A pair whose calculation cannot apply to
the session (a declared input is missing) is never in demand and is not
reported anywhere, as today.

**Whether a result exists** is answered from two sources, and only these:

- For a loaded session, the engine's blocker inspection of the wanted
  name, exactly as the plot rows use it today.
- For a session that is not loaded, the logbook's knowledge of which
  record files exist (names only; no record is opened), together with
  each record's outcome as the logbook index records it. A column's cell
  has a result only when every requested calculation it needs has a
  record; a missing record puts that pair in demand. The index records,
  beside each record's name, the reason when the stored result did not
  produce its outputs (a rejection or a solver failure). It learns the
  reason at the two moments something has the record open anyway, when
  the record is written after a publish and when it is restored (at a
  load, or into the column worker's temporary copy), and forgets it when
  the record is deleted. Such a record is a result like any other: the
  pair is not in demand, and the session is listed as failed with that
  reason, before and after a restart alike, without being loaded. A record
  whose outcome the index does not know yet (written by an earlier build)
  counts as a success until it is next restored. A known record counts as
  a result until something restores it and finds it stale. The column
  worker already does that when it fills the session's column values from
  a temporary copy, and deletes a stale record; the record change it
  causes is what moves the pair into demand. The demand layer performs no
  staleness check of its own for unloaded sessions.

Demand does not depend on how the state arose: a gesture, applying a
profile, or restoring the previous view at start-up create demand the same
way. (At start-up every session is hidden, so plots create no demand until
sessions are shown; enabled columns do.) Nothing about demand is persisted:
at the next start it is derived again from what is switched on. Applying a
profile that carries a column over a requested output therefore computes
that calculation for every session in the logbook that lacks a result; that
is intended, and it is why no default profile should carry such a column.

When a requested calculation depends on another requested calculation, the
demand covers both, upstream first, as blocker inspection orders them.

### 6. Work follows demand

- A pair that enters demand is wanted at once. No gesture is needed:
  checking a plot, showing a session while a plot is checked, enabling a
  column, or an input change that invalidates a demanded result all create
  the want, and the indicator (section 10) shows it immediately.
- A pair that leaves demand (the plot is unchecked, the session hidden, the
  column disabled, or the result appeared by other means) is dropped before
  it starts. There is no queue of accepted requests to prune: demand is the
  only list of waiting work.
- The running job is never stopped because its pair left demand: it
  finishes, and its result is published and stored. It is stopped only as
  today when its inputs change (the engine would refuse the result), its
  session goes away, or the application closes.
- After an input change on a demanded session, the pair is in demand at
  once but is not started until the session's inputs have been still for a
  short moment, so a burst of edits runs one job, not many. Showing,
  hiding, checking and enabling take effect without such a wait. (The
  engine already stops a running job whose inputs changed; the wait only
  decides when the replacement starts.)
- Jobs run one at a time on the executor's worker thread.

### 7. Priority

The demand layer chooses the next job when a job ends and whenever demand
changes while no job is running, from the demand as it is at that moment,
not from the order in which pairs entered it:

1. Plot demand first: the focused session, then the other visible sessions
   in logbook row order.
2. Then column demand: visible sessions first, then the other loaded
   sessions, then sessions that are not loaded as the fill loads them, each
   group in logbook row order. What the user is looking at is computed
   before what only the logbook shows.

A session made visible while column demand is being worked through is
therefore computed next; it waits at most for the running job. The running
job is not preempted.

### 8. Failures

- An input-determined failure (rejection, solver failure) is a result: it
  is stored, shown with the warning badge and its reason as today, and
  never run again.
- A failure of the job itself (the worker could not start, the computation
  ran out of memory, or it failed for a reason that is not a function of
  its inputs) is shown with the same warning badge and its reason. The pair
  is not started again in this run of the application unless its inputs
  change. Such failures are not stored, so the next start of the
  application tries again. The memory of such failures belongs to the
  demand layer, like today's memory of refused requests.
- There is no retry control.

### 9. Sessions that are not loaded

Column demand covers sessions that are not loaded; the executor needs its
session loaded, so that the result is published into the session's own
engine, the record is written by the same path as for a visible session,
and the column value is computed from it by the existing loaded-row
refresh. The demand layer, not the column worker, is responsible for
getting such sessions loaded:

- For a pair of column demand whose session is not loaded, the demand
  layer has the session loaded the way showing it would (a real load, with
  its stored results restored and its engine attached), without making it
  visible. A session loaded this way is an ordinary hidden loaded session:
  it lives in the in-memory pool of hidden sessions and is pinned from its
  load until no column it needs is still waiting or running (so a chain of
  requested calculations keeps it loaded between links). When the pin is
  released the session stays in the pool until the pool evicts it as
  usual.
- At most a small, fixed number of sessions are loaded for this purpose at
  a time, so a large logbook never holds many sessions in memory beyond the
  pool's capacity. The next one is loaded when one has ended. The bound is
  not smaller than the number of jobs that may run at once (section 11).
- The fill is a task of the idle scheduler, at a lower priority than
  saving, loading visible sessions, bulk edits and column work; its steps
  are the loads. The task has work while any session has a column cell
  waiting or running, and it can step only while a hold is free and a
  session that is not loaded is waiting. Running the loads through the
  scheduler keeps the existing ordering guarantees by construction: saves
  and bulk edits still come first, so no result is computed from a file
  that is about to be rewritten. The logbook's progress line reports the
  fill for its whole duration, from the first load to the last result, as
  it reports on-demand column values today. For this the scheduler gains
  one generic notion: a task may have work it cannot step right now. Such
  a task is reported as active with its progress; the scheduler steps the
  highest-priority task that can step, and when none can it rests until it
  is woken instead of spinning. The scheduler still learns nothing about
  jobs: the task is one more source of steps.
- The session-id correction that happens when a session is first loaded is
  part of that load and takes place before the pair is offered to the
  executor.
- A session whose requested calculation turns out not to apply (a missing
  input, known only once it is loaded) is settled as not applicable for
  this run without a job; its column value was already cached as
  unavailable by the column worker and stays so.
- The column worker is unchanged. It settles a column over a requested
  output as unavailable when the session has no record, computes it from
  its temporary copy when there is one, and never knows that a job exists.
  When a job later writes the record, the existing record-changed path
  drops the cached value and the loaded-row refresh computes the new one.
  Cheap column values (those needing no requested calculation) are
  computed exactly as today, from the temporary copy, and never wait for a
  requested calculation.

### 10. What the user sees

- **No refresh and no cancel for requested calculations anywhere.** The
  plot row's refresh and cancel controls are removed. Unchecking a plot,
  hiding sessions or disabling a column is how the user changes what is
  wanted.
- **A working indicator.** A plot row and a logbook column header show a
  small animated "working" indicator at the right of their name while any
  of their demand is waiting or running.
- **Hover detail.** Hovering the indicator (or the row or header) shows how
  many sessions are done out of how many are wanted, the session currently
  being computed with its progress text, and the sessions that could not
  be computed with their reasons.
- **The warning badge** replaces the indicator when work is finished and
  some sessions could not be computed; its hover lists them with reasons,
  as the plot row does today.
- **A column cell whose pair is in demand reads as pending**, not as
  "unavailable", so the user can tell "not yet" from "never". Pending is a
  presentation of demand, supplied by the demand layer to the view; it is
  never a cached value, never written to the logbook index, and distinct
  from the row's existing pending state for a record that could not be
  read. The cached value underneath stays "unavailable" until the record
  is written, and sorting treats a pending cell as it treats an unavailable
  one.
- The logbook's existing progress line for background work (saving,
  loading, bulk edits, column values) is unchanged in form. A column fill
  is reported on it for its whole duration, as on-demand column values
  are today: the sessions remaining of those wanted, and no cancel.

### 11. Background work must not degrade the application

- The executor's worker thread runs below normal priority, so that the
  user interface stays responsive and the machine remains usable while a
  whole-logbook column fills in over time.
- Jobs run one at a time. The number that may run at once is a single
  bound of the executor, so that raising it later changes no contract of
  the demand layer or the column worker; the load bound of section 9
  follows it.

### 12. Architecture

**One concept, one demand layer, two executors.** Column work and
requested calculations are the same idea: something says which values
should exist, a reconciler finds the ones that are missing, and fills them
one piece at a time. The column worker is already demand-driven in this
sense (its demand is "a row with a value neither cached nor pending"; there
is no queue and no gesture). This change makes requested calculations work
the same way. What cannot be shared is the execution: the engine is
main-thread only and lets only the compute step of a prepared requested
calculation leave the main thread; cheap values take milliseconds by the
thousand and must be sliced between saves and loads, while a fit takes
minutes and needs a worker thread, cancellation, progress and a history;
and the units differ (a row's missing cells from a throwaway copy, versus
one calculation for one loaded session whose result must land in a live
engine). So there are two executors under one demand layer, and the flow
between them is one way.

**The demand layer** replaces the plot rows' request logic (today
`PlotRequests`: the missing, pending, failed classification, the refresh
gesture, the waiting sets, the pruning of unwanted queued jobs, the memory
of refused requests). It is widget-free. It reads the plot model (checked
plots), the session model (visible, focused, loaded sessions and row
order), the enabled logbook columns, the engine's blocker reports for
loaded sessions, and the logbook's record set, with each record's
outcome, for unloaded ones. It derives
demand, applies the priority of section 7, has sessions loaded for column
demand within the bound of section 9, offers the executor the next pair,
remembers this run's job-level failures and not-applicable pairs, and
publishes the per-plot and per-column state that the indicators, hover
details, badges and pending cells present. It is the only caller of the
executor. Nothing calls back into it: it observes the models and the
executor's signals.

**The executor** (today `JobQueue`) stays the only place a requested
calculation runs, with the engine's prepare / compute / publish steps, one
at a time, and still never loads a session. It stops being a queue. The
choice of what runs next needs focus, visibility and row order, which the
executor must not know, so it holds at most the running job and the one
job the demand layer has chosen to run next; it keeps no order of arrival,
no deduplication against a list, and no pruning of unwanted work, because
demand is that list. Its lifecycle (start, stale-while-running, cancel,
supersede, fail, shutdown), its pinning of the running session, the
publication and storing of results, and the job history in `JobModel`
that the jobs dock will read are unchanged.

**The column worker and the idle scheduler** keep their tasks and
priorities (save, load, bulk edit, column work). Column work is not
changed: its contract remains that cached column values are a function of
what is on disk. The demand layer's column fill (section 9) is a new task
at the lowest priority. The scheduler gains the generic notion of a task
with work it cannot step right now, and no knowledge of jobs. The logbook
manager and the result store record each stored result's outcome in the
index (section 5); that is a fact about what is on disk, learned where the
record is already open, and nothing about demand.

**The flow is one way.** The demand layer chooses; the executor publishes
into the loaded session and its listener writes the record, whose outcome
the index notes; the record
change drops the cached column values over it and the existing loaded-row
refresh recomputes them; the demand layer sees the result through the same
blocker inspection and record set it always reads, and the pair leaves
demand. No component asks another what it intends to do.

**What stays true from the earlier specifications:** restoring a stored
result is not requesting (only a missing result creates demand); the
temporary copy used for cheap column values never writes a stored result
and never requests; a restored result is indistinguishable from a
published one; the engine's threading rules are unchanged; the plot
widget's "no data" warning keeps asking the engine directly.

### 13. Tests

- Checking a plot starts the visible sessions without a result; showing
  another session while it is checked starts that session with no other
  action; hiding a session drops its waiting pair; unchecking the plot
  drops all its waiting pairs; the running job finishes and its result is
  stored.
- A session made visible during column demand is the next job to start;
  within column demand, visible sessions come before hidden loaded ones,
  and those before sessions not yet loaded.
- Enabling a column over a requested output wants every session without a
  result, loads sessions that are not loaded a bounded number at a time as
  hidden, pinned sessions, fills the column as results are published, and
  ends with every session either computed, not applicable, or failed; at
  no time are more than the bound of sessions held for this purpose, and
  they leave the hidden pool by ordinary eviction afterwards.
- The column worker's behaviour and statistics are unchanged by column
  demand: it settles and computes exactly as before, and a stale record it
  deletes moves the pair into demand.
- Stored results are restored, not recomputed: enabling the column on a
  logbook whose sessions all have valid stored results creates no job. A
  stored rejection of a session that is not loaded is listed as failed
  with its reason after a restart, without loading it. A column over a
  chain of requested calculations whose upstream record alone is stored
  is completed.
- An input-determined failure is stored, badged and never run again; a
  job-level failure is badged, not run again in the run, and tried again
  after a restart.
- A burst of input changes on a demanded session produces one job; the
  indicator shows from the first change.
- Applying a profile with such a column creates demand; start-up with a
  checked plot and no visible sessions creates none.
- The working indicator and hover detail reflect waiting, running, done
  and failed counts on plot rows and column headers; no refresh or cancel
  control exists.
- A pending column cell is distinguishable from an unavailable one, is
  not written to the logbook index, and becomes the value when the record
  is written.
- Saves and bulk edits still precede the fill's loads; no result is
  computed from a file being rewritten. The progress line reports the fill
  from its first load to its last result, and the scheduler does not spin
  while the fill waits on a job.
- The executor never holds more than the running job and one chosen next
  job; changing demand replaces the chosen next job.
- The UI thread is not blocked by a running requested calculation, and the
  calculation runs below normal priority.
- The existing tests of stored results and of the executor pass, with
  those that asserted the refresh gesture, the queue order or the pruning
  of queued jobs rewritten to the demand rules.

### 14. Documentation

`docs/CALCULATIONS.md` sections 15 (background jobs: the executor is no
longer a queue), 16 (plot-driven requests, rewritten as the demand layer)
and 17 (logbook columns over requested results); `docs/COMPUTED_PLOTS.md`
(what the user sees: no refresh, the working indicator, failures);
`docs/SENSOR_FUSION.md` section 2 (using it); `docs/DATA_SCHEMA.md`
sections 11 and 12 where they say a column over a requested calculation
stays unavailable or pending until the calculation is requested from the
plot list, and section 11 for the record outcomes the index now carries.


## Phases

| Phase | Name | Purpose | Dependencies |
|-------|------|---------|--------------|
| 1 | Executor and plot demand | Turn `JobQueue` into an executor that holds at most the running job and one chosen next job (no FIFO, no pruning, one bound, below-normal worker priority), and replace `PlotRequests` with the widget-free demand layer `CalculationDemand` covering plot demand, priority, the input-settle wait, failure and not-applicable memory, and per-plot state; plot rows lose refresh and cancel. | None |
| 2 | Column demand | Extend the demand layer to enabled logbook columns over every session in the logbook: record-set knowledge for unloaded sessions, a lowest-priority idle-scheduler load task that loads a bounded number of hidden, pinned sessions, second-tier priority, and per-column and per-cell state; the column worker stays unchanged. | Phase 1 |
| 3 | Working indicator, hover detail and pending cells | Present the demand layer's state: an animated working indicator and warning badge with hover detail on plot rows and logbook column headers, and pending logbook cells distinct from unavailable ones, without touching cached values or the index. | Phase 2 |
| 4 | Documentation, acceptance map and audit | Rewrite `docs/CALCULATIONS.md` §15-17, `docs/COMPUTED_PLOTS.md`, `docs/SENSOR_FUSION.md` §2 (and §7's refresh wording), `docs/DATA_SCHEMA.md` §11-12 and `README.md`; add acceptance items, tests/README traceability and manual steps, and an audit group for the demand boundaries. | Phase 3 |

## Dependency Graph

```
Phase 1  Executor and plot demand
   │      (executor API, CalculationDemand, per-plot state, gestures gone)
   ▼
Phase 2  Column demand
   │      (column demand, hidden-load task, per-column / per-cell state)
   ▼
Phase 3  Working indicator, hover detail and pending cells
   │      (PlotRowDelegate indicator, logbook header view, pending cells)
   ▼
Phase 4  Documentation, acceptance map and audit
```

Strictly sequential. Phases 1-3 all edit `src/calculationdemand.{h,cpp}`,
`src/mainwindow.cpp`, `tests/CMakeLists.txt` and `tests/audit/cleanup_audit.cmake`,
so no two of them run in parallel.

## Key Patterns & References

### Request logic being replaced (Phase 1)
- `src/plotrequests.h` / `src/plotrequests.cpp` — today's plot-row request logic: `PlotTrackCondition`, `PlotRowState` (counts, `waitingTotal`/`waitingDone`, `progressLabel`, `toolTip`, `showsWarning`), class comment h:113-192, `isExplicitBacked` (cpp:127, `CalculationRegistry::dependsOnExplicit`), `syncCheckedSet`, `rebuildRelevantNames`, `inspectUnderGuard`/`inspect`/`classify` (226-345), coalesced pass via zero-interval `m_updateTimer` (`scheduleUpdate` 376, `recompute` 518), `requestBlockers` (580, the one `JobQueue::request` call at 591), `continueAfter` (673, chained calculations), refused memory `m_refused` (574; cleared at 893/917/981), `buildToolTip` (394), `isMerelyUncomputed` (216), slots 871-981. Replaced wholesale by `CalculationDemand`.
- `src/ui/docks/plot/PlotWidget.cpp:535-538` — uses `PlotRequests::isMerelyUncomputed` to suppress the "No data available" warning; must keep working (the widget still asks the engine directly).
- `src/mainwindow.cpp:205-225` (construction after the model is populated, `AppContext` wiring), `346-349` (delete order: requests before queue), `363-370` (`closeEvent` → `shutdown()`), `288` (`setPlots(dependentPlots())`); `src/mainwindow.h:135-136`; `src/ui/docks/AppContext.h:31-32`.

### Executor (Phase 1)
- `src/jobqueue.h` / `src/jobqueue.cpp` — class comment h:21-99 (lifecycle, STALE WHILE RUNNING, first-writer-wins end, pinning, end order); `RequestResult::Kind` (h:117); `request` (cpp:164/173; readiness mapping 192-205; pin at 223); `activeJob`/`activeJobs`/`runningJob`/`job`/`isIdle`; FIFO `oldestQueued` (284); `scheduleStart` (303, always from the event loop); `startNext` (311, prepare + PrepareOutcome → Superseded); `JobWorker : QThread` (57, 64 MiB stack, no priority set; created 365-372, started 391); `finishRun` (423, `ticket->publish()` 443); `endJob` (490, unpin 505, `publishCalculationInvalidation` 499); `requestStop` (519); `cancel`/`cancelSession`/`cancelAll`/`cancelUnwantedQueued` (535-577); `stopRunIfRefused` (606); `shutdown` (638); test seam `failNextWorkerStarts`.
- `src/jobmodel.h` / `src/jobmodel.cpp` — `JobState` (Queued/Running/Succeeded/Cancelled/Superseded/Failed), `JobRecord`, friend mutators (`append`, `markRunning`, `markCancelRequested`, `setProgress`, `markFinished`, `trimFinished`). Job history stays as it is (the jobs dock reads it).
- `src/engine/calculationengine.h` / `.cpp` — `blockers(name)` (h:299, cpp:2090 via `inspectName` 2008), `readiness(id, instanceOutput)` (h:302, cpp:2100), `prepare` (cpp:1359), `setExplicitResultListener` (h:158, cpp:908), `ExplicitResultEvent` (h:122).
- `src/engine/blockerreport.h` — `CalculationBlocker`, `UnproducedNote`, `BlockerReport::State` (Available/Blocked/NotProduced/NotApplicable), `CalculationReadiness::State` (Unknown/MissingInput/Blocked/Ready/Done).
- `src/engine/calculationregistry.cpp` — `explicitDependencies` (458), `dependsOnExplicit` (486).

### Plot model, session model state the demand layer reads (Phases 1-2)
- `src/plotmodel.h` / `.cpp` — roles (h:21-30, `PlotValueIdRole`), `enabledPlots` (118), `setPlotEnabled` (144), `isPlotEnabled` (167), check state in `setData` (292).
- `src/sessionmodel.h` / `.cpp` — `SessionRow` (`visible`, `isLoaded`, `loadFailed`, `pendingColumns` h:45), `setRowsVisibility` (857, emits `visibilityChanged(shown, hidden)`), `focusedSessionId`/`setFocusedSessionId` (1173/1178), `getSessionRow` (1130), `rowAt`, `rowCount`, `loadedSession` (1061), `forEachLoadedSession` (1048), `dependencyChanged`, `publishCalculationInvalidation` (1724), `RowStabilityGuard` (h:193), pinning `pinSession`/`unpinSession`/`isSessionPinned` (1744/1751/1773; PINNED SESSIONS comment h:142-150).
- `src/profilestatebridge.cpp:158` — `applyProfile` sets checked plots (`PlotModel::setPlotEnabled`, ~161-167) and columns (`LogbookColumnStore::setColumns`, ~255); called from `mainwindow.cpp:310` and `1344`. Demand must arise the same way from it.

### Logbook columns, column worker, loading, records (Phase 2)
- `src/logbookcolumn.h` / `.cpp` — `LogbookColumn`, `logbookColumnNames` (h:96), `logbookColumnExplicitCalculations` (h:101: what makes a column depend on requested outputs), `LogbookColumnStore` (`enabledColumns` cpp:212, `setColumns`, signal `columnsChanged`).
- `src/sessionmodel.cpp` — `rebuildColumns` (158), `rebuildColumnDependencies` (1433), `isExplicitBacked(col)` (1454), `needsColumnWork` (1460), `invalidateColumns`/`invalidateAllColumns` (1469/1496), `fillMissingColumns` (1521), `settleExplicitColumns` (1570; "unavailable" = invalid QVariant in `cachedValues`, 1591-1605), `restoreForColumnWorker` (1608), `onCalculationRecordsChanged` (1653) → `queueRecordColumnRefresh` (1680) → `refreshRecordColumns` (1690), `startColumnWorker`/`cancelColumnWorker`/`processNextDirtyColumn` (2063/2082/2087, temporary copy, id remap 2127), `computeColumnValues` (2356), `sort` (2395).
- `src/sessionmodel.cpp` loading — `sessionRef(row)` (1073: the real load; session-id correction via `setRowSessionId` 800 at 1084, `attachSession` 1288 with the explicit-result listener 1307-1309, `restoreStoredResults` 1313, `sessionLoaded`, LRU touch/evict), `resolveIdentityStubs` (783), background visible load queue `m_loadQueue`/`loadNextVisibleSession`/`cancelLoader` (1907/1935), hidden LRU `m_lruList`/`m_cacheCapacity` (preference `LogbookCacheSize`, default 50), `lruTouch/Remove/Insert` (1942-1953), `evictIfNeeded` (1959), `evictSession` (1978).
- `src/idlescheduler.h` / `.cpp` — `TaskDef{priority, step, hasWork, progress, onComplete, cancellable}`, `registerTask` (15), `wake` (27), `cancel` (33), `tick` (49); signals `activeTaskChanged`, `progressChanged`, `schedulerIdle`. `SessionModel::WorkerTask` (h:180: Save=0, Load=1, BulkEdit=2, Column=3) registered in the SessionModel ctor (cpp:34/48/91/104, priorities 1-4); `scheduler()` (h:190).
- `src/calculationresultstore.h` / `.cpp` — `CalculationResultStore` (`onExplicitResultEvent` cpp:58, `restoreSession` 105, `deleteRecord` 279, `Stats`).
- `src/logbookmanager.h` — `writeCalculationRecord` (163), `readCalculationRecord` (171), `calculationRecordIds` (175), `removeCalculationRecord` (185), `knownCalculationRecords` (198), `unconfirmedCalculationRecords` (201), `discardUnconfirmedCalculationRecords` (206), `markCalculationRecordSkipped` (216), `reserveSessionFile` (309), signal `calculationRecordsChanged` (359).

### Plot rows UI (Phases 1 and 3)
- `src/ui/docks/plotselection/PlotRowDelegate.h` / `.cpp` — class comment h:17-63; glyphs `drawRefreshGlyph` (42), `drawCancelGlyph` (75), `drawWarningGlyph` (92); `stateFor` (153), `controlLabelFor` (172), `geometryFor` (182), `controlRect` (192), `paint` (205), `editorEvent` (265: refresh 305, cancel 307, `plotCheckedByUser` 331), `toolTipFor` (337), `helpEvent` (342), `onRowStateChanged` (358).
- `src/ui/docks/plotselection/PlotRowLayout.h` — `PlotRowMetrics`, `PlotRowGeometry`, `layoutPlotRow()`.
- `src/ui/docks/plotselection/PlotSelectionDockFeature.cpp` — the `QTreeView` and `setItemDelegate(new PlotRowDelegate(ctx.plotRequests, ...))` (29).

### Logbook view UI (Phases 2 and 3)
- `src/ui/docks/logbook/LogbookView.cpp` — `setupView` (70, `setSortingEnabled(true)` 97; plain `QTreeView`, no custom header view), progress line slots `onActiveTaskChanged`/`onProgressChanged`/`onSchedulerIdle` (266/273/302), task labels (283-293).
- `src/ui/docks/logbook/LogbookDockFeature.cpp:36-49` — scheduler → progress line wiring; `cancelRequested(taskId)` → `scheduler().cancel`.
- `src/sessionmodel.cpp` — `data()` (381: loaded rows live, stubs from `cachedValues`, invalid renders empty), `headerData` (428), `formatRawValue` (329).

### Tests and test support
- `tests/tst_plot_requests.cpp` (becomes `tst_calculation_demand.cpp`), `tests/tst_jobqueue.cpp`, `tests/tst_jobmodel.cpp`, `tests/tst_plot_row_delegate.cpp` (widget tests, `FLYSIGHT_BUILD_WIDGET_TESTS`), `tests/tst_plot_row_layout.cpp`.
- `tests/tst_result_store.cpp`, `tests/tst_result_columns.cpp`, `tests/tst_result_records.cpp`, `tests/tst_column_cache.cpp`, `tests/tst_logbook_index.cpp`, `tests/tst_session_model_engine.cpp`, `tests/tst_workflow.cpp`.
- Fusion: `tests/tst_fusion_jobs.cpp`, `tests/tst_fusion_rows.cpp` (35 refresh/cancel gesture uses), `tests/tst_fusion_store.cpp` (20).
- Support: `tests/support/jobfixture.h/.cpp`, `tests/support/plotfixture.h/.cpp` (`spin(PlotRequests*)` h:67), `tests/support/storedresults.h/.cpp`, `tests/support/logbookprobe.h/.cpp`, `tests/support/testenvironment.cpp`.
- `tests/CMakeLists.txt` — `flysight_add_test` (71); current registrations 146-187; widget tests 165; `flysight_add_fusion_test` (301; fusion tests 340-348); `audit_cleanup` (511-520).
- `tests/acceptance_map.txt` — ranges 1-19, 101-120, 201-247, 301-350, 401-442; line forms `<item> <tst> <fn>`, `manual Mk`, `ci`, `audit <group>`. New items for this feature start at 501.
- `tests/README.md` — test table (~78-141), §9 traceability (658; 9.1-9.5), §10 cleanup audit (976; prose on gestures ~1029-1032), §12 manual steps (1608; 12.1 plot-driven jobs 1614, 12.3 stored results 1730; M1, M3, M16, M18, M19, M22 describe refresh/cancel), appendices B/D/E.
- `tests/audit/cleanup_audit.cmake` — groups `one-worker` (354), `gestures` (366-404: gesture names only in plotrequests/PlotRowDelegate, exactly one `.request(` in src, prepare/publish only in jobqueue/engine, `JobQueue|JobModel` in src/ui only in AppContext.h, `EvaluationPolicy::Explicit` only in engine/fusionregistration), `widget-free-core` (407), `stored-results` (516; "restoring is not requesting" 556), `result-validity` (603), `FUSION_CORE` list (266).

### Documentation (Phase 4; earlier phases update comments in code only)
- `docs/CALCULATIONS.md` — §13 Blocker inspection (577), §15 Background jobs (658; 15.1-15.8, 15.8 Stored results 939), §16 Plot-driven requests (1054; 16.1-16.9), §17 Sensor fusion (1411-1580).
- `docs/COMPUTED_PLOTS.md` (161 lines, §1-§9).
- `docs/SENSOR_FUSION.md` — §2 Using it (33-56), refresh wording in §7 Lifecycle (334, 342-358).
- `docs/DATA_SCHEMA.md` — §11 Logbook column cache (299-387), §12 Stored calculation results (388-555).
- `README.md:416` — link text naming "the refresh control and its count, progress and cancel".

## Decisions & Constraints

- **Names.** The demand layer is a new widget-free `QObject` class `CalculationDemand` in `src/calculationdemand.h` / `.cpp`, in the same build target that holds `plotrequests.cpp` today. `PlotRequests` and its files are deleted in Phase 1; what survives of its types (per-plot state, tooltip text builder, `isMerelyUncomputed`) moves into the demand layer's header under demand-layer names. The executor keeps the class and file names `JobQueue` / `jobqueue.{h,cpp}` (about 244 references; the jobs dock reads `JobModel`); comments and docs call it "the executor". `JobModel` and `JobState` are unchanged.
- **Executor contract (Phase 1).** The queueing `request()` API, FIFO order (`oldestQueued`), deduplication against a list and `cancelUnwantedQueued` go. The executor holds at most the running job and one chosen next job. The demand layer offers a (session, calculation) as the chosen next job; an offer that differs from the current chosen next replaces it, and the replaced job ends `Cancelled` in the job history (the existing "nothing wanted it any more" meaning); an offer equal to it changes nothing. The demand layer can also withdraw the chosen next job. When nothing is running, an offered job starts from the event loop, never synchronously (today's `scheduleStart` discipline). The offer reports the engine's readiness verdicts (MissingInput, NothingToDo, Blocked, SessionNotLoaded, UnknownCalculation, ShuttingDown) as `request()` does today, so the demand layer learns "not applicable" as `PlotRequests` does now. A single `constexpr` bound of simultaneous jobs (value 1) lives on the executor. The worker thread starts with `QThread::LowPriority`. Lifecycle, stale-while-running, supersede, pinning of the running session (and of the chosen next job's session), publication and storing, `failNextWorkerStarts` and shutdown are unchanged. No product code cancels the running job except as the spec lists (inputs changed, session gone, shutdown); whether `cancel(JobId)` stays as an API for the later jobs dock is the Phase 1 documenter's call, but no UI calls it.
- **Only caller.** `CalculationDemand` is the only product caller of the executor's offer API (the audit's "exactly one call site" rule moves from `.request(` to the offer). Nothing calls back into the demand layer; it observes `PlotModel`, `SessionModel`, `LogbookColumnStore`, `LogbookManager::calculationRecordsChanged`, the engine's blocker reports and the executor's signals.
- **Priority (spec §7), resolved.** At each choice: (a) plot demand of the focused session; (b) plot demand of the other visible sessions in logbook row order; (c) column demand: visible sessions, then the other loaded sessions, then sessions that are not loaded as the fill loads them, each group in logbook row order (what the user is looking at first). Within a session, upstream calculations first, as blocker inspection orders them. A choice is made when a job ends and whenever demand changes; the demand layer keeps the executor's chosen next job equal to its current choice, so the executor's next job is always the demand layer's answer "as it is at that moment".
- **Input-settle wait.** A named constant of the demand layer (on the order of one second; exact value chosen in Phase 1), restarted per session by each input change (`dependencyChanged`). A pair of a session inside its settle wait is in demand and counted as waiting at once, but is not offered until the wait ends. Showing, hiding, checking, enabling and profile application take effect without the wait. Tests get a seam to shorten it.
- **Failure memory.** Job-level failures (`JobState::Failed`) and not-applicable verdicts are remembered by the demand layer per (session, calculation instance), with reasons; cleared for a session when its inputs change, and wholly on session-model reset and registry change (as `m_refused` is today). Input-determined failures (rejection, solver failure) are results and need no memory. Nothing is persisted.
- **Result existence (spec §5).** Loaded sessions (visible or hidden): the engine's blocker inspection only. Unloaded sessions: `LogbookManager`'s known record set (names; no record opened, no staleness check) together with the outcome the logbook index records for each record (the reason of a rejection or solver failure, learned when the record is written and when it is restored). A cell has a result only when every requested calculation it needs has a record; a record with a reason is a failed result, listed with that reason before and after a restart alike.
- **Hidden loads for column demand (Phase 2).** The load step is an `IdleScheduler` task at a priority below the column task (lowest of all), registered through `SessionModel::scheduler()`; the scheduler gains no knowledge of jobs. The load uses the same real load as showing a session (session-id correction, engine attach, stored-results restore, `sessionLoaded`, LRU), without making the row visible; the session is pinned from its load until it has no waiting or running column cell left (a chain keeps it), then unpinned and left to ordinary LRU eviction. The number of sessions held for this purpose is at most the executor bound plus one (so 2: the running job's session and the chosen next). The task is the column fill: it has work while any session has a waiting or running column cell, can step only while a hold is free and an unloaded session waits, and reports remaining sessions of those wanted, so the progress line shows the fill for its whole duration under the same label as on-demand column work. The `IdleScheduler` gains the generic notion of a task with work it cannot step right now (reported as active, not stepped, no spinning) and learns nothing about jobs. The column worker's code and statistics are untouched.
- **Pending cells (Phase 3).** Pending is a view-side presentation: `SessionModel::data`, `cachedValues`, `pendingColumns` and `index.json` never see it. The logbook view asks the demand layer (via `AppContext`) whether a (session, column) is in demand and draws it differently from "unavailable" and from the row's unreadable-record pending state. Sorting is untouched (the underlying value is invalid and sorts as unavailable).
- **Indicators (Phase 3).** Plot rows: the refresh/cancel control area of `PlotRowDelegate`/`PlotRowLayout` becomes the working-indicator / warning-badge slot. Logbook: a custom `QHeaderView` for `LogbookView` draws the same indicator/badge at the right of the header text and shows hover detail. Animation is driven by a timer that runs only while something is working (no idle repaint). Hover detail text is built by the demand layer's state (counts done of wanted, the running session with its progress text, failures with reasons) so plot rows and headers say the same thing.
- **Start-up.** `MainWindow` constructs the demand layer where it constructs `PlotRequests` today and deletes it before the executor. Restoring checked plots creates no plot demand (sessions start hidden); enabled columns create column demand, whose loads queue behind start-up loading by scheduler priority.
- **Audit and tests stay green per phase.** Each phase updates the `audit_cleanup` rules its code changes invalidate (Phase 1 rewrites the `gestures` group) and the tests whose asserted behaviour it changes; Phase 4 adds the new `demand` audit group, the acceptance items and the documentation. Existing stored-results and executor tests pass, with refresh/queue-order/pruning assertions rewritten to the demand rules.
- **Build and test.** Build only `build-phase1/`: `cmake --build build-phase1 --config Release`. Test: `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure` (labels core, fusion, exact, python, audit). **Never build `build/`.** Reconfigure the inner project with `cmake build-phase1/FlySightViewer-build` only after a new test source exists. A new test that links fusion must be listed in `_FLYSIGHT_GTSAM_REACHERS` in `cmake/SolverDependencies.cmake`.
- **Out of scope** (spec §3): the fusion kernel and record format, the jobs dock, running more than one job at a time, persisting demand or history, any pause/throttle switch.

## Integration Notes

(Added by the planning coordinator after the phase documents were written. Where these notes and a Decisions & Constraints bullet differ, these notes and the phase documents win.)

- **Implementation order: strictly 1 → 2 → 3 → 4.** Each phase leaves the build, the full test suite and `audit_cleanup` green. Phase 3 adds the new test source `tst_logbook_indicators.cpp` (reconfigure the inner project once); Phases 1 and 2 add no new fusion test sources.
- **Executor API (Phase 1).** `JobQueue::offer(sessionId, CalculationBlocker | CalculationId)` → `OfferResult` (the old `RequestResult` kinds), `withdrawChosenNext()`, `chosenNextJob()`, `publishingJob()`, `static constexpr kMaxRunningJobs = 1`; worker `QThread::LowPriority`. Removed: `request()`, `oldestQueued`, `cancelSession`, `cancelAll`, `cancelUnwantedQueued`. `cancel(JobId)` is kept with no product caller (for the jobs dock); an audit rule forbids product callers. A replaced chosen-next job ends `Cancelled` "No longer needed".
- **Demand layer (Phases 1-3).** `CalculationDemand` (`src/calculationdemand.{h,cpp}`): `plotState` / `columnState` → `DemandState` of `DemandTrack`s, `plotStateChanged` / `columnStateChanged` / `statesChanged`, `isCellPending(sessionId|row, columnId|column)`, `workingPlotIds()` / `workingColumnIds()` (Phase 3), `buildToolTip` (at most 10 tracks per section, then "and N more"), `isMerelyUncomputed`; `kInputSettleMs = 1000`; `kMaxHeldSessions = JobQueue::kMaxRunningJobs + 1`.
- **Refinements accepted from the phase documents:**
  - Phase 1: `publishingJob()` lets the demand layer tell its own publication's `dependencyChanged` from an edit, so chained calculations are not delayed by the settle wait. Failure / not-applicable memory is cleared on a model reset only for sessions that no longer have a row (a sort resets the model). The demand layer withdraws only a chosen-next job it offered itself. The audit group slug `gestures` is kept with rewritten rules.
  - Phase 2: the hidden-load pin starts at the load (immediately after the choice) and is released when the session has no waiting or running cell left (so chained calculations keep their hold); sessions that were already loaded are offered directly and do not count towards the bound. Within tier (c) visible sessions come first, then hidden loaded sessions, then sessions as the fill loads them. A column-level settlement memory keyed by (session, column) holds not-applicable, job-level failures and failed loads; it takes precedence over the record set and is cleared by record, input, single-row data, registry and column changes. A failed-load placeholder, visible or hidden, is settled as failed ("The session file could not be loaded"). Tracks of unloaded sessions are named by the logbook row's description (`rowDisplayName`). The fill task `SessionModel::ColumnFillTask = 4`, priority 5, label "Computing columns: %v / %m" (the same label as on-demand column work, on purpose), is **not cancellable** from the progress line (a cancel would be undone at once and spec §3 excludes a pause switch). `IdleScheduler` gains a generic `unregisterTask` and a `canStep` predicate on `TaskDef`; `SessionModel` gains `loadPinnedSession(id)`; `LogbookManager` records each record's reason in `index.json` (`"recordReasons"`), set by the result store at write and at restore.
  - Phase 3: `src/ui/docks/DemandIndicator.{h,cpp}` (glyphs + per-view animation clock), `LogbookHeaderView`, `LogbookCellDelegate`; the header shows the glyph only (no counts); pending cells are a muted "…" and do not animate; neither the header nor the cell delegate handles mouse events.
- **Known limits, documented in Phase 4 rather than changed:** a column reaching a requested calculation only through an alternative candidate would load each session once per run to find out (no such column exists); `QThread::LowPriority` has no OS effect on Linux under `SCHED_OTHER`; a job cancelled from outside is re-offered at once while in demand (a policy question for the jobs dock).
- **Not automatable:** `ProfileStateBridge::applyProfile` needs a `MainWindow`; tests replay its model calls and manual step M2 covers the real path. The Windows locked-file step M28 rests on the assumption that a file held open without sharing makes the load fail; acceptance item 526 is carried by automated tests either way.
- **`tests/README.md` §9 evidence is stale during Phases 1-3** (it still cites `tst_plot_requests` and old `tst_jobqueue` names); it is not machine-checked and Phase 4 rewrites it. `tests/acceptance_map.txt` is machine-checked and each phase keeps it valid.
- **Acceptance items** for this feature are 501-563; items 110, 111, 113-117, 304, 306, 321 and 337 are restated "(as amended)" in Phase 4.
- **Line numbers** quoted in the phase documents are those of `b55869d` and of Qt 6.9.3's sources. Locate every passage by the text quoted next to it, never by the number alone; a moved line is not a reason to stop.
- **Amendments after the plan review (2026-09-25).** The phase documents were edited in place to carry these; this list is the summary.
  - Phase 2, Task 2.2, unloaded rule: a cell of a session that is not loaded reads Done only when **every** storable calculation it needs has a record (not any), and Failed with the recorded reason when one of those records carries a reason; otherwise Waiting. With "any", a column over a chain of requested calculations whose upstream record alone was stored would never have been computed.
  - Phase 2: the logbook index records each stored result's reason (`"recordReasons"`), learned by the result store at write and at restore, so a stored rejection of an unloaded session is badged after a restart exactly as before it, without a load.
  - Phase 2: tier (c) sub-order (visible, hidden loaded, then as loaded).
  - Phase 2: the scheduler task is the column fill, `ColumnFillTask`, with `TaskDef::canStep`; the progress line shows "Computing columns: k / n" for the whole fill; `IdleScheduler` never spins while the fill waits.
  - Phase 4: the restart limit is gone from the documents; DATA_SCHEMA §11 documents `"recordReasons"`; acceptance items 513, 523, 528, 530, 539 and 546 carry the amended clauses.

## Commit Policy

Michael has authorized commits for this plan on the existing working branch.
The implementation orchestrator makes every commit; implementation, revision,
and review agents never run git commands that change repository state.
Nothing is ever pushed, and nothing is ever committed on `master` or
`fusion-improvements`. Copy this section into the plan overview as its own
top-level section with this exact heading.

- **Branch.** Confirm the working tree has no modified tracked files (if there
  are any, stop and ask). Switch to the existing branch
  `store-requested-calculations`; never recreate, reset, or rebase it.
- **Never staged:** `PLANS/`, `TEMP/`, `experiments/`, `build*/`, `dist/`,
  `results/`, and everything under `third-party/` that is not tracked.
- **One commit per accepted phase**, when the review agent returns ACCEPT,
  containing exactly the files the phase's agents reported, staged by explicit
  path. Subject `Demand phase N: <phase name>`; body a short summary, then
  the session's attribution line. Rejected iterations are never committed.
- **Tag** each phase commit `plan/demand-driven-requested-calculations/phase-N-done`;
  tags are never moved or deleted.
- **Fixes to a closed phase** are committed separately as
  `Demand phase N fixup: <what>` once reviewed and accepted.
- **CI workflow edits** are committed separately as
  `Demand phase N: CI (unverified)`.
- **Escalated phases** are not committed; their paths are listed in the final
  report, and phases that depend on them are not started.
