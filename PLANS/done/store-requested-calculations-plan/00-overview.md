# Implementation Plan: Storing requested calculation results with the session

## Feature Specification

The complete specification (`PLANS/store-requested-calculations.md`, dated
2026-09-23) follows unabridged, headings demoted by one level. Its Commit Policy
section is reproduced as its own top-level section at the end of this document
(as the spec requires) and is not repeated here.

## Storing requested calculation results with the session

Date: 2026-09-23
Status: specification for planning. Not an implementation plan.
Baseline: `master` if it contains branch `fusion-improvements`, otherwise
`fusion-improvements` (the fusion engine this document assumes).
Related: `docs/CALCULATIONS.md` (sections 8, 12, 15, 16), `docs/DATA_SCHEMA.md`
(sections 5, 9, 11), `docs/SENSOR_FUSION.md`.

This document says what the change must do and the boundaries it must
respect. It avoids class layouts and function signatures unless a name is
part of the observable contract. The planning agent decides phasing; the
implementation agent decides code structure.

### 1. Motivation

An explicitly requested calculation (`EvaluationPolicy::Explicit`; today
only sensor fusion) can take tens of seconds to minutes per recording. Its
result lives only in the loaded session's memory: it is not written to the
session file, and it is lost when the session is unloaded. Hidden sessions
enter the logbook's least-recently-used pool and are unloaded beyond the
cache capacity, so with a hundred tracks a user who shows them all, fits
them, hides them and shows them again has to fit half of them again; a
restart loses all of them. On-demand calculations are recomputed in
milliseconds and need nothing of this.

### 2. Scope

In scope:

- Storing, on disk, alongside the session, the result of every requested
  calculation that ended as a function of its inputs: a successful result
  with all its outputs, or a rejection or solver failure with its reason and
  diagnostics.
- Restoring such a result when the session is loaded, so that readers,
  plot rows and logbook columns see it exactly as if it had just been
  published, provided it is still valid.
- Validity: a stored result is used only while everything it was computed
  from is unchanged and the code that computed it is the same.
- Removing stored results with their session and dropping stale ones.
- Logbook columns that depend on a requested calculation, which today are
  cached as unavailable.
- Tests and documentation.

Out of scope:

- Storing on-demand or plugin calculation results.
- Starting any calculation implicitly. A stored result is a memory, not a
  request: when it is stale or missing, the calculation reads as not
  requested, exactly as today, until a gesture requests it.
- Any change to the session file (`sessions/<uuid>.csv`): its bytes, its
  format, what it enumerates. Section 5 of the data schema stays true:
  derived sensors never appear in enumeration or in the session file.
- Any change to the kernel, the job queue's gestures, or the plot rows'
  semantics beyond counting a restored result as computed.
- Sharing stored results between logbooks or machines.

### 3. What is stored

For each (session, requested calculation id) at most one record, written
when the result is published (the moment the engine installs it, on the
main thread) and replaced by the next publish for the same pair:

- the calculation id and the outcome: `Ok` with outputs, or `Ok` with the
  reason text of a rejection / solver failure and no measurements (the
  fusion kernel's `Rejected` and `SolverFailed`, which are functions of the
  inputs and are cached in memory today);
- every output the publish installed: measurements (name, samples, unit if
  any) and attributes (name, value), the diagnostics attribute included;
- the validity stamp of section 4.

Never stored: a result the engine did not install (`Cancelled`,
`ResourceExhausted`, a refused publish), and an `UndeclaredRead` or
`InvalidOutput` status. Such a run also deletes nothing: a record on disk
is removed only when it is found stale on load, when its session is
deleted, when the input it depends on changes (the same invalidation that
drops the in-memory result), or when the next publish for the same pair
replaces it.

Values round-trip bit for bit: a restored measurement equals the published
one in every double (the session file's rule for numbers applies:
shortest round-trip decimal, `-0` kept, non-finite written as such), and a
restored attribute string is byte-identical. The golden tests must not be
able to tell a restored fusion result from a freshly computed one.

### 4. Validity

A stored result is valid when all of the following match what is current
when the session is loaded:

1. **Inputs.** A fingerprint over the values the result depended on, as
   the engine's dependency records name them at publish: every source
   measurement (samples and unit text), every attribute and every declared
   preference that the calculation reached, directly or transitively. The
   fingerprint is computed from those values, not from the whole session
   file, so an edit the result does not depend on (moving a marker, a
   description) leaves it valid, and an edit it does depend on (a merge that
   adds IMU data, a changed `SCHEMA_VER`) invalidates it. The record lists
   the dependency names beside the fingerprint so the check can be
   repeated on load.
2. **Code.** `CalculationCompatibilityVersion` and the calculation
   environment fingerprint, the two stamps `index.json` already uses
   (`docs/DATA_SCHEMA.md` section 11), plus a per-calculation result version
   that the registration declares and bumps whenever the calculation's
   arithmetic changes (for fusion, the kernel's `algorithm` string serves).
   The compatibility-version rule of `docs/CALCULATIONS.md` section 9 gains
   the clause: bump it, or the calculation's result version, whenever a
   change can alter what a requested calculation produces.

A record that fails any check is deleted when the session is loaded and
the calculation reads as not requested. Nothing is recomputed on its own.

### 5. When results are restored, and what sees them

- On load of a session (from the logbook on start-up, on show, on
  reveal, on import-merge of an existing session), every valid record for
  that session is installed into the engine as a published result before
  any reader asks: the same outputs, the same status and detail, the same
  dependency edges as a fresh publish, so that later invalidation behaves
  identically (an input change drops it, and drops the record).
- Plot rows count a session with a restored result as computed: no refresh
  count, no job. A stale or absent record leaves the row exactly as today.
- Blocker inspection reports a restored result as it reports a published
  one (a failure's `NotProduced` with its detail included).
- Logbook columns that depend on a requested calculation are computed from
  the restored or published result when the session is loaded, and their
  values are cached in `index.json` like any other column, stamped so that
  dropping the record drops them. Section 11's "cached as unavailable"
  sentence no longer applies to a session with a valid record; without one
  the column stays unavailable, as today.
- Unloading a session (eviction, hide beyond the cache capacity, quit)
  loses nothing: the record is already on disk.

Publishing writes the record; nothing else does. Writing goes through the
logbook manager with the same atomicity as a session save (a temporary
file replaced at the end; a failed write leaves the previous record, if
any, intact and the in-memory result untouched; the write is tried again at
the next publish). A record is deleted when its session is deleted from the
logbook and when it is found stale.

### 6. Where

One record file per (session, calculation) in the logbook, next to the
session file, named from the session's file stem and the calculation id,
in a form the logbook's session scan (which lists `sessions/*.csv`) can
never mistake for a session. The encoding is the planner's choice under
these constraints: bit-exact round trip of doubles (section 3), one record
readable without the session, a size of the order of the session file
(seventeen fusion channels at IMU rate), and a version field so that a
future format can refuse or migrate an old record. Human readability is not
required.

Existing logbooks have no records: every requested calculation reads as
not requested until requested, as today. No migration.

### 7. Boundaries

- The engine keeps its threading rules: publish, restore and the writing of
  records happen on the main thread; the record's serialization may run on
  the worker only if it touches nothing the engine owns.
- The store never reads a record for a session that is not being loaded,
  and never starts a calculation.
- Purity holds: with or without a record, the value every reader sees for
  a requested output is the same function of the session's inputs.
- The session file is unchanged: saving a session with a stored result
  gives the same bytes as saving it without.

### 8. Tests

- A fusion fixture fitted, saved, unloaded and reloaded yields the
  seventeen channels and the diagnostics bit-identical to the goldens, with
  no job created; the row shows no refresh count.
- The same after an application restart (the test's logbook directory
  survives across two `SessionModel` lifetimes).
- A rejection and a solver failure are restored with their reason; the row
  shows the warning as today; no job runs.
- Editing an attribute the result does not depend on keeps the record;
  merging IMU data (or changing any dependency) drops it, the calculation
  reads not requested, and the file is gone.
- Bumping `CalculationCompatibilityVersion`, the environment fingerprint,
  or the calculation's result version drops the record on load.
- A record whose write fails (a read-only directory) leaves the in-memory
  result usable and the previous record intact.
- Deleting a session removes its records; a stray record whose session
  does not exist is ignored and removed by the next logbook scan.
- Logbook column over `Fusion/roll`: cached from a valid record, `unavailable`
  without one, invalidated when the record is dropped.
- Saving a session with a stored result produces the same session-file
  bytes as without it.

### 9. Documentation

`docs/DATA_SCHEMA.md` gains the record files (what they hold, when they
are valid, that the session file is untouched) and amends section 11;
`docs/CALCULATIONS.md` amends sections 8 and 9 and describes restore in
section 12 or 15; `docs/SENSOR_FUSION.md` replaces "results are kept in
memory only" by the new behaviour.

### 10. Principles

- A requested result is expensive and deterministic; keep it as long as
  its inputs and its code are the same, and not a moment longer.
- The session file is the recording. Derived data lives beside it, never
  in it.
- Restoring is not requesting: nothing starts on its own.
- A restored result must be indistinguishable from a fresh one, to the bit.


## Phases

| Phase | Name | Purpose | Dependencies |
|-------|------|---------|--------------|
| 1 | Engine snapshot and restore | The engine can export an installed Explicit result with the leaf inputs it reached and an input fingerprint, restore such a snapshot as if it had just been published (same edges, same status and detail, no compute), and tell its listener when an Explicit result is installed or dropped by an input change; descriptors gain a per-calculation result version (fusion: the kernel's algorithm string). | None |
| 2 | Record files in the logbook | A versioned, bit-exact binary record format for one (session, calculation) result, and the logbook manager's storage of record files beside the session file: atomic write, read, list, delete, removal with the session, stray removal on scan. | Phase 1 |
| 3 | Store on publish, restore on load | Session model wiring: publishing writes the record; every load path restores valid records before any reader asks; stale records are deleted; input changes drop the record; plot rows and blockers see restored results as computed; fusion end-to-end tests (spec section 8 except the column items). | Phases 1, 2 |
| 4 | Logbook columns over requested results | Columns that depend on an Explicit calculation are computed from the restored or published result for loaded sessions and cached in `index.json` with a stamp that ties them to the record set, so dropping or writing a record drops or refreshes them. | Phase 3 |
| 5 | Documentation, acceptance map and audit | `docs/DATA_SCHEMA.md`, `docs/CALCULATIONS.md`, `docs/SENSOR_FUSION.md`, `tests/README.md` and the code comments that state the old behaviour; a new acceptance item range mapping every spec clause to a test, manual step or audit rule; audit rules for the new boundaries. | Phases 1-4 |

## Dependency Graph

```
Phase 1 (engine) ──► Phase 2 (record format + logbook storage)
        │                         │
        └──────────┬──────────────┘
                   ▼
        Phase 3 (store on publish / restore on load, fusion tests)
                   ▼
        Phase 4 (logbook columns)
                   ▼
        Phase 5 (docs, acceptance map, audit)
```

Strictly sequential for implementation (every phase edits files the next one
builds on). Phases 1 and 2 may be documented in parallel because the snapshot
contract between them is fixed below.

## Key Patterns & References

All paths relative to the repository root. Line numbers are as of the branch
point (`fusion-improvements`, 8dc4e38) and are guides, not contracts.

### Calculation engine (`src/engine/`, widget-free core)
- `src/engine/calctypes.h` — `EvaluationPolicy` (28-31), `GraphNode` kinds StoredAttribute/SourceMeasurement/SourceUnit/Preference/Resolution/Result/Prepared (109-183), `ResultStatus` (199-207).
- `src/engine/calculationresult.h` — `CalculationResult`: per output `{available, attribute QVariant, samples QVector<double>, unit QString}`, `reason()`, output order (23-64).
- `src/engine/calculationengine.h` / `.cpp` — `ResultEntry` (h 237-244); maps `m_resolutions`, `m_results`, `m_dependsOn`, `m_dependents`, `m_prepared` (h 341-358); `computeResult` (cpp 466), `gatherInputs` (549-602, records edges), `acceptRun` (604-674), `publishEdges`/`setEdges`/`dropForwardEdges` (680-759), `invalidate` (798-839), leaf notifications `attributeChanged`/`sourceMeasurementChanged`/`sourceUnitChanged` (841-874), `clear` (876), `deliverBroadcast` (895), `onPreferenceChanged` (926), `onRegistryChanged` (933-998), `request` (1004), `dropNotRequested` (1025), `requestInstance` (1044), `prepare` (1095-1189), `publishPrepared` (1205-1281, install at 1273-1274), `resultStatus`/`resultDetail`/`cachedState` (1287-1318), `dependenciesOf` (1375, direct edges only), blockers `inspectInstance`/`inspectName`/`blockers` (1433-1587).
- `src/engine/preparedcalculation.h` / `.cpp` — ticket, `ComputedCalculation::Kind` (51-56), `PublishOutcome` kinds and reasons (71-95), `publish()` (cpp 92).
- `src/engine/blockerreport.h` — `UnproducedNote`, `BlockerReport::State`, `CalculationReadiness`.
- `src/engine/calculationdescriptor.h` — `CalculationDescriptor {id, title, inputs, outputs, policy, compute}` (23-38), `CalculationFamily` (43-66).
- `src/engine/calculationregistry.h` / `.cpp` — `registerCalculation` (h 88), `staticDependencies` (h 142), `dependsOnExplicit` (h 151, cpp 464).
- `src/sessiondata.h` / `.cpp` — engine ownership (`calculationEngine()` h 191, `m_engine` h 218), copy vs move semantics (h 131-142; copy-assignment clears the engine silently, cpp 45-57), `SessionKeys::FusionDiagnostics` (h 57).
- `src/dependencykey.h` — key types used by `GraphNode`.

### Fusion registration
- `src/fusion/fusionregistration.h` / `.cpp` — `FitCalculationId` (h 18), `fitInputs()` (cpp 95-105), `fitOutputs()` (138-145), rejection/solver failure published as `Ok` with `setReason` and diagnostics, no measurements (159-170), `registerFit()` (195-205), on-demand dependents `builtin.fusion.accH` (210) and `builtin.fusion.systemTime` (237), entry point (267).
- `src/fusion/fusionoutput.cpp` — `kAlgorithm = "batch-temperature-bias-v3"` (18), written into diagnostics (181, 210).
- `src/fusion/fusion.h` — fit outcomes Succeeded/Rejected/SolverFailed/Cancelled (33-38).
- `src/engine/calculationprogress.h` — `CalculationCancelled` (18).
- `src/mainwindow.cpp` — built-in and fusion registration (173-176), start-up (188-197), session delete (820-827).

### Compatibility stamps and column cache
- `src/calculations/builtincalculations.h` / `.cpp` — `CalculationCompatibilityVersion = 2` with its bump rules and history (h 19-38), `calculationEnvironmentFingerprint()` (h 40-62, cpp 42-80).
- `src/sessionmodel.h` / `.cpp` — column cache class comment (h 70-74); `computeColumnValues()` treats `dependsOnExplicit` names as unavailable (cpp 2028-2039); `checkCalculationEnvironment()` (cpp 1301-1325); column worker temp loads (cpp 1807-1815); `fillMissingColumns`.
- `src/logbookcolumn.h` / `.cpp` — column definitions.
- `src/logbookmanager.h` / `.cpp` — index read in `initialize()` (cpp 164-173 stamps, 155-228 extended format, 210-224 orphan adoption), `flushIndex()` (748-822, `m_cacheEnvironment` at 799-800), `discardCachedValues()` (389).

### Logbook manager and session model
- `src/logbookmanager.h` / `.cpp` — logbook directory and `sessions/` (cpp 124-136), save ordering and crash table (h 34-49, cpp 575-595), `saveSession()` (596-650), `loadSessionRaw()` / `loadSession()` (492-548), `sessionFileStems()` lists `*.csv` (656-667), `scanSessionFilenames()` (669), `remapSessionId` (681), `removeSession()` (719-742), `reset()` (256), `m_sessionIdToUuid` (h 207).
- `src/dataexporter.cpp` — atomic session write through `QSaveFile` (253-286): the atomicity pattern to follow.
- `src/sessionmodel.h` / `.cpp` — `m_cacheCapacity` (h 314, cpp 123-129), `mergeSessions` (cpp 546; new 600-616, loaded in place 617-630, unloaded 632-680), `populateFromUuids`/`populateFromIndex` (769, 791), `setRowsVisibility` (811-914, sync threshold 870-889), `removeSessions` (916), `sessionRef` (1027-1071, the central install point), `setFocusedSessionId` (1126-1158), `updateAttribute`/`removeAttribute` (1172, 1213), `attachSession` (1236-1249, engine listener), `publishCalculationInvalidation` (1438, h 271-279), pins (1458-1487), `loadNextVisibleSession` (1621), `evictIfNeeded`/`evictSession` (1673, 1692-1757), bulk edit temp loads that install (1951-1984).
- `src/preferences/preferencekeys.h` — `LogbookCacheSize` (132).

### Number formatting
- `src/csvformat.h` / `.cpp` — `formatDouble` / `parseDouble` / `formatAttributeValue` (the session file's number and attribute rules; `FloatingPointShortest` is confined to `csvformat.cpp` by the audit).

### Jobs and plot rows
- `src/jobqueue.h` / `.cpp` — contract (h 21-99), `request()` readiness check (cpp 173-192), `startNext`/`prepare` (311-331), `finishRun` → `publish` (423-488), `endJob` (490-515).
- `src/jobmodel.h` — `JobState`, `JobRecord.resultStatus`.
- `src/plotrequests.h` / `.cpp` — `PlotRowState::controlCount()` (h 80-97), the "restored plots are Missing after a restart" contract (h 153-166), `isExplicitBacked` (cpp 127), `isMerelyUncomputed` (216), `inspectUnderGuard` (226-241), `classify` (314-372), `recompute` (518).

### Tests
- `tests/CMakeLists.txt` — `flysight_add_test` (70-115), persistence tests (175-178), `flysight_add_fusion_test` with `flysight_fusion_session_support` (292-336), `flysight_add_fusion_exact_test` (418-449).
- `tests/support/testenvironment.h` — `useFreshLogbook()` (48), `reopenLogbook()` (51, simulated restart), `newTempDir` (44), `registerBuiltIns` (60), `waitForIdle` (75).
- `tests/support/logbookprobe.h` — on-disk probes (`readIndex`, `writeIndex`, `indexValue`, `sessionFilePath`, `sessionCsvFiles`, column helpers).
- `tests/fusion/fusionsessions.h` — `registerFusionOnce`, `fixtureSession`, `sessionWithoutImu`, `naturalSession`, `fusionNames`, `goldenDifference`, `addSessions`.
- `tests/fusion/fusiongolden.h` — `loadFusionGolden`, `compareSamples`/`compareJson`, `exactParityRequested` (`FLYSIGHT_FUSION_EXACT=1`), portable bounds; goldens in `tests/data/fusion/` (3 success fixtures, 9 `reject_*`, `capture.json`).
- `tests/tst_fusion_session.cpp` — in-memory engine behaviour of the fit (registration shape, publish together, async = sync, change after publication drops everything, rejection is a cached result, blockers).
- `tests/tst_fusion_jobs.cpp` — model + queue + fresh logbook (init 147-157), golden check (195), `columnOnFusionOutputIsNotCached` (510), `columnShowsValueStraightAfterPublication` (557), `shutdownDuringFit` (631).
- `tests/tst_fusion_rows.cpp` — `realRowScript` (266), `rejectedTrackShowsBadge` (548).
- `tests/tst_fusion_golden.cpp` — golden comparison of the kernel.
- `tests/tst_column_cache.cpp` — `upgradeDiscardsAndRecomputes` (226), `interruptedSaveViaModel` (696), `explicitBackedColumnIsNeverCached` (1278).
- `tests/tst_logbook_index.cpp` — environment stamps, orphan adoption, save ordering.
- `tests/tst_persistence_roundtrip.cpp` — bit-identical samples, non-finite round trip, save/reload cycle.
- `tests/tst_session_model_engine.cpp` — pinned session not evicted (638), calculation invalidation published (704).
- `tests/tst_calcengine_async.cpp` — async = sync, change after publication drops dependents, resource exhaustion not cached, ordinary exception is a cached failure; fake explicit calculations to copy.
- `tests/tst_calcengine.cpp`, `tests/tst_calcengine_blockers.cpp`, `tests/tst_calcengine_safety.cpp` — engine test idioms.
- `tests/README.md` — test catalogue (48-133), isolation rules (§6, 383-416), conventions (§8, 481-592), acceptance matrix (§9, 594+), audit (§10, 788), golden tolerance policy (§11, 1059).
- `tests/acceptance_map.txt` — four line forms (header 1-25); item ranges 1-19, 101-120, 201-247.
- `tests/audit/cleanup_audit.cmake` — groups and `expect_none`/`expect_only`/`expect_count` (27-43), the single `CalculationCompatibilityVersion` definition rule (186), `FloatingPointShortest` confinement (187), acceptance checker with hard-coded ranges (494-592, ranges at 567-568 and 573-591).

### Documentation
- `docs/DATA_SCHEMA.md` — §5 "Source and effective values" (112; "never saved" 124-125), §9 "Saved session files" (243), §11 "Logbook column cache" (291; explicit-backed columns cached as unavailable 314-317).
- `docs/CALCULATIONS.md` — §8 "Explicit policy" (152), §9 compatibility-version bump rules (168), §12 "Asynchronous request" (203), §15 "Background jobs" (422; "Nothing is persisted" 665), §16 "Plot-driven requests" (697), §17 logbook columns (1139-1146).
- `docs/SENSOR_FUSION.md` — §2 "Using it" (33; "Results are kept in memory only..." 45-46), §7 "Calculation lifecycle" (325).

### Build and test commands (Michael's machine)
- Test build: `build-phase1/` (Visual Studio 17 2022 superbuild, tests and fusion tests ON, exact tests AUTO, GTSAM from `build-solver-deps/`). Build: `cmake --build build-phase1 --config Release`. Test: `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure` (labels `-L fusion`, `-L exact`, `-L audit`, `-L core`). Running one executable by hand needs Qt's bin, GeographicLib, `build-solver-deps/GTSAM-install/bin` and `build-solver-deps/oneTBB-install/bin` on `PATH` (`tests/README.md` §2-4).
- **Never build `build/`**: it has third-party ON and would overwrite the irreplaceable Boost-enabled solver install.
- A new source file in the core library must be added to `src/CMakeLists.txt` (`flysight_core`, around line 264); a new test to `tests/CMakeLists.txt`.

## Decisions & Constraints

These bind every phase. A phase document may refine them but not contradict them.

### What is stored
- **Only an installed result with status `Ok` is stored.** For fusion that covers success and the input-determined `Rejected`/`SolverFailed` (published as `Ok` with `reason()` and the diagnostics attribute, no measurements). `Failed` (an ordinary exception), `UndeclaredRead`, `InvalidOutput`, `Cancelled`, `ResourceExhausted` and every refused publish store nothing and delete nothing.
- **Only Explicit calculations are stored.** On-demand and plugin results never are.
- **Both install paths write.** The async path (`publishPrepared`, used by the job queue) and the synchronous `request()` of an Explicit calculation both install a result; the engine notifies its listener for either, and the store writes on that notification. Writing is on the main thread; encoding may run anywhere as long as it touches nothing the engine owns.

### Snapshot contract between the engine (Phase 1) and the record format (Phase 2)
The engine exports and restores a plain value type (Phase 1 names it; working name `StoredCalculationResult`, header in `src/engine/`) holding exactly:
- the calculation id;
- the result version declared by the descriptor at publish (string; empty when the descriptor declares none);
- the reason / detail text;
- the installed `CalculationResult` bundle: output names in the bundle's order, and per output `available`, the attribute `QVariant`, the samples, the unit text;
- the dependency leaves: the sorted list of `(GraphNode kind, name)` for every StoredAttribute, SourceMeasurement, SourceUnit and Preference node reached from the result through the recorded edges, transitively (through Resolution and on-demand Result nodes), **including leaves that were looked at and absent**;
- the input fingerprint: SHA-256 over a canonical encoding of those leaves in that order: kind, name, present flag, and the value (source samples as little-endian IEEE-754 bits with every NaN canonicalised to one pattern and `-0` kept distinct; unit text as UTF-8; attribute values as the session file's text (`csvformat`'s attribute formatting); preference values in a documented canonical text form).

The code stamps (`CalculationCompatibilityVersion`, the environment fingerprint) are **not** part of the engine snapshot (the engine layer does not depend on `src/calculations/`); the store adds them to the record (Phase 2/3).

### Restore
- Restoring runs the same input resolution and gathering that `prepare()` runs (on-demand intermediates are evaluated as for a fresh request; they are cheap), so the edges recorded are exactly those a fresh publish records; it never runs the calculation's compute function.
- It then derives the leaf list and fingerprint from the current session exactly as at export; if the leaf list or the fingerprint differs, or the descriptor's current result version differs, nothing is installed and the caller is told the record is stale (with which check failed, for tests). Otherwise the stored bundle, status `Ok` and detail are installed through the same install step as a publish, so readers, blockers, `resultStatus`/`resultDetail`, invalidation and `dependenciesOf` cannot tell the difference.
- Restore is not a request: it creates no job, no ticket that outlives the call, and never starts a calculation. If the result is already installed (a fresh publish raced ahead), restore does nothing.
- Restore does not notify "installed" in a way that makes the store write the record again (restoring must not rewrite the file).

### When a record is deleted
- When its session is deleted from the logbook (`LogbookManager::removeSession`).
- When it is found stale on load (code stamps, result version, leaf list or fingerprint mismatch, unreadable or unknown format version).
- When the in-memory result is dropped **by an input change**: a stored-attribute, source-measurement or source-unit change on the session, or a preference change the result depends on. The engine tells its listener which Explicit calculation results such an invalidation dropped.
- **Never** when the drop is caused by `clear()` (session copy-assignment), a registry change, eviction, unload, engine destruction or shutdown. A registry change alters the environment fingerprint, so the next load's check removes the record.
- When a stray record is found at the logbook scan whose session file does not exist.
- A new publish for the same pair replaces the record (atomic replace).

### Where and how records are written
- One file per (session, calculation) in `sessions/`, named `<session file stem>.<calculation id>.<extension>` with an extension other than `.csv` (Phase 2 fixes it; e.g. `.fvresult`), so `sessionFileStems()`, the fallback scan and orphan adoption never see it. Phase 2 documents the name mapping for ids containing characters unsafe in file names.
- Written with `QSaveFile` (same atomicity as the session save). A failed write leaves the previous file untouched and the in-memory result installed; nothing retries until the next publish for the pair.
- Encoding: binary, `QDataStream` with a pinned stream version and explicit double precision (bit-exact doubles, byte-identical strings), a magic and a format version first, then the code stamps, then the snapshot. A future format version is refused (treated as stale). No compression required.
- Only the logbook manager touches record files. Records are read only for a session being loaded (installed into a model row), never by the column worker's temporary loads.
- Session-id remapping (`remapSessionId`) and anything that renames a session file must carry its records (Phase 2 confirms whether file stems can change; if they can, records move with them).

### Load paths that restore (Phase 3)
Every path that installs new contents into a model row restores that session's valid records before the row is published to readers: `sessionRef()` (start-up, show, focus, sync and background visibility loads), the bulk-edit load that installs, and all three import-merge branches (new session: has no records; loaded in place: its result was dropped by `clear()`, so re-restore after the merge; unloaded: restore after the merge and backfill). The column worker's temporary loads never restore.

### Validity code stamps
- `CalculationDescriptor` gains an optional result version (string). Fusion's fit declares the kernel's algorithm string, exposed from `src/fusion/` without duplicating the literal.
- The record carries `CalculationCompatibilityVersion` and `calculationEnvironmentFingerprint()` as current at write time; on load both must equal the current values (computed fresh, not the index's cached environment).
- `builtincalculations.h`'s bump rules and `docs/CALCULATIONS.md` §9 gain the clause "bump it, or the calculation's result version, whenever a change can alter what a requested calculation produces" (Phase 1 for the code comment, Phase 5 for the docs).

### Logbook columns (Phase 4)
- For a loaded session, a column whose name `dependsOnExplicit` is computed from the engine (restored or published value, or unavailable when not requested) instead of being forced unavailable, and cached in `index.json` with a stamp that ties the value to the session's record set for the Explicit calculations it depends on.
- The stamp must be checkable without reading record contents (directory metadata the manager already has, or a token the manager writes itself); a crash between writing a record and flushing the index must never leave a cached value inconsistent with the records on disk. Phase 4 chooses the mechanism and documents it.
- Writing or deleting a record for a session drops that session's cached explicit-backed values (recomputed at once for a loaded session). The column worker never restores: for an unloaded session with a record and no valid cached value it leaves the value pending until the session is loaded; for an unloaded session without any record it caches unavailable, as today.
- The existing tests `explicitBackedColumnIsNeverCached` and `columnOnFusionOutputIsNotCached` state the old behaviour; Phase 4 rewrites and renames them.

### Tests
- The golden comparison of restored fusion channels uses the existing golden machinery (`goldenDifference` / portable bounds, exact in exact mode) **and** a direct bit-for-bit comparison between the freshly published and the restored outputs (every double's bits, attribute strings byte-identical), which holds on every compiler.
- New fusion end-to-end tests go in a new `tst_fusion_store` target (`flysight_add_fusion_test`); engine-level tests with fake Explicit calculations extend `tst_calcengine_async` or a new `tst_calcengine_restore`; record-format and logbook-storage tests go in a new core test (e.g. `tst_result_records`) or `tst_logbook_index`. Tests must run on Windows, macOS and Linux CI: a write-failure test must not rely on directory permission bits (Windows ignores the read-only attribute on directories); use a failure the platform honours everywhere (for example, a directory occupying the record's path) or a documented test seam.
- An "application restart" is `FlySightTest::TestEnvironment::reopenLogbook()` plus a new `SessionModel`.
- Every phase runs the full suite (`ctest ... -C Release`) at the end; fusion tests have a 600 s timeout each.

### Risks and open questions
- Restore cost at load: gathering the fit's inputs evaluates on-demand intermediates on the main thread for every loaded session with a record. Expected to be milliseconds; Phase 3 measures it on a fixture and reports it.
- NaN payloads: the fingerprint canonicalises NaN, so a session saved and reloaded (which normalises NaN payloads through the text format) keeps its record valid.
- `PlotRequests`'s header contract ("restored plots are Missing after a restart") and `docs/SENSOR_FUSION.md` §2 become untrue in Phase 3; Phase 3 fixes the code comment, Phase 5 the docs.

## Integration Notes (binding, from the phase documents)

Recorded by the planning coordinator after Phases 1 and 2 were documented.
They settle the questions the phase documents raised.

- **Snapshot type** is `StoredCalculationResult` in `src/engine/storedcalculationresult.h`
  (public members `calculationId`, `resultVersion`, `detail`, `bundle`, `leaves`,
  `inputFingerprint` as 32 raw bytes). Leaf kind codes are pinned by Phase 1
  (0 StoredAttribute, 1 SourceMeasurement, 2 SourceUnit, 3 Preference) and the record
  format (Phase 2) uses the same helpers; there is one set of codes.
- **Record file name** is `<stem>.<encoded id>.fvresult`, the id percent-encoded outside
  `[a-z0-9_-]` (fusion: `<uuid>.builtin%2Efusion%2Efit.fvresult`). The record type and
  codec live in `src/calculationrecord.{h,cpp}` (core library), not in `src/engine/`.
- **`csvformat.cpp` moves from `flysight_core` to `flysight_model`** (Phase 1) so the
  engine's fingerprint can use the session file's attribute text without a library cycle.
- **The leaf walk goes through Explicit Result nodes too** (an Explicit calculation that
  reads another Explicit calculation's output depends on that one's leaves). Phase 3
  therefore restores a session's records in repeated passes until no further record
  restores, and treats a record as stale only when it still fails after the last pass
  (`InputsUnavailable` from an upstream Explicit that is not restored is stale then).
- **Restore refuses an inconsistent bundle** (`Bundle` stale check: undeclared output,
  detail differing from the bundle's reason) in addition to the overview's checks.
- **The engine's `Installed` event fires for every installed status**; the store writes a
  record only when the status is `Ok` and never deletes on an `Installed` event of any
  status (spec 3: such a run deletes nothing). A later `Ok` publish replaces the record.
- **`DroppedByInputChange`** covers leaf notifications, preference broadcasts and
  `dropNotRequested` cascades (an upstream calculation's result changed, which is an
  input change for its dependents). It never comes from `clear()`, a registry change or
  destruction.
- **Write-failure testing without a product seam** (Phase 2): a directory at the record's
  path, or an Explicit test calculation whose publish carries an attribute type the
  record format refuses; Phase 3's "previous record intact" test uses the latter.
- Phases 1 and 2 each add one row to `tests/README.md`'s catalogue; Phase 5 owns the rest
  of that file.
- **Loaded-in-place merge does not use `clear()`** (Phase 3 finding, correcting "Load paths
  that restore" above): `SessionMerge::apply` uses the ordinary setters, so a result the merge
  does not touch stays installed with its record, and one it touches is dropped by the input
  change and its record deleted. No re-restore is needed on that branch; both merge branches
  are tested.
- **Records for sessions not yet saved:** a record is written whenever the session has a
  logbook identity (file stem), whether or not its session file exists yet; a record whose
  session never gets saved is a stray at the next scan. (Phase 3 documents how the stem is
  available at import.)
- **Phase 3 changes `tst_jobmodel::nothingIsPersisted`** (Ok jobs now leave record files);
  Phase 5 updates the jobs-dock spec 8.4 acceptance-map entry accordingly. (Coordinator
  integration note: that "Spec 8.4" is §8.4 of the sensor-fusion-jobs specification,
  acceptance 18 = map item 118, which had no map line; Phase 5 adds
  `118 tst_jobmodel nothingIsPersisted` and rewrites the catalogue row.)
- **Risk for Michael (spec 4.2 as written):** the environment fingerprint covers the whole
  registry candidate order and every declared preference, so any registry change (e.g. an
  altitude-marker list edit) or declared-preference change makes every record stale at its
  next load; users may see refits after unrelated settings changes.

- **Phase 4 column stamp (accepted):** every session entry in `index.json` carries a
  `"records"` object (calculation id -> result version of each record on disk, `{}` when
  none). An explicit-backed cached value survives start-up only if the stamp matches the file
  listing. This supersedes Phase 2's statements that records are never referenced from the
  index and that writing a record never flushes the index: a record write flushes `index.json`
  first when the index on disk holds a value depending on that record.
- **Phase 4 adds `CalculationRegistry::explicitDependencies()`** (`dependsOnExplicit()` is
  rewritten on top of it); Phase 5 updates the audit comment at `cleanup_audit.cmake` 387-388.
- **Unconfirmed records (Phase 4):** after a failed write or an environment change, a loaded
  session's engine and the disk can disagree; values over such records are never written to
  `index.json` and eviction discards them.
- **Phase 5 documentation hand-offs** from Phase 4: `DATA_SCHEMA.md` §11 (the `"records"`
  stamp, pending values), `CALCULATIONS.md` §17 including the old test name near line 1160,
  `tests/README.md` §8 probe list, and the acceptance entry for the spec §8 column item.

## Commit Policy

Michael has authorized commits for this plan on a new working branch. The
implementation orchestrator makes every commit; implementation, revision, and
review agents never run git commands that change repository state. Nothing is
ever pushed, and nothing is ever committed on `master` or `fusion-improvements`.

- **Branch.** Confirm the working tree has no modified tracked files (if there
  are any, stop and ask). Base: `master` if it contains `fusion-improvements`,
  otherwise `fusion-improvements`. Create with
  `git switch -c store-requested-calculations <base>`; if it exists, switch to
  it; never recreate, reset, or rebase it.
- **Never staged:** `PLANS/`, `TEMP/`, `experiments/`, `build*/`, `dist/`,
  `results/`, and everything under `third-party/` that is not tracked.
- **One commit per accepted phase**, when the review agent returns ACCEPT,
  containing exactly the files the phase's agents reported, staged by explicit
  path. Subject `Phase N: <phase name>`; body a short summary, then the
  session's attribution line. Rejected iterations are never committed.
- **Tag** each phase commit `plan/store-requested-calculations/phase-N-done`;
  tags are never moved or deleted.
- **Fixes to a closed phase** are committed separately as
  `Phase N fixup: <what>` once reviewed and accepted.
- **CI workflow edits** are committed separately as `Phase N: CI (unverified)`.
- **Escalated phases** are not committed; their paths are listed in the final
  report, and phases that depend on them are not started.

### Plan-specific notes
- The branch `store-requested-calculations` was created from `fusion-improvements` (8dc4e38) on 2026-09-23; `master` does not contain `fusion-improvements`.
- Pre-existing untracked paths, never staged: `experiments/`, `third-party/gtsam-build/`, `third-party/gtsam/`, `third-party/oneTBB-build/`, `third-party/oneTBB/`.
- The attribution line is `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
