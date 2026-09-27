# Phase 10: Documentation, acceptance map, and audit

## Overview

This is the closing phase. It adds no feature. It (1) reconciles the places
where the phase documents 1-9 disagree with each other or left work for "Phase
10", (2) adds the one acceptance test no earlier phase could write - the
end-to-end plot-row script with the **real** fusion plots (`PlotRequests` +
`JobQueue` + the fusion registration), (3) replaces the two provisional
acceptance-map conventions by one scheme covering this specification's
criteria 1-20 and makes `audit_cleanup` enforce it, (4) turns acceptance 20
("none of the branch's modal dialog, re-entrancy guard, idle-scheduler pause,
or plot-widget rebuild guard exists") and the GTSAM confinement of spec
section 3 into permanent automated checks, (5) writes the final documentation
(`docs/SENSOR_FUSION.md`, user-facing `docs/COMPUTED_PLOTS.md`, the engine
docs, both READMEs), and (6) runs the full suite and the deferred manual
script (acceptance 15 visual, 16, 17, 19).

## Dependencies

- **Depends on:** Phases 1-9, all committed on `sensor-fusion-jobs`.
- **Blocks:** None (final phase).
- **Assumptions (names used verbatim from the earlier documents):**
  - Test targets that exist and pass: the 24 of `master` plus
    `tst_solver_smoke` (1), `tst_time_fit`, `tst_local_coordinates` (2),
    `tst_simplified_track`, `tst_map_models` (3), `tst_calcengine_async`,
    `tst_calcengine_blockers` (4), `tst_jobqueue`, `tst_jobmodel` (5),
    `tst_plot_requests` (6), `tst_plot_row_layout`, `tst_plot_row_delegate` (7),
    `tst_fusion_parity`, `tst_fusion_kernel` (8), `tst_fusion_session`,
    `tst_fusion_jobs` (9): 40 executables, plus `audit_cleanup`.
    `solver_deploy_probe` is an executable but not a test.
  - Solver: GTSAM is the pinned upstream revision built **without Boost**
    (Phase 1); locally it is installed in `build-solver-deps/` (git-ignored),
    and every local configure passes `-DGTSAM_INSTALL_DIR` /
    `-DONETBB_INSTALL_DIR` pointing there. The old Boost-enabled
    `third-party/GTSAM-install` is never used, staged or modified. Phase 8's
    goldens were captured against the Boost-free build and cross-checked
    byte for byte against the old one (`tests/data/fusion/capture.json`).
  - CMake: options `FLYSIGHT_BUILD_SOLVER_DEPS`, `FLYSIGHT_BUILD_FUSION_TESTS`
    (root + tests), `FLYSIGHT_BUILD_WIDGET_TESTS` (tests only, see finding
    F10); helpers `flysight_add_test`, `flysight_add_fusion_test`,
    `flysight_solver_stack`, `flysight_solver_test_environment`,
    `flysight_install_solver_runtime`; targets `flysight_model`,
    `flysight_core`, `flysight_fusion` (`PUBLIC flysight_model Qt::Core PRIVATE
    gtsam`), `flysight_test_support`, `flysight_fusion_test_support`,
    `flysight_fusion_session_support`, `FlySightViewer` (links
    `flysight_fusion`).
  - Core API: `CalculationEngine::{prepare, blockers, readiness, resultDetail,
    preparedCount, request, runCount, resultStatus}`, `PreparedCalculation`,
    `CalculationProgress`, `CalculationCancelled`,
    `CalculationRegistry::{title, staticDependencies, candidatesFor}`;
    `JobQueue::{request, activeJobs, job, runningJob, cancel, shutdown, isIdle,
    model}` with signals `jobQueued / jobStarted / jobProgress / jobFinished /
    idle`; `JobModel`, `JobRecord`, `JobState`;
    `SessionModel::{pinSession, publishCalculationInvalidation, mergeSessions,
    updateAttribute, setRowsVisibility, removeSessions}`;
    `PlotRequests::{plotId, rowState, plotCheckedByUser, refreshPressed,
    cancelPressed, flush}`, `PlotRowState`, `PlotTrackState`; `PlotModel` in
    `flysight_core`; `Fusion::registerFusionCalculations`,
    `Fusion::FitCalculationId` (`"builtin.fusion.fit"`),
    `SessionKeys::FusionDiagnostics`, `SessionKeys::LocalOrigin*`.
  - Test support: `jobfixture.h` (`waitIdle`), `plotfixture.h` (`show`,
    `giveInput`), `asyncdriver.h`, `tests/fusion/fusionfixtures.*`,
    `fusiongolden.*` (`loadFusionGolden`, `compareSamples`, `compareJson`,
    `exactParityRequested`), `fusionsessions.*` (`registerFusionOnce`,
    `sessionFromFixture`, `sessionWithoutImu`, `fusionMeasurementNames`).
  - `tests/acceptance_map.txt` holds, after the previous plan's items 1-19,
    real lines numbered 102, 103 (Phases 2, 3) and 104 (Phase 8), and `# SFJ n`
    comment lines (Phases 4, 5, 6, 7, 9), possibly under two different block
    headers. `tests/README.md` holds the nine-step script "Manual
    verification: plot-driven jobs" (Phase 7) and "Fusion golden parity"
    (Phase 8).
  - The implementer runs **no git command that changes repository state**
    (overview "Commit Policy"; the orchestrator commits). Read-only git is
    fine. This phase edits `.github/workflows/build.yml` in **Task 10.12
    only** (removal of the Boost steps); that path is reported under its own
    heading and committed separately as `Phase 10: CI (unverified)`. No other
    task touches a workflow file.
  - macOS and Linux cannot be built or run locally. Nothing is pushed.

---

## Acceptance traceability matrix (spec section 11, items 1-20)

"Phase" = where the named evidence is defined. **NEW** = defined in this
document (task in brackets). Map item number = 100 + n (Task 10.4).

| # | Clause | Evidence | Phase |
|---|---|---|---|
| 1 | solver libraries build, link, run; right GTSAM build; 64 MiB main-thread stack | `tst_solver_smoke::gtsamIsTheRightBuild`, `smallGraphOptimizes`, `mainThreadHasSolverStack` | 1 |
| 1 | runtime libraries deployed (Windows, macOS, Linux); tests pass in CI | workflow steps "Verify third-party installations", "Verify deployment output (...)" incl. `solver_deploy_probe` (map kind `ci`); Windows verified locally [10.11]. **Open until a push: macOS / Linux** | 1, 10 |
| 1 | only fusion code links GTSAM; other tests build without it | **NEW** configure-time link confinement [10.6]; **NEW** audit group `solver-confinement` [10.5]; suite run with `FLYSIGHT_BUILD_FUSION_TESTS=OFF` [10.11] | 10 |
| 2 | exact clock at high uptime: microsecond-level | `tst_time_fit::highUptimeExactClock`, `fitFollowsTimeSource` | 2 |
| 2 | existing time-dependent tests unchanged | `tst_time_fit::fixtureFitIsExact`; the unedited `tst_builtins_golden`, `tst_builtins_engine`, `tst_session_engine` | 2 |
| 3 | origin gates | `tst_local_coordinates::originGates`, `speedAccuracyPlaysNoPart` | 2 |
| 3 | known displacement and velocity rotate correctly | `::knownDisplacement`, `::knownVelocityRotation` | 2 |
| 3 | NaN at the invalid index only; no fix: all unavailable; source changes invalidate; markers do not | `::invalidSamplesAreNaNAtTheirIndexOnly`, `::noQualifyingFixMakesEverythingUnavailable`, `::sourceChangesInvalidate`, `::markersAndDisplayDoNotAffectTheFrame` | 2 |
| 3 | seven outputs, same indices; 0.5 m | `tst_simplified_track::sevenOutputsShareIndices`, `droppedSamplesWithinTolerance` | 3 |
| 3 | duplicate endpoints, closed, degenerate, empty; non-finite skipped | `::duplicatePositionEndpoints`, `::closedTrack`, `::degenerateTracks`, `::emptyTrack`, `::nonFiniteSamplesAreSkipped` | 3 |
| 3 | projection once per recording, in the local-coordinate calculation | `::projectionRunsOncePerRecording`; audit group `local-projection` (Phase 3's two rules) | 3, 10 |
| 3 | no origin: no track, no dot, bounds; recovery | `tst_simplified_track::unavailableWithoutOriginAndRecovers`; `tst_map_models::noOriginRemovesTrackAndDot`, `boundsClearedWhenNoTrack`, `recoversAfterSourceCorrection` | 3 |
| 4 | port reproduces the goldens | `tst_fusion_parity::successFixturesMatchGolden`, `rejectionFixturesMatchGolden`; `tst_fusion_kernel::fitTraceMatchesGolden` | 8 |
| 5 | reads (outputs, `accH`, diagnostics, interpolated value) never run anything | `tst_fusion_session::readsNeverRunTheFit`; `tst_fusion_jobs::readersNeverStartAFit` | 9 |
| 6 | runs once, publishes together, derived values appear, second request runs nothing | `tst_fusion_session::requestRunsOnceAndPublishesTogether`; `tst_fusion_jobs::jobPublishesAllOutputsTogether` | 9 |
| 7 | async == sync | `tst_calcengine_async::asyncMatchesSync`; `tst_fusion_session::asyncMatchesSync`; `tst_fusion_jobs::queueMatchesSynchronousRequest` | 4, 9 |
| 8 | input change while running: superseded, nothing published, requestable | `tst_calcengine_async::inputChangeWhileRunningRefuses`; `tst_jobqueue::inputChangeWhileRunningSupersedes`; `tst_fusion_jobs::inputChangeDuringFitSupersedes` | 4, 5, 9 |
| 8 | change after publication drops result and dependents | `tst_calcengine_async::changeAfterPublicationDropsDependents`; `tst_fusion_session::changeAfterPublicationDropsEverything` | 4, 9 |
| 9 | rejection: unavailable, diagnostics reason, succeeded job with reason, no second run, fresh run after input change | `tst_fusion_session::rejectionIsACachedResult`; `tst_fusion_jobs::rejectedRecordingIsSucceededJob`; (synthetic) `tst_jobqueue::rejectionSucceedsWithReason` | 5, 9 |
| 10 | cancel stops at the next boundary, publishes nothing, requestable; next job starts | `tst_fusion_parity::cancelAtEachKindOfBoundary`; `tst_fusion_session::cancelStopsAtNextBoundary`; `tst_jobqueue::cancelRunningThenNextStarts`; `tst_fusion_jobs::cancelDuringFitThenNextJobStarts` | 5, 8, 9 |
| 11 | no-IMU session: never missing / pending / failed; no job possible | `tst_jobqueue::refusesMissingInput`; `tst_plot_requests::sessionWithoutInputIsNeverListed`; `tst_fusion_session::missingInputsAreNotApplicable`; `tst_fusion_jobs::noImuSessionCannotHaveAJob`; **NEW** `tst_fusion_rows::noImuSessionIsNeverCounted` (all 17 real rows) [10.3] | 5, 6, 9, 10 |
| 12 | blockers report fusion for `accH`, nothing after publication, never a fit | `tst_calcengine_blockers::derivedNameReportsExplicitBlocker`, `inspectionNeverRunsExplicit`; `tst_fusion_session::blockersReportFusion` | 4, 9 |
| 13 | B consumes A: one gesture runs A then B | `tst_calcengine_blockers::chainedBlockers`; `tst_plot_requests::chainedBlockersContinue` | 4, 6 |
| 14 | one job at a time; no duplicates | `tst_jobqueue::oneAtATimeInRequestOrder`, `duplicateRequestsCreateNoDuplicates` | 5 |
| 15 | row script, without widgets, synthetic | `tst_plot_requests::rowScript` | 6 |
| 15 | row script with the real fusion plots; roll / pitch / yaw share one job; `accH` blocked by fusion | **NEW** `tst_fusion_rows::realRowScript`, `rollPitchYawShareOneJob`, `accHRowIsBlockedByFusion`, `rejectedTrackShowsBadge` [10.3] | 10 |
| 15 | what the user sees | manual M3, M4, M6, M7 [10.11] | 7, 10 |
| 16 | startup restore / profile / programmatic check start nothing (logic) | `tst_plot_requests::startupRestoreStartsNothing`, `profileStyleApplyStartsNothing`, `programmaticCheckStartsNothing` | 6 |
| 16 | ... with the view attached; only a direct check is a gesture | `tst_plot_row_delegate::programmaticCheckStartsNothingWithViewAttached`, `startupStyleRestoreStartsNothingWithViewAttached`, `checkBoxClickIsGesture` | 7 |
| 16 | ... in the real `MainWindow` (cannot be constructed in the harness) | **NEW** audit group `gestures` [10.5]; manual M1, M2 [10.11] | 10 |
| 17 | remove / unload with queued or running job; shutdown | `tst_jobqueue::removeSessionWithRunningJob`, `removeSessionWithQueuedJob`, `evictionDeferredWhileJobActive`, `shutdownWithQueuedAndRunning`; `tst_fusion_jobs::shutdownDuringFit` | 5, 9 |
| 17 | quitting the application | manual M8, M9 [10.11] | 7, 10 |
| 18 | every transition through model signals; history from the model alone | `tst_jobmodel::historyFromSignalsAlone`, `retentionBound` | 5 |
| 19 | sessions editable, tracks shown / hidden, other values readable while a real fit runs | **NEW** `tst_fusion_rows::editsAndVisibilityDuringFit` [10.3]; `tst_jobqueue::idleSchedulerKeepsWorking` | 5, 10 |
| 19 | plots pan and zoom during a fit | manual M5 [10.11] - **widget-only, no automated test possible in the harness** | 10 |
| 20 | none of the branch's mechanisms exists | **NEW** audit group `branch-mechanisms` [10.5] | 10 |

**GAPs found (criteria for which phases 1-9 named no automated test):**
20 (owned by this phase: Task 10.5); 19 (only manual in Phases 7 and 9: this
phase adds the non-visual half, pan/zoom stays manual); the real-plot half of
15 (Task 10.3); the `MainWindow` half of 16 and the application-quit half of
17 (manual + audit; `MainWindow` is outside the test library boundary); the
macOS / Linux half of 1 (cannot be closed before a push).

---

## Cross-phase findings

"Here" = resolved by a task of this phase. "Amend" = the earlier document
should be corrected by the coordinator; this phase does not depend on it.

| # | Finding | Resolution |
|---|---|---|
| F1 | **Two acceptance-map conventions and two block headers.** `100 + n` audited lines: 02 Task 2.7, 03 Task 3.4, 08 Task 8.12. `# SFJ n` comment lines: 04 Task 4.8, 05 Task 5.8, 06 Task 6.8, 07 Task 7.8, 09 Task 9.9. 02 gives the header `# ---- Sensor fusion as an explicit calculation (PLANS/sensor-fusion-jobs.md) ----`, 04 gives `# ---- sensor-fusion-jobs (overview section 11); comment lines until phase 10`. The audit still checks only items 1-19. | **Here (10.4, 10.5).** |
| F2 | **`docs/CALCULATIONS.md` section numbers collide.** 04 Task 4.8 adds 12-14, 05 Task 5.8 adds 15, 06 Task 6.8 adds "16. Plot-driven requests" (07 Task 7.8 appends a subsection to it), and 09 Task 9.9 says "numbered next (16 if Phases 4 and 5 added 12-15)". Depending on commit order there are two sections 16 or 17 precedes 16. | **Here (10.1a):** final order 12 Asynchronous request, 13 Blocker inspection, 14 The threading rule, 15 Background jobs, 16 Plot-driven requests (with 07's subsection), 17 Sensor fusion as a registered calculation. **Amend** 09 Task 9.9 ("17"). |
| F3 | **Executable counts in `tests/README.md` are given as absolutes by phases that run in parallel**: 01 Task 1.4 "24 -> 25", 04 Task 4.8 "24 to 26", 05 Task 5.8 "26 to 28", 06 Task 6.8 "28 to 29"; 02, 03, 07, 08, 09 say "+2". | **Here (10.10):** the number is taken from `ctest -N`; expected 41 executables (40 above + `tst_fusion_rows`) plus the audit. **Amend** 01/04/05/06 to relative wording. |
| F4 | **`tests/README.md` section placement.** 08 Task 8.12 adds "## 11. Fusion golden parity" before Appendix A; 07 Task 7.8 adds an unnumbered "new final section" (after Appendix A). | **Here (10.10):** "## 12. Manual verification: plot-driven jobs", then Appendix A (previous spec), Appendix B (this spec); table of contents updated. |
| F5 | **The "nothing is named EKF" greps are wrong as written.** 09 Task 9.1: `grep -rni "ekf" src tests` "finds nothing". (a) `src/ui/docks/video/VideoWidget.cpp:124` contains `SP_MediaSeekForward`, which matches case-insensitively on `master` today. (b) 08 Task 8.12 requires `tests/README.md` to reproduce the capture harness verbatim, which names the branch files `imugnssekf.cpp` / `imugnssekf.h` (and the branch's `posN`). | **Here (10.5):** the audit uses the case-sensitive pattern `EKF|[Ee]kf` and excludes `tests/README.md` and `tests/data/fusion/capture.json`. **Amend** 09 Task 9.1 and DoD. |
| F6 | **07 "What must not appear" names `m_pendingRebuildLevel` as part of the branch's rebuild guard.** It exists on `master` (`PlotWidget.h:236`, `PlotWidget.cpp:421, 1675, 1681, 1688`). The branch added only `m_rebuildingPlot` and the `QScopedValueRollback` / `qScopeGuard` pair at the top of `updatePlot()`. | **Here (10.5):** the audit bans `m_rebuildingPlot|QScopedValueRollback` under `src/ui/docks/plot`. **Amend** 07. |
| F7 | **07 lists "any edit to `src/sessiondata.h`" under what must not appear**, while 02 Task 2.3 and 09 Task 9.1 legitimately add `SessionKeys` there. The branch's edit to that file was two keys: `ImuGnssEkf = "_IMU_GNSS_EKF"` (must not exist) and `FusionDiagnostics` (Phase 9 adds it). | **Here (10.5):** naming rule on `_IMU_GNSS_EKF|ImuGnssEkf`. **Amend** 07: "no `ImuGnssEkf` key" (its own "Phase 7 does not edit the file" stays true). |
| F8 | **"The only target that names `gtsam`".** 09 DoD 5 says `flysight_fusion` is the only target that names `gtsam`; 01 Task 1.4 links `tst_solver_smoke` and `solver_deploy_probe` to `gtsam`, 08 Task 8.12 links `tst_fusion_kernel` to it. 08 says "only *product* target", which is right. | **Here (10.6):** allowed direct namers are exactly those four. **Amend** 09 DoD 5. |
| F9 | **09 Tasks 9.7 / 9.8: "`blockers()` of all 18 plot names".** There are 17 plots (16 fit measurements other than `_time`, plus `accH`); 18 is the number of outputs of `builtin.fusion.fit`. | **Here (10.3):** the new test uses the 17 plot ids. **Amend** 09 ("the 17 plot names"). |
| F10 | **`FLYSIGHT_BUILD_WIDGET_TESTS` is not forwarded by the root superbuild.** 07 Task 7.7 defines it in `tests/CMakeLists.txt` only; 01 Task 1.1 forwards `FLYSIGHT_BUILD_FUSION_TESTS` through `_APP_CMAKE_ARGS` and documents it in `README.md`. With the usual Windows workflow (root superbuild) `-DFLYSIGHT_BUILD_WIDGET_TESTS=OFF` is silently ignored - the very escape hatch 07 describes for CI would not work. (Same defect as the previous plan's F3.) | **Here (10.1b).** |
| F11 | **Boost is dead weight after 01 and 03.** 01 builds GTSAM **without Boost** (Michael's decision after planning) and deliberately leaves the application's Boost lookup, README text and CI steps as on `master`; 03 removes the only Boost include in `src` (`boost::geometry`). After both, nothing needs Boost, but `find_package(Boost REQUIRED)`, `cmake/BoostDiscovery.cmake`, the `Boost::boost` link items, the README prerequisite and the CI install steps are all still there. | **Here (10.1c, 10.12):** remove Boost from the build, the README and CI. |
| F12 | **Two private definitions of "explicit-backed".** 06 (`PlotRequests::isExplicitBacked`, "use `candidatesFor()` only") and 09 Task 9.5 (`dependsOnExplicitCalculation` in `sessionmodel.cpp`, "`candidatesFor`, plus `sourceConversionsFor`"). Same answer today (source conversions are never explicit), two authorities for one fact. | **Here (10.2).** |
| F13 | **08's purity grep versus 09's registration file.** 08 Task 8.1: `grep "sessiondata\|engine/" src/fusion` finds nothing; 09 Task 9.1 puts `fusionregistration.*` there (and restates the grep with the exemption). 08 also notes `src/fusion` is not in the audit's "pure compute functions" rule. | **Here (10.5).** No amendment needed (09 already restates it). |
| F14 | **08 Task 8.1 criterion** "`git diff master -- src/CMakeLists.txt` shows one added hunk besides Phase 1's" ignores the hunks of phases 2, 4, 5, 6, 7. | None needed; stated so the final review does not read it as a violation. **Amend** 08 ("one hunk of this phase"). |
| F15 | **07 cites Phase 5 'Task "session model additions"'**; the task is 5.1 "`SessionModel` hooks - pinning and publication of engine-returned invalidations". | **Amend** 07 (wording only). |
| F16 | **Open questions answered by later phases:** 05 "cached logbook columns on explicit outputs" -> 09 Task 9.5; 04 "does a completed compute publish after a cancel request" -> 05 (cancel wins). | Listed as resolved in Task 10.11's hand-off. |

## Leftovers handed forward, and where they land

| From | Item | Task |
|---|---|---|
| 1 | Boost removal from build, README and CI (10.1c, 10.12; the macOS Boost / ICU deployment-target question is resolved by the Boost-free GTSAM); optional submodule pinning; `FLYSIGHT_BUILD_SOLVER_DEPS=OFF` warning for Michael's `build/`; README requirements complete | 10.9 (README check), 10.11 (hand-off) |
| 2 | README links to the new docs; audit range check; README matrix for the new items | 10.9, 10.5, 10.10 |
| 3 | Boost lookup, comment and link items in `src/CMakeLists.txt`, unused since this phase | 10.1c |
| 4, 5, 6 | acceptance-map scheme; user documentation of the controls | 10.4, 10.8 |
| 7 | nine-step manual script; `manual` map entries; keyboard limitation; `FLYSIGHT_BUILD_WIDGET_TESTS` | 10.11, 10.4, 10.8, 10.1b |
| 8 | purity grep exemption; `src/fusion` in the pure-compute rule; optional real-recording procedure (SCHEMA_VER note); portable parity bound may need a fixup after the first push | 10.5, 10.7, 10.11 |
| 9 | unify the explicit-backed predicate; manual acceptance 19; real-plot row test | 10.2, 10.11, 10.3 |

---

## Tasks

Order: 10.1 -> 10.2 -> 10.3 (code and tests green) -> 10.4 -> 10.5 -> 10.6
(map and audits green) -> 10.7-10.10 (documentation) -> 10.12 (CI Boost
removal; a text edit, any time after 10.1) -> 10.11 (full run, manual script,
hand-off; last). The application builds and all tests pass after each task.

### Task 10.1: Reconcile the shared files

**Purpose:** Remove the integration leftovers (F2, F10, F11) before building the map and audit on top of them.

**Files to modify:**
- `docs/CALCULATIONS.md` - section order and numbers (a).
- `CMakeLists.txt` (root), `README.md` - forward the widget-test option (b).
- `src/CMakeLists.txt`, `README.md`, `.github/CONTRIBUTING.md` - Boost removal (c).

**Files to delete:**
- `cmake/BoostDiscovery.cmake` (c). Delete the file in the working tree (no `git rm`; the orchestrator stages the deletion) and list it in the report under "deleted".

**Technical Approach:**

a. **Section numbers (F2).** Make sections 12-17 appear in the order given
   under F2, renumber headings, and fix every in-file cross reference
   ("section 15", "sections 12-14", the pointer sentence in section 8 that
   replaced "Nothing uses the policy yet"). Do not rewrite content.

b. **Option forwarding (F10).** In the root `CMakeLists.txt`, exactly as
   `FLYSIGHT_BUILD_PYTHON_TESTS` and `FLYSIGHT_BUILD_FUSION_TESTS` are handled
   (option declaration near line 80, the option comment block near lines
   67-71, `_APP_CMAKE_ARGS` near line 165, the summary near line 237): declare
   `option(FLYSIGHT_BUILD_WIDGET_TESTS "With FLYSIGHT_BUILD_TESTS: also build the tests that need Qt Widgets" ON)`
   and forward `-DFLYSIGHT_BUILD_WIDGET_TESTS=${FLYSIGHT_BUILD_WIDGET_TESTS}`
   unconditionally. Add the option to the README build-options table.

c. **Remove Boost (F11).** Precondition: Phase 1 shipped the Boost-free GTSAM
   (`<GTSAM install>/include/gtsam/config.h` has both Boost macros `0`). If
   Phase 1's fallback B (Boost-enabled GTSAM) was taken instead, do **not** do
   this task as written: remove only the two `Boost::boost` link items, keep
   the lookup with a comment "Boost: required by GTSAM", skip Task 10.12, and
   say so in the report.

   Verify first that nothing uses Boost. Each of these must find nothing:
   - `git grep -n -E "#include <boost/|boost::" -- src tests python_plugins`
     (Phase 3 removed `simplificationcalculations.cpp`'s includes);
   - `git grep -n -E "find_package\(Boost|find_dependency\(Boost" -- third-party/CMakeLists.txt cmake CMakeLists.txt tests`
     (the only hit in the tree is `src/CMakeLists.txt`; pybind11's own
     `tests/CMakeLists.txt` mentions Boost but is not built, and
     KDDockWidgets, GeographicLib and QCustomPlot do not use it);
   - `git grep -n "BoostDiscovery" -- ':!README.md' ':!src/CMakeLists.txt' ':!cmake/BoostDiscovery.cmake'`
     (`src/CMakeLists.txt` is the only includer; Phase 1's
     `SolverSuperbuild.cmake` does not include it).
   If one of them finds something, stop and report it; do not remove Boost
   around a remaining user.

   Then, in `src/CMakeLists.txt`:
   - remove the `include(.../BoostDiscovery.cmake)` line and its two comment
     lines (lines ~95-97), and `BOOST_ROOT, BOOST_LIBRARYDIR` from the
     conventions comment (line ~36);
   - remove the Boost comment block and `find_package(Boost REQUIRED)`
     (lines ~217-221). Phase 1's `include(.../SolverDependencies.cmake)` that
     follows it stays exactly where it is (after GeographicLib discovery);
   - remove `Boost::boost` from `target_link_libraries(flysight_core ... PRIVATE ...)`
     (line ~310) and from `target_link_libraries(FlySightViewer PRIVATE ...)`
     (line ~440);
   - in the deployment comment at line ~666, drop "Boost".
   Delete `cmake/BoostDiscovery.cmake`.

   In `README.md`: remove the Boost row of the prerequisites table (line ~76),
   the "Boost Components" subsection (lines ~101-116) and its table-of-contents
   entry if it has one, the `BOOST_ROOT` row of the path-variable table
   (line ~228), `BoostDiscovery.cmake` from the project tree (line ~315), and
   the "Boost not found" troubleshooting entry (lines ~659-665; renumber the
   entries after it and fix links to `#boost-components`). In the solver
   subsection Phase 1 added, keep the sentence that GTSAM is built without
   Boost, and add: "FlySight Viewer does not use Boost." In
   `.github/CONTRIBUTING.md` (not a workflow file; it goes in the phase
   commit): remove the two Boost rows of the version table, the "Install
   Boost" setup step, `boost` from the `brew install` line, `libboost-all-dev`
   from the `apt` list, and the "Boost not found (Windows)" entry.

   Verify with a **fresh** build directory and no `BOOST_ROOT` in the
   environment or on the command line (Task 10.11 step 1 does this). If
   anything then fails to configure or compile for want of Boost, stop and
   report which file needed it; do not put Boost back piecemeal.

**Acceptance Criteria:**
- [ ] `docs/CALCULATIONS.md` headings are numbered 1-17 without gaps or repeats, in the order of F2; `git grep -n "section 1[2-7]" -- docs` references resolve.
- [ ] With the superbuild configured `-DFLYSIGHT_BUILD_TESTS=ON -DFLYSIGHT_BUILD_WIDGET_TESTS=OFF`, `FlySightViewer-build/CMakeCache.txt` has `FLYSIGHT_BUILD_WIDGET_TESTS:BOOL=OFF` and `ctest -N` does not list `tst_plot_row_delegate`.
- [ ] `git grep -n -E "find_package\(Boost|Boost::boost|BoostDiscovery|BOOST_ROOT|BOOST_LIBRARYDIR" -- CMakeLists.txt src cmake tests third-party/CMakeLists.txt README.md .github/CONTRIBUTING.md` finds nothing; `cmake/BoostDiscovery.cmake` does not exist; the only remaining `boost` text in tracked CMake is Phase 1's two `-DGTSAM_…BOOST…=OFF` arguments, its Boost-free guard, and comments about them.
- [ ] A fresh configure with `BOOST_ROOT` unset succeeds; `build-phase10/FlySightViewer-build/CMakeCache.txt` contains no `Boost_` or `BOOST_` entry; application, `flysight_cpp_bridge` and all tests build; `tst_solver_smoke` passes (including its two Boost-macro assertions).
- [ ] `README.md` and `.github/CONTRIBUTING.md` no longer list Boost as a prerequisite, and no link to a removed README anchor remains (`git grep -n "boost-components"` finds nothing).

**Complexity:** S

---

### Task 10.2: One authority for "depends on an explicit calculation" (F12)

**Purpose:** Replace the two private predicates by one registry query, so row state and column caching can never disagree.

**Decision: do it**, because it is small and registration-only. **Stop rule:** if the diff exceeds about 60 changed lines outside tests, or any existing test needs an edited expectation, revert the task, leave both predicates, and record that in the report (the audit rule below is then omitted).

**Files to modify:**
- `src/engine/calculationregistry.h` / `.cpp` - `bool dependsOnExplicit(const DependencyKey &name) const;` next to `staticDependencies()`.
- `src/plotrequests.cpp` - `isExplicitBacked()` calls it (keeps its per-plot memo; the registry observer still clears it).
- `src/sessionmodel.cpp` - remove the file-local `dependsOnExplicitCalculation`; `computeColumnValues()` calls the registry.
- `tests/tst_calcregistry.cpp` - `dependsOnExplicit`.
- `docs/CALCULATIONS.md` - one sentence in section 16 and in section 17's column-cache rule naming the query.

**Technical Approach:**
- Definition (Phase 9's, the wider of the two): true when any name in `staticDependencies(name).names` (the name itself included) has a candidate from `candidatesFor(n)` or, for measurements, `sourceConversionsFor(...)`, whose descriptor has `policy == EvaluationPolicy::Explicit`. A function of the registrations only; no engine, no session, nothing runs; const; safe to call from a registry observer's later pass (not from inside the observer callback, as today).
- Memoize exactly as `staticDependencies()` is memoized and clear with it.
- Test on a private registry with `Synthetic::registerExplicitWorld`: `DDA` (two on-demand levels above `expA`) true; `EA_DIAG` true; `EA_IN` (stored input) false; `DB` true; an unknown name false; after `unregister("expA")`, `DDA` false.

**Acceptance Criteria:**
- [ ] `tst_calcregistry::dependsOnExplicit` passes; `tst_plot_requests`, `tst_column_cache` (incl. `explicitBackedColumnIsNeverCached`), `tst_fusion_jobs::columnOnFusionOutputIsNotCached` pass **unedited**.
- [ ] `git grep -n "EvaluationPolicy::Explicit" -- src` has hits only under `src/engine/` and in `src/fusion/fusionregistration.cpp`.
- [ ] `flysight_model` still links Qt Core only.

**Complexity:** S

---

### Task 10.3: `tst_fusion_rows` - the row script with the real fusion plots

**Purpose:** Acceptance 15 end to end on the real names: `PlotModel` + `PlotRequests` + `JobQueue` + `SessionModel` + `Fusion::registerFusionCalculations`, with real fits on the queue's 64 MiB worker; plus the real-plot halves of 11 and 19.

**Files to create:** `tests/tst_fusion_rows.cpp` - class `FusionRowsTest`, `FLYSIGHT_TEST_MAIN`, `#include "tst_fusion_rows.moc"`.

**Files to modify:**
- `tests/fusion/fusionsessions.h` / `.cpp` - add `QVector<FlySight::PlotValue> fusionPlots();`: the seventeen plots as `PlotValue`s (category `"Sensor fusion"`, sensor `"Fusion"`, measurements `north, east, down, velN, velE, velD, accN, accE, accD, accH, roll, pitch, yaw, qx, qy, qz, qw`, names / units / types as 09 Task 9.4's table; colours irrelevant). Comment: "mirrors `MainWindow::registerBuiltInPlots()`; `audit_cleanup` pins that list at seventeen rows".
- `tests/CMakeLists.txt` - inside `if(FLYSIGHT_BUILD_FUSION_TESTS)`, after Phase 9's two tests:
  ```cmake
  # The plot-row script with the real fusion plots (acceptance 15, end to end)
  flysight_add_fusion_test(tst_fusion_rows SOURCES tst_fusion_rows.cpp
    LIBS flysight_fusion_session_support Threads::Threads)
  ```

**Technical Approach:**

*Fixture* (pattern: 09 Task 9.8 `tst_fusion_jobs`, plus 06 Task 6.7's object
order). `initTestCase()`: `registerBuiltIns()`, `registerFusionOnce()`.
`init()`: fresh logbook, preferences reset, `LogbookManager::initialize()`,
registry snapshot; then `SessionModel`, `JobQueue`, `PlotModel`
(`setPlots(fusionPlots() + six "GNSS (Local frame)" PlotValues on sensor Local)`),
`PlotRequests`. `cleanup()`: destroy in reverse; compare the snapshot;
`enrolledEngineCount() == 0`. Sessions enter through
`model.mergeSessions(QList<SessionData>)` + `waitForIdle(model)` and are shown
with `plotfixture.h`'s `show()`.

*Sessions* (coarse fixtures only, to bound the run time): `s1` =
`sessionFromFixture(coarse_maneuver)`, `s2`, `s3`, `s4` =
`sessionFromFixture(coarse_linear)` under different ids, `n1` =
`sessionWithoutImu(coarse_linear)`, `r1` = `sessionFromFixture(reject_origin)`.

*Determinism without a gate* (09 Task 9.8's technique): a real fit cannot be
held, but a slot on `JobQueue::jobProgress` acting on the **first** text of a
given job runs while that job is still `Running` for the queue, whatever the
worker has done meanwhile. Map job ids to sessions through
`queue.job(id).sessionId` after the gesture. Counts are read after
`requests.flush()`. A slot on `jobFinished` that calls `flush()` and records
`rowState("Fusion/roll")` observes each publication before the next job
starts (`startNext()` is always queued). No sleeps.

*Test functions* (plot id literals: `"Fusion/roll"` etc.; "nothing started" =
empty `jobQueued` spy and unchanged `queue.model()->rowCount()`):

| Function | Script and literal expectations |
|---|---|
| `allSeventeenFusionPlotsAreExplicitBacked` | check all 17 + the six `Local` plots programmatically with `s2` visible; `flush()`: every fusion row `explicitBacked`, `missingCount 1`, `Control::Refresh`; every `Local` row equals a default `PlotRowState`; nothing started; `runCount("builtin.fusion.fit") == 0` |
| `realRowScript` (acceptance 15) | 1. show `s1 s2 s3`; `setPlotEnabled("Fusion","roll",true)`; flush: `missingCount 3`, `Refresh`, nothing started. 2. `plotCheckedByUser("Fusion/roll")` returns 3; three `Queued`/`Running` records, all `calculationId == "builtin.fusion.fit"`, `calculationTitle == "Sensor fusion"`; `pendingCount 3`, `progressLabel "0 of 3"`, `Control::Cancel`. 3. the `jobFinished` recorder sees, after `s1`'s job, `pendingCount 2` and `"1 of 3"`; `Fusion/roll` of `s1` matches the `coarse_maneuver` golden (`compareSamples`). 4. on the first progress text of `s2`'s job: `setPlotEnabled("Fusion","roll",false)`: `s3`'s job `Cancelled`, reason `"No longer needed"`; `s2`'s job still `Running`; `waitIdle`: `s2` `Succeeded`, its `Fusion/roll` matches the `coarse_linear` golden; `s3` unavailable, `runCount == 0` there. 5. re-check + `plotCheckedByUser` returns 1; on that job's first progress text `cancelPressed("Fusion/roll")` returns 1: at once `isPlotEnabled` true, `pendingCount 0`, `missingCount 1`, `Refresh`; `waitIdle`: job `Cancelled`, `s3` unavailable, no `dependencyChanged` for `s3`'s fusion names. 6. `refreshPressed` returns 1; `waitIdle`; row `isPlain()`. 7. show `s4`: `missingCount 1`, `missing[0].sessionId == "s4"`, nothing started, also after two `QTest::qWait(0)` + `flush()`. 8. `refreshPressed` returns 1; `waitIdle`; `isPlain()`; `Fusion/roll` of `s4` matches the golden. Job history literal: `[Succeeded, Succeeded, Cancelled, Cancelled, Succeeded, Succeeded]` |
| `rollPitchYawShareOneJob` | `s2` visible; roll, pitch, yaw checked programmatically, then `plotCheckedByUser("Fusion/pitch")` returns 1; `plotCheckedByUser("Fusion/yaw")` returns 0; exactly one job record; on its first progress text all three rows have `pendingCount 1`, the same `pending[0].job`, the same non-empty `jobProgressText`; after `waitIdle` all three are plain and `runCount("builtin.fusion.fit") == 1` |
| `accHRowIsBlockedByFusion` | `s2` visible; only `Fusion/accH` checked; flush: `missing[0].calculationTitles == {"Sensor fusion"}`; gesture returns 1; the job is `builtin.fusion.fit` (no job for `builtin.fusion.accH` exists or can exist); afterwards `Fusion/accH` is available, aligned with `Fusion/_time`, and `runCount("builtin.fusion.accH") == 1` |
| `noImuSessionIsNeverCounted` (acceptance 11) | `s2`, `n1` visible; all 17 plots checked programmatically: before, during (first progress text) and after a gesture on `Fusion/roll`, `n1` appears in no row's `pending` / `missing` / `failed`; every row's counts are 1 or 0, never 2; gesture returns 1; `queue.request("n1", "builtin.fusion.fit").kind == MissingInput` |
| `rejectedTrackShowsBadge` (acceptance 9 seen from the row) | `r1` visible; gesture on `Fusion/roll` returns 1; after `waitIdle`: job `Succeeded`; row `failedCount 1`, `showsWarning()`, `Control::None`, `failed[0].reason == "Sensor fusion: Local origin index outside GNSS samples"`, tooltip contains that text; `refreshPressed` returns 0; after `updateAttribute("r1","_LOCAL_ORIGIN_INDEX", qlonglong(0))`: `missingCount 1`, `Refresh`; refresh computes; row plain |
| `editsAndVisibilityDuringFit` (acceptance 19, non-visual half) | with only `s1` visible, `plotCheckedByUser("Fusion/roll")` returns 1; then show `s2 s3` (nothing starts for them). On the first progress text of `s1`'s job, from the main thread: `updateAttribute("s2","_DESCRIPTION","edited")` succeeds and is announced by `dependencyChanged`; `setRowsVisibility` hides and re-shows `s3`; `session(s2).getMeasurement("Local","north")` returns data. `s1`'s job still ends `Succeeded` and matches the golden (unrelated edits do not supersede); `waitForIdle(model)` afterwards saved the edit |

**Acceptance Criteria:**
- [ ] All functions pass in Release on Windows, 20 consecutive runs, under 120 s per run on the development machine (CTest timeout 600 s). If `coarse_maneuver` alone takes more than 20 s, use `coarse_linear` for `s1` too and say so in the report.
- [ ] The test names no `gtsam`, includes `fusion/fusionregistration.h` and no other `src/fusion/` header, and contains no `qSleep`, `QThread::sleep`, or `qWait(n > 0)`.
- [ ] With `FLYSIGHT_BUILD_FUSION_TESTS=OFF` the target does not exist.
- [ ] Expected values are literals or committed goldens.

**Complexity:** L

---

### Task 10.4: Final acceptance-map scheme and migration

**Purpose:** One scheme for this specification's items, every earlier line migrated (F1).

**Files to modify:** `tests/acceptance_map.txt`.

**Technical Approach:**

*Scheme (decided).* Item number = **100 + n** for acceptance n of
`PLANS/sensor-fusion-jobs.md`; items 1-19 of the previous specification are
untouched. Rationale: three phases already wrote audited `100 + n` lines, the
audit's line grammar needs no change for them, and the two specifications can
never be confused. Four line forms, one per kind of evidence:

```
<item> <tst_target> <function>    automated test function (unchanged form)
<item> manual M<k>                step M<k> of tests/README.md section 12
<item> ci <token>                 <token> occurs in .github/workflows/build.yml
<item> audit <group>              rule group <group> of cleanup_audit.cmake
```

*Migration of every existing line:*
1. Leading comment: rewrite to describe both ranges, the four forms, and that
   items 101-120 are stated in `tests/README.md` appendix B.
2. Items 1-19: unchanged.
3. Replace both provisional block headers by one: `# ---- Sensor fusion as an explicit calculation (PLANS/sensor-fusion-jobs.md): item = 100 + acceptance number ----`.
4. Keep the 102 / 103 / 104 lines of Phases 2, 3, 8 as they are.
5. Every `# SFJ <n> <target> <function>` comment becomes the real line `<100+n> <target> <function>`.
6. Every `# SFJ <n> manual ... steps a-b` comment becomes one `manual` line per step: 116 -> `M1`, `M2`; 117 -> `M8`, `M9`; 119 -> `M5`.
7. Add the lines of the matrix above that no phase wrote: 101 (`tst_solver_smoke` x3; `ci solver_deploy_probe`, `ci libgtsam`, `ci gtsam.dll`; `audit solver-confinement`), 102 `fixtureFitIsExact`, 103 `audit local-projection`, 107 / 108 `tst_session_engine asyncRequestOnSession`, 109 `tst_jobqueue rejectionSucceedsWithReason`, 110 `tst_fusion_parity cancelAtEachKindOfBoundary`, 111 / 115 / 119 the `tst_fusion_rows` functions, 112 `inspectionNeverRunsExplicit`, 115 `manual M3 M4 M6 M7` (one line each), 116 `audit gestures`, 119 `tst_jobqueue idleSchedulerKeepsWorking`, 120 `audit branch-mechanisms`, `audit naming`, `audit one-worker`.
8. Sort the new block by item, one `# 1nn - <clause>` comment per item, in the style of the existing blocks. No `# SFJ` text remains.

**Acceptance Criteria:**
- [ ] `git grep -n "SFJ" -- tests` finds nothing.
- [ ] Every item 101-120 has at least one line of kind test or audit; 115, 116, 117, 119 also have `manual` lines; 101 has `ci` lines.
- [ ] Every row of the matrix in this document has its lines, and no line names a function that does not exist (`audit_cleanup`, after Task 10.5).

**Complexity:** S

---

### Task 10.5: Audit - map parser, acceptance 20, naming, confinement, threading, gestures

**Purpose:** Make the audit enforce the new map, and turn acceptance 20 and the structural principles of spec section 12 into permanent rules.

**Files to modify:** `tests/audit/cleanup_audit.cmake`.

**Technical Approach:**

*Groups.* Add `function(audit_group slug)` that appends the slug to a global
property (`set_property(GLOBAL APPEND PROPERTY AUDIT_GROUPS ...)`); call it
once at the head of each group below (and wrap Phase 3's two rules as
`local-projection`). Header comment of the script: extend the summary with
"the mechanisms of `sensor-fusion-clean-port` that have no successor stay
absent (acceptance 120)".

*Traceability block.* Accept the four line forms of Task 10.4:
- test lines: as today;
- `^([0-9]+) +manual +M([0-9]+)$`: `tests/README.md` must contain the literal `**M<k> `;
- `^([0-9]+) +ci +([^ ]+)$`: `.github/workflows/build.yml` must contain the token (`string(FIND)`);
- `^([0-9]+) +audit +([a-z-]+)$`: the group must be in `AUDIT_GROUPS`.
Range check: every item must be in 1-19 or 101-120 (anything else is a
violation); each of 1-19 has a line (as today); each of 101-120 has at least
one test or audit line.

*Rules* (every pattern is a `git grep -E`; verify each against the finished
tree before adding it, and give each an `# Allow:` comment as the existing
rules have). `FUSION_CORE` = `src/jobqueue.* src/jobmodel.* src/plotrequests.* src/fusion`.
(The `|` inside a pattern is regex alternation, not a table separator: read
this table as raw text, as with the previous plan's audit table.)

| Group | Rule | Pattern | Where / expectation |
|---|---|---|---|
| `branch-mechanisms` | modal progress is the import dialog only | `QProgressDialog` | `expect_only` `^src/mainwindow\.cpp$` in `src`; `expect_count` 2 in `src` (the include and the import dialog, as on `master`) |
| | no nested event loop | `QEventLoop|processEvents` | none in `src` (none on `master` today) |
| | no re-entrancy guard, no "a calculation is running" flag | `thread_local|sensorFusionIsRunning|fusionRunning` | none in `src tests` |
| | the idle scheduler is never paused for a calculation | `[Ff]usion|[Jj]ob[Qq]ueue|calculations/|IsRunning` | none in `src/idlescheduler.cpp src/idlescheduler.h` |
| | jobs never touch the idle scheduler | `[Ii]dle[Ss]cheduler` | none in `FUSION_CORE` |
| | no plot rebuild guard | `m_rebuildingPlot|QScopedValueRollback` | none in `src/ui/docks/plot` (not `m_pendingRebuildLevel`: F6) |
| | no sibling-frame scaffolding | add `m_sideEffectFrames` to the existing "old engine" pattern | `${P}` |
| | no dialog or message box for a calculation outcome | `QMessageBox|QProgressDialog|QDialog|QErrorMessage|statusBar\(` | none in `FUSION_CORE src/engine src/calculations src/ui/docks/plotselection` |
| | no hand-cached failure | `catch *\(` | none in `src/fusion/fusionregistration.cpp` |
| `naming` | nothing is called an EKF | `EKF|[Ee]kf` | none in `src tests docs python_plugins cmake CMakeLists.txt README.md`, excluding `:!tests/README.md :!tests/data/fusion/capture.json` (F5) |
| | branch output names are gone | `posN|posE|posD|_IMU_GNSS_EKF|ImuGnssEkf` | none, same pathspec and exclusions |
| | seventeen fusion plots, six local-frame plots | `^ *\{"Sensor fusion", ` / `^ *\{"GNSS \(Local frame\)", ` | `expect_count` 17 / 6 in `src/mainwindow.cpp` (adjust the regex to the rows' actual spelling) |
| `solver-confinement` | GTSAM headers: kernel and its tests only | `#include <gtsam/` | `expect_only` `^src/fusion/` and `^tests/(tst_solver_smoke\.cpp|solverprobe\.h|solver_deploy_probe\.cpp|tst_fusion_kernel\.cpp)$` in `src tests cmake` |
| | public and registration files are GTSAM-free | `#include <(gtsam|Eigen)` | none in `src/fusion/fusion.h src/fusion/fusionregistration.h src/fusion/fusionregistration.cpp` |
| | the kernel is pure (F13) | `#include [<"](sessiondata|sessionmodel|engine/|jobqueue|plotrequests|preferences/|QApplication|QWidget|QtWidgets|QtGui)` | none in `src/fusion`, excluding `:!src/fusion/fusionregistration.cpp :!src/fusion/fusionregistration.h` |
| | the kernel does not log | `qWarning|qInfo|qDebug|qCritical` | none in `src/fusion` |
| | pure compute functions | add `src/fusion` to the pathspec of the existing "pure compute functions" rule | |
| | nobody but the application references the fusion library | `fusion/|Fusion::` | none in `src/calculations src/engine src/sessionmodel.* src/jobqueue.* src/jobmodel.* src/plotrequests.*` |
| | no Boost in FlySight sources or build (only if Task 10.1c ran in full) | `#include <boost/|boost::[a-z]|find_package\(Boost|find_dependency\(Boost|Boost::boost|BoostDiscovery` | none in `src tests cmake CMakeLists.txt third-party/CMakeLists.txt` (case-sensitive and narrow on purpose: Phase 1's `GTSAM_…_BOOST_…` option and macro names, and the `Boost::` test inside its Boost-free guard in `cmake/SolverDependencies.cmake`, must not match; plant a hit once to prove the rule works) |
| `one-worker` | one place creates a thread | `QThread|std::thread|QtConcurrent|QThreadPool|std::async` | `expect_only` `^src/jobqueue\.(cpp|h)$` in `src` |
| | no locks | `QMutex|QReadWriteLock|QWaitCondition|QSemaphore|std::mutex|std::shared_mutex|std::condition_variable` | none in `src` |
| | one atomic: the cancel flag | `std::atomic|QAtomic` | `expect_only` `^src/jobqueue\.cpp$` in `src` |
| `gestures` | gestures come from the row delegate only | `plotCheckedByUser|refreshPressed|cancelPressed` | `expect_only` `^src/plotrequests\.(h|cpp)$|^src/ui/docks/plotselection/PlotRowDelegate\.cpp$` in `src` |
| | explicit work is prepared and published in one place | `[.>]prepare\(|[.>]publish\(` | `expect_only` `^src/jobqueue\.cpp$|^src/engine/` in `src` |
| | no reader requests | `[.>]request\(` | `expect_only` `^src/plotrequests\.cpp$|^src/jobqueue\.(cpp|h)$|^src/engine/` in `src` (none of these exists in `src` on `master` today) |
| | no jobs window, no view of the queue | `[Jj]ob[Qq]ueue|JobModel` | `expect_only` `^src/ui/docks/AppContext\.h$` in `src/ui` |
| | one authority: explicit-backed (only if Task 10.2 held) | `EvaluationPolicy::Explicit` | `expect_only` `^src/engine/|^src/fusion/fusionregistration\.cpp$` in `src` |
| `widget-free-core` | the logic components see no widget | `QtWidgets|#include <Q(Widget|TreeView|AbstractItemView|StyledItemDelegate|Application|ToolTip)>` | none in `src/jobqueue.* src/jobmodel.* src/plotrequests.* src/plotmodel.* src/ui/docks/plotselection/PlotRowLayout.h` |

**Acceptance Criteria:**
- [ ] `ctest -L audit` passes on the finished tree and prints the new rule count.
- [ ] Planting each of the following (one at a time, never committed) makes it fail with a message naming the rule: `// QEventLoop` in `src/jobqueue.cpp`; `bool m_rebuildingPlot;` in `PlotWidget.h`; `#include <gtsam/geometry/Rot3.h>` in `src/mainwindow.cpp`; a map line `121 tst_smoke importFs2Sensor`; a map line `120 audit no-such-group`; removing every `119` line.
- [ ] Proof that the audit detects what it bans: export the branch's `src` outside the working tree (`git archive sensor-fusion-clean-port src | tar -x -C <scratch>`, read-only for this repository), run `git init` **in the scratch directory only**, and run `cmake -DREPO=<scratch> -P tests/audit/cleanup_audit.cmake`: the `branch-mechanisms` and `naming` groups report violations (`QEventLoop`, `thread_local`, `sensorFusionIsRunning`, `m_rebuildingPlot`, `_IMU_GNSS_EKF`, `posN`). Other groups fail too (the branch predates the engine); only these two are inspected. Delete the scratch directory afterwards.
- [ ] No rule matches `tests/audit` itself or anything under `PLANS/`.

**Complexity:** M

---

### Task 10.6: Configure-time GTSAM link confinement

**Purpose:** Spec section 3: "Only the code that needs GTSAM links it. The engine, session model, and job queue do not depend on GTSAM, and their tests build without it." A text audit cannot see link lines that come from variables or transitive interfaces; CMake can.

**Files to modify:**
- `cmake/SolverDependencies.cmake` - `function(flysight_assert_solver_confinement)`.
- `src/CMakeLists.txt` - one call directly after the `if(FLYSIGHT_BUILD_TESTS) ... endif()` block (line ~475), so test targets are in scope.

**Technical Approach:**
- Collect targets with `get_property(... DIRECTORY <dir> PROPERTY BUILDSYSTEM_TARGETS)` for `src/` and, when it was added, the tests directory.
- For each, walk `LINK_LIBRARIES`, and for every dependency that is a target its `INTERFACE_LINK_LIBRARIES`, recursively with a visited list: unwrap `$<LINK_ONLY:x>`, skip every other generator expression and every non-target item, resolve `ALIASED_TARGET`. This is the walker Phase 1 wrote for `FLYSIGHT_SOLVER_RUNTIME_TARGETS`; reuse its shape.
- Rules, each a `message(FATAL_ERROR ...)` naming the offending target and the path by which it reaches `gtsam`:
  1. targets that **name** `gtsam` directly are a subset of `flysight_fusion`, `tst_solver_smoke`, `solver_deploy_probe`, `tst_fusion_kernel`;
  2. targets that **reach** `gtsam` are a subset of those plus `FlySightViewer`, `flysight_fusion_test_support`, `flysight_fusion_session_support`, `tst_fusion_parity`, `tst_fusion_session`, `tst_fusion_jobs`, `tst_fusion_rows`;
  3. stated separately for a clear message although implied by 2: `flysight_model`, `flysight_core`, `flysight_test_support`, `flysight_cpp_bridge`, `qcustomplot` do not reach `gtsam`.
- On success print one `message(STATUS "GTSAM link confinement: OK (<n> targets reach gtsam)")`.

**Acceptance Criteria:**
- [ ] Configure succeeds with fusion tests `ON` and `OFF`, and with `FLYSIGHT_BUILD_TESTS=OFF`.
- [ ] Temporarily adding `gtsam` (or `flysight_fusion`) to `flysight_core`'s link line makes configure fail naming `flysight_core` (and every ordinary test that then reaches it); restored afterwards, not committed.
- [ ] No behaviour of any target changes (the function only reads properties).

**Complexity:** S

---

### Task 10.7: `docs/SENSOR_FUSION.md`

**Purpose:** Spec sections 4 and 10: carry the branch's document over, with names updated and "Calculation lifecycle" rewritten for this design.

**Files to create:** `docs/SENSOR_FUSION.md`. Start from `git show sensor-fusion-clean-port:docs/SENSOR_FUSION.md`. `docs/PORT_VALIDATION.md` is **not** carried over as a file (it is the branch's validation record; its durable content is section 8 below and `tests/README.md` section 11).

**Technical Approach - section outline and what each must state:**

1. **Summary.** A batch GNSS/IMU factor-graph fit over the whole recording; one registered calculation titled "Sensor fusion", explicit: it runs only when asked for from the plot list (link `COMPUTED_PLOTS.md`), in the background; zoom and markers do not select a fit window; no Python needed. Do not mention any historical identifier.
2. **Using it.** Seventeen plots in the "Sensor fusion" category; needs matching TRACK and SENSOR data; a fit takes seconds to minutes; results are not kept across restarts.
3. **Inputs.** The 21 declared inputs by name (09 "Declared inputs"): `GNSS/_time`, six `Local/...`, `GNSS/hAcc|vAcc|sAcc`, `IMU/_time`, six IMU channels, four `_LOCAL_ORIGIN_*`. Effective values only: **correct the branch text** "already SI after CSV import" - on `master` the conversion layer supplies m/s^2, and for a recording without `SCHEMA_VER` the gyro carries the legacy correction (link `DATA_SCHEMA.md` section 4); degrees/s become radians/s once, inside the kernel. Shared frame (link `LOCAL_COORDINATES.md`; hMSL approximation; no RAW.UBX). Shared UTC time; one common epoch subtracted and added back; no fusion-specific clock. Keep the branch's paragraph on the centered time fit as history, shortened, pointing to `tst_time_fit`.
4. **Model and output contract.** Carried over in substance (states per GNSS fix, shared biases, `ImuFactor` preintegration, prior sigmas, LM / QR, passes and thresholds, densities, dense reconstruction, acceleration formula). Outputs: sensor `Fusion`: `_time, north, east, down, velN, velE, velD, accN, accE, accD, roll, pitch, yaw, qx, qy, qz, qw`, aligned with `_time` = original IMU samples in `[first GNSS, last GNSS)`; unwrap rule shared with GNSS course (`calculations/anglehelper.h`); derived on demand `Fusion/accH`, `Fusion/_system_time`; attribute `_FUSION_DIAGNOSTICS` (list its top-level keys from 08 Task 8.7).
5. **Initialization and limitations.** Carried over unchanged in substance, including "numerical convergence does not establish physical accuracy" and the 08-35-23 example.
6. **What is rejected.** The rules of spec section 6 and 08 Task 8.3 (non-finite data, non-positive sigmas, non-monotonic time, coverage, IMU gap > 1.6 median intervals, GNSS gap > max(2 s, 5 median intervals), origin index); GNSS outside IMU coverage is trimmed, not rejected; no origin or no IMU is a *missing input*, not a rejection.
7. **Calculation lifecycle (rewritten).** Unavailable until requested; no read ever starts it (plots, legend, measure tool, logbook, map, export, plugins). A gesture on a plot row creates a job; **prepare** on the main thread captures the inputs, **compute** runs on the one worker thread (64 MiB stack) and sees nothing else, **publish** on the main thread installs all outputs at once, and ordinary invalidation repaints consumers. The engine refuses a result whose inputs changed (job superseded). Cancellation is observed at three kinds of boundary (before the fit, every 256 states of graph construction, before each optimizer iteration); a linear solve in progress finishes first. Rejections and solver failures are cached results with a reason (warning badge; nothing offers a retry until an input changes); out-of-memory and worker-start failure are never cached. A declared-input change drops the result and every dependent; marker and display edits do not. One job at a time; quitting cancels and waits for at most one solver step. Logbook columns on fusion outputs are not cached for unloaded sessions. **Must not describe:** a modal dialog, a nested event loop, sibling caching, a scheduler pause, a retry offer.
8. **Validation.** `tst_fusion_parity`, `tst_fusion_kernel`, `tst_fusion_session`, `tst_fusion_jobs`, `tst_fusion_rows`; goldens captured from `sensor-fusion-clean-port` `83a64479...`; exact and portable modes (link `tests/README.md` section 11). **Optional real-recording check** (not CI): `FLYSIGHT_FUSION_RECORDING`, run `tst_fusion_jobs realRecordingCheck` directly; reference numbers from the branch (17-26-24: objective 65602.22485051976, 9247 GNSS states, 24411 outputs); the objective matches only with a copy of `SENSOR.CSV` carrying `$VAR,SCHEMA_VER,2`, because the branch predates the legacy gyro correction; with the unmodified file a different objective is expected and correct. Replace the branch's "Native regression harness" section entirely.

**Acceptance Criteria:**
- [ ] All eight sections exist; `audit_cleanup` passes (`naming` group covers `docs`).
- [ ] `git grep -n -i -E "modal|progress dialog|event loop|sibling|idle scheduler|retry|fusion_regression|BUILD_TESTING" -- docs/SENSOR_FUSION.md` finds nothing, except a sentence stating that nothing offers a retry.
- [ ] Every name, id, file path and test mentioned exists (`git grep` each once); links resolve.

**Complexity:** M

---

### Task 10.8: User-facing documentation - `docs/COMPUTED_PLOTS.md`

**Purpose:** Spec section 10: explain the refresh control, the counts, the warning badge, and cancel.

**Decision on location.** The repository has no user guide, help menu, or
documentation site (checked: `README.md` is build-centric; the About box links
only to flysight.ca). `docs/DATA_SCHEMA.md` is the precedent for a
user-readable document in `docs/`. A separate file, not a section of
`SENSOR_FUSION.md`, because the controls belong to the plot list and to any
future explicit calculation, and because the principle "the plot is the
request" means the user should not need to know which calculation is behind a
plot.

**Files to create:** `docs/COMPUTED_PLOTS.md`, title "Plots that are computed on request". Written for users: no class names, no "explicit calculation", no "job queue" beyond one plain sentence.

**Section outline:**
1. **Why some plots need computing.** Most plots are instant; a few (today the "Sensor fusion" category) take seconds to minutes per track, so they are computed only when you ask. A check mark means "show this plot wherever its data is available", not "compute it".
2. **What a row can show.** Refresh icon with a number: that many visible tracks are not computed. "k of n" with a circled x: computing; k of the n tracks this row was waiting for are done. Warning triangle with a number: that many tracks could not be computed. Nothing: everything available (or the plot needs no computing). The tooltip lists the tracks behind each number, the current step of the running computation, and the reason for each failure. Several rows can show the same progress because they share one computation.
3. **Starting a computation.** Exactly two ways: check the plot in the list (mouse, or Space on the selected row), or press its refresh icon. What never starts one: starting the application with plots checked, applying a profile, the Plots menu and its shortcuts, showing a track, importing or merging files, editing a recording. After any of those the row simply shows the refresh icon.
4. **Cancelling.** The circled x stops the computations that row is waiting for; the plot stays checked, the tracks count as not computed again; other rows waiting on the same computations change the same way.
5. **When you stop needing a result.** Unchecking a plot or hiding a track removes computations that have not started; the one in progress finishes and its result is kept.
6. **When results go away.** When the recording's data changes (re-import, merge, a changed input) the track is "not computed" again. Results are not saved: after a restart, press refresh. Moving markers or zooming never discards a result.
7. **When a track cannot be computed.** The badge, the reason in the tooltip, no message box; there is no retry because the same data gives the same answer; the track becomes computable again when its data changes. Tracks that lack the needed sensor are silently absent, as for every other plot.
8. **While computing.** The application stays usable; one computation runs at a time, in the order requested; quitting stops it (a short wait).
9. **Known limitations.** Refresh and cancel have **no keyboard surface** (no shortcut, menu item, or context menu): a keyboard user unchecks and re-checks the row with Space to start, and cannot cancel from the keyboard. There is no window listing computations. Logbook columns based on a computed plot are blank for recordings that are not loaded.

**Acceptance Criteria:**
- [ ] The nine sections exist; every statement agrees with `tst_plot_requests`, `tst_plot_row_delegate` and the manual run of Task 10.11 (no behaviour is described that was not observed).
- [ ] The words used for the three tooltip headings are exactly Phase 6's ("Computing", "Not computed (press refresh to compute)", "Could not be computed").
- [ ] Linked from `README.md` and from `docs/SENSOR_FUSION.md` section 1.

**Complexity:** S

---

### Task 10.9: Engine and schema docs verified; root `README.md`

**Purpose:** Spec section 10, bullets 2 and 4, and the links Phase 2 deferred.

**Files to modify:** `docs/CALCULATIONS.md`, `docs/DATA_SCHEMA.md`, `docs/LOCAL_COORDINATES.md` (verification edits only), `README.md`.

**Technical Approach:**
- **`docs/CALCULATIONS.md` checklist** (fix what is missing; do not rewrite): sections 12-14 name every public type of Phase 4 with its thread (`prepare`, `PreparedCalculation::compute` "any thread, once", `publish`, `PrepareOutcome`, `PublishOutcome`, `ComputedCalculation`, `CalculationProgress`, `CalculationCancelled`, `blockers`, `readiness`, `resultDetail`, `title`, `setReason`), the failure classification table, "who publishes `invalidated`", and the threading rule including "explicit compute functions are re-entrant" and "no locks: remove the sharing"; section 15 the four end states with both mapping tables, dedup incl. the cancel-requested exception, pinning, `shutdown()` without a timeout; section 16 the five conditions, the two gestures and the complete list of non-gestures, continuation, cancel, pruning, the y-name-only rule, the view subsection (no keyboard surface; creation and teardown order); section 17 the three ids, 21 inputs, 18 outputs, outcome table, column-cache rule. Section 1's title line and the README link text should say the file now also covers background execution.
- **`docs/DATA_SCHEMA.md`:** Phase 9's two sentences (derived sensors `Local`, `Simplified`, `Fusion` in section 5; explicit-backed columns cached as unavailable in section 11) are present.
- **`docs/LOCAL_COORDINATES.md`:** Phase 2's body and Phase 3's "Simplified map track" section are present; add one closing sentence linking `SENSOR_FUSION.md` ("the fusion outputs use this same frame").
- **`README.md`:** (1) split "Developer Documentation" into **User documentation** (`docs/COMPUTED_PLOTS.md`, `docs/SENSOR_FUSION.md`, `docs/LOCAL_COORDINATES.md`, `docs/DATA_SCHEMA.md`) and **Developer documentation** (`docs/CALCULATIONS.md` with the extended description, `python_plugins/README.md`, `tests/README.md`); update the table of contents. (2) Project-structure tree: `src/fusion/` ("batch GNSS/IMU fit and its registration; library `flysight_fusion`, the only target that links GTSAM"), `src/jobqueue.*`, `src/jobmodel.*`, `src/plotrequests.*`, `src/ui/docks/plotselection/PlotRow*`, `tests/fusion/`, `tests/data/fusion/`, the three new `docs/` files, `third-party/GTSAM-install`, `oneTBB-install`. (3) Verify Phase 1's README items against the tree: every option (`FLYSIGHT_BUILD_SOLVER_DEPS`, `FLYSIGHT_BUILD_FUSION_TESTS`, now `FLYSIGHT_BUILD_WIDGET_TESTS`), path variable, clean target, the statement that GTSAM is built without Boost, the deployed runtime set per platform (five solver libraries, no Boost or ICU library from this feature). (4) Confirm Task 10.1c's Boost removal left the README coherent: no Boost prerequisite, no `BOOST_ROOT`, no dangling anchor; add to the solver subsection one "unverified" note: the Boost-free GTSAM build and its deployment on macOS and Linux are confirmed only by CI after a push.

**Acceptance Criteria:**
- [ ] Every identifier named in the checklist occurs in `docs/CALCULATIONS.md`; every file, option and target named in `README.md` exists in the tree.
- [ ] All relative links in `README.md` and `docs/*.md` resolve (check each target path with `Test-Path` / `ls`).
- [ ] `audit_cleanup` passes (its `naming` and "superseded commit" rules cover `docs` and `README.md`).

**Complexity:** M

---

### Task 10.10: `tests/README.md` final form

**Purpose:** One consistent test document after nine phases of additive edits (F3, F4).

**Files to modify:** `tests/README.md`.

**Technical Approach:**
- **Section 1:** the executable count from `ctest -N` (expected "41 executables plus the audit"; `solver_deploy_probe` named as a non-test). Every new target has a row in the right table; add a table "Solver and sensor fusion (label `fusion`)" if Phase 1 / 8 / 9 rows are scattered, and "Jobs and plot rows" for Phases 5-7. Correct the sentence "They need Qt Core, Gui and Test, GeographicLib, and Boost headers only" (Boost headers no longer; one test links Widgets; fusion tests link GTSAM).
- **Section 3:** options `FLYSIGHT_BUILD_FUSION_TESTS`, `FLYSIGHT_BUILD_WIDGET_TESTS` (both forwarded by the superbuild), labels `fusion`, `widgets`; `ctest -LE fusion` is the GTSAM-free run; environment variables `FLYSIGHT_FUSION_EXACT`, `FLYSIGHT_FUSION_RECORDING`.
- **Section 9:** keep the items 1-19 matrix as 9.1; add **9.2 "Sensor fusion and plot-driven jobs (items 101-120)"** = the matrix of this document without the Phase column, and one paragraph on the four line forms of the map.
- **Section 10:** extend the bullet list with the new groups (one bullet per group of Task 10.5), the four map line forms and the two item ranges, and the sentence that GTSAM *link* confinement is checked at configure time by `flysight_assert_solver_confinement` (Task 10.6), not by this script.
- **Section 11** "Fusion golden parity" (Phase 8) stays; **section 12 "Manual verification: plot-driven jobs"**: Phase 7's nine steps, each now opening with a bold id and its map items - `**M1 Startup (116).**` ... `**M9 Quit (117).**` - preceded by the rule "always on a COPY of a logbook" (Task 10.11) and the recordings needed. Text of the steps otherwise unchanged.
- **Appendix B:** the twenty acceptance items of spec section 11, verbatim, introduced as "items 101-120 of `tests/acceptance_map.txt`". Table of contents updated (11, 12, Appendix A, Appendix B).

**Acceptance Criteria:**
- [ ] The count in section 1 equals `ctest -N` minus `audit_cleanup`; every `tst_*` target of `tests/CMakeLists.txt` appears in a section 1 table and vice versa.
- [ ] `audit_cleanup` passes (`manual M<k>` lines resolve against the bold ids).
- [ ] Sections 9.2 and the map agree line for line (spot-check five items by hand, and report which).

**Complexity:** M

---

### Task 10.11: Full build, full run, manual script, hand-off

**Purpose:** Acceptance 1 (Windows half), the deferred manual verification (15 visual, 16, 17, 19), and the final report.

**Technical Approach:**

1. **Build (Windows, a new `build-phase10` directory; never Michael's `build/`, never the default solver install directories):**
   ```
   cmake -G "Visual Studio 17 2022" -A x64 -B build-phase10 -S . -DCMAKE_PREFIX_PATH="C:/Qt/6.9.3/msvc2022_64" -DFLYSIGHT_BUILD_THIRD_PARTY=OFF -DFLYSIGHT_BUILD_TESTS=ON -DGTSAM_INSTALL_DIR=<repo>/build-solver-deps/GTSAM-install -DONETBB_INSTALL_DIR=<repo>/build-solver-deps/oneTBB-install
   cmake --build build-phase10 --config Release
   ctest --test-dir build-phase10/FlySightViewer-build -C Release --output-on-failure
   ```
   Run this in a shell with `BOOST_ROOT` / `BOOST_LIBRARYDIR` unset and pass neither: after Task 10.1c the build must not look for Boost (check `CMakeCache.txt` for `Boost_` entries: none). The solver install is Phase 1's Boost-free `build-solver-deps/` (the one the goldens were captured against), never the old `third-party/GTSAM-install`.
   Expected: 41 executables + `audit_cleanup` pass; `tst_python_bridge` not "Disabled". Then: `$env:FLYSIGHT_FUSION_EXACT = "1"; ctest ... -R tst_fusion` (zero non-identical samples; quote the logged counts); `ctest ... -L audit`; reconfigure in `build-phase10-nofusion` with `-DFLYSIGHT_BUILD_FUSION_TESTS=OFF -DFLYSIGHT_BUILD_WIDGET_TESTS=OFF` and run the suite (no target reaches `gtsam` except `flysight_fusion` and `FlySightViewer`; Task 10.6 prints the count); once with `-DFLYSIGHT_BUILD_TESTS=OFF` (application and install step only).
2. **Install and deployment (Windows):** `cmake --install` as the README says; the five solver DLLs are in the install root and no `boost*.dll` is; copy `solver_deploy_probe.exe` in, run with `PATH=C:\Windows\System32`, remove it; start `FlySightViewer.exe` from the install directory with the same scrubbed `PATH`.
3. **Manual script M1-M9** (`tests/README.md` section 12) **on a copy**: copy a real logbook folder to a scratch location and point Preferences -> General -> logbook folder at the copy **before anything else**; restore the preference at the end. (A development build rewrites `index.json`: the compatibility marker is 2 and the calculation environment changed.) Use at least three recordings with IMU data, one without, and one the model rejects (made by hand if none is at hand: a copy of a SENSOR file with several seconds of `$IMU` rows removed from the middle is rejected for its IMU gap). Record pass / fail and a note per step; for M5 list what was done while the fit ran; for M6 and M9 note the observed wait.
4. **Hand-off list for Michael** (in the report, not a file): (a) what only a push can confirm - acceptance 1 on macOS and Linux: the Boost-free GTSAM / oneTBB build (GTSAM's own CI covers that configuration on Ubuntu only), the application build with no Boost installed on the runners (Task 10.12), runtime deployment and rpaths with no `libboost*` in the bundle or AppDir, `tst_solver_smoke::mainThreadHasSolverStack` on Linux (`RLIMIT_STACK` constructor), the portable parity bound of `tst_fusion_parity` / `tst_fusion_kernel` / `tst_fusion_session` on other compilers (expected remedy: `Phase 8 fixup` widening the bound for a named platform with observed numbers, never new goldens), and `tst_plot_row_delegate` under the offscreen plugin (remedy: `-DFLYSIGHT_BUILD_WIDGET_TESTS=OFF` in the workflow, a separate unverified CI edit); (b) open questions: submodule pinning preference (Phase 1); the two "let's try" decisions and their fallbacks (upstream GTSAM pin; GTSAM without Boost - Phase 1 Appendix) with what was observed, including Phase 8's byte-identical cross-check result; (c) resolved inside the plan: F16; (d) a reminder that `cmake --build build` in the everyday superbuild now builds GTSAM unless `-DFLYSIGHT_BUILD_SOLVER_DEPS=OFF` is set; (e) the cross-phase amendments F1-F15.

**Acceptance Criteria:**
- [ ] Steps 1-2 recorded with command lines and the CTest summaries.
- [ ] M1-M9 each marked pass / fail with a note; the logbook-folder preference restored; the real logbook untouched (compare `index.json` modification time before and after).
- [ ] The report states explicitly: "Acceptance 1 is confirmed on Windows only; macOS and Linux are unconfirmed until a push."
- [ ] The report lists every created, modified and deleted path; nothing under `third-party/`, `build*/`, `PLANS/`, `TEMP/`; `.github/workflows/build.yml` appears only under the separate heading "CI (unverified)" (Task 10.12).

**Complexity:** M (mostly manual time)

---

### Task 10.12: CI - remove Boost (committed separately as "Phase 10: CI (unverified)")

**Purpose:** After Task 10.1c nothing in the build looks for Boost; stop installing and verifying it on the runners.

**Files to modify:**
- `.github/workflows/build.yml` - **the only path of this task, and the only path of this phase reported under the separate heading "CI (unverified)".**

**Technical Approach (deletions only; line numbers are `master`'s, shifted by Phase 1's hunks):**

1. `env:` block (lines ~45-47): remove the comment, `BOOST_VERSION` and `BOOST_VERSION_UNDERSCORE`.
2. Remove the whole step "Install Boost (Windows)" with its banner comment (lines ~126-154).
3. macOS dependencies (lines ~159-170): `brew install ninja` (drop `boost`), and remove the "Export Boost location" comment and the architecture-specific `BOOST_ROOT` lines; if the `if`/`else` on architecture then has no other content, remove it too.
4. Linux dependencies: remove `libboost-all-dev` from the `apt-get install` list (line ~246; keep the line continuations valid) and the three lines that export `BOOST_ROOT=/usr` (lines ~261-263).
5. "Configure main application" (lines ~502-503): remove the `${BOOST_ROOT:+...}` and `${BOOST_LIBRARYDIR:+...}` arguments.
6. Remove the whole step "Verify Boost discovery" with its banner comment (lines ~512-543).
7. Leave untouched: everything Phase 1 added, in particular its GTSAM Boost-free checks and the `libboost*` absence checks in the deployment steps (those are the only places the word may remain), the cache step and its key, the release job.

Before editing, `git grep -n -i boost -- .github/workflows` and account for every hit as either removed by items 1-6 or kept by item 7; report any hit that is neither.

**Acceptance Criteria:**
- [ ] The file is valid YAML (`python -c "import yaml; yaml.safe_load(open('.github/workflows/build.yml'))"`), and every edited shell block passes `bash -n` when extracted.
- [ ] `git grep -n -i boost -- .github/workflows/build.yml` shows only Phase 1's Boost-absence checks (`GTSAM_ENABLE_BOOST_SERIALIZATION`, `GTSAM_USE_BOOST_FEATURES`, `Boost::`, `libboost`).
- [ ] The diff contains deletions only, apart from the rewritten `brew install` and `apt-get install` lines; no step other than the two removed ones changes name, order or `if:` condition; `audit_cleanup` still passes (every `ci` token of the acceptance map is still in the file).
- [ ] The implementation report lists `.github/workflows/build.yml` under its own heading "CI (unverified)" and nowhere else.

**Complexity:** S

## Testing Requirements

### Unit Tests
- New target: `tst_fusion_rows` (10.3, label `fusion`). New function: `tst_calcregistry::dependsOnExplicit` (10.2).
- No existing expectation changes in this phase. If an earlier test fails after all phases are together, the cause is an integration defect, not the expectation.

### Integration Tests
- `audit_cleanup` with the new groups and the new map grammar (10.4, 10.5), including the planted-violation checks.
- Configure-time link confinement with fusion tests on and off (10.6).
- The suite in four configurations (10.11 step 1): default; `FLYSIGHT_FUSION_EXACT=1`; fusion and widget tests off; tests off.

### Manual Verification
- Task 10.11: deployment probe in the installed tree; M1-M9 on a copied logbook.

## Notes for Implementer

### Gotchas
- **The audit's patterns would match documents that quote them.** `tests/audit` is excluded from every search; `PLANS/` is not in any pathspec. Write the new docs so they never contain the banned tokens: `docs/SENSOR_FUSION.md` cannot even say that nothing is called by the three-letter filter name - say nothing; it must not name `posN`-style channels either ("renamed from the branch" is enough).
- `tests/README.md` legitimately contains branch file names inside Phase 8's verbatim harness; that is why the `naming` rules exclude it. Do not "fix" the harness text.
- `QProgressDialog` occurs on exactly two lines of `src` on `master` (`mainwindow.cpp` include and the import dialog). If Phase 7's `closeEvent()` comment mentions the class, the count is 3: reword the comment rather than raising the number.
- `[.>]publish\(` must not match `publishInvalidation(` / `publishEdges(` / `publishCalculationInvalidation(` - it does not, because of the opening parenthesis directly after `publish`; keep it that way when editing the pattern.
- `QThread` in a *comment* outside `jobqueue.*` (for example "any thread" prose is fine, the class name is not) trips `one-worker`; reword the comment.
- Pathspecs like `src/jobqueue.*` are git globs (the script already uses `src/*_bindings.cpp`); a pathspec that matches no file is not an error for `git grep`, so a misspelt path silently passes: test each new rule once with a planted hit.
- The map's `ci` tokens are checked against `.github/workflows/build.yml`, which this phase edits only to delete Boost steps (Task 10.12); choose tokens that Phase 1's workflow hunk really contains (`git grep -n "solver_deploy_probe\|libgtsam\|gtsam.dll" -- .github`) and re-run `audit_cleanup` after Task 10.12. Never use a Boost-related token.
- In `tst_fusion_rows`, never use `asyncdriver.h`'s threaded modes for a real fit (default stacks); all fits go through the `JobQueue`.
- A `jobProgress` slot must disconnect itself (or latch a flag) after acting; later texts of the same job arrive many times.
- `requests.flush()` inside a `jobFinished` slot is allowed (it is what `PlotRequests::continueAfter` itself does there), but do not call `cancel*()` or spin an event loop from it.
- Fixture sessions carry `SCHEMA_VER = 2` (Phase 9); `reject_length`, `reject_too_few_fixes`, `reject_coverage` cannot go into a `SessionModel` (ragged or not useful) - use `reject_origin`.
- Manual verification changes a global preference (logbook folder). Change it first, restore it last.
- Shared files in this phase: `CMakeLists.txt`, `src/CMakeLists.txt`, `tests/CMakeLists.txt`, `README.md`, `docs/CALCULATIONS.md`, `tests/README.md`, `tests/acceptance_map.txt`. No other phase is open, so restructuring (not only appending) is allowed here - and expected for the two READMEs and the map.

### Decisions Made
- **Map scheme `100 + n` with four line forms** (test, `manual`, `ci`, `audit`); every `# SFJ` comment becomes a real line; the audit checks both ranges and that each new item has automated evidence.
- **Acceptance 20 is an audit group, not a test**: a permanent text rule is the only thing that keeps a guard from coming back. The same mechanism carries the structural principles of spec 12 (one worker, no locks, gestures only from the delegate, no view of the queue).
- **GTSAM confinement is checked twice, deliberately:** at configure time on real link closures (authoritative for "links"; sees transitive and variable-borne items; works only where CMake has targets), and as text rules on `#include <gtsam/` and kernel purity (work without a configure, and catch header use that a `PRIVATE` link would otherwise only reveal as a compile error on another platform).
- **User documentation lives in `docs/COMPUTED_PLOTS.md`**, linked from the README's new "User documentation" list; it speaks of plots, never of calculations.
- **`docs/PORT_VALIDATION.md` is not carried over**; its optional real-recording check moves into `SENSOR_FUSION.md` section 8 with the `SCHEMA_VER` caveat.
- **The real-plot row test builds its own `PlotValue` list** (`fusionPlots()`), because `MainWindow::registerBuiltInPlots()` is outside the test library boundary; the audit pins the application's list at seventeen rows so the two cannot drift silently.
- **Real fits are steered by the first `jobProgress` delivery**, not by gates (Phase 9's technique), and only coarse fixtures are used, to keep the run bounded.
- **Acceptance 19 gets an automated non-visual half** (`editsAndVisibilityDuringFit`); pan and zoom remain manual because widgets are outside the harness.
- **The explicit-backed predicate is unified in `CalculationRegistry`** with a stop rule; Phase 9's wider definition is the one kept.
- **Boost is removed entirely**: the two `Boost::boost` link items, `find_package(Boost REQUIRED)`, `cmake/BoostDiscovery.cmake`, the README and CONTRIBUTING prerequisite, and (Task 10.12) the CI install steps. GTSAM is built without Boost (Phase 1, Michael's decision) and Phase 3 removed the application's only Boost use, so nothing needs it. It happens here rather than earlier because phases 1 and 3 run in parallel and each alone leaves a Boost user behind. An audit rule keeps it out. If Phase 1 fell back to a Boost-enabled GTSAM, only the link items go (Task 10.1c's precondition).
- **`FLYSIGHT_BUILD_WIDGET_TESTS` is forwarded by the superbuild** like the other two test options.
- **One workflow edit in this phase: deleting the Boost steps** (Task 10.12), committed separately as `Phase 10: CI (unverified)` per the Commit Policy, so it can be dropped on its own (a workflow that still installs Boost is merely slower). Everything else that needs CI is listed for Michael instead.
- **Earlier phase documents are not edited by this phase;** the amendments are listed under "Cross-phase findings" and in the final report.

### Open Questions
- None blocking. For Michael: whether acceptance 1 should be declared met before the first push (this plan says no: Windows only).

## Definition of Done

This phase is complete when:
1. All twelve tasks have passing acceptance criteria (or Task 10.2 was reverted under its stop rule and reported).
2. All 41 test executables and `audit_cleanup` pass via CTest on Windows Release, in the four configurations of Task 10.11; exact-mode parity shows zero non-identical samples.
3. Every acceptance item 101-120 has lines in `tests/acceptance_map.txt`, every line resolves, and `tests/README.md` section 9.2 matches it; no `# SFJ` text remains.
4. `docs/SENSOR_FUSION.md`, `docs/COMPUTED_PLOTS.md`, `docs/LOCAL_COORDINATES.md`, `docs/CALCULATIONS.md` (sections 1-17), `docs/DATA_SCHEMA.md`, `README.md` and `tests/README.md` are final and mutually linked; none describes a modal dialog, a retry, or a scheduler pause.
5. Code follows patterns established in reference files; no TODOs or placeholder code remains.
6. The manual script M1-M9 was run on a copied logbook and reported; the user's logbook and preferences are untouched.
7. **Spec section 12 checklist**, each with its evidence named in the report:
   - *The algorithm is frozen* - `tst_fusion_parity` / `tst_fusion_kernel` in exact mode; this phase changes no file under `src/fusion/`.
   - *The plot is the request* - `COMPUTED_PLOTS.md`; audit `gestures` ("no jobs window, no view of the queue"); no menu, action, or preference names a calculation (`git grep -n "Sensor fusion" -- src` shows the plot category and the descriptor title only).
   - *Only a gesture starts expensive work* - map items 116; audit `gestures`.
   - *One thread owns all state; no locks* - audit `one-worker`; `tst_fusion_parity::workerThreadMatchesMainThread`.
   - *The engine decides staleness* - map items 108; audit "prepared and published in one place".
   - *A result that is a function of the inputs is cached, including a failure; others never* - map items 109; `tst_calcengine_async::resourceExhaustionIsNotCached`, `tst_jobqueue::resourceExhaustionFails`; audit "no hand-cached failure".
   - *The job model is the single source of truth* - map items 118; `tst_fusion_rows::rollPitchYawShareOneJob` (three rows, one record).
   - *Nothing from the old mechanism is ported for its own sake* - audit `branch-mechanisms`, `naming`.
8. The hand-off list (post-push checks, open questions, amendments F1-F15) was delivered in the final report.
