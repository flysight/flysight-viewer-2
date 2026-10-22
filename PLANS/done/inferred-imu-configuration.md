# Inferred IMU configuration

Date: 2026-10-21.
Status: specification for planning. Not an implementation plan. Small enough
to be implemented by one agent in one or two phases.
Baseline: branch `store-requested-calculations` at `dbf5a78`; the committed
code is authoritative.
Related: `docs/DATA_SCHEMA.md` section 2 (the configuration keys, their
values and the default), `docs/SENSOR_FUSION.md` section 3 ("Configuration",
"The lattice", "The rate") and section 6 (the rejections that name a key),
`docs/CALCULATIONS.md` section 5 ("Defaults are calculations") and section
17 (the registration table, the fit's inputs), `src/sensorconfiguration.h`
(the vocabulary: keys, allowed values, the firmware default),
`src/calculations/attributecalculations.h` (`addConstantDefault`),
`src/fusion/fusionregistration.cpp` (the four constant defaults, the fit's
inputs), `src/fusion/sensornoise.h` (`rangeShownByReadings`, the datasheet
table), `src/fusion/fusionsamples.h` (`requireReadingsOnLattice`,
`requireStatedRates`), `src/fusion/fusionoutput.cpp` (the diagnostics'
`configuration` object), `tests/acceptance_map.txt`, `tests/README.md`.
`PLANS/done/noise-model-part-1.md` introduced the keys, the default and the
checks this specification amends.

## 1. Motivation

The fit reads the IMU's configuration from four header attributes of
`SENSOR.CSV` (`ACCEL_FS_G`, `GYRO_FS_DEG_S`, `ACCEL_ODR_HZ`, `GYRO_ODR_HZ`),
or, where a recording states none, from a constant default: the
configuration of firmware v2023.09.22, +/-16 g, +/-2000 deg/s, 12.5 Hz for
both sensors. Stated or defaulted, the configuration is checked against the
data (the readings must lie on the configured range's lattice, the logged
interval must be within 10 % of the configured rate's), and a mismatch
rejects the recording.

No firmware writes the keys yet, so every recording reads the default, and
a recording from a firmware that logs the IMU at another rate is rejected
with `ACCEL_ODR_HZ states 12.5 Hz but the IMU is logged at 100.0 Hz; sensor
fusion unavailable`. Firmware v2024.02.07 logs at 104 Hz; a user's
recording from it cannot be fitted at all, although the data say plainly
what the configuration was. The message also misleads: the file states
nothing.

The remedy is to infer an absent key from the data, choosing among the
values the key allows, and to keep the check as the guard. The constant
default remains only for data that cannot decide.

## 2. Principles

- **A stated key is the fact; an absent key is inferred.** A recorded
  `$VAR` wins, as every stored attribute does, and is checked as today. An
  absent key is derived from the recording, as the ground elevation and
  the course reference are (`CALCULATIONS.md` section 5: a default is
  derived from the data where possible, constant otherwise).
- **Inference chooses an allowed value.** Each key has a list of values
  the part supports (`DATA_SCHEMA.md` section 2). Inference never produces
  a value outside the list: it chooses the allowed value nearest to what
  the data show, and the existing check then judges that choice against
  the data exactly as it judges a stated value. The fit still does not
  guess: a recording whose data fit no allowed value is rejected, naming
  what the data show.
- **One rule for stated and inferred.** The lattice check and the rate
  check are unchanged and apply to both. The inference and the check read
  the same readings and the same interval, through the same functions, so
  that an inferred range always passes the lattice check and an inferred
  rate fails it only when the logged rate is far from every allowed rate.
- **The default is written once.** The firmware default stays where it is,
  as the fallback of the inference; nothing else spells it. The filters
  are not configurable and stay as documented; they hold for every
  firmware to date.
- **The kernel reads no session.** The configuration reaches the kernel as
  numbers in its channels, as today, together with the fact of whether
  each value was stated or inferred, so that the kernel can word its
  rejection and record its diagnostics without reaching outside its
  inputs.

## 3. The inference

Four on-demand calculations replace the four constant defaults, under the
same ids (`builtin.default.ACCEL_FS_G`, `builtin.default.GYRO_FS_DEG_S`,
`builtin.default.ACCEL_ODR_HZ`, `builtin.default.GYRO_ODR_HZ`), each with
the attribute as its one output, the text of an allowed value, the type the
importer stores, so that a stated value and an inferred value of the same
text read the same.

| Key | Inputs | The value |
| --- | --- | --- |
| `ACCEL_FS_G` | `IMU/ax`, `IMU/ay`, `IMU/az` | the coarsest allowed range whose lattice all but one in a thousand of the readings of the three axes fit, as the lattice check finds it (`SENSOR_FUSION.md` section 3): the range the readings show |
| `GYRO_FS_DEG_S` | `IMU/wx`, `IMU/wy`, `IMU/wz` | the same rule for the gyro |
| `ACCEL_ODR_HZ` | `IMU/_time` | the allowed rate nearest to one over the recording's median logged IMU interval, the interval the rate check uses (nearest by rate, not by interval: 70 Hz is nearer 52 than 104) |
| `GYRO_ODR_HZ` | `IMU/_time` | the same rule over the gyro's list |

- The readings are the effective values, after the conversion layer and the
  legacy gyro correction, as the kernel receives them; the interval is the
  median over the whole recording, as the rate check measures it. The
  inference uses the kernel's own functions for both (one authority), and
  the fusion registration stays free of GTSAM and Eigen as it is today.
- **Fallback.** Where the data cannot decide, the calculation yields the
  firmware default: readings on no allowed range's lattice; an IMU time
  axis without a nominal interval (fewer than three samples, the continuity
  authority's rule); a recording without the input channel. The check then
  rejects or passes as it would for a stated default, naming what the data
  show. The calculation is never unavailable because of what the data say:
  a recording with IMU data always has a configuration, and one without
  IMU data has a missing input as today.
- **Nearest, not exact.** The firmware's oscillator runs a few percent
  fast (the recordings on disk log at 13.2-13.3 Hz against 12.5 Hz), and
  timestamps are logged to the millisecond, so a 104 Hz recording shows a
  median interval of 10 ms, 100 Hz. The listed rates are a factor of two
  apart, so "nearest" is unambiguous; 104 is chosen, and the 10 % check
  passes at 4 %. A recording logged at 70 Hz infers 52, the nearest, and
  the check rejects it at 25 %, naming both: the data fit no allowed rate.
- **Equal rates, in practice.** The sensor file logs one `$IMU` row per
  sample, so a recording has one IMU interval and the two inferred rates
  are always the same allowed value, and the two inferred ranges are each
  their own sensor's. A firmware that configures the two sensors at
  different rates and still logs one row would state two keys that the
  single logged interval cannot both satisfy; that is a question for the
  firmware's format, noted here (`SENSOR_FUSION.md` section 3) and not
  resolved: the check keeps its rule, `ACCEL_ODR_HZ` judged before
  `GYRO_ODR_HZ`.
- **On disk.** Every recording on disk (firmware v2023.09.22) infers
  exactly the firmware default: the readings lie on the +/-16 g and +/-2000
  deg/s lattices and the 13.2-13.3 Hz interval is nearest 12.5. Their fits
  are bit for bit what they are today; only the algorithm string moves.

## 4. The fit

- **Inputs.** The fit's twenty-six inputs are unchanged by name; the four
  configuration attributes resolve to the stated value or, absent that, to
  the inference above. With them the fit learns, for each key, whether the
  value was stated by the recording or inferred: the engine already knows
  (its resolution's first step is a stored attribute), and it tells a
  calculation, for a declared attribute input, whether the value came
  from the session's stored attribute or from a candidate, as a query of
  the evaluation context beside the value. The fit carries that fact
  beside the numbers into the kernel's configuration. It is not a fifth
  attribute, it is not written to any file, and the kernel never reads a
  session.
- **The checks.** Unchanged in rule and order: the datasheet entry, the
  lattice (accelerometer before gyro), the rate (`ACCEL_ODR_HZ` before
  `GYRO_ODR_HZ`), after every other check. Their messages say how the
  value came about: `ACCEL_ODR_HZ states 12.5 Hz but the IMU is logged at
  100.0 Hz; sensor fusion unavailable` for a stated key, and `ACCEL_ODR_HZ
  inferred as 52 Hz but the IMU is logged at 70.0 Hz; sensor fusion
  unavailable` for an inferred one; `ACCEL_FS_G states +/-16 g but the
  accelerometer readings lie on the +/-8 g lattice` and `ACCEL_FS_G
  defaulted to +/-16 g but the accelerometer readings lie on no range's
  lattice` likewise (an inferred range is by construction the range the
  readings show, so an inferred range fails the lattice check only through
  the fallback, which the message calls defaulted). The `No datasheet
  entry for ...` rejection is unchanged.
- **The diagnostics.** The `configuration` object of the diagnostics
  records, beside each of the four values, whether it was stated or
  inferred, so that a details view and a stored record say what the fit
  ran under and why. (A defaulted range never reaches a success: the
  lattice check rejects it.)
- **The algorithm string.** `Fusion::Algorithm` becomes
  `batch-temperature-bias-v12`: what the four inputs resolve to for every
  recording on disk changes in mechanism and, for a recording from another
  firmware, in value, so a record written under `v11` is stale at load and
  computed again once, as a result is needed, like any stale record. A
  stored rejection of a 104 Hz recording is therefore retried and, with
  the inference, succeeds. The fit's arithmetic, its outputs, the stored
  record's shape and the goldens' channels are unchanged.

## 5. What the documentation says

- `docs/DATA_SCHEMA.md` section 2: an absent key is inferred from the data
  among the key's allowed values, the firmware default being the fallback
  of the inference; the default's description says so.
- `docs/SENSOR_FUSION.md` section 3: "Configuration" says stated or
  inferred, the inference's rules per key, the fallback, the equal-rates
  note; "The lattice" and "The rate" say the inference reads what the
  checks read. Section 6: the reworded rejections, stated, inferred and
  defaulted, with the 104 Hz recording as the realistic case that now
  fits and the 70 Hz one as the case that is rejected. Section 7 "Stored
  results": `v12` and why. The diagnostics' `configuration` object.
- `docs/CALCULATIONS.md` section 5: the four IMU configuration attributes
  move from the constant defaults to the defaults derived from the data;
  section 17: the registration table's four rows gain their inputs, and
  the text that calls them constant changes. The engine's section that
  describes what a calculation may ask of its evaluation context gains
  the provenance query for an attribute input.
- `src/sensorconfiguration.h` and `src/fusion/fusionregistration.h`: the
  contracts.
- `tests/README.md`: the rows of the tests that change, the manual step,
  the appendix and section of the acceptance items.

## 6. Tests

- **The inference** (`tst_fusion_derived` or `tst_fusion_session`
  pattern): on a synthetic recording without keys logged at 104 Hz with
  readings on the +/-8 g and +/-500 deg/s lattices, the four attributes
  read `104`, `104`, `8` and `500`; on one logged at 13.3 Hz on the
  default lattices they read the firmware default's text, bit for bit the
  constant default's; on one whose readings fit no lattice, the range
  reads the default; on one with a stated key, the stated text, whatever
  the data; a recording without IMU data leaves the attribute unavailable
  as a missing input. Reading an attribute never runs the fit.
- **The fit** (`tst_fusion_kernel` or `tst_fusion_golden` pattern): a
  104 Hz recording without keys fits, its diagnostics' `configuration`
  recording 104 Hz inferred; a 70 Hz recording without keys is rejected
  with the `inferred as 52 Hz` reason; a recording stating `ACCEL_ODR_HZ`
  12.5 and logged at 104 Hz is rejected with the `states` reason as today;
  a recording on no lattice is rejected with the `defaulted` reason. The
  fourteen fixtures' goldens are unchanged except the algorithm string
  and the configuration's new provenance entries; the existing channels
  are bit for bit.
- **The record** (`tst_fusion_store` pattern): a record under `v11` is
  stale at load and the fit runs again once; a stored rejection under
  `v11` likewise.
- **The audit**: the rules that pin the algorithm string say `v12`; the
  rule that the default is written once still holds with the fallback.
- **Manual**, one step in `tests/README.md` section 12: a `TRACK.CSV` and
  `SENSOR.CSV` pair from firmware v2024.02.07 (logged at 104 Hz, no keys)
  loaded and fitted: the fit succeeds, the details show the inferred
  configuration, and the status bar shows the stale `v11` rejection
  computed again once after the update.
- The acceptance items in the next free hundred of `tests/acceptance_map.txt`,
  with their appendix and section in `tests/README.md`; the items of
  `PLANS/done/noise-model-part-1.md` whose statements say the default is
  constant or that the fit reads the default where a key is absent are
  restated "(as amended)".

## Commit Policy

Michael has authorized commits for this plan on the existing working branch.
Nothing is ever pushed, and nothing is ever committed on `master` or
`fusion-improvements`. One commit per accepted phase, subject
`Inferred IMU configuration: <phase>`, with the session's attribution line,
staged by explicit path; `PLANS/` is never staged.
