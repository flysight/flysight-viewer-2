# Uncertainty of the fused attitude and acceleration

Date: 2026-09-28
Status: specification for planning. Not an implementation plan.
Baseline: branch `store-requested-calculations` at the head that implements
`PLANS/fusion-plots.md` (the eight-plot fusion category, the attitude
derivation and the orientation attribute); the committed code is
authoritative. Where this document and the earlier specifications disagree,
this one wins; everything it does not mention stays as they specify.
Related: `docs/SENSOR_FUSION.md` (sections 4 and 8), `docs/CALCULATIONS.md`
(section 17), `docs/COMPUTED_PLOTS.md`, `docs/DATA_SCHEMA.md` (section 11,
the stored record), `tests/README.md` (section 11, the golden fixtures),
`tests/acceptance_map.txt`, `tests/audit/cleanup_audit.cmake`.

This document says what the change must do and the boundaries it must
respect. It avoids class layouts and function signatures unless a name is
part of the observable contract. The planning agent decides phasing; the
implementation agent decides code structure.

## 1. Motivation

The fused accelerations exist because the GNSS ones, finite differences of
position, are too noisy to use. A user who reads a fused acceleration, or a
fused heading, has no way to know how far to trust it: the fit reports a
converged solution and nothing about how tightly the data constrained it.
The GNSS category shows its receiver's own accuracy estimates beside its
values; the fusion category should do the same for what the fit estimates.

The fit already has what it needs. It solves a factor graph whose noise
model expresses the GNSS accuracies, the IMU's per-step noise and the prior
on the biases, and it already computes a marginal covariance once, to
decide whether the initializer's heading is determined. The same machinery,
applied after convergence to every pose, gives the uncertainty of the
fitted attitude; the acceleration is built from that attitude, the
accelerometer and the fitted bias, so its uncertainty follows by first-order
propagation.

## 2. Principles

- **An accuracy is the fit's own statement**, derived from the covariance
  of its solution under the noise model it assumed. It is a measure of how
  well the data constrained the fit, not a calibrated truth, and the
  documentation says so in one sentence.
- **The fit publishes what only the fit can compute**: the attitude
  uncertainty per sample, and the per-fit constants the propagation needs.
  What can be derived from published channels is derived on demand.
- **Uncertainty is a channel like any other**: stored with the fit's
  record, restored with it, listed by the plot list, readable by a column.
- **A fit that cannot compute its uncertainty still succeeds.** The
  uncertainty channels are then absent; nothing else about the fit changes.
- **A fit output changes, so the result version changes**: every stored
  fit is recomputed at the next start, once, and the golden fixtures are
  captured again from the changed kernel by the existing tool.

## 3. Scope

In scope:

- New fit outputs (section 5): per-sample heading and tilt uncertainty, and
  the per-fit accelerometer noise and bias uncertainty.
- Derived accuracies on demand (section 6): the uncertainty of the
  horizontal and the vertical fused acceleration per sample.
- Four plots in the "Sensor fusion" category (section 7).
- The result version bump, the golden fixtures captured again, and the
  record format carrying the new channels (section 8).

Out of scope, unchanged:

- The fit's estimate itself: the graph, the noise model, the initializer,
  the stopping rule, the published position, velocity, acceleration and
  quaternion channels. The uncertainty is read from the converged solution
  and changes no value in it.
- The uncertainty of position and velocity: nothing plots them.
- Validation of the accuracies against an external reference. The
  documentation states what they are; a validation study, if ever wanted,
  is its own work.
- The demand layer, the executor, the status bar, the logbook.

## 4. Terms

- **Tilt**: the angle between the fitted vertical and the true vertical, the
  part of the attitude error that is not about the vertical axis; heading
  uncertainty is the part that is.
- **Uncertainty**: one standard deviation of the quantity, in its own unit,
  as the fit's covariance gives it.
- **Specific force**: what the accelerometer measures, the acceleration
  less gravity, in the device frame.

## 5. What the fit publishes

After convergence, and only for a fit that succeeds, the fit computes the
marginal covariance of every pose node from the converged graph, with the
facility it already uses for the initializer's heading check, and
publishes:

- **Heading uncertainty** per sample, degrees: the square root of the
  rotation covariance's element about the vertical axis of the navigation
  frame, as the heading check computes it for one node.
- **Tilt uncertainty** per sample, degrees: from the rotation covariance's
  two horizontal elements, combined as the documentation states.
- Between pose nodes, the per-sample values are the node values carried
  over the samples of the fit's interval, as the position and velocity
  channels are interpolated onto the IMU samples; the planner decides
  whether holding or interpolating, and the documentation says which.
- **Per-fit constants**: the accelerometer's per-sample noise and the
  fitted accelerometer bias's uncertainty, both in metres per second
  squared, as attributes published with the fit, beside its diagnostics.

The computation runs after the fit's last iteration, inside the same
cancellable job, at the cost of one factorization of the converged graph.
If the factorization fails, the uncertainty channels and attributes are
absent and the fit's other outputs are published as before; the diagnostics
record that the uncertainty was not computed.

## 6. Derived accuracies

On demand, from the published channels and constants, as the other derived
quantities of the fusion category:

- **Horizontal acceleration accuracy** per sample: first-order propagation
  of the tilt uncertainty through the specific force magnitude, combined in
  quadrature with the accelerometer noise and the bias uncertainty.
- **Vertical acceleration accuracy** per sample: the same propagation with
  the horizontal component of the specific force, combined the same way.

The documentation states the two formulas in words and symbols, so that a
reader can judge what they include and what they leave out (the
interpolation between nodes, gravity's own uncertainty, sensor scale
factors), and says that both are one standard deviation.

## 7. What the user sees

The "Sensor fusion" category gains four plots, named as the GNSS accuracy
plots are: Heading accuracy and Tilt accuracy in degrees, Horizontal
acceleration accuracy and Vertical acceleration accuracy in the
acceleration unit. They are absent, like any unavailable value, for a fit
that could not compute its uncertainty. A logbook column over any of them
works as over any fusion value. The first start after the change recomputes
every stored fit; the status bar shows it as it shows any computation.

## 8. The record, the version and the goldens

- The fit's outputs gain the two channels and the attributes; the record
  stores and restores them with the rest, in the record format as it is.
- The fit's algorithm string changes, so every stored fit is stale at the
  next start and is computed again once, by the existing validity rules.
- The golden fixtures are captured again with the existing capture tool
  from the changed kernel; the golden comparison then covers the new
  channels and the diagnostics' note about them. The existing channels
  must be bit-identical to the goldens before the capture, which the
  planner arranges as a check: the uncertainty changes no value of the
  estimate.

## 9. Architecture

- **The kernel** gains the uncertainty computation after convergence, in
  its own step, using the marginals facility it already links; GTSAM stays
  confined to the kernel. The fit's cancellation boundaries are unchanged;
  the new step is not a boundary of its own unless the planner finds it
  long enough to deserve one.
- **The fit calculation** publishes the new channels and attributes; its
  result version follows the algorithm string as today.
- **The derived accuracies** are on-demand calculations in the fusion
  registration, like the derived kinematics.
- **The plot registry** gains the four plots.
- Nothing above the fusion registration changes.

## 10. Tests

- On the committed fixtures, a successful fit publishes finite, positive
  heading and tilt uncertainties for every sample, and the two constants;
  a rejected fit publishes none.
- Scaling every GNSS accuracy in a fixture up scales the attitude and
  acceleration uncertainties up, never down; a fixture with a long
  stationary start has a larger heading uncertainty at its start than a
  fixture that begins in motion.
- The heading uncertainty of the first node agrees with what the
  initializer's heading check reports for the same node.
- The derived accuracies follow their stated formulas on synthetic inputs
  with known answers, and are absent when the fit's uncertainty is.
- The estimate is unchanged: before the goldens are captured again, the
  seventeen existing channels are bit-identical to the goldens on every
  fixture; after, the golden comparison covers the new channels.
- A stored fit from before the change is stale at the next start, and a
  fit stored after it restores the uncertainty channels bit for bit.
- The four plots are explicit-backed like the other fusion plots, and a
  column over one works.
- The audit's confinement rules hold, and the documents describe the
  uncertainty, its formulas and its meaning.

## 11. Documentation

`docs/SENSOR_FUSION.md` (section 4, the new outputs and the diagnostics'
note; a subsection on the uncertainty with the formulas and their meaning;
section 8, validation, saying what is and is not validated);
`docs/CALCULATIONS.md` (section 17, the plots and derivations, the version
bump); `docs/COMPUTED_PLOTS.md` (the four plots as the user sees them);
`docs/DATA_SCHEMA.md` (section 11, the record's new channels and
attributes); `tests/README.md` section 11 (the goldens captured again, and
why) and `tests/acceptance_map.txt` (a new range from 901; amended items
restated "(as amended)").

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
  path. Subject `Fusion accuracy phase N: <phase name>`; body a short summary,
  then the session's attribution line. Rejected iterations are never
  committed.
- **Tag** each phase commit `plan/fusion-accuracy/phase-N-done`; tags are
  never moved or deleted.
- **Fixes to a closed phase** are committed separately as
  `Fusion accuracy phase N fixup: <what>` once reviewed and accepted.
- **CI workflow edits** are committed separately as
  `Fusion accuracy phase N: CI (unverified)`.
- **Escalated phases** are not committed; their paths are listed in the final
  report, and phases that depend on them are not started.
