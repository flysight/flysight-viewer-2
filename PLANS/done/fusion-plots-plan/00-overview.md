# Implementation plan: Sensor fusion plots, attitude and the orientation attribute

## Feature Specification

## Sensor fusion plots, attitude and the orientation attribute

Date: 2026-09-28
Status: specification for planning. Not an implementation plan.
Baseline: branch `store-requested-calculations` at its head (the status bar
work and its fixups are implemented; the committed code is authoritative).
Where this document and the earlier specifications disagree, this one wins;
everything it does not mention stays as they specify.
Related: `docs/SENSOR_FUSION.md`, `docs/CALCULATIONS.md` (sections 4, 5 and
17), `docs/COMPUTED_PLOTS.md`, `docs/LOCAL_COORDINATES.md`,
`docs/DATA_SCHEMA.md`, `tests/README.md`, `tests/acceptance_map.txt`,
`tests/audit/cleanup_audit.cmake`. A later specification adds the fit's
uncertainty outputs (`PLANS/fusion-accuracy.md`); nothing here depends on it.

This document says what the change must do and the boundaries it must
respect. It avoids class layouts and function signatures unless a name is
part of the observable contract. The planning agent decides phasing; the
implementation agent decides code structure.

### 1. Motivation

The "Sensor fusion" category of the plot list shows the fit's seventeen
output channels as seventeen plots: position, velocity and acceleration by
north, east and down component, three Euler angles and the four quaternion
components. Almost none of them is a plot anyone wants. Nobody plots a
quaternion. Position and velocity are measured well by GNSS already, and
nobody has asked to see the fused ones. The users this feature exists for,
skydivers and people doing aeronautical test and measurement, think in the
quantities the GNSS categories already offer: speeds, course, glide, dive
angle, accelerations. Where fusion genuinely improves on GNSS is in two
places: attitude, which GNSS cannot give at all, and acceleration, which the
GNSS categories derive by finite differences of position and which is the
noisiest thing the application plots.

The attitude plots have a second problem. Roll, pitch and yaw are the Euler
angles of the device's own x axis, and the fit assumes nothing about how
the device is mounted. A FlySight 2 on the back of a helmet has its x axis
pointing right, its y axis roughly forward and its z axis up. Its "yaw" is
therefore the heading of an axis that points sideways, and its "roll" is a
rotation about that sideways axis. There is no way to tell the application
which way the unit was mounted.

Finally, the "GNSS (Local frame)" category exists to compare the local-frame
components with the fused ones. Once the fused components leave the plot
list, it has no purpose.

This change trims the fusion category to the plots that earn their place,
derives attitude in the aircraft convention on a body frame the user can
declare, and gives constant attribute defaults the home the data-derived
ones already have.

### 2. Principles

- **A fused plot earns its place** by being a quantity the user already
  reads from GNSS, offered under the same name so the two overlay, or by
  being something only the IMU can give. Frame components are neither.
- **The fit stays the fit.** Its outputs, its algorithm string and its
  stored results are unchanged. Everything new is derived on demand from
  what the fit publishes, as horizontal acceleration is today.
- **The quaternion is the attitude.** Euler angles are a presentation of it
  in a chosen frame, computed when read, never stored by the fit.
- **The orientation attribute describes the mount and nothing else.** It
  says which device axes point forward and up. It says nothing about the
  posture of whoever wears the device; the documentation says so.
- **A default is a calculation.** The importer stores only what is a fact
  of the import; anything that stands in for a value the user has not set
  is a registered calculation, derived where possible, constant otherwise.
- **A profile that names a plot the application does not have is applied
  without it**, silently, now and in future.

### 3. Scope

In scope:

- The "Sensor fusion" category reduced to eight plots (section 5), the
  removal of the "GNSS (Local frame)" category, and the stated rule for
  profiles that name removed plots.
- Elevation and the four accelerations as on-demand derivations from the
  fit's outputs (section 6).
- Heading, pitch and roll derived from the quaternion on the body frame the
  orientation attribute defines, heading unwrapped (section 7).
- The orientation attribute: its vocabulary, its constant default, how it
  is displayed and edited (section 8).
- A "choice" attribute format type in the attribute registry, generic, used
  by the orientation attribute (section 9).
- Constant attribute defaults as registered calculations, with one helper,
  and the wind defaults moved from the importer onto it (section 10).

Out of scope, unchanged:

- The fit: its inputs, kernel, outputs, diagnostics, algorithm string,
  stored results and golden fixtures. The fit's own roll, pitch and yaw
  channels remain measurements (columns, export and plug-ins still read
  them); they simply have no plot.
- Fused position and velocity plots of any kind. The fit's position and
  velocity channels stay as measurements.
- A posture offset between the device and the body it is mounted on, and
  angle of attack or sideslip derived from one.
- The fit's uncertainty outputs and the accuracy plots
  (`PLANS/fusion-accuracy.md`).
- The demand layer, the executor, the status bar and the logbook's
  presentation of computed values: a derived plot creates demand for the
  fit exactly as horizontal acceleration does today.
- Jumper mass, planform area and the fixed ground elevation of the import
  settings stay creation defaults: each is a fact of the import (a snapshot
  the user chose then).

### 4. Terms

- **Device frame**: the FlySight's own axes, in which the IMU measures and
  in which the fit's quaternion expresses the body-to-north-east-down
  rotation.
- **Body frame**: the aircraft frame of forward, right and down, in which
  heading, pitch and roll are defined.
- **Orientation**: which device axis points forward and which points up,
  each one of +x, −x, +y, −y, +z, −z, the two neither equal nor opposite.
  Forward and up fix the body frame: right follows from them.
- **Choice attribute**: an attribute whose value is one of a fixed list,
  each entry a stored token and a display label.
- **Constant default**: a registered calculation with no inputs whose
  output is an attribute's default value.

### 5. The plot list

The "Sensor fusion" category holds exactly these plots, named and typed as
their GNSS counterparts so that the two overlay:

| Plot | From | Unit and type as |
| --- | --- | --- |
| Elevation | origin height minus fused down, above ground (section 6) | GNSS (Basic) Elevation |
| Horizontal acceleration | as today | GNSS (Advanced) Horizontal acceleration |
| Vertical acceleration | fused down acceleration | GNSS (Advanced) Vertical acceleration |
| Along-track acceleration | section 6 | GNSS (Advanced) Along-track acceleration |
| Cross-track acceleration | section 6 | GNSS (Advanced) Cross-track acceleration |
| Heading | section 7 | GNSS (Basic) Course |
| Pitch | section 7 | angle |
| Roll | section 7 | angle |

Removed from the plot list: the fused north, east and down position, the
north, east and down velocity, the north and east acceleration, roll, pitch
and yaw, and the four quaternion components; and the whole "GNSS (Local
frame)" category. Every measurement behind a removed plot still exists and
is still computed as before: the local-frame calculation is the fit's input,
and the fit's channels are its record. An existing logbook column over such
a measurement keeps computing, labelled by the measurement's name; the
column editor lists plots, so a new column over a removed plot cannot be
added, and nothing is added to make that possible.

**Profiles.** A profile lists the plots it enables by id. Applying a
profile enables the listed plots the application has and ignores the rest,
silently: no message, no log, no failure. This is the application's
behaviour today and becomes a stated rule with a test. Nothing rewrites a
profile.

### 6. Derived kinematics

Each is an on-demand calculation over the fit's outputs, registered with
the fit as horizontal acceleration is: its inputs exist only once the fit
has published, so it appears with the fit and never starts one.

- **Elevation**: the local frame origin's height above mean sea level (an
  attribute the local-frame calculation already provides) minus the fused
  down position, minus the ground elevation, because the GNSS elevation is
  height above ground; unavailable when the ground elevation is not a
  number, as the GNSS one is. The two then overlay directly.
- **Vertical acceleration**: the fused down acceleration, presented as the
  GNSS one is.
- **Along-track and cross-track acceleration**: the same definitions as
  the GNSS ones, applied to the fused velocity and acceleration, so the
  signs and the meaning of "track" agree between the two categories.

### 7. Attitude

**The body frame.** The orientation attribute (section 8) names the device
axes that point forward and up; right is their cross product. The rotation
from the body frame to the device frame is a constant axis permutation with
signs, a proper rotation, never a reflection.

**The angles.** From the fit's body-to-north-east-down quaternion composed
with that constant, heading, pitch and roll are the aircraft Euler angles:
heading is the direction of the forward axis measured clockwise from north,
pitch is the elevation of the forward axis above the horizontal, and roll is
the rotation about the forward axis, positive right side down. They are
computed when read, from the quaternion channels and the orientation
attribute, never by the fit.

- **Heading is a compass heading**, unwrapped by the application's one
  unwrap rule and referenced to nothing. The GNSS course is relative to its
  course reference, so the two are not meant to overlay, and the
  documentation says so. The attitude reads the quaternion and the
  orientation only: every fitted recording has heading, pitch and roll.
- **Pitch and roll** are reported in their natural ranges.
- **Where the forward axis is vertical**, heading and roll are not defined;
  the derivation reports what the standard formulas give, and the
  documentation says that a mount, or a posture, that points the forward
  axis straight up or down makes them meaningless.

A change of the orientation attribute recomputes the angles through
ordinary invalidation and never refits.

### 8. The orientation attribute

- **The value** is a token naming the forward and the up axis, in a fixed
  form the fusion module owns, for example the default `+y,+z`. The 24
  valid pairs, and only they, are the attribute's choices; the display
  label of each is "forward +y, up +z" and so on, the default first.
- **The default** is a constant calculated attribute (section 10): forward
  +y, up +z, which is a FlySight 2 on the back of a helmet with its label
  up. Every recording has it from the moment the code exists, imported
  before or after, with nothing written into any session file. A stored
  value wins over it; removing the stored value returns to it.
- **Where it is defined.** One orientation type in the fusion module owns
  the vocabulary: the pair of axes, the token, the label, the body-frame
  rotation, the default and the enumeration of valid pairs. The attitude
  derivation parses tokens with it, the attribute definition fills its
  choices from it, and nothing else spells a token.
- **Where it is registered.** The fusion registration registers the
  attribute definition (a choice attribute, editable, in a category of its
  own or beside the other session attributes as the planner sees fit) with
  the derivation that reads it. Its key follows the session keys'
  conventions and is part of the session file's vocabulary.
- **How it is edited.** As any editable attribute: a logbook column the
  user can add (not in the default columns), edited in place, and set for
  the selected sessions from the logbook's context menu. Both offer the
  list, not free text; both offer a "Default" entry that removes the
  stored value rather than storing anything. A value outside the list is
  refused.

### 9. The choice attribute type

The attribute registry's format types (text, date and time, duration,
number) gain a fifth, **choice**. A definition of that type carries its
list of allowed values, each a stored token and a display label. From the
type alone follow: display and sorting by label; a list editor in place of
the line edit for in-place editing; a list in place of the text prompt in
the context menu's "Set ..." action; the same bulk edit with the chosen
token; a "Default" entry that removes the stored attribute; refusal of a
token outside the list. Nothing about the type is specific to orientation;
a later enumerated attribute reuses it.

### 10. Constant defaults

A constant default is a registered calculation with no inputs whose one
output is the attribute. The engine already supports it: the calculation
runs once and is cached, a stored value wins over it, setting or removing a
stored value invalidates what read the attribute, and a dependent
calculation follows every change (`tst_calcengine::constantCalculationIsADefault`).

- **One helper** registers a constant default for an attribute key and a
  value, beside the exit-time defaults of the attribute calculations, so
  that every constant default in the application is found by one search.
  The orientation default uses it from the fusion registration.
- **The rule**, stated in the calculations document: the importer stores
  only what is a fact of the import (identity, provenance, a choice the
  user made at import); anything that stands in for a value the user has
  not set is a calculation, derived where possible, constant otherwise.
- **Wind north and wind east** move from the importer's creation defaults
  onto the helper, as constants of zero. A recording that has them stored
  keeps them, since stored wins; a recording without them reads zero as
  before. The importer's placeholder comment goes with them.
- Jumper mass, planform area and the fixed ground elevation stay in the
  importer (section 3).

Note one property of the engine that the editors must respect: a stored
attribute wins even when its value is invalid or empty. Returning a
recording to its default therefore removes the stored attribute; it never
stores a blank.

### 11. What the user sees

- **The plot list**: "Sensor fusion" with eight plots, named as the GNSS
  plots; no "GNSS (Local frame)" category. Checking a fusion plot starts
  the fit as it does today; the status bar shows it; a stored fit draws at
  once.
- **Attitude** plots in the aircraft convention for the mount the
  orientation attribute describes; heading continuous through turns.
- **The orientation column**, once added, shows "forward +y, up +z" for
  every recording that has not been set, and the chosen label for one that
  has; editing it offers the list and "Default". Changing it redraws the
  attitude plots without a fit.
- **Profiles** that name removed plots apply without complaint.
- **Wind** columns show zero where nothing was stored, as before.

### 12. Architecture

- **The fusion kernel and the fit calculation** are unchanged: same inputs,
  outputs, diagnostics and algorithm string; the golden fixtures and the
  stored results stay valid. The audit's confinement of GTSAM to the fusion
  kernel and its rules on the fusion tooling hold.
- **The fusion registration** gains the derived kinematics, the attitude
  derivation, the orientation type and the registration of the orientation
  attribute with its constant default. The derivations are ordinary
  on-demand calculations with declared inputs; the attitude derivation
  declares the orientation attribute among them.
- **The attribute registry** gains the choice type and its list; the
  logbook's model, cell editing and context menu present and validate it;
  the bulk edit carries a token as it carries text today.
- **The attribute calculations** gain the constant-default helper and the
  two wind defaults; the importer loses them.
- **The plot registry** loses the removed plots and the local-frame
  category; the profile bridge keeps its silent rule.
- **The demand layer** is untouched: a derived plot's blockers lead to the
  fit through the chain it already follows.

### 13. Tests

- The plot list has the eight fusion plots and no local-frame plots; every
  fusion plot is explicit-backed (waits on the fit) as the seventeen were;
  an existing column over a removed plot's measurement still computes, and
  export still writes the measurement.
- Applying a profile that names a removed plot enables its other plots and
  ignores that one, silently.
- Elevation equals the origin height minus down minus the ground elevation,
  and is unavailable without a ground elevation; vertical acceleration is
  the down acceleration; along-track and cross-track equal the GNSS
  definitions applied to the fused velocity and acceleration, on synthetic
  data with known answers.
- Attitude, on synthetic quaternions with known answers: for the default
  orientation, heading, pitch and roll of a level, north-facing body are
  zero; a known yaw, pitch and roll come back; heading unwraps through a
  full turn and is measured from north, unchanged by a GNSS track or a stored
  course reference and available without GNSS data; a different orientation (a side mount) changes the angles as
  the axis permutation predicts; the rotation is proper for all 24 pairs.
- The orientation attribute: every recording reads the default without a
  stored value and without any write to its file; a stored token wins and
  the attitude recomputes without a fit; "Default" removes the stored
  value; a token outside the list is refused by the model, the editor and
  the bulk edit; the labels and tokens come from one enumeration of 24.
- The choice type: display and sort by label; the in-place editor and the
  context menu offer the list; the bulk edit sets the token for the
  selected sessions.
- Constant defaults: the helper registers a calculation that the engine
  serves as a default (the existing engine test covers the mechanism); wind
  north and east read zero for a recording without stored wind, keep a
  stored value, and the importer no longer writes them.
- The fit is unchanged: the golden fixtures and the stored-result tests
  pass unchanged; the algorithm string is the same.
- The audit keeps the removed plot names out of the registry and the
  documents describe the eight plots, the attitude convention, the
  orientation attribute and the rule for defaults.

### 14. Documentation

`docs/SENSOR_FUSION.md` (section 2, the plot list and the attitude
convention with the orientation attribute and its limits; section 4, the
outputs table gains the derived quantities); `docs/CALCULATIONS.md`
(section 5, the rule for defaults and the constant-default helper beside the
existing precedence rule; section 17, the plots and derivations);
`docs/COMPUTED_PLOTS.md` (the fusion category as the user sees it);
`docs/DATA_SCHEMA.md` (the orientation attribute in the session file and
the choice type, and that wind defaults are no longer written at import);
`docs/LOCAL_COORDINATES.md` where it names the local-frame plots;
`tests/README.md` and `tests/acceptance_map.txt` (a new range from 801;
amended items restated "(as amended)"); the audit's document rule extended
for the removed plots.

### Commit Policy

Michael has authorized commits for this plan on the existing working branch.
The implementation orchestrator makes every commit; implementation, revision,
and review agents never run git commands that change repository state.
Nothing is ever pushed, and nothing is ever committed on `master` or
`fusion-improvements`. Copy this section into the plan overview as its own
top-level section with this exact heading.

- **Branch.** Confirm the working tree has no modified tracked files (if there
  are any, stop and ask). Switch to the existing branch
  `store-requested-calculations`; never recreate, reset, or rebase it.
- **Never staged:** `PLANS/implementation-plan/`, `TEMP/`, `experiments/`,
  `build*/`, `dist/`, `results/`, and everything under `third-party/` that
  is not tracked. Specifications and archived plans under `PLANS/` are
  committed by Michael, never in a phase commit.
- **One commit per accepted phase**, when the review agent returns ACCEPT,
  containing exactly the files the phase's agents reported, staged by explicit
  path. Subject `Fusion plots phase N: <phase name>`; body a short summary,
  then the session's attribution line. Rejected iterations are never
  committed.
- **Tag** each phase commit `plan/fusion-plots/phase-N-done`; tags are never
  moved or deleted.
- **Fixes to a closed phase** are committed separately as
  `Fusion plots phase N fixup: <what>` once reviewed and accepted.
- **CI workflow edits** are committed separately as
  `Fusion plots phase N: CI (unverified)`.
- **Escalated phases** are not committed; their paths are listed in the final
  report, and phases that depend on them are not started.

## Phases

| Phase | Name | Purpose | Dependencies |
|-------|------|---------|--------------|
| 1 | The choice attribute type | Spec §9 and the generic half of §8's editing: the attribute registry's fifth format type with its list of token and label; the logbook model's display and sort by label, validation of a token, and "Default" as removal of the stored attribute; the list editor for in-place editing; the list in the context menu's "Set ..." action; the bulk edit carrying a token or a removal. Generic: tested with a test-registered choice attribute; no product attribute uses it until phase 4. | none |
| 2 | Constant defaults | Spec §10: the one constant-default helper (header-only, decision 18), used beside the exit-time defaults; wind north and east as constants of zero; the importer's wind defaults and its placeholder comment go, and so does the wind half of the logbook's legacy backfill (decision 10); `docs/CALCULATIONS.md` §5 states the rule; `docs/DATA_SCHEMA.md` says wind is no longer written at import. | none |
| 3 | Derived kinematics | Spec §6: fused elevation, along-track and cross-track acceleration as on-demand calculations over the fit's outputs, registered with the fit; the track-relative definition shared with the GNSS calculations, one authority; vertical acceleration is the fit's own down acceleration (decision 1). A new test executable with synthetic fit outputs. `docs/SENSOR_FUSION.md` §4 and `docs/CALCULATIONS.md` §17 gain the derivations. | none |
| 4 | Orientation and attitude | Spec §7 and §8: the orientation type that owns the vocabulary; the orientation attribute (a choice attribute, key, category, constant default through phase 2's helper) registered by the fusion registration; the attitude derivation (heading, pitch, roll on the body frame, heading unwrapped); `docs/SENSOR_FUSION.md` (attitude convention, the attribute and its limits; §4 rows), `docs/CALCULATIONS.md` §17, `docs/DATA_SCHEMA.md` (the attribute in the session file, the choice type). | 1, 2, 3 |
| 5 | The plot list | Spec §5 and the plot half of §11: the "Sensor fusion" category reduced to the eight plots, the "GNSS (Local frame)" category removed; the profile rule stated with a test; the tests' mirror of the plot list and every test over the seventeen or the six plots; the audit's plot counts and its document rule for the removed plots; `docs/COMPUTED_PLOTS.md`, `docs/LOCAL_COORDINATES.md`, `docs/SENSOR_FUSION.md` §2 (plot list), `docs/CALCULATIONS.md` §17 (plots), the root `README.md`'s document list. | 3, 4 |
| 6 | Traceability and the test guide | Spec §13 and §14's test-guide half: `tests/README.md` (section 9.9, section 10 for the rules phases 2-5 added, section 12.7 manual steps, appendix I), `tests/acceptance_map.txt` (items 801 onward, amended items restated "(as amended)"), the audit's traceability range for the new items. | 5 |

```text
1 The choice attribute type ──┐
2 Constant defaults ──────────┼──> 4 Orientation and attitude ──> 5 The plot list ──> 6 Traceability
3 Derived kinematics ─────────┘
```

Phases 1, 2 and 3 depend on nothing and their documents are written in
parallel. **They are implemented one at a time, in the order 1, 2, 3**: every
phase builds and runs the whole suite in the one `build-agent/` tree, which
`CLAUDE.md` and `CLAUDE.local.md` allow to one build or ctest at a time, and
phases 2 and 3 both edit `docs/CALCULATIONS.md` and may both add rules to
`tests/audit/cleanup_audit.cmake`. Phase 4 needs phase 1's type, phase 2's
helper, and phase 3's state of `src/fusion/fusionregistration.*`, the new
test executable and the fusion documents. Phase 5 needs the measurements
that phases 3 and 4 publish, since each plot row names one. Phase 6 cites the
test functions and audit groups as phases 1-5 leave them.

## Key patterns and references

Locate every passage by quoting it; line numbers go stale.

### Project rules and earlier work

- `CLAUDE.md`, `CLAUDE.local.md`: build tree (`build-agent/` only), sequential ctest, audit and acceptance-map rules, documentation duty, conventions.
- `PLANS/done/status-bar-plan/00-overview.md` and its `05-*.md`: the most recent plan; the pattern for a final traceability phase (item numbering, appendix, section 9.x, the audit range).
- `PLANS/done/fusion-improvements.md`, `PLANS/done/sensor-fusion-jobs.md`: the earlier fusion specifications whose acceptance items (appendices B and C of `tests/README.md`) this change amends.
- `PLANS/fusion-accuracy.md`: a later specification that adds four plots to the category; nothing here may make that harder (it is not implemented here).

### Attribute registry, logbook model and editing (phases 1, 4)

- `src/attributeregistry.h`, `src/attributeregistry.cpp`: `AttributeFormatType` (four types today), `AttributeDefinition` (aggregate, brace-initialized by every registration), `findByKey()`.
- `src/calculations/attributeregistration.cpp`: the built-in definitions (categories "Session", "Location", "Aerodynamics"); the pattern for a definition.
- `src/calculations/spcalculations.cpp`, `src/calculations/wspcalculations.cpp`: other registrations of definitions, brace-initialized; they must keep compiling unchanged.
- `src/sessionmodel.cpp`: `formatAttributeValue()`, `formatRawValue()` (display by format type), `sort()` (`useStringCompare` from the format type), `setData()` for `Qt::EditRole` on a `ColumnType::SessionAttribute` column (per-type switch, `invalidateColumns`, `publishInvalidation`, `scheduleSave`), `flags()` (`Qt::ItemIsEditable`), `startBulkEdit()` and `processNextBulkEdit()` (the per-type switch "a format type that is not edited in bulk", the loaded and the stub paths).
- `src/sessionmodel.h`: `startBulkEdit(const QList<int> &rows, int columnIndex, const QVariant &value)`, `BulkEditItem`.
- `src/sessiondata.h`, `src/sessiondata.cpp`: `setAttribute()`, `removeAttribute()` (both return the changed names), `hasAttribute()` (stored only), `hasStoredAttribute()`, `getAttribute()` (the engine's effective value: stored first, then calculations); `SessionKeys`.
- `src/ui/docks/logbook/LogbookCellDelegate.h`, `.cpp`: the delegate; "Sizes and editing are the base class's" (no editor today).
- `src/ui/docks/logbook/LogbookView.cpp`: `onContextMenuRequested()`, the "Set %1..." actions, `QInputDialog::getText`, the rule on nested event loops (session ids and attribute key captured, resolved again after the dialog), `startBulkEdit`.
- `src/logbookcolumn.cpp`: a session-attribute column's label from the registry.
- `src/preferences/addcolumndialog.cpp`: the attribute tree (from the registry, by category) and the measurement tree (from `PlotRegistry::allPlots()`).
- `src/preferences/logbooksettingspage.cpp`: the default columns (the orientation is not one).
- `src/csvformat.h`: `CsvFormat::singleLine` (text edits are flattened).

### Engine, attribute calculations and the importer (phase 2)

- `src/calculations/attributecalculations.h`, `.cpp`: `registerAttributeCalculations()`; the exit-time defaults (`exitTimeDefaults`, `builtin.attr.syncTime`, `builtin.attr.courseRef`) beside which the helper goes; the data-derived ground-elevation default ("This serves as the data-derived default").
- `src/calculations/registration.h`: `Calculations::addCalculation`.
- `src/engine/calculationdescriptor.h`, `src/engine/calculationregistry.h`: the descriptor (id, inputs, outputs, compute, policy).
- `tests/tst_calcengine.cpp`: `constantCalculationIsADefault()`, the engine's proof that a no-input calculation is a default (stored wins, removal returns to it, dependants follow).
- `src/dataimporter.cpp` `applyCreationDefaults()` (step 5, "Wind defaults (placeholder; will be calculated per-track in future)"), `src/dataimporter.h` (its comment lists `_WIND_N / _WIND_E`).
- `src/logbookmanager.cpp` `applyLegacyBackfill()`, `src/logbookmanager.h` (its comment names `_WIND_N` / `_WIND_E`).
- `src/calculations/gnsscalculations.cpp` `windComponent()`: a non-numeric wind reads as zero; wind is an input of `wcVel`, `wcVelH`, the track accelerations and the aerodynamic coefficients.
- Tests that pin the wind writes: `tests/tst_importer.cpp` (`_WIND_N` stored after import), `tests/tst_import_merge.cpp`, `tests/tst_persistence_roundtrip.cpp` (`releasedLogbookBackfillIsAdditive`, the saved `$VAR,_WIND_E,0` lines), `tests/tst_logbook_index.cpp` (the attribute list), `tests/support/oraclecatalogue.cpp` (`_WIND_N` edits), `tests/tst_column_cache.cpp` and `tests/tst_logbook_index.cpp` (fixtures that store wind).

### Fusion registration and the derivations (phases 3, 4)

- `src/fusion/fusionregistration.h`, `.cpp`: the fit's outputs table `kFitOutputs`, `registerFit`, `registerHorizontalAcceleration` (the pattern: on demand, inputs exist only after the fit publishes, "appears with the fit ... and never starts one"), `registerSystemTime`, `registerFusionCalculations()` (the application's one entry point), the header's list of registered calculations.
- `src/fusion/fusion.h`: `Result` (the seventeen channels; "roll, pitch and yaw are degrees, unwrapped ..."), `Algorithm`.
- `src/fusion/fusionoutput.cpp` `fillOutputChannels()`: how the quaternion is produced (`dense.rotation[i].toQuaternion()`, xyzw, body (device) to fixed NED) and how the fit's own RPY is derived and unwrapped; the attitude derivation must agree with this convention and must not include GTSAM or Eigen.
- `src/calculations/gnsscalculations.cpp`: `registerGnss`, GNSS `z` (hMSL minus `_GROUND_ELEV`), `course` (atan2, `unwrapDegrees`, minus the course reference), `accD` (derivative of velD, positive down), `trackInputs`, `accAlongTrack`, `accCrossTrack`.
- `src/calculations/anglehelper.h`: `unwrapDegrees`, "The one unwrap rule of the application"; its comment names its users.
- `src/sessiondata.h` `SessionKeys`: `LocalOriginHmsl`, `GroundElev`, `WindN`, `WindE`; where the orientation key goes.
- `src/mainwindow.cpp`: the registration order (`registerBuiltInAttributes()` before `Fusion::registerFusionCalculations()`).
- `docs/LOCAL_COORDINATES.md` §2-§3: `_LOCAL_ORIGIN_HMSL` and the meaning of `Local/down`.

### Fusion tests (phases 3, 4, 5)

- `tests/CMakeLists.txt`: `flysight_add_test`, the FUSION block (the tests that link the fusion library and may run the solver).
- `tests/fusion/fusionsessions.h`, `.cpp`: `registerFusionCalculations(registry)` per test registry, `fusionMeasurementNames()`, `fusionPlots()` (the tests' mirror of the application's list, pinned by the audit), `fusionNames()`, `naturalSession()`.
- `tests/tst_fusion_session.cpp`: `plotNames()` (the measurement behind each of the seventeen plots), the registration tests (`registry.title(kFit)`).
- `tests/tst_fusion_rows.cpp`: `allSeventeenFusionPlotsAreExplicitBacked()` (cited by item 115 of the map), `localFramePlots()`, `accHRowIsBlockedByFusion()` (the pattern for a derived plot's blocker), `rollPitchYawShareOneJob()`.
- `tests/tst_fusion_store.cpp`, `tests/tst_fusion_golden.cpp`, `tests/tst_fusion_kernel.cpp`: stored results and golden fixtures that must pass unchanged.
- `tests/tst_builtins_golden.cpp`, `tests/support/builtinfixture.cpp`: the GNSS calculations' golden values; the track accelerations must stay identical after phase 3's extraction.
- `tests/support/testenvironment.cpp`: the test environment's registrations (`registerBuiltInAttributes()`).

### Plot list and profiles (phase 5)

- `src/mainwindow.cpp` `registerBuiltInPlots()`: the rows, the palette variables, the "GNSS (Local frame)" and "Sensor fusion" blocks and their comments.
- `src/plotregistry.h`, `.cpp`: `PlotValue`, `allPlots()`, `dependentPlots()`.
- `src/profilestatebridge.cpp` `applyProfile()` step 1 ("Enabled plots"): the silent rule as it is today; `captureProfile()` step 1.
- `src/profile.h`, `src/profilemanager.cpp`, `src/resources/profiles/*.fvprofile`: the built-in profiles (none names a fusion or local-frame plot).
- `src/plotmodel.h`: `setPlotEnabled()`, `makePlotId()`.
- `tests/tst_calculation_demand.cpp` `profileStyleApplyCreatesDemand()`: the test that imitates applyProfile's loop today.
- `src/logbookcolumn.cpp`, `src/preferences/addcolumndialog.cpp`: a measurement column's label and the Add Column dialog both come from the plot registry (decision 13).
- `src/dataexporter.cpp`, `src/pluginsessionview.h`, `src/pluginadapters.cpp`: what "export" and the plug-ins read.

### Audit and traceability (all phases; phase 6)

- `tests/audit/cleanup_audit.cmake`: group `naming` ("seventeen fusion plots", "six local-frame plots", pinned to `tests/fusion/fusionsessions.cpp`); group `solver-confinement` ("public and registration files are GTSAM-free" lists files; "the kernel is pure" excludes only the registration files); group `fusion-tooling` (the internal-header list a test may not include); group `fusion-model` ("the fusion document describes the current model"); `one authority` rules; the traceability block and its ranges (1-19, ..., 701-754).
- `tests/acceptance_map.txt`: the four line forms; the head's list of amended ranges; the 701-754 block as the latest pattern.
- `tests/README.md`: section 1 (a table row per executable), 4 (one test), 9.8, 10, 12.6, appendix H (the latest appendix, its preamble on numbering and amended items).

### Documents

- `docs/SENSOR_FUSION.md` §2 (Using it: the plot list), §4 (Model and output contract: the outputs table).
- `docs/CALCULATIONS.md` §5 (Candidates and order: the precedence of stored over calculated), §17 (Sensor fusion as a registered calculation: its table of calculations, "Plots. Seventeen plots in the category ...").
- `docs/COMPUTED_PLOTS.md` (what the user sees; the "Sensor fusion" category).
- `docs/DATA_SCHEMA.md` §8 (importing), §9 (saved session files), §10 (existing logbooks).
- `docs/LOCAL_COORDINATES.md` §1 ("GNSS (Local frame) plots show it"), §10 (consumers).
- `README.md` (root): the documents list, "the "GNSS (Local frame)" plots".

## Decisions and constraints

1. **Vertical acceleration is `Fusion/accD` itself.** Spec §6 lists it among
   the derivations, but the fit already publishes the fused down
   acceleration, and the GNSS "Vertical acceleration" plot is `GNSS/accD`,
   positive down. A calculation that copied it under another name would be
   a second authority for one fact. The plot row names `Fusion/accD`; the
   §13 test "vertical acceleration is the down acceleration" asserts that
   the row's measurement is `Fusion/accD` with the GNSS row's unit and type.
2. **Fused elevation subtracts the ground elevation.** GNSS "Elevation" is
   `GNSS/z` = hMSL minus `_GROUND_ELEV`. Spec §6 says elevation is "the
   origin's height minus the fused down position, so it overlays the GNSS
   elevation directly"; only with the same ground reference does it
   overlay. `Fusion/z` = `_LOCAL_ORIGIN_HMSL` − `Fusion/down` −
   `_GROUND_ELEV`, unavailable when the ground elevation is not a number, as
   GNSS `z` is.
3. **The track-relative accelerations have one definition.** The GNSS
   arithmetic (wind-corrected velocity unit vector, along-track zero below
   the 1e-9 m/s speed, cross-track the magnitude of the remainder) moves into
   one helper in `src/calculations/` that the GNSS and the fused
   registrations both call; the fused calculations declare the same wind
   attributes. GNSS results stay bit-identical (`tst_builtins_golden`).
4. **Names.** The fit's `roll`, `pitch` and `yaw` stay its own. The derived
   measurements are `Fusion/z`, `Fusion/accAlongTrack`, `Fusion/accCrossTrack`
   (the GNSS names) and `Fusion/bodyHeading`, `Fusion/bodyPitch`,
   `Fusion/bodyRoll` (the angles of the body frame the orientation
   attribute defines). One attitude calculation publishes the three angles
   together.
5. **Heading is a compass heading, not referenced to `_COURSE_REF`** (spec
   §7 as amended a second time, after the implementation showed that a
   heading offset by course's reference neither overlays course, which each
   series unwraps from its own first sample, nor follows it outside steady
   flight). The attitude calculation reads the quaternion and `_ORIENTATION`
   only, so heading, pitch and roll exist for every fitted recording. The
   course-reference helper in `src/calculations/` stays for the GNSS course
   alone. The plot has course's unit and type.
6. **The orientation attribute.** Key `_ORIENTATION`
   (`SessionKeys::Orientation`), category "Session", display name
   "Orientation", editable, not among the default columns. A token is
   `<forward>,<up>`, each axis one of `+x`, `-x`, `+y`, `-y`, `+z`, `-z`
   (ASCII hyphen-minus), for example `+y,+z`; a label is
   `forward +y, up +z`. The 24 choices are listed with the default first,
   then the rest in the orientation type's one enumeration order.
7. **An invalid stored orientation makes the attitude unavailable.** A
   stored value wins even when it is invalid (spec §10's note); the
   derivation parses it with the orientation type and publishes nothing for
   a token outside the 24. The logbook shows a stored token that has no
   label as the raw text, so the user can see it and choose a value or
   "Default".
8. **One entry point, one definition.** `Fusion::registerFusionCalculations()`
   stays the application's one entry point into the fusion library and also
   registers the orientation attribute's definition in the attribute
   registry, once per process: tests call it for several registries, and a
   second call must not add a second definition.
9. **"Default" is an invalid `QVariant` at the model's boundary.** For a
   choice attribute, `SessionModel::setData(index, QVariant(), Qt::EditRole)`
   and `startBulkEdit(rows, column, QVariant())` remove the stored attribute
   (never store a blank), invalidate and save as an edit does. Other format
   types keep their behaviour. The editors offer "Default" first, then the
   labels in definition order.
10. **The wind half of the legacy backfill goes with the importer's.**
    `LogbookManager::applyLegacyBackfill()` writes `_WIND_N` / `_WIND_E` = 0
    into files saved before wind existed: the same stand-in the importer
    writes, which spec §10 moves to a calculation. Keeping it would leave
    two authorities for one default. The mass and area backfill stays (spec
    §3 keeps them as import-time snapshots).
11. **The constant-default helper.** `Calculations::addConstantDefault`,
    an inline function in `src/calculations/attributecalculations.h`
    (header-only, so the fusion library can call it: decision 18); the calculation's id is `builtin.default.` followed by the
    attribute key. Wind is registered by `registerAttributeCalculations()`,
    beside the exit-time defaults, the orientation by
    `registerFusionCalculations()`. Every constant default in `src` is found
    by searching for `addConstantDefault(`: the seven SP and WS-P constant
    defaults, registered today by local helpers with ids
    `builtin.sp.default.*` and `builtin.wsp.default.*`, move onto it and take
    its ids (phase 2; their logbook columns recompute once, and no stored
    result reads them).
12. **Plot colours.** Each fusion plot's colour differs from its GNSS
    counterpart's, so the two can be told apart when they overlay; phase 5
    picks them from the palette variables of `registerBuiltInPlots()`.
13. **A removed plot's measurement in a column.** The logbook's Add Column
    dialog lists measurements from the plot registry, and a measurement
    column's label comes from it. After phase 5 a new column over, say,
    `Fusion/roll` or `Local/north` cannot be picked in the dialog; an
    existing one (a saved column list, a profile) still computes, labelled
    with the fallback `Fusion/roll @ Exit`. Spec §5 and §13 (as amended) say
    exactly this: an existing column keeps computing and export still writes
    the measurement, and nothing is added to keep them in the dialog.
14. **The profile rule has a test seam.** The enabled-plots step of
    `applyProfile()` moves into a function that takes what it needs
    (the plot model and the listed ids) instead of the main window, and
    `applyProfile()` calls it; the test calls the same function and asserts
    that nothing is logged. `profilestatebridge.cpp` is compiled into the
    application only, so the rule becomes
    `void PlotModel::setEnabledPlotIds(const QStringList &plotIds)` in
    `src/plotmodel.*` (`flysight_core`).
15. **Traceability is phase 6's.** Items are 801 onward, item = 800 + the
    clause number of appendix I, which phase 6 enumerates from spec §§5-14
    as appendix H did. Phases 1-5 add tests and cite no new item. A phase
    that renames or removes a test function the map cites (for example
    `tst_fusion_rows allSeventeenFusionPlotsAreExplicitBacked`, item 115)
    updates that line in the same phase, so the map check stays green; the
    item is restated "(as amended)" in phase 6.
16. **A phase that adds a test executable** registers it in
    `tests/CMakeLists.txt` and adds its `tests/README.md` section 1 row in
    the same phase.
17. **The fit is untouched.** No phase edits `src/fusion/` kernel files,
    `kFitOutputs`, `fitInputs()`, `Fusion::Algorithm`, `tests/data/fusion/`
    or the golden tests. New files under `src/fusion/` are GTSAM- and
    Eigen-free and are added to the audit's "public and registration files
    are GTSAM-free" list; a header the tests include is not added to the
    fusion-tooling internal-header list.

18. **Library boundaries.** `flysight_fusion` links `flysight_model` (the
    engine, `SessionData`) and nothing of `flysight_core`, where
    `src/calculations/*.cpp` and the attribute registry live today
    (`src/CMakeLists.txt`); `flysight_core` never references the fusion
    library. So a calculations helper the fusion registration uses is
    header-only (as `registration.h`, `anglehelper.h` and `timefithelper.h`
    are): phase 2's `addConstantDefault` (in `attributecalculations.h`) and
    phase 3's track-acceleration helper (`src/calculations/trackhelper.h`)
    are inline. The audit also forbids the text `fusion/` and `Fusion::` in
    `src/calculations`, and any logging in `src/fusion`. For the fusion
    registration to register the orientation attribute (decision 8), phase 4
    moves `src/attributeregistry.{h,cpp}` from `flysight_core` to
    `flysight_model`: it needs Qt Core only, and the move adds no dependency
    to either library. No phase makes `flysight_fusion` link `flysight_core`.

## Interfaces between phases

### Phase 1 provides (used by phase 4)

- `AttributeFormatType::Choice` in `src/attributeregistry.h`.
- `struct AttributeChoice { QString token; QString label; };` in
  `src/attributeregistry.h`, and `QVector<AttributeChoice> choices;` as the
  last member of `AttributeDefinition` (after `measurementType`), so every
  existing brace initializer compiles unchanged.
- The model's contract for a `Choice` column: `Qt::DisplayRole` is the label
  of the effective token, or the raw text when the token has none; sort is
  by that text; `setData(index, QString token, Qt::EditRole)` stores a token
  of the list and returns false for any other; `setData(index, QVariant(),
  Qt::EditRole)` removes the stored attribute; `startBulkEdit(rows, column,
  value)` takes a token or `QVariant()` with the same meanings and refuses a
  token outside the list before queuing anything.
- The in-place editor and the context menu's list: "Default" first, then
  the labels in definition order.
- The test executable and fixture phase 1 uses for the choice type (named in
  phase 1's document), which phase 4 extends for the orientation attribute
  where the model, editor and bulk edit are exercised.

### Phase 2 provides (used by phase 4)

- `inline void Calculations::addConstantDefault(CalculationRegistry &registry,
  const QString &attributeKey, const QVariant &value);` defined in
  `src/calculations/attributecalculations.h` (decision 18): registers the calculation
  `builtin.default.<attributeKey>`, no inputs, one output
  `DependencyKey::attribute(attributeKey)`, whose value is `value`.
- `builtin.default._WIND_N` and `builtin.default._WIND_E`, value `0.0`.

### Phase 3 provides (used by phases 4 and 5)

- `builtin.fusion.z` → `Fusion/z`; `builtin.fusion.accAlongTrack` →
  `Fusion/accAlongTrack`; `builtin.fusion.accCrossTrack` →
  `Fusion/accCrossTrack`. Vertical acceleration is `Fusion/accD`
  (decision 1).
- `tests/tst_fusion_derived.cpp`, a new executable in the FUSION block of
  `tests/CMakeLists.txt`, with a way to give a session synthetic fit
  outputs without running the solver; phase 4 adds the attitude tests to it.
- `docs/SENSOR_FUSION.md` §4 and `docs/CALCULATIONS.md` §17 describing the
  derived kinematics, which phase 4 extends with the attitude.

### Phase 4 provides (used by phase 5)

- `SessionKeys::Orientation` = `"_ORIENTATION"` in `src/sessiondata.h`.
- `src/attributeregistry.{h,cpp}` in `flysight_model` (decision 18).
- `src/fusion/orientation.h`, `.cpp`: the orientation type
  `FlySight::Fusion::Orientation`, the one owner of tokens, labels, the
  body-frame rotation, the default and the enumeration of 24.
- `builtin.fusion.attitude` → `Fusion/bodyHeading`, `Fusion/bodyPitch`,
  `Fusion/bodyRoll`, inputs `Fusion/qx`, `qy`, `qz`, `qw`, the attribute
  `_ORIENTATION`, and for the heading reference `GNSS/velN`, `GNSS/velE`,
  `GNSS/_time` and `_COURSE_REF` (decision 5).
- The course-reference helper in `src/calculations/` (header-only), called
  by the GNSS course registration and the attitude calculation.
- `builtin.default._ORIENTATION`, value `"+y,+z"`, registered by
  `registerFusionCalculations()` with the attribute definition.
- The tail of `registeredIds()`: `builtin.fusion.fit`, `accH`, `systemTime`,
  `z`, `accAlongTrack`, `accCrossTrack`, `builtin.default._ORIENTATION`,
  `builtin.fusion.attitude`, pinned by `tst_fusion_session::registrationShape`.
- The audit group `orientation` (tokens are spelled only in
  `src/fusion/orientation.*`).

### Phase 5 provides (used by phase 6)

- The eight rows of the "Sensor fusion" category, in this order and with
  these measurements: Elevation `Fusion/z`, Horizontal acceleration
  `Fusion/accH`, Vertical acceleration `Fusion/accD`, Along-track
  acceleration `Fusion/accAlongTrack`, Cross-track acceleration
  `Fusion/accCrossTrack`, Heading `Fusion/bodyHeading`, Pitch
  `Fusion/bodyPitch`, Roll `Fusion/bodyRoll`; units and types as spec §5.
- `void PlotModel::setEnabledPlotIds(const QStringList &plotIds)`, called by
  `applyProfile()` step 1 (decision 14).
- The test functions and audit groups as phases 1-5 leave them, which
  phase 6 cites; each phase document ends with its list.

## Commit Policy

Michael has authorized commits for this plan on the existing working branch.
The implementation orchestrator makes every commit; implementation, revision,
and review agents never run git commands that change repository state.
Nothing is ever pushed, and nothing is ever committed on `master` or
`fusion-improvements`. Copy this section into the plan overview as its own
top-level section with this exact heading.

- **Branch.** Confirm the working tree has no modified tracked files (if there
  are any, stop and ask). Switch to the existing branch
  `store-requested-calculations`; never recreate, reset, or rebase it.
- **Never staged:** `PLANS/implementation-plan/`, `TEMP/`, `experiments/`,
  `build*/`, `dist/`, `results/`, and everything under `third-party/` that
  is not tracked. Specifications and archived plans under `PLANS/` are
  committed by Michael, never in a phase commit.
- **One commit per accepted phase**, when the review agent returns ACCEPT,
  containing exactly the files the phase's agents reported, staged by explicit
  path. Subject `Fusion plots phase N: <phase name>`; body a short summary,
  then the session's attribution line. Rejected iterations are never
  committed.
- **Tag** each phase commit `plan/fusion-plots/phase-N-done`; tags are never
  moved or deleted.
- **Fixes to a closed phase** are committed separately as
  `Fusion plots phase N fixup: <what>` once reviewed and accepted.
- **CI workflow edits** are committed separately as
  `Fusion plots phase N: CI (unverified)`.
- **Escalated phases** are not committed; their paths are listed in the final
  report, and phases that depend on them are not started.
