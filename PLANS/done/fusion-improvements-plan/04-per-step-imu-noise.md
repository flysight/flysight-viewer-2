# Phase 4: Per-step IMU noise term

## Overview

This phase adds the per-step white-noise term of spec section 5 to the IMU preintegration: before every `integrateMeasurement` call the shared preintegration parameters receive the covariance `(density^2 + sigma^2 x dt) I` for that step, where `sigma` is a tuning slope times the step length times the norm of the change of the interpolated signal across the step. The two slopes become `Tuning` fields (`gyroStepSlope = .026`, `accStepSlope = .40`), are validated, and are reported in the diagnostics under `model.per_step`. Unit tests hold the covariance to expectations stated independently of the implementation, and the phase ends with the golden re-capture of Phase 1, for which `coarse_linear` (zero signal change) must reproduce its numbers byte for byte.

## Dependencies

- **Depends on:** Phase 1 (golden regression harness: `fusion_golden_capture`, `tst_fusion_golden`, the re-capture recipe quoted in Task 4.6). Phase 3 (stopping rule) is implemented before this phase; nothing here depends on its content, but its files are read as they are after it.
- **Blocks:** Phase 5 (segment and prefix fits use the production graph, which includes this term).
- **Assumptions:**
  - The working tree is the plan branch with Phases 1 and 3 committed: `fusion_golden_capture` exists, the goldens under `tests/data/fusion/` are Phase 3's capture, `tests/README.md` section 11 has the "Re-capture procedure" subsection, and the `algorithm` string is `batch-shared-bias-v2`.
  - Phase 3 changed `src/fusion/factorgraphfit.cpp`, `src/fusion/fusion.cpp`, `src/fusion/fusionpipeline.h`, the `stopping` / `quality` objects in `src/fusion/fusionoutput.cpp` and added stopping-rule fields to `Tuning`. This phase does not touch any of that; line numbers below are those of `master` at `1f3139a` and may have shifted by Phase 3's edits (the named functions have not moved).
  - The configured test build is `build-phase1/` on the capture machine (64-bit MSVC 19.44, Release, GTSAM from `build-solver-deps/GTSAM-install`). `cmake --build build-phase1 --config Release`; never rebuild `build/`.
  - GTSAM on this machine is 4.3 with `GTSAM_TANGENT_PREINTEGRATION` defined (`build-solver-deps/GTSAM-install/include/gtsam/config.h` line 90). `PreintegratedImuMeasurements::integrateMeasurement` reads `p().accelerometerCovariance` and `p().gyroscopeCovariance` on every call (`third-party/gtsam/gtsam/navigation/ImuFactor.cpp` lines 72-84), so assigning the shared params before each call is the per-step covariance, as the spec's implementation constraint says. `ImuFactor` takes its noise model from `pim.preintMeasCov_` at construction (line 118), never from the params afterwards.

## Tasks

### Task 4.1: The two slopes in `Tuning`, with validation

**Purpose:** The constants are tuning values with the spec's defaults, so that a test can set them (including zero, which is exactly today's covariance) and the diagnostics can report them.

**Files to modify:**
- `src/fusion/fusionsamples.h` — two fields in `Tuning` (lines 34-40)
- `src/fusion/fusionsamples.cpp` — `requireValidTuning()` (lines 52-61) admits zero and rejects negative or non-finite slopes

**Technical Approach:**

In `struct Tuning` (`fusionsamples.h` lines 34-40), after `accBiasSigma, gyroBiasSigma` and before `maxGap`, add one line in the existing style:

`double accStepSlope = .40, gyroStepSlope = .026;` with a `///<` comment and a short block comment above it stating the units exactly as the spec does: per integration step of length `dt` (s), `sigma_w = gyroStepSlope x dt x |delta omega|` (radians; `|delta omega|` the norm of the change of the interpolated rate across the step, rad/s) and `sigma_a = accStepSlope x dt x |delta f|` (m/s; `|delta f|` the change of the specific force, m/s^2); the step's covariance is `(density^2 + sigma^2 x dt) I`; zero disables the term and gives exactly the density covariance. The order of the two fields in the declaration is free; the values are binding (`.40` and `.026`, overview "Fit contract evolution"). The struct comment ("The defaults are the model; only maxGap is derived from the recording ... and only a test changes anything else") stays true and stays as it is.

In `requireValidTuning()` (`fusionsamples.cpp` lines 52-61) the existing loop rejects `<= 0`; the slopes may be zero, so they cannot join it. Add a second loop over `{ tuning.accStepSlope, tuning.gyroStepSlope }` that throws `std::invalid_argument("Invalid fusion configuration")` (the same text as the first loop; no new rejection wording) when a value is not finite or is negative. Phase 3 may have added its own stopping-rule fields to this function; add the slope loop beside whatever is there without reordering the existing checks (the first violation is the reported reason, and the order of the loops within `requireValidTuning` only matters for tests that make two fields invalid at once, which none does).

Nothing else reads `Tuning` in a way that needs changing: `planFit()` copies `baseTuning` whole (`fusion.cpp` line 49) and only overwrites `maxGap`, so a caller's slopes reach the fit and the diagnostics untouched.

**Acceptance Criteria:**
- [ ] `Tuning{}.gyroStepSlope == .026` and `Tuning{}.accStepSlope == .40` (asserted in the diagnostics test of Task 4.4)
- [ ] `validateSamples()` throws `std::invalid_argument` for a negative slope and for a NaN slope, and accepts both slopes at zero (Task 4.4 extends `validationRejectsEachDefect`)
- [ ] The rejection text for an invalid slope is `Invalid fusion configuration` (unchanged wording)
- [ ] The `Tuning` comment states the units and the zero-disables rule in the spec's terms

**Complexity:** S

---

### Task 4.2: The per-step covariance in `preintegrateImu()`

**Purpose:** This is the numerical change of the phase: every integration step gets its own gyroscope and accelerometer covariance, set on the shared parameters immediately before its `integrateMeasurement` call, with the step boundaries, the midpoint sampling, the densities and the integration covariance unchanged.

**Files to modify:**
- `src/fusion/imuintegration.cpp` — `preintegrateImu()` (lines 92-108); `preintegrationParams()` (82-90) unchanged
- `src/fusion/imuintegration.h` — the file comment (lines 12-15) and the doc comment of `preintegrateImu()` (lines 38-42) state the term

**Technical Approach:**

Keep `preintegrationParams()` exactly as it is: it still builds the params with the density-only covariances (`I * density^2`, lines 86-87) and `integrationCovariance = I * 1e-8` (line 88), and its declaration and doc comment stay. The behavioural reference is the lab's `preintegrateImu()` in `experiments/fusion_lab/fusion/imuintegration.cpp` lines 92-110 (read-only; the algorithm, not the style, and note that the lab's sigma is `k x |change|` without the `dt` factor, with different constants: at the lab's 0.0756 s step the spec's slopes equal the lab's `k_w = 2.0e-3` and `k_a = 0.030`).

In `preintegrateImu()`:

1. Hold the params in a local `std::shared_ptr<gtsam::PreintegrationParams>` (today line 96 passes the temporary straight into the `pim` constructor; keep the pointer so the loop can write through it — `pim.params()` also returns the same shared pointer, `PreintegrationBase.h` line 91, but the local is clearer).
2. The loop over the edges `e` (lines 98-102) keeps its midpoint sampling and its single `integrateMeasurement` call. Before that call, for the step `[e[i-1], e[i]]` with `dt = e[i]-e[i-1]`:
   - the signal change is the difference between the interpolated values at the step's **end** and **start** (`interpolateAt(samples.imuTime, samples.gyro, e[i]) - interpolateAt(..., e[i-1])`, likewise for `samples.force`), per spec section 5 — not the midpoint values, and not raw sample differences (a step that ends at a GNSS time ends at an interpolated value). Interpolate each edge once: carry the previous step's end values into the next step's start (a two-vector pair updated at the end of each iteration; the first pair is the interpolation at `e[0]`);
   - `sigmaW = tuning.gyroStepSlope * dt * deltaGyro.norm()` and `sigmaA = tuning.accStepSlope * dt * deltaForce.norm()`;
   - `params->gyroscopeCovariance = gtsam::I_3x3 * (tuning.gyroDensity*tuning.gyroDensity + sigmaW*sigmaW*dt)` and `params->accelerometerCovariance = gtsam::I_3x3 * (tuning.accDensity*tuning.accDensity + sigmaA*sigmaA*dt)`; `integrationCovariance` is never written in the loop.
3. The duration check (lines 105-106) stays.

Two file-local helpers in the anonymous namespace (next to `gyroIncrements()`, lines 27-36) keep the loop readable, e.g. `double stepSigma(double slope, double dt, const gtsam::Vector3 &change)` and `gtsam::Matrix3 stepCovariance(double density, double sigma, double dt)`; names are the implementer's, the expressions above are the spec.

No `if (slope > 0)` branch: with a slope of zero or a zero change, `sigma` is exactly `0.0`, `sigma*sigma*dt` is exactly `0.0`, `density*density + 0.0` is exactly `density*density`, and `I_3x3 * (density*density)` has the same elements as today's `I_3x3*density*density` (`1*x == x`). So the density-only covariance is reproduced bit for bit without a special case; Task 4.4's first test and the `coarse_linear` criterion of Task 4.6 are the proof. Phase 6's per-interval bias does not touch this loop.

After the loop the params object holds the last step's covariance. Nothing reads it: the factor's noise model is built from `preintMeasCov()` (`ImuFactor.cpp` line 118), and the params are created per call. Do not restore the densities and do not read `pim.params()->gyroscopeCovariance` anywhere expecting the density; say so in a one-line comment.

Header (`imuintegration.h`): extend the file comment (lines 12-15) by one sentence — each step's measurement covariance is the density plus a white-noise term proportional to the change of the signal across the step (`Tuning::gyroStepSlope`, `accStepSlope`) — and the `preintegrateImu()` doc comment (38-42) by "with the per-step noise term of `tuning`". `gyroIncrement()`, `propagateAttitude()`, `interpolateAt()` and `integrationEdges()` are unchanged; their users in `src/fusion/initializer.cpp` (lines 81, 98, 147) and `src/fusion/trajectoryreconstruction.cpp` (line 28) do not preintegrate and are unaffected. `addImuFactor()` and `buildFactorGraph()` in `factorgraphfit.cpp` (lines 43-47, 117-131) call `preintegrateImu()` with the fit's `Tuning` already; they are not edited.

**Acceptance Criteria:**
- [ ] `preintegrateImu()` assigns `params->gyroscopeCovariance` and `params->accelerometerCovariance` before every `integrateMeasurement` call, from the interpolated values at the step's end and start, with the expressions above; `preintegrationParams()` is unchanged (`git diff` shows no change to lines 82-90)
- [ ] `integrationEdges()`, midpoint sampling, the duration check and `kIntegrationVariance` are unchanged
- [ ] Task 4.4's three covariance tests pass in `tst_fusion_kernel`
- [ ] Existing kernel tests that preintegrate a zero-change signal (`preintegrationHonoursExactBoundaries`, `headingIsUnconstrained`, `exactConstantVelocityFit`) pass unchanged
- [ ] No `#include` is added outside `src/fusion/`; `flysight_assert_solver_confinement()` still prints `GTSAM link confinement: OK` and `ctest -L audit` passes

**Complexity:** M

---

### Task 4.3: `model.per_step` in the diagnostics

**Purpose:** The constants the fit ran with are part of the account of the fit (spec section 7); the key layout is the overview's binding "Diagnostics key layout", and Phase 6 adds `model.gyro_bias` beside `per_step`.

**Files to modify:**
- `src/fusion/fusionoutput.cpp` — a `modelSummary()` helper next to `seedSummary()` (lines 27-39); one entry in `successDiagnostics()` (lines 84-110)
- `src/fusion/fusionoutput.h` — `successDiagnostics()` gains a `const Tuning &` parameter (lines 25-27), unless Phase 3 already added one (see below)
- `src/fusion/fusion.cpp` — the single call site in `fitAndAssemble()` (lines 78-79) passes `plan.tuning`; one argument added, nothing else in the file changes

**Technical Approach:**

`successDiagnostics()` does not receive the `Tuning` today; the tuning is in scope only in `fitAndAssemble()` (`plan.tuning`, `fusion.cpp` lines 61-81). The smallest route is a parameter:

- If Phase 3 already routed the `Tuning` into `successDiagnostics()` (likely: its `stopping` object reports `bias_settled_tolerance` and the slow-tail thresholds, which are `Tuning` fields after Phase 3), use that parameter and change nothing in `fusion.cpp` or the header signature.
- Otherwise add `const Tuning &tuning` as the last parameter of `successDiagnostics()` in `fusionoutput.h` and `.cpp`, and add `plan.tuning` as the last argument at the one call site (`fusion.cpp` line 79). This is the only edit this phase makes to a Phase 3 file, and it is one argument; record which of the two cases applied in the phase report. Do not report the constants from a default-constructed `Tuning{}`: a test that overrides the slopes must see its own values (Task 4.4's diagnostics test checks exactly that).

In `fusionoutput.cpp`, add in the anonymous namespace, after `seedSummary()`:

`QJsonObject modelSummary(const Tuning &tuning)` returning `QJsonObject{{"per_step", QJsonObject{{"gyro_slope_s", tuning.gyroStepSlope}, {"acc_slope_s", tuning.accStepSlope}}}}` with a doc comment "The model constants of this fit that are not fitted quantities; Phase 6 adds the fitted gyro bias model beside `per_step`". Adding `gyro_bias` later is one more `{"gyro_bias", ...}` entry in that initializer. In `successDiagnostics()` add `{"model", modelSummary(tuning)}` to the initializer list (position is irrelevant: `QJsonObject` serializes with sorted keys, so `model` lands between `max_seed_vs_selected_angle_deg` and `objective` in the goldens). The key names `model`, `per_step`, `gyro_slope_s`, `acc_slope_s` are binding (overview, "Diagnostics key layout"); do not rename or add to them. `failureDiagnostics()` stays `{algorithm, failure}` (plus whatever Phase 3 added for a solver failure); the term is not reported on a rejection or failure. `kAlgorithm` is not changed by this phase (Phase 3 set `batch-shared-bias-v2`; the next change is Phase 6's).

`compareJson()` (`tests/fusion/fusiongolden.cpp` lines 99-145) compares numbers under these keys with the portable bound in portable mode; they are copied constants, so they are equal in every mode. No change to `isExactKey()`.

**Acceptance Criteria:**
- [ ] A successful fit's diagnostics has a top-level `model` object whose only key is `per_step`, whose only keys are `gyro_slope_s` and `acc_slope_s`, holding the `Tuning` values the fit ran with (Task 4.4's diagnostics test: default values, and overridden values)
- [ ] `failureDiagnostics()` output has no `model` key; the nine `reject_*.json` goldens are unchanged by the re-capture of Task 4.6
- [ ] `git diff -- src/fusion/fusion.cpp` is empty or shows exactly one changed line (the added argument)
- [ ] `docs/SENSOR_FUSION.md`'s key list names `model` (Task 4.5) and the goldens carry it (Task 4.6)

**Complexity:** M

---

### Task 4.4: Unit tests on the covariance and the diagnostics

**Purpose:** Spec section 10's three expectations for the term (zero change gives the density covariance exactly; a known change gives the specified covariance; the term scales with `dt`), stated independently of the implementation, plus the validation and the reporting of the constants.

**Files to modify:**
- `tests/tst_fusion_kernel.cpp` — four new test slots after `preintegrationHonoursExactBoundaries` (declared in the slot list at lines 156-171), two new sample builders in the anonymous namespace next to `boundarySamples()` (lines 53-67), and additions to `validationRejectsEachDefect()` (212-264)

**Technical Approach:**

The pattern is `preintegrationHonoursExactBoundaries` (lines 197-210): a hand-built `Samples`, a `Tuning`, `preintegrateImu()`, and an assertion on the result. `preintegrateImu()` needs only `imuTime`, `force` and `gyro` (`integrationEdges()` with `includeGnss = false` never reads the GNSS arrays), so the builders below fill no GNSS fields and the tests do not call `validateSamples()`. The 9x9 covariance is `pim.preintMeasCov()` (`ImuFactor.h` line 138, order rotation / position / velocity). The reasoning that makes the expected values implementation-independent: GTSAM's propagation is `P <- A P A^T + B (aCov/dt) B^T + C (wCov/dt) C^T + iCov dt` per step (`ImuFactor.cpp` lines 78-84), where `A`, `B`, `C` depend on the measurements and the bias only, so a preintegration in which every step's covariance is `(d^2 + s^2 dt) I` produces the same arithmetic as one with slopes zero and the density replaced by `sqrt(d^2 + s^2 dt)` — provided every step has the same `s` and `dt`. The builders make that so with exactly representable numbers.

Builders (anonymous namespace):

- `Samples rampSamples()`: 1 s of IMU at 8 Hz, `i = 0..8`, `imuTime = i * .125`, `gyro = (0, 0, i * .03125)` rad/s, `force = (i * .125, 0, -9.80665)` m/s^2. Every time and value is a small integer times a power of two, so every per-step change is exactly `.03125` rad/s (gyro) and `.125` m/s^2 (force) and every step is exactly `.125` s. Use these literal constants, not division at run time.
- `Samples singleStepSamples(double dt)`: two IMU samples at `0` and `dt`, `gyro = {0, (0, 0, .5)}`, `force = {(0, 0, -9.80665), (1, 0, -9.80665)}`: one step of length `dt` with a change of `.5` rad/s and `1` m/s^2 regardless of `dt`.
- A helper `Tuning withSlopes(double gyro, double acc)` (a copy of `Tuning{}` with the two slopes set) and `Tuning densityFor(const Tuning &t, double dt, double deltaGyro, double deltaForce)` returning `t` with slopes zero and `gyroDensity = sqrt(t.gyroDensity^2 + (t.gyroStepSlope * dt * deltaGyro)^2 * dt)`, `accDensity = sqrt(t.accDensity^2 + (t.accStepSlope * dt * deltaForce)^2 * dt)`: the expected value is always a *second preintegration* with these densities and no term, so the test never restates the covariance propagation.
- Comparison: `sameCovariance(got, expected)` = `(got - expected).cwiseAbs().maxCoeff() <= 1e-9 * expected.cwiseAbs().maxCoeff()`. The tolerance exists only because `sqrt(x)^2` is not `x` in floating point; a missing term, a missing `dt` factor, or midpoint differences instead of end-minus-start move the result by 1e-3 to 1e0 relative in the cases below.

Tests:

1. `perStepTermIsZeroWithoutSignalChange`: `boundarySamples(Vector3(1, -2, .5))` (constant force, zero rate), interval `[.037, .863]` as in `preintegrationHonoursExactBoundaries`. Preintegrate with `Tuning{}` (default slopes) and with `withSlopes(0, 0)`; `QVERIFY(a.preintMeasCov() == b.preintMeasCov())` (Eigen's `==` is element-wise equality of every entry, i.e. bitwise for these finite values), and likewise `deltaPij()`, `deltaVij()`, `deltaRij().matrix()`. Then the same with `withSlopes(1e3, 1e3)`: still identical, because the change is zero, not because the slopes are small.
2. `perStepTermMatchesSpecifiedCovariance`: `rampSamples()`, interval `[0, 1]` (both ends on samples; the edges are the nine samples, eight steps of `.125`). With `withSlopes(8, 4)` (`sigma_w = 8 x .125 x .03125 = .03125` rad, `sigma_w^2 dt = 1.22e-4` against `gyroDensity^2 = 1e-6`; `sigma_a = 4 x .125 x .125 = .0625` m/s, `sigma_a^2 dt = 4.9e-4` against `accDensity^2 = 2.25e-4`): `sameCovariance(got, preintegrateImu(same samples, same interval, densityFor(tuning, .125, .03125, .125)))`. Repeat with `Tuning{}` (the defaults: the term is 1.3e-3 of the gyro covariance and 2.2 % of the accelerometer covariance, both above the tolerance). In both runs also assert that `got` is **not** within tolerance of the density-only preintegration (`withSlopes(0, 0)`), so the check cannot pass vacuously.
3. `perStepTermScalesWithStep`: `singleStepSamples(.125)` over `[0, .125]` and `singleStepSamples(.25)` over `[0, .25]`, both with `withSlopes(8, 4)`. Each must match its own expectation: `densityFor(tuning, .125, .5, 1)` for the first, `densityFor(tuning, .25, .5, 1)` for the second (same change, twice the step, twice the sigma: `sigma_w = .5` then `1.0`; `sigma_a = .5` then `1.0`). Then the cross check that pins the `dt` factor: the second step's covariance is *not* within tolerance of a preintegration of the second samples with `densityFor(tuning, .125, .5, 1)` (the sigma of the shorter step); the added variance differs by a factor four, so `maxCoeff` of the difference exceeds `1e-3` of the expected's.
4. `diagnosticsReportPerStepConstants`: `QCOMPARE(Tuning{}.gyroStepSlope, .026)` and `QCOMPARE(Tuning{}.accStepSlope, .40)`. Then `runPipeline(toChannels(fusionFixture("coarse_linear")), tuning, Checkpoint())` twice, with `Tuning{}` and with `withSlopes(.5, .7)` (the fixture has zero signal change, so both fits are the same fit); parse `result.diagnosticsJson` as `nonConvergenceIsSolverFailure` does (lines 544-546) and check `diagnostics["model"].toObject().keys() == {"per_step"}`, `per_step.keys() == {"acc_slope_s", "gyro_slope_s"}` (sorted), and the two values equal the tuning's (`QCOMPARE` on doubles; exact copies). Also check that both results' `objective` are equal (the term is zero on this fixture whatever the slopes).

In `validationRejectsEachDefect()` (lines 212-264), after the existing kernel-validation block: a `Tuning` with `gyroStepSlope = -1e-3` throws `std::invalid_argument` from `validateSamples(d, tuning)`; likewise `accStepSlope = NaN`; and `withSlopes(0, 0)` does not throw.

No CMake change: `tst_fusion_kernel` already links `gtsam` and includes `fusion/imuintegration.h`. No entry in `tests/acceptance_map.txt` is needed (the map cites `fitTraceMatchesGolden` for acceptance 104; Phase 7 maps spec section 10's "per-step term" line to the tests named here).

**Acceptance Criteria:**
- [ ] The four new slots exist with the names above and pass under `ctest -C Release -R tst_fusion_kernel$` and, on the capture machine, `-R tst_fusion_kernel_exact$`
- [ ] Test 1 compares `preintMeasCov()` for bitwise equality (no tolerance) and passes with slopes `0`, default and `1e3`
- [ ] Tests 2 and 3 compute every expectation by a second `preintegrateImu()` with slopes zero and the densities `sqrt(density^2 + sigma^2 dt)`; no test restates GTSAM's propagation formula
- [ ] Tests 2 and 3 each contain a negative check proving the tolerance would reject the density-only (test 2) or wrong-`dt` (test 3) covariance
- [ ] Test 4 shows the reported constants follow the `Tuning` passed to `runPipeline`, not the defaults
- [ ] `validationRejectsEachDefect` covers a negative slope, a NaN slope, and zero slopes accepted

**Complexity:** M

---

### Task 4.5: Documentation of the keys and the fixtures

**Purpose:** The diagnostics key list in `docs/SENSOR_FUSION.md` and the fixture table in `tests/README.md` state what the kernel now produces; the explanation of the term's meaning at other output rates is Phase 7's.

**Files to modify:**
- `docs/SENSOR_FUSION.md` — the noise-model statement (lines 103-105) and the diagnostics key list (lines 134-143)
- `tests/README.md` — section 11 fixture table, "Exercises" column of the three success fixtures (the table Phase 1 kept unchanged)

**Technical Approach:**

`docs/SENSOR_FUSION.md`:

- Key list (lines 134-143, "After a successful fit its top-level keys are ..."): insert `` `model` (`per_step`: the per-step noise constants `gyro_slope_s` and `acc_slope_s`) `` immediately before `` `display_position_velocity` ``, i.e. after `max_endpoint_correction_deg` and after any `stopping` / `quality` entries Phase 3 placed there. Keep the sentence's structure ("..., `X`, `model` (...), `display_position_velocity` and `limitations`.").
- Noise-model statement (lines 103-105, "Modeling noise densities are 0.015 m/s^2/sqrt(Hz) and 0.001 rad/s/sqrt(Hz), with integration covariance I x 1e-8."): after that sentence and before "These are modeling weights, not sensor specifications.", add one sentence of fact, no explanation: "Each integration step adds a white-noise term in quadrature, `sigma = slope x dt x |change of the interpolated signal across the step|` with slopes 0.026 (gyro, giving radians) and 0.40 (accelerometer, m/s), so the step's covariance is `(density^2 + sigma^2 x dt) I`; the slopes are reported under `model.per_step`." Phase 7 writes what the term means and how it behaves at other output rates; do not pre-empt that here.
- Nothing else in the file: section 8's table row was renamed by Phase 1; the "frozen algorithm" paragraph is Phase 7's.

`tests/README.md` section 11, fixture table (the `coarse_linear` / `coarse_maneuver` / `stationary_spin` rows), append to the "Exercises" cell:

- `coarse_linear`: "; zero signal change, so the per-step IMU noise term is identically zero and this golden's numbers are the density-only model (they did not change when the term was added)"
- `coarse_maneuver`: "; the per-step term on a noisy 100 Hz stream, where it is small against the density"
- `stationary_spin`: "; the per-step term at 25 Hz, including the 90 deg/s step at 31 s (one step with a change of 1.57 rad/s)"

Keep the rows on one line each (the tables are single-line rows). No change to the "Re-capture procedure" subsection, the tolerance policy or the "what changes" list (its statement that a fixture's files change when a phase changes its numbers already covers this phase).

**Acceptance Criteria:**
- [ ] `grep -n "model" docs/SENSOR_FUSION.md` shows the key list naming `model`, `per_step`, `gyro_slope_s`, `acc_slope_s`, and the noise-model paragraph naming the two slopes and the `(density^2 + sigma^2 x dt)` form
- [ ] `grep -n "per-step" tests/README.md` hits the three success rows of the fixture table and nothing describes the term as absent
- [ ] `ctest -C Release -L audit` passes (`tests/README.md` is excluded from the naming rule; nothing added names an old mechanism)

**Complexity:** S

---

### Task 4.6: Re-capture the goldens (the Phase 1 recipe)

**Purpose:** Every numerical phase ends by re-capturing the goldens from the product kernel with `fusion_golden_capture`; `coarse_maneuver` and `stationary_spin` change numerically with this term, `coarse_linear` must not.

**Files to modify:**
- `tests/data/fusion/capture.json` — rewritten by the tool (date, revision, hashes)
- `tests/data/fusion/coarse_linear.json` — the `model` object is added to `diagnostics`; nothing else changes
- `tests/data/fusion/coarse_maneuver.json`, `coarse_maneuver.channels.txt`, `stationary_spin.json`, `stationary_spin.channels.txt` — rewritten by the tool with the new numbers (the `.channels.txt` files change if any output bit changed, which is expected)
- The nine `reject_*.json` and `coarse_linear.channels.txt` — rewritten byte-identical, so `git status` does not list them

**Technical Approach:**

Quoted from Phase 1 (`01-golden-regression-harness.md`, Task 1.6 and "Notes for Implementer: The re-capture recipe"), to be run verbatim after Tasks 4.1-4.5 are complete and the kernel tests pass:

> The recipe, Git Bash, from the repository root, on the capture machine (64-bit MSVC 19.44, Release, `build-phase1/`):
>
> ```bash
> # 1. Build the kernel, the tests and the tool (Release).
> cmake --build build-phase1 --config Release
>
> # 2. Capture into tests/data/fusion/ (the default output directory), recording HEAD.
> #    PATH: Qt's bin, then the GTSAM and oneTBB install bin directories (gtsam.dll,
> #    metis-gtsam.dll, cephes-gtsam.dll, tbb12.dll, tbbmalloc.dll). No other solver on PATH.
> PATH="/c/Qt/6.9.3/msvc2022_64/bin:$PWD/build-solver-deps/GTSAM-install/bin:$PWD/build-solver-deps/oneTBB-install/bin:$PATH" \
>   build-phase1/FlySightViewer-build/Release/fusion_golden_capture.exe --revision "$(git rev-parse HEAD)"
> #    Check: twelve lines, one per fixture, in fixture order; the three success fixtures
> #    "succeeded", the nine reject_* "rejected (<reason>)"; no "** UNEXPECTED **";
> #    then "wrote 16 files to .../tests/data/fusion"; exit status 0.
>
> # 3. Reconfigure the application build: the exact-test gate reads capture.json at
> #    configure time (file(READ) is not a dependency, so this step is explicit).
> cmake build-phase1/FlySightViewer-build
> #    Check: the log says "Fusion exact tests registered for Release (... MSVC 19.44...)".
>
> # 4. Run the fusion tests in both modes.
> ctest --test-dir build-phase1/FlySightViewer-build -C Release -L fusion --output-on-failure
> ctest --test-dir build-phase1/FlySightViewer-build -C Release -L exact --output-on-failure
> #    Check: all green. -L fusion covers the portable comparison and (on this machine) the
> #    exact runs; -L exact alone is the explicit bit-identity proof of the rebuilt kernel.
>
> # 5. Report the changed golden files; they are part of the phase's files.
> git status --porcelain -- tests/data/fusion/
> git diff --stat -- tests/data/fusion/
> ```
>
> PowerShell equivalent of step 2's `PATH`: `$env:PATH = "C:\Qt\6.9.3\msvc2022_64\bin;$PWD\build-solver-deps\GTSAM-install\bin;$PWD\build-solver-deps\oneTBB-install\bin;$env:PATH"` then `build-phase1\FlySightViewer-build\Release\fusion_golden_capture.exe --revision (git rev-parse HEAD)`.
>
> What changes, and what it means:
> - `capture.json` changes on every capture (date, revision, hashes).
> - A `<fixture>.json` / `.channels.txt` changes when the phase changed that fixture's numbers or texts. Phase 3 changes the `algorithm` string, so all twelve `.json` files change then; the nine `reject_*.json` otherwise change only when a rejection reason changes.
> - Two captures of the same build are byte-identical; the tool verifies this in-process and refuses to write otherwise (exit 3).
> - A `** UNEXPECTED **` line (exit 2) means a success fixture no longer converges or a rejection fixture is no longer rejected: a kernel regression or a fixture the phase invalidated. Do not commit that capture; fix the kernel, or (only when the phase's specification says a fixture's expectation changes) the fixture and its `expectSuccess`, then capture again.
> - The phase's report lists every file `git status` shows under `tests/data/fusion/`.
>
> An unexpected outcome (exit 2) or a non-deterministic capture (exit 3) is never committed; a red test after the capture means the kernel and the goldens disagree with the tests' literal expectations (a count, a text, a key set), which the phase must resolve in the tests or the kernel, never by editing a golden.

Phase 4's checks on the capture, run after step 5 (Git Bash, read-only git):

```bash
git status --porcelain -- tests/data/fusion/
# expected: capture.json, coarse_linear.json, coarse_maneuver.json, stationary_spin.json,
# and (expected, not required) coarse_maneuver.channels.txt, stationary_spin.channels.txt.
# Never: coarse_linear.channels.txt or any reject_*.json.
git diff --numstat -- tests/data/fusion/coarse_linear.json
# expected: 6 0
git diff -U0 -- tests/data/fusion/coarse_linear.json | grep -E '^[-+][^-+]'
# expected: exactly these six added lines and no removed line
#        "model": {
#            "per_step": {
#                "acc_slope_s": 0.4,
#                "gyro_slope_s": 0.026
#            }
#        },
```

The `coarse_linear` criterion is the proof that a zero-change signal reproduces the density covariance through the whole fit, not only in the unit test. If any other line of `coarse_linear.json` changes, or `coarse_linear.channels.txt` appears in `git status`, the term is not exactly zero on a zero change (a branch that alters arithmetic, a changed density expression, an extra operation in `preintegrationParams()`): fix `imuintegration.cpp`, never the golden, and capture again (the orchestrator restores `tests/data/fusion/` if a capture must be discarded). The `coarse_maneuver` and `stationary_spin` diffs are expected to touch numbers (objective, biases, residuals, trace history, channel bits) and to add the same `model` object; the tool must still print `succeeded` for both without `** UNEXPECTED **`, and their iteration counts should be of the same order as before (the spec's corpus evidence: iterations unchanged).

**Acceptance Criteria:**
- [ ] The recipe ran verbatim; the tool printed twelve lines, no `** UNEXPECTED **`, `wrote 16 files`, exit 0; reconfigure logged `Fusion exact tests registered for Release`
- [ ] `git status --porcelain -- tests/data/fusion/` lists `capture.json`, `coarse_linear.json`, `coarse_maneuver.json`, `stationary_spin.json` and at most the two `.channels.txt` of the latter two; no `reject_*.json`, not `coarse_linear.channels.txt`
- [ ] `git diff --numstat -- tests/data/fusion/coarse_linear.json` is `6 0` and the filtered `diff -U0` shows only the `model` object
- [ ] `ctest -C Release -L fusion` and `-L exact` are green after the reconfigure; the whole suite passes without `-L`
- [ ] The phase report lists every changed golden file (Commit Policy: re-captured goldens are committed with the phase)

**Complexity:** S

---

## Testing Requirements

### Unit Tests
- New in `tst_fusion_kernel`: `perStepTermIsZeroWithoutSignalChange`, `perStepTermMatchesSpecifiedCovariance`, `perStepTermScalesWithStep`, `diagnosticsReportPerStepConstants` (Task 4.4), and the slope cases in `validationRejectsEachDefect`.
- Existing tests expected to pass unchanged: `preintegrationHonoursExactBoundaries`, `headingIsUnconstrained`, `exactConstantVelocityFit`, `backwardPropagationUndoesForward`, `reconstructionTimingAndEndpointCorrection` (zero-change signals or no preintegration).
- Existing tests that compare with goldens (`fitTraceMatchesGolden`, `tst_fusion_golden`, `tst_fusion_session`, `tst_fusion_jobs`, `tst_fusion_rows`) pass after the re-capture; before it, the two changed fixtures fail by design (the same ordering Phase 1 documents).
- `nonConvergenceIsSolverFailure` (or Phase 3's replacement for it) rests on `coarse_maneuver` with `maxIterations = 1`; its literal expectation may be affected by the changed covariance. If it fails after this phase, the remedy is in the test (a harder non-convergence case), as its own comment says, never in the kernel or a golden.

### Integration Tests
- `ctest --test-dir build-phase1/FlySightViewer-build -C Release -L fusion --output-on-failure` green against the re-captured goldens.
- `ctest ... -L exact --output-on-failure` green: the rebuilt kernel is bit-identical to the capture.
- `ctest ... -L audit` green: confinement regex and naming rules unaffected.
- The whole suite once without `-L`.

### Manual Verification
- The three `git` checks of Task 4.6 on `coarse_linear.json`.
- Inspect `git diff -- tests/data/fusion/coarse_maneuver.json`: the `model` object is present, `seeds[0].converged` is still `true`, the `history` length is of the same order as before.
- Optional, not an acceptance criterion: if Phase 2's `fusion_runner` is built, run it on one of the spec section 12 recordings (`TEMP/data/...`) and confirm `model.per_step` in the printed diagnostics and, against a run at the Phase 3 commit, a `quality.imu_nrms` that did not rise (spec section 5's corpus evidence is a small improvement in whitened RMS at unchanged iteration counts).

## Notes for Implementer

### Gotchas
- **Phase 3 came first.** `factorgraphfit.cpp`, `fusion.cpp`, `fusionpipeline.h` and the `stopping` / `quality` code in `fusionoutput.cpp` are Phase 3's; read them as they are, edit only the one call-site argument in `fusion.cpp` if Task 4.3 needs it. Line numbers in this document are `master` at `1f3139a`.
- **End minus start, not midpoints.** The spec defines the change across the step as the difference of the interpolated signal at the step's end and start. The midpoint value is what is integrated; the end/start values are what the sigma uses. Both come from `interpolateAt()`; the edges lie inside IMU coverage by construction of `integrationEdges()`, so it does not throw.
- **`dt` appears twice** in the covariance: once in `sigma = slope x dt x |change|`, once in `sigma^2 x dt`. The lab's form has only the second; the spec's form is binding, and Task 4.4's third test fails if the first is dropped.
- **No branch for zero slopes.** The arithmetic gives the density covariance exactly (`x + 0.0 == x`); a branch would be a second code path to keep bit-identical. Task 4.4's first test compares bitwise.
- **The params hold the last step's covariance after the loop.** Nothing reads them (the factor's noise model is built from `preintMeasCov()`); do not "restore" them, and do not add anything that reads `pim.params()->gyroscopeCovariance` as the density.
- **Exactly representable test numbers.** `rampSamples()` uses `.125`, `.03125` and small integers so every step has the same `sigma` bit for bit; a ramp built from `i * .01` would give per-step differences that vary in the last bit and break the equivalence with a constant modified density.
- **`sqrt(x)^2 != x`.** That is the only reason tests 2 and 3 have a tolerance; keep it at `1e-9` relative to the largest entry, and keep the negative checks that show the tolerance rejects the wrong covariance.
- **Goldens change only by re-capture.** If the `coarse_linear` check fails, the kernel is wrong; if `coarse_maneuver` or `stationary_spin` prints `** UNEXPECTED **`, the kernel is wrong (the term is far too small at 100 Hz and 25 Hz to change convergence); never edit a golden or a fixture.
- **Read-only git.** The implementer runs `status`, `diff`, `rev-parse`; the orchestrator stages and commits, and restores `tests/data/fusion/` if a capture is discarded.
- **Key names are binding**, including the `_s` suffix of `gyro_slope_s` / `acc_slope_s` (overview "Diagnostics key layout"); do not rename them to match the `Tuning` field names.

### Decisions Made
- **Route of the `Tuning` into the diagnostics:** a `const Tuning &` parameter on `successDiagnostics()` with `plan.tuning` passed at the one call site, unless Phase 3 already added it (then reuse). Alternatives rejected: copying the slopes into `FitResult` (Phase 3 owns `FitResult`'s shape and it would carry configuration, not a result) and reporting `Tuning{}` (wrong whenever a test overrides the slopes).
- **`modelSummary()` helper** so that Phase 6 adds `gyro_bias` with one initializer entry, as the assignment asks.
- **`preintegrationParams()` unchanged** and the loop overwrites the two sensor covariances; the density-only params remain the documented starting point and the function keeps its header contract.
- **No zero-slope branch** (see Gotchas); the exactness is arithmetical and tested.
- **Validation admits zero** (a second loop with `< 0`), same rejection text as the other tuning checks.
- **Expected values in tests are second preintegrations** with modified densities and slopes zero, so the tests hold whatever GTSAM's propagation formula is; the equivalence rests only on every step having the same covariance, which the builders guarantee.
- **Test constants** (slopes 8 and 4, 8 Hz ramps, single steps of `.125` and `.25` s) were chosen so the term is comparable to or larger than the density term; with the production slopes at 100 Hz the term is 1e-10 of the density and no covariance test could see it, which is the spec's intended behaviour at high rates, not a test target.
- **`docs/SENSOR_FUSION.md` gets one sentence of fact** in the noise-model paragraph in addition to the key list, so the document does not describe a model the kernel no longer has; the explanation is Phase 7's.
- **`coarse_linear` byte-identity is an acceptance criterion** of the re-capture, checked mechanically with `git diff --numstat` (`6 0`) and a filtered `diff -U0`.

### Open Questions
- None that block implementation. One conditional the implementer resolves by reading the code: whether Phase 3 already passes the `Tuning` to `successDiagnostics()` (Task 4.3 gives both branches). One note for the orchestrator: if Phase 3's version of `nonConvergenceIsSolverFailure` (or its successor) flips on the changed `coarse_maneuver` numbers, the fix is a harder test case in `tst_fusion_kernel.cpp`, reported as part of this phase.

## Definition of Done

This phase is complete when:
1. All tasks have passing acceptance criteria, in particular Task 4.6's three `git` checks on `coarse_linear.json` and no `reject_*.json` or `coarse_linear.channels.txt` in the capture's `git status`
2. All tests pass: `ctest -C Release` on `build-phase1/FlySightViewer-build` without `-L`, including `-L fusion`, `-L exact` and `-L audit`, after the reconfigure that reads the new `capture.json`
3. Code follows the patterns of `imuintegration.cpp` (file-local helpers in the anonymous namespace, the existing loop and duration check), `fusionoutput.cpp` (`seedSummary()`-style helper, initializer-list diagnostics) and `tst_fusion_kernel.cpp` (hand-built `Samples`, `Tuning` overrides, literal expectations)
4. No file outside `src/fusion/imuintegration.h`, `imuintegration.cpp`, `fusionsamples.h`, `fusionsamples.cpp`, `fusionoutput.h`, `fusionoutput.cpp`, the one argument in `fusion.cpp`, `tests/tst_fusion_kernel.cpp`, `docs/SENSOR_FUSION.md`, `tests/README.md` and `tests/data/fusion/` is changed; the kernel's purity, cancellation boundaries and the public result contract are untouched; nothing outside `src/fusion/` includes a GTSAM header
5. No TODOs or placeholder code remains; the phase report lists every file modified and every golden file re-captured, and states which branch of Task 4.3's `Tuning` routing applied
