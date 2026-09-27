# Phase 3: Stopping rule and quality metrics

## Overview

This phase changes how the batch fit decides it has finished (spec section 4) and adds the quality account beside the verdict (spec section 7). The bias-shift test (`biasSettled()`, thresholds 1e-5 m/s^2 and 1e-6 rad/s) is replaced by a cost test: after a pass settles, the graph is rebuilt at the pass's fitted bias and its cost at the pass's values must differ from the pass's final cost by at most 1e-6 relative. A fifth pass that reaches its iteration limit without settling is accepted as a "slow tail" when its last 20 iterations lowered the cost by less than 1e-4 relative per iteration on average and the position and velocity factors' normalized RMS are both below 2. The reported graph (the rebuild at the fitted bias, today's final `buildFactorGraph()` call) becomes the graph the cost test evaluates, so nothing is built twice; from it the normalized RMS of the IMU, position and velocity factors and the objective per state are computed. The diagnostics gain the `stopping` and `quality` objects of the overview's key layout, `algorithm` becomes `batch-shared-bias-v2`, and a solver failure that ended a pass carries `stopping` and `quality` too.

The first numerical change of the plan: `coarse_maneuver` converges in two passes instead of three and `stationary_spin` in one instead of two (Task 3.9 states the expected golden diff), so the phase ends with the Phase 1 re-capture.

## Dependencies

- **Depends on:** Phase 1 (golden regression harness: `fusion_golden_capture`, `tst_fusion_golden`, the re-capture recipe quoted in Task 3.9)
- **Blocks:** Phase 5 (its prefix and segment fits are ordinary `fitFactorGraph()` fits and terminate under this rule) and, through it, Phase 6. Phase 4 is implemented after this phase and touches only `imuintegration.cpp` and the density-related `Tuning` fields; this phase does not touch `preintegrateImu()`, `preintegrationParams()` or `imuintegration.cpp`.
- **Assumptions:**
  - Phase 1 is committed: `tests/tst_fusion_golden.cpp` exists (the former `tst_fusion_parity.cpp`), `tests/fusion/fusiontrace.h` holds `traceJson()`, `fusion_golden_capture` builds, `tests/README.md` section 11 is "Fusion golden regression" with a "Re-capture procedure" subsection, and the goldens under `tests/data/fusion/` carry the kernel's own progress texts.
  - The kernel is `master`'s (`src/fusion/factorgraphfit.cpp` as at `1f3139a`: `kMaxBiasPasses = 5`, `runOptimizerPass()`, `biasSettled()`, `collectResiduals()`, the rebuild at the fitted bias at the end of `fitFactorGraph()`).
  - The verification build is `build-phase1/` (overview, "Verification commands"); `build/` is never rebuilt.
  - Line numbers below refer to the files as they are on `master` before Phase 1; Phase 1 does not edit `src/fusion/`, so they hold for the kernel. For `tests/README.md` and `tests/tst_fusion_kernel.cpp` (which Phase 1 edits) the text is identified by content.

## Tasks

### Task 3.1: Tuning fields for the two rules

**Purpose:** The thresholds of the cost test and the slow tail are tuning fields with the spec's defaults, so production uses the spec's numbers and a test can force each branch.

**Files to modify:**
- `src/fusion/fusionsamples.h` — four fields on `Tuning`; comment
- `src/fusion/fusionsamples.cpp` — `requireValidTuning()` (lines 52-61)

**Technical Approach:**

Add to `Tuning` (`fusionsamples.h` lines 34-40), after `maxIterations`, keeping `relativeTolerance = 1e-8` and `maxIterations = 100` as they are:

```cpp
double biasSettledTolerance = 1e-6;          ///< a settled pass has converged when re-preintegrating at its bias changes the cost by at most this, relative to max(1, cost)
int slowTailWindow = 20;                     ///< iterations at the end of a final pass at the limit over which the slow tail is judged
double slowTailMaxMeanRelativeDecrease = 1e-4;  ///< slow tail: mean (before - after) / max(1, before) over the window must be below this
double slowTailMaxNrms = 2;                  ///< slow tail: position and velocity normalized RMS must both be below this
```

Extend the struct's comment ("The defaults are the model ... only a test changes anything else") with one sentence: the four stopping fields and `relativeTolerance` may be set to a negative value by a test, which makes the corresponding test impossible to satisfy ("never settles", "never accepted"); production never does.

`requireValidTuning()`: split the one loop into two. The strictly positive list keeps `accDensity`, `gyroDensity`, `accBiasSigma`, `gyroBiasSigma`, `maxGap`. A second loop over `relativeTolerance`, `biasSettledTolerance`, `slowTailMaxMeanRelativeDecrease`, `slowTailMaxNrms` requires only `std::isfinite`, with the same text `"Invalid fusion configuration"`. `slowTailWindow < 1` throws `"Invalid iteration limit"` like `maxIterations < 1`. `relativeTolerance` moves from the positive list to the finite list: the settle test `before - after <= relativeTolerance * max(1, before)` with a negative tolerance is simply never true, which is the only way to force a pass to run to `maxIterations` (see Decisions Made: GTSAM's LM makes a rejected step an exact no-op, so `relativeTolerance = 0` or `1e-300` can settle).

**Acceptance Criteria:**
- [ ] `Tuning{}` has `biasSettledTolerance == 1e-6`, `slowTailWindow == 20`, `slowTailMaxMeanRelativeDecrease == 1e-4`, `slowTailMaxNrms == 2`, `relativeTolerance == 1e-8`, `maxIterations == 100`
- [ ] `validateSamples()` throws `std::invalid_argument` for `slowTailWindow = 0`, for a NaN `biasSettledTolerance`, for an infinite `slowTailMaxNrms`, and does not throw for `relativeTolerance = -1` or `biasSettledTolerance = -1` (asserted in `validationRejectsEachDefect`, Task 3.7)
- [ ] The densities, bias sigmas and `maxGap` still reject zero and negative values

**Complexity:** S

---

### Task 3.2: The stopping account and the quality metrics in the fit contract

**Purpose:** `fitFactorGraph()` keeps its signature; its result and its one failure exception carry everything the diagnostics and the tests need, so the output stage needs no `Tuning` and the kernel tests need no JSON.

**Files to modify:**
- `src/fusion/factorgraphfit.h` — `Stopping`, `Quality`, `StopRule` names, `FitFailure`; `FitResult` gains two members; the comment on `fitFactorGraph()`
- `src/fusion/fusionpipeline.h` — `PipelineTrace` gains `Stopping stopping`; the comment on `baseTuning` (lines 243-245) names the new forcing (`maxIterations = 1` with a negative `relativeTolerance`)

**Technical Approach:**

In `factorgraphfit.h`, before `FitResult`:

- The rule names, as string constants so the kernel tests compare `trace.stopping.rule` with them and `fusionoutput.cpp` writes them unchanged:
  ```cpp
  /// The rule that ended a fit, as the diagnostics report it.
  namespace StopRule {
  constexpr char kSettled[] = "settled";
  constexpr char kSlowTailAccepted[] = "slow tail accepted";
  constexpr char kIterationLimit[] = "iteration limit";
  constexpr char kBiasNotSettled[] = "bias not settled";
  constexpr char kCostIncreased[] = "cost increased";
  }
  ```
  `std::string` for the rule follows `FactorResidual::kind` and `InitialAttitude::method`.
- `struct Stopping`: `std::string rule` (empty until a pass has run); `int passes = 0` (bias passes run, 1..5; the pass a failure happened in counts); `double lastPassMeanRelativeDecrease` (NaN default): the mean of `(before - after) / max(1, before)` over the last `min(slowTailWindow, n)` iterations of the last pass, `n` its iteration count; `double repreintegrationCostDifference` (NaN default): `|cost of the graph rebuilt at the last pass's fitted bias, evaluated at that pass's values - the pass's final cost| / max(1, the pass's final cost)`, NaN when no pass completed; and the four thresholds in force, copied from `Tuning` (`biasSettledTolerance`, `slowTailWindow`, `slowTailMaxMeanRelativeDecrease`, `slowTailMaxNrms`). NaN via `std::numeric_limits<double>::quiet_NaN()` (`<limits>`); the JSON writer turns a non-finite measurement into `null` (Task 3.5).
- `struct Quality { double imuNrms = 0, positionNrms = 0, velocityNrms = 0, objectivePerState = 0; }` with the definition in its comment: normalized RMS of a factor kind is `sqrt(sum over its factors of the squared whitened error / (number of factors x factor dimension))`, dimensions 3 (position, velocity) and 9 (IMU); `objectivePerState` is the objective divided by the number of states.
- `FitResult` gains `Stopping stopping; Quality quality;` after `residuals`. `converged` keeps its meaning (true for `settled` and `slow tail accepted`).
- `class FitFailure : public std::runtime_error` with a public `Stopping stopping` and a constructor `(const std::string &what, Stopping stopping)`. Comment: thrown by `fitFactorGraph()` when a pass makes the cost non-finite or raises it, the one failure the fit cannot continue from; a `std::runtime_error` so that a caller treating every failure alike (`runPipeline()`, the Phase 5 initializer's "infinite-objective start") needs no special case, and carrying the stopping account so the diagnostics can say `cost increased`. Its `what()` stays `"Nonfinite or increasing optimizer cost"` (the overview's Fit contract quotes that text for Phase 5). Include `<stdexcept>`.
- Rewrite the comment on `fitFactorGraph()` (lines 52-55): passes and re-preintegration as today; a settled pass has converged when the graph rebuilt at its bias changes the cost by at most `biasSettledTolerance`; a fifth pass at the limit is accepted on the slow-tail conditions; otherwise the result says which rule ended the fit and `converged` is false; a non-finite or increasing cost throws `FitFailure`. The reported objective, residuals and quality are those of the graph rebuilt at the fitted bias.

`fusionpipeline.h`: `PipelineTrace` gains `Stopping stopping;` after `converged`, with the comment that it is filled whenever a pass ran, including for a `FitFailure`.

**Acceptance Criteria:**
- [ ] `factorgraphfit.h` declares `StopRule::kSettled` ... `kCostIncreased` with exactly the five texts above, `Stopping`, `Quality`, `FitFailure` and the two new `FitResult` members; `fitFactorGraph()`'s signature is unchanged
- [ ] `FitFailure` is a `std::runtime_error` (a `catch (const std::runtime_error &)` catches it) and `what()` is `"Nonfinite or increasing optimizer cost"`
- [ ] `PipelineTrace` has `Stopping stopping`
- [ ] No GTSAM header is added to either header beyond what they include today

**Complexity:** S

---

### Task 3.3: The pass loop: cost test, slow tail, one rebuild

**Purpose:** Replace the bias-shift test by the re-preintegration cost test and add the slow-tail acceptance, keeping the pass structure, the settle tolerance, the checkpoint sequence and the rule that the reported graph is the rebuild at the fitted bias, without building any graph twice.

**Files to modify:**
- `src/fusion/factorgraphfit.cpp` — `runOptimizerPass()` (lines 63-88), `biasSettled()` and its constants (lines 29-33, 91-94: delete), `fitFactorGraph()` (lines 133-161); one new helper

**Technical Approach:**

*Constants.* Delete `kAccBiasSettled`, `kGyroBiasSettled` and `biasSettled()`. Keep `kMaxBiasPasses = 5`, `kStatesPerCheckpoint = 256`, `kCostIncreaseTolerance = 1e-6`.

*Helper.* `double meanRelativeDecrease(const std::vector<FitIteration> &history, int outer, int window)`: over the entries of `history` whose `outer` equals the given pass, take the last `min(window, n)` and return the mean of `(before - after) / std::max(1., before)`. `n >= 1` whenever the pass ran (a pass always records at least one iteration since `maxIterations >= 1`), so the mean is defined; for `n == 0` return NaN.

*`runOptimizerPass()`.* Unchanged in its loop, its checkpoint text `Pass %1, iteration %2`, the `MULTIFRONTAL_QR` solver and the settle test at lines 81-84 (which now works with any finite `relativeTolerance`). The guard at lines 79-80 throws `FitFailure("Nonfinite or increasing optimizer cost", stopping)` instead of `std::runtime_error`, where `stopping` has `rule = StopRule::kCostIncreased`, `passes = outer + 1`, `lastPassMeanRelativeDecrease = meanRelativeDecrease(history, outer, c.slowTailWindow)` (the failing iteration is already in `history`; the mean may be negative or NaN, that is the point), `repreintegrationCostDifference` NaN, and the four thresholds from `c`. The function therefore needs a helper that fills the thresholds from `Tuning` (a small `Stopping thresholdsOf(const Tuning &)`), shared with `fitFactorGraph()`.

*`fitFactorGraph()`.* The loop keeps at most five passes and the re-preintegration between them; what changes is that the rebuild after a pass is done once and serves three purposes: the cost test, the next pass's graph, and (after the last pass) the reported graph. In prose:

1. `validateSamples(d, c)`; `values = initialValues(...)`; `graph = buildFactorGraph(d, bias of values, c, checkpoint)` (pass 1's graph; reports `Integrating IMU factors` as today).
2. For `outer = 0 .. kMaxBiasPasses - 1`:
   a. `settled = runOptimizerPass(graph, values, c, outer, checkpoint, history)`; `stopping.passes = outer + 1`.
   b. `rebuilt = buildFactorGraph(d, bias of values, c, checkpoint)` — the graph re-preintegrated at the pass's fitted bias (same checkpoint reports as any build, so the cancellation boundary between passes is exactly today's). `costRebuilt = rebuilt.error(values)`; `passFinal = history.back().after`; `stopping.repreintegrationCostDifference = |costRebuilt - passFinal| / max(1, passFinal)`.
   c. If `settled` and the difference `<= c.biasSettledTolerance`: `stopping.rule = kSettled`, `converged = true`, leave the loop.
   d. Otherwise, if passes remain: `graph = std::move(rebuilt)` and continue. (A pass that hit the limit is followed by another pass while passes remain, as today; the slow tail is only ever judged on the fifth pass.)
3. After the loop, `rebuilt` is the reported graph in every case: `result.values = values`, `result.objective = costRebuilt` (the value already computed: do not evaluate the graph again), then `collectResiduals(d, rebuilt, result)` (Task 3.4 makes it fill `quality`).
4. If not converged, `lastSettled` being pass five's settle flag:
   - `lastSettled` true: `stopping.rule = kBiasNotSettled` (the fifth pass settled but the cost test failed).
   - `lastSettled` false: the slow tail. `n` = iterations of pass five. Accepted when `n >= c.slowTailWindow` and `stopping.lastPassMeanRelativeDecrease < c.slowTailMaxMeanRelativeDecrease` and `quality.positionNrms < c.slowTailMaxNrms` and `quality.velocityNrms < c.slowTailMaxNrms` (all strict): `rule = kSlowTailAccepted`, `converged = true`. Otherwise `rule = kIterationLimit`.
5. In every case `stopping.lastPassMeanRelativeDecrease = meanRelativeDecrease(history, last outer, c.slowTailWindow)` and the thresholds are copied from `c`. Return `result`.

The position and velocity factors do not depend on the bias, so their normalized RMS is the same in the pass's graph and in the rebuild; the slow tail uses the values `collectResiduals()` computed from the rebuild, which are exactly `quality.position_nrms` and `quality.velocity_nrms` of the diagnostics.

Number of graph builds per fit: `passes + 1`, as today (one per pass plus the final rebuild); the progress sequence of a fit with the same number of passes is therefore identical to today's, and the only progress change on the fixtures comes from the smaller pass counts (Task 3.9).

The header comment on factor order (`factorgraphfit.h` lines 44-46) is binding and unchanged; `buildFactorGraph()` is untouched.

**Acceptance Criteria:**
- [ ] `factorgraphfit.cpp` contains no `biasSettled`, `kAccBiasSettled` or `kGyroBiasSettled`; `fitFactorGraph()` calls `buildFactorGraph()` exactly `passes + 1` times per fit (one call site before the loop, one inside it)
- [ ] `result.objective` is the value the cost test used (`rebuilt.error(values)` evaluated once) and `collectResiduals()` receives that same `rebuilt` graph
- [ ] With `Tuning{}`, `fitFactorGraph()` on `coarse_maneuver`'s window ends with `stopping.rule == "settled"` and `stopping.passes == 2`, on `stationary_spin`'s with `passes == 1`, on `coarse_linear`'s with `passes == 1` (visible in the re-captured goldens' `stopping.passes` and `seeds[0].iterations`: 6, 5, 2)
- [ ] With `relativeTolerance = -1`, `maxIterations = 25` on `coarse_maneuver`: five passes of exactly 25 iterations, `rule == "slow tail accepted"`, `converged == true`; with `slowTailMaxNrms = 0` or `slowTailMaxMeanRelativeDecrease = 0` in addition: `rule == "iteration limit"`, `converged == false` (Task 3.7 asserts these through `runPipeline`)
- [ ] With `maxIterations = 1`, `relativeTolerance = -1`: `rule == "iteration limit"` after five one-iteration passes (the window is not reached: 1 < 20)
- [ ] With `biasSettledTolerance = -1` on `coarse_linear`: `rule == "bias not settled"`, `passes == 5`
- [ ] A `FitFailure` from a pass carries `rule == "cost increased"`, `passes == outer + 1`, the thresholds, and a NaN `repreintegrationCostDifference`
- [ ] Every iteration is still a checkpoint; the rebuild between passes still reports `Integrating IMU factors` every 256 states and nothing else is reported between passes

**Complexity:** M

---

### Task 3.4: Normalized RMS and objective per state

**Purpose:** The quality metrics of spec section 7, computed from the reported graph in the same walk that already collects the per-factor residuals.

**Files to modify:**
- `src/fusion/factorgraphfit.cpp` — `collectResiduals()` (lines 98-113)

**Technical Approach:**

`collectResiduals()` walks the factors in insertion order and pushes `2 * factor->error(values)` per factor, i.e. the squared whitened residual `r' Sigma^-1 r` (GTSAM's factor error is half of it). While pushing, accumulate three sums: position, velocity, IMU. After the loop, with `n = d.gnssTime.size()`:

- `result.quality.positionNrms = sqrt(sumPosition / (3 * n))`
- `result.quality.velocityNrms = sqrt(sumVelocity / (3 * n))`
- `result.quality.imuNrms = sqrt(sumImu / (9 * (n - 1)))` (`n >= 3` is guaranteed by `fittedWindow()` / `validateSamples()`, so `n - 1 >= 2`)
- `result.quality.objectivePerState = result.objective / n` (`collectResiduals()` is called after `result.objective` is set, as today; keep that order)

`positionRms` and `velocityRms` (the vector RMS of the fitted state against the GNSS measurement, lines 106-111) stay exactly as they are. The bias prior contributes to no normalized RMS. This is the lab's definition (`experiments/fusion_lab/fusion_lab.cpp` line 339, `sqrt(sum / (dim * count))` with dimensions 3, 3, 9).

Expected values, computed from the committed goldens' `residuals` arrays (they do not change in this phase for `coarse_linear` and `stationary_spin`; `coarse_maneuver`'s change in the 13th digit): `coarse_maneuver` position 0.0978, velocity 0.191, IMU 0.00311, objective per state 0.0715; `stationary_spin` 0.102, 0.162, 0.0118, 0.0555; `coarse_linear` 4.6e-7, 8.2e-9, 2.5e-10, 3.2e-13.

**Acceptance Criteria:**
- [ ] The three normalized RMS values and the objective per state appear in `FitResult::quality` for every fit that completes its passes, computed from the rebuild at the fitted bias
- [ ] For each success fixture, recomputing the three values from the diagnostics' `residuals` array (sum of `squared_whitened_error` per `kind`, divided by `dimension x count`, square root) agrees with `quality` within the portable bound, and `objective_per_state` equals `objective / gnss_states` (Task 3.7 asserts this)
- [ ] `positionRms` and `velocityRms` are bit-identical to today's on `coarse_linear` and `stationary_spin` (their goldens' `position_residual_rms_m` and `velocity_residual_rms_m_s` do not change in the re-capture)

**Complexity:** S

---

### Task 3.5: Outcome and diagnostics

**Purpose:** A slow-tail acceptance is a Succeeded result; a non-converged fit is `SolverFailed` with a reason that names the rule; the diagnostics report `stopping` and `quality` per the overview's key layout, `algorithm` becomes `batch-shared-bias-v2`, and the failure diagnostics have a defined shape.

**Files to modify:**
- `src/fusion/fusion.cpp` — `fitAndAssemble()` (lines 61-81), `withoutChannels()` (84-91), `runPipeline()` (95-127)
- `src/fusion/fusionoutput.h` — `failureDiagnostics()` signature
- `src/fusion/fusionoutput.cpp` — `kAlgorithm` (line 17), `successDiagnostics()` (84-110), `failureDiagnostics()` (112-115), two new writers

**Technical Approach:**

*`fusionoutput.cpp`.*
- `kAlgorithm = "batch-shared-bias-v2"`.
- `QJsonObject stoppingObject(const Stopping &s)` in the anonymous namespace: `{"rule": s.rule, "passes": s.passes, "last_pass_mean_relative_decrease": number or Null, "repreintegration_cost_difference": number or Null, "bias_settled_tolerance": s.biasSettledTolerance, "slow_tail": {"window": s.slowTailWindow, "max_mean_relative_decrease": ..., "max_nrms": ...}}`. A measurement that is not finite is written as `QJsonValue::Null` explicitly (the pattern of `max_seed_vs_selected_angle_deg` at line 102); do not rely on what `QJsonValue(double)` does with a NaN.
- `QJsonObject qualityObject(const Quality &q)`: `{"imu_nrms", "position_nrms", "velocity_nrms", "objective_per_state"}`.
- `successDiagnostics()`: add `{"stopping", stoppingObject(fit.stopping)}` and `{"quality", qualityObject(fit.quality)}` to the initializer list (QJsonObject sorts keys; position in the list does not matter). Every existing key stays with its meaning; `seeds[0].converged` is `fit.converged` (true for a slow-tail acceptance) and `seeds[0].iterations` stays `fit.history.size()`.
- `failureDiagnostics(const QString &reason, const Stopping *stopping = nullptr, const Quality *quality = nullptr)`: `{"algorithm", "failure"}` as today, plus `"stopping"` when `stopping` is given and `"quality"` when `quality` is given. Update the declaration and its comment in `fusionoutput.h`.

*`fusion.cpp`.*
- `withoutChannels(Outcome, const QString &reason, const Stopping *stopping = nullptr, const Quality *quality = nullptr)` passes them through to `failureDiagnostics()`.
- `fitAndAssemble()`: after `fitFactorGraph()` returns, fill the trace (`history`, `converged`, and now `stopping`). If `!fit.converged`, return `withoutChannels(Outcome::SolverFailed, reason, &fit.stopping, &fit.quality)` instead of throwing, with `reason = QStringLiteral("Batch fusion did not converge (%1); sensor fusion unavailable").arg(QString::fromStdString(fit.stopping.rule))`, i.e. `Batch fusion did not converge (iteration limit); sensor fusion unavailable` or `... (bias not settled); ...`. Otherwise reconstruction, channels and `successDiagnostics()` as today.
- `runPipeline()`: in the second `try`, add `catch (const FitFailure &e)` **before** `catch (const std::exception &e)` (it derives from `std::runtime_error`; a compiler warns if the order is wrong): `if (trace) trace->stopping = e.stopping; return withoutChannels(Outcome::SolverFailed, QString::fromUtf8(e.what()), &e.stopping);` — no quality, since the fit did not reach a rebuild. The reason text is the exception's, unchanged. `FusionCancelled` and `std::bad_alloc` handling stay as they are; the classification by stage is unchanged.

*The failure-diagnostics shape (the overview left it to this phase):*
- Rejected: `{algorithm, failure}` — unchanged (`tst_fusion_session::rejectionIsACachedResult` and `tst_fusion_golden::rejectionFixturesMatchGolden` pin this key set).
- SolverFailed after the fit completed its passes (`iteration limit`, `bias not settled`): `{algorithm, failure, quality, stopping}`; `quality` is included because the two nrms values explain a refused slow tail.
- SolverFailed from a `FitFailure` (`cost increased`): `{algorithm, failure, stopping}` with `repreintegration_cost_difference` null.
- SolverFailed from anything else in the fit stage (reconstruction, output): `{algorithm, failure}`.
- A Cancelled result has no diagnostics, as today.

**Acceptance Criteria:**
- [ ] `successDiagnostics()` output has the keys it has today plus `stopping` and `quality`, `algorithm == "batch-shared-bias-v2"`, and `stopping` has exactly `rule`, `passes`, `last_pass_mean_relative_decrease`, `repreintegration_cost_difference`, `bias_settled_tolerance`, `slow_tail` (with `window`, `max_mean_relative_decrease`, `max_nrms`); `quality` has exactly `imu_nrms`, `position_nrms`, `velocity_nrms`, `objective_per_state`
- [ ] A slow-tail acceptance returns `Outcome::Succeeded` with all seventeen channels filled, an empty `reason`, and `stopping.rule == "slow tail accepted"` in its diagnostics
- [ ] A fit ending on `iteration limit` or `bias not settled` returns `Outcome::SolverFailed`, `reason` equal to `Batch fusion did not converge (<rule>); sensor fusion unavailable`, no channels, and diagnostics with the sorted key list `algorithm, failure, quality, stopping` whose `failure` equals `reason`
- [ ] `failureDiagnostics(reason, &stopping)` with `rule == "cost increased"` and NaN measurements yields the key list `algorithm, failure, stopping` with both measurements `null` (Task 3.7 asserts this directly)
- [ ] Rejections still produce exactly `{algorithm, failure}` with `algorithm == "batch-shared-bias-v2"`
- [ ] `PipelineTrace::stopping` is filled for every run that started a pass, including one that threw `FitFailure`

**Complexity:** M

---

### Task 3.6: Exact keys in the golden comparator

**Purpose:** The new counts and copied thresholds are exact in both comparison modes, like every other count.

**Files to modify:**
- `tests/fusion/fusiongolden.cpp` — `isExactKey()` (lines 80-88 on `master`; Phase 1 does not move it)
- `tests/tst_fusion_golden.cpp` — `comparatorHoldsItsBounds()`: one assertion pair

**Technical Approach:**

Add `"passes"`, `"window"`, `"bias_settled_tolerance"`, `"max_mean_relative_decrease"` and `"max_nrms"` to the `keys` set of `isExactKey()`: `passes` and `window` are counts, the other three are copies of tuning values, "never the result of solver arithmetic" (the function's own comment). `rule` is a string and is exact already. The measurements (`last_pass_mean_relative_decrease`, `repreintegration_cost_difference`, the four `quality` numbers) are solver arithmetic and stay under the portable bound; note that `repreintegration_cost_difference` may be `null` in a golden (never for the three success fixtures) and `compareJson()` already compares nulls by type.

In `comparatorHoldsItsBounds()`, next to the `iterations` pair (`jsonPasses("iterations", 7, 7)` / `!jsonPasses("iterations", 7 + 1e-9, 7)`), add the same pair for `passes` with 2.

**Acceptance Criteria:**
- [ ] `isExactKey()` returns true for the five new keys and `comparatorHoldsItsBounds` has the `passes` pair
- [ ] `tst_fusion_golden` and `tst_fusion_golden_exact` pass after the re-capture (Task 3.9)

**Complexity:** S

---

### Task 3.7: Kernel tests for the two rules and the failure shapes

**Purpose:** The spec's two synthetic tests ("bias-settled test: a fit whose cost stops changing converges within two passes"; "slow tail: a forced final pass at the limit is accepted when both conditions hold and is a failure when either fails"), stated on existing fixtures with forced tuning, plus the failure shapes and the re-specified non-convergence test. Every test runs in seconds.

**Files to modify:**
- `tests/tst_fusion_kernel.cpp` — five new slots, three edited tests

**Technical Approach:**

All fits go through `runPipeline(toChannels(fusionFixture(name)), tuning, Checkpoint(), &trace)` as `nonConvergenceIsSolverFailure` does, and read the diagnostics with `QJsonDocument::fromJson(result.diagnosticsJson.toUtf8()).object()`; `stopping` is `diagnostics.value("stopping").toObject()`. Nothing new is added to `fusionfixtures.cpp`: every expected value can be stated on `coarse_maneuver` (28 states, 6 s) or `coarse_linear` with forced tuning, which the assignment prefers over new fixtures.

1. **`validationRejectsEachDefect`** (line 212): after the existing tuning-free checks add, with `const Samples d = boundarySamples(...)` already in scope: `Tuning t; t.slowTailWindow = 0;` throws `std::invalid_argument`; `t = Tuning{}; t.biasSettledTolerance = NaN` throws; `t = Tuning{}; t.slowTailMaxNrms = infinity` throws; `t = Tuning{}; t.relativeTolerance = -1; validateSamples(d, t)` does not throw; likewise `biasSettledTolerance = -1`.

2. **`exactConstantVelocityFit`** (line 467): after `QVERIFY(fitted.objective < 1e-12)` add `QCOMPARE(fitted.stopping.rule, std::string(StopRule::kSettled))`, `QCOMPARE(fitted.stopping.passes, 1)`, `QVERIFY(fitted.stopping.repreintegrationCostDifference < 1e-12)` (the golden shows 1e-22), `QVERIFY(fitted.quality.positionNrms < 1e-5 && fitted.quality.velocityNrms < 1e-5 && fitted.quality.imuNrms < 1e-5)`, `QVERIFY(fitted.quality.objectivePerState < 1e-12)`, and `QVERIFY(std::isfinite(fitted.stopping.lastPassMeanRelativeDecrease))`.

3. **`biasSettledByCostTest`** (new; the spec's bias-settled test). `coarse_maneuver`, `Tuning{}`. Comment: under the bias-shift rule this fixture needed a third pass of one iteration that lowered the cost by 8e-15 to prove the bias had stopped moving (the committed history before this phase has passes of 4, 2, 1 iterations); under the cost test the second pass's re-preintegration changes the cost by 3.8e-12 relative and the fit is converged there. Assert: `Succeeded`; `trace.converged`; `trace.stopping.rule == kSettled`; `trace.stopping.passes <= 2` (2 on the capture machine; `qInfo()` the value); the number of distinct `outer` values in `trace.history` equals `trace.stopping.passes`; the cost test re-derived from the trace: with `after` the last history row's `after` and `objective` the diagnostics' `objective`, `|objective - after| <= 1e-6 * max(1, after)`, and `stopping.repreintegration_cost_difference` in the JSON is within `1e-9` of that ratio; `stopping.bias_settled_tolerance == 1e-6`, `slow_tail.window == 20`, `slow_tail.max_mean_relative_decrease == 1e-4`, `slow_tail.max_nrms == 2`; `seeds[0].converged == true` and `seeds[0].iterations == int(trace.history.size())`; `algorithm == "batch-shared-bias-v2"`. Then the quality recomputation of Task 3.4: walk `residuals`, sum `squared_whitened_error` by `kind`, and `QVERIFY(withinPortableBound(quality.position_nrms, sqrt(sumPosition / (3 * 28))))` etc. with dimensions 3, 3, 9 and counts 28, 28, 27 (`gnss_states` is 28); `QVERIFY(sameRecomputedValue(quality.objective_per_state, objective / 28.))`.

4. **`slowTailAtTheIterationLimit`** (new, data-driven; the spec's slow-tail test). Base tuning: `relativeTolerance = -1` (a pass never settles: `before - after >= -1e-6` by the cost-increase guard, so it is never `<= -max(1, before)`), `maxIterations = 25` (at least the 20-iteration window). On `coarse_maneuver` every pass then runs exactly 25 iterations: the first four do the work and the rest are steps of order 1e-15 or exact no-ops (GTSAM's LM leaves the values untouched when it rejects a step), so the last 20 iterations of pass five have a mean relative decrease of about 0 and the fit's position and velocity normalized RMS are 0.098 and 0.19 (Task 3.4). Rows:
   - `"accepted"`: base tuning. Expect `Succeeded`, empty `reason`, all seventeen channels non-empty (`result.time.size() > 0`, `qw` too), `trace.converged`, `trace.stopping.rule == kSlowTailAccepted`, `trace.stopping.passes == 5`, `trace.history.size() == size_t(125)`, every history row with `before >= after`; JSON `stopping.rule == "slow tail accepted"`, `stopping.passes == 5`, `0 <= last_pass_mean_relative_decrease < 1e-4`, `quality.position_nrms < 2`, `quality.velocity_nrms < 2`, `seeds[0].converged == true`, `seeds[0].iterations == 125`.
   - `"nrms bound fails"`: base tuning plus `slowTailMaxNrms = 0`. Expect `SolverFailed`; `reason == "Batch fusion did not converge (iteration limit); sensor fusion unavailable"`; `!trace.converged`; `trace.stopping.rule == kIterationLimit`; `trace.history.size() == 125`; `diagnostics.keys() == {"algorithm", "failure", "quality", "stopping"}` (QJsonObject sorts); `failure == reason`; `stopping.rule == "iteration limit"`, `stopping.passes == 5`, `slow_tail.max_nrms == 0`; `quality.position_nrms > 0`; every channel empty (the six-array check of the existing test).
   - `"decrease bound fails"`: base tuning plus `slowTailMaxMeanRelativeDecrease = 0`. Same expectations as the previous row with `slow_tail.max_mean_relative_decrease == 0` and `last_pass_mean_relative_decrease >= 0` (a rejected LM step is an exact no-op and an accepted one lowers the cost, so the mean is never negative; the strict `< 0` therefore fails).
   Three fits of 125 iterations on 28 states: well under a second each.

5. **`nonConvergenceIsSolverFailure`** (line 518, re-specified). Replace `relativeTolerance = 1e-300` by `relativeTolerance = -1` and delete the paragraph "THE ONE PLACE TO WATCH ON OTHER PLATFORMS": with a negative tolerance no pass can settle on any platform, so the fifth pass ends at its one-iteration limit deterministically, and one iteration cannot fill the 20-iteration slow-tail window, so the slow tail is not judged. Assert: `SolverFailed`; `reason == "Batch fusion did not converge (iteration limit); sensor fusion unavailable"`; `!trace.converged`; `trace.history.size() == 5`; `trace.stopping.rule == kIterationLimit`; `trace.stopping.passes == 5`; `diagnostics.keys() == {"algorithm", "failure", "quality", "stopping"}`; `failure == reason`; `stopping.passes == 5`; the channel-empty check as today.

6. **`biasNeverSettlesIsSolverFailure`** (new). `coarse_linear`, `Tuning t; t.biasSettledTolerance = -1;` (the cost test can never pass, so every settled pass is followed by another until the fifth). Expect `SolverFailed`; `reason == "Batch fusion did not converge (bias not settled); sensor fusion unavailable"`; `trace.stopping.rule == kBiasNotSettled`; `trace.stopping.passes == 5`; `diagnostics.keys() == {"algorithm", "failure", "quality", "stopping"}`; `quality.position_nrms < 1e-3`; `stopping.bias_settled_tolerance == -1`. Five passes of one or two iterations on 9 states: milliseconds.

7. **`failureDiagnosticsShape`** (new; the `cost increased` shape without forcing the kernel, see Decisions Made). Build `Stopping s; s.rule = StopRule::kCostIncreased; s.passes = 2;` with the measurements left NaN and the thresholds set to `Tuning{}`'s, and call `failureDiagnostics(QStringLiteral("Nonfinite or increasing optimizer cost"), &s)`. Assert keys `{"algorithm", "failure", "stopping"}`, `algorithm == "batch-shared-bias-v2"`, `stopping.rule == "cost increased"`, `stopping.passes == 2`, `stopping.value("last_pass_mean_relative_decrease").isNull()`, `stopping.value("repreintegration_cost_difference").isNull()`, `slow_tail.window == 20`. Also `failureDiagnostics(QStringLiteral("x"))` has exactly `{algorithm, failure}`. `fusionoutput.h` is an internal header this file may include (it already includes `factorgraphfit.h` and `fusionpipeline.h`; add `fusion/fusionoutput.h`).

8. **`fitTraceMatchesGolden`** (line 491): unchanged in code; it passes only after the re-capture (its history sizes shrink to 6 and 5 for `coarse_maneuver` and `stationary_spin`).

Update the file's header comment (lines 1-8) to mention the stopping rule among what the kernel tests reach that the goldens cannot ("the two stopping rules forced through the tuning"). Register the new slots in the class declaration in the order above.

**Acceptance Criteria:**
- [ ] `tst_fusion_kernel` has the slots `biasSettledByCostTest`, `slowTailAtTheIterationLimit` (three data rows), `biasNeverSettlesIsSolverFailure`, `failureDiagnosticsShape`, and the re-specified `nonConvergenceIsSolverFailure` no longer contains `1e-300` or the platform paragraph
- [ ] Every assertion listed above holds on the capture machine; `ctest -C Release -R "tst_fusion_kernel(_exact)?$" --output-on-failure` passes after Task 3.9, and `tst_fusion_kernel` alone finishes in under 60 s
- [ ] No test compares a number against a literal that is solver arithmetic except through `withinPortableBound()`, `sameRecomputedValue()` or an inequality with a stated margin (the spec's expected values are stated independently of the goldens)
- [ ] No new fixture is added to `tests/fusion/fusionfixtures.cpp` (the fixture generator hashes in `capture.json` are the proof)

**Complexity:** L

---

### Task 3.8: Documentation that must not lie

**Purpose:** Phase 7 rewrites the fusion documentation; until then the statements this phase falsifies are corrected in place, minimally.

**Files to modify:**
- `docs/SENSOR_FUSION.md` — section 4 convergence sentence (lines 98-104), the diagnostics key list (135-147), the "zero bias shift" sentence in section 5 (173-175), the first sentence of section 8 (258-261, after Phase 1's edit of the table below it)
- `tests/README.md` — the `coarse_maneuver` row of the fixtures table (section 11, "Fixtures"; on `master` line 837) and the portable-mode sentence listing the always-exact counts (section 11, "Tolerance policy"; on `master` line 933)

**Technical Approach:**

`docs/SENSOR_FUSION.md`, exact edits:

1. Section 4, the sentence "Convergence requires a settled cost and bias shifts below 1e-5 m/s^2 and 1e-6 rad/s." becomes: "A pass has settled when an iteration lowers the cost by at most 1e-8 of max(1, cost); the fit has converged when the graph re-preintegrated at the settled pass's bias changes that cost by at most 1e-6 relative (of max(1, cost)). A fifth pass that reaches its iteration limit without settling is accepted when, over its last 20 iterations, the mean relative decrease per iteration is below 1e-4 and the position and velocity normalized RMS (root mean squared whitened residual per scalar component) are both below 2; the diagnostics then say `slow tail accepted`." The sentences before ("Batch Levenberg-Marquardt uses QR, 100 iterations per pass, a relative cost-change threshold of 1e-8, and up to five bias reintegrations.") and after ("Reported factors are reintegrated at the final bias.") stay.
2. The key list: after "`seeds` (per starting heading: biases, convergence, iterations, objective, residual RMS)," insert "`stopping` (the rule that ended the fit: `settled`, `slow tail accepted`, `iteration limit`, `bias not settled` or `cost increased`; the number of passes; the last pass's mean relative cost decrease per iteration; the re-preintegration cost difference; the thresholds in force), `quality` (`imu_nrms`, `position_nrms`, `velocity_nrms`, the normalized RMS of each factor kind's whitened residuals, and `objective_per_state`),". The sentence "When the recording was rejected or the solver failed it is `{"algorithm", "failure"}` with the reason." becomes: "When the recording was rejected it is `{"algorithm", "failure"}` with the reason; when the solver failed it is the same, plus `stopping` and `quality` when the fit completed a pass (`iteration limit`, `bias not settled`), or `stopping` alone when a pass raised the cost (`cost increased`)."
3. Section 5: "The stopping test can treat a no-update step and a zero bias shift as settled. That behaviour is part of the frozen algorithm." becomes "The stopping test can treat a no-update step as settled, and a slow tail is accepted on numerical grounds alone."
4. Section 8: "The algorithm is frozen: for identical inputs this implementation must produce the same objective, biases, residuals, output timestamps and output channels as the reference implementation on the branch `sensor-fusion-clean-port` (revision `83a64479fd4e7e2e10bce0b5477c5dd7a49dee7d`). That is demonstrated by tests, all labelled `fusion`:" becomes "For identical inputs the kernel must reproduce its goldens (objective, biases, residuals, output timestamps and output channels), captured from it by `fusion_golden_capture` at the end of the last phase that changed numerical results. That is demonstrated by tests, all labelled `fusion`:". (Phase 1 left this paragraph to Phase 7; it becomes false in this phase, so this phase replaces the one sentence and Phase 7 still owns the section's rewrite.)

`tests/README.md`, in the Phase 1 wording:

1. Fixtures table, `coarse_maneuver` row, last column: "several LM iterations over three bias passes" becomes "several LM iterations over two bias passes".
2. Tolerance policy, portable mode: "counts (`gnss_states`, `imu_outputs`, `iterations`, the input audit, residual nodes)" becomes "counts and copied tuning values (`gnss_states`, `imu_outputs`, `iterations`, `passes`, the input audit, residual nodes, and the thresholds and window under `stopping`)".

No other documentation changes: `tests/README.md` section 11's re-capture procedure is what Task 3.9 runs; the acceptance map (`tests/acceptance_map.txt`) is Phase 7's (this phase renames no mapped function, and `fitTraceMatchesGolden`, the only kernel test in the map, keeps its name).

**Acceptance Criteria:**
- [ ] `grep -n "bias shifts below\|zero bias shift\|frozen" docs/SENSOR_FUSION.md` prints nothing
- [ ] `grep -n "three bias passes" tests/README.md` prints nothing; `grep -n "passes" tests/README.md` shows the tolerance-policy sentence
- [ ] The key list in `docs/SENSOR_FUSION.md` names `stopping` and `quality` with the five rule names
- [ ] `ctest -C Release -L audit` still passes (the naming rules exclude `tests/README.md`; nothing here adds a forbidden pattern)

**Complexity:** S

---

### Task 3.9: Re-capture the goldens

**Purpose:** Every numerical phase ends by re-capturing the goldens with the Phase 1 recipe, so the suite is green at the phase boundary and the goldens on disk are the last capture.

**Files to modify:**
- `tests/data/fusion/capture.json` — rewritten by the tool
- `tests/data/fusion/coarse_linear.json`, `coarse_maneuver.json`, `stationary_spin.json` and the nine `reject_*.json` — rewritten (the `algorithm` string changes in all twelve; the three successes also gain `stopping` and `quality`)
- `tests/data/fusion/coarse_maneuver.channels.txt` — rewritten with slightly different bits (the fit now stops at the end of pass two; see below)
- `tests/data/fusion/coarse_linear.channels.txt`, `stationary_spin.channels.txt` — rewritten byte-identical (expected not to appear in `git status`)

**Technical Approach:**

Run Tasks 3.1-3.8 first (build green, `tst_fusion_kernel` green except `fitTraceMatchesGolden`, which needs the new goldens). Then the Phase 1 recipe, quoted verbatim from `01-golden-regression-harness.md` (Task 1.6 and "The re-capture recipe (quote this in Phases 3-6)"):

From the repository root, Git Bash, on the capture machine (`build-phase1/`, 64-bit MSVC 19.44, Release):

1. `cmake --build build-phase1 --config Release`
2. `PATH="/c/Qt/6.9.3/msvc2022_64/bin:$PWD/build-solver-deps/GTSAM-install/bin:$PWD/build-solver-deps/oneTBB-install/bin:$PATH" build-phase1/FlySightViewer-build/Release/fusion_golden_capture.exe --revision "$(git rev-parse HEAD)"` — twelve lines, no `** UNEXPECTED **`, `wrote 16 files to .../tests/data/fusion`, exit 0.
3. `cmake build-phase1/FlySightViewer-build` — log says `Fusion exact tests registered for Release`.
4. `ctest --test-dir build-phase1/FlySightViewer-build -C Release -L fusion --output-on-failure` and `ctest --test-dir build-phase1/FlySightViewer-build -C Release -L exact --output-on-failure` — all green.
5. `git status --porcelain -- tests/data/fusion/` and `git diff --stat -- tests/data/fusion/` — list every changed golden file among the phase's files.

An unexpected outcome (exit 2) or a non-deterministic capture (exit 3) is never committed; a red test after the capture means the kernel and the goldens disagree with the tests' literal expectations (a count, a text, a key set), which the phase must resolve in the tests or the kernel, never by editing a golden.

The full recipe with its PATH note and "what changes" list is in `tests/README.md` section 11, "Re-capture procedure" (Phase 1, Task 1.7).

What this phase expects the capture to change (derived from the committed goldens' histories: each pass's first `before` is the cost of the graph rebuilt at the previous pass's bias, so the new cost test can be evaluated on them by hand):

| File | Expected change |
|---|---|
| `capture.json` | date, revision, hashes |
| all twelve `<fixture>.json` | `algorithm` `batch-shared-bias-v1` -> `batch-shared-bias-v2` |
| `coarse_linear.json` | plus `stopping` (`settled`, `passes` 1, difference about 1e-22) and `quality`; `iterations` stays 2; `history`, `progress`, numbers and `channels.txt` unchanged |
| `stationary_spin.json` | `passes` 1 (the pass-one re-preintegration difference is 5.2e-7 <= 1e-6); `iterations` 6 -> 5; `history` loses its pass-two row (which was an exact no-op); `progress` 10 -> 8 texts; `objective` 11.095095993796871, residuals, RMS values and `channels.txt` unchanged bit for bit |
| `coarse_maneuver.json` | `passes` 2 (differences 2.1e-5 after pass one, 3.8e-12 after pass two); `iterations` 7 -> 6; `history` loses its pass-three row; `progress` 12 -> 10 texts; `objective` 2.001640277974819 -> about 2.001640277974833 (the third pass had lowered it by 8e-15); residuals, RMS values and `channels.txt` change in the last digits |
| nine `reject_*.json` | `algorithm` only |

Check with `git status --porcelain -- tests/data/fusion/` (expected: `capture.json`, the twelve `.json`, `coarse_maneuver.channels.txt`; if `coarse_linear.channels.txt` or `stationary_spin.channels.txt` is listed, the fit's values moved where they should not have: investigate before committing) and with `git diff -U0 -- tests/data/fusion/stationary_spin.json | grep -c '^[-+]'` (a small number: the algorithm line, the iterations line, the removed history row, the two removed progress lines, and the added `stopping` / `quality` blocks). Report the list from step 5 in the phase's report; the golden files are committed with the phase (Commit Policy, plan-specific notes).

**Acceptance Criteria:**
- [ ] The tool printed twelve lines, none `** UNEXPECTED **`, exited 0
- [ ] `stopping.passes` in the re-captured goldens is 1, 2, 1 for `coarse_linear`, `coarse_maneuver`, `stationary_spin`, and `seeds[0].iterations` is 2, 6, 5
- [ ] `stationary_spin.channels.txt` and `coarse_linear.channels.txt` are byte-identical to the committed files (not listed by `git status`)
- [ ] `ctest -C Release -L fusion`, `-L exact`, `-L audit` and the whole suite without `-L` pass on `build-phase1/FlySightViewer-build`
- [ ] The phase's report lists every file `git status` shows under `tests/data/fusion/`

**Complexity:** S

---

## Testing Requirements

### Unit Tests
- New in `tst_fusion_kernel`: `biasSettledByCostTest`, `slowTailAtTheIterationLimit` (rows accepted / nrms bound fails / decrease bound fails), `biasNeverSettlesIsSolverFailure`, `failureDiagnosticsShape`. Edited: `validationRejectsEachDefect` (tuning validity), `exactConstantVelocityFit` (stopping and quality of an exact fit), `nonConvergenceIsSolverFailure` (negative tolerance, rule in the reason, four-key failure diagnostics). `fitTraceMatchesGolden` unchanged, green after the re-capture.
- `tst_fusion_golden`: `comparatorHoldsItsBounds` gains the `passes` exact pair; `successFixturesMatchGolden`, `rejectionFixturesMatchGolden`, `progressMatchesGoldenBoundaries` and `fitTraceMatchesGolden` compare against the re-captured goldens (the diagnostics gain keys, the algorithm string changes, two fixtures lose one pass). The cancellation tests are unaffected: their boundaries (`Starting fit`, `Integrating IMU factors`, `Pass 1, iteration 1`, `Pass 1, iteration 2`) precede any pass change.
- `tst_fusion_session`, `tst_fusion_jobs`, `tst_fusion_rows`: unchanged; they compare channels through `goldenDifference()` and read only `objective` and `failure` from the diagnostics. `rejectionIsACachedResult`'s `{algorithm, failure}` assertion still holds (rejections carry no `stopping`).

### Integration Tests
- `ctest --test-dir build-phase1/FlySightViewer-build -C Release -L fusion --output-on-failure` and `-L exact` after the re-capture: every fusion test green in both modes; `-L audit` green; the whole suite without `-L`.
- Configure-time: `GTSAM link confinement: OK` and `Fusion exact tests registered for Release` unchanged (no new target, no new GTSAM-including source).

### Manual Verification
- Task 3.9's `git status` / `git diff -U0` checks: the expected file list and the expected small diff of `stationary_spin.json`.
- Optional, with the Phase 2 runner if it is built: run one of the four ground recordings of `experiments/fusion_lab/NOTES.md` section 5 (`TEMP/data/...`, untracked) and read `stopping.rule` and `quality` in the printed diagnostics; the two "cost stops changing" recordings should now end `settled`, the two slow-tail recordings `slow tail accepted` with position nrms well under 2. This is Michael's corpus check (spec section 12), not an acceptance criterion.

## Notes for Implementer

### Gotchas
- **Do not touch `preintegrateImu()`, `preintegrationParams()` or `imuintegration.cpp`**: Phase 4 owns them and is implemented after this phase. `Tuning`'s density fields are Phase 4's too; this phase adds only the four stopping fields and relaxes `relativeTolerance`'s validity.
- **One rebuild per pass, none extra.** The rebuild after a pass is the next pass's graph and, after the last pass, the reported graph; `graph.error(values)` on it is evaluated once and reused as the objective. Building or evaluating twice changes no number but changes the progress sequence (`Integrating IMU factors` is reported per build), which the goldens and `progressMatchesGoldenBoundaries` pin.
- **GTSAM's LM never accepts an increasing step and leaves the values untouched when it rejects one** (`LevenbergMarquardtOptimizer::tryLambda`: a step is accepted only with `modelFidelity > 1e-3`, so `costChange > 0`; a rejected step returns with the state unchanged). Consequences: `before >= after` on every recorded iteration, a no-op iteration has `before == after` exactly, and `relativeTolerance = 0` or `1e-300` *can* settle. Only a negative tolerance forces the limit; that is why Task 3.1 makes it legal and why the platform caveat in `nonConvergenceIsSolverFailure` goes away.
- **Strict comparisons in the slow tail** (`<`, not `<=`) are what make the two refusal tests deterministic with a zero bound.
- **`catch (const FitFailure &)` before `catch (const std::exception &)`** in `runPipeline()`; the reverse order never reaches the first handler.
- **`QJsonValue` and NaN:** write `QJsonValue::Null` explicitly for a non-finite measurement; do not pass a NaN to `QJsonValue(double)` and hope.
- **Key sorting:** `QJsonObject` sorts keys, so `diagnostics.keys()` for a completed-pass failure is `algorithm, failure, quality, stopping`; write the test literal in that order.
- **`std::string == const char[]`** comparisons (`trace.stopping.rule == StopRule::kSettled`) work; `QCOMPARE(std::string, const char *)` does not, hence `QCOMPARE(rule, std::string(StopRule::kSettled))` or `QVERIFY(rule == StopRule::kSettled)`.
- **`nonConvergenceIsSolverFailure` and the golden fixture generator** must not change `fusionfixtures.cpp`: `capture.json` records its hash, and the Phase 1 provenance depends on the fixtures being the same bits.
- **Line endings and the goldens:** as in Phase 1, never open a golden in an editor between the capture and the check; `core.autocrlf=true` makes `git diff` normalize, `cmp` does not.
- **`fitAndAssemble()` no longer throws for non-convergence**; it returns the failure result. Anything that still throws inside the fit stage (reconstruction, output) is caught by the generic handler as today and produces `{algorithm, failure}` without `stopping`.
- **Cancellation between passes** is observed at the rebuild's `Integrating IMU factors` checkpoints, exactly as today; the extra `graph.error()` evaluation is not a boundary and needs none (it is one pass over the factors, far cheaper than a build).
- **The `git` commands** the implementer runs are read-only (`status`, `diff`, `rev-parse`); the orchestrator stages and commits, including the re-captured goldens by explicit path.

### Decisions Made
- **A fifth rule name, `bias not settled`.** The overview's four names do not cover a fifth pass that settles while the cost test fails; today that is "not converged", and the spec keeps the pass structure, so it stays a failure with its own name in `stopping.rule` and in the reason text. The slow tail is never judged on such a pass (it settled); the two failures are disjoint.
- **The slow tail is judged only on the last pass run, i.e. the fifth**, because a pass that hits the limit while passes remain is followed by another pass, as today. A pass with fewer iterations than the window is not judged (`n >= window` is part of the acceptance), so `maxIterations = 1` still fails deterministically.
- **The cost test is evaluated after every pass, settled or not** (the rebuild happens anyway), so `repreintegration_cost_difference` is a defined number for every fit that completed a pass and is `null` only for `cost increased`. For a settled pass it is the bias-settled test; for a pass at the limit it says how far the reported objective is from the pass's final cost.
- **Reason texts:** `Batch fusion did not converge (iteration limit); sensor fusion unavailable` and `Batch fusion did not converge (bias not settled); sensor fusion unavailable`; the cost-increase text `Nonfinite or increasing optimizer cost` is unchanged because the overview's Fit contract quotes it for Phase 5's initializer.
- **`FitFailure` (a `std::runtime_error` carrying `Stopping`)** is how `cost increased` reaches the diagnostics without text matching and without breaking Phase 5's "catch the routine's failure and count an infinite objective". It is thrown by `runOptimizerPass()` itself, which knows the pass and the history.
- **Failure-diagnostics shape:** `{algorithm, failure, quality, stopping}` for `iteration limit` and `bias not settled` (the nrms values explain a refused slow tail), `{algorithm, failure, stopping}` for `cost increased`, `{algorithm, failure}` for rejections and for any other fit-stage exception.
- **`Stopping` carries the thresholds**, copied from `Tuning`, so `successDiagnostics()` keeps its signature and the output stage needs no `Tuning`; Phase 5 changes that signature anyway, but not for this reason.
- **Negative tuning values are legal "never" forcings** (`relativeTolerance`, `biasSettledTolerance`, the two slow-tail bounds): finite is the validity rule; production uses the positive defaults. This replaces the fragile `1e-300` forcing and its platform caveat.
- **No forced test for `cost increased`.** LM rejects increasing steps by construction, so the guard is reachable only through non-finite arithmetic, which needs contrived inputs whose path through the initializer and GTSAM's linear solver is not deterministic to specify. The writer's shape is proven directly (`failureDiagnosticsShape`); the one-line mapping in `runOptimizerPass()` is reviewed by inspection.
- **Fixtures:** none added. The spec's two tests are stated on `coarse_maneuver` (production tuning for the cost test; forced tuning for the slow tail) and `coarse_linear` (bias never settles). `stationary_spin`'s one-pass convergence is pinned by its re-captured golden rather than by a kernel assertion, because its re-preintegration difference (5.2e-7) sits within a factor of two of the threshold.
- **The trace JSON (`fusiontrace.h`) is unchanged**; the stop rule is in `PipelineTrace` for the kernel tests and in the diagnostics for the goldens. Changing the golden `trace` object would be churn Phase 5 repeats.
- **`docs/SENSOR_FUSION.md` section 8's "frozen" sentence** is replaced now (it is falsified by this phase), although Phase 1 had left the paragraph to Phase 7; Phase 7 still rewrites the section.
- **`isExactKey()` additions** treat copied thresholds as exact, matching the function's stated rule ("copies of an input").

### Open Questions
- None that block implementation. One note for the orchestrator: the expected golden diff in Task 3.9 (which `channels.txt` files change) is derived from the committed histories by hand; if the capture lists `stationary_spin.channels.txt` as changed, the values moved where the analysis says they should not have, and the implementer should report the first differing sample from `successFixturesMatchGolden` before the goldens are committed.

## Definition of Done

This phase is complete when:
1. All tasks have passing acceptance criteria, in particular the kernel assertions of Task 3.7 and the expected golden diff of Task 3.9
2. All tests pass: `ctest -C Release` on `build-phase1/FlySightViewer-build` without `-L`, and with `-L fusion`, `-L exact`, `-L audit`
3. Code follows the patterns of `factorgraphfit.cpp` (one hand-driven LM pass per checkpoint, rebuild at the fitted bias, factor-order walk in `collectResiduals()`), `fusionoutput.cpp` (explicit `QJsonValue::Null`, one writer per object) and `fusion.cpp` (classification by stage, catching only in `runPipeline()`)
4. `src/fusion/imuintegration.h` / `.cpp` are untouched; `tests/fusion/fusionfixtures.cpp` / `.h` are untouched; no file outside `src/fusion/` includes a GTSAM header that did not before
5. No TODOs or placeholder code remains; the phase's report lists every file created, modified and re-captured (`src/fusion/factorgraphfit.h`, `factorgraphfit.cpp`, `fusionsamples.h`, `fusionsamples.cpp`, `fusion.cpp`, `fusionpipeline.h`, `fusionoutput.h`, `fusionoutput.cpp`; `tests/fusion/fusiongolden.cpp`, `tests/tst_fusion_golden.cpp`, `tests/tst_fusion_kernel.cpp`; `docs/SENSOR_FUSION.md`, `tests/README.md`; and every file `git status` shows under `tests/data/fusion/`)
