# Calculation refinements

Date: 2026-09-26 (revised after the plan review, the same day)
Status: specification for planning. Not an implementation plan.
Baseline: branch `store-requested-calculations` at the head that implements
`PLANS/done/demand-driven-requested-calculations.md` (its plan is archived
in `PLANS/done/demand-driven-requested-calculations-plan/`; the committed
code is authoritative over it). Where this document and that specification
disagree, this one wins; everything it does not mention stays as that
document specifies.
Related: `docs/CALCULATIONS.md` (sections 15 and 16), `docs/COMPUTED_PLOTS.md`,
`docs/DATA_SCHEMA.md` (section 11), `tests/README.md`,
`tests/acceptance_map.txt`, `tests/audit/cleanup_audit.cmake`,
`PLANS/jobs-dock-clean.md` (a later feature that reads the executor's job
history).

This document says what the change must do and the boundaries it must
respect. It avoids class layouts and function signatures unless a name is
part of the observable contract. The planning agent decides phasing; the
implementation agent decides code structure.

## 1. Motivation

The demand-driven design landed with its boundaries intact: the demand layer
is the only thing that starts requested calculations, the executor holds at
most two jobs, the column worker and the scheduler learned nothing about
jobs. The cost of getting there under those constraints is concentrated in
the demand layer, and it is of three kinds:

- **Workarounds for contracts below it.** The scheduler cannot complete a
  task whose work vanished without a step, so the demand layer fakes a final
  step. The logbook index learns a record's outcome silently, so the demand
  layer polls for it on an unrelated model signal. The session model already
  knows what each column depends on, and the demand layer computes it again.
  Three components compute a session's display name.
- **Two pipelines for one idea.** A plot and a logbook column are both a
  source of demand: a set of names to inspect and a set of sessions to
  inspect them for. The demand layer walks them through two separate paths
  that share only the classification of a loaded session, and it keeps two
  memories of this run, keyed differently and cleared on different events,
  for facts that overlap.
- **Generality nothing uses.** Guards against a second offerer that the audit
  forbids, executor signals and queries with no product caller, state fields
  only tests read, and presentation plumbing repeated in each view.

This change removes those costs without moving any boundary. It also makes
the two working indicators identical, gives the column fill a progress text
of its own, and turns one silent case, a result that could not be stored,
into a shown failure.

## 2. Principles

- The principles of the demand-driven specification hold unchanged: what is
  switched on is the request; anything wanted is wanted at once; finished
  work is never wasted; background work never degrades the application; a
  failure is shown, never retried in a loop; each component's contract can
  be stated without naming the others.
- A fact is computed by the component that owns it and read by the others.
  Where two components compute the same fact, one of them stops.
- A component announces what it changes. No component infers another's
  change from a signal about something else.
- One rule, one path. Where two code paths implement one rule with
  different special cases, they become one path and the special cases
  become data.
- What no product code uses is removed, unless a named later feature needs
  it, in which case it is kept and says so.

## 3. Scope

In scope:

- The idle scheduler's completion of a task whose work is gone.
- The logbook index announcing a record's outcome, and the result store
  announcing a record that could not be written.
- The session model's column dependency tables and session display name as
  the one source of those facts, and its bulk edit announcing what it
  changed.
- The demand layer: one source walk for plots and columns, one run-memory,
  and its division into parts with contracts of their own.
- Removal of the demand layer's and the executor's unused generality.
- The user interface: one indicator, one clock, one progress text for the
  fill, and shared glyph plumbing.
- Tests, the acceptance map, the audit and the documentation.

Out of scope:

- Any change to the executor's lifecycle, the fusion kernel, the record
  format, the logbook index's other contents, or the column worker's
  behaviour and statistics.
- Any change to what is in demand, its priority, the input-settle wait, the
  bound on held sessions, or the one-way flow between the demand layer, the
  executor, the store and the column worker.
- The jobs dock. The executor's cancel operation and its job history stay
  for it.
- Making the column worker load sessions for the demand layer. That was
  considered: the worker's temporary copy is restored without a listener and
  its unconfirmed marks are discarded, so installing it as a loaded session
  would re-enter the stored-results validity rules, and the worker would have
  to ask the demand layer what is wanted. The double load per session is
  bounded and the boundary is worth more.

## 4. Terms

The terms of the demand-driven specification apply. In addition:

- **Source**: something the user switches on that creates demand: a checked
  plot over a requested output, or an enabled logbook column over one. A
  source has an identity (the plot id or the column's definition key), the
  names it reads, the storable requested calculations those names depend on,
  and a kind that decides which sessions are its tracks.
- **Track**: one session of one source: for a plot, a visible loaded session
  that is not a failed-load placeholder; for a column, every session row of
  the logbook, loaded or not.
- **Verdict**: a track's condition once nothing is left to compute for it:
  done, failed, or not applicable.
- **Pair memory**: the demand layer's one memory of this run, keyed by pair
  (session, requested calculation): section 10.
- **The fill**: the demand layer's lowest-priority scheduler task, whose
  steps are hidden loads, and its holds.

## 5. The scheduler completes a task whose work is gone

Today a task completes only through a step: the scheduler checks, after
stepping a task, whether it still has work. A task that has work it cannot
step (the fill, waiting on a job) loses that work when the job ends, without
any step, and the scheduler goes idle without ever completing it. The demand
layer works around this with a final step that loads nothing and exists only
to be stepped.

The scheduler gains the missing rule for tasks that can wait, which are the
tasks registered with a step predicate: when the task it last reported
active is such a task and has no work any more, the next tick reports its
progress one last time and calls its completion (not cancelled) before it
reports the next active task or goes idle. A task that lost its work because
it was cancelled completes once, as cancelled, exactly as today. Whoever
takes a resting task's work away wakes the scheduler, as today.

The rule is scoped to tasks that can wait on purpose. The other tasks (save,
load, bulk edit, column work) can also lose their work outside a step:
hiding sessions empties the load queue, an eviction saves a dirty row, a
flush clears the bulk-edit queue. They have never been completed at those
moments, and completing them there would change behaviour; the load task's
completion, for one, would announce as shown the sessions the user has just
hidden. With the scope, the existing tasks complete exactly as before, by
construction.

Consequences:

- The demand layer's "fill ending" state, its extra step and the clause in
  the task's step predicate that admits it are removed. The fill's progress
  is the sessions with a pending column cell of the fill's high-water mark,
  as today, and the high-water mark resets when the task completes, as
  today. Because the scheduler completes only the task it last reported
  active, a fill that loses its work while another task is active is not
  completed; so a fill that starts while no session had a pending cell also
  starts its own count.
- The scheduler still learns nothing about jobs or demand.

## 6. The logbook index announces record outcomes

Today the index learns a record's reason (a rejection, a solver failure)
silently: the manager notes it, marks the index for a flush, and emits
nothing. The demand layer learns of the change indirectly, by comparing the
reasons it memoized against the manager's on every single-row display change
of a session that is not loaded, because that is the signal the column
worker happens to emit after the restore that taught the index the reason.

The manager announces the change instead: recording a reason that differs
from what it held emits the same record-changed signal it emits when a
record is written or removed, for that (session, calculation). Recording an
unchanged reason emits nothing. The demand layer's handling of a record
change (drop what it memoized of the session's records and reports, forget
the pair, schedule a pass) covers it; the reason comparison on the model's
display-change signal is removed.

The model's single-row display change carried one more fact for the demand
layer: a bulk edit changed a session's attributes without a dependency
change, so what the demand layer remembered of the session might no longer
hold. That, too, is a component inferring a change from a signal about
something else, and the same signal is emitted by the column worker for
every stub it processes. The bulk edit announces its edit instead, through
the publication a direct edit already uses: the session's dependency change
for the attribute it changed, on both of its paths (a loaded session, and
the temporary copy of one that is not loaded). The demand layer then
observes no display change of the model at all: a bulk edit reaches it as
the input change it is, with the memory clearing and the settle wait an
input change gets (sections 10.2 and 6 of the demand-driven specification),
and the column worker's display changes are never read.

## 7. A record that could not be written is a shown failure

Today, when the store's write of a published result fails, the result stays
in the loaded session's engine and the demand layer reports the track done.
Once the session is evicted the record does not exist, the pair is in demand
again, the fill loads the session, the job runs again, the write fails
again: a retry loop across evictions that nothing shows. The demand layer
hides it only by remembering, per cell, that the cell was once done.

The store announces a write that failed, with the reason, for the (session,
calculation). The demand layer remembers it in the pair memory (section 10)
as a failure of this run: the track is failed with that reason, on plot rows
and column headers alike, whether the session is loaded or not, and the pair
is not offered again until its inputs change. A later successful write of
the pair's record clears the memory (a record change of the pair). Nothing
is persisted: at the next start the pair is in demand again and the write is
tried again, exactly like a job-level failure.

A track whose source needs such a pair is not done while the session is
loaded either, although the engine holds the result: a source is done only
when none of its storable calculations is remembered this way. That keeps
the user's view the same before and after an eviction.

## 8. One source of column knowledge and session names

The session model computes, for each enabled column, the static dependency
closure of its names and the requested calculations it depends on. The
demand layer computes both again from the registry, keeps them in step with
the model's column set through dirty flags of its own, and unites the
closures into the set of names whose change is relevant.

The session model becomes the one source: it exposes, per column, the
static dependency closure and the requested calculations, valid between two
column rebuilds (a column change resets the model, which the demand layer
already observes) and current under the registrations at the moment of the
call. The demand layer reads them and computes neither. The demand layer
still computes a plot's closure and requested calculations from the
registry, since the model knows nothing of plots. The logbook index keeps
its own registry-side computation of a column's requested calculations: it
sits below the model and decides the validity of cached values before any
model exists.

A session's display name is computed in three places: the demand layer's
inspection of a loaded track reads the description attribute; its name for
a track that is not loaded rebuilds a column definition key by hand to read
the index's cached description; the executor reads the attribute again for
the job record. The session model exposes one display name of a row: the
loaded session's description, else the description the logbook index caches
for the row, else the session id. The demand layer and the executor use it
and compute none.

## 9. What goes

Removed from the demand layer:

- The memory of "the offer this component made" and the rule that only a
  chosen next job it offered itself is withdrawn. The audit makes the demand
  layer the only offerer, so the chosen next job is always its own; when the
  choice finds nothing to offer, the chosen next job is withdrawn.
- The check, before each offer, whether the candidate equals the chosen next
  job. The executor answers the same question and reports it; the demand
  layer acts on the executor's answer.
- From a track: the settling flag and the job id. From a source's state: the
  list of waiting tracks. Nothing the user sees reads them; the counts stay.
  Tests that read them assert the same facts through the counts, the
  executor's records and the demand layer's existing test seams.

Removed from the executor, which has no product caller for any of them: the
idle signal, the queued signal and the query of both active jobs. The busy
period bookkeeping that existed only to emit idle once goes with it. The
cancel operation stays for the jobs dock, and its comment and the audit say
that it has no product caller today.

The executor's refusal kinds are unchanged; the demand layer offers upstream
first, so the "blocked" refusal is not expected from it, and the
documentation says so instead of the code guarding for it.

## 10. One source walk and one memory

### 10.1 One walk

The demand layer derives its state and its candidates in one walk over the
session rows, for all sources at once. For each row and each source whose
tracks include the row:

- **A loaded session** (not a failed-load placeholder) is classified from the
  engine's blocker inspection of the source's names, combined as columns are
  combined today (any not applicable, else any not produced, else any
  blocked, else available), the executor's running and chosen next jobs, the
  pair memory and the settle set. A plot's single name combines to itself,
  so plots and columns share the combination as well as the classification.
  The combined reports of a loaded session are memoized per session as the
  column reports are today, for every source, and dropped by the same events.
- **A session that is not loaded** is a track of columns only. It is
  classified from the pair memory and the logbook's record set, in this
  order: no storable calculation, not applicable; a storable calculation
  remembered as failed (a job-level failure, a failed load, a write that
  failed), failed with the remembered reason; every storable calculation
  remembered not applicable, not applicable; every storable calculation with
  a record, done, or failed with the first recorded reason; a failed-load
  placeholder, failed ("The session file could not be loaded", remembered
  for the session's storable calculations as a failed load); otherwise
  waiting.

Candidates are filed by tier as the walk goes: the focused session's plot
pairs, the other visible sessions' plot pairs in row order, then the column
pairs of visible loaded sessions, then of hidden loaded sessions, each in
row order; within a session, source order then blocker order; a pair once,
in its first tier; never a pair that is remembered, running and not asked
to stop, or of a session inside its settle wait. The load candidates are the
waiting, hidden, unloaded, not settling sessions in row order up to the
bound, as today. Every state's counts and listed tracks are tallied in the
same walk. Priority, the settle wait and the bound are unchanged.

The walk reads the session model under one row stability guard and returns
plain values; offers, withdrawals, holds, loads and signals happen after it,
as today.

### 10.2 One memory

The demand layer keeps one memory of this run, keyed by pair (session id,
requested calculation instance id), holding a kind and a reason:

- **Not applicable**, not shown, from one of two origins. The executor
  refused the offer (a missing input, nothing to do, an unknown
  calculation): the pair is not offered again. Or the engine reported a
  loaded column's value not applicable for the session: every storable
  calculation of that column is remembered so **for that column only**, and
  the fact is read only when the session is not loaded and that column is
  classified, so that the session is not loaded again for it. It never
  makes another source's track not applicable and never keeps a pair from
  being offered, because the engine reports a name not applicable whenever
  any one input is missing, even when the requested calculation behind it
  can run (a column over a marker that the session lacks, beside a column
  over the same calculation's output). Plots remember no such verdict: they
  have no track that is not loaded.
- **Failed**, with a reason, from one of: a job that ended failed (the worker
  could not start, out of memory); a load that failed; a record write that
  failed (section 7); a result the engine holds that is never stored, a
  computation that threw (an input-determined failure for a loaded session,
  which would otherwise be run again after every eviction). Shown with the
  warning badge and the reason.

There is no separate per-cell memory of verdicts. A track's verdict is
always derived: for a loaded session from the engine and the memory, for one
that is not loaded from the memory and the record set. The cases the
per-cell memory covered are each covered by a pair fact: refusals and job
failures were pair facts already; a column's verdict, a failed load, a
failed write and an unstored result become pair facts; a done track without
a record is impossible once a failed write is a failure, except for a
source over explicit family instances alone, which has no storable
calculation and is not applicable when the session is not loaded, as today.
The last claim rests on the product's registrations giving every output of
a requested calculation one candidate, which a test of the registrations
checks; the documentation states the assumption.

The memory is cleared: for a session, by a relevant dependency change of
that session (a bulk edit is one, section 6), and its failed-load facts by
a load of that session that succeeds; for a pair, by a record change of
that pair; for everything, by a registry change; and a reset of the session
model forgets the pairs of sessions that no longer have a row. Nothing else
clears it: in particular the column worker's display changes, which the
demand layer no longer observes, so a session that was found not applicable
is not loaded again after an eviction. Nothing is persisted.

## 11. The demand layer in parts

The demand layer stays one component with one contract towards the views
and the executor. Internally it is divided into parts whose contracts can
each be stated alone, so that the component's description no longer has to
carry all of them at once:

- **The presentation values**: the track, the per-source state, the tooltip
  text and the counts, as plain values with no behaviour beyond building the
  text. The views depend on these and on the component's read interface
  only.
- **The fill**: the holds, the load candidates, the counters and the
  scheduler task. Its contract: given, after each pass, the set of sessions
  with a pending column cell and the load candidates in order, keep at most
  the bound of hidden sessions loaded and pinned for column demand, release
  a hold when its session has no pending cell or no loaded row, load the
  next candidate when a hold is free and the scheduler steps it, report
  progress as the sessions remaining of the high-water mark, and hold
  nothing once the executor is shut down or the component goes.
- **The settle clock**: the per-session deadlines of the input-settle wait,
  the earliest-deadline timer and the questions "is this session settling"
  and "when does the next wait end".
- **The reconciler**: the walk, the classification rules, the memory, the
  choice and the pass.

Whether these are files, classes or sections is the implementation's call;
what is critical is that the fill and the settle clock expose nothing of
the walk, and the walk calls neither the executor nor the session model's
loading and pinning.

## 12. What the user sees

- **One indicator.** A plot row and a logbook column header show the same
  thing: the turning arc while any of the source's demand is waiting or
  running, the warning badge once the work is finished and some sessions
  could not be computed, and nothing else. The plot row's "k of n" label
  beside the arc and the count beside the badge are removed; the hover
  carries both numbers, as it does for the header today. The row layout
  keeps room for one glyph and nothing more.
- **One clock.** The arcs of the plot list and the logbook header turn in
  step: one working-indicator clock per application, created beside the
  demand layer and handed to the views the way the demand layer is. It runs
  only while any plot or column is working and stops when nothing is; each
  view repaints its own working rows or sections on its frames, as today.
- **The fill's own progress text.** The logbook's progress line says
  "Computing results: k / n" while the fill is the active task, distinct
  from the column worker's "Computing columns: k / n", so the line no longer
  jumps between two totals under one label when the fill follows a column
  pass. Everything else about the line is unchanged: the fill is not
  cancellable, and saves, loads, bulk edits and column work still come
  first.
- **A stored result that could not be written** is listed among the
  sessions that could not be computed, with its reason, on the plot row and
  the column header (section 7).
- Nothing else the user sees changes: hover detail, pending cells, the
  warning badge's meaning, and the absence of any refresh or cancel control
  are as specified before.

The shared glyph painting also owns the glyph's text colour for a style
option and the glyph's side and spacing for a text line, so that the plot
row delegate and the header compute neither; the destroyed-demand-layer
repaint and the tooltip display are written once as well.

## 13. Architecture

The contracts of the demand-driven specification stand, with these
amendments:

- **The idle scheduler** completes a task that can wait once its work is
  gone (section 5). It still knows nothing of jobs or demand.
- **The logbook manager** announces a recorded reason that changed through
  its record-changed signal (section 6). **The result store** announces a
  record write that failed, with the reason (section 7). Both remain facts
  about what is on disk, learned where the record is open anyway.
- **The session model** is the one source of each column's dependency
  closure and requested calculations, and of a row's display name
  (section 8), and its bulk edit announces its edit as a dependency change
  (section 6).
- **The executor** loses its idle and queued signals and its two-job query;
  it keeps cancel for the jobs dock (section 9). Its lifecycle, its bound,
  its pinning, and the one-way flow are unchanged.
- **The demand layer** derives every source's state and every candidate in
  one walk, keeps one pair memory, and is divided into the parts of
  section 11. It remains widget-free, the only offerer, and observed by
  nothing.
- **The views** present the same state through one glyph and one clock, and
  decide nothing.

The flow stays one way: the demand layer chooses; the executor publishes;
the store writes the record or announces that it could not; the index notes
the outcome and announces it; the record change drops the cached column
values and the demand layer's memos; the demand layer sees the result, or
the failure, through what it always reads.

## 14. Tests

- The scheduler completes a task that can wait once its work is gone: a
  task with work it cannot step loses its work without a step; its
  completion is called once, not cancelled, after a final progress report,
  before the next active task is reported or the scheduler goes idle. A
  task without a step predicate that loses its work without a step is not
  completed, and the existing tasks complete at the same moments as before.
  The fill's progress line reports the fill from its first load to its last
  result without any step that loads nothing, and a fill that starts from
  nothing starts its own count.
- A reason recorded by a restore into the column worker's copy reaches the
  demand layer through the manager's record-changed signal alone: a stored
  rejection of a session that is not loaded is badged with its reason after
  the worker's pass.
- A bulk edit announces a dependency change for the attribute it changed,
  for a loaded session and for one that is not loaded alike. The demand
  layer observes no display change of the model: a bulk edit of a session
  that was found not applicable makes it applicable again through that
  dependency change, and the column worker's processing of a stub clears
  nothing.
- A record write that fails is a shown failure: the track is listed as
  failed with the reason while the session is loaded and after its
  eviction, the pair is not offered again in this run, a later successful
  write clears it, and a restart tries again. No session is loaded twice for
  it.
- The demand layer reads each column's closure and requested calculations
  from the session model, and a column change reaches it through the
  model's reset alone. The demand layer's tracks and the executor's job
  records name a session by the model's display name, for loaded rows,
  stubs and failed-load placeholders alike.
- Plots and columns are classified, tallied and filed as candidates by one
  walk: the acceptance items of the demand-driven specification on demand,
  priority, the settle wait, failures, not applicable, chains and unloaded
  sessions pass unchanged in what they assert. Counts, listed running and
  failed tracks, pending cells and tooltips are as before.
- One memory: every case the per-cell memory covered is covered by a pair
  fact. In particular, a session found not applicable, a job-level
  failure, an unstored result, a failed load and a failed write are not
  loaded or offered again after eviction, after a sort, or after the column
  worker processes the stub; a column's not-applicable verdict does not
  suppress another column over the same calculation, loaded or not; a
  session whose file loads later in the run loses its failed-load badge; a
  record change of the pair, an input change of the session and a registry
  change clear what is remembered.
- Removed generality: no product code reads the removed signals, queries
  and fields; the executor never holds more than the running and the chosen
  next job; the chosen next job is withdrawn when demand no longer wants it.
- The user interface: the plot row shows the arc or the badge and no label
  or count; the two views' clocks are one; the fill's progress text is its
  own; a stored result that could not be written is listed in the hover.
- The audit's demand and gestures groups are updated for the removed and
  moved names, and a rule forbids a second computation of a column's
  requested calculations outside the session model and the registry.

## 15. Documentation

`docs/CALCULATIONS.md` section 15 (the executor's signals and queries;
cancel kept for the jobs dock), section 16 (one walk, one memory, the parts
of the demand layer, the fill's completion, the failed write, the session
model as the source of column knowledge and display names) and the
scheduler's description wherever it says a task completes by stepping;
`docs/COMPUTED_PLOTS.md` sections 2, 7 and 8 (one glyph, no label or count,
the failed write among the failures, the fill's progress text);
`docs/DATA_SCHEMA.md` section 11 where it describes when the index learns a
reason and what it announces; `tests/README.md` and
`tests/acceptance_map.txt` (new items continue after the last item of the
demand-driven feature; amended items are restated "(as amended)").

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
  path. Subject `Refinements phase N: <phase name>`; body a short summary,
  then the session's attribution line. Rejected iterations are never
  committed.
- **Tag** each phase commit `plan/calculation-refinements/phase-N-done`; tags
  are never moved or deleted.
- **Fixes to a closed phase** are committed separately as
  `Refinements phase N fixup: <what>` once reviewed and accepted.
- **CI workflow edits** are committed separately as
  `Refinements phase N: CI (unverified)`.
- **Escalated phases** are not committed; their paths are listed in the final
  report, and phases that depend on them are not started.
