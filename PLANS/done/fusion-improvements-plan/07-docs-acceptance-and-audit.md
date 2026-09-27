# Phase 7: Documentation, acceptance map and audit

## Overview

This is the closing phase; it changes no kernel code. It (1) rewrites `docs/SENSOR_FUSION.md` so that it states the model as it now is (segmented initializer, stopping rule with the slow-tail acceptance, per-step noise term and its meaning at other output rates, temperature-dependent gyro bias, the final diagnostics key list and failure shapes), and brings `docs/CALCULATIONS.md`, `README.md` and `tests/README.md` into agreement with it (spec section 11); (2) writes the acceptance map of this plan, items 201-247, one per requirement of spec sections 3-11 and per test of section 10, each resolved to a test function, an audit group or a manual step, with the human-readable matrix in `tests/README.md` section 9.3 and the items in a new appendix C; (3) extends the cleanup audit with the patterns of this plan (the retired detector, the silent poll, the old initial-attitude type, the old algorithm strings, the branch name outside its one historical note, the runner's isolation, the tools' non-installation) and makes it accept the new item range; (4) runs the closing audit: the goldens on disk are the last capture, the detector is gone, GTSAM stays confined, the runner touches no logbook or settings, the public contract of `fusion.h` is unchanged except for the temperature field, `Outcome` semantics and the cancellation boundaries hold, and nothing under `src/` outside `src/fusion/` (the registration included) and the build list changed; and (5) runs the whole suite and the manual verification of the runner on the four reference recordings of spec section 12.

Every earlier phase made "minimal edits so the documentation does not lie" and deferred the prose to this phase. The deferrals collected from the phase documents are: Phase 1 (`docs/SENSOR_FUSION.md` section 8's rewrite; `tests/README.md` option table and section 4 notes for the tools), Phase 2 (the runner in `tests/README.md` and `docs/`), Phase 3 (section 8's rewrite; the acceptance map), Phase 4 ("what the term means and how it behaves at other output rates"), Phase 5 (the acceptance map; section 5's rewrite beyond the one-paragraph replacement), Phase 6 ("the explanation and the acceptance map entry for spec section 10's temperature line"). Each lands in a task below.

## Dependencies

- **Depends on:** Phases 1-6, all committed on `fusion-improvements` and tagged `plan/fusion-improvements/phase-N-done`.
- **Blocks:** None (final phase).
- **Assumptions (names used verbatim from the earlier documents; the implementer confirms each by `grep` before relying on it):**
  - Test executables: the 41 of `master` with `tst_fusion_parity` renamed to `tst_fusion_golden` (Phase 1), plus `tst_fusion_runner` (Phase 2): 42, plus `audit_cleanup`. Non-test executables in the fusion block: `solver_deploy_probe`, `fusion_golden_capture` (Phase 1), `fusion_runner` (Phase 2). Exact-mode twins (label `exact`, Release, MSVC 19.44 only): `tst_fusion_golden_exact`, `tst_fusion_kernel_exact`, `tst_fusion_session_exact`, `tst_fusion_jobs_exact`, `tst_fusion_rows_exact`, `tst_fusion_runner_exact`: six.
  - Test functions this phase cites (all must exist; Task 7.4's audit run proves it): `tst_fusion_golden::{comparatorHoldsItsBounds, fixturesAreDeterministic, successFixturesMatchGolden, rejectionFixturesMatchGolden, progressMatchesGoldenBoundaries, channelsWriterIsTheInverseOfTheLoader, cancelAtEachKindOfBoundary, cancelDuringPreparation, cancelNeverRequestedChangesNothing, twoRunsAreBitIdentical, workerThreadMatchesMainThread, resultIsIndependentOfCallerState}`; `tst_fusion_kernel::{suspectSegmentIsRestartedFromItsNeighbour, restartDecisionUsesTwiceTheMedian, startsOnTheLimitAreStillUsed, preintegrationHonoursExactBoundaries, validationRejectsEachDefect, headingIsUnconstrained, exactConstantVelocityFit, fitTraceMatchesGolden, nonConvergenceIsSolverFailure, biasSettledByCostTest, slowTailAtTheIterationLimit, biasNeverSettlesIsSolverFailure, failureDiagnosticsShape, perStepTermIsZeroWithoutSignalChange, perStepTermMatchesSpecifiedCovariance, perStepTermScalesWithStep, diagnosticsReportPerStepConstants, shortWindowIsOneSegment, segmentsAreCutOnFixes, yawSigmaIsMarginalAboutTheVertical, initializerProgressTexts, initializerDiagnosticsShape, initializerFixturesAreDeterministic, startsInMotionGrowsToTheManoeuvre, atRestPrefixStopsGrowing, smallestSaccFixIsTheAnchor, driftingBiasSegmentsConverge, allPrefixFitsFailFallsBack, temperatureFactorJacobians, temperatureGraphShape, reconstructionUsesIntervalBias}` plus the two placeholders of the next bullet; `tst_fusion_session::{registrationShape, inputsAreBitIdenticalToFixture, missingInputsAreNotApplicable, rejectionIsACachedResult, cancelStopsAtNextBoundary}`; `tst_fusion_runner::{noPreferenceOnTheFitPath, outputTableMatchesGolden, successMatchesDirectRun, matchesTheApplicationImportPath, rejectionExitsOne, legacySchemaScalesTheGyro, usageAndImportFailures}`; `tst_fusion_rows::rejectedTrackShowsBadge`; `tst_solver_smoke::mainThreadHasSolverStack`.
  - **`IMU/temperature` is an ordinary required input** (decision of 2026-09-22, after the phase documents were first written): Phase 6's engine change (the optional-input flag, its Task 6.7) is dropped, `src/engine/` is untouched by the plan, the kernel rejects a recording handed no temperature with a reason naming `IMU/temperature`, and a session without the column has a missing input like one without IMU data. The amended `06-temperature-gyro-bias.md` names the tests this map cites for it: `tst_fusion_kernel::validationRejectsEachDefect` (the two `IMU/temperature` reasons), `tst_fusion_kernel::constantTemperatureKeepsSlopeAtPrior`, `tst_fusion_session::temperatureReachesTheKernel` and `inputsAreBitIdenticalToFixture`; the implementer verifies them against that document before writing the map (a wrong name fails `audit_cleanup`). The names `temperatureAbsentOrConstantKeepsSlopeAtPrior`, `temperatureIsOptional`, `optionalMeasurementDoesNotBlock` and `optionalInputNeverBlocks` are no longer cited; whether the amended Phase 6 keeps a golden fixture without a temperature channel (impossible, if the kernel rejects it: the twelve golden fixtures then carry one) and what its re-capture changed are read from the amended document, not assumed here.
  - The diagnostics of a successful fit carry, after Phase 6: `algorithm` (`batch-temperature-bias-v3`), `input` (the input audit; no temperature flag: the channel is required), `initialization` (`segmented initialization; heading from segment fits`), `stationary_interval_s`, `anchor_time_s`, `selected_heading_deg` (all `null`), `initializer`, `start_s`, `end_s`, `gnss_states`, `imu_outputs`, `objective`, `orientation`, `residuals` (kinds `position`, `velocity`, `imu`, `bias_prior`, `slope_prior`), `seeds` (one entry; `heading_deg` `null`), `seed_comparison_performed`, `max_seed_vs_selected_angle_deg`, `max_seed_vs_selected_acceleration_m_s2`, `max_endpoint_correction_deg`, `stopping`, `quality`, `model` (`per_step` with `gyro_slope_s`, `acc_slope_s`; `gyro_bias` with `b0_rad_s`, `b1_rad_s_per_degc`, `t_ref_degc`, always numbers: no `b1_fixed_zero`, no `null`), `display_position_velocity`, `limitations`. Failure shapes are Phase 3's: `{algorithm, failure}` (rejection; any other fit-stage exception), `{algorithm, failure, quality, stopping}` (`iteration limit`, `bias not settled`), `{algorithm, failure, stopping}` (`cost increased`).
  - `tests/audit/cleanup_audit.cmake` is the audit (registered as `audit_cleanup`, label `audit`; there is no script under `scripts/` for it: `scripts/` holds only `fix_dock_layout.py`). Its traceability block accepts four line forms and the ranges 1-19 and 101-120 (lines 401-491 on `master`); `audit_group()` names groups; the GTSAM-header allowlist (line 315-317) admits `fusion_golden_capture.cpp` since Phase 1.
  - `cmake/SolverDependencies.cmake` lists `_FLYSIGHT_GTSAM_NAMERS` = `flysight_fusion tst_solver_smoke solver_deploy_probe tst_fusion_kernel fusion_golden_capture` and `_FLYSIGHT_GTSAM_REACHERS` = those plus `FlySightViewer flysight_fusion_test_support flysight_fusion_session_support tst_fusion_golden tst_fusion_session tst_fusion_jobs tst_fusion_rows fusion_runner tst_fusion_runner`.
  - The verification build is `build-phase1/` (overview, "Verification commands"); `build/` is never rebuilt. The four reference recordings are under `TEMP/data/` (untracked) at the paths of spec section 12.
  - The implementer runs no git command that changes repository state; read-only git (`status`, `diff`, `show`, `grep`, `rev-parse`, `log`) is fine. The orchestrator stages and commits. Nothing under `PLANS/`, `TEMP/`, `experiments/`, `build*/` is ever staged.

---

## Acceptance map of this plan (items 201-247)

Item = 200 + n, one row per requirement, stated here once and copied into `tests/acceptance_map.txt` (Task 7.4) and `tests/README.md` section 9.3 (Task 7.3). "Section" = spec section of the overview. Evidence names are `target::function`, `audit <group>` (Task 7.5 defines `fusion-model` and `fusion-tooling`), `manual M<k>` (Task 7.3 defines M10-M15) or a document section (README 9.3 only; the map file needs a test or audit line per item, and every row below has one).

| # | Section | Clause | Evidence |
|---|---|---|---|
| 201 | 3.2 step 1 | segments of 600 s from the first fix; a final piece shorter than 120 s joins the one before; a window shorter than one segment is one segment | `tst_fusion_kernel::segmentsAreCutOnFixes`, `shortWindowIsOneSegment`, `driftingBiasSegmentsConverge` (four segments under 60 s / 12 s), `initializerDiagnosticsShape` (`segment_length_s` 600) |
| 202 | 3.2 step 2a | the anchor is the fix with the smallest sAcc, the earliest on a tie; the coarse attitude there (force aligned with GNSS acceleration minus gravity, zero bias) | `tst_fusion_kernel::smallestSaccFixIsTheAnchor`, `shortWindowIsOneSegment` (tie: the first fix), `allPrefixFitsFailFallsBack` (`startRotation` equals `coarseAttitude()` at the anchor bit for bit) |
| 203 | 3.2 step 2b | the prefix is 60 s centred on the anchor, clipped to the segment; its start is the coarse attitude carried to the window's first fix with zero bias; four heading starts with the production graph and tuning | `tst_fusion_kernel::smallestSaccFixIsTheAnchor` (window 170-230 s), `initializerProgressTexts` (`prefix 60 s`, four starts before the segment fit), `initializerDiagnosticsShape` (`prefix_fits` 4) |
| 204 | 3.2 step 2c | the marginal yaw sigma of the window's first pose from the best start's graph rebuilt at the fitted bias, rotated into the navigation frame, vertical element | `tst_fusion_kernel::yawSigmaIsMarginalAboutTheVertical` (exactly unobservable: 180; observable: below 20; invariant to a yaw rotation of the linearization point), `startsInMotionGrowsToTheManoeuvre` (the sigma sequence) |
| 205 | 3.2 step 2d | the window doubles while the sigma exceeds 20 degrees, the window does not cover the segment, and the last doubling cut the sigma by at least 20 %; otherwise growth stops (`observable`, `covers`, `no_gain`) | `tst_fusion_kernel::startsInMotionGrowsToTheManoeuvre` (60 -> 120 s, exactly at the first length containing the manoeuvre), `atRestPrefixStopsGrowing` (`growth_stop` `no_gain` at 120 s of a 300 s resting recording, the second sigma above 0.8 of the first), `startsOnTheLimitAreStillUsed` (`covers`), `tst_fusion_golden::successFixturesMatchGolden` (`stationary_spin`'s `prefix_length_s` 120, `prefix_fits` 8) |
| 206 | 3.2 step 2e | the segment is fitted once from the best prefix fit's attitude carried backwards and forwards with that fit's gyro bias; the fitted attitude of every fix and the bias are kept | `tst_fusion_kernel::smallestSaccFixIsTheAnchor` (the carried-back start, `1e-9`), `driftingBiasSegmentsConverge` (every segment fit converged, `iterations > 0`), `initializerProgressTexts` (the segment fit follows the prefix fits) |
| 250 | 3.2 step 3 | with two or more segments, a segment that fell back or whose segment fit's IMU normalized RMS exceeds twice the median of the fitted segments' is refitted once from its neighbour's boundary attitude and bias; the lower-misfit result is kept and reported | `tst_fusion_kernel::suspectSegmentIsRestartedFromItsNeighbour`, `restartDecisionUsesTwiceTheMedian`, `shortWindowIsOneSegment` (one segment: no restart) |
| 207 | 3.2 step 4 | initial values: per-pose attitude from its segment's fit, GNSS positions and velocities, the first segment's gyro bias, zero accelerometer bias | `tst_fusion_kernel::shortWindowIsOneSegment` (`state.gyroBias == segments[0].gyroBias`, one rotation per fix), `validationRejectsEachDefect` (`initialValues()` refuses a wrong-sized state), `fitTraceMatchesGolden` (the full fit's first cost is the stitched start's) |
| 208 | 3.2 step 5, 3.4 | the full fit runs from the stitched state and converges; a resting segment's yaw is arbitrary and accepted | `tst_fusion_kernel::startsInMotionGrowsToTheManoeuvre` (truth attitude within 2 degrees), `atRestPrefixStopsGrowing` (roll and pitch within 0.5 degrees; yaw unasserted), `driftingBiasSegmentsConverge`; corpus and reference recordings: `manual M11`, `M12`, `M13`, `M14`, `M15` |
| 209 | 3.3 rule 1 | segment fits are cancellable and report progress; each iteration is a boundary; the text names the segment and, for a prefix, its length | `tst_fusion_golden::cancelAtEachKindOfBoundary` (rows `prefix fit iteration`, `prefix fit second iteration`, `segment fit iteration`), `progressMatchesGoldenBoundaries`; `tst_fusion_kernel::initializerProgressTexts` |
| 210 | 3.3 rule 2 | a failed prefix or segment fit is a start with infinite objective; all four failing grows the prefix; all failing at full length falls back to the propagated coarse attitude and the diagnostics say so | `tst_fusion_kernel::allPrefixFitsFailFallsBack` (`coarse_maneuver`: `fallback` true, `fallback_segments` `[0]`, `yaw_sigma_deg` null, the full fit still runs; `motion_start`: growth to 120 s with eight failed starts) |
| 211 | 3.3 rule 3 | a segment satisfies the fitted window's validity rules (fixes inside IMU coverage, no IMU gap, at least three fixes) | `tst_fusion_kernel::segmentsAreCutOnFixes` (a final piece with fewer than three fixes is merged), `validationRejectsEachDefect` |
| 249 | 3.3 Budgets | a prefix fit is one pass of at most 50 iterations, a segment fit uses the production limits; the stopping rule with its slow tail applies to prefix and segment fits; a fit that ends on the limit is still used as a start and reported (`prefix_on_limit`, `segment_on_limit`) | `tst_fusion_kernel::startsOnTheLimitAreStillUsed`, `atRestPrefixStopsGrowing`; `tst_fusion_kernel::validationRejectsEachDefect` (`maxPasses` 0 refused) |
| 212 | 3.3 rule 4 | no stationary-window detector takes part; it decides nothing | `audit fusion-model` (the detector, its gates and the silent poll are absent from the tree); `tst_fusion_golden::cancelDuringPreparation` (preparation asks nothing: the first boundary is `Starting fit`) |
| 213 | 4.1 | the bias-settled test is the re-preintegration cost test at 1e-6 relative; the pass structure (at most five passes) and the settle tolerance 1e-8 are unchanged | `tst_fusion_kernel::biasSettledByCostTest`, `biasNeverSettlesIsSolverFailure` (five passes, `bias not settled`), `exactConstantVelocityFit`, `nonConvergenceIsSolverFailure` (five passes); `tst_fusion_golden::successFixturesMatchGolden` (`stopping.passes` 1, 2, 1 and the thresholds) |
| 214 | 4.2 | a final pass at the limit is accepted when the last 20 iterations' mean relative decrease is below 1e-4 and the position and velocity nRMS are below 2; the diagnostics say so; a fit meeting neither stays a solver failure | `tst_fusion_kernel::slowTailAtTheIterationLimit` (rows `accepted`, `nrms bound fails`, `decrease bound fails`), `nonConvergenceIsSolverFailure` (one iteration cannot fill the window) |
| 215 | 5 formula | `sigma_w = slope x dt x norm(delta omega)`, `sigma_a = slope x dt x norm(delta f)`, change of the interpolated signal across the step, covariance `(density^2 + sigma^2 x dt) I`; zero change gives the density covariance exactly | `tst_fusion_kernel::perStepTermMatchesSpecifiedCovariance`, `perStepTermIsZeroWithoutSignalChange`; `tst_fusion_golden::successFixturesMatchGolden` (`coarse_linear`'s numbers are the density-only model) |
| 216 | 5 constants | slopes 0.026 (gyro) and 0.40 (accelerometer); densities, step boundaries and midpoint sampling unchanged | `tst_fusion_kernel::diagnosticsReportPerStepConstants` (`Tuning{}` values and `model.per_step`), `preintegrationHonoursExactBoundaries` |
| 217 | 5 dt scaling | the dt factor keeps the constants valid at higher rates | `tst_fusion_kernel::perStepTermScalesWithStep` (a wrong-dt expectation is rejected) |
| 218 | 5 constraint | the per-step covariance is applied by setting the shared parameters before each `integrateMeasurement` call | `audit fusion-model` (exactly one `integrateMeasurement(` in `imuintegration.cpp`); `tst_fusion_kernel::perStepTermMatchesSpecifiedCovariance` (the effect) |
| 219 | 6 model | `b(t) = b0 + b1 (T(t) - T_ref)`, `T_ref` the mean IMU temperature over the fitted window, the accelerometer bias constant; each interval evaluated at its own bias in the fit and in the reconstruction | `tst_fusion_kernel::temperatureGraphShape` (`tRef` the index-order mean; the factor's `temperatureDelta`), `temperatureFactorJacobians` (six Jacobians; equal to `ImuFactor` at zero slope), `reconstructionUsesIntervalBias`, `driftingBiasSegmentsConverge` (`b1` within 20 %, `t_ref_degc` 35, `b0` at `T_ref`; `t_ref_degc` and `b1_rad_s_per_degc` are always numbers) |
| 220 | 6 priors | `b0` under today's prior (0.03 rad/s); `b1` zero-mean with sigma 0.010 deg/s per degC | `tst_fusion_kernel::temperatureGraphShape` (the slope prior's sigmas equal `Tuning{}.gyroBiasSlopeSigma`), `validationRejectsEachDefect` (a non-positive `gyroBiasSlopeSigma` is refused), `driftingBiasSegmentsConverge` (`slope_prior` last, after `bias_prior`) |
| 221 | 6 input channel | `IMU/temperature` is the twenty-second input, required, one value per IMU sample; a recording handed no temperature is rejected by the kernel with a reason naming `IMU/temperature`, and a session without the column is blocked like any missing input | `tst_fusion_kernel::validationRejectsEachDefect` (absent, wrong length, non-finite: the reason names `IMU/temperature`), `tst_fusion_kernel::validationRejectsEachDefect`; `tst_fusion_session::registrationShape` (22 inputs, all required), `missingInputsAreNotApplicable` (the row without `IMU/temperature` of the amended Phase 6), `tst_fusion_session::temperatureReachesTheKernel (and inputsAreBitIdenticalToFixture)`; `tst_fusion_golden::rejectionFixturesMatchGolden` (if the amended Phase 6 adds a temperature rejection fixture) |
| 222 | 6 constant temperature | a recording whose temperature does not change leaves `b1` at its prior | `tst_fusion_kernel::constantTemperatureKeepsSlopeAtPrior` (every component of `b1` below 1 % of the prior sigma in magnitude; the slope prior's residual zero) |
| 223 | 6 initializer unaffected | segment and prefix fits use a constant bias; the full fit starts `b1` at zero | `tst_fusion_kernel::driftingBiasSegmentsConverge` (stock prefix fits: `prefix_fits` 4, `prefix_length_s` 60), `temperatureGraphShape` (the four-argument builder is the stock graph); `tst_fusion_golden::successFixturesMatchGolden` (the goldens as the amended Phase 6 re-captured them) |
| 224 | 7 initializer | the diagnostics report the segments (start, end, prefix length, yaw sigma, iterations) and any fallback | `tst_fusion_kernel::initializerDiagnosticsShape` (the key sets), `allPrefixFitsFailFallsBack` (`fallback_segments`); `tst_fusion_golden::successFixturesMatchGolden` |
| 225 | 7 stopping | which rule ended the fit, the last pass's mean relative decrease, the re-preintegration cost difference | `tst_fusion_kernel::biasSettledByCostTest`, `slowTailAtTheIterationLimit`, `failureDiagnosticsShape` (`cost increased` with nulls), `nonConvergenceIsSolverFailure` (the failure key set) |
| 226 | 7 quality | normalized RMS of the IMU, position and velocity factors, and the objective per state | `tst_fusion_kernel::biasSettledByCostTest` (recomputed from the residual array), `exactConstantVelocityFit` |
| 227 | 7 model | the per-step constants and the fitted `b0`, `b1` with `T_ref` (always numbers: the temperature is required) | `tst_fusion_kernel::diagnosticsReportPerStepConstants`, `driftingBiasSegmentsConverge`, `tst_fusion_kernel::constantTemperatureKeepsSlopeAtPrior` |
| 228 | 7 tooltip / account | the tooltip keeps showing the reason; the diagnostics are an account, not an input | `tst_fusion_rows::rejectedTrackShowsBadge` (the reason in the tooltip); `tst_fusion_session::registrationShape` (`_FUSION_DIAGNOSTICS` is an output and no input) |
| 229 | 8 input | a folder or two paths; imported as the application imports (parser, conversion layer, on-demand derivation, the legacy gyro scale); the kernel gets the job queue's channels | `tst_fusion_runner::matchesTheApplicationImportPath`, `successMatchesDirectRun`, `legacySchemaScalesTheGyro`, `outputTableMatchesGolden` |
| 230 | 8 output | diagnostics JSON on stdout; `--csv` writes the seventeen channels; exit 0 for Succeeded, non-zero otherwise with the outcome and reason on stderr | `tst_fusion_runner::successMatchesDirectRun`, `rejectionExitsOne`, `usageAndImportFailures` |
| 231 | 8 no GUI, logbook, preferences | the runner reads and writes no user logbook or settings | `tst_fusion_runner::noPreferenceOnTheFitPath`; `audit fusion-tooling` (`fusion_runner.cpp` names no `LogbookManager`, `PreferencesManager`, `SessionModel`, `SessionImport`, `EnginePreferenceProvider`, `applyCreationDefaults`, `JobQueue`) |
| 232 | 8 progress | progress texts on stderr; cancellation not required | `tst_fusion_runner::successMatchesDirectRun` (stderr is the kernel's texts in order, then `Succeeded`) |
| 233 | 8 tooling target | built with the tests, never shipped | `audit fusion-tooling` (no install rule names `fusion_runner` or `fusion_golden_capture`); `manual M10` |
| 234 | 9 purity, threading, cancellation | a function of the channels, no session or GUI object, every solver iteration a boundary, cancellation observed at boundaries only | `tst_fusion_golden::resultIsIndependentOfCallerState`, `workerThreadMatchesMainThread`, `twoRunsAreBitIdentical`, `cancelAtEachKindOfBoundary`, `cancelDuringPreparation`, `cancelNeverRequestedChangesNothing`; `tst_fusion_session::cancelStopsAtNextBoundary`; `audit solver-confinement` (the kernel is pure, does not log); `audit fusion-model` (exactly three `checkpoint(` call sites, no silent poll) |
| 235 | 9 result contract | channels and outcomes unchanged; a slow-tail acceptance is `Succeeded` | `tst_fusion_kernel::slowTailAtTheIterationLimit` (`Succeeded`, seventeen channels filled, empty reason); `tst_fusion_session::registrationShape` (18 outputs); `tst_fusion_runner::outputTableMatchesGolden` (seventeen channels in order); Task 7.6 step 5 (the `fusion.h` diff) |
| 236 | 9 GTSAM | nothing outside the fusion library links GTSAM | `audit solver-confinement`; `flysight_assert_solver_confinement()` at configure time (Task 7.6 step 3) |
| 237 | 10 goldens | the parity tests are retired; goldens are captured from the changed kernel with the same harness and the regression tests compare against them; fixtures are deterministic | `tst_fusion_golden::successFixturesMatchGolden`, `rejectionFixturesMatchGolden`, `progressMatchesGoldenBoundaries`, `channelsWriterIsTheInverseOfTheLoader`, `fixturesAreDeterministic`, `comparatorHoldsItsBounds`; `tst_fusion_kernel::fitTraceMatchesGolden`, `initializerFixturesAreDeterministic`; Task 7.6 step 1 (the goldens equal a fresh capture) |
| 238 | 10 test 1 | starts in motion: prefixes grow until the sigma is below 20 degrees exactly at the first length containing the manoeuvre; the full fit within 2 degrees of the truth | `tst_fusion_kernel::startsInMotionGrowsToTheManoeuvre` |
| 239 | 10 test 2 | at rest throughout, longer than two doublings: growth stops at 120 s (`no_gain`); the full fit converges; roll and pitch within 0.5 degrees | `tst_fusion_kernel::atRestPrefixStopsGrowing` |
| 251 | 10 test 3b | a three-segment recording in motion whose middle segment's prefix fits are forced to fail: the middle segment is restarted from the first segment's boundary attitude, converges, the restart is kept and reported, the full fit converges within 2 degrees of truth | `tst_fusion_kernel::suspectSegmentIsRestartedFromItsNeighbour` |
| 248 | 10 test 2b | prefix fits run one pass of at most 50 iterations; a prefix and a segment fit forced onto the iteration limit are still used as the start and the diagnostics say so | `tst_fusion_kernel::startsOnTheLimitAreStillUsed`, every initializer test (`prefix_passes` 1, `prefix_iterations` <= 50) |
| 240 | 10 test 3 | sAcc 2 m/s except 0.3 m/s at 200 s: that fix is the anchor, the first prefix is 170-230 s, the segment's start attitude is the prefix fit's carried back | `tst_fusion_kernel::smallestSaccFixIsTheAnchor` |
| 241 | 10 test 4 | longer than two segments with a 1 deg/s linear drift: every segment fit converges; with section 6 the full fit recovers `b1` within 20 % in at most 30 iterations | `tst_fusion_kernel::driftingBiasSegmentsConverge` |
| 242 | 10 test 5 | a segment whose prefix fits all fail falls back, the diagnostics say so, the full fit still runs | `tst_fusion_kernel::allPrefixFitsFailFallsBack` |
| 243 | 10 test 6 | bias-settled test: a fit whose cost stops changing converges within two passes | `tst_fusion_kernel::biasSettledByCostTest` |
| 244 | 10 test 7 | slow tail: a forced final pass at the limit is accepted when both conditions hold and fails when either fails | `tst_fusion_kernel::slowTailAtTheIterationLimit` |
| 245 | 10 test 8 | per-step term: zero change gives the density covariance exactly; a known change gives the specified covariance; the term scales with dt | `tst_fusion_kernel::perStepTermIsZeroWithoutSignalChange`, `perStepTermMatchesSpecifiedCovariance`, `perStepTermScalesWithStep` |
| 246 | 10 test 9 | section 6, as amended: a recording without `IMU/temperature` is rejected by the kernel (the reason names the channel) and blocked in a session like any missing input; constant temperature leaves `b1` at its prior | `tst_fusion_kernel::validationRejectsEachDefect`, `tst_fusion_kernel::constantTemperatureKeepsSlopeAtPrior`; `tst_fusion_session::missingInputsAreNotApplicable` |
| 247 | 11 | `docs/` describes the segmented initializer, the stopping rule with its slow tail, the per-step term and its meaning at other output rates, and the temperature-dependent bias, in the place that documents the fusion model | `docs/SENSOR_FUSION.md` sections 4 and 5 (Task 7.1); `audit fusion-model` (the document describes no retired mechanism: no stationary window, candidate window, coarse initializer, frozen algorithm, bias-shift test, branch name, twenty-one inputs) |

Items with no automated proof (the corpus tally and the four reference recordings of spec section 12) are `manual` lines on item 208 and are stated in `tests/README.md` section 12.2 (Task 7.3); the map's automated-evidence rule is satisfied by the tests on the same row.

---

## Tasks

Order: 7.1, 7.2 (documents) -> 7.3 (`tests/README.md`, which the map's `manual` lines and the audit's doc rule need) -> 7.5 (audit patterns and range) -> 7.4 (the map, checked by the extended audit) -> 7.6 (the closing audit run) -> 7.7 (the whole suite and the manual verification). Tasks 7.1-7.3 may be done in any order among themselves; nothing here changes code under `src/`.

### Task 7.1: `docs/SENSOR_FUSION.md` states the model as it now is

**Purpose:** Spec section 11: the document that describes the fusion model describes the segmented initializer, the stopping rule with its slow-tail acceptance, the per-step noise term and what it means at other output rates, and the temperature-dependent bias; the deferred explanations of Phases 3-6 land here, and nothing in the file describes the retired model.

**Files to modify:**
- `docs/SENSOR_FUSION.md` — sections 1, 3, 4, 5, 6, 7 (one number) and 8 rewritten where the model changed; sections 2 and 7 otherwise as they are

**Technical Approach:**

Keep the file's voice (short declarative sentences, numbers stated once with their unit, bold lead-ins for rules), its eight-section structure and its table of contents. Read the file as Phases 1, 3, 4, 5 and 6 left it before editing; the edits below are stated against that state, and every number is the spec's. Do not paraphrase a spec number; quote it.

**Section 1 (Summary).** Keep the first paragraph. Add one sentence after "solved once": "The solver starts from attitudes fitted segment by segment, holds a gyro bias that follows the IMU temperature, and stops when the cost has stopped moving (sections 4 and 5)." Nothing else changes.

**Section 3 (Inputs).** The list is twenty-two with `IMU/temperature` on the IMU line, "all required" (the amended Phase 6 wrote the list; if an "Optional temperature" paragraph from its earlier draft is still there, delete it). Add one paragraph after "Effective values only": "**Temperature.** `IMU/temperature` is the IMU's own temperature in degrees Celsius, as recorded and never converted, one value per IMU sample; it drives the gyro bias model of section 4. A recording without the column has a missing input, like one without IMU data (section 6)." Check that the sentence "Rates are degrees per second at the boundary of the fit and are converted to radians per second once, inside it" is still there and true (it is; the temperature sentence goes after it).

**Section 4 (Model and output contract).** Rewrite the first two paragraphs as four:

1. *The graph.* The states (pose and velocity per fix), one shared accelerometer bias, the gyro bias model `b(t) = b0 + b1 (T(t) - T_ref)` with `T_ref` the mean IMU temperature over the fitted window, position and velocity factors at every fix, IMU preintegration factors between adjacent fixes, each interval evaluated at the bias of its first fix; no attitude, stationary, zero-velocity or magnetic factors. Priors: 0.3 m/s^2, 0.03 rad/s, and 0.010 deg/s per degC for `b1` (the datasheet's typical drift: a recording without a temperature change leaves `b1` at its prior and a well-behaved unit loses nothing). Phase 6 wrote most of this; make it one paragraph.
2. *Integration and noise.* Splits at exact GNSS boundaries and IMU timestamps, midpoint inputs, densities 0.015 m/s^2/sqrt(Hz) and 0.001 rad/s/sqrt(Hz), integration covariance `I x 1e-8` (as today). Then the per-step term with its explanation (Phase 4's deferral): "The integration treats the IMU stream as piecewise linear between samples. At the default 12.5 Hz output rate that is wrong during manoeuvres by an amount that grows with the change of the signal across a step, so each step adds a white-noise term in quadrature: `sigma = slope x dt x |change of the interpolated signal across the step|`, with slopes 0.026 s (gyro; `sigma` in radians) and 0.40 s (accelerometer; m/s), and the step's covariance is `(density^2 + sigma^2 x dt) I`, i.e. a per-step variance of `density^2 / dt + sigma^2`. The slopes were calibrated at a 0.076 s step. The `dt` factor is what keeps them valid at higher output rates: the sampling error falls with the square of the sample interval, and so does the term, so at 50 Hz and above it vanishes against the density and the model is the density-only one. A step with no signal change has the density covariance exactly, whatever the slopes. These are modelling weights, not sensor specifications; the slopes are reported under `model.per_step`."
3. *Solver and stopping.* Keep "Batch Levenberg-Marquardt uses QR, 100 iterations per pass, a relative cost-change threshold of 1e-8, and up to five bias reintegrations." Then Phase 3's sentences, extended with the meaning (Phase 3's deferral): a pass has settled at 1e-8; the fit has converged when the graph re-preintegrated at the settled pass's bias changes the cost by at most 1e-6 relative (the bias has stopped moving as far as the preintegration can tell); a fifth pass at its iteration limit is accepted when its last 20 iterations lowered the cost by less than 1e-4 relative per iteration on average and the position and velocity normalized RMS (root mean squared whitened residual per scalar component) are both below 2: "a slow tail is a fit that is done for any practical purpose (four ground recordings of the reference corpus end this way with position RMS of 0.3-0.5 m); a fit that is still descending faster, or that disagrees with GNSS, remains a solver failure". Name the five rule texts the diagnostics use. Keep "Reported factors are reintegrated at the final bias."
4. *Dense reconstruction.* As today, plus Phase 6's "the gyro bias of an interval is the model's bias at that interval's first fix".

The outputs table and the unwrap paragraph stay. The diagnostics paragraph becomes the complete key list of the final `successDiagnostics()` (the Assumptions list above), grouped: identity and audit (`algorithm`, `input`); the initializer (`initialization`, the three legacy `null` keys with one clause saying they remain for readers of older diagnostics, `initializer` with `segment_length_s`, `segments` and its ten keys, `fallback_segments`); the fit (`start_s`, `end_s`, `gnss_states`, `imu_outputs`, `objective`, `orientation`, `residuals` with the five kinds, `seeds` with `heading_deg` `null`, `seed_comparison_performed`, `max_seed_vs_selected_angle_deg`, `max_seed_vs_selected_acceleration_m_s2`, `max_endpoint_correction_deg`); `stopping` (`rule` with its five texts, `passes`, `last_pass_mean_relative_decrease`, `repreintegration_cost_difference`, `bias_settled_tolerance`, `slow_tail` with `window`, `max_mean_relative_decrease`, `max_nrms`); `quality` (`imu_nrms`, `position_nrms`, `velocity_nrms`, `objective_per_state`); `model` (`per_step` with `gyro_slope_s`, `acc_slope_s`; `gyro_bias` with `b0_rad_s`, `b1_rad_s_per_degc`, `t_ref_degc`, always numbers); `display_position_velocity`, `limitations`. Then the failure shapes exactly as Phase 3 defined them (three sentences). Keep "A successful stop describes the optimizer's numerical behaviour, not an independent accuracy assessment."

**Section 5 (Initialization and limitations).** Rewrite entirely; Phase 5's one-paragraph replacement is the skeleton. Three paragraphs:

1. *The segmented initializer*, spec section 3.2 in prose with every number: 600 s segments from the first fix, a final piece shorter than 120 s merged; per segment the smallest-sAcc anchor (earliest on a tie; "the single-fix GNSS acceleration has an error of about seven times sAcc, so this bounds the tilt error of the start"), the coarse attitude there; the 60 s prefix centred on the anchor, its start carried back with zero bias, four heading offsets (0, 90, 180, 270 degrees) with the production graph and tuning; the marginal yaw sigma of the window's first pose from the best start's graph rebuilt at the fitted bias; doubling while the sigma exceeds 20 degrees and the window does not cover the segment; one segment fit from the best prefix's attitude and bias; the full fit from every segment's fitted attitudes, the first segment's gyro bias and a zero accelerometer bias. Then the rules: prefix and segment fits are ordinary fits under the stopping rule, cancellable, with progress texts that name the segment; a failed start counts as infinite objective; all four failing grows the prefix; all failing at the segment's full length falls back to the coarse attitude propagated with zero bias over that segment only, and `initializer.fallback_segments` says so. "A segment entirely at rest has no yaw information: its prefix grows to the segment's end and its yaw is arbitrary. That is acceptable: yaw is unobservable there in the full fit too, and neighbouring segments carry the heading through their own motion. Agreement of the fitted yaw between starts is not an observability test and is not used." Keep Phase 5's closing sentence on heading freedom.
2. *Why* (spec section 1 and 3.4, two or three sentences): one anchor attitude propagated with one bias through a long recording ends hundreds of degrees off on a unit whose bias drifts, and a resting unit whose bias exceeds 1 deg/s failed the old detector; starting every pose near its answer converged every recording the reference corpus fitted worst in 9-17 iterations. Name the corpus (97 recordings, seven units) once.
3. *Limitations.* Keep "**Numerical convergence does not establish physical accuracy.**" Replace the deleted `08-35-23` sentences by the motivation and the expected result (spec section 12): "The recording `24-09-07/08-35-23` has no resting window; the previous initializer converged it to an objective of 14 million with a position RMS of 25 m, the segmented one is expected to reach about 11,000 and 0.7 m in under 40 iterations. The temperature model exists for two units of the reference corpus whose gyro bias follows the temperature at 0.10-0.13 deg/s per degC on one axis, ten times the datasheet's typical value; with a constant bias their fits converge with an IMU normalized RMS of about 1.1 where their 600 s segments reach 0.2-0.5." Keep Phase 3's "The stopping test can treat a no-update step as settled, and a slow tail is accepted on numerical grounds alone." Keep the closing sentences on heading ambiguity, local minima and the shared-bias assumption, replacing "the shared constant-bias assumption" by "the shared accelerometer bias and the linear temperature model of the gyro bias". Not in scope, stated in one sentence: the magnetometer is not read, and a per-unit gyro scale factor is not fitted (the corpus shows one unit 2 % off nominal).

**Section 6 (What is rejected).** The first bullet ("a non-finite value in any input channel") and the length bullet now cover `IMU/temperature` like every other channel; add one bullet: "- no `IMU/temperature` handed to the kernel at all (the reason names the channel; inside the application this cannot happen, because a session without the column has a missing input and the fit never runs);". Add to "A solver that does not converge is reported the same way." the clause ", with the stopping rule that ended it in the reason (`Batch fusion did not converge (iteration limit); sensor fusion unavailable`, or `bias not settled`, or `Nonfinite or increasing optimizer cost`)". In the last paragraph, "A recording with no IMU data, no local origin, or no shared time fit" becomes "A recording with no IMU data, no `IMU/temperature` column, no local origin, or no shared time fit".

**Section 7 (Calculation lifecycle).** Only one number: "resolves and captures the twenty-one inputs" -> "the twenty-two inputs". Phase 5 rewrote the cancellation paragraph; confirm it names the three boundary kinds and "preparation neither reports nor asks" and leave it.

**Section 8 (Validation).** Rewrite the opening and the table; keep the exact-mode paragraph and the real-recording check with its `SCHEMA_VER` caveat. Opening: for identical inputs the kernel reproduces its goldens, captured from it by `fusion_golden_capture` at the end of the last phase that changed numerical results (`tests/README.md` section 11); the tests, all labelled `fusion`:

| Test | What it holds |
| --- | --- |
| `tst_fusion_golden` | the kernel through its public API reproduces its goldens for twelve synthetic fixtures (three fits, nine rejections), the progress texts at its boundaries, cancellation at each kind of boundary (prefix, segment and full-fit iterations included), determinism and thread independence |
| `tst_fusion_kernel` | the kernel's stages: the segmented initializer on the five synthetic recordings of the specification, the two stopping rules forced through the tuning, the per-step covariance, the temperature factor's Jacobians and the three temperature cases, and the fit trace iteration by iteration against the goldens |
| `tst_fusion_session` | the registered calculation on real sessions: reads never run it, one request publishes everything, rejections are cached results, a session without `IMU/temperature` has a missing input |
| `tst_fusion_jobs` | the real fit through the job queue: supersede, cancel, rejection, shutdown |
| `tst_fusion_rows` | the plot rows with the real fusion plots, end to end |
| `tst_fusion_runner` | `fusion_runner`, the command-line fit on a recording written as `TRACK.CSV` / `SENSOR.CSV`, against a direct kernel run and against the application's own import path |

Then: "The exact-mode twins (`tst_fusion_*_exact`, six on the capture configuration) ..." with the existing paragraph's content updated from "five" to "six" and from the `sensor-fusion-clean-port` revision to the tool. Add one paragraph on the runner: what it is (a test/tooling target, not installed), the command line (`fusion_runner [options] <folder>` / `<TRACK.CSV> <SENSOR.CSV>`, `--csv`, `--dump-inputs`), the exit codes (0 Succeeded, 1 Rejected, 2 SolverFailed, 3 import, 4 output, 5 internal, 64 usage), that it never reads or writes the user's logbook or settings, and a pointer to `tests/README.md` section 12.2 for the reference recordings. The real-recording paragraph stays; drop "Reference numbers from the branch" in favour of "Reference numbers recorded before this plan's changes (the branch, constant bias, stationary-window start)" and say that with the current kernel the counts still match and the objective differs (the model changed); the `SCHEMA_VER` sentence stays.

Words that must not appear anywhere in the file afterwards (the audit rule of Task 7.5 checks them): `stationary window`, `candidate window`, `coarse initializer`, `frozen`, `bias shifts below`, `zero bias shift`, `sensor-fusion-clean-port`, `twenty-one`. The factor-list sentence "no attitude, stationary, zero-velocity or magnetic measurement factors" and the key name `stationary_interval_s` are fine (they do not match the patterns).

**Acceptance Criteria:**
- [ ] `grep -n -E "stationary window|candidate window|coarse initializer|frozen|bias shifts below|zero bias shift|sensor-fusion-clean-port|twenty-one|optional|b1_fixed_zero|imu_temperature_present" docs/SENSOR_FUSION.md` prints nothing
- [ ] `grep -c -E "600 s|120 s|60 s|20 degrees|0, 90, 180, 270|1e-6|1e-8|20 iterations|1e-4|below 2|0\.026|0\.40|0\.076|0\.010 deg/s per degC|T_ref|t_ref_degc|per_step|gyro_bias|slow tail accepted|bias not settled|cost increased|iteration limit" docs/SENSOR_FUSION.md` is at least 22 (every spec number and rule name appears at least once); the phrase "vanishes against the density" or "vanishes" with "density" appears in section 4
- [ ] The key list names every key of the Assumptions' diagnostics list (check each with `grep -c`), the ten segment keys, the three `slow_tail` keys, the four `quality` keys, the four `gyro_bias` keys, and the three failure shapes with their key sets
- [ ] Section 8's table has six rows naming `tst_fusion_golden`, `tst_fusion_kernel`, `tst_fusion_session`, `tst_fusion_jobs`, `tst_fusion_rows`, `tst_fusion_runner`; `grep -n "tst_fusion_parity" docs/` prints nothing
- [ ] Every test name, key name, file path and number in the file exists or equals the spec's (spot-check: `grep -c "kSlowTailAccepted\|slow tail accepted" src/fusion/factorgraphfit.h` is at least 1; `grep -c "b1_rad_s_per_degc" src/fusion/fusionoutput.cpp` is at least 1; every relative link resolves with `ls`)
- [ ] `ctest --test-dir build-phase1/FlySightViewer-build -C Release -L audit` passes after Task 7.5 (the `naming` group covers `docs/`; the new doc rule passes)

**Complexity:** L

---

### Task 7.2: `docs/CALCULATIONS.md`, `docs/DATA_SCHEMA.md` and `README.md` agree with the fusion document

**Purpose:** The engine document's fusion section and the root README still count twenty-one inputs, call the goldens "captured from the reference implementation" and list the fusion block's executables without the two tools; the engine document must carry no trace of the dropped optional-input mechanism.

**Files to modify:**
- `docs/CALCULATIONS.md` — section 17 (lines 1046-1157 on `master`); section 3 checked for a leftover optional-input bullet
- `README.md` — the option table (line 205), the project tree (line 392), and any stale statement the check below finds
- `docs/DATA_SCHEMA.md` — no edit expected (cross-reference only)

**Technical Approach:**

`docs/CALCULATIONS.md` section 17:
- Table row: "the 21 below" -> "the 22 below"; the input block gains `IMU/temperature` on the IMU line; "**Inputs of the fit** (all required; exactly the members of `Fusion::Channels`, in member order)" -> "(all required; exactly the vector members of `Fusion::Channels`, in member order, then the four origin attributes)".
- The missing-input sentence becomes "A recording without IMU data, without `IMU/temperature`, without a local origin (no fix under 10 m), or without a time fit has a **missing input**: ..." (the rest unchanged).
- "Progress and cancellation": "at its boundaries only (before the fit, between graph-construction blocks, before each optimizer iteration)" -> "at its boundaries only: before the fit, every 256 states of every graph build, and before each optimizer iteration of every fit, the initializer's prefix and segment fits included (`SENSOR_FUSION.md` section 7)".
- "held to the reference by the golden tests" -> "held to the kernel's goldens by the golden tests (`tests/README.md` section 11)".
- The tests sentence at the end adds `tests/tst_fusion_runner.cpp` ("the command-line runner against the application's import path").

`docs/CALCULATIONS.md` section 3: `grep -n -i "optional" docs/CALCULATIONS.md` must print nothing (the optional-input mechanism was dropped with the amended Phase 6; "All declared inputs are required" stands as on `master`). If an earlier draft's bullet is there, delete it and report it; `git diff master -- src/engine` must be empty (Task 7.6 step 7).

`README.md`, check every fusion-related line (`grep -n -i "fusion\|golden\|gtsam" README.md`) and edit only what is stale:
- line 205 (`FLYSIGHT_BUILD_FUSION_TESTS`): "and `solver_deploy_probe`" -> "and the three non-test executables `solver_deploy_probe`, `fusion_golden_capture` and `fusion_runner`";
- line 392 (`tests/data/fusion/`): "Golden outputs captured from the reference implementation" -> "Golden outputs captured from the fusion kernel by `fusion_golden_capture` (tests/README.md section 11)";
- line 207 and 225 were reworded by Phase 1 ("golden regression", `fusion_golden_capture`); confirm and leave;
- line 412 (`docs/SENSOR_FUSION.md` description) and 420 (`tests/README.md` description: "fusion golden parity" -> "fusion golden regression", if Phase 1 missed it) checked.

`docs/DATA_SCHEMA.md`: confirm line 26-27 (`temperature` column, unit `deg C`), section 4 (the legacy gyro correction) and line 168 ("temperatures in degrees Celsius") say what `SENSOR_FUSION.md` section 3 links to; no edit unless a statement contradicts (none expected).

**Acceptance Criteria:**
- [ ] `grep -n "21 below\|21 inputs" docs/CALCULATIONS.md` prints nothing; `grep -n -i "optional" docs/CALCULATIONS.md` prints nothing; `grep -c "IMU/temperature" docs/CALCULATIONS.md` is at least 2 (the input block and the missing-input sentence of section 17)
- [ ] `grep -n "reference implementation\|golden parity" README.md docs/` prints nothing; `grep -c "fusion_runner" README.md` is at least 1
- [ ] Every relative link in the three files resolves (`ls` each target)
- [ ] The phase report states whether a leftover optional-input bullet was found in section 3

**Complexity:** S

---

### Task 7.3: `tests/README.md` final form

**Purpose:** One consistent test document after six phases of additive edits: the executable counts, the tool and runner rows, the final golden-regression section with its fixture tables, the runner's manual verification with the reference recordings, the acceptance matrix of this plan, and the audit's new groups.

**Files to modify:**
- `tests/README.md` — sections 1, 3, 4 (one note), 9 (new 9.3), 10, 11, 12 (restructured into 12.1 and 12.2), new Appendix C, the table of contents

**Technical Approach:**

Read the file as Phases 1-6 left it. Section 11 is "Fusion golden regression" (Phase 1) with the subsections Fixtures, Files, Tolerance policy, The capture tool, Re-capture procedure, Fusion sessions, Real recordings; Phases 3-6 edited the fixture rows, the exact-key sentence and the `trace` key list. Do not rewrite what is right; the edits are:

**Section 1.** Counts: "There are 41 test executables plus the audit. `ctest -N` lists 42 entries, or 47 where the bit-exact runs of the five fusion golden tests are registered" -> "42 test executables plus the audit. `ctest -N` lists 43 entries, or 49 where the bit-exact runs of the six fusion golden tests are registered". The non-test sentence names the three: "`solver_deploy_probe`, `fusion_golden_capture` and `fusion_runner` are also built, but are not tests". In the "Solver and sensor fusion" table add a row for `tst_fusion_runner` after `tst_fusion_jobs`: "`fusion_runner`, the command-line fit, driven as a child process on fixtures written out as `TRACK.CSV` / `SENSOR.CSV`: its diagnostics equal a direct `Fusion::run()` on the fixture and equal the application's own import-and-fit path (`SessionImport` on a `SessionModel`); the CSV output reloads bit for bit; `--dump-inputs` shows the effective inputs, including the legacy gyro scale of a file without `SCHEMA_VER`; a rejection exits 1 with the failure JSON and writes no CSV; usage and import failures exit 64 and 3; no calculation on the fit's input path declares a preference (the premise of the model-free import). Label `fusion`". The `_exact` row: "the five tests above" -> "the six tests above", adding `tst_fusion_runner_exact` ("bit identity across the process boundary"). Rewrite the `tst_fusion_golden`, `tst_fusion_kernel` and `tst_fusion_session` rows as one description each (Phases 1, 3, 4, 5, 6 appended clauses): what the test reaches, then the subjects in spec order (initializer, stopping rule, per-step term, temperature), then the golden comparison. The paragraph after the fusion table introduces the three non-test executables: keep the `solver_deploy_probe` text, add one sentence each for `fusion_golden_capture` (section 11, "The capture tool") and `fusion_runner` (section 12.2).

**Section 3.** Option table, `FLYSIGHT_BUILD_FUSION_TESTS`: the list names `tst_fusion_golden`, `tst_fusion_kernel`, `tst_fusion_session`, `tst_fusion_jobs`, `tst_fusion_rows`, `tst_fusion_runner` and the executables `solver_deploy_probe`, `fusion_golden_capture`, `fusion_runner`. Labels paragraph: "The bit-exact runs have `core`, `fusion` and `exact`" gains "(six on the capture configuration)". The `ctest -L exact` comment reads "bit-exact golden regression" (Phase 1).

**Section 4.** After the `PATH` paragraph, one sentence: a fusion executable, the capture tool and the runner also need the solver runtime directories (`build-solver-deps/GTSAM-install/bin`, `build-solver-deps/oneTBB-install/bin`; section 11's re-capture procedure and section 12.2 spell the full `PATH`).

**Section 9.** Add **9.3 "Sensor fusion improvements (items 201-247)"** after 9.2: one paragraph (the forty-seven requirements of the specification "Sensor fusion: segmented initializer, stopping rule and IMU model", stated in full in appendix C; item = 200 + n; the same four line forms as 9.2; items with manual evidence beyond their tests point to section 12.2), then the matrix of this document's "Acceptance map" verbatim (four columns), the document-section citation of item 247 kept as text. Section 9's intro "Two specifications, two ranges" -> "Three specifications, three ranges".

**Section 10.** After the `widget-free-core` bullet add two bullets, one per group of Task 7.5 (`fusion-model`, `fusion-tooling`), listing the rules in prose the way the `solver-confinement` bullet does, and saying that this file is excluded from the `fusion-model` text rules because this section spells the patterns. Traceability bullet: "an item outside 1-19 and 101-120" -> "outside 1-19, 101-120 and 201-247"; "an item 101-120 has no test or audit line" -> "an item 101-120 or 201-247 has no test or audit line". The `solver-confinement` bullet names the five allowed GTSAM test and tool sources (`tst_solver_smoke.cpp`, `solverprobe.h`, `solver_deploy_probe.cpp`, `tst_fusion_kernel.cpp`, `fusion_golden_capture.cpp`).

**Section 11.** Final state:
- *Intro:* Phase 1's text plus: "Since then every phase that changed numerical results re-captured (the stopping rule, the per-step term, the segmented initializer, the temperature model); the goldens on disk are the capture of the last of them, and the closing audit of the plan re-ran the tool and found no difference."
- *Fixtures:* the two golden tables as Phases 3-6 left their "Exercises" cells (verify: `coarse_linear` one segment, exactly unobservable yaw, zero per-step term; `coarse_maneuver` the full fit from the segment solution, the per-step term at 100 Hz; `stationary_spin` one resting segment whose prefix grows to 120 s, the per-step term at 25 Hz with the 90 deg/s step; and, for each, the temperature channel the amended Phase 6 gave it, stated as that document states it). Rejection rows: any temperature rejection fixture the amended Phase 6 added, with its reason. Then a **third table, "Initializer recordings (not goldens)"**: `motion_start`, `rest_throughout`, `sacc_anchor`, `drifting_bias` with their content (Phase 5 Task 5.6's numbers: duration, rates, attitude, motion, sAcc, noise, seed) and what each exercises (spec section 10's five tests), the `drifting_bias` row also stating Phase 6's temperature ramp (30-40 degC, `b1 = 0.1 deg/s per degC` by construction, `T_ref = 35`), and a sentence that the constant-temperature variant (2001 x 35.0) is made in the test, not in the generator; the other three initializer recordings carry whatever temperature the amended Phase 6 gave them (required input: none is temperature-free). State that `initializerFixture(name)` returns them, `fusion_golden_capture` never sees them, and their expected values live in `tst_fusion_kernel`.
- *Files:* the `trace` key list as Phase 5 wrote it; the diagnostics description becomes "(the key list of `docs/SENSOR_FUSION.md` section 4: the input audit, the segment account, objective, biases, residuals, `stopping`, `quality`, `model`)".
- *Tolerance policy:* the exact-key sentence as Phases 3 and 5 extended it (Phase 6 added no key); the `tst_fusion_*_exact` list gains `tst_fusion_runner_exact` and the `-R` regex gains `runner`.
- *The capture tool*, *Re-capture procedure:* Phase 1's text; the check sentence reads "`stationary_spin`'s `initializer.segments[0].prefix_length_s` is 120" (Phase 5). Add to "what changes": "the `algorithm` string is `batch-temperature-bias-v3` since the temperature model; a capture that prints another string is from a stale build".
- *Fusion sessions:* "twenty-two effective inputs" (Phase 6); verify.
- *Real recordings:* add after the `SCHEMA_VER` sentence: "With the current kernel the counts (9247 GNSS states, 24411 outputs) still hold and the objective is not the branch's whatever the file says: the model changed. The four reference recordings of the specification, with their expected objectives, are in section 12.2."

**Section 12.** Retitle "## 12. Manual verification"; "### 12.1 Plot-driven jobs" holds today's content unchanged (M1-M9); new "### 12.2 The fusion runner and the reference recordings", marked in bold **not part of the automated tests**, steps M10-M15, each opening with its bold id and its map items in parentheses:

- Preamble: the build (`cmake --build build-phase1 --config Release`); the `PATH` (Git Bash): `PATH="/c/Qt/6.9.3/msvc2022_64/bin:$PWD/third-party/GeographicLib-install/bin:$PWD/build-solver-deps/GTSAM-install/bin:$PWD/build-solver-deps/oneTBB-install/bin:$PATH"` (the runner links `flysight_core`, hence GeographicLib; the PowerShell form as section 11 gives it); `R=build-phase1/FlySightViewer-build/Release/fusion_runner.exe`; the recordings under `TEMP/data/` (untracked, on Michael's machine); an output folder `TEMP/runs/` (never staged); reading the JSON with Python 3 (on the machine; `jq` is not assumed): `python -c "import json,sys; d=json.load(open(sys.argv[1])); print(d['stopping']['rule'], d['seeds'][0]['iterations'], round(d['objective']), d['quality'], d['model']['gyro_bias'], [(s['prefix_length_s'], s['yaw_sigma_deg'], s['iterations'], s['fallback']) for s in d['initializer']['segments']])" TEMP/runs/<name>.json`.
- **M10 Runner streams and exit codes (233).** `"$R" --help; echo $?`: the usage on stdout, 64. `"$R" "TEMP/data/Data comp 5 - FS 2 - serie nr 2 - 014667 (test 10)/24-09-05/11-17-12" > TEMP/runs/11-17-12.json 2> TEMP/runs/11-17-12.log; echo $?`: stdout one JSON line; stderr the progress texts (`Starting fit`, `Integrating IMU factors`, `Segment i of n: prefix L s, pass p, iteration k`, `Segment i of n: pass p, iteration k`, `Pass p, iteration k`), then the outcome line; exit 0 for `Succeeded`. With `--csv TEMP/runs/11-17-12.csv`: a CSV whose header is the seventeen names and whose row count equals `imu_outputs`. A folder without `SENSOR.CSV`: exit 3.
- **M11 Reference recording `11-17-12` (208).** Expected (spec section 12): converged (`settled` or `slow tail accepted`), about 12 full-fit iterations, objective near 45,000 with the density-only model; with the per-step term the objective is lower (the lab measured 26,515 with a weaker term), so the iteration count and the rule are the comparison and the objective is recorded. First segment: the unit rests for 180 s, so its prefix grows to 240 s (`prefix_length_s` 240, `yaw_sigma_deg` a few degrees).
- **M12 Reference recording `08-35-23` (208).** `TEMP/data/Data comp 1 - FS 2 - serie nr 2 - 01465 (test 08)/24-09-07/08-35-23`. Expected: converged, objective about 11,000, position RMS 0.7 m (`position_residual_rms_m` in `seeds[0]`; the baseline was 14 million and 25 m), under 40 iterations, `prefix_length_s` 60.
- **M13 Reference recording `10-15-24` (208, 219).** `TEMP/data/Data comp 2 - FS 2 - serie nr 2 - 01086 (pers.)/24-09-07/10-15-24`. Expected: converged in 10-15 iterations, objective about 80,000 with a constant bias and lower with the temperature model; five segments (45 minutes); `b1_rad_s_per_degc[1]` near `2.1e-3` (0.12 deg/s per degC), `t_ref_degc` in the thirties; `quality.imu_nrms` below the constant-bias value (about 1.1). `--dump-inputs TEMP/runs/10-15-24.in.txt`: the `IMU/temperature` line holds values between 26 and 42.
- **M14 Reference recording `08-35-41` (208, 219).** Same unit and day: converged in 10-15 iterations, about 108,000 with a constant bias, the same temperature signature.
- **M15 Corpus tally (208).** Michael's experiment tooling (`experiments/fusion_lab`, untracked), not this repository: 97 recordings, one fit each; the specification expects every recording the corpus fitted worst converged in 9-17 iterations. Recorded here for completeness; no command of this repository runs it.

Pass / fail and the numbers per step go in the phase report. The `manual M<k>` lines of the map resolve against the bold ids (`**M10 ` ... `**M15 `).

**Appendix C.** "The acceptance items of the sensor fusion improvements (201-247)": the forty-seven clauses of the map, numbered 1-47, one sentence each, grouped by spec section with the section number in front, introduced as items 201-247 of `tests/acceptance_map.txt`. Table of contents: 12 (retitled), 12.1, 12.2, Appendix C; every anchor checked.

**Acceptance Criteria:**
- [ ] `grep -n "41 test executables\|42 entries\|47 where\|five fusion golden\|tst_fusion_parity\|twenty-one" tests/README.md` prints nothing
- [ ] `grep -c "^\*\*M1[0-5] " tests/README.md` is 6; `grep -n "^### 9.3\|^### 12.1\|^### 12.2\|^## Appendix C" tests/README.md` shows the four headings; the TOC links resolve
- [ ] Section 9.3's table has 47 rows whose evidence agrees line for line with `tests/acceptance_map.txt` (spot-check items 205, 213, 221, 231, 237 by hand and report which)
- [ ] Section 11 has the three fixture tables; `grep -n "0x8F05000[4-7]" tests/README.md tests/fusion/fusionfixtures.cpp` shows the four seeds in both files
- [ ] Section 12.2's commands run verbatim on the capture machine (Task 7.7 runs them)
- [ ] `ctest --test-dir build-phase1/FlySightViewer-build -C Release -L audit` passes (the `manual` lines resolve; this file is excluded from the text rules its section 10 spells)

**Complexity:** L

---

### Task 7.4: The acceptance map, items 201-247

**Purpose:** Every requirement of the specification resolves mechanically to the test, audit group or manual step that proves it, in the file the audit checks.

**Files to modify:**
- `tests/acceptance_map.txt` — a third block

**Technical Approach:**

Follow the layout of the 101-120 block exactly: `# ---- <title> ----` header, one `# <item> - <clause>` comment per item, lines sorted by item, one line per piece of evidence, the four line forms. Header: `# ---- Sensor fusion improvements (PLANS/fusion-improvements.md): item = 200 + requirement number ----`. Extend the leading comment with the third range: `201-247   sensor fusion improvements (segmented initializer, stopping rule, per-step IMU noise, temperature-dependent gyro bias, the command-line runner): item = 200 + requirement number. Stated in full in tests/README.md, appendix C.`; "every item 101-120 needs at least one test or audit line" gains "or 201-247"; the matrices are "sections 9.1, 9.2 and 9.3".

Lines, from the map of this document: each `target::function` becomes `<item> <target> <function>`; each `audit <group>` becomes `<item> audit <group>`; the manual references become `208 manual M11` ... `208 manual M15` and `233 manual M10`; item 247's document citation has no line form and is carried by `247 audit fusion-model`. A function cited on several rows appears on each. Never cite a `_data` function, a helper, or a CTest name (`tst_fusion_golden_exact`): the audit resolves `::<function>()` in `tests/<target>.cpp`.

Item 104's comment and lines (Phase 1) are unchanged; item 237 restates the fact for the changed kernel and is separate.

**Acceptance Criteria:**
- [ ] `for i in $(seq 201 247); do grep -q "^$i " tests/acceptance_map.txt || echo "missing $i"; done` prints nothing
- [ ] Every item 201-247 has at least one `tst_` or `audit` line; 208 has five `manual` lines and 233 one
- [ ] `ctest --test-dir build-phase1/FlySightViewer-build -C Release -L audit` passes after Task 7.5 (every function, group and manual id exists)
- [ ] Planting `248 tst_smoke importFs2Sensor` fails the audit with "outside 1-19, 101-120 and 201-247"; removing every `240` line fails it with "no resolving test or audit line"; both restored, never committed

**Complexity:** M

---

### Task 7.5: The cleanup audit: two new groups, the new item range

**Purpose:** The retired mechanisms of this plan stay absent, the tools stay isolated and uninstalled, the fusion document stays current, and the map's third range is checked like the other two.

**Files to modify:**
- `tests/audit/cleanup_audit.cmake` — two groups after `widget-free-core` (before "leftover markers"); the traceability block's range checks; the header comment

**Technical Approach:**

Every pattern is a `git grep -E` over its pathspec; verify each against the finished tree before adding it, plant a hit once to prove it fires, and give each rule an `# Allow:` comment as the existing rules have. `tests/audit` and `python_plugins/README.md` are always excluded; `PLANS/`, `experiments/` and `TEMP/` are in no pathspec. Read the table as raw text: `|` inside a pattern is regex alternation.

Header comment: extend the third bullet with "; the stationary-window initializer, its silent poll and the constant-bias algorithm strings retired by the sensor fusion improvements stay absent, and the fusion tools stay isolated (items 212, 218, 231, 233, 234, 247)".

| Group | Rule (label) | Pattern | Pathspec / expectation |
|---|---|---|---|
| `fusion-model` | the stationary-window detector is gone | `stationarywindow|StationaryWindow|assessStationaryWindow|bestStationaryWindow|kMaxMeanRate|imuGapLimit|kWindowLength|kWindowGrid` | `expect_none` in `src tests cmake docs README.md CMakeLists.txt ":!tests/README.md"` |
| | the silent poll is gone | `pollCancel` | `expect_none` in `src tests` |
| | the anchor-attitude initializer is gone | `InitialAttitude|initialAttitude\(|kInitialHeadingDeg|attitudeFromStationaryWindow` | `expect_none` in `src tests` |
| | the retired algorithm strings are gone | `batch-shared-bias-v[12]` | `expect_none` in `src tests docs README.md ":!tests/README.md"` (the goldens say `batch-temperature-bias-v3`; a hit under `tests/data/fusion` means a stale capture) |
| | the branch is history only | `sensor-fusion-clean-port` | `expect_only` with allowed-file regex `^tests/README\.md$` in `src tests docs cmake CMakeLists.txt README.md` (the historical sentence of section 11 and appendix B; if `tests/acceptance_map.txt`'s 101 block still names the branch, reword that comment to name the plan file); `expect_count` of the same pattern in `tests/README.md` with the number found on the finished file (expected 2-3; the `# Allow:` comment says it rises only with a new historical note) |
| | three kinds of boundary | `checkpoint\(` | `expect_count` 3 in `src/fusion` (`Starting fit` in `fusion.cpp`; the pass iteration and `Integrating IMU factors` in `factorgraphfit.cpp`). Allow: a new boundary kind is added to the `run()` comment of `fusion.h`, to `docs/SENSOR_FUSION.md` section 7 and to this count together |
| | one integrateMeasurement, per-step covariance | `integrateMeasurement\(` | `expect_count` 1 in `src/fusion/imuintegration.cpp` |
| | the fusion document describes the current model | `stationary window|candidate window|coarse initializer|frozen|bias shifts below|zero bias shift|sensor-fusion-clean-port|twenty-one` | `expect_none` in `docs/SENSOR_FUSION.md` |
| `fusion-tooling` | the runner never touches the logbook or settings | `PreferencesManager|LogbookManager|EnginePreferenceProvider|SessionModel|SessionImport|applyCreationDefaults|JobQueue` | `expect_none` in `tests/fusion_runner.cpp` (not `QSettings`: the runner's redirect block names it on purpose, Phase 2 Task 2.2) |
| | one number formatter in the tools | `QString::number\(|FloatingPointShortest|std::to_chars|QLocale|'g', 17` | `expect_none` in `tests/fusion_runner.cpp tests/fusion_golden_capture.cpp` |
| | the fusion tools are not installed | `install\(.*(fusion_runner|fusion_golden_capture|solver_deploy_probe)` | `expect_none` in `src tests cmake CMakeLists.txt` |
| | the tools see the public header or the trace seam only | `#include "fusion/(factorgraphfit|initializer|imuintegration|inputadapter|fusionsamples|fusionoutput|fusionprogress|trajectoryreconstruction|temperatureimufactor)\.h"` | `expect_only` allowed-file regex `^src/fusion/|^tests/tst_fusion_kernel\.cpp$` in `src tests` (the runner includes `fusion/fusion.h` and `fusion/fusionregistration.h`; the capture tool and `tests/fusion/fusiontrace.h` include `fusion/fusionpipeline.h`, which is not in the pattern). Verify the exact include set of the finished tree and narrow the regex to it |

Traceability block: the range check gains `OR (item GREATER_EQUAL 201 AND item LESS_EQUAL 247)` and its message names the three ranges; the `foreach(item RANGE 101 120)` automated-evidence loop is followed by the same loop over `RANGE 201 247`; the block's comment and the head of the map name the third range.

Order in the file: `audit_group(fusion-model)` and its rules, then `audit_group(fusion-tooling)` and its rules, after `widget-free-core` and before "leftover markers", so `AUDIT_GROUPS` holds both when the map is read.

**Acceptance Criteria:**
- [ ] `ctest --test-dir build-phase1/FlySightViewer-build -C Release -L audit` passes on the finished tree and prints the new rule count (thirteen more rules than before)
- [ ] Planting each of the following, one at a time, never committed, fails the audit with a message naming the rule: `// pollCancel` in `src/fusion/fusion.cpp`; `// StationaryWindow` in `src/fusion/initializer.cpp`; `"batch-shared-bias-v2"` in `tests/tst_fusion_kernel.cpp`; `// LogbookManager` in `tests/fusion_runner.cpp`; `frozen` in `docs/SENSOR_FUSION.md`; a fourth `checkpoint(` line in `src/fusion/fusion.cpp`; the two map plantings of Task 7.4
- [ ] `cmake -DREPO=. -P tests/audit/cleanup_audit.cmake` (without a build tree) gives the same verdict
- [ ] No rule matches `tests/audit` itself, `PLANS/`, `experiments/` or `TEMP/`

**Complexity:** M

---

### Task 7.6: The closing audit

**Purpose:** Spec section 9 and the overview's closing duties, each as a command with an expected output: the goldens on disk are the last capture, the detector is gone, GTSAM stays confined, the runner touches no logbook or settings, the public contract is unchanged except for the temperature field, a slow-tail acceptance is `Succeeded`, every solver iteration is a boundary, and nothing under `src/` outside `src/fusion/` (the registration included) and the build list changed.

**Files to modify:** none (a defect found here is a fixup to the owning phase per the Commit Policy, never a change in this phase). The report records every command and its output.

**Technical Approach:** Git Bash, repository root, `build-phase1/` built in Release, the working tree at the tip of `fusion-improvements` with Tasks 7.1-7.5 applied. `SOLVER_PATH` below is the `PATH` of `tests/README.md` section 12.2's preamble.

1. **The goldens equal a fresh capture.**
   ```bash
   rm -rf TEMP/golden-check && mkdir -p TEMP/golden-check
   PATH="$SOLVER_PATH" build-phase1/FlySightViewer-build/Release/fusion_golden_capture.exe --revision "$(git rev-parse HEAD)" TEMP/golden-check
   # expected: twelve lines, none "** UNEXPECTED **", "wrote 16 files to .../TEMP/golden-check", exit 0
   diff -rq --strip-trailing-cr tests/data/fusion TEMP/golden-check
   # expected: exactly one line, "Files tests/data/fusion/capture.json and TEMP/golden-check/capture.json differ"
   python - <<'PY'
   import json
   a = json.load(open("tests/data/fusion/capture.json")); b = json.load(open("TEMP/golden-check/capture.json"))
   for k in ("capture_date", "repository_revision"): a.pop(k); b.pop(k)
   print("capture.json otherwise identical:", a == b)
   PY
   # expected: True (hashes, compiler, solver, fixture-generator hashes, machine all equal;
   # the committed revision is the parent of Phase 6's golden commit, the fresh one is HEAD: both are the same kernel)
   ```
   `--strip-trailing-cr` because the working tree has CRLF (`core.autocrlf=true`) and the tool writes LF. A difference in any fixture file means the goldens are not the last capture: the owning phase is the last numerical one (Phase 6); stop and report the first differing file.

2. **The detector is gone.**
   ```bash
   ls src/fusion/stationarywindow.h src/fusion/stationarywindow.cpp 2>&1        # expected: two "No such file" lines
   git grep -n -E "stationarywindow|StationaryWindow|assessStationaryWindow|kMaxMeanRate|pollCancel|InitialAttitude|kInitialHeadingDeg" -- src tests cmake docs README.md CMakeLists.txt ':!tests/README.md' ':!tests/audit'
   # expected: no output
   git grep -n "batch-shared-bias" -- src tests docs README.md ':!tests/README.md' ':!tests/audit'      # expected: no output
   ```
   (Task 7.5's rules keep this true; the commands are the same greps run by hand.)

3. **GTSAM confinement.**
   ```bash
   cmake build-phase1/FlySightViewer-build 2>&1 | grep -E "GTSAM link confinement|Fusion exact tests"
   # expected: "GTSAM link confinement: OK (14 targets reach gtsam)" and "Fusion exact tests registered for Release (...MSVC 19.44...)"
   git grep -n "#include <gtsam/" -- src tests cmake | grep -v -E "^src/fusion/|^tests/(tst_solver_smoke\.cpp|solverprobe\.h|solver_deploy_probe\.cpp|tst_fusion_kernel\.cpp|fusion_golden_capture\.cpp|README\.md):"
   # expected: no output
   git grep -n -E "#include <(gtsam|Eigen)" -- src/fusion/fusion.h src/fusion/fusionregistration.h src/fusion/fusionregistration.cpp tests/fusion_runner.cpp
   # expected: no output
   ```
   The fourteen reachers: `flysight_fusion`, `tst_solver_smoke`, `solver_deploy_probe`, `tst_fusion_kernel`, `fusion_golden_capture`, `FlySightViewer`, `flysight_fusion_test_support`, `flysight_fusion_session_support`, `tst_fusion_golden`, `tst_fusion_session`, `tst_fusion_jobs`, `tst_fusion_rows`, `fusion_runner`, `tst_fusion_runner`. If the count differs, `grep -n "_FLYSIGHT_GTSAM_" cmake/SolverDependencies.cmake` shows the lists; report the difference.

4. **The runner reads and writes no logbook or settings.**
   ```bash
   git grep -n -E "PreferencesManager|LogbookManager|EnginePreferenceProvider|SessionModel|SessionImport|applyCreationDefaults|JobQueue" -- tests/fusion_runner.cpp    # expected: no output
   git grep -c "QSettings" -- tests/fusion_runner.cpp    # expected: the lines of the redirect block only (Phase 2: setDefaultFormat and setPath), no QSettings object constructed
   git grep -n "^#include \"" -- tests/fusion_runner.cpp
   # expected: only dataimporter.h, parsedfile.h, sessiondata.h, sessionmerge.h, csvformat.h, calculations/builtincalculations.h,
   # engine/calctypes.h or engine/calculationregistry.h, fusion/fusion.h, fusion/fusionregistration.h
   ```
   Plus the test that pins the fit path: `ctest --test-dir build-phase1/FlySightViewer-build -C Release -R "tst_fusion_runner$" --output-on-failure` green, `noPreferenceOnTheFitPath` and `matchesTheApplicationImportPath` among its passes (the `-v2` log of the executable lists them).

5. **The public contract is unchanged except for the temperature field.**
   ```bash
   git diff master -- src/fusion/fusion.h | grep -E '^[-+][^-+]' | grep -v -E '^[-+] *///' | grep -v -E '^\+ *QVector<double> imuTemperature;'
   # expected: no output (every changed line is a /// comment line or the one added field)
   for rev in master HEAD; do git show $rev:src/fusion/fusion.h | sed -n '/^enum class Outcome/,/^};/p;/^struct Result/,/^};/p;/^using ProgressFn/p;/^using CancelFn/p;/^Result run(/,/;/p' > TEMP/contract-$rev.txt; done
   diff TEMP/contract-master.txt TEMP/contract-HEAD.txt      # expected: no output (Outcome, Result, the two callback types and run()'s signature are byte-identical)
   git diff master -- src/fusion/fusionregistration.h | grep -E '^[-+][^-+]' | grep -c "fitInputs\|MeasurementReader\|AttributeReader\|channelsFrom\|FitOutputChannel\|fitOutputChannels\|#include"
   # expected: a positive number; the header gained Phase 2's declarations and nothing was removed (git diff ... | grep -E '^-[^-]' prints only comment lines)
   ```
   The tag `plan/fusion-improvements/phase-1-done` has the same `fusion.h` as `master` (Phase 1 does not touch `src/fusion/`); `master` is the reference.

6. **`Outcome` semantics and the boundaries.**
   ```bash
   PATH="$SOLVER_PATH" QT_FORCE_STDERR_LOGGING=1 build-phase1/FlySightViewer-build/Release/tst_fusion_kernel.exe slowTailAtTheIterationLimit -v2 2>&1 | grep -E "PASS|FAIL"
   # expected: PASS for the three rows (accepted -> Succeeded with channels; the two refusals -> SolverFailed)
   PATH="$SOLVER_PATH" QT_FORCE_STDERR_LOGGING=1 build-phase1/FlySightViewer-build/Release/tst_fusion_golden.exe cancelAtEachKindOfBoundary cancelDuringPreparation -v2 2>&1 | grep -E "PASS|FAIL"
   # expected: PASS for the seven boundary rows (before the fit, graph construction, prefix fit iteration x2, segment fit iteration, full fit iteration x2) and the two preparation rows
   git grep -n "checkpoint(" -- src/fusion     # expected: exactly three lines: fusion.cpp "Starting fit", factorgraphfit.cpp passFormat, factorgraphfit.cpp "Integrating IMU factors"
   git grep -n -E "catch *\(" -- src/fusion
   # expected: runPipeline()'s handlers in fusion.cpp (FusionCancelled, FitFailure, std::bad_alloc, std::exception), the two catch (const FitFailure &) of initialize() in initializer.cpp, the catch (const gtsam::IndeterminantLinearSystemException &) of yawSigmaDeg() in factorgraphfit.cpp, and nothing else; list them in the report
   ```
   Compare the `run()` comment of `fusion.h` with the three call sites: it must name `Starting fit`, `Integrating IMU factors` every 256 states of every graph build, and every iteration of every optimizer pass in the prefix, segment and full fits, and say that preparation asks nothing. Report any sentence of the comment that the code does not bear out.

7. **Nothing outside `src/fusion/` and the build list changed.**
   ```bash
   git diff --stat master -- src | grep -v "^ src/fusion/" | grep -v -E "^ src/CMakeLists\.txt |files? changed"
   # expected: no output
   git diff --stat master -- src/engine src/calculations src/sessionmodel.cpp src/mainwindow.cpp
   # expected: no output (the engine is untouched: the optional-input flag was dropped with the amended Phase 6)
   git diff --stat master -- src | grep "^ src/fusion/" | sed 's/ *|.*//' | sort
   # expected, exactly:
   #  src/fusion/factorgraphfit.cpp  factorgraphfit.h  fusion.cpp  fusion.h  fusionoutput.cpp  fusionoutput.h  fusionpipeline.h
   #  fusionprogress.h  fusionregistration.cpp  fusionregistration.h  fusionsamples.cpp  fusionsamples.h  imuintegration.cpp
   #  imuintegration.h  initializer.cpp  initializer.h  inputadapter.cpp  stationarywindow.cpp (deleted)  stationarywindow.h (deleted)
   #  temperatureimufactor.cpp (new)  temperatureimufactor.h (new)  trajectoryreconstruction.cpp  trajectoryreconstruction.h
   # not listed: inputadapter.h, samplestatistics.cpp, samplestatistics.h (if the amended Phase 6 touched
   # inputadapter.h for the required channel, it is listed and the amended document says so)
   git diff master -- src/CMakeLists.txt | grep -E '^[-+][^-+]'
   # expected: the source-list lines only (stationarywindow.* removed, temperatureimufactor.* added)
   ```
   A file outside the set is a defect of the phase that touched it, reported as such.

**Acceptance Criteria:**
- [ ] Step 1: `diff -rq` prints the `capture.json` line only and the Python comparison prints `True`
- [ ] Steps 2-4: every grep prints what its comment says; the configure prints `GTSAM link confinement: OK (14 targets reach gtsam)` and `Fusion exact tests registered for Release`
- [ ] Step 5: both filtered diffs print nothing; `Outcome`, `Result`, `ProgressFn`, `CancelFn` and `run()` are byte-identical to `master`'s
- [ ] Step 6: the named test functions pass; exactly three `checkpoint(` call sites; every `catch` in `src/fusion` is one of those listed; the `run()` comment is borne out by the code
- [ ] Step 7: the `src` file set is exactly the one listed; `git diff --stat master -- src/engine` is empty
- [ ] The report quotes every command and its output; any deviation is filed as `Phase N fixup: <what>` against the owning phase, with this task re-run after the fixup

**Complexity:** M (procedural)

---

### Task 7.7: End to end: the whole suite, and the manual verification

**Purpose:** The suite is green in every mode on the finished tree, with the counts this plan predicts, and the runner is exercised on the four reference recordings against the specification's expectations.

**Files to modify:** none.

**Technical Approach:**

1. Build and configure: `cmake --build build-phase1 --config Release`; `cmake build-phase1/FlySightViewer-build` (the exact-test gate reads `capture.json`; the log says `Fusion exact tests registered for Release`).
2. Counts: `ctest --test-dir build-phase1/FlySightViewer-build -C Release -N | tail -1` prints `Total Tests: 49` (42 executables, `audit_cleanup`, six `_exact` runs); `-N -L fusion` 13 (`tst_solver_smoke`, `tst_fusion_golden`, `tst_fusion_kernel`, `tst_fusion_session`, `tst_fusion_jobs`, `tst_fusion_rows`, `tst_fusion_runner` and their six `_exact` twins); `-N -L exact` 6; `-N -LE fusion` 36; `-N -L audit` 1.
3. Runs, each `--output-on-failure`, all green: `ctest ... -C Release` (the whole suite); `-L fusion`; `-L exact`; `-L audit`; `-LE fusion` (the GTSAM-free run). Quote the four summary lines. `tst_python_bridge` must not be `Disabled`.
4. Manual verification: `tests/README.md` section 12.2, steps M10-M14, verbatim, on the four recordings under `TEMP/data/`; M15 is Michael's and is reported as not run. Record per step: the exit code, `stopping.rule`, `seeds[0].iterations`, `objective`, `quality`, the segments' `prefix_length_s` / `yaw_sigma_deg` / `iterations` / `fallback`, and for M13 and M14 `model.gyro_bias`. Compare with the expectations written in each step; a recording that misses its expectation is reported with its numbers, not adjusted (the expectations are the specification's; whether a miss is a defect or a spec correction is Michael's call).
5. Hand-off list for Michael, in the report: (a) what only a push confirms: the portable-tolerance comparison of the re-captured goldens on GCC and AppleClang (`tst_fusion_golden`, `tst_fusion_kernel`, `tst_fusion_session`, `tst_fusion_jobs`, `tst_fusion_rows`, `tst_fusion_runner`), in particular `stationary_spin`'s arbitrary yaw (Phase 5 Open Questions: remedy in a fixup, never a golden edit); the two new source files compiling with GCC and AppleClang (`temperatureimufactor.cpp`, `fusion_runner.cpp`); (b) the decision that `IMU/temperature` is a required input: a legacy recording whose `SENSOR.CSV` lacks the column never fits (missing input, silently absent from the fusion plots), which the user documentation states; (c) follow-ups the spec recorded as out of scope: a per-unit gyro scale state; (d) the M11-M14 numbers against the spec's, and M15 not run.

**Acceptance Criteria:**
- [ ] `ctest -N` counts 49 / 13 / 6 / 36 / 1 as stated (or the report explains a difference by naming the target)
- [ ] The five `ctest` runs are green; the summary lines are quoted
- [ ] M10-M14 recorded with numbers; M15 reported as not run
- [ ] The hand-off list is in the report

**Complexity:** S

---

## Testing Requirements

### Unit Tests
- No new test function and no change to any test's expectations in this phase. The map cites the functions of Phases 1-6 by their exact names (Assumptions); `audit_cleanup` resolves every citation.
- `audit_cleanup` gains thirteen rules (two groups) and the third item range; its planted-violation checks (Tasks 7.4 and 7.5) are run once, never committed.

### Integration Tests
- `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure` and the four labelled runs of Task 7.7 step 3.
- Configure-time: `GTSAM link confinement: OK (14 targets reach gtsam)`; `Fusion exact tests registered for Release`.
- The closing audit of Task 7.6 (goldens, detector, confinement, runner isolation, contract, semantics, boundaries, `src` diff set).

### Manual Verification
- `tests/README.md` section 12.2, M10-M14, on the four reference recordings (Task 7.7 step 4).

## Notes for Implementer

### Gotchas
- **The audit's patterns match documents that quote them.** `tests/audit` is excluded from every search and `PLANS/` is in no pathspec, but `docs/SENSOR_FUSION.md` and `README.md` are. Write section 5 of the fusion document without the phrases "stationary window", "candidate window", "coarse initializer" and "frozen" (say "the previous initializer", "a resting window", "the coarse attitude at the anchor"); `tests/README.md` is excluded from the `fusion-model` text rules because its section 10 spells them, exactly as the `naming` group treats it. The doc rule of Task 7.5 lists `twenty-one` too: the fusion document's section 7 said it on `master` (line 216) and Phase 6's grep covered the file, but check.
- **`sensor-fusion-clean-port` remains legitimately in three places**: `tests/README.md` (section 11's historical sentence, appendix B's "the branch is ..."), `tests/audit/cleanup_audit.cmake` (excluded) and `tests/acceptance_map.txt`'s 101 block header if it names the branch rather than the plan file (it names the plan file on `master`; if Phase 1 left a branch name there, reword). Everywhere else it is a leftover: `git grep -n sensor-fusion-clean-port -- . ':!PLANS'` before Task 7.5, and account for every hit.
- **`expect_count` counts lines, comments included.** The `checkpoint(` count of 3 and the `integrateMeasurement(` count of 1 trip on a comment that spells the call; reword the comment. Verify both counts on the finished tree before adding the rules.
- **The two `git grep` forms differ.** `git grep --untracked` (the audit) also searches untracked non-ignored files under its pathspec; `experiments/`, `TEMP/` and `build*/` are outside every pathspec, `PLANS/` too. Never add a pathspec that reaches them.
- **The map resolves `::<function>()` textually** in `tests/<target>.cpp`: a function that exists only as a `_data` slot, or a name that appears only in a comment, fails or passes by accident. Cite the test function itself; the audit's check that the source contains `::name()` is the same for a `_data` twin, so citing `successFixturesMatchGolden` (not `_data`) is the rule.
- **Manual ids must be unique bold tokens**: `**M10 ` ... `**M15 `, one space after the number, at the start of the step's paragraph, or the audit's `string(FIND)` will not find them. Keep M1-M9 untouched.
- **Line endings**: the goldens are LF in the index and CRLF in the working tree; use `--strip-trailing-cr` for `diff` and never open a golden in an editor. The scratch capture in `TEMP/golden-check/` is never staged; delete it when done.
- **The capture tool's `capture.json` differs in two keys** from the committed one by design (`capture_date`, `repository_revision`: the committed revision is the parent of the commit that holds the goldens); Task 7.6 compares the rest.
- **`git diff --stat master -- src`** shows a deleted file with `0` additions and its old line count; a renamed file would show as a rename only with `-M`, which is not wanted here (nothing is renamed under `src/`).
- **Do not run `build/`.** Everything runs in `build-phase1/`.
- **Read-only git** for the implementer; the orchestrator stages `docs/SENSOR_FUSION.md`, `docs/CALCULATIONS.md`, `README.md`, `tests/README.md`, `tests/acceptance_map.txt`, `tests/audit/cleanup_audit.cmake` by path.

### Decisions Made
- **Item range 201-247, one item per requirement**, following the `100 + n` convention of the previous plan with `200 + n`: forty-seven requirements from spec sections 3.2 (eight: steps 1, 2a-2e, 3, 4), 3.3 (four), 4 (two), 5 (four), 6 (five), 7 (five), 8 (five), 9 (three), 10 (ten: the goldens and the nine listed tests) and 11 (one). The audit's range check and automated-evidence loop are extended for it; the four line forms are unchanged (no `doc` form: item 247's document citation lives in README 9.3, and the item is carried in the map by the audit rule that keeps the document current).
- **Two audit groups, `fusion-model` and `fusion-tooling`**, rather than one, so that the map can cite the kernel-side rules (items 212, 218, 234, 247) and the tool-side rules (231, 233) separately; the descriptive slugs follow `branch-mechanisms` / `solver-confinement`.
- **The "goldens equal a fresh capture" audit compares files, not hashes**: `diff -rq` on the fifteen fixture files and a two-key-blind comparison of `capture.json`; the tool's own two-run determinism check makes a second scratch run unnecessary (Phase 1's decision).
- **The public-contract audit filters the diff mechanically** (comment lines and the one field) and diffs the extracted `Outcome`, `Result`, callback types and `run()` signature between `master` and `HEAD`, so "byte-identical except the added field" is a command with empty output, not a reading.
- **Manual steps M10-M15 in a new section 12.2**, under a retitled section 12 whose 12.1 is the previous plan's script unchanged; the map's `manual` form and the audit's `**M<k> ` check are reused as they are.
- **Expected objectives in M11-M14 are the specification's**, with the note that the per-step term and the temperature model lower them; the comparison that is asked for is the iteration count, the stopping rule and the quality numbers. A miss is reported, never tuned away.
- **`docs/CALCULATIONS.md` section 17 is this phase's**: no phase edited it, and its "21 inputs" would otherwise contradict the registration's twenty-two.
- **Phase 6's test names were resolved by the planning coordinator** against the amended `06-temperature-gyro-bias.md` (`validationRejectsEachDefect`, `constantTemperatureKeepsSlopeAtPrior`, `temperatureReachesTheKernel`, `inputsAreBitIdenticalToFixture`); the map is checked mechanically, so the implementer confirms each name exists in the test sources before the audit runs.
- **No kernel or test code changes**; a defect found by the closing audit is a fixup to the owning phase (Commit Policy), and this task is re-run after it.

### Open Questions
- None that block implementation. Two notes for the orchestrator: (1) if Task 7.6 step 1 finds a golden that differs from the fresh capture, the last numerical phase (6) is the owner of the fixup, and the re-capture recipe of `tests/README.md` section 11 is the remedy, committed as `Phase 6 fixup: re-capture`; (2) M11-M14 depend on the untracked recordings; if one is missing on the machine, report the step as not run rather than substituting another recording.

## Definition of Done

This phase is complete when:
1. All seven tasks have passing acceptance criteria, in particular Task 7.6's commands with their expected outputs and Task 7.7's counts
2. All tests pass: `ctest -C Release` on `build-phase1/FlySightViewer-build` without `-L`, and with `-L fusion`, `-L exact`, `-L audit`, `-LE fusion`
3. `docs/SENSOR_FUSION.md`, `docs/CALCULATIONS.md`, `README.md` and `tests/README.md` state the model, the inputs, the tools, the counts and the procedures as they are; none names a retired mechanism, the branch outside the historical notes, or twenty-one inputs
4. Every acceptance item 201-247 has resolving lines in `tests/acceptance_map.txt`, section 9.3 matches them line for line, appendix C states the items, and `audit_cleanup` enforces the range with the two new groups
5. No file under `src/` or `tests/*.cpp` changed in this phase (`git status --porcelain -- src tests/*.cpp tests/fusion` prints nothing); the phase's files are exactly `docs/SENSOR_FUSION.md`, `docs/CALCULATIONS.md`, `README.md`, `tests/README.md`, `tests/acceptance_map.txt`, `tests/audit/cleanup_audit.cmake` (plus `docs/DATA_SCHEMA.md` only if a contradiction was found and reported)
6. No TODOs or placeholder text remains; the report carries the closing audit's outputs, the manual numbers M10-M14, and the hand-off list
