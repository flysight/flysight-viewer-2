# Implementation plan: the scale factors as a refinement

Plan for `PLANS/staged-scale.md`, written 2026-10-03 against
`store-requested-calculations` at `fa00b4a`, whose code is `1c6d5c6`'s (the
commits between are the GNSS-holes specification and plan moving to
`PLANS/done/`); the specification was committed on its own as `3cd22f2`
while the plan was written, and nothing else is between. The specification below is the authority; where the phase
document disagrees with it, the specification wins, except where a decision
below states how the plan reads it.

## Feature specification

## The scale factors as a refinement: held until the fit is stable, then released

Date: 2026-10-03
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

### 1. Motivation

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

### 2. Principles

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

### 3. What changes

- **The full fit runs in two stages.** The pass loop of `fitFactorGraph` is
  unchanged in kind: passes, each settled or not, each followed by the
  rebuild at the fitted bias and scale and its cost test. The full fit runs
  it twice: the held stage from the initializer's start with the scale
  prior's sigma divided by a thousand, then, if that stage converged, the
  released stage from the held stage's values with the datasheet's sigma.
  The initializer's prefix and segment fits, which have no scale state,
  are untouched. The held stage has the tuning's pass budget (`maxPasses`)
  and per-pass iteration budget, as the single fit has today; the released
  stage has a budget of its own, `releasePasses`, two: it starts from a
  solution, and what two passes cannot settle, five will not, so a release
  that is not going to converge falls back in minutes, not an hour.
- **The release trigger is convergence.** The released stage runs only when
  the held stage ended `settled` or `slow tail accepted`. A held stage that
  ends any other way ends the fit as it ends today, under the same rule and
  with the same reason.
- **The fallback.** A released stage that ends `settled` or `slow tail
  accepted` is the fit. One that ends under any other rule, or diverges, is
  discarded: the held stage's values, graph, objective, residuals and
  quality are the fit, `converged` is true, the stopping account is the
  held stage's, and the diagnostics record the release's outcome.
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

### 4. Tests

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

### 5. Documentation

`docs/SENSOR_FUSION.md`: section 4 describes the two stages, the trigger,
the fallback, the divergence rule and its bounds, with the probe's numbers
on `13-35-10` as the case; the stopping-rule paragraph gains `diverged`;
section 7 adds `scale_release` to the key list and `v9` to the string's
history; section 8 gains the manual step's numbers. `docs/DATA_SCHEMA.md`
section 12 and `docs/CALCULATIONS.md` section 17 name `v9`.
`tests/README.md`: the kernel test rows, the manual step, the specification's
appendix and matrix.

### Commit Policy

Michael has authorized commits for this plan on the existing working branch.
Nothing is ever pushed, and nothing is ever committed on `master` or
`fusion-improvements`. One commit, staged by explicit path, with the
session's attribution line; `PLANS/` is never staged.

## Phases

| Phase | Name | Purpose | Dependencies |
|-------|------|---------|--------------|
| 1 | Staged scale | The whole specification: the full fit in two stages, held then released, with the release trigger and the fallback (§3); the `diverged` rule and its two tuning bounds (§3); the pass numbering across the stages and the "Releasing the scale factors" boundary (§3); the `scale_release` diagnostics object (§3); the algorithm string `batch-temperature-bias-v9` and everything that quotes it (§3); the kernel tests of §4 (the recovery through the released stage, the resting recording, the fallback, the divergence forced in each stage), the re-capture of the goldens, the manual step on the reference recordings; the audit group; the acceptance items in a new hundred with their appendix, matrix and manual step (§4); the documentation of §5 and the documents the kernel's changes reach. | none |

```text
1 Staged scale
```

One phase. The specification says it is small enough for one agent in one
phase, and its Commit Policy authorizes one commit; a plan of several phases
would ask the orchestrator for commits the policy does not allow. The change
does not split along a green boundary either: the second stage changes every
fit's numbers, which changes the algorithm string, which changes every golden
and every test and document that quotes the string; the diagnostics object,
the new rule and the boundary are each visible only through a fit that runs
both stages.

## Key patterns and references

Every path is relative to the repository root. The documenter and the
implementer read the whole file where the line says so; elsewhere the line
says what to look at.

### Project rules

- `CLAUDE.md`: build only in `build-agent/`, the suite sequentially, what
  must stay green, the conventions the audit enforces.
- `CLAUDE.local.md`: this machine's trees and git rules (never push; never
  commit on `master` or `fusion-improvements`).
- `.claude/docs/WORKFLOW-REFERENCE.md`: the stages and the conventions on
  altitude, phases and commits.
- `PLANS/done/gnss-holes-bridged-plan/00-overview.md` and
  `01-gnss-holes-bridged.md`: the previous one-phase plan, for the shape of a
  phase document, its acceptance criteria, its tests section and its
  decisions; its decisions 1, 4, 5, 8 and 9 and its "Constraints the code
  imposes" describe the re-capture, the fixture rules and the audit's
  pathspecs, all still true.
- `PLANS/done/gnss-holes-bridged.md` and `PLANS/done/noise-model-part-1.md`:
  the two specifications this one builds on (the slow tail's three bounds;
  the scale state, its prior and `withScaleHeldAtOne`'s thousandfold prior).

### The fusion kernel (`src/fusion/`)

- `src/fusion/factorgraphfit.h` (read whole): the fit's contract. `StopRule`
  (six texts today), `Stopping` (the rule, `passes` as "1..maxPasses", the
  two measurements, the thresholds copied from the tuning), `Quality`,
  `GyroBiasModel` (`temperatureLinear` is what distinguishes the full fit
  from the initializer's fits), `BiasLinearization`, `FitResult` (`values`,
  `converged`, `objective`, `history`, `residuals`, `stopping`, `quality`,
  `graph`, `scale`), `FitFailure` (the two failures a pass cannot continue
  from, thrown with their account), `kFullFitPassFormat` ("Pass %1,
  iteration %2"), and the long contract comment of `fitFactorGraph()`: the
  passes, the cost test, the slow tail, the throwing failures, the
  checkpoint's placeholders, the temperature model with `S(0)`.
- `src/fusion/factorgraphfit.cpp` (read whole): `addScalePrior()` builds the
  scale prior from `c.noise.accelerometer.sensitivityTolerance` and the
  gyro's, the one place the tolerance is read outside the noise unit;
  `thresholdsOf()` and `failedPass()` fill a `Stopping`; `runOptimizerPass()`
  is one pass driven by hand, one boundary per iteration,
  `passFormat.arg(outer+1).arg(i+1)`; `buildFactorGraph()` reports
  "Integrating IMU factors" every 256 states; `fitFactorGraph()` is the pass
  loop: the initial values (`T(0)` zero, `S(0)` ones under the temperature
  model), the graph built once, then per pass the optimizer pass,
  `stopping.passes = outer+1`, the rebuild at the pass's bias and scale, the
  cost test, and after the loop the reported objective, residuals, quality
  and graph from the rebuild and the `bias not settled` / slow tail decision
  with its strict comparisons ("a zero bound refuses deterministically").
  The two stages are this loop run twice; the `diverged` check sits after
  each pass's rebuild.
- `src/fusion/fusionsamples.h`: `Tuning`, with every bound a test may force
  (`lambdaUpperBound`, `relativeTolerance`, `maxIterations`,
  `biasSettledTolerance`, the three slow-tail bounds, `maxPasses` "the full
  fit's and a segment fit's five; a prefix fit's one"): the pattern for the
  two divergence bounds and their comments.
- `src/fusion/fusion.cpp` (read whole): `fitAndAssemble()` calls
  `fitFactorGraph()` once with `plan.biasModel` and `kFullFitPassFormat`,
  fills the trace, and turns a non-converged fit into `SolverFailed` with
  the reason "Batch fusion did not converge (%1); sensor fusion
  unavailable" and the completed-pass failure diagnostics (`stopping` and
  `quality`); `runPipeline()` catches `FitFailure` ahead of `std::exception`
  (a stopping account, no quality) and lets `FusionCancelled` through.
- `src/fusion/fusion.h`: `Algorithm` ("batch-temperature-bias-v8", the one
  literal in `src`), and the contract comment of `run()` that lists the
  kinds of boundary at which cancellation is observed (three today).
- `src/fusion/fusionprogress.h`: `Checkpoint` (report, then ask; throws
  `FusionCancelled`, which is not a `std::exception`) and the comment
  listing every catch in the library.
- `src/fusion/fusionpipeline.h`: `PipelineTrace` (`history`, `converged`,
  `stopping` are the full fit's; the shape is asserted by a test and the
  trace golden) and `runPipeline()`'s contract: `baseTuning` exists so that
  a test can force a stopping rule.
- `src/fusion/fusionoutput.cpp` (read whole): `stoppingObject()`,
  `qualityObject()`, `seedSummary()` (`iterations` is `fit.history.size()`),
  `scaleObject()` (the factors and the sigmas from the covariance step's
  `S(0)` block), `successDiagnostics()` (the top-level key set) and
  `failureDiagnostics()` (`algorithm`, `failure`, then `stopping` and
  `quality` when given): where `scale_release` is written, once.
- `src/fusion/fusionoutput.h`: the contract comments of the two diagnostics
  writers.
- `src/fusion/initializer.cpp`: `fitOrFail()` wraps `fitFactorGraph()` for
  the prefix and segment fits under the constant model and turns a
  `FitFailure` into a start with infinite objective. These fits are
  untouched: no scale state, one stage, no divergence check.
- `src/fusion/fitcovariance.h/.cpp`: the covariance step reads `fit.graph`
  and `fit.values`; on the fallback it reads the held stage's graph, whose
  scale prior is the tight one, which is what gives "the held stage's
  sigmas" under `model.scale`.
- `src/fusion/scaledimufactor.h`: the scaled IMU factor; nothing changes,
  listed so that the implementer knows the prior's sigma is the only knob.

### Tests of the kernel

- `tests/tst_fusion_kernel.cpp` (read the parts named): the provenance
  comment at the head; `runInitializerFixture()`, `fitOfChannels()`,
  `fixtureFit()` and `withScaleHeldAtOne()` (the thousandfold prior, the
  seam the held stage reuses; after this change it tightens both stages, so
  its one remaining user, the constant-temperature consistency test, is
  re-read); `initializerDiagnosticsShape` (the success diagnostics' sorted
  top-level key list, which gains `scale_release`); `fitTraceMatchesGolden`
  (the trace against the golden); `biasSettledByCostTest` (relates the
  distinct `outer` values of the history to `stopping.passes` and the last
  history row to the objective: both are per-stage facts now);
  `slowTailAtTheIterationLimit` (forced never-settling passes, 125
  iterations and `passes` 5: with the accepted row the held stage converges
  and the released stage runs too); `nonConvergenceIsSolverFailure`,
  `biasNeverSettlesIsSolverFailure` and `kCompletedPassFailureKeys` (the
  completed-pass failure shape `diverged` takes in the held stage);
  `failureDiagnosticsShape` and `dampingSaturationIsASolverFailure` (the
  thrown failures' shape); `allPrefixFitsFailFallsBack` and
  `prefixFitsFailAfterACompletedLength` (the forcing pattern: a checkpoint
  whose progress function throws `FitFailure` at a chosen boundary text,
  the pattern the fallback test follows at "Releasing the scale factors");
  `restLeavesTheScaleAtItsPrior` (the resting recording, each factor within
  its prior); `scaleRecordingRecoversTheFactor` (the 2 % factor recovered,
  the position misfit at most 0.9 of the held fit's: the held fit becomes
  the held stage's account); `diagnosticsReportTheScale` (`model`'s key set,
  which does not change: `scale_release` is top-level);
  `imuRateIsWhatTheFitPublishes` (the pipeline's channels bit for bit
  against the test's own fit through `fixtureFit()`, so both must run the
  same stages).
- `tests/tst_fusion_golden.cpp`: `cancelAtEachKindOfBoundary` (one row per
  kind of boundary, by text; the new boundary is a kind) and the golden
  comparisons; `successFixturesMatchGolden` compares the whole diagnostics
  object, key set included.
- `tests/fusion/fusiongolden.cpp`: `isExactKey()` (`passes` and
  `iterations` are exact keys already; `objective` takes the default bound;
  strings and nulls compare as values) and `compareJsonUnder()` (a key-set
  difference is reported by path). No comparator change is expected.
- `tests/fusion/fusiontrace.h`: `traceJson()`, the trace golden's writer
  (`history` rows carry `outer`, so the pass numbering across stages is in
  the golden).
- `tests/fusion/fusionfixtures.cpp` and `tests/data/fusion/`: the fourteen
  golden fixtures (four fits: `coarse_linear`, `coarse_maneuver`,
  `stationary_spin`, `bridged_hole`; ten rejections) and the initializer
  recordings reached by `initializerFixture()` (`rest_throughout`,
  `scale_recording`, `motion_start`, `long_hole` among them).
- `tests/fusion_golden_capture.cpp`: the capture tool (`--revision`, exit 2
  on an outcome that disagrees with `expectSuccess`, exit 3 when two
  captures differ).
- `tests/tst_fusion_session.cpp`, `tests/tst_fusion_store.cpp`,
  `tests/tst_fusion_jobs.cpp`: quote the algorithm string as the result
  version (`registrationShape`, `restoredFitIsIndistinguishable`, the
  store's stamp literal, the jobs' stamps); each becomes `v9`.

### Audit and traceability

- `tests/audit/cleanup_audit.cmake` (read the header and the groups named):
  the header's rule-group mechanism (`audit_group`, `expect_none`,
  `expect_only`, `expect_count`, the pathspec forms, "these count LINES");
  group `fusion-model` ("three kinds of boundary": exactly three
  `checkpoint(` call sites in `src/fusion`, with the comment that says what
  to do when a kind is added; the comment "the goldens say
  batch-temperature-bias-v8"); group `scale-state` (one builder of the
  scaled factor, the documents describe the fitted scale, `scale_prior` and
  `model.scale` named once each in the fusion document); group
  `stored-results` ("one authority: the fusion algorithm string", count 1
  in `src` and only in `fusion.h`); group `gnss-holes` (the previous string
  `v7` gone, the goldens' fourteen `algorithm` lines with the current
  string, four fits, ten rejections, the slow tail's three comparisons, the
  fusion document's sentences and its two reference recordings): the
  pattern for the new group and the rules whose literal is the current
  string.
- `tests/acceptance_map.txt` (read the tail, items 1301-1313): the line
  forms (`# <item> - (<sections>) <statement>`, then one evidence line per
  test, audit group or manual step); the newest hundred is 1301-1313.
- `tests/README.md` (read the sections named): section 1's table rows for
  `tst_fusion_kernel` and `tst_fusion_golden` (what each proves, in prose,
  with the items in parentheses); section 9.14 and appendix N (the newest
  matrix and appendix, the pattern for 9.15 and appendix O); section 10's
  group descriptions (`scale-state`, `gnss-holes`, and the `fusion-model`
  sentence "three kinds of boundary"); section 11's capture history (one
  paragraph per capture: date, revision, what changed and why) and its
  re-capture procedure (the `PATH`, `--revision`, what a changed file
  means); section 12.2's preamble and M10 (the progress texts the runner
  prints), 12.9's M49 (the form the new manual step takes, with its one-line
  Python printout), 12.12's M55 (the latest step, with its per-pass
  iteration printout from the stderr texts).

### Documents

- `docs/SENSOR_FUSION.md` (read sections 4, 7 and 8): section 4's "The
  scale factors" paragraph (free from the first pass under the 1 % prior)
  and "Solver and stopping" (the passes, the cost test, the slow tail, the
  six rules); section 7's diagnostics key list (`algorithm`, `stopping`
  with "one of the six texts", `quality`, `model.scale`) and the paragraph
  on the failure shapes; section 7's string history
  ("`batch-temperature-bias-v8` since a hole ... `v7` ... `v6` ... `v5`");
  section 8's tables (M49's four reference recordings, M55's two with
  `13-35-10` as a solver failure and the prose that explains its
  `iteration limit` and the -0.058 factor, which this change is expected to
  turn into a success) and "What is and is not validated".
- `docs/DATA_SCHEMA.md` section 12 (the validity rule's sentence naming
  the string, and the example record earlier in the document).
- `docs/CALCULATIONS.md` section 17 ("Stored results": the string and its
  history).
- `docs/COMPUTED_PLOTS.md`: nothing the user sees changes; listed so that
  the documenter checks that nothing there names the string or the rules.

### The reference recordings (this machine)

- `TEMP/data/` holds the corpus (untracked); `TEMP/runs/` the outputs
  (never staged). The four reference recordings of M49 and the two of M55
  with their folders are spelled in `tests/README.md` sections 12.2, 12.9
  and 12.12; the runner is
  `build-agent/FlySightViewer-build/Release/fusion_runner.exe`.

## Decisions and constraints

1. **One phase, one commit.** See Phases. The Commit Policy names no tag;
   the orchestrator makes the one commit and no tag. `PLANS/staged-scale.md`
   was committed on its own (`3cd22f2`); the orchestrator never stages
   anything under `PLANS/`.

2. **The baseline is `fa00b4a`**, whose code is `1c6d5c6`'s, as the
   specification says.

3. **The stages live in the fit, under the temperature model.** The two
   stages are the full fit's. `fitFactorGraph()` under the temperature model
   runs the held stage and, on convergence, the released stage, and returns
   one `FitResult` whose values, objective, residuals, quality, graph,
   stopping and scale are the reported fit's; under the constant model it
   runs one stage as today, so the initializer's prefix and segment fits are
   untouched in behaviour and in trace. Rationale: one authority for the
   stage logic; every caller of the full fit (the pipeline, the kernel
   tests' `fitOfChannels()` and the tests that call the fit directly with
   `gyroBiasModelFor()`) gets the staged fit, which
   `imuRateIsWhatTheFitPublishes` requires; and `fitAndAssemble()` keeps one
   call. How the loop is factored (a helper per stage taking a start, a
   pass offset and the stage's prior) is the implementer's.

4. **"Held" is the prior's sigma, and nothing else.** The held stage's
   scale prior has the datasheet's sensitivity tolerance divided by a
   thousand as its sigma, the number `withScaleHeldAtOne()` already uses;
   the released stage has the tolerance itself. Whether the stage passes a
   divided tolerance into the prior's builder or the builder takes a stage
   factor is the implementer's; `Tuning::noise` as the diagnostics report it
   does not change, and the graph keeps its shape and factor order (the
   scale prior last). The released stage's first graph may be the held
   stage's last rebuild with its scale prior replaced, or a fresh build at
   the held values; both preintegrate at the same bias and scale.

5. **The released stage starts from the held stage's values**: every pose,
   velocity, the bias, the slope and the scale, as the held stage left them.
   Nothing is reset to the initializer's start.

6. **`diverged` is a return, not a throw.** After every pass's rebuild of a
   full-fit stage, judged before that pass's convergence test, the stage
   continues only when the IMU normalized RMS of the rebuilt graph at the
   pass's values is strictly below `Tuning::divergenceMaxImuNrms` (10) and
   every one of the six factors of the pass's values is strictly inside
   `Tuning::divergenceScaleRange` (0.5 to 2); otherwise the stage ends under
   `StopRule::kDiverged` ("diverged") with `converged` false, the rebuilt
   graph's objective, residuals and quality, and a `Stopping` whose `rule`
   is `diverged`. Strict comparisons, as the slow tail's: a bound of zero
   and an empty interval (lower bound at or above the upper) refuse
   deterministically. In the held stage this is a solver failure through the
   existing non-convergence path (reason "Batch fusion did not converge
   (diverged); sensor fusion unavailable", diagnostics `algorithm`,
   `failure`, `quality`, `stopping`); in the released stage it is the
   fallback. `FitFailure` stays what it is: the two failures a pass cannot
   continue from. The initializer's fits are not judged for divergence
   (decision 3).

7. **The thresholds of the divergence rule are reported** (Michael's
   ruling, reversing the plan's first reading): `Stopping` carries the two
   bounds as it carries the ceiling and the slow tail's, since its contract
   is that the account holds every threshold in force, and `stoppingObject()`
   writes them as `divergence_max_imu_nrms` and `divergence_scale_range`
   (an array of the lower and upper bound). The tests that assert the
   `stopping` key set or its values gain the two keys.

7a. **The released stage's budget is `Tuning::releasePasses`, two** (the
    specification as amended): the held stage keeps `maxPasses`; a release
    that has not settled in two passes falls back. A test may set it as it
    sets `maxPasses`.

8. **What the released stage catches.** The released stage runs under the
   same rules as any pass sequence, and its failures of every kind are the
   fallback: it ends `iteration limit`, `bias not settled` or `diverged` by
   return, or `cost increased` or `damping saturated` by a `FitFailure`
   thrown from an iteration, which the full fit catches as the release's
   failure. `FusionCancelled` is not caught: a cancellation at the release
   boundary or inside the released stage cancels the fit, like any other.
   The fallback keeps the held stage's `FitResult` whole (values, graph,
   objective, residuals, quality, stopping, scale), with `converged` true,
   and `history` holding both stages' iterations.

9. **The boundary "Releasing the scale factors"** is reported once through
   the checkpoint, after the held stage converged and before the released
   stage's first graph build, inside the scope that catches the release's
   failures: a `FitFailure` thrown from that boundary (the fallback test's
   forcing) is the release's failure. It is a fourth kind of boundary: the
   audit's "three kinds of boundary" count becomes four with its comment,
   and the `run()` comment of `fusion.h`, `docs/SENSOR_FUSION.md` section 7,
   `tests/README.md` section 10's `fusion-model` sentence and M10's text
   list, and `tst_fusion_golden::cancelAtEachKindOfBoundary` (a row at that
   text) follow.

10. **Pass numbering and counts.** `FitIteration::outer` continues across
    the stages, so the released stage's first pass has `outer` equal to the
    held stage's pass count and its progress texts say "Pass p" with p one
    more than the held stage's last; `Stopping::passes` of each stage counts
    that stage's own passes, 1..maxPasses, as its contract says, and the
    reported `stopping.passes` is the reported stage's; `history` of the
    returned `FitResult` holds both stages' iterations, kept or discarded,
    so `seeds[0].iterations` counts both; `scale_release.held.iterations`
    and `.released.iterations` are each stage's own count. The tests that
    relate the history's distinct `outer` values to `stopping.passes`, or
    the history's last row to the reported objective
    (`biasSettledByCostTest`), are restated in these terms.

11. **`scale_release` is a success key only**, top-level, written once in
    `fusionoutput.cpp` beside `stopping` and `quality`: `held` with `rule`,
    `passes`, `iterations`, `objective` (the held stage's rebuilt-graph
    objective); `released` with the same four when the released stage ran
    at least one iteration, else `null` (a failure thrown at the release
    boundary before any pass, as the forced fallback does); within
    `released`, `rule` is the rule the stage ended under, for a thrown
    failure the thrown account's rule, and `objective` is the cost of the
    stage's last rebuilt graph, `null` when the stage ended by a throw, which
    happens inside a pass before its rebuild; `kept` true when the released
    stage is the fit; `reason` the released stage's `rule` when it was
    discarded, else `null`. A failure's diagnostics (`failureDiagnostics()`)
    keep their shapes: `diverged` in the held stage takes the completed-pass
    shape, and no failure carries `scale_release`. `PipelineTrace` keeps its
    shape: the account is in the diagnostics and in the `FitResult`.

12. **The `FitResult` carries the account** so that tests through the seams
    read it without parsing JSON: the recovery test reads the held stage's
    objective from it in place of its held fit, and the fallback test reads
    `kept` and the reason from it and from the diagnostics. The documenter
    names the member and its struct; `fusionoutput.cpp` reads it and nothing
    recomputes it.

13. **No stage-specific tuning.** The specification lets the planner settle
    how a test forces divergence in the released stage alone. A second set
    of bounds for one stage would be generality the product never uses; the
    checkpoint cannot change a const tuning. The forcing is the data's: on
    `scale_recording`, whose released x accelerometer factor goes to about
    1.02 while the held stage's factors stay within a few 1e-5 of one,
    `divergenceScaleRange` set to an interval that contains the held factors
    and not the released one (0.5 to 1.005; the recovered factor is within
    1 % of 1.02, so it is at least 1.01) ends the released stage `diverged`
    at some pass and leaves the fallback, through the one path that both
    bounds share. `divergenceMaxImuNrms` forced to zero and
    `divergenceScaleRange` forced empty each end the held stage `diverged`
    as a solver failure. The IMU normalized RMS bound is therefore not
    forced in the released stage alone; the documentation and the test's
    comment say so. Michael may overrule this for a stage-specific bound.

14. **`withScaleHeldAtOne()`** tightens the prior the fit is given; after
    this change a fit under it runs both stages tightened (the held stage a
    millionfold, the released a thousandfold), which still holds the scale
    at one. Its remaining user, the constant-temperature consistency test,
    may keep it with its comment restated, or read the held stage's account
    instead and the helper goes: the implementer's call, under the rule that
    nothing unused remains and that the recovery test no longer runs a
    second fit for its comparison.

15. **The forced fallback throws a rule.** The fallback test's checkpoint
    throws `FitFailure` with a `Stopping` whose `rule` is set (the
    initializer tests throw an empty one, which no diagnostics read); the
    test asserts that rule as `scale_release.reason`.

16. **The algorithm string `batch-temperature-bias-v9`** is changed in
    `src/fusion/fusion.h` only; every test literal, every document sentence
    and every audit rule that spells `v8` as the current string moves to
    `v9`, and the new audit group adds the rule that `v8` is gone from
    `src`, `tests` and `docs`, with `tests/README.md` (its capture history
    and matrix rows) and `tests/acceptance_map.txt` (item 1306's statement)
    excluded as history. Item 1306 is not restated: the new hundred has its
    own item for the string and the dropped results. There is no
    compatibility-marker bump: the result version alone drops the stored
    fits.

17. **The manual step is run in the phase** and its numbers go into
    `docs/SENSOR_FUSION.md` section 8 as measured, as M55's did: the
    recordings are on this machine. `13-35-10`'s row and the prose around it
    (the `iteration limit`, the -0.058 factor) are rewritten to what the
    step measures. The audit's `gnss-holes` rules pin each of the two M55
    recordings to one line of the fusion document; section 4 now names
    `13-35-10` as the case, so that rule's count for `13-35-10` becomes two,
    amended in place with its comment and the README's description of the
    group, and `08-35-48` stays at one. A recording that misses the specification's expectation is
    reported with its numbers, not tuned.

18. **The acceptance items** open the hundred 1401; their appendix is O,
    their matrix 9.15, their manual step M56 in a new section 12.13; the
    audit group's slug is `staged-scale`. The documenter enumerates the
    items from the specification's sections 2 to 5.

### Constraints the code imposes

- The suite runs sequentially in `build-agent/`; the fusion tests time out
  under load. The re-capture runs `fusion_golden_capture` from
  `build-agent/FlySightViewer-build/Release` with the DLL directories on
  `PATH` and `--revision "$(git rev-parse HEAD)"`, as section 11's procedure
  says; the `_exact` tests then prove bit identity on this compiler. All
  fourteen `.json` goldens change (`algorithm`), the four fits' numbers,
  traces and progress texts change (the released stage's passes and the new
  boundary), and no `.channels.txt` is expected to stay byte-identical.
- `fusion_golden_capture` refuses to write when two captures differ (exit
  3) and flags a fixture whose outcome disagrees with `expectSuccess` (exit
  2); the four fits must still converge under both stages, which the
  specification's "nothing changes for a recording whose factors stay at
  one" predicts.
- The success diagnostics' sorted top-level key list is asserted literally
  by `tst_fusion_kernel::initializerDiagnosticsShape`, and the golden
  comparator reports a key-set difference by path: `scale_release` joins
  both.
- The audit counts `checkpoint(` call sites in `src/fusion` (lines, comments
  included): a comment that spells it is reworded.
- The audit's `gnss-holes` rules count the goldens' `algorithm` lines with
  the current string and name the fusion document's sentences and two
  reference recordings: those rules are amended to `v9`, not duplicated.
- `experiments/fusion_lab/` holds its own copy of the kernel; it is ignored
  by git, outside the audit's pathspec, and is not changed.
- `tests/README.md` section 10 is excluded from some rules because it
  spells their patterns; a new rule whose literal appears there excludes it
  the same way.

## Interfaces between phases

There is one phase. The names that a later specification, the audit and the
documents rely on are fixed here:

- `StopRule::kDiverged` is `"diverged"`, the seventh rule; the `SolverFailed`
  reason for it is "Batch fusion did not converge (diverged); sensor fusion
  unavailable".
- `Tuning::divergenceMaxImuNrms` is 10 and `Tuning::divergenceScaleRange`
  is the interval 0.5 to 2 (its representation is the implementer's; both
  bounds are settable by a test, and an empty interval refuses).
- The boundary text is `Releasing the scale factors`, reported once per full
  fit that releases, a fourth kind of boundary.
- The progress texts stay `Pass %1, iteration %2` with the pass numbered
  across both stages.
- The diagnostics key is `scale_release`, top-level, a success key only, with
  `held`, `released`, `kept` and `reason` as decision 11 states.
- The algorithm string is `batch-temperature-bias-v9`, in
  `src/fusion/fusion.h` only.
- The audit group slug is `staged-scale`; the acceptance items start at
  1401; the README's appendix is O, its matrix 9.15, its manual step M56 in
  section 12.13.
- `fitFactorGraph(samples, initial, tuning, passFormat, checkpoint, model)`
  keeps its signature and its callers; its contract comment states the two
  stages under the temperature model.

## Commit Policy

Michael has authorized commits for this plan on the existing working branch.
Nothing is ever pushed, and nothing is ever committed on `master` or
`fusion-improvements`. One commit, staged by explicit path, with the
session's attribution line; `PLANS/` is never staged.
