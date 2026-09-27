# Phase 6: Import and merge rules

## Overview

This phase implements spec section 6.2-6.5: a file whose `SESSION_ID` is not in
the logbook creates a session, any other file merges into the existing session
whether or not it is loaded, and a merge is a validated, all-or-nothing
operation on **source data and stored attributes only**. Parsing is separated
from session creation (the parse result carries nothing but what the file
says; Viewer's import-time defaults are applied only when a session is
created), `SessionModel::mergeSessions` is rewritten around a
plan-then-apply algorithm with a per-file result, a merge that changes
something flows through the model's normal invalidation / column / dirty /
save path, a merge that changes nothing is a no-op, and the text of every
import error reaches the existing failure dialog. No new UI is added.

## Dependencies

- **Depends on:** Phase 5 (and 1-4).
- **Blocks:** Phase 8. Phase 7 runs in parallel and owns `src/pluginhost.*`,
  `src/*_bindings.cpp`, `src/pluginsessionview.h`, `src/cpp_bridge.cpp`,
  `python_plugins/` - **do not touch those files**.
- **Assumptions (names used verbatim from the earlier phase documents):**
  - Branch `schema-and-calculations`. Line numbers for `src/dataimporter.*`,
    `src/mainwindow.cpp`, `src/logbookmanager.cpp`, `src/sessiondata.h` are
    **baseline** (`git show v2026.04.1:<path>`); `src/sessionmodel.*` is
    identical at the tag and on `master`, its numbers are as at the tag. The
    branch has drifted through Phases 1-5: locate code by the quoted
    identifiers.
  - `SessionData` (Phase 4 Task 4.3): `SourceColumn`, `SourceSensor`,
    `SourceData`, `sourceData()`, `mergeSourceData`, `setSourceMeasurement`,
    `sourceMeasurement`, `sourceUnit`, `storedAttribute`, `hasStoredAttribute`,
    `attributeKeys`, `setAttribute` (returns the invalidated
    `QSet<DependencyKey>`). Source accessors never create the engine. Copy =
    state only, move carries the engine (Phase 3 Task 3.9).
  - `DataImporter` (Phase 4 Task 4.5): `readFile` parses into `StagedFile`,
    validates (`Schema::parseVersion`, structural errors) **before** publishing,
    publishes with `setAttribute` + `mergeSourceData`, clears `m_lastError` at
    entry, leaves the target untouched on failure, stores every `$VAR` value as
    a verbatim `QString`. `importFile` = `readFile` + `initializeFromDevice`
    (baseline 85-137), unchanged so far.
  - `CsvFormat::formatAttributeValue`, `CsvFormat::singleLine` (Phase 5 Task 5.1).
  - `LogbookManager` (Phase 5 Task 5.6): `saveSession` ordering,
    `markColumnsUnsaved`, `markSessionUnsaved`, `lastSaveError`, orphan
    adoption (identity entries `m_sessionIdToUuid[stem] = stem`),
    `remapSessionId`, `reset()`.
  - `SessionModel` (Phase 3 Task 3.10, Phase 5 Task 5.7): `attachSession`,
    `publishInvalidation`, `flushPendingInvalidations`, `invalidateColumns`,
    `invalidateAllColumns`, `fillMissingColumns`, `scheduleSave`,
    `startColumnWorker`, `columnWorkStats`. The Phase 5 rule "every site that
    mutates a row's persistent state calls `invalidateColumns` /
    `invalidateAllColumns` before returning to the event loop" binds this phase.
  - Baseline `mainwindow.cpp` 586 (the forced `_GROUND_ELEV` read) was already
    deleted by Phase 3 Task 3.9. Finding confirmed here: it only warmed the old
    per-value cache of the temporary session; it never wrote a stored
    attribute, so nothing was ever persisted by it and nothing replaces it.
  - Test support: `TestEnvironment` (`useFreshLogbook`, `reopenLogbook`,
    `sessionsDir`, `indexPath`, `newTempDir`, `registerBuiltIns`,
    `resetPreferencesToDefaults`), `waitForIdle`, `Fs2FileBuilder`,
    `Fs1FileBuilder`, `Fixtures::sensorFile()` / `trackFile()`,
    `DescentFixture`, `readFileBytes`, `writeFile`, `FLYSIGHT_TEST_MAIN`,
    `flysight_add_test`.

## Findings that drive the design (read before the tasks)

1. **Baseline `mergeSessions` (501-583)** has three branches. Loaded match:
   every incoming attribute **overwrites** the session's (so `_DESCRIPTION`,
   `_IMPORT_TIME`, `_JUMPER_MASS`, a fixed `_GROUND_ELEV` from the second file
   clobber edits), measurements are replaced (Phase 4 made this
   `mergeSourceData`), always marked dirty. Stub match (546-553): the stub is
   **replaced wholesale** by the incoming file - the saved session's other
   sensors, edits, and markers are lost at the next save. No match: new row.
   There is no failure path and no return value.
2. **Empty-session fallback (`sessionRef` 828-831).** A failed load installs
   `SessionData()` in the row. On its own this cannot overwrite the file
   (`saveSession` refuses an empty `SESSION_ID`), but the row now counts as
   *loaded*, so the next import of a file with that `SESSION_ID` takes the
   loaded-match branch, merges into the empty session (giving it a
   `SESSION_ID`), marks it dirty, and the saver writes it **over the real
   file** through the existing uuid mapping. Fixed in Task 6.4.
3. **Identity stubs.** After a deferred scan (`scanSessionFilenames`, baseline
   471-486) or Phase 5's orphan adoption, a row's `sessionId` is the file stem
   (a uuid), not the real `SESSION_ID`; it is remapped only when the file is
   parsed (`sessionRef` 821-826, `processNextDirtyColumn` 1257-1261). An import
   arriving before that finds no matching row, creates a duplicate session
   under a new uuid, and the later `remapSessionId` fails because the id is
   taken. Fixed in Task 6.3 / 6.4 by resolving identity stubs before matching.
4. **What makes TRACK-then-SENSOR differ from SENSOR-then-TRACK today:** the
   attribute overwrite (finding 1): `_DESCRIPTION` (file name when the folder
   is not a `HH-MM-SS` device folder, baseline `getDescription` 449-483),
   `_IMPORT_TIME` (clock), mass / area / fixed elevation (preferences at the
   time of the second import), `DEVICE_ID` fallback. Under this phase a merge
   never applies or overwrites any of them, so the only order-dependent values
   left are the ones derived from the **creating** file's path and the clock.
   See "Order-independence comparison rule" under Decisions.
5. **Attribute value types.** Every header attribute published by the importer
   is a `QString` (Phase 4); every attribute of a reloaded session is a
   `QString` (Phase 5 finding 5). In-memory `_` attributes of a freshly created
   or edited session may be `double` / whatever `QSettings` returned. Equality
   for the conflict rule is therefore defined on the **on-disk text form**
   (Task 6.4), which makes `"2"` equal `"2"` and makes a fresh value equal its
   own reloaded form by construction.
6. **Consumers of the backfilled attributes.** `_WIND_N` / `_WIND_E` are
   required inputs of `builtin.gnss.wcVel`, `wcVelH`, `accAlongTrack`,
   `accCrossTrack`, `lift`, `drag`; `_JUMPER_MASS` / `_PLANFORM_AREA` of
   `lift`, `drag` (Phase 3 inventory section 2). All inputs are required, so a
   session lacking them loses those six measurements entirely. Sessions created
   by any import since those attributes were introduced already store all four.
7. **Import entry points** (grep `importFiles\(|mergeSessions|DataImporter|importFile\(` over `src`):
   `MainWindow::on_action_Import_triggered` (442-465),
   `on_action_ImportFolder_triggered` (467-520), and drag-and-drop
   `handleDroppedUrls` (342-381, the call at 368) - all three go through
   `MainWindow::importFiles` (564-681). There is no device-scan or command-line
   import path. The only other `mergeSessions` caller is startup (181-186, the
   legacy flat-index branch: parses every CSV, merges them as dirty rows, and
   thereby rewrites every session file). `DataImporter::readFile` is also used
   by `LogbookManager::loadSession` (362-368) and `scanSessionFiles` (452-461).

---

## Tasks

### Task 6.1: Separate the parse result from session creation in `DataImporter`

**Purpose:** Spec 6.1 / 6.2: a parsed file is source data plus recorded header attributes and nothing else; import-time defaults exist only as an explicit "session creation" step that the model invokes.

**Files to create:**
- `src/parsedfile.h` (in `flysight_core`; header-only)

**Files to modify:**
- `src/dataimporter.h` / `src/dataimporter.cpp`
- `src/sessiondata.h` - add `constexpr char DeviceIdUnknown[] = "n/a";` to `SessionKeys` (the literal currently at baseline `dataimporter.cpp` 107).
- `src/CMakeLists.txt`
- `tests/tst_importer.cpp`

**Technical Approach:**

```cpp
// parsedfile.h
namespace FlySight {
struct ParsedFile {
    QString     filePath;                 // empty for sessions built in memory
    SessionData data;                     // recorded header attributes + source data, exactly as parsed
    QString     sessionId;                // match id: recorded SESSION_ID, else MD5 (lower-case hex) of the file bytes
    bool        sessionIdRecorded = false;
    bool        applyCreationDefaults = true;   // false: adopt `data` as the new session unchanged

    // Wraps an in-memory session (tests, tools). sessionId = stored SESSION_ID ("" if none),
    // sessionIdRecorded = !sessionId.isEmpty(), applyCreationDefaults = false.
    static ParsedFile fromSession(const SessionData &session);
};
}
```

`DataImporter` public surface after this task:

```cpp
bool parseFile(const QString &fileName, ParsedFile &out);      // NEW - what the application imports with
static void applyCreationDefaults(const ParsedFile &file, SessionData &session);   // NEW - replaces initializeFromDevice
bool importFile(const QString &fileName, SessionData &sessionData);   // kept: parseFile + applyCreationDefaults ("import as a brand-new session")
bool readFile(const QString &fileName, SessionData &sessionData, QByteArray *fileData = nullptr);   // unchanged (logbook loads)
QString getLastError() const;
```

- `parseFile`: `readFile(fileName, out.data, &bytes)`; on success
  `out.filePath = fileName`; if `out.data.hasStoredAttribute(SessionKeys::SessionId)`
  and its text is non-empty -> `sessionId` = that text, `sessionIdRecorded = true`;
  otherwise `sessionId = QCryptographicHash::hash(bytes, Md5).toHex()` (the
  computation of baseline 111-116) and `sessionIdRecorded = false`. **Nothing
  is written into `out.data`** - not the hash, not `DEVICE_ID`, not any `_`
  key. On failure `out` is left default-constructed-equivalent (`data` empty)
  and `getLastError()` is set by `readFile`.
- `applyCreationDefaults(file, session)` - `session` is the new session, already
  holding a copy of `file.data`'s state. It is the **only** place import-time
  defaults are written (move the body of `initializeFromDevice`, 85-137, here
  and delete `initializeFromDevice`). In this order, and **only when the key is
  not already stored** (`hasStoredAttribute`) - a Viewer-saved file that is
  imported as a new session keeps its own `_` values:
  1. `SESSION_ID` = `file.sessionId` when `!file.sessionIdRecorded`.
  2. `DEVICE_ID`: `extractDeviceId` (368-422), else
     `SessionKeys::DeviceIdUnknown`. Baseline chose the `FLYSIGHT.TXT` key from
     the file's first line (`Device_ID` for FS2, `Processor serial number` for
     FS1; 89-103); the file bytes are no longer available here, so look for
     `Device_ID` first and `Processor serial number` second - the two keys
     never coexist in one `FLYSIGHT.TXT`, so the result is the same. Skipped
     entirely (no lookup, no placeholder) when `file.filePath` is empty.
  3. `_DESCRIPTION` = `CsvFormat::singleLine(getDescription(file.filePath))`.
  4. `_IMPORT_TIME` = now (double seconds, as today).
  5. `_WIND_N` = `_WIND_E` = `0.0`.
  6. `_JUMPER_MASS` / `_PLANFORM_AREA` from `PreferenceKeys::AeroMass` / `AeroArea`.
  7. `_GROUND_ELEV` = `ImportFixedElevation` when `ImportGroundReferenceMode == "Fixed"`.
  These are all the import-time defaults that exist (audited: baseline 85-137
  is the only writer; `ImportHideOthersOnImport` affects visibility, not the
  session; `ImportDescentPauseSeconds` is a declared calculation input).
  `extractDeviceId` / `findFlySightRoot` / `getDescription` become `static`
  private helpers; the two `qWarning`s about a missing `FLYSIGHT.TXT`
  (374, 421) are downgraded to `qDebug` (spec 12: silence for the normal case).
- `importFile(fileName, session)`: `ParsedFile f; if (!parseFile(fileName, f)) return false;`
  then publish into `session` exactly as `readFile` would have (assign
  `session = std::move(f.data)` **only if `session` was empty**; otherwise
  `setAttribute` per key + `mergeSourceData`, preserving Phase 4's
  "pre-populated target" test), then `applyCreationDefaults(f, session)`.
  One code path, no duplicated default logic. Header comment: "Convenience for
  tests and tools: parse + create. The application never calls this; it imports
  through `SessionImport::importFiles`, where the model decides between
  creation and merge."

**Tests added to `tst_importer` (literals only):**

| Test | Scenario | Expected |
|---|---|---|
| `parseFileCarriesNoDefaults` | `parseFile(Fixtures::sensorFile())` | `data.attributeKeys()` == {`DEVICE_ID`, `FIRMWARE_VER`, `SESSION_ID`} exactly; `sessionId == "test-session"`; `sessionIdRecorded`; `applyCreationDefaults` true |
| `parseFileSynthesizesMatchId` | FS1 file; FS2 file without `SESSION_ID` | `sessionIdRecorded` false; `sessionId` is 32 lower-case hex chars and equals the literal MD5 of the fixture bytes (computed once by hand with `certutil -hashfile` / `md5sum` and pasted); `data.hasAttribute("SESSION_ID")` **false**; `data.hasAttribute("DEVICE_ID")` false; parsing the same bytes from a different path gives the same id |
| `creationDefaultsOnlyFillAbsent` | parsed sensor fixture + `.var("_DESCRIPTION","kept")`, `.var("_JUMPER_MASS","80")`; `applyCreationDefaults` | `_DESCRIPTION == "kept"`, `_JUMPER_MASS == "80"`; `_PLANFORM_AREA == 1.0`, `_WIND_N == 0.0`, `_IMPORT_TIME > 0` added; `DEVICE_ID` still `"test-device"` |
| `creationDefaultsDeviceId` | temp tree `<dir>/FLYSIGHT.TXT` containing `Device_ID: abc123`, file without `DEVICE_ID` in `<dir>/24-01-01/12-00-00/TRACK.CSV` | `DEVICE_ID == "abc123"`; `_DESCRIPTION == "24-01-01/12-00-00"`; without `FLYSIGHT.TXT` (file placed directly under `newTempDir()`, whose ancestors contain none) -> `"n/a"` and zero warnings |
| `fixedGroundElevationOnlyInFixedMode` | prefs `Fixed` / `123.5`, then `Automatic` | stored `_GROUND_ELEV == 123.5`; absent |
| `importFileStillComposes` | existing `tst_smoke::importAppliesCreationDefaults` unchanged | passes |

**Acceptance Criteria:**
- [ ] `git grep -n "initializeFromDevice" -- src tests` returns nothing; `git grep -n "\"n/a\"" -- src` hits only `sessiondata.h`.
- [ ] `parseFile` writes no attribute the file did not contain (test above); `git grep -n "PreferencesManager\|currentDateTime" -- src/dataimporter.cpp` hits only inside `applyCreationDefaults`.
- [ ] `git grep -n "importFile(" -- src` shows no caller outside `dataimporter.cpp` after Task 6.6.
- [ ] Every Phase 1-5 test passes unchanged.

**Complexity:** M

---

### Task 6.2: `LogbookManager` - raw load with error text, backfill as a named shim, identity-stub query

**Purpose:** Give the merge a load that reports *why* it failed and that applies no Viewer-generated values before merge decisions are made; give stub matching the facts it needs.

**Files to modify:**
- `src/logbookmanager.h` / `src/logbookmanager.cpp`
- `src/dataimporter.h` / `.cpp` - `static std::optional<QString> peekHeaderAttribute(const QString &fileName, const QString &key);`
- `tests/tst_logbook_index.cpp`

**Technical Approach:**

```cpp
std::optional<SessionData> loadSessionRaw(const QString &sessionId, QString *error = nullptr); // file contents only
static void applyLegacyBackfill(SessionData &session);                                          // baseline 370-386, verbatim
std::optional<SessionData> loadSession(const QString &sessionId);   // = loadSessionRaw + applyLegacyBackfill (unchanged behavior)
bool isIdentityEntry(const QString &sessionId) const;               // m_sessionIdToUuid.value(id) == id
std::optional<QString> peekSessionId(const QString &sessionId) const; // header-only read of the entry's CSV
```

- `loadSessionRaw` errors (set `*error`, return `nullopt`): unknown id ->
  `"not in the logbook index"`; `readFile` failure -> the importer's
  `getLastError()` (e.g. `"Couldn't read file"`, `"Missing $DATA section"`,
  `"Unsupported SCHEMA_VER '3' (supported: 1, 2)"`). Keep the existing
  `qWarning`s.
- `applyLegacyBackfill` header comment (this is the policy decision, see
  Decisions): "Compatibility shim for session files written by Viewer versions
  that predate `_JUMPER_MASS` / `_PLANFORM_AREA` / `_WIND_N` / `_WIND_E`. It is
  not an import default: it never runs on an incoming file, never runs before
  a merge has been decided, only fills keys that are absent, and does not mark
  the session dirty (pinned by `tst_persistence_roundtrip::releasedLogbookBackfillIsAdditive`)."
- `DataImporter::peekHeaderAttribute`: opens the file, reads lines until
  `$DATA` (or EOF), applies the `$VAR` grammar of Phase 4 Task 4.5 (value =
  remainder after the second comma), returns the first value for `key`. No
  data rows are read; no `SessionData` is built. `peekSessionId` maps id ->
  uuid -> path and calls it with `SessionKeys::SessionId`.

**Tests added to `tst_logbook_index`:** `rawLoadSkipsBackfill` (released-format
file without the four keys: `loadSessionRaw` result lacks them, `loadSession`
result has them); `rawLoadReportsReason` (file overwritten with `"garbage"` ->
`nullopt`, error `"Unknown file format"`; unknown id -> `"not in the logbook index"`);
`identityEntries` (after `orphanSessionFileIsAdopted`'s setup:
`isIdentityEntry(<stem>)` true, `isIdentityEntry("a")` false,
`peekSessionId(<stem>) == "c"`).

**Acceptance Criteria:**
- [ ] `loadSession`'s observable behavior is unchanged (all Phase 5 tests pass).
- [ ] `peekHeaderAttribute` on a 100 000-row file reads no data row (verify by a fixture whose data section is deliberately unparsable binary junk: the peek still succeeds).

**Complexity:** S

---

### Task 6.3: `MergePlan` - pure validation of one file against one session

**Purpose:** Implement spec 6.3 and 6.4 as a side-effect-free function, so "validate everything before mutating" is structural and unit-testable without a model or a logbook.

**Files to create:**
- `src/sessionmerge.h` / `src/sessionmerge.cpp` (in `flysight_core`)
- `tests/tst_session_merge.cpp` (class `SessionMergeTest`)

**Files to modify:** `src/CMakeLists.txt`, `tests/CMakeLists.txt`.

**Technical Approach:**

```cpp
namespace FlySight {
struct MergePlan {
    QVector<QPair<QString, QVariant>> attributesToSet;   // key order; values exactly as the incoming file holds them
    SourceData                        columnsToSet;      // only columns that differ from the session's
    QString                           error;             // non-empty = the merge must not happen
    bool ok() const      { return error.isEmpty(); }
    bool isEmpty() const { return attributesToSet.isEmpty() && columnsToSet.isEmpty(); }
    QSet<DependencyKey> changedKeys() const;              // attribute(k) + measurement(sensor, name) of the plan
};
namespace SessionMerge {
// Never mutates either argument, never reads effective values, never touches an engine.
MergePlan plan(const SessionData &existing, const SessionData &incoming);
// Applies an ok() plan. Cannot fail. Returns the union of the invalidation sets.
QSet<DependencyKey> apply(SessionData &existing, const MergePlan &plan);
bool sameAttributeValue(const QVariant &a, const QVariant &b);
bool sameColumn(const SourceColumn &a, const SourceColumn &b);
}}
```

**Attribute rules** - iterate `incoming.attributeKeys()`, values via `storedAttribute`:

| Incoming key | Session | Result |
|---|---|---|
| any | key absent | add to `attributesToSet` |
| header key (no leading `_`) | present, `sameAttributeValue` | nothing |
| header key | present, different | **conflict** (collected; see message) |
| `_` key | present (any value) | nothing - existing wins, never a conflict |
| `DEVICE_ID`, incoming value `== SessionKeys::DeviceIdUnknown` | any | nothing (a Viewer placeholder is not a recorded fact) |
| `DEVICE_ID`, session value `== DeviceIdUnknown`, incoming differs | - | add to `attributesToSet` (the placeholder counts as absent) |

Keys only in the session are untouched (absence is never a conflict, either
direction). `SESSION_ID` needs no special case: an incoming file only carries
it when recorded, and then it equals the session's by construction of the
match.

- `sameAttributeValue(a, b)`:
  `CsvFormat::formatAttributeValue(a) == CsvFormat::formatAttributeValue(b)` -
  equality of the text that is, or would be, on disk. Exact: no trimming, no
  case folding, no numeric interpretation (`"2"` != `"2.0"`, `"2"` != `" 2"`).
  This is what "preserved exactly" implies, and it guarantees that a session
  compares equal to its own saved-and-reloaded form whatever `QVariant` types
  it held in memory. Two unrepresentable values (`nullopt`) are equal to each
  other.
- **Conflict message.** All conflicts are collected (sorted by key) into one
  error: for one key
  `Attribute '<KEY>' conflicts with the existing session (session: '<a>', file: '<b>').`
  repeated per key, followed once by the hint. Hint when `SCHEMA_VER`
  (`Schema::AttributeKey`) is among them:
  `To change a session's schema version, delete the session and re-import its files.`
  otherwise: `To replace the session, delete it and re-import its files.`
  `<a>` / `<b>` are the `formatAttributeValue` texts.

**Measurement rules** (spec 6.4) - iterate `incoming.sourceData()`; for each
column, if the session has no such column or `!sameColumn(existingColumn, incomingColumn)`
put it in `columnsToSet` (samples **and** unit text; the `QVector` is shared,
nothing is copied). Columns and sensors absent from the incoming file are never
mentioned, hence kept. `sameColumn`: `unit ==` and equal size and
(`constData()` pointers equal, or `std::memcmp` of the sample bytes `== 0`) -
bitwise, so NaN samples and `-0.0` compare sanely and an identical re-import is
recognised.

**Ragged rule.** For every sensor appearing in `columnsToSet`, compute the
length every column of that sensor would have after the merge (incoming length
for columns in the plan, existing length otherwise). If they are not all equal:
error
`Sensor '<s>': the file has <n> rows but the session's column '<c>' has <m>. Delete the session and re-import its files.`
naming the first kept column whose length differs. Rationale under Decisions.
Sensors the plan does not touch are not examined.

Errors do not short-circuit each other's collection order: attribute conflicts
are reported first; the ragged check runs only when there is no conflict.

`apply`: one `setAttribute` per `attributesToSet` entry, then one
`mergeSourceData(columnsToSet)`; unite and return the sets.

**Tests (`tst_session_merge`; programmatic `SessionData`, no engine required; literals only):**

| Test | Expected |
|---|---|
| `absentAttributeIsAdded` / `equalAttributeIsNoop` / `differentAttributeConflicts` | plan contents as tabled; conflict error equals the literal `"Attribute 'FIRMWARE_VER' conflicts with the existing session (session: 'v2023.09.22', file: 'v2024.01.01'). To replace the session, delete it and re-import its files."` |
| `absenceNeverConflicts` (both directions) | session has `SCHEMA_VER` = `"2"`, file has none -> `ok`, attribute untouched; session has none, file `"2"` -> added |
| `explicitSchemaMismatch` | session `"2"`, file `"1"` -> error contains `'SCHEMA_VER'`, `session: '2'`, `file: '1'`, and `delete the session and re-import` |
| `multipleConflictsSorted` | `DEVICE_ID` and `FIRMWARE_VER` both differ -> both named, `DEVICE_ID` first, one hint |
| `typedVersusTextEquality` | session `QVariant(2)` vs file `"2"` equal; session `QVariant(1718900000.123)` vs `"1718900000.123"` equal; `"2"` vs `"2.0"` conflict |
| `viewerAttributesExistingWins` | session `_DESCRIPTION` = `"mine"`, file `_DESCRIPTION` = `"theirs"`, file `_EXIT_TIME` = `"5"` -> `ok`; plan adds only `_EXIT_TIME` |
| `deviceIdPlaceholder` | session `"n/a"` + file `"abc"` -> plan sets `DEVICE_ID`; session `"abc"` + file `"n/a"` -> empty plan; `"abc"` vs `"def"` -> conflict |
| `columnsReplaceAddKeep` | session `IMU/{time,wx,extra}` (2 rows), file `IMU/{time,wx,wy}` (2 rows, `wx` different, `time` identical) -> plan has `IMU/wx`, `IMU/wy` only; after `apply`: `extra` unchanged, `wx` = file's, unit text taken from the file (`"deg/s"` replaces `"rad/s"`) |
| `unitOnlyChangeIsAChange` | same samples, unit `g` vs `m/s^2` -> column in plan |
| `identicalIsEmpty` | plan of a session against a copy of itself: `ok() && isEmpty()`; holds with a NaN sample and a `-0.0` sample |
| `raggedIsRejected` | session `X/{time,v,keep}` 3 rows, file `X/{time,v}` 2 rows -> error == `"Sensor 'X': the file has 2 rows but the session's column 'keep' has 3. Delete the session and re-import its files."`; file with all three columns at 2 rows -> `ok` |
| `disjointSensorsUnaffected` | session `GNSS` 3 rows, file `IMU` 5 rows -> `ok` |
| `planIsPure` | `existing.sourceData()`, `attributeKeys()` and a warm engine's `totalRunCount()` / `cachedNodeCount()` unchanged by `plan()`; on a session without an engine none is created |
| `applyReturnsInvalidation` | warm engine, unmarked gyro read; plan adding `SCHEMA_VER` = `"2"` -> returned set contains `IMU/wx` and `IMU/wTotal`, not `IMU/ax` |

**Acceptance Criteria:**
- [ ] `git grep -nE "getAttribute|getMeasurement|effectiveUnit|calculationEngine" -- src/sessionmerge.cpp` returns nothing (spec 6.4: source only).
- [ ] `plan()` takes both arguments by `const &` and the purity test passes.
- [ ] `sessionmerge.*` includes no model, logbook, or preferences header.

**Complexity:** M

---

### Task 6.4: Rewrite `SessionModel::mergeSessions` - match, load-before-merge, transactional apply, effects

**Purpose:** Spec 6.2 and 6.5, acceptance 3 / 7 / 8, and the fixes for findings 1-3.

**Files to modify:**
- `src/sessionmodel.h` / `src/sessionmodel.cpp`

**Technical Approach:**

```cpp
struct MergeResult {                                   // sessionmodel.h, namespace FlySight
    enum class Outcome { Created, Merged, Unchanged, Failed };
    QString filePath;      // ParsedFile::filePath
    QString sessionId;     // match id ("" when the input had none)
    Outcome outcome = Outcome::Failed;
    QString error;         // non-empty iff Failed
    bool ok() const { return outcome != Outcome::Failed; }
};

QList<MergeResult> mergeSessions(const QList<ParsedFile> &files);        // one result per input, same order
QList<MergeResult> mergeSessions(const QList<SessionData> &sessions);    // = ParsedFile::fromSession for each (tests, tools)
void resolveIdentityStubs();                                             // public: also useful to tests
```

`SessionRow` gains `bool loadFailed = false;`.

Algorithm of `mergeSessions(files)`:

0. Empty list -> return `{}`. Call `resolveIdentityStubs()`.
1. For each file, in list order (a later file sees the effects of earlier ones -
   this is what makes TRACK + SENSOR of one new session work in one batch):
   - `file.sessionId.isEmpty()` -> `Failed`, `"File has no SESSION_ID"` (only
     reachable through the `SessionData` overload).
   - `row = getSessionRow(file.sessionId)`.
   - **No row -> create.** `SessionData created = file.data;` if
     `file.applyCreationDefaults` -> `DataImporter::applyCreationDefaults(file, created)`;
     else require a stored `SESSION_ID` (guaranteed by the check above). Append
     a `SessionRow` (`visible = created.isVisible()`, as today 556-566),
     `attachSession` on the element **in `m_rows`**, remember the row for
     `invalidateAllColumns`. Outcome `Created`.
   - **Row loaded and `!loadFailed` -> merge in place.**
     `plan = SessionMerge::plan(*row.session, file.data)`. `!plan.ok()` ->
     `Failed` with `plan.error`, nothing touched. `plan.isEmpty()` ->
     `Unchanged`, nothing touched. Otherwise
     `keys = SessionMerge::apply(*row.session, plan) | plan.changedKeys()`;
     remember `(sessionId, keys)`; outcome `Merged`.
   - **Row not loaded, or `loadFailed` -> load, then merge on the copy.**
     `loaded = LogbookManager::instance().loadSessionRaw(row.sessionId, &reason)`.
     - `nullopt` -> `Failed`:
       `Existing session '<id>' could not be loaded (<reason>); the file was not imported.`
       The row stays exactly as it was (stub, or failed placeholder).
     - loaded but `loaded->storedAttribute(SESSION_ID).toString() != row.sessionId`
       -> `Failed`: same message with reason `"the logbook file belongs to session '<other>'"`.
     - `plan` on `*loaded`. Not ok -> `Failed`; the copy is discarded, the row
       stays a stub, its `cachedValues`, `dirty`, index entries, and file are
       untouched (nothing was written anywhere). Empty -> `Unchanged`, copy
       discarded, row stays a stub. Otherwise `apply` on the copy, then
       `LogbookManager::applyLegacyBackfill(*loaded)` (**after** the merge
       decision, never before), `row.session = std::move(*loaded)`,
       `row.loadFailed = false`, `row.session->setVisible(row.visible)`,
       `attachSession(row)`, LRU bookkeeping as `sessionRef` 839-843 (`lruTouch`
       when not visible and not focused), remember the id for `sessionLoaded`
       and `(sessionId, plan.changedKeys())`. Outcome `Merged`. **The session
       stays loaded:** it is dirty and must be saved from memory, and
       `importFiles` is about to make it visible anyway.
2. Model-reset bracket: call `beginResetModel()` lazily, immediately before the
   first mutation of `m_rows` (append or `row.session` assignment) or of a
   loaded session; if it was begun, `endResetModel()` after the loop. A batch in
   which every file is `Unchanged` or `Failed` therefore emits **nothing**.
3. After `endResetModel()`, per created row: `invalidateAllColumns(row)`,
   `scheduleSave(id)`. Per merged session: `invalidateColumns(row, keys)`,
   `publishInvalidation(row, keys)` (the single emitter of `dependencyChanged`,
   Phase 3 Task 3.10), `scheduleSave(id)`; `emit sessionLoaded(id)` for rows
   that became loaded. Then once: `evictIfNeeded()`, `startColumnWorker()`,
   `emit modelChanged()`. Delete the baseline per-row dirty/counter code
   (538-542, 550-552, 562-564) in favor of `scheduleSave`, and the
   "recompute every loaded row" loop (571-577, already removed by Phase 5).
4. Replace the `qDebug` lines by one `qDebug` per file stating the outcome.

`resolveIdentityStubs()`: for every row with `!isLoaded()` and
`logbook.isIdentityEntry(row.sessionId)`: `real = logbook.peekSessionId(row.sessionId)`;
if it has a value, is non-empty, differs from the row id, and
`logbook.remapSessionId(row.sessionId, *real)` succeeds -> `row.sessionId = *real`.
Unreadable files and refused remaps (duplicate id) leave the row as it is. No
signals (the id is not displayed; cached values move with the remap). Cost: a
header-only read per unresolved stub, and only in logbooks that came up through
the deferred scan or orphan adoption.

**`sessionRef` (813-850) - the empty-fallback fix (finding 2):** on load failure
keep returning an empty `SessionData` (the function returns a reference and has
many callers) but set `sr.loadFailed = true`, do **not** call `attachSession`,
and keep the existing warning. Guards: `scheduleSave` returns immediately for a
`loadFailed` row; `saveNextSession`, `flushDirtySessions`, and `evictSession`
skip saving a `loadFailed` row (clearing `dirty`); `evictSession` resets it to a
stub and clears the flag so a later access retries the load. `mergeSessions`
treats it as not loaded (above), so a placeholder can never acquire a
`SESSION_ID` and be written over the real file.

Add to the rule comment in `sessionmodel.h` (Phase 5 Task 5.7): "`mergeSessions`
is the only import path. It never assigns an incoming file to an existing row;
an existing row's session changes only through `SessionMerge::apply`."

**Acceptance Criteria:**
- [ ] `git grep -n "getAttribute\|getMeasurement" -- src/sessionmodel.cpp` shows no hit inside `mergeSessions` / `resolveIdentityStubs`; the stub-replace assignment `rowIt->session = newSession` no longer exists for matched rows.
- [ ] A `Failed` or `Unchanged` result leaves, for that session: `dirty`, `cachedValues`, loaded/stub state, `LogbookManager::hasUnsavedColumns(id)`, the session CSV bytes, and `index.json` bytes (after `waitForIdle`) unchanged, and emits no `dependencyChanged`, `dataChanged`, `modelReset`, or `modelChanged`.
- [ ] `emit dependencyChanged` still occurs only inside `publishInvalidation`.
- [ ] Every site in `mergeSessions` that gives a row a session calls `attachSession`; every mutated row gets `invalidateColumns` or `invalidateAllColumns` before the function returns.
- [ ] Tests of Task 6.7 pass.

**Complexity:** L (natural split: result type + create/merge-in-place; load-before-merge + LRU; `loadFailed` guards + `resolveIdentityStubs`; effects)

---

### Task 6.5: Startup no longer imports the logbook through `mergeSessions`

**Purpose:** Finding 7: with `mergeSessions` now meaning "import", the legacy flat-index startup branch (baseline `mainwindow.cpp` 180-186) would mark every saved session as newly created, dirty it, and rewrite every file - contrary to spec 9.3 ("no migration step or rewrite of existing files").

**Files to modify:**
- `src/mainwindow.cpp` (baseline 168-187)
- `src/logbookmanager.h` / `.cpp` - delete `loadAllSessions()` and `scanSessionFiles()` (no callers remain).

**Technical Approach:**

In the legacy flat branch of `initialize()` (204-213) `m_sessionIdToUuid` holds
real ids, and `cachedColumnValues(liveColumns)` (249-292) already returns an
entry for every known session. Startup becomes two branches:

```cpp
if (logbook.hasDeferredScan()) { model->populateFromUuids(logbook.scannedUuids()); }
else { model->populateFromIndex(logbook.cachedColumnValues(liveColumns), logbook.lastAccessedMap()); }
model->startColumnWorker();
```

Rows start as stubs with empty `cachedValues`; the existing `ColumnTask` fills
them and its completion flush writes the extended-format index. No session file
is opened for writing. `initialize()` keeps its (always empty) return value to
avoid churn in tests; mark it `// always empty; kept for source compatibility`.

**Acceptance Criteria:**
- [ ] `git grep -n "mergeSessions" -- src` shows callers only in `sessionimport.cpp` (Task 6.6).
- [ ] New test `tst_logbook_index::legacyFlatIndexStartsAsStubs`: write a flat-format `index.json` (`{"<id>": {"uuid": "<uuid>"}}`) next to one saved session; `reopenLogbook()`, `initialize()`, populate a model as above, `waitForIdle` -> one row, not dirty, CSV bytes unchanged, `index.json` now has `columns` and `calculationCompatibility`.

**Complexity:** S

---

### Task 6.6: Widget-free import driver and error surfacing in the existing dialog

**Purpose:** Keep `MainWindow::importFiles` thin, make the whole import flow testable in `flysight_core`, and put each file's error text in the existing `QMessageBox` ("No new UI").

**Files to create:**
- `src/sessionimport.h` / `src/sessionimport.cpp` (in `flysight_core`)

**Files to modify:**
- `src/mainwindow.cpp` - `importFiles` (baseline 564-681)
- `src/CMakeLists.txt`

**Technical Approach:**

```cpp
namespace FlySight::SessionImport {
struct BatchResult {
    QList<MergeResult> files;                    // one per attempted file, input order (parse failures included)
    QStringList importedSessionIds() const;      // unique ids of Created / Merged / Unchanged, first-seen order
    QList<MergeResult> failures() const;
};
// progress(i, total) is called before file i is parsed; returning false cancels: files not yet
// parsed are neither imported nor reported. May be empty.
BatchResult importFiles(SessionModel &model, const QStringList &filePaths,
                        const std::function<bool(int, int)> &progress = {});
// Text for the existing warning box; empty when there are no failures.
QString failureMessage(const QList<MergeResult> &failures, const QString &baseDir);
}
```

- `importFiles`: for each path a fresh `DataImporter`, `parseFile`; failure ->
  `MergeResult{path, "", Failed, importer.getLastError()}` (plus the existing
  `qWarning`); successes are collected and passed to **one**
  `model.mergeSessions(parsed)` call (one model reset per batch, as today); the
  two result lists are interleaved back into input order.
- `failureMessage`: display path per file as baseline 592-601 (relative to
  `baseDir`, else file name / full path). One line per failure:
  `<displayPath>: <error>`. Up to 5 failures:
  `"Import has been completed.\nHowever, some files failed to import:\n" + lines`.
  More than 5: `"Import has been completed.\nHowever, %1 files failed to import.\nFailed Files:\n"`
  + the first 10 lines + `"\n...and %1 more."` when applicable - the same
  thresholds and sentences as 661-680, now with the error text. Strings go
  through `QCoreApplication::translate("MainWindow", ...)` so existing
  translations keep their context. Failures keep input order (baseline sorted
  them through a `QMap` and silently collapsed duplicates).
- `MainWindow::importFiles` becomes: progress-dialog wiring (the lambda returns
  `!progressDialog.wasCanceled()`), `SessionImport::importFiles`, visibility
  (636-648) driven by `importedSessionIds()` - `Unchanged` sessions are shown
  too, the user asked for them -, zoom (651-658) using
  `QVector<SessionData>` built from `model->sessionRef(row)` of those ids
  **after** `setRowsVisibility` (the merged session, not the single file, is
  what should be framed), then
  `if (!(msg = failureMessage(...)).isEmpty()) QMessageBox::warning(this, tr("Import Completed with Some Failures"), msg);`.
  No `DataImporter` include remains in `mainwindow.cpp`.
- The three entry points (finding 7) are unchanged callers of `MainWindow::importFiles`.

**Acceptance Criteria:**
- [ ] `git grep -n "DataImporter\|mergeSessions" -- src/mainwindow.cpp` returns nothing.
- [ ] `sessionimport.*` includes no Widgets header; `flysight_core`'s link set is unchanged.
- [ ] The dialog title, icon, and thresholds are unchanged; only the body gains `: <error>` per file. No other UI element is added.

**Complexity:** M

---

### Task 6.7: Acceptance suites

**Purpose:** Demonstrate acceptance 3, 7, 8, the merge halves of 10 and 18, and spec 6.2-6.5.

**Files to create:**
- `tests/tst_import_merge.cpp` (class `ImportMergeTest`) - model + temp logbook
- `tests/tst_import_batch.cpp` (class `ImportBatchTest`) - `SessionImport`

**Files to modify:** `tests/CMakeLists.txt`, `tests/tst_smoke.cpp`, `tests/tst_session_model_engine.cpp`, `tests/README.md`.

**Technical Approach:**

Common: `initTestCase` -> `registerBuiltIns()`; `init` -> `useFreshLogbook()`,
`resetPreferencesToDefaults()`, `LogbookManager::instance().initialize()`, a
fresh `SessionModel`. Files are written with the builders into
`newTempDir()` **under a device-style folder** `24-01-01/12-00-00/TRACK.CSV`
and `.../SENSOR.CSV` unless stated. Helper `importOne(model, path)` =
`SessionImport::importFiles(model, {path}).files.first()`. Helper
`makeStub(model)`: `waitForIdle`, destroy the model, `reopenLogbook()`,
`initialize()`, new model + `populateFromIndex` -> the row is an unloaded stub.
`snapshot(id)` = CSV bytes of the session + `index.json` bytes. `near` = 1e-9.

**Order-independence comparison rule (used by `mergeOrder*`):** two end states
are "the same result" iff (a) `sourceData()` are equal (`==`), (b) the set of
attribute keys is equal, (c) every header attribute and every `_` attribute
**except `_IMPORT_TIME`** has equal `CsvFormat::formatAttributeValue` text -
`_DESCRIPTION` included, which is deterministic here because both files live in
the same device folder -, (d) `_IMPORT_TIME` is present in both, (e) effective
`IMU/wx`, `IMU/ax`, `GNSS/hMSL`, `IMU/wTotal`, `_START_TIME`, `_DURATION` are
equal and match the literals, (f) the saved CSVs are byte-identical after
removing the `$VAR,_IMPORT_TIME,` line.

`tst_import_merge`:

| Test | Scenario | Expected |
|---|---|---|
| `newSessionGetsDefaults` (6.2) | import TRACK | `Created`; one row; `_DESCRIPTION == "24-01-01/12-00-00"`, `_JUMPER_MASS == 1.0`, `_WIND_N == 0.0`, `_IMPORT_TIME > 0`; dirty, then saved after `waitForIdle` |
| `mergeOrderLoaded` (acc. 7) | model A: TRACK then SENSOR; model B (fresh logbook): SENSOR then TRACK; sessions stay loaded | second import `Merged` in both; one row each; sensors {`GNSS`,`IMU`,`MAG`}; comparison rule holds; `IMU/wx` near `71.68`, `IMU/ax == 9.80665`, `sourceUnit("IMU","ax") == "g"` |
| `mergeOrderUnloaded` (acc. 7) | same, with `makeStub` between the two imports | same comparison rule, and A-unloaded equals A-loaded; after the merge the row `isLoaded()` and `sessionLoaded` was emitted once |
| `mergeInOneBatch` (acc. 7) | `importFiles({TRACK, SENSOR})` and, in another logbook, `{SENSOR, TRACK}` | outcomes `Created`, `Merged`; comparison rule holds between the two and against `mergeOrderLoaded` |
| `conflictChangesNothing_loaded` / `_unloaded` (acc. 7) | session from TRACK; SENSOR variant with `FIRMWARE_VER` = `v2024.01.01`; second data row: `DEVICE_ID` = `other-device` | `Failed`; error contains `'FIRMWARE_VER'` (resp. `'DEVICE_ID'`) and both values; `snapshot` identical before/after `waitForIdle`; row `dirty` false; stub variant: row still not loaded, `cachedValues` unchanged; no `dependencyChanged` / `modelReset` (spies) ; session has no `IMU` sensor |
| `batchPartialFailure` | `importFiles({TRACK, conflictingSENSOR})` on an empty logbook | `Created`, `Failed`; the session exists with `GNSS` only and is saved |
| `editsSurviveMerge` (acc. 7, 6.5) | TRACK imported; `updateAttribute` `_DESCRIPTION` = `"my jump"`, `_EXIT_TIME` = `1704110400.2` (a marker), `_GROUND_ELEV` = `12.5`, `_WSP_TOP_ALT` = `2400.0`; (variant: `makeStub`); import SENSOR | all four values unchanged (`formatAttributeValue` text `my jump`, `1704110400.2`, `12.5`, `2400`); `_IMPORT_TIME` text unchanged; saved CSV contains them |
| `unmatchedMeasurementsSurvive` (acc. 7, 6.4) | session from a SENSOR file with an extra custom sensor `FOO/{time,bar}`; re-import the plain `Fixtures::sensorFile()` with `IMU/wx` row value changed to `10` | `Merged`; `FOO/bar` still `{7.0}` with unit `furlongs`; `sourceMeasurement("IMU","wx") == {10.0}`; `MAG/x` untouched and **not** in the emitted keys |
| `defaultsNotReappliedOnMerge` (6.2) | TRACK imported with `AeroMass` 1.0; set `AeroMass` 90.0, `ImportGroundReferenceMode` `Fixed`, `ImportFixedElevation` 50.0; import SENSOR from a **different, non-device folder** | `_JUMPER_MASS` text `1`; `_GROUND_ELEV` not stored; `_DESCRIPTION` and `_IMPORT_TIME` text unchanged; `DEVICE_ID` unchanged |
| `rejectedSchemaLeavesSession_loaded` / `_unloaded` (acc. 3; data rows `3`, `abc`) | session from SENSOR (edited description); import the same file with `.var("SCHEMA_VER", <row>)` | `Failed`; error == `Unsupported SCHEMA_VER '3' (supported: 1, 2)` (resp. `'abc'`); `snapshot` identical; session attributes / `sourceData()` equal to pre-import copies; stub variant: still a stub; `hasAttribute("SCHEMA_VER")` false |
| `escapeHatch` (acc. 8) | SENSOR imported; `wx` near `71.68`; `updateAttribute(_DESCRIPTION, "keep me")`; note `rowCount()`; re-import the same file with `.var("SCHEMA_VER","2")` (variant: on a stub) | `Merged`; `rowCount()` unchanged (1); `wx == 62.5`, `wy == -125.0`, `IMU/wTotal` near `139.75424859373686`; `_DESCRIPTION == "keep me"`; `dependencyChanged` spy contains `IMU/wx`, `IMU/wTotal`, `SCHEMA_VER` and not `IMU/ax`; after `waitForIdle` the CSV contains exactly one `$VAR,SCHEMA_VER,2` and the recorded `62.5`; `verifyAgainstFresh({wx, wy, wz, wTotal})` empty |
| `explicitSchemaMismatch` (6.3) | session imported with `SCHEMA_VER,2`; file says `1` | `Failed`; error contains `'SCHEMA_VER'`, `session: '2'`, `file: '1'`, `delete the session and re-import`; `wx` still `62.5` |
| `absenceNeverConflicts` (6.3) | SENSOR with `SCHEMA_VER,2` then TRACK without; and the reverse order | both `Merged`; `SCHEMA_VER == "2"` in both; `wx == 62.5` |
| `failedLoadIsAnError` (6.2) | session saved; `makeStub`; overwrite its CSV with `"corrupt"`; import SENSOR | `Failed`; error == `Existing session 'test-session' could not be loaded (Unknown file format); the file was not imported.`; CSV bytes still `"corrupt"` after `waitForIdle`; row still a stub, not dirty; `index.json` unchanged |
| `failedPlaceholderIsNeverSaved` (finding 2) | as above but call `model.sessionRef(0)` first (placeholder installed), then import SENSOR, `updateAttribute(_DESCRIPTION,"x")`, `waitForIdle`, `flushDirtySessions()` | import `Failed`; CSV bytes still `"corrupt"` throughout |
| `identityStubIsMatched` (finding 3) | save TRACK session; delete `index.json`; `reopenLogbook()`; `initialize()` (deferred scan); `populateFromUuids`; **without** running the column worker import SENSOR | `Merged`, not `Created`; `rowCount() == 1`; exactly one `*.csv` in `sessionsDir()` after `waitForIdle`; row id == `test-session` |
| `mergeEffects` (6.5) | loaded TRACK session, idle, columns D = `_DESCRIPTION`, G = `MeasurementAtMarker{IMU, wx, _M}`-style column whose value needs IMU; import SENSOR | spy has `dependencyChanged(id, IMU/wx)`; row `dirty` right after the call; after `waitForIdle`: not dirty, CSV has `$COL,IMU`, index value of D unchanged and **not recomputed** (`columnWorkStats().valuesComputed` counts only the affected columns), affected column refreshed (acc. 18 merge part) |
| `identicalReimportIsNoop` | session imported and saved; `QSignalSpy` on `dependencyChanged`, `modelReset`, `modelChanged`, `dataChanged`; re-import both original files (loaded variant and stub variant) | `Unchanged` twice; all spies empty; not dirty; `snapshot` identical; file mtime unchanged; stub variant: row still a stub and `columnWorkStats().sessionsLoaded == 0` afterwards |
| `fs1ReimportMergesIntoItself` | import an FS1 file twice | `Created` then `Unchanged`; one row; row id is the 32-hex MD5 |
| `candidateSwitchAfterMerge` (acc. 10) | `DescentFixture::sensorFile()` imported alone; read `_START_TIME` -> `1704110410.0`, `_DURATION` -> `20.0` (BARO candidate); import `DescentFixture::trackFile()` | spy contains `_START_TIME`, `_DURATION`; reads now `1704110400.0` / `295.0`; `runCount("builtin.attr.timeExtent.GNSS") == 1`; `verifyAgainstFresh({_START_TIME, _DURATION, _EXIT_TIME})` empty; stub variant gives the same values |
| `raggedMergeRejected` | session with `X/{time,v,keep}` 3 rows (programmatic via the `SessionData` overload); import a file with `X/{time,v}` 2 rows | `Failed` with Task 6.3's literal message; snapshot identical |
| `sessionDataOverloadAdoptsAsIs` | `mergeSessions({programmaticSession})` | `Created`; no `_IMPORT_TIME` / `_DESCRIPTION` added; a session without `SESSION_ID` -> `Failed`, `"File has no SESSION_ID"` |

`tst_import_batch`:

| Test | Expected |
|---|---|
| `resultsKeepInputOrder` | `{good, "hello"-file, missing path, good2}` -> outcomes `Created`, `Failed` (`Unknown file format`), `Failed` (`Couldn't read file`), `Created`; `importedSessionIds()` == the two ids |
| `progressCancels` | callback returns false at `i == 1` of 3 -> one result, one session; callback saw `(0,3)`, `(1,3)` |
| `failureMessageFew` | two failures, `baseDir` set -> literal: `"Import has been completed.\nHowever, some files failed to import:\nsub/SENSOR.CSV: Attribute 'FIRMWARE_VER' conflicts ...\nbad.csv: Unknown file format"` (full literal in the test) |
| `failureMessageMany` | 12 failures -> count line `12 files failed`, 10 listed each with `: <error>`, `...and 2 more.` |
| `failureMessageEmpty` | no failures -> `""` |
| `mergeFailureReachesMessage` | end-to-end: conflicting SENSOR after TRACK -> `failureMessage(result.failures(), dir)` contains the file's relative path and `'FIRMWARE_VER'` |

Existing tests to update:
- `tst_smoke::modelMergesTrackAndSensor` - remove the `// BASELINE: merge rules change in Phase 6` comment; additionally assert the second call's result is `Merged`.
- `tst_session_model_engine::mergeEmitsDependencyChanged` - under the no-op rule an identical sensor session emits nothing; make the incoming session's `IMU/wx` differ from `s1`'s (e.g. first sample `99`) and keep the expectation (`s1`, `IMU/wx`).
- Any test that ignored `mergeSessions`' (formerly `void`) result needs no change.

**Acceptance Criteria:**
- [ ] Both targets registered with `flysight_add_test`, pass on Windows Release; acceptance numbers 3, 7, 8, 10, 18 appear in comments on the functions above.
- [ ] Every expected value and message is a literal; computed comparisons are limited to A-versus-B state equality (which acceptance 7 asks for), snapshot equality, and `verifyAgainstFresh`.
- [ ] `git grep -n "BASELINE:" tests/` returns nothing.
- [ ] Temporarily restoring "incoming attributes overwrite" in `SessionMerge::plan` makes `editsSurviveMerge` and `defaultsNotReappliedOnMerge` fail; temporarily skipping `resolveIdentityStubs()` makes `identityStubIsMatched` fail - checked once by hand, not committed.
- [ ] Nothing is written outside `TestEnvironment::rootPath()`.

**Complexity:** L

---

## Testing Requirements

### Unit Tests
- New targets: `tst_session_merge` (6.3), `tst_import_merge`, `tst_import_batch` (6.7).
- Extended: `tst_importer` (6.1), `tst_logbook_index` (6.2, 6.5).
- Deliberately updated: `tst_smoke::modelMergesTrackAndSensor`, `tst_session_model_engine::mergeEmitsDependencyChanged` (Task 6.7). Every other Phase 1-5 test passes unchanged; in particular `releasedLogbookBackfillIsAdditive` keeps its expected line set (the backfill is kept).
- Acceptance mapping: **3** - `rejectedSchemaLeavesSession_*` (+ Phase 4 `tst_importer`); **7** - `mergeOrderLoaded`, `mergeOrderUnloaded`, `mergeInOneBatch`, `conflictChangesNothing_*`, `editsSurviveMerge`, `unmatchedMeasurementsSurvive`; **8** - `escapeHatch`; **10** (merge part) - `candidateSwitchAfterMerge`; **18** (merge part) - `mergeEffects`; spec 6.2 - `newSessionGetsDefaults`, `defaultsNotReappliedOnMerge`, `failedLoadIsAnError`; 6.3 - `tst_session_merge`, `explicitSchemaMismatch`, `absenceNeverConflicts`; 6.4 - `columnsReplaceAddKeep`, `raggedIsRejected`; 6.5 - `mergeEffects`, `identicalReimportIsNoop`.

### Integration Tests
- After every task: build with `-DFLYSIGHT_BUILD_TESTS=ON`, `ctest --test-dir build/FlySightViewer-build -C Release --output-on-failure` all green; after 6.4 and 6.6 also build with the option OFF.
- Grep criteria of Tasks 6.1, 6.3, 6.4, 6.5, 6.6 are part of the check.

### Manual Verification
1. Import a device folder containing `TRACK.CSV` + `SENSOR.CSV`: one session, description from the folder names; restart: still one session.
2. Import `TRACK.CSV` alone, edit the description, drag the exit marker, set ground elevation, restart (session unloaded), import `SENSOR.CSV`: IMU plots appear, all edits intact, the `_IMPORT_TIME` line in the CSV unchanged.
3. Re-import the same two files: nothing happens - no save progress in the status bar, the session file's modification time is unchanged.
4. Copy `SENSOR.CSV`, add `$VAR,SCHEMA_VER,2`, import: same session, gyro plots drop by 1/1.14688, edits intact. Change the copy to `SCHEMA_VER,1` and import: the dialog shows the file name followed by the conflict text and the delete / re-import hint; plots unchanged.
5. Change `FIRMWARE_VER` in a copy and import: dialog names `FIRMWARE_VER` with both values; session unchanged.
6. Import a folder with more than five broken files: the summary form of the dialog lists ten files, each with its reason.
7. Make a session's CSV unreadable (rename to keep a backup, write junk), restart, import a file of that session: error dialog says the existing session could not be loaded; the junk file is untouched. Restore the backup.
8. Delete `index.json`, start, and immediately import a file of an existing session: no duplicate row appears.
9. Drag-and-drop a CSV onto the window: same behavior as File > Import.

## Notes for Implementer

### Gotchas
- **Never read effective values in the merge.** `SessionMerge` works on `sourceData()` / `storedAttribute` only. Reading `getMeasurement` would copy corrected gyro values into the source layer and the correction would be applied twice (the bug Phase 4's minimal fix closed).
- **Plan, then apply.** No `setAttribute` / `mergeSourceData` call may precede the last validation. For unloaded sessions the mutation happens on the loaded copy, and the copy is installed only on success - do not load through `sessionRef` (it installs into the row, emits `sessionLoaded`, touches the LRU, and applies the backfill before the merge decision).
- **`applyLegacyBackfill` runs after `apply`, never before `plan`**, and never on the incoming file.
- **`beginResetModel()` is lazy.** A batch of pure no-ops / failures must not reset the model (views lose selection and scroll position on reset).
- **Rows move.** `m_rows.append` may reallocate; do not hold `SessionRow&` / `SessionData&` across an append. Collect `(sessionId, keys)` and look rows up again after the loop. `attachSession` must be called on the element inside `m_rows`.
- **`publishInvalidation` for a session loaded only for the merge** has just the plan's keys (cold engine, no recorded dependents). That is sufficient: nothing can have cached values of an unloaded session except logbook columns, and those are handled by `invalidateColumns`' static closures.
- **`invalidateColumns` before `scheduleSave`** (Phase 5 ordering: unsaved marks must exist before the save runs).
- **A file without a recorded `SESSION_ID` changes identity when edited**: adding `$VAR,SCHEMA_VER,2` to such a file changes its MD5, so it imports as a new session. FlySight 2 firmware always records `SESSION_ID` in both files, and FS1 files have no gyro, so the escape hatch is unaffected.
- **`DEVICE_ID` from `FLYSIGHT.TXT` is an ordinary value after creation.** Only the literal placeholder `n/a` is treated as absent. Test fixtures always record `DEVICE_ID`, so tests never walk parent directories, except `creationDefaultsDeviceId`, which builds its own tree under `newTempDir()`.
- **Bitwise column comparison** touches every sample of same-named columns once per re-import (`memcmp`, no allocation). Do not replace it with `QVector::operator==` (NaN != NaN would defeat the no-op rule).
- **`PreferencesManager::getValue` may return a `QString`** for `AeroMass` when `QSettings` read an INI file (tests): compare `_JUMPER_MASS` through `formatAttributeValue` text or `toDouble()`, never by `QVariant ==`.
- **Bulk-edit queue items hold row indices**; `mergeSessions` only appends rows, so indices stay valid.
- Phase 7 is editing `pluginhost.cpp`, `*_bindings.cpp`, `pluginsessionview.h`, `python_plugins/` concurrently. Nothing here needs them.

### Decisions Made
- **Parse result versus creation.** `DataImporter::parseFile` -> `ParsedFile` (recorded attributes + source data + a match id that is *not* written into the data). `DataImporter::applyCreationDefaults(file, session)` is the single writer of import-time defaults and is invoked by `SessionModel::mergeSessions` only on the create branch. It lives in the importer (it needs the path, `FLYSIGHT.TXT`, and preferences - file-system concerns), while the model owns the decision *when*. `importFile` stays as the composition of the two for tests and tools; `initializeFromDevice` is deleted.
- **Synthesized identifiers** (overview decision): the MD5 `SESSION_ID` and the `FLYSIGHT.TXT` / `n/a` `DEVICE_ID` are creation-time values. Matching uses the recorded `SESSION_ID`, or the hash when none is recorded, so re-importing the same FS1 file matches its own session and is a no-op (confirmed harmless; a modified FS1 file is a new session, as at baseline). A recorded `DEVICE_ID` is a header attribute under the conflict rule; the placeholder `n/a` counts as absent on either side, which keeps "file without `DEVICE_ID` first, file with it second" from conflicting.
- **Result type:** `MergeResult { filePath, sessionId, Outcome {Created, Merged, Unchanged, Failed}, error }`; `mergeSessions` returns `QList<MergeResult>` in input order; `SessionImport::BatchResult` adds parse failures.
- **Equality rule:** equality of `CsvFormat::formatAttributeValue` text - exact, untrimmed, type-independent. Both sides of every real header comparison are verbatim `QString`s (importer and reload), so `2` equals `2`, and a fresh in-memory value equals its reloaded form.
- **`_` attributes in an incoming (Viewer-saved) file:** existing wins, absent added, never a conflict (overview decision, flagged there).
- **All conflicts of a file are reported in one message**, each naming the attribute and both values, with a delete-and-re-import hint; the hint is schema-specific when `SCHEMA_VER` is involved (spec 6.3).
- **Ragged rule: reject.** A merge that would leave a sensor with columns of unequal length fails like a conflict and changes nothing. This does not bend spec 6.4 - kept measurements are kept; the file is simply not merged - and it honors Phase 5's rule that a ragged sensor cannot be saved. Replacing the whole sensor would silently drop measurements (against 6.4); padding would invent samples. In practice the case needs a session holding extra columns *in the same sensor* as the incoming file with a different row count, which no FlySight workflow produces (TRACK = `GNSS`, SENSOR = the other sensors; a re-import of the same file replaces all of its sensor's columns together).
- **No-op rule:** an empty plan (`Unchanged`) mutates nothing, emits nothing, dirties nothing, saves nothing; an unloaded session loaded for the comparison is discarded and the row stays a stub.
- **Order-independence comparison rule:** everything is compared (source data, key set, all header attributes, all `_` attributes, effective values, saved bytes) except the value of `_IMPORT_TIME`, which is the wall clock of the creating import and cannot be equal across two runs. `_DESCRIPTION` is compared and is equal whenever the two files share a folder that yields the same description - always true for the device layout (`YY-MM-DD/HH-MM-SS/`). For files in an arbitrary folder the description falls back to the **creating** file's name (`TRACK.CSV` versus `SENSOR.CSV`); this is inherent in "defaults are applied only at creation" (spec 6.2) and is left as is - see Open Questions.
- **Stub matching:** `resolveIdentityStubs()` runs at the start of every `mergeSessions`, using a header-only `peekSessionId`; unloaded real-id stubs match by row id; a loaded copy whose `SESSION_ID` disagrees with its index entry is a load failure.
- **Unloaded merges leave the session loaded** (dirty, about to be shown), with normal LRU accounting and one `evictIfNeeded()` per batch.
- **Empty-fallback fix:** `SessionRow::loadFailed`; placeholders are never saved, never attached, never merged into; a merge retries the raw load and reports the failure.
- **Backfill policy: keep, as a named legacy-logbook shim** (`LogbookManager::applyLegacyBackfill`), applied on ordinary loads and after (never before) a merge decision, never to incoming files, never dirtying. Removing it would make `wcVel`, `wcVelH`, `accAlongTrack`, `accCrossTrack`, `lift`, `drag` unavailable for every session saved before those attributes existed (finding 6) - a visible regression for users of old logbooks with no benefit to the spec's goals. It cannot create a conflict (`_` keys never conflict) and cannot influence a merge (it runs afterwards).
- **Startup no longer uses `mergeSessions`** (Task 6.5); the legacy flat index comes up as stubs like any other logbook, so no session file is rewritten on upgrade.
- **Dialog format:** same box, title, and thresholds; each listed file becomes `<path>: <error>`; input order instead of alphabetical. Text is built by `SessionImport::failureMessage` so it is testable without widgets.
- **Model reset per batch is kept** (baseline behavior) rather than switching to `beginInsertRows`, to avoid changing view/proxy behavior in this phase; it is merely skipped when nothing changed.

### Open Questions
- **`_DESCRIPTION` for file pairs outside a device folder** depends on which file creates the session. If Michael wants this order-independent too, the cheapest rule is: when the description would fall back to a file name and the file is named `TRACK.CSV` / `SENSOR.CSV` (case-insensitive), use the parent folder name instead. Not done here because it changes a visible default for existing workflows.
- **`DEVICE_ID` placeholder `n/a` treated as absent** is an interpretation of the overview's "creation-time identifiers are not compared". The alternative - not writing a placeholder at all ("missing attribute = no value") - is cleaner but changes what the `DEVICE_ID` logbook column shows for such sessions (blank instead of `n/a`). Michael's call; either way only `SessionMerge::plan` and `applyCreationDefaults` change.
- **Backfill** is kept as a shim (above). If Michael prefers strict "missing = no value", the smallest faithful alternative is constant-default calculations for `_WIND_N` / `_WIND_E` (like `builtin.wsp.default.*`) and leaving mass / area absent; that touches Phase 3's inventory test and Phase 5's `releasedLogbookBackfillIsAdditive`.
- **Reporting every conflict versus the first:** this document reports all, sorted by key. Trivial to reduce if the dialog reads too long.

## Definition of Done

This phase is complete when:
1. All seven tasks have passing acceptance criteria.
2. `tst_session_merge`, `tst_import_merge`, `tst_import_batch`, the extended `tst_importer` / `tst_logbook_index`, the two updated tests, and every other Phase 1-5 suite pass via CTest on Windows Release; the application and `flysight_cpp_bridge` build with `FLYSIGHT_BUILD_TESTS` ON and OFF.
3. Spec coverage: 3.2 (header attributes preserved, nothing invented on merge) - Tasks 6.1, 6.3; 6.1 (rejected file leaves an existing session untouched) - `rejectedSchemaLeavesSession_*`; 6.2 - Tasks 6.1, 6.4 (`newSessionGetsDefaults`, `defaultsNotReappliedOnMerge`, `failedLoadIsAnError`, `identityStubIsMatched`); 6.3 - Task 6.3, `explicitSchemaMismatch`, `absenceNeverConflicts`, `escapeHatch`; 6.4 - Task 6.3, `unmatchedMeasurementsSurvive`; 6.5 - Task 6.4, `mergeEffects`, `editsSurviveMerge`, `identicalReimportIsNoop`; 7.4-7.5 (merge invalidates, fallback replaced) - `candidateSwitchAfterMerge`; 12 (nothing rewrites recorded attributes; one mutation path: `SessionMerge::apply`) - grep criteria of 6.3 / 6.4; acceptance 3, 7, 8, 10 (merge), 18 (merge) - mapped under Testing Requirements; "No new UI" - Task 6.6.
4. No code path assigns an incoming file over an existing session, applies an import-time default during a merge, or saves a failed-load placeholder; `mergeSessions` has no caller outside `SessionImport::importFiles` and tests.
5. Files owned by Phase 7 are untouched; no TODOs, `BASELINE:` markers, or placeholder code remain; nothing has been pushed.
