# Phase 2: The kernel words the provenance

Written 2026-10-21 against branch `store-requested-calculations` at `dbf5a78`
from `PLANS/implementation-plan/00-overview.md` (specification sections 4, 5
and 6; "Decisions and constraints"; "Phase 2 provides"). The overview is the
authority where this document and it disagree.

## Purpose

The kernel learns, beside each of the four configuration values, whether the
recording stated it or it was inferred, and uses that fact in two places
only: the wording of the lattice and rate rejections, and four entries of the
diagnostics' `configuration` object. No check changes rule or order, no
number of the fit changes, no golden channel moves. Because the entries
change what `run()` returns for the same channels, the algorithm string moves
to `batch-temperature-bias-v12` here, with what a move drags along (the
goldens, the `v11` literals, the store tests' stale cases, the documents'
history). One component, the kernel under `src/fusion/`, with its own tests
and documents; it leaves the tree green alone, since the registration (phase
3) never sets `Inferred` yet.

## Dependencies

- Nothing in code. Implemented after phase 1 only because both edit
  `tests/acceptance_map.txt`, `tests/README.md`, `docs/CALCULATIONS.md` and
  `tests/audit/cleanup_audit.cmake`.
- Assumed from phase 1: the audit group `inferred-configuration` with the
  range 1801-1802 in the traceability block (the header comment's group
  list, the range check's clause and message, a completeness loop); the
  map's header at nineteen specifications with an entry for 1801-1802;
  section 9's preamble at nineteen; section 9.19 and appendix S of
  `tests/README.md` with items 1801 and 1802. This phase extends every one
  of those upper bounds to 1809, under phase 1's names.
- Blocks phase 3, which sets `Inferred` from the registration and the runner,
  reads the four entries and asserts the texts below through the engine.

## What changes

### The kernel's contract (`src/fusion/fusion.h`)

- `enum class ConfigurationSource { Stated, Inferred };` and four members of
  `ImuConfiguration`: `accelFsGSource`, `gyroFsDegSSource`,
  `accelOdrHzSource`, `gyroOdrHzSource`, each `ConfigurationSource::Stated`
  by default. The names are the contract with phase 3. There is no third
  state: "defaulted" is a wording.
- The class comment of `ImuConfiguration` drops "their constant defaults
  supply it" and says the source members record whether the recording
  stated each value or it was inferred, which the kernel is told and never
  works out. `Channels::imuConfiguration`'s comment says "configured" ranges
  and rates, not "stated". `Result`'s diagnostics comment says
  `configuration` carries, beside each value, whether it was stated or
  inferred. `Algorithm` becomes `batch-temperature-bias-v12`; its comment
  stands.
- `Detail::imuNoise()` copies the whole struct into `ImuNoise::configuration`
  and `successDiagnostics()` reads `tuning.noise.configuration`, so the
  sources reach the diagnostics through the copy that exists today; add no
  plumbing.

### The two checks (`src/fusion/fusionsamples.h`, `.cpp`)

- `requireReadingsOnLattice()`: the same comparison of
  `rangeShownByReadings()` with the configured range, accelerometer first,
  the same `<shown>` text; the verb follows the sensor's source, `states`
  for `Stated` and `defaulted to` for `Inferred`, whatever `<shown>` is (the
  overview: an inferred range fails only through the fallback, a hand-built
  case gets the same word, no third wording). The texts are under
  "Interfaces".
- `requireStatedRates()` is renamed `requireConfiguredRates()` (a decision
  below): the same `kRateTolerance`, order and number formats; `states` for
  `Stated`, `inferred as` for `Inferred`.
- The three verbs are spelled once each in `src`, in this file; the header
  comments say the configured value, stated or inferred, and how the
  rejection is worded.

### Unchanged by rule, named for the reviewer

- `sensornoise.h`, `.cpp`: `imuNoise()`, the `No datasheet entry` text and
  its key order, `rangeShownByReadings()`, the tables.
- `fusion.cpp`: `planFit()`'s order (the recording's checks, then entry,
  lattice, rate); the only edit is the renamed call. `failureDiagnostics()`:
  `algorithm` and `failure` (and `stopping`, `quality` when there are any),
  never `configuration`.
- `factorgraphfit.*`, `imuintegration.*`, `fitcovariance.*`, `initializer.*`,
  `scaledimufactor.*`, `trajectoryreconstruction.*`, `inputadapter.*`,
  `fusionpipeline.*`: a diff that touches any of them fails the phase.

### The diagnostics (`src/fusion/fusionoutput.cpp`)

`configurationObject()` gains `accel_fs_g_provenance`,
`gyro_fs_deg_s_provenance`, `accel_odr_hz_provenance`,
`gyro_odr_hz_provenance`, each `stated` or `inferred` from the member's
source; the four keys and two texts spelled once, here. QJsonObject sorts
keys, so the object reads `accel_fs_g`, `accel_fs_g_provenance`,
`accel_odr_hz`, `accel_odr_hz_provenance`, `gyro_fs_deg_s`,
`gyro_fs_deg_s_provenance`, `gyro_odr_hz`, `gyro_odr_hz_provenance`.
`tests/fusion/fusiongolden.cpp` compares
a string entry by equality in both modes and needs no change.

### The goldens (`tests/data/fusion/`)

Re-capture all fourteen by section 11's procedure at the end of the phase
from a `v12` build. Expected: the four `.channels.txt` byte-identical to
HEAD's; the ten `reject_*.json` changed in `algorithm` alone; the four
success `.json` in `algorithm` and four added `_provenance` lines, each
`stated`; `capture.json` as always. Prove it before the committed capture as
the 2026-10-06 notes did: capture into a scratch directory, `cmp` each
channels file against `git show HEAD:tests/data/fusion/<f>.channels.txt`
(CRs stripped), and each `.json` once its `algorithm` is set back to `v11`
and, in the four fits, the four `_provenance` lines removed; then steps 3, 4 and
5 on the real capture. Record it as a capture note in section 11 in the
pattern of the 2026-10-06 notes; move the procedure's "What changes" bullet
to `v12` with `v11` as history. `toChannels()` in `fusiongolden.cpp` leaves
the sources `Stated`; a one-line comment says so (item 1045). The capture
tool is unchanged.

### The audit (`tests/audit/cleanup_audit.cmake`)

- Every `batch-temperature-bias-v11` becomes `v12`: the header comment's
  accuracy-channels bullet, `fusion-model`'s "The goldens say ..." comment,
  `stored-results`' "one authority: the fusion algorithm string" pair,
  `gnss-holes`' "the goldens carry the current algorithm string" (14) and
  the three documents' counts (`SENSOR_FUSION.md` 2, `DATA_SCHEMA.md` 2,
  `CALCULATIONS.md` 1). The documents end with exactly those counts of the
  full `v12` string; `v11` appears in them only as the short form in history.
- The rule "alternate column names" is untouched: the `_provenance` keys
  do not match it, which is why they are named so.
- Group `inferred-configuration` gains, in the pattern of `gnss-holes`'
  rules: keys ending in `_provenance"` in `src` in `fusionoutput.cpp` alone,
  four lines; quoted `stated` and `inferred` in `src` in that file alone;
  `inferred as` and `defaulted to` in `src` in `fusionsamples.cpp` alone, one
  line each; sixteen `_provenance": "stated"` lines and no `"inferred"` under
  `tests/data/fusion`; `docs/SENSOR_FUSION.md` names `inferred as` and
  `defaulted to` once each and `accel_fs_g_provenance` once. Plant each hit
  once.
- The traceability block: every `1802` phase 1 wrote becomes `1809`.

### The map and the test guide

- `tests/acceptance_map.txt`: items 1803-1809 after 1802, each a
  `# <item> - (<section>) ...` comment with its evidence lines ("Tests"
  below); the header's 1801 entry says 1801-1809. No existing item's
  evidence changes; item 1702's statement stands (a decision below).
- `tests/README.md`: section 9.19's heading, preamble and table to 1809
  with rows 1803-1809 in 9.18's four columns; appendix S entries 3-9, one
  sentence each with the section in front; section 1's rows for
  `tst_fusion_kernel` (the three wordings, the entries), `tst_fusion_golden`
  (the entries `stated` in the four fits) and `tst_fusion_store` (the `v11`
  rows); section 10's texts for `fusion-model` ("the goldens say ...") and
  the `inferred-configuration` group; section 11 as above; item 1043's
  evidence in matrix 9.11 (`v12` now, `v11` added to its history).

### The documents

- `docs/SENSOR_FUSION.md` section 4: `algorithm` (`batch-temperature-bias-v12`);
  the `configuration` entry lists the four values and beside each its
  `_provenance` entry, `stated` or `inferred`. Section 6: the configuration
  rejections keep their texts for a stated key and add `inferred as` for the
  rate and `defaulted to` for the range, worded as what the kernel says when
  told the value was inferred (it reads no session); no sentence claims the
  inference exists, which phase 3 writes in section 3 and completes here
  with the 104 Hz and 70 Hz cases. Section 7 "Stored results": `v12` since
  the kernel records the configuration's provenance in the diagnostics and
  words its rejections by it, which changes the diagnostics and no number;
  `v11` joins the history. Section 8's "whose numbers `v10` and `v11` keep"
  gains `v12`.
- `docs/DATA_SCHEMA.md`: the `records` example (section 11) and the validity
  bullet (section 12) say `v12`, the bullet with the reason and `v11` as
  history. `docs/CALCULATIONS.md` section 17 "Stored results": the same.
  Section 2 of the schema, section 3 of the fusion document and section 5
  of the calculations document do not change: phase 3's.

## Interfaces

Provided to phase 3, exactly:

- `src/fusion/fusion.h`: `enum class ConfigurationSource { Stated, Inferred };`
  `ImuConfiguration::accelFsGSource`, `gyroFsDegSSource`, `accelOdrHzSource`,
  `gyroOdrHzSource`, each `ConfigurationSource::Stated` by default;
  `inline constexpr char Algorithm[] = "batch-temperature-bias-v12";`.
- The texts: `<KEY> states <v> Hz but the IMU is logged at <f> Hz; sensor
  fusion unavailable`; `<KEY> inferred as <v> Hz but the IMU is logged at
  <f> Hz; sensor fusion unavailable`; `<KEY> states +/-<v> <unit> but the
  <sensor> readings lie on <shown>; sensor fusion unavailable`; `<KEY>
  defaulted to +/-<v> <unit> but the <sensor> readings lie on <shown>; sensor
  fusion unavailable`, `<shown>` being `the +/-<r> <unit> lattice` or `no
  range's lattice`; `No datasheet entry for <KEY> = <value>; sensor fusion
  unavailable` unchanged.
- The entries `accel_fs_g_provenance`, `gyro_fs_deg_s_provenance`,
  `accel_odr_hz_provenance`, `gyro_odr_hz_provenance`, each `stated` or
  `inferred`.
- The goldens under `v12` with the four entries `stated`; the range
  1801-1809.

Consumed from phase 1: the scaffolding under "Dependencies" only. The kernel
does not use `attributeIsStored()`.

## Acceptance criteria

The section is the specification's; each is an item of the map.

- **1803** (4, second bullet): the three checks run after every other check
  in the order entry, lattice (accelerometer before gyro), rate
  (`ACCEL_ODR_HZ` before `GYRO_ODR_HZ`), unchanged in rule and tolerance; the
  kernel is told beside each value whether it was stated or inferred
  (`ConfigurationSource`, `Stated` by default) and words the rejection by
  it: a stated key `states` as today, an inferred rate that fails `inferred
  as`, an inferred range that fails `defaulted to` whatever lattice the
  readings show; the `No datasheet entry` rejection unchanged; the three
  verbs spelled once, in the kernel.
- **1804** (4, third bullet): the diagnostics' `configuration` object
  carries beside each of its four values the entry named above, `stated` or
  `inferred`, following the source the kernel was given and nothing else,
  written once; failure diagnostics carry no `configuration`.
- **1805** (4, fourth bullet): `Fusion::Algorithm` is
  `batch-temperature-bias-v12`; a record under `v11`, success or rejection,
  is stale at load and the fit runs again once as a result is needed; the
  arithmetic, the thirty-four outputs, the record's shape and every golden
  channel unchanged: a source enters no number.
- **1806** (6, the fit bullet, the kernel's half): a `Channels` logged at
  70 Hz whose rates are 52 Hz inferred is rejected `ACCEL_ODR_HZ inferred as
  52 Hz but the IMU is logged at 70.0 Hz; sensor fusion unavailable`;
  `reject_rate` (12.5 Hz stated at 100 Hz) with today's `states` text;
  readings on no lattice under an inferred range `defaulted to ... no
  range's lattice`; a fixture with every source `Inferred` fits, its
  channels bit for bit the golden's and its entries `inferred`; the fourteen
  goldens re-captured under `v12`, differing from the previous capture in
  the string and the four `stated` entries alone, proven before the capture
  and recorded in section 11.
- **1807** (6, the record bullet): a record under `v11` is stale at load
  and the fit runs again once, stored anew under the current string with
  the golden's channels; a stored rejection under `v11` likewise, listed
  again with its reason and stored anew.
- **1808** (6, the audit bullet): every rule that pins the string says
  `v12`; the default is still written once (`sensor-configuration` and
  `constant-defaults` unchanged and green); the verbs, keys and texts have
  one writer each; the alternate-column-names rule is unchanged; the range
  1801-1809 declared and complete; the audit and the map check green.
- **1809** (5, the kernel's documents): `docs/SENSOR_FUSION.md` sections 4,
  6, 7 and 8's sentence as described; the `v12` mentions of
  `docs/DATA_SCHEMA.md` and `docs/CALCULATIONS.md` section 17; the test
  guide's capture note, re-capture bullet, three rows, section 10 texts and
  item 1043's evidence; the three documents' `v12` counts the audit's.

## Tests

`tst_fusion_kernel` (`toChannels(fusionFixture(...))`, `rejectedBy()`,
`rejectedWith()`, `fixtureNoise()` as today):

- `latticeCheckFindsTheCoarsestRange`: the `states` cases unchanged; add, on
  `coarse_linear` with `accelFsGSource = Inferred`, `ax[5] = 1.5 *
  accelerometerTolerance` rejected `ACCEL_FS_G defaulted to +/-16 g but the
  accelerometer readings lie on no range's lattice; sensor fusion
  unavailable` and `ax[5] = 8./32768*g` rejected `... defaulted to +/-16 g
  but the accelerometer readings lie on the +/-8 g lattice; ...`; with
  `gyroFsDegSSource = Inferred`, `wz[7] = 35e-3` rejected `GYRO_FS_DEG_S
  defaulted to +/-2000 deg/s but the gyro readings lie on the +/-1000 deg/s
  lattice; sensor fusion unavailable`; the unmodified fixture with both range
  sources `Inferred` passes `requireReadingsOnLattice()`.
- `rateCheckToleratesTenPercent`: the stated calls unchanged; add both rates
  52 and both sources `Inferred` at median `1/70.`: `ACCEL_ODR_HZ inferred as
  52 Hz but the IMU is logged at 70.0 Hz; sensor fusion unavailable`; a
  stated 12.5 accelerometer with an inferred 52 gyro at `1/12.5`:
  `GYRO_ODR_HZ inferred as 52 Hz but the IMU is logged at 12.5 Hz; ...`;
  through the pipeline, `coarse_linear` with `imuTime[i] = imuTime[0] +
  i/70.` and both rates 52 `Inferred`: the exact 70.0 Hz text (the fixes stay
  inside coverage, the axis has no hole); the existing `gyroOdrHz = 52`
  pipeline case unchanged.
- `configurationChecksComeAfterTheOthers`: plus one case with an inferred
  rate and a lattice defect: the lattice's reason first, worded `states`.
- `configurationWithoutEntryIsRejected`: plus one case with every source
  `Inferred`: the `No datasheet entry` text identical.
- `diagnosticsReportTheNoiseModel`: the eight sorted keys; the four entries
  `stated` on `coarse_linear`.
- New `provenanceEntriesFollowTheSources`: `coarse_linear` with all four
  sources `Inferred` through `runPipeline()` succeeds, its entries
  `inferred`, its thirty-three channels bit-identical (`fusionChannel()` over
  `fusionChannelNames()`) to the stated run's and the golden's, the rest of
  its diagnostics equal to the golden's but for the four entries.
- `failureDiagnosticsShape`: `v12`; a rejection's keys still `algorithm`,
  `failure`. The other two `v11` literals move to `v12`.

`tst_fusion_golden`: `successFixturesMatchGolden`,
`rejectionFixturesMatchGolden`, `channelsWriterIsTheInverseOfTheLoader` and
`fixturesAreDeterministic` as they are, against the re-captured goldens; in
exact mode `diagnosticsJson` is byte-equal to the golden's.

`tst_fusion_store`: `recordUnderPreviousAlgorithmIsComputedAgainOnce`
becomes data-driven (`_data` rows `v9` and `v11`, the stamp as the column;
the body unchanged but for the stamp and its comment). New
`rejectionUnderPreviousAlgorithmIsComputedAgainOnce`: `reject_origin` as `r1`
fitted through the checked Roll row as `restoredRejectionShowsBadge` does;
unloaded; `resultVersion` rewritten to `batch-temperature-bias-v11`; shown
again: the record deleted (`staleRecordsDeleted` 1), the fit run once
(`runCount(kFit)` 1), the rejection listed again with `Local origin index
outside GNSS samples`, a record present again under `Fusion::Algorithm`,
nothing further run. `kSolverFailureDiagnostics` says `v12`;
`codeStampChangeDropsRecordOnLoad` keeps `v5`.

`tst_fusion_jobs` (two stamps) and `tst_fusion_session` (two `resultVersion`
assertions and the comment "v10 since the fit publishes the covariance
blocks"): the literal moves to `v12`; nothing else of these files changes.

Map lines, one per piece of evidence in the map's `<item> <target>
<function>` and `<item> audit <group>` forms (every item has an automated
line):

- 1803: `tst_fusion_kernel` `latticeCheckFindsTheCoarsestRange`,
  `rateCheckToleratesTenPercent`, `configurationChecksComeAfterTheOthers`,
  `configurationWithoutEntryIsRejected`; `tst_fusion_golden`
  `rejectionFixturesMatchGolden`; `audit inferred-configuration`.
- 1804: `tst_fusion_kernel` `diagnosticsReportTheNoiseModel`,
  `provenanceEntriesFollowTheSources`, `failureDiagnosticsShape`;
  `tst_fusion_golden` `successFixturesMatchGolden`; `audit
  inferred-configuration`.
- 1805: `tst_fusion_session` `registrationShape`; `tst_fusion_store`
  `recordUnderPreviousAlgorithmIsComputedAgainOnce`,
  `rejectionUnderPreviousAlgorithmIsComputedAgainOnce`; `tst_fusion_golden`
  `successFixturesMatchGolden`, `rejectionFixturesMatchGolden`;
  `tst_fusion_kernel` `fitTraceMatchesGolden`,
  `provenanceEntriesFollowTheSources`; `audit stored-results`; `audit
  gnss-holes`.
- 1806: `tst_fusion_kernel` `rateCheckToleratesTenPercent`,
  `latticeCheckFindsTheCoarsestRange`, `provenanceEntriesFollowTheSources`;
  `tst_fusion_golden` `successFixturesMatchGolden`,
  `rejectionFixturesMatchGolden`, `channelsWriterIsTheInverseOfTheLoader`.
- 1807: `tst_fusion_store` `recordUnderPreviousAlgorithmIsComputedAgainOnce`,
  `rejectionUnderPreviousAlgorithmIsComputedAgainOnce`.
- 1808: `audit stored-results`, `audit gnss-holes`, `audit
  sensor-configuration`, `audit constant-defaults`, `audit
  inferred-configuration`.
- 1809: `audit gnss-holes`, `audit inferred-configuration`.

The golden lines of 1806 (the capture's proof) and the audit lines of 1809
are floors, as 9.18 says of 1705: the reviewer reads the capture note and the
paragraphs; 9.19's preamble says so.

## Decisions

- **`requireStatedRates` becomes `requireConfiguredRates`.** It judges
  stated and inferred rates under one rule, and "configured" is the word
  `fusion.h` and `SENSOR_FUSION.md` section 3 use for either origin. Only
  `fusionsamples.*`, `fusion.cpp` and `tst_fusion_kernel.cpp` spell it.
- **The entries are named `*_provenance`.** The audit's "alternate column
  names" rule over `src` and `tests` refuses any literal ending in
  `_source"`, the goldens included; it is a column-naming convention and
  gains no path exclusion for the kernel, a test or the goldens, so the
  overview names the entries to stay clear of it. The new group rules keep
  the keys confined to `fusionoutput.cpp` in `src`.
- **Item 1702 is not restated.** Its statement says the string "is `v11`";
  1306 and 1407 say the same of `v8` and `v9` and were left as history when
  `v10` and `v11` arrived. Item 1043's evidence, which says "now", is
  updated. The overview lists no restatement for this phase.
- **Section 6 between the phases.** The registration sets only `Stated` in
  this tree, so section 6 describes the wordings as what the kernel says
  when told a value was inferred, and section 3 keeps the constant defaults
  until phase 3; no transient sentence phase 3 would have to delete.
- **Section 8's sentence gains `v12`.** The overview lists it among the
  references without assigning it; leaving it would make it false.
- **One data-driven store test and one new function.** A `v11` row beside
  the `v9` one proves the success case with no new code; the rejection needs
  its own function. Phase 3's retried rejection that succeeds is a third
  case, there.
- **A source never enters the arithmetic**, proven on the kernel by
  `provenanceEntriesFollowTheSources`, which is also why this phase
  re-captures texts and nothing else.

ready
