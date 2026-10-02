# Implementation plan: the documented noise model and the accuracy, part 1

Plan for `PLANS/noise-model-part-1.md`, written 2026-09-30 against branch
`store-requested-calculations` at `b69eddd`. Five phases, strictly
sequential. The specification below is the authority; where a phase document
and the specification disagree, the specification wins, except where a
decision of this overview reads the specification's letter otherwise and
says so (section "Decisions and constraints"; each such reading is flagged
for Michael and is stated "as settled" in the acceptance clause it touches).

## Feature specification

## The documented noise model and the accuracy, part 1

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

### 1. Motivation

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

### 2. Principles

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

### 3. Scope

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

### 4. Terms

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

### 5. Configuration attributes

- **The keys**, as the firmware will write them, with actual values rather
  than register codes, as the rest of the file is written, and with the
  unit in the key's name, as the fusion diagnostics name theirs
  (`anchor_sacc_m_s`, `yaw_sigma_deg`), since a `$VAR` line has one value
  and no unit field. SENSOR.CSV: `ACCEL_FS_G` (full-scale range: 2, 4, 8 or
  16), `GYRO_FS_DEG_S` (250, 500, 1000 or 2000), `ACCEL_ODR_HZ` and
  `GYRO_ODR_HZ` (output data rate as the part names it: 12.5, 26, 52, 104,
  208, 416, 833, 1666, 3333, 6666, and 1.6 for the accelerometer's low-power
  rate), and, for completeness, `BARO_ODR_HZ`, `HUM_ODR_HZ` and `MAG_ODR_HZ`,
  which nothing in this specification uses. TRACK.CSV: `GNSS_MODEL`, the
  receiver's dynamic model by name rather than by its code, so that the file
  explains itself (`portable`, `stationary`, `pedestrian`, `automotive`,
  `sea`, `airborne_1g`, `airborne_2g`, `airborne_4g`), and `GNSS_RATE_HZ`,
  the measurement rate. The IMU's low-pass filters are not configurable in
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

### 6. The noise model

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

### 7. The accuracy

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

### 8. What the user sees

The "Sensor fusion" category gains four plots after Roll, named as the GNSS
accuracy plots are: Heading accuracy and Tilt accuracy in degrees, Horizontal
acceleration accuracy and Vertical acceleration accuracy in the acceleration
unit, drawn in the deep colours of the GNSS accuracy plots. They are absent,
like any unavailable value, where the fit did not compute them. A logbook
column over any of them works as over any fusion value. The diagnostics show
the fitted scale factors and the configuration the fit ran under. The first
start after the change recomputes every stored fit; the status bar shows it
as any computation.

### 9. The record, the version and the goldens

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

### 10. Architecture

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

### 11. Tests

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

### 12. Documentation

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

### Commit Policy

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

## Phases

| # | Name | Purpose | Depends on |
| --- | --- | --- | --- |
| 1 | Configuration attributes | The nine header keys read, validated, stored and travelling with the session; the four IMU keys defaulted by constant-default calculations and declared as inputs of the fit; carried into `Fusion::Channels`, which the kernel receives and does not yet use. No number of the fit changes: the existing goldens pass unchanged, which is the specification's plumbing check (section 11, "The estimate before the goldens are recaptured"). Also opens the acceptance range 1001-1065 | - |
| 2 | The noise model by configuration | The kernel's datasheet unit: per-sample noise, derived density, quantization step and sensitivity tolerance by configuration; the sampling term and the rotation remainder in place of the slopes; the lattice check and the rate check as kernel validation rules; the optimizer's damping ceiling raised to 1e12 and a pass that settles without moving at the ceiling reported as the solver failure `damping saturated`; the configuration and the noise in the diagnostics; fixtures that state their configuration and lie on its lattice; the algorithm string `batch-temperature-bias-v6`; goldens captured again | 1 |
| 3 | The scale-factor state | One scale factor per axis per sensor in the full fit, with the datasheet's sensitivity tolerance as its prior; the readings divided by it in the integration and in the reconstruction; re-preintegration at the fitted scale under the settled test; the fitted scale in the diagnostics; goldens captured again | 2 |
| 4 | The accuracy | The covariance step (one factorization, a unit of its own), the conditional covariance at every IMU sample through the reconstruction, the attitude and acceleration accuracies, the widening, the four channels as outputs of the fit and of its record, the scale sigmas, the diagnostics' account; goldens captured last | 3 |
| 5 | Plots, validation and traceability | The four plots, the column over them, the validation of the model on the reference recordings and the fixtures recorded in the documentation, the remaining documents, and the acceptance range closed | 4 |

Every phase blocks the next; nothing runs in parallel. Phase 1 must land
before anything changes a number, so that the goldens are its check. Phases
2, 3 and 4 all edit the kernel's integration, fit, reconstruction and
diagnostics and all re-capture the goldens, so they cannot overlap; phase 5's
plots need the channels phase 4 declares (a plot over a name the fit does not
declare is not explicit-backed, and `tst_fusion_rows` fails).

```text
1 Configuration attributes -> 2 Noise model -> 3 Scale state -> 4 Accuracy -> 5 Plots, validation, traceability
```

## Key patterns and references

### Project rules

- `CLAUDE.md`, `CLAUDE.local.md`: build and test in `build-agent/` only,
  Release, sequentially; never build `build/` or `build-deps/`.
- `.claude/docs/WORKFLOW-REFERENCE.md`: the stages and their conventions.
- `PLANS/done/fusion-reconstruction-plan/` (00-overview and the three
  phases): the previous plan on the same kernel; the form of the phase
  documents, the probe-driven "as settled" readings, the traceability phase.

### The datasheet and the measurements behind the specification (local, untracked)

- `TEMP/DS_lsm6dso.pdf`: LSM6DSO datasheet DS12140 Rev 3. Table 2 (pages
  9-10): ranges, sensitivities (LA_So 0.061/0.122/0.244/0.488 mg/LSB; G_So
  4.375/8.75/17.50/35/70 mdps/LSB), sensitivity tolerance (+/-1 %), zero-g and
  zero-rate offsets (LA_TyOff +/-20 mg, G_TyOff +/-1 dps), G_OffDr (+/-0.010
  dps/degC), noise densities (Rn 3.8 mdps/sqrt(Hz), independent of ODR and FS;
  An 70/75/80/110 ug/sqrt(Hz) at +/-2/4/8/16 g in high-performance mode),
  low-power RMS noise (its note 10, "Noise RMS related to BW = ODR/2", belongs
  to those rows; the high-performance ODR/2 bandwidth is Figure 17 note 1 and
  Table 65, as phase 2 found), the ODR list (the datasheet prints 3332 and 6664 where the specification's keys
  write 3333 and 6666). Table 18 (page 29): the gyro's LPF2 cutoff per ODR
  (4.2 Hz at 12.5 Hz). Table 142-143: INTERNAL_FREQ_FINE, the effective ODR's
  deviation from typical in 0.15 % steps. The text extraction of Table 2 is
  scrambled; read the pages as images (`Read` with `pages`) before quoting a
  figure.
- `experiments/fusion_lab/NOTES.md` section 2: the firmware v2023.09.22
  configuration (`imu.c`: accelerometer and gyro at 12.5 Hz, +/-16 g,
  +/-2000 dps, no LPF1 on the gyro, BDU on; logged interval 75.6 ms, 13.2 Hz),
  and the datasheet figures against the model.
- `TEMP/firmware-changes.md`: the firmware's CSV formats (`log.c`: angular
  rates in millidegrees per second written as deg/s with three decimals); the
  accelerometer is written in g with five decimals (see any `SENSOR.CSV`, for
  example `TEMP/17-26-24/SENSOR.CSV`).
- `experiments/accuracy_probe/noise_floor.py` and `noise_floor.txt`: the
  quietest-window noise of 95 corpus recordings, 0.0029-0.0035 m/s^2 per
  sample at 12.5 Hz, +/-16 g, which the datasheet formula reproduces (0.0030).
- `experiments/accuracy_probe/probe.cpp`, `run1.txt`, `run2_cross.txt`:
  joint covariance of pose rotation and accelerometer bias on the real
  kernel; `Marginals::jointMarginalCovariance` at 62 ms per node, against
  solves of the eliminated system (`backSubstitute(backSubstituteTranspose(e_j))`)
  that give the same cross-covariance to 1e-11 in under 0.1 s; they disagree
  on `coarse_linear`, whose heading is undetermined (a singular system).
- `experiments/reconstruction_probe/` (`FINDINGS.md`, `probe.cpp`): how the
  previous plan probed the real kernel before writing a phase; the build
  setup a new probe can copy.

### Import, session, export, merge (phase 1)

- `src/dataimporter.cpp` / `.h`: `importHeaderRow` stages `$VAR` lines;
  the `SCHEMA_VER` validation after the header ("Validate the declared schema
  before reading any data row"), the pattern for a malformed configuration
  value; `publish` stores header attributes verbatim.
- `src/conversion/schematable.h` / `.cpp`: "the one authority on recorded
  data schemas", the pattern for a vocabulary table with a validation
  function and a message. It is in `flysight_core`, which the fusion library
  does not link.
- `src/dataexporter.cpp` (`validateSchema`, `headerBytes`): attributes
  written back as stored; a stored value the importer would reject never
  reaches a file.
- `src/sessionmerge.cpp`: the header-attribute conflict rule
  (`docs/DATA_SCHEMA.md` section 8).
- `src/sessiondata.h`: `SessionKeys` (Viewer's own keys, with a leading
  underscore); header attributes are stored as the recorded text.
- `src/CMakeLists.txt`: `flysight_model` (sessiondata, engine, csvformat,
  attributeregistry), `flysight_core` (importer, conversion, ...),
  `flysight_fusion` (links `flysight_model`, not `flysight_core`).
- Tests: `tests/tst_importer.cpp`, `tests/tst_session_merge.cpp`,
  `tests/tst_import_merge.cpp`, `tests/tst_persistence_roundtrip.cpp`,
  `tests/tst_schema_units.cpp` (the `SCHEMA_VER` cases to mirror).

### The registration and the engine

- `src/fusion/fusionregistration.h` / `.cpp`: `kFitInputs`, `fitInputs()`
  (measurements, then the four origin attributes), `channelsFrom()` (the one
  assembly, with the origin index's not-a-number rule), `kFitOutputs`,
  `fitOutputChannels()`, `publish()`, `registerOrientation()` (a vocabulary
  from a fusion unit, its constant default through
  `Calculations::addConstantDefault`).
- `src/calculations/attributecalculations.h`: `addConstantDefault` (inline so
  that the fusion library can call it).
- `src/fusion/orientation.h` / `.cpp`: the pattern for one authority on a
  vocabulary that the registration reads.
- `docs/CALCULATIONS.md` sections 3 (all declared inputs are required), 5
  (defaults are calculations; a stored value wins), 15.8 (stored results),
  17 (sensor fusion as a registered calculation: inputs, outputs, outcome
  mapping, the record's leaves and resolutions).
- `src/calculationrecord.cpp`, `src/calculationresultstore.cpp`: the record
  format and the store (the four new channels must round-trip without a
  format change).

### The kernel (`src/fusion/`)

- `fusion.h`: `Channels`, `Result` (the seventeen arrays), `run()`,
  `Algorithm` (the one literal).
- `fusion.cpp`: `planFit` (prepare, checks, tuning, window, bias model),
  `fitAndAssemble` (initializer, fit, reconstruction, channels, diagnostics),
  `runPipeline` (Rejected before the fit, SolverFailed from the fit on).
- `inputadapter.h` / `.cpp`: `prepareInput`, the order of the channel checks
  (the first violation is the reported reason), the deg/s to rad/s conversion.
- `fusionsamples.h` / `.cpp`: `Samples`, `Tuning` (the densities, slopes,
  bias priors, stopping and initializer constants), `validateSamples`,
  `requireUsableRecording`, `fittedWindow`, `requireNoGnssOutage`: every rule
  that decides whether a recording can be fitted.
- `imuintegration.h` / `.cpp`: `integrationEdges`, `interpolateAt`,
  `preintegrationParams`, `preintegrateImu` (the one integration loop: the
  midpoint readings, the mid-step turn, `stepSigma` / `stepCovariance`, the
  observer), `propagateAttitude`.
- `factorgraphfit.h` / `.cpp`: keys `X`, `V`, `B`, `T`; `buildFactorGraph`
  (factor order fixes the elimination ordering), `fitFactorGraph` (passes,
  re-preintegration at the fitted bias, the settled test, the slow tail),
  `collectResiduals` (kinds and dimensions), `yawSigmaDeg` (the heading check:
  `Marginals` QR, rotation block into the navigation frame, cap 180).
- `temperatureimufactor.h` / `.cpp`: an IMU factor with an extra variable
  (the slope `T(0)`) through the chain rule on the bias Jacobian: the
  pattern for a factor that also depends on the scale.
- `initializer.h` / `.cpp`: the prefix and segment fits (stock factors, the
  constant bias model).
- `trajectoryreconstruction.h` / `.cpp`: `reconstructInterval` (forward
  states, `P_j`, `F_j`, `M_j` from the preintegration's observer; the
  sharing; the step corrections), `reconstructAtImuRate` (the published
  acceleration `R (f - b_a) + g + (c_before + c_after) / 2`).
- `fusionoutput.h` / `.cpp`: `fillOutputChannels`, `successDiagnostics`,
  `modelSummary` (`model.per_step`, `model.gyro_bias`), `kDenseOutput`,
  `kLimitations` ("no uncertainty is published").
- `fusionpipeline.h`: `runPipeline` with `PipelineTrace`, the test seam.

### Kernel tests, fixtures and goldens

- `tests/fusion/fusionfixtures.h` / `.cpp`: the twelve golden fixtures and
  the initializer fixtures; Qt Core only, nothing from `src/`; bit
  reproducibility rules; its hash is in `capture.json`.
- `tests/fusion/fusiongolden.h` / `.cpp`: the channels file (seventeen
  columns, hex bits), `compareSamples`, `compareJson`, the portable floors
  (`portableFloor`: `_deg` keys, heading channels), `toChannels`.
- `tests/fusion/fusionsessions.h` / `.cpp`: fixtures as real sessions (the
  attributes a session carries).
- `tests/fusion/fusiontrace.h`, `tests/fusion_golden_capture.cpp` (the
  existing tool), `tests/fusion_runner.cpp` (`--dump-inputs`, `--csv`).
- `tests/data/fusion/`: the goldens and `capture.json`.
- `tests/tst_fusion_golden.cpp`, `tests/tst_fusion_kernel.cpp` (stages,
  per-step covariance, temperature factor, reconstruction tests),
  `tests/tst_fusion_session.cpp` (`registrationShape`),
  `tests/tst_fusion_store.cpp` (`codeStampChangeDropsRecordOnLoad`,
  `restoredAfterRestartIsBitIdentical`), `tests/tst_fusion_jobs.cpp`,
  `tests/tst_fusion_rows.cpp` (`allEightFusionPlotsAreExplicitBacked`),
  `tests/tst_fusion_runner.cpp`, `tests/tst_fusion_derived.cpp`.
- `tests/README.md` section 11: fixtures, files, tolerance policy, the
  capture tool, the re-capture procedure (the history paragraphs each capture
  adds), fusion sessions, real recordings; section 12.2: the reference
  recordings under `TEMP/data/` and the runner's manual steps (M10-M15).

### Plots (phase 5)

- `src/mainwindow.cpp` `registerBuiltInPlots`: the GNSS accuracy rows
  (`S_dk`, `L_dw`, `L_dc`, `L_db`: the deep colours) and the eight "Sensor
  fusion" rows.
- `src/plotregistry.*`, `src/plotutils.*` (`plotColor`), `docs/COMPUTED_PLOTS.md`,
  `tests/tst_plot_color.cpp`, `tests/tst_plot_format.cpp`,
  `tests/tst_fusion_rows.cpp`.

### Audit and traceability

- `tests/audit/cleanup_audit.cmake`: groups `solver-confinement` (GTSAM
  headers only under `src/fusion/` and the listed tests; public and
  registration files GTSAM-free; the kernel is pure and does not log),
  `fusion-model` (three kinds of boundary, `checkpoint(` counted 3; one
  `integrateMeasurement`; the step model has one author: sensor covariances
  are set only in `imuintegration.cpp`; the fusion document bans
  "twenty-one"), `fusion-tooling` (the tools see the public header or the
  trace seam only: a new internal header is added to its pattern),
  `stored-results` (one algorithm literal, in `fusion.h`),
  `constant-defaults`, `orientation`, `fusion-reconstruction`; the
  traceability block at its end (the allowed item ranges and the
  completeness loops).
- `tests/acceptance_map.txt`: the head comment (one paragraph per range), the
  four line forms, the 901-940 block as the latest example.
- `tests/README.md` section 9.10 and appendix J: how the previous
  specification's clauses were numbered, stated, and "as settled".

### Documents

- `docs/DATA_SCHEMA.md` sections 2 (file format, `$VAR`, import errors), 3
  (`SCHEMA_VER`), 4 (the legacy gyro scaling, 1.14688 = 0.070 /
  (2000 / 32768)), 6 (conversion: g x 9.80665), 8 (merge), 11 (column cache),
  12 (stored results).
- `docs/SENSOR_FUSION.md` sections 2 (plots), 3 (inputs), 4 (model, noise,
  outputs, diagnostics), 5 (limitations: "a per-unit gyro scale factor is not
  fitted"), 6 (rejections), 7 (lifecycle, boundaries, the algorithm string),
  8 (validation).
- `docs/CALCULATIONS.md` section 17; `docs/COMPUTED_PLOTS.md`.

## Decisions and constraints

Decisions made during discovery. Those marked **(for Michael)** read the
specification's letter, or settle something it leaves open, in a way he may
want to overrule.

1. **Sequencing is the plumbing check.** The specification asks for a build
   on which the configuration is carried and defaulted and the noise model is
   the previous one, with the existing channels bit-identical to the goldens.
   Phase 1 is that build: it carries the configuration to the kernel and
   changes no number, and its exit criterion is that every existing golden
   test passes unchanged, exact mode included. No test-only switch that
   reproduces the previous model is added; the previous model is simply still
   the model at the end of phase 1. Clause 61.

2. **The vocabulary lives in `flysight_model`.** The importer
   (`flysight_core`) validates the keys and the fusion registration
   (`flysight_fusion`, which links `flysight_model` but not `flysight_core`)
   registers the defaults, so the one authority on the keys (names, the file
   that carries each, the value forms, the default of the four IMU keys with
   the firmware version) is a new unit in `flysight_model`,
   `src/sensorconfiguration.h` / `.cpp`, as `schematable.h` is for
   `SCHEMA_VER`. The datasheet figures are a different fact and live in the
   kernel (decision 9).

3. **The default is a constant-default calculation.** "Defaults are
   calculations" (`docs/CALCULATIONS.md` section 5), and all declared inputs
   are required, so a recording without a key reads it from
   `builtin.default.<KEY>`, registered by the fusion registration through
   `Calculations::addConstantDefault` with the value from the vocabulary, as
   the orientation's default is. The importer stores nothing for an absent
   key. The kernel receives a configuration in every run and applies no
   default of its own. Merging a file that adds a key later changes what
   provides that input, so a stored fit goes stale, as for any input.

4. **Only the four IMU keys are inputs of the fit** (for Michael):
   `ACCEL_FS_G`, `GYRO_FS_DEG_S`, `ACCEL_ODR_HZ`, `GYRO_ODR_HZ`. The
   specification's section 10 says the registration declares "them"; the
   fit uses only these four, and declaring `GNSS_MODEL`, `GNSS_RATE_HZ` or the
   other sensors' rates would make every stored fit stale when an unused value
   changed. They are read, validated and stored (clause 6) and nothing uses
   them. The diagnostics' configuration is the four the fit ran under.

5. **Value forms.** A key with a listed set of values accepts exactly the
   spellings the specification lists (`16`, not `16.0`; `12.5`; `1666`),
   as `SCHEMA_VER` accepts `2` and rejects `2.0` and `+2`; `GNSS_MODEL`
   accepts the eight names. `BARO_ODR_HZ`, `HUM_ODR_HZ`, `MAG_ODR_HZ` and
   `GNSS_RATE_HZ` have no list in the specification; phase 1 accepts a
   positive finite number in plain decimal form and states the grammar in
   `docs/DATA_SCHEMA.md`. The error text names the key and the value, in the
   form of `Schema::unsupportedMessage`.

6. **The quantization step and the lattice are the datasheet's sensitivity**
   (for Michael). The specification writes the step as "the range divided by
   32768". For the accelerometer that is the datasheet's sensitivity exactly
   (0.061 mg at +/-2 g ... 0.488 mg at +/-16 g). For the gyro the datasheet's
   sensitivity is 1.14688 times it (70 mdps at +/-2000 deg/s, not 61.04): the
   ratio `docs/DATA_SCHEMA.md` section 4 documents. The plan uses the
   sensitivity for both sensors, in the quantization term and in the lattice.

7. **The lattice check runs on the readings the kernel receives** (for
   Michael). The specification says it "reads the raw readings before any
   correction". Only the conversion layer may read source values
   (`docs/CALCULATIONS.md` section 3), so the check reads the effective
   readings, before any correction of the kernel's own (bias, scale, the
   mid-step turn, the conversion to rad/s), with the lattice in effective
   units: the sensitivity times 9.80665 for the accelerometer, the
   sensitivity in deg/s for the gyro. Legacy gyro values lie on that lattice
   too: the legacy correction maps `counts x range / 32768` onto
   `counts x sensitivity`. The tolerance comes from the decimals FlySight 2
   writes (five of g, three of deg/s) carried through the conversion layer's
   factors (x 9.80665; x 1.14688 at most). Phase 2 settles it at one unit of
   the last decimal, not half of one: firmware v2023.09.22 truncates rather
   than rounds (`TEMP/17-26-24/SENSOR.CSV` shows residuals up to 255/256 of a
   unit). All three axes of a sensor are tested together, ranges
   from the coarsest down; the first that every reading fits is the range the
   values show.

8. **The rate check** (for Michael). The median logged IMU interval is
   compared with the nominal interval, one over the stated rate, of each of
   `ACCEL_ODR_HZ` and `GYRO_ODR_HZ`; a relative difference above 10 % is a
   rejection naming the stated rate and the logged one. The datasheet states
   no oscillator tolerance (its INTERNAL_FREQ_FINE register reports the
   effective ODR's deviation in 0.15 % steps); the reference recordings log
   at 13.2-13.3 Hz against a stated 12.5 Hz (5-7 % fast), and adjacent listed
   rates differ by a factor of two, so 10 % neither rejects a healthy unit
   nor mistakes one rate for another. The specification's "a few percent"
   would reject every recording on disk if read as 2-3 %. A recording whose
   two IMU rates differ cannot pass both checks, and none does.

9. **The datasheet unit is `src/fusion/sensornoise.h` / `.cpp`**, the one place
   the table exists in code: per range and rate, the noise density, the
   bandwidth (the fixed filter setting: the gyro's LPF2 cutoff of Table 18,
   no LPF1; the accelerometer at ODR / 2, Figure 17 note 1 and Table 65), the
   sensitivity (the quantization step and the lattice) and the sensitivity
   tolerance (the scale prior of phase 3). It is GTSAM-free. A configuration
   the table has no entry for (a value outside the lists, or not a number) is
   a rejection naming the key. Phase 2 settles two points (for Michael): the
   accelerometer's 1.6 Hz low-power rate is rejected, not tabulated (it exists
   only in low-power mode and the gyro has no such rate); and Table 2 has no
   sensitivity tolerance for the accelerometer, so G_So%'s 1 % serves both
   sensors. The bias priors stay `Tuning` fields at their
   values (the specification keeps them) and the documentation cites the
   table for each.

10. **"The gyro's LPF2 at its 12.5 Hz cutoff"** is read as the LPF2 cutoff of
    the 12.5 Hz rate, 4.2 Hz (Table 18), which is what firmware v2023.09.22
    runs; the filter is not configurable, so the bandwidth follows the stated
    rate through the same table.

11. **One algorithm string, bumped in phase 2.** The first phase that changes
    what `run()` returns changes `Fusion::Algorithm` to
    `batch-temperature-bias-v6`; phases 3 and 4 change results further under
    the same string, since no build between them is released. Phase 1 adds
    inputs and changes no number, so it keeps `v5`.

12. **Goldens are captured again at the end of phases 2, 3 and 4**, each
    with its history paragraph in `tests/README.md` section 11; the time axis
    of every success fixture stays byte-identical to the capture of
    2026-09-30.

13. **The fixtures state their configuration and lie on its lattice.**
    Phase 2 gives every golden and initializer fixture a stated configuration
    (a rate within decision 8's tolerance of its sampling: the 100 Hz
    fixtures state 104 Hz, the 25 Hz `stationary_spin` states 26 Hz) and
    rounds its readings onto that configuration's lattice; the golden
    fixtures' time axes do not change. Phase 2 settles two exceptions among
    the initializer fixtures, which are not goldens (for Michael):
    `sacc_anchor` and `drifting_bias` log at 10 Hz, which no listed rate is
    within 10 % of, so they are resampled at 12.5 Hz; and `rest_throughout`
    keeps its IMU noise at the datasheet level, rounded onto the lattice
    (Michael's decision), its initializer test re-measured. The optimizer's
    damping ceiling is raised to 1e12 and a pass that settles without moving
    at the ceiling is the solver failure `damping saturated` (Michael's
    decision; phase 2's decision 7). Two rejection fixtures are added, `reject_lattice` (readings on
    the +/-8 g lattice, +/-16 g stated) and `reject_rate` (a stated rate the
    logging disagrees with), for fourteen golden fixtures. Session fixtures
    store the configuration as attributes; the default path is exercised by
    a session without the keys whose recording is on the default's lattice and
    rate. Every existing rejection fixture keeps its reason: the new checks
    come after the existing ones.

14. **The scale state belongs to the full fit only**, as the temperature
    slope does; the initializer's prefix and segment fits stay stock. It is
    one graph variable, `S(0)`, a `gtsam::Vector6`: the accelerometer's
    factors x, y, z, then the gyro's, the factors themselves (not their
    departures), started at one, with a prior of mean one and the
    sensitivity tolerance as sigma. A fit without the state (the comparison of
    the scale test) is the same fit with the state off, as the constant bias
    model is the temperature model off.

15. **The covariance at a sample** (for Michael). The specification lists the
    marginal covariance of every fix state, its cross-covariance with the
    biases and scales, and the conditional covariance at every sample given
    the two fixes around it. Composing the last with the uncertainty of the
    fixes needs the joint covariance of the two fixes as well, so phase 4
    computes the joint covariance of each pair of adjacent fix states from the
    same factorization (it lies within the sparsity of the factor, so it costs
    what the marginals cost) and publishes, at a sample, the covariance of its
    state as a linear function of the two fixes, the biases and the scales,
    plus the conditional covariance. Phase 4's document states the formula.

16. **The covariance step's method and budget.** One factorization of the
    converged graph, linearized at the solution, without damping; no
    `Marginals::jointMarginalCovariance`. Phase 4 measures its time on
    `11-17-12` (8,860 fixes, 23,449 IMU samples, a 25 s fit) and keeps it
    within a stated fraction of the fit; a probe under `experiments/` before
    the document is written is allowed and recommended, as the previous plan
    did. A rank-deficient heading (`coarse_linear`, whose heading the data does
    not determine) is not a failed factorization: the heading accuracy is
    capped at 180 and the four channels are published (clauses 35, 60). Only
    a factorization that cannot be completed at all leaves them absent.
    Phase 4's probe settles the method (for Michael): one multifrontal QR
    elimination in time order with the globals last, then the Bayes tree's
    clique marginals root first (the covariance-form selected inverse was
    unstable on long chains); a 1000 rad heading prior on the first fix, in
    the linearized graph of this step only, keeps an undetermined heading
    finite and moves a determined one by under 5e-6 of itself; about 3.2 s on
    `11-17-12` (12 % of the fit), held to 20 %.

17. **The widening window** (for Michael). For each sample, the fixes whose
    time lies within 2.5 s of it, and at least the two around it; the
    window's factors are those fixes' position and velocity factors and the
    IMU factors between consecutive fixes of the window, the priors excluded;
    the degrees of freedom are the window's residual components less the
    dimension of its states, 6N + 9(N - 1) - 9N = 6N - 9 for N fixes, the
    redundancy that makes the factor one in expectation when every sigma is
    right. The window length is a documented constant of the kernel.

18. **The acceptance clauses are numbered here** (below), items 1001-1065,
    so that every phase adds map lines for its own items and none renumbers
    another's. Phase 1 writes appendix K and section 9.11 of
    `tests/README.md` from the list, extends the audit's allowed ranges to
    1001-1065, and makes the audit's completeness check cover the items of
    phase 1 only, as an explicit list; each later phase appends its items to
    that list; phase 5 replaces the list with the range. A phase that reads a
    clause otherwise than stated here restates it "(as settled)" in appendix
    K and the map and says why.

19. **Amended earlier items.** Each phase restates "(as amended)", in
    `tests/README.md`'s appendices and the map's comments, the earlier items
    its change contradicts. Found during discovery, not exhaustive: 221 (the
    twenty-second input; phase 1), 215, 216, 227, 245, 247 (the per-step
    slopes and their diagnostics; phase 2), the stopping-rule items of the
    fusion-improvements block that list the rules and the failure the fit
    cannot continue from (`damping saturated` joins `cost increased`;
    phase 2), 230, 337, 912 (seventeen channels,
    no uncertainty; phase 4), 509, 801, 842, 854, 862, 863 and
    `allEightFusionPlotsAreExplicitBacked` (eight plots; phase 5). Each
    documenter searches the appendices for the rest.

20. **The validation of the model on the reference recordings** is run in
    phase 5, when the model is final (phase 4 does not change a residual),
    with `fusion_runner` on the recordings of `tests/README.md` section 12.2,
    as manual steps; the numbers go into `docs/SENSOR_FUSION.md` section 8
    whatever they are. A reference recording that no longer converges under
    the datasheet model is reported with its numbers and is not answered by a
    changed constant (the specification's section 2).

Constraints the code imposes, which the specification does not state:

- `flysight_fusion` does not link `flysight_core`: the registration reaches
  `flysight_model`, the engine and the inline helpers only (decision 2).
- The audit's step-model rule: sensor covariances (`accelerometerCovariance`,
  `gyroscopeCovariance`, `integrationCovariance`) are written only in
  `imuintegration.cpp`, and there is one `integrateMeasurement` call. The
  noise unit computes sigmas; the integration loop alone sets them. Any new
  per-step quantity (the sampling term, the rotation remainder, the scale's
  Jacobians) is computed in or read from `preintegrateImu()`.
- Three kinds of cancellation boundary (`checkpoint(` counted 3 in
  `src/fusion`): the covariance step adds none.
- GTSAM headers only under `src/fusion/` and the listed tests; `fusion.h`,
  `fusionregistration.*`, `orientation.*` GTSAM-free; a new internal kernel
  header is added to the `fusion-tooling` pattern; the kernel includes nothing
  of the session or the engine and does not log.
- One algorithm literal in `src/`, in `fusion.h`; the audit's count rule
  spells it and changes with it.
- `tests/fusion/fusionfixtures.*` include nothing from `src/` (Qt Core and the
  standard library only), so a fixture's configuration is its own plain
  numbers.
- Exact mode holds every golden bit for bit on the capture compiler (64-bit
  MSVC 19.44, Release); portable mode needs a floor for every new number
  (accuracies in degrees take the degree floor; phase 4 says which take the
  heading floor).
- The kernel's checks are ordered: the first violation is the reported
  reason, and the rejection fixtures pin it.
- `docs/SENSOR_FUSION.md` may not contain "twenty-one" (the audit's
  description rule); with twenty-six inputs, phase 1 moves the banned word to
  "twenty-two".
- Tests run sequentially in `build-agent/`, Release; the fusion tests run
  below normal priority and time out under load.

## Interfaces between phases

### Phase 1 provides

- `src/sensorconfiguration.h` / `.cpp` in `flysight_model`, namespace
  `FlySight::SensorConfiguration`: the nine key names (`ACCEL_FS_G`,
  `GYRO_FS_DEG_S`, `ACCEL_ODR_HZ`, `GYRO_ODR_HZ`, `BARO_ODR_HZ`,
  `HUM_ODR_HZ`, `MAG_ODR_HZ` in `SENSOR.CSV`; `GNSS_MODEL`, `GNSS_RATE_HZ`
  in `TRACK.CSV`), their value forms and a validation the importer calls, the
  default of the four IMU keys as text (`16`, `2000`, `12.5`, `12.5`) and the
  firmware version it describes (`v2023.09.22`). The key literals appear in
  `src/` only there (audit group `sensor-configuration`).
- Four constant defaults, `builtin.default.ACCEL_FS_G`,
  `builtin.default.GYRO_FS_DEG_S`, `builtin.default.ACCEL_ODR_HZ`,
  `builtin.default.GYRO_ODR_HZ`, registered by
  `Fusion::registerFusionCalculations`.
- `Fusion::fitInputs()`: the eighteen measurements, the four origin
  attributes, then `ACCEL_FS_G`, `GYRO_FS_DEG_S`, `ACCEL_ODR_HZ`,
  `GYRO_ODR_HZ`, twenty-six inputs.
- In `src/fusion/fusion.h`: `struct ImuConfiguration { double accelFsG,
  gyroFsDegS, accelOdrHz, gyroOdrHz; }`, each a quiet NaN unless set (g,
  deg/s, Hz, Hz: the keys' values), and the member
  `Channels::imuConfiguration`. `channelsFrom()` fills it from the four
  attributes (a value that is not a number becomes NaN, as an origin index
  that is not a number becomes -1). The kernel receives it and, in phase 1,
  reads nothing of it.
- `fusion_runner --dump-inputs` lists the four attributes with the other
  inputs.
- The acceptance range 1001-1065 (appendix K, section 9.11, the map's head
  comment, the audit's ranges and completeness list); the audit group
  `sensor-configuration`.

### Phase 2 provides

- `src/fusion/sensornoise.h` / `.cpp`: for an `ImuConfiguration`, per sensor,
  the per-sample sigma (m/s^2 and rad/s), the integration density derived
  from it, the quantization step, the lattice step in effective units, and
  the sensitivity tolerance (a fraction); a rejection naming the key for a
  configuration without a table entry. Phase 2's document names the types
  and functions; phase 3 reads the sensitivity tolerance and phase 4 the
  accelerometer's per-sample sigma from it, nothing else.
- `Tuning` without `accDensity`, `gyroDensity`, `accStepSlope`,
  `gyroStepSlope`; `preintegrateImu()` taking the recording's noise, with the
  sampling term and the rotation remainder computed in its loop. Phase 2's
  document names the new parameter; phases 3 and 4 extend the same function.
- The rejection reasons of the lattice check and the rate check (phase 2's
  document states their text).
- Diagnostics: a top-level `configuration` object (the four values the fit
  ran under) and `model.noise` (per-sample sigmas, densities, steps) in place
  of `model.per_step`; phase 2's document names their inner keys.
- `Fusion::Algorithm` = `batch-temperature-bias-v6`.
- `FlySightTest::FusionFixture` with the fixture's stated configuration (four
  plain doubles), copied by `toChannels()` and stored as attributes by
  `tests/fusion/fusionsessions.cpp`; the fixtures `reject_lattice` and
  `reject_rate`; fourteen golden fixtures, captured.
- The audit group `noise-model` (the retired constants and `per_step` gone,
  the table in one unit).

### Phase 3 provides

- The graph variable `S(0)` (`gtsam::Vector6`: accelerometer x, y, z, then
  gyro x, y, z; the factors themselves, started at one) in the full fit,
  beside `B(0)` and `T(0)`, with its prior; the fitted value read with
  `fit.values.at<gtsam::Vector6>(S(0))` (or a `FitResult` member phase 3's
  document names). Phase 4 takes its covariance.
- `preintegrateImu()` integrating the readings divided by the scale, and
  whatever per-step Jacobian the scale's factor needs, from the one loop.
- The reconstruction's published acceleration `R (f / s - b_a) + g +
  (c_before + c_after) / 2`, `f / s` per axis.
- Diagnostics `model.scale`: `{"acc": [x, y, z], "gyro": [x, y, z]}`.
- The full fit's IMU factor kind stays `imu` (dimension 9); the scale prior
  is a new residual kind, `scale_prior`, after `slope_prior`.

### Phase 4 provides

- `Fusion::Result::headingAcc`, `tiltAcc`, `accHAcc`, `accDAcc` (deg, deg,
  m/s^2, m/s^2), aligned with `time`; `kFitOutputs` and
  `fitOutputChannels()` with them after `qw`: twenty-one measurements and
  `_FUSION_DIAGNOSTICS`, twenty-two outputs; the golden channels file and
  `fusion_runner --csv` with the twenty-one columns in that order.
- The unit `src/fusion/fitcovariance.h` / `.cpp` (the covariance step).
- Diagnostics: `model.scale.acc_sigma` and `model.scale.gyro_sigma`; an
  `accuracy` object (whether the covariance was computed and, if not, why;
  the widening window; whatever summaries phase 4 names); `limitations`
  without "no uncertainty is published".
- The audit group `accuracy-channels`.

### Phase 5 consumes

- The measurement names `Fusion/headingAcc`, `Fusion/tiltAcc`,
  `Fusion/accHAcc`, `Fusion/accDAcc`, declared outputs of
  `builtin.fusion.fit`, so that the plots over them are explicit-backed.

### The acceptance clauses (items 1001-1065, item = 1000 + clause)

The testable statements of the specification, one sentence each, with its
section in front and the phase that adds its evidence after. Its sections
1-4 have no clause; their principles are carried by the clauses that apply
them. Phase 1 copies this list into `tests/README.md` appendix K (without the
phase tags). A clause that a phase settles otherwise is restated "(as
settled)" there and in the map.

1. (5) The configuration keys, with actual values and the unit in the name:
   in `SENSOR.CSV` `ACCEL_FS_G` (2, 4, 8, 16), `GYRO_FS_DEG_S` (250, 500,
   1000, 2000), `ACCEL_ODR_HZ` and `GYRO_ODR_HZ` (12.5, 26, 52, 104, 208,
   416, 833, 1666, 3333, 6666, and 1.6 for the accelerometer), `BARO_ODR_HZ`,
   `HUM_ODR_HZ` and `MAG_ODR_HZ`; in `TRACK.CSV` `GNSS_MODEL` by name
   (portable, stationary, pedestrian, automotive, sea, airborne_1g,
   airborne_2g, airborne_4g) and `GNSS_RATE_HZ`; recorded with their value
   forms in `docs/DATA_SCHEMA.md` section 2 beside `SCHEMA_VER`. [1]
2. (5, 9) The importer stores them as header attributes, as it stores
   `SCHEMA_VER`, and they travel with the session as `SCHEMA_VER` does: kept
   as recorded, merged by the attribute conflict rule, written back by the
   exporter. [1]
3. (5) A malformed value is an import error naming the key, and nothing of
   the file is imported, as for a malformed `SCHEMA_VER`. [1]
4. (5) A recording that lacks a key takes the value of the firmware of the
   recordings on disk, v2023.09.22: +/-16 g, +/-2000 deg/s, 12.5 Hz for both
   sensors; no dynamic model and no rate; the default is written once in the
   documentation and once in the code, with the firmware version it
   describes. [1]
5. (5) The IMU's low-pass filters have no key: that firmware's fixed setting
   (the gyro's LPF2 at the cutoff of its 12.5 Hz rate and no LPF1, the
   accelerometer at its ODR bandwidth) is documented as part of the default,
   with the firmware version. [1]
6. (5, as settled) The dynamic model, the GNSS rate and the barometer's,
   humidity sensor's and magnetometer's rates are read and stored with the
   other attributes; nothing uses them, and they are not inputs of the
   fit. [1]
7. (5, 10) The configuration reaches the kernel as part of its channels, as
   the origin does: the fit declares the four IMU keys as inputs, as it
   declares the origin, the input adapter carries them, the kernel reads no
   preference or constant for them, and nothing above the registration
   changes. [1]
8. (5, as settled) Whether stated or defaulted, each IMU sensor's range is
   checked against the lattice of its readings (the datasheet's sensitivity
   at the range, in effective units, within the rounding of the file's
   decimals): the coarsest range whose lattice every reading fits must be the
   configured one; otherwise the fit rejects the recording with a reason that
   names the range stated and the range the values show, and does not
   guess. [2]
9. (10, as settled) The lattice check is a kernel validation rule, one of the
   rules that decide whether a recording can be fitted, and reads the
   readings as the kernel receives them, before any correction of its
   own. [2]
10. (5, as settled) A logged IMU interval that disagrees with a stated rate by
    more than the oscillator's tolerance (10 %) is a rejection that names
    both. [2]
11. (6, as settled) The per-sample noise of each sensor and axis is
    `sqrt(density^2 x bandwidth + step^2 / 12)`: the datasheet's density at
    the configured range, the datasheet's bandwidth for the configured rate
    and filter, and the quantization step at the range (the datasheet's
    sensitivity). [2]
12. (6) The documentation carries the table: the density per range for each
    sensor and the bandwidth per rate and filter, with the datasheet's table
    numbers. [2]
13. (6) The density the integration uses is the per-sample noise times the
    square root of the nominal sample interval at the configured rate, so that
    a step of the nominal length carries one sample's variance; `accDensity`
    and `gyroDensity` are gone, and no density in the model is derived
    otherwise. [2]
14. (6) The sampling term replaces the slopes: per step, the trapezoid rule's
    error on a piecewise-linear signal, `dt^3 / 12` times the second
    derivative that the change of the signal's slope between the step and its
    neighbours estimates, for velocity from the specific force and for the
    angle from the rate, added in quadrature per step, with no fitted
    coefficient. [2]
15. (6) The rotation remainder of the mid-step scheme, second order in the
    step's rotation, is derived and added the same way. [2]
16. (6) The documentation writes the derivations out, including what they
    assume (a smooth signal between samples, the samples exact). [2]
17. (6) The bias priors stay at 0.3 m/s^2, 0.03 rad/s and 0.010 deg/s per
    degC, and the documentation cites the datasheet's table for each. [2]
18. (6) A scale-factor state: per fit, one factor per axis for each sensor,
    starting at one, with the datasheet's sensitivity tolerance as a
    zero-mean prior on its departure from one. [3]
19. (6) The readings enter the integration divided by the scale, and the
    graph is re-preintegrated at the fitted scale as it is at the fitted
    bias, under the same settled test. [3]
20. (6) A recording without motion leaves the scale at its prior. [3]
21. (6) The fitted scale factors are reported in the diagnostics beside the
    biases. [3]
22. (6) The sigma of each fitted scale factor is reported beside it, from the
    covariance of section 7. [4]
23. (6) The 1.14688 schema correction of legacy recordings is not a scale
    factor and is untouched. [3]
24. (6) Validation, not tuning: the normalized residuals (position, velocity,
    IMU) of the reference recordings of `tests/README.md` section 12.2 and of
    the committed fixtures are reported in the documentation as measured under
    this model, with the sentence that a value far from one measures what the
    model does not yet describe; no constant of section 6 is changed to move
    them. [5]
25. (7, as settled) After convergence, from the converged graph and one
    factorization: the marginal covariance of every fix state, the joint
    covariance of each pair of adjacent fix states, and their
    cross-covariance with the biases and the scale factors, by solves of the
    factorized system and not by the library's joint marginals. [4]
26. (7, as settled) Through the reconstruction's pass, the conditional
    covariance of the state at every IMU sample given the two fixes around
    it, composed with the covariance of those fixes, the biases and the
    scales into the covariance of the state at the sample. [4]
27. (7) The covariance step runs inside the same cancellable job and is not a
    cancellation boundary. [4]
28. (7) The heading accuracy of a sample is the square root of the
    navigation-frame rotation covariance's element about the vertical, the
    tilt accuracy the square root of the sum of its two horizontal elements,
    both in degrees and capped at 180, which means undetermined. [4]
29. (7) The acceleration accuracy of a sample is the first-order propagation
    of the joint covariance of attitude, accelerometer bias and accelerometer
    scale through `a = R (f / s - b) + g`, plus the accelerometer's per-sample
    noise rotated; the horizontal accuracy is the standard deviation along the
    horizontal acceleration's direction, or the larger horizontal principal
    value where the horizontal acceleration is below that accuracy, and the
    vertical accuracy is the vertical element; both in m/s^2. [4]
30. (7) The documentation states the propagation in symbols and what it
    leaves out (gravity's own uncertainty, cross-axis sensitivity, the
    interpolation between nodes). [4]
31. (7, as settled) Widening: over the fixes within 2.5 s of each sample, the
    sum of the squared whitened residuals of the window's factors divided by
    the window's degrees of freedom is a factor; the sample's four accuracies
    are multiplied by its square root where it exceeds one and are unchanged
    where it does not. [4]
32. (7) The documentation says what the factor is (the a-posteriori variance
    factor of the window), that it assumes every sigma is off by the same
    ratio, and that it never tightens. [4]
33. (7) `Fusion/headingAcc`, `Fusion/tiltAcc`, `Fusion/accHAcc` and
    `Fusion/accDAcc` are outputs of the fit, aligned with `Fusion/_time`. [4]
34. (7) They are absent for a rejected or failed fit and for a successful fit
    whose factorization failed, which the diagnostics say; nothing else about
    that fit changes. [4]
35. (7) An undetermined heading does not fail the computation: the cap
    applies, and the acceleration accuracies use the horizontal magnitude's
    direction, which a heading error does not move. [4]
36. (2, 7) The documentation says in one sentence that the accuracy is one
    standard deviation from the covariance of the converged solution under the
    documented model, widened where the residuals exceed what the model
    allows. [4]
37. (8) The "Sensor fusion" category gains four plots after Roll: Heading
    accuracy and Tilt accuracy in degrees, Horizontal acceleration accuracy
    and Vertical acceleration accuracy in the acceleration unit, drawn in the
    deep colours of the GNSS accuracy plots. [5]
38. (8) The four plots are absent, like any unavailable value, where the fit
    did not compute them. [5]
39. (8) A logbook column over any of the four works as over any fusion
    value. [5]
40. (8) The diagnostics show the configuration the fit ran under. [2]
41. (8, 9) The first start after the change finds every stored fit stale at
    its recording's load and recomputes it when something switched on needs
    it, counted in the status bar as any computation. [2]
42. (9) The fit's outputs gain the four channels, and the record stores and
    restores them with the rest, in the record format as it is. [4]
43. (9) The algorithm string changes once, for the whole specification. [2]
44. (9, as settled) The golden fixtures are captured again with the existing
    tool at the end of each phase that changes numerical results; their time
    axes are unchanged and checked byte for byte; the last capture's
    comparison covers the new channels, the scale factors and the
    configuration in the diagnostics. [4]
45. (9) The fixtures state their configuration explicitly, so that the stated
    path (the kernel's and the sessions' fixtures) and the default path (a
    session without the keys) are both exercised. [2]
46. (10) The kernel derives its noise from the configuration in one unit, the
    only place the datasheet table exists in code. [2]
47. (10) The sampling term and the rotation remainder are computed where the
    steps are integrated, from the readings, with no constant of their
    own. [2]
48. (10) The scale factors are variables of the graph beside the biases. [3]
49. (10) GTSAM stays confined to the kernel, and the covariance step is a unit
    of its own, as the reconstruction is. [4]
50. (11) Test: a file with every key imports them; a malformed one is an
    import error naming the key; a file without them takes the default. [1]
51. (11) Test: a fixture whose readings sit on the +/-8 g lattice with a
    header stating +/-16 g is rejected naming both; the lattice check
    identifies the range of every committed fixture and of the reference
    recordings. [2]
52. (11) Test: the per-sample noise and the density follow the table for
    every configuration the fixtures state, bit for bit against the
    formula. [2]
53. (11) Test: a step with a constant signal has no sampling term; the
    sampling term of a step follows the derivation on a synthetic signal with
    a known second derivative; the rotation remainder follows its derivation
    on a constant turn. [2]
54. (11) Test: a synthetic recording with the accelerometer 2 % high on one
    axis recovers the factor within the prior's tolerance, and its position
    misfit falls against a fit without the state; a recording at rest leaves
    the scale at one within its prior. [3]
55. (11) Test: the normalized residuals of the committed fixtures are in the
    goldens; the reference recordings' are recorded in the
    documentation. [5]
56. (11) Test: on the committed fixtures a successful fit publishes finite,
    positive accuracies for every sample, and scaling every GNSS accuracy up
    never lowers them. [4]
57. (11) Test: the heading accuracy of the first node agrees with the heading
    check. [4]
58. (11) Test: the acceleration accuracy follows its propagation on synthetic
    inputs with known answers, and agrees with the library's joint marginals
    on a short fixture. [4]
59. (11) Test: the widening is one where the residuals are at the model, and
    grows where a factor's sigma is understated by a known ratio. [4]
60. (11) Test: the cap holds on a fixture whose heading is undetermined. [4]
61. (11, as settled) Test: on a build where the configuration is carried and
    defaulted and the noise model is the previous one (the end of phase 1),
    the existing channels and diagnostics of every fixture are bit-identical
    to the goldens. [1]
62. (11) Test: a stored fit from before the change is stale. [2]
63. (11) Test: the four plots are explicit-backed like the other fusion plots,
    and a column over one works. [5]
64. (11) Test: the audit's confinement rules hold, and the documents carry the
    table, the derivations and the validation. [5]
65. (12) `docs/` describe the change: `DATA_SCHEMA.md` (section 2, the keys;
    section 11, the algorithm string; section 12, the record),
    `SENSOR_FUSION.md` (section 3, the configuration inputs and the lattice
    check; section 4, the noise model with its table and derivations, the
    scale state, the covariance and its propagation, the widening, the outputs
    and diagnostics; section 8, the validation results and what is and is not
    validated), `CALCULATIONS.md` section 17, `COMPUTED_PLOTS.md` (the four
    plots); `tests/README.md` section 11 records the goldens captured again
    and why; the map gives the specification its range, with the amended
    items restated. [5]

Documentation goes with the phase that changes the behaviour it describes:
phase 1 `DATA_SCHEMA.md` section 2 and the inputs of `SENSOR_FUSION.md`
section 3 and `CALCULATIONS.md` section 17; phase 2 the noise model, the
checks, the rejections, the algorithm string and its first start; phase 3 the
scale state and section 5's limitation; phase 4 the covariance, the
propagation, the widening, the outputs, the record and the lifecycle; phase 5
the plots, the validation and whatever clause 65 still lacks. Phase 5 owns
clause 65 as the check that all of it is in place.

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
