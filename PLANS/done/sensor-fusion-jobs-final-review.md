# sensor-fusion-jobs: final review findings

Date: 2026-09-21. Branch `sensor-fusion-jobs` at `90259d7` (13 commits on
`master`). Three read-only final reviews (architecture, code quality,
requirements coverage) ran after all ten phases were accepted. None found a
blocking problem. This file is the complete list of what they did find, so
that items can be ticked off. "Phase" is the phase whose code the item
belongs to, which is also the number a fixup commit would carry.

Status column: `open`, `in progress`, `done <commit>`, `won't do`.

State on 2026-09-21 after the clean-up round: branch at `e33fc63` (21 commits
on `master`), 47/47 tests (42 plus 5 exact-parity twins), real-recording
reference objective unchanged. Still open: Q7 and Q8 and M-a (need a push),
M-b (Michael's manual pass M1 to M9).

Verified afterwards by the orchestrator on `90259d7`: full suite 42/42;
`FLYSIGHT_FUSION_EXACT=1 ctest -R tst_fusion` 5/5 with 0 of 28 815 samples
not bit-identical; `tst_plot_requests` 50 consecutive passes (the stale
`LastTestsFailed.log` entry was from mid-Phase-10 development).

## A. Architecture review

Verdict: Pass in all six areas (integration, dependencies, separation of
concerns, threading and lifetime, extensibility, no remnants of the old
mechanism). Nothing blocking.

| Id | Phase | Severity | Finding | Remedy | Status |
|---|---|---|---|---|---|
| S1 | 4, 5 | Should-fix | A running job whose inputs went stale computes to the end (minutes) before ending Superseded. Meanwhile `activeJob()` deduplicates against it, the row shows cancel instead of refresh, and the single worker is held. Session replacement by move-assign has the same effect. Correct per acceptance 8, but wasteful. | Main-thread query on the ticket ("will be refused"); the queue re-checks the running ticket on model/registry signals and requests a stop with pending end Superseded. Engine still decides staleness. No lock. | done `13d4b81` |
| S2 | 5 | Should-fix | `docs/CALCULATIONS.md` section 15 does not say the job runs to the end first; user docs do not say Cancel is the way out. | Moot once S1 lands: document the new behaviour instead. | done `13d4b81` |
| N1 | 7 | Nice-to-have | `closeEvent` blocks the GUI thread on `wait()` for up to one solver step with only a wait cursor. | Status-bar message before the wait. | won't do: the main window has no status bar; creating one in closeEvent would not paint before the wait and would disturb the saved dock layout (`532125b` message) |
| N2 | 6 | Nice-to-have | The "request blockers until none remain" chaining policy lives only in `PlotRequests::continueAfter`. A second requester would re-implement it. | Move into a small helper beside `JobQueue` if a second requester ever appears. | won't do: no second requester exists (the jobs-dock spec says the dock starts nothing) |
| N3 | 5 | Nice-to-have | No path for explicit calculations that must run on the main thread (future plugin calculations; GIL). | Descriptor flag plus a queue path that runs prepare/compute/publish from the event loop. Not needed now. | won't do: unused feature the specification put out of scope |
| N4 | 6 | Nice-to-have | "y name available implies x axis available" is documented (`src/plotrequests.h:133-135`) but not checked. | Registry-level debug assertion at registration. | done `532125b` |
| N5 | 5 | Nice-to-have | `JobQueue` iterates `JobModel::m_jobs` through friendship. | `records()` const accessor; narrow friendship to mutators. | done `532125b` |
| N6 | 5 | Nice-to-have | When `SessionData` is replaced by move-assign during a job the reason reads "Session removed or unloaded". | "Session data replaced". Cosmetic. | done `532125b` |

## B. Code quality review

Verdicts: Consistency pass (minor); Maintainability concerns; Documentation
pass (two inaccuracies); Error handling pass (one concern); Tests-as-code
pass (concerns on failure paths). Nothing blocking. Items marked (unchecked)
were reported by a sub-reviewer and not re-verified against the code.

### Should-fix

| Id | Phase | Finding | Remedy | Status |
|---|---|---|---|---|
| Q1 | 8 (docs 10) | The fit's preparation stage has no cancel boundary and does repeated whole-recording work: `planFit()` (`src/fusion/fusion.cpp:39-56`) never receives the checkpoint, so the `catch` at `:102` is dead; `bestStationaryWindow` (`src/fusion/initializer.cpp:36`) calls `assessStationaryWindow` per 5 s step and each call sorts the whole IMU time axis (`stationarywindow.cpp:150`, `samplestatistics.cpp:13`) plus two full scans. For a long recording cancel and shutdown block for seconds to tens of seconds (estimate, unmeasured), contradicting spec 8.3, `docs/SENSOR_FUSION.md:225-228,242` and the `closeEvent()` comment. | Hoist the median out of the loop (bit-identical: same `samples` each time). Add a cancel-only poll per candidate window; it must be silent, because `tst_fusion_parity::progressMatchesReferenceBoundaries` compares the progress text sequence. Correct the docs sentence either way. | done `137daf4` |
| Q2 | 1 | `.gitignore` lists the GeographicLib and KDDockWidgets install dirs (`:46-49`) but not `third-party/GTSAM-install/` and `third-party/oneTBB-install/`, the superbuild's default install dirs. | Add them (and the solver build/source dirs the superbuild creates, if any land under `third-party/`). | done `2fa7c0d` |
| Q3 | 5, 6, 7, 9 | Test `cleanup()` asserts before tearing down (`tests/tst_jobqueue.cpp:180-183`, `tests/tst_plot_row_delegate.cpp:323-328`; (unchecked) `tst_jobmodel.cpp:245`, `tst_plot_requests.cpp:250`, `tst_fusion_jobs.cpp:188`, `tst_fusion_rows.cpp`). After one failure the next `init()` builds a new `JobWorld` while the old is alive; duplicate registration ids can make every later test fail. | Tear down first behind a guard; assert on captured facts afterwards. | done `8d66503` |
| Q4 | 5, 9 | Signal lambdas capture stack locals by reference with `this` as context and are never disconnected (`tests/tst_fusion_jobs.cpp:452-455`; (unchecked) `:604-609`, `tst_jobqueue.cpp:295-299`, `onFirstProgress` callers in `tst_fusion_rows.cpp`). On an early-exit failure `cleanup()`'s `shutdown()` emits into dead stack variables. | Local `QObject` as context, or disconnect with a `qScopeGuard`. | done `8d66503` |
| Q5 | 6, 7 | (unchecked) Stack `QSettings` handed to `PlotModel::setSettings` without a guard (`tests/tst_plot_requests.cpp:858-886`, `tests/tst_plot_row_delegate.cpp:569-588`). | `qScopeGuard` that resets the model. | done `8d66503` |
| Q6 | 8 | Dead statement: `medianInterval(full.gnssTime);` at `src/fusion/fusion.cpp:45` discards its result and cannot throw (`inputadapter.cpp:111-112` already validated). Unreachable `catch (const FusionCancelled &)` at `fusion.cpp:102` (see Q1). | Delete; in the adapter call `requireIncreasingFiniteTimes()` directly. | done `137daf4` |
| Q7 | 1, 10 | "Unverified until the first CI run" notes in permanent files: `README.md:228`; `cmake/SolverDependencies.cmake:251,255,296,298,328`; `cmake/solver_stack_linux.cpp:19`; `tests/README.md:219`. | Push, fix what breaks, then remove all markers in one commit. | open (needs a push) |
| Q8 | 8, 10 | The golden reference exists only in the local clone: `docs/SENSOR_FUSION.md:252-253` and the capture procedure (`tests/README.md`, capture section) cite `sensor-fusion-clean-port` at `83a64479`; (unchecked) no remote has the branch. | Push a tag for `83a6447` and cite the tag. | open (needs a push) |
| Q9 | 10, 7 | Changelog language in user docs and a plan reference in source: `docs/SENSOR_FUSION.md:81-86` ("once lost precision... centered now"), `:280-285`; `docs/CALCULATIONS.md:1060-1061` ("was not bumped"); `src/ui/docks/plot/PlotWidget.cpp:83` ("sensor-fusion-jobs spec 9.6"). | State the current rule in each place. | done `e33fc63` |
| Q10 | 1 | `cmake/SolverSuperbuild.cmake:36-37` says a pin is changed "here and nowhere else", but the version is also encoded at `cmake/SolverDependencies.cmake:121,220`, in `tests/tst_solver_smoke.cpp`, the DLL names in `build.yml`, the README, and the goldens. | Replace with a keep-in-sync list; say that moving the GTSAM pin means re-validating the goldens. | done `2fa7c0d` |

Also flagged as inaccurate documentation (fold into Q1/Q6): `src/fusion/fusion.h:75`
says "Throws nothing except std::bad_alloc" but `runPipeline` has no
`catch (...)`.

### Nice-to-have

Status (2026-09-21): all done, in `137daf4` (phase 8), `2fa7c0d` (phase 1),
`532125b` (phases 4, 5, 6, 7, 9), `8d66503` (tests) and `e33fc63` (phase 10),
except, by decision: a non-converged fit keeps collecting its last-iterate
residuals and `StationaryWindow::imuCount`/`gnssCount` stay (the jobs-dock
spec needs both); the inconsistent rejection wording stays (frozen by the
goldens; Michael's call); `rowScript`/`realRowScript` stay unsplit (they
follow acceptance 15 step by step and share state).

| Phase | Finding |
|---|---|
| 4 | Leaf-input availability (preference, source measurement, source unit) is written twice, in `gatherInputs` and `inspectInstance` (`src/engine/calculationengine.cpp`), despite the "cannot drift apart" comment. Share one predicate. |
| 4 | `src/engine/preparedcalculation.h:125-131` omits the release-mode refusal when `publish()` is called during an evaluation. |
| 5 | If a `rowsInserted` slot cancels a new job, `jobQueued` is emitted after `jobFinished`. Document it or skip the emit. |
| 5 | `isEndState` in `src/jobmodel.cpp:16` duplicates `JobRecord::isFinished`. |
| 6 | Request loops at `src/plotrequests.cpp:586-598` and `:652-662` are the same code; the "visible loaded track" filter is duplicated at `:229` and `:810`; `inspectLocked` is a misleading name (no lock). |
| 7 | `src/ui/docks/plotselection/PlotRowDelegate.h:49-53` cites other source paths, which will rot. |
| 8 | Two functions named `channelsFrom` doing opposite things (`fusionoutput.cpp:56`, `fusionregistration.cpp:92`). |
| 8 | Pi literal repeated at `inputadapter.cpp:68` and `fusionoutput.cpp:61` although `kPi` is in scope. |
| 8 | `StationaryWindow::imuCount` / `gnssCount` are never read (the jobs-dock spec would use them). |
| 8 | Unnamed thresholds at `factorgraphfit.cpp:71,85`, `imuintegration.cpp:81,98`, `initializer.cpp:36,85,89,112`. |
| 8 | `channelsFrom` appends to 17 vectors without `reserve`. |
| 8 | A non-converged fit still rebuilds the graph and collects residuals (`factorgraphfit.cpp:148-151`) before the caller discards it. (The jobs-dock spec wants exactly this data kept.) |
| 8 | The same recording is validated up to four times with different message texts (frozen by the goldens; share the constants). |
| 8 | Terse names in `trajectoryreconstruction.cpp:39-57`, `stationarywindow.cpp:139-147`. |
| 8 | Rejection texts are user-visible in tooltips, untranslated and inconsistent ("fusion", "Sensor fusion", "Batch fusion"). Frozen by the goldens; Michael's call. |
| 9 | The 17 inputs are listed twice (`fusionregistration.cpp:54-79` and `:92-111`) while outputs are table-driven. The inverse time fit (`:216-231`) duplicates `src/calculations/timecalculations.cpp:160-173`. |
| 9 | `src/fusion/fusionregistration.cpp:3-10` orders includes project, Qt, std. |
| tests | Helpers copy-pasted instead of living in `tests/support`: `spin()` in three files; (unchecked) `sameBitsEverywhere` in three, `Quiet` in three, a hand-built description column in five although `logbookprobe.h` offers `descriptionColumn()`; fusion session helpers shared between `tst_fusion_jobs` and `tst_fusion_rows`. |
| tests | `Threads::Threads` linked per test, including tests that use no threads; link once on `flysight_test_support`. |
| tests | `rowScript` (149 lines) and `realRowScript` (155 lines) are overlong. |
| tests | Void helpers such as `setInput` use `QVERIFY`, which returns only from the helper. |
| tests | (unchecked) `tests/tst_calcengine_async.cpp:468,742` use an unbounded `acquire()`. |
| tests | `tests/CMakeLists.txt:9-18` header comment: "The one exception... The second... The third". |
| tests | `tests/tst_plot_row_delegate.cpp:934-944` hand-writes `main()`; test class in `tests/tst_solver_smoke.cpp:24` is `TstSolverSmoke`. |
| 1 | (unchecked) Fallback install-dir defaults at `cmake/SolverSuperbuild.cmake:66-74` are unreachable; speculative `UNKNOWN_LIBRARY` branch at `cmake/SolverDependencies.cmake:170-190`; `flysight_install_solver_runtime` is ~82 lines with a duplicated footer; a Debug application build against a Release-only solver install gets no warning. |
| 10 | `docs/LOCAL_COORDINATES.md` has no numbered sections or table of contents, unlike its siblings. |

Checked and clean: no dangling `BoostDiscovery` references; the CI cache key
hashes the pin file; every `tests/acceptance_map.txt` entry resolves; the
docs' counts (21 inputs, 18 outputs, 17 plots, 41 executables) match the
code; the worker/main partition in `JobQueue` is sound; `MainWindow` destroys
the request component, then the queue, then the model.

## C. Requirements coverage review

Verdict: no requirement unmet or contradicted. The full traceability matrix
(every normative requirement of spec sections 2 to 12, every "must NOT", and
acceptance 1 to 20, with file:line and test function) was produced in the
review; the acceptance part of it is also `tests/README.md` section 9.2 and
`tests/acceptance_map.txt` items 101 to 120.

### Proof gaps (met in code, automated proof missing or weaker than feasible)

| Id | Phase | Gap | Remedy | Status |
|---|---|---|---|---|
| G1 | 8 | Bit-exact parity is opt-in. By default (and in CI) the comparison tolerates `1e-9 + 1e-7*|x|` (`tests/fusion/fusiongolden.cpp:90`); the spec says anything beyond last-bit noise is a defect. Exact mode passes locally but nothing runs it automatically. | Register `tst_fusion_parity_exact` / `tst_fusion_kernel_exact` with `ENVIRONMENT FLYSIGHT_FUSION_EXACT=1`, enabled when the compiler matches `capture.json` (MSVC), or at least on the Windows CI job. | done `d0003c2` |
| G2 | 10 | The audit pins callers of `prepare(` and `publish(` but not the synchronous `calculationEngine().request(`. No `src/` file calls it today. Plugins never starting explicit work has no dedicated test. | `expect_none("no synchronous explicit request in product code" ...)`; optionally a `tst_python_bridge` case reading `Fusion/roll` and asserting `runCount == 0`. | done `d0003c2` |
| G3 | 9 | No test asserts that a logbook cell over a fusion output shows a number straight after publication (`columnOnFusionOutputIsNotCached` checks `data()` only after a marker edit). Code path is sound. | One assertion on `data(row, kRollColumn)` after `waitIdle`, before the marker edit. | done `d0003c2` |
| G4 | 7 | The "no data" warning suppression has no test; `isMerelyUncomputed` is in an anonymous namespace in `PlotWidget.cpp`; only manual M1 covers it. | Move the predicate into a core header and unit-test its four report states, or a message-handler check in a widget test. | done `d0003c2` |
| G5 | 6 | `LastTestsFailed.log` named `tst_plot_requests`. | Resolved: 50 consecutive passes on `90259d7`; the log predates the final commit. | done |

### Manual, or only verifiable after a push

| Id | Phase | Item | Status |
|---|---|---|---|
| M-a | 1, 10 | (First push 2026-09-21: GTSAM, app and tests build on Ubuntu, macOS arm64 and Intel; only test tolerances failed, fixed in `00b53a2`; install and deployment steps did not run because tests run first. Second push 2026-09-21: tests 42/42 on both macOS runners (tolerance fix held); the macOS deployment check then failed on a false positive in the CI script (otool header line), fixed in the `Phase 1 fixup: CI (unverified)` commit. Needs a third push.) Acceptance 1 on macOS and Linux: Boost-free GTSAM build, deployment and rpaths, `-stack_size`, `solver_stack_linux.cpp`, offscreen widget test, portable parity bound. Expect `Phase 1 fixup` commits. | open (needs a push) |
| M-b | 7, 10 | Manual script M1 to M9, `tests/README.md` section 12 (acceptance 15, 16, 17, 19 in the real window). Run by Michael 2026-09-21: M3 to M9 pass; M1 and M2 needed script corrections (`tests/README.md`), two confirmations outstanding; results in section E. | mostly done |
| M-c | 8 | Bit-exact parity is local-only (see G1). | done `d0003c2` (runs in ctest where the compiler matches capture.json; CI result needs a push) |
| M-d | 9 | `tst_fusion_jobs::realRecordingCheck` is optional and local, as the spec allows. Note: QtTest's 5-minute function timeout applies (decision 2026-09-21: leave it). | noted |
| M-e | - | Full `ctest` on HEAD unconfirmed by the reviewer. | done (42/42, orchestrator) |

### Deviations from the spec's wording

Recorded decisions: D1 upstream GTSAM pin instead of the fork; D2 GTSAM
without Boost and Boost removed from the build; D3 the application configure
requires a Boost-free GTSAM install even for non-fusion work; D4 no keyboard
or menu surface for refresh/cancel, and the Plots menu is not a gesture; D5
cancel wins over a compute that finished meanwhile; D6 portable tolerance mode
for goldens; D7 `CalculationCompatibilityVersion` 1 to 2 forced literal
updates in three tests; D10 simplification also declares `Local/down`; D11
cached logbook columns over explicit-backed outputs are stored as
unavailable.

Not recorded as decisions at the time:

| Id | Phase | Deviation | Status |
|---|---|---|---|
| D8 | 9 (and 2) | Plot colours differ from the branch: muted HSL colours instead of `Qt::dark*` for the fusion and local-frame plots. Spec: "as registered on the branch". | done `45bfb8e` (all 23 plots; Michael, 2026-09-21) |
| D9 | 8 | Progress texts reworded ("Starting fit", "Pass n, iteration m" against the branch's "Heading 0 deg, ..."). Boundaries identical and tested. | accepted, harmless |

## D. Found after the reviews

| Id | Finding | Status |
|---|---|---|
| X1 | `24-09-05/11-17-12` does not converge on this branch (5 x 100 iterations, ~7 min) although it converges on `sensor-fusion-clean-port`. Cause: `master`'s legacy gyro-scale correction (x 1.14688) changed the solver's input; the kernel itself is identical (a `SCHEMA_VER` 2 copy reproduces the branch's objective `197341.2190770153` exactly). | Decision 2026-09-21: leave the engine as is; solver work later; reference case. |
| X2 | Failure diagnostics keep only the reason and drop the input audit and iteration history. | Addressed by `PLANS/jobs-dock.md`. |
| X3 | `realRecordingCheck`'s 30-minute `waitIdle` is unreachable under QtTest's default 5-minute function timeout. | Decision 2026-09-21: leave at 5 minutes. |

## E. Manual verification checklist (M-b)

The steps are defined in `tests/README.md` section 12 (tracked; the
acceptance map cites them as `manual M<k>`). This is only the place to record
results. Run `build-phase1/install/FlySightViewer.exe`.

**Before the first launch:** copy the `FlySight Viewer` logbook folder to a
scratch folder and point `general/logbookFolder` at the copy (release build's
Preferences, or the registry value under
`HKEY_CURRENT_USER\Software\FlySight\FlySightViewer\general`). A
development build rewrites `index.json` at start-up and M8 deletes sessions.
Restore the preference afterwards and check the real `index.json`'s
modification time is unchanged.

**Recordings:** three or more with IMU data, one without, one that fails.
`TEMP/data` has plenty; `.../comp 5 .../24-09-05/11-17-12` fails (does not
converge, about 7 minutes); for a fast rejection delete a few seconds of
`$IMU` rows from a copy of a SENSOR file.

| Step | Acceptance | What to see | Result | Notes |
|---|---|---|---|---|
| M1 Startup | 16 | Check Roll with three fusable tracks, let it compute, quit, restart: row checked, refresh control with count 3, nothing starts, no "No data available" line for the fusion plot. | pass? (2026-09-21) | Roll still checked after restart; tracks are not (visibility is not persisted), so no refresh control until the tracks are shown again, then "works as usual". Script corrected (`tests/README.md`). To confirm: after re-showing the tracks the row showed refresh with 3 and nothing started by itself. |
| M2 Profile and menu | 16 | Apply a profile that checks fusion plots; toggle one through the Plots menu and its shortcut: refresh counts appear, nothing starts. | n/a + open | No fusion plot is in the Plots menu or has a shortcut (the menu is a fixed GNSS list): that half does not apply; script corrected. Profile half not reported yet. |
| M3 Refresh | 15 | Press refresh: "0 of 3" and cancel; Pitch and Yaw show the same progress; tooltip lists the computing track with solver text and the queued ones; graphs, legend and logbook column fill in as each fit publishes; row ends plain. | pass | Roll, pitch, yaw refresh and cancel in sync; legend updates as tracks complete. Cosmetic: the legend is evaluated at the cursor position before the plot axes re-layout, so when the axes grow and end up under the cursor the legend shows the value for where the cursor used to be. Michael: not sure worth fixing. |
| M4 Check gesture | 15 | Uncheck and re-check by clicking the box: jobs start. Same with Space. | pass |  |
| M5 Interactive during a fit | 19 | Pan, zoom, switch tools, hide/show tracks, edit a description, set a marker, open Preferences: nothing blocks, no dialog. Note what you did. | pass |  |
| M6 Cancel | 15 | One running, two queued, press cancel: at once refresh with 3, plot stays checked, other fusion rows change identically, CPU idle within one solver step. Refresh recomputes. Note the wait. | pass | Refresh and cancel act on CPU usage almost immediately (Task Manager). |
| M7 Fourth track and failure | 15 | Show a fourth fusable track: refresh with 1, nothing starts, refresh computes it. No-IMU recording never appears in a count or tooltip. Failed recording: warning badge with 1, tooltip gives the reason, no refresh offered for it, no message box. | pass |  |
| M8 Remove and unload | 17 | One running and one queued: delete the queued track's session, then the running one's: no crash, no hang, nothing published, counts fall. | pass |  |
| M9 Quit | 17 | One running and two queued, close the window: wait cursor briefly, application exits, process gone, logbook intact on restart. Repeat with File > Exit. Note the wait. | pass | About 1 s delay either way. |

New since the script was written, worth a look while you are there:

| Check | What to see | Result | Notes |
|---|---|---|---|
| Edit during a fit | While a fit runs, edit an input of that track (or re-import its file): the row flips to refresh at once, the job ends Superseded within one solver step, and refresh starts a new fit straight away (`13d4b81`). | | |
| Plot colours | The 17 "Sensor fusion" and 6 "GNSS (Local frame)" plots use the colours of `sensor-fusion-clean-port` (`build-port/install`) (`45bfb8e`). | | |

Without the fusion plots (side by side with a `master` build, light and dark
theme, 100% and 150% scaling):

| Check | Result | Notes |
|---|---|---|
| Every ordinary plot row looks exactly as before: no glyph, count or tooltip; same height and elision. | | |
| Checking and unchecking by mouse, Space, Plots menu, shortcut and profile behaves as before. | | |
| A visible recording without IMU and an IMU plot checked still logs "No data available for plot". | | |
| Quit via File > Exit and the close button, Debug and Release: no crash, no hang, no debugger output about destroyed objects or running threads. | | |

