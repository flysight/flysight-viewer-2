# Phase 4: Logbook columns over requested results

## Overview

A logbook column that depends on an Explicit calculation (today: any column
over a sensor fusion output) stops being forced to "unavailable". For a loaded
session it is computed from the engine, like every other column: the result
restored at load or just published, or unavailable when the calculation is not
requested. The value is cached in `index.json` with a per-session **record
stamp**: the set of Explicit calculations that have a record file for the
session, and the result version of each. Writing or deleting a record drops
the values that depend on that calculation. A loaded row recomputes them on
the next event-loop pass. An unloaded row is settled by the column worker,
which never reads a record: **pending** when a record exists, **unavailable**
when none does. A write ordering rule and the stamp check at start-up ensure
that, after a crash at any point, no cached value disagrees with the records on
disk.

## Dependencies

- **Depends on:** Phase 1 (`CalculationDescriptor::resultVersion`, the engine
  listener), Phase 2 (record files, `LogbookManager` record methods,
  `calculationRecordFileNames()` / `parseRecordFileName`, stray pass), Phase 3
  (`CalculationResultStore`, `SessionModel::restoreStoredResults`,
  `storedResultStats()`, `LogbookManager::reserveSessionFile` and
  `recordStem`). None of them is implemented yet. This document uses their
  documented names exactly.
- **Blocks:** Phase 5 (documentation, acceptance map, audit).
- **Assumptions:**
  - Records change on disk only in these places: `writeCalculationRecord`,
    `removeCalculationRecord(s)`, `removeSession` (and its reserved-only
    branch, Phase 3), and the stray pass at the start of `initialize()`. All
    of them are in `LogbookManager` (Phase 2 hand-off; Phase 3 hand-off 6).
  - Phase 3's store writes on an `Installed Ok` event and deletes on
    `DroppedByInputChange` and on stale-at-load. Both happen only for a row
    that is loaded, because only `attachSession` installs the listener, and a
    load-time restore runs only in `restoreStoredResults()`.
  - A registry change drops a loaded session's explicit results from memory
    and deletes no record (Phase 3, `noDeleteWithoutInputChange`).
  - Registrations are complete before `LogbookManager::initialize()` (as
    today; `checkCalculationEnvironment` comment, sessionmodel.cpp 1308-1309).
  - Line numbers are from 8dc4e38 and are guides. Phases 1-3 move them.

## The mechanism (summary; the tasks give the detail)

**Vocabulary.**
- *E(c)*: the Explicit calculations behind column `c`. It is the union of the
  new `CalculationRegistry::explicitDependencies(name)` over the names the
  column reads (Task 4.1). A column is *explicit-backed* when E(c) is not
  empty. This is the same test as today's `dependsOnExplicit`.
- *Known records of S*: the calculation ids with a record file for session S.
  `LogbookManager` builds this set from one names-only listing of
  `sessions/*.fvresult` at `initialize()`, then keeps it up to date through
  its own writes and removals. No record is ever opened for it.
- *Unconfirmed records of S*: the ids whose record may disagree with the
  loaded session's engine. They are set by a failed write, by a failed
  removal, and by an environment change while S is loaded. Values that depend
  on them are kept out of `index.json`.

**index.json shape.** Each session entry gains `"records"`:

```json
"sessions": {
  "<SESSION_ID>": {
    "uuid": "3f2c...",
    "lastAccessed": 1727000000,
    "values":  { "<column id>": 0.27, "<column id>": "text" },
    "records": { "builtin.fusion.fit": "batch-temperature-bias-v3" }
  }
}
```

`"records"` maps each known and confirmed record of the session to the
current result version of its calculation (`""` when the descriptor declares
none, or when the id is not a plain registration). `flushIndex()` always
writes it, even when it is empty (`{}`). An entry without the key was written
by an older build. The root object keeps its four fields
(`tst_logbook_index::markerWrittenOnFlush` pins them).

**Validity of an explicit-backed value at start-up** (extended index format,
after the existing marker/environment check has kept the values). Take a value
of column `c` with E(c) ≠ ∅ in the entry of session S:
- The entry has `"records"`: the value is kept only when, for every `e` in
  E(c), `e` is in the stamp exactly when `e` is in S's known records, and,
  when it is, the stamp's version equals the current result version of `e`.
- The entry has no `"records"` (older build, where explicit-backed values were
  always cached as unavailable): the value is kept only when S has no known
  record of any `e` in E(c).
- Otherwise the value is dropped. The session then has no value for that
  column, and `m_indexNeedsFlush` is set.

**Crash consistency** comes from the check above plus one ordering rule. Before
the manager writes a record of (S, e), it checks whether the `index.json` on
disk holds a value of S that depends on `e` while listing `e` as present. If
so, it first flushes the index with those values dropped and `e` left out of
the stamp. Deletions need no flush first: any value that depended on the
deleted record is listed as present in a stamp, and the record is now absent,
so the check catches it. The only sequence the check cannot see is
present → deleted → written again with no flush in between, and that is exactly
the case the ordering rule flushes. Task 4.2 has the crash table.

**Model.** A record change of (S, e) drops S's values of every column with `e`
in E(c), in both the manager and the row. It is signalled by
`LogbookManager::calculationRecordsChanged`. For a loaded row, a queued pass
(`refreshRecordColumns`) recomputes those values from the engine on the next
event-loop pass. The pass emits nothing, because loaded cells are live, and
flushes the index. For a stub, the column worker settles the values from the
known records alone, with no load: pending or unavailable.

## Tasks

### Task 4.1: Registry query and column-name helpers

**Purpose:** Give the column cache one registration-derived answer to "which
Explicit calculations does this column depend on", shared by the model and the
manager.

**Files to modify:**
- `src/engine/calculationregistry.h` / `.cpp`: new query
  `explicitDependencies`, with `dependsOnExplicit` expressed through it.
- `src/logbookcolumn.h` / `.cpp`: two free functions.
- `src/sessionmodel.h` / `.cpp`: delete `SessionModel::columnNames` (h 339,
  cpp 1329-1345) and use `logbookColumnNames` at its call sites.
- `tests/tst_calcregistry.cpp`: new test `explicitDependencies`.

**Technical Approach:**

1. Add to `CalculationRegistry`, in the "Registration-derived queries for the
   logbook column cache" section, directly above `dependsOnExplicit` (h 142-151):
   ```cpp
   /// The instance ids of the explicit calculations behind `name`: every
   /// candidate (and, for a measurement, every source conversion) whose policy
   /// is explicit, of every name in staticDependencies(name).names (`name`
   /// itself included). Sorted, unique. A function of the registrations
   /// alone. Memoized and dropped exactly like staticDependencies().
   QStringList explicitDependencies(const DependencyKey &name) const;
   ```
   Implement it with the loop of `dependsOnExplicit` (cpp 457-485), collecting
   `candidate.instanceId` instead of stopping at the first match. Add the memo
   `mutable QHash<DependencyKey, QStringList> m_explicitDependencyMemo`, and
   clear it where `m_dependsOnExplicitMemo` is cleared (cpp ~558). Then write
   `dependsOnExplicit(name)` as `!explicitDependencies(name).isEmpty()` and
   delete its own memo, so there is one implementation. Update its comment:
   "The one authority for 'explicit-backed': the plot rows ask here; the
   logbook column cache asks explicitDependencies(), of which this is the
   non-emptiness."
2. In `src/logbookcolumn.h` (forward-declare `class CalculationRegistry;` and
   include `dependencykey.h`, `<QList>`, `<QStringList>`):
   ```cpp
   /// The names a column's value is read from (SessionModel::computeColumnValues
   /// reads exactly these): the attribute, or the interpolation key(s) of the
   /// measurement at the marker(s).
   QList<DependencyKey> logbookColumnNames(const LogbookColumn &col);
   /// The explicit calculations the column's value depends on: the union of
   /// CalculationRegistry::explicitDependencies() over logbookColumnNames(col),
   /// sorted and unique. Empty for a column that is not explicit-backed.
   QStringList logbookColumnExplicitCalculations(const LogbookColumn &col,
                                                 const CalculationRegistry &registry);
   ```
   The body of `logbookColumnNames` is the body of `SessionModel::columnNames`
   (cpp 1331-1344), moved unchanged. `logbookcolumn.cpp` includes
   `sessiondata.h` (for `SessionData::interpolationKey`, `SessionKeys::Time`)
   and `engine/calculationregistry.h`. No default argument, so that the header
   does not pull in the engine: callers pass `CalculationRegistry::instance()`.
3. `SessionModel::rebuildColumnDependencies` (cpp 1347-1363) calls
   `logbookColumnNames(col)`. `computeColumnValues`' explicit branch goes away
   in Task 4.3.

**Acceptance Criteria:**
- [ ] `tst_calcregistry::explicitDependencies` (private registry,
      `Synthetic::registerExplicitWorld`): `DDA` → `{"expA"}`, `EA_DIAG` →
      `{"expA"}`, `DB` → `{"expA","expB"}`, `EA_IN` → `{}`, `NO_SUCH_NAME` →
      `{}`. After `unregister("expA")`: `DDA` → `{}`, `DB` → `{"expB"}`. For
      each of these names, `dependsOnExplicit(n) ==
      !explicitDependencies(n).isEmpty()`. `engine.totalRunCount() == 0` and
      `state.readCount() == 0`.
- [ ] `tst_calcregistry::dependsOnExplicit` and `tst_plot_requests` pass
      unchanged.
- [ ] `SessionModel::columnNames` no longer exists. `grep -n "EvaluationPolicy::Explicit" src/logbookcolumn.cpp`
      finds nothing (audit "one authority: explicit-backed").

**Complexity:** S

---

### Task 4.2: Record stamps in `LogbookManager`

**Purpose:** Make the manager, the one authority on cache validity, tie
explicit-backed values to the record set, keep them consistent across crashes,
and tell the model when a record changes.

**Files to modify:**
- `src/logbookmanager.h`: new signal, public accessors, private state and
  helpers, class comment (Task 4.4).
- `src/logbookmanager.cpp`: `initialize`, `reset`, `flushIndex`,
  `remapSessionId`, `removeSession` (including the reserved-only branch),
  `writeCalculationRecord`, `removeCalculationRecord(s)`, new helpers.

**Technical Approach:**

*State* (private):
```cpp
QMap<QString, QSet<QString>> m_knownRecords;        // SESSION_ID -> calculation ids with a record file
QMap<QString, QSet<QString>> m_unconfirmedRecords;  // SESSION_ID -> ids whose record may disagree with the loaded engine
QMap<QString, QSet<QString>> m_recordBackedOnDisk;  // SESSION_ID -> ids index.json on disk lists as present
                                                    //   AND on which some value it holds for the session depends
```
Key all three by session id, as `m_cachedValues` is. A reserved (unsaved)
session is keyed by its id as well. `reset()` clears all three.
`remapSessionId` moves all three with the id, as it moves the unsaved marks
(cpp 700-709). `removeSession` erases all three in both of its branches.

*Public API* (new section `// --- Record stamps of cached column values ---`
after Phase 2's record section):
```cpp
signals:
    // A record of (sessionId, calculationId) was written, removed, or a write or
    // removal of it failed. The cached values of the session that depend on the
    // calculation have already been dropped here. Emitted synchronously, from
    // inside the record method (so possibly from an engine listener): a receiver
    // must only drop state and defer work. Not emitted by removeSession(), the
    // stray pass, or initialize().
    void calculationRecordsChanged(const QString &sessionId, const QString &calculationId);

public:
    // Calculation ids with a record file for the session, as known from the
    // names-only listing at initialize() and this manager's own writes and
    // removals. No record is opened. Files changed behind the application's
    // back are seen at the next initialize().
    QSet<QString> knownCalculationRecords(const QString &sessionId) const;
    // Ids whose record may disagree with the loaded session's engine. Values
    // that depend on them are never written to index.json.
    QSet<QString> unconfirmedCalculationRecords(const QString &sessionId) const;
    // Marks every known record of the session unconfirmed. SessionModel calls
    // it for each loaded row on a calculation-environment change, because a
    // registry change drops explicit results from memory and keeps their records.
    void markCalculationRecordsUnconfirmed(const QString &sessionId);
    // The session's in-memory results are being discarded (eviction): drops
    // the cached values that depend on its unconfirmed records, forgets the
    // marks, and returns the ids. From now on the records on disk are the truth.
    QStringList discardUnconfirmedCalculationRecords(const QString &sessionId);
```

*Helpers* (private, file-local where possible):
- `QHash<QString, QStringList> explicitCalculationsByDefKey(const QVector<LogbookColumn> &columns)`:
  `columnDefinitionKey(col)` → `logbookColumnExplicitCalculations(col, CalculationRegistry::instance())`.
- `QString currentResultVersion(const QString &id)`:
  `CalculationRegistry::instance().instance(id)` → `descriptor->resultVersion`,
  else `QString()`.
- `bool dropRecordDependentValues(const QString &sessionId, const QStringList &calculationIds)`:
  With the enabled columns (`LogbookColumnStore::instance().enabledColumns()`),
  removes from `m_cachedValues[sessionId]` every value whose column's E
  intersects `calculationIds`. It also removes every value whose definition
  key is not an enabled column, for the reason `dropCachedValuesExcept` gives
  (h 119-123). It returns true when something was removed, and in that case
  sets `m_indexNeedsFlush`. It does **not** mark anything unsaved: this is
  cache validity, not save ordering.

*`writeCalculationRecord(sessionId, record)`*: Phase 2's function, with these
steps added. Let `e = record.result.calculationId`.
1. Resolve the stem (Phase 3's `recordStem`). An unknown session fails as
   documented, with no signal.
2. Encode (Phase 2). On failure: `m_unconfirmedRecords[S] += e`,
   `dropRecordDependentValues(S, {e})`, emit, and return false. Encoding
   first means that a refused encoding never flushes anything. Phase 3's
   `writeFailureKeepsPreviousRecord` expects exactly one warning.
3. `m_unconfirmedRecords[S] += e` and `dropRecordDependentValues(S, {e})`. The
   in-memory result is new, so every value over `e` is gone.
4. **Ordering rule.** If `m_recordBackedOnDisk[S]` contains `e`, call
   `flushIndex()` first. Step 3's mark makes that flush leave `e` out of the
   stamp and omit the values over `e`. If the flush fails, set `*error` to
   `"index.json could not be written"`, warn with the Phase 2 warning format,
   emit, and return false. The record is not written, which follows
   `saveSession` step b (cpp 607-616).
5. `QSaveFile` write (Phase 2). On failure: emit and return false. The mark
   stays, and the known set is unchanged (the previous file, if any, is intact).
6. On success: `m_knownRecords[S] += e`, `m_unconfirmedRecords[S] -= e`, emit,
   and return true.

*`removeCalculationRecord(sessionId, e)`*: Phase 2's function, with these
steps added.
1. Resolve the stem. An unknown session returns false with no signal.
2. The file does not exist. If `e` is neither known nor unconfirmed for S,
   return true with no signal: nothing changed. This is the common
   `DroppedByInputChange` case with no record. Otherwise drop the values over
   `e`, `known -= e`, `unconfirmed -= e`, emit, and return true.
3. The file exists: `dropRecordDependentValues(S, {e})`, then `QFile::remove`.
   On success: `known -= e`, `unconfirmed -= e`, emit, return true. On
   failure: warn (Phase 2), `unconfirmed += e` (`known` stays: the file is
   still there), emit, and return false.

No flush comes before a removal. The start-up check sees a present→absent
flip.

`removeCalculationRecords(S)` applies the same rules per id.

*`flushIndex()`* (cpp 748-822). Before the sessions loop, compute
`byDefKey = explicitCalculationsByDefKey(enabledCols)`. Then, per session:
- `confirmed = m_knownRecords[S] - m_unconfirmedRecords[S]`.
- When building `valuesObj`, also skip a value whose `byDefKey[defKey]`
  intersects `m_unconfirmedRecords[S]`.
- `entry["records"]` is a `QJsonObject` holding `id → currentResultVersion(id)`
  for each id in `confirmed`. It is written even when empty.
- Note `backed(S)`: the union of `byDefKey[defKey]` over the values actually
  written, intersected with `confirmed`.

On a successful commit, replace `m_recordBackedOnDisk` with the `backed(S)`
map of the sessions written. This sits next to
`m_needsFlushBeforeSave.clear()`.

*`initialize()`*:
- In every branch (extended, legacy flat, fallback scan), once
  `m_sessionIdToUuid` is complete (after orphan adoption in the extended
  branch), call a private `adoptCalculationRecordSet()`. It lists
  `calculationRecordFileNames()` (Phase 2, names only, `*.fvresult`), parses
  each with `parseRecordFileName`, and, for every `(stem, id)` whose stem maps
  to a session id (invert `m_sessionIdToUuid`), inserts `id` into
  `m_knownRecords[sessionId]`. It runs after Phase 2's stray pass, which is
  the first statement. It may reuse that pass's listing.
- In the extended branch, collect while parsing entries (cpp 183-208):
  - `uuid → LogbookColumn` (from `columnFromJson`), next to `uuidToDefKey`;
  - per session, whether `"records"` is present and its object.
  
  Then, only when `valid` (marker and environment matched), after
  `adoptCalculationRecordSet()`, run `validateRecordStamps(...)`. It applies
  the validity rule of the summary to every cached value whose column is
  explicit-backed and drops the ones that fail. It also sets
  `m_recordBackedOnDisk[S]`: the E of the kept values, intersected with the
  stamp's keys. When nothing survives or `valid` is false, that map stays
  empty.
- Nothing else changes: marker/environment handling, orphan adoption,
  `m_cacheEnvironment`.

*`discardCachedValues()`* (cpp 389): unchanged. It clears every value. Known
and unconfirmed records concern the disk, not the environment, and stay.

*`removeSession`*: erase the three maps for the id, and emit nothing (the row
is already gone).

**Crash table** (S a session, e one of its explicit calculations, V a value of
a column over e):

| Crash after | index.json on disk (V / stamp for e) | record e on disk | next start |
|---|---|---|---|
| a write, with the stamp on disk listing e absent (first fit) | V computed without e / absent | present | stamp ≠ disk: V dropped, pending until loaded |
| a delete (input change, stale on load) | V / present | absent | stamp ≠ disk: V dropped, the worker caches unavailable |
| delete, then write again before any flush | no V, e absent (step 4 flushed first) | present | V missing: pending until loaded |
| step 4's flush failed | V / present | absent (deleted before) | stamp ≠ disk: V dropped |
| a failed write (encode or I/O) | V never flushed since the failure (unconfirmed) | previous or none | missing or older values only; the check applies |
| the refresh flush (Task 4.3) | V1 / present, current version | present | valid: the stub shows V1 |
| a result-version bump (upgrade) | V / present, old version | present (stale) | version ≠ current: V dropped, pending; the load deletes the record |

**Acceptance Criteria:**
- [ ] After `flushIndex()`, every session entry has `"records"`, a JSON
      object: `{}` for a session without records, and
      `{"test.columns.y": "y-v1"}` for one with a record of that calculation.
      The root keys are unchanged (`tst_logbook_index::markerWrittenOnFlush`
      passes unchanged) (`tst_result_columns::stampWrittenOnFlush`).
- [ ] A write of (S, e) drops exactly S's cached values of columns over e (and
      of non-enabled columns), keeps the others, adds e to
      `knownCalculationRecords(S)` and emits one
      `calculationRecordsChanged(S, e)`. A removal of an absent,
      unknown record emits nothing and changes nothing
      (`managerDropsDependentValues`).
- [ ] The ordering rule: with V and `{x}` on disk, a delete followed by a
      write leaves `index.json` on disk, right after the write, with no value
      of S's X column and no `x` in S's stamp. With the stamp on disk lacking
      `x`, a write leaves `index.json`'s bytes unchanged
      (`rewriteAfterDropFlushesIndexFirst`, `crashAfterRecordWrite`).
- [ ] The start-up check drops values exactly as the summary's rule says, for
      stamped entries, unstamped entries and version mismatches
      (`crashAfterRecordWrite`, `crashAfterRecordDelete`,
      `oldIndexWithoutStamp`, `resultVersionChangeDropsCachedValue`).
- [ ] A failed write keeps the values over e out of `index.json` and e out of
      the stamp until the pair is written or removed successfully, or the row
      is evicted (`writeFailureKeepsValueOutOfIndex`).
- [ ] `tst_logbook_index`, `tst_result_records` and `tst_result_store` pass
      unchanged.

**Complexity:** L

---

### Task 4.3: Session model: compute, cache, settle, refresh

**Purpose:** Compute explicit-backed columns of loaded rows from the engine.
Settle them for stubs without loading or reading records. React to record
changes and environment changes.

**Files to modify:**
- `src/sessionmodel.h`: `SessionRow::pendingColumns`, new private members and
  methods, the `fillMissingColumns` signature, class comment (Task 4.4).
- `src/sessionmodel.cpp`: constructor, `rebuildColumns`, `removeSessions`,
  `checkCalculationEnvironment`, `rebuildColumnDependencies`,
  `invalidateColumns`, `invalidateAllColumns`, `fillMissingColumns` and its
  callers, `evictSession`, `startColumnWorker`, `processNextDirtyColumn`,
  `processNextBulkEdit`, `restoreStoredResults` (Phase 3), `computeColumnValues`.

**Technical Approach:**

1. **`SessionRow`** (h 20-39) gains:
   ```cpp
   // Explicit-backed columns of a row that is not loaded, left uncached
   // because the session has a record that only a load may read (see CACHED
   // COLUMN VALUES). Never holds an index that cachedValues holds. Empty for a
   // loaded row (except a failed-load placeholder).
   QSet<int> pendingColumns;
   ```
   It is displayed exactly like a missing value today: `data()` returns an
   invalid `QVariant` for a stub cell with no cached value (cpp 367-372), and
   `sort` puts it at the bottom. No change there.

2. **Explicit calculations per column.** Add
   `QVector<QStringList> m_columnExplicitCalculations; // parallel to m_columns: E(c)`.
   Fill it in `rebuildColumnDependencies` with
   `logbookColumnExplicitCalculations(col, registry)`. Add
   `bool isExplicitBacked(int column) const`.

3. **Work predicate.** Add
   `bool needsColumnWork(const SessionRow &row) const`: true when some index in
   `[0, m_columns.size())` is neither in `cachedValues` nor in
   `pendingColumns`. Replace the four uses of
   `cachedValues.size() < m_columns.size()`: the ColumnTask `hasWork` (cpp
   110), `removeSessions` (952), `startColumnWorker` (1766) and
   `processNextDirtyColumn` (1791). This keeps pending columns from making the
   scheduler spin.

4. **`computeColumnValues`** (cpp 2018-2066): delete the explicit branch
   (2028-2039) and its comment. It now reads the session for every column.
   Keep the header comment ("What a column stores is part of cache
   validity"). Callers decide which indices reach it (step 5).

5. **`fillMissingColumns`** gains the source of the session:
   ```cpp
   enum class ColumnSource {
       LoadedRow,      ///< the row's own session: its engine holds the restored / published results
       TemporaryLoad   ///< a session loaded for this computation only: no stored result was restored
   };
   void fillMissingColumns(int row, const SessionData &session, ColumnSource source);
   ```
   - Missing now means "neither cached nor pending".
   - `TemporaryLoad`, and `LoadedRow` when `m_rows[row].loadFailed`: first call
     `settleExplicitColumns(row)` (step 6), then compute only the columns that
     are still missing.
   - `LoadedRow` otherwise: compute every missing column from the session,
     explicit-backed ones included, reading the engine. That gives the
     restored or published value, or unavailable when the calculation is not
     requested, and it never starts one.

   Callers:
   - `rebuildColumns` (loaded rows, 174): already calls `computeColumnValues`
     directly, which counts as `LoadedRow`.
   - `flushDirtySessions` (1572), `saveNextSession` (1614), `evictSession`
     (1751), `processNextDirtyColumn` loaded branch (1805), and the bulk edit's
     loaded path (1945): `LoadedRow`.
   - `processNextDirtyColumn` stub branch (1815) and the bulk edit's stub path
     (1967): `TemporaryLoad`.

   `m_columnWorkStats.valuesComputed` also counts the values that
   `settleExplicitColumns` caches as unavailable. It does not count pending
   ones.

6. **`settleExplicitColumns(int row)`** (private):
   ```cpp
   /// For each missing explicit-backed column of a row whose engine holds no
   /// stored result (a stub, a temporary load, a failed-load placeholder):
   /// PENDING when LogbookManager knows a record of the session for any
   /// calculation the column depends on, else cached as unavailable (row and
   /// LogbookManager::updateCachedValues). Loads nothing and reads no record:
   /// the record set comes from knownCalculationRecords().
   ```
   It uses the row's current `sessionId`, which is the identity stem for an
   identity row. `m_knownRecords` is keyed by that id, and a later remap moves
   it (Task 4.2).

7. **`processNextDirtyColumn` stub branch** (1806-1822): call
   `settleExplicitColumns(dirtyIdx)` **before** loading. If
   `!needsColumnWork(row)` afterwards, do not load: decrement
   `m_columnWorkerRemaining`, emit the row's `dataChanged` as today (1827),
   and return. Otherwise temporarily load and call
   `fillMissingColumns(dirtyIdx, loaded, ColumnSource::TemporaryLoad)`. Phase
   3's comment "A temporary load: stored results are never read here" stays,
   and gains "explicit-backed columns are settled from the record set
   instead". A stub whose only missing values are explicit-backed columns is
   settled without any load.

8. **Removal of cached values also removes pending entries.**
   - `invalidateColumns` (1380): `sr.pendingColumns.remove(i)` next to
     `sr.cachedValues.remove(i)`.
   - `invalidateAllColumns` (1396): clear both.
   - `rebuildColumns`: clear `pendingColumns` for every row (indices change;
     the worker settles stubs again, without loading when only those are
     missing).
   - `checkCalculationEnvironment` (1318-1319): clear both for every row.

9. **Record-change slot.** In the constructor:
   `connect(&LogbookManager::instance(), &LogbookManager::calculationRecordsChanged, this, &SessionModel::onCalculationRecordsChanged);`
   (direct). The private method:
   ```cpp
   /// A record of (sessionId, calculationId) changed (LogbookManager has
   /// dropped its copies). Removes the row's cached and pending values of every
   /// column over that calculation; a loaded row is recomputed on the next
   /// event-loop pass (queueRecordColumnRefresh), a stub is settled by the
   /// column worker. Runs inside record methods, which the result store calls
   /// from an engine listener and from a load: it only drops state and defers.
   void onCalculationRecordsChanged(const QString &sessionId, const QString &calculationId);
   ```
   - Find the row. If there is none, return.
   - Remove indices `i` where `m_columnExplicitCalculations[i].contains(calculationId)`.
   - If nothing was removed, return. This means no wake and no queue, so
     sessions without explicit-backed columns see no change: `tst_jobmodel`,
     `tst_result_store` and `tst_fusion_store` all run with
     `{descriptionColumn()}`.
   - Loaded and not `loadFailed`: `queueRecordColumnRefresh(sessionId)`.
     Otherwise: `startColumnWorker()`.

   It emits nothing and does not touch the row list or any session.

10. **Queued refresh of loaded rows.**
    ```cpp
    QSet<QString> m_recordColumnRefresh;        // loaded rows whose explicit-backed values are to be recomputed
    bool m_recordColumnRefreshQueued = false;
    void queueRecordColumnRefresh(const QString &sessionId);   // coalesced, QueuedConnection
    void refreshRecordColumns();
    ```
    `queueRecordColumnRefresh` follows `queueInvalidation` (cpp 1251-1262):
    `QMetaObject::invokeMethod(this, &SessionModel::refreshRecordColumns, Qt::QueuedConnection)`.
    `refreshRecordColumns` swaps the set out. For every id whose row is loaded
    and not `loadFailed`, it calls `fillMissingColumns(row, session,
    ColumnSource::LoadedRow)`. It **emits no `dataChanged`**: a loaded row's
    cells are read live from the session (`data()`, cpp 356-366), and the
    publish or edit that caused the change has already told the views. If it
    computed at least one value, it calls `LogbookManager::instance().flushIndex()`
    once at the end, as the ColumnTask's completion does (cpp 114). A row that
    was evicted meanwhile is skipped: `evictSession` has completed its values.
    That is why `tst_fusion_jobs::columnShowsValueStraightAfterPublication`
    keeps passing unchanged ("told once, for that row only").

11. **Load paths.** In Phase 3's `restoreStoredResults(SessionRow &sr)`, which
    every installing path calls (sessionRef, the unloaded merge branch, the
    bulk-edit promotion):
    `const bool hadPending = !sr.pendingColumns.isEmpty(); sr.pendingColumns.clear();`
    before the restore, and `if (hadPending) queueRecordColumnRefresh(sr.sessionId);`
    after it. A stale record deleted by the restore reaches
    `onCalculationRecordsChanged` through the manager, which queues the same
    refresh. The row's existing cached values stay: they were valid for the
    records on disk, and the restore installs exactly those records.

12. **Eviction** (`evictSession`, cpp 1750-1756). After `fillMissingColumns(...,
    LoadedRow)` and before `sr.session = std::nullopt`:
    `const QStringList ids = logbook.discardUnconfirmedCalculationRecords(sessionId);`
    For each column with E ∩ ids ≠ ∅, remove the row's cached value. If any
    was removed, call `startColumnWorker()` after the row is a stub. The worker
    then settles it against the disk: pending with a previous record,
    unavailable without one.

13. **Environment change.** At the top of `checkCalculationEnvironment`
    (cpp 1301), **before** the unchanged-fingerprint early return, call
    `LogbookManager::markCalculationRecordsUnconfirmed(row.sessionId)` for
    every row that is loaded and not `loadFailed`. Comment it: "A registry
    change drops explicit results from loaded engines and keeps their records
    (they are checked at the next load), so a loaded engine can no longer
    vouch for them. Values over them stay out of index.json until the row is
    evicted or the record is written or deleted again." It must run before
    the early return, because A → B → A within one pass drops the in-memory
    results all the same. The existing discard is unchanged.

14. **Writes and removals never come from temporary loads.** Temporary
    sessions have no listener (Phase 3). No code is needed; the comment at
    the bulk edit's stub path (Phase 3) stays.

**Acceptance Criteria:**
- [ ] A loaded row caches the engine's value of an explicit-backed column:
      unavailable before any request, the published value after one
      (`unrequestedIsCachedUnavailable`, `publishedResultIsCached`).
- [ ] A record write or delete removes the row's value at once (before any
      event-loop pass). It is recomputed by the next pass without a
      `dataChanged` for the row, and the index is flushed with it
      (`publishedResultIsCached`, `inputChangeDropsCachedValue`,
      `tst_fusion_jobs::columnShowsValueStraightAfterPublication` unchanged).
- [ ] The column worker never restores and never reads a record
      (`storedResultStats().restoreCalls == 0`, `recordsRead == 0`). With a
      record and no valid value it leaves the column pending, and
      `waitForIdle` returns true. With no record it caches unavailable. It
      loads nothing when only explicit-backed columns are missing
      (`workerLeavesPendingWithRecord`, `noRecordStaysUnavailableAfterRestart`).
- [ ] After a restart an unloaded session shows the cached value with no load
      (`restartShowsCachedValueWithoutLoading`,
      `tst_fusion_jobs::columnOnFusionOutputIsCachedFromRecord`).
- [ ] A stale record surviving a bulk edit on a stub leaves the column pending,
      never computed from presence, and the load that deletes the record
      caches unavailable (`staleRecordOnLoadDropsCachedValue`).
- [ ] An environment change still discards every cached value. Afterwards a
      stub with a record is pending, and one without is unavailable
      (`environmentChangeDiscardsCachedValue`; the existing
      `tst_column_cache` environment tests pass unchanged).
- [ ] Only columns over the changed calculation are dropped
      (`onlyDependentColumnsDrop`).

**Complexity:** L

---

### Task 4.4: Comments that state the old behaviour

**Purpose:** Code comments must describe the new contract. The documents under
`docs/` are Phase 5's.

**Files to modify:**
- `src/sessionmodel.h`:
  - **CACHED COLUMN VALUES** (h 61-75). Replace the sentences at 70-74 ("A
    column that depends on an explicitly requested calculation ... does not
    touch the cache.") with this meaning:
    > A column that depends on an explicitly requested calculation
    > (logbookColumnExplicitCalculations() is not empty) is computed like any
    > other for a LOADED row: from the engine, which holds the stored result
    > restored at load or the one just published, and reads unavailable when
    > the calculation is not requested. Its value is valid only with the
    > session's record set. LogbookManager stamps it in index.json and drops
    > it whenever a record of one of those calculations is written or deleted
    > (calculationRecordsChanged). The row then loses it at once and gets it
    > back on the next event-loop pass (refreshRecordColumns, which emits
    > nothing: loaded cells are live). A row that is NOT loaded has no stored
    > result in any engine, so its value is never computed from a temporary
    > load. Without a record it is cached as unavailable. With one it stays
    > PENDING (SessionRow::pendingColumns: not cached, shown empty) until the
    > session is loaded. The column worker never reads a record.
  - `publishCalculationInvalidation` (h 271-279). After Phase 3's addition,
    replace "no cached column is invalidated" with this meaning: "publishing
    invalidates no cached column by itself; the record written at the install
    drops the explicit-backed values of the session through
    LogbookManager::calculationRecordsChanged".
  - `ColumnWorkStats::valuesComputed`: "column values computed or settled
    (unavailable) by column maintenance; pending columns are not counted".
- `src/logbookmanager.h` class comment:
  - The root/entry description (h 23-26): per session `"records"` (the stamp),
    as documented in this phase's summary.
  - The CACHE VALIDITY paragraph: add the record-stamp rule, including old
    entries without a stamp.
  - A new **RECORD STAMPS** paragraph: the known/unconfirmed sets, the
    ordering rule ("a write whose calculation index.json on disk lists as
    present under a value flushes the index first; a removal never needs
    to"), and a pointer to the crash table in the .cpp. Put the table itself
    above `writeCalculationRecord` in the .cpp, next to the save-ordering
    table style (cpp 575-595).
  - Phase 2's records paragraph: "never referenced from index.json" becomes
    "referenced only by the per-session record stamp".
- `src/engine/calculationregistry.h`: the `dependsOnExplicit` comment (Task 4.1).
- `tests/tst_fusion_jobs.cpp` header comment (lines 2-4): "the rule that a
  logbook column over a fusion output is never cached" becomes "a logbook
  column over a fusion output is cached from the stored result and follows
  its record".

**Acceptance Criteria:**
- [ ] `git grep -n "cached as present and invalid\|explicit results are never saved\|An explicit result is never persisted" -- src tests`
      returns nothing.
- [ ] The sessionmodel files contain neither `fusion/` nor `Fusion::` (audit
      "nobody but the application references the fusion library"), and the
      cleanup audit passes.

**Complexity:** S

---

### Task 4.5: Rewrite and rename the two tests of the old behaviour; probe

**Purpose:** The existing tests state "never cached". Turn them into tests of
the new rule, under names that say so.

**Files to modify:**
- `tests/support/logbookprobe.h` / `.cpp`: under "index.json", mirroring
  `indexValue`:
  ```cpp
  /// The "records" stamp index.json holds for a session: an object, or
  /// Undefined when the entry has none (older index) or there is no entry.
  QJsonValue indexRecordStamp(const QJsonObject &root, const QString &sessionId);
  QJsonValue indexRecordStamp(const QString &sessionId);   // reads index.json from disk
  ```
- `tests/tst_column_cache.cpp`: `explicitBackedColumnIsNeverCached` (1278)
  becomes **`explicitBackedColumnFollowsItsResult`** (slot declaration at 151
  too).
- `tests/tst_fusion_jobs.cpp`: `columnOnFusionOutputIsNotCached` (510)
  becomes **`columnOnFusionOutputIsCachedFromRecord`** (slot at 94).

**Technical Approach:**

*`tst_column_cache::explicitBackedColumnFollowsItsResult`*. Keep the setup
(registration of `test.explicit` with the scope guard, reopen and initialize,
columns `{m_d, m_g, m_e, explicitColumn}`, `startWithLoadedSessions({gyroSession()})`).
The comment becomes: "A column over an explicit result is cached from the
result the session has, restored or published, and follows its record: written
→ the value, dropped → unavailable. After a restart the stub shows the cached
value without a load." Steps and checks:
1. Never requested: `cachedValues` contains kX, invalid. `indexValue(root,
   "g1", explicitColumn).isNull()`. `indexRecordStamp("g1") == QJsonObject()`.
   `runCount(id) == 0`.
2. `request(id).status == Ok`. Before any event-loop pass:
   `!rowAt(0).cachedValues.contains(kX)`. Then `waitForIdle`: `cached(0, kX)
   == "computed"`, `indexValue(...) == "computed"`,
   `indexRecordStamp("g1") == QJsonObject{{"test.explicit", ""}}`.
3. `updateAttribute("g1", "_DESCRIPTION", "second")` (an input): kX is gone at
   once, and the record file is gone (Phase 3). `waitForIdle`: kX cached
   invalid, index value null, stamp `{}`, `indexValue(root, "g1", m_d) ==
   "second"`.
4. `request(id)` again, then `waitForIdle`: `"computed"` cached and in the
   index. `runCount(id) == 2`. The loaded row displays `"computed"`.
5. `restartAsStubs()`: `cached(0, kX) == "computed"`, and the stub cell displays
   `"computed"`. `resetColumnWorkStats()`, `startColumnWorker()`,
   `waitForIdle`: `sessionsLoaded == 0`, `valuesComputed == 0`, and the row is
   not loaded.

*`tst_fusion_jobs::columnOnFusionOutputIsCachedFromRecord`* (`coarse_linear`,
session `a`). This is the spec §8 fusion item. One fit. Add a file-local
`restart()` helper: reset queue and model (in that order), `reopenLogbook()`,
`initialize()`, a new model with `populateFromIndex(logbook.cachedColumnValues(enabledColumns()), lastAccessedMap())`,
and a new `JobQueue`. Steps and checks:
1. `addSessions`. The roll column is cached invalid, `indexValue("a", column)`
   is null, and `indexRecordStamp("a") == QJsonObject()`.
2. Fit through the queue (`Succeeded`), then `waitForIdle`.
   - `cachedRoll().value(kRollColumn)` is a double, and `isNear` to
     `session("a").getAttribute(fusionRollAtExit()).toDouble()`.
   - `indexValue("a", column).isDouble()`, with the same value (`isNear`).
   - `indexRecordStamp("a") == QJsonObject{{"builtin.fusion.fit", "batch-temperature-bias-v3"}}`
     (literals).
3. Marker edit (`_EXIT_TIME` + .25, not a fit input): the column is dropped at
   once (static dependency), then recomputed with the fit still published
   (`waitForIdle`). The cached value is a double equal to the new live one,
   `runCount(kFit) == 1`, the index holds the new double, and the stamp is
   unchanged.
4. Note the cached double R. Call `restart()`. Row `a` is a stub, its cached
   roll `isNear` R, and the cell shows a number. `resetColumnWorkStats()`,
   `resetStoredResultStats()`, `startColumnWorker()`, `waitForIdle`:
   `sessionsLoaded == 0`, `storedResultStats().restoreCalls == 0`, and row
   `a` is still a stub.
5. `session("a")` loads it: the fit is restored (`resultStatus == Ok`,
   `runCount == 0`). Then `updateAttribute("a", "_LOCAL_ORIGIN_INDEX",
   qlonglong(4))`. The record file is gone and the column is dropped at once.
   `waitForIdle`: the cached roll is present and invalid, the index value is
   not a double, and the stamp is `{}`.

Update the test's comment accordingly. `readersNeverStartAFit` and
`columnShowsValueStraightAfterPublication` stay unchanged and must pass.

**Acceptance Criteria:**
- [ ] Neither old name remains in `tests/`. Both new functions pass. The
      fusion one also passes as `tst_fusion_jobs_exact` where exact tests are
      registered.
- [ ] `indexRecordStamp` exists in `logbookprobe.h` with a one-line comment.
- [ ] `tst_fusion_jobs` stays within its 600 s timeout (one extra fit at most
      compared with before: the rewritten test still fits once).

**Complexity:** M

---

### Task 4.6: Core tests with synthetic explicit calculations: `tst_result_columns`

**Purpose:** Pin every rule of Tasks 4.2-4.3 quickly and on every platform,
including crash points and old indexes, which fusion cannot reach cheaply.

**Files to create:**
- `tests/tst_result_columns.cpp`: class `ResultColumnsTest`,
  `FLYSIGHT_TEST_MAIN(ResultColumnsTest)`, `#include "tst_result_columns.moc"`.

**Files to modify:**
- `tests/CMakeLists.txt`: directly after Phase 3's `tst_result_store` line:
  `flysight_add_test(tst_result_columns SOURCES tst_result_columns.cpp)`.
- `tests/README.md`: one catalogue row next to `tst_result_store`: "Logbook
  columns over explicit results: cached from the restored or published
  result with a record stamp in index.json, dropped when a record is written
  or deleted, pending for unloaded sessions with a record, crash points, old
  indexes, write failures." Nothing else in that file (Phase 5).

**Technical Approach:**

*Fixture.* Follow `tst_column_cache` (fixture, `restartAsStubs`) and
`tst_result_store` (queue and cleanup discipline).
- `initTestCase`: `registerBuiltIns()`,
  `registerPreference(LogbookColumnsVersion, 0)`,
  `qRegisterMetaType<DependencyKey>()`. Register two explicit calculations on
  the global registry. They stay registered for the process, and
  `cleanupTestCase` unregisters them. Registering here puts them before every
  `initialize()`. Put the literals in the comment table:

  | Id | Inputs | Output | resultVersion |
  |----|--------|--------|---------------|
  | `test.columns.x` | attr `_DESCRIPTION` | `X_OUT = "x:" + _DESCRIPTION` (QString) | `""` |
  | `test.columns.y` | attr `Y_IN` | `Y_OUT = Y_IN * 2` (double) | `"y-v1"` |

  Columns: `{descriptionColumn(), xColumn, yColumn}`, where `xColumn` and
  `yColumn` are `SessionAttribute` columns over `X_OUT` and `Y_OUT`. The
  indices are kD = 0, kX = 1, kY = 2.
- `init`: `useFreshLogbook()`, `resetPreferencesToDefaults()`, `initialize()`,
  note `m_registryBefore`, then model, then
  `mergeSessions({columnSession("s1","d1",3.0), columnSession("s2","d2",5.0)})`,
  `QVERIFY(waitForIdle(*m_model))`, then queue. `columnSession` is
  tst_column_cache's `gyroSession` shape (SESSION_ID, DEVICE_ID, the four
  backfilled attributes, TIME/IMU source data, so that the file loads) plus
  `_DESCRIPTION` and `Y_IN`.
- `cleanup`: queue shutdown and reset, model reset, then compare the
  registry, `enrolledEngineCount() == 0`, `LogbookCacheSize` back to 50.
- Helpers (file-local, returning `QString` or values, tests/README §8):
  - `session(id)`, `engine(id)`, `row(id)`, `cached(id, col)`;
  - `recordPath(id, encoded)` with the literal encoded ids
    `"test%2Ecolumns%2Ex"` and `"test%2Ecolumns%2Ey"`;
  - `evict(ids)` (as in Phase 3's tst_result_store);
  - `restart()` (reset queue and model, `reopenLogbook()`, `initialize()`, new
    model via `populateFromIndex`, new queue);
  - `crash()`: the same, but with **no event-loop pass** after the last
    action. The model's destructor saves and flushes nothing, and a queued
    refresh dies with the model;
  - `editIndex(std::function<void(QJsonObject &)>)`: `readIndex`, mutate,
    `writeIndex`. Used only while no model exists.

*Test functions and acceptance criteria.*

1. `columnExplicitCalculations`: `logbookColumnExplicitCalculations` gives
   `{}` for `descriptionColumn()` and `gyroColumn()`, `{"test.columns.x"}`
   for xColumn, and `{"test.columns.y"}` for yColumn.
2. `unrequestedIsCachedUnavailable`: for s1 and s2, kX and kY are cached and
   invalid. The index values are null, the stamps are `{}`, and
   `calculationRecordFiles()` is empty. `runCount` is 0 for both
   calculations.
3. `stampWrittenOnFlush`: request `test.columns.y` on s1, then `waitForIdle`.
   `indexRecordStamp("s1") == {"test.columns.y":"y-v1"}` and
   `indexRecordStamp("s2") == {}`. `readIndex().keys()` still holds the four
   root fields.
4. `publishedResultIsCached_data` / `publishedResultIsCached`: rows `request`
   (`engine("s1").request`) and `queue` (`m_queue->request` + `waitIdle`) of
   `test.columns.y`.
   - Right after the request (row `request`) or in a `jobFinished` slot (row
     `queue`): kY is not in s1's `cachedValues`, kX is still cached, and s2
     is untouched.
   - After `waitForIdle`: `cached("s1", kY) == 6.0`, `indexValue` 6.0, the
     stamp `{"test.columns.y":"y-v1"}`, and `columnWorkStats().sessionsLoaded == 0`.
   - "No `dataChanged` from the refresh" is not asserted here, because
     `waitForIdle` wakes the column worker, which may reach the row first. The
     fusion test `columnShowsValueStraightAfterPublication`, where nothing
     wakes the worker, pins it.
5. `restartShowsCachedValueWithoutLoading`: fit Y on s1, `waitForIdle`, note
   the `index.json` bytes, then `restart()`.
   - s1 is a stub, `cached("s1", kY) == 6.0`, and the cell displays `"6"`.
   - `startColumnWorker()` + `waitForIdle`: `sessionsLoaded == 0`,
     `valuesComputed == 0`, `storedResultStats().recordsRead == 0`, and the
     `index.json` bytes are unchanged.
6. `noRecordStaysUnavailableAfterRestart`: `restart()` with no request.
   Every kX / kY cached value is present and invalid, and the worker loads
   nothing.
7. `inputChangeDropsCachedValue`: fit Y on s1, `waitForIdle`, then
   `updateAttribute("s1","Y_IN",4.0)`.
   - Before any event-loop pass: the Y record file is gone, and kY is neither
     cached nor pending.
   - `waitForIdle`: kY is cached invalid, the index value is null, and the
     stamp is `{}`.
8. `onlyDependentColumnsDrop`: fit X and Y on s1, `waitForIdle`, then
   `updateAttribute("s1","Y_IN",7.0)`. kX is still cached `"x:d1"` before and
   after `waitForIdle`, the X record keeps its bytes, and the stamp is
   `{"test.columns.x":""}`.
9. `staleRecordOnLoadDropsCachedValue`: fit X on s1, `waitForIdle` (kX
   `"x:d1"`), `evict({"s1"})`, then `startBulkEdit({row("s1")}, kD, "bulk")`
   and `waitForIdle` (a bulk edit on the stub; `_DESCRIPTION` is the input of
   X).
   - s1 is still a stub. kX is not cached and `pendingColumns == {kX}`. The
     X record file still exists (it is stale: Phase 3 hand-off).
     `restoreCalls == 0`, `recordsRead == 0`, and `indexValue` for kX is
     undefined.
   - `session("s1")`: `staleRecordsDeleted == 1` and the file is gone. After
     `waitForIdle`, kX is cached invalid, `pendingColumns` is empty, and the
     index value is null.
10. `workerLeavesPendingWithRecord`: fit X on s1, `waitForIdle`, reset queue
    and model, then `editIndex` removes s1's kX value, then `restart()`.
    - `startColumnWorker()` + `waitForIdle` returns true (no spin).
      `pendingColumns("s1") == {kX}`, `sessionsLoaded == 0`,
      `restoreCalls == 0`, and the kX cell displays nothing.
    - Then `session("s1")` + `waitForIdle`: kX is `"x:d1"`, cached and in
      the index.
    - Second part (a stub with a record and another missing column): repeat
      with s1's kD value removed as well. The worker loads s1 once
      (`sessionsLoaded == 1`), caches kD `"d1"`, and leaves kX pending.
11. `crashAfterRecordWrite`: note the `index.json` bytes. Then
    `engine("s1").request("test.columns.x")` and `crash()`.
    - The `index.json` bytes before the crash equal the noted ones (no
      pre-write flush: the stamp on disk did not list `x`).
    - After the crash: `cachedValuesForSession("s1")` has no kX key, while s2
      keeps its null kX. After `waitForIdle`, s1's kX is pending and
      `sessionsLoaded == 0`.
    - `session("s1")` + `waitForIdle`: kX is `"x:d1"`.
12. `crashAfterRecordDelete`: fit X on s1, `waitForIdle` (the index holds
    `"x:d1"` and `{"test.columns.x":""}`). Then
    `updateAttribute("s1","_DESCRIPTION","e")` (the record is deleted) and
    `crash()`.
    - After the crash, s1 has no kX value (stamp present, disk absent).
    - After `waitForIdle`: kX is cached invalid with `sessionsLoaded == 0`,
      and the csv still says `_DESCRIPTION,d1` (the edit was not saved).
    - `session("s1")`: `resultStatus("test.columns.x")` is not Ok (no
      record), so the value matches a reload.
13. `rewriteAfterDropFlushesIndexFirst`: fit X on s1, `waitForIdle`. Then,
    with no event-loop pass in between: `updateAttribute("s1","_DESCRIPTION","e")`
    (delete), then `engine("s1").request("test.columns.x")` (write, `Ok`).
    - `readIndex()` right after: s1 has no kX value and no `x` in its stamp,
      and the X record file exists.
    - `crash()`: kX is pending after the worker. `session("s1")` deletes the
      record as stale (the csv still says `d1`), and kX is cached invalid.
14. `oldIndexWithoutStamp_data` / `oldIndexWithoutStamp`: rows `noRecord` and
    `withRecord` (fit X on s1 first). `waitForIdle`, reset queue and model,
    then `editIndex`: remove `"records"` from every entry and set s1's and
    s2's kX values to `null` (as an older build wrote them). Then `restart()`.
    - `noRecord`: s1's kX is cached null, and the worker loads nothing.
    - `withRecord`: s1 has no kX value. The worker leaves it pending with
      `sessionsLoaded == 0`. After `session("s1")` it is `"x:d1"`.
    - Both rows: s2's kX stays cached null. After the next flush every entry
      has `"records"`.
15. `resultVersionChangeDropsCachedValue`: fit Y on s1, `waitForIdle`, reset
    queue and model, then `editIndex` sets s1's stamp to
    `{"test.columns.y":"y-v0"}`. Then `restart()`.
    - s1 has no kY value; after the worker it is pending. s2's kY stays null.
    - `session("s1")` restores the record (its version equals the current
      `y-v1`) and kY becomes 6.0.
16. `writeFailureKeepsValueOutOfIndex`: `QDir().mkdir(recordPath("s1", y))`,
    then request Y on s1 inside a `WarningCapture`, then `waitForIdle` and
    `flushIndex()`.
    - The result is Ok and the loaded cell shows `"6"`.
    - `unconfirmedCalculationRecords("s1") == {"test.columns.y"}`.
    - The index has no kY value for s1, and its stamp has no `test.columns.y`.
    - Then `evict({"s1"})` + `waitForIdle`: kY is cached invalid (no record
      on disk), the index value is null, and the unconfirmed set is empty.
    - A scope guard removes the directory.
17. `recordBeforeFirstSaveIsCachedAfterSave`:
    `mergeSessions({columnSession("n1","dn",3.0)})`, then, in the same pass,
    request Y on n1 inside a `WarningCapture` (zero warnings).
    - `flushIndex()`: the index has no `n1` entry.
    - `waitForIdle` (saved): the index has n1's kY 6.0 and the stamp
      `{"test.columns.y":"y-v1"}`.
    - `restart()`: n1 is a stub with kY 6.0 and `sessionsLoaded == 0`.
18. `environmentChangeDiscardsCachedValue`: fit Y on s1, `waitForIdle`,
    `evict({"s1"})`. Register an OnDemand `test.columns.extra` (no inputs,
    output attr `_COLUMNS_EXTRA`). A scope guard resets queue and model, then
    unregisters it. Then `flushPendingInvalidations()` and `waitForIdle`.
    - Every row lost its values and got them back: s1's kY is pending, its kD
      is `"d1"` (one temporary load), and s2's kY is null.
    - The index has no kY value for s1.
19. `environmentChangeUnconfirmsLoadedRows`: fit Y on s1 (loaded), then
    register and unregister `test.columns.extra` (the fingerprint returns to
    the original) and `flushPendingInvalidations()`.
    - `unconfirmedCalculationRecords("s1") == {"test.columns.y"}`, and the
      next `flushIndex()` writes no kY value for s1.
    - `evict({"s1"})`, `waitForIdle`, `session("s1")`: the record restores
      and kY is 6.0, cached and flushed.
20. `managerDropsDependentValues` (manager level, the model reset first):
    `setCachedValues("s1", {xColumn: "v", descriptionColumn(): "d"})`, then
    write a stamped record of `test.columns.x` for s1 (from
    `exportResult` of a loaded copy, as in Phase 3's `alreadyInstalledIsKept`)
    under a `QSignalSpy` on `calculationRecordsChanged`.
    - `cachedValuesForSession("s1")` keeps only the description's key.
    - The spy has exactly `("s1","test.columns.x")`.
    - `knownCalculationRecords("s1") == {"test.columns.x"}`.
    - `removeCalculationRecord("s1","test.columns.y")` (absent, unknown):
      the spy count is unchanged and it returns true.
21. `deletingSessionRemovesStamp`: fit X on s1, `waitForIdle`, then the main
    window's delete sequence (`removeSessions`, `removeSession`,
    `flushIndex`). The index has no `s1` entry, the X record file is gone,
    and s2's values and stamp keep their content.

Expected values are literals (`6.0`, `"x:d1"`, `"y-v1"`, file names), never
recomputed with the code under test. Any registration a test adds is undone
before `cleanup()` compares the registry.

**Acceptance Criteria:**
- [ ] `tst_result_columns` builds, has label `core`, and passes. Every function
      above exists with the checks stated.
- [ ] No test depends on permission bits, case sensitivity or directory
      iteration order. The write-failure test uses a directory at the record
      path.
- [ ] The test leaves the global registry as it found it (after
      `cleanupTestCase`) and no engine enrolled.

**Complexity:** L

---

### Task 4.7: Verification

**Purpose:** Build and run the whole suite.

**Technical Approach:** Use the commands under Testing Requirements. Report any
existing test that had to change beyond the two renamed ones.

**Acceptance Criteria:**
- [ ] `cmake --build build-phase1 --config Release` succeeds.
- [ ] `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure`
      passes, with `-L audit` and `-L fusion` included.

**Complexity:** S

## Testing Requirements

### Unit Tests
- New `tst_calcregistry::explicitDependencies` (Task 4.1).
- New `tst_result_columns` (Task 4.6): the stamp, the drop on write and
  delete, the queued refresh, pending and unavailable on stubs, crash points,
  old indexes, result-version change, write failure, unsaved sessions,
  environment changes.
- Rewritten and renamed: `tst_column_cache::explicitBackedColumnFollowsItsResult`
  (Task 4.5).

### Integration Tests
- Rewritten and renamed:
  `tst_fusion_jobs::columnOnFusionOutputIsCachedFromRecord`, also run as
  `_exact`. Spec §8 bullet "Logbook column over Fusion/roll: cached from a
  valid record, `unavailable` without one, invalidated when the record is
  dropped" maps to:

  | Clause | Test |
  |---|---|
  | cached from a valid record (and shown after a restart without a load) | `tst_fusion_jobs::columnOnFusionOutputIsCachedFromRecord` (steps 2, 4); core: `publishedResultIsCached`, `restartShowsCachedValueWithoutLoading` |
  | `unavailable` without one | `columnOnFusionOutputIsCachedFromRecord` (step 1); core: `unrequestedIsCachedUnavailable`, `noRecordStaysUnavailableAfterRestart` |
  | invalidated when the record is dropped: input change | `columnOnFusionOutputIsCachedFromRecord` (step 5); core: `inputChangeDropsCachedValue`, `onlyDependentColumnsDrop` |
  | ... stale on load | core: `staleRecordOnLoadDropsCachedValue`, `resultVersionChangeDropsCachedValue` |
  | ... session delete | core: `deletingSessionRemovesStamp` |
  | crash consistency | core: `crashAfterRecordWrite`, `crashAfterRecordDelete`, `rewriteAfterDropFlushesIndexFirst` |
  | old index compatibility | core: `oldIndexWithoutStamp` |

- These must pass unchanged: `tst_fusion_jobs::columnShowsValueStraightAfterPublication`
  and `readersNeverStartAFit`, `tst_column_cache` (every other function,
  especially `upgradeDiscardsAndRecomputes`, `interruptedSaveViaModel` and
  the environment tests), `tst_logbook_index`, `tst_result_records`,
  `tst_result_store`, `tst_fusion_store`, `tst_jobmodel` (Phase 3's
  `nothingIsPersisted`: no explicit-backed column is enabled, so no value is
  dropped, no refresh runs, and the index is not rewritten),
  `tst_session_model_engine`, `tst_import_merge`, `tst_plot_requests`,
  `tst_python_bridge` and `tst_workflow`.

### Verification commands (Michael's machine)
- Build: `cmake --build build-phase1 --config Release`. **Never build
  `build/`**: it has third-party ON and would overwrite the Boost-enabled
  solver install.
- Focused: `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure -R "tst_result_columns|tst_column_cache|tst_calcregistry|tst_logbook_index|tst_result_store|tst_result_records|tst_jobmodel|tst_session_model_engine"`
- Fusion: `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure -L fusion`
- Audit: `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure -L audit`
- Full: `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure`
  (fusion tests have a 600 s timeout each).

### Manual Verification
1. Start the application from `build-phase1`. Enable a logbook column over a
   sensor fusion output (for example roll at the exit marker). Show a track
   with IMU data, check a fusion plot, press refresh. When the job ends the
   cell shows a number.
2. Hide the track, set the cache size small enough to evict it, and restart.
   The cell of the unloaded track shows the same number, and nothing loads
   (no progress in the status bar).
3. Show the track and edit `_LOCAL_ORIGIN_INDEX`. The cell empties and the
   record file disappears.
4. With a record present, delete the track's value for that column from
   `index.json` by hand, with the application closed. After a restart the
   cell stays empty (pending) and the worker does not load the track. Showing
   it fills the cell.

## Notes for Implementer

### Gotchas
- **Pending must not make the worker spin.** Every "row needs column work"
  test goes through `needsColumnWork()`. A stray
  `cachedValues.size() < m_columns.size()` turns a pending column into an
  endless ColumnTask.
- **Never compute an explicit-backed column from a temporary load**, not even
  "unavailable" when a record exists: the engine of a temporary session holds
  no stored result, so its answer says nothing about the record. Presence of a
  record means pending, never a value.
- **The record-change slot runs inside engine listeners and loads.** It may
  only remove entries from `cachedValues` / `pendingColumns`, queue the
  refresh and wake the scheduler. It must not read a session, emit, or change
  the row list. The partial state of a merge (`SessionMerge::apply` in
  progress) is exactly what it would otherwise read.
- **The refresh emits nothing.** A loaded row's cells are live. An extra
  `dataChanged` breaks `columnShowsValueStraightAfterPublication` ("told once").
- **Encode before any flush in `writeCalculationRecord`.** A refused
  encoding must not flush `index.json` (Phase 3 expects exactly one warning).
- **`dropRecordDependentValues` sets `m_indexNeedsFlush` only when it removed
  something.** A record write in a logbook without explicit-backed columns
  must not cause an index rewrite (`tst_jobmodel::nothingIsPersisted`
  compares the logbook folder's bytes, and every flush writes fresh column
  ids).
- **`m_recordBackedOnDisk` must describe the file on disk.** Update it only
  after a committed flush and at `initialize()`, never on a failed flush.
- **Unconfirmed is cleared only by a successful write or removal of the
  pair, by eviction (`discardUnconfirmedCalculationRecords`), by
  `removeSession` and by `reset()`.** A new load does not clear it: a loaded
  row is never evicted-and-reloaded without passing through eviction.
- **Audit spellings.** `logbookcolumn.cpp` must not spell
  `EvaluationPolicy::Explicit` ("one authority"). `src/sessionmodel.*`
  comments must not contain `fusion/` or `Fusion::`. `emit dependencyChanged`
  stays on one line of `sessionmodel.cpp` (nothing here emits it).
- **Registrations in tests.** `tst_result_columns` registers in
  `initTestCase`, before any `initialize()`. Test 18/19 registrations are made
  while a model lives and are undone after it is destroyed (tests/README §8).
- **Doubles through `index.json`.** Compare cached fusion doubles with
  `isNear`. Synthetic values (6.0) are exact.

### Decisions Made
- **The stamp is presence + result version per record, per session**
  (`"records"` in each entry), not file metadata. Modification times and
  sizes can coincide on coarse file systems. Presence flips are always
  visible, and the one invisible sequence (delete then write of the same
  pair before any flush) is removed by construction: a flush comes before
  such a write. The rule follows the existing SAVE ORDERING idea (flush
  before a mutation when the on-disk index holds a value that depends on it).
- **The result version is in the stamp** because it is not in the
  environment fingerprint (Phase 1). Without it, a fusion algorithm bump
  would leave old column values on stubs until each session loads.
- **Known records come from one names-only listing at `initialize()`**, kept
  by the manager's own mutations. The worker asks
  `knownCalculationRecords()`, never the directory (per row that would be
  O(N²)), and never opens a record. External changes to the folder are seen
  at the next start. That is the same trust model as every other cached
  column, whose session file could also be edited behind the application's
  back.
- **Validity lives in `LogbookManager`**, following its "CACHE VALIDITY is
  decided here and nowhere else". The model only reacts to
  `calculationRecordsChanged`.
- **"Recompute at once"** means: the stale value is removed synchronously in
  the record method, and the new one is computed on the next event-loop pass
  (`refreshRecordColumns`), before any idle work. The first moment would be
  inside an engine listener or a merge, where a session read is not safe.
- **Unconfirmed records** cover a write or removal that failed and an
  environment change while loaded: the three ways in which a loaded engine
  and the records on disk can differ. Values over them are never flushed.
  Eviction discards them, and the stub is settled against the disk.
- **Unsaved (reserved) sessions:** the record is written under the reserved
  stem, and the manager knows it under the session id. The loaded row caches
  the value in memory. `index.json` does not list the session
  (`m_unsavedAll` and not in `m_sessionIdToUuid`) until the first save, after
  which the next flush writes the value with its stamp. A crash before the
  save: the record is a stray and the session is gone. A crash after the csv
  commit but before the flush: orphan adoption, known records under the
  identity id, and the explicit column pending until loaded.
- **Test placement:** synthetic mechanics in a new core target
  `tst_result_columns`. The two existing tests are renamed in place. The
  single fusion end-to-end test is the renamed `tst_fusion_jobs` test,
  because that file already enables the roll column. `tst_fusion_store` keeps
  `{descriptionColumn()}`.

### Open Questions
- None blocking.

### Deviations / questions for the coordinator
1. **Phase 2 statements superseded.** Phase 2's class-comment paragraph says
   records are "never referenced from index.json". Its gotcha says "Do not
   flush the index from any record method". Phase 4 references records from
   the per-session stamp, and a write may flush the index first (ordering
   rule). Both are deliberate. The flush keeps the save-ordering invariant,
   because `flushIndex()` still omits unsaved columns.
2. **The engine layer changes in Phase 4:** `CalculationRegistry` gains
   `explicitDependencies()` and `dependsOnExplicit()` is re-expressed through
   it. The audit comment at `tests/audit/cleanup_audit.cmake` 387-388 ("plot
   rows and the logbook column cache ask it") should be updated by Phase 5.
3. **Environment changes make loaded rows' records unconfirmed** (Task 4.3
   step 13). This is needed because Phase 3 keeps records across registry
   changes while the in-memory results are dropped. After an environment
   change, a loaded row's fusion column value reaches `index.json` again only
   after the row is evicted and reloaded, or re-fitted. Every environment
   change already discards all cached values, so users see no difference on
   stubs (pending until loaded).
4. **A stale record behind a valid stamp.** The stamp checks presence and
   result version, not contents. A record made stale by an edit the
   application did not make (a hand-edited csv) keeps its column value on the
   stub until the session loads, and the load then deletes the record and
   caches unavailable. Every edit the application makes invalidates the
   column through its static dependencies first (the fit's leaves are in the
   column's static closure), so in-application paths never reach this.
5. **Pre-existing, not changed:** the column worker fills the columns of a
   failed-load placeholder row from its empty session
   (`processNextDirtyColumn`, loaded branch). This phase only makes such a
   row's explicit-backed columns settle like a stub's. Whether placeholders
   should cache anything at all is outside this plan.
6. **Hand-off to Phase 5:**
   - `docs/DATA_SCHEMA.md` §11: replace 314-317 ("cached as unavailable"),
     describe `"records"` and the start-up check.
   - `docs/CALCULATIONS.md` §17: 1139-1146, and the test name at 1160 →
     `tst_column_cache::explicitBackedColumnFollowsItsResult`.
   - `tests/README.md` §8 probe list: add `indexRecordStamp`.
   - The acceptance map: add the spec §8 column item with the tests of the
     table above.
   - The audit comment of item 2.

## Definition of Done

This phase is complete when:
1. Every task's acceptance criteria pass.
2. `cmake --build build-phase1 --config Release` succeeds, and the full
   `ctest --test-dir build-phase1/FlySightViewer-build -C Release` passes,
   audit, fusion and exact (where registered) included.
3. The code follows the patterns of the reference files: queued, coalesced
   work as in `queueInvalidation`; ordering tables as in `saveSession`; test
   fixtures of `tst_column_cache` and `tst_result_store`; `tests/README.md` §8
   conventions.
4. No TODOs or placeholder code remain. The only files changed are those named
   in the tasks, and `PLANS/`, `experiments/` and `build*/` are untouched.
