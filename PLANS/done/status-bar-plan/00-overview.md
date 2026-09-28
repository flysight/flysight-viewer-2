# Implementation plan: One status bar for background work

## Feature Specification

## One status bar for background work

Date: 2026-09-27
Status: specification for planning. Not an implementation plan.
Baseline: branch `store-requested-calculations` at the head that implements
`PLANS/done/calculation-refinements.md` (its plan is archived in
`PLANS/done/calculation-refinements-plan/`; the committed code is
authoritative over it). Where this document and the earlier specifications
disagree, this one wins; everything it does not mention stays as they
specify.
Related: `docs/CALCULATIONS.md` (section 16), `docs/COMPUTED_PLOTS.md`,
`tests/README.md`, `tests/acceptance_map.txt`,
`tests/audit/cleanup_audit.cmake`, `PLANS/jobs-dock-clean.md` (a later
feature; the status bar may one day open it, and nothing here prevents that).

This document says what the change must do and the boundaries it must
respect. It avoids class layouts and function signatures unless a name is
part of the observable contract. The planning agent decides phasing; the
implementation agent decides code structure.

### 1. Motivation

Background work is shown in three places today, and each place shows a
projection of the same two facts.

- **Progress is global, but it is shown per source.** One executor runs one
  job at a time from one queue, and one scheduler steps one task at a time.
  Yet every plot row and every logbook column header over a requested
  calculation turns its own arc, so the twelve rows of the sensor fusion
  group turn together for one computation, and the logbook column headers
  turn beside a progress line under the logbook that already counts the
  sessions still to compute.
- **Failure is a fact about one recording and one calculation, but it is
  shown per source.** A recording that cannot be fused would badge every
  sensor fusion plot row and every column over that calculation, and the
  badge disappears with the source: hide the plot or disable the column and
  nothing says the recording still cannot be computed.
- **Two progress lines for one idea.** The logbook's progress line shows
  the logbook's own tasks and the fill; a computation started by a plot
  shows nowhere but the row. The user cannot tell why one kind of work has a
  bar under the logbook and another has an arc in a row: the split reflects
  which component runs the work, which is nothing the user needs to know.

This change gives background work one home, the main window's status bar,
and gives failure one home per recording, the logbook row, so that each
fact is shown once, where the user asks the question it answers: "is the
application still busy?" at the bottom of the window, and "why is this
value missing?" on the recording's row. It also removes the per-source
presentation state, the one-clock apparatus, and the logbook's own progress
line.

### 2. Principles

- The principles of the demand-driven specification and of the calculation
  refinements hold unchanged.
- **A fact is shown where its question is asked, and once.** Progress is
  one fact per application. A failure is one fact per recording and
  calculation. Neither is a property of a plot or a column.
- **The status bar shows what the application is doing.** It is a general
  place for background work, not for one kind of it: everything the
  scheduler and the executor do is shown there, and only there. What can be
  stopped is stopped from there.
- **The warning says what could not be done, for as long as that is true.**
  It does not depend on the run that discovered it, and it is not a log: a
  failure that is no longer current is not shown.
- **The views decide nothing.** The status bar and the logbook present
  values the demand layer and the scheduler already hold; the demand layer
  remains widget-free and observed by nothing that changes what it does.

### 3. Scope

In scope:

- An activity area in the main window's status bar that presents every
  scheduler task and the computations of the demand layer, with one rule
  for what is shown when work overlaps, the hover detail, and the cancel
  control for the tasks that can be cancelled (sections 5 and 6).
- A warning in the status bar for the recordings that could not be
  computed, standing alone once nothing is computing and persisting across
  restarts for as long as the failure is current (section 7).
- One warning per logbook row for a recording that could not be computed,
  with the detail in its hover (section 8).
- Removal of the per-source working indicator and warning badge on plot
  rows and column headers, of the working-indicator clock, of the logbook's
  progress line and cancel button, and of the fill's separate progress
  text (section 9).
- The reduced presentation surface of the demand layer (section 10).

Out of scope, unchanged:

- The demand layer's decisions: what is in demand, priority, the settle
  wait, the pair memory and when it clears, the fill's loads and holds, the
  absence of any refresh or cancel for computations.
- The scheduler's contract and the tasks the session model registers, other
  than where their progress is shown and one read-only query the scheduler
  gains (section 12).
- The pending cell ("…") of the logbook and its hover; the plot widget's
  reading of a merely uncomputed value (it does not warn "No data
  available" for it).
- The import progress dialog: a modal, user-initiated wait, not background
  work.
- The jobs dock (`PLANS/jobs-dock-clean.md`): not built by this change.
- Sorting or filtering the logbook by the row warning.

### 4. Terms

The terms of the demand-driven specification and the calculation
refinements apply. In addition:

- **Activity area**: the part of the status bar that presents work in
  progress: a label, a progress bar and, when the shown item can be
  cancelled, a cancel control.
- **Item**: one piece of work the activity area can present: a scheduler
  task (saving, loading, updating, computing columns) or the computations
  ("computing results"). The fill is not an item (section 5).
- **Shown item**: the item the activity area presents when more than one is
  in progress.
- **Current failure**: a pair the demand layer holds as failed for a
  session in demand: a stored rejection read from the record set or from
  the engine, or a failure remembered this run (a job that failed, a load
  that failed, a record write that failed, an unstored result).

### 5. The activity area

The main window has a status bar. It is always present, and empty when
nothing is in progress and nothing has failed: it never appears or
disappears, so the layout does not jump.

Its activity area presents the items in progress:

- **The scheduler's tasks**, as the logbook's progress line presents them
  today: the active task, its label ("Saving sessions", "Loading sessions",
  "Updating sessions", "Computing columns") and its progress as the
  scheduler reports it. The labels are the existing ones.
- **The computations**, as one item, "Computing results": the sessions
  with a track waiting or running in any source, plots and columns alike,
  out of the high-water mark since the count was last zero. The item exists
  while that count is above zero. Its hover names the recording being
  computed and the running job's latest progress text.

The fill is never an item. Its loads serve the computations, and its count
is contained in the computations' count; while the fill is the scheduler's
active task, the activity area presents the computations. The fill keeps
its scheduler registration and its scheduler contract (it is completed when
its work is gone), and stops reporting a progress of its own for display.

### 6. Overlap, hover and cancel

At most two items can be in progress at once today: the scheduler steps one
task at a time, and the executor runs one job at a time, on its own thread.
The rule does not depend on that number.

- **One rule.** The shown item is the scheduler's active task when it has
  one that is an item; otherwise the computations. A scheduler task is
  short, ranks above the computations in the scheduler's own priorities,
  and is usually something the user asked for; the computations continue
  underneath and return to the label when the task ends. A bulk edit reads
  as "Updating sessions" followed by "Computing results".
- **The hover** of the activity area lists every item in progress, the
  shown item first, each with its own label and count, and for the
  computations the recording being computed and its current step. Nothing
  in the hover is a control.
- **Cancel.** The cancel control is present exactly while the shown item is
  a scheduler task registered as cancellable, and cancels that task, as the
  logbook's button does today. The computations have no cancel, as decided
  before, so the control is absent while they are the shown item. Saving is
  not cancellable, as today.
- **Idle.** When no item is in progress the activity area is empty. The
  warning (section 7) is not part of the activity area and stays.

### 7. The warning

Beside the activity area, the status bar shows a warning while any
recording has a current failure: the warning glyph and a short text with
the number of recordings that could not be computed. The warning counts
recordings, not pairs: a recording with two failed calculations is one.

- **It stands alone.** Once nothing is computing, the status bar shows the
  warning and nothing else. It is also shown while computations continue
  (the per-source rule that hid failures behind the arc does not apply: the
  status bar has room for both), so a failure found early in a long fill is
  visible at once.
- **The hover** lists the recordings that could not be computed, in
  session-model row order, each with the calculation that failed and the
  reason; a failure that is tried again at the next start says so. The list
  shows at most ten recordings and then says how many more there are, as
  the per-source hover did.
- **It persists across restarts, because its facts do.** Nothing new is
  written. A stored rejection is a record in the logbook's `cache/` folder
  and is read at the next start without loading the recording, as soon as
  the source that wants it is restored (checked plots and enabled columns
  are restored at start-up today) and the column worker's pass has
  reported the record set; the warning shows it then. A failure that is not
  stored (out of memory, a file that could not be read, a result that could
  not be written) is tried again at the next start, by the existing rule,
  and shows again only if it fails again. So the warning reflects what
  currently cannot be computed for what is currently switched on, and a
  disk that has since been freed, or a file that has since been restored,
  clears it by the retry succeeding.
- **It clears as the pair memory clears**: a record change of the pair, an
  input change of the session, a registry change, the session losing its
  row, or the last source over the calculation being switched off. Nothing
  else clears it, and nothing dismisses it by hand: a failure that is
  current stays shown.

The warning is about what the user has switched on. A recording whose
stored rejection concerns a calculation no plot or column wants is not a
current failure and is not counted.

### 8. The logbook row

A recording that has a current failure shows one warning glyph on its row
in the logbook, at the left of the row's first cell, whatever column that
is: the logbook has no fixed column, and the glyph belongs to the row, not
to a column. Its hover lists the recording's failed calculations with their
reasons, in the form of the status bar's hover for that recording. A row
that is not loaded shows the glyph from the record set, without loading,
exactly as its column header was badged before.

The cells of the row over the failed calculation are blank, as a cell whose
value does not exist for the recording is blank today. A blank cell
explains nothing; the row's glyph does. The pending cell ("…") is unchanged:
it is per cell by nature, answers "will this fill in?" where the question is
asked, and duplicates nothing.

The glyph takes room in the first cell's text rectangle only when it is
shown, so a logbook without failures looks exactly as it does today.

### 9. What goes

- The working indicator and the warning badge on plot rows and on logbook
  column headers, with the room the plot row layout reserves for a glyph
  and the header's hover of counts and lists. A plot row over a requested
  calculation looks exactly as any other row. A column header looks exactly
  as any other header.
- The working-indicator clock: the one animation clock created beside the
  demand layer, its following of the demand layer, its place in the
  application context, and the views' repaint on its frames. The status
  bar's progress bar needs no clock of its own beyond what the toolkit's
  widget does.
- The shared glyph painting and its hover helper, except for whatever the
  status bar and the logbook row reuse of the warning glyph itself; the
  planner decides whether the painting is kept for that or the toolkit's
  standard warning icon is used. One drawing of the warning glyph exists,
  not two.
- The logbook's progress line, its cancel button, and the dock feature's
  wiring of the scheduler's signals to the logbook view; with them the
  fixed minimum size hint that kept the dock from resizing when the line
  appeared.
- The fill's separate progress text ("Computing results" under the
  logbook), and the fill's reporting of a progress total of its own for
  display.
- The demand layer's per-source presentation: the per-source state with its
  counts, lists and hover text, the queries that return it for a plot or a
  column, the per-source change signals, the lists of working plot and
  column ids. What replaces them is section 10.

The audit's demand group keeps the removed names out.

### 10. What the demand layer presents instead

The demand layer's presentation surface becomes three things, each a direct
projection of what it already holds after a pass:

- **Progress**: the number of sessions with a track waiting or running in
  any source, the high-water mark of that number since it was last zero,
  and, while a job runs, the recording's display name and the job's latest
  progress text. Announced when any of them changes; the progress text
  changes without a pass, as today.
- **Failures**: for each session with a current failure, in session-model
  row order, the session's display name and its failed pairs, each with the
  calculation's title, the reason, and whether it is tried again at the
  next start. Announced when the set or any entry changes. A view asks for
  the whole list (the status bar) or for one session (the logbook row).
- **Pending cells**: unchanged.

These are computed in the one walk, from the tracks it already classifies,
and from the pair memory; the walk gains no second pass and the memory no
second key. Counts that no view reads any more are not kept.

### 11. What the user sees

- **The status bar** is the one place background work is shown. Its label
  names the work and its count ("Saving sessions: 3 / 10", "Computing
  results: 5 / 12"), its bar shows the same, and its hover lists everything
  in progress. A cancel control appears only for work that can be stopped.
  When the application is idle the bar is empty.
- **The warning** in the status bar says how many recordings could not be
  computed, for as long as that is true, restart or not, and its hover says
  which and why. It stands alone when nothing is computing.
- **The logbook row** of a recording that could not be computed carries one
  warning glyph, whose hover says what failed and why. Its cells over the
  failed calculation are blank. A cell still to come shows "…", as today.
- **Plot rows and column headers** show nothing about computing. A track
  still to come is absent from the plot until it arrives, as a track without
  the sensor is absent, and the plot widget still does not warn about it.
- Nothing offers a refresh or a cancel for computations.

### 12. Architecture

The contracts of the earlier specifications stand, with these amendments:

- **The main window** owns the status bar and the component that fills it.
  That component is created like the dock features are, from the
  application context, and is not a dock. It reads the scheduler's
  active-task and progress signals, the demand layer's progress and
  failures (section 10), and it asks the scheduler to cancel. It decides
  nothing: what is shown follows from section 6's rule and the values it
  reads.
- **The scheduler** keeps its contract. It still knows nothing of jobs or
  demand, and it still reports the fill as a task; the status bar is what maps
  the fill to the computations' item. It gains one read-only query, whether
  any task has work, so that a test can wait for background work to end
  without inferring it from signals; the fill no longer reports progress, so
  signals alone no longer tell a resting fill from an idle scheduler.
- **The demand layer** stays widget-free, the only offerer, and observed by
  nothing that changes what it does. Its presentation surface is section
  10. The fill loses its display progress only.
- **The logbook view** presents the row glyph from the demand layer's
  per-session failures and the pending cell as today, and presents no task
  progress. The dock feature no longer connects the scheduler.
- **The plot list** presents nothing of the demand layer.
- **The application context** carries the demand layer for the logbook and
  the status bar, and no clock.

The flow stays one way: the demand layer chooses; the executor publishes;
the store writes; the index announces; the demand layer sees the result or
the failure through what it always reads, and presents progress and
failures as values; the status bar and the logbook read them.

### 13. Tests

- The activity area presents the scheduler's tasks with the existing labels
  and the scheduler's counts, and presents the computations as one item
  counting sessions with a waiting or running track across plots and
  columns, out of the high-water mark, which resets when the count reaches
  zero; the fill is never the shown item, and while it is the scheduler's
  active task the computations are shown.
- With a scheduler task and a computation in progress, the task is the
  shown item and the computations return when it ends; the hover lists both
  with their own counts and the recording being computed with its step.
- The cancel control is present exactly while the shown item is a
  cancellable scheduler task, and cancels that task; it is absent for
  saving and for the computations.
- The warning is shown while any session in demand has a current failure,
  counts recordings, lists them in row order with calculation and reason,
  says which are tried again at the next start, caps the list at ten, is
  shown alongside the computations while they continue, stands alone once
  they end, and is absent when nothing has failed.
- The warning persists through a restart for a stored rejection: with the
  source restored and the record set reported, the recording is counted
  without being loaded. A failure that is not stored is absent after a
  restart until it is tried again and fails again; a retry that succeeds
  leaves nothing shown.
- The warning clears when the pair memory clears or the last source over
  the calculation is switched off, and nothing dismisses it by hand.
- The logbook row of a recording with a current failure shows one glyph at
  the left of its first cell, for a loaded row and for one that is not
  loaded; its hover lists the failed calculations with reasons; the cells
  over the failed calculation are blank; a row without a failure takes no
  room for the glyph; the pending cell is unchanged.
- Removed presentation: no plot row or column header paints a glyph or
  reserves room for one; no clock exists; the logbook view has no progress
  line and no cancel button and receives no scheduler signal; no product
  code reads the removed per-source state, queries, signals or lists; the
  audit's demand group keeps their names out.
- The demand layer's progress and failures are computed in the one walk
  and announced only when they change; the acceptance items of the earlier
  specifications on demand, priority, the settle wait, failures, not
  applicable, chains, unloaded sessions and the pair memory pass unchanged
  in what they assert.

### 14. Documentation

`docs/COMPUTED_PLOTS.md` sections 2, 4, 7 and 8 (the status bar in place of
the row glyph and the header; the row warning; the fill under "Computing
results" in the status bar; what stays visible after a restart);
`docs/CALCULATIONS.md` section 16 wherever it describes the per-source
state, the one clock, the progress line and the fill's progress text;
`tests/README.md` (section 12's manual verification gains the status bar
and the row warning; a new appendix lists the acceptance items, numbered
from 701) and `tests/acceptance_map.txt` (new items continue from 701;
amended items are restated "(as amended)").

### Commit Policy

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
  path. Subject `Status bar phase N: <phase name>`; body a short summary,
  then the session's attribution line. Rejected iterations are never
  committed.
- **Tag** each phase commit `plan/status-bar/phase-N-done`; tags are never
  moved or deleted.
- **Fixes to a closed phase** are committed separately as
  `Status bar phase N fixup: <what>` once reviewed and accepted.
- **CI workflow edits** are committed separately as
  `Status bar phase N: CI (unverified)`.
- **Escalated phases** are not committed; their paths are listed in the final
  report, and phases that depend on them are not started.

## Phases

| Phase | Name | Purpose | Dependencies |
|-------|------|---------|--------------|
| 1 | Progress and failures in the demand layer | Spec §10 and the demand side of §5 and §7: the demand layer computes, in the one walk, the progress value (sessions waiting or running in any source, the high-water mark, the running recording and its step) and the failures value (per session in row order, its failed calculations with reason and retry flag), announces each only when it changes, gives the pending cells an announcement of their own, and provides the one text form of a recording's failures and of the capped list. Additive: the per-source state stays until phase 4. | none |
| 2 | The status bar | Spec §5, §6, §7 (presentation), §9 (the logbook's progress line, its cancel button, the dock feature's scheduler wiring, the minimum size hint, the fill's display progress), §12 (the main window and the scheduler). A new component fills the main window's status bar from the scheduler and the demand layer; the logbook stops presenting task progress; the fill registers no progress of its own. | 1 |
| 3 | The logbook row warning; the per-source indicators go | Spec §8 and the view half of §9: the logbook row's warning glyph and hover; the plot rows and column headers lose their glyph, hover and reserved room; the working-indicator clock, the shared glyph painting and its hover helper, and the application context's clock go. After this phase no product code reads the per-source state. | 2 |
| 4 | The per-source presentation goes | Spec §9 (last bullet) and §10 ("counts that no view reads any more are not kept"): the per-source state, its queries, change signals and working-id lists are removed from the demand layer; every test that read them is rewritten to assert the same facts through progress, failures, pending cells and the executor; the audit's demand group keeps the removed names out. | 3 |
| 5 | Documentation, acceptance map and README | Spec §14 and the traceability of §13: `docs/COMPUTED_PLOTS.md` §2, 4, 7, 8; `docs/CALCULATIONS.md` §16; `tests/README.md` (sections 9, 10, 12, the table rows, appendix H); `tests/acceptance_map.txt` items from 701 and the amended items; the audit's traceability range for the new items. | 4 |

The phases are strictly sequential:

```text
1 Progress and failures in the demand layer   (additive values, signals, text form)
        |
2 The status bar                              (activity area, warning; logbook progress line and fill display progress go)
        |
3 The logbook row warning; indicators go      (row glyph; plot row / header glyph, clock, shared painting go)
        |
4 The per-source presentation goes            (demand layer surface reduced; tests rewritten)
        |
5 Documentation, acceptance map and README
```

No two phases run in parallel. Phases 2 and 3 both edit
`tests/tst_logbook_indicators.cpp`, `tests/CMakeLists.txt`,
`tests/acceptance_map.txt` and `tests/audit/cleanup_audit.cmake`; phases 1, 3
and 4 all edit the demand layer or its tests; and every phase builds and runs
the whole suite in the one `build-agent/` tree, which `CLAUDE.md` and `CLAUDE.local.md`
require to be used by one build or ctest at a time. The order puts each new home in place
before the old presentation goes: progress has the status bar (phase 2) before
the arcs go (phase 3), and failures have the status bar and the logbook row
(phases 2 and 3) before the header and row badges go (phase 3), so no phase
leaves a fact shown nowhere. The per-source state goes last (phase 4), once
nothing in the product reads it.

## Key Patterns and References

Locate every passage by quoting it; line numbers go stale.

### Project rules and earlier work

- `CLAUDE.md` and `CLAUDE.local.md`: build and test in `build-agent/` only, sequentially, and never build `build/` or `build-deps/`; what must stay green; the audit's conventions.
- `PLANS/done/calculation-refinements.md`: the baseline specification (one walk, one pair memory, one indicator, one clock, the fill's progress text) that this one amends.
- `PLANS/done/calculation-refinements-plan/00-overview.md`: the previous plan of the same area; its "Decisions & Constraints" and "Integration Notes" explain today's shape of the demand layer and its views.
- `PLANS/done/demand-driven-requested-calculations.md`: the principles and the terms (demand, track, pair, pending cell) the specification builds on.
- `PLANS/jobs-dock-clean.md`: the later jobs dock; nothing here may prevent the status bar from opening it one day.

### Demand layer (phases 1 and 4)

- `src/calculationdemand.h`: the class comment is the demand layer's contract (WHAT IS WANTED, TRACK CONDITIONS, MEMORY, WHEN A PASS RUNS, PRESENTATION, PARTS); the public presentation surface (`plotState`, `columnState`, `isCellPending`, `workingPlotIds`, `workingColumnIds`, the three signals); the private `Walk` struct the one walk returns.
- `src/calculationdemand.cpp`: `walkRows()` (the one walk, which already classifies every track of every source), `classifyLoaded()`, `classifyUnloaded()` (rule 4 reads the record set and its reasons without loading), `failedTrack()`, `recompute()` and `applyStates()` (where states are compared and announced), `onJobProgress()` (the step text without a pass).
- `src/demandstate.h`, `src/demandstate.cpp`: the presentation values (`DemandCondition`, `DemandTrack`, `DemandState`), `kToolTipListLimit` and `buildToolTip()`: the pattern for widget-free presentation values and their `tr()` text. Phase 1 adds the new values here; phase 4 removes `DemandState`.
- `src/demandfill.h`, `src/demandfill.cpp`: the fill; its scheduler registration (`registerTask()`, the `progress` lambda returning `m_remaining, m_highWater`) and `update()`'s high-water bookkeeping, which phase 2 removes.
- `src/demandsettleclock.h`: the other part of the demand layer; unchanged, named by audit rules.
- `src/jobqueue.h`: the executor's `runningJob()`, `job()`, `jobProgress`, `jobStarted`, `jobFinished`: where the running recording and its step come from.
- `src/sessionmodel.h`: `sessionDisplayName()`, `rowAt()`, `getSessionRow()`, the `WorkerTask` enum (`SaveTask` .. `ColumnFillTask`), `scheduler()`.
- `src/logbookmanager.h`: `knownCalculationRecords()`, `calculationRecordReason()`: the record set a stored rejection is read from after a restart, without loading.

### Scheduler and status bar (phase 2)

- `src/idlescheduler.h`, `src/idlescheduler.cpp`: the scheduler's contract, `activeTaskChanged(id, cancellable)`, `progressChanged(id, remaining, total)`, `schedulerIdle()`, `cancel(id)`; `reportProgress()` skips a task registered without a progress function; `isTicking()`, the existing test seam beside which phase 2 adds the one read-only query `hasWork()`. The contract is otherwise unchanged.
- `src/sessionmodel.cpp`: the four task registrations (`m_scheduler.registerTask(SaveTask, {` ...) with their `cancellable` flags; saving is not cancellable.
- `src/ui/docks/logbook/LogbookView.h`, `.cpp`: today's progress line (`m_progressBar`, `m_cancelButton`, `onActiveTaskChanged`, `onProgressChanged` with the five labels, `onSchedulerIdle`, `minimumSizeHint`): the behaviour the status bar takes over and the code phase 2 removes.
- `src/ui/docks/logbook/LogbookDockFeature.cpp`: the scheduler wiring and the cancel connection (`IdleScheduler &sched = ctx.sessionModel->scheduler();`) that phase 2 removes.
- `src/mainwindow.cpp`, `src/mainwindow.h`, `src/mainwindow.ui`: where the executor, the demand layer and the clock are created (`m_calculationDemand = new CalculationDemand(`), the `AppContext` is filled and `DockRegistry::createAll` is called, and the destructor's order (demand layer, clock, executor). The window has no status bar today.
- `src/ui/docks/AppContext.h`, `src/ui/docks/DockRegistry.h`, `src/ui/docks/DockFeature.h`, `src/ui/docks/logbook/LogbookDockFeature.h`: how a feature is created from the application context (`explicit LogbookDockFeature(const AppContext& ctx, QObject* parent = nullptr);`).
- `src/CMakeLists.txt`: the application's source list (the `ui/docks/...` block).

### Views (phase 3)

- `src/ui/docks/logbook/LogbookCellDelegate.h`, `.cpp`: the pending cell, its tooltip, and the repaint on `columnStateChanged`; where the row glyph goes.
- `src/ui/docks/logbook/LogbookHeaderView.h`, `.cpp`: the column header's glyph, reserve, two-line elision and hover. Added for the indicator (commit d338a01).
- `src/ui/docks/plotselection/PlotRowDelegate.h`, `.cpp`, `PlotRowLayout.h`, `PlotSelectionDockFeature.cpp`: the plot row's glyph, its geometry and its installation; the delegate's contract says it presents the demand layer and nothing else.
- `src/ui/docks/DemandIndicator.h`, `.cpp`, `src/ui/docks/DemandIndicatorView.h`, `.cpp`: the glyphs, `glyphMetrics`, `WorkingAnimation`, `glyphColor`, `drawDemandGlyph`, `showIndicatorToolTip`, `repaintWhenDemandDestroyed`, `followDemand`.
- `src/ui/docks/logbook/LogbookView.cpp`: `setupView()` hands the demand layer and the clock to the header and the cell delegate.

### Tests

- `tests/README.md`: section 4 (running one test or function), section 8 (writing a test), the table of test executables (for example the `tst_calculation_demand` row), section 9.7 and appendix G (the form of a specification's traceability section and appendix), section 10 (the audit), section 12.5 (manual steps M29-M33, which describe the arcs, the badges and the progress line).
- `tests/CMakeLists.txt`: `flysight_add_test`, and the `FLYSIGHT_BUILD_WIDGET_TESTS` block that compiles view sources into `tst_plot_row_delegate` and `tst_logbook_indicators`: the pattern for a widget test of the status bar.
- `tests/tst_calculation_demand.cpp`: the demand layer's suite; the helpers `row()`, `col()`, `isCellPending()`, `settle()`, `spin()`, `restartDemand()`, `running()`, `chosenNext()`, `jobOf()`; about 150 reads of the per-source state that phase 4 rewrites; `columnStateCountsAndPendingCells` and the working-ids test ("is working, and statesChanged() says when they change").
- `tests/tst_logbook_indicators.cpp`: the view suite in a real `LogbookView` (headers, pending cells, the progress line in `fillProgressLineHasItsOwnText`, one clock, the hover of a failed load and a failed write, `survivesDemandDestroyedFirst`).
- `tests/tst_plot_row_delegate.cpp`, `tests/tst_plot_row_layout.cpp`: the plot row's glyph and geometry.
- `tests/tst_fusion_rows.cpp`, `tests/tst_fusion_store.cpp`, `tests/tst_result_columns.cpp`: further readers of `plotState` / `columnState` that phase 4 rewrites.
- `tests/support/plotfixture.h`, `.cpp`, `tests/support/jobfixture.h`, `.cpp`, `tests/support/testenvironment.h`, `.cpp`, `tests/support/storedresults.h`: the fixtures (synthetic plots, the gate-controlled job world, `waitDemandIdle`, stored records).
- `tests/tst_jobqueue.cpp`, `tests/tst_session_model_engine.cpp`: scheduler and executor tests; the scheduler keeps its contract, so they stay as they are, and phase 2 adds the test of `hasWork()` to `tst_session_model_engine` beside `schedulerCompletesWaitingTaskWhoseWorkIsGone`.

### Audit and traceability

- `tests/audit/cleanup_audit.cmake`: the `widget-free-core` group (it lists `DemandIndicator.*` and `PlotRowLayout.h`) and the `demand` group, in particular "only the application and its views know the demand layer", "nothing below the demand layer includes it", "the demand views handle no event of their own", "the load step is the demand layer's scheduler task" (allows `LogbookView.cpp`), "the views keep no label, no cluster and no clock logic of their own", the three "one working-indicator clock" rules, "the plot rows and the headers share the glyph plumbing", "the views learn of the demand layer's end in one place", "the fill has a progress text of its own", "the documents describe the refined demand layer"; and the traceability check with its item ranges.
- `tests/acceptance_map.txt`: its head (the ranges and line forms). About 55 lines cite `tst_logbook_indicators`, 47 cite the plot row tests and 326 cite `tst_calculation_demand`.

### Documentation (phase 5)

- `docs/COMPUTED_PLOTS.md`: §2 "What a plot row shows", §4 "Logbook columns over computed values", §7 "When a track cannot be computed", §8 "While computing".
- `docs/CALCULATIONS.md`: §16, in particular 16.2 "Plot and column state", 16.7, 16.8, 16.10 "The views and application wiring", 16.11, 16.12.

## Decisions and Constraints

1. **The warning glyph is the toolkit's standard warning icon**
   (`QStyle::SP_MessageBoxWarning`, from the widget's style), used by the
   status bar and the logbook row alike. Spec §9 leaves the choice open. The
   standard icon is the platform's own look in a status bar, reads on a
   selected row as any item icon does, and lets `DemandIndicator.*` and
   `DemandIndicatorView.*` go entirely: nothing of the custom painting keeps a
   user. Between phases 2 and 3 the header's badge still uses the custom
   painting; phase 3 removes it, so one drawing remains.
2. **The fill registers no progress function.** The scheduler already skips
   a task without one (`reportProgress()`), so "stops reporting a progress of
   its own for display" needs no scheduler change, and the fill's high-water
   mark goes with it. The fill's scheduler contract (work while a cell is
   pending, completed when its work is gone, not cancellable) is unchanged.
3. **The computations' count follows the scheduler's convention**: the bar and
   the label show done out of total, where total is the high-water mark and
   done is the high-water mark minus the sessions still waiting or running
   ("Computing results: 5 / 12" means five of twelve are done), as the fill's
   line reads today.
4. **Pending cells keep an announcement of their own.** Spec §9 removes the
   per-source change signals and spec §10 keeps the pending cells unchanged;
   today the cell delegate repaints on `columnStateChanged`. Phase 1 adds
   `pendingCellsChanged(columnId)`, emitted when that column's set of pending
   cells differs after a pass; phase 3 moves the delegate to it; phase 4
   removes `columnStateChanged`.
5. **The text form of failures is written once, beside the values**, in
   `src/demandstate.*`, widget-free and with `tr()`, as `buildToolTip()` is
   today. The status bar's hover (at most ten recordings, then how many more)
   is built from the same one-recording text the logbook row's hover shows, so
   spec §8's "in the form of the status bar's hover for that recording" holds
   by construction.
6. **The status bar's component** is `StatusBarFeature` in
   `src/ui/statusbar/StatusBarFeature.{h,cpp}`: a `QObject` created by the main
   window from the `AppContext` and the window's `QStatusBar`, after the dock
   features. It is not a `DockFeature`. It holds the demand layer weakly and
   shows no computation and no warning once the demand layer is gone
   (`~MainWindow` destroys the demand layer first).
7. **The status bar's height never changes.** Its widgets keep their size
   while hidden, or the bar has a fixed height; spec §5's "never appears or
   disappears, so the layout does not jump" is tested by comparing the bar's
   height idle, while working, and with the warning.
8. **Views that only presented the demand layer are deleted, not emptied.**
   If, after phase 3, `LogbookHeaderView` or `PlotRowDelegate` (with
   `PlotRowLayout.h`) paints nothing the base class would not, it is removed
   with its installation and its tests (`tst_plot_row_delegate`,
   `tst_plot_row_layout`); the phase 3 document settles each by reading the
   code. Map lines that cite a removed test are re-pointed in the same phase to
   the test that now proves the item, or to the audit rule that keeps the thing
   removed.
9. **Tests of earlier items keep what they assert.** Phase 4 rewrites each
   per-source read to the fact it stood for: a Waiting or Running track is a
   session counted in `progress()` (and, for Running, the executor's running
   job); a Failed track is an entry of `failures()`; Done and NotApplicable are
   neither; a pending column cell is `isCellPending()`. A test whose only
   subject was the per-source state itself (its counts, its tooltip, its
   signals, the working ids) is replaced by the equivalent test of progress,
   failures and their signals, or removed where spec §9 removes its subject.
10. **No test seam exposes per-source state.** Tests of the removed surface
    assert through the values of spec §10 and the executor, so the audit can
    keep the removed names out of `tests/` as well as `src/`.
11. **Documentation, the acceptance items from 701 and the README are phase
    5's**, as in the previous plan: `docs/CALCULATIONS.md` §16 describes the
    per-source state, the clock and the progress line in paragraphs that
    phases 1-4 each change in part, and it is rewritten once. Phases 1-4 keep
    the tree green meanwhile: each updates the audit rules and the map lines
    that name what it removes or renames, and phase 2 adds the new test
    executable's row to the table of `tests/README.md`.

12. **The scheduler gains one read-only query, `hasWork()`** (phase 2; spec
    §12 as amended). Without the fill's progress reports, signals no longer
    tell a resting fill from an idle scheduler, and `waitForIdle()` already
    works around the missing query with a sentinel timer. The query ends both
    workarounds: the helper waits until no task has work and no tick is due,
    and emits and registers nothing on the scheduler.

Constraints the code imposes:

- The scheduler has no query for its active task: the status bar learns it
  only from `activeTaskChanged` and `schedulerIdle`, and is connected before the
  event loop runs (in the main window's constructor), as the logbook view is
  today. Every scheduler signal is emitted from a tick.
- The scheduler reports the progress of the active task only, so the items in
  progress are at most the active task (unless it is the fill) and the
  computations.
- The demand layer stays widget-free (`widget-free-core` group): the new values
  and their text use QtCore only.
- `onJobProgress()` updates the step text without a pass; the progress value
  follows it the same way (spec §10).
- The walk runs under one `RowStabilityGuard` and the pass compares before it
  announces; the new values follow the same compare-then-announce pattern, and
  the audit's "one walk" count stays 1.
- Tests run in `build-agent/`, Release, sequentially; the widget tests need
  `FLYSIGHT_BUILD_WIDGET_TESTS` (ON by default) and run offscreen.

## Interfaces Between Phases

### Phase 1 provides (used by phases 2, 3 and 4)

In `src/demandstate.h` (namespace `FlySight`; plain values with `operator==`
and `operator!=`):

- `struct DemandProgress { int count = 0; int highWater = 0; QString sessionName; QString progressText; };`
  - `count`: sessions with a Waiting or Running track in any source, plot and
    column tracks alike, each session once.
  - `highWater`: the largest `count` since `count` was last 0; 0 when `count` is 0.
  - `sessionName`: the display name (`SessionModel::sessionDisplayName()`) of
    the recording of the executor's running job; empty when no job runs.
  - `progressText`: that job's latest progress text; empty when none.
- `struct FailedCalculation { QString calculationId; QString title; QString reason; bool retriedAtNextStart = false; };`
  - one current failure of one pair; `title` is the registry's title (the id
    when the title is empty); `reason` is never empty; `retriedAtNextStart` is
    true for a failure that is not stored (a failed job, load or write, an
    unstored result) and false for a stored rejection.
- `struct SessionFailures { QString sessionId; QString sessionName; QList<FailedCalculation> calculations; };`
  - `static constexpr int kListLimit = 10;` recordings listed at most by `listText()`.
  - `QString text() const;` one recording's hover: its failed calculations,
    each naming its title and reason once, and saying "tried again at the next
    start" where `retriedAtNextStart`.
  - `static QString listText(const QList<SessionFailures> &failures);` the
    status bar warning's hover: each recording's name and `text()`, in the
    given order, at most `kListLimit` recordings, then how many more there are.

On `CalculationDemand` (`src/calculationdemand.h`):

- `DemandProgress progress() const;`
- `QList<SessionFailures> failures() const;` every session with a current
  failure, in session-model row order; each session once, each pair once (a
  pair failed for a plot and for a column is one entry).
- `SessionFailures sessionFailures(const QString &sessionId) const;` that
  session's entry; one with no calculations when it has none.
- signal `void progressChanged();` emitted when `progress()` differs from what
  it was, after a pass or on a job's progress text.
- signal `void failuresChanged();` emitted when `failures()` differs.
- signal `void pendingCellsChanged(const QString &columnId);` emitted per
  column whose set of pending cells differs after a pass.

"Current failure" is spec §4's term: the Failed tracks the walk already
classifies, for the sessions in demand, per pair. Nothing is persisted.

### Phase 2 provides

- `src/ui/statusbar/StatusBarFeature.{h,cpp}`, class `FlySight::StatusBarFeature`,
  created in `MainWindow`'s constructor after the dock features, from the
  `AppContext` and `statusBar()`. Its test seams are its own; phase 5 cites
  its test functions by name from the tree.
- The labels "Saving sessions", "Loading sessions", "Updating sessions",
  "Computing columns" and "Computing results" live in the status bar only. The
  logbook view has no progress line, no cancel button, no scheduler slot and no
  `minimumSizeHint` override; `LogbookDockFeature` connects nothing of the
  scheduler; `DemandFill` registers no progress function.
- The audit's "the load step is the demand layer's scheduler task" rule allows
  the status bar's file instead of `LogbookView.cpp`, and "only the application
  and its views know the demand layer" allows `src/ui/statusbar/StatusBarFeature.(cpp|h)`.
- `bool IdleScheduler::hasWork() const`: true while some registered task's
  `hasWork()` is true, the fill included, whether or not it can step. It
  neither ticks nor wakes. The scheduler's signals and completion rule are
  unchanged.
- `FlySightTest::waitForIdle()` (`tests/support/testenvironment.*`) keeps its
  contract (true only when no scheduler task, the fill included, has work)
  by waiting until `!hasWork() && !isTicking()`. It registers nothing and
  adds no scheduler signal, so tests that spy on the scheduler across a wait
  see only what the tasks cause.

### Phase 3 provides

- `LogbookCellDelegate` paints the row glyph from `sessionFailures()` and
  repaints on `pendingCellsChanged` and `failuresChanged`; it no longer
  connects `columnStateChanged`.
- No product code calls `plotState`, `columnState`, `workingPlotIds` or
  `workingColumnIds`, or connects `plotStateChanged`, `columnStateChanged` or
  `statesChanged`. `AppContext` has no `workingClock`. `WorkingAnimation`,
  `followDemand`, `DemandIndicator.*` and `DemandIndicatorView.*` do not exist.

### Phase 4 provides

- `CalculationDemand`'s presentation surface is exactly `progress()`,
  `failures()`, `sessionFailures()`, `isCellPending()` (both overloads),
  `isMerelyUncomputed()`, the signals `progressChanged`, `failuresChanged` and
  `pendingCellsChanged`, and the existing test seams. `DemandState`,
  `DemandTrack` and `DemandCondition` do not exist; the walk's classification
  is the private nested `CalculationDemand::TrackCondition`, and
  `src/demandstate.h` holds phase 1's three values and their text only.
- The audit's demand group names the removed surface so that it stays out of
  `src/` and `tests/`.

### Phase 5 consumes

- The test functions as the tree holds them after phase 4 (cited by name in
  the map and the README), and the audit groups as phases 2-4 leave them.

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
  path. Subject `Status bar phase N: <phase name>`; body a short summary,
  then the session's attribution line. Rejected iterations are never
  committed.
- **Tag** each phase commit `plan/status-bar/phase-N-done`; tags are never
  moved or deleted.
- **Fixes to a closed phase** are committed separately as
  `Status bar phase N fixup: <what>` once reviewed and accepted.
- **CI workflow edits** are committed separately as
  `Status bar phase N: CI (unverified)`.
- **Escalated phases** are not committed; their paths are listed in the final
  report, and phases that depend on them are not started.
