# Phase 3: Simplified track on the shared frame

## Overview

This phase re-layers the map's `Simplified/...` calculation on the shared
`Local/north` / `Local/east` frame of Phase 2 (spec 5.3). The calculation stops
projecting on its own, keeps the horizontal Ramer-Douglas-Peucker
simplification at 0.5 m, retains the **indices** of the surviving samples
instead of re-matching coordinates, publishes seven equal-length outputs
(`lat`, `lon`, `hMSL`, `_time`, `north`, `east`, `down`), skips samples whose
local coordinates are not finite, and is unavailable when the shared frame is.
It also demonstrates, with a widget-free test that compiles the two map models
directly, that the map shows no track and no cursor dot for a recording
without a qualifying origin, excludes it from the bounds, clears the bounds
when no track remains, and recovers when the source is corrected (the
simplified-track / map half of acceptance 3).

## Dependencies

- **Depends on:** Phase 2 (Time fit and local coordinates).
- **Blocks:** Phase 10.
- **Assumptions:**
  - Phase 2 is committed on `sensor-fusion-jobs`: calculation
    `builtin.local.coordinates` publishes `Local/north`, `east`, `down` (one
    entry per GNSS sample, NaN at an invalid sample's own index only) together
    with the four `_LOCAL_ORIGIN_*` attributes; with no qualifying origin
    compute returns an empty bundle, so every `Local/...` position output is
    *unavailable* (`resultStatus == Ok`, `cachedState == Unavailable`); a
    recording without `GNSS/hAcc` or velocities is `MissingInput`.
    `registerLocalCoordinateCalculations` is called **before**
    `registerSimplificationCalculations` in
    `src/calculations/builtincalculations.cpp`.
  - `CalculationCompatibilityVersion` is 2 (Phase 2); the test literals for it
    are 2.
  - `docs/LOCAL_COORDINATES.md` exists without a "Simplified" section.
    `tests/acceptance_map.txt` ends with the `100 + n` block of Phase 2, whose
    item 103 comment says the simplified-track half is added by its own phase.
  - "Stored data always wins over any calculation" (`docs/CALCULATIONS.md`
    section 5), and an unknown stored measurement with empty unit text passes
    through the default conversion unchanged
    (`src/conversion/sourceconversion.cpp`, `convert()`, identity branch).
    The index-selection tests rely on this to store `Local/north`, `east`,
    `down` directly (Task 3.2).
  - The implementer runs no git command that changes repository state
    (overview, "Commit Policy"). The branch `sensor-fusion-clean-port` is read
    with `git show sensor-fusion-clean-port:<path>` for behavior only.

## Tasks

### Task 3.1: Index-retaining simplification on `Local/north` / `Local/east`

**Purpose:** Replace the private projection and the coordinate re-matching
search with direct index retention on the shared frame, and add the three
local-coordinate outputs (spec 5.3).

**Files to modify:**
- `src/calculations/simplificationcalculations.cpp` — rewritten (the file is
  small; everything between the includes and the end changes).
- `src/calculations/simplificationcalculations.h` — doc comment only.

**No other source file changes.** In particular `src/CMakeLists.txt` is *not*
edited (no new file; see "Decisions Made" for the now-unused Boost link),
`builtincalculations.cpp` is not edited (the registration call and its position
already exist), and no `SessionKeys` constant is added (`"Simplified"`,
`"Local"`, `"north"`, ... stay string literals, as on `master`).

**Technical Approach:**

*Descriptor.* The id stays `builtin.simplified.track` (every inventory list,
run-count assertion, and the environment fingerprint keep working), policy
OnDemand, one calculation.

| | |
|---|---|
| Declared inputs (this order, all `CalcInput::measurement`) | `GNSS/lat`, `GNSS/lon`, `GNSS/hMSL`, `GNSS/_time` (`SessionKeys::Time`), `Local/north`, `Local/east`, `Local/down` |
| Outputs (this order, all `DependencyKey::measurement("Simplified", ...)`) | `lat`, `lon`, `hMSL`, `_time` (`SessionKeys::Time`), `north`, `east`, `down` |

Not declared: `GNSS/hAcc`, velocities, the `_LOCAL_ORIGIN_*` attributes, any
marker or preference. The origin reaches this calculation only through the
`Local` channels. The first four outputs keep their meaning: the recorded
geographic sample and its UTC time. No unit argument on any output (as on
`master`; `tst_builtins_engine::simplifiedRunsOnce` asserts the empty unit).

*Removed.* `#include <boost/geometry...>`, `#include <GeographicLib/LocalCartesian.hpp>`,
the `LocalCartesian proj(...)`, the `LineString` types, `bg::simplify`, and the
whole forward-matching loop. Do not leave the word `LocalCartesian`,
`GeographicLib` or `boost` anywhere in the two files, comments included: Task
3.4 adds audit rules that are text searches.

*Structure* (anonymous namespace; small named functions with comments at the
density of `src/calculations/derivativehelper.cpp`; `qsizetype` indices):

- `constexpr double kToleranceMetres = 0.5;` with the existing comment
  ("sensor noise floor").
- `QVector<qsizetype> finiteSampleIndices(north, east, down)` — the indices
  `i` for which `std::isfinite` holds for all three of `north[i]`, `east[i]`,
  `down[i]`, ascending. This is the spec's "samples whose local coordinates
  are not finite are left out". Nothing else filters: `lat`/`lon`/`hMSL` of a
  sample with finite local coordinates are valid by Phase 2's
  `isValidPosition`, and `_time` is not inspected (the map models already
  ignore a non-finite time; `master` did not filter it either).
- `double squaredDistanceToSegment(...)` — horizontal squared distance from
  sample `i` to the segment `first`-`last`, with the branch's expressions
  (`git show sensor-fusion-clean-port:src/calculations/simplificationcalculations.cpp`):
  `dn = north[last] - north[first]`, `de = east[last] - east[first]`,
  `lengthSquared = dn*dn + de*de`, `n = north[i] - north[first]`,
  `e = east[i] - east[first]`,
  `t = lengthSquared > 0 ? std::clamp((n*dn + e*de) / lengthSquared, 0.0, 1.0) : 0.0`,
  `rn = n - t*dn`, `re = e - t*de`, result `rn*rn + re*re`. The clamp makes it
  a distance to the *segment*, which is what `boost::geometry::simplify` used
  on `master` (projected-point strategy); `lengthSquared == 0` (coincident
  endpoints: closed tracks, duplicate positions) degenerates to the distance
  to that point.
- `QVector<qsizetype> retainedIndices(north, east, candidates)` — iterative
  Ramer-Douglas-Peucker over the positions of `candidates` (the finite
  samples), returning a subset of `candidates`, ascending:
  - `candidates.size() <= 2`: return `candidates` unchanged (one point stays
    one point; two points stay two, also when they coincide).
  - Otherwise a `keep` flag per candidate, first and last set, and an explicit
    `std::vector<std::pair<qsizetype, qsizetype>>` work stack seeded with
    `(0, size - 1)`. **Not recursion:** a long recording that spirals can
    nest tens of thousands deep. For each popped span, find the candidate
    strictly between its ends with the largest squared distance; it is
    retained, and the span split at it, only if that distance is **strictly
    greater** than `kToleranceMetres * kToleranceMetres`; ties go to the
    **first** such candidate (compare with `>`, scanning ascending). Both
    rules are `boost::geometry`'s, so "the simplification is unchanged".
  - Segments join across skipped samples: the span `(a, b)` is between
    `candidates[a]` and `candidates[b]` whatever lies between them in the
    original arrays.
- `QVector<double> samplesAt(values, indices)` — `values[i]` for each index,
  reserved to size.
- The compute lambda:
  1. read the seven inputs through the `EvaluationContext`;
  2. if the seven sizes are not all equal: `CalculationResult::unavailable()`
     (an empty input never reaches compute; the engine reports `MissingInput`);
  3. `finiteSampleIndices`; empty: `CalculationResult::unavailable()`;
  4. `retainedIndices`;
  5. one `CalculationResult` with all seven outputs, each
     `samplesAt(<its input>, indices)`: `Simplified/lat` from `GNSS/lat`, ...,
     `Simplified/down` from `Local/down`.

  Because every output is `samplesAt` of the same index list, "all seven
  outputs always have the same length" and "every output is the original
  sample at those indices" hold by construction, and two samples at one
  position are two indices.

*Engine terms for the spec's statements.*
- "Unavailable when the shared frame is unavailable": nothing to implement.
  `Local/north` unavailable means a declared input is unavailable:
  `resultStatus("builtin.simplified.track") == ResultStatus::MissingInput`,
  compute does not run, all seven names are unavailable, and the engine has
  recorded the dependency, so a source change that gives the frame an origin
  invalidates all seven. There is no second candidate for any `Simplified`
  name: no fallback (spec 5.3).
- "The projection runs once per recording": this file performs none; the only
  `LocalCartesian` in `src` is in `localcoordinatecalculations.cpp`, which the
  engine runs once for all its outputs.

*Header comment* of `simplificationcalculations.h`: "Register the simplified
track (builtin.simplified.track: one calculation, seven measurement outputs
at the same retained sample indices; horizontal Ramer-Douglas-Peucker, 0.5 m,
on the shared Local/north and Local/east) with the calculation engine."

**Acceptance Criteria:**
- [ ] `simplificationcalculations.cpp` includes neither Boost nor GeographicLib
      and contains no projection and no search for matching coordinates.
- [ ] `registeredIds()` is unchanged (`tst_builtins_engine::inventory` passes
      with Phase 2's counts, 73 / 70).
- [ ] The descriptor declares exactly the seven inputs and seven outputs above,
      in that order; `engine.undeclaredReadCount() == 0` after reading them.
- [ ] On the descent fixture `Simplified/lat`, `lon`, `hMSL`, `_time` have the
      values they had before (existing assertions in `tst_builtins_golden`,
      `tst_builtins_engine::simplifiedRunsOnce`,
      `tst_session_engine::readMultiOutputGroups` pass **unedited**). If one
      of them changes, stop and explain; do not edit the literal.
- [ ] The compute function touches no session, preference, clock, or Qt GUI
      object (`audit_cleanup`, "pure compute functions").

**Complexity:** M

---

### Task 3.2: `tst_simplified_track` and golden rows (acceptance 3, simplified-track half)

**Purpose:** Demonstrate index alignment, the tolerance, duplicate-position
endpoints, closed / degenerate / empty tracks, non-finite skipping, the single
projection, and unavailability with recovery at engine level.

**Files to create:**
- `tests/tst_simplified_track.cpp` — class `SimplifiedTrackTest`.

**Files to modify:**
- `tests/CMakeLists.txt` — one line directly after Phase 2's
  `tst_local_coordinates` line:
  `flysight_add_test(tst_simplified_track SOURCES tst_simplified_track.cpp)`.
- `tests/support/builtinfixture.cpp` — `goldenValues()`, in the existing
  `// ---- simplificationcalculations` block, three rows appended to the four
  that are there (which do not change):
  ```
  << meas("Simplified", "north", 2, {{0, 0.0}}, 1e-6)
  << meas("Simplified", "east",  2, {{0, 0.0}, {1, 0.0}}, 1e-6)   // same meridian as the origin
  << meas("Simplified", "down",  2, {{0, 0.0}}, 1e-6)
  ```
  (Row 0 is the origin; the track runs along one meridian, so both retained
  samples have east 0. North and down of the last sample are not
  hand-derivable to a useful tolerance and are covered relationally below.)
  The oracle catalogue picks the rows up automatically.
- `tests/tst_builtins_engine.cpp` — `simplifiedRunsOnce()` only: inside the
  two-pass loop add `QCOMPARE(engine.measurement("Simplified", "north").size(), 2);`
  and the same for `east` and `down`; after the loop add
  `QCOMPARE(engine.runCount("builtin.local.coordinates"), 1);` and
  `QCOMPARE(engine.measurementUnit("Simplified", "north"), QString());`.
  The existing lines stay as they are.

**Technical Approach:**

Pattern: `tests/tst_builtins_engine.cpp` lines 33-49 — a file-local `World`
(private `CalculationRegistry` with `registerBuiltInCalculations`,
`FakePreferenceProvider`, `FakeSessionState`, `CalculationEngine`), fresh per
test function; `FLYSIGHT_TEST_MAIN(SimplifiedTrackTest)`;
`#include "tst_simplified_track.moc"`. No global registry, no importer, no
files, no GeographicLib.

Two file-local builders:

- `addPath(state, north, east, down = {})` — **isolates index selection from
  the projection** by storing the local channels directly (stored data wins):
  for `i` in `0..n-1` stores `GNSS/time = i`, `GNSS/lat = 45 + i * 0.001`,
  `GNSS/lon = -75 + i * 0.001`, `GNSS/hMSL = 1000 + i`, and `Local/north`,
  `Local/east`, `Local/down` (default `down[i] = -i`). `Local/east` may be given
  a different length on purpose. Because `GNSS/_time` is `GNSS/time`,
  **`Simplified/_time` reads back as the retained indices**, so expectations
  are literal index lists.
- `addGnssTrack(state, lat, lon, hMSL, hAcc)` — the real path: also stores
  `GNSS/time = i` and zero `velN/velE/velD` of the same length (Phase 2's
  calculation requires them), and nothing under `Local`.

A helper `verifyAlignment(engine, state)` asserts: the seven outputs have the
same size; `idx[k] = Simplified/_time[k]` is strictly increasing; and for each
of the seven, `output[k] == input[idx[k]]` **exactly** (`==` on doubles, no
tolerance), where the input is the effective `GNSS/...` or `Local/...`
measurement read through the engine.

Test functions:

- `sevenOutputsShareIndices` — `addPath` with 1000 samples,
  `north[i] = 0.1 * i`, `east[i] = 5 * sin(0.02 * i)`: `verifyAlignment`;
  `2 < size < 100`; `runCount("builtin.simplified.track") == 1` after reading
  the seven in a scrambled order twice; `runCount("builtin.local.coordinates") == 0`
  (stored `Local` data won; nothing projected).
- `droppedSamplesWithinTolerance` — same path. For each consecutive retained
  pair `(a, b)` and every `i` in `a..b`, the distance from sample `i` to the
  segment `a`-`b`, computed by a small function written in the test (clamped
  projection, `std::hypot`), is `<= 0.5 + 1e-9`. Plus the two literal cases:
  north `{0, 1, 2}` with east `{0, 0.49, 0}` gives `_time == {0, 2}`; with east
  `{0, 0.51, 0}` gives `{0, 1, 2}`.
- `duplicatePositionEndpoints` — four samples all at north 0 / east 0:
  `_time == {0, 3}`, `hMSL == {1000, 1003}`, `down == {0, -3}`,
  `lat == {45.0, 45.003}`. Comment on the function: `master`'s re-matching
  search returned sample 0 twice here (the search for the second point started
  at, and matched, the first); that is the defect index retention removes.
  Second case: a straight path whose last two samples coincide
  (north `{0, 10, 20, 20}`) gives `{0, 3}`, the true last sample.
- `closedTrack` — north `{0, 3, 0}`, east zeros: `{0, 1, 2}` (the excursion
  survives coincident endpoints). A square returning to its start, north
  `{0, 10, 10, 0, 0}`, east `{0, 0, 10, 10, 0}`: all five retained.
- `degenerateTracks` (data-driven) — one sample: all seven have size 1, index
  `{0}`; two samples 100 m apart: `{0, 1}`; two coincident samples: `{0, 1}`;
  five collinear samples: `{0, 4}`; five samples all within 0.3 m of each
  other: `{0, 4}`. Each row: `resultStatus == Ok`, `verifyAlignment`.
- `emptyTrack` — no `GNSS` data at all, and (second row) zero-length GNSS and
  Local columns: all seven outputs empty,
  `resultStatus("builtin.simplified.track") == ResultStatus::MissingInput`,
  `runCount == 0`.
- `nonFiniteSamplesAreSkipped` (data-driven), five samples, north
  `{0, 1, 2, 3, 4}` unless stated:
  - east `{0, NaN, 0.51, 0, 0}` gives `{0, 2, 4}` (the hole is bridged; its
    neighbour is judged against the segment 0-4);
  - north `{NaN, 1, 2, 3, NaN}`, east zeros gives `{1, 3}` (the endpoints are
    the first and last *finite* samples);
  - east `{0, 0, 0.51, 0, 0}` with `down[2] = NaN` gives `{0, 4}` (a sample
    that would have been retained is left out when any of its three local
    coordinates is not finite);
  - east `{0, +Inf, 0, -Inf, 0}` gives `{0, 4}`;
  - every north NaN: all seven unavailable, `resultStatus == Ok`,
    `runCount == 1`.
  In every available row no output contains a non-finite value, and
  `verifyAlignment` holds.
- `mismatchedLengthsUnavailable` — `Local/east` one sample shorter: all seven
  unavailable, `resultStatus == Ok`.
- `projectionRunsOncePerRecording` — `addGnssTrack` with ten fixes,
  `lat[i] = 45 + i * 1e-4` (about 11 m apart), `lon = -75` except
  `lon[5] = -75 + 1e-4` (about 7.9 m east), `hMSL = 1000`, `hAcc = 1`. Read the
  seven `Simplified` outputs, the six `Local` outputs and the four origin
  attributes, interleaved, twice: `runCount("builtin.local.coordinates") == 1`,
  `runCount("builtin.simplified.track") == 1`, `undeclaredReadCount() == 0`,
  `cycleCount() == 0`. `Simplified/_time == {0, 4, 5, 6, 9}` (worked by hand:
  sample 5 is 7.9 m off the chord 0-9; samples 1-4 lie on the meridian, up to
  6.3 m off the chord 0-5, and are collinear among themselves, so only 4
  survives; 6-8 mirror that). `verifyAlignment` then proves
  `Simplified/north` etc. are exactly the `Local` samples at those indices,
  i.e. produced by the one projection. The static half of this clause is the
  audit rule of Task 3.4.
- `unavailableWithoutOriginAndRecovers` — `addGnssTrack` with `hAcc` all 10:
  seven outputs empty, `resultStatus == MissingInput`,
  `runCount("builtin.simplified.track") == 0`,
  `cachedState(measKey("Simplified", "lat")) == CachedState::Unavailable`.
  `state.setMeasurement(engine, "GNSS", "hAcc", all 1)`: the returned set
  contains all seven `Simplified` names; the outputs are available,
  `runCount == 1`. Back to all 10: the set again contains all seven; they are
  unavailable again (no stale geometry). A `NaN` latitude at one interior
  sample of the good track: that index never appears in `Simplified/_time`.
- `siblingsInvalidateTogether` — `addPath` with east `{0, 0.51, 0}`; read only
  `Simplified/lat`; `state.setMeasurement(engine, "Local", "east", {0, 0, 0})`
  returns a set containing `Simplified/hMSL` and `Simplified/down`; afterwards
  `Simplified/hMSL.size() == 2` and `runCount == 2`.

All expectations are literals or independently stated properties; nothing is
obtained from the code under test (`tests/README.md` section 8).

**Acceptance Criteria:**
- [ ] `ctest -R "tst_simplified_track|tst_builtins_golden|tst_builtins_engine|tst_session_engine|tst_session_oracle"`
      passes.
- [ ] `duplicatePositionEndpoints` fails against `master`'s implementation
      (verify once locally by stashing Task 3.1; commit nothing for this).
- [ ] The executable links `flysight_test_support` only and includes no
      GeographicLib or Boost header.
- [ ] `engine.undeclaredReadCount() == 0` at the end of every function.

**Complexity:** M

---

### Task 3.3: Map models with an unavailable track (`tst_map_models`)

**Purpose:** Show, on the real `TrackMapModel` and `MapCursorDotModel` driven
by the real `SessionModel`, that a recording without a qualifying origin has
no track and no cursor dot, is excluded from the bounds, that the bounds are
cleared when no track remains, and that everything returns when the source is
corrected (spec 5.3, last two bullets; acceptance 3, last sentence).

**Files to create:**
- `tests/tst_map_models.cpp` — class `MapModelsTest`.

**Files to modify:**
- `tests/CMakeLists.txt` — a second block after the `tst_workflow` line and
  before the embedded-Python section:
  ```cmake
  # Map models on the simplified track (no track without a local origin). The
  # two models and their two helpers are application sources that need Qt Core
  # and Gui only, so they are compiled into the test; nothing here needs
  # Widgets, WebEngine or Quick.
  flysight_add_test(tst_map_models
    SOURCES tst_map_models.cpp
            "${FLYSIGHT_SRC_DIR}/ui/docks/map/TrackMapModel.cpp"
            "${FLYSIGHT_SRC_DIR}/ui/docks/map/MapCursorDotModel.cpp"
            "${FLYSIGHT_SRC_DIR}/plotrangemodel.cpp"
            "${FLYSIGHT_SRC_DIR}/plotutils.cpp")
  ```
  and, in the file's header comment, extend the sentence "The one exception is
  tst_python_bridge ..." to name `tst_map_models` as the second test that
  compiles application sources, with the reason above.
- `src/ui/docks/map/TrackMapModel.h` — class comment only: add that each point
  also carries `"t"` (UTC seconds), that a visible recording whose `Simplified`
  track is unavailable (no local-frame origin) or shorter than two points
  contributes no row and nothing to the bounds, and that with no row
  `hasData` is false and center and bounds are all 0.
- `src/ui/docks/map/MapCursorDotModel.h` — class comment only: one sentence,
  "A session whose Simplified track is unavailable gets no dot."

**No behavioral change to either model is specified**, because none is needed
(see "Technical Approach"). The spec allows changes only "as needed"; the
test is what decides. The one permitted contingency is described at the end
of this task.

**Technical Approach:**

*Why the models already behave.* Both read `Simplified/lat`, `lon`, `_time`
through `SessionData::getMeasurement` on every rebuild and hold no per-session
state between rebuilds:
- `TrackMapModel::rebuild()` (`TrackMapModel.cpp` lines 143-321) skips a
  session with fewer than two samples *before* it touches the bounds
  accumulator (lines 175-177), sets `m_hasData = !m_tracks.isEmpty()`, and
  resets center and bounds to 0 when nothing accumulated (lines 305-312),
  emitting `hasDataChanged` / `boundsChanged` on a difference.
- `MapCursorDotModel::rebuild()` adds a dot only when `sampleLatLonAtUtc`
  succeeds, which needs two samples (line 54).
- Both rebuild on `SessionModel::modelChanged` and `visibilityChanged`. Every
  path that can change a recording's source measurements in the application
  ends in `modelChanged`: `mergeSessions` (the only import path;
  `sessionmodel.cpp` line 733) and the deferred flush of registry / preference
  invalidations (line 1282). `updateAttribute` / `removeAttribute` do not emit
  it, but no attribute is an input of `Local/...` or `Simplified/...`.
- "Bounds cleared" for the consumers: `MapWidget::onBoundsChanged()` returns
  without pushing when `!hasData()`, so the web view is never asked to fit
  (0, 0, 0, 0); it keeps its viewport, receives an empty track list through
  `modelReset` -> `onTracksReset()`, and is fitted again by the
  `boundsChanged` that accompanies recovery (0 differs from any real bound, so
  the signal is always emitted, also when the recovered bounds equal the ones
  before the loss). `MapBridge` only forwards. Nothing in `MapWidget`,
  `MapBridge`, `MapDockFeature` or `map.html` changes.

*The testable seam.* `TrackMapModel.cpp`, `MapCursorDotModel.cpp`,
`plotrangemodel.cpp` and `plotutils.cpp` include only Qt Core/Gui and
`flysight_core` headers (`sessionmodel.h`, `momentmodel.h`,
`preferences/preferencesmanager.h`, `plotregistry.h`, `units/unitconverter.h`,
`calculations/timecalculations.h`). They are compiled into the test exactly as
`tst_python_bridge` compiles `pluginhost.cpp`; `CMAKE_AUTOMOC` (global, from
`src/CMakeLists.txt` line 17) generates the meta-objects from the headers next
to them. Nothing moves between targets and `src/CMakeLists.txt` is untouched.

*Fixture.* Pattern: `tests/tst_session_model_engine.cpp` lines 97-140.
- `initTestCase()`: `TestEnvironment::instance().registerBuiltIns()`; the
  `LogbookColumnsVersion` preference and the single stored-data description
  column exactly as in `tst_session_model_engine.cpp` lines 101-107; and the
  three preferences the map models read in their constructors, registered
  locally with `MainWindow`'s literal defaults
  (`PreferenceKeys::MapLargeDotSize` 10, `MapSmallDotSize` 6,
  `MapTrackOpacity` 0.85). They are not core preferences, so they are not
  added to `TestEnvironment::registerCorePreferences()`; an unregistered key
  asserts in `PreferencesManager::getValue`.
- `init()`: fresh logbook, preferences reset, registry snapshot; a
  `SessionModel`; two sessions from `DescentFixture::load("a")` and
  `load("b")`, with `b.setMeasurement("GNSS", "lon", QVector<double>(296, -74.0))`,
  both `setVisible(true)` **before** `mergeSessions({a, b})` (a created row
  takes `created.isVisible()`). Then a `PlotRangeModel` (no range), a
  `MomentModel` with one moment (`positionSource = PositionSource::Attribute`,
  `attributeKey = SessionKeys::ExitTime`,
  `mapPresentation = MapPresentation::LargeDot`; the fixture's exit time is
  calculated and lies inside the track), a `TrackMapModel(&model, &range)` and
  a `MapCursorDotModel(&model, &moments, &range)`.
- `cleanup()`: destroy the two map models, the moment and range models, then
  the `SessionModel`; compare the registry snapshot.
- A helper `correctSource(id, hAccValue)` builds `DescentFixture::load(id)`
  (for "b" with the same longitude edit), sets
  `GNSS/hAcc = QVector<double>(296, hAccValue)`, and calls
  `model.mergeSessions({thatSession})`. `SessionMerge` replaces only the
  differing column, so this is the application's own source-change path:
  `mergeSourceData` -> invalidation -> `publishInvalidation` -> `modelChanged`.
  (The audit's "one import path" rule covers `src` only; tests may call
  `mergeSessions`.)
- The map models rebuild from a zero-interval timer. Wait with
  `QTRY_COMPARE(tracks.count(), n)`; **do not call `rebuild()` in the
  functions that demonstrate recovery or loss**: the signal path is what is
  under test. `rebuild()` may be called only to prove that a rebuild changes
  nothing.

Test functions (expected values are literals; bounds compared with
`qAbs(x - literal) <= 1e-9`; fixture latitudes run from 45.0 to 45.0295):

- `tracksDotsAndBounds` — 2 rows, `hasData()`, 2 dots (one per session id);
  `boundsSouth 45.0`, `boundsNorth 45.0295`, `boundsWest -75.0`,
  `boundsEast -74.0`, `centerLat 45.01475`, `centerLon -74.5`. Each track has
  2 points (the fixture is a straight line) with keys `lat`, `lon`, `t`.
- `noOriginRemovesTrackAndDot` — `correctSource("a", 10.0)`: `QTRY` 1 row,
  whose `SessionIdRole` is `"b"`; 1 dot, for `"b"`; `boundsWest` and
  `boundsEast` are both -74.0 (session a is excluded from the bounds);
  `hasData()` still true. `model.rowAt(row of a).visible` is still true: the
  recording is visible, it just has no track.
- `boundsClearedWhenNoTrack` — spies on `hasDataChanged` and `boundsChanged`;
  `correctSource("a", 10.0)` and `correctSource("b", 10.0)`: `QTRY` 0 rows,
  `!hasData()`, 0 dots, and `centerLat`, `centerLon`, the four bounds all
  exactly 0.0; `hasDataChanged` emitted once; `boundsChanged` emitted at least
  once. An explicit `tracks.rebuild()` afterwards emits neither again.
- `recoversAfterSourceCorrection` — from the state above,
  `correctSource("a", 1.0)`: `QTRY` 1 row (`"a"`), `hasData()`, 1 dot for
  `"a"`, `boundsWest == boundsEast == -75.0`, `boundsNorth 45.0295`;
  `boundsChanged` and `hasDataChanged` were emitted by the recovery. Then
  `correctSource("b", 1.0)`: 2 rows, bounds as in `tracksDotsAndBounds`.
- `hiddenRecordingStaysOutAfterCorrection` — hide `"a"`
  (`setRowsVisibility`), make it bad, correct it: never more than the row of
  `"b"`; showing `"a"` again brings its track back.
- `rangeFilterOnRecoveredTrack` — after a loss and a recovery of `"a"`,
  `range.setRange(SessionKeys::Time, SessionKeys::ExitTime, -0.25, 0.25)`:
  both tracks remain, each with exactly 2 points whose `t` differ by 0.5 and
  straddle the session's exit time by 0.25 s (the boundary interpolation still
  works on the index-retained track); 2 dots.

*Contingency (only if `recoversAfterSourceCorrection` or
`noOriginRemovesTrackAndDot` fails without a manual `rebuild()`).* The
permitted change is, in **both** model constructors, one additional
connection from `SessionModel::dependencyChanged` to `scheduleRebuild()`
filtered to measurement keys of sensor `"Simplified"`
(`key.type == DependencyKey::Type::Measurement && key.measurementKey.first == "Simplified"`),
following `PlotWidget.cpp` line 173. Record in the completion report which
emission was missing. Do not add it speculatively.

**Acceptance Criteria:**
- [ ] `ctest -R tst_map_models` passes on a build whose test target links only
      `flysight_test_support` (check the link line: no Widgets, WebEngine,
      Quick, KDDockWidgets).
- [ ] The diff of `TrackMapModel.*` and `MapCursorDotModel.*` is comments only
      (or comments plus the contingency connection, with the reason reported).
- [ ] `MapWidget.cpp`, `MapBridge.*`, `MapDockFeature.*`, `map.html`,
      `src/CMakeLists.txt` are unchanged.
- [ ] `CalculationRegistry::instance().registeredIds()` is unchanged by every
      function (checked in `cleanup()`).

**Complexity:** M

---

### Task 3.4: Documentation, audit rules, traceability

**Purpose:** Carry over and correct the branch's "Simplified map track"
documentation (spec 4, 10), make "one projection" a permanent invariant, and
record which tests demonstrate the second half of acceptance 3.

**Files to modify:**
- `docs/LOCAL_COORDINATES.md` — append one section (text below).
- `tests/audit/cleanup_audit.cmake` — two rules, one hunk, directly after the
  `"one authority: number formatting"` lines of the "one authority per fact"
  block:
  ```cmake
  # The recording-wide local frame is the only projection: the simplified track
  # (and anything else that needs metres) consumes Local/..., never its own.
  # Allow: a second legitimate user of LocalCartesian is added to the
  # allowed-file regex - after asking why it cannot read Local/... instead.
  expect_only("one authority: local projection" "LocalCartesian"
    "^src/calculations/localcoordinatecalculations\\.cpp$" src)
  expect_none("simplified track: shared frame only" "GeographicLib|boost"
    "src/calculations/simplificationcalculations.*")
  ```
  Do not touch the traceability block (Phase 10 owns its range check).
- `tests/acceptance_map.txt` — append directly under Phase 2's item-103 lines:
  ```
  # 103 (continued) - simplified track on the shared frame; the map without an origin
  103 tst_simplified_track sevenOutputsShareIndices
  103 tst_simplified_track droppedSamplesWithinTolerance
  103 tst_simplified_track duplicatePositionEndpoints
  103 tst_simplified_track closedTrack
  103 tst_simplified_track degenerateTracks
  103 tst_simplified_track emptyTrack
  103 tst_simplified_track nonFiniteSamplesAreSkipped
  103 tst_simplified_track projectionRunsOncePerRecording
  103 tst_simplified_track unavailableWithoutOriginAndRecovers
  103 tst_map_models noOriginRemovesTrackAndDot
  103 tst_map_models boundsClearedWhenNoTrack
  103 tst_map_models recoversAfterSourceCorrection
  ```
  and change the wording of Phase 2's comment "(the simplified-track half is
  added by its own phase)" to "(local coordinates)". The audit requires each
  named function to exist as `::<name>()` in `tests/<target>.cpp`
  (data-driven functions are named without `_data`).
- `tests/README.md` — raise the executable count in section 1 by two (from
  whatever the parallel phases have made it); add a `tst_simplified_track` row
  to the "Built-in calculations and sessions on the engine" table and a
  `tst_map_models` row to "Import and merge, workflows" (one sentence each,
  the latter saying that it compiles the two map models and their helpers
  directly and needs no Widgets or WebEngine); in section 10 add one bullet,
  "anything but `localcoordinatecalculations.cpp` constructs a local
  projection, or the simplified track includes a projection or geometry
  library".

**Not modified:** `docs/CALCULATIONS.md` (nothing in it describes the
simplified track beyond the registration order, which is unchanged),
`docs/DATA_SCHEMA.md` (does not mention calculated sensors by name),
`README.md`, `src/calculations/builtincalculations.h` (see "Decisions Made":
no compatibility bump).

**Technical Approach:**

*`docs/LOCAL_COORDINATES.md`, new last section "Simplified map track".* Start
from the branch's section of the same name and correct it:

- The map draws `Simplified/lat`, `lon`, `hMSL`, `_time`; `Simplified/north`,
  `east`, `down` are the same samples in the local frame. One on-demand
  calculation (`builtin.simplified.track`) produces all seven; its inputs are
  `GNSS/lat`, `lon`, `hMSL`, `_time` and `Local/north`, `east`, `down`.
- Horizontal Ramer-Douglas-Peucker on `Local/north` / `Local/east`, tolerance
  0.5 m, strictly-greater rule. It performs no projection and has no second
  origin; the projection runs once per recording, in the local-coordinates
  calculation.
- It retains sample **indices**: every output is the recorded sample at those
  indices, all seven always have the same length, and distinct samples at the
  same position (a stationary start or end) stay distinct. There is no
  coordinate re-matching.
- **Correct the branch text:** a sample whose local coordinates are not finite
  is *left out* of the simplified track and the path is joined across it; the
  track is not made unavailable by it (the branch made the whole track
  unavailable).
- With no qualifying origin the simplified track is unavailable (missing
  input). The map then shows no track and no cursor dot for that recording and
  leaves it out of its bounds; when no visible recording has a track the
  model's bounds are cleared and the map keeps its current view. There is no
  fallback to an unsimplified or privately projected track: a recording in
  which no fix ever reaches 10 m horizontal accuracy has no track worth
  drawing. Correcting the source brings track, dot and bounds back through
  ordinary invalidation.
- Validation: `tst_simplified_track`, `tst_map_models`, and the golden rows in
  `tests/support/builtinfixture.cpp`. Name no branch-only executable.

**Acceptance Criteria:**
- [ ] `audit_cleanup` passes, and fails when a `LocalCartesian` is put back
      into `simplificationcalculations.cpp` (verify once locally; commit
      nothing for this).
- [ ] Every appended map line names an existing test function.
- [ ] `docs/LOCAL_COORDINATES.md` says "left out", not "unavailable", for
      non-finite samples, and names only tests that exist.

**Complexity:** S

## Testing Requirements

### Unit Tests
- New: `tests/tst_simplified_track.cpp` (Task 3.2), `tests/tst_map_models.cpp`
  (Task 3.3).
- Updated: three golden rows in `tests/support/builtinfixture.cpp`;
  `tst_builtins_engine::simplifiedRunsOnce` (additions only).
- Must pass **unedited** in their simplified-track expectations:
  `tst_builtins_golden`, `tst_builtins_engine` (inventory and counts as Phase 2
  left them), `tst_session_engine` (`multiOutputRunsOnce`,
  `declaredInputChangeRunsOnceMore`: a TIME change still does not invalidate
  `Simplified/lat`, because neither `GNSS/_time` nor `Local/...` depends on the
  time fit), `tst_source_layer`, `tst_workflow`.

### Integration Tests
- The full suite: `ctest --test-dir build/FlySightViewer-build -C Release --output-on-failure -LE python`
  (and the whole suite where Python/NumPy is available). `tst_session_oracle`
  exercises the three new golden names under randomized source replacement and
  merges and asserts `cycleCount() == 0`; `Simplified` now sits one level
  deeper in the graph (`GNSS` -> `Local` -> `Simplified`), which the oracle
  covers without changes.
- `audit_cleanup` (`-L audit`).
- Build: `cmake --build build --config Release` from the repository root.

### Manual Verification
1. Import a TRACK recording: the map shows the track as before; the cursor dot
   follows the plot cursor; zooming a plot trims the track to the range.
2. Import a recording whose start or end is stationary: the track's ends sit
   on the true first and last fixes (hover the ends with the plot cursor at the
   first and last sample).
3. Take a copy of a TRACK file, set every `hAcc` to 10 or more, import it:
   the recording appears in the logbook and its plots work, the map shows no
   track and no dot for it, and the view does not jump to latitude 0 /
   longitude 0. With another recording visible, the map fits that one only.
4. Delete that session, restore the real `hAcc` values in the file, import it
   again: the track, the dot, and the fit return.

## Notes for Implementer

### Gotchas
- **Strictly greater than 0.5 m, first maximum wins.** Compare squared
  distances with `>` against `0.25`, scanning ascending. `>=` changes which
  tracks keep a sample at exactly the tolerance, and "last maximum" changes
  the shape of symmetric tracks; both would make the simplification differ
  from `master`'s.
- **Iterative, not recursive.** See Task 3.1.
- The distance is to the *segment* (clamped `t`), not to the infinite line;
  with coincident span endpoints it is the distance to that point. A closed
  track depends on this.
- Endpoints are the first and last **finite** samples, not samples 0 and
  `n - 1`.
- `GNSS/_time`, not `GNSS/time`, is the declared input (as on `master`).
- Do not add a candidate that falls back to `GNSS/lat`/`lon` when `Local` is
  unavailable, and do not give the map models a fallback read of `GNSS`. The
  spec forbids both.
- A one-point simplified track is an *available* result of length 1; the map
  models ignore it (`n < 2`). Do not turn it into "unavailable" in the
  calculation.
- The audit rules are text searches over `src`: the words `LocalCartesian`,
  `GeographicLib` and `boost` must not appear in
  `simplificationcalculations.cpp/.h`, comments included. Write "its own
  projection" instead.
- In `tst_map_models`, destroy the map models before the `SessionModel`, and
  the `SessionModel` before comparing the registry snapshot (a live model
  schedules a calculation-environment check; `tests/README.md` section 8).
- `TrackMapModel` and `MapCursorDotModel` read `PreferencesManager` in their
  constructors; the three map preferences must be registered before the first
  model is constructed or a debug build asserts.
- Shared files (`tests/CMakeLists.txt`, `tests/acceptance_map.txt`,
  `tests/README.md`, `tests/support/builtinfixture.cpp`,
  `tests/tst_builtins_engine.cpp`, `tests/audit/cleanup_audit.cmake`) are
  edited by parallel phases. Keep each edit to the insertion point named in
  its task, change no neighbouring whitespace, and list every touched path in
  the completion report so the orchestrator can stage by explicit path. This
  phase edits **none** of `src/CMakeLists.txt`, `CMakeLists.txt`,
  `src/calculations/builtincalculations.cpp`, `src/mainwindow.cpp`,
  `docs/CALCULATIONS.md`, `README.md`.
- Report no git state changes; the orchestrator commits.

### Decisions Made
- **Declared inputs:** `GNSS/lat`, `lon`, `hMSL`, `_time`, `Local/north`,
  `east`, `down` — all required, in that order (the branch's order, matching
  the output order). `Local/down` is an input because it is an output source.
  No `hAcc`, no origin attributes: the frame is consumed, not re-derived.
- **Non-finite rule:** a sample is used when `Local/north`, `east` and `down`
  are all finite; nothing else is inspected. This follows the spec ("left out
  ... rather than poisoning it") and deliberately departs from the branch,
  which made the whole track unavailable.
- **Own RDP instead of `boost::geometry::simplify`.** Boost's algorithm
  returns points, which is what forced the re-matching step; retaining indices
  needs the flags. The replacement reproduces Boost's rules (segment distance,
  strict `>`, first maximum). The arithmetic is the branch's.
- **Boost stays linked.** `Boost::boost` (header-only) in `flysight_core`'s
  link line and `find_package(Boost)` become unused by `src` after this phase.
  They are left alone: Phase 1 works in that part of `src/CMakeLists.txt` in
  parallel. (GTSAM is built without Boost and does not need them either.)
  Phase 10 Task 10.1c removes the lookup, the comment "boost::geometry used
  by simplification calculations" (`src/CMakeLists.txt` line 217), the link
  items, `cmake/BoostDiscovery.cmake`, the README prerequisite and the CI
  Boost steps.
- **Calculation id unchanged** (`builtin.simplified.track`): it is not a
  public name, nothing gains from renaming it, and the inventory, run-count
  tests and environment fingerprint stay stable.
- **No further `CalculationCompatibilityVersion` bump** (it stays 2). The
  marker guards cached *logbook column* values. A column is a stored or
  calculated attribute or a measurement interpolated at a marker, and the
  column chooser offers measurements from the registered plots only
  (`src/preferences/addcolumndialog.cpp` line 205); no plot is registered on
  sensor `Simplified`, so no column an existing logbook can hold reads these
  values. Independently, value 2 has not shipped: Phases 2 and 3 land on the
  same branch and the first release that contains either discards every
  released index anyway. No value is reused.
- **Existing golden values do not change.** The descent fixture is a straight
  line along one meridian with strictly increasing latitude, its first row
  qualifies as the origin (so the shared frame equals the old private one),
  and index retention yields `{0, 295}` as the search did. Three rows are
  added for the new outputs, asserting only what is hand-derivable (the
  origin sample is 0 / 0 / 0 and both samples have east 0).
- **The map models need no behavioral change**; the analysis is in Task 3.3.
  They get comment updates and a test. A `dependencyChanged` connection is a
  documented contingency, not part of the plan, because every source-change
  path already ends in `modelChanged` and the spec asks for changes only "as
  needed".
- **"Bounds cleared" means:** `hasData == false` and center and the four
  bounds equal 0.0, with `boundsChanged` emitted; `MapWidget` does not forward
  a fit while `hasData` is false, so the web map keeps its view and is
  re-fitted on recovery.
- **Test seam:** compile `TrackMapModel.cpp`, `MapCursorDotModel.cpp`,
  `plotrangemodel.cpp`, `plotutils.cpp` into `tst_map_models`, following the
  `tst_python_bridge` precedent, rather than moving them into `flysight_core`.
  This touches only `tests/CMakeLists.txt` and leaves Phase 6 free to decide
  where `PlotModel` and friends live.
- **Source correction in the map test goes through `mergeSessions`**, the
  application's only source-change path, and waits on the models' own timers;
  the branch's test mutated the session behind the model's back and called
  `rebuild()` by hand, which would not show recovery.
- **"Projection runs once per recording" is demonstrated twice:** dynamically
  (`runCount("builtin.local.coordinates") == 1` with all `Local` and
  `Simplified` outputs read, and exact equality of `Simplified/north...` with
  the `Local` samples) and statically (the audit rule that only
  `localcoordinatecalculations.cpp` names `LocalCartesian`).
- **Index-selection tests store `Local/...` directly** (stored data wins), as
  the branch's regression did, so expectations are exact literals that do not
  depend on geodesy.

### Open Questions
- None blocking. For Phase 10: the now-unused Boost lookup, comment and link noted above (removed there).

## Definition of Done

This phase is complete when:
1. All tasks have passing acceptance criteria
2. All tests pass (`ctest ... -LE python` at minimum, plus `audit_cleanup`)
3. Code follows patterns established in reference files
   (`derivativehelper.cpp` and `localcoordinatecalculations.cpp` for the
   calculation; `tst_builtins_engine.cpp` and `tst_session_model_engine.cpp`
   for the tests)
4. No TODOs or placeholder code remains
5. The completion report lists every created and modified path, with the
   shared files called out, and states whether the Task 3.3 contingency was
   used
