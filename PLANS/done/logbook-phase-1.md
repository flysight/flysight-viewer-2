# Phase 1: Persistence Layer

## Overview

Sessions currently exist only in memory — closing the app loses all imported
data. This phase adds save-to-disk on import and load-from-disk on startup so
sessions survive across app launches. No lazy-loading or background
optimization (that's Phase 2).

## Dependencies

- **Depends on:** Prerequisite cleanup (plan `happy-orbiting-harbor`) — ensures
  `m_attributes` contains only ground-truth data.
- **Blocks:** Phase 2 (startup optimization).

## Tasks

### Task 1.1: Store Unit Strings in `SessionData`

**Purpose:** The FS2 exporter needs to write `$UNIT` lines. Currently, unit
strings are consumed during import for SI conversion and then discarded.

**Files to modify:**

- `src/sessiondata.h` — Add `m_units` map and accessors
- `src/sessiondata.cpp` — Implement accessors
- `src/dataimporter.cpp` — Store unit strings during import

**Technical Approach:**

Add a private member `QMap<QString, QMap<QString, QString>> m_units` to
`SessionData`, keyed as `m_units[sensorKey][measurementKey] = unitString`.
Add `setUnit()` and `getUnit()` public methods.

In `DataImporter::importHeaderRow()` (line 218), when parsing `$UNIT` lines,
call `sessionData.setUnit()` for each column alongside the existing local
`columnUnits` storage.

In `DataImporter::importFS2()` (lines 159-173), after applying SI conversion
to a column, update the stored unit to the SI value from
`ConversionSpec::siUnit`. This way `getUnit()` always returns the unit of
the data as it exists in memory (post-conversion).

**Reference:** `UnitConversion` lookup table in `src/units/unitconversion.h`
lines 93-125 defines all known units and their SI equivalents.

**Acceptance Criteria:**

- [ ] After importing an FS2 file with `$UNIT` lines, `getUnit()` returns the
  correct SI unit string for each sensor/measurement pair
- [ ] For units that required conversion (e.g., `g` → `m/s^2`), `getUnit()`
  returns the post-conversion SI unit, not the original
- [ ] For measurements with no `$UNIT` line, `getUnit()` returns empty string
- [ ] Build passes

**Complexity:** S

---

### Task 1.2: Separate Import Parsing from Device Initialization

**Purpose:** `importFile()` currently mixes FS2 parsing with device-specific
initialization (description from file path, device ID from FLYSIGHT.TXT,
session ID from MD5). Logbook files already contain these as `$VAR` lines and
must not go through initialization. Cleanly separating these concerns lets
logbook loads use the parser directly.

**Files to modify:**

- `src/dataimporter.h` — Add `readFile()` and `initializeFromDevice()`
- `src/dataimporter.cpp` — Extract initialization from `importFile()`

**Technical Approach:**

Split `importFile()` into two public methods:

1. `readFile(fileName, sessionData, fileData*)` — Reads the file, detects
   format, calls `importSimple()` or `importFS2()`. Optionally writes raw
   bytes to `fileData` out-parameter (needed by `initializeFromDevice` for
   MD5 hashing). Pure data parsing — no side effects beyond populating
   `sessionData`.

2. `initializeFromDevice(fileName, fileData, sessionData)` — The three
   post-import steps currently at lines 67-92: set description from path
   (`getDescription`), extract device ID from FLYSIGHT.TXT
   (`extractDeviceId`), generate session ID via MD5 if absent.

`importFile()` remains as a convenience wrapper that calls both. Existing
callers (`MainWindow::importFiles`) are unchanged.

**Reference:** Current `importFile()` at `src/dataimporter.cpp` lines 18-94.

**Acceptance Criteria:**

- [ ] `readFile()` parses FS1 and FS2 files correctly without setting
  description, device ID, or session ID
- [ ] `initializeFromDevice()` sets all three when called
- [ ] `importFile()` produces identical results to the current implementation
- [ ] Build passes

**Complexity:** M

---

### Task 1.3: FS2 Writer (`DataExporter`)

**Purpose:** Serialize a `SessionData` to FS2 format for logbook persistence.

**Files to create:**

- `src/dataexporter.h` — Class declaration
- `src/dataexporter.cpp` — Implementation

**Files to modify:**

- `CMakeLists.txt` — Add new source files

**Technical Approach:**

Single static method: `bool exportSession(const QString &filePath, const SessionData &sessionData)`.

Output format (fully compliant FS2):

```text
$FLYS,1
$VAR,key,value
...
$COL,sensorName,col1,col2,...
$UNIT,sensorName,unit1,unit2,...
$COL,sensorName2,...
$UNIT,sensorName2,...
$DATA
$sensorName,val1,val2,...
...
```

Key decisions:

- Version string is `$FLYS,1` (current FS2 format version).
- Attributes: iterate `attributeKeys()` / `getAttribute()`, write as `$VAR`
  lines. Use `QVariant::toString()`.
- Sensors: iterate `sensorKeys()` / `measurementKeys()`. Write `$COL` then
  `$UNIT` for each sensor. Unit strings come from `SessionData::getUnit()`.
  Empty string for unknown units (importer treats these as identity).
- Data rows: one per sample, `$sensorName,val1,val2,...`. All columns for a
  sensor have the same length.
- Use `qSetRealNumberPrecision(15)` with `SmartNotation` for full IEEE 754
  double precision.
- UTF-8 encoding, `\n` line endings, no BOM.
- Atomic writes: write to `filePath + ".tmp"`, then rename.
- Uses only the public `SessionData` API (no `friend` access needed).

**Reference:** `DataImporter::importFS2()` at `src/dataimporter.cpp` lines
138-174 for the inverse (reading) operation.

**Acceptance Criteria:**

- [ ] Exported file is valid FS2: starts with `$FLYS,1`, has `$VAR`/`$COL`/
  `$UNIT`/`$DATA` sections
- [ ] Round-trip: export a session, re-import with `readFile()`, verify
  attributes and measurements match the original
- [ ] Double precision preserved (no truncation artifacts)
- [ ] Partial writes don't corrupt existing files (atomic rename)
- [ ] Build passes

**Complexity:** M

---

### Task 1.4: Logbook Manager (`LogbookManager`)

**Purpose:** Own the logbook directory structure, map SESSION_IDs to UUID
filenames, and provide save/load/remove operations.

**Files to create:**

- `src/logbookmanager.h` — Class declaration
- `src/logbookmanager.cpp` — Implementation

**Files to modify:**

- `CMakeLists.txt` — Add new source files

**Technical Approach:**

Directory: `<GeneralLogbookFolder>/FlySight Viewer/logbook/sessions/`. The
`GeneralLogbookFolder` preference defaults to `QStandardPaths::DocumentsLocation`
(registered at `src/mainwindow.cpp` line 970).

Core state: `QMap<QString, QString> m_sessionIdToUuid` — maps SESSION_ID to
UUID filename (without extension).

Methods:

- `initialize()` — Create directory via `QDir().mkpath()`. Read `index.json`
  if it exists to populate `m_sessionIdToUuid`. If index is missing, scan
  `*.csv` files and read each file's SESSION_ID via `DataImporter::readFile()`.

- `saveSession(session)` — Look up SESSION_ID. Reuse existing UUID if found
  (handles re-import/merge). Otherwise generate via `QUuid::createUuid()`.
  Call `DataExporter::exportSession()`.

- `loadAllSessions()` — List `*.csv` in sessions dir. Parse each via
  `DataImporter::readFile()` (NOT `importFile()` — logbook files must not go
  through device initialization). Populate `m_sessionIdToUuid`. Return list.

- `removeSession(sessionId)` — Look up UUID, delete CSV, remove from map.

- `flushIndex()` — Write `index.json` with `m_sessionIdToUuid` entries and
  their attributes. Atomic write (temp + rename).

**Reference:** `ProfileManager` at `src/profilemanager.cpp` for the pattern of
managing a directory of files with UUID-based naming.

**Acceptance Criteria:**

- [ ] `initialize()` creates the directory structure if missing
- [ ] `saveSession()` creates a valid FS2 file with UUID filename
- [ ] Re-saving the same SESSION_ID overwrites the same file (no duplicates)
- [ ] `loadAllSessions()` returns sessions with correct attributes (no
  description/device-ID overwriting)
- [ ] `removeSession()` deletes the file from disk
- [ ] `flushIndex()` writes valid JSON; next `initialize()` reads it back
- [ ] Build passes

**Complexity:** L

---

### Task 1.5: Integration — Wire into MainWindow

**Purpose:** Connect the logbook manager to the application lifecycle: load on
startup, save on import, delete on remove, flush on shutdown.

**Files to modify:**

- `src/mainwindow.h` — Add `LogbookManager*` member, forward declaration
- `src/mainwindow.cpp` — Startup load, save-on-import, remove, flush

**Technical Approach:**

**Startup** (constructor, after model initialization): Create `LogbookManager`,
call `initialize()`, then `loadAllSessions()` and feed results into
`model->mergeSessions()`.

**Save on import** (`importFiles()`, after `model->mergeSessions()` at line
604): For each imported session, look up the merged result from the model by
SESSION_ID (via `getSessionRow()` + `sessionRef()`), and save that. Must save
the merged version, not the raw import, because `mergeSessions()` may have
combined data from multiple files.

**Remove** (`on_action_Delete_triggered()`, after `model->removeSessions()`
at line 790): Call `removeSession()` for each deleted SESSION_ID.

**Shutdown** (`closeEvent()` at line 301, before base class call): Call
`flushIndex()`.

**Reference:** Current `importFiles()` at `src/mainwindow.cpp` lines 541-653.
Current `on_action_Delete_triggered()` at lines 749-804. Current
`closeEvent()` at lines 301-305.

**Acceptance Criteria:**

- [ ] App starts with previously imported sessions visible in the logbook
- [ ] Newly imported sessions are persisted to disk immediately
- [ ] Deleting sessions removes their files from disk
- [ ] `index.json` is updated on clean shutdown
- [ ] All existing functionality (plots, markers, calculations, visibility)
  works with logbook-loaded sessions
- [ ] Build passes

**Complexity:** M

---

## Testing Requirements

### Manual Verification

1. Import sessions — verify `*.csv` files appear in
   `Documents/FlySight Viewer/logbook/sessions/`
2. Inspect a saved CSV — confirm valid FS2 with `$FLYS,1` header, `$VAR`
   lines, `$COL`/`$UNIT`/`$DATA` sections, full double precision
3. Close and reopen — verify sessions reappear with correct column values
4. Re-import the same files — verify no duplicate rows, existing CSV
   overwritten with merged data
5. Import multi-file session (GNSS + IMU from same flight) — verify merged
   CSV contains both sensors
6. Delete sessions — verify CSV files removed from disk
7. Verify `index.json` written on shutdown
8. Delete `index.json`, reopen — verify sessions still load (CSV scan
   fallback)
9. Existing functionality (plots, markers, calculations, visibility) works
   with logbook-loaded sessions

## Notes for Implementer

### Gotchas

- `getMeasurement()` triggers calculated-value computation for calculated
  keys. For raw sensor data (which is what we're exporting), it returns
  directly from `m_sensors` — but be aware of this if iterating keys that
  might include calculated measurements.
- `QFile::rename()` on Windows cannot overwrite an existing file. Use
  `QFile::remove()` on the target first, or use a platform-aware rename
  helper.
- The importer currently uses `friend class DataImporter` to access
  `m_sensors` directly for unit conversion (line 169). The exporter should
  NOT need friend access — the public API is sufficient.

### Decisions Made

- **`$UNIT` lines always written**, even though data is already in SI. This
  keeps exported files fully compliant with the FS2 format spec.
- **Unit strings stored in `SessionData`** during import rather than using a
  reverse lookup table, because measurement names come from file `$COL` lines
  and aren't constrained to a fixed set.
- **`readFile()` / `initializeFromDevice()` split** rather than a boolean flag,
  because the two concerns (parsing vs. device initialization) are genuinely
  independent and the split makes the intent clear at each call site.
- **Index stores attributes only** in Phase 1. Column display values will be
  added in Phase 2 when they're needed for instant table population.

## Definition of Done

This phase is complete when:

1. All tasks have passing acceptance criteria
2. Build passes with no errors
3. Sessions survive app restart (close → reopen → same sessions visible)
4. Exported files are valid FS2 that can be re-imported cleanly
5. No TODOs or placeholder code remains
