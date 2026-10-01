# The documented noise model and the accuracy, part 1

Date: 2026-09-30
Status: specification for planning. Not an implementation plan.
Baseline: branch `store-requested-calculations` at the head that implements
`PLANS/fusion-reconstruction.md` and the commits after it (the mid-step
rotation, algorithm string `batch-temperature-bias-v5`); the committed code
is authoritative. Where this document and the earlier specifications
disagree, this one wins; everything it does not mention stays as they
specify.
Related: `docs/SENSOR_FUSION.md` (sections 3, 4, 7 and 8),
`docs/CALCULATIONS.md` (section 17), `docs/COMPUTED_PLOTS.md`,
`docs/DATA_SCHEMA.md` (sections 2, 11 and 12), `tests/README.md` (section
11), `tests/acceptance_map.txt`, `tests/audit/cleanup_audit.cmake`.
`PLANS/noise-model-part-2.md` follows this specification once the bench and
high-rate recordings it names exist; nothing here depends on it.

This document says what the change must do and the boundaries it must
respect. It avoids class layouts and function signatures unless a name is
part of the observable contract. The planning agent decides phasing; the
implementation agent decides code structure.

## 1. Motivation

The fit's noise model is a set of tuning constants: densities of 0.015
m/s^2/sqrt(Hz) and 0.001 rad/s/sqrt(Hz) that `docs/SENSOR_FUSION.md` calls
"modelling weights, not sensor specifications", and per-step slopes of 0.40 s
and 0.026 s calibrated on one recording, at one sample rate, with an
integration error that the mid-step rotation has since removed. The
constants were chosen so that the fit converges on the recordings we have,
and the earlier attempt to lower a density was withdrawn because the errors
it had been covering (scale, drift) were not modelled. A model tuned to a
corpus cannot say what it is confident in, and an accuracy computed under it
is a statement about the tuning.

The users this exists for, in test and measurement, need an acceleration
whose error bar someone can check against a datasheet. That needs a model
whose every parameter has a documented source: the sensor's datasheet, a
derivation, or a controlled measurement. The datasheet and the derivations
exist today; the measurements come in part 2. It also needs the fit to know
how the sensors were configured, because the noise of a sample depends on the
range and rate the user chose, and the file does not yet say what they were.

With such a model in place, the fit's covariance is the accuracy: the
attitude uncertainty of every sample, and the uncertainty of the fused
acceleration that follows from it, published as channels and plotted beside
the values as the GNSS accuracies are. Where a recording exceeds what the
model allows, the fit's residuals say so, and the published accuracy widens
rather than hides it.

## 2. Principles

- **Every parameter has a source**, named in the documentation: a datasheet
  figure with its table, a derivation written out, or (part 2) a measurement
  with its recording. No parameter is a number chosen because the corpus fits
  better with it.
- **The corpus validates, it does not tune.** The normalized residuals over
  the reference recordings are reported as a result. A shortfall names a
  missing term; it is never answered by a wider sigma.
- **The configuration is read, not assumed.** The ranges, rates and filters
  the sensors ran at come from the recording's header where it states them,
  and from a documented default, checked against the data, where it does not.
- **The accuracy is the model's statement**: one standard deviation from the
  covariance of the converged solution under the documented model, widened
  where the residuals exceed what the model allows, and the documentation
  says so in one sentence.
- **Uncertainty is a channel like any other**: stored with the fit's record,
  restored with it, listed by the plot list, readable by a column.
- **A fit output changes, so the result version changes**, once for this
  whole specification: every stored fit is recomputed at its recording's
  next load, and the golden fixtures are captured again by the existing tool.

## 3. Scope

In scope:

- The configuration attributes (section 5): read from the headers, defaulted
  and checked, and carried to the fit.
- The noise model (section 6): densities and quantization by configuration,
  the sampling term by derivation, a scale-factor state, and the corpus
  validation.
- The accuracy (section 7): the covariance, its propagation, the widening,
  and the four channels.
- The plots (section 8), the record, the version and the goldens (section 9).

Out of scope, unchanged:

- The firmware. This specification names the header keys it reads; writing
  them is the firmware's work, agreed separately. Every recording on disk
  lacks them, and the default path is the one exercised first.
- The GNSS factors' weighting and the receiver's dynamic model. The dynamic
  model key is read and stored with the other attributes so that recordings
  carry it; nothing uses it yet.
- Bias instability over time, and any parameter a measurement must settle:
  part 2.
- The reconstruction between fixes, the demand layer, the executor, the
  status bar, the logbook.

## 4. Terms

- **Configuration**: the accelerometer's and the gyro's full-scale range,
  output data rate and low-pass filter setting, and the receiver's dynamic
  model, as the recording states them or the default supplies them.
- **Lattice**: the set of values a sensor can log at a range: multiples of
  the range divided by 32768, the one property of a recording that reveals
  its range.
- **Per-sample noise**: one standard deviation of a single reading, in the
  reading's unit, from the datasheet's noise density at the range, the
  bandwidth at the rate and filter, and the quantization step at the range.
- **Sampling term**: the error the integration makes by treating the samples
  as a piecewise-linear signal, per step, as a standard deviation.
- **Scale factor**: the per-axis factor by which a sensor's reading differs
  from the true rate or specific force, near one.
- **Widening**: the factor, never below one, by which the published
  accuracies of a stretch of a fit are multiplied when its residuals exceed
  what the model allows.

## 5. Configuration attributes

- **The keys**, as the firmware will write them, with actual values rather
  than register codes, as the rest of the file is written. SENSOR.CSV:
  `ACCEL_FS` (full-scale range, g: 2, 4, 8 or 16), `GYRO_FS` (deg/s: 250,
  500, 1000 or 2000), `ACCEL_ODR` and `GYRO_ODR` (output data rate, Hz, as
  the part names it: 12.5, 26, 52, 104, 208, 416, 833, 1666, 3333, 6666, and
  1.6 for the accelerometer's low-power rate), and, for completeness,
  `BARO_ODR`, `HUM_ODR` and `MAG_ODR` (Hz), which nothing in this
  specification uses. TRACK.CSV: `GNSS_MODEL`, the receiver's dynamic model
  by name rather than by its code, so that the file explains itself
  (`portable`, `stationary`, `pedestrian`, `automotive`, `sea`,
  `airborne_1g`, `airborne_2g`, `airborne_4g`), and `GNSS_RATE`, the
  measurement rate in Hz. The IMU's low-pass filters are not configurable in
  the firmware, so they have no key: the fixed setting is documented as part
  of the default, with the firmware version, and a key is added only if a
  firmware makes them configurable. The keys and their value forms are
  recorded in `docs/DATA_SCHEMA.md` section 2 beside `SCHEMA_VER`.
- **Reading them.** The importer stores them as header attributes, as it
  stores `SCHEMA_VER`; a malformed value is an import error naming the key,
  as a malformed `SCHEMA_VER` is.
- **The default.** A recording that lacks a key takes the value the firmware
  of the recordings on disk used: ±16 g, ±2000 deg/s, 12.5 Hz for both
  sensors, the filters as that firmware fixes them (the gyro's LPF2 at its
  12.5 Hz cutoff and no LPF1, the accelerometer at its ODR bandwidth); no
  dynamic model, no rate. The default is written once, in the documentation
  and in one place in the code, with the firmware version it describes.
- **The check.** Whether stated or defaulted, the ranges are checked against
  the lattice of the recorded values: the coarsest range whose lattice the
  values fit, to within the rounding of the file's decimals, must be the
  configured one. A recording that fails the check is rejected by the fit
  with a reason that names the range stated and the range the values show;
  the fit does not guess.
- **To the fit.** The configuration reaches the kernel as part of its
  channels, as the origin does; the kernel never reads a preference or a
  constant for it. A logged interval that disagrees with the stated rate by
  more than the oscillator's tolerance (a few percent) is a rejection that
  names both.

## 6. The noise model

- **Per-sample noise**, for each sensor and axis:
  `sigma = sqrt(density^2 x bandwidth + step^2 / 12)`, with the density the
  datasheet's at the configured range, the bandwidth the datasheet's for the
  configured rate and filter, and the step the range divided by 32768. The
  documentation carries the table: density per range for each sensor,
  bandwidth per rate and filter, with the datasheet's table numbers.
- **The density the integration uses** is the per-sample noise times the
  square root of the nominal sample interval at the configured rate, so that
  a step of the nominal length carries one sample's variance. The constants
  `accDensity` and `gyroDensity` go; nothing in the model is a density that
  is not derived this way.
- **The sampling term** replaces the slopes. It is the error of the
  trapezoid rule on a piecewise-linear signal, derived: for a step of length
  `dt` the velocity error is bounded by `dt^3 / 12` times the signal's second
  derivative, which the change of the signal's slope between the step and
  its neighbours estimates; the same for the angle from the rate. The term
  is added in quadrature per step as today, with no fitted coefficient; the
  documentation writes the derivation out, including what it assumes (a
  smooth signal between samples, the samples exact). The rotation remainder
  of the mid-step scheme, second order in the step's rotation, is derived and
  added the same way.
- **Bias priors** stay at the datasheet's typical offsets, which they already
  match (0.3 m/s^2 against ±20 mg; 0.03 rad/s against ±1 deg/s; 0.010
  deg/s per °C), and the documentation cites the table for each.
- **A scale-factor state**: per fit, a factor per axis for each sensor,
  starting at one, with the datasheet's sensitivity tolerance as a zero-mean
  prior on its departure from one. The readings enter the integration
  divided by it; the graph is re-preintegrated at the fitted scale as it is at
  the fitted bias, under the same settled test. A recording without motion
  leaves the scale at its prior; the fitted scale and its sigma are
  reported in the diagnostics beside the biases. The `1.14688` schema
  correction of legacy recordings is not a scale factor and is untouched.
- **Validation, not tuning.** On the reference recordings of `tests/README.md`
  section 12.2 and on the committed fixtures, the fit's normalized residuals
  (position, velocity, IMU) are reported in the documentation as measured
  under this model, with the sentence that a value far from one measures what
  the model does not yet describe. No constant of this section is changed to
  move them.

## 7. The accuracy

- **What the fit computes**, after convergence, from the converged graph:
  the marginal covariance of every fix state, its cross-covariance with the
  biases and the scale factors (by the solves of the factorized system, not
  by the library's joint marginals, which are too slow), and, through the
  reconstruction's pass, the conditional covariance of the state at every
  IMU sample given the two fixes around it. One factorization; the step is
  inside the same cancellable job and is not a boundary.
- **Attitude accuracy per sample**: heading, the square root of the
  navigation-frame rotation covariance's element about the vertical; tilt,
  the square root of the sum of its two horizontal elements; both in degrees,
  capped at 180 as the heading check caps its own, 180 meaning undetermined.
- **Acceleration accuracy per sample**: the first-order propagation of the
  joint covariance of attitude, accelerometer bias and accelerometer scale
  through `a = R (f / s - b) + g`, plus the per-sample noise of the
  accelerometer, rotated; from the resulting 3x3 covariance in the navigation
  frame, the horizontal accuracy is that of the plotted horizontal magnitude
  (the variance along the horizontal acceleration's direction; where the
  horizontal acceleration is below the accuracy itself, the larger horizontal
  principal value) and the vertical accuracy is the vertical element. Both
  in m/s^2. The documentation states the propagation in symbols and what it
  leaves out (gravity's own uncertainty, cross-axis sensitivity, the
  interpolation between nodes).
- **Widening.** Over a window of fixes around each sample (the planner sets
  its length, of the order of a few seconds), the sum of the fit's squared
  whitened residuals of every factor divided by the window's degrees of
  freedom gives a factor; the published accuracies of the sample are
  multiplied by the square root of the factor where it exceeds one and are
  unchanged where it does not. The documentation says what the factor is (the
  a-posteriori variance factor of the window), that it assumes every sigma is
  off by the same ratio, and that it never tightens.
- **The channels**: `Fusion/headingAcc`, `Fusion/tiltAcc`, `Fusion/accHAcc`,
  `Fusion/accDAcc`, aligned with `Fusion/_time`, outputs of the fit, absent
  for a rejected or failed fit and for a successful fit whose factorization
  failed (the diagnostics say so; nothing else about that fit changes).
- **Undetermined heading** does not fail the computation: the cap applies,
  and the acceleration accuracies use the horizontal magnitude's direction,
  which a heading error does not move.

## 8. What the user sees

The "Sensor fusion" category gains four plots after Roll, named as the GNSS
accuracy plots are: Heading accuracy and Tilt accuracy in degrees, Horizontal
acceleration accuracy and Vertical acceleration accuracy in the acceleration
unit, drawn in the deep colours of the GNSS accuracy plots. They are absent,
like any unavailable value, where the fit did not compute them. A logbook
column over any of them works as over any fusion value. The diagnostics show
the fitted scale factors and the configuration the fit ran under. The first
start after the change recomputes every stored fit; the status bar shows it
as any computation.

## 9. The record, the version and the goldens

- The fit's outputs gain the four channels; the record stores and restores
  them with the rest, in the record format as it is.
- The configuration attributes are session attributes and travel with the
  session as `SCHEMA_VER` does.
- The algorithm string changes once, for the whole specification, so every
  stored fit is stale at its recording's next load.
- The golden fixtures are captured again with the existing tool; their time
  axes are unchanged and checked byte for byte; the golden comparison then
  covers the new channels, the scale factors and the configuration in the
  diagnostics. The fixtures state their configuration explicitly, so that the
  default path and the stated path are both exercised.

## 10. Architecture

- **The importer** reads and validates the keys; **the session** stores them
  as attributes; **the fusion registration** declares them as inputs of the
  fit, as it declares the origin, and the input adapter carries them to the
  kernel. Nothing above the registration changes.
- **The kernel** derives its noise from the configuration in one unit, which
  is the only place the datasheet table exists in code; the sampling term
  and the rotation remainder are computed where the steps are integrated,
  from the readings, with no constant of their own. The scale factors are
  variables of the graph beside the biases. GTSAM stays confined to the
  kernel; the covariance step is a unit of its own, as the reconstruction is.
- **The lattice check** is a kernel validation rule, one of the rules that
  decide whether a recording can be fitted, and reads the raw readings before
  any correction.
- **The plot registry** gains the four plots.

## 11. Tests

- Configuration: a file with every key imports them; a malformed one is an
  import error naming the key; a file without them takes the default; a
  fixture whose values sit on the ±8 g lattice with a header stating ±16 g is
  rejected naming both; the lattice check identifies the range of every
  committed fixture and of the reference recordings.
- Noise: the per-sample noise and the density follow the table for every
  configuration the fixtures state, bit for bit against the formula; a step
  with a constant signal has no sampling term; the sampling term of a step
  follows the derivation on a synthetic signal with a known second
  derivative; the rotation remainder follows its derivation on a constant
  turn.
- Scale: a synthetic recording with the accelerometer 2 % high on one axis
  recovers the factor within the prior's tolerance and the position misfit
  falls against a fit without the state; a recording at rest leaves the
  scale at one within its prior.
- Validation: the normalized residuals of the committed fixtures are in the
  goldens; the reference recordings' are recorded in the documentation.
- Accuracy: on the committed fixtures a successful fit publishes finite,
  positive accuracies for every sample; scaling every GNSS accuracy up never
  lowers them; the heading accuracy of the first node agrees with the heading
  check; the acceleration accuracy follows its propagation on synthetic
  inputs with known answers, against the library's joint marginals on a short
  fixture; the widening is one where residuals are at the model and grows
  where a factor's sigma is understated by a known ratio; the cap holds on a
  fixture whose heading is undetermined.
- The estimate before the goldens are recaptured: the existing channels of
  every fixture are bit-identical to the goldens on a build where the
  configuration is defaulted and the noise constants are set to reproduce
  the previous model, which the planner arranges as a check that the
  plumbing changes nothing by itself; then the model changes and the goldens
  are captured.
- The four plots are explicit-backed like the other fusion plots, and a
  column over one works; a stored fit from before the change is stale; the
  audit's confinement rules hold; the documents carry the table, the
  derivations and the validation.

## 12. Documentation

`docs/DATA_SCHEMA.md` (section 2, the keys; section 11, the algorithm string;
section 12, the record); `docs/SENSOR_FUSION.md` (section 3, the
configuration inputs and the lattice check; section 4, the noise model with
its table and derivations, the scale state, the covariance and its
propagation, the widening, the outputs and diagnostics; section 8, the
validation results and what is and is not validated); `docs/CALCULATIONS.md`
(section 17); `docs/COMPUTED_PLOTS.md` (the four plots); `tests/README.md`
section 11 (the goldens captured again, and why) and
`tests/acceptance_map.txt` (a new range in the next free hundred; amended
items restated "(as amended)").

## Commit Policy

Michael has authorized commits for this plan on the existing working branch.
The implementation orchestrator makes every commit; implementation, revision,
and review agents never run git commands that change repository state.
Nothing is ever pushed, and nothing is ever committed on `master` or
`fusion-improvements`. Copy this section into the plan overview as its own
top-level section with this exact heading.

- **Branch.** Confirm the working tree has no modified tracked files (if there
  are any, stop and ask). Switch to the existing branch
  `store-requested-calculations`; never recreate, reset, or rebase it.
- **Never staged:** `PLANS/implementation-plan/`, `TEMP/`, `experiments/`,
  `build*/`, `dist/`, `results/`, and everything under `third-party/` that
  is not tracked. Specifications and archived plans under `PLANS/` are
  committed by Michael, never in a phase commit.
- **One commit per accepted phase**, when the review agent returns ACCEPT,
  containing exactly the files the phase's agents reported, staged by explicit
  path. Subject `Noise model part 1 phase N: <phase name>`; body a short
  summary, then the session's attribution line. Rejected iterations are never
  committed.
- **Tag** each phase commit `plan/noise-model-part-1/phase-N-done`; tags are
  never moved or deleted.
- **Fixes to a closed phase** are committed separately as
  `Noise model part 1 phase N fixup: <what>` once reviewed and accepted.
- **CI workflow edits** are committed separately as
  `Noise model part 1 phase N: CI (unverified)`.
- **Escalated phases** are not committed; their paths are listed in the final
  report, and phases that depend on them are not started.
