# FlySight data schema, source preservation, and the conversion layer

This document is for users who want to know what Viewer does with their
recordings, for firmware developers, and for maintainers. Contributors writing
calculations should also read [CALCULATIONS.md](CALCULATIONS.md); plugin authors
the [plugin README](../python_plugins/README.md).

## 1. Summary

- Viewer stores every measurement exactly as it was recorded: the samples as
  parsed, the unit text as written, the header attributes as declared.
- A fixed, built-in conversion layer produces the values everything else reads
  (plots, calculations, logbook columns, plugins).
- Only the recorded `SCHEMA_VER` header attribute selects a schema correction.
- Saved logbook files contain the recorded values, never converted ones.
- The results of explicitly requested calculations (sensor fusion) are stored
  in separate files in the logbook's `cache/` folder, never in the session
  file, and are used only while they are still valid (section 12).
- Normal handling of legacy files is silent: no dialog, no setting, no badge.

## 2. Recorded file format

A FlySight 2 file is a text file:

```
$FLYS,1
$VAR,FIRMWARE_VER,v2023.09.22
$VAR,SESSION_ID,0123456789abcdef
$COL,IMU,time,wx,wy,wz,ax,ay,az,temperature
$UNIT,IMU,s,deg/s,deg/s,deg/s,g,g,g,deg C
$DATA
$IMU,3.250,62.50,-125.00,0.00,0.01,0.02,1.00,24.5
```

- `$FLYS,1` identifies the file encoding and header syntax. It says nothing
  about the meaning of the data.
- `$VAR,<key>,<value>` declares a header attribute. The value is everything
  after the second comma, verbatim; it may contain commas. Unknown keys are
  kept.
- `$COL,<sensor>,<columns...>` declares the columns of a sensor.
- `$UNIT,<sensor>,<units...>` declares their unit text. It is optional, and it
  may list fewer units than there are columns; a missing unit is the empty
  text.
- `$DATA` ends the header. Every following row is `$<SENSOR>,v1,v2,...`.

FlySight 1 files (a `time,lat,lon,hMSL,...` header line, a unit line, then
rows) are read as one sensor named `GNSS`. Their unit text is kept verbatim,
for example `(m)`.

A field ending in `Z` is an ISO-8601 UTC timestamp and is parsed to seconds
since the Unix epoch with millisecond precision, which is what the firmware
writes. Every other field is parsed as a double without loss; `nan`, `inf`,
and `-inf` are accepted.

A file is **rejected with an error**, and nothing is imported from it, when

- it declares an unsupported or malformed `SCHEMA_VER` (section 3);
- a `$VAR` has an empty key, or the same key is declared twice with different
  values;
- a `$COL` names no sensor, lists no columns, has an empty or repeated column
  name, or the sensor already has a `$COL`;
- a `$UNIT` names an unknown sensor, lists more units than the sensor has
  columns, or the sensor already has a `$UNIT`;
- the `$DATA` line is missing.

Malformed or truncated data rows (for example the last line after a power
loss) are **tolerated**: they are skipped and counted in one log line. A file
with a header and no rows is valid.

## 3. `SCHEMA_VER`

`$VAR,SCHEMA_VER,<integer>` declares the schema of the recorded data: the
physical interpretation of the recorded values.

| Recorded `SCHEMA_VER` | Meaning |
| --- | --- |
| Absent, or `1` | Legacy: the gyroscope was recorded with the nominal-range scale (section 4). |
| `2` | Corrected gyroscope scale. Everything else is unchanged. |
| Anything else, or malformed (`3`, `abc`, empty, `2.0`, `+2`) | Import error: `Unsupported SCHEMA_VER '3' (supported: 1, 2)`. |

Absence **is** the information. Viewer never writes, defaults, or rewrites
`SCHEMA_VER` or `FIRMWARE_VER`. A session whose files did not declare a schema
has no `SCHEMA_VER` attribute, in memory and on disk; absence is read as
schema 1 only inside the conversion layer.

`FIRMWARE_VER`, file names, dates, and the data itself never take part in the
decision.

For firmware developers: increment `SCHEMA_VER` only when the physical
interpretation of recorded values changes, never for unrelated firmware fixes.

## 4. Legacy gyroscope scaling

At the LSM6DSO's +/-2000 deg/s setting, legacy firmware converted counts to
deg/s with `2000 / 32768` deg/s per count. ST specifies a sensitivity of
`0.070` deg/s per count. Legacy gyro rates are therefore too small by

```
0.070 / (2000 / 32768) = 1.14688
```

For schema 1 the conversion layer multiplies the recorded `IMU/wx`, `IMU/wy`,
and `IMU/wz` by **1.14688**, and nothing else.

- This is a nominal scale correction, not a per-device calibration. It cannot
  recover precision lost to rounding, clipping, or overflow.
- A *recorded* `IMU/wTotal` column is not in the table and is not corrected.
  The *derived* `IMU/wTotal`, and values interpolated at a marker, are computed
  from the corrected `wx`, `wy`, `wz`.

Note for maintainers: the code holds the decimal literal `1.14688` exactly
once, in `src/conversion/schematable.cpp`. The expression above evaluates to
`1.1468800000000001` in double precision and must not replace the literal.

## 5. Source and effective values

A measurement has one name and two layers behind it. The layers are
distinguished by how they are accessed, never by renaming: there is no
`wx_source` column and no `source:wx` key.

- **Source** = the recorded samples and the recorded unit text. It changes
  only through import, merge, or explicit programmatic replacement. Explicit
  source access never computes anything, and reports absence for a name that
  has no recorded data (for example a purely derived `IMU/wTotal`).
- **Effective** = what plots, calculations, logbook columns, and plugins read
  under the recorded names: the source passed through the conversion layer
  (section 6). Effective values are computed on first read, cached per
  session, and never saved.

Enumeration (`sensorKeys`, `measurementKeys`, `hasMeasurement`,
`attributeKeys`, `hasAttribute`) describes stored data only. A calculated value
never appears in it because it happened to be computed.
The purely derived sensors `Local`, `Simplified`, and `Fusion` therefore never
appear in enumeration or in saved files, and `Fusion` additionally reads
unavailable until sensor fusion has been requested for the session, or a
still-valid stored result of it was restored when the session was loaded
(section 12).

In C++ (`SessionData`): `getMeasurement` and `effectiveUnit` read the effective
layer; `sourceMeasurement`, `sourceUnit`, `hasSourceMeasurement`, and
`sourceData` read the source layer; they serve the exporter, the merge, and
tests. Among calculations, only the conversion layer reads the source layer:
every other calculation, built-in or Python plugin, reads effective values
(for Python see [the plugin README](../python_plugins/README.md)).

## 6. The conversion layer

The conversion layer is fixed and built in. It is not configurable per session
and not extensible by plugins. For every measurement with recorded data it
applies, in this order:

1. the **schema correction** selected by the session's `SCHEMA_VER`, the
   sensor, and the column (section 4);
2. the **unit normalization** selected by the recorded unit text.

The result carries the normalized unit label.

| Recorded unit text | Effective values | Effective label |
| --- | --- | --- |
| `g` | x 9.80665 | `m/s^2` |
| `gauss` | x 0.0001 | `T` |
| `deg C` | unchanged (Celsius is kept) | `degC` |
| `m`, `m/s`, `Pa`, `s`, `deg`, `deg/s`, `V`, `%`, empty | unchanged | unchanged |
| `m/s^2`, `T`, `degC` (written by released Viewer versions) | unchanged | unchanged |
| `(m)`, `(m/s)`, `(deg)` | unchanged | `m`, `m/s`, `deg` |
| `volt`, `percent` | unchanged | `V`, `%` |
| anything else | unchanged | unchanged |

- The lookup ignores surrounding whitespace; the stored unit text is never
  trimmed.
- Unknown unit text is the normal case for custom columns. It is an identity
  conversion, the label passes through unchanged, and nothing is logged.
- Angular rates stay in deg/s, angles in degrees, temperatures in degrees
  Celsius.
- The metric / imperial presentation chosen in Viewer's display settings is a
  separate layer on top of the effective values and is unchanged.
- Cost: a column whose conversion is the identity shares its buffer with the
  source; a converted column costs one extra buffer while it is cached.

The table is `src/units/unitconversion.h`; the conversion itself is
`src/conversion/sourceconversion.cpp`.

## 7. The escape hatch

A user who needs an unmarked file read literally (for example a custom file
whose gyro values are already correct) declares its schema in the file:

1. Open the original `SENSOR.CSV` in a text editor.
2. Add the line `$VAR,SCHEMA_VER,2` after `$FLYS,1`.
3. Import the file again.

The same session (matched by `SESSION_ID`) gains the attribute and its gyro
values are read literally from then on. Edits and markers are kept and no new
session appears.

If the file's explicit value differs from the session's explicit value (`1`
versus `2`), the import fails with a message naming `SCHEMA_VER` and both
values: delete the session and re-import its files.

Caveat: matching needs a recorded `SESSION_ID`. A file without one is matched
by a hash of its contents, so an edited copy imports as a new session.

There is no setting, per-session switch, dialog, or badge for this.

## 8. Importing and merging

A FlySight session is normally two files, `TRACK.CSV` and `SENSOR.CSV`, sharing
a `SESSION_ID`.

- A file whose `SESSION_ID` is not in the logbook creates a **new session**.
- A file whose `SESSION_ID` is known is **merged** into that session, whether
  or not the session is currently loaded. An unloaded session is loaded first;
  if it cannot be loaded the import fails and the stored file is untouched.

Import-time defaults (`_DESCRIPTION`, `_IMPORT_TIME`, `_WIND_N`, `_WIND_E`,
`_JUMPER_MASS`, `_PLANFORM_AREA`, a fixed `_GROUND_ELEV`, and a synthesized
`SESSION_ID` / `DEVICE_ID` when the file records none) are applied only when a
session is created. A merge never applies them.

**Attribute conflict rule**, for header attributes (keys without a leading
`_`):

| Session | Incoming file | Result |
| --- | --- | --- |
| lacks the key | has it | the value is added |
| has it | same text, exactly | nothing changes |
| has it | different text | the file is rejected; the error names the attribute and both values; the session is unchanged |
| has it | lacks it | nothing changes |

Absence on either side is never a conflict: a `TRACK.CSV` without `SCHEMA_VER`
merges cleanly with a `SENSOR.CSV` that declares it.

- Viewer's own attributes (keys starting with `_`) in an incoming file, which
  happens when a Viewer-saved file is imported: the existing value wins, absent
  ones are added, never a conflict.
- The `DEVICE_ID` placeholder `n/a` (stored when neither the file nor a
  `FLYSIGHT.TXT` names a device) counts as absent.

**Measurements.** Incoming columns replace same-named columns of the same
sensor, samples and unit text together, and new columns are added. Everything
the file does not mention is kept. Merging works on source data only. A merge
that would leave a sensor with columns of different lengths is rejected.

Re-importing identical files changes nothing, and nothing is saved. All
validation happens before anything changes; errors appear per file in the
import dialog.

## 9. Saved session files

Viewer saves sessions in the same FlySight 2 CSV format, so the data can be
recovered with ordinary tools:

- `$FLYS,1`;
- every attribute as a `$VAR` line: header attributes exactly as recorded,
  `SCHEMA_VER` if and only if it was recorded, plus Viewer's `_` attributes;
- `$COL` and `$UNIT` lines with the recorded labels and unit text (`$UNIT` is
  always written);
- `$DATA`, then rows of **source** samples.

Effective values are never written. Saving with a warm or a cold calculation
cache gives identical bytes. No calculated result is written either: the
stored results of requested calculations are separate files (section 12), and
saving a session that has one gives exactly the same bytes as saving it
without.

Numbers, in rows and in `$VAR` values, are written as the shortest decimal
text that parses back to the identical double. `-0` keeps its sign. Non-finite
values are written as `nan`, `inf`, `-inf` (the payload of a NaN is not
preserved); they are never written as zero. Time columns are saved as numeric
seconds, as every released version has done, for example `1718900000.123`;
attributes holding a date and time are written as UTC ISO-8601 text with
milliseconds.

Attribute text: strings verbatim (commas allowed), a line break replaced by
one space, booleans as `true` / `false`, integers exactly.

A save **fails and leaves the previous file intact** when a sensor has columns
of unequal length, when a sensor name, column label, or unit contains a comma
or a line break, or when the session holds a `SCHEMA_VER` the importer would
reject (section 3). Viewer never writes a file it could not read back.

Byte identity holds from the first Viewer-written file onward. The device file
may differ from it textually (`62.50` becomes `62.5`, an ISO time becomes
seconds, `$VAR` lines may be reordered); the *values* are identical from the
device file onward.

## 10. Existing logbooks

Session files written by released Viewer versions contain already-normalized
values (`m/s^2`, `T`, `degC`) and no `SCHEMA_VER`. They load unchanged: their
contents are the source, the unit normalization is the identity, and the gyro
receives the legacy correction once, at read time. Saving such a session does
not rescale or relabel it and does not add `SCHEMA_VER`. There is no
migration.

Very old session files gain `_JUMPER_MASS`, `_PLANFORM_AREA`, `_WIND_N`, and
`_WIND_E` lines on their next save (a legacy backfill; purely additive).

## 11. Logbook column cache

`index.json` caches the logbook column values of sessions that are not loaded.
They are derived data. Two fields say what they are valid for:

- `calculationCompatibility`, an integer at the root
  (`FlySight::CalculationCompatibilityVersion`). It is unrelated to
  `SCHEMA_VER`. If it is missing or different, every cached value is
  discarded.
- `environment`, one per column definition under `columns`: a digest of what
  the column's value can observe of the registered calculations and the
  preferences. It covers every name the column's value can be computed from
  (following every candidate calculation, not only the one that would win), and
  for each such name the calculations tried for it in their order with the
  result version each declares, the conversion layer for a measurement, and
  the values of the preferences those calculations declare as inputs
  (`docs/CALCULATIONS.md` section 17). Every Python plugin registration
  declares the plug-in code identity (section 12) as its result version, so
  editing, adding or removing a plugin file, or upgrading the Python or NumPy
  the plugins run on, changes the environment of the columns that can be
  computed through a plugin calculation; an update that changes the sensor
  fusion algorithm changes the environment of the columns over fusion outputs.

If a column's `environment` is missing or different, that column's cached
values are discarded; the other columns keep theirs. So adding an altitude
marker, changing a preference or editing a plugin discards only the columns
that can depend on it (a new altitude marker, for instance, only a column that
reads that marker). An index written before per-column environments existed
(it has a root `calculationEnvironment` instead, which is ignored) is
discarded once. Discarded values are recomputed in the background. Session
files, ids, and access times are untouched. Such a change discards cached
column values only; it never makes a stored calculation result stale (section
12, "Validity"). A full discard happened once on the upgrade to this version,
because gyro-dependent columns changed for every legacy session.

```json
"columns": {
  "<id>": {"type": "SessionAttribute", "attributeKey": "_EXIT_TIME",
           "environment": "<40 hex digits>"}
}
```

Each session entry also has `"records"`: an object naming the requested
calculations that have a stored result for that session (section 12), each
mapped to the result version the cached values over it were computed under,
and `{}` when there are none. A value cached for a column that depends on such
a calculation is valid only together with this stamp. For a session with a
stored sensor fusion result:

```json
"records": {"builtin.fusion.fit": "batch-temperature-bias-v3"}
```

An edit or a merge refreshes only the columns that can depend on the change,
and an interrupted save cannot leave cached values that disagree with the
saved session file. A save that fails (a full disk, say) loses nothing: the
session stays in memory with its changes, its affected columns stay out of
`index.json`, and the save is tried again at the next edit and when the
application closes.

A column that depends on an explicitly requested calculation (a sensor fusion
value at a marker, for example) is computed, while the session is loaded, from
the restored or just-published result, and cached like any other column. At
start-up such a cached value is kept only while the session's `"records"`
stamp matches the record files on disk and the calculations' current result
versions; writing or deleting a record drops it. For a session that is not
loaded and has no cached value, the column is unavailable when the session has
no stored result. When it has one, the column stays empty (pending) until the
session is loaded, because records are read only when a session is loaded.

Before a record is written whose calculation `index.json` lists as present
under a cached value, the index is rewritten without that value, so a crash at
any point cannot leave a cached value that disagrees with the records. A value
whose record may disagree with the loaded session (a record write or removal
that failed, or a record that could not be read when the session was loaded,
section 12) is kept out of `index.json` until the record is written or deleted
again or the session is unloaded. An unloaded session whose record was skipped
shows such a column empty (pending) until it is loaded again, when the record
is read again. An index written before stamps existed keeps such a value only
for a session without a record.

Developers: when to change the marker is described in
[CALCULATIONS.md](CALCULATIONS.md#9-when-to-bump-calculationcompatibilityversion).

## 12. Stored calculation results

**What.** A result of an explicitly requested calculation (today only sensor
fusion) is stored when the calculation publishes it: a success with all its
outputs (measurements with their samples and unit, attributes including the
diagnostics), or a rejection or solver failure with its reason and
diagnostics. Nothing is stored for a computation that was cancelled, ran out
of memory, or whose inputs changed while it ran, nor for a calculation that
failed. On-demand and plugin results are never stored: they are recomputed in
milliseconds.

**Where.** The logbook folder holds:

```
FlySight Viewer/logbook/
  index.json                                   the logbook column cache (section 11)
  sessions/<uuid>.csv                          the recordings (section 9)
  cache/<uuid>.builtin%2Efusion%2Efit.fvresult the stored results
```

One file per (session, calculation) in `cache/`, a sibling of `sessions/`, with
no subfolders: `<stem>.<encoded calculation id>.fvresult`. `<stem>` is the
session file's name without `.csv`. In the id, every byte other than `a-z`,
`0-9`, `_` and `-` is written as `%` and two upper-case hex digits, so the
sensor fusion record of a session is `<uuid>.builtin%2Efusion%2Efit.fvresult`.
`sessions/` holds only the recordings; everything in `cache/` is derived.
`cache/` is created by the first record written, and a logbook without it
simply has no stored results. A record is written through a temporary file
that replaces the previous record at the end, like a session save. A session
that is imported and computed before its first save gets its record under the
name its first save will use.

`cache/` may be deleted while FlySight Viewer is closed. At the next start
every requested calculation reads as not requested until it is requested again
from the plot list, and the logbook column values that came from a stored
result are dropped (section 11) and show as unavailable.

**Format.** Binary, not meant to be read by people: the magic `FVRESULT`, a
format version (2), then the code stamp (`CalculationCompatibilityVersion`),
the calculation id, the result version, the reason, the input fingerprint
(SHA-256), the names of the inputs the result depended on, the outputs, and
what provided each name the result looked up (its resolutions, see "Validity").
Doubles are stored as their eight bytes, so a restored value is bit for bit the
published one: `-0`, NaN and the infinities included. Strings round-trip
exactly. An attribute is stored with its type, which must be one of: string,
byte array, boolean, double, float (stored as a double and read back as the
same float), and the 8-, 16-, 32- and 64-bit signed and unsigned integers
(`char`, `signed char`, `unsigned char`, `short`, `unsigned short`, `int`,
`unsigned int`, `qlonglong`, `qulonglong`). `long` and `unsigned long` are
refused, because their width differs between Windows and Linux / macOS; a
result holding any other type is not stored (the write fails and the result
stays in memory). A SHA-256 of everything before it closes the file. The size
is of the order of the session file. A damaged record, or one of another
format version (such as format 1 from development builds), is deleted as stale
at its session's next load, and the calculation has to be requested again.
There is no migration.
Only a regular file whose name ends exactly in `.fvresult` (lower case) is a
record: a directory at a record's path is never listed, so neither the
start-up pass nor deleting its session removes it.

**Validity.** A record is used only while all of these hold:

- **Code.** `CalculationCompatibilityVersion` (section 11) and the
  calculation's result version equal the ones it was written with. For sensor
  fusion the result version is the kernel's algorithm string, the
  `"algorithm"` of its diagnostics.
- **Inputs.** The names of the inputs the result reached are the same (source
  measurements with their unit text, attributes and declared preferences,
  directly or through other calculations, including inputs that were looked
  at and found absent), and a fingerprint over their current values equals
  the stored one.
- **Lookups.** For every name the result looked up, directly or through other
  calculations, the record states what provided it: a calculation (its id and
  its result version), the session's own data (a stored attribute or recorded
  data), or nothing. When the session is loaded the same lookups are repeated
  against the calculations registered now, and every answer must be the same.

So an edit the result does not depend on (a marker, a description) keeps the
record, and an edit it depends on (a merge that adds or changes IMU data, a
changed `SCHEMA_VER`, a changed local origin) makes it stale. What else is
registered, and preferences the result did not read, never matter: adding or
removing an altitude marker, loading a set of plugins whose calculations the
result never looked up, or changing the descent pause timeout keeps every
stored sensor fusion result valid. A new calculation that would now provide a
name the result looked up (a plugin that declares one of its inputs, say)
makes it stale. In short, a record goes stale when the same result in memory
would have been dropped, or when the code that computed it changed.

**Plug-in code identity.** Each Python plugin registration (attribute,
measurement or calculation) declares one result version, the plug-in code
identity: a SHA-256 digest, written `plugins-sha256:<hex>`, over every `*.py`
file under the plugin folder, subfolders included (its path relative to the
folder and its bytes; hidden files count, files in `__pycache__` and in hidden
folders are left out, and a linked folder is read through under its own name
without following a link back to a folder above it), the plugin SDK file, and
the Python and NumPy versions (`none` for a version that cannot be read). It
covers nothing else: data files, compiled modules and other packages a plugin
uses are not part of it. It is computed once, when the plugins are loaded at
start-up, and written to the log, with a warning naming any ingredient that
could not be read.
Editing, adding or removing any such file (also under `examples/`, which is
never imported) or upgrading Python or NumPy changes it for every plugin
registration at once, so a stored result whose lookups went through any
plugin calculation is stale at its session's next load, and the cached values
of the columns that can be computed through a plugin calculation are
discarded at start-up (section 11). A stored result whose lookups
touched no plugin calculation is unaffected. Plugin results themselves are
never stored.

**Lifecycle.**

- A record is written when the result is published, and replaced by the next
  publish for the same session and calculation.
- It is deleted when an input it depends on changes, when a change of the
  registered calculations made while the application runs drops its result
  (one that changes what a name it looked up resolves to: a calculation
  registered that provides a name it looked up and found missing, or the
  removal of the calculation that provided one, for example),
  when its session is deleted from the logbook, when it is found stale as the
  session is loaded, and at start-up when no session file with its stem exists
  in `sessions/` (that start-up pass deletes in `cache/` only).
- It is never deleted by hiding a track, unloading a session, quitting, or a
  change of the registered calculations that does not change what a name it
  looked up resolves to (a calculation registered for such a name that the
  session's own data or an earlier calculation still wins over, for example).
- When a session is loaded, every valid record is restored before anything
  reads the session, a record whose result read another stored result after
  that one. Restoring is not requesting: nothing is computed. A stale or
  missing record leaves the calculation not computed until it is requested
  again from the plot list.
- A record that exists but cannot be opened or read in full when its session
  is loaded (another program holding the file locked, for example) is skipped
  for that load: it is neither restored nor deleted, the calculation reads as
  not requested, and a warning is logged. A record whose result reads the
  result of a skipped record is skipped too. The next load tries again; a new
  publish for the same session and calculation replaces the record, and
  deleting the session, or the start-up pass for a session file that no longer
  exists, removes it. A file that is locked without sharing (on Windows) can
  be neither replaced nor removed while the lock lasts: such a publish fails
  like any failed write (next item), and deleting its session then leaves the
  record behind as a stray, which the start-up pass removes at the first start
  at which the file can be deleted. Logbook column values that depend on a
  skipped record are not cached in `index.json` while it stays skipped
  (section 11). A record that was read but is not a record, is damaged, or
  has another format version is deleted as stale.
- A write that fails (a full disk, say) leaves the previous record, if any,
  intact and the result in memory. The next publish tries again.

**Guarantees.** The session file is untouched: its bytes, its format and what
it lists do not depend on whether a record exists. Records are derived data.
Deleting them by hand, or the whole `cache/` folder, with Viewer closed is safe
and only means that the calculation has to be requested again. Existing
logbooks have no records and need no migration. Records are not meant to be
shared between logbooks or machines; one whose stamps do not match is simply
discarded. A logbook synced between machines whose plugins or NumPy versions
differ sees a different plug-in code identity on each, so each machine
discards the other's stored results that went through a plugin, and the
cached values of the columns over plugin calculations, at its next start.

## 13. What Viewer never does

- Infer the schema from the firmware version, a file name, a date, or the data.
- Rewrite recorded header attributes. None of them is editable in the logbook
  (the Device Name column, `DEVICE_ID`, is read-only); only `_` attributes are.
- Rename measurements or add prefixed / suffixed copies of them.
- Rescale stored data.
- Show a dialog, badge, or warning for legacy files.

## 14. Adding a future schema version

For maintainers:

1. Add rows to the table in `src/conversion/schematable.cpp` and the version to
   `supportedVersions()`.
2. Add a step to `src/conversion/sourceconversion.cpp` only if the correction
   is not a scale.
3. Bump `CalculationCompatibilityVersion`
   (`src/calculations/builtincalculations.h`).
4. Add fixtures to `tst_schema_units` and `tst_conversion_engine` (see
   [the test README](../tests/README.md)).
5. Update the table in section 3.

No session attribute, setting, or control is added.
