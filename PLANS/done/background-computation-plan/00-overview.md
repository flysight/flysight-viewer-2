# Implementation plan: background computation per recording, the Compute attribute

Plan for `PLANS/background-computation.md`, written 2026-10-04 against
`store-requested-calculations` at `d0c054b`, the specification's own commit;
the code is `3260e24`'s, and nothing else is between. The specification
below is the authority; where the phase document disagrees with it, the
specification wins, except where a decision below states how the plan reads
it.

## Feature specification

## Background computation per recording: the Compute attribute

Date: 2026-10-04.
Status: specification for planning. Not an implementation plan. Small enough
to be implemented directly by one agent in one phase.
Baseline: branch `store-requested-calculations` at `3260e24`; the committed
code is authoritative.
Related: `src/calculationdemand.h/.cpp` (the reconciler: the walk, the track
conditions, the pair memory, the choice), `src/demandfill.h` (the column
fill), `src/demandstate.h` (progress, failures, pending cells),
`src/jobqueue.h/.cpp` (the executor, `cancel(JobId)`),
`src/logbookmanager.h/.cpp` (the index: `records`, `recordReasons`),
`src/sessionmodel.h/.cpp` (Choice cells, `startBulkEdit`),
`src/ui/docks/logbook/LogbookCellDelegate.h` (the pending mark, the row
warning), `src/ui/docks/logbook/LogbookView.h` ("Set ..."),
`src/attributeregistry.h` and `src/calculations/attributeregistration.cpp`
(the built-in attribute definitions), `src/fusion/fusionregistration.cpp`
(the Orientation definition and its constant default, the pattern to
follow), `docs/CALCULATIONS.md` sections 15 and 16, `docs/COMPUTED_PLOTS.md`,
`docs/DATA_SCHEMA.md` sections 9 and 11, `docs/SENSOR_FUSION.md` section 2,
`tests/audit/cleanup_audit.cmake` (group `gestures`: "no product code cancels
a job").

### 1. Motivation

Requested calculations follow demand: a checked fusion plot or an enabled
column over a fusion output wants a result for every track it covers, and
the demand layer computes what is missing, one recording at a time, for as
long as the plot stays checked or the column enabled. There is no way to
leave one recording out. A logbook of a hundred recordings with a column
over a fusion output fits all hundred; a recording the user knows will not
converge, or does not care about, is fitted all the same, and a recording
whose fit is known to take an hour holds the worker while the recordings
the user is looking at wait behind it. The only remedies today are to
uncheck the plot or disable the column, which drop every recording at once.

The remedy is a per-recording switch, stored with the recording as the
Orientation is: on, the recording takes part in background computation as
it does today; off, nothing is computed for it in the background, a fit
already running for it is stopped, and what was computed earlier is kept
and shown. The switch is read in one place, the demand layer, which is the
only starter of requested calculations, so it covers the sensor fusion fit
and every background calculation added later without a switch of its own.

### 2. Principles

- **One switch, one reader.** The attribute is read by the demand layer
  alone, when it decides what is wanted. The engine, the executor, the
  column worker, the stores and the kernel know nothing of it. A future
  requested calculation is covered by construction.
- **Demand, not data.** The attribute is not an input of any calculation.
  Changing it never invalidates a result, stored or in memory, never starts
  a settle wait, and never makes a running job stale. It changes what is
  wanted, exactly as checking a plot or showing a track does, and the
  running job is stopped for that reason alone.
- **Off is an explicit intent.** Hiding a track is momentary, and a running
  fit for it finishes and is kept (the demand specification). Switching a
  recording off is a decision about that recording: the fit running for it
  is stopped at its next boundary, and nothing of it is published or
  stored.
- **What exists is shown.** A stored result of a recording that is switched
  off is restored and drawn like any other. Restoring is not demand.
- **Nothing happens by default.** The attribute is absent from every
  recording on disk today, and absent means on. No file is rewritten, no
  index is discarded, no stored result is dropped; the compatibility
  marker and the algorithm strings are untouched.
- **On-demand calculations are not gated.** An instant calculation (a
  value derived from stored outputs, a column that needs no computing) is
  computed for a recording that is switched off as it is for any other.

### 3. The attribute

- **Key, type, values.** A session attribute `_COMPUTE` (`SessionKeys`), a
  Choice of two tokens, `on` (label "On") and `off` (label "Off"), in that
  order. Its definition is a built-in of the core (category "Session",
  display name "Compute", editable), registered beside the other built-in
  definitions, not by the fusion library: the switch is the demand layer's,
  not the fit's.
- **Storage.** Stored with the session like the Orientation (DATA_SCHEMA
  section 9): a `$VAR,_COMPUTE,<token>` line, written when the user sets
  it, never removed by an edit, saved verbatim. A recording without the
  line reads the constant default `on`, through a constant default of the
  attribute (as `builtin.default._ORIENTATION` serves the Orientation), so
  the column shows "On" for every recording that has never been set and
  the session file is not touched by showing it. A hand-edited token
  outside the list is kept and shown as written; for the demand layer any
  token but `off` is on.
- **Not an input.** No calculation declares it. Setting it emits the
  model's ordinary change for the attribute's name; the demand layer
  observes that change as a change of demand (a pass), and nothing else
  reacts: no result depends on the name, so the engine invalidates nothing
  but the attribute's own value, the executor's running job is not stale,
  and no settle wait starts.
- **The column.** "Compute", header tooltip "compute results for this
  recording in the background", a Choice column like the Orientation: its
  cell shows the label of the effective token, is edited in place with the
  list, and the logbook's context menu offers "Set Compute..." for the
  selected recordings with the same list, applied as a bulk edit; a bulk
  edit of a recording that is not loaded takes the existing stub path (edit,
  save, index) without loading it. The column is not in the default column
  set; the user adds it (Add Column, "Session").
- **The import preference.** Preferences > Import gains "Compute newly
  imported recordings in the background", on by default, beside the
  orientation. It works as the orientation's does: a recording imported
  while it is off is written with `$VAR,_COMPUTE,off`, a fact of the import
  the user can change per recording afterwards; while it is on nothing is
  written, and the recording reads the default. It serves the user who
  wants positive control: new recordings compute nothing until switched on
  one by one or with "Set Compute...". Changing the preference touches no
  recording already imported.

### 4. What changes in the demand layer

- **An excluded track.** A track (a session of a source, the demand
  specification's term) whose session reads `off` is **excluded**: a new
  condition beside `Done`, `Failed`, `NotApplicable`, `Waiting` and
  `Running`, decided before any other, for loaded sessions and sessions
  that are not loaded alike, for plot tracks and column tracks alike. An
  excluded track is never a candidate, never counted in progress, never an
  entry of the failures, and never pending. A session all of whose tracks
  are excluded is, to the demand layer, a session that wants nothing: not
  loaded by the column fill, not held, not offered.
- **Stored results still serve.** The engine restores a stored result of an
  excluded session when it is loaded, as today; its plots draw, its columns
  fill from the record, and its derived values compute on demand. A stored
  rejection or solver failure of an excluded session is not listed in the
  failures and shows no row warning: the warning is about what is wanted,
  and nothing is wanted of the recording. Switching it on again lists the
  failure as before, without computing.
- **The cell of an excluded track.** A requested column's cell of an
  excluded session that has no value reads "excluded", muted like the
  pending mark and with the tooltip "Not computed: background computation
  is switched off for this recording", where it would otherwise read "···"
  or be blank. A value always wins: the cell of an excluded session with a
  stored result shows the value. It is a presentation of demand, like the
  pending mark: never a cached value, never written to the index, and
  sorting treats the cell as it treats a pending one.
- **Switching off.** A pass runs. Every waiting pair of the session leaves
  demand: a chosen next job for it is withdrawn ("No longer needed", as
  today). The **running job** for it, if any, is asked to stop through the
  executor's cancellation: it ends Cancelled with the reason "Switched off
  for this recording" at its next boundary, publishes nothing and stores
  nothing (cancel wins over a late result, as today), and the next job
  starts after the worker is joined. The demand layer is the executor's one
  product caller of `cancel()`; the executor's lifecycle, its first-writer
  rule for the pending end and its shutdown are unchanged. A held session
  whose last pending cell was excluded releases its hold and leaves the
  hidden pool by ordinary eviction. The pair memory is not touched: a
  failure remembered for the session is still remembered, and an exclusion
  is not a memory, it is read from the attribute at every pass.
- **Switching on.** A pass runs; the session's tracks are classified as any
  other's, and its missing results enter demand under the ordinary
  priority, from the start: a fit that was stopped runs again from its
  beginning. A failure remembered for the pair in this run still keeps it
  from being offered, as it would any other pair.
- **The plot widget's "no data" warning** keeps asking the engine and
  stays silent for an excluded track whose result is missing, because the
  engine still reports the name `Blocked`; nothing changes there.
- **Priority, the settle wait, the fill's bound, the executor's limits and
  the status bar's form** are unchanged. The status bar's counts and lists
  simply do not contain excluded sessions, and the fill never loads one.

### 5. Sessions that are not loaded

The demand layer decides an excluded track without loading the session:
loading it would be the background work the switch forbids. For a loaded
session the attribute is read from the session; for one that is not loaded
it is read from the logbook index, which records, per session, whether the
attribute is `off`: a fact about what is on disk, derived, optional and
additive, beside `records` and `recordReasons` (DATA_SCHEMA section 11).
The index learns it wherever it reads or writes the session's file: the
import, a save (the bulk edit's stub path included), a load, and the column
worker's temporary copy. An index without the entry means on, which is what
every existing logbook reads. The model already announces a bulk edit of a
recording that is not loaded under the attribute's own name (CALCULATIONS
16.3), so the edit takes effect at the next pass without a load.

The session file is the authority; the index entry is a cache of it. An
index that is missing or discarded is rebuilt as it is today: the filename
scan, then the column worker's background pass over every session file,
which relearns the entry. Until the worker reaches a session whose
attribute is `off`, the entry is absent and the session reads as on, so
the column fill may load it once as a hidden session; the loaded session
then reads `off` from its own attribute, no job starts for it, and its hold
is released. The recovery costs at most one hidden load per such
recording, never a fit.

### 6. Tests

- A session switched off is not computed when a plot is checked while it is
  visible, when it is shown while a plot is checked, or when a column over a
  requested output is enabled; the other sessions are; progress never counts
  it; its column cell reads "excluded" and is not pending.
- Switching a session off while its job is running ends the job Cancelled
  with the reason, publishes and stores nothing, releases its hold, and the
  next candidate runs; switching off while its pair is the chosen next job
  withdraws it; switching off while the session settles after an edit
  leaves nothing waiting.
- Switching a session on creates demand for its missing results and no
  other; a stopped fit runs again from its start.
- A session switched off with a stored result restores and draws it, fills
  the column from it, and is not loaded again for it; a stored rejection of
  a session switched off is not listed among the failures and shows no row
  warning, and is listed again when the session is switched on.
- Setting the attribute invalidates no stored result and no cached value
  over a requested output, starts no settle wait and makes no running job
  stale; the compatibility marker and the algorithm string are unchanged
  (the goldens and the stored-result tests prove it).
- The column fill never loads a session that is switched off; a logbook
  whose unloaded sessions are all off creates no load and no job when the
  column is enabled; the index records the attribute on the bulk edit's stub
  path and reports it without a load.
- The column: the default shows "On" without a write; an edit stores a
  token and refuses others; "Set Compute..." bulk-edits the selected
  sessions; a hand-edited token is shown as written and read as on. The
  Orientation column's tests are the model.
- The import preference: a recording imported while it is off carries the
  `off` line and is excluded at once, for a column that is enabled and a
  plot that is checked alike; one imported while it is on carries no line
  and reads the default; the preference's page round-trips the setting.
- A session whose attribute is `off` on disk but absent from the index (an
  index discarded or written by an earlier build) is loaded at most once by
  the fill and never fitted.
- A manual step: a logbook with a long-running recording, the recording
  switched off while it is being computed, the status bar's count, the
  cell, the row and the plot observed; switched on again; the whole-logbook
  column fill with half the recordings off.
- The cleanup audit's rule that no product code cancels a job is restated:
  the demand layer is the one product caller. The acceptance map opens a
  new hundred. `audit_cleanup` and the whole suite green.

### 7. Documentation

`docs/COMPUTED_PLOTS.md`: a section on switching a recording off (what it
does, what is kept, the cell, the context menu, the import preference), the status bar and
logbook sections where they say every recording is computed, and the known
limitation "there is no switch that pauses background work" restated: there
is one per recording and none for all. `docs/CALCULATIONS.md` section 16:
the condition, the reading of the attribute, the stop of the running job
(and 15.3 where it says no product code cancels a job), the fill, the
pending mark's third text. `docs/DATA_SCHEMA.md` section 9 (the attribute
line) and section 11 (the index entry). `docs/SENSOR_FUSION.md` section 2
(using it, beside the Orientation). `tests/README.md`: the test rows, the
manual step, the specification's appendix and matrix.

### Commit Policy

Michael has authorized commits for this plan on the existing working branch.
Nothing is ever pushed, and nothing is ever committed on `master` or
`fusion-improvements`. One commit, staged by explicit path, with the
session's attribution line; `PLANS/` is never staged.

## Phases

| Phase | Name | Purpose | Dependencies |
|-------|------|---------|--------------|
| 1 | Background computation | The whole specification: the `_COMPUTE` Choice attribute with its definition, constant default, column, header tooltip and "Set Compute..." (§3); the import preference and the importer's line (§3); the `Excluded` condition, the reading of the attribute for loaded and unloaded sessions, the withdrawal and the cancellation with its reason, the fill that never loads an excluded session, the "excluded" cell (§4, §5); the index entry and its learn points (§5); the tests of §6 in their executables, the manual step, the audit group and the restated gestures rule, the acceptance items in a new hundred with their appendix and matrix; the documentation of §7. | none |

```text
1 Background computation
```

One phase. The specification says it is small enough for one agent in one
phase, and its Commit Policy authorizes one commit; a plan of several phases
would ask the orchestrator for commits the policy does not allow. Nor does
the change split along a green boundary: the attribute without the condition
is a column that does nothing; the condition without the index entry loads
the sessions it forbids loading; the cell, the cancellation reason and the
preference are each visible only through a session that is excluded.

## Key patterns and references

Every path is relative to the repository root. The documenter and the
implementer read the whole file where the line says so; elsewhere the line
says what to look at.

### Project rules

- `CLAUDE.md`: build only in `build-agent/`, the suite sequentially, what
  must stay green, the conventions the audit enforces.
- `CLAUDE.local.md`: this machine's trees and git rules (never push; never
  commit on `master` or `fusion-improvements`).
- `.claude/docs/WORKFLOW-REFERENCE.md`: the stages and the conventions on
  altitude, phases and commits.
- `PLANS/done/staged-scale-plan/00-overview.md` and `01-staged-scale.md`:
  the previous one-phase plan, for the shape of a phase document, its
  acceptance criteria, its tests section and its decisions.
- `PLANS/done/fusion-plots.md` (its section on the Orientation attribute)
  and `PLANS/done/fusion-plots-plan/`: the specification and plan that
  introduced the Orientation, the Choice type, the import preference and
  the "Set ..." list: the pattern this specification names throughout.
- `PLANS/done/demand-driven-requested-calculations.md` and
  `PLANS/done/calculation-refinements.md`: the demand layer's own
  specifications (the track conditions, the one walk, the pair memory, the
  fill, the hold); the terms "track", "source" and "pair" are theirs.

### The attribute: key, definition, default, column (`src/`)

- `src/sessiondata.h`: the `SessionKeys` namespace, `Orientation` last with
  its comment (the pattern for `Compute`); `getAttribute()` (stored, else
  calculated: it reaches the engine), `hasStoredAttribute()` and
  `storedAttribute()` (the stored line alone, no engine).
- `src/attributeregistry.h` (read whole): `AttributeFormatType::Choice`,
  `AttributeChoice`, `AttributeDefinition` (an aggregate; a member left out
  takes its default; no tooltip member today), `findChoice()`,
  `AttributeRegistry`.
- `src/calculations/attributeregistration.cpp` (read whole):
  `registerBuiltInAttributes()`, the built-in definitions of category
  "Session": where the Compute definition goes.
- `src/calculations/attributecalculations.h`: `Calculations::addConstantDefault()`
  and its contract comment: the id `builtin.default.<key>`, no inputs, a
  stored attribute wins even when invalid, "every constant default in the
  application is registered here".
- `src/calculations/attributecalculations.cpp`: the wind constant defaults
  (`addConstantDefault(registry, QString::fromLatin1(SessionKeys::WindN), 0.0)`)
  in `registerAttributeCalculations()`: the core pattern for
  `builtin.default._COMPUTE`.
- `src/fusion/fusionregistration.cpp`, `registerOrientation()`: the
  Orientation's definition and constant default together, registered once
  per process; the pattern, but in the fusion library: the Compute
  attribute is the core's.
- `src/fusion/fusionregistration.h`: the contract comment that lists
  `builtin.default._ORIENTATION`.
- `src/mainwindow.cpp`: `registerBuiltInAttributes()`,
  `registerBuiltInCalculations()` and `Fusion::registerFusionCalculations()`
  in order; `prefs.registerPreference(PreferenceKeys::ImportOrientation, ...)`
  with its comment, beside which the new preference is registered.
- `src/sessionmodel.h` (read the class comment and the parts named):
  `SessionRow` (`cachedValues`, `session`, `visible`, `loadFailed`,
  `pendingColumns`, `isLoaded()`); CACHED COLUMN VALUES (a stub's cell is
  the index's cached value, computed by the column worker from a temporary
  copy through the engine, so a constant default shows without a write);
  "A Choice attribute's cell displays the label of the effective token" and
  the `Qt::EditRole` rule; `headerData()` (no `Qt::ToolTipRole` is served
  today); `startBulkEdit()` ("For a Choice attribute `value` is a token of
  its list"); the `dependencyChanged` signal's comment (a bulk edit
  publishes the names it changed on both of its paths).
- `src/sessionmodel.cpp`: `headerData()`; the Choice branches of `data()`
  and `setData()`; `processNextBulkEdit()`, the bulk edit's two paths (a
  loaded row edited and saved inline; a stub loaded through
  `logbook.loadSessionRaw(file.sessionId, &reason)` into a temporary copy,
  edited, saved, never installed as a row), then `dataChanged`, one
  `dependencyChanged` per changed name and `LogbookManager::instance().flushIndex()`;
  the import path around `DataImporter::applyCreationDefaults(file, created)`;
  the column worker's temporary copy ("A temporary copy via
  LogbookManager::loadSession()") and `restoreForColumnWorker()`.
- `src/preferences/addcolumndialog.cpp`: lists
  `AttributeRegistry::instance().allAttributes()` by `category`: "Compute"
  appears under "Session" with no change here.
- `src/ui/docks/logbook/LogbookView.h/.cpp`: the context menu's
  `tr("Set %1...")` action per attribute column and `askAndSetAttribute()`,
  whose Choice branch offers `def->choices`: "Set Compute..." needs no
  code of its own.

### The import preference

- `src/preferences/preferencekeys.h`: `ImportOrientation` with its comment.
- `src/preferences/importsettingspage.h/.cpp` (read whole): the page, its
  groups (`createOrientationGroup()`, the check box of
  `createTrackVisibilityGroup()`), `saveSettings()` connected to every
  control.
- `docs/PREFERENCE_PAGES.md` (read whole): how a Preferences page and a
  group are laid out; a check box with a sentence label.
- `src/dataimporter.cpp`, `applyCreationDefaults()`, step 7 (the
  orientation: `if (!session.hasStoredAttribute(SessionKeys::Orientation))`,
  nothing written without a preference): the Compute line is a step 8 in
  the same shape, written only when the preference reads exactly `off`;
  any other text, empty and invalid included, writes nothing (decision
  17).
- `src/dataimporter.h`: `applyCreationDefaults()` ("the one writer of
  import-time defaults"), `peekHeaderAttribute()`.
- `tests/support/testenvironment.cpp`: the literal mirror of the defaults
  MainWindow registers, `ImportOrientation` empty with the reason in its
  comment: the Compute preference is added here with the application's
  default (on), which writes nothing, so every fixture file and attribute
  list stays as it is.
- `tests/tst_importer.cpp`, `orientationFromThePreference()`: the model for
  the importer's test (a file's own line kept; nothing written while on).

### The demand layer (`src/`)

- `src/calculationdemand.h` (read whole): the component's contract. WHAT IS
  WANTED, WHERE A RESULT IS LOOKED UP, the conditions, PRESENTATION, WHAT A
  PASS COSTS, the API (`isCellPending()` in both forms, `progress()`,
  `failures()`, `sessionFailures()`, `isMerelyUncomputed()`,
  `isNotYetComputed()`), the signals (`progressChanged`, `failuresChanged`,
  `pendingCellsChanged(columnId)`), the test seams (`flush()`,
  `hasPendingUpdate()`, `passCount()`, `setInputSettleDelay()`,
  `endInputSettleWaits()`, `isSettling()`, `heldSessionIds()`,
  `hasFillWork()`, `canLoad()`, `runLoadStep()`, `recordSetLookups()`), the
  private `TrackCondition` (five values today), `Source`, `PairMemory`,
  `LearnedFact`, `Candidate`, `Walk` (`tiers`, `pendingCells`,
  `pendingSessions`, `loadCandidates`, `learned`, `progress`, `failures`).
- `src/calculationdemand.cpp` (read whole; 1300 lines, and the phase lives
  here): the constructor's connections (`dependencyChanged`,
  `visibilityChanged`, `modelChanged`, `sessionLoaded`,
  `focusedSessionChanged`, the model's reset pair,
  `calculationRecordWriteFailed`, the logbook's
  `calculationRecordsChanged`, the executor's `jobStarted`,
  `jobCancelRequested` (to `scheduleUpdate`), `jobFinished`, `jobProgress`);
  `isRelevantName()` ("an edit that affects no requested calculation starts
  no settle wait"); `classifyLoaded()` and `classifyUnloaded()` (the six
  numbered steps); `recordSet()` (the memo of the index's record names,
  `++m_recordSetLookups`); `walkRows()` (the one place every track is
  classified, under `RowStabilityGuard`; `runningIsLive`; a plot's tracks
  `loaded && sr.visible`; `isPending()` fills `pendingCells` and
  `pendingSessions`; `loadCandidates` from waiting, hidden, unsettled,
  unheld rows); `offerChoice()` and `withdrawChoice()` ("the only place
  that offers"); `recompute()` (the pass: the walk under the guard, then
  the facts, the fill's `update()`, the offers, the values);
  `onDependencyChanged()` (drops the reports, then returns before
  scheduling when `isRelevantName()` is false: the `_COMPUTE` change takes
  this path today and schedules nothing; the input-change branch forgets the
  session and starts the settle wait); `onVisibilityChanged()` (a hidden
  session with a chosen next job runs the pass synchronously: the shape
  for switching off); `onJobFinished()` (the synchronous pass that starts
  the next candidate); `onCalculationRecordsChanged()`;
  `onSessionModelAboutToBeReset()` / `onSessionModelReset()` (what is
  forgotten on a reset).
- `src/demandfill.h` (read whole): `update(pendingSessions, loadCandidates)`
  releases every hold whose session is not pending, so an excluded
  session's hold is released by construction; `Hooks`; `kMaxHeldSessions`.
- `src/demandfill.cpp`: `step()` (`runPendingPass` before a load).
- `src/demandstate.h` (read whole): the presentation values and
  `pendingMark()`, the home of a view's text that the demand layer owns.
- `src/demandsettleclock.h`: `start()`, `isSettling()`, `keepOnly()`,
  `endAll()`.
- `src/jobqueue.h` (read whole): LIFECYCLE (Cancelled: "cancel() /
  shutdown(); nothing published"), STALE WHILE RUNNING, A PENDING END ("the
  first writer wins"), ORDER OF A JOB'S END, `withdrawChosenNext()`
  ("No longer needed"), `activeJob()` (a running job asked to stop is not
  active), `cancel(JobId)` and its doc comment ("No product code calls it
  today: it is kept for the jobs dock"), `jobCancelRequested`.
- `src/jobqueue.cpp`: `cancel()` (the reason is the fixed `tr("Cancelled")`
  for both the chosen next and the running job;
  `requestStop({JobState::Cancelled, tr("Cancelled")})`),
  `withdrawChosenNext()`, `offer()`'s replacement (`tr("No longer needed")`),
  `shutdown()` (`tr("Application closing")`), `endJob()`, `requestStop()`.
- `src/jobmodel.h`: `JobRecord` (`sessionId`, `instanceId`, `state`,
  `reason`, `cancelRequested`, `progressText`), `JobState`.

### The logbook index (`src/`)

- `src/logbookmanager.h` (read the class comment and the record API): the
  `index.json` root and per-session entries ("records" always written,
  "recordReasons" optional and additive: the pattern for the new entry);
  `knownCalculationRecords()`, `calculationRecordReason()`,
  `setCalculationRecordReason()` (a fact learned from a restore, announced
  through `calculationRecordsChanged` only when it differs); `loadSessionRaw()`
  and `loadSession()` (every load and the column worker's temporary copy
  pass through the first); `saveSession()` (the one writer of a session
  file: the import, a loaded row's save and the bulk edit's stub path all
  end here); `flushIndex()`; `reset()`; the remap of a temporary id.
- `src/logbookmanager.cpp`: `initialize()`'s index read loop
  (`entry[QStringLiteral("recordReasons")].toObject()`), `saveSession()`,
  `loadSessionRaw()`, `flushIndex()`'s entry writer
  (`entry[QStringLiteral("records")] = recordsObj` and the optional
  `recordReasons` after it), `remapSessionId()` (`m_recordReasons` carried
  to the new id), the removal paths that erase `m_recordReasons`, `reset()`.
- `docs/DATA_SCHEMA.md` section 11: how `records` and `recordReasons` are
  described (derived, additive, optional; what absence means).

### The views (`src/ui/`)

- `src/ui/docks/logbook/LogbookCellDelegate.h/.cpp` (read whole): PENDING
  CELL (three looks; "a value always wins"; sorting treats a pending cell
  as unavailable), HOVER (`helpEvent`), REPAINT (`pendingCellsChanged(id)`
  repaints one column), CHOICE EDITOR; `showsPending()`, `pendingText()`,
  `pendingToolTip()`, `cellOption()`; the muted placeholder colour.
- `src/ui/statusbar/StatusBarFeature.cpp`: reads `progress()` and
  `failures()` only; unchanged by this specification.
- `src/ui/docks/plot/PlotWidget.cpp` and
  `src/ui/docks/legend/LegendPresenter.cpp`: `isMerelyUncomputed()` /
  `isNotYetComputed()` keep the "no data" warning silent for a `Blocked`
  name; unchanged.

### Tests

- `tests/tst_calculation_demand.cpp` (read the head, the fixtures and the
  functions named): the harness (a real `PlotModel`, executor,
  `SessionModel`, logbook, synthetic requested plots of `plotfixture.h`,
  column support of `logbookprobe.h`); `rowScript`,
  `hidingASessionDropsItsWaitingPair`, `uncheckingDropsWaitingPairsKeepsRunning`
  (what "the running job finishes and is kept" looks like, the contrast to
  switching off), `changingDemandReplacesChosenNext`,
  `inputBurstRunsOneJob` and `supersededJobIsRunAgainAfterInputsSettle`
  (the settle wait), `staleRunningJobIsWaitingAtOnce`,
  `enablingColumnFillsEveryUnloadedSession`, `storedResultsCreateNoJob`,
  `storedRejectionIsBadgedAfterRestartWithoutLoad`,
  `storedRejectionIsAFailureWithoutLoad`, `disablingColumnReleasesHeldSessions`,
  `chainedColumnKeepsItsHold`, `bulkEditMakesSettledSessionApplicable` (a
  bulk edit announced for a stub reaching the demand layer),
  `settledPairsSurviveEvictionSortAndColumnWorker`,
  `passOverManyStubsReadsEachRecordSetOnce`, `progressCountsEachSessionOnce`,
  `failuresFollowWhatIsSwitchedOn`, `pendingCellsChangedPerColumn`,
  `changeSignalsAreMinimal`, `columnProgressAndPendingCells`.
- `tests/support/plotfixture.h/.cpp`, `jobfixture.h/.cpp`,
  `logbookprobe.h/.cpp`, `fixturebuilder.h/.cpp`, `storedresults.h/.cpp`,
  `testenvironment.h/.cpp`: the fixtures the demand tests build on (a
  cancellable synthetic requested calculation, the logbook probe's columns,
  a stored record planted on disk).
- `tests/support/choicefixture.h/.cpp` (read whole): a `SessionModel` over
  the test logbook with one Choice attribute column, parametrized by key:
  the seam for the Compute column's tests.
- `tests/tst_fusion_derived.cpp`, `orientationColumnShowsTheDefaultWithoutAWrite`
  and `orientationEditStoresATokenAndRefusesOthers`: the Orientation
  column's tests through `ChoiceFixture`, widget-free: the model the
  specification names for the Compute column.
- `tests/tst_choice_attribute.cpp` (read the head): the generic Choice
  tests (display, sort, edit, bulk edit, the list editor, the "Set ..."
  dialog driven through its modal `QInputDialog` by a timer); its head says
  it is specific to no product attribute.
- `tests/tst_logbook_indicators.cpp`: `pendingCellsAreDistinctFromUnavailable`,
  `pendingCellBecomesValueWhenRecordIsWritten`,
  `sortingTreatsPendingAsUnavailable`, `unreadableRecordPendingIsNotDemandPending`,
  `pendingCellsChangeRepaintsOnlyThatColumn`, `rowWarningFollowsFailures`:
  the model for the "excluded" cell's tests (text, tooltip, muted colour,
  value wins, sorting, repaint, no row warning).
- `tests/tst_logbook_index.cpp`: `recordReasonsRoundTrip`,
  `recordReasonChangeIsAnnounced`, `legacyFlatIndexStartsAsStubs`,
  `prepareCachedSession()`: the model for the new entry's round trip, its
  learn points and an index without it.
- `tests/tst_jobqueue.cpp`: `cancelRunningThenNextStarts`, `cancelQueued`,
  `cancelIgnoredForOneStepStillCancelled`, `userCancelThenStaleEndsCancelled`,
  `staleThenUserCancelEndsSuperseded`: the executor's cancel tests, touched
  if `cancel()`'s signature or reason changes.
- `tests/tst_fusion_jobs.cpp`: `cancelDuringFitThenNextJobStarts` (a real
  fit cancelled through the executor; "mid-run actions are taken in a slot
  on the job's first progress text"), `columnOnFusionOutputIsCachedFromRecord`,
  `workerRefillsColumnFromStoredFit`: the real-fit shape, should the
  documenter want one end-to-end test of switching a fit off.
- `tests/tst_status_bar.cpp`: `computationsAreOneItem`,
  `warningCountsRecordingsAndListsThem`: the counts the status bar shows,
  which simply omit excluded sessions.
- `tests/CMakeLists.txt`: `flysight_add_test()`; the three widget tests and
  the application sources each compiles (no preference page is compiled
  into any test).

### Audit and traceability

- `tests/audit/cleanup_audit.cmake`: the header (rule groups,
  `audit_group(<slug>)`, the acceptance-map check); group `gestures` (the
  rule `expect_none("no product code cancels a job" ...)` and its comment,
  and `expect_count("cancel is kept for the jobs dock" "bool cancel\\(JobId" 1 src/jobqueue.h)`),
  both restated by this specification; group `demand`
  (`DEMAND_FILES`; "pending is the view's presentation of demand", the
  `expect_only` of `isCellPending|showsPending|pendingText\(|pendingToolTip\(`,
  which the excluded cell's names join; "the model, the logbook and the
  scheduler know nothing of the executor"; "the demand layer never loads a
  session or reads a record itself"); group `constant-defaults` (the helper
  is the one way); group `orientation` (a one-authority rule for an
  attribute's vocabulary); group `staged-scale` (the newest group: the form
  of a group that pins a specification's words in the documents and names
  its hundred).
- `tests/acceptance_map.txt`: the header comment ("Fifteen specifications,
  fifteen item ranges" and one paragraph per range, amendments included);
  the 1401-1415 block for the current form (`# <item> - (<sections>)
  <statement>`, then one line per piece of evidence: `<item> <executable>
  <function>`, `<item> audit <group>`, `<item> manual M<k>`).
- `tests/README.md`: section 1's table (rows `tst_calculation_demand`,
  `tst_logbook_indicators`, `tst_choice_attribute`, `tst_importer`,
  `tst_logbook_index`, `tst_jobqueue`, `tst_fusion_jobs`); section 9.15 (the
  matrix form) and 9.6 / 9.9 (the demand and fusion-plots matrices whose
  items this specification amends, if any are restated); section 10 (the
  audit groups); section 12's preamble ("Thirteen scripts") and 12.13 (the
  form of a manual step, M56 the last id); appendix O (the form of an
  appendix).

### Documents

- `docs/COMPUTED_PLOTS.md` (read whole): sections 2 (the status bar's
  counts), 3 ("Anything that switches such a value on"), 4 ("filled for
  every recording in the logbook"), 5 (when you stop needing a result; "no
  refresh and no cancel"), 6 ("Changing a recording's Orientation" among
  the edits that drop results), 9 (the limitation "There is no switch that
  pauses background work").
- `docs/CALCULATIONS.md`: section 5 (the constant defaults, the orientation
  among them); 15.2 (the lifecycle); 15.3 ("`cancel(JobId)` is kept for the
  jobs dock ... no product code calls it today"); 16.1 (the conditions
  table and the unloaded order); 16.2 (progress, failures, pending cells);
  16.3 (the bulk edit announced under the attribute's name); 16.4; 16.7
  (the pair memory); 16.8 (the fill); 16.10 (the views); 16.11 (the
  executor "keeps `cancel()` for the jobs dock"); 16.12; the attribute
  registration passage ("The entry point also registers the definition of
  the orientation attribute").
- `docs/DATA_SCHEMA.md` sections 9 (the choice attribute's `$VAR` line,
  with the Orientation as the example) and 11 (the index entries).
- `docs/SENSOR_FUSION.md` section 2, the Orientation bullet ("to change one
  recording, add the Orientation column ... or use 'Set Orientation...'").
- `docs/PREFERENCE_PAGES.md`: the layout the Import page follows.

## Decisions and constraints

1. **One phase, one commit.** As the specification says and its Commit
   Policy allows; see "Phases".

2. **Baseline.** `d0c054b` on `store-requested-calculations`; the code is
   `3260e24`'s. `PLANS/` is never staged.

3. **Where the attribute lives.** The key `SessionKeys::Compute` goes in
   `src/sessiondata.h` beside `Orientation`. The definition goes in
   `registerBuiltInAttributes()` (`attributeregistration.cpp`), the constant
   default `builtin.default._COMPUTE` through `Calculations::addConstantDefault()`
   in `registerAttributeCalculations()` (`attributecalculations.cpp`) beside
   the wind: the core's two registration points, which the specification's
   "registered beside the other built-in definitions, not by the fusion
   library" names, and which the audit's `constant-defaults` group requires
   (the helper is the one way). The two tokens are spelled once in `src`,
   as constants beside the key (`SessionKeys::ComputeOn`, `ComputeOff`),
   which the definition, the default, the importer, the walk and the index
   read; the labels are spelled in the definition alone. A one-authority
   audit rule in the new group pins both, as `orientation` does for its
   tokens. (Amended at the integration check: the walk and the manager must
   compare a stored token with `off`, so the definition cannot be the only
   speller.)

4. **The executor's cancel carries no reason today.** `JobQueue::cancel(JobId)`
   ends the job with the fixed text "Cancelled"; the specification requires
   the job to end Cancelled with the reason "Switched off for this
   recording". The executor's API must therefore take the caller's reason
   (a reason parameter is the plain reading; the documenter settles the
   signature and what the executor does with a reason for a chosen next
   job, which ends at once). The audit's count of `bool cancel\(JobId` in
   `jobqueue.h` stays satisfied by a signature that begins the same way.
   The gestures rule "no product code cancels a job" becomes "the demand
   layer is the one product caller" (an `expect_only` on
   `calculationdemand.cpp`, with a count of one), and `cancel()`'s doc
   comment, `CALCULATIONS.md` 15.3 and 16.11 are restated with it. The
   first-writer rule, the lifecycle and `shutdown()` are untouched.

5. **How the change of the attribute reaches the demand layer.** Setting
   `_COMPUTE` through `setData()` or a bulk edit emits
   `SessionModel::dependencyChanged(sessionId, attribute(_COMPUTE))` on
   both paths, as the model's contract says. In `onDependencyChanged()` the
   key is not relevant (no requested source depends on it, by construction
   and for ever: nothing declares it), so today the slot drops the
   session's reports and returns without scheduling. The slot must
   recognise the Compute key and run a pass for it, without
   `forgetSession()` and without `m_settle->start()`: the specification's
   "a pass runs", "never starts a settle wait" and "the pair memory is not
   touched". Switching off while a chosen next job or the running job is
   the session's wants the pass at once, as `onVisibilityChanged()` runs it
   synchronously when a hidden session has the chosen next job. Whether
   the check is on the key before or after `isRelevantName()` is the
   implementer's.

6. **Where the attribute is read.** For a loaded row, from the row's
   session: `storedAttribute(SessionKeys::Compute)` suffices, since "any
   token but `off` is on" and the stored line is the fact the specification
   describes; the engine need not be asked (and asking it inside the walk
   would run the constant default under the row stability guard, which is
   legal but needless). For a row that is not loaded, from a new query of
   `LogbookManager`, per session, answering whether the index recorded the
   attribute as `off`. The exclusion is read at every pass, memoized
   nowhere in the demand layer and never a `PairMemory`; the manager's
   lookup is a map lookup, so no memo like `recordSet()`'s is needed.

7. **The index entry.** One optional, additive boolean per session entry
   beside `records` and `recordReasons` (the documenter names it; absent
   means on). It is learned in exactly two places, which together cover
   the four cases the specification lists: `saveSession()` (the import, a
   loaded row's save and the bulk edit's stub path all write the file
   there) and `loadSessionRaw()` (a load and the column worker's temporary
   copy both read the file there). Learning a value that differs from what
   the index holds marks the index for the next flush, through the flush
   points that exist (the bulk edit's stub path already flushes); no new
   flush is added. The entry follows a remap, a removal and a reset as
   `recordReasons` does. Nothing announces the learned entry: the
   specification's recovery argument (at most one hidden load, never a fit)
   does not need an announcement, and the loaded session then answers for
   itself. The documenter may add an announcement if a test needs it, and
   says so.

8. **The excluded cell.** The delegate's PENDING CELL gains a fourth look:
   "excluded" (a translated word), muted like the pending mark, tooltip
   "Not computed: background computation is switched off for this
   recording", painted when the demand layer says the cell's track is
   excluded and the model has no value; a value always wins, exactly as for
   pending. The demand layer exposes the excluded cells as it exposes the
   pending ones (the walk computes both; `pendingCellsChanged(columnId)`
   may announce both, since both are "what a requested column's cell shows
   of demand"). Sorting needs no change: the model has no value underneath.
   The audit rule "pending is the view's presentation of demand" lists the
   new names beside the pending ones. Where the texts live (`demandstate.h`
   beside `pendingMark()`, or the delegate beside `pendingToolTip()`) is
   the documenter's, following the existing split: the mark that several
   views share is the demand layer's, a delegate-only text the delegate's.

9. **A header tooltip does not exist today.** `SessionModel::headerData()`
   serves no `Qt::ToolTipRole`, and `AttributeDefinition` has no tooltip
   member; the specification requires the Compute column's header to carry
   "compute results for this recording in the background". Decision:
   `AttributeDefinition` gains an optional tooltip member (an aggregate
   member with an empty default, so no other registration changes), and
   `headerData()` serves `Qt::ToolTipRole` for an attribute column from it;
   the logbook's header is the tree's own `QHeaderView`, which shows the
   model's tooltip without code of its own. Other definitions leave it
   empty and show no tooltip, as today.

10. **Which executable tests the Compute column.** The Orientation column's
    tests live in `tst_fusion_derived` through `ChoiceFixture`, widget-free;
    the Compute attribute is the core's, so its column tests go in a core
    executable. The documenter chooses between `tst_calculation_demand`
    (the attribute's reader; one harness for the column and the demand)
    and `tst_result_columns` (the logbook's columns). "Set Compute..." is
    the generic Choice path proven by
    `tst_choice_attribute::setDialogOffersTheList`; a product-specific
    dialog test is optional and would relax that file's "specific to no
    product attribute" head.

11. **The preference page's round trip is manual.** No preference page is
    compiled into any test today, and `importsettingspage.cpp` includes the
    fusion library's orientation type, so a widgets test of the page would
    link `flysight_fusion` and GTSAM into a core test. The specification's
    "the preference's page round-trips the setting" is therefore the manual
    step's (M57), while the preference's effect is tested through the
    importer (`tst_importer`) and the key's default mirrored in
    `testenvironment.cpp`. Michael may overrule by asking for a widgets
    test that compiles the page.

17. **The preference holds the token, not a boolean** (Michael, at the
    plan review). `import/compute` stores `on` or `off` as text, the
    orientation preference's mechanism, registered with `on` as its
    default and mirrored `on` in `testenvironment.cpp`; the page's check
    box reads and writes the token. The importer writes `off` only when
    the text is exactly `off`; an absent, empty or invalid value writes
    nothing, as the orientation's empty does. A boolean read of an
    unregistered key would have converted to false and excluded every
    import in a context that lacked the registration.

12. **What does not change.** The status bar, the plot widget and the
    legend (their inputs simply omit excluded sessions, and the engine still
    reports `Blocked`); the executor's lifecycle, first-writer rule and
    shutdown; the engine, the stores, the column worker and the fusion
    library (none reads the attribute; the new audit group confines its
    readers to the demand layer, the registrations, the importer, the
    preference's registration and page, the model's generic Choice paths
    and the manager's two learn points). No golden
    moves: the algorithm string and the compatibility marker are untouched,
    so the exact tests are unaffected.

13. **Staleness by construction.** No calculation declares `_COMPUTE`, so
    the engine's ticket never marks a running job stale for it and no
    stored or cached value depends on it. The specification still asks for
    the proof; the test is a direct one (set the attribute while a job
    runs: the job is not `cancelRequested` through staleness, the stored
    record and the cached column value survive, no settle wait starts).

14. **The settle case.** A session that settles after an edit is passed
    over by the walk's offers already; excluding it makes its track no
    candidate at all, and the clock's wait may expire harmlessly. Ending
    the wait is not required; the documenter says whether the
    implementation does.

15. **Where the cancel is made.** The walk classifies under the row
    stability guard and may call nothing on the executor; the pass asks the
    executor to cancel the running job after the walk, beside the offers,
    when the walk found the running job's session excluded. The executor
    then emits `jobCancelRequested` (already connected to `scheduleUpdate`)
    and, at the job's end, `jobFinished`, whose synchronous pass starts the
    next candidate. The running job asked to stop is "described by nothing"
    in the next walk, as today.

16. **Traceability.** The sixteenth specification: items 1501 onward in
    `tests/acceptance_map.txt` (the header says "Sixteen specifications"
    and gains a paragraph), `tests/README.md` section 9.16 with its matrix,
    appendix P, section 12.14 with M57 ("Fourteen scripts"), and a table
    row amendment for every executable that gains tests. Items of earlier
    specifications that this one amends (the demand specification's
    "hiding a track ... the running job finishes", 16.1's condition list,
    the status bar's counts) are restated "(as amended)" where the
    documenter finds them.

## Interfaces between phases

One phase: there are no interfaces between phases. The names the
specification fixes as observable contract, which the phase document states
and the implementer does not vary:

- `SessionKeys::Compute`, the key `_COMPUTE`; the tokens `on` and `off`,
  the labels "On" and "Off", in that order; category "Session", display
  name "Compute", editable; the constant default `builtin.default._COMPUTE`
  with the value `on`.
- The `$VAR,_COMPUTE,<token>` line, written when the user sets it or when a
  recording is imported while the preference is off, never removed.
- The header tooltip "compute results for this recording in the
  background"; the context menu's "Set Compute..." (the generic "Set %1..."
  of the column's label).
- The preference "Compute newly imported recordings in the background" on
  Preferences > Import, on by default.
- The cell text "excluded" with the tooltip "Not computed: background
  computation is switched off for this recording".
- The cancelled job's reason "Switched off for this recording"; the
  withdrawn chosen next job's "No longer needed", as today.
- The condition's name `Excluded` beside `Done`, `Failed`, `NotApplicable`,
  `Waiting` and `Running` (private to the reconciler; the documents name
  it).

## Commit Policy


Michael has authorized commits for this plan on the existing working branch.
Nothing is ever pushed, and nothing is ever committed on `master` or
`fusion-improvements`. One commit, staged by explicit path, with the
session's attribution line; `PLANS/` is never staged.
