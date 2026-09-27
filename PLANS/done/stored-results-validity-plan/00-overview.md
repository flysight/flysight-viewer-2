# Implementation Plan: Stored results — validity that mirrors memory

## Feature Specification

The complete specification (`PLANS/stored-results-validity.md`, dated
2026-09-24) follows unabridged, headings demoted by one level. Its Commit
Policy section is reproduced as its own top-level section at the end of this
document and is not repeated here. The specification it amends,
`PLANS/store-requested-calculations.md`, is implemented on the working branch;
its implementation plan is archived in `PLANS/done/store-requested-calculations-plan/`
(its `00-overview.md` "Decisions & Constraints" and "Integration Notes" describe
the design as built, but the committed code is authoritative).

## Stored results: validity that mirrors memory

Date: 2026-09-24
Status: specification for planning. Not an implementation plan.
Baseline: branch `store-requested-calculations`, which implements
`PLANS/store-requested-calculations.md`. This document amends that
specification; where the two disagree, this one wins.
Related: `docs/DATA_SCHEMA.md` (sections 11, 12), `docs/CALCULATIONS.md`
(sections 5, 9, 12, 15, 17), `python_plugins/README.md`.

This document says what the change must do and the boundaries it must
respect. It avoids class layouts and function signatures unless a name is
part of the observable contract. The planning agent decides phasing; the
implementation agent decides code structure.

### 1. Motivation

A stored result (a requested calculation's result saved in the logbook's
`cache/` folder) is valid only while its record's code stamps match. One of
those stamps is the calculation environment fingerprint, which covers the
whole registry's candidate order and every declared preference value. So
adding an altitude marker, loading a different set of plug-ins, or changing
the descent-pause preference makes every stored fit in the logbook stale at
its next load, although none of them changes what a fit computes. In memory
the same fits survive those changes: the engine drops a result only when
something it actually reached changes.

A second problem: a record file that cannot be opened or read at load is
deleted as stale. On Windows a transient lock (antivirus, cloud sync) is
enough to destroy a good record that took minutes to compute.

### 2. Goal

A stored result goes stale under exactly the conditions that would drop the
same result in memory, plus a change of the code that computed it. Nothing
unrelated to what the result reached makes it stale.

### 3. When a result in memory is dropped

For reference, the engine drops an installed requested result when:

1. **A value it reached changes on its session**: the samples or unit text
   of a source measurement, or a stored attribute, reached directly or
   through the calculations and conversions its inputs resolved through,
   including a value it looked for and did not find that later appears.
2. **A declared preference it reached changes.**
3. **A registry change touches a name it resolved**, directly or
   transitively: a calculation is added or removed whose outputs include
   such a name, a calculation whose result it used is removed, a family that
   accepts such a name is added or removed, or the source-conversion layer
   changes.

Everything else leaves it installed: edits to values it did not reach
(markers, description, wind), registrations of names it never looked up
(altitude markers, unrelated plug-in outputs), and preferences it did not
reach.

### 4. Validity of a stored result

A record is valid at load when all of the following hold. Together they
replace section 4 of the amended specification.

1. **Inputs** (unchanged): the fingerprint over the values of every leaf the
   result reached, directly or transitively, present or absent, including
   declared preference values, matches the session and preferences as they
   are now. This covers conditions 1 and 2 of section 3.
2. **Resolutions** (new): for every name the result looked up while its
   inputs were gathered, directly or transitively, the record states what
   provided it: a calculation (its instance id and result version), the
   session's own data, or nothing. At load the same lookups are repeated
   against the current registry and must give the same answers. This covers
   condition 3 across runs.
3. **Code**: `CalculationCompatibilityVersion` and the calculation's own
   result version are unchanged.

The calculation environment fingerprint is no longer part of a record.
Section 4.2 of the amended specification loses it, and its bump-rule clause
reads: bump `CalculationCompatibilityVersion`, or the result version of the
calculation concerned, whenever a change can alter what a requested
calculation or anything it reads produces.

A record that fails any check is deleted when its session is loaded and the
calculation reads as not requested, as today. Nothing is recomputed on its
own.

### 5. Registry changes while the application runs

A registry change made while the application runs that drops an installed
requested result (condition 3) also deletes that result's record, exactly
like an input change. Tearing the registry down at shutdown, destroying or
evicting a session, and replacing a session's contents without an input
change delete nothing, as today.

A registry change that does not touch a result leaves both the installed
result and its record alone, and logbook column values over that record
remain cacheable.

### 6. Plug-in code identity

Built-in calculations change their arithmetic only with a new build, which
`CalculationCompatibilityVersion` covers. A Python plug-in can change between
runs under the same calculation id, so every calculation, measurement and
attribute a plug-in registers declares a result version: the plug-in code
identity.

The plug-in code identity is one digest over:

- every `.py` file in the plug-in folder (its name relative to the folder
  and its bytes, in name order), helper modules included;
- the plug-in SDK file;
- the Python version and the numpy version the plug-ins run on.

It is computed once when the plug-ins are loaded. Editing any plug-in file,
adding or removing one, or upgrading Python or numpy changes it, so a stored
result whose lookups went through any plug-in calculation goes stale at its
next load. A stored result whose lookups touched no plug-in calculation is
unaffected by plug-in changes.

The calculation environment fingerprint that stamps the logbook column
cache in `index.json` also covers every registration's result version, so a
plug-in edit or a built-in result-version change discards cached column
values as a registry change does today. The fingerprint's other contents
are unchanged, and its role for the column cache is unchanged.

### 7. Unreadable records

A record file that exists but cannot be opened or read in full when its
session loads is skipped for that load: it is neither restored nor deleted,
and the calculation reads as not requested for that load. The next load of
the session tries again. A new publish for the same pair replaces it;
deleting the session or the stray pass at start-up removes it.

A record that was read but is not a record, is damaged, or has a format
version this build does not read is deleted as stale, as today.

Logbook column values of a session that depend on a skipped record are not
cached in `index.json` for as long as the record stays skipped, so an
unloaded row never shows a value that disagrees with the record on disk.

### 8. Record format

The record gains the resolutions of section 4.2 and loses the environment
fingerprint, so its format version increases. A record of an earlier format
version is deleted as stale when its session loads. No migration.

### 9. Boundaries

- Everything in `PLANS/store-requested-calculations.md` not amended here
  still holds: restoring is not requesting, publishing writes the record and
  nothing else does (except the deletions listed there and in section 5),
  records are read only for a session being loaded, the session file is
  untouched, and a restored result is bit-identical to a fresh one.
- The engine keeps its threading rules. Repeating the lookups at load uses
  the same resolution the engine uses for a fresh request; it never runs a
  requested calculation.
- Plug-in loading stays a start-up operation. Nothing here reloads plug-ins
  or watches their files.

### 10. Tests

- A stored fit survives each of these and is restored with no job: adding
  and removing an altitude marker; registering and unregistering a
  calculation whose outputs the fit never looks up; changing the
  descent-pause preference; a different plug-in set whose calculations the
  fit never looks up; an application restart after any of these.
- Registering, while the application runs, a calculation that provides a
  name the fit looked up drops the installed fit and deletes its record.
- A record whose lookups resolve differently at load (a new candidate for a
  looked-up name, one that computes from the same inputs, registered before
  the load) is deleted and the fit reads not requested.
- With a synthetic requested calculation that reads a plug-in calculation's
  output: editing any plug-in file, adding one, or changing the recorded
  Python or numpy version between runs makes its record stale; editing a
  plug-in file when the requested calculation reads no plug-in output does
  not.
- The plug-in code identity is deterministic: the same files and versions
  give the same digest, and each listed ingredient changes it.
- A plug-in edit discards cached logbook column values over plug-in
  calculations at the next start.
- A record that cannot be read at load (for example held open without
  sharing on Windows, or a directory at its path) is kept, not restored, and
  restored at a later load once readable; its session's dependent column
  values are not cached while it is skipped.
- A record of the previous format version is deleted as stale.
- The existing tests of the stored-results feature pass, with the tests
  that asserted environment-change staleness rewritten to the rules above.

### 11. Documentation

`docs/DATA_SCHEMA.md` section 12 (validity, the record's resolutions, the
format version, unreadable records) and section 11 (column values over
skipped records, the environment fingerprint now covering result versions);
`docs/CALCULATIONS.md` sections 9 (the bump rule), 12, 15 and 17;
`python_plugins/README.md` (what the plug-in code identity covers and that
editing a plug-in stales stored results that used it); the user-facing
notes that said unrelated settings changes make stored results stale are
removed.

### 12. Principles

- A stored result is a memory of an in-memory result: it goes stale when
  the in-memory one would be dropped, and when the code changes, and at no
  other time.
- What a result reached decides its validity, never what else is
  registered.
- A transient failure to read is not evidence that a record is wrong.


## Phases

| Phase | Name | Purpose | Dependencies |
|-------|------|---------|--------------|
| 1 | Engine resolutions and registry drops | The engine snapshot records what every name the result looked up resolved to (calculation instance id + result version, session data, or nothing); restore repeats the lookups and refuses a mismatch as stale; a registry change made while the application runs that drops an installed requested result reports it like an input change, while registry teardown and destruction report nothing. | None |
| 2 | Plug-in code identity | One digest over the plug-in folder's `.py` files, the SDK file, and the Python and numpy versions, computed at plug-in load and declared as the result version of every plug-in registration; the calculation environment fingerprint (index.json column cache) also covers every registration's result version. | None |
| 3 | Record format 2, store and columns | The record carries the resolutions and drops the environment fingerprint (format version 2; version 1 deleted as stale); load validity per spec section 4; unreadable records skipped (kept, not restored) with their dependent column values never cached; runtime registry drops delete records; environment-change "unconfirmed" handling removed where it existed only for records; end-to-end tests of spec section 10. | Phases 1, 2 |
| 4 | Documentation, acceptance map and audit | `docs/DATA_SCHEMA.md`, `docs/CALCULATIONS.md`, `python_plugins/README.md`, `docs/COMPUTED_PLOTS.md`, `docs/SENSOR_FUSION.md`, `tests/README.md`; acceptance items for this spec; audit rules updated for the new boundaries. | Phases 1-3 |

## Dependency Graph

```
Phase 1 (engine) ─────┐
                      ├──► Phase 3 (record v2, store, columns, tests) ──► Phase 4 (docs, map, audit)
Phase 2 (plug-ins) ───┘
```

Phases 1 and 2 touch disjoint files and may be documented and implemented in
parallel. Phase 3 needs both (its plug-in staleness tests use Phase 2's
identity; its record carries Phase 1's resolutions).

## Key Patterns & References

All paths relative to the repository root; the branch head is ce2fb2b. Line
numbers are guides only.

### Engine (Phase 1)
- `src/engine/calculationengine.h` / `.cpp` — `ResolutionEntry {provider, instanceId, available, attribute, samples, unit}` (h ~323) in `m_resolutions`; `m_dependsOn`/`m_dependents`; `leafClosure()` (~1375, BFS through Resolution and Result nodes); `exportResult()`, `restoreResult()` and `RestoreOutcome` (stale checks ResultVersion, Bundle, InputsUnavailable, Leaves, Fingerprint); `gatherAsRoot()`, `installRequested(…, InstallOrigin)`; `invalidate(…, ExplicitDrops)`; `onRegistryChanged()` (~977, delivers with `ExplicitDrops::Suppress` today), `m_pendingRegistrySeeds`, `registryDestroyed()`, `clear()`; explicit-result listener (`ExplicitResultEvent {Installed | DroppedByInputChange}`), `deliverExplicitEvents()`.
- `src/engine/storedcalculationresult.h` / `.cpp` — `StoredCalculationResult` (members calculationId, resultVersion, detail, bundle, leaves, inputFingerprint), leaf kind codes, `inputFingerprint`, `sameContent`.
- `src/engine/calculationregistry.h` / `.cpp` — registration, `RegistryChange`, descriptor lookup (result versions), candidate order.
- `src/engine/calculationdescriptor.h` — `resultVersion`.
- `tests/tst_calcengine_restore.cpp`, `tests/tst_calcengine_async.cpp`, `tests/tst_calcengine.cpp` — engine test idioms (private registries, fake explicit calculations).
- `tests/tst_fusion_session.cpp` — `restoredFitIsIndistinguishable`, `explicitOutputsHaveOneCandidate`.

### Plug-ins and the environment fingerprint (Phase 2)
- `src/pluginhost.cpp` / `.h` — `PluginHost::initialise(pluginDir)`: interpreter boot, `sys.path`, SDK import (~305), import of every `*.py` in name order (~321-329), `registerEach()` over the SDK lists `_attributes`, `_measurements`, `_calculations` (~77-94, 339-347), plots and markers.
- `src/pluginadapters.cpp` / `.h` — `makeAttributeAdapter`, `makeMeasurementAdapter`, `makeCalculationAdapter` (~343-435) build `CalculationDescriptor`s.
- `python_plugins/flysight_plugin_sdk.py`, `python_plugins/README.md`, `python_plugins/examples/imu_tilt.py`.
- `src/calculations/builtincalculations.h` / `.cpp` — `CalculationCompatibilityVersion`, `calculationEnvironmentFingerprint()` (~42-80; candidate order + declared preference values), bump rules comment.
- `src/sessionmodel.cpp` — `checkCalculationEnvironment()`; `src/logbookmanager.cpp` — index stamps.
- `tests/tst_python_bridge.cpp`, `tests/python_plugins/` — plug-in tests (label `python`); `tests/tst_logbook_index.cpp` (`differentEnvironmentDiscards`, `environmentIsTheCachedOne`), `tests/tst_column_cache.cpp` (`upgradeDiscardsAndRecomputes`).

### Records, store, columns (Phase 3)
- `src/calculationrecord.h` / `.cpp` — `CalculationRecord` (stamps + snapshot), `stamped()`, `stampsAreCurrent()`, `CalculationRecordFormatVersion = 1`, `CalculationRecordStatus` (Ok, Missing, Unreadable, NotARecord, UnsupportedVersion, Corrupt), codec and layout comment, accepted attribute types.
- `src/calculationresultstore.h` / `.cpp` — `onExplicitResultEvent` (writes on Installed Ok, deletes on DroppedByInputChange, skips family instances), `restoreSession` (passes; staleDelete for Unreadable today), `Stats`.
- `src/logbookmanager.h` / `.cpp` — record API in `cache/`, known/unconfirmed/backed-on-disk sets, `markCalculationRecordsUnconfirmed`, `discardUnconfirmedCalculationRecords`, `validateRecordStamps`, `flushIndex` (skips values over unconfirmed ids), crash table above `writeCalculationRecord`.
- `src/sessionmodel.h` / `.cpp` — `restoreStoredResults`, `checkCalculationEnvironment` (marks loaded rows' records unconfirmed on an environment change), `settleExplicitColumns`, `onCalculationRecordsChanged`, eviction's `discardUnconfirmedCalculationRecords`.
- Tests: `tests/tst_result_records.cpp`, `tests/tst_result_store.cpp` (`staleRecordDeletedOnLoad` rows incl. environment, `noDeleteWithoutInputChange` registry row), `tests/tst_result_columns.cpp` (`environmentChangeDiscardsCachedValue`, `writeAfterStartupDropFlushesIndexFirst` indexNotValid row), `tests/tst_fusion_store.cpp` (`codeStampChangeDropsRecordOnLoad` environment row), `tests/support/logbookprobe.*`, `tests/support/testenvironment.*`, `tests/fusion/fusionsessions.h`.
- Altitude markers as a runtime registry change: `src/altitudemarkerfeature.cpp` (~41, ~175).

### Documentation and traceability (Phase 4)
- `docs/DATA_SCHEMA.md` §11-§12, `docs/CALCULATIONS.md` §5, §9, §12, §15.8, §17, `docs/COMPUTED_PLOTS.md` §6 (the "unrelated settings changes make stored results stale" notes), `docs/SENSOR_FUSION.md` §2/§7, `python_plugins/README.md`.
- `tests/acceptance_map.txt` (items 1-19, 101-120, 201-247, 301-350 today), `tests/audit/cleanup_audit.cmake` (group `stored-results`, range checker with hard-coded ranges), `tests/README.md` (§1 counts, catalogue, §8, §9 matrix and appendices, §10, §12 manual steps).

### Build and test commands (Michael's machine)
- Build only `build-phase1/`: `cmake --build build-phase1 --config Release`. Test: `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure` (labels core, fusion, exact, python, audit). **Never build `build/`.** Reconfigure the inner project with `cmake build-phase1/FlySightViewer-build` only after a new test source exists. A new test that links fusion must be listed in `_FLYSIGHT_GTSAM_REACHERS` in `cmake/SolverDependencies.cmake`.

## Decisions & Constraints

These bind every phase; a phase document may refine them, not contradict them.

### Resolutions (Phase 1 → Phase 3)
- The snapshot gains, for every Resolution node in the result's closure (the same walk as the leaves, through Resolution and Result nodes), a sorted entry: the public name and what provided it — `Calculation` with the provider's instance id and that registration's current result version, `SessionData` (stored attribute or source data, passthrough or conversion layer as the engine distinguishes), or `Nothing`. The ordering and the encoding are fixed by Phase 1 and consumed by Phase 3's record codec.
- Restore repeats the lookups through the same gathering as today (`gatherAsRoot`), derives the same list from the new graph, and refuses a difference with a new stale check (working name `Resolutions`), placed so that a resolution change is reported as such rather than as a Leaves/Fingerprint mismatch when both differ. The existing checks stay.
- The calculation's own result version stays a separate check (ResultVersion).

### Registry drops (Phase 1 → Phase 3)
- A registry change made while the application runs that drops an installed requested result delivers the same drop event as an input change (the store deletes the record). Teardown (`registryDestroyed()`, engine or session destruction, `clear()`) reports nothing. Phase 1 confirms which code paths remove registrations at shutdown (plug-in host teardown, altitude-marker feature destruction, registry destruction) and guarantees none of them deletes records; if a shutdown path unregisters calculations through the ordinary registry-change path, Phase 1 must distinguish it and say how.

### Plug-in identity (Phase 2)
- One digest (SHA-256, hex, with a fixed prefix such as `plugins-sha256:`) over: each `*.py` in the plug-in folder (relative name + bytes, name order, the same set the host imports), the SDK file's bytes, the Python version string and the numpy version string. Computed once in `PluginHost::initialise`, set as `resultVersion` of every attribute, measurement and calculation descriptor the host registers. If a version cannot be read (numpy missing), a fixed token takes its place. Deterministic and testable without Python where possible (the digest function takes its ingredients as data).
- `calculationEnvironmentFingerprint()` also covers every registration's result version (in candidate order). `CalculationCompatibilityVersion` is not bumped by this (the fingerprint change itself discards old caches once); Phase 2 states this in the bump-rules comment history if the project's rules call for it.

### Records, store, columns (Phase 3)
- Record format version 2: code stamps become `CalculationCompatibilityVersion` only (the environment fingerprint is removed from the record) plus the snapshot with its resolutions. Version 1 decodes as `UnsupportedVersion` and is deleted as stale on load.
- `Unreadable` (exists but cannot be opened or read in full) is skipped: not restored, not deleted, counted in the store's stats; the calculation reads not requested for that load. `NotARecord`, `Corrupt`, `UnsupportedVersion` and stale checks still delete.
- Column values of a session over a skipped record are not cached in index.json while it stays skipped (Phase 3 chooses the mechanism; the existing "unconfirmed" set is the natural fit) and are settled again at the next load.
- The environment-change path that marks loaded rows' records unconfirmed exists only because records carried the environment fingerprint; Phase 3 removes it (and anything else that exists only for that reason) while keeping the index-level discard of cached values on an environment change and the unconfirmed handling for failed writes.
- Tests that asserted environment staleness (`staleRecordDeletedOnLoad` environment rows, `codeStampChangeDropsRecordOnLoad` environment row, `environmentChangeDiscardsCachedValue`, `writeAfterStartupDropFlushesIndexFirst` indexNotValid, `noDeleteWithoutInputChange` registry row) are rewritten to the new rules, not deleted without replacement.

### Tests
- Spec section 10 is the test contract. Fusion-level tests use `tst_fusion_store`; engine-level tests use private registries (`tst_calcengine_restore`); plug-in identity tests go where the plug-in host is testable (`tst_python_bridge` or a core test of the digest function); record/store/column tests extend `tst_result_records`, `tst_result_store`, `tst_result_columns`. The "held open without sharing" unreadable case is Windows-only; the portable case is a directory at the record path (already used) — a Windows-only row must skip cleanly elsewhere.
- Every phase ends with the full suite green.

## Integration Notes

(Added by the planning coordinator after the phase documents are written.)

- **Phase 1 changes the record codec (accepted, Phase 1 D1).** Phase 1 adds the resolutions section to the record (after the outputs) with the format version still 1, so that restores keep working within Phase 1; records in the pre-Phase-1 layout decode as Corrupt and are deleted as stale. Phase 3 keeps that section, removes the environment stamp, bumps the format version to 2 once, and re-pins the layout tests.
- **Runtime registry drops reuse `DroppedByInputChange`** (Phase 1). Teardown is marked explicitly: `CalculationRegistry::unregister(id, Removal::Teardown)` / `RegistryChange::teardown`; the altitude-marker manager's destructor unregisters as teardown; the plug-in host never unregisters.
- **Results that met a dependency ring are not exported** (Phase 1 D5); restoring where gathering meets a ring is `Stale/Resolutions`. No built-in calculation is affected.
- **Stale-check order** is ResultVersion, Bundle, InputsUnavailable, Resolutions, Leaves, Fingerprint (Phase 1 D6); a lookup change reports `Resolutions`.
- **The plug-in identity digests every `*.py` under the plug-in folder recursively** (relative '/' names, `__pycache__` and hidden directories skipped), not only the top-level files the host imports; the import loop is unchanged. The shipped `examples/` are part of it. The digest lives in `src/plugincodeidentity.{h,cpp}` (core, no Python); the environment fingerprint's id line gains `#<escaped version>` only for registrations that declare a result version.
- **Implementation order: Phase 1, then Phase 2, sequentially** (both touch `tests/tst_builtins_engine.cpp`, `tests/CMakeLists.txt` and the `resultVersion` comment in `src/engine/calculationdescriptor.h`), then Phase 3, then Phase 4.
- Phase 1 hands to Phase 3: the store-level test that a runtime registry drop deletes the record; rewriting the environment-staleness test rows; removing the stale comment near `sessionmodel.cpp` ~1376. Phase 1 and 2 hand to Phase 4: acceptance entries for their new tests, README §8 note on unregistering while a model is alive, plug-in README and DATA_SCHEMA text (including that subfolders count).

- **Phase 3 decisions (accepted):** the store reads the union of listed and known record ids (so a directory at a known record's path reads Unreadable within a run; restart rows use a Windows lock or POSIX permissions and skip elsewhere); a record unrestorable only because it reads a skipped record is skipped too, not deleted; lookup-resolves-differently tests use test-owned providers re-registered to change their order. `markCalculationRecordSkipped()` reuses the unconfirmed set; `markCalculationRecordsUnconfirmed()` is removed. Phase 3 edits one acceptance-map line (the renamed `registryChangeKeepsLoadedRowConfirmed`); the rest of the map is Phase 4's.

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
- **Never staged:** `PLANS/`, `TEMP/`, `experiments/`, `build*/`, `dist/`,
  `results/`, and everything under `third-party/` that is not tracked.
- **One commit per accepted phase**, when the review agent returns ACCEPT,
  containing exactly the files the phase's agents reported, staged by explicit
  path. Subject `Validity phase N: <phase name>`; body a short summary, then
  the session's attribution line. Rejected iterations are never committed.
- **Tag** each phase commit `plan/stored-results-validity/phase-N-done`; tags
  are never moved or deleted.
- **Fixes to a closed phase** are committed separately as
  `Validity phase N fixup: <what>` once reviewed and accepted.
- **CI workflow edits** are committed separately as
  `Validity phase N: CI (unverified)`.
- **Escalated phases** are not committed; their paths are listed in the final
  report, and phases that depend on them are not started.

### Plan-specific notes
- Branch `store-requested-calculations`, head ce2fb2b when this plan was written (2026-09-24); the tree had no modified tracked files.
- Pre-existing untracked paths, never staged: `experiments/`, `third-party/gtsam-build/`, `third-party/gtsam/`, `third-party/oneTBB-build/`, `third-party/oneTBB/`.
- The attribution line is `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
