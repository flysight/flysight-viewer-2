# Phase 8: Documentation, end-to-end acceptance, and cleanup audit

## Overview

This is the integration phase. It adds no feature. It (1) reconciles the few
places where the phase documents 1-7 disagree with each other, (2) closes the
acceptance clauses that no earlier test covers - most importantly the
session-level randomized idempotency oracle across reads, edits, merges,
preference changes, and registry changes (spec 7.4, acceptance 10) and the
end-to-end workflow of acceptance 19, (3) turns "no old mechanism remains" into
a repeatable, automated audit, (4) writes the final documentation
(`docs/DATA_SCHEMA.md`, `docs/CALCULATIONS.md`, plugin README deltas,
`tests/README.md`, root `README.md`), and (5) finalizes CI. Every acceptance
item 1-19 ends this phase mapped, clause by clause, to a named passing test.

## Dependencies

- **Depends on:** Phases 5, 6, 7 (and transitively 1-4). Both parallel tracks
  (5 -> 6 and 7) must be merged into `schema-and-calculations` before Task 8.1.
- **Blocks:** None (final phase).
- **Assumptions (names used verbatim from the earlier documents):**
  - Branch `schema-and-calculations`, created from `v2026.04.1`; nothing from
    `master` commit `4668f48` exists on it. **The working tree the planner read
    is `master`**: `docs/DATA_SCHEMA.md` and `tests/` there are the superseded
    versions. On the branch, `docs/` does not exist until Task 8.6 and
    `tests/README.md` is the file grown by Phases 1-7. Read the superseded
    schema text only with `git show master:docs/DATA_SCHEMA.md`.
  - The 22 test targets of Phases 1-7 exist and pass: `tst_harness`,
    `tst_smoke`, `tst_calcregistry`, `tst_calcengine`, `tst_calcengine_safety`,
    `tst_calcengine_oracle`, `tst_builtins_golden`, `tst_builtins_engine`,
    `tst_session_engine`, `tst_session_model_engine`, `tst_schema_units`,
    `tst_conversion_engine`, `tst_importer`, `tst_source_layer`,
    `tst_csvformat`, `tst_persistence_roundtrip`, `tst_logbook_index`,
    `tst_column_cache`, `tst_session_merge`, `tst_import_merge`,
    `tst_import_batch`, `tst_python_bridge`.
  - Test support: `FlySightTest::TestEnvironment` (`rootPath`, `newTempDir`,
    `useFreshLogbook`, `reopenLogbook`, `sessionsDir`, `indexPath`,
    `registerBuiltIns`, `resetPreferencesToDefaults`), `waitForIdle`,
    `FLYSIGHT_TEST_MAIN`, `flysight_add_test(<name> SOURCES .. [LIBS ..] [ENVIRONMENT ..])`,
    `Fs2FileBuilder`, `Fs1FileBuilder`, `Fixtures::sensorFile/trackFile`,
    `DescentFixture::{T0, trackFile, sensorFile, load}`, `goldenValues()`,
    `readFileBytes`, `writeFile`, `LogbookManager::reset()`.
  - Engine / session API: `CalculationEngine::{verifyAgainstFresh, evaluateFresh,
    sameValue, request, runCount, runCountForInstance, totalRunCount,
    resultStatus, cachedState, scopeDepth, cycleCount, undeclaredReadCount,
    setInvalidationListener}`, `CalculationRegistry::{instance,
    registerCalculation, unregister, registeredIds, hasCandidateFor,
    staticDependencies, declaredPreferenceKeys}`,
    `CalculationDescriptor::allowSourceInputs`,
    `SessionData::{getAttribute, getMeasurement, effectiveUnit,
    sourceMeasurement, sourceUnit, hasSourceMeasurement, sourceData,
    setSourceMeasurement, mergeSourceData, setAttribute, removeAttribute,
    setUnit, storedAttribute, calculationEngine}`,
    `AltitudeMarkerManager::{makeDescriptor, calculationId, registerAll, refresh}`.
  - Persistence / merge API: `CsvFormat::*`, `DataExporter::{exportSession,
    toBytes}`, `DataImporter::{parseFile, importFile, readFile,
    applyCreationDefaults}`, `ParsedFile`, `SessionMerge::{plan, apply}`,
    `MergePlan`, `MergeResult`, `SessionModel::{mergeSessions (both overloads),
    updateAttribute, removeAttribute, sessionRef, rowAt, populateFromIndex,
    startColumnWorker, flushPendingInvalidations, columnWorkStats,
    resetColumnWorkStats}`, `SessionImport::{importFiles, failureMessage,
    BatchResult}`, `LogbookManager::{initialize, saveSession, loadSession,
    loadSessionRaw, flushIndex, cachedValuesForSession, cachedColumnValues,
    lastAccessedMap, cacheEnvironment}`, `FlySight::CalculationCompatibilityVersion`,
    `calculationEnvironmentFingerprint()`, `Schema::{AttributeKey, parseVersion,
    unsupportedMessage}`.
  - Python: `python_plugins/README.md`, `python_plugins/examples/imu_tilt.py`,
    `tests/python_plugins/t_*.py`, options `FLYSIGHT_BUILD_TESTS`,
    `FLYSIGHT_BUILD_PYTHON_TESTS`.
  - The only remote is `upstream`; there is no `gh` CLI. **Nothing is pushed
    in this phase unless Michael asks**; workflow edits are therefore
    unverified until he pushes (Task 8.10).

---

## Acceptance traceability matrix

"Existing" tests are defined by the phase named; **NEW** tests are defined in
this document (task in brackets). A clause counts as covered only when a named
test function asserts it with literal expectations.

| # | Clause | Test target :: function | Phase |
|---|---|---|---|
| 1 | effective 71.68 / -143.36 / 0 | `tst_source_layer::unmarkedFileIsCorrected`; `tst_conversion_engine::legacyGyroCorrected`; `tst_smoke::importFs2Sensor` | 4 |
| 1 | source values + recorded unit text | `tst_source_layer::unmarkedFileIsCorrected`, `sourceAccessNeverComputes` | 4 |
| 1 | `SCHEMA_VER` absent from session | `tst_importer::neverStampsSchema`; `tst_source_layer::unmarkedFileIsCorrected` | 4 |
| 2 | schema-2 file unchanged | `tst_source_layer::schema2FileIsLiteral`; `tst_conversion_engine::schema2Unchanged` | 4 |
| 3 | `3` / `abc` rejected with error | `tst_importer::rejectsSchema3`, `rejectsSchemaAbc`, `rejectsSchemaEmpty`, `rejectsSchemaNoValue` | 4 |
| 3 | existing session with that `SESSION_ID` unmodified | `tst_importer::failedImportLeavesTargetUntouched`; `tst_import_merge::rejectedSchemaLeavesSession_loaded` / `_unloaded` | 4, 6 |
| 4 | `g`, `gauss`, source retained, custom unit passes through | `tst_source_layer::unitNormalization`; `tst_conversion_engine::unitNormalization`, `internalLabelsAreIdentity`; `tst_schema_units` | 4 |
| 5 | samples bit-identical | `tst_persistence_roundtrip::samplesAreBitIdentical`; `tst_csvformat::roundTripBits`, `roundTripSweep`, `negativeZero` | 5 |
| 5 | unit text + all header attributes preserved | `::unitsAndHeaderAttributesPreserved`, `::typedAttributesRoundTrip` | 5 |
| 5 | `SCHEMA_VER` only if recorded | `::schemaVerOnlyIfRecorded` | 5 |
| 5 | effective values identical before / after | `::effectiveValuesUnchanged`; **NEW** `tst_session_oracle::modelSequences` (reload check) [8.3] | 5, 8 |
| 5 | repeating the cycle changes nothing | `::secondCycleIsByteIdentical`, `::logbookSaveReloadCycle` | 5 |
| 5 | warm and cold caches produce the same file | `::warmAndColdCachesSameFile` (in-memory writer); **NEW** `::fileAndMemoryWritersAgree` (the streaming file writer is a second code path) [8.2]; **NEW** `tst_workflow::warmModelSaveEqualsColdExport` (the model's save path) [8.4] | 5, 8 |
| 6 | released file loads, gyro corrected once | `tst_source_layer::releasedLogbookFormatLoads` | 4 |
| 6 | saving does not rescale or relabel | `tst_persistence_roundtrip::releasedLogbookSaveKeepsBytes`, `releasedLogbookBackfillIsAdditive`; **NEW** `tst_workflow::releasedLogbookUpgrade` (through `SessionModel`) [8.4] | 5, 8 |
| 7 | either order, same result, loaded | `tst_import_merge::mergeOrderLoaded`, `mergeInOneBatch` | 6 |
| 7 | ... whether or not the session is loaded | `::mergeOrderUnloaded`, `::identityStubIsMatched` | 6 |
| 7 | conflicting header attribute fails, changes nothing | `::conflictChangesNothing_loaded` / `_unloaded`; `tst_session_merge::differentAttributeConflicts`, `planIsPure` | 6 |
| 7 | session edits survive | `::editsSurviveMerge` (loaded + stub variant) | 6 |
| 7 | unmatched measurements survive | `::unmatchedMeasurementsSurvive` (loaded); **NEW** stub variant of the same function [8.2]; `tst_session_merge::columnsReplaceAddKeep` | 6, 8 |
| 8 | escape hatch: attribute + effective gyro update, no new session, edits kept | `tst_import_merge::escapeHatch` (loaded + stub variant); `tst_source_layer::schemaAttributeFlipsGyro` | 6, 4 |
| 9 | three outputs run once, any order | `tst_calcengine::threeOutputsRunOnce`; `tst_session_engine::multiOutputRunsOnce` | 2, 3 |
| 9 | declared input change -> exactly one run; unrelated -> none | `tst_calcengine::declaredInputChangeRunsOnceMore`, `unrelatedChangeRunsNothing`; `tst_session_engine` same names | 2, 3 |
| 10 | absent-input candidate selected once input appears, replacing cached fallback | `tst_calcengine::preferredCandidateReplacesFallback`, `rejectedCandidatesAreDependencies`; `tst_session_engine::preferredSensorReplacesFallback`; `tst_import_merge::candidateSwitchAfterMerge` | 2, 3, 6 |
| 10 | randomized reads + edits (+ registry changes) == fresh, engine level | `tst_calcengine_oracle` (seeds 1-25) | 2 |
| 10 | randomized reads + edits + **merges** on real sessions == fresh | **NEW** `tst_session_oracle::sessionSequences`, `::modelSequences` [8.3] | 8 |
| 11 | override of one output coexists, no cycle | `tst_calcengine::overrideOneOutput`, `overrideFeedsDownstream`; `tst_session_engine::overrideOneOutput`, `overrideFlareStart`; **NEW** oracle asserts `cycleCount() == 0` under random overrides [8.3] | 2, 3, 8 |
| 12 | nested, cycles, exceptions: no partial result, clean state | `tst_calcengine_safety` (all); `tst_python_bridge::exceptionYieldsCleanUnavailable`, `bundleExceptionPublishesNothing`; **NEW** `tst_session_engine::safetyOnRealSession` [8.2] | 2, 7, 8 |
| 13 | two sessions, one registration, independent results | `tst_calcengine::twoSessionsIndependent`; `tst_session_model_engine::twoSessionsOneRegistration` | 2, 3 |
| 13 | unregister invalidates every session (loaded) | `tst_calcengine::unregisterInvalidatesEverySession`; `tst_session_model_engine::unregisterInvalidatesEverySession`, `reRegisterRestores` | 2, 3 |
| 13 | ... including cached columns of **unloaded** sessions | `tst_column_cache::altitudeMarkerChangeDiscards` (registration *added*); **NEW** `::altitudeMarkerRemovalDiscardsStubValues` (registration *removed*) [8.2] | 5, 8 |
| 14 | explicit: unavailable until requested; request publishes all outputs | `tst_calcengine::explicitPolicy`; **NEW** `tst_session_engine::explicitPolicyOnSession` (real `SessionData`, global registry) [8.2] | 2, 8 |
| 15 | declared preference invalidates dependents | `tst_calcengine::declaredPreferenceInvalidates`; `tst_builtins_engine::analysisRangeFollowsPreference`; `tst_session_engine::declaredPreferenceInvalidates`; `tst_session_model_engine::preferenceBroadcastReachesModel`; `tst_column_cache::preferenceChangeDiscardsUnloadedRows` | 2, 3, 5 |
| 15 | snapshotted preference does not affect existing sessions | `tst_calcengine::snapshottedPreferenceDoesNot`; `tst_session_engine::snapshotPreferencesDoNot`; `tst_column_cache::snapshotPreferenceDoesNotDiscard`; `tst_import_merge::defaultsNotReappliedOnMerge` | 2, 3, 5, 6 |
| 16 | derived `wTotal` + interpolated gyro attribute are corrected and follow source | `tst_source_layer::derivedWTotalUsesCorrectedGyro`, `interpolatedGyroAttribute`; `tst_session_engine::derivedWTotalFollowsSource`, `interpolatedAttributeFollows` | 4, 3 |
| 16 | file-supplied `wTotal` wins | `tst_source_layer::fileSuppliedWTotalWins` | 4 |
| 17 | single-output plugins, real bridge, effective reads | `tst_python_bridge::bootsRealBridge`, `singleOutputPluginsReadEffectiveValues` | 7 |
| 17 | multi-output plugin runs once | `::multiOutputRunsOnce`, `::bundledExampleRuns` | 7 |
| 17 | source access from Python matches C++ | `::sourceAccessMatchesCpp`, `::sourceOfDerivedNameIsAbsent` | 7 |
| 17 | Python exception -> clean unavailable | `::exceptionYieldsCleanUnavailable`, `::bundleExceptionPublishesNothing` | 7 |
| 18 | upgrade discards and recomputes cached gyro columns | `tst_logbook_index::missingMarkerDiscardsValues`; `tst_column_cache::upgradeDiscardsAndRecomputes`; **NEW** `tst_workflow::releasedLogbookUpgrade` (released CSV **and** released index together) [8.4] | 5, 8 |
| 18 | a session edit refreshes only affected columns | `tst_column_cache::editRefreshesOnlyAffectedColumn_warm` / `_cold`, `markerEditRefreshesDependents`; `tst_import_merge::mergeEffects` | 5, 6 |
| 19 | full application builds | Task 8.12 (build commands; CI, Task 8.10) | 8 |
| 19 | import -> rows -> columns -> marker / attribute edit -> save -> reopen | **NEW** `tst_workflow::importEditSaveReopen` [8.4] | 8 |
| 19 | plugin workflow end to end | **NEW** `tst_python_bridge::pluginWorkflowThroughModel` [8.4]; manual checklist (Task 8.12) | 8 |
| 19 | plot / marker / GUI workflows | manual checklist (Task 8.12) - widgets are outside the test library boundary (Phase 1) | 8 |
| 19 | no remaining use of the old engine or direct cache setters | **NEW** CTest `audit_cleanup` [8.5]; grep criteria of Tasks 3.9, 3.11, 4.5, 4.6, 7.8 | 8 |
| spec 12 | only `SCHEMA_VER` decides (not firmware, file name, date) | **NEW** `tst_source_layer::onlySchemaVerDecides` [8.2]; `audit_cleanup` | 8 |

---

## Cross-phase findings

Findings from reading Phases 1-7 against each other. "Here" = resolved by a
task of this phase; "Amend" = the earlier document should be corrected by the
coordinator (the code-level consequence, if any, is still handled here so that
this phase does not depend on the amendment).

| # | Finding | Resolution |
|---|---|---|
| F1 | **Phases 5 and 7 both edit the engine in parallel**, although the overview says the tracks touch disjoint files. Phase 5 Task 5.5 edits `src/engine/calculationregistry.{h,cpp}`, `tests/tst_calcregistry.cpp`, `tests/tst_builtins_engine.cpp`; Phase 7 Task 7.1 edits `src/engine/calculationdescriptor.h`, `calculationregistry.cpp`, `evaluationcontext.{h,cpp}`, `tests/tst_calcregistry.cpp`, `tests/tst_calcengine.cpp`. Neither phase lists `src/engine/*` as owned or forbidden. | **Here (Task 8.1a):** merge check + one new test proving the two additions compose (`staticDependencies` must see opt-in source inputs). **Amend** 00-overview (dependency-graph note) and the "do not touch" lists of 05 / 07: both may edit `calculationregistry.cpp` and `tst_calcregistry.cpp`, as self-contained appended blocks. |
| F2 | **`allowSourceInputs` (Phase 7) versus spec 7.5 / overview "Graph identities"** ("only the conversion layer depends on source nodes"). Phase 7 flags it; the overview still states the absolute rule. | **Here:** documented in `docs/CALCULATIONS.md` (Task 8.7) as the single, plugin-only exception; `audit_cleanup` asserts the flag is set only in `src/pluginadapters.cpp`. **Amend** 00-overview decision "Graph identities" to name the exception. Stays an open question for Michael (Q15). |
| F3 | **`FLYSIGHT_BUILD_PYTHON_TESTS` is not forwarded by the root superbuild.** Phase 1 established that the root `CMakeLists.txt` forwards a hand-picked list (`_APP_CMAKE_ARGS`); Phase 7 defines the option only in `tests/CMakeLists.txt`. With the documented Windows workflow (`cmake --build build --config Release` on the superbuild) `-DFLYSIGHT_BUILD_PYTHON_TESTS=OFF` is silently ignored. | **Here (Task 8.1b).** |
| F4 | **Phase 5 names one test two ways:** Task 5.7's table defines `tst_column_cache::indexFlushWhileDirtyOmitsUnsaved`; its "Testing Requirements" acceptance mapping cites `columnTaskFlushWhileDirtyOmitsUnsaved`. | **Here:** the function is `indexFlushWhileDirtyOmitsUnsaved` (the table is authoritative); `tests/acceptance_map.txt` (Task 8.5) uses that name and the audit fails if it does not exist. **Amend** 05 Testing Requirements. |
| F5 | **Grep criteria that later tasks of the same or a later phase break.** (a) Phase 4 Task 4.3: `git grep -nE "getUnit\(|\.units\(" -- src tests` "returns nothing", but Phase 1's `Fs1FileBuilder::units()` is called as `.units(` in `tests/`. (b) Phase 3 Task 3.11 and Phase 7 Tasks 7.4 / 7.8 require `setCalculated…` and `DependencyKey` to be absent from `python_plugins`, while Phase 7 Task 7.9 requires `python_plugins/README.md` section 9 to *name* `session.setCalculatedMeasurement(...)` and `flysight_cpp_bridge.DependencyKey` as removed. | **Here:** `audit_cleanup` (Task 8.5) restricts (a) to `-- src` and excludes `python_plugins/README.md` and `docs/` from (b). **Amend** 04 Task 4.3 criterion 1 (`-- src`) and 07 Tasks 7.4 / 7.8 (add `':!python_plugins/README.md'`). |
| F6 | **Phase 2 promises forwarding that no phase implements.** Task 2.7: "`invalidated` is returned to the caller (Phase 3 forwards it to `dependencyChanged`)". Phase 3 defines no model-level `request` wrapper, and nothing is explicit. | **Here:** no API is added (nothing uses explicit policy; spec 7.7 asks only for the flag and the synchronous request, which exist on `CalculationEngine`). `explicitPolicyOnSession` (Task 8.2) proves it works on a real session; `docs/CALCULATIONS.md` states that a future caller must publish the returned set through `SessionModel`'s single emitter. **Amend** 02 Task 2.7: "a future model-level caller forwards it". |
| F7 | **Phase 5 replaces Phase 3's row clearing.** Phase 3 Task 3.10: `flushPendingInvalidations` clears the row's `cachedValues`; Phase 5 Task 5.7 removes that in favor of `checkCalculationEnvironment`. Phase 3's criterion "clears the row's `cachedValues`" still holds only because every broadcast is an environment change, and only if `LogbookManager::cacheEnvironment()` differs from the new fingerprint. | **Here:** no code change. Task 8.1e re-runs `tst_session_model_engine` after both tracks are merged and pins the reason in a comment. **Amend** 03 Task 3.10 with a forward note ("superseded by 05 Task 5.7: clearing happens in `checkCalculationEnvironment`"). |
| F8 | **Phase 3 allows two `emit dependencyChanged` sites** (`publishInvalidation` and the merge path); Phase 6 Task 6.4 requires exactly one. | **Here:** `audit_cleanup` enforces exactly one (`publishInvalidation`). No amendment needed (later phase tightens). |
| F9 | **An unsupported `SCHEMA_VER` held in memory can be saved, producing a file the importer rejects.** Phase 4 decides that a programmatic `setAttribute("SCHEMA_VER","3")` yields "no correction + warning"; Phase 5's exporter writes any stored attribute; Phase 4's importer then rejects the file on the next load, so the session becomes unloadable. No UI path reaches this (`SCHEMA_VER` is not in `AttributeRegistry`), but `SessionModel::updateAttribute` is public and Phase 5's own test edits `SCHEMA_VER` through it. | **Here (Task 8.1d):** exporter validation refuses to write a stored `SCHEMA_VER` that `Schema::parseVersion` rejects. |
| F10 | **Acceptance-19 and oracle tests must use the real import path.** Phase 6 keeps a `mergeSessions(const QList<SessionData>&)` overload "for tests, tools" that adopts a session without creation defaults; every Phase 1-5 model test goes through it. The application path is `SessionImport::importFiles` -> `mergeSessions(QList<ParsedFile>)`. | **Here:** `tst_workflow` and `tst_session_oracle::modelSequences` import **only** through `SessionImport::importFiles` with files on disk. `audit_cleanup` asserts `mergeSessions` has no caller in `src` outside `sessionimport.cpp`. No amendment. |
| F11 | **Open questions answered by later phases** but still listed as open: Phase 3 Q1 (stale columns of unloaded sessions) -> answered by Phase 5's environment fingerprint; Phase 5 Q4a-c (ragged merge, mutation rule, backfill) -> answered by Phase 6; Phase 4 manual step 4 ("dialog lists only the file name until Phase 6") -> done in Task 6.6. | **Here:** the consolidated list below marks them resolved. No amendment required. |
| F12 | **Phase 1 Task 1.7 criterion** "no mention of ... `docs/DATA_SCHEMA.md` is introduced" in `README.md` is a Phase 1-local rule; this phase adds the link on purpose. | None needed; stated to prevent a false audit failure. |

---

## Tasks

Order: 8.1 -> 8.2 -> 8.3 -> 8.4 -> 8.5 (tests and audit green) -> 8.6-8.9
(documentation) -> 8.10 (CI) -> 8.11 (open questions hand-off) -> 8.12
(sign-off). The application builds and all tests pass after each task.

### Task 8.1: Cross-phase reconciliation

**Purpose:** Remove the integration hazards found above before building acceptance tests on top of them.

**Files to modify:**
- `CMakeLists.txt` (root) - forward the Python test option (b).
- `src/dataexporter.cpp`, `src/dataexporter.h` - `SCHEMA_VER` write guard (d).
- `tests/tst_calcregistry.cpp` (a), `tests/tst_persistence_roundtrip.cpp` (d), `tests/tst_session_model_engine.cpp` (e, comment only).

**Technical Approach:**

a. **Merged engine edits (F1).** After both tracks are merged, confirm
   `calculationregistry.cpp` contains Phase 5's `staticDependencies` /
   `declaredPreferenceKeys` / observers **and** Phase 7's relaxed validation.
   Add `tst_calcregistry::staticDependenciesCoverOptInSourceInputs`: private
   registry; plain descriptor `srcProbe` with `allowSourceInputs = true`,
   inputs `CalcInput::sourceMeasurement("S","m")`, `CalcInput::sourceUnit("S","m")`,
   `CalcInput::attribute("A")`, output attribute `SRC0`. Expected:
   `staticDependencies(attribute("SRC0")).names` == {`SRC0`, `S/m`, `A`}
   (literal set; `S/m` arrives through Phase 5's rule "SourceMeasurement /
   SourceUnit -> insert `DependencyKey::measurement(sensor, name)`"),
   `.preferences` empty. This is what makes a logbook column fed by a
   source-reading plugin refresh when its source column is merged.
b. **Option forwarding (F3).** In the root `CMakeLists.txt`, next to
   `FLYSIGHT_BUILD_TESTS` (Phase 1 Task 1.4 step 2): declare
   `option(FLYSIGHT_BUILD_PYTHON_TESTS "Build and run the embedded-Python plugin bridge test" ON)`
   and append `-DFLYSIGHT_BUILD_PYTHON_TESTS=${FLYSIGHT_BUILD_PYTHON_TESTS}` to
   `_APP_CMAKE_ARGS` unconditionally; add it to the usage / options comment
   blocks and the summary printout exactly as Phase 1 did for the first option.
c. **Test name (F4).** Verify `tst_column_cache` has
   `indexFlushWhileDirtyOmitsUnsaved`; if the implementer of Phase 5 used the
   other spelling, rename the function to the table's name.
d. **Exporter guard (F9).** In `DataExporter` validation (Phase 5 Task 5.3
   step 2, before the file is opened): if
   `sessionData.hasStoredAttribute(Schema::AttributeKey)` and
   `!Schema::parseVersion(sessionData.storedAttribute(Schema::AttributeKey))`
   -> fail with `Schema::unsupportedMessage(value)`; nothing is written, the
   previous file stays. Include `conversion/schematable.h` (same library; the
   `"SCHEMA_VER"` literal still exists once). Header comment: "Never writes a
   file that `DataImporter` would reject." Test
   `tst_persistence_roundtrip::unsupportedSchemaIsNotSaved`: awkward fixture
   imported, `setAttribute("SCHEMA_VER","3")`; target path pre-populated with
   `"keep"` -> `exportSession` false, error ==
   `"Unsupported SCHEMA_VER '3' (supported: 1, 2)"`, file still `"keep"`;
   with `"2"` the export succeeds and contains one `$VAR,SCHEMA_VER,2` line.
e. **Phase 3 / 5 interplay (F7).** Run `tst_session_model_engine`; above the
   `cachedValues` assertion of `preferenceBroadcastReachesModel` add the comment
   `// Cleared by SessionModel::checkCalculationEnvironment (Phase 5), not by flushPendingInvalidations.`
   Do not change the expectation.

**Acceptance Criteria:**
- [ ] With the superbuild configured `-DFLYSIGHT_BUILD_TESTS=ON -DFLYSIGHT_BUILD_PYTHON_TESTS=OFF`, `build/FlySightViewer-build/CMakeCache.txt` contains `FLYSIGHT_BUILD_PYTHON_TESTS:BOOL=OFF` and `ctest -N` does not list `tst_python_bridge`; with `ON` it does.
- [ ] `staticDependenciesCoverOptInSourceInputs` and `unsupportedSchemaIsNotSaved` pass; all 22 existing targets pass.
- [ ] `git grep -n "\"SCHEMA_VER\"" -- src` still has exactly one hit.

**Complexity:** M

---

### Task 8.2: Gap-closing acceptance tests in existing targets

**Purpose:** Cover the acceptance clauses the matrix marks NEW that fit naturally into an existing suite.

**Files to modify:** `tests/tst_persistence_roundtrip.cpp`, `tests/tst_import_merge.cpp`, `tests/tst_column_cache.cpp`, `tests/tst_session_engine.cpp`, `tests/tst_source_layer.cpp`.

**Technical Approach (literals only; `near` = `qAbs(a-b) <= 1e-9`; `T0` = 1704110400.0):**

| Test | Scenario | Expected |
|---|---|---|
| `tst_persistence_roundtrip::fileAndMemoryWritersAgree` (acc. 5) | (i) awkward fixture session; (ii) programmatic sensor `BIG`, columns `time,a,b,c,d,e,f,g` unit `s` + seven `u`, 80 000 rows, row `i` = `i * 0.001`, `i + 0.25`, `-i - 0.5`, `i * 3.0`, `1e-3 * i`, `i % 7`, `0.1`, `-0.0`. For each: `exportSession(path)`; `bytes = readFileBytes(path)` | `bytes == *DataExporter::toBytes(session)` for both; (ii): `bytes.size() > 4 * 1024 * 1024` (crosses the periodic-flush boundary of Phase 5 Task 5.3 step 5), ends with `'\n'`, and `reload(bytes).sourceData() == session.sourceData()` |
| `tst_import_merge::unmatchedMeasurementsSurvive` - add a data row "stub" (acc. 7) | same scenario with `makeStub(model)` between creation and re-import | `Merged`; `FOO/bar == {7.0}` unit `furlongs`; `sourceMeasurement("IMU","wx") == {10.0}`; after `waitForIdle` the saved CSV still has `$COL,FOO,time,bar` |
| `tst_column_cache::altitudeMarkerRemovalDiscardsStubValues` (acc. 13) | two `DescentFixture` sessions `s1` (loaded), `s2` (stub via reopen); column A = `SessionAttribute _ALTITUDE_1000_M`; altitude list `[1000]` Metric as in Phase 3 Task 3.12; `waitForIdle` -> A cached for both (`s1` near T0+71.5); then empty the list, trigger `refresh()`, `flushPendingInvalidations()`, `waitForIdle` | `CalculationRegistry::instance().hasCandidateFor(attribute("_ALTITUDE_1000_M"))` false; for **both** rows the cached value of A is absent or an invalid `QVariant` (never T0+71.5); `index.json` holds no number for A under either session; neither row dirty; no CSV rewritten (bytes equal); `cacheEnvironment()` == the new fingerprint. Restore the list in `cleanup()` |
| `tst_session_engine::explicitPolicyOnSession` (acc. 14) | descent fixture; register on the **global** registry `test.explicit.pair`: policy `Explicit`, input `A(_EXIT_TIME)`, outputs `_T_EXPL_A` = exit + 1, `_T_EXPL_B` = exit + 2; `cleanup()` unregisters | before request: both unavailable, `runCount == 0`, `resultStatus == NotRequested`; `request("test.explicit.pair")`: `found`, status `Ok`, `invalidated` contains both names; values `1704110410.0`, `1704110411.0`; `runCount == 1`; second `request` runs nothing; `setAttribute(_EXIT_TIME, T0+20.0)` -> both unavailable again with no run; after the next `request`: `1704110421.0`, `1704110422.0`, `runCount == 2`; `verifyAgainstFresh` of both names empty at every step |
| `tst_session_engine::safetyOnRealSession` (acc. 12) | descent fixture; temporary global registrations (removed in `cleanup()`): `test.throw` (input `A(_EXIT_TIME)`, outputs `_T_THROW_A`, `_T_THROW_B`; sets A on the bundle, then `throw std::runtime_error("x")`), `test.nested` (input `A(_T_THROW_A)`, output `_T_NESTED`), `test.cycX` (input `A(_T_CYC_Y)` -> `_T_CYC_X`), `test.cycY` (input `A(_T_CYC_X)` -> `_T_CYC_Y`) | all five names unavailable; `resultStatus("test.throw") == Failed`, `("test.nested") == MissingInput`; `cycleCount() >= 1`; `scopeDepth() == 0`; `_EXIT_TIME` still T0+9 and `IMU/wTotal[0]` still near 5.7344; repeated reads do not re-run `test.throw` (`runCount == 1`); `verifyAgainstFresh(all golden names + the five)` empty |
| `tst_source_layer::onlySchemaVerDecides` (spec 12) | three imports of the sensor fixture: (i) `FIRMWARE_VER=v2099.12.31`, no `SCHEMA_VER`, file named `SCHEMA_VER_2.CSV`; (ii) `FIRMWARE_VER=v2020.01.01` + `.var("SCHEMA_VER","2")`, file named `LEGACY.CSV`; (iii) unmarked sensor fixture that additionally declares a `GNSS` sensor (`time,lat,lon,hMSL`) with one row whose ISO timestamp is `2031-01-01T00:00:00.000Z` (a recording date after the firmware fix) | (i) `wx` near 71.68; (ii) `wx == 62.5`; (iii) `wx` near 71.68 |

**Acceptance Criteria:**
- [ ] Each new function carries its acceptance number in a comment and passes on Windows Release.
- [ ] Tests that register on the global registry leave `registeredIds()` as they found it (compare a snapshot in `cleanup()`).
- [ ] No expectation is computed by the code under test.

**Complexity:** M

---

### Task 8.3: Session-level randomized idempotency oracle (`tst_session_oracle`)

**Purpose:** Spec 7.4 as a test on real objects: after any sequence of reads, edits, merges, preference changes, and registry changes, every name equals a fresh evaluation (acceptance 10, second sentence; also 5, 11, 13, 15, 16).

**Files to create:**
- `tests/tst_session_oracle.cpp` (class `SessionOracleTest`)
- `tests/support/oraclecatalogue.h` / `.cpp` - catalogue, fragment pool, op log (added to `flysight_test_support`)

**Files to modify:** `tests/CMakeLists.txt`:

```cmake
flysight_add_test(tst_session_oracle SOURCES tst_session_oracle.cpp ENVIRONMENT "QT_HASH_SEED=0")
set_tests_properties(tst_session_oracle PROPERTIES TIMEOUT 300 LABELS "core;oracle")
```

(`QT_HASH_SEED=0` makes `QSet` / `QHash` iteration order reproducible, so a
failure that depends on invalidation order reproduces from its seed.)

**Technical Approach:**

**Determinism rules.** `std::mt19937 rng(seed)`; choose with `rng() % n` only
(Phase 2 rule: no `std::uniform_*_distribution`). Every choice indexes a fixed
`QList` / array - never iterate a `QSet` / `QHash` to choose. No clock, no
`QUuid` in any decision. `_IMPORT_TIME` (wall clock) is excluded from the
catalogue and has no dependents.

**Catalogue** (`QList<DependencyKey> oracleCatalogue()`), fixed order:
1. every `name` in `goldenValues()` (Phase 3 Task 3.1 - covers an output of each
   of the 67 built-ins and the interpolation family, including the three
   "unavailable" interpolation keys);
2. every recorded column of `DescentFixture` as an **effective** measurement:
   `GNSS/{time,lat,lon,hMSL,velN,velE,velD,hAcc,vAcc,sAcc,numSV}`,
   `IMU/{time,wx,wy,wz,ax,ay,az,temperature}`, `MAG/{time,x,y,z,temperature}`,
   `BARO/{time,pressure,temperature}`, `TIME/{time,tow,week}`, `VBAT/voltage`;
3. attributes `SCHEMA_VER`, `FIRMWARE_VER`, `_DESCRIPTION`, `_M`,
   `_WSP_TOP_ALT`, `_WSP_BOTTOM_ALT`, `_JUMPER_MASS`, `_WIND_N`;
4. interpolation keys `_M:IMU/_time/wx`, `_M:IMU/_time/wTotal`,
   `_EXIT_TIME:IMU/_time/wx`, `_WSP_ENTRY_TIME:GNSS/_time/z`, malformed `a:b/c`;
5. altitude attributes `_ALTITUDE_1000_M`, `_ALTITUDE_2000_M`, `_ALTITUDE_300_FT`;
6. never-existing `NOPE/none` and `_NOPE`.
`verifyAgainstFresh` compares availability, value, samples bit-wise, **and
unit**, so effective units are covered.

**Fragment pool** (`fragments(sessionId)`; each is an `Fs2FileBuilder` written
under `<tmp>/24-01-01/12-00-00/` and also parsed once with
`DataImporter::parseFile` for part A):

| Name | Content |
|---|---|
| `TRACK` / `SENSOR` | `DescentFixture::trackFile(id)` / `sensorFile(id)` |
| `SENSOR_V2` / `SENSOR_V1` | `SENSOR` + `.var("SCHEMA_VER","2")` / `"1"` (escape hatch; the second one to arrive conflicts - a legitimate `Failed` outcome) |
| `IMU_ALT` | IMU only, 3 rows at 10/20/30: wx 30/60/90, wy 40/80/120, wz 0, ax 0, ay 0, az 2, temperature 41; units as `SENSOR` |
| `IMU_SI` | IMU only, released-style units `s,deg/s,deg/s,deg/s,m/s^2,m/s^2,m/s^2,degC`, az `9.80665` (unit-only differences are changes) |
| `IMU_WTOTAL` | `SENSOR`'s IMU plus a recorded column `wTotal` (`deg/s`) = 99, 99, 99 (stored beats derived) |
| `TRACK_SHORT` | rows 0..119 of `TRACK` (descent incomplete: many outputs become unavailable) |
| `BARO_ONLY` | BARO sensor only |
| `CONFLICT` | `SENSOR` with `FIRMWARE_VER=v2024.01.01` (always `Failed`) |

All IMU fragments have 3 rows and all GNSS fragments replace every GNSS column,
so no merge is ragged; the test asserts that no merge error contains
`"rows but"`.

**Edit tables** (value `remove` = `removeAttribute`):
`_GROUND_ELEV` {0.0, 50.0, 100.0, remove}; `_EXIT_TIME` {T0+12, T0+30, remove};
`_TIME_FIT_A` {"2", "1", remove}; `_FLARE_START_TIME` {T0+150, remove};
`_ANALYSIS_END_TIME` {T0+250, remove}; `_START_TIME` {T0+1, remove};
`_WSP_TOP_ALT` {2400.0, 3000.0, remove}; `_WSP_BOTTOM_ALT` {-50.0, 1000.0, remove};
`_WSP_ENTRY_TIME` {T0+45, remove}; `_SP_WINDOW_START_ALT` {3000.0, remove};
`_M` {T0+15, T0+25, T0+500, remove}; `_WIND_N` {0.0, 5.0}; `_JUMPER_MASS` {80.0, 1.0};
`_DESCRIPTION` {"x", "y"}. (Six of these are user overrides of one output of a
multi-output calculation - acceptance 11.)
Source replace (same length as the existing column; skipped as a read when the
column is absent): `IMU/wx`, `IMU/az`, `TIME/tow`, `GNSS/hMSL`, `GNSS/velD`,
`BARO/pressure`; new sample `i` = old sample `i` + `(rng() % 5) - 2`.
Unit text: `IMU/ax`, `IMU/az` {`g`, `m/s^2`, `furlongs`}; `IMU/temperature`
{`deg C`, `degC`}; `MAG/x` {`gauss`, `T`}.
Preferences: declared `ImportDescentPauseSeconds` {30.0, 5.0, 1.0}; snapshotted
`AeroMass` {1.0, 90.0}.
Altitudes: `_ALTITUDE_1000_M` / 1000.0, `_ALTITUDE_2000_M` / 2000.0,
`_ALTITUDE_300_FT` / 91.44.

**Part A - `sessionSequences`** (data-driven, seeds 1..20, 300 operations each;
`SessionData` only, no model, no logbook). Two sessions `o1`, `o2` share the
global registry and run interleaved (`rng() % 2` picks the session) - this is
what exercises cross-session registry invalidation.

- *Fixed prefix per session:* `DataImporter::importFile(SENSOR)` (odd seeds;
  even seeds start with `TRACK`). Literal checkpoint (sensor first):
  `_START_TIME == T0+10`, `_DURATION == 20.0`, `IMU/wTotal[0]` near `5.7344`,
  `GNSS/z` unavailable. Then merge the other file through
  `SessionMerge::plan` + `apply`. Literal checkpoint: `_START_TIME == T0`,
  `_DURATION == 295.0`, `_EXIT_TIME == T0+9`, `_GROUND_ELEV == 100.0`.
- *Operation mix:* 40 % read one random catalogue name and compare it with
  `evaluateFresh` (`sameValue`); 5 % read burst of 10 random names in random
  order; 15 % attribute set / remove; 8 % merge a random fragment
  (`plan`; if `!ok()` assert `sourceData()` and `attributeKeys()` equal their
  pre-op copies and that `apply` is not called; else `apply`); 8 % source
  replace (`setSourceMeasurement`); 5 % `setUnit`; 5 % `SCHEMA_VER`
  {set `"2"`, set `"1"`, remove, set `"3"` (in-memory unsupported: Phase 4
  decision; warnings are swallowed by a message-handler filter)}; 5 %
  preference change through `PreferencesManager::setValue`; 5 % altitude
  register / unregister through `CalculationRegistry::instance()` with
  `AltitudeMarkerManager::makeDescriptor` / `calculationId`; 4 % object motion
  (`SessionData tmp = std::move(s); s = std::move(tmp);` or copy-assign a copy
  back) to exercise `rebind` and cold copies.
- *After every mutation:* `totalRunCount()` of both sessions is unchanged by
  the mutation itself (invalidation never computes); for a snapshotted
  preference the invalidation listener was **not** called; then
  `verifyAgainstFresh` of 8 random catalogue names on the mutated session.
- *Always:* `scopeDepth() == 0`, `undeclaredReadCount() == 0`,
  **`cycleCount() == 0`** (overrides never create cycles), and no name that is
  derived-only in the op log so far appears in `attributeKeys()` /
  `measurementKeys()` (spec 4).
- *Every 25 operations and at the end:* `verifyAgainstFresh(oracleCatalogue())`
  is empty for both sessions.
- *End-state literal check:* restore persistent state - remove every attribute
  of the edit tables (set `_WIND_N` 0.0, `_JUMPER_MASS` 1.0), remove
  `SCHEMA_VER`, re-merge `TRACK` and `SENSOR`, `resetPreferencesToDefaults()`,
  unregister the three altitude ids - then every row of `goldenValues()` holds
  to its tolerance, skipping rows whose name has recorded source data that the
  original fixture lacks (`IMU/wTotal` after `IMU_WTOTAL`, which cannot be
  removed). This is independent of the oracle: values come back to literals.

**Part B - `modelSequences`** (seeds 1..6, 120 operations; real `SessionModel`,
`LogbookManager`, `AltitudeMarkerManager`, temp logbook). Per seed:
`useFreshLogbook()`, `resetPreferencesToDefaults()`, `initialize()`, columns
D = `SessionAttribute _DESCRIPTION`, X = `SessionAttribute _EXIT_TIME`,
G = `MeasurementAtMarker{IMU, wx, markerAttributeKey "_M"}`,
A = `SessionAttribute _ALTITUDE_1000_M`; model + manager + `registerAll()`.

- *Fixed prefix:* `SessionImport::importFiles(model, {TRACK(o1), SENSOR(o1)})`
  -> `Created`, `Merged`; same for `o2` in reverse file order.
- *Operation mix:* 35 % read a random name via `model.sessionRef(row)` and
  compare with `evaluateFresh`; 25 % `updateAttribute` / `removeAttribute` from
  the edit tables; 12 % `SessionImport::importFiles(model, {one fragment file})`
  (**the application path, F10**); 5 % preference change; 5 % altitude list edit
  through the `altitudeMarkers` `QSettings` array + preference bump (Phase 3
  Task 3.12 procedure); 3 % `flushPendingInvalidations()`; 5 %
  `waitForIdle(model)`; 10 % **restart**: `waitForIdle`, destroy manager and
  model, `reopenLogbook()`, `initialize()`, new model,
  `populateFromIndex(cachedColumnValues(liveColumns), lastAccessedMap())`,
  `startColumnWorker()`, new manager + `registerAll()` - rows are stubs again
  and the next read or merge loads them.
- *After every operation:* for each row with `rowAt(row).isLoaded()` (never
  force a load just to verify) `verifyAgainstFresh` of 8 random names;
  import outcomes are one of `Created` / `Merged` / `Unchanged` / `Failed`, and
  a `Failed` / `Unchanged` result leaves the session CSV bytes unchanged after
  `waitForIdle`.
- *End of sequence - persisted-state oracle:* `waitForIdle`; for each session
  `live = model.sessionRef(row)`; `fresh = *LogbookManager::instance().loadSession(id)`;
  then (a) `fresh.sourceData() == live.sourceData()`; (b) for every catalogue
  name `sameValue(live value, fresh value)` (acceptance 5: effective values
  identical before and after a save / reload, under random histories); (c) the
  values in `cachedValuesForSession(id)` for D, X, G, A equal the values
  computed from `fresh` (spec 9.4: cached columns never disagree with the saved
  file); (d) `flushIndex()`, then the JSON on disk has
  `calculationCompatibility == 1`.

**Failure reporting.** Every operation appends one line to an in-memory op log
(`#<step> <session> <op> <arguments>`, e.g. `#57 o2 setAttribute _EXIT_TIME 1704110412`).
All checks go through one helper that on failure calls `QFAIL` with
`seed=<s> step=<n> name=<key> cached=<...> fresh=<...>` and first prints the
whole op log with `qWarning` (at most 300 lines). Reproduce with
`FLYSIGHT_ORACLE_SEEDS=<s>` (a single seed or `a-b` range, replaces the default
seed list of both parts) and optionally `FLYSIGHT_ORACLE_OPS=<n>`;
`FLYSIGHT_ORACLE_SEEDS=1-500` is the documented soak run.

**Runtime budget.** Default configuration <= 60 s wall time in Windows Release
on the development machine (CTest `TIMEOUT 300`). If it exceeds the budget,
reduce the per-step sample from 8 to 4 names - **never** the seed count, the
operation mix, or the full-catalogue checkpoints.

**Acceptance Criteria:**
- [ ] `tst_session_oracle` passes on Windows Release within the budget; acceptance 10 (and 5, 11, 13, 15, 16) are named in comments.
- [ ] The only computed expectations are oracle comparisons and live-versus-reloaded equality; both parts contain the literal checkpoints above.
- [ ] Mutation check, done once by hand and not committed: (i) skipping the `Result` note for rejected candidates in `CalculationEngine::resolve` step 6, (ii) making `SessionMerge::apply` skip its invalidation return, and (iii) restoring Phase 3's unconditional whole-row recompute **without** `markColumnsUnsaved` each make the test fail, and the failure message names a seed that reproduces it.
- [ ] Two consecutive runs with the same seed produce identical op logs (diff them once).
- [ ] Nothing is written outside `TestEnvironment::rootPath()`; the global registry and preferences are restored in `cleanup()`.

**Complexity:** L (natural split: catalogue + fragments + op log; part A; part B; failure reporting / env overrides)

---

### Task 8.4: End-to-end workflow tests (acceptance 19, non-widget)

**Purpose:** One automated pass through the workflow a user performs, on the application's own code path, plus the plugin variant.

**Files to create:** `tests/tst_workflow.cpp` (class `WorkflowTest`).

**Files to modify:** `tests/CMakeLists.txt` (`flysight_add_test(tst_workflow SOURCES tst_workflow.cpp)`), `tests/tst_python_bridge.cpp`, `tests/python_plugins/` (no new file; reuses `t_multi.py`).

**Technical Approach:** `initTestCase`: `registerBuiltIns()`. `init`:
`useFreshLogbook()`, `resetPreferencesToDefaults()`, `initialize()`. Files are
written to `<newTempDir>/24-01-01/12-00-00/{TRACK,SENSOR}.CSV` from
`DescentFixture`. Columns: D = `_DESCRIPTION`, X = `_EXIT_TIME`,
G = `MeasurementAtMarker{IMU, wx, "_M"}`.

| Test | Steps | Expected |
|---|---|---|
| `importEditSaveReopen` (acc. 19) | 1. `SessionImport::importFiles(model, {TRACK, SENSOR})`. 2. Read rows / columns. 3. Marker edit `updateAttribute(id, "_EXIT_TIME", T0+12.0)`; attribute edits `_M` = T0+19.0, `_GROUND_ELEV` = 50.0, `_DESCRIPTION` = `"workflow jump"`. 4. `waitForIdle`. 5. Destroy the model, `reopenLogbook()`, `initialize()`, new model + `populateFromIndex` + `startColumnWorker()` + `waitForIdle`. 6. `sessionRef(0)`. | 1: outcomes `Created`, `Merged`; `rowCount() == 1`; `failureMessage(...)` empty. 2: `_DESCRIPTION == "24-01-01/12-00-00"`; `_EXIT_TIME == T0+9`; effective `IMU/wx[0]` near `3.44064`, source `{3,6,9}`, `sourceUnit("IMU","ax") == "g"`. 3: `_SYNC_TIME == T0+12`; `_EXIT_TIME:GNSS/_time/hMSL` near `3970.0`; `GNSS/z[0] == 3950.0`; `dependencyChanged` spy contains `_SYNC_TIME`. 4: one CSV; it contains `$VAR,_EXIT_TIME,1704110412`, `$VAR,_GROUND_ELEV,50`, `$UNIT,IMU,s,deg/s,deg/s,deg/s,g,g,g,deg C`, no `SCHEMA_VER`, and neither `3.44064` nor `9.80665`. 5: the row is a stub; cached D == `"workflow jump"`, X `toDouble() == 1704110412.0`, G near `6.537216` (= (3 + 0.9 * 3) * 1.14688); `columnWorkStats().sessionsLoaded == 0` (values came from `index.json`). 6: the same literals as step 3 from the reloaded session; `verifyAgainstFresh(oracleCatalogue())` empty |
| `warmModelSaveEqualsColdExport` (acc. 5) | import as above; read every catalogue name on `sessionRef(0)` (warm); `updateAttribute(_DESCRIPTION, "w")`; `waitForIdle` | saved CSV bytes `== *DataExporter::toBytes(SessionData(model.sessionRef(0)))` (a copy is state-only, hence cold) |
| `releasedLogbookUpgrade` (acc. 6, 18) | Build a released-style logbook: save any session with id `rel`, set columns D and G, flush; overwrite the CSV with literal file **R2** = Phase 5's `R` (`releasedLogbookSaveKeepsBytes`: `_IMPORT_TIME=1.7189e+09`, the four backfill keys, `m/s^2` / `degC` / `T` labels) extended by `$VAR,_M,1704110415`, a `TIME` sensor (`time,tow,week` / `s,s,`; rows `10,129610,2295` ... `30,129630,2295`) and three IMU rows at 10/20/30 with wx 1/2/3; rewrite `index.json` without `calculationCompatibility` and `calculationEnvironment` and with G = `1.5`, D = `"old jump"`. `reopenLogbook()`, `initialize()`, model from index, worker, `waitForIdle`. Then `updateAttribute("rel", _DESCRIPTION, "new text")`, `waitForIdle`. | after the worker: G near `1.72032` (not `1.5`), CSV bytes `== R2` (no migration); `index.json` has `calculationCompatibility` 1. After the edit: the CSV's line set differs from R2's **only** in the `$VAR,_DESCRIPTION,` line; `_IMPORT_TIME` text still `1.7189e+09`; units still `m/s^2` / `degC` / `T`; no `SCHEMA_VER`; G still near `1.72032`; `columnWorkStats().valuesComputed == 1` for the edit |
| `tst_python_bridge::pluginWorkflowThroughModel` (acc. 17 / 19) | temp logbook; column P = `SessionAttribute _PY_W_MAX` (`t_multi.py` `PyGyroStats`); `SessionImport::importFiles(model, {bridgeSensorFile})`; `waitForIdle`; reopen as above | live `_PY_W_MAX` near `71.68`; cached P near `71.68` before and after the reopen with `sessionsLoaded == 0`; the saved CSV contains no `_PY_` text (plugin outputs are never persisted); `calculationEnvironmentFingerprint()` is equal before and after the reopen (same plugin set) |

**Acceptance Criteria:**
- [ ] `tst_workflow` is registered with `flysight_add_test`, passes on Windows Release, and never calls `mergeSessions` or `DataImporter` directly (import only through `SessionImport::importFiles`).
- [ ] `pluginWorkflowThroughModel` passes when NumPy is available and is part of the (possibly disabled) `tst_python_bridge` target otherwise.
- [ ] Nothing is written outside `TestEnvironment::rootPath()`.

**Complexity:** M

---

### Task 8.5: Cleanup audit as an automated test (`audit_cleanup`) and machine-checked traceability

**Purpose:** Acceptance 19 "no remaining use of the old per-value cache engine or direct cache setters", spec 12 "remove the old mechanisms", spec 2 "no new UI" - as one repeatable command.

**Files to create:**
- `tests/audit/cleanup_audit.cmake` - CMake script (`cmake -P`), portable, needs only `git`.
- `tests/acceptance_map.txt` - one line per matrix row: `<item> <target> <function>`.

**Files to modify:** `tests/CMakeLists.txt`:

```cmake
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/../.git")
  add_test(NAME audit_cleanup COMMAND "${CMAKE_COMMAND}"
           -DREPO=${CMAKE_CURRENT_LIST_DIR}/.. -DGIT=${GIT_EXECUTABLE}
           -P "${CMAKE_CURRENT_LIST_DIR}/audit/cleanup_audit.cmake")
  set_tests_properties(audit_cleanup PROPERTIES LABELS "audit" TIMEOUT 60)
endif()
```

**Technical Approach:** The script defines three helpers and ends with
`message(FATAL_ERROR ...)` listing **all** violations:
`expect_none(<label> <regex> <pathspec>...)` (`git grep -nE`, exit code 1 =
pass), `expect_only(<label> <regex> <allowed-file-regex> <pathspec>...)`
(every hit's file must match), `expect_count(<label> <regex> <n> <pathspec>...)`.
Default pathspec `P` = `src tests python_plugins cmake CMakeLists.txt`, with
`':!python_plugins/README.md'` (F5) and `':!tests/audit'` always appended.

| Group | Pattern (ERE) | Rule |
|---|---|---|
| Old engine | `CalculatedValue\b|calculatedvalue\.|DependencyManager|dependencymanager|calculatedvalueregistry|CalculatedValueRegistry|m_sideEffectKeys|m_activeCalculations|toDependencyKey` | none in `P` |
| Old registration / setters | `setCalculatedAttribute|setCalculatedMeasurement|registerCalculatedAttribute|registerCalculatedMeasurement|unregisterCalculatedAttribute|addDependencies|hasRegisteredCalculation|set_calculated|m_calculatedAttributes|m_calculatedMeasurements` | none in `P` |
| Import-time conversion | `toSI\b|PHASE4-SWITCH` ; `UnitConversion|unitconversion\.h` in `src/dataimporter.*` | none |
| Friend / back doors | `friend class DataImporter|invalidateAllCalculations|initializeFromDevice|loadAllSessions|scanSessionFiles\b` | none in `src tests` |
| Ambiguous unit API | `\bgetUnit\(|[.>]units\(` | none in `src` (**not** `tests`: `Fs1FileBuilder::units`) |
| Dead bridge | `bridgeimpl|bridge_impl|get_key_name|session_get_time_series|dependencykey_bindings` ; `DependencyKey` in `python_plugins src/cpp_bridge.cpp src/*_bindings.cpp` | none |
| Superseded commit `4668f48` | `GyroScaling|ImportGyroScaling|DataSchema\b|dataSchemaVersion|SchemaVersion\b` | none in `P docs README.md` |
| Renamed-column conventions | `"[A-Za-z]*_source"|"source:|_source"` | none in `src python_plugins tests` |
| One authority | `1\.14688` -> exactly 1 hit in `src` (`conversion/schematable.cpp`); `"SCHEMA_VER"` -> exactly 1 hit in `src` (`conversion/schematable.h`); `CalculationCompatibilityVersion *=` -> exactly 1; `FloatingPointShortest` only in `src/csvformat.cpp`; `<charconv>` none in `src` | counts |
| No schema inference | `FIRMWARE_VER|FirmwareVer|fileName|filePath|QFileInfo|QDate` | none in `src/conversion src/engine`; `FIRMWARE_VER|FirmwareVer` none in `src/dataimporter.cpp src/sessionmerge.cpp src/calculations` |
| Pure compute functions | `PreferencesManager|QSettings|QDateTime::current|std::rand|QRandomGenerator|_SESSION_ID` | none in `src/calculations src/conversion src/engine` (declared preference inputs arrive through `EvaluationContext::preference`; `altitudemarkerfeature.cpp` reads preferences only at registration time - Phase 3 decision - and is outside these directories) |
| Source-input opt-in (F2) | `allowSourceInputs *= *` (assignment) | only in `src/pluginadapters.cpp` |
| One mutation / emission path | `emit dependencyChanged` -> exactly 1 hit in `src/sessionmodel.cpp`; `exportSession\(` callers only `src/logbookmanager.cpp` (+ definition in `dataexporter.*`); `mergeSessions\(` in `src` only in `sessionmodel.*` and `sessionimport.cpp`; `importFile\(` in `src` only in `dataimporter.*`; `getAttribute|getMeasurement|effectiveUnit|calculationEngine` none in `src/dataexporter.cpp src/sessionmerge.cpp` | as stated |
| Leftover markers | `BASELINE:|PHASE4-SWITCH` none in `tests src`; added lines containing `TODO|FIXME` in `git diff v2026.04.1 -- src tests python_plugins` none | |
| No new UI (only when tag `v2026.04.1` resolves; otherwise print "skipped") | `git diff --name-only v2026.04.1 -- src/preferences src/mainwindow.ui src/qml src/resources.qrc src/ui` | must be a subset of {`src/preferences/preferencesmanager.h` (adds `hasPreference`), `src/preferences/enginepreferenceprovider.h`, `.cpp`, `src/ui/docks/plot/PlotWidget.cpp`}; `git diff v2026.04.1 -- src/preferences/preferencekeys.h` empty; added lines of `git diff v2026.04.1 -- src/mainwindow.cpp src/ui` contain none of `new QAction|addAction\(|addMenu\(|QDialog|QLabel|statusBar\(\)|QInputDialog|registerPreference\(`, and at most one `QMessageBox::` (the existing import-failure box moved in Task 6.6) |
| Traceability | for every line of `tests/acceptance_map.txt`: `tests/<target>.cpp` exists and contains `void <function>(`; every item 1..19 has at least one line | |

`tests/acceptance_map.txt` is the matrix above in machine form (including the
NEW tests and F4's corrected name). Keep the two in sync: the matrix in
`tests/README.md` (Task 8.9) is generated by hand from this file.

**Acceptance Criteria:**
- [ ] `ctest -R audit_cleanup` passes on the finished branch and reports every violation (not only the first) when one is planted (check once by adding `// CalculatedValue` to a source file; not committed).
- [ ] Run against `master` (`cmake -DREPO=<a master worktree> -P ...`) it fails on the `GyroScaling` / `DataSchema` / `dataSchemaVersion` group - proof that the audit detects the superseded commit.
- [ ] Without a `.git` directory the test is not registered and configuration succeeds.

**Complexity:** M

---

### Task 8.6: Rewrite `docs/DATA_SCHEMA.md`

**Purpose:** Spec 10 bullet 1. The file does not exist on the branch; create it from scratch. Nothing of `master`'s import-time, stamping, or preference text may survive; the gyro-factor rationale may be reused.

**Files to create:** `docs/DATA_SCHEMA.md` (title: `# FlySight data schema, source preservation, and the conversion layer`).

**Technical Approach - section outline and the facts each section must state:**

1. **Summary** (5 lines): Viewer stores every measurement exactly as recorded; a fixed conversion layer produces the values everything else reads; only `SCHEMA_VER` selects schema corrections; saved logbook files contain recorded values.
2. **Recorded file format.** `$FLYS,1` (encoding / header syntax, not data semantics); `$VAR,<key>,<value>` - value is everything after the second comma, verbatim, commas allowed, unknown keys kept; `$COL,<sensor>,<columns...>`; `$UNIT,<sensor>,<units...>` (optional; fewer units than columns allowed, missing = empty); `$DATA`; rows `$<SENSOR>,v1,...`. FlySight 1 files (`time,lat,lon,hMSL,...` header line, unit line, rows; sensor `GNSS`; units kept verbatim, e.g. `(m)`). Fields ending in `Z` are ISO-8601 UTC timestamps parsed to seconds since the Unix epoch with millisecond precision; all other fields are doubles; `nan`, `inf`, `-inf` accepted. Rejected with an error (list from Phase 4 Task 4.5): unsupported `SCHEMA_VER`, empty `$VAR` key, conflicting duplicate `$VAR`, `$COL` problems (no sensor, no columns, empty or repeated column, duplicate `$COL`), `$UNIT` problems (unknown sensor, more units than columns, duplicate), missing `$DATA`. Tolerated: malformed or truncated data rows are skipped and counted in one log line; header-only files are valid.
3. **`SCHEMA_VER`.** Table: absent or `1` = legacy (gyro recorded with the nominal-range scale); `2` = corrected gyro, everything else unchanged; anything else or malformed (`3`, `abc`, empty, `2.0`, `+2`) = import error. Absence **is** the information: Viewer never writes, defaults, or rewrites `SCHEMA_VER` or `FIRMWARE_VER`; absence is interpreted as schema 1 only inside the conversion layer. `FIRMWARE_VER`, file names, dates, and the data never participate. `SCHEMA_VER` should be incremented only when the physical interpretation of recorded values changes, never for unrelated firmware fixes.
4. **Legacy gyroscope scaling.** LSM6DSO at +/-2000 deg/s: legacy firmware used `2000 / 32768` deg/s per count; ST specifies `0.070` deg/s per count; `0.070 / (2000 / 32768) = 1.14688`. Applies to recorded `IMU/wx`, `IMU/wy`, `IMU/wz` only. It is a nominal scale correction, not a per-device calibration, and cannot recover precision lost to rounding, clipping, or overflow. A recorded `IMU/wTotal` column is **not** corrected (not in the table); the derived `IMU/wTotal` and interpolated values use corrected inputs. Note for maintainers: the code holds the decimal literal `1.14688` once (`src/conversion/schematable.cpp`); the expression above evaluates to `1.1468800000000001` in double and must not replace it.
5. **Source and effective values.** One name, two layers, distinguished by access, never by renaming (no `wx_source`, no `source:wx`). Source = recorded samples + recorded unit text, immutable except through import, merge, or explicit programmatic replacement; explicit source access never computes and reports absence for derived names. Effective = what plots, calculations, logbook columns, and plugins read under the recorded names; computed lazily on first read, cached per session, never saved. Enumeration (`sensorKeys`, `measurementKeys`, `hasMeasurement`, `attributeKeys`, `hasAttribute`) describes stored data only. C++ names: `getMeasurement` / `effectiveUnit`; `sourceMeasurement` / `sourceUnit` / `hasSourceMeasurement` / `sourceData`. Python: link to `python_plugins/README.md`.
6. **The conversion layer.** Fixed, built in, not configurable per session, not extensible by plugins. Order: (1) schema correction selected by `SCHEMA_VER` + sensor + column, (2) unit normalization selected by the recorded unit text; the result carries the normalized label. Unit table exactly as `src/units/unitconversion.h` after Phase 4: `g` -> x 9.80665 `m/s^2`; `gauss` -> x 0.0001 `T`; `deg C` -> label `degC` (values unchanged, Celsius kept); identities `m`, `m/s`, `Pa`, `s`, `deg`, `deg/s`, `V`, `%`, empty, and the labels released Viewer versions wrote: `m/s^2`, `T`, `degC`; label-only aliases `(m)` -> `m`, `(m/s)` -> `m/s`, `(deg)` -> `deg`, `volt` -> `V`, `percent` -> `%`. Lookup trims the text; stored text is never trimmed. Unknown unit text = identity, label unchanged, silently (custom columns). Angular rates stay deg/s, angles degrees, temperatures Celsius. The metric / imperial display layer is separate and unchanged. Cost: identity columns share the source buffer; a converted column costs one extra buffer while cached.
7. **The escape hatch.** Procedure, numbered: (1) open the original `SENSOR.CSV` in a text editor, (2) add the line `$VAR,SCHEMA_VER,2` after `$FLYS,1`, (3) import it again. Result: the same session (matched by `SESSION_ID`) gains the attribute, gyro values are read literally, edits and markers are kept, no new session. If the file's explicit value differs from the session's explicit value (`1` versus `2`) the import fails with a message naming `SCHEMA_VER` and both values: delete the session and re-import its files. Caveat: matching needs a recorded `SESSION_ID`; a file without one is matched by content hash, so an edited copy imports as a new session. There is no preference, per-session switch, dialog, or badge.
8. **Importing and merging.** New `SESSION_ID` -> new session; known `SESSION_ID` -> merge, loaded or not (an unloaded session is loaded first; if it cannot be loaded the import fails and the stored file is untouched). Import-time defaults (`_DESCRIPTION`, `_IMPORT_TIME`, `_WIND_N/E`, `_JUMPER_MASS`, `_PLANFORM_AREA`, fixed `_GROUND_ELEV`, synthesized `SESSION_ID` / `DEVICE_ID`) only at creation. **Attribute conflict rule** for header attributes (keys without leading `_`): absent in session -> added; equal (exact text) -> nothing; different -> the file is rejected with an error naming the attribute and both values, session unchanged; absence on either side is never a conflict. Viewer attributes (`_` keys) in an incoming file: existing value wins, absent ones are added. `DEVICE_ID` placeholder `n/a` counts as absent. Measurements: incoming columns replace same-named columns (samples and unit text together) and add new ones; others are kept; source data only. A merge that would leave a sensor with columns of different lengths is rejected. Re-importing identical files changes nothing (no save). All validation happens before anything changes; errors appear per file in the import dialog.
9. **Saved session files.** Same FlySight 2 CSV format: `$FLYS,1`; every attribute as `$VAR` (header attributes exactly as recorded, `SCHEMA_VER` if and only if recorded, plus `_` attributes); `$COL` / `$UNIT` with recorded labels and unit text (`$UNIT` always written); `$DATA`; rows of **source** samples. Never effective values; warm and cold caches give identical bytes. Numbers: shortest decimal text that parses back to the identical double (rows and `$VAR`); `-0` keeps its sign; non-finite values are written as `nan`, `inf`, `-inf` (NaN payload not preserved) - never as zero. Time columns are saved as numeric seconds (as every released version has done), e.g. `1718900000.123`; `QDateTime`-typed attributes as UTC ISO-8601 with milliseconds. Attribute text: strings verbatim (commas allowed), line breaks replaced by one space, booleans `true` / `false`, integers exact. A sensor with columns of unequal length, a label or unit containing a comma or line break, or an unsupported `SCHEMA_VER` makes the save fail and leaves the previous file intact. Byte identity holds from the first Viewer-written file onward (the device file may differ textually: `62.50` -> `62.5`, ISO time -> seconds, `$VAR` order); values are identical from the device file onward.
10. **Existing logbooks.** Files from released versions contain normalized values (`m/s^2`, `T`, `degC`) and no `SCHEMA_VER`; they load unchanged, normalization is the identity, the gyro receives the legacy correction once at read time; saving does not rescale, relabel, or add `SCHEMA_VER`; no migration. Very old session files gain `_JUMPER_MASS` / `_PLANFORM_AREA` / `_WIND_N` / `_WIND_E` lines on their next save (legacy backfill; additive).
11. **Logbook column cache.** `index.json` caches column values of unloaded sessions; derived data. Root fields `calculationCompatibility` (integer, `FlySight::CalculationCompatibilityVersion`, unrelated to `SCHEMA_VER`) and `calculationEnvironment` (fingerprint of registered calculation ids and declared preference values). Missing or different -> all cached values are discarded and recomputed in the background; session files, ids, and access times are untouched. This happened once on upgrade to this version (gyro-dependent columns). Edits and merges refresh only the columns that can depend on the change; an interrupted save cannot leave cached values that disagree with the saved file. Developers: when to bump -> `docs/CALCULATIONS.md`.
12. **What Viewer never does** (bullets from spec 12): infer schema from firmware / file name / date / data; rewrite recorded header attributes; rename measurements; rescale stored data; show a dialog for legacy files.
13. **Adding a future schema version** (maintainers): add rows to `src/conversion/schematable.cpp` and the version to `supportedVersions()`; add a step in `src/conversion/sourceconversion.cpp` only if the correction is not a scale; bump `CalculationCompatibilityVersion`; add fixtures to `tst_schema_units` / `tst_conversion_engine`; update the table in section 3. No session attribute, preference, or control is added.

**Acceptance Criteria:**
- [ ] All thirteen sections exist; every numeric fact above appears with the same literal (`1.14688`, `9.80665`, `0.0001`, `0.070`, `2000 / 32768`).
- [ ] `git grep -niE "preference|stamp|Gyroscope scaling|override|Treat as|schema 2 with|normalized on read" -- docs/DATA_SCHEMA.md` has hits only in sentences that say such a thing does **not** exist (sections 7 and 12) and in "display ... preferences" of section 6; no sentence claims Viewer writes `SCHEMA_VER`.
- [ ] Every C++ identifier and file path mentioned exists (`git grep` each once).
- [ ] Links to `CALCULATIONS.md`, `../python_plugins/README.md`, `../tests/README.md` resolve.

**Complexity:** M

---

### Task 8.7: Developer note `docs/CALCULATIONS.md`

**Purpose:** Tell a contributor how to write a registered calculation without reading the engine source. Location decision: a separate file - `DATA_SCHEMA.md` addresses users and firmware developers; this note addresses C++ contributors. Both link to each other.

**Files to create:** `docs/CALCULATIONS.md` (about 150 lines).

**Technical Approach - sections:**
1. **Model in one paragraph:** registered calculation = id + declared inputs + declared outputs + pure `compute`; the engine owns results; one dependency graph; ids are dotted (`builtin.<area>.<name>`, `plugin.*`), never contain `#`.
2. **Minimal example:** the `builtin.time.fit` descriptor pattern of Phase 3 Task 3.3 (copy the real code from `src/calculations/timecalculations.cpp`), registered from `registerBuiltInCalculations` in `src/calculations/builtincalculations.cpp`.
3. **Declared inputs:** `CalcInput::attribute / measurement / preference`; **all are required**, in declared order (availability short-circuits); never declare "to be safe"; reads go only through `EvaluationContext`; an undeclared read makes every output unavailable (`ResultStatus::UndeclaredRead`) and is caught by `tst_builtins_engine::noUndeclaredReads`; no clock, RNG, `PreferencesManager`, or `SessionData` inside `compute`; ordinary calculations cannot declare source inputs (only the conversion families and, as the single exception, Python plugins using `source()` through `CalculationDescriptor::allowSourceInputs` - never set it in C++).
4. **Multi-output and partial results:** one computation = one calculation; set what you found, leave the rest unset; never pass an invalid `QVariant` to "clear"; published atomically; a user-stored attribute overrides one output while the others stay available.
5. **Candidates and order:** several calculations may declare one output; first in registration order whose inputs are all available and which produces the output wins; "works with or without X" = two registrations; registration order = the order in `registerBuiltInCalculations` (conversion families, attribute, gnss, imu, mag, time, simplification, wsp, sp, interpolation), Python plugins first; stored data always wins; cycles make every calculation on the ring unavailable - never rely on that.
6. **Parameterized families:** `CalculationFamily::instantiate(name)`; the interpolation key syntax; the two conversion families; instance ids `family#key`.
7. **Preferences:** compute-time preferences are declared inputs (today only `import/descentPauseSeconds`); preferences snapshotted into session attributes at creation are read as attributes; changing which calculations exist (altitude markers) is done by register / unregister, which invalidates every loaded session.
8. **Explicit policy:** the flag, `CalculationEngine::request(id)`, results revert to `NotRequested` when an input changes; nothing uses it yet; a future caller must publish the returned set through `SessionModel` (single `dependencyChanged` emitter) (F6).
9. **When to bump `CalculationCompatibilityVersion`** (`src/calculations/builtincalculations.h`): copy the rule from the constant's comment - arithmetic, inputs, or candidate order of a built-in; the schema table or unit table; the interpolation family; what `SessionModel::computeColumnValues` stores. Not for added / removed / renamed registrations (the environment fingerprint covers those). Never reuse a value, never 0.
10. **Testing a calculation:** private `CalculationRegistry` + `FakeSessionState` (`tst_builtins_engine` pattern); add golden rows to `goldenValues()`; extend the `inventory` literal; the oracle (`tst_session_oracle`) picks up new golden names automatically.
11. **Invalidation and the model:** mutate only through `SessionData` setters / `SessionMerge::apply`; every model-level mutation calls `invalidateColumns` / `invalidateAllColumns` (rule in `sessionmodel.h`).

**Acceptance Criteria:**
- [ ] Every identifier, id, and path in the note exists in the tree.
- [ ] The example compiles as written when pasted into a scratch registration function (checked once).
- [ ] The bump rule is textually consistent with the comment on `CalculationCompatibilityVersion`.

**Complexity:** S

---

### Task 8.8: Plugin documentation deltas

**Purpose:** Phase 7 Task 7.9 already covers spec 10 bullet 2 (source versus effective: README section 4; conflict rule: section 10; plugin changes: sections 5-9). Only the following gaps remain.

**Files to modify:** `python_plugins/README.md`, `python_plugins/flysight_plugin_sdk.py` (docstrings only).

**Technical Approach:**
1. **Links.** The README is installed next to the SDK, where a relative path does not resolve. Use `https://github.com/flysight/flysight-viewer-2/blob/master/docs/DATA_SCHEMA.md` with anchors `#the-conversion-layer` (README section 4) and `#importing-and-merging` (section 10), and mention the in-repo path `docs/DATA_SCHEMA.md` in parentheses. Add a link to `docs/CALCULATIONS.md` in section 3 ("the same rules bind built-in C++ calculations").
2. **Conflict-rule paragraph (section 10)** - extend with the two facts Phase 6 fixed after Phase 7 was written: Viewer's own `_` attributes from an incoming file never overwrite existing values (existing wins, absent are added), and the `DEVICE_ID` placeholder `n/a` counts as absent.
3. **Cached logbook columns (Phase 5 open question 3).** New short subsection under "Errors": Viewer caches logbook column values of unloaded sessions and invalidates them when the *set of registered calculation ids* changes, not when a plugin's code changes. After changing `compute`, either rename the plugin (its id changes) or remove and re-add the logbook column.
4. **SDK module docstring:** add one sentence - "Header attributes (`FIRMWARE_VER`, `SCHEMA_VER`, ...) are single-valued per session: a merged file with a different value is rejected (see README, 'Header attributes and the conflict rule')." Update "four main extension points" to five if Phase 7 has not already.
5. Verify against Task 7.9's checklist that the README still states each required topic after the edits.

**Acceptance Criteria:**
- [ ] `python -c "import ast; ast.parse(open('python_plugins/flysight_plugin_sdk.py').read())"` succeeds; no code line of the SDK changed (`git diff` shows docstring lines only).
- [ ] The README contains the absolute links, the `_` attribute sentence, and the cached-column subsection; `audit_cleanup` still passes (the README is excluded from the removed-name greps, F5).

**Complexity:** S

---

### Task 8.9: `tests/README.md` final form and root `README.md`

**Purpose:** Spec 10 bullet 3, and the project-level pointers.

**Files to modify:** `tests/README.md` (restructure into the final form; keep all correct text from Phases 1-7), `README.md` (baseline structure: TOC lines 5-29, Build Options table 207-214, Project Structure 276-309, CI/CD 413-421).

**Technical Approach - `tests/README.md` sections:**
1. *What this is* - Qt Test executables on `flysight_core`; one Python-embedding target; one audit script. Target list grouped by area (24 executables: the 22 above + `tst_session_oracle`, `tst_workflow`; plus `audit_cleanup`).
2. *Configure, build, run* - **Windows (superbuild):** the configure line of `README.md` "Full Build" plus `-DFLYSIGHT_BUILD_TESTS=ON`; `cmake --build build --config Release`; `ctest --test-dir build/FlySightViewer-build -C Release --output-on-failure`. **Direct / CI, macOS, Linux:** `cmake -G Ninja -S src -B build ... -DFLYSIGHT_BUILD_TESTS=ON`; `cmake --build build`; `ctest --test-dir build --output-on-failure`. Release only on Windows (GeographicLib and Python debug libraries are absent). CMake >= 3.22 needed for automatic DLL / `PYTHONPATH` resolution.
3. *Options* - `FLYSIGHT_BUILD_TESTS` (default `OFF`; no install rules, packaging unaffected); `FLYSIGHT_BUILD_PYTHON_TESTS` (default `ON`, effective only with the first; forwarded by the superbuild; NumPy missing -> the test is listed as **Disabled**, not omitted). Labels: `ctest -L core`, `-L python`, `-L oracle`, `-L audit`; `-LE python` to skip Python.
4. *Running one test / function* - `ctest -R tst_smoke`; direct execution with the `PATH` additions; `-v2`.
5. *Python bridge test* - Phase 7's text (how CTest sets `PYTHONHOME`, `PYTHONPATH`, DLL directory; `python -m pip install numpy`; running outside CTest; one interpreter per process; `slots` include order); per platform: Windows as above; macOS `BUILD_RPATH` note (unverified until CI runs it); Linux needs `python3-dev` + NumPy for the **build** interpreter.
6. *Isolation guarantees* - temp INI settings, test organization name, temp logbook folder, the `qFatal` guards, `PYTHONDONTWRITEBYTECODE`; "tests never touch your preferences or logbook"; how to verify (registry key / `index.json` timestamps).
7. *The idempotency oracle* - what `verifyAgainstFresh` means; `tst_calcengine_oracle` (synthetic) versus `tst_session_oracle` (real sessions); **reproducing a failure**: copy `seed=` from the message, run `set FLYSIGHT_ORACLE_SEEDS=<seed>` (PowerShell: `$env:FLYSIGHT_ORACLE_SEEDS='<seed>'`) then `ctest -R tst_session_oracle --output-on-failure`; the op log is printed before the failure; `QT_HASH_SEED=0` is set by CTest and must be set by hand when running the executable directly; soak run `1-500`; never "fix" a mismatch by removing a name from the catalogue.
8. *Writing a test* - Phase 1's conventions (`FLYSIGHT_TEST_MAIN`, builders, literals only, one class per executable, `tst_<area>`); register built-ins before `LogbookManager::initialize()`; restore global registry / preferences in `cleanup()`; add the function to `acceptance_map.txt` when it demonstrates an acceptance clause.
9. *Acceptance traceability* - the matrix of this document (item, clause, target :: function).
10. *Cleanup audit* - what `audit_cleanup` checks, how to run it alone, how to add a pattern.

Remove: the Phase 1 placeholder "Adding tests that need Python (Phase 7) - not implemented yet" if still present, and any `// BASELINE:` convention text (no such marker remains after Phase 6).

**Root `README.md`:**
- TOC: after "Quick Start" add `- [Data schema and conversion layer](docs/DATA_SCHEMA.md)`; after "Build Output Locations" add `  - [Running the Tests](#running-the-tests)`; after "Project Structure" add `- [Developer Documentation](#developer-documentation)`.
- Build Options table: keep `FLYSIGHT_BUILD_TESTS` (Phase 1), add `FLYSIGHT_BUILD_PYTHON_TESTS` | `ON` | "With tests enabled: build the embedded-Python plugin bridge test (needs NumPy in the build interpreter)".
- "Running the Tests" subsection (Phase 1 added it): the three Windows commands verbatim and a link to `tests/README.md`.
- Project Structure tree: add `docs/` (`DATA_SCHEMA.md`, `CALCULATIONS.md`), `python_plugins/` (`flysight_plugin_sdk.py`, `README.md`, `examples/imu_tilt.py`), `tests/` (`support/`, `python_plugins/`, `audit/`, `tst_*.cpp`), and expand `src/` to directory granularity: `engine/` (calculation engine: registry, per-session results, dependency graph), `conversion/` (schema table, source -> effective conversion), `calculations/` (built-in registered calculations, `builtincalculations.*` entry point), `units/`, `preferences/`, `ui/`, plus one line naming the libraries `flysight_model` (session data + engine) and `flysight_core` (import, export, logbook, model, calculations) and the notable new files `csvformat.*`, `sessionmerge.*`, `sessionimport.*`, `parsedfile.h`, `pluginadapters.*`, `pluginsessionview.h`. Remove any mention of `calculatedvalue.*` / `dependencymanager.*` left by earlier phases.
- "Developer Documentation" section (4 lines): links to the two `docs/` files, `python_plugins/README.md`, `tests/README.md`.
- CI/CD list (413-421): add "Runs the test suite on branch and pull-request builds (not on release tags)" iff Task 8.10 is adopted.

**Acceptance Criteria:**
- [ ] Every command in `tests/README.md` was executed once on Windows and works verbatim; macOS / Linux commands are marked "unverified locally" where they were not run.
- [ ] `README.md` TOC links resolve; the tree lists `src/engine/` and `src/conversion/`; `git grep -n "gyro scaling\|Gyroscope scaling" -- README.md` returns nothing.
- [ ] `tests/README.md` contains all ten sections, both option names, the seed-reproduction procedure, and the matrix; it does not contain `dataSchemaVersion`, `BASELINE`, or master's "Coverage includes ..." paragraph.

**Complexity:** M

---

### Task 8.10: CI - run the tests without affecting release-tag builds

**Purpose:** Finalize the optional proposals of Phase 1 Task 1.7 and Phase 7 into one decision.

**Files to modify:** `.github/workflows/build.yml` (triggers today: every branch push, tags `v*`, pull requests, `workflow_dispatch`; release steps are gated by `startsWith(github.ref, 'refs/tags/v')`).

**Technical Approach (decision: tests run on everything except `refs/tags/v*`):**
1. After "Setup Python" (301-306) add
   `- name: Install NumPy for bridge tests` / `if: ${{ !startsWith(github.ref, 'refs/tags/v') }}` / `shell: bash` / `run: python -m pip install numpy`.
2. In "Configure main application" (468-495), before the `cmake` call:
   `TEST_ARGS=""; if [[ "${{ github.ref }}" != refs/tags/v* ]]; then TEST_ARGS="-DFLYSIGHT_BUILD_TESTS=ON"; fi`
   and append `$TEST_ARGS` to the command.
3. Between "Build main application" and "Install application":
   `- name: Run tests` / `if: ${{ !startsWith(github.ref, 'refs/tags/v') }}` / `shell: bash` / `run: ctest --test-dir build --output-on-failure -C Release`.
4. If Phase 1 or 7 already added unconditional variants of these steps, replace them with the conditional forms above - one configure flag, one pip step, one ctest step in total.
5. Release-tag runs are byte-for-byte the old pipeline: no test targets are configured or built, so a test problem can never block or alter a release; tests have no install rules, so packaging is unaffected on branch builds too. The commit a tag points to has normally been tested by its branch push; note (project memory) that a simultaneous branch + tag push of one SHA shares a concurrency group, so only one of the two runs survives - push the branch first and wait for green before tagging.
6. `audit_cleanup` needs history for the diff-based UI check: checkout already uses `fetch-depth: 0`; if the tag is unavailable the script prints "skipped" for that group only.
7. Workflow edits cannot be validated locally (no `gh`, no push in this phase): make this **one separate commit**, tell Michael it is unverified, and list the likely follow-ups: macOS `libGeographicLib` / `libpython` lookup for test executables (`BUILD_RPATH` under `if(APPLE)`, Phases 1 and 7), Linux offscreen platform (not needed: tests use `QCoreApplication`).

**Acceptance Criteria:**
- [ ] The workflow diff is limited to the three additions (or their conditional replacements); YAML parses (`python -c "import yaml,sys; yaml.safe_load(open('.github/workflows/build.yml'))"`).
- [ ] No step that runs on `refs/tags/v*` changed its command line.
- [ ] The commit is separate and its message says it is unverified until pushed.

**Complexity:** S

---

### Task 8.11: Open-questions hand-off

**Purpose:** Give Michael one list to decide from; record his answers where the code is affected.

**Technical Approach:** Present the consolidated list below (section "Consolidated open questions") to Michael. For each answer that differs from the assumed default, open a follow-up task naming the documents' own "if Michael prefers ..." paths (each earlier phase document describes the alternative's blast radius). Update `docs/DATA_SCHEMA.md` / `docs/CALCULATIONS.md` / the plugin README where an answer changes a documented fact (Q3 precedence, Q9 time columns, Q12 `DEVICE_ID`, Q15 source inputs).

**Acceptance Criteria:**
- [ ] Every question has a recorded answer or an explicit "default accepted".

**Complexity:** S

---

### Task 8.12: Full build, full run, manual GUI verification, sign-off

**Purpose:** Acceptance 19, first sentence, and the parts of it that live in widgets.

**Technical Approach:**
1. **Build (Windows):** configure as in `README.md` with `-DFLYSIGHT_BUILD_TESTS=ON`; `cmake --build build --config Release`; then once more with `-DFLYSIGHT_BUILD_TESTS=OFF` (application, `flysight_cpp_bridge`, install step must succeed; no test target exists).
2. **Run:** `ctest --test-dir build/FlySightViewer-build -C Release --output-on-failure` from a shell without Qt or Python on `PATH`: 24 executables + `audit_cleanup` pass; `tst_python_bridge` is not "Disabled".
3. **Isolation proof:** note the modification time of `Documents/FlySight Viewer/logbook/index.json` and export `HKCU\Software\FlySight` before and after the run: unchanged.
4. **Manual GUI checklist - always on a COPY.** Copy a real released-version logbook folder (`Documents/FlySight Viewer`) to a scratch location and point Preferences -> General -> logbook folder at the copy (or start with an empty temp folder) **before** anything else; restore the preference at the end. Never open, import into, or edit the real logbook with a development build.

| # | Step | Expect |
|---|---|---|
| M1 | Start with the copied released logbook, a gyro-dependent logbook column enabled (measurement-at-marker on `IMU/wx`) | cells blank briefly, then fill with values about 14.7 % larger than the released build showed; `index.json` gains `calculationCompatibility`; no file under `sessions/` has a new modification time; no dialog |
| M2 | Import a legacy `TRACK.CSV` + `SENSOR.CSV` device folder | one session; description from the folder; no dialog, badge, or warning |
| M3 | Plot gyro (`wx`, `wy`, `wz`, `wTotal`) for that **legacy** session next to the released build | 1.14688 x the released build's values; acceleration, magnetometer, temperature, pressure, GNSS plots identical |
| M4 | Add `$VAR,SCHEMA_VER,2` to a copy of a `SENSOR.CSV` with a different `SESSION_ID` and import (**schema-2 file**) | gyro plots equal the numbers in the file - unchanged relative to the released build |
| M5 | Escape hatch on the M2 session (same `SESSION_ID`, add `SCHEMA_VER,2`, re-import); then a copy saying `SCHEMA_VER,1` | same session, gyro drops by 1/1.14688, edits intact; second import: dialog shows the file, `SCHEMA_VER`, both values, and the delete / re-import hint |
| M6 | Markers: drag exit / manoeuvre markers, "Reset to default" on a bubble, WS-P and SP docks change parameters and restore defaults | dependent markers, plots, and docks update; defaults return |
| M7 | Edit a description containing a comma; restart | logbook shows it; the CSV line is verbatim; only that `$VAR` line changed in the file (diff against a copy) |
| M8 | Preferences -> Import -> descent pause 30 -> 5 with a session plotted and many unloaded sessions | analysis-range markers move without re-import; exit-dependent logbook columns blank and refill; nothing under `sessions/` is rewritten. Mass / area changes move nothing |
| M9 | Preferences -> Altitude markers: add an altitude, remove it, switch units, with a session plotted and an altitude logbook column | markers appear / disappear, no stale values, column refreshes for loaded and unloaded rows |
| M10 | Re-import identical files | nothing happens: no save progress, file modification time unchanged |
| M11 | Conflicting `FIRMWARE_VER` copy; a folder with more than five broken files | dialog names attribute and both values; summary form lists ten files each with its reason; same dialog box as before (title, icon) |
| M12 | Plugin example: copy `python_plugins/examples/imu_tilt.py` next to the SDK in `build/install/python_plugins/`, restart | "Tilt pitch" / "Tilt roll" plots exist and plot in degrees; a scratch plugin that raises logs one traceback and the app keeps working; clean exit |
| M13 | Kill the process right after a description edit of a large session; restart | the logbook cell always matches the `_DESCRIPTION` line in that session's CSV |
| M14 | Look through Preferences and menus | no new preference, page, dialog, badge, or menu item compared with `v2026.04.1` |

**Acceptance Criteria:**
- [ ] Steps 1-3 done and recorded (command lines + ctest summary) in the phase report.
- [ ] M1-M14 each marked pass / fail with a note; the logbook-folder preference restored; the real logbook untouched (step 3 timestamps).
- [ ] `git status` clean apart from intended changes; nothing pushed.

**Complexity:** M (mostly manual time)

---

## Consolidated open questions for Michael

Default = what the plan currently assumes and the code will do unless told otherwise.

| Q | Phase | Question | Default |
|---|---|---|---|
| Q1 | 0 / 7 | Registration order: should Python plugins precede built-ins (a plugin declaring a built-in output wins)? | Yes - preserved from baseline; tested by `pluginBeatsBuiltinStoredBeatsBoth`. |
| Q2 | 0 / 6 | `_` attributes in an incoming Viewer-saved file on merge | Existing wins, absent are added, never a conflict. |
| Q3 | 1 / 8 | Should CI run the tests, and also on release tags? | Branch / PR builds yes; tag builds no (Task 8.10). |
| Q4 | 2 | Cycle rule has no formal proof for arbitrary overlapping cycles | Accepted; built-ins are acyclic; escalate if an oracle mismatch involves `Cycle`. |
| Q5 | 3 | Stale cached columns of unloaded sessions on preference / registration change | **Resolved** by Phase 5's environment fingerprint (see Q10). |
| Q6 | 3 | A few golden values (`_WSP_DIST_RESULT`, `_WSP_SPEED_RESULT`, some GNSS samples) are captured, not hand-derived | Keep captured literals. |
| Q7 | 4 | In-memory unsupported `SCHEMA_VER`: gyro as recorded + warning, or unavailable (needs an engine "blocking result")? | As recorded + warning; additionally such a session cannot be saved (Task 8.1d - new, confirm). |
| Q8 | 4 | Header strictness (duplicate `$COL`, extra `$UNIT` fields, missing `$DATA` reject the file) | Reject; downgrade a rule only if a real file trips it. |
| Q9 | 5 | Spec 9.2 "ISO timestamps at millisecond precision": time *columns* stay numeric seconds (as all releases); ISO only for `QDateTime` attributes | Numeric seconds. Alternative needs a per-column "recorded as ISO" flag (touches Phases 4-6). |
| Q10 | 5 | Environment fingerprint in `index.json` goes beyond the spec's integer marker | Keep. |
| Q11 | 5 / 8 | Changed Python plugin code does not invalidate cached columns | Documented workaround in the plugin README (Task 8.8). |
| Q12 | 5 | Orphan CSV with an already-indexed `SESSION_ID` shows as a duplicate row | Leave as is. |
| Q13 | 6 | `_DESCRIPTION` for file pairs outside a device folder depends on which file created the session | Leave; optional parent-folder rule. |
| Q14 | 6 | `DEVICE_ID` placeholder `n/a` treated as absent (alternative: write no placeholder) ; legacy mass / area / wind backfill kept as a shim ; all conflicts reported, not only the first | As stated. |
| Q15 | 7 | `allowSourceInputs` relaxes the letter of spec 7.5 so that plugin source reads are tracked | Keep (the alternative breaks spec 7.4). |
| Q16 | 7 | macOS / Linux behavior of `tst_python_bridge` in CI | Unverified until a push; follow-ups listed in Task 8.10. |
| Q17 | 8 | Developer note location | `docs/CALCULATIONS.md` (separate from `DATA_SCHEMA.md`). |

Resolved inside the plan and listed only for completeness: Phase 5 Q4a (ragged
merge -> Phase 6 rejects), Q4b (mutation rule -> Phase 6 follows it), Q4c
(backfill -> kept, Q14).

---

## Testing Requirements

### Unit Tests
- New targets: `tst_session_oracle` (8.3), `tst_workflow` (8.4); new CTest `audit_cleanup` (8.5).
- New functions in existing targets: `tst_calcregistry::staticDependenciesCoverOptInSourceInputs`, `tst_persistence_roundtrip::unsupportedSchemaIsNotSaved` (8.1); `tst_persistence_roundtrip::fileAndMemoryWritersAgree`, `tst_import_merge::unmatchedMeasurementsSurvive` (stub row), `tst_column_cache::altitudeMarkerRemovalDiscardsStubValues`, `tst_session_engine::explicitPolicyOnSession`, `tst_session_engine::safetyOnRealSession`, `tst_source_layer::onlySchemaVerDecides` (8.2); `tst_python_bridge::pluginWorkflowThroughModel` (8.4).
- No existing expectation changes in this phase. If an earlier test fails after the two tracks are merged, the cause is an integration defect (start with F1, F7), not the expectation.

### Integration Tests
- After every task: build with `-DFLYSIGHT_BUILD_TESTS=ON`; `ctest --test-dir build/FlySightViewer-build -C Release --output-on-failure`; after 8.1 and 8.5 also configure with `-DFLYSIGHT_BUILD_PYTHON_TESTS=OFF` and with `-DFLYSIGHT_BUILD_TESTS=OFF`.
- `ctest -L audit` and `ctest -L oracle` run alone.
- Soak (once, not in CI): `FLYSIGHT_ORACLE_SEEDS=1-500`.

### Manual Verification
- Task 8.12 checklist M1-M14, on a copied logbook only.

## Notes for Implementer

### Gotchas
- **You are reading `master`.** `docs/DATA_SCHEMA.md` and `tests/*` in the working tree before the branch switch are the superseded files. Write the schema document from the outline here, not by editing master's text.
- **Global state in tests.** `tst_session_oracle`, `explicitPolicyOnSession`, `safetyOnRealSession`, and the altitude tests mutate `CalculationRegistry::instance()` and preferences; snapshot `registeredIds()` in `init()` and compare in `cleanup()`. A `SessionModel` alive during a registry change schedules an environment check - destroy models before unregistering test calculations, or flush.
- **Registering built-ins precedes `LogbookManager::initialize()`** in every model-level test, or every reopen discards the column cache (Phase 5 gotcha) and `sessionsLoaded == 0` assertions fail.
- **Oracle op choice must never depend on hash order or the clock.** Choose from fixed lists; `QT_HASH_SEED=0` is a second line of defense, not a license.
- **`evaluateFresh` builds a temporary engine per name.** Full-catalogue checks are the expensive part; keep them at every 25 operations.
- **`SessionData(model.sessionRef(0))`** is a cold, state-only copy (Phase 3) - the intended way to get a cold twin.
- **In part B never call `sessionRef` just to verify** - it loads stubs, touches the LRU, and changes the sequence under test.
- **`62.5 * 1.14688` is `71.67999999999999`**: corrected gyro values are compared with `1e-9`, never `==` (Phase 4 rule); the same holds for `6.537216`, `1.72032`, `3.44064`.
- **The audit script's own patterns** would match themselves: `tests/audit` is excluded from every pathspec, and `tests/acceptance_map.txt` contains only test names.
- **`python_plugins/README.md` legitimately names removed APIs** ("What was removed"); it and `docs/` are excluded from removed-name greps (F5).
- **Documentation must not promise byte identity with the device file** - only value identity; byte identity starts with the first Viewer-written file (Phase 5 gotcha).
- **The logbook-folder preference is global.** For manual verification change it first, restore it last; a development build pointed at the real logbook would rewrite `index.json` (column-cache discard) even though session files are safe.
- **Workflow edits are unverifiable locally**; keep them in their own commit.

### Decisions Made
- **Audit is an automated CTest (`audit_cleanup`, a `cmake -P` script over `git grep`)** rather than a checklist, so "no old mechanism remains" stays true after this phase; the traceability matrix is machine-checked through `tests/acceptance_map.txt`.
- **Oracle in two parts:** a fast `SessionData`-level part with broad operation coverage (20 seeds x 300 ops, merges through `SessionMerge::plan` / `apply`), and a `SessionModel`-level part on the application's import path with restarts and a persisted-state oracle (6 seeds x 120 ops). Merge fragments are fixed and never ragged; conflicts are legitimate outcomes whose only obligation is "nothing changed".
- **End-state literal check** in part A (restore state, compare `goldenValues()`) so the oracle suite is not purely self-referential.
- **Workflow tests import only through `SessionImport::importFiles`** (F10); the `QList<SessionData>` overload stays a seam for the older suites.
- **Exporter refuses an unsupported stored `SCHEMA_VER`** (F9): "never write a file the importer rejects" is a persistence invariant worth one validation line; flagged as Q7.
- **No model-level `request` API** is added (F6); explicit policy is proven on a real session and documented.
- **Developer note is a separate file `docs/CALCULATIONS.md`;** `DATA_SCHEMA.md` keeps only the bump pointer.
- **Plugin README links are absolute GitHub URLs** because the README is installed without `docs/`.
- **CI:** tests on branch / PR / manual runs, skipped on `v*` tags so release builds are unaffected; `FLYSIGHT_BUILD_PYTHON_TESTS` is forwarded by the superbuild.
- **Earlier phase documents are not edited by this phase;** the amendments they need are listed under "Cross-phase findings" and in the phase report, and the code-level consequences are handled here.

### Open Questions
- Q3 (tests on tag builds), Q7 (exporter guard), and Q17 (note location) are new in this phase; all others are inherited. See the consolidated list; none blocks implementation with the stated defaults.

## Definition of Done

This phase is complete when:
1. All twelve tasks have passing acceptance criteria.
2. All 24 test executables and `audit_cleanup` pass via CTest on Windows Release; the application and `flysight_cpp_bridge` build with `FLYSIGHT_BUILD_TESTS` ON and OFF and with `FLYSIGHT_BUILD_PYTHON_TESTS` ON and OFF.
3. Every acceptance item 1-19 has at least one line in `tests/acceptance_map.txt`, every line resolves to an existing test function, and the matrix in `tests/README.md` matches it.
4. `docs/DATA_SCHEMA.md` and `docs/CALCULATIONS.md` exist with the outlined content; the plugin README, `tests/README.md`, and root `README.md` are final; no document describes import-time correction, schema stamping, or a gyro-scaling preference.
5. Spec coverage: 2 (out of scope: no new UI) - `audit_cleanup` UI group, M14; 7.4 / 7.9 - Task 8.3; 9.1 "warm and cold" - Task 8.2 / 8.4; 10 - Tasks 8.6-8.9; 11 items 1-19 - matrix; 12 - `audit_cleanup`, `onlySchemaVerDecides`.
6. The manual checklist was run on a copied logbook; the user's logbook and preferences are demonstrably untouched.
7. The consolidated open questions were handed to Michael; cross-phase amendments were reported to the coordinator.
8. No TODOs or placeholder code remain; the CI change is a separate, clearly labeled commit; nothing has been pushed.
