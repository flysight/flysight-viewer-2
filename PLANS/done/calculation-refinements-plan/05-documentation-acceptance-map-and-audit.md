# Phase 5: Documentation, acceptance map and audit

## Overview

This phase brings every document, test catalogue and traceability record in
line with "Calculation refinements" as Phases 1-4 implement it (spec §14's
audit bullet and §15). It rewrites the executor's and the demand layer's
sections of `docs/CALCULATIONS.md` (15, 16 and every scheduler passage), the
user guide `docs/COMPUTED_PLOTS.md` (sections 2, 3, 4, 7, 8), `docs/DATA_SCHEMA.md`
sections 11 and 12, one sentence of `docs/SENSOR_FUSION.md`, the root
`README.md` (source tree and documentation list) and `tests/README.md`. It adds
the acceptance range 601-662 (one item per testable statement of the
specification, item = 600 + clause number of appendix G), restates thirteen
demand-driven items "(as amended)", and finishes the audit's `gestures` and
`demand` groups with 21 new rules: the removed and moved names stay absent,
one working-indicator clock, one place that makes it follow the demand layer,
shared glyph plumbing, the fill's own progress text, the demand layer
observing no display change of the model, and the new rule that no code
outside the session model, the logbook index and the registry-side authority
computes a column's requested calculations. No behaviour changes:
the only edits outside `docs/`, `README.md`, `tests/README.md`,
`tests/acceptance_map.txt` and `tests/audit/cleanup_audit.cmake` are comment
rewordings that a new rule forces (Task 5.5, "plant a hit").

## Dependencies

- **Depends on:** Phases 1-4, all implemented, reviewed, committed and tagged
  (`plan/calculation-refinements/phase-4-done`).
- **Blocks:** None.
- **Assumptions** (names fixed by the phase documents; if a committed phase
  deviated, follow the committed code, point a map line at the committed name
  only if that function asserts the same clause, and report the difference;
  never delete a map line to make the audit pass):
  - **Phase 1.** `IdleScheduler` completes a task registered **with
    `TaskDef::canStep`** that it last reported active and that has no work any
    more, at the head of the next tick: `progressChanged` once, then
    `onComplete(false)`, then the next `activeTaskChanged` or `schedulerIdle`.
    Tasks without `canStep` (save, load, bulk edit, column work) complete only
    through their step or `cancel()`. The fill has no `isFillEnding()` /
    `m_fillEnding`; its `canStep` is `canLoad()`; its high-water mark resets in
    `onComplete` and starts afresh when the pending sessions rise from 0.
    `LogbookManager::setCalculationRecordReason()` emits
    `calculationRecordsChanged(session, calculation)` when the stored reason
    changes (private helper `storeRecordReason`); a write or removal still
    emits exactly once. `CalculationResultStore` is a `QObject` with
    `recordWriteFailed(sessionId, calculationId, reason)`, relayed as
    `SessionModel::calculationRecordWriteFailed(...)`; it follows the manager's
    `calculationRecordsChanged` for the pair (not for an unknown session);
    `reason` is never empty ("The result could not be stored" when the manager
    gives none). `SessionModel::columnRequestedCalculations(int)`,
    `columnDependencyClosure(int)` (lazily current: `m_columnTablesStale`) and
    `sessionDisplayName(int row)`. `LogbookManager` keeps its own call of
    `logbookColumnExplicitCalculations()` (logbookmanager.cpp ~112).
    `SessionModel::processNextBulkEdit` publishes its edit through
    `publishInvalidation` on both paths (a stub: the attribute's own
    dependency key; a loaded row: what `setAttribute` returned), and
    `CalculationDemand` has no `onSessionDataChanged` and no connection to
    the model's `dataChanged`. `JobQueue`
    has no `jobQueued`, `idle()`, `activeJobs()`, `m_idleAnnounced`,
    `announceIdleIfIdle()` or `AfterEnd`; a job record's `sessionName` is
    `sessionDisplayName()` of its row; `cancel(JobId)` is unchanged and its doc
    comment says it is kept for the jobs dock with no product caller. Tests use
    `activeJobIds(const JobQueue&)` (`tests/support/jobfixture.h`) and the job
    model's `rowsInserted`.
  - **Phase 2.** Parts: `src/demandstate.{h,cpp}` (`DemandCondition`,
    `DemandTrack` without `settling` / `job`, `DemandState` without `waiting`,
    with `addTrack()`, `finish()`, static `buildToolTip()`,
    `kToolTipListLimit`), `src/demandsettleclock.{h,cpp}` (`DemandSettleClock`),
    `src/demandfill.{h,cpp}` (`DemandFill`, `Hooks`, `kMaxHeldSessions =
    JobQueue::kMaxRunningJobs + 1`), reconciler `CalculationDemand` in
    `src/calculationdemand.{h,cpp}`. No `m_offeredJob` / `withdrawOwnOffer`;
    `withdrawChoice()` withdraws the chosen next job whenever the choice finds
    nothing. Audit: variables `DEMAND_LAYER` / `DEMAND_FILES` and three rules
    in `audit_group(demand)`: "the demand layer loads and pins through its fill
    only", "the fill and the settle clock never call the executor", "the fill
    and the settle clock know nothing of the walk".
  - **Phase 3.** One walk `walkRows()` under one `RowStabilityGuard`; `Source`
    (kind Plot / Column); one report memo `m_reports`; one pair memory
    (`PairMemory`: NotApplicable with origin Refused or ColumnVerdict; Failed
    with origin Result, Job, Load or Write); no `Memory`, `Settlement`,
    `CellKey`, `m_settled`, `eraseSettlements`, `loadFailedSettlement`,
    `ColumnWalk`, `walkColumns`, `ColumnInfo`, `plotCandidates`,
    `inspectUnderGuard`, `inspectedPlots`, `syncColumns`, `m_columnReports`,
    `buildState`, `JobFailed`. As the specification has it: a column verdict
    holds for that column only and is read only by the unloaded rule 3; an
    unstored result (a computation that threw) is a Result fact; a successful
    load forgets the session's Load facts; a session's facts are cleared by
    its input change alone (a bulk edit publishes one), never by a display
    change of the model (`Forget { Everything, LoadFailures }`). Settled by
    the plan: column candidate filing follows the plot rule; the fill
    releases every hold once the executor is shut down. The demand layer calls
    `CalculationRegistry::explicitDependencies()` once, for a plot's storable
    calculations. The walk-boundary rule's regex holds `PairMemory|LearnedFact`.
  - **Phase 4.** `src/ui/docks/DemandIndicator.{h,cpp}` (QtCore + QtGui, no
    demand type) gains `GlyphMetrics` / `glyphMetrics()`; new
    `src/ui/docks/DemandIndicatorView.{h,cpp}` (`glyphColor` ×2,
    `drawDemandGlyph`, `showIndicatorToolTip`, `repaintWhenDemandDestroyed`,
    `followDemand`); one `WorkingAnimation` created in `MainWindow`
    (`m_workingClock`) right after the demand layer, deleted right after it,
    handed through `AppContext::workingClock`; `PlotRowDelegate(demand, clock,
    view)`, `LogbookView(model, demand, clock, parent)`,
    `LogbookHeaderView(model, demand, clock, parent)`; `indicatorRect()`
    replaces `clusterRect()`; `PlotRowGeometry {glyph, reservedWidth}`;
    `DemandState::progressLabel` removed; the fill's label
    `tr("Computing results: %v / %m")` in `LogbookView`; the audit's "only the
    application and its views know the demand layer" allows
    `DemandIndicatorView.*` and "the demand views handle no event of their own"
    searches it; map line `562 tst_plot_row_layout indicatorAndWarning` deleted.
  - **Tests the map will cite** (all named in the phase documents):
    - Phase 1: `tst_session_model_engine::schedulerCompletesWaitingTaskWhoseWorkIsGone`,
      `tst_session_model_engine::bulkEditAnnouncesADependencyChange`,
      `tst_logbook_index::recordReasonChangeIsAnnounced`,
      `tst_result_store::writeFailureIsAnnounced`,
      `tst_result_columns::sessionModelExposesColumnKnowledge`,
      `tst_result_columns::sessionDisplayNameOfEveryRowKind`,
      `tst_calculation_demand::recordReasonReachesDemandThroughRecordChange`,
      `columnKnowledgeComesFromTheSessionModel`,
      `fillEndingBehindAnotherTaskStartsNextCountFresh`.
    - Phase 2: `tst_calculation_demand::chosenNextJobFollowsTheExecutorsAnswer`,
      `settleClockAnswersItsQuestions`.
    - Phase 3: `tst_calculation_demand::failedRecordWriteIsShownAndNotRetried`,
      `failedRecordWriteIsShownOnThePlotRow`,
      `settledPairsSurviveEvictionSortAndColumnWorker`,
      `pairMemoryIsClearedByRecordInputAndRegistryChanges`,
      `columnVerdictDoesNotSuppressAnotherColumn`,
      `runningColumnTrackFilesItsOtherBlockers`.
    - Phase 4: `tst_plot_row_delegate::sharedGlyphPlumbing`,
      `tst_logbook_indicators::plotRowsAndHeaderTurnOnOneClock`,
      `fillProgressLineHasItsOwnText`, `failedWriteIsListedInTheHover`;
      `tst_plot_row_layout::indicatorOnly` (retargeted), `nothingShown`,
      `rightToLeftIsMirrorImage` (kept); `indicatorAndWarning`, `warningOnly`,
      `emptyLabelOmitsItsSpacing` deleted.
  - `docs/`, `README.md` and `tests/README.md` are untouched by Phases 1-4, so
    the line numbers quoted below (taken at `ec6bd43`) hold for them.
    `tests/acceptance_map.txt` (one line deleted by Phase 4) and
    `tests/audit/cleanup_audit.cmake` (edited by Phases 2-4) have moved:
    locate their passages by the quoted text.
  - `PLANS/` is untracked. Nothing written in this phase links to it or names a
    file in it. The specification is cited by its title, "Calculation
    refinements".

## Tasks

### Task 5.1: `docs/CALCULATIONS.md` section 15 (the executor)

**Purpose:** Spec §15 first item: the executor's signals and queries as they
are now, cancel kept for the jobs dock, the job record's session name, and the
store's announcement of a failed write.

**Files to modify:**
- `docs/CALCULATIONS.md` (lines 662-1128)

**Technical Approach:**

1. **§15 introduction, line 686-688.** "except a job-level failure, which it
   remembers for the rest of the run (16.7)" becomes "except a failure that is
   not a function of the inputs, which it remembers for the rest of the run
   (16.7)". After the paragraph "**It is not a queue.**" add one paragraph:
   **What it announces.** The executor's state is in its job model and four
   queries; it has no idle signal, no queued signal and no query of both active
   jobs, because no product code used them. A new job is a new row of the job
   model (`rowsInserted`); `isIdle()` is "nothing runs and no chosen next job";
   `runningJob()` and `chosenNextJob()` are the two active jobs. `cancel(JobId)`
   is kept for the jobs dock, a later view of the job history; no product code
   calls it today (15.3).
2. **§15.2 Replacement, lines 730-737.** Replace
   "with no `idle()` between the old job and the new one, and the executor
   validates the offer again after that end's signals (a slot may shut it down
   or offer). The new job is Queued, pinned, announced (`jobQueued`,
   `jobsChanged`) and started from the event loop, never synchronously.
   **Withdrawal.** `withdrawChosenNext()` ends the chosen next job Cancelled
   ("No longer needed"); `idle()` follows when nothing runs."
   with: the executor validates the offer again after that end's signals (a
   slot may shut it down or offer); the new job is pinned, appended to the job
   model as Queued (`rowsInserted`), announced by `jobsChanged` and started from
   the event loop, never synchronously. **Withdrawal.** `withdrawChosenNext()`
   ends the chosen next job Cancelled ("No longer needed"); nothing else
   follows, and `isIdle()` is then true when nothing runs.
3. **§15.2 Order of a job's end, lines 798-810.** Step (6) becomes "the chosen
   next job, which a slot may have offered during (3), is scheduled to start".
   Delete "A chosen next job that an offer replaces skips (6)." Replace "so the
   next link of a chain is chosen before step (6) and the executor reports no
   `idle()` between links (16.4)" with "so the next link of a chain is chosen
   before step (6) and the executor is never idle between links (16.4)". Keep
   the last sentence (slots may call `offer()`, `withdrawChosenNext()`,
   `cancel()` and `shutdown()`).
4. **§15.3 first bullet, lines 818-822.** Replace "`cancel(JobId)` is kept for a
   later jobs dock; no product code calls it (audit group `gestures`, "no
   product code cancels a job")." with "`cancel(JobId)` is kept for the jobs
   dock, a later view of the job history; no product code calls it today, and
   its doc comment and the cleanup audit (group `gestures`: "no product code
   cancels a job", "cancel is kept for the jobs dock") say so." Keep the rest
   of the bullet (a jobs dock that cancels will need its own policy).
5. **§15.6, line 955.** `SessionNameRole` "(a snapshot at the offer:
   `_DESCRIPTION`, else the id)" becomes "(a snapshot at the offer: the session
   model's display name of the row, `SessionModel::sessionDisplayName()`,
   16.2)".
6. **§15.7 table, lines 988-1008.**
   - Delete the row `activeJobs()`.
   - Row `cancel(id)`: "false: unknown or already finished. Kept for the jobs
     dock; no product caller today (15.3)".
   - Signals row: the left cell lists `jobStarted(id)`, `jobProgress(id,
     text)`, `jobCancelRequested(id)`, `jobFinished(id, state)`,
     `jobsChanged()` (after each of the others except `jobProgress`); the right
     cell becomes "A created job is announced by the job model's `rowsInserted`
     (then `jobsChanged`); a job that a `rowsInserted` slot ended (cancel,
     shutdown) has `jobFinished` as its only executor signal, and `offer()`
     still returns `Created`. There is no idle or queued signal: `isIdle()`,
     `runningJob()`, `chosenNextJob()` and the records answer those
     questions".
   - Row `runningJob(), job(id), isIdle(), isShutDown()`: keep; add "the
     running job and the chosen next job are the only active jobs".
   - Tests paragraph (1010-1012): add "`activeJobIds()` (the ids of the active
     records, in model order)" to the helpers of `jobfixture.h`, and say
     `Quiet` means "no new job row".
7. **§15.8 Stored results, bullet at 1030-1036** ("`Installed` with status
   `Ok`: ..."). Replace its last sentence ("A failure is warned once, leaves
   ... before the next `Ok` publish.") with:
   - A failure is warned once, leaves the previous record and the in-memory
     result untouched, is not retried, and is **announced**:
     `CalculationResultStore::recordWriteFailed(sessionId, calculationId,
     reason)`, relayed by `SessionModel::calculationRecordWriteFailed(...)`
     (consumers connect to the model, never to the store), with the manager's
     error text ("The result could not be stored" when it gives none). It is
     emitted from inside the engine's explicit-result listener, after the
     manager's `calculationRecordsChanged` for the same pair (a failure for an
     unknown session has no record change), so a slot records state and
     schedules only. `stats().writeFailures` counts the announcements. The
     demand layer shows the failure (16.7).
   Add one bullet after it: **Reasons.** The store reports to the logbook index
   the reason of every record it writes and of every record it restores and
   keeps (`LogbookManager::setCalculationRecordReason()`); a reason that
   differs from what the index held is announced as a record change of the
   pair (`calculationRecordsChanged`), an unchanged one emits nothing
   (DATA_SCHEMA section 11). A write or a removal emits exactly one record
   change, whether or not its reason changed.
   Tests paragraph (1125-1128): unchanged file list (the new functions are in
   the listed files).

**Acceptance Criteria:**
- [ ] `docs/CALCULATIONS.md` contains no `idle()`, `jobQueued`, `activeJobs`, and `_DESCRIPTION` only where it names the session attribute outside 15.6 (grep).
- [ ] 15.3 and 15.7 say that `cancel(JobId)` is kept for the jobs dock with no product caller, citing the two `gestures` rules by label.
- [ ] 15.8 describes `recordWriteFailed` / `calculationRecordWriteFailed`, its order after the record change, and the announcement of a changed reason.

**Complexity:** M

---

### Task 5.2: `docs/CALCULATIONS.md` section 16 and the scheduler passages

**Purpose:** Spec §15 second and third items: one walk, one memory, the parts
of the demand layer, the fill's completion, the failed write, the session
model as the source of column knowledge and display names, one glyph and one
clock, and every sentence that says a task completes by stepping.

**Files to modify:**
- `docs/CALCULATIONS.md` (lines 1130-1766; §16 keeps its subsection numbers
  16.1-16.11, which other documents and comments cite, and gains 16.12)

**Technical Approach.** Rewrite in place, subsection by subsection. Write "the
executor", "the demand layer", "the session model", "the logbook index", "the
result store" in prose, and class names in code spans. Never write
"settlement", "settled" (for the memory), `idle()`, `progressLabel`, "one
clock per view", "lose work only by stepping", "under the same label" (Task
5.5's docs rule).

1. **§16 introduction, lines 1137-1146.** Keep the first paragraph. The second
   becomes: `CalculationDemand` (`src/calculationdemand.h`), the demand layer,
   is widget-free and lives in `flysight_core` ... (unchanged) ... It observes
   the models, the logbook's record changes (which also announce a reason the
   index learned, 16.7), the session model's relay of a record that could not
   be written, the registry and the executor's signals. It is divided into
   parts whose contracts stand alone (16.12): the presentation values
   (`src/demandstate.h`), the column fill (`src/demandfill.h`), the settle
   clock (`src/demandsettleclock.h`), and the reconciler, the component itself.
   Keep the principles list; add a seventh bullet: "a fact is computed by the
   component that owns it and read by the others; a component announces what
   it changes (16.11)".
2. **§16.1, lines 1158-1227.**
   - Plot demand, column demand: unchanged. Add after column demand: a
     **source** is a checked requested plot or an enabled requested column; a
     **track** is one session of a source (plot: a visible, loaded row that is
     not a failed-load placeholder; column: every row). Plots and columns are
     walked, classified, tallied and filed as candidates by one walk (16.3,
     16.6); the kind of the source is the only difference.
   - **Where a result is looked up.** Loaded session: blocker inspection of the
     source's names - a plot's y name, a column's one or two names - combined
     alike for both (the combination rule stays; a single name combines to
     itself). Session that is not loaded (a stub, or a failed-load
     placeholder): what this run remembers of its pairs (16.7) and the
     logbook's known record names with the reasons the index recorded; no
     record is opened. Keep the sentences on staleness (a known record counts
     until the column worker's restore deletes it as stale) and on explicit
     family instances.
   - **Conditions table** (1190-1198). Rows:

     | Report | Condition |
     |---|---|
     | `Available`, no storable calculation of the source remembered failed | `Done` |
     | `Available`, a storable calculation remembered failed (a record that could not be written, 16.7) | `Failed` |
     | `NotApplicable` | `NotApplicable`: silently absent |
     | `NotProduced` | `Failed`: an input-determined failure, reason built from the notes |
     | `Blocked`, a blocker is the running job not asked to stop | `Running` |
     | `Blocked`, otherwise a blocker has a remembered failure (16.7) | `Failed` |
     | `Blocked`, otherwise every blocker was refused by the executor as not applicable | `NotApplicable` |
     | `Blocked`, otherwise | `Waiting` |

     Then: a column's not-applicable verdict (16.7) is never read for a loaded
     session: the engine is the authority there.
   - **Unloaded order**, replacing lines 1202-1208: a column track of a session
     that is not loaded is decided in this order: (1) the source has no
     storable calculation (explicit family instances alone) -> `NotApplicable`;
     (2) a storable calculation remembered failed (a job, a load or a record
     write that failed) -> `Failed` with the remembered reason(s); (3) every
     storable calculation remembered not applicable for this column ->
     `NotApplicable`; (4) every storable calculation has a record -> `Done`,
     or `Failed` with the first reason the index recorded; (5) a failed-load
     placeholder -> `Failed` ("The session file could not be loaded",
     remembered for its pairs, so the next pass gives the same track); (6)
     otherwise `Waiting`. The index is asked only in step 4, once per session
     between record changes (the memo of 16.3). Keep the two bullets (a
     session without the inputs; the failure reason form) and the paragraph
     "Only the y name is inspected".
3. **§16.2, lines 1229-1289.**
   - Opening: `plotState()` / `columnState()` return a `DemandState`
     (`src/demandstate.h`), a plain value. Rest of paragraph unchanged.
   - Table: delete the `progressLabel` row. Row `running, waiting, failed`
     becomes `running`, `failed`: "lists of `DemandTrack` in row order; waiting
     tracks are counted (`waitingCount`), never listed". Row `failedCount`:
     "input-determined failures and failures remembered by this run (a job, a
     load, a record write)". Row `toolTip`: "`DemandState::buildToolTip(*this)`;
     empty for a plain state". Add: `addTrack()` counts a track and `finish()`
     builds the tooltip; the reconciler calls them.
   - `DemandTrack` paragraph (1254-1260): `sessionId`; `sessionName`
     (`SessionModel::sessionDisplayName()` of the row: the loaded session's
     description, else the description the logbook index caches for the row,
     else the session id - the executor's job records use the same name;
     `tst_result_columns::sessionDisplayNameOfEveryRowKind`); `condition`;
     `calculationTitles`; `reason` (failed); `jobFailure` (failed: not a stored
     result - a job, a load or a record write that failed - so tried again at
     the next start); `progressText` (running). Delete `settling` and `job`.
   - Tooltip block and limit: "Each list shows at most
     `DemandState::kToolTipListLimit` (10) sessions ...". The tooltip is the
     only place a view shows the counts (16.10).
   - Line 1280-1281: "which is what the views' animation clocks follow" becomes
     "which is what the application's one working-indicator clock follows
     (16.10)".
   - Running job's progress text: "updates the states without a pass and
     without inspection, on the running tracks of the job's session".
4. **§16.3, lines 1291-1345.**
   - Requested: keep the plot sentence (`dependsOnExplicit()`). Replace "A
     column is requested when `logbookColumnExplicitCalculations()` is not empty
     (the same registry answer the column cache uses; section 17)" with: a
     column is requested when `SessionModel::columnRequestedCalculations(i)` is
     not empty. **The session model is the one source of each enabled column's
     requested calculations and static dependency closure**
     (`columnRequestedCalculations()`, `columnDependencyClosure()`: computed
     from the registry by `logbookColumnExplicitCalculations()` and
     `staticDependencies()`, section 17; valid between two column rebuilds -
     a column change resets the model - and current under the registrations at
     the moment of the call). The demand layer reads them at the start of every
     pass and computes neither; a column change reaches it through the model's
     reset alone. A plot's closure and requested calculations
     (`CalculationRegistry::staticDependencies()` and `explicitDependencies()`
     of the y name) the demand layer computes itself: the model knows nothing
     of plots. The logbook index keeps its own registry-side use, because it
     sits below the model and decides the validity of cached values before any
     model exists (DATA_SCHEMA section 11). The cleanup audit (group `demand`)
     allows `logbookColumnExplicitCalculations()` in the registry-side
     definition, the session model and the index only.
   - **What a pass costs** (1309-1324), replaced: one walk over the session
     rows, for every source at once, under one `RowStabilityGuard`, returns
     plain values: states, counts, listed tracks, pending cells, candidates by
     tier, load candidates and the facts it learned (16.7). Nothing is loaded,
     evicted or touched in the LRU inside it; offers, withdrawals, holds, loads
     and signals come after it. Plots that are not requested are never
     inspected. A loaded session's combined report is memoized per session and
     source and computed only for that source's tracks (a plot's only for
     visible loaded rows), so `blockers()` runs for (checked requested plots x
     visible loaded tracks) and (requested columns x loaded rows) only when the
     memo is missing. The memo is dropped per session by any
     `dependencyChanged` of the session (an input change, a bulk edit or a
     publication), a load, a record change, a job's end, and wholly by a
     reset and a registry change - so after A publishes, B's report
     is computed again although B's output may not be re-announced (the reason
     the memo is per session, not per name). A session that is not loaded
     costs one `knownCalculationRecords()` call between changes to its records.
     A pass is O(rows x sources) hash lookups; with no source, the rows are not
     walked.
   - **When a pass runs** table (1333-1340): row `SessionModel`: "...,
     `dependencyChanged` of a relevant name (16.5; a bulk edit publishes one
     on both of its paths, so a bulk edit of a session that is not loaded
     arrives here too) and `calculationRecordWriteFailed` (16.7). The demand
     layer observes no `dataChanged` of the model: the column worker's
     display change of every stub it processes reaches nothing (16.7)". Row
     `LogbookManager`: "`calculationRecordsChanged` (a record written or
     removed, or a reason the index learned)". Row "the demand layer": "the end
     of a settle wait (16.5); a load of the column fill (16.8); an offer refused
     as not applicable, a failed load (16.7)".
   - Last paragraph (1342-1345): "So the demand layer drops its memos on
     `modelAboutToBeReset`, before any of that, and the pass reads the new rows
     and the model's columns."
5. **§16.4, lines 1347-1409.**
   - **Offering** (1384-1398), replaced: each pass offers the candidates in
     priority order (16.6) and acts on the executor's answer; it does not
     compare a candidate with the chosen next job itself:

     | Executor's answer | Reaction |
     |---|---|
     | `Created` | the chosen next job (a different one was replaced); done |
     | `AlreadyActive`, naming the chosen next job | already the choice, kept as it is; done |
     | `AlreadyActive`, naming the running job | next candidate (not expected: the running pair is not a candidate) |
     | `MissingInput`, `NothingToDo`, `UnknownCalculation` | remembered as refused, not applicable (16.7); next candidate |
     | `Blocked`, `SessionNotLoaded` | next candidate; nothing remembered |
     | `ShuttingDown` | stop |

     `Blocked` is not expected: the candidates list upstream first (16.6), and
     the executor's refusal kinds are unchanged; the code does not guard for
     it. When the choice finds nothing, the demand layer withdraws the chosen
     next job: it is the only offerer (cleanup audit, group `gestures`), so the
     chosen next job is always its own.
   - **Chained calculations** (1400-1406): "The pass that runs synchronously in
     `jobFinished` (before the executor schedules the next start, 15.2) offers
     the next link, so the executor is never idle between links; ...".
   - **After shutdown** (1408-1409): "nothing is offered or loaded, the holds
     are released (16.8), and the states stay working until the demand layer is
     destroyed".
6. **§16.5, lines 1411-1427.** "An input change forgets what this run
   remembered of the session (16.7), and starts or restarts its wait; the
   settle clock (16.12) holds the deadlines." First bullet: "counted
   `Waiting`, so the indicator shows from the first change, but they are not
   offered" (delete "(with `settling`)"). Name the constant
   `CalculationDemand::kInputSettleMs` as now.
7. **§16.6, lines 1429-1452.** Keep the three tiers and the last paragraph.
   Replace "Within a session: ... in row order." with: the walk files the
   candidates of plots and columns by one rule: every blocker of a `Blocked`
   report of a loaded track, in the tier of its source and session; within a
   session, source order (plot-model order, then column order), then the
   blockers' order (upstream first); each (session, instance) once, in its
   first tier. A pair is not a candidate while a failure or a refusal is
   remembered for it (a column's not-applicable verdict does not keep it from
   being offered), while it is the running job not asked to stop, or while its
   session settles. So a column track that is running on one blocker, or
   failed on one, files its other blockers, as a plot track does. A session
   that is not loaded is never offered: it enters tier 3 once the fill has
   loaded it (16.8).
8. **§16.7, lines 1454-1503.** Heading `### 16.7 Failures, not applicable, the
   pair memory`. Bullets, in this order:
   - **Input-determined failures are results** (keep). A result the engine
     cached as `Failed` (a compute function that threw) is kept in memory, not
     stored: badged, remembered for the pair (a session evicted afterwards is
     not loaded again for it), computed again after a restart.
   - **Failures that are not a function of the inputs** are remembered per
     (session, calculation instance) with a reason: a job that ended `Failed`
     (the worker could not be started, out of memory: "<title>: <reason>", for
     example "Sensor fusion: Out of memory"); a session file that could not be
     loaded ("The session file could not be loaded", for each storable
     calculation of the session's column sources; a failed-load placeholder,
     visible or hidden, gives the same); a record that could not be written
     ("<title>: <the manager's error>", for example "Sensor fusion: Couldn't
     write file ..."; 15.8). Badged (`jobFailure`), never pending, not offered
     and its session not loaded again for it until the memory is cleared (below).
     Not persisted: the next start tries again.
   - **A record that could not be written** is a failure although the loaded
     engine holds the result: a source is done only when none of its storable
     calculations is remembered failed, so the plot row and the column header
     list the session with the write's reason while it is loaded and after its
     eviction alike, and no session is loaded twice for it. The index's record
     change of the pair comes first and forgets what was remembered; the
     relay's `calculationRecordWriteFailed` follows and records the failure. A
     later successful write of the pair's record (its record change) clears it.
   - **Not applicable.** An offer the executor refuses as `MissingInput`,
     `NothingToDo` or `UnknownCalculation` is remembered for the pair; the
     track reads `NotApplicable`. A loaded column track the engine reports
     `NotApplicable` remembers each storable calculation of the column not
     applicable **for that column only** (a column verdict): it is read only
     when the session is not loaded and that column is classified (16.1 step
     3), so the session is not loaded again for the column; it never makes
     another source's track not applicable and never keeps a pair from being
     offered, because the engine reports a name `NotApplicable` whenever one
     input is genuinely missing, even if the calculation behind it is
     requestable. Plots remember no verdict. The refusal schedules a pass; the
     walk classifies before the offers, so a refusal reaches the plot row and
     the column header in the next pass.
   - **The pair memory.** One memory of this run, keyed by (session id,
     requested calculation instance id), holding a kind (not applicable, or
     failed with a reason) and where the fact came from. There is no memory per
     cell: a track's verdict is always derived - for a loaded session from the
     engine and the memory, for one that is not loaded from the memory and the
     record set. Facts the walk learns (a column verdict, an exception result,
     a placeholder's failed load) are applied after the walk.
   - **Clearing.** For a session: a relevant input change (16.5; a bulk edit
     is one, on both of its paths: the session model publishes it as a
     dependency change, for a stub with the attribute's own name); a
     successful load forgets its failed-load facts. For a pair: a record change
     of that pair. For everything: a registry change. A session-model reset (a
     sort resets the model) forgets the sessions that no longer have a row; an
     id correction by the fill forgets the old id. Multi-row changes (the unit
     system, the environment check) clear nothing, and neither does any display
     change of the model: the demand layer observes none, so the column
     worker's processing of a stub (the recomputation after an eviction that
     dropped values over an unconfirmed record included) never clears a fact.
     A session found not applicable is therefore not loaded again after an
     eviction, and a session whose record could not be written is not loaded
     again either.
   - **There is no retry control** (keep).
   - **Stored failures of sessions that are not loaded** (keep, with): the
     index learns each record's reason when the store writes it and whenever it
     restores it; a reason that differs from what the index held is announced
     as a record change of that pair, so the demand layer learns it without a
     load and through no other signal. Keep the
     sentence on a record written by an earlier build.
   Delete the bullets "Memory and resets", "Settlements" and "A session whose
   file cannot be loaded" (their content is in the bullets above).
9. **§16.8, lines 1505-1595.**
   - **The column fill** bullet (1510-1528), replaced: an `IdleScheduler` task
     under `SessionModel::ColumnFillTask` (4), priority 5 (below saving 1,
     visible loads 2, bulk edits 3, column work 4), registered by the fill
     (`src/demandfill.h`) and unregistered when the demand layer is destroyed,
     not cancellable. It has work while any session has a waiting or running
     column cell; it can step (load) only while a hold is free and an unloaded
     session waits; its progress is the sessions with such a cell of the fill's
     high-water mark. **Completion without a step:** when the last pending cell
     is resolved (the job's result published, a failure or a verdict
     remembered, the column disabled), the fill has no work, and the pass that
     found it so wakes the scheduler; on its next tick the scheduler reports the
     fill's final progress and completes it, before it reports the next active
     task or goes idle. There is no step that loads nothing. The high-water mark
     resets when the task completes; a fill that starts while no session had a
     pending cell starts its own count (a fill that ended while another task
     was active is completed only if it was the task last reported active). The
     logbook's progress line shows "Computing results: k / n" for the whole
     fill (16.10). **The scheduler's side** (generic, knowing nothing of jobs or
     demand): a task registered with `TaskDef::canStep` can wait on something
     outside the scheduler - reported as active with its progress, not stepped,
     the scheduler resting until woken instead of spinning - and can lose its
     work outside a step; when the task the scheduler last reported active is
     such a task and has no work any more, the next tick reports its progress
     one last time and calls its completion (not cancelled) before it reports
     the next active task or goes idle. Whoever takes a resting task's work
     away wakes the scheduler. The rule applies to tasks that can wait; the
     others (save, load, bulk edit, column work) complete through their step or
     `cancel()`, as before - they, too, can lose work outside a step (hiding
     the rest of a background load batch, an eviction's save, a flush), and
     completing them there would change when they complete. A cancelled task
     completes once, as cancelled; an unregistered one never. Keep the last
     sentences (the active task reported once per tick; a count shorter than a
     tick never shown; a tick that goes to a higher-priority task shows that
     task's progress).
   - **A load step** (1529-1539): last sentence "A load that fails is
     remembered as a failed load for the session's pairs (16.7)".
   - **The bound** (1540-1544): `DemandFill::kMaxHeldSessions`.
   - **Holds** (1545-1553): released "... when its row is gone or unloaded, when
     the demand layer is inert, once the executor is shut down (at the first
     pass after `shutdown()`, which runs inside it when a job was active), and
     at its destruction".
   - **Edge cases**: "a calculation found not applicable after the load: the
     column's verdict is remembered, without a job ..."; "the executor shut
     down: nothing more is loaded, and the holds are released".
   - **Known costs** (1583-1595): "'not applicable' is a verdict this run
     remembers for the column (16.7), not a record ... its cell is `Waiting`
     ("…"), counted in the column's counts (its header's tooltip) and in the
     fill's progress". Keep the second bullet and add: the product's
     registrations give every output of an explicit calculation one candidate
     (`tst_fusion_session::explicitOutputsHaveOneCandidate`); a registration
     that gave a requested column's name a second, non-explicit path could make
     a loaded track done while a storable calculation has no record, and that
     session would be loaded again after each eviction; the fix would belong to
     `CalculationRegistry::explicitDependencies()`.
10. **§16.9, lines 1597-1630.**
    - Table rows:
      - `CalculationDemand::kInputSettleMs` | 1000 ms (16.5).
      - `DemandFill::kMaxHeldSessions` | `JobQueue::kMaxRunningJobs + 1` (16.8).
      - `DemandState::kToolTipListLimit`, `DemandState::buildToolTip(state)` |
        10 (16.2); static and pure.
      - Delete the old combined constants row and `buildToolTip(state)` row.
      - `DemandCondition`, `DemandTrack`, `DemandState` (`isWorking()`,
        `showsWarning()`, `isPlain()`, `addTrack()`, `finish()`,
        `operator==`) | 16.1, 16.2; `src/demandstate.h`.
      - Test seams row: add "(the fill and settle seams forward to the parts,
        16.12)".
      - New rows: `SessionModel::columnRequestedCalculations(column)`,
        `columnDependencyClosure(column)` | 16.3: the enabled column's
        requested calculations and static closure; empty out of range; plain
        reads, allowed under a `RowStabilityGuard`, not from inside a registry
        observer. `SessionModel::sessionDisplayName(row)` | 16.2.
        `SessionModel::calculationRecordWriteFailed(sessionId, calculationId,
        reason)` | 15.8, 16.7.
      - `IdleScheduler` row: "`registerTask` replaces a task already
        registered under the id; `unregisterTask` removes one without calling
        its `onComplete`; `TaskDef::canStep` marks a task that can wait: it is
        not stepped while it cannot, and it is completed when it was the task
        last reported active and its work is gone (16.8)".
    - Tests paragraph (1624-1630): add `tst_session_model_engine` (a waiting
      task completed when its work is gone), `tst_logbook_index` (a changed
      reason announced), `tst_result_store` (a failed write announced),
      `tst_result_columns` (the model's column knowledge and display name).
11. **§16.10, lines 1632-1730.**
    - **Plot rows** (1634-1646), replaced: `PlotRowDelegate` ... paints one
      glyph right-aligned in the row of a requested plot: while working, the
      working indicator (an open 270-degree arc in the row's text colour,
      turning about once a second); once the work is finished and some sessions
      could not be computed, the warning badge; nothing else - no label, no
      count: the hover carries the numbers (`DemandState::toolTip`, over the
      whole row). Plain rows are painted by the unmodified
      `QStyledItemDelegate`, pixel for pixel; the name is elided so the glyph
      is never covered; the row height never changes. The geometry is the pure
      `layoutPlotRow()` (`PlotRowLayout.h`): one square glyph, no hit
      rectangle. Keep **No gestures**.
    - **The shared glyphs** (1648-1654), replaced by two paragraphs:
      - `src/ui/docks/DemandIndicator.h` (Qt Core and Gui only; no demand-layer
        type): `drawWorkingGlyph()`, `drawWarningGlyph()`, `glyphMetrics()`
        (the glyph's side for a line of text, `min(room, line height)`, and the
        spacing beside it, `max(2, side / 4)`), and `WorkingAnimation`, the
        working indicator's clock (80 ms per frame, 30 degrees per frame).
        **One clock per application:** `MainWindow` creates it right after the
        demand layer and hands it to the views through `AppContext::workingClock`,
        as it hands them the demand layer; `followDemand()` makes it active
        exactly while `workingPlotIds()` or `workingColumnIds()` is not empty
        (asked again on every `statesChanged()`), and stops it at frame 0 when
        neither is or the demand layer is destroyed. It never repaints anything
        itself: each view repaints only its own working rows or sections on
        `frameAdvanced` and reads the clock's angle when it paints, so the plot
        list's arcs and the logbook headers' arcs turn in step. A view given no
        clock draws the arc at rest.
      - `src/ui/docks/DemandIndicatorView.h`, the views' half (Qt Widgets; a
        view of the demand layer that decides nothing): `glyphColor()` - the
        style's text colour in its colour group (Disabled when not enabled,
        else Inactive when the window is not active, else Normal): `Text` or
        `HighlightedText` for a row, `ButtonText` for a header section;
        `drawDemandGlyph()` - the one glyph of a `DemandState`;
        `showIndicatorToolTip()` - the state's tooltip over an area, or nothing
        for a plain state; `repaintWhenDemandDestroyed()`; `followDemand()`.
        The plot rows, the headers and the cells compute none of these (the
        cell delegate's pending colour and tooltip are its own). The glyphs are
        drawn with `QPainter` (no bundled images).
    - **Logbook column headers**: "... shows the same glyph (`drawDemandGlyph()`,
      sized by `glyphMetrics()`) immediately right of its centred text ...;
      the header shows no count". Rest unchanged.
    - **Pending cells**: unchanged; add "the cell delegate turns plain when the
      demand layer is destroyed (`repaintWhenDemandDestroyed()`)".
    - **The progress line** (1678-1679), replaced: the logbook's progress line
      is unchanged in form. It labels the active scheduler task: "Saving
      sessions", "Loading sessions", "Updating sessions", "Computing columns:
      k / n" for the column worker, and **"Computing results: k / n"** for the
      column fill (`SessionModel::ColumnFillTask`, 16.8), so the line never
      shows two totals under one label when the fill follows a column pass.
      The fill shows no cancel button.
    - **Ownership and order** (1681-1698): "creates the `JobQueue`, then the
      `CalculationDemand` and right after it the working-indicator clock (made
      to follow the demand layer by `followDemand()`) ... and hands them to the
      docks through `AppContext` (`jobQueue`, `calculationDemand`,
      `workingClock`)". "`~MainWindow()` deletes the demand layer, then the
      clock, then the executor, explicitly and before everything else ...".
      "The plot list's delegate and the logbook's header view and cell delegate
      hold the demand layer and the clock weakly and turn plain once the demand
      layer is gone."
    - Tests paragraph (1724-1730): `tst_plot_row_layout.cpp` (the one glyph's
      geometry), `tst_plot_row_delegate.cpp` (also the shared plumbing,
      `sharedGlyphPlumbing`), `tst_logbook_indicators.cpp` (the header view,
      the cell delegate and, beside them, a plot list with the row delegate on
      one clock; the progress line's texts; a failed write in both hovers).
      Manual steps "M1-M9 and M23-M33".
12. **§16.11, lines 1732-1765.** Contracts:
    - **The demand layer:** from what is switched on and where results are, it
      derives every source's state and every candidate in one walk, chooses by
      priority, has sessions loaded for column demand within its bound, offers
      the next pair, remembers this run's failures and not-applicable verdicts
      in one pair memory, and publishes the state the views present.
    - **The executor:** unchanged, plus "; it announces its jobs through its
      job model and keeps `cancel()` for the jobs dock".
    - **The column worker:** unchanged.
    - **The idle scheduler:** runs steps of registered tasks by priority, and
      completes a task that can wait once its work is gone, whether or not it
      was stepped; it knows nothing about jobs or demand.
    - New: **The session model:** the one source of each enabled column's
      closure and requested calculations and of a row's display name; it knows
      nothing of the demand layer's use of them. **The logbook index:** notes
      each stored result's outcome and announces a changed one as a record
      change. **The result store:** writes the record of an `Ok` result, or
      announces that it could not.
    - **The views:** present the demand layer's state through one glyph and one
      clock; they never run a pass, offer, or write.
    - **The flow is one way:** the demand layer chooses; the executor
      publishes into the loaded session; the store writes the record or
      announces that it could not; the index notes the outcome and announces
      it; the record change drops the cached column values and the demand
      layer's memos, and the loaded-row refresh recomputes the values; the
      demand layer sees the result, or the failure, through the blocker
      inspection, the record names and the pair memory it always reads, and
      the pair leaves demand. No component infers another's change from a
      signal about something else.
    - Keep **What stays true** and the audit sentence.
13. **New §16.12 "The parts of the demand layer"**, after 16.11:
    - `src/demandstate.h`: the presentation values (`DemandCondition`,
      `DemandTrack`, `DemandState`, the tooltip and its limit); plain values
      with no behaviour beyond counting a track and building the text; the
      views depend on these and on the component's read interface only.
    - `src/demandfill.h`, `DemandFill`: given, after each pass, the set of
      sessions with a pending column cell and the load candidates in order, it
      keeps at most `kMaxHeldSessions` hidden sessions loaded and pinned for
      column demand, releases a hold when its session has no pending cell or no
      loaded row, loads the next candidate when a hold is free and the
      scheduler steps it, reports progress as the sessions remaining of the
      high-water mark, and holds nothing once the executor is shut down or the
      component goes. It speaks of session ids only, never calls the executor
      (it learns of a shutdown through its owner's hook) and calls back only
      through its owner's hooks.
    - `src/demandsettleclock.h`, `DemandSettleClock`: the per-session deadlines
      of the input-settle wait and one single-shot timer for the earliest; it
      answers whether a session is settling and when the next wait ends, and
      calls its owner when a wait ends. What starts a wait is the owner's
      decision.
    - `src/calculationdemand.h`, the reconciler: the walk, the classification
      rules, the pair memory, the choice and the pass. The walk reads the rows
      under one guard, asks the settle clock and reads a copy of the holds; it
      calls neither the executor nor the fill, never loads, pins or emits, and
      returns every state, candidate and learned fact as plain values. After it
      the pass applies the learned facts, gives the fill the pending sessions
      and load candidates, offers, and announces.
    - The cleanup audit (group `demand`) keeps these boundaries: only the fill
      loads and pins, the fill and the settle clock never call the executor and
      know nothing of the walk, one guard per pass.
14. **Other scheduler passages.** §15.5 lines 937-939 stay true. Line
    1363-1364 (§16.4 Start-up) stays true. There is no other sentence that says
    a task completes by stepping (grep "step" in the file after the edit; the
    §16.8 rewrite and the §16.9 row are the only scheduler descriptions).

**Acceptance Criteria:**
- [ ] §16 contains no "settlement", "settled" in the sense of the removed memory, `settling` as a field, `waiting` as a listed field, `progressLabel`, `idle()`, "one last step", "loads nothing, reports", "under the same label", `CalculationDemand::kMaxHeldSessions` or `CalculationDemand::buildToolTip`.
- [ ] 16.1 states the unloaded order of spec §10.1 as implemented (six steps, the column verdict per column); 16.7 states the memory's kinds, origins and every clearing rule (no display change among them) and the load forgetting; 16.8 states the completion rule scoped to tasks that can wait; 16.10 states one glyph, one clock and the fill's text; 16.12 exists.
- [ ] Every cross-reference in the file ("16.x") still points to the subsection it names.

**Complexity:** L

---

### Task 5.3: `docs/COMPUTED_PLOTS.md`, `docs/DATA_SCHEMA.md`, `docs/SENSOR_FUSION.md`, root `README.md`

**Purpose:** Spec §15 items 2-3 and every other passage Phases 1-4 made false.

**Files to modify:**
- `docs/COMPUTED_PLOTS.md`
- `docs/DATA_SCHEMA.md` (sections 11 and 12)
- `docs/SENSOR_FUSION.md` (one sentence)
- `README.md` (source tree, documentation list)

**Technical Approach:**

1. **COMPUTED_PLOTS §2, lines 32-39.** Table:

   | You see | It means |
   | --- | --- |
   | A turning arc | Computing. The tooltip says how many of the visible tracks that can be computed for this plot are done (a track that could not be computed counts as done) |
   | A warning triangle | Computing has finished, and some visible tracks could not be computed (section 7); the tooltip says which |
   | Nothing | (unchanged) |

   Keep "The arc and the triangle are never shown together". Add: "The row
   shows no number: hover over it for the counts." Lines 41-47 (the tooltip's
   two parts) unchanged.
2. **COMPUTED_PLOTS §3, line 71.** "and the count on the row rises" becomes
   "and the numbers in the row's tooltip rise".
3. **COMPUTED_PLOTS §4, lines 86-87.** "The progress line under the logbook
   shows "Computing columns: k / n" for the whole fill, with no cancel button."
   becomes: "The progress line under the logbook shows "Computing results:
   k / n" for the whole fill, with no cancel button. The logbook's other
   background work, such as filling a column that needs no computing ("Computing
   columns: k / n"), saving or loading, goes first." Lines 98-99 "counted in
   the header's numbers" becomes "counted in the numbers of the header's
   tooltip".
4. **COMPUTED_PLOTS §7.** Line 157-159: "A few failures are not about the
   data: the computer ran out of memory, the recording's file could not be
   read, or a computed result could not be kept (the disk is full, or the
   logbook's `cache/` folder cannot be written)." Add one paragraph after it: a
   result that could not be kept is listed the same way, with the reason
   ("Couldn't write file ..."): while the recording stays loaded the plot still
   draws it, but the plot row and the column header show the warning triangle,
   the recording is not computed again while FlySight Viewer runs unless its
   data change, and the next start computes and keeps it again.
5. **COMPUTED_PLOTS §8.** Add after the first sentence: "The turning arcs of
   the plot list and of the logbook's column headers turn together; they stop
   when nothing is being computed."
6. **DATA_SCHEMA §11, lines 352-364** ("Each session entry may also have
   `"recordReasons"` ..."). After "as the application learned it when the
   record was written or last restored" add: when a restore (a load, or the
   background worker's temporary copy) learns a reason that differs from what
   the index held - a record of an earlier build, or an index whose
   `"recordReasons"` was lost - the index records it and announces it as a
   change of that record, so the logbook lists the session as "could not be
   computed" with that reason at once, without loading it, and the cached
   column values over that record are computed again once, with the same
   values; learning the same reason again changes nothing. Keep the rest of the
   paragraph and the JSON example.
7. **DATA_SCHEMA §12, lines 573-583.**
   - "Failures that are not a function of the inputs - running out of memory, a
     worker that could not be started, a session file that could not be
     loaded, a result whose record could not be written - are never stored:
     they are shown until the application closes (or the recording's inputs
     change) and tried again at the next start."
   - Line 582-583 becomes: "A write that fails (a full disk, say) leaves the
     previous record, if any, intact and the result in memory. The recording is
     then listed as "could not be computed" with the write's reason for the
     rest of the run, whether or not it stays loaded, and the calculation is
     not run again for it until its inputs change; nothing about the failure is
     stored, so the next start tries again ([COMPUTED_PLOTS.md](COMPUTED_PLOTS.md),
     section 7)."
8. **SENSOR_FUSION.md line 385-388** ("Running out of memory, or failing to
   start the worker, is not a function of the inputs ..."): "Running out of
   memory, failing to start the worker, or failing to write the fit's stored
   copy is not a function of the inputs and is never cached as a failure: it
   is shown with the warning badge and its reason, ..." (rest unchanged).
9. **README.md source tree, lines 367-374.**
   ```
   │   ├── calculationdemand.*                # The demand layer: what checked plots and
   │   │                                      #   enabled columns need computed, what runs next
   │   ├── demandstate.*, demandfill.*,       # Its parts: the values the views present, the
   │   │   demandsettleclock.*                #   column fill (hidden loads), the input-settle clock
   ```
   and in the `ui/` comment: "(ui/docks: plotselection/PlotRow*, DemandIndicator.*,
   DemandIndicatorView.*, logbook/LogbookHeaderView.*, LogbookCellDelegate.*:
   working indicator and its one clock, badge, pending cells)". Keep the
   column alignment of the tree.
10. **README.md "User Documentation", COMPUTED_PLOTS line:** "the working
    indicator and its count" becomes "the working indicator and its tooltip".

**Acceptance Criteria:**
- [ ] `docs/COMPUTED_PLOTS.md` contains no `"k of n"` beside the arc, no "triangle with a number", no "count on the row", and names "Computing results: k / n" for the fill.
- [ ] DATA_SCHEMA 11 says when the index learns a reason and that it announces it; DATA_SCHEMA 12 lists the failed write among the failures shown and not stored.
- [ ] `README.md` lists the three part files and `DemandIndicatorView.*`.

**Complexity:** M

---

### Task 5.4: `tests/acceptance_map.txt`: items 601-662 and the amended items

**Purpose:** Make the specification's testable statements and §14's bullets
traceable, and restate the demand-driven items whose meaning changed.

**Files to modify:**
- `tests/acceptance_map.txt`

**Technical Approach:**

1. **Head comment (lines 1-44).**
   - "Six specifications, six item ranges" -> "Seven specifications, seven item
     ranges".
   - The 501-563 entry gains: "It is amended by the specification of 601-662:
     items 513, 526, 528, 530, 532, 535-537, 539, 546, 547, 557 and 559 are
     stated as amended."
   - New entry after it:
     ```
     #   601-662   calculation refinements (the scheduler's completion of a
     #             task whose work is gone, announced record outcomes, the
     #             session model as the source of column knowledge and display
     #             names, one walk and one pair memory in the demand layer and
     #             its parts, one indicator, one clock, the fill's progress
     #             text): item = 600 + clause number. Stated in full in
     #             tests/README.md, appendix G. It amends the specification of
     #             501-563.
     ```
   - "Every item must lie in one of the seven ranges; ... every item 101-120,
     201-247, 301-350, 401-442, 501-563 or 601-662 needs at least one test or
     audit line ... The human-readable matrices are tests/README.md sections 9.1
     to 9.7".
2. **Amended items.** Rewrite each item's comment line in place as
   `# <item> - (<section>, as amended) <clause>` and append the lines listed,
   after the item's existing lines (keep every existing line):

   ```
   # 513 - (5, as amended) whether a result exists: blocker inspection for a loaded session; for a session not loaded, first what this run remembers of its pairs (a failure; a not-applicable verdict of that column), then the logbook's record names and the reasons the index recorded for them, no record opened; a cell has a result only when every calculation it needs has a record; a record with a reason is a failed result before and after a restart alike; a reason the index learns later is announced as a record change; a known record counts until the column worker's restore deletes it as stale, which moves the pair into demand; the demand layer checks no staleness itself
   513 tst_calculation_demand recordReasonReachesDemandThroughRecordChange
   513 tst_calculation_demand failedRecordWriteIsShownAndNotRetried
   513 tst_logbook_index recordReasonChangeIsAnnounced

   # 526 - (8, as amended) a failure that is not a function of the inputs (the worker could not start, out of memory, a session file that could not be loaded, a result whose record could not be written) is badged with its reason, not offered and its session not loaded again in the run unless its inputs change, not stored, so tried again at the next start; the demand layer remembers it for the pair
   526 tst_calculation_demand failedRecordWriteIsShownAndNotRetried
   526 tst_calculation_demand settledPairsSurviveEvictionSortAndColumnWorker
   526 tst_calculation_demand pairMemoryIsClearedByRecordInputAndRegistryChanges
   526 manual M32

   # 528 - (9, as amended) for column demand the demand layer, not the column worker, has a session that is not loaded loaded the way showing it would, without making it visible: an ordinary hidden session, pinned from its load until no column it needs is still waiting or running, the executor is shut down or the demand layer goes, then left to ordinary eviction
   (no new line: noLoadsAfterExecutorShutdown and demandDestroyedReleasesHoldsAndTask, already cited, assert it since Phase 3)

   # 530 - (9, as amended) the fill is an idle-scheduler task below saving, loading visible sessions, bulk edits and column work, whose steps are the loads, so saves and bulk edits come first; it has work for the whole fill and steps only while it can load; the progress line reports the whole fill; the scheduler has the generic notion of a task with work it cannot step right now, rests instead of spinning, completes such a task once its work is gone without a step, and learns nothing about jobs
   530 tst_session_model_engine schedulerCompletesWaitingTaskWhoseWorkIsGone
   530 tst_calculation_demand fillEndingBehindAnotherTaskStartsNextCountFresh

   # 532 - (9, as amended) a session whose requested calculation turns out not to apply once loaded is remembered as not applicable for that column for the run, without a job, and is not loaded again for it; its column value stays unavailable
   532 tst_calculation_demand columnVerdictDoesNotSuppressAnotherColumn
   532 tst_calculation_demand settledPairsSurviveEvictionSortAndColumnWorker

   # 535 - (10, as amended) a plot row and a logbook column header show one small animated working indicator at the right of their name while any of their demand is waiting or running, with no label or count beside it; the indicators of the plot list and the logbook turn on one clock
   535 tst_logbook_indicators plotRowsAndHeaderTurnOnOneClock
   535 tst_plot_row_layout indicatorOnly
   535 manual M29
   535 manual M30

   # 536 - (10, as amended) hovering the indicator, the row or the header shows how many sessions are done of how many are wanted (the only place the numbers are shown), the session being computed with its progress text, and the sessions that could not be computed with their reasons, a result that could not be stored included
   536 tst_logbook_indicators failedWriteIsListedInTheHover
   536 tst_plot_row_delegate sharedGlyphPlumbing
   536 manual M29

   # 537 - (10, as amended) the warning badge, with no count, replaces the indicator once work is finished and some sessions could not be computed, a result that could not be stored included; its hover lists them with reasons
   537 tst_logbook_indicators failedWriteIsListedInTheHover
   537 tst_calculation_demand failedRecordWriteIsShownOnThePlotRow
   537 manual M29

   # 539 - (10, as amended) the logbook's progress line for background work is unchanged in form and reports a column fill for its whole duration, remaining of wanted, with no cancel, under a text of its own ("Computing results: k / n") distinct from the column worker's ("Computing columns: k / n")
   539 tst_logbook_indicators fillProgressLineHasItsOwnText
   539 manual M31

   # 546 - (12, as amended) the column worker and the idle scheduler keep their tasks and priorities; the column fill is a new lowest-priority task; the scheduler has the notion of a task with work it cannot step right now, completes it when its work is gone, and knows nothing of jobs; the manager and the store record each stored result's outcome in the index, the manager announces a changed outcome, and the store announces a record it could not write
   546 tst_session_model_engine schedulerCompletesWaitingTaskWhoseWorkIsGone
   546 tst_logbook_index recordReasonChangeIsAnnounced
   546 tst_result_store writeFailureIsAnnounced

   # 547 - (12, as amended) the flow is one way: the demand layer chooses, the executor publishes, the listener writes the record or the store announces that it could not, the index notes the outcome and announces it, the record change drops cached column values and the demand layer's memos, the loaded-row refresh recomputes the values, and the demand layer sees the result or the failure through the inspection, the record set and the pair memory it always reads
   547 tst_calculation_demand recordReasonReachesDemandThroughRecordChange
   547 tst_calculation_demand failedRecordWriteIsShownAndNotRetried

   # 557 - (13, as amended) test: the working indicator and hover detail reflect waiting, running, done and failed counts on plot rows and column headers, the plot row showing one glyph and no label or count; no control to refresh or cancel a calculation exists
   557 tst_plot_row_delegate workingRowPaintsIndicator
   557 tst_plot_row_delegate sharedGlyphPlumbing
   557 tst_logbook_indicators failedWriteIsListedInTheHover

   # 559 - (13, as amended) test: saves and bulk edits still precede the fill's loads; no result is computed from a file being rewritten; the progress line reports the fill from its first load to its last result without any step that loads nothing, and the scheduler does not spin while the fill waits on a job
   559 tst_calculation_demand fillTaskReportsProgressWhileWaiting
   559 tst_session_model_engine schedulerCompletesWaitingTaskWhoseWorkIsGone
   559 tst_session_model_engine schedulerWaitingTaskDoesNotSpin
   559 tst_logbook_indicators fillProgressLineHasItsOwnText
   ```
   The "(no new line ...)" note under 528 is for the implementer, not for the
   file. Item 562 keeps its text (Phase 4 already deleted its
   `indicatorAndWarning` line).
3. **New range**, appended at the end of the file after a separator line
   `# ---- Calculation refinements: item = 600 + clause number ----`. Each block
   is the comment line `# <item> - (<section>) <clause>` (sections as below)
   followed by its lines, a blank line between blocks:

   ```
   # 601 - (2) the principles of the demand-driven specification hold unchanged: what is switched on is the request, anything wanted is wanted at once, finished work is never wasted, background work never degrades the application, a failure is shown and never retried in a loop, each component's contract can be stated without naming the others
   601 tst_calculation_demand rowScript
   601 tst_calculation_demand enablingColumnFillsEveryUnloadedSession
   601 tst_calculation_demand storedResultsCreateNoJob
   601 tst_calculation_demand nullCollaborators
   601 tst_calculation_demand failedRecordWriteIsShownAndNotRetried
   601 tst_jobqueue workerRunsBelowNormalPriority
   601 audit demand

   # 602 - (2) a fact is computed by the component that owns it and read by the others: each enabled column's closure and requested calculations and a row's display name by the session model, a record's reason by the logbook index, a failed record write by the result store
   602 tst_result_columns sessionModelExposesColumnKnowledge
   602 tst_result_columns sessionDisplayNameOfEveryRowKind
   602 tst_calculation_demand columnKnowledgeComesFromTheSessionModel
   602 tst_logbook_index recordReasonChangeIsAnnounced
   602 tst_result_store writeFailureIsAnnounced
   602 audit demand

   # 603 - (2) a component announces what it changes; no component infers another's change from a signal about something else
   603 tst_logbook_index recordReasonChangeIsAnnounced
   603 tst_result_store writeFailureIsAnnounced
   603 tst_calculation_demand recordReasonReachesDemandThroughRecordChange
   603 audit demand

   # 604 - (2) one rule, one path: plots and columns are classified, tallied and filed as candidates by one walk, the difference between them being data
   604 tst_calculation_demand runningColumnTrackFilesItsOtherBlockers
   604 tst_calculation_demand focusedSessionFirstThenRowOrder
   604 tst_calculation_demand columnPriorityFollowsRowOrderAfterPlots
   604 tst_calculation_demand columnStateCountsAndPendingCells
   604 audit demand

   # 605 - (2) what no product code uses is removed, unless a named later feature needs it, in which case it is kept and says so: the executor's cancel operation and job history stay for the jobs dock
   605 tst_jobqueue cancelRunningThenNextStarts
   605 tst_jobmodel historyFromSignalsAlone
   605 audit gestures
   605 audit demand

   # 606 - (3) unchanged: the executor's lifecycle, the fusion kernel, the record format, the logbook index's other contents, and the column worker's behaviour and statistics
   606 tst_jobqueue runsAndPublishes
   606 tst_jobqueue staleRunningJobIsStoppedAtOnce
   606 tst_jobqueue shutdownWithQueuedAndRunning
   606 tst_fusion_golden successFixturesMatchGolden
   606 tst_result_records layoutIsPinned
   606 tst_logbook_index recordReasonsRoundTrip
   606 tst_result_columns columnWorkerIsUnchangedByDemand

   # 607 - (3) unchanged: what is in demand, its priority, the input-settle wait, the bound on held sessions, and the one-way flow between the demand layer, the executor, the store and the column worker
   607 tst_calculation_demand focusedSessionFirstThenRowOrder
   607 tst_calculation_demand visibleSessionsFirstWithinColumnDemand
   607 tst_calculation_demand inputBurstRunsOneJob
   607 tst_calculation_demand supersededJobIsRunAgainAfterInputsSettle
   607 tst_calculation_demand enablingColumnFillsEveryUnloadedSession
   607 tst_result_columns staleRecordDeletedByWorkerCreatesDemand
   607 audit demand

   # 608 - (3) the jobs dock is out of scope: the executor keeps its cancel operation and its job history for it; the column worker does not load sessions for the demand layer
   608 tst_jobqueue cancelQueued
   608 tst_jobqueue cancelRunningThenNextStarts
   608 tst_jobmodel historyFromSignalsAlone
   608 tst_result_columns columnWorkerIsUnchangedByDemand
   608 audit gestures

   # 609 - (5) a task registered as one that can wait (TaskDef::canStep) which the scheduler last reported active and which has no work any more is completed at the next tick whether or not it was stepped: its progress is reported one last time and its completion is called, not cancelled, before the next active task is reported or the scheduler goes idle; whoever takes a resting task's work away wakes the scheduler
   609 tst_session_model_engine schedulerCompletesWaitingTaskWhoseWorkIsGone
   609 tst_calculation_demand fillTaskReportsProgressWhileWaiting

   # 610 - (5) a task that lost its work because it was cancelled completes once, as cancelled; a task unregistered while active is never completed
   610 tst_session_model_engine schedulerCompletesWaitingTaskWhoseWorkIsGone
   610 tst_session_model_engine schedulerTaskCanBeUnregistered

   # 611 - (5) the other tasks (save, load, bulk edit, column work) complete through their step or by cancel at the same moments as before: they can lose work outside a step, so the rule applies to tasks that can wait only
   611 tst_session_model_engine schedulerCompletesWaitingTaskWhoseWorkIsGone
   611 tst_session_model_engine schedulerWaitingTaskDoesNotSpin
   611 tst_calculation_demand savesAndBulkEditsPrecedeLoadStep
   611 tst_calculation_demand startupWithEnabledColumnLoadsAfterColumnWorker

   # 612 - (5) the fill has no ending state and no step that loads nothing; its progress is the sessions with a pending column cell of its high-water mark, which resets when the task completes and starts afresh when the sessions with a pending cell rise from none
   612 tst_calculation_demand fillTaskReportsProgressWhileWaiting
   612 tst_calculation_demand fillEndingBehindAnotherTaskStartsNextCountFresh
   612 tst_calculation_demand fillTaskIsLowestAndNotCancellable
   612 audit demand

   # 613 - (5) the scheduler still learns nothing about jobs or demand
   613 tst_session_model_engine schedulerCompletesWaitingTaskWhoseWorkIsGone
   613 audit demand
   613 audit branch-mechanisms

   # 614 - (6) recording a reason that differs from what the logbook index held emits the record-changed signal for that session and calculation, as a record write or removal does; an unchanged reason emits nothing, and a write or removal still emits exactly once
   614 tst_logbook_index recordReasonChangeIsAnnounced
   614 tst_result_store recordReasonRecordedAtWriteAndRestore
   614 tst_result_columns managerDropsDependentValues

   # 615 - (6) a reason taught to the index by a restore into the column worker's copy reaches the demand layer through that signal alone; the demand layer compares no reasons on the model's display change
   615 tst_calculation_demand recordReasonReachesDemandThroughRecordChange
   615 tst_calculation_demand storedRejectionIsBadgedAfterRestartWithoutLoad
   615 audit demand

   # 616 - (6) the bulk edit announces its edit as a dependency change on both of its paths (a loaded session, and the temporary copy of one that is not loaded), and the demand layer observes no display change of the model: a bulk edit reaches it as an input change, and the column worker's display changes are never read
   616 tst_session_model_engine bulkEditAnnouncesADependencyChange
   616 tst_calculation_demand bulkEditMakesSettledSessionApplicable
   616 tst_calculation_demand settledPairsSurviveEvictionSortAndColumnWorker
   616 audit demand

   # 617 - (7) the result store announces a record write that failed, with the reason, for the session and calculation, after the index's record change of that pair; the result stays installed and nothing retries the write
   617 tst_result_store writeFailureIsAnnounced
   617 tst_result_store writeFailureLeavesResultUsable
   617 tst_result_store writeFailureKeepsPreviousRecord

   # 618 - (7) a track whose record could not be written is failed with that reason on plot rows and column headers alike, whether the session is loaded or not: a source is done only when none of its storable calculations is remembered so
   618 tst_calculation_demand failedRecordWriteIsShownAndNotRetried
   618 tst_calculation_demand failedRecordWriteIsShownOnThePlotRow
   618 tst_logbook_indicators failedWriteIsListedInTheHover
   618 manual M32

   # 619 - (7) such a pair is not offered again and its session is not loaded again for it in the run until its inputs change, so the user's view is the same before and after an eviction
   619 tst_calculation_demand failedRecordWriteIsShownAndNotRetried
   619 tst_calculation_demand failedRecordWriteIsShownOnThePlotRow
   619 tst_calculation_demand settledPairsSurviveEvictionSortAndColumnWorker
   619 manual M32

   # 620 - (7) a later successful write of the pair's record clears the failure; nothing is persisted, so the next start tries the write again
   620 tst_calculation_demand failedRecordWriteIsShownAndNotRetried
   620 tst_calculation_demand pairMemoryIsClearedByRecordInputAndRegistryChanges
   620 manual M32

   # 621 - (8) the session model exposes, per enabled column, its static dependency closure and its requested calculations, current under the registrations at the moment of the call and valid between two column rebuilds
   621 tst_result_columns sessionModelExposesColumnKnowledge
   621 tst_result_columns columnExplicitCalculations

   # 622 - (8) the demand layer reads each column's closure and requested calculations from the session model and computes neither; a column change reaches it through the model's reset alone; it still computes a plot's closure and requested calculations from the registry
   622 tst_calculation_demand columnKnowledgeComesFromTheSessionModel
   622 tst_calculation_demand ordinaryColumnsCreateNoDemand
   622 tst_calculation_demand ordinaryPlotsAreNeverInspected
   622 audit demand

   # 623 - (8) the session model exposes one display name of a row: the loaded session's description, else the description the logbook index caches for the row, else the session id
   623 tst_result_columns sessionDisplayNameOfEveryRowKind

   # 624 - (8) the demand layer's tracks and the executor's job records name a session by that display name, for loaded rows, stubs and failed-load placeholders alike, and neither computes a name
   624 tst_jobqueue runsAndPublishes
   624 tst_calculation_demand visibleFailedLoadIsSettledAsFailed
   624 tst_calculation_demand recordReasonReachesDemandThroughRecordChange
   624 tst_calculation_demand failedRecordWriteIsShownAndNotRetried
   624 audit demand

   # 625 - (9) the demand layer keeps no memory of its own offer: it is the only offerer, so the chosen next job is always its own, and when the choice finds nothing it is withdrawn
   625 tst_calculation_demand chosenNextJobFollowsTheExecutorsAnswer
   625 tst_calculation_demand changingDemandReplacesChosenNext
   625 tst_calculation_demand hidingASessionDropsItsWaitingPair
   625 audit gestures

   # 626 - (9) the demand layer does not compare a candidate with the chosen next job before offering it; it acts on the executor's answer, and an offer equal to the chosen next job keeps it as it is
   626 tst_calculation_demand chosenNextJobFollowsTheExecutorsAnswer
   626 tst_calculation_demand executorHoldsAtMostRunningAndChosenNext
   626 tst_jobqueue duplicateOffersCreateNoDuplicates

   # 627 - (9) a track has no settling flag and no job id, and a source's state no list of waiting tracks; the counts stay, and nothing the user sees changes
   627 tst_calculation_demand columnStateCountsAndPendingCells
   627 tst_calculation_demand tooltipText
   627 tst_calculation_demand changeSignalsAreMinimal
   627 tst_calculation_demand progressUpdatesWithoutInspection
   627 audit demand

   # 628 - (9) the executor has no idle signal, no queued signal, no query of both active jobs and no busy-period bookkeeping; its job model, its idle and running queries and its other signals carry the same facts
   628 tst_jobqueue runsAndPublishes
   628 tst_jobqueue holdsAtMostRunningAndChosenNext
   628 tst_jobqueue offerReplacesChosenNext
   628 tst_jobqueue cancelFromRowsInsertedLeavesNoPin
   628 tst_jobmodel historyFromSignalsAlone
   628 audit gestures

   # 629 - (9) the executor keeps its cancel operation for the jobs dock; no product code calls it, and its comment and the audit say so
   629 tst_jobqueue cancelRunningThenNextStarts
   629 tst_jobqueue cancelQueued
   629 audit gestures

   # 630 - (9) the executor's refusal kinds are unchanged; the demand layer offers upstream first, so a Blocked refusal is not expected from it, and the documentation says so instead of the code guarding for it
   630 tst_jobqueue refusesBlockedAndDone
   630 tst_jobqueue refusesMissingInput
   630 tst_calculation_demand chainedBlockersContinue
   630 tst_calculation_demand columnOfferRefusalIsNotLeftPending

   # 631 - (10.1) one walk over the session rows derives every source's state, counts, listed tracks, pending cells and candidates, for plots and columns at once, under one row stability guard, returning plain values; offers, withdrawals, holds, loads and signals happen after it
   631 tst_calculation_demand changeSignalsAreMinimal
   631 tst_calculation_demand columnStateCountsAndPendingCells
   631 tst_calculation_demand workingIdsFollowStates
   631 tst_calculation_demand passOverManyStubsReadsEachRecordSetOnce
   631 audit demand

   # 632 - (10.1) a loaded session is classified from the engine's blocker inspection of the source's names, combined alike for a plot's one name and a column's names, the running job, the pair memory and the settle wait; the combined report is memoized per session and source and dropped by an input change or publication, a load, a record change, a job's end, a single-row display change, a reset or a registry change
   632 tst_calculation_demand hiddenAndStubRowsAreNotTracks
   632 tst_calculation_demand ordinaryPlotsAreNeverInspected
   632 tst_calculation_demand uncheckedPlotsAreNeverInspected
   632 tst_calculation_demand progressUpdatesWithoutInspection
   632 tst_calculation_demand chainedBlockersContinue
   632 tst_calculation_demand registryChangeReclassifies

   # 633 - (10.1) a session that is not loaded is a track of columns only, classified in this order: no storable calculation, not applicable; a storable calculation remembered failed (a job, a load or a write that failed), failed with the remembered reason; every storable calculation remembered not applicable for that column, not applicable; every storable calculation with a record, done, or failed with the first recorded reason; a failed-load placeholder, failed ("The session file could not be loaded"); otherwise waiting
   633 tst_calculation_demand storedRejectionIsBadgedAfterRestartWithoutLoad
   633 tst_calculation_demand unloadableSessionIsSettledAsFailed
   633 tst_calculation_demand failedRecordWriteIsShownAndNotRetried
   633 tst_calculation_demand columnVerdictDoesNotSuppressAnotherColumn
   633 tst_calculation_demand passOverManyStubsReadsEachRecordSetOnce
   633 tst_logbook_indicators failedLoadSessionShowsBadgeNotPending

   # 634 - (10.1) candidates are filed in the walk by tier: the focused session's plot pairs, the other visible sessions' plot pairs, then the column pairs of visible loaded sessions, then of hidden loaded sessions, each in row order; within a session source order, then blocker order; a pair once, in its first tier; never a pair with a remembered failure or refusal, the running job not asked to stop, or a pair of a settling session; a column track running or failed on one blocker files its other blockers, as a plot track does
   634 tst_calculation_demand focusedSessionFirstThenRowOrder
   634 tst_calculation_demand columnPriorityFollowsRowOrderAfterPlots
   634 tst_calculation_demand visibleSessionsFirstWithinColumnDemand
   634 tst_calculation_demand sessionShownDuringColumnDemandRunsNext
   634 tst_calculation_demand runningColumnTrackFilesItsOtherBlockers
   634 tst_calculation_demand inputBurstRunsOneJob

   # 635 - (10.1) the load candidates are the waiting, hidden, unloaded sessions that are not settling and not held, in row order, up to the bound
   635 tst_calculation_demand enablingColumnFillsEveryUnloadedSession
   635 tst_calculation_demand sessionShownDuringColumnDemandRunsNext
   635 tst_calculation_demand heldSessionShownStaysLoaded
   635 tst_calculation_demand chainedColumnKeepsItsHold

   # 636 - (10.2) one memory of the run, keyed by pair (session, requested calculation), holds not applicable (the executor refused the pair, or a loaded column found its source not applicable) or failed with a reason (a job that ended failed, a result that is never stored, a load that failed, a record write that failed); there is no memory per cell, and every verdict is derived from the engine or the record set and this memory
   636 tst_calculation_demand settledPairsSurviveEvictionSortAndColumnWorker
   636 tst_calculation_demand notApplicableSessionIsSettledWithoutAJob
   636 tst_calculation_demand columnFailuresAreBadgedNotReloaded
   636 tst_calculation_demand columnJobLevelFailureIsNotReloadedUntilRestart
   636 tst_calculation_demand columnOfferRefusalIsNotLeftPending
   636 tst_calculation_demand unloadableSessionIsSettledAsFailed
   636 audit demand

   # 637 - (10.2) a column's not-applicable verdict holds for that column only: it keeps its session from being loaded again for that column, and it never makes another source's track not applicable nor keeps a pair from being offered
   637 tst_calculation_demand columnVerdictDoesNotSuppressAnotherColumn
   637 tst_calculation_demand notApplicableSessionIsSettledWithoutAJob

   # 638 - (10.2) the memory is cleared for a session by a relevant input change, for a pair by a record change of that pair, for everything by a registry change; a model reset forgets the pairs of sessions that no longer have a row; nothing is persisted
   638 tst_calculation_demand pairMemoryIsClearedByRecordInputAndRegistryChanges
   638 tst_calculation_demand removedSessionLeavesNoTrace
   638 tst_calculation_demand registryChangeReclassifies
   638 tst_calculation_demand jobLevelFailureIsBadgedNotRerunUntilRestart

   # 639 - (10.2) a bulk edit of a session clears what the run remembered of it, through the dependency change it publishes, for a loaded session and for one that is not loaded alike; nothing else clears a session's facts, the column worker's display changes in particular; a load of the session that succeeds forgets its failed-load facts
   639 tst_calculation_demand bulkEditMakesSettledSessionApplicable
   639 tst_calculation_demand settledPairsSurviveEvictionSortAndColumnWorker
   639 tst_calculation_demand failedRecordWriteIsShownAndNotRetried
   639 tst_calculation_demand successfulLoadForgetsFailedLoadFacts
   639 manual M33

   # 640 - (10.2) a session found not applicable, a job-level failure, a failed load and a failed write are not loaded or offered again after an eviction, a sort, or the column worker's processing of the stub
   640 tst_calculation_demand settledPairsSurviveEvictionSortAndColumnWorker
   640 tst_calculation_demand notApplicableSessionIsSettledWithoutAJob
   640 tst_calculation_demand columnJobLevelFailureIsNotReloadedUntilRestart
   640 tst_calculation_demand unloadableSessionIsSettledAsFailed

   # 641 - (11) the demand layer stays one component with one contract towards the views and the executor, divided into parts with contracts of their own (the presentation values, the fill, the settle clock and the reconciler); the fill and the settle clock expose nothing of the walk, and the walk calls neither the executor nor the session model's loading and pinning
   641 tst_calculation_demand settleClockAnswersItsQuestions
   641 tst_calculation_demand nullCollaborators
   641 audit demand

   # 642 - (11) the fill keeps at most the bound of hidden sessions loaded and pinned for column demand, releases a hold when its session has no pending cell or no loaded row, loads the next candidate when a hold is free and the scheduler steps it, reports the sessions remaining of its high-water mark, and holds nothing once the executor is shut down or the component goes
   642 tst_calculation_demand enablingColumnFillsEveryUnloadedSession
   642 tst_calculation_demand disablingColumnReleasesHeldSessions
   642 tst_calculation_demand removedOrRepopulatedHeldSessionIsReleased
   642 tst_calculation_demand noLoadsAfterExecutorShutdown
   642 tst_calculation_demand demandDestroyedReleasesHoldsAndTask
   642 tst_calculation_demand fillTaskReportsProgressWhileWaiting

   # 643 - (11) the settle clock holds the per-session deadlines of the input-settle wait and one timer for the earliest, answers whether a session is settling and when the next wait ends, and calls its owner when a wait ends
   643 tst_calculation_demand settleClockAnswersItsQuestions
   643 tst_calculation_demand inputBurstRunsOneJob
   643 tst_calculation_demand supersededJobIsRunAgainAfterInputsSettle

   # 644 - (12) a plot row and a logbook column header show the same one glyph: the turning arc while any of the source's demand is waiting or running, the warning badge once the work is finished and some sessions could not be computed, and nothing else; the plot row has no "k of n" label and no count, and its layout keeps room for one glyph
   644 tst_plot_row_layout indicatorOnly
   644 tst_plot_row_layout nothingShown
   644 tst_plot_row_layout rightToLeftIsMirrorImage
   644 tst_plot_row_delegate workingRowPaintsIndicator
   644 tst_plot_row_delegate badgeReplacesIndicatorOnceFinished
   644 tst_plot_row_delegate longNameIsElidedNotTheCluster
   644 tst_logbook_indicators workingColumnShowsIndicatorRightOfText
   644 tst_logbook_indicators badgeReplacesIndicatorWhenFinished
   644 audit demand
   644 manual M29

   # 645 - (12) the hover carries both numbers, on a plot row as on a column header
   645 tst_plot_row_delegate hoverDetailFollowsDemandState
   645 tst_plot_row_delegate toolTipComesFromPlotState
   645 tst_logbook_indicators headerToolTipFollowsDemandState
   645 tst_calculation_demand tooltipText
   645 manual M29

   # 646 - (12) one working-indicator clock per application, created beside the demand layer and handed to the views as the demand layer is: the arcs of the plot list and the logbook's headers turn in step, the clock runs only while any plot or column is working, and each view repaints its own working rows or sections on its frames
   646 tst_logbook_indicators plotRowsAndHeaderTurnOnOneClock
   646 tst_logbook_indicators indicatorAnimatesOnlyWhileWorking
   646 tst_plot_row_delegate workingIndicatorAnimatesOnlyWhileWorking
   646 tst_plot_row_delegate workingAnimationClock
   646 tst_plot_row_delegate survivesDemandDestroyedFirst
   646 audit demand
   646 manual M30

   # 647 - (12) the logbook's progress line says "Computing results: k / n" while the fill is the active task, distinct from the column worker's "Computing columns: k / n"; the fill is not cancellable, and saves, loads, bulk edits and column work still come first
   647 tst_logbook_indicators fillProgressLineHasItsOwnText
   647 tst_calculation_demand fillTaskIsLowestAndNotCancellable
   647 tst_calculation_demand savesAndBulkEditsPrecedeLoadStep
   647 audit demand
   647 manual M31

   # 648 - (12) a stored result that could not be written is listed among the sessions that could not be computed, with its reason, on the plot row and the column header
   648 tst_logbook_indicators failedWriteIsListedInTheHover
   648 tst_calculation_demand failedRecordWriteIsShownOnThePlotRow
   648 manual M32

   # 649 - (12) nothing else the user sees changes: the hover detail, pending cells, the warning badge's meaning, and the absence of any refresh or cancel control
   649 tst_logbook_indicators pendingCellsAreDistinctFromUnavailable
   649 tst_logbook_indicators pendingCellBecomesValueWhenRecordIsWritten
   649 tst_logbook_indicators plainHeaderAndCellsAreIdenticalToBase
   649 tst_logbook_indicators clickOnIndicatorIsAClickOnTheSection
   649 tst_plot_row_delegate plainRowsAreIdenticalToBaseDelegate
   649 tst_plot_row_delegate clickOnClusterIsAClickOnTheRow
   649 audit demand

   # 650 - (12) the shared glyph plumbing owns the glyph's colour for a style option and its side and spacing beside a line of text, and the repaint when the demand layer is destroyed and the tooltip display are written once; the views compute none of them
   650 tst_plot_row_delegate sharedGlyphPlumbing
   650 tst_plot_row_delegate survivesDemandDestroyedFirst
   650 tst_logbook_indicators survivesDemandDestroyedFirst
   650 tst_logbook_indicators indicatorClearsSortArrowAndNarrowSections
   650 audit demand
   650 audit widget-free-core

   # 651 - (13) the demand layer remains widget-free, the only offerer and observed by nothing; the views present the same state through one glyph and one clock and decide nothing
   651 tst_calculation_demand nullCollaborators
   651 tst_calculation_demand survivesExecutorShutdown
   651 audit widget-free-core
   651 audit gestures
   651 audit demand

   # 652 - (13) the flow stays one way: the demand layer chooses; the executor publishes; the store writes the record or announces that it could not; the index notes the outcome and announces it; the record change drops the cached column values and the demand layer's memos; the demand layer sees the result, or the failure, through what it always reads
   652 tst_result_columns staleRecordDeletedByWorkerCreatesDemand
   652 tst_calculation_demand enablingColumnFillsEveryUnloadedSession
   652 tst_calculation_demand recordReasonReachesDemandThroughRecordChange
   652 tst_calculation_demand failedRecordWriteIsShownAndNotRetried
   652 tst_logbook_indicators pendingCellBecomesValueWhenRecordIsWritten
   652 audit demand

   # 653 - (14) test: a task with work it cannot step loses its work without a step and is completed once, not cancelled, after a final progress report, before the next active task is reported or the scheduler goes idle; the existing tasks complete at the same moments as before; the fill's progress line reports the fill from its first load to its last result without any step that loads nothing
   653 tst_session_model_engine schedulerCompletesWaitingTaskWhoseWorkIsGone
   653 tst_calculation_demand fillTaskReportsProgressWhileWaiting
   653 tst_calculation_demand fillEndingBehindAnotherTaskStartsNextCountFresh
   653 tst_calculation_demand savesAndBulkEditsPrecedeLoadStep

   # 654 - (14) test: a reason recorded by a restore into the column worker's copy reaches the demand layer through the manager's record-changed signal alone: a stored rejection of a session that is not loaded is badged with its reason after the worker's pass, and no reason comparison happens on the model's display change
   654 tst_calculation_demand recordReasonReachesDemandThroughRecordChange
   654 tst_calculation_demand storedRejectionIsBadgedAfterRestartWithoutLoad
   654 tst_logbook_index recordReasonChangeIsAnnounced

   # 655 - (14) test: a record write that fails is a shown failure: listed as failed with the reason while the session is loaded and after its eviction, not offered again in the run, cleared by a later successful write, tried again at a restart; no session is loaded twice for it
   655 tst_calculation_demand failedRecordWriteIsShownAndNotRetried
   655 tst_calculation_demand failedRecordWriteIsShownOnThePlotRow
   655 tst_result_store writeFailureIsAnnounced

   # 656 - (14) test: the demand layer reads each column's closure and requested calculations from the session model, and a column change reaches it through the model's reset alone; the tracks and the job records name a session by the model's display name, for loaded rows, stubs and failed-load placeholders alike
   656 tst_calculation_demand columnKnowledgeComesFromTheSessionModel
   656 tst_result_columns sessionModelExposesColumnKnowledge
   656 tst_result_columns sessionDisplayNameOfEveryRowKind
   656 tst_jobqueue runsAndPublishes
   656 tst_calculation_demand visibleFailedLoadIsSettledAsFailed

   # 657 - (14) test: plots and columns are classified, tallied and filed by one walk, and the acceptance items of the demand-driven specification on demand, priority, the settle wait, failures, not applicable, chains and unloaded sessions pass unchanged in what they assert: counts, listed running and failed tracks, pending cells and tooltips are as before
   657 tst_calculation_demand rowScript
   657 tst_calculation_demand enablingColumnFillsEveryUnloadedSession
   657 tst_calculation_demand focusedSessionFirstThenRowOrder
   657 tst_calculation_demand inputBurstRunsOneJob
   657 tst_calculation_demand columnFailuresAreBadgedNotReloaded
   657 tst_calculation_demand notApplicableSessionIsSettledWithoutAJob
   657 tst_calculation_demand chainedBlockersContinue
   657 tst_calculation_demand columnStateCountsAndPendingCells
   657 tst_calculation_demand runningColumnTrackFilesItsOtherBlockers
   657 tst_fusion_rows realRowScript

   # 658 - (14) test: every case the per-cell memory covered is covered by a pair fact: a session found not applicable, a job-level failure, a failed load and a failed write are not loaded or offered again after eviction, a sort or the column worker's pass; a bulk edit makes the session applicable again; a record change of the pair, an input change of the session and a registry change clear what is remembered
   658 tst_calculation_demand settledPairsSurviveEvictionSortAndColumnWorker
   658 tst_calculation_demand bulkEditMakesSettledSessionApplicable
   658 tst_calculation_demand pairMemoryIsClearedByRecordInputAndRegistryChanges
   658 tst_calculation_demand columnVerdictDoesNotSuppressAnotherColumn

   # 659 - (14) test: no product code reads the removed signals, queries and fields; the executor never holds more than the running and the chosen next job; the chosen next job is withdrawn when demand no longer wants it
   659 tst_calculation_demand chosenNextJobFollowsTheExecutorsAnswer
   659 tst_calculation_demand executorHoldsAtMostRunningAndChosenNext
   659 tst_calculation_demand changingDemandReplacesChosenNext
   659 tst_jobqueue holdsAtMostRunningAndChosenNext
   659 audit gestures
   659 audit demand

   # 660 - (14) test: the plot row shows the arc or the badge and no label or count; the two views' clocks are one; the fill's progress text is its own; a stored result that could not be written is listed in the hover
   660 tst_plot_row_delegate workingRowPaintsIndicator
   660 tst_plot_row_delegate badgeReplacesIndicatorOnceFinished
   660 tst_plot_row_delegate sharedGlyphPlumbing
   660 tst_plot_row_layout indicatorOnly
   660 tst_logbook_indicators plotRowsAndHeaderTurnOnOneClock
   660 tst_logbook_indicators fillProgressLineHasItsOwnText
   660 tst_logbook_indicators failedWriteIsListedInTheHover

   # 661 - (14) the audit's demand and gestures groups keep the removed and moved names out, and a rule forbids a second computation of a column's requested calculations outside the session model and the registry
   661 audit demand
   661 audit gestures

   # 662 - (15) docs/ and tests/README.md describe the executor's signals and queries (cancel kept for the jobs dock), the one walk, the one memory, the parts of the demand layer, the fill's completion, the failed write, the session model as the source of column knowledge and display names, one glyph and one clock, and the fill's progress text
   662 audit demand
   ```

   The clause text of each comment line is also the sentence of appendix G and
   the "Clause" cell of section 9.7 (Task 5.6). Write the "k of n" and the
   progress texts with plain double quotes, as above; the map's parser only
   reads the item number and the evidence lines.

**Acceptance Criteria:**
- [ ] Every line of the file matches one of the four forms or is a comment or blank (the audit's check).
- [ ] Items 601-662 each have at least one test or audit line; every cited function exists as `::<function>()` in `tests/<target>.cpp`; every cited manual step M29-M33 exists as `**M<k> ` in `tests/README.md`.
- [ ] The thirteen amended items read "(…, as amended)" and keep every line they had.

**Complexity:** L

---

### Task 5.5: `tests/audit/cleanup_audit.cmake`: the `gestures` and `demand` groups, the range 601-662

**Purpose:** Spec §14 last bullet: the removed and moved names stay absent,
and no code computes a column's requested calculations outside the session
model, the logbook index and the registry-side authority. Also the machine
check of the new range.

**Files to modify:**
- `tests/audit/cleanup_audit.cmake`

**Technical Approach.** Locate every passage by its quoted text (Phases 2-4
edited the file). Each new rule gets a comment in the file's style with an
"Allow:" line. Patterns match comments too; a rule that trips on a comment a
phase wrote is fixed by rewording the comment, never by widening the rule.

1. **File head comment** (lines 1-34). After the 501-563 bullet add:
   ```
   #   - a fact is computed by the component that owns it and announced by it:
   #     the session model alone computes a column's requested calculations
   #     (with the index, below it), the demand layer keeps one memory and one
   #     walk in parts of its own, one working-indicator clock turns every
   #     view, and the executor's unused signals and queries stay gone
   #     (items 601-662).
   ```
2. **Group `gestures`** (after `audit_group(gestures)`).
   - Header line `# ─── gestures (acceptance 116)` becomes
     `# ─── gestures (items 116, 306, 501, 519, 527, 534, 543, 544, 562, 605, 608, 625, 628, 629, 651, 659, 661)`.
   - In the long comment above "no synchronous explicit request in product
     code", replace "no product code cancels a job (JobQueue::cancel() is kept
     for a later jobs view)" with "no product code cancels a job
     (JobQueue::cancel() is kept for the jobs dock, a later view of the job
     history, and its doc comment says so)".
   - In the comment above "one authority: explicit-backed" (the sentence "plot
     rows ask dependsOnExplicit(), the logbook column cache asks
     explicitDependencies() through logbookColumnExplicitCalculations()"),
     add: "; the session model hands each enabled column's requested
     calculations to the demand layer (columnRequestedCalculations()), and the
     demand layer asks explicitDependencies() for a plot's (group demand)".
   - After `expect_none("no product code cancels a job" ...)` add, with the
     comments shown:
     ```cmake
     # The cancel operation stays for the jobs dock, a later view of the job
     # history; the rule above keeps it without a product caller. Allow: none
     # expected; removing it is a decision about the jobs dock, not this rule.
     expect_count("cancel is kept for the jobs dock" "bool cancel\\(JobId" 1 src/jobqueue.h)
     # The executor lost what no product code used: the idle and queued signals,
     # the query of both active jobs, and the busy-period bookkeeping behind the
     # idle signal. The job model's rowsInserted, isIdle(), runningJob(),
     # chosenNextJob() and the records carry the same facts. Allow: none
     # expected; a jobs dock that needs one brings it back with its first caller
     # (tests/README.md is excluded: section 10 spells these names).
     expect_none("the executor has no idle or queued signal and no two-job query"
       "jobQueued|activeJobs\\(|JobQueue::idle\\b|announceIdleIfIdle|m_idleAnnounced|AfterEnd"
       src tests ":!tests/README.md")
     expect_none("the executor announces no idle()" "\\bidle\\(\\)" src)
     # The demand layer is the only offerer (the rules above), so the chosen next
     # job is always its own: it keeps no memory of its own offer and withdraws
     # the chosen next job whenever its choice finds nothing. Allow: none expected.
     expect_none("the demand layer keeps no memory of its own offer" "m_offeredJob|withdrawOwnOffer"
       src tests ":!tests/README.md")
     ```
3. **Group `demand`** (after `audit_group(demand)`).
   - Header line: append the new items:
     `(items 506, 508, 513, 515, 518, 527, 529, 530, 533, 534, 538, 540-544, 546, 547, 563, 601-605, 607, 612, 613, 615, 616, 622, 624, 627, 631, 636, 641, 644, 646, 647, 649-652, 659, 661, 662)`.
   - Append, after the last existing rule of the group (`no default profile
     carries a column over a requested output`) and before `leftover markers`:
     ```cmake
     # ─── calculation refinements (items 601-662)
     # A column's requested calculations are computed by the registry-side
     # authority (logbookColumnExplicitCalculations(), logbookcolumn.*) for two
     # owners only: the session model, the one source of each enabled column's
     # closure and requested calculations (the demand layer reads
     # columnRequestedCalculations() and columnDependencyClosure()), and the
     # logbook index, which sits below the model, runs before any model exists
     # and decides the validity of cached values over records. The demand layer
     # computes a plot's requested calculations and closure from the registry
     # (the model knows nothing of plots), one call each; comments name the
     # registry's functions as CalculationRegistry::name(). Allow: none expected.
     expect_only("one computation of a column's requested calculations: the session model and the index"
       "logbookColumnExplicitCalculations\\("
       "^src/logbookcolumn\\.(cpp|h)$|^src/sessionmodel\\.(cpp|h)$|^src/logbookmanager\\.(cpp|h)$" src)
     expect_count("the demand layer asks the registry for a plot's requested calculations only"
       "[.>]explicitDependencies\\(" 1 ${DEMAND_LAYER})
     expect_count("the demand layer computes a plot's closure only" "[.>]staticDependencies\\(" 1 ${DEMAND_LAYER})
     # One display name of a session: SessionModel::sessionDisplayName(). The
     # demand layer and the executor read it and compute none. Allow: none
     # expected; a comment says "the display name".
     expect_none("one display name of a session: the session model's" "SessionKeys::Description|_DESCRIPTION"
       ${DEMAND_LAYER} "src/jobqueue.*" "src/jobmodel.*")
     # The index announces a reason it learns (a record change); the demand
     # layer reads reasons only when it looks up a session's record set.
     # Allow: none expected.
     expect_count("the demand layer reads a record's reason in one place" "[.>]calculationRecordReason\\(" 1
       ${DEMAND_LAYER})
     # The demand layer observes no display change of the model: a bulk edit
     # reaches it as the dependency change the session model publishes, and
     # the column worker's display changes reach nothing. Allow: none expected;
     # a new fact the demand layer needs is announced by its owner (a signal of
     # its own), never inferred from dataChanged.
     expect_none("the demand layer observes no display change of the model"
       "QAbstractItemModel::dataChanged|&SessionModel::dataChanged|onSessionDataChanged" ${DEMAND_LAYER})
     # One walk over the rows per pass, under one guard. Allow: none expected.
     expect_count("the one walk reads the rows under one guard" "RowStabilityGuard +[A-Za-z_]+\\(" 1
       ${DEMAND_LAYER})
     # What the one walk and the one pair memory replaced stays gone: the fill's
     # ending step, the per-cell memory, the second walk and its column tables,
     # the dirty flags, the demand layer's own display name and the fields no
     # view read. Allow: none expected (tests/README.md is excluded: section
     # 10 spells these names).
     expect_none("the demand layer's replaced machinery stays gone"
       "isFillEnding|m_fillEnding|[Ss]ettlement|m_settled|CellKey|ColumnWalk|walkColumns|ColumnInfo|plotCandidates|inspectUnderGuard|inspectedPlots|syncColumns|m_columnReports|buildState|finishState|rebuildRelevantNames|m_relevantNames|m_columnsDirty|rowDisplayName|JobFailed|CalculationDemand::(buildToolTip|kToolTipListLimit|kMaxHeldSessions)|\\.(settling|waiting)\\b"
       src tests ":!tests/README.md")
     # One indicator: a plot row shows the arc or the badge and nothing else;
     # the hover carries the numbers. Allow: none expected.
     expect_none("the views keep no label, no cluster and no clock logic of their own"
       "progressLabel|clusterRect|syncAnimation" src tests ":!tests/README.md")
     expect_none("the plot row shows one glyph" "warningCount|warningIcon|indicatorIcon|drawText\\("
       src/ui/docks/plotselection)
     # One working-indicator clock per application: MainWindow creates it beside
     # the demand layer and hands it to the views through AppContext, and
     # followDemand() (DemandIndicatorView.h) makes it follow the demand layer.
     # The followDemand pattern needs an argument, so a comment that names
     # "followDemand()" does not match. Allow: none expected (tests make their
     # own clock; tests are not searched).
     expect_count("one working-indicator clock" "new WorkingAnimation\\b|make_unique<WorkingAnimation>" 1 src)
     expect_only("one working-indicator clock" "new WorkingAnimation\\b|make_unique<WorkingAnimation>"
       "^src/mainwindow\\.cpp$" src)
     expect_only("the clock follows the demand layer in one place" "followDemand\\([^)]"
       "^src/ui/docks/DemandIndicatorView\\.(cpp|h)$|^src/mainwindow\\.cpp$" src)
     # The glyph's colour, size and spacing, the choice of glyph, the tooltip
     # display and the repaint when the demand layer goes are written once
     # (DemandIndicator.*, DemandIndicatorView.*). The cell delegate's pending
     # colour and tooltip are its own. Allow: none expected.
     expect_none("the plot rows and the headers share the glyph plumbing"
       "QPalette::ColorGroup|QToolTip::|qMax\\(2|drawWorkingGlyph|drawWarningGlyph|[.>]setActive\\("
       "src/ui/docks/plotselection/PlotRowDelegate.*" "src/ui/docks/logbook/LogbookHeaderView.*")
     expect_only("the views learn of the demand layer's end in one place" "&QObject::destroyed"
       "^src/ui/docks/DemandIndicatorView\\.cpp$" src/ui)
     # The fill's progress text is its own, distinct from the column worker's.
     # Allow: reword, never duplicate.
     expect_count("the fill has a progress text of its own" "\"Computing results: %v / %m\"" 1
       src/ui/docks/logbook/LogbookView.cpp)
     # The documents describe the refined demand layer. Allow: say "the pair
     # memory", "a remembered failure", "the scheduler completes it"; never name
     # the removed API or the per-cell memory.
     expect_none("the documents describe the refined demand layer"
       "jobQueued|activeJobs\\(|\\bidle\\(\\)|[Ss]ettlement|progressLabel|isFillEnding|one clock per view|lose work only by stepping|under the same label|arc with \"k of n\"|triangle with a number|CalculationDemand::(kMaxHeldSessions|kToolTipListLimit|buildToolTip)"
       docs README.md)
     ```
     That is 17 rules in `demand` and 4 in `gestures`: 21 new rules.
4. **Acceptance traceability block** (the comment above `math(EXPR RULES ...)`
   and the check).
   - Comment: "... and 501-563 (demand-driven requested calculations, item =
     500 + clause number) and 601-662 (calculation refinements, item = 600 +
     clause number)".
   - The range test becomes
     `... OR (item GREATER_EQUAL 501 AND item LESS_EQUAL 563) OR (item GREATER_EQUAL 601 AND item LESS_EQUAL 662)))`
     and its message "... 401-442, 501-563 and 601-662".
   - Add after the 501-563 loop:
     ```cmake
     foreach(item RANGE 601 662)
       list(FIND items_automated "${item}" index)
       if(index EQUAL -1)
         _violation("[traceability] acceptance item ${item} has no resolving test or audit line in tests/acceptance_map.txt")
       endif()
     endforeach()
     ```
5. **Plant a hit** (the file's head asks for it: a misspelt pathspec passes
   silently). For each rule below, make the planted edit, run
   `cmake -DREPO=. -P tests/audit/cleanup_audit.cmake`, see exactly that rule
   report the planted line, and undo the edit. Report each result.

   | Rule | Plant |
   |---|---|
   | cancel is kept for the jobs dock | rename the declaration to `bool cancelJob(JobId` in `src/jobqueue.h` (count 0) |
   | the executor has no idle or queued signal ... | `// jobQueued` in `tests/tst_jobqueue.cpp` |
   | the executor announces no idle() | `// idle()` in `src/jobmodel.h` |
   | the demand layer keeps no memory of its own offer | `// m_offeredJob` in `src/demandfill.h` |
   | one computation of a column's requested calculations | `// logbookColumnExplicitCalculations(` in `src/calculationdemand.cpp` |
   | ... plot's requested calculations only | a second line `// registry.explicitDependencies(` in `src/calculationdemand.h` (count 2) |
   | ... plot's closure only | `// registry.staticDependencies(` in `src/demandstate.cpp` (count 2) |
   | one display name of a session | `// _DESCRIPTION` in `src/jobmodel.cpp` |
   | ... reason in one place | `// logbook.calculationRecordReason(` in `src/demandsettleclock.cpp` (count 2) |
   | the demand layer observes no display change of the model | `// &QAbstractItemModel::dataChanged` in `src/demandfill.h` |
   | ... under one guard | `// SessionModel::RowStabilityGuard guard(model);` in `src/calculationdemand.h` (count 2) |
   | the demand layer's replaced machinery stays gone | `// settlement` in `tests/tst_calculation_demand.cpp`; then `state.waiting` in a comment of `src/calculationdemand.cpp` |
   | the views keep no label, no cluster ... | `// progressLabel` in `tests/tst_fusion_rows.cpp` |
   | the plot row shows one glyph | `// drawText(` in `src/ui/docks/plotselection/PlotSelectionDockFeature.cpp` |
   | one working-indicator clock (count) | nothing to plant: remove the `new WorkingAnimation(this)` line temporarily (count 0) |
   | one working-indicator clock (only) | `// new WorkingAnimation(this)` in `src/ui/docks/plotselection/PlotRowDelegate.cpp` (also trips the count) |
   | the clock follows the demand layer in one place | `// followDemand(m_clock, m_demand);` in `src/ui/docks/logbook/LogbookHeaderView.cpp` |
   | the plot rows and the headers share the glyph plumbing | `// QToolTip::showText` in `src/ui/docks/logbook/LogbookHeaderView.h` |
   | ... demand layer's end in one place | `// &QObject::destroyed` in `src/ui/docks/logbook/LogbookCellDelegate.cpp` |
   | the fill has a progress text of its own | change the literal's `results` to `result` (count 0) |
   | the documents describe the refined demand layer | `idle()` in `docs/COMPUTED_PLOTS.md` |

   Also confirm, without planting, that each `${DEMAND_LAYER}` rule counts its
   one expected line (the audit passes), and that the rules added by Phases 2-4
   still pass.

**Acceptance Criteria:**
- [ ] `audit_cleanup` passes with **exactly 21 more rules** than after Phase 4 (expected: "cleanup audit passed (132 rules)" after Phase 4 -> "cleanup audit passed (153 rules)"; if the Phase 4 number differs, the difference must still be 21).
- [ ] Every row of the plant table tripped its rule and only its rule (report it).
- [ ] A map line citing item 663 or 600 is reported as outside the ranges (check once by planting a line in `tests/acceptance_map.txt`, then remove it).

**Complexity:** M

---

### Task 5.6: `tests/README.md`

**Purpose:** Traceability rows for the new tests, the audit's description, the
new range and its appendix, the amended items, and manual steps for what
cannot be automated.

**Files to modify:**
- `tests/README.md`

**Technical Approach:**

1. **Section 1 test catalogue** (lines 53-144). Edit these rows:
   - `tst_session_model_engine` (80): after "(`schedulerWaitingTaskDoesNotSpin`)"
     add "; a task that can wait and loses its work without a step is completed
     once, not cancelled, after a final progress report and before the next
     active task or idle, while a task without `canStep` is not
     (`schedulerCompletesWaitingTaskWhoseWorkIsGone`)".
   - `tst_result_store` (81): "write failures" becomes "write failures, each
     announced with its reason after the record change of the pair
     (`writeFailureIsAnnounced`)"; the last clause adds "and a changed reason
     announced as a record change".
   - `tst_result_columns` (82): add "the session model's column knowledge -
     each enabled column's requested calculations and closure, current before
     the queued environment check (`sessionModelExposesColumnKnowledge`) - and
     a row's display name for loaded rows, stubs and failed-load placeholders
     (`sessionDisplayNameOfEveryRowKind`)".
   - `tst_logbook_index` (111): "(and each session's `"recordReasons"`,
     `recordReasonsRoundTrip`; a changed reason announced as a record change,
     an unchanged one silent, `recordReasonChangeIsAnnounced`)".
   - `tst_jobqueue` (89): delete "without an `idle()` in between"; replace "(no
     pin left, no `jobQueued` after its `jobFinished`)" with "(no pin left)";
     append "the job record naming its session by the session model's display
     name (`runsAndPublishes`)"; items: "... demand items 521, 540, 544, 560,
     561; refinement items 606, 624, 628, 629".
   - `tst_calculation_demand` (91): rewrite the relevant clauses:
     "chained calculations upstream first with no `idle()` between links" ->
     "chained calculations upstream first, the executor never idle between
     links"; "settlements (not applicable, failures, a session file that cannot
     be loaded, visible or hidden) that are not loaded again" -> "one pair
     memory (a refused offer or a column's not-applicable verdict; a failed
     job, load or record write; an exception result) that keeps a session from
     being loaded or a pair from being offered again after eviction, a sort or
     the column worker's pass (`settledPairsSurviveEvictionSortAndColumnWorker`),
     cleared by a record change, an input change or a registry change
     (`pairMemoryIsClearedByRecordInputAndRegistryChanges`), a column's verdict
     for that column only (`columnVerdictDoesNotSuppressAnotherColumn`)"; add
     "**Refinements:** one walk for plots and columns, a column track running
     on one blocker filing its others (`runningColumnTrackFilesItsOtherBlockers`);
     a record that could not be written shown as a failure on the column header
     and the plot row, not retried in the run, cleared by a later write, tried
     again after a restart (`failedRecordWriteIsShownAndNotRetried`,
     `failedRecordWriteIsShownOnThePlotRow`); a reason learned by the column
     worker's restore reaching the demand layer through the record change alone
     (`recordReasonReachesDemandThroughRecordChange`); column knowledge from the
     session model (`columnKnowledgeComesFromTheSessionModel`); the choice
     acting on the executor's answer and withdrawing a chosen next job nothing
     wants (`chosenNextJobFollowsTheExecutorsAnswer`); the settle clock
     (`settleClockAnswersItsQuestions`); the fill completed by the scheduler, a
     fill that ends behind another task not leaking its total
     (`fillEndingBehindAnotherTaskStartsNextCountFresh`), holds released at the
     executor's shutdown"; items "(... demand items 501-560; refinement items
     601-604, 607, 609, 611, 612, 615, 616, 618-620, 622, 624-627, 630-643,
     645, 647, 648, 651-659)".
   - `tst_plot_row_layout` (92): "the pure geometry of a plot-list row's one
     glyph, without widgets or a font: the glyph shown, nothing shown,
     right-to-left as the exact mirror image; there is no hit rectangle".
   - `tst_plot_row_delegate` (93): "... one of the **two tests that link Qt
     Widgets** ...; it also compiles the shared indicator (`DemandIndicator.*`,
     `DemandIndicatorView.*`). Plain rows pixel-identical to the base delegate;
     one glyph, the working indicator, painted with no label and the name
     elided rather than the glyph; the application's clock, made to follow the
     demand layer by `followDemand()`, turning only while a plot is working
     (`workingAnimationClock`); the badge replacing the indicator once finished,
     with no count; hover detail from `DemandState`; the shared glyph metrics,
     colours and tooltip display (`sharedGlyphPlumbing`); ...; a click, right
     click or double click on the glyph being a click on the row; ... (...;
     demand items 514, 517, 534-537, 557; refinement items 644-646, 649, 650,
     660)".
   - `tst_logbook_indicators` (94): "`LogbookView` with `LogbookHeaderView` and
     `LogbookCellDelegate` in an offscreen window beside a reference
     `QTreeView`, and a plot list with the row delegate, on a real demand
     layer, executor and `SessionModel` and one working-indicator clock ...;
     the plot rows and the headers turning on that one clock
     (`plotRowsAndHeaderTurnOnOneClock`); the progress line's texts, "Computing
     results" for the fill (`fillProgressLineHasItsOwnText`); a record that
     could not be written listed in the row's and the header's hover
     (`failedWriteIsListedInTheHover`); ... (demand items 527, 533-539, 557,
     558; refinement items 618, 633, 644-650, 652, 660)".
2. **Section 8, "Writing a test", lines 582-606.** `Quiet` is "nothing started
   since" (no new job row); add `activeJobIds(queue)` ("the ids of the active
   job records, in model order: the running job, then the chosen next job").
   Add a sentence to `waitDemandIdle()`: "it returns as soon as the fill has
   no work, which can be before the scheduler's completion tick: read the
   fill's final progress or `schedulerIdle` with `QTRY_*`". In the "Stored
   results" bullet add: "a write failure is also announced
   (`SessionModel::calculationRecordWriteFailed`), and the demand layer shows it
   as a failure".
3. **Section 9 intro (678-679).** "Seven specifications, seven ranges of items
   in `tests/acceptance_map.txt`, the machine-checked form of the seven tables
   below (section 10)".
4. **Section 9.6** (997-1073).
   - Intro, after "(items 304, 306, 321, 337).": "Clauses 13, 26, 28, 30, 32,
     35, 36, 37, 39, 46, 47, 57 and 59 are stated as amended by the
     specification "Calculation refinements" (9.7)."
   - Rows 513, 526, 528, 530, 532, 535, 536, 537, 539, 546, 547, 557, 559: the
     "Section" cell becomes "<section>, as amended", the "Clause" cell the
     amended clause of Task 5.4, and the "Evidence" cell gains the new lines in
     the table's style (`tst_x::a`, `b`; `tst_y::c`; `manual M29`).
   - Row 562: delete `indicatorAndWarning` (the evidence reads
     "`tst_plot_row_layout::indicatorOnly`").
5. **New section 9.7 "Calculation refinements (items 601-662)"**, after 9.6.
   Intro: the sixty-two clauses of the specification "Calculation
   refinements", stated in full in
   [appendix G](#appendix-g-the-acceptance-items-of-calculation-refinements-601-662).
   In the map, item = 600 + the clause number; the same four line forms as
   9.2, and every item has at least one test or audit line. "Section" is the
   section of the specification (10.1 and 10.2 its subsections); its sections 1
   (motivation) and 4 (terms) have no item. Clauses 53-61 are its section 14
   tests, one per bullet, and clause 62 its section 15. No clause is stated
   as settled: the specification, revised after the plan review, carries
   what the first plan draft had settled (the scheduler's rule scoped to
   tasks that can wait, a fill that starts from nothing starting its own
   count, a column's verdict holding for that column, the bulk edit's
   announcement of its edit, a successful load forgetting failed-load
   facts). The specification amends those of 9.6 (items 513, 526, 528, 530,
   532, 535-537, 539, 546, 547, 557, 559). Then the table
   `| # | Section | Clause | Evidence |`, one row per item of Task 5.4
   item 3, the Evidence cell in the 9.6 style.
6. **Section 10.**
   - Group `widget-free-core` bullet (1143-1146): "the executor, the job
     model, the demand layer (its four files), the plot model, `PlotRowLayout.h`
     or the shared glyphs (`DemandIndicator.*`; not its view half,
     `DemandIndicatorView.*`, which is a view) include a widget header".
   - Group `gestures` bullet (1130-1142): items list as in Task 5.5; append:
     "; `cancel(JobId` is not declared exactly once in `src/jobqueue.h` (kept
     for the jobs dock); the executor's removed idle and queued signals, its
     two-job query or its busy-period bookkeeping (`jobQueued`, `activeJobs(`,
     `JobQueue::idle`, `announceIdleIfIdle`, `m_idleAnnounced`, `AfterEnd`)
     appear in `src` or `tests`, or `idle()` in `src`; or the demand layer's
     own-offer memory (`m_offeredJob`, `withdrawOwnOffer`) reappears".
   - Group `demand` bullet (1219-1248), updated to the rules as Phases 2-4 left
     them and extended:
     - "`CalculationDemand` is named outside the demand layer's files
       (`calculationdemand`, `demandstate`, `demandfill`, `demandsettleclock`),
       `MainWindow`, `AppContext.h`, the plot list's delegate and dock
       feature, the logbook view, header view, cell delegate and dock feature,
       the plot widget and the shared indicator's view half
       (`DemandIndicatorView.*`)";
     - "... includes a header of the demand layer";
     - "the plot-row delegate, the header view, the cell delegate or the shared
       indicator (`DemandIndicator.*`, `DemandIndicatorView.*`) handle a mouse
       or key event of their own";
     - "`ColumnFillTask` appears outside the reconciler, the fill,
       `sessionmodel.h` and the logbook view, or `loadPinnedSession(` outside
       the reconciler, the fill and the session model; the demand layer loads
       or pins other than through its fill; the fill or the settle clock calls
       the executor or names a type of the walk (`BlockerReport`,
       `DemandTrack`, `DemandState`, `DemandCondition`, `RowStabilityGuard`,
       `PairMemory`, `LearnedFact`)";
     - "the load bound in `demandfill.h` is not `JobQueue::kMaxRunningJobs +
       1`";
     - appended: "`logbookColumnExplicitCalculations(` is called outside the
       registry-side definition (`logbookcolumn.*`), the session model and the
       logbook index (one computation of a column's requested calculations);
       the demand layer calls the registry's `explicitDependencies(` or
       `staticDependencies(` more than once each (a plot's), reads a record's
       reason more than once, or holds more than one row stability guard; the
       demand layer or the executor names `SessionKeys::Description` or
       `_DESCRIPTION` (one display name, the session model's); a name the one
       walk and the one pair memory replaced (`isFillEnding`, `Settlement`,
       `ColumnWalk`, `walkColumns`, `plotCandidates`, `syncColumns`,
       `rowDisplayName`, `CalculationDemand::buildToolTip` ..., a track's
       `.settling`, a state's `.waiting`) appears in `src` or `tests`;
       `progressLabel`, `clusterRect` or `syncAnimation` appears in `src` or
       `tests`, or a label or count in the plot list's sources; a
       `WorkingAnimation` is created other than once, in `mainwindow.cpp`, or
       `followDemand(` is called outside `DemandIndicatorView.*` and
       `mainwindow.cpp`; the plot row or the header computes a glyph colour, a
       tooltip display, a glyph size, a glyph choice or drives the clock, or
       anything in `src/ui` but `DemandIndicatorView.cpp` connects to a
       `destroyed` signal; the fill's "Computing results" text is not in
       `LogbookView.cpp` exactly once; or `docs` or `README.md` name the removed
       executor API, the per-cell memory, `progressLabel`, the fill's ending
       step, one clock per view, a "k of n" beside the arc or a count beside
       the triangle".
   - Traceability bullet (1249-1253): add 601-662 to both range lists.
7. **Section 12 manual steps.**
   - Intro (1743): "Five scripts."
   - M1 (1787): "the row shows the turning arc and "2 of 3", then turns
     plain" -> "the row shows the turning arc (its tooltip reads "Computing: 2
     of 3 done"), then turns plain".
   - M2 (1789): replace "(The progress line may show "Computing columns" for
     the cheap columns of a profile: that label is shared on purpose.)" with
     "(The progress line may show "Computing columns" for the cheap columns of
     a profile; "Computing results" would mean a column over a requested
     output.)".
   - M3 (1791): "the row shows the turning arc at the right of its name, with
     no number; Pitch and Yaw, if checked, show the same. The tooltip reads
     "Computing: 0 of 3 done" ... As each fit publishes, its graph appears
     without any further action, the tooltip's numbers advance ("1 of 3", "2 of
     3"), and finally ...".
   - M4 (1793): "Click, right-click and double-click the arc of a working row".
   - M6 (1797): "With one fit running and two tracks waiting (the tooltip reads
     "Computing: 0 of 3 done"), hide one waiting track: the tooltip reads "0 of
     2" at once."
   - M7 (1799): "the row shows the warning badge instead of the arc, with no
     number".
   - M17 (1877): "the progress line shows "Computing results" until the fill
     has loaded it".
   - M23 (1903): "The arc at the right of the row's name turns about once a
     second, with no number beside it."
   - M24 (1905): "the progress line shows "Computing results: k / n" for the
     whole fill".
   - New **12.5 Calculation refinements**, after 12.4 and before appendix A.
     Intro: what the automated tests cannot show - the two arcs turning in step
     in the real window, the progress line switching texts, a real record
     write that fails and a session file that loads later in the run. Use the
     preamble of 12.1 (a COPY of a logbook) and the recordings of 12.4. Steps
     (each opens with its bold id and the items in parentheses, exactly
     `**M29 (...)` etc., so the audit finds `**M29 `):
     - **M29 One glyph (535-537, 644, 645).** Show two fusable tracks without
       stored fits and check Roll: the row shows the turning arc at its right
       end and nothing else - no "0 of 2". Hovering the row or the arc shows
       "Computing: 0 of 2 done" and the running track with its step; the
       numbers advance as fits finish. With a recording the model rejects shown
       as well, once the other fits end the row shows only the warning
       triangle, with no number; its tooltip lists the recording with its
       reason. Compare the row's height, elision and plain look with a row that
       needs no computing, in the light and the dark theme.
     - **M30 One clock (535, 646).** With the fits of M29 running, add a
       logbook column over a fusion value so that its header works too. Watch
       the row's arc and the header's arc for a few seconds: they point the
       same way at every moment. When the plot's fits end and the column still
       works, the header's arc keeps turning and the row is plain. When nothing
       works both arcs are gone and Task Manager shows FlySight Viewer at 0 %
       CPU.
     - **M31 The fill's own progress text (539, 647).** Hide every track and add
       the fusion column of M30 on a logbook with recordings that are not
       loaded and have no stored fit: the progress line shows "Computing
       results: k / n", with no cancel button. While it fills, add a column
       that needs no computing (for example the exit time): the line shows
       "Computing columns: k / n" (with its cancel button) while that column is
       filled, then "Computing results: k / n" again with the fill's own total;
       it never shows one label with two totals. When the fill ends the line
       goes.
     - **M32 A result that cannot be stored (526, 618-620, 648).** Quit. Pick a
       fusable recording without a stored fit and, in `<scratch>/FlySight
       Viewer/logbook/cache/`, create a folder with its record file's name
       (`<uuid>.builtin%2Efusion%2Efit.fvresult`; on Linux or macOS, `chmod a-w
       cache` instead). Start, show the recording and check Roll: the fit runs
       once; the plot draws it, and the row shows the warning triangle whose
       tooltip lists the recording with "Sensor fusion: Couldn't write file
       ...". Hide and show the recording, set "Maximum cached sessions" to 0 and
       hide it: no second fit starts (CPU idle). Add the fusion column: its
       header shows the triangle with the same entry, and the recording is not
       loaded or fitted for it. Quit and start again with the folder in place:
       the fit runs once more and fails the same way. Quit, remove the folder
       (or `chmod u+w cache`), start: the fit runs, its record file appears and
       the row is plain.
     - **M33 A session file that loads later in the run (639).** Windows only,
       with the caveat of M28 (if the held file still loads, record that the
       step could not be carried out). Reproduce M28 up to the badge that lists
       "<description> - The session file could not be loaded". Without
       quitting, run `$f.Close()` and show that recording: it loads, its entry
       leaves the header's tooltip, its fit runs (a visible session first) and
       its cell gets its number; the badge goes once nothing else failed.
     End with "Pass / fail and a note per step go in the phase report. A step
     that fails is reported as it failed, not adjusted."
8. **Appendix F intro (2540-2546).** Append: "Clauses 13, 26, 28, 30, 32,
   35-37, 39, 46, 47, 57 and 59 are stated as amended by the specification
   "Calculation refinements" (appendix G)." Rewrite those thirteen clauses as
   "N. (<section>, as amended) <clause of Task 5.4, capitalized, full stop>".
9. **New appendix G** at the end: `## Appendix G. The acceptance items of
   calculation refinements (601-662)`. Intro: the testable statements of the
   specification "Calculation refinements", which amends "Demand-driven
   requested calculations" (appendix F), one sentence each, with the
   specification's section number in front. The specification numbers no
   clauses; the numbers below are this list's, and item = 600 + the number.
   Its sections 1 (motivation) and 4 (terms) have no item; section 13's
   contracts are clauses 51 and 52 and the amended clauses of appendix F.
   No clause is stated as settled (9.7 says why). Then "1. (2) The
   principles of ..." through "62. (15) ...", each the clause of Task 5.4
   item 3, capitalized, ending with a full stop.

**Acceptance Criteria:**
- [ ] `tests/README.md` contains `**M29 `, `**M30 `, `**M31 `, `**M32 `, `**M33 ` exactly once each.
- [ ] Section 9.7 has 62 rows (601-662) and appendix G 62 numbered clauses, each identical in substance to the map's comment line.
- [ ] No row of section 1 or line of section 12 mentions `idle()`, `jobQueued`, "settlement", "k of n" beside an arc, or "Computing columns" for the fill.

**Complexity:** L

---

### Task 5.7: Verification

**Purpose:** The full suite and the audit pass, and the documents contain
nothing the refinements made false.

**Files to modify:** none (fix what the checks find in the files of Tasks
5.1-5.6).

**Technical Approach:**
1. Build: `cmake --build build-phase1 --config Release` (this phase adds no
   source file, so no reconfigure). Only documentation, the map and the audit
   changed, but build so that the test executables the traceability check
   names are current.
2. Audit alone: `ctest --test-dir build-phase1/FlySightViewer-build -C Release
   --output-on-failure -R audit_cleanup`; also `cmake -DREPO=. -P
   tests/audit/cleanup_audit.cmake` from the repository root. Expected: passes,
   "(153 rules)" if Phase 4 left 132 (the count rises by exactly 21).
3. Full suite, sequentially, with no other `ctest` / `tst_*` process running:
   `ctest --test-dir build-phase1/FlySightViewer-build -C Release
   --output-on-failure`. Never `-j`. All pass (the executor-driven fusion tests
   are load-sensitive).
4. Greps (Git Bash, repository root), each expected empty:
   - `git grep -n -E "jobQueued|activeJobs\(|\bidle\(\)|[Ss]ettlement|progressLabel|isFillEnding|one clock per view" -- docs README.md`
   - `git grep -n -E "k of n\"|with a number|count on the row" -- docs/COMPUTED_PLOTS.md`
   - `git grep -n -E "idle\(\)|jobQueued|settlement|\"k of n\"" -- tests/README.md` except the section 10 description of the rules that spell them (inspect every hit).
   - `git grep -n "Computing columns" -- docs tests/README.md`: every hit is the column worker, never the fill.
5. Check every "16.x" cross-reference in `docs/`, `README.md`, `tests/README.md`
   and `src/` comments still names the right subsection
   (`git grep -n -E "16\.[0-9]+" -- docs src tests/README.md README.md`).

**Acceptance Criteria:**
- [ ] `audit_cleanup` passes with the expected rule count; the full sequential `ctest` run passes.
- [ ] The greps of step 4 are empty (or every remaining hit is justified in the report).

**Complexity:** S

## Testing Requirements

### Unit Tests
- None added or changed. The phase cites the test functions Phases 1-4 added;
  `audit_cleanup`'s traceability check proves each exists.

### Integration Tests
- `audit_cleanup` (the 21 new rules, the new range, the plant-a-hit checks of
  Task 5.5).
- The full suite, sequentially, on `build-phase1`.

### Manual Verification
- Read the rewritten §15-16 of `docs/CALCULATIONS.md` against the code as
  committed (the class comments of `calculationdemand.h`, `demandfill.h`,
  `demandsettleclock.h`, `demandstate.h`, `idlescheduler.h`, `jobqueue.h`,
  `calculationresultstore.h`, `sessionmodel.h`, `DemandIndicator.h`,
  `DemandIndicatorView.h`): every behaviour stated in the documents is what the
  code comments state.
- Steps M29-M33 are written for Michael to run; this phase does not run them.

## Notes for Implementer

### Build and test (from the overview's Decisions & Constraints)
- Build **only** `build-phase1/`: `cmake --build build-phase1 --config Release`. **Never build `build/`.**
- Test with `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure`, **sequentially, never `-j`**, with no stray `ctest` or `tst_*` process running (executor-driven fusion tests are load-sensitive). The audit alone: `-R audit_cleanup`, or `cmake -DREPO=. -P tests/audit/cleanup_audit.cmake`.
- This phase adds no source file: no reconfigure of `build-phase1/FlySightViewer-build`, and `_FLYSIGHT_GTSAM_REACHERS` is unchanged.
- Commits are the orchestrator's. Never run a git command that changes repository state.

### Gotchas
- **Existing audit rules search the documents.** In `docs/` and `README.md`
  never write the removed queue API (`oldestQueued`, `cancelUnwantedQueued`,
  `cancelSession(`, `cancelAll(`, `RequestResult`), "cancel control", "cancel
  icon", "refresh icon", "press refresh", "circled x", the old plot-request
  names, "Results are kept in memory only" or similar, "makes every stored
  result stale", the branch name, `EKF`, the stationary-window names, or a
  second copy of "Bump it, or the result version of the calculation
  concerned" (counted once in `docs/CALCULATIONS.md`). The new docs rule adds
  `idle()`, `jobQueued`, `activeJobs(`, "settlement", `progressLabel`,
  `isFillEnding`, "one clock per view", "lose work only by stepping", "under
  the same label", `arc with "k of n"`, "triangle with a number" and the
  three `CalculationDemand::` constants.
- **Do not write "the existing tasks lose work only by stepping or by
  cancel"** (spec §5 says why not). Write that the rule applies
  to tasks that can wait (`canStep`) and the others complete through their step
  or cancel, as before (Phase 1's decision).
- **Count rules count lines, comments included.** `[.>]explicitDependencies\(`,
  `[.>]staticDependencies\(`, `[.>]calculationRecordReason\(` and
  `RowStabilityGuard +[A-Za-z_]+\(` in the demand layer's files match one code
  line each; a comment that names these functions writes them as
  `CalculationRegistry::explicitDependencies()` or `LogbookManager::...()` (no
  member-access prefix) and never as `guard(`. If a rule counts 2 because of a
  comment a phase wrote, reword that comment.
- **`\bidle\(\)` over `src`.** Phase 1 was to leave no `idle()` in `src/`
  comments. If `src/jobqueue.*` or `src/calculationdemand.*` still has one,
  reword it ("before it schedules the next start"). `tests/tst_jobqueue.cpp`
  comments may still say `idle()`: tests are not searched by that rule, so leave
  them.
- **`followDemand` in comments.** `DemandIndicator.h`'s `WorkingAnimation`
  comment names "followDemand()"; the rule's pattern `followDemand\([^)]` needs
  an argument, so it does not match a comment written with empty parentheses.
  Never write an argument list for it in a comment outside the allowed files.
- **`warningCount`** is also a local of `tests/tst_schema_units.cpp`: that is
  why the "one glyph" rule searches `src/ui/docks/plotselection` only and the
  "no label, no cluster" rule does not name it.
- **CMake quoting.** In the new rules `\\b`, `\\(`, `\\.` and `\"` are CMake
  escapes for `\b`, `\(`, `\.` and `"`; `${DEMAND_LAYER}` is unquoted (a list
  of pathspecs), as Phase 2 uses it.
- **The map's comment lines are free text** for the parser, but the audit's
  "the demand layer's replaced machinery stays gone" rule searches
  `tests/acceptance_map.txt`: no clause may contain "settlement", `.settling`
  or `.waiting` (the clauses above do not).
- **Manual step ids** must appear as `**M29 ` (bold, id, space) for the
  traceability check.
- **`as amended`.** It marks a demand-driven item (5xx) whose meaning this
  specification changes. No 6xx item is marked "as settled": the
  specification was revised after the plan review to carry what the plan
  had settled, so every 6xx clause is the specification's own.

### Decisions Made
- **Item numbering:** 601-662, item = 600 + clause number of appendix G (the
  specification has no numbered clauses; the list follows its sections, one
  clause per testable statement, then one per §14 bullet and one for §15).
- **Amended items:** 513, 526, 528, 530, 532, 535, 536, 537, 539, 546, 547,
  557, 559 - those whose sentence became false or incomplete (the unloaded
  order and announced reasons; failed loads and writes as remembered failures;
  holds released at shutdown; the scheduler's completion; column-scoped
  verdicts; one glyph, one clock, the hover as the only place of the numbers;
  the fill's own text; the announcing index and store; the one-way flow with
  the failed write). Items 505, 542-545, 560 keep their meaning and their
  citations (Phase 1: their `idle()` assertions went; the facts stay).
- **The one-computation rule** forbids `logbookColumnExplicitCalculations(`
  outside `logbookcolumn.*` (the registry-side definition over
  `explicitDependencies()`), `sessionmodel.*` and `logbookmanager.*`. The
  index's use stays (Phase 1): it sits below the model, runs at `initialize()`
  before any model exists and decides cached-value validity. The demand
  layer's plot-side registry use is allowed but pinned to one call each of
  `explicitDependencies` and `staticDependencies` (Phase 3's `syncSources()` and
  `isRequested()`), which also forbids a column computation that bypasses
  `logbookColumnExplicitCalculations()`.
- **Group placement.** Executor-side and offering rules (cancel kept, executor
  signals and queries, the own-offer memory) go in `gestures`, which already
  holds the offer / withdraw / cancel rules; everything about the demand layer,
  its views and its documents goes in `demand`.
- **§16 keeps its numbering**; the parts are a new 16.12, so no existing "16.x"
  reference moves.
- **DATA_SCHEMA §12 and SENSOR_FUSION.md** are edited although spec §15 names
  only DATA_SCHEMA §11: both state which failures are shown and not stored, and
  a failed write is now one of them (Phase 3's behaviour).
- **M32 uses a folder at the record's path** on Windows (portable, and the
  mechanism of the tests); a read-only `cache/` works only on POSIX (Windows
  ignores the read-only attribute on folders).

### Open Questions
- None. The load-forgetting rule of spec §10.2 is live (eviction turns a
  failed-load placeholder back into a stub, so a later show loads it again
  in the same run); Phase 3's test 7 covers it and item 639 cites it.

## Definition of Done

This phase is complete when:
1. All tasks have passing acceptance criteria.
2. All tests pass: the full `ctest` run on `build-phase1`, sequential, including `audit_cleanup` with exactly 20 more rules than after Phase 4 and its traceability check over 601-662.
3. The documents follow the patterns of the previous documentation phase (`docs/` in the voice of the existing sections, the map and `tests/README.md` in their existing forms).
4. No TODOs or placeholder text remain; no document or catalogue row describes the removed executor API, the per-cell memory, the fill's ending step, a "k of n" label, a count beside the badge or a clock per view.
