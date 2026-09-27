# Phase 3: One walk and one pair memory

## Overview

This phase rewrites the reconciler inside `CalculationDemand` to spec §10. The
two paths are `inspect()`/`plotCandidates()` for plots and `walkColumns()` for
columns. They become one walk over the session rows, for every source at once,
under one `RowStabilityGuard`. Its output is plain values. Plots and columns
become data: one `Source` type with a kind. The run's two memories are `Memory`
per pair and `Settlement` per cell. They become one pair memory, keyed by
(session id, requested calculation instance id), with the kinds NotApplicable
and Failed(reason). The failed record write that Phase 1 announces becomes a
shown failure (spec §7, the demand side). The fill also releases every hold
once the executor is shut down (spec §11, literal reading).

Nothing the user sees changes except two things. A result that could not be
stored is now badged. A pair is no longer loaded again, or offered again, in
the cases listed in Decisions Made. Phase 4 owns every pixel. Phase 5 owns
`docs/`, the new acceptance items and the new audit rules.

## Dependencies

- **Depends on:** Phase 1 (Contracts below the demand layer) and Phase 2
  (Demand layer in parts), both committed and tagged
  `plan/calculation-refinements/phase-1-done` and `.../phase-2-done`.
- **Blocks:** Phase 4 (its failed-write hover test needs the failure this
  phase shows) and Phase 5 (documentation, acceptance map and audit).
- **Assumptions.** These facts about the code as Phases 1 and 2 leave it are
  taken from their documents. Verify each before starting.
  - `SessionModel` has `columnRequestedCalculations(int)`,
    `columnDependencyClosure(int)` and `sessionDisplayName(int row)`. It also
    has the relay signal
    `calculationRecordWriteFailed(sessionId, calculationId, reason)`.
    - On a failing write that is not for an unknown session, the manager's
      `calculationRecordsChanged(s, c)` is emitted **before**
      `calculationRecordWriteFailed(s, c, reason)`.
    - A later successful write of the pair emits `calculationRecordsChanged`
      again.
    - `reason` is never empty.
    - Both are emitted from inside the engine's explicit-result listener, so
      a directly connected slot may only record state and schedule.
  - `LogbookManager::setCalculationRecordReason()` emits
    `calculationRecordsChanged` when the reason changes. The demand layer
    has no `onSessionDataChanged` and observes no `dataChanged` of the
    model: the bulk edit publishes its edit as a `dependencyChanged` on both
    of its paths (`publishInvalidation`), and `onDependencyChanged` is the
    one slot that learns of an input change.
  - The demand layer is split into parts:
    - `src/demandstate.{h,cpp}`: `DemandCondition` and `DemandTrack` (no
      `settling` and no `job`), and `DemandState` (no `waiting`) with
      `addTrack()`, `finish()`, `static buildToolTip()` and
      `kToolTipListLimit`.
    - `src/demandsettleclock.{h,cpp}`: `DemandSettleClock`.
    - `src/demandfill.{h,cpp}`: `DemandFill` with `Hooks{enabled,
      runPendingPass, runPass, loadFailed(id), loaded(id, heldId)}`,
      `update(pendingSessions, loadCandidates)`, `kMaxHeldSessions`,
      `heldSessionIds()`, `hasWork()`, `canLoad()`, `step()` and
      `detach()`.
  - The reconciler is still `CalculationDemand` in `src/calculationdemand.{h,cpp}`. Phase 2 left these seams:
    - `walkColumns(const JobRecord &running, const QSet<QString> &held)`
    - `classify(const Track &, const BlockerReport &, const JobRecord &running)`
    - `classifyUnloaded(int row, const SessionRow &, const ColumnInfo &)`
    - `plotCandidates(inspections, focusedId, running)`
    - `offerChoice()` acting on the executor's answer, and `withdrawChoice()`
    - the fill hooks `onFillLoadFailed(id)` and `onFillLoaded(id, held)`
    - `isRelevantName(key)`
    - `syncColumns()`, which runs every pass and reads the model
    - `Memory`, `Settlement`, `m_settled`, `eraseSettlements()`,
      `loadFailedSettlement()`, `m_columnReports`, `m_recordSets` and
      `m_recordReasons`, all unchanged in logic.
  - The audit has the variables `DEMAND_LAYER` / `DEMAND_FILES` and Phase 2's
    three part-boundary rules. One of them is "the fill and the settle clock
    know nothing of the walk", whose regex contains `Settlement`.
  - Line numbers below are those of `ec6bd43`. Phases 1 and 2 have moved
    them. Locate every passage by its name or its quoted text. For code that
    Phase 2 created, use the names from Phase 2's document.

## Tasks

### Task 3.1: Sources as data, and one report memo for every source

**Purpose:** A plot and a column become one data type (spec §4, §10.1: "one
rule, one path; special cases become data"). The engine's combined report of a
loaded session is memoized for every source.

**Files to modify:**
- `src/calculationdemand.h`: replace `ColumnInfo`, `Track`, `Inspection` and
  `Inspections` with `Source`. Replace `m_columns` with `m_sources` and
  `m_columnReports` with `m_reports`. Declare `syncSources()` and
  `combinedReport()`. Remove `inspectedPlots()`, `syncColumns()`,
  `inspectUnderGuard()` and `columnReports()`.
- `src/calculationdemand.cpp`: the bodies.

**Technical Approach:**

1. **`Source`** (private nested struct, plain data):
   ```cpp
   /// One source of demand (spec §4): a checked requested plot or an enabled
   /// requested logbook column. Rebuilt at the start of every pass.
   struct Source {
       enum class Kind {
           Plot,       ///< tracks: visible, loaded rows that are not failed-load placeholders; pairs in tiers (a)/(b)
           Column      ///< tracks: every row, loaded or not; pairs of loaded rows in tier (c)
       };
       Kind kind = Kind::Column;
       QString id;                     ///< plotId() or columnId()
       QList<DependencyKey> names;     ///< what inspection reads: {y name} or logbookColumnNames()
       QStringList storable;           ///< requested calculations without explicit family instances ('#'), in the authority's order
       QStringList storableTitles;     ///< parallel to storable: the registry's titles (the id when a title is empty)
       QString sensorId;               ///< Plot only: the sensor whose time axes the debug check reads
   };
   QVector<Source> m_sources;          ///< plots in plot-model order, then columns in SessionModel column order
   ```
   The kind is the only special case. It decides which rows are tracks, which
   candidate tier a pair is filed in, and whether the walk records a column
   verdict (Task 3.2).

2. **`syncSources()`** replaces `syncColumns()` and `inspectedPlots()`. Today
   they are two functions (calculationdemand.cpp 273-282 and 287-338). It runs
   in the non-inert branch of every pass, right after `syncCheckedSet()`.
   - **Plots.** For each id in `m_checkedOrder` where `isRequested(plot)`
     holds (the memo stays as it is: `m_requested`, `m_staticNames`):
     - `names = {yName(plot)}` and `sensorId = plot.sensorID`.
     - `storable` is `CalculationRegistry::instance().explicitDependencies(yName(plot))`
       without the ids that contain `'#'`. The registry memoizes it
       (calculationregistry.h 162-167: "Sorted, unique. A function of the
       registrations alone. Memoized").
     - The demand layer still computes a plot's closure from the registry
       (spec §8). This is the plot counterpart of a column's
       `columnRequestedCalculations()`.
   - **Columns.** For each model column `i` where
     `model.columnRequestedCalculations(i)` is not empty:
     - `id = columnId(column)`.
     - `names = logbookColumnNames(column)`.
     - `storable` is the list without `'#'` ids, as Phase 1's `syncColumns()`
       builds it.
   - A shared static helper `setStorable(Source &, const QStringList &calculations)`
     fills `storable` and `storableTitles`. The body is Phase 1's loop
     (`// An explicit family instance is never stored`,
     `registry.title(id)`, the id when the title is empty).
   - The source list needs no before/after comparison. Today's code clears
     the report memo and erases settlements when the column ids change
     (287-338 tail, "The report memo is parallel to the columns"). That goes,
     because the memo is keyed by source id (item 3) and settlements no longer
     exist.

3. **One report memo for every source.**
   `QHash<QString, QHash<QString, BlockerReport>> m_reports; // memo: a loaded session's combined report per source id`.
   It replaces `m_columnReports` (header 554). Spec §10.1 says to memoize it
   "per session as the column reports are today, for every source, and dropped
   by the same events". The drops are those of `m_columnReports` today:
   - `sessionLoaded` (the lambda, cpp 92-95);
   - `onDependencyChanged` (first line, before the relevance test; a bulk
     edit of a loaded row arrives here too);
   - `onSessionModelAboutToBeReset` / `onSessionModelReset` (clear);
   - `onCalculationRecordsChanged`;
   - `onJobFinished` (the record's session);
   - `onRegistryChanged` (clear);
   - `onFillLoaded` with a corrected id (the old id);
   - the walk, for a row that is not loaded (today
     `m_columnReports.remove(sessionId)` at cpp 640).

   Keying the inner hash by source id makes the memo independent of the
   source list. A report is a function of the session's engine state and the
   source's names. The id fixes the names: the plot id fixes the y name, and
   a column id is its definition key. So an entry of an unchecked plot or a
   disabled column is never wrong. It is only unused until the session's next
   drop.

   Entries are computed lazily. A hidden loaded row never computes a plot's
   entry, because it is no plot track. This keeps today's cost rule that
   "blockers() is called only for (checked AND requested plots) x (tracks)"
   (header 189-191).

4. **`static BlockerReport combinedReport(const SessionData &session, const Source &source)`**
   is today's per-column body of `columnReports()` (cpp 505-568: any
   NotApplicable, else any NotProduced, else any Blocked, else Available;
   blockers and notes unique by instance id), applied to `source.names`. For
   a plot's single name it yields exactly what `inspectUnderGuard()` gave:
   - Available and NotApplicable reset to empty reports.
   - NotProduced keeps its notes and drops the blockers.
   - Blocked keeps its blockers and drops the notes, which `classify()`
     ignored anyway ("notes on a Blocked report are ignored: Blocked wins").

   Move the `#ifndef QT_NO_DEBUG` time-axis check of `inspectUnderGuard()`
   (cpp 356-374) into `combinedReport()`. It runs when
   `source.kind == Plot && combined.state == Available` and reads
   `DependencyKey::measurement(source.sensorId, axis)`. Keep its warning text,
   with `source.id` in place of `plotId(plot)`. Call it under the walk's
   guard only.

5. **Remove the old recompute comment** "Plot classifications are not cached
   across passes: … a cache keyed on dependencyChanged would be wrong (after A
   publishes, the blocker of B's output changes from A to B although B's
   output may not be re-announced)" (cpp 1206-1211). Explain in its place why
   the memo is right. It is keyed per **session**, not per name. Any
   `dependencyChanged` of the session drops the whole session entry, and so
   do a job's end and a load. So A's publication drops B's report too.

**Acceptance Criteria:**
- [ ] `ColumnInfo`, `Track`, `Inspection`, `Inspections`, `inspect(`,
  `inspectUnderGuard`, `inspectedPlots`, `syncColumns`, `columnReports(` and
  `m_columnReports` do not exist in `src/`.
- [ ] `m_sources` lists the checked requested plots in plot-model order, then
  the requested enabled columns in model column order. A plot's `storable` is
  `explicitDependencies(yName)` without `'#'` ids. A column's `storable` is
  `columnRequestedCalculations(i)` without `'#'` ids.
- [ ] `CalculationEngine::blockers()` is called for a plot only for its tracks
  (visible, loaded, not a placeholder), and for a (session, source) only when
  its memo entry is missing. `ordinaryPlotsAreNeverInspected`,
  `uncheckedPlotsAreNeverInspected`, `hiddenAndStubRowsAreNotTracks` and
  `progressUpdatesWithoutInspection` pass unchanged.
- [ ] Debug builds still warn when a Done plot's time axis is Blocked.

**Complexity:** M

---

### Task 3.2: The pair memory and its clearing rules

**Purpose:** Replace `Memory` (per pair) and `Settlement` (per cell) with one
memory keyed by pair (spec §10.2). Every case the per-cell memory covered
becomes a pair fact. It is learned from the same event and cleared by the
spec's events.

**Files to modify:**
- `src/calculationdemand.h`: remove `Memory`, `Settlement`, `CellKey`,
  `m_settled`, `eraseSettlements()` and `loadFailedSettlement()`. Add
  `PairMemory`, `LearnedFact`, `m_memory` (new type), the helpers below and
  the slot `onCalculationRecordWriteFailed`.
- `src/calculationdemand.cpp`: the helpers, and every slot and hook that
  touched `m_memory` or `m_settled`.

**Technical Approach:**

1. **The data.**
   ```cpp
   /// What this run remembers of one pair (see MEMORY). Plain data.
   struct PairMemory {
       enum class Kind {
           NotApplicable,  ///< there is nothing to run for it: not shown
           Failed          ///< shown with the warning badge and `reason`
       };
       enum class Origin {
           Refused,        ///< NotApplicable: the executor refused an offer of the pair
           ColumnVerdict,  ///< NotApplicable: `columns` found the loaded session not applicable
           Result,         ///< Failed: the engine held a result of the pair that is never stored (status not Ok)
           Job,            ///< Failed: the pair's job ended Failed
           Load,           ///< Failed: the session file could not be loaded
           Write           ///< Failed: the result's record could not be written
       };
       Kind kind = Kind::NotApplicable;
       Origin origin = Origin::Refused;
       QString reason;             ///< Failed only: the text shown, "<title>: <why>" or the load text; never empty
       QSet<QString> columns;      ///< ColumnVerdict only: the ids of the columns that found it so
   };
   /// Session id -> requested calculation instance id -> what this run remembers.
   /// Keyed by pair; nested so that a session's facts are found in O(1).
   QHash<QString, QHash<QString, PairMemory>> m_memory;
   /// A fact the walk learned, applied after the walk (the walk writes memos only).
   struct LearnedFact { QString sessionId; QString calculationId; PairMemory fact; };
   ```
   Kind and origin always come in these pairs:
   - NotApplicable with Refused or ColumnVerdict;
   - Failed with Result, Job, Load or Write.

   Provide small static factories (`refused()`, `columnVerdict(columnId)`,
   `failed(Origin, reason)`) so that no call site builds an invalid pair.

2. **Queries** (const, O(1)):
   - `const PairMemory *remembered(const QString &sessionId, const QString &calculationId) const`.
   - `static bool blocksOffer(const PairMemory *m)`: `m && (m->kind == Failed || m->origin == Refused)`.
     These are the facts that keep a pair from being offered and that a
     loaded session's classification reads. A ColumnVerdict does neither
     (see Decisions).
   - `static bool notApplicableFor(const PairMemory *m, const QString &columnId)`:
     `m && m->kind == NotApplicable && (m->origin == Refused || m->columns.contains(columnId))`.

3. **Writing**, through one function
   `void remember(const QString &sessionId, const QString &calculationId, const PairMemory &fact)`.
   The precedence is fixed:
   - A Failed fact replaces anything.
   - A Refused fact replaces a ColumnVerdict and never a Failed fact.
   - A ColumnVerdict is inserted when nothing is remembered. It unites its
     column id into an existing ColumnVerdict, and it leaves a Refused or
     Failed fact alone.

   `remember()` never schedules. Its callers do, as listed below.

4. **The sources of facts.** Each is one call site:

   | Fact | Where | Replaces today's |
   |---|---|---|
   | NotApplicable / Refused | `offerChoice()`, `MissingInput` / `NothingToDo` / `UnknownCalculation` (cpp 1022-1031), then `scheduleUpdate()` as today | `m_memory.insert(key, Memory{NotApplicable})` |
   | NotApplicable / ColumnVerdict | learned by the walk (Task 3.3) from a loaded column track classified NotApplicable: for every storable calculation of the column that is not `blocksOffer()` | the NotApplicable settlement (cpp 621-623) |
   | Failed / Result | learned by the walk (Task 3.3) from a loaded track (plot or column) whose report is NotProduced: for each note whose `status != ResultStatus::Ok` and whose calculation is in the source's `storable`, with reason `"<titleOf(note.calculation)>: <detail>"` (the default detail as in `failureReason()`) | the Failed settlement of an exception result |
   | Failed / Job | `onJobFinished`, `state == JobState::Failed` (cpp 1554-1559), reason `record.calculationTitle + ": " + record.reason` as today | `Memory::Kind::JobFailed` |
   | Failed / Load | `onFillLoadFailed(id)`, for every storable calculation of every column source, then `scheduleUpdate()`. The walk's placeholder rule learns the same facts (Task 3.3). The reason is `tr("The session file could not be loaded")` | `loadFailedSettlement()` (cpp 752-756, 916-922, 734-744) |
   | Failed / Write | the new slot `onCalculationRecordWriteFailed(sessionId, calculationId, reason)`, direct connection to `SessionModel::calculationRecordWriteFailed`, reason `"<title>: <reason>"` with `title = CalculationRegistry::instance().title(calculationId)` (the id when empty), then `scheduleUpdate()` | nothing (the new §7 failure) |

   Connect the new slot in the constructor inside `if (m_sessionModel)`, next
   to the other `SessionModel` connections (cpp 85-105). The comment says it
   is emitted from inside the engine's explicit-result listener and only
   records and schedules, as the `calculationRecordsChanged` connection's
   comment says (cpp 107-110). **Never name the result store's class** in
   the demand layer's files (audit "one result store, owned by the session
   model"). Write "the result store".

5. **Forgetting**, through
   `bool forgetSession(const QString &sessionId, Forget which)` with
   `enum class Forget { Everything, LoadFailures }`, and
   `bool forgetPair(const QString &sessionId, const QString &calculationId)`.
   Both return true when something was forgotten.
   - **A relevant input change** of the session (`onDependencyChanged`,
     after the publishing-job early return, where today
     `m_memory.removeIf(...)` and `eraseSettlements(sessionId)` stand, cpp
     1433-1436): `forgetSession(s, Everything)`. A bulk edit arrives here
     (Phase 1): for a stub it publishes the attribute's own key, for a loaded
     row what the engine invalidated. There is no other clearing for a
     session that is not loaded: the demand layer observes no display change
     of the model, so the column worker's processing of a stub, its
     eviction's recomputation included, forgets nothing.
   - **A record change of a pair** (`onCalculationRecordsChanged`):
     `forgetPair(s, id)`, together with the memo drops of today (cpp
     1538-1540), then `scheduleUpdate()`. This is spec §7's "a later
     successful write clears it". On the failing write itself, the manager's
     record change comes first and forgets whatever was remembered; the
     write-failed signal follows and records the failure. See Gotchas.
   - **A registry change** (`onRegistryChanged`): `m_memory.clear()`, which
     replaces `m_memory.clear()` and `m_settled.clear()`.
   - **A model reset** (`onSessionModelReset`): forget the sessions that have
     no row: `m_memory.removeIf(... !ids.contains(it.key()))`. This replaces
     the `m_memory.removeIf` and `m_settled.removeIf` lines (cpp 1478 and
     1481). A sort resets the model and forgets nothing else.
   - **A successful load** (the `sessionLoaded` lambda): if the row is loaded
     and not a placeholder, `forgetSession(s, LoadFailures)`. The failed-load
     facts of a session that has just loaded are false. See Decisions.
     `sessionLoaded` is also emitted for a placeholder: `sessionRef()` emits
     it after a failed load (sessionmodel.cpp 1086-1109). For a placeholder
     nothing is forgotten.
   - **An id correction** (`onFillLoaded(id, held)` with `held != id`):
     `forgetSession(id, Everything)`, which replaces `eraseSettlements(id)`.

   The nested hash keeps every per-session operation O(1) plus the session's
   own facts.

6. **Remove** `Memory`, `Settlement`, `CellKey`, `m_settled`, `eraseSettlements()`
   and `loadFailedSettlement()`. Also remove every mention of "settlement" in
   `src/` comments (Task 3.6).

**Acceptance Criteria:**
- [ ] `Memory`, `Settlement`, `m_settled`, `eraseSettlements`,
  `loadFailedSettlement` and `CellKey` do not exist in `src/`. No comment in
  `src/` says "settlement" or "settled" in the sense of the removed memory.
- [ ] Every fact of the table in item 4 is written by exactly the listed call
  site, through `remember()`.
- [ ] Each clearing rule of item 5 is implemented at its slot and nowhere
  else. A multi-row change (the unit system, the environment check) clears
  nothing.
- [ ] `SessionModel::calculationRecordWriteFailed` is connected with a direct
  connection. `CalculationResultStore` does not appear in `src/calculationdemand.*`.

**Complexity:** L

---

### Task 3.3: The classification rules

**Purpose:** Implement the rules of spec §10.1 for a loaded track and for a
track that is not loaded, and spec §7 for a failed write. One helper builds
every failure from remembered facts, so all rules share one path.

**Files to modify:**
- `src/calculationdemand.h` / `.cpp`: replace `classify()` and
  `classifyUnloaded()` with the two functions below and one helper.

**Technical Approach:**

1. **The shared helper.**
   ```cpp
   /// A Failed track from remembered failures: `titles` and `facts` are
   /// parallel. The reason is the distinct reasons joined by "; " in order;
   /// jobFailure is true unless every fact's origin is Result.
   static DemandTrack failedTrack(const QString &sessionId, const QStringList &titles,
                                  const QList<const PairMemory *> &facts);
   ```
   Today there are three separate places that do this: the job-failure
   branches (cpp 485-490 and 721-727) and the load settlement (cpp 736-744).
   All of them now use this helper. Reasons are deduplicated so that the load
   failure of a column over two calculations reads once.

   `jobFailure` follows the field's meaning ("not stored; retried at the next
   start"). A Result fact (an exception result) gives false, which matches
   what the loaded classification of a NotProduced report gives today (cpp
   444-449).

2. **`DemandTrack classifyLoaded(const QString &sessionId, const Source &source,
   const BlockerReport &report, const JobRecord &running, QList<LearnedFact> *learned) const`**,
   for a loaded row that is not a placeholder. This is today's `classify()`
   (cpp 428-501, Phase 2's version without `chosen`), with these changes:
   - **Available** gives Done, **unless** some storable calculation of the
     source is remembered Failed. Then it gives `failedTrack()` over those
     calculations (their `storableTitles`). This is spec §7: "a source is done
     only when none of its storable calculations is remembered this way". In
     the product only a Write fact can meet it. A Job or Result fact means the
     result is absent. A Load fact is forgotten when the session loads.
   - **NotApplicable** gives NotApplicable. If `source.kind == Column`,
     append a ColumnVerdict for every storable calculation that is not
     `blocksOffer()` to `*learned`.
   - **NotProduced** gives Failed with titles and `failureReason(notes)` as
     today, `jobFailure` false. Append a Result fact to `*learned` for each
     note described in the Task 3.2 table.
   - **Blocked** keeps today's order: running first, then failures, then
     refusals, then Waiting.
     - A blocker is running when it is the running job and the job was not
       asked to stop. Then the track is Running.
     - Otherwise, when some blocker is remembered Failed (any origin), the
       track is `failedTrack()` over those blockers, using `titleOf(blocker)`.
     - Otherwise, when **every** blocker is remembered **Refused**, the track
       is NotApplicable. For a column, append ColumnVerdicts for its storable
       calculations that are not `blocksOffer()`, as in the NotApplicable
       case.
     - Otherwise the track is Waiting, with the titles of the blockers that
       are not refused.
     - A ColumnVerdict is never read here. See Decisions.

   The walk sets `track.sessionName` (Task 3.4). This function only
   classifies.

3. **`DemandTrack classifyUnloaded(int row, const SessionRow &sr, const Source &source,
   QList<LearnedFact> *learned)`**, for a column track whose row is not loaded
   or is a failed-load placeholder. The rules come in spec §10.1's order:
   1. `source.storable` is empty. The track is NotApplicable. This is a
      source over explicit family instances alone.
   2. Some storable calculation is remembered Failed. The track is
      `failedTrack()` over those calculations.
   3. Every storable calculation is `notApplicableFor(..., source.id)`. The
      track is NotApplicable.
   4. Every storable calculation has a record (`recordSet(sessionId)`). The
      track is Done, or Failed with the first recorded reason. This is
      today's step 3, cpp 684-706, unchanged: titles, reason text,
      `jobFailure` false.
   5. The row is a failed-load placeholder (`sr.isLoaded() && sr.loadFailed`).
      Append a Load fact for every storable calculation to `*learned`. Return
      exactly the track that rule 2 will give once those facts are applied:
      `failedTrack()` over every storable calculation with the Load fact. The
      next pass then announces nothing, and
      `failedLoadSessionShowsBadgeNotPending` asserts that nothing repaints.
   6. Otherwise the track is Waiting.

   Rules 2 and 3 read the memory only. The manager is asked (rule 4) only
   after them. So `passOverManyStubsReadsEachRecordSetOnce` (2000 lookups,
   then 2001) is unchanged.

   How this order differs from today's order (cpp 657-750: settlement,
   records, job failure, not applicable, placeholder) is described under
   Decisions.

4. **Names.** Neither function reads the model. The walk sets `sessionName`
   for listed tracks (Running, Failed) from `model.sessionDisplayName(row)`,
   once per row, lazily. Today's code does this in `walkColumns()` (cpp
   609-617) and in each rule of `classifyUnloaded()`.

**Acceptance Criteria:**
- [ ] A loaded source whose report is Available, with a storable calculation
  remembered Failed/Write, is Failed with `"<title>: <reason>"` and
  `jobFailure` true, on a plot row and on a column header.
- [ ] A loaded column that is NotApplicable learns a ColumnVerdict for each
  storable calculation not already blocked. After eviction the column is
  NotApplicable for that session. A second column over the same calculation
  is not affected by that verdict, loaded or not.
- [ ] An exception result (a note with status Failed) of a loaded track is
  learned as a Result fact. After eviction the column track is Failed with
  the same reason and is not loaded again (`columnFailuresAreBadgedNotReloaded`).
- [ ] A failed-load placeholder gives the same track in the pass that learns
  its facts and in every later pass.
- [ ] The unloaded rules run in the order of item 3. The manager is asked
  only in rule 4.

**Complexity:** M

---

### Task 3.4: The one walk and the pass

**Purpose:** One walk over the session rows for all sources, under one guard,
returning plain values: states, counts, listed tracks, pending cells,
candidates by tier, load candidates, the pending-session set and the learned
facts. Offers, withdrawals, holds, loads and signals happen after it (spec
§10.1).

**Files to modify:**
- `src/calculationdemand.h`: remove `ColumnWalk`, `walkColumns()`, `plotCandidates()`,
  `buildState()` and `classify()`. Add `Walk` and `walkRows()`.
- `src/calculationdemand.cpp`: `walkRows()`, and `recompute()` rewired.

**Technical Approach:**

1. **The walk's value.**
   ```cpp
   /// What the one walk returns: plain values, built under one guard.
   struct Walk {
       enum Tier { FocusedPlots, OtherPlots, VisibleColumns, HiddenColumns, TierCount };
       std::array<QList<Candidate>, TierCount> tiers;  // each in row order; deduplicated by the pass
       QVector<DemandState> states;                    // parallel to m_sources: counts and listed tracks, not finished
       QVector<QSet<QString>> pendingCells;            // parallel to m_sources; empty for plots
       QSet<QString> pendingSessions;                  // sessions with a pending column cell
       QStringList loadCandidates;                     // at most DemandFill::kMaxHeldSessions, row order
       QList<LearnedFact> learned;                     // applied by the pass after the walk
   };
   Walk walkRows(const JobRecord &running, const QSet<QString> &held, const QString &focusedId);
   ```

2. **`walkRows()`.** It holds one `SessionModel::RowStabilityGuard` for the
   whole row loop, as `walkColumns()` does (cpp 593-596).
   - `runningKey` is `(running.sessionId, running.instanceId)` when
     `running.id != 0 && !running.cancelRequested` (cpp 580-582).
   - For each `row`, with `sr = model.rowAt(row)` and `id = sr.sessionId`:
     - `loaded = sr.isLoaded() && !sr.loadFailed`.
     - `settling = m_settle->isSettling(id)`.
     - `name` stays empty until a listed track needs it.
     - If `!loaded`, run `m_reports.remove(id)`.
     - For each source `s` in `m_sources` order:
       - **Tracks.** A plot source includes the row only when
         `loaded && sr.visible` (today's `isVisibleLoadedTrack()`, cpp
         34-37). A column source includes every row.
       - **Loaded.** Take `report` from `m_reports[id][source.id]` (computed
         and stored by `combinedReport()` when missing). Then
         `track = classifyLoaded(id, source, report, running, &walk.learned)`.
         If `report.state == Blocked && !settling`, file each blocker whose
         key is neither `runningKey` nor `blocksOffer(remembered(...))`. The
         tier is:
         - FocusedPlots for a plot when `id == focusedId`;
         - OtherPlots for any other plot;
         - VisibleColumns for a column when `sr.visible`;
         - HiddenColumns otherwise.
       - **Not loaded** (column sources only):
         `track = classifyUnloaded(row, sr, source, &walk.learned)`. Note
         whether any track of the row is Waiting.
       - If the track is listed (Running or Failed), set `track.sessionName`
         to `name`. Compute `name` once per row with
         `model.sessionDisplayName(row)`.
       - Run `walk.states[s].addTrack(track)`. For a column, when the track is
         Waiting or Running, insert `id` into `walk.pendingCells[s]` and
         `walk.pendingSessions`.
     - After the sources, a row that is not loaded is appended to
       `walk.loadCandidates` when all of these hold:
       - some column track of the row is Waiting;
       - `!sr.visible && !settling && !held.contains(id)`;
       - `walk.loadCandidates.size() < DemandFill::kMaxHeldSessions`.

       This is today's rule from cpp 648-651.
   - The walk calls neither the executor nor the fill. It never loads, pins
     or emits. It writes the report memo and the record-set memo, as today.
     It does not write the memory: facts go to `walk.learned`.

3. **The pass** (`recompute()`, Phase 2's version). The inert branch is
   unchanged. The non-inert branch becomes:
   1. `syncCheckedSet(); syncSources();`
   2. `const JobRecord running = m_queue->job(m_queue->runningJob());`,
      `const QStringList held = m_fill->heldSessionIds();` and
      `const QString focused = m_sessionModel->focusedSessionId();`, all read
      before the guard.
   3. `const Walk walk = m_sources.isEmpty() ? Walk() : walkRows(running, QSet<QString>(held.cbegin(), held.cend()), focused);`.
      With no source, no row is walked (today: "with none, rows are not even
      walked").
   4. `for (const LearnedFact &f : walk.learned) remember(f.sessionId, f.calculationId, f.fact);`.
      These facts change no track of this pass. A NotApplicable or
      NotProduced track stays so, and rule 5 already returned rule 2's track.
      So no further pass is needed.
   5. `const bool shutDown = m_queue->isShutDown();`, then
      `m_fill->update(walk.pendingSessions, shutDown ? QStringList() : walk.loadCandidates);`.
   6. If `!shutDown`, the candidates are the concatenation of the four tiers
      in order. Keep a pair once, in its first tier: the `listed` set of cpp
      1266-1277, applied to all four tiers. Then `offerChoice(candidates)`.
   7. Finish the states. For every source: `state = walk.states.value(s)`,
      `sourceId = source.id`, `requested = true`, `state.finish()`.
      - Plots go into `order` / `states`.
      - Columns go into `columnOrder` / `columnStates` /
        `pendingCells.insert(id, walk.pendingCells.at(s))`.

      A plot source with no track gets the requested, all-zero state, as
      `buildState(id, {})` gave today.
   8. Leave the scope, then `applyStates(...)`. The body and the order of
      emissions are unchanged: columns first, then plots, then
      `statesChanged()`.

4. **What moves in time.**
   - Plot tracks are now classified before the offers, in the walk. Today
     they are classified after them (cpp 1282-1295, "After the offers: the
     executor's jobs as they are now"). Spec §10.1 requires it: "Every
     state's counts and listed tracks are tallied in the same walk".
   - Running cannot change during the offers. An offer never starts a job
     synchronously (the comment at cpp 1250-1252). A replaced chosen next job
     ends Cancelled and is not running.
   - The one difference is a pair the executor refuses. Its plot track shows
     NotApplicable one scheduled pass later, which is what a column cell
     already does today (the refusal schedules the pass). No existing test
     reads a plot state within the pass of a refusal. The refusal tests
     (`columnOfferRefusalIsNotLeftPending`) are column tests.

5. **Remove** `inspect()`, `plotCandidates()`, `walkColumns()`, `ColumnWalk`,
   `buildState()`, `classify()` and `isVisibleLoadedTrack()`, if it is not
   used by `walkRows()`. Remove `isFinal()`, which was used only for
   settlements. Keep `isPending()`, `isListed()`, `titleOf()` and `yName()`.

**Acceptance Criteria:**
- [ ] One `RowStabilityGuard` per pass. `walkRows()` is the only function
  that takes one in `src/calculationdemand.cpp` (apart from
  `isMerelyUncomputed`, which takes none).
- [ ] `walkRows()` calls nothing on `m_queue` or `m_fill` and never loads,
  pins or emits (Phase 2's audit rules and a read of the function).
- [ ] The tiers follow spec §10.1:
  - the focused session's plot pairs;
  - the other plot pairs in row order;
  - the column pairs of visible loaded sessions, then of hidden loaded
    sessions, each in row order;
  - within a session, source order and then blocker order;
  - a pair once, in its first tier;
  - never a pair that `blocksOffer()`, that is running and not asked to
    stop, or whose session is settling.
- [ ] `focusedSessionFirstThenRowOrder`, `columnPriorityFollowsRowOrderAfterPlots`,
  `visibleSessionsFirstWithinColumnDemand`, `sessionShownDuringColumnDemandRunsNext`,
  `changingDemandReplacesChosenNext` and `executorHoldsAtMostRunningAndChosenNext`
  pass unchanged.
- [ ] `changeSignalsAreMinimal`, `columnStateCountsAndPendingCells`,
  `workingIdsFollowStates`, `chainedBlockersContinue` and
  `registryChangeReclassifies` pass unchanged in their signal counts.

**Complexity:** L

---

### Task 3.5: The fill holds nothing once the executor is shut down

**Purpose:** Spec §11 says the fill must "hold nothing once the executor is
shut down or the component goes". Phase 2 kept today's behaviour: holds
survive a shutdown until the component is destroyed. This phase owns
behaviour changes and implements the literal reading.

**Files to modify:**
- `src/demandfill.cpp`: `DemandFill::update()`, the release step.
- `src/demandfill.h`: the class comment (Phase 2's sentence "loads nothing
  once the executor is shut down (the owner's `enabled` hook), and holds
  nothing once the component goes (detach())"), and the doc of `update()`.

**Technical Approach:**
- In `update()` step 2 (Phase 2 doc Task 2.3), release every hold when the
  model is gone **or** `!(m_hooks.enabled && m_hooks.enabled())`. The
  per-hold rule is unchanged otherwise. The fill still never names the
  executor. It learns of the shutdown through its owner's `enabled` hook,
  which is `!isInert() && !m_queue->isShutDown()`.
- When it takes effect: at the first pass after the shutdown.
  `JobQueue::shutdown()` ends every active job synchronously (jobqueue.cpp
  638-663: queued jobs `endJob(... Cancelled ...)`, the running job
  `wait()` then `finishRun()`). Each end runs the demand layer's synchronous
  pass (`onJobFinished`). So with any job active, the holds are released
  inside `shutdown()`. A hold never outlives the running and chosen next
  jobs by more than a scheduled pass, and a scheduled pass after the
  shutdown also releases.
- Rewrite the class comment's contract sentence to spec §11's words: "…
  report progress as the sessions remaining of the high-water mark, and hold
  nothing once the executor is shut down or the component goes".
- The progress counts are unchanged. After a shutdown `hasWork()` is false,
  and the scheduler completes the fill with its progress at that moment
  (Phase 1, Gotchas).

**Acceptance Criteria:**
- [ ] After `m_queue->shutdown()` with two holds, `heldSessionIds()` is empty
  and neither session is pinned, before the demand layer is destroyed.
- [ ] `src/demandfill.*` still never calls the executor and names none of the
  walk's types (Phase 2's audit rules).

**Complexity:** S

---

### Task 3.6: The class comment and the presentation comments

**Purpose:** The code documents what it does. No comment in `src/` describes
settlements, two walks or plots classified after the offers.

**Files to modify:**
- `src/calculationdemand.h`: the class comment.
- `src/demandstate.h`: the docs of `DemandCondition::Failed` and `DemandTrack::jobFailure`.

**Technical Approach.** Rewrite these paragraphs of the class comment. The
line numbers are those of `ec6bd43`; Phase 2 already rewrote some of them.
- **WHERE A RESULT IS LOOKED UP** (126-146). For a loaded session, "the
  engine's blocker inspection of the source's names, combined …; a plot's
  single name combines to itself". For a session that is not loaded, the
  six rules of Task 3.3 item 3, in order, worded as the spec words them.
  Delete "a settlement (below) - its verdict" and the old order.
- **TRACK CONDITIONS** (148-171). The table stays. Add two rows:
  - "Available — a storable calculation is remembered failed → Failed (a
    result that could not be stored)".
  - "Blocked — otherwise, a blocker has a remembered failure → Failed". The
    old row said "job failure".

  In "every blocker is remembered not applicable" write "refused".
- **SETTLEMENTS** (173-187). Delete it.
- **MEMORY** (270-277). Replace it with spec §10.2 as the code implements it:
  - one memory keyed by pair;
  - the kinds, and the origins with their sources (Task 3.2's table);
  - which facts keep a pair from being offered and which a loaded session's
    classification reads;
  - the column verdict holds for its column only;
  - the clearing rules (no display change of the model is among them);
  - "Nothing is persisted".
- **WHAT A PASS COSTS** (189-206). Every source's combined report is
  memoized per loaded session and dropped by the listed events. A plot's
  report is computed for its tracks only. Delete "Plot classifications are
  not cached across passes".
- **THE CHOICE** (208-225, Phase 2's wording). The tiers and the filing
  rule of Task 3.4. "A pair is not a candidate while a failure or a refusal
  is remembered for it, while it is the running job not asked to stop, or
  while its session is settling."
- **WHEN A PASS RUNS** (279-292). Add
  "SessionModel::calculationRecordWriteFailed". No display change of the
  model is listed (Phase 1 removed it).
- **PARTS** (Phase 2's paragraph). "The walk reads the rows under one guard,
  asks the clock and reads a copy of the holds; it returns every state,
  candidate and learned fact as plain values".

In `src/demandstate.h`:
- `DemandCondition::Failed`: "an input-determined failure (NotProduced), or a
  failure remembered this run: a job that failed, a session that could not
  be loaded, a result that could not be stored".
- `DemandTrack::jobFailure`: "Failed only: not a stored result (a job that
  failed, a load that failed, a record that could not be written); tried
  again at the next start".

Say "the executor", "the result store" and "the session model" where the
audit forbids the class names.

**Acceptance Criteria:**
- [ ] `grep -rniE "settlement|m_settled|ColumnWalk|walkColumns|plotCandidates|inspectUnderGuard|JobFailed" src` finds nothing.
- [ ] The class comment states the unloaded rules in spec §10.1's order and
  the memory's clearing rules of Task 3.2.

**Complexity:** S

---

### Task 3.7: Tests

**Purpose:** Prove spec §14's bullets "A record write that fails", "Plots and
columns are classified … by one walk" and "One memory", and the fill's
shutdown rule. Keep every existing assertion.

**Files to modify:**
- `tests/tst_calculation_demand.cpp`: new test functions (declared in
  `private slots:` in the order given), the two edited tests, and three
  comments.

**Technical Approach:** See Testing Requirements. No test reads the removed
internals: they were private. The only edits forced are the behaviour change
of Task 3.5 and comments that name settlements.

**Acceptance Criteria:**
- [ ] The new tests pass, and the full suite passes sequentially.
- [ ] No existing test function is renamed or removed.

**Complexity:** L

---

### Task 3.8: The audit, the acceptance map, and the documentation debt

**Purpose:** Keep `audit_cleanup` and `tests/acceptance_map.txt` green, and
list what Phase 5 must rewrite.

**Files to modify:**
- `tests/audit/cleanup_audit.cmake`: Phase 2's rule "the fill and the settle
  clock know nothing of the walk". In its regex
  `"BlockerReport|blockers\\(|DemandTrack|DemandState|DemandCondition|RowStabilityGuard|Settlement"`,
  replace `Settlement` with `PairMemory|LearnedFact`. Leave everything else
  alone. Plant `// PairMemory` in `src/demandfill.cpp` once, see the rule
  trip, and remove it.

**Technical Approach:**
- `tests/acceptance_map.txt` does not change. No cited test function is
  renamed or removed. The new test functions are cited by Phase 5 (list in
  the hand-off).
- Check each rule this phase could trip:
  - "one result store, owned by the session model": no
    `CalculationResultStore` in the demand layer's files.
  - "the demand layer never loads a session or reads a record itself": the
    regex `sessionRef\(|loadSession\(|readCalculationRecord|calculationRecordIds\(|restoreSession\(|restoreStoredResults\(`.
    The new names `onCalculationRecordWriteFailed` and
    `calculationRecordWriteFailed` match none of these.
  - "one call of offer(": still one.
  - "one authority: explicit-backed" (`EvaluationPolicy::Explicit`): the demand
    layer calls `explicitDependencies()`, not the policy.
  - "hidden loads go through loadPinnedSession" and "the demand layer loads
    and pins through its fill only": unchanged.
- Do not edit `docs/` or `tests/README.md` (Phase 5). The list of passages is
  under "Hand-off to Phase 5" below.

**Acceptance Criteria:**
- [ ] `audit_cleanup` passes, including its traceability check.
- [ ] The edited regex trips on a planted `PairMemory` in `src/demandfill.cpp`.

**Complexity:** S

## Testing Requirements

### Unit Tests

A **helper** for the failed write goes in the private section of
`CalculationDemandTest`. It blocks a record by putting a directory at its
path, the mechanism of Phase 1's `writeFailureIsAnnounced` and of
`tst_result_store::writeFailureLeavesResultUsable`:
```cpp
/// The record file of (session, calculation), where the manager writes it:
/// TestEnvironment::cacheDir() + "/" + recordFileName(sessionFileStem(id), calculationId).
static QString recordPath(const QString &id, const QString &calculationId);
```
`recordFileName()` is the public free function of `src/calculationrecord.h`
(calculationrecord.h ~87: "stem + "." + encodeRecordFileId(calculationId) +
"." + extension"). Add `#include "calculationrecord.h"`. `logbookprobe.h`
(for `sessionFileStem`) and `testenvironment.h` (for `cacheDir()`) are
already included. The test creates the folder with
`QDir().mkpath(path)` and removes it in a `qScopeGuard` with `QDir().rmdir(path)`.
Wrap the passes that fail to write in a `WarningCapture` (testutil.h) and
assert the "not written" warning, as `writeFailureLeavesResultUsable` does.

**New test functions** go in `tests/tst_calculation_demand.cpp`. Declare them
in `private slots:` after `bulkEditMakesSettledSessionApplicable`, in this
order.

1. **`failedRecordWriteIsShownAndNotRetried`** (column demand, a session that
   is not loaded):
   1. `giveInput({"s1"}, "G_IN", 1)`, `makeStubs()`, `waitForIdle`. Block
      `recordPath("s1", "gated")`. Start a `QSignalSpy loadedSpy(sessionLoaded)`.
      `gate().open(1)`, then `enableColumns({"G_OUT"})`, `waitDemandIdle()`
      and `waitForIdle(*m_model)`. (s2-s4 have no `G_IN`: they are loaded to
      find the column not applicable, as today.)
   2. Check:
      - `jobOf("s1","gated").state == Succeeded`;
      - `!stored("s1","gated")`;
      - `col("G_OUT")` has `sessionIdsOf(failed) == {"s1"}`,
        `failed.at(0).reason.startsWith("Gated: Couldn't write file")`,
        `failed.at(0).jobFailure`, `failed.at(0).calculationTitles == {"Gated"}`
        and `failed.at(0).sessionName == "Jump 1"`;
      - `showsWarning()`;
      - `!isCellPending("s1","G_OUT")`;
      - `loadsOf(loadedSpy,"s1") == 1`.
   3. **Eviction.** Spy on `m_model`'s `dataChanged`. `makeStubs()`, `spin()`
      ×3, `waitForIdle(*m_model)`. Assert that the spy saw a single-row
      `DisplayRole` change of `rowOf("s1")`. The column worker processed the
      stub, because the eviction dropped the values over the record that
      could not be written. After that, `col("G_OUT")` still lists s1 failed
      with the same reason, `loadsOf("s1") == 1`, `jobCount("gated") == 1`
      and `!hasFillWork()`.
   4. **Sort.** `m_model->sort(0, Qt::DescendingOrder)`, `spin()`. Same
      assertions.
   5. **The column worker.** `m_model->startColumnWorker()`, `waitForIdle`,
      `spin()`. Same assertions. Hold a `Quiet` over steps 3-5.
   6. **A restart tries again.** `gate().open(1)`, `restartDemand()`,
      `waitDemandIdle()`, `waitForIdle`. Now `jobCount("gated") == 2`,
      `loadsOf("s1") == 2`, and s1 is failed again with the same reason
      prefix.
   7. **A later successful write clears it.** `rmdir` the folder. Then
      `makeStubs({"s1"})`: the second run left s1 loaded in the pool with the
      result installed, and a request of an installed result publishes
      nothing. Then `session("s1")`, which loads a fresh engine without the
      result. After `m_demand->flush()`, s1 is still failed (Blocked on
      `gated`, which is remembered failed) and `Quiet` holds: it is not
      offered. Then `gate().open(1)` and
      `QCOMPARE(engine("s1").request(QStringLiteral("gated")).status, ResultStatus::Ok)`.
      Now `stored("s1","gated")` holds, and after `flush()`, `col("G_OUT")`
      has `failedCount == 0`, s1 done, and `isPlain()`.

2. **`failedRecordWriteIsShownOnThePlotRow`** (plot demand, a loaded session):
   1. `giveInput({"s1"}, "G_IN", 4)`, `show({"s1"})`, block
      `recordPath("s1","gated")`, `gate().open(1)`, `check("g")`,
      `waitDemandIdle()`.
   2. Check `row("Syn/g")`:
      - `failedCount == 1`, `failed.at(0).sessionId == "s1"`;
      - the reason starts "Gated: Couldn't write file", `jobFailure` is true;
      - `showsWarning()`, and the tooltip contains
        `"  Jump 1 - Gated: Couldn't write file"`;
      - `values("s1","g") == {5.0}` (the result is installed; the write
        failed).
   3. With a `Quiet` held over `spin()` ×3, nothing is offered.
   4. Hide and evict s1 (`makeStubs({"s1"})`), then `show({"s1"})` and
      `waitForIdle(*m_model)`. The visible loader loads it and the fresh
      engine has no result. `row("Syn/g")` still has s1 failed with the same
      reason, and `Quiet` still holds.

3. **`settledPairsSurviveEvictionSortAndColumnWorker`** (spec §14 "One
   memory": a session found not applicable, a job-level failure, a failed
   load and a failed write). Four stubs, columns `G_OUT` and `X_OUT`:
   - s1 has `G_IN 1` and `recordPath("s1","gated")` blocked: a failed write.
     It is also not applicable to `X_OUT`.
   - s2 has no input: not applicable to both.
   - s3 has `G_IN 3`, and its session file is removed after `makeStubs()`: a
     failed load.
   - s4 has `X_IN 4`: `exhausted` throws `bad_alloc` on its first call, a
     job-level failure. It is not applicable to `G_OUT`.

   Steps:
   1. `gate().open(1)`, `enableColumns({"G_OUT","X_OUT"})`,
      `waitDemandIdle()`, `waitForIdle`.
   2. Record the baseline:
      - `col("G_OUT")` failed = {s1 (write), s3 (load)};
      - `col("X_OUT")` failed = {s3 (load), s4 (job)};
      - every wanted count;
      - `loadsOf` per session (1 each; s3's placeholder counts once);
      - the job count.
   3. **Eviction:** `makeStubs()`. Capacity 0 evicts every hidden row, and
      `evictSession` turns s3's placeholder back into a stub
      (sessionmodel.cpp 2032-2045). Then `spin()` ×3 and `waitForIdle`.
   4. **Sort:** `m_model->sort(0, Qt::AscendingOrder)`, `spin()`.
   5. **Column worker:** `m_model->startColumnWorker()`, `waitForIdle`,
      `spin()`.
   6. After each of steps 3-5, and with one `Quiet` held over all three:
      both column states equal the baseline (compare `failed` session ids,
      reasons and the counts), no `loadsOf` grew, and `!hasFillWork()`.

   This test also pins that the column worker's display changes are not
   observed: the worker processes s1's stub in step 3 (its eviction dropped
   the values over the unconfirmed record), and s1's `X_OUT` verdict and
   `G_OUT` failure survive it.

4. **`pairMemoryIsClearedByRecordInputAndRegistryChanges`** (plot demand,
   job-level failures made with `m_queue->failNextWorkerStarts(1)`):
   1. `giveInput({"s1"},"G_IN",4)`, `show({"s1"})`,
      `failNextWorkerStarts(1)`, `check("g")`, `waitDemandIdle()`. s1 is
      failed with `jobFailure`.
   2. **Record change of the pair.**
      `emit LogbookManager::instance().calculationRecordsChanged("s1","gated")`,
      `gate().open(1)`, `waitDemandIdle()`. `jobCount("gated") == 2`, the
      last job Succeeded, and the row is plain.
   3. **Input change.** `failNextWorkerStarts(1)`,
      `giveInput({"s1"},"G_IN",5)`, `settle()`, `waitDemandIdle()`: failed.
      Then `giveInput({"s1"},"G_IN",6)`, `gate().open(1)`, `settle()`,
      `waitDemandIdle()`: Succeeded, plain.
   4. **Registry change.** `failNextWorkerStarts(1)`,
      `giveInput({"s1"},"G_IN",7)`, `settle()`, `waitDemandIdle()`: failed.
      Then add an unrelated explicit calculation with `m_extra->add()` (any
      id, input `"UNRELATED_IN"`), `gate().open(1)`, `waitDemandIdle()`:
      Succeeded, plain.
   5. Between the failure and each clearing, `Quiet` holds over `spin()`: a
      remembered failure is not offered.

5. **`columnVerdictDoesNotSuppressAnotherColumn`** (the Decision on column
   verdicts). Add with `m_extra->add` an **on-demand** calculation
   `test.demand.needsZ`: inputs `G_OUT` and `Z_IN`, output `Z_OUT`, compute
   `G_OUT + Z_IN`. With `G_IN` and no `Z_IN`, the engine reports `Z_OUT`
   NotApplicable while `gated` is requestable.
   1. `m_demand->setInputSettleDelay(60000)`.
      `enableColumns({"Z_OUT","G_OUT"})`, so that `Z_OUT` comes first in
      source order. `flush()`.
   2. `giveInput({"s1"},"G_IN",4)`: s1 settles. `flush()`: the walk learns
      `Z_OUT`'s verdict for (s1, gated). `flush()` again, or
      `hasPendingUpdate()` false.
   3. `col("G_OUT")` counts s1 as waiting (`wantedCount == 1`,
      `waitingCount == 1`). `col("Z_OUT").wantedCount == 0`.
   4. `endInputSettleWaits()`, `flush()`. `(s1, gated)` is the running or
      chosen next job: `jobOf("s1","gated").id != 0`.
   5. `gate().open(1)`, `waitDemandIdle()`. `G_OUT` is done for s1 and
      `Z_OUT` is not applicable.
   6. `makeStubs()`, `spin()` ×3, with a `QSignalSpy` on `sessionLoaded`
      started before. `col("G_OUT")` has `doneCount == 1`, `wantedCount == 1`
      (the record); `col("Z_OUT").wantedCount == 0`; no load.

   A literal pair verdict would make s1's `G_OUT` NotApplicable in step 3
   and step 6, and step 4 would find no job.

6. **`runningColumnTrackFilesItsOtherBlockers`** (the spec §10.1 filing rule,
   which differs from today's column path). Add an on-demand
   `test.demand.pair`: inputs `G_OUT` and `S_OUT`, output `PAIR_OUT`.
   `giveInput({"s1"},"G_IN",1)` and `giveInput({"s1"},"S_IN",1)`.
   `enableColumns({"PAIR_OUT"})`, `gate().waitEntered()`, `flush()`.
   - `running()` is (s1, gated), and `chosenNext()` is (s1, stubborn): the
     second blocker of a Running track is filed.
   - Today's column path filed nothing from a Running cell, and the chosen
     next job would have been 0.
   - `gate().open(2)`, `waitDemandIdle()`. Both jobs Succeeded, and
     `col("PAIR_OUT")` is done for s1.

7. **`successfulLoadForgetsFailedLoadFacts`** (spec §10.2: a successful
   load forgets the session's failed-load facts). Start as
   `visibleFailedLoadIsSettledAsFailed` does, but copy s2's session file
   aside before `QFile::remove(sessionFilePath("s2"))`. After the first
   `waitDemandIdle()`, s2 is listed failed ("The session file could not be
   loaded"). Then put the file back and make the product load s2 again by
   the real path: hide s2 and evict it (`makeStubs({"s2"})`; `evictSession`
   turns a failed-load placeholder back into a stub, sessionmodel.cpp ~2034,
   "so that a later access retries the load"), then `show({"s2"})`, which
   loads it. `waitDemandIdle()`, then check:
   `sessionIdsOf(col("G_OUT").failed)` is empty, `jobOf("s2","gated").state
   == Succeeded`, and `failedCount == 0`.

**Existing tests to edit** (no function renamed):

| Test | Today | Change (same fact, or the Task 3.5 behaviour) |
|---|---|---|
| `noLoadsAfterExecutorShutdown` (~3781-3810) | `QCOMPARE(m_demand->heldSessionIds(), QStringList({"s1", "s2"}));` after the shutdown | `QCOMPARE(m_demand->heldSessionIds(), QStringList());` and `QVERIFY(!m_model->isSessionPinned("s1"))`, `QVERIFY(!m_model->isSessionPinned("s2"))` right after `m_demand->flush()`. The tail (`m_demand.reset()` and the pin checks) stays. Comment: "Nothing is loaded after the executor has shut down, the scheduler goes idle, and no session is held once it has." |
| `demandDestroyedReleasesHoldsAndTask` (~3812-3838) | shuts the executor down, asserts s1/s2 still pinned, then destroys | Reorder: with the holds `{"s1","s2"}`, connect the activation counter, then `m_demand.reset()`, then `m_queue->shutdown()`. The executor's own pins go at the shutdown, so `!isSessionPinned` for s1 and s2 now proves the destructor released the holds. Then `waitForIdle`, `fillActivations == 0`. Comment: "The destructor unregisters the fill and releases every hold." |
| header comment (line 12) | "settlements;" | "the pair memory;" |
| `notApplicableSessionIsSettledWithoutAJob` comment (~2985) | "Evicted: the settlement keeps it from being loaded again" | "Evicted: the remembered verdict keeps it from being loaded again" |
| `bulkEditMakesSettledSessionApplicable` comment (~3677-3678) | "the single-row change clears the settlement" | "the single-row change clears what the run remembered of the session" |

**Existing tests that must pass unchanged** (they assert the facts this
phase re-implements):
- The pair memory: `notApplicableSessionIsSettledWithoutAJob`,
  `columnFailuresAreBadgedNotReloaded`,
  `columnJobLevelFailureIsNotReloadedUntilRestart`,
  `columnOfferRefusalIsNotLeftPending`, `unloadableSessionIsSettledAsFailed`,
  `visibleFailedLoadIsSettledAsFailed`,
  `bulkEditMakesSettledSessionApplicable`,
  `jobLevelFailureIsBadgedNotRerunUntilRestart`,
  `inputDeterminedFailureIsStoredBadgedNeverRerun`,
  `removedSessionLeavesNoTrace`,
  `storedRejectionIsBadgedAfterRestartWithoutLoad` and Phase 1's
  `recordReasonReachesDemandThroughRecordChange`.
- The walk and the tiers: all of `tst_calculation_demand`,
  `tst_logbook_indicators::failedLoadSessionShowsBadgeNotPending` (nothing
  repaints once settled), `tst_result_columns::columnWorkerIsUnchangedByDemand`,
  `staleRecordDeletedByWorkerCreatesDemand`, `tst_fusion_rows` (all) and
  `tst_fusion_store::columnOverFusionFillsUnloadedSessions` /
  `fusionColumnWithStoredFitsRunsNothing`.
- The acceptance items 501-563 cite these tests. None reads a removed
  internal, so each keeps asserting its fact with no edit other than the
  table above.

### Integration Tests

- The full suite, sequentially:
  `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure`.
  The executor-driven fusion tests (`tst_fusion_rows`, `tst_fusion_store`,
  `tst_fusion_jobs`, `tst_fusion_session`) must pass without `-j`.
- `audit_cleanup`, including the traceability check.

### Manual Verification

1. Build `build-phase1` and run the application. Check a fusion plot with a
   visible session. The row works, and then it goes plain as before.
2. Make the logbook's `cache/` folder read-only, or put a folder at a fit's
   record path. Then check a fusion plot on a session without a stored fit.
   After the fit, the row shows the warning badge. Its hover lists the session
   with "Sensor fusion: Couldn't write file …". The plot still draws the fit.
   Hiding and showing the session does not run the fit again. Restarting the
   application tries once more.
3. Enable a fusion column on a logbook with sessions without IMU data. They
   are loaded once each and then left alone. Sorting the logbook or evicting
   (lower the cache size) does not load them again. A bulk edit of such a
   session loads it once more.

## Notes for Implementer

### Build and test (from the overview's Decisions & Constraints)
- Build **only** `build-phase1/`: `cmake --build build-phase1 --config Release`.
  **Never build `build/`.**
- Test with `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure`,
  **sequentially, never `-j`**, with no stray `ctest` / `tst_*` process
  running (the executor-driven fusion tests are load-sensitive). One target:
  add `-R tst_calculation_demand`. The audit: `-R audit_cleanup`.
- This phase adds no source file. Reconfigure the inner project
  (`cmake build-phase1/FlySightViewer-build`) only if you add one (see
  Decisions: no split is planned). A new test source that links fusion would
  go in `_FLYSIGHT_GTSAM_REACHERS` (`cmake/SolverDependencies.cmake`); none is
  planned.
- Commits are the orchestrator's. Never run a git command that changes
  repository state.

### Gotchas
- **Signal order on a failing write.**
  - The manager emits `calculationRecordsChanged(s, c)` first, from its
    `fail()` lambda (logbookmanager.cpp 1095-1101). `forgetPair(s, c)` runs
    and forgets whatever was there.
  - The model's relay `calculationRecordWriteFailed(s, c, reason)` follows and
    records the Write fact.
  - Do not queue either connection. Both are direct and both only record and
    schedule.
  - For an unknown session only the second signal comes. The fact is
    recorded, and a reset forgets it if the session has no row.
- **The failed write makes the column worker process the stub, and that
  must clear nothing.**
  - `SessionModel::evictSession` drops the values over an unconfirmed record
    and calls `startColumnWorker()` (sessionmodel.cpp 2062-2080: "Values over
    records the engine could not vouch for (a failed write or removal, a
    record skipped at the load) go with the engine; the worker computes them
    again from the records on disk").
  - The worker then emits the single-row display change for that stub
    (sessionmodel.cpp 2138 or 2180). The demand layer does not observe it
    (Phase 1), so the Write fact survives. Do not add a `dataChanged`
    connection for any reason: it would bring back the reload loop that
    spec §7 ends (load, fail to write, evict, clear, load again). Test 3 and
    step 3 of test 1 pin this.
- **Rule 5 must equal rule 2.** A placeholder's track in the pass that learns
  its Load facts must be identical to the track of every later pass: same
  titles, reason and `jobFailure`. Otherwise the column announces a change
  once more, and `failedLoadSessionShowsBadgeNotPending` sees a repaint.
- **Facts are applied after the walk.** A ColumnVerdict learned for column C
  in a pass is not visible to column D of the same row in that pass. It must
  not be, and nothing reads a ColumnVerdict for a loaded row anyway. The
  learned Result and Load facts do not change the pass's own tracks.
- **The report memo covers plots now.** If a plot test shows a stale state,
  find the event that should have dropped the session's memo. The drops are
  `dependencyChanged` (any key; a bulk edit of a loaded row publishes one),
  a job's end, a load, a record change, a reset and a registry change. Do
  not bypass the memo for plots.
- **`sessionDisplayName(row)` inside the guard** is a plain read (Phase 1).
  Call it at most once per row, and only for a listed track.
- **Tier dedupe.** A pair filed in two tiers (a plot pair and a column pair
  of the same visible session) is offered in its first tier only. Keep the
  `listed` set over all four tiers.
- **The shutdown release** happens in `DemandFill::update()`, not in the
  pass. The inert branch's `m_fill->update({}, {})` already releases
  everything.
- **Audit text rules match comments.** Never write `CalculationResultStore`,
  `->offer(`, `.offer(`, `->withdrawChosenNext(`, `->pinSession(` or
  `.loadPinnedSession(` in a demand-layer file. Never write `PairMemory` or
  `LearnedFact` in `src/demandfill.*` or `src/demandsettleclock.*` after
  Task 3.8.
- **`recordSetLookups()`** counts manager lookups for rows that are not
  loaded. Loaded rows must not look up records: the §7 rule reads the memory
  only.

### Decisions Made

- **A column's not-applicable verdict holds for that column only**
  (`PairMemory::columns`, origin ColumnVerdict), as spec §10.2 has it. A
  fact for the pair would suppress other sources that need the same
  calculation, because the engine reports a name NotApplicable whenever one
  input is genuinely missing, even though the requested calculation behind
  it is requestable. See calculationengine.cpp 1942-1944: "with one input
  blocked and another genuinely missing, a request for the blocker could
  never help, so there is no blocker to report". Concrete cases:
  - a Delta column whose marker is missing, next to a column over the fit's
    output. The archived plan's Decisions (02-column-demand.md, "Column
    settlements") rejected a per-pair not-applicable memory for exactly
    this reason;
  - `builtin.fusion.systemTime` (fusionregistration.cpp 245-271) on a
    session without a GNSS time fit.

  So the fact carries the ids of the columns that found it. The consequences:
  - It is read only by the unloaded rule 3 of those columns.
  - A loaded session's classification and the candidate filter read only
    Failed and Refused facts. The engine is the authority for a loaded
    session.
  - Plots record no verdict. They have no track that is not loaded, so they
    need none.
  - It is still one memory keyed by pair, cleared by the pair and session
    rules. Test 5 pins this.
- **No clearing on a display change of the model.** Phase 1 made the bulk
  edit publish its edit as a dependency change and removed the demand
  layer's display-change slot, so a session's facts are cleared by its
  input change alone (and its failed-load facts by a load). A session whose
  record could not be written has an unconfirmed record, and its eviction
  starts the column worker, which emits a display change for the stub (see
  Gotchas); nothing observes it, so the facts survive and the retry loop
  spec §7 ends (load, fail to write, evict, clear, load again) cannot come
  back. No shield is needed.
- **The two pair facts spec §10.2 lists beside the refusal and the job
  failure**, each covering a case the per-cell settlement covered:
  - **Result:** an unstored result. A NotProduced note whose status is not
    Ok (a computation that threw) is never stored, so without a fact the
    column would be loaded and the calculation rerun after every eviction.
    `columnFailuresAreBadgedNotReloaded` guards this.
  - **The column verdict** above.

  Spec §10.2's claim that "a done track without a record is impossible" holds
  for the product's registrations. The only explicit calculation is the fit,
  whose outputs have one candidate. See Open Questions.
- **A successful load forgets the session's failed-load facts** (spec
  §10.2). They are false once the session has loaded. Without this, a file
  that loads later in the run (repaired, or restored by a merge) would stay
  badged "could not be loaded". This clearing rule is additive: it never
  clears anything else. Test 7 covers it by the real path (a placeholder
  evicted back to a stub, then shown).
- **Candidate filing follows spec §10.1 for plots and columns alike**, as
  today's plot path does: every blocker of a Blocked report that is not
  blocked by the memory, not running and not settling. Today's column path
  (cpp 628-635) filed only from a Waiting cell. So a column track that is
  Running on one blocker, or Failed on one blocker, now files its other
  blockers. Test 6 pins it. Priority, tiers, the settle wait and the load
  candidates are otherwise unchanged in effect. The tiers and the dedupe are
  those of cpp 1263-1277 and 942-997.
- **The unloaded order follows spec §10.1**: memory before records. Today the
  order is settlement, records, job failure, not applicable, placeholder
  (cpp 657-750). The new order changes a result only when a failure or a
  not-applicable fact coexists with a record of the same pair. Examples are a
  failed write over an older record (`writeFailureKeepsPreviousRecord`'s
  situation), and a verdict whose calculation another column computed. In
  both cases the new result matches what the loaded session showed.
- **Plot tracks are classified in the walk, before the offers** (spec
  §10.1). The only observable difference is that a refusal reaches a plot row
  one scheduled pass later. See Task 3.4 item 4.
- **The walk returns learned facts; the pass applies them.** The walk then
  writes nothing but memos, and no fact can influence a later source of the
  same walk.
- **The reconciler stays in `src/calculationdemand.cpp`.** After Phase 2 moved
  the parts out, and this phase replaced two walks with one, the file stays
  near its Phase 2 size. A separate walk file would need `DEMAND_LAYER` /
  `DEMAND_FILES` changes and brings no boundary of spec §11: the walk belongs
  to the reconciler.
- **The fill releases every hold once the executor is shut down** (Task
  3.5). No concrete reason in the code argues against the literal reading:
  `MainWindow` destroys the demand layer right after closing the executor
  anyway.
- **Report memo keyed by source id.** It needs no invalidation when the
  source list changes. See Task 3.1 item 3.

### Open Questions

- **"A done track without a record."** If a future registration gives a
  requested column's name a second, non-explicit path, a loaded track can be
  Done while a storable calculation has no record. That session would be
  loaded again after each eviction, with no job. Today's settlement prevented
  that. The product's registrations cannot produce it: the fit's outputs
  have one candidate, checked by
  `tst_fusion_session::explicitOutputsHaveOneCandidate`. Spec §10.2 states
  the assumption and asks the documentation to state it; Phase 5 does.

### Hand-off to Phase 4
- A failed write is a Failed track with `reason` `"<title>: <manager's
  error>"`, for example "Gated: Couldn't write file '…': …", `jobFailure`
  true, on the plot row and the column header.
  `failedRecordWriteIsShownOnThePlotRow` shows how to provoke one (a folder
  at `recordPath(id, "gated")`). Phase 4's hover test can reuse the same
  helper.

### Hand-off to Phase 5
- **New test functions to cite:** `failedRecordWriteIsShownAndNotRetried`,
  `failedRecordWriteIsShownOnThePlotRow`, `settledPairsSurviveEvictionSortAndColumnWorker`,
  `pairMemoryIsClearedByRecordInputAndRegistryChanges`,
  `columnVerdictDoesNotSuppressAnotherColumn` and
  `runningColumnTrackFilesItsOtherBlockers`, all in `tst_calculation_demand`.
- **Removed names for the demand group's rules:** `Settlement`, `m_settled`,
  `eraseSettlements`, `loadFailedSettlement`, `ColumnWalk`, `walkColumns`,
  `ColumnInfo`, `plotCandidates`, `inspectUnderGuard`, `inspectedPlots`,
  `syncColumns`, `m_columnReports`, `Memory::Kind::JobFailed` / `JobFailed` and
  `buildState`.
- **The demand layer now calls `CalculationRegistry::explicitDependencies()`
  for plots** (a plot's storable requested calculations, Task 3.1). The rule
  "no second computation of a column's requested calculations" must allow
  it in `src/calculationdemand.cpp`, or forbid only
  `logbookColumnExplicitCalculations(`, as Phase 1 proposed. Also update the
  comment above "one authority: explicit-backed"
  (cleanup_audit.cmake 416-421) to say the demand layer reads
  `explicitDependencies()` for plots.
- **Passages that become false:**
  - `docs/CALCULATIONS.md`:
    - §16.1 (~1203-1207, the unloaded order with "a settlement of this run",
      "settled");
    - §16.3 (plot reports not cached; now every source is memoized per
      loaded session);
    - §16.4 (~1418, "not-applicable verdicts and settlements");
    - §16.6 (the column filing rule, the tiers);
    - §16.7 (~1454-1490, "Settlements", the memory, the refusal pass
      "column cells are classified before the offers", ~1471);
    - §16.8 (~1547, ~1579-1585 "settled", and the shutdown: holds are now
      released);
    - the failed write as a shown failure, §16.7 or §15.8.
  - `docs/COMPUTED_PLOTS.md` §7/§8 (the failed write among the failures).
  - `tests/README.md` (the `tst_calculation_demand` row and every mention of
    settlements).
  - Acceptance items that should be restated "(as amended)":
    - 528 (holds end at the shutdown);
    - 532 and 526 (the pair memory in place of settlements);
    - a new item for spec §7.

## Definition of Done

This phase is complete when:
1. All tasks have passing acceptance criteria.
2. All tests pass: the full `ctest` run on `build-phase1`, sequential, with
   `audit_cleanup` and its traceability check.
3. The code follows the patterns of the reference files:
   - plain values out of the one guarded read;
   - direct connections that only record and schedule;
   - one owner per fact;
   - special cases as data (`Source::Kind`, `PairMemory::Origin`).
4. No TODOs or placeholder code remain. No comment in `src/` describes
   settlements, two walks, or plot tracks classified after the offers.
