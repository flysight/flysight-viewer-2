# Phase 8: Fusion kernel port and golden parity

## Overview

This phase retypes the batch GNSS/IMU factor-graph fit of `sensor-fusion-clean-port` as a pure static library, `flysight_fusion`, in `master`'s code style: input validation and audit, initializer, GTSAM fit, dense output extraction (timestamps, position / velocity / acceleration, unwrapped roll / pitch / yaw, quaternion) and the diagnostics JSON, with progress text and cancellation observed at exactly the branch's boundaries. The algorithm is frozen; the code is not (spec section 4).

Parity is demonstrated, not asserted: golden outputs are captured from the branch's own code for a set of deterministic synthetic fixtures (three successes, nine rejections), committed as test data under `tests/data/fusion/`, and the port is tested against them with QtTest (spec acceptance 4). The goldens are captured with the branch's fusion sources built against the GTSAM this plan **ships** (upstream `814a734`, built without Boost; Phase 1). Because the branch itself was validated against a Boost-enabled build of the fork commit, the capture also runs against that old build and requires byte-identical output from both before any golden is accepted (Task 8.9, "cross-check"). Nothing here touches the engine, a session, the registry, or Qt GUI: registration, derived values and plots are Phase 9.

## Dependencies

- **Depends on:** Phase 1 (Solver dependencies).
- **Blocks:** Phase 9 (Fusion calculation, derived values, and plots), Phase 10.
- **Assumptions:**
  - Phase 1 is accepted locally: `cmake/SolverDependencies.cmake` is included by `src/CMakeLists.txt` after `find_package(Boost REQUIRED)`, the imported target `gtsam` exists in the application project, and `tests/CMakeLists.txt` has the gated block `if(FLYSIGHT_BUILD_FUSION_TESTS)` with `flysight_add_fusion_test(<name> SOURCES ... [LIBS ...] [ENVIRONMENT ...])` (which adds the 64 MiB main-thread stack through `flysight_solver_stack`, the DLL search path through `flysight_solver_test_environment`, labels `core;fusion`, timeout 600 s, and `/bigobj`).
  - **Shipping solver (Boost-free).** Phase 1 left a Release build of the pinned upstream GTSAM (`814a734`, `GTSAM_ENABLE_BOOST_SERIALIZATION=OFF`, `GTSAM_USE_BOOST_FEATURES=OFF`, TBB on) and oneTBB in `build-solver-deps/GTSAM-install` and `build-solver-deps/oneTBB-install` at the repository root (git-ignored). This is the solver the goldens are captured against **and** the port's tests run against. Every application configure in this phase passes `-DGTSAM_INSTALL_DIR=<repo>/build-solver-deps/GTSAM-install -DONETBB_INSTALL_DIR=<repo>/build-solver-deps/oneTBB-install`. If that directory is missing or its `include/gtsam/config.h` does not have both Boost macros `0`, stop and report; do not rebuild it into another place and do not substitute the old install.
  - **Old solver (Boost-enabled), cross-check only.** The pre-existing untracked `third-party/GTSAM-install` and `third-party/oneTBB-install` (Release, VS 2022, TBB on, fork commit `8938b9f`, Boost 1.87 static, both Boost macros `1`) are intact. They are the configuration the branch was validated with and are used for one thing: the second harness build of Task 8.9's cross-check. Nothing else is built or tested against them. Never clean, rebuild, stage or modify them. Boost 1.87 at `C:/Program Files/Boost/boost_1_87_0` is needed for that second harness build only.
  - The implementation agent runs **no git command that changes repository state** (Commit Policy). `git show`, `git archive`, `git rev-parse`, `git log`, `git diff`, `git status` are read-only and allowed. `git worktree add` is **not** (it writes under `.git/`); see Task 8.9 for the read-only equivalent.
  - Phases 2 and 4 are *not* assumed. The kernel's inputs are plain arrays and its progress / cancel parameters are its own types (Decision 3).

### Facts established during documentation (do not re-derive)

- **Reference revision:** `sensor-fusion-clean-port` = `83a64479fd4e7e2e10bce0b5477c5dd7a49dee7d`.
- **Branch fusion sources (all of them):** `src/batchfusion.h/.cpp` (330 lines: statistics, IMU integration, validation, initializer, graph, solve, reconstruct, window), `src/fusioninput.h/.cpp` (session adapter: reads channels, validates, subtracts the epoch, converts deg/s to rad/s, builds the audit), `src/imugnssekf.h/.cpp` (`runFusion`: wrapper validation, window, GNSS-gap rule, initialize, solve, reconstruct, channel and diagnostics assembly), and in `src/calculations/sensorfusioncalculations.cpp` only three things belong to the kernel: the `unwrapDegrees` calls on roll / pitch / yaw, the failure diagnostics `{"algorithm":"batch-shared-bias-v1","failure":<what()>}`, and the 64 MiB stack. Everything else in that file (thread-local guard, modal dialog, nested event loop, sibling caching, message box) has no successor (spec section 2, acceptance 20).
- **Branch compile line for the fusion sources** (from `build-port/flysight_fusion.dir/Release/flysight_fusion.tlog/CL.command.1.tlog`): MSVC 14.44.35207, `/O2 /Ob2 /fp:precise /EHsc /MD /GR /GS /std:c++17 /permissive- /bigobj -Zc:__cplusplus -utf-8 /W1`, definitions `NDEBUG QT_NO_DEBUG QT_CORE_LIB _ENABLE_EXTENDED_ALIGNED_STORAGE EIGEN_NO_STATIC_ASSERT UNICODE WIN32 WIN64`, no `/arch`, no `/fp:fast`, no `/fp:contract`. Target definition on the branch: `target_link_libraries(flysight_fusion PUBLIC gtsam Qt::Core)`, `target_compile_options(... PRIVATE /EHsc /bigobj)` on MSVC, nothing else. `CMAKE_CXX_FLAGS_RELEASE` was the CMake default. (That build used the Boost-enabled GTSAM; Boost contributed an include directory and no code-generation flag. The same flags apply to a build against the Boost-free GTSAM.)
- `build-port/` was configured with `src/` of *this* working tree as its source directory while the branch was checked out. The working tree is now `master`, so `build-port/` cannot be rebuilt and its `flysight_fusion.lib` is of unknown freshness. It is used here only as the record of compile flags above; goldens are captured from a fresh build of the branch sources (Task 8.9).
- **The shared unwrap rule.** Branch `src/calculations/anglehelper.h` (`unwrapDegrees`) and `master`'s inline loop in the GNSS `course` calculation (`src/calculations/gnsscalculations.cpp` lines 297-306) perform the identical arithmetic: keep the first angle; `delta = a[i] - a[i-1]`; `if (delta > 180) delta -= 360; if (delta < -180) delta += 360;` append `previousResult + delta`.
- On the branch, `SessionData::getMeasurement` / `getAttribute` return a stored value before trying a calculated one, so a harness can store `GNSS/_time`, `IMU/_time`, `Local/...` and the `_LOCAL_ORIGIN_*` attributes directly and drive the branch's real `prepareFusionInput` without importing files or registering any calculation. Branch `flysight_model` is `sessiondata.cpp dependencymanager.cpp calculatedvalue.cpp`, Qt Core only. `calculations/localcoordinatecalculations.h` contributes only `constexpr` names.
- Effective `IMU/ax|ay|az` on `master` are m/s^2 (the conversion layer multiplies `g` by 9.80665) and `IMU/wx|wy|wz` are deg/s, exactly what the branch adapter assumes.

## Design summary (binding for all tasks)

### Library layout

Directory `src/fusion/`, namespace `FlySight::Fusion` (public) and `FlySight::Fusion::Detail` (internal). Header guards `FLYSIGHT_FUSION_<NAME>_H`. File names lower-case, concatenated, named for what the thing is; nothing is named EKF.

| File | Visibility | Contents (branch origin) |
|---|---|---|
| `fusion.h` | **public**, Qt Core only | `Channels`, `Outcome`, `Result`, `ProgressFn`, `CancelFn`, `run()` |
| `fusion.cpp` | | `run()` (5 lines) and `Detail::runPipeline()` (branch `runFusion` orchestration + the catch block of `sensorfusioncalculations.cpp`) |
| `fusionpipeline.h` | internal | `PipelineTrace`, declaration of `Detail::runPipeline()` |
| `fusionprogress.h` | internal, header-only | `Checkpoint`, `FusionCancelled` (branch `Progress`, `checkProgress`) |
| `fusionsamples.h/.cpp` | internal | `Vectors`, `Samples`, `Tuning`, constants, time checks, `medianInterval`, `validateSamples`, `fittedWindow`, wrapper checks, GNSS-outage rule (branch `Input`, `Config`, `validTimes`, `medianInterval`, `validate`, `window`, the checks at the top of `runFusion`) |
| `inputadapter.h/.cpp` | internal | `PreparedInput`, `prepareInput()` (branch `fusioninput.cpp`) |
| `samplestatistics.h/.cpp` | internal | `quantile`, `componentMean`, `componentStddev` (branch `quantile`, `mean`, `stddev`) |
| `imuintegration.h/.cpp` | internal | `integrationEdges`, `interpolateAt`, `gyroIncrement`, `preintegrateImu`, `propagateAttitude` (branch `edges`, `interpolate`, `preintegrate`, `propagate`) |
| `stationarywindow.h/.cpp` | internal | `StationaryWindow`, `assessStationaryWindow` (branch `Candidate`, `stationaryStats`) |
| `initializer.h/.cpp` | internal | `InitialAttitude`, `rotationAligning`, `initialAttitude`, `initialValues` (branch `Initialization`, `align`, `initialize`, `initialValues`) |
| `factorgraphfit.h/.cpp` | internal | `FitIteration`, `FactorResidual`, `FitResult`, `buildFactorGraph`, `fitFactorGraph` (branch `Iteration`, `Residual`, `Result`, `makeGraph`, `solve`) |
| `trajectoryreconstruction.h/.cpp` | internal | `DenseTrajectory`, `reconstructTrajectory` (branch `Dense`, `reconstruct`) |
| `fusionoutput.h/.cpp` | internal | channel assembly, unwrap, success and failure diagnostics (tail of branch `runFusion`) |

Only `fusion.h` is GTSAM-free. Every other header may include GTSAM and is included only by `src/fusion/*.cpp` and by `tests/tst_fusion_kernel.cpp`.

### Public API (contract for Phase 9)

```cpp
// src/fusion/fusion.h -- includes <QString>, <QVector>, <functional> only
namespace FlySight::Fusion {

/// Effective values of the declared inputs, exactly as the engine supplies them.
struct Channels {
    QVector<double> gnssTime;            // GNSS/_time, UTC s
    QVector<double> north, east, down;   // Local/north|east|down, m
    QVector<double> velN, velE, velD;    // Local/velN|velE|velD, m/s
    QVector<double> hAcc, vAcc, sAcc;    // GNSS/hAcc|vAcc (m), GNSS/sAcc (m/s)
    QVector<double> imuTime;             // IMU/_time, UTC s
    QVector<double> ax, ay, az;          // IMU/ax|ay|az, m/s^2
    QVector<double> wx, wy, wz;          // IMU/wx|wy|wz, deg/s
    qint64 originIndex = -1;             // _LOCAL_ORIGIN_INDEX
    double originLat = 0, originLon = 0, originHMSL = 0;   // _LOCAL_ORIGIN_LAT|LON|HMSL; audit only
};

enum class Outcome { Succeeded, Rejected, SolverFailed, Cancelled };

struct Result {
    Outcome outcome = Outcome::Cancelled;
    QString reason;            // Rejected / SolverFailed: the text that is also diagnostics "failure"
    QString diagnosticsJson;   // compact JSON; empty only when Cancelled
    // Succeeded only; all the same length, aligned with `time` (UTC s)
    QVector<double> time, north, east, down, velN, velE, velD, accN, accE, accD,
                    roll, pitch, yaw, qx, qy, qz, qw;
};

using ProgressFn = std::function<void(const QString &text)>;   // must not throw
using CancelFn   = std::function<bool()>;                      // true: abandon at the next boundary; must not throw

/// Pure: a function of `channels`. The callbacks cannot influence the result
/// except by abandoning it. Throws nothing except std::bad_alloc.
Result run(const Channels &channels, const ProgressFn &progress = {}, const CancelFn &cancelRequested = {});

} // namespace FlySight::Fusion
```

The twenty-one declared inputs Phase 9 will need are exactly the members of `Channels`: seventeen measurements (`GNSS/_time`, `IMU/_time`, six `Local/...`, three GNSS accuracies, six IMU channels) and the four `_LOCAL_ORIGIN_*` attributes. This is what the branch's adapter consumes (spec section 6); the branch's longer dependency list (`GNSS/lat`, `TIME/...`, `_TIME_FIT_*`) named transitive inputs, which the engine tracks by itself.

### Outcome representation

| Outcome | When | `reason` | `diagnosticsJson` | channels |
|---|---|---|---|---|
| `Succeeded` | fit converged | empty | full success object (Task 8.7) | 17 arrays, equal length |
| `Rejected` | anything thrown **before the fit starts**: adapter, wrapper checks, window, `validateSamples`, GNSS outage, initializer | `what()` | `{"algorithm":"batch-shared-bias-v1","failure":<reason>}` | empty |
| `SolverFailed` | anything thrown **from the fit onward** (non-finite or increasing cost, preintegration duration mismatch, a GTSAM exception, "did not converge", reconstruction) | `what()` | same two-key object | empty |
| `Cancelled` | `cancelRequested()` returned true at a boundary | empty | empty | empty |

Classification is by stage, not by exception type (GTSAM may throw `std::invalid_argument` from inside a solve). Internally the retyped functions keep the branch's throw-on-violation structure with the branch's message texts; `runPipeline` is the only place that catches. Catch order in both stages: `FusionCancelled` (not derived from `std::exception`, so nothing can swallow it) then `std::bad_alloc` (rethrown: memory exhaustion is not a function of the inputs, spec 8.2) then `std::exception`. `Rejected` and `SolverFailed` are both "results" in the sense of spec section 6; Phase 9 publishes both the same way and turns `Cancelled` into the engine's cancellation.

### Progress and cancellation boundaries

`Detail::Checkpoint` wraps the two callbacks. `checkpoint(text)` first calls `progress(text)` (if set), then `cancelRequested()` (if set) and throws `FusionCancelled` when it returns true. That is the branch's single callback (`Progress` returned false to cancel) split in two, in the same order. It is called at exactly the branch's three places and nowhere else:

| # | Place | Branch text | Port text |
|---|---|---|---|
| B1 | once, after the initializer and immediately before `fitFactorGraph` | `Starting heading 0 deg` | `Starting fit` |
| B2 | in `buildFactorGraph`, at the top of the GNSS-state loop when `k % 256 == 0`, before that state's factors are added (every bias pass and the final residual graph) | `Integrating IMU factors` | `Integrating IMU factors` |
| B3 | before every `optimizer.iterate()` | `Heading 0 deg, pass P, iteration I` | `Pass P, iteration I` (P, I one-based) |

No check exists before or during validation, inside a linear solve, or after the last iteration (reconstruction and assembly are short). Do not add any.

### Floating-point preservation rules

Inside every function marked **kernel** in the decomposition maps below:

**May:** move whole statements into a named function; rename variables, types and functions; pass by const reference; add `reserve()`; replace an index loop by another loop with the same iteration order; give a literal a named `constexpr` with the *same literal text* (`.015`, `1e-8`, `3.14159265358979323846`); replace `std::to_string(int)` in progress text; add comments.

**May not:**
1. Split one arithmetic expression into several statements, merge statements, or introduce a named sub-expression. One sanctioned exception: `gyroIncrement()` (Task 8.4), whose value is materialized into a `Vector3` by its consumer on the branch already.
2. Change operand order or parenthesization, even where algebra says it is neutral: `(e[i]+e[i-1])/2` stays a sum divided by 2; `x*180/pi` stays `(x*180)/pi`; `wm*pi/180` stays `(wm*pi)/180`; `wx * radians` with `constexpr double radians = 3.14159265358979323846 / 180;` stays a multiplication by that constant; `1.6*medianInterval(...)`, `5*medianInterval(...)`, `2*factor->error(values)` keep the constant on the left.
3. Change accumulation order or method: `std::accumulate(v.begin()+n, v.end()-n, 0.)`; `v += (x-m).cwiseAbs2()` over samples in order; `positionRms += ...squaredNorm()` over `k` ascending; the scan variable of the stationary search is accumulated with `s += 5`, not computed as `first + 5*i`. No `std::reduce`, no parallel algorithms.
4. Change `auto` to a concrete Eigen/GTSAM type or back in a numeric statement (it can switch lazy and eager evaluation), or change container element types in kernels (`Vectors` stays `std::vector<gtsam::Vector3, Eigen::aligned_allocator<gtsam::Vector3>>`).
5. Change which GTSAM call is made, its arguments, or the order of calls: factor insertion order (per state: `GPSFactor`, velocity `PriorFactor<Vector3>`, then for `k > 0` the `ImuFactor`; the bias prior last) fixes factor indices, the elimination ordering and therefore the numbers; one `PreintegrationParams::MakeSharedD` per preintegration, as on the branch; `r.compose(Rot3::Expmap(-inc))` in reverse order for backward propagation, not an inverse; LM driven by hand with `error()` / `iterate()` / `error()`.
6. Drop arithmetic that looks like a no-op: `Rot3::Rz(heading*pi/180).compose(attitude.rotation)` stays although the heading is the constant 0 (sign of zero).
7. Change integer conversions or tie-breaking: `size_t(index)` and `size_t(.1 * v.size())` truncate; `std::min_element` (first minimum) picks the anchor fix; the best stationary window is replaced only on a strictly lower score; `std::max({0., ...})` / `std::min({...})` keep their argument order.
8. Change the boundary conversions: `gt[k] - epoch`, `it[k] - epoch`, `emplace_back(north[k], east[k], down[k])`, `emplace_back(ha[k], ha[k], va[k])`, `emplace_back(sa[k], sa[k], sa[k])`, output time `epoch + dense.time[i]`.

Value-neutral removals that **are** made (decided here): the branch's experiment-only `earliest` / `latest` parameters of `initialize` (defaults +/-1e100 never bind: `max({0., x, -1e100}) == max({0., x})`), the `qInfo()` logging in `runFusion`, and `Config`'s use as a caller-supplied value (only `Tuning{}` with `maxGap` overwritten is ever used, plus the test seam of Task 8.7).

### Tuning constants to carry verbatim

| Group | Constants |
|---|---|
| Physical | gravity `(0, 0, 9.80665)` NED; `pi = 3.14159265358979323846` |
| `Tuning` defaults | `accDensity .015`, `gyroDensity .001`, `accBiasSigma .3`, `gyroBiasSigma .03`, `maxGap .025` (always overwritten by `1.6 * medianInterval(imuTime of the full recording)`), `relativeTolerance 1e-8`, `maxIterations 100` |
| Preintegration | covariances `I_3x3*density*density`; integration covariance `I_3x3*1e-8`; duration check `abs(deltaTij - (end-start)) > 1e-10` |
| Gaps | IMU `1.6` median intervals; GNSS `max(2., 5*medianInterval(full gnssTime))`; minimum 3 GNSS fixes and 2 IMU samples |
| Fit | at most `5` bias passes; LM `setLinearSolverType("MULTIFRONTAL_QR")`, `maxIterations` from tuning, every other LM parameter default; failure when `!isfinite(after) || after > before + 1e-6`; settled when `before - after <= relativeTolerance*max(1., before)`; converged when settled and accelerometer-bias shift norm `< 1e-5` and gyro-bias shift norm `< 1e-6`; checkpoint every `256` states; initial heading `0.` deg |
| Initializer scan | window `30` s on a `5` s grid; `first = max({0., ceil(imuTime.front()/5)*5})`; `last = min({imuTime.back(), gnssTime.back()})`; loop `s + 30 <= last + 1e-6`; score `fs.norm()` |
| Stationary window | at least `10` IMU and `5` GNSS samples and non-empty halves; IMU edge and gap `< 1.6` median; GNSS edge and gap `< .4` s; trimmed mean drops `size_t(.1*n)` per end; gates in this order: `velocity_variability` p95 `<= .3`, `uncertainty` p95 `<= .5`, `normalized_velocity_variability` p95 `<= 3.5`, `velocity_drift` `<= .3`, `gyro_variability` max std `<= .06` deg/s, `gyro_p95` `<= .15`, `gyro_peak` `<= .5`, `mean_rate` `<= 1.`, `force_variability` max std `<= .025`, `force_p95` `<= .08`, `force_peak` `<= .25`, `tilt_drift` `<= .03`, `plausible_gravity` in `(.7, 1.3)*9.80665` (the `coverage` gate precedes them) |
| Alignment | identity when either norm `< .1`; antiparallel branch when `dot < -1 + 1e-10` |
| Initial values | start-time agreement `1e-9`; accelerometer bias starts at zero, gyro bias at the initializer's |
| Strings | `"batch-shared-bias-v1"`; initializer methods `"coarse GNSS/force initialization; heading unknown"` and `"stationary initialization only; heading unknown"`; residual kinds `"position"`, `"velocity"`, `"imu"`, `"bias_prior"`; every failure message (captured in the goldens) |

## Tasks

### Task 8.1: Library skeleton, public header, CMake target

**Purpose:** Create `flysight_fusion` as the only product target that links GTSAM, with the public API above, so the remaining tasks fill in internals.

**Files to create:**
- `src/fusion/fusion.h` — exactly the API in "Public API", with `///` comments at the density of `src/engine/calculationresult.h` (what each member means, units, the purity statement, what `Cancelled` leaves empty).
- `src/fusion/fusion.cpp` — `run()`; `Detail::runPipeline()` arrives in Task 8.7 (until then a stub is not acceptable at phase end; tasks may be implemented in any order).
- `src/fusion/fusionprogress.h` — `class FusionCancelled {};` (comment: deliberately not a `std::exception`) and `class Checkpoint` holding const references or copies of the two callbacks, with `void operator()(const QString &text) const` as specified in "Progress and cancellation boundaries".

**Files to modify:**
- `src/CMakeLists.txt` — one additive hunk between the `flysight_core` block (ends line 310) and the `# ── application` banner (line 312).

**Technical Approach:**

```cmake
# ─────────────────────────────── sensor fusion library
# The batch GNSS/IMU factor-graph fit. The ONLY target that links GTSAM
# (PRIVATE: fusion/fusion.h is GTSAM-free, so consumers get the link
# dependency but none of GTSAM's headers or definitions). Pure: no engine, no
# session, no Qt GUI. Registration joins it in a later phase.
add_library(flysight_fusion STATIC
  fusion/fusion.cpp                    fusion/fusion.h
  fusion/fusionpipeline.h              fusion/fusionprogress.h
  ... (the pairs of the layout table)
)
target_include_directories(flysight_fusion PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(flysight_fusion PUBLIC Qt${QT_VERSION_MAJOR}::Core PRIVATE gtsam)
set_target_properties(flysight_fusion PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
if(MSVC)
  target_compile_options(flysight_fusion PRIVATE /EHsc /bigobj)
else()
  # Keep the kernel's own arithmetic IEEE-exact like the MSVC reference build
  # (GCC and Clang otherwise contract a*b+c into fma on targets that have it).
  target_compile_options(flysight_fusion PRIVATE -ffp-contract=off)
endif()
```

No other flag. In particular no `/fp:*`, `/arch:*`, `-O*`, `-march`, `-ffast-math`: the Release defaults are the branch's (see "Facts"). Includes inside the library are written relative to `src/` (`#include "fusion/fusionsamples.h"`), like `#include "engine/calctypes.h"` elsewhere. `FlySightViewer` does **not** link the library in this phase.

**Acceptance Criteria:**
- [ ] `fusion.h` includes no GTSAM, Eigen, engine, session or Qt GUI header (`grep -n "#include" src/fusion/fusion.h` shows only `<QString>`, `<QVector>`, `<QtGlobal>`/`<functional>`).
- [ ] `grep -rn "sessiondata\|engine/\|QApplication\|QWidget\|QProgressDialog\|PreferencesManager\|QSettings" src/fusion` finds nothing outside `src/fusion/fusionregistration.*` (those two files are added by Phase 9 and legitimately include `sessiondata.h` and engine headers; they do not exist yet if this phase lands first); `grep -rni "ekf" src/fusion tests/tst_fusion_*.cpp tests/fusion` finds nothing.
- [ ] In the generated build system the only product target with `gtsam` on its link line is `flysight_fusion`; `flysight_model`, `flysight_core`, `FlySightViewer`, `flysight_cpp_bridge` are unchanged (`git diff master -- src/CMakeLists.txt` shows one added hunk attributable to this phase; hunks of other phases in this shared file - Phase 1's and any other that has landed - are expected; overview, Integration Note 11).
- [ ] The library builds in Release with `FLYSIGHT_BUILD_TESTS=OFF` and with `FLYSIGHT_BUILD_FUSION_TESTS=OFF`.

**Complexity:** M

---

### Task 8.2: Shared unwrap helper

**Purpose:** Make "unwrapped with the same rule the GNSS course uses" (spec section 6) literally true by giving both one implementation.

**Files to create:**
- `src/calculations/anglehelper.h` — header-only, Qt Core only: `namespace FlySight::Calculations { inline QVector<double> unwrapDegrees(const QVector<double> &angles); }`, retyped from `branch:src/calculations/anglehelper.h` with a `///` comment stating the rule (shortest adjacent change, first angle kept, whole turns accumulate, exact half turn is not wrapped).

**Files to modify:**
- `src/calculations/gnsscalculations.cpp` — replace only the "Unwrap phase" block (lines 297-306) by `QVector<double> course = unwrapDegrees(rawDeg);` and add the include. `rawDeg` is never empty there (guarded at line 283).
- `src/CMakeLists.txt` — add `calculations/anglehelper.h` to the `flysight_core` source list (one line, next to `derivativehelper`).

**Technical Approach:** The arithmetic must stay `result.back() + delta` with `delta` adjusted by the two `if`s in that order. `flysight_fusion` includes the header (`"calculations/anglehelper.h"`); that is a header dependency only, not a link dependency on `flysight_core`.

**Acceptance Criteria:**
- [ ] `tst_builtins_golden`, `tst_builtins_engine` and every other existing test pass unchanged.
- [ ] `git diff master -- src/calculations/gnsscalculations.cpp` shows one include line and the ten-line loop replaced by one call, nothing else.
- [ ] `tst_fusion_kernel::unwrapRule` (Task 8.11) passes with the branch's literal vectors.

**Complexity:** S

---

### Task 8.3: Samples, validation, and the input adapter

**Purpose:** Retype every rule that decides whether a recording can be fitted, with the branch's order of checks and message texts, because the first failing check is the reported reason.

**Files to create:** `src/fusion/fusionsamples.h/.cpp`, `src/fusion/inputadapter.h/.cpp`.

**Technical Approach:**

Types (`fusionsamples.h`): `Vectors`; `struct Samples { std::vector<double> imuTime, gnssTime; Vectors force, gyro, position, velocity, positionSigma, velocitySigma; }` (times are seconds since the epoch, gyro rad/s); `struct Tuning` (defaults from the constants table); `extern const gtsam::Vector3 kGravity;` and `constexpr double kPi`. `inputadapter.h`: `struct PreparedInput { Samples recording; double epoch = 0; double usableStart = 0; QJsonObject audit; };`.

Decomposition map (none of these is a numeric kernel except the two conversion loops, rule 8):

| Branch | Port | Notes |
|---|---|---|
| `validTimes` | `requireIncreasingFiniteTimes(times)` | "At least two timestamps required"; "Timestamps must be finite and strictly increasing" |
| `medianInterval` | `medianInterval(times)` | validates, differences in order, `quantile(dt, .5)` |
| `validate` | `validateSamples(samples, tuning)` calling, in this order: `requireIncreasingFiniteTimes` (IMU, then GNSS); `requireFiniteArrays(count, fields)` for `{force, gyro}` then `{position, velocity, positionSigma, velocitySigma}` (per field: length first, then finiteness); `requirePositiveSigmas`; `requireValidTuning` (six doubles finite and `> 0`, then `maxIterations >= 1`); `requireGnssInsideImuCoverage`; `requireNoImuGapInsideGnssSpan` | the gap rule is `dt > maxGap && imuTime[i-1] < gnssTime.back() && imuTime[i] > gnssTime.front()`; message `"IMU gap at " + <t> + " s; fusion unavailable across missing data"` with `<t>` = `QString::number(imuTime[i-1], 'f', 6)` (identical to `std::to_string` in the C locale, but not locale-dependent) |
| `window` | `fittedWindow(samples, start, end)` with `gnssInsideWindowAndCoverage(...)` and `imuRangeCovering(...)` | GNSS fixes with `start <= t <= end` and inside IMU coverage; fewer than 3: "Fewer than three GNSS fixes in IMU coverage"; IMU from one sample before the first kept fix (`lower_bound`, then `--lo` when non-zero) to one past `lower_bound(last fix)`, clamped |
| top of `runFusion` | `requireUsableRecording(prepared)` | order: counts (`gnss < 3 || imu < 2`: "Sensor fusion needs at least three GNSS fixes and two IMU samples"); finite `epoch` and `usableStart`; arrays IMU then GNSS ("Mismatched fusion input array lengths", "Nonfinite fusion input"); sigmas |
| GNSS-gap loop in `runFusion` | `requireNoGnssOutage(window, limit)` | "GNSS gap: fusion unavailable for a disconnected session" |
| `prepareFusionInput` | `prepareInput(const Channels &)` with `requireChannel(label, values, count)`, `appendGnssSamples`, `appendImuSamples`, `inputAudit` | below |

`prepareInput` order (each step is the branch's): (1) `gnssTime.size() < 3 || imuTime.size() < 2`: "Sensor fusion needs GNSS, IMU and shared UTC time conversion"; (2) `originIndex < 0 || originIndex >= ng`: "Local origin index outside GNSS samples"; (3) `requireChannel` for, in this order and with these labels, `Local/north`, `Local/east`, `Local/down`, `Local/velN`, `Local/velE`, `Local/velD`, `GNSS/hAcc`, `GNSS/vAcc`, `GNSS/sAcc` (count `ng`), `IMU/ax`, `IMU/ay`, `IMU/az`, `IMU/wx`, `IMU/wy`, `IMU/wz` (count `ni`): size mismatch gives `"Missing or mismatched " + label`, a non-finite value `"Nonfinite " + label`; (4) `epoch = gnssTime.front()`, `usableStart = gnssTime[originIndex] - epoch`; (5) GNSS loop: non-finite time "Nonfinite GNSS UTC timestamp", any of `hAcc|vAcc|sAcc <= 0` "GNSS measurement sigmas must be positive", then the conversions of rule 8; (6) IMU loop: "Nonfinite IMU UTC timestamp", conversions with `radians`; (7) `medianInterval(imuTime)` then `medianInterval(gnssTime)` for their checks; (8) audit object with keys `epoch_utc_s`, `imu_count` (int), `gnss_count` (int), `origin_index` (int), `origin` (array lat, lon, hMSL), `height_method` = "CSV hMSL used as approximate ellipsoid height in GeographicLib local frame", `time_method` = "SessionData shared UTC conversion; common epoch subtraction only" (verbatim: spec section 6 says the diagnostics content is as on the branch).

The branch's "No shared local origin (...)" rejection is not representable with a plain `qint64` and is not ported: in the engine a missing origin attribute is a missing input (spec 7.2), so the kernel never runs.

**Acceptance Criteria:**
- [ ] Every message literal listed above appears verbatim in `src/fusion/` (the goldens assert the nine that fixtures reach).
- [ ] `tst_fusion_kernel::validationRejectsEachDefect` and `tst_fusion_parity::rejectionFixturesMatchGolden` pass.
- [ ] No function in these files is longer than about 40 lines; each has a `///` or `//` comment stating the rule it enforces and why (for example why GNSS outside IMU coverage is trimmed but an IMU gap is fatal).

**Complexity:** M

---

### Task 8.4: Statistics and IMU integration

**Purpose:** Retype the numeric helpers every later stage uses.

**Files to create:** `src/fusion/samplestatistics.h/.cpp`, `src/fusion/imuintegration.h/.cpp`.

**Technical Approach (all kernels):**

| Branch | Port | Notes |
|---|---|---|
| `quantile(std::vector<double> a, double q)` | `quantile(values, q)` | by value, sorted inside; "Empty statistic"; `index = q*(n-1)`, `i = size_t(index)`, `j = min(i+1, n-1)`, `a[i] + (a[j]-a[i])*(index-i)` |
| `mean(Vectors, trimmed)` | `componentMean(values, trimmed = false)` | per component: copy, sort only when trimmed, `n = trimmed ? size_t(.1*v.size()) : 0`, `accumulate(begin+n, end-n, 0.) / (v.size() - 2*n)` |
| `stddev` | `componentStddev(values)` | population form, `(v / double(n)).cwiseSqrt()` |
| `edges` | `integrationEdges(samples, start, end, includeGnss = false)` | "Integration outside IMU coverage"; `{start} + IMU times in (start, end) [+ GNSS times in (start, end), sort, unique] + {end}` using `upper_bound(start)` / `lower_bound(end)` |
| `interpolate` | `interpolateAt(times, values, t)` | "Interpolation outside coverage"; returns `v[j]` when `j == 0` or exact hit; else `v[j-1] + f*(v[j]-v[j-1])` with `f = (q-t[j-1])/(t[j]-t[j-1])` |
| the expression `(interpolate(imuTime, gyro, (e[i]+e[i-1])/2) - bias) * (e[i]-e[i-1])` (three sites) | `gtsam::Vector3 gyroIncrement(samples, from, to, gyroBias)` returning exactly that expression with `to = e[i]`, `from = e[i-1]` | the sanctioned exception to rule 1; `bias` is a `Vector3` at every site (`bias.gyroscope()` in reconstruction) |
| `preintegrate` | `preintegrateImu(samples, start, end, bias, tuning)` with `preintegrationParams(tuning)` | midpoint force and gyro per edge pair, `integrateMeasurement(force, gyro, e[i]-e[i-1])`; "Preintegration duration mismatch" |
| `propagate` | `propagateAttitude(samples, rotation, start, end, gyroBias)` with `requireNoImuGap(edges, limit)` and `gyroIncrements(...)` | returns `rotation` unchanged when `start == end`; gap check on IMU-only edges with `1.6*medianInterval(imuTime)` ("Anchor propagation cannot bridge an IMU gap"); increments on edges **including GNSS times**; forward: compose in order; backward: reverse order with negated increments |

**Acceptance Criteria:**
- [ ] `tst_fusion_kernel::preintegrationHonoursExactBoundaries` and `::backwardPropagationUndoesForward` pass with the branch's literals and tolerances (`1e-10`, `1e-12`).
- [ ] A reviewer comparing each function body with its branch origin finds the same arithmetic statements in the same order (rules 1-8).

**Complexity:** M

---

### Task 8.5: Stationary window and initializer

**Purpose:** Retype the search for a stationary 30 s window, the gravity-anchored attitude, the coarse fallback, and the initial values.

**Files to create:** `src/fusion/stationarywindow.h/.cpp`, `src/fusion/initializer.h/.cpp`.

**Technical Approach (kernels):**

`assessStationaryWindow(samples, start, end) -> StationaryWindow` (`accepted`, `start`, `end`, `score`, `imuCount`, `gnssCount`, `forceMean`, `gyroMean`, `rejected`), decomposed as: `collectImuWindow` (times, rate in deg/s as `gyro*180/pi`, force, first-half and second-half force, half split at `(start+end)/2` evaluated per sample as on the branch); `collectGnssWindow` (times, velocity, velocity sigma, halves, flattened sigma components); `hasMinimumSamples` (early return with `rejected = {"coverage"}`); `deviationNorms(values, centre)`; `largestInterval(times)`; then the gates of the constants table appended to `rejected` **in the branch's order**, each gate one named predicate or one commented line; finally `accepted`, `forceMean = fm`, `gyroMean = wm*pi/180`, `score = fs.norm()`. Keep the branch's comment that constant translation is compatible with a gravity-based initialization; it explains why velocity is tested relative to its trimmed mean.

`initializer.cpp`: `rotationAligning(from, to)` (branch `align`; the quaternion is built from `(1+dot, c.x(), c.y(), c.z())` and normalized); `bestStationaryWindow(samples)` (the scan loop; rule 3 for `s += 5`, rule 7 for the strict `<`); `attitudeFromStationaryWindow(samples, window, graphStart)` (anchor = GNSS time nearest the window middle by `min_element`; `gyroBias = window.gyroMean`; `rotation = propagateAttitude(samples, rotationAligning(window.forceMean, -kGravity), anchor, graphStart, gyroBias)`; method string); `coarseAttitude(samples, graphStart)` (`k = lower_bound(gnssTime, graphStart)`; `k+1 >= size`: "No GNSS initializer interval"; `a = (velocity[k+1]-velocity[k])/(gnssTime[k+1]-gnssTime[k])`; `rotationAligning(interpolateAt(imuTime, force, graphStart), a - kGravity)`); `initialAttitude(fullRecording, graphStart)` choosing between them; `initialValues(window, headingDeg, attitude)` ("Initialization must be propagated to graph start with finite bias"; `B(0)`, then per state the attitude propagated across the interval with `gyroIncrement`, `X(k) = Pose3(r, position[k])`, `V(k) = velocity[k]`).

Note the asymmetry the branch has and the port keeps: the initializer scans the **full** recording, the graph covers the **window**.

**Acceptance Criteria:**
- [ ] `tst_fusion_kernel::stationaryGates` (all ten branch cases) and `::shortInputUsesCoarseInitializer` pass.
- [ ] Every gate threshold in the constants table appears once, as a named constant or a commented literal, in `stationarywindow.cpp`.

**Complexity:** M

---

### Task 8.6: Factor-graph fit

**Purpose:** Retype graph construction and the Levenberg-Marquardt passes with bias re-integration, with the checkpoints B2 and B3.

**Files to create:** `src/fusion/factorgraphfit.h/.cpp`.

**Technical Approach (kernels):**

- `buildFactorGraph(samples, bias, tuning, checkpoint)`: loop over GNSS states; `if (k % 256 == 0) checkpoint("Integrating IMU factors")` first; `addGnssFactors(graph, samples, k)` (GPSFactor then velocity prior, `noiseModel::Diagonal::Sigmas`); for `k > 0` `addImuFactor(...)` with `preintegrateImu(samples, gnssTime[k-1], gnssTime[k], bias, tuning)`; after the loop `addBiasPrior(graph, tuning)` (`Vector6` of three `accBiasSigma` then three `gyroBiasSigma`, prior mean `imuBias::ConstantBias()`).
- `fitFactorGraph(samples, headingDeg, tuning, attitude, checkpoint) -> FitResult`: `validateSamples` again (kept; it cannot fail a second time and costs nothing); `initialValues`; up to five passes of `runOptimizerPass(graph, values, tuning, pass, checkpoint, history) -> settled` (per iteration: `checkpoint("Pass P, iteration I")`, `before = error()`, `iterate()`, `after = error()`, record `{outer, iteration, before, after}`, the failure test, the settled test), then `values = optimizer.values()`, the bias shift `Vector6`, and `biasSettled(shift)`; after the passes a final `buildFactorGraph` at the fitted bias (it checkpoints too), `objective = graph.error(values)`, and `collectResiduals` (factor index walks the insertion order: position, velocity, then `imu` for `k > 0`; `bias_prior` last with node 0 and `gnssTime[0]`; each `2*graph.at(i)->error(values)`; RMS sums in the same loop, then `sqrt(sum / n)`).
- `FitResult`: `values`, `converged`, `objective`, `heading`, `positionRms`, `velocityRms`, `history`, `residuals`.
- Non-convergence is **not** thrown here (as on the branch): `fitFactorGraph` returns `converged == false`, and the pipeline turns it into "Batch fusion did not converge; sensor fusion unavailable".

**Acceptance Criteria:**
- [ ] `tst_fusion_kernel::headingIsUnconstrained`, `::exactConstantVelocityFit`, `::fitTraceMatchesGolden` pass.
- [ ] The checkpoint texts and places match the boundary table; `grep -n "checkpoint(" src/fusion/*.cpp` finds exactly three call sites (B1 in `fusion.cpp`, B2 and B3 here).

**Complexity:** M

---

### Task 8.7: Reconstruction, output assembly, diagnostics, and the pipeline

**Purpose:** Retype the dense trajectory, turn it into the seventeen channels and the diagnostics JSON, and compose the stages into `run()` with the outcome classification.

**Files to create:** `src/fusion/trajectoryreconstruction.h/.cpp`, `src/fusion/fusionoutput.h/.cpp`, `src/fusion/fusionpipeline.h`; complete `src/fusion/fusion.cpp`.

**Technical Approach:**

*Reconstruction (kernel).* `reconstructTrajectory(samples, fit) -> DenseTrajectory { time, endpointCorrection, rotation, acceleration, position, velocity }`, per GNSS interval: `propagateThroughInterval` (rotations at every edge, by `gyroIncrement` with the fitted gyro bias); `error = Rot3::Logmap(p1.rotation().compose(r.inverse()))`; `endpointCorrection.push_back(error.norm()*180/pi)`; then for every original IMU sample in `[start, end)` (`lower_bound` both ends): `fraction = (t-start)/(end-start)`, `j = lower_bound(edges, t)`, `adjusted = Rot3::Expmap(error*fraction).compose(rotations[j])`, acceleration `adjusted.rotate(force[index]-bias.accelerometer())+kGravity`, position `(1-fraction)*p0.translation()+fraction*p1.translation()`, velocity `(1-fraction)*v0+fraction*v1`. Keep the branch's comment that position and velocity are display-only linear interpolation of optimized GNSS states.

*Channels (kernel).* `channelsFrom(dense, epoch, Result &)`: per sample `time = epoch + dense.time[i]`; `rpy = dense.rotation[i].rpy()*180/3.14159265358979323846`; `q = toQuaternion()`; append in the branch's order. Output names follow the spec (`north/east/down`, not `posN/posE/posD`). Then `roll`, `pitch`, `yaw` are replaced by `Calculations::unwrapDegrees(...)` of themselves, over the whole fit.

*Diagnostics.* `successDiagnostics(prepared, attitude, fit, window, dense) -> QJsonObject` with exactly the branch's keys and value types: `algorithm`, `input` (the audit), `seeds` (one object: `heading_deg`, `converged`, `objective`, `iterations` = `int(history.size())`, `acc_bias_m_s2`, `gyro_bias_rad_s`, `position_residual_rms_m`, `velocity_residual_rms_m_s`), `initialization`, `stationary_interval_s`, `anchor_time_s`, `selected_heading_deg`, `objective`, `start_s`, `end_s`, `gnss_states`, `imu_outputs`, `seed_comparison_performed` (false), `max_seed_vs_selected_angle_deg` (null), `max_seed_vs_selected_acceleration_m_s2` (null), `max_endpoint_correction_deg`, `display_position_velocity`, `orientation`, `limitations` (the three strings verbatim from `branch:src/imugnssekf.cpp`), `residuals` (array of `kind`, `node`, `time_s`, `squared_whitened_error`). `failureDiagnostics(reason)`: the two-key object. `toCompactJson(object)` = `QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact))`. Split into small builders (`seedSummary`, `residualArray`, ...).

*Pipeline.* `fusionpipeline.h`: `struct PipelineTrace { InitialAttitude attitude; std::vector<FitIteration> history; bool converged = false; };` and `Result runPipeline(const Channels &, const Tuning &baseTuning, const Checkpoint &, PipelineTrace *trace = nullptr);`. Body, in the branch's order: stage 1 (`Rejected` on exception): `prepareInput`; `requireUsableRecording`; `medianInterval(full.gnssTime)`; `tuning.maxGap = 1.6*medianInterval(full.imuTime)`; `window = fittedWindow(full, usableStart, full.gnssTime.back())`; `validateSamples(window, tuning)`; `requireNoGnssOutage(window, std::max(2., 5*medianInterval(full.gnssTime)))`; `attitude = initialAttitude(full, window.gnssTime.front())`. Stage 2 (`SolverFailed` on exception): `checkpoint("Starting fit")`; `fit = fitFactorGraph(window, kInitialHeadingDeg, tuning, attitude, checkpoint)`; not converged: throw the non-convergence message; `reconstructTrajectory`; channels; diagnostics. `run()` builds a `Checkpoint` and calls `runPipeline(channels, Tuning{}, checkpoint)`. `baseTuning` exists only so a test can force non-convergence (`maxIterations = 1`); say so in its comment. No logging anywhere in the library.

**Acceptance Criteria:**
- [ ] `tst_fusion_kernel::reconstructionTimingAndEndpointCorrection` passes (83 samples, first `.04`, last `.86`, distributed correction within `1e-12`).
- [ ] `tst_fusion_parity` passes in full (Task 8.10).
- [ ] A `Cancelled` result has every array empty, `reason` and `diagnosticsJson` empty; `Rejected` and `SolverFailed` have empty arrays and a diagnostics object with exactly the keys `algorithm` and `failure`.
- [ ] `std::bad_alloc` is rethrown from both stages (by inspection: the catch order is `FusionCancelled`, `std::bad_alloc`, `std::exception`).

**Complexity:** M

---

### Task 8.8: Deterministic fixture generators

**Purpose:** Define the synthetic recordings once, in code that both the capture harness (compiled against the branch) and the ported tests compile, so the inputs on both sides are the same bits.

**Files to create:** `tests/fusion/fusionfixtures.h/.cpp` — namespace `FlySightTest`, Qt Core and the C++ standard library only; **no** include from `src/` (the harness compiles this file against the branch, where `fusion/fusion.h` does not exist).

**Technical Approach:**

```cpp
struct FusionFixture {
    QString name;                                  // also the golden file stem
    QVector<double> gnssTime, north, east, down, velN, velE, velD, hAcc, vAcc, sAcc;
    QVector<double> imuTime, ax, ay, az, wx, wy, wz;   // wx..wz in deg/s
    qint64 originIndex = 0;
    double originLat = 45.0, originLon = -75.0, originHMSL = 100.0;
    bool expectSuccess = true;
};
QList<FusionFixture> fusionFixtures();             // all twelve, in the order below
FusionFixture fusionFixture(const QString &name);
```

Rules for the generators, so they give identical bits with any IEEE-754 compiler: only `+ - * /` and `std::sqrt` (no `sin`, `cos`, `pow`, `exp`); every sample computed from its index (`t = i * .01`), never by accumulation; noise from SplitMix64 (`z = (state += 0x9E3779B97F4A7C15); z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9; z = (z ^ (z >> 27)) * 0x94D049BB133111EB; z ^= z >> 31;`), mapped to `[-1, 1)` as `double(z >> 11) * 0x1.0p-53 * 2 - 1`, drawn in a fixed documented order (per sample: channels in struct order); no `<random>` distribution. UTC epoch `1700000000.0`; all times are `epoch + t`.

Success fixtures (intended parameters; see the adjustment rule below):

| Name | Content | Exercises |
|---|---|---|
| `coarse_linear` | the branch self-test's `linear`: IMU `t = i*.01`, `i = 0..200`, `a = (0, 0, -9.80665)`, `w = 0`; GNSS `t = .037 + i*.2`, `i = 0..8`, position `(7,8,9) + t*(12,-4,2)`, velocity `(12,-4,2)`, `hAcc = vAcc = 1`, `sAcc = .1`; origin index 0; no noise | coarse initializer, near-zero objective, boundary timing |
| `coarse_maneuver` | 6 s. IMU `i = 0..600` at `.01`. GNSS `t = -.163 + j*.2`, `j = 0..32` (one fix before and two after IMU coverage: trimmed, not rejected); origin index 3, `hAcc = 12` for `j < 3` else `1.5`, `vAcc = 2.5`, `sAcc = .3`. NED acceleration `(1.5 - .4t, .8t, -.6 + .1t^2)`, `v0 = (20,-5,3)`, velocity and position its exact polynomial integrals, attitude identity, so force `= a - (0,0,9.80665) + (.05,-.03,.08)`, gyro `= (.2,-.15,.3)` deg/s. Uniform noise amplitudes: force `.02`, gyro `.05` deg/s, position `.3`, velocity `.1`; seed `0x8F050002` | window trimming, `usableStart > 0`, several LM iterations and bias passes, non-zero biases and residuals |
| `stationary_spin` | 40 s. IMU `i = 0..1000` at `.04` (25 Hz keeps the golden small); GNSS `t = .1 + j*.2`, `j = 0..199`; position and velocity zero; force `(0,0,-9.80665) + (.03,-.02,.05)`; gyro `(.2,-.1,.15)` deg/s plus 90 deg/s on `wz` for `t >= 31`; `hAcc = 1`, `vAcc = 1.5`, `sAcc = .1`; noise force `.005`, gyro `.02`, position `.2`, velocity `.03`; seed `0x8F050003` | stationary window `[0, 30)` accepted and `[5, 35)` rejected, backward anchor propagation, yaw past 360 degrees (unwrap) |

Rejection fixtures, each one mutation (one per rejection class of spec section 6 plus the rest of the branch's validation):

| Name | Base and mutation | Expected reason (captured, listed for orientation) |
|---|---|---|
| `reject_nonfinite` | `coarse_linear`, `ax[10] = NaN` | Nonfinite IMU/ax |
| `reject_time_order` | `coarse_linear`, `imuTime[4] = imuTime[3]` | Timestamps must be finite and strictly increasing |
| `reject_too_few_fixes` | `coarse_linear`, GNSS channels truncated to 2 | Sensor fusion needs GNSS, IMU and shared UTC time conversion |
| `reject_coverage` | `coarse_linear`, IMU channels truncated to the first 36 samples | Fewer than three GNSS fixes in IMU coverage |
| `reject_imu_gap` | `coarse_linear`, IMU samples 40..49 removed | IMU gap at 0.353000 s; fusion unavailable across missing data |
| `reject_gnss_gap` | `coarse_maneuver`, GNSS fixes `j = 12..23` removed (2.6 s > max(2 s, 5 x .2 s)) | GNSS gap: fusion unavailable for a disconnected session |
| `reject_sigma` | `coarse_linear`, `sAcc[0] = 0` | GNSS measurement sigmas must be positive |
| `reject_length` | `coarse_linear`, last `velE` sample removed | Missing or mismatched Local/velE |
| `reject_origin` | `coarse_linear`, `originIndex = 9` | Local origin index outside GNSS samples |

*Adjustment rule.* These parameters could not be run while this document was written. If the **reference** (Task 8.9) does not converge on a fixture meant to succeed, change that fixture only, in this order, until it does: halve its noise amplitudes (repeat down to zero), then halve its injected biases, then (spin only) raise the IMU rate to 50 Hz. If a rejection fixture is rejected by a different rule than intended, fix the mutation so that it reaches the intended rule. Record the final parameters in the generator's comments and in `tests/README.md`. Never adjust a fixture to make the *port* pass.

**Acceptance Criteria:**
- [ ] `fusionfixtures.cpp` includes nothing from `src/` and calls no transcendental function.
- [ ] Two calls of `fusionFixtures()` give bit-identical arrays (`tst_fusion_parity::fixturesAreDeterministic`).
- [ ] The three success fixtures reach `Succeeded` and each rejection fixture its intended rule **on the reference**.

**Complexity:** M

---

### Task 8.9: Golden capture from `sensor-fusion-clean-port`

**Purpose:** Produce the committed golden data from the branch's own code, built against the shipping (Boost-free, upstream) GTSAM, outside the working tree, by a procedure that can be repeated; and prove, before accepting that data, that it is byte-identical to what the same code produces against the Boost-enabled GTSAM the branch was validated with.

**Files to create (committed):**
- `tests/data/fusion/capture.json` — provenance (below).
- `tests/data/fusion/<fixture>.json` — one per fixture (twelve).
- `tests/data/fusion/<fixture>.channels.txt` — success fixtures only (three).

**Files created outside the repository (never staged):** the scratch tree and the harness.

**Technical Approach:**

1. *Scratch tree, read-only git.* Choose a directory outside the working tree, e.g. `C:/Users/crwpe/Desktop/flysight-fusion-golden/`. Export the branch sources without touching repository state: `git archive --format=tar 83a64479fd4e7e2e10bce0b5477c5dd7a49dee7d src | tar -x -C <scratch>/branch` (first confirm `git rev-parse sensor-fusion-clean-port` still prints that SHA; if not, stop and report). This is Decision 7's "separate tree outside the working tree", done with `git archive` because `git worktree add` writes under `.git/`.
2. *Harness* at `<scratch>/harness/`: a `CMakeLists.txt` and one `main.cpp`.
   - The harness has **its own minimal `CMakeLists.txt`**. It does not use the branch's `cmake/SolverDependencies.cmake`, `SolverSuperbuild.cmake` or `src/CMakeLists.txt` (they assume a Boost-enabled GTSAM and the branch's install layout), and it does not use `master`'s `cmake/SolverDependencies.cmake` either (its Boost-free guard would refuse the old install, which the cross-check needs). Only `<scratch>/branch/src/*.cpp|*.h` comes from the branch.
   - CMake: C++17, `find_package(Qt6 COMPONENTS Core)`, `find_package(GTSAM 4.3 CONFIG REQUIRED)`; two cache paths `HARNESS_GTSAM_PREFIX` and `HARNESS_TBB_PREFIX` are prepended to `CMAKE_PREFIX_PATH` next to Qt, so one source tree configures against either solver. `if(MSVC) set(Boost_USE_STATIC_LIBS ON) endif()` before `find_package(GTSAM)`: it has no effect with the Boost-free install (its config never looks for Boost) and is what the Boost-enabled install's `find_dependency(Boost …)` needs; `-DBOOST_ROOT=…` is passed only to the Boost-enabled configure. After `find_package`, print the `gtsam` target's `INTERFACE_LINK_LIBRARIES` and the two Boost macro lines of the found `config.h` with `message(STATUS)`, so each build log states which solver it used. `AUTOMOC ON`; sources `<scratch>/branch/src/{batchfusion,imugnssekf,fusioninput,sessiondata,dependencymanager,calculatedvalue}.cpp`, `<repo>/tests/fusion/fusionfixtures.cpp`, `main.cpp`; include directories `<scratch>/branch/src` and `<repo>/tests/fusion`; link `gtsam Qt6::Core`; on MSVC `target_compile_options(... PRIVATE /EHsc /bigobj)` and `target_link_options(... PRIVATE /STACK:67108864)`. **No other flag**, Release configuration, Visual Studio 17 2022 x64: this reproduces the branch target's compile line in "Facts".
   - `main.cpp <output dir>`: for each fixture, build a branch `SessionData` by storing `GNSS/_time`, `IMU/_time`, `Local/north|east|down|velN|velE|velD`, `GNSS/hAcc|vAcc|sAcc`, `IMU/ax|ay|az|wx|wy|wz` with `setMeasurement` and `_LOCAL_ORIGIN_INDEX` (`qlonglong`), `_LOCAL_ORIGIN_LAT|LON|HMSL` with `setAttribute`; then do what the branch's calculation did around the kernel, and nothing else: `try { input = prepareFusionInput(session); out = runFusion(input, recordingProgress); out.roll = unwrapDegrees(out.roll); (pitch, yaw likewise) } catch (const std::exception &e) { diagnostics = {{"algorithm","batch-shared-bias-v1"},{"failure", e.what()}}; }`. For success fixtures also compute the *trace* by calling the branch's pieces the way `runFusion` does (`Config c; c.maxGap = 1.6*medianInterval(full.imuTime); d = window(full, usableStart, full.gnssTime.back()); init = initialize(full, d.gnssTime.front()); r = solve(d, 0., c, init)`).
   - **Shipping build:** configure `<scratch>/harness/build-noboost` with `-DHARNESS_GTSAM_PREFIX=<repo>/build-solver-deps/GTSAM-install -DHARNESS_TBB_PREFIX=<repo>/build-solver-deps/oneTBB-install`, with `BOOST_ROOT` unset; run with `PATH` = Qt `bin`, `build-solver-deps/GTSAM-install/bin`, `build-solver-deps/oneTBB-install/bin` and nothing else solver-related. Run it **twice** into two directories (`out-noboost-1`, `out-noboost-2`) and require byte-identical output before going on (reference determinism with TBB on). If the two runs differ, stop and report: the parity policy below depends on it.
   - **Cross-check build (Boost-enabled, Windows only):** configure `<scratch>/harness/build-boost` from the *same* harness sources with `-DHARNESS_GTSAM_PREFIX=<repo>/third-party/GTSAM-install -DHARNESS_TBB_PREFIX=<repo>/third-party/oneTBB-install -DBOOST_ROOT="C:/Program Files/Boost/boost_1_87_0"` (plus `-DBOOST_LIBRARYDIR=…/stage/lib` if FindBoost needs it); same generator, same Release configuration, same flags. Run with `PATH` = Qt `bin`, `third-party/GTSAM-install/bin`, `third-party/oneTBB-install/bin` — never both solvers on one `PATH`. Run it twice as well (`out-boost-1`, `out-boost-2`), byte-identical. Before each run, confirm which `gtsam.dll` the process will load (`where gtsam.dll` with that `PATH`, or the harness prints `GTSAM_ENABLE_BOOST_SERIALIZATION` / `GTSAM_USE_BOOST_FEATURES` from `<gtsam/config.h>` as its first output line — to `stdout`, not into the golden files).
   - **Cross-check rule.** Compare `out-noboost-1` with `out-boost-1` file by file (`fc /b`, `cmp`, or a hash of every file): all twelve `<fixture>.json` and all three `<fixture>.channels.txt`. `capture.json` is written afterwards and is not part of the comparison.
     - **Identical for every fixture:** record that fact (below) and commit the `out-noboost-1` files as the goldens.
     - **Any difference, in any file, of any size: STOP and ask Michael.** Do not commit goldens, do not pick one of the two outputs, do not loosen anything, do not continue to Task 8.10. Report, per differing fixture, the first differing channel/sample or JSON key, both values, and the worst absolute and relative difference. This is the spec's "if a change to the numerical kernels seems necessary, stop and ask" applied to the solver configuration: the choice between the two configurations (Phase 1, Appendix B is the way back to Boost) is his. Tasks 8.1-8.8 and the harness itself are unaffected and need not be redone whichever way he decides.
     - If the old install turns out to be unusable (missing, will not configure), that is also a stop-and-report, not a reason to skip the cross-check.
3. *Golden formats.*
   - `<fixture>.json` (written with `QJsonDocument::Indented`; Qt writes doubles in shortest round-trip form, so every number is exact): `{"fixture": name, "outcome": "succeeded" | "rejected", "diagnostics": <the branch's diagnostics object, as an object>, "progress": [branch progress texts in order], "trace": {"method", "interval_s", "anchor_time_s", "gyro_bias_rad_s", "start_quaternion_xyzw", "converged", "history": [[outer, iteration, before, after], ...]}, "rows": n, "channels_file": "<fixture>.channels.txt"}`; rejection files have `fixture`, `outcome`, `diagnostics` and an empty `progress`. Objective, biases, RMS values, residuals, the input audit and the initializer description are all inside `diagnostics`; nothing is recorded twice.
   - `<fixture>.channels.txt`: line 1 `# flysight fusion golden channels v1`; line 2 `# columns: _time north east down velN velE velD accN accE accD roll pitch yaw qx qy qz qw` (branch `posN/posE/posD` are written under their new names; roll, pitch, yaw are the **unwrapped** values); then one line per output sample, seventeen space-separated 16-digit upper-case hexadecimal IEEE-754 bit patterns (`quint64` of the double). Exact by construction, locale- and line-ending-proof, and about 290 bytes per sample: roughly 50 KB + 160 KB + 290 KB for the three fixtures. Budget for `tests/data/fusion/`: under 1 MB in total; if an adjusted fixture would exceed it, shorten or slow that fixture rather than dropping channels.
   - `capture.json`: branch SHA; capture date; OS and CPU model; MSVC toolset and compiler version; the exact `cl` command line of `batchfusion.cpp` from the harness's `CL.command.1.tlog`; under `"solver"`: GTSAM version string, repository and revision (`https://github.com/borglab/gtsam.git`, `814a734...`; or the fork pair if Phase 1's fallback A was taken), the GTSAM option values of Phase 1's table read from `build-solver-deps/gtsam-build/CMakeCache.txt` (including `GTSAM_ENABLE_BOOST_SERIALIZATION=OFF`, `GTSAM_USE_BOOST_FEATURES=OFF`), `GTSAM_USE_TBB` and the two Boost macros (`0`) from the install's `include/gtsam/config.h`, the install prefix used; oneTBB and Qt versions (no Boost version: none is involved); the statement that two consecutive runs were byte-identical; and under `"cross_check"`: `"reference": "Boost-enabled GTSAM, fork commit 8938b9f (upstream 814a734 plus a GeographicLib CMake line), Boost 1.87 static, third-party/GTSAM-install"`, its two Boost macros (`1`), its option values from `third-party/gtsam-build/CMakeCache.txt`, the `cl` command line of `batchfusion.cpp` from *that* build's tlog, `"files_compared"` (15), `"byte_identical": true`, and the date. `capture.json` with `"byte_identical": false` is never committed (see the cross-check rule).
4. *Sanity before committing:* `coarse_linear`'s objective is tiny (the branch self-test asserts `< 1e-12` for exact times; with UTC-sized timestamps expect a little more), `stationary_spin`'s `initialization` is the stationary method with `stationary_interval_s = [0, 30]` and its yaw column spans more than 360 degrees, `coarse_maneuver`'s `gnss_states` is 28, and each rejection reason is the intended rule.
5. *Procedure text* goes into `tests/README.md` (Task 8.12), including the harness's `CMakeLists.txt` and `main.cpp` reproduced verbatim in fenced blocks, since the harness itself is not committed (Decision 7, Commit Policy plan-specific notes).

**Acceptance Criteria:**
- [ ] `tests/data/fusion/` holds `capture.json`, twelve `<fixture>.json` and three `.channels.txt`, under 1 MB together; nothing else from the scratch tree is in the repository (`git status --porcelain` shows no harness source, no branch source).
- [ ] `capture.json` records the SHA `83a64479...`, the compiler version, the full compile line (containing `/O2`, `/fp:precise`, no `/arch`, no `/fp:fast`), the GTSAM repository, revision and options of the **Boost-free** build (both Boost options `OFF`, both macros `0`), and the byte-identical double run.
- [ ] The goldens were produced by the harness built against `build-solver-deps/GTSAM-install`; the harness log of that build shows a `gtsam` link interface without `Boost::`.
- [ ] Cross-check: the same harness built against the old Boost-enabled `third-party/GTSAM-install` produced, for all twelve `<fixture>.json` and all three `.channels.txt`, files byte-identical to the goldens; `capture.json` records this under `"cross_check"` and the implementation report quotes the comparison command and its result. If the outputs were not identical, this criterion is not "failed and worked around": the phase stopped at this point and Michael was asked.
- [ ] `third-party/GTSAM-install`, `third-party/oneTBB-install` and `build-solver-deps/` are unmodified by this task (the harness builds and writes only under `<scratch>`).
- [ ] The implementation report states the final fixture parameters if the adjustment rule was used.

**Complexity:** L

---

### Task 8.10: Golden loader and `tst_fusion_parity`

**Purpose:** Prove spec acceptance 4 through the public API only, plus the rejection, cancellation, progress and determinism behavior Phase 9 relies on.

**Files to create:**
- `tests/fusion/fusiongolden.h/.cpp` — `struct FusionGolden { QString outcome; QJsonObject diagnostics; QStringList progress; QJsonObject trace; QHash<QString, QVector<double>> channels; }`; `FusionGolden loadFusionGolden(const QString &name)` (directory from the compile definition `FLYSIGHT_FUSION_GOLDEN_DIR`; tolerates CRLF; fails loudly on a malformed file); `bool exactParityRequested()` (`qEnvironmentVariableIntValue("FLYSIGHT_FUSION_EXACT") == 1`); `QString compareSamples(name, got, golden)`, `QString compareJson(path, got, golden)` returning an empty string or a description of the first difference plus the worst one (index, both values, absolute and relative difference); `Fusion::Channels toChannels(const FusionFixture &)` (field-by-field copy).
- `tests/tst_fusion_parity.cpp` — class `FusionParityTest`, `FLYSIGHT_TEST_MAIN`, `#include "tst_fusion_parity.moc"`.

**Technical Approach:**

*Tolerance policy.*
- **Exact mode** (`FLYSIGHT_FUSION_EXACT=1`): every sample `sameBits` with the golden; `diagnosticsJson` byte-equal to `QJsonDocument(golden.diagnostics).toJson(Compact)`. This is the mode that decides acceptance of the port, run by the implementer and the reviewer on the capture configuration (this machine, Release, the Boost-free GTSAM install in `build-solver-deps/` that the goldens were captured against). With the same compiler, flags and GTSAM binary, `/fp:precise` on x64 (SSE2 scalar arithmetic, no contraction) and the rules above, restructuring into functions cannot change a bit; a difference is a transcription defect and is to be found (the `trace` of Task 8.9 and `tst_fusion_kernel::fitTraceMatchesGolden` locate the first diverging stage), not tolerated. Should a difference survive that the implementer can attribute to the compiler alone, it must be explained statement by statement in the report and in `tests/README.md`, and may not exceed `1e-12` relative.
- **Portable mode** (default; CI on all platforms, other MSVC versions): exact for output length, `_time` (one IEEE addition of the epoch), `gnss_states`, `imu_outputs`, `iterations`, every string, bool and null; for every other number `|got - golden| <= 1e-9 + 1e-7 * |golden|`. Justification: other platforms differ legitimately in the last bits (different `sin`/`cos` in the C library, fma contraction inside GTSAM, a GTSAM compiled by another compiler), and the solver amplifies that by its conditioning; `1e-7` is about nine orders of magnitude above one unit in the last place and still far below anything physical or any transcription error (the branch's own record in `docs/PORT_VALIDATION.md`: sub-microsecond timestamp changes moved orientation by 5e-5 degrees and acceleration by 7e-6 m/s^2; a wrong constant, index or term moves outputs by many orders more). The absolute floor covers the near-zero objective and residuals of `coarse_linear`. Every run logs, per fixture, the number of samples that are not bit-identical and the worst absolute and relative difference (`qInfo`), so drift is visible in CI logs. Non-Windows results are unverified until the first push; a `Phase 8 fixup` that widens the portable bound for a named platform, with the observed numbers, is the expected remedy, never a change to the goldens.

*Test functions.*
- `fixturesAreDeterministic` — Task 8.8.
- `successFixturesMatchGolden` (data-driven over the three success fixtures): outcome `Succeeded`; `reason` empty; seventeen arrays of the golden length; each channel by `compareSamples`; diagnostics by `compareJson`; `roll`, `pitch`, `yaw` have no adjacent step above 180 degrees; for `stationary_spin`, `|yaw.last - yaw.first| > 360`.
- `rejectionFixturesMatchGolden` (data-driven over the nine): outcome `Rejected`; `reason` equals the golden `failure` string exactly; diagnostics has exactly the two keys; all arrays empty.
- `progressMatchesReferenceBoundaries`: the texts received for `coarse_maneuver`, mapped back (`"Starting fit"` -> `"Starting heading 0 deg"`, `"Pass P, iteration I"` -> `"Heading 0 deg, pass P, iteration I"`), equal the golden `progress` list: same boundaries, same order, same count.
- `cancelAtEachKindOfBoundary` (rows: the first checkpoint, the first "Integrating IMU factors", the first iteration, the 4th checkpoint overall, on `coarse_maneuver`): the `CancelFn` returns true from the n-th call on; outcome `Cancelled`; every array, `reason` and `diagnosticsJson` empty; the number of progress texts received equals n (the text of the cancelling boundary is the last one: no further boundary was reached, i.e. it returned promptly); a following uncancelled run of the same fixture still matches the golden (no state leaks between runs).
- `cancelNeverRequestedChangesNothing`: a `CancelFn` that always returns false and a `ProgressFn` that records give bit-identical results to a run with neither.
- `twoRunsAreBitIdentical`: `stationary_spin` run twice in the process: all seventeen arrays `sameBits`, `diagnosticsJson` byte-equal (TBB on; `tst_fusion_kernel::solverUsesTbb` asserts that it is).
- `workerThreadMatchesMainThread`: the same fixture run on a `QThread` with `setStackSize(64 * 1024 * 1024)` gives bit-identical results to the main-thread run (what Phase 5's worker will do; also shows the library has no thread affinity).
- `resultIsIndependentOfCallerState`: `run()` on a copy of the `Channels` whose original is destroyed before the call returns the same result (no retained references), and on `Channels{}` returns `Rejected` with the "needs GNSS, IMU" reason.

Expectations are the committed goldens and literals; never a second call of the code under test, except in the determinism and thread tests, where that is the point.

**Acceptance Criteria:**
- [ ] `ctest -C Release -R tst_fusion_parity --output-on-failure` passes in portable mode, and again with `FLYSIGHT_FUSION_EXACT=1` set in the calling environment on the capture machine; the report quotes the logged "not bit-identical" counts (expected: 0).
- [ ] The test target links `flysight_fusion` and not `gtsam` directly, and includes no header from `src/fusion/` other than `fusion.h`.

**Complexity:** M

---

### Task 8.11: `tst_fusion_kernel` (internals, from the branch self-test)

**Purpose:** Guard what the goldens cannot reach (every gate threshold, integration boundaries, the solver-failure path) and localize a parity failure to a stage.

**Files to create:** `tests/tst_fusion_kernel.cpp` — class `FusionKernelTest`; may include the internal headers and GTSAM.

**Technical Approach:** Retype the ideas and literal expectations of `branch:tests/fusion_regression.cpp` `selftest()` as QtTest functions (the executable itself is not ported):

- `unwrapRule` — the six literal vectors of `branch:tests/fusion_session_regression.cpp` (`{}`; `{45}`; forward crossings `{170,179,-179,-170,-10,150,-50,110,-90}` -> `{170,179,181,190,350,510,670,830,990}`; the mirrored backward case; `{170,-170,170,-170}` -> `{170,190,170,190}`; `{0,180,0,-180}` unchanged).
- `preintegrationHonoursExactBoundaries` — 1 s of 100 Hz constant acceleration `(1,-2,.5)`, GNSS at `.037` and `.863`: predicted velocity and position within `1e-10`.
- `validationRejectsEachDefect` — the branch's seven `reject(...)` cases against `validateSamples`, plus its seven wrapper cases against `runPipeline` (empty input, shortened force, cleared position, NaN epoch is not constructible through `Channels` and is covered by a NaN `gnssTime[0]`, infinite gyro, repeated GNSS time, zero velocity sigma), each asserting `Outcome::Rejected`.
- `backwardPropagationUndoesForward` — non-commuting rates with a gyro bias, `Logmap(backward.between(r)).norm() < 1e-12`.
- `headingIsUnconstrained` — stationary graph cost equal within `1e-8` for yaw `0`, `.8`, `2`.
- `reconstructionTimingAndEndpointCorrection` — 83 samples, first `.04`, last `.86`, rotation equal to the distributed `Rz(.2 * fraction)` within `1e-12`, acceleration norm `< 1e-12`.
- `stationaryGates` — the branch's ten cases: quiet accepted; constant translation accepted and giving the same attitude and gyro bias as quiet (`1e-12`); sustained drift rejected with `velocity_drift` and without `velocity_variability`; constant-speed turn, uncertain velocity, normalized variation, constant rotation, low-speed handling, uncertain GNSS rejected.
- `shortInputUsesCoarseInitializer`, `exactConstantVelocityFit` — the branch's `linear` case with epoch-relative exact times: converged, objective `< 1e-12`, position, velocity and acceleration within `1e-8`; `fittedWindow(linear, 0, .3)` throws.
- `fitTraceMatchesGolden` (data-driven, three success fixtures) — `runPipeline` with a `PipelineTrace`: method, interval, anchor time, initial gyro bias and start quaternion, `converged`, and the full `history` (count exact; `before` / `after` by the mode's tolerance) against the golden `trace`.
- `nonConvergenceIsSolverFailure` — `runPipeline(toChannels(coarse_maneuver), tuningWith(maxIterations = 1), ...)`: outcome `SolverFailed`, reason "Batch fusion did not converge; sensor fusion unavailable", two-key diagnostics, empty arrays.
- `solverUsesTbb` — `#ifdef GTSAM_USE_TBB` from `<gtsam/config.h>`, `QFAIL` otherwise.

**Acceptance Criteria:**
- [ ] All functions pass; in exact mode `fitTraceMatchesGolden` is bit-exact on the capture machine.
- [ ] This is the only test source that includes an internal `src/fusion/` header.

**Complexity:** M

---

### Task 8.12: Test registration, README, acceptance map

**Purpose:** Wire the tests in with one small gated hunk, document the capture so it is repeatable, and record acceptance 4.

**Files to modify:**
- `tests/CMakeLists.txt` — inside Phase 1's `if(FLYSIGHT_BUILD_FUSION_TESTS)` block, after `tst_solver_smoke`, one hunk:
  ```cmake
  # Fusion kernel: golden parity against sensor-fusion-clean-port (tests/README.md section 11)
  add_library(flysight_fusion_test_support STATIC
    fusion/fusionfixtures.cpp fusion/fusionfixtures.h
    fusion/fusiongolden.cpp   fusion/fusiongolden.h)
  target_include_directories(flysight_fusion_test_support PUBLIC "${CMAKE_CURRENT_LIST_DIR}/fusion")
  target_link_libraries(flysight_fusion_test_support PUBLIC flysight_fusion Qt${QT_VERSION_MAJOR}::Core)
  target_compile_definitions(flysight_fusion_test_support PUBLIC
    FLYSIGHT_FUSION_GOLDEN_DIR="${CMAKE_CURRENT_LIST_DIR}/data/fusion")
  if(NOT MSVC)
    target_compile_options(flysight_fusion_test_support PRIVATE -ffp-contract=off)  # fixture bits
  endif()
  flysight_add_fusion_test(tst_fusion_parity SOURCES tst_fusion_parity.cpp LIBS flysight_fusion_test_support)
  flysight_add_fusion_test(tst_fusion_kernel SOURCES tst_fusion_kernel.cpp LIBS flysight_fusion_test_support gtsam)
  ```
  `tst_fusion_kernel` names `gtsam` itself because its translation unit uses GTSAM types (spec section 3). `flysight_test_support` and `flysight_add_test` are untouched. `FLYSIGHT_FUSION_EXACT` needs no CMake support: CTest passes the caller's environment through.
- `tests/README.md` — additive: two rows in the fusion / solver table Phase 1 added; raise the executable count by two (relative to whatever count is found; overview, Integration Note 3); in section 3 mention `FLYSIGHT_FUSION_EXACT`; a new numbered section "## 11. Fusion golden parity" placed before Appendix A (if another phase has already taken 11, use the next free number and cite that number in the `tests/CMakeLists.txt` comment) containing: what the goldens are and the reference SHA; the fixture table with final parameters; the file formats; the tolerance policy with its justification; how to run exact mode (`$env:FLYSIGHT_FUSION_EXACT = "1"; ctest --test-dir build/FlySightViewer-build -C Release -R tst_fusion`); the capture procedure of Task 8.9 step by step with the harness `CMakeLists.txt` and `main.cpp` verbatim; a paragraph "Solver configuration of the goldens" stating that they were captured against upstream GTSAM `814a734` built without Boost (the shipped configuration), that the same harness built against the Boost-enabled fork build the branch was validated with produced byte-identical files for all fixtures on the capture machine (date, as recorded in `capture.json`), and how to repeat that cross-check when a Boost-enabled install is available (it is optional for a re-capture on a machine that has none, and must then be recorded as not done); the rule "goldens change only by re-capture from the branch, never to make the port pass"; and a note that the real-recording comparison of `branch:docs/PORT_VALIDATION.md` (17-26-24: objective 65602.22485051976, 9247 GNSS states, 24411 outputs) is an optional local check that becomes possible once Phase 9 can run a session through the calculation, not a CI test.
- `tests/acceptance_map.txt` — append (reuse the three "Sensor fusion as an explicit calculation" header comment lines if another phase already added them; otherwise add them as in Phase 2's document):
  ```
  # 104 - the ported fusion reproduces the goldens captured from sensor-fusion-clean-port
  104 tst_fusion_parity successFixturesMatchGolden
  104 tst_fusion_parity rejectionFixturesMatchGolden
  104 tst_fusion_kernel fitTraceMatchesGolden
  ```
  Do not touch the audit script or the file's leading comment (Phase 10 owns them).

**Acceptance Criteria:**
- [ ] With `-DFLYSIGHT_BUILD_FUSION_TESTS=OFF` neither test target nor `flysight_fusion_test_support` exists and every other test builds and passes; `ctest -LE fusion` runs nothing that links GTSAM.
- [ ] `audit_cleanup` passes (map lines name existing `void <Class>::<function>()`; target names match `tst_[a-z_]+`).
- [ ] A reader can repeat the capture from `tests/README.md` alone.
- [ ] The diffs of `tests/CMakeLists.txt`, `tests/README.md` and `tests/acceptance_map.txt` are additive.

**Complexity:** S

## Testing Requirements

### Unit Tests
- New: `tests/tst_fusion_parity.cpp` (public API: golden parity, rejections, progress boundaries, cancellation, determinism, worker thread) and `tests/tst_fusion_kernel.cpp` (internals and the fit trace), both label `fusion`, both with the 64 MiB main-thread stack.
- Existing tests are unchanged; `tst_builtins_golden` / `tst_builtins_engine` cover the `course` calculation after Task 8.2.

### Integration Tests
- Full suite with fusion tests on, and configured with `FLYSIGHT_BUILD_FUSION_TESTS=OFF`: both pass.
- Exact mode on the capture machine (`FLYSIGHT_FUSION_EXACT=1`), both fusion tests.
- CI on macOS and Linux is unverified until a push; expected follow-up is a `Phase 8 fixup` to the portable bound, with observed numbers.

### Manual Verification (Windows; a new `build-phase8` directory, never Michael's `build/`)
1. `cmake -G "Visual Studio 17 2022" -A x64 -B build-phase8 -S . -DCMAKE_PREFIX_PATH="C:/Qt/6.9.3/msvc2022_64" -DFLYSIGHT_BUILD_THIRD_PARTY=OFF -DFLYSIGHT_BUILD_TESTS=ON -DGTSAM_INSTALL_DIR=<repo>/build-solver-deps/GTSAM-install -DONETBB_INSTALL_DIR=<repo>/build-solver-deps/oneTBB-install`, then `cmake --build build-phase8 --config Release`.
2. `ctest --test-dir build-phase8/FlySightViewer-build -C Release --output-on-failure`, then again with `FLYSIGHT_FUSION_EXACT=1` and `-R tst_fusion`.
3. Inspect `flysight_fusion.vcxproj`'s compile line for one fusion source: `/O2 /fp:precise /bigobj`, no `/arch`, no `/fp:fast`, the two GTSAM definitions present.
4. Read one golden diagnostics object against `run()`'s `diagnosticsJson` for `coarse_maneuver` by eye (keys, order, types).

## Notes for Implementer

### Gotchas
- **Do not "fix" the algorithm.** Known oddities are preserved on purpose: the stopping test can accept a no-update LM step; `validateSamples` runs twice; the initializer scans the full recording while the graph covers the window; a non-finite `Local/...` sample anywhere in the recording (even before the origin) rejects the whole recording; the diagnostics carry every residual. If a numerical change seems necessary, stop and ask (spec section 12).
- The branch's `unwrapDegrees`, failure diagnostics and cancellation lived in the calculation file, not in `runFusion`; the harness must reproduce those few lines or the goldens will hold wrapped angles.
- The goldens must come from the **same GTSAM binary** the port's tests use: `build-solver-deps/GTSAM-install` (Boost-free, upstream pin). The old `third-party/GTSAM-install` is used by the cross-check build of the harness and by nothing else; the port is never built or tested against it (Phase 1's configure-time guard refuses it anyway).
- The two harness builds differ in exactly one thing, the solver prefix (and the Boost that the old solver's package config drags in). Same harness sources, same generator, same configuration, same flags, separate build directories, separate output directories, separate `PATH`s. If anything else differs, a difference between the outputs proves nothing.
- A Boost-enabled GTSAM changes the *layout-irrelevant* parts of GTSAM's headers the harness compiles (serialization member templates, Boost concept checks, Boost timers); none of it is arithmetic. Byte-identical output is therefore the expectation, not a hope; treat a difference as a real finding and stop.
- `FusionCancelled` must not derive from `std::exception`, and the checkpoint is never called from inside a TBB region (all three sites are in sequential code), so it never crosses GTSAM.
- `Result` arrays are `QVector<double>`; kernels use `std::vector` and `Vectors`. Convert only at the two boundaries (adapter in, channels out).
- Qt on Unix calls `setlocale(LC_ALL, "")`; that is why the IMU-gap message uses `QString::number(t, 'f', 6)` instead of `std::to_string`. Progress texts use `QString::number` / `arg` too.
- `QJsonObject` serializes keys sorted, so key order in the compact JSON is not the insertion order; byte comparison in exact mode relies on that being the same on both sides (it is: both go through `QJsonDocument`).
- `src/CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/README.md`, `tests/acceptance_map.txt` are shared with other phases: additive, self-contained hunks; do not reflow neighbours. Do not touch `builtincalculations.cpp`, `mainwindow.cpp`, `docs/`, the root `CMakeLists.txt`, or the CI workflow.
- `src/fusion` is not yet in the cleanup audit's "pure compute functions" rule (Phase 10); comply anyway: no `PreferencesManager`, `QSettings`, clock, or random source.
- GTSAM headers are heavy: keep them out of `fusion.h`, and include only what each `.cpp` uses.

### Decisions Made
- **Kernel input is the engine's plain channels**, so the branch's session adapter (validation order, epoch subtraction, deg-to-rad, audit) is inside the library and under golden test; Phase 9 only copies engine values into `Channels`.
- **`PRIVATE gtsam`** (the branch had `PUBLIC`): no consumer translation unit uses GTSAM types, so none needs its headers or ABI definitions; this keeps GTSAM out of the application's compile lines in Phase 9. `tst_fusion_kernel` names `gtsam` itself.
- **Stage-based outcome classification** (`Rejected` before the fit, `SolverFailed` from it), internal exceptions with the branch's messages, one catching function; `std::bad_alloc` propagates.
- **Progress texts drop the historical "Heading 0 deg"** (it would confuse users in tooltips); boundaries are unchanged and tested against the branch's recorded sequence.
- **Removed as value-neutral:** the initializer's experiment-only scan range, kernel logging. **Kept although constant:** the zero initial heading and its composition.
- **The "no shared local origin" rejection is not ported** (unrepresentable; a missing attribute is a missing input in the engine).
- **`time_method` keeps the branch's wording** ("SessionData shared UTC conversion; ...") because the diagnostics content is specified as the branch's.
- **Shared `anglehelper.h`** used by the GNSS course and by fusion, as on the branch.
- **Goldens come from the shipping solver configuration** (upstream GTSAM `814a734`, built without Boost; Michael's decisions recorded in Phase 1), not from the Boost-enabled fork build the branch was validated with. The bridge between the two is the Windows cross-check of Task 8.9: both builds, same harness, byte-identical output for every fixture, recorded in `capture.json`; any difference stops the phase and goes to Michael.
- **The harness has its own CMake** and takes the solver prefix as a parameter, because neither the branch's nor `master`'s solver discovery can serve both solver builds.
- **`git archive` instead of `git worktree add`** for the reference tree; **fresh build of the branch sources instead of `build-port/`'s library**, whose freshness is unknown; `build-port/` served as the record of compile flags.
- **Golden format:** JSON for everything scalar (exact shortest-round-trip doubles) plus hex bit-pattern text for channels; fixtures generated in code rather than stored; under 1 MB.
- **Two comparison modes:** exact (decides acceptance on the capture configuration) and a portable bound `1e-9 + 1e-7*|golden|` (CI), with the reasoning in Task 8.10.
- **`-ffp-contract=off`** on non-MSVC compilers for the library and the fixture generator, to match the reference platform's arithmetic; no flag is added on MSVC.
- **`Detail::runPipeline(..., Tuning, ..., PipelineTrace*)`** as the single internal seam, for the solver-failure test and the fit trace.

### Open Questions
- None blocking, with one designed stop: if the Boost-free and Boost-enabled solver builds do not give byte-identical harness output, the phase halts at Task 8.9 and Michael decides which configuration is the reference (Phase 1, Appendix B describes the way back to Boost). Whether the fixture parameters of Task 8.8 converge on the reference can only be learned by running the harness; the adjustment rule covers it. Cross-platform differences are unknown until the first push; the portable bound is a reasoned starting point.

## Definition of Done

This phase is complete when:
1. All tasks have passing acceptance criteria.
2. `tst_fusion_parity` and `tst_fusion_kernel` pass in portable mode and, on the capture machine, in exact mode with zero non-identical samples (or with every exception explained as specified), and the full suite passes with fusion tests on and off.
3. `flysight_fusion` is the only product target linking GTSAM, is pure (no engine, session, GUI, preference, logging), and nothing in it is named EKF.
4. The goldens (captured against the Boost-free upstream GTSAM), `capture.json` with the recorded byte-identical cross-check against the Boost-enabled build, the fixture generators and the repeatable procedure in `tests/README.md` are in place; no harness or branch source is in the repository.
5. Code follows the style of `src/engine/` and `src/calculations/` (small intent-named functions, `///` contracts, comments that say why); no TODOs or placeholder code remain.
6. The implementation report lists the files for the phase commit by explicit path (none under `third-party/`, `build*/`, `PLANS/`), and states the final fixture parameters, the cross-check comparison and its result, and the exact-mode result.
