# Implementation plan: the fused state at every IMU sample

Plan for `PLANS/fusion-reconstruction.md`, written 2026-09-29. The
specification below is the authority; where a phase document disagrees with
it, the specification wins, except where a decision below states how the
plan reads it.

## Feature specification

## The fused state at every IMU sample

Date: 2026-09-29
Status: specification for planning. Not an implementation plan.
Baseline: branch `store-requested-calculations` at the head that implements
`PLANS/done/fusion-plots.md` (the eight-plot fusion category, the attitude
derivation and the orientation attribute); the committed code is
authoritative. Where this document and the earlier specifications disagree,
this one wins; everything it does not mention stays as they specify.
Related: `docs/SENSOR_FUSION.md` (sections 4, 7 and 8), `docs/CALCULATIONS.md`
(section 17), `docs/COMPUTED_PLOTS.md`, `docs/DATA_SCHEMA.md` (sections 11
and 12), `tests/README.md` (section 11, the golden fixtures),
`tests/acceptance_map.txt`, `tests/audit/cleanup_audit.cmake`.
The uncertainty of the fused outputs is not specified here; a later
specification will build on the output this one defines, and the earlier
`PLANS/fusion-accuracy.md` is retired. Nothing here depends on it.

This document says what the change must do and the boundaries it must
respect. It avoids class layouts and function signatures unless a name is
part of the observable contract. The planning agent decides phasing; the
implementation agent decides code structure.

### 1. Motivation

The fit estimates the state at each GNSS fix: attitude, velocity and
position, plus the sensor biases. Between fixes it uses the IMU only to tie
neighbouring states together. What it publishes, though, is one sample per
IMU timestamp, and today those samples are filled in poorly:

- Position and velocity are points on the straight line between the fitted
  states at the two surrounding fixes. The IMU is not used at all, so the
  published velocity has the content of the GNSS rate, stored at the IMU
  rate. Its slope inside a fix interval is not the published acceleration.
- Attitude is the gyro integrated forward from the fitted state at the first
  fix, with what the integration misses of the second fitted state spread
  over the interval in proportion to elapsed time, whether or not the error
  is likely to have occurred then.
- Acceleration is the accelerometer reading rotated by that attitude. Where
  the readings and the fitted velocities disagree, as they do through a
  parachute opening sampled at 13 Hz, the published acceleration and the
  published velocity contradict each other.

None of the published samples is a fitted state: the fix times do not
coincide with IMU timestamps, so the plotted line cuts the corner at every
fix. A user reading elevation or velocity from the fusion category sees a
line that bends where two samples straddle a fix and runs straight between,
which looks like irregular sampling and is not.

The value of the IMU is its rate: it resolves what happens between fixes,
and GNSS pulls it back to an absolute measurement. The fusion output should
be the state a fit with a state at every IMU sample would give, and it can
be, without such a fit: with the states at the fixes settled, each fix
interval can be solved on its own, given its two ends and the biases, in
one pass over its samples.

### 2. Principles

- **The fit is unchanged.** The graph, the noise model, the initializer,
  the solver and the stopping rule are as they are. This specification
  changes only how the fitted states are turned into the published samples.
- **The output is the IMU-rate posterior.** At every IMU sample the
  published attitude, velocity and position are, to within one
  linearization, what a fit with a state at every IMU sample and the same
  measurements would give. The documentation says so, and says what the
  linearization leaves out.
- **One time axis.** The published samples are the recording's IMU samples
  between the first and last fix, as today, whatever the GNSS rate. No fix
  time is added and nothing is resampled.
- **The acceleration is the model's.** It is the accelerometer reading,
  corrected for bias and attitude, plus the share of the disagreement
  between the IMU and the fitted velocities that the model attributes to
  that reading. Integrated, it reproduces the published velocity.
- **One authority for the integration.** The reconstruction integrates the
  IMU with the same rule, the same interpolation and the same per-step noise
  as the fit's own factors. Nothing restates the step model.
- **A fit output changes, so the result version changes:** every stored fit
  is recomputed at the next start, once, and the golden fixtures are
  captured again from the changed kernel by the existing tool.

### 3. Scope

In scope:

- The reconstruction between fixes (section 5): the forward integration,
  the mismatch, its sharing and the one adjustment pass.
- The published channels as the reconstruction gives them (section 6): the
  seventeen existing channels, on the existing time axis, with new values.
- The diagnostics' account of the reconstruction (section 7).
- The result version bump and the golden fixtures captured again (section
  8).

Out of scope, unchanged:

- The fit's estimate at the fixes and the biases. The reconstruction reads
  the converged solution and changes no value in it.
- The published time axis: the IMU samples in the half-open fitted interval,
  as `docs/SENSOR_FUSION.md` section 4 defines it.
- The set of published channels, the plot list, the derived calculations of
  the fusion category, the record format, the demand layer, the status bar.
- The uncertainty of any output. A later specification will take the
  per-sample covariance from the same pass; this specification only requires
  that the pass be the one described here.
- The fit's noise densities and the weighting of the GNSS factors.

### 4. Terms

- **Fix interval**: the time between two consecutive GNSS fixes of the
  fitted window, with the fitted state at each end.
- **Integration edge**: a time at which the integration advances: every IMU
  sample in the interval and the two fix times at its ends, as the fit's
  factors already split their steps.
- **Step**: the time between two consecutive integration edges.
- **Forward state**: the state at an edge obtained by integrating the IMU
  from the fitted state at the interval's first fix, with the biases the
  fit assigns to the interval.
- **Mismatch**: the fitted state at the interval's second fix minus the
  forward state there: nine components, three each for attitude, position
  and velocity.
- **Share**: the part of the mismatch applied at an edge.
- **Corrected state**: the forward state at an edge with its share applied.
- **Step correction**: for a step from edge j to edge j+1, the corrected
  velocity change across the step, less the mean of the readings at the
  step's two edges, each with the bias removed and rotated by the corrected
  attitude at its own edge, less gravity times the step length; all divided
  by the step length: an acceleration, c_j. (The reading at a sample edge is
  the sample itself; at a fix it is the interpolated reading the
  integration uses.)

### 5. The reconstruction

For every fix interval of a fit that succeeds, in one pass over its samples:

- **Forward pass.** From the fitted state at the first fix, integrate gyro
  and accelerometer together, step by step: the attitude advances with the
  gyro; the accelerometer reading, bias removed, is rotated by that attitude
  and gravity is added; velocity and position advance with the result. The
  integration is the fit's own: the same midpoint interpolation of the
  readings, the same step boundaries and the same per-step noise. Alongside
  the states, the pass accumulates, for every edge j, the covariance P_j of
  the noise the steps up to j have added (P_0 = 0 at the first fix;
  P_(j+1) = F_j P_j F_jᵀ + Q_j), where F_j and Q_j are the transition and
  the noise that the solver library's preintegration update uses to
  propagate its own covariance across step j. P_n at the second fix is
  therefore the covariance of the fit's IMU factor for the interval.
- **Mismatch.** d is the fitted state at the second fix expressed in the
  local coordinates of the forward state there: nine components, attitude,
  position and velocity, in the order the fit's IMU factor uses. (The
  factor's own residual is the same difference taken the other way round;
  d is defined so that applying it to the forward state gives the fitted
  one.)
- **Sharing.** P_j and F_j are in the preintegration's own tangent
  coordinates, the frame of the state at the first fix, while d and the
  corrected states are in the local coordinates of each forward state. With
  M_j the Jacobian of the retraction at forward state j with respect to the
  preintegration tangent there, the correction at edge j is

      δx_j = M_j P_j Φ_jᵀ P_n⁻¹ M_n⁻¹ d,   Φ_j = F_(n-1) ⋯ F_j

  (Φ_n is the identity). Equivalently, the corrections are the ones that
  reach the fitted state at the second fix while minimizing the sum over
  the steps of each step's noise squared, weighted by the inverse of Q_j:
  the conditional mean of the step chain given both ends, linearized about
  the forward states. The share is nothing at the first fix and the whole
  mismatch at the second. It is one nine-component correction, applied
  jointly: an attitude share changes the direction in which every later
  reading was applied, and Φ_j carries that. Where the per-step noise is
  uniform the velocity share grows with elapsed time; a step that carries
  more noise, because the signal changed across it, takes more of the
  mismatch.
- **Corrected states.** The corrected state at edge j is the forward state
  there with δx_j applied by the retraction that inverts its local
  coordinates. At the second fix δx_n = d, so the corrected state there is
  the fitted state, exactly.
- **One pass.** The corrections are computed once, about the forward
  states, and applied once. What the linearization costs is only the split
  inside the interval, which the equivalence test of section 10 bounds; the
  ends are exact by construction.
- **Step corrections.** c_j for every step, as section 4 defines it, from
  the corrected states.

Where the mismatch is zero the pass is the forward integration and the step
corrections are zero.

### 6. What is published

The seventeen channels of `docs/SENSOR_FUSION.md` section 4, on the same
time axis as today. For each IMU sample:

- **Attitude** (the quaternion, and roll, pitch and yaw derived from it as
  today): the corrected attitude at the sample's edge.
- **Velocity and position:** the corrected state at the sample's edge.
- **Acceleration:** for a sample at edge i, with the step ending at i
  (correction c_before) and the step starting at i (c_after):

      a_i = R_i (f_i − b_a) + g + (c_before + c_after) / 2

  where f_i is the sample's own accelerometer reading, b_a the fitted
  accelerometer bias, R_i the corrected attitude at the sample and g the
  model's gravity. The first and last sample of the fitted interval have one
  adjacent step and take its correction alone. A step adjacent to a fix is
  the part-step between the sample and the fix, in that fix interval.

The reading is the sample itself, not the interpolated midpoint value the
integration uses, so that nothing smooths the accelerometer's signal: only
the correction is spread, over the two steps beside each sample. The
documentation states the consequence: integrated with the kernel's own rule,
the published acceleration reproduces the published velocity change over any
run of samples to within the spread of the corrections, and over the fitted
interval as a whole; it does not do so step by step. Where the corrections
are negligible, in steady flight, the published acceleration is the rotated
reading.

The time axis is the IMU's samples between the first and last fix, whatever
the GNSS rate, including a recording whose GNSS rate exceeds its IMU rate.
The documentation says so in one sentence.

Under rotation the fit's integration rotates each reading by the attitude at
the start of its step, which is off by about half the rotation across the
step. With exact readings that alone leaves a mismatch, and the pass shares
it out as corrections, so under fast rotation the published acceleration
carries part of the integration's own error. The documentation states the
size measured on a synthetic tumble at the IMU rates it names, 13 Hz
included, and names the remedy, rotating the reading by the mid-step
attitude, which changes the fit's estimate and is outside this
specification.

### 7. The diagnostics

The success diagnostics say what the reconstruction did:

- The account of the dense output names the reconstruction, replacing the
  present statement that position and velocity are interpolated linearly,
  and the limitations sentence no longer says that the dense output is not
  an IMU-rate smoothing posterior. The documentation and the diagnostics
  say the same thing.
- The largest step correction of the fit, in acceleration units, and the
  time it occurred; and the largest velocity mismatch of any interval, in
  velocity units; alongside the existing largest attitude endpoint
  correction, which stays.

Rejected and failed fits' diagnostics are unchanged.

### 8. The record, the version and the goldens

- The fit's outputs are the same channels with new values; the record
  stores and restores them as today, in the record format as it is.
- The fit's algorithm string changes, so every stored fit is stale at the
  next start and is computed again once, by the existing validity rules.
  The first start after the change recomputes every stored fit; the status
  bar shows it as it shows any computation.
- The golden fixtures are captured again with the existing capture tool
  from the changed kernel. The time axis of every fixture is unchanged, and
  the planner arranges a check that it is: the same samples, bit for bit,
  before and after.

### 9. Architecture

- **The kernel** replaces the present dense reconstruction with the pass of
  section 5. The pass takes its step model from the fit's own
  preintegration: F_j and Q_j are read from the solver library's
  preintegration update as it advances, not written again, and P_n is
  checked against the factor's covariance in the tests. GTSAM stays
  confined to the kernel. The pass
  runs after the fit's last iteration, inside the same cancellable job; it
  is not a cancellation boundary unless the planner finds it long enough to
  deserve one, which at a few microseconds per sample it is not.
- **The fit calculation** publishes the same channels; its result version
  follows the algorithm string as today.
- **The registration, the plot registry and everything above them** are
  unchanged.

### 10. Tests

- **Equivalence.** On a short committed fixture, a graph with a state at
  every integration edge (the IMU samples and the fixes), a one-step IMU
  factor between each pair of neighbouring edges, and the states at the
  fixes and the biases held at the fit's values, solved by the same solver
  from the forward states, gives attitude, velocity and position at every
  sample that agree with the reconstruction to within the linearization;
  the test states its tolerance against the size of the fixture's
  mismatches, and the documentation records the agreement measured. The
  fix states are held because, freed, they move along the unobservable
  heading, which is not what the test measures; the documentation says so,
  and records how far a freed graph's fix states moved.
- **The ends.** On every committed success fixture, the corrected state at
  each interval's second fix is the fitted state there, to rounding; and
  P_n equals the interval's IMU factor covariance, to rounding.
- **Sharing by noise.** On a fixture where a step carries more noise than
  its neighbours, that step takes the larger share; where the noise is
  uniform, the velocity share grows with elapsed time.
- **Zero mismatch.** On a fixture whose readings integrate exactly to the
  fitted states, the reconstruction is the forward integration, the step
  corrections are zero and the published acceleration is the rotated
  reading.
- **Consistency.** On every committed success fixture, the published
  acceleration integrated by the kernel's rule reproduces the published
  velocity change over the fitted interval, and over any run of samples to
  within the spread the documentation states.
- **The time axis is unchanged:** every fixture's `_time` is bit-identical
  before and after the change, and the same holds for a recording whose
  GNSS rate exceeds its IMU rate, built synthetically.
- **The diagnostics** carry the account of section 7, and the golden
  comparison covers it.
- **A stored fit from before the change is stale at the next start,** and a
  fit stored after it restores bit for bit.
- **The audit's confinement rules hold,** and the documents describe the
  reconstruction, what it publishes and what its one pass leaves out.

### 11. Documentation

`docs/SENSOR_FUSION.md` (section 4: the dense reconstruction paragraph, the
outputs table's rows for position, velocity and acceleration, the
diagnostics key list; section 7 if the boundaries' description mentions the
reconstruction; section 8, validation: the equivalence test and what the
one pass leaves out); `docs/CALCULATIONS.md` (section 17: the outputs and
the version bump); `docs/COMPUTED_PLOTS.md` (what the fusion plots now show
between fixes, in one paragraph); `docs/DATA_SCHEMA.md` (section 11, the
algorithm string; section 12, the record's channels are the same);
`tests/README.md` section 11 (the goldens captured again, and why) and
`tests/acceptance_map.txt` (a new range in the next free hundred; amended
items restated "(as amended)").

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
  path. Subject `Fusion reconstruction phase N: <phase name>`; body a short
  summary, then the session's attribution line. Rejected iterations are never
  committed.
- **Tag** each phase commit `plan/fusion-reconstruction/phase-N-done`; tags
  are never moved or deleted.
- **Fixes to a closed phase** are committed separately as
  `Fusion reconstruction phase N fixup: <what>` once reviewed and accepted.
- **CI workflow edits** are committed separately as
  `Fusion reconstruction phase N: CI (unverified)`.
- **Escalated phases** are not committed; their paths are listed in the final
  report, and phases that depend on them are not started.

## Phases

| Phase | Name | Purpose | Dependencies |
|-------|------|---------|--------------|
| 1 | The reconstruction pass | Spec §5 and the kernel half of §9: a per-step observer on the one preintegration loop, and the pass over every fix interval of a fitted window (forward states from the library's prediction, P_j read from the preintegration, the mismatch, the coordinate-mapped sharing, corrected states, step corrections, the published acceleration of §6 and the four summaries of §7), as a kernel function nothing publishes yet. Its §10 tests in `tst_fusion_kernel`: equivalence, the ends, sharing by noise, zero mismatch, consistency. The fit, the published channels and the goldens are unchanged bit for bit. | none |
| 2 | Publishing the reconstruction | Spec §6-§8, the rest of §9, the rest of §10 and all of §11's `docs/`: `run()` publishes the pass; the present dense reconstruction is removed; the diagnostics carry the account of §7; the algorithm string becomes `batch-temperature-bias-v4`; the goldens are captured again, with the `_time` column checked bit for bit against the previous capture; the time-axis test (fixtures and a GNSS-faster-than-IMU recording); stale and restored stored fits; every literal of the old string in the tests; the audit's algorithm-string rules and a rule that keeps the removed reconstruction out; `docs/SENSOR_FUSION.md`, `docs/CALCULATIONS.md`, `docs/COMPUTED_PLOTS.md`, `docs/DATA_SCHEMA.md`, `tests/README.md` section 11 and the section-1 rows. | 1 |
| 3 | Traceability | Spec §10's last bullet and §11's `tests/README.md` / `tests/acceptance_map.txt` half: the specification's clauses as items 901 onward (appendix J, section 9.10), the map's lines, the audit's traceability ranges and the item numbers in the headers of the audit groups phases 1 and 2 touched, earlier items this specification amends restated "(as amended)", section 10 for any rule phases 1 and 2 added, and section 12 manual steps if the phase adds any. | 1, 2 |

```text
1 The reconstruction pass ──> 2 Publishing the reconstruction ──> 3 Traceability
```

Strictly sequential. Phase 2 wires in and tests through the function phase 1
provides and removes what it replaces; phase 3 cites the test functions and
audit groups as phases 1 and 2 leave them. No two phases could run in
parallel without editing the same files (`tests/tst_fusion_kernel.cpp`,
`src/fusion/trajectoryreconstruction.*`, `tests/README.md`), and all build and
test in the one `build-agent/` tree.

## Key patterns and references

### Project rules

- `CLAUDE.md`: build and test in `build-agent/` (`CLAUDE.local.md`), ctest
  sequentially, what must stay green, the audit's conventions.
- `CLAUDE.local.md`: the three trees; `build/` and `build-deps/` are not
  touched; git rules.
- `.claude/docs/WORKFLOW-REFERENCE.md`: the stages and conventions.

### The probe (local, ignored; read before writing phase 1 or 2)

- `experiments/reconstruction_probe/FINDINGS.md`: the numbers behind
  decisions 2-7 below: how to read F_j and P_j from GTSAM, the coordinate
  question, the two reference graphs and the agreement measured, step
  corrections and the consistency bound, cost, GNSS faster than IMU.
- `experiments/reconstruction_probe/probe.cpp`: a working implementation of
  every formulation, reference graph and check. `IntervalPass` and `share()`
  (the forward pass and the lambda recursion), `correct()` (formulations a, b,
  c), `stepCorrectionsS/T()`, `publish()` (the published acceleration),
  `solveDenseFull()` (the dense one-step-factor graphs R1 and R2),
  `consistency()` (the bound), `tumble()` (the synthetic rotating recording),
  `gnssFasterThanImu()`. It is a probe, not a pattern: the product code is
  written in the kernel's style.
- `experiments/reconstruction_probe/run1.txt`, `run2_consistency.txt`: raw
  output.

### The kernel (`src/fusion/`)

- `trajectoryreconstruction.h`, `.cpp`: the present dense reconstruction
  (`DenseTrajectory`, `reconstructTrajectory()`): linear position and
  velocity, attitude spread by elapsed time. Replaced by this plan; phase 1
  adds beside it, phase 2 removes it.
- `imuintegration.h`, `.cpp`: `integrationEdges()`, `interpolateAt()`,
  `preintegrationParams()`, `preintegrateImu()` (the one
  `integrateMeasurement` call, the per-step covariance written into the
  shared params before it, the duration check), `gyroIncrement()`,
  `propagateAttitude()`. The step model the pass must read, not restate.
- `factorgraphfit.h`, `.cpp`: `FitResult` (`values`, `graph` rebuilt at the
  fitted bias, `gyroBiasSlope`, `biasModel`), `intervalBias()`,
  `buildFactorGraph()` and its factor order, `addTemperatureImuFactor()`,
  `collectResiduals()`.
- `temperatureimufactor.h`, `.cpp`: the IMU factor of the fit; its noise
  model is `pim.preintMeasCov()`; `preintegratedMeasurements()` gives a test
  the factor's covariance; `evaluateError()` shows the interval bias rule.
- `fusion.cpp`: `planFit()` (the tuning with `maxGap`, the window, the bias
  model) and `fitAndAssemble()`, where the reconstruction is called and the
  channels and diagnostics are assembled.
- `fusionpipeline.h`: `runPipeline()` and `PipelineTrace`, the seam for
  tests that need a run with a custom recording.
- `fusionoutput.h`, `.cpp`: `fillOutputChannels()`, `successDiagnostics()`
  (the keys `max_endpoint_correction_deg`, `display_position_velocity`,
  `limitations`), `failureDiagnostics()`.
- `fusion.h`: `Result` (its comment on what position and velocity are),
  `Algorithm` (`batch-temperature-bias-v3`, spelled once in `src`), the
  cancellation contract of `run()`.
- `fusionsamples.h`: `Samples`, `Vectors`, `Tuning`, `kGravity`, `kPi`,
  `kImuGapMedians`, `medianInterval()`.
- `fusionregistration.cpp`: `resultVersion` is `Fusion::Algorithm`; nothing
  else there changes.
- `src/CMakeLists.txt`: the `flysight_fusion` source list (a new file under
  `src/fusion` goes there).

### GTSAM 4.3a0 (tangent preintegration)

- `third-party/GTSAM-install/include/gtsam/navigation/ImuFactor.h`,
  `PreintegrationBase.h` (public virtual `update()`, `predict()`,
  `computeErrorAndJacobians()`), `TangentPreintegration.h` (`preintegrated()`,
  `UpdatePreintegrated()`), `NavState.h` (`retract()` with its Jacobians,
  `localCoordinates()`).
- Sources, read only: `build-deps/solver-sources/gtsam/gtsam/navigation/ImuFactor.cpp`
  (`integrateMeasurement`: the qualified `update` call and the covariance
  propagation), `TangentPreintegration.cpp` (the step: the midpoint reading
  rotated by the attitude at the step's first edge), `PreintegrationBase.cpp`
  (the residual is `state_j.localCoordinates(predicted)`), `NavState.cpp`
  (the chart).

### Kernel tests and golden infrastructure

- `tests/tst_fusion_kernel.cpp`: the only test that includes the kernel's
  internal headers. Helpers `boundarySamples()`, `linearSamples()`; tests
  `preintegrationHonoursExactBoundaries`, `perStepTermMatchesSpecifiedCovariance`
  (per-step covariance), `reconstructionTimingAndEndpointCorrection`,
  `exactConstantVelocityFit`, `reconstructionUsesIntervalBias` (the three
  that call `reconstructTrajectory()`), `diagnosticsReportPerStepConstants`,
  the diagnostics key-set assertion (search `"display_position_velocity"`),
  `fitTraceMatchesGolden`, the algorithm literal (search
  `batch-temperature-bias-v3`).
- `tests/fusion/fusionfixtures.h`, `.cpp`: the twelve golden fixtures
  (`fusionFixture()`) and the four initializer fixtures
  (`initializerFixture()`); bit-reproducible generators.
- `tests/fusion/fusiongolden.h`, `.cpp`: golden loading, the portable bound
  (`portableFloor()`: keys ending `_deg` take the degree floor), `_time`
  exact in both modes, `toChannels()`.
- `tests/fusion/fusiontrace.h`: the trace seam used by the capture tool.
- `tests/tst_fusion_golden.cpp`: `successFixturesMatchGolden`,
  `rejectionFixturesMatchGolden`, `comparatorHoldsItsBounds` (search
  `max_endpoint_correction_deg`), `twoRunsAreBitIdentical`.
- `tests/fusion_golden_capture.cpp`: the capture tool.
- `tests/data/fusion/`: the goldens and `capture.json` (captured 2026-09-22;
  `algorithm` `batch-temperature-bias-v3`).
- `tests/CMakeLists.txt`: the fusion-tests block, `FLYSIGHT_FUSION_GOLDEN_DIR`,
  the exact-test gate that reads `capture.json` at configure time.

### Stored fits and the algorithm literal in tests

- `tests/tst_fusion_store.cpp`: `restoredAfterRestartIsBitIdentical`,
  `codeStampChangeDropsRecordOnLoad` (stamps `batch-temperature-bias-v2`
  today), a diagnostics literal near the top.
- `tests/tst_fusion_jobs.cpp`, `tests/tst_fusion_session.cpp`
  (`registrationShape`), `tests/tst_result_records.cpp`: literals of the
  algorithm string.

### Audit and traceability

- `tests/audit/cleanup_audit.cmake`: groups `solver-confinement` (GTSAM
  headers: kernel and its tests), `fusion-model` (three kinds of boundary,
  one `integrateMeasurement`, the fusion document's forbidden phrases, the
  retired algorithm strings), `fusion-tooling` (the internal-header pattern
  the tools may not include; a new header under `src/fusion` is added to
  it), `stored-results` (one authority: the fusion algorithm string, count 1
  in `src`), the traceability check (the list of ranges and the per-range
  loops near the end).
- `tests/acceptance_map.txt`: header (ranges, amendments), line forms;
  items 801-863 as the latest pattern.
- `tests/README.md`: section 1 (a row per executable; `tst_fusion_kernel`),
  section 9.9 and appendix I (the latest specification's matrix and
  clauses, the pattern for 9.10 and appendix J), section 10 (the audit
  rules described), section 11 (golden regression, the re-capture procedure
  and its stale paths `build-phase1/` and `build-solver-deps/`), section 12
  (manual steps; 12.2 the runner on the reference recordings).

### Documents

- `docs/SENSOR_FUSION.md`: section 4 (the dense reconstruction paragraph,
  the outputs table, the diagnostics key list), section 7 (stored results
  and the algorithm string), section 8 (validation, the test table).
- `docs/CALCULATIONS.md` section 17: the fit's outputs; the result version.
- `docs/COMPUTED_PLOTS.md`: the "Sensor fusion" category.
- `docs/DATA_SCHEMA.md` sections 11 (the `records` stamp example) and 12
  (stored results).
- `PLANS/done/fusion-plots.md`, `PLANS/done/fusion-improvements.md`: the
  specifications whose items (appendices I and C) may be amended.

## Decisions and constraints

1. **Build first, publish second.** Phase 1 adds the pass beside the present
   reconstruction and publishes nothing, so that its tests prove the pass
   against the probe's findings while the goldens hold the fit unchanged bit
   for bit; phase 2 switches `run()` to it and removes the present
   reconstruction in the same change. The coexistence lasts one phase and is
   not a kept mechanism: phase 2's audit rule keeps the removed names out.
   The numbers change, the algorithm string changes and the goldens are
   captured again in one phase (phase 2), because a golden test is red
   otherwise.

2. **The mismatch.** The specification describes d twice: as "the residual
   the fit's IMU factor reports" and as "the fitted state at the second fix
   expressed in the local coordinates of the forward state there". In GTSAM
   4.3 the factor's residual is `fitted.localCoordinates(predicted)`, the
   other way round, and minus it misses the fitted end by up to 1.7e-7. The
   plan takes the second description, **d = forward_n.localCoordinates(fitted_n)**,
   which reaches the fitted state exactly, as §5 "Corrected states" requires.

3. **The sharing is coordinate-mapped.** P_j and Φ_j live in the
   preintegration's tangent coordinates (the delta's rotation vector,
   position and velocity, in the frame of the state at the first fix); the
   corrections are applied in the NavState local coordinates of each forward
   state. The plan reads §5's formula in the coordinates the covariance
   lives in: with M_j the Jacobian of the local coordinates at forward_j
   with respect to the preintegration tangent at edge j (the `H2` Jacobian
   of `NavState::retract` as the library computes it),
   δξ_j = P_j Φ_jᵀ P_n⁻¹ M_n⁻¹ d, and the corrected state is
   `forward_j.retract(M_j δξ_j)`. At the second fix M_n δξ_n = d, so the end
   is exact. Applying the formula literally in the local coordinates (P_j
   unmapped) is wrong at first order under rotation: 5-15 % of the velocity
   mismatch in the probe's tumble, 9.9e-5 relative on `coarse_maneuver`.
   The mapped form agrees with the per-interval dense solve to rounding on
   every fixture and is quadratic in the mismatch.

4. **Reading the step model.** The one `integrateMeasurement` call stays in
   `preintegrateImu()`; the pass hangs a per-step observer off that loop.
   F_j is the `A` of the library's own `update()`, obtained before the step
   by updating a copy of the preintegration (`auto c = pim; c.update(...)`,
   bit-identical to the internal A); P_(j+1) is `pim.preintMeasCov()` after
   the step. Q_j is never formed: the sharing runs backward as
   λ_n = P_n⁻¹ M_n⁻¹ d, λ_j = F_jᵀ λ_(j+1), δξ_j = P_j λ_j. The forward state at
   edge j is `pim.predict(fitted state at the first fix, interval bias)`
   after j steps, the factor's own prediction and gravity. With no observer
   `preintegrateImu()` is bit-identical to today (phase 1's exact golden
   runs prove it). A subclass cannot intercept the step: the library calls
   `update` qualified.

5. **The bias and noise of an interval are the fit's.** The pass
   preintegrates interval k at `intervalBias(window, k, B(0), fit.gyroBiasSlope, fit.biasModel)`
   of the converged values, with the fit's `Tuning` (the one `planFit()`
   made, `maxGap` included), so that P_n is bit for bit the covariance of
   the interval's factor in `FitResult::graph`.

6. **The step correction is the trapezoid of the edge-rotated readings.**
   §4 defines c_j as the corrected velocity change less "what the step's
   reading gives when rotated by the corrected attitude", divided by the
   step length, without saying which attitude or which reading. The plan
   takes c_j = (v_(j+1) − v_j)/Δt_j − ½[R_j (f(e_j) − b_a) + R_(j+1) (f(e_(j+1)) − b_a)] − g,
   f(e) the kernel's interpolated reading at the edge (the sample's own at
   a sample edge), R the corrected attitudes at the step's two edges. It is
   the definition under which §6's stated consequence holds: integrated by
   the kernel's rule, the published acceleration reproduces the published
   velocity change over the fitted interval (2.1e-5 m/s worst on the
   fixtures) and over any run to within the jumps of the corrections at its
   ends. The reading the fit's integration applies (the midpoint reading
   rotated by the attitude at the step's first edge) makes the whole-interval
   claim fail under rotation (3.2 m/s over 6 s in the probe's 3 rad/s
   tumble at 25 Hz, 3.8e-4 m/s on `coarse_maneuver`) and doubles to
   quadruples the acceleration error against truth there. The zero-mismatch
   test of §10 runs on a recording without rotation, where the two
   definitions agree and c is zero; under rotation with a zero mismatch c is
   minus the rotation lag, which phase 1 documents in the test.

7. **The equivalence reference holds the ends.** §10 describes a dense graph
   with GNSS factors at the fixes and free bias variables. Solved from the
   fit's solution, its fix states move away from the fit's along the
   unobservable yaw (0.9 × the mismatch on `coarse_maneuver`, 0.18 degrees on
   `stationary_spin`, 5.7 degrees on `rest_throughout`), so no tolerance can
   be stated against the mismatch. The test's reference is the same dense
   graph (a state at every edge, one-step IMU factors with the same per-step
   covariance, the interval bias rule) with the fix states and B(0), T(0)
   held at the fit's values: §1's premise that, "with the states at the
   fixes settled, each fix interval can be solved on its own". On
   `coarse_maneuver` the tolerance is 1e-5 × the largest mismatch component
   + 1e-12 per component (measured 3.3e-8 relative; the literal formula of
   decision 3 fails it). A synthetic rotating recording is tested as well,
   because `coarse_maneuver` barely rotates; its tolerance also covers the
   difference between multi-step and chained one-step preintegration
   (1.1e-6 degrees on `stationary_spin`), and phase 1 states it from its
   measurement. The documentation (phase 2) records the agreement measured
   and why the free graph is not the reference.

8. **The consistency bound**, the "spread the documentation states": for a
   run from sample a to sample b, ¼Δt_a|c_before(a) − c_after(a)| +
   ¼Δt_b|c_after(b) − c_before(b)|, plus ½Δt × the largest difference between
   the corrections of the part-steps of every sample step that contains a
   fix. Tight on the fixtures; the tests allow 1.01 × the bound + 1e-12 m/s.
   Over the whole fitted interval the error is the first and last terms
   only.

9. **No new cancellation boundary.** The pass costs 2.5-5.7 µs per sample in
   the probe's unoptimized code, 0.5 % of the fit on the largest fixture. It
   runs after the fit, inside the job; the audit's count of three boundary
   kinds stays.

10. **The diagnostics' names.** Kept: `max_endpoint_correction_deg` (the
    norm of d's attitude part, the largest over the intervals; the same
    quantity as today), `imu_outputs`, `limitations` (rewritten per §7).
    Replaced: `display_position_velocity` becomes `dense_output`, a text that
    names the reconstruction; the old key would call the output
    display-only, which it no longer is. Added: `max_step_correction_m_s2`,
    `max_step_correction_time_s` (the midpoint of that step, seconds since
    the epoch like `start_s`), `max_velocity_mismatch_m_s` (the norm of d's
    velocity part, the largest over the intervals). Rejected and failed
    fits' diagnostics are unchanged.

11. **The algorithm string** becomes `batch-temperature-bias-v4`. A stored
    fit stamped `batch-temperature-bias-v3` is stale at the next load.

12. **The time axis is checked twice.** Permanently: a kernel test that the
    published `_time` of every success fixture, and of a synthetic recording
    whose GNSS rate exceeds its IMU rate run through `runPipeline()`, is
    exactly the IMU samples in `[first fix, last fix)` of the window
    (`FitPlan` is internal, so the test computes the expected samples from
    the fixture and the diagnostics' `start_s` and `end_s`). Once, at the
    re-capture: the `_time` column of each success fixture's channels file is
    compared byte for byte with the previous capture, and the result is in
    the phase report and `tests/README.md` section 11.

13. **Where the tests go.** Every new kernel test is a function of
    `tst_fusion_kernel` (it may include the internal headers and GTSAM); no
    new executable, so the audit's allowed-file regexes do not change.

14. **Re-capture.** Phase 2 captures on `build-agent/` with the installs
    under `third-party/*-install` (`CLAUDE.local.md`); `tests/README.md`
    section 11's procedure, which names trees that no longer exist
    (`build-phase1/`, `build-solver-deps/`), is brought up to date in the
    same change, naming the tree generically as `CLAUDE.md` does.

15. **Documentation.** Every `docs/` change is phase 2's: phase 1 changes no
    behaviour anyone sees. Each phase updates the `tests/README.md` section-1
    row of the executables it adds functions to. Traceability (the map,
    section 9.10, appendix J) is phase 3's, as in earlier plans.

16. **Acceptance items** 901 onward, item = 900 + the clause number of
    appendix J, which phase 3 numbers. Phases 1 and 2 name their test
    functions in their reports; they do not add map lines.

Constraints the code imposes:

- GTSAM stays in `src/fusion` and `tst_fusion_kernel`, the capture tool and
  the solver probes (`solver-confinement`); the kernel does not log.
- `preintegrateImu()` holds the one `.integrateMeasurement(` of
  `imuintegration.cpp`; the per-step covariance is written into the shared
  params before it, and the observer must not change that order.
- `Fusion::Algorithm` is spelled once in `src` (`stored-results`); the audit
  rules that name the string change with it.
- A new header under `src/fusion` joins the internal-header pattern of
  `fusion-tooling`.
- Phase 1 leaves every golden bit-identical (the exact tests on this
  machine); phase 2 changes the numbers of the three success fixtures' JSON
  and channels, the `algorithm` of all twelve JSON files and nothing of the
  rejections' reasons.
- Tests run sequentially in `build-agent/`; the fusion tests time out under
  load.

## Interfaces between phases

### Phase 1 provides (used by phase 2)

In `src/fusion/trajectoryreconstruction.h`, namespace
`FlySight::Fusion::Detail`, beside the present `DenseTrajectory` and
`reconstructTrajectory()`:

```cpp
/// The fitted window at every original IMU sample (spec section 5).
struct ImuRateTrajectory {
    std::vector<double> time;          ///< IMU samples in [first fix, last fix), s since the epoch
    std::vector<gtsam::Rot3> rotation; ///< corrected attitude, body to NED
    Vectors position, velocity;        ///< corrected state, NED, m and m/s
    Vectors acceleration;              ///< spec section 6, NED, m/s^2
    double maxEndpointCorrectionDeg;   ///< largest |attitude part of d| over the intervals, deg
    double maxStepCorrection;          ///< largest |c_j| over the window, m/s^2
    double maxStepCorrectionTime;      ///< midpoint of that step, s since the epoch
    double maxVelocityMismatch;        ///< largest |velocity part of d| over the intervals, m/s
};

ImuRateTrajectory reconstructAtImuRate(const Samples &window, const FitResult &fit, const Tuning &tuning);
```

Everything but the four summaries aligns with `time`. The per-interval seam
the tests need (the edges, forward and corrected states, P_n, d, the
corrections and the step corrections of one interval) and the per-step
observer of `preintegrateImu()` are phase 1's to name; phase 2 does not use
them, except that phase 2 may keep using phase 1's test helpers.

Phase 1 also provides, in its report: the names of its test functions, and
the agreement measured by the equivalence test on `coarse_maneuver` and on
the rotating recording (the numbers phase 2 writes into
`docs/SENSOR_FUSION.md` section 8).

### Phase 2 provides (used by phase 3)

- `run()` publishes `reconstructAtImuRate()`; `DenseTrajectory`,
  `reconstructTrajectory()` and `endpointCorrection` exist nowhere in `src`
  or `tests`.
- The success diagnostics' keys of decision 10 and
  `Fusion::Algorithm == "batch-temperature-bias-v4"`.
- The audit group that keeps the removed reconstruction out (phase 2 names
  it; `fusion-reconstruction` is suggested) and the amended algorithm-string
  rules.
- In its report: the names of its test functions, the files the re-capture
  changed, the `_time` comparison's result.

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
  path. Subject `Fusion reconstruction phase N: <phase name>`; body a short
  summary, then the session's attribution line. Rejected iterations are never
  committed.
- **Tag** each phase commit `plan/fusion-reconstruction/phase-N-done`; tags
  are never moved or deleted.
- **Fixes to a closed phase** are committed separately as
  `Fusion reconstruction phase N fixup: <what>` once reviewed and accepted.
- **CI workflow edits** are committed separately as
  `Fusion reconstruction phase N: CI (unverified)`.
- **Escalated phases** are not committed; their paths are listed in the final
  report, and phases that depend on them are not started.
