# Phase 4: The accuracy

## Purpose

The specification's section 7, and the parts of sections 9 and 10 it implies.
After convergence the fit computes its covariance once, composes it at every
IMU sample through the reconstruction's pass, and publishes four new channels
of the fit: the heading, tilt, horizontal-acceleration and
vertical-acceleration accuracies, each widened where the local residuals
exceed the model. The scale sigmas and an `accuracy` account join the
diagnostics, and the goldens are captured for the last time (decision 12).
This is one phase because the channels, the record, the goldens and the
documents all change together.

The technical core is the covariance step: the method, its cost and a heading
that the data does not determine. All three were settled by a probe on the
real kernel; its findings are at the end. In short:

- **Method.** One multifrontal QR factorization in the chain ordering,
  followed by the Bayes tree's clique marginals, top-down.
- **Covariance-form recursion.** The obvious alternative, the Takahashi
  selected inverse, was measured unstable. On a long chain with 1 Hz fixes
  its error reaches 1e4 to 1e18.
- **Heading.** A 1000 rad heading prior is added for the factorization only.

Clauses owned: 22, 25-36, 42, 44, 49, 56-60 (items 1022, 1025-1036, 1042,
1044, 1049, 1056-1060).

- **Restated "(as settled)":** 25 and 35 (Decisions 1, 2, 5).
- **Restated "(as amended)":** 230, 235, 337 and 912 (Traceability).

## Dependencies

**Depends on phase 3.** At the start:

- The full fit holds `S(0)` (`gtsam::Vector6`: acc x, y, z, then gyro x, y,
  z) beside `B(0)` and `T(0)`. Its prior is the last factor of
  `FitResult::graph`, and that graph is linearized at `fit.values` with
  every `ScaledImuFactor`'s `S - s^ = 0`.
- `FitResult::scale` holds the fitted scale.
- The integration function has this signature:

  ```cpp
  preintegrateImu(samples, start, end, bias, scale, noise, observer, gtsam::Matrix96 *scaleJacobian)
  ```

- `ImuStep::transition` is the step's `A`.
- The reconstruction preintegrates at `fit.scale` and publishes
  `R (f ./ s_a - b_a) + g + (c_before + c_after) / 2`.
- `Tuning::noise.accelerometer.sampleSigma` exists (phase 2).
- The diagnostics carry `configuration`, `model.noise` and `model.scale`
  (`acc`, `gyro`). The residual kinds end with `bias_prior`, `slope_prior`
  and `scale_prior`.
- `kLimitations` ends "...fix states, biases and scale factors held; no
  uncertainty is published."
- `Fusion::Algorithm` is `batch-temperature-bias-v6`.
- There are fourteen golden fixtures, plus
  `initializerFixture("scale_recording")`.
- The audit groups `noise-model` and `scale-state` exist. The explicit
  completeness list ends at phase 3's items. M47-M49 are in section 12.9.

**Blocks phase 5.** Phase 5 registers four plots and a column over
`Fusion/headingAcc`, `Fusion/tiltAcc`, `Fusion/accHAcc` and `Fusion/accDAcc`
(Interfaces).

## What changes

### The covariance step: `src/fusion/fitcovariance.h` / `.cpp` (new; clauses 25, 27, 49)

This is a unit of its own, as the reconstruction is. It is internal to the
kernel, and GTSAM is allowed in it. The class or namespace comment states the
contract below.

- **Input.** It takes the converged `FitResult` (graph, values, `N` states).
- **Output.** It returns, for every fix `k`:
  - `Σ_k`, the 9x9 covariance of the fix state, in the graph's tangent: the
    `Pose3` tangent (rotation, translation, both body frame), then `V(k)` in
    the navigation frame;
  - `Σ_k,k+1`, the 9x9 covariance of the adjacent pair;
  - `Σ_k,g`, the 9x15 cross-covariance with `g = (B(0) 6, T(0) 3, S(0) 6)`;
  - `Σ_gg`, 15x15, once.
- **Status.** It also reports whether the covariance was computed and, if
  not, why.
- **The method** (Decision 1):
  - linearize `fit.graph` at `fit.values`, without damping;
  - add the heading prior (below) to the linear graph only;
  - eliminate once by multifrontal QR in the ordering `X(0), V(0), X(1),
    V(1), ..., X(N-1), V(N-1), B(0), T(0), S(0)`. The Bayes tree is then a
    path of cliques `(x_k | x_k+1, g)` under a root that holds
    `(x_N-1, g)`;
  - visit the cliques top-down (root first), so that every
    `separatorMarginal` finds its parent's cached and recursion stays one
    level deep;
  - for each clique, take its marginal (`marginal2(EliminateQR)`), factor
    that 33-dimensional graph by QR, and form `R^-1 R^-T`. That is the joint
    covariance of `(x_k, x_k+1, g)`, which contains all four blocks;
  - call `deleteCachedShortcuts()` and discard the tree.
- **What it never calls.** No `Marginals` (constructing one is a second
  factorization) and no `jointMarginalCovariance`.
- **The heading prior** (Decision 2):
  - a 1x6 linear factor on `X(0)`, `(e_D^T R_0, 0, 0, 0) / σ_h`, where `R_0`
    is `X(0)`'s rotation;
  - `σ_h = 1000` rad, a named constant of the unit;
  - it is added to the linear graph only, never to `fit.graph`, and the fit
    is unchanged.
- **Failure** (clause 34). The step has failed in these cases:
  - linearization or elimination throws a `std::exception`. `std::bad_alloc`
    still propagates, as `run()` promises;
  - any covariance block is not finite;
  - a node's diagonal entry is not positive.

  A failure is a result, not a thrown error. Its fixed text is
  `covariance unavailable: the factorization of the converged graph failed`.
  A rank-deficient heading never fails, because the prior makes it finite.
- **Not a boundary** (clause 27). It runs inside `fitAndAssemble` after
  convergence, in the same job, and calls no `checkpoint(`: the count stays at
  3. Update `run()`'s comment in `fusion.h`, which says the work after the last
  iteration is not a boundary, to name the covariance step and the composition
  with the reconstruction.
- **The cap.** Both caps (the heading check's and the accuracies') use one
  constant, 180 deg. Move `kYawSigmaCapDeg` out of `factorgraphfit.cpp`'s
  anonymous namespace into `factorgraphfit.h`.

### The composition at every IMU sample: `trajectoryreconstruction.*` (clause 26)

The reconstruction's pass alone holds `P_j`, `F_j`, `M_j` and the sharing, so
the per-sample covariance is computed there, in the same loop that publishes
the sample. `reconstructAtImuRate` takes the fit's covariance. When it is
present and computed, it returns the four unwidened accuracies per sample
alongside the states.

**Bits must not change.** The published states and accelerations stay
bit-identical. Every new Jacobian comes from a separate call, as `M_j`
already does ("a retraction that also computes a Jacobian is not promised to
give the same bits").

**The interval's quantities.** For interval `k` (fix `k` to fix `k+1`), edges
`j = 0..n`:

- **`P_j`, `F_j`, `M_j`, `P_n`, `d`:** the reconstruction's own. `d` is
  `forward_n.localCoordinates(fitted_k+1)`.
- **`D_1`, `D_2`:** the two Jacobians of `localCoordinates` with the same
  arguments, taken by a separate call so that `d` keeps its bits.
- **`Ψ_j`, `Ψb_j`:** from `pim.predict(start, bias, Ψ_j, Ψb_j)` at edge `j`.
  They are d forward_j / d start (NavState tangent) and d forward_j / d
  interval bias.
- **`Hs_j`:** phase 3's scale Jacobian accumulated up to edge `j`.
  `ImuStep` gains it as the Jacobian at the step's first edge, which phase 3's
  Interfaces allowed. The last edge takes the `scaleJacobian` out-parameter.
  Asking for it changes no bit (phase 3 criterion 1).
- **`ΔT_k`:** `T(fix k) - T_ref`.

**The composition.** Write it so that it reads as the probe's:

```text
Φ_j = F_(n-1) ... F_j,  Φ_n = I                    (step chain transition, edge j to the fix)
G_j = P_j Φ_j^T                                     Cov(ζ_j, ζ_n) of the step chain
K_j = M_j G_j P_n^-1 M_n^-1                         the sharing: the correction at edge j is K_j d
C_j = M_j (P_j - G_j P_n^-1 G_j^T) M_j^T            conditional covariance given both ends
J^k_j   = Ψ_j  + K_j D_1 Ψ_n                        w.r.t. fix k's NavState tangent
J^k+1_j = K_j D_2                                   w.r.t. fix k+1's NavState tangent
J^b_j   = Ψb_j + K_j D_1 Ψb_n                       w.r.t. the interval bias (acc, gyro)
J^s_j   = M_j Hs_j + K_j D_1 M_n Hs_n               w.r.t. S(0)
J_j = [ J^k_j N_k | J^k+1_j N_k+1 | J^b_j | J^b_j[:, gyro] ΔT_k | J^s_j ]     9 x 33
      N_k = diag(I_3, I_3, R_k^T)  (graph tangent -> NavState tangent: V(k) is NED, NavState's velocity body)
Σ_j = J_j Σ_z J_j^T + C_j,   Cov(x_j, g) = J_j Σ_z,g
      Σ_z: the 33x33 joint of z_k = (x_k, x_k+1, B, T, S) from the covariance step
```

**Why the sum holds.** It is the law of total variance. Given `z_k`, the
state at an interior edge depends only on the interval's IMU chain (the
Markov property), so the two terms are independent.

**The tangent.** `Σ_j` is in the tangent of the forward state at the edge.
To first order in the correction, which is small, that is the tangent of the
published (corrected) state.

**What is needed of `Σ_j`.** Only the attitude rows of `J_j`: the attitude
block, and the attitude rows of `Cov(x_j, g)` against `B(0)`'s and `S(0)`'s
accelerometer parts.

**At a fix.** A sample on fix `k` has `P_0 = 0`, so `C_0 = 0`, `K_0 = 0` and
`Ψ_0 = I`. Its `Σ_j` is `Σ_k`.

**Attitude** (clause 28). `Σ^n = R_j Σ_j[φ,φ] R_j^T`, where `R_j` is the
published attitude. `R_j Exp(φ)` is a right perturbation, as in
`yawSigmaDeg`.

- heading = `sqrt(Σ^n(D,D))`;
- tilt = `sqrt(Σ^n(N,N) + Σ^n(E,E))`;
- both in degrees and capped at 180. A non-finite or negative variance reads
  as 180.

**Acceleration** (clauses 29, 35). Let `u = f ./ s_a - b_a`, with `f` the
sample's own reading, and `σ = R_j u` (specific force, NED). The first-order
error of `a = R (f / s - b) + g` is:

```text
δa = -[σ]x φ^n - R_j δb_a - R_j diag(f_i / s_i^2) δs_a,     φ^n = R_j φ  (navigation-frame attitude error)
Σ_a = L Σ_q L^T + σ_acc^2 I_3,    L = [ -[σ]x | -R_j | -R_j diag(f_i / s_i^2) ]
```

- `Σ_q` is the 9x9 joint of `(φ^n, B(0)_acc, S(0)_acc)`.
- `σ_acc` is phase 2's `accelerometer.sampleSigma`. It is isotropic, so the
  rotation leaves `σ_acc^2 I_3`.
- **The heading enters at most at the cap.** Where
  `Σ_q(D,D) > π^2`, row and column `D` of `Σ_q` are multiplied by
  `π / sqrt(Σ_q(D,D))`. That bounds what an undetermined heading can put into
  the principal-value fallback below.
- **Horizontal accuracy.** Let `a_h` be the published `(accN, accE)` and
  `σ_dir = sqrt(û^T Σ_a,hh û)`, with `û = a_h / |a_h|`.
  - `accHAcc = σ_dir` when `|a_h| >= σ_dir`;
  - otherwise the square root of the larger eigenvalue of `Σ_a,hh` (also when
    `a_h = 0`).
- **Vertical accuracy.** `accDAcc = sqrt(Σ_a(D,D))`.
- **Why a heading error does not move them.** Its row `-[σ]x e_D` is
  perpendicular both to the horizontal force and to the vertical.

### The widening (clauses 31, 32; Decision 4)

The window for a sample at time `t`:

- the fixes `k` with `|t_k - t| <= 2.5` s (a named constant of the kernel);
- extended to include the two fixes around the sample;
- a contiguous range `lo..hi` of `N >= 2` fixes.

From `fit.residuals`, the factor and the widening are:

```text
Q = Σ_k=lo..hi (r²_position,k + r²_velocity,k) + Σ_k=lo+1..hi r²_imu,k
F = Q / (6N - 9),   w = sqrt(max(1, F))
```

The IMU residual's node index is the later fix's, as `collectResiduals`
writes it.

**Published values.** `headingAcc = min(180, w x heading)` and
`tiltAcc = min(180, w x tilt)`. `accHAcc` and `accDAcc` are multiplied by
`w` and not capped. Use prefix sums: on `11-17-12` the widening costs 3 ms.

### Output, registration, record

- **`fusion.h` (clause 33).** `Result` gains `headingAcc`, `tiltAcc`,
  `accHAcc`, `accDAcc` (deg, deg, m/s², m/s²) after `qw`, aligned with
  `time`.
  - They are filled only for `Succeeded` with the covariance computed.
  - They are empty for every other outcome, and for a success whose
    covariance failed.
  - Update the comments of `Result` and `run()`.
- **`fusionoutput.*`.** `fillOutputChannels` fills the four arrays from the
  trajectory and the widening. Update the comments that count
  ("seventeen").
- **`fusionregistration.cpp`.** `kFitOutputs` gains the four after `qw`:
  twenty-one measurements and `_FUSION_DIAGNOSTICS`, twenty-two outputs.
  - `publish()` leaves unset any output whose array is empty, so the four are
    unavailable, not empty, when absent. The record format refuses an
    available measurement without samples.
  - `fitOutputChannels()` returns all twenty-one in order.
  - Update the counts in `fusionregistration.h`'s comments.
- **The record (clause 42).** `calculationrecord.cpp` needs no change: it
  writes the outputs that are set.
- **`tests/fusion_runner.cpp`.** `--csv` writes the columns of
  `fitOutputChannels()` whose arrays are non-empty: twenty-one on a normal
  success, the seventeen when the accuracy is absent. Update the usage and
  the comments ("seventeen").

### Diagnostics (clauses 22, 34, 36)

- **`model.scale`** gains `acc_sigma` and `gyro_sigma`: three numbers each,
  `sqrt` of the diagonal of `Σ_gg`'s `S(0)` block. Each key is `null` when
  the covariance was not computed.
- **A top-level `accuracy` object:**
  - `computed`: boolean;
  - `failure`: `null`, or the fixed text above;
  - `heading_prior_sigma_rad`: `1000`;
  - `widening_half_width_s`: `2.5`;
  - `max_widening`: number, or `null`;
  - `widened_samples`: integer (samples with `w > 1`), or `null`;
  - `undetermined_heading_samples`: integer (samples with
    `headingAcc == 180`), or `null`.
- **`kLimitations`** becomes, exactly: "Local batch convergence; heading may
  be ambiguous. Between fixes one linearized pass with the fitted fix states,
  biases and scale factors held. Accuracies are first-order, one standard
  deviation under the documented noise model, widened where the residuals
  exceed it."
- **Unchanged:** failure diagnostics, and every other key. With the
  covariance failed, the seventeen channels and every other diagnostics key
  are identical to the computed case.

### Goldens (clause 44; decision 12)

**Before the capture**, run the pre-capture check. Against phase 3's capture,
everything below must be byte-identical, and the implementer reports it:

- the seventeen existing columns of every channels file;
- every diagnostics key but `limitations`;
- the eleven rejection goldens.

Phase 4 changes no existing number.

**The capture** uses the tool as `tests/README.md` section 11 describes.
After it:

- each channels file's column line lists the twenty-one names, under the
  same `v1` header;
- `fusionChannelNames()` and `fusionChannel()` gain the four;
- the history paragraph reads "The capture of <date> (the documented noise
  model, part 1, phase 4)": the four channels and `accuracy` added, nothing
  else changed;
- then run the `capture.json` gate step and both test modes.

**Portable floors** (`portableFloor`, `isExactKey`):

- `headingAcc` takes the heading floor, like `yaw`: on `stationary_spin` it
  is the sigma of a nearly flat direction (133-138 deg);
- `tiltAcc` takes the degree floor;
- `accHAcc` and `accDAcc` take the default floor;
- `acc_sigma`, `gyro_sigma` and `max_widening` take the default bound;
- `heading_prior_sigma_rad`, `widening_half_width_s`, `widened_samples` and
  `undetermined_heading_samples` are exact keys.

The fallback rule of `accHAcc` is discontinuous at `|a_h| = σ_dir`, and no
floor covers a sample that flips between platforms. In the probe the closest
sample of a golden fixture lies 3.2e-4 relative from the switch
(`stationary_spin`; `coarse_maneuver` 5.9, `coarse_linear` 1), against
platform differences of order 1e-7. If CI shows a flip anyway, the fix is a
reported sample, not a wider floor.

### What must not change

- the fit, its graph, its stopping and its bits;
- the seventeen channels;
- the reconstruction's states and acceleration;
- `Fusion::Algorithm` (`v6`, decision 11);
- `fitInputs()`;
- the record format;
- the number of cancellation boundaries;
- the initializer, and `yawSigmaDeg`'s behaviour;
- anything above the registration.

GTSAM stays out of `fusion.h` and `fusionregistration.*`.

### Documents (same change)

- **`docs/SENSOR_FUSION.md` section 4.** A new paragraph, "Accuracy", with:
  - the method and why it is not the joint marginals, the heading prior and
    what it costs, and the failure case;
  - the composition, stated as above;
  - the attitude accuracies;
  - the propagation in symbols, with what it leaves out (clause 30):
    - gravity's own uncertainty;
    - cross-axis sensitivity and misalignment;
    - the interpolation between nodes, that is, the step corrections `c`
      (their uncertainty is what the per-sample noise stands for) and the
      linear interpolation of the readings between samples;
  - the horizontal rule and the cap;
  - the widening: the a-posteriori variance factor of the window, which
    assumes every sigma is off by the same ratio and never tightens
    (clause 32);
  - the one sentence of clause 36: "The accuracy is one standard deviation
    from the covariance of the converged solution under the documented model,
    widened where the residuals exceed what the model allows."

  Also in section 4: four rows in the Outputs table, with their units; the
  diagnostics list gains `accuracy` and `model.scale`'s sigmas; and the new
  `limitations` text.
- **`docs/SENSOR_FUSION.md` section 7.** "all twenty-two outputs"; the
  covariance step and the composition are not boundaries, and take about 3 s
  on `11-17-12` (M50 replaces the probe's number); the record holds the four.
- **`docs/SENSOR_FUSION.md` section 8.** The new tests in the
  `tst_fusion_kernel` row, and the agreement numbers the tests log.
- **`docs/CALCULATIONS.md`.**
  - Section 15, `shutdown()`: the bound after the last iteration is the
    covariance step and the reconstruction, a few seconds on the longest
    reference recording.
  - Section 17: the outcome mapping (`Succeeded`: the seventeen measurements
    of the state, the four accuracies when the covariance was computed, and
    `_FUSION_DIAGNOSTICS`) and the record's contents.
- **`docs/DATA_SCHEMA.md` section 12.** "twenty-one measurements" where it
  counts the record's.

## Interfaces

### Provided to phase 5 (names and units are contract)

- **The channels.** `Fusion/headingAcc` (deg), `Fusion/tiltAcc` (deg),
  `Fusion/accHAcc` (m/s²) and `Fusion/accDAcc` (m/s²) are declared outputs of
  `builtin.fusion.fit` (`Fusion::FitCalculationId`), so plots over them are
  explicit-backed. The kernel sets no unit text; units are the plot
  registry's.
- **When they are absent.** Exactly when they are unavailable (unset, never
  empty):
  - before a fit;
  - for `Rejected` and `SolverFailed`;
  - for a success whose covariance failed (`accuracy.computed` false). The
    other seventeen are then present.

  A plot over them draws nothing there, and a column shows the unavailable
  mark, as for any fusion value.
- **What phase 5 may assume when present:**
  - they align sample for sample with `Fusion/_time`;
  - `headingAcc` and `tiltAcc` lie in (0, 180], and 180 means undetermined;
  - `accHAcc` and `accDAcc` are finite and at least `σ_acc` (> 0);
  - the stored record carries them, and a restored record gives the same
    bits.
- **The rest.** The audit group `accuracy-channels` (phase 5 extends its
  channel-name rule with the plot rows), and manual step M50.

### Consumed

- Phase 3: `S(0)`, `FitResult::scale` and `FitResult::graph`, the
  `scaleJacobian` out-parameter, `ImuStep::transition`, the reconstruction
  at `fit.scale`.
- Phase 2: `ImuNoise::accelerometer.sampleSigma` through `Tuning::noise`.

## Acceptance criteria

1. **`fitcovariance.*`.** It exists in `src/fusion` and holds the one
   `eliminateMultifrontal` call. `src` has no `jointMarginalCovariance` and
   no new `Marginals` (clauses 25, 49; audit).
2. **The covariance step against the joint marginals.**
   - On `coarse_maneuver`, every adjacent pair's 33x33 joint of `(x_k, x_k+1,
     B, T, S)` agrees with `Marginals::jointMarginalCovariance` on
     `fit.graph` within 1e-6 relative (Frobenius).
   - On `drifting_bias` the same holds at nodes 0, N/2 and N-2.
   - The node-0 heading equals `yawSigmaDeg(fit.graph, fit.values, X(0))`
     within 1e-5 relative, or both are 180, on every success and initializer
     fixture (clauses 25, 57).
3. **The composition against an edge graph.** On `coarse_maneuver`, at every
   tenth sample, the per-sample attitude covariance and the four unwidened
   accuracies agree within 1e-5 relative with those from
   `jointMarginalCovariance({X(j), V(j), B(0), S(0)})`. The edge graph is the
   `heldEndsReference` construction with the fixes, biases and scale free
   (GNSS factors at the fix edges, one-step `ScaledImuFactor`s, the three
   priors), solved to convergence. A sample on a fix has the fix's marginal
   (clauses 26, 29, 58).
4. **The accuracy formulas on known answers** (clauses 28, 29, 35, 58):
   - attitude: nav-frame elements, the cap, and NaN reading as 180;
   - acceleration: bias only, tilt only and scale only;
   - the directional accuracy is unchanged by an arbitrary heading variance
     under horizontal force;
   - the principal-value fallback;
   - the yaw cap inside the propagation.
5. **The widening** (clauses 31, 59).
   - The window rule on hand-built residuals.
   - On every committed fixture the widening is exactly 1.
   - On `scale_recording` with hAcc = vAcc = 0.2/√3 and sAcc = 0.03/√3 (the
     noise's standard deviations), every widening is ≤ 1.25 and the median
     factor is within [0.9, 1.1].
   - With those accuracies divided by 3 over [20, 40) s:
     - every sample in [22.5, 37.5) s has a widening in [2.6, 3.4], with the
       median within 10 % of 3;
     - every sample farther than 2.5 s from the stretch has a widening
       ≤ 1.25.
6. **Positive, finite, and monotone in the GNSS accuracies** (clause 56).
   - On the three success fixtures and the four initializer fixtures, every
     sample's four accuracies are finite and positive.
   - With every hAcc, vAcc and sAcc doubled and refitted, no published
     accuracy of the three successes falls below (1 - 1e-6) times its value.
7. **The undetermined heading** (clauses 35, 60). On `coarse_linear`:
   - `headingAcc` is 180 at every sample;
   - tilt and the acceleration accuracies are finite and agree within 1e-6
     relative with a gauge-fixed reference: the linearized `fit.graph` plus a
     1e-9 rad heading prior on `X(0)`, through `gtsam::Marginals` on the
     `GaussianFactorGraph`. Estimable quantities do not depend on the gauge.
8. **Channels and their absence** (clauses 33, 34, 42).
   - `kFitOutputs` and `registrationShape` list twenty-two outputs.
   - The four are filled only on success. A forced covariance failure leaves
     them empty and unpublished, gives `accuracy.computed = false` with the
     failure text, and leaves the seventeen channels and every other
     diagnostics key bit-identical.
   - A stored and restored fit has all twenty-one bit-identical.
9. **The diagnostics** (clause 22). `model.scale.acc_sigma` and `gyro_sigma`
   equal `sqrt(diag)` of `Marginals(fit.graph).marginalCovariance(S(0))`
   within 1e-6 relative. `accuracy` has exactly its keys. The `limitations`
   text has no "no uncertainty".
10. **Not a boundary** (clause 27). `checkpoint(` is still counted 3. The
    progress texts of every golden are unchanged.
11. **The capture** (clause 44). The pre-capture check holds. After the
    capture, the `_time` columns are byte-identical, the rejections are
    byte-identical, and the history paragraph is written.
12. **The budget** (Decision 3). The covariance step, the composition and the
    widening on `11-17-12` take at most 20 % of the initializer and full fit
    (M50).
13. **The documents** carry the statements of clauses 30, 32 and 36 (audit).
    The suite is green in `build-agent/` (Release, sequentially), and
    `audit_cleanup` is green with the list extended.

## Tests

### `tst_fusion_kernel`

- **Added:**
  - `covarianceMatchesJointMarginals` (2);
  - `firstNodeHeadingIsTheHeadingCheck` (2: clause 57; also the first
    published sample within 1e-3 relative where it lies within one IMU
    interval of fix 0);
  - `sampleCovarianceMatchesTheEdgeGraph` (3);
  - `sampleOnAFixHasTheFixMarginal` (3);
  - `attitudeAccuracyFollowsTheNavigationFrame` (4);
  - `accelerationAccuracyFollowsItsPropagation` (4);
  - `wideningWindowAndFactor` (5, hand-built);
  - `wideningIsOneAtTheModel` (5);
  - `wideningGrowsWithAnUnderstatedSigma` (5);
  - `accuraciesFiniteAndPositive` (6);
  - `gnssAccuracyScalingNeverLowersThem` (6);
  - `undeterminedHeadingIsCapped` (7);
  - `covarianceFailureLeavesTheFitAsItIs` (8: a seam that runs the success
    assembly with a covariance result that failed, for instance one computed
    from a `FitResult` copy whose `values` carry a NaN);
  - `diagnosticsReportTheScaleSigma` (9).
- **Amended:**
  - `initializerDiagnosticsShape` gains `accuracy` in the key set;
  - `imuRateIsWhatTheFitPublishes` also checks the four channels against
    the test's own composition and widening, bit for bit;
  - `diagnosticsReportTheScale`: `model.scale`'s keys;
  - the failure-shape tests (`slowTailAtTheIterationLimit`'s refused case
    and the non-convergence cases) assert the four arrays are empty.

### Others

- `tst_fusion_golden`: `successFixturesMatchGolden` (twenty-one columns),
  `rejectionFixturesMatchGolden`, `channelsWriterIsTheInverseOfTheLoader`.
  The `fusiongolden.*` changes are above.
- `tst_fusion_session`: `registrationShape` (twenty-two outputs, as a
  literal).
- `tst_fusion_runner`: `outputTableMatchesGolden` (twenty-one columns in
  order), `successMatchesDirectRun` (the CSV's columns).
- `tst_fusion_store`: `restoredAfterRestartIsBitIdentical` and
  `restoredAfterEvictionIsBitIdentical` (twenty-one channels).
- `tst_fusion_jobs`: `jobPublishesAllOutputsTogether` (twenty-two).
- `tests/fusion/fusionsessions.*`: `fusionMeasurementNames()` and
  `goldenDifference` gain the four; `syntheticFitSession`'s units gain deg,
  deg, m/s², m/s²; update the comments that count.
- `tst_fusion_rows::removedPlotMeasurementsStayAvailable` then covers the
  four, since they have no plot until phase 5.

### Audit: the new group `accuracy-channels`, beside `scale-state`

Each rule gets an "Allow:" comment and is planted once to prove it.

- `expect_none` of `jointMarginalCovariance` in `src`.
- `expect_only` of `eliminateMultifrontal|eliminateSequential` in `src`,
  allowed only in `^src/fusion/fitcovariance\.cpp$`.
- `expect_count` of `eliminateMultifrontal\(`, 1, in that file.
- `expect_count` of `constexpr double kYawSigmaCapDeg`, 1, in `src/fusion`:
  one cap.
- `expect_only` of `"(headingAcc|tiltAcc|accHAcc|accDAcc)"` in `src`,
  allowed only in `^src/fusion/fusionregistration\.cpp$`. Phase 5 adds its
  plot rows.
- `expect_none` of `no uncertainty` in `src docs`.
- `expect_count` (as written) in `docs/SENSOR_FUSION.md` of
  `a = R (f / s - b) + g`, `gravity's own uncertainty`,
  `cross-axis sensitivity`, `a-posteriori variance factor`,
  `never tightens`, and the clause 36 sentence.

Changes to other groups and files:

- `fusion-tooling`: `fitcovariance` joins the internal-header pattern.
- `fusion-model`: the description rule's banned `twenty-two` (phase 1)
  narrows to `twenty-(one|two) inputs`, so that section 7 may say
  "twenty-two outputs". Its comment says so.
- Traceability: append
  `1022 1025 1026 1027 1028 1029 1030 1031 1032 1033 1034 1035 1036 1042 1044 1049 1056 1057 1058 1059 1060`
  to the explicit list.
- `tests/README.md` section 10: a bullet for the new group.

### Traceability

Map lines, the comment line first:

- 1022 `tst_fusion_kernel diagnosticsReportTheScaleSigma`; `tst_fusion_golden successFixturesMatchGolden`
- 1025 `tst_fusion_kernel covarianceMatchesJointMarginals`; `audit accuracy-channels`
- 1026 `tst_fusion_kernel sampleCovarianceMatchesTheEdgeGraph`, `sampleOnAFixHasTheFixMarginal`
- 1027 `tst_fusion_golden cancelAtEachKindOfBoundary`, `progressMatchesGoldenBoundaries`; `audit fusion-model`
- 1028 `tst_fusion_kernel attitudeAccuracyFollowsTheNavigationFrame`
- 1029 `tst_fusion_kernel accelerationAccuracyFollowsItsPropagation`, `sampleCovarianceMatchesTheEdgeGraph`
- 1030, 1032, 1036 `audit accuracy-channels`
- 1031 `tst_fusion_kernel wideningWindowAndFactor`, `wideningGrowsWithAnUnderstatedSigma`
- 1033 `tst_fusion_session registrationShape`; `tst_fusion_kernel imuRateIsWhatTheFitPublishes`
- 1034 `tst_fusion_kernel covarianceFailureLeavesTheFitAsItIs`; `tst_fusion_golden rejectionFixturesMatchGolden`
- 1035 `tst_fusion_kernel undeterminedHeadingIsCapped`, `accelerationAccuracyFollowsItsPropagation`
- 1042 `tst_fusion_store restoredAfterRestartIsBitIdentical`; `tst_fusion_jobs jobPublishesAllOutputsTogether`
- 1044 `tst_fusion_golden successFixturesMatchGolden`, `rejectionFixturesMatchGolden`; `tst_fusion_kernel fitTraceMatchesGolden`
- 1049 `audit solver-confinement`; `audit accuracy-channels`; `audit fusion-tooling`
- 1056 `tst_fusion_kernel accuraciesFiniteAndPositive`, `gnssAccuracyScalingNeverLowersThem`; `manual M50`
- 1057 `tst_fusion_kernel firstNodeHeadingIsTheHeadingCheck`
- 1058 `tst_fusion_kernel accelerationAccuracyFollowsItsPropagation`, `sampleCovarianceMatchesTheEdgeGraph`
- 1059 `tst_fusion_kernel wideningIsOneAtTheModel`, `wideningGrowsWithAnUnderstatedSigma`
- 1060 `tst_fusion_kernel undeterminedHeadingIsCapped`

`tests/README.md` section 9.11 gets rows for these items, and its intro gains
the readings of 25 and 35. Appendix K restates:

- **25 (as settled):** "... by the clique marginals of one factorization (one
  multifrontal QR elimination in time order with the globals last, each
  clique's marginal from its parent's), not by the library's joint marginals;
  a heading prior of 1000 rad on the first fix, in the factorization only,
  keeps an undetermined heading finite and moves a determined one by less
  than 5e-6 of itself."
- **35 (as settled):** "... the cap applies, the heading's variance enters
  the acceleration's propagation at most at the cap's, and the acceleration
  accuracies use the horizontal magnitude's direction, which a heading error
  does not move."

Amended items, each restated "(as amended)" in its appendix, its 9.x row and
the map comment:

- **230** (C 30): the runner writes the twenty-one channels, or the
  seventeen when the accuracy is absent, since the specification of
  1001-1065.
- **235** (C 35): the result contract was unchanged by that specification;
  the specification of 1001-1065 adds the four accuracy channels (twenty-two
  outputs).
- **337** (D 37): twenty-one channels.
- **912** (J 12): the twenty-one channels of section 4, the uncertainty
  among them since the specification of 1001-1065.
- **Searched and holding:**
  - 917 and 937: the account and `limitations` are still compared;
  - 925 and 929: the pass still runs after the last iteration and is not a
    boundary;
  - 854 is phase 5's.

Section 1 rows to update: `tst_fusion_golden` (seventeen channels),
`tst_fusion_kernel`, `tst_fusion_runner`, `tst_fusion_session` (twenty-two
outputs), `tst_fusion_store`. Section 11 changes: the channels file's
columns, "all seventeen channels", and the history paragraph. Section 12.2's
M10 changes: the CSV header has twenty-one names.

### Manual step M50 (section 12.9; items 1056, budget)

M49's commands on the reference recordings, with `--csv`. Per recording:

- exit 0, and `accuracy.computed` true;
- the CSV has twenty-one columns;
- the four accuracies' minimum, median and maximum, `max_widening`,
  `widened_samples`, `undetermined_heading_samples`, and
  `model.scale.acc_sigma` and `gyro_sigma`;
- on `11-17-12`, the runner's wall time against M49's. The increase must be
  at most 20 % of M49's fit time.

The numbers go in the report, not adjusted.

## Decisions

1. **The Bayes tree's clique marginals, not the selected inverse in
   covariance form** (for Michael).
   - **Why not Takahashi.** The Takahashi recursion
     `Σ_fP = -R^-1 S Σ_PP` is O(N) and exact in arithmetic. But it compounds
     rounding along the chain: on `drifting_bias` and `sacc_anchor` (12.5 Hz,
     1 Hz fixes) the node blocks were wrong by 1e4 and 2e18 relative at the
     start of the chain, with negative variances, under QR and Cholesky
     alike.
   - **Why clique marginals.** They stay in square-root form, cost O(N) with
     the cached separator marginals, and agree with the dense inverse and
     with the joint marginals to 2e-9..2e-5.
   - **Why not solves.** Block solves (`backSubstitute` per column) would
     need nine columns per fix: O(N²).
   - **Clause 25.** "By solves of the factorized system" is read as "from the
     factorized system", hence "as settled".
2. **A 1000 rad heading prior, in the factorization only** (for Michael).
   - **Unregularized.** On `coarse_linear` the QR pivot of the heading falls
     to rounding (1.3e-9). The published tilt was then wrong by 210 % and
     `accHAcc` by 18x, and Cholesky threw.
   - **With the prior.** Tilt and the acceleration accuracies equal a
     gauge-fixed reference to 2e-12 and 8e-11.
   - **Its cost.** Where the heading is determined, the relative change is
     `σ_ψ² / (2 σ_h²)`: ≤ 4.9e-6 at the cap, 2.7e-7 measured on
     `sacc_anchor` (42 deg).
   - **Choice of 1000 rad.** 100 rad costs 5e-4 at the cap; 1e4 rad loses
     accuracy to 8e-9; 1e6 rad loses it to 8e-5.
   - **A rank test instead?** It would need a threshold of its own.
3. **The budget: at most 20 % of the initializer and full fit on `11-17-12`.**
   - **Measured.** The probe took 3.2 s against 25.8 s (12 %): linearize
     0.02 s, eliminate 0.59 s, clique marginals 2.0 s, composition 0.52 s,
     widening 0.003 s.
   - **The alternative.** `jointMarginalCovariance` took 0.62 s per node
     there, about 92 minutes for all.
   - **Cancellation.** A cancel during these seconds waits for them, as for
     the reconstruction today.
4. **The window** (decision 17) uses prefix sums over the fixes, and the
   published heading and tilt are widened before the cap.
   - **Measured at the model.** On `scale_recording` with the stated
     accuracies at the noise's standard deviations, the median factor is
     1.01 and the largest widening 1.10.
   - **Understated by r = 2, 3, 5.** The median widening inside the stretch
     is 1.99, 2.97 and 4.93, and stays ≤ 1.10 outside it.
   - **Boundary effects.** The window's end states take some information
     from outside, so the factor runs a few percent above one. That is not
     corrected.
5. **The heading enters the propagation at most at the cap.** This bounds
   the principal-value fallback when the horizontal acceleration is small.
   Where the heading is below the cap it changes nothing. Clause 35 is
   restated "(as settled)".
6. **The fallback of `accHAcc` compares `|a_h|` with `σ_dir`, not with the
   principal value.** Comparing with the principal value would let an
   undetermined heading, which inflates only the perpendicular direction,
   replace the directional accuracy (clause 35).
7. **Absent channels are unset, not empty.** The record refuses an
   available measurement without samples, and "absent like any unavailable
   value" is the plots' contract.
8. **The composition lives in the reconstruction's pass,** which owns
   `P_j`, `F_j`, `M_j` and the sharing. The covariance step owns the
   factorization, the formulas and the widening. Neither restates the
   other's facts.

## Probe (`experiments/covariance_probe/`)

The probe is built against the third-party installs in its own `build/`, on
phase 2 and 3's kernel copies from `experiments/scale_probe/` (fixtures
rounded onto the lattice). `11-17-12.inputs` is `fusion_runner
--dump-inputs` from `build-agent/`. The outputs are `run1_selected.txt`,
`run2_gauge.txt`, `run3_dense.txt`, `run4_fixtures.txt` (without the
heading prior), `run4b_fixtures_prior.txt` (with it, as specified),
`run5_11-17-12.txt`, `run6_widening.txt` and `run6b_widening_scaled.txt`.

- **The clique marginals against the joint marginals and a dense LU
  inverse.**
  - 5e-8 (`coarse_maneuver`, all 27 pairs), 2e-8 (`motion_start`), 2e-9
    (`sacc_anchor`), 3e-9 (`drifting_bias`).
  - 3e-6 and 2e-5 on the weak headings of `stationary_spin` and
    `rest_throughout`.
  - The node-0 heading matches `yawSigmaDeg` to 1e-13.
  - The Takahashi recursion failed as Decision 1 says.
- **The composition against the edge graph.**
  - `coarse_maneuver` (568 edges): state 1.1e-7, heading 1e-9, tilt 1e-8,
    `accHAcc` 6e-8, `accDAcc` 7e-9.
  - `stationary_spin` (heading 133-138 deg): 1e-3, from the re-solved
    linearization along the weak heading.
- **Fixtures** (headings unwidened, deg; with the prior):
  - `coarse_linear`: 180 everywhere, with tilt 2.54-2.87 deg, `accHAcc`
    0.065-0.176 and `accDAcc` 0.0637 m/s². Without the prior the tilt read
    6.9-8.7 deg;
  - `coarse_maneuver`: 13.9-17.3;
  - `motion_start`: 0.4-5.4, with tilt 1.76 and `accHAcc` 0.0044-0.0057
    m/s²;
  - `drifting_bias`: 0.45-1.0;
  - `rest_throughout`: 180 everywhere.

  The widening factor is ≤ 0.03 everywhere, because the fixtures' noise is
  below their stated accuracies. Doubling every GNSS accuracy lowered no
  accuracy: the smallest ratio was 1.0000017, `rest_throughout`'s tilt, and
  1.029 among the three successes (`stationary_spin`'s `accDAcc`).
- **`11-17-12`** (8,860 fixes, 23,449 samples).
  - Fit 10.8 s plus initializer 15.0 s. The timings are in Decision 3.
  - Heading 0.35-5.3 deg (median 2.5); tilt 0.04-4.2 deg (median 0.37);
    `accHAcc` 0.0035-1.74 m/s² (median 0.040); `accDAcc` 0.0030-1.36 m/s²
    (median 0.0046).
  - The widening factor's median was 0.08 and its maximum 1.11; 0.26 % of
    samples were widened.
  - The first sample's heading was 4.333 deg, against `yawSigmaDeg` 4.333.

Ready with caveats: Decisions 1 (clause 25 as settled), 2 (the heading prior)
and 5 (clause 35 as settled) are flagged for Michael. The portable floors of
the new channels follow the existing policy and are not yet measured across
compilers. The probe's numbers come from kernel copies, not the implemented
phases 2-3.
