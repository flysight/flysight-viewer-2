# Phase 2: The noise model by configuration

## Purpose

The specification's section 6 up to the scale state, and the checks of
section 5 that need the kernel: the fit's noise stops being tuning constants
and becomes the datasheet's, by configuration, plus two derived per-step
terms with no constant of their own. Concretely:

- the datasheet unit `src/fusion/sensornoise.h` / `.cpp` (decisions 6, 9, 10);
- the per-sample noise and the integration density derived from it;
- the sampling term and the rotation remainder in place of the slopes,
  computed in `preintegrateImu()`'s loop;
- the lattice check, the rate check and the table-entry check as kernel
  rules, after every existing check (decisions 7, 8, 9);
- `configuration` and `model.noise` in the diagnostics;
- fixtures that state their configuration and lie on its lattice, two new
  rejection fixtures, the algorithm string `batch-temperature-bias-v6`, the
  goldens captured again (decisions 11, 12, 13);
- the documents of all of it, and the audit group `noise-model`.

It is one phase because the densities, the step terms, the checks and the
fixtures all move the same goldens: they are captured once, here.

Clauses owned: 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 40, 41, 43, 45, 46, 47,
51, 52, 53, 62 (items 1008-1017, 1040, 1041, 1043, 1045-1047, 1051-1053,
1062). Clauses 14 and 15 are restated "(as settled)" (Decisions 2 and 3).
Earlier items restated "(as amended)": 215, 216, 217, 227, 245, 247, 847,
861, 921, 925 (Tests, Traceability).

## Dependencies

- **Depends on phase 1.** At the start: `Fusion::ImuConfiguration` and
  `Channels::imuConfiguration` exist and are filled by `channelsFrom()`
  (NaN when an attribute is not a number); the kernel reads nothing of them;
  `fitInputs()` has twenty-six inputs; the four `builtin.default.*`
  calculations exist; `src/sensorconfiguration.h` holds the key constants
  (`SensorConfiguration::AccelFsG` ...); `Fusion::Algorithm` is
  `batch-temperature-bias-v5`; the goldens are the capture of 2026-09-30;
  `tests/fusion/fusionfixtures.*` and `toChannels()` are unchanged; the audit
  has an explicit completeness list `1001 ... 1061`.
- **Blocks phase 3**, which reads `SensorNoise::sensitivityTolerance` and
  extends `preintegrateImu()`, and **phase 4**, which reads
  `ImuNoise::accelerometer.sampleSigma`.

## What changes

### The datasheet unit: `src/fusion/sensornoise.h` / `.cpp` (new)

The only place the datasheet exists in code (clause 46). Qt Core and std only,
no GTSAM, no Eigen; it includes `fusion/fusion.h` for `ImuConfiguration` and
`sensorconfiguration.h` for the key names of its rejection text. Names under
Interfaces. The table, DS12140 Rev 3, every figure read from the page images:

| Quantity | Value | Source |
| --- | --- | --- |
| accelerometer noise density, high-performance | 70, 75, 80, 110 ug/sqrt(Hz) at +/-2, 4, 8, 16 g | Table 2, An (note 8: independent of ODR) |
| gyro noise density, high-performance | 3.8 mdps/sqrt(Hz) | Table 2, Rn (note 6: independent of ODR and FS) |
| accelerometer sensitivity (step, lattice) | FS / 32768 g: 1/16384, 1/8192, 1/4096, 1/2048 g | Table 2, LA_So prints 0.061, 0.122, 0.244, 0.488 mg/LSB: these values to three decimals |
| gyro sensitivity (step, lattice) | 8.75, 17.50, 35, 70 mdps at +/-250, 500, 1000, 2000 | Table 2, G_So |
| accelerometer bandwidth | ODR / 2 | Figure 17 note 1 (LPF1 at ODR/2 in high-performance mode) and Table 65 (LPF2_XL_EN = 0) |
| gyro bandwidth (LPF2 cutoff, no LPF1) | 4.2, 8.3, 16.6, 33.0, 66.8, 135.9, 295.5, 1108.1, 1320.7, 1441.8 Hz at 12.5 ... 6666 Hz | Table 18 (it prints 417, 1667, 3333, 6667; Table 2 prints 416, 1666, 3332, 6664; the keys write 416, 1666, 3333, 6666) |
| sensitivity tolerance | 0.01 for both sensors | Table 2, G_So% +/-1 % (Decision 5) |

- The accelerometer step is `FS / 32768` g exactly, not the printed 0.488 mg:
  the firmware writes `counts x FS / 32768`, and the printed figure would put
  a 1 g reading 0.58 mg off the lattice. The gyro step is the sensitivity,
  1.14688 times `FS / 32768` (decision 6).
- Note 10 of Table 2 ("BW = ODR/2") belongs to the low-power RMS rows, not to
  An; the overview's citation is corrected to Figure 17 note 1 and Table 65.
- The table assumes high-performance mode (XL_HM_MODE and G_HM_MODE at their
  reset value 0, which firmware v2023.09.22 does not change) and the fixed
  filter of decision 10. `docs/SENSOR_FUSION.md` says so.
- Values per sensor, in kernel units (m/s^2 with 9.80665; rad/s with pi/180):
  `sampleSigma = sqrt(datasheetDensity^2 x bandwidth + step^2 / 12)`
  (clause 11) and `density = sampleSigma x sqrt(1 / rate)`, the rate the key's
  value (clause 13). The bandwidth stands in for the filter's noise-equivalent
  bandwidth (the datasheet gives cutoffs, not filter orders); the documents
  say so.
- **No entry**: a member that is NaN or not in its list is rejected naming the
  first such key in key order (`ACCEL_FS_G`, `GYRO_FS_DEG_S`, `ACCEL_ODR_HZ`,
  `GYRO_ODR_HZ`). The accelerometer's 1.6 Hz has no entry (Decision 4).

For the three configurations the fixtures state (+/-16 g, +/-2000 deg/s):

| Rate | acc sigma, m/s^2 | acc density | gyro sigma, rad/s (deg/s) | gyro density | old / new density, acc and gyro |
| --- | --- | --- | --- | --- | --- |
| 12.5 Hz | 0.0030304 | 8.5714e-4 | 3.7797e-4 (0.02166) | 1.0691e-4 | 17.5, 9.4 |
| 26 Hz | 0.0041278 | 8.0952e-4 | 4.0112e-4 (0.02298) | 7.8665e-5 | 18.5, 12.7 |
| 104 Hz | 0.0079007 | 7.7473e-4 | 5.1917e-4 (0.02975) | 5.0909e-5 | 19.4, 19.6 |

The 12.5 Hz accelerometer sigma is the corpus's quietest-window floor,
0.0029-0.0035 m/s^2 (`experiments/accuracy_probe/noise_floor.txt`). The gyro
model is below the corpus median, 0.030 deg/s, which phase 5 reports as
validation, not tuning.

### The checks (decisions 7, 8, 9)

In `planFit` (`fusion.cpp`), after `requireNoGnssOutage(...)` and in this
order: the table entry (`imuNoise()`), the lattice (accelerometer, then gyro),
the rate (`ACCEL_ODR_HZ`, then `GYRO_ODR_HZ`); then `plan.tuning.noise` is set.
Every existing rejection keeps its reason (decision 13). The lattice and rate
rules live in `fusionsamples.cpp` beside the other rules; the steps come from
the unit.

- **Lattice** (clauses 8, 9). On every reading of `Channels` (`ax`..`az` in
  m/s^2, `wx`..`wz` in deg/s), before the adapter's conversion: no correction
  of the kernel's. A reading fits a range's lattice when its distance to the
  nearest multiple of that range's lattice step is at most the tolerance. The
  ranges are tried coarsest first (16, 8, 4, 2 g; 2000, 1000, 500, 250 deg/s),
  all three axes together; the first that every reading fits is the range
  shown, and it must equal the configured one.
- **Tolerance**: one unit of the file's last decimal times the conversion
  factor, plus nothing: `1e-5 x 9.80665 = 9.80665e-5` m/s^2 and
  `1e-3 x 1.14688 = 1.14688e-3` deg/s. One unit, not a half: v2023.09.22
  truncates (C integer division). `TEMP/17-26-24/SENSOR.CSV` (24,511 samples)
  shows the largest residuals 9.6534e-5 m/s^2 and 1.1424e-3 deg/s: 63/64 and
  255/256 of a unit, the largest fractions its factors
  (48.828125 x 10 ug, 61.03515625 mdps per count) allow. Against the
  printed 0.488 mg lattice the same file is off by 2.39e-3 m/s^2. The margin
  to half the finest step (2.99e-4 m/s^2, 4.375e-3 deg/s) is at least 3x.
- **Rate** (clause 10). `medianInterval(full.imuTime)` (already computed for
  `maxGap`) against `1 / rate` of each key; a relative difference
  `|median - 1/rate| / (1/rate)` above 0.10 rejects.
- Consequence to document (section 6 and `DATA_SCHEMA.md` section 7): a
  legacy file given `$VAR,SCHEMA_VER,2` by hand is read without the 1.14688
  correction, its gyro readings lie on no range's lattice, and the fit
  rejects it. `tests/README.md` section 11 "Real recordings" cites that file
  to reproduce the branch's objective; that sentence is rewritten.

### The step model: `imuintegration.h` / `.cpp` (clauses 14, 15, 47)

`preintegrateImu()` takes `const ImuNoise &noise` in place of
`const Tuning &tuning` (it reads nothing else of the tuning);
`preintegrationParams()` likewise. `stepSigma` and the slopes go;
`stepCovariance` becomes the formula below. Everything else of the loop
(edges, midpoint readings, the half-step turn, the observer contract, one
`.integrateMeasurement(` call) is unchanged. The function throws
`std::invalid_argument("Invalid fusion configuration")` when a density is not
finite and positive (unreachable through `planFit`; it guards a test that
forgets the noise).

Notation for one step `[a, b]`, `dt = b - a`, inside the sample interval
`[t_k, t_k+1]`, `h = t_k+1 - t_k` (every step lies in one interval: the edges
include every IMU time). `f`, `omega`: the piecewise-linear readings of the
`Samples` handed in, as `interpolateAt()` gives them.

**Sampling term.** The midpoint reading of a linear segment integrates it
exactly, so the step's whole sampling error is that of the interpolant `L`.
For a signal with constant second derivative across the interval,
`f - L = (f''/2)(t - t_k)(t - t_k+1)`, so

```
error over the step = -f'' w,   w = 1/2 * integral_a^b (t - t_k)(t_k+1 - t) dt
                                  = 1/2 [ h (u_b^2 - u_a^2) / 2 - (u_b^3 - u_a^3) / 3 ],  u = t - t_k
```

For a step that is a whole interval `w = h^3 / 12`, the trapezoid rule's
error of the specification; the pieces of a split interval sum to it.
`f''` is estimated per sample interval from the change of slope at its two
ends: at sample `i` (exists for `1 <= i <= m - 2`, `m` samples),
`D_i = 2 (s_i - s_i-1) / (h_i-1 + h_i)`, `s_i = (x_i+1 - x_i) / h_i`, exact for
a quadratic and for uneven spacing. Interval `k` takes
`c_k = max(|D_k|, |D_k+1|)` over those that exist (vector norms), 0 when
neither does. Then `s_v = w c_k(f)` (m/s) and `s_theta = w c_k(omega)`
(rad); the gyro bias, constant within an interval, cancels.

**Rotation remainder.** With `theta = (omega_mid - b_g) dt` (twice the
argument of the existing `halfStep`), `dtheta = (omega(b) - omega(a)) dt`,
`fbar = f_mid - b_a`, `df = f(b) - f(a)`: for rate and force linear across the
step, the true velocity change `R_a integral_0^dt Exp(phi(s)) f(s) ds` minus
the scheme's `R_a Exp(theta/2) fbar dt`, expanded to second order, is

```
r_v     = | (dt/24) theta x (theta x fbar) + (dt/12) (theta x df - dtheta x fbar) |    m/s
r_theta = | theta x dtheta | / 12                                                     rad
```

The first term is the pure rotation remainder (second order in `theta`);
the bracket and `r_theta` (coning: `Exp(theta)` for a rate that turns within
the step) are the same expansion's other second-order terms (Decision 3).
Position remainders are fourth order in `dt` and stay in the integration
covariance.

**The step covariance.** GTSAM adds `B (Sigma / dt) B^T` per step with
`B ~ R dt` for velocity (rotation likewise), so a step's velocity variance
is `Sigma dt`. Hence, in quadrature:

```
accelerometerCovariance = (D_a^2 + (s_v^2 + r_v^2) / dt) I
gyroscopeCovariance     = (D_g^2 + (s_theta^2 + r_theta^2) / dt) I
```

A step of nominal length carries one sample's variance `(sigma T)^2`; a step
with a constant signal and no rotation has `D^2 I` exactly (clause 53);
the added variance vanishes as `dt^3` or faster for a sliver step
beside a fix, so a fix next to a sample costs nothing. Isotropic, from vector norms,
as the term it replaces: an isotropic covariance is unchanged by the
half-step turn.

**What it assumes** (clause 16): a smooth signal between samples (a step
change of the signal at a sample is underestimated; on `motion_start`'s
2 m/s^2 step the true error is 6x the term), the samples exact (on noisy
readings the second differences read noise as curvature: for noise at the
model's level, about 4-8 % of a full step's noise variance), and in the
remainder, linear rate and force across the step.

### The tuning, the fit and the diagnostics

- `Tuning` (`fusionsamples.h`): remove `accDensity`, `gyroDensity`,
  `accStepSlope`, `gyroStepSlope` and their comments; add `ImuNoise noise`
  ("derived from the recording's configuration by `planFit`, as `maxGap`
  is; NaN until then"). `requireValidTuning` drops the four fields and does
  not check the noise (the check order above). Bias priors stay
  (`accBiasSigma`, `gyroBiasSigma`, `gyroBiasSlopeSigma`; clause 17).
- Callers pass `tuning.noise`: `addImuFactor`, `addTemperatureImuFactor`,
  `reconstructInterval`. Nothing else in `factorgraphfit`, `initializer` or
  `trajectoryreconstruction` changes; the initializer's prefix tuning copies
  the noise.
- `fusionoutput.cpp`: `modelSummary` writes `noise` in place of `per_step`;
  `successDiagnostics` adds the top-level `configuration`. Failure diagnostics
  are unchanged but for the algorithm string. Update the comments of
  `fusion.h` (`Result::diagnosticsJson`) and `fusionoutput.h`.
- `fusion.h`: `Algorithm` becomes `"batch-temperature-bias-v6"` (decision 11).
- **The optimizer's damping ceiling** (decision 7, Michael's): `Tuning` gains
  `lambdaUpperBound = 1e12`, the ceiling of Levenberg-Marquardt's damping,
  with the comment that GTSAM's default of 1e5 is below the damping a
  resting recording needs under the datasheet densities (the IMU blocks of
  the Hessian are about 1e9), and that only a test changes it.
  `runOptimizerPass` sets `params.lambdaUpperBound` from it. `thresholdsOf`
  copies it into the stopping account, so the account stays complete on its
  own, and the diagnostics' `stopping` object reports it beside the other
  thresholds.
- **A new stopping rule, `damping saturated`** (decision 7): a constant
  beside `kCostIncreased` in `factorgraphfit.h`, a value of `StopRule`. In
  `runOptimizerPass`, an iteration that leaves the cost unchanged
  (`before == after`) while the optimizer's damping is at the ceiling
  (`optimizer.lambda() >= c.lambdaUpperBound`) throws `FitFailure` with that
  rule, exactly as a non-finite or increasing cost does: the fit cannot
  continue from it, and it is a solver failure, never a convergence. The
  settled test is unchanged where the damping is below the ceiling: a step
  that did not move there still means LM found no better point. The
  function's comment states both. `failureDiagnostics` reports the rule's
  text as it reports `cost increased`; nothing else in the failure shape
  changes.

### Fixtures and sessions (decision 13, clause 45)

- `FusionFixture` gains the stated configuration (Interfaces), quiet NaN by
  default; `toChannels()` copies it into `Channels::imuConfiguration`.
- Every golden and initializer fixture states +/-16 g, +/-2000 deg/s and a
  rate: 104 Hz for `coarse_linear`, `coarse_maneuver` and the rejections built
  from them; 26 Hz for `stationary_spin`, `motion_start`, `rest_throughout`;
  12.5 Hz for `sacc_anchor` and `drifting_bias`.
- Readings are rounded onto the stated lattice after the noise is added
  (`round(v / s) * s`, `s = FS / 32768 x 9.80665` m/s^2 or 0.070 deg/s). The
  draw order is unchanged, so every GNSS value is unchanged. `std::round` is
  exact and joins `+ - * /` in the generator's rules (`fusionfixtures.cpp`
  head and `tests/README.md` section 11). `coarse_linear`'s readings (0 and
  `-9.80665 = -2048` counts) are already on it.
- Time axes are unchanged except two **(for Michael)**: `sacc_anchor` and
  `drifting_bias` log at 10 Hz, 25 % from 12.5 Hz, the nearest rate, so no
  rate passes the check. Both are resampled at 12.5 Hz, `t = i / 12.5` (exact
  at the integer manoeuvre bounds), `i = 0..3750` and `0..2500`;
  `drifting_bias` blocks become `k = i / 375`. They are not goldens.
- `rest_throughout` keeps its IMU noise, at the datasheet level: its force
  and gyro amplitudes become the per-sample sigmas of its stated
  configuration (`imuNoise()`'s `sampleSigma` for 16 g / 2000 deg/s / 26 Hz),
  and its readings are rounded onto the lattice like every fixture's
  (decision 6, Michael's). Its initializer test is re-measured under that
  noise: if the prefix growth of item 239 no longer stops at 120 s, the item
  is restated "(as amended)" with the measured length and the reason, and
  the test's literal follows; the noise is not removed to keep the literal.
- Two rejections, after `reject_origin`: `reject_lattice` (the
  `coarse_maneuver` generator with the accelerometer rounded onto the
  +/-8 g lattice, +/-16 g stated) and `reject_rate` (`coarse_linear` stating
  12.5 Hz for both rates). Fourteen golden fixtures.
- `fusionsessions.cpp`: `sessionFromFixture` stores the four values as
  attributes, the text `QString::number(value)` ("16", "2000", "104", "26",
  "12.5", the vocabulary's spellings), keys from `SensorConfiguration`; a NaN
  member is not stored. `inputsMatchFixture` also checks them.
  `naturalSession()` stores none and is the default path (clause 45): its IMU
  moves to 12.5 Hz, `100 + i * .08`, `i = 0..500` (still covering the 40 s of
  GNSS); its readings (0, -9.80665) are on the +/-16 g lattice.
- Comments that count ("twenty-two inputs", "twelve") are updated.

### What must not change

The observer contract and its bits; one `integrateMeasurement`; the
reconstruction's formulas; the initializer's constants; the stopping rules
other than the one decision 7 adds, and the optimizer's settings other than
the ceiling it raises; the bias priors; the seventeen
channels; `fitInputs()`; the record format; anything above the
registration. GTSAM stays out of `sensornoise.*` and the public files.

## Interfaces

### Provided

- `src/fusion/sensornoise.h`, namespace `FlySight::Fusion::Detail`:

  ```cpp
  struct SensorNoise {        // one sensor at its configured range and rate; every member NaN by default
      double range, rate;                 // g or deg/s, Hz: the configuration
      double datasheetDensity;            // An or Rn, kernel units: m/s^2/sqrt(Hz), rad/s/sqrt(Hz)
      double bandwidth;                   // Hz
      double step;                        // quantization step, kernel units (m/s^2, rad/s)
      double latticeStep;                 // the same step in Channels units (m/s^2, deg/s)
      double sampleSigma;                 // sqrt(datasheetDensity^2 bandwidth + step^2/12), kernel units
      double density;                     // sampleSigma sqrt(1/rate): the integration's density
      double sensitivityTolerance;        // fraction: 0.01
  };
  struct ImuNoise { ImuConfiguration configuration; SensorNoise accelerometer, gyroscope; };
  ImuNoise imuNoise(const ImuConfiguration &configuration);   // throws std::invalid_argument (text below)
  enum class ImuSensor { Accelerometer, Gyroscope };
  // The coarsest range whose lattice every value of the three axes fits (Channels units); NaN when none.
  double rangeShownByReadings(ImuSensor sensor, const QVector<double> &x,
                              const QVector<double> &y, const QVector<double> &z);
  ```

  Phase 3 reads `sensitivityTolerance`; phase 4 reads
  `accelerometer.sampleSigma`; nothing else outside the kernel's checks and
  the integration.
- `Tuning::noise` (`ImuNoise`); the four fields gone.
- `preintegrateImu(const Samples &, double start, double end,
  const gtsam::imuBias::ConstantBias &bias, const ImuNoise &noise,
  const ImuStepObserver &observer = ImuStepObserver())`;
  `preintegrationParams(const ImuNoise &)`. Phases 3 and 4 extend this
  function. Its header adds: at the observer's call for step `j`, the shared
  params hold step `j - 1`'s sensor covariances, and after the return the
  last step's (the tests' seam).
- In `fusionsamples.h`: `requireReadingsOnLattice(const Channels &)` and
  `requireStatedRates(double medianImuInterval, const ImuConfiguration &)`.
- Rejection reasons (values by `QString::number(v)`, NaN as `nan`; the logged
  rate by `QString::number(1 / median, 'f', 1)`):
  - `No datasheet entry for <KEY> = <value>; sensor fusion unavailable`
  - `ACCEL_FS_G states +/-<stated> g but the accelerometer readings lie on the +/-<shown> g lattice; sensor fusion unavailable`
    (gyro: `GYRO_FS_DEG_S states +/-<stated> deg/s but the gyro readings lie on the +/-<shown> deg/s lattice; ...`);
    none fits: `... but the <accelerometer|gyro> readings lie on no range's lattice; sensor fusion unavailable`
  - `<ACCEL_ODR_HZ|GYRO_ODR_HZ> states <rate> Hz but the IMU is logged at <logged> Hz; sensor fusion unavailable`
  - goldens: `reject_lattice`: `ACCEL_FS_G states +/-16 g but the accelerometer readings lie on the +/-8 g lattice; sensor fusion unavailable`;
    `reject_rate`: `ACCEL_ODR_HZ states 12.5 Hz but the IMU is logged at 100.0 Hz; sensor fusion unavailable`.
- Diagnostics: `configuration`: `accel_fs_g`, `gyro_fs_deg_s`,
  `accel_odr_hz`, `gyro_odr_hz`. `model.noise`: `acc` with
  `datasheet_density_m_s2_rthz`, `bandwidth_hz`, `step_m_s2`,
  `sample_sigma_m_s2`, `density_m_s2_rthz`; `gyro` with
  `datasheet_density_rad_s_rthz`, `bandwidth_hz`, `step_rad_s`,
  `sample_sigma_rad_s`, `density_rad_s_rthz`. `model` keys: `gyro_bias`,
  `noise`.
- `FlySightTest::FusionFixture`: `double accelFsG, gyroFsDegS, accelOdrHz,
  gyroOdrHz` (g, deg/s, Hz, Hz; quiet NaN unless set), copied by
  `toChannels()`, stored by `sessionFromFixture()`.
- `Fusion::Algorithm == "batch-temperature-bias-v6"`; fourteen golden
  fixtures; audit group `noise-model`.

### Consumed

Phase 1's names, unchanged: `ImuConfiguration`, `Channels::imuConfiguration`,
`SensorConfiguration::AccelFsG`, `GyroFsDegS`, `AccelOdrHz`, `GyroOdrHz`.

## Acceptance criteria

1. `sensornoise.*` holds the table above with its figures and nothing else
   in `src/` spells them; it includes no GTSAM or Eigen (clauses 11, 12, 46).
2. For each fixture configuration, `sampleSigma` and `density` equal, bit for
   bit, the formula written in the test with the datasheet literals; every
   listed value of every key has an entry; `ACCEL_ODR_HZ` 1.6, 3, NaN and the
   other keys' non-entries are rejected with the text naming the key
   (clauses 11, 13, 52).
3. `Tuning` has none of the four fields; `git grep` finds `accDensity`,
   `gyroDensity`, `StepSlope`, `stepSigma`, `per_step` nowhere in `src`,
   `tests` (README and map excepted) or `docs` (clause 13; audit).
4. On a constant signal with zero rate every step's two sensor covariances
   are `D^2 I` bit for bit; on a quadratic signal the written covariance is
   `D^2 + (w 2c)^2 / dt` for whole and part steps (rel. 1e-12), and the summed
   `w |f''|` equals the preintegrated velocity's departure from the exact
   integral (rel. 1e-9) (clauses 14, 47, 53).
5. On a constant turn (rate about z, force along x) the covariance is
   `D_a^2 + r_v^2 / dt` with `r_v = dt theta^2 |f| / 24` (rel. 1e-12), and the
   library's one-step velocity differs from the closed-form integral by
   `r_v` within 1 % at `theta <= 0.05`; on a linear ramp of rate and force the
   difference from a 1000-times subdivided preintegration of the same signal
   equals the vector `r_v` within 5 % (clauses 15, 53).
6. The checks run after every existing one, in the stated order, with the
   stated texts; 10 % passes at 9.9 % and rejects at 10.1 %; a reading one
   tolerance off the lattice fits and one 1.5 tolerances off does not
   (clauses 8, 9, 10).
7. `rangeShownByReadings` gives the stated range for every golden and
   initializer fixture and `naturalSession`'s readings (clause 51).
8. `reject_lattice` and `reject_rate` are rejected with the golden texts;
   every other rejection keeps its reason; the three successes and the four
   initializer fixtures succeed, and their existing tests hold with the
   literals this document changes (clauses 45, 51).
9. Success diagnostics carry `configuration` and `model.noise` with exactly
   the keys above, equal to `imuNoise()` of the fixture's configuration
   (clause 40).
10. `Fusion::Algorithm` is `batch-temperature-bias-v6`, spelled once in `src`;
    a record stamped `batch-temperature-bias-v5` is dropped at load
    (clauses 41, 43, 62).
11. Goldens captured again by the tool: fourteen lines, no
    `** UNEXPECTED **`, `wrote 18 files`; the `_time` column of each success
    fixture byte-identical to the previous capture; history paragraph written.
12. The suite is green in `build-agent/` (Release, sequentially), exact tests
    included; `audit_cleanup` green with the list extended.
13. `rest_throughout`, with its IMU noise at the datasheet level and rounded
    onto the lattice, converges under the default ceiling (`settled`, not
    `damping saturated`), and its initializer test holds, with item 239
    restated if its measured growth differs (decision 6).
14. With `lambdaUpperBound` forced to 1e5 by a test tuning, `rest_throughout`'s
    fit ends `SolverFailed` with `stopping.rule` `damping saturated` and the
    failure diagnostics' shape; with the default ceiling it converges; and
    the three success fixtures and `coarse_maneuver`'s prefix fits are bit
    identical under 1e5 and 1e12, because none of them reaches either
    (decision 7). `stopping` in every diagnostics carries
    `lambda_upper_bound`.

## Tests

### `tst_fusion_kernel`

- Remove `perStepTermIsZeroWithoutSignalChange`,
  `perStepTermMatchesSpecifiedCovariance`, `perStepTermScalesWithStep`,
  `withSlopes`, `densityFor`. Add a helper `fixtureNoise(rate)`
  (`imuNoise({16, 2000, rate, rate})`); hand-built `Samples` at 8 Hz use the
  12.5 Hz noise (only the densities are read).
- Add: `noiseFollowsTheTable` (criterion 2, positive half),
  `configurationWithoutEntryIsRejected` (criterion 2, negative half),
  `constantSignalHasNoSamplingTerm` (4), `samplingTermFollowsTheDerivation`
  (4), `rotationRemainderFollowsTheDerivation` (5, constant turn),
  `rotationRemainderMatchesTheSchemeError` (5, ramp),
  `latticeCheckFindsTheCoarsestRange` (6: each range's lattice; a
  legacy-style truncated reading; the tolerance edges; no range),
  `latticeCheckIdentifiesEveryFixture` (7), `rateCheckToleratesTenPercent`
  (6; accelerometer reported first; a gyro-only mismatch),
  `configurationChecksComeAfterTheOthers` (an IMU gap plus a NaN
  configuration reports the gap). Covariances are read through the observer
  and `pim.p()` (Interfaces), not through propagation.
- `diagnosticsReportPerStepConstants` becomes `diagnosticsReportTheNoiseModel`
  (criterion 9; `t_ref_degc` check kept).
- `validationRejectsEachDefect`: drop the slope cases.
- `imuRateSharesByNoise`: the noisy step now comes from the sampling term:
  the force steps by 1 m/s^2 between samples 49 and 50, which raises the term
  on the three intervals touching samples 49 and 50; assert their increments
  exceed every other step's.
- `atRestPrefixStopsGrowing`, `smallestSaccFixIsTheAnchor`,
  `driftingBiasSegmentsConverge`, `constantTemperatureKeepsSlopeAtPrior`
  (the series length from `imuTime.size()`, 2501), `startsInMotion...`: keep
  the assertions; update comments stating 10 Hz or counts.
- The three `"batch-temperature-bias-v5"` literals become `v6`.
- Add `dampingSaturationIsASolverFailure` (criterion 14: the forced 1e5 on
  `rest_throughout`, the rule and the failure shape; then the default) and
  `dampingCeilingChangesNothingBelowIt` (criterion 14: the three fixtures and
  `coarse_maneuver`'s prefix fit, 1e5 against 1e12, `sameBitsEverywhere` on
  the channels and the trace). `failureDiagnosticsShape` gains the new
  rule's text in its list of rules; `atRestPrefixStopsGrowing` is
  re-measured (criterion 13).

### Others

- `tst_fusion_golden`: `fixturesAreDeterministic` (14); the comparisons pick
  up the two new rejections from `fusionFixtures()`.
- `tests/fusion/fusiongolden.cpp`: `isExactKey` gains `accel_fs_g`,
  `gyro_fs_deg_s`, `accel_odr_hz`, `gyro_odr_hz`, `bandwidth_hz` (copies);
  `toChannels()` copies the configuration.
- `tst_fusion_session`: `registrationShape` (`v6`); `inputsAreBitIdenticalToFixture`
  (the four attributes); `configurationReachesTheKernel` rewritten: a fixture
  session matches its golden, and the same session with the keys removed (so
  the 12.5 Hz defaults) is rejected with the rate text (clause 45, stated
  path and default path); `naturalSessionEndToEnd` is the default path and
  says so; `v6` literal.
- `tst_fusion_store`: `codeStampChangeDropsRecordOnLoad`'s `resultVersion` row
  stamps `batch-temperature-bias-v5`, the string this phase retires (clause
  62); `kSolverFailureDiagnostics` says `v6`.
- `tst_fusion_jobs`: the two record stamps say `v6`. `tst_result_records`
  keeps its sample string (it tests the format, not the kernel).
- `tst_fusion_runner`: unchanged code; `matchesTheApplicationImportPath` runs
  the 12.5 Hz `naturalSession`.

### Goldens

Capture with the existing tool (`tests/README.md` section 11): fourteen lines,
`wrote 18 files`, the `_time` check of step 3. What changes: everything of
the three successes but `_time`, `rows` and the counts; the nine old
rejections in `algorithm` only; two new files. Add the history paragraph
("The capture of <date> (the documented noise model, part 1, phase 2)"),
listing these. Then the `capture.json` gate step and both test modes.

### Audit (`tests/audit/cleanup_audit.cmake`)

- New group `noise-model` (beside `fusion-model`), each rule with an "Allow:"
  comment and planted once to prove it:
  - `expect_none` of
    `accDensity|gyroDensity|accStepSlope|gyroStepSlope|stepSigma|per_step|gyro_slope_s|acc_slope_s`
    in `src tests docs README.md` excluding `tests/README.md` and
    `tests/acceptance_map.txt` (the goldens are searched: a stale capture
    hits);
  - `expect_only` of the table's distinctive figures (the LPF2 cutoffs
    `1441\.8|1320\.7|1108\.1|295\.5|135\.9`, and the density literals as
    `sensornoise.cpp` spells them) in `src`, allowed only in
    `^src/fusion/sensornoise\.cpp$`;
  - `expect_none` of `#include <(gtsam|Eigen)` in `src/fusion/sensornoise.*`;
  - `expect_none` of `modelling weights|calibrated at a 0.076 s step` in
    `docs/SENSOR_FUSION.md`;
  - `expect_count` rules that `docs/SENSOR_FUSION.md` cites `Table 2`,
    `Table 18`, `Table 65` and `Figure 17` (counts as written) and carries the
    derivation's `h^3 / 12` (clauses 12, 16, 17).
- `fusion-tooling`: `sensornoise` joins the internal-header pattern.
- `stored-results`: both algorithm rules spell `v6`. `fusion-model`: the
  comment "The goldens say ..." says `v6`.
- Traceability: append to the explicit list
  `1008 1009 1010 1011 1012 1013 1014 1015 1016 1017 1040 1041 1043 1045 1046 1047 1051 1052 1053 1062`.
- `tests/README.md` section 10: a bullet for `noise-model`.

### Traceability

Map lines (`tests/acceptance_map.txt`, comment line first):

- 1008 `tst_fusion_kernel latticeCheckFindsTheCoarsestRange`; `tst_fusion_golden rejectionFixturesMatchGolden`; `manual M47`
- 1009 `tst_fusion_kernel latticeCheckFindsTheCoarsestRange`, `configurationChecksComeAfterTheOthers`
- 1010 `tst_fusion_kernel rateCheckToleratesTenPercent`; `tst_fusion_golden rejectionFixturesMatchGolden`; `tst_fusion_session configurationReachesTheKernel`
- 1011 `tst_fusion_kernel noiseFollowsTheTable`
- 1012 `audit noise-model`
- 1013 `tst_fusion_kernel noiseFollowsTheTable`; `audit noise-model`
- 1014 `tst_fusion_kernel samplingTermFollowsTheDerivation`, `constantSignalHasNoSamplingTerm`; `audit noise-model`
- 1015 `tst_fusion_kernel rotationRemainderFollowsTheDerivation`, `rotationRemainderMatchesTheSchemeError`
- 1016 `audit noise-model`
- 1017 `tst_fusion_kernel validationRejectsEachDefect`; `audit noise-model`
- 1040 `tst_fusion_kernel diagnosticsReportTheNoiseModel`; `tst_fusion_golden successFixturesMatchGolden`; `manual M47`
- 1041 `tst_fusion_store codeStampChangeDropsRecordOnLoad`; `manual M48`
- 1043 `tst_fusion_session registrationShape`; `audit stored-results`
- 1045 `tst_fusion_session configurationReachesTheKernel`, `naturalSessionEndToEnd`, `inputsAreBitIdenticalToFixture`; `tst_fusion_golden successFixturesMatchGolden`
- 1046 `tst_fusion_kernel noiseFollowsTheTable`, `configurationWithoutEntryIsRejected`; `audit noise-model`
- 1047 `tst_fusion_kernel samplingTermFollowsTheDerivation`; `audit fusion-model`
- 1051 `tst_fusion_golden rejectionFixturesMatchGolden`; `tst_fusion_kernel latticeCheckIdentifiesEveryFixture`; `manual M47`
- 1052 `tst_fusion_kernel noiseFollowsTheTable`
- 1053 `tst_fusion_kernel constantSignalHasNoSamplingTerm`, `samplingTermFollowsTheDerivation`, `rotationRemainderFollowsTheDerivation`
- 1062 `tst_fusion_store codeStampChangeDropsRecordOnLoad`

`tests/README.md`: section 9.11 rows for these twenty items, and in its intro
how clauses 14 and 15 are settled; appendix K restates 14 and 15
"(as settled)":

- 14: "... `dt^3 / 12` times the second derivative for a step that is a whole
  sample interval, and for a part of one the same derivation's integral over
  the part, the second derivative estimated per sample interval as the
  larger of the changes of slope at its two ends ..."
- 15: "The remainder of the mid-step scheme to second order, for rate and
  force linear across the step: its pure rotation term, second order in the
  step's rotation, and the same expansion's terms in the change of force and
  of rate across the step (coning included), derived and added the same
  way."

Amended items, each restated "(as amended)" in appendix C/I/J, the 9.x row
and the map comment, each head paragraph naming the amendment:

- **215**: the per-step term is the sampling term and the rotation remainder
  of the specification of 1001-1065, in quadrature as `(D^2 + eps^2 / dt) I`;
  a step without signal change and without rotation has the density
  covariance exactly. Evidence: `constantSignalHasNoSamplingTerm`,
  `samplingTermFollowsTheDerivation`.
- **216**: no slopes; the densities are derived from the configuration; the
  step boundaries and the midpoint sampling are unchanged.
  `noiseFollowsTheTable`, `preintegrationHonoursExactBoundaries`.
- **217**: the term has no constant and falls with the step as the sampling
  error does. `samplingTermFollowsTheDerivation`.
- **227**: the diagnostics report the configuration, `model.noise` and `b0`,
  `b1`, `T_ref`. `diagnosticsReportTheNoiseModel` and the existing two.
- **245**: the three tests above in place of the slope tests.
- **247**: `docs/` describes the noise model of 1001-1065 in place of the
  per-step term. `audit fusion-model`, `audit noise-model`.
- **847, 861**: the algorithm string, goldens and stored results are those of
  the specification of 1001-1065.
- **921**: "... and to `v6` with the specification of 1001-1065".
- **925**: the specification of 1001-1065 changes the noise model; the pass
  still reads the converged solution and changes nothing in it.

Searched and holding: 202, 211, 218, 220, 237, 239, 241 (the fixtures change
under them, not the statements), 902, 907, 919, 927.

### Manual steps (`tests/README.md` section 12.9, new)

- **M47 The configuration on the reference recordings (1008, 1040, 1051).**
  M11-M14's commands: each exits 0 or names its outcome; `configuration` is
  16 / 2000 / 12.5 / 12.5 (the default); no lattice or rate rejection. Record
  `stopping.rule`, iterations per pass, `objective`, `quality`,
  `model.noise`, the segments, and whether any fit ended `damping
  saturated` (Decision 7), which the stopping rule now reports instead of
  calling the start converged. Plus, by Python on each
  `SENSOR.CSV`, the largest lattice residuals against the tolerances.
  Numbers go in the report, not adjusted.
- **M48 The first start (1041).** As M45's first start, over fits stored by a
  `v5` build: each stored fit is dropped at its recording's load and fitted
  again once when needed, counted in the status bar.

### Documents (same change)

- `docs/SENSOR_FUSION.md` section 3: the configuration paragraph says the
  model now uses it, and states the lattice and rate checks. Section 4: the
  "Integration and noise" paragraph is rewritten: the table with its
  sources, the per-sample formula, the density, the two derivations with
  what they assume, the high-performance and bandwidth assumptions, the bias
  priors each with its Table 2 row (LA_TyOff +/-20 mg, G_TyOff +/-1 dps,
  G_OffDr +/-0.010 dps/degC); the diagnostics list gains `configuration` and
  `model.noise`. The "Solver and stopping" paragraph gains the damping
  ceiling (1e12, and why GTSAM's 1e5 is too low under these densities) and
  the rule `damping saturated` in its list of rules, as the one other
  failure the fit cannot continue from; the `stopping` key list gains
  `lambda_upper_bound`. Section 6: the three rejections and the hand-edited legacy
  file. Section 7: `v6` and its first start. Section 8: fourteen fixtures
  (three fits, eleven rejections); the `tst_fusion_kernel` row names the step
  model tests; re-read the measured reconstruction numbers from the test logs
  after the capture and update them.
- `docs/DATA_SCHEMA.md` section 7 (the hand-edited legacy file), section 11
  (`v6` in the `records` example), section 12 if it names the version.
- `docs/CALCULATIONS.md` section 17: `v6` "since the documented noise model";
  the first start.
- `tests/README.md`: section 1 rows (`tst_fusion_golden` fourteen, eleven;
  `tst_fusion_kernel`; `tst_fusion_session`; `tst_fusion_store`; the capture
  tool's count); section 11 (intro count, the fixture and rejection tables
  with configuration and rounding, the initializer table, the generator's
  rule, the files' counts, step 2's check, "what changes", the history
  paragraph, "Real recordings").

## Decisions

1. **The noise travels in `Tuning`, the step model takes it explicitly.**
   `planFit` derives `Tuning::noise` as it derives `maxGap`, so the fit, the
   initializer and the reconstruction keep their signatures, while
   `preintegrateImu()` names the one input it reads. The noise is not checked
   by `validateSamples`, so that the new checks come after the old ones.
2. **The sampling term of a part step is the derivation's, not `dt^3/12`**
   (clause 14 as settled). For a step that is a whole sample interval they
   agree. A fix splits most intervals at 5 Hz and 13 Hz; `dt^3/12` per piece
   would understate a half-interval piece 4x. The second derivative is
   per sample interval of the `Samples` handed in, so the fit and the
   reconstruction agree (item 932); the larger end value reads the
   specification's "bounded by"; the first and last interval of a window
   have one end; fewer than three samples give no term.
3. **The rotation remainder includes the expansion's cross terms** (clause 15
   as settled, for Michael). The pure term is the smallest of them on real
   data: at 13 Hz, 1 rad/s, a turn-rate change of 0.2 rad/s per step and
   15 m/s^2, `(dt/24) theta^2 f` is 2.7e-4 m/s and `(dt/12) dtheta f` 1.4e-3,
   against 2.3e-4 of noise per step. Leaving them out would leave a known
   error of the same scheme unmodelled (section 2's principle). On a constant
   turn only the pure term remains, which is the specification's test.
4. **1.6 Hz is rejected, not tabulated** (for Michael). It exists only in
   low-power mode (Table 2 note 11), and the gyro has no 1.6 Hz rate: one
   logged IMU axis can never match both rates, so a low-power row would be a
   table entry nothing can reach.
5. **The accelerometer's tolerance is G_So%'s 1 %** (for Michael). Table 2 has
   no LA_So% row; section 4.6.1 says the accelerometer's sensitivity tolerance
   is in Table 2, whose only tolerance row is G_So%. Phase 3 consumes it.
6. **`rest_throughout` keeps its IMU noise, at the datasheet level**
   (Michael's decision, overruling the plan's first reading, which removed
   it). Probe, with the fixture's old noise amplitude: the noise reads as
   horizontal acceleration and the at-rest yaw sigma falls with each
   doubling (179, 122, 99 deg), so growth stops at 240 s, not 120 s (item
   239); rounded onto the lattice it did not converge, which Decision 7
   explains. Without any noise its readings sit on the lattice like a
   resting FlySight (the corpus's quietest windows show 4-5 accelerometer
   counts and gyro axes with zero spread): sigmas 180 and 180, `no_gain` at
   120 s, 8 fits, roll and pitch within 0.26 deg, converged in 12
   iterations. A fixture with no noise at all is a weaker test than the
   corpus's quiet windows, which do toggle by a few counts, so the noise
   stays, at the amplitude the model says a resting unit has, and item 239
   is restated with what is then measured rather than preserved by removing
   the noise.
7. **The optimizer's damping ceiling is raised and saturation is a solver
   failure** (Michael's decision, overruling the plan's first reading, which
   left the solver alone). With noisy readings rounded, `rest_throughout`'s
   prefix starts saturate Levenberg-Marquardt's damping at GTSAM's default
   upper bound 1e5 (the IMU blocks of the Hessian are ~1e9 under the
   datasheet densities), every iteration returns the same values, and the
   "settled on a step that did not move" rule reports the start (objective
   3.7e10) as converged. That is a kernel weakness any real recording can
   reach, not a fixture problem. Raising `lambdaUpperBound` to 1e12 made it
   converge (84 iterations) and left every fit that never reached the bound
   bit identical; diagonal damping also converged but moved every fit, so
   the ceiling is raised and the damping kept. And a pass that settles
   without moving while at the ceiling is reported as the solver failure it
   is, `damping saturated`, so that a stuck fit is never called converged
   again; the settled rule keeps its meaning below the ceiling. M47 records
   whether any reference recording ends that way.
8. **The rate check uses the whole recording's median interval**, already
   computed for `maxGap`, and the lattice check every reading `Channels`
   holds, inside the window or not.
9. **The fixture rates**: 104, 26 and 12.5 Hz are within 4 %, 4 % and 0 % of
   the sampling. `reject_rate` states the default's 12.5 Hz on a 100 Hz
   recording: the realistic error, a key-less file from a faster firmware.
10. **Portable comparison**: the configuration keys and `bandwidth_hz` are
    copies and exact; the other noise numbers are products and take the
    default bound.

Probe (`experiments/noise_probe/`, built against the third-party installs;
`run1.txt`, `run2_rest.txt`, `run3_rest_quiet.txt`): the new step model in a
copy of `imuintegration.cpp`, the densities set from the table, the readings
rounded. Converged with the existing tests' literals holding:
`coarse_linear` (2 iterations, objective unchanged at 2.9e-12),
`coarse_maneuver` (4; prefix 5 and full 4 iterations, so the cancellation
rows exist), `stationary_spin` (10; prefix 120 s, `covers`), `motion_start`
(4; 120 s, sigmas 180 then 5.7; yaw 52.7 against 53.13), `sacc_anchor` at
12.5 Hz (6; 60 s, sigma 8.0), `drifting_bias` at 12.5 Hz (7; four segments;
`b1` 0.0479 deg/s/degC against 0.05), `rest_throughout` as Decision 6. The
fixtures' fits use a few more iterations; none changed stopping rule.

Ready with caveats: Decisions 3, 4, 5, 6 and 7, and the resampled 10 Hz
fixtures, are flagged for Michael; the phase can be implemented as written.
