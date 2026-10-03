# Implementation plan: sample continuity

Plan for `PLANS/sample-continuity.md`, written 2026-10-02 against
`store-requested-calculations` at `5552941`. The specification below is the
authority; where the phase document disagrees with it, the specification
wins, except where a decision below states how the plan reads it.

## Feature specification

## Sample continuity: a hole in a sensor's samples is not drawn across

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

### 1. Motivation

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

### 2. Principles

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

### 3. The rule

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

### 4. What the user sees

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

### 5. Architecture

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

### 6. Tests

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

### 7. Documentation

`docs/COMPUTED_PLOTS.md` gains a short section, "Holes in the data": what a
hole is, that nothing is drawn, read or differenced across one, that the
fusion plots are on the IMU's axis, and that the crossing times are the one
exception. `docs/CALCULATIONS.md` section 18 and
the derivative helper's contract state that a stencil never spans a hole.
`docs/SENSOR_FUSION.md` section 6 states the IMU gap rule as the
application's continuity rule at 1.5. `tests/README.md` gains the
specification's appendix and matrix.

### Commit Policy

Michael has authorized commits for this plan on the existing working branch.
Nothing is ever pushed, and nothing is ever committed on `master` or
`fusion-improvements`. One commit, staged by explicit path, with the
session's attribution line; `PLANS/` is never staged.

## Phases

| Phase | Name | Purpose | Dependencies |
|-------|------|---------|--------------|
| 1 | Sample continuity | The whole specification: the authority in `flysight_model` (§3, §5); the kernel's IMU gap rule read from it (§5); every reader of §4 and §5 (the plot's graph data, the two plot-utility interpolations, the crosshair's graph interpolation, the measure tool, the legend through them, the map track model, the page, the cursor dot model, the simplified track, the interpolation family, the derivative helper); the compatibility marker; the tests of §6; the audit group; the acceptance items in a new hundred with their appendix, matrix and manual step; the documentation of §7 and the documents the readers' changes reach. | none |

```text
1 Sample continuity
```

One phase. The specification says it is small enough for one agent in one
phase, and its Commit Policy authorizes one commit; a plan of several phases
would ask the orchestrator for several commits the policy does not allow. No
part of the change could leave the tree green and honest on its own anyway:
an authority with no readers is generality nothing uses (an audit
convention), and readers without the authority do not compile.

## Key patterns and references

### Project rules

- `CLAUDE.md`: build tree, sequential suite, the audit, the acceptance map,
  `tests/README.md`, the documents, the conventions the audit enforces.
- `CLAUDE.local.md` (untracked): build and test in `build-agent/` only.
- `tests/README.md` section 2 (configure), section 4 (one test or function).

### The model library (where the authority goes)

- `src/CMakeLists.txt`, the `add_library(flysight_model STATIC ...)` block:
  each non-engine unit is listed with a comment saying why it is in the model
  library (`csvformat`, `attributeregistry`, `sensorconfiguration`); the
  authority follows that pattern. `flysight_fusion` links `flysight_model`
  publicly (`target_link_libraries(flysight_fusion PUBLIC flysight_model ...)`),
  so the kernel can call it without a new dependency.
- `src/sensorconfiguration.h/.cpp`, `src/csvformat.h/.cpp`: the shape and
  comment style of a small model-library unit with a class or file contract.
- `src/sessiondata.h`: `SessionKeys::Time` (`_time`), `SessionKeys::SystemTime`
  (`_system_time`), `getMeasurement()`.
- `src/calculations/timecalculations.cpp`: `GNSS/_time` is `GNSS/time`
  passed through, sharing the buffer ("_time for GNSS (time is already in
  UTC): passthrough"); every other sensor's `_time` is `a * time + b`.

### The fusion kernel (`src/fusion/`)

- `fusionsamples.h`: `kImuGapMedians = 1.6`, `medianInterval()`,
  `Tuning::maxGap` and the `Tuning` comment ("1.6 median IMU intervals").
- `fusionsamples.cpp`: `medianInterval()` (validates, then
  `quantile(intervals, .5)`), `requireNoImuGapInsideGnssSpan()` (the window
  check, "IMU gap at ... s; fusion unavailable across missing data"),
  `requireValidTuning()` (maxGap must be finite and positive),
  `requireStatedRates()` (reads the median IMU interval; its message prints
  `1/median` to one decimal).
- `samplestatistics.cpp`: `quantile()`, the definition of the median the
  kernel uses today (fractional rank, `values[i] + (values[j] - values[i]) * (index - i)`).
- `fusion.cpp`, `planFit()`: `plan.tuning.maxGap = kImuGapMedians*imuInterval`,
  the rate check, and the GNSS outage rule
  (`kGnssOutageSeconds`, `kGnssOutageMedians`, `medianInterval(full.gnssTime)`),
  which `PLANS/gnss-holes-bridged.md` removes.
- `imuintegration.cpp`: `requireNoImuGap()` ("Anchor propagation cannot
  bridge an IMU gap") and `propagateAttitude()`, which computes its own
  threshold over the samples it is handed
  (`kImuGapMedians*medianInterval(samples.imuTime)`).
- `initializer.cpp`: the three `propagateAttitude(...)` callers; they hold
  the tuning.
- `fusionpipeline.h`: the comment on `baseTuning` ("maxGap and the noise are
  always ...").

### Fusion tests and goldens

- `tests/tst_fusion_kernel.cpp`: seven sites set
  `maxGap = kImuGapMedians*medianInterval(...)`; one compares against
  `medianInterval(f.window.imuTime)`; `validationRejectsEachDefect` and
  `segmentsAreCutOnFixes` (item 211).
- `tests/fusion/fusionfixtures.cpp`: `rejection(coarseLinear(), "reject_imu_gap")`.
- `tests/tst_fusion_golden.cpp`, `tests/fusion/fusiongolden.cpp`,
  `tests/data/fusion/`: the goldens that must stay bit for bit.
- `tests/README.md` section 11, the fixture table row for `reject_imu_gap`.

### Plots, legend, crosshair, measure tool

- `src/plotutils.h/.cpp`: `interpolateAtX()` (rejects outside the bracketing
  range, "See also synthesizeInterpolation()" in its comment),
  `interpolateSessionMeasurement()`, `kNaN`.
- `src/ui/docks/plot/PlotWidget.cpp`: the graph build (`yData =
  session.getMeasurement(sensorID, measurementID)`, `xData =
  session.getMeasurement(sensorID, m_xVariable)`, the reference offset,
  `graph->setData(xData, yData)`); `PlotWidget::interpolateY()`; its use for
  the y range in `onXAxisRangeChanged` (`interpolateY(graph, newRange.lower)`)
  and near `const double yPlot = PlotWidget::interpolateY(g, xPlot);`.
- `src/plotviewsettingsmodel.cpp`: the x variable is `_time` or
  `_system_time`, so a plot's x data is not always the sensor's `_time`.
- `src/crosshairmanager.cpp`: seven reads through `PlotWidget::interpolateY`.
- `src/ui/docks/legend/LegendPresenter.cpp`: reads through
  `interpolateSessionMeasurement()` (series values and the GNSS position).
- `src/plottool/measuretool.cpp`: `interpolateAtX(xData, yData, rawLo)` and
  friends for the ends and the min/avg/max, and
  `interpolateSessionMeasurement()` for the header's coordinates.
- `tests/tst_plot_format.cpp`, `tests/tst_plot_color.cpp`: widget-free tests
  compiled with `plotutils.cpp` (`tests/CMakeLists.txt`,
  `flysight_add_test(tst_plot_format SOURCES tst_plot_format.cpp "${FLYSIGHT_SRC_DIR}/plotutils.cpp")`).
  No test builds a `PlotWidget` today.

### The map

- `src/ui/docks/map/TrackMapModel.h/.cpp`: `kDefaultSensor = "Simplified"`,
  the `trackPoints` role (a flat `QVariantList` of `{lat, lon, t}` today), the
  plot-range filter and `addBoundaryPoint` (interpolates at the range edges).
- `src/ui/docks/map/MapCursorDotModel.h/.cpp`: `sampleLatLonAtUtc()` on the
  Simplified track.
- `src/ui/docks/map/MapWidget.cpp`, `onTracksReset()`: converts the role to
  JSON for the page.
- `src/resources/map.html.in`: `onTracksChanged()` (one `google.maps.Polyline`
  per session in `polylines`, `trackData` per session), the hover detection
  and its pixel cache (`rebuildPixelCache`, the `trackData.forEach` loops),
  `onPreferenceChanged()` (iterates `polylines`).
- `src/calculations/simplificationcalculations.cpp`: `builtin.simplified.track`,
  Ramer-Douglas-Peucker over the finite samples (`retainedIndices()`); the
  Simplified sensor's samples are a subset of the GNSS samples.
- `docs/LOCAL_COORDINATES.md` section 9 (the simplified track).
- `tests/tst_map_models.cpp` (compiled with the two models, `plotrangemodel.cpp`
  and `plotutils.cpp`), `tests/tst_simplified_track.cpp`.

### Calculations

- `src/calculations/derivativehelper.h/.cpp`: `differenceOverStencil()`, the
  one stencil of `computeDerivative()` and `computeDerivativeAccuracy()`.
- `src/calculations/gnsscalculations.cpp`: `registerGnssDerivative()` (accN,
  accE, accD against `GNSS/time`) and `accAcc` (against `GNSS/_time`); the
  readers of the accelerations downstream (the aero coefficients and the
  rest of the file) see the unavailable samples as NaN.
- `src/calculations/interpolationcalculations.cpp`: the interpolation family,
  key `{timeAttr}:{sensor}/{timeVector}/{dataVector}`; every caller passes
  `SessionKeys::Time` as `timeVector` (`src/logbookcolumn.cpp`,
  `logbookColumnNames()`; `src/sessionmodel.cpp`).
- `src/calculations/builtincalculations.h`: `CalculationCompatibilityVersion`
  and its comment (the bump rule and the history).
- `docs/CALCULATIONS.md` section 9 (when to bump the marker), section 18 (the
  GNSS acceleration accuracy and the stencil).
- `tests/tst_builtins_engine.cpp`: `accelerationAccuracyKnownAnswers`,
  `accelerationAccuracyUnavailable`, `interpolationInstances`,
  `interpolationUnavailableIsCached`; `tests/support/builtinfixture.cpp`.
- `tests/tst_builtins_golden.cpp`: `sessionDataMatchesGolden` (no hole in its
  data; must stay unchanged).
- `tests/tst_result_records.cpp` (`stampsAreCurrent`), `tests/tst_fusion_store.cpp`
  (`codeStampChangeDropsRecordOnLoad`): read the marker.

### Audit and traceability

- `tests/audit/cleanup_audit.cmake`: the helpers (`expect_none`,
  `expect_only`, `expect_count`, `audit_group`, `WB_START`/`WB_END`, no GNU
  escapes); the `gnss-acceleration-accuracy` group as the most recent
  pattern (its rule "one stencil for the derivative and its accuracy" counts
  `\[i \+ 1\] - [A-Za-z]+\[i - 1\]` once in `src`, which a stencil change
  must keep true); `expect_count("one authority: compatibility marker" ...)`;
  the traceability block at the end, which lists the allowed item ranges
  ("item ${item} is outside 1-19, ... and 1101") and the items each range
  must cover.
- `tests/acceptance_map.txt`: the last section, "GNSS acceleration accuracy
  (PLANS/gnss-acceleration-accuracy.md): one item", is the pattern for a
  small specification; item 211 names the IMU gap rule without a number.
- `tests/README.md`: section 1 (a row per test executable), section 9.12 and
  Appendix L (the pattern for a small specification's matrix and appendix),
  section 10 (the audit groups), section 11 (the fixture table), section
  12.10 (the last manual step, M53).

### Documents

- `docs/COMPUTED_PLOTS.md` (sections 1-9; the new "Holes in the data").
- `docs/CALCULATIONS.md` sections 9 and 18.
- `docs/SENSOR_FUSION.md` section 6 ("an IMU gap longer than 1.6 times the
  median IMU interval"), and anywhere else that states the factor.
- `docs/LOCAL_COORDINATES.md` section 9.
- `PLANS/gnss-holes-bridged.md`: the next specification, which reads the
  authority from the kernel (its runs and holes on the GNSS axis); read it
  for what the authority will be asked next, not to implement it.

## Decisions and constraints

1. **One phase, one commit.** See Phases. The Commit Policy names no tag;
   the orchestrator makes the one commit and no tag.

2. **The authority is `src/samplecontinuity.h/.cpp`**, in `flysight_model`,
   listed in the library with a comment in the style of its neighbours. The
   file name is fixed because the audit names it. It reads a time axis as a
   contiguous run of doubles so that the session's `QVector<double>` and the
   kernel's `std::vector<double>` reach it without a copy. Its function
   names are the implementer's.

3. **The nominal interval is the kernel's median today**: the median of the
   successive differences with the definition of `quantile(values, .5)` in
   `samplestatistics.cpp`, to the bit. The kernel's thresholds and the rate
   check's message are then unchanged, which is what keeps the goldens and
   the rejection reasons bit for bit. What the authority does with an axis
   that is not finite and strictly increasing (the session does not
   guarantee it; the kernel validates before asking) is the implementer's
   to decide and to state in the unit's contract; it must not throw on a
   session axis.

4. **The kernel's own median goes with its gap constant.** Every median
   interval the kernel reads (the IMU gap threshold, the rate check, the
   GNSS outage threshold) comes from the authority's nominal interval, and
   `kImuGapMedians` and the kernel's `medianInterval()` are removed, and
   the unit `samplestatistics.h/.cpp` with them: `quantile()`'s only caller
   was the median, and `componentMean()` and `componentStddev()` have no
   caller in `src` or `tests`, so the unit would be dead; its entry in
   `src/CMakeLists.txt` and its name in the audit's internal-header list
   (`fusion-tooling`) go with it. The tests that used the median read the
   authority. Because the whole IMU axis now
   needs a nominal interval, `requireUsableRecording()` requires three IMU
   samples rather than two (no fixture or test has fewer; its message and
   `docs/SENSOR_FUSION.md` section 6 say three). `tuning.maxGap` is the
   authority's threshold for the whole recording's IMU axis, set in
   `planFit()`. `propagateAttitude()` stops computing its own threshold over
   the sub-window it is handed and is given `tuning.maxGap` by its callers,
   as the specification's §5 says ("`tuning.maxGap`, which the attitude
   propagation and the window checks read"). The strict comparison of an
   interval with the threshold is the authority's; the kernel's two checks
   ask it rather than spell `> maxGap` themselves. The GNSS outage rule
   itself stays as it is (its own factor and its 2 s floor) until
   `PLANS/gnss-holes-bridged.md` removes it; only its median comes from the
   authority.

5. **The map judges holes on `GNSS/_time`, never on `Simplified/_time`.**
   The Simplified sensor is a subset of the GNSS samples chosen for shape;
   its intervals are not a sampling interval, and the rule applied to them
   would find holes wherever the simplifier dropped a sample. So:
   - `builtin.simplified.track` simplifies each run of connected GNSS
     samples on its own, which keeps the two samples around every hole and
     gives exactly today's output for a recording without one;
     `docs/LOCAL_COORDINATES.md` section 9 and `tst_simplified_track` say so.
   - `TrackMapModel` splits the simplified points by the GNSS run they
     belong to and hands the page one point list per run; its range-edge
     interpolation never interpolates across a hole.
   - `MapCursorDotModel` gives no dot at a time inside a GNSS hole.
   - The `trackPoints` role becomes a list of runs, each a list of
     `{lat, lon, t}`; `MapWidget::onTracksReset()` carries it to the page;
     the page draws one polyline per run and its hover detection never
     spans two runs. A session's preference and colour updates reach every
     polyline of the session.

6. **The plot's graph data is built by a widget-free function** in the plot
   utilities, which `PlotWidget` calls, so that §6's plot test runs without
   a widget. The break is QCustomPlot's: a NaN-valued point between the two
   samples around a hole. Every reader of the graph data must tolerate it
   (the y-range fit, the crosshair, the hover). The crosshair's graph
   interpolation must then answer "--" strictly inside the hole and the
   sample's own value at both samples around it; a NaN neighbour at the
   first sample after the hole must not make that sample unavailable. That
   logic must be reachable from a widget-free test too, since no test
   links `PlotWidget`.

7. **The plot-utility interpolations judge holes on the sensor's `_time`,
   by sample index**, whatever the x variable is: a plot drawn against
   `_system_time` passes `_system_time` as x data, whose spacing is not the
   rule's axis. `interpolateAtX()` therefore needs the sensor's time axis
   (or is replaced by a session-aware form); the measure tool reads through
   whichever the implementer chooses, and the legend gains the behaviour
   through `interpolateSessionMeasurement()`. The measure tool's
   min/avg/max over the samples between its ends is unchanged (§4: it
   claims nothing about the middle).

8. **The interpolation family judges its `timeVector`**, which is `_time`
   in every caller; the derivative helper judges its `times` argument,
   which is `GNSS/_time` for `accAcc` and `GNSS/time` (the same buffer) for
   the accelerations, so the derivative and its accuracy are unavailable at
   exactly the same samples. The helper's whole-vector refusals (empty,
   mismatched, fewer than two samples, equal times) are unchanged; a stencil
   over a hole yields NaN at that sample only.

9. **`CalculationCompatibilityVersion` goes from 2 to 3**, with a history
   line. The code's rule (`builtincalculations.h`, `docs/CALCULATIONS.md`
   section 9) requires it: the interpolation family and the derivatives'
   values change for a session with a hole, and logbook columns read both.
   The bump drops every cached column value and every stored result at the
   first start, the fusion fits included; that also covers the kernel's
   move from 1.6 to 1.5, so the fusion algorithm string stays
   `batch-temperature-bias-v7` and `PLANS/gnss-holes-bridged.md` keeps v8
   for itself. The tests that spell the value 2 (`tst_logbook_index`,
   `tst_column_cache`, `tst_session_oracle`, `tst_workflow`) and
   `docs/CALCULATIONS.md` section 17 ("The marker's current value, 2, ...")
   move to 3. The specification does not mention the bump; Michael may
   prefer otherwise (see the report).

   A fixture whose own times hold an interval above 1.5 nominal intervals
   now has a hole: `tst_builtins_engine::accelerationAccuracyKnownAnswers`
   steps 0.25, 0.25, 0.25, 0.5, 0.25 s, so its 0.5 s step is one. The phase
   document settles it (its decision D2); any other such fixture the full
   suite turns up is treated the same way.

10. **Traceability.** The specification's items take the hundred 1201
    onward (one item per bullet of §4, the ground elevation reads and the
    crossing-time exception included, plus the rule of §3, the architecture
    of §5, the tests of §6 that are not a user-visible bullet, and §7:
    thirteen items, 1201-1213), with appendix M, matrix section 9.13 and
    the audit's traceability ranges extended. `PLANS/gnss-holes-bridged.md` will take
    1300. A manual step M54 in a new section 12.11 checks, on a corpus
    recording with a GNSS hole, the plot's break, the legend's "--", the
    map's break and the absent dot. No existing item is restated (§6).

11. **The audit group is `sample-continuity`**, holding the rules of §5:
    the factor and the defining phrase in `src/samplecontinuity.*` alone,
    no other median of a time axis for a continuity decision in `src`
    (the GNSS outage rule reads the authority's interval, decision 4, so
    the rule needs no exception), each reader that consults the authority directly including it, and
    `kImuGapMedians` absent from the pathspec. Each rule is planted once to
    prove it matches.

12. **Two point reads are folded in; the crossing detections are the
    specification's stated exception.** The Set Ground tool's elevation at
    the clicked time and the automatic ground elevation at the analysis end
    (`builtin.attr.groundElev`) are point reads with the measure tool's
    semantics, and §4 names them: inside a hole they have no value. The
    crossing-time calculations (exit, altitude markers, the WS-P and SP
    windows) keep their linear interpolation across a hole, as §4 states and
    the documentation says; a later specification may refine them.

### Constraints the code imposes

- The suite runs sequentially in `build-agent/`; the fusion tests time out
  under load.
- `experiments/fusion_lab/` holds its own copy of the kernel with
  `kImuGapMedians`; it is ignored by git, outside the audit's pathspec, and
  is not changed.
- The map page (`map.html.in`) has no automated test; its change is covered
  by M54 and by `tst_map_models` on the role's shape.

## Interfaces between phases

There is one phase. The interfaces that matter are the ones a later
specification and the audit rely on, and they are fixed here:

- `src/samplecontinuity.h/.cpp` in `flysight_model` is the authority: the
  nominal interval of a time axis, whether the interval between samples
  `i - 1` and `i` is a hole, the runs of connected samples as half-open or
  closed index ranges (the implementer states which), and the threshold in
  seconds. `PLANS/gnss-holes-bridged.md` reads the runs and holes of the
  GNSS axis from the kernel through this unit.
- `Tuning::maxGap` keeps its name and meaning (seconds); it is set from the
  authority in `planFit()` and is the only threshold the kernel's IMU checks
  and `propagateAttitude()` read.
- The `trackPoints` role of `TrackMapModel` is a list of runs, each a list
  of `{lat, lon, t}` maps, in time order; `MapWidget` and `map.html.in`
  read that shape.
- The audit group slug is `sample-continuity`; the acceptance items are
  1201-1213.

## Commit Policy

Michael has authorized commits for this plan on the existing working branch.
Nothing is ever pushed, and nothing is ever committed on `master` or
`fusion-improvements`. One commit, staged by explicit path, with the
session's attribution line; `PLANS/` is never staged.
