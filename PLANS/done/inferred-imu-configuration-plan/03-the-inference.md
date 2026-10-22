# Phase 3: The inference

Written 2026-10-21 against branch `store-requested-calculations` at `dbf5a78`
from `PLANS/implementation-plan/00-overview.md` (specification sections 3, 4,
5 and 6; "Decisions and constraints"; "Phase 3 provides"). The overview is the
authority where this document and it disagree.

## Purpose

The four calculations `builtin.default.ACCEL_FS_G`, `GYRO_FS_DEG_S`,
`ACCEL_ODR_HZ` and `GYRO_ODR_HZ` stop being constants and become on-demand
inferences from the recording's own IMU data, the firmware default their
fallback; the fit hands the kernel, beside each configuration value, whether
the recording stated it, through phase 1's query and phase 2's members. A
recording from firmware v2024.02.07 (104 Hz, no keys) then fits; every
recording on disk infers exactly what it read before and fits bit for bit.
One phase: the registration, the vocabulary it reads, the tooling that shares
its assembly, their tests, the documents that describe the user-visible
change, the restated items and the manual step; nothing after it.

## Dependencies

- Phase 1: `bool EvaluationContext::attributeIsStored(const QString &key) const`;
  its audit rule already allows `fusionregistration.cpp` as the one reader.
- Phase 2: `Fusion::ConfigurationSource { Stated, Inferred }` and the four
  `*Source` members of `ImuConfiguration` (`Stated` by default);
  `requireConfiguredRates()`; the `inferred as` and `defaulted to` texts;
  the four `*_provenance` entries; `batch-temperature-bias-v12` with the
  goldens captured under it (every entry `stated`); the group
  `inferred-configuration` at 1801-1809 in its three places; section 9.19 and
  appendix S at 1809; the store tests' `v11` cases.
- Blocks nothing. Re-captures nothing: the fixtures state their
  configuration, so the kernel sees `Stated` and the goldens stand.

## What changes, by area

### The vocabulary (`src/sensorconfiguration.h`, `.cpp`)

The private helper `listedValues()` becomes part of the contract, same name
and signature: `QStringList listedValues(const QString &key);` the values a
key with a list accepts, in the order `unsupportedMessage()` prints them
(ascending; `1.6` first for `ACCEL_ODR_HZ`), empty for a key without a list
and for a key that is not one of the nine. The header comment says the four
IMU keys' default is the fallback of the inference the fusion registration
makes (not "registers their defaults"), and that the inference chooses among
`listedValues()`. `defaultValue()`, `isValidValue()` and the lists' spellings
do not change; the firmware version stays spelled once.

### The registration (`src/fusion/fusionregistration.cpp`, `.h`)

- `registerConfigurationDefaults()` registers, through
  `Calculations::addCalculation` and no longer `addConstantDefault`, four
  on-demand descriptors in the same place and order (the first four of the
  seventeen `registrationShape` pins), ids unchanged, no title, no result
  version, each with the attribute as its one output:
  - `ACCEL_FS_G`: inputs `IMU/ax`, `IMU/ay`, `IMU/az`; the value is
    `Detail::rangeShownByReadings(ImuSensor::Accelerometer, ax, ay, az)`
    (`fusion/sensornoise.h`, Qt Core and std only) over the effective
    vectors the context serves, the same vectors `channelsFrom()` hands the
    kernel; NaN means the fallback.
  - `GYRO_FS_DEG_S`: `IMU/wx`, `wy`, `wz`, the gyro likewise.
  - `ACCEL_ODR_HZ`, `GYRO_ODR_HZ`: input `IMU/_time`; the median logged
    interval is `SampleContinuity::nominalInterval()` (`samplecontinuity.h`,
    `flysight_model`, which `flysight_fusion` links publicly) of the axis as
    the engine serves it; NaN (fewer than three samples, or an axis not
    finite and increasing) means the fallback.
  - The text: the entry of `SensorConfiguration::listedValues(key)` whose
    number equals the range shown (ranges), or whose number is nearest to
    one over the median (rates; the lower entry on an exact tie); the
    fallback is `SensorConfiguration::defaultValue(key)`. The result is a
    `QString` from the list, so a stated and an inferred value of the same
    text are the same text, and nothing here spells a key or a value.
  - The compute never returns `unavailable()` on account of the data; the
    engine withholds it only when an input is missing (no IMU sensor, no
    `IMU/_time`). Pattern: `registerHorizontalSpeed()` for the descriptor,
    `builtin.attr.groundElev` (`attributecalculations.cpp`) for a default
    derived from the data with a fallback.
- `kConfigurationInputs` stays the one table for the declaration, the
  hand-over and now the inferences: extend it (the source member pointer
  `ConfigurationSource Fusion::ImuConfiguration::*source`, and what the
  implementer needs to drive the four registrations from it); structure is
  the implementer's.
- `Fusion::channelsFrom()` gains a third reader:
  `using StoredAttributeReader = std::function<bool(const QString &key)>;`
  `Channels channelsFrom(const MeasurementReader &, const AttributeReader &,
  const StoredAttributeReader &stored);` For each configuration key the
  source member is `Stated` when `stored(key)` is true and `Inferred`
  otherwise; the value is read as today (a stored value that is not a number
  stays NaN, and stays `Stated`, so the datasheet check rejects it as it
  does now). The origin attributes have no provenance. The private adapter
  `channelsFrom(const EvaluationContext &)` passes
  `[&ctx](const QString &key) { return ctx.attributeIsStored(key); }`; the
  four keys are declared inputs of the fit, so this is never an undeclared
  read. `computeFit()`, `fitInputs()` (twenty-six, unchanged by name and
  order), `publish()`, the output table and every other registration are
  untouched.
- Includes added: `fusion/sensornoise.h` and `samplecontinuity.h`. Never
  `fusionsamples.h` or any other internal kernel header; the file stays free
  of GTSAM and Eigen (`solver-confinement`'s rule holds as it is).
- The header's contract: the four bullets describe on-demand inferences with
  their inputs and the fallback; `channelsFrom()`'s comment describes the
  third reader and the sources; the "constant defaults" wording goes.

### The tooling (`tests/fusion_runner.cpp`)

The one assembly passes `[&session](const QString &key) { return
session.hasStoredAttribute(key); }` as the third reader. Nothing else: the
dump already prints the effective text of each configuration attribute, which
for a keyless recording is now the inferred text.

### What does not change

The kernel (`src/fusion/*` other than the registration; a diff to the fit's
arithmetic files named in the overview fails the phase), the engine, the
importer, the exporter, the plugin bridge, `fusiongolden.cpp`'s `toChannels()`
(sources `Stated`), the capture tool, the goldens, the fixtures,
`addConstantDefault` and the `constant-defaults` group. A session without IMU
data: the four attributes are unavailable there as missing inputs of their
calculations, and the fit's readiness, blockers and the demand layer report
`MissingInput` / `NotApplicable` exactly as today (`IMU/_time` is declared
before them).

## Interfaces

Provided to later phases: nothing. Consumed: phase 1's query and phase 2's
enum, members, texts, entries and string. The two public signatures this
phase fixes, because the runner and the tests share them:
`SensorConfiguration::listedValues(const QString &key)` and the three-reader
`Fusion::channelsFrom()` with `Fusion::StoredAttributeReader`, above.

## Acceptance criteria

The section is the specification's; each is an item of the map, its
statement written out in full there in the `# <item> - (<section>) ...` form
(1815 and 1816 list every case of their tests), with the lines named after
"Lines:" (test names are fixed under "Tests").

- **1810** (3, the table and its first bullet): the four calculations under
  the same ids, place and order, each the attribute as its one output, the
  text an entry of the key's list as the importer stores it; the ranges
  through `Detail::rangeShownByReadings()` on the effective readings, the
  rates through `SampleContinuity::nominalInterval()` on `IMU/_time`; the
  registration includes `fusion/sensornoise.h` and `samplecontinuity.h`,
  never `fusionsamples.h`, and stays solver-free; the vocabulary exposes each
  key's list; the firmware default spelled once, where the fallback reads it.
  Lines: `tst_fusion_session registrationShape`, `configurationDefaults`;
  `tst_sensor_configuration listedValuesAreExposed`; `audit
  inferred-configuration`, `sample-continuity`, `solver-confinement`,
  `sensor-configuration`, `constant-defaults`.
- **1811** (3, the fallback): no lattice, or an axis without a nominal
  interval (fewer than three samples included), yields the default's text bit
  for bit; a recording with IMU data always has a configuration; one without
  has a missing input; the check then rejects `defaulted to` naming what the
  data show, or passes. Lines: `tst_fusion_session configurationIsInferred`,
  `inferredConfigurationFits`, `missingInputsAreNotApplicable`.
- **1812** (3, nearest not exact; equal rates): 100.0 Hz logged infers 104
  and passes at 4 %; 13.2-13.3 Hz infers 12.5; 70 Hz infers 52 and is
  rejected at 25 %; near 1.6 Hz the accelerometer infers `1.6` (the datasheet
  check rejects it as it rejects a stated `1.6`) and the gyro `12.5`; the two
  rates are always one allowed value; the inference and the checks read the
  same readings and interval through the same functions. Lines:
  `tst_fusion_session configurationIsInferred`, `inferredConfigurationFits`,
  `naturalSessionEndToEnd`.
- **1813** (3, on disk): every recording on disk infers the firmware default
  and fits bit for bit; a 104 Hz fixture session without its keys infers its
  stated configuration, its channels bit for bit the golden's, its
  diagnostics the golden's but for the four entries `inferred`; nothing
  re-captured. Lines: `tst_fusion_session inferredConfigurationFits`,
  `naturalSessionEndToEnd`; `tst_fusion_runner matchesTheApplicationImportPath`;
  `tst_fusion_golden successFixturesMatchGolden`; `manual M60`.
- **1814** (4, first bullet, the fit's half): the inputs unchanged by name;
  the fit asks `attributeIsStored` per configuration input and hands the
  kernel `Stated` / `Inferred` through the third reader; the runner passes
  `hasStoredAttribute`; the session test's local assembly follows; no fifth
  attribute, nothing written, the kernel reads no session. Lines:
  `tst_fusion_session registrationShape`, `configurationReachesTheKernel`,
  `inferredConfigurationFits`, `missingInputsAreNotApplicable`;
  `tst_fusion_runner matchesTheApplicationImportPath`; `tst_fusion_jobs
  noImuSessionCannotHaveAJob`; `audit inferred-configuration`.
- **1815** (6, the inference bullet): the cases of `configurationIsInferred`.
  Lines: it, `configurationDefaults`, `missingInputsAreNotApplicable`,
  `readsNeverRunTheFit`, all `tst_fusion_session`.
- **1816** (6, the fit bullet, the session's half): the four session-level
  fit outcomes and the stored `v11` rejection retried at load and succeeding.
  Lines: `tst_fusion_session inferredConfigurationFits`,
  `configurationReachesTheKernel`; `tst_fusion_store
  staleRejectionWithoutKeysIsFittedAtLoad`; `tst_fusion_runner
  matchesTheApplicationImportPath`; `manual M60`.
- **1817** (6, the manual bullet): M60 in section 12.17; seventeen scripts.
  Lines: `tst_fusion_store staleRejectionWithoutKeysIsFittedAtLoad` (a
  floor); `manual M60`.
- **1818** (5, the remaining documents and the two contracts): as under "The
  documents"; the audit's reader count at eleven. Lines: `audit
  inferred-configuration`, `audit sample-continuity` (floors; the reviewer
  reads the documents).
- **1819** (6, the last bullet): 1801-1819 complete in the map, appendix S
  and section 9.19; items 930, 1004, 1008, 1010, 1045 and 1050 restated;
  every upper bound 1809 at 1819; the audit and the map check green. Line:
  `audit inferred-configuration`.

## Tests

### `tests/tst_fusion_session.cpp`

How a test builds its sessions: start from `sessionFromFixture(coarse_linear)`
(201 IMU samples at 100 Hz, `ax = ay = 0`, `az = -9.80665`, zero rates,
stated 16 / 2000 / 104 / 104) and `removeAttribute()` the four keys. Times:
`setMeasurement("IMU", "_time", axis)` with `axis[i] = f.imuTime[0] + i / r`
(the column is stored source data in "s", served unchanged). A finer lattice:
set every sample of `IMU/ax` to an odd multiple of the +/-8 g step,
`(2 i + 1) * (8. / 32768 * 9.80665)`, and of `IMU/wx` to an odd multiple of
the +/-500 deg/s step, `(2 i + 1) * 17.5e-3`: on the finer lattice, and off
the coarser ones by a whole fine step, far above the check's tolerances
(9.8e-5 m/s^2, 1.15e-3 deg/s); `az` at -2048 counts of +/-16 g lies on both.
No lattice: `ax[5] = 1.5 * 9.80665e-5` (one reading in 603 is more than one
in a thousand); the gyro `wz[7] = 1.5 * 1.14688e-3`.

- `registrationShape`: the seventeen ids unchanged; nothing else here (the
  `v12` literal is phase 2's).
- `configurationDefaults` (amended; the map cites it for 1004 and 1050): each
  `builtin.default.<KEY>` is `OnDemand`, no result version, the attribute its
  one output, one candidate, inputs `IMU/ax`, `ay`, `az` / `IMU/wx`, `wy`,
  `wz` / `IMU/_time` / `IMU/_time` in that order; the other five keys have no
  candidate. `naturalSession()` (12.5 Hz, default lattices, no keys) reads
  `16`, `2000`, `12.5`, `12.5`, each `userType()` `QString` and the text
  literal (the fallback is the default's text, bit for bit). A stored value
  wins, also one that is not a value of the key (`8`, `abc`); removing it
  returns to the inferred text. `runCount(kFit)` 0.
- New `configurationIsInferred` (item 1815): (a) 104 Hz on the +/-8 g and
  +/-500 deg/s lattices reads `104`, `104`, `8`, `500`; (b) 13.3 Hz on the
  fixture's own lattices reads `16`, `2000`, `12.5`, `12.5`; (c) no
  accelerometer lattice at 100 Hz reads `16` for the range and `104` for both
  rates, the gyro likewise `2000`; (d) a stated `ACCEL_FS_G` `8` over the
  +/-16 g readings reads `8`, a stated `ACCEL_ODR_HZ` `12.5` at 100 Hz reads
  `12.5`; (e) 1.6 Hz reads `1.6` for `ACCEL_ODR_HZ` and `12.5` for
  `GYRO_ODR_HZ`; (f) `IMU/_time` truncated to two samples reads `12.5` twice;
  (g) on every case but (e) the two rates read the same text;
  (h) `sessionWithoutImu()` leaves each of the four `!isValid()`. Throughout:
  `runCount(kFit)` 0, `undeclaredReadCount()` 0; after a first read each
  `builtin.default.<KEY>` has run once and a second read runs nothing.
- `configurationReachesTheKernel` (amended): the local `channelsOf` passes
  `session.hasStoredAttribute` as the third reader; the stated session's four
  sources are `Stated`; the keys-removed session's values are 16 / 2000 /
  104 / 104 with every source `Inferred`; the edited session's `gyroFsDegS`
  500 `Stated`, `gyroOdrHz` NaN `Stated`. The stated path's fit as today. The
  rejection: a session that stores `ACCEL_ODR_HZ` and `GYRO_ODR_HZ` as `12.5`
  over the 100 Hz fixture (ranges stated) is rejected `ACCEL_ODR_HZ states
  12.5 Hz but the IMU is logged at 100.0 Hz; sensor fusion unavailable`,
  `resultDetail` and the diagnostics' `failure` equal, no value available.
- New `inferredConfigurationFits` (items 1813, 1816): `coarse_maneuver` as a
  session without its keys fits (`request` `Ok`, empty detail),
  `goldenDifference` empty, the diagnostics' four `_provenance` entries
  `inferred` and, with those four set to `stated`, `compareJson` against the
  golden empty (byte-equal in exact mode); `coarse_linear` without keys at
  70 Hz rejected `ACCEL_ODR_HZ inferred as 52 Hz but the IMU is logged at
  70.0 Hz; sensor fusion unavailable`; `coarse_linear` without keys with
  `ax[5]` off every lattice rejected `ACCEL_FS_G defaulted to +/-16 g but the
  accelerometer readings lie on no range's lattice; sensor fusion
  unavailable`; each rejection a cached result, no value available, run once.
- `naturalSessionEndToEnd`: add the four entries `inferred`; the comment says
  the recording infers the firmware default. `missingInputsAreNotApplicable`,
  `readsNeverRunTheFit`, `restoredFitIsIndistinguishable`: unchanged (the
  last already asserts a resolution per declared input).

### `tests/tst_fusion_store.cpp`

New `staleRejectionWithoutKeysIsFittedAtLoad` (items 1816, 1817's floor):
`fixtureSession("coarse_linear")` with the four keys removed, as `a`; shown,
the Roll row checked, the fit ends `Succeeded` and the record is written;
unloaded as `recordUnderPreviousAlgorithmIsComputedAgainOnce` does; the
record rewritten as `restoredSolverFailureShowsBadge` rewrites one, into the
rejection shape with `resultVersion` `batch-temperature-bias-v11` and the
reason and diagnostics `v11` wrote for this recording (`ACCEL_ODR_HZ states
12.5 Hz but the IMU is logged at 100.0 Hz; sensor fusion unavailable`, the
diagnostics `algorithm` and `failure` only); shown again: the record deleted
(`staleRecordsDeleted` 1), the fit run once and `Ok`, a record present again
under `Fusion::Algorithm`, `goldenDifference` empty, the four entries
`inferred`, the recording listed among no failures (`nothingToShow()`),
further spins run nothing. Phase 2's two `v11` functions stand.

### `tests/tst_sensor_configuration.cpp`

New `listedValuesAreExposed` (item 1810): `listedValues()` of each listed key
equals the test's `listed()` literal in order; empty for the four free-form
keys and for `SCHEMA_VER`; each IMU key's `defaultValue()` is in its list.

### Other executables

- `tst_fusion_runner`: no new case. `matchesTheApplicationImportPath` fits
  `naturalSession()` (no keys) through the tool and through the application
  and compares the diagnostics: with the third reader missing from the tool
  its entries would read `stated` against the application's `inferred`, so
  the existing comparison is the proof of the runner's hand-over; its comment
  says so. `successMatchesDirectRun`'s four stated lines stand.
- `tst_fusion_derived`: nothing; its sessions store no configuration and
  read none of the four. `tst_fusion_jobs::noImuSessionCannotHaveAJob` and
  the demand tests: unchanged, cited for the no-IMU rule.
- `tst_fusion_kernel`, `tst_fusion_golden`: unchanged.

### The audit (`tests/audit/cleanup_audit.cmake`)

- `sample-continuity`, "the readers consult the authority": add
  `src/fusion/fusionregistration.cpp` to the list, count 11; the "Allow:"
  comment already describes the addition.
- Group `inferred-configuration` gains, each with an "Allow:" comment and
  planted once: `expect_none` that `fusionregistration.cpp` and `.h` include
  no internal kernel header (the alternation of "the tools see the public
  header or the trace seam only" plus `fusionpipeline`); `expect_only` that
  `rangeShownByReadings\(` is spelled in `src` in `sensornoise.(h|cpp)`,
  `fusionsamples.cpp` and `fusionregistration.cpp` alone (one authority for
  the range the readings show); `expect_none` over `docs`, `tests/README.md`
  and `src` of `four constant defaults|configuration's constant
  defaults|their constant defaults` (the stale wording). Phase 2's document
  rules (`inferred as` and `defaulted to` once each in `SENSOR_FUSION.md`)
  are counts of lines: write section 6's two cases so that each phrase stays
  on one line, or raise the count with the sentence that needs it. Phase 1's
  count of `EvaluationContext::attributeIsStored` in `CALCULATIONS.md` stays
  1: section 17 names the query unqualified ("the provenance query of
  section 3, `attributeIsStored`").
- The header comment's last bullet extended with the inference (the four
  calculations infer from the data through the kernel's own function and the
  continuity authority, the default their fallback, spelled once); the
  traceability block: every `1809` becomes `1819` (the comment, the range
  check's clause and its message, the loop).

### The map (`tests/acceptance_map.txt`)

Header: the 1801 paragraph reads 1801-1819, its item sentence extended
(1810-1813 section 3, 1814 the fit's half of section 4, 1815-1817 and 1819
section 6, 1818 section 5) and its last sentence "Items 930 (section 9.10,
appendix J), 1004, 1008, 1010, 1045 and 1050 (section 9.11, appendix K) are
restated as amended"; the 901-940 and 1001-1065 paragraphs gain "It is
amended by the specification of 1801-1819: ..." with those items; "or
1801-1819" in the automated-line sentence. The restated statements:

- 930 (9, as amended): "... since the specification of 1801-1819 the
  registration declares the four configuration inputs and registers their
  inferences, the firmware default their fallback ...".
- 1004 (5, as amended): a recording that lacks a key infers it from its IMU
  data among the key's allowed values; where the data cannot decide it takes
  the firmware default, v2023.09.22's ...; the default is written once in the
  documentation and once in the code, as the fallback, with the firmware
  version. Evidence: add `tst_fusion_session::configurationIsInferred`.
- 1008 (5, as amended): "whether stated or inferred, each IMU sensor's range
  is checked ..."; the reason names the range stated, or defaulted to, and the
  range the values show. Evidence: add
  `tst_fusion_session::inferredConfigurationFits`.
- 1010 (5, as amended): "... with a stated or an inferred rate ... a rejection
  that names both and says which". Evidence: `configurationReachesTheKernel`
  (now the stated 12.5 at 100 Hz) and `inferredConfigurationFits`.
- 1045 (9, as amended): the stated path and the inferred path (a session
  without the keys). Evidence: add `inferredConfigurationFits`.
- 1050 (11, as amended): "a file without them infers its configuration, the
  firmware default where the data cannot decide". Evidence stands
  (`matchesTheApplicationImportPath` is the keyless import).

Items 1810-1819 follow 1809 with the statements and lines of "Acceptance
criteria".

### The guide (`tests/README.md`)

- Contents: 12.17. Section 1: the rows of `tst_fusion_session`,
  `tst_fusion_store`, `tst_fusion_runner` and `tst_sensor_configuration`
  gain what their new and amended functions prove.
- Section 9.10: row 930 "(as amended)"; 9.11: rows 1004, 1008, 1010, 1045,
  1050 "(as amended)"; 9.19: heading, preamble and table to 1819 in 9.18's
  columns, the preamble saying which item is which, that the audit lines of
  1817 and 1818 are floors (M60 and the reviewer's reading are the checks)
  and which items of 9.10 and 9.11 are restated. Appendix J item 30 and
  appendix K items 4, 8, 10, 45 and 50 restated; appendix S entries 10-19,
  its preamble naming the amended appendices.
- Section 10: the `inferred-configuration` bullet lists this phase's rules;
  the `sample-continuity` bullet's reader count, if it states one. Section
  11, "Fusion sessions": `naturalSession()` infers the firmware default.
- Section 12's preamble: "Seventeen scripts."; 12.17 "Inferred IMU
  configuration" with **M60 (1813, 1816, 1817)**, on 12.16's pattern:
  Michael's `TRACK.CSV` / `SENSOR.CSV` pair from firmware v2024.02.07 (104 Hz,
  no keys), a logbook COPY. 1. On the build before this change (as M59 step
  7 sets one up): import the pair, check a fusion plot; the status bar's
  warning lists the recording with `ACCEL_ODR_HZ states 12.5 Hz but the IMU
  is logged at 100.0 Hz`. 2. Start this build on the copy and check the plot:
  the stale rejection is computed again once (the status bar's "Computing
  results"), the fit succeeds and the plots draw; not again after a hide and
  a show, or a restart. 3. `fusion_runner` on the pair (12.2): exit 0, the
  diagnostics' `configuration` reads 16, 2000, 104, 104 with the four
  `_provenance` entries `inferred`; `--dump-inputs` shows `ACCEL_FS_G,16`,
  `GYRO_FS_DEG_S,2000`, `ACCEL_ODR_HZ,104`, `GYRO_ODR_HZ,104`. 4. A recording
  on disk (firmware v2023.09.22): computed again once after the update; its
  `configuration` reads the firmware default with every entry `inferred`,
  and its `--csv` from this build is byte-identical to the earlier build's.
  Pass / fail per step in the phase report.

### The documents

- `docs/DATA_SCHEMA.md` section 2: the keys' paragraph says "stated or
  inferred"; "**The default.**" becomes the inference: an absent key is
  inferred among the key's allowed values (ranges from the lattice the
  readings show, rates from the median logged interval, `SENSOR_FUSION.md`
  section 3), the firmware default, v2023.09.22's values and its fixed
  filters as described, the fallback where the data cannot decide; a
  calculation (`builtin.default.<key>`), never stored or written; every
  recording on disk lacks the keys and infers this default.
- `docs/SENSOR_FUSION.md` section 3, "Configuration": stated or inferred, the
  rule per key, the fallback, the equal-rates note (one `$IMU` row per
  sample; two stated rates one logged interval cannot both satisfy is a
  question for the format, not resolved; the check keeps its order); "The
  lattice" and "The rate": the inference reads what the check reads, through
  the same function. Section 6: a 104 Hz recording without keys infers 104
  and fits (4 %), one logged at 70 Hz infers 52 and is rejected `inferred as
  52 Hz ... 70.0 Hz`; the `states 12.5 Hz` example becomes a recording that
  states 12.5. Section 7's `v12` sentence completed with the inference;
  section 8's "at the default configuration (none states one)" says each
  infers the firmware default; the test table's `tst_fusion_session` row.
- `docs/CALCULATIONS.md` section 5: the four move from the constants to the
  defaults derived from the data, pointing at section 17; section 17: the
  four table rows with their inputs; "The four constant defaults above carry
  the default's text" and "never missing ... reads its constant default"
  rewritten (inferred from the IMU data, the default the fallback; a
  recording with IMU data never lacks them, one without lacks them with the
  fit's own inputs); the fit asks the provenance query of section 3 per
  configuration input and hands the kernel whether it was stated; the
  record's resolutions name "the configuration's inferences where the
  session states no value".
- `docs/COMPUTED_PLOTS.md`: nothing (it names neither the configuration nor
  12.5 Hz). The contracts of `sensorconfiguration.h` and
  `fusionregistration.h` as above; `tests/fusion/fusionsessions.h` and
  `.cpp` ("the default path" becomes the inferred path).

## Decisions

- **Nearest by rate.** The specification's table (amended 2026-10-21) says
  the allowed rate nearest to one over the median interval; its bullet, its
  section 6 test, the overview's items 1806 and 1816 and phase 2's kernel
  case all say a 70 Hz recording infers 52. By interval 1/70 would be nearer
  1/104 (4.67 ms) than 1/52 (4.95 ms; the boundary is 69.3 Hz), which is why
  the table's earlier phrase was amended. This phase infers the allowed rate
  nearest to one over the median; for a real recording (a listed rate a few
  percent fast) the two rules never differ.
- **The fallback follows the authority.** `nominalInterval()` is NaN below
  three samples, so two samples infer the default, as the specification's
  fallback bullet (amended 2026-10-21) says. The engine's UTC axis and the kernel's
  epoch-relative axis give the same differences (each subtraction of two
  nearby doubles is exact, Sterbenz), and the nearest rule's gaps and the
  10 % tolerance are in any case many orders above an ulp, so the inference
  and the check cannot disagree on a healthy recording.
- **`listedValues` promoted, not renamed**; **a third reader, not a struct**:
  the contract and `channelsFrom()` keep their shape, and the runner and the
  session test change one line each.
- **The inference test lives in `tst_fusion_session`** (sessions with the
  fit's inputs on the global registry); `tst_fusion_derived` stores the fit's
  outputs and reads none of the four. **No new runner case**:
  `matchesTheApplicationImportPath` fits a keyless recording on both paths and
  compares the diagnostics entry by entry; a runner without the third reader
  fails it.
- **Items 930 and 1045 restated** beyond the four the overview names: 930
  says the registration "registers their constant defaults", 1045 names "the
  default path"; both would be false. 1005, 1007 and 1061 stand.
- **The store case is a rewritten record**, the rejection shape with the
  `v11` stamp and reason a `v11` build stored, as
  `restoredSolverFailureShowsBadge` rewrites one. **Test names fixed here**,
  because the map cites them.

Ready: the specification's table phrase and fallback sentence were amended
on 2026-10-21 to the rules above; everything follows the overview.
