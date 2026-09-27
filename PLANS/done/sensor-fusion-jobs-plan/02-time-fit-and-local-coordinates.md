# Phase 2: Time fit and local coordinates

## Overview

This phase fixes the precision of the shared `_TIME_FIT_A` / `_TIME_FIT_B`
calculation by computing the least-squares fit with centered sums (spec 5.1),
and adds the one on-demand, recording-wide local-coordinates calculation that
publishes the frame's origin attributes and the six `Local/...` channels, with
their six "GNSS (Local frame)" plots (spec 5.2). Both are ordinary cheap
built-ins in `flysight_core`; nothing here is explicit, threaded, or linked to
GTSAM. The phase also carries `docs/LOCAL_COORDINATES.md` over from the branch,
corrected for `master`'s engine, and adds the tests for acceptance 2 and the
local-coordinates half of acceptance 3.

## Dependencies

- **Depends on:** None — can begin immediately.
- **Blocks:** Phase 3 (Simplified track on the shared frame), Phase 9 (Fusion
  calculation, which declares `Local/...` and `_LOCAL_ORIGIN_*` as inputs),
  Phase 10.
- **Assumptions:**
  - `master` at or after `13c6115`: the registered-calculation engine is in
    place, a descriptor may declare attribute and measurement outputs together
    (`src/engine/calculationdescriptor.h` line 30: "attributes and/or
    measurements"), and `CalculationResult` carries both kinds in one atomic
    bundle (`src/engine/calculationresult.h`).
  - GeographicLib is already a PRIVATE link dependency of `flysight_core`
    (`src/CMakeLists.txt` line 308-310) and its DLL directory is already on
    `PATH` for every test (`tests/CMakeLists.txt` line 88). No build-system
    work beyond one source-list line is needed.
  - Work happens on branch `sensor-fusion-jobs`. The implementer runs no git
    command that changes repository state (overview, "Commit Policy").
  - The branch `sensor-fusion-clean-port` is a **behavioral** reference, read
    with `git show sensor-fusion-clean-port:<path>`. Its code uses the old
    per-value mechanism (`registerCalculatedAttribute`, sibling writes through
    `setCalculatedMeasurement`); none of that is copied. The arithmetic is
    retyped exactly (see "Gotchas").

## Tasks

### Task 2.1: Centered least-squares time fit

**Purpose:** Remove the tens-of-milliseconds precision loss of the uncentered
normal equations at realistic device uptimes (spec 5.1, acceptance 2).

**Files to modify:**
- `src/calculations/timecalculations.cpp` — rewrite the body of
  `fitSystemTimeToUtc` (lines 19-55). Nothing else in the file changes.

**Technical Approach:**

Keep the function's name, signature, and return type
(`std::optional<TimeFit> fitSystemTimeToUtc(systemTime, tow, week)`), the
descriptor `builtin.time.fit`, its three declared inputs, its two outputs, and
the `QString::number(x, 'g', 17)` publication. `docs/CALCULATIONS.md` section 2
quotes the registration block verbatim; it must still be accurate afterwards.

Keep, unchanged:
- `N = std::min({systemTime.size(), tow.size(), week.size()})`, `N < 2` gives
  `std::nullopt`.
- The UTC construction `utcTime[i] = week[i] * 604800 + tow[i] + 315964800`.

Replace the four uncentered sums and the `denom` formula with the centered
form, **in exactly this operation order** (it is the branch's order; the fused
output timestamps of Phase 9 depend on `a` and `b` bit for bit):

1. `systemReference = systemTime[0]`, `utcReference = utcTime[0]`.
2. First pass: `meanS += systemTime[i] - systemReference`,
   `meanU += utcTime[i] - utcReference`; then `meanS /= N`, `meanU /= N`.
3. Second pass: `S = (systemTime[i] - systemReference) - meanS`,
   `U = (utcTime[i] - utcReference) - meanU`, `variance += S * S`,
   `covariance += S * U`.
4. Degenerate fit: `if (!(variance > 0.0)) return std::nullopt;` — this takes
   the place of `denom == 0.0`.
5. `a = covariance / variance`;
   `b = (utcReference - a * systemReference) + (meanU - a * meanS)`.
   Keep the parentheses: each bracket is small-minus-small or an exact
   product, which is where the precision comes from.

Write a comment above the sums, at the density of
`src/calculations/derivativehelper.cpp`, that says *why* (the uncentered
normal equations subtract huge, nearly equal products of Unix UTC (~1.7e9) and
device uptime (~1e5), losing milliseconds even for an exact linear clock).
If it reads better, split the two passes into a small named helper inside the
anonymous namespace; do not introduce a class.

Consumers (`builtin.time.utc.*`, `builtin.time.system.*`,
`Calculations::systemTimeToUtc`, `utcToSystemTime`) are not touched.

**Acceptance Criteria:**
- [ ] `fitSystemTimeToUtc` contains no sum of `S * S` or `S * U` over
      uncentered values; the operation order is the one listed above.
- [ ] Ids, declared inputs, outputs, and the 17-significant-digit string
      publication of `builtin.time.fit` are byte-for-byte what they were.
- [ ] On the descent fixture `_TIME_FIT_A` is exactly the string `"1"` and
      `_TIME_FIT_B` exactly `"1704110400"` (existing assertions in
      `tst_builtins_engine::timeFitRunsOnce`, `tst_session_engine`,
      `goldenValues()` pass without edits to those assertions).
- [ ] `tst_builtins_golden`, `tst_builtins_engine` (except the inventory edit
      of Task 2.3), `tst_session_engine`, `tst_source_layer`,
      `tst_column_cache`, `tst_session_oracle`, `tst_workflow` pass with no
      change to any time-related expectation.

**Complexity:** S

---

### Task 2.2: Bump `CalculationCompatibilityVersion` to 2

**Purpose:** The rule in `src/calculations/builtincalculations.h` (lines 19-36)
and `docs/CALCULATIONS.md` section 9 is binding: a change to a built-in's
arithmetic that can alter a logbook column of an existing session requires a
bump. The centered fit moves every non-GNSS `_time` by up to tens of
milliseconds at high uptime, so every cached column interpolated on an
IMU/BARO/MAG/... time axis can change.

**Files to modify:**
- `src/calculations/builtincalculations.h` — `CalculationCompatibilityVersion = 2`;
  extend the "History:" comment: `2 - centered time fit: _TIME_FIT_A/B, and
  with them every non-GNSS _time, changed at high device uptime.`
- `tests/tst_logbook_index.cpp` — literals `1` -> `2` at lines 154, 155, 209;
  in `differentMarkerDiscards_data()` (lines 217-220) the rows must all still
  be *different* from the current marker: replace row `"2"` (`QJsonValue(2)`)
  with row `"1"` (`QJsonValue(1)`, the previous release) and add row `"3"`;
  replace row `"string 1"` with `"string 2"` (`QJsonValue(QStringLiteral("2"))`:
  right number, wrong type).
- `tests/tst_column_cache.cpp` — line 259, `1` -> `2`.
- `tests/tst_workflow.cpp` — line 335, `1` -> `2`.
- `tests/tst_session_oracle.cpp` — line 762 (`.toInt() == 1`), lines 949-950
  (`!= 1` and the message text "calculationCompatibility 1") -> `2`.

**Technical Approach:**

These tests state the marker as a literal on purpose (tests/README.md section
8: "Expected values are literals"); keep them literal, do not substitute the
constant. `tests/audit/cleanup_audit.cmake` line 165 requires exactly one
`CalculationCompatibilityVersion *=` in `src`; editing the value in place keeps
that true. Before finishing, run
`git grep -n "calculationCompatibility" -- tests` and confirm no other literal
`1` remains (line numbers above are from `13c6115` and may have drifted).

Adding the local-coordinate registrations (Task 2.3) needs **no** bump of its
own: pure additions are covered by the environment fingerprint
(`calculationEnvironmentFingerprint`).

**Acceptance Criteria:**
- [ ] `CalculationCompatibilityVersion` is 2 with a history line; `audit_cleanup`
      still passes its "one authority: compatibility marker" rule.
- [ ] `tst_logbook_index`, `tst_column_cache`, `tst_workflow`,
      `tst_session_oracle` pass; `differentMarkerDiscards` still has a row for
      a lower, a higher, a negative, a zero, and a wrong-typed marker.

**Complexity:** S

---

### Task 2.3: The local-coordinates calculation

**Purpose:** Provide the single recording-wide north/east/down frame that the
plots (this phase), the simplified track (Phase 3), and fusion (Phase 9) share
(spec 5.2).

**Files to create:**
- `src/calculations/localcoordinatecalculations.h` — declares
  `void registerLocalCoordinateCalculations(CalculationRegistry &registry);` in
  `FlySight::Calculations`, in the exact shape of
  `src/calculations/simplificationcalculations.h` (forward-declared registry,
  one doc comment listing the ids and outputs). No GeographicLib include, no
  constants.
- `src/calculations/localcoordinatecalculations.cpp` — the implementation.

**Files to modify:**
- `src/sessiondata.h` — add four keys to `namespace SessionKeys`, next to the
  other calculated-attribute keys, under a one-line comment ("Local coordinate
  frame origin (calculated)"):
  `LocalOriginLat[] = "_LOCAL_ORIGIN_LAT"`, `LocalOriginLon[] = "_LOCAL_ORIGIN_LON"`,
  `LocalOriginHmsl[] = "_LOCAL_ORIGIN_HMSL"`, `LocalOriginIndex[] = "_LOCAL_ORIGIN_INDEX"`.
  The sensor name is the string literal `"Local"`, as `"GNSS"` and
  `"Simplified"` are on `master`; there is no constant for it.
- `src/CMakeLists.txt` — one line in the `flysight_core` source list, directly
  after the `timecalculations` line (line 301):
  `calculations/localcoordinatecalculations.cpp calculations/localcoordinatecalculations.h`.
  No link change.
- `src/calculations/builtincalculations.cpp` — `#include "localcoordinatecalculations.h"`
  and one call `Calculations::registerLocalCoordinateCalculations(registry);`
  between `registerTimeCalculations` and `registerSimplificationCalculations`.
- `src/calculations/builtincalculations.h` — add "local coordinates" between
  "time" and "simplified track" in the order sentence of the doc comment
  (lines 10-14).

**Technical Approach:**

*One calculation, mixed outputs.* `master`'s engine supports attribute and
measurement outputs in one descriptor, so the spec's "one on-demand
calculation" is implemented literally; no split is needed. The branch's ten
per-output registrations that write siblings as side effects collapse into:

| Id | Policy | Declared inputs (this order) | Outputs |
|---|---|---|---|
| `builtin.local.coordinates` | OnDemand | `GNSS/lat`, `GNSS/lon`, `GNSS/hMSL`, `GNSS/hAcc`, `GNSS/velN`, `GNSS/velE`, `GNSS/velD` (all `CalcInput::measurement`) | attributes `_LOCAL_ORIGIN_LAT`, `_LOCAL_ORIGIN_LON`, `_LOCAL_ORIGIN_HMSL`, `_LOCAL_ORIGIN_INDEX`; measurements `Local/north`, `Local/east`, `Local/down`, `Local/velN`, `Local/velE`, `Local/velD` |
| `builtin.local.time` | OnDemand | `GNSS/_time` | `Local/_time` |
| `builtin.local.systemTime` | OnDemand | `GNSS/_system_time` | `Local/_system_time` |

Not declared, deliberately: `GNSS/sAcc`, any speed (spec: "Speed and speed
accuracy play no part"), `GNSS/_time` on the main calculation (the branch used
it only for a size check; declaring it would be "an input to be safe",
`docs/CALCULATIONS.md` section 3), any marker attribute, any preference (spec:
"Markers, zoom, and display preferences do not affect the frame").

*Time axes.* "`Local` shares the GNSS time axes" is two passthroughs that
return the GNSS vector itself, so the buffers are shared (implicitly shared
`QVector`), exactly like `builtin.time.utc.GNSS`
(`src/calculations/timecalculations.cpp` lines 89-100) and
`builtin.time.system.<sensor>` (lines 154-165). They live in
`localcoordinatecalculations.cpp`, not in `timecalculations.cpp`. They are
separate calculations because (a) all declared inputs are required, and
`GNSS/_system_time` needs the TIME sensor's fit: folding it into the main
calculation would make positions unavailable for a track-only recording;
(b) the plot widget reads `<sensor>/<xVariable>` for the plot's own sensor
(`src/ui/docks/plot/PlotWidget.cpp` line 540), so both axes must exist under
`Local`. They are not gated on the origin (branch behavior): with no
qualifying fix `Local/_time` still equals `GNSS/_time`, which is harmless
because every reader asks for the y data first (PlotWidget line 530).

*Structure of `localcoordinatecalculations.cpp`* (anonymous namespace, small
named functions, comments at the density of `attributecalculations.cpp`):

- `bool isValidPosition(double lat, double lon, double hMSL)` — all finite,
  `|lat| <= 90`, `|lon| <= 180`.
- `std::optional<int> findOriginIndex(lat, lon, hMSL, hAcc)` — the first index
  with `isValidPosition` **and** `std::isfinite(hAcc[i]) && hAcc[i] >= 0.0 && hAcc[i] < 10.0`.
  The upper bound is exclusive: `hAcc == 10` does not qualify; `hAcc == 0` does.
- `struct LocalTrack { QVector<double> north, east, down, velN, velE, velD; };`
- `LocalTrack transformToLocalFrame(originLat, originLon, originHmsl, lat, lon, hMSL, velN, velE, velD)`:
  - constructs one `GeographicLib::LocalCartesian frame(originLat, originLon, originHmsl)`
    (CSV hMSL passed as the ellipsoid height: the documented approximation);
  - all six vectors start as `QVector<double>(n, quiet_NaN)`;
  - per sample: if `!isValidPosition(...)` leave all six NaN and continue;
    call the rotation-returning overload
    `frame.Forward(lat, lon, h, east, north, up, rotation)` with a
    `std::vector<double> rotation(9)` allocated once outside the loop; write
    `north[i] = north`, `east[i] = east`, `down[i] = -up`;
  - velocity: if any of `velN[i]`, `velE[i]`, `velD[i]` is not finite, leave
    the three velocity entries NaN (positions for that sample stay valid);
    otherwise rotate with **exactly** these expressions (`n`, `e`, `d` are the
    recorded per-fix NED components; GeographicLib's matrix maps per-fix ENU
    to origin ENU, row-major):
    `rE = rotation[0]*e + rotation[1]*n - rotation[2]*d`,
    `rN = rotation[3]*e + rotation[4]*n - rotation[5]*d`,
    `rU = rotation[6]*e + rotation[7]*n - rotation[8]*d`;
    `velN[i] = rN`, `velE[i] = rE`, `velD[i] = -rU`.
- The compute lambda:
  1. reads the seven inputs through the `EvaluationContext`;
  2. if the seven sizes are not all equal, returns
     `CalculationResult::unavailable()` (an empty input never reaches compute:
     the engine reports `MissingInput`);
  3. `findOriginIndex`; none -> `CalculationResult::unavailable()`;
  4. otherwise returns one `CalculationResult` with all ten outputs set:
     the three origin values as `double` `QVariant`s taken verbatim from the
     origin sample (`lat[i]`, `lon[i]`, `hMSL[i]`, no rounding, no string
     form), `_LOCAL_ORIGIN_INDEX` as `QVariant::fromValue(qlonglong(i))`, and
     the six measurements with **no unit argument** (as every derived
     measurement on `master`; the plots carry the unit labels).

*Engine terms for the spec's statements.*
- "No qualifying fix makes every output unavailable": compute ran and returned
  an empty bundle. `resultStatus("builtin.local.coordinates") == ResultStatus::Ok`,
  every one of the ten names has `cachedState(...) == Unavailable`,
  `attribute()` returns an invalid `QVariant`, `measurement()` returns an empty
  vector, and the answer is cached with its dependencies like any other, so a
  change to any of the seven inputs gives exactly one new run. A recording
  without a GNSS column (for example no `hAcc`) is `ResultStatus::MissingInput`
  and compute does not run.
- "NaN where the sample is invalid": every measurement output always has
  exactly one entry per GNSS sample; an invalid sample is NaN *at its own
  index only* and never shortens or shifts the arrays. A vector that is
  entirely NaN is still an available output (only an empty vector is
  normalized to unavailable, `calculationresult.h` lines 16-19); this cannot
  happen for positions, because the origin sample itself is valid.
- "Source changes invalidate the outputs": nothing to implement; the engine's
  dependency records on the seven declared inputs do it.

*AttributeRegistry.* Do **not** register the four origin attributes in
`src/calculations/attributeregistration.cpp`. That registry feeds the logbook
column chooser and editing (`src/attributeregistry.h`); `_TIME_FIT_A/B` are not
registered either, and the branch did not register these. They are frame
identifiers for calculations and diagnostics, not user-facing columns.

**Acceptance Criteria:**
- [ ] `registeredIds()` contains `builtin.local.coordinates`,
      `builtin.local.time`, `builtin.local.systemTime`, in that order, directly
      after `builtin.time.system.VBAT` and before `builtin.simplified.track`.
- [ ] One read of any of the ten outputs runs `builtin.local.coordinates`
      exactly once; reading the other nine afterwards runs nothing
      (`runCount == 1`).
- [ ] `engine.undeclaredReadCount() == 0` and `cycleCount() == 0` after
      requesting every built-in on the descent fixture, and every built-in is
      `ResultStatus::Ok` there (`tst_builtins_engine::noUndeclaredReads`).
- [ ] The compute function includes nothing from `sessiondata.h` beyond
      `SessionKeys`, and touches no session, preference, clock, or Qt GUI
      object (audit rules in `tests/audit/cleanup_audit.cmake` still pass).
- [ ] `localcoordinatecalculations.h` does not include a GeographicLib header.

**Complexity:** M

---

### Task 2.4: The six "GNSS (Local frame)" plots

**Purpose:** Expose the channels as ordinary, cheap, on-demand plots (spec 5.2).

**Files to modify:**
- `src/mainwindow.cpp` — `MainWindow::registerBuiltInPlots()`, one contiguous
  block of six rows plus a category comment, inserted directly after the last
  "GNSS (Advanced)" row (`Specific energy rate`, ~line 914) and before the
  `// Category: IMU` comment. No other line of the file changes.

**Technical Approach:**

Rows follow the `PlotValue` aggregate order used by the neighbours
(category, name, units, colour, sensor, measurement, measurementType):

| Plot name | Units | Sensor / measurement | measurementType | Colour |
|---|---|---|---|---|
| North position | `m` | `Local` / `north` | `distance` | `QColor::fromHsl(  0, S_dk, L_dw)` |
| East position | `m` | `Local` / `east` | `distance` | `QColor::fromHsl(120, S_dk, L_dc)` |
| Down position | `m` | `Local` / `down` | `distance` | `QColor::fromHsl(240, S_dk, L_db)` |
| North velocity | `m/s` | `Local` / `velN` | `speed` | `QColor::fromHsl(  0, S, L_w)` |
| East velocity | `m/s` | `Local` / `velE` | `speed` | `QColor::fromHsl(120, S, L_c)` |
| Down velocity | `m/s` | `Local` / `velD` | `vertical_speed` | `QColor::fromHsl(240, S, L_b)` |

Category string exactly `"GNSS (Local frame)"`. Names, units, sensors,
measurements, and measurement types are the branch's
(`git show sensor-fusion-clean-port:src/mainwindow.cpp`, lines 970-975). The
branch's colours were `Qt::darkRed / darkGreen / darkBlue`; the same
red/green/blue-per-axis scheme is kept but expressed in the function's muted
palette, because the palette's own comments record that dark named colours
were "invisible on dark backgrounds". Positions use the deep variants,
velocities the standard ones, so a position and its velocity are
distinguishable when both are shown.

Nothing else is needed: per-plot preferences are registered from
`PlotRegistry::allPlots()` in `initializePreferences()`, and the x axis comes
from `Local/_time` / `Local/_system_time` (Task 2.3).

**Acceptance Criteria:**
- [ ] The plot list shows a "GNSS (Local frame)" category with the six plots
      in the order above, after "GNSS (Advanced)".
- [ ] Checking any of them draws a curve against both "UTC time" and "System
      time" for a recording with TRACK and SENSOR data, and against "UTC time"
      for a TRACK-only recording; no dialog, no job, no perceptible delay.
- [ ] The diff of `src/mainwindow.cpp` is one hunk.

**Complexity:** S

---

### Task 2.5: `tst_time_fit` (acceptance 2)

**Purpose:** Bound the UTC conversion error of an exact synthetic clock at high
uptime at the microsecond level, and pin the fit's edge behavior.

**Files to create:**
- `tests/tst_time_fit.cpp` — class `TimeFitTest`.

**Files to modify:**
- `tests/CMakeLists.txt` — one line, directly after the `tst_builtins_engine`
  line (line 114):
  `flysight_add_test(tst_time_fit SOURCES tst_time_fit.cpp)`.

**Technical Approach:**

Pattern: `tests/tst_builtins_engine.cpp` — a local `World` struct (private
`CalculationRegistry` with `registerBuiltInCalculations`, `FakePreferenceProvider`,
`FakeSessionState`, `CalculationEngine`), `FLYSIGHT_TEST_MAIN(TimeFitTest)`,
`#include "tst_time_fit.moc"`. No global registry, no importer, no files.

A file-local helper `addClock(FakeSessionState &, system, utc)` splits each UTC
value into `week = floor((utc - 315964800) / 604800)` and
`tow = utc - 315964800 - week * 604800`, and stores `TIME/time`, `TIME/tow`,
`TIME/week`, `IMU/time = system`, `GNSS/time = utc`. This is test-input
construction from the file-format definition, not a call into the code under
test.

Test functions:
- `highUptimeExactClock` — the branch's fixture
  (`git show sensor-fusion-clean-port:tests/time_regression.cpp`):
  `epoch = 1725729984.6`, `slope = 1.0000053`, 1850 pulses,
  `system[i] = 247616.413 + i / slope`, `utc[i] = epoch + i`. Assert:
  `IMU/_time` has 1850 entries and `max |IMU/_time[i] - utc[i]| < 2e-6`;
  `|_TIME_FIT_A.toDouble() - 1.0000053| < 1e-9`; `GNSS/_system_time` has 1850
  entries and `max |GNSS/_system_time[i] - system[i]| < 2e-6`;
  `runCount("builtin.time.fit") == 1`. Put a comment on the function: with the
  uncentered sums the first bound fails by four orders of magnitude (tens of
  milliseconds); that is what makes it a regression test.
- `fitFollowsTimeSource` — after the reads above,
  `state.setMeasurement(engine, "TIME", "tow", tow + 2)`: the returned set
  contains `_TIME_FIT_A`, `_TIME_FIT_B`, and `IMU/_time`; re-read `IMU/_time`
  is the earlier vector plus 2 within `2e-6`; run count is 2.
- `weekRollover` — system `{100, 101, 102}`, UTC
  `315964800 + 2400*604800 + {-1, 0, 1}`: `IMU/_time` equals `GNSS/time`
  exactly (`QCOMPARE` of the vectors); the values are small integers around
  the means, so the centered fit is exact.
- `degenerateFits` (data-driven): one pulse; two pulses with equal system time
  (`{1, 1}`) -> both fit attributes invalid, `IMU/_time` empty,
  `resultStatus("builtin.time.fit") == ResultStatus::Ok` (it ran and produced
  nothing); no TIME sensor at all -> `ResultStatus::MissingInput` and
  `runCount == 0`.
- `fixtureFitIsExact` — TIME `{10, 20, 30}` / tow `{129610, 129620, 129630}` /
  week 2295: `_TIME_FIT_A` is the `QString` `"1"`, `_TIME_FIT_B` the `QString`
  `"1704110400"` (the same literals the older tests use; stated here so a
  failure points at the fit rather than at a consumer).

All expected numbers are literals or the literal clock model above; nothing is
obtained from the code under test.

**Acceptance Criteria:**
- [ ] `ctest -R tst_time_fit` passes; the executable links
      `flysight_test_support` only (no extra `LIBS`).
- [ ] `highUptimeExactClock` fails when Task 2.1 is reverted (verify once
      locally by stashing the fit change; do not commit anything for this).
- [ ] `engine.undeclaredReadCount() == 0` at the end of every function.

**Complexity:** M

---

### Task 2.6: `tst_local_coordinates`, golden rows, inventory (acceptance 3, first half)

**Purpose:** Demonstrate origin gates, the analytic transform and velocity
rotation, NaN-at-index semantics, all-unavailable without a qualifying fix,
and invalidation on source changes.

**Files to create:**
- `tests/tst_local_coordinates.cpp` — class `LocalCoordinatesTest`.

**Files to modify:**
- `tests/CMakeLists.txt` — one line directly after the `tst_time_fit` line:
  `flysight_add_test(tst_local_coordinates SOURCES tst_local_coordinates.cpp)`.
- `tests/tst_builtins_engine.cpp` — `inventory()`: insert, after
  `"builtin.time.system.VBAT"`, a comment `// localcoordinatecalculations` and
  the three ids of Task 2.3; `QCOMPARE(ids.size(), 70)` -> `73` with the
  comment `2 conversion families + 70 calculations + 1 family`;
  `noUndeclaredReads()`: `QCOMPARE(plain, 67)` -> `70`.
- `tests/support/builtinfixture.cpp` — `goldenValues()`: a block
  `// ---- localcoordinatecalculations` between the sensor-time block and the
  simplification block (rows below).

**Technical Approach:**

*Golden rows* (descent fixture: every row has `hAcc = 1.0`, so the origin is
row 0 = 45.0 N, -75.0 E, 4000 m; the whole track is on one meridian; row 0 has
`velN = 50`, `velE = 0`, `velD = 0`). All derived by hand:

```
attr("_LOCAL_ORIGIN_LAT", 45.0, 0.0)      attr("_LOCAL_ORIGIN_LON", -75.0, 0.0)
attr("_LOCAL_ORIGIN_HMSL", 4000.0, 0.0)   attr("_LOCAL_ORIGIN_INDEX", 0.0, 0.0)
meas("Local", "north", 296, {{0, 0.0}}, 1e-6)
meas("Local", "east",  296, {{0, 0.0}, {295, 0.0}}, 1e-6)   // same meridian as the origin
meas("Local", "down",  296, {{0, 0.0}}, 1e-6)
meas("Local", "velN",  296, {{0, 50.0}})
meas("Local", "velE",  296, {{0, 0.0}, {295, 0.0}})        // rotation about the east axis only
meas("Local", "velD",  296, {{0, 0.0}})
meas("Local", "_time", 296, {{0, T0}, {295, T0 + 295.0}})
meas("Local", "_system_time", 296, {{0, 0.0}, {295, 295.0}})
```

(`compareToGolden` reads attributes with `toDouble()`, which a `qlonglong`
supports.) The golden rows are picked up automatically by
`tst_builtins_golden`, `tst_builtins_engine::goldenOnEngine`, and the oracle
catalogue (`tests/support/oraclecatalogue.cpp`), which gives the new names
randomized source-replacement and merge coverage for free.

*`tst_local_coordinates`.* Engine-level functions use the `World` pattern of
`tst_builtins_engine.cpp`; a file-local `addTrack(state, lat, lon, hMSL, hAcc)`
also stores `velN/velE/velD` (default all zeros, same length) and `GNSS/time`.
No GeographicLib in the test: every expectation is analytic on WGS84
(`a = 6378137 m`).

- `originGates` — eight fixes, `hAcc = {10, -1, NaN, +Inf, 1, 1, 1, 0}`, with
  fix 4 `lat = 91`, fix 5 `lon = 181`, fix 6 `hMSL = NaN`, fix 7
  `(0, 0, 100 m)`: `_LOCAL_ORIGIN_INDEX` is 7 (`userType` `QMetaType::LongLong`),
  `_LOCAL_ORIGIN_HMSL` 100.0, `_LOCAL_ORIGIN_LAT` 0.0, `_LOCAL_ORIGIN_LON` 0.0.
  Second data set for the inclusive bounds: a first fix at
  `lat = 90, lon = -180, hAcc = 0` is accepted (index 0).
- `speedAccuracyPlaysNoPart` — add `GNSS/sAcc` all `+Inf` and then all NaN:
  the returned invalidation set does not contain any `Local` name or origin
  attribute, the index is unchanged, `runCount("builtin.local.coordinates")`
  stays 1.
- `knownDisplacement` — origin `(0, 0, 0)`; fixes `(0, 0, 100)`,
  `(0, 90, 0)`, `(0.001, 0, 0)`; all `hAcc = 1`:
  `north[0], east[0], down[0]` are 0 within `1e-8`; `down[1] = -100` within
  `1e-8` (and `north[1]`, `east[1]` 0 within `1e-8`); `east[2] = 6378137` and
  `down[2] = 6378137` within `1e-6`, `north[2] = 0` within `1e-6` (a quarter
  of the equator: the origin's east axis points at it, and it lies one radius
  below the origin's tangent plane); `north[3] = 110.574276` within `1e-3`
  (meridian radius of curvature at the equator, `a(1 - e^2) = 6335439.327 m`,
  times `0.001 deg` in radians), `east[3] = 0` within `1e-6`,
  `0 < down[3] < 0.01`.
- `knownVelocityRotation` — same track, `velN = 2, velE = 3, velD = 4` at every
  fix: at the origin `(velN, velE, velD) = (2, 3, 4)`; at `(0, 90, 0)` it is
  `(2, -4, 3)` (that fix's down is the origin's negative east; its east is the
  origin's down); all within `1e-12`. Recorded `GNSS/velN/velE/velD` read back
  unchanged.
- `invalidSamplesAreNaNAtTheirIndexOnly` — five valid fixes; sample 1 has
  `lat = NaN`, sample 3 has `velE = NaN`: all six outputs have five entries;
  index 1 is NaN in all six; index 3 is NaN in the three velocity outputs and
  finite in the three position outputs; indices 0, 2, 4 are finite in all six;
  `down[2]` equals `-(hMSL[2] - hMSL[0])` within `1e-8` for a fix directly
  above the origin (proves nothing shifted).
- `noQualifyingFixMakesEverythingUnavailable` — all `hAcc = 10`: the four
  attributes are invalid, the six measurements empty,
  `resultStatus("builtin.local.coordinates") == ResultStatus::Ok`, `runCount == 1`,
  `cachedState(measKey("Local", "north")) == CachedState::Unavailable`; a second
  round of reads runs nothing. Second case: no `GNSS/hAcc` column at all ->
  `ResultStatus::MissingInput`, `runCount == 0`. Third case: `hAcc` of a
  different length than `lat` -> all unavailable, status `Ok`.
- `runsOnceForAllOutputs` — read the ten outputs in a scrambled order, twice:
  `runCount == 1`.
- `timeAxesAreTheGnssAxes` — with TIME data present (`addTimeData` as in
  `tst_builtins_engine.cpp` lines 51-56), `Local/_time == GNSS/_time` and
  `Local/_system_time == GNSS/_system_time` (`QCOMPARE` of vectors, exact);
  without TIME data `Local/_time` is still available and `Local/_system_time`
  is not, while `Local/north` is unaffected.
- `sourceChangesInvalidate` — **session level**: `TestEnvironment::instance().registerBuiltIns()`
  in `initTestCase()`, a programmatic `SessionData` built with
  `setMeasurement` (pattern: `tests/tst_session_engine.cpp` lines 30-40, and
  its `init()`/`cleanup()` registry snapshot). Read all ten outputs, then:
  `setMeasurement("GNSS", "hAcc", {10, 1, 1, 1})` returns a set containing all
  four attributes and all six measurements; afterwards the index is 1 and
  every position is relative to the new origin (the old origin sample is now
  at `down = +100` for a new origin 100 m above it). `setMeasurement("GNSS", "velE", ...)`
  invalidates `Local/velD` although only `Local/north` had been read since the
  last run (siblings go together). Making every `hAcc` 10 makes everything
  unavailable, and restoring it brings everything back (acceptance 3,
  "source changes invalidate the outputs"; also the engine half of Phase 3's
  "recovers when the source is corrected").
- `markersAndDisplayDoNotAffectTheFrame` — same session:
  `setAttribute(SessionKeys::ExitTime, ...)`, `setAttribute(SessionKeys::AnalysisStartTime, ...)`,
  `setAttribute(SessionKeys::GroundElev, ...)`: none of the returned sets
  contains a `Local` name or origin attribute, and
  `calculationEngine().runCount("builtin.local.coordinates")` stays 1. Also
  assert `!session.hasSensor("Local")` /
  `!session.sensorKeys().contains("Local")` after the reads (calculated
  outputs never appear in stored enumeration).

**Acceptance Criteria:**
- [ ] `ctest -R "tst_local_coordinates|tst_builtins_golden|tst_builtins_engine|tst_session_oracle"` passes.
- [ ] No test includes a GeographicLib header or computes an expectation with
      the code under test.
- [ ] `CalculationRegistry::instance().registeredIds()` is unchanged by the
      session-level functions (checked in `cleanup()`).

**Complexity:** M

---

### Task 2.7: Documentation and traceability

**Purpose:** Carry over and correct `docs/LOCAL_COORDINATES.md` (spec 4, 10)
and record which tests demonstrate acceptance 2 and 3.

**Files to create:**
- `docs/LOCAL_COORDINATES.md`

**Files to modify:**
- `docs/CALCULATIONS.md` — section 5, the registration-order sentence: insert
  "local coordinates" between "time" and "simplification"; append one sentence
  to section 4: "`builtin.local.coordinates` is the example of one calculation
  with attribute and measurement outputs together; see
  [LOCAL_COORDINATES.md](LOCAL_COORDINATES.md)." Two small hunks, nothing else
  (Phase 4 edits section 8 of the same file).
- `src/calculations/timecalculations.h` — no change needed (its comment stays
  true); listed so the reviewer does not look for one.
- `tests/acceptance_map.txt` — append one block at the end of the file (format
  below).
- `tests/README.md` — add two rows to the "Built-in calculations and sessions
  on the engine" table (`tst_time_fit`, `tst_local_coordinates`, one sentence
  each) and raise "There are 24 executables" by two.

**Technical Approach:**

*`docs/LOCAL_COORDINATES.md`.* Start from
`git show sensor-fusion-clean-port:docs/LOCAL_COORDINATES.md`; its description
of the frame, the origin gates, and the hMSL approximation is accurate and is
kept in substance. Corrections for this design:
- Say that **one** on-demand registered calculation
  (`builtin.local.coordinates`) produces the four attributes and six channels
  together, runs once per recording however many outputs are read, and needs
  no job; list its seven declared inputs; say that a recording with no
  qualifying fix has all ten outputs unavailable (the branch text said only
  "the coordinates").
- State the NaN rule precisely: one entry per GNSS sample; an invalid position
  is NaN in all six channels at that index; a non-finite velocity is NaN in
  the three velocity channels only.
- `Local/_time` and `Local/_system_time` are the GNSS axes (shared, not
  refitted).
- Say what does *not* affect the frame: markers, zoom, display preferences,
  speed and speed accuracy.
- Keep the paragraph on GeographicLib and "CSV hMSL is used as an approximate
  ellipsoid height, without geoid correction ... documented, not corrected".
- Replace the "Validation" line with `tst_local_coordinates` (and the golden
  rows in `tests/support/builtinfixture.cpp`).
- **Omit** the branch's "Simplified map track" section entirely. Phase 3 adds
  it, rewritten, when that behavior exists. End the file with no placeholder.
- No "Calculation lifecycle" material exists in this file on the branch; none
  is added.

*`tests/acceptance_map.txt`.* The file and the audit
(`tests/audit/cleanup_audit.cmake` lines 220-259) number items 1-19 of the
**previous** specification, and the audit requires each of those to keep a
line, so this plan's items must not reuse those numbers. The line format is
`^[0-9]+ +tst_[a-z_]+ +[A-Za-z_0-9]+$`. Convention for this plan: **item
number = 100 + acceptance number of `PLANS/sensor-fusion-jobs.md`**. Append:

```
# ---- Sensor fusion as an explicit calculation (PLANS/sensor-fusion-jobs.md) ----
# Items of that specification are numbered 100 + n, so that they cannot be
# confused with items 1-19 above.

# 102 - exact synthetic clock at high uptime: microsecond-level UTC conversion
102 tst_time_fit highUptimeExactClock
102 tst_time_fit fitFollowsTimeSource

# 103 - local coordinates (the simplified-track half is added by its own phase)
103 tst_local_coordinates originGates
103 tst_local_coordinates speedAccuracyPlaysNoPart
103 tst_local_coordinates knownDisplacement
103 tst_local_coordinates knownVelocityRotation
103 tst_local_coordinates invalidSamplesAreNaNAtTheirIndexOnly
103 tst_local_coordinates noQualifyingFixMakesEverythingUnavailable
103 tst_local_coordinates sourceChangesInvalidate
103 tst_local_coordinates markersAndDisplayDoNotAffectTheFrame
```

If another phase has already appended the three header comment lines, reuse
them and add only the item blocks. Do not touch the audit script or the file's
leading comment: Phase 10 owns the audit's range check and the README matrix
for items 101-120.

**Acceptance Criteria:**
- [ ] `docs/LOCAL_COORDINATES.md` exists, names no branch-only executable, no
      `registerCalculated*` API, and contains no "Simplified" section.
- [ ] `audit_cleanup` passes (every appended map line names an existing
      `void <Class>::<function>()`).
- [ ] `docs/CALCULATIONS.md` section 5 lists the registration order actually
      used by `registerBuiltInCalculations`.

**Complexity:** S

## Testing Requirements

### Unit Tests
- New: `tests/tst_time_fit.cpp` (Task 2.5), `tests/tst_local_coordinates.cpp`
  (Task 2.6).
- Updated: `tst_builtins_engine` (inventory list and the two counts), the
  golden table in `tests/support/builtinfixture.cpp` (twelve new rows), and
  the compatibility-marker literals in `tst_logbook_index`, `tst_column_cache`,
  `tst_workflow`, `tst_session_oracle` (Task 2.2).
- Must pass unchanged in their time-related expectations (acceptance 2,
  "existing time-dependent tests pass unchanged"): `tst_builtins_golden`,
  `tst_builtins_engine`, `tst_session_engine`, `tst_session_model_engine`,
  `tst_source_layer`, `tst_column_cache`, `tst_smoke`, `tst_workflow`,
  `tst_session_oracle`.

### Integration Tests
- The full suite: `ctest --test-dir build/FlySightViewer-build -C Release --output-on-failure -LE python`
  and, where Python/NumPy is available, the whole suite. `tst_session_oracle`
  (seeds 1-20 / 1-6) exercises the new golden names under randomized source
  replacement and merges and asserts `cycleCount() == 0`.
- `audit_cleanup` (`-L audit`).
- Build: `cmake --build build --config Release` from the repository root.

### Manual Verification
1. Start the application, import a TRACK+SENSOR recording. In the plot list,
   "GNSS (Local frame)" appears after "GNSS (Advanced)" with six plots.
2. Check "North position" and "Down velocity": curves appear immediately;
   north starts near 0 at the first good fix; down position mirrors elevation
   (falls as elevation rises). Switch the independent variable to "System
   time": the curves remain.
3. Drag the exit marker, change the ground elevation, zoom: the local curves
   do not change.
4. Open a recording whose IMU `_time` was previously visibly offset against
   GNSS at long uptime (if one is at hand): IMU and GNSS events line up.
5. First start after the change: the logbook's cached columns are discarded
   once and refill lazily (the compatibility marker moved to 2).

## Notes for Implementer

### Gotchas
- **Operation order is part of the contract.** The centered fit's five steps
  and the three velocity-rotation expressions are retyped with the branch's
  exact operand order and parenthesization. Do not "simplify"
  `(utcReference - a * systemReference) + (meanU - a * meanS)` and do not
  reorder the rotation terms; Phase 9's golden parity sits downstream.
- GeographicLib's `LocalCartesian::Forward` returns **east, north, up**. The
  outputs are **north, east, down**: swap the first two and negate the third,
  for positions and for the rotated velocity alike.
- The rotation-matrix overload takes a `std::vector<double>&` and resizes it
  to 9; allocate it once outside the loop.
- `hAcc` selects the origin only. A sample with a poor or NaN `hAcc` still
  gets coordinates if its position is valid.
- All seven inputs are required. A recording without `GNSS/hAcc` or without
  velocities has no local frame (`MissingInput`). Every FlySight TRACK file has
  these columns; do not add a second "positions only" candidate.
- "Stored data always wins" (`docs/CALCULATIONS.md` section 5): a *stored*
  attribute named `_LOCAL_ORIGIN_LAT` would override that one output without
  moving the frame. Nothing in the application can store one (the keys are not
  in the `AttributeRegistry`, so they are not editable); do not add code for it.
- `tst_builtins_engine::noUndeclaredReads` requires every built-in to be `Ok`
  on the descent fixture. It is, because the fixture has `hAcc = 1.0` and TIME
  data; if you change the fixture, this breaks first.
- The line numbers quoted for test literals are from `13c6115`; grep before
  editing.
- Shared files (`src/CMakeLists.txt`, `tests/CMakeLists.txt`,
  `src/calculations/builtincalculations.cpp`, `src/mainwindow.cpp`,
  `docs/CALCULATIONS.md`, `tests/acceptance_map.txt`, `tests/README.md`,
  `src/sessiondata.h`) are edited by parallel phases. Keep each edit to the
  single insertion point named in its task, add no blank-line or whitespace
  changes elsewhere, and list every touched path in the completion report so
  the orchestrator can stage by explicit path.
- Report no git state changes; the orchestrator commits.

### Decisions Made
- **One calculation with mixed outputs; no split.** The engine allows it, the
  spec asks for it, and it makes "the origin attributes and the channels always
  describe the same frame" true by construction (one atomic bundle).
- **Velocity inputs are required by the single calculation.** Consequence: no
  `velN/E/D` means no positions either. Accepted, because the spec defines one
  calculation, all declared inputs are required, and FlySight GNSS data always
  has velocities. The branch's position-without-velocity independence was an
  artifact of its per-output registrations.
- **Time axes are two separate ungated passthroughs** (`builtin.local.time`,
  `builtin.local.systemTime`), for the reasons given in Task 2.3. They are not
  among "every output" of the frame calculation; this matches the branch.
- **`GNSS/_time` is not an input of the frame calculation.** The branch's size
  check against it is dropped: on `master` `GNSS/_time` is the `GNSS/time`
  buffer, and a ragged GNSS sensor is caught by the seven-way size check.
- **Calculation ids** `builtin.local.coordinates`, `builtin.local.time`,
  `builtin.local.systemTime`; **file** `localcoordinatecalculations.{h,cpp}`;
  **entry point** `Calculations::registerLocalCoordinateCalculations`.
- **Keys live in `SessionKeys`** (`LocalOriginLat`, `LocalOriginLon`,
  `LocalOriginHmsl`, `LocalOriginIndex`), not in a `LocalCoordinates`
  namespace as on the branch: that is where `master` keeps every calculated
  attribute key, and `sessiondata.h` is in `flysight_model`, so Phase 9's
  `flysight_fusion` can use them without including a `flysight_core` header.
- **Attribute value types:** `double` for lat/lon/hMSL (exact, the recorded
  sample), `qlonglong` for the index. Not the 17-digit strings of the time
  fit: nothing needs a text form, and Phase 9 reads them with `toDouble()` /
  `toLongLong()`.
- **No units on the `Local` measurements**, like every derived measurement on
  `master`.
- **Origin attributes are not registered in the `AttributeRegistry`.**
- **Only the fit's arithmetic changes.** The branch also tightened validation
  (equal-length TIME columns instead of `min`, non-finite inputs rejected,
  `a <= 0` rejected). Spec 5.1 says "computed with centered sums. Names,
  inputs, outputs, and consumers are unchanged" and acceptance 2 requires
  existing tests to pass unchanged, so those are not ported. The one guard
  that changes form is the degenerate-fit check (`!(variance > 0.0)` in place
  of `denom == 0.0`), which the new formula needs.
- **`CalculationCompatibilityVersion` 1 -> 2**, as the constant's own rule
  demands for an arithmetic change. The new registrations need no bump.
- **Plot colours** use `master`'s muted palette in the branch's
  red/green/blue-per-axis scheme instead of the branch's dark named colours.
- **Acceptance-map numbering `100 + n`** for this specification's items.
- **`README.md` is not edited** by this phase (Phase 1 edits it in parallel);
  the new document is linked from `docs/CALCULATIONS.md`, and Phase 10 adds
  the README links.

### Open Questions
- None blocking. One point for the orchestrator: Phase 3 changes the
  simplified track's arithmetic and must decide for itself whether that
  warrants a further marker bump (to 3); values are never reused.

## Definition of Done

This phase is complete when:
1. All tasks have passing acceptance criteria
2. All tests pass (`ctest ... -LE python` at minimum, plus `audit_cleanup`)
3. Code follows patterns established in reference files
   (`timecalculations.cpp`, `simplificationcalculations.cpp`,
   `attributecalculations.cpp`, `tst_builtins_engine.cpp`)
4. No TODOs or placeholder code remains
5. The completion report lists every created and modified path, with the
   shared files called out
