# Phase 3: Record format 2, store and columns

## Overview

A stored result must go stale exactly when the same result in memory would be
dropped, plus a change of the code that computed it (spec sections 2-8). Phase 1
gave the snapshot its resolutions and made runtime registry drops reportable;
Phase 2 gave every plug-in registration a result version. This phase finishes the
change where the records live: the record loses the calculation environment
fingerprint and becomes format version 2 (format 1 is deleted as stale); a record
that cannot be read is skipped (kept, not restored) and the logbook column values
over it are never cached while it is skipped; the environment-change "unconfirmed"
handling that existed only because records carried the environment fingerprint is
removed; and the end-to-end tests of spec section 10 are written, with the tests
that asserted environment staleness rewritten to the new rules.

## Dependencies

- **Depends on:** Phase 1 (engine resolutions and registry drops), Phase 2 (plug-in code identity).
- **Blocks:** Phase 4 (documentation, acceptance map, audit).
- **Assumptions:**
  - Branch `store-requested-calculations` with Phases 1 and 2 committed on top of
    ce2fb2b. From Phase 1: `StoredCalculationResult::resolutions`
    (`StoredResolution {name, provider Nothing|SessionData|Calculation, instanceId,
    resultVersion}`), `storedResolutionLess`, the record codec's resolution section
    (added under format version 1), `RestoreOutcome::StaleCheck::Resolutions`
    (check order ResultVersion, Bundle, InputsUnavailable, Resolutions, Leaves,
    Fingerprint), runtime registry drops delivered as
    `ExplicitResultEvent::Kind::DroppedByInputChange`,
    `CalculationRegistry::unregister(id, CalculationRegistry::Removal::Teardown)`
    reporting nothing, the store's drop branch logging "an input or the registry
    changed", and `tst_result_store::noDeleteWithoutInputChange` part (b) already
    rewritten to an unrelated registration. From Phase 2: `src/plugincodeidentity.h`
    in `flysight_core` (`PluginSourceFile`, `PluginCodeIngredients`,
    `pluginCodeIdentity()`), plug-in registrations carrying it as `resultVersion`,
    `calculationEnvironmentFingerprint()` covering every registration's result
    version, `tst_column_cache::pluginEditDiscardsCachedValues`.
  - Build and test only in `build-phase1/`: `cmake --build build-phase1 --config Release`,
    then `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure`.
    **Never build `build/`.** No test executable and no source file is added, so no
    reconfigure is needed (every new helper goes into an existing support file).
  - Line numbers below are as of ce2fb2b plus Phase 1's edits and are guides only.

## Background the implementer needs

What exists today (read these before starting):

- `src/calculationrecord.h/.cpp`: `CalculationRecord {calculationCompatibility,
  calculationEnvironment, result}`, `stamped(result[, registry])` and
  `stampsAreCurrent([registry])` (compatibility AND environment, computed fresh),
  `CalculationRecordFormatVersion = 1`, `CalculationRecordStatus {Ok, Missing,
  Unreadable, NotARecord, UnsupportedVersion, Corrupt}`, the layout comment (h ~94-135,
  item 4 is the environment string, item 11 the resolutions after Phase 1, item 12
  the checksum), encoder (cpp ~226) and decoder (cpp ~315; the version is checked
  before the checksum).
- `src/calculationresultstore.cpp` `restoreSession()` (~88-192): returns at once when
  the manager knows no record of the session; otherwise lists `calculationRecordIds()`
  (files only, `QDir::Files`), reads each, deletes (`staleDelete`) on a stamp mismatch
  and on Unreadable / NotARecord / UnsupportedVersion / Corrupt, then restores in
  passes; a record still `InputsUnavailable` after a pass that restored nothing is
  deleted.
- `src/logbookmanager.h/.cpp`: the known set (`m_knownRecords`, names-only listing at
  `initialize()` plus the manager's own writes and removals), the unconfirmed set
  (`m_unconfirmedRecords`: `flushIndex()` leaves those ids out of the `"records"` stamp
  and omits every value over them; eviction calls
  `discardUnconfirmedCalculationRecords()`), `markCalculationRecordsUnconfirmed()`
  (whole session; its only caller is `SessionModel::checkCalculationEnvironment()`
  ~1382-1386), the crash table above `writeCalculationRecord()` (~958-990),
  `calculationRecordsChanged` (emitted from inside record methods; the model drops the
  row's values over the calculation and queues a refresh).
- `src/sessionmodel.cpp` `checkCalculationEnvironment()` (~1369-1406): marks every loaded
  row's known records unconfirmed ("A registry change drops explicit results from
  loaded engines and keeps their records"), then discards all cached values when the
  fingerprint differs from the index's. After Phase 1 that first paragraph is false: a
  registry change that drops a requested result deletes its record; one that does not
  reach it leaves both.
- A directory at a record's path is Unreadable to `readCalculationRecord()` (pinned by
  `tst_result_records::readStatuses`) but is never listed (`calculationRecordFileNames()`
  lists files only), so today the store never reaches it.

## Tasks

### Task 3.1: Record format 2

**Purpose:** The record carries the snapshot with its resolutions and the compatibility marker, and no longer the environment fingerprint (spec 4, 8).

**Files to modify:**
- `src/calculationrecord.h` - struct, stamps, status docs, format version and history, layout comment.
- `src/calculationrecord.cpp` - stamps, encoder, decoder, includes.

**Technical Approach:**

1. `CalculationRecord` becomes:
   ```cpp
   struct CalculationRecord {
       int calculationCompatibility = 0;   ///< CalculationCompatibilityVersion at write time
       StoredCalculationResult result;     ///< the engine's snapshot, with its resolutions
       /// The snapshot with the CURRENT code stamp.
       static CalculationRecord stamped(const StoredCalculationResult &result);
       /// calculationCompatibility == CalculationCompatibilityVersion. The
       /// calculation's own result version and the lookups (what provided each
       /// name, with its result version) are checked by
       /// CalculationEngine::restoreResult(); nothing else is.
       bool stampsAreCurrent() const;
   };
   ```
   Remove the `calculationEnvironment` member, both `const CalculationRegistry &` overloads,
   the `class CalculationRegistry;` forward declaration, and in the .cpp the
   `#include "engine/calculationregistry.h"` (keep `calculations/builtincalculations.h`
   for `CalculationCompatibilityVersion`). Struct comment: "the engine's snapshot plus
   the code stamp that was current when it was written"; add one sentence: "The
   calculation environment fingerprint is not part of a record: what else is registered
   never makes a stored result stale (plan stored-results-validity)."

2. `CalculationRecordFormatVersion = 2`. Its comment keeps "Changing anything after the
   version field ... bumps it" and gains a history: "1 - first format (the environment
   fingerprint as a second stamp; the resolutions were added late, without a bump); 2 -
   the environment fingerprint left the record; the resolutions are section 10. A record
   of any other version is UnsupportedVersion and is deleted as stale when its session
   loads; there is no migration."

3. `CalculationRecordStatus::Unreadable` doc: "the file (or whatever is at its path)
   exists but could not be opened or read; the result store skips it: kept, not
   restored".

4. Layout comment, format version 2 (renumbered; the Phase 1 note about "section 11 ...
   format 2" is replaced by the history above):
   ```
   //    1  magic                       8 raw bytes "FVRESULT"
   //    2  format version              quint32 (2)
   //    3  calculation compatibility   qint32
   //    4  calculation id              QString, non-empty
   //    5  result version              QString
   //    6  reason / detail             QString (the bundle's reason(); written once)
   //    7  input fingerprint           QByteArray, InputFingerprintSize bytes
   //    8  leaf count                  quint32
   //   8a  per leaf                    quint8 storedLeafKindCode, QString a, QString b
   //    9  output count                quint32
   //   9a  per output ...              (unchanged text of the former 10a)
   //   10  resolution count            quint32
   //  10a  per resolution ...          (unchanged text of Phase 1's 11a)
   //   11  checksum                    32 raw bytes: SHA-256 of every preceding byte
   ```

5. Encoder: drop `stream << record.calculationEnvironment;` (the only byte change besides
   the version). Decoder: drop `record.calculationEnvironment` from the header read
   (`stream >> compatibility >> result.calculationId >> ...`). Everything else, including
   every error text, is unchanged. The version check (before the checksum) refuses 1
   with "format version 1 is not supported".

6. `stamped()` sets `calculationCompatibility = CalculationCompatibilityVersion` and
   `result`; `stampsAreCurrent()` compares that one field.

**Acceptance Criteria:**
- [ ] `grep calculationEnvironment src/calculationrecord.h src/calculationrecord.cpp` finds nothing; `CalculationRecordFormatVersion == 2`.
- [ ] `tst_result_records::layoutIsPinned` matches the version-2 bytes of Task 3.6 exactly.
- [ ] A format-1 record (either layout, Task 3.6) decodes as `UnsupportedVersion` with the error "format version 1 is not supported" and leaves `*out` untouched.
- [ ] `stampsAreCurrent()` is true for `stamped()` output, false for any other compatibility value, and stays true after a registration changes `calculationEnvironmentFingerprint()`.

**Complexity:** S

---

### Task 3.2: The store skips unreadable records

**Purpose:** A record that exists but cannot be read is kept and not restored, and so is a record that is unrestorable only because it reads a skipped one (spec 7, principle "a transient failure to read is not evidence that a record is wrong").

**Files to modify:**
- `src/calculationresultstore.h` - class comment, `Stats`, `RestoreSummary`.
- `src/calculationresultstore.cpp` - `restoreSession()`, comments.

**Technical Approach:**

1. `Stats`: add `int recordsSkipped = 0; ///< kept, not restored: could not be read, or read the result of one that could not`. Change the `staleRecordsDeleted` doc to "deleted by restoreSession(): not a record, damaged, another format version, another compatibility marker, stale (RestoreOutcome), unknown calculation". `recordsRead` doc: "records read by restoreSession() (listed or known ids)". `RestoreSummary` gains `int skipped = 0;`.

2. Class comment bullets (after Phase 1's): "restoreSession() installs a session's valid records into its engine and deletes the stale ones. A record that exists but cannot be read (Unreadable) is skipped: neither restored nor deleted; the calculation reads as not requested and the logbook manager keeps the column values over it out of index.json (LogbookManager::markCalculationRecordSkipped()). A record whose inputs stay unavailable only because it reads the result of a skipped record is skipped too. The next load tries again."

3. `restoreSession()`:
   - Step 1: keep the early return when `knownCalculationRecords(sessionId)` is empty and the `recordListings` count. The ids to read are the union of `calculationRecordIds(sessionId)` (the listing) and the known set, sorted (`QStringList::sort()`). Comment: the listing finds records the known set does not (a calculation no longer registered, a file put there behind the application's back while another record was known); the known set finds a record the listing cannot see because something other than a file stands at its path (a directory), which must be read - and found Unreadable - rather than silently ignored. A known id with nothing at its path reads Missing and is passed over, as today.
   - Step 2, per status: `Missing` - nothing (unchanged); `Ok` - `stampsAreCurrent()` false deletes with the reason "calculation compatibility changed", else pending (unchanged); `Unreadable` - skip (below); `NotARecord`, `UnsupportedVersion`, `Corrupt` - delete with `read.error` (unchanged).
   - Skip: a local lambda `skip(id, reason)` calls `LogbookManager::instance().markCalculationRecordSkipped(sessionId, id)` (Task 3.3), inserts `id` into a local `QSet<QString> skipped`, increments `summary.skipped`, and warns:
     `qWarning("CalculationResultStore: stored %s of %s skipped (kept for the next load): %s", id, sessionId, reason)`.
     Unreadable passes `read.error` as the reason.
   - Step 3 (passes) is unchanged until the final "no progress" branch. There, instead of deleting every remaining record: repeat a scan of the remaining records, skipping (reason "it reads a stored result that could not be read") each record whose `result.resolutions` has an entry with `provider == StoredResolution::Provider::Calculation` and `instanceId` in `skipped`, until a scan skips none; delete the rest with "inputs unavailable" as today. (A requested result's resolutions cover every name reached through Result nodes, explicit results included, so a record that reads a skipped explicit result names it directly; the repeat only makes the rule independent of that.)
   - Step 4: `m_stats.recordsSkipped += summary.skipped;` with the other counters.
   - The `NotFound` / `NotExplicit` branch comment becomes: "The calculation is not registered, or not as an explicit one (a plug-in calculation removed, an id re-registered on demand): no record is kept that can never be used." Behaviour unchanged (deleted).

4. Nothing else in the store changes: `onExplicitResultEvent()` already writes on an Ok install and deletes on every `DroppedByInputChange` (Phase 1 made that include runtime registry drops).

**Acceptance Criteria:**
- [ ] An Unreadable record is neither restored nor deleted, is counted in `recordsSkipped`, warns once with "skipped (kept for the next load)", and is marked with `markCalculationRecordSkipped` (`tst_result_store::unreadableRecordIsSkipped`).
- [ ] A readable record whose only obstacle is a skipped upstream record is skipped, not deleted (`tst_result_store::dependentOfSkippedRecordIsKept`); with the upstream record removed instead, it is still deleted (`upstreamMissingAfterLastPassDeletes` unchanged).
- [ ] NotARecord, Corrupt, UnsupportedVersion (format 1 included), another compatibility marker, every `Stale` outcome and an unknown calculation still delete (`staleRecordDeletedOnLoad`, `formatOneRecordIsDeletedOnLoad`).
- [ ] A directory at a known record's path is read (Unreadable) and skipped.

**Complexity:** M

---

### Task 3.3: The logbook manager: skipped records are unconfirmed; the environment marking goes

**Purpose:** Column values over a skipped record never reach `index.json` while it is skipped (spec 7, last paragraph), reusing the unconfirmed mechanism; the whole-session marking that existed only for environment changes is removed.

**Files to modify:**
- `src/logbookmanager.h` - new method, removed method, class comment (RECORD STAMPS), `readCalculationRecord` and signal docs.
- `src/logbookmanager.cpp` - the method, the crash table row.

**Technical Approach:**

1. Remove `markCalculationRecordsUnconfirmed(const QString &sessionId)` (declaration, definition, header comment). Its only caller goes in Task 3.4.

2. Add, next to `discardUnconfirmedCalculationRecords()`:
   ```cpp
   // A record of the session exists but was not restored at the session's load
   // for a reason that may pass (CalculationResultStore: it could not be read,
   // or it reads the result of one that could not). The loaded engine does not
   // hold what the record holds, so the pair is UNCONFIRMED until the record is
   // written or removed, or the row is evicted: values over it stay out of
   // index.json and the stamp leaves it out. Drops the session's cached values
   // over the calculation and emits calculationRecordsChanged(). The known set
   // is untouched (the file is still there). Nothing for an unknown session.
   void markCalculationRecordSkipped(const QString &sessionId, const QString &calculationId);
   ```
   Definition: `if (recordStem(sessionId).isEmpty()) return;` then
   `m_unconfirmedRecords[sessionId].insert(calculationId); dropRecordDependentValues(sessionId, {calculationId}); emit calculationRecordsChanged(sessionId, calculationId);`
   (the same three steps as `writeCalculationRecord()`'s `fail` lambda, ~1011-1016).

3. Class comment, RECORD STAMPS: "Ids whose record may disagree with the loaded session's engine (a failed write or removal, a record skipped at the session's load) are UNCONFIRMED". `unconfirmedCalculationRecords()` comment unchanged. `readCalculationRecord()` comment: "Unreadable when the file, or whatever stands at its path, cannot be opened / read". Signal comment: "A record of (sessionId, calculationId) was written, removed, skipped at a load, or a write or removal of it failed."

4. Crash table (above `writeCalculationRecord`), one row after "a failed write":
   ```
   //   a record skipped at a load           as last flushed before  present    V agrees with the record
   //     (unreadable)                         the load, or no V and            (it was computed from it),
   //                                          e absent (a flush while          or V missing: pending
   //                                          skipped: unconfirmed)            until loaded
   ```

5. `flushIndex()`, `validateRecordStamps()`, `discardUnconfirmedCalculationRecords()`, the known set and `m_recordBackedOnDisk` are unchanged: a skipped pair is exactly an unconfirmed one.

**Acceptance Criteria:**
- [ ] `grep -rn markCalculationRecordsUnconfirmed src tests` finds nothing.
- [ ] After a skip, `unconfirmedCalculationRecords(s)` contains the id, `knownCalculationRecords(s)` still does, and a flushed `index.json` has no value over it and no stamp entry for it; after eviction the unconfirmed set is empty and the row's column is pending (`tst_result_columns::skippedRecordValuesStayOutOfIndex`).
- [ ] `markCalculationRecordSkipped` for an unknown session changes nothing and emits nothing.

**Complexity:** S

---

### Task 3.4: The session model and the bump rule

**Purpose:** Remove the environment-change marking (it existed only because records carried the environment fingerprint) while keeping the index-level discard; bring the comments that describe the old rules up to date.

**Files to modify:**
- `src/sessionmodel.cpp` - `checkCalculationEnvironment()`, eviction comment, `restoreStoredResults()` comment.
- `src/sessionmodel.h` - STORED RESULTS paragraph.
- `src/calculations/builtincalculations.h` - the stored-result sentences of the `CalculationCompatibilityVersion` comment.

**Technical Approach:**

1. `checkCalculationEnvironment()` (~1369): delete the comment block beginning "A registry change drops explicit results from loaded engines and keeps their records" and the loop calling `markCalculationRecordsUnconfirmed`. Keep `rebuildColumnDependencies()`, the fingerprint comparison with `logbook.cacheEnvironment()`, `discardCachedValues()`, the row clears, `dataChanged` and `startColumnWorker()` exactly as they are. `LogbookManager &logbook` is still needed for the comparison.

2. Eviction (~1934): "Values over records the engine could not vouch for (a failed write or removal, a record skipped at the load) go with the engine; ...".

3. `restoreStoredResults()` (~1305): "... (a stale record it deletes, or one it skips, drops its values through the manager)."

4. `sessionmodel.h` STORED RESULTS (~137-151): "A result that an input change, or a registry change made while the application runs, drops deletes its record. Every path that installs a session into a row ... restores the session's valid records ... and deletes the stale ones; a record that cannot be read is skipped (kept for the next load, its column values never cached meanwhile). ... Eviction, unloading, a registration removed as teardown, the model's destruction and removeSessions() never delete one (LogbookManager::removeSession does, with the session file)."

5. `builtincalculations.h`, the paragraph starting "Bump it, or the calculation's result version": keep the one line holding `(CalculationDescriptor::resultVersion)` (the audit counts exactly one such line in this file) and rewrite the rest to:
   "Bump it, or the result version of the calculation concerned
   (CalculationDescriptor::resultVersion), whenever a change can alter what a
   requested calculation, or anything it reads, produces. A stored result of an
   explicit calculation is used only while this marker and the result version it
   was stored with equal the current ones, and every name it looked up still
   resolves to the same provider with the same result version. Bumping a result
   version drops the stored results of that calculation and of every requested
   calculation whose lookups went through it; bumping this marker drops every
   stored result and every cached column value."
   Do not touch the lines Phase 2 changed; write no `fusion/`, `Fusion::` or algorithm literal.

**Acceptance Criteria:**
- [ ] `checkCalculationEnvironment()` touches no record state; a registry change that reaches no requested result leaves `unconfirmedCalculationRecords()` empty for every loaded row (`tst_result_columns::registryChangeKeepsLoadedRowConfirmed`).
- [ ] An environment change still discards every cached column value (`environmentChangeDiscardsCachedValue`, `tst_column_cache::altitudeMarkerChangeDiscards`, `preferenceChangeDiscardsUnloadedRows` pass).
- [ ] No comment in `src/` states that a registry change keeps records or that the environment fingerprint makes stored results stale (`grep -rn -i "environment" src/calculationresultstore.* src/calculationrecord.* src/sessionmodel.*` shows only the column-cache meaning).
- [ ] The audit passes (`CalculationDescriptor::resultVersion` on exactly one line of `builtincalculations.h`).

**Complexity:** S

---

### Task 3.5: Test support

**Purpose:** Shared helpers for making a record unreadable by a platform-honoured mechanism, building a genuine format-1 record, adding extra registrations safely, and a fusion session whose `GNSS/sAcc` only a calculation can provide.

**Files to modify:**
- `tests/support/logbookprobe.h/.cpp` - `UnreadableFile`, `asFormatOne()`.
- `tests/support/jobfixture.h/.cpp` - `ExtraRegistrations`.
- `tests/fusion/fusionsessions.h/.cpp` - `fixtureSessionWithSAccStoredAs()`.

**Technical Approach:**

1. `UnreadableFile` (logbookprobe, section "calculation records"):
   ```cpp
   /// Makes an existing file unreadable to QFile::open() while it lives, and puts
   /// it back (same path, same bytes, readable) on release() or destruction.
   /// A mechanism the platform does not honour changes nothing and says why in
   /// skipReason(); the caller QSKIPs the row.
   class UnreadableFile {
   public:
       enum class Mechanism {
           Directory,             ///< every platform: the file is removed and a directory made at its path
           LockedWithoutSharing,  ///< Windows only: held open with share mode 0 (CreateFileW)
           NoReadPermission       ///< not Windows: permissions cleared; skipped when it stays readable (root)
       };
       UnreadableFile(const QString &path, Mechanism mechanism);
       ~UnreadableFile();                       // release()
       Q_DISABLE_COPY_MOVE(UnreadableFile)
       QString skipReason() const;              ///< empty when the path exists and QFile cannot open it for reading
       bool release();                          ///< idempotent; false when the file could not be put back
   private:
       QString m_path; Mechanism m_mechanism; QByteArray m_bytes; QString m_skip;
       void *m_handle = nullptr;                ///< Windows HANDLE while locked
       bool m_applied = false;
   };
   ```
   The constructor reads the bytes first (`readFileBytes`-style; failure sets `m_skip`). Directory: `QFile::remove` then `QDir().mkdir(path)`; release: `QDir().rmdir`, write `m_bytes` back. LockedWithoutSharing (under `#ifdef Q_OS_WIN`, with `WIN32_LEAN_AND_MEAN` and `NOMINMAX` defined before `<windows.h>`, in the .cpp only): `CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)`; `INVALID_HANDLE_VALUE` sets `m_skip` with `GetLastError()`; release: `CloseHandle`. Elsewhere `m_skip = "Windows only"`. NoReadPermission (not Windows): `QFile::setPermissions(path, QFileDevice::Permissions())`; release restores `ReadOwner | WriteOwner | ReadUser | WriteUser`; on Windows `m_skip = "not on Windows"`. After applying any mechanism, probe: if `QFileInfo::exists(path)` is false or `QFile(path).open(QIODevice::ReadOnly)` succeeds, release at once and set `m_skip` ("the file stays readable here (running as root?)" / "nothing at the path"). Document on the class: a directory at a record's path is never listed as a record (the listing takes files only), so the store reaches it only through the ids the logbook manager knows - within a run, not after a restart.

2. `QByteArray asFormatOne(const QByteArray &formatTwo)` (logbookprobe): "the same record in the layout of format version 1 as Phase 1 of stored-results-validity wrote it: version field 1 and the environment fingerprint string (40 '0' characters, QDataStream form pinned to Qt_6_0 little-endian) after the compatibility stamp, with a fresh SHA-256 trailer; an empty array unless `formatTwo` holds at least 48 bytes with version field 2." Build: `formatTwo.first(8)` + little-endian `quint32(1)` + `formatTwo.mid(12, 4)` + the serialized string + `formatTwo.mid(16, formatTwo.size() - 16 - 32)`, then append `QCryptographicHash::hash(..., Sha256)`. Expected bytes are not recomputed with the codec: it only splices.

3. `ExtraRegistrations` (jobfixture, after `JobWorld`; add `#include "engine/calculationdescriptor.h"` and `"engine/calculationregistry.h"`):
   ```cpp
   /// Calculations one test function adds to the GLOBAL registry beyond a
   /// JobWorld. Destroy it after the JobQueue and the SessionModel are gone (as
   /// JobWorld): the destructor unregisters, newest first and as runtime
   /// changes, every id it registered and still holds.
   class ExtraRegistrations {
   public:
       ExtraRegistrations() = default;
       ~ExtraRegistrations();
       Q_DISABLE_COPY_MOVE(ExtraRegistrations)
       [[nodiscard]] bool add(const FlySight::CalculationDescriptor &d);
       [[nodiscard]] bool addFamily(const FlySight::CalculationFamily &f);
       /// Unregisters and forgets `id`; false when it is not held or the registry refuses.
       [[nodiscard]] bool remove(const QString &id,
                                 FlySight::CalculationRegistry::Removal removal = FlySight::CalculationRegistry::Removal::Change);
       QStringList ids() const;    ///< held, in registration order
   };
   ```

4. `fixtureSessionWithSAccStoredAs(const QString &fixtureName, const QString &sessionId, const QString &storedAs)` (fusionsessions): `fixtureSession()` with the GNSS/sAcc source data stored as `GNSS/<storedAs>` (same samples, unit "m/s"): give `addGnssSide()` a `const QString &sAccName = QStringLiteral("sAcc")` parameter and build like `fixtureSession()` (identity, GNSS side with the new name, IMU side, `_EXIT_TIME`), without the `inputsMatchFixture` assertion. Header doc: "GNSS/sAcc then has no source data and no built-in candidate, so only a registered calculation can provide it (tst_fusion_store's lookup test)."

**Acceptance Criteria:**
- [ ] The support library builds on every platform (the Windows code is under `Q_OS_WIN`; `<windows.h>` is included by the .cpp only).
- [ ] `UnreadableFile` with `Directory` makes `LogbookManager::readCalculationRecord()` of that path return `Unreadable`, and `release()` restores the exact bytes.
- [ ] `asFormatOne(v2 bytes)` decodes as `UnsupportedVersion` "format version 1 is not supported".
- [ ] `ExtraRegistrations` leaves `CalculationRegistry::instance().registeredIds()` as it found it after destruction.

**Complexity:** M

---

### Task 3.6: `tests/tst_result_records.cpp`

**Purpose:** Pin format 2 and the stamp without the environment.

**Files to modify:**
- `tests/tst_result_records.cpp`

**Technical Approach:**

1. Header comment: "the code stamp (the calculation-compatibility marker), computed fresh; the environment fingerprint is not a stamp"; "refusal of other format versions, the previous one (1) included".
2. Remove every use of `calculationEnvironment`: `recordDifference()` (the "environment" comparison), `largeSampleRecord()`, `unavailableAndEmptyOutputs()` (the assignment and the final `QCOMPARE`), `layoutIsPinned()`, `corruptInputIsRefused()` (`out.calculationEnvironment = "untouched"` becomes `out.calculationCompatibility = 12345` with the matching `QCOMPARE`).
3. `craft()`: header `s << quint32(2) << qint32(7) << QStringLiteral("x.y") << QStringLiteral("v1") << QString() << QByteArray(32, '\xAB');` and its comment "(magic, version 2, compatibility 7, id "x.y", version "v1", null reason, 32-byte fingerprint), with `body` writing the leaves, outputs and resolutions". Every crafted row keeps its body and expected text.
4. `stampsAreCurrent()` (keep the name, acceptance map 316): `stamped(sampleSnapshot())` has `calculationCompatibility == 2` and `stampsAreCurrent()`; compatibility 3 is not current; then register `kExtraId` (as today), `QVERIFY(calculationEnvironmentFingerprint()` differs from the value taken before the registration`)` and `QVERIFY(record.stampsAreCurrent())` - a registration never makes a record stale by itself.
5. `layoutIsPinned()`: the expected prefix is the Phase 1 one minus the line `"02000000" "6500"` (environment), with `"02000000"` as the format version and the comments renumbered (1 magic, 2 version 2, 3 compatibility 7, 4 id, 5 result version, 6 reason, 7 fingerprint, 8/8a leaf, 9/9a outputs, 10/10a resolutions); comment "format version 2 (plan stored-results-validity)".
6. `futureVersionIsRefused_data()`: versions `{0, 1, 3, 0xFFFFFFFFu}` (keep the name, map 331).
7. New `formatOneIsRefused_data()/formatOneIsRefused()`: a local `craftFormatOne(bool withResolutions)` writes magic, `quint32(1)`, `qint32(2)`, `QString(40, '0')`, "x.y", "v1", null reason, 32 bytes, `quint32(0)` leaves, `quint32(0)` outputs, then `quint32(0)` resolutions only when `withResolutions`, and a correct SHA-256 trailer. Rows `"without resolutions (ce2fb2b)"` and `"with resolutions (Phase 1)"`, plus a row built with `asFormatOne(encoded(recordFor("x")))`. Each decodes as `UnsupportedVersion` with "format version 1 is not supported" and `out.calculationCompatibility` still 12345.
8. `readStatuses()`: the "z" record is `patchVersion(xBytes, 3)` with "format version 3 is not supported".

**Acceptance Criteria:**
- [ ] The file compiles without `calculationEnvironment`; every function passes; `formatOneIsRefused` has three rows.
- [ ] No existing function is renamed or removed.

**Complexity:** M

---

### Task 3.7: `tests/tst_result_store.cpp`

**Purpose:** Store-level tests of spec 5, 6, 7, 8 and the rewrite of the environment row.

**Files to modify:**
- `tests/tst_result_store.cpp`

**Technical Approach:**

Infrastructure: include `calculations/builtincalculations.h`, `plugincodeidentity.h`; member `std::unique_ptr<ExtraRegistrations> m_extra` created in `init()` after `m_store` and before the model, reset in `cleanup()` after `m_model.reset()` and before `m_store.reset()`; `restart()` becomes `restart(const std::function<void()> &whileClosed = {})`, calling `whileClosed` after the model is gone and before `reopenLogbook()` (as `tst_fusion_store::restart`). Header comment: add "a registry change made while the application runs that reaches a result deletes its record, a teardown removal does not; a registry or preference change that does not reach it leaves the record valid across loads and restarts; a record whose lookups resolve differently at load, or whose plug-in provider's code identity changed, is stale; an unreadable record is skipped and restored later; a format-1 record is deleted", and replace "Nothing depends on permission bits" by "Only the unreadable rows use a platform mechanism (a lock on Windows, permission bits elsewhere) and skip where it is not honoured."

1. `staleRecordDeletedOnLoad_data/()` (keep the name): row `environment` becomes `resolutions`: `rewriteRecord("s1", kExpA, [](CalculationRecord &r) { r.result.resolutions.first().provider = StoredResolution::Provider::Nothing; })` (expA's only resolution is `EA_IN SessionData`). Row `unsupportedVersion` writes `"\x03\x00\x00\x00"`. The `kExtra` guard of the old row is removed. Every row still deletes the altered record with `staleRecordsDeleted == 1` and runs nothing.

2. `formatOneRecordIsDeletedOnLoad()` (new): EA_IN 4, request expA, `waitForIdle`, evict s1; `writeBytes(path, asFormatOne(bytesOf(path)))`; premise `readCalculationRecord("s1", kExpA).status == UnsupportedVersion`; reset stats, `Quiet`, `WarningCapture`; load s1: the file is gone, `staleRecordsDeleted == 1`, `recordsRestored == 0`, `recordsSkipped == 0`, expA not Ok, `runCount(kExpA) == 0`, no warning containing "skipped", Quiet holds.

3. `unreadableRecordIsSkipped_data/()` (new). Columns `mechanism` (int, `UnreadableFile::Mechanism`) and `viaRestart` (bool). Rows: `directory` (Directory, false), `locked without sharing` (LockedWithoutSharing, false), `locked without sharing, restart` (true), `no read permission` (NoReadPermission, false), `no read permission, restart` (true). Body:
   - EA_IN 4; request expA (Ok); `waitForIdle`; path `recordPath("s1", "exp%41")`; `r0 = bytesOf(path)`; evict s1.
   - `UnreadableFile u(path, mechanism); if (!u.skipReason().isEmpty()) QSKIP(qPrintable(u.skipReason()));`
   - If `viaRestart`: `restart()`; `QVERIFY(knownCalculationRecords("s1").contains(kExpA))`.
   - Reset stats; `Quiet`; `WarningCapture`; load s1 (`session("s1")`).
   - Assert: `resultStatus(kExpA)` not Ok; `EA1` not valid; `runCount(kExpA) == 0`; `preparedCount() == 0`; `recordsSkipped == 1`; `staleRecordsDeleted == 0`; `recordsRestored == 0`; `recordsWritten == 0`; `warnings.count("skipped (kept for the next load)") == 1`; `QFileInfo::exists(path)`; `unconfirmedCalculationRecords("s1") == {kExpA}`; `knownCalculationRecords("s1")` contains kExpA; Quiet holds.
   - `QVERIFY(u.release()); QCOMPARE(bytesOf(path), r0);` evict s1; `unconfirmedCalculationRecords("s1")` empty; reset stats; load s1: expA Ok, `EA1 == 5`, `runCount == 0`, `recordsRestored == 1`, `recordsSkipped == 0`, `bytesOf(path) == r0`.

4. `dependentOfSkippedRecordIsKept()` (new, Directory mechanism, every platform): UP_IN 2, DOWN_IN 5; request kUp then kDown; `waitForIdle`; evict s1; `upBytes`, `downBytes`; `UnreadableFile u(upPath, Directory)` (QSKIP on a skip reason); reset stats; `WarningCapture`; load s1: kUp and kDown not Ok, `recordsSkipped == 2`, `staleRecordsDeleted == 0`, `recordsRestored == 0`, `bytesOf(downPath) == downBytes`, `warnings.count("skipped (kept for the next load)") == 2` and one of them contains "it reads a stored result that could not be read", `unconfirmedCalculationRecords("s1") == {kUp, kDown}`. Release; evict; reset stats; load: both Ok, `DOWN_OUT == 11`, both `runCount == 0`, `recordsRestored == 2`.

5. `registryChangeDeletesRecord_data/()` (new; spec 5 at store level, Phase 1 hand-off). Column `change` (QString). Local helpers: `kShadow` constant calculation on `EA_IN` (`constantCalculation(kShadow, "EA_IN", 0)`), family `test.store.famEaIn` (OnDemand; `instantiate` accepts only `attr EA_IN`, instance key "k", no inputs, output `EA_IN` = 0). Rows and setup before the requests:
   | Row | Before the requests | The change (loaded, fitted) | Records deleted |
   |---|---|---|---|
   | providerRegistered | - | `m_extra->add(shadow)` | 2 |
   | providerUnregistered | `m_extra->add(shadow)` (stored EA_IN still wins: EA1 == 5) | `m_extra->remove(kShadow)` | 2 |
   | familyRegistered | - | `m_extra->addFamily(famEaIn)` | 2 |
   | teardownRemoval | `m_extra->add(shadow)` | `m_extra->remove(kShadow, Removal::Teardown)` | 0 |
   Body: EA_IN 4, EB_IN 10; request expA and expB (Ok); `waitForIdle`; paths A and B, their bytes; reset stats; `Quiet`; apply the change; with no event-loop pass: for 2, both files gone, `droppedRecordsDeleted == 2`, expA and expB not Ok; for 0, both byte-identical, `droppedRecordsDeleted == 0`, expA not Ok in memory (dropped silently). Quiet holds. Then evict s1, reset stats, load: for 2, `recordListings == 0`, `recordsRead == 0`, expA not Ok; for 0, `recordsRestored == 2`, expA Ok, `runCount(kExpA) == 0`.

6. `recordSurvivesUnrelatedChanges_data/()` (new; the old environment row, now surviving). Column `change`: `unrelatedRegistration` (`m_extra->add(constantCalculation(kExtra, "_STORE_EXTRA", 1))`), `declaredPreference` (`PreferencesManager::setValue(PreferenceKeys::ImportDescentPauseSeconds, 45.0)`). Body: EA_IN 4; request expA; `waitForIdle`; `r0`; `env0 = calculationEnvironmentFingerprint()`; premise: the record's leaves do not contain `GraphNode::preference(ImportDescentPauseSeconds)`; evict s1; apply the change; `m_model->flushPendingInvalidations()`; `QVERIFY(calculationEnvironmentFingerprint() != env0)`; reset stats; `Quiet`; load: expA Ok, `runCount == 0`, `recordsRestored == 1`, `staleRecordsDeleted == 0`, bytes `r0`; `restart()` (the change still in effect); reset stats; load: the same four assertions; Quiet holds.

7. `lookupResolvingDifferentlyDeletesRecord()` (new; spec 10 third bullet at store level). Descriptors: `provider(id)` OnDemand, input attr `PROV_IN`, output `PROV_OUT = PROV_IN + 1` (int); `reader` `test.store.reader` Explicit, input attr `PROV_OUT`, output `READ_OUT = PROV_OUT * 10` (int). `m_extra->add(provider("test.store.provider1"))`, `m_extra->add(reader)`; PROV_IN 2; request reader: Ok, `READ_OUT == 30`; the record's resolutions contain `{attr PROV_OUT, Calculation, "test.store.provider1", ""}` and `{attr PROV_IN, SessionData}`; `waitForIdle`; evict; `r0`.
   - Control, a losing candidate: `m_extra->add(provider("test.store.provider2"))` (registered after provider1, never tried first); load: restored (`recordsRestored == 1`, `runCount(reader) == 0`, `READ_OUT == 30`); evict.
   - A winning candidate with the same inputs: `m_extra->add(provider("test.store.provider0"))`, `m_extra->remove("test.store.provider1")`, `m_extra->add(provider("test.store.provider1"))` (provider0 is now tried before provider1 and provider2); reset stats; `Quiet`; load: the record is gone, `staleRecordsDeleted == 1`, reader not Ok, `runCount(reader) == 0`, Quiet holds. Which check: `decodeCalculationRecord(r0, &record)` Ok, `engine("s1").restoreResult(record.result)` is `Stale` with `StaleCheck::Resolutions`.

8. `pluginEditStalesRecordsThatReadIt_data/()` (new; spec 10 fourth bullet, core). Ingredients `v1`: files `{"a_plugin.py": "x = 1\n", "helper.py": "y = 2\n"}`, SDK `"sdk"`, Python `"3.13.3"`, numpy `"2.2.4"`. Column `ingredients` (a row name mapped in the body): `unchanged` (v1), `editedFile` (`a_plugin.py` = `"x = 2\n"`), `addedFile` (+ `"b_plugin.py": "z = 3\n"`), `pythonVersion` (`"3.13.4"`), `numpyVersion` (`"2.2.5"`); column `stale` (bool: false only for `unchanged`). Descriptors: stand-in plug-in `test.store.plugin` OnDemand, input attr `PL_IN`, output `PL_OUT = PL_IN + 1`, `resultVersion = pluginCodeIdentity(ingredients)`; `test.store.readsPlugin` Explicit, input attr `PL_OUT`, output `RP_OUT = PL_OUT * 2`; `test.store.readsNoPlugin` Explicit, input attr `NP_IN`, output `RN_OUT = NP_IN * 2`. Body: register the three (plug-in with v1); PL_IN 1, NP_IN 3; request both readers (Ok; `RP_OUT == 4`, `RN_OUT == 6`); premise: readsPlugin's record has `{attr PL_OUT, Calculation, "test.store.plugin", pluginCodeIdentity(v1)}`, readsNoPlugin's has no Calculation resolution; `waitForIdle`; `restart([&] { remove the plug-in; add it again with pluginCodeIdentity(row ingredients); })`; reset stats; `Quiet`; load s1: readsNoPlugin restored in every row (`RN_OUT == 6`, `runCount == 0`); readsPlugin restored (`RP_OUT == 4`) when not `stale`, else its record is gone, it is not Ok, `staleRecordsDeleted == 1`; `runCount(readsPlugin) == 0` always; Quiet holds.

**Acceptance Criteria:**
- [ ] Every function above exists with that name and passes in `build-phase1` Release on Windows; the `locked without sharing` rows run on Windows and skip elsewhere; the `no read permission` rows skip on Windows (and as root).
- [ ] `staleRecordDeletedOnLoad` has no `environment` row; every other existing function keeps its name and passes.
- [ ] `cleanup()`'s registry and enrolled-engine checks hold after every function.

**Complexity:** L

---

### Task 3.8: `tests/tst_result_columns.cpp` and the acceptance map line

**Purpose:** Column values over a skipped record stay out of `index.json`; the environment tests follow the new rules.

**Files to modify:**
- `tests/tst_result_columns.cpp`
- `tests/acceptance_map.txt` - line `324 tst_result_columns environmentChangeUnconfirmsLoadedRows` becomes `324 tst_result_columns registryChangeKeepsLoadedRowConfirmed` (the only map edit of this phase; the audit requires every mapped function to exist).

**Technical Approach:**

Header comment: "... environment changes (which discard cached values but never make a record stale), and a record skipped at a load (its values never cached while skipped)".

1. `writeAfterStartupDropFlushesIndexFirst()` (keep rows and name): after `crash()` and before the edit to "f", `m_model->resetStoredResultStats()`; after `updateAttribute("s1", "_DESCRIPTION", "f")`: in `indexNotValid` the load restored X although the environment changed (`recordsRestored == 1`, `staleRecordsDeleted == 0`) and the edit then deleted it (`droppedRecordsDeleted == 1`); in `droppedAtStart` nothing was read (`recordsRead == 0`). The index bytes assertion that follows is unchanged. Comment: "... or the index was written in another environment (nothing is kept; the record itself stays valid and is restored at the load, then dropped by the edit)".

2. `environmentChangeDiscardsCachedValue()` (extend): after the existing assertions, `m_model->resetStoredResultStats(); session("s1"); waitForIdle`: `resultStatus(kCalcY) == Ok`, `runCount(kCalcY) == 0`, `recordsRestored == 1`, `staleRecordsDeleted == 0`, `cached("s1", kY) == 6.0`; `flushIndex()`: `indexValue("s1", yColumn()) == 6.0`, stamp `{kCalcY: "y-v1"}`.

3. `environmentChangeUnconfirmsLoadedRows` is renamed `registryChangeKeepsLoadedRowConfirmed` and rewritten:
   - Fit Y on s1 (index 6.0).
   - Unrelated A -> B -> A (register and unregister `kExtra` in one pass, then `flushPendingInvalidations()`): `unconfirmedCalculationRecords("s1")` empty; Y Ok with `runCount == 1`; flush: 6.0 and stamp `{Y: y-v1}`.
   - An environment change that stays (register `kExtra`; `flushPendingInvalidations()`; `waitForIdle`): still no unconfirmed id, Y Ok, `runCount == 1`, `cached("s1", kY) == 6.0` (recomputed from the engine after the discard); flush: 6.0 and `{Y: y-v1}`.
   - A registration reaching Y (a constant calculation `test.columns.shadow` with output `Y_IN`): the record is gone at once, `droppedRecordsDeleted == 1`; after `waitForIdle` and a flush: `indexValue("s1", yColumn())` is null and the stamp is `{}`.
   - The guard resets the model first, then unregisters both extra ids (the pattern of `environmentChangeDiscardsCachedValue`).

4. `skippedRecordValuesStayOutOfIndex_data/()` (new). Rows and columns exactly as `tst_result_store::unreadableRecordIsSkipped`. Body:
   - `fit("s1", kCalcY)`; index 6.0 and `{Y: y-v1}`; evict s1; `cached("s1", kY) == 6.0`.
   - `UnreadableFile u(recordPath("s1", kEncodedY), mechanism)`; QSKIP on a reason.
   - If `viaRestart`: `restart()`; `cached("s1", kY) == 6.0` (the kept value agrees with the record on disk).
   - Reset stats; `session("s1")`: at once `!isCached("s1", kY)`, `unconfirmedCalculationRecords("s1") == {kCalcY}`, `recordsSkipped == 1`, `staleRecordsDeleted == 0`.
   - `waitForIdle`: `isCached("s1", kY)` and not valid; `cell("s1", kY)` empty; `flushIndex()`: `indexValue("s1", yColumn())` undefined, the stamp is an object without `kCalcY`, `indexValue("s1", descriptionColumn()) == "d1"`; s2's Y value null and stamp `{}`.
   - Evict s1 (`resetColumnWorkStats()` first); `waitForIdle`: no unconfirmed id, `pending("s1") == {kY}`, not cached, `sessionsLoaded == 0`; flush: Y still undefined.
   - `u.release()`; `session("s1")`; `waitForIdle`: Y Ok, `runCount == 0`, `cached == 6.0`; flush: 6.0 and `{Y: y-v1}`.

**Acceptance Criteria:**
- [ ] The four functions pass; `environmentChangeUnconfirmsLoadedRows` no longer exists and the map names its replacement; the audit passes.
- [ ] Every other function of the file passes unchanged.

**Complexity:** M

---

### Task 3.9: `tests/tst_fusion_store.cpp`

**Purpose:** Spec section 10 end to end with real fits.

**Files to modify:**
- `tests/tst_fusion_store.cpp`

**Technical Approach:**

Infrastructure: include `altitudemarkerfeature.h`, `calculations/builtincalculations.h`, `plugincodeidentity.h`; members `std::unique_ptr<ExtraRegistrations> m_extra` (created in `init()` before the model) and `std::unique_ptr<AltitudeMarkerManager> m_altitudes`; `cleanup()` resets, after `m_model.reset()` and before the registry comparison, `m_altitudes` then `writeAltitudes({})` then `m_extra`. Header comment: "validity follows the inputs, what the fit looked up and the code stamps; nothing unrelated to what it reached (altitude markers, other registrations, the descent-pause preference, another plug-in set) makes it stale, in memory or across a restart; a registration that provides a name it looked up drops it and its record at once; a lookup that resolves differently at load deletes it". Remove the `kExtra` guard from `codeStampChangeDropsRecordOnLoad`.

1. `codeStampChangeDropsRecordOnLoad_data/()` (keep the name): row `environment` becomes `providerResultVersion`: `rewriteRecord("a", ...)` finds the resolution named `DependencyKey::measurement("IMU", "az")` (QVERIFY found, provider Calculation) and sets its `resultVersion = "v-old"`. The comment: "Bumping the compatibility marker, the fit's result version, or the result version recorded for a calculation its lookups went through drops the record on load." The body's assertions are unchanged.

2. `storedFitSurvivesUnrelatedChanges_data/()` (new; spec 10 first bullet). Column `change` (QString). Fixture `coarse_linear`, session "a". Common flow per row:
   - `addSessions`; `show({"a"})`; `check("roll")`; `m_queue->request("a", kFit)` Created; `waitIdle(kFitTimeoutMs)`; `waitForIdle`; `r0 = bytesOf(recordPath("a"))`; `fresh = capture("a")`.
   - A "runtime step" is: `env0 = calculationEnvironmentFingerprint()`; reset stats; `Quiet`; apply; `m_model->flushPendingInvalidations()`; `QVERIFY(calculationEnvironmentFingerprint() != env0)`; the fit stays installed: `resultStatus(kFit) == Ok`, `runCount(kFit)` unchanged, `bytesOf(path) == r0`, `droppedRecordsDeleted == 0`, `differenceFrom(fresh, "a")` empty, Quiet holds.
   - A "restart check" is: `restart(whileClosed)`; `check("roll")`; `Quiet`; `show({"a"})`; `resultStatus(kFit) == Ok`, `runCount == 0`, `recordsRestored == 1`, `staleRecordsDeleted == 0`, `bytesOf(path) == r0`, `differenceFrom(fresh, "a")` empty, `m_queue->model()->rowCount() == 0`, Quiet holds.
   | Row | Steps |
   |---|---|
   | altitudeMarker | `m_altitudes = make_unique<AltitudeMarkerManager>()`; runtime step `writeAltitudes({1000})` (registers `builtin.altitude._ALTITUDE_1000_FT`); `unloadAndReload("a")` restores (`recordsRestored == 1`, `runCount == 0`); restart check (the manager lives on: the next run registers the same marker); runtime step `writeAltitudes({})`; restart check |
   | unrelatedCalculation | runtime step `m_extra->add(constant kExtra "_STORE_EXTRA")`; restart check; runtime step `m_extra->remove(kExtra)`; restart check |
   | descentPause | premise: the record's leaves do not contain `GraphNode::preference(ImportDescentPauseSeconds)`; runtime step `setValue(ImportDescentPauseSeconds, 45.0)`; restart check; runtime step `setValue(..., 30.0)`; restart check |
   | unrelatedPluginSet | stand-ins: set A = `test.plugin.a` (OnDemand, no inputs, output attr `_TEST_PLUGIN_A` = 1.0) with `resultVersion = pluginCodeIdentity(A)`, A = files `{"a.py": "a = 1\n"}`, SDK "sdk", "3.13.3", "2.2.4"; set B = `test.plugin.b` (output attr `_TEST_PLUGIN_B` = 2.0) with B = files `{"b.py": "b = 1\n"}`, same SDK and versions. Restart check with `whileClosed` adding A; restart check with `whileClosed` removing A and adding B; restart check with `whileClosed` removing B. Before each restart note `env0`; after it `calculationEnvironmentFingerprint() != env0` |

3. `runtimeRegistrationDropsFitAndRecord()` (new; spec 10 second bullet). `coarse_linear` fitted and shown as above; premise: the record's resolutions contain an entry named `attr _LOCAL_ORIGIN_INDEX`. Reset stats; `Quiet`; `m_extra->add(` an OnDemand `test.store.originShadow`, no inputs, output attr `_LOCAL_ORIGIN_INDEX` = `QVariant::fromValue(qlonglong(0))` `)`. With no event-loop pass: the record file is gone, `droppedRecordsDeleted == 1`, `isNotRequested(resultStatus(kFit))`, `availableIn("a")` empty. `row(kRoll)`: `missingCount == 1`, `control() == Control::Refresh`; `PlotFixture::spin`; Quiet holds. Then `waitForIdle`, reset stats, `unloadAndReload("a")` (the record is absent before and after, which it accepts): `recordsRead == 0`, not requested, `runCount == 0`. The guard order is the fixture's: `cleanup()` destroys the model before `m_extra` unregisters the shadow.

4. `lookupResolvingDifferentlyAtLoadDeletesFit()` (new; spec 10 third bullet). Descriptor `sAccFrom(id)`: OnDemand, input measurement `GNSS/testSAcc`, output measurement `GNSS/sAcc` = the input's samples, unit "m/s". `m_extra->add(sAccFrom("test.store.sacc1"))`; `addSessions({fixtureSessionWithSAccStoredAs("coarse_linear", "a", "testSAcc")})`; request the fit (Created, Succeeded, `resultStatus == Ok`); `waitForIdle`; premise: the record's resolutions contain `{meas GNSS/sAcc, Calculation, "test.store.sacc1", ""}`; `r0`. `restart([&] { m_extra->add(sAccFrom("test.store.sacc0")); m_extra->remove("test.store.sacc1"); m_extra->add(sAccFrom("test.store.sacc1")); })` (sacc0, same inputs, is now tried first); `check("roll")`; `Quiet`; `show({"a"})`: the record is gone, `staleRecordsDeleted == 1`, `isNotRequested(resultStatus(kFit))`, `runCount == 0`, `row(kRoll).control() == Control::Refresh`, Quiet holds. Which check: decode `r0`, `engine("a").restoreResult(record.result)` is `Stale` / `StaleCheck::Resolutions`.

**Acceptance Criteria:**
- [ ] The three new functions and the changed row pass in `build-phase1` Release (label fusion; exact mode is not required).
- [ ] `tst_fusion_store` as a whole stays within its 600 s timeout; report its duration in the phase report.
- [ ] `cleanup()`'s registry, pin and enrolled-engine checks hold after every function; `writeAltitudes({})` leaves no altitude registration.

**Complexity:** L

## Testing Requirements

### Unit Tests
- `tst_result_records`: modified `stampsAreCurrent`, `layoutIsPinned`, `futureVersionIsRefused` rows, `readStatuses`, `corruptInputIsRefused` (craft header), helpers; new `formatOneIsRefused`.
- `tst_result_store`: modified `staleRecordDeletedOnLoad` rows; new `formatOneRecordIsDeletedOnLoad`, `unreadableRecordIsSkipped`, `dependentOfSkippedRecordIsKept`, `registryChangeDeletesRecord`, `recordSurvivesUnrelatedChanges`, `lookupResolvingDifferentlyDeletesRecord`, `pluginEditStalesRecordsThatReadIt`.
- `tst_result_columns`: modified `writeAfterStartupDropFlushesIndexFirst`, `environmentChangeDiscardsCachedValue`; `environmentChangeUnconfirmsLoadedRows` renamed and rewritten as `registryChangeKeepsLoadedRowConfirmed`; new `skippedRecordValuesStayOutOfIndex`.

### Integration Tests
- `tst_fusion_store`: modified `codeStampChangeDropsRecordOnLoad` row; new `storedFitSurvivesUnrelatedChanges`, `runtimeRegistrationDropsFitAndRecord`, `lookupResolvingDifferentlyAtLoadDeletesFit`.
- Full suite in `build-phase1`: `cmake --build build-phase1 --config Release`, then `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure`; labels core, fusion, exact, python, audit all green. Spec 10's plug-in identity items are Phase 2's (`tst_plugin_identity`, `tst_column_cache::pluginEditDiscardsCachedValues`) and must still pass.

### Manual Verification
- On a scratch logbook with `build-phase1`'s application: fit a session; add an altitude marker and change the descent-pause preference; restart: the fit is restored with no job. Quit, open the session's `cache/*.fvresult` in another program that locks it (or, on Windows, hold it open with PowerShell `[IO.File]::Open(path,'Open','Read','None')`), start and show the session: the fit reads not requested, the log shows "skipped (kept for the next load)", the record file is still there; release it, restart, show: the fit is restored.

## Notes for Implementer

### Gotchas
- `evict()` in the store and column tests loads a stub first (`session(id)`), then evicts; do not call it between making a record unreadable and the load under test unless the row is already loaded.
- A skip is decided at the read, before any restore; it emits `calculationRecordsChanged` from inside `restoreStoredResults()` exactly as a stale delete already does, so the model's handler must stay "drop state and defer work".
- The union of listed and known ids changes nothing for a known id with no file (Missing, passed over) and adds no listing: keep `recordListings` counting one per call that got past the known-set check.
- `ExtraRegistrations` must be destroyed after the model (a live model reacts to registry changes, and a runtime removal that reaches a requested result now deletes its record).
- `UnreadableFile::Directory` removes and recreates the file itself, behind the manager's back; the known set keeps the id, which is what lets the store reach it. After `restart()` the directory would not be known: use the lock or permission rows for restarts.
- On Windows `<windows.h>` defines `min`/`max` and many names; include it only in `logbookprobe.cpp` under `Q_OS_WIN` with `WIN32_LEAN_AND_MEAN` and `NOMINMAX`.
- In the fusion tests, a registry change with the model alive also triggers the environment check (cached column values discarded, worker restarted): harmless, but call `flushPendingInvalidations()` before asserting.
- `staleRecordDeletedOnLoad`'s `resolutions` row relies on expA's single resolution; if the premise changes, find the entry by name instead of `first()`.
- The altitude manager's default unit is Imperial: `writeAltitudes({1000})` registers `builtin.altitude._ALTITUDE_1000_FT`.
- A teardown removal with a live model leaves an engine and a record disagreeing; that is only for shutdown, where the model is gone first. Tests other than `registryChangeDeletesRecord`'s teardown row must not use it.

### Decisions Made
- **D1 - Format 2 is Phase 1's layout minus the environment string.** Sections renumbered; nothing else moves, so every crafted corrupt payload keeps its body. Records of format 1 (both the ce2fb2b layout and Phase 1's) are `UnsupportedVersion` and deleted at load; no migration (spec 8).
- **D2 - `stampsAreCurrent()` keeps its name** (acceptance map 316) and compares the compatibility marker only; the registry overloads go, because no stamp depends on the registry any more.
- **D3 - Skipped records reuse the unconfirmed set** through a new per-pair `markCalculationRecordSkipped()`; the whole-session `markCalculationRecordsUnconfirmed()` is removed with its only caller. The values are dropped when the pair is marked (loaded row recomputes from the engine: unavailable, never flushed), and eviction discards the mark so the stub goes pending; the next load settles it.
- **D4 - A record that is unrestorable only because it reads a skipped record is skipped too.** Deleting it would destroy a good record because of another file's transient lock, against spec 12's third principle. Identified by its stored resolutions naming a skipped calculation.
- **D5 - The store reads the union of listed and known ids.** This is what makes spec 10's "a directory at its path" example reachable: the listing takes files only, so a directory is seen only as a known id. After a restart a directory is not known and not listed (nothing is read, nothing is cached as if restored); the restart rows therefore use a Windows lock or POSIX permissions, each skipping where the platform does not honour it.
- **D6 - Unreadable warns** (`qWarning`, "skipped (kept for the next load)"): unlike a stale record it is not an expected state, and a user report should show it.
- **D7 - Store-level "lookup resolves differently"** uses test-owned providers and re-registration (`Re-registering after unregister() goes to the end`), so the global registry is restored exactly. The fusion-level test does the same with a session whose `GNSS/sAcc` has no source data (`fixtureSessionWithSAccStoredAs`), because no built-in name the fit reads can be given a winning new candidate without reordering built-ins.
- **D8 - The environment-staleness tests are rewritten, not deleted**: `staleRecordDeletedOnLoad` row `environment` -> `resolutions`; `codeStampChangeDropsRecordOnLoad` row `environment` -> `providerResultVersion`; `environmentChangeDiscardsCachedValue` extended to show the record restores; `writeAfterStartupDropFlushesIndexFirst` `indexNotValid` pins that the record survived the environment change; `environmentChangeUnconfirmsLoadedRows` renamed `registryChangeKeepsLoadedRowConfirmed` (its old claim is now false), with the one acceptance-map line updated so the audit stays green. The surviving scenarios are the new `recordSurvivesUnrelatedChanges` and `storedFitSurvivesUnrelatedChanges`. `noDeleteWithoutInputChange` part (b) was already rewritten by Phase 1.
- **D9 - "Read in full"** stays the reader's existing check (open, `readAll()` without a device error); a short read without an error is indistinguishable from a truncated file, which is Corrupt and deleted.

### Open Questions
- None blocking. Hand-off to Phase 4: `tests/README.md` (catalogue rows and §9.4 matrix row 324 for the renamed function; the new functions; §8: `UnreadableFile`, `asFormatOne`, `ExtraRegistrations`, `fixtureSessionWithSAccStoredAs`; the permission-bit and lock rows), acceptance entries for every new test, `docs/DATA_SCHEMA.md` §12 (format 2 layout, unreadable and dependent-skipped records, the `recordsSkipped` behaviour) and §11 (values over skipped records never cached).

## Definition of Done

This phase is complete when:
1. All tasks have passing acceptance criteria.
2. The full suite passes in `build-phase1` (Release): labels core, fusion, exact, python, audit.
3. Code follows the patterns of the reference files (literal expectations, `QScopeGuard`/RAII registrations destroyed after the model, data-driven rows that `QSKIP` where a platform mechanism is not honoured).
4. No TODOs or placeholder code remain, and no comment in `src/` describes the environment fingerprint as part of a record or a registry change as keeping records.
