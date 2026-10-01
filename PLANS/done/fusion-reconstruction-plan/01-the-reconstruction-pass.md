# Phase 1: The reconstruction pass

## Purpose

Spec §5 and the kernel half of §9: the one pass over every fix interval of a
fitted window that turns the fitted states at the fixes into the IMU-rate
posterior, as a kernel function nothing publishes yet. It reads its step
model from the fit's own preintegration through a per-step observer on the
one loop of `preintegrateImu()`, and computes forward states, the mismatch,
the coordinate-mapped sharing, corrected states, step corrections, the
published acceleration of §6 and the four summaries of §7. Its §10 tests
(equivalence, the ends, sharing by noise, zero mismatch, consistency) run in
`tst_fusion_kernel`.

It is its own phase so that the pass is proven against the probe's findings
while every golden still holds the fit bit for bit (decision 1). Nothing a
user or a golden sees changes; phase 2 switches `run()` to the pass.

## Dependencies

- Depends on nothing; blocks phase 2, which blocks phase 3.
- At the start `DenseTrajectory` / `reconstructTrajectory()` publish the
  fusion channels, `Fusion::Algorithm` is `batch-temperature-bias-v3`, and the
  goldens in `tests/data/fusion/` are that kernel's.
- The probe (`experiments/reconstruction_probe/`, local and ignored) is the
  source of the numbers below and a working example of every piece
  (`forwardPass()`, `share()`, `correct()` form b, `assemble()`,
  `stepCorrectionsT()`, `publish()`, `solveDenseConditional()`,
  `consistency()`, `tumble()`). Product code and tests are written in the
  kernel's style and do not include, link or copy it; its copies of
  `stepSigma()` / `stepCovariance()` and its own `integrateMeasurement` loop
  are exactly what this phase must not do.

## What changes

### The per-step observer (`src/fusion/imuintegration.h`, `.cpp`)

`preintegrateImu()` gains an optional per-step observer (decision 4). Its
name and type are the implementer's; its contract:

- It is called once per step, before that step's `pim.integrateMeasurement(`,
  with read-only access to the preintegration after the steps before (so
  `preintMeasCov()`, `preintegrated()` and `deltaTij()` are edge j's) and with
  what the step integrates: its edges (or `dt`) and the two midpoint readings
  passed to `integrateMeasurement`. The returned `pim` is edge n.
- The order stays: both covariances written into the shared params, then the
  one `.integrateMeasurement(`. The observer runs before the call, and can
  modify neither `pim` nor the params.
- With no observer (the default; every present caller passes none) the
  function is bit-identical to today, which the exact golden tests prove.
  With one, the returned `pim` is the same bits too, which the ends test
  proves through P_n.
- The header comment states this contract and why it exists: the
  reconstruction reads F_j and P_j from the fit's own step instead of
  restating it.

`stepSigma()` and `stepCovariance()` stay private; nothing else changes.

### The pass (`src/fusion/trajectoryreconstruction.h`, `.cpp`)

`ImuRateTrajectory` and `reconstructAtImuRate()` (Interfaces) are added beside
the present reconstruction, which stays untouched. The file and struct
comments state the contract. Per fix interval k (fix k to fix k+1):

1. **Bias and start** (decision 5): `intervalBias(window, k, B(0), fit.gyroBiasSlope, fit.biasModel)`
   of `fit.values`, and the fitted state at fix k as a `gtsam::NavState` from
   `X(k)`, `V(k)`. The tuning is the caller's (the one `planFit()` made,
   `maxGap` included).
2. **Forward pass** through `preintegrateImu(..., observer)`. At edge j: the
   forward state `pim.predict(fitted_k, bias)`, P_j = `pim.preintMeasCov()`,
   and F_j, the `A` of `update()` on a copy of the preintegration with the
   step's raw midpoint readings and `dt` (`auto c = pim; c.update(...)`), not
   `TangentPreintegration::UpdatePreintegrated` and not a hand-written
   Jacobian. Q_j is never formed.
3. **Mismatch** (decision 2): d = `forward_n.localCoordinates(fitted_(k+1))`,
   ordered attitude, position, velocity.
4. **Sharing** (decisions 3, 4): M_j is the `H2` Jacobian of
   `NavState::retract` at `fitted_k` for the tangent `predict()` retracts by
   at edge j (`fitted_k.correctPIM(pim.preintegrated(), pim.deltaTij(), ...)`
   with the params' gravity and Coriolis settings); only that call's Jacobian
   is used, the state stays `predict()`'s. Backward: λ_n = P_n⁻¹ M_n⁻¹ d,
   λ_j = F_jᵀ λ_(j+1), δξ_j = P_j λ_j.
5. **Corrected states**: `forward_j.retract(M_j δξ_j)` for j < n, and
   `forward_n.retract(d)` at the second fix, from d itself, so the end is
   exact whatever the rounding of the solve.
6. **Step corrections** (decision 6), for the step from edge j to j+1:
   c_j = (v_(j+1) − v_j)/Δt_j − ½[R_j (f(e_j) − b_a) + R_(j+1) (f(e_(j+1)) − b_a)] − g,
   corrected v and R, f(e) = `interpolateAt(window.imuTime, window.force, e)`
   (the sample's own reading at a sample edge), b_a the interval bias's
   accelerometer part, g = `kGravity`.

Then over the window:

- **Time axis**: the window's IMU samples in `[first fix, last fix)`, each
  at its edge in the interval whose `[t_k, t_(k+1))` contains it (the lower
  bound of its time in that interval's edges, as `reconstructTrajectory()`
  finds it). A sample exactly on fix k is published once, at edge 0 of
  interval k. An interval with no sample inside is still passed over; its
  mismatch and step corrections count in the summaries.
- **Published values**: rotation, position and velocity are the corrected
  state at the sample's edge; a_i = R_i (f_i − b_a) + g + (c_before + c_after)/2
  with f_i = `window.force[sample]`. c_before and c_after are the steps on
  either side of the sample's edge in the window's step sequence (the
  intervals' steps concatenated): the part-step from a fix to the next
  sample belongs to the interval it starts; a sample on fix k takes
  c_before from the last step of interval k−1. Only a sample on the window's
  first fix has one adjacent step and takes its correction alone.
- **Summaries**: `maxEndpointCorrectionDeg`, the largest norm of d's attitude
  part in degrees; `maxVelocityMismatch`, the largest norm of d's velocity
  part; `maxStepCorrection`, the largest |c_j| over the window's steps, and
  `maxStepCorrectionTime`, that step's midpoint in window time (seconds since
  the epoch, like `start_s`). The first maximum wins a tie.

The pass has no checkpoint (audit "three kinds of boundary"; decision 9),
does not log, does not revalidate the window, and lets what
`preintegrateImu()` throws propagate. Nothing in `fusion.cpp` calls it.

**The per-interval seam.** The tests need, for one interval, the edges, the
forward and corrected states at every edge, P_n, d and the step corrections
(enough to compute decision 8's bound). Its name and shape are the
implementer's (a struct and a function in `trajectoryreconstruction.h` is the
obvious home), but `reconstructAtImuRate()` must be built from it, so that
the tests examine what is published. A new header under `src/fusion`, if
any, joins `fusion-tooling`'s pattern and its source joins `flysight_fusion`
in `src/CMakeLists.txt`.

### What must not change

`reconstructTrajectory()`, `DenseTrajectory`, `fusion.cpp`, `fusionoutput.*`,
`fusion.h` (`Algorithm` included), `factorgraphfit.*`,
`temperatureimufactor.*`, the registration, `tests/data/fusion/`, the three
tests that call `reconstructTrajectory()`, the other test executables,
`docs/`, `tests/acceptance_map.txt`, and `tests/README.md` outside the one
section-1 row (decisions 15, 16).

## Interfaces

### Provided to phase 2

In `src/fusion/trajectoryreconstruction.h`, namespace
`FlySight::Fusion::Detail`, beside the present reconstruction, exactly:

```cpp
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

Default member initializers on the four doubles are allowed; nothing else
may differ. Everything but the four summaries aligns with `time`. The
per-interval seam and the observer are phase 1's; phase 2 uses neither, but
may keep using phase 1's test helpers.

### Reported for phase 2

The implementation agent's report names the files changed and:

- every test function added, with the §10 bullet each covers;
- the agreement measured by the equivalence tests, on `coarse_maneuver` and
  on the rotating recording: the largest attitude (degrees), velocity (m/s)
  and position (m) difference from the reference over the published
  samples, each also relative to the largest mismatch of its kind, and those
  mismatches; for the rotating recording also the attitude term its absolute
  bound covers. The tests print these with `qInfo()`, as
  `reconstructionUsesIntervalBias` does;
- per success fixture, the worst consistency ratio (error over bound) and
  the whole-axis error; phase 2 writes these and the equivalence numbers
  into `docs/SENSOR_FUSION.md` section 8;
- the audit rule added.

## Acceptance criteria

1. `ImuRateTrajectory` and `reconstructAtImuRate()` exist exactly as under
   Interfaces, and nothing in `src` calls `reconstructAtImuRate()`.
   (Decision 1.)
2. `preintegrateImu()` holds the only `.integrateMeasurement(` in `src`,
   writes both covariances before it, and gives the observer no way to
   modify the preintegration or the params. (§2 "One authority for the
   integration", §9, decision 4.)
3. The pass forms no Q_j or step covariance, calls neither
   `UpdatePreintegrated` nor `integrateMeasurement`, takes F_j only from
   `update()` on a copy, and takes forward states from `predict()`. (§9,
   decision 4.)
4. Every golden test passes in both modes on `build-agent/`, the seven
   `*_exact` tests included, and nothing under `tests/data/fusion/` changes.
   (Overview constraints; decision 1.)
5. On every committed success fixture, for every interval: the corrected
   state at the second fix equals the fitted state to rounding (attitude
   within 1e-12 rad; velocity and position within 1e-12 × (1 + the norm));
   P_n is bit-identical (`==`) to `preintMeasCov()` of the interval's IMU
   factor in `FitResult::graph`, found by its keys `X(k)`, `X(k+1)`. (§10
   "The ends", decision 5.)
6. On every committed success fixture `time` is exactly the window's IMU
   samples in `[first fix, last fix)`; on a synthetic recording with a
   sample exactly on a fix, that sample is published once, with the fitted
   state there to the tolerance of criterion 5. (§3, §6.)
7. On `coarse_maneuver`, against the held-ends dense graph (decision 7),
   the attitude (angle, rad), velocity and position at every published
   sample are within 1e-5 × the largest mismatch of that kind over the
   intervals + 1e-12. (§10 "Equivalence", decision 7.)
8. On the rotating recording, against the same kind of reference: velocity
   and position within the same formula; attitude within an absolute bound
   the test states from its measurement (Tests). (§10; decisions 3, 7.)
9. With one step carrying more noise, the increment of the velocity
   correction across it exceeds that across every other step of the
   interval; with uniform noise the velocity correction increases strictly
   with elapsed time and is proportional to it within the tolerance the test
   states. (§10 "Sharing by noise", §5 "Sharing".)
10. With a zero mismatch and no rotation, corrected states equal forward
    states, every c_j is zero, and the published acceleration is
    R_i (f_i − b_a) + g, to a rounding tolerance the test states. (§10
    "Zero mismatch", §5, decision 6.)
11. On every committed success fixture, for every run of 1, 2, 5 and 20
    sample steps and for the whole axis, the published acceleration
    integrated by the kernel's rule matches the published velocity change
    within 1.01 × decision 8's bound + 1e-12 m/s. (§10 "Consistency", §6;
    decisions 6, 8.)
12. On `coarse_maneuver` the four summaries equal, bit for bit, the maxima
    computed from the per-interval seam. (§7, decision 10.)
13. The audit passes with the rule this phase adds (Tests), and the
    `tst_fusion_kernel` row of `tests/README.md` section 1 names the new
    coverage.

## Tests

All in `tests/tst_fusion_kernel.cpp` (decision 13). Test names are the
implementer's; the report states them.

- **A fixture's fit, computed once per run.** A helper that fits a fixture
  through the internal seams in the order of `planFit()` and
  `fitAndAssemble()`: `prepareInput`, the derived `maxGap`, `fittedWindow`,
  `validateSamples`, `gyroBiasModelFor`, `initialize`, and `fitFactorGraph`
  with `kFullFitPassFormat` and that model (the end of
  `constantTemperatureKeepsSlopeAtPrior`; `windowOf()` and `pipelineTuning()`
  do the first half). It returns window, tuning and fit and requires
  convergence; the fits dominate the executable's time, so each is made once.
- **The reference graph** (decision 7), a helper: a state at every edge; per
  step one IMU factor preintegrated by
  `preintegrateImu(window, e_j, e_(j+1), intervalBias_k, tuning)`, which over
  one step gives the loop's own midpoint reading and per-step covariance
  without restating either; `TemperatureImuFactor` with interval k's `dT`
  under the temperature model, `gtsam::ImuFactor` under the constant one;
  the fix states and `B(0)` (and `T(0)`) held at the fit's values by
  `gtsam::NonlinearEquality`; started from the forward states; solved by
  `LevenbergMarquardtOptimizer` with `MULTIFRONTAL_QR` to a tight settle (3
  to 4 iterations in the probe).
- **Equivalence, `coarse_maneuver`** (criterion 7). Probe: 3.6e-12 degrees,
  2.8e-13 m/s, 2.9e-14 m, against largest mismatches 1.1e-4 degrees,
  9.1e-5 m/s, 9.1e-6 m.
- **Equivalence, rotating** (criterion 8). The probe's `tumble()`: 3 rad/s
  about a horizontal axis, IMU 25 Hz, GNSS 5 Hz from 0.013 s, 6 s, exact
  readings, the true states as a hand-built `FitResult`, zero bias, the
  constant model. Probe: velocity 1.0e-9 m/s against |d_v| = 0.11 m/s (the
  literal formula of decision 3: 1.7e-2); attitude 2.0e-6 degrees, which is
  multi-step against chained one-step preintegration, not linearization
  (|d_att| is 6.5e-14 degrees). The attitude bound is absolute, stated from
  the measurement at least ten times above it and ten times below the
  1.2e-3 degrees the literal formula gives, and the comment says why.
  The same recording is also run at 13 Hz (FlySight's default IMU rate) and
  at 100 Hz, and for each rate the test prints, with `qInfo()`, the largest
  difference between the published acceleration and the true one, and the
  same for the rotated reading alone (FINDINGS Q4's table, with the 13 Hz
  row added); phase 2 documents these numbers.
- **The ends** (criteria 5, 6, 12) on the three fixtures, plus a small
  recording with dyadic times so that a sample falls exactly on a fix.
- **Sharing by noise** (criterion 9). One interval, no rotation, identity
  attitude (velocity local coordinates are then NED), a hand-built
  `FitResult` whose second state is the forward end plus a vertical velocity
  offset Δv and position offset Δv T/2: a constant acceleration error, for
  which the velocity correction is Δv (t − t_0)/T, vertical so that tilt does
  not couple. Uniform: `boundarySamples(Vector3::Zero())`; the integration
  covariance makes proportionality approximate, and the test states its
  tolerance from the measurement. Noisy step: the force changed from one
  sample on, with an accelerometer slope (`withSlopes()`) large enough that
  the step's variance is several times its neighbours'.
- **Zero mismatch** (criterion 10). Several intervals, a constant non-identity
  attitude, zero gyro and gyro bias, a non-zero accelerometer bias, a force
  that changes piecewise-linearly, fitted states that are the forward
  predictions interval by interval (d is zero). The comment records decision
  6's caveat: under rotation a zero mismatch leaves c_j at minus the rotation
  lag, which is why this recording does not rotate.
- **Consistency** (criterion 11). The kernel's rule is the midpoint of the
  piecewise-linear published acceleration (`interpolateAt()` over `time` and
  `acceleration`). The bound from sample a to b is decision 8's:
  ¼Δt_a |c_before(a) − c_after(a)| + ¼Δt_b |c_after(b) − c_before(b)|, plus
  ½Δt × the largest difference between the part-step corrections of every
  sample step in the run that contains a fix. Probe: ratio at most 1.000008;
  whole-axis error at most 2.1e-5 m/s (`coarse_maneuver` 1.9e-6).

The initializer recordings are not required (their fits cost seconds each).

**Audit** (`tests/audit/cleanup_audit.cmake`, group `fusion-model`), one rule
beside "one integrateMeasurement, per-step covariance", with a comment in the
file's style: the step model has one author. Pattern
`[.>]integrateMeasurement\(|UpdatePreintegrated|(accelerometer|gyroscope|integration)Covariance`,
allowed only in `^src/fusion/imuintegration\.cpp$` and `^tests/README\.md$`
(which spells the call), searched over `src` and `tests`. It guards §2
"Nothing restates the step model": no other kernel file and no test, the
reference graph included, integrates an IMU step or sets a per-step
covariance, and F_j comes from the library's `update()`, not the static
tangent update. The count rule stays; the group header's items and
`tests/README.md` section 10 are phase 3's.

**Documentation.** The `tst_fusion_kernel` row of `tests/README.md` section 1
gains a clause: the IMU-rate reconstruction pass (equivalence with the
held-ends dense graph on `coarse_maneuver` and a rotating recording, exact
ends and P_n equal to the factor covariance, sharing by noise, zero mismatch,
consistency of the acceleration with the velocity). No `docs/` change, no
acceptance-map line.

**Run.** `cmake --build build-agent --config Release`, then the whole suite
with `ctest --test-dir build-agent/FlySightViewer-build -C Release --output-on-failure`,
sequentially, after checking for stray `ctest` / `tst_*` processes.

## Decisions

- **One observer call per step, before it.** Edge j is the pim the observer
  receives for step j and edge n the returned pim, so one callback covers
  everything, and before the call is where F_j must be taken.
- **M_j from the library's retract**, not the closed form blockdiag(Dexp(θ),
  Exp(θ)ᵀ, Exp(θ)ᵀ) the probe derived: the chart stays the library's.
  `predict()` still makes the state, since a retract that also computes a
  Jacobian is not promised to give the same bits.
- **The end is `forward_n.retract(d)`**, not `M_n δξ_n`: §5 requires the
  fitted state exactly.
- **§6's "first and last sample"** is read as the samples at the window's
  first and last edge. The last fix is never published, so only a sample
  exactly on the first fix takes one correction (as the probe's `publish()`).
- **Committed success fixtures** are the three golden fixtures with
  `expectSuccess`, as `fitTraceMatchesGolden_data` enumerates them.
- **The rotating recording** is the probe's harshest tumble, because
  `coarse_maneuver` barely rotates and cannot tell the mapped from the
  literal sharing at the stated tolerance (decision 7).
- **The audit rule is added now**, because this phase adds the second reader
  of the step model; building the reference graph from one-step
  `preintegrateImu()` calls is what lets the rule cover `tests`.
- A fix interval whose preintegrated rotation nears a full turn makes M_n
  singular; the preintegration degrades there first, far beyond what the fit
  handles, and the pass does not special-case it.
