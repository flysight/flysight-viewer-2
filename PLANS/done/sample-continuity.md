# Sample continuity: a hole in a sensor's samples is not drawn across

Date: 2026-10-02
Status: specification for planning. Not an implementation plan. Small enough
to be implemented directly by one agent in one phase. It precedes
`PLANS/gnss-holes-bridged.md`, which reads the rule this one establishes.
Baseline: branch `store-requested-calculations` at `5552941`; the committed
code is authoritative.
Related: `src/plotutils.h/.cpp` (`interpolateAtX`,
`interpolateSessionMeasurement`), `src/ui/docks/plot/PlotWidget.cpp` (the
graph data, `interpolateY`), `src/crosshairmanager.cpp`,
`src/plottool/measuretool.cpp`, `src/ui/docks/map/TrackMapModel.cpp`,
`src/ui/docks/map/MapCursorDotModel.cpp`, `src/resources/map.html.in` (the
track polylines), `src/calculations/interpolationcalculations.cpp`,
`src/calculations/derivativehelper.h/.cpp`, `src/fusion/fusion.cpp` and
`src/fusion/fusionsamples.h` (`kImuGapMedians`), `src/fusion/imuintegration.cpp`
(the gap check of the attitude propagation), `docs/COMPUTED_PLOTS.md`,
`docs/CALCULATIONS.md` section 18, `docs/SENSOR_FUSION.md` section 6,
`tests/acceptance_map.txt`, `tests/README.md`.

## 1. Motivation

A FlySight loses its GNSS fix in an aircraft cabin and under a canopy, and
logs nothing while it has none. Two recordings of the reference corpus have
holes of 13 and 16 seconds during the climb. Today every plot draws a straight
line from the last fix before a hole to the first after it, the map draws the
track through it, the legend and the measure tool read a value inside it by
interpolation, a logbook measurement at a marker inside it is interpolated
the same way, and a derived value whose stencil spans it, such as the GNSS
acceleration or its accuracy, is a plausible-looking number averaged over the
hole. All of these assert a continuity the recording does not hold.

Plotting packages agree on the drawing convention: a missing value breaks the
line, and nothing is drawn across it. They leave the question of when a long
interval is a hole to the data's owner. This specification answers that
question once, for every sensor, in one place, and makes every reader of a
sensor's samples honour it.

## 2. Principles

- **One rule, one authority.** What makes an interval a hole is defined once,
  in the model library, and every reader asks it. Nothing else computes a
  median interval or compares an interval to a threshold for this purpose.
  The sensor fusion kernel's IMU gap rule is the same rule, read from the
  same place.
- **Behaviour, not decoration.** A hole is a fact about the samples. The plot,
  the map, the interpolations and the derivatives each react to it in the way
  that is honest for them; none of them hides it and none of them fills it.
- **Nothing is invented inside a hole.** No value is interpolated across it,
  no derivative is differenced across it, and no line is drawn across it. A
  reader that is asked for a value inside a hole says it has none.
- **Per sensor.** Each sensor has its own time axis, its own sampling
  interval and its own holes. A hole in the GNSS samples is not a hole in the
  IMU's, and a plot drawn on the IMU's axis draws through a GNSS hole.

## 3. The rule

A sensor's **time axis** is its `_time` column as the session holds it. Its
**nominal interval** is the median of the successive differences of that
axis over the whole session. An interval between two successive samples is a
**hole** when it is strictly greater than 1.5 times the nominal interval. A
time axis with fewer than three samples has no nominal interval and no
holes.

The factor is 1.5 because it is halfway between the nominal interval and the
shortest interval that can hold a missing sample. The timing of the reference
corpus is far inside it: over the 2.26 million IMU intervals of 99
recordings the largest is 1.013 times its median. The kernel's IMU gap rule
has used 1.6 since the port, without a recorded reason; it becomes this rule.

The two samples around a hole are ordinary samples. A hole is the interval
between them, nothing more: no sample is removed, inserted or altered, and
the session's data is exactly what the recording holds.

## 4. What the user sees

- **Plots.** A series is drawn on its sensor's time axis, and the line stops
  at the last sample before a hole and resumes at the first after it, with
  nothing between. This holds for every series of the sensor, recorded and
  computed alike: elevation and the GNSS speeds break at a GNSS hole, and so
  do the accelerations, the acceleration accuracy and every other value on
  the GNSS axis. It holds whichever independent variable the plot uses,
  because the hole is in the sensor's own time, not in the axis it is drawn
  against.
- **The fusion plots** are on the IMU's time axis and draw through a GNSS
  hole once `PLANS/gnss-holes-bridged.md` lets the fit bridge it; their
  published accuracies say what that stretch is worth. A hole in the IMU's
  samples breaks them like any other series, though today such a recording
  is not fitted at all.
- **The legend and the crosshair** show "--" for a series at a cursor time
  inside one of its holes, as they do beyond the series' ends.
- **The measure tool** reads a series at its two ends; an end inside a hole
  reads "--" and the measurement that needs it is not shown. A measurement
  whose two ends are on samples is reported even when the span between them
  contains a hole: the tool compares its ends and claims nothing about the
  middle.
- **The map** draws one polyline per run of connected fixes, so the track
  has a visible break at a hole; the cursor dot is absent while the cursor
  is inside a GNSS hole.
- **Logbook measurements at a marker** ("measurement at marker" columns and
  the interpolation calculation behind them) are unavailable when the
  marker's time falls inside a hole of the sensor they read, exactly as they
  are outside the sensor's coverage.
- **Ground elevation reads.** The Set Ground tool's elevation at the clicked
  time and the automatic ground elevation at the analysis end are point
  reads like the measure tool's: at a time inside a hole they have no
  value, as they have none outside the samples, so the click sets nothing
  and the attribute is unavailable.
- **Derived values.** A stencil that spans a hole yields no value: the
  derivative and its accuracy are unavailable at the samples whose centred
  difference crosses a hole, and the one-interval forms at the ends of the
  recording are likewise unavailable when that one interval is a hole. A
  value computed from a single sample, such as the glide ratio or the dive
  angle, is unaffected.
- **Crossing times are the exception.** The exit, the altitude markers and
  the analysis windows of the WS-P and SP methods find the time a value
  crosses a threshold by linear interpolation between the two samples
  around the crossing, also when those samples are the sides of a hole: the
  crossing happened inside the hole, and the interpolated time is the
  estimate the recording allows. The documentation says so. A later
  specification may refine it.

What the user does not see: no dialog, no badge, no count of holes. A hole
shows where it is, in the data, and nowhere else.

## 5. Architecture

- **The authority** is one small unit of the model library (`flysight_model`,
  beside `csvformat` and `sensorconfiguration`): given a time axis, it answers
  its nominal interval, whether the interval between two successive samples
  is a hole, and the runs of connected samples as index ranges. The factor
  1.5 is its one constant and is defined nowhere else. The kernel links the
  model library and calls the same function for its IMU gap rule, so
  `kImuGapMedians` and the kernel's own comparison go; `tuning.maxGap`, which
  the attitude propagation and the window checks read, is set from the
  authority's threshold for the IMU axis.
- **Readers**, each consulting the authority and none re-deriving it:
  the plot widget where it builds a graph's data (a break between two samples
  across a hole, which the plotting library draws as an interruption); the
  two interpolation functions of the plot utilities and the graph
  interpolation the crosshair uses; the measure tool and the Set Ground tool
  through those functions; the automatic ground elevation at the analysis
  end; the map track model, which hands the page one point list per run so that
  the page draws one polyline per run; the map cursor dot model; the
  interpolation calculation family; and the derivative helper, whose stencil
  refuses to span a hole. The interpolation sites already treat "outside the
  range" as unavailable; "inside a hole" joins it as the same answer.
- **The audit** gains a rule group: the factor 1.5 and the phrase that
  defines a hole occur in the authority alone; no other file in `src`
  computes a median of a time axis for a continuity decision; the readers
  named above reference the authority; `git grep kImuGapMedians` is empty.
- **Nothing is stored.** Holes are recomputed from the samples when needed;
  they are cheap, and storing them would be a second authority.

## 6. Tests

- The authority on hand-built axes: the nominal interval; an interval of
  exactly 1.5 times it is not a hole and one just above is; fewer than three
  samples give no holes; the runs of a session with two holes are three
  ranges that partition the samples.
- The plot: a synthetic session with one GNSS hole produces a graph whose
  data has exactly one interruption, between the samples around the hole,
  and a series on the IMU axis of the same session has none.
- The interpolations: each of the two plot-utility functions, the crosshair's
  graph interpolation and the interpolation calculation are unavailable at a
  time inside a hole and available at the samples on either side of it.
- The two ground elevation reads: no value at a time inside a hole, the
  sample's value at the samples around it, and today's answers outside the
  samples.
- The derivative and its accuracy: unavailable exactly at the samples whose
  stencil spans a hole, the ends included, and bit for bit what they were
  everywhere else, proven on the existing known-answer cases with a hole cut
  into them.
- The map models: the track model emits two runs for a session with one
  hole, in order, together holding every finite fix; the cursor dot is absent
  inside the hole.
- The fusion kernel: the IMU gap fixture still rejects with the same reason;
  the goldens are unchanged bit for bit, since no fixture and no reference
  recording has an IMU interval between 1.013 and 1.6 nominal intervals.
- The acceptance map opens a new hundred for this specification's items. No
  existing item states the factor (item 211 names the IMU gap rule without
  a number), so none is restated; the fixture table's row for
  `reject_imu_gap` and section 6 of the fusion document state 1.5.
  `audit_cleanup` and the whole suite green.

## 7. Documentation

`docs/COMPUTED_PLOTS.md` gains a short section, "Holes in the data": what a
hole is, that nothing is drawn, read or differenced across one, that the
fusion plots are on the IMU's axis, and that the crossing times are the one
exception. `docs/CALCULATIONS.md` section 18 and
the derivative helper's contract state that a stencil never spans a hole.
`docs/SENSOR_FUSION.md` section 6 states the IMU gap rule as the
application's continuity rule at 1.5. `tests/README.md` gains the
specification's appendix and matrix.

## Commit Policy

Michael has authorized commits for this plan on the existing working branch.
Nothing is ever pushed, and nothing is ever committed on `master` or
`fusion-improvements`. One commit, staged by explicit path, with the
session's attribution line; `PLANS/` is never staged.
