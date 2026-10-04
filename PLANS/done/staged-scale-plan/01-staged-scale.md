# Phase 1: Staged scale

The whole of `PLANS/staged-scale.md`, as reproduced in `00-overview.md`, in
one commit. The overview's decisions 1-18 apply as written; this document
applies them to the code as it is at `fa00b4a` (the code of `1c6d5c6`). The
specification was committed since the overview was written (`3cd22f2`,
`PLANS/staged-scale.md`); nothing under `PLANS/` is staged by the phase.

**Findings against the overview (none blocking; each settled below).**

1. `driftingBiasSegmentsConverge` asserts `seeds[0].iterations <= 30`, the sum
   over the full fit's passes. With two stages the sum may exceed it although
   the bias and slope are fitted in the held stage. Settled by D5: the bound
   moves to the held stage's count; no item states the number.
2. The `gnss-holes` audit rules pin `13-35-10` and `08-35-48` to one line
   each of `docs/SENSOR_FUSION.md`, and section 4 must now carry the
   `13-35-10` case. Settled by D6 (overview decision 17): section 4 names
   the recording, and the rule's count for `13-35-10` becomes two.
3. `withScaleHeldAtOne()` has two users today; the recovery test loses its
   (decision 12) and the constant-temperature test's fit with it is, under
   the staged fit, the held stage of a plain full fit. Settled by D2: the
   helper goes and the test reads the account.

## Purpose

The full fit finds its stable solution with the scale factors held, then
releases them from it as a refinement that is judged, not trusted: a released
stage that converges is the fit; one that does not falls back to the held
solution, reported as converged with its factors at one and an account of
why. A pass whose rebuilt graph has left the model ends a stage at once under
`diverged`. Every fit's numbers move, so the algorithm string becomes
`batch-temperature-bias-v9` and every stored result is dropped once. One
phase, one commit (overview, Phases).

## Dependencies

Depends on nothing and blocks nothing. It consumes the kernel as it is at
`1c6d5c6`: `fitFactorGraph()`'s pass loop, `addScalePrior()` reading the
sensitivity tolerances, `Stopping`, `FitFailure`, `FitResult`, the slow tail's
three bounds and `kCompletedPassFailureKeys`'s shape, `Checkpoint`,
`PipelineTrace`, `successDiagnostics()` / `failureDiagnostics()`,
`fitCovariance()` reading `fit.graph` and `fit.values`, and the golden suite
with its capture tool.

## What changes

### 1. The fit's two stages (spec §2, §3 first three bullets; decisions 3, 4, 5, 8, 9, 10)

`src/fusion/factorgraphfit.h/.cpp`.

- Under `model.temperatureLinear`, `fitFactorGraph()` runs the pass loop
  twice. The held stage from `initial` with the scale prior's sigmas divided
  by a thousand (each sensor's `sensitivityTolerance / 1000`, the number
  `withScaleHeldAtOne()` used); then, only when the held stage ended
  `settled` or `slow tail accepted`, the boundary `Releasing the scale
  factors` through `checkpoint`, and the released stage from the held stage's
  values (every pose and velocity, `B(0)`, `T(0)`, `S(0)` as the held stage
  left them) with the datasheet's sigmas. Under the constant model one stage
  as today: the initializer's prefix and segment fits, `fitOrFail()` and
  `initializer.cpp` are not touched. Whether the stage passes a divided
  tolerance into `addScalePrior()` or the builder takes a factor is the
  implementer's; `tuning.noise` is never changed (the diagnostics report it).
  The graph keeps its shape and factor order (the scale prior last).
- The held stage has `maxPasses` passes of `maxIterations`; the released
  stage has `releasePasses` (a new `Tuning` field, two) passes of
  `maxIterations` (overview decision 7a); `outer` continues across the
  stages (the released stage's first pass has `outer` equal to the
  held stage's pass count, so its texts say "Pass p" with p one more than the
  held stage's last). `history` of the returned result holds both stages'
  iterations. `Stopping::passes` of each stage counts its own passes, and the
  reported `stopping` is the reported stage's.
- The released stage's failures of every kind are the fallback: the stage
  ends `iteration limit`, `bias not settled` or `diverged` by return, or
  `cost increased` / `damping saturated` by a `FitFailure` thrown from a pass
  (or from the release boundary itself, which is inside the scope that
  catches: the fallback test's forcing). On the fallback the returned
  `FitResult` is the held stage's whole (values, graph, objective, residuals,
  quality, stopping, scale, slope), `converged` true, `history` both stages'.
  A `FitFailure` thrown in the held stage propagates as today (the pipeline's
  `catch (const FitFailure &)`), and `FusionCancelled` is never caught.
- The contract comment of `fitFactorGraph()` states the two stages, the
  trigger, the fallback, the divergence rule and the boundary; the comment
  on `maxPasses` in `fusionsamples.h` says "per stage of the full fit". How
  the loop is factored (a helper per stage taking a start, a pass offset and
  the stage's prior) is the implementer's; `fitFactorGraph(samples, initial,
  tuning, passFormat, checkpoint, model)` keeps its signature and callers.

### 2. The `diverged` rule (spec §2 fourth bullet, §3 fourth bullet; decisions 6, 7, 13)

- `StopRule::kDiverged` is `"diverged"`, the seventh text. `Tuning` gains
  `divergenceMaxImuNrms` (10) and `divergenceScaleRange` (the interval 0.5 to
  2; a lower and an upper bound a test can set, in whatever small form the
  implementer chooses under that name), each with a comment in the style of
  the slow tail's bounds: far outside anything a slow tail accepts (2) or a
  1 % tolerance admits, they catch a fit that has left the model.
- After every pass's rebuild of a full-fit stage, before that pass's cost
  test: the stage continues only when the rebuilt graph's IMU normalized RMS
  at the pass's values is strictly below `divergenceMaxImuNrms` and every one
  of the six factors of the pass's values is strictly inside the range;
  otherwise the stage ends at once under `diverged` with `converged` false,
  the rebuilt graph's objective, residuals and quality, and `stopping.rule`
  `diverged`. A zero bound and an empty interval refuse deterministically. In
  the held stage this is the existing non-convergence path of
  `fitAndAssemble()`: reason "Batch fusion did not converge (diverged);
  sensor fusion unavailable", keys `algorithm`, `failure`, `quality`,
  `stopping`. In the released stage it is the fallback.
- The two bounds are reported (overview decision 7): `Stopping` gains
  `divergenceMaxImuNrms` and the range's two bounds, copied from the tuning
  where the other thresholds are, and `stoppingObject()` writes
  `divergence_max_imu_nrms` and `divergence_scale_range` (an array
  `[lower, upper]`); `failureDiagnosticsShape` and every test that asserts
  the `stopping` keys or compares `stopping` objects gain them.

### 3. The account and the diagnostics (spec §3 fifth bullet; decisions 11, 12)

- `factorgraphfit.h` gains `struct StageAccount { std::string rule; int
  passes; int iterations; double objective; }` (`objective` NaN when the
  stage ended by a throw before its rebuild) and `struct ScaleRelease {
  StageAccount held; std::optional<StageAccount> released; bool kept; std::string
  reason; }` (`released` empty when the stage ran no iteration; `reason` the
  released stage's rule when discarded, empty otherwise), carried as
  `FitResult::scaleRelease`, filled by the fit under the temperature model
  and left default under the constant model. The exact names are D1's;
  nothing recomputes the account.
- `src/fusion/fusionoutput.cpp`: `successDiagnostics()` writes `scale_release`
  once, beside `stopping` and `quality`, from `fit.scaleRelease`: `held`
  `{rule, passes, iterations, objective}`; `released` the same four or
  `null`, its `objective` `null` for a throw; `kept`; `reason` a string or
  `null`. `failureDiagnostics()` is unchanged: no failure carries the key.
  `seedSummary()`'s `iterations` is `fit.history.size()`, both stages.
  `scaleObject()` is unchanged: on the fallback the covariance step reads the
  held stage's graph, whose prior is the tight one, which gives "the held
  stage's sigmas". The contract comment of `successDiagnostics()` in
  `fusionoutput.h` and the `Result::diagnosticsJson` comment in `fusion.h`
  name the key. `PipelineTrace` and `traceJson()` do not change.

### 4. The boundary and the string (spec §3 last two bullets; decisions 9, 16)

- `fusion.h`: `Algorithm[]` becomes `batch-temperature-bias-v9`, comment
  unchanged; the `run()` comment lists four kinds of boundary, the fourth
  "Releasing the scale factors, once per full fit whose held stage
  converged". `fusionprogress.h`'s catch list gains the release scope of
  `fitFactorGraph()`, which catches `FitFailure` from the released stage.
- Every quote of `batch-temperature-bias-v8` at `fa00b4a` (`git grep`), and
  what happens to it: `src/fusion/fusion.h` (the literal); the fourteen
  goldens (re-captured, section 6); `tst_fusion_kernel.cpp`'s three
  `QCOMPARE`s (`biasSettledByCostTest`, `failureDiagnosticsShape`,
  `constantTemperatureKeepsSlopeAtPrior`); `tst_fusion_session.cpp`
  (`registrationShape`, `restoredFitIsIndistinguishable`);
  `tst_fusion_store.cpp` (`kSolverFailureDiagnostics`); `tst_fusion_jobs.cpp`
  (the two stamps); `cleanup_audit.cmake` (the `fusion-model` comment, the two
  `stored-results` rules, the five `gnss-holes` rules that count the current
  string: amended in place, not duplicated); `tests/README.md` (section 10's
  `fusion-model` bullet, the re-capture procedure's sentence, with `v8` as
  history; the matrix rows 1043 and 1306 and the capture paragraphs stay as
  history); `docs/SENSOR_FUSION.md` (section 7's key list and history),
  `docs/DATA_SCHEMA.md` (the `"records"` example and section 12),
  `docs/CALCULATIONS.md` section 17, each with the history sentence of
  spec §5. `tests/acceptance_map.txt` item 1306 is not restated.

### 5. What does not change

- `experiments/fusion_lab/` (ignored; its own kernel). `scaledimufactor.*`,
  `imuintegration.*`, `fitcovariance.*`, `trajectoryreconstruction.*`,
  `initializer.*`, `inputadapter.*`, `fusionsamples.cpp`.
- `docs/COMPUTED_PLOTS.md`: checked; it names neither the string nor a rule.
- The `_time` axis of every success fixture.

### 6. Goldens and the capture (spec §4 sixth bullet; constraints)

After the kernel change, from `build-agent/FlySightViewer-build/Release` with
the DLL directories on `PATH` and `--revision "$(git rev-parse HEAD)"`
(section 11's procedure): fourteen lines, four `succeeded`, ten `rejected`, no
`** UNEXPECTED **`, "wrote 19 files", exit 0. Expected changes: all fourteen
`.json` (`algorithm`); in the four fits `scale_release` (new, `kept` true),
`trace.history` (the released stage's rows with `outer` continuing),
`progress` (the new boundary and the released passes), `rows` and every
number of the fit; the four `.channels.txt` (every column but `_time`);
`capture.json`. The ten rejections change in `algorithm` alone. A fit whose
released stage is discarded (`kept` false in a golden) or that no longer
converges is an escalation with the phase uncommitted, not a tuning. Then
`cmake build-agent/FlySightViewer-build` (the exact-test gate), `-L fusion`
and `-L exact`.

### 7. Order of work

1. Sections 1-4 of the kernel; the string.
2. The kernel tests that read no golden (the account test, the recovery, the
   rest, the fallback, the two divergence tests, the amended literals) green.
3. The capture; the configure; `-L fusion`, `-L exact`.
4. The audit group, the map, the README, the documents; `audit_cleanup` and
   the whole suite, sequentially, in `build-agent/`.
5. M56 on this machine; its numbers into `docs/SENSOR_FUSION.md` section 8
   and the report.

## Interfaces

Provided, as the overview fixes them, plus D1's names:

- `StopRule::kDiverged` `"diverged"`; the `SolverFailed` reason "Batch fusion
  did not converge (diverged); sensor fusion unavailable".
- `Tuning::divergenceMaxImuNrms` 10; `Tuning::divergenceScaleRange` 0.5 to 2,
  both settable by a test; an empty interval refuses; both reported in
  `stopping` as `divergence_max_imu_nrms` and `divergence_scale_range`.
- `Tuning::releasePasses` 2, the released stage's pass budget, settable by
  a test.
- The boundary text `Releasing the scale factors`, a fourth kind.
- `Pass %1, iteration %2` with the pass numbered across both stages.
- `FitResult::scaleRelease` of type `ScaleRelease` (`held`, `released`, `kept`,
  `reason`), `StageAccount` (`rule`, `passes`, `iterations`, `objective`).
- The diagnostics key `scale_release`, success only, top level; the success
  key list of `initializerDiagnosticsShape` gains it between `residuals` and
  `seed_comparison_performed`.
- `Fusion::Algorithm` `batch-temperature-bias-v9`, in `fusion.h` only.
- Audit group `staged-scale`; items 1401-1415; appendix O; matrix 9.15; M56
  in section 12.13.

Consumed: nothing beyond the kernel at `1c6d5c6`.

## Acceptance criteria

Each is traced to the specification (§) and its item (D3).

1. (§2 first and second principles, §3 first bullet; 1401) Under the
   temperature model `fitFactorGraph()` runs the held stage from `initial`
   with the scale prior's sigma `sensitivityTolerance / 1000` per sensor,
   then the released stage from the held stage's values with the tolerance
   itself; the graph has the same factors in the same order in both; the
   held stage has `maxPasses` passes and the released stage `releasePasses`
   (two) passes of `maxIterations`; `tuning.noise` as
   reported is unchanged. Under the constant model one stage: the
   initializer's fits' traces and accounts are bit for bit what they were
   (the four fits' `initializer` objects and prefix iteration counts in the
   re-captured goldens equal the previous capture's).
2. (§2 first principle, §3 second bullet; 1402) The released stage runs only
   after a held stage that ended `settled` or `slow tail accepted`; a held
   stage ending `iteration limit`, `bias not settled` or `diverged` is the
   solver failure of today's path with `kCompletedPassFailureKeys`, and a
   held stage's `FitFailure` propagates with `{algorithm, failure,
   stopping}`; no failure's diagnostics contain `scale_release`.
3. (§2 third principle, §3 third bullet; 1403) A released stage ending
   `settled` or `slow tail accepted` is the fit (`kept` true, `reason` null).
   One ending under any other rule, or by a `FitFailure` from a pass or from
   the release boundary, leaves `converged` true, `stopping` the held
   stage's, `objective` equal to `scale_release.held.objective` bit for bit,
   `model.scale` within a tenth of the tolerance of one (the held stage's
   largest departure on `13-35-10` was 0.04 %) with `acc_sigma` /
   `gyro_sigma` each below a hundredth of the tolerance, `kept`
   false, `reason` the rule, and the published channels those of the held
   fit. Cancellation at the boundary or inside the released stage is
   `Outcome::Cancelled`.
4. (§2 fourth principle, §3 fourth bullet; 1404) After every pass's rebuild
   of either full-fit stage, judged before the cost test with strict
   comparisons: `divergenceMaxImuNrms` 0 ends the held stage's first pass
   `diverged` as a solver failure with the completed-pass shape and the
   reason of Interfaces; an empty `divergenceScaleRange` does the same; on
   `scale_recording` a range of 0.5 to 1.005 passes the held stage and ends
   the released stage `diverged` at some pass: the fallback of criterion 3
   with `released.rule` `diverged` and a numeric `released.objective`. The
   `stopping` object reports `divergence_max_imu_nrms` 10 and
   `divergence_scale_range` [0.5, 2] on every fit and failure that carries
   it. The production bounds change nothing on any fixture: the four
   goldens' `kept` are true.
5. (§3 fifth bullet; 1405) `outer` continues across the stages in `history`
   (the released stage's first row has `outer` equal to `held.passes`), the
   progress texts' pass numbers continue, `Releasing the scale factors` is
   reported exactly once per full fit that releases, after the held stage's
   last iteration text and before the released stage's first `Integrating
   IMU factors`; cancelling there cancels the fit; `stopping.passes` is the
   reported stage's own count.
6. (§3 sixth bullet; 1406) `scale_release` has exactly the shape of
   decision 11 (`held`, `kept`, `reason`, `released`; each account
   `iterations`, `objective`, `passes`, `rule`), `held.iterations +
   released.iterations == seeds[0].iterations == history.size()`, the
   distinct `outer` values number `held.passes + released.passes`, and
   `FitResult::scaleRelease` agrees with it field by field.
7. (§3 seventh bullet; 1407) `Fusion::Algorithm` is `batch-temperature-bias-v9`;
   the fit's `resultVersion`, every diagnostics object and the fourteen
   goldens say so; a record stamped with an earlier string is dropped at
   load; `batch-temperature-bias-v8` occurs nowhere in `src`, `tests` or
   `docs` except `tests/README.md` and `tests/acceptance_map.txt` as history.
8. (§4 first bullet; 1408) `scale_recording` through one full fit:
   `s_ax` within the tolerance of 1.02 and the other factors as today, `kept`
   true, `released.rule` `settled`, and `fit.objective` strictly below
   `fit.scaleRelease.held.objective`, the ratio logged.
9. (§4 second bullet; 1409) `rest_throughout`: every factor within its prior
   as today, `kept` true, `released.rule` `settled`.
10. (§4 third bullet; 1410) The forced fallback (a checkpoint throwing
    `FitFailure` with a `Stopping` whose `rule` is set, at the release
    boundary) gives criterion 3 with `released` null and `reason` the thrown
    rule, through the pipeline and through the seam, the channels bit for
    bit.
11. (§4 fourth bullet; 1411) Criterion 4's three forcings as tests;
    `divergenceMaxImuNrms` is not forced in the released stage alone
    (decision 13), which the test's comment and the documents say.
12. (§4 fifth bullet; 1412) M56 has been run on this machine on the four
    recordings of M49 and on `13-35-10`; the numbers are in
    `docs/SENSOR_FUSION.md` section 8 and the report as measured; a miss is
    reported, not tuned.
13. (§4 sixth bullet; 1413) The goldens are re-captured as section 6 says;
    section 11's capture paragraph records what moved and why.
14. (§4 seventh bullet; 1414) Items 1401-1415 each have a test or audit line;
    `audit_cleanup` and the whole suite green, `_exact` included.
15. (§5; 1415) The documents say what "Documentation" lists; the audit pins
    the sentences it can.

## Tests

Run in `build-agent/` only, sequentially, Release.

**`tst_fusion_kernel`** (names cited by the map).

- `scaleReleaseIsAccountedFor` (new; criteria 1, 5, 6): `runPipeline()` on
  `coarse_maneuver` with a `PipelineTrace` and a progress-collecting
  `Checkpoint`; asserts the key shapes, the sums and the first released
  `outer`, the boundary's single occurrence and place among the texts, the
  pass numbers of the texts after it, and the agreement of the diagnostics
  with `fixtureFit("coarse_maneuver").fit.scaleRelease`.
- `releaseFailureFallsBackToTheHeldFit` (new; criteria 3, 10): on
  `scale_recording` (whose release changes the factors), the forcing of
  `allPrefixFitsFailFallsBack` with the text `Releasing the scale factors`
  and `FitFailure("Nonfinite or increasing optimizer cost", s)` where
  `s.rule = StopRule::kCostIncreased` (decision 15); the pipeline's result
  against the seam's (`fitFactorGraph()` with the same checkpoint on
  `windowOf()` / `initialize()`, then `fitCovariance()`,
  `reconstructAtImuRate()`, `wideningFactors()`, `fillOutputChannels()`, the
  pattern of `imuRateIsWhatTheFitPublishes`), `sameBitsEverywhere` on the
  twenty-one channels.
- `divergenceEndsTheHeldStage` (new, `_data`; criteria 4, 11): rows
  "imu nrms bound zero" (`divergenceMaxImuNrms = 0`) and "scale range empty"
  (lower at or above upper) on `coarse_maneuver`: `SolverFailed`, the reason,
  `kCompletedPassFailureKeys`, `stopping.rule` `diverged`, `passes` 1,
  `history.size()` the first pass's count, `trace.converged` false,
  `allChannelsEmpty`; `QCOMPARE(StopRule::kDiverged, "diverged")`.
- `divergenceEndsTheReleasedStage` (new; criteria 4, 11): `scale_recording`
  with `divergenceScaleRange` 0.5 to 1.005: `Succeeded`, the fallback's
  assertions, `released.rule` `diverged`, `reason` `diverged`,
  `released.passes >= 1`, `released.objective` finite; the comment says why
  the IMU bound is not forced here.
- `scaleRecordingRecoversTheFactor` (amended; criterion 8): one
  `fitFactorGraph()` call; the `held` fit, its loop and `withScaleHeldAtOne`
  go; `QVERIFY(with.objective < with.scaleRelease.held.objective)` and the
  release assertions; the log line keeps the per-stage numbers from the
  account.
- `restLeavesTheScaleAtItsPrior` (amended; criterion 9): the release
  assertions on `restThroughoutRun().diagnostics`.
- `constantTemperatureKeepsSlopeAtPrior` (amended; D2): the `temperature`
  fit goes; the stock fit's objective is compared, within 1e-6 relative, with
  `diagnostics["scale_release"]["held"]["objective"]` of the pipeline run
  already made; the departure assertion goes with the fit (the held stage's
  factors are not reported), its reasoning kept in the comment; v9.
- `initializerDiagnosticsShape` (amended): the key list gains
  `"scale_release"`.
- `biasSettledByCostTest` (amended; criterion 6): `trace.stopping.passes <=
  2` stays as the released stage's; `outers.size()` compared with
  `held.passes + released.passes`; `held.passes <= 2` added; `kept` true;
  the objective / last-row and `seed.iterations` assertions stand as they
  are (both now span both stages); v9.
- `slowTailAtTheIterationLimit` (amended): the "accepted" row: `history`
  175, `seed.iterations` 175, `held` `{slow tail accepted, 5, 125}`,
  `released.passes` 2, `released.iterations` 50, `kept` true; the three
  refused rows unchanged (125, `passes` 5, `kCompletedPassFailureKeys`).
- `failureDiagnosticsShape`: v9. `nonConvergenceIsSolverFailure`,
  `biasNeverSettlesIsSolverFailure`, `dampingSaturationIsASolverFailure`,
  `dampingCeilingChangesNothingBelowIt`, `allPrefixFitsFailFallsBack`,
  `imuRateIsWhatTheFitPublishes`, `diagnosticsReportTheScale`: unchanged in
  text (the first three prove criterion 2; the fourth compares `scale_release`
  under both ceilings; the last two run both stages on both sides).
- `driftingBiasSegmentsConverge` (amended; D5): `<= 30` on
  `scale_release.held.iterations`; the total logged.
- `fitTraceMatchesGolden`: unchanged in text; the re-captured goldens.

**`tst_fusion_golden`.** `cancelAtEachKindOfBoundary_data` gains the row
`rowAt("release boundary", "Releasing the scale factors")` and a row at the
released stage's first iteration text (the golden's first `Pass p, iteration
1` after the boundary, found by index); `successFixturesMatchGolden`,
`progressMatchesGoldenBoundaries` and `rejectionFixturesMatchGolden`
unchanged in text.

**`tst_fusion_session`** (`registrationShape`, `restoredFitIsIndistinguishable`),
**`tst_fusion_store`** (`kSolverFailureDiagnostics`), **`tst_fusion_jobs`**
(the two stamps): v9. **`tst_fusion_runner`**: unchanged.

**Audit: group `staged-scale`** in `cleanup_audit.cmake`, in `gnss-holes`'s
shape with `Allow:` comments, each rule planted once; a bullet in the file's
head comment; the traceability block's comment, range test, message and a
`foreach(item RANGE 1401 1415)`. The rules:

- `batch-temperature-bias-v8` absent from `src tests docs README.md
  ":!tests/README.md" ":!tests/acceptance_map.txt"`;
- `withScaleHeldAtOne` absent from `src tests docs ":!tests/README.md"`;
- `"scale_release"` on exactly 4 lines of `tests/data/fusion` and on exactly
  one line of `src`, in `src/fusion/fusionoutput.cpp`;
- `constexpr char kDiverged\[\] = "diverged"` once in `src`;
- `"Releasing the scale factors"` once in `src`; `checkpoint\(` on four lines
  of `src/fusion` (the `fusion-model` count amended to four with its
  comment, not a new rule);
- `divergenceMaxImuNrms|divergenceScaleRange` present in `src` only in
  `fusionsamples.h`, `factorgraphfit.h`, `factorgraphfit.cpp` and
  `fusionoutput.cpp` (the writer of `stopping`); `releasePasses` in
  `fusionsamples.h` and `factorgraphfit.cpp` only;
- `docs/SENSOR_FUSION.md`: `scale_release` once, `Releasing the scale
  factors` once, `diverged` at least once (`expect_count` on a phrase of the
  implementer's choice in the stopping-rule paragraph, e.g. "has left the
  model"), no `six texts|six rules`; `M56` once (the attribution of the
  measurement in section 8, the rule behind item 1412's audit line); the
  `gnss-holes` count of `08-35-48` (one line) stays satisfied and the count
  of `13-35-10` is amended to two (section 4's case and section 8's row),
  with its comment and the README's description of the group.

Amended in place: the `fusion-model` boundary count and comment, the
`stored-results` pair and the `gnss-holes` five to v9.

**`tests/acceptance_map.txt`.** Header: fifteen specifications, the
`1401-1415` entry in `1301-1313`'s form, "9.1 to 9.15", the coverage
sentences; item 1054 restated "(as amended)": the misfit falls against the
held stage's objective, read from the fit's account. A last section "The
scale factors as a refinement (PLANS/staged-scale.md): fifteen items":

```text
# 1401 - (2, 3) the full fit runs in two stages under the temperature model: the held stage from the initializer's start with the scale prior's sigma divided by a thousand, then the released stage from the held stage's values with the datasheet's sigma; one graph, the prior's sigma the only difference; each stage with the tuning's pass and iteration budgets; the initializer's fits untouched
1401 tst_fusion_kernel scaleReleaseIsAccountedFor
1401 tst_fusion_kernel fitTraceMatchesGolden
1401 tst_fusion_golden successFixturesMatchGolden
1401 audit staged-scale
# 1402 - (2, 3) the release trigger is convergence: the released stage runs only after a held stage that ended settled or slow tail accepted; any other end of the held stage ends the fit as today, under the same rule, reason and diagnostics shape, and no failure carries scale_release
1402 tst_fusion_kernel nonConvergenceIsSolverFailure
1402 tst_fusion_kernel biasNeverSettlesIsSolverFailure
1402 tst_fusion_kernel dampingSaturationIsASolverFailure
1402 tst_fusion_kernel divergenceEndsTheHeldStage
# 1403 - (2, 3) the fallback: a released stage that ends settled or slow tail accepted is the fit; one that ends under any other rule, diverges or throws is discarded: the held stage's values, graph, objective, residuals, quality and stopping are the fit, converged is true, the factors are at one with the held stage's sigmas, and the diagnostics record the outcome; a refinement never turns a converged fit into a failure; cancellation is not caught
1403 tst_fusion_kernel releaseFailureFallsBackToTheHeldFit
1403 tst_fusion_kernel divergenceEndsTheReleasedStage
# 1404 - (2, 3) divergence between passes: after every pass's rebuild of either stage, before the cost test, the IMU normalized RMS strictly below divergenceMaxImuNrms (10) and every factor strictly inside divergenceScaleRange (0.5 to 2), else the stage ends diverged: a solver failure with the completed-pass shape in the held stage, the fallback in the released; a zero bound or an empty interval refuses; the bounds are reported in stopping as divergence_max_imu_nrms and divergence_scale_range; the production bounds change nothing on any fixture
1404 tst_fusion_kernel divergenceEndsTheHeldStage
1404 tst_fusion_kernel divergenceEndsTheReleasedStage
1404 tst_fusion_golden successFixturesMatchGolden
1404 audit staged-scale
# 1405 - (3) passes are numbered across both stages in the trace and the progress texts; the boundary "Releasing the scale factors" is reported once before the released stage's first graph build, a fourth kind of cancellation boundary; each stage's stopping.passes counts its own passes
1405 tst_fusion_kernel scaleReleaseIsAccountedFor
1405 tst_fusion_golden cancelAtEachKindOfBoundary
1405 tst_fusion_golden progressMatchesGoldenBoundaries
1405 audit staged-scale
1405 audit fusion-model
# 1406 - (3) the diagnostics: scale_release with held, released (or null), kept and reason, a success key written once; stopping and quality describe the reported fit; model.scale the reported fit's factors; seeds[0].iterations counts both stages; the fit's result carries the account and nothing recomputes it
1406 tst_fusion_kernel scaleReleaseIsAccountedFor
1406 tst_fusion_kernel initializerDiagnosticsShape
1406 tst_fusion_kernel biasSettledByCostTest
1406 tst_fusion_kernel slowTailAtTheIterationLimit
1406 tst_fusion_golden successFixturesMatchGolden
1406 audit staged-scale
# 1407 - (2, 3) the algorithm string is batch-temperature-bias-v9, and every stored result is dropped once; v8 remains only as history
1407 tst_fusion_session registrationShape
1407 tst_fusion_session restoredFitIsIndistinguishable
1407 tst_fusion_store codeStampChangeDropsRecordOnLoad
1407 tst_fusion_store restoredSolverFailureShowsBadge
1407 tst_fusion_jobs columnOnFusionOutputIsCachedFromRecord
1407 tst_fusion_jobs workerRefillsColumnFromStoredFit
1407 tst_fusion_kernel biasSettledByCostTest
1407 tst_fusion_kernel failureDiagnosticsShape
1407 audit stored-results
1407 audit gnss-holes
1407 audit staged-scale
# 1408 - (4) the recovery holds: scale_recording recovers its 2 % factor through the released stage, which is kept, and its objective falls against the held stage's, read from the fit's account
1408 tst_fusion_kernel scaleRecordingRecoversTheFactor
# 1409 - (4) the resting recording leaves every factor at its prior through both stages, and its released stage settles
1409 tst_fusion_kernel restLeavesTheScaleAtItsPrior
# 1410 - (4) the fallback forced: a checkpoint that throws at the release boundary leaves a converged fit with the held stage's values and factors at one, kept false with the thrown rule as the reason, released null, and the published channels those of the held fit
1410 tst_fusion_kernel releaseFailureFallsBackToTheHeldFit
# 1411 - (4) divergence forced: the IMU bound at zero and an empty scale range each end the held stage diverged as a solver failure; the scale range narrowed to exclude the recovered factor ends scale_recording's released stage diverged, the fallback; the IMU bound is not forced in the released stage alone
1411 tst_fusion_kernel divergenceEndsTheHeldStage
1411 tst_fusion_kernel divergenceEndsTheReleasedStage
# 1412 - (4) the reference recordings: M56 runs the four of M49 and 13-35-10, reading scale_release from each diagnostics; 13-35-10 converges, the four at or near their M49 numbers; the numbers go in the documentation as measured
1412 audit staged-scale
1412 manual M56
# 1413 - (4) the goldens captured again under v9: four fits, ten rejections; the capture note records what moved and why
1413 tst_fusion_golden fixturesAreDeterministic
1413 tst_fusion_golden successFixturesMatchGolden
1413 tst_fusion_kernel fitTraceMatchesGolden
1413 audit gnss-holes
# 1414 - (4) a new hundred in the acceptance map; audit_cleanup and the whole suite green, the exact tests included
1414 tst_fusion_golden successFixturesMatchGolden
1414 audit staged-scale
# 1415 - (5) the documents: SENSOR_FUSION.md section 4 (the two stages, the trigger, the fallback, the divergence rule and its bounds, the case), the stopping paragraph with diverged, section 7 (scale_release, the fourth boundary, v9 in the history), section 8 (M56's numbers); DATA_SCHEMA.md section 12 and CALCULATIONS.md section 17 (v9); tests/README.md (the kernel and golden rows, M56 in 12.13, appendix O, matrix 9.15)
1415 audit staged-scale
```

1412's audit line stands on the group's `M56` rule; the map requires a test
or audit line beside a manual one.

**`tests/README.md`.**

- Section 1: `tst_fusion_kernel`'s row gains the five new or renamed
  proofs with their function names and rewords "the position misfit falling
  against the fit with the scale held at one" to the held stage's objective
  from the account; the temperature-case sentence names the held stage in
  place of the thousandfold prior; `tst_fusion_golden`'s row adds the release
  boundary to its cancellation list. Counts unchanged.
- 9.15, the matrix of 1401-1415 in 9.14's form; 9.11's row 1054 restated;
  section 10: a `staged-scale` bullet in prose, the `fusion-model` bullet's
  "three kinds of boundary" and string, the traceability bullet's ranges.
- Section 11: the `scale_recording` and `long_hole` rows' pass and
  iteration counts and the recovery sentence to the measured numbers; a
  capture paragraph "The capture of 2026-10-0n (the scale factors as a
  refinement)" stating what moved (section 6) and that the `_time` columns
  did not; the re-capture procedure's string sentence (v9, v8 history).
- Section 12: "Thirteen scripts"; 12.2's M10 text list gains `Releasing the
  scale factors`; a new 12.13 "The scale factors as a refinement" with
  **M56 The staged scale on the reference recordings (1412)**: M49's commands
  on the four recordings, outputs `m56-<name>.*`, and M55's command on
  `13-35-10` (its CSV and hole printout as there), each with the exit code;
  one Python line per recording printing `failure`, `stopping.rule`,
  `stopping.passes`, `seeds[0].iterations`, `objective`, `quality`,
  `model.scale` and `scale_release`; the per-pass iterations from the stderr
  texts as M55 prints them, which now show the boundary between the stages.
  Expected: `13-35-10` converges; the four at or near M49's numbers, the
  released stage adding a few iterations; the numbers go in the report and
  in section 8 as they are.
- Appendix O lists the fifteen items in appendix N's form; appendix K's
  item 54 restated.

**Documentation.**

- `docs/SENSOR_FUSION.md` section 4: "The scale factors" paragraph says the
  factors are held through the first stage and released from its solution;
  a new paragraph after "Solver and stopping" describes the two stages, the
  trigger, the fallback, the divergence rule with its two bounds and the
  case (the attitude handed to a segment, the x factor collapsing to -0.058
  in the first pass, 1707; held, two passes and 33 iterations, 0.45; the
  recording named, D6); the
  stopping paragraph gains `diverged` as the seventh rule and says "seven";
  section 5's limitations sentence on the 1 % prior stays. Section 7: the
  cancellation paragraph's fourth kind; the key list's `stopping.rule` "one
  of the seven texts", `scale_release` in "the fit" group with its four
  keys, and v9 in the history ("since the scale factors are released from
  the held solution"). Section 8: `tst_fusion_kernel`'s row; under
  "Validating the model" a paragraph and table of M56's four recordings
  (outcome, held and released passes / iterations / objective, kept, largest
  departure); the hole table's second row and the prose around it rewritten
  to what M56 measures (the table's preamble dating each row's measurement
  and string); "What is and is not validated" gains the release judged on
  the recordings. The two recording names once each.
- `docs/DATA_SCHEMA.md`: the `"records"` example and section 12 say v9.
- `docs/CALCULATIONS.md` section 17: v9, with v8 in the history.
- `docs/COMPUTED_PLOTS.md`: nothing.

## Decisions

- **D1. The account's names.** `StageAccount {rule, passes, iterations,
  objective}` and `ScaleRelease {held, released (optional), kept, reason}`
  in `factorgraphfit.h`, carried as `FitResult::scaleRelease`; `reason` empty
  stands for null. One struct per stage keeps the four fields in one place
  for both the writer and the tests.
- **D2. `withScaleHeldAtOne()` goes.** Under the staged fit the held stage
  of a plain full fit is the fit the helper used to make (the same graph,
  prior and start), so the constant-temperature test reads
  `scale_release.held.objective` from the pipeline run it already makes, and
  the recovery test reads the account. Nothing unused remains, and two fits
  of the executable's longest tests go.
- **D3. Item numbering**, fifteen items: 1401 the stages, 1402 the trigger,
  1403 the fallback, 1404 divergence, 1405 numbering and the boundary, 1406
  the diagnostics, 1407 the string (§2 and §3); 1408-1414 the seven bullets
  of §4 in order; 1415 §5. Item 1054 is restated as amended, since its test's
  comparison changes; 246, 244 and 1306 are not.
- **D4. The recovery's comparison** is strict: the reported objective below
  the held stage's, the ratio logged and recorded in the README's row. The
  specification's "position misfit falls" cannot be read from the account
  without a second fit, which decision 12 forbids.
- **D5. The 30-iteration bound** of `driftingBiasSegmentsConverge` applies to
  the held stage, where the bias and slope are fitted; the total is logged.
  No acceptance item states the number.
- **D6. The `13-35-10` case in section 4** names the recording, as the
  specification asks; the `gnss-holes` rule that pins `13-35-10` to one line
  of the fusion document is amended to two, in place, with its comment and
  the README's description of the group (overview decision 17). M56's
  numbers for it go into section 8's hole-table row and its prose, and the
  four M49 recordings into their own table, whose names no rule pins.
- **D7. The forced fallback throws `cost increased`** with the failure text
  the kernel uses for it, so that the account's `reason` is a rule the
  released stage can end under in production.

Ready.
