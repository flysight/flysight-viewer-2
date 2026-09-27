# Altitude time references

Date: 2026-09-19
Status: specification for planning. Not an implementation plan. Parked until
the source-preservation / registered-calculations work
(`schema-and-calculations`) is closed off.
Baseline: the head of `schema-and-calculations` once merged. This change
builds on its calculation engine and is meaningless without it.

This document describes what the change must do and the architectural
boundaries it must respect. It deliberately avoids class layouts, container
choices, and function signatures unless a name is part of the observable
contract. The planning agent decides phasing; the implementation agent decides
code structure within these boundaries.

## 1. Motivation

A logbook column that reads a measurement "at a marker", or the change in a
measurement "between two markers", stores nothing about markers. It stores the
name of a session attribute whose value is a time. The interpolation behind
the column accepts any attribute name. "Marker" is the word the Add Column
dialog uses for "a named time".

An altitude marker is two unrelated things created together:

- A calculation whose output is a time attribute named
  `_ALTITUDE_<n>_<FT|M>`: the time the track last descends through `<n>`
  feet or metres above ground level within the analysis window. The threshold
  is derived from the name.
- A marker definition that draws that attribute on the plot.

Today the calculation exists only while an entry in Preferences asks for it:
the altitude marker manager registers one calculation per listed altitude and
unregisters it when the entry goes. Three problems follow.

- Altitude markers cannot be used in logbook columns. The Add Column dialog
  skips them, because a column that depended on one would go blank whenever
  the Preferences list changed or the units were switched. A user cannot make
  a column such as "change in vertical speed between 3000 ft and 2000 ft".
- Every change to the altitude list changes the set of registered
  calculations, which is a change of calculation environment: the cached
  column values of every session in the logbook are discarded and recomputed,
  although no value that any column shows has changed.
- Registering and unregistering at runtime is the only dynamic registration in
  the application, and has already produced two defects (stale values after an
  unregister in the old engine; a fingerprint that depended on registration
  order in the new one).

The attribute name is the condition. A marker on the plot and a column in the
logbook are siblings: two consumers of the same named time, neither owning the
other. This change makes the code say so.

## 2. Scope

In scope:

- The altitude-crossing time as one parameterized calculation that is always
  available, selected by attribute name, with today's name syntax and today's
  crossing rule.
- The altitude marker manager reduced to display: it maintains marker
  definitions for the altitudes listed in Preferences and registers nothing
  with the calculation engine.
- Logbook columns that take their time reference either from a marker or from
  an altitude, for both "measurement at" and "delta between" columns, each end
  of a delta chosen independently.
- One shared authority for building, parsing, and labelling an altitude
  attribute name.
- Automated tests for the behavior described here.
- Documentation updates where altitude markers or logbook columns are
  described.

Out of scope:

- Any other crossing rule (first crossing, upward crossing, a crossing outside
  the analysis window, altitude above mean sea level). A future rule is a new
  name form, not a change to this one.
- Non-integer altitudes, and units other than feet and metres.
- Other kinds of condition (a time offset from a marker, a speed threshold).
  This change must not preclude them, and does not build them or a general
  framework for them.
- Any change to how markers are drawn, enabled, coloured, or ordered, or to
  the Altitude Markers preferences page.
- Any change to the column cache, the environment fingerprint, or the engine,
  beyond what section 4 requires.

## 3. Concepts

### 3.1 Time reference

A time reference is a session attribute name whose value is a time in the
session's UTC time base. Fixed events (`_EXIT_TIME`, `_START_TIME`, ...),
markers the user has placed, and altitude crossings are all time references.
A logbook column holds time references by name, exactly as it does today. No
new field is added to a column to say what kind of reference it holds; the
name says it.

### 3.2 Altitude attribute name

The name is `_ALTITUDE_<n>_<unit>` where `<n>` is a decimal integer, optionally
negative, and `<unit>` is `FT` or `M`. This is today's syntax, and names
already stored in preferences, marker state, and column definitions keep their
meaning. `_ALTITUDE_3000_FT` and `_ALTITUDE_914_M` are different names with
different thresholds; no equivalence between units is inferred.

The only validation is well-formedness. There is no minimum and no maximum:
altitude above ground level can legitimately be negative, and no ceiling is
defensible. A name that does not parse (`_ALTITUDE_abc_FT`,
`_ALTITUDE_3000_YD`, `_ALTITUDE__M`, a value outside the integer range) is not
an altitude attribute name and is not given a calculation; reading it reports
no value, as any unknown attribute does.

Exactly one piece of code builds a name from a value and unit, parses a name
into a value and unit, converts the threshold to metres (feet x 0.3048), and
produces the display labels (`3000 ft AGL`, `3000ft`). The calculation, the
marker manager, the Add Column dialog, and the column header all use it.
Nothing else formats or parses these names.

## 4. The altitude calculation

### 4.1 Registration

The altitude crossing is registered once, at startup with the other built-in
calculations, as a parameterized calculation: it is instantiated on demand for
any well-formed altitude attribute name that something reads, with distinct
results and dependencies per name, like the interpolation and conversion
calculations. It has one stable identity. No per-altitude registration exists,
at startup or at runtime, and nothing is unregistered when Preferences change.

It is registered after the existing built-ins, so its position in the order of
parameterized calculations relative to them is fixed.

### 4.2 Value

Unchanged from today: the time of the last downward crossing of the threshold
within the analysis window, linearly interpolated between the two samples
either side, using altitude above ground level (`GNSS/z`) and the GNSS time
vector. No crossing in the window means the attribute has no value.

Its declared inputs are today's: the analysis window start and end, `GNSS/z`,
and the GNSS time vector. It is a pure function of them.

### 4.3 Precedence

A stored session attribute of the same name keeps precedence over the
calculation, as for every attribute. The user interface never creates one
(altitude markers are not draggable), but a file may carry one, and setting or
removing it invalidates dependents like any other attribute change.

### 4.4 Caching and invalidation

No special handling. The rules below are consequences of the declared inputs
and must hold; tests should demonstrate them rather than the implementation
adding mechanisms for them.

- A result, including "no crossing", is computed once per session and kept
  until a declared input changes.
- Setting or removing `_GROUND_ELEV` invalidates every altitude attribute of
  that session, through `GNSS/z`. When `_GROUND_ELEV` is not stored, its
  calculated fallback changes with the analysis window, and altitude
  attributes follow.
- A change to the analysis window invalidates them, including through the
  descent-pause preference that the analysis range declares.
- Replacing GNSS source data (merge, re-import) invalidates them.
- Adding, removing, or changing the units of altitude markers in Preferences
  invalidates nothing, discards no cached column value, and does not change
  the calculation environment fingerprint.
- For sessions that are not loaded, the cached value of a column that uses an
  altitude reference is refreshed when, and only when, something in that
  column's dependency closure changes for that session. The closure of such a
  column contains `_GROUND_ELEV`, the analysis window, and the GNSS
  measurements it reads. Editing the ground elevation of many sessions at once
  refills only the columns that depend on it.
- A change to the crossing rule in code is a change of calculation semantics
  and requires the calculation-compatibility marker to be incremented. Making
  this calculation parameterized is not such a change: every value is the same
  as before.

### 4.5 Acyclicity

The dependency shape is altitude -> `GNSS/z` -> `_GROUND_ELEV` -> analysis
window -> `GNSS/hMSL`, and altitude -> analysis window -> `GNSS/hMSL`. No
built-in calculation may declare an altitude attribute as an input. This keeps
the built-in graph acyclic, which the engine's documented limitation on
overlapping cycles requires.

## 5. Altitude markers

The altitude marker manager keeps marker definitions in step with the
altitudes and units in Preferences: category, display name, short label,
colour, the "altitude" group, enabled by default. That is all it does. It
holds no calculation registrations and has nothing to undo when it is
destroyed.

Observable behavior is unchanged: the same markers appear on the plot and in
the marker list, with the same names, colours, enable state, and ordering;
adding, removing, and switching units take effect immediately; a marker for an
altitude the track never crosses shows nothing. Marker state keyed by
attribute name (enabled flags, colours) keeps working because the names do not
change.

Whether a name has a marker and whether it has a value are now independent. A
marker exists because Preferences list it. A value exists because the name is
well-formed and the track crosses the altitude.

Anything that asks whether an attribute can be calculated (for example the
"Reset to default" check on a marker bubble) gets the right answer for
altitude names without special cases.

## 6. Logbook columns

### 6.1 Choosing a time reference

In the Add Column dialog, wherever a marker is chosen today ("Measurement at
Marker": one; "Delta between Markers": from and to), the user chooses either a
marker or an altitude. Choosing an altitude means entering a value and a unit.
Each end of a delta is chosen independently, so a delta may run between two
markers, two altitudes, or one of each.

The choice is expressed in the existing dialog; no new dialog, page, or
preference is added. The unit offered by default follows the Altitude Markers
units preference. The altitudes offered are not limited to those listed in
Preferences, and entering one does not add it there.

The resulting column is an ordinary column whose time reference is an altitude
attribute name. Its stored form, its identity for caching, and its identity
for duplicate detection are what they would be for any other name. Two columns
that differ only in altitude or unit are different columns; two that name the
same altitude and unit and the same measurement are duplicates and are
collapsed as duplicates are today.

Markers in the "altitude" group remain absent from the marker list in this
dialog. The altitude choice replaces them, and offering both would present two
ways to make the same column.

### 6.2 Labels

A column header derives its text for an altitude reference from the name,
through the shared authority of section 3.2 (for example `3000 ft AGL`). It
never consults the marker registry for it. A user-supplied custom label keeps
precedence, as today. The label reflects the unit in the name, not the current
units preference.

### 6.3 Independence from Preferences

A column that uses an altitude reference is unaffected by the Altitude Markers
preferences: removing the matching marker, removing all markers, or switching
units leaves the column's definition, header, and values as they were.

### 6.4 Values

Computed as for any marker: the measurement interpolated at the time
reference, or the difference between two. A session whose track never crosses
the altitude shows an empty cell, cached as "no value" and not recomputed
until something it depends on changes.

Adding such a column to a logbook fills that column only. It does not discard
or recompute any other column.

## 7. Compatibility

No migration. Attribute names, preferences, marker state, column definitions,
session files, and `index.json` are unchanged in form. Every altitude value is
the same as before.

Because the set of registered calculations changes once (per-altitude
registrations disappear, one parameterized calculation appears), the
calculation environment fingerprint changes once on upgrade, and logbooks
recompute their cached columns once. That is expected and needs no handling.
The calculation-compatibility marker is not incremented.

## 8. Tests that change

- Tests that use adding or removing an altitude marker as their example of a
  registry change invalidating every session must use another registration.
  The property they test still holds; altitude markers no longer exercise it.
- The test that an altitude marker added at runtime leaves the fingerprint
  equal to a fresh start's is superseded by the stronger statement in 4.4 and
  should be replaced by it.
- Tests that assert the full list of registered calculation identities change
  by the removal of per-altitude entries and the addition of one.

## 9. Acceptance

Demonstrated by automated tests using generated fixtures in temporary
directories, never the user's logbook or preferences, with expected values
stated independently. Dialog layout is verified by hand.

1. On a descent fixture, `_ALTITUDE_<n>_FT` and `_ALTITUDE_<n>_M` read the
   expected crossing times with no altitude listed in Preferences and no
   marker defined. The values equal those produced before this change for the
   same names.
2. A negative altitude and a very large altitude are accepted as names; each
   reads a value when the track crosses it and no value when it does not. A
   malformed name reads no value and creates no calculation.
3. Reading several altitude names in one session computes each once; reading
   them again computes nothing. A track that never crosses an altitude is
   scanned once, not on every read.
4. Setting, changing, and removing `_GROUND_ELEV` each cause exactly one
   recomputation of an altitude attribute on its next read, and the new value
   reflects the new ground elevation. An unrelated attribute change causes
   none.
5. Changing the descent-pause preference invalidates altitude attributes;
   replacing GNSS source data through a merge invalidates them.
6. Adding, removing, and re-uniting altitude markers through the real marker
   manager leaves the calculation environment fingerprint unchanged, discards
   no cached column value, recomputes nothing, and leaves the set of
   registered calculations unchanged. The markers themselves appear and
   disappear as before.
7. A "measurement at altitude" column and a "delta between two altitudes"
   column produce the expected literal values; a delta between a marker and an
   altitude works in both orders.
8. With such columns in a logbook of loaded and unloaded sessions: editing one
   session's ground elevation refreshes only that session's altitude-dependent
   columns; a bulk ground-elevation edit refills only altitude-dependent
   columns; the saved `index.json` agrees with the session files after a
   restart, and a restart computes nothing.
9. Removing the matching altitude marker, and switching the units preference,
   leave an altitude column's definition, header text, and values unchanged.
10. Header text for an altitude reference is derived from the name and is
    correct when no marker for it exists. A custom label overrides it.
11. A stored attribute with an altitude name takes precedence over the
    calculation, and removing it restores the calculated value.
12. The dependency closure of an altitude column contains `_GROUND_ELEV`, the
    analysis window, and the GNSS measurements, and randomized sequences of
    reads, edits, and merges that include altitude names equal a fresh
    evaluation with caches cleared.
13. Two columns naming the same altitude, unit, and measurement are collapsed
    as duplicates; columns differing in altitude or unit are kept.
14. The full application builds; altitude markers behave as before on the
    plot; no per-altitude registration or unregistration remains in the code.

## 10. Principles for the implementers

- The name is the condition. Nothing owns an altitude value: not a marker, not
  a column, not a preference.
- One authority for the name: one place builds it, parses it, converts it, and
  labels it.
- Markers are display. Whether something is drawn on the plot never decides
  whether it can be calculated.
- Remove the dynamic registration rather than leaving it beside the new
  calculation. After this change nothing in the application registers or
  unregisters a calculation in response to a preference.
- Let the engine do the caching. If a test needs a special invalidation rule
  to pass, a declared input is missing.
- Same values as before. This change moves where the calculation lives and who
  may use it; it does not change what it computes.
