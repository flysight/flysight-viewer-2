# Phase 4: Source layer, conversion layer, and parser

## Overview

This phase splits every stored measurement into two layers behind one name.
The **source layer** holds samples and unit text exactly as parsed and is read
only through explicit accessors that never compute. The **effective layer** is
what ordinary `getMeasurement` reads return: the source passed through the
built-in **conversion layer**, a parameterized engine calculation (schema
correction, then unit normalization) registered through Phase 2's
`registerSourceConversion` hook. The importer stops converting in place, parses
into a staging structure, validates `SCHEMA_VER` and file structure before
publishing anything, and never stamps attributes. The exporter and the merge
path are switched to source access so nothing is written or merged in effective
form.

User-visible result: gyro plots of legacy recordings become correct
(x 1.14688); nothing else changes.

## Dependencies

- **Depends on:** Phase 3 (and 1, 2).
- **Blocks:** Phases 5 and 7 (transitively 6 and 8).
- **Assumptions:**
  - Branch `schema-and-calculations`. `SessionData` is as Phase 3 Task 3.9 left
    it: implements `ISessionState` (`hasStoredAttribute`, `storedAttribute`,
    `hasSourceMeasurement`, `sourceMeasurement`, `sourceUnit` - all **public**
    overrides reading `m_attributes` / `m_sensors` / `m_units`), owns a lazily
    created `CalculationEngine` (`calculationEngine()`), every
    `getAttribute` / `getMeasurement` goes through the engine with no stored
    fast path, `setUnit` returns the invalidated set,
    `invalidateAllCalculations()` exists, `friend class DataImporter` is still
    present, `getUnit` still returns stored unit text.
  - No source-conversion family is registered yet, so a stored measurement
    resolves with provider `Source` (zero-copy passthrough) - Phase 2 Task 2.4
    step 5.
  - `CalculationRegistry::registerSourceConversion(const CalculationFamily &)`
    keeps an **ordered** list; `resolve` iterates
    `sourceConversionsFor(sensor, name)` exactly like ordinary candidates and
    **never** falls through to derived candidates when source data exists.
    `CalcInput::sourceMeasurement` / `sourceUnit` inputs are accepted only on
    conversion families. `CalculationResult::setMeasurement(sensor, name, values, unit)`
    carries a unit; `CalculationEngine::measurementUnit` returns it.
  - `registerBuiltInCalculations(CalculationRegistry &)` and
    `TestEnvironment::registerBuiltIns()` exist (Phase 3 Tasks 3.3 / 3.9).
    `PluginSessionView` reads through `EvaluationContext`.
  - Test support: `Fs2FileBuilder`, `Fs1FileBuilder`, `Fixtures::sensorFile()`,
    `Fixtures::trackFile()`, `DescentFixture`, `copyStoredState`,
    `FakeSessionState`, `FLYSIGHT_TEST_MAIN`, `flysight_add_test`.
  - Line numbers for `src/dataimporter.cpp/.h`, `src/dataexporter.cpp`,
    `src/sessiondata.h`, `src/logbookmanager.cpp`, `src/mainwindow.cpp` are
    **baseline** (`git show v2026.04.1:<path>`). Nothing from `master`'s
    `GyroScaling` / `DataSchema` / schema-stamping code is used.

## Design summary (read before the tasks)

### Names introduced by this phase

| Name | Where | Meaning |
|---|---|---|
| `SessionData::getMeasurement(sensor, name)` | unchanged name | **Effective** samples (may compute). |
| `SessionData::effectiveUnit(sensor, name)` | new | Unit the effective samples are expressed in (may compute). |
| `SessionData::sourceMeasurement(sensor, name)` | exists (ISessionState) | Recorded samples; never computes; empty when absent. |
| `SessionData::sourceUnit(sensor, name)` | exists (ISessionState) | Recorded unit text; never computes; empty when the measurement has no source data. |
| `SessionData::hasSourceMeasurement(sensor, name)` | exists (ISessionState) | Presence of source data. `hasMeasurement` is its enumeration-side synonym. |
| `SessionData::setSourceMeasurement(sensor, name, samples, unit)` | new | Samples and unit text move together. |
| `SessionData::sourceData()` / `mergeSourceData(const SourceData &)` | new | Bulk source read / bulk source-set path (replaces `friend class DataImporter`). |
| `SessionData::getUnit`, `SessionData::units` | **removed** | No ambiguous unit accessor remains. |
| `FlySight::Schema` (`src/conversion/schematable.h`) | new | The one schema table + `SCHEMA_VER` validation helper. |
| `builtin.conversion.schema`, `builtin.conversion.default` | new | The two source-conversion families, in this registration order. |

### How "absent `SCHEMA_VER` means schema 1" works with all-required inputs

Engine inputs are all required (spec 7.1), so one calculation cannot declare
`SCHEMA_VER` and also run without it. Phase 2 anticipated this with the ordered
conversion list. Two families are registered, in this order:

1. **`builtin.conversion.schema`** - `instantiate` matches **only**
   measurements for which the schema table has a correction in any version
   (`Schema::isSchemaDependent`: today `IMU/wx`, `IMU/wy`, `IMU/wz`). Inputs,
   in this order: `CalcInput::sourceMeasurement(s,n)`,
   `CalcInput::sourceUnit(s,n)`, `CalcInput::attribute("SCHEMA_VER")`.
2. **`builtin.conversion.default`** - matches **every** measurement name.
   Inputs: `CalcInput::sourceMeasurement(s,n)`, `CalcInput::sourceUnit(s,n)`.
   It applies the correction of `Schema::ImpliedVersion` (= 1).

Resolution of `IMU/wx` on an unmarked session (Phase 2 Task 2.4): `resolve`
notes `SourceMeasurement(IMU,wx)`, then tries candidate 1. Its availability
pass resolves the attribute `SCHEMA_VER`: `Resolution(attr SCHEMA_VER)` notes
`StoredAttribute(SCHEMA_VER)` **unconditionally**, finds nothing stored and no
candidate, and is cached as `None`. Candidate 1 is published with status
`MissingInput` and an edge to `Resolution(attr SCHEMA_VER)`; because every
candidate tried is noted, `Resolution(meas IMU/wx)` depends on
`Result(builtin.conversion.schema#IMU/wx)` **and** on
`Result(builtin.conversion.default#IMU/wx)`, which wins.

When `SCHEMA_VER` appears later (escape hatch, acceptance 8; or a programmatic
`setAttribute`): `attributeChanged("SCHEMA_VER")` seeds
`StoredAttribute(SCHEMA_VER)` -> `Resolution(attr SCHEMA_VER)` ->
`Result(...schema#IMU/wx)` -> `Resolution(meas IMU/wx)` -> everything that read
it (`IMU/wTotal`, interpolated attributes, ...). The next read re-resolves;
candidate 1 now runs and wins. `Result(...default#IMU/wx)` was not invalidated
(it never looked at the attribute) and is simply not consulted; removing the
attribute later re-selects it **without re-running it**.

Measurements that are not schema-dependent have only candidate 2, so they have
**no** dependency on `SCHEMA_VER`: changing the attribute invalidates the three
gyro columns and their dependents, nothing else.

### Conversion arithmetic and buffer sharing (spec 5.3)

Per instance, with `s` = schema scale (1.0 or the table's factor) and
(`k`, `o`, `label`) = the unit table's scale, offset, normalized label:

- If `s == 1.0 && k == 1.0 && o == 0.0` the conversion is a **value identity**:
  the compute function passes the `QVector<double>` it got from
  `ctx.sourceMeasurement(...)` straight into
  `CalculationResult::setMeasurement(sensor, name, samples, label)`.
  `QVector` is implicitly shared, so `m_sensors`, the engine's `Result` bundle,
  the `Resolution` entry, and every consumer's copy reference **one buffer**.
  This covers unknown unit text, already-internal labels, and label-only
  changes such as `deg C` -> `degC` or `(m/s)` -> `m/s`. The compute function
  must not call any non-const member (`operator[]`, `data()`, `begin()`,
  `detach()`) on that vector - only `constData()` / `size()` / copy.
- Otherwise exactly one new buffer is allocated (`QVector<double> out(n)`),
  filled in a single pass reading `in.constData()`:
  schema step first, unit step second - `v = in[i]; if (s != 1.0) v *= s;
  if (k != 1.0 || o != 0.0) v = v * k + o; out[i] = v;`. One extra buffer per
  converted column while cached; nothing is allocated at import; nothing is
  evicted.
- Conversion happens on the first ordinary read of that column (lazy), is
  cached by the engine, and is dropped by normal invalidation.

---

## Tasks

Order matters: Tasks 4.1-4.5 are behavior-neutral (every Phase 1-3 test passes
unchanged after each). **Task 4.6 is the switch**: it removes import-time
conversion, registers the conversion families, and flips the pinned
expectations in one step. Task 4.7 adds the session-level acceptance suite.

### Task 4.1: Schema table and `SCHEMA_VER` validation helper

**Purpose:** One authority for which schema versions exist, what each corrects, and what counts as a valid recorded `SCHEMA_VER` - shared by the conversion layer and the importer.

**Files to create:**
- `src/conversion/schematable.h` / `.cpp` (in `flysight_core`)
- `tests/tst_schema_units.cpp` (class `SchemaUnitsTest`; extended in Task 4.2)

**Files to modify:**
- `src/CMakeLists.txt` - add both files to `flysight_core`.
- `tests/CMakeLists.txt` - `flysight_add_test(tst_schema_units SOURCES tst_schema_units.cpp)`.

**Technical Approach:**

```cpp
namespace FlySight::Schema {
constexpr char AttributeKey[] = "SCHEMA_VER";      // the only place this string literal appears in src/
constexpr int  ImpliedVersion = 1;                 // meaning of an ABSENT attribute - conversion layer only

QList<int> supportedVersions();                    // {1, 2}, ascending
// Recorded value -> version. nullopt = malformed or unsupported. Never returns ImpliedVersion for
// "absent": callers that have no attribute do not call this.
std::optional<int> parseVersion(const QVariant &recorded);
// "Unsupported SCHEMA_VER '<text>' (supported: 1, 2)" - the single message template.
QString unsupportedMessage(const QVariant &recorded);
// True iff any supported version has a correction for this measurement.
bool   isSchemaDependent(const QString &sensor, const QString &name);
// Multiplicative correction for (version, measurement); 1.0 when the table has no row.
double correctionScale(int version, const QString &sensor, const QString &name);
}
```

- The table is a file-local `static const` array in `schematable.cpp`:
  rows `{version 1, "IMU", "wx", 1.14688}`, `{1, "IMU", "wy", 1.14688}`,
  `{1, "IMU", "wz", 1.14688}`; version 2 has no rows. Write the factor as the
  **decimal literal `1.14688`**, once, in a named constant
  (`kLegacyGyroScale`) with a comment citing spec section 1. Do **not** write
  it as `0.070 / (2000.0 / 32768.0)`: in IEEE double that expression evaluates
  to `1.1468800000000001`, a different value.
- `parseVersion`: `const QString t = recorded.toString().trimmed();` valid iff
  `t` matches `^[0-9]+$`, `t.toInt(&ok)` succeeds, and the value is in
  `supportedVersions()`. So `"1"`, `"2"`, `" 2"`, `QVariant(2)` are valid;
  `""`, `"abc"`, `"3"`, `"0"`, `"2.0"`, `"-1"`, `"+2"`, an invalid `QVariant`
  are not. The *stored* attribute is never trimmed or rewritten by anyone.
- Nothing here reads `FIRMWARE_VER`, file names, dates, or data (spec 12).
- Future schema versions add rows here and steps in Task 4.4; nothing else.

**Tests (`tst_schema_units`, literals only):** `supportedVersions() == {1,2}`;
`parseVersion` for each example above; `unsupportedMessage("3")` ==
`"Unsupported SCHEMA_VER '3' (supported: 1, 2)"`, same shape for `"abc"` and
`""`; `isSchemaDependent("IMU","wx"/"wy"/"wz")` true, `("IMU","wTotal")`,
`("IMU","ax")`, `("GNSS","wx")` false; `correctionScale(1,"IMU","wx") == 1.14688`
(exact `==`), `correctionScale(2,"IMU","wx") == 1.0`,
`correctionScale(1,"IMU","ax") == 1.0`.

**Acceptance Criteria:**
- [ ] `git grep -n "1\.14688" -- src` has exactly one hit (in `schematable.cpp`); `git grep -n "\"SCHEMA_VER\"" -- src` has exactly one hit (in `schematable.h`).
- [ ] `schematable.*` includes Qt Core only; no `SessionData`, engine, or preferences include.
- [ ] `tst_schema_units` passes; all earlier tests pass unchanged.

**Complexity:** S

---

### Task 4.2: Unit normalization table

**Purpose:** Turn the importer-only file-unit table into the conversion layer's normalization table: add the identity rows for labels written by released logbooks, make lookups silent.

**Files to modify:**
- `src/units/unitconversion.h`
- `tests/tst_schema_units.cpp`

**Technical Approach:**

- Keep `struct ConversionSpec { double scale; double offset; QString siUnit; }`
  and `UnitConversion::getConversion` / `requiresConversion` / `lookup()`.
  Update the comments: the table now means "recorded unit text -> Viewer's
  internal unit", used by the conversion layer at read time; `siUnit` is the
  *normalized label*.
- Add identity rows (after line 107): `t["m/s^2"] = {1.0, 0.0, "m/s^2"};`
  `t["T"] = {1.0, 0.0, "T"};` `t["degC"] = {1.0, 0.0, "degC"};`. These are the
  labels baseline Viewer wrote into logbook files (spec 5.2, 9.3). The existing
  rows stay exactly as they are: `g` -> x9.80665 `m/s^2`; `gauss` -> x0.0001
  `T`; `deg C` -> label `degC`; `m`, `m/s`, `Pa`, `s`, `deg`, `deg/s`, `V`,
  `%`, `""` identity; aliases `(m)`, `(m/s)`, `(deg)`, `volt`, `percent`
  (label-only).
- Lookup key is `unitText.trimmed()`; an unknown text returns
  `{1.0, 0.0, unitText}` with the label **untrimmed and unchanged**.
- **Delete the `qWarning` at lines 45-48.** Unknown unit text is the normal
  case for custom columns; the conversion runs lazily per column per session,
  so a warning would repeat for every session opened (spec 12: silence).
- `toSI` (73-85) stays until Task 4.6 deletes it together with its only caller.

**Tests added to `tst_schema_units`:** `getConversion("g")` == {9.80665, 0.0, "m/s^2"};
`"gauss"` == {0.0001, 0.0, "T"}; `"deg C"` == {1.0, 0.0, "degC"};
`"m/s^2"`, `"T"`, `"degC"`, `"deg/s"`, `""` identity with the same label;
`"(m/s)"` -> label `"m/s"`; `"percent"` -> `"%"`; `"furlongs"` and
`" odd unit "` -> identity with the label returned verbatim;
`requiresConversion` true only for `g` and `gauss`; a `qInstallMessageHandler`
counter shows **zero** warnings across all of these lookups.

**Acceptance Criteria:**
- [ ] No `qWarning` remains in `unitconversion.h`.
- [ ] Rows for `m/s^2`, `T`, `degC` exist; no existing row changed value.
- [ ] All earlier tests pass unchanged (the importer still uses the table at this point).

**Complexity:** S

---

### Task 4.3: `SessionData` source / effective API; exporter, merge, and test-support switch to source access

**Purpose:** Give the source layer an explicit, non-computing API and a bulk set path; remove the ambiguous unit accessor; move every caller that must see recorded data onto it. Behavior-neutral (no conversion family is registered yet, so source == effective).

**Files to modify:**
- `src/sessiondata.h` / `.cpp`
- `src/dataexporter.cpp` (baseline 97, 121, 128)
- `src/sessionmodel.cpp` (`mergeSessions`, 527-536)
- `tests/support/builtinfixture.cpp` (`DescentFixture::load`, `copyStoredState`)
- `tests/tst_smoke.cpp` (mechanical `getUnit` -> `sourceUnit` rename only; expectations unchanged in this task)

**Technical Approach:**

Public surface after this task (additions / changes relative to Phase 3 Task 3.9):

```cpp
struct SourceColumn {
    QVector<double> samples;
    QString unit;                                   // recorded unit text, verbatim
    bool operator==(const SourceColumn &o) const;   // samples == && unit ==
};
using SourceSensor = QMap<QString, SourceColumn>;  // measurement name -> column
using SourceData   = QMap<QString, SourceSensor>;  // sensor name -> columns

class SessionData : public ISessionState {
public:
    // ---- effective layer (ordinary consumers; may run calculations) ----
    QVector<double> getMeasurement(const QString &sensor, const QString &name) const;   // unchanged
    QString effectiveUnit(const QString &sensor, const QString &name) const;            // NEW: calculationEngine().measurementUnit(...)

    // ---- source layer (never computes, never falls back to derived values) ----
    bool            hasSourceMeasurement(const QString&, const QString&) const override; // unchanged
    QVector<double> sourceMeasurement(const QString&, const QString&) const override;    // unchanged; empty when absent
    QString         sourceUnit(const QString&, const QString&) const override;           // CHANGED: "" unless hasSourceMeasurement
    SourceData      sourceData() const;                                                  // NEW: everything, shares buffers
    QSet<DependencyKey> setSourceMeasurement(const QString &sensor, const QString &name,
                                             const QVector<double> &samples, const QString &unit);   // NEW
    QSet<DependencyKey> mergeSourceData(const SourceData &incoming);                     // NEW: bulk source-set path
    QSet<DependencyKey> setMeasurement(const QString&, const QString&, const QVector<double>&);      // unchanged: replaces source samples, unit kept
    QSet<DependencyKey> setUnit(const QString&, const QString&, const QString&);         // CHANGED: no-op + qWarning + {} when the measurement has no source data

    // ---- enumeration: stored source data and stored attributes only (spec 4) ----
    QStringList sensorKeys() const;  bool hasSensor(const QString&) const;
    QStringList measurementKeys(const QString&) const;
    bool hasMeasurement(const QString&, const QString&) const;        // == hasSourceMeasurement
    QStringList attributeKeys() const;  bool hasAttribute(const QString&) const;

    // REMOVED: getUnit(), units()
};
```

- `effectiveUnit`: for a stored measurement it is the normalized label carried
  by the conversion result (after Task 4.6; until then the engine's `Source`
  provider returns the source unit). For a **derived** measurement it is
  whatever unit its calculation passed to `CalculationResult::setMeasurement`;
  all built-ins pass none, so it is `""` ("not reported"), exactly as `getUnit`
  behaved for calculated measurements at baseline. Assigning units to built-in
  outputs is out of scope. For an unavailable name it is `""`.
- `sourceMeasurement` / `sourceUnit` / `hasSourceMeasurement` / `sourceData`
  read `m_sensors` / `m_units` only. They must not touch `m_engine` (not even to
  create it). Asking for the source of a derived name such as `IMU/wTotal`
  returns an empty vector / `""` / `false`.
- `setSourceMeasurement`: insert samples and unit, then (only if `m_engine`
  exists) unite `sourceMeasurementChanged` and `sourceUnitChanged`; with no
  engine return `{DependencyKey::measurement(sensor, name)}` (Phase 3 rule).
- `mergeSourceData(incoming)`: for every column in `incoming`, the same as
  `setSourceMeasurement`; columns and sensors not mentioned are kept (this is
  spec 6.4's rule, which Phase 6 builds on). Returns the union. Handing over a
  `QVector` shares its buffer - no sample is copied.
- `sourceData()` builds the nested map from `m_sensors` + `m_units` (implicit
  sharing; no deep copy). The internal representation (`m_sensors`, `m_units`)
  is unchanged, so the `ISessionState` overrides stay as Phase 3 wrote them
  apart from the `sourceUnit` presence rule.
- `friend class DataImporter` and `invalidateAllCalculations()` are **not**
  removed yet (Task 4.5 removes both).

Caller audit (every hit of
`git grep -nE "getUnit\(|\.units\(|sensorKeys\(|measurementKeys\(|hasMeasurement\(|hasSensor\(|setUnit\(|setMeasurement\(" -- src`):

| Caller | Today | After this task |
|---|---|---|
| `dataexporter.cpp` 82, 84, 112 | `sensorKeys` / `measurementKeys` | unchanged (stored enumeration). |
| `dataexporter.cpp` 97 | `getUnit` | `sessionData.sourceUnit(sensorKey, col)`. |
| `dataexporter.cpp` 121, 128 | `getMeasurement` | `sessionData.sourceMeasurement(sensorKey, ...)`. **This is the whole "minimal exporter switch".** Line 78 (`getAttribute` on a stored key), `'g', 15` formatting, row count from the first column, non-finite handling are Phase 5 - do not touch. |
| `sessionmodel.cpp` 527-529 | `setAttribute(k, newSession.getAttribute(k))` | `newSession.storedAttribute(k)` (same value; never consults the engine). Overwrite semantics unchanged (Phase 6). |
| `sessionmodel.cpp` 531-536 | per-measurement `setMeasurement(..., newSession.getMeasurement(...))`; units never merged | `existingSession.mergeSourceData(newSession.sourceData())`, its returned set united into the local invalidation hash added by Phase 3 Task 3.10. Without this, after Task 4.6 a merge would write *effective* (already corrected, SI) values into the source layer under no unit, and the gyro would be corrected twice. Nothing else in `mergeSessions` changes here. |
| `PlotWidget.cpp` 339 | `sensorKeys()` then `getMeasurement(sensorKey, m_xVariable)` | unchanged: enumeration is stored-only, the read is effective. |
| `timecalculations.cpp` 45-47, 116 | `hasMeasurement` | already removed by Phase 3. |
| `sessiondata_bindings.cpp` / `pluginsessionview.h` | `PluginSessionView` over `EvaluationContext` | unchanged: `ctx.measurement` is an ordinary (effective) read. Source access from Python is Phase 7. |
| `logbookmanager.cpp` 351-389 `loadSession`, 441-465 `scanSessionFiles` | `readFile` + attribute backfill | unchanged in this phase (see Task 4.6 notes). |
| `tests/support/builtinfixture.cpp` | `load()` merges with `setMeasurement` + `setUnit` from `getMeasurement` / `getUnit`; `copyStoredState` copies via `getUnit` (+ measurement reads) | `load()`: `track.mergeSourceData(sensor.sourceData())` (attributes as before). `copyStoredState`: iterate `from.sourceData()` and `from.attributeKeys()` / `storedAttribute`. After Task 4.6 these helpers must carry **raw** data (`g`, `gauss`), so they may not use effective getters. |

**Acceptance Criteria:**
- [ ] `git grep -nE "getUnit\(|\.units\(" -- src` returns nothing (restricted to `src`: Phase 1's `Fs1FileBuilder::units()` in `tests/` is unrelated).
- [ ] `git grep -n "getMeasurement" -- src/dataexporter.cpp` returns nothing; `git grep -n "getMeasurement\|setMeasurement" -- src/sessionmodel.cpp` shows no hit inside `mergeSessions`.
- [ ] With a warm engine, calling `sourceMeasurement`, `sourceUnit`, `hasSourceMeasurement`, `sourceData`, `sensorKeys`, `measurementKeys`, `hasMeasurement`, `attributeKeys`, `hasAttribute` leaves `totalRunCount()` and `cachedNodeCount()` unchanged; on a session that never had an engine they do not create one.
- [ ] `setUnit` on a measurement without source data stores nothing and returns an empty set.
- [ ] All Phase 1-3 tests pass with unchanged expectations; a saved session file is byte-identical to one written before this task.

**Complexity:** M

---

### Task 4.4: The conversion families (side by side, not yet registered for the application)

**Purpose:** Implement spec section 5 as two `CalculationFamily` registrations and prove them on `FakeSessionState` with a private registry before they affect the application.

**Files to create:**
- `src/conversion/sourceconversion.h` / `.cpp` (in `flysight_core`)
- `tests/tst_conversion_engine.cpp` (class `ConversionEngineTest`)

**Files to modify:**
- `src/CMakeLists.txt`, `tests/CMakeLists.txt`.

**Technical Approach:**

```cpp
namespace FlySight::Calculations {
// Registers, in this order, "builtin.conversion.schema" then "builtin.conversion.default"
// through CalculationRegistry::registerSourceConversion. Engine registrations only.
void registerSourceConversions(CalculationRegistry &registry);
}
```

Follow the descriptor pattern of Phase 3 Task 3.3 (`Q_ASSERT(ok)` on the
registration result).

- **Instance key** (both families): `escape(sensor) + "/" + escape(name)` where
  `escape` replaces `%` -> `%25` and `#` -> `%23`. Phase 2 forbids `'#'` in an
  instance key and treats an invalid instance as "family does not match";
  because resolution never falls through once conversion families exist, an
  unescaped custom column called e.g. `temp#1` would become unreadable. Instance
  ids look like `builtin.conversion.default#IMU/ax`.
- **`builtin.conversion.schema`** `instantiate(name)`: `nullopt` unless
  `name.type == Measurement` and
  `Schema::isSchemaDependent(name.sensorKey, name.measurementKey)`. Inputs in
  this order: `sourceMeasurement(s,n)`, `sourceUnit(s,n)`,
  `attribute(Schema::AttributeKey)`. Output: `name`. Compute:
  `version = Schema::parseVersion(ctx.attribute(Schema::AttributeKey))`;
  `s = version ? Schema::correctionScale(*version, sensor, name) : 1.0`; then
  the shared kernel below. When `version` is `nullopt` emit one
  `qWarning("SCHEMA_VER '%s' is not supported; %s/%s left as recorded")`
  (see Decisions: an explicit but unrecognized declaration is never treated as
  legacy; the state is unreachable through import or logbook load).
- **`builtin.conversion.default`** `instantiate(name)`: any measurement name.
  Inputs: `sourceMeasurement(s,n)`, `sourceUnit(s,n)`. Compute:
  `s = Schema::correctionScale(Schema::ImpliedVersion, sensor, name)`; shared
  kernel. This is the **only** place absence is interpreted as schema 1.
- **Shared kernel** (file-local):
  `CalculationResult convert(const QString &sensor, const QString &name, const QVector<double> &in, const QString &unitText, double schemaScale)` -
  `spec = UnitConversion::getConversion(unitText)`; arithmetic and sharing
  rules exactly as in "Conversion arithmetic and buffer sharing" above;
  result unit = `spec.siUnit`.
- Compute functions read only through `ctx`; they never see `SessionData`,
  preferences, firmware version, or file names.
- An empty source column is "unavailable" by Phase 2's definition, so both
  candidates report `MissingInput` and the name is unavailable - the same
  outcome as Phase 3's stored-but-empty passthrough.
- The families are **not** added to `registerBuiltInCalculations` yet.

**Tests (`tst_conversion_engine`; private `CalculationRegistry`, `registerSourceConversions(registry)`, `FakeSessionState`, one `CalculationEngine`; literals only; `near(a,b)` = `qAbs(a-b) <= 1e-9`):**

| Test | Scenario | Expected |
|---|---|---|
| `legacyGyroCorrected` | `IMU/wx` {62.5}, `wy` {-125}, `wz` {0}, unit `deg/s`, no `SCHEMA_VER` | near 71.68, near -143.36, `== 0.0`; `measurementUnit` `"deg/s"`; `resultStatus("builtin.conversion.schema", meas IMU/wx) == MissingInput`; `runCountForInstance("builtin.conversion.default#IMU/wx") == 1` |
| `schema2Unchanged` | same + stored `SCHEMA_VER` = `"2"` | exactly 62.5, -125.0, 0.0; winner is the schema family (`runCountForInstance("builtin.conversion.schema#IMU/wx") == 1`, default instance 0) and the result shares the source buffer |
| `schema1Explicit` | `SCHEMA_VER` = `"1"` | near 71.68 |
| `absentSchemaDependencyRecorded` | unmarked; read `IMU/wx` | `dependenciesOf(GraphNode::resolution(meas IMU/wx))` == {`sourceMeasurement(IMU,wx)`, `result("builtin.conversion.schema#IMU/wx")`, `result("builtin.conversion.default#IMU/wx")`}; `dependenciesOf(resolution(meas IMU/ax))` contains no `...schema#...` node |
| `schemaAppearsLater` | unmarked, read `wx` (near 71.68) and `IMU/ax`; `fake.setAttribute(engine, "SCHEMA_VER", "2")` | returned set contains `IMU/wx`, not `IMU/ax`; `totalRunCount()` unchanged by the notification; next read 62.5; remove the attribute -> set contains `IMU/wx`, next read near 71.68 with `runCountForInstance("...default#IMU/wx")` **still 1** |
| `unsupportedInMemory` | stored `SCHEMA_VER` = `"3"`, then `"abc"` | 62.5 (as recorded), one warning each; `verifyAgainstFresh` empty |
| `unitNormalization` | `IMU/ax` {1} `g`; `MAG/x` {1}, `MAG/z` {-0.5} `gauss`; `IMU/temperature` {40} `deg C`; `X/c` {7} `furlongs`; `BARO/pressure` {90000} `Pa` | `== 9.80665` `m/s^2`; `== 0.0001`, `== -0.00005` `T`; 40 `degC`; 7 `furlongs`; 90000 `Pa` (x1.0 products are exact, so exact `==` is correct here) |
| `internalLabelsAreIdentity` | `IMU/ax` {9.80665} `m/s^2`, `MAG/x` {0.0001} `T`, `IMU/temperature` {40} `degC` | values and labels unchanged, buffers shared |
| `identitySharesBuffer` | `GNSS/hMSL` 1000 samples unit `m`; also `deg C` column; also `g` column | `fake.sourceMeasurement(...).constData() == engine.measurement(...).constData()` for the first two (hold both vectors in locals while comparing); **not equal** for the `g` column |
| `orderSchemaThenUnit` | hypothetical: `IMU/wx` {1} with unit `g` | `near(v, 11.247050752)` (= 1 x 1.14688 x 9.80665, schema step first, unit step second), unit `m/s^2` |
| `hashInColumnName` | `X/temp#1` {5} unit `""` | 5; `hasSourceConversions()`; instance id contains `%23` |
| `emptySourceUnavailable` | `IMU/wx` present but `{}` | unavailable, `cachedState == Unavailable`, no run |
| `unitChangeInvalidates` | `IMU/ax` {1} `g` -> read -> `setUnit(engine, ..., "m/s^2")` | set contains `IMU/ax`; next read 1.0 `m/s^2` |
| `derivedNotConverted` | no source `IMU/wTotal`; register Phase 3's `registerImuCalculations(registry)` too | `IMU/wTotal` near 160.28135262718 for 62.5/-125/0 (tolerance 1e-9); `runCount("builtin.conversion.default")` counts only wx, wy, wz |

**Acceptance Criteria:**
- [ ] `registerSourceConversions` registers exactly two ids, in the order `builtin.conversion.schema`, `builtin.conversion.default`; `isFamily` true for both; a plain calculation with a source input is still rejected.
- [ ] `sourceconversion.cpp` contains no numeric schema factor and no unit factor (they come from `Schema::` and `UnitConversion::`).
- [ ] `tst_conversion_engine` passes; every earlier test passes unchanged; the application's behavior is unchanged (families not registered globally).

**Complexity:** M

---

### Task 4.5: Importer restructure - staged parsing, validation before publication, error policy

**Purpose:** Make the importer parse into a private staging structure, validate, and only then publish through the public source API; define structural errors; preserve `$VAR` values exactly; drop the friendship. Import-time SI conversion is **kept for this task only** (applied to the staged columns) so that every pinned expectation still holds.

**Files to modify:**
- `src/dataimporter.h` / `.cpp`
- `src/sessiondata.h` / `.cpp` - remove `friend class DataImporter` (baseline h 155) and `invalidateAllCalculations()`.
- `tests/CMakeLists.txt`

**Files to create:**
- `tests/tst_importer.cpp` (class `ImporterTest`)

**Technical Approach:**

Public interface is unchanged (`importFile`, `readFile`, `initializeFromDevice`,
`getLastError`; baseline h 15-24). Contract, to be written in the header:

- `m_lastError` is cleared at entry of `readFile` and `importFile`.
- `readFile` returns `false` **and publishes nothing**: on any failure the
  `SessionData&` argument is left exactly as it was passed in. Callers pass a
  fresh `SessionData` (all three do: baseline `mainwindow.cpp` 582,
  `logbookmanager.cpp` 363, 453), so an existing session can never be touched
  by a failed import. Transactional *merge* is Phase 6.
- On success the importer publishes with public API only: one
  `setAttribute(key, value)` per header attribute, then one
  `mergeSourceData(staged)`. No `m_sensors` access, no
  `invalidateAllCalculations()` calls (delete the ones Phase 3 added before
  each `return`).
- The importer never inserts, rewrites, or defaults `SCHEMA_VER`.
- `initializeFromDevice` (85-137) is unchanged; Phase 6 moves its defaults to
  session creation. `importFile` calls it only after `readFile` succeeded.

Staging types (anonymous namespace in `dataimporter.cpp`; private helper
signatures in the header change accordingly - they take the staging object and
return `bool`):

```cpp
struct StagedSensor { QVector<QString> columns; QVector<QString> units; QVector<QVector<double>> samples; bool hasUnitLine = false; };
struct StagedFile   { QVector<QPair<QString,QString>> attributes;   // file order, conflicts already rejected
                      QMap<QString, StagedSensor> sensors; int skippedRows = 0; };
```

Rows append to `samples[i]` locally; at publication each vector is handed to
`SourceColumn` (shared, not copied).

**Header grammar (FS2, baseline 229-292), line by line until `$DATA`:**

| Line | Rule |
|---|---|
| `$FLYS,...` (first line) | format sniff as today (49-67). The version field is not validated (unchanged). |
| `$VAR,<key>,<value>` | `key` = text between the first and second comma, verbatim. `value` = **everything after the second comma, verbatim, commas included** (baseline 251 truncates at the next comma, which corrupts e.g. a `_DESCRIPTION` containing a comma on every reload). No second comma -> value `""`. Stored as `QString`. Unknown keys are kept. Empty key -> structural error. The same key twice with equal values collapses; with different values -> structural error. |
| `$COL,<sensor>,<c1>,...` | Tokenize with empty parts kept. Structural error if: sensor name empty; no columns; an empty column name; a duplicate column name within the sensor (baseline would append every row twice to that column); a second `$COL` for the same sensor. Column labels are stored verbatim (custom columns included). |
| `$UNIT,<sensor>,<u1>,...` | Structural error if the sensor has no preceding `$COL`, if there are **more** unit fields than columns, or on a second `$UNIT` for the sensor. Fewer fields than columns is allowed: missing units are `""`. Unit text is stored verbatim (not trimmed). No `$UNIT` line at all: all units `""`. |
| `$DATA` | compare `line.trimmed() == "$DATA"`. End of file before `$DATA` -> structural error `"Missing $DATA section"` (baseline silently produced an empty session). |
| blank line, any other `$XXX` or text | ignored (forward compatibility, as today). |

**Validation before publication:** after the header is parsed, if the staged
attributes contain `Schema::AttributeKey` and
`Schema::parseVersion(value)` is `nullopt` -> fail with
`Schema::unsupportedMessage(value)`. This covers `3`, `abc`, and the empty
value (`$VAR,SCHEMA_VER,` and `$VAR,SCHEMA_VER`). It applies to every file that
goes through `readFile`, including `TRACK.CSV` and logbook files. Do it before
reading data rows (cheap rejection).

**Data rows (baseline 294-366) - skipped, not errors.** Real recordings end in
a truncated line after power loss and occasionally contain a glitched row; the
current tolerance is kept. A row is **skipped** (and `skippedRows` incremented)
when: its `$TAG` has no `$COL`; its field count differs from the column count;
a field is empty; a field ending in `Z` is not a valid ISO date-time; a field
is not a valid double. A row is appended only when every field parsed (unchanged
all-or-nothing per row). Replace the per-row `qWarning`s (333, 341, 353) with
one summary per file after parsing:
`qWarning("%s: skipped %d malformed data row(s)")`, only when the count is > 0.
Zero data rows is **not** an error (header-only recordings exist, and rejecting
them would make already-saved empty sessions unloadable); declared columns are
published as empty source measurements, as today (268).

**Numeric parsing (confirming current behavior, spec 6.1):** a field ending in
`Z` -> `QDateTime::fromString(text, Qt::ISODate)`, value
`toMSecsSinceEpoch() / 1000.0` (millisecond precision; `tst_smoke::importFs2Track`
pins `...T12:00:00.400Z` -> 1704110400.4). Every other field ->
`QStringView::toDouble`, correctly rounded, no further processing. Whatever
`toDouble` accepts today (including `nan` / `inf` spellings) is still accepted;
non-finite handling on write is Phase 5.

**FS1 (baseline 139-186):** line 1 = column names split on `','` with empty
parts **kept**; an empty or duplicate name is a structural error (baseline used
`SkipEmptyParts`, which silently shifts columns). Line 2 = units, split keeping
empties, stored **verbatim** (baseline trimmed them; the normalization lookup
trims instead - Task 4.2); more units than columns is a structural error; a
missing unit line (file ends after line 1) is success with zero rows. Sensor
name `GNSS`. Rows as above. GNSS-only FS2 files and custom sensors/columns need
no special handling.

**Error text.** Existing: `"Couldn't read file"`, `"Empty file"`,
`"Unknown file format"` (unchanged; `tst_smoke::rejectsUnknownFormat` pins the
last). Schema: `Schema::unsupportedMessage`. Structural:
`"Line <n>: <reason>"` with 1-based line numbers, reasons exactly:
`"$VAR with empty name"`, `"conflicting values for $VAR <key>"`,
`"$COL without sensor name"`, `"$COL <sensor> has no columns"`,
`"$COL <sensor> has an empty column name"`,
`"$COL <sensor> repeats column '<c>'"`, `"duplicate $COL for sensor <sensor>"`,
`"$UNIT for unknown sensor <sensor>"`,
`"$UNIT <sensor> has more units than columns"`,
`"duplicate $UNIT for sensor <sensor>"`; FS1 uses `"column header has an empty name"`,
`"column header repeats '<c>'"`, `"unit line has more units than columns"`;
and the un-numbered `"Missing $DATA section"`.

**Temporary (removed in Task 4.6):** immediately before publication, apply the
old SI pass to the staged data - for each staged column
`UnitConversion::toSI(samples, unit)` and replace the staged unit with
`getConversion(unit).siUnit` - i.e. the behavior of baseline 176-185 / 209-226.
Mark the block `// PHASE4-SWITCH: remove in Task 4.6`.

**Tests (`tst_importer`; fresh `DataImporter` and fresh temp dir per test; literals only):**

| Test | Input | Expected |
|---|---|---|
| `rejectsSchema3` / `rejectsSchemaAbc` / `rejectsSchemaEmpty` / `rejectsSchemaNoValue` (acc. 3) | `Fixtures::sensorFile()` + `.var("SCHEMA_VER","3")` / `"abc"` / `""` / `rawHeaderLine("$VAR,SCHEMA_VER")` | `importFile` false; `getLastError()` == `"Unsupported SCHEMA_VER '3' (supported: 1, 2)"` etc.; target session: `attributeKeys()` empty, `sensorKeys()` empty |
| `failedImportLeavesTargetUntouched` (acc. 3) | target pre-populated with `SESSION_ID`, `_DESCRIPTION` = `"edited"`, `IMU/wx` {1,2} `deg/s`; import the `SCHEMA_VER,3` file into it | false; `attributeKeys()`, `sourceData()` equal the pre-import copies; `_IMPORT_TIME` absent (`initializeFromDevice` did not run) |
| `acceptsSchema1And2` | `.var("SCHEMA_VER","2")`, `"1"` | true; `getAttribute("SCHEMA_VER")` == `"2"` / `"1"` (`QString`) |
| `neverStampsSchema` (acc. 1) | unmarked sensor and track fixtures, FS1 file | `hasAttribute("SCHEMA_VER")` false for all three |
| `lastErrorClearedAtEntry` | one importer: failing file, then a good file | second call true and `getLastError().isEmpty()` |
| `varValueKeepsCommas` | `rawHeaderLine("$VAR,_DESCRIPTION,Perris, run 2, windy")`, `rawHeaderLine("$VAR,CUSTOM_KEY,a=b")`, `rawHeaderLine("$VAR,EMPTY_VAL,")` | `"Perris, run 2, windy"`, `"a=b"`, `""`; all three in `attributeKeys()` |
| `structuralErrors` (data-driven) | one row per reason above | false; `getLastError()` equals the literal incl. line number; nothing published |
| `missingDataSection` | header only, no `$DATA` | false; `"Missing $DATA section"` |
| `headerOnlyWithDataMarker` | header + `$DATA`, no rows | true; `hasMeasurement("IMU","wx")`; `sourceMeasurement` empty |
| `malformedRowsAreSkipped` | good row; `rawDataLine("$IMU,4,1,2")` (short); `"$IMU,5,,1,0,62.5,40"` (empty field); `"$IMU,6,x,1,0,62.5,40"`; `"$NOPE,1,2"`; truncated last line `"$IMU,7,-12"`; second good row | true; two samples per IMU column with the literal values; exactly one summary warning (message-handler counter) |
| `isoTimestamps` | track fixture | `GNSS/time` [0] near 1704110400.0, [2] near 1704110400.4 (1e-6); a row with `2024-01-01T12:00:00.123Z` -> near 1704110400.123 |
| `fs1StillImports` | `Fs1FileBuilder` as in `tst_smoke::importFs1` | same literals; units stored verbatim, e.g. `sourceUnit("GNSS","hMSL") == "(m)"` **after Task 4.6** (until then `"m"` - mark `// PHASE4-SWITCH`) |
| `customColumnsAndSensors` | `$COL,FOO,time,bar#1,baz` / `$UNIT,FOO,s,furlongs,` + one row | measurements present with literal values |
| `crlfLineEndings` | sensor fixture with `.lineEnding("\r\n")` | identical results to `\n` |

**Acceptance Criteria:**
- [ ] `git grep -n "m_sensors\|friend class\|invalidateAllCalculations" -- src/dataimporter.cpp src/dataimporter.h src/sessiondata.h src/sessiondata.cpp` shows `m_sensors` only inside `SessionData`'s own members; no `friend`, no `invalidateAllCalculations` anywhere in `src` or `tests`.
- [ ] Every `return false` in `readFile` sets a non-empty `m_lastError`, and none of them is reachable after the first call that mutates the target `SessionData`.
- [ ] `tst_importer` passes; `tst_smoke` and all Phase 2-3 suites pass with **unchanged** expectations (the temporary SI pass keeps values and labels as before).
- [ ] The application imports a real `TRACK.CSV` / `SENSOR.CSV` pair and an FS1 file as before.

**Complexity:** L (natural split: staging + header grammar + `$VAR`; row policy + summary warning; publication + friendship removal; tests)

---

### Task 4.6: The switch - store as recorded, register the conversion layer, flip the pins

**Purpose:** Make the source layer truly "as recorded" and the effective layer the conversion output, in one atomic step, and update every expectation that deliberately changes.

**Files to modify:**
- `src/dataimporter.cpp` - delete the `// PHASE4-SWITCH` SI block and the `#include "units/unitconversion.h"` (baseline 6, 176-185, 209-226).
- `src/units/unitconversion.h` - delete `toSI` (73-85), now unused.
- `src/calculations/builtincalculations.cpp` - `registerBuiltInCalculations(registry)` calls `Calculations::registerSourceConversions(registry)` **first**, before the attribute calculations.
- `tests/tst_smoke.cpp`, `tests/support/builtinfixture.cpp` (golden table), `tests/tst_builtins_engine.cpp` (`inventory`), `tests/tst_session_engine.cpp`, `tests/tst_importer.cpp` (`// PHASE4-SWITCH` expectations).

**Technical Approach:**

- Conversion families live in their own ordered list, so their position among
  the built-ins does not affect candidate order; registering them first keeps
  `registeredIds()` readable: `builtin.conversion.schema`,
  `builtin.conversion.default`, `builtin.attr.analysisRange`, ...
  Python plugins still register before built-ins (overview decision); the
  bridge exposes no way to register a source conversion, so the layer is not
  extensible by plugins (spec 5).
- `mainwindow.cpp` needs no edit: startup already calls
  `registerBuiltInCalculations()` before any session exists (Phase 3 Task 3.9),
  so no broadcast occurs. `TestEnvironment::registerBuiltIns()` needs no edit.
- **`LogbookManager::loadSession` / `scanSessionFiles` need no edit.** They read
  saved CSVs through the same `readFile`. A file written by a released Viewer
  carries `m/s^2` / `T` / `degC` / `deg/s` labels, already-normalized values,
  and no `SCHEMA_VER`: those become the source, normalization is the identity
  (Task 4.2 rows), and the absent schema selects `builtin.conversion.default`,
  which corrects the gyro exactly once at read time. Nothing is rewritten on
  load, and because the exporter now writes the source layer (Task 4.3) a later
  save does not rescale or relabel it (acceptance 6; the byte-level guarantee
  is Phase 5). The mass/area/wind backfill (371-386) is untouched here
  (overview decision: Phase 6).
- **Consumers.** Everything that relied on stored values already being SI keeps
  working because ordinary reads are effective: plots and
  `UnitConverter` (display layer keyed by measurement type - untouched),
  `PlotWidget.cpp` 339, `builtin.imu.aTotal` / `wTotal`, `builtin.mag.total`,
  the time fit, logbook columns, `PluginSessionView`. `{sensor}/time` has unit
  `s` or `""` -> value identity -> the effective vector shares the source
  buffer; `builtin.time.utc.GNSS` passes `ctx.measurement("GNSS","time")`
  through, so `GNSS/_time` still shares that same buffer; the interpolation
  family reads shared vectors and allocates nothing. Task 4.7 pins these.
- **Known interim limitation (Phase 5 closes it):** `index.json` still caches
  gyro-dependent column values computed without the correction for unloaded
  sessions; they refresh when a session is loaded and recomputed, or wholesale
  once Phase 5 adds the calculation-compatibility marker.

**Expectations that flip (find them with `git grep -n "BASELINE:\|PHASE4-SWITCH" tests/`); use `qAbs(a - b) <= 1e-9` unless noted:**

| Test | Old | New |
|---|---|---|
| `tst_smoke::importFs2Sensor` | `IMU/wx` 62.5, `wy` -125, `wz` 0; `IMU/ax` 9.80665 with `getUnit` `m/s^2`; `MAG/x` 0.0001 `T`; temperature `degC` | effective near 71.68 / near -143.36 / `== 0.0`; `effectiveUnit("IMU","wx") == "deg/s"`; effective `IMU/ax` `== 9.80665`, `effectiveUnit` `"m/s^2"`, **`sourceMeasurement("IMU","ax") == {1.0}`, `sourceUnit == "g"`**; effective `MAG/x` `== 0.0001` `"T"`, source `{1.0}` / `"gauss"`; temperature 40 effective unit `"degC"`, source unit `"deg C"`; `sourceMeasurement("IMU","wx") == {62.5}`; `hasAttribute("SCHEMA_VER")` false (unchanged). Remove the two `BASELINE` comments. |
| `tst_smoke::derivedMeasurement` | `IMU/wTotal` 139.75424859373686 | near **160.28135262718** (= 139.75424859373686 x 1.14688; tolerance 1e-9); `hasMeasurement("IMU","wTotal")` still false; `sourceMeasurement("IMU","wTotal")` empty |
| `tst_smoke::exportReloadRoundTrip` | units `m/s^2`, `T`, `degC`, `deg/s` in the reloaded session | `sourceUnit` values `g`, `gauss`, `deg C`, `deg/s`; source samples equal within 1e-12; effective values of original and reloaded equal; file still has no `SCHEMA_VER`; second export still byte-identical |
| `tst_smoke::logbookSaveReload` | `IMU/wx` = 62.5 | effective near 71.68; `sourceMeasurement("IMU","wx") == {62.5}` |
| `goldenValues()` `IMU/wTotal` (`tst_builtins_golden`, `tst_builtins_engine`) | {5, 10, 15} | near {5.7344, 11.4688, 17.2032}; remove the `BASELINE` comment. `IMU/aTotal` {9.80665 x3} and `MAG/total` {0.0005 x3} keep their literals but are now produced through the conversion from recorded `g` / `gauss`. No other golden row depends on the gyro. |
| `tst_builtins_engine::inventory` | 67 ids + `builtin.interpolation` | the two `builtin.conversion.*` ids prepended |
| `tst_session_engine::derivedWTotalFollowsSource` | {5.0}, then {10.0}, then stored 42.0 | near {5.7344}, near {11.4688}; the file/programmatic `IMU/wTotal` stays **exactly 42.0** (only wx/wy/wz are in the schema table; a supplied `wTotal` is never corrected) |
| `tst_session_engine::interpolatedAttributeFollows` | `_M:IMU/_time/wx` 1.5, 15.0, 25.0 | near 1.72032, near 17.2032, near 28.672 (not marked `BASELINE` in Phase 3, but it reads `IMU/wx`) |
| `tst_importer::fs1StillImports` | `sourceUnit` `"m"` | `"(m)"` |

Nothing else in Phases 1-3's suites changes; if another test fails, investigate
rather than editing its expectation.

**Acceptance Criteria:**
- [ ] `git grep -n "UnitConversion\|unitconversion.h\|PHASE4-SWITCH" -- src/dataimporter.cpp tests` returns nothing; `git grep -n "toSI" -- src` returns nothing.
- [ ] `git grep -n "BASELINE:.*Phase 4" tests/` returns nothing; the Phase 6 `BASELINE` pins are untouched.
- [ ] `CalculationRegistry::instance().registeredIds()` starts with `builtin.conversion.schema`, `builtin.conversion.default` after startup.
- [ ] All suites pass with exactly the flips listed above.
- [ ] Application: a legacy `SENSOR.CSV` shows gyro rates 1.14688x the recorded numbers; the same file with `$VAR,SCHEMA_VER,2` added shows the recorded numbers; accelerometer / magnetometer / temperature plots are unchanged from Phase 3; the saved logbook CSV contains the recorded values and `g` / `gauss` / `deg C` labels and no `SCHEMA_VER` unless the file had one.

**Complexity:** M

---

### Task 4.7: Session-level acceptance suite for the source and conversion layers

**Purpose:** Demonstrate acceptance 1, 2, 4, 6 (load part), and 16 and the access / enumeration / performance rules of spec sections 4 and 5.3 on real `SessionData`, the real importer, and the global registry.

**Files to create:**
- `tests/tst_source_layer.cpp` (class `SourceLayerTest`)

**Files to modify:**
- `tests/CMakeLists.txt`; `tests/README.md` (list the four new targets).

**Technical Approach:**

`initTestCase`: `TestEnvironment::instance().registerBuiltIns()`. `init`:
`useFreshLogbook()`, `resetPreferencesToDefaults()`. Files are generated with
`Fs2FileBuilder` in `newTempDir()`. `near` = `qAbs(a-b) <= 1e-9`. Comparison
rule to state at the top of the file: `62.5 * 1.14688` evaluates to
`71.67999999999999`, which is **not** the double nearest `71.68`, so corrected
gyro values are always compared with the explicit `1e-9` absolute tolerance,
never with `==`; values whose conversion multiplies by exactly `1.0` inputs
(`1 g`, `1 gauss`, identities, zero) are compared with `==`.

| Test | Scenario | Expected |
|---|---|---|
| `unmarkedFileIsCorrected` (acc. 1) | import `Fixtures::sensorFile()` | effective `wx` near 71.68, `wy` near -143.36, `wz == 0.0`; `sourceMeasurement` `{62.5}`, `{-125.0}`, `{0.0}`; `sourceUnit == "deg/s"`; `effectiveUnit == "deg/s"`; `hasAttribute("SCHEMA_VER")` false and `attributeKeys()` lacks it **after** all reads |
| `schema2FileIsLiteral` (acc. 2) | same + `.var("SCHEMA_VER","2")` | effective exactly 62.5 / -125.0 / 0.0; attribute `"2"` |
| `unitNormalization` (acc. 4) | sensor fixture + custom sensor `$COL,FOO,time,bar` / `$UNIT,FOO,s,furlongs`, row `3,7` | `IMU/ax == 9.80665` `m/s^2`, source `{1.0}` `g`; `MAG/x == 0.0001`, `MAG/z == -0.00005` `T`, source `{1.0}` / `{-0.5}` `gauss`; `FOO/bar == 7.0`, `effectiveUnit == sourceUnit == "furlongs"`, zero warnings |
| `releasedLogbookFormatLoads` (acc. 6, load) | `LogbookManager::initialize()`; `saveSession` any session with id `rel`; overwrite the single `*.csv` in `sessionsDir()` with a released-format file: no `SCHEMA_VER`, `IMU` units `s,deg/s,deg/s,deg/s,m/s^2,degC`, row `3,62.5,-125,0,9.80665,40`, `MAG` unit `T` row `3,0.0001`; `flushIndex()`; `reopenLogbook()`; `initialize()`; `loadSession("rel")` | loaded; `ax == 9.80665` with `effectiveUnit == sourceUnit == "m/s^2"` and shared buffer; `MAG/x == 0.0001` `T`; temperature `degC`; gyro near 71.68 / -143.36 (corrected **once**: `runCount("builtin.conversion.default")` for `IMU/wx` is 1 and the value is not 82.2...); file bytes on disk unchanged by the load |
| `derivedWTotalUsesCorrectedGyro` (acc. 16) | sensor fixture | `IMU/wTotal` near 160.28135262718; `setMeasurement("IMU","wx",{0.0})` returns a set containing `IMU/wx` and `IMU/wTotal`; then `wTotal` near 143.36 |
| `interpolatedGyroAttribute` (acc. 16) | programmatic: TIME data of Phase 3 Task 3.12, `IMU/time` {10,20,30}, `IMU/wx` {1,2,3} unit `deg/s`, `_M` = 1704110415.0 | `_M:IMU/_time/wx` near 1.72032; `setSourceMeasurement("IMU","wx",{10,20,30},"deg/s")` -> near 17.2032 |
| `fileSuppliedWTotalWins` (acc. 16) | IMU `$COL` with an extra `wTotal` column (unit `deg/s`), value `99` | effective `IMU/wTotal == 99.0` exactly (not corrected - not in the schema table); `runCount("builtin.imu.wTotal") == 0`; `hasMeasurement` true |
| `schemaAttributeFlipsGyro` | imported unmarked session; read `wx`, `wTotal`, `ax`; `setAttribute("SCHEMA_VER","2")` | returned set contains `IMU/wx`, `IMU/wTotal`, not `IMU/ax`; `wx == 62.5`; `wTotal` near 139.75424859373686; `removeAttribute` -> near 71.68 again; `calculationEngine().verifyAgainstFresh({wx, wy, wz, wTotal, ax})` empty after each step |
| `enumerationIgnoresComputed` (spec 4) | sensor fixture; read `IMU/wTotal`, `IMU/aTotal`, `MAG/total`, `IMU/_time`, every effective column, `_START_TIME` | `sensorKeys()` == {`IMU`,`MAG`}; `measurementKeys("IMU")` == the six file columns; `hasMeasurement("IMU","wTotal")`, `hasAttribute("_START_TIME")` false |
| `sourceAccessNeverComputes` (spec 4) | fresh import, nothing read | after `sourceMeasurement`, `sourceUnit`, `hasSourceMeasurement`, `sourceData`, and all enumeration calls on every column: `calculationEngine().totalRunCount() == 0` and `cachedNodeCount() == 0`; `sourceMeasurement("IMU","wTotal")` empty, `sourceUnit` `""`, `hasSourceMeasurement` false - and still `totalRunCount() == 0` (no fallback to the derived value) |
| `lazyConversion` (spec 5.3) | fresh import | `totalRunCount() == 0` right after import; reading `IMU/ax` makes `runCount("builtin.conversion.default") == 1`; ten more reads leave it 1 |
| `identitySharesBuffer` (spec 5.3) | track fixture | for `GNSS/hMSL`, `GNSS/time`, and sensor-fixture `IMU/temperature` (`deg C`): `sourceMeasurement(...).constData() == getMeasurement(...).constData()` (both held in locals); `getMeasurement("GNSS","_time").constData()` equals the source `GNSS/time` pointer; for `IMU/ax` and `IMU/wx` (unmarked) the pointers differ |
| `exporterWritesSource` | import sensor fixture, read every effective column (warm), `DataExporter::exportSession` | file contains `$UNIT,IMU,s,deg/s,deg/s,deg/s,g,deg C`-style recorded labels in exporter column order and the recorded numbers `62.5`, `-125`, `1`; contains neither `71.6` nor `9.80665`; no `SCHEMA_VER` line |
| `mergeCopiesSource` | `SessionModel::mergeSessions({track})`, then `mergeSessions({sensor})` (loaded row) | row's `sourceMeasurement("IMU","ax") == {1.0}`, `sourceUnit == "g"`, effective `wx` near 71.68 (not 82.2...) |

**Acceptance Criteria:**
- [ ] The target is registered with `flysight_add_test`, passes on Windows Release, and acceptance numbers 1, 2, 4, 6, 16 each appear in a comment on a test function (3 is in `tst_importer`).
- [ ] Every expected value is a literal; the only computed comparison is `verifyAgainstFresh`.
- [ ] Temporarily swapping the registration order of the two conversion families makes `schema2FileIsLiteral` fail; temporarily deleting the `m/s^2` row of Task 4.2 does **not** change `releasedLogbookFormatLoads` values (unknown = identity) - both checked once by hand, not committed.
- [ ] Nothing is written outside `TestEnvironment::rootPath()`.

**Complexity:** L

---

## Testing Requirements

### Unit Tests
- New targets: `tst_schema_units` (4.1-4.2), `tst_conversion_engine` (4.4), `tst_importer` (4.5), `tst_source_layer` (4.7).
- Deliberately updated: `tst_smoke` (`importFs2Sensor`, `derivedMeasurement`, `exportReloadRoundTrip`, `logbookSaveReload`), `goldenValues()` `IMU/wTotal` row (serves `tst_builtins_golden` and `tst_builtins_engine`), `tst_builtins_engine::inventory`, `tst_session_engine::derivedWTotalFollowsSource` and `::interpolatedAttributeFollows` - exact new literals in Task 4.6. Test support: `DescentFixture::load`, `copyStoredState` (Task 4.3).
- All other Phase 1-3 tests pass unchanged. A test that asserts effective values must call `TestEnvironment::registerBuiltIns()`; without it no conversion family exists and effective == source.

### Integration Tests
- After every task: build with `-DFLYSIGHT_BUILD_TESTS=ON`, `ctest --test-dir build/FlySightViewer-build -C Release --output-on-failure` all green; after 4.3, 4.5, 4.6 also build with the option OFF.
- Grep criteria of Tasks 4.1, 4.3, 4.5, 4.6 are part of the check ("one authority per fact", no friend access, no import-time conversion).

### Manual Verification
1. Import a legacy `TRACK.CSV` + `SENSOR.CSV` pair: gyro plots read about 14.7 % higher than in the Phase 3 build; acceleration, magnetometer, temperature, pressure, and all GNSS plots are identical; no dialog, badge, or log warning appears.
2. Open `logbook/sessions/<uuid>.csv` for that session: `$UNIT,IMU` shows `g` / `deg C`, the numbers match the device file, there is no `SCHEMA_VER` line.
3. Copy the `SENSOR.CSV`, add `$VAR,SCHEMA_VER,2` after `$FLYS,1`, import it under a different `SESSION_ID`: gyro plots show the recorded numbers.
4. Change it to `SCHEMA_VER,3` and import: the import fails (the dialog still lists only the file name until Phase 6; the log shows `Unsupported SCHEMA_VER '3' (supported: 1, 2)`); the logbook is unchanged.
5. Restart with an existing logbook from the released version: sessions open, accelerometer / magnetometer values are unchanged, gyro is corrected once; edit a description and let it save; reopen - gyro values are the same as before the save.
6. Edit a session description to contain a comma, restart: the full description survives.
7. Import an FS1 file and a GNSS-only FS2 file.

## Notes for Implementer

### Gotchas
- **Write the factor as `1.14688`.** `0.070 / (2000.0 / 32768.0)` is `1.1468800000000001` in double.
- **Never touch the shared source vector with a non-const call** in the conversion kernel; one stray `operator[]` on a non-const `QVector` detaches and silently doubles memory for every identity column. The pointer-equality tests catch it.
- **`-0.0`:** skip the unit step entirely when it is the identity; `v * 1.0 + 0.0` would turn `-0.0` into `+0.0`.
- **Resolution never falls through** once conversion families exist: a source column for which no conversion instance can be built is unavailable, not derived. Hence the `#` / `%` escaping of instance keys.
- **Input order of `builtin.conversion.schema`** is source, unit, attribute; tests pin dependency sets produced by the short-circuiting availability pass.
- **`Resolution(attr SCHEMA_VER)` is an ordinary attribute resolution.** Do not register any calculation that outputs `SCHEMA_VER`, and do not add a "resolved schema" attribute: absence is interpreted inside `builtin.conversion.default` only (spec 5.1, 3.2).
- **Source accessors must not create the engine.** They are called on hundreds of temporary sessions during scans.
- **`mergeSourceData` on a session with a warm engine** issues two notifications per column; that is fine (invalidation never computes) - do not add an engine batch API.
- **`setUnit` semantics changed** (no-op without source data). The importer publishes through `mergeSourceData`, so it never hits this.
- `QTextStream::readLine` already strips `\r\n`; do not trim data fields or `$VAR` values.
- A `$VAR` value is kept verbatim but the exporter still writes it with `QVariant::toString()` and no escaping; newline-free by construction. Exact attribute formatting is Phase 5.
- `m_lastError` must also be cleared in `importFile`, which returns early from `readFile`.
- Two copies of `flysight_model` statics exist (executable and `.pyd`, Phase 3); the conversion code lives in `flysight_core` and is never linked into the bridge.
- Tests that count warnings must install their message handler after `registerBuiltIns()`.

### Decisions Made
- **Accessor names:** effective = `getMeasurement` + new `effectiveUnit`; source = the existing public `ISessionState` overrides `sourceMeasurement` / `sourceUnit` / `hasSourceMeasurement`, plus `sourceData`, `setSourceMeasurement`, `mergeSourceData`. `getUnit` and the caller-less `units` are deleted rather than renamed, so no accessor with ambiguous layer remains. `setMeasurement` / `setUnit` keep their names (a mutator can only address the source layer) to avoid churn in Phase 3's tests.
- **Absence is reported by value:** empty vector / empty string / `false`; no `std::optional` API, matching the engine's availability definition.
- **`effectiveUnit` of a derived measurement** = the unit its calculation reported, `""` for all current built-ins (same as baseline's `getUnit`).
- **Two conversion families, ordered `builtin.conversion.schema` then `builtin.conversion.default`;** the first matches only schema-dependent names, so only gyro columns depend on `SCHEMA_VER`.
- **Schema table location:** `src/conversion/schematable.*` (`flysight_core`), namespace `FlySight::Schema`; the importer and both families call it; the factor and the key string each appear once.
- **Invalid `SCHEMA_VER` held in memory** (programmatic `setAttribute` only - import and logbook load reject it): the schema family wins, applies **no** correction, and warns once per evaluation. With all-required inputs and first-available-wins resolution, an "unavailable" answer from the schema family would fall through to the default family and silently apply the *legacy* correction, which is the worst outcome; making the name unavailable would need a new "blocking result" concept in the engine, not justified for an unreachable state.
- **Unit table stays in `src/units/unitconversion.h`**, gains `m/s^2` / `T` / `degC` identity rows, loses its warning and `toSI`. Lookup trims; stored unit text is never trimmed.
- **Importer error policy:** header problems are structural errors (list in Task 4.5) and reject the file; data-row problems skip the row and are summarized in one warning; zero rows is valid; missing `$DATA` is an error.
- **`$VAR` parsing:** value = remainder of the line after the second comma, verbatim; missing value = `""`; empty key and conflicting duplicates are errors.
- **FS1 unit text stored verbatim** (`(m)`), normalized by the table at read time.
- **Minimal exporter switch** = three call sites (`sourceUnit`, `sourceMeasurement` x2); **minimal merge fix** = `storedAttribute` + `mergeSourceData(sourceData())`.
- **`invalidateAllCalculations()` removed** with the friendship: nothing writes around the mutation path any more (spec 12: one mutation path).
- **Side-by-side then switch** (as in Phase 3): Tasks 4.1-4.5 keep all pins; Task 4.6 flips them together with the behavior.

### Open Questions
- If Michael prefers gyro data to be **unavailable** for an in-memory unsupported `SCHEMA_VER`, the engine needs a small addition (a result that claims an output as unavailable and stops candidate iteration). Not needed for any acceptance item; current decision documented above.
- Header strictness (duplicate `$COL`, extra `$UNIT` fields, missing `$DATA` now reject the file) is believed never to occur in firmware- or Viewer-written files. If a real-world file trips one of these, downgrade that rule to "ignore + summary warning" rather than weakening `SCHEMA_VER` validation.

## Definition of Done

This phase is complete when:
1. All seven tasks have passing acceptance criteria.
2. `tst_schema_units`, `tst_conversion_engine`, `tst_importer`, `tst_source_layer`, and every Phase 1-3 suite (with exactly the flips listed in Task 4.6) pass via CTest on Windows Release; the application and `flysight_cpp_bridge` build with `FLYSIGHT_BUILD_TESTS` ON and OFF.
3. Spec coverage: 3.1 / 3.2 - Tasks 4.5, 4.6 (`neverStampsSchema`, source retention); 3.3 / 4 - Tasks 4.3, 4.7 (`enumerationIgnoresComputed`, `sourceAccessNeverComputes`); 5, 5.1, 5.2 - Tasks 4.1, 4.2, 4.4; 5.3 - `lazyConversion`, `identitySharesBuffer`; 6.1 - Task 4.5; 9.3 - `releasedLogbookFormatLoads`; 12 - grep criteria of 4.1, 4.5, 4.6; acceptance 1, 2, 4, 6 (load), 16 - Task 4.7; acceptance 3 (rejection) - Task 4.5.
4. No import-time conversion, no `friend class DataImporter`, no `getUnit`, no schema stamping, and no second copy of the schema factor exist in `src`.
5. Code follows the patterns in the reference files; no TODOs, `PHASE4-SWITCH` markers, or placeholder code remain; nothing has been pushed.
