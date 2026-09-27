# Measurement contracts agreed in Phase 0

This is the concrete companion to [final-measurement-plan.md](final-measurement-plan.md).
It specifies interfaces and fixtures, not an activated replacement of production
storage or calculation code. Phase owners may change private layouts, not these
observable contracts without updating this note and informing the coordinator.

## Ownership and shared types

Use namespace `FlySight::Measurement`. Phase 1 owns `src/measurementtypes.h` and
`src/persistentmeasurementstore.{h,cpp}`; Phase 2 owns
`src/measurementevaluator.{h,cpp}`. Phase 2 may consume but not independently edit
Phase 1's header. Put evaluator-specific keys in the evaluator header. Phase 3
owns the integration changes to `sessiondata.*` and legacy dependency adapters.
Phase 4 owns `src/measurementsessioncodec.{h,cpp}`. Coordinate additions to
`tests/CMakeLists.txt` through the coordinator. No production headers are created
in Phase 0: the following signatures are the shared implementation contract.

```cpp
using SessionId = QString; // existing stable logbook identity; never a source ID
using SourceId = QString;  // nonempty opaque UUID text for newly accepted sources
using AxisId = QString;    // nonempty opaque local identity
using MeasurementKey = QPair<QString, QString>; // sensor, measurement
using Attributes = QMap<QString, QVariant>;
struct SourceRecord { SourceId id; Attributes attributes; };
struct SampleAxis {
    AxisId id;
    SourceId sourceId; // source which supplied this alignment context
    std::optional<MeasurementKey> coordinateMeasurement; // exact recorded time column
    QString clockDomain; // "device:<DEVICE_ID>:<SESSION_ID>" or explicit caller domain
    QVector<double> coordinates; // normalized seconds, finite; order preserved
    QString ordinalProvenance; // nonempty only when no usable time coordinate exists
    qsizetype count;
};
struct SourceSeries {
    SourceId sourceId;
    AxisId axisId;
    QString unit;
    QVector<double> samples;
};
struct PersistentState {
    SessionId sessionId;
    QVector<SourceRecord> sources; // acceptance order, oldest to newest
    QMap<AxisId, SampleAxis> axes;
    QMap<MeasurementKey, SourceSeries> series;
    Attributes overrides;
    QSet<QString> suppressedAttributes;
};
```

Containers use Qt value semantics. Snapshots are detached on modification; callers
cannot mutate store-owned vectors through a view. Sources with no remaining
current series are still retained with their complete attributes and ordering.
Imported `_SESSION_ID`/`SESSION_ID` remain source attributes; the existing
`SessionKeys::SessionId` public name resolves the authoritative session identity
as a reserved field (not an override). Other imported keys are not reserved or
whitelisted. Session identity cannot be changed through attribute edits.
Runtime visibility, dirty flags, caches and loaded state are not persistent inputs.

`PersistentMeasurementStore` supplies `snapshot() -> PersistentState` by value,
`sourceSeries(key) -> optional<SourceSeries>`,
`sourceAttribute(sourceId,key) -> optional<QVariant>`,
`storedAttribute(key) -> optional<QVariant>`, and source/stored enumeration and
presence methods. Missing is optional absence; an empty vector or empty string
is present. Invalid QVariant is not a stored attribute and is rejected by mutation.
`sourceUnit(key)` uses the same series optional; it never consults calculations.

`Mutation` is a transaction with added source records/axes, series upserts and
removals, and attribute edits `{SetOverride(value), Suppress, ClearOverride}`.
`apply(mutation) -> MutationResult { bool success; QString error;
QSet<InputKey> changedInputs; QMap<SourceId,SourceId> sourceRemap; }` validates a
candidate snapshot before committing. Failure changes nothing. A semantic no-op
has an empty change set. No raw mutable map access is exported. Source replacement
uses a new complete SourceRecord or explicit replacement of an existing record,
and invalidates every changed attribute identity plus membership if keys change.
Programmatic construction must supply explicit units, source attributes and axis
context; already-effective plugin results never enter this path.

Input identities are tagged `InputKey` values:

| Kind | Payload | Changed when |
| --- | --- | --- |
| SessionAttribute | attribute key | override/suppression/presence changes |
| SourceSeries | MeasurementKey | samples, unit, source/axis association or presence changes |
| SourceAttribute | SourceId + attribute key | recorded value or presence changes |
| SourceSelection | none | source ordering/membership, source attribute key membership, current series key membership or resolved stored attribute key membership changes |
| SourceAxis | AxisId | coordinates/domain/count/presence changes |
| LivePreference | QString key | an explicitly supported live recipe preference changes |

The store produces the first five; Phase 5's preference adapter produces the last.
`SourceSelection` is the structural membership dependency for stored enumeration,
including negative sensor/measurement queries and newly introduced overrides.
Value-only series edits do not change it. Key-specific reads still use their
fine-grained identities; the store also emits `SessionAttribute` when a selected
stored attribute value changes through source replacement or selection.
The evaluator additionally tracks `LogicalAttribute(key)`,
`LogicalMeasurement(key)`, `Result(instanceKey)` and `Registry` (revision). Do not
reinterpret new kinds as measurement keys in legacy/Python switches.
Phase 7 wraps one store apply + evaluator invalidation operation in the model,
returning affected logical keys, dirty state and normal dependency notifications.
Notifications never initiate calculation. Batch work stores SessionId + attribute
key, not a row or column index.

## Attribute resolution and identity

Sources are searched newest to oldest for **that key**. Missing in the newest
source does not hide an older value. Source order is serialized explicitly as the
`sources` array; do not sort it by ID. Table rows are mirrored in the fixture table.

| Sources A then B | Override | Suppressed | Default | Ordinary value | Stored? |
| --- | --- | --- | --- | --- | --- |
| absent | absent | no | absent | unavailable | no |
| absent | absent | no | D | D | no |
| A=old, B=new | absent | no | D | new | yes |
| A=old, B absent | absent | no | D | old | yes |
| A=old, B=new | edit | no | D | edit | yes |
| A=old, B=new | absent | yes | D | D | no |
| A=old, B=new | absent | yes | absent | unavailable | no |
| A=old, B=new | empty string | no | D | empty string | yes |

SetOverride clears suppression. Suppress removes the override and persists a
tombstone, even if currently no source provides the key. ClearOverride removes
both override and suppression and restores inheritance. Existing `removeAttribute`
and UI reset-to-calculated-default callers map to Suppress. A separately named
`clearAttributeOverride` restores source inheritance. A tombstone survives later
merges. Defaults never become stored keys. Source access ignores all overlays.
The reserved stable session-ID attribute is always present. Correction is a normal
session override with a constant calculated default, not source schema metadata.

Merge keeps destination SessionId, overrides and tombstones. Incoming matching
series replace existing series including units, source/axis association; unmatched
series survive. Incoming source records append in their existing order. No schema
deduplication. If an incoming source/axis ID collides, assign a fresh ID and rewrite
all incoming references, even if the colliding records happen to compare equal.
Return the remap; repeated save/reload without merge retains exact IDs. Explicit
loading of a saved session restores its complete state; importing it as additional
source content discards its session overlays/identity, not its source records.

## Axis validation

Each series length equals its axis count; each timed axis has count coordinates.
Coordinates are derived from recorded time using supported unit normalization,
without modifying recorded time samples. They are alignment metadata, not an
alternative effective measurement or an independently editable duplicate: rebuild
them when the corresponding source time is replaced. Corresponding recorded time series must agree
with the normalized axis. Require same clockDomain and exact finite coordinates
to mix series in a sensor across axes; equality of counts is insufficient. No
sorting or resampling. Use DEVICE_ID + SESSION_ID for device-relative time when
available, `utc` for parsed UTC timestamps; otherwise assign unique import-block
provenance. An ordinal axis has empty coordinates, count and nonempty unique
ordinalProvenance; separate ordinal blocks are never presumed aligned. Empty
timed axes are compatible only in the same clock domain. Reject a partial sensor
merge whose retained and incoming grids differ, atomically. Complete sensor
replacement can use a different grid if no incompatible series remains. Do not
silently drop unmatched columns to achieve that replacement.
`coordinateMeasurement` is required for timed axes and absent for ordinal axes.
It identifies the current logical source time column, whose normalized coordinates
must equal the axis. Its source association may differ from the axis's historical
provenance when an incoming time column supplies the same grid. A transaction
replacing/removing that column must validate every dependent axis and all retained
series, or reject the transaction. It cannot silently reinterpret retained series
on a new grid. Remove unreferenced axes; never keep old source buffers just to
support an incompatible partial replacement. This explicit rule
avoids guessing between `time`, `_time` and `tow`.

## Evaluator API and lifetimes

```cpp
struct CalculationInstanceKey {
    QString definitionId;
    QStringList parameters; // ordered immutable UTF-16 strings; no delimiter joining
};
enum class FieldKind { Attribute, Measurement };
struct MeasurementValue { QVector<double> samples; QString unit; };
using FieldValue = std::variant<QVariant, MeasurementValue>;
struct ResultField { std::optional<FieldValue> value; QString diagnostic; };
struct CalculationResult { QMap<QString, ResultField> fields; };
struct OutputBinding {
    LogicalKey output; CalculationInstanceKey instance; QString field;
};
using CalculationFunction = std::function<CalculationResult(
    CalculationReadView&, const QStringList& parameters)>;
struct CalculationDefinition {
    QString id; QMap<QString,FieldKind> fields; CalculationFunction evaluate;
};
```

Definition IDs such as `builtin.time-fit.v1` are independent of public names.
Source normalization uses `builtin.source-normalize.v1` with parameters
`{sensor,measurement}`; interpolation uses `builtin.interpolate.v1` with the
full existing interpolation expression as its one parameter. Fixed definitions
use no parameters. Binding registration order is candidate order. Replacing a
definition or binding/unregistering bumps a registry revision; all session caches
observe it before reads, including cached reads. Definitions own their callables;
cache entries never own a dangling plugin callback. Definitions cannot capture
one session's input. Registry lifetime exceeds evaluator lifetime.

`CalculationReadView` exposes tracked ordinary `attribute`, `measurement`,
`unit`, `hasAttribute`, `hasMeasurement`, stored enumeration, and explicit
`sourceSeries`, `sourceAttribute`, `sourceUnit`, `sources`, `axis` reads. Return
optional values/Qt value copies with implicit sharing, not references into an
evictable cache. Source and stored queries delegate to an abstract `InputAccess`
with the store's read methods; Phase 2 can use a fixture implementation. The
read view is valid only during its callback and offers no mutation/cache writer.

`MeasurementEvaluator(InputAccess&, CalculationRegistry&)` has
`evaluate(instance) -> shared_ptr<const CalculationResult>`,
`inspect(instance) -> shared_ptr<const CalculationResult>` (null if not retained
and valid), ordinary output resolution and `invalidate(changedInputs) ->
QSet<LogicalKey>`. Add `clearResults()` for deterministic fresh-result tests.
Every declared field must appear, with explicit absence for unavailable outputs;
extra/missing/type-invalid fields fail validation of the whole result. Callback
exceptions become one cached unavailable result with diagnostics for all fields.
Missing inputs, failed candidates, negative presence and cache-hit reads all
retain dependency edges. Partial availability is explicit per field; no stale
field publication. Returned handles remain immutable and alive across eviction,
but are snapshots, not promises of current validity. Cache lifetime is the loaded
session. Evaluator and registry are synchronous and not thread-safe; no queue.

Ordinary attributes first consult stored resolution, then ordered bindings.
Ordinary measurements first track source presence and use normalization if present;
conversion failure never falls through. Empty present source remains an available
empty result. Only absent source allows ordered derived providers. Logical output
resolution records dependencies from all failed candidates before a fallback.
Nested reads have separate scopes with cycle detection; unwinding restores the
previous scope, including exceptions. Invalidation replaces edges on rerun,
does not calculate, and propagates through logical outputs/result dependencies.
All one-output adapters return a single declared field through this same engine.

## Correction and initialization

`SessionKeys::CorrectLegacyGyro = "_CORRECT_LEGACY_GYRO"` is boolean. Its default
calculation returns true. `PreferenceKeys::ImportCorrectLegacyGyro =
"import/correctLegacyGyro"` is a distinct boolean preference, default true.
Preference label: **Enable legacy gyro correction for new recordings**.
Session actions: **Enable legacy gyro correction** / **Disable legacy gyro correction**.
Help: “Applies Viewer's legacy gyro scale correction. Unit normalization always
applies; schema-2 recordings are unaffected.” No three-state stored policy.

Parsing sources never initializes session state. New-recording construction
snapshots the import preference only if no valid saved choice exists. Restore and
additional-source merge never read that preference. First adoption of a released
legacy logbook uses constant true (persist at next normal save), independent of
the current import preference. A malformed saved boolean is rejected. The old
`import/gyroScaling` preference is not migrated or reinterpreted.

Source schema absent means interpreted 1 without inserting a recorded attribute.
Explicit markers must be trimmed decimal integer text `1` or `2` (or equivalent
integer QVariant); no fractional/exponent/empty/future marker. Normalize units
after scale correction. Only IMU.wx/wy/wz get the legacy multiplier; supplied
wTotal wins unchanged apart from supported unit normalization. All source schemas
are validated at import, including sources without IMU. Opaque fields may retain
unknown units; unsupported units of known physical quantities fail effective reads.

## Saved format version 1 and atomic index

Extension `.fsv`; exact first bytes `FLYSIGHT-VIEWER-SESSION\n` (ASCII, no BOM),
followed by one UTF-8 JSON object. The old reader cannot recognize this as `$FLYS`
or a valid two-line CSV header. `formatVersion` integer 1 is independent of source
SCHEMA_VER and index `calculationCompatibility = "measurement-core-1"`. The latter
is explicitly bumped for any algorithm/interpretation change affecting columns.
`tests/fixtures/measurement/mixed-session.fsv` is normative structural example:
sessionId, sources in precedence order, axes, series, overrides, suppressedAttributes.
Unknown format version, duplicate IDs/series keys, dangling references, invalid
axis structure or duplicate JSON object keys are errors. Unknown top-level fields
may be ignored within version 1; missing required fields are errors. Source maps
can be empty. Metadata never contains an effective result or nested session.

Attributes encode `{ "type": T, "value": V }`: T is string (JSON string), bool
(JSON boolean), int/uint/int64/uint64 (decimal string with range validation), double
(decimal string), stringList (array of strings), or dateTime (UTC ISO with milliseconds).
Device attributes are strings, including schema; supported session QVariant types
retain type. Unsupported QVariant types cause a write error, not silent coercion.
All sample and coordinate doubles encode as JSON strings using C-locale
`max_digits10` (17 significant digits); preserve signed zero. Nonfinite source
samples explicitly encode `NaN`, `+Inf`, `-Inf`; NaN payload bits are not preserved.
Axes must be finite. Reject malformed numbers; never replace with zero. Device
CSV accepts these nonfinite sample spellings too. No numeric row writer may use
the old 15-digit precision for source storage. Parsed ISO times retain the current
millisecond limit; already-lost device spelling/precision is unrecoverable.

Device `$VAR` grammar: `$VAR,` then key up to the next comma, then the entire
remaining line as value, including commas, quotes and backslashes literally.
UTF-8, LF/CRLF supported; keys nonempty with no comma/newline, values may be empty
and contain commas but not literal line breaks. No CSV quote unescaping in VAR.
Duplicate VAR keys use last occurrence. Unknown non-attribute record types are
ignored. JSON storage supports arbitrary Unicode keys, commas and escaped line
breaks. Numeric CSV rows and COL/UNIT retain their existing unquoted grammar;
malformed structural/numeric rows reject the import atomically. This deliberately
fixes today's truncation after the first value token and silent malformed-row skip.

Codec APIs: `parseRecording(path) -> parsed source transaction` (no preferences),
`readSession(path) -> PersistentState`, `writeSession(path,const PersistentState&)
-> success/error`. Released `.csv` logbook compatibility creates one source from
the normalized data actually present. Never divide old values to reconstruct raw
data. Restoring `.fsv` directly restores records/IDs rather than wrapping a source.

Phase 7 saves `<stable session filename>.fsv` using QSaveFile; only successful
commit clears dirty state. Index entries name the file, SHA-256 of its committed
bytes, SessionId, and calculationCompatibility. QSaveFile commits index.json only
after the session commit. On startup hash-verify before displaying indexed derived
columns; mismatch/missing/unknown compatibility rebuilds from saved source.
On first CSV migration, keep the valid CSV until the FSV and index commits succeed;
keeping it thereafter is allowed. Startup scans recognized `.fsv` files to recover
an orphan committed before its index update, matching embedded sessionId; prefer
the FSV to the same session's legacy CSV. Do not delete either copy on failed save.
An index failure after session commit reports durable session success plus index
failure, invalidates the in-memory index entry and permits rebuilding; it never
claims cached columns are valid. A session commit failure keeps dirty state and
the old index. Hash checking avoids a two-file transactional protocol or journaling.

## Baseline inventory and cutover ownership

* `dataimporter.cpp`: header setters + direct friend m_sensors append; unit setters
  normalize in place; schema reset/stamp and initialization setters. Phase 4.
* `dataexporter.cpp`: stored enumeration followed by ordinary getters, 15-digit
  numeric writer. Replace with snapshot codec, Phase 4/9.
* `sessiondata.cpp`: flat map setters, cached getters, separate interpolation
  cache/dependencies. Phases 1/3/5; never activate facade before writer/merge cutover.
* `sessionmodel.cpp`: table edits around 451/464, merge 527-535, single edits
  963/1001, row-based bulk edits 1379/1409. Phase 7, source snapshot merge.
* `logbookmanager.cpp`: initialization around 376-389, source file restore and
  derived-column index. Phase 7; do not overwrite saved overlays with defaults.
* Built-in registration: attribute, GNSS, IMU, MAG, time, simplification, WS-P,
  SP calculations plus altitudemarkerfeature.cpp. Group side effects exist in
  analysis range/flare/time fit/simplification/WS-P/SP. Phase 5.
* `PlotWidget.cpp` stored sensor enumeration and marker reset; analysis widgets
  call removeAttribute for defaults. Keep reset-to-default as Suppress, Phase 5/7.
* `pluginhost.cpp`, sessiondata/dependencykey bindings and Python SDK register
  one-output functions; Python exposes arbitrary setCalculatedMeasurement.
  Phase 6 removes unsupported cache writes with clear migration error, supplies
  read-only view and result bundle API. No untracked compatibility cache.

Prototype tests remain explicitly labeled baseline evidence: schema-2 stamping,
in-place gyro/acceleration scaling, three-way import preference and exporting
effective values must be replaced at Phase 9 cutover. New core fixture tests need
only Qt Core/Test; model tests will be separate with actual SessionModel/logbook
sources, and Python tests separate with actual bridge/Python/pybind11 dependencies.
Absent optional integration dependencies must be reported, not silently passed.

Phase 3 replaces SessionData's persistent/cache authority with the new modules;
it must not add a second editable source store beside the old maps. Production
parser/save/merge call sites must use bounded adapters until the Phase 9 cutover,
or be migrated coherently when the facade is activated. Phase 9 removes those
adapters; it does not defer the actual Phase 3 integration.

### Cycle rejection refinement after independent integration review

On detecting a cycle, every participating calculation instance is wholly unavailable. Propagate the internal cycle failure through those callbacks to the earliest participating calculation, then permit alternatives outside that cycle. Participating callbacks may not recover by making further reads or publishing a successful result. Cache these unavailable results with their actual dependencies; relevant input/registry changes permit retry. Acyclic callers remain eligible to handle unavailable inputs. This conservative rule avoids context-dependent cache poisoning without introducing fixed-point solving.

### Index/provider compatibility refinement after model review

Runtime registry revisions invalidate loaded evaluator results and unloaded row/index columns. Persisted index reuse additionally checks the live descent timeout and native altitude-marker definition context. Arbitrary Python callbacks have no durable compatibility declaration: application startup disables persisted column reuse whenever Python calculation providers are registered. The index records that restriction so removing the provider on a later launch cannot make its old results valid. Session identities, committed source files, access metadata and recovery still work; only calculated columns rebuild. This deliberately favors correctness over cross-launch column-cache reuse for Python providers, without inventing callable hashes or a second evaluator.
