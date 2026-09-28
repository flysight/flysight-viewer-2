# Phase 4: The per-source presentation goes

## Purpose

This phase covers spec §9's last bullet and §10's "counts that no view reads
any more are not kept". Phases 1-3 moved every view onto progress, failures
and pending cells. The per-source state is now read by tests only, so it goes
from the demand layer:

- the per-source state, with its counts, lists and hover text;
- the queries that return it and the per-source signals;
- the working-id lists.

Every test that read this state is rewritten to assert the same fact through
the values of spec §10 and the executor (overview decisions 9 and 10). It is
one phase because the removal and the rewrite of its readers cannot be
separated: the tree compiles only when both are done.

Nothing the user sees changes in this phase. Documentation beyond code
comments, the new acceptance items and their README appendix belong to
phase 5 (overview decision 11).

## Dependencies

- **Depends on phase 3**, and through it on phases 1 and 2. When this phase
  starts, the following hold as those documents state them:
  - no product code reads `plotState`, `columnState`, `workingPlotIds` or
    `workingColumnIds`, or connects `plotStateChanged`, `columnStateChanged`
    or `statesChanged`;
  - `LogbookCellDelegate` repaints on `pendingCellsChanged` and
    `failuresChanged`;
  - `StatusBarFeature` reads `progress()` and `failures()`;
  - `DemandIndicator*`, `LogbookHeaderView`, `PlotRowDelegate`,
    `PlotRowLayout.h`, `tst_plot_row_delegate` and `tst_plot_row_layout` do
    not exist;
  - phase 1's values, queries and signals exist, and so do its test functions
    in `tst_calculation_demand` (suggested names `progressTextWithoutAPass`
    and `failureTextAndItsLimit`; use the names the tree holds);
  - the audit rule "the views read no per-source state" from phase 3 covers
    `src/ui` and `src/mainwindow.*`.
- **Blocks phase 5**, which cites the test functions and audit rules as this
  phase leaves them.

## What changes

### The demand layer (`src/calculationdemand.h`, `src/calculationdemand.cpp`)

**Removed from the public surface:**

- `plotState()` and `columnState()`;
- `workingPlotIds()` and `workingColumnIds()`;
- the signals `plotStateChanged`, `columnStateChanged` and `statesChanged`.

**Removed from the private bookkeeping:** everything that exists only to build
or announce the per-source state:

- `m_states` and `m_columnStates`;
- `Walk::states`, with the `addTrack()` tally in `walkRows()` and the
  `finish()` / `sourceId` / `requested` loop in `recompute()`;
- the per-source diffs in `applyStates()`, and its per-source parameters;
- `onJobProgress()`'s update of the running tracks and of their tooltips.
  After this phase it updates only `progress().progressText`, as phase 1 made
  it do.

**`DemandTrack` goes.** After this phase the walk needs one thing from a
track: its condition. The other fields have no reader left:

- `sessionName` is read by the walk for phase 1's values;
- `calculationTitles`, the joined `reason` and `jobFailure` fed only the
  tooltip. Phase 1's entries carry the title, the why and
  `retriedAtNextStart` themselves;
- `progressText` is `progress().progressText`.

`classifyLoaded()`, `classifyUnloaded()` and `failedTrack()` therefore return
the condition, together with the failure entries phase 1 attached to the same
calls. Helpers whose only product was a field of the track also go: the
`"; "`-joined reason, `failureReason()`, and the title lists. The default
detail texts that the entries use stay, in whichever function the implementer
keeps them.

**The condition stays, as the walk's own type.** It becomes a private nested
`enum class TrackCondition` of `CalculationDemand`, with the same five
values (Done, Waiting, Running, Failed, NotApplicable). It leaves
`demandstate.h`, whose contract is the values the views read. The helpers
`isPending()` and `isListed()` follow it; making them private statics is one
way.

**What stays, and why:**

| Stays | Why |
|---|---|
| The walk classifies every track of every source, plots and columns, under the one guard | It is how the three values are computed: Waiting and Running give `progress().count` and a pending cell; Running gives `sessionName`; Failed gives the entries of `failures()`; Done and NotApplicable give none |
| `Walk::pendingCells` (per column), `pendingSessions`, `loadCandidates`, `tiers`, `learned`, and phase 1's per-row progress flag and failure list | Pending cells, the fill, the choice, the memory, progress and failures |
| `m_pendingCells`, `m_hasPendingCells` and the per-column compare that emits `pendingCellsChanged` | Pending cells (spec §10: unchanged). A dropped column is found from `m_pendingCells`' old keys, not from `m_columnStates` |
| `isRequested()`, `syncCheckedSet()`, `syncSources()`, `Source` | What is wanted. A plot that is not requested is still never inspected |
| `plotId()` and `columnId()` | `columnId()` is what `pendingCellsChanged` carries; `plotId()` keys the sources and the report memo (see Decisions) |
| `isMerelyUncomputed()`, `progress()`, `failures()`, `sessionFailures()`, `isCellPending()` (both overloads), the three phase 1 signals, and every test seam | The surface the overview lists under "Phase 4 provides" |

The walk keeps **no count that no view reads**. It does not tally tracks per
source, keep a list of running or failed tracks per source, or build any
text. The count it keeps is the number of rows with a Waiting or Running track,
and that is `progress().count`.

**Comments** (the contract; `docs/` is phase 5's). Rewrite every passage that
names the removed surface.

- **The head paragraph:** the demand layer publishes progress, failures and
  pending cells.
- **TRACK CONDITIONS:**
  - the table stays, with `TrackCondition`;
  - the sentence "Every count is a function of the current tracks: wanted =
    ..., done = Done + Failed" becomes what each condition feeds (the
    projection in the table above);
  - "A cell is PENDING while it is Waiting or Running" stays.
- **WHAT A PASS COSTS:** a plot that is not requested contributes nothing to
  progress, failures or pending cells, and no signal is emitted for it. This
  replaces "its state is the default value".
- **MEMORY** and `PairMemory::Kind::Failed`: "shown with the warning badge"
  becomes "listed among the failures".
- **WHEN A PASS RUNS:** `jobProgress` updates the progress text only.
- **PRESENTATION:** the three values and the three signals, stored before
  announced, and who reads them (the logbook's cells and rows, the status
  bar). The tooltip-limit sentence goes.
- **PARTS:**
  - the walk returns the tracks' contributions to progress, failures and
    pending cells, the candidates and the learned facts;
  - the presentation values are phase 1's.
- **LIFETIME:** inert means the default progress, no failures and no pending
  cell.
- **`isMerelyUncomputed()`:** "The engine is asked, not plotState()" becomes
  "not the last pass's values".
- **The removed declarations' comments** go with them.

### The presentation values (`src/demandstate.h`, `src/demandstate.cpp`)

Remove `DemandCondition`, `DemandTrack` and `DemandState`, together with
`addTrack()`, `finish()`, `buildToolTip()`, `kToolTipListLimit` and their
`Q_DECLARE_TR_FUNCTIONS`. What remains is phase 1's `DemandProgress`,
`FailedCalculation` and `SessionFailures`, with its `kListLimit`, `text()` and
`listText()`. The file's head comment names exactly those three values and
their text.

### Test support (`tests/support/plotfixture.{h,cpp}`)

Remove `sessionIdsOf()` and its declaration's comment. `waitDemandIdle()`
reads no per-source state and is unchanged.

### Must not change

These stay as they are:

- what is wanted, the candidates and the offers, the settle wait, the fill's
  inputs, and the pair memory's keys and clearing;
- the one guard;
- the values of phase 1 and when they are announced;
- `isMerelyUncomputed()`;
- the test seams;
- `src/demandfill.*`, `src/demandsettleclock.*`, `src/ui/**` and
  `src/mainwindow.*`, except where a comment names a removed name.

## Interfaces

**Consumed:**

- from phase 1: `progress()`, `failures()`, `sessionFailures()`, the three
  signals, `SessionFailures::text()` and `kListLimit`, and its test functions
  named above;
- from phase 3: no product reader of the removed surface, and the audit rule
  "the views read no per-source state".

**Provided (the overview's "Phase 4 provides"):**

- `CalculationDemand`'s presentation surface is exactly `progress()`,
  `failures()`, `sessionFailures()`, both `isCellPending()` overloads,
  `isMerelyUncomputed()`, `progressChanged`, `failuresChanged`,
  `pendingCellsChanged`, and the existing test seams.
- `DemandState`, `DemandTrack` and `DemandCondition` do not exist; the walk's
  condition is the private `CalculationDemand::TrackCondition`.
- `sessionIdsOf()` does not exist.
- The test functions of this phase, by the names the tree holds. Phase 5
  cites them.
- The audit rules below.

## Acceptance criteria

1. **Surface** (§9, §10). No declaration of `plotState`, `columnState`,
   `workingPlotIds`, `workingColumnIds`, `plotStateChanged`,
   `columnStateChanged`, `statesChanged`, `DemandState`, `DemandTrack`,
   `DemandCondition`, `buildToolTip`, `kToolTipListLimit` or `sessionIdsOf`
   exists. The audit checks `src` and `tests`.
2. **No per-source count** (§10). The demand layer holds no per-source map of
   states and no track tally, list or text. The audit checks it.
3. **One walk** (§10). The walk still classifies every track. The audit's
   "one walk" count stays 1, and its `calculationRecordReason(` count stays 1.
4. **Values unchanged** (§10, §13). Phase 1's and phase 3's tests of
   progress, failures, pending cells and the row warning pass unchanged, and
   so do `tst_status_bar`'s.
5. **Earlier items keep what they assert** (§13, last bullet). Every rewritten
   function asserts the fact its per-source read stood for, by the rules
   below, and passes. The items concerned are demand, priority, the settle
   wait, failures, not applicable, chains, unloaded sessions and the pair
   memory. A function is removed only where §9 removes its subject: the list
   under Tests.
6. **No seam** (overview decision 10). No test seam exposes per-source state.
7. **Green.** The whole suite and `audit_cleanup` pass in `build-agent/`,
   Release, run sequentially.

## Tests

### Translation rules (overview decision 9)

Every read is rewritten by these rules. Read after a flush, as `row()` and
`col()` did.

| Per-source read | Rewritten as |
|---|---|
| Waiting / Running tracks: `waitingCount + runningCount` of the one source a test has | `progress().count` (sessions; with several sources over the same session, the distinct sessions). `highWater` where the test crosses 0 |
| `running` list, `runningCount == 1`, `running.at(0).sessionId` / `sessionName` / `progressText` | `progress().sessionName`, `progressText`, and `running()` (the executor's job: `sessionId`, `calculationId`, `calculationTitle`) |
| `runningCount 0, waitingCount 1` for a job asked to stop | `count` 1 with `sessionName` empty (phase 1, contract note 1) |
| `failed` list, `failedCount`, `failed.at(i).reason`, `calculationTitles`, `sessionName` | `failures()` / `sessionFailures(id)`: `sessionName`, and per entry `calculationId`, `title`, the why alone as `reason` (`"Explicit A: negative input"` becomes title "Explicit A", reason "negative input"; the load failure is one entry per storable calculation, reason "The session file could not be loaded") |
| `jobFailure` | `retriedAtNextStart` (also true for an unstored result: phase 1, contract note 3) |
| `showsWarning()`, `!isWorking()` | the session is in `failures()`; `progress().count == 0` |
| `isPlain()`, `== DemandState()` after the work | `count == 0` and no failure, beside the executor's facts the test already asserts (jobs, `stored()`, values) |
| `wantedCount` / `doneCount` | the facts they stood for: a done track is no longer counted and has its job, record or value; a not-applicable track is not counted, not failed, not pending, and has no job |
| a column's counts | `isCellPending(id, key)` per session, plus the rules above |
| `toolTip` | the progress value's fields, or `sessionFailures(id).text()` |
| `plotStateChanged` / `columnStateChanged` / `statesChanged` spies and connections | `progressChanged` / `failuresChanged` / `pendingCellsChanged`. Where the old signal fired but no value changes, the new assertion is that nothing is emitted |

**Helpers.**

- `row()` (in `tst_calculation_demand`, `tst_fusion_rows` and
  `tst_fusion_store`) and `col()` (in `tst_calculation_demand` and
  `tst_logbook_indicators`) are replaced by helpers that flush and return
  `progress()`, `sessionFailures(id)`, or `failures()` rendered as strings for
  `QCOMPARE`. Reuse phase 1's renderer, and move it to a shared place if two
  files want it.
- The `isCellPending(id, key)` helper stays.
- `watchHolds()` connects `pendingCellsChanged` instead of
  `columnStateChanged`: the holds follow the pending sessions.

### `tst_calculation_demand`

**Removed, because §9 removes their subject:**

- `workingIdsFollowStates` (the working ids);
- `tooltipText` (the per-source tooltip);
- `toolTipListsAtMostTenFailures`, whose cap is phase 1's
  `failureTextAndItsLimit`;
- `progressUpdatesWithoutInspection`, which phase 1's
  `progressTextWithoutAPass` asserts identically.

**Renamed:** `columnStateCountsAndPendingCells` becomes, as a suggestion,
`columnProgressAndPendingCells`. What it asserts:

- the fill's four sessions give `count` 4 and `highWater` 4;
- running s1 gives "Jump 1" / "step 1";
- s2, found not applicable, drops `count` to 3, and its cell is not pending;
- at the end `count` is 0 and there are no failures;
- `pendingCellsChanged` carries only `G_OUT`;
- a pass that changes nothing emits none of the three signals.

**Rewritten by the rules, keeping their names:**

- **Plot demand:**
  - `ordinaryPlotsAreNeverInspected` and `uncheckedPlotsAreNeverInspected`:
    no signal, the default progress, and no failure. The `No/such` query goes,
    since there is no per-id query.
  - `hiddenAndStubRowsAreNotTracks`, `failedLoadPlaceholderIsNotATrack`:
    `count` 1 and the running session's name.
  - `rowScript`:
    - 3 of 3, then 2 of 3;
    - 0 once unchecked, while s2's job runs for no source;
    - 1 of 1 when checked again;
    - 2 of 2 when s4 is shown.
  - `chainedBlockersContinue` records `progress()` on each `progressChanged`.
  - `heldChainContinues`, `chainCompletesAfterFirstJobDoesNotSucceed`,
    `chainStopsForHiddenTrackOrUncheckedPlot`,
    `programmaticCheckCreatesDemand`, `profileStyleApplyCreatesDemand`,
    `startupRestoreWithHiddenSessionsStartsNothing`,
    `showingASessionStartsIt`, `hidingASessionDropsItsWaitingPair`,
    `uncheckingDropsWaitingPairsKeepsRunning`,
    `loadingAVisibleSessionStartsIt`, `mergeCreatesDemandForShownSessions`:
    the rules above. The per-plot `requested` checks are proven by the demand
    that follows.
  - `waitingPairNeededByAnotherPlotSurvives`: `count` 2 after `g` is
    unchecked, and 0 after `g2` is.
  - `plotCheckedDuringAJobJoinsIt`: `count` stays 1 and no second job.
  - `sharedJobSameProgress`: one job per session, `count` 2, "Jump 1",
    "step 1".
- **The settle wait:**
  - `inputBurstRunsOneJob`: `count` 1 with no job.
  - `staleRunningJobIsWaitingAtOnce`: `count` 1 with an empty `sessionName`
    for both plots, which is now one assertion.
  - `dependencyBurstIsCoalesced`: `count` 3, then `sessionName` "Renamed".
- **What is offered:**
  - `sessionWithoutInputIsNeverListed`: s3 is never counted or named and
    has no failure.
  - `onlyRequestableCalculationsAreOffered`: `failures()` is s4 alone.
  - `resultAppearingWhileWaitingDropsThePair`: `count` 2 across `g` and `ea`,
    then 0.
- **Failures:**
  - `inputDeterminedFailureIsStoredBadgedNeverRerun`,
    `jobLevelFailureIsBadgedNotRerunUntilRestart`: the `_data` reason column
    splits into title and why, and the tooltip check becomes `text()`.
  - `failuresListedWhileWorking`: s1 is listed while `count` is 1 and after.
    The `!showsWarning()` while working goes: §7 drops that rule.
  - `removedSessionLeavesNoTrace`, `failedRecordWriteIsShownAndNotRetried`,
    `failedRecordWriteIsShownOnThePlotRow`,
    `pairMemoryIsClearedByRecordInputAndRegistryChanges`,
    `successfulLoadForgetsFailedLoadFacts`: the rules above.
- **Signals and robustness:**
  - `changeSignalsAreMinimal`:
    - checking `plain`, `g` and `g2` gives one `progressChanged` in the one
      pass;
    - a pass that changes nothing emits nothing;
    - unchecking `g2` while `g` still wants the job emits nothing.
  - `registryChangeReclassifies`:
    - the two preamble `requested` checks go;
    - not requested gives `count` 0 and no job;
    - registered gives `count` 1;
    - unregistered gives no signal, `count` 0 and Quiet;
    - "announced once" goes with its signal.
  - `survivesExecutorShutdown`: `count` 2, 0, then 1 with an empty name. The
    inert case gives the default progress and no failures.
  - `nullCollaborators`: the default progress and no failures.
- **Column demand:**
  - `columnIdIsTheDefinitionKey`: the three default-state checks go, and its
    `isCellPending` checks stay.
  - `ordinaryColumnsCreateNoDemand`: spies on the three signals.
  - `enablingColumnFillsEveryUnloadedSession`,
    `loadedHiddenSessionsNeedNoLoad`, `profileStyleColumnsCreateDemand`,
    `startupWithEnabledColumnLoadsAfterColumnWorker`,
    `passOverManyStubsReadsEachRecordSetOnce`: waiting N becomes `count` N.
  - `sessionShownDuringColumnDemandRunsNext`,
    `chainedColumnWithUpstreamRecordIsCompleted`, `storedResultsCreateNoJob`,
    `notApplicableSessionIsSettledWithoutAJob`,
    `columnOfferRefusalIsNotLeftPending`, `bulkEditMakesSettledSessionApplicable`,
    `savesAndBulkEditsPrecedeLoadStep`, `columnVerdictDoesNotSuppressAnotherColumn`
    (per-column `isCellPending` for `G_OUT` and `Z_OUT`),
    `runningColumnTrackFilesItsOtherBlockers`, `fillTaskIsLowestAndNotCancellable`:
    the rules above.
  - `storedRejectionIsBadgedAfterRestartWithoutLoad`,
    `recordReasonReachesDemandThroughRecordChange`: s2's entry is
    {expA, "Explicit A", "negative input", false}, "Jump 2", and before the
    reason is known s2 has no failure and no pending cell.
  - `columnFailuresAreBadgedNotReloaded`: s1 then s2 in row order.
  - `columnJobLevelFailureIsNotReloadedUntilRestart`,
    `unloadableSessionIsSettledAsFailed`, `visibleFailedLoadIsSettledAsFailed`:
    the rules above.
  - `settledPairsSurviveEvictionSortAndColumnWorker`: its snapshot renders
    three things, sorted, because a sort reorders `failures()`:
    - `failures()`;
    - `progress()`;
    - `isCellPending` for the four sessions and both columns.

Update the file's head comment ("the per-plot state and its signals", "the
per-column state").

### Other executables

- **`tst_fusion_rows`:**
  - `row()` goes. `offenceInRows()` becomes a check over the values: the
    absent session is never `progress().sessionName`, has no failure, and
    `count` never exceeds 1.
  - `allSeventeenFusionPlotsAreExplicitBacked`:
    - before the fit, each fusion plot's value is
      `isMerelyUncomputed(session("s2"), "Fusion", m)` and no local-frame
      plot's is. That is the observable "explicit-backed", through the demand
      layer's public static;
    - with all 17 checked, `count` is 1 with one fit chosen; during the fit,
      `count` 1, s2's name and the running job;
    - after the fit, `count` 0, no failures, and no fusion value is merely
      uncomputed.
  - `realRowScript`: `Seen` records `progress()`. Job 1's end gives 2 of
    `highWater` 3; job 2's end gives 0; job 4's end gives 1 of 2; job 5's end
    gives 0. `whileS3Runs` gives 2 of 2 with s3 running, and `afterUncheck`
    gives `count` 0.
  - `rollPitchYawShareOneJob`: one value, `count` 1, the name, a progress
    text and the running job.
  - `accHRowIsBlockedByFusion`: `count` 1 and `isMerelyUncomputed` for accH.
  - `noImuSessionIsNeverCounted`: the rules above.
  - `rejectedTrackShowsBadge`: r1's entry {fit, "Sensor fusion", "Local
    origin index outside GNSS samples", false} and its `text()`; then `count`
    1 and no failure while settling.
  - `editsAndVisibilityDuringFit`: the counts 1, 3, 2, 3, then 0.
  - Update the head comment ("sees each publication in the state").
- **`tst_fusion_store`:**
  - `row()` goes. `offeredFitIsDroppedByUncheck()`: `count` 1 with an empty
    name, then 0 and no failure.
  - `restoredAfterEvictionIsBitIdentical`,
    `restoredAfterRestartIsBitIdentical`,
    `runtimeRegistryChangeDropsFitAndRecord`, `dependencyEditDropsRecord`:
    the rules above.
  - `restoredRejectionShowsBadge`, `restoredSolverFailureShowsBadge`: r1's
    entry with the why and `retriedAtNextStart` false, still listed after the
    spins.
  - `columnOverFusionFillsUnloadedSessions`: `count` 2, then 0.
  - `fusionColumnWithStoredFitsRunsNothing`: `count` 0, no failure, and a's
    cell not pending.
- **`tst_result_columns`:**
  - `columnWorkerIsUnchangedByDemand`: `count` 0 and no failure.
  - `staleRecordDeletedByWorkerCreatesDemand`: the "pending" step of its
    sequence connects `pendingCellsChanged`.
- **`tst_logbook_indicators`, as phase 3 leaves it.** Rewrite whichever of
  these reads phase 3 kept:
  - `col()`;
  - `makeWorkingColumn()`: "step 1", `count` 3, "Jump 1";
  - `makeBadgedColumn()`: s1 in `failures()`;
  - `workingColumnIds()` in `plainHeaderAndCellsAreIdenticalToBase`: `count`
    0;
  - `sessionIdsOf(col().running)` in `pendingCellsAreDistinctFromUnavailable`;
  - `col().isWorking()` / `showsWarning()` in the renamed row-warning and
    click functions and in `unreadableRecordPendingIsNotDemandPending`;
  - `col("G_OUT")` in `failedWriteIsListedInTheHover`.
- **Search** `tests/` (including `tst_status_bar`) for every name the new
  audit rule forbids. A read found there is rewritten by the same rules.

### Acceptance map (`tests/acceptance_map.txt`)

| Lines as phase 3 leaves them | After this phase |
|---|---|
| `513`, `536`, `538`, `557`, `604`, `627`, `631`, `657 tst_calculation_demand columnStateCountsAndPendingCells` | the same items, the renamed function |
| `535`, `631 tst_calculation_demand workingIdsFollowStates` | removed |
| `536`, `627`, `645 tst_calculation_demand tooltipText` | removed |
| `536 tst_calculation_demand toolTipListsAtMostTenFailures` | `536 tst_calculation_demand failureTextAndItsLimit` |
| `536`, `627`, `632 tst_calculation_demand progressUpdatesWithoutInspection` | the same items, `tst_calculation_demand progressTextWithoutAPass` |

Use phase 1's function names as the tree holds them. Every other line stays.
No item loses its last test or audit line. The following were checked
against the lines phases 2 and 3 leave:

- 535: `headerIsPlainAndNothingAnimates`, `audit demand`;
- 536: `failureTextAndItsLimit`, `sharedJobSameProgress`, `tst_status_bar`;
- 627 and 631: `changeSignalsAreMinimal`, `audit demand`;
- 632: six other functions;
- 645: `tst_status_bar`.

**Items whose tests change meaning.** Phase 5 restates these "(as amended)";
this phase changes no item text.

- **111:** "never waiting, running or failed for any fusion plot" becomes
  "never counted in progress nor listed among failures".
- **115:** "the count of done tracks rising" becomes "the sessions still to
  compute falling".
- **228:** the plot row's tooltip becomes the failure text.
- **339:** "the row shows the warning" becomes "listed among failures".
- **517, 555:** "the indicator shows it" becomes "progress counts it from the
  first change".
- **525, 526, 554, 654:** "badged" becomes "listed among failures with its
  reason".
- **536:** the hover's numbers and lists become the progress value and the
  failure text.
- **537:** the badge only once finished becomes the failure listed while
  computations continue.
- **542:** "publishes per-plot and per-column state" becomes "publishes
  progress, failures and pending cells".
- **557:** the counts on rows and headers become progress and failures.
- **604:** "classified, tallied and filed" loses "tallied".
- **618, 648:** failed "on plot rows and column headers alike" becomes a
  failure of the recording.
- **624, 656:** "the tracks name a session" becomes the failure entries and
  progress naming it.
- **627:** "the counts stay" becomes "the counts go".
- **631:** "every source's state, counts, listed tracks" becomes "progress,
  failures".
- **657:** "counts, listed tracks and tooltips are as before" becomes spec
  §13's last bullet.

### Audit (`tests/audit/cleanup_audit.cmake`, group `demand`)

Plant a hit once to prove each new or changed pattern.

- **Widened and replaced:** phase 3's "the views read no per-source state"
  becomes, over the whole tree:

  ```cmake
  expect_none("the per-source presentation stays gone",
    "DemandState|DemandTrack|DemandCondition|kToolTipListLimit|buildToolTip|workingPlotIds|workingColumnIds|plotStateChanged|columnStateChanged|statesChanged|${WB_START}(plotState|columnState)\\(|sessionIdsOf|wantedCount|doneCount|waitingCount|failedCount|showsWarning\\("
    src tests ":!tests/README.md")
  ```

  `runningCount` and `m_states` are not in this pattern: they occur
  elsewhere, in `jobqueue.*` and `tst_jobmodel.cpp`. They and `isPlain(` are
  kept out of the demand layer by the next rule; a test cannot reach them
  without a `DemandState`, which this rule forbids.
- **Added:**

  ```cmake
  expect_none("the demand layer keeps no per-source state",
    "m_columnStates|m_states${WB_END}|addTrack|runningCount|jobFailure|calculationTitles|isWorking\\(|isPlain\\(|toolTip${WB_END}"
    ${DEMAND_LAYER})
  ```

  It checks spec §10's "counts that no view reads any more are not kept".
- **Changed:** "the fill and the settle clock know nothing of the walk". Its
  `DemandTrack|DemandState|DemandCondition` becomes `TrackCondition`, and it
  keeps `BlockerReport|blockers\\(|RowStabilityGuard|PairMemory|LearnedFact`
  and phase 1's `DemandProgress|SessionFailures|FailedCalculation`. The
  removed names are kept out everywhere by the first rule.
- **Unchanged and checked:**
  - "the one walk reads the rows under one guard" (1);
  - "the demand layer reads a record's reason in one place" (1);
  - `widget-free-core`: `demandstate.*` is still in `DEMAND_LAYER`, QtCore
    only;
  - "the demand layer's replaced machinery stays gone".
- The group's header comment, its item list and the traceability ranges are
  phase 5's.

### README (`tests/README.md`)

Update only what is now false:

- **The `tst_calculation_demand` row:**
  - "per-plot state, counts, tooltip (at most ten per list), change signals"
    becomes progress, failures and their signals;
  - "per-column state and pending cells" becomes the column's progress and
    pending cells;
  - "badged" becomes "listed" where the row describes the demand layer's
    tests;
  - the renamed and removed functions are named correctly.
- **The `tst_fusion_rows` row:**
  - "the count rising as each publishes" becomes "the sessions still to
    compute falling";
  - "in no count, list or tooltip of any of the seventeen rows" becomes
    "never counted and never listed among failures";
  - "shows the warning badge" becomes "is listed among failures".
- **The `tst_fusion_store` row:** "rejection / solver failure badge" becomes
  "rejection / solver failure listed with its reason".
- **Section 8** loses "`sessionIdsOf()` for a state's track lists". Phase 5
  does not revisit section 8.

Sections 9, 10 and 12, the appendices and `docs/` are phase 5's.

## Decisions

- **The condition becomes private, as `TrackCondition`.** The overview lets
  `DemandCondition` remain if the walk uses it. It does, but only the walk
  does. `demandstate.h` is the contract of what the views read, so the type
  moves into the reconciler. That lets the audit keep all three old names
  out of `src` and `tests` with one rule. The name is fixed so that the audit
  rules can name it.
- **`DemandTrack` goes entirely.** No field outlives the tooltip except the
  condition. Phase 1 already attaches the entries to the calls that build a
  Failed track, so a struct with one field would be generality for nothing.
- **Duplicate tests are removed, not rewritten twice.**
  `progressUpdatesWithoutInspection` and `toolTipListsAtMostTenFailures`
  would assert exactly what phase 1's functions assert. Their map lines move
  to those functions.
- **Names are kept where the assertions keep their meaning.** This covers
  "Badged", "ShowsBadge" and "OnThePlotRow" in function names. Renaming them
  would re-point about forty map lines and prove nothing new; phase 3 kept
  the name of `tst_logbook_indicators` for the same reason. Only
  `columnStateCountsAndPendingCells` is renamed: its name names the removed
  query, and its subject changed.
- **"Explicit-backed" in `tst_fusion_rows` is asserted through
  `isMerelyUncomputed()`.** Without a per-plot state, the observable fact is
  that each fusion value waits on a requested calculation. The demand
  layer's own public static says so, and it inspects without running
  anything.
- **`plotId()` stays public.** The spec does not name it, and tests name
  plots by it. After this phase it has no product reader outside the demand
  layer: its `PlotModel::PlotValueIdRole` contract served the plot rows.
  Making it private would also leave that role unread, a model question
  outside this phase.

Ready with caveats:

- phase 1's test-function names (`progressTextWithoutAPass`,
  `failureTextAndItsLimit`) and phase 3's renamed `tst_logbook_indicators`
  functions are the documents' suggestions; the map lines use the names the
  tree holds;
- which `tst_logbook_indicators` reads phase 3 left is known only from the
  tree: the implementer searches;
- `plotId()` keeps a public contract with no product reader. Whether to make
  it private is left to the orchestrator.
