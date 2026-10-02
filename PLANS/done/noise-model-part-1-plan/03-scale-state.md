# Phase 3: The scale-factor state

## Purpose

This phase implements the specification's scale-factor state: section 6's fifth bullet, and
section 10's "the scale factors are variables of the graph beside the biases".
The full fit gains one graph variable, `S(0)`, holding six per-axis
sensitivity factors with the datasheet's tolerance as their prior. The
integration divides the readings by it. The graph is re-preintegrated at the
fitted scale under the existing settled test. The reconstruction publishes
`R (f / s - b_a) + g + (c_before + c_after) / 2`, and the diagnostics report
`model.scale`. These pieces form one phase because they move the same goldens
and are captured once. Overview decision 14 fixes the design: the scale
belongs to the full fit only, and a fit without the state is the same fit with
the state off.

The technical core is how the IMU factor depends on `S(0)`. GTSAM gives
first-order bias corrections only. The scale's effect differs from sample to
sample, so the loop accumulates a 9x6 Jacobian of the preintegrated vector
with respect to the scale. It uses the library's own per-step transition and
input Jacobians, and a new factor applies that Jacobian (What changes, below).
A probe built on the real kernel confirmed the formula, the factor, the
recovery test and convergence on every fixture (Probe, at the end).

Clauses owned: 18, 19, 20, 21, 23, 48, 54 (items 1018-1021, 1023, 1048,
1054). Clauses 18 and 20 are restated "(as settled)" (Decisions 7, 8). Earlier
items restated "(as amended)": 246, 901, 910, 914, 925 (Traceability).

## Dependencies

- **Depends on phase 2.** At the start the following hold:
  - `src/fusion/sensornoise.h` provides `ImuNoise`, whose `SensorNoise` members
    carry `sensitivityTolerance` (0.01 for both sensors, phase 2 decision 5).
  - `Tuning::noise` holds the recording's `ImuNoise`, and the four retired
    constants are gone.
  - The signature is `preintegrateImu(samples, start, end, bias, const ImuNoise &noise, observer)`.
    It computes the sampling term and the rotation remainder in its loop, from
    the second differences of the `Samples` it is handed and from the step's
    bias-corrected midpoint quantities.
  - The diagnostics carry `configuration`, plus `model.noise` beside
    `model.gyro_bias`.
  - `Fusion::Algorithm` is `batch-temperature-bias-v6`.
  - There are fourteen golden fixtures. All of them, and the initializer
    fixtures, state their configuration and lie on its lattice:
    `rest_throughout` has its IMU noise at the datasheet level, and `sacc_anchor` and `drifting_bias`
    are at 12.5 Hz.
  - The audit group `noise-model` exists, and the explicit completeness list
    ends at phase 2's items.
  - `tests/README.md` section 12.9 holds M47 and M48.
- **Blocks phase 4.** Phase 4 consumes exactly the names under Interfaces.

## What changes

### The integration: `imuintegration.h` / `.cpp` (clause 19)

`preintegrateImu()` gains the linearization scale. It also gains an optional
output for the scale Jacobian, accumulated in its one loop (signature under
Interfaces). Notation: the step runs from edge `j` to `j+1`, of length `dt`.
The preintegration is linearized at `(b, s)`, with `s = (s_a; s_g)`. `f` and
`w` are the raw midpoint readings, as `interpolateAt()` gives them today.
`./` divides component-wise.

```
w~ = w ./ s_g      f~ = f ./ s_a                    what the step integrates
phi = (w~ - b_g) dt/2,  R = Exp(phi),  u = f~ - b_a
ImuStep::gyro = w~      ImuStep::force = R u + b_a  (today's turn, on the divided readings)
A, B, C = the library's update(step.force, step.gyro, dt, &A, &B, &C) on a copy of the
          preintegration before the step (the reconstruction's F_j today, bit for bit)
W = diag(w~ ./ s_g)     F = diag(f~ ./ s_a)
G = [ -B R F  |  B R [u]x Jr(phi) W dt/2 - C W ]     9x6: columns acc x,y,z then gyro x,y,z
H_0 = 0,  H_(j+1) = A H_j + G,  scaleJacobian = H_n  (d preintegrated() / d s at s)
```

`Jr` is `gtsam::Rot3::ExpmapDerivative`, and `[u]x` is `gtsam::skewSymmetric(u)`.
`B` and `C` are the library's Jacobians with respect to the bias-corrected
acceleration and rate that it integrates. The middle term is the half-step
turn's dependence on the gyro scale. Without it the gyro columns are wrong by
1.3 % at 2 rad/s and 100 Hz (probe). With it the Jacobian agrees with central
differences to 4e-10 relative.

The library's own bias Jacobians, `A H - B` and `A H - C`, treat
`d acc / d b_a` as `-I` and ignore the turn. That is existing behaviour, and
the settled test absorbs it. The scale's Jacobian is exact.

Rules, each observable by a test:

- **The divided readings are what the step integrates.** The sampling term
  and the rotation remainder are computed from them: `c_k` of the readings
  divided by the scale, and `theta`, `dtheta`, `fbar`, `df` of the divided
  readings (Decision 5). The densities are not divided. At `s = 1` every
  division is exact, so every number is bit-identical to phase 2's.
- **The library's per-step Jacobians are taken once, here.** The loop
  computes `A`, `B` and `C` when an observer is given or the Jacobian is
  asked for. It hands `A` to the observer as `ImuStep::transition`.
  `reconstructInterval` reads `step.transition` and stops updating a copy of
  its own (Decision 4). The fits that ask for neither (the initializer's, and
  the comparison fit) pay nothing.
- One `.integrateMeasurement(` call. The sensor covariances are written only
  here. No `UpdatePreintegrated` anywhere (`fusion-model` holds as it stands).
- `ImuStep`'s comments say that `force` and `gyro` are the divided readings,
  and that `transition` is the step's `A`. The header paragraph says that the
  readings are divided by the scale.

### The factor: `src/fusion/scaledimufactor.h` / `.cpp` (new; clauses 18, 48)

`ScaledImuFactor` is the full fit's IMU factor. It is a
`gtsam::NoiseModelFactorN<Pose3, Vector3, Pose3, Vector3, imuBias::ConstantBias, Vector3, Vector6>`
over `X(k-1), V(k-1), X(k), V(k), B(0), T(0), S(0)`. It is built from the pim
preintegrated at the interval's bias and scale, `temperatureDelta`, the
`scaleJacobian` and the `linearizationScale` `s^`. Its noise model is
`Gaussian::Covariance(pim.preintMeasCov())`, as in `TemperatureImuFactor`.

The error is `PreintegrationBase::predict()` and `computeError()`, with the
preintegrated delta `zeta` in place of `biasCorrectedDelta`, using NavState's
public functions:

```
b    = [B_a ; B_g + T dT]                                          (TemperatureImuFactor's interval bias)
zeta = pim.biasCorrectedDelta(b, &Db) + H_s (S - s^)
xi   = x_i.correctPIM(zeta, pim.deltaTij(), p.n_gravity, p.omegaCoriolis, p.use2ndOrderCoriolis, &Dxi_x, &Dxi_z)
x^_j = x_i.retract(xi, &Dp_x, &Dp_xi)
e    = x_j.localCoordinates(x^_j, &De_j, &De_p)
H1|H2 = De_p (Dp_x + Dp_xi Dxi_x), split [:, 0:6] and [:, 6:9] R_i^T  (computeErrorAndJacobians' velocity rule)
H3|H4 = De_j, split the same way with R_j^T
H5 = De_p ((Dp_xi Dxi_z) Db)    H6 = H5's gyro columns x dT    H7 = De_p ((Dp_xi Dxi_z) H_s)
```

The products are associated as GTSAM's `predict()` and `computeError()`
associate them. The factor then reproduces `TemperatureImuFactor` on the same
pim bit for bit at `S = s^`, for the error and for `H1`-`H6` (probe). The
factor is stateless (GTSAM may linearize in parallel). Its accessors are
`preintegratedMeasurements()`, `temperatureDelta()`, `scaleJacobian()` and
`linearizationScale()`. The class comment states the contract above.

`TemperatureImuFactor` stays as the factor of the temperature model with the
scale state off: the comparison fit (decision 14) and the tests' held-ends
reference use it. Its header comment says so.

### The fit: `factorgraphfit.h` / `.cpp` (clauses 18, 19, 48)

- **The switch.** The fit's model gains a scale-state switch, off by default,
  so the initializer's prefix and segment fits stay stock. `planFit` turns it
  on for the full fit, and `gyroBiasModelFor()` does not, so
  `temperatureGraphShape` holds as it stands. The implementer may extend
  `GyroBiasModel`, renaming it if the name then misleads, or add a sibling.
  The switch on without the temperature model throws
  `std::invalid_argument`. That is a programming error, unreachable through
  `planFit`.
- **The variable.** `S(0)` (`gtsam::symbol_shorthand::S`) is inserted at
  `Vector6::Ones()` beside `T(0)`. `BiasLinearization` gains
  `gtsam::Vector6 scale = gtsam::Vector6::Ones()`, and `linearizationOf()`
  reads `S(0)` when the state is on. Every build preintegrates each interval
  at its interval bias and at `at.scale`, with the Jacobian, into a
  `ScaledImuFactor`.
- **The prior.** The prior is
  `PriorFactor<gtsam::Vector6>(S(0), Vector6::Ones(), Diagonal::Sigmas(t_a, t_a, t_a, t_g, t_g, t_g))`,
  where `t_a` and `t_g` are `tuning.noise.accelerometer.sensitivityTolerance`
  and `tuning.noise.gyroscope.sensitivityTolerance`. It is added after the
  slope prior, as the last factor. It is the zero-mean prior on the departure
  from one, since `Vector6` retracts by addition.
- **Re-preintegration.** The pass structure, `runOptimizerPass` and the
  settled test are untouched. Because `linearizationOf()` carries the scale,
  the rebuild after each pass is at the pass's fitted bias and scale (clause
  19). The reported graph is that rebuild, with `S - s^ = 0`. The comments of
  `Stopping::repreintegrationCostDifference`, `FitResult::graph` and
  `fitFactorGraph` say "bias and scale". The rule text `bias not settled` is
  diagnostics contract and does not change.
- **The result.** `FitResult` gains `gtsam::Vector6 scale`: the fitted
  `S(0)`, or ones with the state off.
- **The residuals.** `collectResiduals` appends `{"scale_prior", 0,
  gnssTime[0], ...}` after `slope_prior`. The IMU kind stays `imu`, with
  dimension 9, and no prior enters a normalized RMS.
- `yawSigmaDeg` is unchanged. The initializer calls it on stock prefix
  graphs, which have no `S(0)`. On a graph that holds `S(0)`, `Marginals`
  marginalizes it like any variable (phase 4's heading check).

### The reconstruction: `trajectoryreconstruction.*` (clause 19; overview's "Phase 3 provides")

`reconstructInterval` preintegrates at the interval's bias and at
`fit.scale`. Its forward states are therefore `predict()` at the pim's own
linearization, as today. The readings at the edges become
`interpolateAt(force, e) ./ s_a - b_a`. `reconstructAtImuRate` publishes
`R (f ./ s_a - b_a) + g + (c_before + c_after) / 2`, where `f` is the
sample's own reading. The header comments state that formula. The transition
comes from `step.transition`. `kLimitations` becomes "... one linearized pass
with the fitted fix states, biases and scale factors held; no uncertainty is
published." (phase 4 removes the last clause).

### Diagnostics: `fusionoutput.cpp` (clause 21)

`modelSummary` adds `"scale": {"acc": [x, y, z], "gyro": [x, y, z]}` from
`fit.scale`, beside `gyro_bias` and `noise`. The values are the factors
themselves. Nothing else in the diagnostics changes. `seeds` and the failure
diagnostics stay as they are.

### What the other stages see

- **The initializer's fits are stock.** They have no `S(0)`, and their
  preintegration runs at unit scale, which is bit-identical to phase 2.
  `coarseAttitude`, `propagateAttitude`, `attitudesCarriedForward` and
  `gyroIncrement` read the raw readings and take no scale. The full fit
  starts `S(0)` at one.
- **The legacy correction is not the scale state.** The 1.14688 correction
  is applied by the conversion layer before the kernel (clause 23).
  `S(0)` is the unit's departure from the datasheet sensitivity, after it.
  Nothing in `src/fusion` names the correction. The lattice check reads the
  readings before any correction of the kernel's, the scale included
  (phase 2).

### Observability on the fixtures

The scale is weakly observable in most fixtures (probe):

- The gyro factors stay within 1e-5 of one, except `drifting_bias` z
  (1.0024). There the drifting gyro reading trades with the gyro bias.
- An accelerometer axis that sees no change in specific force has a zero
  Jacobian column or one confounded with its bias.
- The z accelerometer factor shares the z bias under gravity in proportion
  to their prior variances: `g^2 sigma_s^2 / (g^2 sigma_s^2 + sigma_b^2)`, which
  is 9.7 %. A fixture with `b_z = 0.08` fits `s_z = 0.9992` and `b_z = 0.072`.
  The golden biases move by that much.

A recording at rest leaves every factor within a tenth of its prior sigma,
from the same split (`rest_throughout`: largest departure 3.4e-4). The
documents say all of this (Documents).

### The fixture of clause 54: `tests/fusion/fusionfixtures.cpp`

`scale_recording` is served by `initializerFixture()`, whose header comment
becomes "the synthetic recordings of the kernel's model tests". It is not a
golden. It follows the generator's rules (only `+ - * /`, every value from its
index), and the period index comes from integer arithmetic:

- **Duration and rates.** 60 s. GNSS at 5 Hz, `t = .1 + j * .2`,
  `j = 0..299`. IMU at 25 Hz, `t = i * .04`, `i = 0..1500`. The stated
  configuration is +/-16 g, +/-2000 deg/s, 26 Hz for both sensors.
- **Motion.** Level, heading north, not rotating. North acceleration
  `a = A c x (1 - x)(1 - 2x)`, with `A = 5` m/s^2, `c = 10`, `P = 10` s and
  `x = tau / P`.
- **Period phase.** The period is `k = i / 250` and `tau = (i - 250 k) * .04`
  for the IMU. For GNSS, `k = (2 j + 1) / 100` and `tau = .1 + (j - 50 k) * .2`,
  which is exact because no fix falls on a period boundary.
- **Velocity and position.** `vN = 10 + A P c x^2 (1 - x)^2 / 2`.
  `pN = k (10 P + A P^2 c / 60) + 10 tau + A P^2 c (x^3/3 - x^4/2 + x^5/5) / 2`.
  East and down are zero. The acceleration has zero mean per period and is
  C1 across period ends. Its peak is 4.8 m/s^2, and velocity swings by
  15.6 m/s.
- **Readings.** Accelerometer `(1.02 (a + .05), -.03, -9.80665 + .08)`: only x
  is 2 % high. Gyro `(.2, -.15, .3)` deg/s.
- **Accuracies and noise.** hAcc 1, vAcc 1.5, sAcc .1. Noise: force .005,
  gyro .02 deg/s, position .2, velocity .03. Seed `0x8F050009`.
- **Rest.** Temperature 25, origin index 0. Readings rounded onto the lattice
  as phase 2 rounds every fixture.

The yaw is observable in the 60 s prefix, and the recording is one segment.
"The position misfit falls" is measured as `FitResult::positionRms`: the
vector RMS of the fitted position minus the GNSS measurement over the
window's fixes. Both fits run on the same window, from the same
initialization: the full fit with the state, then the full fit with it off.

### Documents (same change)

- **`docs/SENSOR_FUSION.md`, section 4.**
  - "The graph" gains the scale state: the six factors, the order, the prior
    and its Table 2 source (G_So%, applied to the accelerometer too, phase 2
    decision 5), started at one, full fit only.
  - "Integration and noise" adds that the readings are divided by the scale,
    and that the sampling term and the remainder are of the divided readings.
  - "Solver and stopping" adds that re-preintegration is at the fitted bias
    and scale, with the scale's first-order correction between rebuilds.
  - The reconstruction and "Acceleration" carry the formula with `f / s` and
    "the biases and scale factors held".
  - The diagnostics list gains `scale_prior` among the kinds, `model.scale`,
    and the new `limitations` text.
- **`docs/SENSOR_FUSION.md`, section 5.** Replace "a per-unit gyro scale factor
  is not fitted (the corpus shows one unit 2 % off nominal)" with what is
  fitted and what is not:
  - one constant factor per axis per fit;
  - the sensitivity's temperature change is not modelled (LA_SoDr +/-0.01
    %/degC, G_SoDr +/-0.007 %/degC, about 0.3 % over 30 degC);
  - there is no cross-axis sensitivity or misalignment;
  - without motion an axis stays at its prior;
  - the z accelerometer factor shares the z bias under gravity in proportion
    to their priors;
  - the unit of the reference corpus that is 2 % off nominal is fitted, with a
    1 % prior.
- **`docs/SENSOR_FUSION.md`, section 8.** The `tst_fusion_kernel` row names
  the scale tests. "Validating the reconstruction" says "biases and scale
  factors held". After the capture, re-read the measured numbers from the test
  logs and update them.
- **`docs/DATA_SCHEMA.md`, section 4.** Add one sentence: the correction
  restores the nominal sensitivity of legacy files and is not the fit's scale
  state, which is centred on one after it (link to `SENSOR_FUSION.md`
  section 4).
- **`tests/README.md`.** The section 1 row of `tst_fusion_kernel`. In
  section 11, the initializer table gains `scale_recording`, plus the
  history paragraph. Section 10, section 9.11 and appendix K are under
  Traceability.

### What must not change

- `Fusion::Algorithm` (`v6`) and the seventeen channels.
- `fitInputs()` and everything above the kernel.
- The record format.
- The optimizer and its settings (Decision 11).
- The initializer, the stopping rules and their texts.
- The bias priors, and the noise unit's figures.
- The observer's other members and call order.
- The bits of every unit-scale path.
- GTSAM stays out of the public files.

## Interfaces

### Provided (phase 4 consumes these names unchanged)

- **The graph variable** `S(0)`: a `gtsam::Vector6` holding accelerometer x,
  y, z, then gyro x, y, z. These are the factors themselves, started at one,
  in the full fit beside `B(0)` and `T(0)`. Its tangent is the plain
  difference (a vector space), so its covariance block is dimensionless, in
  that component order.
  - The fitted value is `FitResult::scale`, equal to
    `fit.values.at<gtsam::Vector6>(S(0))`.
  - Its prior is the last factor of `FitResult::graph`.
  - That graph is linearized at `fit.values` with every
    `ScaledImuFactor`'s `S - s^ = 0`.
- **The factor.** `ScaledImuFactor` (`src/fusion/scaledimufactor.h`) has keys
  in the order `X(k-1), V(k-1), X(k), V(k), B(0), T(0), S(0)`, and its
  Jacobians follow GTSAM's tangent conventions.
  - `H7` is `d error / d S`.
  - `scaleJacobian()` is `H_s`: rows theta, position, velocity of the
    preintegration's tangent (`TangentPreintegration`'s order); columns the
    six factors.
  - It also exposes `preintegratedMeasurements()`, `temperatureDelta()` and
    `linearizationScale()`.
- **The integration functions.**
  `preintegrateImu(const Samples &, double start, double end, const gtsam::imuBias::ConstantBias &bias, const gtsam::Vector6 &scale, const ImuNoise &noise, const ImuStepObserver &observer = ImuStepObserver(), gtsam::Matrix96 *scaleJacobian = nullptr)`.
  The unit scale is `gtsam::Vector6::Ones()`, and every stock caller passes
  it.
  - `ImuStep` gains `gtsam::Matrix9 transition`: the library's `A` for the
    step, valid whenever an observer is called.
  - Phase 4 may add the scale Jacobian at each edge to `ImuStep`, from the
    same accumulation. Phase 3 does not, because nothing reads it.
- **Acceleration.** The published acceleration is
  `R (f ./ s_a - b_a) + g + (c_before + c_after) / 2`. For phase 4's
  propagation, `d (f ./ s_a) / d s_a = -diag(f ./ s_a ./ s_a)`.
- **Diagnostics.** `model.scale` is `{"acc": [x, y, z], "gyro": [x, y, z]}`.
  The residual kind `scale_prior` (dimension 6) comes last. The IMU kind stays
  `imu`.
- **The fixture** `initializerFixture("scale_recording")`.
- **Audit and manual step.** The audit group `scale-state`; manual step M49.

### Consumed

From phase 2:
- `ImuNoise`, together with `SensorNoise::sensitivityTolerance`.
- `Tuning::noise`.
- `preintegrateImu`'s noise parameter and its loop's sampling term and
  remainder.
- `modelSummary`'s `noise`.
- The fixtures' stated configuration and lattice rounding.
- The explicit completeness list.

## Acceptance criteria

1. **The divided readings.** Preintegrating samples `f` at scale `s` equals
   preintegrating samples `f ./ s` at unit scale. `preintegrated()` agrees
   within 1e-12 relative, and `preintMeasCov()` and the last step's two
   sensor covariances within 1e-9 relative. This covers the readings, the
   turn, the sampling term and the remainder. At unit scale, asking for the
   Jacobian or passing an observer changes no bit of the result (clause 19).
2. **The Jacobian against central differences.** `scaleJacobian` agrees with
   central differences of `preintegrated()` (step 1e-6, all six factors, at
   `s` != 1, a non-zero bias, a turning and accelerating window) within 1e-7 of
   the differences' largest entry. Without the turn term it would not (clause
   19).
3. **The factor's Jacobians.** `ScaledImuFactor`'s seven Jacobians agree with
   numerical derivatives within 1e-6 at a point where every variable differs
   from the linearization. At `S = s^` the error and `H1`-`H6` equal
   `TemperatureImuFactor`'s on the same pim bit for bit, and `H7` is not zero
   (clauses 18, 48).
4. **The full fit's graph.** Per state, the full fit's graph holds the position
   factor, the velocity factor and a `ScaledImuFactor` with the keys above.
   Then come the bias prior and the slope prior, and last the scale prior:
   mean ones, sigmas the noise unit's tolerances. `values` holds `S(0)`. With
   the state off there is no `S(0)` and the IMU factors are
   `TemperatureImuFactor` (clauses 18, 48).
5. **Re-preintegration.** After a converged fit, every factor of
   `FitResult::graph` has `linearizationScale() == fit.scale` bit for bit.
   Its pim equals `preintegrateImu` at the interval bias and `fit.scale`.
   `stopping.rule` is `settled` (clause 19).
6. **The reconstruction.** On a hand-built fit whose second state is the
   forward integration of the readings divided by `s != 1`, the
   reconstruction with `fit.scale = s` has a mismatch at rounding, and with
   ones it does not. The published acceleration is
   `R (f ./ s_a - b_a) + g + correction` (clause 19).
7. **At rest.** On `rest_throughout` every fitted factor is within 0.1 x its
   tolerance of one (clause 20 as settled, 54).
8. **The scale recording.** On `scale_recording` the full fit converges, and
   the following hold (clause 54):
   - `|s_ax - 1.02| <= 0.01`;
   - `s_ay` and the three gyro factors are within 0.1 x tolerance of one;
   - `s_az` is within the tolerance of one;
   - `positionRms` is at most 0.9 times that of the fit with the state off,
     on the same window and initialization.
9. **Diagnostics.** Success diagnostics carry `model.scale` with `acc` and
   `gyro` arrays equal to `fit.scale`. The `model` keys are exactly
   `gyro_bias`, `noise` and `scale`. The last residual is `scale_prior` and
   the two before it are `bias_prior` and `slope_prior` (clause 21).
10. **The legacy correction.** The 1.14688 correction is applied as before
    (the existing conversion and runner tests). Nothing in `src/fusion`
    names it. The documents distinguish it from the scale state (clause 23).
11. **The pre-capture check.** Before the capture, with the switch off in
    `planFit` (a local edit, not committed), every existing golden test,
    exact mode included, passes against phase 2's capture. The implementer
    reports this.
12. **The capture.** The goldens are captured again: the three successes
    change, the `_time` columns are byte-identical, the eleven rejection
    goldens are byte-identical to phase 2's, and the history paragraph is
    written.
13. **The suite.** The suite is green in `build-agent/` (Release,
    sequentially). `audit_cleanup` is green with the list extended.

## Tests

### `tst_fusion_kernel`

- **Add:**
  - `preintegrationDividesByTheScale` (criterion 1).
  - `scaleJacobianMatchesCentralDifferences` (2). It uses a 100 Hz window
    turning at about 2 rad/s, as the probe's.
  - `scaleFactorJacobians` (3). It follows `temperatureFactorJacobians`.
    `gtsam/base/numericalDerivative.h` stops at six arguments, so bind six
    and use `numericalDerivative11` per block. Also check the clone and the
    whitened error through `Values`.
  - `scaleGraphShape` (4), with the state on and off.
  - `fitRepreintegratesAtTheFittedScale` (5).
  - `reconstructionUsesTheFittedScale` (6), in the pattern of
    `reconstructionUsesIntervalBias`.
  - `restLeavesTheScaleAtItsPrior` (7).
  - `scaleRecordingRecoversTheFactor` (8). It logs both fits' numbers with
    `qInfo`.
  - `diagnosticsReportTheScale` (9).
- **Amend:**
  - `fixtureFit` turns the state on as `planFit` does.
  - `heldEndsReference` and `predictFits` preintegrate at `fit.scale`, or at
    ones. The reference keeps `TemperatureImuFactor`, since `S` is held.
  - `imuFactorCovariance` also finds `ScaledImuFactor`.
  - `expectedAcceleration` divides by `s_a`.
  - Every `preintegrateImu` call passes the scale.
  - `diagnosticsReportTheNoiseModel`: the model keys gain `scale`.
  - `driftingBiasSegmentsConverge`: the last three residuals are the three
    priors in order.
  - `constantTemperatureKeepsSlopeAtPrior`: the pipeline run (state on) still
    checks `b1` at its prior. The objective comparison with the stock fit
    runs the full fit with the state off: the scale moves the objective by
    0.2 % (probe), so the constant-bias fit is reproduced only without it
    (item 246 as amended).
  - `initializerFixturesAreDeterministic`: the new name.
- **Must hold unchanged:**
  - `biasSettledByCostTest`: `coarse_maneuver` takes 2 passes with the state.
  - `slowTailAtTheIterationLimit`, `temperatureFactorJacobians`,
    `temperatureGraphShape`, `imuRate*`, and the initializer tests.

### Others

- `tests/fusion/fusionfixtures.*`: the new recording, and the header comment.
  `tst_fusion_golden`, `tst_fusion_session`, `tst_fusion_store`,
  `tst_fusion_jobs`, `tst_fusion_rows` and `tst_fusion_runner` have no code
  change: their goldens are captured again.
- `src/CMakeLists.txt`: `scaledimufactor.*` joins `flysight_fusion`.

### Goldens

Before capturing, run criterion 11's check. Then capture with the tool as
`tests/README.md` section 11 describes, run the `capture.json` gate step (the
generator's hash changes) and run both modes. The history paragraph is "The
capture of <date> (the documented noise model, part 1, phase 3)": the scale
state; the three successes changed, the z biases by about 10 %; the rejections
byte-identical; `_time` unchanged.

### Audit (`tests/audit/cleanup_audit.cmake`)

- **New group `scale-state`**, beside `noise-model`. Each rule gets an
  "Allow:" comment and is planted once to prove it:
  - `expect_none` of `1\.14688|kLegacyGyroScale` in `src/fusion`;
  - `expect_only` of `ScaledImuFactor` in `src`, allowed only in
    `^src/fusion/(scaledimufactor\.(h|cpp)|factorgraphfit\.cpp)$`;
  - `expect_none` of `scale factor is not fitted` in `docs`;
  - `expect_count` of `scale_prior` in `docs/SENSOR_FUSION.md` (as written)
    and of `model.scale`.
- `fusion-tooling`: `scaledimufactor` joins the internal-header pattern.
- `fusion-model`: the step-model rule's comment says that every step's
  transition is the library's `update()` on a copy, taken in
  `preintegrateImu()`.
- Traceability: append `1018 1019 1020 1021 1023 1048 1054` to the explicit
  list.
- `tests/README.md` section 10: a bullet for `scale-state`.

### Traceability

Map lines (`tests/acceptance_map.txt`, comment line first):

- 1018 `tst_fusion_kernel scaleGraphShape`, `scaleFactorJacobians`; `audit scale-state`
- 1019 `tst_fusion_kernel preintegrationDividesByTheScale`, `scaleJacobianMatchesCentralDifferences`, `fitRepreintegratesAtTheFittedScale`, `reconstructionUsesTheFittedScale`
- 1020 `tst_fusion_kernel restLeavesTheScaleAtItsPrior`; `manual M49`
- 1021 `tst_fusion_kernel diagnosticsReportTheScale`; `tst_fusion_golden successFixturesMatchGolden`; `manual M49`
- 1023 `tst_conversion_engine legacyGyroCorrected`; `tst_fusion_runner legacySchemaScalesTheGyro`; `audit scale-state`
- 1048 `tst_fusion_kernel scaleGraphShape`; `audit scale-state`
- 1054 `tst_fusion_kernel scaleRecordingRecoversTheFactor`, `restLeavesTheScaleAtItsPrior`

`tests/README.md` section 9.11 gets rows for the seven items. Its intro says
how clauses 18 and 20 are settled. Appendix K restates them "(as settled)":

- 18: "... with the datasheet's sensitivity tolerance (G_So%, +/-1 %, for both
  sensors: Table 2 states none for the accelerometer) as a zero-mean prior on
  its departure from one."
- 20: "A recording without motion leaves the scale at one within its prior:
  the data then constrain only the corrected reading along gravity, which the
  scale and the bias of that axis share in proportion to their priors'
  variances."

Amended items, each restated "(as amended)" in its appendix, its 9.x row and
the map comment, with each head paragraph naming the amendment:

- **246** (appendix C 46): "... a constant temperature leaves `b1` at its
  prior, and with the scale state of the specification of 1001-1065 off,
  reproduces the constant-bias fit." Evidence:
  `constantTemperatureKeepsSlopeAtPrior`.
- **901**: "... with the biases and scale factors the fit assigns to that
  interval: ... the reading, divided by the scale and bias removed, is
  rotated by it ...".
- **910**: "... the mean of the readings at its two edges, divided by the
  scale, bias removed ...".
- **914**: "The acceleration is the sample's own reading, divided by the
  fitted scale, bias removed, rotated ...". Evidence adds
  `reconstructionUsesTheFittedScale`.
- **925** (again, after phase 2): "the specification of 1001-1065 changes the
  noise model and adds the scale state to the graph; the pass still reads the
  converged solution (states, biases and scale factors) and changes nothing
  in it."
- **Searched and holding:**
  - 219 and 223: the initializer is stock, and the scale is the full fit's.
  - 213 and 227: the settled test is the same; the diagnostics gain a key.
  - 902, 903, 927: the transition is still the library's update, in one
    author.
  - 934: at unit scale.
  - 917: the texts are still quoted.
  - 847 and 861: still `v6`.
  - 239: roll and pitch at rest are unaffected.

### Manual step (`tests/README.md` section 12.9)

- **M49 The scale on the reference recordings (1020, 1021).** Run M47's
  commands.
  - Record per recording: `model.scale`, `stopping.rule`, the iterations per
    pass, `objective`, `quality`, and the fit time against the M47 run.
  - Record whether any pass settled with its first iteration's cost unchanged
    (phase 2 decision 7).
  - Reference: `11-17-12` is unit 014667, whose gyro the lab found 2-3 %
    above nominal (`experiments/fusion_lab/NOTES.md` 7.4). Its fitted gyro
    factors are expected to depart from one by about that much on the
    turning axes. `08-35-23` is from a unit found on nominal.
  - The numbers go in the report, not adjusted.

## Decisions

1. **The scale enters through a Jacobian accumulated in the loop.** It is
   built from the library's own per-step `A`, `B` and `C`, taken by
   `update()` on a copy, exactly as the reconstruction already takes `F_j`.
   The alternatives were worse:
   - finite differences: six extra preintegrations per factor per build;
   - folding the scale into the bias: impossible, since its input matrix
     differs per step;
   - an outer loop over the scale, like the lab's sweep: not a graph
     variable, so phase 4 could not take its covariance.
2. **The factor writes GTSAM's prediction chain out with `zeta`**, rather than
   subclassing the preintegration. That keeps it stateless and bit-identical
   to the temperature factor at its linearization.
3. **A new class.** `TemperatureImuFactor` stays as the scale-off factor, by
   decision 14's "the same fit with the state off". It is used by the
   comparison fit and the held-ends reference.
4. **A step's transition has one author.** Once the loop takes `A` for the
   scale, the reconstruction's own copy update would compute the same fact
   twice. The loop hands it over in `ImuStep::transition`, bit for bit.
5. **The sampling term and the remainder are of the divided readings.** The
   rule is that every term of a step is computed from what the step
   integrates, as the remainder already uses the bias-corrected readings. The
   densities are the datasheet's figures and are not divided. The scale's 1 %
   is below a typical figure's precision, and dividing would make the noise a
   function of the fitted state.
6. **The switch lives in `planFit`, not in `gyroBiasModelFor()`.** The
   temperature tests and their graphs stay as they are. The state requires the
   temperature model ("no generality nothing uses").
7. **Clause 18 as settled.** The accelerometer's tolerance is G_So%, the only
   tolerance row of Table 2 (re-read from the page images), as phase 2
   decision 5 chose. The prior is on the factor with mean one, which is the
   zero-mean prior on the departure.
8. **Clause 20 as settled.** "Leaves the scale at its prior" reads as "at one
   within its prior". At rest the bias and the scale of the gravity axis are
   separated only by their priors.
9. **The clause 54 recording.** A horizontal, zero-mean periodic
   acceleration on the scaled axis decouples the scale from that axis's bias.
   The GNSS velocity sees `0.02 A P c / 32` of misfit per period without the
   state: 0.31 m/s against sAcc 0.1. In the probe the prior's pull and the
   noise together leave 1.5 % of the 2 % departure. The thresholds (`<= 0.01`, `0.9 x`) are the clause's
   tolerance and a margin against the probe's 0.774.
10. **`kLimitations` names the held scale factors**, because the
    reconstruction now holds them. Phase 4 rewrites the rest.
11. **The optimizer is not changed.** `S(0)` adds a direction that the priors
    alone determine (the z factor against the z bias), not a stiffer block.
    The prior's information, 1e4, is far below the IMU blocks', about 1e9.
    In the probe no fixture's damping went above 1e-5, and no fit changed its
    stopping rule, so the saturation of phase 2 decision 7 is not more
    likely. M49 checks the reference recordings for it.
12. **The pre-capture check is procedural** (criterion 11), as phase 1's was
    sequencing. After the capture, phase 2's numbers are gone, so a committed
    test could not compare against them. Criterion 1's unit-scale invariance
    stays as the permanent guard.

## Probe (`experiments/scale_probe/`)

The probe is built against the third-party installs, in its own `build/`. It
reuses phase 2's step model from `experiments/noise_probe/`, and adds the
scale in a copy of `imuintegration.cpp` and `factorgraphfit.cpp`, plus
`scaledimufactor_probe.h`. Outputs are in `run1_jacobians.txt`,
`run2_scale_recording.txt` and `run3_fixtures.txt`.

- **The scale Jacobian.** Against central differences it agrees to 3.9e-10
  relative at step 1e-6. Without the turn term the gyro columns are off by
  1.26e-2 relative.
- **The factor.** `H1`-`H7` against numerical derivatives agree to under
  9e-11. At `S = s^` the error and `H1`-`H6` are bit-identical to
  `TemperatureImuFactor`'s with GTSAM's association; with the other
  association `H5` and `H6` differ by 4e-16. The first-order correction over
  a 1 % scale step misses re-preintegration by 6e-4 against a correction of
  0.066, which is why re-preintegration is needed.
- **The scale recording (as specified).**
  - With the state: `settled`, 3 passes, 8 iterations; `s_ax` 1.01969
    (error 3.1e-4, 0.03 of the tolerance); `s_ay` 1.00000, `s_az` 0.99922,
    gyro 1.00000; `positionRms` 0.195, velocity RMS 0.030.
  - Without the state: `positionRms` 0.252 (ratio 0.774), velocity RMS
    0.112.
  - With `A = 2`: `s_ax` 1.0186, ratio 0.96.
  - With `P = 4` over 30 s: 1.0173, ratio 0.985. These variants are weaker,
    hence the specified 5 m/s^2, 10 s and 60 s.
  - At 12.5 Hz: 1.0195, ratio 0.78.
  - With a true factor of one: 0.99995.
- **The fixtures.** Every fixture with the state on converged `settled`, as
  with it off: `coarse_linear`, `coarse_maneuver` (2 passes, 6 iterations
  against 1 and 4), `stationary_spin`, `motion_start`, `rest_throughout`
  (15 iterations against 12), and `sacc_anchor` and `drifting_bias` at
  12.5 Hz.
  - Every factor is within 1e-3 of one, except `drifting_bias` gyro z
    (1.0024).
  - `s_az` is about 0.9992 wherever `b_z` is 0.08.
  - The largest last damping was 1e-5, and there were at most 2 no-move
    iterations per fit.

Ready with caveats: Decisions 4 (the reconstruction reads the loop's transition), 5 (the step terms are of the divided readings), 7 and 8 (clauses 18 and 20 as settled) are flagged for Michael. M49's expectations for the reference recordings are the lab's, not measured on this kernel.
