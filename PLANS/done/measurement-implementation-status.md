# Measurement implementation status

Baseline: actual dirty workspace, 2026-09-17. Existing experiments, third-party trees, uncommitted files and ignored PLANS/build artifacts preserved. No applicable AGENTS.md. The current user request explicitly authorizes implementation.

| Phase | Status | Accepted evidence |
| --- | --- | --- |
| 0 | Complete | Shared contracts settled before dependent work; fixtures checked |
| 1 | Complete | Persistent store, atomic mutations and merge tests |
| 2 | Complete | One evaluator, tracked reads and shared results; cycle defects fixed and independently reviewed |
| 3 | Complete | Source interpretation and native-name facade; source-only IO |
| 4 | Complete | Parser/typed codec precision, preservation and atomic failure checks |
| 5 | Complete | Pure built-in groups, interpolation and live preferences; Debug core/builtins 7/7 |
| 6 | Complete | Actual host/external Python bridge, ownership and provider lifetime; Debug CTest 1/1 |
| 7 | Complete | Mutation, merge, 100-session batches, persistence/recovery and cache context; Debug 20 checks |
| 8 | Complete | Actual correction commands, import default, failures and UI; Debug UI/IO 2/2 |
| 9 | Complete | Full two-process application lifecycle, normal harness-OFF Release build and test-flag rejection passed |

## Shared decisions

[measurement-contracts.md](measurement-contracts.md) defines the interfaces. Complete imported attributes belong to shared source records; series reference source/axis and recorded units. Overrides/suppression are separate. Recorded schema and correction Bool are separate. Native effective reads use one evaluator and per-calculation/session cache. Source-only `.fsv` uses typed JSON and exact finite-double round trips. Session QSaveFile commit precedes the disposable hash/context-checked index. Fusion and asynchronous jobs remain outside scope.

Cycle detection rejects participating calculation instances without fixed-point evaluation. Actual and negative reads determine invalidation. Runtime registry revisions invalidate loaded/stub columns. Across restarts, arbitrary Python providers conservatively disable persisted-column reuse; the saved policy prevents later plugin removal from rehabilitating old columns. Native altitude/descent context and the independent calculation token guard index values. Context is synchronized before fresh column insertion and before index commit.

## Final validation

- Debug core/builtins: 7/7 passed, 2.44s after final cleanup.
- Debug model: 20 passed, zero failures/skips, 21.503s (`build-measurement-model-tests/phase9-model-test.txt`). Four immediate-flush regressions were demonstrated failing before the fix.
- Debug actual Python host/external bridge: CTest 1/1 passed, 0.57s.
- Debug actual UI/IO: CTest 2/2 passed, 1.20s; offscreen screenshots inspected.
- Full Release application with opt-in harness: built successfully. `build/measurement-app-final-8` passed both initial and restart processes, runner exit 0, 3.853s. Actual commands, mixed-source import, corrected native reads, simplification, saved state, failed close/retry, failed delete/retry and exact restart snapshot checked. Both browser pages were destroyed before Python shutdown and both processes exited cleanly.
- Normal harness-OFF Release: configure/build passed (`build/measurement-release-configure.log`, `build/measurement-release-build.log`). Both test-only switches rejected with exit 2 before MainWindow; no test state created (`build/measurement-release-switch-check.txt`).
- Final `git diff --check`: passed.

The lifecycle uncovered detached dock ownership on shutdown. MainWindow now deletes feature wrappers and guarded docks while models/Python remain alive. Two PlotDockFeature callbacks now use their owning feature as receiver. Independent review accepted both fixes; browser destruction is asserted by the application harness.

## Isolation and limits

All mutated settings, profiles, recordings and logbooks are generated/temporary; user state and experiment results were not used. The app harness isolates both QSettings scopes, profiles, plugin directory and WebEngine storage, and blocks network requests. In this restricted Windows environment the renderer exited with code49 under its default sandbox; the passing run used child-only `QTWEBENGINE_DISABLE_SANDBOX=1`. Production and runner sandbox defaults remain unchanged. No normal-mode application launch was performed.

Validation covers Windows Qt6.9.3/MSVC2022/Python3.13. Full-app Debug startup has pre-existing preference-registration assertions (`general/units`, `map/type`); component tests ran Debug. Online maps, hardware, packaged deployment, other operating systems and actual process termination midway through a file commit were not exercised. Full details and exact commands are in phase handoffs and tests/README.md.

## Ownership and next action

Post-acceptance TRACK import regression: a real device header leaves the ISO time
unit blank, unlike the initial synthetic fixtures. The rejection was reproduced
before the fix. SourceSeries now persists an explicit utcTimestamp encoding flag
while retaining the recorded label; effective samples/axis coordinates use seconds.
Missing flags in existing FSVs retain the old numeric representation. Import dialogs
now include parser errors. Independent review found no propagation/cache defect.
Debug core 7/7 passed (12.86s), including import/save/reload of a temporary copy of
the user-specified TRACK.CSV. A focused check confirms all 9,267 GNSS samples and
exact saved-state round-trip. Debug actual Python bridge 1/1 passed (0.47s).
Updated normal Release build/install and packaged dependency checks passed; installed
EXE/bridge hashes match the build. The original recording checksum is unchanged.
See measurement-handoffs/track-import-regression.md and build/track-fix-*.log.

All phases accepted by the coordinator. No active or pending implementation, temporary adapters awaiting removal, requirement conflicts or external blockers. The application build tree is left with the harness OFF. Detailed phase handoffs record file ownership, interfaces and validation commands. Independent source/evaluator, persistence/model and final integration reviews are recorded under `measurement-handoffs/` with confirmed findings closed. No further assignment is required for this plan.
