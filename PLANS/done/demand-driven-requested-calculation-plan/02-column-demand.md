# Phase 2: Column demand

## Overview

This phase extends the demand layer `CalculationDemand` from checked plots to **enabled logbook columns over requested outputs**, for every session in the logbook, loaded or not. It covers:

- where results are looked up: blocker inspection for loaded sessions, and `LogbookManager`'s record set for sessions that are not loaded;
- priority tier (c);
- a per-column state and a per-cell "pending" query for Phase 3 to present;
- a new lowest-priority idle-scheduler task, the **column fill**, whose steps are hidden loads. It loads at most `JobQueue::kMaxRunningJobs + 1` hidden sessions at a time and pins them, and it reports the fill's progress for the fill's whole duration;
- each stored result's outcome recorded in the logbook index, so that a stored rejection of a session that is not loaded reads as a failed result before and after a restart alike.

The column worker, the executor and the scheduler's existing tasks are unchanged. The scheduler gains two generic things: `unregisterTask`, and a task that has work it cannot step right now. `SessionModel` gains one narrow entry, `loadPinnedSession`. `LogbookManager` records record outcomes, which the result store reports where it already has the record open.

## Dependencies

- **Depends on:** Phase 1 (Executor and plot demand).
- **Blocks:** Phase 3 (Working indicator, hover detail and pending cells), Phase 4 (Documentation, acceptance map and audit).
- **Assumptions:**
  - **From the executor**, everything `01-executor-and-plot-demand.md` describes exists: `JobQueue::offer` / `withdrawChosenNext` / `chosenNextJob` / `publishingJob` / `kMaxRunningJobs`, `OfferResult`, and the lifecycle, pinning and end order.
  - **From `CalculationDemand`:**
    - `DemandCondition`, `DemandTrack`, `DemandState`, `plotState`, `plotStateChanged`, `statesChanged`, `buildToolTip`;
    - the private `PairKey` / `Memory` / `Candidate`, `plotCandidates()`, `offerChoice()`, `m_offeredJob`, `m_reconciling`;
    - the settle state `m_settleUntil`, the coalesced `recompute()` behind `m_updateTimer`, `m_relevantNames`, and the registry observer;
    - the test seams `flush`, `hasPendingUpdate`, `passCount`, `setInputSettleDelay`, `endInputSettleWaits`, `isSettling` and `hasSettlingSessions`.
  - **From the tests:**
    - `tests/tst_calculation_demand.cpp` with its fixture (s1–s4 "Jump 1"–"Jump 4", loaded and hidden, saved in an initialized logbook) and the helpers `row`, `settle`, `spin`, `restartDemand`, `stored`;
    - `PlotFixture` with 8 plots, and `waitDemandIdle`.
  - **Phase 1 decisions this phase relies on:**
    - memory keyed by `(session id, instance id)`, which survives model resets for ids that still have a row;
    - the `gestures` audit group, including the rule "exactly one `offer(` call site";
    - the "jobs never touch the idle scheduler" rule narrowed to `src/jobqueue.*`, `src/jobmodel.*` and `src/fusion`, so `calculationdemand.*` may register a scheduler task.
  - Build only `build-phase1/`. This phase adds no test source file, so no reconfigure is needed. Test with `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure`. **Never build `build/`.**

---

## Tasks

### Task 2.1: Entry points below the demand layer: `unregisterTask`, `canStep`, `ColumnFillTask`, `loadPinnedSession`, record outcomes

**Purpose:** Give the demand layer exactly what it needs from below: a task slot it can register and remove, a task that may have work it cannot step right now, a task id the progress line can label, one call that loads a hidden session the way showing it would and pins it under its corrected id, and the outcome of every stored result without opening a record.

**Files to modify:**
- `src/idlescheduler.h` / `.cpp`: `unregisterTask(TaskId)`; `registerTask` replaces an existing id; `TaskDef::canStep` and the waiting semantics of `tick()`.
- `src/logbookmanager.h` / `.cpp`: record reasons in memory and in `index.json`.
- `src/calculationresultstore.cpp`: reports a record's reason at write and at restore.
- `src/sessionmodel.h`: the `WorkerTask` enum value, the `loadPinnedSession` declaration and doc, the PINNED SESSIONS and CACHED COLUMN VALUES comment updates.
- `src/sessionmodel.cpp`: `loadPinnedSession`.

**Technical Approach:**

1. **`IdleScheduler`** (`idlescheduler.h:39`, `.cpp:15`). This change is generic and says nothing about jobs or calculations. The audit rule "the idle scheduler is never paused for a calculation" (`cleanup_audit.cmake:287`) must stay green: no `Fusion`, `JobQueue`, `calculations/` or `IsRunning` in these files.

   ```cpp
   /// Registers `def` under `id`, sorted by priority (a lower number runs
   /// first). An id that is registered already is replaced.
   void registerTask(TaskId id, const TaskDef &def);
   /// Removes the task registered under `id`; nothing when there is none. Its
   /// onComplete is not called. If it was the active task, the next tick
   /// reports the next task with work (activeTaskChanged) or goes idle
   /// (schedulerIdle).
   void unregisterTask(TaskId id);
   ```

   - `registerTask` first erases any entry with that id, then inserts it with `lower_bound` as today.
   - `unregisterTask` erases the entry. If `m_activeTask == id`, it sets `m_activeTask = -1` and calls `wake()`.
   - The demand layer needs `unregisterTask` because it is destroyed before the model (`MainWindow`), and tests create several demand layers over one model (`restartDemand()`). The task's lambdas capture the demand layer.

   **A task with work it cannot step right now.** `TaskDef` gains `BoolFn canStep = nullptr;` after `hasWork` (null means "always"). The semantics of `tick()` become:
   - the **active task** is the highest-priority task whose `hasWork()` is true, exactly as today; a switch emits `activeTaskChanged` and its progress;
   - the **step** goes to the highest-priority task whose `hasWork()` is true **and** whose `canStep` is null or true. It may be a lower-priority task than the active one; the active task's progress is what is reported after the step;
   - when a task has work but no task can step, the tick reports the active task's progress (so a `wake()` after an external change refreshes the line) and **does not re-arm the timer**. The scheduler is not idle: `schedulerIdle` is emitted only when no task has work. A later `wake()` ticks again;
   - `cancel()`, `onComplete` and `progress` are unchanged.

   This is generic: the scheduler learns nothing about jobs, demand or calculations (the audit rule of item 1 stays green; write "waiting on something outside the scheduler" in the comment). The four existing tasks have no `canStep` and behave exactly as before. Document the notion in the class comment of `idlescheduler.h`: "A task may have work it cannot step right now (it waits on something outside the scheduler): it is reported as active with its progress, is not stepped, and the scheduler rests until woken rather than spinning."


2. **`SessionModel::WorkerTask`** (`sessionmodel.h:180-185`). Add:

   ```cpp
   ColumnFillTask = 4   // priority 5 (lowest): registered by the demand layer
                        // (calculationdemand.h), not by the model - the column
                        // fill, whose steps are hidden loads
   ```

   - The enum is the one namespace of the scheduler's task ids. The progress line (`LogbookView`) already maps these ids to labels.
   - Add a line to the enum's comment: "The model registers tasks 0-3 in its constructor; a task registered by another component takes its id from this enum."

3. **`SessionModel::loadPinnedSession`**. Public; declare it after `isSessionPinned` (`sessionmodel.h:325`):

   ```cpp
   /// Loads the session of the row with this id the way showing it would -
   /// sessionRef(): the session-id correction, the engine attached, its
   /// stored results restored, sessionLoaded, the LRU and its eviction pass -
   /// without making the row visible, and pins it (pinSession()) under the
   /// id the row has after the load, before anything can evict it. Returns
   /// that id; the caller releases the pin with unpinSession(<returned id>).
   /// A row that is already loaded is pinned without a load.
   /// Returns an empty string and pins nothing when no row has `sessionId`,
   /// or when the session file cannot be loaded (the row is then a
   /// failed-load placeholder, as after any failed load; eviction turns it
   /// back into a stub). Emits what sessionRef() emits. Must not be called
   /// while a RowStabilityGuard is held.
   QString loadPinnedSession(const QString &sessionId);
   ```

   **Implementation** (`sessionmodel.cpp`, next to `pinSession`, `:1744`):
   1. `const int row = getSessionRow(sessionId); if (row < 0) return {};`
   2. `assertRowsMutable("SessionModel::loadPinnedSession");`
   3. `sessionRef(row);`
   4. `const SessionRow &sr = m_rows[row];` If `sr.loadFailed`, return `{}`.
   5. Otherwise: `const QString id = sr.sessionId; pinSession(id); return id;`

   **Verified from the code:**
   - `sessionRef(row)` on a non-visible stub (`sessionmodel.cpp:1073-1123`) performs the real load:
     - `loadSession`;
     - the id correction through `setRowSessionId` (`:1084`), which also moves the manager's known records with the id;
     - `attachSession`;
     - `restoreStoredResults`;
     - `sessionLoaded`;
     - `lruTouch`;
     - `evictIfNeeded(keep = sr.sessionId)`.
   - It never touches `visible`.
   - The just-loaded session cannot be evicted before the pin. `sessionLoaded` is emitted before the session enters the LRU list, and `evictIfNeeded` keeps it. `pinSession` follows with no return to the event loop.
   - The row index is re-read after `sessionRef()`, exactly as `sessionRef()` itself returns a reference after its emission.
   - No new `restoreSession(` call site is created. The stored-results audit rules "two restore call sites" and "stored results are restored at a load or by the column worker only" stay satisfied.

4. **Record outcomes in the logbook index.** A stored result of an explicit calculation is always status `Ok`; a rejection or solver failure is an `Ok` result whose outputs are unavailable and whose `reason` is not empty (`CalculationRecord`, field 6, "reason / detail"). The demand layer must know that reason for a session that is not loaded without opening the record. The index learns it at the two moments the record is open anyway.
   - `LogbookManager` keeps `QMap<QString, QMap<QString, QString>> m_recordReasons` (session id -> calculation id -> reason, non-empty reasons only), next to `m_knownRecords`, with the same keying and the same remap in `remapSessionId()`.
   - Public API, next to `knownCalculationRecords()`:

     ```cpp
     /// The reason the stored result of (session, calculation) did not produce
     /// its outputs (a rejection, a solver failure), as this index learned it
     /// when the record was written or last restored; empty when the result
     /// produced its outputs, when the record's outcome has not been learned
     /// yet (a record of an earlier build, until its next restore), and when
     /// there is no record. Never opens a record.
     QString calculationRecordReason(const QString &sessionId, const QString &calculationId) const;
     /// Records what a record holds. Called by the result store with the
     /// record it has just written or just read. Emits nothing; marks the
     /// index for a flush when the value changed.
     void setCalculationRecordReason(const QString &sessionId, const QString &calculationId, const QString &reason);
     ```

   - `writeCalculationRecord()` sets the reason from the record it writes (a replacement record replaces the reason). `removeCalculationRecord()`, `removeCalculationRecords()` and `removeSession()` erase it. `initialize()` reads it from `index.json` and keeps it only for ids whose record file the names-only listing found; the stray pass needs nothing (it deletes files whose session has no entry).
   - `index.json`: each session entry gains `"recordReasons"`, an object `{"<calculation id>": "<reason>"}` holding the non-empty reasons; omitted when empty. It is additive: an index without it reads as "no reason learned yet". `flushIndex()` writes it beside `"records"`; `"records"` itself (the stamp of the cached values) is unchanged.
   - `CalculationResultStore` calls `setCalculationRecordReason()` in `onExplicitResultEvent()` right after a successful `writeCalculationRecord()` (from the snapshot's reason), and in `restoreSession()` for every record it **read and did not delete** (restored, kept as `AlreadyInstalled`, or refused for a reason that is not staleness), from `record.result`'s reason. A skipped (unreadable) record sets nothing. A deleted record's reason goes with the record.
   - This changes no cache-validity rule and no record: `CalculationCompatibilityVersion` is not bumped. The column worker's code is untouched: its temporary copy's restore goes through `restoreSession()`, which is where the store reports.

5. **Comments** (`sessionmodel.h`):
   - **PINNED SESSIONS** (`:142-150`): replace the second sentence with: "The executor pins the session of its running job and of its chosen next job. The demand layer pins each session it loads for column demand (`loadPinnedSession()`) until that session has no column demand left. Pins are counted per session id." Keep the remainder, including "The model knows pinned ids only; it knows nothing about jobs."
   - **CACHED COLUMN VALUES**: after "...and reads unavailable when the calculation is not requested." (`:83`), add: "While such a column is enabled, the demand layer (`calculationdemand.h`) has the requested calculations it needs computed for every session, loading unloaded ones as hidden sessions. The column worker knows nothing of it: a record written by a job drops the values over it, and the paths below compute them again."
   - Leave the text "it never requests, prepares or runs a requested calculation" (`:98-99`) as it is: it remains true.

**Acceptance Criteria:**
- [ ] `IdleScheduler::unregisterTask` exists. Unregistering the active task leads to `activeTaskChanged` for the next task with work, or to `schedulerIdle`, within one tick. Re-registering an id leaves exactly one entry. Tested in 2.7.
- [ ] A task registered with a `canStep` that returns false is reported active with its progress on `wake()`, is never stepped, does not make the scheduler spin (no further tick until the next `wake()`), and does not make it idle; a lower-priority task with work is still stepped meanwhile. Tested in 2.7 (`schedulerWaitingTaskDoesNotSpin`).
- [ ] `SessionModel::ColumnFillTask == 4`. No code in `SessionModel` registers it.
- [ ] `LogbookManager::calculationRecordReason()` returns the reason after a write of a record with a reason, after a restore of one, after a restart from an index that holds it, and `""` after the record is removed, for a record that produced its outputs, and for a record no restore has read since the index lacked the field. `index.json` round-trips `"recordReasons"` (tested in 2.7).
- [ ] `loadPinnedSession(stubId)`:
  - loads the row without changing `visible` or its `Qt::CheckStateRole`;
  - emits `sessionLoaded` once;
  - restores the stored results (`storedResultStats().restoreCalls` +1);
  - returns the row's id after the load and pins exactly that id.
- [ ] A row whose file cannot be loaded returns `""`, becomes a failed-load placeholder, and is not pinned.
- [ ] `grep -c "m_resultStore.restoreSession(" src/sessionmodel.cpp` is still 2.

**Complexity:** M

---

### Task 2.2: Column demand model in `CalculationDemand`: columns, cell classification, settlements, memos, per-column and per-cell state

**Purpose:** Derive, for every enabled logbook column over a requested output and every session in the logbook, whether the cell's requested calculations are done, waiting, running, failed or not applicable, cheaply enough for thousands of sessions. Publish it as a `DemandState` per column and a pending query per cell.

**Files to modify:**
- `src/calculationdemand.h` / `.cpp`

**Technical Approach:**

1. **Public API additions** (keep Phase 1's naming):

   ```cpp
   /// Sessions the demand layer holds loaded for column demand at most:
   /// the running job's and the chosen next job's (spec 9, 11).
   static constexpr int kMaxHeldSessions = JobQueue::kMaxRunningJobs + 1;

   /// A logbook column's id: logbookColumnDefinitionKey(column). Unique
   /// among the columns (LogbookColumnStore collapses equal definitions).
   static QString columnId(const LogbookColumn &column);
   /// Default for a column that is not enabled, not requested or unknown.
   DemandState columnState(const QString &columnId) const;
   /// True while the requested calculations the cell needs are waiting or
   /// running (the cell is in demand). Never true for a column that is not
   /// requested.
   bool isCellPending(const QString &sessionId, const QString &columnId) const;
   /// The same by SessionModel row and column index; false out of range.
   bool isCellPending(int row, int column) const;

   // test seams (in addition to Phase 1's)
   QStringList heldSessionIds() const;   ///< sessions pinned by the load step, in load order
   bool hasFillWork() const;             ///< the load step's hasWork
   void runLoadStep();                   ///< the load step's step, callable directly
   int  recordSetLookups() const;        ///< LogbookManager::knownCalculationRecords() calls so far

   signals:
   /// columnState(columnId) or the column's set of pending cells differs
   /// from what it was. Emitted per column, before statesChanged().
   void columnStateChanged(const QString &columnId);
   ```

   - `isCellPending(int row, int column)`:
     - bounds-check against `m_sessionModel->rowCount()` and `columnCount()`;
     - take `rowAt(row).sessionId` and `columnId(m_sessionModel->column(column))`, **computed on every call** so a stale mapping can never answer;
     - look up the pending set with `constFind`.

     Phase 3 calls it from paint, so it must be O(1) apart from building the key string.
   - **`DemandState` for a column:**
     - `sourceId` = column id;
     - `requested` = E(c) is not empty (below);
     - counts, `running`, `failed`, `progressLabel` and `toolTip` exactly as Phase 1 item 5, over **every session row** in row order;
     - **`waiting` stays empty** for a column state (it could hold thousands of entries); `waitingCount` carries the number. State this in the struct comment of `waiting`: "plot states only; a column state counts its waiting tracks without listing them".

2. **Column bookkeeping** (private):

   ```cpp
   struct ColumnInfo {
       QString id;                          // columnId()
       QList<DependencyKey> names;          // logbookColumnNames(): what inspection reads
       QStringList calculations;            // E(c) = logbookColumnExplicitCalculations(col, registry)
       QStringList storable;                // E(c) without explicit family instances (ids containing '#')
   };
   QVector<ColumnInfo> m_columns;           // requested enabled columns, in SessionModel column order
   bool m_columnsDirty = true;
   void syncColumns();
   ```

   - `syncColumns()` reads `m_sessionModel->columnCount()` and `column(i)`, keeps the columns whose E(c) is not empty, and compares the id list with the previous one. If it changed:
     - clear the loaded-report memo;
     - erase settlements of columns no longer present;
     - drop the states of columns that vanished (announce each once through `applyStates`, as Phase 1 does for dropped plots);
     - set `m_relevantNamesDirty`.
   - `m_columnsDirty` is set by `SessionModel::modelReset` (`rebuildColumns()` resets the model, `sessionmodel.cpp:160`) and by the registry observer. E(c) follows the registrations.
   - Use `logbookColumnExplicitCalculations` (`logbookcolumn.h:101`), the same authority the column cache uses. Do not test `EvaluationPolicy::Explicit`: the audit rule "one authority: explicit-backed" forbids it outside the engine.
   - **Relevant names** (Phase 1 `rebuildRelevantNames`): also unite `registry.staticDependencies(name).names` for every `name` of every `ColumnInfo`. A relevant input change on a loaded session then starts its settle wait and clears its memory (Phase 1 step 5), and also its settlements and report memo (item 6).

3. **Loaded sessions: one combined report per cell.** For a row with `isLoaded() && !loadFailed`, inspect `engine.blockers(name)` for each name of the column (1 name, or 2 for `Delta`) and combine:
   - any `NotApplicable` → `NotApplicable` (a value that needs both names can never exist);
   - else any `NotProduced` → `NotProduced` (union of the notes);
   - else any `Blocked` → `Blocked` (union of the blockers, deduplicated by instance id, in name order, then report order);
   - else `Available`.

   Classify the combined report with **Phase 1's track table** (`Available` → Done, ..., `Blocked` with the running key → Running, job-failure memory → Failed with `jobFailure`, all blockers in not-applicable memory → NotApplicable, otherwise Waiting with `settling`). `sessionName` is the live `_DESCRIPTION`, else the id.

4. **Sessions that are not loaded, and failed-load placeholders.** An engine that holds no stored result is never asked, and **no record is opened**. For the row's current id `s` and a column, decide in this order:
   1. `storable` is empty → `NotApplicable`. Explicit family instances are never stored (`CalculationResultStore`), so the column of an unloaded session can never hold them.
   2. A settlement `m_settled[(s, column id)]` exists → its condition, name, titles, reason and `jobFailure`.
   3. If **every** id of `storable` is in `recordSet(s)`: some record carries a reason (`recordReason(s, id)` non-empty, from the memo of item 7) → `Failed` with `jobFailure = false`, `reason` = `"<title>: <reason>"` of the first such id in `storable` order (titles from the registry's descriptor), `titles` = the ids' titles, name = `rowDisplayName(sr)`; otherwise → `Done`. "A known record counts as a result", per pair: a column over a chain of requested calculations (its `storable` holds the upstream and the downstream id) has a result only when both records exist. Note that this is deliberately stricter than `settleExplicitColumns` (`sessionmodel.cpp:1595`), whose "any record" test decides only whether the column worker restores into its copy; that worker then computes the value it can, which for a missing downstream record is "unavailable", and the demand layer is what gets the downstream link computed.
   4. Some id of `storable` has `Memory::JobFailed` for `(s, id)` → `Failed` with `jobFailure`, reason from memory, name = `rowDisplayName(sr)` (item 4a).
   5. Every id of `storable` has `Memory::NotApplicable` → `NotApplicable`.
   6. **The row is a failed-load placeholder** (`sr.isLoaded() && sr.loadFailed`, visible or hidden) → record the settlement `m_settled[(s, column id)] = {Failed, rowDisplayName(sr), {}, tr("The session file could not be loaded"), /*jobFailure*/ true}` and use it. Such a pair can never be computed in this run: the load step takes only stubs, and nothing retries a failed visible load. Without this rule the cell would stay pending and the column would keep working for the whole run. Recording in the walk writes only the demand layer's own state, which the guard allows.
   7. Otherwise `Waiting`, with `settling = isSettling(s)`. This is the load step's work (Task 2.3).

   `recordSet(s)` is served from a memo (item 7), and `LogbookManager::knownCalculationRecords(s)` is called only on a miss (`++m_recordSetLookups`). A **failed-load placeholder** is classified by this path too: its engine holds no restore. By rule 6 it is never `Waiting`, so it is never pending and never a load candidate.

   **(4a) Display names of rows whose session is not loaded or failed to load** (spec §10: the user sees sessions named the way the logbook names them). The private helper `QString rowDisplayName(const SessionRow &sr) const` returns, in this order:
   1. For a loaded, non-placeholder row: the live `_DESCRIPTION`. This is what Phase 1 already does.
   2. Otherwise, the logbook's cached description: `LogbookManager::instance().cachedValuesForSession(sr.sessionId).value(logbookColumnDefinitionKey(<SessionAttribute column over SessionKeys::Description>))`, converted with `LogbookManager::jsonToVariant(...).toString()`.
      - This is the value index.json holds for the session and that `SessionModel::data()` shows for a stub row in the description column (`sessionmodel.cpp:403-407`).
      - It is kept for as long as the row is a stub. A failed-load placeholder keeps it too: `sessionRef()` does not clear the manager's values.
      - Build the key string once, as a static.
      - The lookup is one `QMap` lookup. It runs only for tracks that are listed (`running` and `failed`) and when a settlement is recorded, never for the thousands of counted-only waiting cells.
   3. The session id, as a last resort, when that value is missing or empty.

   There is no separate file-name fallback. `LogbookManager` exposes no public accessor for a session's file stem (`recordStem()` is private), and for the only rows known by their file name (identity stubs) the id already *is* the file stem. Every rule that sets a track's `sessionName` for a row that is not loaded (4.2 through a settlement, 4.4, 4.6, and the load step's failure path) uses `rowDisplayName`.

5. **Settlements** (a column-level memory; see Decisions Made):

   ```cpp
   using CellKey = QPair<QString, QString>;              // (session id, column id)
   struct Settlement { DemandCondition condition; QString sessionName; QStringList titles;
                       QString reason; bool jobFailure = false; };
   QHash<CellKey, Settlement> m_settled;
   ```

   - **Recording:** in every pass, a loaded, non-placeholder session's cell with a final condition (`Done`, `Failed`, `NotApplicable`) records its settlement; a non-final one (`Waiting`, `Running`) erases it. A failed-load placeholder, visible or hidden, records a `Failed` settlement ("The session file could not be loaded", `jobFailure`) by item 4.6. The load step records the same settlements at once for a load that fails (Task 2.3), so its task stops having work before the next pass. Both paths give the same settlement, and the clearing rules below apply to it. After a clearing (a bulk edit, a record change, an input change), a row that is still a placeholder is settled again in the next pass without any load. A row that has become a stub is in demand again and is loaded once more.
   - **Purpose:** after the session is evicted, the unloaded rule sees the last final verdict instead of "no record, so in demand". Without it, a session whose calculation does not apply would be loaded again in the same run, and a thrown exception (a Failed-status result, which is not stored), a failed record write or a job-level failure would be retried, indefinitely.
   - **Clearing:**
     - `(s, *)` on a relevant input change of `s` (the Phase 1 input-change path);
     - `(s, c)` for every column whose `calculations` contain `id` on `calculationRecordsChanged(s, id)`: a write, a deletion (including the column worker's stale deletion) or a skip changes what the record set says;
     - `(s, *)` on a single-row display change of `s` while `s` is not loaded (item 6), which also drops `m_recordSets[s]` and `m_recordReasons[s]` so a reason the column worker's restore just learned is read;
     - `(*, c)` when column `c` leaves the enabled requested set;
     - everything on a registry change;
     - `(s, *)` on `SessionModel::modelReset` when `s` has no row any more (like Phase 1's memory: `sort()` resets the model and must not forget settlements).

6. **Triggers** added to Phase 1's table (`01-...md` Task 1.3 item 4):

   | Signal | Reaction |
   |---|---|
   | `SessionModel::modelReset` (in addition to Phase 1's) | `m_columnsDirty = true`; clear the loaded-report memo and the record-set memo; erase the settlements of ids without a row; schedule |
   | `LogbookManager::calculationRecordsChanged(s, id)` (direct connection; the slot only drops state and schedules, as `SessionModel::onCalculationRecordsChanged` does, because it runs inside record methods) | erase `m_recordSets[s]`, the loaded-report memo of `s`, and the settlements of `s` over columns whose `calculations` contain `id`; schedule |
   | `SessionModel::dataChanged(tl, br, roles)` with `tl.row() == br.row()` and (`roles` empty or containing `Qt::DisplayRole`) | `s = rowAt(tl.row()).sessionId`; erase the loaded-report memo of `s` (no pass is scheduled for that alone); if the row is not loaded or is a placeholder and `s` has settlements, erase them and schedule |
   | `JobQueue::jobFinished(id, state)` (in addition to Phase 1's) | before Phase 1's handling: erase the loaded-report memo of the record's session |
   | registry observer (in addition to Phase 1's) | `m_columnsDirty = true`; clear settlements, the loaded-report memo and the column states' memos |
   | `SessionModel::dependencyChanged(s, key)`, input change (Phase 1 step 5) | also erase settlements and the loaded-report memo of `s` |
   | `SessionModel::dependencyChanged(s, key)`, publication | also erase the loaded-report memo of `s` |

   Why the single-row display rule: the bulk edit (`processNextBulkEdit`, `sessionmodel.cpp:2332`, both the stub and the loaded path) emits no `dependencyChanged`, and neither does the column worker (`:2115`, `:2157`), but both announce a changed row this way. Multi-row changes (`setRowsVisibility`: CheckState only; the unit system and the environment check: all rows) are not input changes of a session, and the registry observer covers the environment. Hover changes carry `BackgroundRole` / `IsHoveredRole` only.

7. **Memos**, which make a pass cheap. They are private, and each has exact invalidation:
   - **`QHash<QString, QSet<QString>> m_recordSets`** and, beside it, `QHash<QString, QHash<QString, QString>> m_recordReasons`: a mirror of `knownCalculationRecords(s)` and of `calculationRecordReason(s, id)` for each id of it, for rows that are not loaded, filled together on a miss. Every change of the manager's known set emits `calculationRecordsChanged`, except `initialize()`, `removeSession()` and the stray pass, which come with a model repopulation or `removeSessions()` (a model reset clears the memo). A reason changes only with a write, a removal or a restore of that record; a write and a removal emit `calculationRecordsChanged`, and a restore into the column worker's copy that learns a reason for a record the index did not know yet is followed by that worker's single-row `dataChanged`, which item 6 handles for an unloaded row. A second pass with nothing changed makes **zero** manager lookups.
   - **`QHash<QString, QVector<BlockerReport>> m_columnReports`**: combined reports per loaded session, parallel to `m_columns`. Engine state changes reach the demand layer through:
     - `dependencyChanged` (edits, merges, publications, broadcast invalidations);
     - `sessionLoaded` (the restore at load): erase `s` in that slot too;
     - the single-row display change (the bulk edit's loaded path, which publishes nothing);
     - `calculationRecordsChanged`;
     - `jobFinished`.

     A pass that finds `s` not loaded erases its entry. Model reset, column change and registry change clear it.

8. **Per-column tallies and pending cells.** The walk (Task 2.3 item 1) accumulates, per `ColumnInfo`:
   - the counts;
   - `running` and `failed` tracks (`DemandTrack` with `job` / `progressText` for the running one, taken from the executor's record as Phase 1 does);
   - a `QSet<QString>` of session ids whose cell is `Waiting` or `Running`.

   The states are built after the offers, stored before any emission, and compared with the previous ones. Extend Phase 1's `applyStates`:
   - emit `columnStateChanged(id)` for each column whose `DemandState` **or** pending set differs;
   - emit `plotStateChanged` as before;
   - then `statesChanged()` once.

   Phase 1's "progress without inspection" (`onJobProgress`) also updates `progressText` / `toolTip` of column states whose `running` track has that job, and emits `columnStateChanged` for them.

9. **Class comment.** Extend Phase 1's sections:
   - **WHAT IS WANTED:** add column demand: requested enabled columns × every session row.
   - **WHERE A RESULT IS LOOKED UP:** loaded: blocker inspection of the column's names; not loaded: the manager's record set and the reasons the index recorded for those records, never a record opened; a cell has a result only when every calculation it needs has a record; a record with a reason is a failed result with that reason, after a restart as before it; a known record counts until the column worker's restore finds it stale and deletes it, and that record change moves the cell into demand.
   - **SETTLEMENTS:** why they exist and what clears them.
   - **HIDDEN LOADS** (Task 2.3).
   - **WHAT A PASS COSTS:**
     - O(rows) plus O(rows × requested columns) hash lookups;
     - one manager lookup per unloaded session between changes to its records;
     - blocker inspection only for loaded sessions whose memo was dropped;
     - no walk at all when no enabled column is requested.

   Name no widget class. You may now name the scheduler task (the audit rule was narrowed in Phase 1), but still write neither `QThread` nor `QEventLoop`.

**Acceptance Criteria:**
- [ ] `columnState()` of a non-requested, disabled or unknown column is `DemandState()`, and such a column is never announced.
- [ ] With no requested column enabled, a pass performs no row walk and no record-set lookup (`recordSetLookups()` stays 0).
- [ ] A session that is not loaded is never loaded, read or LRU-touched by a pass. No record is read by a pass (`storedResultStats().recordsRead` unchanged across `flush()`).
- [ ] The unloaded decision order is exactly items 4.1-4.7. A cell whose column needs two requested calculations reads Done only with both records, and Failed with the recorded reason when a record carries one. A failed-load placeholder is classified as not loaded, and a placeholder, whether visible or hidden, is settled `Failed` with `jobFailure` and the reason "The session file could not be loaded". It is never `Waiting`, never pending, never a load candidate, and it appears in the column state's `failed` list, so `showsWarning()` once other work is done.
- [ ] Every listed track (`running`, `failed`) of a row that is not loaded or failed to load has `sessionName` = `rowDisplayName()` (item 4a): the logbook's cached description when one exists, and the session id only when none does.
- [ ] A settlement is recorded for every final cell of a loaded session, erased when the cell is non-final, and cleared by exactly the triggers of item 5.
- [ ] `isCellPending` is true exactly for `Waiting` and `Running` cells of requested columns, and false out of range.
- [ ] A second `flush()` after an unrelated trigger makes zero `knownCalculationRecords` lookups. A `calculationRecordsChanged(s, …)` makes exactly one more.

**Complexity:** L

---

### Task 2.3: Tier (c), the load step, holds and the bound

**Purpose:** Make column demand produce work. After plot demand, visible sessions are offered first, then the other loaded sessions, each in row order. Sessions that are not loaded are loaded by the lowest-priority scheduler task, the column fill, at most `kMaxHeldSessions` at a time, pinned, and released when their column demand is gone. The fill task reports the whole fill's progress and never makes the scheduler spin.

**Files to modify:**
- `src/calculationdemand.h` / `.cpp`

**Technical Approach:**

1. **The pass.** This extends `recompute()` from Phase 1 Task 1.3 item 2. There is one function, run by every path:
   1. As Phase 1: stop the timer, `++m_passCount`, `m_reconciling = true`.
   2. Drop expired settle entries.
   3. `syncCheckedSet()`, `inspectedPlots()`; `syncColumns()` if `m_columnsDirty`.
   4. **If inert:** default states for plots and columns, release every hold (item 5), clear `m_loadCandidates`, and go to step 9.
   5. **Plot inspections**, as Phase 1. The early exit "no inspected plots" becomes "skip the plot inspection only".
   6. **Column walk**, if `m_columns` is not empty. It runs under **one** `RowStabilityGuard` and **reads only**: it may write the demand layer's own memos and settlements, but it emits, offers and pins nothing. For `row = 0 .. rowCount()-1`, with `sr = rowAt(row)` and `s = sr.sessionId`:
      - If `sr.isLoaded() && !sr.loadFailed`:
        - classify each column's cell from the report memo, inspecting on a miss (Task 2.2 item 3);
        - record or erase settlements;
        - for every `Waiting` cell that is not settling, append the report's blockers to `columnCandidates` in column order, then blocker order. Skip keys in memory, the running key (not asked to stop), and keys already present in any tier (dedupe on `PairKey`).
      - Otherwise:
        - classify by Task 2.2 item 4;
        - if some cell is `Waiting`, the row is **not visible** (a visible stub belongs to the visible loader, priority 2), the session is not settling and not held, append `s` to `loadCandidates` until it has `kMaxHeldSessions` entries, and `++unloadedWaiting`.
      - Count the row in `fillRemaining` when any of its cells is `Waiting` or `Running` (loaded or not).
      - Accumulate the tallies (Task 2.2 item 8).
   7. **Release holds** (item 5), outside the guard.
   8. If `!m_queue->isShutDown()`: `offerChoice(plotCandidates(...) + columnCandidates)`. Phase 1's `offerChoice` is unchanged: a column candidate is always a loaded session, so it never gets `SessionNotLoaded`. Phase 1's withdraw rule now reads "if no tier has an accepted candidate and the chosen next job is `m_offeredJob`: withdraw".
   9. Build the plot and column states; `applyStates`.
      - `m_loadCandidates = shutDown ? {} : loadCandidates`.
      - `m_fillRemaining = fillRemaining`; `m_fillHighWater = qMax(m_fillHighWater, m_fillRemaining)`; when `m_fillRemaining` is 0, reset `m_fillHighWater = 0` (the fill is over; the next one starts its own count). This is the pattern of `m_columnWorkerRemaining` / `m_columnWorkerHighWater`.
      - Always `m_sessionModel->scheduler().wake()` when the fill's numbers or the load candidates changed: the scheduler reports the active task's progress on the tick even when nothing can step.

   Classification happens before the offers. An offer never starts a job synchronously, so `Running` cannot change in between. Only a waiting track's `job` field could, and column states do not list waiting tracks.

2. **Priority (tier (c)).** The final candidate list is:
   - (a) plot demand of the focused session;
   - (b) plot demand of the other visible sessions in row order;
   - (c) column demand of every **loaded** session (not a placeholder): first the **visible** ones in logbook row order, then the **hidden** ones (the pool, and sessions the fill has loaded) in logbook row order. What the user is looking at is computed before what only the logbook shows.

   A session that is not loaded enters tier (c) once the fill has loaded it. The fill loads in row order. When it loads a session earlier in row order than the current chosen next job's, the next pass offers that session and the executor replaces the chosen next job (`Cancelled`, "No longer needed"). This is Phase 1's contract, and it keeps the choice "as demand is at that moment".

3. **The column fill** is a scheduler task registered in the constructor when the collaborators are present. Its steps are the hidden loads; its work is the whole fill:

   ```cpp
   m_sessionModel->scheduler().registerTask(SessionModel::ColumnFillTask, TaskDef{
       /*priority*/    5,                        // below save (1), load (2), bulk edit (3), column work (4)
       /*step*/        [this] { runLoadStep(); },
       /*hasWork*/     [this] { return hasFillWork(); },
       /*progress*/    [this] { return Progress{m_fillRemaining, m_fillHighWater}; },
       /*onComplete*/  [](bool) {},
       /*cancellable*/ false,
       /*canStep*/     [this] { return canLoad(); } });
   ```

   - **`hasFillWork()`** = `!isInert() && !m_queue->isShutDown() && m_fillRemaining > 0`: some session has a column cell `Waiting` or `Running`. The task is therefore active, and the progress line shows it, from the first pass that finds column demand to the pass that finds none.
   - **`canLoad()`** = `hasFillWork() && m_held.size() < kMaxHeldSessions && !m_loadCandidates.isEmpty()`. Both are O(1): the scheduler calls them on every tick. While the fill waits on a job with both holds taken, `canLoad()` is false and the scheduler rests (Task 2.1): no tick, no spinning, the line still showing "Computing columns: k / n".
   - **`runLoadStep()`**:
     1. If `hasPendingUpdate()`, run `recompute()` first, so the candidates reflect everything up to now.
     2. If `!canLoad()`, return.
     3. `const QString id = m_loadCandidates.takeFirst();`
     4. Re-validate with one `getSessionRow(id)`. If there is no row, or the row is loaded and not a placeholder, or it is visible (the column worker or `resolveIdentityStubs()` may have remapped the id silently, or a slot loaded it), run `recompute()` and return. The next tick tries again.
     5. `const QString held = m_sessionModel->loadPinnedSession(id);`
     6. **If `held` is empty** (the load failed): for every `ColumnInfo`, set `m_settled[(id, col.id)] = {Failed, rowDisplayName(m_sessionModel->rowAt(row)), {}, tr("The session file could not be loaded"), /*jobFailure*/ true}`, then `scheduleUpdate()`. The placeholder is not held. It stays in the pool and is evicted as usual; the settlement keeps it from being loaded again in this run.
     7. **Otherwise:**
        - `m_held.append(held)`;
        - if `held != id` (**id correction**), erase `m_recordSets[id]`, `m_columnReports[id]` and the settlements of `id`: the row is known by `held` from now on, and the manager moved its records to `held`;
        - `scheduleUpdate()`. `sessionLoaded` already scheduled a pass; the call is harmless.

   - The pass that follows sees the row loaded under `held`, inspects it, and offers `(held, blocker)` in tier (c). **The session-id correction therefore always takes place before the pair is offered**: an unloaded session is never offered, and every key the executor sees is the row's corrected id.
   - **Progress line:** remaining = sessions with a `Waiting` or `Running` column cell, loaded or not; total = the fill's high-water mark. The task is active for the whole fill, so the line shows "Computing columns: k / n" from the first load to the last result, exactly as on-demand column work is shown, with `k` rising as sessions finish. Every pass that changes the numbers wakes the scheduler, whose tick reports the active task's progress even when it cannot step.
   - **Not cancellable** (see Decisions Made). `onComplete` is empty. If `IdleScheduler::cancel(ColumnFillTask)` is called anyway, nothing changes and the task has work again on the next tick while demand remains.

4. **Holds** (`QStringList m_held`, in load order). A hold is the demand layer's own pin, in addition to the executor's pins of the running and chosen next jobs' sessions. Pins are counted, so both coexist.
   - A hold exists from the moment `loadPinnedSession` returns. Pinning at the choice would be meaningless for a stub (there is nothing to evict yet) and wrong across the id correction.
   - Sessions that were **already loaded** (visible, focused, or in the hidden pool) are never held. They are offered directly, the executor pins them while their job is chosen or running, and they do not count toward the bound.

5. **Release** (`releaseHolds()`, step 7 of the pass, outside any guard). A hold of `h` is released (`unpinSession(h)`, removed from `m_held`) when:
   - no row has id `h` (removed or repopulated); or
   - the row is not loaded (repopulated as a stub); or
   - **no cell of `h` is `Waiting` or `Running`**: its last job has ended and its result is published, or it was settled (done / failed / not applicable), or its column was disabled, or its result appeared by other means.

   The rule covers chained requested calculations: the hold lasts across links (`expA` → `expB`), because the cell stays `Waiting` between them. The executor's own pin keeps a running job's session loaded after the hold's release (for example after the column is disabled mid-job). `unpinSession` never evicts synchronously. It queues one eviction pass, after which the session leaves the pool by ordinary LRU eviction, or stays in it when the capacity allows.

   Also:
   - **when inert**, release all holds;
   - **in the destructor**, if the model is alive: `unregisterTask(SessionModel::ColumnFillTask)`, then unpin every hold.

6. **Edge cases.** Each follows from the rules above; state each in the class comment's HIDDEN LOADS section:
   - **Pool capacity (`LogbookCacheSize`) below the bound.** Pinned rows are passed over by eviction (PINNED SESSIONS), so the pool may exceed its capacity by at most the holds. On release, the queued eviction pass evicts them. Capacity 0 works.
   - **Column disabled mid-load.** The load itself is atomic within a step. The next pass finds no demand for the held session and releases it. If its pair was the chosen next job, Phase 1's withdraw rule ends it `Cancelled` "No longer needed". A running job finishes and is stored.
   - **Session shown while held.** Nothing changes for the hold. Plot demand may move the session into tier (b). After release, the visible row is not in the LRU list and stays loaded.
   - **Logbook closed or repopulated** (`populateFromIndex` / `populateFromUuids`), and `removeSessions()`: a model reset, then release by rule 5. Phase 1 forgets the memory of ids without a row, and Task 2.2 forgets their settlements.
   - **Not applicable after load.** The loaded cell classifies `NotApplicable`, gets a settlement and is released without a job. The column worker had already cached the value as unavailable (no record) and it stays so.
   - **Executor shut down.** `hasFillWork()` is false: nothing more is loaded and the line goes idle.

7. **Why saves and bulk edits come first (spec §9, §13).**
   - The scheduler steps, on each tick, the highest-priority task that has work and can step (`idlescheduler.cpp:49-58`, amended in Task 2.1). The fill's load (5) never runs while save (1), visible load (2), bulk edit (3) or column work (4) has work; while any of them has work it is the active task and the line shows it, and the fill returns to the line when it finishes.
   - A stub is never dirty: dirty rows are loaded. The save task therefore writes only loaded rows, from memory, and the load step loads only stubs.
   - A bulk edit on a stub finishes (edit, save, index) before any load step. The session the load step reads is the rewritten file.
   - A bulk edit on a session the load step already loaded takes the bulk edit's loaded path. It edits and saves that very session. The engine drops any result computed from the old value, and a running job over it goes stale (`Superseded`) and is offered again.
   - No result is therefore computed from a file that is about to be rewritten. The demand layer adds no check of its own.

**Acceptance Criteria:**
- [ ] At any moment `heldSessionIds().size() <= kMaxHeldSessions` (== 2), and every held id is pinned in `SessionModel`.
- [ ] The fill's load never runs while save, load, bulk edit or column work has work (the scheduler's `activeTaskChanged` order in 2.6).
- [ ] The fill task is active, with `progressChanged(ColumnFillTask, remaining, total)` where remaining counts sessions with a waiting or running cell, from the first pass with column demand until the last result, and the scheduler ticks no more than once per `wake()` while both holds are taken (`fillTaskReportsProgressWhileWaiting`).
- [ ] Within tier (c), visible sessions are offered before hidden loaded ones (`visibleSessionsFirstWithinColumnDemand`).
- [ ] An identity stub known by its file stem is loaded, corrected, held and offered only under its real id. Nothing stays pinned under the stem.
- [ ] A session that failed to load, turned out not applicable, threw, or had a job-level failure is not loaded again in the run: `sessionLoaded` count stable across `spin()` ×3 and `hasFillWork()` false.
- [ ] A hold is released exactly when rule 5 says, and never while the session still has a `Waiting` or `Running` cell (chains).
- [ ] The fill task is registered with priority 5, id `SessionModel::ColumnFillTask`, not cancellable, with `canStep`. It is unregistered, and every hold is unpinned, when the demand layer is destroyed.
- [ ] `offer(` still appears on exactly one source line in `src/` (audit `gestures`).

**Complexity:** L

---

### Task 2.4: Wiring and comments: progress-line label, `MainWindow`, build comment

**Purpose:** Show the new task in the logbook's progress line, and make the start-up and teardown comments true again now that enabled columns create work.

**Files to modify:**
- `src/ui/docks/logbook/LogbookView.cpp` (`onProgressChanged`, `:283-293`): add `case SessionModel::ColumnFillTask: label = tr("Computing columns: %v / %m"); break;`, the same text as `ColumnTask`'s, on purpose: to the user a column fills in the background the same way whether its values are cheap or requested (Michael's decision at the plan review). The cancel button follows `cancellable` (false), so it stays hidden for this task, while it shows for the cheap column pass as today. `LogbookDockFeature.cpp:36-49` is unchanged.
- `src/mainwindow.cpp` (the construction comment Phase 1 wrote at `:205-213`): replace "every session starts hidden, so starting the application starts no job; work starts when a session is shown" with: "Every session starts hidden, so checked plots start no job at start-up. An enabled logbook column over a requested calculation does create demand at once. Its hidden loads are the idle scheduler's lowest-priority task, so they wait for the column worker's start-up pass." In the destructor comment, add that the demand layer is deleted before the model it registered a scheduler task with (the order is already so: `m_calculationDemand` first, and the model is a child created earlier).
- `src/CMakeLists.txt` (the comment Phase 1 wrote above `calculationdemand.cpp`): "the demand layer: what requested calculations the checked plots need for the visible sessions and the enabled logbook columns for every session, which runs next, hidden loads for column demand, and the per-plot and per-column state".
- `src/calculationdemand.h`: the class comment of Task 2.2 item 9. LIFETIME adds: "registers the load step with the session model's scheduler; destroy it before the session model".

**Technical Approach:** Comment and label edits only; no behaviour beyond the label. Leave `docs/*.md` to Phase 4. Run the audit (`ctest -L audit`) after Tasks 2.1-2.3. This phase's code must not break any rule:
- `offer(` count;
- `withdrawChosenNext(` only in `calculationdemand.cpp`;
- restore call sites;
- `loadAllSessions` is a forbidden name, so do not use it;
- no `QThread`, `QEventLoop` or `processEvents` in `calculationdemand.*`.

If a rule trips, change the code, not the rule. The `demand` audit group is Phase 4's.

**Acceptance Criteria:**
- [ ] The progress line shows "Computing columns: k / n" with no cancel button for the whole fill, from the first load to the last result (manual M-P2-1, M-P2-4).
- [ ] `ctest -L audit` passes with no rule edited.
- [ ] No comment in `src/` says that starting the application never starts a job, or that nothing requests a column's calculation.

**Complexity:** S

---

### Task 2.5: Test support

**Purpose:** Let tests wait for column demand to finish and build columns and stubs concisely.

**Files to modify:**
- `tests/support/plotfixture.h` / `.cpp`: `waitDemandIdle(executor, demand, timeoutMs)` also requires `!demand.hasFillWork()`. The executor goes idle between one held session's job and the next load, and must not be mistaken for the end. Document: "follow with `waitForIdle(model)` when column values must be filled".
- `tests/support/logbookprobe.h` / `.cpp`: add `FlySight::LogbookColumn attributeColumn(const QString &key);`, a session-attribute column over `key` (the pattern of `descriptionColumn()` and `tst_result_columns.cpp:81`). Do not remove `tst_result_columns`' local `attributeColumn(const char *)`: its anonymous namespace shadows cleanly, or rename the local one to avoid ambiguity if the compiler complains.

**Acceptance Criteria:**
- [ ] The support library builds; `waitDemandIdle` returns false while `hasFillWork()` is true.

**Complexity:** S

---

### Task 2.6: Column demand tests in `tst_calculation_demand`

**Purpose:** Prove column demand, the load step, the bound, holds, settlements, priority, the per-column and per-cell state, and the ordering guarantee with synthetic calculations. This covers spec §13's column rows.

**Files to modify:**
- `tests/tst_calculation_demand.cpp`

**Technical Approach:**

**Fixture additions:**
- `initTestCase`: `PreferencesManager::instance().registerPreference(PreferenceKeys::LogbookColumnsVersion, 0)` and `LogbookColumnStore::instance().setColumns({descriptionColumn()})`.
- `cleanup`:
  - move `m_demand.reset()` to **right after** `m_queue->shutdown()` and **before** the pin check. The demand layer releases its holds when destroyed, so the check proves both release paths;
  - after the model is gone: `LogbookColumnStore::instance().setColumns({descriptionColumn()})`.
- Helpers:
  - `enableColumns(QList<LogbookColumn>)`: description plus the given columns, through `LogbookColumnStore::setColumns`, the path `applyProfile` step 7 takes;
  - `makeStubs(QStringList)`: hide, `LogbookCacheSize` 0, then back to the test's capacity; verify every row is a stub;
  - `section(key)`: the column index of an attribute column in `m_model`;
  - `colId(key)` = `CalculationDemand::columnId(attributeColumn(key))`;
  - `col(key)` = `flush()` + `columnState(colId(key))`;
  - `heldHidden()`: pinned, hidden, loaded rows.
- Columns: `G_OUT` (gated: title "Gated", G_OUT = G_IN + 1), `H_OUT` (afterG ← gated, a chain), `T_OUT` (thrower), `EA1` (expA; rejects EA_IN < 0 with "negative input"), `X_OUT` (exhausted: `bad_alloc` on the first call).
- Inputs through `PlotFixture::giveInput` before `makeStubs`.
- Gate: open with enough permits (`gate().open(n)`) wherever the test does not hold a job.

**Functions:**

| Function | What it proves |
|---|---|
| `columnIdIsTheDefinitionKey` | `columnId(c) == logbookColumnDefinitionKey(c)`; `columnState` of `_DESCRIPTION`, of a disabled `G_OUT` and of `"nope"` is default; `isCellPending(-1, 0)`, `(0, 99)` and `(0, description)` are false. |
| `ordinaryColumnsCreateNoDemand` | Only the description column: no job (`Quiet`), no load (`sessionLoaded` spy), `recordSetLookups() == 0` after `spin()`, no `columnStateChanged`. |
| `enablingColumnFillsEveryUnloadedSession_data/()` | Rows "capacity 0", "capacity 1", "capacity 50" (`LogbookCacheSize`). |
| `enablingColumnFillsEveryUnloadedSession` (§13) | `G_IN` = 1, 3, 4 on s1, s3, s4; s2 has none. All four are stubs. `enableColumns({G_OUT})`. **At once** (`col`): `requested`, `wantedCount 4`, `waitingCount 4`, `isCellPending` true for all four rows. Spies on `sessionLoaded`, `jobsChanged` and `columnStateChanged` check on every emission that `heldSessionIds().size() <= 2`, that every held id `isSessionPinned`, and that `heldHidden()` ⊆ held ∪ the executor's active sessions. After `waitDemandIdle` + `waitForIdle(model)`: jobs `Succeeded` for s1, s3, s4 in row order (`gate().startOrder() == {1, 3, 4}`); **no job for s2**; each is `stored`; `cachedValues[section(G_OUT)]` is 2, 4, 5 and the index value equals it; s2's cell is cached unavailable. The state is `wanted 3`, `done 3`, plain, with no pending cell. `heldSessionIds()` is empty and nothing is pinned. With capacity 0 every row is a stub. With capacity 50 the loaded ones are hidden and unpinned, and setting the capacity to 0 then evicts them (ordinary eviction). `sessionLoaded` fired once per session (s2 included, once). |
| `loadedHiddenSessionsNeedNoLoad` | s1–s4 loaded and hidden (the pool), `G_IN` everywhere. Enabling `G_OUT` offers them directly in row order. `heldSessionIds()` stays empty throughout; `sessionLoaded` 0. |
| `sessionShownDuringColumnDemandRunsNext` (§13) | `g` checked; `G_OUT` enabled; s1–s4 stubs with `G_IN` 1–4; gate held. s1 runs, s2 is held and chosen next. `show({"s4"})` → s4's job is the chosen next job and s2's is `Cancelled` "No longer needed". s2 stays held (`heldSessionIds() == {s1, s2}`). Open the gate: `startOrder() == {1, 4, 2, 3}`. The bound held throughout. |
| `columnPriorityFollowsRowOrderAfterPlots` | s1 and s3 stubs, s2 and s4 loaded and hidden, `G_IN` 1–4; no plot; gate held. The first pass offers s2, the first loaded candidate, and it starts. The chosen next job is s4's until the fill has loaded s1, and s1's replaces it (s4's ends `Cancelled` "No longer needed"). Hold the gate until `heldSessionIds().size() == 2` (`QTRY_VERIFY`), then open it: `startOrder() == {2, 1, 3, 4}`. Second part: with the executor idle again, give new inputs, focus s3 and show s4 with `g` checked. The focused session's plot pair runs first, then s4's, before any remaining column demand. |
| `visibleSessionsFirstWithinColumnDemand` (§13) | All four loaded, `G_IN` 1–4, no plot checked; s1 and s2 hidden in the pool, s3 and s4 visible (their plots unchecked, so no plot demand); gate held; `enableColumns({G_OUT})`. `startOrder() == {3, 4, 1, 2}`: visible sessions first in row order, then hidden loaded ones. Then hide s4 while s3 runs and s4 is chosen next: s4's job stays chosen (it is still loaded and in demand; only its tier changed), because a re-choice with the same key changes nothing. |
| `chainedColumnWithUpstreamRecordIsCompleted` (§13) | `H_OUT` (afterG ← gated) enabled over a stub s1 (`G_IN 4`) that already has a `gated` record (a synchronous `engine.request("gated")` before eviction, gate open) and no `afterG` record. The cell is `Waiting`, not `Done`: s1 is loaded by the fill, `afterG` alone runs (`jobCount("gated") == 0`, `jobCount("afterG") == 1`), both records exist afterwards, cached `H_OUT` is 6. |
| `storedRejectionIsBadgedAfterRestartWithoutLoad` (§13) | `EA1` over s2 with `EA_IN -1`: the rejection runs, is stored, and `LogbookManager::calculationRecordReason("s2", "expA") == "negative input"`. Evict s2, `restartDemand()`: `col(EA1)` lists s2 in `failed` with `"Explicit A: negative input"`, `showsWarning()`, `!isCellPending`, and no `sessionLoaded("s2")` (`Quiet`). Then a full restart (reopen the logbook, `populateFromIndex`, a new demand layer): the same state, from `index.json`'s `"recordReasons"`, with no load and no job. An index whose entry lacks `"recordReasons"` (write it without the field) reads s2 as `Done` until the column worker's copy restores the record, after which the reason is in the index and the next pass lists s2 as failed. |
| `fillTaskReportsProgressWhileWaiting` (§13) | Four stubs with `G_IN`, gate held, `enableColumns({G_OUT})`. Spy on `activeTaskChanged` / `progressChanged` / `schedulerIdle` and count `ColumnFillTask` steps through `sessionLoaded`. After the two loads: `progressChanged(ColumnFillTask, 4, 4)` was reported, both holds are taken, `hasFillWork()` and `!canLoad()`, and over `QTest::qWait(200)` no `schedulerIdle`, no further `sessionLoaded`, and the scheduler's timer is not active (`m_model->scheduler()` exposes `isTicking()` as a test seam, or count `progressChanged` emissions: at most one per `wake()`). Open the gate once: the finished session's release wakes the scheduler, `progressChanged(ColumnFillTask, 3, 4)`, the next load happens. At the end `progressChanged(ColumnFillTask, 0, 4)` then `schedulerIdle`, and the fill's high-water mark resets (a second fill of one session reports `1, 1`). |
| `storedResultsCreateNoJob` (§13) | Synchronous `engine.request("gated")` on s1–s4 (gate opened), records written; stubs; `enableColumns({G_OUT})` → no job (`Quiet`), no `sessionLoaded`, `doneCount 4`, plain. The column worker's copies may read the records to fill the new column; that is the worker's, not a load. The same after a restart (`LogbookManager` reopen, `populateFromIndex`, `restartDemand()`). A loaded session with a record restored reads `Done` as well. |
| `notApplicableSessionIsSettledWithoutAJob` | s2 (no `G_IN`), stub → loaded once, no job, released, not pending, not wanted. After eviction, `spin()` ×3: no second `sessionLoaded("s2")`; `hasFillWork()` false. |
| `columnFailuresAreBadgedNotReloaded` | `T_OUT` on s1 (`T_IN` given): `Succeeded`; the Failed-status result is not stored; failed with "Thrower: synthetic failure"; after eviction still failed (a settlement); no second load or job. `EA1` on s2 (`EA_IN -1`): a rejection, **stored**; `failed` with "Explicit A: negative input"; after eviction still failed (the settlement comes before the record). `showsWarning()` once work is done. |
| `columnJobLevelFailureIsNotReloadedUntilRestart` | `X_OUT` on s1 (a stub): the job ends `Failed` ("Exhausted: Out of memory"), `failed` with `jobFailure`, released, evicted, not loaded again (`spin()` ×3). `restartDemand()` → s1 loaded again → `Succeeded` → stored → Done. |
| `unloadableSessionIsSettledAsFailed` | Delete s3's session file while s3 is a stub. The load step leaves a placeholder: `failed` with "The session file could not be loaded" and `jobFailure`; not held, not pinned; no job; not loaded again. |
| `visibleFailedLoadIsSettledAsFailed` | `G_OUT` enabled, `G_IN` on s1–s4, all stubs. Delete s2's session file, then `show({"s2"})`. The synchronous visible load fails and s2 is a visible failed-load placeholder. After `waitDemandIdle` + `waitForIdle(model)`: s2's track is in `failed` with `jobFailure` and the reason "The session file could not be loaded", and **its `sessionName` is `"Jump 2"`** (the logbook's cached description, set in `init()`), not `"s2"`; the tooltip contains `"  Jump 2 - The session file could not be loaded"`; `isCellPending(row(s2), section(G_OUT))` is false; the column state has `waitingCount 0`, `runningCount 0`, `!isWorking()`, `showsWarning()`, `failedCount 1`, and s1, s3, s4 are `Done`. No job was created for s2. `heldSessionIds()` never contained s2, and `hasFillWork()` is false after `spin()` ×3. Hide s2 and evict it (capacity 0): it is still failed and is not loaded again. |
| `chainedColumnKeepsItsHold` | `H_OUT` over a stub s1: one `sessionLoaded`; `gated` then `afterG` with no other call; `heldSessionIds() == {"s1"}` from the load until `afterG` ends, and never empty in between (checked in a `jobFinished` slot); both stored; cached `H_OUT` 6 (for `G_IN 4`). |
| `disablingColumnReleasesHeldSessions` | s1 running (held), s2 held and chosen next. Disable `G_OUT`: after `flush()`, s2's job is `Cancelled` "No longer needed", s2 is unpinned and s1 is still pinned (by the executor). Open the gate: s1 is `Succeeded`, `stored`, unpinned. With capacity 0 both are stubs after `spin()`. |
| `heldSessionShownStaysLoaded` | s1 held and running; `show({"s1"})`; the job ends; s1 is unpinned and still loaded (visible). Hide → evicted (capacity 0). |
| `removedOrRepopulatedHeldSessionIsReleased` | `removeSessions({"s2"})` while s2 is held → released at once (not pinned, not in `heldSessionIds`). `populateFromIndex(...)` (the logbook reopened) while s1 is held → released; the executor supersedes. |
| `identityStubIsOfferedUnderItsRealId` | Remove `index.json`, `reopenLogbook()`, `initialize()` (deferred scan), `populateFromUuids(scannedUuids())`, then `restartDemand()`, `enableColumns({G_OUT})` and `flush()`. **Before any event-loop turn**, call `runLoadStep()`, so the column worker has not remapped the row. Assert: the row's id is the real id; `heldSessionIds()` equals it; `isSessionPinned(stem)` is false. Then `spin()`: the job's `sessionId` is the real id, and `stored(realId, "gated")`. |
| `columnStateCountsAndPendingCells` | Four stubs, s2 without input, gate held. Before any load: `wanted 4`, `waiting 4`. After `gate().waitEntered()` + `flush()`: `runningCount 1` (s1, `progressText` "step 1", tooltip starting `"Computing: 0 of "` and containing `"  Jump 1 - Gated: step 1"`), `waitingCount == wantedCount - 1`, `wantedCount` 4 or 3 (s2 may already be settled), `waiting` list empty. `isCellPending(row(s1), section)` is true and the cached value underneath is present and invalid. When s2 is found not applicable: `wanted 3`. At the end `progressLabel` is empty and the state plain. `columnStateChanged` is only ever emitted with `colId("G_OUT")`, and a pass that changes nothing emits nothing. |
| `profileStyleColumnsCreateDemand` (§13) | `LogbookColumnStore::setColumns` with the `applyProfile` step-7 shape (a new list, `G_OUT` enabled) creates the same demand as `enableColumns`: jobs for every session that lacks a result. |
| `startupWithEnabledColumnLoadsAfterColumnWorker` | Restart with `G_OUT` enabled and some cached values removed from the index (as `tst_result_columns::workerRestoresStoredResult` does), then create the demand layer. Record the scheduler's `activeTaskChanged` ids. The first `ColumnFillTask` comes after the last `ColumnTask` step of the start-up pass, and demand exists at once (`col().waitingCount > 0`) before any load. |
| `savesAndBulkEditsPrecedeLoadStep` (§13) | `ExtraRegistrations`: explicit `test.demand.desc`, input `_DESCRIPTION`, output `DESC_OUT = "x:" + _DESCRIPTION`. s1–s4 stubs. In one turn: `enableColumns({DESC_OUT})`, `startBulkEdit(all rows, description section, "bulk")`, `updateAttribute("s1", _DESCRIPTION, "edited")` (s1 loaded and dirty). Assertions: every `ColumnFillTask` activation comes after the last `BulkEditTask` and `SaveTask` progress; inside each `sessionLoaded` slot the session file on disk already contains `$VAR,_DESCRIPTION,bulk`; every `DESC_OUT` value is `"x:bulk"` (s1: `"x:edited"` or `"x:bulk"`, whichever was applied last, and equal to its file). No `jobQueued` for a stub session happens before its bulk item was processed. |
| `bulkEditMakesSettledSessionApplicable` | The same calculation; s2's `_DESCRIPTION` removed (`removeAttribute`), so it is not applicable: settled, evicted, not reloaded. `startBulkEdit({row(s2)}, description, "bulk")` → the settlement is cleared (single-row display change) → s2 is loaded → `DESC_OUT` `"x:bulk"`, stored. |
| `fillTaskIsLowestAndNotCancellable` | `activeTaskChanged(ColumnFillTask, false)` is seen once per fill, after every `ColumnTask` step of the pass and never while save, load or bulk edit has work. `progressChanged(ColumnFillTask, r, t)`: `t` is the fill's high-water mark and `r` falls by one per finished session across the fill. `m_model->scheduler().cancel(SessionModel::ColumnFillTask)` mid-fill changes nothing: every session is still computed. |
| `noLoadsAfterExecutorShutdown` | `shutdown()` with stubs in demand: `hasFillWork()` false, no `sessionLoaded`, `schedulerIdle` emitted, holds released on destruction. |
| `demandDestroyedReleasesHoldsAndTask` | Two holds; `m_demand.reset()` → neither is pinned. `waitForIdle(model)` sees no `ColumnFillTask` activation afterwards. |
| `passOverManyStubsReadsEachRecordSetOnce` | `populateFromIndex` with 2000 synthetic stub ids, `G_OUT` enabled, a fresh demand layer, `flush()` **without an event-loop turn** (the load step cannot run): `recordSetLookups() == 2000`, `col().waitingCount == 2000`, `waiting` empty, `recordsRead == 0`, no `sessionLoaded`. Trigger a pass through an unrelated check change (`Syn/plain`) + `flush()`: lookups stay 2000. `emit LogbookManager::instance().calculationRecordsChanged(<id 7>, "gated")` + `flush()`: 2001. Destroy the demand layer before any event-loop turn. |

**Acceptance Criteria:**
- [ ] Every function above exists as `CalculationDemandTest::<name>()` and passes.
- [ ] Spec §13 column rows are covered:
  - enabling a column fills every session, bounded, hidden, pinned, ending computed / not applicable / failed, with ordinary eviction afterwards: `enablingColumnFillsEveryUnloadedSession`;
  - visible next: `sessionShownDuringColumnDemandRunsNext`, `visibleSessionsFirstWithinColumnDemand`;
  - a chain with the upstream record stored: `chainedColumnWithUpstreamRecordIsCompleted`;
  - a stored rejection badged after a restart without a load: `storedRejectionIsBadgedAfterRestartWithoutLoad`;
  - the progress line through the whole fill, no spinning: `fillTaskReportsProgressWhileWaiting`, `fillTaskIsLowestAndNotCancellable`;
  - stored results create no job: `storedResultsCreateNoJob`;
  - profile: `profileStyleColumnsCreateDemand`;
  - saves and bulk edits first: `savesAndBulkEditsPrecedeLoadStep`.
- [ ] Every Phase 1 function of the file still passes unchanged: the description column is not requested.

**Complexity:** L

---

### Task 2.7: The column worker is unchanged; model, scheduler, manager and store entry points (`tst_result_columns`, `tst_column_cache`, `tst_session_model_engine`, `tst_logbook_index`, `tst_result_store`)

**Purpose:** Prove that the column worker behaves and counts exactly as before with column demand active, and that a stale record it deletes moves the pair into demand. Also prove the new `SessionModel`, `IdleScheduler`, `LogbookManager` and `CalculationResultStore` entries.

**Files to modify:**
- `tests/tst_result_columns.cpp`:
  - includes `calculationdemand.h`, `plotmodel.h`, `plotfixture.h`;
  - the new functions create a local `PlotModel` (no plots) and a `CalculationDemand` over `m_model`, `plots` and `m_queue`, declared so that they are destroyed before `cleanup()` resets the queue.

  | Function | What it proves |
  |---|---|
  | `columnWorkerIsUnchangedByDemand_data/()` | Rows "without demand" and "with demand". |
  | `columnWorkerIsUnchangedByDemand` | Both rows set the columns to {D, X} first; `cleanup()` restores {D, X, Y}. The `workerRestoresStoredResult` set-up (s1 has an X record, its X value removed from the index; s2 has none), `restart()`, then in the "with demand" row a demand layer. `startColumnWorker()`. **Snapshot** `columnWorkStats()` and `storedResultStats()` at the first `activeTaskChanged(ColumnFillTask)`, or after `waitForIdle` in the other row. Both rows give identical statistics: `sessionsLoaded 1`, equal `valuesComputed`, `calculationRuns 0`, `restoreCalls 1`, `recordsRead 1`, `recordsRestored 1`, `staleRecordsDeleted 0`, `recordsWritten 0`. At the snapshot, s1's X is `"x:d1"` and s2's X is cached unavailable. In the "with demand" row, afterwards s2 is loaded once, computed (`"x:d2"`), stored and stamped, and s1 is never loaded. |
  | `staleRecordDeletedByWorkerCreatesDemand` | Columns {D, X}. `fit("s1", kCalcX)` and `fit("s2", kCalcX)`, then `evict({"s1"})`, then the demand layer: nothing is in demand (`Quiet`). Bulk edit `_DESCRIPTION` "bulk" on s1. The spies observe this sequence: the column worker's copy deletes the stale record (`staleRecordsDeleted 1`, no `sessionLoaded` yet); `calculationRecordsChanged("s1", kCalcX)`; the cell becomes pending (`isCellPending("s1", colId X)`); the load step loads s1 once; exactly one job, `test.columns.x` for s1, `Succeeded`; the record is written again; the cached X is `"x:bulk"`, as in the index with its stamp. Right after the bulk edit, before the worker's step, the cell is **not** pending: a known record counts as a result. |

- `tests/tst_column_cache.cpp`:

  | Function | What it proves |
  |---|---|
  | `loadPinnedSessionLoadsWithoutShowing` | `startWithLoadedSessions`, then `restartAsStubs()`. `loadPinnedSession("g1")` returns `"g1"`. The row is loaded, `visible` false, `data(CheckStateRole)` Unchecked; `sessionLoaded` once; `storedResultStats().restoreCalls` +1; `isSessionPinned("g1")`. Setting `LogbookCacheSize` to 0 leaves it loaded; `unpinSession("g1")` + one event-loop turn → a stub. On an already-loaded row: pinned, no second `sessionLoaded`. For `"nobody"`: `""`, nothing pinned. `columnWorkStats().sessionsLoaded` unchanged (a real load, not the worker's). |
  | `loadPinnedSessionFollowsIdentityRemap` | The `bulkEditFollowsIdentityStubRemap` set-up (deferred scan; row known by `stem`). `loadPinnedSession(stem)` returns `"g1"`; `rowAt(0).sessionId == "g1"`; `isSessionPinned("g1")` true; `isSessionPinned(stem)` false. |
  | `loadPinnedSessionFailedLoadPinsNothing` | Remove the session file of a stub. `loadPinnedSession(id)` returns `""`; the row is a failed-load placeholder (`loadFailed`); not pinned. |

- `tests/tst_session_model_engine.cpp`:

  | Function | What it proves |
  |---|---|
  | `schedulerTaskCanBeUnregistered` | On `m_model->scheduler()`: register a probe task (id 90, priority 9, `hasWork` from a counter, `step` decrements it); `wake()`; the first tick emits `activeTaskChanged(90, …)`. `unregisterTask(90)` while it is active: no further step; `schedulerIdle` is emitted. Registering 90 twice and waking steps it only once per tick (one entry). `unregisterTask(12345)` is a no-op. |
  | `schedulerWaitingTaskDoesNotSpin` | Register task 91 (priority 8, `hasWork` true, `canStep` from a flag that is false, `step` counts) and task 92 (priority 9, `hasWork` from a counter of 3, `step` decrements). `wake()`: `activeTaskChanged(91, …)` and `progressChanged(91, …)` are emitted; task 92's three steps run (a lower task with work is stepped while the higher one waits); task 91's step count stays 0; then no `schedulerIdle`, and over `QTest::qWait(100)` no further `progressChanged` (the timer is not re-armed: one report per `wake()`). `wake()` again: one more `progressChanged(91, …)`, still no step. Set the flag true and `wake()`: task 91 steps. Make its `hasWork` false: `schedulerIdle`. |

- `tests/tst_logbook_index.cpp`:

  | Function | What it proves |
  |---|---|
  | `recordReasonsRoundTrip` | `setCalculationRecordReason("g1", "x", "no")` marks the index for a flush; `calculationRecordReason` returns it; `flushIndex()` writes `"recordReasons": {"x": "no"}` in `g1`'s entry and no such object in an entry without reasons; `reset()` + `initialize()` reads it back only while `cache/` holds `g1`'s `x` record file (write a record first, or place a file of that name); `removeCalculationRecord("g1", "x")` clears it; `remapSessionId` moves it; an empty reason clears it. |

- `tests/tst_result_store.cpp`:

  | Function | What it proves |
  |---|---|
  | `recordReasonRecordedAtWriteAndRestore` | A rejection published through the executor writes the record and `calculationRecordReason(s, calc) == "<the rejection's reason>"`; a success writes `""`. Remove the reason from the index by hand (an older index), reload the session: the restore sets it again; the column worker's copy restore (`restoreForColumnWorker`, through `startColumnWorker` on a stub with a missing column) sets it too; a skipped unreadable record sets nothing; a stale record deleted at restore leaves no reason. |

**Acceptance Criteria:**
- [ ] Both rows of `columnWorkerIsUnchangedByDemand` give identical worker and store statistics and values at the snapshot. No production code in `processNextDirtyColumn`, `settleExplicitColumns`, `restoreForColumnWorker`, `fillMissingColumns` or `refreshRecordColumns` changed (`git diff` of those functions is empty). The store's `restoreSession()` gained only the reason report.
- [ ] `schedulerWaitingTaskDoesNotSpin`, `recordReasonsRoundTrip` and `recordReasonRecordedAtWriteAndRestore` pass.
- [ ] `staleRecordDeletedByWorkerCreatesDemand` passes.
- [ ] Every existing function in the three files passes unchanged.

**Complexity:** M

---

### Task 2.8: A real fusion column (`tst_fusion_store`)

**Purpose:** Prove end to end, with real fits, that a column over a fusion output is filled for sessions that are not loaded, stored, and not recomputed on the next start.

**Files to modify:**
- `tests/tst_fusion_store.cpp`. It is already a fusion test in `_FLYSIGHT_GTSAM_REACHERS` (`cmake/SolverDependencies.cmake:531`), so no CMake change is needed. Update the header comment: it no longer excludes "the logbook-column item".

**Technical Approach:**

Column: `MeasurementAtMarker` {`sensorID "Fusion"`, `measurementID "roll"`, `measurementType` as `fusionPlots()` gives roll, `markerAttributeKey = SessionKeys::ExitTime`}. Its value is the attribute `fusionRollAtExit()` names. Restore the columns to `{descriptionColumn()}` with a `qScopeGuard`.

| Function | What it proves |
|---|---|
| `columnOverFusionFillsUnloadedSessions` | `fixtureSession("coarse_maneuver", "a")`, and `sessionWithoutImu(fusionFixture("coarse_linear"), "n1")` via `addSessions`; `waitForIdle`; both evicted to stubs (capacity 0, restored by the guard). Enable the column. After `waitDemandIdle(*m_queue, *m_demand, kFitTimeoutMs)` + `waitForIdle(*m_model)`: exactly one job (`a`, `Succeeded`); no job for n1 (not applicable, not wanted); `recordPath("a")` exists; `cachedValues` of `a` for the column equals `session("a").getAttribute(fusionRollAtExit())` of a fresh load (compare after the check; do not load before); the index value is the same, with the fit's stamp. `columnState` is plain with `wanted 1`. Both rows are stubs again (capacity 0). |
| `fusionColumnWithStoredFitsRunsNothing` | Repeat the previous flow's fit (the functions are independent), then `restart()` with the column enabled and a new demand layer. `Quiet` holds through `spin()` and `waitForIdle(*m_model)`; no `sessionLoaded`; `columnState(...).doneCount == 1`; the column's cached value for `a` is unchanged. |

**Acceptance Criteria:**
- [ ] Both functions pass under the `fusion` label, and in `exact` where that runs.

**Complexity:** M

---

## Testing Requirements

### Unit Tests
- **New functions:**
  - `tst_calculation_demand`: 26 functions (Task 2.6);
  - `tst_result_columns`: `columnWorkerIsUnchangedByDemand`, `staleRecordDeletedByWorkerCreatesDemand`;
  - `tst_column_cache`: `loadPinnedSessionLoadsWithoutShowing`, `loadPinnedSessionFollowsIdentityRemap`, `loadPinnedSessionFailedLoadPinsNothing`;
  - `tst_session_model_engine`: `schedulerTaskCanBeUnregistered`;
  - `tst_fusion_store`: `columnOverFusionFillsUnloadedSessions`, `fusionColumnWithStoredFitsRunsNothing`.
- **Changed:**
  - `tst_calculation_demand`: `initTestCase` and `cleanup` (column store; the demand layer destroyed before the pin check);
  - `tests/support/plotfixture.*` (`waitDemandIdle`);
  - `tests/support/logbookprobe.*` (`attributeColumn`).
- **Unchanged and must pass:** every other suite, in particular `tst_column_cache`, `tst_result_columns`, `tst_result_store`, `tst_result_records`, `tst_logbook_index`, `tst_jobqueue`, `tst_plot_row_delegate` and `tst_fusion_rows`.

### Integration Tests
- Full `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure` (labels core, fusion, exact, python, audit): all green.
- `audit_cleanup` passes with no rule edited. The acceptance map is untouched; Phase 4 adds items 501+.

### Manual Verification
1. **M-P2-1, fill.** On a logbook of ~50 sessions, enable a column "roll @ exit" in the column editor. The progress line shows "Computing columns: k / n" for the whole fill, with no cancel button, k rising as fits finish; it disappears when the last value is in. Values appear row by row, and at most two sessions beyond the pool are loaded at once (Task Manager memory stays flat). While a fit runs and both holds are taken, the application is idle apart from the fit (no main-thread activity from the scheduler).
2. **M-P2-2, visible first.** During the fill, check the roll plot and show a session whose fit is missing. It is computed next, after the running fit.
3. **M-P2-3, disable.** Disable the column mid-fill. The running fit finishes and is stored; nothing else starts; the progress line goes idle.
4. **M-P2-4, restart.** Restart with the column enabled. No fit runs for sessions that were computed; sessions without IMU data are not loaded again during the run.
5. **M-P2-5, bulk edit.** Bulk-edit `_DESCRIPTION` on 100 stubs while the fill is running. The bulk edit's progress runs to completion before any further "Computing columns" step.

## Notes for Implementer

### Gotchas
- **The unloaded rule must never open a record.** Use `knownCalculationRecords()` through the memo only. `readCalculationRecord`, `calculationRecordIds` and `restoreSession` are the result store's (audit `stored-results`).
- **Never call `getSessionRow()` inside the walk.** It is a linear scan. Walk rows by index under one guard, and call `getSessionRow` at most once per load step.
- **Settlements are what stop reload loops.** A session loaded for column demand whose cell ends without a record would otherwise be loaded again after eviction, indefinitely. This applies to not applicable, an exception result, a failed record write, a job-level failure and a failed load. `columnFailuresAreBadgedNotReloaded`, `notApplicableSessionIsSettledWithoutAJob` and `unloadableSessionIsSettledAsFailed` guard this. Do not drop settlements on multi-row changes (the unit system, the environment check): that would reload every settled session.
- **`SessionModel::sort()` resets the model.** Forget settlements only for ids without a row, as Phase 1 does for memory.
- **Silent id changes.** The column worker (`sessionmodel.cpp:2127`), the bulk edit (`:2289`) and `resolveIdentityStubs()` remap an identity stub's id without a signal. Entries under the old id become orphans, harmless until the next model reset, and the load step re-validates its candidate. Never pin before the load.
- **`dataChanged` from the column worker fires for every stub it processes** (thousands at start-up). The slot must be O(1) and must schedule only when it erased a settlement of an unloaded session.
- **Order of destruction.** The demand layer must unregister its task in its destructor, because the lambdas capture `this`. Tests that recreate the demand layer (`restartDemand`) rely on `registerTask` replacing and `unregisterTask` removing.
- **`waitForIdle(model)` can return while column demand is unfinished.** The load step has no work while both holds are taken, so the scheduler goes idle. Use `waitDemandIdle` first.
- **The executor is idle between one held session's job and the next load.** Phase 1's synchronous pass in `jobFinished` offers the other held session, if any. Otherwise the next session arrives by the fill's load on a later tick, which the pass wakes.
- **The fill task must not spin.** Its `hasWork` is true for the whole fill; only `canStep` decides whether a load happens. Never make `hasWork` depend on a free hold, or the line flashes; never make `canStep` true without a candidate, or the scheduler ticks for nothing. Every pass that changes the fill's numbers or candidates calls `wake()`.
- **"Every record", not "any".** The unloaded rule's Done needs a record for each calculation the column needs. `containsAnyOf` is the column worker's test, not the demand layer's.
- **The `offer(` audit count is 1.** Column candidates go through the same `offerChoice`; do not add a second call site.
- `LogbookColumnStore` is a process-wide singleton. Every test that changes columns restores them after the model is gone.

### Decisions Made
- **`SessionModel::loadPinnedSession(id)`** is a new narrow entry. `sessionRef()` already performs the real load for a hidden row (verified). The entry adds a pin under the id the row has after the correction, placed before anything can evict, and a failure report. The demand layer never touches `sessionRef()` or row references.
- **Holds begin at the load, not at the choice.** A stub has nothing to evict, and its id may change during the load. Already-loaded sessions are never held; the executor's pins cover them while chosen or running, and they do not count toward the bound.
- **Refinement of "pinned until its job has ended".** A hold is released when the session has no `Waiting` or `Running` cell left. This equals "its job has ended" for single calculations and keeps the hold across chained requested calculations; releasing between links would let the session be evicted with only its upstream record and never compute the downstream one. It also covers "settled not applicable" and "left demand".
- **Bound:** `kMaxHeldSessions = JobQueue::kMaxRunningJobs + 1` (2). The pool may exceed `LogbookCacheSize` by at most the holds.
- **Tier (c) is visible sessions first, then hidden loaded sessions, each in row order** (Michael's decision at the plan review: what the user is looking at before what only the logbook shows). Unloaded sessions are loaded in row order by the fill and enter the choice once loaded. A load for an earlier row replaces a later chosen next job (Phase 1's replacement, `Cancelled` "No longer needed"). No offer is ever made for an unloaded session.
- **Result existence for an unloaded session:** a record for **every** id of E(c) without family instances (the plan review's correction: with "any", a chain whose upstream record alone was stored would never complete), and the reason the index recorded for those records decides Done against Failed. Explicit family instances are never stored, so they create no column demand for unloaded sessions.
- **Record outcomes live in the logbook index** (`"recordReasons"`), reported by the result store where it already has the record open (write and restore). This is a fact about what is on disk, not about demand, so it sits with the manager and the store; the column worker's code is untouched, and the demand layer reads it as it reads the record names. It replaces the earlier known limit "a stored rejection counts as Done after a restart".
- **Column settlements** are a new column-level memory keyed by `(session, column id)`. They refine the overview's failure memory, which is per pair: a session that does not apply to a column, or a Delta whose marker is missing, must not suppress plot demand of the same calculation, so a per-pair not-applicable memory could not be used. For unloaded sessions a settlement takes precedence over the record set. A rejection then stays badged after eviction in this run; after a restart it counts as Done, because records are never opened.
- **Progress-line cancel: not cancellable.** Spec §3 excludes a pause switch and §10 "no cancel for requested calculations anywhere"; what is wanted changes only by disabling the column. A cancel that left the demand in place would be undone on the next tick, and one that remembered it would be the excluded pause switch. `onComplete` is a no-op, and an API `cancel()` changes nothing.
- **Task id:** `SessionModel::ColumnFillTask = 4` in the existing `WorkerTask` enum (the scheduler's one id namespace, which `LogbookView` already switches on), priority 5, label "Computing columns: %v / %m" (the same as on-demand column work, on purpose). It is registered by the demand layer through `SessionModel::scheduler()`, has work for the whole fill and steps only when it can load.
- **`IdleScheduler::unregisterTask`**, `registerTask` replacing an existing id, and **`TaskDef::canStep`** with the waiting semantics (reported, not stepped, no spinning). All generic, and the scheduler learns nothing about jobs. The alternative, the logbook view reading the demand layer's counts when the scheduler is idle, would have given the progress line two sources of truth.
- **Column id** = `logbookColumnDefinitionKey()`. Definitions are unique in the store, and the index already keys values by it.
- **Column states do not list waiting tracks**, only count them. Phase 3's hover detail shows counts, the running session and failures.
- **Loaded-report memo and record-set memo** are specified with exact invalidation, so a pass over thousands of sessions costs hash lookups only.
- **The single-row display `dataChanged` rule** stands in for the missing `dependencyChanged` of the bulk edit and the column worker.
- **A failed-load placeholder is classified like an unloaded session and is always settled.** A failed load, whether by the load step (a hidden row) or by the visible loader, focus or any other `sessionRef()` (a visible row), is settled as a job-level failure with the reason "The session file could not be loaded". It is badged, not in demand, and not retried in this run unless a clearing rule fires (the row's data, records or inputs change). Without this, a visible placeholder would stay `Waiting` forever: the load step takes only non-visible stubs, and nothing retries a failed visible load. This is demand-layer logic, so Phase 3 adds nothing for it.

### Open Questions
- **Columns whose requested calculation is only an alternative candidate.** Suppose a column's static closure reaches a requested calculation only through one of several candidates of an on-demand name. Then each unloaded session without a record would be loaded once per run to find that the column is available, or not applicable, without it. No registered column is like this today: fusion outputs have one candidate each. If one appears, the fix belongs to the registry's authority (`explicitDependencies`), not to the demand layer.

## Definition of Done

This phase is complete when:
1. All tasks have passing acceptance criteria.
2. All tests pass under `build-phase1` (core, fusion, exact, python, audit).
3. Code follows the patterns of the reference files:
   - the demand layer's guarded read and coalesced pass;
   - `SessionModel`'s pin and LRU rules;
   - the scheduler's task definitions;
   - the manager's record-stamp bookkeeping for the record reasons;
   - the column worker is untouched.
4. No TODOs or placeholder code remain. No product code calls `sessionRef()` on behalf of the demand layer or opens a record for it. The demand layer has no second `offer(` call site.
