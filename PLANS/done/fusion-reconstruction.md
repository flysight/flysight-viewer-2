# The fused state at every IMU sample

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

## 1. Motivation

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

## 2. Principles

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

## 3. Scope

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

## 4. Terms

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

## 5. The reconstruction

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

## 6. What is published

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

## 7. The diagnostics

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

## 8. The record, the version and the goldens

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

## 9. Architecture

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

## 10. Tests

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

## 11. Documentation

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
