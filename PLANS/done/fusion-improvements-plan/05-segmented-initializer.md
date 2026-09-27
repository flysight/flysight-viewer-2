# Phase 5: Segmented initializer

## Overview

This phase replaces the stationary-window initializer and the coarse fallback by the segmented initializer of spec section 3. The fitted window is cut into consecutive 600 s segments (a final piece shorter than 120 s merged into the one before it); in each segment the fix with the smallest sAcc is the anchor, the coarse attitude there (measured force aligned with GNSS acceleration minus gravity, gyro bias zero) is carried by the gyro to the first fix of a prefix window of 60 s centred on the anchor, that window is fitted from four heading offsets with the production graph and tuning, the marginal yaw sigma of its first pose is computed from the best start's graph rebuilt at the fitted bias, and the window doubles until the sigma is below 20 degrees or the window covers the segment; the whole segment is then fitted once from the best prefix fit's attitude and bias, and the full fit starts from every segment's fitted attitudes, the first segment's gyro bias and a zero accelerometer bias. Prefix and segment fits are ordinary `fitFactorGraph()` fits under Phase 3's stopping rule with Phase 4's graph; each of their iterations is a cancellation boundary with the segment texts of the overview; a fit that throws `FitFailure` is a start with infinite objective; a segment in which every start fails at full length falls back to the coarse attitude propagated with zero bias over that segment only, and the diagnostics say so.

The stationary-window detector, its gates and its silent polling are deleted. `InitialAttitude` gives way to an initial-state type (one `Rot3` per fix, one gyro bias), `fitFactorGraph()` takes the initial state and a progress-text format, `FitResult` exposes the reported graph, a `yawSigmaDeg()` helper lives next to the fit, the diagnostics gain the `initializer` object, `PipelineTrace` and the golden `trace` carry the segment account, four synthetic recordings of spec section 10 become fixtures with expectations stated in the kernel tests, and the phase ends with the Phase 1 re-capture (every success golden changes; the nine rejections do not).

## Dependencies

- **Depends on:** Phase 3 (`FitResult` with `Stopping` / `Quality`, `FitFailure` as the fit's one failure exception with `what()` `Nonfinite or increasing optimizer cost`, the rebuild at the fitted bias as the reported graph, negative tuning values as legal "never" forcings), Phase 4 (`Tuning::gyroStepSlope` / `accStepSlope` in the production graph, `model.per_step` in the diagnostics, the `Tuning` routed into `successDiagnostics()`), Phase 1 (`fusion_golden_capture`, `tst_fusion_golden`, `tests/fusion/fusiontrace.h`, the re-capture recipe quoted in Task 5.11).
- **Blocks:** Phase 6 (temperature-dependent gyro bias). Phase 6 consumes `InitialState` (per-fix `Rot3`, one constant gyro bias; the accelerometer bias always starts at zero) unchanged and starts `b1` at zero; it adds `IMU/temperature` to `Channels` / `Samples` and a custom IMU factor for the full fit only. Two things Phase 6 must know: (1) the initializer's prefix and segment fits call the same `fitFactorGraph()` on sub-windows cut by `fittedWindow()`, so Phase 6 must select the stock `ImuFactor` for those calls (a flag on the call, or sub-windows without temperature: Phase 6 decides); (2) the `drifting_bias` fixture of Task 5.6 has no temperature channel; Phase 6 adds one (a linear ramp) and the `b1` assertion.
- **Assumptions:**
  - The working tree is the plan branch with Phases 1, 3 and 4 committed. `src/fusion/factorgraphfit.cpp` has Phase 3's pass loop (`runOptimizerPass()` throwing `FitFailure`, one rebuild per pass, `rebuilt` as the reported graph, `collectResiduals()` filling `quality`), `Tuning` has Phase 3's four stopping fields and Phase 4's two slopes, `successDiagnostics()` receives the `Tuning`, the goldens are Phase 4's capture and `algorithm` is `batch-shared-bias-v2`.
  - Line numbers below for `src/fusion/initializer.*`, `imuintegration.*`, `fusionsamples.*`, `fusion.cpp`, `fusionpipeline.h`, `fusionprogress.h`, `fusionoutput.*`, `tests/tst_fusion_kernel.cpp` and `tests/fusion/fusionfixtures.cpp` are those of `master` at `1f3139a`; Phases 3 and 4 shifted some of them, and the named functions are identified by content. `tests/tst_fusion_golden.cpp` is `tests/tst_fusion_parity.cpp` under its Phase 1 name; its tests are named by their Phase 1 names (`progressMatchesGoldenBoundaries`, `cancelAtEachKindOfBoundary`, `cancelDuringPreparation`).
  - GTSAM on this machine (`build-solver-deps/GTSAM-install/include/gtsam/config.h`) defines `GTSAM_POSE3_EXPMAP` and `GTSAM_ROT3_EXPMAP` and not `GTSAM_USE_QUATERNIONS`: `Pose3::retract(xi)` is `compose(ChartAtOrigin::Retract(xi))` (`third-party/gtsam/gtsam/base/Lie.h` lines 131-133), which is `compose(Expmap(xi))` under `GTSAM_POSE3_EXPMAP`: a right perturbation, so the tangent vector is expressed in the body frame, with the rotation block first (`Pose3::rotationInterval()` is `{0, 2}`, `translationInterval()` `{3, 5}`, `Pose3.h` lines 369-380). `gtsam::Marginals(graph, values, Marginals::QR)` and `marginalCovariance(Key)` are declared in `third-party/gtsam/gtsam/nonlinear/Marginals.h` (lines 37-40, 59, 114); a rank-deficient system throws `gtsam::IndeterminantLinearSystemException` (`gtsam/linear/linearExceptions.h` line 94, a `ThreadsafeException`, hence a `std::exception`).
  - The verification build is `build-phase1/` (overview, "Verification commands"); `build/` is never rebuilt. The fusion tests run under a 600 s timeout each; every fixture below keeps its fits to seconds.

## Tasks

### Task 5.1: Tuning fields, and the detector retired

**Purpose:** The two lengths the spec marks as tuning become `Tuning` fields with the spec's defaults so a test can shorten them; the stationary-window detector, its gates and the silent cancellation poll go, because nothing decides on them any more.

**Files to modify:**
- `src/fusion/fusionsamples.h` — two fields on `Tuning`; the struct comment
- `src/fusion/fusionsamples.cpp` — `requireValidTuning()`
- `src/fusion/fusionprogress.h` — delete `Checkpoint::pollCancel()` (lines 38-45) and the sentence about it in the class comment (lines 20-21); reword the comment so it says: calling it reports `text`, then asks whether to stop, in that order; nothing is reported or polled before the first reported boundary ("Starting fit")
- `src/CMakeLists.txt` — remove `fusion/stationarywindow.cpp fusion/stationarywindow.h` (line 339) from `flysight_fusion`'s source list; `samplestatistics.*` stays (used by `fusionsamples.cpp`)

**Files to delete:**
- `src/fusion/stationarywindow.h`, `src/fusion/stationarywindow.cpp` — in full (plain file operations; the orchestrator stages the removal by path)

**Technical Approach:**

In `struct Tuning` (`fusionsamples.h` lines 34-40, after Phase 3's and Phase 4's fields), add:

```cpp
double segmentLength = 600;      ///< the initializer cuts the fitted window into segments this long, s
double minFinalSegment = 120;    ///< a final piece shorter than this is merged into the segment before it, s
int maxPasses = 5;               ///< re-preintegration passes of one fit: the full fit's and a segment fit's five; a prefix fit's one
```

`maxPasses` replaces the constant `kMaxBiasPasses` (delete it): the pass loop of `fitFactorGraph()` runs `for (int outer = 0; outer < c.maxPasses; ++outer)`, and `requireValidTuning()` refuses `maxPasses < 1` in the same clause as `maxIterations < 1` (`Invalid iteration limit`). `Tuning{}` behaviour is unchanged (five passes).

with a short comment naming the spec (section 3.2 step 1) and stating that a test may shorten both to keep a multi-segment recording small. Add both to the strictly-positive loop of `requireValidTuning()` (`fusionsamples.cpp` lines 52-61; Phase 3 split it into a positive list and a finite list: these two join the positive list, same text `Invalid fusion configuration`). The 60 s prefix, the 20 degree sigma and the four heading offsets are specification values and stay constants in `initializer.cpp` (Task 5.3).

`pollCancel()`'s only caller is `bestStationaryWindow()` (`initializer.cpp` line 53), which Task 5.3 deletes; `grep -rn pollCancel src tests` must then print nothing. `stationarywindow.h` is included by `initializer.cpp` (line 12) and `tests/tst_fusion_kernel.cpp` (line 32) only; both includes go (Tasks 5.3 and 5.7). No other file names the detector (`grep -rn "stationarywindow\|assessStationaryWindow\|imuGapLimit\|StationaryWindow" src tests cmake` prints nothing after this phase).

**Acceptance Criteria:**
- [ ] `Tuning{}` has `segmentLength == 600` and `minFinalSegment == 120`; `validateSamples()` throws `std::invalid_argument` for `segmentLength = 0` and for `minFinalSegment = -1` (asserted in `validationRejectsEachDefect`, Task 5.7)
- [ ] `src/fusion/stationarywindow.h` and `.cpp` no longer exist; `src/CMakeLists.txt` does not name them; the configure still prints `GTSAM link confinement: OK`
- [ ] `Checkpoint` has no `pollCancel()`; `grep -rn "pollCancel\|StationaryWindow\|stationarywindow" src tests` prints nothing
- [ ] `src/fusion/samplestatistics.h` / `.cpp` are untouched

**Complexity:** S

---

### Task 5.2: The initial state, the fit entry point, the reported graph and the yaw sigma

**Purpose:** The fit takes what the initializer produces (one rotation per fix and one gyro bias) instead of an anchor attitude and a heading offset, reports its iterations under a caller-chosen text, exposes the graph its objective was evaluated on, and a helper next to it computes the marginal yaw sigma of a pose from that graph.

**Files to modify:**
- `src/fusion/initializer.h` — `InitialAttitude` (lines 19-28) replaced by `InitialState`; `initialValues()` (line 46) re-declared; the file comment (lines 12-15) rewritten (Task 5.3 adds the rest of the header)
- `src/fusion/initializer.cpp` — `initialValues()` (lines 132-153) rewritten; `kStartTimeTolerance` (line 35) deleted
- `src/fusion/factorgraphfit.h` — `FitResult` (drop `heading`, add `graph`), `kFullFitPassFormat`, the new `fitFactorGraph()` signature, `yawSigmaDeg()`
- `src/fusion/factorgraphfit.cpp` — `runOptimizerPass()` takes the format; `fitFactorGraph()` takes the initial state; `yawSigmaDeg()` defined

**Technical Approach:**

*`InitialState`* (`initializer.h`, replacing `InitialAttitude`):

```cpp
/// Where the optimizer starts: one attitude per fix of the window it is for
/// (body to NED), and the gyro bias. The accelerometer bias always starts
/// at zero; positions and velocities are the GNSS measurements.
struct InitialState {
    std::vector<gtsam::Rot3> rotations;
    gtsam::Vector3 gyroBias = gtsam::Vector3::Zero();   ///< rad/s
};
```

`initialValues(const Samples &window, const InitialState &initial)`: throws `std::invalid_argument("Initial state must have one attitude per fix and a finite bias")` unless `initial.rotations.size() == window.gnssTime.size()` and `initial.gyroBias.allFinite()`; inserts `B(0)` as `ConstantBias(Vector3::Zero(), initial.gyroBias)`, and per fix `X(k) = Pose3(initial.rotations[k], window.position[k])`, `V(k) = window.velocity[k]`, in that order (the insertion order is today's). No propagation happens here any more: the rotations arrive ready (Task 5.3 owns the carrying). The heading composition of lines 137-139 goes with `headingDeg`.

*`FitResult`* (`factorgraphfit.h`): delete `double heading` (nothing selects a heading; Task 5.5 writes `null` where the diagnostics reported it). Add, after `residuals` and Phase 3's `stopping` / `quality`:

```cpp
gtsam::NonlinearFactorGraph graph;   ///< the graph the objective, residuals and quality were evaluated on:
                                     ///< rebuilt at the fitted bias; the initializer takes the marginal yaw sigma from it
```

In `fitFactorGraph()` this is Phase 3's `rebuilt` after the loop (`result.graph = std::move(rebuilt)` once `collectResiduals()` has run). Nothing else reads it and it is never serialized.

*The pass-text format.* Declare in `factorgraphfit.h`, namespace scope:

```cpp
/// The full fit's iteration text; the initializer's fits pass the segment texts.
inline constexpr char kFullFitPassFormat[] = "Pass %1, iteration %2";
```

`runOptimizerPass()` gains a `const QString &passFormat` parameter and reports `passFormat.arg(outer+1).arg(i+1)` instead of `QStringLiteral("Pass %1, iteration %2").arg(outer+1).arg(i+1)`. The contract, stated in the header comment: `passFormat` holds exactly two unreplaced `QString::arg` placeholders, the lower-numbered one for the one-based pass, the other for the one-based iteration; `QString::arg` replaces the lowest-numbered remaining placeholder, so `QStringLiteral("Segment %1 of %2: prefix %3 s, pass %4, iteration %5").arg(1).arg(3).arg(60.)` leaves `%4` and `%5` for the pass loop and yields `Segment 1 of 3: prefix 60 s, pass 2, iteration 7`. The full fit passes `kFullFitPassFormat`, so its texts are byte-identical to today's. `buildFactorGraph()` is untouched and still reports `Integrating IMU factors` every 256 states of every build, in the initializer's fits as in the full fit (Decisions Made).

*The entry point:*

```cpp
FitResult fitFactorGraph(const Samples &samples, const InitialState &initial, const Tuning &tuning,
                         const QString &passFormat = QString::fromLatin1(kFullFitPassFormat),
                         const Checkpoint &checkpoint = Checkpoint());
```

Body as Phase 3 left it, with `values = initialValues(d, initial)` at the top and `passFormat` handed to every `runOptimizerPass()` call; the pass structure, the cost test, the slow tail, `FitFailure` and the one rebuild per pass are unchanged, except that the loop bound is `c.maxPasses` (Task 5.1) instead of the deleted constant. Rewrite the header comment (lines 52-55 on `master`, as Phase 3 reworded it): the fit starts from `initial`; the reported objective, residuals, quality and `graph` are those of the rebuild at the fitted bias.

*`yawSigmaDeg()`* (`factorgraphfit.h` / `.cpp`; includes `<gtsam/nonlinear/Marginals.h>` and `<gtsam/linear/linearExceptions.h>` in the `.cpp`):

```cpp
/// The marginal standard deviation, in degrees, of the rotation of pose `key`
/// about the navigation vertical, from `graph` linearized at `values`
/// (QR factorization). Capped at 180 degrees: a rotation about the vertical is
/// an angle on a circle, and an indeterminate system or a non-finite
/// covariance means the yaw is simply undetermined, which is what 180 says.
double yawSigmaDeg(const gtsam::NonlinearFactorGraph &graph, const gtsam::Values &values, gtsam::Key key);
```

Implementation, the lab's (`experiments/fusion_lab/fusion_lab.cpp` lines 214-222, algorithm only): `gtsam::Marginals marginals(graph, values, gtsam::Marginals::QR); const gtsam::Matrix cov = marginals.marginalCovariance(key);` the rotation block is `cov.block<3, 3>(0, 0)` (rotation first in the `Pose3` tangent, Assumptions) and is expressed in the body frame (right perturbation); with `R = values.at<gtsam::Pose3>(key).rotation().matrix()` the navigation-frame covariance is `R * block * R.transpose()` and the sigma is `std::sqrt(covW(2, 2)) * 180 / kPi` (the D axis is the vertical). Return `std::min(sigma, kYawSigmaCapDeg)` with `kYawSigmaCapDeg = 180`; return the cap when `covW(2, 2)` is not finite or negative, and when the constructor or `marginalCovariance()` throws `gtsam::IndeterminantLinearSystemException` (catch that type only; everything else propagates). The helper is a pure function of its arguments; it lives next to the fit because it needs the graph the fit built.

**Acceptance Criteria:**
- [ ] `InitialAttitude` no longer exists anywhere under `src/` or `tests/` (`grep -rn InitialAttitude src tests` prints nothing after Task 5.7); `InitialState` is declared in `initializer.h` with exactly `rotations` and `gyroBias`
- [ ] `initialValues(window, initial)` throws `std::invalid_argument` for a `rotations` vector of the wrong size and for a NaN bias (asserted in Task 5.7) and inserts `B(0)`, then `X(k)`, `V(k)` per fix
- [ ] `fitFactorGraph()` has the signature above; `FitResult` has `graph` and no `heading`; the full fit's texts are exactly `Pass %1, iteration %2` and `Integrating IMU factors` (Task 5.9's golden `progress` for the full-fit part, and `tst_fusion_session::cancelStopsAtNextBoundary`'s `Starting fit` first text)
- [ ] `yawSigmaDeg()` returns exactly `180` on a graph with an exactly unobservable yaw and a value below 20 on one with horizontal acceleration, and its value is invariant to a yaw rotation of the linearization point (Task 5.7's `yawSigmaIsMarginalAboutTheVertical`)
- [ ] `factorgraphfit.h` gains no GTSAM include beyond `<gtsam/inference/Key.h>` if `Key` is not already visible through `Values.h`; `Marginals.h` and `linearExceptions.h` are included in the `.cpp` only

**Complexity:** M

---

### Task 5.3: The segmented initializer

**Purpose:** Spec section 3.2 and 3.3 as one routine: cut, anchor, coarse attitude, prefix growth on the marginal yaw sigma with four heading starts, one segment fit, the stitched initial state, the first segment's bias, the fallback, and the account of all of it.

**Files to modify:**
- `src/fusion/initializer.h` — the account types, `Initialization`, `segmentBounds()`, `coarseAttitude()`, `attitudesCarriedForward()`, `initialize()`; `rotationAligning()` stays; `initialAttitude()` (lines 40-41) deleted
- `src/fusion/initializer.cpp` — delete `bestStationaryWindow()` (41-63), `attitudeFromStationaryWindow()` (69-85), `initialAttitude()` (123-130), the window constants (24-28), the `stationarywindow.h` include (12); generalize `coarseAttitude()` (90-100); add the rest

**Technical Approach:**

*Types* (`initializer.h`, after `InitialState`; `<limits>` for the NaN defaults):

```cpp
/// What the initializer did in one segment. Times are seconds since the epoch.
struct SegmentAccount {
    int index = 0;                             ///< zero-based
    size_t firstFix = 0, lastFix = 0;          ///< inclusive indices into the fitted window
    double start = 0, end = 0;                 ///< the segment's first and last fix
    double anchorTime = 0, anchorSacc = 0;     ///< the smallest-sAcc fix and its sAcc, m/s
    double prefixLength = 0;                   ///< the nominal prefix length reached, s (60, 120, ...)
    double prefixStart = 0, prefixEnd = 0;     ///< the last prefix window's first and last fix
    std::vector<double> prefixYawSigmaDeg;     ///< the chosen start's yaw sigma per length tried; NaN when every start failed
    double yawSigmaDeg = NaN;                  ///< the last entry; NaN when the segment fell back with no prefix fit
    int prefixFits = 0;                        ///< prefix fits attempted, all lengths and starts, failed ones included
    int prefixIterations = 0;                  ///< the chosen prefix fit's iterations (0 without one)
    int prefixPasses = 0;                      ///< its passes (1 by construction; 0 without one)
    bool prefixOnLimit = false;                ///< the chosen prefix fit ended on its budget (used anyway)
    bool segmentOnLimit = false;               ///< the segment fit ended on the iteration limit (used anyway)
    std::string growthStop;                    ///< why the prefix stopped growing: observable, covers, no_gain, all_failed
    double imuNrms = NaN;                      ///< the segment fit's IMU normalized RMS (NaN for a fallback)
    bool restarted = false, restartKept = false; ///< spec 3.2 step 3: refitted from the neighbour; the refit replaced the fit
    std::string restartReason;                 ///< "", "fallback", "nrms", "no neighbour"
    double restartNrms = NaN;                  ///< the restart's IMU normalized RMS
    int iterations = 0;                        ///< the segment fit's; 0 for a fallback
    bool converged = false;                    ///< the segment fit's; false for a fallback
    bool fallback = false;                     ///< the segment's attitudes are its start state, not a fit
    gtsam::Rot3 prefixRotation;                ///< the best prefix fit's attitude at prefixStart (identity without one)
    gtsam::Vector3 prefixGyroBias = Zero;      ///< its fitted gyro bias (zero without one)
    gtsam::Rot3 startRotation;                 ///< the segment fit's start attitude at `start`
    gtsam::Vector3 startGyroBias = Zero;       ///< the segment fit's start gyro bias
    gtsam::Vector3 gyroBias = Zero;            ///< the segment's fitted gyro bias (startGyroBias for a fallback)
};

struct InitializerAccount {
    double segmentLength = 0;                  ///< Tuning::segmentLength
    std::vector<SegmentAccount> segments;
};

/// The initializer's result: the full fit's start and the account of how it was chosen.
struct Initialization {
    InitialState state;
    InitializerAccount account;
};
```

*Functions* (declared in the header so the kernel tests reach them):

- `std::vector<std::pair<size_t, size_t>> segmentBounds(const std::vector<double> &gnssTime, double segmentLength, double minFinalSegment)`: inclusive index pairs. Boundaries at `t0 + k * segmentLength`, `t0 = gnssTime.front()`; fix `i` belongs to the piece whose half-open interval `[t0 + k L, t0 + (k+1) L)` contains `gnssTime[i]` (walk the fixes once, advancing `k` while `gnssTime[i] >= t0 + (k+1) L`; no `floor` of a quotient). A window shorter than one segment yields one piece. The final piece is merged into the piece before it when there is one and either its duration (last fix minus first fix of the piece) is below `minFinalSegment` or it holds fewer than three fixes (the fitted window's validity rule: a segment must have at least three fixes; the first piece always has them at any GNSS rate the outage rule admits, the last may not). Every fix is in exactly one piece; the pieces are consecutive.
- `gtsam::Rot3 coarseAttitude(const Samples &d, size_t k)`: today's rule (lines 90-100) at an arbitrary fix: the GNSS acceleration is the forward difference `(velocity[k+1] - velocity[k]) / (gnssTime[k+1] - gnssTime[k])` when `k+1 < gnssTime.size()`, else the backward difference over `(k-1, k)` (a segment has at least three fixes, so one of the two exists); the attitude is `rotationAligning(interpolateAt(d.imuTime, d.force, d.gnssTime[k]), a - kGravity)`. Gyro bias zero is implied: the function returns only the rotation.
- `std::vector<gtsam::Rot3> attitudesCarriedForward(const Samples &d, const gtsam::Rot3 &first, const gtsam::Vector3 &gyroBias)`: one rotation per fix; element 0 is `first`, element `k` is element `k-1` composed with the gyro increments of `integrationEdges(d, gnssTime[k-1], gnssTime[k])` at `gyroBias` (today's loop, `initializer.cpp` lines 143-148, moved out of `initialValues()`).
- `Initialization initialize(const Samples &window, const Tuning &tuning, const Checkpoint &checkpoint = Checkpoint())`.

*`initialize()`*, in prose. `window` is `plan.window` (already validated; `fittedWindow()` sub-windows of it inherit the validity rules and keep one IMU sample before and after their fixes, so consecutive segments share IMU samples at their seams, which is harmless: fixes are what a segment owns). Constants in the anonymous namespace: `kPrefixLength = 60`, `kYawSigmaLimitDeg = 20`, `kHeadingOffsetsDeg = {0, 90, 180, 270}`, `kSameObjectiveRelative = 1e-9`, `kPrefixIterations = 50`, `kPrefixPasses = 1`, `kGrowthMinGain = .2`. `prefixTuning` is a copy of `tuning` with `maxIterations = kPrefixIterations` and `maxPasses = kPrefixPasses` (spec 3.3, Budgets: a prefix fit is a start, not an answer, one pass of at most 50 iterations); segment fits use `tuning` itself. The stopping rule of Phase 3, slow-tail acceptance included, applies to prefix and segment fits through the shared `fitFactorGraph()`; a prefix or segment fit that ends on its iteration limit is still used as a start (its values are the best available) and the account records it.

1. `bounds = segmentBounds(window.gnssTime, tuning.segmentLength, tuning.minFinalSegment)`; `count = bounds.size()`. `account.segmentLength = tuning.segmentLength`. `state.rotations` is sized to `window.gnssTime.size()`.
2. For each segment `s` (index `i`, fixes `[a, b]`), `segment = fittedWindow(window, window.gnssTime[a], window.gnssTime[b])` (its fix `j` is window fix `a + j`; assert the count is `b - a + 1`):
   a. Anchor: the segment fix with the smallest `velocitySigma[j].x()` (sAcc is stored three times; take `.x()`), the earliest on a tie (`std::min_element` with `<` does this). Record `anchorTime`, `anchorSacc`. `coarse = coarseAttitude(segment, anchor)`.
   b. Prefix loop, `L = kPrefixLength`:
      - The window: fixes of the segment with `gnssTime` in `[max(segment.start, anchorTime - L/2), min(segment.end, anchorTime + L/2)]`, found by `lower_bound` / `upper_bound` on `segment.gnssTime` (indices `p..q`). `covers = (p == 0 && q == last)`. If the window holds fewer than three fixes it is not fitted: double `L` and repeat (nothing is counted; this only happens at a GNSS rate far below 1 Hz).
      - `prefix = fittedWindow(segment, segment.gnssTime[p], segment.gnssTime[q])`. Start attitude at the window's first fix: `first = propagateAttitude(prefix, coarse, anchorTime, prefix.gnssTime.front(), Vector3::Zero())` (backwards, zero bias, per spec 3.2 b; `propagateAttitude()` returns its input unchanged when the two times are equal).
      - Four starts: for each offset `h` in `kHeadingOffsetsDeg`, `InitialState init{attitudesCarriedForward(prefix, Rot3::Rz(h * kPi / 180).compose(first), Vector3::Zero()), Vector3::Zero()}`; `fitFactorGraph(prefix, init, prefixTuning, QStringLiteral("Segment %1 of %2: prefix %3 s, pass %4, iteration %5").arg(i+1).arg(count).arg(L), checkpoint)` inside `try { ... } catch (const FitFailure &) { /* a start with infinite objective */ }`. Never catch anything else: `FusionCancelled` is not a `std::exception` and must propagate; any other `std::exception` is a defect and propagates to `runPipeline()` (SolverFailed). `prefixFits` is incremented per attempted start, failed ones included.
      - The best start: the first start that completed is the best; a later start replaces it only when its objective is lower by more than `kSameObjectiveRelative * max(1, the current best's objective)` (Decisions Made: rounding-equal objectives are equal, the earliest wins; without this rule an exactly yaw-unobservable recording picks its yaw by rounding noise, which differs between compilers and would break the portable goldens).
      - If no start completed: push NaN to `prefixYawSigmaDeg`; if `covers`, `growthStop = "all_failed"` and the segment falls back (step d); otherwise double `L` and repeat (spec 3.3: all four failing is "yaw not observable").
      - Otherwise `sigma = yawSigmaDeg(best.graph, best.values, X(0))` (the prefix's first pose; `best.graph` is the rebuild at the fitted bias, `best.values` the solution); push it. Record `prefixLength = L`, `prefixStart`, `prefixEnd`, `prefixRotation = best.values.at<Pose3>(X(0)).rotation()`, `prefixGyroBias = best.values.at<ConstantBias>(B(0)).gyroscope()`. Record `prefixIterations = int(best.history.size())`, `prefixPasses = best.stopping.passes` and `prefixOnLimit = (best.stopping.rule == "iteration limit")`. Then decide (spec 3.2 d): if `sigma <= kYawSigmaLimitDeg`, `growthStop = "observable"` and leave the loop; else if `covers`, `growthStop = "covers"` and leave the loop; else if `prefixYawSigmaDeg` has a finite entry `prev` before this one and `sigma > (1 - kGrowthMinGain) * prev`, `growthStop = "no_gain"` and leave the loop (a doubling that cuts the sigma by less than 20 % means the segment has no motion to find, and fitting more of it will not help); else double `L` and repeat.
   c. The segment fit (always run when a prefix fit completed, even when the prefix covers the segment: one path, and the segment's bias is then the bias of a fit that started at its answer). Start: `segmentFirst = propagateAttitude(segment, prefixRotation, prefixStart, segment.gnssTime.front(), prefixGyroBias)` (backwards, the prefix fit's bias), `init{attitudesCarriedForward(segment, segmentFirst, prefixGyroBias), prefixGyroBias}` (forwards to the segment's last fix with that bias, passing through the prefix's first fix and beyond). Record `startRotation = segmentFirst`, `startGyroBias = prefixGyroBias`. `fitFactorGraph(segment, init, tuning, QStringLiteral("Segment %1 of %2: pass %3, iteration %4").arg(i+1).arg(count), checkpoint)` in the same `try` / `catch (const FitFailure &)`. On success: `state.rotations[a + j] = fit.values.at<Pose3>(X(j)).rotation()` for every `j`; `gyroBias = fitted`; `iterations = fit.history.size()`; `converged = fit.converged`; `fallback = false`. On `FitFailure`: the segment falls back (step d) with the start it had. Record `segmentOnLimit = (the segment fit's `stopping.rule == "iteration limit"`)`: a segment fit that ends on the limit is still used (spec 3.3, Budgets), so an account with `converged == false` and `fallback == false` is valid and means exactly that.
   d. Fallback: the segment's attitudes are its start state. When no prefix fit completed, that start is the coarse attitude at the anchor carried to the segment's first fix by `propagateAttitude(segment, coarse, anchorTime, segment.gnssTime.front(), Vector3::Zero())` and forward over the segment with zero bias (spec 3.3's "today's fallback, over that segment only"), `startGyroBias = 0`, `yawSigmaDeg` NaN, `prefixLength = L`, `prefixStart` / `prefixEnd` the last window tried; when the segment fit threw after a completed prefix, the start is the one recorded in step c. In both cases `state.rotations[a..b] = the start's rotations`, `gyroBias = startGyroBias`, `iterations = 0`, `converged = false`, `fallback = true`.
3. Segment check (spec 3.2 step 3), when `count >= 2`. `nrms_i` is the segment fit's IMU normalized RMS (`quality.imu_nrms` of the segment's `FitResult`; NaN for a fallback). `median` is the median of the finite `nrms_i`. A segment is suspect when it fell back or `nrms_i > 2 * median` (`kRestartNrmsFactor = 2`). For each suspect segment, in index order, one restart: the neighbour is the previous segment when `i > 0`, else the next; a neighbour that is itself suspect or already restarted is skipped (then no restart; `restartReason` records `"no neighbour"`). Start attitude: for the previous neighbour, `propagateAttitude(window, neighbour.rotations.back(), neighbour.end, segment.start, neighbour.gyroBias)` (the neighbour's fitted attitude at its last fix carried forward across the seam; the two segments share the IMU samples at the seam); for the next neighbour, its first-fix attitude carried backwards. `init{attitudesCarriedForward(segment, that attitude, neighbour.gyroBias), neighbour.gyroBias}`; `fitFactorGraph(segment, init, tuning, QStringLiteral("Segment %1 of %2: restart, pass %3, iteration %4")...)` inside the same `try` as the segment fit. Keep the restart when the segment was a fallback and the restart completed, or when the restart's `imu_nrms` is lower than `nrms_i`; then `state.rotations[a..b]`, `gyroBias`, `iterations`, `converged`, `segmentOnLimit` and `fallback = false` come from the restart. Record `restarted = true`, `restartKept`, `restartReason` (`"fallback"` or `"nrms"`), and `restartNrms` (the restart's `imu_nrms`, NaN when it threw). One round only.
3. `state.gyroBias = account.segments.front().gyroBias` (the first segment's, fitted or fallback). The accelerometer bias is zero by construction of `initialValues()`.

Rules the routine must keep, by inspection: no static state; the only checkpoint calls are those inside `fitFactorGraph()` / `buildFactorGraph()` (the initializer itself reports nothing of its own); `FusionCancelled` crosses the initializer untouched; "agreement of the fitted yaw between starts" is never consulted (`NOTES.md` section 4.6 is why); no code path other than steps b-d chooses an attitude.

**Acceptance Criteria:**
- [ ] `initializer.cpp` contains no `StationaryWindow`, `assessStationaryWindow`, `imuGapLimit`, `kWindowLength`, `kWindowGrid` or `pollCancel`; `initialAttitude()` is gone; `rotationAligning()` is unchanged
- [ ] `segmentBounds()` on 1 Hz times `0..200` with `(60, 12)` yields `{0,59}, {60,119}, {120,179}, {180,200}`; on `0..190` yields three pieces ending `{120,190}`; on `0..30` one piece; on `0..60` one piece `{0,60}` (the final piece has one fix); with `(600, 120)` on `0..200` one piece (Task 5.7)
- [ ] `coarseAttitude(d, last fix)` uses the backward difference and does not throw; `coarseAttitude(d, 0)` reproduces today's `coarseAttitude()` rotation bit for bit on `coarse_maneuver`'s window (the forward difference and the same `rotationAligning()` call)
- [ ] The prefix texts are exactly `Segment %1 of %2: prefix %3 s, pass %4, iteration %5` and the segment texts `Segment %1 of %2: pass %3, iteration %4` with one-based segment index, the segment count after merging, and `L` printed as `QString::arg(double)` prints it (`60`, `120`, ...); every graph build reports `Integrating IMU factors`
- [ ] A `FitFailure` from any prefix or segment fit is caught inside `initialize()`; a `FusionCancelled` from inside a prefix fit and from inside a segment fit propagates (Task 5.9)
- [ ] On the four fixtures of Task 5.6 and the three golden fixtures the account satisfies Task 5.8's expectations; `yawSigmaDeg` is never consulted from more than one start per length
- [ ] `initialize()` on `coarse_maneuver`'s window with the throwing checkpoint of Task 5.8 falls back with `startRotation == coarseAttitude(window, 0)` bit for bit

**Complexity:** L

---

### Task 5.4: The pipeline runs the initializer inside the fit stage

**Purpose:** The initializer now runs solver fits, so it belongs to stage 2: its cancellations are the fit's, anything it throws that is not a caught `FitFailure` is `SolverFailed`, and preparation neither reports nor polls.

**Files to modify:**
- `src/fusion/fusion.cpp` — `kInitialHeadingDeg` (line 22) deleted; `FitPlan` (30-35) loses `attitude`; `planFit()` (42-58) loses the initializer call and its comment is rewritten; `fitAndAssemble()` (61-81) runs the initializer; `runPipeline()` (95-127) no longer copies `plan.attitude`; the `initializer.h` include stays
- `src/fusion/fusionpipeline.h` — `PipelineTrace::attitude` replaced by `InitializerAccount initializer`; the comments on `PipelineTrace` and `runPipeline()`

**Technical Approach:**

`planFit()`: keep the checks, the window and the tuning (lines 44-53); delete lines 55-56. Its comment: stage 1 is a few single passes over the recording, reports nothing and asks nothing; the first boundary of a run is `Starting fit`.

`fitAndAssemble()`:

```
checkpoint(QStringLiteral("Starting fit"));
const Initialization init = initialize(plan.window, plan.tuning, checkpoint);
if (trace) trace->initializer = init.account;          // before the full fit: a cancelled or failed full fit keeps the account
const FitResult fit = fitFactorGraph(plan.window, init.state, plan.tuning, QString::fromLatin1(kFullFitPassFormat), checkpoint);
... Phase 3's trace filling, non-convergence result, reconstruction, channels ...
result.diagnosticsJson = toCompactJson(successDiagnostics(plan.prepared, init.account, fit, plan.window, dense, plan.tuning));
```

`runPipeline()`: delete `if (trace) trace->attitude = plan.attitude;`. The handlers are Phase 3's: `FusionCancelled` from inside a prefix or segment fit reaches the second `try` and returns `Result()`; a `FitFailure` that escapes the initializer can only be the full fit's (`cost increased`, as Phase 3 shaped it); a `std::exception` from the initializer (a `propagateAttitude()` IMU-gap throw cannot happen on a validated window, a GTSAM exception from inside a solve can) is `SolverFailed` with `{algorithm, failure}`, unchanged from Phase 3's "anything else in the fit stage".

`fusionpipeline.h`: `struct PipelineTrace { InitializerAccount initializer; std::vector<FitIteration> history; bool converged = false; Stopping stopping; };` with the comment: `initializer` is filled once the initializer returned, before the full fit; `history`, `converged` and `stopping` are the full fit's (the segment fits' histories are not traced; their iteration counts are in the account). The `runPipeline()` comment adds one sentence: the initializer's prefix and segment fits are part of the fit stage.

**Acceptance Criteria:**
- [ ] `fusion.cpp` contains no `kInitialHeadingDeg`, no `initialAttitude` and no `plan.attitude`; `planFit()` calls nothing that takes a `Checkpoint`
- [ ] A run whose cancel function answers "yes" at the first call ends `Cancelled` with exactly one reported text, `Starting fit`, on every success fixture (Task 5.9's `cancelDuringPreparation`)
- [ ] `PipelineTrace::initializer` is filled for every run that started the full fit, including one the full fit ended as `SolverFailed` (Task 5.8's fallback row reads it) and one cancelled inside the full fit
- [ ] `git diff -- src/fusion/fusion.cpp` shows no change to the classification handlers of `runPipeline()` beyond the deleted `trace->attitude` line

**Complexity:** S

---

### Task 5.5: Diagnostics: the `initializer` object and the retired keys

**Purpose:** The overview's binding layout for `initializer`; the legacy keys keep their names with the meaning the overview assigns (`initialization` names the segmented method, `stationary_interval_s` and `anchor_time_s` are `null`); nothing selects a heading, so the heading keys are `null` too.

**Files to modify:**
- `src/fusion/fusionoutput.h` — `successDiagnostics()` takes `const InitializerAccount &` instead of `const InitialAttitude &`
- `src/fusion/fusionoutput.cpp` — `seedSummary()` (27-39), `successDiagnostics()` (84-110), one new writer

**Technical Approach:**

- `kInitializationMethod[] = "segmented initialization; heading from segment fits"` next to `kAlgorithm` (`kAlgorithm` stays `batch-shared-bias-v2`; the next change is Phase 6's).
- `QJsonObject initializerObject(const InitializerAccount &a)` in the anonymous namespace, one segment object per entry with exactly the keys `index`, `start_s`, `end_s`, `anchor_s`, `anchor_sacc_m_s`, `prefix_length_s`, `yaw_sigma_deg` (`QJsonValue::Null` when not finite, the explicit pattern of Phase 3's `stoppingObject()`), `prefix_fits`, `prefix_iterations`, `prefix_passes`, `prefix_on_limit`, `segment_on_limit`, `growth_stop` (string), `imu_nrms` (`Null` when not finite), `restarted`, `restart_kept`, `restart_reason` (string), `iterations`, `fallback`; the object is `{"segment_length_s": a.segmentLength, "segments": [...], "fallback_segments": [indices with fallback true, ascending]}`. No other key (the layout is binding; the C++ account's extra fields stay in the trace, Task 5.6).
- `seedSummary()`: `{"heading_deg", QJsonValue::Null}` in place of `fit.heading`; the other seven entries unchanged.
- `successDiagnostics()`: `{"initialization", kInitializationMethod}`, `{"stationary_interval_s", QJsonValue::Null}`, `{"anchor_time_s", QJsonValue::Null}`, `{"selected_heading_deg", QJsonValue::Null}`, and `{"initializer", initializerObject(account)}` added; every other entry (including Phase 3's `stopping` / `quality` and Phase 4's `model`) unchanged. `failureDiagnostics()` is untouched: the failure shapes stay Phase 3's (a fallback is visible in the trace and, for a successful run, in the diagnostics).

**Acceptance Criteria:**
- [ ] A successful fit's diagnostics has the keys it had after Phase 4 plus `initializer`; `initializer.keys()` is `{fallback_segments, segment_length_s, segments}` (sorted) and every segment's keys are `{anchor_s, anchor_sacc_m_s, end_s, fallback, growth_stop, imu_nrms, index, iterations, prefix_fits, prefix_iterations, prefix_length_s, prefix_on_limit, prefix_passes, restart_kept, restart_reason, restarted, segment_on_limit, start_s, yaw_sigma_deg}` (Task 5.7's `initializerDiagnosticsShape`)
- [ ] `initialization` is `segmented initialization; heading from segment fits`; `stationary_interval_s`, `anchor_time_s`, `selected_heading_deg` and `seeds[0].heading_deg` are JSON `null`
- [ ] `segment_length_s` equals the `Tuning` the run used (600 by default, 60 under Task 5.8's override); `yaw_sigma_deg` is `null` exactly for a segment whose `fallback` is true with `prefix_fits` all failed
- [ ] Rejections still produce exactly `{algorithm, failure}`; the nine `reject_*.json` are byte-identical after the re-capture

**Complexity:** S

---

### Task 5.6: Test support: the trace writer, the exact keys, and the four synthetic recordings

**Purpose:** The golden `trace` object carries the segment account; the comparator treats the account's counts, times and copied lengths as exact; spec section 10's recordings exist as fixtures that obey the bit-reproducibility rules and are not captured as goldens.

**Files to modify:**
- `tests/fusion/fusiontrace.h` — `traceJson()` rewritten (Phase 1 wrote it as the one authority for the golden `trace`)
- `tests/fusion/fusiongolden.cpp` — `isExactKey()` (lines 80-88, plus Phase 3's five keys)
- `tests/fusion/fusionfixtures.h` / `.cpp` — one new accessor and four generators

**Technical Approach:**

*`traceJson()`* returns `{"initializer": {"segment_length_s", "segments": [...]}, "converged", "history"}` with `history` rows `[outer, iteration, before, after]` exactly as today and, per segment, the keys `index`, `start_s`, `end_s`, `anchor_s`, `anchor_sacc_m_s`, `prefix_length_s`, `prefix_start_s`, `prefix_end_s`, `yaw_sigma_deg` (`Null` when not finite), `prefix_fits`, `prefix_iterations`, `prefix_passes`, `prefix_on_limit`, `segment_on_limit`, `growth_stop`, `imu_nrms`, `restarted`, `restart_kept`, `restart_reason`, `restart_nrms`, `iterations`, `converged`, `fallback`, `start_quaternion_xyzw` (of `startRotation`, the `toQuaternion()` xyzw order of today's `start_quaternion_xyzw`), `start_gyro_bias_rad_s`, `gyro_bias_rad_s`. The per-length sigmas are not written (they are for the kernel tests through `PipelineTrace`). Keep the header GTSAM-include-free (`Rot3` through `fusion/fusionpipeline.h`) and the one-paragraph authority comment.

*`isExactKey()`*: add `index`, `prefix_fits`, `segment_length_s`, `prefix_length_s`, `prefix_start_s`, `prefix_end_s`, `start_s`, `end_s`, `anchor_s`, `anchor_sacc_m_s`, `fallback_segments`, `prefix_iterations`, `prefix_passes`, `prefix_on_limit`, `segment_on_limit`, `growth_stop`, `restarted`, `restart_kept`, `restart_reason`. They are counts, flags, strings, epoch-relative copies of fixture times (one exact-rounded subtraction of two fixture doubles, the same bits on every IEEE platform) or copied tuning values. Note the top-level `start_s` / `end_s` of the diagnostics become exact by the same rule; they are the same kind of copy. `yaw_sigma_deg` is solver arithmetic under the degree floor (`portableFloor()` sees the `_deg` suffix); the quaternions and biases are under the absolute floor.

*Fixtures.* `fusionFixtures()` stays the twelve golden fixtures (the capture tool and `tst_fusion_golden` iterate it; the overview keeps goldens to those). Add to the header:

```cpp
/// The synthetic recordings of the initializer's tests: motion_start,
/// rest_throughout, sacc_anchor, drifting_bias. Not golden fixtures: their
/// expected values are stated in tst_fusion_kernel, and fusion_golden_capture
/// does not see them. Same bit-reproducibility rules as the rest of this file.
FusionFixture initializerFixture(const QString &name);
```

Every generator follows the file's rules (lines 5-15): `+ - * /` only, every sample from its index, noise from one `NoiseSource` in the documented order (all GNSS samples first, each drawing north, east, down, velN, velE, velD; then all IMU samples, each drawing ax, ay, az, wx, wy, wz), no noise on accuracies or times, times `kEpochUtc + t`, `originIndex = 0`, `expectSuccess = true`. Rotation constants are exact rationals (`.6 / .8` and `.96 / .28`, Pythagorean), so no transcendental function appears. Specific force in the body frame is `R^T (a - g) + b_a` with `g = (0, 0, 9.80665)` in NED, exactly as `coarse_maneuver`'s comment states for the identity; the gyro reads its bias only (the attitude is constant in every recording).

1. `motion_start` (spec: "starts in motion"). 90 s. GNSS 5 Hz, `t = j * .2`, `j = 0..449`. IMU 25 Hz, `t = i * .04`, `i = 0..2250`. Attitude `R = Rz(psi)` with `cos psi = .6`, `sin psi = .8` (53.13 deg). NED motion: `vN = 20`, `pN = 20 t`; `aE = 2` for `40 <= t < 50` else `0`, `vE = 0` / `2 (t - 40)` / `20`, `pE = 0` / `(t - 40)^2` / `100 + 20 (t - 50)` on the three pieces; down zero. Body force `(.8 aE + .05, .6 aE - .03, -9.80665 + .08)`; gyro `(.2, -.15, .3)` deg/s. `hAcc = 1.5`, `vAcc = 2.5`, `sAcc = .3`. Noise: force `.005`, gyro `.02` deg/s, position `.2`, velocity `.03`; seed `0x8F050004`. 450 states; the fits are 151-, 301- and 450-state graphs at 5 IMU samples per interval: seconds in total.
2. `rest_throughout`. 300 s at rest (longer than two prefix doublings, so the growth stop of spec 3.2 d is exercised). IMU 25 Hz `i = 0..7500`; GNSS `t = .1 + j * .2`, `j = 0..1499`; position and velocity zero. Attitude `R = Ry(theta)`, `cos theta = .96`, `sin theta = .28`, as the matrix `[[.96, 0, .28], [0, 1, 0], [-.28, 0, .96]]`; body force `(.28 * 9.80665 + .03, -.02, -.96 * 9.80665 + .05)`; gyro `(.2, -.1, .15)` deg/s. `hAcc = 1`, `vAcc = 1.5`, `sAcc = .1`. Noise as `stationary_spin` (`.005, .02, .2, .03`); seed `0x8F050005`. 1500 states.
3. `sacc_anchor`. 300 s. GNSS 1 Hz, `t = j`, `j = 0..300`; IMU 10 Hz, `t = i * .1`, `i = 0..3000`. Attitude identity. `vN = 15`, `pN = 15 t`; `aE = 3` for `190 <= t < 200` else `0`, `vE = 0` / `3 (t - 190)` / `30`, `pE = 0` / `1.5 (t - 190)^2` / `150 + 30 (t - 200)`. Body force `(.05, aE - .03, -9.80665 + .08)`; gyro `(.2, -.15, .3)` deg/s. `hAcc = 1.5`, `vAcc = 2.5`; `sAcc = 2` for every fix except `j == 200`, where it is `.3`. Noise `.005, .02, .2, .03`; seed `0x8F050006`. 301 states; the prefix is 61.
4. `drifting_bias`. 200 s. GNSS 1 Hz, `j = 0..200`; IMU 10 Hz, `i = 0..2000`. Attitude identity. `vN = 15`, `pN = 15 t`. A manoeuvre in every 30 s block: with `k = j / 30` (GNSS) or `k = i / 300` (IMU) as integer division and `s = t - 30 k`: `aE = 2` for `10 <= s < 15`, `-2` for `15 <= s < 20`, else `0`; `vE = 0` / `2 (s - 10)` / `10 - 2 (s - 15)` / `0`; `pE = 50 k + (0 / (s - 10)^2 / 25 + 10 (s - 15) - (s - 15)^2 / 50)` on the four pieces (continuous: 25 at `s = 15`, 50 at `s = 20`). Body force `(.05, aE - .03, -9.80665 + .08)`. Gyro `(.2, -.15, .3 + t / 200)` deg/s: the z bias drifts linearly by exactly 1 deg/s over the length; the attitude does not rotate. `hAcc = 1.5`, `vAcc = 2.5`, `sAcc = .3`. Noise `.005, .02, .2, .03`; seed `0x8F050007`. 201 states; under Task 5.8's `segmentLength = 60`, `minFinalSegment = 12` it has four segments of 60, 60, 60 and 21 fixes, each with its manoeuvre in the first 30 s.

Document each generator with the same kind of comment the three success fixtures have (the numbers above, and what the recording is for). `capture.json`'s `fixture_generator_sha256_lf` changes with this file: expected in Task 5.11.

**Acceptance Criteria:**
- [ ] `traceJson()` writes exactly the keys listed; `tst_fusion_kernel::fitTraceMatchesGolden` passes against the re-captured goldens for all three success fixtures
- [ ] `isExactKey()` returns true for the eleven new keys; `comparatorHoldsItsBounds` gains the pair `jsonPasses("prefix_fits", 8, 8)` / `!jsonPasses("prefix_fits", 8 + 1e-9, 8)` next to Phase 3's `passes` pair
- [ ] `initializerFixture()` returns the four recordings by name and a default-constructed fixture for any other name; `fusionFixtures()` still returns twelve; `fusion_golden_capture` still prints twelve lines and writes 16 files
- [ ] `tests/fusion/fusionfixtures.cpp` contains no call to `std::sin`, `std::cos`, `std::sqrt`, `std::pow` or `<random>`; every new sample value is computed from its index
- [ ] Two calls of `initializerFixture(name)` are bit-identical (Task 5.7's `initializerFixturesAreDeterministic`)

**Complexity:** M

---

### Task 5.7: Kernel tests: deletions, re-specifications and the initializer's units

**Purpose:** The detector's tests go with it; the tests that used the old initializer are re-specified; the cutting rule, the yaw sigma, the progress texts and the diagnostics shape are held to expectations stated independently of the implementation.

**Files to modify:**
- `tests/tst_fusion_kernel.cpp` — header comment (lines 1-8), includes (32), helpers (`quietSamples` 69-86, `translatingSamples` 88-97, `hasGate` 120-123: delete), slots (`stationaryGates` 328-394, `stationaryScanPollsSilently` 395-456: delete), re-specified and new slots

**Technical Approach:**

Header comment: "every stationary gate" becomes the segmented initializer (segment cutting, the anchor, prefix growth on the marginal yaw sigma, the fallback when every start fails, its progress texts); keep "exact integration boundaries, heading freedom, the solver-failure path", Phase 3's "the two stopping rules forced through the tuning" and Phase 4's covariance tests; "the fit trace ... initializer first" becomes "the segment account first". Drop the `fusion/stationarywindow.h` include. `linearSamples()` and `boundarySamples()` stay.

1. **`validationRejectsEachDefect`**: add `Tuning t; t.segmentLength = 0;` throws; `t = Tuning{}; t.minFinalSegment = -1;` throws; and `initialValues(d, InitialState{})` (empty rotations on the two-fix `boundarySamples`) throws `std::invalid_argument`, as does a two-rotation state with a NaN `gyroBias`.
2. **`shortWindowIsOneSegment`** (replaces `shortInputUsesCoarseInitializer`): `linearSamples(Vector3(12, -4, 2), Vector3(7, 8, 9))` (2 s, 9 fixes), `initialize(linear, Tuning{})`: one segment with `index 0`, `firstFix 0`, `lastFix 8`, `start == linear.gnssTime.front()`, `end == linear.gnssTime.back()`, `anchorTime == start` (every sAcc `.1`: the earliest wins), `anchorSacc == .1`, `prefixLength == 60` (the first window covers 2 s), `prefixStart == start`, `prefixEnd == end`, `prefixFits == 4`, `prefixYawSigmaDeg.size() == 1`, `!fallback`, `iterations > 0`; `state.rotations.size() == 9`; `state.gyroBias == segments[0].gyroBias`. Exact constant velocity has no yaw information: `yawSigmaDeg == 180` (the cap; Task 5.2) — assert `yawSigmaDeg >= 20` rather than the exact cap if the implementer finds QR returns a finite garbage value on this data, and `qInfo()` it.
3. **`segmentsAreCutOnFixes`**: `segmentBounds()` on hand-built time vectors, the five cases of Task 5.3's acceptance criteria, plus: a 0.2 Hz recording `0, 5, 10, ..., 200` with `(60, 12)` gives `{0,11}, {12,23}, {24,35}, {36,40}` (the final piece `180..200` is 20 s, five fixes: kept), and with `(60, 30)` the final piece is merged (`{24,40}`).
4. **`yawSigmaIsMarginalAboutTheVertical`**: `Samples d = boundarySamples(Vector3::Zero())` with `velocitySigma = Vectors(2, Vector3::Constant(.1))`, graph `buildFactorGraph(d, ConstantBias(), Tuning{})`, values as in `headingIsUnconstrained` (identity poses, zero velocities): `yawSigmaDeg(graph, values, X(0)) == 180` exactly (no horizontal acceleration: the yaw column is zero and the system is indeterminate or its covariance is not finite; either way the cap). Then `boundarySamples(Vector3(1, -2, .5))` with the same sigmas and the values `V(0) = 0`, `V(1) = acceleration * (.863 - .037)`, `X(1)` translated by `.5 * acceleration * duration^2` (the trajectory `preintegrationHonoursExactBoundaries` verifies), identity rotations: the sigma is finite, positive and below 20 (the horizontal acceleration of 2.2 m/s^2 over .83 s against a velocity sigma of .1 m/s determines the yaw to a few degrees). Then the same graph with every pose's rotation replaced by `Rot3::Rz(.8)` and the translations, velocities and IMU data left as they are: the two sigmas agree within `1e-6` relative (a yaw rotation of the linearization point rotates the body-frame block and the nav-frame rotation undoes it; a helper that forgot the rotation into the navigation frame would still pass this for a level pose, which is why the implementer's `R cov R^T` line is also an inspection item of Task 5.2).
5. **`initializerProgressTexts`**: `initialize()` on `coarse_maneuver`'s window (`prepareInput(toChannels(fusionFixture("coarse_maneuver")))`, then `fittedWindow(recording, usableStart, gnssTime.back())`, then `validateSamples` with a `Tuning` whose `maxGap` is `kImuGapMedians * medianInterval(imuTime)`; wrap this in a file-local `windowOf(name, tuning)` helper, Task 5.8 uses it too) with a collecting `Checkpoint`: every text is `Integrating IMU factors` or matches `^Segment 1 of 1: (prefix 60 s, )?pass [0-9]+, iteration [0-9]+$` (a `QRegularExpression`); the first non-build text is `Segment 1 of 1: prefix 60 s, pass 1, iteration 1`; a text `Segment 1 of 1: pass 1, iteration 1` occurs, and after it no `prefix` text; no text starts with `Pass `. On `motion_start` (Task 5.6) the texts contain `prefix 60 s` and `prefix 120 s` and no other length.
6. **`initializerDiagnosticsShape`**: `runPipeline(toChannels(fusionFixture("coarse_maneuver")), Tuning{}, Checkpoint(), &trace)`, parse the diagnostics: the key sets of Task 5.5's first criterion; `initialization`, the four `null`s; `initializer.segment_length_s == 600`; `segments.size() == 1`; `segments[0]`: `index == 0`, `start_s == diagnostics.start_s`, `end_s == diagnostics.end_s`, `anchor_s == start_s`, `anchor_sacc_m_s == .3`, `prefix_length_s == 60`, `prefix_fits == 4`, `fallback == false`, `iterations == trace.initializer.segments[0].iterations`, `fallback_segments` empty; and the trace object: `traceJson(trace)["initializer"]["segments"][0]` has the sixteen keys of Task 5.6.
7. **`exactConstantVelocityFit`**: replace `initialAttitude(...)` and `fitFactorGraph(linear, 0, Tuning{}, init)` by `const Initialization init = initialize(linear, Tuning{}); const FitResult fitted = fitFactorGraph(linear, init.state, Tuning{});`; keep every assertion (Phase 3's included: `converged`, `objective < 1e-12`, `stopping.rule == kSettled`, the dense checks). The yaw is arbitrary and unasserted; the position, velocity and acceleration checks hold for any yaw.
8. **`initializerFixturesAreDeterministic`**: for each of the four names, two calls of `initializerFixture()` compare bit-identical on every array (the pattern of `tst_fusion_golden::fixturesAreDeterministic`).
9. **`fitTraceMatchesGolden`**: unchanged in code; green after Task 5.11.
10. Phase 3's and Phase 4's slots are unchanged in code. Their expectations still hold because the trace `history` and `stopping` are the full fit's: `nonConvergenceIsSolverFailure` (five one-iteration passes of the full fit, `history.size() == 5`), `slowTailAtTheIterationLimit` (the full fit's 125 iterations; the prefix and segment fits also run 125 each under the forced tuning, on 28 states: under a second), `biasSettledByCostTest` (`passes <= 2`: the full fit now starts at the segment fit's solution and may settle in one pass), `biasNeverSettlesIsSolverFailure`, `diagnosticsReportPerStepConstants`. If a literal of Phase 3's tests flips on the new start (only `passes <= 2` and the `history.size()` literals are candidates), the remedy is in the test and is reported.

Register the slots in the class declaration in the order above; `shortWindowIsOneSegment` takes `shortInputUsesCoarseInitializer`'s place.

**Acceptance Criteria:**
- [ ] `tst_fusion_kernel.cpp` contains no `stationaryGates`, `stationaryScanPollsSilently`, `shortInputUsesCoarseInitializer`, `quietSamples`, `translatingSamples`, `hasGate`, `assessStationaryWindow`, `InitialAttitude` or `initialAttitude`
- [ ] The slots `shortWindowIsOneSegment`, `segmentsAreCutOnFixes`, `yawSigmaIsMarginalAboutTheVertical`, `initializerProgressTexts`, `initializerDiagnosticsShape`, `initializerFixturesAreDeterministic` exist and pass; `validationRejectsEachDefect` and `exactConstantVelocityFit` are edited as stated
- [ ] No test compares solver arithmetic against a literal except through `withinPortableBound()`, `sameRecomputedValue()` or an inequality with a stated margin
- [ ] `ctest -C Release -R "tst_fusion_kernel(_exact)?$" --output-on-failure` passes after Task 5.11; `tst_fusion_kernel` alone finishes in under 120 s on the capture machine

**Complexity:** L

---

### Task 5.8: Kernel tests: the five recordings of spec section 10

**Purpose:** The spec's synthetic expectations for the initializer, each on the fixture designed for it, with values stated before the code runs; plus the two fallback rows.

**Files to modify:**
- `tests/tst_fusion_kernel.cpp` — five new slots (one data-driven)

**Technical Approach:**

Every fit goes through `runPipeline(toChannels(initializerFixture(name)), tuning, checkpoint, &trace)`; the diagnostics through `QJsonDocument::fromJson(...)`, `initializer = diagnostics["initializer"]`, `segment = segments[i]`. Truth attitudes are built in the test from the exact matrices of Task 5.6 (`Rot3(matrix)`), and roll / pitch / yaw truths are `truth.rpy() * 180 / kPi`; angle differences are taken modulo 360 with `std::remainder(a - b, 360.)`.

1. **`startsInMotionGrowsToTheManoeuvre`** (`motion_start`, `Tuning{}`). Expected from the fixture: the anchor is the first fix (every sAcc `.3`), so the 60 s window is `[0, 30]` and contains no horizontal acceleration (yaw unobservable, sigma above 20 or the cap), and the 120 s window `[0, 60]` contains the manoeuvre at 40-50 s (yaw determined to a few degrees). Assert: `Succeeded`; `trace.converged`; one segment; `prefix_length_s == 120`; `prefix_fits == 8`; `trace.initializer.segments[0].prefixYawSigmaDeg` has two entries, the first `> 20` (or the cap), the second `< 20`; `yaw_sigma_deg < 20`; `fallback == false`; `prefixStart == 0`, `prefixEnd == 60` (`sameRecomputedValue` is not needed: fix times are exact); and the full fit's attitude against the truth `Rz(53.13...)`: for every output sample, `|remainder(result.yaw[i] - truthYaw, 360)| < 2`, `|result.roll[i]| < 2`, `|result.pitch[i]| < 2` (the unwrapped channels; the fixture never turns).
2. **`atRestPrefixStopsGrowing`** (`rest_throughout`, `Tuning{}`). Expected: the anchor is the first fix (every sAcc `.1`), the 60 s window `[0.1, 30.1]` (clipped at the segment start) has no yaw information, and neither has the 120 s window `[0.1, 60.1]`: its sigma is not 20 % below the first, so growth stops there (`no_gain`) although the 300 s segment is far from covered. Assert: `Succeeded`; `trace.converged`; one segment; `prefix_length_s == 120`; `prefix_fits == 8`; `growth_stop == "no_gain"`; `trace.initializer.segments[0].prefixYawSigmaDeg` has two entries, both `> 20`, the second `> .8 *` the first; `prefixStart == .1`, `prefixEnd == 60.1`; `prefix_passes == 1`; `prefix_iterations <= 50`; `fallback == false`; roll and pitch of every output sample within `.5` degrees of the truth's `rpy()` (`Ry` with `sin .28`: pitch about 16.26 degrees, roll 0); `yaw_sigma_deg` and `prefix_on_limit` are logged with `qInfo()`, not asserted (arbitrary yaw is the spec's accepted outcome; a resting prefix may or may not settle within its budget).
3. **`smallestSaccFixIsTheAnchor`** (`sacc_anchor`, `Tuning{}`). Assert: one segment; `anchor_s == 200`; `anchor_sacc_m_s == .3`; `prefixStart == 170` and `prefixEnd == 230` (the window of length 60 centred on 200, unclipped); the manoeuvre at 190-200 is inside, so `prefix_length_s == 60` and `prefix_fits == 4`; `yaw_sigma_deg < 20`; and the spec's third clause through the account: with `window = windowOf("sacc_anchor", Tuning{})` (Task 5.7's helper), `expected = propagateAttitude(window, s.prefixRotation, s.prefixStart, s.start, s.prefixGyroBias)` and `Rot3::Logmap(s.startRotation.between(expected)).norm() < 1e-9` (the initializer performs this very call; bit identity is likely, `1e-9` is the stated margin). Also `Succeeded`.
4. **`driftingBiasSegmentsConverge`** (`drifting_bias`; `Tuning t; t.segmentLength = 60; t.minFinalSegment = 12;`). Assert: `segment_length_s == 60`; four segments with `(start_s, end_s)` `(0, 59)`, `(60, 119)`, `(120, 179)`, `(180, 200)` and `index` 0..3; for every segment `converged == true` in the account, `fallback == false`, `iterations > 0`, `prefix_length_s == 60` (each anchor is its segment's first fix and the manoeuvre lies in the first 20 s of every 30 s block, inside the 30 s half-window), `prefix_fits == 4`, `yaw_sigma_deg < 20`; `fallback_segments` empty; `seeds[0].gyro_bias_rad_s` finite and `trace.history.front().before` finite (the full fit started from the stitched state; which segment's bias it took is `initialize()`'s contract, asserted in `shortWindowIsOneSegment` through `state.gyroBias`); the full fit returns `Succeeded` (see Gotchas: a constant-bias fit against a 1 deg/s drift is the spec's section 6 evidence case and converges). Phase 6 adds the `b1` assertion on this fixture.
5. **`allPrefixFitsFailFallsBack`** (data-driven). The forcing (Decisions Made): a `Checkpoint` whose progress function throws `FitFailure(std::string("Nonfinite or increasing optimizer cost"), Stopping{})` whenever the text contains `: prefix ` — i.e. at the first iteration boundary of every prefix fit — and a cancel function that never answers yes. The exception leaves `runOptimizerPass()` at its checkpoint, the initializer counts the start as failed, every start fails at every length, the window grows to the segment and the segment falls back; no segment fit runs (nothing to start it from), and the full fit's texts (`Pass ...`) do not trigger the throw. Rows:
   - `"coarse_maneuver"` (6 s: the first window covers): `Succeeded` (the fallback start is the coarse attitude at the first fix carried with zero bias: exactly the start the kernel used before this phase, which this fixture's history proves converges); `segments[0].fallback == true`; `fallback_segments == [0]`; `prefix_fits == 4`; `prefix_length_s == 60`; `yaw_sigma_deg` is `null`; `iterations == 0`; `trace.initializer.segments[0].startRotation.matrix() == coarseAttitude(windowOf("coarse_maneuver", Tuning{}), 0).matrix()` (bitwise: `propagateAttitude` with equal times returns its input); `trace.history` non-empty; `stopping.rule == "settled"`.
   - `"motion_start"` (90 s): the 60 s window does not cover, so the all-fail rule grows it: `prefix_fits == 8`, `prefix_length_s == 120`, `fallback == true`, `trace.initializer.segments[0].prefixYawSigmaDeg` is two NaNs; the full fit ran (`trace.history` non-empty, `trace.stopping.passes >= 1`); its outcome is logged, not asserted (a 90 s fit from a zero-yaw coarse start is not the spec's claim).
   The lambda needs `#include "fusion/factorgraphfit.h"` (already included) and `Stopping{}`; the kernel does not promise anything about a throwing progress function, which is why this forcing lives only in the kernel test.
6. **`startsOnTheLimitAreStillUsed`** (`coarse_maneuver`; `Tuning t; t.maxIterations = 1;`, the forcing `nonConvergenceIsSolverFailure` already uses). Every prefix fit ends after its one iteration and the segment fit after its one iteration per pass; nothing throws. Assert through the trace and the diagnostics: `segments[0].prefix_on_limit == true`, `prefix_iterations == 1`, `prefix_passes == 1`, `segment_on_limit == true`, the account's `converged == false` with `fallback == false` and `iterations >= 1`, `growth_stop == "covers"` (6 s: the first window covers); `state.rotations` has one entry per fix and `trace.initializer.segments[0].startRotation` equals `propagateAttitude(segment, prefixRotation, prefixStart, start, prefixGyroBias)` to `1e-9` (the start was used, not the fallback); the full fit ran from it and ends `SolverFailed` with `stopping.rule == "iteration limit"` (Phase 3's semantics, unchanged). Then, with `Tuning{}` on the same fixture: `prefix_on_limit == false`, `segment_on_limit == false`, `prefix_passes == 1`, `prefix_iterations <= 50`, and `prefix_iterations` equals the value in the re-captured golden (an exact key).
   Every initializer test above also asserts `prefix_passes == 1` and `prefix_iterations <= 50` on each segment it inspects (the budget is a property of every prefix fit).
7. **`suspectSegmentIsRestartedFromItsNeighbour`** (`drifting_bias`; `Tuning t; t.segmentLength = 60; t.minFinalSegment = 12;` as in test 4). Forcing: the progress-throwing `Checkpoint` of test 5, but only for texts starting with `Segment 2 of 4: prefix ` (the second segment's prefix fits fail at their first boundary; nothing else is touched). Assert: `segments[1].fallback == true` before the check is visible only in the trace's `restartReason == "fallback"`; in the diagnostics `segments[1].restarted == true`, `restart_kept == true`, `restart_reason == "fallback"`, `fallback == false`, `iterations > 0`, `converged == true` (the restart from segment 1's boundary attitude converges in a few iterations), `imu_nrms` finite and below `2 *` the median of the other three; every other segment `restarted == false`; `fallback_segments` empty; the full fit `Succeeded` and, as in test 4, within the truth bounds. Also the `nrms` branch through a unit test on the check alone: `restartDecisionUsesTwiceTheMedian` builds four `SegmentAccount`s with `imuNrms` `{.3, .35, .8, .4}` (median of the finite four `.375`, so `.8 > .75`) and asserts the third is the only suspect, then `{.3, .35, .7, .4}` and asserts none (the decision helper is a pure function of the accounts; expose it in the header like `segmentBounds()`).

**Acceptance Criteria:**
- [ ] The five slots exist with the names above and every assertion listed holds on the capture machine
- [ ] `startsInMotionGrowsToTheManoeuvre` asserts the two-entry sigma sequence (first above 20, second below) and the 2-degree attitude bound on every output sample; `atRestPrefixStopsGrowing` asserts `prefix_length_s == 120` and the 0.5-degree roll / pitch bound; `smallestSaccFixIsTheAnchor` asserts `anchor_s == 200`, the `170..230` window and the carried-back start; `driftingBiasSegmentsConverge` asserts four converged segment fits under `segmentLength = 60`; `allPrefixFitsFailFallsBack` asserts the fallback diagnostics on `coarse_maneuver` and the growth count on `motion_start`
- [ ] `atRestPrefixStopsGrowing` (growth stops on `no_gain` at 120 s of a 300 s resting recording) and `startsOnTheLimitAreStillUsed` (prefix and segment fits on the limit are used and reported; the full fit's `SolverFailed` semantics unchanged) pass; every initializer test asserts `prefix_passes == 1` and `prefix_iterations <= 50`
- [ ] `suspectSegmentIsRestartedFromItsNeighbour` and `restartDecisionUsesTwiceTheMedian` pass; a recording with one segment never restarts (`count >= 2`)
- [ ] The whole of `tst_fusion_kernel` runs in under 120 s on the capture machine (state counts: 450, 200, 301, 201 plus the golden fixtures)
- [ ] No expected value in these tests is derived from running the implementation; each is stated from the fixture's construction (the assertions' comments say from what)

**Complexity:** L

---

### Task 5.9: Golden test: cancellation inside the initializer, and preparation that asks nothing

**Purpose:** Spec 3.3's "segment fits are cancellable; each iteration is a boundary" through the public API, and the retired silent polls of preparation.

**Files to modify:**
- `tests/tst_fusion_golden.cpp` — `cancelAtEachKindOfBoundary_data` / `cancelAtEachKindOfBoundary`, `cancelDuringPreparation_data` / `cancelDuringPreparation`, `comparatorHoldsItsBounds` (the pair of Task 5.6)

**Technical Approach:**

`cancelAtEachKindOfBoundary_data`: rows are built from the golden's `progress` array of `coarse_maneuver` (loaded in the data function), so the call indices follow the capture rather than a hand count. With `p = golden.progress` and `at(text) = p.indexOf(text) + 1` (one-based call index; `QVERIFY(at(...) > 0)` for each):
- `"before the fit"`: 1, `Starting fit`
- `"graph construction"`: 2, `Integrating IMU factors` (the first prefix fit's build)
- `"prefix fit iteration"`: `at("Segment 1 of 1: prefix 60 s, pass 1, iteration 1")`
- `"prefix fit second iteration"`: `at("Segment 1 of 1: prefix 60 s, pass 1, iteration 2")`
- `"segment fit iteration"`: `at("Segment 1 of 1: pass 1, iteration 1")`
- `"full fit iteration"`: `at("Pass 1, iteration 1")`
- `"full fit second iteration"`: `at("Pass 1, iteration 2")`
The test body is unchanged: `Cancelled`, empty reason and diagnostics, all arrays empty, `received.size() == cancelAtCall`, `calls == cancelAtCall`, `received.last() == lastText`, and the re-run matches the golden channels.

`cancelDuringPreparation` keeps its name (the acceptance map cites it) and now proves the opposite of what its comment said: preparation neither reports nor asks. Rows: `"stationary_spin"` and `"coarse_maneuver"`, each with `cancelAtCall = 1` and `expectedTexts = {"Starting fit"}`; assert `calls == 1`, `received == expectedTexts`, `Cancelled`, empty result, and the re-run matches the golden. Rewrite the data function's comment: the first question of a run comes with the first reported boundary, `Starting fit`, on every recording; nothing in stage 1 grows with the recording beyond single passes.

**Acceptance Criteria:**
- [ ] `cancelAtEachKindOfBoundary` has the seven rows above, each reaching exactly `cancelAtCall` boundaries and ending `Cancelled`, and no row's index is a hand-written constant beyond 1 and 2
- [ ] `cancelDuringPreparation` passes with one call and `{"Starting fit"}` on both fixtures; its comment no longer mentions candidate windows
- [ ] `progressMatchesGoldenBoundaries` passes unchanged in code against the re-captured `coarse_maneuver.json`
- [ ] `ctest -C Release -R "tst_fusion_golden(_exact)?$"` passes after Task 5.11; `ctest -L audit` passes (both cited function names still exist)

**Complexity:** S

---

### Task 5.10: Documentation that must not lie

**Purpose:** Phase 7 rewrites the fusion documentation; until then every statement this phase falsifies is corrected in place, minimally, and the fixture table says what the fixtures now exercise.

**Files to modify:**
- `src/fusion/fusion.h` — the public contract's two statements about preparation: the `CancelFn` comment (lines 62-64) and the cancellation sentences of the `run()` comment (lines 73-77)
- `docs/SENSOR_FUSION.md` — section 4 (lines 93-96, the key list 134-146), section 5 (150-174), section 7's cancellation paragraph (229-235)
- `tests/README.md` — section 1's rows for `tst_fusion_golden` and `tst_fusion_kernel` (lines 122-123 on `master`, in Phase 1's wording), section 11's fixture table (three success rows), the `trace` key list (line 863), the capture-tool check sentence (lines 1105-1106 on `master`; identified by the words `stationary_spin`'s `initialization`), the tolerance-policy sentence listing the exact keys (Phase 3's wording)

**Technical Approach:**

`src/fusion/fusion.h`, exact edits (comments only; no declaration changes):

1. `CancelFn` (lines 62-64): "Asked at each boundary whether to abandon the fit; true abandons it at that boundary. Asked more often than ProgressFn is called: preparation asks without reporting. Must not throw." becomes "Asked at each boundary whether to abandon the fit, immediately after ProgressFn has been called for that boundary; true abandons it there. Must not throw."
2. `run()` (lines 73-77): "Cancellation is observed during preparation before each candidate window of the initializer's stationary-window scan (nothing is reported there), then before the fit starts, between graph-construction blocks and before each optimizer iteration; a linear solve in progress finishes first. The rest of preparation is a few single passes over the recording." becomes "Preparation is a few single passes over the recording and asks nothing. Cancellation is observed at every reported boundary: `Starting fit`; `Integrating IMU factors`, every 256 states of every graph build; and every iteration of every optimizer pass, in the initializer's prefix and segment fits (whose texts name the segment) as in the full fit. A linear solve in progress finishes first." The purity paragraph before it and the exception paragraph after it stay.

`src/fusion/fusionpipeline.h`: the `runPipeline()` comment ("The whole fit: adapter, checks, window, initializer, fit, ...; anything thrown before the fit starts is Rejected; anything from the fit onward is SolverFailed") lists the initializer as a stage without placing it under preparation, and Task 5.4 already adds the sentence that the initializer's fits are part of the fit stage; check after Task 5.4 that the comment does not say the initializer runs before `Starting fit` or polls silently, and correct it there if it does.

`docs/SENSOR_FUSION.md`, exact edits:

1. Lines 93-96: "There are no attitude, stationary, zero-velocity or magnetic measurement factors. The zero-centered bias prior has sigmas 0.3 m/s^2 and 0.03 rad/s; stationary averages only initialize the free variables." becomes "There are no attitude, stationary, zero-velocity or magnetic measurement factors. The zero-centered bias prior has sigmas 0.3 m/s^2 and 0.03 rad/s; the initializer only chooses where the solver starts."
2. Key list: "`initialization`, `stationary_interval_s`, `anchor_time_s`," becomes "`initialization`, `stationary_interval_s` and `anchor_time_s` (both `null`; the keys remain for readers of older diagnostics), `initializer` (`segment_length_s`; `segments`, per segment `index`, `start_s`, `end_s`, `anchor_s`, `anchor_sacc_m_s`, `prefix_length_s`, `yaw_sigma_deg`, `prefix_fits`, `iterations`, `fallback`; `fallback_segments`),"; "`selected_heading_deg`," becomes "`selected_heading_deg` (`null`),"; "`seeds` (per starting heading: biases, convergence, iterations, objective, residual RMS)" becomes "`seeds` (the one fit: biases, convergence, iterations, objective, residual RMS; `heading_deg` is `null`)".
3. Section 5, the first two paragraphs (from "The initializer scans 30-second windows" to "No preliminary short-fit initializer or heading search is implemented.") become one paragraph: "The initializer cuts the fitted window into consecutive 600 s segments (a final piece shorter than 120 s joins the segment before it). In each segment the fix with the smallest speed accuracy is the anchor; the attitude there is the measured force aligned with GNSS acceleration minus gravity, with zero gyro bias; a window of 60 s centred on the anchor is fitted from four heading offsets (0, 90, 180, 270 degrees), the marginal yaw sigma of its first pose is computed from the best fit, and the window doubles until that sigma is below 20 degrees or the window is the segment; the whole segment is then fitted once from the best prefix fit. The full fit starts from every segment's fitted attitudes, the first segment's gyro bias and a zero accelerometer bias; a segment in which every prefix start fails starts from its coarse attitude propagated by the gyro, and the diagnostics say so. A segment without motion has an arbitrary yaw. Heading remains free during optimization; the fit does not assume a known mounting heading or equate GNSS course with sensor orientation."
4. Section 5, third paragraph: delete the two sentences "In the diagnostic recording `24-09-07/08-35-23` no stationary window passed and the coarse initializer produced a poor converged fit, with large residuals (position and velocity RMS of 25.9 m and 7.5 m/s) and an accelerometer bias near 19 m/s^2." (they describe the previous initializer; spec section 12 expects this recording to fit well now). Keep "**Numerical convergence does not establish physical accuracy.**", Phase 3's stopping sentence and the rest.
5. Cancellation paragraph (229-235): "**Cancellation** is observed at four kinds of boundary: during preparation, before each candidate window of the search for a stationary interval (the one part of preparation that grows with the recording beyond a few single passes; nothing is reported there, so the progress texts begin with the fit); before the fit starts; every 256 states of graph construction; and before each optimizer iteration." becomes "**Cancellation** is observed at three kinds of boundary: before the fit starts (preparation neither reports nor asks; the progress texts begin with `Starting fit`); every 256 states of graph construction, in the initializer's prefix and segment fits as in the full fit; and before each optimizer iteration of any of those fits (the segment fits' texts name the segment and, for a prefix, its length)." The following sentences stay.

`tests/README.md`:

1. Section 1, `tst_fusion_golden` row: the parenthesis about silent preparation boundaries ("including the silent ones of preparation, before each candidate stationary window, where no progress text is emitted", in Phase 1's wording) becomes "(preparation has none: the first boundary is `Starting fit`; the initializer's prefix and segment fits have the same kinds as the full fit)". `tst_fusion_kernel` row: "the ten stationary-gate cases, the stationary-window scan asking for cancellation once per candidate window without reporting (and a window assessed with a given gap limit equal to one that derives it), the coarse initializer," becomes "the segmented initializer (segment cutting on fixes, the smallest-sAcc anchor, prefix growth on the marginal yaw sigma, the fallback when every prefix start fails, its progress texts, and the five synthetic recordings of the specification),"; "the fit trace (initializer result and cost before and after every optimizer iteration)" becomes "the fit trace (the segment account and cost before and after every optimizer iteration)".
2. Fixture table, "Exercises" cells: `coarse_linear`: "coarse initializer, near-zero objective (2.9e-12), boundary timing" becomes "a window shorter than one segment (one segment, the 60 s prefix covers it), an exactly unobservable yaw (the four prefix starts tie and the first wins), near-zero objective, boundary timing"; `coarse_maneuver`: Phase 3's "several LM iterations over two bias passes" becomes "the full fit started from the segment fit's solution"; `stationary_spin`: "stationary window `[0, 30)` accepted and `[5, 35)` rejected, anchor at 15 s propagated backwards," becomes "one segment at rest whose prefix grows from 60 s to 120 s to cover it (yaw unobservable and arbitrary; the anchor is the first fix, every sAcc being equal),". Keep the rows on one line each and Phase 4's per-step clauses.
3. The `trace` key list: "(`method`, `interval_s`, `anchor_time_s`, `gyro_bias_rad_s`, `start_quaternion_xyzw`, `converged`, `history` as ...)" becomes "(`initializer` with `segment_length_s` and per segment its bounds, anchor, prefix window, yaw sigma, fit counts, `converged`, `fallback`, start quaternion and biases; `converged`; `history` as ...)".
4. The capture-tool check sentence: "`stationary_spin`'s `initialization` is the stationary method with `stationary_interval_s = [0, 30]`" becomes "`stationary_spin`'s `initializer.segments[0].prefix_length_s` is 120".
5. Tolerance policy, the exact-keys sentence (Phase 3's "counts and copied tuning values (...)"): append "and the initializer account's counts, fix times and lengths (`index`, `prefix_fits`, `start_s`, `end_s`, `anchor_s`, `anchor_sacc_m_s`, `prefix_start_s`, `prefix_end_s`, `prefix_length_s`, `segment_length_s`, `fallback_segments`)".

No other documentation: the acceptance map is Phase 7's; `README.md` and `CMakeLists.txt` name nothing this phase changes.

**Acceptance Criteria:**
- [ ] `grep -n "stationary\|preparation asks\|candidate window" src/fusion/fusion.h src/fusion/fusionpipeline.h` prints nothing; the `run()` comment names the three boundary kinds (`Starting fit`, `Integrating IMU factors` per graph build, every iteration of every prefix, segment and full-fit pass) and says preparation asks nothing
- [ ] `grep -n "stationary" docs/SENSOR_FUSION.md` prints only the factor-list sentence of section 4 ("no attitude, stationary, zero-velocity or magnetic measurement factors") and the key list's `stationary_interval_s`
- [ ] `grep -n "candidate window\|stationary window\|coarse initializer\|stationary gate" tests/README.md docs/SENSOR_FUSION.md` prints nothing
- [ ] The key list in `docs/SENSOR_FUSION.md` names `initializer` with `segment_length_s`, `segments` (ten keys per segment) and `fallback_segments`, and the four `null` keys
- [ ] `ctest -C Release -L audit` passes

**Complexity:** S

---

### Task 5.11: Re-capture the goldens

**Purpose:** Every numerical phase ends by re-capturing the goldens with the Phase 1 recipe, so the suite is green at the phase boundary and the goldens on disk are the last capture.

**Files to modify:**
- `tests/data/fusion/capture.json` — rewritten by the tool (date, revision, hashes, the fixture generator hashes)
- `tests/data/fusion/coarse_linear.json`, `coarse_maneuver.json`, `stationary_spin.json` — rewritten (new `initializer` object, the four `null`s, new `progress` arrays with the segment texts, new `trace` objects, new histories and numbers)
- `tests/data/fusion/coarse_maneuver.channels.txt`, `stationary_spin.channels.txt` — rewritten with new bits; `coarse_linear.channels.txt` may change in the last bits (the full fit now starts from the segment solution) and may not
- The nine `reject_*.json` — rewritten byte-identical (rejections happen before the initializer)

**Technical Approach:**

Run Tasks 5.1-5.10 first (build green; `tst_fusion_kernel` green except `fitTraceMatchesGolden`; `tst_fusion_golden` red on the golden comparisons only). Then the recipe, quoted verbatim from Phase 1 (`01-golden-regression-harness.md`, "The re-capture recipe (quote this in Phases 3-6)"):

> From the repository root, Git Bash, on the capture machine (`build-phase1/`, 64-bit MSVC 19.44, Release):
>
> 1. `cmake --build build-phase1 --config Release`
> 2. `PATH="/c/Qt/6.9.3/msvc2022_64/bin:$PWD/build-solver-deps/GTSAM-install/bin:$PWD/build-solver-deps/oneTBB-install/bin:$PATH" build-phase1/FlySightViewer-build/Release/fusion_golden_capture.exe --revision "$(git rev-parse HEAD)"` — twelve lines, no `** UNEXPECTED **`, `wrote 16 files to .../tests/data/fusion`, exit 0.
> 3. `cmake build-phase1/FlySightViewer-build` — log says `Fusion exact tests registered for Release`.
> 4. `ctest --test-dir build-phase1/FlySightViewer-build -C Release -L fusion --output-on-failure` and `ctest --test-dir build-phase1/FlySightViewer-build -C Release -L exact --output-on-failure` — all green.
> 5. `git status --porcelain -- tests/data/fusion/` and `git diff --stat -- tests/data/fusion/` — list every changed golden file among the phase's files.
>
> An unexpected outcome (exit 2) or a non-deterministic capture (exit 3) is never committed; a red test after the capture means the kernel and the goldens disagree with the tests' literal expectations (a count, a text, a key set), which the phase must resolve in the tests or the kernel, never by editing a golden.

The full recipe with its PATH note and "what changes" list is in `tests/README.md` section 11, "Re-capture procedure".

What this phase expects the capture to change:

| File | Expected change |
|---|---|
| `capture.json` | date, revision, every success hash, both `fixture_generator_sha256_lf` entries (Task 5.6 edited the generator) |
| `coarse_linear.json` | `initializer` added (one segment, `prefix_length_s` 60, `prefix_fits` 4, `fallback` false); `initialization` text; `anchor_time_s`, `stationary_interval_s`, `selected_heading_deg`, `seeds[0].heading_deg` `null`; `progress` gains the four prefix fits' and the segment fit's texts before the full fit's; `trace` in the new shape; the full fit's `history` and `stopping.passes` may shrink; numbers of rounding size may move |
| `coarse_maneuver.json` | as above with `prefix_length_s` 60; the full fit starts near its answer, so `seeds[0].iterations` and `history` shrink; `objective` about `2.0016` (the same optimum) |
| `stationary_spin.json` | as above with `prefix_length_s` 120, `prefix_fits` 8; the yaw is arbitrary now, so `seeds[0]` biases, the `residuals`, `objective` and the channels change beyond rounding; `max_endpoint_correction_deg` changes |
| `coarse_maneuver.channels.txt`, `stationary_spin.channels.txt` | new bits (expected) |
| `coarse_linear.channels.txt` | unchanged or last-bit changes (not required either way) |
| nine `reject_*.json` | unchanged (not listed by `git status`) |

Checks after step 5: `git status --porcelain -- tests/data/fusion/` lists `capture.json`, the three success `.json`, the two or three `.channels.txt`, and no `reject_*.json` (a listed rejection means the initializer ran before a rejection: a stage error in Task 5.4). `grep -c '"Segment 1 of 1: ' tests/data/fusion/coarse_maneuver.json` is greater than 4; `grep -c '"prefix 120 s' tests/data/fusion/stationary_spin.json` is greater than 0 and the same count for `coarse_maneuver.json` is 0. `tst_fusion_session::naturalSessionEndToEnd` (env-gated on a real recording) is run if the recording is available and its outcome reported.

**Acceptance Criteria:**
- [ ] The tool printed twelve lines, none `** UNEXPECTED **`, exited 0; the reconfigure logged `Fusion exact tests registered for Release`
- [ ] The three success goldens carry `initializer` with `prefix_length_s` 60, 60, 120 and `prefix_fits` 4, 4, 8 for `coarse_linear`, `coarse_maneuver`, `stationary_spin`, and `fallback_segments` empty in all three
- [ ] No `reject_*.json` is listed by `git status`
- [ ] `ctest -C Release -L fusion`, `-L exact`, `-L audit` and the whole suite without `-L` pass on `build-phase1/FlySightViewer-build`
- [ ] The phase's report lists every file `git status` shows under `tests/data/fusion/`

**Complexity:** S

---

## Testing Requirements

### Unit Tests
- `tst_fusion_kernel`: deleted `stationaryGates`, `stationaryScanPollsSilently`; re-specified `shortWindowIsOneSegment` (was `shortInputUsesCoarseInitializer`), `exactConstantVelocityFit`, `validationRejectsEachDefect`; new `segmentsAreCutOnFixes`, `yawSigmaIsMarginalAboutTheVertical`, `initializerProgressTexts`, `initializerDiagnosticsShape`, `initializerFixturesAreDeterministic` (Task 5.7), `startsInMotionGrowsToTheManoeuvre`, `atRestPrefixStopsGrowing`, `smallestSaccFixIsTheAnchor`, `driftingBiasSegmentsConverge`, `allPrefixFitsFailFallsBack` (Task 5.8). `fitTraceMatchesGolden` unchanged in code, green after the re-capture. Phase 3's and Phase 4's slots unchanged in code.
- `tst_fusion_golden`: `cancelAtEachKindOfBoundary` (seven rows from the golden's progress), `cancelDuringPreparation` (one call, `Starting fit`, two fixtures), `comparatorHoldsItsBounds` (`prefix_fits` pair); `successFixturesMatchGolden`, `rejectionFixturesMatchGolden`, `progressMatchesGoldenBoundaries`, `fitTraceMatchesGolden` compare against the re-captured goldens; determinism and thread tests unchanged.
- `tst_fusion_session`, `tst_fusion_jobs`, `tst_fusion_rows`: unchanged; they compare channels through `goldenDifference()`. `tst_fusion_session::cancelStopsAtNextBoundary` (rows 1-4 on `coarse_maneuver`, first text `Starting fit`) still holds: its boundaries are now `Starting fit`, the first prefix build, and the first two prefix iterations. `tst_fusion_session`'s `stationary_spin` unwrap assertion (yaw spans more than 360 degrees) holds for any starting yaw.

### Integration Tests
- `ctest --test-dir build-phase1/FlySightViewer-build -C Release -L fusion --output-on-failure` and `-L exact` after the re-capture; `-L audit`; the whole suite without `-L`.
- Configure-time: `GTSAM link confinement: OK` (no new target, no new GTSAM-including source outside `src/fusion/`; `Marginals.h` is included by `factorgraphfit.cpp` only) and `Fusion exact tests registered for Release`.

### Manual Verification
- Task 5.11's `git status` and `grep` checks.
- With the Phase 2 runner, if built: the four spec section 12 recordings under `TEMP/data/` (untracked); read `initializer.segments` (about 3000 states per 600 s segment at 5 Hz, prefix lengths, yaw sigmas), `stopping.rule` and `seeds[0].iterations`; the spec expects `11-17-12` converged in about 12 iterations near 45,000, `08-35-23` about 11,000 in under 40 iterations, and the two Data comp 2 recordings converged in 10-15 iterations. This is Michael's corpus check, not an acceptance criterion.

## Notes for Implementer

### Gotchas
- **Catch `FitFailure` only, and only around the two fit calls.** A `catch (const std::exception &)` in the initializer would turn a defect into a silent fallback; `FusionCancelled` is not a `std::exception` and must never be caught here. GTSAM can throw `IndeterminantLinearSystemException` from inside a solve; that is a `std::exception`, propagates, and becomes `SolverFailed` through Phase 3's generic handler, which is the right verdict.
- **Rounding-equal objectives are equal.** The `1e-9 * max(1, best)` rule is what keeps `coarse_linear` (exactly unobservable yaw: four starts with objectives of order 1e-12 to 1e-25) on start 0 on every compiler. Compare with `best` finite: the first completed start is taken unconditionally (an `inf - inf` comparison is NaN and would never replace).
- **The yaw sigma is capped, never `null`, for a completed start.** `null` in `yaw_sigma_deg` means "no prefix fit completed". A recording with no yaw information reports 180; the golden of `stationary_spin` may hold a value between 20 and 180 (noise gives a weak spurious constraint, NOTES.md 4.6), and that value is solver arithmetic under the degree floor.
- **Body frame first.** The `Pose3` tangent is `[omega; v]` with `omega` in the body frame (`retract = compose(Expmap)`); `cov.block<3,3>(0,0)` is the rotation block and must be rotated by `R cov R^T` before its `(2, 2)` element means "about the vertical". The lab did this at lines 220-222.
- **`QString::arg` order.** The pass format's two remaining placeholders must be the lowest-numbered ones left (`%4`, `%5` after the segment, count and length are in; `%3`, `%4` for the segment fit). Do not pre-format the pass text with `%1`/`%2` and then `arg` the segment numbers into it: `arg` fills the lowest placeholder first.
- **`prefix_length_s` is the nominal length** (60, 120, 240, ...), not the clipped window's duration; the window's actual first and last fix are `prefix_start_s` / `prefix_end_s` in the trace. `sacc_anchor` is the case where they coincide (170-230, length 60).
- **`fittedWindow()` throws below three fixes.** Count the fixes of a prefix window before cutting it; grow instead of catching. A segment always has three fixes by `segmentBounds()`'s merging rule.
- **Always run the segment fit when a prefix completed**, even when the prefix covered the segment. It converges in one or two iterations from its own solution, keeps one code path, and makes `iterations` and `converged` meaningful for every non-fallback segment.
- **The trace is filled before the full fit.** A run that is cancelled or fails inside the full fit still leaves the account in `PipelineTrace` (Task 5.8's `motion_start` fallback row reads it without asserting the outcome).
- **`drifting_bias` and the constant-bias full fit.** The spec's section 6 evidence is that such recordings converge under this initializer with a raised IMU misfit; the fixture's 1 deg/s drift over 200 s is milder than the corpus's. If the full fit nevertheless ends on `iteration limit`, report it: the fixture's assertion for Phase 5 is the segments' convergence, the full fit's is stated as Succeeded because the spec's evidence says so, and the remedy would be a gentler drift in the fixture (the spec fixes 1 deg/s; the length is free), never the kernel.
- **Phase 3's literals.** `biasSettledByCostTest` asserts `passes <= 2` and `slowTailAtTheIterationLimit` `history.size() == 125` for the full fit; both hold whatever the start. `nonConvergenceIsSolverFailure`'s `history.size() == 5` counts the full fit only.
- **Goldens change only by re-capture.** If a success fixture prints `** UNEXPECTED **` the initializer regressed it; never edit a golden or a golden fixture. `coarse_maneuver`'s fallback row of Task 5.8 is the proof that the fallback start is the pre-phase start, so a `coarse_maneuver` regression is in the segmented path, not the fallback.
- **Line endings** as in Phases 1-4: do not open a golden in an editor between the capture and the checks.
- **Read-only git** for the implementer (`status`, `diff`, `rev-parse`); the orchestrator stages by path, including the two deleted `stationarywindow.*` files and every re-captured golden.

### Decisions Made
- **The initializer runs in the fit stage**, after `Starting fit`: its fits are fits, its cancellations are the fit's, and anything it throws that is not a caught `FitFailure` is `SolverFailed`, consistent with `fusionpipeline.h`'s rule "anything from the fit onward is SolverFailed". Preparation (stage 1) reports nothing and asks nothing; `pollCancel()` is deleted with its only caller.
- **`InitialState` (rotations, gyroBias) is separate from `Initialization` (state plus account).** The overview says the initial-state type carries the account; the prefix and segment fits take an `InitialState` and have no account, so the account travels with the state in `Initialization` instead of inside it. Phase 6 adds to neither: `b1` starts at zero.
- **`fitFactorGraph(samples, initial, tuning, passFormat, checkpoint)`**: the progress label is a `QString` format with two remaining placeholders; the full fit's default reproduces `Pass %1, iteration %2` byte for byte. Rejected: a label prefix (cannot produce both segment texts) and a struct (more shape for one string).
- **`Integrating IMU factors` is reported per graph build in every fit**, unchanged text, per the overview ("reported per graph build as today"). The segment texts appear on iterations only.
- **The yaw sigma comes from `FitResult::graph`**, the rebuild at the fitted bias Phase 3 already computes, through a free `yawSigmaDeg(graph, values, key)` next to the fit. Rejected: a flag on the fit routine (the routine would compute a number it does not need) and a rebuild in the initializer (a second graph build changes the progress sequence).
- **The yaw sigma is capped at 180 degrees**, and an indeterminate system or non-finite covariance is reported as the cap: a rotation about the vertical is an angle on a circle, so any larger value carries the same information, and the cap makes the golden of an exactly unobservable recording portable (a finite garbage covariance would differ between compilers).
- **Rounding-equal objectives tie and the earliest start wins** (`1e-9` relative of `max(1, best)`): the spec's "lowest objective" refined to what "lowest" means at rounding precision, so that an exactly unobservable yaw is chosen deterministically (start 0) on every platform. Objectives that differ by more are compared as numbers.
- **Fallback means "the segment's start state stands in for its fit"**: coarse-at-anchor with zero bias when no prefix fit completed (the spec's fallback, over that segment only), the prefix-carried start when the segment fit itself threw after a completed prefix (the spec does not cover this case; the prefix result is strictly better information than the coarse attitude). Both set `fallback = true`, `iterations = 0`.
- **The segment fit always runs when a prefix completed**, even when the prefix covered the segment (one path).
- **`coarseAttitude(d, k)` uses the forward difference and, at a segment's last fix, the backward one**, within the segment; today's rule at `k = 0` is reproduced bit for bit.
- **`segmentLength` and `minFinalSegment` are `Tuning` fields** (spec: 600 s tuning; 120 s named a tuning value by the assignment); 60 s, 20 degrees and the four offsets are constants. The final piece is also merged when it has fewer than three fixes (the validity rule the spec assigns to segments).
- **`prefix_length_s` is the nominal length reached**; the clipped window's fixes are `prefix_start_s` / `prefix_end_s` in the trace only (the diagnostics layout is binding).
- **`selected_heading_deg` and `seeds[0].heading_deg` are `null`**: nothing selects a heading; the keys stay so readers of older diagnostics find them. `FitResult::heading` is deleted.
- **`initialization` is `segmented initialization; heading from segment fits`.**
- **The failure diagnostics keep Phase 3's shapes**; the account of a run whose full fit failed is in `PipelineTrace`, not in the JSON. Adding `initializer` to solver-failure diagnostics would change the key sets Phase 3's tests pin, for no reader.
- **The forcing for "every prefix fit fails"** is a kernel-test `Checkpoint` whose progress function throws `FitFailure` at every prefix iteration text. No data or tuning can fail the prefix fits without failing the full fit on the same fixes (the full graph is a superset), so a kernel knob would have to be scoped to the initializer, which would be a test hook in production code; the throwing checkpoint exercises exactly the catch-and-count path with nothing added to the kernel. `maxIterations = 0` was rejected (Phase 3 validates `>= 1`, and the full fit would fail too).
- **The four synthetic recordings are not golden fixtures**: `initializerFixture(name)` beside `fusionFixtures()`, so the capture tool, `tst_fusion_golden` and the "twelve fixtures, 16 files" checks are untouched; their expectations live in `tst_fusion_kernel`.
- **`drifting_bias` uses `segmentLength = 60`, `minFinalSegment = 12`** (the spec's 600:120 ratio) at 1 Hz GNSS and 10 Hz IMU: 201 states, four segments, seconds; 1320 s of 600 s segments would be 6600 states at 5 Hz.
- **Rotations in fixtures are exact rationals** (`.6/.8`, `.96/.28`) so the generators keep the file's no-transcendental rule.
- **The trace JSON is the diagnostics' segment object plus `prefix_start_s`, `prefix_end_s`, `converged`, the start quaternion and the two biases**: enough to localize a divergence to a segment's start before the first full-fit iteration; the per-length sigmas stay in the C++ account for the kernel tests.
- **Exact keys**: the account's counts, fix times and copied lengths; this makes the diagnostics' top-level `start_s` / `end_s` exact too, which they always were in substance.
- **`cancelDuringPreparation` keeps its name** (acceptance map) and its meaning inverts: preparation has no boundary.
- **The cancellation rows are indexed from the golden's progress array**, not hand-counted, so a change in a prefix fit's iteration count does not silently move a row onto a different boundary.

### Open Questions
- None that block implementation. Two notes for the orchestrator: (1) if `driftingBiasSegmentsConverge`'s full-fit `Succeeded` assertion fails (Gotchas), the implementer reports the `stopping` object and the assertion is reduced to the spec's own clause pending Phase 6, with the fixture unchanged; (2) `stationary_spin`'s golden `yaw_sigma_deg` and its arbitrary yaw are validated on MSVC only; if the CI portable comparison later fails on that fixture's yaw-dependent channels, the tie rule's margin (1e-9) is the first thing to look at, and the spec's "arbitrary yaw is acceptable" means a fixture-side remedy (a small manoeuvre in `stationary_spin`) would be the right one, in a fixup, not a kernel change.

## Definition of Done

This phase is complete when:
1. All tasks have passing acceptance criteria, in particular Task 5.8's five recordings and Task 5.11's expected golden diff (three successes changed, no rejection changed); the growth stop (spec 3.2 d), the prefix budget and the on-limit accounting (spec 3.3, Budgets) are demonstrated by `atRestPrefixStopsGrowing` and `startsOnTheLimitAreStillUsed`; the segment check with neighbour restart (spec 3.2 step 3) by `suspectSegmentIsRestartedFromItsNeighbour` and `restartDecisionUsesTwiceTheMedian`
2. All tests pass: `ctest -C Release` on `build-phase1/FlySightViewer-build` without `-L`, and with `-L fusion`, `-L exact`, `-L audit`, after the reconfigure that reads the new `capture.json`
3. Code follows the patterns of `initializer.cpp` (small named helpers, one anonymous namespace, constants with units), `factorgraphfit.cpp` (one hand-driven LM pass per checkpoint, the rebuild as the reported graph), `fusionoutput.cpp` (explicit `QJsonValue::Null`, one writer per object), `fusion.cpp` (classification by stage, catching only in `runPipeline()`) and `fusionfixtures.cpp` (index-based samples, SplitMix64 noise in the documented order)
4. `src/fusion/stationarywindow.*` are gone; no file outside `src/fusion/` includes a GTSAM header that did not before; `Marginals.h` is included by `factorgraphfit.cpp` only; the kernel's purity, threading and cancellation rules and the public result contract are unchanged
5. No TODOs or placeholder code remains; the phase's report lists every file created, modified, deleted and re-captured (`src/fusion/initializer.h`, `initializer.cpp`, `factorgraphfit.h`, `factorgraphfit.cpp`, `fusionsamples.h`, `fusionsamples.cpp`, `fusionprogress.h`, `fusion.cpp`, `fusionpipeline.h`, `fusionoutput.h`, `fusionoutput.cpp`; deleted `src/fusion/stationarywindow.h`, `stationarywindow.cpp`; `src/CMakeLists.txt`; `tests/fusion/fusiontrace.h`, `fusiongolden.cpp`, `fusionfixtures.h`, `fusionfixtures.cpp`; `tests/tst_fusion_kernel.cpp`, `tests/tst_fusion_golden.cpp`; `docs/SENSOR_FUSION.md`, `tests/README.md`; and every file `git status` shows under `tests/data/fusion/`)
