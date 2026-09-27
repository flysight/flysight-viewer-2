# Phase 3: Store on publish, restore on load

## Overview

This phase connects Phase 1's engine API and Phase 2's record files to the
session model. When an Explicit result is installed with status `Ok`, a record
is written beside the session file. When an input change drops the result,
the record is deleted. Every path that installs a session into a model row
restores that session's valid records into its engine before any reader can
see the row, and deletes the stale ones. A restore is not a request: nothing
starts, and plot rows and blocker inspection see a restored result exactly as
they see a fresh publish. This phase also adds the end-to-end tests of spec
section 8, except the logbook-column items (Phase 4).

## Dependencies

- **Depends on:**
  - Phase 1 (Engine snapshot and restore): `StoredCalculationResult`,
    `sameContent`, `CalculationEngine::exportResult`, `restoreResult` /
    `RestoreOutcome` (`NotFound`, `NotExplicit`, `AlreadyInstalled`, `Stale`
    with `StaleCheck`, `Restored`, and `invalidated`),
    `setExplicitResultListener` / `ExplicitResultEvent` (`Installed`,
    `DroppedByInputChange`), and `CalculationDescriptor::resultVersion`.
  - Phase 2 (Record files in the logbook): `CalculationRecord` (`stamped()`,
    `stampsAreCurrent()`), `CalculationRecordStatus`, `CalculationRecordRead`,
    `LogbookManager::writeCalculationRecord` / `readCalculationRecord` /
    `calculationRecordIds` / `removeCalculationRecord(s)`, `removeSession`
    deleting records, stray removal in `initialize()`, and the probes
    `calculationRecordFiles()` / `sessionFileStem()` in `tests/support/logbookprobe.h`.
  - Both phases are documented but not yet implemented. This document uses
    their documented names exactly. Phase 1 declares `RestoreOutcome` and
    `ExplicitResultEvent` in `src/engine/calculationengine.h`. Qualify them as
    that header does (`PrepareOutcome`, the pattern they follow, is nested in
    `CalculationEngine`).
- **Blocks:** Phase 4 (logbook columns over requested results), Phase 5
  (documentation, acceptance map, audit).
- **Assumptions:**
  - Phase 1's listener semantics hold:
    - `Installed` is reported for every requested install of an Explicit
      calculation, on both the synchronous `request()` path and the job
      queue's `publishPrepared()` path, whatever the status.
    - `DroppedByInputChange` is reported only for leaf notifications,
      preference broadcasts and `dropNotRequested` cascades. It is never
      reported for `clear()`, a registry change, or the destruction of the
      engine or registry.
    - The listener is never called for `restoreResult()`'s own install.
    - The listener travels with the engine when a `SessionData` is moved.
    - The listener may call `exportResult()`.
  - Phase 2's `calculationRecordIds(sessionId)` returns ids sorted and reads
    only file names. `readCalculationRecord` never deletes. The file name of a
    record is `<stem>.<encoded id>.fvresult`, with every byte outside
    `[a-z0-9_-]` percent-encoded in upper-case hex. So `expA` is
    `exp%41`, `builtin.fusion.fit` is `builtin%2Efusion%2Efit`, and
    `test.store.up` is `test%2Estore%2Eup`.
  - File stems never change. `remapSessionId` keeps the uuid (Phase 2, Task 2.4).
  - A session that an import has just created has **no** stem today. Its uuid
    is created only inside `saveSession` (logbookmanager.cpp 618-624). The
    model's created branch (sessionmodel.cpp 600-616) and
    `markSessionUnsaved` never add it to `m_sessionIdToUuid`. Task 3.1 adds
    a stem reservation so that such a session can have records before its
    first save.

## Tasks

### Task 3.1: The result store (write, delete, restore) and a stem reservation for new sessions

**Purpose:** Hold every rule of this phase in one small, widget-free class.
The session model owns it and calls it. Tests can also drive it directly.

**Files to create:**
- `src/calculationresultstore.h`: class `CalculationResultStore`, its `Stats`
  and `RestoreSummary`.
- `src/calculationresultstore.cpp`: its implementation.

**Files to modify:**
- `src/CMakeLists.txt`: add `calculationresultstore.cpp calculationresultstore.h`
  to `flysight_core`, next to the `calculationrecord.*` entry that Phase 2
  added, with the one-line comment "Stored requested-calculation results:
  written on publish, restored on load (SessionModel owns it)".
- `src/logbookmanager.h` / `.cpp`: a stem reservation for sessions that
  have not been saved yet (below). It touches `saveSession`, `removeSession`,
  `remapSessionId`, `reset` and Phase 2's session-to-stem lookup of the
  record methods.

**Technical Approach:**

*Stem reservation (LogbookManager).* The store writes a record whenever the
session has a logbook identity, whether or not its session file exists yet.
A newly imported session gets that identity at import:

- New private member
  `QMap<QString, QString> m_reservedStems; // SESSION_ID -> uuid of a session not saved yet`.
  It is deliberately **not** `m_sessionIdToUuid`. That map means "has a
  session file": `flushIndex()` lists every entry of it (cpp 765), and
  `loadSessionRaw`, `peekSessionId`, `markColumnsUnsaved` /
  `markSessionUnsaved` (the `m_needsFlushBeforeSave` rule, 414-416 and
  425-426), `isIdentityEntry` and `removeSession` all rely on that meaning.
  If an unsaved session were put there, index.json could name a session
  that has no file after a crash, and the save ordering would change.
- New public method, in the public section next to `isIdentityEntry` (h 148):
  ```cpp
  // Gives a session that has no session file yet the file stem its first
  // save will use, so that files kept beside the session file (calculation
  // records) can be written before that save. Returns the stem: the
  // session's existing one if it has a file, else a reserved fresh uuid
  // (idempotent). A reservation is never listed in index.json, is not a
  // session of the scan, and is forgotten by reset(). Empty id: returns "".
  QString reserveSessionFile(const QString &sessionId);
  ```
- `saveSession` (618-624). When `m_sessionIdToUuid` has no entry, use
  `m_reservedStems.value(sessionId)` if there is one. Only when neither
  exists, use `QUuid::createUuid()`. In step d, on success, also
  `m_reservedStems.remove(sessionId)`. On failure the reservation stays. So
  the first save writes `<reserved uuid>.csv` beside the records already
  written under that stem.
- Phase 2's session-to-stem lookup for the record methods (`write`, `read`,
  `calculationRecordIds`, `removeCalculationRecord(s)`): make it one
  private helper, `QString recordStem(const QString &sessionId) const`. It
  returns `m_sessionIdToUuid` value, else the `m_reservedStems` value, else
  empty. "Not in the logbook index" then means neither.
- `removeSession` (719-742). If the id is only reserved (no
  `m_sessionIdToUuid` entry), call `removeCalculationRecordsForStem(<reserved uuid>)`,
  drop the reservation, and return true (there is no csv to delete).
  Otherwise behave as Phase 2 specifies, and also drop any reservation.
  Update the header comment.
- `remapSessionId` (681-713): also refuse (return false) when `newId` is
  reserved, the same rule as "newId already exists".
- `reset()` (256): clear `m_reservedStems`. A simulated restart forgets
  unsaved sessions, like a process exit.
- Class comment: one sentence in Phase 2's records paragraph: "A session
  not saved yet may have records under the stem reserved for it
  (reserveSessionFile()); if it is never saved they are strays and the next
  initialize() removes them."
- Nothing else. The stray pass (Phase 2, Task 2.4) already removes, at the
  next start-up, every record whose `<stem>.csv` does not exist. A record
  written before the first save is simply joined by its session file when the
  idle saver runs. If the session is never saved (crash, quit before the
  save, save always failing), the next start removes the record.

*The class.* Namespace `FlySight`. It includes
`engine/calculationengine.h`, `calculationrecord.h` and `<QSet>`, and forward
declares nothing from `src/ui/`. It holds no state except `Stats`, because
every record lives on disk. It is not a `QObject`.

```cpp
/// Stored results of explicitly requested calculations (the spec
/// "store-requested-calculations"): the rules between a session's engine and
/// its record files in the logbook. Main thread only. SessionModel owns one
/// and is its only product caller. Tests may drive it directly.
class CalculationResultStore {
public:
    struct Stats {
        int recordsWritten = 0;         ///< Ok installs whose record was committed
        int writeFailures = 0;          ///< Ok installs whose record could not be encoded or written
        int restoreCalls = 0;           ///< restoreSession() calls that listed a session's records
        int recordsRead = 0;            ///< record files read by restoreSession()
        int recordsRestored = 0;        ///< ... installed into the engine
        int recordsKept = 0;            ///< ... not installed because a result was already installed (AlreadyInstalled)
        int staleRecordsDeleted = 0;    ///< deleted by restoreSession(): unreadable, stamps, stale, unknown calculation
        int droppedRecordsDeleted = 0;  ///< deleted because an input change dropped the in-memory result
        qint64 restoreNanoseconds = 0;  ///< wall time inside restoreSession() (listing, reading, checks, restore)
        qint64 writeNanoseconds = 0;    ///< wall time exporting, stamping, encoding and writing records
    };

    struct RestoreSummary {
        int restored = 0;
        int kept = 0;
        int deleted = 0;
        QSet<DependencyKey> invalidated;   ///< union of RestoreOutcome::invalidated over every restoreResult() call
    };

    /// The engine's explicit-result listener, for the engine of session `sessionId`.
    /// May be called only from that listener (it calls engine.exportResult()).
    void onExplicitResultEvent(const QString &sessionId, const CalculationEngine &engine,
                               const ExplicitResultEvent &event);

    /// Installs every valid record of `sessionId` into `engine` and deletes
    /// the stale ones. Call it once, when the session has just been installed
    /// into a model row and before the row is published to readers. Never
    /// from inside an evaluation or an engine callback, never for a temporary
    /// load. Starts nothing.
    RestoreSummary restoreSession(const QString &sessionId, CalculationEngine &engine);

    const Stats &stats() const { return m_stats; }
    void resetStats() { m_stats = Stats(); }

private:
    bool deleteRecord(const QString &sessionId, const QString &calculationId, const char *why);   // counts nothing
    Stats m_stats;
};
```

*`onExplicitResultEvent`*, by event kind:

- **`Installed`** with a status other than `Ok`: nothing. No write and no
  delete (spec 3: "Such a run also deletes nothing").
- **`Installed` with `Ok`:**
  1. Start a `QElapsedTimer`. Add its elapsed time to `writeNanoseconds` on
     every return below.
  2. Call `engine.exportResult(event.instanceId)`. If it returns `nullopt`,
     return and count nothing. This happens when a deferred invalidation in
     the same engine call has already dropped the result; Phase 1 documents
     that sequence, and the `Dropped` event follows.
  3. Build `CalculationRecord::stamped(*snapshot)`, then call
     `LogbookManager::writeCalculationRecord(sessionId, record, &error)`. On
     `true`, add 1 to `recordsWritten`. On `false`, add 1 to `writeFailures`
     and add nothing else: the manager has already warned once, the previous
     record (if any) is intact, and the in-memory result is untouched. Never
     retry. The next `Ok` publish for the pair tries again.

     No check is made on whether the session file exists. Every model row
     has a logbook identity: a stem the manager knows, or one reserved at
     import (Task 3.2, step 5). So a session imported and fitted before the
     idle saver has run gets its record under its reserved stem. A write for
     an id with neither (only a test that bypasses the model can cause this)
     fails with the manager's "not in the logbook index" warning and counts
     as a write failure.
- **`DroppedByInputChange`**, whatever its status:
  `deleteRecord(sessionId, event.instanceId, "an input changed")`. Add 1 to `droppedRecordsDeleted` only when a file existed
  before the call. Check that with `calculationRecordIds(sessionId).contains(id)`
  before removing. That is a name listing, and it keeps the counter
  meaningful for tests. If this listing shows up in the restore-cost
  measurement, drop the check and count every call instead. Deleting when
  no file exists is harmless: `removeCalculationRecord` treats an absent file
  as success.

`deleteRecord` calls `removeCalculationRecord(sessionId, calculationId)` and
logs `qDebug("CalculationResultStore: stored %s of %s dropped: %s", ...)`. A
failure has already been warned about by the manager, so there is no second
warning.

*`restoreSession`* (the whole body is timed into `restoreNanoseconds`):

1. Add 1 to `restoreCalls`, and set `ids = calculationRecordIds(sessionId)`.
   The list is empty for a session the manager does not know. If `ids` is
   empty, return.
2. **Read and stamp check**, in `ids` order (sorted). For each id, call
   `readCalculationRecord(sessionId, id)` and add 1 to `recordsRead`:
   - `Missing`: the file vanished since the listing. Skip it.
   - `Ok`: if `!read.record->stampsAreCurrent()`, delete it ("calculation
     compatibility or environment changed"). Otherwise append it to `pending`.
     `stampsAreCurrent()` computes both stamps fresh. It never uses
     `LogbookManager::cacheEnvironment()`.
   - `Unreadable`, `NotARecord`, `UnsupportedVersion`, `Corrupt`: delete it,
     giving `read.error` as the reason.
3. **Restore in passes.** Loop while `pending` is not empty:
   - `next = {}`, `progressed = false`.
   - For each record in `pending`, in order: `outcome =
     engine.restoreResult(record.result)`, and
     `summary.invalidated.unite(outcome.invalidated)`. Then, by
     `outcome.kind`:
     - `Restored`: add 1 to `restored`, set `progressed = true`.
     - `AlreadyInstalled`: add 1 to `kept`. **Never delete.** A result
       installed some other way wins and its record stays.
     - `Stale` with `staleCheck == InputsUnavailable`: append to `next`. An
       upstream Explicit result may be restored later in this pass or in the
       next one.
     - `Stale` with any other check (`ResultVersion`, `Bundle`, `Leaves`,
       `Fingerprint`): delete it, giving the check's name as the reason.
     - `NotFound`, `NotExplicit`: delete it ("calculation not registered as
       explicit"). Stamps that are current make this unreachable in
       practice, because the environment fingerprint covers registrations.
       The rule stays so that no record is kept that can never be used.
   - If `!progressed`, delete every record in `next` ("inputs unavailable")
     and stop. Otherwise set `pending = next` and loop.

   Each pass that continues restores at least one record, so the number of
   passes is at most the number of records. This is the repeated-pass rule
   from the Integration Notes: explicit-on-explicit chains restore in any file
   order, and `InputsUnavailable` counts as stale only after the last pass.
4. Add the counters to `m_stats` (`recordsRestored`, `recordsKept`,
   `staleRecordsDeleted`) and return the summary.

`restoreResult` is called only between evaluations and never from a listener,
so no engine re-entrancy rule is broken. A `DroppedByInputChange` event
caused by a restore itself would re-enter `onExplicitResultEvent` and delete a
file. That cannot happen at load time. `dropNotRequested` reports only
*requested* explicit entries, and a session that has just been loaded has none
(see Gotchas). The code needs no guard for it.

**Acceptance Criteria:**
- [ ] `flysight_core` builds with the two new files. `calculationresultstore.*`
      includes nothing from `src/ui/` and no Qt Widgets header.
- [ ] `reserveSessionFile` gives an unsaved session a stem that its first
      save then uses. The reservation never appears in index.json.
      `removeSession` of a reserved-only session deletes its records.
      `reset()` forgets reservations. Covered by
      `tst_result_store::recordBeforeFirstSave`,
      `recordOfNeverSavedSessionIsStray` and `removeBeforeFirstSave`.
- [ ] `tst_logbook_index`, `tst_persistence_roundtrip`,
      `tst_column_cache` and `tst_result_records` pass unchanged. A session
      that was never reserved saves exactly as before.
- [ ] An `Installed Ok` event writes exactly one record whose
      `result` is `sameContent` with `exportResult()` and whose stamps are
      current (`tst_result_store::writesOnOkInstall`).
- [ ] An `Installed` event with any other status neither writes nor deletes
      (`nonOkInstallWritesAndDeletesNothing`).
- [ ] A `DroppedByInputChange` event deletes the pair's record
      (`inputChangeDeletesRecord`).
- [ ] `restoreSession` installs valid records in passes, deletes stale,
      unreadable and unknown records, and keeps a record whose result is
      already installed (`restoresChainInPasses`,
      `upstreamMissingAfterLastPassDeletes`, `staleRecordDeletedOnLoad`,
      `alreadyInstalledIsKept`).
- [ ] Neither function creates a job, a ticket or a run.
      `CalculationEngine::runCount` and `preparedCount` are unchanged by a
      restore (asserted in every restore test).

**Complexity:** L

---

### Task 3.2: Session model wiring

**Purpose:** Hook the store to both install paths (through the listener) and
to every load path that installs a session into a row, before the row is
published to readers. Leave the temporary loads alone.

**Files to modify:**
- `src/sessionmodel.h`: member, private helper, test seam, class comment.
- `src/sessionmodel.cpp`: `attachSession`, `sessionRef`, `mergeSessions`
  (the created branch reserves the stem; the unloaded branch restores),
  `processNextBulkEdit` (promotion), comments at the temporary loads.

**Technical Approach:**

1. **Member.** Add `CalculationResultStore m_resultStore;` to the private
   members, declared **before** `m_rows`, so that it outlives every row's
   engine during destruction. Engines never call the listener when they are
   destroyed, but the order costs nothing and removes the question. Include
   `calculationresultstore.h` in `sessionmodel.h`.

2. **Test seam** (public, next to `columnWorkStats()`, same style):
   ```cpp
   /// Work done by the stored-result store (see STORED RESULTS). A test seam.
   const CalculationResultStore::Stats &storedResultStats() const { return m_resultStore.stats(); }
   void resetStoredResultStats() { m_resultStore.resetStats(); }
   ```

3. **Listener: both install paths** (`attachSession`, cpp 1236-1249). After
   the existing invalidation listener, add:
   ```cpp
   CalculationEngine &engine = sr.session->calculationEngine();
   engine.setExplicitResultListener([this, sessionId, &engine](const ExplicitResultEvent &event) {
       m_resultStore.onExplicitResultEvent(sessionId, engine, event);
   });
   ```
   This follows the same capture rules as the invalidation listener: the
   model and the session id, never a row address. Capturing the engine by
   reference is safe. The engine lives on the heap
   (`SessionData::m_engine`, `std::unique_ptr`), keeps its address when the
   session moves (it is only rebound), and owns the listener, so the listener
   can only run while the engine is alive. The existing comment at 1241-1243
   gains one sentence: "The explicit-result listener also captures the engine
   it is installed on: it is owned by that engine and moves with it."

   Both install paths reach this listener with no further code:
   - The job queue's publish: `JobQueue::finishRun` → `ticket->publish()`
     (jobqueue.cpp 443) → `publishPrepared` → `Installed`.
   - The synchronous `CalculationEngine::request()` of an Explicit
     calculation.

   No job is ever started from a restore. `JobQueue::request()` is the only
   way to create a job, and `PlotRequests` calls it only for gestures (the
   header contract at plotrequests.h 153-166). After a restore,
   `readiness()` is `Done`, so a later `request()` answers `NothingToDo`.

   The session id captured at attach time stays the row's id while the row is
   loaded. `setRowSessionId` runs only before `attachSession` in `sessionRef`
   (1035-1038) and in the bulk edit (1954-1956), and for stub rows in the
   column worker and `resolveIdentityStubs`.

4. **Private helper:**
   ```cpp
   /// Restores the stored results of a row whose session has just been installed
   /// (see STORED RESULTS). Returns the names the restore invalidated.
   QSet<DependencyKey> restoreStoredResults(SessionRow &sr);
   ```
   Body: `Q_ASSERT(sr.isLoaded() && !sr.loadFailed)`, then return
   `m_resultStore.restoreSession(sr.sessionId, sr.session->calculationEngine()).invalidated`.

5. **Load paths that restore.** Each one restores after `attachSession` and
   before anything is emitted for the row:
   - **`sessionRef()`** (1027-1071). This one call covers start-up stubs,
     show (`setRowsVisibility`' synchronous path at 870-889), the background
     loader (`loadNextVisibleSession`, 1643), focus (`setFocusedSessionId`,
     1148), the force-loads of `updateAttribute` / `removeAttribute` / `setData`,
     the flush paths, and the plot widgets. Replace
     `if (!sr.loadFailed) attachSession(sr);` (1053-1054) with a block that
     attaches and then calls `restoreStoredResults(sr)`. Keep the comment
     about the id remap, and add: "Stored results go in before
     sessionLoaded: no reader ever sees the row without them." The returned
     names are discarded. Nothing outside the model has read this engine yet,
     because the row is published only by the `sessionLoaded` that follows.
     Say so in a comment. The restore runs before `lruTouch` /
     `evictIfNeeded(sr.sessionId)`.
   - **`mergeSessions()`, unloaded branch** (632-680). After
     `attachSession(row);` (668), add
     `keys.unite(restoreStoredResults(row));` before `recordMerge(...)`, so
     that the invalidated names are published with the merge's own
     (`publishInvalidation`, 723). The restore is checked against the
     *merged* in-memory state. If the merge changed an input of a stored
     result, the fingerprint or leaf check fails and the record is deleted
     ("found stale on load"). `sessionLoaded` for this row is emitted at 727-728,
     after the restore.
   - **`mergeSessions()`, loaded-in-place branch** (617-630): **no restore.**
     `SessionMerge::apply` changes the session through `setAttribute` /
     `mergeSourceData` (sessionmerge.cpp 195-203; sessiondata.cpp 165-186).
     Those are leaf notifications, not `clear()`. A stored result the merge
     does not touch stays installed and keeps its record. One it touches is
     dropped with `DroppedByInputChange`, and the listener deletes its record.
     Add a one-line comment saying this. The overview's statement that this
     branch drops results through `clear()` is wrong; see Deviations 1,
     accepted by the coordinator.
   - **`mergeSessions()`, created branch** (600-616): no restore, because a
     new session has no records. Before `attachSession(m_rows.last())` (613),
     call `LogbookManager::instance().reserveSessionFile(file.sessionId)`
     (the `logbook` local at 554). This gives the session its logbook
     identity at import, so that a result published before the idle saver
     runs is stored (Task 3.1). The reservation changes nothing else:
     `invalidateAllColumns` / `markSessionUnsaved` still see an unknown
     session, index.json does not list it, and the first save uses the
     reserved uuid.
   - **Bulk edit, promotion of a temporary session** (1979-1986, when the save
     of an edited stub fails). After `attachSession(sr);`, call
     `restoreStoredResults(sr);` and discard the result. The `dataChanged`
     at 1993-1994 follows.

6. **Temporary loads never restore.** Add a comment and no code at:
   - the column worker's stub branch (1806-1822): "A temporary load: stored
     results are never read here (see STORED RESULTS)";
   - the bulk edit's stub path (1950): the same sentence.

7. **Deletion paths, unchanged.** `removeSessions()` (916) erases rows only.
   `MainWindow::on_action_Delete...` (mainwindow.cpp 820-827) then calls
   `LogbookManager::removeSession`, which deletes the records (Phase 2). No
   code change. `evictSession` (1692-1757), `flushDirtySessions`, the model's
   destructor, `checkCalculationEnvironment` and the registry observer delete
   nothing: they cause no `DroppedByInputChange`.

8. **Class comment** (`sessionmodel.h`, after the PINNED SESSIONS paragraph),
   a new paragraph with this content:

   > STORED RESULTS. The result of an explicit calculation that the engine
   > installs with status Ok (a job's publish or a synchronous request) is
   > written beside the session file by CalculationResultStore, through the
   > explicit-result listener that attachSession() installs. That includes a
   > session not saved yet: mergeSessions() reserves its file stem at import
   > (LogbookManager::reserveSessionFile). A result that
   > an input change drops deletes its record. Every path that installs a
   > session into a row (sessionRef(), the unloaded branch of
   > mergeSessions(), the promotion of a bulk edit's temporary session)
   > restores the session's valid records into its engine before the row is
   > published (before sessionLoaded, dataChanged or any plot pass) and
   > deletes the stale ones. Restoring is not requesting: it starts nothing.
   > Temporary loads (column worker, bulk edit on a stub) never read a
   > record. Eviction, unloading, a registry change, the model's destruction
   > and removeSessions() never delete one (LogbookManager::removeSession
   > does, with the session file).

   Also amend the comment on `publishCalculationInvalidation` (h 276-277).
   After "nothing is saved", add: "(the record of a stored result is written
   by the engine's listener at the install itself, see STORED RESULTS)". Leave
   the column-cache comment (h 70-74) and `computeColumnValues` (cpp 2028-2031)
   to Phase 4, which rewrites that code.

**Acceptance Criteria:**
- [ ] After a job publishes `Ok` and after a synchronous `request()` returns
      `Ok`, the record exists before control returns to the event loop
      (`writesOnOkInstall`, rows `queue` and `request`). The same holds for a
      session created by an import in the same event-loop pass, before its
      first save (`recordBeforeFirstSave`).
- [ ] Every load path (show with 3 or fewer stubs, show with more than 3
      stubs through the background loader, focus, an edit that force-loads,
      start-up after a restart, a merge into an unloaded session, and the
      bulk edit's promotion) installs the stored result: `resultStatus == Ok`,
      the output reads the stored value, `runCount == 0`. For the paths that
      emit `sessionLoaded`, the result is already installed when
      `sessionLoaded` is emitted (`restoreOnEveryLoadPath`,
      `bulkEditPromotionRestores`).
- [ ] The column worker's and the bulk edit's temporary loads read no record
      (`storedResultStats().restoreCalls` unchanged) and leave even a stale
      record in place (`temporaryLoadsNeverRestore`).
- [ ] Eviction, a registry change and a restart leave a record's bytes
      unchanged (`noDeleteWithoutInputChange`).
- [ ] The existing suites `tst_session_model_engine`, `tst_jobqueue`,
      `tst_plot_requests`, `tst_column_cache`, `tst_import_merge`,
      `tst_logbook_index`, `tst_fusion_jobs` and `tst_fusion_rows` pass
      unchanged.

**Complexity:** M

---

### Task 3.3: Contract comments and the one existing test that states the old behaviour

**Purpose:** Correct the statements that stop being true in this phase.

**Files to modify:**
- `src/plotrequests.h`: the "ONLY GESTURES START WORK" paragraph (153-166).
- `tests/tst_jobmodel.cpp`: `nothingIsPersisted` (752-790).

**Technical Approach:**

- `plotrequests.h`. Replace the last sentence of the paragraph ("Restoring
  checked plots, ... request nothing: the affected tracks are Missing and the
  refresh control shows.") with this meaning:

  > Restoring checked plots, applying a profile, the Plots menu, showing a
  > track, loading or merging a session, an input change, a job ending
  > cancelled / superseded / failed, a registry change, rowState() and
  > flush() request nothing. A track whose session was loaded with a valid
  > stored result of the calculation (SessionModel restores it before the
  > row is published) is Available, or Failed for a stored rejection, exactly
  > as after a fresh publish, and adds nothing to the refresh count. Every
  > other affected track is Missing, and the refresh control shows.

  No code in `plotrequests.cpp` changes. `classify()` (314-372) already maps
  `BlockerReport::Available` to Available and `NotProduced` to Failed, and a
  restored result produces exactly those reports.
- `tst_jobmodel::nothingIsPersisted` asserts that the whole logbook folder is
  byte-identical after four jobs. Two of those jobs publish `Ok` (`gated` on
  s1, and the `expA` rejection on s2), and they now write records. Keep the
  test's intent: no *job* is persisted. Make it precise:
  - Before the jobs, note `const QString stem1 = sessionFileStem("s1"), stem2 = sessionFileStem("s2");`.
  - After the jobs, split `snapshot(env.logbookDir())` into its entries that
    end in `.fvresult` and the rest. The rest must equal `logbookBefore`. The
    `.fvresult` entries' relative paths must be exactly
    `{"sessions/" + stem1 + ".gated.fvresult", "sessions/" + stem2 + ".exp%41.fvresult"}`.
    That shows the ResourceExhausted job and the cancelled job stored nothing.
  - Update the comment: "Spec 8.4: no job is persisted. Jobs of every ending
    leave the settings and the logbook folder byte-identical, except for the
    stored results of the jobs that published Ok (store-requested-calculations):
    records are results, not jobs."

**Acceptance Criteria:**
- [ ] `plotrequests.h` no longer implies that a loaded track of an
      explicit-backed plot is always Missing. It states the restored case.
- [ ] `tst_jobmodel` passes, and `nothingIsPersisted` asserts the exact set of
      two record files.

**Complexity:** S

---

### Task 3.4: Core tests with synthetic explicit calculations: `tst_result_store`

**Purpose:** Prove the store's rules on real `SessionModel` / `LogbookManager`
/ `JobQueue` instances with fast synthetic calculations. These include the
cases that fusion cannot reach cheaply: write failures, chains, every stale
reason, every load path, and the temporary loads.

**Files to create:**
- `tests/tst_result_store.cpp`: class `ResultStoreTest`,
  `FLYSIGHT_TEST_MAIN(ResultStoreTest)`, `#include "tst_result_store.moc"`.

**Files to modify:**
- `tests/CMakeLists.txt`: in the "SessionData / SessionModel on the engine"
  block (after `tst_session_model_engine`, about line 139):
  `flysight_add_test(tst_result_store SOURCES tst_result_store.cpp)`. Extend
  the block's comment with ", and the stored results of explicit calculations".

**Technical Approach:**

*Fixture* (follow `tst_jobqueue.cpp` 162-230 and its cleanup discipline):
- `initTestCase`: `registerBuiltIns()`, `registerPreference(LogbookColumnsVersion, 0)`,
  `LogbookColumnStore::instance().setColumns({descriptionColumn()})`,
  `qRegisterMetaType<DependencyKey>()`.
- `init`: `useFreshLogbook()`, `resetPreferencesToDefaults()`,
  `LogbookManager::instance().initialize()`, note `m_registryBefore`. Then
  `m_world = std::make_unique<JobWorld>()` (registers `expA`, `derivA`,
  `derivA2`, `expB`, `derivB` and the gated / thrower / exhausted
  calculations on the global registry; `jobfixture.h`). Then
  `m_store = std::make_unique<StoreWorld>()` (below). Then the model,
  `mergeSessions(JobWorld::sessions({"s1","s2","s3","s4"}))`,
  `QVERIFY(waitForIdle(*m_model))` (saved, and index flushed, so every session
  has a stem), and the queue.
- `cleanup`: queue shutdown, then reset queue, model, `StoreWorld`,
  `JobWorld` in that order. Then compare the registry ids with
  `m_registryBefore` and `enrolledEngineCount() == 0`. Restore the columns to
  `{descriptionColumn()}` and `LogbookCacheSize` to 50.
- A file-local RAII `StoreWorld`, registering on the global registry and
  unregistering exactly its own ids in its destructor (pattern:
  `PlotFixture` / `JobWorld`). Put the literals in the comment table.
  | Id | Policy | Inputs | Output |
  |----|--------|--------|--------|
  | `test.store.up` | Explicit | attr `UP_IN` | `UP_OUT = UP_IN * 3` (int) |
  | `test.store.down` | Explicit | attr `UP_OUT`, attr `DOWN_IN` | `DOWN_OUT = UP_OUT + DOWN_IN` (int) |
  | `test.store.listy` | Explicit | attr `LY_IN` | `LY_OUT = LY_IN * 2` (int) when `LY_IN >= 0`; else `QVariantList{LY_IN}` (a type records refuse) |

  Literals: `UP_IN = 2`, `DOWN_IN = 5` give `UP_OUT 6` and `DOWN_OUT 11`.
  `test.store.down` sorts before `test.store.up`. That is what makes
  `restoresChainInPasses` exercise a second pass.
- Helpers (file-local, returning `QString` or values, per tests/README §8):
  - `session(id)` / `engine(id)`, as in `tst_jobqueue`.
  - `setInput(id, key, value)`: `updateAttribute`.
  - `recordPath(id, encodedId)`: `sessionsDir() + "/" + sessionFileStem(id) + "." + encodedId + ".fvresult"`.
    Encoded ids are literals: `"exp%41"`, `"exp%42"`, `"thrower"`,
    `"test%2Estore%2Eup"`, `"test%2Estore%2Edown"`, `"test%2Estore%2Elisty"`.
  - `evict(ids)`: touch each id with `session(id)` so that it is in the LRU
    (a created row is not), then set `LogbookCacheSize` to 0 and check that
    each row is a stub. The test's scope guard restores 50.
  - `restart()`: reset queue and model, `reopenLogbook()`, `initialize()`,
    new model with `populateFromIndex(cachedColumnValues(enabledColumns()), lastAccessedMap())`,
    new queue (pattern: `tst_column_cache::restartAsStubs`, 210-220).
  - `rewriteRecord(id, calcId, mutate)`: `readCalculationRecord`, apply a
    lambda to the `CalculationRecord`, then `writeCalculationRecord`. Returns
    empty on success.
  - `statusAtLoad`: a `QObject scope;` connection to `sessionLoaded` that,
    for the watched id, takes a `stableRows()` guard and notes
    `loadedSession(id)->calculationEngine().resultStatus(calc)`.
- Expected values are literals (`EA1 == 5`, `"negative input"`, file names).
  Never recompute them with the code under test.

*Test functions and acceptance criteria:*

1. `writesOnOkInstall_data` / `writesOnOkInstall`. Rows `request` (the
   synchronous `engine("s1").request("expA")`) and `queue`
   (`m_queue->request("s1","expA")` + `waitIdle`). `setInput("s1","EA_IN",4)`,
   `waitForIdle`, then the row's request. Check:
   - `calculationRecordFiles() == {stem(s1) + ".exp%41.fvresult"}`;
   - the read status is `Ok`, `stampsAreCurrent()`, and the record's
     `result` is `sameContent` with `engine("s1").exportResult("expA")`;
   - `storedResultStats().recordsWritten == 1`;
   - the file existed immediately after the request returned (row
     `request`) or when `jobFinished` was emitted (row `queue`: check in a
     slot).
2. `rejectionIsWritten`: `EA_IN = -1`, request. The record's `result.detail`
   and `bundle.reason()` are `"negative input"`, `EA_DIAG == "rejected"`,
   and `EA1` / `EA2` are unavailable.
3. `nonOkInstallWritesAndDeletesNothing`: write, through
   `writeCalculationRecord`, a hand-built stamped record for `thrower` on s1
   (`T_IN = 1` set first) and one for `expA` on s2 (no `EA_IN`). Then run
   `thrower` through the queue (job ends Succeeded with status `Failed`) and
   `engine("s2").request("expA")` (`MissingInput`). Both files keep their
   bytes. `recordsWritten == 0`, `droppedRecordsDeleted == 0`.
4. `recordBeforeFirstSave_data` / `recordBeforeFirstSave`. Rows `evict` and
   `restart`. Build `DescentFixture`'s session `n1` (`JobWorld::sessions({"n1"})`)
   with stored `EA_IN = 4` and call `mergeSessions({n1})` (Created). In the
   same event-loop pass, with no save yet, call
   `engine("n1").request("expA")` (Ok) inside a `WarningCapture`. Check:
   - zero warnings, `recordsWritten == 1`;
   - `calculationRecordFiles()` gained exactly one name, ending in
     `".exp%41.fvresult"`, and `sessionCsvFiles()` is unchanged (no csv for
     n1 yet);
   - after `LogbookManager::instance().flushIndex()`, `readIndex()` has no
     entry for n1 (a reservation is never listed).

   Then `waitForIdle`: the idle saver has saved n1. Now
   `sessionFileStem("n1")` is non-empty, and the record's name is exactly
   `sessionFileStem("n1") + ".exp%41.fvresult"` (the first save used the
   reserved stem). Take `Quiet`, then run the row: `evict({"n1"})` and
   `session("n1")`, or `restart()` and `session("n1")`. Check:
   - `resultStatus("expA") == Ok`, `EA1 == 5`, `runCount == 0`;
   - `recordsRestored == 1`, no job.
5. `recordOfNeverSavedSessionIsStray`: import `n1` and request `expA` as in
   4, so the record exists. Then, before any event-loop pass, reset the
   queue and the model (the model's destructor saves nothing; this is a
   crash before the first save) and `restart()`. Check: the record file is
   gone (the stray pass), there is no row `n1`, and `sessionCsvFiles()` has
   no new file.
6. `removeBeforeFirstSave`: import `n1` and request `expA` as in 4. Then run
   the main window's delete sequence: `removeSessions({"n1"})`,
   `LogbookManager::instance().removeSession("n1")` (returns true),
   `flushIndex()`. Check: the record file is gone at once, and a second
   `removeSession("n1")` returns false.
7. `writeFailureLeavesResultUsable`: `QDir().mkdir(recordPath("s1","exp%41"))`,
   `EA_IN = 4`, request `expA` inside a `WarningCapture`. Check:
   - status `Ok`, `EA1 == 5`, `DA == 105`;
   - exactly one captured warning containing `"not written"`;
     `writeFailures == 1`;
   - the directory is still there, and `calculationRecordFiles()` is empty
     (no temporary file).
   Then remove the directory, `setInput("s1","EA_IN",6)`, and request again:
   the record is written.
8. `writeFailureKeepsPreviousRecord`: `setInput("s1","LY_IN",-1)`,
   `waitForIdle`. Write a hand-built, stamped record for `test.store.listy`
   on s1 (bundle `LY_OUT = 4`, one `storedAttribute("LY_IN")` leaf, a 32-byte
   fingerprint of `'x'`) and note its bytes. Request `test.store.listy`
   inside a `WarningCapture`. Check:
   - status `Ok`; `session("s1").getAttribute("LY_OUT")` is a `QVariantList`
     `{-1}` (the in-memory result is usable);
   - one warning that names `LY_OUT`;
   - the file's bytes are unchanged; `writeFailures == 1`.
   This is the Integration Notes' encoder-refusal technique. No permission
   bits are involved.
9. `inputChangeDeletesRecord`: `EA_IN = 4`, `EB_IN = 10`, request `expA` then
   `expB`: two records. `setInput("s1","EA_IN",7)`. Before any event-loop
   pass: both files are gone, `droppedRecordsDeleted == 2`,
   `resultStatus("expA")` is not `Ok`, and `m_queue->model()->rowCount()` is
   unchanged. `setInput("s1","_DESCRIPTION","x")` on a fresh `expA` record
   leaves it byte-identical.
10. `noDeleteWithoutInputChange`: `expA` installed with record R on s1. Then:
   - (a) `evict({"s1"})`: R's bytes unchanged.
   - (b) reload s1 (restored). Register, then unregister, an OnDemand
     calculation `test.store.shadow` that declares output attr `EA_IN`
     (returns 0; the stored value wins). The first registry change drops
     `expA`'s in-memory result (`resultStatus` not `Ok`). R's bytes are
     unchanged, and `droppedRecordsDeleted == 0`.
   - (c) `restart()`: R's bytes unchanged. `session("s1")` restores it
     (`recordsRestored == 1`).
11. `restoreOnEveryLoadPath_data` / `restoreOnEveryLoadPath`. Common setup:
   `EA_IN = 4` on s1, request `expA`, `waitForIdle`, `evict({"s1","s2","s3","s4"})`,
   `resetStoredResultStats()`, watch `sessionLoaded` for s1, and take
   `Quiet quiet(*m_queue)`. Rows:
   - `show`: `setRowsVisibility({row(s1): true})`;
   - `background`: all four rows visible in one call (more than
     `kSyncLoadThreshold`), then `waitForIdle`;
   - `focus`: `setFocusedSessionId("s1")`;
   - `edit`: `updateAttribute("s1","_DESCRIPTION","renamed")`;
   - `startup`: `restart()`, then `show`;
   - `mergeUnloaded`: `mergeSessions({incoming})`, where `incoming` holds only
     `SESSION_ID = s1` and header attribute `STORE_NOTE = "n"`. The result is
     `Merged`.

   Check after each row:
   - s1 is loaded, `resultStatus("expA") == Ok`, `EA1 == 5`,
     `runCount("expA") == 0`, `preparedCount() == 0`;
   - `statusAtLoad == Ok` (installed before `sessionLoaded`);
   - `recordsRestored == 1`, `recordsWritten == 0`;
   - the record's bytes are unchanged (a restore never rewrites);
   - `quiet.holds()`.
12. `bulkEditPromotionRestores`: s1 with an `expA` record, `waitForIdle` (the
    index holds the description column's value), evicted. Replace
    `index.json` with a directory (`QFile::remove`, then `QDir().mkdir`), so
    that `saveSession`'s pre-save flush fails. Then
    `startBulkEdit({row(s1)}, 0, "bulk")` and `waitForIdle`, inside a
    `WarningCapture`. Check:
    - s1 is loaded, `dirty`, `saveFailed`;
    - `resultStatus("expA") == Ok`, `EA1 == 5`, `runCount == 0`,
      `recordsRestored == 1`;
    - the record's bytes are unchanged.
    Remove the directory before the test returns (scope guard). If this
    technique does not make the save fail, report it rather than weakening
    the check. A directory at a `QSaveFile` target is the same technique
    Phase 2 relies on.
13. `restoresChainInPasses`: `UP_IN = 2`, `DOWN_IN = 5`, request
    `test.store.up` then `test.store.down`, `evict`. Check:
    - `calculationRecordIds("s1") == {"test.store.down","test.store.up"}`
      (the premise: the downstream record comes first);
    - after `session("s1")`, both are `Ok`, `DOWN_OUT == 11`, both run
      counts 0, `recordsRestored == 2`, `staleRecordsDeleted == 0`.
14. `upstreamMissingAfterLastPassDeletes`: as in 13, then
    `removeCalculationRecord("s1","test.store.up")`, and reload. Check: the
    down record's file is gone, `resultStatus("test.store.down")` is not
    `Ok`, `staleRecordsDeleted == 1`, `recordsRestored == 0`.
15. `staleRecordDeletedOnLoad_data` / `staleRecordDeletedOnLoad`. Common
    setup: an `expA` record for `EA_IN = 4`, `evict`, then the row's
    alteration, then `session("s1")`. Rows:
    - `compatibility`: `calculationCompatibility + 1` via `rewriteRecord`;
    - `environment`: register an OnDemand `test.store.extra` (no inputs,
      output attr `_STORE_EXTRA`) after eviction; a scope guard unregisters it
      after the checks;
    - `resultVersion`: `result.resultVersion = "v-old"`;
    - `bundle`: add an undeclared attribute `NOT_DECLARED = 1` to the bundle;
    - `leaves`: drop the only leaf;
    - `fingerprint`: flip the first fingerprint byte;
    - `notARecord`: overwrite the file with `"garbage"`;
    - `unsupportedVersion`: patch bytes 8-11 to `02 00 00 00`;
    - `unknownCalculation`: an additional stamped record whose `calculationId`
      is `test.store.gone` (a copy of the `expA` record with its id and file
      name changed). In this row the `expA` record must also be restored.

    Check: the altered file is gone, `staleRecordsDeleted == 1`,
    `runCount("expA") == 0`, and no job. Also `resultStatus("expA")` is not
    `Ok`, except in `unknownCalculation`, where it is `Ok`.
16. `alreadyInstalledIsKept`: s1 with an `expA` record. Load a separate copy
    with `LogbookManager::instance().loadSession("s1")` and call
    `copy->calculationEngine().request("expA")` (Ok). Then a local
    `CalculationResultStore store;` and
    `store.restoreSession("s1", copy->calculationEngine())`. Check:
    `kept == 1`, `restored == 0`, `deleted == 0`, and the record's bytes are
    unchanged. Repeat after `copy->removeAttribute("EA_IN")` and a request
    that caches `MissingInput`: still kept.
17. `temporaryLoadsNeverRestore`: s1 with an `expA` record made stale with
    `rewriteRecord` (`calculationCompatibility + 1`), evicted,
    `resetStoredResultStats()`, `resetColumnWorkStats()`. Then:
    - `LogbookColumnStore::instance().setColumns({descriptionColumn(), exitTimeColumn()})`
      and `waitForIdle`. The column worker loaded s1 temporarily
      (`columnWorkStats().sessionsLoaded >= 1`).
    - `startBulkEdit({row(s1)}, 0, "bulk")` and `waitForIdle`. s1 is still a
      stub.

    After both: `restoreCalls == 0`, `recordsRead == 0`, and the stale file
    still has its bytes. Then `session("s1")` deletes it
    (`staleRecordsDeleted == 1`). That shows it would have been deleted if
    anything had read it.
18. `deletingSessionRemovesRecords`: `expA` records on s1 and s2. Run the
    main window's sequence: `m_model->removeSessions({"s1"})`,
    `LogbookManager::instance().removeSession("s1")`, `flushIndex()`. Check:
    s1's csv and record are gone, and s2's record keeps its bytes.
19. `strayRecordRemovedAtRestart`: s1 with an `expA` record. Reset the queue
    and the model, delete s1's csv with `QFile::remove`, then `restart()`.
    Check: the record is gone (Phase 2's stray pass) and
    `calculationRecordFiles()` holds no name with s1's stem.

**Acceptance Criteria:**
- [ ] `tst_result_store` builds, has label `core`, and passes. Every function
      listed above exists with the checks stated.
- [ ] No test depends on permission bits, on case sensitivity, or on
      directory iteration order.
- [ ] The test leaves the global registry as it found it, and no engine
      enrolled.

**Complexity:** L

---

### Task 3.5: Fusion end-to-end tests: `tst_fusion_store` (+ exact twin)

**Purpose:** Spec section 8 on the real fit. A result survives unload and
restart bit for bit, rejections and solver failures keep their badge,
validity follows inputs and code stamps, and the session file is untouched.

**Files to create:**
- `tests/tst_fusion_store.cpp`: class `FusionStoreTest`.

**Files to modify:**
- `tests/CMakeLists.txt`:
  - after `tst_fusion_rows` (about line 324):
    ```cmake
    # Stored fusion results: fit, unload / restart, restore (store-requested-calculations)
    flysight_add_fusion_test(tst_fusion_store SOURCES tst_fusion_store.cpp
      LIBS flysight_fusion_session_support)
    ```
  - in the `if(_fs_exact)` block (about line 434):
    `flysight_add_fusion_exact_test(tst_fusion_store)`. Extend the comment
    "the three that hold the registered calculation, the job queue's worker
    thread and the plot rows to the same goldens" so that it also names the
    stored and restored results.

**Technical Approach:**

*Fixture:* follow `tst_fusion_rows.cpp` (149-201) with the same members
`m_model`, `m_queue`, `m_plots` (`setPlots(fusionPlots())`), `m_requests` and
the same cleanup discipline.
- `initTestCase`: `registerBuiltIns()`, `registerFusionOnce()`, columns
  `{descriptionColumn()}`. Do not add a fusion column: that is Phase 4's.
- Constants: `kFit` (`Fusion::FitCalculationId`), `kRoll = "Fusion/roll"`,
  `kRecordSuffix = ".builtin%2Efusion%2Efit.fvresult"` (a literal),
  `kFitTimeoutMs = 120000`.
- Helpers:
  - `session` / `engine` / `fusion`, `availableIn`, `row(plotId)`
    (flush + rowState), `show`, `check`. Copy these from `tst_fusion_rows`.
  - `recordPath(id)`: `sessionsDir() + "/" + sessionFileStem(id) + kRecordSuffix`.
  - `struct FitValues { QHash<QString, QVector<double>> channels; QString diagnostics; QString detail; };`
    - `capture(id)`: every name of `fusionMeasurementNames()`, plus `accH`
      and `_system_time`, via `fusion(id, name)`; `_FUSION_DIAGNOSTICS`
      as a string; `engine(id).resultDetail(kFit)`.
    - `QString differenceFrom(const FitValues &, id)`: empty when every
      channel is `sameBitsEverywhere`, the diagnostics are byte-identical
      in UTF-8, and the detail is equal. Otherwise the first difference.
  - `unloadAndReload(id)`, returning `QString`: hide, set `LogbookCacheSize`
    to 0, check that the row is a stub and the record's bytes are unchanged,
    set it back to 50, then `show({id})`. A scope guard at the start of every
    test that uses it resets the capacity to 50.
  - `restart()`: reset requests, plots, queue and model (in that order),
    `reopenLogbook()`, `initialize()`, new model with `populateFromIndex(...)`,
    new queue, plots and requests.
  - `statusAtLoad`: the same `sessionLoaded` watcher as in Task 3.4.

*Test functions and acceptance criteria:*

1. `restoredAfterEvictionIsBitIdentical` (`coarse_maneuver`, session `a`):
   - `addSessions`, `show({"a"})`, `check("roll")`,
     `plotCheckedByUser(kRoll) == 1`, `waitIdle`. The job has Succeeded, and
     the record file exists.
   - Note `fresh = capture("a")`, `fusionRollAtExit()`, the record's bytes
     R0, `dependenciesOf(GraphNode::result(kFit))`, and the fresh
     `blockers(fusionKey("roll"))`.
   - `resetStoredResultStats()`, take `Quiet`, time `unloadAndReload("a")`
     with a `QElapsedTimer`. Then check:
     - `differenceFrom(fresh, "a")` is empty, and roll at exit has the same
       bits;
     - `goldenDifference(session("a"), loadFusionGolden("coarse_maneuver"))`
       is empty, and `compareJson` of the diagnostics against the golden is
       empty;
     - `runCount(kFit) == 0`, `preparedCount() == 0`,
       `readiness(kFit).state == Done`,
       `m_queue->request("a", kFit).kind == NothingToDo`;
     - `m_queue->model()->rowCount() == 1` (only the first job);
       `quiet.holds()`;
     - `row(kRoll).isPlain()` and `controlCount() == 0`;
     - `blockers(fusionKey("roll")).state == Available`;
     - `dependenciesOf` equals the fresh one;
     - `statusAtLoad == Ok`; the record's bytes are still R0;
       `recordsRestored == 1`, `recordsWritten == 0`.
   - **Restore cost:** `qInfo().noquote()` one line:
     `"restore of coarse_maneuver: <restoreNanoseconds/1e6> ms of a <elapsed> ms reload; record <bytes> bytes, session file <bytes> bytes; write <writeNanoseconds/1e6> ms"`.
     Measure the write on the first publish, before the stats reset. The
     implementer copies this line into the phase report. No timing assertion.
2. `restoredAfterRestartIsBitIdentical` (`stationary_spin`):
   - `m_queue->request("a", kFit)`, `waitIdle`. Note `fresh`.
   - `restart()`, `check("roll")` (programmatic), take `Quiet`,
     `show({"a"})`.
   - Same checks as 1 against `stationary_spin`'s golden. The new queue has
     `rowCount() == 0`.
3. `restoredRejectionShowsBadge` (`reject_origin`, session `r1`):
   - Gesture as in `tst_fusion_rows::rejectedTrackShowsBadge` (548-570). Note
     the fresh `blockers(fusionKey("roll"))` report and the diagnostics.
   - `unloadAndReload("r1")`. Check:
     - `row(kRoll)`: `failedCount == 1`, `showsWarning()`,
       `control() == None`;
     - `failed.at(0).reason == "Sensor fusion: Local origin index outside GNSS samples"`
       (a literal);
     - the blockers report is `NotProduced`. Its state, blocker ids, and each
       note's instance id, status and detail equal the fresh report's. The
       note detail is `"Local origin index outside GNSS samples"`;
     - diagnostics are byte-identical; every fusion measurement is empty;
     - `refreshPressed(kRoll) == 0` and `plotCheckedByUser(kRoll) == 0`
       under `Quiet`; `runCount == 0`.
4. `restoredSolverFailureShowsBadge` (`reject_origin`, then a rewritten
   record):
   - Fit as in 3 (a rejection publishes the same shape as a solver failure;
     see Deviations 3).
   - `rewriteRecord`: set the bundle to a fresh `CalculationResult` holding
     only `_FUSION_DIAGNOSTICS = kSolverFailureDiagnostics` and reason
     `"Batch fusion did not converge (iteration limit); sensor fusion unavailable"`.
     Set `result.detail` to the same reason. The leaves and fingerprint are
     kept.
   - `kSolverFailureDiagnostics` is a JSON literal with `"algorithm"`
     `"batch-temperature-bias-v3"` and `"failure"` equal to the reason.
   - `unloadAndReload("r1")`. Check:
     - `failedCount == 1`; the reason is
       `"Sensor fusion: Batch fusion did not converge (iteration limit); sensor fusion unavailable"`;
     - the blockers are `NotProduced` with that detail;
     - `getAttribute("_FUSION_DIAGNOSTICS")` equals the literal byte for
       byte; no job; `runCount == 0`.
5. `unrelatedEditKeepsRecord` (`coarse_linear`):
   - Fit through the queue. Note `fresh` and R0.
   - `updateAttribute("a","_DESCRIPTION","renamed")` and
     `updateAttribute("a","_EXIT_TIME", kFixtureExitTime + .25)`. Check: the
     record's bytes are R0, `resultStatus == Ok`, `runCount == 1`, and
     `droppedRecordsDeleted == 0`.
   - `waitForIdle` (saved), `unloadAndReload`. Check: restored, and
     `differenceFrom(fresh)` is empty (channels, diagnostics, detail).
6. `dependencyEditDropsRecord` (`coarse_maneuver`):
   - Show, check roll, fit by gesture.
   - `updateAttribute("a","_LOCAL_ORIGIN_INDEX", qlonglong(4))`. Immediately
     (no event-loop pass), check:
     - the record file is gone, `droppedRecordsDeleted == 1`;
     - `resultStatus` is nullopt or `NotRequested`, and `availableIn("a")`
       is empty.
   - Then check:
     - `row(kRoll)`: `missingCount == 1`, `control() == Refresh`,
       `controlCount() == 1` ("exactly as today");
     - `Quiet` holds across a `PlotFixture::spin(m_requests.get())`;
     - after `waitForIdle` and `unloadAndReload`: `recordsRead == 0`, still
       not requested, `runCount == 0`.
7. `mergeIntoLoadedSessionDropsRecord` (`coarse_linear`):
   - Fit. Then `mergeSessions({incoming})`, where `incoming` holds only
     `SESSION_ID = a` and `IMU/az` with its first sample plus `1e-3` (same
     length, same unit text).
   - Check: the outcome is `Merged`; the record file is gone right after
     `mergeSessions` returns; the fit is not requested; no job.
8. `mergeIntoUnloadedSession_data` / `mergeIntoUnloadedSession`
   (`coarse_linear`; fit; `waitForIdle`; hide and evict with capacity 0
   *kept* during the merge, so that the row really is a stub). Rows:
   - `unrelatedSensor`: `incoming` holds `SESSION_ID = a` and a new sensor
     `XTRA/value = {1, 2, 3}`. After the merge: `Merged`, loaded; the fit is
     restored (`resultStatus == Ok`, `runCount == 0`,
     `differenceFrom(fresh)` empty); the record's bytes are R0;
     `statusAtLoad == Ok`.
   - `imuData`: `incoming` as in 7. After the merge: `Merged`; the record
     file is gone (`staleRecordsDeleted == 1`, check `Fingerprint`); the fit
     is not requested; `runCount == 0`.

   Both rows: `Quiet` holds.
9. `codeStampChangeDropsRecordOnLoad_data` / `codeStampChangeDropsRecordOnLoad`
   (`coarse_linear`; show; check roll; fit through the queue; hide; evict).
   Rows:
   - `compatibility`: `rewriteRecord` sets `calculationCompatibility + 1`;
   - `resultVersion`: `rewriteRecord` sets `result.resultVersion = "batch-temperature-bias-v2"`;
   - `environment`: register an OnDemand calculation `test.store.extra` (no
     inputs, output attr `_STORE_EXTRA = 1`) on the global registry. A scope
     guard unregisters it at the end of the test function, before `cleanup()`
     compares the registry.

   Then `show({"a"})` under `Quiet`. Check:
   - the record file is gone, `staleRecordsDeleted == 1`;
   - `resultStatus` is not `Ok`, `readiness(kFit).state == Ready`,
     `availableIn("a")` is empty, `runCount == 0`;
   - `row(kRoll).missingCount == 1`, `control() == Refresh`; no job.
10. `sessionFileBytesUnaffectedByRecord` (`coarse_linear`):
    - `addSessions`; note the csv bytes B0 (`sessionFilePath("a")`).
    - Fit **synchronously**: `engine("a").request(kFit).status == Ok`. This
      is the second install path at fusion level. The record exists.
    - `LogbookManager::instance().saveSession(session("a"))` returns true.
      The csv's bytes equal B0, and the record's bytes are unchanged.
    - `sessionCsvFiles()` still lists one file, and B0 contains no line that
      starts with `$COL,Fusion`.

11. `fittedBeforeFirstSaveIsRestored_data` / `fittedBeforeFirstSaveIsRestored`
    (`coarse_linear`). Rows `evict` and `restart`.
    - Call `m_model->mergeSessions({fixtureSession("coarse_linear","a")})`
      directly, **not** `addSessions` (which waits for the saver).
    - In the same event-loop pass, fit **synchronously**:
      `engine("a").request(kFit).status == Ok`. The idle saver cannot run
      during a synchronous call, so the fit is published before the first
      save.
    - Check:
      - `sessionCsvFiles()` is empty;
      - `calculationRecordFiles()` holds exactly one name, ending in
        `kRecordSuffix`;
      - `recordsWritten == 1`.
    - Note `fresh = capture("a")`, then `waitForIdle` (the saver has written
      the csv). Check that the record's name is
      `sessionFileStem("a") + kRecordSuffix`.
    - Take `Quiet`. Row `evict`: `unloadAndReload("a")`. Row `restart`:
      `restart()`, then `show({"a"})`.
    - Check:
      - `differenceFrom(fresh, "a")` is empty, and `goldenDifference` against
        `coarse_linear` is empty;
      - `runCount == 0`, `readiness(kFit).state == Done`;
      - `quiet.holds()` and no job;
      - `recordsRestored == 1`.

Never call `verifyAgainstFresh` / `evaluateFresh` on a session with the fit
installed. The oracle would run the fit again (Phase 1 gotcha).

**Acceptance Criteria:**
- [ ] `tst_fusion_store` builds, has labels `core;fusion`, a 600 s timeout,
      and passes.
- [ ] `tst_fusion_store_exact` is registered where the build registers exact
      tests (label `exact`, `FLYSIGHT_FUSION_EXACT=1`) and passes. The direct
      bit-for-bit comparisons with the fresh publish hold in both modes.
- [ ] Every spec section 8 item except the logbook-column bullet maps to a
      named test function (see the table under Testing Requirements).
- [ ] The restore-cost line appears in the test output and in the phase
      report.

**Complexity:** L

---

### Task 3.6: Catalogue rows and verification

**Purpose:** Keep `tests/README.md`'s catalogue true at this phase's commit,
and run the full suite.

**Files to modify:**
- `tests/README.md`: one catalogue row for `tst_result_store`, next to
  `tst_session_model_engine`: "Stored results of explicit calculations on a
  real model and logbook (synthetic calculations): written on an Ok install
  (also before a new session's first save), deleted on an input change, restored on every load path in passes, stale
  records deleted; temporary loads never read one; write failures." Add one
  row for `tst_fusion_store` in the fusion table: "The fit's stored result:
  bit-identical after unload and restart (also when fitted before the first
  save), rejection / solver failure badge,
  dependency and code-stamp invalidation, merges, session file untouched;
  also run as `_exact`". Nothing else in that file (Phase 5 owns it).

**Technical Approach:** Build and run as below. Write the restore-cost line
and the record/session-file sizes into the phase report.

**Acceptance Criteria:**
- [ ] `cmake --build build-phase1 --config Release` succeeds.
- [ ] `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure` passes, including `-L audit`.
- [ ] The phase report quotes the restore-cost measurement.

**Complexity:** S

## Testing Requirements

### Unit Tests
- New `tst_result_store` (core, Task 3.4): the store's rules with synthetic
  explicit calculations on a real model, logbook and queue.
- Changed `tst_jobmodel::nothingIsPersisted` (Task 3.3): exact set of the
  two record files, and everything else byte-identical.
- Phase 1's `tst_calcengine_restore` and Phase 2's `tst_result_records` must
  still pass unchanged.

### Integration Tests
- New `tst_fusion_store` and `tst_fusion_store_exact` (Task 3.5).
- Spec section 8 → test map (the column bullet is Phase 4's):

  | Spec 8 bullet | Test |
  |---|---|
  | fitted, saved, unloaded, reloaded: goldens, no job, no refresh count | `tst_fusion_store::restoredAfterEvictionIsBitIdentical` |
  | the same after a restart | `restoredAfterRestartIsBitIdentical` |
  | (coordinator) imported and fitted before the first save, restored after evict / restart | `fittedBeforeFirstSaveIsRestored`; core: `recordBeforeFirstSave`, `recordOfNeverSavedSessionIsStray`, `removeBeforeFirstSave` |
  | rejection and solver failure restored with reason, warning, no job | `restoredRejectionShowsBadge`, `restoredSolverFailureShowsBadge` |
  | unrelated edit keeps; IMU merge / dependency drops; file gone | `unrelatedEditKeepsRecord`, `dependencyEditDropsRecord`, `mergeIntoLoadedSessionDropsRecord`, `mergeIntoUnloadedSession`; core: `inputChangeDeletesRecord` |
  | compatibility / environment / result version drop on load | `codeStampChangeDropsRecordOnLoad`; core: `staleRecordDeletedOnLoad` |
  | write failure: in-memory usable, previous record intact | core: `writeFailureLeavesResultUsable`, `writeFailureKeepsPreviousRecord` |
  | deleting a session removes records; stray removed at next scan | core: `deletingSessionRemovesRecords`, `strayRecordRemovedAtRestart` (+ Phase 2's manager tests) |
  | session-file bytes identical with and without a record | `sessionFileBytesUnaffectedByRecord` |

- The whole suite, because the model sits under everything. The following
  must pass unchanged: `tst_fusion_jobs`, `tst_fusion_rows`,
  `tst_fusion_session`, `tst_jobqueue`, `tst_plot_requests`,
  `tst_session_model_engine`, `tst_column_cache` (including
  `explicitBackedColumnIsNeverCached`, which Phase 4 rewrites),
  `tst_import_merge`, `tst_logbook_index`, `tst_persistence_roundtrip` and
  `tst_session_oracle`. If another existing test fails only because a result
  now survives unload or leaves a file in the logbook, it states the old
  behaviour. Update it the way Task 3.3 does, and list it in the phase report.

### Verification commands (Michael's machine)
- Build: `cmake --build build-phase1 --config Release`. **Never build
  `build/`.** It has third-party ON and would overwrite the Boost-enabled
  solver install.
- Focused: `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure -R "tst_result_store|tst_result_records|tst_jobmodel|tst_jobqueue|tst_plot_requests|tst_session_model_engine|tst_column_cache"`
- Fusion: `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure -L fusion`
  (includes `tst_fusion_store` and, when registered, `_exact`).
- Audit: `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure -L audit`
- Full: `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure`
  (fusion tests have a 600 s timeout each).
- To run one executable by hand, put Qt's bin, GeographicLib,
  `build-solver-deps/GTSAM-install/bin` and
  `build-solver-deps/oneTBB-install/bin` on `PATH` (`tests/README.md` §2-4).
  Run `tst_fusion_store restoredAfterEvictionIsBitIdentical` to read the
  cost line.

### Manual Verification
1. Start the application built in `build-phase1`. Show a track that has IMU
   data, check a sensor-fusion plot, and press refresh. The job runs, and a
   `<uuid>.builtin%2Efusion%2Efit.fvresult` file appears in the logbook's
   `sessions/` folder.
2. Quit and start again, then show the track. The fusion plot draws at once:
   no refresh count, no job in the jobs dock.
3. Edit `_LOCAL_ORIGIN_INDEX` of that track, or merge its SENSOR file again
   with different data. The record file disappears, and the row shows the
   refresh count.
4. Delete the track. Its csv and record file are gone.

## Notes for Implementer

### Gotchas
- **Restore before `sessionLoaded`, after `attachSession`.** In
  `sessionRef()`, the listener must already be installed. If an input
  change ever happened during a restore, the record would then be dropped as
  it should. `sessionLoaded`, `visibilityChanged` and the plot passes must
  come after the restore. Do not move the restore below `evictIfNeeded`.
- **A newly created row is not in the LRU.** `mergeSessions`' created
  branch does not touch the LRU. Tests must `sessionRef` a row before
  shrinking the cache, or it will not be evicted (idiom:
  `tst_session_model_engine` 648-656).
- **Capacity 0 evicts every hidden, unfocused, unpinned row at once.** Keep
  it 0 only as long as the test needs stubs. Reset it with a scope guard.
- **A reservation is not a session file.** Keep `m_reservedStems` separate
  from `m_sessionIdToUuid`. Putting a new session into `m_sessionIdToUuid`
  at import would make `flushIndex()` list a session that has no file. After
  a crash, the next start would then create a row whose load fails, and the
  pre-save flush rule would change for every import. Only the record methods,
  `saveSession`, `removeSession`, `remapSessionId` and `reset` look at
  reservations.
- **`sessionFileStem()` (the probe) reads index.json.** It is empty for a
  session that has not been saved yet. Tests of the before-first-save path
  identify the record by its suffix until `waitForIdle` has saved the session.
- **`calculationRecordIds` is sorted by id.** The chain test depends on
  `test.store.down` sorting before `test.store.up`. Assert that premise in
  the test.
- **No `DroppedByInputChange` can come out of a load-time restore.**
  `dropNotRequested` reports only *requested* explicit entries, and a freshly
  loaded engine has none. A restore that fails caches nothing for its
  calculation (Phase 1, Task 1.4, step 7). Do not add a re-entrancy guard for
  it.
- **The merge's unloaded branch restores against the merged, unsaved
  state.** Deleting a record that an unsaved merge made stale is correct:
  once the row is saved, the record would be stale anyway.
- **Delete on every `DroppedByInputChange`, whatever its status.** The
  record, if any, describes older inputs.
- **Do not warn for expected states.** A stale record or a dropped result
  gets `qDebug` only. The manager already warns
  for write and removal failures. Tests that provoke those use
  `WarningCapture`.
- **Audit:** `emit dependencyChanged` stays on exactly one line of
  `src/sessionmodel.cpp`. Publish restore invalidations through
  `publishInvalidation`, never with a new emit. The new files must not
  contain `QProgressDialog`, `processEvents` or `QEventLoop`.
- **`fusionpipeline.h` is internal** ("Nothing outside src/fusion/ and
  tests/tst_fusion_kernel.cpp includes this"). `tst_fusion_store` must not
  include it (see Deviations 3).
- **Never build `build/`.**

### Decisions Made
- **Location:** a small non-QObject class `CalculationResultStore` in
  `src/calculationresultstore.{h,cpp}` (`flysight_core`). `SessionModel`
  owns it and it holds no state but counters. Tests can drive
  `restoreSession` directly, which is the only way to reach
  `AlreadyInstalled`.
- **Listener hookup:** in `attachSession`, next to the invalidation listener.
  It covers the job queue's publish and the synchronous `request()` with one
  code path. It captures the model, the session id and the engine (heap,
  owned by the engine).
- **A new session's stem is reserved at import** (coordinator decision). The
  code gives a new session its uuid only in `saveSession` (cpp 618-624), so
  this phase adds `LogbookManager::reserveSessionFile()`. The model calls it
  in `mergeSessions`' created branch.
  - The store writes every `Ok` result of any row with no session-file
    check. A record written before the first save is joined by its csv when
    the idle saver runs.
  - A never-saved session's records are strays for the next start-up's
    scan. `removeSession` deletes them in any case.
  - Writing the pending records after the first save was not needed, because
    the reservation is possible and smaller.
  - `hasSessionFile()` and the `writesSkipped` counter are dropped.
- **Encoding and writing on the main thread,** inside the listener. The
  write cost is measured and reported. Moving the encoding off the thread is
  left for later if the numbers call for it (the codec is pure; Phase 2).
- **Listing is the source of record ids** (`calculationRecordIds`), not the
  registry's explicit ids. That way a record of a calculation that is no
  longer registered is found and deleted at load. The cost of one filtered
  directory listing per load is part of the measured restore cost.
- **`NotFound` / `NotExplicit` at restore delete the record.**
  `InputsUnavailable` is stale only after a pass that restored nothing.
  `AlreadyInstalled` is never deleted.
- **Loaded-in-place merge: no restore** (Deviations 1).
- **Invalidated names of a restore** are discarded in `sessionRef()` and in
  the bulk-edit promotion, because nobody has read the row yet. They are
  published with the merge's names in the unloaded-merge branch.
- **Two test targets:** `tst_result_store` (core, synthetic, fast) holds the
  mechanics. `tst_fusion_store` (fusion, with an `_exact` twin) holds spec
  section 8 on the real fit.
- **Code-stamp simulation:**
  - compatibility and result version: rewrite the record through Phase 2's
    API with an altered stamp;
  - environment: register an extra calculation on the global registry
    between the write and the load.
  Re-registering the fit with another result version was rejected. It would
  unregister a calculation that `accH`, `systemTime` and the plots depend
  on, in the middle of a live model.
- **Write-failure tests** (no permission bits):
  - a directory at the record's path (the write fails, the in-memory result
    is usable);
  - an Explicit test calculation whose output is a `QVariantList`, with a
    hand-written previous record (the encoder refuses, the previous bytes
    stay).
  The previous record is written by the test, because a real previous
  publish of the same pair can only be followed by a different publish after
  an input change, and that change deletes the record.

### Open Questions
- None blocking. See the coordinator questions below.

### Deviations / questions for the coordinator
1. **Loaded-in-place merge (accepted by the coordinator).** The overview's
   Load-paths note is wrong. It says "loaded in place: its result was dropped
   by `clear()`, so re-restore after the merge". The code does not do that. `SessionMerge::apply` calls `setAttribute` /
   `mergeSourceData`, which are leaf notifications. `clear()` runs only on
   `SessionData` copy-assignment, and the model never uses that. A result the
   merge does not touch stays installed, and so does its record. A result it
   touches is dropped with `DroppedByInputChange`, and its record is deleted.
   A re-restore would find nothing to do. This phase adds none. The spec's
   behaviour ("merging IMU data drops it") is tested on both branches.
2. **Records before the first save (revised per the coordinator's
   decision).** A session created by an import has no uuid until its first
   save (logbookmanager.cpp 618-624; the created branch at sessionmodel.cpp
   600-616 and `markSessionUnsaved` do not assign one).
   - The smallest change that gives it a logbook identity at import is a
     separate reservation map, `LogbookManager::reserveSessionFile()`,
     called from the created branch. The first save uses the reserved uuid.
     Reusing `m_sessionIdToUuid` was rejected: `flushIndex()` would list a
     session with no file (see Gotchas).
   - The store then writes whenever the session has a stem, saved or
     reserved.
   - Phase 2's session-to-stem lookup of the record methods also consults
     reservations. `removeSession` handles a reserved-only session.
     `reset()` forgets reservations.
   - The stray pass at the next `initialize()` removes the records of a
     session that was never saved.
   - The fallback of writing pending records after the first save was not
     needed.
3. **Solver failure in `tst_fusion_store`.** The registered fit cannot be
   driven into `SolverFailed` from a session fixture. The kernel tests reach
   it only through a `Tuning` that the registration does not expose, and
   `fusionpipeline.h` is off-limits outside `tst_fusion_kernel`. The test
   therefore restores a record whose bundle it builds itself: the literal
   reason of `tst_fusion_kernel::nonConvergenceIsSolverFailure` and a
   diagnostics literal. The leaves and fingerprint come from a real
   `reject_origin` publish. `fusionregistration.cpp` 159-170 publishes
   `Rejected` and `SolverFailed` in the same shape, so the store path is
   identical. If the coordinator wants a kernel-produced solver-failure
   bundle, the options are an exception to the `fusionpipeline.h` rule for
   this test, or a golden of a solver-failure diagnostics string.
4. **The environment fingerprint is broad.** A record is valid only while
   `calculationEnvironmentFingerprint()` is unchanged (spec 4.2). That
   fingerprint covers *every* registration and *every* declared preference
   value. So any registry change makes every stored record stale at its next
   load, and so does any change of a declared preference, whether fusion reads
   it or not. The altitude-marker list registers calculations
   (`tst_column_cache` 920), so editing it counts. In-memory results survive
   until the session is unloaded. After that, the calculation reads not
   requested. This is what the spec prescribes, and this phase implements it
   as written. It is flagged because users may notice refits after changing
   an unrelated preference. A per-calculation environment (only what the
   result's static closure reaches) would be a spec change.
5. **`tst_jobmodel::nothingIsPersisted` changes** (Task 3.3). It asserted that
   Ok jobs leave the logbook folder byte-identical, which is no longer true.
   Phase 5 should reflect this in the acceptance map entry for jobs-dock spec
   8.4.
6. **Hand-off to Phase 4.**
   - Records change only through `writeCalculationRecord` (store,
     `onExplicitResultEvent`), `removeCalculationRecord` (store:
     `DroppedByInputChange` and stale-on-load), `removeSession`, and the stray
     pass.
   - A session not saved yet can already have a record under its reserved
     stem. index.json does not list it until the first save.
   - The column worker never restores. A stub edited through a temporary
     load (bulk edit on a stub) keeps a record that may now be stale on disk
     until its next load deletes it. Phase 4's "record exists" test for
     unloaded sessions must not assume that an existing record is valid.
   - In Phase 3, explicit-backed columns are still forced unavailable
     (`computeColumnValues`, cpp 2028-2039). `explicitBackedColumnIsNeverCached`
     and `columnOnFusionOutputIsNotCached` pass unchanged. Their comments and
     `sessionmodel.h` 70-74 ("explicit results are never saved") are left for
     Phase 4 to rewrite.
   - `SessionModel::storedResultStats()` is available as a test seam.

## Definition of Done

This phase is complete when:
1. Every task's acceptance criteria pass.
2. `cmake --build build-phase1 --config Release` succeeds, and the full
   `ctest --test-dir build-phase1/FlySightViewer-build -C Release` passes,
   audit and fusion (and `exact` where registered) included.
3. The code follows the patterns of the reference files: listener capture in
   `attachSession`, the test fixtures of `tst_jobqueue` / `tst_fusion_rows`,
   and the `tests/README.md` §8 conventions (literal expectations, helpers
   that return `QString`).
4. No TODOs or placeholder code remain. The only files changed are those
   named in the tasks, and `PLANS/`, `experiments/` and `build*/` are
   untouched.
5. The phase report quotes the restore-cost line, the record and session-file
   sizes, and any existing test that had to change beyond
   `tst_jobmodel::nothingIsPersisted`.
