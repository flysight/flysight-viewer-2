# Demand-driven requested calculations

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

## 1. Motivation

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

## 2. Principles

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

## 3. Scope

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

## 4. Terms

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

## 5. Demand

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

## 6. Work follows demand

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

## 7. Priority

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

## 8. Failures

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

## 9. Sessions that are not loaded

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

## 10. What the user sees

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

## 11. Background work must not degrade the application

- The executor's worker thread runs below normal priority, so that the
  user interface stays responsive and the machine remains usable while a
  whole-logbook column fills in over time.
- Jobs run one at a time. The number that may run at once is a single
  bound of the executor, so that raising it later changes no contract of
  the demand layer or the column worker; the load bound of section 9
  follows it.

## 12. Architecture

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

## 13. Tests

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

## 14. Documentation

`docs/CALCULATIONS.md` sections 15 (background jobs: the executor is no
longer a queue), 16 (plot-driven requests, rewritten as the demand layer)
and 17 (logbook columns over requested results); `docs/COMPUTED_PLOTS.md`
(what the user sees: no refresh, the working indicator, failures);
`docs/SENSOR_FUSION.md` section 2 (using it); `docs/DATA_SCHEMA.md`
sections 11 and 12 where they say a column over a requested calculation
stays unavailable or pending until the calculation is requested from the
plot list, and section 11 for the record outcomes the index now carries.

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
