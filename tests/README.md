# FlySight Viewer tests

1. [What this is](#1-what-this-is)
2. [Configure, build, run](#2-configure-build-run)
3. [Options and labels](#3-options-and-labels)
4. [Running one test / one function](#4-running-one-test--one-function)
5. [The embedded-Python bridge test](#5-the-embedded-python-bridge-test-tst_python_bridge)
6. [Isolation guarantees](#6-isolation-guarantees)
7. [The idempotency oracle](#7-the-idempotency-oracle)
8. [Writing a test](#8-writing-a-test)
9. [Acceptance traceability](#9-acceptance-traceability)
10. [Cleanup audit](#10-cleanup-audit)
11. [Fusion golden regression](#11-fusion-golden-regression)
12. [Manual verification: plot-driven jobs](#12-manual-verification-plot-driven-jobs)

[Appendix A. The acceptance items (1-19)](#appendix-a-the-acceptance-items-1-19)
[Appendix B. The acceptance items of sensor fusion and plot-driven jobs (101-120)](#appendix-b-the-acceptance-items-of-sensor-fusion-and-plot-driven-jobs-101-120)

## 1. What this is

Qt Test executables that link the `flysight_core` static library (session
data, calculations, import/export, logbook, session model, job queue, plot
request logic, registries, unit conversion). They need Qt Core, Gui and Test
and GeographicLib only: no UI, no WebEngine, no KDDockWidgets, no QCustomPlot,
no Boost, and - with three kinds of exception - nothing else:
`tst_python_bridge` embeds Python, `tst_plot_row_delegate` links Qt Widgets,
and the tests labelled `fusion` link GTSAM (through `flysight_fusion`, or
directly). Each of the three can be switched off (section 3). One more CTest
entry, `audit_cleanup`, is not an executable but a CMake script (section 10).

The tests are not a standalone project. `tests/` is added by
`src/CMakeLists.txt` when `FLYSIGHT_BUILD_TESTS=ON` (default `OFF`), and every
test is registered with CTest. Test executables have no install rules, so
packages are the same whether or not the option is set.

There are 41 test executables plus the audit. `ctest -N` lists 42 entries, or
47 where the bit-exact runs of the five fusion golden tests are registered
(`tst_fusion_*_exact`: the same executables a second time, label `exact`,
Release only; sections 3 and 11). `solver_deploy_probe` and
`fusion_golden_capture` are also built, but are not tests (see below).

**Harness**

| Test | Covers |
|------|--------|
| `tst_harness` | The test-support code itself: settings and logbook isolation, fixture builders |
| `tst_smoke` | End-to-end characterization of importer, session, calculations, exporter, logbook, and model (started as a pin of v2026.04.1; expectations the rework changed on purpose were rewritten with the change that altered them) |

**Calculation engine (synthetic calculations)**

| Test | Covers |
|------|--------|
| `tst_calcregistry` | Calculation engine: value types, registration order and validation, family instances, private registries, registration-derived static dependencies (followed through source conversions) and `dependsOnExplicit()`, the one authority for "explicit-backed", source inputs refused outside source conversions |
| `tst_calcengine` | Calculation engine: resolution, caching, dependency recording, invalidation across sessions, explicit policy, preferences, families, measurement layers |
| `tst_calcengine_safety` | Calculation engine: nested scopes, cycles (single and overlapping rings: the same literal answers for every read order), exceptions, re-entrancy guards |
| `tst_calcengine_oracle` | Calculation engine: randomized (seeded) sequences, and randomized (seeded) topologies full of overlapping rings, compared against a fresh evaluation; the same with explicit calculations in play (blocker inspection, synchronous and asynchronous requests, requests refused because an input changed), where an explicit calculation may run only in a request or publish step |
| `tst_calcengine_async` | Calculation engine: the asynchronous request (prepare / compute / publish) with compute inline, on a `std::thread` and on a `QThread`: identical to `request()` in every observable, captured inputs, engine-decided staleness (an input changing while compute is running, transitive changes, registration removed, engine destroyed, a synchronous request in between), `willBeRefused()` / `refusalReason()` reporting the engine's mark for every cause of a refusal that can be known before publish (false for a healthy ticket, false again once published or refused), cancellation, progress text, resource exhaustion never cached, an ordinary exception cached as `Failed`, abandoned tickets leaving nothing behind |
| `tst_calcengine_blockers` | Calculation engine: blocker inspection: an explicit calculation reported through on-demand intermediates, missing input is never a blocker, chained explicit calculations (A then B), "ran and did not produce" with the reason or failure text, fallbacks, stored and unknown names, an explicit family instance, rings, `readiness()`; inspection never runs an explicit calculation and never changes a later read |

**Built-in calculations and sessions on the engine**

| Test | Covers |
|------|--------|
| `tst_builtins_golden` | Every built-in calculation read through `SessionData` on the generated descent fixture, against hand-derived golden literals |
| `tst_builtins_engine` | The built-ins on a private registry and `FakeSessionState`: golden values, registration inventory, declared inputs only, multi-output groups, candidate order, the declared preference, interpolation family, altitude descriptor |
| `tst_time_fit` | The system-time-to-UTC fit: microsecond-level conversion of an exact synthetic clock at high device uptime (the regression test of the centered sums), invalidation through the TIME sensor, GPS week rollover, degenerate clocks |
| `tst_local_coordinates` | The recording-wide `Local` frame: origin gates, analytically known displacements and velocity rotation on WGS84, NaN at the index of an invalid sample only, all outputs unavailable without a qualifying fix, the shared GNSS time axes, invalidation on source changes and independence from markers on a real `SessionData` |
| `tst_simplified_track` | The simplified map track on the shared `Local` frame: all seven outputs at the same retained sample indices, every dropped sample within 0.5 m of the path and the strictly-greater rule, duplicate-position endpoints, closed, degenerate and empty tracks, non-finite samples left out, one projection per recording, unavailable without a local origin and back after a source correction, siblings invalidated together |
| `tst_session_engine` | `SessionData` on the engine with the real built-ins: run-once, invalidation, candidate replacement, overrides, preferences, the fresh-evaluation oracle, copy/move semantics; an explicit-policy calculation and a throwing / nested / cyclic set of calculations registered temporarily on the global registry (acceptance 14, 12); an asynchronous request against real session ownership (published, session moved, destroyed, move- and copy-assigned over, declared input edited) |
| `tst_session_model_engine` | `SessionModel` + `AltitudeMarkerManager`: registry and preference broadcasts reaching `dependencyChanged`, coalescing, merges, rows surviving sort, the read-only `DEVICE_ID` column, a marker only for an altitude whose calculation registered; the job queue's hooks: counted session pins that defer LRU eviction (and nothing else), and immediate publication of engine-returned invalidations without any persistent effect |
| `tst_session_oracle` | The session-level idempotency oracle (section 7): randomized, seeded sequences of reads, edits, merges, preference and registry changes on real `SessionData` objects (part A) and on the real `SessionModel` / `LogbookManager` through the application's import path, with restarts, simulated crashes and a persisted-state check (part B), compared against a fresh evaluation |

**Jobs and plot rows** (synthetic explicit calculations; no GTSAM)

| Test | Covers |
|------|--------|
| `tst_jobqueue` | The application-wide `JobQueue` on a real `SessionModel`, real session engines and the global registry, with the synthetic explicit calculations of `jobfixture.h` (no GTSAM): publication through the session model, the 64 MiB worker thread, deduplication, one job at a time in request order, refusals (missing input, unloaded or unknown session, blocked, done, unknown), never loading a session, every superseded / succeeded / failed / cancelled path with its reason text, a running job whose ticket went stale (input edit, merge, registration removed, model destroyed, the row's session data replaced - "Session data replaced", not "removed or unloaded") asked to stop at once and ended Superseded with the refusal's reason without its compute reaching the end, a new request behind it created rather than deduplicated and run with the new inputs, first writer wins between a user cancel and a stale stop, cancel wins over a completed compute, pruning of unwanted queued jobs, a job cancelled from a `rowsInserted` slot (no pin left, no `jobQueued` after its `jobFinished`), session removal, deferred eviction, repopulation, merge, sort, shutdown in every order, and the idle scheduler working during a job (sensor-fusion-jobs acceptance 8, 10, 11, 14, 17) |
| `tst_jobmodel` | The `JobModel` contract under `QAbstractItemModelTester`: every role on every column, a test view that renders the whole job history from model signals alone, never more than one running row, ordered UTC timestamps, progress and cancel-requested as their own signals, removal of finished rows only, the retention bound, and nothing persisted (sensor-fusion-jobs acceptance 18) |
| `tst_plot_requests` | `PlotRequests`, the widget-free logic behind the plot list's rows, on a real `PlotModel`, `JobQueue`, `SessionModel`, real session engines and the global registry, with the synthetic plots of `plotfixture.h` (no widgets, no GTSAM): track conditions and row aggregation (counts, progress label, tooltip text, change signals, coalesced passes, text-only progress updates), ordinary and unchecked plots never inspected, hidden rows, stubs and failed-load placeholders are not tracks, the two gestures as one-shot requests, chained continuation and everything that stops it, everything that is not a gesture (programmatic checks, profile apply, startup restore, showing, loading, merging, input invalidation, a superseded job), a running job gone stale showing refresh instead of cancel at once and a refresh queueing a new job behind it, sessions without input never listed, the failed badge and its reason, `PlotRequests::isMerelyUncomputed()` (the plot widget's "No data available" warning is withheld for a value that waits on an explicit calculation or was rejected by one, and for nothing else: one case per blocker state, and asking runs nothing), rows sharing a job, cancel, cancel then refresh while the job winds down, pruning of queued jobs on uncheck and hide, session removal, registry changes, queue shutdown, and null collaborators (sensor-fusion-jobs acceptance 11, 13, 15, 16) |
| `tst_plot_row_layout` | `layoutPlotRow()` (`src/ui/docks/plotselection/PlotRowLayout.h`), the pure geometry of a plot-list row's control cluster, without widgets or a font: control only, control and warning, warning only, nothing shown, an empty label, right-to-left as the exact mirror image, and the control's hit rectangle (the icon's column over the full row height, out to the row's edge; label and badge outside it) |
| `tst_plot_row_delegate` | `PlotRowDelegate` in an offscreen `QTreeView` on a real `PlotRequests`, `PlotModel`, `JobQueue` and `SessionModel`, driven by synthesized mouse and key events; the **only test that links Qt Widgets** (`FLYSIGHT_BUILD_WIDGET_TESTS`, label `widgets`). What the view owns: plain rows are pixel-identical to the base delegate, the control is painted and the name elided rather than the cluster, a click on the check box and Space are the check gesture while unchecking, `setPlotEnabled()`, `togglePlot()`, `setData()` and a start-up style restore with the view attached start nothing, refresh and cancel clicks (no toggle, no selection change), press/release pairs that must do nothing, the inert label and badge, right clicks, a double click on refresh, the tooltip from `PlotRowState`, repaint on `rowStateChanged`, and survival of a destroyed `PlotRequests` (sensor-fusion-jobs acceptance 16, wiring half). The offscreen platform has no fonts, so text is drawn as boxes; the assertions are about geometry, identity and events, not about letter shapes |

**Source layer, conversion layer, importer**

| Test | Covers |
|------|--------|
| `tst_schema_units` | The two tables behind the conversion layer: the schema table (`SCHEMA_VER` validation, which measurements each version corrects) and the unit normalization table (silent, identity for unknown text) |
| `tst_conversion_engine` | The conversion families on a private registry and `FakeSessionState`: legacy gyro correction, `SCHEMA_VER` 1 / 2 / absent / unsupported, unit normalization, schema-then-unit order, buffer sharing for identity conversions, the dependencies that make the choice follow the attribute |
| `tst_importer` | `DataImporter`: data stored exactly as recorded, nothing stamped, `SCHEMA_VER` and structural errors rejected without touching the target session, `$VAR` values kept verbatim, malformed rows skipped with one summary warning, FS1, custom columns, CRLF; `parseFile` carries nothing the file did not say (match id synthesized from the bytes but not stored), `applyCreationDefaults` is the one writer of import-time defaults and only fills absent keys, header-only `peekHeaderAttribute` |
| `tst_source_layer` | Session-level acceptance for the source / effective split on real `SessionData`, importer, exporter, logbook and model: acceptance 1, 2, 4, 6 (load), 16; enumeration and source access never compute; lazy conversion; buffer sharing; exporter and merge use the source layer; only `SCHEMA_VER` decides (not the firmware version, the file name, or the recording date) |

**Persistence and the logbook column cache**

| Test | Covers |
|------|--------|
| `tst_csvformat` | `CsvFormat`, the one definition of the on-disk text forms: shortest round-trip doubles (a 200 000-value bit-pattern sweep), `-0`, `nan` / `inf` / `-inf`, attribute values by `QVariant` type, line-break flattening, valid names and units |
| `tst_persistence_roundtrip` | Save / reload on the real importer, exporter and logbook: acceptance 5 (bit-identical samples, units and header attributes preserved, `SCHEMA_VER` only if recorded, effective values unchanged, second cycle byte-identical, independent of any cache) and acceptance 6 (a released logbook file is not rescaled, relabelled or stamped by a save; the `loadSession` backfill is additive and idempotent); non-finite samples, ragged sensors, unrepresentable text; the file writer and the in-memory writer agree (also across the 4 MB flush boundary); an unsupported stored `SCHEMA_VER` is never written |
| `tst_logbook_index` | `LogbookManager`'s `index.json` column cache: the calculation-compatibility marker and environment fingerprint gate the cached values (acceptance 18 at the storage level), unsaved-column tracking and save ordering (an interrupted save never leaves a cached column that disagrees with the session file), orphan session files adopted, marks follow remap / remove / reset; the raw load with its failure reason, the legacy backfill as a separate step, identity-entry queries, a legacy flat index coming up as stubs without rewriting a session file |
| `tst_column_cache` | The same through `SessionModel`: upgrade discards and lazily recomputes (acceptance 18), an edit refreshes only the affected columns with a warm and a cold engine, merges and bulk edits, interrupted saves, environment changes (declared preference, altitude-marker registrations) discarding loaded and unloaded rows without saving, save failures (the row stays dirty and loaded, is skipped by the idle saver and the LRU, stays out of the index, and is saved by a later edit or the shutdown flush), line breaks flattened at edit |

**Import and merge, workflows**

| Test | Covers |
|------|--------|
| `tst_session_merge` | `SessionMerge`, the pure plan-then-apply merge (attribute conflict rule, measurement merge) on programmatic sessions: absent attributes added, equal ones ignored, different header attributes conflict (all reported, sorted, with the delete-and-re-import hint), `_` attributes keep the session's value, the `n/a` device placeholder counts as absent, equality on the on-disk text form, columns replaced / added / kept with samples and unit together, bitwise column comparison (NaN, `-0`), the ragged rule, purity of `plan()`, the invalidation set of `apply()` |
| `tst_import_merge` | The import path (`SessionImport::importFiles` -> `SessionModel::mergeSessions`) against a temporary logbook: acceptance 3 (a rejected file leaves the session untouched), 7 (TRACK/SENSOR order independence loaded, unloaded and in one batch; conflicts change nothing; edits and unmatched measurements survive), 8 (the `SCHEMA_VER` escape hatch), 10 and 18 (merge parts); defaults only at creation, failed loads are errors, failed-load placeholders are never saved, identity stubs are matched, identical re-imports are no-ops |
| `tst_import_batch` | `SessionImport`: one result per file in input order with parse failures included, cancellation through the progress callback, and the text of the import-failure dialog with each file's error |
| `tst_workflow` | End-to-end workflows on the application's own code path (acceptance 19): import through `SessionImport::importFiles`, rows and columns, marker and attribute edits, save, reopen as stubs served from `index.json`; the model's warm save equals a cold export (acceptance 5); a released session file together with a released `index.json` (acceptance 6, 18) |
| `tst_map_models` | `TrackMapModel` and `MapCursorDotModel` on a real `SessionModel`: a recording without a local-frame origin has no track and no cursor dot and is left out of the bounds, the bounds are cleared when no track remains, and all of it returns after a source correction through `mergeSessions`; hidden recordings and the plot-range filter on a recovered track. It compiles the two map models and their helpers (`plotrangemodel.cpp`, `plotutils.cpp`) directly and needs no Widgets or WebEngine |

**Python plugin bridge**

| Test | Covers |
|------|--------|
| `tst_python_bridge` | The Python plugin bridge through the real embedded interpreter and the real `flysight_cpp_bridge` module (acceptance 17, plugin half): effective reads in single-output plugins, declared-read diagnostics (`UndeclaredInputError`), effective values and units matching C++, no source access (a `source` key is an unknown kind; the view has no source methods), the multi-output form running once, exceptions and malformed output giving a clean unavailable result with negative caching, returned arrays copied, explicit key decoding with per-plugin rejection, plugin-before-built-in precedence, the bundled `imu_tilt.py` example; plugins never start explicit work (`pluginsNeverStartExplicitWork`, on the synthetic explicit calculation `expA`: a plugin that declares its output, one that declares an on-demand value derived from it, and one whose Python code reaches for both undeclared all stay unavailable with a run count of 0 until the calculation is requested, and the first two show the value afterwards). See "The embedded-Python bridge test" below. `pluginWorkflowThroughModel`: a plugin-fed logbook column through import, save and restart (acceptance 17 / 19) |

**Solver and sensor fusion** (label `fusion`; `FLYSIGHT_BUILD_FUSION_TESTS`)

| Test | Covers |
|------|--------|
| `tst_solver_smoke` | GTSAM's exported CMake target compiles, links and runs in a test: the install is the shipped configuration (`4.3a0`, TBB on, bundled Eigen 3.4, built without Boost: `GTSAM_ENABLE_BOOST_SERIALIZATION` and `GTSAM_USE_BOOST_FEATURES` are `0`), a small pose graph optimizes to its analytic answer (Eigen, METIS, TBB, library loading), and the main thread really has the 64 MiB stack of `flysight_solver_stack()` (the test uses 48 MiB of it; with a default stack it crashes). Label `fusion`. Nothing from the fusion model is involved |
| `tst_fusion_golden` | The fusion kernel (`flysight_fusion`) through its public API, `src/fusion/fusion.h`, only: golden regression against the kernel itself. For every committed synthetic fixture the fit reproduces the goldens captured from the kernel by `fusion_golden_capture` (three successes: seventeen channels and the diagnostics; nine rejections: the exact reason), the progress texts at the kernel's boundaries are the golden's, the channel writer of the capture tool is the inverse of the loader on the committed files (and the hex sample form round-trips signed zero, a NaN and a subnormal by bit pattern), cancellation at each kind of boundary leaves an empty result and no state behind (preparation has none: the first boundary is `Starting fit`; the initializer's prefix and segment fits have the same kinds as the full fit), two runs are bit-identical with TBB on, a 64 MiB worker thread matches the main thread, and nothing depends on the caller's data (sensor-fusion-jobs acceptance 4; section 11). Label `fusion` |
| `tst_fusion_kernel` | The kernel's internals, with the literal expectations of the reference's self-test: the shared unwrap rule, preintegration across exact boundaries, every validation defect, backward attitude propagation, heading freedom, dense reconstruction timing and endpoint correction, the segmented initializer (segment cutting on fixes, the smallest-sAcc anchor, prefix growth on the marginal yaw sigma, the fallback when every prefix start fails, its progress texts, and the five synthetic recordings of the specification), an exact constant-velocity fit, non-convergence as a solver failure, TBB really on, and the fit trace (the segment account and cost before and after every optimizer iteration) against the goldens, which localizes a golden failure to a stage (acceptance 4; section 11). The only test that includes internal `src/fusion/` headers. Label `fusion` |
| `tst_fusion_session` | Sensor fusion as a registered calculation (`src/fusion/fusionregistration.cpp`) on real `SessionData` engines bound to the global registry, with the real fit on the test's main thread: the shape of the three registrations (21 inputs, 18 outputs, explicit, title "Sensor fusion"); fixture sessions whose effective inputs are bit-identical to the kernel's fixtures; reads of every fusion value, `accH`, the system-time axis, the diagnostics and an interpolated logbook value never run the fit, in any order, nor does the exporter (sensor-fusion-jobs acceptance 5); a request runs once, publishes all outputs together, matches the kernel's goldens, and brings `accH` and `_system_time` with it (6); prepare / compute / publish equals `request()` bit for bit (7); an input change after publication drops everything while markers do not (8); a rejection is a cached result with its reason, `NotProduced` for inspection, and requestable again after an input change (9); cancellation at each kind of boundary through the engine's facility publishes and caches nothing (10); sessions without IMU data, without a local origin or without a time fit are `MissingInput` / `NotApplicable` (11); blocker inspection reports the fit through on-demand intermediates and never starts it (12); a natural session through the real input chain; two sessions are independent. Label `fusion` |
| `tst_fusion_jobs` | The real fit through `JobQueue` on a real `SessionModel`, on the queue's 64 MiB worker: one job publishes all outputs together and announces them through the session model (acceptance 6); the queue gives the bits a synchronous request gives (7); an input edit during the fit asks the fit to stop at once, ends the job Superseded, publishes nothing, and leaves it requestable (8); a rejected recording is a Succeeded job carrying the reason, with nothing to do on re-request and a fresh run after an input change (9); cancel during the fit publishes nothing and the next job starts afterwards (10); a session without IMU data cannot have a job (11); the logbook column over `Fusion/roll`, the column worker and the saver never start a fit (5), and that column is cached as unavailable before and after a published fit, while the loaded row's cell shows the golden's number the moment the job publishes (`dataChanged` for that row only, and the number already there when the view is told); shutdown during a fit. Mid-run actions are taken in a slot on the job's first progress text, which the queue delivers before the job's end: no gate, no sleeps. `realRecordingCheck` is the optional local check of section 11 and skips unless `FLYSIGHT_FUSION_RECORDING` is set. Label `fusion` |
| `tst_fusion_golden_exact`, `tst_fusion_kernel_exact`, `tst_fusion_session_exact`, `tst_fusion_jobs_exact`, `tst_fusion_rows_exact` | Not executables: the five tests above that compare with the goldens, run a second time with `FLYSIGHT_FUSION_EXACT=1` and otherwise the same environment, so that every golden comparison is bit equality (section 11, "Tolerance policy"). Registered only where that is a fair demand, the compiler the goldens were captured with (`FLYSIGHT_FUSION_EXACT_TESTS`, section 3). The first two decide bit-identity; the other three show that the bits survive the engine, the queue's worker thread and the plot rows. Labels `fusion` and `exact` |
| `tst_fusion_rows` | The plot-row script with the **real** fusion plots: `PlotModel` + `PlotRequests` + `JobQueue` + `SessionModel` + the fusion registration, with real fits on the queue's 64 MiB worker and the seventeen plots of `fusionPlots()` (`tests/fusion/fusionsessions.h`, which mirrors `MainWindow::registerBuiltInPlots()`; `audit_cleanup` pins the application's list at seventeen rows). All seventeen plots are explicit-backed and the six local-frame plots are not; the row script of acceptance 15 on three real tracks (three jobs, the count falling as each publishes, unchecking mid-way removes the queued job and lets the running one finish, cancel leaves the plot checked and the track missing with nothing published, a fourth track is missing with a count of one and starts nothing, refresh computes it), with every published track held to the kernel's goldens and the job history as a literal; roll, pitch and yaw share one job and one progress text; `accH` is blocked by the fit and never has a job of its own; a session without IMU data is in no count, list or tooltip of any of the seventeen rows before, during and after a fit (11); a rejected recording shows the warning badge with the reason, offers no retry, and becomes refreshable when its input changes (9); sessions are edited, tracks hidden and shown and other values read while a real fit runs, without disturbing it (19, the half that needs no widget). Steered by the first progress text of a job and by `jobFinished`: no gate, no sleeps. Label `fusion` |

Two executables are built with these but are not tests and are not counted
above. `solver_deploy_probe` is a plain executable (no Qt) around the same
pose-graph exercise (`solverprobe.h`). A QtTest executable cannot run inside an installed
application tree, because Qt Test is not deployed; this one can. Copy it into
the installed tree, run it with a minimal `PATH`, and remove it again: it
starts only if every solver library was deployed.

```bash
# Windows (Git Bash); the CI workflow does the same on non-tag builds
cp build/FlySightViewer-build/Release/solver_deploy_probe.exe build/install/
(cd build/install && PATH=/c/Windows/System32 ./solver_deploy_probe.exe)   # prints "solver probe ok, error=..."
rm build/install/solver_deploy_probe.exe
```

`fusion_golden_capture` is the other: it runs the product kernel on the twelve
synthetic fixtures and writes the goldens of `tests/data/fusion/` (section 11,
"The capture tool" and "Re-capture procedure").

**Audit**

| Test | Covers |
|------|--------|
| `audit_cleanup` | No old mechanism remains, each fact has one authority, none of the mechanisms of `sensor-fusion-clean-port` that have no successor exists, the structural rules of background work hold (one worker, no locks, GTSAM confined, gestures from the row delegate only, a widget-free core), and every line of `tests/acceptance_map.txt` resolves (section 10) |

The `tst_calc*` tests drive `src/engine/` with synthetic calculations against
`FakeSessionState` / `FakePreferenceProvider` (`support/fakesessionstate.h`).
Each builds its own `CalculationRegistry`; none registers anything in
`CalculationRegistry::instance()`.

The built-in calculations are pinned by one golden table
(`support/builtinfixture.*`: `DescentFixture` generates a 296-row jump plus a
three-row sensor file; `goldenValues()` holds literals only). Rows marked
"captured" were recorded from the v2026.04.1 engine before the migration; all
others were derived by hand. `tst_builtins_engine` uses private registries
(except `fingerprintSurvivesRuntimeAltitudeMarker`, which drives the real
`AltitudeMarkerManager` on the process-wide one and removes what it added);
`tst_builtins_golden`, `tst_session_engine` and `tst_session_model_engine` use
the process-wide registry through `TestEnvironment::registerBuiltIns()`, and
must leave it as they found it (altitude-marker registrations are removed in
`cleanup()`).

Ordinary reads (`getMeasurement`) return *effective* values: the recorded data
passed through the conversion layer, which `registerBuiltIns()` registers along
with the other built-ins. A test that asserts effective values must therefore
call `TestEnvironment::registerBuiltIns()`; without it no conversion family
exists and effective == source. Corrected gyro values are compared with an
absolute tolerance of `1e-9` (`62.5 * 1.14688` is not the double nearest
`71.68`); values whose conversion is exact (`1 g`, `1 gauss`, identities) are
compared with `==`. Test helpers that move data between sessions
(`DescentFixture::load`, `copyStoredState`) use the source accessors, so the
copies carry the data as recorded. Tests that count warnings install their
message handler after `registerBuiltIns()`.

Tests that use `LogbookManager::initialize()` together with cached column
values must call `TestEnvironment::registerBuiltIns()` first, as the
application does: the calculation-environment fingerprint captured at
`initialize()` is compared with the one of the next start, and registering in
between would make every reopen discard the cache. For the same reason set the
logbook columns (`LogbookColumnStore::setColumns`, which flushes the index)
before planting a hand-edited `index.json`. `tst_logbook_index` and
`tst_column_cache` remove whatever they register globally in `cleanup()`.

## 2. Configure, build, run

**Windows (superbuild).** Third-party dependencies must already be built in
`third-party/*-install` (see the top-level `README.md`).

```bash
cmake -G "Visual Studio 17 2022" -A x64 -B build -S . -DCMAKE_PREFIX_PATH="C:/Qt/6.9.3/msvc2022_64" -DGOOGLE_MAPS_API_KEY="your-api-key" -DFLYSIGHT_BUILD_THIRD_PARTY=OFF -DFLYSIGHT_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build/FlySightViewer-build -C Release --output-on-failure
```

The superbuild forwards `FLYSIGHT_BUILD_TESTS` to the application project, whose
build tree is `build/FlySightViewer-build`; that is the directory CTest runs
against. To stop building the tests, configure again with
`-DFLYSIGHT_BUILD_TESTS=OFF`.

**Windows (application project directly, as CI does):**

```bash
cmake -G "Visual Studio 17 2022" -A x64 -S src -B build-app -DCMAKE_PREFIX_PATH="C:/Qt/6.9.3/msvc2022_64" -DFLYSIGHT_BUILD_TESTS=ON
cmake --build build-app --config Release
ctest --test-dir build-app -C Release --output-on-failure
```

**macOS / Linux** (single-configuration generators: no `--config`, no `-C`):

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DFLYSIGHT_BUILD_THIRD_PARTY=OFF -DFLYSIGHT_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build/FlySightViewer-build --output-on-failure
```

On Windows only the **Release** configuration is supported for tests: only a
Release `GeographicLib.dll` exists in `third-party/GeographicLib-install/bin`.

The macOS / Linux commands, and the application-project form on those
platforms, are **unverified locally** (development happens on Windows); CI is
where they run. CMake 3.22 or newer is needed for the automatic DLL and
`PYTHONPATH` resolution; with an older CMake the directories have to be put on
`PATH` by hand (section 4) and `tst_python_bridge` is disabled.

## 3. Options and labels

| Option | Default | Effect |
|--------|---------|--------|
| `FLYSIGHT_BUILD_TESTS` | `OFF` | Adds `tests/` to the application build. No install rules: packaging is unaffected |
| `FLYSIGHT_BUILD_PYTHON_TESTS` | `ON` | Only with the first: also build `tst_python_bridge`. `OFF` removes the target. If NumPy is missing from the build-time Python the test is still built but listed as **Disabled**, not omitted (section 5) |
| `FLYSIGHT_BUILD_WIDGET_TESTS` | `ON` | Only with the first: also build `tst_plot_row_delegate`, the one test that links Qt Widgets (it runs an offscreen `QTreeView`). `OFF` removes the target, and then no test target links Widgets. Forwarded by the root `CMakeLists.txt` like the others |
| `FLYSIGHT_BUILD_FUSION_TESTS` | `ON` | Only with the first: also build the GTSAM-linked tests (`tst_solver_smoke`, `tst_fusion_golden`, `tst_fusion_kernel`, `tst_fusion_session`, `tst_fusion_jobs`, `tst_fusion_rows`), `solver_deploy_probe` and the golden capture tool `fusion_golden_capture`, all defined in one block of `tests/CMakeLists.txt` (the tests through `flysight_add_fusion_test()`). `OFF` removes the targets, and then no test target references GTSAM. Forwarded by the root `CMakeLists.txt` like the other two |
| `FLYSIGHT_FUSION_EXACT_TESTS` | `AUTO` | Only with the fusion tests: register the bit-exact runs `tst_fusion_*_exact` (label `exact`; no new executable). `AUTO` registers them when the compiler is 64-bit MSVC of the same major.minor as `cl_version` in `tests/data/fusion/capture.json` (19.44; the file is written by `fusion_golden_capture` at every capture), and otherwise prints "Fusion exact tests not registered: ..." at configure time; `ON` registers them whatever the compiler is; `OFF` never does. In every mode they exist for the Release configuration only. Forwarded by the root `CMakeLists.txt` like the others (section 11, "Tolerance policy") |

All four sub-options are forwarded by the root (superbuild) `CMakeLists.txt` to
the application project, unconditionally, so switching one back reaches the
inner cache too.

CTest labels: every executable has `core`; `tst_python_bridge` also `python`;
`tst_session_oracle` also `oracle`; `audit_cleanup` has `audit`. GTSAM-linked
tests also have `fusion`, so `ctest -LE fusion` is the GTSAM-free run. The
Widgets test also has `widgets` (`ctest -LE widgets` runs everything else).
The bit-exact runs have `core`, `fusion` and `exact` (`ctest -L exact`).

Environment: `FLYSIGHT_FUSION_EXACT=1` in the calling environment switches the
golden comparisons of the fusion tests from the portable tolerance to bit-exact
comparison (section 11). The `tst_fusion_*_exact` tests set it themselves; set
by hand, CTest passes it through to every fusion test, which is how to run
exact mode on a configuration where those tests are not registered. `FLYSIGHT_FUSION_RECORDING=<folder>` enables the optional real-recording
check of `tst_fusion_jobs` (section 11, "Real recordings"); without it that one
function skips. The oracle's variables are in section 7.

```bash
ctest --test-dir build/FlySightViewer-build -C Release --output-on-failure -L core
ctest --test-dir build/FlySightViewer-build -C Release --output-on-failure -LE python   # everything but the Python bridge
ctest --test-dir build/FlySightViewer-build -C Release --output-on-failure -LE fusion   # everything that does not link GTSAM
ctest --test-dir build/FlySightViewer-build -C Release --output-on-failure -L oracle
ctest --test-dir build/FlySightViewer-build -C Release --output-on-failure -L audit
ctest --test-dir build/FlySightViewer-build -C Release --output-on-failure -L exact    # bit-exact golden regression, where registered
```

## 4. Running one test / one function

```bash
ctest --test-dir build/FlySightViewer-build -C Release --output-on-failure -R tst_smoke
ctest --test-dir build/FlySightViewer-build -C Release -N        # list tests without running them
```

CTest puts the Qt `bin` directory and `third-party/GeographicLib-install/bin`
on `PATH` for each test (CMake 3.22 or newer), so nothing has to be deployed
next to the executables. To run an executable directly, for example to select
one test function, add those two directories to `PATH` yourself:

```bash
PATH="/c/Qt/6.9.3/msvc2022_64/bin:$PWD/third-party/GeographicLib-install/bin:$PATH" QT_FORCE_STDERR_LOGGING=1 build/FlySightViewer-build/Release/tst_smoke.exe importFs2Sensor -v2
```

(Git Bash syntax; in PowerShell prepend the same two directories to
`$env:PATH` and set `$env:QT_FORCE_STDERR_LOGGING = "1"`.) The executables are
in the application build tree's per-configuration directory
(`build/FlySightViewer-build/Release/`). `QT_FORCE_STDERR_LOGGING=1` is needed
on Windows whenever the output is piped or captured: without a console, Qt Test
otherwise sends its log to the debugger and prints nothing. CTest sets it for
every test. All standard Qt Test command-line options work, e.g. `-functions`
to list the test functions.

## 5. The embedded-Python bridge test (`tst_python_bridge`)

`tst_python_bridge` is the one exception to "no Python": it boots the real
embedded interpreter, imports the real `flysight_cpp_bridge` module from the
build tree, and runs the SDK (`python_plugins/flysight_plugin_sdk.py`), the
test plugins in `tests/python_plugins/*.py`, and the bundled example
(`python_plugins/examples/imu_tilt.py`) against real `SessionData` objects. It
covers acceptance 17 and the plugin half of acceptance 19: effective reads in existing
single-output plugins, effective values and units matching C++, no source
access from Python, the multi-output form
running once, exceptions and malformed output giving a clean unavailable
result, explicit key decoding, and registration-order precedence.
`pluginhost.cpp` and `pluginadapters.cpp` are not part of `flysight_core`
(that would force Python onto every test), so this target compiles them
directly and links `pybind11::embed`. No other test links Python.

**How it finds Python.** A test executable has no `python/` folder next to it,
so `PluginHost` takes its "system Python" branch, which honours the standard
environment variables. CTest sets all of them (see the block at the end of
`tests/CMakeLists.txt`); nothing comes from your `PATH` or profile, and
`PluginHost` has no test-only code path:

| Variable | Value | Why |
|---|---|---|
| `PYTHONHOME` | `sys.base_prefix` of the interpreter CMake found (`Python_EXECUTABLE`) | the standard library |
| `PYTHONPATH` | the directory of the built `flysight_cpp_bridge` (`build/FlySightViewer-build/Release`), then the site directory that holds NumPy | the bridge is imported before the plugin folder is on `sys.path`; NumPy may live in a venv or user site |
| `PATH` (Windows) | `+=` the directory of `python3XX.dll`, plus Qt and GeographicLib as for every test | DLL lookup |
| `PYTHONDONTWRITEBYTECODE` | `1` | nothing is written outside the test's temporary directory |

The plugin files are copied into a temporary directory first, because
`PluginHost` imports every `*.py` in the plugin folder.

**NumPy is required** in the build-time interpreter (the SDK imports it). If
`python -c "import numpy"` fails at configure time (or CMake is older than
3.22), CMake prints a warning and the test is registered as **disabled**:
`ctest` reports it as "Not Run (Disabled)" rather than omitting it, and the
target is still built so `pluginhost.cpp` stays compile-checked. Fix with
`python -m pip install numpy` and re-run CMake.
`-DFLYSIGHT_BUILD_PYTHON_TESTS=OFF` (default `ON`; forwarded by the root
`CMakeLists.txt`) removes the target entirely. Like every test on Windows it is
Release only.

**Running it outside CTest** means setting the three variables by hand (Git
Bash; adjust the Python and Qt paths):

```bash
PY="$(python -c 'import sys; print(sys.base_prefix)')"
NP="$(python -c 'import numpy, os; print(os.path.dirname(os.path.dirname(numpy.__file__)))')"
PYTHONHOME="$PY" \
PYTHONPATH="$(cygpath -w "$PWD/build/FlySightViewer-build/Release");$NP" \
PATH="$(cygpath -u "$PY"):/c/Qt/6.9.3/msvc2022_64/bin:$PWD/third-party/GeographicLib-install/bin:$PATH" \
PYTHONDONTWRITEBYTECODE=1 QT_FORCE_STDERR_LOGGING=1 \
build/FlySightViewer-build/Release/tst_python_bridge.exe -v2
```

**Adding a case.** There is one interpreter per process and
`PluginHost::initialise()` runs once, so every plugin file is loaded in
`initTestCase()`: add a file under `tests/python_plugins/` and a test function
in `tst_python_bridge.cpp`, never a second `initialise()`. Plugin calculations,
plots and markers stay registered for the life of the process: use unique
`_PY_*` / `py*` names and never assert that a registry is empty. Files are
imported in name order and the calculation ids contain the registration index,
so a new file that sorts before an existing one shifts the literal list in
`registrationOrderIsDeterministic` (name it to sort last, like `t_zdocs.py` and `t_zexplicit.py`).
Log assertions install their message handler inside the test function (after
`initTestCase`) and `cleanup()` removes it.

**Include order.** Include the pybind11 headers before any Qt header, wrapped
in `#pragma push_macro("slots")` / `#undef slots` / `#pragma pop_macro("slots")`
(Qt's `slots` macro breaks the Python headers), and mask `_DEBUG` around
`<Python.h>` on MSVC; copy the top of `tst_python_bridge.cpp`.

**Platforms.** Windows is as described above. On macOS the target gets a
`BUILD_RPATH` to the Python library directory so that the executable finds
`libpython` (unverified until CI runs it). On Linux the *build* interpreter
needs the development package (`python3-dev`) and NumPy.

## 6. Isolation guarantees

Tests never touch your preferences or logbook. Every test executable creates
one `FlySightTest::TestEnvironment` in `main()` before any application
singleton exists. It:

- sets the organization to `FlySightTests` and the application name to the test
  class name, never the real `FlySight` / `FlySightViewer`;
- enables `QStandardPaths` test mode;
- creates one temporary directory per process; everything the test writes goes
  under it, and it is deleted when the process exits;
- makes INI the default `QSettings` format and redirects both user and system
  scope into that directory, so every default-constructed `QSettings`
  (`PreferencesManager`, `LogbookColumnStore`, `AltitudeMarkerManager`) reads
  and writes a throw-away INI file instead of the Windows registry;
- registers the preferences the core library reads with literal defaults, and
  points `general/logbookFolder` at a folder under the temporary directory
  (the application default, the Documents folder, is never used);
- **aborts the process with `qFatal`** if a probe `QSettings` is not an INI
  file under the temporary directory, or if the logbook directory is not under
  it. A misconfigured harness therefore fails loudly instead of writing to
  real user data.

`TestEnvironment::useFreshLogbook()` switches to a new empty logbook folder and
calls `LogbookManager::reset()`; `reopenLogbook()` calls `reset()` alone, which
simulates an application restart on the same folder.

`tst_python_bridge` additionally runs with `PYTHONDONTWRITEBYTECODE=1`, so the
interpreter writes no `__pycache__` next to the SDK in the source tree.

To convince yourself: note the modification time of
`Documents/FlySight Viewer/logbook/index.json` and export the registry key
`HKEY_CURRENT_USER\Software\FlySight` (`reg export HKCU\Software\FlySight before.reg`)
before a test run, and compare afterwards. Both are unchanged.

## 7. The idempotency oracle

The invariant (idempotency): the value returned for any name is a pure function of
the session's persistent state and the registry. The order of reads, and what
happens to be cached, may change the work that is done, never the answer.
`CalculationEngine::verifyAgainstFresh(names)` reads each name the ordinary way
and compares it - availability, attribute value, samples bit by bit, and unit -
with `evaluateFresh(name)`: the same name evaluated on a throw-away engine with
empty caches over the same state and registry.

- `tst_calcengine_oracle` applies it to synthetic calculations on a fake
  session state: `randomizedSequences` (seeds 1-25) over the shared world plus
  the overlapping rings of `Synthetic::registerTangleWorld`, checking a few
  random names in both sessions after every change; `randomizedTopologies`
  (seeds 1-300) over generated worlds of six names whose candidates read each
  other at random, so that rings overlap in ways nobody designed.
- `tst_session_oracle` applies it to real sessions with the real built-ins.
  Part A (`sessionSequences`, seeds 1-20, 300 operations) interleaves two
  `SessionData` objects: reads, attribute edits (several of them user overrides
  of one output of a multi-output calculation), merges of a fixed pool of file
  fragments through `SessionMerge`, source and unit replacement, `SCHEMA_VER`
  changes, declared and snapshotted preferences, altitude registrations, and
  object moves / copies; it ends by restoring the state and comparing with the
  golden literals. Part B (`modelSequences`, seeds 1-6, 120 operations) drives
  a real `SessionModel` on a temporary logbook through
  `SessionImport::importFiles`, with restarts and simulated crashes, a
  subscriber that must be told (`dependencyChanged`) about every value it
  holds, and a final check that memory, the saved files, and the cached columns
  in `index.json` agree.

**Reproducing a failure.** The failure message starts with `seed=<s> step=<n>`
and names what differed; the operation log of that sequence (`#<step> <session>
<op> <arguments>`) is printed just before it. Run that seed alone:

```bash
# cmd
set FLYSIGHT_ORACLE_SEEDS=7
# PowerShell
$env:FLYSIGHT_ORACLE_SEEDS='7'
# Git Bash
export FLYSIGHT_ORACLE_SEEDS=7

ctest --test-dir build/FlySightViewer-build -C Release --output-on-failure -R tst_session_oracle
```

`FLYSIGHT_ORACLE_SEEDS` takes one seed or a range `a-b` and replaces the seed
list of both parts; `FLYSIGHT_ORACLE_OPS=<n>` changes the number of operations
(use it to shorten a failing sequence: the first `n` operations of a seed are
always the same). `FLYSIGHT_ORACLE_LOG=<file>` appends the operation log of
every sequence to a file, failing or not; two runs of one seed produce
identical logs. The soak run is `FLYSIGHT_ORACLE_SEEDS=1-500`; it takes about
twelve minutes, which is more than the test's CTest timeout of 300 s, so run the
executable directly (section 4, plus `QT_HASH_SEED=0`) or in ranges of 150
seeds.

CTest sets `QT_HASH_SEED=0` for this test so that `QSet` / `QHash` iteration
order, and with it the order of invalidation, is reproducible. Set it by hand
when running the executable directly.

Never "fix" a mismatch by removing a name from the catalogue
(`support/oraclecatalogue.cpp`): a mismatch is a missing dependency or a
missing invalidation in the code under test.

## 8. Writing a test

- One `QObject` test class per executable. Name the file and target
  `tst_<area>.cpp` / `tst_<area>` and register it in `tests/CMakeLists.txt`:

  ```cmake
  flysight_add_test(tst_<area> SOURCES tst_<area>.cpp)
  ```

  `flysight_add_test(<name> SOURCES ... [LIBS ...] [ENVIRONMENT VAR=value ...])`
  creates the executable, links `flysight_test_support` (and through it
  `flysight_core`, Qt Test and the platform's thread library), registers the CTest test with
  `QT_QPA_PLATFORM=offscreen` and `QT_FORCE_STDERR_LOGGING=1`, a 120 s timeout and the `core` label, and sets
  up DLL lookup on Windows.
- End the file with `FLYSIGHT_TEST_MAIN(YourTestClass)` (from `testmain.h`) and
  `#include "tst_<area>.moc"`. Never use `QTEST_MAIN` / `QTEST_GUILESS_MAIN`:
  they give no hook to isolate settings before the first singleton is touched.
  For the same reason, do not touch application singletons from global or
  static initializers in a test file. A test that needs Qt Widgets
  (`tst_plot_row_delegate` is the only one) writes its own `main()` instead:
  the same order - deterministic hash seed, application object,
  `TestEnvironment`, test object - with a `QApplication` and the application's
  Fusion style. The macro is not given a Widgets form: `testmain.h` is included
  by every test and must not come to need Qt Widgets.
- Get the environment with `FlySightTest::TestEnvironment::instance()`. Typical
  fixtures: `registerBuiltIns()` in `initTestCase()` (registers built-in
  attributes and calculations once per process; the UI-owned plots and markers
  are deliberately not registered), `useFreshLogbook()` and
  `resetPreferencesToDefaults()` in `init()`, `newTempDir()` for input and
  output files, and `waitForIdle(model)` to let a `SessionModel` finish its
  deferred saves and column fills.
- Shared helpers live in the support library; use them instead of a local
  copy. `testutil.h`: `isNear` (absolute 1e-9), `sameBits`,
  `sameBitsEverywhere` (two sample vectors), and `WarningCapture`, which collects warnings while it lives (`count()`,
  `messages()`, `matching(fragment)`, `count(fragment)`). `logbookprobe.h`:
  what is on disk in the test logbook (`readIndex`, `writeIndex`,
  `indexValue`, `sessionFilePath`, which is empty for an unknown session,
  `sessionCsvFiles`), the shared logbook columns (`descriptionColumn`,
  `gyroColumn`, `exitTimeColumn`), `writeAltitudes`, and the
  `dependencyChanged` spy checks (`spyHasAttribute`, `spyHasMeasurement`).
  `Synthetic::attr` / `Synthetic::measKey` (`fakesessionstate.h`) are the
  shorthand for public names. `asyncdriver.h` drives
  `PreparedCalculation::compute()` inline, on a joined `std::thread`, or on a
  `QThread` (`computeOn`, `ComputeRun`, `addComputeModeRows`) and provides
  `RecordingProgress`; it uses `std::thread`, which is why
  `flysight_test_support` carries `Threads::Threads` as a usage requirement
  (stated once there; no test names it).
  `jobfixture.h` (`JobWorld`, `Gate`, `waitIdle`, `Quiet`, `onFirstProgress`) registers controllable
  explicit calculations on the global registry for tests of the job queue and
  whatever sits on top of it: a test holds the queue's worker inside a compute
  function (`Gate::waitEntered`), then releases (`Gate::open`) or cancels it,
  without sleeps; construct the `JobWorld` before the `SessionModel` and
  destroy it after the `JobQueue` and the model. `Quiet` is "nothing started
  since" (no `jobQueued`, no new job row); `onFirstProgress` acts on the main
  thread while a job that no gate can hold (a real fit) is still running.
  `plotfixture.h` (`PlotFixture`) adds synthetic plots (`Syn/...`) over those
  calculations through on-demand bridge calculations, plus `show()` and
  `giveInput()` for the application's visibility and edit paths, `spin()`
  (two turns of the event loop and a flush of the `PlotRequests`) and
  `sessionIdsOf()` for a row's track lists; construct it
  after the `JobWorld` and destroy it before it. Helpers that need GTSAM or
  the fusion library live in `tests/fusion/` instead (section 11, "Fusion
  sessions"), which only the `fusion` tests link: the support library stays
  free of GTSAM, `flysight_fusion` and Qt Widgets.
- `QVERIFY` / `QCOMPARE` return from the function they are written in, so in
  a helper they would let the calling test carry on after a failure. A helper
  that can fail returns `bool` (`[[nodiscard]]`), or a `QString` that is empty
  on success and says what went wrong otherwise, and the test function checks
  it: `QVERIFY(setInput("s1", "G_IN", 4));`,
  `QCOMPARE(addSessions({...}), QString());`.
- `cleanup()` notes what it is going to check, tears everything down in order,
  and only then asserts. A failing check returns from `cleanup()`; whatever is
  still alive at that point is alive under the next `init()` (a second
  `JobWorld` registering the same ids on top of the first), and every later
  test fails for a reason that has nothing to do with it.
- A slot that captures locals of a test function by reference is connected with
  a function-local `QObject scope;` as its context, declared after what it
  captures - not with `this`, which outlives the function: a test that leaves
  early would otherwise be called back into dead stack variables by
  `cleanup()`'s `shutdown()`. Likewise a `PlotModel` given a stack `QSettings`
  is detached from it by a `qScopeGuard`.
- A wait on a worker thread is bounded (`tryAcquire` with a generous timeout
  under a `QVERIFY`, `Gate::waitEntered`), so that a defect fails the test
  instead of hanging it until CTest's timeout.
- `FLYSIGHT_TEST_MAIN` fixes the global `QHash` seed, so `QSet` / `QHash`
  iteration order is the same in every run, by hand or under CTest.
- If a test exercises core code that reads a preference not yet registered,
  add the key and its literal default to
  `TestEnvironment::registerCorePreferences()`.
- Generate input files in code with `Fs2FileBuilder` / `Fs1FileBuilder`
  (`fixturebuilder.h`). Values are passed as text and written verbatim, so the
  test controls the exact bytes; nothing is added implicitly. `Fixtures::sensorFile()`
  and `Fixtures::trackFile()` are small canned files. Always include a
  `DEVICE_ID` so the importer does not search parent directories for
  `FLYSIGHT.TXT`.
- Use a fresh `DataImporter` per import when asserting on `getLastError()`.
- Expected values are literals, worked out independently. Never obtain an
  expectation by calling the code under test.
- A test of the import path imports through `SessionImport::importFiles` (the
  model decides between creation and merge). `DataImporter::importFile` is the
  "parse + create" convenience for tests that just need a session;
  `SessionModel::mergeSessions(QList<SessionData>)` adopts in-memory sessions
  as they are (no import-time defaults). Put TRACK / SENSOR pairs in one
  device-style folder (`<tmp>/24-01-01/12-00-00/`) when the description default
  matters. `index.json` gets fresh column ids on every flush, so "the index
  bytes are unchanged" also proves that nothing flushed it.
- Tests that register on the global registry, or change preferences, restore
  both in `cleanup()`: snapshot `CalculationRegistry::instance().registeredIds()`
  in `init()` and compare. Destroy a `SessionModel` before unregistering test
  calculations (a live model schedules a calculation-environment check).
- When a test function demonstrates an acceptance clause, add it to
  `tests/acceptance_map.txt` and to the matrix in section 9.

## 9. Acceptance traceability

Two specifications, two ranges of items in `tests/acceptance_map.txt`, the
machine-checked form of the two tables below (section 10); keep them in sync.

### 9.1 Schema and calculation engine (items 1-19)

Every acceptance item (items 1-19, stated in full in
[appendix A](#appendix-a-the-acceptance-items-1-19)), clause by
clause, and the test functions that assert it with literal expectations.

| # | Clause | Test target :: function |
|---|---|---|
| 1 | effective 71.68 / -143.36 / 0 | `tst_source_layer::unmarkedFileIsCorrected`; `tst_conversion_engine::legacyGyroCorrected`; `tst_smoke::importFs2Sensor` |
| 1 | source values and recorded unit text | `tst_source_layer::unmarkedFileIsCorrected`, `sourceAccessNeverComputes` |
| 1 | `SCHEMA_VER` absent from the session | `tst_importer::neverStampsSchema`; `tst_source_layer::unmarkedFileIsCorrected` |
| 2 | schema-2 file unchanged | `tst_source_layer::schema2FileIsLiteral`; `tst_conversion_engine::schema2Unchanged` |
| 3 | `3` / `abc` (and empty, no value, `2.0`) rejected with an error | `tst_importer::rejectsUnsupportedSchema` (one data row each) |
| 3 | existing session with that `SESSION_ID` unmodified | `tst_importer::failedImportLeavesTargetUntouched`; `tst_import_merge::rejectedSchemaLeavesSession_loaded` / `_unloaded` |
| 4 | `g`, `gauss`, source retained, custom unit passes through | `tst_source_layer::unitNormalization`; `tst_conversion_engine::unitNormalization`, `internalLabelsAreIdentity`; `tst_schema_units::convertingUnits`, `identityUnits`, `unknownUnitsPassThroughVerbatim` |
| 5 | samples bit-identical | `tst_persistence_roundtrip::samplesAreBitIdentical`; `tst_csvformat::roundTripBits`, `roundTripSweep`, `negativeZero` |
| 5 | unit text and all header attributes preserved | `tst_persistence_roundtrip::unitsAndHeaderAttributesPreserved`, `typedAttributesRoundTrip` |
| 5 | `SCHEMA_VER` only if recorded (and never an unsupported one) | `tst_persistence_roundtrip::schemaVerOnlyIfRecorded`, `unsupportedSchemaIsNotSaved` |
| 5 | effective values identical before / after | `tst_persistence_roundtrip::effectiveValuesUnchanged`; `tst_session_oracle::modelSequences` |
| 5 | repeating the cycle changes nothing | `tst_persistence_roundtrip::secondCycleIsByteIdentical`, `logbookSaveReloadCycle` |
| 5 | warm and cold caches produce the same file | `tst_persistence_roundtrip::warmAndColdCachesSameFile`, `fileAndMemoryWritersAgree`; `tst_workflow::warmModelSaveEqualsColdExport` |
| 6 | released file loads, gyro corrected once | `tst_source_layer::releasedLogbookFormatLoads` |
| 6 | saving does not rescale or relabel | `tst_persistence_roundtrip::releasedLogbookSaveKeepsBytes`, `releasedLogbookBackfillIsAdditive`; `tst_workflow::releasedLogbookUpgrade` |
| 7 | either order, same result, loaded | `tst_import_merge::mergeOrderLoaded`, `mergeInOneBatch` |
| 7 | ... whether or not the session is loaded | `tst_import_merge::mergeOrderUnloaded`, `identityStubIsMatched` |
| 7 | conflicting header attribute fails, changes nothing | `tst_import_merge::conflictChangesNothing_loaded` / `_unloaded`; `tst_session_merge::differentAttributeConflicts`, `planIsPure` |
| 7 | session edits survive | `tst_import_merge::editsSurviveMerge` (loaded, unloaded) |
| 7 | unmatched measurements survive | `tst_import_merge::unmatchedMeasurementsSurvive` (loaded, stub); `tst_session_merge::columnsReplaceAddKeep` |
| 7 | `_` attributes of a Viewer-saved file: existing wins, absent are added | `tst_session_merge::viewerAttributesExistingWins`; `tst_import_merge::viewerSavedFileKeepsExistingEdits` |
| 8 | escape hatch: attribute and effective gyro update, no new session, edits kept | `tst_import_merge::escapeHatch`; `tst_source_layer::schemaAttributeFlipsGyro` |
| 9 | three outputs run once, any order | `tst_calcengine::threeOutputsRunOnce`; `tst_session_engine::multiOutputRunsOnce` |
| 9 | declared input change: exactly one run; unrelated: none | `tst_calcengine::declaredInputChangeRunsOnceMore`, `unrelatedChangeRunsNothing`; `tst_session_engine`, same names |
| 10 | absent-input candidate selected once the input appears, replacing a cached fallback | `tst_calcengine::preferredCandidateReplacesFallback`, `rejectedCandidatesAreDependencies`; `tst_session_engine::preferredSensorReplacesFallback`; `tst_import_merge::candidateSwitchAfterMerge` |
| 10 | randomized reads, edits, registry changes equal a fresh evaluation (engine level), also across overlapping dependency rings | `tst_calcengine_oracle::randomizedSequences`, `randomizedTopologies`; `tst_calcengine_safety::overlappingRingsIndependentOfReadOrder` |
| 10 | randomized reads, edits, **merges** on real sessions equal a fresh evaluation | `tst_session_oracle::sessionSequences`, `modelSequences` |
| 11 | override of one output coexists, no cycle | `tst_calcengine::overrideOneOutput`, `overrideFeedsDownstream`; `tst_session_engine::overrideOneOutput`, `overrideFlareStart`; `tst_session_oracle::sessionSequences` (`cycleCount() == 0` under random overrides) |
| 12 | nested, cycles, exceptions: no partial result, clean state | `tst_calcengine_safety` (all); `tst_python_bridge::exceptionYieldsCleanUnavailable`, `bundleExceptionPublishesNothing`; `tst_session_engine::safetyOnRealSession` |
| 13 | two sessions, one registration, independent results | `tst_calcengine::twoSessionsIndependent`; `tst_session_model_engine::twoSessionsOneRegistration` |
| 13 | unregister invalidates every session (loaded) | `tst_calcengine::unregisterInvalidatesEverySession`; `tst_session_model_engine::unregisterInvalidatesEverySession`, `reRegisterRestores` |
| 13 | ... including cached columns of unloaded sessions | `tst_column_cache::altitudeMarkerChangeDiscards` (added), `altitudeMarkerRemovalDiscardsStubValues` (removed) |
| 14 | explicit: unavailable until requested; the request publishes all outputs | `tst_calcengine::explicitPolicy`; `tst_session_engine::explicitPolicyOnSession` |
| 15 | declared preference invalidates dependents | `tst_calcengine::declaredPreferenceInvalidates`; `tst_builtins_engine::analysisRangeFollowsPreference`; `tst_session_engine::declaredPreferenceInvalidates`; `tst_session_model_engine::preferenceBroadcastReachesModel`; `tst_column_cache::preferenceChangeDiscardsUnloadedRows` |
| 15 | snapshotted preference does not affect existing sessions | `tst_calcengine::snapshottedPreferenceDoesNot`; `tst_session_engine::snapshotPreferencesDoNot`; `tst_column_cache::snapshotPreferenceDoesNotDiscard`; `tst_import_merge::defaultsNotReappliedOnMerge` |
| 16 | derived `wTotal` and interpolated gyro attribute are corrected and follow the source | `tst_source_layer::derivedWTotalUsesCorrectedGyro`, `interpolatedGyroAttribute`; `tst_session_engine::derivedWTotalFollowsSource`, `interpolatedAttributeFollows` |
| 16 | file-supplied `wTotal` wins | `tst_source_layer::fileSuppliedWTotalWins` |
| 17 | single-output plugins, real bridge, effective reads | `tst_python_bridge::bootsRealBridge`, `singleOutputPluginsReadEffectiveValues` |
| 17 | multi-output plugin runs once | `tst_python_bridge::multiOutputRunsOnce`, `bundledExampleRuns` |
| 17 | plugin reads are effective values and units, matching C++; plugins have no source access | `tst_python_bridge::effectiveReadAndUnitMatchCpp`, `derivedMeasurementReadsEffective`, `sourceKindIsUnknownToPlugins`; `tst_calcregistry::sourceInputsOnlyInSourceConversions` |
| 17 | Python exception: clean unavailable | `tst_python_bridge::exceptionYieldsCleanUnavailable`, `bundleExceptionPublishesNothing` |
| 18 | upgrade discards and recomputes cached gyro columns | `tst_logbook_index::missingMarkerDiscardsValues`; `tst_column_cache::upgradeDiscardsAndRecomputes`; `tst_workflow::releasedLogbookUpgrade` |
| 18 | a session edit refreshes only affected columns | `tst_column_cache::editRefreshesOnlyAffectedColumn_warm` / `_cold`, `markerEditRefreshesDependents`; `tst_import_merge::mergeEffects` |
| 18 | an interrupted save leaves no cached column that disagrees with the file | `tst_column_cache::interruptedSaveViaModel`, `indexFlushWhileDirtyOmitsUnsaved`; `tst_session_oracle::modelSequences` (crash operations) |
| 19 | the full application builds | the build itself (section 2); CI |
| 19 | import, rows, columns, marker / attribute edit, save, reopen | `tst_workflow::importEditSaveReopen` |
| 19 | plugin workflow end to end | `tst_python_bridge::pluginWorkflowThroughModel`; manual check |
| 19 | plot, marker, and other GUI workflows | manual checks - widgets are outside the test library boundary |
| 19 | no remaining use of the old engine or direct cache setters | `audit_cleanup` |
| - | (not a numbered item) only `SCHEMA_VER` decides (not firmware, file name, date) | `tst_source_layer::onlySchemaVerDecides`; `audit_cleanup` |

### 9.2 Sensor fusion and plot-driven jobs (items 101-120)

The twenty acceptance items of "Sensor fusion as an explicit calculation, with
plot-driven background jobs", stated in full in
[appendix B](#appendix-b-the-acceptance-items-of-sensor-fusion-and-plot-driven-jobs-101-120).
In the map, item = 100 + the number in the first column.

A line of the map has one of four forms, one per kind of evidence:

```
<item> <tst_target> <function>    an automated test function
<item> manual M<k>                step M<k> of section 12
<item> ci <token>                 <token> occurs in .github/workflows/build.yml
<item> audit <group>              rule group <group> of tests/audit/cleanup_audit.cmake
```

Every item 101-120 has at least one test or audit line; `manual` and `ci` lines
never stand alone.

| # | Clause | Evidence |
|---|---|---|
| 1 | solver libraries build, link, run; the right GTSAM build; 64 MiB main-thread stack | `tst_solver_smoke::gtsamIsTheRightBuild`, `smallGraphOptimizes`, `mainThreadHasSolverStack` |
| 1 | runtime libraries deployed (Windows, macOS, Linux); tests pass in CI | `ci solver_deploy_probe`, `ci libgtsam`, `ci gtsam.dll` (the workflow's deployment checks). Windows is verified locally; **macOS and Linux are confirmed only by a CI run** |
| 1 | only fusion code links GTSAM; other tests build without it | `audit solver-confinement`; `flysight_assert_solver_confinement()` at configure time (section 10); the suite with `FLYSIGHT_BUILD_FUSION_TESTS=OFF` |
| 2 | exact clock at high uptime: microsecond level | `tst_time_fit::highUptimeExactClock`, `fitFollowsTimeSource` |
| 2 | existing time-dependent tests unchanged | `tst_time_fit::fixtureFitIsExact`; the unedited `tst_builtins_golden`, `tst_builtins_engine`, `tst_session_engine` |
| 3 | origin gates | `tst_local_coordinates::originGates`, `speedAccuracyPlaysNoPart` |
| 3 | known displacement and velocity rotate correctly | `tst_local_coordinates::knownDisplacement`, `knownVelocityRotation` |
| 3 | NaN at the invalid index only; no fix: all unavailable; source changes invalidate; markers do not | `tst_local_coordinates::invalidSamplesAreNaNAtTheirIndexOnly`, `noQualifyingFixMakesEverythingUnavailable`, `sourceChangesInvalidate`, `markersAndDisplayDoNotAffectTheFrame` |
| 3 | seven outputs, same indices; 0.5 m | `tst_simplified_track::sevenOutputsShareIndices`, `droppedSamplesWithinTolerance` |
| 3 | duplicate endpoints, closed, degenerate, empty; non-finite skipped | `tst_simplified_track::duplicatePositionEndpoints`, `closedTrack`, `degenerateTracks`, `emptyTrack`, `nonFiniteSamplesAreSkipped` |
| 3 | projection once per recording, in the local-coordinate calculation | `tst_simplified_track::projectionRunsOncePerRecording`; `audit local-projection` |
| 3 | no origin: no track, no dot, bounds; recovery | `tst_simplified_track::unavailableWithoutOriginAndRecovers`; `tst_map_models::noOriginRemovesTrackAndDot`, `boundsClearedWhenNoTrack`, `recoversAfterSourceCorrection` |
| 4 | the kernel reproduces its goldens, captured from it by `fusion_golden_capture` | `tst_fusion_golden::successFixturesMatchGolden`, `rejectionFixturesMatchGolden`; `tst_fusion_kernel::fitTraceMatchesGolden`. Bit for bit on the capture compiler: the same functions in the CTest tests `tst_fusion_golden_exact` and `tst_fusion_kernel_exact` (the map names test functions, not CTest tests, so these have no line of their own) |
| 5 | reads (outputs, `accH`, diagnostics, interpolated value) never run anything | `tst_fusion_session::readsNeverRunTheFit`; `tst_fusion_jobs::readersNeverStartAFit` |
| 5 | ... nor does a Python plugin that reads an explicit output or a value derived from one (synthetic explicit calculation; no GTSAM) | `tst_python_bridge::pluginsNeverStartExplicitWork`; `audit gestures` (product code never calls the engine's synchronous `request()`) |
| 6 | runs once, publishes together, derived values appear, second request runs nothing | `tst_fusion_session::requestRunsOnceAndPublishesTogether`; `tst_fusion_jobs::jobPublishesAllOutputsTogether` |
| 6 | ... and the logbook cell over a fusion output shows the number straight after publication | `tst_fusion_jobs::columnShowsValueStraightAfterPublication` |
| 7 | asynchronous == synchronous | `tst_calcengine_async::asyncMatchesSync`; `tst_session_engine::asyncRequestOnSession`; `tst_fusion_session::asyncMatchesSync`; `tst_fusion_jobs::queueMatchesSynchronousRequest` |
| 8 | input change while running: superseded, nothing published, requestable | `tst_calcengine_async::inputChangeWhileRunningRefuses`; `tst_session_engine::asyncRequestOnSession`; `tst_jobqueue::inputChangeWhileRunningSupersedes`; `tst_fusion_jobs::inputChangeDuringFitSupersedes`; (stopped early) `tst_calcengine_async::willBeRefusedReportsTheEnginesMarks`, `tst_jobqueue::staleRunningJobIsStoppedAtOnce`, `requestWhileStaleJobWindsDown`, `tst_plot_requests::staleRunningJobShowsRefreshAtOnce` |
| 8 | change after publication drops the result and dependents | `tst_calcengine_async::changeAfterPublicationDropsDependents`; `tst_fusion_session::changeAfterPublicationDropsEverything` |
| 9 | rejection: unavailable, diagnostics reason, succeeded job with the reason, no second run, fresh run after an input change | `tst_fusion_session::rejectionIsACachedResult`; `tst_fusion_jobs::rejectedRecordingIsSucceededJob`; (synthetic) `tst_jobqueue::rejectionSucceedsWithReason`; (the row) `tst_fusion_rows::rejectedTrackShowsBadge` |
| 10 | cancel stops at the next boundary, publishes nothing, requestable; the next job starts | `tst_fusion_golden::cancelAtEachKindOfBoundary`, `cancelDuringPreparation`; `tst_fusion_session::cancelStopsAtNextBoundary`; `tst_jobqueue::cancelRunningThenNextStarts`; `tst_fusion_jobs::cancelDuringFitThenNextJobStarts` |
| 11 | no-IMU session: never missing / pending / failed; no job possible | `tst_jobqueue::refusesMissingInput`; `tst_plot_requests::sessionWithoutInputIsNeverListed`; `tst_fusion_session::missingInputsAreNotApplicable`; `tst_fusion_jobs::noImuSessionCannotHaveAJob`; `tst_fusion_rows::noImuSessionIsNeverCounted` (all seventeen real rows) |
| 12 | blockers report fusion for `accH`, nothing after publication, never a fit | `tst_calcengine_blockers::derivedNameReportsExplicitBlocker`, `inspectionNeverRunsExplicit`; `tst_fusion_session::blockersReportFusion` |
| 13 | B consumes A: one gesture runs A then B | `tst_calcengine_blockers::chainedBlockers`; `tst_plot_requests::chainedBlockersContinue` |
| 14 | one job at a time; no duplicates | `tst_jobqueue::oneAtATimeInRequestOrder`, `duplicateRequestsCreateNoDuplicates` |
| 15 | row script, without widgets, synthetic | `tst_plot_requests::rowScript` |
| 15 | row script with the real fusion plots; roll / pitch / yaw share one job; `accH` blocked by fusion | `tst_fusion_rows::realRowScript`, `rollPitchYawShareOneJob`, `accHRowIsBlockedByFusion`, `rejectedTrackShowsBadge`, `allSeventeenFusionPlotsAreExplicitBacked` |
| 15 | what the user sees | `manual M3`, `M4`, `M6`, `M7` |
| 16 | start-up restore / profile / programmatic check start nothing (logic) | `tst_plot_requests::startupRestoreStartsNothing`, `profileStyleApplyStartsNothing`, `programmaticCheckStartsNothing` |
| 16 | ... with the view attached; only a direct check is a gesture | `tst_plot_row_delegate::programmaticCheckStartsNothingWithViewAttached`, `startupStyleRestoreStartsNothingWithViewAttached`, `checkBoxClickIsGesture` |
| 16 | ... and the plot widget does not warn "No data available" about a checked plot that is merely uncomputed (the predicate; the widget's one call of it is M1) | `tst_plot_requests::merelyUncomputedIsNotWorthAWarning` |
| 16 | ... in the real `MainWindow` (cannot be constructed in the harness) | `audit gestures`; `manual M1`, `M2` |
| 17 | remove / unload with a queued or running job; shutdown | `tst_jobqueue::removeSessionWithRunningJob`, `removeSessionWithQueuedJob`, `evictionDeferredWhileJobActive`, `shutdownWithQueuedAndRunning`; `tst_fusion_jobs::shutdownDuringFit` |
| 17 | quitting the application | `manual M8`, `M9` |
| 18 | every transition through model signals; history from the model alone | `tst_jobmodel::historyFromSignalsAlone`, `retentionBound` |
| 19 | sessions editable, tracks shown / hidden, other values readable while a real fit runs | `tst_fusion_rows::editsAndVisibilityDuringFit`; `tst_jobqueue::idleSchedulerKeepsWorking` |
| 19 | plots pan and zoom during a fit | `manual M5` - widget-only, no automated test is possible in the harness |
| 20 | none of the branch's mechanisms exists | `audit branch-mechanisms`, `audit naming`, `audit one-worker` |

## 10. Cleanup audit

`audit_cleanup` runs `tests/audit/cleanup_audit.cmake`, a CMake script over
`git grep` that needs nothing but git. Every rule is a permanent invariant of
the working tree: nothing is compared with an earlier revision, so the result
does not depend on which tags or history a checkout has, and the whole audit
takes about a second. It fails, listing **all** violations, when

- a name of the old per-value engine, the old registration API, or a direct
  cache setter appears anywhere in `src`, `tests`, `python_plugins`, `cmake`;
- the importer converts units, anything but the conversion layer knows the gyro
  factor, a second place spells `"SCHEMA_VER"`, or anything but
  `csvformat.cpp` formats numbers for files;
- the conversion layer, the engine, the importer, the merge, or a calculation
  looks at the firmware version, a file name, or a date; a compute function
  reaches for preferences, settings, the clock, or random numbers;
- names of the superseded import-time correction (`master` commit `4668f48`)
  or alternate column names (`wx_source`, `source:wx`) appear;
- there is more than one `emit dependencyChanged`, a second caller of
  `exportSession`, `mergeSessions`, or `importFile` in `src`;
- anything outside `src/conversion`, `src/engine` and the listed engine tests
  declares a source input, or the name of the removed per-descriptor
  permission to do so reappears in `src`, `tests`, `python_plugins`: only the
  conversion layer reads the source layer;
- anything but `localcoordinatecalculations.cpp` constructs a local
  projection, or the simplified track includes a projection or geometry
  library;
- **group `local-projection`** (the two rules just above, cited by item 103);
- **group `branch-mechanisms`** (item 120): a modal progress dialog other than
  the import dialog, a nested event loop, a thread-local or "a calculation is
  running" flag, anything about calculations or jobs in the idle scheduler (or
  the idle scheduler in the job code), the plot widget's rebuild guard, the
  sibling-frame scaffolding, a dialog, message box or status-bar message for a
  calculation outcome, or a hand-cached failure in the fusion registration
  appears. `m_pendingRebuildLevel` in the plot widget is `master`'s own and is
  not part of the rule;
- **group `naming`**: something is named after a filter (the case-sensitive
  pattern `EKF|[Ee]kf`), a branch output name (`posN` ...) or the branch's sensor
  key reappears, or `MainWindow` no longer registers exactly seventeen "Sensor
  fusion" and six "GNSS (Local frame)" plots. This file is excluded: this
  section spells the patterns;
- **group `solver-confinement`**: a GTSAM header is included outside
  `src/fusion/` and the five GTSAM test and tool sources, the public or registration
  files of the fusion library include GTSAM or Eigen, the kernel includes a
  session, engine, queue, preference or GUI header (the registration adapter
  excepted) or logs, `flysight_core`'s calculation, engine, session-model, queue
  or plot-request code references the fusion library, or Boost reappears in
  the sources or the build;
- **group `one-worker`**: a thread is created anywhere but in `src/jobqueue.*`,
  a lock of any kind appears in `src`, or an atomic other than the queue's
  cancel flag does;
- **group `gestures`** (item 116): `plotCheckedByUser`, `refreshPressed` or
  `cancelPressed` is named outside `PlotRequests` and the row delegate,
  `prepare(` / `publish(` is called outside the queue and the engine,
  `request(` outside `PlotRequests`, the queue and the engine, anything in
  `src` outside `src/engine` calls the engine's synchronous `request()`
  (spelled through `calculationEngine()`, `engine.` or `engine->`; and, against
  an alias, the one line in `src` outside `src/engine` that calls any
  `request(` through an object is the job request in `plotrequests.cpp`:
  explicit work runs only as a job), the UI refers to
  the job queue beyond `AppContext.h`, or `EvaluationPolicy::Explicit` is
  tested outside the engine (`CalculationRegistry::dependsOnExplicit()` is the
  one authority for "explicit-backed");
- **group `widget-free-core`**: the queue, the job model, the plot request
  logic, the plot model or `PlotRowLayout.h` includes a widget header;
- a line of `tests/acceptance_map.txt` is malformed, names a test function, a
  manual step (`**M<k> ` in this file), a CI token or an audit group that does
  not exist, or an item outside 1-19 and 101-120; an item 1-19 has no line; or
  an item 101-120 has no test or audit line.

Whether a target **links** GTSAM is not a text question (link items come from
variables and from other targets' link interfaces). That half of the
confinement is checked at configure time by
`flysight_assert_solver_confinement()` (`cmake/SolverDependencies.cmake`),
which walks the real link closure of every target and fails the configure,
naming the target and the path, when anything but `flysight_fusion`,
`FlySightViewer` and the fusion tests reaches `gtsam`. On success it prints
`GTSAM link confinement: OK (<n> targets reach gtsam)`.

Run it alone:

```bash
ctest --test-dir build/FlySightViewer-build -C Release --output-on-failure -L audit
cmake -DREPO=. -P tests/audit/cleanup_audit.cmake        # without a build tree
```

It is registered only when the source tree is a git checkout. To add a rule,
add one `expect_none` / `expect_only` / `expect_count` line to the script;
`tests/audit` itself and `python_plugins/README.md` (whose "What was removed"
section names removed APIs on purpose) are excluded from every search.

The rules are text searches, so a legitimate change can trip one (a comment
that quotes the gyro factor, a new caller of `exportSession`, an unrelated
`units()` accessor). Each rule that is likely to do so carries an `Allow:`
comment in the script saying what to edit: add a `:!path` exclusion to an
`expect_none` rule, add the file to the allowed-file regex of an `expect_only`
rule, and change an `expect_count` number only when the fact really has gained
a second authority.

## 11. Fusion golden regression

`tests/data/fusion/` holds what the product kernel (`src/fusion/`, library
`flysight_fusion`) produced for twelve synthetic fixtures when the goldens were
last captured by `fusion_golden_capture`, the capture tool built with the
fusion tests. `tst_fusion_golden` and `tst_fusion_kernel` compare the kernel
with them, and so, through the registered calculation and the job queue, do
`tst_fusion_session`, `tst_fusion_jobs` and `tst_fusion_rows`
(sensor-fusion-jobs acceptance 4). A golden test that goes red means that the
kernel no longer produces what it produced at the last capture: an unintended
numerical change, or an intended one whose phase has not re-captured yet.

**Goldens change only by re-capture with the tool, at the end of a phase that
deliberately changes numerical results, never by hand and never to make a test
pass.** A fixture is adjusted only when its specification changes, never to
make a test pass either.

History, in one sentence: the first goldens were captured from the branch
`sensor-fusion-clean-port` by an out-of-tree harness; on 2026-09-22 the
in-tree tool reproduced them bit for bit from the ported kernel, apart from the
wording of the progress texts, which is now the kernel's own; since that
capture the branch is the source of nothing.

### Fixtures

Generated in code by `tests/fusion/fusionfixtures.cpp`, which uses Qt Core and
the standard library only, includes nothing from `src/`, and is compiled once,
into `flysight_fusion_test_support`, for the tests and for
`fusion_golden_capture`, so the goldens and the tests see the same bits
(`capture.json` records the generator's hash). To be
bit-identical with any IEEE-754 compiler the generators use only `+ - * /`,
compute every sample from its index, and draw noise from SplitMix64 mapped to
[-1, 1) in one fixed order (all GNSS fixes in time order, each drawing north,
east, down, velN, velE, velD; then all IMU samples, each drawing ax, ay, az, wx,
wy, wz). All times are UTC seconds, `1700000000.0 + t`. The local origin
attributes are 45, -75, 100 (they appear in the diagnostics only).

| Fixture | Content | Exercises |
|---|---|---|
| `coarse_linear` | The exact constant-velocity case. IMU `t = i*.01`, `i = 0..200`, force `(0, 0, -9.80665)`, rate 0. GNSS `t = .037 + i*.2`, `i = 0..8`, position `(7, 8, 9) + t*(12, -4, 2)`, velocity `(12, -4, 2)`, `hAcc = vAcc = 1`, `sAcc = .1`. Origin index 0. No noise | a window shorter than one segment (one segment, the 60 s prefix covers it), an exactly unobservable yaw (the four prefix starts tie and the first wins), near-zero objective, boundary timing; 9 states, 160 output samples; zero signal change, so the per-step IMU noise term is identically zero and this golden's numbers are the density-only model (they did not change when the term was added) |
| `coarse_maneuver` | 6 s. IMU `i = 0..600` at `.01`. GNSS `t = -.163 + j*.2`, `j = 0..32` (one fix before and two after IMU coverage). Origin index 3; `hAcc = 12` for `j < 3`, else `1.5`; `vAcc = 2.5`, `sAcc = .3`. NED acceleration `(1.5 - .4t, .8t, -.6 + .1t^2)`, `v(0) = (20, -5, 3)`, `p(0) = 0`, velocity and position its exact polynomial integrals; attitude identity, so force `= a - (0, 0, 9.80665) + (.05, -.03, .08)` and rate `= (.2, -.15, .3)` deg/s. Uniform noise: force `.02`, rate `.05` deg/s, position `.3`, velocity `.1`; seed `0x8F050002` | trimming of fixes outside IMU coverage, a fit that starts after the first fix, the full fit started from the segment fit's solution, non-zero biases and residuals; 28 states, 540 output samples; the per-step term on a noisy 100 Hz stream, where it is small against the density |
| `stationary_spin` | 40 s. IMU `i = 0..1000` at `.04` (25 Hz keeps the golden small). GNSS `t = .1 + j*.2`, `j = 0..199`. Position and velocity zero. Force `(0, 0, -9.80665) + (.03, -.02, .05)`; rate `(.2, -.1, .15)` deg/s plus 90 deg/s on `wz` for `t >= 31`. `hAcc = 1`, `vAcc = 1.5`, `sAcc = .1`. Noise: force `.005`, rate `.02`, position `.2`, velocity `.03`; seed `0x8F050003` | one segment at rest whose prefix grows from 60 s to 120 s to cover it (yaw unobservable and arbitrary; the anchor is the first fix, every sAcc being equal), yaw through more than two turns (unwrap); 200 states, 995 output samples; the per-step term at 25 Hz, including the 90 deg/s step at 31 s (one step with a change of 1.57 rad/s) |

Rejections, each one mutation, with the reason the kernel gives:

| Fixture | Base and mutation | Reason |
|---|---|---|
| `reject_nonfinite` | `coarse_linear`, `ax[10] = NaN` | Nonfinite IMU/ax |
| `reject_time_order` | `coarse_linear`, `imuTime[4] = imuTime[3]` | Timestamps must be finite and strictly increasing |
| `reject_too_few_fixes` | `coarse_linear`, GNSS channels truncated to 2 | Sensor fusion needs GNSS, IMU and shared UTC time conversion |
| `reject_coverage` | `coarse_linear`, IMU channels truncated to the first 36 samples | Fewer than three GNSS fixes in IMU coverage |
| `reject_imu_gap` | `coarse_linear`, IMU samples 40..49 removed | IMU gap at 0.353000 s; fusion unavailable across missing data |
| `reject_gnss_gap` | `coarse_maneuver`, GNSS fixes 12..23 removed (2.6 s > max(2 s, 5 x .2 s)) | GNSS gap: fusion unavailable for a disconnected session |
| `reject_sigma` | `coarse_linear`, `sAcc[0] = 0` | GNSS measurement sigmas must be positive |
| `reject_length` | `coarse_linear`, last `velE` sample removed | Missing or mismatched Local/velE |
| `reject_origin` | `coarse_linear`, `originIndex = 9` | Local origin index outside GNSS samples |

### Files in `tests/data/fusion/`

- `<fixture>.json` (all twelve; `QJsonDocument::Indented`, so every double is
  in shortest round-trip form and therefore exact): `fixture`; `outcome`
  (`"succeeded"` or `"rejected"`); `diagnostics`, the kernel's diagnostics
  object (objective, biases, RMS values, every residual, the input audit and
  the initializer description are inside it; for a rejection it is
  `{"algorithm", "failure"}`); `progress`, the kernel's progress texts in
  order (`Starting fit`, `Integrating IMU factors`, `Pass N, iteration M`;
  empty for rejections, since nothing is reported before the fit starts); and
  for successes `trace` (`initializer` with `segment_length_s` and per
  segment its bounds, anchor, prefix window, yaw sigma, fit counts,
  `converged`, `fallback`, start quaternion and biases; `converged`;
  `history` as `[pass, iteration, cost before, cost after]` rows), `rows` and
  `channels_file`.
- `<fixture>.channels.txt` (the three successes): line 1
  `# flysight fusion golden channels v1`; line 2 `# columns: _time north east
  down velN velE velD accN accE accD roll pitch yaw qx qy qz qw` (roll, pitch
  and yaw are the **unwrapped** values); then one line per output sample:
  seventeen space-separated 16-digit upper-case hexadecimal IEEE-754 bit
  patterns. Exact by construction and locale-proof; the tool writes LF, the
  loader accepts LF and CRLF.
- `capture.json`: provenance, written by the tool. `description` and
  `captured_by`; `capture_date`; `repository_revision`, the `--revision` text
  the procedure passes (`git rev-parse HEAD`), or `null` when none was given
  (never committed that way); `machine` (`os`, `cpu_architecture`); `compiler`
  (`id`, `version`, and on MSVC `cl_version`, the same string as `version`
  and what the configure-time gate of the exact tests reads; `generator`,
  `platform`, `configuration`, `cmake`); `qt`; `solver` (`gtsam_version`,
  `gtsam_use_tbb`, `gtsam_enable_boost_serialization`,
  `gtsam_use_boost_features`, `gtsam_dir`, and `pinned_in`, the file that
  fixes the solver revision at the recorded repository revision);
  `determinism` (two runs in one process, byte-identical, and the method);
  `sha256_lf`, the SHA-256 of each of the fifteen fixture files as written
  (LF line endings; `sha256_note`); and `fixture_generator_sha256_lf`, the
  SHA-256 of `tests/fusion/fusionfixtures.cpp` and `.h` with CRLF normalized
  to LF (`fixture_generator_note`: the goldens are valid for those inputs
  only).

  Revision convention: the capture happens before the phase's commit exists,
  so `repository_revision` is the parent of the commit that contains the
  goldens, and that commit's own changes are what the kernel was at capture
  time. The commit that holds the goldens is
  `git log -1 -- tests/data/fusion/capture.json`.

About 650 KB in total.

### Tolerance policy

- **Exact mode** (`FLYSIGHT_FUSION_EXACT=1`): every sample has the golden's
  bit pattern, every JSON number is equal, and `diagnosticsJson` is byte-equal
  to the compact serialization of the golden diagnostics. This is the mode that
  proves a rebuild on the capture configuration is bit-identical to the
  capture, which is what catches an unintended numerical change. It is
  meaningful on the capture configuration: the capture machine's compiler and
  flags (`capture.json`), Release, and the same GTSAM binary the goldens were
  captured against. There, with `/fp:precise` on x64 (scalar SSE2 arithmetic,
  no contraction), restructuring code into functions cannot change a bit, so a
  difference is a change to be found, not tolerated. `fitTraceMatchesGolden`
  says where: the initializer, or the first optimizer iteration whose cost
  differs. Result at the first in-tree capture (2026-09-22, the unchanged
  kernel against the goldens of the port): 0 of 28 815 samples not
  bit-identical, all three traces and diagnostics identical.

  Exact mode runs automatically where it is meaningful: the CTest tests
  `tst_fusion_golden_exact`, `tst_fusion_kernel_exact`,
  `tst_fusion_session_exact`, `tst_fusion_jobs_exact` and
  `tst_fusion_rows_exact` (label `exact`) are the ordinary fusion executables
  run again with `FLYSIGHT_FUSION_EXACT=1`, so on the capture configuration an
  ordinary `ctest -C Release` fails on the first bit that differs. With
  `FLYSIGHT_FUSION_EXACT_TESTS=AUTO` (the default) `tests/CMakeLists.txt`
  registers them only when the compiler is 64-bit MSVC with the major.minor
  of `compiler.cl_version` in `capture.json` (19.44; the file is read at
  configure time, so a re-capture moves the gate with it). The gate is the
  compiler because code generation is what decides the last bit. It
  deliberately does not register the tests for another compiler and let them
  fail: a Visual Studio update that changes a bit is not a defect of the kernel,
  and a red test that nobody can fix except by touching the goldens invites
  exactly that. Instead the configure log says `Fusion exact tests not
  registered: compiler differs from capture.json (...)`, the portable
  comparison still runs, and the remedy is to look: configure with
  `-DFLYSIGHT_FUSION_EXACT_TESTS=ON`, and either the new compiler is still
  bit-identical (re-capture, which records it) or `fitTraceMatchesGolden` says
  where it stopped being so. The exact tests are also Release-only, in every
  mode, because the goldens promise nothing about a Debug build: with a
  multi-config generator (Visual Studio) they are registered with
  `CONFIGURATIONS Release`, so `ctest -C Release` runs them and
  `ctest -C Debug` does not list them; with a single-config generator (Ninja,
  Makefiles) they are registered only when `CMAKE_BUILD_TYPE` is `Release`,
  and then unrestricted, so that `ctest` finds them with or without `-C`
  (otherwise the configure log says `Fusion exact tests not registered: the
  goldens were captured from a Release build`). What the gate cannot see is
  the third condition of the capture configuration: a solver built by that
  same compiler, in Release, from the pinned revision; the superbuild and CI
  give it. If the exact tests ever fail on a machine whose solver was built
  differently, that is what to check first, and `OFF` is the switch for such a
  machine - never a change to the goldens.
- **Portable mode** (default; CI on every platform, other compilers): exact
  for the output length, `_time` (one IEEE addition of the epoch), counts
  and copied tuning values (`gnss_states`, `imu_outputs`, `iterations`,
  `passes`, the input audit, residual nodes, and the thresholds and window
  under `stopping`) and the initializer account's counts, fix times and
  lengths (`index`, `prefix_fits`, `start_s`, `end_s`, `anchor_s`,
  `anchor_sacc_m_s`, `prefix_start_s`, `prefix_end_s`, `prefix_length_s`,
  `segment_length_s`, `fallback_segments`), and every string, bool and null; for every other number
  `|got - golden| <= floor + 1e-7 * |golden|`, where the floor is `1e-7` in
  the solver's own units (m, m/s, m/s^2, rad, rad/s, quaternion components)
  and therefore `1e-7 * 180/pi = 5.73e-6` for a number expressed in degrees
  (`roll`, `pitch`, `yaw`, and JSON keys ending in `_deg`)
  (`kPortableAbsolute`, `kPortableAbsoluteDegrees`, `kPortableRelative`,
  `portableFloor()` and `withinPortableBound()` in
  `tests/fusion/fusiongolden.h`; the same bound serves the channels, the
  diagnostics and the fit trace). Other platforms differ legitimately in the
  last bits (a different `sin`/`cos` in the C library, fma contraction inside
  GTSAM, a GTSAM compiled by another compiler) and the solver amplifies that by
  its conditioning: the fit stops at a relative cost decrease of `1e-8`, so
  where it stops is known no better than that.

  The bound rests on what the first CI run measured (2026-09-21, goldens from
  MSVC 19.44; GCC 11.4 on Ubuntu 22.04 x86_64, AppleClang 17 on macOS 15 x86_64
  and on macOS 15 arm64; `successFixturesMatchGolden` logs these numbers on
  every run):

  | Fixture | GCC x86_64 | AppleClang x86_64 | AppleClang arm64 |
  |---|---|---|---|
  | `coarse_linear` (2720 samples) | 0 differ | 0 differ | 1271 differ; abs 1.9e-20, rel 8.57e-06 |
  | `coarse_maneuver` (4320 of 9180) | 3780 differ; abs 1.06e-08, rel 2.45e-07 | the same | the same |
  | `stationary_spin` (16 915) | 14 874 differ; abs 6.17e-11, rel 1.05e-08 | 14 921; abs 8.89e-10, rel 1.3e-06 | 14 960; abs 2.31e-09, rel 3.12e-06 |

  ("abs" and "rel" are the worst absolute and the worst relative difference of
  any sample of any channel; the large relative differences belong to samples
  near zero.) The `coarse_maneuver` numbers cover `_time` to `accN` only: the
  comparison then stopped at the first failing channel, so `accE`, `accD`, the
  angles and the quaternion of that fixture were not measured on those
  compilers (the comparison now always covers all seventeen channels, so the
  next run's log is complete). The iteration counts and every cost of the fit trace agreed on
  all three, within the bound as it then was. That bound, `1e-9 + 1e-7 *
  |golden|`, was too tight in its absolute floor only: `coarse_maneuver` failed
  at `accN[332] = -0.011633` with a difference of 2.17e-09 where it allowed
  2.16e-09. The floor is now `1e-7`, about ten times the largest difference
  observed (1.06e-08). For the angles in degrees it is the same angle,
  `1e-7` rad, in their unit: the solver works in radians, the output is that
  times 57.3, and so is its rounding difference (an estimate for the unmeasured
  `coarse_maneuver` yaw, from the 2.5e-09 m/s^2 seen in `accN` and horizontal
  accelerations of a few m/s^2, is of the order of 1e-9 rad, 6e-8 degrees:
  too near `1e-7` degrees for that to be the floor). The relative term is unchanged, because nothing observed
  needed more (it serves the large channels: positions in metres, unwrapped
  angles in degrees, UTC-sized numbers in the diagnostics).

  What the floor means per channel: 0.1 micrometre of position; `1e-7` m/s of
  velocity; `1e-7` m/s^2 of acceleration, a hundred-millionth of g; 5.7e-6
  degrees of roll, pitch or yaw; and for a unit quaternion component (bound
  `2e-7` at magnitude one) a rotation of about 2e-5 degrees. All of that is
  below the sensitivity the branch's `docs/PORT_VALIDATION.md` records for
  sub-microsecond timestamp changes (5e-5 degrees of orientation, 7e-6 m/s^2 of
  acceleration), and far below anything physical or any transcription error: a
  wrong constant, index or term moves outputs by many orders more.
  `comparatorHoldsItsBounds` (`tst_fusion_golden`) holds the comparator to
  this: values just inside the bound pass and just outside fail, at zero and at
  small and large goldens; one part in 100 000 of a sample fails; `_time` and
  the counts fail on one unit in the last place; and exact mode rejects one
  unit in the last place everywhere. The absolute floor also covers the
  near-zero objective and residuals of `coarse_linear`. If a platform
  legitimately exceeds the bound, the remedy is a wider portable bound, with
  the observed numbers recorded here; never a change to the goldens, and never
  a compiler flag that changes the product's arithmetic.

  Values a test **recomputes** from floating-point products are a separate
  matter. `requestRunsOnceAndPublishesTogether` recomputes `accH` as
  `sqrt(accN^2 + accE^2)`; `accH` is not a golden channel. A compiler that
  contracts `a*a + b*b` into a fused multiply-add (clang on arm64 by default)
  may do so in the library and differently in the test, so the last bit can
  differ: `sameRecomputedValue()` demands the same bits in exact mode and
  4 ulp in portable mode. Comparisons of two runs of the same binary
  (determinism, worker thread against main thread, queue against synchronous
  request) and of copied values (the fit's inputs, `_time` against `IMU/_time`)
  stay bit-exact in both modes.

```powershell
ctest --test-dir build/FlySightViewer-build -C Release -L exact --output-on-failure
# where the exact tests are not registered (another compiler), by hand:
$env:FLYSIGHT_FUSION_EXACT = "1"; ctest --test-dir build/FlySightViewer-build -C Release -R "tst_fusion_(golden|kernel|session|jobs|rows)$" --output-on-failure
```

### The capture tool

`fusion_golden_capture` (`tests/fusion_golden_capture.cpp`) is a plain
executable in the `FLYSIGHT_BUILD_FUSION_TESTS` block of
`tests/CMakeLists.txt`: not a test, not installed. It links
`flysight_fusion_test_support` (the fixtures, the golden readers and writers,
and through it `flysight_fusion`) and `gtsam`, has the 64 MiB main-thread
stack of `flysight_solver_stack()` because the fits run on its main thread,
and is one of the targets named in `_FLYSIGHT_GTSAM_NAMERS`
(`cmake/SolverDependencies.cmake`) and in the audit's `solver-confinement`
regex, because it includes `fusion/fusionpipeline.h` for the trace seam and
`<gtsam/config.h>` for the provenance.

```
fusion_golden_capture [--revision <text>] [<output-directory>]
```

The output directory defaults to this checkout's `tests/data/fusion/`
(`FLYSIGHT_FUSION_GOLDEN_DIR`) and is created if missing; `--revision` is
recorded in `capture.json` as `repository_revision` (without it the tool
records `null` and warns on stderr). Anything else on the command line is a
usage error.

For each fixture, in `fusionFixtures()` order, the tool runs
`Detail::runPipeline()` with the production `Tuning`, a progress-collecting
`Checkpoint` and a `PipelineTrace`: exactly what `Fusion::run()` does, plus
the trace, so the golden is what the public API returns plus the trace. It
builds the golden object (`diagnostics` is the compact
`Result::diagnosticsJson` parsed and written indented, nothing edited; `trace`
is `traceJson()` of `tests/fusion/fusiontrace.h`, the same function
`tst_fusion_kernel::fitTraceMatchesGolden` reads the golden back with) and the
channels file (`fusionChannelsText()` of `tests/fusion/fusiongolden.cpp`,
next to the loader; `channelsWriterIsTheInverseOfTheLoader` holds it to the
committed files). Then it captures the fixture a **second time** and compares
the bytes of both files: only when the two captures are identical does it
write anything of that fixture (TBB is on; two runs must not differ). It
prints one line per fixture, `<fixture>: succeeded, <rows> rows, objective
<objective>` or `<fixture>: rejected (<reason>)` (or `solver_failed
(<reason>)`), followed by `  ** UNEXPECTED **` when the outcome is not the
fixture's `expectSuccess`; after the last fixture it writes `capture.json` and
prints `wrote <n> files to <directory>`. Files are written with LF line
endings and overwrite what is there.

Exit statuses: 0 ok; 1 usage or I/O error; 2 at least one fixture's outcome
was unexpected (the files are still written so that they can be looked at; a
capture that exits 2 is never committed); 3 the two captures of a fixture
differed (nothing further is written).

### Re-capture procedure

Every phase that changes numerical results ends with this. Git Bash, from the
repository root, on the capture machine (64-bit MSVC 19.44, Release, the test
build in `build-phase1/`):

```bash
# 1. Build the kernel, the tests and the tool (Release).
cmake --build build-phase1 --config Release

# 2. Capture into tests/data/fusion/ (the default output directory), recording HEAD.
#    PATH: Qt's bin, then the GTSAM and oneTBB install bin directories (gtsam.dll,
#    metis-gtsam.dll, cephes-gtsam.dll, tbb12.dll, tbbmalloc.dll). No other solver on PATH.
PATH="/c/Qt/6.9.3/msvc2022_64/bin:$PWD/build-solver-deps/GTSAM-install/bin:$PWD/build-solver-deps/oneTBB-install/bin:$PATH" \
  build-phase1/FlySightViewer-build/Release/fusion_golden_capture.exe --revision "$(git rev-parse HEAD)"
#    Check: twelve lines, one per fixture, in fixture order; the three success fixtures
#    "succeeded", the nine reject_* "rejected (<reason>)"; no "** UNEXPECTED **";
#    then "wrote 16 files to .../tests/data/fusion"; exit status 0.

# 3. Reconfigure the application build: the exact-test gate reads capture.json at
#    configure time (file(READ) is not a dependency, so this step is explicit).
cmake build-phase1/FlySightViewer-build
#    Check: the log says "Fusion exact tests registered for Release (... MSVC 19.44...)".

# 4. Run the fusion tests in both modes.
ctest --test-dir build-phase1/FlySightViewer-build -C Release -L fusion --output-on-failure
ctest --test-dir build-phase1/FlySightViewer-build -C Release -L exact --output-on-failure
#    Check: all green. -L fusion covers the portable comparison and (on this machine) the
#    exact runs; -L exact alone is the explicit bit-identity proof of the rebuilt kernel.

# 5. Report the changed golden files; they are part of the phase's files.
git status --porcelain -- tests/data/fusion/
git diff --stat -- tests/data/fusion/
```

PowerShell equivalent of step 2's `PATH`:
`$env:PATH = "C:\Qt\6.9.3\msvc2022_64\bin;$PWD\build-solver-deps\GTSAM-install\bin;$PWD\build-solver-deps\oneTBB-install\bin;$env:PATH"`
then
`build-phase1\FlySightViewer-build\Release\fusion_golden_capture.exe --revision (git rev-parse HEAD)`.

What changes, and what it means:

- `capture.json` changes on every capture (date, revision, hashes).
- A `<fixture>.json` / `.channels.txt` changes when the phase changed that
  fixture's numbers or texts. A phase that changes the `algorithm` string
  changes all twelve `.json` files; the nine `reject_*.json` otherwise change
  only when a rejection reason changes.
- Two captures of the same build are byte-identical; the tool verifies this
  in-process and refuses to write otherwise (exit 3).
- A `** UNEXPECTED **` line (exit 2) means a success fixture no longer
  converges or a rejection fixture is no longer rejected: a kernel regression
  or a fixture the phase invalidated. Do not commit that capture; fix the
  kernel, or (only when the phase's specification says a fixture's expectation
  changes) the fixture and its `expectSuccess`, then capture again.
- A red test after the capture means the kernel and the goldens disagree with
  the tests' literal expectations (a count, a text, a key set), which the phase
  resolves in the tests or the kernel, never by editing a golden.
- The phase's report lists every file `git status` shows under
  `tests/data/fusion/`.

### Fusion sessions

`tst_fusion_session`, `tst_fusion_jobs` and `tst_fusion_rows` test sensor fusion
as a registered calculation, on real sessions. `tests/fusion/fusionsessions.h`
(`flysight_fusion_session_support`) turns a fixture into a `SessionData` whose
twenty-one effective inputs are bit-identical to the fixture, so that
session-level results are held to the same goldens as the kernel. It relies on
"stored data always wins": the fit's inputs are stored as source data under
their own names - `Local/north` ... `velD`, `IMU/_time`, the
`_LOCAL_ORIGIN_*` attributes, an exact stored time fit (`_TIME_FIT_A = "1"`,
`_TIME_FIT_B = "1699999900"`) - with unit texts the conversion layer passes
through unchanged (`m`, `m/s`, `m/s^2`, `deg/s`, `s`); only `GNSS/_time` comes
from a calculation, the passthrough of the stored `GNSS/time`. Fixture sessions
carry `SCHEMA_VER = 2`: without it the conversion layer multiplies the gyro
channels by 1.14688 and nothing matches. `inputsAreBitIdenticalToFixture`
asserts the premise. Only fixtures with equal-length columns per sensor can go
into a `SessionModel` (the saver refuses a ragged sensor): not `reject_length`.
`naturalSession()` is the opposite: a recording as the importer leaves it
(GNSS, IMU, TIME; nothing under `Local`, no stored origin, no stored fit), for
the real input chain; its expectations are structural, not golden. The header
also holds what those tests share: `fixtureSession()` (a fixture session with
an exit marker inside the fit, `kFixtureExitTime`), `fusionKey()`,
`goldenDifference()` (a session's published channels against a golden) and
`addSessions()` (sessions into an empty `SessionModel` as the application adds
them, waited for until idle; empty text on success).
`registerFusionOnce()` registers the fusion calculations on the global registry
after `TestEnvironment::registerBuiltIns()`, as the application does;
`TestEnvironment` itself stays GTSAM-free. The real fit is never run on
`asyncdriver.h`'s thread modes (default stacks): `ComputeMode::Inline` or the
job queue's worker.

### Real recordings

Real recordings are not in the repository. The comparison against one,
documented in the branch's `docs/PORT_VALIDATION.md` (recording 17-26-24:
objective 65602.22485051976, 9247 GNSS states, 24411 outputs), is an optional
local check and not a CI test: `tst_fusion_jobs::realRecordingCheck`. It skips
unless the environment variable `FLYSIGHT_FUSION_RECORDING` names a folder that
contains `TRACK.CSV` and `SENSOR.CSV`; then it imports both through the
application's import path into the test's temporary logbook, requests the fit
through the job queue, waits up to 30 minutes, requires a Succeeded job without
a reason, aligned outputs and `Fusion/_time` as a contiguous run of
`IMU/_time`, and logs the objective, `gnss_states`, `imu_outputs`, the elapsed
time and the diagnostics. Run the executable directly (section 4), not under
CTest, whose timeout for fusion tests is 600 s:

```
$env:FLYSIGHT_FUSION_RECORDING = "D:\recordings\17-26-24"
.\tst_fusion_jobs.exe realRecordingCheck
```

Nothing in code asserts the reference's numbers. The counts should match as
they are. **The objective matches only if the gyro input is the branch's**:
the branch predates the legacy gyroscope correction, while here a recording
without `SCHEMA_VER` has its gyro channels multiplied by 1.14688
(`docs/DATA_SCHEMA.md` section 4). To reproduce the branch's objective, run the
check on a copy of the folder whose `SENSOR.CSV` has `$VAR,SCHEMA_VER,2` added
after the `$FLYS,1` line (the documented escape hatch). With the unmodified
legacy file a different objective is expected and correct.

## 12. Manual verification: plot-driven jobs

What no automated test can reach: the real `MainWindow` start-up and profile
paths (sensor-fusion-jobs acceptance 16), quitting with jobs queued and running
(17), and interactivity during a fit (19), plus what the rows look like (15).
Each step opens with its id and, in parentheses, the items of
`tests/acceptance_map.txt` it is evidence for; the map cites the ids
(`manual M<k>`) and `audit_cleanup` checks that they exist here.

**Always on a COPY of a logbook.** A development build rewrites `index.json`
as soon as it starts (its calculation environment differs from a released
build's), and the script deletes sessions. The logbook lives in
`<logbook folder>/FlySight Viewer/logbook`, where `<logbook folder>` is the
preference `general/logbookFolder` (default: your Documents folder; on Windows
the value `logbookFolder` under
`HKEY_CURRENT_USER\Software\FlySight\FlySightViewer\general`). Therefore,
**before the development build is started for the first time**:

1. With FlySight Viewer closed, note the modification time of the real
   `<logbook folder>/FlySight Viewer/logbook/index.json` and the current value
   of the preference.
2. Copy the whole `FlySight Viewer` folder into an empty scratch folder, so that
   `<scratch>/FlySight Viewer/logbook/index.json` exists.
3. Point the preference at `<scratch>` without starting the development build:
   in your installed release build (Preferences > General > Logbook folder,
   then quit), or by editing the registry value above.
4. Start the development build and check in Preferences > General that the
   logbook folder is `<scratch>` before doing anything else.

When you are done, quit, restore the preference the same way, and compare the
real `index.json`'s modification time with the one you noted: it must be
unchanged. Never change this preference in a running development build
and carry on: the index was read at start-up, so restart after every change.

**Recordings needed:** at least three with IMU data (TRACK and SENSOR files),
one without IMU data, and one the model rejects. If none is at hand, make one:
a copy of a SENSOR file with several seconds of `$IMU` rows removed from the
middle is rejected for its IMU gap. The script needs the fusion plots (the
"Sensor fusion" category of the plot list). Record pass / fail and a note per
step; for M5 note what you did while the fit ran, for M6 and M9 the wait you
observed.

**M1 Startup (116).** Check "Sensor fusion > Roll" with three fusable tracks visible, let it compute, quit, restart. After restart the row is still checked. Track visibility is not kept across restarts, so no track is visible and the row is plain; show the three tracks again: the row shows the refresh control with the count 3 and no job starts (no progress appears, CPU idle) until the control is pressed. The debug output contains no "No data available" line for the fusion plot.

**M2 Profile (116).** Uncheck Roll. With fusable tracks visible, apply a profile that checks fusion plots: the rows show refresh with counts; nothing starts. (The Plots menu and its shortcuts list a fixed set of GNSS plots; no fusion plot can be toggled from there.)

**M3 Refresh (115).** Press Roll's refresh control: the row shows "0 of 3" and the cancel control; Pitch and Yaw, if checked, show the same progress. The tooltip lists the computing track with the solver's progress text and the queued tracks. As each fit publishes, its graph appears without any further action, the label advances ("1 of 3", "2 of 3"), and finally the row is plain. The legend and any fusion logbook column fill in at the same moments.

**M4 Check gesture (115).** On a fresh set of tracks, uncheck and re-check a fusion plot by clicking its check box: jobs start. Do the same with Space.

**M5 Interactive during a fit (119).** While a fit runs: pan and zoom the plot, switch tools, hide and show other tracks, edit a session's description, set a marker, open Preferences. Nothing blocks; no dialog appears.

**M6 Cancel (115).** Press cancel on a row with one running and two queued jobs: at once the row shows refresh with 3, the plot stays checked, other fusion rows change identically; within one solver step the CPU goes idle. Press refresh again: it recomputes.

**M7 Fourth track and failure (115).** Show a fourth fusable track afterwards: the row shows refresh with 1 and nothing starts; refresh computes it. Show the recording without IMU data: it never appears in any count or tooltip. For a rejected recording the row shows the warning badge with 1, the tooltip gives the reason, no refresh is offered for it, and no message box appears.

**M8 Remove and unload (117).** With one fit running and one queued, delete the queued track's session, then the running one's: no crash, no hang, nothing published for them; the remaining rows' counts fall.

**M9 Quit (117).** With one fit running and two queued, close the window: a wait cursor for at most one solver step, then the application exits; no crash dialog, the process is gone from the task manager, and on restart the logbook is intact. Repeat with File > Exit.

Possible without the fusion plots, after any change to the plot list or to
`MainWindow`'s close path:

- Every existing plot row looks exactly as before (compare with a `master` build side by side, light and dark theme, 100% and 150% scaling): no glyph, no count, no tooltip, same row height, same elision.
- Checking and unchecking plots by mouse, by Space, through the Plots menu, by shortcut, and by applying a profile behaves as before.
- With a visible recording that lacks a sensor (for example no IMU) and an IMU plot checked, the "No data available for plot" warning still appears in the debug output.
- Quit through File > Exit and through the close button, in Debug and Release: no crash, no hang, no debugger output about destroyed objects or running threads.

There is no keyboard, menu, or context-menu surface for refresh and cancel (a
known limitation): a keyboard user unchecks and checks the row with Space.

## Appendix A. The acceptance items (1-19)

The numbered acceptance list that section 9.1, `tests/acceptance_map.txt`, and
the "acceptance N" comments on test functions refer to. All of it must be
demonstrated by automated tests that use generated fixtures in temporary
directories, never the user's logbook or preferences, with expected values
stated independently rather than computed by the code under test.

1. Importing an unmarked file with `wx=62.5, wy=-125, wz=0` yields effective
   `71.68, -143.36, 0` deg/s. Source access returns the recorded values and
   the recorded unit text. `SCHEMA_VER` is absent from the session.
2. The same values in a file declaring `SCHEMA_VER,2` are unchanged in the
   effective layer.
3. A file declaring `SCHEMA_VER,3` or `SCHEMA_VER,abc` is rejected with an
   error and an existing session with that `SESSION_ID` is unmodified.
4. Recorded `1 g` reads as `9.80665 m/s^2` effective; recorded `1 gauss` reads
   as `0.0001 T`; the source retains `1` and `g` / `gauss`. A custom column
   with unknown unit text passes through unchanged with its label.
5. Save then reload: every source sample is bit-identical, unit text and all
   header attributes are preserved, `SCHEMA_VER` is present only if it was
   recorded, and effective values are identical before and after. Repeating
   the cycle changes nothing. Saving with warm and cold caches produces the
   same file.
6. A released-format logbook file (normalized units, no `SCHEMA_VER`) loads,
   its gyro is corrected once, and saving it does not rescale or relabel it.
7. `TRACK.CSV` and `SENSOR.CSV` merge in either order, whether or not the
   session is loaded, with the same result. A merge whose header attribute
   conflicts with the session fails and changes nothing. Session edits and
   unmatched measurements survive a merge.
8. Adding `SCHEMA_VER,2` to an unmarked file and re-importing it updates the
   session's attribute and its effective gyro values, without a new session
   and without losing edits.
9. A three-output calculation runs once when its outputs are read in any
   order across repeated reads. A change to a declared input causes exactly
   one new run on the next read; an unrelated change causes none.
10. A candidate that could not run because a declared input was absent is
    selected on the next read once that input is added, replacing a cached
    fallback. For randomized sequences of reads, edits, and merges, every
    value returned equals the value from a fresh evaluation with caches
    cleared (idempotency: the value of a name is a pure function of the
    session's persistent state, the declared preferences, and the registry).
11. A user override of one output of a multi-output calculation coexists with
    the calculation's remaining outputs and causes no cycle.
12. Nested calculations, cycles, and thrown exceptions leave no partial
    results and no corrupted evaluation state.
13. Two sessions using one registration have independent results.
    Unregistering a calculation invalidates its results in every session.
14. An explicit-policy calculation reports unavailable until requested, and
    requesting it publishes all outputs at once.
15. Changing a declared preference input invalidates dependents; changing a
    preference that is only snapshotted at import does not affect existing
    sessions.
16. Derived `IMU/wTotal` and an interpolated gyro attribute reflect the
    corrected values and follow source changes; a file-supplied `wTotal`
    keeps precedence over the derived one.
17. Existing single-output Python plugins work through the real bridge with
    effective reads; a multi-output plugin runs once across its outputs; a
    Python exception yields a clean unavailable result. (The original clause
    "source access from Python matches C++" no longer applies: plugin source
    access was removed by decision; plugins read effective values only.)
18. Upgrading a logbook with cached gyro-dependent column values discards and
    recomputes them; a session edit refreshes only the affected columns.
19. The full application builds and ordinary import, plot, marker, logbook,
    and plugin workflows work end to end with no remaining use of the old
    per-value cache engine or direct cache setters.

One further rule is traced in section 9 although it is not one of the 19 items
(`tests/acceptance_map.txt` files it under item 19): only the recorded
`SCHEMA_VER` attribute selects a schema correction - never the firmware
version, a file name, a date, or the data.

## Appendix B. The acceptance items of sensor fusion and plot-driven jobs (101-120)

The acceptance list of the specification "Sensor fusion as an explicit
calculation, with plot-driven background jobs", verbatim. These are items
101-120 of `tests/acceptance_map.txt` (item = 100 + the number below) and the
rows of section 9.2; "sensor-fusion-jobs acceptance N" in comments on test
functions refers to them. "The branch" is `sensor-fusion-clean-port`, the
behavioural reference the fusion kernel was ported from.

1. The application builds, deploys its solver runtime libraries, and passes
   its tests on Windows, macOS, and Linux through the existing CI workflow.
2. With exact synthetic clock data at high device uptime, UTC conversion
   error is at the microsecond level; existing time-dependent tests pass
   unchanged.
3. Local coordinates: origin selection follows the stated gates; an
   analytically known displacement and velocity rotate correctly into the
   frame; invalid samples give NaN at their index only; no qualifying fix
   makes all outputs unavailable; source changes invalidate the outputs.
   Simplified track: all seven outputs select the same original sample
   indices; every dropped sample lies within 0.5 m of the simplified path;
   duplicate-position endpoints, closed, degenerate, and empty tracks behave
   sensibly; non-finite samples are skipped; the projection runs once per
   recording, in the local-coordinate calculation. With no qualifying origin
   the map shows no track or cursor dot for that recording and recovers when
   the source is corrected.
4. For each committed synthetic fixture, the ported fusion reproduces the
   golden outputs captured from `sensor-fusion-clean-port`.
5. Reading any fusion output, `accH`, the diagnostics attribute, or an
   interpolated logbook value of them, on a session where fusion has not been
   requested, returns unavailable and runs nothing, however many times and in
   whatever order.
6. Requesting fusion runs it once and publishes all outputs together;
   `accH` and the fusion system-time axis become available without being
   requested; a second request runs nothing.
7. Asynchronous and synchronous requests produce identical results.
8. Changing a declared input while a job is running ends the job as
   superseded, publishes nothing, and leaves the calculation requestable.
   Changing one after publication drops the result and every dependent.
9. A recording the model rejects yields unavailable measurements, a
   diagnostics attribute with the reason, a succeeded job carrying that
   reason, no second run on re-request, and a fresh run after its inputs
   change.
10. Cancelling a running job stops it at the next solver boundary, publishes
    nothing, and leaves the calculation requestable. The next queued job then
    starts.
11. A session with no IMU data never appears as missing, pending, or failed
    for any fusion plot, and no job can be created for it.
12. Blocker inspection reports fusion for `accH` on an unrequested session,
    nothing after publication, and never triggers a fit.
13. With two explicit test calculations where B consumes A, one gesture on a
    plot of B's output runs A then B.
14. Never more than one job runs at a time. Duplicate requests create no
    duplicate jobs.
15. Row behavior, tested without widgets: checking a fusion plot with three
    visible fusable tracks queues three jobs; the pending count falls as each
    publishes; unchecking mid-way removes the queued jobs and lets the
    running one finish; cancel leaves the plot checked and the tracks
    missing; showing a fourth track afterwards leaves it missing with a
    count of one and starts nothing; pressing refresh computes it.
16. Starting the application with fusion plots checked and tracks visible
    starts no job. Applying a profile that checks fusion plots starts no job.
17. Removing or unloading a session with a queued or running job, and
    quitting with jobs queued and running, neither crash nor hang, and
    publish nothing stale.
18. The job model reports every transition through model signals and retains
    finished jobs with state, timing, and reason; a test view can render the
    whole history from the model alone.
19. The application stays interactive during a fit: plots pan and zoom,
    tracks can be shown and hidden, and sessions can be edited.
20. None of the branch's modal dialog, re-entrancy guard, idle-scheduler
    pause, or plot-widget rebuild guard exists in the result.
