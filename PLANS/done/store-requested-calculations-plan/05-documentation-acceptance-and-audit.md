# Phase 5: Documentation, acceptance map and audit

## Overview

This phase brings every document, test catalogue and code comment in line with
stored requested-calculation results (Phases 1-4). It also adds the machine-checked
evidence for the specification: a new acceptance item range, 301-350, in
`tests/acceptance_map.txt`. Each specification clause gets one item, mapped to
test functions, manual steps or audit rules. A new audit rule group,
`stored-results`, keeps the new boundaries in place. Nothing in this phase
changes behaviour. The only source edits are comments.

## Dependencies

- **Depends on:** Phases 1-4, all documented and none implemented yet. This
  document uses the names those phase documents fix: test targets, test
  functions, classes and methods. If an implemented phase deviated from its
  document, follow the code as committed and report the difference. Do not
  quietly drop an acceptance line.
- **Blocks:** None.
- **Assumptions:**
  - The five new test targets exist and pass: `tst_calcengine_restore`
    (Phase 1), `tst_result_records` (Phase 2), `tst_result_store` and
    `tst_fusion_store` (+ `tst_fusion_store_exact` where exact tests are
    registered) (Phase 3), and `tst_result_columns` (Phase 4).
  - These existing tests have been changed or renamed:
    `tst_fusion_session::registrationShape` and the new
    `restoredFitIsIndistinguishable` (Phase 1),
    `tst_jobmodel::nothingIsPersisted` (Phase 3),
    `tst_calcregistry::explicitDependencies` (new, Phase 4),
    `tst_column_cache::explicitBackedColumnFollowsItsResult` and
    `tst_fusion_jobs::columnOnFusionOutputIsCachedFromRecord` (renamed, Phase 4).
  - Phases 1, 2, 3 and 4 each added their own catalogue rows to
    `tests/README.md`: `tst_calcengine_restore`, `tst_result_records`,
    `tst_result_store`, `tst_fusion_store` and `tst_result_columns`. Do not add
    them again. Everything else in that file is owned by this phase.
  - Phases 3 and 4 already rewrote these comments: `plotrequests.h`'s ONLY
    GESTURES paragraph, `sessionmodel.h` (STORED RESULTS, CACHED COLUMN
    VALUES, `publishCalculationInvalidation`), `logbookmanager.h`,
    `calculationregistry.h` (`dependsOnExplicit`), and the headers of
    `tst_fusion_jobs.cpp` and `tst_jobmodel.cpp`. Task 5.4 only verifies
    them.
  - `PLANS/` is untracked. Nothing written in this phase may link to it or
    name a file in it. Specifications are cited by title, as appendix B and
    appendix C of `tests/README.md` already do.

## Tasks

### Task 5.1: `docs/DATA_SCHEMA.md`: record files, and section 11

**Purpose:** The data schema document must say what now lives beside a session
file, when it is valid, that the session file is untouched, and how the column
cache is tied to it (spec §9).

**Files to modify:**
- `docs/DATA_SCHEMA.md`

**Technical Approach:**

1. **§1 Summary.** Add a bullet after "Saved logbook files contain the recorded
   values, never converted ones.": the results of explicitly requested
   calculations (sensor fusion) are stored in files beside the session file,
   never in it, and are used only while still valid (section 12).
2. **§5 Source and effective values.** Keep "Effective values are ... never
   saved" (lines 124-125). It is about effective values and is still true.
   Change the last sentence of the enumeration paragraph (130-132) to say that
   `Fusion` reads unavailable until sensor fusion has been requested for the
   session **or a still-valid stored result of it was restored when the
   session was loaded (section 12)**. It still never appears in enumeration or
   in saved files.
3. **§9 Saved session files.** After "Effective values are never written."
   add: no calculated result is written either. The stored results of
   requested calculations are separate files (section 12), and saving a session
   that has one gives exactly the same bytes as saving it without.
4. **§11 Logbook column cache.**
   - After the two root-field bullets, add a paragraph: each session entry of
     `index.json` also has `"records"`. It is an object naming the requested
     calculations that have a stored result for that session, each mapped to
     its calculation's result version (`{}` when there are none). A value
     cached for a column that depends on such a calculation is valid only
     together with this stamp. The format example of Phase 4 may be shown as
     a short JSON block (`"records": {"builtin.fusion.fit":
     "batch-temperature-bias-v3"}`).
   - Replace the paragraph at 314-317 ("A column that depends on an explicitly
     requested calculation ... Loaded rows show the live value.") with one
     that says all of the following:
     - Such a column is computed from the restored or just-published result
       while the session is loaded, and cached like any other column.
     - At start-up a cached value is kept only while the session's `"records"`
       stamp matches the record files on disk and the calculations' current
       result versions. Writing or deleting a record drops it.
     - For a session that is not loaded and has no cached value, the column is
       unavailable when the session has no stored result. When it has one,
       the column stays empty (pending) until the session is loaded, because
       records are read only when a session is loaded.
     - Before a record is written whose calculation `index.json` lists as
       present under a cached value, the index is rewritten without that
       value. So a crash at any point cannot leave a cached value that
       disagrees with the records.
     - An index written before stamps existed keeps such a value only for a
       session without a record.
   - Keep the developer pointer to CALCULATIONS.md §9.
5. **New §12 "Stored calculation results"**, inserted after §11. Renumber
   "What Viewer never does" to 13 and "Adding a future schema version" to 14.
   (No inbound link or "section 12/13" reference to this document exists in
   `src`, `tests`, `docs`, `python_plugins` or `README.md`. Only sections 4, 5,
   6 and 8 are cited.) Content, in this order, for users and maintainers:
   - **What.** A result of an explicitly requested calculation (today only
     sensor fusion) is stored when the calculation publishes it: a success
     with all its outputs (measurements with samples and unit, attributes
     including the diagnostics), or a rejection or solver failure with its
     reason and diagnostics. Nothing is stored for a cancelled computation,
     one that ran out of memory, one whose inputs changed while it ran, or a
     calculation that failed. On-demand and plugin results are never stored:
     they are recomputed in milliseconds.
   - **Where.** One file per (session, calculation) in `sessions/`, next to
     the session file: `<stem>.<encoded calculation id>.fvresult`. `<stem>` is
     the session file's name without `.csv`. In the id, every byte other than
     `a-z`, `0-9`, `_` and `-` is written as `%` and two upper-case hex digits,
     so the sensor fusion record is `<uuid>.builtin%2Efusion%2Efit.fvresult`.
     A record never ends in `.csv`, so the logbook scan never takes it for a
     session. It is written through a temporary file that replaces the
     previous one at the end, like a session save. A session imported and
     computed before its first save gets its record under the name its first
     save will use.
   - **Format.** Binary, not meant to be read by people. Magic `FVRESULT`, a
     format version (1), then the two code stamps, the calculation id, the
     result version, the reason, the input fingerprint (SHA-256), the names of
     the inputs the result depended on, and the outputs. Doubles are stored
     as their eight bytes, so a restored value is bit for bit the published
     one: `-0`, NaN and infinities included. Strings round-trip byte for byte.
     A SHA-256 of the file closes it. The size is of the order of the session
     file. A record of another format version, or a damaged one, is treated
     as stale.
   - **Validity.** A record is used only while all of these hold:
     - `CalculationCompatibilityVersion`, the calculation environment
       fingerprint (both as in section 11, computed fresh at load) and the
       calculation's result version equal the ones it was written with. For
       sensor fusion the result version is the kernel's algorithm string, the
       `"algorithm"` of its diagnostics.
     - The names of the inputs the result reached (source measurements with
       their unit text, attributes, declared preferences; directly or through
       other calculations, including inputs that were absent) are the same.
     - A fingerprint over those inputs' current values equals the stored one.
     So an edit the result does not depend on (a marker, a description) keeps
     the record, and an edit it depends on (a merge that adds or changes IMU
     data, a changed `SCHEMA_VER`, a changed local origin) makes it stale.
     Because the environment fingerprint covers every registered calculation
     and every declared preference, any change to those (for example editing
     the altitude markers) also makes every record stale at its session's next
     load.
   - **Lifecycle.**
     - Written when the result is published, and replaced by the next publish
       for the same pair.
     - Deleted when an input it depends on changes, when its session is
       deleted from the logbook, when it is found stale as the session is
       loaded, and at start-up when no session file with its stem exists.
     - Never deleted by hiding a track, unloading a session, quitting, or a
       change of the registered calculations. Such a change is checked at the
       next load instead.
     - When a session is loaded, every valid record is restored before
       anything reads the session. Restoring is not requesting: nothing is
       computed. A stale or missing record leaves the calculation not
       computed until it is requested again from the plot list.
     - A write that fails (a full disk, say) leaves the previous record, if
       any, intact and the result in memory. The next publish tries again.
   - **Guarantees.** The session file is untouched: its bytes, its format,
     and what it lists do not depend on whether a record exists. Records are
     derived data. Deleting them by hand (with Viewer closed) is safe and only
     means the calculation has to be requested again. Existing logbooks have
     no records and need no migration. Records are not meant to be shared
     between logbooks or machines. One whose stamps do not match is simply
     discarded.
6. Do not add any link to `PLANS/`.

**Acceptance Criteria:**
- [ ] §1, §5, §9 and §11 contain the statements of steps 1-4; the paragraph
      "is cached as unavailable, because such results are not saved" is gone
      (`git grep -n "such results" docs/DATA_SCHEMA.md` finds nothing).
- [ ] A §12 "Stored calculation results" exists with the five parts of step 5
      (what, where, format, validity, lifecycle / guarantees); the later
      sections are numbered 13 and 14.
- [ ] The document contains the literal `<uuid>.builtin%2Efusion%2Efit.fvresult`
      and the word `"records"`.
- [ ] `audit_cleanup` passes (the `stored-results` document rule of Task 5.6
      included).

**Complexity:** M

---

### Task 5.2: `docs/CALCULATIONS.md`: sections 8, 9, 12, 15, 16 and 17

**Purpose:** The developer note must describe the result version, the
bump-rule clause, export and restore, the store, and the logbook columns
over requested results (spec §4.2, §9; Integration Notes hand-offs).

**Files to modify:**
- `docs/CALCULATIONS.md`

**Technical Approach:**

1. **§8 Explicit policy.** After "they revert to that state when an input
   changes", add two statements:
   - A result an explicit calculation installs with status `Ok` is stored
     beside the session file and restored when the session is loaded again
     (section 15.8). Restoring is not requesting.
   - A descriptor may declare `CalculationDescriptor::resultVersion`, opaque
     text that identifies the arithmetic of the calculation's results. The
     engine never interprets it. It is not part of the environment
     fingerprint. A stored result is used only while it is unchanged (section
     9). `builtin.fusion.fit` declares its kernel's `Fusion::Algorithm`
     (section 17).
2. **§9 When to bump.** After the list and before "Do not bump it ...", add a
   paragraph that starts with exactly this sentence (the audit counts it,
   Task 5.6):

   > Bump it, or the calculation's result version (`CalculationDescriptor::resultVersion`, section 8), whenever a change can alter what a requested calculation produces.

   Continue with the following points:
   - A stored result is used only while this marker, the environment
     fingerprint and the result version it was stored with all equal the
     current ones.
   - Bumping a result version drops the stored results of that calculation
     only. Bumping the marker drops every stored result and every cached
     column value.
   - After "the environment fingerprint covers those", add: since it covers
     every registration and every declared preference value, such a change
     also makes every stored result stale at its session's next load.
3. **§11 Invalidation and the model.** Add one sentence: a mutation that
   changes an input of a stored result deletes its record through the
   engine's explicit-result listener (15.8). The call site needs to do nothing
   more.
4. **§12 Asynchronous request.** Add a final part, **Export and restore**,
   after the vocabulary list and the paragraph that follows it (before §13).
   It describes the engine half (Phase 1), main thread only:
   - `exportResult(id)` returns a `StoredCalculationResult`
     (`src/engine/storedcalculationresult.h`), or nothing unless a plain
     explicit calculation has an installed result with status `Ok`. Its
     members:
     - `calculationId`, `resultVersion`, `detail`, and `bundle` (outputs in
       order);
     - `leaves`: every stored attribute, source measurement, source unit and
       declared preference reached through the recorded edges, transitively,
       through on-demand and explicit results, absent ones included. The list
       is sorted.
     - `inputFingerprint`: SHA-256 over a pinned canonical encoding. Attribute
       and preference values use the session file's text. Samples are stored
       as bits, with every NaN as one pattern and `-0` kept. Changing the
       encoding means a new magic.

     It is an inspection that reads state for the fingerprint. It never
     computes. The code stamps are not part of it (the engine does not depend
     on `src/calculations/`); the record adds them.
   - `restoreResult(snapshot)` → `RestoreOutcome {kind, staleCheck, status,
     invalidated}`:
     - Kinds: `NotFound`, `NotExplicit`, `AlreadyInstalled` (nothing
       changes; a cached result is never replaced), `Stale`, `Restored`.
     - Stale checks, in order: `ResultVersion`, `Bundle`, `InputsUnavailable`,
       `Leaves`, `Fingerprint`.
     - It gathers the inputs exactly as `prepare()` does and never runs the
       compute function.
     - A stale restore caches nothing: the calculation still reads "not
       requested".
     - A successful one installs through the same step as a publish, so
       status, detail, bundle, edges, blockers, `dependenciesOf` and later
       invalidation equal a fresh publish.
     - It counts no run and creates no ticket.
     - An outstanding ticket for the calculation then publishes as
       `RefusedStale` / `AlreadyPublished`.
     - `invalidated` must be published like `RequestOutcome::invalidated`.
   - `setExplicitResultListener(listener)` receives `ExplicitResultEvent
     {kind, instanceId, status}`:
     - `Installed` is reported for every requested install of an explicit
       calculation (synchronous `request()`, `prepare()`'s
       NothingToRun / Blocked, a `Published` publish), whatever its status.
     - `DroppedByInputChange` is reported for leaf notifications, preference
       broadcasts, and the cascade when an upstream explicit result appears.
     - Never for `restoreResult()`'s own install, `clear()`, a registry
       change, or the destruction of the registry or the engine.
     - Events are delivered at the end of the engine call, never inside an
       evaluation. The listener travels with the engine.
     - An `Installed` can be followed in the same call by a `Dropped` for the
       same result.

   Also add to §13's rules one bullet: a restored result is reported exactly
   as a published one (a stored rejection is `NotProduced` with its detail).
5. **§15 Background jobs.**
   - Intro paragraph ("Reads never start jobs."): add that loading a session
     starts none either. A stored result is restored, not requested (15.8).
   - 15.2, the `Done -> NothingToDo` clause: "(already computed, a cached
     rejection or failure included, **a restored result included**, or not an
     explicit calculation)".
   - 15.4, last paragraph: replace "A published result is not a persistent
     change: no cached logbook column is invalidated, nothing is marked dirty,
     nothing is saved." with the following. Publishing marks nothing dirty
     and saves no session file. The engine's explicit-result listener stores
     an `Ok` result as a record beside the session file during the install
     itself (15.8). Writing that record drops the session's cached values of
     the columns over that calculation (section 17).
   - 15.6, last sentence "Nothing is persisted." (line 665): "No job is
     persisted: the job history lives as long as the application. (The result
     a job published may be: 15.8.)"
   - New **15.8 Stored results** after 15.7. It describes the store (Phases 2
     and 3):
     - `CalculationResultStore` (`src/calculationresultstore.h`) is owned by
       `SessionModel`, holds no state but counters, and runs on the main
       thread only.
     - The listener is installed in `attachSession()`, so both install paths
       write: the queue's publish and a synchronous `request()`.
     - `Installed` with `Ok`: `exportResult`, then
       `CalculationRecord::stamped()`, then
       `LogbookManager::writeCalculationRecord()`, on the main thread. The
       write is atomic (`QSaveFile`). A failure is warned once, leaves the
       previous record and the in-memory result untouched, and is not retried
       before the next `Ok` publish.
     - `Installed` with any other status writes nothing and deletes nothing.
     - `DroppedByInputChange` → `removeCalculationRecord()`.
     - Every path that installs a session into a row restores that session's
       valid records after `attachSession()` and before the row is published
       (`sessionLoaded`, `dataChanged`, any plot pass): `sessionRef()`, the
       unloaded branch of `mergeSessions()`, and the promotion of a bulk
       edit's temporary session. The loaded-in-place merge needs no restore:
       it changes the session through setters, so only the results it touches
       are dropped, with their records.
     - Records are restored in passes until none restores, so explicit-on-
       explicit chains restore in any file order. `InputsUnavailable` counts
       as stale only after the last pass.
     - These are deleted: unreadable records, records of an unsupported
       format, records with stale stamps, records failing a stale check, and
       records of a calculation that is not registered as explicit. A record
       whose result is already installed is kept.
     - The column worker's and the bulk edit's temporary loads never read a
       record.
     - Records are deleted with their session (`LogbookManager::removeSession`)
       and, as strays whose session file does not exist, at `initialize()`.
       Eviction, unloading, a registry change and the model's destruction
       never delete one.
     - A session created by an import has its file stem reserved
       (`LogbookManager::reserveSessionFile`), so a result published before
       its first save is stored. A session that is never saved leaves a stray.
     - After a restore `readiness()` is `Done`, so `JobQueue::request()`
       answers `NothingToDo`.
     - Record format and file names: `src/calculationrecord.h` and DATA_SCHEMA
       §12. Test seam: `SessionModel::storedResultStats()`.
     - Tests: `tst_calcengine_restore`, `tst_result_records`,
       `tst_result_store`, `tst_fusion_store`.
6. **§16 Plot-driven requests.**
   - 16.3 (lines 827-830): `dependsOnExplicit(name)` stays the plot rows'
     query. Replace "which the logbook column cache (section 17) asks too, so
     rows and columns cannot disagree" with: the logbook column cache asks
     `CalculationRegistry::explicitDependencies(name)` (which explicit
     calculations; section 17), of which `dependsOnExplicit()` is the
     non-emptiness, so rows and columns cannot disagree.
   - 16.4 (line 871): "None of the following starts a job; the affected
     tracks are `Missing` and the refresh control shows:" becomes: none of
     the following starts a job. An affected track is `Missing` and the
     refresh control shows, unless its session was loaded with a valid stored
     result (15.8). That makes it `Available`, or `Failed` for a stored
     rejection, exactly as after a publish, and it adds nothing to the
     refresh count.
   - 16.9, "Results appear through ordinary invalidation only.": add that a
     restored result needs no publication. It is installed before the row's
     `sessionLoaded`, which already makes the plot, the legend and the rows
     read the session.
7. **§17 Sensor fusion.**
   - Outcome table, row `Rejected, SolverFailed`: add "stored and restored
     like a success (15.8)". Row `Cancelled` / `std::bad_alloc`: nothing
     stored.
   - A new paragraph **Stored results.** after "Plots.":
     - The fit's result version is `Fusion::Algorithm`
       (`"batch-temperature-bias-v3"`), the same string as the diagnostics'
       `"algorithm"`. The literal exists once in `src/`, in
       `src/fusion/fusion.h`. Change it whenever a change can alter what the
       fit returns for the same channels. That drops every stored fit.
     - The record holds the seventeen measurements and `_FUSION_DIAGNOSTICS`,
       or, for a rejection or solver failure, the diagnostics and the reason.
     - Its leaves are the source data and attributes behind the 22 inputs:
       IMU and GNSS source columns, `SCHEMA_VER`, the `TIME` sensor, the
       stored origin attributes. Markers and preferences are not among them.
   - Replace the **Logbook columns** paragraph (1139-1151). It says:
     - Which columns: a column over names for which
       `CalculationRegistry::explicitDependencies()` is not empty
       (`logbookColumnExplicitCalculations()`).
     - For a loaded row the value is computed from the engine like any other
       column: the restored or published value, or unavailable when not
       requested.
     - It is cached in `index.json` with the session's `"records"` stamp
       (calculation id → result version).
     - Writing or deleting a record drops the values over that calculation at
       once (`LogbookManager::calculationRecordsChanged`). A loaded row
       recomputes them on the next event-loop pass (`refreshRecordColumns`,
       which emits nothing: loaded cells are live).
     - A stub is settled without a load: pending (not cached, shown empty)
       when the logbook knows a record for it, unavailable otherwise. The
       column worker never reads a record.
     - The ordering rule: a record write flushes the index first when the
       index on disk lists that calculation under a cached value. Unconfirmed
       records (a failed write or removal, an environment change while
       loaded) keep their values out of `index.json` until the row is evicted
       or the record is written or deleted again.
     - Why `CalculationCompatibilityVersion` did not change: an index from
       before the stamp has explicit-backed values only as "unavailable", and
       they are kept only for sessions without a record.
     - Keep the sentence about the marker's current value 2.
   - Tests paragraph (1153-1161):
     - Add `tests/tst_fusion_store.cpp` (the fit's stored result: unload,
       restart, rejections, invalidation, merges, session file untouched).
     - Replace `tst_column_cache::explicitBackedColumnIsNeverCached` by
       `tst_column_cache::explicitBackedColumnFollowsItsResult`, and add
       `tst_result_columns` (the stamp, crash points, pending stubs) and
       `tst_fusion_jobs::columnOnFusionOutputIsCachedFromRecord`.

**Acceptance Criteria:**
- [ ] `git grep -n -e "explicit results are never saved" -e "explicitBackedColumnIsNeverCached" -e "Nothing is persisted" -- docs/CALCULATIONS.md`
      finds nothing.
- [ ] `git grep -c "Bump it, or the calculation's result version" -- docs/CALCULATIONS.md`
      is exactly 1, in §9.
- [ ] §12 names `exportResult`, `restoreResult`, `RestoreOutcome` with its five
      kinds and five stale checks, `StoredCalculationResult`, and
      `setExplicitResultListener` with `Installed` / `DroppedByInputChange`.
- [ ] §15.8 exists and names `CalculationResultStore`, `writeCalculationRecord`,
      `reserveSessionFile`, the three restoring load paths and
      `storedResultStats()`.
- [ ] §16.3 names `explicitDependencies`; §16.4 states the restored case.
- [ ] §17 names `Fusion::Algorithm`, the `"records"` stamp,
      `calculationRecordsChanged`, pending stubs, and the tests of step 7.

**Complexity:** L

---

### Task 5.3: `docs/SENSOR_FUSION.md`, `docs/COMPUTED_PLOTS.md`, root `README.md`

**Purpose:** The user-facing documents must stop saying that results are lost
on restart (spec §9: "replaces 'results are kept in memory only' by the new
behaviour"). `COMPUTED_PLOTS.md` says the same thing in two places, so it
changes too.

**Files to modify:**
- `docs/SENSOR_FUSION.md`
- `docs/COMPUTED_PLOTS.md`
- `README.md` (root)

**Technical Approach:**

1. **SENSOR_FUSION §2 "Using it"**, replace the bullet at 45-46 with this
   meaning:
   - Results are stored with the recording in the logbook (in a file beside
     the session file) and come back when the recording is loaded again,
     after hiding it or after a restart. A fitted recording is not fitted
     again.
   - A stored result is dropped when an input of the fit changes (a
     re-import or merge of different data, a changed `SCHEMA_VER`, a changed
     local origin), after an update that changes the fit's arithmetic, and
     after a change of the registered calculations or of a preference that
     calculations read. The plot then shows the refresh icon again.
   - Nothing is recomputed on its own.
2. **SENSOR_FUSION §7 "Calculation lifecycle".**
   - "The plot is the request.": after the list of what does not request, add
     that loading a recording whose stored result is still valid restores it:
     no job, no refresh count.
   - "Invalidation.": the change drops the result, every value derived from
     it, **and its stored copy**.
   - New paragraph **Stored results.**, before "One at a time.":
     - The fit's result is stored when it is published: a success, a
       rejection or a solver failure. It is restored bit for bit when the
       recording is loaded, and the restored result is indistinguishable from
       a fresh one.
     - Its code stamp is the algorithm string of the diagnostics
       (`batch-temperature-bias-v3`).
     - A cancelled fit or one that ran out of memory stores nothing.
     - A stored rejection shows the warning badge again, with its reason.
   - Replace the **Logbook columns** paragraph (374-376):
     - A loaded recording shows the live value.
     - An unloaded one shows the value cached from its stored result.
     - A recording without a stored result shows none.
     - One with a stored result whose value is not cached yet (after an
       update, say) stays empty until it is loaded.
3. **SENSOR_FUSION §8 "Validation".**
   - Add a table row: `tst_fusion_store` | the fit's stored result: bit for
     bit after unloading and after a restart (also when fitted before the
     first save), rejection and solver-failure badges, dropped by a
     dependency edit, a merge or a code-stamp change and kept by an unrelated
     edit, the session file untouched.
   - Extend the `tst_fusion_session` row with "a fit exported and restored
     into another session is indistinguishable".
   - Extend the `tst_fusion_jobs` row with "the logbook column cached from the
     stored result".
   - "CTest runs each of these six tests a second time" becomes **seven** (the
     table lists seven rows that compare with the goldens).
4. **COMPUTED_PLOTS.md.**
   - §3, after "After any of these the row simply shows the refresh icon with
     the number of tracks that are not computed.": tracks whose result was
     kept from earlier (section 6) count as computed and are drawn at once.
   - §6: replace the bullet "**When you quit.** Results are not saved. ..."
     with a bullet of this meaning: **Hiding a track or quitting loses
     nothing.** Results are kept with the recording in the logbook. When the
     track is shown again, also after a restart, the plot is drawn at once,
     without the refresh icon and without computing.
   - Add a bullet to §6: after an update of FlySight Viewer that changes how
     such a plot is computed, or after a change to calculations or to
     preferences that calculations read (for example editing the altitude
     markers), kept results are discarded when their track is next shown. The
     track is "not computed" again.
   - §9: replace the last bullet (142-143) with: a logbook column based on a
     computed plot shows the kept value for recordings that are not loaded.
     It stays blank for a recording that has never been computed. After an
     update it can stay blank until the recording is shown.
5. **Root `README.md`.**
   - Line 326-327 (docs tree comment): "saved-file guarantees" becomes
     "saved-file guarantees, stored calculation results".
   - Line 415: "and what saved files contain" becomes "what saved files
     contain, and the stored results of requested calculations".
   - Line 419: add "stored results of requested calculations" to the list for
     CALCULATIONS.md.
6. **Words to avoid** (existing audit rules):
   - In `docs/SENSOR_FUSION.md`: `frozen`, `stationary window`,
     `candidate window`, `coarse initializer`, `twenty-one`,
     `sensor-fusion-clean-port` (group `fusion-model`).
   - In any document: `EKF` / `ekf` (group `naming`).
   - The new rule of Task 5.6 also rejects these phrases: "Results are kept in
     memory only", "Results are not saved", "explicit results are never
     saved", "An explicit result is never persisted / saved", "cached as
     present and invalid", "never cached for unloaded ones".

**Acceptance Criteria:**
- [ ] `git grep -n -i -e "kept in memory only" -e "results are not saved" -e "never cached for unloaded" -- docs README.md`
      finds nothing.
- [ ] SENSOR_FUSION.md §2 and §7 state restore on load, the drop on an input
      change or code-stamp change, and that nothing recomputes by itself; §8
      has a `tst_fusion_store` row and says "seven".
- [ ] COMPUTED_PLOTS.md §3, §6 and §9 state the kept results as in step 4.
- [ ] `audit_cleanup` passes (`fusion-model`, `naming`, `stored-results`).

**Complexity:** M

---

### Task 5.4: Code comments that state the old behaviour

**Purpose:** No comment in `src/` or `tests/` may still say that explicit
results are never kept. The bump rule must name the result version, as the
audit requires.

**Files to modify** (only if the checks below find something; comments only):
- `src/calculations/builtincalculations.h`
- any file the sweep in step 2 reports

**Technical Approach:**

1. **Bump-rule clause (Phase 1, Task 1.1 step 5).** Check that the comment
   above `CalculationCompatibilityVersion` has the result-version paragraph,
   and that the paragraph names `CalculationDescriptor::resultVersion` exactly
   once: `git grep -c "CalculationDescriptor::resultVersion" -- src/calculations/builtincalculations.h`
   must be `1`. If the paragraph is missing, or its wording names the field
   differently, add or reword it with this meaning:
   > Bump it, or the calculation's result version
   > (`CalculationDescriptor::resultVersion`), whenever a change can alter
   > what a requested calculation produces. A stored result of an explicit
   > calculation is used only while this marker, the environment fingerprint
   > and the result version it was stored with all equal the current ones.
   > Bumping a result version drops the stored results of that calculation
   > only; bumping this marker drops every stored result and every cached
   > column value.

   Keep the existing rules intact. The comment must not contain
   `CalculationCompatibilityVersion =`, `fusion/` or `Fusion::`.
2. **Sweep.** Each of these commands must print nothing:
   ```
   git grep -n -E "[Rr]esults are kept in memory only|[Rr]esults are not saved|explicit results are never saved|[Aa]n explicit result is never (persisted|saved)|cached as present and invalid|never cached for unloaded ones" -- src tests docs README.md python_plugins ":!tests/README.md" ":!tests/audit"
   git grep -n -E "request nothing: the affected tracks are Missing" -- src
   git grep -n -E "Spec 8.4: nothing is persisted|nothing persisted \(sensor-fusion-jobs" -- tests
   git grep -n -E "logbook column over a fusion output is never cached|column over .* is never cached" -- tests src
   ```
   Coordinator integration note: `":!tests/audit"` is needed because, after
   Task 5.6, `cleanup_audit.cmake` spells the first sweep's phrases in its
   own rule (the audit itself excludes that directory). The second sweep
   matches the text before the line break: the old `plotrequests.h`
   sentence wraps after "refresh control", so a pattern ending in "shows"
   never matched it.
   For each hit, rewrite the comment so that it states the current behaviour,
   following the wording of the phase that changed it. Phase 3 set the
   wording for `plotrequests.h` and `tst_jobmodel`, and Phase 4 for
   `sessionmodel.h`, `logbookmanager.h`, `tst_column_cache` and
   `tst_fusion_jobs`. Change no code. List every file changed in the phase
   report.
3. **Leave as they are** (checked, still true):
   - `src/jobmodel.h` 73 "nothing is persisted": the job records.
   - `docs/DATA_SCHEMA.md` §5 "never saved": effective values.
   - `tests/tst_python_bridge.cpp` 996 / 1028: plugin outputs.
   - `docs/CALCULATIONS.md` §15.4's "unpinSession() never evicts
     synchronously".
4. **The audit comment at `tests/audit/cleanup_audit.cmake` 387-388** is
   changed in Task 5.6.

**Acceptance Criteria:**
- [ ] `git grep -c "CalculationDescriptor::resultVersion" -- src/calculations/builtincalculations.h` prints `1`.
- [ ] The four sweep commands of step 2 print nothing.
- [ ] No code line changes: `git diff --stat -- src tests` shows only
      comment edits, and none of them in files other than those listed in
      the phase report.

**Complexity:** S

---

### Task 5.5: `tests/acceptance_map.txt`: the range 301-350

**Purpose:** Every clause of the specification "Storing requested calculation
results with the session" is traceable to evidence, and the audit checks it.

**Files to modify:**
- `tests/acceptance_map.txt`

**Technical Approach:**

1. **Header (lines 1-26).**
   - "Three specifications, three item ranges" becomes "Four specifications,
     four item ranges". Add the range:
     ```
     #   301-350   storing requested calculation results with the session (record
     #             files, restore on load, validity, logbook columns over them):
     #             item = 300 + clause number. Stated in full in
     #             tests/README.md, appendix D.
     ```
   - "Every item must lie in one of the three ranges" becomes "four ranges".
   - "every item 101-120 or 201-247" becomes "every item 101-120, 201-247 or
     301-350".
   - "sections 9.1, 9.2 and 9.3" becomes "9.1, 9.2, 9.3 and 9.4".
2. **Item 118.** After the two existing `118` lines, add (the "nothing is
   persisted" of the job model, spec §8.4 of "Sensor fusion as an explicit
   calculation", changed by Phase 3 to "no job is persisted"):
   ```
   118 tst_jobmodel nothingIsPersisted
   ```
3. **Append the new block** at the end of the file, exactly as below. Item
   numbers follow the clause list of appendix D (Task 5.7). A `#` line above
   each item gives its one-line summary, in the style of the 201-247 block.

```
# ---- Storing requested calculation results with the session: item = 300 + clause number ----

# 301 - (2) the result of a requested calculation that ended as a function of its inputs is stored on disk beside the session: success with its outputs, rejection / solver failure with reason and diagnostics
301 tst_result_store writesOnOkInstall
301 tst_result_store rejectionIsWritten
301 tst_fusion_store restoredAfterEvictionIsBitIdentical
301 tst_fusion_store restoredRejectionShowsBadge

# 302 - (2) it is restored at load, so readers, plot rows and logbook columns see it as if just published, while valid
302 tst_result_store restoreOnEveryLoadPath
302 tst_calcengine_restore restoreIntoFreshEngine
302 tst_fusion_store restoredAfterRestartIsBitIdentical
302 tst_result_columns restartShowsCachedValueWithoutLoading

# 303 - (2) on-demand and plugin results are never stored
303 tst_calcengine_restore exportOnlyInstalledOk
303 tst_calcengine_restore restoreNotFound

# 304 - (2) a stored result is a memory, not a request: stale or missing, the calculation reads not requested until a gesture
304 tst_fusion_store codeStampChangeDropsRecordOnLoad
304 tst_fusion_store dependencyEditDropsRecord
304 tst_result_store staleRecordDeletedOnLoad
304 tst_calcengine_restore restoreStaleChecks
304 audit stored-results

# 305 - (2) the session file is unchanged: bytes, format, enumeration; derived sensors never in it
305 tst_fusion_store sessionFileBytesUnaffectedByRecord
305 tst_result_records saveSessionIgnoresRecords
305 tst_result_records strayRecordsRemovedAtScan
305 audit stored-results

# 306 - (2) kernel, job queue gestures and plot-row semantics unchanged beyond counting a restored result as computed
306 tst_fusion_golden successFixturesMatchGolden
306 tst_plot_requests rowScript
306 tst_fusion_rows realRowScript
306 audit gestures

# 307 - (3) at most one record per (session, calculation), written at the install on the main thread, replaced by the next publish
307 tst_result_store writesOnOkInstall
307 tst_result_records writeReadReplace
307 tst_calcengine_restore installedOnSyncAndAsync

# 308 - (3) a record holds the id, the outcome, every installed output (measurements with unit, attributes with the diagnostics) and the validity stamp
308 tst_result_records roundTripIsBitExact
308 tst_result_records rejectionShapedRecord
308 tst_result_records unavailableAndEmptyOutputs
308 tst_result_store rejectionIsWritten

# 309 - (3) nothing stored for a result not installed (cancelled, out of memory, refused) or not Ok; such a run deletes nothing
309 tst_result_store nonOkInstallWritesAndDeletesNothing
309 tst_jobmodel nothingIsPersisted
309 tst_calcengine_restore installedForEveryStatus
309 tst_calcengine_restore exportOnlyInstalledOk

# 310 - (3) a record is removed only when stale on load, with its session, on an input change, or replaced; never by eviction, unload, registry change, quit
310 tst_result_store noDeleteWithoutInputChange
310 tst_result_store inputChangeDeletesRecord
310 tst_calcengine_restore droppedByInputChange
310 tst_calcengine_restore droppedByPreferenceAndSource
310 tst_calcengine_restore noDropEventWithoutInputChange

# 311 - (3) values round-trip bit for bit (-0, NaN, infinities; strings byte for byte)
311 tst_result_records roundTripIsBitExact
311 tst_result_records layoutIsPinned
311 tst_calcengine_restore restoreNaNPayloadAndSignedZero
311 tst_fusion_session restoredFitIsIndistinguishable

# 312 - (3) the golden tests cannot tell a restored fusion result from a fresh one
312 tst_fusion_store restoredAfterEvictionIsBitIdentical
312 tst_fusion_store restoredAfterRestartIsBitIdentical
312 tst_fusion_session restoredFitIsIndistinguishable

# 313 - (4.1) the input fingerprint covers the values the result depended on, transitively, absent ones included; the record lists their names
313 tst_calcengine_restore fingerprintKnownAnswer
313 tst_calcengine_restore fingerprintCanonicalForms
313 tst_calcengine_restore leafKindCodes
313 tst_calcengine_restore exportLeaves
313 tst_calcengine_restore restoreChain

# 314 - (4.1) an edit the result does not depend on keeps the record valid
314 tst_fusion_store unrelatedEditKeepsRecord
314 tst_fusion_store mergeIntoUnloadedSession
314 tst_result_store inputChangeDeletesRecord
314 tst_calcengine_restore restoreIgnoresAttributeType

# 315 - (4.1) an edit it depends on (IMU merge, SCHEMA_VER, any dependency) invalidates it
315 tst_fusion_store dependencyEditDropsRecord
315 tst_fusion_store mergeIntoLoadedSessionDropsRecord
315 tst_fusion_store mergeIntoUnloadedSession
315 tst_calcengine_restore restoreStaleChecks
315 manual M18

# 316 - (4.2) valid only while CalculationCompatibilityVersion, the environment fingerprint and the result version match; fusion's is the algorithm string
316 tst_fusion_store codeStampChangeDropsRecordOnLoad
316 tst_result_store staleRecordDeletedOnLoad
316 tst_result_records stampsAreCurrent
316 tst_fusion_session registrationShape
316 audit stored-results

# 317 - (4.2) the bump rule gains the clause: bump it, or the calculation's result version
317 audit stored-results

# 318 - (4) a record that fails any check is deleted at load; the calculation reads not requested; nothing is recomputed
318 tst_result_store staleRecordDeletedOnLoad
318 tst_result_store upstreamMissingAfterLastPassDeletes
318 tst_fusion_store codeStampChangeDropsRecordOnLoad

# 319 - (5) every load path installs the valid records before any reader asks
319 tst_result_store restoreOnEveryLoadPath
319 tst_result_store bulkEditPromotionRestores
319 tst_result_store restoresChainInPasses
319 tst_result_store alreadyInstalledIsKept
319 tst_fusion_store mergeIntoUnloadedSession

# 320 - (5) same outputs, status, detail and dependency edges as a fresh publish; later invalidation identical
320 tst_calcengine_restore restoreIntoFreshEngine
320 tst_calcengine_restore restoredResultInvalidatesLikePublished
320 tst_calcengine_restore restoreBeatsOutstandingTicket
320 tst_calcengine_restore restoreNeverReplaces
320 tst_fusion_session restoredFitIsIndistinguishable

# 321 - (5) plot rows count a restored result as computed: no refresh count, no job; stale or absent as today
321 tst_fusion_store restoredAfterEvictionIsBitIdentical
321 tst_fusion_store restoredAfterRestartIsBitIdentical
321 tst_fusion_store dependencyEditDropsRecord
321 manual M1
321 manual M16

# 322 - (5) blocker inspection reports a restored result as a published one, NotProduced with its detail
322 tst_calcengine_restore restoreRejection
322 tst_fusion_store restoredRejectionShowsBadge
322 tst_fusion_store restoredSolverFailureShowsBadge

# 323 - (5) logbook columns over a requested calculation: computed from the restored or published result, cached with the record stamp; unavailable without a record
323 tst_result_columns columnExplicitCalculations
323 tst_result_columns unrequestedIsCachedUnavailable
323 tst_result_columns stampWrittenOnFlush
323 tst_result_columns publishedResultIsCached
323 tst_result_columns restartShowsCachedValueWithoutLoading
323 tst_result_columns noRecordStaysUnavailableAfterRestart
323 tst_result_columns recordBeforeFirstSaveIsCachedAfterSave
323 tst_calcregistry explicitDependencies
323 tst_column_cache explicitBackedColumnFollowsItsResult
323 tst_fusion_jobs columnOnFusionOutputIsCachedFromRecord
323 manual M17

# 324 - (5) dropping or writing a record drops the values stamped with it: crashes, old indexes, failed writes, environment changes
324 tst_result_columns inputChangeDropsCachedValue
324 tst_result_columns onlyDependentColumnsDrop
324 tst_result_columns staleRecordOnLoadDropsCachedValue
324 tst_result_columns workerLeavesPendingWithRecord
324 tst_result_columns crashAfterRecordWrite
324 tst_result_columns crashAfterRecordDelete
324 tst_result_columns rewriteAfterDropFlushesIndexFirst
324 tst_result_columns oldIndexWithoutStamp
324 tst_result_columns resultVersionChangeDropsCachedValue
324 tst_result_columns writeFailureKeepsValueOutOfIndex
324 tst_result_columns environmentChangeDiscardsCachedValue
324 tst_result_columns environmentChangeUnconfirmsLoadedRows
324 tst_result_columns managerDropsDependentValues
324 tst_result_columns deletingSessionRemovesStamp

# 325 - (5) unloading (eviction, hide beyond the cache capacity, quit) loses nothing
325 tst_result_store noDeleteWithoutInputChange
325 tst_fusion_store restoredAfterEvictionIsBitIdentical
325 manual M16
325 manual M17

# 326 - (5) publishing writes the record, nothing else does: a restore never rewrites it
326 tst_result_store restoreOnEveryLoadPath
326 tst_calcengine_restore restoreIsNotAnInstall
326 audit stored-results

# 327 - (5) written through the logbook manager atomically; a failed write leaves the previous record and the in-memory result; tried again at the next publish
327 tst_result_store writeFailureLeavesResultUsable
327 tst_result_store writeFailureKeepsPreviousRecord
327 tst_result_records writeFailureRefusedEncoding
327 tst_result_records writeFailureDirectoryAtPath
327 tst_result_records writeForUnknownSession
327 audit stored-results

# 328 - (5) a session fitted before its first save stores its result under its reserved stem; a never-saved session's record is a stray
328 tst_result_store recordBeforeFirstSave
328 tst_result_store recordOfNeverSavedSessionIsStray
328 tst_result_store removeBeforeFirstSave
328 tst_fusion_store fittedBeforeFirstSaveIsRestored

# 329 - (5) a record is deleted with its session and when found stale
329 tst_result_store deletingSessionRemovesRecords
329 tst_result_records removeSessionDeletesRecords
329 tst_result_records removeSessionDottedStems
329 tst_result_records failedSessionRemovalKeepsRecords
329 tst_result_records removeOneAndAll
329 manual M19

# 330 - (6) one file per (session, calculation) next to the session file, named from its stem and the id, never mistaken for a session
330 tst_result_records fileIdEncoding
330 tst_result_records fileNameParsing
330 tst_result_records fileNamesDifferIgnoringCase
330 tst_result_records strayRecordsRemovedAtScan
330 tst_result_records orphanAdoptionKeepsRecord
330 tst_result_records recordsFollowRemap
330 audit stored-results

# 331 - (6) the encoding: bit-exact doubles, readable without the session, the order of the session file's size, a version so a future format is refused
331 tst_result_records layoutIsPinned
331 tst_result_records futureVersionIsRefused
331 tst_result_records corruptInputIsRefused
331 tst_result_records readStatuses
331 tst_result_records sizeIsOrderOfSamples
331 tst_result_records encoderRefusesUnsupportedAttribute
331 tst_result_store staleRecordDeletedOnLoad

# 332 - (6) existing logbooks have no records: not requested until requested; no migration
332 tst_result_columns oldIndexWithoutStamp
332 tst_result_columns noRecordStaysUnavailableAfterRestart
332 tst_fusion_jobs columnOnFusionOutputIsCachedFromRecord

# 333 - (7) publish, restore and record writes on the main thread; the engine's threading rules unchanged
333 tst_result_store writesOnOkInstall
333 audit one-worker
333 audit stored-results

# 334 - (7) the store never reads a record for a session not being loaded, and never starts a calculation
334 tst_result_store temporaryLoadsNeverRestore
334 tst_result_columns workerLeavesPendingWithRecord
334 tst_result_columns restartShowsCachedValueWithoutLoading
334 audit stored-results

# 335 - (7) purity: with or without a record, every reader sees the same value
335 tst_calcengine_restore restoreIntoFreshEngine
335 tst_fusion_session restoredFitIsIndistinguishable
335 tst_fusion_store fittedBeforeFirstSaveIsRestored

# 336 - (7) saving a session with a stored result gives the same bytes as without
336 tst_fusion_store sessionFileBytesUnaffectedByRecord
336 tst_result_records saveSessionIgnoresRecords

# 337 - (8) test: a fusion fixture fitted, saved, unloaded, reloaded: seventeen channels and diagnostics bit-identical to the goldens, no job, no refresh count
337 tst_fusion_store restoredAfterEvictionIsBitIdentical

# 338 - (8) test: the same after an application restart
338 tst_fusion_store restoredAfterRestartIsBitIdentical

# 339 - (8) test: a rejection and a solver failure restored with their reason; the warning as today; no job
339 tst_fusion_store restoredRejectionShowsBadge
339 tst_fusion_store restoredSolverFailureShowsBadge

# 340 - (8) test: an unrelated edit keeps the record; merging IMU data or changing any dependency drops it, reads not requested, the file is gone
340 tst_fusion_store unrelatedEditKeepsRecord
340 tst_fusion_store dependencyEditDropsRecord
340 tst_fusion_store mergeIntoLoadedSessionDropsRecord
340 tst_fusion_store mergeIntoUnloadedSession
340 tst_result_store inputChangeDeletesRecord

# 341 - (8) test: bumping the compatibility version, the environment fingerprint or the result version drops the record on load
341 tst_fusion_store codeStampChangeDropsRecordOnLoad
341 tst_result_store staleRecordDeletedOnLoad

# 342 - (8) test: a failed write leaves the in-memory result usable and the previous record intact
342 tst_result_store writeFailureLeavesResultUsable
342 tst_result_store writeFailureKeepsPreviousRecord

# 343 - (8) test: deleting a session removes its records; a stray record is ignored and removed by the next scan
343 tst_result_store deletingSessionRemovesRecords
343 tst_result_store strayRecordRemovedAtRestart
343 tst_result_records removeSessionDeletesRecords
343 tst_result_records strayRecordsRemovedAtScan

# 344 - (8) test: a logbook column over Fusion/roll is cached from a valid record, unavailable without one, invalidated when the record is dropped
344 tst_fusion_jobs columnOnFusionOutputIsCachedFromRecord
344 tst_result_columns publishedResultIsCached
344 tst_result_columns unrequestedIsCachedUnavailable
344 tst_result_columns inputChangeDropsCachedValue
344 tst_result_columns staleRecordOnLoadDropsCachedValue
344 tst_result_columns deletingSessionRemovesStamp

# 345 - (8) test: saving a session with a stored result produces the same session-file bytes as without
345 tst_fusion_store sessionFileBytesUnaffectedByRecord

# 346 - (9) docs/ describe the record files and amend DATA_SCHEMA 11, CALCULATIONS 8 / 9 / 12 / 15, SENSOR_FUSION 2 / 7
346 audit stored-results

# 347 - (10) a requested result is kept as long as its inputs and its code are the same, and not a moment longer
347 tst_fusion_store unrelatedEditKeepsRecord
347 tst_fusion_store dependencyEditDropsRecord
347 tst_fusion_store codeStampChangeDropsRecordOnLoad

# 348 - (10) the session file is the recording; derived data lives beside it, never in it
348 tst_fusion_store sessionFileBytesUnaffectedByRecord
348 audit stored-results

# 349 - (10) restoring is not requesting: nothing starts on its own
349 tst_calcengine_restore restoreIsNotAnInstall
349 tst_result_store restoreOnEveryLoadPath
349 tst_fusion_store restoredAfterRestartIsBitIdentical
349 audit stored-results
349 manual M17

# 350 - (10) a restored result is indistinguishable from a fresh one, to the bit
350 tst_fusion_session restoredFitIsIndistinguishable
350 tst_fusion_store restoredAfterEvictionIsBitIdentical
350 tst_calcengine_restore restoreIntoFreshEngine
```

4. Every function above is one that Phases 1-4 name. If one does not exist
   under that name in the committed test file, report it under "Deviations"
   in the phase report. Point the line at the committed name only if that
   function asserts the same clause. Never delete a line to make the audit
   pass.

**Acceptance Criteria:**
- [ ] The header describes four ranges, including 301-350.
- [ ] `118 tst_jobmodel nothingIsPersisted` is present.
- [ ] Every item 301-350 has at least one test or audit line, and every line
      resolves (the audit of Task 5.6 checks this; it passes).
- [ ] `grep -c "^3[0-9][0-9] " tests/acceptance_map.txt` counts the lines of
      the block above (202), and no line in the file lies outside 1-19,
      101-120, 201-247 and 301-350.

**Complexity:** M

---

### Task 5.6: `tests/audit/cleanup_audit.cmake`: group `stored-results`, the comment at 387-388, the range 301-350

**Purpose:** Keep the boundaries of stored results in place with text rules
that hold for the Phase 1-4 code as documented, and extend the traceability
checker to the new range.

**Files to modify:**
- `tests/audit/cleanup_audit.cmake`

**Technical Approach:**

1. **Header (lines 1-19).** Add one bullet before the usage line: the
   stored results of requested calculations live beside the session file,
   never in it. They are named, written, read, restored and deleted in one
   place each, and the documents describe them (items 301-350).
2. **Comment at 387-388.** Replace it with:
   ```cmake
   # CalculationRegistry::explicitDependencies() is the one definition of
   # "explicit-backed" (dependsOnExplicit() is its non-emptiness): plot rows ask
   # dependsOnExplicit(), the logbook column cache asks explicitDependencies()
   # through logbookColumnExplicitCalculations(). Neither tests the policy
   # itself, and neither does the result store: what may be stored is decided by
   # CalculationEngine::exportResult().
   ```
   The rule below it is unchanged.
3. **New block** after the `fusion-tooling` group and before "leftover
   markers". Keep the style of the existing blocks: a `=====` banner, a
   `─────` group line, and an `Allow:` comment on every rule that a legitimate
   change could trip.

```cmake
# =============================================================================
# Storing requested calculation results with the session (acceptance items
# 301-350): the result of an explicit calculation is kept in a record file
# beside the session file. The rules below keep the file names and the file
# I/O in the record format and the logbook manager, the store as the one caller
# of that I/O and of the engine's export / restore, restoring separate from
# requesting, the session file ignorant of records, the result version one
# literal, and the documents current.
# =============================================================================

# ─────────────────────────────── stored-results (items 304, 305, 316, 317, 326, 327, 330, 333, 334, 346, 348, 349)
audit_group(stored-results)
# Allow: none expected. The extension and the magic are file-local constants of
# calculationrecord.cpp; everything else asks calculationRecordExtension() or the
# name functions. Header comments are not searched (a comment may quote the
# extension); a .cpp comment that quotes it is reworded.
expect_only("one authority: the record file extension" "\"\\.?fvresult\""
  "^src/calculationrecord\\.cpp$" "src/*.cpp")
# Allow: a new user of the record file names is the logbook manager or nobody.
expect_only("record file names: the record format and the logbook manager only"
  "recordFileName\\(|parseRecordFileName\\(|calculationRecordExtension\\(|calculationRecordPath\\(|calculationRecordFileNames\\("
  "^src/calculationrecord\\.(cpp|h)$|^src/logbookmanager\\.(cpp|h)$" src)
# Allow: none expected. Records are written on an Ok install, read at a load and
# deleted on an input change or when stale - all by the result store; the
# logbook manager deletes them itself with their session and as strays.
expect_only("records are written, read and removed by the result store"
  "\\b(write|read|remove)CalculationRecords?\\("
  "^src/logbookmanager\\.(cpp|h)$|^src/calculationresultstore\\.(cpp|h)$" src)
expect_only("one result store, owned by the session model" "CalculationResultStore"
  "^src/calculationresultstore\\.(cpp|h)$|^src/sessionmodel\\.(cpp|h)$" src)
# Allow: none expected. exportResult() / restoreResult() have one product caller.
expect_only("only the result store exports and restores results" "exportResult\\(|restoreResult\\("
  "^src/engine/|^src/calculationresultstore\\.(cpp|h)$" src)
expect_only("one explicit-result listener, installed by the session model" "setExplicitResultListener\\("
  "^src/engine/|^src/sessionmodel\\.cpp$" src)
expect_only("stored results are restored at a load only" "restoreSession\\("
  "^src/calculationresultstore\\.(cpp|h)$|^src/sessionmodel\\.cpp$" src)
# Restoring is not requesting. Allow: none expected; a comment that names the
# queue is reworded.
expect_none("restoring is not requesting" "JobQueue|PlotRequests|[.>](request|prepare|publish)\\("
  "src/calculationresultstore.*")
# The session file is the recording: nothing on its path knows a record exists.
# Allow: none expected.
expect_none("the session file knows nothing of stored results"
  "CalculationRecord|calculationrecord|CalculationResultStore|StoredCalculationResult|exportResult|fvresult"
  src/dataexporter.cpp src/dataexporter.h src/dataimporter.cpp src/dataimporter.h
  src/sessionmerge.cpp src/sessionmerge.h src/csvformat.cpp src/csvformat.h "src/sessiondata.*")
expect_none("stored results are widget-free"
  "QtWidgets|#include [<\"]Q(Widget|Application|MessageBox|Dialog)|#include \"(\\.\\./)?ui/"
  "src/calculationrecord.*" "src/calculationresultstore.*" "src/engine/storedcalculationresult.*")
# Fusion's result version is its kernel's algorithm string, spelled once.
# Allow: none expected. A changed algorithm changes the one literal; a comment
# or test in src that quotes it names Fusion::Algorithm instead.
expect_count("one authority: the fusion algorithm string" "batch-temperature-bias-v3" 1 src)
expect_only("one authority: the fusion algorithm string" "batch-temperature-bias-v3"
  "^src/fusion/fusion\\.h$" src)
# The compatibility rule names the result version, in the code and in the note.
# Allow: reword the sentence, never duplicate it; the count is 1 in each file.
expect_count("the bump rule names the result version" "CalculationDescriptor::resultVersion" 1
  src/calculations/builtincalculations.h)
expect_count("the bump rule names the result version (docs)" "Bump it, or the calculation's result version" 1
  docs/CALCULATIONS.md)
# Allow: tests/README.md is excluded because its section 10 describes this rule.
# Say what is kept instead of what used to be lost.
expect_none("no text says requested results are not kept"
  "[Rr]esults are kept in memory only|[Rr]esults are not saved|explicit results are never saved|[Aa]n explicit result is never (persisted|saved)|cached as present and invalid|never cached for unloaded ones"
  src tests docs python_plugins README.md ":!tests/README.md")
```

   Why each rule holds for the documented Phase 1-4 code:
   - **Extension literal.** Phase 2 Task 2.1 puts the extension and the magic
     in `calculationrecord.cpp` only. The header may quote it in a comment,
     so headers are not searched.
   - **Name functions.** Phase 2 uses them in the manager and the record
     code, and Phase 4 in `adoptCalculationRecordSet()` in the manager.
     Tests are outside `src`.
   - **I/O.** Phase 3 calls `write` / `read` / `removeCalculationRecord` from
     `calculationresultstore.cpp`. Phase 4's model calls
     `markCalculationRecordsUnconfirmed` / `discardUnconfirmedCalculationRecords`
     / `knownCalculationRecords`, which the pattern does not match (`\b`
     before `remove`, `(` right after `Record`/`Records`).
     `removeCalculationRecordsForStem(` does not match either, and it lives in
     the manager anyway.
   - **Store owner.** Phase 3 has the member and test seam in `sessionmodel.h`
     and nothing else in `src`.
   - **Export / restore / listener / `restoreSession`.** Phase 3 has one call
     site each.
   - **Starts nothing.** Phase 3's store calls `engine.exportResult(` and
     `engine.restoreResult(`, and neither matches `[.>](request|prepare|publish)\(`.
   - **Session file.** No phase touches those files.
   - **Widgets.** Phase 1 AC and Phase 3 AC require it.
   - **Algorithm string.** Phase 1 AC says one line, in `fusion.h`.
   - **Bump rule.** Task 5.4 guarantees the code count. Task 5.2 writes the
     docs sentence once.
   - **Old statements.** Phases 3 and 4 plus Tasks 5.1-5.4 remove them.

   If a rule trips on committed Phase 1-4 code anyway (a comment worded
   differently from its document), reword the **comment**. Change a rule only
   when the committed code has a legitimate second user, and report that.
4. **Traceability checker.**
   - Comment at 494-498: add "and 301-350 (storing requested calculation
     results with the session, item = 300 + clause number)".
   - Lines 567-570:
     ```cmake
     if(NOT ((item GREATER_EQUAL 1 AND item LESS_EQUAL 19) OR (item GREATER_EQUAL 101 AND item LESS_EQUAL 120)
             OR (item GREATER_EQUAL 201 AND item LESS_EQUAL 247) OR (item GREATER_EQUAL 301 AND item LESS_EQUAL 350)))
       _violation("[traceability] item ${item} is outside 1-19, 101-120, 201-247 and 301-350: ${line}")
     endif()
     ```
   - After the `foreach(item RANGE 201 247)` block:
     ```cmake
     foreach(item RANGE 301 350)
       list(FIND items_automated "${item}" index)
       if(index EQUAL -1)
         _violation("[traceability] acceptance item ${item} has no resolving test or audit line in tests/acceptance_map.txt")
       endif()
     endforeach()
     ```

**Acceptance Criteria:**
- [ ] `cmake -DREPO=. -P tests/audit/cleanup_audit.cmake` from the repository
      root prints "cleanup audit passed", with the rule count raised by 15 (the fifteen rules of the new group).
- [ ] Planted-hit proof. Do each step in turn, run the script after it, see
      the violation, and revert the plant. None of this is committed.
      - (a) Add `// restoreResult(` to `src/mainwindow.cpp` → "only the result
        store exports and restores results".
      - (b) Add `const char k[] = ".fvresult";` to `src/sessionmodel.cpp` →
        "one authority: the record file extension".
      - (c) Add `// JobQueue` to `src/calculationresultstore.cpp` → "restoring
        is not requesting".
      - (d) Add the phrase `Results are not saved` to `docs/COMPUTED_PLOTS.md` →
        "no text says ...".
      - (e) Add a map line `351 audit stored-results` → "outside ... 301-350".
      - (f) Comment out every `317` line → "acceptance item 317 has no
        resolving test or audit line".
      - (g) Add a map line `301 tst_result_store noSuchFunction` → "has no
        test function".
- [ ] The comment at 387-388 names `explicitDependencies()`.

**Complexity:** M

---

### Task 5.7: `tests/README.md`

**Purpose:** The test README must describe the suite as it is: its counts,
catalogue rows, conventions, probes, the acceptance matrix 9.4 with appendix
D, the audit group, and the manual steps. This phase owns everything except
the five new catalogue rows.

**Files to modify:**
- `tests/README.md`

**Technical Approach:**

1. **Table of contents (3-20).** Add under 12: "12.3 [Stored results](#123-stored-results)".
   Add "[Appendix D. The acceptance items of stored requested results
   (301-350)](#appendix-d-the-acceptance-items-of-stored-requested-results-301-350)".
2. **§1 counts (line 39-44).** Replace "42 test executables" / "43 entries"
   / "49 where ... the six fusion golden tests" with the numbers
   `ctest --test-dir build-phase1/FlySightViewer-build -C Release -N` shows.
   Expected: 47 executables, 48 entries, 55 with the **seven** exact runs.
   If the numbers differ, write the real ones and say why in the report.
3. **§1 catalogue rows to update** (the rows for `tst_calcengine_restore`,
   `tst_result_records`, `tst_result_store`, `tst_result_columns` and
   `tst_fusion_store` already exist; check that each exists exactly once,
   `git grep -c "^| \`tst_result_store\`" -- tests/README.md` = 1 and so on):
   - `tst_calcregistry`: "... and `dependsOnExplicit()`, the one authority for
     'explicit-backed'" becomes "... `explicitDependencies()` (which explicit
     calculations stand behind a name) and its non-emptiness
     `dependsOnExplicit()`, the one authority for 'explicit-backed'".
   - `tst_jobmodel`: "and nothing persisted (sensor-fusion-jobs acceptance
     18)" becomes "and no job persisted: the settings and the logbook folder
     are byte-identical after jobs of every ending, except for the stored
     results of the two jobs that published `Ok` (sensor-fusion-jobs
     acceptance 18; store-requested item 309)".
   - `tst_column_cache`: add "a column over an explicit result cached from
     the result the session has and following its record, shown by a stub
     after a restart without a load
     (`explicitBackedColumnFollowsItsResult`)".
   - `tst_fusion_session`: add before "The golden comparison:": "The fit
     declares its kernel's algorithm string as its result version, and a fit
     exported from one session and restored into another is indistinguishable
     from the fresh one: every channel bit for bit, the diagnostics byte for
     byte, status, detail, dependency edges, blockers and invalidation
     (`restoredFitIsIndistinguishable`)".
   - `tst_fusion_jobs`: replace "and that column is cached as unavailable
     before and after a published fit" with "and that column is cached as
     unavailable before the fit, from the published result after it (with the
     `"records"` stamp in `index.json`), shown by the unloaded row after a
     restart without a load, and dropped with the record by an input change
     (`columnOnFusionOutputIsCachedFromRecord`)".
   - The `_exact` row: add `tst_fusion_store_exact`; "the six tests above" →
     "the seven tests above"; "the next three show that the bits survive the
     engine, the queue's worker thread and the plot rows" → "the next four
     show that the bits survive the engine, the queue's worker thread, the
     plot rows and a stored and restored record".
4. **§1 notes (190-197).** After the paragraph on `registerBuiltIns()` before
   `initialize()`, add a sentence: stored results follow the same rule. A
   record is valid only while the environment fingerprint it was written with
   is current, so a test that registers a calculation between writing a
   record and loading its session sees the record deleted as stale.
   `tst_result_store` and `tst_fusion_store` use exactly that to simulate an
   environment change.
5. **§3.**
   - The `FLYSIGHT_BUILD_FUSION_TESTS` row: add `tst_fusion_store` to the
     list.
   - Labels paragraph: "six on the capture configuration" → "seven".
6. **§6 Isolation.** After the `useFreshLogbook()` / `reopenLogbook()`
   paragraph: stored-result records live in the test logbook's `sessions/`
   folder and go with it. `reset()` also forgets the file stems reserved for
   sessions not saved yet, as a process exit would.
7. **§8 Writing a test.**
   - In the `logbookprobe.h` list (515-520), add `calculationRecordFiles`
     (the `*.fvresult` names in `sessions/`), `sessionFileStem` (the stem of a
     session's file, empty before its first save) and `indexRecordStamp` (a
     session's `"records"` stamp in `index.json`).
   - Add a bullet after the `plotfixture.h` sentence. Stored results:
     - `SessionModel::storedResultStats()` / `resetStoredResultStats()` count
       records written, read, restored, kept and deleted, and the restore
       time.
     - A write failure is provoked portably by a directory at the record's
       path, or by an output attribute type a record refuses. Never use
       permission bits (Windows ignores the read-only attribute on
       directories).
     - Expected record names are literals (`<stem> + ".builtin%2Efusion%2Efit.fvresult"`).
     - Never call `verifyAgainstFresh()` / `evaluateFresh()` on a session with
       a fit installed: the oracle replays requested calculations and would
       fit again.
8. **§9 Acceptance traceability.**
   - Intro (596-597): "Three specifications, three ranges ... the three
     tables" → four.
   - 9.2: add a row after the existing row 18: `| 18 | ... and no job is persisted (the records of jobs that published Ok are results, not jobs) | \`tst_jobmodel::nothingIsPersisted\` |`.
   - New **9.4 Stored requested calculation results (items 301-350)**. Intro:
     - the fifty clauses of the specification "Storing requested calculation
       results with the session", stated in full in appendix D;
     - item = 300 + clause number; the same four line forms as 9.2, and every
       item has at least one test or audit line;
     - "Section" is the section of the specification. Clauses 37-45 are its
       section 8 tests, one per bullet, and clauses 47-50 its principles.
     Then a table `| # | Section | Clause | Evidence |` with one row per item
     301-350:
     - Section and Clause are from the item's `#` line in the map.
     - Evidence renders the item's map lines in the style of 9.3:
       `` `target::function` ``, several functions of one target
       comma-separated after the first, targets separated by `;`, then
       `` `audit <group>` `` and `` `manual M<k>` ``.
     The table and the map must list the same evidence.
9. **§10 Cleanup audit.**
   - In the `gestures` bullet (841-852), replace
     "(`CalculationRegistry::dependsOnExplicit()` is the one authority for
     "explicit-backed")" with "(`CalculationRegistry::explicitDependencies()`
     is the one authority for "explicit-backed"; `dependsOnExplicit()` is its
     non-emptiness)".
   - Add a bullet before the traceability bullet: **group `stored-results`**
     (items 304, 305, 316, 317, 326, 327, 330, 333, 334, 346, 348, 349). It
     fails when:
     - the record file extension is spelled as a literal in a `.cpp` other
       than `calculationrecord.cpp`;
     - a record file name is built or parsed outside the record format and
       the logbook manager;
     - a record is written, read or removed by anything but the result store
       and the logbook manager;
     - the result store is used outside the session model;
     - anything but the store exports or restores an engine result;
     - anything but the session model installs the explicit-result listener
       or calls `restoreSession`;
     - the store names the job queue, the plot requests or a request /
       prepare / publish call;
     - the exporter, importer, merge, number formatter or `SessionData` names
       a record;
     - the record or store code includes a widget header;
     - the fusion algorithm string appears anywhere but `src/fusion/fusion.h`;
     - the compatibility rule stops naming the result version exactly once
       in `builtincalculations.h` or in `docs/CALCULATIONS.md`;
     - any text outside this file says requested results are kept in memory
       only or are not saved.
     This file is excluded from the last rule because this section describes
     it.
   - The traceability bullet (886-889): "an item outside 1-19, 101-120 and
     201-247" → "... 201-247 and 301-350", and "an item 101-120 or 201-247 has
     no test or audit line" → "an item 101-120, 201-247 or 301-350 ...".
10. **§11.**
    - Intro (924-928): after "`tst_fusion_session`, `tst_fusion_jobs` and
      `tst_fusion_rows`" add "and, through a stored and restored record,
      `tst_fusion_store`".
    - "Fusion sessions" (1310): add `tst_fusion_store` to the list of tests
      that use `fusionsessions.h`.
11. **§12 Manual verification.**
    - Intro (1374): "Two scripts." → "Three scripts."
    - **M1** (1417), keeping its bold id and making it "(116, 321)", with this
      meaning:
      - Check "Sensor fusion > Roll" with three fusable tracks visible, press
        refresh, and cancel as soon as the first track has published.
      - Quit and restart. After the restart the row is still checked. Track
        visibility is not kept, so the row is plain.
      - Show the three tracks again. The first track's roll is drawn at once
        (its stored result). The row shows the refresh control with the
        count 2, and no job starts (no progress appears, CPU idle) until the
        control is pressed.
      - The debug output contains no "No data available" line for the fusion
        plot.
    - New **12.3 Stored results** after 12.2's closing paragraph. Say first
      that it uses the scratch-logbook preamble of 12.1 (a COPY of a logbook),
      the same recordings, and a file browser open on
      `<scratch>/FlySight Viewer/logbook/sessions/`. The steps, each opening
      with its bold id:
      - **M16 Hide and show again (321, 325).** Set Preferences > Logbook >
        "Maximum cached sessions" to 0. With three fusable tracks visible and
        "Sensor fusion > Roll" checked, press refresh and let the three fits
        finish. `sessions/` now holds three
        `<uuid>.builtin%2Efusion%2Efit.fvresult` files. Hide the three tracks;
        with a capacity of 0 every hidden track is unloaded at once. Show
        them again. Roll is drawn at once for each, the row is plain (no
        refresh icon, no count), no job starts, and the modification times of
        the three files are unchanged. Set the preference back.
      - **M17 Restart (323, 325, 349).** Add a logbook column over a fusion
        value (roll at the exit marker). With the three tracks of M16 fitted,
        quit and start again. Before any track is shown, the column shows a
        number for each of the three, and nothing loads (no progress in the
        status bar). Show them: the plots draw at once, the row is plain, no
        job starts.
      - **M18 Change an input (315).** Edit the description of one fitted
        track: its roll stays drawn and its record file keeps its
        modification time. Then re-import a copy of that track's `SENSOR.CSV`
        in which the `az` value of one `$IMU` row was changed (header,
        `SESSION_ID` included, unchanged). The track's roll disappears, the
        row shows the refresh control with 1, the record file is gone, the
        logbook cell of the column of M17 becomes empty, and nothing starts
        until refresh is pressed. Refresh fits it again, and a record file
        appears again.
      - **M19 Delete (329).** Delete one fitted track from the logbook: its
        `.csv` and its `.fvresult` files are gone. With the application
        closed, copy another track's record file to a name whose `<uuid>`
        part matches no `.csv` (change one character). Start the application:
        the copy is gone, no extra track appears, and no dialog is shown.
      - Closing sentence: pass / fail and a note per step go in the phase
        report. A step that fails is reported, not adjusted.
12. **Appendix D** after appendix C, titled "Appendix D. The acceptance items
    of stored requested results (301-350)". Intro:
    - the clauses of the specification "Storing requested calculation results
      with the session", one sentence each, with the specification's section
      number in front;
    - items 301-350 of `tests/acceptance_map.txt` (item = 300 + the number
      below) and the rows of section 9.4;
    - two sentences of its scope have no item because no test can show them:
      results are not shared between logbooks or machines, and human
      readability of a record is not required. Say so.
    Then the fifty sentences. Each one expands its `#` line of Task 5.5, using
    the wording of the specification, for example "1. (2) The result of every
    explicitly requested calculation that ended as a function of its inputs is
    stored on disk alongside the session: a successful result with all its
    outputs, or a rejection or solver failure with its reason and
    diagnostics." Clause 28 is stated as the plan settled it: "(5, as
    settled) A session imported and computed before its first save stores its
    result under the file stem reserved for it; if it is never saved, the
    record is removed at the next start."
13. Do not add any mention of `sensor-fusion-clean-port`. The audit counts it
    at exactly 4 in this file.

**Acceptance Criteria:**
- [ ] `git grep -c -E "^\| \`tst_(calcengine_restore|result_records|result_store|result_columns|fusion_store)\` \|" -- tests/README.md` is 5 (one row each).
- [ ] The counts of §1 equal `ctest -N`'s; §3 and the exact row say seven.
- [ ] 9.4 has 50 rows, 301-350, and matches the map line for line.
- [ ] Appendix D has 50 numbered clauses and the note on the two clauses
      without an item.
- [ ] `**M16 `, `**M17 `, `**M18 `, `**M19 ` exist; M1 describes the restored
      track and the count 2.
- [ ] §8 names `calculationRecordFiles`, `sessionFileStem`,
      `indexRecordStamp` and `storedResultStats`.
- [ ] §10 has the `stored-results` bullet and the 301-350 range.
- [ ] `audit_cleanup` passes.

**Complexity:** L

---

### Task 5.8: Verification

**Purpose:** Prove that the documents, map and audit agree with the code, and
that nothing regressed.

**Technical Approach:**
1. `cmake --build build-phase1 --config Release`. **Never build `build/`**:
   it has third-party ON and would overwrite the Boost-enabled solver install.
   The build re-configures only if a CMake file changed. This phase changes
   none, but a build proves the comment edits compile.
2. `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure -L audit`
3. The planted-hit proofs of Task 5.6 (with
   `cmake -DREPO=. -P tests/audit/cleanup_audit.cmake`; no build needed).
   Every plant is reverted: `git status --short` afterwards shows only the
   phase's files.
4. `ctest --test-dir build-phase1/FlySightViewer-build -C Release -N` for the
   counts of Task 5.7 step 2.
5. Full suite: `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure`
   (fusion tests have a 600 s timeout each).
6. Render check: open the four changed documents and `tests/README.md` in a
   Markdown preview. Tables must have the column counts of their header rows
   (9.4: four columns), and the anchors of the table of contents must
   resolve.

**Acceptance Criteria:**
- [ ] Build succeeds; `-L audit` passes; the full suite passes.
- [ ] Each planted hit (a)-(g) was reported by the script, and none remains.
- [ ] The phase report lists every changed file, every comment rewritten in
      Task 5.4, and any map line pointed at a committed name that differs
      from its phase document.

**Complexity:** S

## Testing Requirements

### Unit Tests
- None new. This phase adds evidence lines, not tests.
- The audit is the phase's test. The `stored-results` group, the extended
  checker and every map line must pass. Each new rule is proven by a planted
  hit (Task 5.6).

### Integration Tests
- The full suite must pass unchanged. No test file changes in this phase,
  apart from any stale comment the Task 5.4 sweep finds.

### Manual Verification
- Michael runs M1 (revised) and M16-M19 of `tests/README.md` §12 on a
  development build from `build-phase1`, on a copy of a logbook (the 12.1
  preamble). Pass / fail per step goes in the phase report.
- Read-through: `docs/DATA_SCHEMA.md` §11-12, `docs/CALCULATIONS.md` §8, §9,
  §12, §15.8, §17, `docs/SENSOR_FUSION.md` §2 and §7,
  `docs/COMPUTED_PLOTS.md` §6 and §9. Check them against the behaviour seen in
  M16-M19.

## Notes for Implementer

### Gotchas
- **The checker finds a test function by the text `::<function>()`** in
  `tests/<target>.cpp`. A function defined inside the class body has no such
  text, and its map line fails. The fix is to define it out of line, as
  every existing test does, not to drop the line.
- **Data-driven tests** (`writesOnOkInstall`, `restoreStaleChecks`,
  `codeStampChangeDropsRecordOnLoad`, `mergeIntoUnloadedSession`,
  `oldIndexWithoutStamp`, `publishedResultIsCached`,
  `fittedBeforeFirstSaveIsRestored`, `restoreOnEveryLoadPath`, ...) are cited
  by their base name, never with `_data`.
- **Exact twins are CTest tests, not executables.** The map names
  `tst_fusion_store` only. `tst_fusion_store_exact` runs the same functions.
  As in 9.2 row 4, it has no map line of its own.
- **Audit patterns are case-sensitive and line-based.** A sentence that
  breaks across lines does not match `Bump it, or the calculation's result
  version`, so keep that sentence's first part on one line in
  CALCULATIONS.md §9. And do not write "Results are not saved" anywhere
  outside `tests/README.md`, not even when quoting the old behaviour.
- **SENSOR_FUSION.md is under `fusion-model`'s text rule.** No `frozen`,
  `twenty-one`, `stationary window`, `candidate window`, `coarse
  initializer`, `bias shifts below`, `zero bias shift` or branch name.
- **`sensor-fusion-clean-port` count in tests/README.md is pinned at 4.**
  Appendix D and 9.4 must not name it.
- **The item numbers are fixed by clause order.** 9.4, appendix D and the
  map use the same numbering: 301 = clause 1. Do not renumber when adding
  evidence.
- **Do not link `PLANS/`.** Cite the specification by its title.
- **Line numbers in this document are from 8dc4e38** and are guides. Phases
  1-4 have moved them.

### Decisions Made
- **Range 301-350, item = 300 + clause number.** There is one item per
  testable clause of spec §§2-7, one per §8 test bullet (337-345), one for
  §9 (346, audit only), and one per §10 principle (347-350). Two scope
  sentences are untestable and have no item: "not shared between logbooks
  or machines", and "human readability is not required". Appendix D says so.
- **Item 328 (records before the first save)** is the plan's settled reading
  of spec §5 ("Publishing writes the record") for newly imported sessions.
  It is traced because the coordinator added it to Phase 3.
- **Duplicate evidence across items is intentional.** The §8 test items
  (337-345) repeat functions already cited by the behavioural items. The map
  holds one line per piece of evidence per item, as in the 201-247 block.
- **Where restore is documented:** the engine half goes in CALCULATIONS §12
  ("Export and restore") and the store half in a new §15.8. The spec allows
  "section 12 or 15". The two halves live in different layers, and the
  existing sections are split the same way (engine in 12-14, the application
  in 15-16).
- **DATA_SCHEMA gains a new §12**, and later sections are renumbered. There
  are no inbound references to §12/§13 (checked across `src`, `tests`,
  `docs`, `python_plugins`, `README.md`). A subsection of §9 would hide
  record files inside "Saved session files", which they are not.
- **COMPUTED_PLOTS.md and the root README change too.** They are not in spec
  §9's list, but they state the old behaviour ("Results are not saved",
  "blank ... after a restart"). §9's intent is that the documents stop
  saying so.
- **M1 is revised, not retired.** After this change a restart restores
  computed tracks, so "count 3" is no longer what the user sees. The revised
  step keeps the start-up check of item 116 and adds a restored track.
- **Audit rules are chosen for zero false positives on the documented code.**
  - The extension-literal rule searches `.cpp` files only, because a header
    comment may quote the extension.
  - The I/O rule uses `\b` and a `(` right after the name, so that the
    manager's own `...ForStem(` helpers and Phase 4's `...Unconfirmed...`
    methods cannot match.
  - There is no `QSaveFile` rule: `QSaveFile` is legitimately used by the
    exporter, the index flush and two profile writers.
  - Two rules were rejected: "temporary loads never restore" (not a text
    fact) and "on-demand results are never stored" (the existing
    `EvaluationPolicy::Explicit` confinement already keeps the store from
    deciding policy, so it can only store what `exportResult()` hands it).
    Both are carried by tests.
- **`118 tst_jobmodel nothingIsPersisted`** is added (see Deviations, 1).

### Open Questions
- None blocking.

## Deviations / questions for the coordinator

1. **"Jobs-dock spec 8.4 acceptance entry" does not exist.** `tst_jobmodel::nothingIsPersisted`
   cites "Spec 8.4". That is §8.4 "The model" of the specification "Sensor
   fusion as an explicit calculation, with plot-driven background jobs"
   ("Nothing is persisted."), not the jobs-dock specification, which has no
   implementation on this branch. `tests/acceptance_map.txt` has no line for
   `nothingIsPersisted`. The only statement of it is the `tst_jobmodel`
   catalogue row ("nothing persisted (sensor-fusion-jobs acceptance 18)").
   This phase therefore does three things:
   - it rewrites that row;
   - it adds `118 tst_jobmodel nothingIsPersisted` with a matching 9.2 row
     ("no job is persisted");
   - it cites the same function under item 309, where it shows that the
     cancelled and out-of-memory jobs store nothing.
   If the coordinator meant a different entry, only these three edits move.
2. **Two documents beyond spec §9's list state the old behaviour and are
   changed.** They are `docs/COMPUTED_PLOTS.md` (§6 "Results are not saved.
   After a restart ...", §9 "blank ... after a restart") and the manual step
   M1 of `tests/README.md` ("the row shows the refresh control with the count
   3" after a restart). Neither appears in the overview's hand-off list.
3. **CALCULATIONS §16.3 and the `gestures` bullet of tests/README §10** also
   name `dependsOnExplicit()` as what "the logbook column cache asks too". They
   are updated with the audit comment at 387-388 (Phase 4 hand-off 2).
4. **The environment-fingerprint risk** (Integration Notes, Phase 3 Deviation
   4) is now written for users. DATA_SCHEMA §12 and COMPUTED_PLOTS §6 say
   that editing the altitude markers, or changing a preference that
   calculations read, makes stored results stale at their next load. If
   Michael decides to narrow the fingerprint later, those two sentences
   change with it.
5. **Map lines depend on test functions being defined out of line** (see
   Gotchas). If a Phase 1-4 test file defines one inside its class, the
   audit fails until the definition moves. That is a test-file edit, which
   the phase report lists.

## Definition of Done

This phase is complete when:
1. Every task's acceptance criteria pass.
2. `cmake --build build-phase1 --config Release` succeeds, and the full
   `ctest --test-dir build-phase1/FlySightViewer-build -C Release` passes,
   `-L audit` included.
3. The documents follow the style of their existing sections: numbered
   sections, tables for vocabularies, no class layouts in user documents, and
   no links to `PLANS/`.
4. No TODOs or placeholder text remain. The only files changed are
   `docs/DATA_SCHEMA.md`, `docs/CALCULATIONS.md`, `docs/SENSOR_FUSION.md`,
   `docs/COMPUTED_PLOTS.md`, `README.md`, `tests/README.md`,
   `tests/acceptance_map.txt` and `tests/audit/cleanup_audit.cmake`, plus the
   comment-only edits of Task 5.4 listed in the phase report. `PLANS/`,
   `experiments/` and `build*/` are untouched.

```
Phase 5 documentation complete.
- Tasks: 8
- Estimated complexity: 16 (M, L, M, S, M, M, L, S)
- Ready for implementation: Yes with caveats (see Deviations 1, 2 and 5)
```
