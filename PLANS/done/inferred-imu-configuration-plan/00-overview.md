# Inferred IMU configuration: implementation plan overview

Plan written 2026-10-21 from `PLANS/inferred-imu-configuration.md` against
branch `store-requested-calculations` at `dbf5a78`. Three phases; the
specification is the authority wherever a phase document and it disagree.

## Feature specification

## Inferred IMU configuration

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

### 1. Motivation

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

### 2. Principles

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

### 3. The inference

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

### 4. The fit

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

### 5. What the documentation says

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

### 6. Tests

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

### Commit Policy

Michael has authorized commits for this plan on the existing working branch.
Nothing is ever pushed, and nothing is ever committed on `master` or
`fusion-improvements`. One commit per accepted phase, subject
`Inferred IMU configuration: <phase>`, with the session's attribution line,
staged by explicit path; `PLANS/` is never staged.

## Phases

The specification draws three boundaries: what the engine tells a
calculation about an attribute input (section 4, first bullet, the engine's
half), what the kernel does with the fact (section 4: the checks, the
diagnostics, the algorithm string), and the inference itself in the
registration, with the fit's hand-over of the fact (sections 3 and 4, first
bullet, the fit's half). The plan cuts along them.

| # | Name | Purpose | Depends on |
| --- | --- | --- | --- |
| 1 | The engine's provenance query | `EvaluationContext` tells a calculation, for a declared attribute input, whether the value is the session's stored attribute or came from a candidate; served on the synchronous and the prepared paths alike; an undeclared query is an undeclared read; the engine tests; `docs/CALCULATIONS.md` section 3; the acceptance range opened at 1801-1802 (the audit group `inferred-configuration`, the map's header, section 9.19 and appendix S of `tests/README.md`) | nothing |
| 2 | The kernel words the provenance | `Fusion::ImuConfiguration` carries, beside each value, whether it was stated or inferred (`Stated` by default); the lattice and rate checks, unchanged in rule and order, word their rejections `states`, `inferred as` and `defaulted to`; the diagnostics' `configuration` object records the provenance; `Fusion::Algorithm` becomes `batch-temperature-bias-v12`; the fourteen goldens re-captured (the string and the new entries, every channel bit for bit); the kernel, golden and store tests (a `v11` record and a `v11` rejection stale at load); the audit's `v12` rules; the kernel's documents; items 1803-1809 | nothing in code; implemented after 1 (shared files) |
| 3 | The inference | The four calculations `builtin.default.<KEY>` become on-demand inferences from `IMU/ax..az`, `IMU/wx..wz` and `IMU/_time` through the kernel's own functions, the firmware default their fallback; the vocabulary exposes each key's allowed values; the fit and the runner carry the provenance into the kernel through the context's query; the session, store and vocabulary tests; the remaining documents and contracts; the restated items; the manual step M60; items 1810-1819 | 1 and 2 |

Phases 1 and 2 touch disjoint code (`src/engine/` and its tests; `src/fusion/`
and its tests), but both edit `tests/acceptance_map.txt`, `tests/README.md`,
`docs/CALCULATIONS.md` and `tests/audit/cleanup_audit.cmake`, so they are
implemented in order, 1 then 2; their documents were written in parallel.
Phase 3 consumes phase 1's query and phase 2's members, strings and
diagnostics keys, and runs last.

```text
1 The engine's provenance query  --+
                                   +--> 3 The inference
2 The kernel words the provenance--+
```

## Key patterns and references

### The engine (phase 1)

- `src/engine/evaluationcontext.h`, `src/engine/evaluationcontext.cpp`: the
  context's contract (the only window onto session state; what is
  deliberately absent), `InputValue` (value, samples, unit), `declared()`
  and the undeclared-read mechanism, `isDeclared()` as the one silent query.
- `src/engine/calculationengine.cpp`: `resolve()` for an attribute name (a
  stored attribute wins, `entry.provider = Provider::Stored`; the absence of
  a stored value is a dependency too); `gatherInputs()` (where an
  `InputValue` is filled from a `ResolutionEntry` in declared order);
  `gatherAsRoot()`, `prepare()` and the restore path, which all gather
  through `gatherInputs()`; `readLeafInput()` (preferences and source
  inputs, which have no provenance).
- `src/engine/calculationengine.h`: `ResolutionEntry`, `Provider`, `Layer`.
- `src/engine/preparedcalculation.h`, `.cpp`: the captured context of the
  asynchronous path, read on another thread.
- `src/engine/calctypes.h`: `CalcInput` and its kinds.
- `tests/tst_calcengine.cpp`: `storedWins`, `removingStoredFallsBackToCalc`,
  `storedInvalidValueStillWins`, `constantCalculationIsADefault`,
  `undeclaredReadDetected`, `isDeclaredIsSilent`: the patterns for a test of
  the new query on the synchronous path.
- `tests/tst_calcengine_async.cpp`: `asyncMatchesSync` and the undeclared
  read on the prepared path (the `sneaky` calculation): the pattern for the
  same query through `prepare()` / compute / publish.
- `tests/support/fakesessionstate.h`: the fake state the engine tests
  store attributes in.
- `tests/tst_builtins_engine.cpp`, `noUndeclaredReads`: every built-in
  reads only what it declares; unchanged, but the reason an undeclared
  query must count as a read.
- `docs/CALCULATIONS.md` section 3 "Declared inputs" (reads go only through
  the `EvaluationContext`): where the provenance query is documented;
  section 12 names `EvaluationContext::progress()` as the one non-input of
  the context, the style to follow.
- `tests/README.md`: the rows of `tst_calcengine` and `tst_calcengine_async`
  in section 1's table, section 9's preamble (eighteen specifications), the
  head of section 9.18 and appendix R as the pattern for 9.19 and appendix
  S.
- `tests/acceptance_map.txt`: the header (eighteen specifications, eighteen
  ranges; the amendments listed per range) and the 1701 block as the
  pattern of an item's lines.
- `tests/audit/cleanup_audit.cmake`: the header comment's list of groups
  (ending with the fused position and speed accuracy), the group
  `accuracy-channels` as the pattern of a group, the traceability block
  (the comment that lists the ranges, the range check that names them all,
  and the completeness loop per range, `foreach(item RANGE 1701 1717)`).

### The kernel (phase 2)

- `src/fusion/fusion.h`: `ImuConfiguration` (four doubles, NaN until set),
  `Channels` (the configuration beside the origin), `Result` (the comment
  that describes the diagnostics, `configuration` among them), `Algorithm`
  and the rule for changing it.
- `src/fusion/fusionsamples.h`, `src/fusion/fusionsamples.cpp`:
  `requireReadingsOnLattice()` and `requireStatedRates()`, their message
  formats and `kRateTolerance`; the header includes GTSAM and is internal
  to the kernel.
- `src/fusion/sensornoise.h`, `src/fusion/sensornoise.cpp`:
  `rangeShownByReadings()` (the coarsest lattice all but one in a thousand
  of the readings fit, NaN for none), `imuNoise()` and the `No datasheet
  entry` rejection, the tables of ranges and rates; solver-free by audit.
- `src/fusion/fusion.cpp`: `planFit()`, the order of the checks after every
  other check (datasheet entry, lattice, rate), the median IMU interval
  from `SampleContinuity::nominalInterval()` on the epoch-relative axis;
  `withoutChannels()` and the failure diagnostics.
- `src/fusion/fusionoutput.cpp`: `configurationObject()` (the four values),
  the success diagnostics that place it, `failureDiagnostics()` (algorithm
  and failure only).
- `tests/tst_fusion_kernel.cpp`: `latticeCheckFindsTheCoarsestRange` (the
  four `states +/-` messages), `latticeCheckIdentifiesEveryFixture`,
  `rateCheckToleratesTenPercent` (the `states 12.5 Hz` and `GYRO_ODR_HZ
  states 52 Hz` messages), `configurationChecksComeAfterTheOthers`,
  `configurationWithoutEntryIsRejected`, `diagnosticsReportTheNoiseModel`,
  `failureDiagnosticsShape`; three `batch-temperature-bias-v11` literals;
  `fixtureNoise()` builds an `ImuConfiguration`.
- `tests/fusion/fusiongolden.cpp` (the fixture-to-`Channels` assembly that
  copies the four configuration values), `tests/fusion/fusiongolden.h` (the
  comparison modes), `tests/tst_fusion_golden.cpp`
  (`successFixturesMatchGolden`, `rejectionFixturesMatchGolden`; the comment
  "The stated configuration: copies, so ==").
- `tests/fusion_golden_capture.cpp`, `tests/data/fusion/*.json` (fourteen
  goldens and `capture.json`), `tests/README.md` section 11 (the fixture
  table, "The capture tool", "Re-capture procedure", the capture notes per
  version).
- `tests/tst_fusion_store.cpp`: `codeStampChangeDropsRecordOnLoad_data` /
  `codeStampChangeDropsRecordOnLoad` (stamps `v5`),
  `recordUnderPreviousAlgorithmIsComputedAgainOnce` (a `v9` record stale at
  load, the fit run again once), `restoredRejectionShowsBadge` (a stored
  rejection restored); the `v11` literal in its stored-record text.
- `tests/tst_fusion_jobs.cpp` (two `v11` stamps), `tests/tst_fusion_session.cpp`
  (`resultVersion` asserted `v11` twice): the literal moves to `v12` here;
  nothing else of these files changes in phase 2.
- `tests/audit/cleanup_audit.cmake`: every `batch-temperature-bias-v11`
  (the header comment, the retired-strings comment, "one authority: the
  fusion algorithm string", "the goldens carry the current algorithm
  string" at fourteen, the three documents' counts), the group
  `gnss-holes`' document rules as the pattern for a document rule, the
  traceability block (phase 1 opened the range; phase 2 extends it).
- `docs/SENSOR_FUSION.md`: section 4 (the diagnostics' top-level keys,
  `configuration` under "identity and audit"), section 6 (the three
  configuration rejections with their texts), section 7 "Stored results"
  (the version history), section 8 (the sentence that names `v10` and
  `v11`); `docs/DATA_SCHEMA.md` (two `v11` mentions, sections 11 and 12);
  `docs/CALCULATIONS.md` section 17 "Stored results" (the version history,
  one `v11`).

### The inference (phase 3)

- `src/fusion/fusionregistration.h`: the contract (the four constant
  defaults, `fitInputs()`, `channelsFrom()` and its two readers,
  `registerFusionCalculations()`).
- `src/fusion/fusionregistration.cpp`: `kConfigurationInputs` (one table for
  the declaration, the hand-over and the defaults), `fitInputs()`,
  `channelsFrom()` (the public assembly and the context adapter),
  `computeFit()`, `registerConfigurationDefaults()`, the on-demand pattern
  (`registerHorizontalSpeed()`, `registerElevation()`: a descriptor with
  inputs, outputs and a compute that returns `unavailable()` on bad input),
  `registerFusionCalculations()` (the vocabulary before its reader).
- `src/calculations/attributecalculations.h` (`addConstantDefault`, the
  contract that every constant default is registered through it),
  `src/calculations/attributecalculations.cpp` (`builtin.attr.groundElev`: a
  default derived from the data, with its fallbacks).
- `src/sensorconfiguration.h`, `src/sensorconfiguration.cpp`: the vocabulary
  (`listedValues()` is a private helper today; `defaultValue()`; the lists
  and their spellings, the accelerometer's `1.6`).
- `src/samplecontinuity.h`: `nominalInterval()` (NaN with fewer than three
  samples or an axis not finite and increasing), `TimeAxis` from a
  `QVector<double>`.
- `src/fusion/sensornoise.h`: `Detail::rangeShownByReadings()`, which the
  registration may include (Qt Core and std only).
- `src/CMakeLists.txt`: `flysight_model` (the vocabulary and the continuity
  authority), `flysight_fusion` (the registration beside the kernel, GTSAM
  private).
- `tests/fusion_runner.cpp`: the one assembly through `Fusion::channelsFrom`
  with `session.getMeasurement` / `session.getAttribute` readers;
  `inputDump()` for `--dump-inputs`.
- `tests/fusion/fusionsessions.h`, `tests/fusion/fusionsessions.cpp`:
  `sessionFromFixture()` (stores the fixture's configuration as text),
  `statedConfiguration()` / `addConfiguration()`, `naturalSession()` (12.5
  Hz, no keys, readings on the default lattices), `syntheticFitSession()`
  (engine-level tests without the solver), `goldenDifference()`.
- `tests/fusion/fusionfixtures.cpp`: `stateConfiguration()` and the fixtures
  that state 104 Hz (`coarse_linear`, `coarse_maneuver`, `stationary_spin`,
  `bridged_hole`), on the +/-16 g and +/-2000 deg/s lattices.
- `tests/tst_fusion_session.cpp`: `registrationShape` (the seventeen ids in
  order), `configurationDefaults` (asserts no inputs today; items 1004,
  1050), `configurationReachesTheKernel` (a local `channelsFrom` assembly,
  the stated path, the `states 12.5 Hz` rejection through the engine),
  `naturalSessionEndToEnd` (the default path), `readsNeverRunTheFit`.
- `tests/tst_fusion_derived.cpp`: `derivedRegistrationShape`,
  `derivedValuesWaitOnTheFit`, `syntheticOutputsAreServedWithoutAFit`: the
  pattern for reading a registered calculation's output without the fit.
- `tests/tst_fusion_store.cpp`: `restoredRejectionShowsBadge`,
  `recordUnderPreviousAlgorithmIsComputedAgainOnce`: the pattern for a stored
  `v11` rejection retried at load and succeeding.
- `tests/tst_fusion_runner.cpp`: the runner driven as a child process, if a
  runner case is added.
- `tests/tst_sensor_configuration.cpp`: the vocabulary's tests
  (`keysAndValueForms`, `defaultIsTheFirmwareConfiguration`), where a test
  of the exposed list belongs.
- `tests/audit/cleanup_audit.cmake`: group `sensor-configuration` (keys
  spelled in the vocabulary alone; the firmware version once), group
  `constant-defaults` (a compute that ignores its context lives in the
  helper alone), group `solver-confinement` ("public and registration files
  are GTSAM-free"), "the tools see the public header or the trace seam
  only", group `sample-continuity` ("the readers consult the authority":
  the include counted over a file list, ten today).
- `docs/DATA_SCHEMA.md` section 2 "Sensor configuration keys" and "The
  default"; `docs/SENSOR_FUSION.md` section 3 ("Configuration", "The
  lattice", "The rate") and section 6 (the realistic case);
  `docs/CALCULATIONS.md` section 5 ("Defaults are calculations", where the
  four are listed among the constants) and section 17 (the registration
  table's four rows, "The four constant defaults above carry the default's
  text", "The four configuration attributes are never missing", the
  record's resolutions naming "the configuration's constant defaults").
- `tests/README.md`: section 12's preamble ("Sixteen scripts"), 12.16 and
  M59 as the pattern for 12.17 and M60, appendix K and matrix 9.11 (items
  1004, 1008, 1010, 1050), the rows of `tst_fusion_session`,
  `tst_fusion_derived`, `tst_fusion_store`, `tst_fusion_runner`,
  `tst_sensor_configuration`.
- `tests/acceptance_map.txt`: items 1004, 1008, 1010 and 1050, and the
  header's amendment lists per range.
- `PLANS/done/noise-model-part-1.md`: the specification whose items are
  restated.

## Decisions and constraints

- **Three phases, one commit each.** The specification says one or two; the
  engine's query, the kernel's wording and the registration's inference are
  three components with three owners of files, and each leaves the tree
  green on its own. Phases 1 and 2 are independent in code but share the
  map, the guide, `docs/CALCULATIONS.md` and the audit file, so they are
  implemented in order.
- **The provenance query is one boolean.** Phase 1 adds
  `bool EvaluationContext::attributeIsStored(const QString &key) const`:
  true when the declared attribute input `key` was served by the session's
  stored attribute (resolution step 1), false when a candidate provided it.
  It is a read of a declared input: for an undeclared key it returns false
  and records an undeclared read, exactly as `attribute()` does, so a
  calculation cannot learn about an input it did not declare. The value
  travels in `InputValue` beside the attribute's value, filled in
  `gatherInputs()` from the resolution's provider, so the synchronous,
  prepared and restore paths cannot differ. It is not exposed to Python
  plugins: nothing asks for it there.
- **The kernel carries two states per key, `Stated` by default.** Phase 2
  adds to `fusion.h` `enum class ConfigurationSource { Stated, Inferred }`
  and four members of `ImuConfiguration`, `accelFsGSource`,
  `gyroFsDegSSource`, `accelOdrHzSource`, `gyroOdrHzSource`, each
  `Stated` unless set. The default keeps every hand-built `Channels`, every
  fixture (which states its configuration, item 1045), the capture tool and
  the goldens' texts as they are; only the registration (phase 3) sets
  `Inferred`. There is no third state: "defaulted" is a wording, below.
- **The wording rules.** A stated key keeps today's texts. An inferred rate
  that fails reads `ACCEL_ODR_HZ inferred as 52 Hz but the IMU is logged at
  70.0 Hz; sensor fusion unavailable`. An inferred range that fails reads
  `ACCEL_FS_G defaulted to +/-16 g but the accelerometer readings lie on no
  range's lattice; sensor fusion unavailable`: by construction (the same
  readings through the same function) an inferred range fails only through
  the fallback, so every failing inferred range is worded `defaulted to`,
  naming what the data show; a hand-built `Channels` with an inferred range
  and readings on another lattice gets the same word, and no third wording
  exists for a case the registration cannot produce. The `No datasheet
  entry` text is unchanged. `requireStatedRates` may be renamed by phase 2's
  implementer now that it judges stated and inferred rates; nothing outside
  the kernel, `tst_fusion_kernel` and the test guide's row spells it.
- **The diagnostics' provenance entries.** The `configuration` object gains,
  beside each value, a text entry `accel_fs_g_provenance`,
  `gyro_fs_deg_s_provenance`, `accel_odr_hz_provenance` and
  `gyro_odr_hz_provenance`, each `stated` or `inferred`. Not `*_source`: the
  audit's "alternate column names" rule refuses any literal ending in
  `_source"` (a column-naming convention), and excluding the kernel, a test
  and the goldens from it to fit a name is the wrong trade. Failure
  diagnostics carry no `configuration` today and still do not: the reason's
  wording says how the value came about.
- **`v12` in phase 2.** The new entries change what `run()` returns for the
  same channels, which the rule in `fusion.h` says must move the string, so
  phase 2 moves it, re-captures the fourteen goldens (every channel bit for
  bit; only the string and the four entries differ), updates every `v11`
  literal in the tests and the audit, and adds the `v11` rows of the store
  tests (a `v11` record and a `v11` rejection stale at load, the fit run
  again once). The specification's reason for `v12`, the inference, arrives
  in phase 3: phase 2 writes section 7's sentence for the provenance in the
  wording and the diagnostics, and phase 3 completes it with the inference
  and adds the stored-rejection case that now succeeds. Phase 3 re-captures
  nothing: the fixtures state their configuration, so the kernel sees
  `Stated` and the goldens stand.
- **The rate's inference reads the continuity authority.** The median
  logged interval is `SampleContinuity::nominalInterval()` of `IMU/_time` as
  the engine serves it (UTC seconds); the kernel takes the same function on
  the epoch-relative axis, whose intervals are the same doubles (each
  subtraction of two nearby doubles is exact), so the inference and the
  check read one interval. The authority has no nominal interval below
  three samples (NaN), so the fallback applies there, as the
  specification's fallback bullet says (amended 2026-10-21 to the
  authority's rule); a recording with two samples infers the default, which
  the kernel never reaches (it rejects fewer than three).
- **Nearest by rate, not by interval.** The specification's table (amended
  2026-10-21) says the allowed rate nearest to one over the median
  interval; its "Nearest, not exact" bullet, its section 6 test and the
  rejection text it quotes say a 70 Hz recording infers 52. By interval,
  1/70 would be nearer 1/104 (4.67 ms away) than 1/52 (4.95 ms; the
  boundary is 69.3 Hz), which is why the table's earlier phrase was
  amended. For a real recording (a listed rate a few percent fast) the two
  rules never differ. Settled: the implementer infers by rate.
- **The allowed values come from the vocabulary.** Phase 3 exposes each
  key's list from `SensorConfiguration` (the private `listedValues()` made
  part of the contract; name and signature the documenter's), and the
  inference yields the list's own spelling: the entry whose number equals
  the range the readings show, and the entry whose value is nearest one
  over the median interval; so a stated and an inferred value of the
  same text are the same `QString`. The accelerometer's list holds `1.6`, so
  a recording logged near 1.6 Hz infers `1.6` for the accelerometer, and the
  datasheet check rejects it as it rejects a stated `1.6`; the gyro's list
  does not, so its nearest is `12.5`. That is the specification's rule
  applied, and the documenter says so.
- **The range's inference reads the kernel's function.** The registration
  includes `fusion/sensornoise.h` for `Detail::rangeShownByReadings()`: the
  header is Qt Core and std only (audit, "the datasheet unit is
  solver-free"), and the registration stays free of GTSAM and Eigen (audit,
  "public and registration files are GTSAM-free"); it never includes
  `fusionsamples.h`. It reads `IMU/ax`, `ay`, `az` (`wx`, `wy`, `wz`) as
  effective values, the same vectors `channelsFrom()` hands the kernel, so
  the inference and `requireReadingsOnLattice()` see the same readings. The
  include of `samplecontinuity.h` from the registration is added to the
  audit's "readers consult the authority" list, eleven.
- **The hand-over gains a third reader.** `Fusion::channelsFrom()` takes,
  beside the measurement and attribute readers, a reader that says whether
  the session states a key (`using StoredAttributeReader =
  std::function<bool(const QString &key)>`); the registration passes
  `ctx.attributeIsStored`, the runner passes `session.hasStoredAttribute`,
  and `tst_fusion_session`'s local assembly follows. The fact is not a
  fifth attribute, is not written anywhere, and the kernel reads no session.
- **The four ids, their order and their outputs are unchanged.** The
  calculations are registered with `Calculations::addCalculation` in the
  same place and order (`registrationShape` holds the seventeen ids), with
  inputs now, so `addConstantDefault` no longer registers them and the
  audit's `constant-defaults` group is untouched; `configurationDefaults`
  of `tst_fusion_session` is amended (inputs declared, the default as the
  fallback, a stored value still wins). The firmware default stays spelled
  once, in `SensorConfiguration::defaultValue()`, which the fallback reads. The
  inference test lives in `tst_fusion_session` (sessions with the fit's
  inputs on the global registry); `tst_fusion_derived` stores the fit's
  outputs and reads none of the four, so it does not change; the runner's
  hand-over is proven by the existing `matchesTheApplicationImportPath`,
  which compares a keyless recording's diagnostics on both paths.
- **A session without IMU data has no configuration attributes.** The four
  are unavailable there as missing inputs; the fit's own missing input
  (`IMU/_time`, declared before them) is unchanged, so blockers and the
  demand layer report what they report today. Nothing else reads the four:
  the audit confines the keys' spelling to the vocabulary, and the importer
  and exporter read stored values.
- **Acceptance items are fixed by number.** The next free hundred is 1801.
  Phase 1 opens 1801-1802 (the audit's group comment, range check and
  completeness loop; the map's header, nineteen specifications; section
  9.19 and appendix S of `tests/README.md`); phase 2 extends to 1809; phase
  3 to 1819. The subjects are under "Interfaces between phases"; the
  statements are the documenters'.
- **Restated items.** Phase 3 restates "(as amended)" the items of
  `PLANS/done/noise-model-part-1.md` whose statements say the default is
  read where a key is absent or that a check judges a stated value: 1004,
  1008, 1010, 1045 and 1050, in the map, appendix K and matrix 9.11, and
  item 930 (appendix J, matrix 9.10), which says the registration registers
  the four's constant defaults. Items whose evidence text alone mentions the
  default have the evidence updated. Item 1702, which says the string "is
  `v11`", stands as history, as 1306 and 1407 stood for `v8` and `v9`.
- **The documents go with the phase that changes the behaviour.** Phase 1:
  `docs/CALCULATIONS.md` section 3. Phase 2: `docs/SENSOR_FUSION.md`
  sections 4 (the entries), 6 (the three wordings) and 7 (`v12`), the
  `v12` mentions of `docs/DATA_SCHEMA.md` and `docs/CALCULATIONS.md`
  section 17's history, section 8's sentence that names `v10` and `v11`,
  `tests/README.md` section 11's capture note. Phase
  3: `docs/DATA_SCHEMA.md` section 2, `docs/SENSOR_FUSION.md` section 3 and
  section 6's two cases (104 Hz fits, 70 Hz is rejected), `docs/CALCULATIONS.md`
  sections 5 and 17, the contracts of `sensorconfiguration.h` and
  `fusionregistration.h`, `tests/README.md`'s rows, 12.17 with M60,
  appendix S and 9.19 completed.
- **The manual step is M60**, section 12.17, and section 12's preamble
  counts seventeen scripts. The recording pair from firmware v2024.02.07 is
  Michael's; the step says what to look for.
- **The fit's arithmetic is untouched.** No channel of any golden moves in
  any phase. A review that sees a change to `factorgraphfit.*`,
  `imuintegration.*`, `fitcovariance.*`, `initializer.*`,
  `scaledimufactor.*` or `trajectoryreconstruction.*` rejects the phase.

## Interfaces between phases

### Phase 1 provides

- `bool EvaluationContext::attributeIsStored(const QString &key) const` in
  `src/engine/evaluationcontext.h`: true iff the declared attribute input
  `key` was served by the session's stored attribute; false when a
  candidate provided it; false, and an undeclared read, for a key the
  calculation did not declare. Identical on the synchronous and the prepared
  paths.
- The audit group `inferred-configuration` in `tests/audit/cleanup_audit.cmake`,
  with the range 1801-1802 declared in the traceability block; the map's
  header reading nineteen specifications; section 9.19 and appendix S of
  `tests/README.md` with items 1801 and 1802.
- Items: 1801 (specification section 4, first bullet, the engine's half:
  the query, its paths, the undeclared read), 1802 (section 5, the engine's
  section of `docs/CALCULATIONS.md`).

### Phase 2 provides

- In `src/fusion/fusion.h`: `enum class ConfigurationSource { Stated,
  Inferred };` and the members `ImuConfiguration::accelFsGSource`,
  `gyroFsDegSSource`, `accelOdrHzSource`, `gyroOdrHzSource`, each
  `ConfigurationSource::Stated` by default; `Algorithm` is
  `batch-temperature-bias-v12`.
- The rejection texts, exactly: `<KEY> states <v> Hz but the IMU is logged
  at <f> Hz; sensor fusion unavailable` and `<KEY> inferred as <v> Hz but the
  IMU is logged at <f> Hz; sensor fusion unavailable` for the rates; `<KEY>
  states +/-<v> <unit> but the <sensor> readings lie on <shown>; sensor
  fusion unavailable` and `<KEY> defaulted to +/-<v> <unit> but the <sensor>
  readings lie on <shown>; sensor fusion unavailable` for the ranges, with
  `<shown>` `the +/-<r> <unit> lattice` or `no range's lattice` as today.
- In the diagnostics' `configuration` object, beside `accel_fs_g`,
  `gyro_fs_deg_s`, `accel_odr_hz` and `gyro_odr_hz`, the entries
  `accel_fs_g_provenance`, `gyro_fs_deg_s_provenance`, `accel_odr_hz_provenance`,
  `gyro_odr_hz_provenance`, each the text `stated` or `inferred`.
- The goldens under `tests/data/fusion/` captured under `v12`, with the four
  entries `stated` in the four fits.
- The range extended to 1801-1809. Items: 1803 (section 4, second bullet:
  the checks unchanged in rule and order, the three wordings, the datasheet
  rejection unchanged), 1804 (section 4, third bullet: the diagnostics'
  provenance entries), 1805 (section 4, fourth bullet: `v12`; a `v11` record
  and a `v11` rejection stale at load, the fit run again once; the
  arithmetic, outputs, record shape and goldens' channels unchanged), 1806
  (section 6, the fit bullet, the kernel's half: the `inferred as 52 Hz`
  rejection at 70 Hz, the `states` rejection for a stated 12.5 at 104 Hz,
  the `defaulted` rejection on no lattice, the fourteen goldens unchanged
  but for the string and the entries), 1807 (section 6, the record bullet),
  1808 (section 6, the audit bullet: the rules say `v12`; the default is
  still written once), 1809 (section 5, the kernel's documents: sections 4,
  6 and 7 of `docs/SENSOR_FUSION.md`, the `v12` mentions).

### Phase 3 provides

- Nothing a later phase consumes. It consumes phase 1's query and phase 2's
  enum, members, texts, entries and string, and completes the range:
  items 1810 (section 3, the table and its first bullet: the four on-demand
  calculations under the same ids, their inputs, the kernel's functions, the
  registration solver-free), 1811 (section 3, the fallback), 1812 (section
  3, nearest not exact, and equal rates in practice), 1813 (section 3, on
  disk: every fixture session without its keys infers its stated
  configuration, its channels bit for bit the golden), 1814 (section 4,
  first bullet, the fit's half: the inputs unchanged by name, the provenance
  carried through the context's query into the kernel by the registration
  and the runner alike, no fifth attribute, nothing written), 1815 (section
  6, the inference bullet), 1816 (section 6, the fit bullet, the session's
  half: 104 Hz without keys fits with `inferred` in its diagnostics, 70 Hz
  is rejected `inferred as 52 Hz`, a stated 12.5 at 104 Hz is rejected
  `states`, no lattice is rejected `defaulted`; a stored `v11` rejection of
  a 104 Hz recording without keys retried at load and succeeding), 1817
  (section 6, the manual bullet: M60), 1818 (section 5, the remaining
  documents and the two contracts), 1819 (section 6, the last bullet: items
  1801-1819, appendix S, section 9.19, the restated items).
- `tests/README.md` section 12.17 with step M60, and section 12's preamble
  at seventeen scripts.

## Commit Policy

Michael has authorized commits for this plan on the existing working branch.
Nothing is ever pushed, and nothing is ever committed on `master` or
`fusion-improvements`. One commit per accepted phase, subject
`Inferred IMU configuration: <phase>`, with the session's attribution line,
staged by explicit path; `PLANS/` is never staged.
