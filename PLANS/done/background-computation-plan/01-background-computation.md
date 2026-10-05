# Phase 1: Background computation

The whole of `PLANS/background-computation.md`, as reproduced in
`00-overview.md`, in one commit. The overview's decisions 1-17 apply as
written; this document applies them to the code as it is at `d0c054b` (the
code of `3260e24`). Nothing under `PLANS/` is staged by the phase.

**Findings against the overview (none blocking; each settled below).**

1. Overview decision 3 wants the tokens spelled "in the definition and nowhere
   else in `src`", but the walk and the manager compare a stored token with
   `off`, and the importer writes it. Settled by D1: constants beside the key
   are the one authority; the definition reads them and spells the labels.
2. With no source the pass does not walk the rows (`m_sources.isEmpty() ?
   Walk() : walkRows(...)`), yet a job kept running after its plot was
   unchecked must stop when its recording is switched off. Settled by D7.
3. The walk names the running job in `progress` for its row whatever the
   track's condition, so the pass that excludes a running job's session would
   name it once more. Settled by D7: such a job is "described by nothing".

## Purpose

A per-recording switch, the Choice attribute `_COMPUTE`, read in one place:
the demand layer, which classifies a track of a session that reads `off` as
`Excluded` before any other condition, for loaded and unloaded sessions, plot
and column tracks alike. Nothing is computed for such a recording in the
background, a job running for it is cancelled with a reason, what was
computed earlier is kept and shown, and a logbook cell without a value reads
"excluded". The attribute has a column with a header tooltip, "Set
Compute...", an import preference, a constant default `on`, and an entry in
the logbook index so that a session is never loaded to learn it is off. One
phase, one commit (overview, Phases).

## Dependencies

Depends on nothing and blocks nothing. It consumes the code at `3260e24`:
the reconciler's walk, pass and slots; `DemandFill::update()`; the executor's
`cancel(JobId)`, `requestStop()` and `withdrawChosenNext()`; the manager's
`saveSession()`, `loadSessionRaw()`, `flushIndex()`, `remapSessionId()`,
`removeSession()` and `reset()` with `m_recordReasons` as the pattern; the
model's Choice paths and `headerData()`; `AttributeDefinition`;
`Calculations::addConstantDefault()`; `DataImporter::applyCreationDefaults()`;
the delegate's PENDING CELL; the fixtures `ChoiceFixture`, `JobWorld`,
`PlotFixture` and `logbookprobe.h`.

## What changes

### 1. The attribute (spec §3 first three bullets; decisions 3, 9, 13)

- `src/sessiondata.h`: `SessionKeys::Compute` (`"_COMPUTE"`) after
  `Orientation`, with `ComputeOn` (`"on"`) and `ComputeOff` (`"off"`) beside
  it (D1); the comment says the demand layer reads it and any token but `off`
  is on.
- `src/attributeregistry.h`: `AttributeDefinition` gains a last member
  `QString tooltip;` (empty default, so no other registration changes), the
  column header's tooltip.
- `src/calculations/attributeregistration.cpp`: the definition `{"Session",
  "Compute", SessionKeys::Compute, Choice, true, QString(), {{ComputeOn, "On"},
  {ComputeOff, "Off"}}, "compute results for this recording in the
  background"}` after "Exit Time". `src/calculations/attributecalculations.cpp`:
  `addConstantDefault(registry, QString::fromLatin1(SessionKeys::Compute),
  QString::fromLatin1(SessionKeys::ComputeOn))` beside the wind.
- `src/sessionmodel.cpp`, `headerData()`: `Qt::ToolTipRole` for a
  `SessionAttribute` column whose definition's `tooltip` is not empty; all
  else as today. `QHeaderView` shows it without code of its own.
- Nothing else: `data()`, `setData()`, `startBulkEdit()`, the CHOICE EDITOR,
  `LogbookView::askAndSetAttribute()` ("Set %1...") and the Add Column dialog
  serve the definition as they serve the Orientation. No default column set
  changes.

### 2. The import preference (spec §3 fourth bullet; decision 11)

- `src/preferences/preferencekeys.h`: `ImportCompute` (`"import/compute"`),
  holding the token `on` or `off` as text, the mechanism of
  `ImportOrientation` (D11), commented in its style; registered with
  `SessionKeys::ComputeOn` as its default in `src/mainwindow.cpp` beside
  `ImportOrientation`; mirrored `on` in `tests/support/testenvironment.cpp`
  (writes nothing, so fixtures stand).
- `src/preferences/importsettingspage.h/.cpp`: a group "Background
  computation" after the Orientation group, one check box row "Compute newly
  imported recordings in the background" (`addRow(widget)`, per
  `docs/PREFERENCE_PAGES.md`): checked unless the stored text is exactly
  `ComputeOff`; `saveSettings()` writes `ComputeOn` or `ComputeOff`.
- `src/dataimporter.cpp`, `applyCreationDefaults()`: step 8 in step 7's
  shape: when `!session.hasStoredAttribute(SessionKeys::Compute)` and the
  preference's text is exactly `ComputeOff`,
  `setAttribute(SessionKeys::Compute, ComputeOff)`; any other value, `on`,
  empty or invalid, writes nothing, as step 7's empty orientation does. The
  doc comment in `dataimporter.h` lists it.

### 3. The index entry (spec §5; decision 7)

`src/logbookmanager.h/.cpp`: per session, `"computeOff": true` in its
`index.json` entry, written by `flushIndex()` after `recordReasons` only for a
session whose file was last seen to read `off`; absent means on (D2). Public
`bool isComputeOff(const QString &sessionId) const`, a map lookup. Learned in
two places and no other: `saveSession()` (under the file's SESSION_ID, after
the write) and `loadSessionRaw()` (under the id asked for, after the read),
comparing `storedAttribute(SessionKeys::Compute)` with `ComputeOff`; a value
that differs from what is held sets `m_indexNeedsFlush`, an equal one changes
nothing, nothing is announced. `initialize()` reads it; `remapSessionId()`
carries it; both branches of `removeSession()` and `reset()` drop it. The
class comment's `index.json` paragraph names it. No new flush point.

### 4. The demand layer (spec §4, §5; decisions 5, 6, 12, 14, 15)

`src/calculationdemand.h/.cpp`.

- `TrackCondition::Excluded`, the sixth value. Decided at the row, before the
  source loop, by one exclusion reader (D6): `sr.session->storedAttribute(SessionKeys::Compute)`
  equal to `ComputeOff` when `loaded`, else `LogbookManager::instance().isComputeOff()`.
  An excluded row files no candidate and contributes nothing to
  `pendingCells`, `pendingSessions`, `loadCandidates`, `progress.count` or
  `failures`; its column tracks go to the new `Walk::excludedCells`
  (parallel to `pendingCells`). Read at every pass, memoized nowhere, never a
  `PairMemory`.
- `isCellExcluded(sessionId, columnId)` and `isCellExcluded(row, column)`
  in `isCellPending()`'s shape, from `m_excludedCells` stored by
  `applyValues()`; `pendingCellsChanged(columnId)` is emitted when either set
  of the column differs (its comment says so).
- `onDependencyChanged()`: a key equal to `attribute(SessionKeys::Compute)`
  is a change of demand: no `forgetSession()`, no `m_settle->start()`;
  `recompute()` at once when the executor has a chosen next or a running job
  and `m_reconciling` is false, else `scheduleUpdate()`. Whether the check
  precedes `isRelevantName()` is the implementer's.
- `recompute()`: after the walk and `m_fill->update()`, when the running job
  is live (`running.id != 0 && !running.cancelRequested`) and the reader says
  its session is off, `m_queue->cancel(running.id, tr("Switched off for this
  recording"))`, whether or not the walk ran (D7); the executor's
  `jobCancelRequested` schedules the next pass as today. In that pass the walk
  does not name the job in `progress` (D7).
- Switching on needs no code; the settle wait is not touched (D5).
- The class comment: TRACK CONDITIONS gains the row `session reads off |
  Excluded`, decided first for loaded and unloaded alike; THE CHOICE's "The
  running job is never stopped here" gains "except when its recording is
  switched off"; THE ONLY CALLER names `cancel()`; PRESENTATION the excluded
  cells; WHEN A PASS RUNS the Compute change. `demandfill.h` is unchanged:
  `update()` releases a hold whose session is not pending.

### 5. The executor (spec §4 "Switching off"; decision 4)

`src/jobqueue.h/.cpp`: `bool cancel(JobId id, const QString &reason)`; the
reason, never empty, is the end's reason for a chosen next job (ended at
once) and for the running job (`requestStop({JobState::Cancelled, reason})`);
the fixed `tr("Cancelled")` goes (D10). The doc comment says the demand layer
is the one product caller, for a recording switched off, and that the jobs
dock may call it too. LIFECYCLE, the first-writer rule, `withdrawChosenNext()`,
`offer()`'s replacement and `shutdown()` are unchanged.

### 6. The excluded cell (spec §4 third bullet; decision 8)

`src/ui/docks/logbook/LogbookCellDelegate.h/.cpp`: a fourth look, painted
when `isCellExcluded(row, column)` and the display text is empty (a value
always wins, checked first as for pending): `excludedText()`
(`tr("excluded")`) in the pending mark's muted colours through `cellOption()`;
`excludedToolTip()` (`tr("Not computed: background computation is switched
off for this recording")`) served in `helpEvent()` where the pending tooltip
is; `showsExcluded(index)` beside `showsPending()`. A cell is never both.
PENDING CELL lists the four looks and says the excluded cell is, like the
pending one, never in the model, the cached values, `index.json` or `sort()`.

### 7. What does not change

The engine, the stores, the column worker, the fusion library and the kernel
read nothing of the attribute (the audit pins the readers). The compatibility
marker and `Fusion::Algorithm` are untouched; no golden moves.
`demandfill.*`, `demandsettleclock.h`, `demandstate.h`, `StatusBarFeature.cpp`,
`PlotWidget.cpp`, `LegendPresenter.cpp`, `LogbookView.cpp`,
`addcolumndialog.cpp`: unchanged.

### 8. Order of work

1. Sections 1-3 and 5; `tst_jobqueue`, `tst_importer`, `tst_logbook_index`.
2. Sections 4 and 6; `tst_calculation_demand`, `tst_logbook_indicators`.
3. The audit, the map, the README, the documents; `audit_cleanup` and the
   whole suite, sequentially, in `build-agent/`.
4. M57 is written, not run (criterion 14); the report says so.

## Interfaces

Provided, as the overview fixes them, plus this document's names:

- `SessionKeys::Compute` `"_COMPUTE"`, `ComputeOn` `"on"`, `ComputeOff`
  `"off"`; labels "On", "Off" in that order; category "Session", display name
  "Compute", editable; `builtin.default._COMPUTE` with value `on`.
- `AttributeDefinition::tooltip`; "compute results for this recording in the
  background" as the Compute header's `Qt::ToolTipRole`.
- `PreferenceKeys::ImportCompute` `"import/compute"`, the token `on` or
  `off` as text, default `on`; the page text "Compute newly imported
  recordings in the background".
- The `$VAR,_COMPUTE,<token>` line: written by `setData()`, a bulk edit, and
  the importer while the preference is off; never removed.
- `LogbookManager::isComputeOff(const QString &) const`; the entry
  `"computeOff": true`, present only when off.
- `TrackCondition::Excluded`; `CalculationDemand::isCellExcluded()` in both
  forms; `pendingCellsChanged(columnId)` covering both sets.
- `JobQueue::cancel(JobId, const QString &reason)`; the reason "Switched off
  for this recording"; "No longer needed" for a withdrawn or replaced chosen
  next job, as today.
- `LogbookCellDelegate::showsExcluded()`, `excludedText()` ("excluded"),
  `excludedToolTip()`.
- Audit group `background-computation`; items 1501-1524; appendix P; matrix
  9.16; M57 in 12.14.

Consumed: nothing beyond the code at `3260e24`.

## Acceptance criteria

Each is traced to the specification (§) and its item (D3).

1. (§2 first bullet, §3 first; 1501) The registry's `_COMPUTE` definition is
   category "Session", "Compute", `Choice`, editable, choices `on`/"On" then
   `off`/"Off", registered by `registerBuiltInAttributes()`; `_COMPUTE` and
   `SessionKeys::Compute*` occur in `src` only in `sessiondata.h`,
   `calculationdemand.cpp`, `dataimporter.cpp`, `logbookmanager.cpp`,
   `attributeregistration.cpp`, `attributecalculations.cpp`,
   `mainwindow.cpp` and `preferences/importsettingspage.cpp` (the last two
   for the token constants the preference holds, D11).
2. (§3 second; 1502) A session without the line displays "On", its file
   gains no `$VAR,_COMPUTE` line and, while the Compute column is enabled,
   `index.json` caches `on` for its stub;
   `setData()` writes `on` or `off` verbatim and refuses any other value; no
   edit removes the line; a token outside the list is displayed as written
   and the session is computed.
3. (§2 second, §3 third; 1503) Setting a token other than `off` while a job
   of the session runs leaves `cancelRequested` false; the stored records,
   the cached column values and every other session's results survive;
   `isSettling()` is false; one pass follows (the change's own; this does
   not count passes the executor's signals schedule in other tests);
   `CalculationCompatibilityVersion` and `Fusion::Algorithm` are unchanged.
4. (§3 fourth; 1504) The Compute header answers `Qt::ToolTipRole` with the
   fixed text and the description header answers an invalid variant; "Set
   %1..." yields "Set Compute..." over the definition's list; a bulk edit of
   stubs writes the token with every row still a stub.
5. (§3 fifth; 1505) The preference is registered `on`; while it reads
   `off` an imported file without the line stores `off` and one with its own
   line keeps it; while it reads `on`, or anything but `off`, nothing is
   stored; the page's box round-trips (M57).
6. (§4 first; 1506) For a plot checked while the session is visible, a
   session shown while a plot is checked, and a column enabled, the session
   reading `off` gets no job, no load and no hold, is absent from
   `progress().count` and `failures()`, and its cells are `isCellExcluded()`
   and not `isCellPending()`; the other sessions compute.
7. (§2 fourth, §4 second; 1507) A session reading `off` with a stored result
   restores it when loaded and its column fills from the record without a
   load; its stored rejection is absent from `failures()` and shows no glyph;
   switched on, it is listed and no job runs.
8. (§4 third; 1508) The excluded cell paints `excludedText()` muted and
   differs from the base delegate's cell and from a pending cell; its tooltip
   is `excludedToolTip()`; a cell with a value paints it; `index.data()`,
   `cachedValues`, `pendingColumns` and `index.json` carry nothing of it; it
   sorts with pending and blank cells; `pendingCellsChanged(id)` repaints that
   column only.
9. (§4 fourth; 1509) Switching off while the session's job runs ends it
   `Cancelled` with reason "Switched off for this recording" after
   `jobCancelRequested`, no record written and nothing published, the hold
   released and the next candidate started after `jobFinished`; while its
   pair is the chosen next job, that job is `Cancelled` "No longer needed"
   before control returns to the event loop; `remembered()` of the session is
   unchanged.
10. (§4 fifth; 1510) Switching on creates demand for the session's missing
    results only; a cancelled compute function is entered again from its
    start; a failure remembered for the pair still keeps it from being
    offered.
11. (§4 sixth, seventh; 1511) `isMerelyUncomputed()`, `isNotYetComputed()`,
    `DemandProgress`, `DemandFill::kMaxHeldSessions`, `JobQueue::kMaxRunningJobs`,
    the settle clock and `StatusBarFeature.cpp` are unchanged in text;
    `progress().sessionName` is empty in the pass that cancels.
12. (§5; 1512) `isComputeOff()` is true after `saveSession()` of a session
    reading `off` and after `loadSessionRaw()` of a file carrying the line,
    false after a save without it; `flushIndex()` writes `"computeOff": true`
    for such entries only; a restart reads it back; a remap carries it, a
    removal and `reset()` drop it; an index without it reads on; a bulk edit
    of a stub to `off` excludes it at the next pass without a load.
13. (§6 bullets 1-9; 1513-1521) Each bullet has the tests named below.
14. (§6 tenth; 1522) M57 is written in `tests/README.md` 12.14 for Michael:
    it observes the status bar, a cell, a row and a plot in the running
    application, which no agent can do. The phase is complete without it,
    and the report says it was not run.
15. (§6 eleventh; 1523) The gestures rule reads "the demand layer is the one
    product caller of cancel" (`expect_only` and a count of one on
    `calculationdemand.cpp`); items 1501-1524 each have a test or audit line;
    `audit_cleanup` and the whole suite green.
16. (§7; 1524) The documents say what "Documentation" lists; the audit pins
    the sentences it can.

## Tests

Run in `build-agent/` only, sequentially, Release.

**`tst_calculation_demand`** (new unless marked; s2 reads `off` through
`updateAttribute()` and a save, or through a Compute column where the test
says so).

- `excludedSessionIsNotComputed` (`_data`; criteria 6, 13): rows "plot
  checked while visible", "shown while checked", "column enabled"; s1 and s3
  compute (`startOrder()`), s2 is never offered, loaded or held, excluded
  from `count`, its cell excluded and not pending.
- `switchingOffCancelsTheRunningJob` (criteria 9, 11): the gate holds s1's
  job; `off` on s1 through a Compute column's `setData()`;
  `jobCancelRequested`, `Cancelled` with the reason, no `G_OUT`, no record,
  `heldSessionIds()` without s1, `progress().sessionName` empty, the next
  candidate entered.
- `switchingOffWithdrawsTheChosenNextJob` (criterion 9): s2 chosen next
  behind s1 running; `off` on s2 with no event loop: `Cancelled`
  `kNoLongerNeeded` on return.
- `switchingOffWhileSettlingLeavesNothingWaiting` (criterion 13): an input
  edit of s2, then `off`; `count` 0 and no candidate after `settle()`.
- `switchingOnCreatesDemandForItsMissingResults` (criterion 10): `off` then
  `on`: s2's input appears a second time in `startOrder()`, no other session
  re-runs; with a job-level failure remembered for s2 the switch offers
  nothing.
- `excludedSessionWithStoredResultServes` (criterion 7): a record planted as
  `storedResultsCreateNoJob` does and a rejection as
  `storedRejectionIsAFailureWithoutLoad`, the session `off`: the column fills
  from the record, no load, `failures()` empty; `on` lists it with no job.
- `settingComputeInvalidatesNothing` (criterion 3): `on` set explicitly on a
  session whose job runs: not `cancelRequested`; records and cached values
  intact; `isSettling()` false; `passCount()` advanced by one.
- `fillNeverLoadsAnExcludedSession` (criterion 12): every stub `off` through
  a bulk edit of the Compute column (`bulkEditMakesSettledSessionApplicable`'s
  path); `isComputeOff()` true with every row a stub; the column enabled: no
  `sessionLoaded`, no job, `hasFillWork()` false, every cell excluded.
- `offOnDiskButAbsentFromIndexIsLoadedOnce` (criterion 12): the index edited
  to drop the entry (as `restartWithoutRecordReasons()` drops
  `recordReasons`); one `sessionLoaded`, no job, the hold released, and
  after eviction the stub is excluded from the learned entry.
- `computeColumnShowsTheDefaultWithoutAWrite`, `computeEditStoresATokenAndRefusesOthers`,
  `computeBulkEdit`, `handEditedTokenIsShownAsWrittenAndReadAsOn` (criteria
  2, 4; D4): the Orientation tests' shape through `ChoiceFixture` over
  `SessionKeys::Compute`, loaded and stub rows, plus `headerData(column,
  Qt::Horizontal, Qt::ToolTipRole)` on the Compute and description columns.
  They tear down the harness's demand layer, executor and model first and
  build the fixture over a fresh logbook; `cleanup()` resets the fixture
  before the registry check.
- `importedWhilePreferenceOffIsExcludedAtOnce` (criterion 5): preference
  `off`; a session imported through `mergeSessions()` with creation
  defaults (`ParsedFile::applyCreationDefaults`; else the line planted as the
  importer writes it) is excluded for an enabled column and a checked plot in
  the same pass; back to `on`, an import carries no line.
- Unchanged in text: `hidingASessionDropsItsWaitingPair`,
  `uncheckingDropsWaitingPairsKeepsRunning`, `progressCountsEachSessionOnce`,
  `failuresFollowWhatIsSwitchedOn`, `pendingCellsChangedPerColumn`.

**`tst_importer`.** `computeFromThePreference` (new; criterion 5), in
`orientationFromThePreference`'s shape, with a row for a preference that is
empty or not a token: nothing written.

**`tst_logbook_index`.** `computeOffRoundTrip` (new; criterion 12): save,
flush, the JSON of the off entry only, restart; a file edited on disk to
carry the line learned by `loadSessionRaw()`; a save without the line clears
it and marks a flush; remap and removal as `remapAndRemoveCarryMarks`; an
index without the entry (as `legacyFlatIndexStartsAsStubs`) reads false.

**`tst_logbook_indicators`** (new; criteria 7, 8), in
`pendingCellsAreDistinctFromUnavailable`'s shape:
`excludedCellIsDistinctFromPendingAndUnavailable`, `excludedCellShowsItsValue`,
`sortingTreatsExcludedAsUnavailable`, `excludedSessionShowsNoRowWarning`,
`excludedCellsChangeRepaintsOnlyThatColumn`.

**`tst_jobqueue`** (amended; criterion 9): `cancelRunningThenNextStarts`,
`cancelQueued`, `cancelIgnoredForOneStepStillCancelled`,
`userCancelThenStaleEndsCancelled`, `staleThenUserCancelEndsSuperseded` pass a
reason; the first two assert `record.reason`. **`tst_fusion_jobs`**:
`cancelDuringFitThenNextJobStarts` passes a reason; no new fusion test (D8).
**`tst_status_bar`**: unchanged; `computationsAreOneItem` cited for 1511.

**Audit: group `background-computation`** in `cleanup_audit.cmake`, in
`staged-scale`'s shape with `Allow:` comments, each rule planted once; a head
bullet; the traceability comment and a `foreach(item RANGE 1501 1524)`:

- `"\"(on|off)\""` only in `src/sessiondata.h`; `"\"(On|Off)\""` only in
  `src/calculations/attributeregistration.cpp` (both over `src`);
- `_COMPUTE|SessionKeys::Compute(On|Off)?${WB_END}` only in criterion 1's
  eight files (`src`);
- `"Switched off for this recording"`, `"excluded"` and the tooltip text once
  each in `src`; `"computeOff"` on exactly two lines of
  `src/logbookmanager.cpp`; the page text once in `importsettingspage.cpp`;
- `docs/COMPUTED_PLOTS.md`: `Set Compute...` once; `docs/CALCULATIONS.md`:
  `Switched off for this recording` once; `tests/README.md`: `M57` at least
  once (behind 1522's audit line).

Amended in place: group `gestures`, `expect_none("no product code cancels a
job" ...)` becomes `expect_count(... 1 src)` plus `expect_only(...
"^src/calculationdemand\\.cpp$" src)`, named "the demand layer is the one
product caller of cancel"; the comment and name of `expect_count("cancel is
kept for the jobs dock" "bool cancel\\(JobId" 1 src/jobqueue.h)` restated
(declared once). Group `demand`: the regex of "pending is the view's
presentation of demand" gains `isCellExcluded|showsExcluded|excludedText\\(|excludedToolTip\\(`.

**`tests/acceptance_map.txt`.** Header: "Sixteen specifications, sixteen
item ranges", a `1501-1524` paragraph in `1401-1415`'s form (it amends
501-563: item 519; 601-662: items 629 and 633); those three restated "(as
amended)" (D9). A last section "Background computation per recording, the
Compute attribute (PLANS/background-computation.md): twenty-four items":
1501-1512 as the criteria state them, 1513-1523 the eleven bullets of §6 in
order, 1524 §7; `1505 manual M57` beside its tests, `1522 audit
background-computation` and `1522 manual M57`.

**`tests/README.md`.** Section 1: the rows of `tst_calculation_demand`,
`tst_logbook_indicators`, `tst_importer`, `tst_logbook_index` and
`tst_jobqueue` gain their proofs by name. 9.16 "Background computation per
recording (items 1501-1524)" in 9.15's form; 9.6's row 519 and 9.7's rows 629
and 633 restated. Section 10: a `background-computation` bullet; the
`gestures` bullet's "product code cancels a job" reworded; the `demand`
bullet's names. Section 12: "Fourteen scripts"; 12.14 with **M57 The Compute
switch (1505, 1522)**: a logbook with a long-running recording and a column
over a fusion output; the recording switched off mid-fit (the status bar's
count drops and its item ends, the cell reads "excluded" with its tooltip, no
row warning, the plot's track absent, the file carries the line); switched on
(the fit runs from its start); half the recordings off and the column
re-enabled (the others loaded and fitted, those never loaded); Preferences >
Import: the box unchecked, the dialog reopened (still unchecked), one
recording imported (its file carries `off`, its cell "Off", nothing
computed), the box checked again. Appendix P lists the twenty-four items in
appendix O's form; appendix F's item 19, appendix G's items 29 and 33
restated.

**Documentation.**

- `docs/COMPUTED_PLOTS.md`: a section "Switching a recording off" (the
  column, what it does and keeps, the cell, "Set Compute...", the import
  preference); sections 1, 2, 3 and 4 where they say every recording is
  computed or counted ("that is switched on"); section 5 (a running computation stops when its
  recording is switched off); section 6 (changing Compute discards nothing);
  section 9's limitation ("a switch per recording and none for all").
- `docs/CALCULATIONS.md`: 15.3's first bullet and 16.11's executor sentence
  (the one product caller, the reason); 16.1 the `Excluded` row decided
  first and the reading for loaded and unloaded sessions; 16.2 the excluded
  cells and the signal; 16.3's bulk-edit passage (the Compute change arrives
  under its own name and runs a pass without a wait); 16.4 the stop of the
  running job; 16.7 (an exclusion is not a memory); 16.8 the fill and the
  recovery of an index without the entry; 16.9 the cancel call; 16.10 the
  cell's third text; the registration passage names Compute as the core's.
- `docs/DATA_SCHEMA.md` section 9: `$VAR,_COMPUTE,<token>` beside the
  Orientation; section 11: `"computeOff"` beside `recordReasons` (derived,
  additive, optional, absent means on, learned at save and load).
- `docs/SENSOR_FUSION.md` section 2: a bullet beside the Orientation's.
- `docs/PREFERENCE_PAGES.md`: unchanged.

## Decisions

- **D1. The tokens are constants beside the key** (`SessionKeys::ComputeOn`,
  `ComputeOff`); the definition, the default, the importer, the walk and the
  manager read them, and the audit pins the literals to `sessiondata.h`.
  Overview decision 3's intent, met where a comparison can read it.
- **D2. The index entry is `"computeOff": true`**, present only when off:
  additive, absent means on, and an index written by this build looks like an
  old one for every on session. `isComputeOff()` names what the index holds.
- **D3. Item numbering**, twenty-four items: 1501-1512 as the criteria order
  them; 1513-1523 the eleven bullets of §6; 1524 §7.
- **D4. The Compute column's tests go in `tst_calculation_demand`**, the
  attribute's reader (overview decision 10), through `ChoiceFixture` after
  tearing the harness down; `tst_result_columns` is about columns over
  explicit results. "Set Compute..." stays the generic path
  (`tst_choice_attribute::setDialogOffersTheList`); no dialog test.
- **D5. The settle wait is not ended.** An excluded session is no candidate
  whatever the clock says; its wait expires into a pass that finds nothing,
  and a session switched on inside the wait is offered once its inputs are
  still.
- **D6. One exclusion reader** in the reconciler, called per row by the walk
  and once by the pass for the running job's session; the engine is not
  asked (overview decision 6).
- **D7. The cancel is made in the pass, outside the walk, whether or not the
  walk ran**; a running job whose session is excluded is "described by
  nothing", so progress never names an excluded recording.
- **D8. No new real-fit test.** `cancelDuringFitThenNextJobStarts` proves the
  fit's cancellation through `cancel()`; the switch's path is the synthetic
  suite's; M57 observes the real fit.
- **D9. Restated items**: 519 (the running job is stopped when its recording
  is switched off), 629 (the one product caller), 633 (the exclusion first in
  the unloaded order). 534 and 703 stand: the user has no cancel control (the
  switch is a change of demand), and an excluded track is neither waiting nor
  running.
- **D10. The cancel's reason is required**, not defaulted: one caller, one
  reason, no generality nothing uses.
- **D11. The preference holds the token, not a boolean** (Michael, at the
  plan review): the orientation preference's mechanism, one vocabulary. The
  importer writes the line only when the text is exactly `off`, so an
  absent, empty or invalid value writes nothing; a boolean read of an
  unregistered key would have converted to false and excluded every
  import in a context that lacked the registration.

Ready.
