# One status bar for background work

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

## 1. Motivation

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

## 2. Principles

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

## 3. Scope

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
  than where their progress is shown.
- The pending cell ("…") of the logbook and its hover; the plot widget's
  reading of a merely uncomputed value (it does not warn "No data
  available" for it).
- The import progress dialog: a modal, user-initiated wait, not background
  work.
- The jobs dock (`PLANS/jobs-dock-clean.md`): not built by this change.
- Sorting or filtering the logbook by the row warning.

## 4. Terms

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

## 5. The activity area

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

## 6. Overlap, hover and cancel

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

## 7. The warning

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

## 8. The logbook row

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

## 9. What goes

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

## 10. What the demand layer presents instead

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

## 11. What the user sees

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

## 12. Architecture

The contracts of the earlier specifications stand, with these amendments:

- **The main window** owns the status bar and the component that fills it.
  That component is created like the dock features are, from the
  application context, and is not a dock. It reads the scheduler's
  active-task and progress signals, the demand layer's progress and
  failures (section 10), and it asks the scheduler to cancel. It decides
  nothing: what is shown follows from section 6's rule and the values it
  reads.
- **The scheduler** is unchanged. It still knows nothing of jobs or demand,
  and it still reports the fill as a task; the status bar is what maps the
  fill to the computations' item.
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

## 13. Tests

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

## 14. Documentation

`docs/COMPUTED_PLOTS.md` sections 2, 4, 7 and 8 (the status bar in place of
the row glyph and the header; the row warning; the fill under "Computing
results" in the status bar; what stays visible after a restart);
`docs/CALCULATIONS.md` section 16 wherever it describes the per-source
state, the one clock, the progress line and the fill's progress text;
`tests/README.md` (section 12's manual verification gains the status bar
and the row warning; a new appendix lists the acceptance items, numbered
from 701) and `tests/acceptance_map.txt` (new items continue from 701;
amended items are restated "(as amended)").

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
