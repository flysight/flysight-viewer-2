# Sensor fusion: segmented initializer, stopping rule and IMU model

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

## 1. Goal and motivation

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

## 2. Scope

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

## 3. Segmented initializer

### 3.1 What it produces

Initial values for the full graph: an attitude for every pose, the GNSS
position and velocity for every state as today, and one starting bias. The
full fit itself (graph, factors, tuning, pass structure) is unchanged.

### 3.2 Behaviour

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

### 3.3 Rules

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

### 3.4 Evidence

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

## 4. Stopping rule

Today a fit converges when a pass settles (relative cost decrease at or
below 1e-8 of max(1, cost)) and re-preintegrating at the pass's fitted bias
moves the bias by less than 1e-5 m/s2 and 1e-6 rad/s.

### 4.1 Bias-settled test

Replace the bias-shift test by a cost test: after a pass settles, rebuild
the graph at the new bias and evaluate its cost at the pass's values; the
bias has settled when that cost differs from the pass's final cost by at
most 1e-6 relative (of max(1, cost)). The pass structure (at most five
passes, re-preintegration between them) and the settle tolerance are
unchanged.

### 4.2 Slow tail

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

## 5. Per-step IMU noise term

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

## 6. Temperature-dependent gyro bias

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

## 7. Diagnostics

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

## 8. Command-line runner

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

## 9. Boundaries

- Purity, threading and cancellation rules of the kernel are unchanged: a
  function of the channels, no session or GUI object, every solver
  iteration a boundary, cancellation observed at boundaries only.
- The public result contract (channels, outcomes Rejected / SolverFailed /
  Cancelled / Succeeded) is unchanged; a slow-tail acceptance is a
  Succeeded result.
- Nothing outside the fusion library links GTSAM.

## 10. Tests and golden references

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

## 11. Documentation

`docs/` describes the segmented initializer, the stopping rule with its
slow-tail acceptance, the per-step noise term and its meaning at other
output rates, and the temperature-dependent bias, in the place that
documents the fusion model today.

## 12. Reference cases and corpus

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

## 13. Principles

- Start every pose near its answer. Nothing is propagated further than a
  segment.
- The graph is the model; the initializer only chooses where the solver
  starts. Never let an initializer decide a model quantity the fit can
  estimate.
- "Converged" means the cost has stopped moving. Report quality beside it.
- Model what the data determine (bias, its drift) rather than absorbing it
  in noise.
- Keep it small: one initializer path, no gates.

## Commit Policy

Michael has authorized commits for this plan on a new working branch. The
implementation orchestrator makes every commit; implementation, revision, and
review agents never run git commands that change repository state. Nothing is
ever pushed, and nothing is ever committed on `master`. Copy this section
into the plan overview as its own top-level section with this exact heading.

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
