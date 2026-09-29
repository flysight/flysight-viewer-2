# Phase 3: Derived kinematics

## Purpose

Spec §6, with overview decisions 1, 2, 3 and 4. The fit gains three on-demand
derivations over its published outputs: fused elevation `Fusion/z` and the
fused along-track and cross-track accelerations `Fusion/accAlongTrack` and
`Fusion/accCrossTrack`. Vertical acceleration is the fit's own `Fusion/accD`
and gets no calculation (decision 1). The track-relative arithmetic moves out
of the GNSS calculations into one helper that both registrations call
(decision 3), and the GNSS results do not change.

It is one phase because the extraction and its second caller belong together:
a helper with one caller would be generality nothing uses, and a fused copy of
the GNSS arithmetic would be a second authority. It also introduces the test
executable and the synthetic-output seam that phase 4 extends.

Nothing the user sees changes in this phase. The new measurements get plot
rows in phase 5, and until then the Add Column dialog cannot offer them,
because it lists measurements from the plot registry. `docs/COMPUTED_PLOTS.md`
is therefore not touched.

## Dependencies

- Depends on nothing in code. Blocks phase 4, which extends
  `src/fusion/fusionregistration.*`, `tests/tst_fusion_derived.cpp` and its
  seam, and the two documents, and phase 5, whose plot rows name these
  measurements.
- Phases 1 and 2 are implemented first. At the start of this phase:
  - `Calculations::addConstantDefault` exists.
  - `builtin.default._WIND_N` and `builtin.default._WIND_E` (value `0.0`)
    are registered by `registerAttributeCalculations()`, and the importer no
    longer writes wind.
  - `docs/CALCULATIONS.md` §5 states the rule for defaults.

  This phase does not call the helper. Its tests rely on the wind defaults in
  one place only: a session without stored wind reads the track accelerations
  with zero wind (Tests). Every other track test stores its wind.
- `gnsscalculations.cpp`: phase 2 may edit only its comments on wind. Take
  `windComponent()` as phase 2 leaves it, with today's rule that a stored
  value that is not a number counts as zero.

## What changes

### The shared track-relative helper (`src/calculations/`)

Create a new header-only file, `src/calculations/trackhelper.h`, and follow
the pattern of `timefithelper.h`:

- Namespace `FlySight::Calculations`, with `inline` functions.
- A comment that states the contract and names its users, as `anglehelper.h`
  and `timefithelper.h` do.
- Listed in `flysight_core`'s sources in `src/CMakeLists.txt`, next to
  `calculations/timefithelper.h`.

It must be header-only because `flysight_fusion` links only `flysight_model`,
not `flysight_core` (see the comment of `timefithelper.h`, "Header-only, so
that a library that does not link the built-in calculations can share it").

The header owns two facts, which are today spelled out in
`gnsscalculations.cpp`:

1. **The wind rule.** A wind component whose value is not a number counts as
   zero. This is today's `windComponent()`. Every GNSS caller moves onto the
   helper's version: `wcVel`, `wcVelH`, the two track accelerations and
   `computeAeroCoefficient()`. The file-local copy is deleted, because one
   rule may not have two copies. The helper takes the attribute's value (a
   `QVariant`) or the context; the implementer chooses.
2. **Along-track and cross-track acceleration** of an acceleration vector
   relative to the wind-corrected velocity, sample by sample:
   - `wc = (velN − windN, velE − windE, velD)`, and `|wc|` is its norm.
   - When `|wc| < 1e-9`, along-track is `0`. Otherwise the unit vector is
     `u = wc / |wc|` (three divisions), and along-track is
     `accN·uN + accE·uE + accD·uD`.
   - Cross-track is `sqrt(max(0, |a|² − along²))`, with `|a|²` computed as
     `accN·accN + accE·accE + accD·accD`.
   - The result is nothing when any of the six arrays is empty or their
     lengths differ.

   These are the same IEEE operations in the same order as today's
   `accAlongTrack` and `accCrossTrack` lambdas. Today's cross-track computes
   `alongTrack = -dot` before squaring it, and the negation does not survive
   the move: `(−d)·(−d)` and `d·d` are the same double, so the result is
   bit-identical. The implementer shapes the API (two functions, or one that
   returns both). The GNSS registrations keep two calculations and call the
   helper.

Constraints on the helper file:

- It does not log.
- It includes no engine type other than what the wind rule needs.
- It contains neither `fusion/` nor `Fusion::`, in code or in comments. The
  audit rule "nobody but the application references the fusion library"
  searches `src/calculations` for both. Write "the sensor fusion's track
  accelerations", as `timefithelper.h` writes "the sensor fusion's
  trajectory".

What must not change in `gnsscalculations.cpp`:

- the ids, inputs, outputs and order of every GNSS registration, including
  `trackInputs`;
- GNSS `z` and its warnings;
- the lift and drag coefficients. Their projection uses gravity-corrected
  acceleration and gives NaN below the speed threshold. That is a different
  quantity, so it stays where it is and only its wind reads move to the
  helper (Decisions).

### The three derivations (`src/fusion/fusionregistration.cpp`)

Follow `registerHorizontalAcceleration` exactly:

- a `CalculationDescriptor` with the default `OnDemand` policy, no title and
  no `resultVersion`;
- inputs that exist only once the fit has published;
- one measurement output under `kSensor`;
- registration through `Calculations::addCalculation`.

The compute functions keep no state and do not log: the audit rule "the kernel
does not log" covers all of `src/fusion`, this file included. Every output has
the length of its inputs, and so the length of `Fusion/_time` (the time-axis
rule of `docs/CALCULATIONS.md` §16.1).

| Id | Inputs, in this order | Output | Value |
| --- | --- | --- | --- |
| `builtin.fusion.z` | `Fusion/down`, `_LOCAL_ORIGIN_HMSL`, `_GROUND_ELEV` | `Fusion/z` | `z[i] = (originHmsl − down[i]) − groundElev` (decision 2). Unavailable when either attribute is not a number (`toDouble(&ok)`) or when `down` is empty |
| `builtin.fusion.accAlongTrack` | `Fusion/accN`, `accE`, `accD`, `velN`, `velE`, `velD`, `_WIND_N`, `_WIND_E` | `Fusion/accAlongTrack` | the helper's along-track, wind read by the helper's rule |
| `builtin.fusion.accCrossTrack` | the same eight | `Fusion/accCrossTrack` | the helper's cross-track |

- The two track calculations declare the same inputs as GNSS `trackInputs`,
  in the same order, with the sensor `Fusion`.
- The origin-height and ground-elevation keys are `SessionKeys::LocalOriginHmsl`
  and `SessionKeys::GroundElev`, and the wind keys are `SessionKeys::WindN`
  and `WindE`.
- `fusionregistration.cpp` includes `calculations/trackhelper.h` the way it
  already includes `calculations/timefithelper.h`.

`Fusion::registerFusionCalculations()` registers them after
`builtin.fusion.systemTime`, in the order z, accAlongTrack, accCrossTrack
(Decisions). The following stay untouched (decision 17): `registerFit`,
`kFitOutputs`, `kFitInputs`, `fitInputs()`, `channelsFrom()`,
`fitOutputChannels()`, `Fusion::Algorithm`, every kernel file, and
`tests/data/fusion/`.

The header comment of `src/fusion/fusionregistration.h` ("Register sensor
fusion with the calculation engine:") gains one bullet for each of the three
calculations, in registration order, in the style of the `accH` bullet. The
header also states that vertical acceleration is `Fusion/accD` itself.

### Vertical acceleration

This needs no code. `Fusion/accD` is kinematic acceleration in NED, positive
down, which is the same convention as `GNSS/accD` (derivative of `velD`). The
documents say so.

### Build and confinement

The new test must be added to three places:

- **`tests/CMakeLists.txt`:** in the FUSION block, as
  `flysight_add_fusion_test(tst_fusion_derived SOURCES tst_fusion_derived.cpp
  LIBS flysight_fusion_session_support)`, with a one-line comment in the
  block's style. It must be a fusion test even though it never runs the
  solver: it links `flysight_fusion`, so it needs the solver runtime on the
  path and belongs where "the only place a test may link GTSAM" says.
- **`cmake/SolverDependencies.cmake`:** in `_FLYSIGHT_GTSAM_REACHERS`, next to
  `tst_fusion_session`. Without this, `flysight_assert_solver_confinement()`
  fails at configure time.
- **`tests/README.md`:** in its section 1 row (decision 16).

## Interfaces

Phase 3 provides:

- The ids and names `builtin.fusion.z` → `Fusion/z`,
  `builtin.fusion.accAlongTrack` → `Fusion/accAlongTrack` and
  `builtin.fusion.accCrossTrack` → `Fusion/accCrossTrack`, with the inputs
  above and registered after `builtin.fusion.systemTime`. Vertical
  acceleration is `Fusion/accD` (decision 1). Phase 5's rows name these five
  measurements together with `Fusion/accH`.
- `tests/tst_fusion_derived.cpp`, a fusion test in the FUSION block linking
  `flysight_fusion_session_support`. Phase 4 adds its attitude tests to it.
- **The seam**, declared in `tests/fusion/fusionsessions.h` and defined in
  `fusionsessions.cpp`:

  ```cpp
  FlySight::SessionData syntheticFitSession(const QString &sessionId,
                                            const QHash<QString, QVector<double>> &channels);
  ```

  The contract, which goes in the declaration's comment:
  - Each entry becomes source data `Fusion/<name>`, and only the seventeen
    names of `fusionMeasurementNames()` are allowed (`Q_ASSERT`).
  - Each is stored with the unit text of that output: `_time` "s";
    `north`/`east`/`down` "m"; `velN`/`velE`/`velD` "m/s";
    `accN`/`accE`/`accD` "m/s^2"; `roll`/`pitch`/`yaw` "deg"; `qx`..`qw` "".
    The conversion layer passes all of these through unchanged
    (`src/units/unitconversion.h`; no schema row names `Fusion`).
  - A reader of `Fusion/<name>` therefore gets exactly these samples, and
    resolution never reaches `builtin.fusion.fit`. This is step 2 of the
    resolution in `docs/CALCULATIONS.md` §5: "a measurement with source data:
    the source conversions ...", and `CalculationEngine` never falls through
    from source data to a derived candidate.
  - The session also carries the identity attributes of the fixture sessions
    (`SESSION_ID`, `DEVICE_ID`, `SCHEMA_VER = 2`) and nothing else. The test
    stores whatever attributes a derivation reads with `setAttribute`:
    `_LOCAL_ORIGIN_HMSL`, `_GROUND_ELEV` and the wind here, `_ORIENTATION` in
    phase 4.
  - It is a `SessionData` for engine-level tests. It never goes into a
    `SessionModel`: its fit outputs are data, and the saver would write them
    into a session file.

  Phase 4 calls it with `qx`, `qy`, `qz`, `qw` (and `_time` if it wants one),
  and asserts that the fit's `runCount` stays 0 when an orientation edit
  recomputes the angles.
- `docs/SENSOR_FUSION.md` §4 and `docs/CALCULATIONS.md` §17 as below. Phase 4
  adds the attitude beside the derived kinematics.

Phase 3 consumes phase 2's wind defaults, used only by the test that has no
stored wind.

## Acceptance criteria

1. **Registration** (spec §6, §12 "fusion registration"):
   - `builtin.fusion.z`, `builtin.fusion.accAlongTrack` and
     `builtin.fusion.accCrossTrack` are registered.
   - Each is `OnDemand`, has an empty `resultVersion`, has exactly the inputs
     of the table in that order, and has one output, the measurement named in
     the table.
   - Each output has exactly one candidate.
   - `registeredIds()` ends with `builtin.fusion.fit`, `builtin.fusion.accH`,
     `builtin.fusion.systemTime`, `builtin.fusion.z`,
     `builtin.fusion.accAlongTrack`, `builtin.fusion.accCrossTrack`.
2. **They wait on the fit and start nothing** (spec §6 "appears with the fit
   and never starts one", §12 "demand layer untouched"):
   - For each of the three output names,
     `CalculationRegistry::explicitDependencies()` is exactly
     `{builtin.fusion.fit}`.
   - Take a fixture session with the fit's inputs and a stored
     `_GROUND_ELEV`, and no fit. For each of the three names:
     `engine.blockers(name)` is `Blocked` with the fit as its one blocker,
     `CalculationDemand::isMerelyUncomputed` is true, and a read returns
     empty.
   - After all of that, `runCount(builtin.fusion.fit)` is 0.
3. **Elevation** (spec §6, §13; decision 2):
   - `Fusion/z[i] == (_LOCAL_ORIGIN_HMSL − Fusion/down[i]) − _GROUND_ELEV`,
     compared with `QCOMPARE` on exactly representable values.
   - Editing `_GROUND_ELEV` or `_LOCAL_ORIGIN_HMSL` recomputes `Fusion/z`
     without the fit.
   - `Fusion/z` is unavailable when a stored `_GROUND_ELEV` or
     `_LOCAL_ORIGIN_HMSL` is not a number.
4. **Track accelerations, known answers** (spec §6, §13), with stored wind.
   Every case is exact:
   - velocity along north, acceleration (3, 4, 0): along 3, cross 4;
   - velocity reversed: along −3, cross 4;
   - a vertical descent with vertical acceleration: along equals `accD`,
     cross 0;
   - a stored wind that turns a skewed ground velocity into a northward air
     velocity gives the northward answer;
   - below 1e-9 m/s of wind-corrected speed: along 0, cross `|a|`;
   - a stored wind that is not a number reads as zero;
   - with no stored wind, the result is the same as with zero wind (phase 2's
     defaults);
   - arrays of unequal length make both unavailable.
5. **One definition** (decision 3):
   - On one session with the same samples stored as `GNSS/accN..velD` and
     `Fusion/accN..velD`, and the same wind, `GNSS/accAlongTrack` and
     `Fusion/accAlongTrack` agree at every sample, and so do the cross-track
     pair. The comparison is `sameRecomputedValue()` (bit-exact in exact
     mode, within 4 ulp otherwise), on vectors without near-cancellation in
     cross-track.
   - `gnsscalculations.cpp` has no along-track or cross-track loop of its own
     and no `windComponent` of its own.
6. **GNSS unchanged** (decision 3):
   - `tst_builtins_golden`, `tst_builtins_engine` and `tst_session_oracle`
     pass unmodified.
   - The helper performs the operations listed under "What changes" in their
     current order, which a reviewer checks by reading the diff.
7. **The fit untouched** (spec §12, §13 "the fit is unchanged"; decision 17):
   - No file in the decision-17 list is modified, and `Fusion::Algorithm` is
     `batch-temperature-bias-v3`.
   - `tst_fusion_golden`, `tst_fusion_kernel`, `tst_fusion_session`,
     `tst_fusion_jobs`, `tst_fusion_rows`, `tst_fusion_store`,
     `tst_fusion_runner` and their `_exact` runs pass. The only amended body
     is `tst_fusion_session::registrationShape` (see Tests).
8. **Vertical acceleration** (decision 1): nothing registers a second output
   for vertical acceleration. `Fusion/accD` keeps its one candidate, which
   `tst_fusion_session::explicitOutputsHaveOneCandidate` already asserts.
9. **Audit and confinement:**
   - `audit_cleanup` is green without any rule loosened.
   - The configure-time confinement check passes with `tst_fusion_derived`
     among the reachers.
10. **Documentation** (CLAUDE.md, spec §14): the documents below describe the
    derivations as implemented.

## Tests

### New: `tests/tst_fusion_derived.cpp`

This executable never runs the solver. Use the fixture and harness pattern of
`tst_fusion_session`:

- `TestEnvironment::instance().registerBuiltIns()` and then
  `registerFusionOnce()` in `initTestCase()`;
- `cleanup()` compares `registeredIds()` with the value taken in `init()` and
  checks `enrolledEngineCount() == 0`;
- `testmain.h`.

The function names below are the ones phase 6 cites. Keep them.

- `syntheticOutputsAreServedWithoutAFit`: the seam's premise. Every stored
  `Fusion/<name>` reads back bit-identical (`sameBitsEverywhere`), and the
  fit's `runCount` is 0 with no `resultStatus`.
- `derivedRegistrationShape`: criteria 1 and the registry half of 2.
- `derivedValuesWaitOnTheFit`: the session half of criterion 2, on
  `fixtureSession("coarse_linear")` plus a stored `_GROUND_ELEV`. Use a
  `SessionData` only; no `SessionModel` and no executor.
- `elevationIsOriginHeightMinusDownAboveGround`: criterion 3.
- `trackAccelerationsKnownAnswers`: criterion 4.
- `trackAccelerationsAreTheGnssDefinitions`: criterion 5. GNSS source data is
  stored under `GNSS/...` (units "m/s" and "m/s^2"), and stored data wins
  over `builtin.gnss.accN`.

"Creates no job on its own" needs no new real-fit test. The generic half is
already proven with a real fit by `tst_fusion_rows::accHRowIsBlockedByFusion`:

- an on-demand calculation is refused as a job
  (`!m_queue->offer("s2", kAccH).created()`);
- the demand layer sees through it to the fit.

The demand layer offers only what `explicitDependencies()` returns for a
checked plot, so the specific half of criterion 2 is enough for the new
names: the fit is their only explicit dependency, and inspection reports them
blocked by it. The same rows in phase 5 are then exercised by the plot-list
tests over the eight plots.

### Amended

- `tst_fusion_session::registrationShape`: the tail of `registeredIds()` is
  the six ids of criterion 1. The function name is unchanged, because item
  221 of `tests/acceptance_map.txt` cites it. The detailed checks of the
  three new descriptors live in `derivedRegistrationShape`, not here.
- `tests/fusion/fusionsessions.h`, `.cpp`: add `syntheticFitSession()`, and
  add `tst_fusion_derived` to the header's list of users.
- `fusionNames()` and `fusionPlots()` are **not** extended (Decisions).

### Audit and acceptance map

- **No audit rule is added or changed.** The change must satisfy these rules:
  - `solver-confinement`: no GTSAM or Eigen in `fusionregistration.*`;
    "nobody but the application references the fusion library" for
    `src/calculations`; "the kernel does not log" for `src/fusion`.
  - "pure compute functions" and "only the conversion layer reads the source
    layer": the test stores source data through `setSourceMeasurement` and
    declares no source input.
  - `fusion-tooling`: the new test includes no internal fusion header.
  - The GTSAM-header rule: the new test includes no `<gtsam/`.
  - `naming`: the plot counts are untouched, since no plot row is added.
  - `fusion-model`: `SENSOR_FUSION.md` text avoids its banned phrases, for
    example "frozen" and "twenty-one".
- **Acceptance map:** no new line (decision 15). No cited function is renamed
  or removed.

### `tests/README.md`

- **Section 1:**
  - add a row for `tst_fusion_derived`, placed after `tst_fusion_session`,
    saying what it covers and "Label `fusion`";
  - change the counts to 49 executables, 50 `ctest -N` entries, and 57 with
    the exact runs (the new test compares with no golden, so it has no
    `_exact` run);
  - in the `tst_fusion_session` row, "three registrations" becomes six.
- **Section 3:** add `tst_fusion_derived` to the list of GTSAM-linked tests
  in the `FLYSIGHT_BUILD_FUSION_TESTS` row.
- **Section 11, "Fusion sessions":** name `tst_fusion_derived` among the
  tests, and describe `syntheticFitSession()` as the opposite premise to the
  fixture sessions: the fit's outputs stored as data so that what is derived
  from them can be tested without the solver.

### Documentation

**`docs/SENSOR_FUSION.md` §4**

- After the outputs table, the paragraph "Two values are derived from the
  outputs on demand ..." becomes a short table of the derived measurements,
  each with its meaning:
  - `accH`;
  - `_system_time`;
  - `z`: height above the ground elevation, `_LOCAL_ORIGIN_HMSL − down −
    _GROUND_ELEV`, metres;
  - `accAlongTrack` and `accCrossTrack`: the GNSS definitions, wind-corrected,
    on the fused velocity and acceleration.
- Then a sentence that vertical acceleration is `accD` itself, positive down
  like `GNSS/accD`.
- One sentence on why `z` and `GNSS/z` can differ away from the origin. Both
  use the same ground elevation, but `down` is measured along the origin's
  vertical, so a point at constant height lies lower in the frame by about
  d²/2R: some 0.08 m at 1 km, 2 m at 5 km (`LOCAL_COORDINATES.md` §7).

**`docs/SENSOR_FUSION.md` §8**

- The validation table gains a `tst_fusion_derived` row.
- The sentence "CTest runs each of these seven tests a second time" (the
  exact runs) stays true: say which tests have an exact run, every one but
  `tst_fusion_derived`, which compares with no golden.

**`docs/CALCULATIONS.md` §17**

- "Three calculations are registered" becomes six, and the table gains the
  three rows of this document.
- "**Derived values.**" gains the definition of `Fusion/z` and of the two
  track accelerations: the helper, `src/calculations/trackhelper.h`,
  header-only for the reason given for `timefithelper.h`, shared with
  `builtin.gnss.accAlongTrack` / `accCrossTrack`. It also states that
  vertical acceleration is `Fusion/accD`.
- The sentence "Both are on demand, but their inputs exist only once the fit
  has published ..." covers all five derived values.
- The test paragraph at the end names `tests/tst_fusion_derived.cpp`.
- "**Plots.**" is left to phase 5.

## Decisions

- **The helper file and what it owns.** It is `src/calculations/trackhelper.h`
  and header-only, because the fusion library does not link the built-in
  calculations.
  - It owns the wind rule as well as the projection. Otherwise the fused
    calculations would either spell "not a number counts as zero" a second
    time or call into a file-local function of `gnsscalculations.cpp`.
  - Every GNSS reader of wind moves to it.
  - The lift and drag coefficients keep their own projection. It projects the
    gravity-corrected acceleration and gives NaN, not zero, below the
    threshold, which is a different quantity that spec §3 does not touch.
    Moving it would put their goldens at risk for no stated gain.
- **Registration order.** The three follow `builtin.fusion.systemTime`, so
  the existing tail keeps its order and phase 4 appends after them. Each name
  has one candidate, so the order decides nothing but the registry listing
  that `registrationShape` pins.
- **`Fusion/z` does not log.** GNSS `z` warns on a missing ground elevation,
  but `src/fusion` may not log (audit). An unavailable value is reported by
  the engine, as it is for `accH`.
- **The seam is stored source data, not a private registry.** The
  alternative was a private registry with `FakeSessionState`, as in
  `tst_builtins_engine`. The seam instead runs the real registration on real
  `SessionData` engines bound to the global registry, the same objects as
  `tst_fusion_session`. That lets phase 4 prove "recomputes without a fit"
  through ordinary invalidation of a stored attribute. The seam lives in
  `fusionsessions.*` because that header already documents the
  "stored data always wins" premise for the fixture sessions.
- **`fusionNames()` stays as it is.** `tst_fusion_session`, `tst_fusion_jobs`
  and `tst_fusion_store` assert that every name in it is available after a
  fit. Fixture sessions have no `GNSS/hMSL`, so they have no ground elevation
  and no `Fusion/z`. Adding the new names would turn every such assertion
  into a special case. The new names are tested in their own executable.
- **No real-fit test for "appears with the fit".** The mechanism (an
  on-demand calculation over fit outputs is blocked by the fit, appears
  through invalidation, and is never a job) is proven with a real fit for
  `accH`. The new registrations differ only in their arithmetic, which the
  synthetic tests cover exactly.
- **No audit rule.** A text rule for "one definition" of the projection
  cannot tell the helper's arithmetic from the aerodynamic coefficients'.
  Criterion 5 guards the definition as a test, and criterion 1's candidate
  count guards the names.
- **The cross-platform comparison.** `flysight_fusion` is compiled with
  `-ffp-contract=off` on GCC and Clang and `flysight_core` is not. The same
  inline helper may therefore round differently in the two libraries on a
  contracting compiler. Hence `sameRecomputedValue()` in criterion 5, and
  exactly representable known answers in criteria 3 and 4.

Ready, with one caveat: `trackAccelerationsKnownAnswers`'s no-stored-wind case
assumes phase 2's constant wind defaults are in place, as the implementation
order 1, 2, 3 guarantees.
