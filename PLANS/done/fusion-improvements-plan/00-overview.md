# Implementation Plan: Sensor fusion improvements (segmented initializer, stopping rule, IMU model)

## Feature Specification

The complete specification, `PLANS/fusion-improvements.md` as of 2026-09-22, follows unabridged. The Commit Policy at its end is repeated as a top-level section of this overview (the implementation orchestrator reads it there).

---

## Sensor fusion: segmented initializer, stopping rule and IMU model

Date: 2026-09-22
Status: specification for planning. Not an implementation plan.
Baseline: `master` (contains the ported fusion kernel and the job queue).
Supersedes the earlier draft of this file. `PLANS/jobs-dock-clean.md` is
parked; nothing here depends on it.

This document says what the changes must do and the boundaries they must
respect. It avoids class layouts and function signatures unless a name is
part of the observable contract. The planning agent decides phasing; the
implementation agent decides code structure. Every number below is a
specification value unless marked "tuning".

### 1. Goal and motivation

A recording that looks normal must produce a converged fit. On the
reference corpus (97 recordings from seven units) the shipped kernel
converges 83 with one fit, and on corrected gyro data it loses eleven more.
The causes are in how the fit is started and how it decides it has
finished, not in the solver or the factor graph:

- The starting attitude of every pose is produced by integrating the gyro
  from one anchor (a stationary window, or failing that the first fix) with
  one bias value. Two units in the corpus have a gyro bias that follows
  temperature at 0.12 deg/s per degC (ten times the datasheet's typical
  value), so over a long recording the propagated attitudes end up hundreds
  of degrees off and the optimizer creeps or settles in a wrong minimum. The
  same recordings converge in about ten iterations when every pose starts
  near its answer.
- The stationary-window detector rejects a resting unit whose bias exceeds
  1 deg/s, which the LSM6DSO's typical zero-rate level is; a quarter of the
  corpus' units sit above it once the gyro scale is corrected.
- Fits that have finished are reported as not converged: the bias-settled
  threshold is below anything the preintegration can respond to, and a slow
  final descent never meets a 1e-8 relative tolerance.
- The IMU noise model is one constant per sensor; at the default 12.5 Hz
  output rate the integration error during manoeuvres is many times the
  modelled noise.

### 2. Scope

In scope:

- A segmented initializer that replaces both the stationary-window
  initializer and the coarse fallback (section 3).
- The stopping rule (section 4).
- A per-step IMU noise term (section 5).
- A temperature-dependent gyro bias (section 6).
- Diagnostics for the above (section 7), a command-line runner (section 8),
  tests and golden references (section 10), documentation (section 11).

Out of scope:

- The magnetometer; the fit does not read it.
- Changes to the GNSS factors, the local frame, the input adapter's
  contract beyond the one channel in section 6, the output channels, the job
  queue, plot rows, or any dock.
- The IMU noise densities (1e-3 rad/s/rtHz gyro, 0.015 m/s2/rtHz
  accelerometer stay as they are; a corpus test of a lower gyro density made
  agreement with GNSS worse on 47 of 83 recordings).
- A gyro scale-factor state. The corpus shows one unit 2 % off nominal and
  two on it; the gain is about 10 % of the objective on that unit at 12.5
  Hz. Recorded as a follow-up.
- Solver changes (linear solver, damping schedule, robust losses).
- User-adjustable initialization.

### 3. Segmented initializer

#### 3.1 What it produces

Initial values for the full graph: an attitude for every pose, the GNSS
position and velocity for every state as today, and one starting bias. The
full fit itself (graph, factors, tuning, pass structure) is unchanged.

#### 3.2 Behaviour

1. Cut the fitted window into consecutive segments of 600 s (tuning),
   starting at its first fix. A final piece shorter than 120 s is merged
   into the segment before it. A fitted window shorter than one segment is
   one segment.
2. For each segment, independently:
   a. Choose the anchor: the fix in the segment with the smallest logged
      speed accuracy (sAcc), the earliest on a tie. The single-fix GNSS
      acceleration has an error of about seven times sAcc, so this bounds
      the tilt error of the start. Take the coarse attitude there
      (existing rule: measured force aligned with GNSS acceleration minus
      gravity; gyro bias zero).
   b. The coarse attitude gives a tilt but no yaw and no bias, and both
      are needed before attitudes are propagated across the segment. So
      first fit only a prefix: the window of length L = 60 s centred on
      the anchor, clipped to the segment. Its start attitude is the coarse
      attitude carried from the anchor to the window's first fix by
      integrating the gyro with zero bias (backwards). Fit it from four
      heading offsets (0, 90, 180, 270 degrees about the vertical, applied
      to that attitude), using the production graph and tuning restricted
      to the window's fixes.
   c. For the lowest-objective start, compute the marginal covariance of
      the window's first pose from the graph linearized at the solution
      (rebuilt at the fitted bias), rotate its rotation block into the
      navigation frame, and take the square root of the vertical element
      as the yaw sigma.
   d. If the yaw sigma exceeds 20 degrees, the window does not yet cover
      the segment, and the last doubling reduced the yaw sigma by at least
      20 %, double L (still centred on the anchor, clipped to the segment)
      and repeat from b. Otherwise stop growing: a segment whose prefixes
      do not gain yaw information by growing has no motion, and fitting
      more of it will not help.
   e. Fit the whole segment once, starting from the lowest-objective
      prefix fit's attitude at the window's first fix, carried backwards to
      the segment's first fix and forwards to its last with that fit's gyro
      bias (so the whole segment starts near its answer). Keep the fitted
      attitude at every fix of the segment and the fitted bias.
3. Segment check. A segment fit can settle in a wrong family of its own
   and would seed the full fit into it. When there are at least two
   segments: a segment whose segment fit has an IMU normalized RMS above
   twice the median of the fitted segments', or that fell back without a
   fit, is refitted once from its neighbour: the start attitude is the
   previous segment's fitted attitude at its last fix carried to this
   segment's first fix with the previous segment's bias (for the first
   segment, the next segment's fitted attitude at its first fix carried
   backwards). The result with the lower IMU normalized RMS is kept (a
   fallback segment keeps the refit whenever it converges). One round; a
   restarted segment is not used as the neighbour of another restart.
4. Initial values for the full graph: for each pose, the attitude of that
   fix from its segment's fit; positions and velocities from the GNSS
   fixes; the gyro bias of the first segment; accelerometer bias zero.
5. Run the full fit.

The yaw of a segment comes from the segment's own motion. A segment that
is entirely at rest has none; its prefix stops growing at the first doubling that gains no
yaw information, the returned yaw is arbitrary, and that is acceptable: yaw is unobservable there
in the full fit too, and neighbouring segments carry the heading through
their own motion. "Agreement of the fitted yaw between starts" is not an
observability test and must not be substituted for the marginal sigma.

#### 3.3 Rules

- Segment fits are cancellable and report progress like the full fit; each
  of their iterations is a boundary. Progress text names the segment (its
  index and count) and, for prefix fits, the prefix length.
- Budgets. A prefix fit is a start, not an answer: one pass of at most 50
  iterations, no re-preintegration passes. A segment fit uses the
  production pass structure and limits. The stopping rule of section 4,
  including the slow-tail acceptance, applies to prefix and segment fits
  exactly as to the full fit; a prefix or segment fit that ends on the
  iteration limit without meeting it is still used as a start (its values
  are the best available), and the diagnostics say so.
- A prefix or segment fit that fails (non-finite or increasing cost) counts
  as a start with infinite objective. If all four starts of a prefix fail,
  treat the prefix as "yaw not observable" and grow; if all starts fail at
  the segment's full length, the segment's attitudes are the coarse attitude
  propagated by the gyro with zero bias (today's fallback, over that segment
  only) and the diagnostics say so.
- A segment must satisfy the validity rules of the fitted window (fixes
  inside IMU coverage, no IMU gap, at least three fixes); the fitted window
  already does, so a segment cut from it does too.
- No stationary-window detector takes part. The detector and its gates may
  be deleted or kept for diagnostics only; they decide nothing.

#### 3.4 Evidence

Corpus, one full fit each, corrected gyro: the opening-only form of this
initializer (one segment, used where no stationary window existed)
converged all 29 such recordings, four of which no start of the earlier
five-start study converged. The segmented form converged every recording
the corpus fitted worst: Data comp 2 / 10-15-24 (395,751 unconverged after
500 iterations, now 80,597 in 10), 08-35-41 (timed out, now 108,048 in 11),
16-02-25 (551,244 in 328, now 28,195 in 11), 10-52-49 (640,553, now 88,917
in 17). Segment fits take 8-20 iterations on about 3000 states each. Over the
full corpus the segmented form converges 91 of 97 (baseline 89), with
fewer iterations (median 29 to 11) and a lower objective on 23 of the 82
both converge; on 2 it converged into a worse family because one segment's
own fit had (IMU normalized RMS 1.4 and 4.1 against 0.2-0.4 in its
neighbours), which the segment check of step 3 repairs to exactly the
baseline's solution. The six not converged are stopping-rule cases on
resting recordings (section 4). Tally in section 12.

### 4. Stopping rule

Today a fit converges when a pass settles (relative cost decrease at or
below 1e-8 of max(1, cost)) and re-preintegrating at the pass's fitted bias
moves the bias by less than 1e-5 m/s2 and 1e-6 rad/s.

#### 4.1 Bias-settled test

Replace the bias-shift test by a cost test: after a pass settles, rebuild
the graph at the new bias and evaluate its cost at the pass's values; the
bias has settled when that cost differs from the pass's final cost by at
most 1e-6 relative (of max(1, cost)). The pass structure (at most five
passes, re-preintegration between them) and the settle tolerance are
unchanged.

#### 4.2 Slow tail

When the final pass reaches the iteration limit without settling, the fit
is accepted as converged if both hold over the last 20 iterations of that
pass: the mean relative decrease per iteration is below 1e-4, and the
position and velocity factors' normalized RMS (square root of the mean
squared whitened residual per scalar component) are both below 2. The
diagnostics say the fit was accepted on this rule. A fit that meets neither
remains a solver failure.

Evidence: four ground recordings of the corpus end this way with position
RMS of 0.3-0.5 m; the two hard recordings of section 3.4 descended at 4e-4
per iteration under the old initializer and would not have been accepted.

### 5. Per-step IMU noise term

The integration treats the IMU stream as piecewise linear between samples.
At 12.5 Hz (the gyro is filtered at 4.2 Hz at that rate) this is wrong
during manoeuvres by an amount that grows with the change of the signal
across a step and falls with the square of the sample interval. Model it as
an additional white-noise term per integration step, added in quadrature to
the density term:

- gyro: sigma_w = 0.026 x dt x |delta omega| (radians), dt the step length
  in seconds, |delta omega| the norm of the difference between the
  interpolated rate at the step's end and at its start, rad/s;
- accelerometer: sigma_a = 0.40 x dt x |delta f| (m/s), |delta f| the norm
  of the difference of the specific force at the step's end and start,
  m/s2.

The per-step covariance for that step is (density^2 + sigma^2 x dt) times
identity, so that the per-step variance (covariance over dt) is
density^2/dt + sigma^2. Step boundaries and midpoint sampling are
unchanged; the densities are unchanged. The constants are tuning values
calibrated at a 0.076 s step; the dt factor keeps them valid at higher
output rates, where the term vanishes against the density.

Implementation constraint: GTSAM's preintegration holds one covariance per
sensor in its shared parameters; a per-step covariance is applied by setting
those parameters before each `integrateMeasurement` call.

Evidence (full corpus, 97 recordings, one fit each with the baseline
initializer): convergence 89 against the baseline's 89 (one recording
lost, one gained; both converge under the section 3 initializer). On the
88 both converge: whitened position RMS 0.35 to 0.29 at the median and
0.99 to 0.77 at the 90th percentile, velocity 0.59 to 0.55, IMU 0.45 to
0.42 (90th percentile 2.50 to 2.09), iterations unchanged at 28; agreement
with GNSS better on 58 recordings, worse on 1.

### 6. Temperature-dependent gyro bias

Two of the corpus' seven units have a gyro bias that follows the logged IMU
temperature at 0.10-0.13 deg/s per degC on one axis, consistently across
nine and seven recordings; the other five are within the datasheet's
+/-0.010. Over a 14 degC excursion that is a 1.4 deg/s change that one
constant cannot represent; after the section 3 initializer these
recordings converge, with an IMU normalized RMS of about 1.1 where their
600 s segments reach 0.2-0.5.

Change: the gyro bias becomes b(t) = b0 + b1 x (T(t) - T_ref), with b0 and
b1 three-vectors estimated per recording, T the IMU temperature channel of
the recording, and T_ref the mean IMU temperature over the fitted window.
Priors: b0 as today's bias prior (0.03 rad/s per axis, zero mean); b1
zero-mean with sigma 0.010 deg/s per degC per axis (the datasheet's
typical), so a recording without a temperature change leaves b1 at its
prior and a well-behaved unit loses nothing. The accelerometer bias stays
constant.

Shape in the graph (either is acceptable; the planner chooses): each fix
carries its own gyro bias variable tied to (b0, b1) by a linear factor
through that fix's temperature, with the existing IMU factor unchanged; or
a custom IMU factor taking (b0, b1) directly. Per fix, T is the temperature
interpolated at the fix. Inputs: the fit's input channels gain IMU
temperature (same length as the other IMU channels). It is a required
input like the others: the column is part of every FlySight 2 IMU record,
and a file without it is blocked like any file missing a declared input.

The initializer of section 3 is unaffected: segment fits use a constant
bias, and the full fit starts b1 at zero.

Evidence: the per-unit coefficients above, measured on the raw data at
rest, and the segment fits of 10-15-24 whose fitted constant biases step
from 1.56 to 0.25 deg/s with the temperature. This model has not yet been
exercised in the graph; its acceptance (section 10) is the reduction of the
IMU misfit on those recordings with no loss elsewhere. It is independent
of sections 3 to 5 and may be planned as the last phase.

### 7. Diagnostics

The diagnostics object reports, in addition to what it carries today:

- initializer: the segments (start, end, prefix length reached, yaw sigma
  of the chosen start, iterations of the segment fit, IMU normalized RMS of
  the segment fit), any segment that fell back to propagation, and any
  segment that was restarted from its neighbour and whether the restart was
  kept;
- stopping: which rule ended the fit ("settled", "slow tail accepted",
  "iteration limit", "cost increased"), the last pass's mean relative
  decrease per iteration, and the re-preintegration cost difference;
- quality: normalized RMS of the IMU, position and velocity factors, and
  the objective per state;
- model: the per-step constants and the fitted b0, b1.

The tooltip keeps showing what it shows today. Diagnostics remain an
account, not an input.

### 8. Command-line runner

A command-line program that runs the fit on one recording and prints its
diagnostics, so that the corpus comparison behind this document can be
repeated on the product kernel.

- Input: a folder holding `TRACK.CSV` and `SENSOR.CSV` (or the two paths).
  It imports the files exactly as the application does (same session
  import, conversion layer and channel derivation, so a legacy-schema file
  gets the gyro scale correction) and feeds the kernel the same channels
  the job queue would.
- Output: the diagnostics JSON on standard output; with an option, the
  seventeen output channels as CSV to a given path. Exit status 0 for
  Succeeded, non-zero otherwise, with the outcome and reason on standard
  error.
- No GUI, no logbook, no preferences: it must not read or write the user's
  logbook or settings.
- Progress texts go to standard error; cancellation is not required.

It is a test/tooling target, built with the tests, not shipped with the
application.

### 9. Boundaries

- Purity, threading and cancellation rules of the kernel are unchanged: a
  function of the channels, no session or GUI object, every solver
  iteration a boundary, cancellation observed at boundaries only.
- The public result contract (channels, outcomes Rejected / SolverFailed /
  Cancelled / Succeeded) is unchanged; a slow-tail acceptance is a
  Succeeded result.
- Nothing outside the fusion library links GTSAM.

### 10. Tests and golden references

Every change here alters numerical results. The golden references captured
from the reference branch no longer apply: their bit-exact parity tests are
retired, new goldens are captured from the changed kernel with the same
harness, and the regression tests compare against those. Diagnostics
goldens likewise.

Synthetic fixtures, expected values stated independently:

- a recording that starts in motion: segment prefixes grow until the yaw
  sigma is below 20 degrees exactly at the first length containing the
  manoeuvre; the full fit converges to the truth attitude within 2 degrees;
- a recording at rest throughout, longer than two prefix doublings: growth
  stops at 120 s because the doubling reduces the yaw sigma by less than
  20 %, the full fit converges, roll and pitch within 0.5 degrees of truth;
- prefix fits run one pass of at most 50 iterations; a prefix and a segment
  fit forced onto the iteration limit are still used as the start and the
  diagnostics say so;
- a recording whose fixes carry sAcc 2 m/s except one at 200 s with
  0.3 m/s: that fix is the anchor, the first prefix is the window 170-230 s,
  and the start attitude at the segment's first fix is the prefix fit's
  carried back by the gyro;
- a recording longer than two segments with a bias that drifts linearly by
  1 deg/s over its length: every segment fit converges; the full fit with
  section 6 recovers b1 within 20 % and converges within 30 iterations;
- a segment whose prefix fits all fail: the segment falls back to
  propagation, the diagnostics say so, the full fit still runs;
- a three-segment recording in motion whose middle segment's prefix fits
  are all forced to fail: the middle segment is restarted from the first
  segment's boundary attitude, converges, the restart is kept and reported,
  and the full fit converges to the truth attitude within 2 degrees;
- bias-settled test: a fit whose cost stops changing converges within two
  passes;
- slow tail: a forced final pass at the limit is accepted when both
  conditions hold and is a failure when either fails;
- per-step term: a step with zero signal change has the density covariance
  exactly; a step with a known change has the specified covariance; the term
  scales with dt;
- section 6: a recording without a temperature channel is blocked as a
  missing input; a recording with constant temperature leaves b1 at its
  prior and reproduces the constant-bias fit.

### 11. Documentation

`docs/` describes the segmented initializer, the stopping rule with its
slow-tail acceptance, the per-step noise term and its meaning at other
output rates, and the temperature-dependent bias, in the place that
documents the fusion model today.

### 12. Reference cases and corpus

Untracked, on Michael's machine, under `TEMP/data/`:

- `Data comp 5 - FS 2 - serie nr 2 - 014667 (test 10)/24-09-05/11-17-12`:
  bias 1.03 deg/s after the schema correction; baseline: no window
  accepted, not converged after five passes. Expected: converged in about
  12 iterations, objective near 45,000 with today's noise model.
- `Data comp 1 - FS 2 - serie nr 2 - 01465 (test 08)/24-09-07/08-35-23`:
  no resting window; baseline "converges" to objective 14 million, position
  RMS 25 m. Expected: about 11,000, position RMS 0.7 m, under 40 iterations.
- `Data comp 2 - FS 2 - serie nr 2 - 01086 (pers.)/24-09-07/10-15-24` and
  `08-35-41`: drifting-bias unit, 45 and 40 minutes; baseline unconverged.
  Expected: converged in 10-15 iterations (about 80,000 and 108,000 with a
  constant bias).

Corpus tally (97 recordings, one fit each, corrected gyro): baseline
initializer 89 converged; opening-only bootstrap on the 29 no-window
recordings 29 of 29; segmented initializer (without the segment check)
over all 97: 91 converged, the six left being stopping-rule stalls or slow
tails on resting recordings, and two converged fits in a worse family that
the segment check repairs. On the twelve recordings the corpus fitted
worst it converged ten in 8-17 iterations (one ground recording in 144) to
objectives 3-30x lower than the baseline's. Michael runs the corpus comparison with the experiment
tooling; it is not part of the automated tests.

### 13. Principles

- Start every pose near its answer. Nothing is propagated further than a
  segment.
- The graph is the model; the initializer only chooses where the solver
  starts. Never let an initializer decide a model quantity the fit can
  estimate.
- "Converged" means the cost has stopped moving. Report quality beside it.
- Model what the data determine (bias, its drift) rather than absorbing it
  in noise.
- Keep it small: one initializer path, no gates.

## Phases

| Phase | Name | Purpose | Dependencies |
|-------|------|---------|--------------|
| 1 | Golden regression harness | Bring the golden capture into this repository: a non-test executable built with the fusion tests that runs the product kernel on the twelve synthetic fixtures and writes the golden files in their existing formats; prove it by re-capturing from the unchanged kernel and getting the committed goldens bit for bit; retire the branch-parity provenance (capture procedure, wording, `capture.json` fields) so that from here on goldens are captured from the product kernel with one command and every later numerical phase re-captures at its end (spec section 10). No numerical change. | None |
| 2 | Command-line runner | `fusion_runner`: import a folder's `TRACK.CSV` and `SENSOR.CSV` exactly as the application does, feed the kernel the channels the job queue would, print the diagnostics JSON on stdout, optionally the seventeen channels as CSV, progress on stderr, exit status from the outcome; no logbook, no user settings, no GUI (spec section 8). Built in the fusion-tests block, not shipped. | None |
| 3 | Stopping rule and quality metrics | Replace the bias-shift settled test by the re-preintegration cost test; accept a slow tail at the iteration limit on the two conditions; compute the normalized RMS of the IMU, position and velocity factors and the objective per state; report `stopping` and `quality` in the diagnostics (spec sections 4, 7); synthetic tests; re-capture goldens. | Phase 1 |
| 4 | Per-step IMU noise term | Add the per-step white-noise term to the preintegration covariance before each `integrateMeasurement` call, with the two tuning constants and their `dt` scaling; report `model.per_step` in the diagnostics (spec sections 5, 7); unit tests on the covariance; re-capture goldens. | Phase 1 |
| 5 | Segmented initializer | Replace the stationary-window initializer and the coarse fallback by the segmented initializer (600 s segments, sAcc anchor, grown prefix with four heading starts and the marginal yaw sigma, one segment fit, stitched per-pose attitudes, first segment's bias); delete the detector and its gates; segment fits report progress and are cancellable; report `initializer` in the diagnostics (spec sections 3, 7); synthetic tests; re-capture goldens. | Phases 3, 4 |
| 6 | Temperature-dependent gyro bias | Add the IMU temperature channel to the fit's inputs and registration; in the full fit model the gyro bias as `b0 + b1 (T - T_ref)` through a custom IMU factor with its prior on `b1`; the temperature is an ordinary required input (a recording without it is rejected like any missing input); report `model.gyro_bias` in the diagnostics (spec sections 6, 7); synthetic tests; re-capture goldens. | Phase 5 |
| 7 | Documentation, acceptance map and audit | `docs/SENSOR_FUSION.md` describes the segmented initializer, the stopping rule with its slow-tail acceptance, the per-step term and its meaning at other output rates, and the temperature-dependent bias; `tests/README.md` states the final golden regression procedure; an acceptance map from every spec requirement and every section 10 test to the test that proves it; audit that the goldens on disk are the last capture, that the detector is gone, and that nothing outside `src/fusion/` links GTSAM (spec sections 9, 11). | Phases 1-6 |

## Dependency Graph

```
Phase 1 (golden harness) --> 3 (stopping rule) --+
          |                                      +--> 5 (segmented initializer) --> 6 (temperature bias) --> 7 (docs, audit)
          +----------------> 4 (per-step term) --+                                                            ^
Phase 2 (runner) ---------------------------------------------------------------------------------------------+
```

- Phases 1 and 2 have no dependencies and can be documented and implemented in parallel; Phase 2 touches nothing under `src/fusion/` except the registration header it may add a public declaration to.
- Phases 3 and 4 both depend only on Phase 1 and are documented in parallel. They are implemented one after the other (3 then 4); their code does not overlap (Phase 3 lives in `factorgraphfit.cpp` and `fusionoutput.cpp`, Phase 4 in `imuintegration.cpp` and `fusionsamples.h`), and the diagnostics key layout below keeps their JSON additions disjoint.
- Phase 5 needs the stopping rule (its segment and prefix fits are ordinary fits and must terminate under the new rule) and the per-step term (segment fits use the production graph; the corpus evidence was gathered with it).
- Phase 6 needs Phase 5: the initializer hands the full fit per-pose attitudes and one constant gyro bias, and the full fit starts `b1` at zero.
- Phase 7 closes the plan. Michael's corpus comparison (spec section 12) is manual, through the Phase 2 runner, and is not part of any phase's acceptance.
- Every numerical phase (3, 4, 5, 6) ends with the golden re-capture of Phase 1, so the whole suite is green at every phase boundary and the final goldens are simply the last capture.

## Key Patterns & References

### Fusion kernel (`src/fusion/`, library `flysight_fusion`, the only target linking GTSAM)
- `src/fusion/fusion.h` — the public contract: `Channels` (21 fields, no temperature), `Outcome`, `Result` (17 output channels), `ProgressFn` / `CancelFn`, `run()`. Phase 6 adds one channel field here.
- `src/fusion/fusion.cpp` — `planFit()` (checks, fitted window, `tuning.maxGap`, GNSS outage limit, initializer call at line 56) and `fitAndAssemble()`; the only place that catches; `kInitialHeadingDeg = 0` at line 22; boundary "Starting fit" at line 63.
- `src/fusion/fusionpipeline.h` — `runPipeline()` and `PipelineTrace` (the test seam for the initializer result, iteration history and convergence flag).
- `src/fusion/fusionprogress.h` — `Checkpoint` (report then poll), `FusionCancelled` (not a `std::exception`); the cancellation-boundary rule.
- `src/fusion/fusionsamples.h` / `.cpp` — `Samples`, `Tuning` (densities, bias prior sigmas, `maxGap`, `relativeTolerance`, `maxIterations`), `fittedWindow()` (lines 162-174: fixes in `[start, end]` inside IMU coverage, IMU one sample before and after), `validateSamples()`, `requireNoGnssOutage()`; the sub-window helper the segment cutting reuses.
- `src/fusion/factorgraphfit.h` / `.cpp` — `FitResult`, `FitIteration`, `FactorResidual`; `buildFactorGraph()` (lines 117-131, factor order is load-bearing), `runOptimizerPass()` (lines 63-88: LM with `MULTIFRONTAL_QR`, one `iterate()` per boundary, settle test at 81-84, cost-increase guard at 79-80), `biasSettled()` (91-94, the test Phase 3 replaces), `fitFactorGraph()` (133-161: `kMaxBiasPasses = 5`, rebuild at the fitted bias for the reported objective), `collectResiduals()` (98-113: per-factor squared whitened errors, the basis of the normalized RMS).
- `src/fusion/imuintegration.h` / `.cpp` — `integrationEdges()`, `interpolateAt()`, `preintegrationParams()` (82-90, one covariance per sensor, `kIntegrationVariance = 1e-8`), `preintegrateImu()` (92-108; the single `integrateMeasurement` call at 100-101 with midpoint sampling: the place Phase 4 sets the per-step covariance), `propagateAttitude()` (110-134, forwards and backwards, IMU-gap check).
- `src/fusion/initializer.h` / `.cpp` — `InitialAttitude`, `initialAttitude()` (123-130), `initialValues()` (132-153: `B(0)`, per-fix `X(k)` carried by the gyro, `V(k)`), `coarseAttitude()` (90-100: the coarse rule Phase 5 keeps for anchors), `rotationAligning()`, `bestStationaryWindow()` (41-63, retired by Phase 5).
- `src/fusion/stationarywindow.h` / `.cpp` — the detector and its fourteen gates (`kMaxMeanRate` at line 26); deleted by Phase 5.
- `src/fusion/samplestatistics.h` / `.cpp` — trimmed means and quantiles; also used by `fusionsamples.cpp`, so it stays.
- `src/fusion/trajectoryreconstruction.cpp` — `reconstructTrajectory()`: dense output from the fitted states; unchanged in shape, but it reads the fitted bias (Phase 6 must hand it the per-interval bias consistently).
- `src/fusion/fusionoutput.h` / `.cpp` — `successDiagnostics()` (84-110, the key list), `seedSummary()`, `residualArray()`, `failureDiagnostics()`, `toCompactJson()`; `algorithm` string at line 17. Every diagnostics addition lands here.
- `src/fusion/inputadapter.h` / `.cpp` — `prepareInput()` (94-117: check order, epoch, `usableStart`), `appendImuSamples()` (66-76: deg/s to rad/s), `inputAudit()` (79-90). Phase 6 adds the temperature channel here.
- `src/fusion/fusionregistration.h` / `.cpp` — `kFitInputs[]` (62-80) and `fitInputs()` (86-96): the declared inputs of the explicit calculation; `channelsFrom(ctx)` (109-124): session values to `Channels`; `computeFit()` (147-164): progress and cancel wiring, `Cancelled` to `CalculationCancelled`; `registerFusionCalculations()` (238-243). Phase 2 reuses the channel assembly; Phase 6 adds `IMU/temperature` to the inputs.

### Session import, conversion and calculations (for the runner)
- `src/dataimporter.h` / `.cpp` — `DataImporter::parseFile()` and `applyCreationDefaults()`: model-free parsing of a FlySight CSV.
- `src/sessionmerge.h` / `.cpp` — merging parsed files into one session without the model.
- `src/sessionimport.h` / `.cpp` — `SessionImport::importFiles(SessionModel &, paths)` and `failureMessage()`: the application's widget-free import driver (needs a `SessionModel`, which touches the logbook and preferences).
- `src/sessionmodel.cpp` — `LogbookManager::instance()` and `PreferencesManager::instance()` uses (lines 42, 114, 122, 162, 466, 554, 739, 757, 861): what a headless tool must avoid or isolate.
- `src/conversion/schematable.cpp` — `kLegacyGyroScale = 1.14688` (line 15) and `Schema::correctionScale()`; `src/conversion/sourceconversion.cpp` lines 99 and 137 apply it at read time. The runner gets it for free by reading effective values through the conversion layer.
- `src/calculations/builtincalculations.h` — `registerBuiltInCalculations()`; registration order in `src/mainwindow.cpp` lines 170-180 (built-ins first, then fusion).
- `src/dataexporter.cpp` — column order of the native CSV per sensor (lines 31-34), for a test that writes a fixture out as `TRACK.CSV` / `SENSOR.CSV`.
- `src/sessiondata.h` — `SessionKeys::FusionDiagnostics` (line 57).

### Tests and goldens
- `tests/CMakeLists.txt` — `flysight_add_test()` (65-116), the fusion block (275-443): `flysight_add_fusion_test()` (292-301: 64 MiB stack, solver DLLs, labels, `/bigobj`), `flysight_fusion_test_support` (308-316, `FLYSIGHT_FUSION_GOLDEN_DIR`), `flysight_fusion_session_support` (322-325), the exact-mode registration (335-433, `FLYSIGHT_FUSION_EXACT_TESTS`, reads `capture.json` at configure time), `solver_deploy_probe` (439-443: a non-test executable inside the block, the pattern for the capture tool and the runner).
- `tests/fusion/fusionfixtures.h` / `.cpp` — the twelve synthetic fixtures and their bit-reproducibility rules (lines 5-15); the place new synthetic recordings of spec section 10 are generated.
- `tests/fusion/fusiongolden.h` / `.cpp` — golden loading (`fromHexBits()` 38-47), comparison (`compareSamples()` 244, `compareJson()` 285), tolerance policy (`kPortable*` in the header 53-57), exact mode (`exactParityRequested()` 207-210). The capture tool writes what these read.
- `tests/data/fusion/` — `<fixture>.json`, `<fixture>.channels.txt`, `capture.json` (formats in `tests/README.md` lines 854-882).
- `tests/README.md` section 11 (lines 803-1467) — golden parity: fixtures, files, tolerance policy, solver configuration, the out-of-tree capture procedure (1032-1407, retired by Phase 1), fusion sessions, real recordings.
- `tests/tst_fusion_kernel.cpp` — internal tests: `stationaryGates` (328) and `stationaryScanPollsSilently` (395) go with the detector; `shortInputUsesCoarseInitializer` (457), `fitTraceMatchesGolden` (491-517), `nonConvergenceIsSolverFailure` (518-540, forces `maxIterations = 1`), `preintegrationHonoursExactBoundaries` (197), `headingIsUnconstrained` (282).
- `tests/tst_fusion_parity.cpp` — public-API golden tests: `progressMatchesReferenceBoundaries` (344), `cancelAtEachKindOfBoundary` (367), `twoRunsAreBitIdentical` (455), determinism and thread independence.
- `tests/tst_fusion_session.cpp` — fusion as a registered calculation on real sessions, fit inline on the main thread (143-163), `naturalSessionEndToEnd` (162).
- `tests/tst_fusion_jobs.cpp` — real fits on the queue worker; `realRecordingCheck()` (682-722): `SessionImport::importFiles` on `FLYSIGHT_FUSION_RECORDING`, the existing CSV-import-then-fit pattern.
- `tests/fusion/fusionsessions.h` / `.cpp` — `registerFusionOnce()` (98), `fixtureSession()` (124), `naturalSession()` (131), `goldenDifference()` (247).
- `tests/support/testenvironment.h` / `.cpp` — isolates QSettings and the logbook in a temporary directory (17-24, 38-50).
- `tests/support/testmain.h` — bare `QCoreApplication` main.
- `tests/tst_importer.cpp`, `tests/tst_import_batch.cpp`, `tests/tst_csvformat.cpp` — import and CSV format tests.

### Build
- `src/CMakeLists.txt` — `flysight_fusion` (line 331 onward, GTSAM PRIVATE, `flysight_assert_solver_confinement()`), `flysight_core` (264), `tests/` inclusion under `FLYSIGHT_BUILD_TESTS`.
- `cmake/SolverDependencies.cmake` — `flysight_solver_stack()`, `flysight_solver_test_environment()`, the `gtsam` target.
- `README.md` — configure and build; `tests/README.md` sections 2-4 — configure, build and run the tests.

### Documentation
- `docs/SENSOR_FUSION.md` — section 4 (model, convergence at 98-104, noise model 93-106, diagnostics keys 135-147), section 5 (initialization and limitations, 148-177), section 6 (rejections), section 8 (validation).
- `docs/DATA_SCHEMA.md` — `IMU/temperature` column (line 26), legacy gyro correction (96-110).

### Behavioral references outside the tree (untracked, on Michael's machine; read for the algorithm, never copy the style)
- `experiments/fusion_lab/fusion_lab.cpp` — lines 165-241: the grown bootstrap with four yaw starts and the marginal yaw sigma (`gtsam::Marginals` with QR at 215-222: rotation block of `X(0)`'s 6x6 covariance rotated into the navigation frame, vertical element's square root); lines 275-299: initial values stitched from segment solutions.
- `experiments/fusion_lab/fusion/imuintegration.cpp` lines 96-111 — the per-step covariance set before each `integrateMeasurement` (`density^2 + s^2 dt`).
- `experiments/fusion_lab/fusion/fusionsamples.h` lines 35-51 — the lab's extra tuning fields.
- `experiments/fusion_lab/NOTES.md` — the evidence behind the spec (sections 4.5, 4.6, 4.8, 5).
- `TEMP/data/...` — the four reference recordings of spec section 12, for manual runs with the Phase 2 runner.

## Decisions & Constraints

### Goldens and tests
- **Goldens come from the product kernel from Phase 1 on.** The out-of-tree harness of `tests/README.md` compiled the reference branch and cannot capture the changed kernel, so Phase 1 adds `fusion_golden_capture` (a non-test executable in the `FLYSIGHT_BUILD_FUSION_TESTS` block, pattern `solver_deploy_probe`) that links `flysight_fusion_test_support`, runs `Fusion::run()` on every fixture and writes `<fixture>.json`, `<fixture>.channels.txt` and `capture.json` in the formats `fusiongolden.cpp` reads. Its first run must reproduce the committed goldens bit for bit on Michael's machine (MSVC 19.44 x64, Release); that is Phase 1's proof and the only time the tool's output is compared with the branch's.
- **`capture.json` keeps `compiler.cl_version`** (the configure-time gate of the exact tests) and records this repository's revision, the tool's build configuration and the file hashes; the branch revision, cross-check and `batchfusion.cpp` command line are dropped. The exact-mode tests stay: on the capture configuration they prove that a rebuild is bit-identical to the capture, which is what catches an unintended numerical change.
- **Every numerical phase ends with a re-capture task**: build, run `fusion_golden_capture` into `tests/data/fusion/`, reconfigure (the gate reads `capture.json`), run `ctest -C Release -L fusion` including `-L exact`, and report the golden files among the phase's files. `tst_fusion_parity` is renamed by Phase 1 to what it now is (a golden regression test against the product kernel; the documenter chooses the name and updates the CMake and README references); its determinism, cancellation and thread tests are unchanged.
- **The three success fixtures stay** (`coarse_linear`, `coarse_maneuver`, `stationary_spin`) as golden regression fixtures; `stationary_spin` no longer asserts a stationary window once Phase 5 lands, only its golden. The nine rejection fixtures are unchanged throughout. The synthetic recordings of spec section 10 are new fixtures in `fusionfixtures.cpp`, obeying its bit-reproducibility rules, with expected values stated in the tests, not goldens.
- **Verification commands** (Michael's machine): the configured test build is `build-phase1/` (`FLYSIGHT_BUILD_TESTS=ON`, `FLYSIGHT_BUILD_FUSION_TESTS=ON`, third-party OFF, GTSAM from `build-solver-deps/GTSAM-install`). Build with `cmake --build build-phase1 --config Release`, test with `ctest --test-dir build-phase1/FlySightViewer-build -C Release -L fusion --output-on-failure` (and the whole suite without `-L`). Do not rebuild `build/`: it has third-party ON and would rebuild the solver.
- Fusion tests run under a 600 s timeout each; new synthetic recordings must keep fits to seconds (a few hundred to a few thousand states, 25 Hz IMU at most).

### Diagnostics key layout (binding for Phases 3-6, so their additions never collide)
Every key `successDiagnostics()` writes today stays with its meaning (tests and goldens depend on them; nothing outside `src/fusion/` reads individual keys). New top-level objects, each owned by one phase:
- `"stopping"` (Phase 3): `"rule"` one of `"settled"`, `"slow tail accepted"`, `"iteration limit"`, `"cost increased"`; `"passes"`; `"last_pass_mean_relative_decrease"`; `"repreintegration_cost_difference"` (relative, of `max(1, cost)`); `"bias_settled_tolerance"` and `"slow_tail"` with its two thresholds and the window length.
- `"quality"` (Phase 3): `"imu_nrms"`, `"position_nrms"`, `"velocity_nrms"`, `"objective_per_state"`.
- `"model"` (Phases 4 and 6): `"per_step"` with `"gyro_slope_s"`, `"acc_slope_s"` (Phase 4); `"gyro_bias"` with `"b0_rad_s"`, `"b1_rad_s_per_degc"`, `"t_ref_degc"` (Phase 6; always numbers, since the temperature is a required input). The fitted accelerometer bias stays where it is today (`seeds[0].acc_bias_m_s2`).
- `"initializer"` (Phase 5): `"segment_length_s"`, `"segments"` array with per segment `"index"`, `"start_s"`, `"end_s"`, `"anchor_s"`, `"anchor_sacc_m_s"`, `"prefix_length_s"`, `"yaw_sigma_deg"`, `"prefix_fits"` (count), `"prefix_iterations"`, `"prefix_passes"`, `"prefix_on_limit"` (bool), `"segment_on_limit"` (bool), `"growth_stop"` (`observable`, `covers`, `no_gain`, `all_failed`), `"imu_nrms"`, `"restarted"`, `"restart_kept"` (bools), `"restart_reason"` (`""`, `fallback`, `nrms`, `no neighbour`), `"iterations"`, `"fallback"` (bool), and `"fallback_segments"` (array of indices). The legacy `"initialization"` string then names the segmented method; `"stationary_interval_s"` and `"anchor_time_s"` become `null`.
- The failure diagnostics stay `{algorithm, failure}`; a solver failure additionally carries `"stopping"` when the fit reached the end of a pass (Phase 3 decides the exact shape and documents it). The `"algorithm"` string changes to `batch-shared-bias-v2` in Phase 3 (first numerical change) and to `batch-temperature-bias-v3` in Phase 6.
- The progress texts are part of the goldens. Phase 5 defines the segment texts as `Segment %1 of %2: prefix %3 s, pass %4, iteration %5` and `Segment %1 of %2: pass %3, iteration %4`; the full fit keeps `Pass %1, iteration %2`, and "Integrating IMU factors" is reported per graph build as today.

### Fit contract evolution
- Phase 3 keeps `fitFactorGraph()`'s signature and extends `FitResult` (stop rule, last-pass mean relative decrease, re-preintegration cost difference, the three normalized RMS values, objective per state, passes). `Tuning` gains the slow-tail and bias-settled constants as tuning fields with the spec's defaults so tests can force each branch; `maxIterations` stays per pass.
- Phase 4 adds `gyroStepSlope = .026` and `accStepSlope = .40` to `Tuning`; a zero value is exactly today's covariance.
- Phase 5 replaces `InitialAttitude` by an initial-state type carrying one `Rot3` per fix of the fitted window, one gyro bias, and the segment account; `initialValues()` takes it; `fitFactorGraph()` takes the initial state (heading offset folded in by the caller: the four starts of a prefix fit are four initial states). A `yawSigmaDeg(graph, values, key)` helper (marginal covariance, QR) lives next to the fit. `PipelineTrace` carries the segment account instead of `InitialAttitude`; `fitTraceMatchesGolden` and the golden `trace` object are updated accordingly.
- Prefix and segment fits are the production fit routine (`fitFactorGraph()` with the production `Tuning`) on `fittedWindow(recording, start, end)` sub-windows; their boundaries are the same checkpoints with the segment texts; a failed fit (the routine's "Nonfinite or increasing optimizer cost") is caught by the initializer and counted as an infinite-objective start, never surfacing as `SolverFailed` on its own.
- Phase 6's graph shape: **a custom IMU factor** on keys `X(i), V(i), X(j), V(j), B(0), T(0)` where `B(0)` is today's `ConstantBias` (accelerometer bias and `b0`) and `T(0)` is a `Vector3` (`b1`, rad/s per degC). The factor holds the interval's preintegrated measurement, evaluates the bias as `B(0) + [0; T(0) (T_i - T_ref)]` with `T_i` the IMU temperature linearly interpolated at fix `i`, and chains the preintegration's bias Jacobian. No per-fix bias variables, no constrained noise models (GTSAM's Cholesky path cannot eliminate them). Priors: `B(0)` as today; `T(0)` zero-mean, sigma `0.010 deg/s/degC = 1.745e-4 rad/s/degC` per axis. Re-preintegration between passes and the rebuild at the fitted bias evaluate each interval at its own bias. The temperature is a required input, so the full fit always carries `T(0)`; only segment and prefix fits use the stock path (`b1` fixed at zero, internal). `T_ref` is the mean of the IMU temperature samples inside the fitted window's IMU range.
- The kernel's purity, threading and cancellation rules (spec section 9) are unchanged; nothing outside `src/fusion/` includes a GTSAM header; the runner and the capture tool use the public `fusion.h` only.

### Runner
- `fusion_runner` is a non-test executable in the `FLYSIGHT_BUILD_FUSION_TESTS` block, linking `flysight_core` and `flysight_fusion` (through the registration's public header), with the 64 MiB stack the fits need (`flysight_solver_stack()`), no install rule.
- It must produce exactly the channels the job queue feeds the kernel. The Phase 2 documenter chooses between (a) the model-free path (`DataImporter::parseFile` for both files, `sessionmerge` into one `SessionData`, the calculation engine with built-in and fusion calculations registered, then the registration's channel assembly exposed as a public function that `computeFit()` also uses) and (b) `SessionImport::importFiles` on a `SessionModel` with QSettings and the logbook isolated to a temporary directory the way `tests/support/testenvironment` does. (a) is preferred when the engine can be driven without the model; either satisfies "no user logbook or settings".
- Output: compact diagnostics JSON on stdout (the `Result::diagnosticsJson` string, unchanged), progress texts on stderr, `--csv <path>` writes the seventeen channels with a header line, exit 0 only for `Succeeded`, otherwise the outcome and reason on stderr and a non-zero status (one per outcome).
- Its test writes a synthetic fixture as native `TRACK.CSV` / `SENSOR.CSV`, runs the tool through `QProcess`, and checks that the diagnostics equal a direct `Fusion::run()` on the fixture's channels through the golden comparator, that the CSV parses back to the result's channels, and the exit statuses of a success and a rejection.

### Risks and open questions
- Re-captured goldens are validated on Michael's compiler only; the portable-tolerance comparison on macOS and Linux CI is exercised after the branch is pushed, outside this plan. The tolerance constants are unchanged, and each phase's document must keep fixture fits small so the tolerances stay meaningful.
- The temperature model (Phase 6) is untested in the graph; its acceptance is the spec's synthetic drifting-bias recording (`b1` within 20 %), the constant-temperature case, and the rejection of a recording without the channel. If the custom factor's Jacobians are wrong the drifting-bias test will not converge within 30 iterations; the implementer should verify them numerically in a unit test before the fit test.
- `fittedWindow()` keeps one IMU sample before and after a segment, so consecutive segments share IMU samples at their seams; segments are cut on GNSS fixes and every fix belongs to exactly one segment.
- The four reference recordings of spec section 12 are not automated; Michael runs them with the runner after Phase 5 and after Phase 6 and compares with the expected objectives in the spec.

## Integration Notes (binding corrections)

Recorded after the phase documents were written. Where a note conflicts with an earlier section of this overview, the note wins; where it conflicts with a phase document, the phase document wins (it is the more detailed statement).

1. **The capture tool uses the internal seam.** The overview said the runner and the capture tool use the public `fusion.h` only. That holds for `fusion_runner`. `fusion_golden_capture` must write the golden `trace` object, which exists only through `Detail::runPipeline(..., PipelineTrace*)`, so it includes `fusion/fusionpipeline.h` and links `gtsam` like `tst_fusion_kernel`, and joins `_FLYSIGHT_GTSAM_NAMERS`; `fusion_runner` and `tst_fusion_runner` join `_FLYSIGHT_GTSAM_REACHERS` only. Both lists live in `cmake/SolverDependencies.cmake`; Phases 1 and 2 append to them and never reorder (Phase 1 doc Task 1.2, Phase 2 doc Task 2.4).
2. **Phase 1's first capture changes eighteen progress lines.** The committed goldens hold the reference branch's progress wording, which the old parity test translated before comparing. The tool writes the kernel's own texts, so the proof of Phase 1 is "every number, trace row, channel bit and rejection file bit-identical; exactly the enumerated progress lines differ" (Phase 1 doc Task 1.5), not "nothing but `capture.json` changes".
3. **Golden test name:** `tst_fusion_parity` becomes `tst_fusion_golden` (class `FusionGoldenTest`); function names are unchanged so acceptance references stay valid.
4. **Stop rules are five, not four:** `settled`, `slow tail accepted`, `iteration limit`, `bias not settled` (a fifth pass that settles while the cost test fails), `cost increased`. The slow tail is judged only on the fifth pass when it hit the limit with at least the window's iterations. `Stopping` carries the thresholds copied from `Tuning`; negative tuning values are legal "never" forcings for tests (Phase 3 doc).
5. **`FitFailure`** (a `std::runtime_error` carrying `Stopping`) is the fit routine's failure exception; Phase 5's initializer catches it and counts an infinite-objective start; `FusionCancelled` is never caught there.
6. **Initial state versus account (Phase 5):** `InitialState` (one rotation per fix, one gyro bias) is what the fits take; `Initialization` (state plus the segment account) is what the initializer returns and `PipelineTrace` carries. `fitFactorGraph(samples, initial, tuning, passFormat, checkpoint)` takes a `QString` format with two remaining placeholders for pass and iteration; the full fit's default reproduces `Pass %1, iteration %2` byte for byte. `FitResult::graph` is the rebuild at the fitted bias; `yawSigmaDeg(graph, values, key)` is computed from it, capped at 180 degrees. `segmentLength` (600 s) and `minFinalSegment` (120 s) are `Tuning` fields so the multi-segment fixture can use 60 s segments.
7. **The initializer runs inside the fit stage**, after `Starting fit`; preparation reports and polls nothing; `Checkpoint::pollCancel()` is deleted. The `run()` comment in `fusion.h` and the `Checkpoint` comment are rewritten by Phase 5 (Task 5.10); `cancelDuringPreparation` keeps its name with inverted meaning.
8. **Retired diagnostics keys** `stationary_interval_s`, `anchor_time_s`, `selected_heading_deg` and `seeds[0].heading_deg` become `null` and stay present; `initialization` reads `segmented initialization; heading from segment fits`. The failure shapes are Phase 3's and gain nothing in Phases 5 and 6.
9. **The four initializer recordings are not golden fixtures**: `initializerFixture(name)` beside `fusionFixtures()`; the twelve golden fixtures and the "twelve fixtures, sixteen files" checks are untouched by Phases 5 and 6. Their expectations live in `tst_fusion_kernel`.
10. **Temperature is a required input (decided after the first draft of the phase documents).** The engine has no optional-input concept, and Phase 6's first draft added one so a recording without `IMU/temperature` could still fit. The spec's author withdrew that clause: the column is in every FlySight 2 `SENSOR.CSV` and every corpus recording, so a file without it is hand-made and is blocked like any file missing a declared input. Nothing outside `src/fusion/`, the registration and the tests changes. Costs: every golden fixture gains a constant temperature column, and so do the session builder (`sessionFromFixture()`) and the runner test's fixture-to-CSV writer; the spec's "fits with `b1` fixed at zero" case becomes "rejected / blocked"; `b1_fixed_zero` and the audit's presence flag do not exist. With `T(0)` and its prior in every full-fit graph, Phase 6's re-capture may move numbers in the last digits (the Phase 6 document states the expected diff).
11. **Temperature model shape (Phase 6):** `TemperatureImuFactor` in `src/fusion/temperatureimufactor.{h,cpp}` on `X(i), V(i), X(j), V(j), B(0), T(0)`; the interval between fixes `k-1` and `k` uses fix `k-1`'s interpolated temperature (the `ImuFactor` `bias_i` convention) in the fit and in the reconstruction; `T_ref` is computed in `planFit()` and travels in a `GyroBiasModel` that `fitFactorGraph()` takes as a trailing parameter defaulting to the constant model, so the initializer's fits are stock without touching `initializer.cpp`. The `b1` prior's residual kind is `slope_prior`; `t_ref_degc` and `b1_rad_s_per_degc` are always numbers.
12. **Runner (Phase 2):** model-free import (`DataImporter::parseFile` + `SessionMerge` + the engine on a bare `SessionData`), never instantiating `PreferencesManager`, `LogbookManager` or `SessionModel`, with a defensive `QSettings` redirect to a temporary directory; the registration header exposes `fitInputs()`, a reader-based `channelsFrom()` and `fitOutputChannels()` so the runner, `computeFit()` and Phase 6's new input share one table; `--dump-inputs` proves the legacy gyro scale; exit codes 0 Succeeded, 1 Rejected, 2 SolverFailed, 3 import failure, 4 output not written, 5 internal, 64 usage and `--help`.
13. **Per-step term (Phase 4):** `successDiagnostics()` receives the `Tuning` (added by Phase 4 unless Phase 3 already routed it); `modelSummary()` builds the `model` object; `coarse_linear`'s numbers must be byte-identical after Phase 4's re-capture (its signal change is zero), which the phase checks mechanically.
14. **Expected golden diffs per phase** are stated in each phase's re-capture task (which files change, `git diff --numstat` expectations). A capture that changes a file the phase says must not change is a defect of that phase, never a reason to edit a golden.
15. **Order of implementation:** 1, 2, 3, 4, 5, 6, 7. Phases 1 and 2 may be implemented in either order; everything else is sequential.
16. **Phase 7 owns the gaps no phase claimed:** `docs/CALCULATIONS.md` section 17 and `docs/SENSOR_FUSION.md` section 7 still count "21 inputs, all required" after Phase 6 (which edits section 3 only); the cleanup audit (`cleanup_audit.cmake`, target `audit_cleanup`) gains the `fusion-model` and `fusion-tooling` pattern groups and the acceptance range 201-247; the expected end state is 49 registered tests (13 fusion, 6 exact) and 14 targets reaching GTSAM in the configure log (Phase 7 doc Tasks 7.2, 7.5, 7.7).

17. **Prefix growth stop, prefix budget and on-limit starts (spec 3.2 d and 3.3 Budgets, added to the spec after Phase 5 was first written; Phase 5 amended on 2026-09-22).** Growth stops when a doubling cuts the yaw sigma by less than 20 % (`kGrowthMinGain = .2`, `growth_stop` = `no_gain`); a prefix fit runs with `maxIterations = 50`, `maxPasses = 1` (a new `Tuning::maxPasses`, default 5, replaces `kMaxBiasPasses`); Phase 3's stopping rule applies to prefix and segment fits through the shared routine, and a fit that ends on the limit is still the start (`prefix_on_limit`, `segment_on_limit`). The `rest_throughout` fixture is 300 s so that the growth stop is exercised; the test is `atRestPrefixStopsGrowing`, and `startsOnTheLimitAreStillUsed` covers the budgets.

18. **Segment check with neighbour restart (spec 3.2 step 3, added 2026-09-23 after the full-corpus run).** Two of 82 recordings converged into a worse family because one segment's own fit did; refitting that segment from its neighbour's boundary attitude repairs both to the baseline's solution. Rule: with two or more segments, a segment that fell back or whose IMU normalized RMS exceeds twice the median of the fitted segments' is refitted once from its neighbour, the lower-misfit result kept; keys `imu_nrms`, `restarted`, `restart_kept`, `restart_reason` (and `restart_nrms` in the trace); tests `suspectSegmentIsRestartedFromItsNeighbour`, `restartDecisionUsesTwiceTheMedian`. Phase 5 step numbering follows the spec (check is step 3, initial values step 4, full fit step 5).

## Commit Policy

Michael has authorized commits for this plan on a new working branch. The
implementation orchestrator makes every commit; implementation, revision, and
review agents never run git commands that change repository state. Nothing is
ever pushed, and nothing is ever committed on `master`.

- **Branch.** Confirm the working tree has no modified tracked files (if there
  are any, stop and ask). Create with `git switch -c fusion-improvements
  master`; if it exists, switch to it; never recreate, reset, or rebase it.
- **Never staged:** `PLANS/`, `TEMP/`, `experiments/`, `build*/`, `dist/`,
  `results/`, and everything under `third-party/` that is not tracked.
- **One commit per accepted phase**, when the review agent returns ACCEPT,
  containing exactly the files the phase's agents reported, staged by explicit
  path. Subject `Phase N: <phase name>`; body a short summary, then the
  session's attribution line. Rejected iterations are never committed.
- **Tag** each phase commit `plan/fusion-improvements/phase-N-done`; tags are
  never moved or deleted.
- **Fixes to a closed phase** are committed separately as
  `Phase N fixup: <what>` once reviewed and accepted.
- **CI workflow edits** are committed separately as `Phase N: CI (unverified)`.
- **Escalated phases** are not committed; their paths are listed in the final
  report, and phases that depend on them are not started.

### Plan-specific notes
- Re-captured golden files under `tests/data/fusion/` are part of the phase that re-captured them and are committed with it.
- The untracked `experiments/` and `TEMP/` trees are read-only references for the agents and are never staged.
