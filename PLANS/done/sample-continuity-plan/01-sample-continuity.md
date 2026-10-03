# Phase 1: Sample continuity

The whole of `PLANS/sample-continuity.md`, as reproduced in
`00-overview.md`, in one commit. The overview's decisions 1-12 apply as
written; this document applies them to the code as it is at `5552941`.

**Findings against the overview (none blocking; each is settled below).**

1. `tst_builtins_engine::accelerationAccuracyKnownAnswers` already holds a hole
   under the new rule: its times step 0.25, 0.25, 0.25, 0.5, 0.25 s, so the
   nominal interval is 0.25 s and the 0.5 s interval is a hole (> 0.375 s).
   Its samples 3 and 4 become unavailable and the test as written fails. The
   test is amended, off the boundary (decision D2).
2. The marker bump (overview decision 9) reaches tests that spell the value 2:
   `tst_logbook_index` (`markerWrittenOnFlush` and a later check),
   `tst_column_cache`, `tst_session_oracle` (two checks) and `tst_workflow`;
   and `docs/CALCULATIONS.md` section 17 says "The marker's current value, 2,
   identifies the centered time fit".
3. `quantile()` in `src/fusion/samplestatistics.cpp` has one caller, the
   kernel's `medianInterval()`, and the unit's other two functions have none
   anywhere. When the median goes, the unit goes with it (overview decision
   4).
4. Spec §4 names two point reads beside the measure tool, the Set Ground
   tool's and the automatic ground elevation at the analysis end, and
   states the crossing-time calculations as the one exception. The point
   reads are made hole-aware here and the crossings are not touched
   (decision D3).

## Purpose

There is one rule for when an interval between two samples of a sensor is a
hole. It is defined once, in `flysight_model`. The fusion kernel's IMU gap
rule and every reader the specification names ask it: the plot's graph data,
the two plot-utility interpolations, the crosshair's graph interpolation, the
measure tool and legend through those, the map (simplified track, track
model, page, cursor dot), the interpolation family and the derivative
helper. There is one phase because an authority with no readers is
generality nothing uses, and readers without the authority do not compile
(overview, Phases).

## Dependencies

Depends on nothing and blocks nothing in this plan. It starts from
`store-requested-calculations` at `5552941`. `PLANS/gnss-holes-bridged.md`
follows it and reads the authority from the kernel. That specification needs
the runs and holes of a GNSS window's `std::vector<double>` axis and each
hole's start and length. The interface below gives these. Nothing in that
specification is implemented here: the GNSS outage rule
(`requireNoGnssOutage`, `kGnssOutageSeconds`, `kGnssOutageMedians`) and the
algorithm string `batch-temperature-bias-v7` stay.

## What changes

### 1. The authority: `src/samplecontinuity.h/.cpp` (spec §3, §5; decisions 2, 3)

- A new unit in `flysight_model`. It is listed in the `add_library(flysight_model
  STATIC ...)` block of `src/CMakeLists.txt` with a comment in the style of
  `csvformat`, `attributeregistry` and `sensorconfiguration` saying why it is
  there: the kernel (`flysight_fusion` links `flysight_model` publicly) and
  the core both read it. The comment style of a small model unit with a
  stated contract is in `src/sensorconfiguration.h`.
- It needs the standard library or Qt Core only. It does not include
  `sessiondata.h` or anything the audit rule "the kernel is pure" forbids,
  because the kernel includes it. It neither logs nor throws on a session
  axis.
- It reads a time axis as a contiguous run of doubles, so a `QVector<double>`
  and a `std::vector<double>` reach it without a copy. It answers:
  - the **nominal interval**: the median of the successive differences. The
    definition is that of `quantile(values, .5)` in `samplestatistics.cpp`,
    to the bit: fractional rank `.5 * (m - 1)`, then
    `values[i] + (values[j] - values[i]) * (index - i)`. A linear-time
    selection of the one or two order statistics gives the same bits as the
    sort. The fraction is 0 or 0.5, so the product is exact and contraction
    into an fma cannot change the result. No compile flag is needed in
    `flysight_model`. An axis with fewer than three samples has none.
  - the **threshold** in seconds: 1.5 times the nominal interval. The factor
    1.5 is the unit's one constant and appears nowhere else in `src`.
  - whether an interval is a hole against a threshold, strictly
    (`interval > threshold`); and whether the interval between samples
    `i - 1` and `i` of an axis is a hole.
  - the **runs** of connected samples as index ranges. The unit's contract
    states whether they are half-open or closed. The runs of any axis
    partition its samples in order.
- The derivative helper and the graph build judge every interval of an axis.
  So the interface lets a caller compute the nominal interval or threshold
  once and judge many intervals against it. No reader sorts or selects over a
  whole axis more than once per graph build, calculation or cursor event per
  axis.
- An axis that is not finite and strictly increasing throughout can occur on
  a session. The kernel validates its axes first. What the unit answers for
  such an axis is the implementer's choice, stated in the contract (decision
  3). One natural choice is no nominal interval and no holes, so every reader
  behaves on it as today.
- Nothing is stored (spec §5): no session attribute, no cache field, no
  logbook value.

### 2. The fusion kernel (spec §5; decision 4)

- `kImuGapMedians` and `medianInterval()` are removed from
  `fusionsamples.h/.cpp`. `quantile()` loses its only caller, and
  `componentMean()` and `componentStddev()` have none in `src` or `tests`,
  so `samplestatistics.h/.cpp` is removed whole: its line in the
  `flysight_fusion` block of `src/CMakeLists.txt` and its name in the
  audit's `fusion-tooling` internal-header pattern go with it.
  `requireIncreasingFiniteTimes()` stays.
- `planFit()` (`fusion.cpp`) reads every median interval from the authority:
  - `plan.tuning.maxGap` becomes the authority's threshold for
    `full.imuTime`, replacing `plan.tuning.maxGap = kImuGapMedians*imuInterval`;
  - the rate check's argument is the authority's nominal interval of
    `full.imuTime`;
  - the GNSS outage limit's median is the nominal interval of `full.gnssTime`.
    The outage rule keeps its own factor 5 and its 2 s floor.
- The full IMU axis needs a nominal interval. `requireUsableRecording()` now
  requires three IMU samples ("at least three GNSS fixes and three IMU
  samples"), so a NaN threshold never reaches `requireValidTuning()`
  (decision D5). `prepareInput()` already guarantees a finite, strictly
  increasing axis.
- `requireNoImuGapInsideGnssSpan()` and `requireNoImuGap()`
  (`imuintegration.cpp`) ask the authority whether an interval is a hole
  against `tuning.maxGap`. They no longer spell `> maxGap` or `> limit`
  themselves. Their messages are unchanged: "IMU gap at ... s; fusion
  unavailable across missing data" and "Anchor propagation cannot bridge an
  IMU gap".
- `propagateAttitude()` stops computing
  `kImuGapMedians*medianInterval(samples.imuTime)` over the sub-window it is
  handed. It takes the threshold from its caller. The three callers in
  `initializer.cpp` (`growPrefix`, twice in `initializeSegment`) pass
  `tuning.maxGap` from the tuning they hold. Its header comment says so.
- Comments that state the factor are updated: the `Tuning` comment ("1.6
  median IMU intervals") and `Tuning::maxGap`'s. The comment on `baseTuning`
  in `fusionpipeline.h` stays true.
- The following do not change: rejection reasons, the rate check's message
  (it prints `1/nominal` to one decimal, the same bits), the algorithm
  string, every golden, and the `experiments/fusion_lab` copy (ignored by
  git).

### 3. Plots, crosshair, legend, measure tool (spec §4; decisions 6, 7)

- **Graph data.** A widget-free function in the plot utilities builds a
  graph's keys and values from the session, the sensor, the measurement, the
  x variable and the reference offset. It inserts one NaN-valued point
  between the two samples around each hole of the sensor's `_time`, judged
  by sample index, whatever the x variable. The break's key lies strictly
  between the two samples' keys, which also holds when the x variable is
  `_system_time`. `PlotWidget`'s graph build (`yData =
  session.getMeasurement(sensorID, measurementID)` ... `graph->setData(xData,
  yData)`) calls it, so no graph is built from raw session vectors.
  `plotutils` does not include QCustomPlot.
- **Readers of the graph data** must not read the break as a value:
  - the y-range fit in `onXAxisRangeChanged` skips it on purpose, not because
    of the argument order of `std::min`;
  - `PlotWidget::interpolateY()` and its users (`crosshairmanager.cpp`, the
    moment-overlap check near `const double yPlot =
    PlotWidget::interpolateY(g, xPlot);`) answer NaN strictly inside a hole,
    and the sample's own value at both samples around it. A NaN neighbour
    must not make the first sample after the hole unavailable. Today
    `findBegin` at that sample's key pairs it with the break and gives NaN.
    This logic is a plot-utility function on keys and values that
    `interpolateY` delegates to. It is reachable from a widget-free test, and
    a cursor move does not copy the graph's data;
  - `SelectTool::sessionsInRect` already treats a segment with a NaN end as
    no hit through its comparisons. The implementer confirms this and leaves
    it.
- **Session interpolations.** `interpolateSessionMeasurement()` and the
  measure tool's ends judge holes on the sensor's `_time` by the bracketing
  sample index (decision 7). Strictly inside a hole the answer is NaN. At
  the sample after the hole it is that sample's value. Beyond the ends it is
  unchanged. `interpolateAtX(xData, yData, x)` therefore takes the sensor's
  time axis, or is replaced by a session-aware form; the implementer
  chooses. Its stale "See also synthesizeInterpolation()" note is
  corrected. A sensor whose `_time` is missing or of another length than the
  data has no holes for these readers.
- **Legend.** `LegendPresenter.cpp` gains the behaviour through
  `interpolateSessionMeasurement()`: a series value, the range-stats values
  and the GNSS coordinates of the header. Its pending-mark logic is
  unchanged.
- **Measure tool.** `measuretool.cpp` reads its ends through the session
  interpolation, in single-track and multi-track modes. An end inside a hole
  reads "--", and the delta that needs it reads "--". The min/avg/max over
  the samples between the ends is unchanged (spec §4, decision 7). Its
  header coordinates go through `interpolateSessionMeasurement()`.
- **Set Ground tool.** `SetGroundTool::computeGroundElevation()`
  (`setgroundtool.cpp`) reads the elevation at the clicked time through the
  session interpolation instead of its own `lower_bound` and linear step: a
  click strictly inside a `GNSS/_time` hole gets NaN, as a click outside the
  samples does today, and the press handler then sets nothing, as it does
  for NaN today. At the samples around the hole it reads the sample's
  value.

### 4. The map (spec §4; decision 5)

- **Simplified track.** `builtin.simplified.track` judges holes on its
  existing input `GNSS/_time` and runs Ramer-Douglas-Peucker on each run of
  connected GNSS samples (each run's finite candidates) on its own. The two
  samples around every hole survive. For a recording without a hole the
  output is exactly today's. The declared inputs are unchanged.
- **`TrackMapModel`.** The `trackPoints` role becomes a list of runs, each a
  time-ordered list of `{lat, lon, t}`. The simplified points are split by
  the `GNSS/_time` run they belong to, never by `Simplified/_time` spacing.
  The plot-range filter and `addBoundaryPoint` never interpolate between
  points of two runs: a run that starts or ends inside the range simply
  starts or ends there. The class comment states the new shape and what a
  run with fewer than two points contributes; that choice is the
  implementer's. Bounds and centre are computed as today.
- **`MapCursorDotModel`.** `sampleLatLonAtUtc()` gives no dot at a UTC time
  strictly inside a `GNSS/_time` hole. At the fixes around it the dot is
  shown.
- **`MapWidget::onTracksReset()`** carries the runs to the page; the JSON key
  is the implementer's.
- **`src/resources/map.html.in`.** `onTracksChanged()` draws one
  `google.maps.Polyline` per run. `polylines` and `trackData` hold every run
  of a session. The pixel cache, the grid and the fallback hover loop build
  segments within a run only, so hover never spans two runs.
  `onPreferenceChanged()` reaches every polyline of every session. There is
  no automated test of the page: M54 and the role's shape in
  `tst_map_models` cover it.

### 5. Calculations (spec §4, §5; decisions 8, 9)

- **Interpolation family** (`interpolationcalculations.cpp`). Holes are
  judged on `timeVector` (`_time` in every caller). A marker strictly inside
  a hole is `CalculationResult::unavailable()`, as outside the range. A
  marker at the sample after a hole gives that sample's value. The comment
  that cites `interpolateAtX()` is updated. The declared inputs are
  unchanged.
- **Derivative helper** (`derivativehelper.h/.cpp`). `differenceOverStencil()`
  judges `times` with the authority. A sample whose stencil interval is a
  hole yields NaN at that sample only: interior samples whose two-interval
  stencil contains a hole, and the forward and backward ends when their one
  interval is a hole. The whole-vector refusals (empty, mismatched, fewer
  than two, equal times) and every other sample's bits are unchanged. The
  header contract says a stencil never spans a hole. `computeDerivative()`
  (the accelerations against `GNSS/time`, and `courseRate`, `diveAngleRate`,
  `specificEnergyRate`) and `computeDerivativeAccuracy()` (`accAcc` against
  `GNSS/_time`, the same buffer) are therefore unavailable at exactly the
  same samples. The audit's "one stencil" count of `[i + 1] -
  <name>[i - 1]` in `src` stays 1.
- **Ground elevation at the analysis end** (`builtin.attr.groundElev` in
  `attributecalculations.cpp`). It consults the authority on its existing
  input `GNSS/time` (the `_time` buffer) by sample index: an analysis end
  strictly inside a hole gives `CalculationResult::unavailable()`; its
  clamping to the first and last sample outside the range, and its value at
  and between connected samples, are unchanged. The declared inputs are
  unchanged. A user-set ground elevation takes precedence as before.
- **Crossing times stay.** The exit, the altitude markers, the WS-P and SP
  windows and every other crossing detection keep their linear
  interpolation across a hole (spec §4, decision 12 of the overview). Their
  tests are untouched.
- **Marker.** `CalculationCompatibilityVersion` goes from 2 to 3 in
  `builtincalculations.h`, with a history line ("3 - sample continuity: no
  value is interpolated or differenced across a hole"). The literal sites of
  finding 2 move to 3.

### 6. What does not change

- No sample is removed, inserted or altered in the session (spec §3). The
  break is a graph point only.
- The x-range extent, `keyRangeOf`, the logbook column machinery, the demand
  layer and the executor are unchanged.
- No dialog, badge or count of holes appears (spec §4).
- The crossing-time calculations are unchanged (decision D3).

## Interfaces

Provided (fixed by the overview; restated as contracts):

- `src/samplecontinuity.h/.cpp` in `flysight_model`. It has the nominal
  interval of an axis (none below three samples), the threshold in seconds
  (1.5 x nominal), the strict hole test of an interval against a threshold,
  whether the interval between samples `i - 1` and `i` is a hole, and the
  runs as index ranges (half-open or closed, stated in the contract). It
  takes contiguous doubles. Function names are the implementer's.
  `PLANS/gnss-holes-bridged.md` reads the runs and holes of a window's
  `gnssTime` through it.
- `Tuning::maxGap` keeps its name and meaning (seconds). `planFit()` sets it
  from the authority. It is the only threshold the kernel's IMU checks and
  `propagateAttitude()` read.
- `TrackMapModel`'s `trackPoints` role: a list of runs, each a list of
  `{lat, lon, t}` maps, in time order. `MapWidget` and `map.html.in` read
  it.
- Audit group slug `sample-continuity`; acceptance items 1201-1213;
  appendix M; matrix 9.13; manual step M54 in section 12.11.

Consumed: nothing from another phase.

## Acceptance criteria

Each criterion is traced to the specification (§) and its item.

1. (§3; 1201) On hand-built axes, the nominal interval is the median of the
   successive differences, with the fractional-rank value for an even count.
   An interval of exactly 1.5 nominal intervals is not a hole and one just
   above it is. Fewer than three samples give no nominal
   interval and no holes. An axis with two holes has three runs that
   partition its samples in order. A malformed axis gets the contract's
   answer without throwing.
2. (§4 Plots; 1202) The graph data of a GNSS series of a session with one
   GNSS hole has exactly one NaN-valued point, keyed strictly between the two
   samples around the hole. Every other point is a sample, in order. With
   the x variable `_system_time` the break is at the same sample pair.
3. (§2, §4 fusion plots; 1203) A series on the IMU axis of the same session
   has no break point.
4. (§4 legend and crosshair; 1204) `interpolateSessionMeasurement()` and the
   crosshair's graph interpolation are NaN strictly inside the hole. They
   equal the sample's value at the sample before and at the sample after it.
   Beyond the ends they behave as today.
5. (§4 measure tool; 1205) The measure tool's end read is NaN inside the hole
   and the sample's value at either sample around it. The min/avg/max code
   is unchanged.
6. (§4 map; 1206) For a session with one GNSS hole, the track model emits two
   runs, in order. Together they hold every point of the simplified track,
   each once (spec §6 "every finite fix", read as every fix the simplified
   track keeps). No range-edge point is interpolated across the hole. The
   cursor dot is absent strictly inside the hole and present at its two
   fixes. The simplified track of a path with a hole keeps both samples
   around it. Without a hole, its output is unchanged.
7. (§4 logbook measurements; 1207) An interpolation instance whose marker is
   strictly inside a hole is unavailable. At the two samples around it, the
   instance is available. The marker is 3.
8. (§4 ground elevation reads; 1208) `computeGroundElevation()` is NaN at
   a time strictly inside a hole and the sample's value at the two samples
   around it; `builtin.attr.groundElev` is unavailable for an analysis end
   strictly inside a hole and is today's value at those samples and outside
   the range.
9. (§4 derived values; 1209) For a known-answer case with a hole cut in, the
   derivative and its accuracy are NaN exactly at the samples whose stencil
   spans the hole, ends included. Every other sample is bit for bit the
   value of the same formula on the same data. `tst_builtins_golden` is
   unchanged.
10. (§4 crossing times; 1210) The crossing-time calculations are byte for
    byte unchanged, their tests pass untouched, and the "Holes in the data"
    section names them as the exception.
11. (§5; 1211) `git grep kImuGapMedians` over `src tests docs README.md` is
   empty. `medianInterval`, `quantile(`, `componentMean` and
   `componentStddev` do not occur in `src` or `tests`, and
   `samplestatistics` is absent from `src`, `tests` and the audit. The factor appears in `src` only in
   `samplecontinuity.*`. `planFit()` sets `maxGap` from the
   authority. `propagateAttitude()` reads its threshold from its caller and
   throws "Anchor propagation cannot bridge an IMU gap" across an interval
   above it. `audit_cleanup` passes with the new group.
12. (§6; 1212) `reject_imu_gap` rejects with "IMU gap at 0.353000 s; fusion
    unavailable across missing data". Every fusion golden matches bit for
    bit, the exact runs included.
13. (§7; 1213) The documents in "Documentation" below say what is listed
    there. The audit pins the sentences it can.

## Tests

Run in `build-agent/` only, sequentially, Release (`CLAUDE.local.md`).

**`tst_sample_continuity` (new; decision D1).** It is registered with
`flysight_add_test` and compiled with `"${FLYSIGHT_SRC_DIR}/plotutils.cpp"`,
as `tst_plot_format` is. It covers:

- the authority on hand-built axes (criterion 1);
- the graph-data function on a synthetic `SessionData` with a GNSS hole and a
  regular IMU axis, under both x variables (criteria 2, 3). Storing `_time`
  directly is fine: stored data wins, as `tst_simplified_track` notes;
- the session interpolation, the measure tool's end read, the Set Ground
  tool's `computeGroundElevation()` and the crosshair's graph interpolation
  inside the hole, at both samples and beyond the ends (criteria 4, 5, 8).
  `computeGroundElevation()` must be reachable without a widget; if it is
  not, the implementer moves the read into the plot utilities and the tool
  calls it.

Function names are the implementer's. The map lines cite them.

**`tst_builtins_engine`.**

- Amend `accelerationAccuracyKnownAnswers` (decision D2).
- Add a function for the derivative and accuracy with a hole cut into the
  known-answer data: accN, accE, accD and accAcc, the hole as an interior,
  first and last interval (criterion 9).
- Add a function for the interpolation family with a marker inside a hole
  and at both samples around it (criterion 7).
- Add a function for `builtin.attr.groundElev` with the analysis end inside
  a hole, at both samples around it, and outside the range (criterion 8).
- `accelerationAccuracyUnavailable`, `interpolationInstances` and
  `interpolationUnavailableIsCached` are unchanged.

**`tst_map_models`.** Existing functions read `trackPoints` as runs: one run
for the descent fixture, with the same points, bounds and range-edge times
(`T0 + 8.75`, `T0 + 9.25`). Add a function with a GNSS hole cut into a
session: two runs, the edges and the dot (criterion 6).

**`tst_simplified_track`.** Add a function: a path whose `GNSS/time` has a
hole simplifies each run on its own, keeps the two samples around the hole,
and keeps the seven outputs aligned. Existing functions are unchanged; their
time is the sample index, with no hole.

**`tst_fusion_kernel`.**

- The seven `maxGap = kImuGapMedians*medianInterval(...)` sites (including
  `pipelineTuning()` and `fitOfChannels()`) and the
  `medianInterval(f.window.imuTime)` comparison read the authority.
- The direct `propagateAttitude(...)` calls pass a threshold.
- Add a check that `propagateAttitude()` throws across an IMU interval above
  the threshold it is given, and that `planFit()`'s `maxGap` is the
  authority's threshold, through whatever seam exists (criterion 11).
- `validationRejectsEachDefect` and `configurationChecksComeAfterTheOthers`
  are unchanged.

**`tst_fusion_golden`.** Unchanged and green (criterion 12).

**Marker.** `tst_logbook_index::markerWrittenOnFlush` and the other literal
sites of finding 2 expect 3. `tst_result_records` and `tst_fusion_store`
read the constant and need nothing.

Run the whole suite. Any other test whose fixture has an interval above 1.5
nominal intervals shows up there and is treated like
`accelerationAccuracyKnownAnswers`: its data or its expectation changes,
with the reason in the test's comment.

**Audit: group `sample-continuity`** in `tests/audit/cleanup_audit.cmake`.
Follow the `gnss-acceleration-accuracy` block's shape and its `Allow:`
comments, use `WB_START`/`WB_END` only, and plant each rule once to prove it
matches. The rules:

- the factor (`1.5` as a number, not inside a longer number) appears in
  `src` only in `src/samplecontinuity.*`;
- the phrase that defines a hole in the unit's contract (the implementer
  picks it; for example "strictly greater than 1.5 times the nominal
  interval") is spelled once in `src`, in the authority;
- no other median of a time axis for a continuity decision exists in `src`:
  `medianInterval`, `quantile\(` and a selection over intervals appear only
  in the authority;
- each file that consults the authority directly includes its header:
  `plotutils.cpp`, `interpolationcalculations.cpp`, `derivativehelper.cpp`,
  `attributecalculations.cpp`, `simplificationcalculations.cpp`,
  `TrackMapModel.cpp`, `MapCursorDotModel.cpp` and the kernel files that
  ask it. `PlotWidget.cpp` calls the graph-data function; `setgroundtool.cpp`
  reads through the session interpolation and has no `lower_bound` of its
  own;
- `kImuGapMedians` and `samplestatistics` are absent from `src tests docs
  README.md`;
- the documents: `docs/COMPUTED_PLOTS.md` has the "Holes in the data"
  section once, and `docs/SENSOR_FUSION.md` no longer says "1.6 times the
  median IMU interval".

Also extend the file's head comment with a bullet for the group. Extend the
traceability block: its comment, the range test and message ("... 1101 and
1201-1213"), and a `foreach(item RANGE 1201 1213)`.

**`tests/acceptance_map.txt`.**

- Header: thirteen specifications, the 1201-1213 range described, the
  sections "9.1 to 9.13", the coverage sentence.
- A last section, "Sample continuity (PLANS/sample-continuity.md): thirteen
  items", in the 1101 section's form. It has the item comment lines and at
  least one test or audit line per item. The manual line `manual M54` is on
  1202, 1204, 1205, 1206 and 1208.
- No existing item is restated (spec §6).

**`tests/README.md`.**

- Section 1: a row for `tst_sample_continuity`. The counts become 54
  executables, 55 `ctest -N` entries, and 62 with the exact runs. Update the
  rows of `tst_builtins_engine`, `tst_map_models`, `tst_simplified_track`
  and `tst_fusion_kernel` where their content changed.
- 9.13, the matrix of 1201-1213, in 9.12's form.
- Section 10: a bullet for `sample-continuity`.
- Section 11's fixture row `reject_imu_gap` states the rule: `.11 s > 1.5 x
  .01 s`.
- Section 12's script count goes up by one, and section 12.11 has **M54**.
  On a corpus recording with a GNSS hole (`24-09-07/08-35-48` or
  `24-09-04/13-35-10`), on a copy of a logbook:
  - elevation and a GNSS speed break at the hole against both x variables;
  - an IMU plot draws through;
  - the legend reads "--" inside the hole;
  - a measure-tool end inside it reads "--";
  - a Set Ground click inside it sets nothing;
  - the map track has a visible break, and the cursor dot is absent while
    the cursor is inside the hole.
- Appendix M lists the thirteen items in Appendix L's form.

**Documentation.**

- `docs/COMPUTED_PLOTS.md` gets a new section, "Holes in the data", and a
  table-of-contents entry. It says:
  - what a hole is: a sensor's own `_time`, the median interval, 1.5 times
    it, per sensor;
  - that nothing is drawn, read, interpolated or differenced across one:
    the plot's break, the legend's "--", the measure tool, the Set Ground
    tool and the automatic ground elevation, the map's break and absent
    dot, logbook measurements at a marker, the derivatives at the samples
    around it;
  - that the fusion plots are on the IMU's axis, so a GNSS hole is not a
    hole in them;
  - that the crossing times (exit, altitude markers, the analysis windows)
    are the one exception: a crossing inside a hole is placed by linear
    interpolation between the samples around it.

  The bridging sentence belongs to `PLANS/gnss-holes-bridged.md`. Avoid the
  fusion-reconstruction rule's banned phrases, such as "states interpolated".
- `docs/CALCULATIONS.md`:
  - section 18: the accuracy, like the derivative, is unavailable at a
    sample whose stencil spans a hole. The formula block and the two
    sentences the audit counts stay one line each;
  - section 17: the marker's current value is 3, with what it identifies;
  - section 9 lists both reasons for the bump already and needs no change.
- `docs/SENSOR_FUSION.md` section 6 states the IMU gap rule as the
  application's continuity rule at 1.5 times the median IMU interval. Its
  bullet "fewer than three GNSS fixes and two IMU samples" becomes three IMU
  samples (decision D5).
- `docs/LOCAL_COORDINATES.md` section 9: the track is simplified per run of
  connected fixes, both fixes around a hole are kept, and the list of what
  `tst_simplified_track` covers is updated.

## Decisions

- **D1. One new test executable, `tst_sample_continuity`,** holds the
  authority's tests and the plot-utility tests (graph data, the session
  interpolation, the crosshair's interpolation). Reason: the unit is new, the
  plot-utility functions it serves have no widget-free home, and
  `tst_plot_format` is about formatting. It is compiled with `plotutils.cpp`
  as `tst_plot_format` is.
- **D2. `accelerationAccuracyKnownAnswers` keeps an uneven interval that is
  not a hole, off the boundary.** Its 0.5 s interval becomes 0.3125 s
  (`t[4] = T0 + 1.0625`, `t[5] = T0 + 1.3125`): 1.25 nominal intervals,
  clearly inside the rule, and exact binary fractions on the integer epoch,
  so every interval is exact and the last one is still 0.25 s. The test
  keeps its point (the stencil divides by the actual intervals) and its
  three hand-exact values (5.0, 2.5, 15.0 still hold). The boundary itself
  (exactly 1.5 nominal intervals is not a hole) is criterion 1's, in the
  authority's unit test with small exact values, and is not placed in a
  known-answer fixture. The hole case is the new function of criterion 9,
  cut into the same data. The alternative was to keep the data and expect
  NaN at samples 3 and 4, which would lose the uneven non-hole case.
- **D3. The two point reads are in; the crossing detections stay.**
  `SetGroundTool::computeGroundElevation()` and `builtin.attr.groundElev`
  are point reads with the measure tool's semantics and spec §4 names them,
  so they are hole-aware here (sections 3 and 5 above). The crossing-time
  calculations (exit, altitude markers, the WS-P and SP windows) keep their
  linear interpolation across a hole: spec §4 states them as the exception
  and the documentation names them. They are not touched.
- **D4. Item numbering.**
  - 1201 the rule (§3);
  - 1202-1210 the bullets of §4, in order: plots, fusion plots and per
    sensor, legend and crosshair, measure tool, map, logbook measurements,
    ground elevation reads, derived values, crossing times;
  - 1211 the architecture (§5);
  - 1212 the kernel and goldens (§6);
  - 1213 the documentation (§7).

  The audit ranges and appendix M follow this numbering.
- **D5. The kernel requires three IMU samples.** The full IMU axis needs a
  nominal interval, which an axis of two samples does not have. So
  `requireUsableRecording()` requires three IMU samples, and its message and
  `docs/SENSOR_FUSION.md` section 6 say three. No fixture or test has fewer
  than three IMU samples. `prepareInput()`'s own check for fewer than two,
  with its message, is unchanged.
- **D6. Ends of the measure tool.** It reads both ends through the session
  interpolation, as the legend does, so one hole-aware function serves both.
  Its min/avg/max is left alone (decision 7).

Status: ready with the caveats named at the top (findings 1-4), all settled
by decisions D2, D3 and D5 and the marker sites listed under Tests.
