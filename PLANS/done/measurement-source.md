> Historical proposal, superseded by final-measurement-plan.md, measurement-contracts.md and docs/DATA_SCHEMA.md. References below to the local import-time correction prototype are historical; do not restore schema stamping or destructive normalization.

# Measurement sources and calculated representations

Status: proposal for independent review; not an implementation instruction.

Date: 2026-09-17

## 1. Purpose and review context

Preserve imported measurement sources while allowing FlySight Viewer to change
how it interprets those measurements after import. Use the existing calculated
value and dependency machinery wherever it fits, and keep the familiar
measurement identifiers used by plots, calculations, and plugins.

The motivating workflow is an experiment involving approximately a hundred
FlySights. A user imports and organizes many recordings, then wants to enable or
disable a measurement correction for those sessions. Deleting and reimporting
the recordings must not be necessary, and descriptions, markers, organization,
and other session edits must survive the change.

This document is intended to give a fresh reviewer the problem, constraints,
proposed architecture, and unresolved questions without requiring the preceding
conversation. It distinguishes established requirements from implementation
suggestions. Reviewers should challenge the proposed architecture rather than
assume the illustrative APIs or resolution order are already settled.

Only this specification is being requested at this stage. Do not implement the
proposal as a side effect of reviewing it.

## 2. Background: gyro conversion and the local prototype

Approximately 4,000 FlySight units are in the field. An investigation found that
legacy FlySight 2 firmware configured the LSM6DSO gyroscope at +/-2000 degrees/s
but converted raw counts using `2000 / 32768` degrees/s/count. The sensor's nominal
sensitivity at that range is `0.070` degrees/s/count. The nominal correction to
legacy CSV angular rates is therefore:

```text
0.070 / (2000 / 32768) = 1.14688
```

The correction applies to `IMU.wx`, `IMU.wy`, and `IMU.wz`. It is not an
accelerometer correction or a per-device calibration. It cannot recover values
lost to rounding, overflow, or clipping. Physical validation remains part of the
firmware investigation.

The firmware should write correct values directly and mark the applicable
logical data schema. The agreed header field is `SCHEMA_VER`:

| Header | Meaning |
| --- | --- |
| `$FLYS,1` | Existing file encoding/header syntax. |
| `$VAR,FIRMWARE_VER,...` | Firmware that originally recorded the measurements. |
| Missing `SCHEMA_VER`, or explicit `1` | Legacy measurement representation, including the gyro scaling error. |
| `$VAR,SCHEMA_VER,2` | Corrected gyro representation; other measurement semantics are unchanged. |

Schema versions describe the meaning of stored values, independently of firmware
release names. Detection should not depend on production/development release
cutoffs, filenames, recording dates, or inferred motion. The official numeric fix
and schema marker must ship together. A separate `APPLIED_CORRECTIONS` field is
not required to interpret the file.

The current local prototype corrects values during import, applies existing unit
normalization in place, and marks the resulting session as schema 2. Its Import
preferences select Automatic, Treat as legacy, or Treat as corrected. Those
choices are not revisitable through a session-level control. Existing legacy
logbook files are also corrected when read, but that does not solve the broader
problem of preserving and revisiting interpretation decisions.

This proposal replaces that conversion boundary. The prototype is local and
unreleased. Do not design a deployed-data migration framework around its
intermediate behavior. Use original recordings when validating the replacement;
do not manufacture originals by dividing already modified local data. No
information can be recovered from an overwritten source alone, but supporting
such prototype artifacts is not a product requirement here.

## 3. Established requirements

### 3.1 Source preservation and naming

- Keep one persistent source representation of each imported measurement.
- Preserve the direct relationship between file labels and the in-memory
  property/measurement structure. Import must not rename every column to
  `wx_raw`, `source:wx`, or an equivalent alternative identifier.
- Do not persist a complete second corrected dataset alongside the source.
- Calculations may create cached representations in memory, without modifying
  their source vectors.
- Ordinary consumers should continue to request familiar identifiers such as
  `IMU.wx` and `GNSS.velD` and obtain the representation appropriate for use in
  Viewer. Qualified source access may be introduced without relabelling storage.
- Reading a source for calculation or changing a correction setting must not
  change the source's schema marker or recorded units.

Here, "source" means measurements from the input file, not the sensor's ADC or
register counts. Source immutability applies to interpretation and calculation;
it does not prohibit an explicit import/merge operation from replacing source
input through a controlled mutation path.

### 3.2 Session behavior

- Users must be able to change the correction choice after import, including for
  multiple selected sessions, without deleting/reimporting or losing session
  organization and annotations.
- An Import preference is a global policy for future imports, not a place for
  per-session facts or controls.
- A changed global default must not silently reinterpret sessions already in
  the logbook.
- Existing session attributes and calculated-value defaults should be used
  where suitable, rather than introducing a parallel preferences/caching system.
- Normal legacy handling should not produce pop-ups, import notifications,
  warning badges, or an icon on every affected plot. The behavior should be
  discoverable in an appropriate setting and documentation.

### 3.3 Persistence

- For this proposal, saving/exporting means writing source measurements with
  the applicable source metadata and modified session attributes.
- Writing a file must preserve the agreement between its numeric measurements,
  units, and `SCHEMA_VER`.
- Evaluating a corrected representation must not cause that representation to
  replace the source on the next save.
- There is no user-facing corrected export feature today. Adding one is outside
  this change's scope.

The user observed that either original values with the original schema or
corrected values with an updated schema could be coherent file outputs. The
proposed behavior here is source preservation. Materializing corrected values
would be a separate, explicit transformation if introduced later.

## 4. Relevant existing mechanisms

These observations describe the code inspected during this discussion. The
working tree contains local changes; inspect the actual files before implementing.

### 4.1 Session storage and lookup

`SessionData` stores measurements in a map keyed by sensor and measurement name,
and stores unit strings separately. File `$COL` labels populate these keys
directly. Session attributes are held in a separate map.

`getAttribute(key)` first returns an explicitly stored value and otherwise tries
a calculated attribute. `getMeasurement(sensor, key)` currently does the same
for measurements: a stored vector wins over a registered calculation.

`sensorKeys()`, `measurementKeys()`, `hasMeasurement()`, and `attributeKeys()`
currently describe stored data, not all potentially available calculations.
Changing normal lookup must not accidentally change the meaning of enumeration
or presence checks without auditing callers.

### 4.2 Calculation and invalidation

The calculated-value system supports registered recipes, cached results,
dependency checking, and cycle detection. Successful recipes register concrete
reverse dependencies for the session. Explicit attribute/measurement setters
invalidate the changed key and its transitive dependents.

This already provides most of the desired lifecycle: calculate on demand, cache,
invalidate when an input setting changes, and recalculate on the next request.
It does not automatically invent dependencies for code that reads undeclared
inputs. In particular, `setUnit()` currently changes metadata without emitting
dependency invalidation.

### 4.3 Automatically synthesized references

An existing attribute lookup can synthesize an interpolated measurement at the
time supplied by another attribute. Its syntax is:

```text
{timeAttribute}:{sensor}/{timeVector}/{measurement}

Example: _EXIT_TIME:IMU/_time/wx
```

`synthesizeInterpolation()` resolves this on demand, caches it, and registers
dependencies. The current parser splits at the first colon and then expects
exactly three slash-separated components.

This is a precedent for resolving structured references automatically. It is
not itself a general representation-selection framework.

### 4.4 Persistence and display caches

`DataExporter::exportSession()` is currently called by `LogbookManager` to save
sessions. It enumerates stored keys but obtains vectors with `getMeasurement()`
and units with `getUnit()`. That combination would become unsafe if those
accessors begin returning effective representations: source saving must be made
explicit before or together with that change.

Logbook column values are also cached in `index.json`, outside the session's
calculated-value caches. Clearing an in-memory measurement cache alone does not
update those persisted values. Session edits must use the model's notification,
recomputation, and save paths so all affected displays and caches stay consistent.

## 5. Proposed conceptual model

Keep the stored measurement name unchanged and distinguish its representation
when resolving a request.

```text
Stored source:              sensor=IMU, measurement=wx
Cached effective value:     sensor=IMU, measurement=wx
```

These are the same logical measurement with different roles, not two imported
column names. The source is persistent input; the effective value is disposable
calculation output.

An illustrative API is:

```cpp
session.getMeasurement("IMU", "wx");        // Effective representation
session.getSourceMeasurement("IMU", "wx");  // Explicit stored source
```

An accessor with a representation argument is equally possible. Neither API is
settled. Source access must bypass calculation and must not fall back to a
derived value when no source exists. For example, `wTotal` normally has no
source vector to retrieve.

The user suggested that several versions might exist behind a logical name and
the highest-priority version could be returned. That idea is under review. The
assistant's narrower recommendation is a deterministic resolution policy for
source-backed values, integrated with existing derived recipes, rather than
arbitrary numeric priorities or an extensible transformation language.

## 6. Source schema and correction choice are separate

### 6.1 Source schema

Keep an explicit `SCHEMA_VER` from the file as a stored attribute. If absent,
register a calculated default returning legacy schema 1. Existing stored-value
precedence then gives the desired behavior without inserting a synthetic header
attribute on import.

Consequences:

- `getAttribute("SCHEMA_VER")` can return `1` for an unmarked file.
- The stored attribute map can still report that no marker was present.
- Computing that default must not cause it to be serialized as an explicitly
  recorded attribute.
- Setting/removing an explicit value follows the existing invalidation path.
- Source-preserving save can retain the absence of the original marker.

`SCHEMA_VER` must not be changed merely to enable or disable the Viewer
correction. It describes the source representation.

### 6.2 Session correction choice

The recommended initial model is a session attribute expressing whether schema
corrections are enabled. Its name, exact UI wording, and whether it should be
gyro-specific or general-purpose remain review decisions.

Recommended behavior:

| Source schema | Corrections enabled | Effective gyro values |
| --- | --- | --- |
| Missing / 1 | Yes | Source gyro values multiplied by 1.14688, in internal units. |
| Missing / 1 | No | Source gyro scaling retained, in internal units. |
| 2 | Yes | Source gyro values in internal units; no legacy correction. |
| 2 | No | Same as above; this does not undo a firmware fix. |

The normal default is enabled. An import preference can supply the initial
session choice. A saved session choice wins on reload; changing Import
preferences later must not affect it. A calculated default can cover sessions
without an explicit choice, but should not consult a mutable global preference
on each evaluation and thereby reinterpret existing sessions unexpectedly.

The prototype's three-way selector answers a different question: whether to
trust the declared source interpretation. Disabling correction and declaring
an incorrectly labelled custom file to be schema 2 are not the same operation.
A rare escape hatch for mislabelled/custom data remains desirable, but its
representation must be reviewed separately. Do not silently repurpose the
source schema as an on/off switch or silently discard its original declaration.

### 6.3 Unknown schemas

An unknown or malformed schema must not silently be treated as legacy or relabelled
as schema 2. Decide whether to reject the file or retain its source while marking
affected effective measurements unavailable. Retaining a source and understanding
all of its physical semantics are separate capabilities.

Do not import a schema-2-or-higher rule based solely on numeric ordering; a future
schema's meaning must be understood before applying its transformations.

## 7. Measurement resolution and unit conversion

### 7.1 Recommended resolution behavior

For an ordinary measurement request:

1. Return its cached effective result, if present and valid.
2. If there is source data, resolve the applicable source-to-effective
   transformation and cache its result.
3. Otherwise, use an existing registered derived calculation, such as `wTotal`.

This is a suggested default, not a complete policy for every existing plugin or
override case. The implementation review must establish how explicitly supplied
derived measurements and multiple registered recipes retain their intended
precedence. Do not globally reverse stored/calculated precedence for attributes.

For a source-backed value, a recipe must access the source directly rather than
call the ordinary getter for its own output and recurse. Identity conversion is
a successful transformation. A failed required conversion is not equivalent to
identity: returning an uncorrected source as if it were effective data would hide
the failure.

The common source-to-effective calculation should handle arbitrary imported
fields without handwritten registration for every sensor column. The means of
doing that—generic fallback, reusable recipes, or another small resolver
extension—is open. Avoid capturing per-session units or values in the globally
shared recipe registry, and avoid adding duplicate recipes on every import.

### 7.2 Units

The user considers existing unit conversion unambiguous and lower priority than
making schema corrections reversible. The recommended complete model nevertheless
preserves source numbers and source unit metadata, and performs unit normalization
in the calculated representation as well. This gives source saving a coherent
meaning and avoids maintaining two definitions of source across different fields.

Examples of current internal conventions:

- `IMU.ax`: g in firmware CSV, m/s^2 internally.
- Magnetometer fields: gauss in firmware CSV, tesla internally.
- Gyro fields on `master`: degrees/s remain degrees/s. Do not change these to
  radians/s as an incidental part of this work.
- Existing angle and temperature conventions likewise remain as established by
  Viewer; "internal units" does not mean forcing every field into strict SI.

Disabling schema corrections should not disable unit normalization for ordinary
measurement access. Otherwise downstream calculations could receive g or m/s^2
under the same key depending on the session's correction setting.

Source-unit access and effective-unit access must be explicit and consistent
with their respective values. A common implementation may combine normalization
and gyro rescaling in one vector calculation without caching intermediate stages.
The current gyro multiplier commutes with its angular-rate unit conversion, but
future transformations may involve offsets or other order-sensitive operations;
their order must be defined rather than inferred from arbitrary priority numbers.

A smaller first implementation that retains today's unit normalization would
need a clearly narrower definition of source. That is a review option, not the
same guarantee as preserving the file's recorded numbers and units.

### 7.3 Memory

Keep one persistent source dataset. Effective vectors are cached only as requested.
Do not eagerly calculate every column or every possible representation on import.
Where no numeric transformation is necessary, Qt's implicitly shared vectors can
avoid allocating a second sample buffer. Transformed vectors do require storage
while cached; source preservation does not promise zero additional RAM.

Review cache lifetime within a loaded session as well as existing session eviction.
No new general cache-eviction framework is required without demonstrated need.

## 8. Dependency identities and invalidation

Source and effective measurements with the same sensor/name need distinct
dependency identities. An illustrative graph is:

```text
Source(IMU, wx) -----------+
SourceUnits(IMU, wx) ------+--> Effective(IMU, wx) --> IMU.wTotal --> consumers
SCHEMA_VER ---------------+
Correction setting -------+
```

A representation field or a separate source-measurement dependency kind is one
possible implementation. Exact C++ types are open. The requirements are:

- No self-cycle between source `wx` and effective `wx`.
- Ordinary existing measurement dependencies refer to effective values.
- Source replacement invalidates effective values and downstream calculations.
- Correction-setting changes invalidate affected effective values and their
  dependents, without changing the source.
- Schema changes/removal invalidate calculations that use the interpreted schema.
- Unit metadata changes invalidate effective values if such changes are allowed.
- Identity/pass-through results also register dependencies where their behavior
  could change with a later setting or source update.
- Cached interpolation attributes depending on an affected effective measurement
  participate in the same transitive invalidation.

Use the existing invalidation machinery rather than maintaining a second
independent cache with its own stale-data rules. Route user edits through model
mutation/notification paths so plots, calculated logbook columns, persisted index
values, and session saves are updated together.

The prototype's global `dataSchemaVersion` index marker was introduced to discard
cached columns computed before gyro normalization. Under this proposal, source
schema is not a complete cache-validity indicator: two sessions with schema 1 can
have different correction settings. Review index invalidation accordingly. This
need not imply a new per-session cache metadata format if existing edit/recompute
and persistence paths already establish validity.

## 9. Reference syntax and collision avoidance

Storage labels should remain unmodified. Earlier suggestions to persist `_raw`
counterparts or store everything under a `source:` prefix were superseded because
they obscure the direct mapping from file labels and may collide with real names.

Explicit source access could be exposed in code through a separate accessor or
representation argument, requiring no string syntax change. If plugins or
interpolation expressions need a serialized source reference, a reserved qualifier
may be useful strictly as an access notation.

For example, `source:wx` was discussed. It could fit as a measurement component in:

```text
_EXIT_TIME:IMU/_time/source:wx
```

By contrast, `source/wx` adds a fourth slash-separated component and is rejected
by the current interpolation parser. Neither notation has been selected. Any
extension must consider qualifiers on both the time vector and the value vector,
and must keep existing unqualified expressions working.

Do not claim collision freedom just because punctuation was chosen. Current
import parsing does not enforce a reserved identifier namespace. Decide whether
validation, escaping, or a structured reference API is needed before exposing
new string syntax. There is no requirement to build a general expression language.

## 10. Save, reload, and semantic round trips

The writer must enumerate source fields and explicitly retrieve source values
and source units. It must not accidentally invoke ordinary effective getters.
Derived caches such as corrected `wx` or calculated `wTotal` are not added to the
file merely because they were evaluated.

Persist modified session attributes alongside source metadata, including the
chosen correction policy where necessary. Preserve `FIRMWARE_VER`. Preserve the
source `SCHEMA_VER` or its absence; do not serialize the calculated default as
though it had been read from the file.

Example sequence:

1. Read an unmarked legacy recording whose `wx` sample is `62.5` degrees/s.
2. The calculated schema default is 1; corrections are enabled.
3. Ordinary `wx` evaluates to `71.68` degrees/s.
4. Save the session: the source sample is still `62.5`, the original schema
   marker is still absent, and the session correction choice is preserved.
5. Reload: source remains `62.5`; ordinary `wx` evaluates to `71.68` once.
6. Disable correction: ordinary `wx` becomes `62.5`; source never changed.
7. Save/reload/re-enable: the same original input is used, without repeated
   rescaling, division, or accumulated transformation history.

For schema 2, both correction settings leave gyro scale unchanged. For example,
source `71.68` remains `71.68`; disabling correction does not reconstruct legacy
values that were never present in that recording.

### 10.1 Meaning of "identical to the original"

The intended minimum is a semantic round trip of source measurements, units,
labels, and source schema, subject to deliberately modified session attributes.
The conversation used an identical output file as an example, not a settled
requirement for byte-for-byte archival reproduction.

The current parser converts numeric text to doubles and timestamps to numeric
values; the writer reorders headers/records and formats numbers with 15 significant
digits. Source immutability in memory alone does not guarantee exact textual or
even lossless floating-point round trips. Review numeric output precision,
timestamp serialization, and retained source metadata. Use sufficient precision
to round-trip stored doubles; do not introduce fresh rounding on every save.

If byte identity, original ordering, original numeric spelling, or preservation
of currently unrecognized records is required, additional lexical information
would have to be retained. That stronger requirement remains open and should not
be silently assumed or claimed by an implementation using the existing parser.

## 11. Session merging and other integration points

FlySight sessions may combine TRACK.CSV and SENSOR.CSV. Existing merging copies
attributes and measurements into a session. Source preservation requires reviewing
this boundary rather than assuming one session always comes from one file.

In particular, an unmarked track file merged with schema-2 sensor data must not
cause the gyro to be treated as legacy. Conversely, a schema-2 track file must not
relabel legacy sensor values as corrected. GNSS semantics happen to be unchanged
between schemas 1 and 2, but a last-writer-wins schema attribute is not a sufficient
general merge rule. Review whether source schema context belongs per input,
per sensor, or whether compatible inputs can be reconciled into one truthful
session schema. Save/reload behavior must match that decision.

Also audit:

- Source unit metadata when merging or replacing individual sensors.
- Existing manual session attributes and which incoming attributes may replace
  them on an explicit reimport.
- Plugins/bindings that enumerate measurements, inspect stored presence, or call
  `setMeasurement()` with values already in internal units. Define source-setting
  semantics explicitly; do not accidentally normalize plugin results twice.
- Existing recipes for a key that can also have a stored value. Preserve intended
  overrides or document deliberate changes instead of reversing all precedence.
- GNSS-only and original FlySight-format recordings, unknown columns, empty
  measurements, and sessions with incomplete sensors.
- Batch edits to unloaded sessions: source and session state must be loaded,
  changed, saved, and evicted through the existing model lifecycle as appropriate.

## 12. User-facing controls

The need for a session control is now established by the post-import editing
workflow. Earlier in the discussion, an unsolicited "Session details" panel and
gyro plot badges were rejected; there is no existing session-details panel to
reuse by that name.

The exact control is open. A logbook command for selected sessions, or an existing
editable-attribute/batch-edit mechanism, are candidates. It must support mixed
selections and communicate the setting being applied without pretending every
selected session originally had the same value.

Keep global Import defaults separate from that control. Do not make ordinary
track viewing announce a routine gyro compatibility adjustment. Documentation
and the controls themselves provide discoverability.

Changing a session choice must preserve all unrelated session attributes and
organization. Define how the choice is saved and how its missing/default state
is represented. A user's choice must survive eviction, restart, and subsequent
changes to global Import preferences.

## 13. Implementation areas to inspect

| Files | Role in review |
| --- | --- |
| `src/dataimporter.{h,cpp}` | Direct file mapping; current in-place gyro and unit conversions; schema stamping. |
| `src/sessiondata.{h,cpp}` | Stored/source access, ordinary resolution, units, enumeration, attribute defaults, interpolation. |
| `src/calculatedvalue.{h,cpp}` | Recipes, caching, dependency registration, cycle handling. |
| `src/dependencykey.h`, `src/dependencymanager.{h,cpp}` | Distinct source/effective identities and transitive invalidation. |
| `src/calculations/` | Default schema/policy recipes, existing measurement consumers and derived calculations. |
| `src/units/unitconversion.h` | Current conversion definitions; preserve internal conventions. |
| `src/dataexporter.{h,cpp}` | Source-only writing and precise round trips. |
| `src/logbookmanager.{h,cpp}`, `src/sessionmodel.{h,cpp}` | Reloads, merges, batch edits, notifications, index cache and persistence. |
| `src/preferences/importsettingspage.{h,cpp}`, `src/mainwindow.cpp` | Import default and current prototype control. |
| `src/sessiondata_bindings.cpp`, `src/pluginhost.cpp`, `python_plugins/flysight_plugin_sdk.py` | Plugin API compatibility and synthesized references. |
| `tests/` | Current prototype regression suite; expectations must change for preserved sources. |

Related documents:

- [Sensor fusion investigation](sensor-fusion.md).
- [Firmware hand-off](../TEMP/firmware-changes.md): the firmware fix and
  `SCHEMA_VER,2` output contract remain relevant. Its description of Viewer
  normalizing stored/exported data reflects the earlier prototype and would need
  updating if this proposal is adopted.
- [Current prototype data-schema documentation](../docs/DATA_SCHEMA.md): describes
  the earlier import-time conversion behavior, not this proposed architecture.

No code changes, documentation rewrites outside this specification, or firmware
changes are authorized merely by this review request.

## 14. Acceptance scenarios for a later implementation

These should become focused tests of observable behavior, not tests that merely
repeat the implementation's formulas or inspect its private map layout.

1. **Missing-schema default:** Reading an unmarked file leaves the stored marker
   absent; calculated lookup returns 1. Explicit schema 2 overrides the default.
   Removing an explicit value restores the default and invalidates dependents.
2. **Source preservation:** Source gyro samples, source accelerometer samples,
   labels, and units stay unchanged after reading effective values repeatedly,
   changing settings, and computing downstream quantities.
3. **Known numeric case:** Legacy `wx=62.5`, `wy=-125`, `wz=0` corresponds to
   effective `71.68`, `-143.36`, `0` degrees/s with correction enabled. Only gyro
   components receive this schema correction.
4. **Schema-2 identity:** Correctly recorded gyro values are not corrected again,
   regardless of cache warm-up, save/reload, or correction toggles.
5. **Post-import toggle:** Enable/disable/re-enable a legacy session correction;
   effective measurements, `wTotal`, and interpolated attributes update without
   changing source inputs or unrelated session attributes.
6. **Unit stability:** With full source-unit preservation, source acceleration
   `1 g` remains `1 g`; ordinary acceleration is `9.80665 m/s^2` in either
   correction mode. Identity conversions report consistent units as well.
7. **No source/effective cycle:** A source-backed recipe resolves its source and
   registers distinct dependencies. Derived-only values cannot masquerade as
   stored source when explicitly queried.
8. **Persistence independence:** Warm and cold calculation caches produce the same
   source file content/meaning. Repeated save/reload does not accumulate scaling
   or numeric rounding, insert default schema headers, or serialize derived vectors.
9. **Import default isolation:** Changing a global preference affects new imports
   but not existing session choices or reloads of saved sessions.
10. **Batch editing:** Change a hundred selected sessions, including unloaded and
    mixed-schema sessions. Their chosen policy persists, dependent displays/index
    values refresh, and organization, markers, and descriptions remain intact.
11. **Source replacement:** Replace/merge source data and unit metadata after a
    cache was populated; dependent effective values recompute correctly.
12. **Mixed input order:** Merge compatible track/sensor files in either order,
    including unmarked track plus schema-2 IMU and the converse. Gyro interpretation
    and source-preserving save/reload do not depend on merge order.
13. **Unsupported interpretation:** An unknown schema or failed required
    transformation follows the chosen explicit failure policy and is never
    silently promoted, corrected as legacy, or passed off as effective data.
14. **API compatibility:** Existing plots, derived recipes, interpolation keys,
    and supported plugins continue to resolve ordinary names. Enumeration does
    not acquire duplicate `_raw`/`source:` fields.
15. **Quiet operation:** Routine legacy import/viewing produces no correction
    dialogs, notifications, or per-plot badges. The session control supports mixed
    selection without displaying a false uniform state.

## 15. Questions for the independent reviewer

1. Is a source/effective distinction in measurement resolution the smallest
   coherent extension of the current model, or is a limited prioritized-recipe
   mechanism a better fit? Which existing precedence behaviors must be retained?
2. Should explicit source access be a separate accessor, an accessor argument,
   or a structured reference? Is any new string syntax needed initially?
3. How should source/effective dependencies and source-unit metadata be represented
   so existing invalidation and cycle detection continue to work reliably?
4. Should source-unit preservation be included immediately, or staged separately?
   What precise round-trip guarantee is appropriate, particularly for timestamps?
5. What session attribute and UI best express correction enablement and batch
   editing? How should an exceptional source-schema override differ from that choice?
6. How should schema and unit context be preserved through multi-file session
   merges and source-preserving serialization?
7. Does the existing model reliably refresh and persist logbook column caches
   after a session policy change, or does that path need targeted changes?
8. Which current importer/exporter/plugin assumptions would break under the new
   resolution semantics, and what bounded changes address them?
9. What is the appropriate behavior for source data whose schema cannot yet be
   interpreted, without disrupting access to unaffected measurements unnecessarily?

The objective is a reversible, understandable measurement model using the
existing infrastructure. Arbitrary transformation pipelines, additional export
features, a correction provenance log, and migration machinery for the unreleased
prototype are not objectives of this proposal.

