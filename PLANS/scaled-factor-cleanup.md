# The scaled IMU factor stands alone

Date: 2026-10-01
Status: specification for planning. Not an implementation plan. Small enough
to be implemented directly by one agent in one phase.
Baseline: branch `store-requested-calculations` at the head that implements
`PLANS/noise-model-part-1.md` and its fixups; the committed code is
authoritative.
Related: `src/fusion/factorgraphfit.h` (`GyroBiasModel::scaleState`),
`src/fusion/factorgraphfit.cpp` (`addTemperatureImuFactor`),
`src/fusion/temperatureimufactor.h/.cpp`, `src/fusion/scaledimufactor.h/.cpp`,
`tests/tst_fusion_kernel.cpp`, `tests/audit/cleanup_audit.cmake` (the
internal-header list of `fusion-tooling`), `docs/SENSOR_FUSION.md` section 4,
`tests/acceptance_map.txt` (the fusion-improvements items that name the
temperature factor's tests).

## 1. Motivation

The scale-factor state replaced the temperature factor: `ScaledImuFactor`
with the scale held at one computes what `TemperatureImuFactor` computed, bit
for bit, as the kernel's tests prove. The application sets `scaleState` on
every full fit (`fusion.cpp`), so the old factor and the flag are reached by
tests alone. The project's convention is that a replaced mechanism is
removed, not kept beside its replacement.

## 2. What changes

- `TemperatureImuFactor` is deleted with its files, its entry in
  `src/CMakeLists.txt` and in the audit's internal-header list.
- `GyroBiasModel::scaleState` is deleted; the full fit always has the scale
  state, as it does today. `addTemperatureImuFactor` builds the scaled factor
  unconditionally and is named for what it builds.
- The initializer's prefix and segment fits, which use the stock
  `gtsam::ImuFactor` with a constant bias, are unchanged.
- No number the fit produces changes: the goldens stay as they are, and the
  exact tests prove it.

## 3. Tests

- The Jacobian tests of the old factor become `ScaledImuFactor`'s: its seven
  Jacobians against finite differences, and its equivalence with the stock
  `gtsam::ImuFactor` at zero slope and unit scale, bit for bit, which the
  library provides as the reference.
- The comparison "with the scale state against without" holds the scale at
  one with a prior a thousand times tighter than the datasheet's in place of
  switching the state off, and asserts what it asserted.
- The fusion-improvements acceptance items that name the temperature
  factor's tests are restated "(as amended)" in the map and the appendices,
  naming the scaled factor's.
- `audit_cleanup` and the whole suite green, the exact tests included;
  `git grep TemperatureImuFactor scaleState` empty in `src`, `tests` and
  `docs`.

## 4. Documentation

`docs/SENSOR_FUSION.md` section 4 describes one IMU factor, the scaled one,
with the temperature model inside it; `tests/README.md`'s rows and
appendices follow the renamed tests.

## Commit Policy

Michael has authorized commits for this plan on the existing working branch.
Nothing is ever pushed, and nothing is ever committed on `master` or
`fusion-improvements`. One commit, subject `Fusion: the scaled IMU factor
stands alone`, with the session's attribution line, staged by explicit path;
`PLANS/` is never staged.
