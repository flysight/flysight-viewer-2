# Sensor fusion plots, attitude and the orientation attribute

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

## 1. Motivation

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

## 2. Principles

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

## 3. Scope

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

## 4. Terms

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

## 5. The plot list

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

## 6. Derived kinematics

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

## 7. Attitude

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

## 8. The orientation attribute

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

## 9. The choice attribute type

The attribute registry's format types (text, date and time, duration,
number) gain a fifth, **choice**. A definition of that type carries its
list of allowed values, each a stored token and a display label. From the
type alone follow: display and sorting by label; a list editor in place of
the line edit for in-place editing; a list in place of the text prompt in
the context menu's "Set ..." action; the same bulk edit with the chosen
token; a "Default" entry that removes the stored attribute; refusal of a
token outside the list. Nothing about the type is specific to orientation;
a later enumerated attribute reuses it.

## 10. Constant defaults

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

## 11. What the user sees

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

## 12. Architecture

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

## 13. Tests

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

## 14. Documentation

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
