# The scale factors as a refinement: held until the fit is stable, then released

Date: 2026-10-03; amended 2026-10-04 after the implementation's measurement
(the release budget is three passes, not two) and review (any exception
inside the released stage is the fallback).
Status: specification for planning. Not an implementation plan. Small enough
to be implemented directly by one agent in one phase.
Baseline: branch `store-requested-calculations` at `fa00b4a`, whose code is
`1c6d5c6`'s; the committed code is authoritative.
Related: `src/fusion/factorgraphfit.h/.cpp` (`fitFactorGraph`, the pass
loop, the scale prior from `sensitivityTolerance`, `Stopping`, `StopRule`),
`src/fusion/fusionsamples.h` (`Tuning`), `src/fusion/fusion.cpp` (the full
fit's call), `src/fusion/fusionoutput.cpp` (the diagnostics, the algorithm
string), `src/fusion/fusionpipeline.h` (`PipelineTrace`),
`tests/tst_fusion_kernel.cpp` (`withScaleHeldAtOne`,
`scaleRecordingRecoversTheFactor`, `slowTailAtTheIterationLimit`),
`tests/README.md` (section 12, the M49 and M55 steps), `docs/SENSOR_FUSION.md`
sections 4, 7 and 8, `docs/DATA_SCHEMA.md` section 12.

## 1. Motivation

The full fit carries one scale factor per axis per sensor, free from its
first pass under a 1 % prior. On `24-09-04/13-35-10` that freedom is the
failure: the initializer hands the fit a poor attitude for one segment, and
in the first pass Levenberg-Marquardt finds that collapsing the x
accelerometer factor to -0.058 lowers the cost faster than straightening the
attitude; from there, with the readings divided by a factor near zero, it
cannot climb back, and five passes of a hundred iterations end at the
iteration limit with an IMU normalized RMS of 1707. Two probes settle where
the fault lies: the whole recording with the factors held at one converges
in two passes and 33 iterations, every factor then within 0.04 % of one and
the IMU normalized RMS 0.45; the whole recording from the coarse start of
every segment with the factors free does not converge at all.

The factors are worth keeping. On the reference recordings they lower the
objective by 4 to 17 % on three and by a factor of nearly five on
`08-35-41`, where the IMU normalized RMS is 0.14 with them and 0.49 without.
What must change is when they are free: a global multiplicative parameter
must not be fitted from a trajectory that has not settled. The fit therefore
finds its stable solution first, with the factors held, and releases them
from it as a refinement.

## 2. Principles

- **A refinement starts from a solution.** The factors are released only
  from a fit that has converged with them held: every pass settled and the
  rebuild at the fitted bias agreeing with it, under the rules that decide
  convergence today.
- **One graph, one prior per stage.** "Held" is the prior on the scale state
  a thousand times tighter than the datasheet's, which the kernel's tests
  already use to hold the scale at one; the graph keeps its shape and the
  state its place, and the stage decides the prior's sigma. Nothing is
  pinned by a second mechanism.
- **The release is judged, not trusted.** The released stage is a fit under
  the same stopping rules as any pass sequence. When it converges, its
  solution is the result. When it does not, the held solution is the result,
  reported as converged with its factors at one, and the diagnostics say the
  factors were not released and why. A refinement never turns a converged
  fit into a failure.
- **A fit that has diverged stops.** A pass whose rebuilt graph shows the
  IMU misfit far outside anything the stopping rules accept, or a factor
  outside any plausible range, ends the fit at once: in the held stage as a
  solver failure, in the released stage as the fallback to the held
  solution. Hours are not spent on a trajectory that is already lost.
- **Nothing changes for a recording whose factors stay at one.** A resting
  recording's factors sit at their prior in either stage; its released
  stage settles in a few iterations and reports what it found.

## 3. What changes

- **The full fit runs in two stages.** The pass loop of `fitFactorGraph` is
  unchanged in kind: passes, each settled or not, each followed by the
  rebuild at the fitted bias and scale and its cost test. The full fit runs
  it twice: the held stage from the initializer's start with the scale
  prior's sigma divided by a thousand, then, if that stage converged, the
  released stage from the held stage's values with the datasheet's sigma.
  The initializer's prefix and segment fits, which have no scale state,
  are untouched. The held stage has the tuning's pass budget (`maxPasses`)
  and per-pass iteration budget, as the single fit has today; the released
  stage has a budget of its own, `releasePasses`, three: it starts from a
  solution, and on the reference recordings the release's second pass
  settles while its rebuild still moves the cost, so the third pass, of one
  or two iterations, is what proves it settled; what three passes cannot
  settle, five will not, so a release that is not going to converge falls
  back in minutes, not an hour.
- **The release trigger is convergence.** The released stage runs only when
  the held stage ended `settled` or `slow tail accepted`. A held stage that
  ends any other way ends the fit as it ends today, under the same rule and
  with the same reason.
- **The fallback.** A released stage that ends `settled` or `slow tail
  accepted` is the fit. One that ends under any other rule, or diverges, is
  discarded, and so is one that throws anything from inside it (a solver
  failure of a pass, or any other exception but a cancellation or memory
  exhaustion): the held stage's values, graph, objective, residuals and
  quality are the fit, `converged` is true, the stopping account is the
  held stage's, and the diagnostics record the release's outcome, the rule
  it ended under or the text of what it threw.
- **Divergence between passes.** After every pass's rebuild, in either
  stage, the IMU normalized RMS of the rebuilt graph and the six factors are
  checked against two new tuning bounds: `divergenceMaxImuNrms`, 10, and
  `divergenceScaleRange`, the factors' allowed interval, [0.5, 2]. A pass
  beyond either ends the stage at once under a new rule, `diverged`. In the
  held stage it is a solver failure, with the failure diagnostics of a
  completed pass (`stopping` and `quality`); in the released stage it is the
  fallback. The bounds are far outside anything a slow tail accepts (the
  normalized RMS bound is 2) and anything a datasheet tolerance of 1 %
  admits; they catch a fit that has left the model, not one that is slow.
  A test may set them to impossible values, as it sets the other bounds,
  and the stopping account reports them beside the other thresholds in
  force, as its contract requires.
- **The pass numbering and the boundaries.** Passes are numbered across
  both stages in the trace and the progress texts, so the released stage's
  first pass is one more than the held stage's last; the boundary before it
  reports "Releasing the scale factors", a cancellation boundary like every
  other.
- **The diagnostics.** `stopping` and `quality` describe the reported fit.
  A new object, `scale_release`, carries the account: `held` with the held
  stage's `rule`, `passes`, `iterations` and `objective`; `released` with
  the same four for the released stage when it ran, or `null`; `kept`, true
  when the released stage is the fit; and `reason`, the rule the released
  stage ended under when it was discarded, or `null`. `model.scale` reports
  the factors of the reported fit, ones with the held stage's sigmas when
  the release was discarded. `seeds[0].iterations` counts both stages.
- **The algorithm string becomes `batch-temperature-bias-v9`.** Every fit's
  numbers move, since the released stage starts from the held solution
  rather than the initializer's start; every stored result is dropped once,
  as the documentation of stored results describes.

## 4. Tests

- **The recovery holds.** `scale_recording`, whose accelerometer factor is
  2 % off, recovers it through the released stage as it does today, and the
  position misfit falls against the held stage's: the existing test, with
  the held stage's objective read from the diagnostics as the comparison it
  used to make with a held fit.
- **The resting recording** leaves every factor at its prior through both
  stages, and its released stage settles.
- **The fallback.** The released stage forced to fail (a checkpoint that
  throws at the "Releasing the scale factors" boundary, as the initializer
  tests force a failed start) leaves a converged fit with the held stage's
  values and factors at one, `scale_release.kept` false with the reason,
  and the published channels those of the held fit.
- **Divergence.** `divergenceMaxImuNrms` forced to zero ends the held stage
  under `diverged` as a solver failure with the completed-pass failure
  shape; forced to zero for the released stage alone (the bound set by the
  same checkpoint seam, or a stage-specific tuning the planner settles) it
  is the fallback. `divergenceScaleRange` forced to an empty interval
  behaves the same. The production bounds change nothing on any fixture:
  the goldens prove it.
- **The reference recordings.** A manual step in the form of M49 runs the
  four reference recordings and `13-35-10`, with `scale_release` read from
  each diagnostics: `13-35-10` is expected to converge; the four are
  expected at or near their M49 numbers, the released stage adding a few
  iterations; the numbers go in the report and in the documentation as
  measured.
- **Goldens** captured again under the new string: four fits, ten
  rejections; the capture note records what moved and why.
- The acceptance map opens a new hundred. `audit_cleanup` and the whole
  suite green, the exact tests included.

## 5. Documentation

`docs/SENSOR_FUSION.md`: section 4 describes the two stages, the trigger,
the fallback, the divergence rule and its bounds, with the probe's numbers
on `13-35-10` as the case; the stopping-rule paragraph gains `diverged`;
section 4's key list gains `scale_release` and section 7's history `v9`;
section 8 gains the manual step's numbers. `docs/DATA_SCHEMA.md`
section 12 and `docs/CALCULATIONS.md` section 17 name `v9`.
`tests/README.md`: the kernel test rows, the manual step, the specification's
appendix and matrix.

## Commit Policy

Michael has authorized commits for this plan on the existing working branch.
Nothing is ever pushed, and nothing is ever committed on `master` or
`fusion-improvements`. One commit, staged by explicit path, with the
session's attribution line; `PLANS/` is never staged.
