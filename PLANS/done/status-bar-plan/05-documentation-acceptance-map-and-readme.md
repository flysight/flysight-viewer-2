# Phase 5: Documentation, acceptance map and README

## Purpose

Spec §14 and the traceability of spec §13 (overview decision 11). Phases 1-4
changed the product and kept the tree green; this phase makes every document,
test catalogue and traceability record say what the tree now does:

- `docs/COMPUTED_PLOTS.md` and `docs/CALCULATIONS.md` §16 describe the status
  bar, the row warning, the demand layer's progress, failures and pending
  cells, and what stays visible after a restart, and no longer describe the
  per-source indicators, the clock, the logbook's progress line or the fill's
  progress text;
- the new acceptance items 701-754 are stated (map, section 9.8, appendix H)
  and every earlier item whose meaning changed is restated "(as amended)";
- the audit's traceability check covers 701-754, and its document rule keeps
  the removed surface out of `docs/` and `README.md`.

**No code changes in this phase.** The files touched are `docs/*.md`, the
root `README.md`, `tests/README.md`, `tests/acceptance_map.txt` and
`tests/audit/cleanup_audit.cmake` (its comments, the document rule's pattern
and the traceability check). No file under `src/`, no `tests/*.cpp`, no
support file and no `CMakeLists.txt` changes. A stale comment found in `src/`
or `tests/` is reported to the orchestrator as a fixup of the phase that left
it, not fixed here.

## Dependencies

- **Depends on phase 4** (tag `plan/status-bar/phase-4-done`), and through it
  on phases 1-3. **Blocks nothing.**
- **The tree is the authority for names.** The phase documents only suggest
  test function names; the tables below use those suggestions. Before writing
  a map line, list the functions the tree holds
  (`grep -n "void [A-Za-z_]*()" tests/tst_status_bar.cpp tests/tst_logbook_indicators.cpp`,
  and the `private slots:` of `tests/tst_calculation_demand.cpp`) and cite the
  function that asserts the fact. If no function asserts a fact a table row
  names, report it; never drop a line to make the audit pass.
- **May assume**, as phases 1-4 state them: `StatusBarFeature`
  (`src/ui/statusbar/`), `tst_status_bar`, the row warning in
  `LogbookCellDelegate`, `DemandProgress` / `FailedCalculation` /
  `SessionFailures` with `text()` and `listText()`, the three signals
  `progressChanged`, `failuresChanged`, `pendingCellsChanged`, the private
  `CalculationDemand::TrackCondition`; no `DemandIndicator*`,
  `LogbookHeaderView`, `PlotRowDelegate`, `PlotRowLayout.h`,
  `tst_plot_row_delegate`, `tst_plot_row_layout`, `WorkingAnimation`,
  `workingClock`, per-source state, progress line or fill progress function.
  The map lines phases 2-4 re-pointed are in place; the audit groups are as
  they left them.
- **Texts are read from the tree, not from the phase documents**: the status
  bar's labels, count form, hover form and warning text in
  `StatusBarFeature.cpp`; the failure text form in `src/demandstate.cpp`.
- The specification is cited by its title, "One status bar for background
  work", not by path: `PLANS/` is tracked, but the specification moves to
  `PLANS/done/` when the work is finished, and a path would go stale.

## What changes

### `docs/COMPUTED_PLOTS.md` (the user's guide)

Write for the user, in the voice of the existing sections; no class names.

- **§2** becomes the status bar (retitle it, e.g. "What the status bar shows",
  and update the table of contents' anchor). The bar at the bottom of the
  window: empty when nothing runs; "Computing results: k / n" with its bar
  while recordings are computed (k done of n, counting each recording once,
  plots and columns together); the other background work by its existing
  labels; its hover lists everything in progress and the recording being
  computed with its current step; a cancel button only for work that can be
  stopped, never for computing. Plot rows show nothing about computing: a
  track still to come is absent from the plot until it arrives. Keep the
  facts that one computation fills roll, pitch and yaw, and that a track
  without the sensor is never counted.
- **§3**: "the numbers in the row's tooltip rise" becomes the status bar's
  count advancing.
- **§4**: the fill is shown as "Computing results: k / n" in the status bar,
  with no cancel button, and other background work shows first; the paragraph
  on the header's arc and hover goes (a column header looks as any other);
  "counted in the numbers of the header's tooltip" becomes counted in the
  status bar's "Computing results"; a recording that could not be computed
  shows the warning at the left of its row (§7). Pending cells unchanged.
- **§7**: as soon as a failure is found (not once the work is done), the
  status bar shows the warning with the number of recordings that could not
  be computed, and the recording's logbook row shows the warning glyph at the
  left of its first cell; hovering either names the calculation and the
  reason, and says which are tried again at the next start; the cells over it
  are blank. **What stays visible after a restart**: a stored rejection shows
  again at the next start without loading the recording, once the plot or
  column that wants it is restored and the background column pass has run; a
  failure not about the data is tried again at the next start and shows only
  if it fails again. It clears when its cause does (data changed, the plot or
  column switched off) and nothing dismisses it. The paragraph on a result
  that could not be kept names the status bar and the row, not "the plot row
  and the column header".
- **§8**: the arcs sentence goes; the status bar shows one thing at a time:
  other background work (saving, loading, updating after a bulk edit, filling
  a cheap column) first, with its own count and, where it can be stopped, a
  cancel button; "Computing results" returns when it ends; the hover lists
  both.
- **§9**: "The plot rows, the column headers and their tooltips are where
  progress and failures are reported" becomes the status bar (progress and
  the warning) and the logbook rows (failures); add that the logbook cannot be
  sorted or filtered by the row warning (spec §3, out of scope).

### `docs/CALCULATIONS.md` §16 and the scheduler passages

Rewrite in place; §16 keeps its subsection numbers (other documents and
comments cite them). Prose names components ("the status bar", "the demand
layer"); code spans name classes.

- **16.1**: the conditions are the walk's own `TrackCondition`, private to
  the reconciler, and feed progress (Waiting, Running), failures (Failed) and
  pending cells (Waiting, Running of a column); the failure-reason bullet
  becomes the entry's reason (the why alone, the title separate, the default
  details "Calculation failed" / "No result for this recording"), with no
  `"; "` join.
- **16.2** (retitle, e.g. "Progress, failures and pending cells"): the three
  values of spec §10 with their fields and the queries `progress()`,
  `failures()`, `sessionFailures()`, `isCellPending()`; the high-water mark;
  the running job not asked to stop; the retry flag; the text form of
  `SessionFailures::text()` and `listText()` (one line per calculation,
  "(tried again at the next start)", the name line and two-space indent, ten
  recordings then "and N more"; phase 1 left its documentation here); the
  three signals, each emitted only when its value differs, all values stored
  before any is announced; the progress text changes without a pass. Keep the
  display-name paragraph (15.6 cites 16.2 for it) and the "one event-loop pass
  behind" sentence.
- **16.3**: "What a pass costs" returns each track's contribution to progress,
  failures and pending cells, not states, counts and listed tracks; the
  executor row of "When a pass runs": `jobProgress` updates the progress text
  only.
- **16.4**, **16.5**: "the indicator shows it immediately" and "the indicator
  shows from the first change" become progress counting the session; "the
  states stay working until the demand layer is destroyed" becomes progress
  still counting them.
- **16.7**: "shown with the warning badge", "badged (`jobFailure`)", "the plot
  row and the column header list the session", "reaches the plot row and the
  column header", "the column's failure list and badge" become entries of the
  failures value, with `retriedAtNextStart`, presented by the status bar and
  the logbook row.
- **16.8**: the fill registers no progress function and keeps no high-water
  mark; the scheduler completes it as before; while it is the active task the
  status bar shows the computations. "The logbook's progress line shows
  "Computing results" ..." and the "No cancel" bullet's "progress line" become
  the status bar. In the scheduler's paragraph note that a task registered
  without a progress function reports none, and describe the one read-only
  query `hasWork()` (true while some task has work, whether or not it can
  step; it neither ticks nor wakes) that tests use to wait for background
  work to end; the rest of the completion rule is unchanged. "Known costs": "counted in the column's counts (its header's
  tooltip) and in the fill's progress" becomes counted in progress.
- **16.9**: the table loses `DemandState::kToolTipListLimit` / `buildToolTip`,
  `plotState`/`columnState`, `workingPlotIds`/`workingColumnIds`, the three
  per-source signals and the `DemandCondition`/`DemandTrack`/`DemandState` row;
  it gains the three queries, the three signals, `DemandProgress`,
  `FailedCalculation`, `SessionFailures` (`kListLimit`, `text()`,
  `listText()`), and the scheduler's `activeTaskChanged`, `progressChanged`,
  `schedulerIdle`, `cancel()` as read by the status bar, and its `hasWork()`
  as read by `waitForIdle()`. Inert means the
  default progress, no failures and no pending cell.
- **16.10**: the plot rows, shared glyphs, clock and header paragraphs go
  (the plot list and the header present nothing of the demand layer). New
  paragraphs: **the status bar** (`StatusBarFeature`, created by the main
  window after the dock features and before the event loop, not a dock; the
  items, the one rule, the count form, the hover form, the cancel button, the
  warning with the style's standard warning icon, its text and its tooltip,
  the constant height, the demand layer held weakly); **the row warning** (the
  first visual cell, the decoration, the hover, the repaint on
  `failuresChanged` and `pendingCellsChanged`, no row-height change). Pending
  cells: repaint on `pendingCellsChanged`, and the delegate's own `destroyed`
  connection. Ownership: no clock; the destructor deletes the demand layer,
  then the executor; the cell delegate and the status bar hold the demand
  layer weakly. "No message box, status message, or progress dialog reports a
  calculation outcome" becomes: no dialog, message box or temporary status
  message; the status bar's warning is where failures are counted. The "no
  data" paragraph: `Blocked` and `NotProduced` are shown by the status bar and
  the logbook row, not "by the plot list". Tests: `tst_status_bar`,
  `tst_logbook_indicators` (the only tests that link Qt Widgets) and the
  manual steps actually current (not M23, M29, M30).
- **16.11**: the demand layer publishes progress, failures and pending cells;
  add **the status bar** (presents the scheduler's active task and the demand
  layer's progress and failures by one rule, cancels a cancellable task,
  decides nothing); **the views** (the status bar and the logbook's cells and
  rows) present values and never run a pass, offer or write; the flow ends
  "presents progress and failures as values; the status bar and the logbook
  read them" (spec §12).
- **16.12**: `src/demandstate.h` holds the three values and their text; the
  fill reports no progress; the walk returns contributions, candidates and
  learned facts.
- **§17**: the tests paragraph's "the demand layer and the plot rows of
  section 16" drops the plot rows.

### Other documents

- **`docs/SENSOR_FUSION.md`**: "the plot row shows a warning badge with that
  reason" (§6), "the row shows the warning badge" and "shown with the warning
  badge" (lifecycle outcomes), "A stored rejection shows the warning badge
  again", and the `tst_fusion_store` row's "rejection and solver-failure
  badges" become the recording listed among those that could not be computed
  (the status bar's warning and the logbook row), with its reason.
- **Root `README.md`**: the `COMPUTED_PLOTS.md` line of the tree ("working
  indicator, badge, pending cells") and of "User Documentation" ("the working
  indicator and its tooltip, the warning badge"); the `ui/` comment names
  `logbook/LogbookCellDelegate.*` (pending cells and the row warning) and
  `ui/statusbar/StatusBarFeature.*` (the status bar), and no removed file.
  Keep the tree's column alignment.
- `DATA_SCHEMA.md` and `LOCAL_COORDINATES.md` say nothing false (checked:
  "listed as could not be computed" stays true); leave them.

### `tests/acceptance_map.txt`

- **Head**: "Eight specifications, eight item ranges"; entries 101-120,
  201-247, 301-350, 501-563 and 601-662 each gain "It is amended by the
  specification of 701-754: items ... are stated as amended."; the new entry

  ```text
  #   701-754   one status bar for background work (the activity area and its
  #             one rule, the warning and what persists across restarts, the
  #             logbook row warning, the per-source presentation removed, the
  #             demand layer's progress, failures and pending cells): item =
  #             700 + clause number. Stated in full in tests/README.md,
  #             appendix H. It amends the specifications of 101-120, 201-247,
  #             301-350, 501-563 and 601-662.
  ```

  and the ranges sentence and "sections 9.1 to 9.8".
- **New range** at the end after `# ---- One status bar for background work:
  item = 700 + clause number ----`: one block per item of the table under
  Tests, comment line `# <item> - (<section>) <clause>` (or `(<section>, as
  settled)`), then its lines.
- **Amended items**: rewrite the comment line in place as `(<section>, as
  amended)` (1xx and 2xx items: `(as amended)`, their existing form), keep
  every line that still resolves, add the lines named below.
- **Superseded manual steps**: every line citing `manual M23`, `M29` or `M30`
  is re-pointed to the 12.6 step that now proves the item (M34 for 535, 536,
  644, 645, 646; M36 for 537).

### `tests/README.md`

- **Section 1**: finish the rows phases 2-4 left: the `tst_calculation_demand`
  row ("the indicator from the first change", "per-column state", "its
  progress for the whole fill" and any other phrase naming the removed
  surface); every row's item list gains its 7xx items as the map has them
  (`tst_calculation_demand`, `tst_status_bar`, `tst_logbook_indicators`,
  `tst_session_model_engine`, `tst_fusion_rows`); the `audit_cleanup` row adds
  that background work is shown in one place.
- **Section 9**: the intro says eight. The intros of 9.2, 9.3, 9.4, 9.6 and
  9.7 name the clauses restated as amended by "One status bar for background
  work" (9.8). Each amended row: Section cell "…, as amended", Clause cell the
  restated clause, Evidence the item's lines. Every Evidence cell of 9.2-9.7
  whose map lines phases 2-5 changed is rewritten to match the map: no cell
  names a removed executable or function.
- **New 9.8 "One status bar for background work (items 701-754)"**, after
  9.7, in 9.7's form: the fifty-four clauses, stated in full in appendix H;
  item = 700 + clause number; the same four line forms; its sections 1-4
  (motivation, principles, scope, terms) have no item, their principles being
  carried by the clauses of the sections that apply them; clauses 45-53 are
  its §13 tests, one per bullet, clause 54 its §14; clauses 3, 25 and 31 are
  stated as settled (Decisions); the list of items it amends. Then the table
  `| # | Section | Clause | Evidence |`, 54 rows.
- **Section 10**: rewrite the `demand` bullet from the group's rules as phases
  2-4 left them, one clause per rule (the status bar's labels, scheduler
  signals and creation in one place each; the logbook presents no task
  progress; the fill reports no progress; one drawing of the warning glyph;
  the plot list presents nothing of the demand layer; the per-source
  presentation, the indicators and their plumbing, and any view's label,
  cluster or clock stay gone; each view that holds the demand layer learns of
  its end itself; the documents name none of it), with the group's item list;
  the `widget-free-core` bullet loses `PlotRowLayout.h` and the shared glyphs;
  the `branch-mechanisms` bullet's "status-bar message for a calculation
  outcome" becomes what the rule checks (no dialog or message box, and no
  status bar used by the kernel, the calculations or the plot list); the
  traceability bullet gains 701-754.
- **Section 12**: "Six scripts." Restate in place, keeping each bold id and
  item list, the steps that observe a removed surface:

  | Step | Restated as |
  |---|---|
  | M1, M3, M6, M8, M16-M18, M20, M22, M24, M26 | the arc, its tooltip counts, "the row is plain" and the progress line become the status bar's "Computing results: k / n", its hover and its going empty; the row looks as any other |
  | M2 | "no column header shows a working indicator" and "the progress line may show" become the status bar |
  | M4 | "click ... the arc of a working row" becomes a click on the status bar while computing: nothing happens, no cancel button |
  | M7, M28, M32, M33 | the badge, the triangle and the header's tooltip become the status bar's warning, its hover, and the row's warning glyph and hover |
  | M25 | the last sentences (the arc beside the elided name, the section fitted to the arc) become: the header looks and resizes as any other |
  | M31 | the fill's "Computing results: k / n" and the cheap column's "Computing columns: k / n" in the status bar, with its cancel button for the latter only |
  | M23, M29, M30 | superseded: keep the bold id and title, the body one sentence naming the step that replaces it (M34, M36); no map line cites them after this phase |

  New **12.6 One status bar for background work**, after 12.5, with 12.5's
  preamble form, M34 onwards (ids continue; each opens `**M<k> <title>
  (<items>).**`):

  - **M34 The status bar (535, 536, 644-646, 701, 703, 723, 733, 735, 736,
    738)**: at the bottom from
    the start, empty; check Roll over two fusable tracks: "Computing results:
    0 / 2", its bar, its hover with the recording and step, no cancel button;
    the plot row and a fusion column's header look as any other; the layout
    does not jump when work starts or ends; light and dark theme.
  - **M35 Overlap and cancel (702, 707, 709)**: during a column fill run a
    bulk edit: "Updating sessions: k / n" with a cancel button, hover lists
    both; cancel it; "Computing results" returns; saving shows no cancel
    button.
  - **M36 The warning and the row (537, 711, 712, 718, 735)**: a rejected recording
    during a fill: the warning at once beside "Computing results", alone once
    the fill ends; its text and hover; the row's glyph at the left of the
    first cell, its hover, the blank cell; move and hide the first column: the
    glyph follows; row heights unchanged; a selected row.
  - **M37 After a restart (714, 715, 734)**: with the column enabled, restart:
    once the background column pass has run the warning counts the stored
    rejection without loading it or fitting; the failed write of M32 says
    "(tried again at the next start)", is absent after a restart until it
    fails again, and gone once the retry succeeds.
  - **M38 Clearing (716, 717)**: disable the column: the warning goes; enable
    it: it is back with no fit; clicking the warning changes nothing;
    re-importing changed data clears it until the new fit fails.

- **Appendices B, C, D, F, G**: each amended clause restated in place ("N.
  (<section>, as amended) ..."), and each intro names the clauses amended by
  "One status bar for background work" (appendix H).
- **New appendix H** at the end, `## Appendix H. The acceptance items of one
  status bar for background work (701-754)`: the intro of 9.8's rule, then
  "1. (5) ..." to "54. (14) ...", each the clause of the map's comment line,
  capitalized, ending with a full stop.

### `tests/audit/cleanup_audit.cmake`

- **File head**: a bullet after the 601-662 one: background work is shown in
  one place and a failure once per recording (the status bar presents the
  scheduler's tasks and the demand layer's progress and failures, the logbook
  row a recording's failures, one drawing of the warning glyph); the
  per-source presentation, the indicators, the clock and the logbook's
  progress line stay gone, in code and documents (items 701-754).
- **Group headers**: the `demand` header line lists its items including every
  7xx item that cites `audit demand`; `gestures` and `branch-mechanisms` add
  the 7xx items that cite them. If phases 2-4 placed their rules without a
  sub-block comment, add `# ─── one status bar for background work (items
  701-754)` above them where they are contiguous; do not reorder rules
  otherwise.
- **`branch-mechanisms`**: the comment "A calculation outcome is reported by
  the plot row (badge and tooltip), never by a dialog, a message box or the
  status bar" becomes: reported by the status bar's warning and the logbook
  row, which present the demand layer; never by a dialog or a message box,
  and the kernel, the calculations and the plot list never use the status
  bar. The rule's pattern and pathspec are unchanged.
- **The document rule** "the documents describe the refined demand layer"
  (keep the label; `docs README.md`): add to its pattern the removed surface
  and presentation, e.g.
  `plotState|columnState|workingPlotIds|workingColumnIds|statesChanged|DemandState|DemandTrack|DemandCondition|buildToolTip|kToolTipListLimit|jobFailure|showsWarning|isPlain|isWorking\\(|WorkingAnimation|followDemand|workingClock|[Ww]orking[- ]indicator|DemandIndicator|PlotRowDelegate|PlotRowLayout|LogbookHeaderView|glyphMetrics|drawDemandGlyph|showIndicatorToolTip|repaintWhenDemandDestroyed|progress line|[Ww]arning badge|badges|turning arc`.
  Not `badge` alone: `DATA_SCHEMA.md` says "no badge" of legacy files. Update
  its Allow comment: say "the status bar", "the row warning", "progress",
  "failures"; never name the removed surface.
- **Traceability**: the comment names 701-754 (item = 700 + clause number);
  the range test gains `OR (item GREATER_EQUAL 701 AND item LESS_EQUAL 754)`
  and its message the range; add `foreach(item RANGE 701 754)` requiring a
  test or audit line, as for 601-662.
- **No new rule**: the rule count reported by the audit equals the count
  after phase 4.

## Interfaces

- **Consumes**: the test functions as the tree holds them after phase 4; the
  audit groups and rules as phases 2-4 left them; the status bar's texts
  (`StatusBarFeature.cpp`) and the failure text form (`src/demandstate.cpp`).
- **Provides**: nothing to a later phase. The numbering 701-754 and the
  clause texts become the traceability record of the specification.

## Acceptance criteria

1. **Audit** (§13, §14). `cmake -DREPO=. -P tests/audit/cleanup_audit.cmake`
   passes with the same rule count as after phase 4 (report both numbers).
2. **Every new item traced** (§13). Items 701-754 each have at least one
   test or audit line; every cited function exists as `::<function>()` in
   `tests/<target>.cpp`; every manual id cited exists as `**M<k> ` in
   `tests/README.md`. Planting a line `755 audit demand` reports an item
   outside the ranges; removing every line of one 7xx item reports that item;
   both plants are undone.
3. **Amended items** (§14). The 42 items of the amended table read "(…, as
   amended)" in the map, in their 9.x row and in their appendix, each
   restatement true of the tree and of its lines; no map line cites M23, M29
   or M30.
4. **Documents** (§14). `docs/COMPUTED_PLOTS.md` §2, §4, §7, §8 (and §3, §9)
   and `docs/CALCULATIONS.md` 16.1-16.5, 16.7-16.12 and §17's tests sentence
   are updated as above; §7 states what stays visible after a restart. Each
   of these greps is empty:
   `git grep -n -E "progress line|[Ww]orking[- ]indicator|[Ww]arning badge|turning arc|header's tooltip|plotState|columnState|DemandState|WorkingAnimation|workingClock|DemandIndicator|PlotRow(Delegate|Layout)|LogbookHeaderView" -- docs README.md`.
5. **The document rule proves itself** (§9: "keeps the removed names out").
   Planting `plotState(` in `docs/CALCULATIONS.md`, then "the logbook's
   progress line" in the root `README.md`, each trips that rule and only it;
   both plants are undone.
6. **`tests/README.md`**. Sections 9.8 (54 rows) and appendix H (54 clauses)
   match the map's comment lines in substance; `**M34 ` to `**M38 ` exist once
   each; outside section 10 the file names none of `tst_plot_row_delegate`,
   `tst_plot_row_layout`, `fillProgressLineHasItsOwnText`, `tooltipText`,
   `workingIdsFollowStates`, `toolTipListsAtMostTenFailures`,
   `progressUpdatesWithoutInspection`, `columnStateCountsAndPendingCells`,
   `fillTaskReportsProgressWhileWaiting` or the renamed
   `tst_logbook_indicators` functions (phase 3's table).
7. **Green.** `cmake --build build-agent --config Release` (nothing to
   rebuild) and the whole suite, `ctest --test-dir build-agent/FlySightViewer-build
   -C Release --output-on-failure`, sequentially, pass.
8. **No code change.** `git status` shows changes only in the five kinds of
   file named under Purpose.

## Tests

No test is added or changed. The audit's traceability check proves that
every line resolves. The items, in the specification's order (section 5
onward: each testable statement of §5-§12, each bullet of §13, and §14). The
map lines are the phase documents' suggested names; take the tree's.
Abbreviations in the lines column: `sb` = `tst_status_bar`, `cd` =
`tst_calculation_demand`, `li` = `tst_logbook_indicators`, `sme` =
`tst_session_model_engine`; `a:` = `audit`; `M` = `manual`.

| Item | § | Clause | Lines |
|---|---|---|---|
| 701 | 5 | the main window has a status bar, always present, empty when nothing is in progress and nothing has failed; it never appears or disappears, so the layout does not jump | sb heightNeverChanges, schedulerTasksShowTheirLabelsAndCounts, warningAbsentWhenNothingFailedAndNotDismissable; a: demand; M34 |
| 702 | 5 | the activity area presents the scheduler's active task with its existing label ("Saving sessions", "Loading sessions", "Updating sessions", "Computing columns") and its progress as the scheduler reports it | sb schedulerTasksShowTheirLabelsAndCounts; a: demand; M35 |
| 703 | 5, as settled | the computations are one item, "Computing results": the sessions with a track waiting or running in any source, plots and columns alike, each once, shown done out of the high-water mark since the count was last zero; the item exists while the count is above zero | sb computationsAreOneItem; cd progressCountsEachSessionOnce; M34 |
| 704 | 5 | the computations' hover names the recording being computed and the running job's latest progress text | sb taskShownOverComputationsAndHoverListsBoth; cd progressCountsEachSessionOnce, progressTextWithoutAPass |
| 705 | 5 | the fill is never an item: its count is contained in the computations', and while it is the scheduler's active task the computations are shown | sb computationsAreOneItem; cd fillTaskIsLowestAndNotCancellable |
| 706 | 5 | the fill keeps its scheduler registration and contract (completed when its work is gone, not cancellable) and reports no progress of its own | cd fillTaskRestsWhileWaiting, fillEndingBehindAnotherTaskStartsNextCountFresh, fillTaskIsLowestAndNotCancellable; sme schedulerCompletesWaitingTaskWhoseWorkIsGone; a: demand |
| 707 | 6 | the shown item is the scheduler's active task when it is an item, otherwise the computations, which return to the label when the task ends | sb taskShownOverComputationsAndHoverListsBoth; M35 |
| 708 | 6 | the hover lists every item in progress, the shown item first, each with its label and count, and for the computations the recording and its step; nothing in it is a control | sb taskShownOverComputationsAndHoverListsBoth; a: demand |
| 709 | 6 | the cancel control is present exactly while the shown item is a scheduler task registered as cancellable and cancels that task; it is absent for saving and for the computations | sb cancelOnlyForACancellableShownTask; M35 |
| 710 | 6 | with no item in progress the activity area is empty; the warning is not part of it and stays | sb schedulerTasksShowTheirLabelsAndCounts, warningBesideComputationsThenAlone |
| 711 | 7 | a warning is shown beside the activity area while any recording has a current failure: the warning glyph and the number of recordings that could not be computed, counting recordings, not pairs | sb warningCountsRecordingsAndListsThem; cd failuresAreOnePerSessionInRowOrder; M36 |
| 712 | 7 | the warning is shown while computations continue, so an early failure is visible at once, and stands alone once nothing is computing | sb warningBesideComputationsThenAlone; M36 |
| 713 | 7 | its hover lists the recordings in session-model row order, each with the failed calculation and the reason, says which are tried again at the next start, and lists at most ten, then how many more | sb warningCountsRecordingsAndListsThem, warningListsAtMostTenRecordings; cd failureTextAndItsLimit, failuresNameEachCalculationOnce |
| 714 | 7 | it persists across restarts with nothing new written: a stored rejection is counted at the next start without loading the recording, once its source is restored and the column worker's pass has reported the record set | sb warningAfterRestart; cd storedRejectionIsAFailureWithoutLoad; M37 |
| 715 | 7 | a failure that is not stored is tried again at the next start and shows again only if it fails again; a retry that succeeds clears it | cd unstoredFailureReturnsOnlyWhenItFailsAgain; sb warningAfterRestart; M37 |
| 716 | 7 | it clears as the pair memory clears (a record change of the pair, an input change, a registry change, the session losing its row) or when the last source over the calculation is switched off; nothing else clears it and nothing dismisses it | cd failuresClearAsThePairMemoryClears, failuresFollowWhatIsSwitchedOn; sb warningAbsentWhenNothingFailedAndNotDismissable; M38 |
| 717 | 7 | a failure of a calculation no plot or column wants is not current and is not counted | cd failuresFollowWhatIsSwitchedOn; M38 |
| 718 | 8 | a recording with a current failure shows one warning glyph on its logbook row, at the left of its first cell, whatever column that is | li rowWarningAtLeftOfFirstCell, rowWarningFollowsTheFirstVisualColumn; M36 |
| 719 | 8 | the glyph's hover lists the recording's failed calculations with their reasons, in the form of the status bar's hover for that recording | li rowWarningHoverIsTheSessionsFailures, failedWriteIsListedInTheHover, failedLoadSessionShowsRowWarningNotPending |
| 720 | 8 | a row that is not loaded shows the glyph from the record set, without loading | li rowWarningAtLeftOfFirstCell; cd storedRejectionIsAFailureWithoutLoad |
| 721 | 8 | the cells over the failed calculation are blank; the pending cell is unchanged | li rowWarningAtLeftOfFirstCell, pendingCellsAreDistinctFromUnavailable, pendingCellBecomesValueWhenRecordIsWritten |
| 722 | 8 | the glyph takes room in the first cell's text rectangle only when shown: a logbook without failures looks exactly as before | li rowWarningAtLeftOfFirstCell, plainHeaderAndCellsAreIdenticalToBase |
| 723 | 9 | the working indicator and the warning badge on plot rows and column headers go, with the reserved room and the header's hover: such a row or header looks as any other | li headerIsPlainAndNothingAnimates, plainHeaderAndCellsAreIdenticalToBase; a: demand; M34 |
| 724 | 9 | the working-indicator clock goes: its creation, its following of the demand layer, its place in the application context and the views' repaint on its frames | li headerIsPlainAndNothingAnimates; a: demand |
| 725 | 9, as settled | the shared glyph painting and its hover helper go; one drawing of the warning glyph exists, the style's standard warning icon, used by the status bar and the logbook row | sb warningCountsRecordingsAndListsThem; li rowWarningAtLeftOfFirstCell; a: demand |
| 726 | 9 | the logbook's progress line, its cancel button, the dock feature's scheduler wiring and the fixed minimum size hint go | li headerIsPlainAndNothingAnimates; a: demand |
| 727 | 9 | the fill's separate progress text and its display progress total go | cd fillTaskRestsWhileWaiting; sb computationsAreOneItem; a: demand |
| 728 | 9 | the demand layer's per-source state, its queries, its change signals and the working id lists go; the audit's demand group keeps the names out | cd columnProgressAndPendingCells; a: demand |
| 729 | 10 | progress: the sessions waiting or running in any source, the high-water mark, the running recording's name and step, announced when any changes; the step changes without a pass | cd progressCountsEachSessionOnce, progressTextWithoutAPass, changeSignalsAreMinimal |
| 730 | 10 | failures: per session with a current failure, in row order, its display name and failed pairs with title, reason and retry flag, announced when the set or an entry changes; for the whole list or one session | cd failuresAreOnePerSessionInRowOrder, failuresNameEachCalculationOnce |
| 731 | 10, as settled | pending cells are unchanged, with an announcement of their own per column | cd pendingCellsChangedPerColumn; li pendingCellsChangeRepaintsOnlyThatColumn, pendingCellsAreDistinctFromUnavailable |
| 732 | 10 | they are computed in the one walk from the tracks it classifies and the pair memory, with no second pass or key; counts no view reads are not kept | cd columnProgressAndPendingCells, passOverManyStubsReadsEachRecordSetOnce; a: demand |
| 733 | 11 | the status bar is the one place background work is shown: label and count, the bar showing the same, the hover listing everything, a cancel control only for work that can be stopped, empty when idle | sb schedulerTasksShowTheirLabelsAndCounts, computationsAreOneItem, cancelOnlyForACancellableShownTask; M34 |
| 734 | 11 | the warning says how many recordings could not be computed for as long as that is true, restart or not, and its hover which and why; it stands alone when nothing is computing | sb warningBesideComputationsThenAlone, warningAfterRestart; M37 |
| 735 | 11 | the row of a recording that could not be computed carries one glyph whose hover says what failed and why; its cells over the failure are blank; a cell still to come shows "…" | li rowWarningHoverIsTheSessionsFailures, rowWarningAtLeftOfFirstCell; M34; M36 |
| 736 | 11 | plot rows and column headers show nothing about computing; a track still to come is absent from the plot and the plot widget does not warn about it | li headerIsPlainAndNothingAnimates; cd merelyUncomputedIsNotWorthAWarning; a: demand; M34 |
| 737 | 11 | nothing offers a refresh or a cancel for computations | li clickOnRowWarningIsAClickOnTheCell; sb cancelOnlyForACancellableShownTask; a: demand; a: gestures |
| 738 | 12 | the main window owns the status bar and its component, created from the application context, not a dock; it reads the scheduler's and the demand layer's values, asks the scheduler to cancel, and decides nothing | sb survivesDemandDestroyedFirst; a: demand; M34 |
| 739 | 12 | the scheduler keeps its contract, knows nothing of jobs or demand and reports the fill as a task; the status bar maps the fill to the computations; it gains one read-only query, whether any task has work, so that a test can wait for background work to end | sme schedulerCompletesWaitingTaskWhoseWorkIsGone, schedulerHasWorkFollowsItsTasks; sb computationsAreOneItem; a: demand; a: branch-mechanisms |
| 740 | 12 | the demand layer stays widget-free, the only offerer and observed by nothing that changes what it does; its presentation surface is progress, failures and pending cells | cd nullCollaborators; a: widget-free-core; a: gestures; a: demand |
| 741 | 12 | the logbook view presents the row glyph and the pending cell and no task progress; its dock feature connects no scheduler | li rowWarningAtLeftOfFirstCell, headerIsPlainAndNothingAnimates; a: demand |
| 742 | 12 | the plot list presents nothing of the demand layer | a: demand |
| 743 | 12 | the application context carries the demand layer for the logbook and the status bar, and no clock | a: demand |
| 744 | 12 | the flow stays one way, ending in the demand layer presenting progress and failures as values that the status bar and the logbook read | cd recordReasonReachesDemandThroughRecordChange, failedRecordWriteIsShownAndNotRetried; li pendingCellBecomesValueWhenRecordIsWritten; a: demand |
| 745 | 13 | test: the activity area bullet of §13 | sb schedulerTasksShowTheirLabelsAndCounts, computationsAreOneItem; cd progressCountsEachSessionOnce |
| 746 | 13 | test: task over computations, their return, the hover | sb taskShownOverComputationsAndHoverListsBoth |
| 747 | 13 | test: the cancel control | sb cancelOnlyForACancellableShownTask |
| 748 | 13 | test: the warning shown, counted, listed, capped, beside and alone, absent | sb warningBesideComputationsThenAlone, warningCountsRecordingsAndListsThem, warningListsAtMostTenRecordings, warningAbsentWhenNothingFailedAndNotDismissable; cd failureTextAndItsLimit |
| 749 | 13 | test: through a restart | sb warningAfterRestart; cd storedRejectionIsAFailureWithoutLoad, unstoredFailureReturnsOnlyWhenItFailsAgain |
| 750 | 13 | test: clearing, not dismissable | cd failuresClearAsThePairMemoryClears, failuresFollowWhatIsSwitchedOn; sb warningAbsentWhenNothingFailedAndNotDismissable; li rowWarningFollowsFailures |
| 751 | 13 | test: the logbook row | li rowWarningAtLeftOfFirstCell, rowWarningHoverIsTheSessionsFailures, failedLoadSessionShowsRowWarningNotPending, plainHeaderAndCellsAreIdenticalToBase |
| 752 | 13 | test: removed presentation | li headerIsPlainAndNothingAnimates; a: demand |
| 753 | 13 | test: one walk, change-only announcements, earlier items unchanged in what they assert | cd changeSignalsAreMinimal, pendingCellsChangedPerColumn, rowScript, enablingColumnFillsEveryUnloadedSession, settledPairsSurviveEvictionSortAndColumnWorker; tst_fusion_rows realRowScript; a: demand |
| 754 | 14 | docs/ and tests/README.md describe the status bar, the row warning, the demand layer's progress, failures and pending cells and what stays visible after a restart, and no document names the removed surface | a: demand |

Items 745-753 state their §13 bullet in full in the map and appendix H (the
table abbreviates them). The Clause column is the substance; wording may be
polished, and appendix H, 9.8 and the map say the same.

**Amended items.** Phase 4's list, plus the items phases 2 and 3 changed.
Restate each with the change named; keep its lines and add those given.

| Items | The restatement says |
|---|---|
| 111 | never counted in progress nor listed among the failures |
| 115 | the sessions still to compute falling |
| 228 | the failure text (the status bar's and the row's hover) keeps the reason |
| 306, 321, 337 | a restored result is not counted in progress and runs no job ("no working indicator", "the row plain" go) |
| 339 | restored with their reason, listed among the failures, not tried again at the next start |
| 517, 555 | progress counts it from the first change |
| 525, 526, 554, 654 | listed among the failures with its reason (526, 554: marked tried again at the next start) |
| 530, 559, 612, 642, 653 | the fill reports no progress of its own; the status bar shows the computations while it works |
| 535, 644 | plot rows and column headers show nothing about computing; progress is shown once, in the status bar; add `sb computationsAreOneItem` and `M34` |
| 536, 645 | the status bar's hover carries the numbers and the running step; the warning's hover the failures; add `M34` |
| 537 | the warning counts the recordings as soon as a failure is current, beside the computations and alone once they end; add `sb warningBesideComputationsThenAlone`, `M36` |
| 539, 647 | the status bar shows the fill as "Computing results: k / n", no cancel button, distinct from "Computing columns: k / n" |
| 542, 631 | the demand layer publishes progress, failures and pending cells (631: the one walk derives them) |
| 557 | progress and failures reflect waiting, running, done and failed tracks; plot rows and headers show nothing |
| 563, 662 | the documents describe the status bar and the row warning, not the indicator, the clock or the fill's progress text |
| 604 | "tallied" goes |
| 618, 648 | a failure of the recording, shown by the status bar's warning and the row |
| 624, 656 | the failure entries and progress name a session by the display name |
| 627 | the per-source counts go |
| 646 | no clock exists; nothing repaints by itself while work runs; add `li headerIsPlainAndNothingAnimates`, `M34` |
| 649 | pending cells, a failure's meaning and the absence of refresh and cancel for computations are unchanged ("the hover detail" goes) |
| 650 | one drawing of the warning glyph, the style's; each view holding the demand layer learns of its end itself (was "as settled") |
| 651 | the status bar and the logbook present its values and decide nothing |
| 657 | spec §13's last bullet |
| 660 | the plot row shows nothing; no clock; the status bar shows the fill as the computations; a failed write listed in the row's and the status bar's hover |

## Decisions

- **Items from §5 onward, plus §14.** As instructed and as the specification
  numbers nothing: one clause per testable statement of §5-§12, one per §13
  bullet, one for §14, as appendices F and G carried their documentation
  section. §2-§3 hold nothing a test can observe beyond what §5-§12 apply.
- **Settled clauses 703, 725, 731**: the count shown done out of the
  high-water mark (overview decision 3), the standard warning icon (decision
  1, spec §9 leaves it open), pending cells' own announcement (decision 4).
- **Amended beyond phase 4's list**: 306, 321, 337, 530, 535, 539, 559, 563,
  612, 642, 644-647, 649-651, 653, 660, 662: phases 2 and 3 removed the
  indicator, the clock, the progress line and the fill's progress these
  clauses name.
- **M23, M29, M30 are superseded, not rewritten**: their subject (the arc,
  one glyph, one clock) is gone; keeping the ids keeps the history readable.
- **SENSOR_FUSION.md, COMPUTED_PLOTS §3 and §9, CALCULATIONS §17** are edited
  though spec §14 does not name them: they state the removed presentation, and
  the extended document rule searches all of `docs/`.
- **No new audit rule**; the document rule is extended, so the rule count is
  unchanged and checkable.

Ready with caveats: the map lines use the phase documents' suggested function
names and must be checked against the tree; the status bar's and the failure
list's exact texts are documented from the tree; 42 items are restated, which
the reviewer checks against the tree rather than against this table's short
forms.
