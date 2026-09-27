# Implementation Plan: Source Preservation, Schema Conversion, and Registered Calculations

> **Baseline warning for every reader of this plan.** The work starts from tag
> `v2026.04.1`, **not** from `master`. The working tree you are reading is
> `master`, which carries one extra commit (`4668f48`, import-time gyro
> correction) that is superseded and must not be carried forward. Only these
> files differ between the tag and `master`; for them, read the baseline with
> `git show v2026.04.1:<path>` and cite baseline line numbers:
>
> `README.md`, `docs/DATA_SCHEMA.md` (absent at baseline), `src/dataexporter.cpp`,
> `src/dataimporter.cpp`, `src/dataimporter.h`, `src/logbookmanager.cpp`,
> `src/mainwindow.cpp`, `src/preferences/importsettingspage.cpp`,
> `src/preferences/importsettingspage.h`, `src/preferences/preferencekeys.h`,
> `src/sessiondata.h`, `tests/*` (absent at baseline).
>
> Every other file is identical at the tag and on `master`. The test
> scaffolding in `master:tests/` (Qt Test target layout, offscreen platform,
> fixture writing) may be borrowed; its gyro-scaling preference, `GyroScaling`
> enum, `DataSchema` namespace, schema stamping, and `dataSchemaVersion` index
> field must not be.
>
> `PLANS/` is git-excluded, so these documents survive the branch switch.

## Feature Specification

The complete specification follows, unabridged (source:
`PLANS/schema-and-calculations.md`; headings demoted two levels, text
otherwise verbatim).

---

### Source preservation, schema conversion, and registered calculations

Date: 2026-09-18
Status: specification for planning. Not an implementation plan.
Baseline: branch from tag `v2026.04.1`. The one commit after that tag on
`master` (import-time gyro correction) is superseded by this specification and
must not be carried forward, although its test scaffolding may be borrowed.

This document describes what the change must do and the architectural
boundaries it must respect. It deliberately avoids class layouts, container
choices, and function signatures unless a name is part of the observable
contract. The planning agent decides phasing; the implementation agent decides
code structure within these boundaries.

#### 1. Motivation

Legacy FlySight 2 firmware scaled the LSM6DSO gyroscope by dividing the 16-bit
reading into the nominal full-scale range (2000 deg/s / 32768 counts). ST
specifies a sensitivity of 0.070 deg/s per count, which implies a slightly
larger full-scale range. Recorded legacy gyro rates are therefore too small by
a factor of

```
0.070 / (2000 / 32768) = 1.14688
```

New firmware writes correctly scaled values and adds a header line to
`SENSOR.CSV`:

```
$VAR,SCHEMA_VER,2
```

Files without this line have an implied schema version of 1. Viewer must
present correctly scaled gyro data for both old and new recordings.

Rather than rescaling data at import, Viewer will keep every measurement
exactly as recorded and apply schema corrections and unit normalization in a
calculation layer. This avoids compounding errors, makes corrections
reversible by fixing the layer instead of the data, and keeps saved logbook
files faithful to the original recordings.

Doing this well requires tightening the calculated-value system. Today,
calculations are registered per output value, may write arbitrary values into
the cache as side effects, and do not always invalidate dependents correctly.
This change replaces that with registered calculations that declare their
inputs and outputs, own their results, and are invalidated through one
dependency mechanism. That model also enables multi-output calculations and,
later, explicitly requested expensive calculations such as sensor fusion.

#### 2. Scope

In scope:

- A source layer that stores measurements and units exactly as parsed from
  the file, with explicit access that never involves calculation.
- A single, hard-coded conversion layer that produces the effective values
  ordinary consumers read under the existing measurement names, applying
  schema corrections and unit normalization.
- Import and merge rules that preserve recorded header attributes and reject
  conflicting values.
- A calculation engine based on registered calculations with declared inputs
  and outputs, engine-owned results, multi-output support, ordered alternative
  candidates, and reliable invalidation.
- Migration of all built-in calculations, synthesized interpolation
  references, and the Python plugin bridge onto that engine.
- Saving sessions in the existing FlySight CSV format with source values,
  source units, recorded header attributes, and Viewer's session attributes,
  with exact numeric round trips.
- Invalidation of persisted logbook column caches when calculation semantics
  change.
- Automated tests for the behavior described here.
- Documentation updates replacing the superseded import-time description.

Out of scope:

- Any new user-interface element, preference, dialog, badge, or notification
  for schema handling. Normal legacy handling is silent.
- A per-session switch to disable corrections. A user who needs a file read
  literally adds `$VAR,SCHEMA_VER,2` to its header and re-imports it.
- A job queue, worker threads, asynchronous evaluation, or progress UI. The
  engine must not preclude these, but does not implement them.
- Sensor fusion, firmware changes, corrected-data export, correction history,
  or byte-identical file archival.
- A general transformation language, per-source attribute maps, or
  representation priorities.

#### 3. Data model

A session holds three kinds of information.

##### 3.1 Source measurements

Each measurement parsed from a file is stored under the sensor name and column
label exactly as they appear in the `$COL` header, with the sample values
exactly as parsed and the unit text exactly as it appears in the `$UNIT`
header. No scaling, unit conversion, or schema stamping is applied at import.

Source measurements are immutable with respect to calculation and
interpretation. They change only through import, merge, or explicit
programmatic replacement.

Source measurements are not renamed. There is no `wx_source`, `source:wx`, or
any other alternate column name. A measurement has one name and two layers
behind it; the layers are distinguished by how they are accessed, not by what
they are called. This preserves the direct mapping between file column labels
and measurement names and avoids collisions with user-defined columns.

##### 3.2 Session attributes

A session has one attribute map. It contains header attributes read from files
(`FIRMWARE_VER`, `SCHEMA_VER`, `DEVICE_ID`, `SESSION_ID`, and anything else
the file declares) and Viewer's own session attributes (the underscore-prefixed
keys such as `_DESCRIPTION`, `_GROUND_ELEV`, markers, and analysis
parameters).

A missing attribute means "no value", never "default value". In particular,
Viewer must not insert `SCHEMA_VER` into a session that did not declare it.
Any default interpretation of absence belongs to the code that interprets the
attribute.

Header attributes are preserved exactly, including unknown keys. Viewer never
rewrites `SCHEMA_VER` or `FIRMWARE_VER`.

##### 3.3 Effective values

Ordinary reads of a measurement return its effective value: the source data
passed through the conversion layer (section 5). Effective values are
calculation outputs. They are cached per session, computed lazily, discarded
with the session, and never persisted.

Ordinary reads of attributes continue to return the stored attribute when
present and a calculated attribute otherwise.

#### 4. Access and enumeration

- Ordinary measurement access under the existing names returns effective
  values. All plots, built-in calculations, logbook columns, and plugins use
  this path unchanged.
- Explicit source access returns the recorded samples and the recorded unit
  text. It never runs a calculation and never falls back to a derived value.
  Asking for the source of a measurement that has no source data (for example
  a purely derived `IMU/wTotal`) reports absence.
- Effective unit access returns the unit the effective values are expressed
  in. Source and effective unit reports must agree with their respective
  values.
- Enumeration and presence queries (`sensorKeys`, `measurementKeys`,
  `hasMeasurement`, `attributeKeys`, `hasAttribute`) describe stored source
  data and stored attributes, as today. Calculation outputs do not appear in
  enumeration because they happened to be computed.

#### 5. Conversion layer

The conversion layer is one fixed, built-in transformation from source to
effective values. It is not extensible by plugins and is not configured per
session. It is implemented as an ordinary calculation in the engine of
section 7 so that its results are cached, tracked, and invalidated like any
other.

For each measurement with source data, the effective value is produced in
this order:

1. Schema correction, selected by the session's recorded `SCHEMA_VER` and the
   measurement's sensor and name.
2. Unit normalization, selected by the recorded unit text.

The result carries the normalized unit label.

##### 5.1 Schema correction

| Recorded `SCHEMA_VER` | Behavior |
| --- | --- |
| Absent or `1` | Multiply `IMU/wx`, `IMU/wy`, `IMU/wz` by 1.14688. No other measurement changes. |
| `2` | No correction. |
| Anything else, or malformed | Import error; the file is rejected (section 6). |

Absence is interpreted as schema 1 inside the conversion layer only. It is not
written back as an attribute.

Future schema versions add rows to this table and steps to this layer. They do
not add session attributes or controls.

##### 5.2 Unit normalization

Unit normalization keeps Viewer's existing internal conventions:

- `g` becomes m/s² (multiply by 9.80665).
- `gauss` becomes tesla (multiply by 0.0001).
- Angular rates stay in deg/s. Angles stay in degrees. Temperatures stay in
  degrees Celsius. This change does not alter any internal unit.
- Unit text already in internal form, including the labels Viewer has written
  in released logbook files (`m/s^2`, `T`, `degC`), is an identity conversion.
- Unknown unit text is an identity conversion; the values and unit text pass
  through unchanged. This matches today's behavior for custom columns.

Viewer's separate display-unit layer (metric/imperial presentation) is
unchanged and continues to operate on effective values.

##### 5.3 Performance expectations

Effective values are computed on first read, not at import. A column whose
conversion is the identity should not require a second copy of its samples.
A converted column costs one additional buffer while it is cached. No eager
conversion of every column and no new eviction machinery is required.

#### 6. Import and merge

##### 6.1 Parsing

The parser reads a file into source measurements, source units, and header
attributes without modification. The existing FlySight 1 (`time,lat,lon,...`)
and FlySight 2 (`$FLYS`) readers, GNSS-only files, custom columns, and the
documented header grammar continue to be supported.

Validation happens before anything is published to the session. A file with an
unsupported or malformed `SCHEMA_VER`, or a structural error, is rejected with
a clear error message and leaves any existing session untouched.

ISO-8601 timestamps ending in `Z` are parsed to seconds with millisecond
precision, which matches what the firmware writes. All other numeric fields
are parsed as doubles without loss.

##### 6.2 New session versus merge

A FlySight session is normally two files, `TRACK.CSV` and `SENSOR.CSV`,
sharing a `SESSION_ID`. Importing a file whose `SESSION_ID` is not in the
logbook creates a new session. Importing a file whose `SESSION_ID` already
exists merges into that session, whether or not the session is currently
loaded. An unloaded session is loaded before merging; a failed load is an
error, never a reason to replace the session with the incoming file alone.

Viewer's import-time defaults (`_DESCRIPTION`, `_IMPORT_TIME`, wind, mass,
area, fixed ground elevation, and any similar values derived from preferences
or the file path) are applied only when a new session is created. A merge
never applies them and never overwrites existing session attributes with them.

##### 6.3 Attribute conflicts

When merging, each header attribute in the incoming file is compared with the
session:

- The session lacks the key: the incoming value is added.
- The session has the key with an equal value: nothing changes.
- The session has the key with a different value: the import of that file
  fails with an error naming the attribute, and the session is left exactly as
  it was.

Absence on either side is never a conflict. A `TRACK.CSV` without
`SCHEMA_VER` merges cleanly with a `SENSOR.CSV` that declares it. Because
absence is "no value", this rule also gives the supported escape hatch: a user
who adds `$VAR,SCHEMA_VER,2` to a previously unmarked file and re-imports it
updates the session's schema, and effective values follow. A file whose
explicit schema value differs from the session's must be deleted and
re-imported, and the error message should say so.

##### 6.4 Measurement merge

Incoming measurements replace same-named measurements in the same sensor and
add new ones. Samples and unit text move together. Measurements not present in
the incoming file are kept. Merging operates on source data only and never
consults effective values or caches.

##### 6.5 Effects of a merge

A merge that changes source data or attributes invalidates every dependent
calculated value, refreshes logbook columns through the model's normal
notification path, marks the session dirty, and schedules a save. Session
edits (descriptions, markers, ground elevation, analysis parameters) survive
a merge.

#### 7. Calculation engine

##### 7.1 Registered calculations

The unit of registration is a calculation, not a value. A calculation
declares:

- A stable identity.
- Its inputs: attributes, measurements, and, where a calculation reads one,
  a preference. All declared inputs are required: the calculation runs only
  when every one of them is available.
- Its outputs: one or more attributes and/or measurements.
- Its evaluation policy: on demand (the default) or explicit (section 7.7).

A calculation is a pure function of its declared inputs. It receives read
access to those inputs and returns its outputs. It cannot write to the cache,
cannot mutate source data, and cannot read undeclared session state. Reading
an undeclared input is an error that tests can detect. A calculation that
should work with or without some input is expressed as two registered
calculations declaring the same output (section 7.3), which is how the
existing per-sensor recipes already work.

Registrations are global and hold no per-session state. The same registration
serves every session.

Existing multi-output helpers (time fit, simplified track, analysis range,
flare detection, WS-P results, SP results) become single calculations with
several declared outputs. A group corresponds to one actual computation; do
not merge unrelated calculations because their outputs share a prefix.

##### 7.2 Results and caching

The engine owns all cached results. A calculation runs at most once per
session while its result is valid, regardless of which output is read first or
how many outputs are read. A result is published atomically: consumers never
observe some outputs from a new run alongside others from an old run.

A result may be partial. A calculation can report some outputs as unavailable
while providing others. An unavailable result is cached like any other and is
not re-run until a declared input changes.

Inspecting whether a result is available never triggers computation.
Invalidating a result never triggers computation.

##### 7.3 Resolving a public name

Reading a measurement by name resolves as follows:

1. If the session has source data for that name, the value is the conversion
   layer's output for it.
2. Otherwise, the registered calculations that declare that output are tried
   in registration order. The first whose declared inputs are all available
   and which produces the output wins.

Reading an attribute by name resolves as follows:

1. If the session stores the attribute, that value is returned.
2. Otherwise, registered calculations declaring that output are tried in
   registration order as above.

Stored data therefore keeps its existing precedence over derived values. A
file that supplies `IMU/wTotal` provides it; a file that does not gets the
derived one. A user who sets a marker attribute overrides the calculated
marker while the calculation's other outputs remain available.

Several calculations may declare the same output. This is how the existing
per-sensor `_START_TIME` and `_DURATION` candidates and the GNSS-versus-other
sensor `_time` variants are expressed. Order is registration order, fixed and
deterministic at startup; there are no numeric priorities.

Whether a candidate's inputs are "available" is decided by the session's
persistent state and this same resolution rule applied recursively. It is
never decided by what is currently cached.

##### 7.4 Idempotency

The value returned for any name is a pure function of the session's
persistent state (source data, attributes, declared preferences) and the
registry. The order in which values are read, and which values happen to be
cached, may change what work is done but never what is returned.

Stated as a testable invariant: after any sequence of reads, edits, merges,
and registry changes, the value returned for every name equals the value
obtained by clearing all caches and evaluating from scratch.

Today's engine violates this because it records dependencies only for the
candidate that won. A cached fallback therefore survives the arrival of a
preferred candidate's inputs, and the answer depends on whether the value was
read before or after a merge. Section 7.5 closes that gap. Tests should
exercise randomized read orders against the fresh-evaluation oracle.

A calculation that consults anything outside its declared inputs, such as the
clock, random state, or an undeclared preference, is not a function of state
and cannot be made idempotent by any invalidation scheme. Such reads are
errors.

##### 7.5 Dependency tracking and invalidation

One mechanism tracks dependencies for everything: built-in calculations, the
conversion layer, synthesized interpolation, and plugins. It must satisfy:

- Resolving a name records everything the resolution looked at: the winning
  calculation's inputs and the availability checks that rejected every
  earlier candidate. Any change that could alter the choice, including a
  previously absent input appearing for a candidate that lost, invalidates
  the cached answer so the next read re-resolves. A cached fallback is
  replaced when a preferred candidate becomes viable.
- Source measurement changes, unit changes, attribute set/remove, and
  preference changes for declared preference inputs all invalidate
  transitively.
- The source layer and the effective layer of the same name have distinct
  identities in the graph. The conversion layer depends on the source; nothing
  else does. No self-cycle arises from a name having two layers.
- Nested evaluation is supported. A calculation may read another
  calculation's output. Each evaluation has its own scope; there is no shared
  mutable side-effect list.
- Cycles are detected and reported, and evaluation unwinds cleanly. An
  exception thrown by a calculation leaves no partial result and no corrupted
  scope.
- Registering or unregistering a calculation invalidates affected results in
  every loaded session. A session's cache must not outlive a removed
  registration.

##### 7.6 Parameterized calculations

Two existing features are instances of one calculation applied with
parameters: the conversion layer (one instance per source measurement) and
synthesized interpolation, `{timeAttribute}:{sensor}/{timeVector}/{measurement}`
(one instance per distinct expression). The engine must support registering
such a calculation once and instantiating it per parameter set with distinct
results and dependencies. No new registration per session or per column, and
no change to the interpolation syntax.

##### 7.7 Evaluation policy

On-demand calculations run when an output is first read. Explicit
calculations run only when something requests them; reading their outputs
before that reports unavailable without starting work. This change provides
the flag and a synchronous request operation and migrates nothing to explicit
mode. It exists so that a future job queue can run a calculation by identity
and publish one result for all its outputs without redesigning the engine.

##### 7.8 Preferences read by calculations

Some calculations read preferences at compute time (for example the descent
pause timeout used by the analysis range). Such reads must be declared inputs
so that changing the preference invalidates dependents. Preferences that are
meant to be snapshotted into a new session at import (mass, area, fixed
ground elevation) continue to be copied into session attributes at session
creation and are read as attributes.

##### 7.9 Test support

The engine must let tests count how many times a calculation actually ran,
and compare a cached answer against a fresh evaluation with caches cleared.
The latter is the oracle for the idempotency invariant in section 7.4.

#### 8. Plugins

There are no third-party plugins; the bundled SDK and its documentation may
change freely, but the simple cases must stay simple.

- `AttributePlugin` and `MeasurementPlugin` remain single-output adapters over
  the engine. Their declared `inputs()` become the calculation's declared
  inputs. Ordinary reads inside `compute()` return effective values under the
  existing names, exactly as C++ consumers see them.
- Plugins get explicit source access (samples and unit text) with the same
  semantics as C++.
- The direct cache setter exposed to Python is removed. Plugins return
  results; they do not publish them. A small multi-output plugin form that
  returns a bundle of declared outputs should be provided, with one example.
- Returned NumPy arrays are copied or otherwise kept valid after the callback
  returns. A Python exception or malformed output produces a clean
  unavailable result, never a partial publication.
- Dependency kinds crossing the bridge are decoded explicitly; unknown kinds
  are errors.

#### 9. Persistence

##### 9.1 Session files

Saved sessions keep the existing FlySight 2 CSV format so that users can
recover their data with ordinary tools. A saved session contains:

- `$FLYS,1`.
- Every session attribute as `$VAR` lines: header attributes exactly as
  recorded (including `SCHEMA_VER` when and only when the session has it) and
  Viewer's underscore attributes.
- `$COL` and `$UNIT` lines with the source column labels and source unit
  text.
- Data rows containing source samples.

The writer reads only the source layer. It never calls effective getters and
never writes calculation outputs. Warm and cold caches produce identical
files.

##### 9.2 Precision

Numeric values round-trip exactly: a double parsed from a file, saved, and
parsed again is the same double. The writer must use a representation that
guarantees this (17 significant digits or a shortest-round-trip formatter),
and the guarantee applies to the bulk row writer, not only to header values.
ISO timestamps are written at millisecond precision. Non-finite values are
handled explicitly and documented rather than silently written as zero.

##### 9.3 Existing logbooks

Logbook files written by released Viewer versions contain already-normalized
values and unit labels and no `SCHEMA_VER`. They load unchanged: their
contents are the source, normalization is the identity, and the absent schema
means their gyro data is legacy-scaled and receives the correction. No
migration step or rewrite of existing files is required. There is no
requirement that older Viewer versions read files written by this version,
although the format is unchanged so they will.

##### 9.4 Logbook column cache

`index.json` caches computed column values for unloaded sessions. Those values
are derived data and must be discarded when the calculations that produce them
change meaning. This change alters gyro-dependent values for every legacy
session, so the cache must be invalidated on upgrade. Tie cache validity to an
explicit calculation-compatibility marker that the implementation increments
when semantics change; do not use `SCHEMA_VER` for this. Per-session edits
and merges refresh the affected values through the model's existing lifecycle.
An interrupted save must not leave cached columns that disagree with the saved
session file.

#### 10. Documentation

- Rewrite `docs/DATA_SCHEMA.md` to describe the recorded schema, the
  conversion layer, the escape hatch, and the preservation guarantees.
  Remove all description of import-time correction and schema stamping.
- Document source versus effective access, the attribute conflict rule, and
  the plugin changes in the plugin README/SDK docstrings.
- Document how to run the tests.

#### 11. Acceptance

The following must be demonstrated by automated tests using generated
fixtures in temporary directories, never the user's logbook or preferences.
Expected values are stated independently, not computed by the code under test.

1. Importing an unmarked file with `wx=62.5, wy=-125, wz=0` yields effective
   `71.68, -143.36, 0` deg/s. Source access returns the recorded values and
   the recorded unit text. `SCHEMA_VER` is absent from the session.
2. The same values in a file declaring `SCHEMA_VER,2` are unchanged in the
   effective layer.
3. A file declaring `SCHEMA_VER,3` or `SCHEMA_VER,abc` is rejected with an
   error and an existing session with that `SESSION_ID` is unmodified.
4. Recorded `1 g` reads as `9.80665 m/s^2` effective; recorded `1 gauss` reads
   as `0.0001 T`; the source retains `1` and `g` / `gauss`. A custom column
   with unknown unit text passes through unchanged with its label.
5. Save then reload: every source sample is bit-identical, unit text and all
   header attributes are preserved, `SCHEMA_VER` is present only if it was
   recorded, and effective values are identical before and after. Repeating
   the cycle changes nothing. Saving with warm and cold caches produces the
   same file.
6. A released-format logbook file (normalized units, no `SCHEMA_VER`) loads,
   its gyro is corrected once, and saving it does not rescale or relabel it.
7. `TRACK.CSV` and `SENSOR.CSV` merge in either order, whether or not the
   session is loaded, with the same result. A merge whose header attribute
   conflicts with the session fails and changes nothing. Session edits and
   unmatched measurements survive a merge.
8. Adding `SCHEMA_VER,2` to an unmarked file and re-importing it updates the
   session's attribute and its effective gyro values, without a new session
   and without losing edits.
9. A three-output calculation runs once when its outputs are read in any
   order across repeated reads. A change to a declared input causes exactly
   one new run on the next read; an unrelated change causes none.
10. A candidate that could not run because a declared input was absent is
    selected on the next read once that input is added, replacing a cached
    fallback. For randomized sequences of reads, edits, and merges, every
    value returned equals the value from a fresh evaluation with caches
    cleared (section 7.4).
11. A user override of one output of a multi-output calculation coexists with
    the calculation's remaining outputs and causes no cycle.
12. Nested calculations, cycles, and thrown exceptions leave no partial
    results and no corrupted evaluation state.
13. Two sessions using one registration have independent results.
    Unregistering a calculation invalidates its results in every session.
14. An explicit-policy calculation reports unavailable until requested, and
    requesting it publishes all outputs at once.
15. Changing a declared preference input invalidates dependents; changing a
    preference that is only snapshotted at import does not affect existing
    sessions.
16. Derived `IMU/wTotal` and an interpolated gyro attribute reflect the
    corrected values and follow source changes; a file-supplied `wTotal`
    keeps precedence over the derived one.
17. Existing single-output Python plugins work through the real bridge with
    effective reads; a multi-output plugin runs once across its outputs;
    source access from Python matches C++; a Python exception yields a clean
    unavailable result.
18. Upgrading a logbook with cached gyro-dependent column values discards and
    recomputes them; a session edit refreshes only the affected columns.
19. The full application builds and ordinary import, plot, marker, logbook,
    and plugin workflows work end to end with no remaining use of the old
    per-value cache engine or direct cache setters.

#### 12. Principles for the implementers

- One authority per fact: source data in the source layer, session facts in
  the attribute map, derived values in the engine. No second copy of any of
  them.
- Nothing infers schema from firmware version, filename, date, or the data
  itself. Only `SCHEMA_VER` decides.
- Nothing rewrites recorded header attributes.
- Ordinary consumers keep using the names they use today. Nothing gains a
  prefix, suffix, or alternate key.
- Remove the old mechanisms rather than leaving them alongside the new ones.
  There is one cache, one dependency graph, and one mutation path at the end.
- Silence for the normal case. The user notices this change only because
  their gyro plots are correct.

---

## Phases

| Phase | Name | Purpose | Dependencies |
|-------|------|---------|--------------|
| 1 | Baseline branch, core library, and test harness | Branch from `v2026.04.1`, move the non-UI model/IO/calculation sources into a linkable static library, and add an isolated Qt Test harness (temp dirs, isolated preferences, generated fixtures) wired into the top-level build. | None |
| 2 | Calculation engine core | Build the new engine as standalone, fully tested code: registered calculations with declared inputs (attribute / measurement / preference) and outputs, engine-owned per-session results, multi-output, partial/unavailable results, ordered candidates, resolution-time dependency recording (including rejected candidates), parameterized calculations, nested scopes, cycle/exception safety, registry-change invalidation across sessions, explicit policy, and test instrumentation. Not yet wired into `SessionData`. | Phase 1 |
| 3 | Engine adoption and built-in migration | Make `SessionData` resolve every attribute/measurement read through the engine; migrate all built-in calculations (multi-output groups, per-sensor candidates), synthesized interpolation, altitude-marker dynamic registration, and the declared preference input; keep the Python bridge compiling via minimal single-output adapters; delete `CalculatedValue`, `DependencyManager`, and the direct cache setters. | Phase 2 |
| 4 | Source layer, conversion layer, and parser | Split source from effective values: explicit source access (samples + unit text), effective unit access, enumeration over stored data; the conversion layer as a parameterized engine calculation (schema correction then unit normalization); importer stores data exactly as recorded, validates `SCHEMA_VER` and structure before publishing, never stamps attributes. | Phase 3 |
| 5 | Persistence: exact round trip and column-cache versioning | Exporter reads only the source layer and writes exact-round-trip numbers (rows and `$VAR` values), ms timestamps, explicit non-finite handling; released logbooks load unchanged; `index.json` gains a calculation-compatibility marker; cached columns never disagree with the saved session after an interrupted save. | Phase 4 |
| 6 | Import and merge rules | New-session versus merge by `SESSION_ID` (loaded or not, load-before-merge, failed load is an error), import-time defaults only on creation, attribute conflict rule, source-only measurement merge with units, transactional failure, invalidation + model notification + dirty + save, and surfacing import error messages. | Phase 5 |
| 7 | Python plugin bridge | `AttributePlugin` / `MeasurementPlugin` as single-output adapters with effective reads; explicit source access from Python; multi-output plugin form with one example; direct cache setter removed; NumPy results copied; exceptions/malformed output → clean unavailable; explicit dependency-kind decoding; tests through the real embedded bridge. | Phase 4 |
| 8 | Documentation, end-to-end acceptance, and cleanup audit | Rewrite `docs/DATA_SCHEMA.md`, write plugin/SDK and test documentation, add the session-level randomized idempotency oracle test across reads/edits/merges, map every acceptance item (1–19) to a passing test, and audit that no old mechanism remains. | Phases 5, 6, 7 |

## Dependency Graph

```
1 ─► 2 ─► 3 ─► 4 ─┬─► 5 ─► 6 ─┬─► 8
                  │           │
                  └─► 7 ──────┘
```

- Phases 1–4 are strictly sequential: each replaces the foundation the next stands on.
- After Phase 4, two tracks run in parallel: persistence → import/merge (5 → 6) and the Python bridge (7). They touch disjoint files (`dataexporter`, `logbookmanager`, `sessionmodel`, `mainwindow` versus `pluginhost`, `*_bindings.cpp`, `python_plugins/`), with one exception: Phase 5 Task 5.5 and Phase 7 Task 7.1 both edit `src/engine/calculationregistry.*` and `tests/tst_calcregistry.cpp`, as self-contained appended blocks.
- Phase 5 precedes Phase 6 because merging into an unloaded session depends on a faithful save/load of source data and units.
- Between the end of Phase 4 and the end of Phase 5 the exporter must not write effective values under source units. Phase 4 therefore includes the one-line switch of the exporter's value/unit getters to source access; Phase 5 does the precision, attribute-formatting, and non-finite work.
- The application must build and run at the end of every phase. Tests added in a phase must pass at the end of that phase and every later one.

## Key Patterns & References

Line numbers are for the baseline (`v2026.04.1`) where the file differs from `master`.

### Session data model (to be restructured)
- `src/sessiondata.h` / `src/sessiondata.cpp` — single `m_attributes` map, `m_sensors` (samples) and `m_units` (unit text), `getAttribute`/`getMeasurement` stored-then-calculated fallback (cpp 27-34, 77-84), `set*` returning the invalidated `QSet<DependencyKey>` (36-56, 102-111), direct cache setters `setCalculatedAttribute/Measurement` (113-122), static registration (140-160), `unregisterCalculatedAttribute` (129-138, zero callers), `synthesizeInterpolation` + `interpolationKey` (170-246), `friend class DataImporter`, `SessionKeys` constants. `units()` has no callers.
- `src/dependencykey.h` — `DependencyKey` (Attribute | Measurement; no preference kind, no source/effective distinction), `qHash`, `toDependencyKey`.

### Old calculation engine (to be removed)
- `src/calculatedvalue.h` / `.cpp` — static `s_methods` recipe lists per key, first-satisfied-recipe-wins (cpp 52-87), deps recorded only for the winner, shared mutable `m_sideEffectKeys` (asserted empty at 49; broken by nested evaluation), `m_activeCalculations` cycle guard, no negative caching, no exception safety.
- `src/dependencymanager.h` / `.cpp` — per-session reverse-edge map, insert-only, BFS `invalidateKeyAndDependents` returning the visited set.
- `src/calculations/calculatedvalueregistry.h` / `.cpp` — startup entry point calling the eleven `Calculations::register*` functions in fixed order.

### Built-in calculations (to be migrated)
- `src/calculations/attributecalculations.cpp` — analysis range (2 outputs, side-effect writes 81-82; reads preference `ImportDescentPauseSeconds` undeclared at 18-19), `_EXIT_TIME`, `_SYNC_TIME`/`_COURSE_REF` passthroughs, manoeuvre start, flare detection (2 outputs, side effects 340-341), `_LANDING_TIME`, `_START_TIME`/`_DURATION` × 7 per-sensor candidates (439-480), max-vel times, `_GROUND_ELEV` fallback (572-607; near-cycle with analysis end / landing).
- `src/calculations/timecalculations.cpp` / `.h` — time fit (2 outputs, side effects 85-86), `{sensor}/_time` GNSS passthrough versus six fit-based candidates (115-165), `_system_time` (168-207); uses `hasMeasurement` on raw inputs (45-47, 116).
- `src/calculations/simplificationcalculations.cpp` — simplified track, 4 measurement outputs via `setCalculatedMeasurement` (128-131).
- `src/calculations/wspcalculations.cpp` — constant-default parameters with empty deps (16-42), `_WSP_REF1_TIME`, ten-output `computeWspResults` with invalid-`QVariant` "clear" writes (86-349), marker/attribute-registry side work (352-442).
- `src/calculations/spcalculations.cpp` — constant-default parameters (17-38), `_SP_WINDOW_START_TIME` + side-effect `_SP_WINDOW_START_ALT` with self-reading recipe (44-102), five-output results (105-317), markers (319-394).
- `src/calculations/gnsscalculations.cpp` — twenty single-output measurements; undeclared `_SESSION_ID` reads in warnings (61, 90).
- `src/calculations/imucalculations.cpp` (`IMU/aTotal`, `IMU/wTotal`), `src/calculations/magcalculations.cpp` (`MAG/total`) — single-output; undeclared `_SESSION_ID` reads.
- `src/calculations/barocalculations.cpp`, `humcalculations.cpp`, `vbatcalculations.cpp` — empty placeholders.
- `src/calculations/derivativehelper.*`, `isadensity.*` — pure helpers, unchanged.
- `src/calculations/attributeregistration.*` — UI attribute metadata only; not part of the engine.

### Dynamic registration and registry consumers
- `src/altitudemarkerfeature.cpp` / `.h` — only runtime registrar: `registerAll()` (34-147) registers `_ALTITUDE_<n>_{FT|M}` with thresholds baked from preferences/`QSettings`; `refresh()` (149-171) calls the raw static `unregisterCalculation`, never flushing session caches (stale-value bug the new engine must fix).
- `src/mainwindow.cpp` 136-165 — startup order: built-in plots/markers/attributes, `PluginHost::initialise()`, preferences, `registerBuiltInCalculations()`, `AltitudeMarkerManager::registerAll()`. Plugin registrations currently precede built-ins.
- `src/ui/docks/plot/PlotWidget.cpp` 1602-1617, `src/ui/docks/analysis/WingsuitPerformanceWidget.cpp` 416-417, `SpeedSkydivingWidget.cpp` 330-332 — `hasRegisteredCalculation` / `removeAttribute` "reset to default" pattern.
- `src/markerregistry.*`, `src/attributeregistry.*`, `src/plotregistry.*` — metadata registries, no engine coupling.

### Interpolation key users
- `src/sessionmodel.cpp` 235-249, 1458-1466, 1497-1504 — logbook `MeasurementAtMarker` / `Delta` columns and sort keys.
- `src/ui/docks/plot/PlotWidget.cpp` 1235 — marker bubble values.
- `src/pluginhost.cpp` 403-410 — Python markers supply the same triples.
- `src/logbookcolumn.h` / `.cpp` — column definition shape, `==` / `<`, JSON form.

### Invalidation notification
- `src/sessionmodel.cpp` 397-499 (`setData`), 940-980 (`updateAttribute`), 982-1018 (`removeAttribute`) — re-emit one `dependencyChanged(sessionId, key)` per invalidated key, `dataChanged`, `scheduleSave`. `mergeSessions` (501-583) discards the invalidation set and emits no `dependencyChanged`.
- Subscribers: `src/momentmodel.cpp` 251-259, `src/ui/docks/plot/PlotWidget.cpp` 172 / 2033+, `src/ui/docks/video/VideoWidget.cpp` ~230, `WingsuitPerformanceWidget.cpp` 140 / 424-429, `SpeedSkydivingWidget.cpp` 110.

### Import / parsing
- `src/dataimporter.cpp` / `.h` (baseline) — `readFile` sniffing (29-83), `importSimple` FS1 (139-186), `importFS2` (188-227), `importHeaderRow` `$VAR/$COL/$UNIT` (229-292), `importDataRow` incl. `Z` timestamp parsing and silent row dropping (294-366), **import-time SI conversion in place** (176-185, 209-226), `initializeFromDevice` defaults (85-137), `getDescription` (449-483), `extractDeviceId` (368-422), direct `m_sensors` writes via friendship, `m_lastError` not cleared at entry.
- `src/units/unitconversion.h` — importer-only file-unit → SI table (`g`, `gauss`, `deg C` → `degC` label, unknown → identity + warning); becomes the conversion layer's normalization table.
- `src/units/unitconverter.*`, `src/units/unitdefinitions.h` — display-unit layer; independent of `SessionData::getUnit`; unchanged.
- `src/mainwindow.cpp` 564-681 — `importFiles`: per-file importer, forces `_GROUND_ELEV` (586), one `mergeSessions` call, failure dialog lists file names only (661-680).

### Merge, model, and logbook
- `src/sessionmodel.cpp` / `.h` — `SessionRow` (h 16-24), `mergeSessions` (501-583: loaded match overwrites attributes incl. defaults, stub match **replaces wholesale**, units never merged), `sessionRef` lazy load with empty-session fallback on failure (813-850), `scheduleSave` (1022-1037), `flushDirtySessions` (1039-1072), `saveNextSession` (1074-1100), `processNextDirtyColumn` (1229-1286), `rebuildColumns` (133-172), `computeColumnValues` (1448+), idle tasks (ctor 32-118).
- `src/logbookmanager.cpp` / `.h` (baseline) — layout `…/FlySight Viewer/logbook/{index.json, sessions/<uuid>.csv}`, `initialize` three cases (≈148-223), `loadSession` via `DataImporter::readFile` + mass/area/wind backfill (≈351-389), `saveSession`, `flushIndex` (no version field at baseline), `columnDefinitionKey` (87-107), identity-stub remap, atomic `QSaveFile` writes.
- `src/idlescheduler.*` — idle task scheduling for saves and column fills.

### Persistence
- `src/dataexporter.cpp` / `.h` (baseline) — `QSaveFile`, header via `QTextStream`, `$VAR` values through `QVariant::toString()` (lossy doubles, no escaping) (76-79), `$COL`/`$UNIT` from `sensorKeys`/`measurementKeys`/`getUnit` (82-100), bulk rows `QByteArray::number(v,'g',15)` (108-150), row count from first column only (121-122), non-finite unhandled, `kAttributeOrder`.

### Python bridge
- `src/pluginhost.cpp` / `.h` — interpreter boot (71-187), bridge import (195), plugin discovery (236-272), attribute (277-324) and measurement (329-364) registration with `inputs()` decoded by `.kind` (283-295, 337-349), return conversion, **no try/catch around `compute`**, plots (369-386), markers (391-413).
- `src/sessiondata_bindings.cpp` — exposes `setCalculatedMeasurement` (13-47, the direct cache setter), `getMeasurement`, `getAttribute`, `hasMeasurement`, `hasAttribute`.
- `src/dependencykey_bindings.cpp`, `src/cpp_bridge.cpp`, `src/bridgeimpl.h` (dead header).
- `python_plugins/flysight_plugin_sdk.py` — `AttributePlugin`, `MeasurementPlugin`, `meas()`/`attr()`, `register_*`, `SimplePlot`, `SimpleMarker`; unregistered example classes 208-260. No plugin README or example plugin exists.

### Build and tests
- `CMakeLists.txt`, `src/CMakeLists.txt` — `flysight_model` STATIC (sessiondata / dependencymanager / calculatedvalue only, 234-241), everything else compiled into the `FlySightViewer` executable (`PROJECT_SOURCES` 244-353); Python/pybind11 embed (114-161, 399), GeographicLib (207), Boost (217), KDDockWidgets (220).
- `master:tests/CMakeLists.txt`, `master:tests/README.md`, `master:tests/dataimporter_test.cpp` — borrowable Qt Test scaffolding (standalone project, offscreen platform, sources compiled directly into the test).
- `src/preferences/preferencesmanager.h`, `src/preferences/preferencekeys.h` — `QSettings`-backed singleton with `preferenceChanged` signal; the declared-preference input and test isolation both hang off this.
- `README.md` — build instructions and project-structure section to update.

## Commit Policy (per-phase audit trail)

Michael has authorized commits on the working branch `schema-and-calculations` for this plan, so that each phase can be audited separately afterward. **The implementation orchestrator makes every commit** (see "Version Control" in `.claude/prompts/implementation-orchestrator.md`); implementation, revision, and review agents never run git commands that change repository state. This section overrides any wording about commits in the phase documents. Nothing is ever pushed, and nothing is committed on `master`.

- **Branch.** Before spawning the Phase 1 agent, the orchestrator creates the branch with `git switch -c schema-and-calculations v2026.04.1` (the git step of Phase 1's branch task; the agent only verifies it). Pre-existing untracked paths (`PLANS/`, `experiments/`, `third-party/*` build directories, `build*/`, `TEMP/`, `results/`, `dist/`) are never staged.
- **Tooling commit.** `.claude/prompts/implementation-orchestrator.md` is tracked, identical at `v2026.04.1` and `master`, and was modified on 2026-09-18 to add the "Version Control" section. If that modification is still uncommitted when the orchestrator starts, it carries across the branch switch; the orchestrator commits that one file first as `Tooling: orchestrator owns commits`, before Phase 1, so every later `git status` check starts clean. It is the only tracked modification expected at the start; anything else means stop and ask.
- **One commit per accepted phase**, made when the phase's review agent returns ACCEPT, containing exactly the files the phase's implementation and revision agents reported. Subject: `Phase N: <phase name>`; body: a short summary of the tasks, then the session's attribution line (`Co-Authored-By: ...`). Rejected iterations are never committed.
- **Tag.** After the commit, the orchestrator creates the lightweight local tag `plan/phase-N-done`. Tags are never moved.
- **Fixes to a closed phase** (found by a later phase, an integration debugging agent, or the final review) are committed as `Phase N fixup: <what>` once reviewed, under the number of the phase being fixed.
- **Parallel track (5 -> 6 alongside 7).** Both tracks share one working tree. The orchestrator stages by explicit path only, so committing Phase 7 never sweeps up Phase 5's in-progress work or vice versa. The files both tracks edit (`src/engine/calculationregistry.*`, `tests/tst_calcregistry.cpp`, and the appended blocks in `src/CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/README.md`) cannot be split by path: the orchestrator either holds the first-accepted phase's commit until the other track's edits to those files are complete, or serializes the tracks (5 -> 6 -> 7). Nothing in the plan depends on the parallelism; serializing gives the cleanest per-phase diffs.
- **CI workflow edits** (`.github/workflows/*`, proposed as optional in Phases 1 and 7 and finalized in Phase 8) cannot be verified without a push. Wherever a phase document says to keep such an edit "in its own commit", the agent reports those paths separately and the orchestrator commits them as `Phase N: CI test step (unverified)` right after the phase commit, so the edit can be dropped on its own.

Auditing afterward:

```
git log --oneline v2026.04.1..HEAD                              # one line per phase, plus fixups
git show --stat plan/phase-3-done                               # Phase 3 as accepted
git diff plan/phase-3-done~1 plan/phase-3-done                  # Phase 3 as one diff
git log --oneline --grep='^Phase [0-9] fixup' v2026.04.1..HEAD  # everything that reopened a closed phase
```

## Decisions & Constraints

- **Branch.** Phase 1 creates the working branch from `v2026.04.1`. Nothing from commit `4668f48` is cherry-picked; only test scaffolding ideas are re-created.
- **Build stays green per phase.** Each phase ends with the application building and all tests so far passing. Within Phase 3 the old and new engines never serve reads at the same time for longer than a task boundary; the phase ends with the old engine deleted.
- **Engine first, standalone.** Phase 2's engine reads session state through a narrow interface so it can be unit-tested with synthetic calculations before `SessionData` is rewired. The engine is single-threaded (main thread), holds no global mutable per-session state, and identifies calculations by stable identity so a future job queue can request one by identity.
- **Graph identities.** The dependency graph distinguishes: stored attribute, source measurement, source unit, effective measurement, calculated attribute/measurement output, preference, and resolution (candidate-choice) nodes. Only the conversion layer depends on source nodes, with one plugin-only exception added in Phase 7 (`CalculationDescriptor::allowSourceInputs`, set only by the plugin host) so Python source reads are tracked — flagged for review against spec §7.5.
- **Availability is state-based.** Candidate viability is decided from persistent state plus recursive resolution, never from cache contents; unavailable results are negatively cached with their dependencies.
- **Registration order.** Order stays deterministic at startup. The current order (Python plugins before built-ins, so a plugin declaring a built-in output wins) is preserved unless Michael decides otherwise — flagged for review.
- **Phase 3 keeps the bridge compiling, Phase 7 finishes it.** Phase 3 re-points `pluginhost.cpp` at the new registration API as single-output adapters and deletes the `setCalculatedMeasurement` binding; all other plugin requirements are Phase 7.
- **Exporter switch in Phase 4.** The moment ordinary reads become effective, the exporter must read source samples and source units; that minimal switch is in Phase 4, the rest of persistence in Phase 5.
- **Notifications.** Invalidation continues to surface to the UI as `SessionModel::dependencyChanged` per public name; the source/effective split is not visible to subscribers. Merges now emit through this same path.
- **Header versus Viewer attributes.** Keys beginning with `_` are Viewer session attributes; all others are recorded header attributes. The §6.3 conflict rule applies to header attributes. When an incoming file being merged carries `_` attributes (a Viewer-saved file), existing session values win and absent ones are added — flagged for review.
- **Import-time generated identifiers.** `SESSION_ID` synthesized from the file hash (FS1 / files without one) and `DEVICE_ID` from `FLYSIGHT.TXT` or `"n/a"` are creation-time defaults, not recorded header attributes, and are not applied or compared on merge beyond session matching.
- **`LogbookManager::loadSession` backfill** of mass/area/wind for old logbooks is existing behavior outside the spec's import path; Phase 6 decides how it coexists with "defaults only at creation" (it must not run during a merge and must not mark absent attributes as defaults in the engine).
- **Column-cache marker.** `index.json` gets an integer calculation-compatibility marker owned by the calculation code (not `SCHEMA_VER`, not `dataSchemaVersion`). A missing or different marker discards all cached column values; session files are untouched.
- **Test isolation.** Tests never touch the user's logbook or preferences: `QStandardPaths` test mode / redirected `QSettings`, logbook folder preference pointed at a `QTemporaryDir`, fixtures generated in code, expected values written as literals.
- **No new UI.** Only the existing import-failure dialog changes, to include error text.

## Risks & Open Questions

The consolidated list of open questions for Michael (Q1–Q17, each with the default the plan assumes) and the cross-phase findings (F1–F12) live in `08-docs-acceptance-and-audit.md`. The ones most worth a decision before implementation starts: plugin-before-built-in registration order; `_` attributes on merge (existing wins); time columns saved as numeric seconds rather than ISO text (reading of spec §9.2); the `calculationEnvironment` fingerprint in `index.json` beyond the integer marker; `allowSourceInputs` versus spec §7.5; the legacy mass/area/wind load-time backfill kept as a shim; behaviour of an in-memory unsupported `SCHEMA_VER` (no correction + warning, and the exporter refuses to save it).


- Phase 3 is the largest phase (engine adoption plus ~60 registrations). Its document must order tasks so each leaves a compiling tree.
- The `_GROUND_ELEV` ↔ `_ANALYSIS_END_TIME` ↔ `_LANDING_TIME` near-cycle currently relies on the active-calculation guard; under the new engine it must be expressed as ordered candidates without a true cycle.
- Tests for acceptance items 7, 8, and 18 need `SessionModel` and `LogbookManager` in the linkable library without dragging in UI/WebEngine dependencies; Phase 1 must draw that library boundary.
- Acceptance 17 needs an embedded-Python test target; its CI cost on all three platforms should be checked in Phase 7.
