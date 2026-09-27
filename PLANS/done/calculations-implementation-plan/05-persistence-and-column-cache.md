# Phase 5: Persistence - exact round trip and column-cache versioning

## Overview

This phase makes saved sessions faithful and the logbook column cache
trustworthy. The exporter is rewritten to read **only** the source layer and
stored attributes, to write every double (rows and `$VAR` values) in a
shortest-round-trip form that the importer parses back to the same bits, to
handle non-finite values and unrepresentable text explicitly, and to refuse
ragged sensors instead of reading out of range. `index.json` gains an integer
calculation-compatibility marker (plus an environment fingerprint) that
discards cached column values when calculation semantics change, a
write-ordering scheme that makes it impossible for cached columns to disagree
with the saved session file after an interrupted save, and a per-column refresh
path so that an edit recomputes only the columns that can depend on it.

User-visible result: none, except that after the upgrade the logbook's
gyro-dependent columns repopulate with corrected values.

## Dependencies

- **Depends on:** Phase 4 (and 1-3).
- **Blocks:** Phase 6 (and 8). Phase 7 runs in parallel and owns
  `src/pluginhost.*`, `src/*_bindings.cpp`, `src/pluginsessionview.h`,
  `src/cpp_bridge.cpp`, `python_plugins/` - **do not touch those files**.
  Shared with Phase 7: `src/engine/calculationregistry.*` and
  `tests/tst_calcregistry.cpp` (Phase 7 Task 7.1 also edits them) - make this
  phase's edits there self-contained appended blocks.
- **Assumptions (names used verbatim from the earlier phase documents):**
  - Branch `schema-and-calculations`. Line numbers cited for
    `src/dataexporter.cpp`, `src/logbookmanager.cpp`, `src/dataimporter.cpp`,
    `src/mainwindow.cpp` are **baseline** (`git show v2026.04.1:<path>`); the
    working branch has drifted from them through Phases 1-4, so locate code by
    the quoted identifiers. `src/sessionmodel.cpp` / `.h`,
    `src/logbookcolumn.*`, `src/idlescheduler.*` are identical at the tag and
    on `master`; their line numbers are as at the tag (before Phase 3 edits).
  - `SessionData` (Phase 4 Task 4.3): `sourceData()`, `sourceMeasurement`,
    `sourceUnit`, `hasSourceMeasurement`, `storedAttribute`,
    `hasStoredAttribute`, `attributeKeys`, `sensorKeys`, `measurementKeys`,
    `setSourceMeasurement`, `mergeSourceData`, `calculationEngine()`. None of
    the source accessors creates or touches the engine. `getUnit` / `units`
    no longer exist.
  - The exporter already calls `sourceUnit` / `sourceMeasurement` at baseline
    lines 97, 121, 128 (Phase 4's "minimal exporter switch"); line 78 still
    calls `getAttribute(key).toString()`, rows still use `'g', 15`.
  - The importer (Phase 4 Task 4.5) parses into `StagedFile`, takes a `$VAR`
    value as **everything after the second comma, verbatim**, never trims
    values, skips (and counts) data rows with an unparsable field, publishes
    with `setAttribute` + `mergeSourceData`, clears `m_lastError` at entry, and
    rejects unsupported `SCHEMA_VER`. All `$VAR` values load as `QString`.
  - `CalculationRegistry` has `registeredIds()`, `isFamily()`,
    `candidatesFor()`, `sourceConversionsFor()`, `preferenceProvider()`,
    `notifyPreferenceChanged()`; registrations made at startup are complete
    before `LogbookManager::initialize()` is called (Phase 3 Task 3.9 startup
    order; baseline `mainwindow.cpp` 160-170).
  - `SessionModel` (Phase 3 Task 3.10) has `attachSession`,
    `queueInvalidation`, `publishInvalidation`, `flushPendingInvalidations`
    (which currently clears the row's `cachedValues` and calls
    `startColumnWorker()`), `m_invalidationFlushQueued`.
  - Test support: `TestEnvironment` (`useFreshLogbook`, `reopenLogbook`,
    `indexPath`, `sessionsDir`, `newTempDir`, `registerBuiltIns`,
    `resetPreferencesToDefaults`), `waitForIdle`, `LogbookManager::reset()`,
    `Fs2FileBuilder`, `Fixtures::sensorFile()` / `trackFile()`,
    `readFileBytes`, `writeFile`, `FLYSIGHT_TEST_MAIN`, `flysight_add_test`.
  - `master`'s `dataSchemaVersion` index field, `DataSchema` namespace, and
    schema stamping are **not** reused in any form.

## Findings that drive the design (read before the tasks)

1. **Baseline exporter** (`dataexporter.cpp`): `$VAR` values via
   `QVariant::toString()` (78) - six significant digits for doubles, so
   `_IMPORT_TIME` = 1718900000.123 is saved as `1.7189e+09`; rows via
   `QByteArray::number(v, 'g', 15)` (136) - not round-trip exact; row count
   taken from the first column only (121-122) and `columnData[c][i]` indexed
   without a bounds check (136) - a shorter later column is an out-of-range
   read; non-finite values are whatever `number()` prints; `$UNIT` is always
   written; ordering (`kAttributeOrder`, `kSensorOrder`, `kMeasurementOrder`,
   then `QMap` order) is deterministic.
2. **Time columns are already numeric on disk.** The importer converts a field
   ending in `Z` to `toMSecsSinceEpoch() / 1000.0` (baseline 337-347); the
   exporter has always written that double back as a number. Every released
   logbook file therefore stores `GNSS/time` as e.g. `1704110400.4`, never as
   ISO text. Nothing in the session records that a column was ISO in the
   original file.
3. **Qt 6.9.3 number formatting** (verified in
   `qtbase/src/corelib/text/qlocale_tools.cpp`):
   `QByteArray::number(v, 'g', QLocale::FloatingPointShortest)` uses
   double-conversion's `SHORTEST` mode - the shortest digit string that parses
   back to the same double - and chooses decimal or exponent form by length.
   Two traps: (a) the formatter deliberately drops the sign of negative zero
   (`if (negative && !qIsNull(d)) // We don't return "-0"`), so `-0.0` prints as
   `0`; (b) non-finite values print as `nan`, `inf`, `-inf`.
   `QStringView::toDouble` is correctly rounded, parses `-0` to negative zero,
   parses denormals such as `1e-320` with `ok == true`, accepts `nan`, `inf`,
   `+inf`, `-inf` case-insensitively, rejects `-nan` / `+nan`, and reports
   overflow (`1e999`) as `ok == false`.
4. **`std::to_chars(double)` is not usable.** CI builds with MSVC 14.3
   (`windows-2022`), GCC 11 (`ubuntu-22.04`), and Apple clang on `macos-15` /
   `macos-15-intel` with `MACOS_DEPLOYMENT_TARGET: '12.0'`
   (`.github/workflows/build.yml` 44-74). libc++ marks floating-point
   `to_chars` as introduced in macOS 13.3, so it does not compile against a
   12.0 deployment target. The project standard is C++17 (`src/CMakeLists.txt`
   20).
5. **Attribute types in memory.** After a load every attribute is a `QString`.
   After an import or edit some are not: `_IMPORT_TIME`, `_WIND_N`, `_WIND_E`,
   `_GROUND_ELEV` (fixed mode), marker times set through
   `SessionModel::updateAttribute` (`setexittool.cpp` 35, `PlotWidget.cpp`
   1569/1578, ...), WS-P / SP parameters are `double`; `_JUMPER_MASS` /
   `_PLANFORM_AREA` are whatever `PreferencesManager::getValue` returns
   (`double`, or `QString` when `QSettings` read it from an INI file);
   `_DESCRIPTION`, `SESSION_ID`, `DEVICE_ID` are `QString`. Calculated
   `_TIME_FIT_A/B` are 17-digit strings but are engine outputs, never stored,
   so they are never saved. No code stores a `QDateTime`. Because a reloaded
   value is a `QString` and strings are written verbatim, a lossless first
   write automatically makes the second save byte-identical.
6. **`LogbookManager::loadSession` backfill** (baseline 370-386) adds
   `_JUMPER_MASS`, `_PLANFORM_AREA` (from the **current** preferences) and
   `_WIND_N` / `_WIND_E` = `0.0` to the in-memory session when absent. It does
   not mark the row dirty, so the file changes only when something else
   triggers a save; that save then contains up to four additional `_` lines.
   It never touches header attributes, units, or samples. Sessions created
   through `DataImporter::importFile` already carry all four, so for them the
   backfill is a no-op. Policy is Phase 6's; this phase only makes its tests
   robust to it (Task 5.4).
7. **Loaded rows never display `cachedValues`.** `SessionModel::data`
   (313-341) formats loaded rows live from the session (engine) and only stub
   rows from `SessionRow::cachedValues`. For loaded rows `cachedValues` /
   `LogbookManager::m_cachedValues` exist solely to feed `index.json` and the
   post-eviction stub. Today every maintenance site recomputes the **whole
   row** with `computeColumnValues` (1448-1478): `rebuildColumns` 143,
   `mergeSessions` 573-577 (for *every* loaded row), `flushDirtySessions` 1060,
   `saveNextSession` 1093, `evictSession` 1191, `processNextDirtyColumn` 1251 /
   1262, bulk edit 1385 / 1415. With a warm engine that is cheap; with a cold
   engine (a session loaded from a stub, or the bulk-edit stub path) an edit of
   `_DESCRIPTION` re-runs the conversion, the time fit, and interpolation for
   every column.
8. **Interrupted-save hazard.** The session CSV (`saveNextSession` 1074-1098 ->
   `LogbookManager::saveSession`) and `index.json` (`flushIndex`, from the
   `SaveTask` / `ColumnTask` `onComplete` at 37-41 / 109-113) are separate
   atomic writes on different idle ticks. A crash between them leaves old
   column values next to a new CSV; conversely a `ColumnTask` flush while a row
   is still dirty can write **new** values next to an **old** CSV. Also
   `flushDirtySessions` (1065-1067) flushes the index only when a session was
   saved, and a brand-new session whose CSV was written but whose index entry
   was not is invisible on the next start because `initialize()` trusts the
   index exclusively once it has a `columns` object (161-202).

---

## Tasks

### Task 5.1: `CsvFormat` - the one place numbers and attribute values become text

**Purpose:** Give the exporter, the importer, and the cache fingerprint a single, tested definition of the on-disk text forms, including the exact-round-trip guarantee of spec 9.2.

**Files to create:**
- `src/csvformat.h` / `src/csvformat.cpp` (in `flysight_core`)
- `tests/tst_csvformat.cpp` (class `CsvFormatTest`)

**Files to modify:**
- `src/CMakeLists.txt` - add both files to `flysight_core`.
- `tests/CMakeLists.txt` - `flysight_add_test(tst_csvformat SOURCES tst_csvformat.cpp)`.

**Technical Approach:**

```cpp
namespace FlySight::CsvFormat {
// Shortest decimal text that parseDouble() maps back to the same bit pattern.
// Finite: QByteArray::number(v, 'g', QLocale::FloatingPointShortest), except negative zero -> "-0".
// Non-finite: exactly "nan", "inf", "-inf" (NaN sign and payload are not preserved).
QByteArray formatDouble(double v);

// Inverse used by the importer for every non-timestamp numeric field.
// Accepts the three canonical tokens by exact comparison first, then QStringView::toDouble.
bool parseDouble(QStringView text, double *out);

// Text written after "$VAR,<key>,". nullopt = the value cannot be represented (caller skips + warns).
std::optional<QString> formatAttributeValue(const QVariant &value);

// Replaces every "\r\n", '\r', '\n' (and U+2028 / U+2029) by one space. Nothing else changes.
QString singleLine(const QString &text);

// True iff text is usable as a $VAR key, sensor name, or column label:
// non-empty, contains no ',', '\r', '\n'.
bool isValidName(const QString &text);
// Same without the non-empty requirement (unit text may be empty).
bool isValidUnit(const QString &text);
}
```

- `formatDouble`: `if (v == 0.0 && std::signbit(v)) return "-0";` **before**
  calling Qt (finding 3a). `std::isnan` -> `"nan"`; `std::isinf` ->
  `v > 0 ? "inf" : "-inf"`. Do not rely on Qt's spelling for non-finite values;
  write the literals.
- `parseDouble`: `if (text == u"nan") ...quiet_NaN`, `u"inf"`, `u"-inf"`, else
  `text.toDouble(&ok)`. The explicit branch documents the contract and makes
  it independent of Qt; the fall-through keeps Phase 4's rule that whatever
  `toDouble` accepts today (`NaN`, `+inf`, ...) still loads. No trimming.
- `formatAttributeValue`, by `value.typeId()`:

  | Stored type | Text |
  |---|---|
  | invalid `QVariant` | `nullopt` (an invalid value means "unavailable"; `""` would reload as an available empty string) |
  | `QString` | `singleLine(value)` - otherwise **verbatim**: no trimming, no quoting, commas kept |
  | `Double`, `Float` | `formatDouble(value.toDouble())` (for `Float`, widen first; the text then round-trips the widened double) |
  | `Int`, `UInt`, `LongLong`, `ULongLong`, `Short`, `UShort`, `Long`, `ULong`, `Char`-family integers | `QString::number` of the `qlonglong` / `qulonglong` value (exact) |
  | `Bool` | `true` / `false` |
  | `QDateTime` | `toUTC().toString(Qt::ISODateWithMs)` - e.g. `2024-06-20T16:13:20.123Z` (this is where spec 9.2's "ISO timestamps are written at millisecond precision" applies; see Decisions) |
  | `QDate` / `QTime` | `Qt::ISODate` / `Qt::ISODateWithMs` |
  | `QByteArray` | `singleLine(QString::fromUtf8(...))` |
  | anything else | `value.canConvert<QString>()` ? `singleLine(value.toString())` : `nullopt` |

  Stability argument (write it in the header comment): every form above, read
  back by the importer, is a `QString` equal to the text written; a `QString`
  is written verbatim; therefore save -> load -> save is byte-identical for
  any attribute map, and `toDouble()` of the reloaded text returns the original
  bits for every numeric attribute.
- Commas: safe in values because of Phase 4's verbatim remainder rule. The
  symmetry to pin in tests is
  `importer("$VAR," + key + "," + formatAttributeValue(v)) == formatAttributeValue(v)`
  for values containing commas, leading/trailing spaces, `$`, `=`, and `""`.
- Line breaks cannot be represented in a line format. Escaping is rejected
  (a recorded header value containing a literal backslash sequence would be
  altered on read, violating "preserved exactly"); failing the save is rejected
  (loses the whole session for one odd character). Replacing by a space is the
  least harmful. To keep memory and disk equal in the normal path, Task 5.7
  applies `singleLine` at the model's text-edit entry points as well; the
  exporter's use is the last line of defence.

**Tests (`tst_csvformat`, literals only; "bits" = `std::memcmp` of the two doubles):**

| Test | Expected |
|---|---|
| `shortestForms` | `0.1` -> `"0.1"`; `0.3333333333333333` -> `"0.3333333333333333"`; `62.5` -> `"62.5"`; `-125.0` -> `"-125"`; `1718900000.123` -> `"1718900000.123"`; `1704110400.4` -> `"1704110400.4"`; `1e-320` -> `"1e-320"`; `4.9e-324` -> `"5e-324"`; `1.7976931348623157e308` -> `"1.7976931348623157e+308"`; `1e21` -> `"1e+21"`; `0.0` -> `"0"` |
| `negativeZero` | `formatDouble(-0.0) == "-0"`; `parseDouble("-0")` gives a value with `std::signbit` true and `== 0.0` |
| `roundTripBits` (data-driven) | for each literal above plus `9.80665`, `0.0001`, `1.14688`, `71.67999999999999`, `123456789012345680000.0`, `2.2250738585072014e-308`, `-1.5e-7`: `parseDouble(formatDouble(v))` has identical bits |
| `roundTripSweep` | `std::mt19937_64 rng(12345)`; 200 000 values made by `memcpy` of `rng()` into a double, skipping non-finite: identical bits. (The one test here where the expectation is the input itself rather than a literal - that is the property under test.) |
| `nonFinite` | `"nan"`, `"inf"`, `"-inf"` out; each parses back (`std::isnan` / `== +inf` / `== -inf`); `parseDouble("")`, `"abc"`, `"1e999"`, `"-nan"`, `"1,2"`, `" 1"` fail |
| `attributeForms` | `QVariant(1718900000.123)` -> `"1718900000.123"` (baseline produced `1.7189e+09`); `QVariant(0.0)` -> `"0"`; `QVariant(80)` -> `"80"`; `QVariant(true)` -> `"true"`; `QString("Perris, run 2, windy")` verbatim; `QString(" padded ")` verbatim; `QString("")` -> `""`; `QString("a\r\nb\nc")` -> `"a b c"`; `QDateTime(2024-06-20 16:13:20.123 UTC)` -> `"2024-06-20T16:13:20.123Z"`; the same instant given in UTC+02:00 -> the same text; invalid `QVariant` -> `nullopt`; `QVariant::fromValue(QPointF())`-style unconvertible type -> `nullopt` |
| `names` | `isValidName("IMU")`, `("bar#1")`, `("my col")` true; `("")`, `("a,b")`, `("a\nb")` false; `isValidUnit("")`, `("deg C")` true, `("m,s")` false |

**Acceptance Criteria:**
- [ ] `git grep -n "'g', *15\|'g', *17\|setRealNumberPrecision" -- src/dataexporter.cpp src/csvformat.cpp` returns nothing after Task 5.3; `FloatingPointShortest` appears in `src` only in `csvformat.cpp`.
- [ ] `csvformat.*` includes Qt Core only (no `SessionData`, engine, or preferences include).
- [ ] `tst_csvformat` passes on Windows Release, including the 200 000-value sweep and the negative-zero test.
- [ ] No `<charconv>` include exists in `src/`.

**Complexity:** M

---

### Task 5.2: Importer accepts the canonical non-finite tokens through `CsvFormat::parseDouble`

**Purpose:** Make "a saved session with a NaN sample reloads with the NaN in place" a documented contract instead of an accident of `toDouble`.

**Files to modify:**
- `src/dataimporter.cpp` - the numeric branch of the data-row parser (baseline 349-357; after Phase 4 it lives in the staged row parser) and the FS1 row path, which shares it.
- `src/dataimporter.h` - contract comment.
- `tests/tst_importer.cpp` - add `nonFiniteTokens`.

**Technical Approach:**

- Replace `dataValueView.toDouble(&ok)` with
  `CsvFormat::parseDouble(dataValueView, &val)`. Nothing else in the row
  policy changes: empty field, bad `Z` timestamp, or a field `parseDouble`
  rejects still skips the whole row and increments `skippedRows`.
- The `Z` test stays first; none of the three tokens ends in `Z`.
- Header comment to add: "Numeric fields: `nan`, `inf`, `-inf` are non-finite
  samples (written by `DataExporter` for non-finite source values); every other
  field is parsed with `QStringView::toDouble` (correctly rounded). Timestamps
  ending in `Z` become seconds since the epoch with millisecond precision."
- `$VAR` values are not parsed at all (they stay `QString`), so a non-finite
  attribute such as `_GROUND_ELEV` = `nan` reloads as the string `"nan"`, whose
  `toDouble()` is NaN - the same contract with no importer code.

**Test `nonFiniteTokens`:** sensor fixture plus rows `$IMU,4,nan,1,0,62.5,40`,
`$IMU,5,-125,inf,0,62.5,40`, `$IMU,6,-125,1,-inf,62.5,40` -> `importFile` true;
IMU columns have 4 samples; `std::isnan(sourceMeasurement("IMU","wy")[1])`;
`ax[2] == +inf`; `wz[3] == -inf`; zero "skipped" warnings.

**Acceptance Criteria:**
- [ ] `git grep -n "toDouble" -- src/dataimporter.cpp` shows no hit in the data-row path (only, if at all, in `extractDeviceId`-style helpers).
- [ ] `tst_importer` passes with the new test; every Phase 4 importer test passes unchanged.

**Complexity:** S

---

### Task 5.3: Exporter rewrite - source layer only, exact numbers, explicit validation

**Purpose:** Implement spec 9.1 / 9.2: the saved file is a function of the source layer and the stored attribute map and nothing else.

**Files to modify:**
- `src/dataexporter.h` / `src/dataexporter.cpp`
- `src/logbookmanager.cpp` - `saveSession` (baseline 395-426) and `scanSessionFiles` (455): read `SESSION_ID` with `storedAttribute`, pass the error string through.

**Technical Approach:**

```cpp
class DataExporter {
public:
    // Writes sessionData in FlySight 2 CSV form. Reads ONLY sourceData() and stored attributes;
    // never calls getAttribute/getMeasurement/effectiveUnit/calculationEngine().
    // On failure nothing is written (QSaveFile) and *error (if given) names the reason.
    static bool exportSession(const QString &filePath, const SessionData &sessionData,
                              QString *error = nullptr);
    // Same bytes into memory; exportSession = validate + toBytes + QSaveFile. Used by tests.
    static std::optional<QByteArray> toBytes(const SessionData &sessionData, QString *error = nullptr);
};
```

Keep `kAttributeOrder`, `kSensorOrder`, `kMeasurementOrder`, and `reorder()`
(baseline 12-54) unchanged: output order is part of the byte-stability
guarantee.

1. **Snapshot.** `const SourceData data = sessionData.sourceData();`
   (implicitly shared, no sample copied) and `sessionData.attributeKeys()`.
2. **Validate before opening the file** (errors are returned, nothing is
   written, the previous file on disk stays intact):
   - every sensor name and column label satisfies `CsvFormat::isValidName`,
     every unit `isValidUnit` - else
     `"Sensor '<s>': name/column/unit text cannot be written ('<text>')"`;
   - **ragged sensors:** all columns of a sensor must have the same length -
     else `"Sensor '<s>' has columns of unequal length (<c1>: <n1>, <c2>: <n2>)"`
     naming the first column and the first column that differs. All-empty
     sensors (header-only recordings) are valid and produce header lines and no
     rows. See Decisions for why this is an error rather than padding.
3. **Header.** `$FLYS,1`. Then for each key in
   `reorder(attributeKeys(), kAttributeOrder)`:
   `text = CsvFormat::formatAttributeValue(sessionData.storedAttribute(key))`.
   - key fails `isValidName`, or `text` is `nullopt`: **skip the attribute**
     and `qWarning("DataExporter: attribute '%s' cannot be written (%s)")`.
     This cannot happen for anything the importer or the UI produces (keys
     come from tokenized lines; values are strings and numbers); it is not
     worth failing the save of a whole session for.
   - otherwise `"$VAR," + key + "," + *text + "\n"`.
   This writes header attributes exactly as recorded - unknown keys included,
   `SCHEMA_VER` if and only if it is stored, never a default - and every `_`
   attribute. The exporter contains no attribute key literal other than
   `kAttributeOrder`.
4. **`$COL` / `$UNIT`** per sensor from `data` (labels and unit text verbatim;
   `$UNIT` always written, as at baseline). `$DATA`.
5. **Rows.** Per sensor, `n` = the validated common length; gather
   `const double *` pointers via `constData()`; per value
   `buf.append(CsvFormat::formatDouble(p[c][i]))`. Keep the 4 MB periodic
   flush (baseline 140-144). Time columns are written as the numeric seconds
   they are stored as (finding 2; Decisions).
6. Write the header through the same `QByteArray` path (UTF-8 via
   `QString::toUtf8()`); drop `QTextStream`, `setRealNumberPrecision(15)` and
   `SmartNotation` (baseline 66-70) - nothing may format a number except
   `CsvFormat`. No BOM, `\n` line endings, as today.
7. `LogbookManager::saveSession`: `session.storedAttribute(SessionKeys::SessionId)`
   instead of `getAttribute` (397) so that a save never creates or consults an
   engine; keep the exporter's error in `m_lastSaveError` (`QString lastSaveError() const`)
   and `qWarning` it. Same substitution at `scanSessionFiles` 455 (hundreds of
   temporary sessions; Phase 4 gotcha "source accessors must not create the engine").

**Acceptance Criteria:**
- [ ] `git grep -nE "getAttribute|getMeasurement|effectiveUnit|calculationEngine|QTextStream" -- src/dataexporter.cpp` returns nothing.
- [ ] Exporting a session whose engine was never created leaves it without an engine-visible effect: afterwards `calculationEngine().totalRunCount() == 0` and `cachedNodeCount() == 0`; exporting a warm session leaves `totalRunCount()` and `cachedNodeCount()` unchanged.
- [ ] A ragged sensor, a column label containing `,`, and a unit containing a line break each make `exportSession` return `false` with the specified message, and a pre-existing file at the target path is byte-for-byte unchanged.
- [ ] A saved file never contains `SCHEMA_VER` unless `hasStoredAttribute("SCHEMA_VER")`.
- [ ] `tst_smoke` and `tst_source_layer::exporterWritesSource` still pass.

**Complexity:** M

---

### Task 5.4: Round-trip acceptance suite

**Purpose:** Demonstrate acceptance 5 and 6 and the non-finite / ragged / unrepresentable-text rules on the real importer, exporter, and logbook.

**Files to create:**
- `tests/tst_persistence_roundtrip.cpp` (class `PersistenceRoundTripTest`)

**Files to modify:**
- `tests/CMakeLists.txt`; `tests/tst_smoke.cpp` (`exportReloadRoundTrip`: tighten "source samples equal within 1e-12" to bit equality; nothing else).

**Technical Approach:**

`initTestCase`: `registerBuiltIns()`. `init`: `useFreshLogbook()`,
`resetPreferencesToDefaults()`. Helpers local to the file:
`bitsEqual(const QVector<double>&, std::initializer_list<double>)`
(`memcmp` per element), `exportBytes(session)` (`DataExporter::toBytes`),
`reload(bytes)` (write to `newTempDir()`, `DataImporter::readFile` into a fresh
`SessionData`). **Exporter-level tests use `readFile`, never
`LogbookManager::loadSession`, so the backfill of finding 6 cannot interfere;
logbook-level tests use fixtures that already contain `_JUMPER_MASS`,
`_PLANFORM_AREA`, `_WIND_N`, `_WIND_E`.**

The "awkward" fixture `awkwardFile()` (an `Fs2FileBuilder`; values are text):
`$VAR` in this order: `FIRMWARE_VER=v2023.09.22`, `DEVICE_ID=test-device`,
`SESSION_ID=awkward`, `CUSTOM_KEY=a=b, with, commas`, `EMPTY_VAL=` (empty),
`_DESCRIPTION=Perris, run 2, windy`; sensor `GNSS` columns `time,lat,hMSL`
units `,deg,m` with rows `2024-06-20T16:13:20.123Z,45.1234567,0.1` and
`2024-06-20T16:13:20.323Z,-0,1e-320`; sensor `IMU` columns `time,wx,ax` units
`s,deg/s,g`, rows `3,62.5,0.3333333333333333` and
`4,-125,1.7976931348623157e+308`; custom sensor `FOO` columns `time,bar#1`
units `s,furlongs`, row `3,7`. No `SCHEMA_VER`.

| Test | Scenario | Expected |
|---|---|---|
| `samplesAreBitIdentical` (acc. 5) | import `awkwardFile()` (A); `exportBytes(A)`; `reload` -> B | every column of `B.sourceData()` `bitsEqual` to A's **and** to literals: `GNSS/time` {`1718900000.123`, `1718900000.323`}; `GNSS/lat` {`45.1234567`, `-0.0` with `std::signbit`}; `GNSS/hMSL` {`0.1`, `1e-320`}; `IMU/ax` {`0.3333333333333333`, `1.7976931348623157e308`}; `IMU/wx` {`62.5`, `-125.0`}; `A.sourceData() == B.sourceData()` |
| `unitsAndHeaderAttributesPreserved` (acc. 5) | same | `sourceUnit` `""`, `deg`, `m`, `s`, `deg/s`, `g`, `furlongs` on B; `B.storedAttribute` of `FIRMWARE_VER`, `DEVICE_ID`, `SESSION_ID`, `CUSTOM_KEY` (`"a=b, with, commas"`), `EMPTY_VAL` (`""`, and `hasAttribute` true), `_DESCRIPTION` (`"Perris, run 2, windy"`) equal the literals; `B.attributeKeys() == A.attributeKeys()` |
| `schemaVerOnlyIfRecorded` (acc. 5) | awkward file as is; then with `.var("SCHEMA_VER","2")` | first: bytes contain no `SCHEMA_VER`, `B.hasAttribute` false **after** reading every effective column of A before export; second: exactly one line `$VAR,SCHEMA_VER,2`, reloaded value `"2"` |
| `effectiveValuesUnchanged` (acc. 5) | unmarked awkward file | effective `IMU/wx` of A and of B both near `71.68` / `-143.36` (1e-9) and bit-equal to each other; `IMU/ax[0]` bit-equal between A and B; `effectiveUnit` equal; `B.calculationEngine().verifyAgainstFresh(...)` empty |
| `secondCycleIsByteIdentical` (acc. 5) | `b1 = exportBytes(A)`; `b2 = exportBytes(reload(b1))`; `b3 = exportBytes(reload(b2))` | `b1 == b2 == b3` |
| `canonicalInputIsReproduced` | a fixture written only with canonical forms (numeric time, `$VAR` in exporter order, `$UNIT` present, standard column order) | `exportBytes(import(F)) == F` as a literal `QByteArray` |
| `typedAttributesRoundTrip` (acc. 5) | on A: `setAttribute("_IMPORT_TIME", 1718900000.123)`, `("_GROUND_ELEV", 123.456)`, `("_WIND_N", 0.0)`, `("_EXIT_TIME", 1718900012.5)`, `("_JUMPER_MASS", 80)`, `("_FLAG", true)`, `("_WHEN", QDateTime 2024-06-20T16:13:20.123Z)` | file lines `$VAR,_IMPORT_TIME,1718900000.123`, `$VAR,_GROUND_ELEV,123.456`, `$VAR,_WIND_N,0`, `$VAR,_EXIT_TIME,1718900012.5`, `$VAR,_JUMPER_MASS,80`, `$VAR,_FLAG,true`, `$VAR,_WHEN,2024-06-20T16:13:20.123Z`; reloaded `.toDouble()` bit-equal to `1718900000.123` / `123.456`; `GNSS/z`-style effective reads that use `_GROUND_ELEV` equal before and after; second cycle byte-identical |
| `warmAndColdCachesSameFile` (acc. 5) | import twice (A cold, A2); on A2 read **every** effective measurement, `effectiveUnit`, `IMU/wTotal`, `_START_TIME`, `_EXIT_TIME`, an interpolation key; record `runs = totalRunCount()`, `nodes = cachedNodeCount()` | `exportBytes(A) == exportBytes(A2)`; A2's counters still `runs` / `nodes`; A: `totalRunCount() == 0`, `cachedNodeCount() == 0`; bytes contain `62.5` and `,g` and neither `71.6` nor `9.80665` |
| `nonFiniteRoundTrip` | programmatic: `setSourceMeasurement("X","v",{1.0, NaN, +inf, -inf, 2.0},"u")`, `"X","time",{0,1,2,3,4},"s"`; `setAttribute("_GROUND_ELEV", NaN)` | rows `$X,1,nan`, `$X,2,inf`, `$X,3,-inf` present; bytes contain no `,0\n` in place of them; reload: 5 samples, `isnan([1])`, `[2] == +inf`, `[3] == -inf`, `[0]`/`[4]` exact; attribute text `"nan"`, `std::isnan(toDouble())`; second cycle byte-identical |
| `raggedSensorIsRejected` | `X/time` 3 samples, `X/v` 2 samples; target path pre-populated with bytes `"keep"` | `exportSession` false; error == `"Sensor 'X' has columns of unequal length (time: 3, v: 2)"`; file still `"keep"`; no crash under the address sanitizer / debug iterator checks |
| `headerOnlySensor` | `$COL` + `$DATA`, no rows | export succeeds; reload has the columns, all empty; second cycle identical |
| `unrepresentableText` | `setAttribute("_DESCRIPTION","a\nb")`; `setAttribute("BAD,KEY","x")`; column label `"a,b"` | description written as `a b`; `BAD,KEY` skipped with exactly one warning and the export succeeds; the bad label makes the export fail with the Task 5.3 message |
| `releasedLogbookSaveKeepsBytes` (acc. 6) | literal released-format file R (exporter order): `$FLYS,1`; `$VAR` `FIRMWARE_VER`, `DEVICE_ID`, `SESSION_ID=rel`, `_DESCRIPTION=old jump`, `_IMPORT_TIME=1.7189e+09` (the lossy text released versions wrote), `_JUMPER_MASS=80`, `_PLANFORM_AREA=2`, `_WIND_E=0`, `_WIND_N=0`; `IMU` `time,wx,wy,wz,ax,temperature` units `s,deg/s,deg/s,deg/s,m/s^2,degC` row `3,62.5,-125,0,9.80665,40`; `MAG` `time,x` units `s,T` row `3,0.0001`. Install it as in `tst_source_layer::releasedLogbookFormatLoads`; `loadSession("rel")`; read effective gyro; `saveSession(loaded)` | effective `wx` near `71.68` (corrected once); saved file bytes `== R`: no rescale (`62.5`, `9.80665`, `0.0001`), no relabel (`m/s^2`, `degC`, `T`), no `SCHEMA_VER`, `_IMPORT_TIME` still `1.7189e+09`; reload again -> `wx` still near `71.68` (not `82.2`) |
| `releasedLogbookBackfillIsAdditive` (acc. 6 / finding 6) | same R without the four `_JUMPER_MASS` / `_PLANFORM_AREA` / `_WIND_*` lines; preferences `AeroMass` = 1.0, `AeroArea` = 1.0 | after `loadSession` + `saveSession`: the file differs from R **only** by added lines `$VAR,_JUMPER_MASS,1`, `$VAR,_PLANFORM_AREA,1`, `$VAR,_WIND_E,0`, `$VAR,_WIND_N,0` (compare the line sets); every non-`_` line and every `$COL` / `$UNIT` / data line is identical; a further load + save is byte-identical. Comment: `// Phase 6 owns the backfill policy; this pins that it is additive and idempotent.` |
| `logbookSaveReloadCycle` (acc. 5) | `importFile` awkward fixture (so creation defaults exist); `saveSession`; `flushIndex`; `reopenLogbook`; `initialize`; `loadSession`; `saveSession` | the session file's bytes before and after the second save are identical; `sourceData()` equal |

**Acceptance Criteria:**
- [ ] The target passes on Windows Release; acceptance numbers 5 and 6 appear in comments on the functions above.
- [ ] Every expected number, line, and file is a literal; the only computed comparisons are A-versus-B equality (which acceptance 5 asks for) and `verifyAgainstFresh`.
- [ ] Temporarily restoring `'g', 15` in `CsvFormat::formatDouble` makes `samplesAreBitIdentical` fail on `0.3333333333333333`; temporarily removing the negative-zero branch makes it fail on `-0` - checked once by hand, not committed.
- [ ] Nothing is written outside `TestEnvironment::rootPath()`.

**Complexity:** L

---

### Task 5.5: Calculation-compatibility marker, environment fingerprint, and registry queries

**Purpose:** Give cache validity an owner in the calculation code, and give the model two registration-derived facts it needs: which columns *can* depend on a changed key, and when the calculation environment changed.

**Files to modify:**
- `src/engine/calculationregistry.h` / `.cpp` (in `flysight_model`, Qt Core only)
- `src/calculations/builtincalculations.h` / `.cpp`
- `tests/tst_calcregistry.cpp`, `tests/tst_builtins_engine.cpp`

**Technical Approach:**

*Registry additions* (all pure functions of the registrations; none touches an engine or runs a calculation):

```cpp
struct StaticDependencies {
    QSet<DependencyKey> names;        // every public name reachable through declared inputs, INCLUDING the queried name
    QSet<QString>       preferences;  // every declared preference key reachable
};
StaticDependencies staticDependencies(const DependencyKey &name) const;
QStringList declaredPreferenceKeys() const;          // sorted, unique; plain calculations only
int  addObserver(std::function<void()> observer);    // called after every successful register*/unregister,
void removeObserver(int token);                      //   after the enrolled engines were notified
```

- `staticDependencies`: worklist over public names with a visited set. For
  each name `N`: insert `N`; take `candidatesFor(N)` and, when `N` is a
  measurement, also `sourceConversionsFor(sensor, name)`; for **every**
  candidate (not only the one that would win) walk `descriptor->inputs`:
  `Attribute` / `Measurement` -> enqueue that name; `Preference` -> insert the
  key; `SourceMeasurement` / `SourceUnit` -> insert
  `DependencyKey::measurement(sensor, name)`. The result is a superset of any
  dynamic dependency set the engine can record for `N`, independent of session
  state and of what is cached - which is exactly what makes it safe for rows
  whose engine is cold or absent. Memoize per name; clear the memo in every
  successful `register*` / `unregister`.
  Example: for the column name `_M:IMU/_time/wx` the set is `_M`,
  `IMU/_time`, `IMU/time`, `_TIME_FIT_A`, `_TIME_FIT_B`, `TIME/time`,
  `TIME/tow`, `TIME/week`, `IMU/wx`, `SCHEMA_VER`, and the name itself;
  it does not contain `_DESCRIPTION`.
- `declaredPreferenceKeys`: union of `Preference` inputs over plain
  registrations (today exactly `import/descentPauseSeconds`). Family instances
  are not enumerable; no family declares a preference, and the header comment
  must say that one which does has to be added here explicitly.
- Observers are plain callbacks (no `QObject` in `flysight_model`), invoked
  synchronously; an observer must not register or unregister.

*Marker* (in `src/calculations/builtincalculations.h`, next to `registerBuiltInCalculations`):

```cpp
namespace FlySight {
// Bump whenever a code change can alter the value that ANY existing session yields for ANY
// logbook column: a built-in calculation's arithmetic, inputs, or candidate order; the schema
// table or the unit-normalization table (src/conversion, src/units/unitconversion.h); the
// interpolation family; SessionModel::computeColumnValues (what a column stores, or its unit).
// Do not bump for pure additions/removals/renames of registrations - the environment
// fingerprint below already covers those. Never reuse a value; never set it to 0
// (0 is what an index without the field reads as). Not related to SCHEMA_VER.
constexpr int CalculationCompatibilityVersion = 1;

// SHA-1 (lower-case hex) over, in this order:
//   "id:<id>\n"            for every id in registry.registeredIds()            (registration order)
//   "pref:<key>=<text>\n"  for every key in registry.declaredPreferenceKeys()  (sorted),
//        text = CsvFormat::formatAttributeValue(provider value).value_or(QString())
QString calculationEnvironmentFingerprint(const CalculationRegistry &registry = CalculationRegistry::instance());
}
```

The initial value `1` means every released `index.json` (no field -> `toInt()`
== 0) is invalidated on upgrade, which is what spec 9.4 requires for the gyro
correction introduced in Phase 4.

**Tests:** `tst_calcregistry`: `staticDependenciesClosure` (shared synthetic
world of Phase 2 Task 2.10: `staticDependencies(W).names` ==
{`W`,`X`,`Z`,`A`,`B`,`C`} and `.preferences` == {`p`} - `Z` comes from the
losing candidate `wAlt`, `C` from the losing candidate `fallbackX`; after
`unregister("wAlt")` the memo is dropped and the set is {`W`,`X`,`A`,`B`,`C`};
`staticDependencies(X2)` terminates on the `P/Q/R/S` ring with {`X2`,`Y2`});
`declaredPreferenceKeys` == {`p`};
`observersFire` (one call per successful register / unregister, none for a
rejected registration, none after `removeObserver`).
`tst_builtins_engine`: `gyroColumnClosure` - the `_M:IMU/_time/wx` example
above as a literal set, and `staticDependencies(attribute("_DESCRIPTION")).names
== {_DESCRIPTION}`; `fingerprintChanges` - equal for two registries built the
same way; differs after registering one extra calculation, after swapping two
registrations, and after `FakePreferenceProvider::set` of
`import/descentPauseSeconds` from 30.0 to 5.0; equal again after setting it
back; a 40-character lower-case hex string.

**Acceptance Criteria:**
- [ ] `flysight_model` still links Qt Core only; the fingerprint and the constant live in `flysight_core`.
- [ ] `CalculationCompatibilityVersion` is defined once; `git grep -n "dataSchemaVersion\|DataSchema" -- src tests` returns nothing.
- [ ] None of the new registry functions calls a `compute` function or touches an engine (run counters and `FakeSessionState::readCount()` unchanged in the tests).

**Complexity:** M

---

### Task 5.6: `LogbookManager` - marker-gated cache, unsaved-column tracking, save ordering, orphan adoption

**Purpose:** Implement spec 9.4 at the storage level: discard stale cached values on upgrade, and guarantee by construction that `index.json` on disk never holds a column value that disagrees with the session file on disk.

**Files to modify:**
- `src/logbookmanager.h` / `src/logbookmanager.cpp`
- `tests/CMakeLists.txt`

**Files to create:**
- `tests/tst_logbook_index.cpp` (class `LogbookIndexTest`)

**Technical Approach:**

`index.json` root after this phase (field order is `QJsonObject`'s, i.e. alphabetical):

```json
{ "calculationCompatibility": 1,
  "calculationEnvironment": "3f7a...40 hex...",
  "columns":  { ... unchanged ... },
  "sessions": { "<SESSION_ID>": { "uuid": "...", "lastAccessed": 0, "values": { ... } } } }
```

New public API:

```cpp
bool    flushIndex();                                  // was void; true on successful commit
QString cacheEnvironment() const;                      // fingerprint the in-memory values are valid for
bool    cachedValuesDiscardedOnLoad() const;           // initialize() found a missing/different marker or environment
void    discardCachedValues();                         // every session; sets cacheEnvironment() to the current fingerprint
bool    indexNeedsFlush() const;
void    updateCachedValues(const QString &sessionId, const QMap<LogbookColumn, QVariant> &columnValues); // MERGES (setCachedValues replaces)
void    markColumnsUnsaved(const QString &sessionId, const QVector<LogbookColumn> &columns);
void    markSessionUnsaved(const QString &sessionId);  // all columns, present and future
bool    hasUnsavedColumns(const QString &sessionId) const;
QString lastSaveError() const;
```

New members: `QString m_cacheEnvironment; bool m_discardedOnLoad = false; bool m_indexNeedsFlush = false; QMap<QString, QSet<QString>> m_unsavedColumns; QSet<QString> m_unsavedAll; QSet<QString> m_needsFlushBeforeSave; QString m_lastSaveError;`
- all cleared by `reset()`; `remapSessionId` (baseline 492-512) moves the three
  per-session entries with the id; `removeSession` (518-537) erases them.

1. **`initialize()`** (baseline 148-223). In the extended-format branch
   compute `valid = root["calculationCompatibility"].toInt() == CalculationCompatibilityVersion
   && root["calculationEnvironment"].toString() == calculationEnvironmentFingerprint()`.
   Always read `uuid` and `lastAccessed`. Read `values` (188-198) **only if
   `valid`**; otherwise set `m_discardedOnLoad = m_indexNeedsFlush = true`.
   In every branch finish with `m_cacheEnvironment = calculationEnvironmentFingerprint()`.
   Session files are never touched. Recomputation needs no new code:
   `populateFromIndex` creates rows with empty `cachedValues`, and the existing
   `startColumnWorker()` call (baseline `mainwindow.cpp` 174) drives
   `processNextDirtyColumn`, whose `ColumnTask` completion flushes the index
   with the current marker. A crash before that flush just repeats the discard.
2. **Orphan adoption** (same branch, after the sessions loop): list
   `scanSessionFilenames()`-style `*.csv` stems **without** inserting identity
   mappings for known ones; every stem that is not a value of
   `m_sessionIdToUuid` is added as `m_sessionIdToUuid[stem] = stem` (the
   identity-stub convention of 471-486, resolved to the real `SESSION_ID` by
   the existing remap at `sessionmodel.cpp` 821-826 / 1257-1261) and
   `m_indexNeedsFlush = true`. This recovers a new session whose CSV was
   committed but whose index entry was not. Refactor `scanSessionFilenames` so
   the listing is shared.
3. **`flushIndex()`** (543-601): write the two root fields -
   `CalculationCompatibilityVersion` and **`m_cacheEnvironment`** (the
   fingerprint the in-memory values were computed under, *not* a freshly
   computed one: if nobody told the manager about an environment change, the
   next start detects the mismatch). When building a session's `values`
   (569-579): none if the id is in `m_unsavedAll`; otherwise skip every
   definition key in `m_unsavedColumns[id]`. On successful `commit()`:
   `m_needsFlushBeforeSave.clear(); m_indexNeedsFlush = false;` return true.
4. **`markColumnsUnsaved(id, columns)`**: for each column remove
   `m_cachedValues[id][columnDefinitionKey(col)]` and insert the key into
   `m_unsavedColumns[id]`; if `m_sessionIdToUuid.contains(id)` insert `id` into
   `m_needsFlushBeforeSave`; `m_indexNeedsFlush = true`.
   `markSessionUnsaved(id)`: `m_cachedValues.remove(id)`, insert into
   `m_unsavedAll`, same flush bookkeeping. Meaning of "unsaved": *the value of
   this column for the in-memory session may differ from its value for the
   session file on disk.* Values may be put back with `updateCachedValues`
   while the mark is set (memory always reflects the in-memory session); the
   mark only controls what `flushIndex` writes.
5. **`saveSession()`** (395-426), in this order:
   a. `SESSION_ID` via `storedAttribute` (Task 5.3).
   b. if `m_needsFlushBeforeSave.contains(id)`: `if (!flushIndex()) { m_lastSaveError = "index.json could not be written"; return false; }`
      - the on-disk index now holds no value for any column this save can change.
   c. `DataExporter::exportSession(path, session, &m_lastSaveError)`; on failure
      `qWarning` and return false **leaving the unsaved marks in place**.
   d. on success: uuid bookkeeping as today, then
      `m_unsavedColumns.remove(id); m_unsavedAll.remove(id); m_indexNeedsFlush = true;`
6. `setCachedValues` (298-315) and the new `updateCachedValues` share the
   `QVariant` -> `QJsonValue` conversion; both set `m_indexNeedsFlush`.
   `discardCachedValues()`: `m_cachedValues.clear()`, recompute
   `m_cacheEnvironment`, `m_indexNeedsFlush = true` (unsaved marks are
   unaffected - they concern persistence, not calculation semantics).

Crash analysis to put in a comment above `saveSession` (D = on-disk index values for the affected columns, F = session file):

| Crash point | D | F | Consistent because |
|---|---|---|---|
| after the edit, before any write | old | old | nothing changed on disk |
| after step b | absent | old | absent values are recomputed from F |
| after step c | absent | new | same |
| after a `ColumnTask` / `SaveTask` flush while the row is still unsaved | absent (skipped by rule 3) | old | same |
| after the post-save flush | new | new | computed from the state that was saved |

Columns *not* marked keep their values throughout: by Task 5.5's closure they
cannot depend on the change, so one value is right for both the old and the
new file.

**Tests (`tst_logbook_index`; `registerBuiltIns()` in `initTestCase`; each test starts with `useFreshLogbook()` + `initialize()`; JSON is inspected with `QJsonDocument` on `readFileBytes(indexPath())`; columns are set with `LogbookColumnStore::instance().setColumns`):**

| Test | Scenario | Expected |
|---|---|---|
| `markerWrittenOnFlush` | save one session, `flushIndex()` | root `calculationCompatibility` is the JSON number `1`; `calculationEnvironment` is a 40-char hex string equal to `calculationEnvironmentFingerprint()`; no `dataSchemaVersion` key |
| `missingMarkerDiscardsValues` (acc. 18) | columns D = `SessionAttribute _DESCRIPTION`, G = `MeasurementAtMarker IMU/wx @ _M`; `setCachedValues(id, {D:"x", G:1.5})`; `setLastAccessed(id, 1234.0)`; flush; rewrite `index.json` with the `calculationCompatibility` key removed (as a released version wrote it); `reopenLogbook()`; `initialize()` | `cachedValuesDiscardedOnLoad()`; `cachedValuesForSession(id)` empty; `lastAccessedMap()[id] == 1234.0`; `loadSession(id)` succeeds (uuid kept); the session CSV bytes unchanged; `indexNeedsFlush()` |
| `differentMarkerDiscards` (data rows: `0`, `2`, `-1`, `"1"` as a JSON string) | same with the field rewritten | discarded |
| `differentEnvironmentDiscards` | field `calculationEnvironment` rewritten to 40 zeros | discarded |
| `matchingMarkerKeepsValues` | unmodified index | not discarded; `cachedValuesForSession(id)` has `1.5` for G (proves gating rather than unconditional recomputation) |
| `environmentIsTheCachedOne` | initialize; register an extra calculation on the global registry (unregister in `cleanup()`); `flushIndex()` without `discardCachedValues()` | written fingerprint is the **old** one; after `reopenLogbook()` + `initialize()` values are discarded. With `discardCachedValues()` before the flush: the new fingerprint is written and nothing is discarded on reopen |
| `unsavedColumnsAreNotFlushed` | values D, G cached and flushed; `markColumnsUnsaved(id,{D})`; `updateCachedValues(id,{D:"new"})`; `flushIndex()` | on disk: G present (`1.5`), D **absent**; in memory `cachedValuesForSession(id)` has D = `"new"` |
| `saveFlushesIndexFirst` | as above up to `markColumnsUnsaved`; then `saveSession(edited)`; **no** further flush (simulated crash) | index on disk lacks D, keeps G; CSV contains the new description; `hasUnsavedColumns(id)` false |
| `interruptedSaveNeverDisagrees` | continue: `reopenLogbook()`; `initialize()` | `cachedValuesForSession(id)` has G only; no value for D exists anywhere that differs from the CSV |
| `failedSaveKeepsMarks` | `markSessionUnsaved(id)`; session made ragged; `saveSession` | false; `lastSaveError()` names the sensor; `hasUnsavedColumns(id)`; a later `flushIndex()` writes `values: {}` for `id`; previous CSV bytes unchanged |
| `orphanSessionFileIsAdopted` | save sessions `a`, `b`; flush; save session `c` **without** flushing; `reopenLogbook()`; `initialize()` | three entries: `a`, `b`, and an identity entry keyed by `c`'s file stem; `loadSession(<stem>)` returns the session whose `SESSION_ID` is `c`; after `remapSessionId(<stem>, "c")` + flush + reopen there are exactly `a`, `b`, `c` |
| `remapAndRemoveCarryMarks` | mark, remap, check `hasUnsavedColumns(newId)`; remove, check gone | as stated |

**Acceptance Criteria:**
- [ ] `tst_logbook_index` passes on Windows Release; acceptance 18 appears in a comment.
- [ ] `git grep -n "exportSession" -- src` shows `saveSession` as the only caller in `src`, so no code path can write a session file without the pre-save flush.
- [ ] `initialize()` on a released-format index performs no file write and does not open any session CSV.
- [ ] `reset()` clears every new member (verified by `useFreshLogbook()` isolation between the tests above).

**Complexity:** L (natural split: marker + environment + discard; unsaved tracking + save ordering; orphan adoption; tests)

---

### Task 5.7: `SessionModel` - per-column refresh, environment changes, save failures

**Purpose:** Make "a session edit refreshes only the affected columns" (acceptance 18) true and testable, keep cached columns of **unloaded** sessions correct when a declared preference or the registration set changes (Phase 3's open question), and wire every mutation into Task 5.6's unsaved marks.

**Files to modify:**
- `src/sessionmodel.h` / `src/sessionmodel.cpp`
- `tests/CMakeLists.txt`

**Files to create:**
- `tests/tst_column_cache.cpp` (class `ColumnCacheTest`)

**Technical Approach:**

Invariant the model maintains from now on: **a value present in
`SessionRow::cachedValues` / `LogbookManager::m_cachedValues` is the column's
value for the row's current in-memory (or, for a stub, on-disk) state; a value
that may no longer hold is removed, never left stale.** "Missing" is already
what `ColumnTask::hasWork` (104-107) and `startColumnWorker` (1205-1222) look
for, so no new scheduling is needed.

New members / functions:

```cpp
public:
    struct ColumnWorkStats { int valuesComputed = 0; int sessionsLoaded = 0; int calculationRuns = 0; };
    const ColumnWorkStats &columnWorkStats() const;   void resetColumnWorkStats();
    ~SessionModel() override;                                   // removes the registry observer
private:
    QVector<StaticDependencies> m_columnDependencies;           // parallel to m_columns
    void rebuildColumnDependencies();                           // union of staticDependencies() over columnNames(col)
    static QList<DependencyKey> columnNames(const LogbookColumn &col);
    void invalidateColumns(int row, const QSet<DependencyKey> &changedKeys);   // persistent change
    void invalidateAllColumns(int row);                                         // persistent change, e.g. new/replaced session
    void fillMissingColumns(int row, const SessionData &session);               // computes ONLY missing indices
    QMap<LogbookColumn, QVariant> computeColumnValues(const SessionData &session, const QVector<int> &columnIndices) const;
    void queueEnvironmentCheck();   void checkCalculationEnvironment();
    bool m_environmentCheckPending = false;   int m_registryObserver = -1;
```

- `columnNames`: `SessionAttribute` -> `attribute(attributeKey)`;
  `MeasurementAtMarker` -> `attribute(interpolationKey(markerAttributeKey, sensorID, SessionKeys::Time, measurementID))`;
  `Delta` -> both interpolation keys (the same keys `computeColumnValues`
  1454-1473 reads). `rebuildColumnDependencies()` runs at the end of
  `rebuildColumns` (133-172) and inside `checkCalculationEnvironment`.
- `invalidateColumns(row, changed)`: the affected columns are those with
  `m_columnDependencies[i].names` intersecting `changed`. Remove those indices
  from `cachedValues`, call `LogbookManager::markColumnsUnsaved(sessionId, thoseColumns)`,
  and `startColumnWorker()` if any. `invalidateAllColumns`: clear +
  `markSessionUnsaved`.
- `fillMissingColumns(row, session)`: indices absent from `cachedValues` ->
  `computeColumnValues(session, indices)` -> `updateCachedValues` + insert into
  `cachedValues`. Stats: `valuesComputed += indices.size()`;
  `calculationRuns +=` the delta of `session.calculationEngine().totalRunCount()`
  around the computation (skip the engine query entirely when `indices` is
  empty, so a row with nothing to do never creates an engine).

Call-site changes (tag line numbers):

| Site | Today | After |
|---|---|---|
| `setData` EditRole 447-469, `updateAttribute` 963, `removeAttribute` 1001 | `setAttribute` / `removeAttribute`, `scheduleSave` | additionally `invalidateColumns(row, {DependencyKey::attribute(key)})` right after the mutation. `Text` values pass through `CsvFormat::singleLine` first (also in `updateAttribute` when `newValue.typeId() == QMetaType::QString`). |
| `mergeSessions` loaded-match branch 523-543 | sets dirty | `invalidateColumns(row, keys)` with `keys` = `attribute(k)` for every incoming attribute key plus `measurement(s, n)` for every column of `newSession.sourceData()` |
| `mergeSessions` stub-replace 545-553 and new row 556-566 | sets dirty | `invalidateAllColumns(row)` |
| `mergeSessions` 571-577 (recomputes **every** loaded row) | `setCachedValues(computeColumnValues)` | delete the loop; call `startColumnWorker()` (rows touched above now have missing columns) |
| `saveNextSession` 1091-1095, `flushDirtySessions` 1058-1062 | save, `setCachedValues(all)`, `dirty = false` | `ok = saveSession(...)`; if `!ok` `qWarning("SessionModel: session %s was not saved: %s")` with `lastSaveError()`; `fillMissingColumns`; `dirty = false` in both cases (baseline behavior; a retry loop on a full disk would spin the idle scheduler - the unsaved marks keep the index honest) |
| `flushDirtySessions` 1065-1067 | flush if `anySaved` | flush if `anySaved \|\| logbook.indexNeedsFlush()` (so a discarded cache is rewritten with the marker even when nothing was dirty) |
| `evictSession` 1180-1197 | save if dirty; recompute all | save if dirty; `fillMissingColumns`; the row's `cachedValues` is then complete for stub display |
| `processNextDirtyColumn` 1247-1278 | whole row | loaded: `fillMissingColumns(row, *row.session)`; stub: temporary `loadSession`, remap as today, `fillMissingColumns(row, loaded)`, `sessionsLoaded++` |
| bulk edit `startBulkEdit` 1288-1312 | queue | additionally `invalidateColumns(row, {attribute(col.attributeKey)})` for every queued row up front, so the first `saveSession` performs one pre-save index flush for the whole batch instead of one per row |
| bulk edit loaded path 1376-1396 / stub path 1398-1421 | edit, save, recompute all | edit (`singleLine` for text), `invalidateColumns` (idempotent), `saveSession`, `fillMissingColumns(row, session-or-loaded)` |
| `rebuildColumns` 140-163 | loaded rows recompute all | unchanged logic, but use `computeColumnValues(session, allIndices)` and keep `setCachedValues`; then `rebuildColumnDependencies()` |
| `flushPendingInvalidations` (Phase 3 Task 3.10) | clears the row's `cachedValues`, `startColumnWorker()` | **remove the per-row clearing**; end with `if (m_environmentCheckPending) checkCalculationEnvironment();` Every broadcast invalidation (declared preference, registration change) is by construction an environment change, and the environment handler below covers loaded **and** unloaded rows in one place. |

Environment changes:
- Constructor: `m_registryObserver = CalculationRegistry::instance().addObserver([this]{ queueEnvironmentCheck(); });`
  and in the existing `preferenceChanged` lambda (122-128) add
  `else if (CalculationRegistry::instance().declaredPreferenceKeys().contains(key)) queueEnvironmentCheck();`.
  `queueEnvironmentCheck` sets the flag and queues `flushPendingInvalidations`
  through Phase 3's `m_invalidationFlushQueued` mechanism (coalesces the N
  registry changes of one `AltitudeMarkerManager::refresh()`).
- `checkCalculationEnvironment()`: if
  `calculationEnvironmentFingerprint() == logbook.cacheEnvironment()` return
  (covers A -> B -> A within one turn and the startup registrations, which
  happen before `initialize()`); otherwise `logbook.discardCachedValues()`,
  clear `cachedValues` of **every** row, `rebuildColumnDependencies()`,
  `emit dataChanged` over all rows, `startColumnWorker()`. Rows are not marked
  dirty or unsaved: persistent state did not change, so values recomputed by
  the worker are valid for the files on disk and may be flushed normally.

**Tests (`tst_column_cache`; `registerBuiltIns()` once; `init`: `useFreshLogbook()`, `resetPreferencesToDefaults()`, `LogbookManager::initialize()`, columns D = `_DESCRIPTION`, G = `MeasurementAtMarker{IMU, wx, markerAttributeKey "_M"}`, E = `SessionAttribute _EXIT_TIME`). The "gyro session" is built programmatically: TIME data of Phase 3 Task 3.12, `IMU/time` {10,20,30} `s`, `IMU/wx` {1,2,3} `deg/s`, `_M` = `1704110415.0`, the four backfill attributes, `SESSION_ID` = `g1`, `_DESCRIPTION` = `"first"`. Expected G: near `1.72032` (stale uncorrected value: `1.5`).**

| Test | Scenario | Expected |
|---|---|---|
| `upgradeDiscardsAndRecomputes` (acc. 18) | `mergeSessions({gyro})`, `waitForIdle`; destroy the model; rewrite `index.json`: remove `calculationCompatibility`, set G's value to `1.5`; `reopenLogbook()`; `initialize()`; new model, `populateFromIndex(cachedColumnValues(...), lastAccessedMap())`, `startColumnWorker()`, `waitForIdle` | before the worker: row's `cachedValues` empty; after: G near `1.72032`, D `"first"`; `columnWorkStats().sessionsLoaded == 1`; `index.json` has `calculationCompatibility` 1 and G near `1.72032`; the session CSV bytes are unchanged |
| `currentMarkerKeepsStaleValue` | same but keep the marker and fingerprint, only plant `1.5` | G stays `1.5`, `sessionsLoaded == 0` (control for the test above) |
| `editRefreshesOnlyAffectedColumn_warm` (acc. 18) | loaded row, `waitForIdle`; `resetColumnWorkStats()`; `sessionRef(0).calculationEngine().resetRunCounts()`; `updateAttribute("g1", _DESCRIPTION, "second")`; `waitForIdle` | `valuesComputed == 1`; `calculationRuns == 0`; engine `totalRunCount() == 0`; index D == `"second"`, G unchanged |
| `editRefreshesOnlyAffectedColumn_cold` (acc. 18) | reopen so the row is a stub with all values cached; `sessionRef(0)` (cold engine); `updateAttribute(_DESCRIPTION, "third")`; `waitForIdle` | `valuesComputed == 1`; `runCountForInstance("builtin.conversion.default#IMU/wx") == 0` and `runCount("builtin.time.fit") == 0` on that session; G still near `1.72032` in the row and the index |
| `markerEditRefreshesDependents` | `updateAttribute("g1", "_M", 1704110425.0)` | G recomputed to near `2.86720` (= 2.5 x 1.14688); D not recomputed (`valuesComputed == 1`) |
| `schemaEditRefreshesGyroColumn` | `updateAttribute("g1", "SCHEMA_VER", "2")` | G becomes `1.5` exactly; D, E untouched |
| `bulkEditOnStubComputesOneColumn` | stub row; `startBulkEdit({0}, colD, "bulk")`; `waitForIdle` | `sessionsLoaded == 1`, `valuesComputed == 1`, `calculationRuns == 0`; CSV has the new description; index D `"bulk"`, G unchanged |
| `interruptedSaveViaModel` | loaded row, all flushed; `updateAttribute(_DESCRIPTION, "crash")`; **do not** spin the event loop; call `LogbookManager::instance().saveSession(model.sessionRef(0))` directly (the `SaveTask` step without its completion flush); snapshot `index.json`; `reopenLogbook()`; `initialize()`; new model + worker | snapshot: D absent for `g1`, G present; after recompute D == `"crash"` == the CSV's `_DESCRIPTION`; at no point does a flushed D differ from the CSV |
| `indexFlushWhileDirtyOmitsUnsaved` | edit as above; without spinning the event loop call `LogbookManager::instance().flushIndex()` (what a `ColumnTask` completion would do before the save ran) | on disk: D absent for `g1`, G present; CSV still says `first` |
| `preferenceChangeDiscardsUnloadedRows` | two sessions from `DescentFixture` (`s1` loaded, `s2` evicted/stub), column `_ANALYSIS_START_TIME`-dependent E; `PreferencesManager::setValue(ImportDescentPauseSeconds, 5.0)`; `flushPendingInvalidations()`; `waitForIdle` | both rows' `cachedValues` were cleared then refilled (`sessionsLoaded >= 1`); `cacheEnvironment()` equals the new fingerprint and is what `index.json` contains; neither row is `dirty`; no CSV was rewritten (bytes equal) |
| `snapshotPreferenceDoesNotDiscard` | `setValue(AeroMass, 90.0)` | `valuesComputed == 0`, fingerprint unchanged |
| `altitudeMarkerChangeDiscards` | `AltitudeMarkerManager` as in Phase 3 Task 3.12; add an altitude | one environment check for the whole refresh; stub row values recomputed; restore in `cleanup()` |
| `saveFailureIsReported` | make the loaded session ragged via `setMeasurement`; `scheduleSave` through an edit; `waitForIdle` | exactly one `"was not saved"` warning naming sensor and lengths; scheduler goes idle (no retry loop); on-disk CSV unchanged; index has no values for the columns marked unsaved |
| `lineBreaksAreFlattenedAtEdit` | `updateAttribute(_DESCRIPTION, "a\nb")` | stored value `"a b"`; saved line `$VAR,_DESCRIPTION,a b` |

**Acceptance Criteria:**
- [ ] `git grep -n "setCachedValues\|computeColumnValues(" -- src/sessionmodel.cpp` shows whole-row computation only in `rebuildColumns`; every other site goes through `fillMissingColumns`.
- [ ] Every site that mutates a row's persistent state (`setAttribute`, `removeAttribute`, `mergeSourceData`, session replacement) calls `invalidateColumns` / `invalidateAllColumns` before the next return to the event loop; this is documented as a rule in `sessionmodel.h` for Phase 6.
- [ ] `tst_column_cache` passes on Windows Release; acceptance 18 appears in comments on the upgrade and the two edit tests.
- [ ] `tst_session_model_engine` (Phase 3) still passes; its expectation "clears the row's `cachedValues`" for the preference broadcast now holds through the environment handler - adjust only that assertion's comment if needed, not its outcome.
- [ ] No new preference, dialog, or UI element exists.

**Complexity:** L (natural split: dependency map + `invalidateColumns` / `fillMissingColumns` + call sites; environment handling; save-failure + text flattening; tests)

---

### Task 5.8: Documentation touch-ups

**Purpose:** Keep the in-tree documentation truthful; the full `docs/DATA_SCHEMA.md` rewrite is Phase 8.

**Files to modify:**
- `tests/README.md` - list `tst_csvformat`, `tst_persistence_roundtrip`, `tst_logbook_index`, `tst_column_cache`.
- `src/dataexporter.h`, `src/dataimporter.h`, `src/logbookmanager.h`, `src/calculations/builtincalculations.h` - the contract comments specified in Tasks 5.1-5.6 (number forms, non-finite tokens, ragged rule, line-break rule, save ordering, marker bump rule).

**Acceptance Criteria:**
- [ ] The bump rule for `CalculationCompatibilityVersion` is readable at the constant's definition.
- [ ] `tests/README.md` lists the four new targets; no mention of `dataSchemaVersion` anywhere.

**Complexity:** S

---

## Testing Requirements

### Unit Tests
- New targets: `tst_csvformat` (5.1), `tst_persistence_roundtrip` (5.4), `tst_logbook_index` (5.6), `tst_column_cache` (5.7).
- Extended: `tst_importer` (`nonFiniteTokens`, 5.2), `tst_calcregistry` and `tst_builtins_engine` (5.5), `tst_smoke::exportReloadRoundTrip` (bit equality, 5.4).
- Every other Phase 1-4 test passes unchanged. If `tst_smoke::logbookSaveReload` or `modelMergeSavesToTempLogbook` fails, the cause is ordering in Task 5.6 / 5.7, not the expectation.
- Acceptance mapping: **5** - `tst_persistence_roundtrip` (`samplesAreBitIdentical`, `unitsAndHeaderAttributesPreserved`, `schemaVerOnlyIfRecorded`, `effectiveValuesUnchanged`, `secondCycleIsByteIdentical`, `typedAttributesRoundTrip`, `warmAndColdCachesSameFile`, `logbookSaveReloadCycle`); **6** - `releasedLogbookSaveKeepsBytes`, `releasedLogbookBackfillIsAdditive` (load half is Phase 4's `releasedLogbookFormatLoads`); **18** - `tst_logbook_index::missingMarkerDiscardsValues`, `tst_column_cache::upgradeDiscardsAndRecomputes`, `editRefreshesOnlyAffectedColumn_warm` / `_cold`; spec 9.2 non-finite - `nonFiniteRoundTrip`, `nonFiniteTokens`; spec 9.4 interrupted save - `saveFlushesIndexFirst`, `interruptedSaveNeverDisagrees`, `interruptedSaveViaModel`, `indexFlushWhileDirtyOmitsUnsaved`; spec 12 "nothing rewrites recorded header attributes" - `releasedLogbookSaveKeepsBytes`, `schemaVerOnlyIfRecorded`.

### Integration Tests
- After every task: build with `-DFLYSIGHT_BUILD_TESTS=ON`, `ctest --test-dir build/FlySightViewer-build -C Release --output-on-failure` all green; after 5.3, 5.6, 5.7 also build with the option OFF.
- Grep criteria of Tasks 5.1, 5.3, 5.5, 5.6, 5.7 are part of the check.

### Manual Verification
1. Copy a logbook produced by the released version (`Documents/FlySight Viewer/logbook`) to a scratch folder and point the logbook-folder preference at it. Start the app with a gyro-dependent logbook column enabled: the column is blank at first and fills in during idle time with values about 14.7 % larger than before; `index.json` now starts with `calculationCompatibility`; no file under `sessions/` has a new modification time.
2. Restart: the columns appear immediately (no recomputation, status-bar progress absent).
3. Edit one description; after the save completes diff the session CSV against its previous copy: only the `$VAR,_DESCRIPTION` line differs (plus, for very old sessions, added `_JUMPER_MASS` / `_PLANFORM_AREA` / `_WIND_*` lines); `$UNIT` lines, numbers, and header attributes are untouched; no `SCHEMA_VER` line appeared.
4. Kill the process (Task Manager) right after an edit of a description in a large session, while the status bar shows saving. Restart: the logbook row shows either the old or the new description, and it always matches the `_DESCRIPTION` line in that session's CSV.
5. Change Preferences -> Import -> descent pause with many unloaded sessions: exit-time-dependent columns blank and refill; nothing is saved to `sessions/`.
6. Import a recording, close, reopen, open the session: plots identical; `GNSS/time` in the CSV is numeric seconds with at most three decimals.
7. Import, then kill the process before the first index flush; restart: the new session is present (orphan adoption).

## Notes for Implementer

### Gotchas
- **Negative zero.** Qt's number formatter prints `-0.0` as `0`. This is reachable with real data: a recorded field such as `-0.00` (a small negative velocity rounded by the firmware) parses to negative zero. Without the explicit branch in `formatDouble`, acceptance 5's "bit-identical" fails for it.
- **Do not format through `QTextStream` or `QVariant::toString()` anywhere in the exporter**; both are lossy for doubles.
- **Byte identity is guaranteed from the first Viewer-written file onward**, not from the device file: `62.50` re-saves as `62.5`, ISO times as numeric seconds, `$VAR` lines are reordered, `$UNIT` is always present. Values are identical from the device file onward.
- **A released file's numbers re-save to the same values but not always the same text** (`0.0001` vs `1e-04` thresholds differ between `'g', 15` and the shortest form). The acceptance-6 fixture uses values where the text is also stable; assert values, units, and attributes in any additional test.
- **`flushIndex()` writes `m_cacheEnvironment`, never a fresh fingerprint.** Computing it fresh would bless stale values after an unnoticed environment change.
- **Tests must call `registerBuiltIns()` before `LogbookManager::initialize()`** (as the application does), otherwise the fingerprint captured at `initialize()` differs from the one at the next start and every reopen discards the cache.
- **`markColumnsUnsaved` before `saveSession`, `fillMissingColumns` after.** Filling before the save is harmless (the flush skips marked columns) but wastes work if the row changes again.
- **`staticDependencies` is a superset on purpose.** Do not "optimize" it to the winning candidate: which candidate wins depends on session state, and the rows this serves are usually not loaded.
- **Interpolation-key column names are family instances**; `candidatesFor` instantiates them, so the closure works without a loaded session. A malformed key simply yields a closure containing only itself.
- **`SessionRow` sorting / row moves:** `m_columnDependencies` is per column, not per row; nothing here keeps row pointers.
- **Stub rows blank while refilling** after an upgrade or environment change. This is the same presentation as a newly added logbook column; do not add "stale value" display state.
- `QJsonObject` orders keys alphabetically; do not assert on field order in `index.json`.
- `LogbookColumnStore::setColumns` emits `columnsChanged` -> `rebuildColumns` -> `flushIndex`; in tests set columns **before** planting a hand-edited `index.json`.
- `DataExporter::toBytes` builds the whole file in memory; `exportSession` must keep the streaming 4 MB flush for large sessions and must not be implemented as `toBytes` + one write.
- Phase 7 is editing `pluginhost.cpp`, `*_bindings.cpp`, `pluginsessionview.h`, `python_plugins/` concurrently. Nothing in this phase needs them; if the fingerprint test wants a "plugin-like" id, register a synthetic calculation on a private registry.

### Decisions Made
- **Number format: shortest round trip via `QByteArray::number(v, 'g', QLocale::FloatingPointShortest)`, wrapped in `CsvFormat::formatDouble`.** Exact by construction (double-conversion `SHORTEST`) and paired with the importer's correctly rounded `toDouble`. Preferred over 17 significant digits because files stay readable and compact (`0.1`, `45.1234567`, `1704110400.4` instead of `0.10000000000000001`, `45.123456699999998`, `1704110400.4000001`) - which is the point of keeping a CSV format "recoverable with ordinary tools" - and because canonical text is reproduced byte-for-byte. Preferred over `std::to_chars` because libc++ makes the floating-point overloads unavailable below macOS 13.3 and CI targets 12.0. The guarantee is pinned by a 200 000-value bit-pattern sweep.
- **`-0.0` is written as `-0`**; NaN sign and payload are not preserved (one quiet NaN) - documented, tested with `std::isnan`.
- **Non-finite tokens: `nan`, `inf`, `-inf`**, written for rows and for double attributes, accepted explicitly by `CsvFormat::parseDouble`. A NaN sample survives save and reload in place; nothing is ever written as zero or dropped.
- **Time columns are saved as numeric seconds**, exactly as every released version has done, using the same shortest form (which yields at most three decimals for millisecond-derived values, e.g. `1718900000.123`). Reasons: spec 3.1 stores the parsed sample (a double of seconds) and gives the session no place to remember that a column was ISO text; bit-identical samples and "released logbooks load unchanged" both follow for free; a Unix-seconds column is recoverable with ordinary tools; inferring "this column is a timestamp" from its name or values would violate the spirit of spec 12. **Spec 9.2's "ISO timestamps are written at millisecond precision" is applied where the writer actually emits ISO text: `QDateTime`-typed attributes -> UTC `Qt::ISODateWithMs`.** No current code stores one (`_IMPORT_TIME` is a double of seconds and is saved as such). See Open Questions.
- **Attribute text forms** per `QVariant` type as tabled in Task 5.1; strings verbatim; all values reload as `QString`, which makes the second cycle byte-identical without any type tagging. The importer is **not** changed to re-type `_` attributes on load.
- **Line breaks in values are replaced by a space** (at the model's edit entry points, and again defensively in the exporter). Escaping would corrupt verbatim recorded values; rejecting would lose the session. Commas need nothing: Phase 4's "value = remainder after the second comma" is the exact inverse of `"$VAR," + key + "," + value`.
- **Unrepresentable attribute (invalid `QVariant`, unconvertible type, key with `,` / line break) -> skipped with a warning; unrepresentable sensor / column / unit text or a ragged sensor -> the save fails** with a message and leaves the previous file intact. Dropping an attribute nobody can produce is tolerable; silently dropping or reshaping measurement data is not.
- **Ragged sensors are an error, not padded.** Padding with `nan` would make the reloaded source differ from the saved one (violating acceptance 5) and would feed NaN time stamps into `min_element`-style calculations; truncating loses data; the row format cannot express unequal lengths. Validation is per sensor, up front, so there is never an out-of-range read. Consequence for Phase 6 under Open Questions.
- **Marker:** `FlySight::CalculationCompatibilityVersion` (`constexpr int`, `src/calculations/builtincalculations.h`), JSON field `calculationCompatibility`, initial value `1`; an absent field reads as `0`, so every released index is invalidated once. Missing or different -> all cached `values` dropped, `uuid` / `lastAccessed` kept, session files untouched, lazy recomputation by the existing `ColumnTask`, index rewritten by its completion flush (or at shutdown via `indexNeedsFlush()`).
- **Unloaded-session staleness (Phase 3's open question): cache validity also includes an environment fingerprint** - JSON field `calculationEnvironment`, SHA-1 over the ordered registration ids and the values of declared preference inputs. At startup a mismatch discards like the marker does (this also covers a Python plugin added or removed between runs). At run time `SessionModel` observes registry changes and declared-preference changes, coalesces them, and on a changed fingerprint discards **all** cached column values of **all** rows (coarse on purpose: the descent-pause preference feeds nearly every marker-based column, such changes are rare, and the recomputation path already exists). `flushIndex` records the fingerprint the values were computed under, so a crash between the change and the discard is detected at the next start. This replaces Phase 3's per-row `cachedValues.clear()` in `flushPendingInvalidations`.
- **Interrupted-save scheme: invalidate-then-write with unsaved-column marks inside `LogbookManager`.** A persistent change marks the affected columns unsaved (values removed; `flushIndex` omits marked columns even if recomputed); `saveSession` flushes the index first when the on-disk index may still hold a marked column, then writes the CSV, then clears the marks; the normal completion flush publishes the new values. Chosen over a content stamp (size + mtime per entry) because it needs no `stat` of every session file at startup, is immune to cloud-sync / copy tools rewriting timestamps, and is deterministic to test; chosen over a save-generation `$VAR` because that would change the file on every save (breaking "repeating the cycle changes nothing") and require opening every CSV at startup. No attribute of any kind is added to session files.
- **Orphan adoption** at `initialize()` (a CSV not referenced by the index becomes an identity stub) closes the remaining interrupted-save case - a new session saved but not yet indexed - at the cost of one directory listing.
- **Per-column refresh uses registration-derived static closures** (`CalculationRegistry::staticDependencies`), not the engine's dynamic invalidation sets. Dynamic sets only contain names that were resolved in that particular engine, so they under-report for a session loaded from a stub (cold engine) and do not exist at all for unloaded rows; the static closure is sound in every state. Affected columns are removed, and every maintenance site computes **only missing** columns (`fillMissingColumns`), which also removes the baseline's "recompute every loaded row on every merge".
- **Save failure handling stays non-retrying** (dirty flag cleared as at baseline) with a warning carrying the exporter's message; the unsaved marks keep `index.json` consistent with the old file.
- **`ColumnWorkStats`** on `SessionModel` is the test seam for stub rows, whose temporary sessions (and engines) are gone by the time a test could inspect run counters.
- **Small Phase 2 surface additions** in `CalculationRegistry`: `staticDependencies`, `declaredPreferenceKeys`, `addObserver` / `removeObserver`. Nothing in Phases 2-4 is contradicted.

### Open Questions
- **ISO timestamps (judgment call; needs Michael's confirmation).** This document reads spec 9.2's sentence as a constraint on ISO text the writer emits, and keeps time *columns* numeric as all released versions do. If Michael instead wants `GNSS/time` re-emitted as `2024-06-20T16:13:20.123Z`, the source layer needs a per-column "recorded as ISO" flag set by the importer, carried by `SourceColumn`, merged by `mergeSourceData`, and honored by the exporter (`llround(v * 1000)` -> `Qt::ISODateWithMs`, which round-trips bit-exactly for millisecond-derived doubles); released logbooks (numeric) would stay numeric. That is a contained follow-up to Tasks 5.3 / 5.4, but it touches Phase 4's `SourceColumn` equality and Phase 6's merge.
- **Environment fingerprint goes beyond the spec's integer marker.** If Michael prefers the minimal reading (integer only), drop the `calculationEnvironment` field and the startup comparison, keep the run-time discard in `checkCalculationEnvironment` (comparing against a fingerprint held only in memory), and accept that a crash between a preference change and the next index flush, or a plugin added between runs, leaves unloaded rows stale until they are next loaded.
- **Python plugin code changes are not detected**: a plugin whose id is unchanged but whose `compute` changed leaves cached columns of unloaded sessions stale until a session is loaded and edited or the environment otherwise changes. There are no third-party plugins; Phase 7 / 8 may want a note in the plugin README ("rename the plugin or toggle a logbook column to refresh").
- **For Phase 6:** (a) spec 6.4's "measurements not present in the incoming file are kept" can produce a ragged sensor when the incoming file has a different row count for that sensor; such a session cannot be saved under this phase's rule. Phase 6 must decide at merge time (reject the file with a clear error, or replace the whole sensor) - it must not rely on the exporter to cope. (b) Every new mutation path must call `invalidateColumns` / `invalidateAllColumns` (rule documented in `sessionmodel.h`), including merges into sessions that were loaded only for the merge. (c) The `loadSession` backfill is observed to be additive and idempotent (`releasedLogbookBackfillIsAdditive`); if Phase 6 removes or moves it, that test's expected line set changes and nothing else here does.
- **Orphan adoption and duplicate `SESSION_ID`s:** if an orphan file carries a `SESSION_ID` that is already indexed (only possible through manual file copying), `remapSessionId` refuses and the row keeps its file-stem id, showing a duplicate. Left as is; say so if Michael wants such files ignored instead.

## Definition of Done

This phase is complete when:
1. All eight tasks have passing acceptance criteria.
2. `tst_csvformat`, `tst_persistence_roundtrip`, `tst_logbook_index`, `tst_column_cache`, the extended `tst_importer` / `tst_calcregistry` / `tst_builtins_engine` / `tst_smoke`, and every other Phase 1-4 suite pass via CTest on Windows Release; the application and `flysight_cpp_bridge` build with `FLYSIGHT_BUILD_TESTS` ON and OFF.
3. Spec coverage: 3.2 (attributes preserved, `SCHEMA_VER` never invented) and 9.1 - Tasks 5.3, 5.4; 9.2 (exact numbers for rows **and** `$VAR`, ms timestamps, non-finite) - Tasks 5.1, 5.2, 5.4; 6.1 timestamp sentence - `samplesAreBitIdentical` (ISO in, exact seconds out and back); 9.3 - `releasedLogbookSaveKeepsBytes`; 9.4 (marker, lazy recompute, per-edit refresh, interrupted save) - Tasks 5.5-5.7; 12 (one authority per fact: text forms only in `CsvFormat`, cache validity only in `LogbookManager`; nothing rewrites recorded header attributes) - grep criteria and Task 5.4; acceptance 5, 6, 18 - mapped under Testing Requirements.
4. The exporter reads only `sourceData()` and stored attributes; no code path writes a session file except through `LogbookManager::saveSession`; `index.json` carries `calculationCompatibility` and `calculationEnvironment`; no `dataSchemaVersion`, no schema stamping, no Viewer-generated header attribute exists.
5. Files owned by Phase 7 are untouched; no TODOs or placeholder code remain; nothing has been pushed.
