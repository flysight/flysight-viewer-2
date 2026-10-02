# Phase 1: Configuration attributes

## Purpose

The specification's section 5 up to the kernel's door, and nothing past it:
the nine header keys are read, validated, stored and carried with the session;
the four IMU keys become inputs of the fit with constant defaults, and reach
the kernel inside `Fusion::Channels`, which reads nothing of them yet. No
number of the fit changes, so every existing golden passes unchanged, exact
mode included. That is the specification's plumbing check (overview
decision 1, clause 61). It is one phase because phase 2 needs a build where
the configuration arrives but the noise model is still the old one, and the
goldens are what proves that.

The phase also opens the acceptance range 1001-1065 (decision 18) and adds
the audit group `sensor-configuration`.

Clauses owned: 1, 2, 3, 4, 5, 6, 7, 50, 61 (items 1001-1007, 1050, 1061).
Earlier items restated "(as amended)": 221, 847, 930 (see Tests).

## Dependencies

- **Depends on nothing.** It starts at `b69eddd` on
  `store-requested-calculations`: `Fusion::Algorithm` is
  `batch-temperature-bias-v5`, `fitInputs()` holds twenty-two inputs, and the
  goldens in `tests/data/fusion/` are the capture of 2026-09-30.
- **Blocks phase 2**, which consumes exactly the names under "Interfaces".

## What changes

### The vocabulary: `src/sensorconfiguration.h` / `.cpp` (new, `flysight_model`)

This is the one authority on the keys (decision 2), as
`src/conversion/schematable.h` is "the one authority on recorded data
schemas". It sits in `flysight_model` because the importer (`flysight_core`)
and the fusion registration (`flysight_fusion`, which links `flysight_model`
and not `flysight_core`) both read it. Qt Core only, no logging, no state.
Add it to the `flysight_model` list in `src/CMakeLists.txt` with a comment
saying why it is there, as `attributeregistry.cpp` has one.

It holds:

- the nine key names as `constexpr char` arrays in namespace
  `FlySight::SensorConfiguration` (names under Interfaces). The namespace
  comment says in prose which file carries each key (seven in `SENSOR.CSV`,
  two in `TRACK.CSV`). No code reads the file, so it is not held as data.
- the value forms (decision 5), checked after surrounding whitespace is
  trimmed, as `Schema::parseVersion` trims. The recorded text is stored as it
  is.
  - `ACCEL_FS_G`: exactly `2`, `4`, `8`, `16`.
  - `GYRO_FS_DEG_S`: exactly `250`, `500`, `1000`, `2000`.
  - `ACCEL_ODR_HZ`: exactly `1.6`, `12.5`, `26`, `52`, `104`, `208`, `416`,
    `833`, `1666`, `3333`, `6666`.
  - `GYRO_ODR_HZ`: the same list without `1.6`.
  - `GNSS_MODEL`: exactly `portable`, `stationary`, `pedestrian`,
    `automotive`, `sea`, `airborne_1g`, `airborne_2g`, `airborne_4g`,
    case-sensitive.
  - `BARO_ODR_HZ`, `HUM_ODR_HZ`, `MAG_ODR_HZ`, `GNSS_RATE_HZ`: a plain
    positive decimal, digits with an optional fraction (`[0-9]+(\.[0-9]+)?`),
    with no sign and no exponent, and above zero.
  - Anything else is malformed: `16.0`, `+16`, `016` (for a listed key), the
    empty value, `$VAR,<key>` with no value, `abc`, `1e2`, `nan`, `inf`,
    `0`, `Portable`, and `1.6` for the gyro.
- a validation the importer and the exporter call, and its message, in the
  form of `Schema::unsupportedMessage`:
  `Unsupported ACCEL_FS_G '16.0' (supported: 2, 4, 8, 16)`. A listed key
  prints its list in the order above (`GNSS_MODEL` its eight names, comma
  separated). A free-form key prints
  `(supported: a positive decimal number)`. These texts are observable
  contract: the tests and `docs/DATA_SCHEMA.md` quote them.
- the default of the four IMU keys as text (`16`, `2000`, `12.5`, `12.5`) and
  the firmware version it describes, `v2023.09.22`, written once. The other
  five keys have no default: "no dynamic model, no rate".

The filters have no key (clause 5). Their fixed setting is a documented fact
of the default (Documents, below). It is not a value in code in this phase:
nothing reads it before phase 2's datasheet unit.

### The importer: `src/dataimporter.cpp`

`publish` already stores every header attribute verbatim, so storing needs no
change. The validation joins the `SCHEMA_VER` check after the header
("Validate the declared schema before reading any data row"). `SCHEMA_VER`
keeps its precedence. After it, the first malformed configuration value in
file order is the error: `m_lastError` is the vocabulary's message, the file
is rejected, and nothing of it is published (the staging guarantee). The
check runs wherever the key appears, in either file, as `SCHEMA_VER`'s does:
the importer does not identify files by name. An absent key stores nothing.
The importer never writes a default (`docs/CALCULATIONS.md` section 5:
"Defaults are calculations"), and `applyCreationDefaults` is not touched.
FS1 files have no `$VAR` and are unaffected.

### The exporter, the merge, the session

- `src/dataexporter.cpp`: `validateSchema` already refuses a stored
  `SCHEMA_VER` the importer would reject. Refuse a stored configuration value
  the importer would reject in the same way, with the same message, before
  the file is opened. The `dataexporter.h` contract ("Never writes a file
  that DataImporter would reject") and `docs/DATA_SCHEMA.md` section 9 say
  so. Keep one validation pass and do not duplicate the grammar: the
  vocabulary is the only place that knows it.
- `src/sessionmerge.cpp`: no code change. The keys are header attributes, so
  the conflict rule of `docs/DATA_SCHEMA.md` section 8 applies as it stands:
  `TRACK.CSV`'s and `SENSOR.CSV`'s keys merge cleanly, and two files stating
  different values conflict with the replace-session hint, not the schema
  hint.
- `src/sessiondata.h`: no change. These are recorded keys, not `SessionKeys`.

### The registration and the kernel's input

- `src/fusion/fusion.h`: add `struct ImuConfiguration` and the member
  `Channels::imuConfiguration` (Interfaces). Use `<limits>` for the quiet
  NaN; the header stays Qt Core and std only. Its comment states the units
  (g, deg/s, Hz, Hz: the keys' values) and that a member is NaN when its
  attribute is not a number. `run()`'s "Pure: a function of `channels`" is
  unchanged.
- `src/fusion/fusionregistration.cpp`:
  - `fitInputs()` appends `ACCEL_FS_G`, `GYRO_FS_DEG_S`, `ACCEL_ODR_HZ`,
    `GYRO_ODR_HZ` as `CalcInput::attribute`, after the four origin
    attributes, for twenty-six inputs. The five other keys are not inputs
    (decision 4, clause 6).
  - `channelsFrom()` fills `imuConfiguration` from those four attributes.
    `toDouble(&ok)` gives the value, or NaN when it is not a number, beside
    the origin index's not-a-number rule. That is the adapter's only new
    logic.
  - Four constant defaults, `builtin.default.<KEY>`, registered through
    `Calculations::addConstantDefault` with the vocabulary's text, as
    `registerOrientation()` registers `_ORIENTATION`'s. The value is a
    `QString`, like the orientation token and like what the importer stores,
    so a stored `16` and the default `16` have the same text. They go first
    in `registerFusionCalculations` ("the vocabulary before its reader"), in
    input order. Nothing here spells a key or a default: both come from the
    vocabulary.
  - Update the comments that count: "the eighteen measurement inputs",
    "then the four origin attributes". Update `fusionregistration.h` the
    same way: the `fitInputs()` and `channelsFrom()` comments, and the
    registration list, which gains the four defaults.
- The kernel (`src/fusion/` other than `fusion.h` and `fusionregistration.*`)
  does not change. `prepareInput` does not read the new member, and the
  diagnostics' `input` audit (`inputAudit` in `inputadapter.cpp`) does not
  gain it, because that would change the goldens. The configuration in the
  diagnostics is phase 2's (clause 40).
- `tests/fusion_runner.cpp` needs no code change: `inputDump` iterates
  `fitInputs()`, so `--dump-inputs` lists the four attributes with their
  effective values. That is the default text when the files do not state
  them. A malformed key is an import failure (exit 3) through `parseFile`.

### What must not change

- `Fusion::Algorithm` (`v5`), the seventeen outputs, the diagnostics,
  `tests/data/fusion/*` (no re-capture), and `tests/fusion/fusionfixtures.*`.
  The capture records those two files' hashes, so they are not touched, not
  even their comment "twenty-two inputs the fit consumes". Phase 2 rewrites
  them.
- No file under `src/` changes except: `sensorconfiguration.*` (new),
  `CMakeLists.txt`, `dataimporter.cpp`, `dataexporter.cpp` / `.h`,
  `fusion/fusion.h`, `fusion/fusionregistration.*`. Nothing above the
  registration changes (clause 7): no plot, demand, session-model, logbook or
  attribute-registry change. The keys are not logbook attributes.
- `toChannels()` (`tests/fusion/fusiongolden.cpp`) is unchanged, so a direct
  kernel run gets an all-NaN configuration while a session run gets the
  defaults. Both must give the same bits, which is part of the check.
- Expected and accepted: a fit stored before this phase is stale at its next
  load. Its record lacks the four new lookups (leaves and resolutions,
  `docs/CALCULATIONS.md` 15.8), so it is fitted again once. Nothing is added
  to preserve it; phase 2 bumps the version anyway.

### Documents (same change)

- `docs/DATA_SCHEMA.md` section 2: a subsection on the configuration keys
  beside the `$VAR` grammar and before `SCHEMA_VER`. It gives each key, the
  file that carries it, its unit, its value form, and the import error with
  one quoted message. It also holds the default, written here and only here
  in the documents: firmware v2023.09.22, +/-16 g, +/-2000 deg/s, 12.5 Hz for
  both sensors, no dynamic model, no rate. Beside the default goes the
  firmware's fixed filter setting: the gyro's LPF2 at the cutoff of its
  12.5 Hz rate, 4.2 Hz (DS12140 Table 18), no LPF1, and the accelerometer at
  its ODR bandwidth. Then: the default is a calculation and is never stored
  or written; the keys are written by the firmware, and every recording on
  disk lacks them. Also add a bullet to the rejection list, a sentence to
  section 8 (the keys follow the conflict rule), and a clause to section 9's
  save-failure sentence.
- `docs/SENSOR_FUSION.md` section 3: the input list with the four keys, the
  count "twenty-six", and a short "Configuration" paragraph. The paragraph
  says the four keys come from `SENSOR.CSV` or their constant defaults
  (linking `DATA_SCHEMA.md` section 2, not restating the default), are
  always available, reach the kernel in `Channels::imuConfiguration`, and do
  not yet change the model. Section 7: "twenty-two inputs" becomes
  "twenty-six". The audit bans "twenty-two" in this file from now on.
- `docs/CALCULATIONS.md` section 5: add the four configuration defaults to
  the named constant defaults. Section 17:
  - "Eight calculations" becomes twelve, with the table's four new rows;
  - the inputs block, and "the 22 below" in the fit's row;
  - the adapter paragraph (the NaN rule);
  - the stored-results paragraph: the 26 inputs, and the configuration
    attributes among the leaves and their defaults among the resolutions,
    so a file merged later that states a key makes a stored fit stale
    (decision 3);
  - the missing-input sentence: the four are never missing.

## Interfaces

### Provided (phase 2 consumes these names unchanged)

- `src/sensorconfiguration.h`, namespace `FlySight::SensorConfiguration`:
  - `constexpr char AccelFsG[] = "ACCEL_FS_G"`, `GyroFsDegS[]`
    (`GYRO_FS_DEG_S`), `AccelOdrHz[]`, `GyroOdrHz[]`, `BaroOdrHz[]`,
    `HumOdrHz[]`, `MagOdrHz[]`, `GnssModel[]`, `GnssRateHz[]`;
  - `constexpr char FirmwareVersion[] = "v2023.09.22"`, the firmware the
    default describes;
  - a validation and its message, and the default text of the four IMU keys.
    The implementer names these; phase 2 reads only the key constants, for
    the rejection reasons that name a key (overview decision 9).
- In `src/fusion/fusion.h`:

  ```cpp
  struct ImuConfiguration {
      double accelFsG, gyroFsDegS, accelOdrHz, gyroOdrHz;  // g, deg/s, Hz, Hz; quiet NaN unless set
  };
  // in Channels:
  ImuConfiguration imuConfiguration;
  ```

  Every member's default initializer is
  `std::numeric_limits<double>::quiet_NaN()`. The kernel reads nothing of it
  in this phase.
- `Fusion::fitInputs()`: the eighteen measurements, the four origin
  attributes, then `ACCEL_FS_G`, `GYRO_FS_DEG_S`, `ACCEL_ODR_HZ`,
  `GYRO_ODR_HZ`.
- `Fusion::channelsFrom()` filling `imuConfiguration` by the NaN rule.
- `builtin.default.ACCEL_FS_G`, `builtin.default.GYRO_FS_DEG_S`,
  `builtin.default.ACCEL_ODR_HZ`, `builtin.default.GYRO_ODR_HZ`, registered by
  `Fusion::registerFusionCalculations`, values `"16"`, `"2000"`, `"12.5"`,
  `"12.5"`.
- `fusion_runner --dump-inputs` with twenty-six lines.
- Acceptance mechanics: items 1001-1065 allowed by the audit, appendix K and
  section 9.11 in `tests/README.md`, the map's head paragraph, and the
  explicit completeness list in `cleanup_audit.cmake`, which phase 2 appends
  to. Audit group `sensor-configuration`.

### Consumed

Nothing from another phase.

## Acceptance criteria

1. `src/sensorconfiguration.*` exists in `flysight_model`, holds the nine
   names, the value forms above, the four defaults and `v2023.09.22`, and is
   the only file in `src/` that spells a key as a string literal or that
   firmware version (clauses 1, 4; audit).
2. A `SENSOR.CSV` with the seven keys and a `TRACK.CSV` with the two import.
   Every value is stored as the recorded `QString`, with surrounding
   whitespace kept (clauses 2, 50).
3. A malformed value of any key, in either file, fails the import with
   exactly the vocabulary's message, and the target session is untouched
   (`nothingPublished`; `failedImportLeavesTargetUntouched`'s form). With a
   malformed `SCHEMA_VER` too, the `SCHEMA_VER` message wins (clauses 3, 50).
4. A file without the keys yields a session without them. With the fusion
   registration, the session reads `"16"`, `"2000"`, `"12.5"`, `"12.5"` from
   the four `builtin.default.*` calculations (no inputs, one output each),
   and a stored key wins over its default (clauses 4, 50).
5. Merging: the keys of the two files of a recording merge in either order.
   Two files stating different values of one key conflict, naming the key
   and both values. Save and reload keeps every key's text byte for byte. A
   session holding a malformed value cannot be saved and the previous file
   is left intact (clause 2).
6. `fitInputs()` is the twenty-six of Interfaces, and none of
   `GNSS_MODEL`, `GNSS_RATE_HZ`, `BARO_ODR_HZ`, `HUM_ODR_HZ`, `MAG_ODR_HZ` is
   among them or is named in `src/` outside the vocabulary (clauses 6, 7).
7. `channelsFrom()` gives the four numbers for stated and defaulted
   sessions, and NaN for a stored non-number. A fit on a session stating a
   configuration other than the default is bit-identical, channels and
   diagnostics, to the same fit with the default and to the golden
   (clauses 7, 61).
8. Storing a configuration key on a session with a stored fit drops the fit
   and its record, like any declared input (decision 3).
9. Every existing golden test passes unchanged, the `_exact` variants
   included: `tst_fusion_golden`, `tst_fusion_kernel`, `tst_fusion_session`,
   `tst_fusion_jobs`, `tst_fusion_rows`, `tst_fusion_store`,
   `tst_fusion_runner`. `git diff` shows nothing under `tests/data/fusion/`
   or in `tests/fusion/fusionfixtures.*`, and `Fusion::Algorithm` is still
   `batch-temperature-bias-v5` (clause 61).
10. The documents carry what Documents lists, the default stated once.
    `docs/SENSOR_FUSION.md` contains neither "twenty-one" nor "twenty-two"
    (clauses 1, 4, 5, 7).
11. The whole suite is green in `build-agent/`, Release, run sequentially;
    `audit_cleanup` is green with items 1001-1007, 1050 and 1061 complete.

## Tests

### New executable: `tst_sensor_configuration` (label `core`)

Mirror `tst_schema_units`: every expectation is a literal. Register it with
`flysight_add_test` and give it a row in `tests/README.md` section 1.
Functions, named here so that the map can cite them:

- `keysAndValueForms`: exactly nine keys, each constant's text; every listed
  value of every listed key accepted, with surrounding whitespace too.
  There is no filter key (clauses 1, 5).
- `malformedValues_data` / `malformedValues`: the malformed list of "The
  vocabulary", per key where it applies, including `1.6` for the gyro rate
  and `0` and `-1` for the free-form keys (clause 3).
- `unsupportedMessage`: the literal texts, a listed key, `GNSS_MODEL` and a
  free-form key (clause 3).
- `defaultIsTheFirmwareConfiguration`: the four default texts are valid
  values, the version is `v2023.09.22`, and the other five have no default
  (clause 4).

### Amended and added functions

- `tst_importer`:
  - add `configurationStoredAsRecorded`: all nine keys, both files, verbatim
    `QString`, `" 16"` kept;
  - add `rejectsMalformedConfiguration_data` / `rejectsMalformedConfiguration`,
    mirroring `rejectsUnsupportedSchema`: `importFile` and `readFile`, a
    track-file row, and the `SCHEMA_VER`-first row;
  - extend `neverStampsSchema` or add `neverStampsConfiguration`;
  - update the file comment.
- `tst_session_merge`: add `configurationFollowsConflictRule` (clean merge of
  the two files' keys; a differing `ACCEL_FS_G` conflicts with the
  replace-session hint).
- `tst_import_merge`: add `configurationTravelsWithTheSession` (either order
  through `SessionImport`, the nine keys on the session).
- `tst_persistence_roundtrip`: add `configurationAttributesRoundTrip`, and
  `malformedConfigurationIsNotSaved` mirroring `unsupportedSchemaIsNotSaved`.
- `tst_fusion_session`:
  - `registrationShape`: the last twelve ids, the four defaults first; the 26
    inputs as a literal list; the outputs and `v5` unchanged;
  - `inputsAreBitIdenticalToFixture`: comment and count wording, plus the
    four defaults read;
  - add `configurationDefaults` (the four descriptors' shape and values; a
    stored value wins);
  - add `configurationReachesTheKernel` (criterion 7, against the golden).
- `tst_fusion_runner::successMatchesDirectRun`: `dump.size()` is 26, the four
  configuration lines hold the default text, and the comment's
  "twenty-two" changes.
- `tst_fusion_store::dependencyEditDropsRecord_data`: a row `ACCEL_FS_G` →
  `"8"`. The precondition `getAttribute(key) != value` rules out `"16"`.
- `tests/fusion/fusionsessions.h` / `.cpp`: comments only ("the 22 declared
  inputs"). The configuration is read from its defaults there, which is the
  default path. Storing it is phase 2's.

### Audit (`tests/audit/cleanup_audit.cmake`)

- New group `sensor-configuration`, placed beside `constant-defaults`:
  - `expect_only` of the quoted key literals
    (`"(ACCEL_FS_G|GYRO_FS_DEG_S|ACCEL_ODR_HZ|GYRO_ODR_HZ|BARO_ODR_HZ|HUM_ODR_HZ|MAG_ODR_HZ|GNSS_MODEL|GNSS_RATE_HZ)"`)
    in `src`, allowed only in `^src/sensorconfiguration\.(h|cpp)$`;
  - `expect_count` of `v2023\.09\.22` in `src`, 1 line, and `expect_only` of
    it in the same files;
  - `expect_only` of the five unused constants (`BaroOdrHz|HumOdrHz|MagOdrHz|GnssModel|GnssRateHz`,
    with `WB_START` / `WB_END`) in `src`, allowed only in the vocabulary:
    nothing uses them.
  - Each rule gets an "Allow:" comment, and you plant a hit once to prove
    each rule.
- `fusion-model`: the description rule's banned word becomes `twenty-two`,
  and its comment says "twenty-six inputs".
- File head: a bullet for this specification's configuration attributes
  (items 1001-1065).
- Traceability block:
  - the comment and the range check gain `1001-1065`, the violation text
    included;
  - add a completeness loop over an explicit list,
    `1001 1002 1003 1004 1005 1006 1007 1050 1061`, with a comment saying
    that each phase appends its items and the last replaces the list with
    the range.
- `tests/README.md` section 10: a bullet for the new group. The
  `fusion-model` bullet changes if it names the banned word.

### Traceability

- `tests/README.md` appendix K: the overview's clause list 1-65, copied
  without the phase tags, under a head paragraph in appendix J's form. The
  paragraph says that the specification numbers no clauses, that item =
  1000 + number, that sections 1-4 carry no item, and which clauses are as
  settled (6, 8, 9, 10, 11, 25, 26, 31, 44, 61).
- Section 9.11, "The documented noise model and the accuracy, part 1 (items
  1001-1065)", in 9.10's form:
  - an intro saying for each as-settled clause how it reads the letter,
    from overview decisions 4, 6, 7, 8, 15, 17, 12 and 1 (only four keys are
    fit inputs; the step and lattice are the datasheet's sensitivity, in
    effective units, on the readings the kernel receives; a 10 % rate
    tolerance; the joint covariance of adjacent fixes; a 2.5 s window;
    captures at the end of each phase that changes numbers; the plumbing
    check is the end of phase 1);
  - the amendments;
  - table rows for phase 1's nine items.
- `tests/acceptance_map.txt`: the head paragraph for 1001-1065 ("eleven
  specifications, eleven item ranges", "the eleven ranges", "sections 9.1 to
  9.11"). The paragraphs of 201-247, 801-863 and 901-940 each gain "It is
  amended by the specification of 1001-1065: item N is stated as amended".
  The lines:
  - 1001 `tst_sensor_configuration keysAndValueForms`; `audit sensor-configuration`
  - 1002 `tst_importer configurationStoredAsRecorded`; `tst_session_merge configurationFollowsConflictRule`; `tst_import_merge configurationTravelsWithTheSession`; `tst_persistence_roundtrip configurationAttributesRoundTrip`
  - 1003 `tst_importer rejectsMalformedConfiguration`; `tst_sensor_configuration malformedValues`, `unsupportedMessage`; `tst_persistence_roundtrip malformedConfigurationIsNotSaved`
  - 1004 `tst_sensor_configuration defaultIsTheFirmwareConfiguration`; `tst_fusion_session configurationDefaults`; `tst_importer neverStampsConfiguration` (or the extended function); `audit sensor-configuration`
  - 1005 `tst_sensor_configuration keysAndValueForms`; `audit sensor-configuration`
  - 1006 `tst_importer configurationStoredAsRecorded`; `tst_fusion_session registrationShape`; `audit sensor-configuration`
  - 1007 `tst_fusion_session registrationShape`, `configurationReachesTheKernel`; `tst_fusion_runner successMatchesDirectRun`; `tst_fusion_store dependencyEditDropsRecord`; `audit solver-confinement`
  - 1050 `tst_importer configurationStoredAsRecorded`, `rejectsMalformedConfiguration`; `tst_fusion_session configurationDefaults`; `tst_fusion_runner successMatchesDirectRun`
  - 1061 `tst_fusion_golden successFixturesMatchGolden`, `rejectionFixturesMatchGolden`; `tst_fusion_kernel fitTraceMatchesGolden`; `tst_fusion_session configurationReachesTheKernel`; `tst_fusion_runner successMatchesDirectRun`
- Amended items, restated in the map's comment line, the 9.x row and the
  appendix item, each "(as amended)", with the appendix's head paragraph
  naming the amendment:
  - **221** (9.3, appendix C 21): `IMU/temperature` is a required input of
    the fit (the twenty-second when it was added; the fit has twenty-six
    since the specification of 1001-1065). The rest as stated. Its row's
    evidence says "26 inputs".
  - **847** (9.9, appendix I 47): the fusion plots leave the kernel and the
    fit calculation as they are, except that the specification of 1001-1065
    adds the four configuration inputs. The rest as stated; phase 2 amends it
    again for the algorithm string and the goldens.
  - **930** (9.10, appendix J 30): the registration and everything above it
    unchanged by the reconstruction; since the specification of 1001-1065
    the registration also declares the four configuration inputs and
    registers their constant defaults.
  - The appendices were searched for other items that state twenty-two
    inputs, the input list, or an unchanged registration. 229 and 235 still
    hold, since the result contract is the outputs; 912 and 925 are phases 4
    and 2.
- `tests/README.md`:
  - section 1 rows: `tst_importer`, `tst_session_merge`, `tst_import_merge`,
    `tst_persistence_roundtrip`;
  - `tst_fusion_session`: "eight registrations, the fit with 22 inputs"
    becomes twelve and 26;
  - `tst_fusion_runner`, `tst_fusion_store`, and the new row;
  - section 11 "Fusion sessions": "twenty-two effective inputs" becomes the
    measurements and origin bit-identical, the configuration from its
    defaults.

## Decisions

1. **Whitespace is trimmed before matching, as for `SCHEMA_VER`.** The text
   is stored verbatim, and `QString::toDouble` in `channelsFrom()` ignores
   surrounding whitespace too, so `" 16"` is 16 everywhere.
2. **The free-form grammar is `[0-9]+(\.[0-9]+)?` and above zero.** The
   message texts are fixed above: they are contract, quoted by tests and
   docs.
3. **A key is validated in either file**, as `SCHEMA_VER` is. The importer
   does not know file names. Which file carries a key is prose in the
   vocabulary and the documentation, not data, because nothing reads it.
4. **`SCHEMA_VER` keeps its precedence**; then the first malformed
   configuration value in file order is the error.
5. **The exporter refuses a malformed stored value.** `DATA_SCHEMA.md`
   section 9 says "Viewer never writes a file it could not read back", and
   once the importer rejects malformed values, the exporter must not write
   them.
6. **The defaults are `QString` text**, registered first in
   `registerFusionCalculations`, in input order.
7. **The keys are not registered as logbook attributes.** Nothing in the
   specification shows them, and "nothing above the registration changes".
8. **The default is written in `DATA_SCHEMA.md` section 2 only**; the other
   documents link to it (one authority per fact). The filter setting is
   documented there with it, and is not a value in code until phase 2 needs
   it.
9. **The vocabulary gets its own test executable**, `tst_sensor_configuration`,
   because it is a new unit of `flysight_model`. `tst_schema_units` is about
   the conversion layer's two tables.
10. **`tests/fusion/fusionfixtures.*` are not touched**, so the hashes in
    `capture.json` stay true. Their stale count goes with phase 2's rewrite.
11. **Amended items: 221, 847, 930.** 847 is amended for the inputs only,
    and phase 2 restates it for the version and the goldens.

Ready.
