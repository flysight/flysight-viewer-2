# Source preservation, schema conversion, and registered calculations

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

## 1. Motivation

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

## 2. Scope

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

## 3. Data model

A session holds three kinds of information.

### 3.1 Source measurements

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

### 3.2 Session attributes

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

### 3.3 Effective values

Ordinary reads of a measurement return its effective value: the source data
passed through the conversion layer (section 5). Effective values are
calculation outputs. They are cached per session, computed lazily, discarded
with the session, and never persisted.

Ordinary reads of attributes continue to return the stored attribute when
present and a calculated attribute otherwise.

## 4. Access and enumeration

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

## 5. Conversion layer

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

### 5.1 Schema correction

| Recorded `SCHEMA_VER` | Behavior |
| --- | --- |
| Absent or `1` | Multiply `IMU/wx`, `IMU/wy`, `IMU/wz` by 1.14688. No other measurement changes. |
| `2` | No correction. |
| Anything else, or malformed | Import error; the file is rejected (section 6). |

Absence is interpreted as schema 1 inside the conversion layer only. It is not
written back as an attribute.

Future schema versions add rows to this table and steps to this layer. They do
not add session attributes or controls.

### 5.2 Unit normalization

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

### 5.3 Performance expectations

Effective values are computed on first read, not at import. A column whose
conversion is the identity should not require a second copy of its samples.
A converted column costs one additional buffer while it is cached. No eager
conversion of every column and no new eviction machinery is required.

## 6. Import and merge

### 6.1 Parsing

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

### 6.2 New session versus merge

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

### 6.3 Attribute conflicts

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

### 6.4 Measurement merge

Incoming measurements replace same-named measurements in the same sensor and
add new ones. Samples and unit text move together. Measurements not present in
the incoming file are kept. Merging operates on source data only and never
consults effective values or caches.

### 6.5 Effects of a merge

A merge that changes source data or attributes invalidates every dependent
calculated value, refreshes logbook columns through the model's normal
notification path, marks the session dirty, and schedules a save. Session
edits (descriptions, markers, ground elevation, analysis parameters) survive
a merge.

## 7. Calculation engine

### 7.1 Registered calculations

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

### 7.2 Results and caching

The engine owns all cached results. A calculation runs at most once per
session while its result is valid, regardless of which output is read first or
how many outputs are read. A result is published atomically: consumers never
observe some outputs from a new run alongside others from an old run.

A result may be partial. A calculation can report some outputs as unavailable
while providing others. An unavailable result is cached like any other and is
not re-run until a declared input changes.

Inspecting whether a result is available never triggers computation.
Invalidating a result never triggers computation.

### 7.3 Resolving a public name

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

### 7.4 Idempotency

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

### 7.5 Dependency tracking and invalidation

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

### 7.6 Parameterized calculations

Two existing features are instances of one calculation applied with
parameters: the conversion layer (one instance per source measurement) and
synthesized interpolation, `{timeAttribute}:{sensor}/{timeVector}/{measurement}`
(one instance per distinct expression). The engine must support registering
such a calculation once and instantiating it per parameter set with distinct
results and dependencies. No new registration per session or per column, and
no change to the interpolation syntax.

### 7.7 Evaluation policy

On-demand calculations run when an output is first read. Explicit
calculations run only when something requests them; reading their outputs
before that reports unavailable without starting work. This change provides
the flag and a synchronous request operation and migrates nothing to explicit
mode. It exists so that a future job queue can run a calculation by identity
and publish one result for all its outputs without redesigning the engine.

### 7.8 Preferences read by calculations

Some calculations read preferences at compute time (for example the descent
pause timeout used by the analysis range). Such reads must be declared inputs
so that changing the preference invalidates dependents. Preferences that are
meant to be snapshotted into a new session at import (mass, area, fixed
ground elevation) continue to be copied into session attributes at session
creation and are read as attributes.

### 7.9 Test support

The engine must let tests count how many times a calculation actually ran,
and compare a cached answer against a fresh evaluation with caches cleared.
The latter is the oracle for the idempotency invariant in section 7.4.

## 8. Plugins

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

## 9. Persistence

### 9.1 Session files

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

### 9.2 Precision

Numeric values round-trip exactly: a double parsed from a file, saved, and
parsed again is the same double. The writer must use a representation that
guarantees this (17 significant digits or a shortest-round-trip formatter),
and the guarantee applies to the bulk row writer, not only to header values.
ISO timestamps are written at millisecond precision. Non-finite values are
handled explicitly and documented rather than silently written as zero.

### 9.3 Existing logbooks

Logbook files written by released Viewer versions contain already-normalized
values and unit labels and no `SCHEMA_VER`. They load unchanged: their
contents are the source, normalization is the identity, and the absent schema
means their gyro data is legacy-scaled and receives the correction. No
migration step or rewrite of existing files is required. There is no
requirement that older Viewer versions read files written by this version,
although the format is unchanged so they will.

### 9.4 Logbook column cache

`index.json` caches computed column values for unloaded sessions. Those values
are derived data and must be discarded when the calculations that produce them
change meaning. This change alters gyro-dependent values for every legacy
session, so the cache must be invalidated on upgrade. Tie cache validity to an
explicit calculation-compatibility marker that the implementation increments
when semantics change; do not use `SCHEMA_VER` for this. Per-session edits
and merges refresh the affected values through the model's existing lifecycle.
An interrupted save must not leave cached columns that disagree with the saved
session file.

## 10. Documentation

- Rewrite `docs/DATA_SCHEMA.md` to describe the recorded schema, the
  conversion layer, the escape hatch, and the preservation guarantees.
  Remove all description of import-time correction and schema stamping.
- Document source versus effective access, the attribute conflict rule, and
  the plugin changes in the plugin README/SDK docstrings.
- Document how to run the tests.

## 11. Acceptance

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

## 12. Principles for the implementers

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

## 13. Amendments decided during implementation

Recorded 2026-09-20. The sections above are left as written; where they
differ, this section governs.

- **Section 8, plugin source access: withdrawn.** Plugins do not get explicit
  source access. They read effective values and attributes only, like every
  other calculation, so section 7.5's "the conversion layer depends on the
  source; nothing else does" holds without exception. A plugin that needs to
  know how data was recorded declares `SCHEMA_VER` as an ordinary attribute
  input. Acceptance item 17 loses its clause "source access from Python
  matches C++". Source access remains available in C++ for the exporter and
  merge.
- **Section 9.2, timestamps.** Time columns are saved as numeric seconds, as
  every released version did: ISO-8601 timestamps are parsed to double epoch
  seconds and sensor system time stays in seconds. Values round-trip exactly.
  Only date-time session attributes are written as ISO text with
  milliseconds. The sentence "ISO timestamps are written at millisecond
  precision" applies to those attributes, not to time columns.
- **Section 9.2, number text.** Doubles are written in shortest round-trip
  form. A value written by a released version may be re-spelled on the next
  save (`0.0001` becomes `1e-04`); the value, unit, and attributes do not
  change. Byte stability holds from the first file this version writes.
- **Section 9.4, column cache.** Besides the integer compatibility marker,
  `index.json` stores a fingerprint of the calculation environment: the
  candidates for each output in the order resolution tries them, and the
  values of declared preferences. A mismatch discards cached column values.
  This is what lets a preference change or a plugin change reach sessions
  that are not loaded.
- **Section 7.3, registration order.** Python plugins register before the
  built-in calculations, so a plugin that declares a built-in output
  supersedes the built-in.
- **Section 7.4, idempotency.** Holds for overlapping dependency cycles too.
  A result shaped by a cycle verdict that involved a frame above it on the
  evaluation stack is not cached, and a cached answer derived from a cycle is
  not served to a nested evaluation that has any of its dependencies on the
  stack.
- **Section 6.3, attribute conflicts.** The rule applies to recorded header
  attributes. Viewer's own attributes (keys beginning with `_`) in an incoming
  Viewer-saved file never conflict: the session's value wins and absent keys
  are added, so an import cannot undo an edit. All conflicting attributes are
  reported, not only the first. The `DEVICE_ID` placeholder `n/a` counts as
  absent.
- **Section 5.1, an unsupported `SCHEMA_VER` set in memory** (which cannot
  come from a file): the gyro reads as recorded with a warning, and the
  session cannot be saved.
- **Section 3.2, `DEVICE_ID`** is not editable in the logbook, because it is a
  recorded header attribute.
- **Kept outside the specified import path:** sessions saved before mass,
  area, and wind attributes existed have them filled from current preferences
  when loaded (in memory only, never during a merge). The tracked replacement
  would be default-value calculations that declare those preferences.
