# Phase 6: Temperature-dependent gyro bias

## Overview

This phase gives the full fit the gyro bias model of spec section 6: `b(t) = b0 + b1 (T(t) - T_ref)`, with `b0` the gyro part of today's shared `B(0)`, `b1` a new three-vector `T(0)` (rad/s per degC) under a zero-mean prior of sigma 0.010 deg/s per degC, `T` the IMU temperature channel and `T_ref` the mean IMU temperature over the fitted window. The IMU temperature becomes the twenty-second input of the fit (`Channels::imuTemperature`, `IMU/temperature`, degC), a required input like the other twenty-one: it is a column of every FlySight 2 `SENSOR.CSV` and of every corpus recording, so a file without it is hand-made and is blocked by the engine like any file missing a declared input, and the kernel rejects a channel of the wrong length or with a non-finite value naming `IMU/temperature`. Every IMU factor of the full fit is a custom factor on `X(i), V(i), X(j), V(j), B(0), T(0)` that evaluates the interval's bias as `B(0) + [0; T(0) (T_i - T_ref)]` and chains the preintegration's bias Jacobian; the pass loop's re-preintegration, the cost test, the reported rebuild and the trajectory reconstruction all evaluate each interval at its own bias. The stock graph (constant bias, `gtsam::ImuFactor`, no `T(0)`) remains as the internal path of the Phase 5 initializer's prefix and segment fits, selected by the fit routine's default argument.

The diagnostics gain `model.gyro_bias` (`b0_rad_s`, `b1_rad_s_per_degc`, `t_ref_degc`, all numbers) beside Phase 4's `model.per_step`, and `algorithm` becomes `batch-temperature-bias-v3`. The tests hold the custom factor's six Jacobians to finite differences, the section 6 cases (a recording without the channel is rejected by the kernel and blocked by the engine; a constant temperature leaves `b1` at its prior; the drifting-bias recording recovers `b1` within 20 % in at most 30 iterations), and the phase ends with the Phase 1 re-capture. Every golden fixture gains a constant 25 degC temperature, so every full fit now carries `T(0)` and its prior: the goldens' numbers may move in their last digits and are re-captured, the rejection files change by the algorithm string only, and no fixture's outcome or progress sequence changes.

## Dependencies

- **Depends on:** Phase 5 (`InitialState` with per-fix `Rot3` and one constant gyro bias; `fitFactorGraph(samples, initial, tuning, passFormat, checkpoint)`; `FitResult::graph`; the initializer's prefix and segment fits calling the same routine on `fittedWindow()` sub-windows; the four initializer fixtures of Task 5.6 (`motion_start`, `rest_throughout`, `sacc_anchor`, `drifting_bias`) and `driftingBiasSegmentsConverge`; the trace writer of Task 5.6), Phase 3 (the pass loop with one rebuild per pass as the cost test's graph and the reported graph; `FitResult`, `Stopping`, `Quality`, `FitFailure`; `collectResiduals()` filling `quality`), Phase 4 (`preintegrateImu()` with the per-step term, untouched here; `modelSummary(tuning)` and the `Tuning` routed into `successDiagnostics()`), Phase 1 (`fusion_golden_capture`, `tst_fusion_golden`, `tests/fusion/fusiontrace.h`, the re-capture recipe quoted in Task 6.10), Phase 2 (`fitInputs()` and the reader-based `channelsFrom()`, through which the new input reaches the engine path and `fusion_runner`; `tst_fusion_runner`'s fixture-to-CSV writer and its `--dump-inputs` assertions, which this phase extends to the temperature column).
- **Blocks:** Phase 7 (documentation, acceptance map, audit).
- **Assumptions:**
  - The working tree is the plan branch with Phases 1-5 committed: the goldens under `tests/data/fusion/` are Phase 5's capture, `algorithm` is `batch-shared-bias-v2`, `src/fusion/stationarywindow.*` are gone, `tests/tst_fusion_golden.cpp` and `tests/tst_fusion_runner.cpp` exist, `tests/tst_fusion_kernel.cpp` has Phase 3's, 4's and 5's slots.
  - Line numbers below for `src/fusion/*`, `tests/tst_fusion_kernel.cpp`, `tests/tst_fusion_session.cpp`, `tests/fusion/*` are those of `master` at `1f3139a`; Phases 3-5 shifted some of them, and every function named here is identified by content (its name has not changed unless a phase document says so). `tests/tst_fusion_runner.cpp` is Phase 2's file, identified by its test-function names.
  - GTSAM on this machine is 4.3 with `GTSAM_TANGENT_PREINTEGRATION` (Phase 4's assumption). `gtsam::symbol_shorthand::T(j)` exists (`third-party/gtsam/gtsam/inference/Symbol.h` line 167, `Symbol('t', j)`). `PreintegrationBase::computeErrorAndJacobians(pose_i, vel_i, pose_j, vel_j, bias_i, H1..H5)` (`PreintegrationBase.h` lines 168-174, `.cpp` 169-196) returns the 9-vector error with `H5` the 9x6 Jacobian with respect to `bias_i`; `ImuFactor::evaluateError` is exactly that call (`ImuFactor.cpp` 153-159) and its noise model is `noiseModel::Gaussian::Covariance(pim.preintMeasCov_)` (`ImuFactor.cpp` 116-120). `imuBias::ConstantBias` is a vector space (`ImuBias.h` line 164: retract is vector addition), so the Jacobian of `B(0) + [0; T(0) dT]` with respect to `B(0)` is the identity and with respect to `T(0)` is `[0_3x3; dT I_3]`. `gtsam/base/numericalDerivative.h` provides `numericalDerivative61` ... `numericalDerivative66` for six-argument functions (lines 643-840) with a static assertion that every argument and the return type is a manifold; `Vector9`, `Vector3`, `Pose3` and `ConstantBias` all have traits. `NonlinearFactor::clone()` (`NonlinearFactor.h` line 154) must be overridden by a factor that is ever cloned; implement it as `ImuFactor` does.
  - The calculation engine blocks a calculation whose declared measurement input the session lacks: `gatherInputs()` returns `ResultStatus::MissingInput` at the first unavailable declared input (`src/engine/calculationengine.cpp` lines 549-603), `readiness()` / `blockers()` report `MissingInput` / `NotApplicable`, and `tst_fusion_session::missingInputsAreNotApplicable` (lines 639-690) pins that behaviour for the fit. That is the intended behaviour for a file without the temperature column; nothing in `src/engine/` changes.
  - The IMU temperature is a stored column of `SENSOR.CSV` (`$COL,IMU,time,wx,wy,wz,ax,ay,az,temperature`, unit `deg C`; `docs/DATA_SCHEMA.md` lines 26-29; `tests/tst_importer.cpp` 461, 509-510) and of `DataExporter`'s IMU column table (`src/dataexporter.cpp` line 34), so a session that stores it exports it. The conversion layer's unit table maps `deg C` and `degC` to scale 1, offset 0, label `degC` (`src/units/unitconversion.h` lines 52, 57: "keep Celsius, don't convert to Kelvin"), so the effective value the engine serves is the recorded degC, bit for bit (`sourceconversion.cpp` lines 51-52 hand the source vector on when both steps are the identity); the Fahrenheit conversion of `unitdefinitions.h` lines 130-136 is display-only and never reaches effective values. No calculation under `src/calculations/` reads `IMU/temperature`.
  - The verification build is `build-phase1/` (overview, "Verification commands"); `build/` is never rebuilt. Fusion tests run under a 600 s timeout; every fit below is a fixture fit of seconds.

## Tasks

### Task 6.1: The temperature channel at the public boundary

**Purpose:** The kernel's input gains the IMU temperature as a required channel: one finite value per IMU sample, converted to nothing, rejected by name otherwise.

**Files to modify:**
- `src/fusion/fusion.h` — `Channels` (lines 16-28) and the `run()` contract comment (lines 67-85)
- `src/fusion/inputadapter.cpp` — `requireAllChannels()` (27-45), `appendImuSamples()` (66-76)

**Technical Approach:**

`Channels`: after `wx, wy, wz` (line 23) and before `originIndex`, add

```cpp
QVector<double> imuTemperature;      ///< IMU/temperature, degC: the IMU's own temperature, one value per imuTime sample
```

The struct comment ("GNSS channels share gnssTime's length and IMU channels imuTime's; anything else is a rejection") stays true as written. In the `run()` comment, where the contract describes what the diagnostics carry (`Result::diagnosticsJson`, lines 45-47: "input audit, initializer, objective, biases, residuals"), add "the gyro bias model (`model.gyro_bias`: `b0`, `b1`, the reference temperature)". The member order is binding: `kFitInputs[]` follows `Channels`' vector members in order (Task 6.7), so `imuTemperature` sits after `wz` and before the attributes.

`inputadapter.cpp`:
- `requireAllChannels()`: after the `IMU/wz` line add `requireChannel("IMU/temperature", c.imuTemperature, ni);` — last in the order, so a defect in any other channel is still the reported reason, and an empty vector is `Missing or mismatched IMU/temperature` (the existing helper's text); a non-finite value is `Nonfinite IMU/temperature`. The nine rejection fixtures keep their reasons because they now carry a well-formed temperature (Task 6.7) and their own defect comes earlier in the order.
- `appendImuSamples()`: inside the loop, `d.temperature.push_back(c.imuTemperature[k]);` (degC stays degC; the function comment gains "temperature is carried as recorded"). `Samples::temperature` is Task 6.2's.
- `inputAudit()` is unchanged (the temperature is now as ordinary as `IMU/ax`; its count is `imu_count`).

`prepareInput()` (94-117) is otherwise unchanged; the check order stays: counts, origin index, channels, then the time axes.

**Acceptance Criteria:**
- [ ] `Channels` has `QVector<double> imuTemperature` immediately after `wz`; `fusion.h` includes nothing new
- [ ] `Fusion::run()` on `coarse_maneuver` with `imuTemperature` cleared returns `Rejected` with reason `Missing or mismatched IMU/temperature`; with a vector one short, the same reason; with one NaN temperature, `Nonfinite IMU/temperature`; with a defect in `IMU/wz` as well, the `IMU/wz` reason (Task 6.8's `validationRejectsEachDefect`)
- [ ] The nine `reject_*` fixtures still reject with the reasons their goldens hold (Task 6.10: their files change by the algorithm string only)
- [ ] `prepared.recording.temperature` equals the channel's values bit for bit and has `imuTime`'s length

**Complexity:** S

---

### Task 6.2: Samples, tuning, and the temperature at a fix

**Purpose:** The numerical stages see the temperature as a per-IMU-sample series that windows slice with the IMU range and validation checks; the `b1` prior sigma is a tuning value with the spec's default; the temperature at a fix is one interpolation, shared by the fit, the reconstruction and the tests.

**Files to modify:**
- `src/fusion/fusionsamples.h` — `Samples` (21-29), `Tuning` (34-40), `kPi` (42) moved above `Tuning`
- `src/fusion/fusionsamples.cpp` — `requireValidTuning()` (52-61, as Phases 3-5 shaped it), `validateSamples()` (146-160), `fittedWindow()` (162-174), `requireUsableRecording()` (176-193), one new helper
- `src/fusion/imuintegration.h` / `.cpp` — a scalar `interpolateAt()` overload next to the vector one (63-74), `temperatureAtFix()`

**Technical Approach:**

*`Samples`*: add `std::vector<double> temperature; ///< IMU temperature, degC, per IMU sample. The public boundary always supplies it; hand-built samples of the stock path (tests) may leave it empty` after `gyro`. A plain `std::vector<double>`, not `Vectors` (Decisions Made).

*`Tuning`*: move `constexpr double kPi = 3.14159265358979323846;` (line 42) above `struct Tuning` (it is a constant; nothing else moves), then add after `accBiasSigma, gyroBiasSigma`:

```cpp
double gyroBiasSlopeSigma = .010 * kPi / 180;  ///< prior on b1, the gyro bias change per degC of IMU temperature, rad/s/degC (spec: 0.010 deg/s/degC)
```

with a two-line comment stating the model `b(t) = b0 + b1 (T(t) - T_ref)` and that `b0`'s prior is `gyroBiasSigma`. Add `gyroBiasSlopeSigma` to the strictly-positive list of `requireValidTuning()` (the same `Invalid fusion configuration` text). The struct comment ("The defaults are the model ...") stays true.

*Validation* (`fusionsamples.cpp`): a helper in the anonymous namespace, `void requireFiniteScalars(size_t count, const std::vector<double> &values, const char *lengthMessage, const char *finiteMessage)`: does nothing for an empty vector (the stock path's hand-built samples); otherwise throws `lengthMessage` unless `values.size() == count`, then `finiteMessage` for any non-finite entry. Call it in `validateSamples()` right after the `force`/`gyro` check with `samples.imuTime.size()`, `samples.temperature`, `kWindowLengthsMessage`, `kWindowNonfiniteMessage`; and in `requireUsableRecording()` after its `force`/`gyro` check with the recording messages. The check order of everything else is unchanged. (The public boundary has already refused an empty or malformed channel by name; these are the internal belt-and-braces checks every other array has.)

*`fittedWindow()`*: after the three `assign` lines (170-172), `if (!recording.temperature.empty()) window.temperature.assign(recording.temperature.begin() + lo, recording.temperature.begin() + hi);` — the same `[lo, hi)` range as `imuTime`, so a sub-window's temperature is exactly the temperature of its IMU samples (one before the first fix to one past the last). An empty temperature stays empty (`gnssInsideWindowAndCoverage()` never touches it).

*`interpolateAt()` scalar overload* (`imuintegration.h`, next to line 28):

```cpp
/// The scalar series `values` (sampled at `times`) linearly interpolated at `t`; the same rule as the vector form.
double interpolateAt(const std::vector<double> &times, const std::vector<double> &values, double t);
```

Implemented with the same statements as the vector form (the out-of-range throw with the same text, `lower_bound`, the "on a sample" shortcut, `values[j-1] + f*(values[j]-values[j-1])`); a private template in the `.cpp` instantiated for both is acceptable, provided the vector overload's arithmetic is unchanged (Phase 4's per-step term and Phase 5's initializer call it; their goldens pin it).

*`temperatureAtFix()`* (`imuintegration.h`):

```cpp
/// The IMU temperature interpolated at fix `k` of `samples`, degC. Requires
/// a non-empty temperature series.
double temperatureAtFix(const Samples &samples, size_t k);
```

= `interpolateAt(samples.imuTime, samples.temperature, samples.gnssTime[k])`; fixes lie inside IMU coverage by `validateSamples()`, so it does not throw on a validated window. The file comment of `imuintegration.h` (12-15) gains one clause: "and the temperature at a fix, for the gyro bias model".

**Acceptance Criteria:**
- [ ] `Tuning{}.gyroBiasSlopeSigma == .010 * kPi / 180` (`1.7453292519943295e-4`); `validateSamples()` throws `std::invalid_argument` for `gyroBiasSlopeSigma = 0` and for `-1` (Task 6.8)
- [ ] `validateSamples()` throws for a temperature vector shorter than `imuTime` and for one with a NaN, and does not throw for an empty one; `requireUsableRecording()` likewise with the recording messages
- [ ] `fittedWindow(recording, start, end).temperature.size() == window.imuTime.size()` whenever the recording has a temperature, and is empty otherwise (Task 6.8 asserts both)
- [ ] `interpolateAt(times, scalar values, t)` returns `values[j]` exactly on a sample and agrees with the vector overload applied to `(v, 0, 0)` bit for bit between samples (Task 6.8)
- [ ] `git diff` shows no change to the vector `interpolateAt()`'s statements, to `gyroIncrement()`, `preintegrateImu()`, `propagateAttitude()` or `integrationEdges()`

**Complexity:** M

---

### Task 6.3: The custom IMU factor on `(B(0), T(0))`

**Purpose:** One factor class, private to the fusion library, that is `ImuFactor` with the bias evaluated as `B(0) + [0; T(0) dT]` for its interval and the chain rule applied; it is the only new numerical object of the phase and Task 6.8 proves its Jacobians before any fit uses it.

**Files to create:**
- `src/fusion/temperatureimufactor.h` — class `TemperatureImuFactor`
- `src/fusion/temperatureimufactor.cpp` — its definitions

**Files to modify:**
- `src/CMakeLists.txt` — `flysight_fusion`'s source list (line 331 onward): add `fusion/temperatureimufactor.cpp fusion/temperatureimufactor.h` next to `fusion/factorgraphfit.cpp`

**Technical Approach:**

```cpp
// Internal to the fusion library: the IMU factor of the temperature-dependent
// gyro bias model. The same measurement, error and noise model as
// gtsam::ImuFactor, with the bias of the interval evaluated from the shared
// constant bias B(0) and the slope T(0): b = B(0) + [0; T(0) * dT], where dT is
// this interval's IMU temperature at its first fix minus the fitted window's
// reference temperature, both fixed at construction.

namespace FlySight::Fusion::Detail {

class TemperatureImuFactor
    : public gtsam::NoiseModelFactorN<gtsam::Pose3, gtsam::Vector3, gtsam::Pose3, gtsam::Vector3,
                                      gtsam::imuBias::ConstantBias, gtsam::Vector3> {
public:
    using Base = gtsam::NoiseModelFactorN<...same six...>;
    using Base::evaluateError;

    /// `pim` preintegrated at this interval's bias (the caller's job), `temperatureDelta` = T_i - T_ref, degC.
    TemperatureImuFactor(gtsam::Key pose_i, gtsam::Key vel_i, gtsam::Key pose_j, gtsam::Key vel_j,
                         gtsam::Key bias, gtsam::Key slope,
                         const gtsam::PreintegratedImuMeasurements &pim, double temperatureDelta);

    gtsam::NonlinearFactor::shared_ptr clone() const override;
    const gtsam::PreintegratedImuMeasurements &preintegratedMeasurements() const;
    double temperatureDelta() const;

    gtsam::Vector evaluateError(const gtsam::Pose3 &pose_i, const gtsam::Vector3 &vel_i,
                                const gtsam::Pose3 &pose_j, const gtsam::Vector3 &vel_j,
                                const gtsam::imuBias::ConstantBias &bias, const gtsam::Vector3 &slope,
                                gtsam::OptionalMatrixType H1, gtsam::OptionalMatrixType H2,
                                gtsam::OptionalMatrixType H3, gtsam::OptionalMatrixType H4,
                                gtsam::OptionalMatrixType H5, gtsam::OptionalMatrixType H6) const override;

private:
    gtsam::PreintegratedImuMeasurements m_pim;
    double m_temperatureDelta;
};

}
```

Constructor: `Base(gtsam::noiseModel::Gaussian::Covariance(pim.preintMeasCov()), pose_i, vel_i, pose_j, vel_j, bias, slope)`, the noise model `ImuFactor` builds (`ImuFactor.cpp` 116-120; `preintMeasCov()` is the public accessor, `ImuFactor.h` line 138). `clone()` as `ImuFactor::clone()` (`ImuFactor.cpp` 123-126).

`evaluateError()`:
1. `const gtsam::imuBias::ConstantBias intervalBias(bias.accelerometer(), bias.gyroscope() + slope * m_temperatureDelta);` — the accelerometer part is untouched (constant accelerometer bias, spec section 6).
2. `gtsam::Matrix96 H5full; const gtsam::Vector9 error = m_pim.computeErrorAndJacobians(pose_i, vel_i, pose_j, vel_j, intervalBias, H1, H2, H3, H4, (H5 || H6) ? &H5full : nullptr);` — `H1..H4` pass straight through (`OptionalMatrixType` is `Matrix*`, which `OptionalJacobian` accepts implicitly: `OptionalJacobian.h` line 99; `ImuFactor` does exactly this).
3. `if (H5) *H5 = H5full;` (the bias enters `intervalBias` with the identity Jacobian: `ConstantBias` is a vector space and `[acc; gyro + slope dT]` is `[acc; gyro] + [0; slope dT]`). `if (H6) *H6 = H5full.rightCols<3>() * m_temperatureDelta;` (chain rule through `[0_3x3; dT I_3]`).
4. Return `error`.

No branch on `slope` or `dT` being zero: with either zero, `gyroscope() + 0.0` equals `gyroscope()` bit for bit (`x + 0.0 == x` for every finite `x` that is not `-0.0`), so the factor reproduces `ImuFactor` at zero slope by arithmetic, which Task 6.8 asserts bitwise.

Header includes: `<gtsam/geometry/Pose3.h>`, `<gtsam/navigation/ImuBias.h>`, `<gtsam/navigation/ImuFactor.h>`, `<gtsam/nonlinear/NonlinearFactor.h>`; the `.cpp` needs nothing more. Both files are under `src/fusion/`, which the audit's GTSAM-header allowlist (`tests/audit/cleanup_audit.cmake` line 316, `^src/fusion/`) already covers; no CMake target or confinement-list change (the class is compiled into `flysight_fusion`). The class has no `print`/`equals` overrides (nothing in the kernel prints or compares factors); do not add them.

**Acceptance Criteria:**
- [ ] `src/fusion/temperatureimufactor.h` declares `TemperatureImuFactor` as above; both files are listed in `flysight_fusion`'s sources; the configure prints `GTSAM link confinement: OK` and `ctest -L audit` passes without a regex change
- [ ] Task 6.8's `temperatureFactorJacobians` holds every analytical block `H1..H6` within `1e-6` of the finite-difference Jacobian at a point with `dT != 0`, `slope != 0`, and `H6` is not identically zero there
- [ ] At zero slope (and, separately, at `dT = 0` with a non-zero slope) the factor's unwhitened error, whitened error and `H1..H5` equal `gtsam::ImuFactor`'s on the same `pim` bit for bit, and `H6` is the zero matrix
- [ ] The class is referenced only from `src/fusion/factorgraphfit.cpp` and `tests/tst_fusion_kernel.cpp`

**Complexity:** M

---

### Task 6.4: The fit with the temperature model

**Purpose:** The fit routine builds the temperature graph when asked and the stock graph otherwise, preintegrates every interval at that interval's bias in every build (pass 1, the rebuild after each pass that serves the cost test and the next pass, and the reported rebuild), starts `b1` at zero, and reports the fitted slope.

**Files to modify:**
- `src/fusion/factorgraphfit.h` — `GyroBiasModel`, `BiasLinearization`, `gyroBiasModelFor()`, `intervalBias()`, `FitResult` (two members), `buildFactorGraph()` (a second overload), `fitFactorGraph()` (one trailing parameter), the factor-order comment (44-46), the file comment (15-20)
- `src/fusion/factorgraphfit.cpp` — `addImuFactor()` (43-47), a new `addTemperatureImuFactor()`, a new `addSlopePrior()`, `collectResiduals()` (98-113, as Phase 3 reshaped it), `buildFactorGraph()` (117-131), `fitFactorGraph()` (as Phases 3 and 5 reshaped it)

**Technical Approach:**

*Types* (`factorgraphfit.h`, before `FitResult`):

```cpp
/// How a fit models the gyro bias. The full fit uses the temperature model
/// (the public boundary always supplies a temperature); the initializer's
/// prefix and segment fits use the constant model.
struct GyroBiasModel {
    bool temperatureLinear = false;   ///< true: b(t) = b0 + b1 (T(t) - tRef) through TemperatureImuFactor and T(0); false: constant bias, stock ImuFactor, no T(0)
    double tRef = 0;                  ///< degC; the mean IMU temperature of the fitted window; meaningful only when temperatureLinear
};

/// The temperature model for `window`, tRef its plain mean temperature (index
/// order). Throws std::invalid_argument("Temperature model without a temperature series") on an empty series.
GyroBiasModel gyroBiasModelFor(const Samples &window);

/// Where a graph is linearized: the constant bias and, with the temperature
/// model, the slope (zero under the constant model).
struct BiasLinearization {
    gtsam::imuBias::ConstantBias bias;
    gtsam::Vector3 slope = gtsam::Vector3::Zero();
};

/// The bias the model assigns to the interval that starts at fix `k`: `bias`
/// unchanged under the constant model; with the temperature model its gyro
/// part shifted by slope * (T_k - tRef), T_k the temperature at fix k.
gtsam::imuBias::ConstantBias intervalBias(const Samples &d, size_t k, const gtsam::imuBias::ConstantBias &bias,
                                          const gtsam::Vector3 &slope, const GyroBiasModel &model);
```

`gyroBiasModelFor()`: throws on an empty series; else `temperatureLinear = true` and `tRef` = the sum of `window.temperature` in index order divided by its size (a plain loop; no pairwise or compensated summation, so the value is the same on every IEEE platform without contraction; for a constant series of an exactly representable value the mean is that value exactly). `intervalBias()`: `if (!model.temperatureLinear) return bias;` else `ConstantBias(bias.accelerometer(), bias.gyroscope() + slope * (temperatureAtFix(d, k) - model.tRef))`. The constant branch performs no arithmetic: the stock path is Phase 5's, bit for bit.

*`FitResult`*: add `gtsam::Vector3 gyroBiasSlope = gtsam::Vector3::Zero(); ///< the fitted b1, rad/s per degC; zero under the constant model` and `GyroBiasModel biasModel; ///< the model this fit used (tRef for the diagnostics and the reconstruction)` after Phase 5's `graph`.

*Graph builders*. Keep `buildFactorGraph(samples, bias, tuning, checkpoint)` with its signature and meaning (the stock graph; Phase 5's tests and the initializer's fits call it or reach it through the fit) and make it forward to a new overload:

```cpp
/// The factor graph of `samples` under `model`, every IMU factor preintegrated
/// at its interval's bias (intervalBias of `at`). Factor order is part of the
/// numerical behavior: per state the position factor, the velocity factor,
/// then for every state but the first the IMU factor; the bias prior; and,
/// with the temperature model, the slope prior last.
gtsam::NonlinearFactorGraph buildFactorGraph(const Samples &samples, const BiasLinearization &at,
                                             const GyroBiasModel &model, const Tuning &tuning,
                                             const Checkpoint &checkpoint = Checkpoint());
```

The four-argument form is `return buildFactorGraph(samples, BiasLinearization{bias}, GyroBiasModel{}, tuning, checkpoint);`. In the six-argument body the loop of lines 121-128 is unchanged except that `addImuFactor()` becomes `if (model.temperatureLinear) addTemperatureImuFactor(graph, d, k, at, model, c); else addImuFactor(graph, d, k, at.bias, c);`, and after `addBiasPrior(graph, c)` comes `if (model.temperatureLinear) addSlopePrior(graph, c);`. The checkpoint texts and their cadence (`Integrating IMU factors` every 256 states) are unchanged.

- `addImuFactor()` (43-47): unchanged (the stock `gtsam::ImuFactor` on `B(0)` at `bias`).
- `addTemperatureImuFactor(graph, d, k, at, model, c)`: `const double dT = temperatureAtFix(d, k-1) - model.tRef;` (the interval between fixes `k-1` and `k` takes the temperature of its first fix, `k-1`, matching `ImuFactor`'s convention that the factor's bias is the bias at state `i`; Decisions Made), `const auto bias = intervalBias(d, k-1, at.bias, at.slope, model);`, `graph.emplace_shared<TemperatureImuFactor>(X(k-1), V(k-1), X(k), V(k), B(0), T(0), preintegrateImu(d, d.gnssTime[k-1], d.gnssTime[k], bias, c), dT);`. `preintegrateImu()` is Phase 4's, untouched; it is simply handed the interval's bias.
- `addSlopePrior(graph, c)`: `graph.emplace_shared<gtsam::PriorFactor<gtsam::Vector3>>(T(0), gtsam::Vector3::Zero(), gtsam::noiseModel::Diagonal::Sigmas(gtsam::Vector3::Constant(c.gyroBiasSlopeSigma)));` with the comment: the datasheet's typical drift; a recording whose temperature does not change leaves the slope here.
- `using gtsam::symbol_shorthand::T;` next to `B`, `V`, `X` (line 18-20). Include `"fusion/temperatureimufactor.h"`.

*`collectResiduals()`*: the walk of Phase 3 (position, velocity, `imu` per state; `bias_prior`; the sums for `quality`) gains, after the `bias_prior` entry, `if (result.biasModel.temperatureLinear) result.residuals.push_back({"slope_prior", 0, d.gnssTime[0], 2*graph.at(++factor)->error(values)});` (or the equivalent index bookkeeping; the walk must consume exactly the factors the builder emitted). The temperature factor's residual kind stays `"imu"`, its dimension is 9, so `imuNrms`'s definition is unchanged; neither prior contributes to a normalized RMS.

*`fitFactorGraph()`*: signature (Phase 5's plus one trailing parameter):

```cpp
FitResult fitFactorGraph(const Samples &samples, const InitialState &initial, const Tuning &tuning,
                         const QString &passFormat = QString::fromLatin1(kFullFitPassFormat),
                         const Checkpoint &checkpoint = Checkpoint(),
                         const GyroBiasModel &model = GyroBiasModel());
```

The default is the constant model, so the initializer's prefix and segment fits (which pass five arguments) and every existing test use the stock graph without an edit (Decisions Made). Body, relative to Phase 3's and 5's:
1. After `values = initialValues(d, initial)`: `if (model.temperatureLinear) values.insert(T(0), gtsam::Vector3::Zero());` (the full fit starts `b1` at zero, spec section 6; `b0` is the initializer's). Under the constant model `T(0)` is never inserted: no factor would touch it and the linear system would be indeterminate.
2. A file-local `BiasLinearization linearizationOf(const gtsam::Values &values, const GyroBiasModel &model)` returns `{values.at<ConstantBias>(B(0)), model.temperatureLinear ? values.at<Vector3>(T(0)) : Vector3::Zero()}`; both build sites (the pass-1 graph before the loop and the rebuild after each pass) call `buildFactorGraph(d, linearizationOf(values, model), model, c, checkpoint)`. The rebuild therefore re-preintegrates each interval at that interval's bias (`b0 + b1 dT_k` with the pass's fitted `b0` and `b1`), the cost test `rebuilt.error(values)` evaluates every factor at its interval's bias, and the reported graph is the same rebuild: nothing else in the loop changes (passes, settle test, slow tail, `FitFailure`, one rebuild per pass, `result.objective = costRebuilt`).
3. After the loop, before `collectResiduals()`: `result.biasModel = model; result.gyroBiasSlope = model.temperatureLinear ? values.at<Vector3>(T(0)) : Vector3::Zero();`.
4. `validateSamples(d, c)` at the top stays; `if (model.temperatureLinear && d.temperature.empty())` throw `std::invalid_argument("Temperature model without a temperature series")` (a programming error, unreachable through `planFit()`; stated so the contract is checkable).

The header comment on `fitFactorGraph()` gains one sentence: under the temperature model the graph carries `T(0)`, every interval is preintegrated and evaluated at its own bias in every build, and the fitted slope is returned in `gyroBiasSlope`. The file comment (15-20) says "one accelerometer-and-gyro bias B(0) shared by the whole recording" — append "and, in the full fit, one slope T(0) that makes the gyro bias linear in the IMU temperature (temperatureimufactor.h); the initializer's fits keep the constant bias".

`yawSigmaDeg()` (Phase 5) is untouched: the initializer's fits are stock.

**Acceptance Criteria:**
- [ ] `buildFactorGraph(d, bias, Tuning{})` (four arguments) on `boundarySamples` still yields a graph whose factors are, in order, `GPSFactor`, `PriorFactor<Vector3>`, `GPSFactor`, `PriorFactor<Vector3>`, `ImuFactor`, `PriorFactor<ConstantBias>` (`headingIsUnconstrained` and `yawSigmaIsMarginalAboutTheVertical` pass unchanged); with a temperature model the same samples yield `TemperatureImuFactor` in the IMU position and a `PriorFactor<Vector3>` on `T(0)` last (Task 6.8's `temperatureGraphShape`)
- [ ] `intervalBias()` under `GyroBiasModel{}` returns its `bias` argument bit for bit; under the temperature model its gyro part is `gyro + slope * (temperatureAtFix(d, k) - tRef)` (Task 6.8)
- [ ] `fitFactorGraph()` with the default model on `drifting_bias`'s window produces a `FitResult` with `gyroBiasSlope == 0`, `biasModel.temperatureLinear == false` and no `slope_prior` residual; with `gyroBiasModelFor(window)` on the same window, `biasModel.tRef` is the window's mean temperature, the last residual is `slope_prior`, and `values` holds `T(0)`
- [ ] `buildFactorGraph()` is called exactly `passes + 1` times per fit (Phase 3's rule; one site before the loop, one inside), both through `linearizationOf()`
- [ ] The initializer's prefix and segment fits are unchanged in code and in numbers: Phase 5's `initializer` diagnostics of the three success goldens (segment bounds, prefix lengths, `prefix_fits`, yaw sigmas, iterations) are identical after Task 6.10's re-capture

**Complexity:** L

---

### Task 6.5: The pipeline chooses the model; the reconstruction uses the interval bias

**Purpose:** `planFit()` computes `T_ref` from the fitted window and the full fit alone receives the temperature model; the dense reconstruction propagates each interval's attitude with that interval's gyro bias.

**Files to modify:**
- `src/fusion/fusion.cpp` — `FitPlan` (30-35, as Phase 5 shaped it) gains `GyroBiasModel biasModel`; `planFit()` (42-58) sets it; `fitAndAssemble()` (61-81) passes it to `fitFactorGraph()`
- `src/fusion/trajectoryreconstruction.h` / `.cpp` — `reconstructTrajectory()` (36-70) and `propagateThroughInterval()`'s caller (45); the header comment (25-31)

**Technical Approach:**

`planFit()`: after `requireNoGnssOutage(...)`, `plan.biasModel = gyroBiasModelFor(plan.window);` with the comment: `T_ref` is a property of the fitted window (the mean of its IMU samples' temperature), decided before the fit starts. `fitAndAssemble()`: the full-fit call becomes `fitFactorGraph(plan.window, init.state, plan.tuning, QString::fromLatin1(kFullFitPassFormat), checkpoint, plan.biasModel)`. The initializer call `initialize(plan.window, plan.tuning, checkpoint)` is unchanged: `initialize()` never sees the model and its fits take the default (Decisions Made). Nothing in `runPipeline()`'s handlers changes.

`reconstructTrajectory()`: line 39 reads `bias = fit.values.at<ConstantBias>(B(0))`; keep it for the accelerometer bias (line 62, `d.force[sample] - bias.accelerometer()`, unchanged: the accelerometer bias is constant). For the gyro, line 45 becomes `propagateThroughInterval(d, edges, startPose.rotation(), intervalBias(d, k, bias, fit.gyroBiasSlope, fit.biasModel).gyroscope())`: the interval `[gnssTime[k], gnssTime[k+1])` is propagated with the bias of its first fix `k`, the same bias its IMU factor was evaluated at. Under the constant model `intervalBias()` returns `bias` itself (a hand-built `FitResult` in the kernel tests). The endpoint correction, the display interpolation and the sample selection are untouched. The header comment of `reconstructTrajectory()` gains "the gyro bias of an interval is the model's bias at that interval's first fix".

**Acceptance Criteria:**
- [ ] `fusion.cpp` calls `gyroBiasModelFor()` once, in `planFit()`, and passes `plan.biasModel` to exactly one `fitFactorGraph()` call; `initialize()`'s call is unchanged (`git diff` on the initializer's call line is empty)
- [ ] `reconstructTrajectory()` calls `intervalBias()` once per interval and still reads the accelerometer bias from `B(0)`; `reconstructionTimingAndEndpointCorrection` passes unchanged (its `FitResult` has the default model)
- [ ] Task 6.8's `reconstructionUsesIntervalBias` holds: on a hand-built `FitResult` whose second attitude is the first propagated with the interval's temperature-dependent bias, the endpoint correction is below `1e-9` degrees under the temperature model and above `0.5` degrees when the same result is reconstructed under the constant model (the per-interval bias is what makes the gyro integration land on the fitted attitude; on `drifting_bias` itself the 1 s intervals make the two reconstructions differ by at most half a degree, which is why the proof is a unit test, not the fit)
- [ ] The nine `reject_*.json` are unchanged by the re-capture except for the algorithm string (the model is computed after every rejection check)

**Complexity:** S

---

### Task 6.6: Diagnostics: `model.gyro_bias`, the algorithm string

**Purpose:** The `model.gyro_bias` object of the overview's key layout, reduced to what a fit can now report (Decisions Made: `b1_fixed_zero` is dropped, the two numbers are never `null`); `seeds[0].gyro_bias_rad_s` keeps its name and now means `b0`; the algorithm string names the model in success and failure shapes alike.

**Files to modify:**
- `src/fusion/fusionoutput.cpp` — `kAlgorithm` (line 17), `modelSummary()` (Phase 4's helper), `seedSummary()`'s comment (24-27), `successDiagnostics()` (84-110, as Phases 3-5 shaped it)

**Technical Approach:**

- `kAlgorithm = "batch-temperature-bias-v3"`. Both `successDiagnostics()` and `failureDiagnostics()` read it, so every shape changes with one edit.
- `modelSummary(const Tuning &tuning, const FitResult &fit)` (Phase 4 declared it over `Tuning` alone and said `gyro_bias` would be one more entry: the entry needs the fit, so the helper takes it; its single call site passes `fit`). Its object becomes `{{"per_step", ...unchanged...}, {"gyro_bias", gyroBiasObject(fit)}}` with, in the anonymous namespace:

  ```cpp
  /// The fitted gyro bias model: b0 is B(0)'s gyro part (also seeds[0].gyro_bias_rad_s),
  /// b1 the slope per degC, t_ref the reference temperature of the fitted window.
  QJsonObject gyroBiasObject(const FitResult &fit)
  ```
  writing exactly the keys `b0_rad_s` (the array `toJsonArray(bias.gyroscope())`, the same expression `seedSummary()` uses, so the two arrays are the same bits), `b1_rad_s_per_degc` (`toJsonArray(fit.gyroBiasSlope)`), `t_ref_degc` (`fit.biasModel.tRef`). The full fit always runs the temperature model, so no `null` and no flag; the helper does not consult `temperatureLinear` (a `FitResult` of the stock path never reaches the diagnostics).
- `seedSummary()`: no change to the entries; the comment gains "`gyro_bias_rad_s` is `b0`, the bias at the reference temperature; the slope is under `model.gyro_bias`".
- `successDiagnostics()`: the `{"model", modelSummary(tuning, fit)}` entry; nothing else. `failureDiagnostics()` untouched (Phase 3's shapes; the model is not reported on a failure). The input audit is untouched (Task 6.1).

**Acceptance Criteria:**
- [ ] A successful fit's `model.keys()` is `{gyro_bias, per_step}` (sorted) and `model.gyro_bias.keys()` is `{b0_rad_s, b1_rad_s_per_degc, t_ref_degc}`; `b0_rad_s` equals `seeds[0].gyro_bias_rad_s` element for element; `b1_rad_s_per_degc` is a three-element array and `t_ref_degc` a number (Task 6.8)
- [ ] `algorithm == "batch-temperature-bias-v3"` in success, rejection and solver-failure diagnostics (`grep -rn "batch-shared-bias-v2" src tests` prints nothing after Task 6.8's literal updates and Task 6.10's re-capture)
- [ ] Phase 3's `failureDiagnosticsShape` and Phase 4's `diagnosticsReportPerStepConstants` pass with their literals updated as Task 6.8 states

**Complexity:** S

---

### Task 6.7: Registration, fixtures, session support, session and runner tests

**Purpose:** `IMU/temperature` joins the fit's input table as the required twenty-second input and flows through the one channel assembly to the engine path and the runner; every synthetic recording carries a temperature so it still fits; every place a fixture becomes a session or a CSV carries the column; the session tests prove that a session without the column is blocked like any missing input and that one with it carries the channel bit for bit; the runner test sees the column in the dump.

**Files to modify:**
- `src/fusion/fusionregistration.cpp` — `kFitInputs[]` (62-80)
- `tests/fusion/fusionfixtures.h` / `.cpp` — `FusionFixture` (18-25), the rules comment (5-15), `appendImu()` (63-70), the three success generators, `imuChannels()` (202-205), the four initializer generators of Phase 5
- `tests/fusion/fusiongolden.cpp` — `toChannels()` (290-304)
- `tests/fusion/fusionsessions.h` / `.cpp` — the header comment (19-24, 38-47), `addImuSide()` (48-57), `inputsMatchFixture()` (69-94), `naturalSession()` (131-173)
- `tests/tst_fusion_session.cpp` — `registrationShape()` (188-251), `inputsAreBitIdenticalToFixture()` (253-300), `missingInputsAreNotApplicable_data()` / `missingInputsAreNotApplicable()` (639-690), one new slot `temperatureReachesTheKernel()`
- `tests/tst_fusion_runner.cpp` — `successMatchesDirectRun` (Phase 2, Task 2.5 item 3)

**Technical Approach:**

*Registration*: `kFitInputs[]` gains the row `{ "IMU", "temperature", &Fusion::Channels::imuTemperature }` after `wz`. `fitInputs()`, `channelsFrom()` and `computeFit()` are unchanged: the table drives them (Phase 2). `fusion_runner --dump-inputs` prints the new label and values from the table (Phase 2, Task 2.3); no runner edit.

*Fixtures*: `FusionFixture` gains `QVector<double> imuTemperature; ///< IMU/temperature, degC, per imuTime sample` after `wx, wy, wz`; the header's "twenty-one inputs" becomes "twenty-two". In `fusionfixtures.h` add `constexpr double kFixtureTemperatureDegC = 25; ///< the constant IMU temperature of every fixture that is not about the temperature (exactly representable; no noise)` so the tests can name it. The rules comment (line 14) "Accuracies and times carry no noise" becomes "Accuracies, times and the temperature carry no noise". `appendImu()` gains a trailing `double temperature` parameter appended to `f.imuTemperature`; the three success generators pass `kFixtureTemperatureDegC` (their doc comments gain "IMU temperature 25 degC throughout"); the nine rejection fixtures inherit it, and `imuChannels()` gains `&f.imuTemperature` so that `reject_coverage`'s truncation and `reject_imu_gap`'s removal keep the temperature aligned with `imuTime` (otherwise those two would be rejected for a mismatched temperature instead of their own defect: Gotchas). Phase 5's four initializer generators: `motion_start`, `rest_throughout`, `sacc_anchor` pass `kFixtureTemperatureDegC`; `drifting_bias` passes `30 + t / 20` — a linear ramp from 30 degC at `t = 0` to 40 degC at `t = 200`, so the z-axis bias `.3 + t / 200` deg/s is `.3 + (T - 30) / 10` deg/s: exactly `0.1 deg/s per degC`; its generator comment states the ramp, the implied `b1 = (0, 0, 0.1 deg/s/degC)`, `T_ref = 35` (the mean of `30 + i * .1 / 20` over `i = 0..2000`) and `b0 = (.2, -.15, .8) deg/s` at `T_ref`. `toChannels()`: add `c.imuTemperature = f.imuTemperature;` after the gyro line. `capture.json`'s two `fixture_generator_sha256_lf` entries change (Task 6.10).

*Session support*: `addImuSide()`: `if (!f.imuTemperature.isEmpty()) session.setSourceMeasurement("IMU", "temperature", f.imuTemperature, "deg C");` (the device's unit text; the conversion layer serves it unchanged with label `degC`; the conditional exists so that a test can build a session lacking the column from a fixture copy with the array cleared). `inputsMatchFixture()`: in the `withImu` block add `&& sameSamples(probe.getMeasurement("IMU", "temperature"), f.imuTemperature)`. `naturalSession()`: after the IMU columns, `session.setSourceMeasurement("IMU", "temperature", QVector<double>(imuTime.size(), 25.0), "deg C");` and its header comment lists the column (a recording as the importer leaves it has one). The header comment lists `IMU/temperature ("deg C")` among the stored inputs and says twenty-two.

*Session tests*:
1. `registrationShape()`: insert `CalcInput::measurement("IMU", "temperature")` after `CalcInput::measurement("IMU", "wz")` in the literal list; `QCOMPARE(inputs.size(), 22)`; `QVERIFY(fit->descriptor->inputs == inputs)` unchanged.
2. `inputsAreBitIdenticalToFixture()`: add `{"IMU", "temperature", &f.imuTemperature}` to the table, `QCOMPARE(names.size(), 22)`, and change the strides to `{3, 5, 7}` (coprime with 22; the comment says so).
3. `missingInputsAreNotApplicable_data()`: a fourth row `QTest::newRow("no IMU temperature") << QStringLiteral("no-temperature")`; in the test, `FusionFixture f = fusionFixture("coarse_linear"); f.imuTemperature.clear(); session = sessionFromFixture(f, "m1"); QVERIFY(session.hasSensor("IMU")); QVERIFY(!session.hasMeasurement("IMU", "temperature"))` (or the equivalent source-layer query the file uses), then the existing assertions of the row run unchanged: `readiness == MissingInput` with no blockers, every plot name `NotApplicable`, `prepare` is `NothingToRun` / `MissingInput`, `request` is `MissingInput`, nothing available, `runCount == 0`. This is spec section 10's "a recording without a temperature channel" on the engine side (Decisions Made).
4. `temperatureReachesTheKernel()` (new slot, after `missingInputsAreNotApplicable`): `sessionFromFixture(initializerFixture("drifting_bias"), "d1")`: `getMeasurement("IMU", "temperature")` is bit-identical to the fixture's `imuTemperature` (`sameBitsEverywhere`) and its effective unit is `degC`; `engine.readiness(kFit).state == Ready`; `request(kFit).status == Ok`; `compareJson("diagnostics", diagnosticsOf(session), QJsonDocument::fromJson(Fusion::run(toChannels(fixture)).diagnosticsJson.toUtf8()).object())` is empty (the engine path and the direct call agree, temperature included; with the default `Tuning` this is one 201-state fit in a single 600 s segment: seconds); `model.gyro_bias.t_ref_degc` within `1e-9` of `35`. Destroy nothing special: the session is a local.

*Runner test* (`tests/tst_fusion_runner.cpp`, `successMatchesDirectRun`): the `writeRecording()` helper needs no edit — it splits `sourceData()` by sensor and `DataExporter` writes the IMU columns from its table, which includes `temperature` (`src/dataexporter.cpp` line 34) — but the test must prove the column made the round trip: add to the dump assertions that the `IMU/temperature` line parses to `fixture.imuTemperature` bit for bit (next to the existing `IMU/wx` check), and note in the test's comment that the twenty-two labels are derived from `fitInputs()`. If the export dropped the column the tool would exit 1 with `Missing or mismatched IMU/temperature` and the existing exit-0 assertion would already show it. `matchesTheApplicationImportPath` (on `naturalSession`, which now stores the column), `rejectionExitsOne` (expected reason from the direct run) and `legacySchemaScalesTheGyro` (the temperature is not schema-dependent; its dump line stays bit-identical to the fixture, which the test may assert alongside `IMU/ax`) pass without further edits; if any assertion hard-codes the input count, correct the literal to 22 and report it.

**Acceptance Criteria:**
- [ ] `fitInputs()` returns 22 entries: the 17 measurements of `master`, then `IMU/temperature`, then the four origin attributes; `registrationShape` asserts the list and the count
- [ ] Every fixture of `fusionFixtures()` and `initializerFixture()` has `imuTemperature.size() == imuTime.size()`: constant `25` except `drifting_bias` (first value `30`, last `40`); `reject_coverage` and `reject_imu_gap` still reject with their goldens' reasons; `fixturesAreDeterministic` and `initializerFixturesAreDeterministic` compare the new array too
- [ ] `tests/fusion/fusionfixtures.cpp` still contains no `std::sin`, `std::cos`, `std::sqrt`, `std::pow` or `<random>`
- [ ] `missingInputsAreNotApplicable`'s `no-temperature` row passes with the row's existing assertions; `temperatureReachesTheKernel` passes; `blockersReportFusion`, `requestRunsOnceAndPublishesTogether`, `asyncMatchesSync`, `rejectionIsACachedResult`, `naturalSessionEndToEnd` pass unchanged in code against the re-captured goldens
- [ ] `tst_fusion_runner::successMatchesDirectRun` asserts the `IMU/temperature` dump line against the fixture bit for bit and passes; `matchesTheApplicationImportPath` passes with the column stored in `naturalSession`

**Complexity:** M

---

### Task 6.8: Kernel tests: the factor's Jacobians and the section 6 cases

**Purpose:** Spec section 10's temperature expectations as this phase reads them (a recording without the channel is rejected; a constant temperature leaves `b1` at its prior; the drifting-bias recording recovers `b1`), the overview's risk note (verify the Jacobians numerically before the fit test), the validation additions, and the literal updates the algorithm string and the `model` object force on earlier phases' tests.

**Files to modify:**
- `tests/tst_fusion_kernel.cpp` — header comment (1-8), includes, four new slots, `validationRejectsEachDefect` (Phases 3-5's version), `driftingBiasSegmentsConverge` (Phase 5, Task 5.8 item 4), the algorithm and `model.keys()` literals of Phases 3 and 4

**Technical Approach:**

Includes: `"fusion/temperatureimufactor.h"`, `<gtsam/base/numericalDerivative.h>`, `<gtsam/navigation/ImuFactor.h>`, `<gtsam/slam/PriorFactor.h>`, `<gtsam/navigation/GPSFactor.h>` as needed (this file is in the audit's allowlist and links `gtsam`). `using gtsam::symbol_shorthand::T;` next to `B`, `V`, `X`. Header comment: add "the temperature-dependent gyro bias (the custom factor's Jacobians, the section 6 cases)". Hand-built `Samples` of this file (`boundarySamples`, `linearSamples`) keep no temperature unless a test sets one: the stock path accepts that.

1. **`temperatureFactorJacobians`** (first: the overview's risk note). `Samples d = boundarySamples(Vector3(1, -2, .5))` with every `gyro` sample set to `Vector3(.1, -.05, .2)` (a non-trivial rotation; the constant-force builder is otherwise fine), `validateSamples(d, Tuning{})`. `const ConstantBias linearizedAt(Vector3(.05, -.03, .08), Vector3(.003, -.002, .004));` `pim = preintegrateImu(d, .037, .863, linearizedAt, Tuning{})`; `dT = 4.5`; `TemperatureImuFactor factor(X(0), V(0), X(1), V(1), B(0), T(0), pim, dT)`. Evaluation point, all different from the linearization so every Jacobian is non-trivial: `pose_i = Pose3(Rot3::RzRyRx(.3, -.2, .1), Vector3(1, 2, 3))`, `vel_i = (2, -1, .5)`, `pose_j = Pose3(Rot3::RzRyRx(.35, -.15, .12), Vector3(2.5, 1.2, 3.4))`, `vel_j = (2.6, -2.4, .9)`, `bias = ConstantBias((.04, -.02, .07), (.002, -.001, .005))`, `slope = (2e-4, -1e-4, 3e-4)`. Analytical: `gtsam::Matrix H1..H6; factor.evaluateError(pose_i, vel_i, pose_j, vel_j, bias, slope, &H1, &H2, &H3, &H4, &H5, &H6)`. Numerical: a lambda calling `factor.evaluateError(...)` without Jacobians, assigned to a `std::function<gtsam::Vector9(const Pose3&, const Vector3&, const Pose3&, const Vector3&, const ConstantBias&, const Vector3&)>` (the helpers take the `std::function` type and cannot deduce it from a lambda; the dynamic `Vector` the factor returns converts to `Vector9` on assignment), through `gtsam::numericalDerivative61<gtsam::Vector9, Pose3, Vector3, Pose3, Vector3, ConstantBias, Vector3>(h, pose_i, vel_i, pose_j, vel_j, bias, slope)` ... `numericalDerivative66` (default delta `1e-5`; the perturbations are the manifolds' own retracts, the same tangents the analytical Jacobians are taken in; GTSAM's `testImuFactor` checks `ImuFactor` the same way). Assert for each block `(analytic - numeric).cwiseAbs().maxCoeff() < 1e-6` (entries are of order 1 to 10), `H6.rows() == 9 && H6.cols() == 3`, and `H6.cwiseAbs().maxCoeff() > 1e-3` (not vacuous: `dT` and the rotation Jacobian are non-zero). Then the equivalence with `ImuFactor`: `gtsam::ImuFactor stock(X(0), V(0), X(1), V(1), B(0), pim)`; with `slope = Vector3::Zero()`, `factor.evaluateError(..., Zero, &H1..&H6) == stock.evaluateError(pose_i, vel_i, pose_j, vel_j, bias, &G1..&G5)` (Eigen `==`, bitwise for finite values) for the error and `H1..H5 == G1..G5`, `H6.isZero(0)`; the same with `dT = 0` (a second factor) and the non-zero slope; and, on a `Values` holding the six keys at the evaluation point, `factor.whitenedError(values) == stock.whitenedError(values)` (the noise model is the same covariance). Finally `factor.clone()` is non-null and its `error(values)` equals `factor.error(values)`.

2. **`temperatureGraphShape`**: `d = boundarySamples(Vector3(1, -2, .5))` with `d.temperature` = `40 + .01 * i` per IMU sample (101 values; any finite series), `Tuning{}`; `model = gyroBiasModelFor(d)`: `temperatureLinear == true`, `tRef` within `1e-12` of the mean the test computes by the same index-order loop; `graph = buildFactorGraph(d, BiasLinearization{ConstantBias(), Vector3::Zero()}, model, Tuning{})`: `graph.size() == 7`; `dynamic_cast<const TemperatureImuFactor *>(graph.at(4).get())` non-null with `temperatureDelta()` equal to `temperatureAtFix(d, 0) - model.tRef` (bitwise), `graph.at(5)` a `PriorFactor<ConstantBias>`, `graph.at(6)` a `PriorFactor<Vector3>` on `T(0)` whose noise sigmas are all `Tuning{}.gyroBiasSlopeSigma`; `buildFactorGraph(d, ConstantBias(), Tuning{})` (four arguments) has `graph.size() == 6` with an `ImuFactor` at 4. `intervalBias(d, 0, bias, slope, GyroBiasModel{})` returns `bias` bitwise; under `model` with `slope = (1e-3, 0, 0)` its gyro x equals `bias.gyroscope().x() + 1e-3 * (temperatureAtFix(d, 0) - model.tRef)` (`QCOMPARE`). `temperatureAtFix(d, 0)` equals `interpolateAt(d.imuTime, d.temperature, .037)` and, converting the series to `Vectors` `(v, 0, 0)`, the vector overload's `.x()` bit for bit. `fittedWindow(d, .037, .863).temperature.size() == fittedWindow(...).imuTime.size()`; with `d.temperature.clear()`, the window's temperature is empty and `gyroBiasModelFor(d)` throws `std::invalid_argument`. Also: `gyroBiasModelFor()` on a series of 101 copies of `25.` yields `tRef == 25.` exactly (`QCOMPARE`).

3. **`reconstructionUsesIntervalBias`** (the proof of Task 6.5, the pattern of `reconstructionTimingAndEndpointCorrection`). `Samples d = boundarySamples(Vector3::Zero())` with every `gyro` sample `Vector3(.01, -.02, .03)` and `d.temperature` = `45.0` at every IMU sample; `GyroBiasModel model{true, 35.0}` (so `dT = 10` at fix 0); `slope = Vector3(0, 0, 2e-3)` rad/s/degC; `g0 = Vector3(.01, -.02, .03)` (the bias the gyro reads, so the interval bias `g0 + slope * dT = (.01, -.02, .05)` differs from the reading and the propagation rotates); `X(0)` identity, `X(1)` = `Pose3(propagateAttitude(d, Rot3(), .037, .863, intervalBias(d, 0, ConstantBias(Zero, g0), slope, model).gyroscope()), Zero)`, `V(0) = V(1) = 0`, `B(0) = ConstantBias(Zero, g0)`, `T(0) = slope`; `FitResult fit` with these `values`, `fit.gyroBiasSlope = slope`, `fit.biasModel = model`. `reconstructTrajectory(d, fit).endpointCorrection[0] < 1e-9` (the reconstruction propagates with the very bias `X(1)` was built from). Then `fit.biasModel = GyroBiasModel{}` (and `gyroBiasSlope` zero): `endpointCorrection[0] > .5` (propagating with `g0` alone leaves `|slope dT| x .826 s = .0165 rad = .95` degrees uncorrected; the `.5` bound is the stated margin).

4. **`driftingBiasSegmentsConverge`** (Phase 5's slot; the fixture now carries the ramp). Keep every Phase 5 assertion (four segments, all converged, stock prefix fits: `prefix_length_s == 60`, `prefix_fits == 4`, `fallback_segments` empty, `Succeeded`). Add, from the diagnostics: `seeds[0].iterations <= 30` (the sum over all passes of the full fit, `= trace.history.size()`; Decisions Made) and `trace.converged`; `b1 = model.gyro_bias.b1_rad_s_per_degc` a three-element array with, for `b1z = .1 * kPi / 180` (`1.7453292519943295e-3`, the fixture's construction), `|b1[2] - b1z| <= .2 * b1z` (the spec's 20 %), `|b1[0]| <= .2 * b1z`, `|b1[1]| <= .2 * b1z`; `|t_ref_degc - 35| < 1e-9`; `b0_rad_s` equals `seeds[0].gyro_bias_rad_s` element for element, and `|b0_rad_s[2] - .8 * kPi / 180| < .1 * kPi / 180` (the bias at `T_ref`, within 0.1 deg/s); the `residuals` array's last entry has `kind == "slope_prior"` and the one before it `kind == "bias_prior"`; the number of `imu` residuals is 200. From the channels: the attitude never rotates, so `|remainder(result.roll[i], 360)| < 2`, `|pitch| < 2` and `|remainder(result.yaw[i] - result.yaw[0], 360)| < 2` for every output sample (a sanity bound on the whole fit; the per-interval bias of the reconstruction is proven by `reconstructionUsesIntervalBias`). `qInfo()` the fitted `b1`, `b0`, `t_ref` and the iteration count.

5. **`constantTemperatureKeepsSlopeAtPrior`** (the spec's "constant temperature" case). `Tuning t; t.segmentLength = 60; t.minFinalSegment = 12;` (Phase 5's). `FusionFixture f = initializerFixture("drifting_bias"); f.imuTemperature = QVector<double>(2001, 35.0);` `runPipeline(toChannels(f), t, Checkpoint(), &trace)`: `Succeeded`; `algorithm == "batch-temperature-bias-v3"`; every `|b1[i]| < .01 * Tuning{}.gyroBiasSlopeSigma` (`1.745e-6` rad/s/degC: with `T_k - T_ref` exactly zero at every fix the factor's `H6` is zero, the slope's normal equation is its prior's alone with a zero right-hand side, so every LM step leaves it at `0.0`; the bound is 1 % of the prior sigma as the margin); `QCOMPARE(t_ref_degc, 35.)` (2001 copies of 35: the sum 70035 and the quotient are exact); the last residual is `slope_prior` with `squared_whitened_error < 1e-10`; `b0_rad_s == seeds[0].gyro_bias_rad_s`. Then the consistency check with the stock path through the internal seams: `window` built as Phase 5's `windowOf()` does from `toChannels(f)` (`prepareInput`, `fittedWindow(recording, usableStart, gnssTime.back())`, `validateSamples` with the derived `maxGap`), `init = initialize(window, t)`, `stock = fitFactorGraph(window, init.state, t)` (the default constant model): `|stock.objective - objective| <= 1e-6 * max(1, objective)` (the same model at `b1 = 0`; both converge under the settle tolerance; `qInfo()` both).

6. **`validationRejectsEachDefect`**: add `Tuning t; t.gyroBiasSlopeSigma = 0;` throws and `= -1` throws; `Samples bad = d; bad.temperature = std::vector<double>(bad.imuTime.size() - 1, 20.);` throws; `bad.temperature = std::vector<double>(bad.imuTime.size(), 20.); bad.temperature[3] = NaN;` throws; `bad.temperature.assign(bad.imuTime.size(), 20.)` does not throw. Through the pipeline on `coarse_maneuver`'s channels (spec section 10's "a recording without a temperature channel", kernel side): `c.imuTemperature.clear()` → `Rejected`, `reason == "Missing or mismatched IMU/temperature"`; `c.imuTemperature.removeLast()` → the same reason; `c.imuTemperature[5] = NaN` → `Rejected`, `"Nonfinite IMU/temperature"`; `c.imuTemperature.clear(); c.wz[0] = infinity` → `"Nonfinite IMU/wz"` (the order); the unmodified channels → `Succeeded`.

7. **Literal updates**: every `"batch-shared-bias-v2"` in this file (Phase 3's `biasSettledByCostTest`, `failureDiagnosticsShape`) becomes `"batch-temperature-bias-v3"`; Phase 4's `diagnosticsReportPerStepConstants` expects `diagnostics["model"].toObject().keys() == {"gyro_bias", "per_step"}` (sorted) and additionally `model.gyro_bias.t_ref_degc == 25` (`kFixtureTemperatureDegC`, exact for a constant series) on `coarse_linear`; Phase 5's `initializerDiagnosticsShape` key-set assertion for the top level is unchanged (no new top-level key). `grep -rn "batch-shared-bias-v2" tests` must print nothing after this task; `tst_fusion_session` and `tst_fusion_golden` compare the string with the goldens, not literals. Phase 3's `biasSettledByCostTest` asserts `stopping.passes <= 2` and Phase 3's / 5's iteration literals count the full fit: they are expected to hold with `T(0)` in the graph (Task 6.10 says what to do if one flips).

8. **`initializerFixturesAreDeterministic`** (Phase 5) and `tst_fusion_golden::fixturesAreDeterministic`: include `imuTemperature` among the compared arrays.

Register the new slots in the class declaration after Phase 5's, in the order 1, 2, 3, 5 (4 and 6-8 are edits of existing slots).

**Acceptance Criteria:**
- [ ] `temperatureFactorJacobians` holds all six blocks within `1e-6` of finite differences with `H6` non-trivial, and the bitwise equivalence with `ImuFactor` at zero slope and at zero `dT`
- [ ] `temperatureGraphShape` asserts the factor order with the slope prior last and the stock four-argument builder unchanged; `reconstructionUsesIntervalBias` asserts the `1e-9` / `.5` degree pair
- [ ] `driftingBiasSegmentsConverge` asserts `b1z` within 20 %, the off-axis components below 20 % of `b1z`, `seeds[0].iterations <= 30`, `t_ref_degc` 35, the residual kinds and the 2-degree attitude bound; `constantTemperatureKeepsSlopeAtPrior` asserts `|b1| < 1 %` of the prior sigma, `t_ref_degc == 35` and the stock-path objective agreement; `validationRejectsEachDefect` asserts the two `IMU/temperature` reasons and the order
- [ ] No expected value is derived from running the implementation: `b1z`, `b0z`, `T_ref` and the attitude come from the fixture's construction (the assertions' comments say from what); solver arithmetic is compared only through inequalities with stated margins, `withinPortableBound()` or `sameRecomputedValue()`
- [ ] `ctest -C Release -R "tst_fusion_kernel(_exact)?$" --output-on-failure` passes after Task 6.10; `tst_fusion_kernel` alone finishes in under 150 s on the capture machine (Phase 5's budget plus three 201-state fits and one Jacobian evaluation)

**Complexity:** L

---

### Task 6.9: Documentation that must not lie

**Purpose:** Phase 7 writes the prose; until then every statement this phase falsifies is corrected minimally in place, and the key list and input list say what the kernel now reads and writes.

**Files to modify:**
- `docs/SENSOR_FUSION.md` — section 3 (lines 45-57 on `master`: the input list and its first paragraph), section 4 (the first paragraph, 50-57; the key list, 95-104, as Phases 3-5 reworded it)
- `tests/README.md` — section 1's `tst_fusion_kernel` and `tst_fusion_session` rows (123-124 on `master`, in Phases 1-5's wording); section 11's fixture table, the three success rows' "Exercises" cells
- `tests/fusion/fusionsessions.h`, `tests/fusion/fusionfixtures.h` — the "twenty-one" comments (Task 6.7 edits them; listed here for the grep)

**Technical Approach:**

`docs/SENSOR_FUSION.md`, exact edits:
1. "The fit declares twenty-one inputs, all required, and reads nothing else:" becomes "The fit declares twenty-two inputs, all required, and reads nothing else:"; the IMU line of the list becomes `IMU/ax  IMU/ay  IMU/az  IMU/wx  IMU/wy  IMU/wz  IMU/temperature`. After the "Effective values only" paragraph add: "**Temperature.** `IMU/temperature` is the IMU's own temperature in degrees Celsius, as recorded; it is a column of every FlySight 2 `SENSOR.CSV`. A file without it lacks a declared input, and the fit does not run, like any other missing input."
2. Section 4, first paragraph: "one shared accelerometer bias, and one shared gyroscope bias" becomes "one shared accelerometer bias, and a gyroscope bias `b0 + b1 (T - T_ref)` of the IMU temperature `T`, with `T_ref` the mean temperature over the fitted window"; "Adjacent states are connected by standard `ImuFactor` preintegration." becomes "Adjacent states are connected by `ImuFactor` preintegration in the form that takes `b0` and `b1`, each interval evaluated at the bias of its first fix."; Phase 5's prior sentence "The zero-centered bias prior has sigmas 0.3 m/s^2 and 0.03 rad/s; the initializer only chooses where the solver starts." becomes "The zero-centered bias priors have sigmas 0.3 m/s^2, 0.03 rad/s and, for `b1`, 0.010 deg/s per degC; the initializer only chooses where the solver starts (its own segment fits hold the gyro bias constant)."
3. Key list: Phase 4's "`model` (`per_step`: the per-step noise constants `gyro_slope_s` and `acc_slope_s`)" becomes "`model` (`per_step`: the per-step noise constants `gyro_slope_s` and `acc_slope_s`; `gyro_bias`: `b0_rad_s`, `b1_rad_s_per_degc`, `t_ref_degc`)".

`tests/README.md`:
1. `tst_fusion_kernel` row: after Phase 5's initializer clause insert ", the temperature-dependent gyro bias (the custom IMU factor's six Jacobians against finite differences and its equivalence with `ImuFactor` at zero slope; a recording without the temperature channel is rejected by name, a constant temperature leaves `b1` at its prior and agrees with the constant-bias fit, the drifting-bias recording recovers `b1` within 20 % in at most 30 iterations)".
2. `tst_fusion_session` row: "(21 inputs, 18 outputs" becomes "(22 inputs, 18 outputs"; clause (11) "sessions without IMU data, without a local origin or without a time fit" becomes "sessions without IMU data, without a local origin, without a time fit or without `IMU/temperature`"; after it insert "; a session with the temperature column carries it to the kernel bit for bit and matches the kernel's direct run".
3. Fixture table, the three success rows: append "; a constant 25 degC IMU temperature (`model.gyro_bias`: `b1` at its prior, `t_ref_degc` 25)" to each "Exercises" cell. Rows stay on one line.

Nothing else: the re-capture procedure, the tolerance policy (no new exact keys: Decisions Made) and the acceptance map are untouched (the map is Phase 7's; this phase renames no mapped function).

**Acceptance Criteria:**
- [ ] `grep -rn "twenty-one" docs/SENSOR_FUSION.md tests/fusion/fusionsessions.h tests/fusion/fusionfixtures.h` prints nothing; `grep -n "twenty-two" docs/SENSOR_FUSION.md` hits section 3
- [ ] `grep -n "gyro_bias\|IMU/temperature" docs/SENSOR_FUSION.md` hits the input list, the temperature paragraph, the model paragraph and the key list; `grep -n "optional" docs/SENSOR_FUSION.md` prints nothing new
- [ ] `grep -n "temperature" tests/README.md` hits the two section 1 rows and the three fixture rows
- [ ] `ctest -C Release -L audit` passes

**Complexity:** S

---

### Task 6.10: Re-capture the goldens

**Purpose:** Every numerical phase ends by re-capturing the goldens with the Phase 1 recipe. Every golden fixture now carries a 25 degC temperature, so every full fit runs the temperature graph with `T(0)` and its prior: the diagnostics gain `model.gyro_bias`, and the numbers may move in their last digits.

**Files to modify:**
- `tests/data/fusion/capture.json` — rewritten by the tool (date, revision, hashes, both `fixture_generator_sha256_lf` entries: Task 6.7 edited `fusionfixtures.h` and `.cpp`)
- `tests/data/fusion/coarse_linear.json`, `coarse_maneuver.json`, `stationary_spin.json` — rewritten: the `algorithm` string, the `model.gyro_bias` object, and possibly last-digit changes of solver arithmetic (`objective`, biases, `residuals`, `quality`, `stopping` measurements, `trace.history` costs, `max_endpoint_correction_deg`)
- `tests/data/fusion/coarse_linear.channels.txt`, `coarse_maneuver.channels.txt`, `stationary_spin.channels.txt` — rewritten; may change in the last bits, may not
- The nine `reject_*.json` — rewritten: the `algorithm` string only

**Technical Approach:**

Run Tasks 6.1-6.9 first (build green; `tst_fusion_kernel` green except `fitTraceMatchesGolden` and the tests that compare diagnostics with goldens; `tst_fusion_golden`, `tst_fusion_session`, `tst_fusion_jobs`, `tst_fusion_rows`, `tst_fusion_runner` red on the golden comparisons only, because the goldens still say `batch-shared-bias-v2` and lack the new keys). Then the recipe, quoted verbatim from Phase 1 (`01-golden-regression-harness.md`, "The re-capture recipe (quote this in Phases 3-6)"):

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

*What the capture is expected to change, and why.* On a constant 25 degC series `T_ref` is exactly 25 and `T_k - T_ref` is exactly `0.0` at every fix, so every temperature factor's `pim`, error and `H1..H5` are bit-identical to the stock `ImuFactor`'s and `H6` is exactly zero; the slope prior's residual is zero at `T(0) = 0`, its right-hand side is zero and its column is decoupled from every other variable, so every LM step leaves `T(0)` at exactly `0.0` and the per-factor errors summed for the cost are the stock values plus an exact zero. What is not argued: the linear solves. The graph has one more variable and one more factor, so `Ordering::Colamd` may order the other variables differently and the multifrontal QR groups its frontal matrices differently; the same normal equations solved along a different elimination path differ in rounding, and LM's damping and step acceptance then see slightly different numbers. The expectation is therefore the weaker one: the three success fixtures keep their outcome, their pass and iteration counts and hence their progress sequences (the settle and cost tests have margins of several orders of magnitude over rounding on these fixtures: Phase 3's analysis of their histories), `T(0)` is exactly zero in the goldens (`b1_rad_s_per_degc` all `0` or `-0`), `t_ref_degc` is exactly `25`, and every other number is either identical or moved in its last digits (well inside the portable bound, as Phase 3's `coarse_maneuver` change was). If the capture shows the three `.channels.txt` files unchanged and every number identical, that is byte identity observed, not promised: report it as such.

| File | Expected change |
|---|---|
| `capture.json` | date, revision, hashes (the `.channels.txt` hashes only if their bits moved), both `fixture_generator_sha256_lf` entries |
| `coarse_linear.json`, `coarse_maneuver.json`, `stationary_spin.json` | `algorithm` -> `batch-temperature-bias-v3`; `model` gains `gyro_bias` (`b0_rad_s` the same three numbers as `seeds[0].gyro_bias_rad_s`; `b1_rad_s_per_degc` three zeros; `t_ref_degc` 25); `initializer` identical (the segment fits are stock); `progress` identical; `stopping.passes`, `seeds[0].iterations`, `history` row count identical; numbers of solver arithmetic identical or last-digit changes |
| nine `reject_*.json` | `algorithm` only (`git diff --numstat` `1 1` each) |
| three `.channels.txt` | unchanged or last-bit changes (not required either way) |

Checks after step 5:

```bash
git status --porcelain -- tests/data/fusion/
# expected: capture.json, the twelve <fixture>.json, and none or some of the three .channels.txt
git diff --numstat -- tests/data/fusion/reject_*.json
# expected: 1 1 for each of the nine
git diff -U0 -- tests/data/fusion/reject_*.json | grep -E '^[-+][^-+]' | grep -vE '"algorithm": "batch-(shared-bias-v2|temperature-bias-v3)",'
# expected: no output
for f in coarse_linear coarse_maneuver stationary_spin; do
  git diff -U0 -- tests/data/fusion/$f.json | grep -E '^[-+][^-+]' | grep -E '"(rule|passes|iterations|prefix_length_s|prefix_fits|gnss_states|imu_outputs|rows)"|"progress"|Segment |Pass [0-9]+, iteration'
done
# expected: no output (no count, rule, count-bearing key or progress text changed)
git diff -U0 -- tests/data/fusion/coarse_linear.json | grep -E '^[-+][^-+]' | grep -vE '"algorithm"|"gyro_bias": \{|"b0_rad_s": \[|"b1_rad_s_per_degc": \[|"t_ref_degc": 25|^\+ *-?[0-9][0-9.eE+-]*,?$|^\+ *\],?$|^\+ *\},?$'
# expected: no output for coarse_linear (an exact fit near zero cost: nothing else should move); for the other two the same command lists the last-digit changes, which the report quotes
```

If a count, a rule, a progress text or a `.channels.txt` row count changed, or a success fixture prints `** UNEXPECTED **`, the change is not rounding: investigate (`fitTraceMatchesGolden` and `successFixturesMatchGolden` name the first differing value) before anything is committed, and never edit a golden. If a Phase 3 or Phase 5 kernel literal flips on the new numbers (`passes <= 2`, a `history.size()`), the remedy is in the test and is reported, as those phases said. `tst_fusion_session::naturalSessionEndToEnd` (env-gated on a real recording, which has the column) is run if the recording is available and its outcome reported.

**Acceptance Criteria:**
- [ ] The tool printed twelve lines, none `** UNEXPECTED **`, exited 0; the reconfigure logged `Fusion exact tests registered for Release`
- [ ] The nine rejection goldens changed by the algorithm string only (`numstat` `1 1`, filtered diff empty); the three success goldens carry `model.gyro_bias` with `b1_rad_s_per_degc` all zero and `t_ref_degc` 25, unchanged `progress` arrays, `stopping.passes`, `seeds[0].iterations`, `history` row counts and `initializer` objects
- [ ] `coarse_linear.json` changes by nothing but the algorithm line and the `gyro_bias` object (the filtered diff is empty); for `coarse_maneuver` and `stationary_spin` the report quotes every other changed line and states the largest relative change
- [ ] `ctest -C Release -L fusion`, `-L exact`, `-L audit` and the whole suite without `-L` pass on `build-phase1/FlySightViewer-build`
- [ ] The phase's report lists every file `git status` shows under `tests/data/fusion/`, and says whether the `.channels.txt` files changed

**Complexity:** S

---

## Testing Requirements

### Unit Tests
- `tst_fusion_kernel`: new `temperatureFactorJacobians`, `temperatureGraphShape`, `reconstructionUsesIntervalBias`, `constantTemperatureKeepsSlopeAtPrior`; extended `driftingBiasSegmentsConverge` (the `b1`, `b0`, `T_ref`, iteration-count, residual-kind and attitude assertions), `validationRejectsEachDefect` (tuning, samples and pipeline cases for the temperature, the rejection reasons and their order), `initializerFixturesAreDeterministic` (the new array); literal updates in `biasSettledByCostTest`, `failureDiagnosticsShape` (algorithm string) and `diagnosticsReportPerStepConstants` (`model` keys, `t_ref_degc`). `fitTraceMatchesGolden`, `initializerDiagnosticsShape` and every other slot unchanged in code, green after the re-capture.
- `tst_fusion_golden`: unchanged in code except `fixturesAreDeterministic` comparing the new array; `successFixturesMatchGolden`, `rejectionFixturesMatchGolden`, `progressMatchesGoldenBoundaries`, `fitTraceMatchesGolden` compare against the re-captured goldens; the cancellation rows (indexed from the golden's `progress`, Phase 5) hold because the progress sequences are unchanged; determinism and thread tests unaffected.
- `tst_fusion_session`: `registrationShape` (22 inputs), `inputsAreBitIdenticalToFixture` (22 names, strides `{3, 5, 7}`), `missingInputsAreNotApplicable` (fourth row), new `temperatureReachesTheKernel`; the rest unchanged.
- `tst_fusion_runner` (Phase 2): `successMatchesDirectRun` asserts the temperature dump line; the rest unchanged (`naturalSession` and every fixture session now store the column).
- `tst_fusion_jobs`, `tst_fusion_rows`: unchanged; they build sessions through `fixtureSession()` / `naturalSession()` (which now store the column) and compare channels through `goldenDifference()`.
- No engine test changes: `src/engine/` is untouched.

### Integration Tests
- `ctest --test-dir build-phase1/FlySightViewer-build -C Release -L fusion --output-on-failure` and `-L exact` after the re-capture; `-L audit`; the whole suite without `-L`.
- Configure-time: `GTSAM link confinement: OK` (no new target; the new source files are inside `flysight_fusion`) and `Fusion exact tests registered for Release`.
- `tst_fusion_session::temperatureReachesTheKernel` and `tst_fusion_runner::successMatchesDirectRun` are the integration proofs of the input path: a stored `IMU/temperature` column is served as degC bit for bit through the conversion layer and the engine, exported and re-imported by the runner's CSV round trip, and the engine-path diagnostics equal the direct kernel call's.

### Manual Verification
- Task 6.10's `git status`, `numstat` and filtered `diff -U0` checks.
- With the Phase 2 runner: a real recording (any of spec section 12's under `TEMP/data/`, untracked) run with `--dump-inputs`: the dump has an `IMU/temperature` line with values in degC (30-45 for a warm unit); the printed diagnostics carry `model.gyro_bias` with a `t_ref_degc` that matches the dump's mean. The spec's acceptance for the model is the reduction of `quality.imu_nrms` on the two Data comp 2 recordings (`10-15-24`, `08-35-41`) against the Phase 5 commit with no loss elsewhere, and a fitted `b1_rad_s_per_degc` on their y axis near `0.12 deg/s/degC` (`2.1e-3` rad/s/degC; `experiments/fusion_lab/NOTES.md` section 7.6). This is Michael's corpus check (spec sections 6 and 12), not an acceptance criterion of the phase.

## Notes for Implementer

### Gotchas
- **Factor order is binding and the slope prior is last.** `collectResiduals()` walks the graph by count; if the slope prior is emplaced anywhere but after the bias prior, the `slope_prior` entry reads the wrong factor and `quality` is wrong. The temperature factor's kind stays `imu`.
- **`H_B` is `H5` whole and `H_T` is `H5.rightCols<3>() * dT`.** `ConstantBias` is a vector space (`ImuBias.h` line 164); the interval bias is `[acc; gyro] + [0; slope dT]`, so the chain rule is the identity for `B(0)` and `[0_3x3; dT I_3]` for `T(0)`. Ask `computeErrorAndJacobians` for `H5` whenever `H5` or `H6` is wanted, and pass `H1..H4` through untouched.
- **Preintegrate at the interval's bias, evaluate at the interval's bias.** The `pim` handed to the factor is linearized at `intervalBias(k-1)` of the build's linearization point; at evaluation the factor computes the interval bias from the current `B(0)`, `T(0)` and the same `dT`, so `biasCorrectedDelta` sees only the increment since the last rebuild. Using `B(0)` alone for the preintegration (a constant bias per build) would make the first-order bias correction carry the whole temperature term and slow every pass.
- **The interval's temperature is its first fix's.** `dT` for the factor between fixes `k-1` and `k` is `temperatureAtFix(d, k-1) - tRef`, and the reconstruction of interval `[k, k+1)` uses fix `k`; both follow `ImuFactor`'s `bias_i` convention. Mixing the two conventions puts a one-interval lag between the fit and the dense attitude.
- **No `T(0)` under the constant model.** A variable no factor touches makes the linear system indeterminate; the constant model must not insert it. Under the temperature model the prior always exists.
- **The stock path must not do arithmetic.** `intervalBias()` under the constant model returns its argument; the four-argument `buildFactorGraph()` forwards with `GyroBiasModel{}`; `linearizationOf()` returns a zero slope without reading a key. The initializer's prefix and segment fits are on this path, and Phase 5's `initializer` diagnostics in the goldens must not move. Inside the factor, by contrast, no zero-slope branch: the arithmetic identity is what Task 6.8 asserts bitwise.
- **`T_ref` is a plain mean in index order** (a loop or `std::accumulate`), computed once in `planFit()` from `plan.window.temperature`. Pairwise or compensated summation would make `t_ref_degc` differ between platforms and would not give exactly 25 on the constant fixtures.
- **`imuChannels()` in `fusionfixtures.cpp` must include the temperature.** `reject_coverage` truncates the IMU channels to 36 samples and `reject_imu_gap` removes ten; without the temperature in that list those two fixtures would carry a 201-sample temperature against a shorter `imuTime` and be rejected as `Missing or mismatched IMU/temperature` instead of their own reason, and their goldens would change beyond the algorithm string. Task 6.10's rejection check catches it.
- **Every synthetic recording needs a temperature now**: the twelve golden fixtures, Phase 5's four initializer fixtures, `naturalSession()`, and any hand-built `Channels` in a test that expects `Succeeded`. Hand-built `Samples` on the internal seams (`boundarySamples`, `linearSamples`) may stay without one: the stock path and `validateSamples()` accept an empty series; only `gyroBiasModelFor()` and the temperature model refuse it.
- **`inputsAreBitIdenticalToFixture`'s strides** must be coprime with the new count (22): `{3, 5, 7}`; the old `{4, 5, 8}` share a factor with 22.
- **Literals from earlier phases** that this phase flips: the algorithm string in `biasSettledByCostTest` and `failureDiagnosticsShape`, and `model.keys()` in `diagnosticsReportPerStepConstants`. Nothing else pins the old string; the goldens are re-captured. A Phase 3/5 iteration literal that flips on the new numbers is fixed in the test and reported.
- **The constant-temperature `b1` bound is 1 % of the prior sigma**, not zero: with `dT` exactly zero at every fix the slope's column in every IMU factor is exactly zero, its prior residual is zero at zero, and LM leaves it at `0.0`; the bound is the margin against a solver that visits a rounding-size value and steps back.
- **The runner's CSV writer is `DataExporter`**: it writes the IMU columns from its table (`time, wx, wy, wz, ax, ay, az, temperature`) and the unit line from the session's source units, so a session that stores `IMU/temperature` with unit `deg C` round-trips it; the writer needs no code, the test needs the assertion.
- **Read-only git** for the implementer (`status`, `diff`, `rev-parse`); the orchestrator stages by path, including the two new source files and every re-captured golden.
- **Line endings** as in Phases 1-5: never open a golden in an editor between the capture and the checks.

### Decisions Made
- **`IMU/temperature` is a required input** (the coordinator's decision, from the spec's author): it is a column of every FlySight 2 `SENSOR.CSV` and every corpus recording; a file without it is hand-made and is blocked by the engine like any file missing a declared input (`missingInputsAreNotApplicable`'s new row) and rejected by the kernel by name. No engine change; `src/engine/` and `docs/CALCULATIONS.md` are untouched. Spec section 10's "a recording without a temperature channel fits with `b1` fixed at zero and says so" is read, under this decision, as "is rejected / blocked", and the kernel and session tests assert that.
- **`b1_fixed_zero` is dropped from `model.gyro_bias`** and `b1_rad_s_per_degc` / `t_ref_degc` are always numbers: the full fit always runs the temperature model, so the flag would always be false and the nulls unreachable. This is a deliberate reduction of the overview's binding key layout, recorded here for Phase 7's documentation and acceptance map. No audit key is added.
- **Rejection reasons and order**: `Missing or mismatched IMU/temperature` and `Nonfinite IMU/temperature`, produced by the existing `requireChannel()` helper, checked last in `requireAllChannels()` so every other channel's defect is reported first (the nine rejection goldens keep their reasons).
- **Key symbol `T(0)`** through `gtsam::symbol_shorthand::T` (`Symbol('t', 0)`), which exists in GTSAM 4.3; no hand-built key.
- **Factor class `TemperatureImuFactor` in `src/fusion/temperatureimufactor.{h,cpp}`**, a `NoiseModelFactorN<Pose3, Vector3, Pose3, Vector3, imuBias::ConstantBias, Vector3>` holding the interval's `pim` and `dT`, noise model `Gaussian::Covariance(pim.preintMeasCov())`, `clone()` implemented, no `print`/`equals`. Its own files rather than the anonymous namespace of `factorgraphfit.cpp` because the kernel test must instantiate it for the Jacobian check.
- **`T_ref` is computed in `planFit()`** through `gyroBiasModelFor(window)` (next to the fit, in `factorgraphfit.h`), as the plain index-order mean of `window.temperature`, which is exactly the IMU samples of the fitted window's IMU range (`fittedWindow()` slices the temperature with `imuTime`). It travels in `GyroBiasModel`, is copied into `FitResult::biasModel`, and is reported from there. Rejected: computing it inside `fitFactorGraph()` (a prefix sub-window would get its own `T_ref`, and the routine would have to know whether it is the full fit).
- **Temperature storage is `std::vector<double>`** on `Samples` (degC per IMU sample), with a scalar `interpolateAt()` overload and `temperatureAtFix()` in `imuintegration.h`. Internal `Samples` may leave it empty (hand-built test samples, the stock path); the public boundary always fills it. Rejected: storing it as `Vectors` `(T, 0, 0)` to reuse the vector overload (three times the memory and a lie in the type).
- **Residual kind of the `b1` prior: `slope_prior`**, node 0, time of the first fix, appended after `bias_prior`.
- **The iteration-count assertion is the sum over passes**: `seeds[0].iterations` (`= trace.history.size()`, Phase 3's definition), at most 30. The spec's "converges within 30 iterations" is read as the whole full fit; `Stopping::passes` counts passes separately and is not bounded by the test.
- **The trace does not carry `b1`**: `fusiontrace.h` is unchanged; the diagnostics' `model.gyro_bias` is the record, and Phase 5's trace shape stays so `fitTraceMatchesGolden`'s golden `trace` objects change only where the numbers move.
- **`fitFactorGraph()` gains a trailing `GyroBiasModel` parameter defaulting to the constant model**; only `fitAndAssemble()` passes the temperature model, so the initializer's prefix and segment fits are stock by default and `initializer.cpp` is untouched (the constant model stays as an internal path, as the coordinator confirmed). Rejected: stripping the temperature from sub-windows (a hidden convention on data shape) and a flag inside `Tuning` (the model is a property of the window, not a tuning value).
- **Two `buildFactorGraph()` overloads**: the four-argument stock form keeps its signature (Phase 5's tests and the yaw-sigma helper's callers) and forwards to the six-argument form with a `BiasLinearization` and the model.
- **`FitResult` gains `gyroBiasSlope` and `biasModel`**; `seeds[0].gyro_bias_rad_s` keeps its name and is `b0` (the bias at `T_ref`); `model.gyro_bias.b0_rad_s` is the same array.
- **`Tuning::gyroBiasSlopeSigma = .010 * kPi / 180`** with `kPi` moved above `Tuning`, so the spec's number is visible in the default; strictly positive.
- **Every fixture that is not about the temperature carries a constant `kFixtureTemperatureDegC = 25`** (exactly representable, noise-free, so the bit-reproducibility rules hold and `T_ref` is exactly 25); `drifting_bias` carries the ramp `30 + t / 20` degC (30 to 40 over 200 s): `b1 = (0, 0, 0.1 deg/s/degC)` exactly by construction, `T_ref = 35`, `b0z = 0.8 deg/s`; ten degrees of excursion give the slope ten times the prior sigma's worth of signal over a 1 deg/s drift, so the data dominate the prior (the prior's shrinkage is about 4 %) and the 20 % bound has margin. The constant-temperature variant is made in the test.
- **No `reject_temperature` golden fixture**: it would change the "twelve fixtures, sixteen files" checks of Phases 1 and 5 (`fusion_golden_capture`'s line count, `fusionFixtures()`'s size, the README's fixture table); the missing and malformed temperature is asserted in `validationRejectsEachDefect` on mutated channels of `coarse_maneuver`, without a golden, and on the engine side in `missingInputsAreNotApplicable`.
- **No `isExactKey()` change**: `t_ref_degc` and `b1` are solver-side arithmetic under the portable bound (a constant series makes `t_ref_degc` exact anyway); no new count or copied tuning value is written.
- **The constant-temperature test compares its objective with a stock fit through the internal seams** (`initialize()` then `fitFactorGraph()` with the default model) within `1e-6` relative, as a consistency check of the custom factor at zero slope through a whole fit; the bitwise equivalence with `ImuFactor` is asserted at the factor level, where it is exact.
- **The re-capture's expectation is the weaker one** (numbers may move in the last digits; counts, texts, outcomes and the initializer's account may not), because the extra variable and factor can change the elimination ordering and the frontal structure of the multifrontal QR; the exact-zero `dT` argument covers the factors and `T(0)` itself, not the linear solves. Observed byte identity is reported, not promised.
- **Task 6.5 keeps the accelerometer bias constant** in the reconstruction and the factor (spec section 6); only the gyro part is temperature-dependent.
- **`docs/SENSOR_FUSION.md` gets facts only** (input list, one paragraph, the model sentence, the key list); Phase 7 writes the explanation and the acceptance map entries for spec section 10's temperature line (`validationRejectsEachDefect` and `missingInputsAreNotApplicable` for the no-channel case, `constantTemperatureKeepsSlopeAtPrior`, `driftingBiasSegmentsConverge`).

### Open Questions
- None that block implementation. Two notes for the orchestrator: (1) if `driftingBiasSegmentsConverge`'s full fit exceeds 30 iterations or misses `b1` by more than 20 %, the Jacobian test is the first thing to look at (a wrong `H6` sign or a missing `dT` factor converges slowly rather than not at all); if the Jacobians are right, report the fitted `b1`, the iteration count and `stopping` before anything is changed, and the remedy is discussed, never a loosened bound; (2) if the re-capture moves a count, a rule or a progress text on a golden fixture (Task 6.10's checks), the extra variable changed more than rounding on that fixture: the implementer reports the first differing value and the orchestrator decides before the goldens are committed.

## Definition of Done

This phase is complete when:
1. All tasks have passing acceptance criteria, in particular Task 6.8's Jacobian and section 6 assertions and Task 6.10's checks (rejections changed by the algorithm string only; counts, texts, outcomes and `initializer` objects unchanged; `coarse_linear` changed by the algorithm line and the `gyro_bias` object only; every other change quoted)
2. All tests pass: `ctest -C Release` on `build-phase1/FlySightViewer-build` without `-L`, and with `-L fusion`, `-L exact`, `-L audit`, after the reconfigure that reads the new `capture.json`
3. Code follows the patterns of `factorgraphfit.cpp` (small named `add*` helpers, one anonymous namespace, the binding factor order, one rebuild per pass), `imuintegration.cpp` (file-local helpers, unchanged public arithmetic), `fusionoutput.cpp` (one writer per object), `fusion.cpp` (decisions in `planFit()`, catching only in `runPipeline()`), `fusionregistration.cpp` (one table serves declaration and hand-over), `fusionfixtures.cpp` (index-based samples, no noise on the temperature) and `tst_fusion_kernel.cpp` (hand-built `Samples`, literal expectations with their derivation in comments)
4. Nothing outside `src/fusion/` includes a GTSAM header that did not before (`tests/tst_fusion_kernel.cpp` includes the new kernel header, which the audit allows); `src/engine/` is untouched; the kernel's purity, threading and cancellation rules and the public result contract are unchanged
5. No TODOs or placeholder code remains; the phase's report lists every file created, modified and re-captured (`src/fusion/fusion.h`, `inputadapter.cpp`, `fusionsamples.h`, `fusionsamples.cpp`, `imuintegration.h`, `imuintegration.cpp`, `factorgraphfit.h`, `factorgraphfit.cpp`, `fusion.cpp`, `trajectoryreconstruction.h`, `trajectoryreconstruction.cpp`, `fusionoutput.cpp`, `fusionregistration.cpp`; created `src/fusion/temperatureimufactor.h`, `temperatureimufactor.cpp`; `src/CMakeLists.txt`; `docs/SENSOR_FUSION.md`; `tests/fusion/fusionfixtures.h`, `fusionfixtures.cpp`, `fusiongolden.cpp`, `fusionsessions.h`, `fusionsessions.cpp`; `tests/tst_fusion_kernel.cpp`, `tst_fusion_session.cpp`, `tst_fusion_runner.cpp`; `tests/README.md`; and every file `git status` shows under `tests/data/fusion/`)
