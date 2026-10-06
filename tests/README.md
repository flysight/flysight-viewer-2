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
12. [Manual verification](#12-manual-verification)
    - 12.1 [Plot-driven jobs](#121-plot-driven-jobs)
    - 12.2 [The fusion runner and the reference recordings](#122-the-fusion-runner-and-the-reference-recordings)
    - 12.3 [Stored results](#123-stored-results)
    - 12.4 [Demand-driven calculations](#124-demand-driven-calculations)
    - 12.5 [Calculation refinements](#125-calculation-refinements)
    - 12.6 [One status bar for background work](#126-one-status-bar-for-background-work)
    - 12.7 [Sensor fusion plots, attitude and the orientation attribute](#127-sensor-fusion-plots-attitude-and-the-orientation-attribute)
    - 12.8 [The fused state at every IMU sample](#128-the-fused-state-at-every-imu-sample)

[Appendix A. The acceptance items (1-19)](#appendix-a-the-acceptance-items-1-19)
[Appendix B. The acceptance items of sensor fusion and plot-driven jobs (101-120)](#appendix-b-the-acceptance-items-of-sensor-fusion-and-plot-driven-jobs-101-120)
[Appendix C. The acceptance items of the sensor fusion improvements (201-247)](#appendix-c-the-acceptance-items-of-the-sensor-fusion-improvements-201-247)
[Appendix D. The acceptance items of stored requested results (301-350)](#appendix-d-the-acceptance-items-of-stored-requested-results-301-350)
[Appendix E. The acceptance items of stored-result validity (401-442)](#appendix-e-the-acceptance-items-of-stored-result-validity-401-442)
[Appendix F. The acceptance items of demand-driven requested calculations (501-563)](#appendix-f-the-acceptance-items-of-demand-driven-requested-calculations-501-563)
[Appendix G. The acceptance items of calculation refinements (601-662)](#appendix-g-the-acceptance-items-of-calculation-refinements-601-662)
[Appendix H. The acceptance items of one status bar for background work (701-754)](#appendix-h-the-acceptance-items-of-one-status-bar-for-background-work-701-754)
[Appendix I. The acceptance items of sensor fusion plots, attitude and the orientation attribute (801-863)](#appendix-i-the-acceptance-items-of-sensor-fusion-plots-attitude-and-the-orientation-attribute-801-863)
[Appendix J. The acceptance items of the fused state at every IMU sample (901-940)](#appendix-j-the-acceptance-items-of-the-fused-state-at-every-imu-sample-901-940)

## 1. What this is

Qt Test executables that link the `flysight_core` static library (session
data, calculations, import/export, logbook, session model, the executor, the
demand layer, registries, unit conversion). They need Qt Core, Gui and Test
and GeographicLib only: no UI, no WebEngine, no KDDockWidgets, no QCustomPlot,
no Boost, and - with three kinds of exception - nothing else:
`tst_python_bridge` embeds Python, `tst_logbook_indicators`,
`tst_status_bar` and `tst_choice_attribute` link Qt Widgets, and the tests labelled `fusion` link GTSAM (through `flysight_fusion`, or
directly). Each of the three can be switched off (section 3). One more CTest
entry, `audit_cleanup`, is not an executable but a CMake script (section 10).

The tests are not a standalone project. `tests/` is added by
`src/CMakeLists.txt` when `FLYSIGHT_BUILD_TESTS=ON` (default `OFF`), and every
test is registered with CTest. Test executables have no install rules, so
packages are the same whether or not the option is set.

There are 54 test executables plus the audit. `ctest -N` lists 55 entries, or
62 where the bit-exact runs of the seven fusion golden tests are registered
(`tst_fusion_*_exact`: the same executables a second time, label `exact`,
Release only; sections 3 and 11). `solver_deploy_probe`,
`fusion_golden_capture` and `fusion_runner` are also built, but are not tests
(see below).

**Harness**

| Test | Covers |
|------|--------|
| `tst_harness` | The test-support code itself: settings and logbook isolation, fixture builders |
| `tst_plot_color` | `plotColor()` (`src/plotutils.h`), the one colour a plot is drawn in: the registry's default without a stored preference, the stored colour when one is valid, the default again for a stored value that is not a colour, and the same answer for the plot, the legend and the measure tool, which all read it there |
| `tst_plot_format` | `formatValue()` and `formatXAxisValue()` (`src/plotutils.h`), the one way a plot's value is written as text in the legend, the analysis tab and the measure tool: converted and rounded by the row's measurement type through the unit converter, the same text a logbook column shows; one decimal unconverted without a type; `--` for NaN; the x axis in seconds as a time. The measurement's name plays no part (a substring test on it once printed the along-track accelerations raw, "lon" being in `accAlongTrack`). The fused accelerations' accuracies have a type of their own, `acceleration_accuracy`: the acceleration's g in both unit systems, at four decimals, so that 0.0046 m/s^2 reads `0.0005` where `acceleration` reads `0.00` (`accelerationAccuracyKeepsItsDigits`; noise-model item 1037). The GNSS acceleration accuracy is of the `acceleration` type: 1.8633 m/s^2 reads `0.19` in both unit systems (`gnssAccelerationAccuracyIsAnAcceleration`; item 1101). The fused speeds carry their GNSS counterparts' types, `speed` and `vertical_speed`, and a value of either type reads as the unit converter's text in both unit systems, never as the untyped one decimal (`speedTypesFormatAsTheConverterDoes`; fused-speed item 1608) |
| `tst_sample_continuity` | The continuity rule (`src/samplecontinuity.h`), the one authority on when the interval between two samples of a sensor is a hole, on hand-built axes: the nominal interval is the median of the successive differences, the mean of the middle two for an even count, read in place from a `QVector` and a `std::vector`; exactly 1.5 nominal intervals is not a hole and the next double above it is; fewer than three samples, and an axis that is not finite and strictly increasing, have no nominal interval and no holes and throw nothing; two holes make three runs that partition the samples. The plot utilities that read a sensor's samples through it (`src/plotutils.h`), on a synthetic session with a GNSS hole and a regular IMU: a graph's points (`graphData()`) with exactly one break, keyed strictly between the two samples around the hole, against `_time` with and without a reference offset and against `_system_time`, and none for the IMU; the legend's and the measure tool's point reads, the Set Ground tool's elevation (`groundElevationAt()`) and the crosshair's reading of a graph (`interpolateGraphAt()`): nothing strictly inside the hole, the sample's value at the samples around it, as before between connected samples and beyond the ends. It compiles `plotutils.cpp` as `tst_plot_format` does (sample-continuity items 1201-1205, 1208) |
| `tst_smoke` | End-to-end characterization of importer, session, calculations, exporter, logbook, and model (started as a pin of v2026.04.1; expectations the rework changed on purpose were rewritten with the change that altered them) |

**Calculation engine (synthetic calculations)**

| Test | Covers |
|------|--------|
| `tst_calcregistry` | Calculation engine: value types, registration order and validation, family instances, private registries, registration-derived static dependencies (followed through source conversions), `explicitDependencies()` (which explicit calculations stand behind a name) and its non-emptiness `dependsOnExplicit()`, the one authority for "explicit-backed", source inputs refused outside source conversions |
| `tst_calcengine` | Calculation engine: resolution, caching, dependency recording, invalidation across sessions, explicit policy, preferences, families, measurement layers |
| `tst_calcengine_safety` | Calculation engine: nested scopes, cycles (single and overlapping rings: the same literal answers for every read order), exceptions, re-entrancy guards |
| `tst_calcengine_oracle` | Calculation engine: randomized (seeded) sequences, and randomized (seeded) topologies full of overlapping rings, compared against a fresh evaluation; the same with explicit calculations in play (blocker inspection, synchronous and asynchronous requests, requests refused because an input changed, registry changes of candidates they look up), where an explicit calculation may run only in a request or publish step; which cached answers a registry change keeps (a candidate behind the provider or behind the session's own data, a removed candidate that was passed over or never tried) and which it drops (a provider removed, a new candidate for a name that resolved to nothing, a change of the conversion layer under recorded data), a kept answer served without a run (`registryChangePrecision`) |
| `tst_calcengine_async` | Calculation engine: the asynchronous request (prepare / compute / publish) with compute inline, on a `std::thread` and on a `QThread`: identical to `request()` in every observable, captured inputs, engine-decided staleness (an input changing while compute is running, transitive changes, registration removed, engine destroyed, a synchronous request in between), `willBeRefused()` / `refusalReason()` reporting the engine's mark for every cause of a refusal that can be known before publish (false for a healthy ticket, false again once published or refused), cancellation, progress text, resource exhaustion never cached, an ordinary exception cached as `Failed`, abandoned tickets leaving nothing behind |
| `tst_calcengine_blockers` | Calculation engine: blocker inspection: an explicit calculation reported through on-demand intermediates, missing input is never a blocker, chained explicit calculations (A then B), "ran and did not produce" with the reason or failure text, fallbacks, stored and unknown names, an explicit family instance, rings, `readiness()`; inspection never runs an explicit calculation and never changes a later read |
| `tst_calcengine_restore` | Calculation engine: stored results. Snapshot export (the leaf closure through on-demand and explicit results, absent leaves included, the fingerprint's canonical forms and a known answer), restore into a fresh engine (identical edges, status, detail, blockers and invalidation; no run, no ticket), the resolutions (what provided each name the result looked up, their pinned codes and order; a result that met a dependency ring is never exported), every stale check (result version, bundle, inputs unavailable, resolutions, leaves, fingerprint), restore across registries (the same or unrelated registrations, a losing candidate and a family-instance provider (`<family>#<key>`, the instance's result version) restore; a new winning candidate with the same inputs, a provider's or a family instance's result version, a source conversion, or a provider removed between runs is stale), and the explicit-result listener (installed on both request paths for every status, never on restore; dropped by input changes and by registry changes made while the application runs that alter what a name it looked up resolves to (a provider removed, a new candidate for a name that resolved to nothing, a source conversion over recorded data) or, for a result with a stored copy, that remove a passed-over candidate through which alone its lookups reached something (a requested MissingInput result is kept); never by a candidate behind the session's data or behind the provider, unrelated registrations, a teardown removal, `clear()`, or the destruction of the registry or the engine), and that a runtime registry change drops a result if and only if restoring its snapshot across the same change is not a restore, with the one documented conservative exception (`registryChangeMirrorsRestore`), also for a result published from a ticket that was outstanding during the change (`publishAcrossPassedOverRemovalMirrorsRestore`) |

**Built-in calculations and sessions on the engine**

| Test | Covers |
|------|--------|
| `tst_builtins_golden` | Every built-in calculation read through `SessionData` on the generated descent fixture, against hand-derived golden literals |
| `tst_builtins_engine` | The built-ins on a private registry and `FakeSessionState`: golden values, registration inventory, declared inputs only, multi-output groups, candidate order, the declared preference, interpolation family, altitude descriptor, the constant defaults (`constantDefaults`, the Compute attribute's `on` among them: background-computation items 1501, 1502), the column environment digest: per set of names, changed only by what their static closure reaches (`digestChanges`), covering every candidate's result version (`digestCoversResultVersions`) and the conversion layer (`digestCoversConversionLayer`), the same for an altitude marker registered at run time or at the next start and unchanged for every other name (`digestSurvivesRuntimeAltitudeMarker`), and the altitude-marker manager's destructor removing its registrations as teardown, which reports no drop while a marker removed at run time does (`altitudeMarkerTeardownReportsNothing`) (fusion-plots items 810, 834-836, 838, 846, 851, 860); the GNSS acceleration accuracy, bit for bit against its formula with the ends included (`accelerationAccuracyKnownAnswers`), and unavailable without the speed accuracy or the time, with one sample and with lengths that differ (`accelerationAccuracyUnavailable`) (item 1101); a hole in the samples: the accelerations and their accuracy NaN exactly where the stencil spans it, an interior, the first and the last interval, and the formula bit for bit elsewhere (`accelerationAcrossHole`), a measurement at a marker unavailable strictly inside a hole and the sample's at its two samples (`interpolationInsideHole`), and the automatic ground elevation likewise, the answers without a hole outside the samples (`groundElevationInsideHole`); a crossing time across a hole: an altitude marker crossed in a hole or beside one at the linearly interpolated time (`altitudeCrossingAcrossHole`), and the exit there finite, its acceleration the crossing interval's slope, one under the 2.5 m/s^2 gate passed over (`exitCrossingBesideHole`) (sample-continuity items 1207-1210) |
| `tst_time_fit` | The system-time-to-UTC fit: microsecond-level conversion of an exact synthetic clock at high device uptime (the regression test of the centered sums), invalidation through the TIME sensor, GPS week rollover, degenerate clocks |
| `tst_local_coordinates` | The recording-wide `Local` frame: origin gates, analytically known displacements and velocity rotation on WGS84, NaN at the index of an invalid sample only, all outputs unavailable without a qualifying fix, the shared GNSS time axes, invalidation on source changes and independence from markers on a real `SessionData` |
| `tst_simplified_track` | The simplified map track on the shared `Local` frame: all seven outputs at the same retained sample indices, every dropped sample within 0.5 m of the path and the strictly-greater rule, duplicate-position endpoints, closed, degenerate and empty tracks, non-finite samples left out, one projection per recording, unavailable without a local origin and back after a source correction, siblings invalidated together, and each run of connected samples simplified on its own across a hole in `GNSS/time`, which keeps the two samples around it (`eachRunSimplifiedOnItsOwn`; item 1206) |
| `tst_session_engine` | `SessionData` on the engine with the real built-ins: run-once, invalidation, candidate replacement, overrides, preferences, the fresh-evaluation oracle, copy/move semantics; an explicit-policy calculation and a throwing / nested / cyclic set of calculations registered temporarily on the global registry (acceptance 14, 12); an asynchronous request against real session ownership (published, session moved, destroyed, move- and copy-assigned over, declared input edited) |
| `tst_session_model_engine` | `SessionModel` + `AltitudeMarkerManager`: registry and preference broadcasts reaching `dependencyChanged`, coalescing, merges, rows surviving sort, the read-only `DEVICE_ID` column, a marker only for an altitude whose calculation registered; the executor's hooks: counted session pins that defer LRU eviction (and nothing else), the idle scheduler's `unregisterTask` and replacing `registerTask` (`schedulerTaskCanBeUnregistered`) and a task with work it cannot step, which the scheduler waits on without spinning (`schedulerWaitingTaskDoesNotSpin`); a task that can wait and loses its work without a step is completed once, not cancelled, after a final progress report and before the next active task or idle, while a task without `canStep` is not (`schedulerCompletesWaitingTaskWhoseWorkIsGone`); the read-only `hasWork()`, true while some task has work, whether or not it can step, the fill's kind included, and neither ticking nor waking (`schedulerHasWorkFollowsItsTasks`; status-bar items 706, 739); a bulk edit publishing its edit as a dependency change on both of its paths (`bulkEditAnnouncesADependencyChange`), and immediate publication of engine-returned invalidations without any persistent effect |
| `tst_result_store` | Stored results of explicit calculations on a real model and logbook (synthetic calculations): written on an Ok install (also before a new session's first save), deleted on an input change, restored on every load path, upstream records first, so that a record whose lookup has a fallback candidate restores in any id order (a session without a known record is not listed), stale records deleted; the bulk edit's temporary load never reads one, the column worker's copy only when a missing column needs it (then with the checks of a load); write failures, each announced with its reason after the record change of the pair (`writeFailureIsAnnounced`); an explicit family instance is not stored; a registry change made while the application runs that changes what a name a result looked up resolves to deletes its record, a candidate registered behind the provider and a teardown removal do not; unrelated registrations and the descent-pause preference keep it valid across loads and restarts; a lookup that resolves differently, or a changed plug-in code identity of a calculation the result read, makes it stale; an unreadable record (a directory at its path, a Windows lock, POSIX permissions; rows skip where the platform does not honour them) is skipped and restored at a later load, a record that reads it is kept, the next publish replaces it (the column value over it then reaches `index.json`), and deleting its session removes it, leaves a locked file as a stray for the next start, or leaves a directory (never a record); a format-1 record is deleted; the reason of a stored rejection recorded in `index.json` at the write and at every restore, and a changed reason announced as a record change (`recordReasonRecordedAtWriteAndRestore`) |
| `tst_result_columns` | Logbook columns over explicit results: cached from the restored or published result with a record stamp in index.json, dropped when a record is written or deleted, filled for an unloaded session with a record by the column worker from its stored results restored into a temporary copy (the row never loaded, no record written, nothing run; a stale record deleted and the value cached unavailable; an unreadable one skipped and the value pending; no record read while the missing columns are all on demand), crash points, old indexes, write failures, and a `cache/` folder deleted while the application was closed (no record, nothing requested, the values over it dropped at start-up, nothing run); values over a record skipped at a load never cached while it is skipped; a registry change that does not reach a result (also a provider registered behind a stored input) keeps its row confirmed, one that does deletes the record and its stamp (`registryChangeKeepsLoadedRowConfirmed`); an environment change discards cached values while the record restores; the column worker's behaviour and statistics unchanged with column demand active, and a stale record it deletes moving the pair into demand (`columnWorkerIsUnchangedByDemand`, `staleRecordDeletedByWorkerCreatesDemand`); the session model's column knowledge - each enabled column's requested calculations and closure, current before the queued environment check (`sessionModelExposesColumnKnowledge`) - and a row's display name for loaded rows, stubs and failed-load placeholders (`sessionDisplayNameOfEveryRowKind`) (fusion-reconstruction item 922) |
| `tst_session_oracle` | The session-level idempotency oracle (section 7): randomized, seeded sequences of reads, edits, merges, preference and registry changes on real `SessionData` objects (part A) and on the real `SessionModel` / `LogbookManager` through the application's import path, with restarts, simulated crashes and a persisted-state check (part B), compared against a fresh evaluation |

**The executor, the demand layer and the demand views** (synthetic explicit calculations; no GTSAM)

| Test | Covers |
|------|--------|
| `tst_jobqueue` | The executor, `JobQueue`, on a real `SessionModel`, real session engines and the global registry, with the synthetic explicit calculations of `jobfixture.h` (no GTSAM): publication through the session model, the 64 MiB worker thread at below-normal priority (`workerRunsBelowNormalPriority`), the main thread free while a job computes (`mainThreadIsNotBlockedByARunningJob`), at most the running job and one chosen next job (`holdsAtMostRunningAndChosenNext`), an equal offer creating nothing, a different offer replacing the chosen next job, which ends Cancelled "No longer needed" (`offerReplacesChosenNext`), withdrawing it (`withdrawEndsChosenNext`), one job at a time in offer order, refusals (missing input, unloaded or unknown session, blocked, done, unknown), never loading a session, every superseded / succeeded / failed / cancelled path with its reason text, a running job whose ticket went stale (input edit, merge, registration removed, model destroyed, the row's session data replaced - "Session data replaced", not "removed or unloaded") asked to stop at once and ended Superseded with the refusal's reason without its compute reaching the end, a new offer behind it becoming the chosen next job and run with the new inputs, first writer wins between a user cancel and a stale stop, the end of a cancelled job carrying the caller's reason (`cancelRunningThenNextStarts`, `cancelQueued`), cancel wins over a completed compute, a job cancelled from a `rowsInserted` slot (no pin left), session removal, deferred eviction, repopulation, merge, sort, shutdown in every order, the idle scheduler working during a job, and the job record naming its session by the session model's display name (`runsAndPublishes`) (sensor-fusion-jobs acceptance 8, 10, 11, 14, 17; demand items 521, 540, 544, 560, 561; refinement items 606, 624, 628, 629; background-computation item 1509) |
| `tst_jobmodel` | The `JobModel` contract under `QAbstractItemModelTester`: every role on every column, a test view that renders the whole job history from model signals alone, never more than one running row, a replaced chosen next job recorded as Cancelled "No longer needed", ordered UTC timestamps, progress and cancel-requested as their own signals, removal of finished rows only, the retention bound, and no job persisted: the settings and the logbook folder are byte-identical after jobs of every ending, except for the stored results of the two jobs that published `Ok` (sensor-fusion-jobs acceptance 18; store-requested item 309) |
| `tst_calculation_demand` | `CalculationDemand`, the widget-free demand layer, on a real `PlotModel`, executor, `SessionModel`, `LogbookColumnStore`, logbook, real session engines and the global registry, with the synthetic plots of `plotfixture.h` (no widgets, no GTSAM). **Plot demand:** checking a plot starts the visible sessions without a result, showing a session or loading a visible one starts it, hiding drops its waiting pair and unchecking all of them while the running job finishes and is stored (`rowScript`, `showingASessionStartsIt`, `hidingASessionDropsItsWaitingPair`, `uncheckingDropsWaitingPairsKeepsRunning`); a waiting pair whose result appears by other means dropped before it starts (`resultAppearingWhileWaitingDropsThePair`); a click, `setPlotEnabled`, `togglePlot`, `setData` and a profile (applied by `PlotModel::setEnabledPlotIds()`, as `applyProfile()` does) create the same demand, the start-up restore with every session hidden none; a profile naming plots the model does not have enables its others and ignores those, silently: no message of any type, no row added (`profileNamingRemovedPlotsAppliesWithoutThem`); chained calculations upstream first, the executor never idle between links; the focused session, then row order, the chosen next job replaced when demand changes, never more than the running and one chosen next job; the one-second input-settle wait (a burst runs one job, counted from the first change); failures (an input-determined one stored, listed and never re-run; a job-level one listed, not re-run in the run and re-run by a new demand layer, as after a restart); not-applicable sessions never listed; progress, failures and their change signals (`changeSignalsAreMinimal`), coalesced passes, `isMerelyUncomputed()` (the plot widget's "No data available" warning is withheld for a value that waits on a requested calculation or was rejected by one, and for nothing else); session removal, registry changes, executor shutdown and null collaborators. **Column demand:** enabling a column over a requested output fills every session of the logbook, loading sessions that are not loaded at most two at a time as hidden, pinned sessions that leave by ordinary eviction (`enablingColumnFillsEveryUnloadedSession`), a session shown meanwhile running next, stored results creating no job (a stored rejection of a session that is not loaded listed with its reason after a restart, without a load), one pair memory (a refused offer or a column's not-applicable verdict; a failed job, load or record write; an exception result) that keeps a session from being loaded or a pair from being offered again after eviction, a sort or the column worker's pass (`settledPairsSurviveEvictionSortAndColumnWorker`), cleared by a record change, an input change or a registry change (`pairMemoryIsClearedByRecordInputAndRegistryChanges`), a column's verdict for that column only (`columnVerdictDoesNotSuppressAnotherColumn`), a column pair the executor refuses at offer time remembered and followed by a pass so that its cell does not stay pending (`columnOfferRefusalIsNotLeftPending`), chains keeping their hold, holds released on disable, show, removal, repopulation and destruction, the identity stub offered under its real id, the column's progress and pending cells (`columnProgressAndPendingCells`), the load step below saves, bulk edits and column work, not cancellable and reporting no progress of its own (`fillTaskRestsWhileWaiting`), and a pass over 2000 stubs reading each record set once. **Refinements:** one walk for plots and columns, a column track running on one blocker filing its others (`runningColumnTrackFilesItsOtherBlockers`); a record that could not be written listed among the failures, for a column and for a plot alike, not retried in the run, cleared by a later write, tried again after a restart (`failedRecordWriteIsShownAndNotRetried`, `failedRecordWriteIsShownOnThePlotRow`); a reason learned by the column worker's restore reaching the demand layer through the record change alone (`recordReasonReachesDemandThroughRecordChange`); column knowledge from the session model (`columnKnowledgeComesFromTheSessionModel`); the choice acting on the executor's answer and withdrawing a chosen next job nothing wants (`chosenNextJobFollowsTheExecutorsAnswer`); the settle clock (`settleClockAnswersItsQuestions`); the fill completed by the scheduler, a new burst of computations after a fill that ends behind another task starting its own high-water mark (`fillEndingBehindAnotherTaskStartsNextCountFresh`), holds released at the executor's shutdown; a successful load forgetting a session's failed-load facts (`successfulLoadForgetsFailedLoadFacts`). **Progress and failures:** the sessions with a waiting or running track counted once across plots and columns, a stub with a waiting cell included, out of a high-water mark that holds until the count is 0, and the running recording with its step (`progressCountsEachSessionOnce`); the step changing without a pass (`progressTextWithoutAPass`); one entry per recording in row order, each pair once, with its title, its reason and whether the next start tries it again (`failuresAreOnePerSessionInRowOrder`, `failuresNameEachCalculationOnce`); a stored rejection a failure of a new demand layer without a load, an unstored one only once its retry fails again (`storedRejectionIsAFailureWithoutLoad`, `unstoredFailureReturnsOnlyWhenItFailsAgain`); failures clearing as the pair memory clears and following what is switched on (`failuresClearAsThePairMemoryClears`, `failuresFollowWhatIsSwitchedOn`); the pending cells' own announcement per column (`pendingCellsChangedPerColumn`); the one text form of the failures and its limit of ten (`failureTextAndItsLimit`). **Background computation:** a recording whose Compute attribute reads `off` is not computed for a plot checked while it is visible, when shown while a plot is checked, or for an enabled column, never loaded, held or counted, its cell excluded and not pending, and its missing plot value still merely uncomputed, so the plot does not warn (`excludedSessionIsNotComputed`); switching it off while its job runs ends the job Cancelled "Switched off for this recording" before control returns, publishing and storing nothing, its hold released and the next candidate run, the running job described by nothing (`switchingOffCancelsTheRunningJob`), while it is the chosen next job withdraws it "No longer needed" (`switchingOffWithdrawsTheChosenNextJob`), after its plot was unchecked still stops the job kept running, though no row is walked (`switchingOffStopsAJobKeptAfterUnchecking`), and while it settles leaves nothing waiting (`switchingOffWhileSettlingLeavesNothingWaiting`); switching it on computes its missing results only, a stopped job from its start, and a remembered job-level failure offers nothing and is listed again (`switchingOnCreatesDemandForItsMissingResults`); a stored result of a recording switched off fills its column without a load and is restored and drawn when it is loaded, its stored rejection not a failure until it is switched on by a bulk edit of a stub (`excludedSessionWithStoredResultServes`); setting the attribute invalidates nothing, starts no settle wait, makes no job stale and runs one pass (`settingComputeInvalidatesNothing`); the fill never loads a stub switched off by a bulk edit (`fillNeverLoadsAnExcludedSession`), and loads one the index does not know is off once, never computing it, the load teaching the index (`offOnDiskButAbsentFromIndexIsLoadedOnce`); the Compute column through `ChoiceFixture` on loaded rows and stubs: the definition and the header's tooltip, "On" shown without a write (`computeColumnShowsTheDefaultWithoutAWrite`), an edit storing a token verbatim and refusing others (`computeEditStoresATokenAndRefusesOthers`), a bulk edit (`computeBulkEdit`), a hand-edited token shown as written and computed (`handEditedTokenIsShownAsWrittenAndReadAsOn`); a recording imported while the import preference reads `off` carrying the line and excluded at once (`importedWhilePreferenceOffIsExcludedAtOnce`) (sensor-fusion-jobs acceptance 11, 13, 15, 16; demand items 501-560; refinement items 601-604, 607, 609, 611, 612, 615, 616, 618-620, 622, 624-627, 629-643, 645, 647, 648, 651-659; status-bar items 703-706, 711, 713-717, 720, 727-732, 736, 740, 744, 745, 748-750, 753; fusion-plots items 805, 845, 852, 855; background-computation items 1501-1507, 1509-1521) |
| `tst_logbook_indicators` | `LogbookView` with `LogbookCellDelegate` on the tree's own header, in an offscreen window beside a reference `QTreeView` with the base delegate, on a real demand layer, executor and `SessionModel`; the first of the three tests that link Qt Widgets (`FLYSIGHT_BUILD_WIDGET_TESTS`, label `widgets`). Without a failure the header and every cell that is not pending are the reference's, with the same sizes, while a requested column works and once it has finished (`plainHeaderAndCellsAreIdenticalToBase`); the header is the tree's own `QHeaderView`, identical to the reference's and reserving no room while a column works and after one has failed, nothing repaints by itself while a column works, and the view has no progress bar or cancel button (`headerIsPlainAndNothingAnimates`); a recording with a current failure shows one glyph, the style's warning icon, right after the text of its row's first visual cell, attached to it and leaving the text where it was, for a loaded row and for one that stays unloaded, every other cell (the failed calculation's blank one included) the base delegate's and every size unchanged (`rowWarningFollowsTheText`); the hover over the glyph is exactly `SessionFailures::text()`, the rest of a pending first cell keeping the pending tooltip (`rowWarningHoverIsTheSessionsFailures`); the glyph in the new first visual cell after a section is moved to the front or hidden, a sort and a rebuild of the columns (`rowWarningFollowsTheFirstVisualColumn`); the glyph appearing when a job fails and going after an input change whose retry succeeds and when the last source is disabled (`rowWarningFollowsFailures`); a session file that cannot be loaded warning on its row, its cell not pending (`failedLoadSessionShowsRowWarningNotPending`); a record that could not be written named in the row's hover, tried again at the next start (`failedWriteIsListedInTheHover`); a click on the glyph selecting as a click elsewhere in the cell, starting and cancelling nothing (`clickOnRowWarningIsAClickOnTheCell`); pending cells distinct from unavailable ones and from the unreadable-record state, never in the model or `index.json`, sorted as unavailable, and replaced by the value when the record is written; `pendingCellsChanged` repainting that column and `failuresChanged` the first visual column, with no model signal (`pendingCellsChangeRepaintsOnlyThatColumn`); a demand layer destroyed first, with a glyph and pending cells shown (`survivesDemandDestroyedFirst`); a cell of a recording switched off reading "excluded", muted, distinct from the base delegate's and from a pending cell, with its own tooltip and nothing of it in the model or `index.json` (`excludedCellIsDistinctFromPendingAndUnavailable`), a value always winning (`excludedCellShowsItsValue`), sorted as unavailable (`sortingTreatsExcludedAsUnavailable`), no row warning for a recording switched off until it is switched on (`excludedSessionShowsNoRowWarning`), and a change of the excluded cells announced and repainted for its column only (`excludedCellsChangeRepaintsOnlyThatColumn`) (demand items 526, 527, 533-538, 543, 547, 557, 558; refinement items 618, 633, 644, 646, 648-650, 652, 660; status-bar items 718-726, 731, 735-737, 741, 744, 750-752; background-computation items 1507, 1508, 1516) |
| `tst_status_bar` | `StatusBarFeature` in the status bar of an offscreen `QMainWindow`, on a real `SessionModel` with its idle scheduler, executor and demand layer, with the synthetic calculations of `jobfixture.h` and the plots of `plotfixture.h`; the second of the three tests that link Qt Widgets (label `widgets`). The scheduler's four tasks under their labels, with the scheduler's count for that id on the label and the bar, a report for another id changing nothing, and idle leaving the activity area empty (`schedulerTasksShowTheirLabelsAndCounts`); the computations as one item, counting the sessions of plots and columns together, each once, out of the high-water mark, shown while the fill is the active task, the fill never reporting a progress of its own, and a later burst starting its own total (`computationsAreOneItem`); a task shown over the computations, the hover listing both with the recording being computed and its step, and the computations returning when the task ends (`taskShownOverComputationsAndHoverListsBoth`); the cancel button exactly while the shown item is a cancellable task, never for saving, the fill or the computations, and a click cancelling a visible load (`cancelOnlyForACancellableShownTask`); the warning beside the computations and then alone, counting recordings with the style's warning icon, its hover the capped list of `SessionFailures::listText()` (`warningBesideComputationsThenAlone`, `warningCountsRecordingsAndListsThem`, `warningListsAtMostTenRecordings`), absent without a failure, not dismissed by a click and gone with the last source (`warningAbsentWhenNothingFailedAndNotDismissable`), and after a restart counting a stored rejection without a load, also from an index without "recordReasons", and an unstored failure only once its retry fails again (`warningAfterRestart`); one height idle, with a task, the cancel button, the warning and both (`heightNeverChanges`); the activity at the left, label then a compact bar then the cancel button, the warning at the right, and no width kept by a hidden widget (`activityLeftAndWarningRight`); a demand layer destroyed first, and none at all (`survivesDemandDestroyedFirst`) (demand items 530, 534-537, 539, 559; refinement items 612, 644, 645, 647, 650, 653, 660; status-bar items 701-705, 707-716, 725, 727, 733, 734, 737-739, 745-750; background-computation item 1511) |
| `tst_choice_attribute` | The Choice attribute format type (`AttributeFormatType::Choice`) through the logbook, on a real `SessionModel` and logbook, with Choice attributes the test registers (three choices whose label, token and definition orders all differ, one token holding a comma; one attribute with a no-input default calculation, one without) and driven through `ChoiceFixture` (`support/choicefixture.h`); the third of the three tests that link Qt Widgets (label `widgets`), with a `LogbookView` and no demand layer. On loaded rows and on stubs after a restart: a cell shows the label of the effective token, stored or calculated, and a token outside the list (planted with `updateAttribute()`) its raw text (`choiceShowsTheLabelOfTheEffectiveToken`); sorting orders by that text, missing values last both ways (`choiceSortsByLabel`); `setData()` stores a token of the list verbatim (`$VAR,<key>,b,1`), shows its label and publishes the change, refuses the stored token again, and compares with the stored value, so the default's own token can be pinned (`choiceEditStoresAToken`); a token outside the list, a label, an empty string, an invalid value or another type is refused with nothing emitted, saved or loaded (`choiceEditRefusesATokenOutsideTheList`); the bulk edit sets a token on loaded and stub rows, files, `index.json` and cells agreeing (`bulkEditSetsAToken`) and queues nothing for any other value, an invalid one included, the bulk edit task never active (`bulkEditRefusesATokenOutsideTheList`); the in-place editor is a non-editable `QComboBox` of the labels in definition order, opening on the cell's label or on no entry for a raw token, a label storing its token and Enter on the opening entry writing nothing, a Text cell keeping the base class's `QLineEdit` (`cellEditorOffersTheList`); the "Set ..." dialog, `LogbookView::askAndSetAttribute()` driven through its modal `QInputDialog` by a timer, is a non-editable list of the same entries opening on the first label, a label bulk-editing its token onto the given sessions and a rejection changing nothing, a Text attribute keeping the text prompt (`setDialogOffersTheList`) (fusion-plots items 823-833, 841, 844, 858, 859; background-computation items 1504, 1519) |

**Source layer, conversion layer, importer**

| Test | Covers |
|------|--------|
| `tst_schema_units` | The two tables behind the conversion layer: the schema table (`SCHEMA_VER` validation, which measurements each version corrects) and the unit normalization table (silent, identity for unknown text) |
| `tst_sensor_configuration` | The sensor configuration vocabulary (`src/sensorconfiguration.h`), every expectation a literal: exactly nine keys with the unit in the name and no filter key, every listed value of every listed key accepted, also with surrounding whitespace, and a plain positive decimal for the four free-form keys (`keysAndValueForms`); the malformed values, among them `16.0`, `+16`, `016`, the empty value, `abc`, `1e2`, `nan`, `inf`, `0`, `-1`, `Portable` and `1.6` for the gyro rate (`malformedValues`); the messages for a listed key, `GNSS_MODEL` and a free-form key (`unsupportedMessage`); the default of the four IMU keys, valid values of their keys, firmware `v2023.09.22`, and no default for the other five (`defaultIsTheFirmwareConfiguration`) (noise-model items 1001, 1003-1005) |
| `tst_conversion_engine` | The conversion families on a private registry and `FakeSessionState`: legacy gyro correction, `SCHEMA_VER` 1 / 2 / absent / unsupported, unit normalization, schema-then-unit order, buffer sharing for identity conversions, the dependencies that make the choice follow the attribute |
| `tst_importer` | `DataImporter`: data stored exactly as recorded, nothing stamped, `SCHEMA_VER` and structural errors rejected without touching the target session, `$VAR` values kept verbatim, malformed rows skipped with one summary warning, FS1, custom columns, CRLF; `parseFile` carries nothing the file did not say (match id synthesized from the bytes but not stored), `applyCreationDefaults` is the one writer of import-time defaults and only fills absent keys, header-only `peekHeaderAttribute`; the orientation of the Import preference stored into a new recording, a file's own kept, nothing written without a preference (`orientationFromThePreference`); a recording switched off for background computation while the Import preference reads exactly `off`, a file's own line kept, nothing written for `on`, an empty or any other value (`computeFromThePreference`; background-computation items 1505, 1520); the nine sensor configuration keys of `SENSOR.CSV` and `TRACK.CSV` stored as recorded, whitespace kept (`configurationStoredAsRecorded`), a malformed value in either file rejected with the vocabulary's message after `SCHEMA_VER`'s, the first in file order, the target untouched (`rejectsMalformedConfiguration`), and nothing written for an absent key (`neverStampsConfiguration`) (noise-model items 1002-1004, 1006, 1050) |
| `tst_source_layer` | Session-level acceptance for the source / effective split on real `SessionData`, importer, exporter, logbook and model: acceptance 1, 2, 4, 6 (load), 16; enumeration and source access never compute; lazy conversion; buffer sharing; exporter and merge use the source layer; only `SCHEMA_VER` decides (not the firmware version, the file name, or the recording date) |

**Persistence and the logbook column cache**

| Test | Covers |
|------|--------|
| `tst_csvformat` | `CsvFormat`, the one definition of the on-disk text forms: shortest round-trip doubles (a 200 000-value bit-pattern sweep), `-0`, `nan` / `inf` / `-inf`, attribute values by `QVariant` type, line-break flattening, valid names and units |
| `tst_persistence_roundtrip` | Save / reload on the real importer, exporter and logbook: acceptance 5 (bit-identical samples, units and header attributes preserved, `SCHEMA_VER` only if recorded, effective values unchanged, second cycle byte-identical, independent of any cache) and acceptance 6 (a released logbook file is not rescaled, relabelled or stamped by a save; the `loadSession` backfill is additive and idempotent (mass and area; wind is not backfilled)); non-finite samples, ragged sensors, unrepresentable text; the file writer and the in-memory writer agree (also across the 4 MB flush boundary); an unsupported stored `SCHEMA_VER` is never written, nor a malformed stored sensor configuration value, with the importer's message and the previous file intact (`malformedConfigurationIsNotSaved`); the nine configuration keys written back as stored and reloaded byte for byte (`configurationAttributesRoundTrip`) (noise-model items 1002, 1003) |
| `tst_logbook_index` | `LogbookManager`'s `index.json` column cache (and each session's `"recordReasons"`, `recordReasonsRoundTrip`; a changed reason announced as a record change, an unchanged one silent, `recordReasonChangeIsAnnounced`): the calculation-compatibility marker gates every cached value and each column's recorded environment the values of that column (acceptance 18 at the storage level; `differentColumnEnvironmentDiscardsThatColumn`, `missingColumnEnvironmentsDiscardOnce`, `environmentIsTheCachedOne`), unsaved-column tracking and save ordering (an interrupted save never leaves a cached column that disagrees with the session file), orphan session files adopted, marks follow remap / remove / reset; the raw load with its failure reason, the legacy backfill as a separate step, identity-entry queries, a legacy flat index coming up as stubs without rewriting a session file; each session's `"computeOff"`, learned by a save and a load, written for an off entry only, read back at a restart, carried by a remap, dropped by a removal and a reset, absent in a legacy index (`computeOffRoundTrip`; background-computation items 1512, 1518) |
| `tst_result_records` | Stored requested-calculation results: the record file name (percent-encoded calculation id, canonical, dot-free, distinct under case folding; the parse of a name), the code stamp (the compatibility marker) computed fresh, never made stale by a registration, the binary record format (bit-exact round trip of `-0`, NaN payloads, infinities and subnormals, null / empty / non-ASCII strings and unavailable outputs; a round trip of every accepted attribute type; the pinned byte layout of format version 2, the resolutions included; other format versions (format 1, in both of its layouts, included), damaged and crafted payloads refused without allocating; every other attribute type, `long` and `unsigned long` included, refused at encode; size), and `LogbookManager`'s record files in the logbook's `cache/` folder: a short read is `Unreadable`, never decoded (`readWholeDevice()` on a buffer that holds less than expected: no file system gives a short read on demand); write, read, replace, list, remove, the folder created by the first write only (a missing folder holds no record), write failures leaving the previous record intact (a `cache/` that cannot be created included), removal with the session (dotted identity stems), stray records removed from `cache/` by `initialize()` in all three index branches, `sessions/` untouched (a name spelling the extension in another case is not a record: neither listed nor removed), orphan adoption, remap, and a session save that never depends on records (fusion-reconstruction item 920; background-computation items 1503, 1517) |
| `tst_column_cache` | The same through `SessionModel`: upgrade discards and lazily recomputes (acceptance 18), an edit refreshes only the affected columns with a warm and a cold engine, merges and bulk edits, interrupted saves, environment changes discarding, in loaded and unloaded rows and without saving, exactly the columns whose environment they change: a declared preference only the columns that read it (`preferenceChangeDiscardsOnlyReadingColumns`), a registration only the columns whose closure it reaches (`environmentCheckDropsExactlyTheReachedColumn`), new altitude markers no column at all, at run time and after a restart (`altitudeMarkerChangeKeepsOtherColumns`), and a value computed after a change but before the queued check (an eviction) stored and flushed only under its column's new environment (`valueComputedBeforeCheckIsStoredUnderItsEnvironment`), save failures (the row stays dirty and loaded, is skipped by the idle saver and the LRU, stays out of the index, and is saved by a later edit or the shutdown flush), line breaks flattened at edit, a column over an explicit result cached from the result the session has and following its record, shown by a stub after a restart without a load (`explicitBackedColumnFollowsItsResult`), a plug-in edit (a changed plug-in code identity) discarding the cached values of the plug-in column at the next start and keeping the others (`pluginEditDiscardsPluginColumns`), and `loadPinnedSession()`: a hidden session loaded the way showing it would, pinned under its corrected id, nothing pinned for a file that cannot be loaded (`loadPinnedSessionLoadsWithoutShowing`, `loadPinnedSessionFollowsIdentityRemap`, `loadPinnedSessionFailedLoadPinsNothing`) |

**Import and merge, workflows**

| Test | Covers |
|------|--------|
| `tst_session_merge` | `SessionMerge`, the pure plan-then-apply merge (attribute conflict rule, measurement merge) on programmatic sessions: absent attributes added, equal ones ignored, different header attributes conflict (all reported, sorted, with the delete-and-re-import hint), `_` attributes keep the session's value, the `n/a` device placeholder counts as absent, equality on the on-disk text form, columns replaced / added / kept with samples and unit together, bitwise column comparison (NaN, `-0`), the ragged rule, purity of `plan()`, the invalidation set of `apply()`; the sensor configuration keys of the two files merge in either order and a differing value conflicts with the replace-session hint (`configurationFollowsConflictRule`, noise-model item 1002) |
| `tst_import_merge` | The import path (`SessionImport::importFiles` -> `SessionModel::mergeSessions`) against a temporary logbook: acceptance 3 (a rejected file leaves the session untouched), 7 (TRACK/SENSOR order independence loaded, unloaded and in one batch; conflicts change nothing; edits and unmatched measurements survive), 8 (the `SCHEMA_VER` escape hatch), 10 and 18 (merge parts); defaults only at creation, failed loads are errors, failed-load placeholders are never saved, identity stubs are matched, identical re-imports are no-ops; the sensor configuration keys of `SENSOR.CSV` and `TRACK.CSV` end on the session in either order and are saved once each (`configurationTravelsWithTheSession`, noise-model item 1002) |
| `tst_import_batch` | `SessionImport`: one result per file in input order with parse failures included, cancellation through the progress callback, and the text of the import-failure dialog with each file's error |
| `tst_workflow` | End-to-end workflows on the application's own code path (acceptance 19): import through `SessionImport::importFiles`, rows and columns, marker and attribute edits, save, reopen as stubs served from `index.json`; the model's warm save equals a cold export (acceptance 5); a released session file together with a released `index.json` (acceptance 6, 18) |
| `tst_map_models` | `TrackMapModel` and `MapCursorDotModel` on a real `SessionModel`: a recording without a local-frame origin has no track and no cursor dot and is left out of the bounds, the bounds are cleared when no track remains, and all of it returns after a source correction through `mergeSessions`; hidden recordings and the plot-range filter on a recovered track; a hole in the GNSS samples: the `trackPoints` role is a list of runs, two at the hole holding every simplified point once, a range edge never interpolated across it, and no cursor dot strictly inside it (`holeBreaksTrackAndDot`; item 1206). It compiles the two map models and their helpers (`plotrangemodel.cpp`, `plotutils.cpp`) directly and needs no Widgets or WebEngine |

**Python plugins**

| Test | Covers |
|------|--------|
| `tst_python_bridge` | The Python plugin bridge through the real embedded interpreter and the real `flysight_cpp_bridge` module (acceptance 17, plugin half): effective reads in single-output plugins, declared-read diagnostics (`UndeclaredInputError`), effective values and units matching C++, no source access (a `source` key is an unknown kind; the view has no source methods), the multi-output form running once, exceptions and malformed output giving a clean unavailable result with negative caching, returned arrays copied, explicit key decoding with per-plugin rejection, plugin-before-built-in precedence, the bundled `imu_tilt.py` example; plugins never start explicit work (`pluginsNeverStartExplicitWork`, on the synthetic explicit calculation `expA`: a plugin that declares its output, one that declares an on-demand value derived from it, and one whose Python code reaches for both undeclared all stay unavailable with a run count of 0 until the calculation is requested, and the first two show the value afterwards). See "The embedded-Python bridge test" below. `pluginWorkflowThroughModel`: a plugin-fed logbook column through import, save and restart (acceptance 17 / 19); every plugin registration declares the plug-in code identity as its result version, equal to the digest recomputed from the folder, the SDK and the versions read independently (`pluginRegistrationsCarryCodeIdentity`) |
| `tst_plugin_identity` | The plug-in code identity (`src/plugincodeidentity.h`) without Python (label `core` only): the pinned encoding (a hand-built byte string for `a.py` and `pkg/helper.py` passed out of order), determinism and independence of the order files are passed in, each ingredient changing it (a file's bytes, a rename, a helper module added or removed, the SDK's bytes or readability, a file's readability, the Python and NumPy versions, bytes moved between files), the token `none` for a version that cannot be read, and the folder walk: every `*.py` under the folder, subfolders included, in name order with `/` names, hidden files counted (a dot-named one; on Windows one with the hidden attribute), `__pycache__` and hidden folders left out, a linked folder read through under its own name and a link loop ended (a symbolic link elsewhere, a junction made with `mklink /J` on Windows; skipped where neither can be made) |

**Solver and sensor fusion** (label `fusion`; `FLYSIGHT_BUILD_FUSION_TESTS`)

| Test | Covers |
|------|--------|
| `tst_solver_smoke` | GTSAM's exported CMake target compiles, links and runs in a test: the install is the shipped configuration (`4.3a0`, TBB on, bundled Eigen 3.4, built without Boost: `GTSAM_ENABLE_BOOST_SERIALIZATION` and `GTSAM_USE_BOOST_FEATURES` are `0`), a small pose graph optimizes to its analytic answer (Eigen, METIS, TBB, library loading), and the main thread really has the 64 MiB stack of `flysight_solver_stack()` (the test uses 48 MiB of it; with a default stack it crashes). Label `fusion`. Nothing from the fusion model is involved |
| `tst_fusion_golden` | The fusion kernel (`flysight_fusion`) through its public API, `src/fusion/fusion.h`, only. What it reaches: `Fusion::run()` on the fourteen committed synthetic fixtures and nothing internal. In spec order: the initializer's prefix and segment fits are boundaries of the same kinds as the full fit's, so cancellation at each kind of boundary (`Starting fit`, graph construction, a prefix fit iteration, a segment fit iteration, a full fit iteration, `Releasing the scale factors` and the released stage's first iteration) leaves an empty result and no state behind, and preparation has no boundary of its own (the first is `Starting fit`); the progress texts at the kernel's boundaries, prefix and segment texts included, are the golden's; two runs are bit-identical with TBB on, a 64 MiB worker thread matches the main thread, and nothing depends on the caller's data. The golden comparison: for every fixture the fit reproduces the goldens captured from the kernel by `fusion_golden_capture` (four successes, `bridged_hole` among them with a 2.6 s hole in its fixes bridged by the IMU: thirty-three channels, the four accuracies and the twelve entries of the position and velocity covariance blocks among them, and the diagnostics with their `configuration`, `initializer`, `stopping`, `quality`, `model` and `accuracy` objects and the input audit's `gnss_holes`; ten rejections: the exact reason, two of them the configuration's checks, `reject_lattice` and `reject_rate`), the channel writer of the capture tool is the inverse of the loader on the committed files (and the hex sample form round-trips signed zero, a NaN and a subnormal by bit pattern), and the comparator holds its bounds, among them the floors of the accuracies (`headingAcc` the heading's, `tiltAcc` the degrees', the twelve covariance entries the default, in m^2 and m^2/s^2, the accuracy account's constants and counts exact) and the rule for the time of the largest step correction, an argmax: not compared in portable mode, bit for bit in exact mode (sensor-fusion-jobs acceptance 4; section 11; fusion-reconstruction items 917-919, 923, 925, 929, 937; noise-model items 1008, 1010, 1022, 1027, 1034, 1040, 1044, 1045, 1051; GNSS-holes items 1301, 1305, 1307, 1309, 1311; staged-scale items 1401, 1404-1406, 1413, 1414; background-computation items 1503, 1517; fused-accuracy items 1702, 1703). Label `fusion` |
| `tst_fusion_kernel` | The kernel's internals through the seams of `src/fusion/` (the only test that includes those headers), with the literal expectations of the reference's self-test: the shared unwrap rule, preintegration across exact boundaries, every validation defect, backward attitude propagation, the propagation refusing an IMU interval above the threshold its caller gives it (`propagationRefusesIntervalAboveItsThreshold`), the IMU gap limit as the continuity rule's threshold, an interval of 1.55 nominal intervals rejected and one of 1.45 not (`imuGapRuleIsTheContinuityThreshold`; sample-continuity item 1211), heading freedom, the reconstruction's timing and its yaw share between two states at rest, an exact constant-velocity fit published exactly by the reconstruction, TBB really on. In spec order: the segmented initializer (segment cutting on fixes and the merges of a short final piece and of any piece with fewer than three fixes, a window shorter than one segment, the smallest-sAcc anchor and the carried-back start, prefix growth on the marginal yaw sigma about the vertical, the growth stop when a doubling gains nothing, the prefix budget of one pass and 50 iterations and starts that end on the limit, the fallback when every prefix start fails, its progress texts and diagnostics keys, the four synthetic initializer recordings of the specification and `coarse_maneuver`); the stopping rule (the bias-settled cost test, the slow tail accepted and refused on each bound, the IMU normalized RMS's alone included (a fit that satisfies its fixes while it ignores the IMU), non-convergence and a never-settling bias as solver failures with their failure shapes, damping saturated at a forced ceiling of 1e5 as the solver failure `damping saturated` where the linearization still predicts a decrease, a stall at the ceiling where it predicts none settled (the exact constant-velocity recording from its exact start under a ceiling at the library's initial damping) and the predicted decrease on a hand-built graph (`saturationAtAMinimumIsSettled`), and the four fits and their prefix fits bit for bit the same under a ceiling of 1e5 and of 1e12); the noise model by configuration (the datasheet's per-sample sigma and integration density against the formula for the fixtures' configurations, bit for bit in exact mode, an entry for every listed value of every key, the configurations without one rejected naming the key; the lattice check finding the coarsest range, a legacy-style truncated reading, the tolerance's edges and every fixture's range; the rate check's 10 % either side; the configuration's checks after every other check; `configuration` and `model.noise` in the diagnostics) and the step model (no sampling term on a constant signal, the sampling term against its derivation for whole and part steps of an unevenly sampled quadratic and against the integration's own departure from the exact integral, the rotation remainder against its derivation on a constant turn and against a thousand-fold subdivided integration on a ramp, the covariances read through the observer); the temperature-dependent gyro bias (the graph shape with `T_ref`, the scaled IMU factor and the slope and scale priors last, the reconstruction at each interval's own bias (the attitude part of the mismatch), a recording without the temperature channel rejected by name, a constant temperature leaving `b1` at its prior and the full fit's held stage, whose prior a thousand times tighter than the datasheet's holds the scale at one, agreeing with the constant-bias fit, the drifting-bias recording recovering `b1` within 20 % in at most 30 iterations of the full fit); the scale state (preintegrating the readings at a scale against preintegrating the divided readings at unit scale, and at any scale the same bits with or without the scale Jacobian and the observer, whose transition is the library's update of the step (`preintegrationDividesByTheScale`); the scale Jacobian against central differences, which it would miss without the half-step turn's term (`scaleJacobianMatchesCentralDifferences`); the scaled IMU factor's seven Jacobians against numerical derivatives, and its equality, bit for bit, with the stock `ImuFactor` at the interval bias at its linearization scale and at the bias itself at zero slope and unit scale (`scaleFactorJacobians`); the graph with the state, the scale prior last, and the constant model's without it (`scaleGraphShape`); every factor of a converged fit's graph preintegrated at the fitted scale (`fitRepreintegratesAtTheFittedScale`); the reconstruction at the fitted scale (`reconstructionUsesTheFittedScale`); `rest_throughout` leaving every factor at one within its prior through both stages, its released stage settled (`restLeavesTheScaleAtItsPrior`); `scale_recording` recovering a 2 % accelerometer factor through the released stage, kept, its objective below the held stage's read from the fit's account (`scaleRecordingRecoversTheFactor`); `model.scale` and the `scale_prior` residual in the diagnostics (`diagnosticsReportTheScale`)). The scale factors as a refinement: the held and the released stage's account in the diagnostics and in the fit's result, its counts adding up to the history, the passes numbered on across the stages and `Releasing the scale factors` reported once between them (`scaleReleaseIsAccountedFor`); the released stage forced to fail at its boundary falling back to the held fit, converged, its factors at one with the held stage's sigmas and its channels the held fit's bit for bit (`releaseFailureFallsBackToTheHeldFit`); divergence forced in the held stage by a zero IMU bound or an empty scale range, a solver failure of the completed-pass shape (`divergenceEndsTheHeldStage`), and in the released stage of `scale_recording` by a range that excludes the recovered factor, the fallback (`divergenceEndsTheReleasedStage`); a slow tail accepted in the held stage followed by the released stage (`slowTailAtTheIterationLimit`). The accuracy: the covariance step's joint covariance of every adjacent pair of fix states with the globals against the library's joint marginals (`covarianceMatchesJointMarginals`), its first node's heading against the heading check on every fit and recording (`firstNodeHeadingIsTheHeadingCheck`), the composition at every tenth sample of `coarse_maneuver` against a graph with a state at every integration edge, the attitude block, the four accuracies and the position and velocity blocks (`sampleCovarianceMatchesTheEdgeGraph`) and, on a fix, the fix's own marginal (`sampleOnAFixHasTheFixMarginal`), the attitude and acceleration formulas on known answers (`attitudeAccuracyFollowsTheNavigationFrame`, `accelerationAccuracyFollowsItsPropagation`), the widening's window and factor on hand-built residuals, one at the model and grown where a sigma is understated three times, and the published accuracies widened by it and the twelve covariance entries by its square, bit for bit (`wideningWindowAndFactor`, `wideningIsOneAtTheModel`, `wideningGrowsWithAnUnderstatedSigma`), the published accuracies finite and positive, the covariance entries finite with a non-negative diagonal and the kernel's blocks symmetric to rounding, and the accuracies never lowered by doubling the GNSS accuracies (`accuraciesFiniteAndPositive`, `gnssAccuracyScalingNeverLowersThem`), the undetermined heading of `coarse_linear` at the cap with tilt and acceleration against a gauge-fixed reference (`undeterminedHeadingIsCapped`), a failed covariance step leaving the sixteen accuracy channels empty and changing nothing else, through the success assembly's seam (`covarianceFailureLeavesTheFitAsItIs`), and the scale factors' sigmas against the library's marginal (`diagnosticsReportTheScaleSigma`). The IMU-rate reconstruction pass (`reconstructAtImuRate()` and its per-interval seam `reconstructInterval()`): equivalence with the held-ends dense graph on `coarse_maneuver` and on a rotating recording (whose published acceleration is also measured against the truth at 13, 25 and 100 Hz), exact ends and P_n equal to the factor covariance, the published time axis with a sample on a fix published once, the summaries equal to the seam's maxima, sharing by noise, zero mismatch (the step corrections zero without rotation, and minus the integration's rotation lag under it), and consistency of the published acceleration with the published velocity. The publication: the thirty-three channels the pipeline publishes are, bit for bit, the reconstruction on the test's own fit of each success fixture with its own covariance step composed and its own widening, and the four numbers of the diagnostics its summaries (`imuRateIsWhatTheFitPublishes`); the success diagnostics' key set with `accuracy`, `dense_output`, `max_step_correction_m_s2`, `max_step_correction_time_s` and `max_velocity_mismatch_m_s`, and limitations that no longer disclaim the reconstruction; the time axis is the IMU samples in `[first fix, last fix)` of the window, in the number `imu_outputs` says, on the four fits and on a recording whose GNSS (25 Hz) is faster than its IMU (12.5 Hz) (`imuRateAxisWhenGnssIsFasterThanImu`). The GNSS holes: on `bridged_hole`, every sample inside its 2.6 s hole published, its heading, tilt and accelerations within three of their own accuracies of the generating trajectory, the position and velocity of the sample covariance of the hole's interval, read through `reconstructInterval()`, growing through the hole and collapsing at the fix after it, the position and velocity within three published standard deviations (`posCovNN`, ...) of the generating trajectory, axis by axis, the composed (unwidened) velocity block larger at its largest inside the hole than beside the two fixes around it, the composed position block, which follows the two fixes' marginals across the hole, logged, the published (widened) blocks logged at the same samples, and the four published accuracies inside the hole logged against the samples nearest the two fixes around it (`bridgedHoleFollowsTheTruth`); the 30 s hole of `long_hole`, the longest the fit bridges, fitted under the production tuning, its iterations per pass, the accuracies inside the hole and the residuals at the fix after it logged, one IMU factor per interval (`longHoleConverges`); the same recording with a 60 s and a 31 s hole rejected by the cap, the reason naming the length and the limit, and with a 30 s hole passing the plan (`holeAboveTheCapIsRejected`); the cutter merging any piece with fewer than three fixes, on hand-built axes and on `long_hole` under 29.57 s segments, whose two fixes after a cut end the first segment (`sparsePiecesAreMerged`); and `input.gnss_holes` on the four fits and `long_hole` against the continuity authority and the fixtures' construction, in time order on a recording with two holes, absent from a rejection (`gnssHolesInTheAudit`); `bridged_hole` is in every list of the success fixtures' properties. The golden comparison: the fit trace (the segment account and the cost before and after every optimizer iteration) against the goldens, which localizes a golden failure to a stage, and the chosen prefix fit's iteration count against the golden's (acceptance 4; section 11; fusion-reconstruction items 901-919, 923-927, 931-937; noise-model items 1008-1011, 1013-1015, 1017-1023, 1025, 1026, 1028, 1029, 1031, 1033-1035, 1040, 1044, 1046-1048, 1051-1054, 1056-1060; GNSS-holes items 1301-1309 and 1313; staged-scale items 1401-1411, 1413; fused-accuracy items 1701-1703). Label `fusion` |
| `tst_fusion_session` | Sensor fusion as a registered calculation (`src/fusion/fusionregistration.cpp`) on real `SessionData` engines bound to the global registry, with the real fit on the test's main thread. What it reaches: the engine's request, prepare / compute / publish and blocker paths on fixture sessions whose effective inputs are bit-identical to the kernel's fixtures, and a natural session through the real input chain. The registration's shape: seventeen registrations, the four configuration defaults first, the fit with 26 inputs (all required, `IMU/temperature` the last measurement, the four configuration attributes last) and 34 outputs (the seventeen measurements of the state, the four accuracies, the twelve covariance entries and the diagnostics, as a literal list, and the result version `batch-temperature-bias-v10`; noise-model item 1033; fused-accuracy items 1701, 1702, 1704), explicit, title "Sensor fusion", then the fused speeds `velH` and `vel` (fused-speed items 1601, 1604), `accH` and the fused accuracies `hAcc`, `vAcc` and `sAcc` before the system time (fused-accuracy item 1707). The configuration: the four constant defaults (no inputs, one output each, the default's text), a stored value winning and its removal returning to the default (`configurationDefaults`); the configuration reaching the kernel as numbers for a stated and a defaulted session and as NaN for a stored value that is not a number, a fixture session (which stores its fixture's configuration) fitted to the golden, channels and diagnostics, and the same session without the keys, reading the 12.5 Hz default, rejected by the rate check (`configurationReachesTheKernel`); a recording without keys, logged at 12.5 Hz, fitted under the default (`naturalSessionEndToEnd`) (noise-model items 1004, 1006, 1007, 1010, 1043, 1045, 1050, 1061). In spec order: cancellation at each kind of boundary through the engine's facility publishes and caches nothing (sensor-fusion-jobs acceptance 10); a session with the temperature column carries it to the kernel bit for bit and matches the kernel's direct run, and a session without `IMU/temperature` is `MissingInput` / `NotApplicable` like one without IMU data, a local origin or a time fit (11). The lifecycle: reads of every fusion value, the fused speeds, `accH`, the fused accuracies, the system-time axis, the diagnostics and an interpolated logbook value never run the fit, in any order (forty-two names, read in three orders whose strides are coprime with the count), nor does the exporter (5; fused-speed item 1607; fused-accuracy item 1708); a request runs once, publishes all outputs together, and brings the fused speeds, `accH`, the fused accuracies and `_system_time` with it, each run once: `vAcc` the square root of the published `posCovDD` bit for bit, `hAcc` and `sAcc` finite and non-negative at every sample (6; fused-speed item 1607; fused-accuracy item 1708); prepare / compute / publish equals `request()` bit for bit (7); an input change after publication drops everything while markers do not (8); a rejection is a cached result with its reason, `NotProduced` for inspection, and requestable again after an input change (9); blocker inspection reports the fit through on-demand intermediates and never starts it (12); two sessions are independent. The fit declares its kernel's algorithm string as its result version, no output of an explicit built-in has another candidate (`explicitOutputsHaveOneCandidate`), and a fit exported from one session and restored into another is indistinguishable from the fresh one: every channel bit for bit, the diagnostics byte for byte, status, detail, dependency edges (`SCHEMA_VER` among its leaves), blockers and invalidation, and the fit's snapshot lists what provided each name it looked up (`restoredFitIsIndistinguishable`) (fusion-reconstruction items 912, 921, 930). The golden comparison: every published result equals the kernel's goldens. Label `fusion` |
| `tst_fusion_derived` | What is derived on demand from the fit's published outputs (`src/fusion/fusionregistration.cpp`), without the solver: the fit's outputs are stored as data on real `SessionData` engines bound to the global registry (`syntheticFitSession()`, section 11), and every expected value is an exactly representable literal or, for "one definition", the GNSS calculation on the same samples. The seam's premise: stored `Fusion/<name>` reads back bit for bit and the fit never runs. The registrations of `builtin.fusion.velH` and `vel` (after the fit, `velH` first), of the fused accuracies `builtin.fusion.hAcc`, `vAcc` and `sAcc` (after `accH`, in that order) and of `builtin.fusion.z`, `accAlongTrack` and `accCrossTrack`: on demand, no title or result version, their inputs in order (the track accelerations those of the GNSS ones), one candidate each, the fit their one explicit dependency, and `Fusion/velD` and `Fusion/accD` with no second producer (vertical speed and vertical acceleration). On a fixture session that has the fit's inputs and no fit, each waits on the fit alone: `Blocked` by it, merely uncomputed, unavailable, and nothing runs it. Elevation is `_LOCAL_ORIGIN_HMSL` less `down` less `_GROUND_ELEV`, recomputed from an attribute edit without the fit and unavailable when either attribute is not a number. The track accelerations' known answers: along north, reversed, a vertical descent, a wind that turns a skewed ground velocity north, no motion through the air, a wind that is not a number (zero), no stored wind (the constant zero default) and unequal lengths (unavailable). The fused and the GNSS track accelerations agree on the same samples and wind (bit-exact in exact mode, within 4 ulp otherwise). The fused speeds' known answers (Pythagorean triples with mixed signs, a standstill, two triples halved), read without the fit, and unequal lengths (a short `velE` takes both speeds, a short `velD` the total alone); the fused and the GNSS speeds agree on the same samples (bit-exact in exact mode, within 4 ulp otherwise) (`fusedSpeedsKnownAnswers`, `fusedSpeedsAreTheGnssDefinitions`, `derivedValuesWaitOnTheFit`; fused-speed items 1601, 1607). The fused accuracies' known answers on the covariance blocks stored as data, exact except one sample within 1e-12: the horizontal accuracy on diagonal, rotated and (5, +-4, 5) horizontal blocks and the zero block, whatever the down entries; the vertical accuracy on down variances 6.25 and 0; the speed accuracy's rule (the horizontal acceleration accuracy's) under a diagonal block, a velocity at or above its along-track sigma giving that sigma, one below it and a zero velocity the largest eigenvalue, under a full block both branches of the 3x3 form, and the zero block 0; unavailable without the blocks (the velocity alone gives no speed accuracy, the down variance alone a vertical and no horizontal one) and with one entry one sample short (the calculation that reads it unavailable, the others as they were); the fit never runs (`fusedAccuraciesKnownAnswers`, `derivedRegistrationShape`, `derivedValuesWaitOnTheFit`; fused-accuracy items 1707, 1708). The orientation vocabulary (`src/fusion/orientation.h`): exactly 24 pairs in the enumeration order, the default first, distinct tokens and labels, each token parsing back and nothing else parsing, every body-to-device rotation exact and proper with the columns forward, forward x up and -up; the attribute's one definition, its choices the enumeration, also after the entry point registers on a private registry. The attitude (`builtin.fusion.attitude`, after the orientation's constant default `builtin.default._ORIENTATION`): its registration, waiting on the fit on a fixture session, and on the quaternion stored as data (expected values built by hand from Euler angles and a hand-written default mount, within 1e-9 degrees): a level north-facing body, known heading, pitch and roll, two turns of heading unwrapped and measured from north (unchanged by a GNSS track or a course reference of any kind, and available without GNSS data), roll and pitch in their ranges through a barrel roll and a loop, the forward axis exactly vertical (the body pitched straight up and down between ordinary samples under the default orientation, and a level device under the mounts forward +z and forward -z, the sample whose pitch argument rounds past 1): all three angles published and finite, pitch at +90 or -90, roll in range and the later samples still reading their own angles, side mounts, the fit's own yaw, pitch and roll from a success golden's quaternion under forward +x, up -z (within 1e-6 degrees), an invalid stored orientation (unavailable) and a stored one recomputed without a fit. The Orientation column's model and bulk edit through `ChoiceFixture` on fixture sessions, loaded and as stubs: the default's label with nothing written and its token cached, a token stored and written verbatim, a token outside the list, a label, an empty and an invalid value refused, and the bulk edit setting and refusing (fusion-plots items 806-827, 831, 835, 843, 844, 849, 850, 856-858). Nothing is held to a golden bit for bit (the one golden it reads, `coarse_maneuver`'s, is compared within 1e-6 degrees), so no `_exact` run. Label `fusion` |
| `tst_fusion_jobs` | The real fit through the executor on a real `SessionModel`, on the executor's 64 MiB worker: one job publishes all thirty-four outputs together, the thirty-three measurements aligned, and announces them through the session model (acceptance 6; noise-model item 1042; fused-accuracy item 1704); the executor gives the bits a synchronous request gives (7); the solver's oneTBB helper threads run at the worker's below-normal priority while they help a fit (`solverThreadsRunAtWorkerPriority`); an input edit during the fit asks the fit to stop at once, ends the job Superseded, publishes nothing, and leaves it requestable (8); a rejected recording is a Succeeded job carrying the reason, with nothing to do on re-request and a fresh run after an input change (9); cancel during the fit publishes nothing and the next job starts afterwards (10); a session without IMU data cannot have a job, and every one of the fifteen fusion plots is `NotApplicable` for it (11; noise-model item 1063; fused-speed item 1603); the logbook column over `Fusion/roll`, the column worker and the saver never start a fit (5), and that column is cached as unavailable before the fit, from the published result after it (with the `"records"` stamp in `index.json`), shown by the unloaded row after a restart without a load, and dropped with the record by an input change (`columnOnFusionOutputIsCachedFromRecord`); an altitude marker added at run time and registered again after a restart keeps that column's and the description's cached values of an unloaded session - no load, nothing pending (`altitudeMarkerKeepsColumnsOfUnloadedSession`); once that value of an unloaded session is gone from `index.json` (cleared, or dropped by an exit-marker move made while the application was closed: `_EXIT_TIME` is not bulk-editable, so the test repeats the `LogbookManager` calls of the bulk edit's stub path - temporary load, column marked unsaved, save), the column worker refills it after a restart from the stored fit restored into its temporary copy: the roll at the marker's time, bit-identical to the loaded session's, with no load of the row, no job, no fit and the record's bytes unchanged (`workerRefillsColumnFromStoredFit`); while the loaded row's cell shows the golden's number the moment the job publishes (`dataChanged` for that row only, and the number already there when the view is told); shutdown during a fit. Mid-run actions are taken in a slot on the job's first progress text, which the executor delivers before the job's end: no gate, no sleeps. `realRecordingCheck` is the optional local check of section 11 and skips unless `FLYSIGHT_FUSION_RECORDING` is set. Label `fusion` |
| `tst_fusion_runner` | `fusion_runner`, the command-line fit, driven as a child process on fixtures written out as `TRACK.CSV` / `SENSOR.CSV`: its diagnostics equal a direct `Fusion::run()` on the fixture and equal the application's own import-and-fit path (`SessionImport` on a `SessionModel`); the CSV output reloads bit for bit; `--dump-inputs` shows the twenty-six effective inputs, including the legacy gyro scale of a file without `SCHEMA_VER` and the default text of the configuration a recording does not state (noise-model items 1007, 1050, 1061); a rejection exits 1 with the failure JSON and writes no CSV; usage and import failures exit 64 and 3; no calculation on the fit's input path declares a preference (the premise of the model-free import); and the thirty-three output channels of `fitOutputChannels()` are the golden's columns in order, which the CSV's header lists (fusion-reconstruction items 912, 923; fused-accuracy item 1704). Label `fusion` |
| `tst_fusion_golden_exact`, `tst_fusion_kernel_exact`, `tst_fusion_session_exact`, `tst_fusion_jobs_exact`, `tst_fusion_rows_exact`, `tst_fusion_store_exact`, `tst_fusion_runner_exact` | Not executables: the seven tests above that compare with the goldens, run a second time with `FLYSIGHT_FUSION_EXACT=1` and otherwise the same environment, so that every golden comparison is bit equality (section 11, "Tolerance policy"). Registered only where that is a fair demand, the compiler the goldens were captured with (`FLYSIGHT_FUSION_EXACT_TESTS`, section 3). The first two decide bit-identity; the next four show that the bits survive the engine, the executor's worker thread, the demand layer's plot demand and a stored and restored record; the last is bit identity across the process boundary (the runner's output against an in-process run). Labels `fusion` and `exact` |
| `tst_fusion_rows` | The plot-row script with the **real** fusion plots: `PlotModel` + `CalculationDemand` + the executor + `SessionModel` + the fusion registration, with real fits on the executor's 64 MiB worker and the fifteen plots of `fusionPlots()` (`tests/fusion/fusionsessions.h`, which mirrors `MainWindow::registerBuiltInPlots()`; `audit_cleanup` pins the application's list at fifteen rows). All fifteen plots are explicit-backed (the fit their one requested calculation) and every one is drawn after the one fit, on the fit's time axis (`allFusionPlotsAreExplicitBacked`, named without a count; noise-model items 1037, 1063; fused-speed items 1602, 1603, 1608); the four accuracy plots are merely uncomputed before the fit, not produced for a rejected recording, which is listed once with the fit's reason, and not applicable to a session without IMU data, like every fusion plot (`accuracyPlotsAreAbsentWithoutAFit`; noise-model item 1038); the row script of acceptance 15 on three real tracks (checking fits them one after another with no other action, the sessions still to compute falling as each publishes; unchecking mid-way drops the waiting one and lets the running one finish; checking again resumes; a fourth track shown is fitted with no other action), with every published track held to the kernel's goldens and the job history as a literal; Heading, Pitch and Roll share one job and one progress text (`headingPitchRollShareOneJob`); `accH` is blocked by the fit and never has a job of its own; the Total speed row alone sees through `vel` and `velH` to the fit, creates one job, the fit, and is drawn after it on the fit's time axis, neither speed ever a job (`totalSpeedRowIsBlockedByFusion`; fused-speed items 1603, 1609); a session without IMU data is never counted and never listed among failures before, during and after a fit (11); a rejected recording is listed among failures with the reason, offers no retry, and is fitted again after its input changes and settles (9); sessions are edited, tracks hidden and shown and other values read while a real fit runs, without disturbing it, and are fitted afterwards with no other action (19, the half that needs no widget); the demand layer's items on demand, failures and chains unchanged in what they assert with real fits (status-bar item 753); the measurements of the removed plots still serve a logbook column kept from before (the fit's roll and the local frame's north at the exit marker, labelled with the measurement's name, `Fusion/roll @ ...`, beside a column over the Roll row labelled `Roll @ ...`) and the fit's stored record carries every channel whose plot the fusion-plots specification removed, `velD` among them though it is drawn again as Vertical speed (`removedPlotMeasurementsStayAvailable`) (fusion-plots items 801-804, 806, 808, 842, 853, 854, 856; fusion-reconstruction item 930). Steered by the first progress text of a job and by `jobFinished`: no gate, no sleeps. Label `fusion` |
| `tst_fusion_store` | The fit's stored result: bit-identical after unload and restart (the thirty-three channels, the four accuracies and the twelve covariance entries among them, and the fused speeds `velH` and `vel` derived from them, with the Total speed row checked after the restart and nothing run; noise-model item 1042; fused-speed items 1603, 1609; also when fitted before the first save), rejection / solver failure listed with its reason, dependency (a declared input, a configuration attribute stored over its default, `SCHEMA_VER`) and code-stamp invalidation (the fit's result version stamped as `batch-temperature-bias-v5`, the algorithm string the documented noise model retired, the one users' logbooks hold; noise-model items 1041, 1062), a record rewritten to `batch-temperature-bias-v9`, the string before the covariance blocks, deleted at load and the fit, still wanted by the checked Roll row, run once more to its end and stored under the current string with the golden's channels, nothing run after (`recordUnderPreviousAlgorithmIsComputedAgainOnce`; fused-accuracy items 1702, 1704), merges, session file untouched, not requested after the `cache/` folder was deleted while closed (one fit offered while the Roll plot is checked, dropped when unchecked, nothing run); kept across altitude-marker, registration, descent-pause and plugin-set changes, in memory and after a restart; dropped at once, with its record, by a registry change that changes what a name it looked up resolves to (the removal of its provider), kept by a candidate registered behind the provider; deleted when a lookup resolves differently at load; a provider's result version in the record; a logbook column over roll filled for sessions that are not loaded, and nothing fitted again after a restart (`columnOverFusionFillsUnloadedSessions`, `fusionColumnWithStoredFitsRunsNothing`); a column over a fusion row (each of the four accuracies, and Total speed, typed `speed`), at the exit marker and typed as its row, filled for a session that is not loaded by one fit, its record written and its value cached and indexed with the fit's stamp, the value the loaded session reads bit for bit, its text the unit converter's for the row's type and its label the row's name (`fusionColumnFillsUnloadedSessions`; noise-model items 1039, 1063; fused-speed items 1603, 1609); a stored success whose accuracy was not computed (the record rewritten without the sixteen accuracy channels, the four accuracies and the twelve covariance entries, no reason, the diagnostics' `accuracy.computed` false) restored without the sixteen and with the seventeen of the golden, the Roll row done with nothing failed and nothing fitted again (`restoredFitWithoutAccuracyDrawsTheRest`; noise-model item 1038; fused-accuracy item 1704); also run as `_exact` (fusion-reconstruction items 920-922, 938) |

Three executables are built with these but are not tests and are not counted
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

`fusion_golden_capture` is the second: it runs the product kernel on the fourteen
synthetic fixtures and writes the goldens of `tests/data/fusion/` (section 11,
"The capture tool" and "Re-capture procedure"). `fusion_runner` is the third:
the fit on one recording imported as the application imports it, with the
diagnostics on standard output, for the reference recordings and the corpus
comparison (section 12.2).

**Audit**

| Test | Covers |
|------|--------|
| `audit_cleanup` | No old mechanism remains, each fact has one authority, none of the mechanisms of `sensor-fusion-clean-port` that have no successor exists, the structural rules of background work hold (one worker, no locks, GTSAM confined, work started only by the demand layer through the executor, views that only read the demand layer, a model and scheduler that know no jobs, a widget-free core), stored results live in the logbook's `cache/` folder, never in the session file, and are named, written, read, restored and deleted in one place each, and a stored result goes stale only when its in-memory twin would be dropped or its code changed (no environment fingerprint in a record, the plug-in code identity computed in one place, teardown removals at shutdown only), and no refresh, cancel, queue or plot-request logic remains in code or documents, background work is shown in one place, the status bar, and a failure once per recording, on its logbook row (the per-source presentation, its indicators, its clock and the logbook's progress line gone from code and documents), a default standing in for a value the user has not set is a calculation, a constant one registered by one helper, one type owns the mount vocabulary, the plot list has its fifteen fusion plots, the four accuracies in the deep scheme of the GNSS accuracy plots, the fit's down velocity and down acceleration the only fit channels with a row, and no local-frame or removed fusion plot remains in code or documents, nor a document that counts the fusion plots as twelve, the noise model's configuration, datasheet table, scale state and covariance each have one authority, the fusion document carries the validation of the model, and every line of `tests/acceptance_map.txt` resolves (section 10) |

The `tst_calc*` tests drive `src/engine/` with synthetic calculations against
`FakeSessionState` / `FakePreferenceProvider` (`support/fakesessionstate.h`).
Each builds its own `CalculationRegistry`; none registers anything in
`CalculationRegistry::instance()`.

The built-in calculations are pinned by one golden table
(`support/builtinfixture.*`: `DescentFixture` generates a 296-row jump plus a
three-row sensor file; `goldenValues()` holds literals only). Rows marked
"captured" were recorded from the v2026.04.1 engine before the migration; all
others were derived by hand. `tst_builtins_engine` uses private registries
(except `digestSurvivesRuntimeAltitudeMarker` and
`altitudeMarkerTeardownReportsNothing`, which drive the real
`AltitudeMarkerManager` on the process-wide one and remove what they added);
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
application does: the column environments captured at `initialize()` are
compared with the ones of the next start, and registering in between would
make every reopen discard the columns the registration reaches. Stored results
do not follow that rule. A record carries no calculation environment, so registering
a calculation between writing a record and loading its session keeps the
record valid unless the registration changes what a name the result looked up
resolves to. To make a record stale on purpose, change a resolution (register
a winning candidate for a looked-up name, as
`tst_result_store::lookupResolvingDifferentlyDeletesRecord` does) or rewrite
the record (`rewriteRecord` in `tst_result_store` and `tst_fusion_store`).
Because of the cache rule above, set the
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

**Run the suite sequentially.** The fusion tests that run real fits through
the executor (`tst_fusion_jobs`, `tst_fusion_rows`, `tst_fusion_store` and
their `_exact` runs) fit on the executor's worker, which runs below normal
priority (docs/CALCULATIONS.md, section 15.5), so they are sensitive to
concurrent CPU load: under load a fit can take long enough to reach a test's
timeout. Run the suite without `-j`, and not beside another CPU-heavy run (a
second `ctest`, a build).

## 3. Options and labels

| Option | Default | Effect |
|--------|---------|--------|
| `FLYSIGHT_BUILD_TESTS` | `OFF` | Adds `tests/` to the application build. No install rules: packaging is unaffected |
| `FLYSIGHT_BUILD_PYTHON_TESTS` | `ON` | Only with the first: also build `tst_python_bridge`. `OFF` removes the target. If NumPy is missing from the build-time Python the test is still built but listed as **Disabled**, not omitted (section 5) |
| `FLYSIGHT_BUILD_WIDGET_TESTS` | `ON` | Only with the first: also build `tst_logbook_indicators`, `tst_status_bar` and `tst_choice_attribute`, the three tests that link Qt Widgets (they run offscreen views). `OFF` removes the targets, and then no test target links Widgets. Forwarded by the root `CMakeLists.txt` like the others |
| `FLYSIGHT_BUILD_FUSION_TESTS` | `ON` | Only with the first: also build the GTSAM-linked tests (`tst_solver_smoke`, `tst_fusion_golden`, `tst_fusion_kernel`, `tst_fusion_session`, `tst_fusion_derived`, `tst_fusion_jobs`, `tst_fusion_rows`, `tst_fusion_store`, `tst_fusion_runner`) and the executables `solver_deploy_probe`, `fusion_golden_capture` (the golden capture tool) and `fusion_runner` (the command-line fit), all defined in one block of `tests/CMakeLists.txt` (the tests through `flysight_add_fusion_test()`). `OFF` removes the targets, and then no test target references GTSAM. Forwarded by the root `CMakeLists.txt` like the other two |
| `FLYSIGHT_FUSION_EXACT_TESTS` | `AUTO` | Only with the fusion tests: register the bit-exact runs `tst_fusion_*_exact` (label `exact`; no new executable). `AUTO` registers them when the compiler is 64-bit MSVC at exactly the `cl_version` in `tests/data/fusion/capture.json` (19.44.35220.0; the file is written by `fusion_golden_capture` at every capture, and a Visual Studio patch de-registers the runs until the next one), and otherwise prints "Fusion exact tests not registered: ..." at configure time; `ON` registers them whatever the compiler is; `OFF` never does. In every mode they exist for the Release configuration only. Forwarded by the root `CMakeLists.txt` like the others (section 11, "Tolerance policy") |

All four sub-options are forwarded by the root (superbuild) `CMakeLists.txt` to
the application project, unconditionally, so switching one back reaches the
inner cache too.

CTest labels: every executable has `core`; `tst_python_bridge` also `python`;
`tst_session_oracle` also `oracle`; `audit_cleanup` has `audit`. GTSAM-linked
tests also have `fusion`, so `ctest -LE fusion` is the GTSAM-free run. The
Widgets tests also have `widgets` (`ctest -LE widgets` runs everything else).
The bit-exact runs have `core`, `fusion` and `exact` (`ctest -L exact`; seven
on the capture configuration).

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
to list the test functions. A fusion executable, the capture tool and the
runner also need the solver runtime directories
(`third-party/GTSAM-install/bin`, `third-party/oneTBB-install/bin`; section
11's re-capture procedure and section 12.2 spell the full `PATH`).

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
`PluginHost` imports every `*.py` in the plugin folder and digests every
`*.py` under it, subfolders included, for the plug-in code identity; the
test's folder holds top-level files only, so the imported and the digested
files are the same.

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
simulates an application restart on the same folder. Stored-result records live
in the test logbook's `cache/` folder (`TestEnvironment::cacheDir()`, created
by the first record written) and go with it. `reset()` also forgets
the file stems reserved for sessions not saved yet, as a process exit would.

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
  other at random, so that rings overlap in ways nobody designed;
  `randomizedExplicitSequences` (seeds 1-25) with explicit calculations and
  registry changes of candidates they look up; `registryChangePrecision`
  (one row per kind of registry change) pins which cached answers a change
  keeps and that a kept one is served without a run.
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
  (`tst_logbook_indicators`, `tst_status_bar` and `tst_choice_attribute` are
  the only ones)
  writes its own `main()` instead:
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
  deferred saves and column fills (it waits until no scheduler task has work,
  the column fill included, and no tick is due: `IdleScheduler::hasWork()`
  and `isTicking()`).
- Shared helpers live in the support library; use them instead of a local
  copy. `testutil.h`: `isNear` (absolute 1e-9), `sameBits`,
  `sameBitsEverywhere` (two sample vectors), and `WarningCapture`, which collects warnings while it lives (`count()`,
  `messages()`, `matching(fragment)`, `count(fragment)`). `logbookprobe.h`:
  what is on disk in the test logbook (`readIndex`, `writeIndex`,
  `indexValue`, `sessionFilePath`, which is empty for an unknown session,
  `sessionCsvFiles`, `calculationRecordFiles` (the `*.fvresult` names in
  `cache/`, none when it does not exist), `sessionFileStem` (the stem of a session's file, empty before
  its first save) and `indexRecordStamp` (a session's `"records"` stamp in
  `index.json`)), `UnreadableFile` (makes an existing file unreadable while
  it lives and puts it back on `release()`: `Directory` on every platform,
  `LockedWithoutSharing` on Windows, `NoReadPermission` elsewhere;
  `skipReason()` is non-empty where the mechanism is not honoured and the row
  `QSKIP`s), `asFormatOne` (a format-2 record's bytes in the format-1
  layout), the shared logbook columns (`descriptionColumn`,
  `gyroColumn`, `exitTimeColumn`, and `attributeColumn(key)`, a
  session-attribute column over `key`), `writeAltitudes`, and the
  `dependencyChanged` spy checks (`spyHasAttribute`, `spyHasMeasurement`).
  `storedresults.h`: `storedResolution` (a `StoredResolution` built in one
  call) and the stand-in plug-in folder (`standInPluginIngredients`,
  `standInPluginIdentity`: a real plug-in code identity for a C++
  registration that stands in for a plug-in, since one process cannot boot
  the interpreter twice).
  `Synthetic::attr` / `Synthetic::measKey` (`fakesessionstate.h`) are the
  shorthand for public names. `asyncdriver.h` drives
  `PreparedCalculation::compute()` inline, on a joined `std::thread`, or on a
  `QThread` (`computeOn`, `ComputeRun`, `addComputeModeRows`) and provides
  `RecordingProgress`; it uses `std::thread`, which is why
  `flysight_test_support` carries `Threads::Threads` as a usage requirement
  (stated once there; no test names it).
  `jobfixture.h` (`JobWorld`, `Gate`, `waitIdle`, `waitStarted`, `Quiet`,
  `activeJobIds`, `onFirstProgress`) registers controllable explicit calculations on the
  global registry for tests of the executor and whatever sits on top of it:
  a test holds the executor's worker inside a compute function
  (`Gate::waitEntered`), then releases (`Gate::open`) or cancels it,
  without sleeps; construct the `JobWorld` before the `SessionModel` and
  destroy it after the executor and the model. `ExtraRegistrations` holds
  calculations one test function adds to the global registry; destroyed after
  the executor and the model, it unregisters them newest first, as runtime
  changes. `Quiet` is "nothing started
  since" (no new job row); `activeJobIds(queue)` is the ids of the active
  job records, in model order: the running job, then the chosen next job;
  `onFirstProgress` acts on the main
  thread while a job that no gate can hold (a real fit) is still running.
  `plotfixture.h` (`PlotFixture`) adds synthetic plots (`Syn/...`, eight,
  the last over the `exhausted` calculation for an out-of-memory job) over
  those calculations through on-demand bridge calculations, plus `show()`
  and `giveInput()` for the application's visibility and edit paths,
  `spin()` (two turns of the event loop and a flush of the demand layer),
  `waitDemandIdle()` (the executor idle, no pass pending, no session
  settling and no load step with work; follow it with `waitForIdle(model)`
  when column values must be filled; it returns as soon as the fill has no
  work, which can be before the scheduler's completion tick: the fill
  reports no progress, so wait for that tick with `waitForIdle(model)` or for
  `schedulerIdle` with `QTRY_*`); construct it
  after the `JobWorld` and destroy it before it.
  `choicefixture.h` (`ChoiceFixture`) is a `SessionModel` over the test
  logbook with the description column and one Choice attribute's column,
  parametrized by the attribute key (the definition is read from the
  attribute registry; it registers nothing): `start()` adopts sessions,
  loaded, or as stubs after `restartAsStubs()`, an application restart, and
  it reads a cell's display text, the token in a session's file
  (`fileToken()`, header-only), and the value `index.json` caches, and edits
  with `setData()` and `bulkEdit()` (followed by `waitForIdle()`). Helpers that need GTSAM or
  the fusion library live in `tests/fusion/` instead (section 11, "Fusion
  sessions"), which only the `fusion` tests link: the support library stays
  free of GTSAM, `flysight_fusion` and Qt Widgets.
- Stored results. `SessionModel::storedResultStats()` /
  `resetStoredResultStats()` count the records written, read, restored, kept,
  skipped and deleted, and the time spent restoring and writing. Provoke a write
  failure portably, with a directory at the record's path or an output
  attribute type a record refuses; never with permission bits (Windows ignores
  the read-only attribute on directories). A write failure is also announced
  (`SessionModel::calculationRecordWriteFailed`), and the demand layer shows
  it as a failure. Make a record unreadable with `UnreadableFile`. A
  directory at a record's path is never listed, so the
  store reaches it only through the ids the logbook manager knows: within a
  run, not after a restart; restart rows use the lock or the permissions.
  Make a record stale by rewriting it or by changing a resolution; a
  registration alone does not. Expected record names are literals
  (`<stem> + ".builtin%2Efusion%2Efit.fvresult"`). Never call
  `verifyAgainstFresh()` / `evaluateFresh()` on a session with a fit
  installed: the oracle replays requested calculations and would fit again.
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
  calculations (a live model schedules a calculation-environment check), and
  a runtime removal that reaches a requested result deletes its record.
  `ExtraRegistrations` and `JobWorld` are destroyed after the model for that
  reason. `unregister()` takes the removal kind with no default: a test
  passes `CalculationRegistry::Removal::Change`, and never switches a fixture
  to `Removal::Teardown` to get around the order: it is for owners destroyed
  at shutdown.
- When a test function demonstrates an acceptance clause, add it to
  `tests/acceptance_map.txt` and to the matrix in section 9.

## 9. Acceptance traceability

Eighteen specifications, eighteen ranges of items in
`tests/acceptance_map.txt`, the machine-checked form of the eighteen tables
below (section 10); keep them in sync.

### 9.1 Schema and calculation engine (items 1-19)

Every acceptance item (items 1-19, stated in full in
[appendix A](#appendix-a-the-acceptance-items-1-19)), clause by
clause, and the test functions that assert it with literal expectations.
Item 6 is stated as amended by the specification "Sensor fusion plots,
attitude and the orientation attribute" (9.9).

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
| 6 | (as amended) saving does not rescale or relabel; loading adds only jumper mass and planform area where they are absent (the legacy backfill), additively and once; wind is not written and reads zero from a constant default | `tst_persistence_roundtrip::releasedLogbookSaveKeepsBytes`, `releasedLogbookBackfillIsAdditive`; `tst_logbook_index::rawLoadSkipsBackfill`; `tst_workflow::releasedLogbookUpgrade` |
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
| 13 | ... including cached columns of unloaded sessions (the columns whose closure the registration reaches) | `tst_column_cache::environmentCheckDropsExactlyTheReachedColumn` (added and removed), `altitudeMarkerRemovalDiscardsStubValues` (removed) |
| 14 | explicit: unavailable until requested; the request publishes all outputs | `tst_calcengine::explicitPolicy`; `tst_session_engine::explicitPolicyOnSession` |
| 15 | declared preference invalidates dependents | `tst_calcengine::declaredPreferenceInvalidates`; `tst_builtins_engine::analysisRangeFollowsPreference`; `tst_session_engine::declaredPreferenceInvalidates`; `tst_session_model_engine::preferenceBroadcastReachesModel`; `tst_column_cache::preferenceChangeDiscardsOnlyReadingColumns` |
| 15 | snapshotted preference does not affect existing sessions | `tst_calcengine::snapshottedPreferenceDoesNot`; `tst_session_engine::snapshotPreferencesDoNot`; `tst_column_cache::snapshotPreferenceDoesNotDiscard`; `tst_import_merge::defaultsNotReappliedOnMerge` |
| 16 | derived `wTotal` and interpolated gyro attribute are corrected and follow the source | `tst_source_layer::derivedWTotalUsesCorrectedGyro`, `interpolatedGyroAttribute`; `tst_session_engine::derivedWTotalFollowsSource`, `interpolatedAttributeFollows` |
| 16 | file-supplied `wTotal` wins | `tst_source_layer::fileSuppliedWTotalWins` |
| 17 | single-output plugins, real bridge, effective reads | `tst_python_bridge::bootsRealBridge`, `singleOutputPluginsReadEffectiveValues` |
| 17 | multi-output plugin runs once | `tst_python_bridge::multiOutputRunsOnce`, `bundledExampleRuns` |
| 17 | plugin reads are effective values and units, matching C++; plugins have no source access | `tst_python_bridge::effectiveReadAndUnitMatchCpp`, `derivedMeasurementReadsEffective`, `sourceKindIsUnknownToPlugins`; `tst_calcregistry::sourceInputsOnlyInSourceConversions` |
| 17 | Python exception: clean unavailable | `tst_python_bridge::exceptionYieldsCleanUnavailable`, `bundleExceptionPublishesNothing` |
| 18 | upgrade discards and recomputes cached gyro columns | `tst_logbook_index::missingMarkerDiscardsValues`, `missingColumnEnvironmentsDiscardOnce`; `tst_column_cache::upgradeDiscardsAndRecomputes`; `tst_workflow::releasedLogbookUpgrade` |
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
In the map, item = 100 + the number in the first column. Items 110, 111, 113,
114, 115, 116 and 117 are stated as amended by the specification
"Demand-driven requested calculations" (9.6), items 111 and 115 again by the
specification "One status bar for background work" (9.8), and item 115 again
by the specification "Sensor fusion plots, attitude and the orientation
attribute" (9.9), by the specification "The documented noise model and the
accuracy, part 1" (9.11), whose four accuracy plots made the real fusion plots
twelve, and by the specification "Fused speed plots" (9.17), whose three fused
speeds make them fifteen.

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
| 8 | input change while running: superseded, nothing published, requestable | `tst_calcengine_async::inputChangeWhileRunningRefuses`; `tst_session_engine::asyncRequestOnSession`; `tst_jobqueue::inputChangeWhileRunningSupersedes`; `tst_fusion_jobs::inputChangeDuringFitSupersedes`; (stopped early) `tst_calcengine_async::willBeRefusedReportsTheEnginesMarks`, `tst_jobqueue::staleRunningJobIsStoppedAtOnce`, `offerWhileStaleJobWindsDown`, `tst_calculation_demand::staleRunningJobIsWaitingAtOnce` |
| 8 | change after publication drops the result and dependents | `tst_calcengine_async::changeAfterPublicationDropsDependents`; `tst_fusion_session::changeAfterPublicationDropsEverything` |
| 9 | rejection: unavailable, diagnostics reason, succeeded job with the reason, no second run, fresh run after an input change | `tst_fusion_session::rejectionIsACachedResult`; `tst_fusion_jobs::rejectedRecordingIsSucceededJob`; (synthetic) `tst_jobqueue::rejectionSucceedsWithReason`; (listed among the failures) `tst_fusion_rows::rejectedTrackShowsBadge` |
| 10 | (as amended) cancel through the executor stops at the next boundary, publishes nothing, requestable; the chosen next job starts | `tst_fusion_golden::cancelAtEachKindOfBoundary`, `cancelDuringPreparation`; `tst_fusion_session::cancelStopsAtNextBoundary`; `tst_jobqueue::cancelRunningThenNextStarts`; `tst_fusion_jobs::cancelDuringFitThenNextJobStarts` |
| 11 | (as amended) no-IMU session: never counted in progress nor listed among the failures; no job possible | `tst_jobqueue::refusesMissingInput`; `tst_calculation_demand::sessionWithoutInputIsNeverListed`; `tst_fusion_session::missingInputsAreNotApplicable`; `tst_fusion_jobs::noImuSessionCannotHaveAJob`; `tst_fusion_rows::noImuSessionIsNeverCounted` (all fifteen real rows) |
| 12 | blockers report fusion for `accH`, nothing after publication, never a fit | `tst_calcengine_blockers::derivedNameReportsExplicitBlocker`, `inspectionNeverRunsExplicit`; `tst_fusion_session::blockersReportFusion` |
| 13 | (as amended) B consumes A: checking the plot runs A then B | `tst_calcengine_blockers::chainedBlockers`; `tst_calculation_demand::chainedBlockersContinue` |
| 14 | (as amended) one job at a time; at most the running and one chosen next job; no duplicates | `tst_jobqueue::oneAtATimeInOfferOrder`, `duplicateOffersCreateNoDuplicates`, `holdsAtMostRunningAndChosenNext` |
| 15 | (as amended) row script, without widgets, synthetic: checking computes the visible tracks one after another, the sessions still to compute falling; unchecking drops the waiting ones, a track shown is computed | `tst_calculation_demand::rowScript`, `showingASessionStartsIt`, `uncheckingDropsWaitingPairsKeepsRunning` |
| 15 | (as amended) row script with the real fusion plots (the fifteen, the four accuracies since the specification of 1001-1065, the three fused speeds after Elevation since the specification of 1601-1612); heading / pitch / roll share one job; `accH` blocked by fusion | `tst_fusion_rows::realRowScript`, `headingPitchRollShareOneJob`, `accHRowIsBlockedByFusion`, `rejectedTrackShowsBadge`, `allFusionPlotsAreExplicitBacked` |
| 15 | what the user sees | `manual M3`, `M4`, `M6`, `M7` |
| 16 | (as amended) start-up restore with hidden tracks starts nothing; a profile or a programmatic check creates demand like a click (logic) | `tst_calculation_demand::startupRestoreWithHiddenSessionsStartsNothing`, `profileStyleApplyCreatesDemand`, `programmaticCheckCreatesDemand` |
| 16 | ... and the plot widget does not warn "No data available" about a checked plot that is merely uncomputed (the predicate; the widget's one call of it is M1) | `tst_calculation_demand::merelyUncomputedIsNotWorthAWarning` |
| 16 | ... in the real `MainWindow` (cannot be constructed in the harness) | `audit gestures`; `manual M1`, `M2` |
| 17 | (as amended) remove / unload with a running or chosen next job; shutdown | `tst_jobqueue::removeSessionWithRunningJob`, `removeSessionWithQueuedJob`, `evictionDeferredWhileJobActive`, `shutdownWithQueuedAndRunning`; `tst_fusion_jobs::shutdownDuringFit` |
| 17 | quitting the application | `manual M8`, `M9` |
| 18 | every transition through model signals; history from the model alone | `tst_jobmodel::historyFromSignalsAlone`, `retentionBound` |
| 18 | ... and no job is persisted (the records of jobs that published `Ok` are results, not jobs) | `tst_jobmodel::nothingIsPersisted` |
| 19 | sessions editable, tracks shown / hidden, other values readable while a real fit runs | `tst_fusion_rows::editsAndVisibilityDuringFit`; `tst_jobqueue::idleSchedulerKeepsWorking` |
| 19 | plots pan and zoom during a fit | `manual M5` - widget-only, no automated test is possible in the harness |
| 20 | none of the branch's mechanisms exists | `audit branch-mechanisms`, `audit naming`, `audit one-worker` |

### 9.3 Sensor fusion improvements (items 201-247)

The forty-seven requirements of the specification "Sensor fusion: segmented
initializer, stopping rule and IMU model" (`PLANS/fusion-improvements.md`),
stated in full in
[appendix C](#appendix-c-the-acceptance-items-of-the-sensor-fusion-improvements-201-247).
In the map, item = 200 + the requirement number; the same four line forms as
9.2, and every item has at least one test or audit line. Items whose evidence
goes beyond their tests (the corpus tally and the four reference recordings of
the specification's section 12) point to the manual steps of section 12.2.
"Section" is the section of the specification; a citation of a document
section (item 247) is text here and carried in the map by the audit rule that
keeps that document current. Two requirements the specification added after
the first numbering, the budgets of prefix and segment fits (3.3) and the
on-limit starts (section 10, test 2b), are second rows of items 210 and 239
rather than items of their own, so that the range stays 201-247. Item 228 is
stated as amended by the specification "One status bar for background work"
(9.8), and items 215, 216, 217, 221, 227, 245 and 247 by the specification
"The documented noise model and the accuracy, part 1" (9.11): the per-step
term and its slopes give way to the datasheet's noise by configuration and
the derived sampling term and rotation remainder. Its accuracy amends items
230 and 235: the runner's CSV and the result gain the four accuracy channels.
The specification "Fused position and speed accuracy" (9.18) amends them
again: the twelve covariance entries, thirty-three channels and thirty-four
outputs.
Items 219 and 246 are stated as amended by the specification "The scaled IMU factor stands alone",
which removes the temperature factor and the switch of the scale state: the
interval bias is evaluated inside the scaled IMU factor, whose tests stand for
the temperature factor's (219), and a constant temperature reproduces the
constant-bias fit with the scale held at one by a prior a thousand times
tighter than the datasheet's (246).
Item 244 is stated as amended by the specification "GNSS holes bridged by
the IMU" (9.14): the slow tail is accepted only when the IMU normalized RMS
is below the bound too. Item 243 is stated as amended by the specification
"The scale factors as a refinement" (9.15): the full fit runs its passes in
two stages, and each converges within two passes.

| # | Section | Clause | Evidence |
|---|---|---|---|
| 201 | 3.2 step 1 | segments of 600 s from the first fix; a final piece shorter than 120 s joins the one before; a window shorter than one segment is one segment | `tst_fusion_kernel::segmentsAreCutOnFixes`, `shortWindowIsOneSegment`, `driftingBiasSegmentsConverge` (four segments under 60 s / 12 s), `initializerDiagnosticsShape` (`segment_length_s` 600) |
| 202 | 3.2 step 2a | the anchor is the fix with the smallest sAcc, the earliest on a tie; the coarse attitude there (force aligned with GNSS acceleration minus gravity, zero bias) | `tst_fusion_kernel::smallestSaccFixIsTheAnchor`, `shortWindowIsOneSegment` (tie: the first fix), `allPrefixFitsFailFallsBack` (`startRotation` equals `coarseAttitude()` at the anchor bit for bit) |
| 203 | 3.2 step 2b | the prefix is 60 s centred on the anchor, clipped to the segment; its start is the coarse attitude carried to the window's first fix with zero bias; four heading starts with the production graph and tuning | `tst_fusion_kernel::smallestSaccFixIsTheAnchor` (window 170-230 s), `initializerProgressTexts` (`prefix 60 s`, four starts before the segment fit), `initializerDiagnosticsShape` (`prefix_fits` 4) |
| 204 | 3.2 step 2c | the marginal yaw sigma of the window's first pose from the best start's graph rebuilt at the fitted bias, rotated into the navigation frame, vertical element | `tst_fusion_kernel::yawSigmaIsMarginalAboutTheVertical` (exactly unobservable: 180; observable: below 20; invariant to a yaw rotation of the linearization point), `startsInMotionGrowsToTheManoeuvre` (the sigma sequence) |
| 205 | 3.2 step 2d | the window doubles while the sigma exceeds 20 degrees, the window does not cover the segment, and the last doubling cut the sigma by at least 20 %; otherwise growth stops (`observable`, `covers`, `no_gain`) | `tst_fusion_kernel::startsInMotionGrowsToTheManoeuvre` (60 -> 120 s, exactly at the first length containing the manoeuvre), `atRestPrefixStopsGrowing` (`growth_stop` `no_gain` at 120 s of a 300 s resting recording, the second sigma above 0.8 of the first), `startsOnTheLimitAreStillUsed` (`covers`), `tst_fusion_golden::successFixturesMatchGolden` (`stationary_spin`'s `prefix_length_s` 120, `prefix_fits` 8) |
| 206 | 3.2 step 2e | the segment is fitted once from the best prefix fit's attitude carried backwards and forwards with that fit's gyro bias; the fitted attitude of every fix and the bias are kept | `tst_fusion_kernel::smallestSaccFixIsTheAnchor` (the carried-back start, `1e-9`), `driftingBiasSegmentsConverge` (every segment fit converged, `iterations > 0`), `initializerProgressTexts` (the segment fit follows the prefix fits) |
| 207 | 3.2 step 3 | initial values: per-pose attitude from its segment's fit, GNSS positions and velocities, the first segment's gyro bias, zero accelerometer bias | `tst_fusion_kernel::shortWindowIsOneSegment` (`state.gyroBias == segments[0].gyroBias`, one rotation per fix), `validationRejectsEachDefect` (`initialValues()` refuses a wrong-sized state), `fitTraceMatchesGolden` (the full fit's first cost is the stitched start's) |
| 208 | 3.2 step 4, 3.4 | the full fit runs from the stitched state and converges; a resting segment's yaw is arbitrary and accepted | `tst_fusion_kernel::startsInMotionGrowsToTheManoeuvre` (truth attitude within 2 degrees), `atRestPrefixStopsGrowing` (roll and pitch within 0.5 degrees; yaw unasserted), `driftingBiasSegmentsConverge`; corpus and reference recordings: `manual M11`, `M12`, `M13`, `M14`, `M15` |
| 209 | 3.3 rule 1 | segment fits are cancellable and report progress; each iteration is a boundary; the text names the segment and, for a prefix, its length | `tst_fusion_golden::cancelAtEachKindOfBoundary` (rows `prefix fit iteration`, `prefix fit second iteration`, `segment fit iteration`), `progressMatchesGoldenBoundaries`; `tst_fusion_kernel::initializerProgressTexts` |
| 210 | 3.3 rule 2 | a failed prefix or segment fit is a start with infinite objective; all four failing grows the prefix; all failing at full length falls back to the propagated coarse attitude and the diagnostics say so | `tst_fusion_kernel::allPrefixFitsFailFallsBack` (`coarse_maneuver`: `fallback` true, `fallback_segments` `[0]`, `yaw_sigma_deg` null, the full fit still runs; `motion_start`: growth to 240 s with twelve failed starts, the window being centred on the first fix), `prefixFitsFailAfterACompletedLength` (`motion_start` with only the 60 s fits spared: the account of the fallback holds no prefix fit, not the completed 60 s one) |
| 210 | 3.3 Budgets | a prefix fit is one pass of at most 50 iterations, a segment fit uses the production limits; the stopping rule with its slow tail applies to prefix and segment fits; a fit that ends on the limit is still used as a start and reported (`prefix_on_limit`, `segment_on_limit`) | `tst_fusion_kernel::startsOnTheLimitAreStillUsed`, `atRestPrefixStopsGrowing`; `tst_fusion_kernel::validationRejectsEachDefect` (`maxPasses` 0 refused) |
| 211 | 3.3 rule 3 | a segment satisfies the fitted window's validity rules (fixes inside IMU coverage, no IMU gap, at least three fixes) | `tst_fusion_kernel::segmentsAreCutOnFixes` (a final piece with fewer than three fixes is merged), `validationRejectsEachDefect` |
| 212 | 3.3 rule 4 | no stationary-window detector takes part; it decides nothing | `audit fusion-model` (the detector, its gates and the silent poll are absent from the tree); `tst_fusion_golden::cancelDuringPreparation` (preparation asks nothing: the first boundary is `Starting fit`) |
| 213 | 4.1 | the bias-settled test is the re-preintegration cost test at 1e-6 relative; the pass structure (at most five passes) and the settle tolerance 1e-8 are unchanged | `tst_fusion_kernel::biasSettledByCostTest`, `biasNeverSettlesIsSolverFailure` (five passes, `bias not settled`), `exactConstantVelocityFit`, `nonConvergenceIsSolverFailure` (five passes); `tst_fusion_golden::successFixturesMatchGolden` (`stopping.passes` and the thresholds) |
| 214 | 4.2 | a final pass at the limit is accepted when the last 20 iterations' mean relative decrease is below 1e-4 and the position and velocity nRMS are below 2; the diagnostics say so; a fit meeting neither stays a solver failure | `tst_fusion_kernel::slowTailAtTheIterationLimit` (rows `accepted`, `nrms bound fails`, `decrease bound fails`), `nonConvergenceIsSolverFailure` (one iteration cannot fill the window) |
| 215 | 5 formula, as amended | the per-step term is the sampling term and the rotation remainder of the specification of 1001-1065, in quadrature as `(D^2 + eps^2 / dt) I`; a step without signal change and without rotation has the density covariance exactly | `tst_fusion_kernel::constantSignalHasNoSamplingTerm`, `samplingTermFollowsTheDerivation`; `tst_fusion_golden::successFixturesMatchGolden` (`coarse_linear`'s numbers are the density-only model) |
| 216 | 5 constants, as amended | no slopes; the densities are derived from the configuration; the step boundaries and the midpoint sampling are unchanged | `tst_fusion_kernel::noiseFollowsTheTable`, `preintegrationHonoursExactBoundaries` |
| 217 | 5 dt scaling, as amended | the term has no constant and falls with the step as the sampling error does | `tst_fusion_kernel::samplingTermFollowsTheDerivation` (whole and part steps of an unevenly sampled quadratic) |
| 218 | 5 constraint | the per-step covariance is applied by setting the shared parameters before each `integrateMeasurement` call | `audit fusion-model` (exactly one `.integrateMeasurement(` call in `imuintegration.cpp`, and a sensor covariance assigned nowhere else); `tst_fusion_kernel::samplingTermFollowsTheDerivation` (the effect, read through the observer) |
| 219 | 6 model, as amended | `b(t) = b0 + b1 (T(t) - T_ref)`, `T_ref` the mean IMU temperature over the fitted window, the accelerometer bias constant; each interval evaluated at its own bias in the fit and in the reconstruction; amended: the bias is evaluated inside the one IMU factor of the full fit, the scaled factor, whose tests stand for the temperature factor's | `tst_fusion_kernel::temperatureGraphShape` (`tRef` the index-order mean; the factor's `temperatureDelta`), `scaleFactorJacobians` (seven Jacobians; equal to `ImuFactor` at the interval bias, and at zero slope and unit scale), `reconstructionUsesIntervalBias`, `driftingBiasSegmentsConverge` (`b1` within 20 %, `t_ref_degc` 35, `b0` at `T_ref`; `t_ref_degc` and `b1_rad_s_per_degc` are always numbers) |
| 220 | 6 priors | `b0` under today's prior (0.03 rad/s); `b1` zero-mean with sigma 0.010 deg/s per degC | `tst_fusion_kernel::temperatureGraphShape` (the slope prior's sigmas equal `Tuning{}.gyroBiasSlopeSigma`), `validationRejectsEachDefect` (a non-positive `gyroBiasSlopeSigma` is refused), `driftingBiasSegmentsConverge` (`slope_prior` last, after `bias_prior`) |
| 221 | 6 input channel, as amended | `IMU/temperature` is a required input of the fit (the twenty-second when it was added; the fit has twenty-six since the specification of 1001-1065), one value per IMU sample; a recording handed no temperature is rejected by the kernel with a reason naming `IMU/temperature`, and a session without the column is blocked like any missing input | `tst_fusion_kernel::validationRejectsEachDefect` (absent, wrong length, non-finite: the reason names `IMU/temperature`, and after every other channel's defect); `tst_fusion_session::registrationShape` (26 inputs, all required), `missingInputsAreNotApplicable` (the `no-temperature` row), `temperatureReachesTheKernel`, `inputsAreBitIdenticalToFixture` (there is no temperature rejection golden: the twelve fixtures all carry the channel) |
| 222 | 6 constant temperature | a recording whose temperature does not change leaves `b1` at its prior | `tst_fusion_kernel::constantTemperatureKeepsSlopeAtPrior` (every component of `b1` below 1 % of the prior sigma in magnitude; the slope prior's residual zero; the objective equals the constant-bias fit's) |
| 223 | 6 initializer unaffected | segment and prefix fits use a constant bias; the full fit starts `b1` at zero | `tst_fusion_kernel::driftingBiasSegmentsConverge` (stock prefix fits: `prefix_fits` 4, `prefix_length_s` 60), `temperatureGraphShape` (the four-argument builder is the stock graph); `tst_fusion_golden::successFixturesMatchGolden` (the goldens as Phase 6 re-captured them: the `initializer` objects unchanged by the temperature model) |
| 224 | 7 initializer | the diagnostics report the segments (start, end, prefix length, yaw sigma, iterations) and any fallback | `tst_fusion_kernel::initializerDiagnosticsShape` (the key sets), `allPrefixFitsFailFallsBack` (`fallback_segments`); `tst_fusion_golden::successFixturesMatchGolden` |
| 225 | 7 stopping | which rule ended the fit, the last pass's mean relative decrease, the re-preintegration cost difference | `tst_fusion_kernel::biasSettledByCostTest`, `slowTailAtTheIterationLimit`, `failureDiagnosticsShape` (`cost increased` with nulls), `nonConvergenceIsSolverFailure` (the failure key set) |
| 226 | 7 quality | normalized RMS of the IMU, position and velocity factors, and the objective per state | `tst_fusion_kernel::biasSettledByCostTest` (recomputed from the residual array), `exactConstantVelocityFit` |
| 227 | 7 model, as amended | the configuration, `model.noise` and the fitted `b0`, `b1` with `T_ref` (always numbers: the temperature is required) | `tst_fusion_kernel::diagnosticsReportTheNoiseModel`, `driftingBiasSegmentsConverge`, `constantTemperatureKeepsSlopeAtPrior` |
| 228 | 7 tooltip / account, as amended | the failure text (the status bar's and the logbook row's hover) keeps showing the reason; the diagnostics are an account, not an input | `tst_fusion_rows::rejectedTrackShowsBadge` (the reason in the failure entry); `tst_fusion_session::registrationShape` (`_FUSION_DIAGNOSTICS` is an output and no input) |
| 229 | 8 input | a folder or two paths; imported as the application imports (parser, conversion layer, on-demand derivation, the legacy gyro scale); the kernel gets the job queue's channels | `tst_fusion_runner::matchesTheApplicationImportPath`, `successMatchesDirectRun`, `legacySchemaScalesTheGyro`, `outputTableMatchesGolden` |
| 230 | 8 output, as amended | diagnostics JSON on stdout; `--csv` writes the seventeen channels, since the specification of 1001-1065 the twenty-one and since that of 1701-1717 the thirty-three, or the seventeen when the accuracy is absent; exit 0 for Succeeded, non-zero otherwise with the outcome and reason on stderr | `tst_fusion_runner::successMatchesDirectRun`, `rejectionExitsOne`, `usageAndImportFailures` |
| 231 | 8 no GUI, logbook, preferences | the runner reads and writes no user logbook or settings | `tst_fusion_runner::noPreferenceOnTheFitPath`; `audit fusion-tooling` (`fusion_runner.cpp` names no `LogbookManager`, `PreferencesManager`, `SessionModel`, `SessionImport`, `EnginePreferenceProvider`, `applyCreationDefaults`, `JobQueue`) |
| 232 | 8 progress | progress texts on stderr; cancellation not required | `tst_fusion_runner::successMatchesDirectRun` (stderr is the kernel's texts in order, then `Succeeded`) |
| 233 | 8 tooling target | built with the tests, never shipped | `audit fusion-tooling` (no install rule names `fusion_runner` or `fusion_golden_capture`); `manual M10` |
| 234 | 9 purity, threading, cancellation | a function of the channels, no session or GUI object, every solver iteration a boundary, cancellation observed at boundaries only | `tst_fusion_golden::resultIsIndependentOfCallerState`, `workerThreadMatchesMainThread`, `twoRunsAreBitIdentical`, `cancelAtEachKindOfBoundary`, `cancelDuringPreparation`, `cancelNeverRequestedChangesNothing`; `tst_fusion_session::cancelStopsAtNextBoundary`; `audit solver-confinement` (the kernel is pure, does not log); `audit fusion-model` (exactly three `checkpoint(` call sites, no silent poll) |
| 235 | 9 result contract, as amended | channels and outcomes unchanged by that specification; the specification of 1001-1065 adds the four accuracy channels and that of 1701-1717 the twelve covariance entries (thirty-four outputs); a slow-tail acceptance is `Succeeded` | `tst_fusion_kernel::slowTailAtTheIterationLimit` (`Succeeded`, seventeen channels filled, empty reason); `tst_fusion_session::registrationShape` (34 outputs since the specification of 1701-1717); `tst_fusion_runner::outputTableMatchesGolden` (thirty-three channels in order); the closing audit's `fusion.h` diff against `master` (comment lines and the one added field) |
| 236 | 9 GTSAM | nothing outside the fusion library links GTSAM | `audit solver-confinement`; `flysight_assert_solver_confinement()` at configure time (`GTSAM link confinement: OK (14 targets reach gtsam)`) |
| 237 | 10 goldens | the parity tests are retired; goldens are captured from the changed kernel with the same harness and the regression tests compare against them; fixtures are deterministic | `tst_fusion_golden::successFixturesMatchGolden`, `rejectionFixturesMatchGolden`, `progressMatchesGoldenBoundaries`, `channelsWriterIsTheInverseOfTheLoader`, `fixturesAreDeterministic`, `comparatorHoldsItsBounds`; `tst_fusion_kernel::fitTraceMatchesGolden`, `initializerFixturesAreDeterministic`; the closing audit's fresh capture equal to the goldens on disk (section 11) |
| 238 | 10 test 1 | starts in motion: prefixes grow until the sigma is below 20 degrees exactly at the first length containing the manoeuvre; the full fit within 2 degrees of the truth | `tst_fusion_kernel::startsInMotionGrowsToTheManoeuvre` |
| 239 | 10 test 2 | at rest throughout, longer than two doublings: growth stops at 120 s (`no_gain`); the full fit converges; roll and pitch within 0.5 degrees | `tst_fusion_kernel::atRestPrefixStopsGrowing` |
| 239 | 10 test 2b | prefix fits run one pass of at most 50 iterations; a prefix and a segment fit forced onto the iteration limit are still used as the start and the diagnostics say so | `tst_fusion_kernel::startsOnTheLimitAreStillUsed`; every initializer test (`prefix_passes` 1, `prefix_iterations` at most 50): `startsInMotionGrowsToTheManoeuvre`, `atRestPrefixStopsGrowing`, `smallestSaccFixIsTheAnchor`, `driftingBiasSegmentsConverge` |
| 240 | 10 test 3 | sAcc 2 m/s except 0.3 m/s at 200 s: that fix is the anchor, the first prefix is 170-230 s, the segment's start attitude is the prefix fit's carried back | `tst_fusion_kernel::smallestSaccFixIsTheAnchor` |
| 241 | 10 test 4 | longer than two segments with a 1 deg/s linear drift: every segment fit converges; with section 6 the full fit recovers `b1` within 20 % in at most 30 iterations | `tst_fusion_kernel::driftingBiasSegmentsConverge` |
| 242 | 10 test 5 | a segment whose prefix fits all fail falls back, the diagnostics say so, the full fit still runs | `tst_fusion_kernel::allPrefixFitsFailFallsBack` |
| 243 | 10 test 6, as amended | bias-settled test: a fit whose cost stops changing converges within two passes in each stage of the full fit, the held and the released (amended by the specification of 1401-1415) | `tst_fusion_kernel::biasSettledByCostTest` (`coarse_maneuver`: held 1 pass, released 2) |
| 244 | 10 test 7, as amended | slow tail: a forced final pass at the limit is accepted when every condition holds (the mean relative decrease, the position, velocity and IMU normalized RMS below their bounds) and fails when any fails, the IMU normalized RMS alone included (the specification of 1301-1313) | `tst_fusion_kernel::slowTailAtTheIterationLimit` (the row `imu nrms bound fails`) |
| 245 | 10 test 8, as amended | per-step term: a step without signal change and without rotation has the density covariance exactly; a known second derivative gives the derived sampling term; a constant turn the derived remainder | `tst_fusion_kernel::constantSignalHasNoSamplingTerm`, `samplingTermFollowsTheDerivation`, `rotationRemainderFollowsTheDerivation` |
| 246 | 10 test 9, as amended | section 6, as amended: a recording without `IMU/temperature` is rejected by the kernel (the reason names the channel) and blocked in a session like any missing input; constant temperature leaves `b1` at its prior and, with the scale factors of the specification of 1001-1065 held at one by a prior a thousand times tighter than the datasheet's, reproduces the constant-bias fit | `tst_fusion_kernel::validationRejectsEachDefect`, `constantTemperatureKeepsSlopeAtPrior`; `tst_fusion_session::missingInputsAreNotApplicable` |
| 247 | 11, as amended | `docs/` describes the segmented initializer, the stopping rule with its slow tail, the noise model of the specification of 1001-1065 in place of the per-step term, and the temperature-dependent bias, in the place that documents the fusion model | `docs/SENSOR_FUSION.md` sections 4 and 5; `audit fusion-model` (the document describes no retired mechanism: no resting-window detector, candidate window, coarse-only initializer, frozen algorithm, bias-shift test, branch name, or input count from before the temperature channel); `audit noise-model` (no modelling weights, no calibrated slopes; the datasheet's tables and the derivation cited) |

### 9.4 Stored requested calculation results (items 301-350)

The fifty clauses of the specification "Storing requested calculation results
with the session", stated in full in
[appendix D](#appendix-d-the-acceptance-items-of-stored-requested-results-301-350).
In the map, item = 300 + the clause number; the same four line forms as 9.2,
and every item has at least one test or audit line. "Section" is the section
of the specification. Clauses 37-45 are its section 8 tests, one per bullet,
and clauses 47-50 its principles. Clauses 10, 16, 17, 34 and 41 are stated
as amended by the specification "Stored results: validity that mirrors memory"
(9.5). Clauses 4, 6, 21 and 37 are stated as amended by the specification
"Demand-driven requested calculations" (9.6), and clauses 6, 21, 37 and 39 by
the specification "One status bar for background work" (9.8). Clause 37 is
stated as amended by the specification "The documented noise model and the
accuracy, part 1" (9.11), and again by the specification "Fused position and
speed accuracy" (9.18): the restored fit has thirty-three channels.

| # | Section | Clause | Evidence |
|---|---|---|---|
| 301 | 2 | the result of a requested calculation that ended as a function of its inputs is stored on disk in the logbook's cache: success with its outputs, rejection / solver failure with reason and diagnostics | `tst_result_store::writesOnOkInstall`, `rejectionIsWritten`; `tst_fusion_store::restoredAfterEvictionIsBitIdentical`, `restoredRejectionShowsBadge` |
| 302 | 2 | it is restored at load, so readers, plot rows and logbook columns see it as if just published, while valid | `tst_result_store::restoreOnEveryLoadPath`; `tst_calcengine_restore::restoreIntoFreshEngine`; `tst_fusion_store::restoredAfterRestartIsBitIdentical`; `tst_result_columns::restartShowsCachedValueWithoutLoading` |
| 303 | 2 | on-demand and plugin results are never stored | `tst_calcengine_restore::exportOnlyInstalledOk`, `restoreNotFound` |
| 304 | 2, as amended | a stored result is a memory, not a request: stale or missing, the calculation reads not requested until something switched on (a checked plot over a visible session, an enabled column over it) has it computed | `tst_fusion_store::codeStampChangeDropsRecordOnLoad`, `dependencyEditDropsRecord`; `tst_result_store::staleRecordDeletedOnLoad`; `tst_calcengine_restore::restoreStaleChecks`; `tst_result_columns::staleRecordDeletedByWorkerCreatesDemand`; `audit stored-results` |
| 305 | 2 | the session file is unchanged: bytes, format, enumeration; derived sensors never in it | `tst_fusion_store::sessionFileBytesUnaffectedByRecord`; `tst_result_records::saveSessionIgnoresRecords`, `strayRecordsRemovedAtScan`; `audit stored-results` |
| 306 | 2, as amended | the kernel is unchanged, and plot rows and columns count a restored result as computed: it is not counted in progress and runs no job | `tst_fusion_golden::successFixturesMatchGolden`; `tst_calculation_demand::rowScript`, `storedResultsCreateNoJob`; `tst_fusion_rows::realRowScript`; `audit gestures` |
| 307 | 3 | at most one record per (session, calculation), written at the install on the main thread, replaced by the next publish | `tst_result_store::writesOnOkInstall`; `tst_result_records::writeReadReplace`; `tst_calcengine_restore::installedOnSyncAndAsync` |
| 308 | 3 | a record holds the id, the outcome, every installed output (measurements with unit, attributes with the diagnostics) and the validity stamp | `tst_result_records::roundTripIsBitExact`, `rejectionShapedRecord`, `unavailableAndEmptyOutputs`; `tst_result_store::rejectionIsWritten` |
| 309 | 3 | nothing stored for a result not installed (cancelled, out of memory, refused) or not Ok; such a run deletes nothing | `tst_result_store::nonOkInstallWritesAndDeletesNothing`; `tst_jobmodel::nothingIsPersisted`; `tst_calcengine_restore::installedForEveryStatus`, `exportOnlyInstalledOk` |
| 310 | 3, as amended | a record is removed only when stale on load, with its session, on an input change or a runtime registry change that drops its result, or replaced; never by eviction, unload, a registry change that does not reach it, a teardown removal, quit | `tst_result_store::noDeleteWithoutInputChange`, `inputChangeDeletesRecord`, `registryChangeDeletesRecord`; `tst_calcengine_restore::droppedByInputChange`, `droppedByPreferenceAndSource`, `noDropEventWithoutInputChange`, `droppedByRequestOfUpstream` |
| 311 | 3 | values round-trip bit for bit (-0, NaN, infinities; strings byte for byte) | `tst_result_records::roundTripIsBitExact`, `attributeTypesRoundTrip`, `layoutIsPinned`; `tst_calcengine_restore::restoreNaNPayloadAndSignedZero`, `sameContentBitExactAttributes`; `tst_fusion_session::restoredFitIsIndistinguishable` |
| 312 | 3 | the golden tests cannot tell a restored fusion result from a fresh one | `tst_fusion_store::restoredAfterEvictionIsBitIdentical`, `restoredAfterRestartIsBitIdentical`; `tst_fusion_session::restoredFitIsIndistinguishable` |
| 313 | 4.1 | the input fingerprint covers the values the result depended on, transitively, absent ones included; the record lists their names | `tst_calcengine_restore::fingerprintKnownAnswer`, `fingerprintCanonicalForms`, `leafKindCodes`, `exportLeaves`, `restoreChain` |
| 314 | 4.1 | an edit the result does not depend on keeps the record valid | `tst_fusion_store::unrelatedEditKeepsRecord`, `mergeIntoUnloadedSession`; `tst_result_store::inputChangeDeletesRecord`; `tst_calcengine_restore::restoreIgnoresAttributeType` |
| 315 | 4.1 | an edit it depends on (IMU merge, SCHEMA_VER, any dependency) invalidates it | `tst_fusion_store::dependencyEditDropsRecord` (a declared input and `SCHEMA_VER`), `mergeIntoLoadedSessionDropsRecord`, `mergeIntoUnloadedSession`; `tst_fusion_session::restoredFitIsIndistinguishable` (`SCHEMA_VER` is a leaf of the fit); `tst_calcengine_restore::restoreStaleChecks`; `manual M18` |
| 316 | 4.2, as amended | valid only while CalculationCompatibilityVersion and the result version match and every lookup resolves as it did; fusion's result version is the algorithm string | `tst_fusion_store::codeStampChangeDropsRecordOnLoad`; `tst_result_store::staleRecordDeletedOnLoad`; `tst_result_records::stampsAreCurrent`; `tst_fusion_session::registrationShape`; `tst_calcengine_restore::restoreAcrossRegistries`; `audit stored-results` |
| 317 | 4.2, as amended | the bump rule's clause: bump it, or the result version of the calculation concerned, whenever a change can alter what a requested calculation or anything it reads produces | `audit stored-results`, `audit result-validity` |
| 318 | 4 | a record that fails any check is deleted at load; the calculation reads not requested; nothing is recomputed | `tst_result_store::staleRecordDeletedOnLoad`, `upstreamMissingAfterLastPassDeletes`; `tst_fusion_store::codeStampChangeDropsRecordOnLoad` |
| 319 | 5 | every load path installs the valid records before any reader asks | `tst_result_store::restoreOnEveryLoadPath`, `bulkEditPromotionRestores`, `restoresChainInPasses`, `restoresUpstreamFirst`, `alreadyInstalledIsKept`; `tst_fusion_store::mergeIntoUnloadedSession` |
| 320 | 5 | same outputs, status, detail and dependency edges as a fresh publish; later invalidation identical | `tst_calcengine_restore::restoreIntoFreshEngine`, `restoredResultInvalidatesLikePublished`, `restoreBeatsOutstandingTicket`, `restoreNeverReplaces`; `tst_fusion_session::restoredFitIsIndistinguishable` |
| 321 | 5, as amended | plot rows count a restored result as computed: it is not counted in progress and runs no job; a stale or absent result is in demand like any missing one | `tst_fusion_store::restoredAfterEvictionIsBitIdentical`, `restoredAfterRestartIsBitIdentical`, `dependencyEditDropsRecord`; `tst_calculation_demand::storedResultsCreateNoJob`; `manual M1`, `M16` |
| 322 | 5 | blocker inspection reports a restored result as a published one, NotProduced with its detail | `tst_calcengine_restore::restoreRejection`; `tst_fusion_store::restoredRejectionShowsBadge`, `restoredSolverFailureShowsBadge` |
| 323 | 5 | logbook columns over a requested calculation: computed from the restored or published result, cached with the record stamp; unavailable without a record | `tst_result_columns::columnExplicitCalculations`, `unrequestedIsCachedUnavailable`, `stampWrittenOnFlush`, `publishedResultIsCached`, `restartShowsCachedValueWithoutLoading`, `noRecordStaysUnavailableAfterRestart`, `recordBeforeFirstSaveIsCachedAfterSave`; `tst_calcregistry::explicitDependencies`; `tst_column_cache::explicitBackedColumnFollowsItsResult`; `tst_fusion_jobs::columnOnFusionOutputIsCachedFromRecord`, `workerRefillsColumnFromStoredFit`; `tst_result_columns::workerRestoresStoredResult`; `tst_fusion_session::explicitOutputsHaveOneCandidate`; `manual M17` |
| 324 | 5 | dropping or writing a record drops the values stamped with it: crashes, old indexes, failed writes, environment changes | `tst_result_columns::inputChangeDropsCachedValue`, `onlyDependentColumnsDrop`, `staleRecordOnLoadDropsCachedValue`, `workerRestoresStoredResult`, `crashAfterRecordWrite`, `crashAfterRecordDelete`, `rewriteAfterDropFlushesIndexFirst`, `writeAfterStartupDropFlushesIndexFirst`, `oldIndexWithoutStamp`, `resultVersionChangeDropsCachedValue`, `writeFailureKeepsValueOutOfIndex`, `environmentChangeDiscardsReachedColumn`, `registryChangeKeepsLoadedRowConfirmed`, `managerDropsDependentValues`, `deletingSessionRemovesStamp` |
| 325 | 5 | unloading (eviction, hide beyond the cache capacity, quit) loses nothing | `tst_result_store::noDeleteWithoutInputChange`; `tst_fusion_store::restoredAfterEvictionIsBitIdentical`; `manual M16`, `M17` |
| 326 | 5 | publishing writes the record, nothing else does: a restore never rewrites it | `tst_result_store::restoreOnEveryLoadPath`; `tst_calcengine_restore::restoreIsNotAnInstall`; `audit stored-results` |
| 327 | 5 | written through the logbook manager atomically; a failed write leaves the previous record and the in-memory result; tried again at the next publish | `tst_result_store::writeFailureLeavesResultUsable`, `writeFailureKeepsPreviousRecord`; `tst_result_records::writeFailureRefusedEncoding`, `writeFailureDirectoryAtPath`, `writeFailureCacheFolderNotCreated`, `writeForUnknownSession`; `audit stored-results` |
| 328 | 5 | a session fitted before its first save stores its result under its reserved stem; a never-saved session's record is a stray | `tst_result_store::recordBeforeFirstSave`, `recordOfNeverSavedSessionIsStray`, `removeBeforeFirstSave`; `tst_fusion_store::fittedBeforeFirstSaveIsRestored` |
| 329 | 5 | a record is deleted with its session and when found stale | `tst_result_store::deletingSessionRemovesRecords`; `tst_result_records::removeSessionDeletesRecords`, `removeSessionDottedStems`, `failedSessionRemovalKeepsRecords`, `removeOneAndAll`; `tst_result_columns::removingReservedSessionForgetsIt`; `manual M19` |
| 330 | 6 | one file per (session, calculation) in the logbook's `cache/` folder (a sibling of `sessions/`, which holds only the recordings), named from its stem and the id, never mistaken for a session; `cache/` may be deleted while the application is closed, after which every requested calculation reads not requested | `tst_result_records::fileIdEncoding`, `fileNameParsing`, `fileNamesDifferIgnoringCase`, `strayRecordsRemovedAtScan`, `orphanAdoptionKeepsRecord`, `recordsFollowRemap`, `cacheFolderCreatedByFirstWrite`; `tst_result_columns::deletedCacheFolderForgetsRequests`; `tst_fusion_store::deletedCacheFolderReadsNotRequested`; `audit stored-results`; `manual M19` |
| 331 | 6 | the encoding: bit-exact doubles, readable without the session, the order of the session file's size, a version so a future format is refused | `tst_result_records::layoutIsPinned`, `futureVersionIsRefused`, `corruptInputIsRefused`, `readStatuses`, `sizeIsOrderOfSamples`, `encoderRefusesUnsupportedAttribute`; `tst_result_store::staleRecordDeletedOnLoad` |
| 332 | 6 | existing logbooks have no records: not requested until requested; no migration | `tst_result_columns::oldIndexWithoutStamp`, `noRecordStaysUnavailableAfterRestart`; `tst_fusion_jobs::columnOnFusionOutputIsCachedFromRecord` |
| 333 | 7 | publish, restore and record writes on the main thread; the engine's threading rules unchanged | `tst_result_store::writesOnOkInstall`; `audit one-worker`, `audit stored-results` |
| 334 | 7, as amended | the store reads a record only for a session being loaded, or for the column worker's temporary copy of an unloaded session when a missing column needs it (a load for reading, never for writing), and never starts a calculation | `tst_result_store::temporaryLoadsReadRecordsOnlyForColumns`; `tst_result_columns::workerRestoresStoredResult`, `onDemandColumnsReadNoRecord`, `restartShowsCachedValueWithoutLoading`; `tst_fusion_jobs::workerRefillsColumnFromStoredFit`; `audit stored-results` |
| 335 | 7 | purity: with or without a record, every reader sees the same value | `tst_calcengine_restore::restoreIntoFreshEngine`; `tst_fusion_session::restoredFitIsIndistinguishable`; `tst_fusion_store::fittedBeforeFirstSaveIsRestored` |
| 336 | 7 | saving a session with a stored result gives the same bytes as without | `tst_fusion_store::sessionFileBytesUnaffectedByRecord`; `tst_result_records::saveSessionIgnoresRecords` |
| 337 | 8, as amended | test: a fusion fixture fitted, saved, unloaded, reloaded: thirty-three channels (the seventeen of the state, since the specification of 1001-1065 the four accuracies and since that of 1701-1717 the twelve covariance entries) and diagnostics bit-identical to the goldens, no job, nothing counted in progress and nothing listed among the failures | `tst_fusion_store::restoredAfterEvictionIsBitIdentical` |
| 338 | 8 | the same after an application restart | `tst_fusion_store::restoredAfterRestartIsBitIdentical` |
| 339 | 8, as amended | test: a rejection and a solver failure restored with their reason, listed among the failures and not tried again at the next start; no job | `tst_fusion_store::restoredRejectionShowsBadge`, `restoredSolverFailureShowsBadge` |
| 340 | 8 | an unrelated edit keeps the record; merging IMU data or changing any dependency drops it, reads not requested, the file is gone | `tst_fusion_store::unrelatedEditKeepsRecord`, `dependencyEditDropsRecord`, `mergeIntoLoadedSessionDropsRecord`, `mergeIntoUnloadedSession`; `tst_result_store::inputChangeDeletesRecord` |
| 341 | 8, as amended | test: bumping the compatibility version, the result version, or the result version recorded for a calculation its lookups went through drops the record on load | `tst_fusion_store::codeStampChangeDropsRecordOnLoad`; `tst_result_store::staleRecordDeletedOnLoad` |
| 342 | 8 | a failed write leaves the in-memory result usable and the previous record intact | `tst_result_store::writeFailureLeavesResultUsable`, `writeFailureKeepsPreviousRecord` |
| 343 | 8 | deleting a session removes its records; a stray record is ignored and removed by the next scan | `tst_result_store::deletingSessionRemovesRecords`, `strayRecordRemovedAtRestart`; `tst_result_records::removeSessionDeletesRecords`, `strayRecordsRemovedAtScan` |
| 344 | 8 | a logbook column over Fusion/roll is cached from a valid record, unavailable without one, invalidated when the record is dropped | `tst_fusion_jobs::columnOnFusionOutputIsCachedFromRecord`; `tst_result_columns::publishedResultIsCached`, `unrequestedIsCachedUnavailable`, `inputChangeDropsCachedValue`, `staleRecordOnLoadDropsCachedValue`, `deletingSessionRemovesStamp` |
| 345 | 8 | saving a session with a stored result produces the same session-file bytes as without | `tst_fusion_store::sessionFileBytesUnaffectedByRecord` |
| 346 | 9 | docs/ describe the record files and amend DATA_SCHEMA 11, CALCULATIONS 8 / 9 / 12 / 15, SENSOR_FUSION 2 / 7 | `audit stored-results` |
| 347 | 10 | a requested result is kept as long as its inputs and its code are the same, and not a moment longer | `tst_fusion_store::unrelatedEditKeepsRecord`, `dependencyEditDropsRecord`, `codeStampChangeDropsRecordOnLoad` |
| 348 | 10 | the session file is the recording; derived data lives beside it, never in it | `tst_fusion_store::sessionFileBytesUnaffectedByRecord`; `audit stored-results` |
| 349 | 10 | restoring is not requesting: nothing starts on its own | `tst_calcengine_restore::restoreIsNotAnInstall`; `tst_result_store::restoreOnEveryLoadPath`; `tst_fusion_store::restoredAfterRestartIsBitIdentical`; `audit stored-results`; `manual M17` |
| 350 | 10 | a restored result is indistinguishable from a fresh one, to the bit | `tst_fusion_session::restoredFitIsIndistinguishable`; `tst_fusion_store::restoredAfterEvictionIsBitIdentical`; `tst_calcengine_restore::restoreIntoFreshEngine` |

### 9.5 Stored results: validity that mirrors memory (items 401-442)

The forty-two clauses of the specification "Stored results: validity that
mirrors memory", stated in full in
[appendix E](#appendix-e-the-acceptance-items-of-stored-result-validity-401-442).
In the map, item = 400 + the clause number; the same four line forms as 9.2,
and every item has at least one test or audit line. "Section" is the section
of the specification; its section 1 (motivation) has no item. Clauses 30-38
are its section 10 tests, one per bullet, and clauses 40-42 its principles.
The specification amends the one of 9.4. Its section 9.1 (logbook columns
over stored results) and the section 10 test of it, added later, are filed
under clause 27, stated as amended.

| # | Section | Clause | Evidence |
|---|---|---|---|
| 401 | 2 | a stored result goes stale exactly when the same result in memory would be dropped, or when the code that computed it changes; nothing unrelated to what it reached makes it stale | `tst_fusion_store::storedFitSurvivesUnrelatedChanges`, `runtimeRegistryChangeDropsFitAndRecord`, `codeStampChangeDropsRecordOnLoad`; `tst_result_store::recordSurvivesUnrelatedChanges`, `registryChangeDeletesRecord`; `tst_calcengine_restore::registryChangeMirrorsRestore`, `publishAcrossPassedOverRemovalMirrorsRestore` |
| 402 | 3.1, 3.2 | in memory a requested result is dropped when a value it reached changes (samples, unit text, a stored attribute, directly or through calculations and conversions, a missing value that appears) or a declared preference it reached changes | `tst_calcengine_restore::droppedByInputChange`, `droppedByPreferenceAndSource`, `droppedByRequestOfUpstream`; `tst_fusion_store::dependencyEditDropsRecord` |
| 403 | 3.3, as settled | it is dropped when a registry change touches a name it resolved, directly or transitively, so that its answer can change: a calculation or family that can now answer it added, the calculation whose result it used removed, the source-conversion layer changed under recorded data; a candidate behind the session's data or behind the provider, or a removed candidate that was passed over, changes nothing | `tst_calcengine_restore::droppedByRegistryChange`, `registryChangeMirrorsRestore`; `tst_calcengine_oracle::registryChangePrecision`; `tst_builtins_engine::altitudeMarkerTeardownReportsNothing`; `tst_fusion_store::runtimeRegistryChangeDropsFitAndRecord` |
| 404 | 3 | everything else leaves it installed: edits to values it did not reach, registrations of names it never looked up (altitude markers, unrelated plug-in outputs), preferences it did not reach | `tst_calcengine_restore::droppedByRegistryChange`, `registryChangeMirrorsRestore`; `tst_calcengine_oracle::registryChangePrecision`; `tst_fusion_store::storedFitSurvivesUnrelatedChanges`, `unrelatedEditKeepsRecord`; `tst_result_store::recordSurvivesUnrelatedChanges`, `noDeleteWithoutInputChange`; `manual M20` |
| 405 | 4.1 | inputs, unchanged: the fingerprint over every leaf the result reached, present or absent, declared preference values included, matches the session and preferences as they are now | `tst_calcengine_restore::fingerprintKnownAnswer`, `exportLeaves`, `restoreStaleChecks`; `tst_fusion_store::dependencyEditDropsRecord` |
| 406 | 4.2 | the record states, for every name the result looked up directly or transitively, what provided it: a calculation (instance id and result version), the session's own data, or nothing | `tst_calcengine_restore::resolutionCodes`, `exportResolutions`; `tst_fusion_session::restoredFitIsIndistinguishable`; `tst_result_records::layoutIsPinned`, `roundTripIsBitExact`, `corruptInputIsRefused` |
| 407 | 4.2 | at load the same lookups are repeated against the current registry and must give the same answers | `tst_calcengine_restore::restoreAcrossRegistries`, `restoreStaleChecks`, `ringIsNeverStored`; `tst_result_store::lookupResolvingDifferentlyDeletesRecord`; `tst_fusion_store::lookupResolvingDifferentlyAtLoadDeletesFit` |
| 408 | 4.3 | code: CalculationCompatibilityVersion and the calculation's own result version are unchanged | `tst_result_records::stampsAreCurrent`; `tst_result_store::staleRecordDeletedOnLoad`; `tst_fusion_store::codeStampChangeDropsRecordOnLoad`; `tst_calcengine_restore::restoreStaleChecks` |
| 409 | 4 | the calculation environment fingerprint is no longer part of a record | `tst_result_records::stampsAreCurrent`, `layoutIsPinned`; `tst_result_store::recordSurvivesUnrelatedChanges`; `audit result-validity` |
| 410 | 4 | the bump-rule clause reads: bump CalculationCompatibilityVersion, or the result version of the calculation concerned, whenever a change can alter what a requested calculation or anything it reads produces | `audit result-validity`, `audit stored-results` |
| 411 | 4 | a record that fails any check is deleted when its session is loaded and the calculation reads not requested; nothing is recomputed | `tst_result_store::staleRecordDeletedOnLoad`, `lookupResolvingDifferentlyDeletesRecord`, `pluginEditStalesRecordsThatReadIt`, `formatOneRecordIsDeletedOnLoad`, `temporaryLoadsReadRecordsOnlyForColumns`; `tst_fusion_store::lookupResolvingDifferentlyAtLoadDeletesFit`; `tst_result_columns::staleRecordOnLoadDropsCachedValue` |
| 412 | 5 | a registry change made while the application runs that drops an installed requested result also deletes its record, like an input change | `tst_result_store::registryChangeDeletesRecord`; `tst_fusion_store::runtimeRegistryChangeDropsFitAndRecord`; `tst_calcengine_restore::droppedByRegistryChange`, `registryChangeMirrorsRestore`, `publishAcrossPassedOverRemovalMirrorsRestore`; `tst_result_columns::registryChangeKeepsLoadedRowConfirmed` |
| 413 | 5 | tearing the registry down at shutdown, destroying or evicting a session, and replacing a session's contents without an input change delete nothing | `tst_calcengine_restore::noDropEventWithoutInputChange`, `registryDestructionReportsNothing`; `tst_builtins_engine::altitudeMarkerTeardownReportsNothing`; `tst_result_store::registryChangeDeletesRecord`, `noDeleteWithoutInputChange`; `audit result-validity` |
| 414 | 5 | a registry change that does not touch a result leaves the installed result and its record alone, and column values over that record remain cacheable | `tst_result_store::recordSurvivesUnrelatedChanges`, `noDeleteWithoutInputChange`; `tst_result_columns::registryChangeKeepsLoadedRowConfirmed`, `environmentChangeDiscardsReachedColumn`; `tst_fusion_store::storedFitSurvivesUnrelatedChanges`; `tst_fusion_jobs::altitudeMarkerKeepsColumnsOfUnloadedSession`; `tst_calcengine_restore::droppedByRegistryChange`, `registryChangeMirrorsRestore` |
| 415 | 6 | every calculation, measurement and attribute a plug-in registers declares a result version: the plug-in code identity | `tst_python_bridge::pluginRegistrationsCarryCodeIdentity`; `audit result-validity` |
| 416 | 6, as settled | the identity is one digest over every `.py` file under the plug-in folder, subfolders included (relative name and bytes, in name order; hidden files counted, `__pycache__` and hidden folders left out, linked folders read through with loops ended), the SDK file, and the Python and numpy versions | `tst_plugin_identity::encodingIsPinned`, `eachIngredientChangesIdentity`, `absentVersionUsesFixedToken`, `readsTheFolderRecursively`, `subfolderFileChangesIdentity`, `pycacheAndHiddenDirectoriesAreIgnored`, `hiddenFilesCount`, `linkedFoldersAreReadThrough`, `unreadableFileIsNotEmpty`; `tst_python_bridge::pluginRegistrationsCarryCodeIdentity` |
| 417 | 6 | it is computed once, when the plug-ins are loaded | `tst_python_bridge::pluginRegistrationsCarryCodeIdentity`, `secondInitialiseIsNoOp`; `audit result-validity` |
| 418 | 6 | editing any plug-in file, adding or removing one, or upgrading Python or numpy changes it, so a stored result whose lookups went through any plug-in calculation goes stale at its next load | `tst_result_store::pluginEditStalesRecordsThatReadIt`; `tst_plugin_identity::eachIngredientChangesIdentity`, `subfolderFileChangesIdentity` |
| 419 | 6 | a stored result whose lookups touched no plug-in calculation is unaffected by plug-in changes | `tst_result_store::pluginEditStalesRecordsThatReadIt`; `tst_fusion_store::storedFitSurvivesUnrelatedChanges`; `manual M21` |
| 420 | 6, as amended | the calculation environment of the logbook column cache also covers every registration's result version: a plug-in edit or a built-in result-version change discards cached column values; amended: the environment is one per column (what the column's static closure can observe), so a change discards only the columns whose environment it changes | `tst_builtins_engine::digestCoversResultVersions`, `digestChanges`, `digestCoversConversionLayer`, `digestSurvivesRuntimeAltitudeMarker`; `tst_column_cache::pluginEditDiscardsPluginColumns`, `altitudeMarkerChangeKeepsOtherColumns`, `environmentCheckDropsExactlyTheReachedColumn`, `preferenceChangeDiscardsOnlyReadingColumns`, `valueComputedBeforeCheckIsStoredUnderItsEnvironment`; `tst_logbook_index::differentColumnEnvironmentDiscardsThatColumn`, `missingColumnEnvironmentsDiscardOnce`, `environmentIsTheCachedOne`; `tst_fusion_jobs::altitudeMarkerKeepsColumnsOfUnloadedSession`; `manual M21` |
| 421 | 7 | a record that exists but cannot be opened or read in full at its session's load is skipped for that load: neither restored nor deleted; the calculation reads not requested | `tst_result_store::unreadableRecordIsSkipped`, `dependentOfSkippedRecordIsKept`, `restoresUpstreamFirst`; `tst_result_records::shortReadIsUnreadable`; `manual M22` |
| 422 | 7 | the next load tries again; a new publish for the pair replaces it; deleting the session or the stray pass at start-up removes it | `tst_result_store::unreadableRecordIsSkipped`, `skippedRecordIsReplacedByPublish`, `deletingSessionWithSkippedRecord`; `tst_result_columns::skippedRecordValuesStayOutOfIndex`, `workerSkipsUnreadableRecord`; `tst_result_records::writeReadReplace`, `removeSessionDeletesRecords`, `strayRecordsRemovedAtScan`; `manual M22` |
| 423 | 7 | a record that was read but is not a record, is damaged, or has a format version this build does not read is deleted as stale | `tst_result_store::staleRecordDeletedOnLoad`, `formatOneRecordIsDeletedOnLoad`; `tst_result_records::readStatuses`, `corruptInputIsRefused` |
| 424 | 7 | logbook column values of a session that depend on a skipped record are not cached in index.json while it stays skipped | `tst_result_columns::skippedRecordValuesStayOutOfIndex`, `workerSkipsUnreadableRecord`; `tst_result_store::unreadableRecordIsSkipped`, `skippedRecordIsReplacedByPublish` |
| 425 | 8 | the record gains the resolutions and loses the environment fingerprint, and its format version increases | `tst_result_records::layoutIsPinned`, `futureVersionIsRefused`, `formatOneIsRefused`, `stampsAreCurrent` |
| 426 | 8 | a record of an earlier format version is deleted as stale when its session loads; no migration | `tst_result_store::formatOneRecordIsDeletedOnLoad`; `tst_result_records::formatOneIsRefused` |
| 427 | 9, as amended by 9.1 | the amended specification still holds where not amended: restoring is not requesting, publishing writes the record, records are read only for a session being loaded or for the column worker's temporary copy (a load for reading, never for writing; it never requests or runs a requested calculation), the session file is untouched, a restored result is bit-identical; a logbook column's value is the same function of the session's valid stored results whether or not it is loaded (and the section 10 test: refilled by the worker from the stored result after its cached value is discarded, with no load of the row and no job) | `tst_calcengine_restore::restoreIsNotAnInstall`; `tst_result_store::restoreOnEveryLoadPath`, `temporaryLoadsReadRecordsOnlyForColumns`; `tst_result_columns::workerRestoresStoredResult`, `staleRecordOnLoadDropsCachedValue`, `workerSkipsUnreadableRecord`, `onDemandColumnsReadNoRecord`; `tst_fusion_jobs::workerRefillsColumnFromStoredFit`; `tst_fusion_store::sessionFileBytesUnaffectedByRecord`, `restoredAfterRestartIsBitIdentical`; `audit stored-results` |
| 428 | 9 | the engine keeps its threading rules; repeating the lookups at load uses the resolution of a fresh request and never runs a requested calculation | `tst_calcengine_restore::restoreAcrossRegistries`, `restoreIsNotAnInstall`; `tst_result_store::lookupResolvingDifferentlyDeletesRecord`; `audit one-worker`, `audit stored-results` |
| 429 | 9 | plug-in loading stays a start-up operation: nothing reloads plug-ins or watches their files | `tst_python_bridge::secondInitialiseIsNoOp`; `audit result-validity` |
| 430 | 10 | test: a stored fit survives, restored with no job, adding and removing an altitude marker, registering and unregistering a calculation it never looks up, the descent-pause preference, another plug-in set, and a restart after any of these | `tst_fusion_store::storedFitSurvivesUnrelatedChanges`; `tst_result_store::recordSurvivesUnrelatedChanges`; `manual M20` |
| 431 | 10, as settled | test: a registry change made while the application runs that changes what a name the fit looked up resolves to (registering a calculation that provides a name it looked up and found missing, removing the provider of one) drops the installed fit and deletes its record; a candidate registered behind the provider does not | `tst_fusion_store::runtimeRegistryChangeDropsFitAndRecord`; `tst_result_store::registryChangeDeletesRecord` |
| 432 | 10 | test: a record whose lookups resolve differently at load (a new candidate with the same inputs, registered before the load) is deleted and the fit reads not requested | `tst_fusion_store::lookupResolvingDifferentlyAtLoadDeletesFit`; `tst_result_store::lookupResolvingDifferentlyDeletesRecord`; `tst_calcengine_restore::restoreAcrossRegistries` |
| 433 | 10 | test: with a requested calculation that reads a plug-in output, editing any plug-in file, adding one, or changing the Python or numpy version makes its record stale; a plug-in edit does not when it reads no plug-in output | `tst_result_store::pluginEditStalesRecordsThatReadIt` |
| 434 | 10 | test: the plug-in code identity is deterministic, and each listed ingredient changes it | `tst_plugin_identity::identityIsDeterministic`, `encodingIsPinned`, `eachIngredientChangesIdentity`, `absentVersionUsesFixedToken` |
| 435 | 10 | test: a plug-in edit discards cached logbook column values over plug-in calculations at the next start | `tst_column_cache::pluginEditDiscardsPluginColumns` |
| 436 | 10 | test: an unreadable record (held open without sharing on Windows, a directory at its path) is kept, not restored, and restored at a later load; its dependent column values are not cached meanwhile | `tst_result_store::unreadableRecordIsSkipped`, `dependentOfSkippedRecordIsKept`, `restoresUpstreamFirst`; `tst_result_columns::skippedRecordValuesStayOutOfIndex`, `workerSkipsUnreadableRecord` |
| 437 | 10 | test: a record of the previous format version is deleted as stale | `tst_result_store::formatOneRecordIsDeletedOnLoad`; `tst_result_records::formatOneIsRefused` |
| 438 | 10 | test: the existing tests of stored results pass, those that asserted environment staleness rewritten to these rules | `tst_result_store::staleRecordDeletedOnLoad`; `tst_fusion_store::codeStampChangeDropsRecordOnLoad`; `tst_result_columns::environmentChangeDiscardsReachedColumn`, `writeAfterStartupDropFlushesIndexFirst`, `registryChangeKeepsLoadedRowConfirmed` |
| 439 | 11 | docs/ and the plug-in README describe validity, the resolutions, the format version, unreadable records, column values over skipped records, the fingerprint covering result versions, the bump rule and the plug-in code identity; no note says unrelated changes make stored results stale | `audit result-validity` |
| 440 | 12 | a stored result is a memory of an in-memory result: stale when that one would be dropped, and when the code changes, and at no other time | `tst_fusion_store::storedFitSurvivesUnrelatedChanges`, `runtimeRegistryChangeDropsFitAndRecord`, `codeStampChangeDropsRecordOnLoad` |
| 441 | 12 | what a result reached decides its validity, never what else is registered | `tst_calcengine_restore::restoreAcrossRegistries`; `tst_result_store::recordSurvivesUnrelatedChanges`; `audit result-validity` |
| 442 | 12 | a transient failure to read is not evidence that a record is wrong | `tst_result_store::unreadableRecordIsSkipped`, `dependentOfSkippedRecordIsKept`, `restoresUpstreamFirst`; `tst_result_records::shortReadIsUnreadable`; `manual M22` |

### 9.6 Demand-driven requested calculations (items 501-563)

The sixty-three clauses of the specification "Demand-driven requested
calculations", stated in full in
[appendix F](#appendix-f-the-acceptance-items-of-demand-driven-requested-calculations-501-563).
In the map, item = 500 + the clause number; the same four line forms as 9.2,
and every item has at least one test or audit line. "Section" is the section
of the specification; its sections 1 (motivation) and 4 (terms) have no item.
Clauses 49-62 are its section 13 tests, one per bullet, and clause 63 its
section 14. The specification amends those of 9.2 (items 110, 111, 113-117)
and 9.4 (items 304, 306, 321, 337). Clauses 13, 26, 28, 30, 32, 35, 36, 37,
39, 46, 47, 57 and 59 are stated as amended by the specification "Calculation
refinements" (9.7). Clauses 17, 25, 26, 30, 35-37, 39, 42, 54, 55, 57, 59 and
63 are stated as amended by the specification "One status bar for background
work" (9.8), which restates them again where 9.7 had. Clause 9 is stated as
amended by the specification "Sensor fusion plots, attitude and the
orientation attribute" (9.9), again by the specification "The documented
noise model and the accuracy, part 1" (9.11), which made the fusion plots
twelve, five of them outputs of the fit, and again by the specification
"Fused speed plots" (9.17): the fusion plots are fifteen, six of them outputs
of the fit.

| # | Section | Clause | Evidence |
|---|---|---|---|
| 501 | 2 | the user expresses intent through plots and logbook columns, never through calculations: what is switched on is the request | `tst_calculation_demand::rowScript`, `programmaticCheckCreatesDemand`, `enablingColumnFillsEveryUnloadedSession`; `tst_fusion_rows::realRowScript`; `audit gestures`; `manual M3` |
| 502 | 2 | anything needed to complete what is switched on is wanted at once; anything no longer needed is dropped | `tst_calculation_demand::showingASessionStartsIt`, `hidingASessionDropsItsWaitingPair`, `uncheckingDropsWaitingPairsKeepsRunning`, `disablingColumnReleasesHeldSessions`, `changingDemandReplacesChosenNext` |
| 503 | 2 | finished work is never wasted: results are stored | `tst_calculation_demand::uncheckingDropsWaitingPairsKeepsRunning`, `disablingColumnReleasesHeldSessions`, `storedResultsCreateNoJob`; `tst_fusion_store::fusionColumnWithStoredFitsRunsNothing` |
| 504 | 2 | background work must not degrade the rest of the application | `tst_jobqueue::workerRunsBelowNormalPriority`, `mainThreadIsNotBlockedByARunningJob`, `idleSchedulerKeepsWorking`; `tst_calculation_demand::passOverManyStubsReadsEachRecordSetOnce`, `enablingColumnFillsEveryUnloadedSession`; `manual M27` |
| 505 | 2 | a failure is shown, never retried in a loop | `tst_calculation_demand::inputDeterminedFailureIsStoredBadgedNeverRerun`, `jobLevelFailureIsBadgedNotRerunUntilRestart`, `columnFailuresAreBadgedNotReloaded`, `notApplicableSessionIsSettledWithoutAJob`, `unloadableSessionIsSettledAsFailed`, `columnOfferRefusalIsNotLeftPending` |
| 506 | 2 | each component's contract can be stated without naming the others (section 12 gives the contracts) | `tst_calculation_demand::nullCollaborators`; `tst_result_columns::columnWorkerIsUnchangedByDemand`; `tst_session_model_engine::schedulerTaskCanBeUnregistered`; `audit demand` |
| 507 | 3 | the fusion kernel, its outputs and the record format are unchanged; the job history the jobs dock will read stays as it is | `tst_fusion_golden::successFixturesMatchGolden`; `tst_result_records::layoutIsPinned`; `tst_jobmodel::historyFromSignalsAlone`; `tst_fusion_store::restoredAfterRestartIsBitIdentical` |
| 508 | 3 | nothing about demand or the job history is persisted across restarts, and no user-facing switch pauses or throttles background work | `tst_jobmodel::nothingIsPersisted`; `tst_calculation_demand::jobLevelFailureIsBadgedNotRerunUntilRestart`, `fillTaskIsLowestAndNotCancellable`; `audit demand` |
| 509 | 5, as amended | plot demand: for every checked plot whose value is a requested output, every visible session needs the requested calculations that block that output; the fifteen plots of the "Sensor fusion" category are such plots: six are outputs of the fit (vertical acceleration, the four accuracies since the specification of 1001-1065, and vertical speed since the specification of 1601-1612), and the nine derived from its outputs are blocked by it | `tst_calculation_demand::rowScript`, `ordinaryPlotsAreNeverInspected`, `hiddenAndStubRowsAreNotTracks`, `failedLoadPlaceholderIsNotATrack`, `plotIdMatchesPlotModelRole`, `uncheckedPlotsAreNeverInspected`; `tst_fusion_rows::allFusionPlotsAreExplicitBacked` |
| 510 | 5 | column demand: for every enabled logbook column whose value depends on a requested output, every session in the logbook needs the requested calculations that block that value | `tst_calculation_demand::enablingColumnFillsEveryUnloadedSession`, `loadedHiddenSessionsNeedNoLoad`, `ordinaryColumnsCreateNoDemand`, `columnIdIsTheDefinitionKey`; `tst_fusion_store::columnOverFusionFillsUnloadedSessions`; `manual M24` |
| 511 | 5 | a pair is in demand only while it has no result; a result counts whether published in this run or restored, success or input-determined failure | `tst_calculation_demand::onlyRequestableCalculationsAreOffered`, `storedResultsCreateNoJob`, `inputDeterminedFailureIsStoredBadgedNeverRerun`, `columnFailuresAreBadgedNotReloaded`; `tst_fusion_store::restoredRejectionShowsBadge`, `restoredAfterRestartIsBitIdentical` |
| 512 | 5 | a pair whose calculation cannot apply (a declared input missing) is never in demand and is not reported anywhere | `tst_calculation_demand::sessionWithoutInputIsNeverListed`, `notApplicableSessionIsSettledWithoutAJob`; `tst_jobqueue::refusesMissingInput`; `tst_fusion_rows::noImuSessionIsNeverCounted`; `tst_fusion_store::columnOverFusionFillsUnloadedSessions` |
| 513 | 5, as amended | whether a result exists: blocker inspection for a loaded session; for a session not loaded, first what this run remembers of its pairs (a failure; a not-applicable verdict of that column), then the logbook's record names and the reasons the index recorded for them, no record opened; a cell has a result only when every calculation it needs has a record; a record with a reason is a failed result before and after a restart alike; a reason the index learns later is announced as a record change; a known record counts until the column worker's restore deletes it as stale, which moves the pair into demand; the demand layer checks no staleness itself | `tst_calculation_demand::storedResultsCreateNoJob`, `chainedColumnWithUpstreamRecordIsCompleted`, `storedRejectionIsBadgedAfterRestartWithoutLoad`, `passOverManyStubsReadsEachRecordSetOnce`, `columnProgressAndPendingCells`, `recordReasonReachesDemandThroughRecordChange`, `failedRecordWriteIsShownAndNotRetried`; `tst_logbook_index::recordReasonsRoundTrip`, `recordReasonChangeIsAnnounced`; `tst_result_store::recordReasonRecordedAtWriteAndRestore`; `tst_result_columns::staleRecordDeletedByWorkerCreatesDemand`; `audit demand` |
| 514 | 5 | demand does not depend on how the state arose (a gesture, a profile, the start-up restore); at start-up every session is hidden, so plots create no demand until sessions are shown, while enabled columns do | `tst_calculation_demand::programmaticCheckCreatesDemand`, `profileStyleApplyCreatesDemand`, `startupRestoreWithHiddenSessionsStartsNothing`, `profileStyleColumnsCreateDemand`, `startupWithEnabledColumnLoadsAfterColumnWorker`; `manual M1`, `M2` |
| 515 | 5 | nothing about demand is persisted: it is derived again at the next start; a profile carrying a column over a requested output computes it for every session without a result, so no default profile carries such a column | `tst_calculation_demand::jobLevelFailureIsBadgedNotRerunUntilRestart`, `profileStyleColumnsCreateDemand`; `audit demand`; `manual M2` |
| 516 | 5 | when a requested calculation depends on another, the demand covers both, upstream first | `tst_calculation_demand::chainedBlockersContinue`, `heldChainContinues`, `chainCompletesAfterFirstJobDoesNotSucceed`, `chainedColumnKeepsItsHold`; `tst_calcengine_blockers::chainedBlockers` |
| 517 | 6, as amended | a pair that enters demand is wanted at once, with no gesture (checking, showing, enabling, an input change that drops a demanded result), and progress counts its session from the first change | `tst_calculation_demand::showingASessionStartsIt`, `loadingAVisibleSessionStartsIt`, `mergeCreatesDemandForShownSessions`, `plotCheckedDuringAJobJoinsIt`, `enablingColumnFillsEveryUnloadedSession`, `inputBurstRunsOneJob`, `programmaticCheckCreatesDemand` |
| 518 | 6 | a pair that leaves demand (plot unchecked, session hidden, column disabled, result appeared by other means) is dropped before it starts; there is no queue of accepted requests to prune | `tst_calculation_demand::hidingASessionDropsItsWaitingPair`, `uncheckingDropsWaitingPairsKeepsRunning`, `waitingPairNeededByAnotherPlotSurvives`, `chainStopsForHiddenTrackOrUncheckedPlot`, `disablingColumnReleasesHeldSessions`, `onlyRequestableCalculationsAreOffered`, `resultAppearingWhileWaitingDropsThePair`; `tst_jobqueue::withdrawEndsChosenNext`; `audit demand`; `manual M6`, `M26` |
| 519 | 6, as amended | the running job is never stopped because its pair left demand: it finishes and its result is published and stored; it is stopped only when its inputs change, its session goes away, its recording is switched off for background computation (amended by the specification of 1501-1524), or the application closes | `tst_calculation_demand::switchingOffCancelsTheRunningJob`, `uncheckingDropsWaitingPairsKeepsRunning`, `hidingASessionDropsItsWaitingPair`, `disablingColumnReleasesHeldSessions`, `removedSessionLeavesNoTrace`; `tst_jobqueue::staleRunningJobIsStoppedAtOnce`, `removeSessionWithRunningJob`, `shutdownWithQueuedAndRunning`; `tst_fusion_rows::realRowScript`; `audit gestures`; `manual M6` |
| 520 | 6 | after an input change of a demanded session the pair is in demand at once but starts only once the inputs have been still for a short moment, so a burst of edits runs one job; showing, hiding, checking and enabling take effect without the wait | `tst_calculation_demand::inputBurstRunsOneJob`, `staleRunningJobIsWaitingAtOnce`, `supersededJobIsRunAgainAfterInputsSettle`, `dependencyBurstIsCoalesced`, `mergeCreatesDemandForShownSessions`; `tst_fusion_rows::rejectedTrackShowsBadge`; `manual M18` |
| 521 | 6 | jobs run one at a time on the executor's worker thread | `tst_jobqueue::oneAtATimeInOfferOrder`, `holdsAtMostRunningAndChosenNext`, `workerIsNotMainThreadAndHasLargeStack`; `tst_jobmodel::neverMoreThanOneRunningRow`; `audit one-worker` |
| 522 | 7 | the next job is chosen when a job ends and whenever demand changes, from the demand as it is at that moment, not from the order in which pairs entered it | `tst_calculation_demand::changingDemandReplacesChosenNext`, `focusedSessionFirstThenRowOrder`, `columnPriorityFollowsRowOrderAfterPlots`; `tst_jobqueue::offerReplacesChosenNext` |
| 523 | 7 | plot demand first (the focused session, then the other visible sessions in logbook row order), then column demand: visible sessions, then the other loaded sessions, then sessions not loaded as the fill loads them, each in logbook row order | `tst_calculation_demand::focusedSessionFirstThenRowOrder`, `visibleSessionsFirstWithinColumnDemand`, `columnPriorityFollowsRowOrderAfterPlots`, `enablingColumnFillsEveryUnloadedSession` |
| 524 | 7 | a session made visible while column demand is worked through is computed next, waiting at most for the running job, which is not preempted | `tst_calculation_demand::sessionShownDuringColumnDemandRunsNext`, `columnPriorityFollowsRowOrderAfterPlots`; `manual M26` |
| 525 | 8, as amended | an input-determined failure (rejection, solver failure) is a result: stored, listed among the failures with its reason, never run again | `tst_calculation_demand::inputDeterminedFailureIsStoredBadgedNeverRerun`, `columnFailuresAreBadgedNotReloaded`; `tst_fusion_rows::rejectedTrackShowsBadge`; `tst_fusion_store::restoredRejectionShowsBadge`, `restoredSolverFailureShowsBadge`; `manual M7` |
| 526 | 8, as amended | a failure that is not a function of the inputs (the worker could not start, out of memory, a session file that could not be loaded, a result whose record could not be written) is listed among the failures with its reason, marked as tried again at the next start, not offered and its session not loaded again in the run unless its inputs change, not stored, so tried again at the next start; the demand layer remembers it for the pair | `tst_calculation_demand::jobLevelFailureIsBadgedNotRerunUntilRestart`, `columnJobLevelFailureIsNotReloadedUntilRestart`, `unloadableSessionIsSettledAsFailed`, `visibleFailedLoadIsSettledAsFailed`, `failedRecordWriteIsShownAndNotRetried`, `settledPairsSurviveEvictionSortAndColumnWorker`, `pairMemoryIsClearedByRecordInputAndRegistryChanges`; `tst_logbook_indicators::failedLoadSessionShowsRowWarningNotPending`; `tst_jobqueue::workerStartFailureFails`, `resourceExhaustionFails`; `manual M28`, `M32` |
| 527 | 8 | there is no retry control | `tst_logbook_indicators::clickOnRowWarningIsAClickOnTheCell`; `audit gestures`, `audit demand` |
| 528 | 9, as amended | for column demand the demand layer, not the column worker, has a session that is not loaded loaded the way showing it would, without making it visible: an ordinary hidden session, pinned from its load until no column it needs is still waiting or running, the executor is shut down or the demand layer goes, then left to ordinary eviction | `tst_calculation_demand::enablingColumnFillsEveryUnloadedSession`, `chainedColumnKeepsItsHold`, `heldSessionShownStaysLoaded`, `removedOrRepopulatedHeldSessionIsReleased`, `demandDestroyedReleasesHoldsAndTask`, `noLoadsAfterExecutorShutdown`; `tst_column_cache::loadPinnedSessionLoadsWithoutShowing`, `loadPinnedSessionFailedLoadPinsNothing` |
| 529 | 9 | at most a small fixed number of sessions is loaded for this purpose at a time, not smaller than the number of jobs that may run at once; the next is loaded when one has ended | `tst_calculation_demand::enablingColumnFillsEveryUnloadedSession`, `sessionShownDuringColumnDemandRunsNext`, `columnPriorityFollowsRowOrderAfterPlots`; `audit demand`; `manual M24` |
| 530 | 9, as amended | the fill is an idle-scheduler task below saving, loading visible sessions, bulk edits and column work, whose steps are the loads, so saves and bulk edits come first; it has work for the whole fill and steps only while it can load; it reports no progress of its own, and the status bar shows the computations while it works; the scheduler has the generic notion of a task with work it cannot step right now, rests instead of spinning, completes such a task once its work is gone without a step, and learns nothing about jobs | `tst_calculation_demand::savesAndBulkEditsPrecedeLoadStep`, `startupWithEnabledColumnLoadsAfterColumnWorker`, `fillTaskIsLowestAndNotCancellable`, `fillTaskRestsWhileWaiting`, `bulkEditMakesSettledSessionApplicable`, `fillEndingBehindAnotherTaskStartsNextCountFresh`; `tst_session_model_engine::schedulerWaitingTaskDoesNotSpin`, `schedulerTaskCanBeUnregistered`, `schedulerCompletesWaitingTaskWhoseWorkIsGone`; `audit demand`; `manual M24`; `tst_status_bar::computationsAreOneItem` |
| 531 | 9 | the session-id correction of a first load happens before the pair is offered to the executor | `tst_calculation_demand::identityStubIsOfferedUnderItsRealId`; `tst_column_cache::loadPinnedSessionFollowsIdentityRemap` |
| 532 | 9, as amended | a session whose requested calculation turns out not to apply once loaded is remembered as not applicable for that column for the run, without a job, and is not loaded again for it; its column value stays unavailable | `tst_calculation_demand::notApplicableSessionIsSettledWithoutAJob`, `enablingColumnFillsEveryUnloadedSession`, `columnOfferRefusalIsNotLeftPending`, `columnVerdictDoesNotSuppressAnotherColumn`, `settledPairsSurviveEvictionSortAndColumnWorker`; `tst_fusion_store::columnOverFusionFillsUnloadedSessions` |
| 533 | 9 | the column worker is unchanged and never knows a job exists; a record written later drops the cached value and the loaded-row refresh computes the new one; cheap column values never wait for a requested calculation | `tst_result_columns::columnWorkerIsUnchangedByDemand`, `staleRecordDeletedByWorkerCreatesDemand`; `tst_calculation_demand::enablingColumnFillsEveryUnloadedSession`; `tst_logbook_indicators::pendingCellBecomesValueWhenRecordIsWritten`; `audit demand` |
| 534 | 10 | no refresh and no cancel for requested calculations anywhere; unchecking a plot, hiding sessions or disabling a column is how the user changes what is wanted | `tst_logbook_indicators::clickOnRowWarningIsAClickOnTheCell`; `tst_status_bar::cancelOnlyForACancellableShownTask`; `audit gestures`, `audit demand`; `manual M4`, `M6` |
| 535 | 10, as amended | plot rows and logbook column headers show nothing about computing: no indicator, no reserved room and no hover of their own, so they look as any other; progress is shown once, in the status bar, as "Computing results: k / n" | `tst_logbook_indicators::headerIsPlainAndNothingAnimates`, `plainHeaderAndCellsAreIdenticalToBase`; `audit demand`; `manual M3`, `M34`; `tst_status_bar::computationsAreOneItem` |
| 536 | 10, as amended | the status bar's hover carries the numbers (done of the high-water mark) and the recording being computed with its step; the warning's hover lists the recordings that could not be computed with their reasons, a result that could not be stored included | `tst_calculation_demand::failureTextAndItsLimit`, `sharedJobSameProgress`, `progressTextWithoutAPass`, `columnProgressAndPendingCells`, `visibleFailedLoadIsSettledAsFailed`; `tst_logbook_indicators::rowWarningHoverIsTheSessionsFailures`, `failedWriteIsListedInTheHover`; `tst_status_bar::taskShownOverComputationsAndHoverListsBoth`; `manual M34` |
| 537 | 10, as amended | the status bar's warning counts the recordings that could not be computed, a result that could not be stored included, as soon as a failure is current: beside the computations while they continue, alone once they end; its hover lists them with reasons | `tst_calculation_demand::failuresListedWhileWorking`, `failedRecordWriteIsShownOnThePlotRow`; `tst_logbook_indicators::rowWarningFollowsTheText`, `failedLoadSessionShowsRowWarningNotPending`, `failedWriteIsListedInTheHover`; `tst_status_bar::warningBesideComputationsThenAlone`; `tst_fusion_rows::rejectedTrackShowsBadge`; `manual M7`, `M36` |
| 538 | 10 | a column cell whose pair is in demand reads as pending, distinct from unavailable and from the unreadable-record pending state; pending is the view's presentation of demand, never a cached value, never in the index; the value underneath stays unavailable until the record is written, and sorting treats pending as unavailable | `tst_logbook_indicators::pendingCellsAreDistinctFromUnavailable`, `pendingCellBecomesValueWhenRecordIsWritten`, `sortingTreatsPendingAsUnavailable`, `unreadableRecordPendingIsNotDemandPending`, `pendingCellsChangeRepaintsOnlyThatColumn`; `tst_calculation_demand::columnProgressAndPendingCells`; `audit demand`; `manual M25` |
| 539 | 10, as amended | the status bar shows the fill as "Computing results: k / n", with no cancel button, distinct from the column worker's "Computing columns: k / n"; the fill reports no progress of its own | `tst_calculation_demand::fillTaskIsLowestAndNotCancellable`; `tst_status_bar::computationsAreOneItem`, `schedulerTasksShowTheirLabelsAndCounts`; `manual M24`, `M31` |
| 540 | 11 | the executor's worker thread runs below normal priority, so that the user interface stays responsive | `tst_jobqueue::workerRunsBelowNormalPriority`, `mainThreadIsNotBlockedByARunningJob`; `tst_fusion_jobs::solverThreadsRunAtWorkerPriority`; `audit demand`; `manual M27` |
| 541 | 11 | the number of jobs that may run at once is a single bound of the executor (one today); the load bound of section 9 follows it, and raising it changes no contract of the demand layer or the column worker | `tst_jobqueue::holdsAtMostRunningAndChosenNext`; `tst_calculation_demand::enablingColumnFillsEveryUnloadedSession`; `audit demand` |
| 542 | 12, as amended | one widget-free demand layer replaces the plot rows' request logic: it reads the plot model, the session model, the enabled columns, blocker reports and the record set, derives demand, applies the priority, has sessions loaded, offers the next pair, remembers this run's failures and not-applicable pairs, and publishes progress, failures and pending cells | `tst_calculation_demand::nullCollaborators`, `changeSignalsAreMinimal`, `registryChangeReclassifies`, `removedSessionLeavesNoTrace`; `audit widget-free-core`, `audit demand` |
| 543 | 12 | the demand layer is the only caller of the executor; nothing calls back into it: it observes the models and the executor's signals | `tst_calculation_demand::survivesExecutorShutdown`, `nullCollaborators`; `tst_logbook_indicators::survivesDemandDestroyedFirst`; `audit gestures`, `audit demand` |
| 544 | 12 | the executor stays the only place a requested calculation runs and never loads a session; it holds at most the running job and one chosen next job, with no order of arrival, no deduplication against a list and no pruning | `tst_jobqueue::holdsAtMostRunningAndChosenNext`, `offerReplacesChosenNext`, `withdrawEndsChosenNext`, `duplicateOffersCreateNoDuplicates`, `neverLoadsASession`; `tst_calculation_demand::executorHoldsAtMostRunningAndChosenNext`; `audit gestures`, `audit demand` |
| 545 | 12 | the executor's lifecycle (start, stale while running, cancel, supersede, fail, shutdown), its pinning, the publication and storing of results, and the job history are unchanged | `tst_jobqueue::runsAndPublishes`, `staleRunningJobIsStoppedAtOnce`, `offerWhileStaleJobWindsDown`, `cancelRunningThenNextStarts`, `evictionDeferredWhileJobActive`, `shutdownWithQueuedAndRunning`, `offerWhileCancellingCreatesNewJob`, `shutdownIsIdempotentAndRefusesOffers`; `tst_jobmodel::historyFromSignalsAlone`; `tst_result_store::writesOnOkInstall`; `tst_fusion_jobs::jobPublishesAllOutputsTogether` |
| 546 | 12, as amended | the column worker and the idle scheduler keep their tasks and priorities; the column fill is a new lowest-priority task; the scheduler has the notion of a task with work it cannot step right now, completes it when its work is gone, and knows nothing of jobs; the manager and the store record each stored result's outcome in the index, the manager announces a changed outcome, and the store announces a record it could not write | `tst_result_columns::columnWorkerIsUnchangedByDemand`; `tst_calculation_demand::fillTaskIsLowestAndNotCancellable`; `tst_session_model_engine::schedulerTaskCanBeUnregistered`, `schedulerWaitingTaskDoesNotSpin`, `schedulerCompletesWaitingTaskWhoseWorkIsGone`; `tst_result_store::recordReasonRecordedAtWriteAndRestore`, `writeFailureIsAnnounced`; `audit branch-mechanisms`, `audit demand`; `tst_logbook_index::recordReasonChangeIsAnnounced` |
| 547 | 12, as amended | the flow is one way: the demand layer chooses, the executor publishes, the listener writes the record or the store announces that it could not, the index notes the outcome and announces it, the record change drops cached column values and the demand layer's memos, the loaded-row refresh recomputes the values, and the demand layer sees the result or the failure through the inspection, the record set and the pair memory it always reads | `tst_result_columns::staleRecordDeletedByWorkerCreatesDemand`; `tst_calculation_demand::enablingColumnFillsEveryUnloadedSession`, `recordReasonReachesDemandThroughRecordChange`, `failedRecordWriteIsShownAndNotRetried`; `tst_logbook_indicators::pendingCellBecomesValueWhenRecordIsWritten`; `tst_fusion_store::columnOverFusionFillsUnloadedSessions`; `audit demand` |
| 548 | 12 | what stays true: restoring is not requesting; the column worker's temporary copy never writes a stored result and never requests; a restored result is indistinguishable from a published one; the engine's threading rules; the plot widget's "no data" warning asks the engine directly | `tst_calculation_demand::storedResultsCreateNoJob`, `merelyUncomputedIsNotWorthAWarning`; `tst_result_columns::columnWorkerIsUnchangedByDemand`; `tst_fusion_store::restoredAfterRestartIsBitIdentical`; `tst_fusion_session::restoredFitIsIndistinguishable`; `audit stored-results`, `audit one-worker` |
| 549 | 13 | test: checking a plot starts the visible sessions without a result; showing another session while it is checked starts it with no other action; hiding drops its waiting pair; unchecking drops all waiting pairs; the running job finishes and its result is stored | `tst_calculation_demand::rowScript`, `showingASessionStartsIt`, `hidingASessionDropsItsWaitingPair`, `uncheckingDropsWaitingPairsKeepsRunning`; `tst_fusion_rows::realRowScript` |
| 550 | 13 | test: a session made visible during column demand is the next job to start; within column demand visible sessions come before hidden loaded ones, and those before sessions not yet loaded | `tst_calculation_demand::sessionShownDuringColumnDemandRunsNext`, `visibleSessionsFirstWithinColumnDemand`, `columnPriorityFollowsRowOrderAfterPlots` |
| 551 | 13 | test: enabling a column over a requested output wants every session without a result, loads unloaded ones a bounded number at a time as hidden pinned sessions, fills the column, ends with every session computed, not applicable or failed, and they leave the pool by ordinary eviction | `tst_calculation_demand::enablingColumnFillsEveryUnloadedSession`, `notApplicableSessionIsSettledWithoutAJob`, `columnFailuresAreBadgedNotReloaded`, `unloadableSessionIsSettledAsFailed`; `tst_fusion_store::columnOverFusionFillsUnloadedSessions` |
| 552 | 13 | test: the column worker's behaviour and statistics are unchanged by column demand, and a stale record it deletes moves the pair into demand | `tst_result_columns::columnWorkerIsUnchangedByDemand`, `staleRecordDeletedByWorkerCreatesDemand` |
| 553 | 13 | test: stored results are restored, not recomputed: enabling the column on a logbook whose sessions all have valid stored results creates no job; a stored rejection of a session not loaded is listed as failed with its reason after a restart without loading it; a column over a chain whose upstream record alone is stored is completed | `tst_calculation_demand::storedResultsCreateNoJob`, `storedRejectionIsBadgedAfterRestartWithoutLoad`, `chainedColumnWithUpstreamRecordIsCompleted`; `tst_fusion_store::fusionColumnWithStoredFitsRunsNothing` |
| 554 | 13, as amended | test: an input-determined failure is stored, listed among the failures and never run again; a job-level failure is listed among the failures as tried again at the next start, not run again in the run, and tried again after a restart | `tst_calculation_demand::inputDeterminedFailureIsStoredBadgedNeverRerun`, `jobLevelFailureIsBadgedNotRerunUntilRestart`, `columnFailuresAreBadgedNotReloaded`, `columnJobLevelFailureIsNotReloadedUntilRestart` |
| 555 | 13, as amended | test: a burst of input changes on a demanded session produces one job; progress counts the session from the first change | `tst_calculation_demand::inputBurstRunsOneJob`, `staleRunningJobIsWaitingAtOnce` |
| 556 | 13 | test: applying a profile with such a column creates demand; start-up with a checked plot and no visible sessions creates none | `tst_calculation_demand::profileStyleColumnsCreateDemand`, `profileStyleApplyCreatesDemand`, `startupRestoreWithHiddenSessionsStartsNothing`; `manual M2` |
| 557 | 13, as amended | test: progress and failures reflect waiting, running, done and failed tracks of plots and columns alike; plot rows and column headers show nothing; no control to refresh or cancel a calculation exists | `tst_calculation_demand::columnProgressAndPendingCells`; `tst_logbook_indicators::headerIsPlainAndNothingAnimates`, `rowWarningFollowsTheText`, `rowWarningHoverIsTheSessionsFailures`, `clickOnRowWarningIsAClickOnTheCell`, `failedLoadSessionShowsRowWarningNotPending`, `failedWriteIsListedInTheHover` |
| 558 | 13 | test: a pending column cell is distinguishable from an unavailable one, is not written to the logbook index, and becomes the value when the record is written | `tst_logbook_indicators::pendingCellsAreDistinctFromUnavailable`, `pendingCellBecomesValueWhenRecordIsWritten`, `sortingTreatsPendingAsUnavailable`, `unreadableRecordPendingIsNotDemandPending` |
| 559 | 13, as amended | test: saves and bulk edits still precede the fill's loads; no result is computed from a file being rewritten; the fill works from its first load to its last result without any step that loads nothing and reports no progress of its own, the status bar showing the computations while it works, and the scheduler does not spin while the fill waits on a job | `tst_calculation_demand::savesAndBulkEditsPrecedeLoadStep`, `startupWithEnabledColumnLoadsAfterColumnWorker`, `fillTaskRestsWhileWaiting`; `tst_session_model_engine::schedulerCompletesWaitingTaskWhoseWorkIsGone`, `schedulerWaitingTaskDoesNotSpin`; `tst_status_bar::computationsAreOneItem` |
| 560 | 13 | test: the executor never holds more than the running job and one chosen next job; changing demand replaces the chosen next job | `tst_jobqueue::holdsAtMostRunningAndChosenNext`, `offerReplacesChosenNext`; `tst_calculation_demand::executorHoldsAtMostRunningAndChosenNext`, `changingDemandReplacesChosenNext` |
| 561 | 13 | test: the UI thread is not blocked by a running requested calculation, and the calculation runs below normal priority | `tst_jobqueue::mainThreadIsNotBlockedByARunningJob`, `workerRunsBelowNormalPriority`; `tst_fusion_jobs::solverThreadsRunAtWorkerPriority` |
| 562 | 13 | test: the existing tests of stored results and of the executor pass, those that asserted the refresh gesture, the queue order or the pruning of queued jobs rewritten to the demand rules | `tst_jobqueue::oneAtATimeInOfferOrder`, `duplicateOffersCreateNoDuplicates`; `tst_fusion_rows::realRowScript`; `tst_fusion_store::dependencyEditDropsRecord`, `codeStampChangeDropsRecordOnLoad`; `audit gestures` |
| 563 | 14, as amended | docs/ describe the executor, the demand layer, logbook columns over requested results, what the user sees (no refresh, the status bar and the row warning, pending cells, failures) and where a column over a requested calculation stays unavailable or pending | `audit demand` |

### 9.7 Calculation refinements (items 601-662)

The sixty-two clauses of the specification "Calculation refinements", stated
in full in
[appendix G](#appendix-g-the-acceptance-items-of-calculation-refinements-601-662).
In the map, item = 600 + the clause number; the same four line forms as 9.2,
and every item has at least one test or audit line. "Section" is the section
of the specification (10.1 and 10.2 its subsections); its sections 1
(motivation) and 4 (terms) have no item. Clauses 53-61 are its section 14
tests, one per bullet, and clause 62 its section 15. Clause 34 is stated as
settled when it was implemented: a column track that is running or failed on
one blocker files its other blockers, as a plot track does. Clauses 42 and 50
were stated as settled too (the fill releases every hold once the executor is
shut down; the glyph plumbing that needed Qt Widgets lived in a views' half
of the shared indicator, and the state lost its progress label); both are now
stated as amended. The specification, revised after its plan review,
carries the rest of what the first plan draft had settled (the scheduler's
rule scoped to tasks that can wait, a fill that starts from nothing starting
its own count, a column's verdict holding for that column, the bulk edit's
announcement of its edit, a successful load forgetting failed-load facts).
The specification amends those of 9.6 (items 513, 526, 528, 530, 532,
535-537, 539, 546, 547, 557, 559). Clauses 4, 12, 18, 24, 27, 31, 42, 44-51,
53, 54, 56, 57, 60 and 62 are stated as amended by the specification "One
status bar for background work" (9.8).

| # | Section | Clause | Evidence |
|---|---|---|---|
| 601 | 2 | the principles of the demand-driven specification hold unchanged: what is switched on is the request, anything wanted is wanted at once, finished work is never wasted, background work never degrades the application, a failure is shown and never retried in a loop, each component's contract can be stated without naming the others | `tst_calculation_demand::rowScript`, `enablingColumnFillsEveryUnloadedSession`, `storedResultsCreateNoJob`, `nullCollaborators`, `failedRecordWriteIsShownAndNotRetried`; `tst_jobqueue::workerRunsBelowNormalPriority`; `audit demand` |
| 602 | 2 | a fact is computed by the component that owns it and read by the others: each enabled column's closure and requested calculations and a row's display name by the session model, a record's reason by the logbook index, a failed record write by the result store | `tst_result_columns::sessionModelExposesColumnKnowledge`, `sessionDisplayNameOfEveryRowKind`; `tst_calculation_demand::columnKnowledgeComesFromTheSessionModel`; `tst_logbook_index::recordReasonChangeIsAnnounced`; `tst_result_store::writeFailureIsAnnounced`; `audit demand` |
| 603 | 2 | a component announces what it changes; no component infers another's change from a signal about something else | `tst_logbook_index::recordReasonChangeIsAnnounced`; `tst_result_store::writeFailureIsAnnounced`; `tst_calculation_demand::recordReasonReachesDemandThroughRecordChange`; `audit demand` |
| 604 | 2, as amended | one rule, one path: plots and columns are classified and filed as candidates by one walk, the difference between them being data | `tst_calculation_demand::runningColumnTrackFilesItsOtherBlockers`, `focusedSessionFirstThenRowOrder`, `columnPriorityFollowsRowOrderAfterPlots`, `columnProgressAndPendingCells`; `audit demand` |
| 605 | 2 | what no product code uses is removed, unless a named later feature needs it, in which case it is kept and says so: the executor's cancel operation and job history stay for the jobs dock | `tst_jobqueue::cancelRunningThenNextStarts`; `tst_jobmodel::historyFromSignalsAlone`; `audit gestures`, `audit demand` |
| 606 | 3 | unchanged: the executor's lifecycle, the fusion kernel, the record format, the logbook index's other contents, and the column worker's behaviour and statistics | `tst_jobqueue::runsAndPublishes`, `staleRunningJobIsStoppedAtOnce`, `shutdownWithQueuedAndRunning`; `tst_fusion_golden::successFixturesMatchGolden`; `tst_result_records::layoutIsPinned`; `tst_logbook_index::recordReasonsRoundTrip`; `tst_result_columns::columnWorkerIsUnchangedByDemand` |
| 607 | 3 | unchanged: what is in demand, its priority, the input-settle wait, the bound on held sessions, and the one-way flow between the demand layer, the executor, the store and the column worker | `tst_calculation_demand::focusedSessionFirstThenRowOrder`, `visibleSessionsFirstWithinColumnDemand`, `inputBurstRunsOneJob`, `supersededJobIsRunAgainAfterInputsSettle`, `enablingColumnFillsEveryUnloadedSession`; `tst_result_columns::staleRecordDeletedByWorkerCreatesDemand`; `audit demand` |
| 608 | 3 | the jobs dock is out of scope: the executor keeps its cancel operation and its job history for it; the column worker does not load sessions for the demand layer | `tst_jobqueue::cancelQueued`, `cancelRunningThenNextStarts`; `tst_jobmodel::historyFromSignalsAlone`; `tst_result_columns::columnWorkerIsUnchangedByDemand`; `audit gestures` |
| 609 | 5 | a task registered as one that can wait (TaskDef::canStep) which the scheduler last reported active and which has no work any more is completed at the next tick whether or not it was stepped: its progress is reported one last time and its completion is called, not cancelled, before the next active task is reported or the scheduler goes idle; whoever takes a resting task's work away wakes the scheduler | `tst_session_model_engine::schedulerCompletesWaitingTaskWhoseWorkIsGone`; `tst_calculation_demand::fillTaskRestsWhileWaiting` |
| 610 | 5 | a task that lost its work because it was cancelled completes once, as cancelled; a task unregistered while active is never completed | `tst_session_model_engine::schedulerCompletesWaitingTaskWhoseWorkIsGone`, `schedulerTaskCanBeUnregistered` |
| 611 | 5 | the other tasks (save, load, bulk edit, column work) complete through their step or by cancel at the same moments as before: they can lose work outside a step, so the rule applies to tasks that can wait only | `tst_session_model_engine::schedulerCompletesWaitingTaskWhoseWorkIsGone`, `schedulerWaitingTaskDoesNotSpin`; `tst_calculation_demand::savesAndBulkEditsPrecedeLoadStep`, `startupWithEnabledColumnLoadsAfterColumnWorker` |
| 612 | 5, as amended | the fill has no ending state and no step that loads nothing; it reports no progress of its own and keeps no high-water mark: the status bar shows the computations while it works, and a burst of computations after none starts its own count | `tst_calculation_demand::fillTaskRestsWhileWaiting`, `fillEndingBehindAnotherTaskStartsNextCountFresh`, `fillTaskIsLowestAndNotCancellable`; `audit demand`; `tst_status_bar::computationsAreOneItem` |
| 613 | 5 | the scheduler still learns nothing about jobs or demand | `tst_session_model_engine::schedulerCompletesWaitingTaskWhoseWorkIsGone`; `audit demand`, `audit branch-mechanisms` |
| 614 | 6 | recording a reason that differs from what the logbook index held emits the record-changed signal for that session and calculation, as a record write or removal does; an unchanged reason emits nothing, and a write or removal still emits exactly once | `tst_logbook_index::recordReasonChangeIsAnnounced`; `tst_result_store::recordReasonRecordedAtWriteAndRestore`; `tst_result_columns::managerDropsDependentValues` |
| 615 | 6 | a reason taught to the index by a restore into the column worker's copy reaches the demand layer through that signal alone; the demand layer compares no reasons on the model's display change | `tst_calculation_demand::recordReasonReachesDemandThroughRecordChange`, `storedRejectionIsBadgedAfterRestartWithoutLoad`; `audit demand` |
| 616 | 6 | the bulk edit announces its edit as a dependency change on both of its paths (a loaded session, and the temporary copy of one that is not loaded), and the demand layer observes no display change of the model: a bulk edit reaches it as an input change, and the column worker's display changes are never read | `tst_session_model_engine::bulkEditAnnouncesADependencyChange`; `tst_calculation_demand::bulkEditMakesSettledSessionApplicable`, `settledPairsSurviveEvictionSortAndColumnWorker`; `audit demand` |
| 617 | 7 | the result store announces a record write that failed, with the reason, for the session and calculation, after the index's record change of that pair; the result stays installed and nothing retries the write | `tst_result_store::writeFailureIsAnnounced`, `writeFailureLeavesResultUsable`, `writeFailureKeepsPreviousRecord` |
| 618 | 7, as amended | a track whose record could not be written is failed with that reason, for plots and columns alike, whether the session is loaded or not, and is a failure of the recording, shown by the status bar's warning and the logbook row: a source is done only when none of its storable calculations is remembered so | `tst_calculation_demand::failedRecordWriteIsShownAndNotRetried`, `failedRecordWriteIsShownOnThePlotRow`; `tst_logbook_indicators::failedWriteIsListedInTheHover`; `manual M32` |
| 619 | 7 | such a pair is not offered again and its session is not loaded again for it in the run until its inputs change, so the user's view is the same before and after an eviction | `tst_calculation_demand::failedRecordWriteIsShownAndNotRetried`, `failedRecordWriteIsShownOnThePlotRow`, `settledPairsSurviveEvictionSortAndColumnWorker`; `manual M32` |
| 620 | 7 | a later successful write of the pair's record clears the failure; nothing is persisted, so the next start tries the write again | `tst_calculation_demand::failedRecordWriteIsShownAndNotRetried`, `pairMemoryIsClearedByRecordInputAndRegistryChanges`; `manual M32` |
| 621 | 8 | the session model exposes, per enabled column, its static dependency closure and its requested calculations, current under the registrations at the moment of the call and valid between two column rebuilds | `tst_result_columns::sessionModelExposesColumnKnowledge`, `columnExplicitCalculations` |
| 622 | 8 | the demand layer reads each column's closure and requested calculations from the session model and computes neither; a column change reaches it through the model's reset alone; it still computes a plot's closure and requested calculations from the registry | `tst_calculation_demand::columnKnowledgeComesFromTheSessionModel`, `ordinaryColumnsCreateNoDemand`, `ordinaryPlotsAreNeverInspected`; `audit demand` |
| 623 | 8 | the session model exposes one display name of a row: the loaded session's description, else the description the logbook index caches for the row, else the session id | `tst_result_columns::sessionDisplayNameOfEveryRowKind` |
| 624 | 8, as amended | the demand layer's failure entries and progress and the executor's job records name a session by that display name, for loaded rows, stubs and failed-load placeholders alike, and neither computes a name | `tst_jobqueue::runsAndPublishes`; `tst_calculation_demand::visibleFailedLoadIsSettledAsFailed`, `recordReasonReachesDemandThroughRecordChange`, `failedRecordWriteIsShownAndNotRetried`; `audit demand` |
| 625 | 9 | the demand layer keeps no memory of its own offer: it is the only offerer, so the chosen next job is always its own, and when the choice finds nothing it is withdrawn | `tst_calculation_demand::chosenNextJobFollowsTheExecutorsAnswer`, `changingDemandReplacesChosenNext`, `hidingASessionDropsItsWaitingPair`; `audit gestures` |
| 626 | 9 | the demand layer does not compare a candidate with the chosen next job before offering it; it acts on the executor's answer, and an offer equal to the chosen next job keeps it as it is | `tst_calculation_demand::chosenNextJobFollowsTheExecutorsAnswer`, `executorHoldsAtMostRunningAndChosenNext`; `tst_jobqueue::duplicateOffersCreateNoDuplicates` |
| 627 | 9, as amended | a track has no settling flag and no job id; the per-source counts and lists are gone, and nothing the user sees depends on them | `tst_calculation_demand::columnProgressAndPendingCells`, `changeSignalsAreMinimal`, `progressTextWithoutAPass`; `audit demand` |
| 628 | 9 | the executor has no idle signal, no queued signal, no query of both active jobs and no busy-period bookkeeping; its job model, its idle and running queries and its other signals carry the same facts | `tst_jobqueue::runsAndPublishes`, `holdsAtMostRunningAndChosenNext`, `offerReplacesChosenNext`, `cancelFromRowsInsertedLeavesNoPin`; `tst_jobmodel::historyFromSignalsAlone`; `audit gestures` |
| 629 | 9, as amended | the executor keeps its cancel operation, with the caller's reason; the demand layer is its one product caller, for the running job of a recording switched off, and the jobs dock may call it too; its comment and the audit say so (amended by the specification of 1501-1524) | `tst_calculation_demand::switchingOffCancelsTheRunningJob`; `tst_jobqueue::cancelRunningThenNextStarts`, `cancelQueued`; `audit gestures` |
| 630 | 9 | the executor's refusal kinds are unchanged; the demand layer offers upstream first, so a Blocked refusal is not expected from it, and the documentation says so instead of the code guarding for it | `tst_jobqueue::refusesBlockedAndDone`, `refusesMissingInput`; `tst_calculation_demand::chainedBlockersContinue`, `columnOfferRefusalIsNotLeftPending` |
| 631 | 10.1, as amended | one walk over the session rows derives progress, failures, pending cells and candidates, for plots and columns at once, under one row stability guard, returning plain values; offers, withdrawals, holds, loads and signals happen after it | `tst_calculation_demand::changeSignalsAreMinimal`, `columnProgressAndPendingCells`, `passOverManyStubsReadsEachRecordSetOnce`; `audit demand` |
| 632 | 10.1 | a loaded session is classified from the engine's blocker inspection of the source's names, combined alike for a plot's one name and a column's names, the running job, the pair memory and the settle wait; the combined report is memoized per session and source and dropped by an input change or publication, a load, a record change, a job's end, a reset or a registry change | `tst_calculation_demand::hiddenAndStubRowsAreNotTracks`, `ordinaryPlotsAreNeverInspected`, `uncheckedPlotsAreNeverInspected`, `progressTextWithoutAPass`, `chainedBlockersContinue`, `registryChangeReclassifies` |
| 633 | 10.1, as amended | a session that is not loaded is a track of columns only, classified in this order, after the exclusion of a session switched off for background computation, which is decided first from the index (amended by the specification of 1501-1524): no storable calculation, not applicable; a storable calculation remembered failed (a job, a load or a write that failed), failed with the remembered reason; every storable calculation remembered not applicable for that column, not applicable; every storable calculation with a record, done, or failed with the first recorded reason; a failed-load placeholder, failed ("The session file could not be loaded"); otherwise waiting | `tst_calculation_demand::fillNeverLoadsAnExcludedSession`, `storedRejectionIsBadgedAfterRestartWithoutLoad`, `unloadableSessionIsSettledAsFailed`, `failedRecordWriteIsShownAndNotRetried`, `columnVerdictDoesNotSuppressAnotherColumn`, `passOverManyStubsReadsEachRecordSetOnce`; `tst_logbook_indicators::failedLoadSessionShowsRowWarningNotPending` |
| 634 | 10.1, as settled | candidates are filed in the walk by tier: the focused session's plot pairs, the other visible sessions' plot pairs, then the column pairs of visible loaded sessions, then of hidden loaded sessions, each in row order; within a session source order, then blocker order; a pair once, in its first tier; never a pair with a remembered failure or refusal, the running job not asked to stop, or a pair of a settling session; a column track running or failed on one blocker files its other blockers, as a plot track does | `tst_calculation_demand::focusedSessionFirstThenRowOrder`, `columnPriorityFollowsRowOrderAfterPlots`, `visibleSessionsFirstWithinColumnDemand`, `sessionShownDuringColumnDemandRunsNext`, `runningColumnTrackFilesItsOtherBlockers`, `inputBurstRunsOneJob` |
| 635 | 10.1 | the load candidates are the waiting, hidden, unloaded sessions that are not settling and not held, in row order, up to the bound | `tst_calculation_demand::enablingColumnFillsEveryUnloadedSession`, `sessionShownDuringColumnDemandRunsNext`, `heldSessionShownStaysLoaded`, `chainedColumnKeepsItsHold` |
| 636 | 10.2 | one memory of the run, keyed by pair (session, requested calculation), holds not applicable (the executor refused the pair, or a loaded column found its source not applicable) or failed with a reason (a job that ended failed, a result that is never stored, a load that failed, a record write that failed); there is no memory per cell, and every verdict is derived from the engine or the record set and this memory | `tst_calculation_demand::settledPairsSurviveEvictionSortAndColumnWorker`, `notApplicableSessionIsSettledWithoutAJob`, `columnFailuresAreBadgedNotReloaded`, `columnJobLevelFailureIsNotReloadedUntilRestart`, `columnOfferRefusalIsNotLeftPending`, `unloadableSessionIsSettledAsFailed`; `audit demand` |
| 637 | 10.2 | a column's not-applicable verdict holds for that column only: it keeps its session from being loaded again for that column, and it never makes another source's track not applicable nor keeps a pair from being offered | `tst_calculation_demand::columnVerdictDoesNotSuppressAnotherColumn`, `notApplicableSessionIsSettledWithoutAJob` |
| 638 | 10.2 | the memory is cleared for a session by a relevant input change, for a pair by a record change of that pair, for everything by a registry change; a model reset forgets the pairs of sessions that no longer have a row; nothing is persisted | `tst_calculation_demand::pairMemoryIsClearedByRecordInputAndRegistryChanges`, `removedSessionLeavesNoTrace`, `registryChangeReclassifies`, `jobLevelFailureIsBadgedNotRerunUntilRestart` |
| 639 | 10.2 | a bulk edit of a session clears what the run remembered of it, through the dependency change it publishes, for a loaded session and for one that is not loaded alike; nothing else clears a session's facts, the column worker's display changes in particular; a load of the session that succeeds forgets its failed-load facts | `tst_calculation_demand::bulkEditMakesSettledSessionApplicable`, `settledPairsSurviveEvictionSortAndColumnWorker`, `failedRecordWriteIsShownAndNotRetried`, `successfulLoadForgetsFailedLoadFacts`; `manual M33` |
| 640 | 10.2 | a session found not applicable, a job-level failure, a failed load and a failed write are not loaded or offered again after an eviction, a sort, or the column worker's processing of the stub | `tst_calculation_demand::settledPairsSurviveEvictionSortAndColumnWorker`, `notApplicableSessionIsSettledWithoutAJob`, `columnJobLevelFailureIsNotReloadedUntilRestart`, `unloadableSessionIsSettledAsFailed` |
| 641 | 11 | the demand layer stays one component with one contract towards the views and the executor, divided into parts with contracts of their own (the presentation values, the fill, the settle clock and the reconciler); the fill and the settle clock expose nothing of the walk, and the walk calls neither the executor nor the session model's loading and pinning | `tst_calculation_demand::settleClockAnswersItsQuestions`, `nullCollaborators`; `audit demand` |
| 642 | 11, as amended | the fill keeps at most the bound of hidden sessions loaded and pinned for column demand, releases a hold when its session has no pending cell or no loaded row, loads the next candidate when a hold is free and the scheduler steps it, reports no progress of its own, and holds nothing once the executor is shut down or the component goes | `tst_calculation_demand::enablingColumnFillsEveryUnloadedSession`, `disablingColumnReleasesHeldSessions`, `removedOrRepopulatedHeldSessionIsReleased`, `noLoadsAfterExecutorShutdown`, `demandDestroyedReleasesHoldsAndTask`, `fillTaskRestsWhileWaiting` |
| 643 | 11 | the settle clock holds the per-session deadlines of the input-settle wait and one timer for the earliest, answers whether a session is settling and when the next wait ends, and calls its owner when a wait ends | `tst_calculation_demand::settleClockAnswersItsQuestions`, `inputBurstRunsOneJob`, `supersededJobIsRunAgainAfterInputsSettle` |
| 644 | 12, as amended | a plot row and a logbook column header show nothing about computing: no glyph, no label, no count and no room reserved for one, so they look as any other; progress is shown once, in the status bar, and a failure on the recording's logbook row | `tst_logbook_indicators::headerIsPlainAndNothingAnimates`, `rowWarningFollowsTheText`; `audit demand`; `tst_status_bar::computationsAreOneItem`; `manual M34` |
| 645 | 12, as amended | the status bar's hover carries the numbers and the running recording's step, and the warning's hover the failures | `tst_status_bar::taskShownOverComputationsAndHoverListsBoth`, `warningCountsRecordingsAndListsThem`; `manual M34` |
| 646 | 12, as amended | no working-indicator clock exists: nothing repaints by itself while work runs, and the status bar's progress bar needs no clock of its own | `tst_logbook_indicators::headerIsPlainAndNothingAnimates`; `audit demand`; `manual M34` |
| 647 | 12, as amended | the status bar shows "Computing results: k / n" while the fill is the active task, with no cancel button, distinct from the column worker's "Computing columns: k / n"; the fill is not cancellable, and saves, loads, bulk edits and column work still come first | `tst_status_bar::schedulerTasksShowTheirLabelsAndCounts`; `tst_calculation_demand::fillTaskIsLowestAndNotCancellable`, `savesAndBulkEditsPrecedeLoadStep`; `audit demand`; `manual M31` |
| 648 | 12, as amended | a stored result that could not be written is a failure of the recording, listed with its reason by the status bar's warning and on the recording's logbook row | `tst_logbook_indicators::failedWriteIsListedInTheHover`; `tst_calculation_demand::failedRecordWriteIsShownOnThePlotRow`; `manual M32` |
| 649 | 12, as amended | nothing else the user sees changes: pending cells, a failure's meaning, and the absence of any refresh or cancel for computations | `tst_logbook_indicators::pendingCellsAreDistinctFromUnavailable`, `pendingCellBecomesValueWhenRecordIsWritten`, `plainHeaderAndCellsAreIdenticalToBase`, `clickOnRowWarningIsAClickOnTheCell`; `audit demand` |
| 650 | 12, as amended | one drawing of the warning glyph exists, the style's standard warning icon, used by the status bar and the logbook row; no view shares a glyph painting or a hover helper, and each view that holds the demand layer learns of its end itself | `tst_logbook_indicators::survivesDemandDestroyedFirst`; `audit demand`, `audit widget-free-core`; `tst_status_bar::survivesDemandDestroyedFirst` |
| 651 | 13, as amended | the demand layer remains widget-free, the only offerer and observed by nothing; the status bar and the logbook present its values and decide nothing | `tst_calculation_demand::nullCollaborators`, `survivesExecutorShutdown`; `audit widget-free-core`, `audit gestures`, `audit demand` |
| 652 | 13 | the flow stays one way: the demand layer chooses; the executor publishes; the store writes the record or announces that it could not; the index notes the outcome and announces it; the record change drops the cached column values and the demand layer's memos; the demand layer sees the result, or the failure, through what it always reads | `tst_result_columns::staleRecordDeletedByWorkerCreatesDemand`; `tst_calculation_demand::enablingColumnFillsEveryUnloadedSession`, `recordReasonReachesDemandThroughRecordChange`, `failedRecordWriteIsShownAndNotRetried`; `tst_logbook_indicators::pendingCellBecomesValueWhenRecordIsWritten`; `audit demand` |
| 653 | 14, as amended | test: a task with work it cannot step loses its work without a step and is completed once, not cancelled, after a final progress report, before the next active task is reported or the scheduler goes idle; the existing tasks complete at the same moments as before; the fill works from its first load to its last result without any step that loads nothing and reports no progress of its own, the status bar showing the computations | `tst_session_model_engine::schedulerCompletesWaitingTaskWhoseWorkIsGone`; `tst_calculation_demand::fillTaskRestsWhileWaiting`, `fillEndingBehindAnotherTaskStartsNextCountFresh`, `savesAndBulkEditsPrecedeLoadStep`; `tst_status_bar::computationsAreOneItem` |
| 654 | 14, as amended | test: a reason recorded by a restore into the column worker's copy reaches the demand layer through the manager's record-changed signal alone: a stored rejection of a session that is not loaded is listed among the failures with its reason after the worker's pass, and no reason comparison happens on the model's display change | `tst_calculation_demand::recordReasonReachesDemandThroughRecordChange`, `storedRejectionIsBadgedAfterRestartWithoutLoad`; `tst_logbook_index::recordReasonChangeIsAnnounced` |
| 655 | 14 | test: a record write that fails is a shown failure: listed as failed with the reason while the session is loaded and after its eviction, not offered again in the run, cleared by a later successful write, tried again at a restart; no session is loaded twice for it | `tst_calculation_demand::failedRecordWriteIsShownAndNotRetried`, `failedRecordWriteIsShownOnThePlotRow`; `tst_result_store::writeFailureIsAnnounced` |
| 656 | 14, as amended | test: the demand layer reads each column's closure and requested calculations from the session model, and a column change reaches it through the model's reset alone; the failure entries, progress and the job records name a session by the model's display name, for loaded rows, stubs and failed-load placeholders alike | `tst_calculation_demand::columnKnowledgeComesFromTheSessionModel`, `visibleFailedLoadIsSettledAsFailed`; `tst_result_columns::sessionModelExposesColumnKnowledge`, `sessionDisplayNameOfEveryRowKind`; `tst_jobqueue::runsAndPublishes` |
| 657 | 14, as amended | test: plots and columns are classified and filed by one walk, which computes progress and failures and announces them only when they change, and the acceptance items of the demand-driven specification on demand, priority, the settle wait, failures, not applicable, chains, unloaded sessions and the pair memory pass unchanged in what they assert | `tst_calculation_demand::rowScript`, `enablingColumnFillsEveryUnloadedSession`, `focusedSessionFirstThenRowOrder`, `inputBurstRunsOneJob`, `columnFailuresAreBadgedNotReloaded`, `notApplicableSessionIsSettledWithoutAJob`, `chainedBlockersContinue`, `columnProgressAndPendingCells`, `runningColumnTrackFilesItsOtherBlockers`; `tst_fusion_rows::realRowScript` |
| 658 | 14 | test: every case the per-cell memory covered is covered by a pair fact: a session found not applicable, a job-level failure, a failed load and a failed write are not loaded or offered again after eviction, a sort or the column worker's pass; a bulk edit makes the session applicable again; a record change of the pair, an input change of the session and a registry change clear what is remembered | `tst_calculation_demand::settledPairsSurviveEvictionSortAndColumnWorker`, `bulkEditMakesSettledSessionApplicable`, `pairMemoryIsClearedByRecordInputAndRegistryChanges`, `columnVerdictDoesNotSuppressAnotherColumn` |
| 659 | 14 | test: no product code reads the removed signals, queries and fields; the executor never holds more than the running and the chosen next job; the chosen next job is withdrawn when demand no longer wants it | `tst_calculation_demand::chosenNextJobFollowsTheExecutorsAnswer`, `executorHoldsAtMostRunningAndChosenNext`, `changingDemandReplacesChosenNext`; `tst_jobqueue::holdsAtMostRunningAndChosenNext`; `audit gestures`, `audit demand` |
| 660 | 14, as amended | test: the plot row shows nothing; no clock exists; the status bar shows the fill as the computations; a stored result that could not be written is listed in the logbook row's and the status bar's hover | `tst_logbook_indicators::headerIsPlainAndNothingAnimates`, `failedWriteIsListedInTheHover`; `tst_status_bar::schedulerTasksShowTheirLabelsAndCounts`, `computationsAreOneItem`; `tst_calculation_demand::failedRecordWriteIsShownAndNotRetried`; `audit demand` |
| 661 | 14 | the audit's demand and gestures groups keep the removed and moved names out, and a rule forbids a second computation of a column's requested calculations outside the session model and the registry | `audit demand`, `audit gestures` |
| 662 | 15, as amended | docs/ and tests/README.md describe the executor's signals and queries (cancel with the caller's reason, the demand layer its one product caller, for the running job of a recording switched off, and the jobs dock may call it too), the one walk, the one memory, the parts of the demand layer, the fill's completion, the failed write, the session model as the source of column knowledge and display names, and the status bar and the row warning; not the indicator, the clock or the fill's progress text (amended by the specification of 1501-1524) | `audit demand` |

### 9.8 One status bar for background work (items 701-754)

The fifty-four clauses of the specification "One status bar for background
work", stated in full in
[appendix H](#appendix-h-the-acceptance-items-of-one-status-bar-for-background-work-701-754).
In the map, item = 700 + the clause number; the same four line forms as 9.2,
and every item has at least one test or audit line. "Section" is the section
of the specification; its sections 1-4 (motivation, principles, scope, terms)
have no item, their principles being carried by the clauses of the sections
that apply them. Clauses 45-53 are its section 13 tests, one per bullet, and
clause 54 its section 14. Clauses 3, 25 and 31 are stated as settled: the
computations are shown done out of the high-water mark, as the scheduler's
tasks are; the one drawing of the warning glyph is the style's standard
warning icon; the pending cells keep an announcement of their own. The
specification amends those of 9.2 (items 111, 115), 9.3 (item 228), 9.4
(items 306, 321, 337, 339), 9.6 (items 517, 525, 526, 530, 535-537, 539, 542,
554, 555, 557, 559, 563) and 9.7 (items 604, 612, 618, 624, 627, 631, 642,
644-651, 653, 654, 656, 657, 660, 662).

| # | Section | Clause | Evidence |
|---|---|---|---|
| 701 | 5 | the main window has a status bar, always present, empty when nothing is in progress and nothing has failed; it never appears or disappears, so the layout does not jump | `tst_status_bar::heightNeverChanges`, `activityLeftAndWarningRight`, `schedulerTasksShowTheirLabelsAndCounts`, `warningAbsentWhenNothingFailedAndNotDismissable`; `audit demand`; `manual M34` |
| 702 | 5 | the activity area presents the scheduler's active task with its existing label ("Saving sessions", "Loading sessions", "Updating sessions", "Computing columns") and its progress as the scheduler reports it | `tst_status_bar::schedulerTasksShowTheirLabelsAndCounts`; `audit demand`; `manual M35` |
| 703 | 5, as settled | the computations are one item, "Computing results": the sessions with a track waiting or running in any source, plots and columns alike, each once, shown done out of the high-water mark since the count was last zero; the item exists while the count is above zero | `tst_status_bar::computationsAreOneItem`; `tst_calculation_demand::progressCountsEachSessionOnce`; `manual M34` |
| 704 | 5 | the computations' hover names the recording being computed and the running job's latest progress text | `tst_status_bar::taskShownOverComputationsAndHoverListsBoth`; `tst_calculation_demand::progressCountsEachSessionOnce`, `progressTextWithoutAPass` |
| 705 | 5 | the fill is never an item: its count is contained in the computations', and while it is the scheduler's active task the computations are shown | `tst_status_bar::computationsAreOneItem`; `tst_calculation_demand::fillTaskIsLowestAndNotCancellable` |
| 706 | 5 | the fill keeps its scheduler registration and contract (completed when its work is gone, not cancellable) and reports no progress of its own | `tst_calculation_demand::fillTaskRestsWhileWaiting`, `fillEndingBehindAnotherTaskStartsNextCountFresh`, `fillTaskIsLowestAndNotCancellable`; `tst_session_model_engine::schedulerCompletesWaitingTaskWhoseWorkIsGone`; `audit demand` |
| 707 | 6 | the shown item is the scheduler's active task when it is an item, otherwise the computations, which return to the label when the task ends | `tst_status_bar::taskShownOverComputationsAndHoverListsBoth`; `manual M35` |
| 708 | 6 | the hover lists every item in progress, the shown item first, each with its label and count, and for the computations the recording and its step; nothing in it is a control | `tst_status_bar::taskShownOverComputationsAndHoverListsBoth`; `audit demand` |
| 709 | 6 | the cancel control is present exactly while the shown item is a scheduler task registered as cancellable, and cancels that task; it is absent for saving and for the computations | `tst_status_bar::cancelOnlyForACancellableShownTask`; `manual M35` |
| 710 | 6 | with no item in progress the activity area is empty; the warning is not part of it and stays | `tst_status_bar::schedulerTasksShowTheirLabelsAndCounts`, `warningBesideComputationsThenAlone` |
| 711 | 7 | a warning is shown beside the activity area while any recording has a current failure: the warning glyph and the number of recordings that could not be computed, counting recordings, not pairs | `tst_status_bar::warningCountsRecordingsAndListsThem`; `tst_calculation_demand::failuresAreOnePerSessionInRowOrder`; `manual M36` |
| 712 | 7 | the warning is shown while computations continue, so a failure found early is visible at once, and stands alone once nothing is computing | `tst_status_bar::warningBesideComputationsThenAlone`; `manual M36` |
| 713 | 7 | its hover lists the recordings in session-model row order, each with the failed calculation and the reason, says which are tried again at the next start, and lists at most ten, then how many more | `tst_status_bar::warningCountsRecordingsAndListsThem`, `warningListsAtMostTenRecordings`; `tst_calculation_demand::failureTextAndItsLimit`, `failuresNameEachCalculationOnce`, `failuresAreOnePerSessionInRowOrder` |
| 714 | 7 | it persists across restarts with nothing new written: a stored rejection is counted at the next start without loading the recording, once its source is restored and the column worker's pass has reported the record set | `tst_status_bar::warningAfterRestart`; `tst_calculation_demand::storedRejectionIsAFailureWithoutLoad`; `manual M37` |
| 715 | 7 | a failure that is not stored is tried again at the next start and shows again only if it fails again; a retry that succeeds clears it | `tst_calculation_demand::unstoredFailureReturnsOnlyWhenItFailsAgain`; `tst_status_bar::warningAfterRestart`; `manual M37` |
| 716 | 7 | it clears as the pair memory clears (a record change of the pair, an input change of the session, a registry change, the session losing its row) or when the last source over the calculation is switched off; nothing else clears it and nothing dismisses it | `tst_calculation_demand::failuresClearAsThePairMemoryClears`, `failuresFollowWhatIsSwitchedOn`; `tst_status_bar::warningAbsentWhenNothingFailedAndNotDismissable`; `manual M38` |
| 717 | 7 | a failure of a calculation no plot or column wants is not current and is not counted | `tst_calculation_demand::failuresFollowWhatIsSwitchedOn`; `manual M38` |
| 718 | 8 | a recording with a current failure shows one warning glyph on its logbook row, right after the text of its first cell, whatever column that is | `tst_logbook_indicators::rowWarningFollowsTheText`, `rowWarningFollowsTheFirstVisualColumn`; `manual M36` |
| 719 | 8 | the glyph's hover lists the recording's failed calculations with their reasons, in the form of the status bar's hover for that recording | `tst_logbook_indicators::rowWarningHoverIsTheSessionsFailures`, `failedWriteIsListedInTheHover`, `failedLoadSessionShowsRowWarningNotPending` |
| 720 | 8 | a row that is not loaded shows the glyph from the record set, without loading | `tst_logbook_indicators::rowWarningFollowsTheText`; `tst_calculation_demand::storedRejectionIsAFailureWithoutLoad` |
| 721 | 8 | the cells over the failed calculation are blank; the pending cell is unchanged | `tst_logbook_indicators::rowWarningFollowsTheText`, `pendingCellsAreDistinctFromUnavailable`, `pendingCellBecomesValueWhenRecordIsWritten` |
| 722 | 8 | the glyph takes room in the first cell's text rectangle only while it is shown: a logbook without failures looks exactly as before | `tst_logbook_indicators::rowWarningFollowsTheText`, `plainHeaderAndCellsAreIdenticalToBase` |
| 723 | 9 | the working indicator and the warning badge on plot rows and column headers go, with the room reserved for them and the header's hover: such a row or header looks as any other | `tst_logbook_indicators::headerIsPlainAndNothingAnimates`, `plainHeaderAndCellsAreIdenticalToBase`; `audit demand`; `manual M34` |
| 724 | 9 | the working-indicator clock goes: its creation, its following of the demand layer, its place in the application context and the views' repaint on its frames | `tst_logbook_indicators::headerIsPlainAndNothingAnimates`; `audit demand` |
| 725 | 9, as settled | the shared glyph painting and its hover helper go; one drawing of the warning glyph exists, the style's standard warning icon, used by the status bar and the logbook row | `tst_status_bar::warningCountsRecordingsAndListsThem`; `tst_logbook_indicators::rowWarningFollowsTheText`; `audit demand` |
| 726 | 9 | the logbook's progress line, its cancel button, the dock feature's scheduler wiring and the fixed minimum size hint go | `tst_logbook_indicators::headerIsPlainAndNothingAnimates`; `audit demand` |
| 727 | 9 | the fill's separate progress text and its display progress total go | `tst_calculation_demand::fillTaskRestsWhileWaiting`; `tst_status_bar::computationsAreOneItem`; `audit demand` |
| 728 | 9 | the demand layer's per-source state, its queries, its change signals and the working id lists go; the audit's demand group keeps the names out | `tst_calculation_demand::columnProgressAndPendingCells`; `audit demand` |
| 729 | 10 | progress: the sessions with a track waiting or running in any source, the high-water mark, the running recording's display name and step, announced when any of them changes; the step changes without a pass | `tst_calculation_demand::progressCountsEachSessionOnce`, `progressTextWithoutAPass`, `changeSignalsAreMinimal` |
| 730 | 10 | failures: for each session with a current failure, in row order, its display name and failed pairs, each with the calculation's title, the reason and whether it is tried again at the next start, announced when the set or an entry changes; asked for the whole list or for one session | `tst_calculation_demand::failuresAreOnePerSessionInRowOrder`, `failuresNameEachCalculationOnce` |
| 731 | 10, as settled | pending cells are unchanged, with an announcement of their own per column | `tst_calculation_demand::pendingCellsChangedPerColumn`; `tst_logbook_indicators::pendingCellsChangeRepaintsOnlyThatColumn`, `pendingCellsAreDistinctFromUnavailable` |
| 732 | 10 | the three values are computed in the one walk from the tracks it classifies and from the pair memory, with no second pass and no second key; counts no view reads are not kept | `tst_calculation_demand::columnProgressAndPendingCells`, `passOverManyStubsReadsEachRecordSetOnce`; `audit demand` |
| 733 | 11 | the status bar is the one place background work is shown: a label naming the work and its count, a bar showing the same, a hover listing everything in progress, a cancel control only for work that can be stopped, and nothing when the application is idle | `tst_status_bar::schedulerTasksShowTheirLabelsAndCounts`, `activityLeftAndWarningRight`, `computationsAreOneItem`, `cancelOnlyForACancellableShownTask`; `manual M34` |
| 734 | 11 | the warning says how many recordings could not be computed for as long as that is true, restart or not, and its hover which and why; it stands alone when nothing is computing | `tst_status_bar::warningBesideComputationsThenAlone`, `warningAfterRestart`; `manual M37` |
| 735 | 11 | the logbook row of a recording that could not be computed carries one glyph whose hover says what failed and why; its cells over the failed calculation are blank; a cell still to come shows the pending mark | `tst_logbook_indicators::rowWarningHoverIsTheSessionsFailures`, `rowWarningFollowsTheText`; `manual M34`, `M36` |
| 736 | 11 | plot rows and column headers show nothing about computing; a track still to come is absent from the plot until it arrives, and the plot widget does not warn about it | `tst_logbook_indicators::headerIsPlainAndNothingAnimates`; `tst_calculation_demand::merelyUncomputedIsNotWorthAWarning`; `audit demand`; `manual M34` |
| 737 | 11 | nothing offers a refresh or a cancel for computations | `tst_logbook_indicators::clickOnRowWarningIsAClickOnTheCell`; `tst_status_bar::cancelOnlyForACancellableShownTask`; `audit demand`, `audit gestures` |
| 738 | 12 | the main window owns the status bar and the component that fills it, created from the application context like the dock features and not a dock; the component reads the scheduler's and the demand layer's values, asks the scheduler to cancel, and decides nothing | `tst_status_bar::survivesDemandDestroyedFirst`; `audit demand`; `manual M34` |
| 739 | 12 | the scheduler keeps its contract, knows nothing of jobs or demand and reports the fill as a task, which the status bar maps to the computations; it gains one read-only query, whether any task has work, so that a test can wait for background work to end | `tst_session_model_engine::schedulerCompletesWaitingTaskWhoseWorkIsGone`, `schedulerHasWorkFollowsItsTasks`; `tst_status_bar::computationsAreOneItem`; `audit demand`, `audit branch-mechanisms` |
| 740 | 12 | the demand layer stays widget-free, the only offerer and observed by nothing that changes what it does; its presentation surface is progress, failures and pending cells | `tst_calculation_demand::nullCollaborators`; `audit widget-free-core`, `audit gestures`, `audit demand` |
| 741 | 12 | the logbook view presents the row glyph from the demand layer's per-session failures and the pending cell, and no task progress; its dock feature connects nothing of the scheduler | `tst_logbook_indicators::rowWarningFollowsTheText`, `headerIsPlainAndNothingAnimates`; `audit demand` |
| 742 | 12 | the plot list presents nothing of the demand layer | `audit demand` |
| 743 | 12 | the application context carries the demand layer for the logbook and the status bar, and no clock | `audit demand` |
| 744 | 12 | the flow stays one way, ending in the demand layer presenting progress and failures as values that the status bar and the logbook read | `tst_calculation_demand::recordReasonReachesDemandThroughRecordChange`, `failedRecordWriteIsShownAndNotRetried`; `tst_logbook_indicators::pendingCellBecomesValueWhenRecordIsWritten`; `audit demand` |
| 745 | 13 | test: the activity area presents the scheduler's tasks with the existing labels and the scheduler's counts, and presents the computations as one item counting sessions with a waiting or running track across plots and columns, out of the high-water mark, which resets when the count reaches zero; the fill is never the shown item, and while it is the scheduler's active task the computations are shown | `tst_status_bar::schedulerTasksShowTheirLabelsAndCounts`, `computationsAreOneItem`; `tst_calculation_demand::progressCountsEachSessionOnce` |
| 746 | 13 | test: with a scheduler task and a computation in progress, the task is the shown item and the computations return when it ends; the hover lists both with their own counts and the recording being computed with its step | `tst_status_bar::taskShownOverComputationsAndHoverListsBoth` |
| 747 | 13 | test: the cancel control is present exactly while the shown item is a cancellable scheduler task, and cancels that task; it is absent for saving and for the computations | `tst_status_bar::cancelOnlyForACancellableShownTask` |
| 748 | 13 | test: the warning is shown while any session in demand has a current failure, counts recordings, lists them in row order with calculation and reason, says which are tried again at the next start, caps the list at ten, is shown alongside the computations while they continue, stands alone once they end, and is absent when nothing has failed | `tst_status_bar::warningBesideComputationsThenAlone`, `warningCountsRecordingsAndListsThem`, `warningListsAtMostTenRecordings`, `warningAbsentWhenNothingFailedAndNotDismissable`; `tst_calculation_demand::failureTextAndItsLimit` |
| 749 | 13 | test: the warning persists through a restart for a stored rejection: with the source restored and the record set reported, the recording is counted without being loaded; a failure that is not stored is absent after a restart until it is tried again and fails again; a retry that succeeds leaves nothing shown | `tst_status_bar::warningAfterRestart`; `tst_calculation_demand::storedRejectionIsAFailureWithoutLoad`, `unstoredFailureReturnsOnlyWhenItFailsAgain` |
| 750 | 13 | test: the warning clears when the pair memory clears or the last source over the calculation is switched off, and nothing dismisses it by hand | `tst_calculation_demand::failuresClearAsThePairMemoryClears`, `failuresFollowWhatIsSwitchedOn`; `tst_status_bar::warningAbsentWhenNothingFailedAndNotDismissable`; `tst_logbook_indicators::rowWarningFollowsFailures` |
| 751 | 13 | test: the logbook row of a recording with a current failure shows one glyph right after the text of its first cell, for a loaded row and for one that is not loaded; its hover lists the failed calculations with reasons; the cells over the failed calculation are blank; a row without a failure takes no room for the glyph; the pending cell is unchanged | `tst_logbook_indicators::rowWarningFollowsTheText`, `rowWarningHoverIsTheSessionsFailures`, `failedLoadSessionShowsRowWarningNotPending`, `plainHeaderAndCellsAreIdenticalToBase` |
| 752 | 13 | test: removed presentation: no plot row or column header paints a glyph or reserves room for one; no clock exists; the logbook view has no progress line and no cancel button and receives no scheduler signal; no product code reads the removed per-source state, queries, signals or lists; the audit's demand group keeps their names out | `tst_logbook_indicators::headerIsPlainAndNothingAnimates`; `audit demand` |
| 753 | 13 | test: the demand layer's progress and failures are computed in the one walk and announced only when they change; the acceptance items of the earlier specifications on demand, priority, the settle wait, failures, not applicable, chains, unloaded sessions and the pair memory pass unchanged in what they assert | `tst_calculation_demand::changeSignalsAreMinimal`, `pendingCellsChangedPerColumn`, `rowScript`, `enablingColumnFillsEveryUnloadedSession`, `settledPairsSurviveEvictionSortAndColumnWorker`; `tst_fusion_rows::realRowScript`; `audit demand` |
| 754 | 14 | docs/ and tests/README.md describe the status bar, the row warning, the demand layer's progress, failures and pending cells and what stays visible after a restart, and no document names the removed surface | `audit demand` |

### 9.9 Sensor fusion plots, attitude and the orientation attribute (items 801-863)

The sixty-three clauses of the specification "Sensor fusion plots, attitude
and the orientation attribute", stated in full in
[appendix I](#appendix-i-the-acceptance-items-of-sensor-fusion-plots-attitude-and-the-orientation-attribute-801-863).
In the map, item = 800 + the clause number; the same four line forms as 9.2,
and every item has at least one test or audit line. "Section" is the section
of the specification; its sections 1-4 (motivation, principles, scope, terms)
have no item. Their principles are carried by the clauses of the sections
that apply them, and the two statements of its section 3 that a test can
observe are clauses 4 (the fit's channels remain measurements) and 40 (mass,
area and the fixed ground elevation stay creation defaults). Clauses 54-62 are
its section 13 tests, one per bullet, and clause 63 its section 14. Clauses 2,
7, 8, 10, 14, 22, 26, 36, 39 and 50 are stated as settled: a fused plot is
coloured apart from its GNSS counterpart; the fused elevation stands on the
GNSS elevation's ground; vertical acceleration is the fit's own down
acceleration; one helper defines the track accelerations and the wind rule for
both categories; heading is a compass heading, measured from north and not
referenced to the course reference, and is available for every fitted
recording; the attribute's key is `_ORIENTATION`, in category "Session",
registered once per process; an invalid stored token is kept and makes the
attitude unavailable; the SP and WS-P defaults move onto the constant-default
helper; the legacy backfill no longer writes wind; the attribute registry
moves to the model library and the helpers the fusion library calls are
header-only. The specification amends those of 9.1 (item 6), 9.2 (item 115)
and 9.6 (item 509). Items 847 and 861 are stated as amended by the
specification "The fused state at every IMU sample" (9.10), and again by the
specification "The documented noise model and the accuracy, part 1" (9.11):
item 847 for the four configuration inputs, both for the algorithm string,
the goldens and the stored results, which are now that specification's.
Items 801, 802, 842, 854, 862 and 863 are stated as amended by the same
specification's plots: the category held twelve plots, the eight as before
and then Heading accuracy, Tilt accuracy, Horizontal acceleration accuracy and
Vertical acceleration accuracy, which have no GNSS counterpart and are named
as the GNSS accuracy plots are. Items 801, 802, 842, 854, 862 and 863 are
stated as amended again by the specification "Fused speed plots" (9.17): the
category holds fifteen plots, Horizontal speed, Vertical speed and Total speed
after Elevation, named, united and typed as the GNSS speeds, so that item 802
speaks of the first eleven. Item 803 is stated as amended by the same
specification: the fused down velocity is drawn as Vertical speed, and the
north and east velocity plots stay removed. The test that held the eight rows
is `allFusionPlotsAreExplicitBacked`, named without a count; items 806, 808,
853 and 856 cite it under that name and hold as they are.

| # | Section | Clause | Evidence |
|---|---|---|---|
| 801 | 5, as amended | the "Sensor fusion" category holds exactly fifteen plots, in this order: Elevation, Horizontal speed, Vertical speed, Total speed, Horizontal acceleration, Vertical acceleration, Along-track acceleration, Cross-track acceleration, Heading, Pitch, Roll, Heading accuracy, Tilt accuracy, Horizontal acceleration accuracy, Vertical acceleration accuracy (the three speeds since the specification of 1601-1612, the last four since the specification of 1001-1065) | `tst_fusion_rows::allFusionPlotsAreExplicitBacked`; `audit naming`; `manual M39`, `M51` |
| 802 | 5, as settled, as amended | each of the first eleven is named, united and typed as its GNSS counterpart (Elevation, the three speeds since the specification of 1601-1612, the four accelerations, Heading as Course), Pitch and Roll as angles, so that the two overlay; a fused plot's colour differs from its counterpart's; the four accuracy plots have no GNSS counterpart and are named as the GNSS accuracy plots are | `tst_fusion_rows::allFusionPlotsAreExplicitBacked`; `manual M39`, `M51`, `M58` |
| 803 | 5, as amended | the fused north, east and down position, north and east velocity, north and east acceleration, roll, pitch, yaw and quaternion plots are removed, and the "GNSS (Local frame)" category with them; the fused down velocity is drawn as Vertical speed since the specification of 1601-1612 | `tst_fusion_rows::allFusionPlotsAreExplicitBacked`; `audit naming`; `manual M39` |
| 804 | 5 | every measurement behind a removed plot still exists and is computed as before: the local frame feeds the fit and a column, and the fit's channels are its record, read by a column kept from before, the stored result and plug-ins | `tst_fusion_rows::removedPlotMeasurementsStayAvailable`; `tst_fusion_jobs::columnOnFusionOutputIsCachedFromRecord`; `manual M44` |
| 805 | 5 | applying a profile enables the listed plots the application has and ignores the rest silently (no message, no log, no failure); nothing rewrites a profile | `tst_calculation_demand::profileNamingRemovedPlotsAppliesWithoutThem`; `manual M43` |
| 806 | 6 | each derived quantity is an on-demand calculation over the fit's outputs, registered with the fit: blocked by it until it publishes, it appears with it and never starts one | `tst_fusion_derived::derivedRegistrationShape`, `derivedValuesWaitOnTheFit`, `attitudeWaitsOnTheFit`; `tst_fusion_rows::accHRowIsBlockedByFusion`, `allFusionPlotsAreExplicitBacked` |
| 807 | 6, as settled | elevation is the local origin's height above mean sea level minus the fused down position, above the ground elevation as the GNSS elevation is, so the two overlay; it is unavailable when either attribute is not a number | `tst_fusion_derived::elevationIsOriginHeightMinusDownAboveGround` |
| 808 | 6, as settled | vertical acceleration is the fit's own fused down acceleration, positive down like the GNSS one; no second calculation publishes it | `tst_fusion_rows::allFusionPlotsAreExplicitBacked`; `tst_fusion_derived::derivedRegistrationShape`; `tst_fusion_session::explicitOutputsHaveOneCandidate` |
| 809 | 6 | along-track and cross-track acceleration are the GNSS definitions applied to the fused velocity and acceleration, wind-corrected, so their signs and the meaning of "track" agree | `tst_fusion_derived::trackAccelerationsKnownAnswers`, `trackAccelerationsAreTheGnssDefinitions` |
| 810 | 6, as settled | one definition of the track-relative accelerations and of the wind rule serves both categories, and the GNSS values are unchanged | `tst_fusion_derived::trackAccelerationsAreTheGnssDefinitions`; `tst_builtins_golden::sessionDataMatchesGolden`; `tst_builtins_engine::goldenOnEngine` |
| 811 | 7 | the orientation's forward and up axes fix the body frame, right being their cross product; the body-to-device rotation is a constant signed axis permutation, a proper rotation for each of the 24 pairs, never a reflection | `tst_fusion_derived::orientationRotationIsProper` |
| 812 | 7 | heading, pitch and roll are the aircraft Euler angles of the fit's quaternion composed with that rotation: heading the forward axis's direction clockwise from north, pitch its elevation above the horizontal, roll the rotation about it, positive right side down | `tst_fusion_derived::levelNorthFacingBodyReadsZero`, `knownAnglesComeBack`, `sideMountPermutesTheAngles`, `deviceFrameMountGivesTheFitsOwnAngles`; `manual M40` |
| 813 | 7 | they are computed when read, from the quaternion channels and the orientation attribute, never by the fit | `tst_fusion_derived::attitudeRegistrationShape`, `attitudeWaitsOnTheFit`, `storedOrientationRecomputesWithoutAFit`; `tst_fusion_session::registrationShape` |
| 814 | 7, as settled | heading is unwrapped by the application's one unwrap rule, the course's, and measured from north: a compass heading, not referenced to the course reference (GNSS course is relative to its reference, so the two are not meant to overlay); the attitude reads the quaternion and the orientation only, so heading, pitch and roll are available for every fitted recording | `tst_fusion_derived::headingUnwrapsThroughAFullTurn`, `attitudeRegistrationShape`; `manual M40` |
| 815 | 7 | pitch and roll are reported in their natural ranges, pitch from -90 to 90 degrees and roll from -180 to 180 | `tst_fusion_derived::rollAndPitchStayInTheirNaturalRanges` |
| 816 | 7 | where the forward axis is vertical, heading and roll are not defined and the derivation reports what the standard formulas give | `tst_fusion_derived::forwardAxisVerticalGivesTheStandardFormulas` |
| 817 | 7 | a change of the orientation attribute recomputes the angles through ordinary invalidation and never refits | `tst_fusion_derived::storedOrientationRecomputesWithoutAFit`; `manual M41` |
| 818 | 8 | the orientation is a token naming the forward and the up axis in a fixed form the fusion module owns (the default `+y,+z`); the 24 valid pairs, and only they, are its choices, labelled "forward +y, up +z" and so on, the default first | `tst_fusion_derived::orientationVocabularyHasTwentyFourPairs`, `orientationDefinitionIsTheEnumeration` |
| 819 | 8, as amended | its constant default, forward +y and up +z, is a calculated attribute, so a recording without a stored orientation has one with nothing written to its file; a new import stores the orientation of the Import preferences (the same default), a fact of the import, editable afterwards | `tst_fusion_derived::orientationColumnShowsTheDefaultWithoutAWrite`, `storedOrientationRecomputesWithoutAFit`, `attitudeRegistrationShape`; `tst_importer::orientationFromThePreference`; `manual M41` |
| 820 | 8, as amended | a stored value wins over the default; nothing an edit can carry removes a stored orientation | `tst_fusion_derived::storedOrientationRecomputesWithoutAFit`, `orientationEditStoresATokenAndRefusesOthers` |
| 821 | 8 | one orientation type in the fusion module owns the vocabulary (the axis pair, token, label, body-frame rotation, default and enumeration); the derivation parses with it, the attribute's choices come from it, and nothing else spells a token | `tst_fusion_derived::orientationDefinitionIsTheEnumeration`, `orientationVocabularyHasTwentyFourPairs`; `audit orientation` |
| 822 | 8, as settled | the fusion registration registers the attribute's definition once per process (key `_ORIENTATION`, "Orientation", category "Session", a choice attribute, editable) with the derivation that reads it; the key is part of the session file's vocabulary | `tst_fusion_derived::orientationDefinitionIsTheEnumeration`, `attitudeRegistrationShape`, `orientationEditStoresATokenAndRefusesOthers`; `manual M41` |
| 823 | 8 | it is edited as any editable attribute: a logbook column the user can add, not among the default columns, edited in place, and set for the selected sessions from the context menu | `tst_fusion_derived::orientationEditStoresATokenAndRefusesOthers`, `orientationBulkEdit`; `tst_choice_attribute::cellEditorOffersTheList`, `setDialogOffersTheList`; `manual M41`, `M42` |
| 824 | 8, as amended | both editors offer the 24 orientations and nothing else: no free text and no entry that removes the stored value | `tst_choice_attribute::cellEditorOffersTheList`, `setDialogOffersTheList`; `tst_fusion_derived::orientationBulkEdit`; `manual M41`, `M42` |
| 825 | 8 | a value outside the list is refused by the model, the in-place editor and the bulk edit | `tst_fusion_derived::orientationEditStoresATokenAndRefusesOthers`, `orientationBulkEdit`; `tst_choice_attribute::cellEditorOffersTheList` |
| 826 | 8, as settled | a stored token outside the list (a hand-edited file) is kept and shown as written, and heading, pitch and roll are unavailable for it until it is changed or removed | `tst_fusion_derived::invalidStoredOrientationMakesAttitudeUnavailable`; `tst_choice_attribute::choiceShowsTheLabelOfTheEffectiveToken`; `manual M42` |
| 827 | 9 | the attribute registry's format types gain a fifth, choice, whose definition carries its allowed values, each a stored token and a display label | `tst_choice_attribute::choiceShowsTheLabelOfTheEffectiveToken`, `choiceEditStoresAToken`; `tst_fusion_derived::orientationDefinitionIsTheEnumeration` |
| 828 | 9 | a choice is displayed and sorted by its label | `tst_choice_attribute::choiceShowsTheLabelOfTheEffectiveToken`, `choiceSortsByLabel` |
| 829 | 9 | in-place editing of a choice gives a list editor in place of the line edit | `tst_choice_attribute::cellEditorOffersTheList`; `manual M41` |
| 830 | 9 | the context menu's "Set ..." action offers a list in place of the text prompt | `tst_choice_attribute::setDialogOffersTheList`; `manual M42` |
| 831 | 9 | the bulk edit sets the chosen token for the selected sessions, loaded or not | `tst_choice_attribute::bulkEditSetsAToken`; `tst_fusion_derived::orientationBulkEdit`; `manual M42` |
| 832 | 9, as amended | a token outside the list, an empty and an invalid value are refused on every edit path; no edit removes a stored attribute | `tst_choice_attribute::choiceEditRefusesATokenOutsideTheList`, `bulkEditRefusesATokenOutsideTheList` |
| 833 | 9 | nothing about the type is specific to orientation: any choice definition gets all of it | `tst_choice_attribute::choiceEditStoresAToken`, `cellEditorOffersTheList` |
| 834 | 10 | a constant default is a registered calculation with no inputs whose one output is the attribute: it runs once and is cached, a stored value wins, setting or removing a stored value invalidates what read the attribute, and a dependent follows every change | `tst_calcengine::constantCalculationIsADefault`; `tst_builtins_engine::constantDefaults` |
| 835 | 10 | one helper registers a constant default for an attribute key and a value, beside the exit-time defaults, so that every constant default is found by one search; the orientation default uses it from the fusion registration | `tst_builtins_engine::constantDefaults`; `tst_fusion_derived::attitudeRegistrationShape`; `audit constant-defaults` |
| 836 | 10, as settled | the seven constant defaults of the SP and WS-P calculations are registered through the same helper, with their values | `tst_builtins_engine::constantDefaults`, `inventory`; `audit constant-defaults` |
| 837 | 10 | the importer stores only what is a fact of the import; anything that stands in for a value the user has not set is a calculation, derived where possible, constant otherwise | `tst_importer::creationDefaultsOnlyFillAbsent`; `tst_smoke::importAppliesCreationDefaults`; `audit constant-defaults` |
| 838 | 10 | wind north and east are constants of zero: a recording without stored wind reads zero, one with stored wind keeps it, and the importer no longer writes them | `tst_builtins_engine::constantDefaults`; `tst_smoke::importAppliesCreationDefaults`; `tst_importer::creationDefaultsOnlyFillAbsent`; `tst_import_merge::newSessionGetsDefaults`; `audit constant-defaults` |
| 839 | 10, as settled | the logbook's legacy backfill no longer writes wind either; it adds jumper mass and planform area only | `tst_persistence_roundtrip::releasedLogbookBackfillIsAdditive`; `tst_logbook_index::rawLoadSkipsBackfill`; `audit constant-defaults` |
| 840 | 10 | jumper mass, planform area and the fixed ground elevation stay creation defaults of the importer (section 3) | `tst_importer::creationDefaultsOnlyFillAbsent`, `fixedGroundElevationOnlyInFixedMode`; `tst_smoke::importAppliesCreationDefaults` |
| 841 | 10, as amended | a stored attribute wins even when invalid or empty, so no edit path stores a blank as a way back to a default; the engine's removal of a stored value falls back to the calculation | `tst_calcengine::storedInvalidValueStillWins`, `removingStoredFallsBackToCalc`; `tst_choice_attribute::choiceEditRefusesATokenOutsideTheList`, `bulkEditRefusesATokenOutsideTheList` |
| 842 | 11, as amended | the plot list shows "Sensor fusion" with fifteen plots, the eleven named as the GNSS plots (the three fused speeds after Elevation since the specification of 1601-1612) and the four accuracies (since the specification of 1001-1065) named as the GNSS accuracy plots, and no local-frame category; checking a fusion plot starts the fit as before, the status bar shows it, and a stored fit draws at once | `tst_fusion_rows::allFusionPlotsAreExplicitBacked`, `realRowScript`; `tst_fusion_store::restoredAfterRestartIsBitIdentical`; `manual M39`, `M51` |
| 843 | 11 | the attitude plots are in the aircraft convention for the mount the orientation describes, heading continuous through turns | `tst_fusion_derived::sideMountPermutesTheAngles`, `headingUnwrapsThroughAFullTurn`; `manual M40` |
| 844 | 11, as amended | the orientation column, once added, shows "forward +y, up +z" for every recording not set and the chosen label for one that is; editing offers the 24 orientations; changing it redraws the attitude plots without a fit; the Import preferences page offers the same list for new imports | `tst_fusion_derived::orientationColumnShowsTheDefaultWithoutAWrite`, `orientationEditStoresATokenAndRefusesOthers`, `storedOrientationRecomputesWithoutAFit`; `tst_choice_attribute::cellEditorOffersTheList`; `manual M41` |
| 845 | 11 | profiles that name removed plots apply without complaint | `tst_calculation_demand::profileNamingRemovedPlotsAppliesWithoutThem`; `manual M43` |
| 846 | 11 | wind reads zero where nothing was stored, as before | `tst_builtins_engine::constantDefaults`; `tst_smoke::importAppliesCreationDefaults` |
| 847 | 12, as amended | the fusion plots leave the fusion kernel and the fit calculation as they are (inputs, outputs, diagnostics, algorithm string), except that the specification of 1001-1065 adds the four configuration inputs, and derive what they add from the fit's published outputs, whose values, diagnostics and algorithm string are those of the specification of 1001-1065, with its goldens and stored results | `tst_fusion_session::registrationShape`; `tst_fusion_golden::successFixturesMatchGolden`, `rejectionFixturesMatchGolden`; `tst_fusion_kernel::fitTraceMatchesGolden`; `tst_fusion_store::restoredAfterRestartIsBitIdentical` |
| 848 | 12 | GTSAM stays confined to the fusion kernel, the new fusion files include neither GTSAM nor Eigen, and the rules on the fusion tooling hold | `audit solver-confinement`, `audit fusion-tooling` |
| 849 | 12 | the fusion registration gains the derived kinematics, the attitude derivation, the orientation type and the orientation attribute with its constant default; each derivation is an ordinary on-demand calculation with declared inputs, the attitude's including the orientation attribute | `tst_fusion_session::registrationShape`; `tst_fusion_derived::derivedRegistrationShape`, `attitudeRegistrationShape` |
| 850 | 12, as settled | the fusion library reaches nothing of the core library: the attribute registry lives in the model library, the constant-default and track helpers are header-only, and the calculations never name the fusion library | `tst_fusion_derived::orientationDefinitionIsTheEnumeration`; `audit solver-confinement` |
| 851 | 12 | the attribute calculations hold the constant-default helper and the wind defaults; the importer holds neither | `tst_builtins_engine::constantDefaults`; `audit constant-defaults` |
| 852 | 12 | the plot registry loses the removed plots and the local-frame category; the profile rule is the plot model's, one function the profile bridge calls | `tst_calculation_demand::profileNamingRemovedPlotsAppliesWithoutThem`, `profileStyleApplyCreatesDemand`; `audit naming` |
| 853 | 12 | the demand layer is untouched: a derived plot's blockers lead to the fit through the chain it already follows, and heading, pitch and roll are filled by one job | `tst_fusion_rows::accHRowIsBlockedByFusion`, `headingPitchRollShareOneJob`, `allFusionPlotsAreExplicitBacked`; `audit demand` |
| 854 | 13, as amended | test: the plot list has the fifteen fusion plots (the four accuracies since the specification of 1001-1065, the three fused speeds after Elevation since the specification of 1601-1612) and no local-frame plots; every fusion plot is explicit-backed (waits on the fit) as the seventeen were; an existing column over a removed plot's measurement still computes, and the export of the fit, its stored record, still holds the measurement | `tst_fusion_rows::allFusionPlotsAreExplicitBacked`, `removedPlotMeasurementsStayAvailable`, `noImuSessionIsNeverCounted`; `tst_fusion_session::missingInputsAreNotApplicable`; `tst_fusion_jobs::noImuSessionCannotHaveAJob` |
| 855 | 13 | test: applying a profile that names a removed plot enables its other plots and ignores that one, silently | `tst_calculation_demand::profileNamingRemovedPlotsAppliesWithoutThem` |
| 856 | 13 | test: elevation equals the origin height minus down minus the ground elevation, and is unavailable without a ground elevation; vertical acceleration is the down acceleration; along-track and cross-track equal the GNSS definitions applied to the fused velocity and acceleration, on synthetic data with known answers | `tst_fusion_derived::syntheticOutputsAreServedWithoutAFit`, `elevationIsOriginHeightMinusDownAboveGround`, `trackAccelerationsKnownAnswers`, `trackAccelerationsAreTheGnssDefinitions`, `derivedRegistrationShape`; `tst_fusion_rows::allFusionPlotsAreExplicitBacked` |
| 857 | 13 | test: attitude, on synthetic quaternions with known answers: for the default orientation, heading, pitch and roll of a level, north-facing body are zero; a known heading, pitch and roll come back; heading unwraps through a full turn and is measured from north, unchanged by a GNSS track or a stored course reference of any kind and available without GNSS data; a different orientation (a side mount) changes the angles as the axis permutation predicts; the rotation is proper for all 24 pairs | `tst_fusion_derived::levelNorthFacingBodyReadsZero`, `knownAnglesComeBack`, `headingUnwrapsThroughAFullTurn`, `sideMountPermutesTheAngles`, `orientationRotationIsProper` |
| 858 | 13, as amended | test: the orientation attribute: every recording reads the default without a stored value and without any write to its file; a stored token wins and the attitude recomputes without a fit; a token outside the list, an empty and an invalid value are refused by the model, the editor and the bulk edit; the labels and tokens come from one enumeration of 24; a new import stores the preference's orientation | `tst_fusion_derived::orientationColumnShowsTheDefaultWithoutAWrite`, `storedOrientationRecomputesWithoutAFit`, `orientationEditStoresATokenAndRefusesOthers`, `orientationBulkEdit`, `tst_importer::orientationFromThePreference`, `orientationDefinitionIsTheEnumeration`; `tst_choice_attribute::cellEditorOffersTheList` |
| 859 | 13 | test: the choice type: display and sort by label; the in-place editor and the context menu offer the list; the bulk edit sets the token for the selected sessions | `tst_choice_attribute::choiceShowsTheLabelOfTheEffectiveToken`, `choiceSortsByLabel`, `cellEditorOffersTheList`, `setDialogOffersTheList`, `bulkEditSetsAToken` |
| 860 | 13 | test: constant defaults: the helper registers a calculation that the engine serves as a default (the existing engine test covers the mechanism); wind north and east read zero for a recording without stored wind, keep a stored value, and the importer no longer writes them | `tst_builtins_engine::constantDefaults`; `tst_calcengine::constantCalculationIsADefault`; `tst_smoke::importAppliesCreationDefaults`; `tst_importer::creationDefaultsOnlyFillAbsent`; `tst_import_merge::newSessionGetsDefaults` |
| 861 | 13, as amended | test: the fusion plots leave the fit as the golden fixtures and the stored-result tests hold it, against the goldens and the algorithm string of the specification of 1001-1065 | `tst_fusion_golden::successFixturesMatchGolden`, `rejectionFixturesMatchGolden`; `tst_fusion_kernel::fitTraceMatchesGolden`; `tst_fusion_store::restoredAfterRestartIsBitIdentical`, `codeStampChangeDropsRecordOnLoad`; `tst_fusion_session::registrationShape` |
| 862 | 13, as amended | test: the audit keeps the removed plot names out of the registry, and the documents describe the fifteen plots (the four accuracies since the specification of 1001-1065, the three fused speeds after Elevation since the specification of 1601-1612), the attitude convention, the orientation attribute and the rule for defaults | `audit naming` |
| 863 | 14, as amended | `docs/` describes the fifteen fusion plots (the four accuracies since the specification of 1001-1065, the three fused speeds after Elevation since the specification of 1601-1612) and the derived quantities, the attitude convention with the orientation attribute, its default and its limits (it describes the mount, not the wearer's posture; a forward axis pointing straight up or down makes heading and roll meaningless), the choice type and the orientation in the session file, the rule for defaults and its helper, and that wind is no longer written at import; no document describes a removed plot or the "GNSS (Local frame)" category | `audit naming` |

### 9.10 The fused state at every IMU sample (items 901-940)

The forty clauses of the specification "The fused state at every IMU sample",
stated in full in
[appendix J](#appendix-j-the-acceptance-items-of-the-fused-state-at-every-imu-sample-901-940).
In the map, item = 900 + the clause number; the same four line forms as 9.2,
and every item has at least one test or audit line. "Section" is the section
of the specification; its sections 1-4 (motivation, principles, scope, terms)
have no item. Their principles are carried by the clauses of the sections
that apply them, and the statements of its sections 2 and 3 that a test can
observe are clauses 12 (the published channels, as amended), 16
(one time axis), 20 (the record format), 25 (the fit and its estimate at the
fixes unchanged), 27 (one authority for the integration) and 30 (the
registration and everything above it). Clauses 31-39 are its section 10
tests, one per bullet, and clause 40 its section 11, which also carries the
documentation sentences of its sections 2 and 6; clause 17 carries its
section 7's "the documentation and the diagnostics say the same". Clauses 4,
5, 10, 11, 14, 15, 17, 18, 19, 21, 22, 24, 29, 31 and 34-37 are stated as
settled, and where the plan departs from the specification's letter they say
so: the mismatch is the fitted state in the local coordinates of the forward
state, not the IMU factor's residual, which the library takes the other way
round (4); the sharing formula is applied in the preintegration's tangent
coordinates, where the covariance lives, and mapped into each forward state's
local coordinates (5); a step correction takes the reading at each edge of the
step, rotated by the corrected attitude at that edge (10); with a zero
mismatch the step corrections are zero only where the recording does not
rotate, not wherever the mismatch is zero as the specification says (11, 34);
only a sample exactly on the first fix has one adjacent step, where the
specification names the first and the last sample of the fitted interval
(14); the spread is the bound `docs/SENSOR_FUSION.md` section 8 states, and
the fitted interval as a whole is reproduced only to within it, the terms at
the fixes inside it included, not exactly as the specification's sections 2
and 6 say: its own part-step rule leaves the corrections of the part-steps
beside a fix uncancelled (15, 35);
the diagnostics' new keys are named, `dense_output` replacing the previous
account (17, 18); the algorithm string of a rejected or failed fit does
change, where the specification calls those diagnostics unchanged (19); the
string is `batch-temperature-bias-v4`, then `v5` with the mid-step rotation (and `v6`
since the documented noise model of 9.11) (21); a stale fit is computed again when
something switched on needs it, not all at the next start (22); the time axis
was compared before and after once, at the re-capture (section 11), and is
held permanently against its definition, not by a test that compares two
captures (24, 36); the pass is not a cancellation boundary (29); the
equivalence reference holds the fix states and the biases at the fit's
values, not the GNSS factors and free biases the specification describes
(31); in portable mode the time of the largest step correction, an argmax,
is not compared at all (37). The
specification amends those of 9.9 (items 847 and 861). Items 915, 921, 925,
930 and 935 are stated as amended by the specification "The documented noise
model and the accuracy, part 1" (9.11): the algorithm string `v6`, the noise
model the pass reads, the configuration inputs and the four accuracy plots
the plot registry gains, and the bound on the
published acceleration, which also includes the kink of the rotated reading
at a fix (the kernel has not changed; under the datasheet's noise the
corrections are small enough that this term shows). Item 912 is stated as
amended by the same specification's accuracy, and again by the specification
"Fused position and speed accuracy" (9.18): the fit publishes thirty-three
channels, the uncertainty among them.

| # | Section | Clause | Evidence |
|---|---|---|---|
| 901 | 5, as amended | for every fix interval of a fit that succeeds, the pass integrates gyro and accelerometer together from the fitted state at the interval's first fix with the biases and scale factors the fit assigns to that interval: the attitude advances with the gyro, the reading, divided by the scale and bias removed, is rotated by it and gravity added, and velocity and position advance with the result | `tst_fusion_kernel::imuRateMatchesHeldEndsGraph`, `imuRateEndsAreTheFit`, `reconstructionUsesIntervalBias`, `reconstructionUsesTheFittedScale`, `exactConstantVelocityFit` |
| 902 | 5 | the integration is the fit's own: its edges are every IMU sample in the interval and the two fix times, with the same midpoint interpolation of the readings and the same per-step noise as the fit's IMU factors | `tst_fusion_kernel::imuRateEndsAreTheFit`; `audit fusion-model` |
| 903 | 5 | the pass accumulates at every edge the covariance of the noise the steps so far have added, from the transition and noise of the solver library's own preintegration update; at the second fix it is the covariance of the fit's IMU factor for the interval | `tst_fusion_kernel::imuRateEndsAreTheFit`; `audit fusion-model` |
| 904 | 5, as settled | the mismatch is the fitted state at the interval's second fix in the local coordinates of the forward state there, nine components; not the IMU factor's residual, which the library takes the other way round and whose negative misses the fitted end | `tst_fusion_kernel::imuRateEndsAreTheFit` |
| 905 | 5, as settled | the correction at each edge is the conditional mean of the step chain given both ends, linearized about the forward states, computed in the preintegration's tangent coordinates, where the covariance lives, and mapped into each forward state's local coordinates by the Jacobian of the library's retraction; the formula applied literally in the local coordinates is wrong at first order under rotation | `tst_fusion_kernel::imuRateMatchesHeldEndsGraph`, `imuRateMatchesHeldEndsGraphUnderRotation` |
| 906 | 5 | the share is nothing at the first fix and the whole mismatch at the second, one nine-component correction applied jointly, so that an attitude share turns the direction of every later reading | `tst_fusion_kernel::imuRateEndsAreTheFit`, `imuRateMatchesHeldEndsGraph`, `imuRateMatchesHeldEndsGraphUnderRotation`, `imuRateSampleOnAFixIsPublishedOnce`, `imuRateSharesByNoise` |
| 907 | 5 | where the per-step noise is uniform the velocity share grows with elapsed time; a step that carries more noise takes more of the mismatch | `tst_fusion_kernel::imuRateSharesByNoise` |
| 908 | 5 | the corrected state is the forward state with its share applied by the retraction that inverts the local coordinates; at the second fix it is the fitted state, exactly | `tst_fusion_kernel::imuRateEndsAreTheFit`, `imuRateSampleOnAFixIsPublishedOnce` |
| 909 | 5 | the corrections are computed once, about the forward states, and applied once; the ends are exact by construction, and the linearization costs only the split inside the interval, within the equivalence test's bound | `tst_fusion_kernel::imuRateMatchesHeldEndsGraph`, `imuRateMatchesHeldEndsGraphUnderRotation`, `imuRateEndsAreTheFit` |
| 910 | 5, as settled, as amended | a step correction for every step: the corrected velocity change over the step length, less the mean of the readings at its two edges, divided by the scale, bias removed, each rotated by the corrected attitude at its own edge, less gravity (at a fix the reading is the one the integration uses there) | `tst_fusion_kernel::imuRateAccelerationIntegratesToVelocity`, `imuRateZeroMismatchIsForward` |
| 911 | 5, as settled | where the mismatch is zero the pass is the forward integration; the step corrections are zero where the recording does not rotate, and under rotation they are minus the integration's rotation lag | `tst_fusion_kernel::imuRateZeroMismatchIsForward`, `imuRateZeroMismatchUnderRotation` |
| 912 | 6, as amended | the fit publishes the thirty-three channels of the fusion document's section 4, the uncertainty among them since the specification of 1001-1065 and the position and velocity covariance blocks since that of 1701-1717, on the same time axis | `tst_fusion_session::registrationShape`; `tst_fusion_runner::outputTableMatchesGolden`; `tst_fusion_kernel::imuRateIsWhatTheFitPublishes`, `imuRateAxisWhenGnssIsFasterThanImu`; `manual M46` |
| 913 | 6 | at every IMU sample the attitude (the quaternion, and roll, pitch and yaw from it) is the corrected attitude at the sample's edge, and velocity and position are the corrected state there | `tst_fusion_kernel::imuRateIsWhatTheFitPublishes`, `imuRateMatchesHeldEndsGraph`, `exactConstantVelocityFit`; `manual M45` |
| 914 | 6, as settled, as amended | the acceleration is the sample's own reading, divided by the fitted scale, bias removed, rotated by the corrected attitude, plus gravity, plus the mean of the corrections of the steps beside its edge (a step beside a fix being the part-step in that fix interval); only a sample exactly on the first fix has one such step, since the last fix is never published; where the corrections are negligible it is the rotated reading | `tst_fusion_kernel::imuRateZeroMismatchIsForward`, `reconstructionUsesTheFittedScale`, `imuRateAccelerationIntegratesToVelocity`, `imuRateIsWhatTheFitPublishes`, `imuRateSampleOnAFixIsPublishedOnce`; `manual M45` |
| 915 | 6, as settled, as amended | integrated by the kernel's rule, the published acceleration reproduces the published velocity change over any run of samples, the whole fitted interval included, to within the spread of the corrections at its ends and at the fixes inside it plus, for a sample step that contains a fix, the difference between the part-steps' trapezoids of the rotated reading and the sample step's (the kink at the fix), the bound the fusion document's section 8 states; not step by step | `tst_fusion_kernel::imuRateAccelerationIntegratesToVelocity` |
| 916 | 6 | the time axis is the IMU samples in the half-open interval from the first fix to the last, whatever the GNSS rate, a GNSS rate above the IMU's included: no fix time added, nothing resampled, a sample on a fix published once | `tst_fusion_kernel::imuRateEndsAreTheFit`, `imuRateSampleOnAFixIsPublishedOnce`, `imuRateIsWhatTheFitPublishes`, `imuRateAxisWhenGnssIsFasterThanImu`; `manual M45`, `M46` |
| 917 | 7, as settled | the success diagnostics' account of the dense output names the reconstruction (`dense_output`, in place of the previous key), and `limitations` no longer denies that the output is the IMU-rate posterior; the documentation quotes both texts | `tst_fusion_kernel::initializerDiagnosticsShape`; `tst_fusion_golden::successFixturesMatchGolden`; `audit fusion-reconstruction`; `manual M46` |
| 918 | 7, as settled | the success diagnostics report the largest step correction (`max_step_correction_m_s2`), the middle of its step in seconds since the epoch (`max_step_correction_time_s`) and the largest velocity mismatch of any interval (`max_velocity_mismatch_m_s`), beside `max_endpoint_correction_deg`, which stays | `tst_fusion_kernel::imuRateEndsAreTheFit`, `imuRateIsWhatTheFitPublishes`, `reconstructionUsesIntervalBias`, `reconstructionTimingAndEndpointCorrection`; `tst_fusion_golden::successFixturesMatchGolden`, `comparatorHoldsItsBounds`; `manual M46` |
| 919 | 7, as settled | the diagnostics of a rejected or failed fit are unchanged but for the algorithm string, which changes for every outcome (the specification's section 8) | `tst_fusion_golden::rejectionFixturesMatchGolden`; `tst_fusion_kernel::failureDiagnosticsShape` |
| 920 | 8 | the record stores and restores the same channels, with the new values, in the record format as it is | `tst_fusion_store::restoredAfterRestartIsBitIdentical`; `tst_result_records::layoutIsPinned` |
| 921 | 8, as settled, as amended | the algorithm string changes (to `batch-temperature-bias-v4` with the reconstruction, to `v5` with the mid-step rotation of the accelerometer reading that followed it, and to `v6` with the specification of 1001-1065), so a fit stored under the previous one is stale at its recording's next load, by the existing validity rules | `tst_fusion_store::codeStampChangeDropsRecordOnLoad`; `tst_fusion_session::registrationShape`; `audit stored-results`; `manual M46` |
| 922 | 8, as settled | after the change every stored fit is computed again once, when something switched on needs it, not all at the next start (a stale record is deleted at its recording's load), and the status bar shows it as any computation | `tst_fusion_store::codeStampChangeDropsRecordOnLoad`; `tst_result_columns::staleRecordDeletedByWorkerCreatesDemand`; `manual M45` |
| 923 | 8 | the goldens are captured again from the changed kernel with the existing capture tool, and the golden tests compare against them | `tst_fusion_golden::successFixturesMatchGolden`, `rejectionFixturesMatchGolden`; `tst_fusion_kernel::fitTraceMatchesGolden`; `tst_fusion_runner::outputTableMatchesGolden` |
| 924 | 8, as settled | every fixture's time axis is unchanged: checked once, at the re-capture, each success fixture's `_time` byte for byte against the previous capture (section 11), and permanently against the IMU samples of its fitted interval | `tst_fusion_kernel::imuRateIsWhatTheFitPublishes`, `imuRateEndsAreTheFit` |
| 925 | 9, as amended | the fit is unchanged by the pass (the specification of 1001-1065 changes the noise model, the damping ceiling and the stopping rules, and adds the scale state to the graph), and so are its states at the fixes, its biases and its scale factors: the pass reads the converged solution (states, biases and scale factors) and changes nothing in it | `tst_fusion_kernel::fitTraceMatchesGolden`, `imuRateEndsAreTheFit`; `tst_fusion_golden::successFixturesMatchGolden`; `manual M46` |
| 926 | 9 | the pass replaces the kernel's linear reconstruction, which no code, test, golden or document names | `tst_fusion_kernel::imuRateIsWhatTheFitPublishes`; `audit fusion-reconstruction` |
| 927 | 9 | the step model is read from the library's preintegration as it advances, never written again: one integration call and one author of the per-step covariance, and the tests check the pass's covariance against the factor's | `tst_fusion_kernel::imuRateEndsAreTheFit`; `audit fusion-model` |
| 928 | 9 | GTSAM stays confined to the kernel, and the rules on the fusion tooling hold | `audit solver-confinement`, `audit fusion-tooling` |
| 929 | 9, as settled | the pass runs after the fit's last iteration, inside the same cancellable job, and is not a cancellation boundary | `tst_fusion_golden::cancelAtEachKindOfBoundary`, `progressMatchesGoldenBoundaries`; `audit fusion-model` |
| 930 | 9, as amended | the fit calculation publishes the same channels and its result version follows the algorithm string; the registration, the plot registry and everything above them are unchanged by the reconstruction; since the specification of 1001-1065 the registration also declares the four configuration inputs and registers their constant defaults, and the plot registry gains the four accuracy plots | `tst_fusion_session::registrationShape`; `tst_fusion_rows::allFusionPlotsAreExplicitBacked`; `audit stored-results` |
| 931 | 10, as settled | test: equivalence: against a graph with a state at every edge, one-step IMU factors with the same per-step covariance and interval bias, and the fix states and biases held at the fit's values, the reconstruction agrees within a tolerance stated against the mismatch, on `coarse_maneuver` and on a rotating recording; a reference with free fix states and biases drifts along the unobservable heading, so that no such tolerance exists for it | `tst_fusion_kernel::imuRateMatchesHeldEndsGraph`, `imuRateMatchesHeldEndsGraphUnderRotation` |
| 932 | 10 | test: on every success fixture, the corrected state at each interval's second fix is the fitted state to rounding, and the pass's covariance there equals the factor's | `tst_fusion_kernel::imuRateEndsAreTheFit` |
| 933 | 10 | test: a step carrying more noise takes the larger share; with uniform noise the velocity share grows with elapsed time | `tst_fusion_kernel::imuRateSharesByNoise` |
| 934 | 10, as settled | test: on a recording that does not rotate and whose readings integrate exactly to the fitted states, the reconstruction is the forward integration, the step corrections are zero and the acceleration is the rotated reading | `tst_fusion_kernel::imuRateZeroMismatchIsForward` |
| 935 | 10, as settled, as amended | test: on every success fixture the published acceleration integrated by the kernel's rule reproduces the published velocity change over runs of samples, the whole fitted interval included, within the stated bound, the kink of the rotated reading at a fix included | `tst_fusion_kernel::imuRateAccelerationIntegratesToVelocity` |
| 936 | 10, as settled | test: the published time axis of every success fixture and of a synthetic recording whose GNSS rate exceeds its IMU rate is exactly the IMU samples of the fitted interval; the before-and-after identity is the re-capture's check | `tst_fusion_kernel::imuRateEndsAreTheFit`, `imuRateSampleOnAFixIsPublishedOnce`, `imuRateIsWhatTheFitPublishes`, `imuRateAxisWhenGnssIsFasterThanImu` |
| 937 | 10, as settled | test: the diagnostics carry the account of the specification's section 7, and the golden comparison covers it; the time of the largest step correction, an argmax, is compared in exact mode only | `tst_fusion_kernel::initializerDiagnosticsShape`, `imuRateIsWhatTheFitPublishes`; `tst_fusion_golden::successFixturesMatchGolden`, `comparatorHoldsItsBounds` |
| 938 | 10 | test: a fit stored under the previous algorithm string is stale at the next load, and one stored after the change restores bit for bit | `tst_fusion_store::codeStampChangeDropsRecordOnLoad`, `restoredAfterRestartIsBitIdentical` |
| 939 | 10 | test: the audit's confinement rules hold, and the documents describe the reconstruction, what it publishes and what its one pass leaves out | `audit solver-confinement`, `audit fusion-tooling`, `audit fusion-model`, `audit fusion-reconstruction` |
| 940 | 11 | `docs/` describe the reconstruction and that its output is the IMU-rate posterior to within one linearization, what that leaves out, what is published (the acceleration's consequence, the time axis in one sentence), the diagnostics keys, the algorithm string and the first start, and what the fusion plots show between fixes; tests/README.md section 11 records the goldens captured again and why; the map gives the specification its range, with the amended items restated | `audit fusion-reconstruction`, `audit fusion-model` |

### 9.11 The documented noise model and the accuracy, part 1 (items 1001-1065)

The sixty-five clauses of the specification "The documented noise model and
the accuracy, part 1", stated in full in
[appendix K](#appendix-k-the-acceptance-items-of-the-documented-noise-model-and-the-accuracy-part-1-1001-1065).
In the map, item = 1000 + the clause number; the same four line forms as 9.2.
"Section" is the section of the specification; its sections 1-4 (motivation,
principles, scope, terms) have no item. Their principles are carried by the
clauses of the sections that apply them. Clauses 50-64 are its section 11
tests and clause 65 its section 12. Every item has at least one test or
audit line, and the audit checks the whole range. Clauses 6, 8, 9, 10, 11,
14, 15, 18, 20, 24, 25, 26, 31, 35, 37, 44, 55 and 61 are stated as settled,
and where the plan departs from the specification's letter they say so: only the four IMU keys
are inputs of the fit, the dynamic model, the GNSS rate and the other
sensors' rates being read and stored and used by nothing, where the
specification's architecture has the registration declare "them" (6); the
quantization step and the lattice, in effective units, are the gyro's
sensitivity (70 mdps at +/-2000 deg/s, 1.14688 times the range over 32768)
and the accelerometer's range over 32768, which the printed sensitivity
rounds: for the gyro not the range divided by 32768 that the specification
writes (8, 11); the lattice check reads
the readings the kernel receives, before any correction of the kernel's own,
not the raw readings, which only the conversion layer may read (9); the
oscillator's tolerance is 10 %, where "a few percent" read as 2-3 % would
reject every recording on disk (10); the sampling term of a step that is part
of a sample interval (a fix splits most intervals) is the derivation's
integral over the part, not `dt^3 / 12` of the part, which would understate
a half-interval part fourfold, and the second derivative is estimated per
sample interval as the larger change of slope at its two ends, the
specification's "bounded by" (14); the rotation remainder includes the same
second-order expansion's terms in the change of force and of rate (coning
included), which on real data are larger than the pure rotation term (15);
the sensitivity tolerance is G_So%, 1 %, for both sensors, the only tolerance
row of the datasheet's Table 2, which states none for the accelerometer, and
the prior on the factor with mean one is the zero-mean prior on its
departure (18); "leaves the scale at its prior" reads as "at one within its
prior": at rest the data constrain only the corrected reading along gravity,
which the scale and the bias of an axis share in proportion to their priors'
variances, for the deterministic part of the readings; on a gyro axis whose
reading at rest is its bias and noise, the noise moves the factor within its
prior, since the IMU factor whitens the divided readings with a noise model
that does not depend on the scale (20); the committed fixtures whose
normalized residuals are reported are the three golden fixtures that fit,
whose goldens hold them; the rejections have no residuals, and the
initializer fixtures and `scale_recording` are model tests whose numbers
nothing holds and whose synthetic noise says nothing about the model (24,
55);
the covariance at a sample also needs
the joint covariance of each pair of adjacent fixes, which the specification
does not list, and the covariance comes from the clique marginals of one
factorization (one multifrontal QR elimination in time order with the globals
last, each clique's marginal from its parent's), the specification's "solves
of the factorized system" read as "from the factorized system": block solves
would cost nine columns per fix, and the covariance-form selected inverse was
measured to compound rounding along a long chain; a heading prior of 1000 rad
on the first fix, in that factorization only, keeps an undetermined heading
finite and moves a determined one by less than 5e-6 of itself (25, 26); the
widening window is the fixes within 2.5 s of the
sample, at least the two around it, and the factors it sums are its fixes'
position and velocity factors and the IMU factors between them, the priors
excluded, over 6N - 9 degrees of freedom (31); the heading's variance enters the
acceleration's propagation at most at the cap's, which bounds the
principal-value fallback where the horizontal acceleration is small, and
changes nothing where the heading is below the cap (35); "the deep colours of
the GNSS accuracy plots" were read as their scheme, a hue of its own for each
in the family of the value it qualifies, until the specification "Plot
colours: one colour per plot, readable on both backgrounds"
(`PLANS/done/plot-colours.md`), which has no items of its own, replaced the
deep scheme: each accuracy is now the quiet member of the value it qualifies,
in a colour that reaches 3:1 on both plot backgrounds, so item 1037 is stated
as amended (and item 1101 with it); the acceleration accuracies are in the
acceleration unit, g, at four decimals, a type of their own, since the
acceleration type's two decimals would read 0.00 over most of a recording
(37); the goldens are captured again at the
end of each phase that changes numerical results, not once (44); and the
plumbing check is the end of phase 1, where the configuration is carried and
defaulted and the noise model is still the previous one, with no switch that
reproduces the previous model (61).

The specification amends those of 9.3 (item 221: the fit has twenty-six
inputs; items 215, 216, 217, 227, 245 and 247: the per-step term and its
slopes give way to the datasheet's noise and the derived sampling term and
remainder, reported under `model.noise`), 9.9 (item 847: the fit calculation
gains the four configuration inputs; items 847 and 861: the algorithm string,
goldens and stored results are this specification's) and 9.10 (item 930: the
registration declares the configuration inputs and registers their constant
defaults; item 921: the algorithm string `v6`; item 925: the noise model the
pass reads; items 915 and 935: the bound on the published acceleration also
includes the kink of the rotated reading at a fix); each is stated "(as amended)" in its row, its appendix and the
map. The scale state amends 9.3 again (item 246: a constant temperature
reproduces the constant-bias fit with the scale state off, since the scale
moves the objective by about 4.5 % on that recording, 246.3 against 258.0)
and 9.10 (items 901, 910 and 914: the
readings the pass integrates, its step corrections and the published
acceleration are divided by the fitted scale; item 925, again: the pass
reads the scale factors with the states and the biases and changes none of
them). Searched and holding: 202, 211, 218, 220, 237, 239 and 241 (the fixtures
change under them, not the statements; `rest_throughout`, its noise at the
datasheet's level, still stops growing at 120 s), and 902, 907, 919 and 927;
with the scale state, 219 and 223 (the initializer's fits are stock, and the
scale is the full fit's), 213 and 227 (the settled test is the same; the
diagnostics gain a key), 902, 903 and 927 (every step's transition is still
the library's update, in one author, now the integration loop), 934 (at unit
scale), 917 (the texts are still quoted), 847 and 861 (still `v6`) and 239
(roll and pitch at rest are unaffected). The accuracy amends 9.3 (items 230
and 235: the runner's CSV and the result contract gain the four accuracy
channels), 9.4 (item 337: a restored fit gains them) and 9.10 (item 912: the
fit publishes the uncertainty among its channels); since the fused accuracies
(9.18) the fit has thirty-three channels and thirty-four outputs. Searched and holding with the accuracy: 917 and 937
(the account and `limitations` are still quoted and compared), 925 and 929
(the pass still runs after the last iteration and is not a boundary; the
covariance step runs there too and changes nothing of the fit). The plots
amended 9.2 (item 115: the real fusion plots became twelve), 9.6 (item 509:
five of the twelve were outputs of the fit), 9.9 (items 801, 802, 842, 854,
862 and 863: the category held twelve plots, the four accuracies named as
the GNSS accuracy plots are) and 9.10 (item 930: the plot registry gains the
four accuracy plots); the test that held the eight rows is renamed
`allFusionPlotsAreExplicitBacked`, without a count. Searched and holding with
the plots: 803, 804, 806, 808, 853 and 856 (their citations change with the
rename only), 111 (its statement names no count), and the manual steps M39
and M44, which hold as steps (M39 continues with the four accuracy plots of
M51; the measurement tree of M44 listed twelve). The specification "Fused
speed plots" (9.17) has since made the category fifteen; items 1037 and 1063
stand, and their evidence below counts fifteen rows.

The specification "The scaled IMU factor stands alone", which has no items of
its own, removes the temperature factor and the switch of the scale state:
the full fit always has the scale factors, so item 1054 is stated as amended
(its comparison holds the scale at one by a prior a thousand times tighter
than the datasheet's), and so are 219 and 246 of 9.3. Searched and holding:
1018, 1019 and 1048 (`scaleGraphShape` and `scaleFactorJacobians` keep their
names; the state-off graph gives way to the constant model's), 220, 222, 223
and 1017 (`temperatureGraphShape` and `constantTemperatureKeepsSlopeAtPrior`
keep theirs), and 901-931's held-ends reference, now built of scaled factors
with the scale held, whose agreement figures are unchanged. The specification
of 1401-1415 (9.15) restates item 1054 again: the full fit's held stage is the
fit with the scale held at one, so the comparison reads the held stage's
objective from the fit's account instead of making a second fit.

| # | Section | Clause | Evidence |
|---|---|---|---|
| 1001 | 5 | the configuration keys, with actual values and the unit in the name: in `SENSOR.CSV` `ACCEL_FS_G` (2, 4, 8, 16), `GYRO_FS_DEG_S` (250, 500, 1000, 2000), `ACCEL_ODR_HZ` and `GYRO_ODR_HZ` (12.5, 26, 52, 104, 208, 416, 833, 1666, 3333, 6666, and 1.6 for the accelerometer), `BARO_ODR_HZ`, `HUM_ODR_HZ` and `MAG_ODR_HZ`; in `TRACK.CSV` `GNSS_MODEL` by name (portable, stationary, pedestrian, automotive, sea, airborne_1g, airborne_2g, airborne_4g) and `GNSS_RATE_HZ`; recorded with their value forms in `docs/DATA_SCHEMA.md` section 2 beside `SCHEMA_VER` | `tst_sensor_configuration::keysAndValueForms`; `audit sensor-configuration` |
| 1002 | 5, 9 | the importer stores them as header attributes, as it stores `SCHEMA_VER`, and they travel with the session as `SCHEMA_VER` does: kept as recorded, merged by the attribute conflict rule, written back by the exporter | `tst_importer::configurationStoredAsRecorded`; `tst_session_merge::configurationFollowsConflictRule`; `tst_import_merge::configurationTravelsWithTheSession`; `tst_persistence_roundtrip::configurationAttributesRoundTrip` |
| 1003 | 5 | a malformed value is an import error naming the key, and nothing of the file is imported, as for a malformed `SCHEMA_VER` | `tst_importer::rejectsMalformedConfiguration`; `tst_sensor_configuration::malformedValues`, `unsupportedMessage`; `tst_persistence_roundtrip::malformedConfigurationIsNotSaved` |
| 1004 | 5 | a recording that lacks a key takes the value of the firmware of the recordings on disk, v2023.09.22: +/-16 g, +/-2000 deg/s, 12.5 Hz for both sensors; no dynamic model and no rate; the default is written once in the documentation and once in the code, with the firmware version it describes | `tst_sensor_configuration::defaultIsTheFirmwareConfiguration`; `tst_fusion_session::configurationDefaults`; `tst_importer::neverStampsConfiguration`; `audit sensor-configuration` |
| 1005 | 5 | the IMU's low-pass filters have no key: that firmware's fixed setting (the gyro's LPF2 at the cutoff of its 12.5 Hz rate and no LPF1, the accelerometer at its ODR bandwidth) is documented as part of the default, with the firmware version | `tst_sensor_configuration::keysAndValueForms`; `audit sensor-configuration` |
| 1006 | 5, as settled | the dynamic model, the GNSS rate and the barometer's, humidity sensor's and magnetometer's rates are read and stored with the other attributes; nothing uses them, and they are not inputs of the fit | `tst_importer::configurationStoredAsRecorded`; `tst_fusion_session::registrationShape`; `audit sensor-configuration` |
| 1007 | 5, 10 | the configuration reaches the kernel as part of its channels, as the origin does: the fit declares the four IMU keys as inputs, as it declares the origin, the input adapter carries them, the kernel reads no preference or constant for them, and nothing above the registration changes | `tst_fusion_session::registrationShape`, `configurationReachesTheKernel`; `tst_fusion_runner::successMatchesDirectRun`; `tst_fusion_store::dependencyEditDropsRecord`; `audit solver-confinement` |
| 1008 | 5, as settled | whether stated or defaulted, each IMU sensor's range is checked against the lattice of its readings (the gyro's sensitivity at the range, 70 mdps at +/-2000 deg/s, 1.14688 times the range over 32768; the accelerometer's range over 32768, which the printed sensitivity rounds; in effective units, within the rounding of the file's decimals): the coarsest range whose lattice all but one in a thousand of the readings fit must be the configured one (a few readings off the lattice do not decide); otherwise the fit rejects the recording with a reason that names the range stated and the range the values show, and does not guess | `tst_fusion_kernel::latticeCheckFindsTheCoarsestRange` (each range's lattice, a legacy-style truncated reading, the tolerance's edges, a few readings off the lattice among thousands, no range, the texts); `tst_fusion_golden::rejectionFixturesMatchGolden` (`reject_lattice`); `manual M47` |
| 1009 | 10, as settled | the lattice check is a kernel validation rule, one of the rules that decide whether a recording can be fitted, and reads the readings as the kernel receives them, before any correction of its own | `tst_fusion_kernel::latticeCheckFindsTheCoarsestRange` (a reading outside the fitted window counts), `configurationChecksComeAfterTheOthers` |
| 1010 | 5, as settled | a logged IMU interval that disagrees with a stated rate by more than the oscillator's tolerance (10 %) is a rejection that names both | `tst_fusion_kernel::rateCheckToleratesTenPercent` (9.9 % passes, 10.1 % rejects, the accelerometer first, a gyro-only mismatch); `tst_fusion_golden::rejectionFixturesMatchGolden` (`reject_rate`); `tst_fusion_session::configurationReachesTheKernel` (the default path of a 100 Hz recording) |
| 1011 | 6, as settled | the per-sample noise of each sensor and axis is `sqrt(density^2 x bandwidth + step^2 / 12)`: the datasheet's density at the configured range, the datasheet's bandwidth for the configured rate and filter, and the quantization step at the range (the gyro's sensitivity, 70 mdps at +/-2000 deg/s, 1.14688 times the range over 32768; the accelerometer's range over 32768, which the printed sensitivity rounds) | `tst_fusion_kernel::noiseFollowsTheTable` |
| 1012 | 6 | the documentation carries the table: the density per range for each sensor and the bandwidth per rate and filter, with the datasheet's table numbers | `audit noise-model` (`docs/SENSOR_FUSION.md` cites Table 2, Table 18, Table 65 and Figure 17) |
| 1013 | 6 | the density the integration uses is the per-sample noise times the square root of the nominal sample interval at the configured rate, so that a step of the nominal length carries one sample's variance; `accDensity` and `gyroDensity` are gone, and no density in the model is derived otherwise | `tst_fusion_kernel::noiseFollowsTheTable`; `audit noise-model` |
| 1014 | 6, as settled | the sampling term replaces the slopes: per step, the trapezoid rule's error on a piecewise-linear signal, `dt^3 / 12` times the second derivative for a step that is a whole sample interval, and for a part of one the same derivation's integral over the part, the second derivative estimated per sample interval as the larger of the changes of slope at its two ends, for velocity from the specific force and for the angle from the rate, added in quadrature per step, with no fitted coefficient | `tst_fusion_kernel::samplingTermFollowsTheDerivation`, `constantSignalHasNoSamplingTerm`; `audit noise-model` |
| 1015 | 6, as settled | the remainder of the mid-step scheme to second order, for rate and force linear across the step: its pure rotation term, second order in the step's rotation, and the same expansion's terms in the change of force and of rate across the step (coning included), derived and added the same way | `tst_fusion_kernel::rotationRemainderFollowsTheDerivation` (a constant turn: the pure term, and the library's one-step error within 1 %), `rotationRemainderMatchesTheSchemeError` (a ramp: the vector and the coning against a thousand-fold subdivided integration, within 5 %) |
| 1016 | 6 | the documentation writes the derivations out, including what they assume (a smooth signal between samples, the samples exact) | `audit noise-model` (`docs/SENSOR_FUSION.md` writes `h^3 / 12`) |
| 1017 | 6 | the bias priors stay at 0.3 m/s^2, 0.03 rad/s and 0.010 deg/s per degC, and the documentation cites the datasheet's table for each | `tst_fusion_kernel::temperatureGraphShape` (the three priors' values, as literals); `audit noise-model` |
| 1018 | 6, as settled | a scale-factor state: per fit, one factor per axis for each sensor, starting at one, with the datasheet's sensitivity tolerance (G_So%, +/-1 %, for both sensors: Table 2 states none for the accelerometer) as a zero-mean prior on its departure from one | `tst_fusion_kernel::scaleGraphShape` (the six factors, the prior last with mean ones and the tolerances), `scaleFactorJacobians`; `audit scale-state` |
| 1019 | 6 | the readings enter the integration divided by the scale, and the graph is re-preintegrated at the fitted scale as it is at the fitted bias, under the same settled test | `tst_fusion_kernel::preintegrationDividesByTheScale`, `scaleJacobianMatchesCentralDifferences`, `fitRepreintegratesAtTheFittedScale`, `reconstructionUsesTheFittedScale` |
| 1020 | 6, as settled | a recording without motion leaves the scale at one within its prior: the data then constrain only the corrected reading along gravity, which the scale and the bias of that axis share in proportion to their priors' variances; this holds for the deterministic part of the readings, and on a noise-dominated gyro axis the noise moves the factor within its prior | `tst_fusion_kernel::restLeavesTheScaleAtItsPrior` (`rest_throughout`: the accelerometer's factors within a tenth of the tolerance, the gyro's within it); `manual M49` |
| 1021 | 6 | the fitted scale factors are reported in the diagnostics beside the biases | `tst_fusion_kernel::diagnosticsReportTheScale` (`model.scale`, the `scale_prior` residual last); `tst_fusion_golden::successFixturesMatchGolden`; `manual M49` |
| 1022 | 6 | the sigma of each fitted scale factor is reported beside it, from the covariance of section 7 | `tst_fusion_kernel::diagnosticsReportTheScaleSigma` (`acc_sigma` and `gyro_sigma` against the library's marginal of S(0), within 1e-6); `tst_fusion_golden::successFixturesMatchGolden` |
| 1023 | 6 | the 1.14688 schema correction of legacy recordings is not a scale factor and is untouched | `tst_conversion_engine::legacyGyroCorrected`; `tst_fusion_runner::legacySchemaScalesTheGyro`; `audit scale-state` (the kernel never names it) |
| 1024 | 6, as settled | validation, not tuning: the normalized residuals (position, velocity, IMU) of the reference recordings of `tests/README.md` section 12.2 and of the committed fixtures that fit (the three golden successes; the rejections have no residuals) are reported in the documentation as measured under this model, with the sentence that a value far from one measures what the model does not yet describe; no constant of section 6 is changed to move them | `audit model-validation` (the sentence once and the two paragraphs in `docs/SENSOR_FUSION.md`); `tst_fusion_golden::successFixturesMatchGolden` (the fits' `quality`, four since the GNSS holes); `manual M52` |
| 1025 | 7, as settled | after convergence, from the converged graph and one factorization: the marginal covariance of every fix state, the joint covariance of each pair of adjacent fix states, and their cross-covariance with the biases and the scale factors, by the clique marginals of one factorization (one multifrontal QR elimination in time order with the globals last, each clique's marginal from its parent's), not by the library's joint marginals; a heading prior of 1000 rad on the first fix, in the factorization only, keeps an undetermined heading finite and moves a determined one by less than 5e-6 of itself | `tst_fusion_kernel::covarianceMatchesJointMarginals` (`coarse_maneuver` at every pair, `drifting_bias` at three, within 1e-6); `audit accuracy-channels` (no `jointMarginalCovariance` in `src`; one elimination, in `fitcovariance.cpp`) |
| 1026 | 7, as settled | through the reconstruction's pass, the conditional covariance of the state at every IMU sample given the two fixes around it, composed with the covariance of those fixes, the biases and the scales into the covariance of the state at the sample | `tst_fusion_kernel::sampleCovarianceMatchesTheEdgeGraph` (every tenth sample of `coarse_maneuver` against a graph with a state at every edge, within 1e-5), `sampleOnAFixHasTheFixMarginal` |
| 1027 | 7 | the covariance step runs inside the same cancellable job and is not a cancellation boundary | `tst_fusion_golden::cancelAtEachKindOfBoundary`, `progressMatchesGoldenBoundaries` (the progress texts unchanged); `audit fusion-model` (three `checkpoint(` call sites) |
| 1028 | 7 | the heading accuracy of a sample is the square root of the navigation-frame rotation covariance's element about the vertical, the tilt accuracy the square root of the sum of its two horizontal elements, both in degrees and capped at 180, which means undetermined | `tst_fusion_kernel::attitudeAccuracyFollowsTheNavigationFrame` |
| 1029 | 7 | the acceleration accuracy of a sample is the first-order propagation of the joint covariance of attitude, accelerometer bias and accelerometer scale through `a = R (f / s - b) + g`, plus the accelerometer's per-sample noise rotated; the horizontal accuracy is the standard deviation along the horizontal acceleration's direction, or the larger horizontal principal value where the horizontal acceleration is below that accuracy, and the vertical accuracy is the vertical element; both in m/s^2 | `tst_fusion_kernel::accelerationAccuracyFollowsItsPropagation`, `sampleCovarianceMatchesTheEdgeGraph` |
| 1030 | 7 | the documentation states the propagation in symbols and what it leaves out (gravity's own uncertainty, cross-axis sensitivity, the interpolation between nodes) | `audit accuracy-channels` (`docs/SENSOR_FUSION.md` writes the propagation and its omissions) |
| 1031 | 7, as settled | widening: over the fixes within 2.5 s of each sample, the sum of the squared whitened residuals of the window's factors divided by the window's degrees of freedom is a factor; the sample's four accuracies are multiplied by its square root where it exceeds one and are unchanged where it does not | `tst_fusion_kernel::wideningWindowAndFactor` (hand-built residuals, exact), `wideningGrowsWithAnUnderstatedSigma` (on `scale_recording` understated three times, the success assembly publishes at every sample `accHAcc` and `accDAcc` equal to w times the unwidened accuracies of the reconstruction and `headingAcc` and `tiltAcc` equal to min(180, w times them), bit for bit, with w > 1 at more than 300 samples; `accuracy.max_widening` and `widened_samples` are those widenings' largest and count) |
| 1032 | 7 | the documentation says what the factor is (the a-posteriori variance factor of the window), that it assumes every sigma is off by the same ratio, and that it never tightens | `audit accuracy-channels` |
| 1033 | 7 | `Fusion/headingAcc`, `Fusion/tiltAcc`, `Fusion/accHAcc` and `Fusion/accDAcc` are outputs of the fit, aligned with `Fusion/_time` | `tst_fusion_session::registrationShape` (a literal list of the outputs, thirty-four since the fused accuracies); `tst_fusion_kernel::imuRateIsWhatTheFitPublishes` (the four against the test's own composition and widening, bit for bit) |
| 1034 | 7 | they are absent for a rejected or failed fit and for a successful fit whose factorization failed, which the diagnostics say; nothing else about that fit changes | `tst_fusion_kernel::covarianceFailureLeavesTheFitAsItIs` (and the failure-shape tests: the four arrays empty); `tst_fusion_golden::rejectionFixturesMatchGolden` |
| 1035 | 7, as settled | an undetermined heading does not fail the computation: the cap applies, the heading's variance enters the acceleration's propagation at most at the cap's, and the acceleration accuracies use the horizontal magnitude's direction, which a heading error does not move | `tst_fusion_kernel::undeterminedHeadingIsCapped`, `accelerationAccuracyFollowsItsPropagation` (the heading under a horizontal force, the cap inside the propagation) |
| 1036 | 2, 7 | the documentation says in one sentence that the accuracy is one standard deviation from the covariance of the converged solution under the documented model, widened where the residuals exceed what the model allows | `audit accuracy-channels` |
| 1037 | 8, as settled, as amended | the "Sensor fusion" category gains four plots after Roll: Heading accuracy and Tilt accuracy in degrees, Horizontal acceleration accuracy and Vertical acceleration accuracy in the acceleration unit, each drawn as the quiet member of the value it qualifies, in a colour that reaches 3:1 on both plot backgrounds; the acceleration accuracies in g at four decimals, a type of their own | `tst_fusion_rows::allFusionPlotsAreExplicitBacked` (the fifteen rows as a literal: names, units, measurements, types); `tst_plot_format::accelerationAccuracyKeepsItsDigits`; `audit naming` (fifteen rows); `audit accuracy-channels` (four accuracy rows); `manual M51` (the colours, the axes, the menu) |
| 1038 | 8 | the four plots are absent, like any unavailable value, where the fit did not compute them | `tst_fusion_rows::accuracyPlotsAreAbsentWithoutAFit` (before the fit, a rejection listed once, no IMU data), `noImuSessionIsNeverCounted`; `tst_fusion_store::restoredFitWithoutAccuracyDrawsTheRest` (a stored success without the accuracy); `manual M51` |
| 1039 | 8 | a logbook column over any of the four works as over any fusion value | `tst_fusion_store::fusionColumnFillsUnloadedSessions` (a row per accuracy); `manual M51` |
| 1040 | 8 | the diagnostics show the configuration the fit ran under | `tst_fusion_kernel::diagnosticsReportTheNoiseModel`; `tst_fusion_golden::successFixturesMatchGolden`; `manual M47` |
| 1041 | 8, 9 | the first start after the change finds every stored fit stale at its recording's load and recomputes it when something switched on needs it, counted in the status bar as any computation | `tst_fusion_store::codeStampChangeDropsRecordOnLoad` (a record stamped `batch-temperature-bias-v5`); `manual M48` |
| 1042 | 9 | the fit's outputs gain the four channels, and the record stores and restores them with the rest, in the record format as it is | `tst_fusion_store::restoredAfterRestartIsBitIdentical`, `restoredAfterEvictionIsBitIdentical` (thirty-three channels since the fused accuracies); `tst_fusion_jobs::jobPublishesAllOutputsTogether` (thirty-four outputs since then) |
| 1043 | 9 | the algorithm string changes once, for the whole specification | `tst_fusion_session::registrationShape` (`batch-temperature-bias-v10` now: `v6` for the specification, `v7` for the lattice rule's fixup after it, `v8` for the GNSS holes bridged, `v9` for the scale factors released from the held solution, `v10` for the covariance blocks published); `audit stored-results` |
| 1044 | 9, as settled | the golden fixtures are captured again with the existing tool at the end of each phase that changes numerical results; their time axes are unchanged and checked byte for byte; the last capture's comparison covers the new channels, the scale factors and the configuration in the diagnostics | `tst_fusion_golden::successFixturesMatchGolden` (thirty-three columns since the fused accuracies), `rejectionFixturesMatchGolden`; `tst_fusion_kernel::fitTraceMatchesGolden`; section 11's history paragraphs (the `_time` columns byte-identical at each capture) |
| 1045 | 9 | the fixtures state their configuration explicitly, so that the stated path (the kernel's and the sessions' fixtures) and the default path (a session without the keys) are both exercised | `tst_fusion_session::configurationReachesTheKernel`, `naturalSessionEndToEnd`, `inputsAreBitIdenticalToFixture`; `tst_fusion_golden::successFixturesMatchGolden` |
| 1046 | 10 | the kernel derives its noise from the configuration in one unit, the only place the datasheet table exists in code | `tst_fusion_kernel::noiseFollowsTheTable`, `configurationWithoutEntryIsRejected`; `audit noise-model` |
| 1047 | 10 | the sampling term and the rotation remainder are computed where the steps are integrated, from the readings, with no constant of their own | `tst_fusion_kernel::samplingTermFollowsTheDerivation`; `audit fusion-model` |
| 1048 | 10 | the scale factors are variables of the graph beside the biases | `tst_fusion_kernel::scaleGraphShape`; `audit scale-state` |
| 1049 | 10 | GTSAM stays confined to the kernel, and the covariance step is a unit of its own, as the reconstruction is | `audit solver-confinement`; `audit accuracy-channels` (`src/fusion/fitcovariance.*`); `audit fusion-tooling` (its header internal) |
| 1050 | 11 | test: a file with every key imports them; a malformed one is an import error naming the key; a file without them takes the default | `tst_importer::configurationStoredAsRecorded`, `rejectsMalformedConfiguration`; `tst_fusion_session::configurationDefaults`; `tst_fusion_runner::matchesTheApplicationImportPath` |
| 1051 | 11 | test: a fixture whose readings sit on the +/-8 g lattice with a header stating +/-16 g is rejected naming both; the lattice check identifies the range of every committed fixture and of the reference recordings | `tst_fusion_golden::rejectionFixturesMatchGolden` (`reject_lattice`); `tst_fusion_kernel::latticeCheckIdentifiesEveryFixture`; `manual M47` (the reference recordings) |
| 1052 | 11 | test: the per-sample noise and the density follow the table for every configuration the fixtures state, bit for bit against the formula | `tst_fusion_kernel::noiseFollowsTheTable` (bit for bit in exact mode; within 4 ulp otherwise, as section 11 compares recomputed values) |
| 1053 | 11 | test: a step with a constant signal has no sampling term; the sampling term of a step follows the derivation on a synthetic signal with a known second derivative; the rotation remainder follows its derivation on a constant turn | `tst_fusion_kernel::constantSignalHasNoSamplingTerm`, `samplingTermFollowsTheDerivation`, `rotationRemainderFollowsTheDerivation` |
| 1054 | 11, as amended | test: a synthetic recording with the accelerometer 2 % high on one axis recovers the factor within the prior's tolerance through the full fit's released stage, and its objective falls below the held stage's, whose prior a thousand times tighter than the datasheet's holds the scale at one, read from the fit's account (amended by the specification of 1401-1415; before it, a second fit with the scale held at one and the position misfit); a recording at rest leaves the scale at one within its prior | `tst_fusion_kernel::scaleRecordingRecoversTheFactor` (`scale_recording`: `s_ax` within 0.01 of 1.02, the released stage kept, the objective strictly below the held stage's), `restLeavesTheScaleAtItsPrior` |
| 1055 | 11, as settled | test: the normalized residuals of the committed fixtures that fit are in the goldens; the reference recordings' are recorded in the documentation | `tst_fusion_golden::successFixturesMatchGolden`; `audit model-validation`; `manual M52` |
| 1056 | 11 | test: on the committed fixtures a successful fit publishes finite, positive accuracies for every sample, and scaling every GNSS accuracy up never lowers them | `tst_fusion_kernel::accuraciesFiniteAndPositive` (the fits, four since the GNSS holes, and the four recordings of the initializer), `gnssAccuracyScalingNeverLowersThem` (every GNSS accuracy doubled: no published accuracy below 1 - 1e-6 of its value); `manual M50` |
| 1057 | 11 | test: the heading accuracy of the first node agrees with the heading check | `tst_fusion_kernel::firstNodeHeadingIsTheHeadingCheck` (within 1e-5, or both at the cap, on every fit and recording; the first sample within 1e-3) |
| 1058 | 11 | test: the acceleration accuracy follows its propagation on synthetic inputs with known answers, and agrees with the library's joint marginals on a short fixture | `tst_fusion_kernel::accelerationAccuracyFollowsItsPropagation`, `sampleCovarianceMatchesTheEdgeGraph` |
| 1059 | 11 | test: the widening is one where the residuals are at the model, and grows where a factor's sigma is understated by a known ratio | `tst_fusion_kernel::wideningIsOneAtTheModel` (exactly one on every committed fixture; `scale_recording` at its noise: median factor in [0.9, 1.1], every widening at most 1.25), `wideningGrowsWithAnUnderstatedSigma` (understated three times: 2.6 to 3.4 inside, at most 1.25 farther than 2.5 s) |
| 1060 | 11 | test: the cap holds on a fixture whose heading is undetermined | `tst_fusion_kernel::undeterminedHeadingIsCapped` (`coarse_linear`: 180 at every sample, tilt and acceleration against a gauge-fixed reference within 1e-6) |
| 1061 | 11, as settled | test: on a build where the configuration is carried and defaulted and the noise model is the previous one (the end of phase 1), the existing channels and diagnostics of every fixture are bit-identical to the goldens | `tst_fusion_golden::successFixturesMatchGolden`, `rejectionFixturesMatchGolden`; `tst_fusion_kernel::fitTraceMatchesGolden`; `tst_fusion_session::configurationReachesTheKernel`; `tst_fusion_runner::successMatchesDirectRun` (at the end of phase 1; the goldens have been captured again since) |
| 1062 | 11 | test: a stored fit from before the change is stale | `tst_fusion_store::codeStampChangeDropsRecordOnLoad` |
| 1063 | 11 | test: the four plots are explicit-backed like the other fusion plots, and a column over one works | `tst_fusion_rows::allFusionPlotsAreExplicitBacked`; `tst_fusion_store::fusionColumnFillsUnloadedSessions` (a row per accuracy); `tst_fusion_jobs::noImuSessionCannotHaveAJob` (fifteen plots, all `NotApplicable`) |
| 1064 | 11 | test: the audit's confinement rules hold, and the documents carry the table, the derivations and the validation | `audit solver-confinement`; `audit fusion-tooling`; `audit noise-model`; `audit scale-state`; `audit accuracy-channels`; `audit model-validation` |
| 1065 | 12 | `docs/` describe the change: `DATA_SCHEMA.md` (section 2, the keys; section 11, the algorithm string; section 12, the record), `SENSOR_FUSION.md` (section 3, the configuration inputs and the lattice check; section 4, the noise model with its table and derivations, the scale state, the covariance and its propagation, the widening, the outputs and diagnostics; section 8, the validation results and what is and is not validated), `CALCULATIONS.md` section 17, `COMPUTED_PLOTS.md` (the four plots); `tests/README.md` section 11 records the goldens captured again and why; the map gives the specification its range, with the amended items restated | `audit sensor-configuration`; `audit noise-model`; `audit scale-state`; `audit accuracy-channels`; `audit model-validation`; `audit naming`; section 11's history paragraphs |

### 9.12 GNSS acceleration accuracy (item 1101)

The specification "GNSS acceleration accuracy", stated in
[appendix L](#appendix-l-the-acceptance-item-of-the-gnss-acceleration-accuracy-1101),
has one acceptance item, 1101; the same four line forms as 9.2. "Section" is
the section of the specification; its section 1 (motivation) has no item.

| Item | Section | Statement | Evidence |
| --- | --- | --- | --- |
| 1101 | 2, 3, 4, as amended | one on-demand calculation, `GNSS/accAcc` in m/s^2 on `GNSS/_time`, from `GNSS/sAcc` and `GNSS/_time` only: `sqrt(sAcc[i+1]^2 + sAcc[i-1]^2) / (t[i+1] - t[i-1])` inside and the one-interval forms of `computeDerivative` at the ends, no correction factor and nothing from the fusion, unavailable where the derivative is; one plot, "Acceleration accuracy" in "GNSS (Advanced)", type `acceleration`, the quiet member of the category's accelerations, in a colour that reaches 3:1 on both plot backgrounds; the documentation states the assumption, that it is conservative, and the measurement on the reference recording, without applying it | `tst_builtins_engine::accelerationAccuracyKnownAnswers` (bit for bit, the ends included), `accelerationAccuracyUnavailable` (no `sAcc`, no time, one sample, two lengths), `inventory`; `tst_builtins_golden::sessionDataMatchesGolden` (`GNSS/accAcc` on the descent fixture); `tst_plot_format::gnssAccelerationAccuracyIsAnAcceleration`; `audit gnss-acceleration-accuracy` (one stencil, the name in the registration and the row, the row's type and literal colour, the document's sentences); `manual M53` (the plot in the window) |

### 9.13 Sample continuity (items 1201-1213)

The specification "Sample continuity", stated in
[appendix M](#appendix-m-the-acceptance-items-of-sample-continuity-1201-1213),
has thirteen acceptance items, 1201-1213; the same four line forms as 9.2.
"Section" is the section of the specification; its section 1 (motivation)
has no item. No existing item is restated: none states the IMU gap factor
(item 211 names the rule without a number).

| Item | Section | Statement | Evidence |
| --- | --- | --- | --- |
| 1201 | 3 | a sensor's time axis is its `_time`; its nominal interval the median of the successive differences over the whole session; a hole an interval strictly greater than 1.5 nominal intervals; fewer than three samples have no nominal interval and no holes; no sample is removed, inserted or altered | `tst_sample_continuity::nominalIntervalIsTheMedian`, `holeIsStrictlyAboveOneAndAHalf`, `shortAxisHasNoHoles`, `malformedAxisHasNoHoles`, `runsPartitionTheSamples`; `audit sample-continuity` (the factor and the defining phrase in the authority alone) |
| 1202 | 4 | a series' line stops at the last sample before a hole of its sensor and resumes at the first after it, for every series of the sensor, whichever independent variable the plot uses | `tst_sample_continuity::graphBreaksAtGnssHole` (one break between the samples around the hole, against `_time` and `_system_time`); `audit sample-continuity` (the plot builds its graphs with `graphData()`); `manual M54` |
| 1203 | 2, 4 | each sensor has its own holes: a series on the IMU's axis draws through a GNSS hole | `tst_sample_continuity::imuGraphHasNoBreak`, `graphBreaksAtGnssHole` |
| 1204 | 4 | the legend and the crosshair show "--" inside a hole, the sample's value at the samples around it | `tst_sample_continuity::sessionReadInsideHole`, `crosshairReadInsideHole`; `audit sample-continuity` (the plot reads its graphs with `interpolateGraphAt()`); `manual M54` |
| 1205 | 4 | a measure-tool end inside a hole reads "--" and the measurement that needs it is not shown; ends on samples are compared across a hole | `tst_sample_continuity::measureEndReadInsideHole`; `manual M54` |
| 1206 | 4 | the map draws one polyline per run of connected fixes, judged on `GNSS/_time`; the cursor dot is absent inside a GNSS hole; the simplified track keeps both fixes around a hole | `tst_map_models::holeBreaksTrackAndDot`, `rangeFilterOnRecoveredTrack` (one run without a hole); `tst_simplified_track::eachRunSimplifiedOnItsOwn`; `manual M54` (the page) |
| 1207 | 4 | a logbook measurement at a marker is unavailable when the marker falls inside a hole of the sensor it reads | `tst_builtins_engine::interpolationInsideHole`, `interpolationInstances`; `tst_logbook_index::markerWrittenOnFlush` (the marker is 3) |
| 1208 | 4 | the Set Ground tool's elevation and the automatic ground elevation have no value inside a hole, so the click sets nothing and the attribute is unavailable | `tst_sample_continuity::groundElevationInsideHole`; `tst_builtins_engine::groundElevationInsideHole`; `audit sample-continuity` (the tool reads through `groundElevationAt()`); `manual M54` |
| 1209 | 4 | the derivative and its accuracy are unavailable exactly at the samples whose stencil spans a hole, the ends included, and bit for bit what they were elsewhere | `tst_builtins_engine::accelerationAcrossHole`, `accelerationAccuracyKnownAnswers` (an uneven step that is not a hole); `tst_builtins_golden::sessionDataMatchesGolden` (unchanged) |
| 1210 | 4 | the crossing times keep their linear interpolation across a hole, and the documentation names them as the exception | `tst_builtins_engine::altitudeCrossingAcrossHole` (a marker crossed in the hole and beside it, at the interpolated time), `exitCrossingBesideHole` (an exit crossed in the hole and beside it, finite, at the interpolated time less the speed over the crossing interval's slope; one under the gate passed over), `wspPartial` (unchanged); `tst_builtins_golden::sessionDataMatchesGolden`; `audit sample-continuity` (the section that names them and its sentence on the crossing inside a hole) |
| 1211 | 5 | one authority in `flysight_model`; the kernel reads its IMU gap rule from it and its own constant, median and statistics unit are gone; the readers consult it; nothing is stored | `tst_fusion_kernel::propagationRefusesIntervalAboveItsThreshold`, `imuGapRuleIsTheContinuityThreshold`, `validationRejectsEachDefect` (a recording of two IMU samples rejected, as the whole IMU axis needs a nominal interval); `audit sample-continuity`; `audit fusion-tooling` (the internal headers) |
| 1212 | 6 | the IMU gap fixture rejects with the same reason, and the goldens are unchanged bit for bit, the exact runs included | `tst_fusion_golden::rejectionFixturesMatchGolden`, `successFixturesMatchGolden` (and their `_exact` runs); `tst_fusion_kernel::segmentsAreCutOnFixes` |
| 1213 | 7 | the documents: `COMPUTED_PLOTS.md` section 10, `CALCULATIONS.md` sections 17 and 18 and the derivative helper's contract, `SENSOR_FUSION.md` section 6, `LOCAL_COORDINATES.md` section 9; this file's appendix M, matrix 9.13 and M54 | `audit sample-continuity` (the documents' sentences) |

### 9.14 GNSS holes bridged by the IMU (items 1301-1313)

The specification "GNSS holes bridged by the IMU", stated in
[appendix N](#appendix-n-the-acceptance-items-of-gnss-holes-bridged-by-the-imu-1301-1313),
has thirteen acceptance items, 1301-1313; the same four line forms as 9.2.
"Section" is the section of the specification; its section 1 (motivation)
has no item. Item 244 (the slow tail) is restated as amended (section 9.3,
appendix C); item 211's IMU gap rule and the continuity items 1211 and 1212
stand as they are, and the rejection the specification removes was nobody's
item. Item 1313 is stated as amended by the specification "The scale factors
as a refinement" (9.15): the slow-tail rule is unchanged, and the reference
recording it expected to end `iteration limit` now converges. Item 1302 is
stated as amended by the specification "Fused position and speed accuracy"
(9.18): the growth through a hole is the step chain's covariance `P_j`, now
that the composed sample covariance is published.

| Item | Section | Statement | Evidence |
| --- | --- | --- | --- |
| 1301 | 2, 3 | connectedness is the IMU's: the IMU gap rule of the continuity authority is the one disconnection; the rule on the spacing of the fixes and its two constants are deleted, and below the cap `planFit()` checks no spacing; the cap is measured, one kernel constant, the longest interval between fixes the fit bridges, 30 s, hole or not, checked on every interval of the fitted window, a longer one rejected with a reason naming the interval, to two decimals, and the limit | `tst_fusion_kernel::longHoleConverges` (a 30 s hole fitted under the production tuning), `holeAboveTheCapIsRejected` (60 s and 31 s rejected, `GNSS fixes 60.00 s apart; fusion bridges at most 30 s`; 30 s passes the plan; fixes uniformly 31 s apart, no hole by the continuity rule, rejected, and 30 s apart passed), `validationRejectsEachDefect`, `imuGapRuleIsTheContinuityThreshold` (the IMU gap rule unchanged: 1.55 nominal intervals rejected, 1.45 not); `tst_fusion_golden::successFixturesMatchGolden` (`bridged_hole`, a 2.6 s hole, fitted), `rejectionFixturesMatchGolden` (`reject_imu_gap` with its reason); `audit gnss-holes` |
| 1302 | 2, 3, as amended | every IMU sample inside a hole is published with its state and its accuracies; the position and velocity of the step chain's covariance (`P_j`, read through the reconstruction's per-interval seam) grow through the hole and collapse at the next fix, and the four published accuracies, bounded by global terms, are never below their values at the fixes around it (measured false on `bridged_hole` and not asserted; the numbers are in `docs/SENSOR_FUSION.md` section 8); the fit, the reconstruction, the covariance step and the widening are unchanged in kind | `tst_fusion_kernel::bridgedHoleFollowsTheTruth` (the sample covariance's growth and collapse through `reconstructInterval()`; the accuracies logged), `imuRateIsWhatTheFitPublishes`, `imuRateEndsAreTheFit` (the hole's interval one IMU factor whose end covariance is the factor's), `accuraciesFiniteAndPositive`, `wideningIsOneAtTheModel` (on `bridged_hole` too) |
| 1303 | 2 | the fixes around a hole are ordinary fixes, with their stated sigmas, weighted as every fix | `tst_fusion_kernel::fitRepreintegratesAtTheFittedScale` (one scaled IMU factor per interval, the hole's included), `gnssAccuracyScalingNeverLowersThem` (on `bridged_hole` too), `longHoleConverges` (the residuals at the fix after the hole ordinary entries, logged) |
| 1304 | 3, 4 | the cutter merges any piece with fewer than three fixes into the piece before it, the first into the one after it; a piece that is the whole window is left alone; a stretch without a fix yields no piece; a window whose cut yields a middle piece of two fixes is fitted, the piece merged into its predecessor; a window whose first piece has two fixes merges it into its successor | `tst_fusion_kernel::sparsePiecesAreMerged` (hand-built axes; `long_hole` under 29.57 s segments, three segments, the first ending at the fix before the hole, converged), `segmentsAreCutOnFixes` (unchanged) |
| 1305 | 3 | `input.gnss_holes`: one `{start_s, length_s}` per hole of the fitted window's GNSS axis, judged by the continuity authority on that axis, in seconds since the epoch, in time order, empty without one; a success key only | `tst_fusion_kernel::gnssHolesInTheAudit`; `tst_fusion_golden::rejectionFixturesMatchGolden` (a rejection's key set unchanged); `audit gnss-holes` (one writer, one walk); `audit sample-continuity` (the walk's file includes the authority) |
| 1306 | 2, 3 | the algorithm string is `batch-temperature-bias-v8`, and every stored result is dropped once | `tst_fusion_session::registrationShape`, `restoredFitIsIndistinguishable`; `tst_fusion_store::codeStampChangeDropsRecordOnLoad`, `restoredSolverFailureShowsBadge`; `tst_fusion_jobs::columnOnFusionOutputIsCachedFromRecord`, `workerRefillsColumnFromStoredFit`; `tst_fusion_kernel::biasSettledByCostTest`, `failureDiagnosticsShape`; `audit stored-results`; `audit gnss-holes` |
| 1307 | 4 | the bridged fixture: the former GNSS gap rejection's recording is the success fixture `bridged_hole`, its golden captured, the golden suite four fits and ten rejections; inside the hole the published attitude and accelerations within three of their accuracies of the generating trajectory, the four published accuracies never below their values at the samples nearest the two fixes around it (measured false on `bridged_hole` and not asserted; the numbers are in `docs/SENSOR_FUSION.md` section 8), the position and velocity of the sample covariance growing through the hole and collapsing at the fix after it | `tst_fusion_golden::fixturesAreDeterministic`, `successFixturesMatchGolden`, `channelsWriterIsTheInverseOfTheLoader`; `tst_fusion_kernel::fitTraceMatchesGolden`, `bridgedHoleFollowsTheTruth` (the truth within three accuracies and the sample covariance's growth); `audit gnss-holes` |
| 1308 | 4 | the long hole at the cap and above it: a 30 s hole with the IMU continuous and a manoeuvre on both sides converges under the production tuning, its iterations per pass, the largest accuracy inside the hole and the residuals at the fix after it logged; the same recording with a 60 s hole is rejected naming the length and the limit; the measurement behind the cap is in the documentation, not re-run | `tst_fusion_kernel::longHoleConverges`, `holeAboveTheCapIsRejected`, `initializerFixturesAreDeterministic` (`long_hole` deterministic, stating 104 Hz) |
| 1309 | 4 | `gnss_holes` in every fit's diagnostics, empty for the three unbroken fixtures and one entry of 2.6 s for `bridged_hole`; no outage constant or check in `src`, `tests` or `docs`; the cap one constant, named by the rejection's reason and the documentation | `tst_fusion_kernel::gnssHolesInTheAudit`; `tst_fusion_golden::successFixturesMatchGolden`; `audit gnss-holes` |
| 1310 | 4 | the reference recordings `24-09-07/08-35-48` and `24-09-04/13-35-10` through the runner: outcome, rule, iterations per pass, objective, quality, `gnss_holes`, the largest heading and tilt accuracy inside the longest hole against the median, as measured | `audit gnss-holes` (the two recordings' rows in `docs/SENSOR_FUSION.md` section 8); `manual M55` |
| 1311 | 4 | a new hundred in the acceptance map; `audit_cleanup` and the whole suite green, the exact tests included | `tst_fusion_golden::successFixturesMatchGolden` (and its `_exact` run); `audit gnss-holes` |
| 1312 | 5 | the documents: `SENSOR_FUSION.md` sections 2, 4 to 8 (section 6 with the cap, the measurement behind it and the slow tail's three bounds); `COMPUTED_PLOTS.md` section 10; `DATA_SCHEMA.md` section 12; `CALCULATIONS.md` section 17; this file's fixture tables, M55, appendix N and matrix 9.14 | `audit gnss-holes` (the documents' sentences) |
| 1313 | 2, 3, as amended | the slow tail is accepted only when every factor kind fits: the mean relative decrease and the position, velocity and IMU normalized RMS all below their bounds (`slowTailMaxNrms`, name and value unchanged); otherwise `iteration limit`, a solver failure; under it, with `v8`, `24-09-04/13-35-10` ended `iteration limit`; as amended by the specification of 1401-1415 the rule is unchanged and the recording converges, its scale factors held until the fit is stable and then released | `tst_fusion_kernel::slowTailAtTheIterationLimit` (the row `imu nrms bound fails`: position and velocity normalized RMS 0.013 and 0.0005, the IMU's 2.5, refused); `audit gnss-holes` (three bounds compared in `factorgraphfit.cpp`); `manual M55` (the `v8` outcome); `manual M56` (the current outcome) |

### 9.15 The scale factors as a refinement (items 1401-1415)

The specification "The scale factors as a refinement: held until the fit is
stable, then released", stated in
[appendix O](#appendix-o-the-acceptance-items-of-the-scale-factors-as-a-refinement-1401-1415),
has fifteen acceptance items, 1401-1415; the same four line forms as 9.2.
"Section" is the section of the specification; its section 1 (motivation)
has no item. Items 243 (section 9.3, appendix C), 1054 (section 9.11,
appendix K) and 1313 (section 9.14, appendix N) are restated as amended;
items 244, 246 and 1306 stand as they are.

| Item | Section | Statement | Evidence |
| --- | --- | --- | --- |
| 1401 | 2, 3 | the full fit runs in two stages under the temperature model: the held stage from the initializer's start with the scale prior's sigma the sensitivity tolerance divided by a thousand, then the released stage from the held stage's values with the tolerance itself; one graph, the prior's sigma the only difference; the held stage with `maxPasses`, the released stage with `releasePasses` (three, a call made at implementation: the specification's two discarded releases that had reached their minimum), each pass of `maxIterations`; the initializer's fits untouched | `tst_fusion_kernel::scaleReleaseIsAccountedFor`, `slowTailAtTheIterationLimit` (the released stage's budget: 3 passes, 75 iterations), `constantTemperatureKeepsSlopeAtPrior` (the held stage's objective reproduces the constant-bias fit), `releaseFailureFallsBackToTheHeldFit` (the held prior: the factors' sigmas below a hundredth of the tolerance), `fitTraceMatchesGolden` (the initializer's account and its prefix iterations unchanged by the capture); `tst_fusion_golden::successFixturesMatchGolden`; `audit staged-scale` |
| 1402 | 2, 3 | the release trigger is convergence: the released stage runs only after a held stage that ended `settled` or `slow tail accepted`; any other end of the held stage ends the fit as before, under the same rule, reason and diagnostics shape, and no failure carries `scale_release` | `tst_fusion_kernel::nonConvergenceIsSolverFailure`, `biasNeverSettlesIsSolverFailure` (completed-pass shape), `dampingSaturationIsASolverFailure` (a thrown failure's shape), `divergenceEndsTheHeldStage` |
| 1403 | 2, 3 | the fallback: a released stage that ends `settled` or `slow tail accepted` is the fit; one that ends under any other rule, diverges or throws is discarded: the held stage's values, graph, objective, residuals, quality and stopping are the fit, `converged` true, the factors at one with the held stage's sigmas, and the diagnostics record the outcome; a refinement never turns a converged fit into a failure; a cancellation is not caught | `tst_fusion_kernel::releaseFailureFallsBackToTheHeldFit` (a `cost increased` thrown at the release boundary), `releaseExceptionFallsBackToTheHeldFit` (a `std::runtime_error` thrown there: the fallback too, its text the reason), `divergenceEndsTheReleasedStage`; `tst_fusion_golden::cancelAtEachKindOfBoundary` (a cancellation at the release boundary or in the released stage cancels the fit) |
| 1404 | 2, 3 | divergence between passes: after every pass's rebuild of either stage, before the cost test, the IMU normalized RMS strictly below `divergenceMaxImuNrms` (10) and every factor strictly inside `divergenceScaleRange` (0.5 to 2), else the stage ends `diverged`: a solver failure of the completed-pass shape in the held stage, the fallback in the released; a zero bound or an empty interval refuses; the bounds are reported in `stopping` as `divergence_max_imu_nrms` and `divergence_scale_range`; the production bounds change nothing on any fixture | `tst_fusion_kernel::divergenceEndsTheHeldStage` (rows `imu nrms bound zero`, `scale range empty`), `divergenceEndsTheReleasedStage` (the range 0.5 to 1.005 on `scale_recording`); `tst_fusion_golden::successFixturesMatchGolden` (every fit's release kept); `audit staged-scale` |
| 1405 | 3 | passes are numbered across both stages in the trace and the progress texts; the boundary `Releasing the scale factors` is reported once, before the released stage's first graph build, a fourth kind of cancellation boundary; each stage's `stopping.passes` counts its own passes | `tst_fusion_kernel::scaleReleaseIsAccountedFor`; `tst_fusion_golden::cancelAtEachKindOfBoundary` (rows `release boundary` and `released stage iteration`), `progressMatchesGoldenBoundaries`; `audit staged-scale`; `audit fusion-model` (four `checkpoint(` call sites) |
| 1406 | 3 | the diagnostics: `scale_release` with `held`, `released` (or `null`), `kept` and `reason`, a success key written once; `stopping` and `quality` describe the reported fit; `model.scale` the reported fit's factors; `seeds[0].iterations` counts both stages; the fit's result carries the account (`FitResult::scaleRelease`) and nothing recomputes it | `tst_fusion_kernel::scaleReleaseIsAccountedFor`, `initializerDiagnosticsShape` (the key set), `biasSettledByCostTest`, `slowTailAtTheIterationLimit` (the accepted row: 200 iterations, held 125 in five passes, released 75 in three); `tst_fusion_golden::successFixturesMatchGolden`; `audit staged-scale` |
| 1407 | 2, 3 | the algorithm string is `batch-temperature-bias-v9`, and every stored result is dropped once; `v8` remains only as history | `tst_fusion_session::registrationShape`, `restoredFitIsIndistinguishable`; `tst_fusion_store::codeStampChangeDropsRecordOnLoad`, `restoredSolverFailureShowsBadge`; `tst_fusion_jobs::columnOnFusionOutputIsCachedFromRecord`, `workerRefillsColumnFromStoredFit`; `tst_fusion_kernel::biasSettledByCostTest`, `failureDiagnosticsShape`; `audit stored-results`; `audit gnss-holes`; `audit staged-scale` |
| 1408 | 4 | the recovery holds: `scale_recording` recovers its 2 % factor through the released stage, which is kept, and its objective falls against the held stage's, read from the fit's account | `tst_fusion_kernel::scaleRecordingRecoversTheFactor` |
| 1409 | 4 | the resting recording leaves every factor at its prior through both stages, and its released stage settles | `tst_fusion_kernel::restLeavesTheScaleAtItsPrior` |
| 1410 | 4 | the fallback forced: a checkpoint that throws at the release boundary leaves a converged fit with the held stage's values and factors at one, `kept` false with the thrown rule as the reason, `released` `null`, and the published channels those of the held fit | `tst_fusion_kernel::releaseFailureFallsBackToTheHeldFit` |
| 1411 | 4 | divergence forced: the IMU bound at zero and an empty scale range each end the held stage `diverged` as a solver failure; the scale range narrowed to exclude the recovered factor ends `scale_recording`'s released stage `diverged`, the fallback; the IMU bound is not forced in the released stage alone (no stage-specific bound exists) | `tst_fusion_kernel::divergenceEndsTheHeldStage`, `divergenceEndsTheReleasedStage` |
| 1412 | 4 | the reference recordings: M56 runs the four of M49 and `24-09-04/13-35-10`, reading `scale_release` from each diagnostics; `13-35-10` converges, the four at or near their M49 numbers; the numbers go in the documentation as measured | `audit staged-scale` (`docs/SENSOR_FUSION.md` attributes its numbers to M56); `manual M56` |
| 1413 | 4 | the goldens captured again under `v9`: four fits, ten rejections; the capture note records what moved and why | `tst_fusion_golden::fixturesAreDeterministic`, `successFixturesMatchGolden`; `tst_fusion_kernel::fitTraceMatchesGolden`; `audit gnss-holes` (fourteen goldens with the current string) |
| 1414 | 4 | a new hundred in the acceptance map; `audit_cleanup` and the whole suite green, the exact tests included | `tst_fusion_golden::successFixturesMatchGolden` (and its `_exact` run); `audit staged-scale` |
| 1415 | 5 | the documents: `SENSOR_FUSION.md` section 4 (the two stages, the trigger, the fallback, the divergence rule and its bounds, the case; the stopping rules with `diverged`; the diagnostics with `scale_release`), section 7 (the fourth boundary, `v9` in the history), section 8 (M56's numbers); `DATA_SCHEMA.md` section 12 and `CALCULATIONS.md` section 17 (`v9`); this file's kernel and golden rows, M56 in 12.13, appendix O and this matrix | `audit staged-scale` (the documents' sentences) |

### 9.16 Background computation per recording (items 1501-1524)

The specification "Background computation per recording: the Compute
attribute", stated in
[appendix P](#appendix-p-the-acceptance-items-of-background-computation-per-recording-1501-1524),
has twenty-four acceptance items, 1501-1524; the same four line forms as 9.2.
"Section" is the section of the specification; its section 1 (motivation)
has no item, and items 1513-1523 are the eleven bullets of its section 6 in
order. Items 519 (section 9.6, appendix F), 629, 633 and 662 (section 9.7,
appendix G) are restated as amended; items 534 and 703 stand as they are: the
user has no cancel control (the switch is a change of demand), and an
excluded track is neither waiting nor running.

| Item | Section | Statement | Evidence |
| --- | --- | --- | --- |
| 1501 | 2, 3 | the Compute attribute: a session attribute `_COMPUTE`, a Choice of on ("On") and off ("Off") in that order, category "Session", display name "Compute", editable, a built-in of the core registered beside the other built-in definitions; its tokens are spelled beside the key and its labels in the definition; read by the demand layer alone, and named in src only by the key's home, the demand layer, the importer, the index, the definition, the constant default and the preference's registration and page | `tst_calculation_demand::computeColumnShowsTheDefaultWithoutAWrite`; `tst_builtins_engine::constantDefaults`; `audit background-computation` |
| 1502 | 3 | a session without the line reads the constant default on: the column shows "On", the file is not touched and index.json caches on for its stub; `setData()` writes on or off verbatim and refuses any other value; no edit removes the line; a token outside the list is shown as written and the session is computed | `tst_calculation_demand::computeColumnShowsTheDefaultWithoutAWrite`, `computeEditStoresATokenAndRefusesOthers`, `handEditedTokenIsShownAsWrittenAndReadAsOn`; `tst_builtins_engine::constantDefaults` |
| 1503 | 2, 3 | not an input: setting the attribute while a job of the session runs leaves it not asked to stop, keeps the stored records, the cached values and every other session's results, starts no settle wait and runs one pass of its own; the compatibility marker and the algorithm string are unchanged | `tst_calculation_demand::settingComputeInvalidatesNothing`; `tst_fusion_golden::successFixturesMatchGolden`; `tst_result_records::stampsAreCurrent` |
| 1504 | 3 | the column: the header's tooltip "compute results for this recording in the background" (no other header has one); "Set Compute..." offers the definition's list and applies a bulk edit, on a stub without loading it | `tst_calculation_demand::computeColumnShowsTheDefaultWithoutAWrite`, `computeBulkEdit`; `tst_choice_attribute::setDialogOffersTheList` |
| 1505 | 3 | the import preference "Compute newly imported recordings in the background", on by default and holding the token: a recording imported while it reads exactly off is stored with the off line, one with its own line keeps it, and any other value stores nothing; the page round-trips the setting | `tst_importer::computeFromThePreference`; `tst_calculation_demand::importedWhilePreferenceOffIsExcludedAtOnce`; `manual M57` |
| 1506 | 4 | a track of a session that reads off is excluded, decided first, for loaded and unloaded sessions, plot and column tracks: no job, no load, no hold, not counted in progress, not a failure, its cell excluded and not pending; the other sessions compute | `tst_calculation_demand::excludedSessionIsNotComputed`, `fillNeverLoadsAnExcludedSession` |
| 1507 | 2, 4 | what exists is shown: a stored result of a session switched off is restored when it is loaded and fills its column from the record without a load; its stored rejection is not a failure and shows no row warning, and is listed again, without computing, when the session is switched on | `tst_calculation_demand::excludedSessionWithStoredResultServes`; `tst_logbook_indicators::excludedSessionShowsNoRowWarning`, `excludedCellShowsItsValue` |
| 1508 | 4 | the excluded cell: a requested column's cell of a session switched off that has no value reads "excluded", muted like the pending mark, with the tooltip "Not computed: background computation is switched off for this recording"; a value always wins; it is a presentation of demand, never in the model, the cached values or the index, sorting treats it as unavailable, and `pendingCellsChanged(id)` announces and repaints its column | `tst_logbook_indicators::excludedCellIsDistinctFromPendingAndUnavailable`, `excludedCellShowsItsValue`, `sortingTreatsExcludedAsUnavailable`, `excludedCellsChangeRepaintsOnlyThatColumn`; `audit demand`; `audit background-computation` |
| 1509 | 4 | switching off: a pass runs at once; the session's chosen next job is withdrawn ("No longer needed"); its running job ends Cancelled with the reason "Switched off for this recording", publishing and storing nothing, and the next job starts after it; the executor's cancel takes the caller's reason; the pair memory is not touched | `tst_calculation_demand::switchingOffCancelsTheRunningJob`, `switchingOffWithdrawsTheChosenNextJob`, `switchingOffStopsAJobKeptAfterUnchecking`; `tst_jobqueue::cancelRunningThenNextStarts`, `cancelQueued`; `audit gestures`; `audit background-computation` |
| 1510 | 4 | switching on: the session's missing results enter demand under the ordinary priority, a stopped computation runs again from its start, and a failure remembered for the pair still keeps it from being offered | `tst_calculation_demand::switchingOnCreatesDemandForItsMissingResults` |
| 1511 | 4 | the plot widget's warning, priority, the settle wait, the fill's bound, the executor's limits and the status bar's form are unchanged; the counts and lists simply omit an excluded session, and the running job of one is described by nothing | `tst_calculation_demand::excludedSessionIsNotComputed`, `switchingOffCancelsTheRunningJob`, `progressCountsEachSessionOnce`; `tst_status_bar::computationsAreOneItem` |
| 1512 | 5 | sessions that are not loaded: the index records, per session, `"computeOff": true` when the file was last seen switched off, learned at a save and a load (the import, a loaded row's save, the bulk edit's stub path, the column worker's copy), carried by a remap, dropped by a removal and a reset; an index without it reads on; an off file the index does not know is loaded at most once and never computed | `tst_logbook_index::computeOffRoundTrip`; `tst_calculation_demand::fillNeverLoadsAnExcludedSession`, `offOnDiskButAbsentFromIndexIsLoadedOnce`; `audit background-computation` |
| 1513 | 6, first bullet | a session switched off is not computed for a plot checked while it is visible, when shown while a plot is checked, or for an enabled column; the others are; progress never counts it; its cell reads excluded and is not pending | `tst_calculation_demand::excludedSessionIsNotComputed` |
| 1514 | 6, second bullet | switching off while the job runs ends it Cancelled with the reason, publishes and stores nothing, releases the hold and runs the next candidate; while it is the chosen next job, it is withdrawn; while the session settles, nothing is left waiting | `tst_calculation_demand::switchingOffCancelsTheRunningJob`, `switchingOffWithdrawsTheChosenNextJob`, `switchingOffStopsAJobKeptAfterUnchecking`, `switchingOffWhileSettlingLeavesNothingWaiting` |
| 1515 | 6, third bullet | switching on creates demand for the session's missing results and no other; a stopped computation runs again from its start | `tst_calculation_demand::switchingOnCreatesDemandForItsMissingResults` |
| 1516 | 6, fourth bullet | a session switched off with a stored result restores and draws it, fills the column from it and is not loaded again for it; its stored rejection is not a failure and shows no row warning, and is listed again when it is switched on | `tst_calculation_demand::excludedSessionWithStoredResultServes`; `tst_logbook_indicators::excludedSessionShowsNoRowWarning` |
| 1517 | 6, fifth bullet | setting the attribute invalidates no stored result and no cached value, starts no settle wait and makes no running job stale; the marker and the algorithm string are unchanged | `tst_calculation_demand::settingComputeInvalidatesNothing`; `tst_fusion_golden::successFixturesMatchGolden`; `tst_result_records::stampsAreCurrent` |
| 1518 | 6, sixth bullet | the column fill never loads a session switched off; with every unloaded session off, enabling the column creates no load and no job; the index records the attribute on the bulk edit's stub path and reports it without a load | `tst_calculation_demand::fillNeverLoadsAnExcludedSession`; `tst_logbook_index::computeOffRoundTrip` |
| 1519 | 6, seventh bullet | the column: the default shows "On" without a write; an edit stores a token and refuses others; "Set Compute..." bulk-edits the selected sessions; a hand-edited token is shown as written and read as on | `tst_calculation_demand::computeColumnShowsTheDefaultWithoutAWrite`, `computeEditStoresATokenAndRefusesOthers`, `computeBulkEdit`, `handEditedTokenIsShownAsWrittenAndReadAsOn`; `tst_choice_attribute::setDialogOffersTheList` |
| 1520 | 6, eighth bullet | a recording imported while the preference is off carries the off line and is excluded at once for an enabled column and a checked plot; one imported while it is on carries no line; the page round-trips the setting | `tst_calculation_demand::importedWhilePreferenceOffIsExcludedAtOnce`; `tst_importer::computeFromThePreference`; `manual M57` |
| 1521 | 6, ninth bullet | a session whose attribute is off on disk but absent from the index is loaded at most once by the fill and never computed | `tst_calculation_demand::offOnDiskButAbsentFromIndexIsLoadedOnce` |
| 1522 | 6, tenth bullet | the manual step M57: a long-running recording switched off while it is computed (the status bar's count, the cell, the row, the plot), switched on again, the whole-logbook column fill with half the recordings off, the import preference | `audit background-computation`; `manual M57` |
| 1523 | 6, eleventh bullet | the audit's gestures rule reads that the demand layer is the one product caller of cancel; a new hundred in the acceptance map; `audit_cleanup` and the whole suite green | `audit gestures`; `audit background-computation` |
| 1524 | 7 | the documents: COMPUTED_PLOTS.md (switching a recording off; the status bar, logbook and limitation sections), CALCULATIONS.md sections 15 and 16 (the condition, the reading, the stop of the running job, the fill, the cell's third text), DATA_SCHEMA.md sections 9 and 11, SENSOR_FUSION.md section 2, tests/README.md (the rows, M57, appendix P, matrix 9.16) | `audit background-computation` |

### 9.17 Fused speed plots (items 1601-1612)

The specification "Fused speed plots", stated in
[appendix Q](#appendix-q-the-acceptance-items-of-the-fused-speed-plots-1601-1612),
has twelve acceptance items, 1601-1612; the same four line forms as 9.2.
"Section" is the section of the specification; its section 1 (motivation) has
no item, items 1601-1604 are its section 2, 1605 its section 3, 1606 its
section 4, and 1607-1612 the six bullets of its section 5 in order. Items 115
(section 9.2, appendix B), 509 (section 9.6, appendix F), 801, 802, 842, 854,
862 and 863 (section 9.9, appendix I) count the fusion plots and are restated
as amended: fifteen, the three fused speeds after Elevation (in 802, the first
eleven). Item 803 (section 9.9, appendix I) listed the fused down velocity
among the removed plots and is restated as amended: the down velocity is drawn
as Vertical speed. Items 1037 and 1063 (section 9.11) stand as they are; their
evidence counts fifteen rows. The `audit naming` line of 1606 and 1611 is a
floor, not a proof: the rule shows that no stale count of the plots survives
in `docs/` and the root README, not that the new paragraph of
`COMPUTED_PLOTS.md` or the rows of `SENSOR_FUSION.md`'s table exist; those are
read by the reviewer, and M58 is the real check of 1611.

| Item | Section | Statement | Evidence |
| --- | --- | --- | --- |
| 1601 | 2 | the calculations: `builtin.fusion.velH` (`Fusion/velH` from `Fusion/velN` and `Fusion/velE`, the magnitude of the two, as `GNSS/velH` is of the GNSS components) and `builtin.fusion.vel` (`Fusion/vel` from `Fusion/velH` and `Fusion/velD`, as `GNSS/vel`), on demand under the Fusion sensor, no title, no result version, `velH` registered before `vel`; each output has one candidate and the fit alone behind it, so they appear with the fit and never start it; Vertical speed is the fit's own `Fusion/velD`, whose one producer is the fit | `tst_fusion_derived::derivedRegistrationShape`; `tst_fusion_session::registrationShape` |
| 1602 | 2 | the rows: Horizontal speed (`Fusion/velH`), Vertical speed (`Fusion/velD`) and Total speed (`Fusion/vel`) in the "Sensor fusion" category after Elevation and before Horizontal acceleration, each named, united (m/s) and typed (`speed`, `vertical_speed`, `speed`) as its GNSS counterpart so that the two overlay; the category holds fifteen plots, in this order: Elevation, Horizontal speed, Vertical speed, Total speed, Horizontal acceleration, Vertical acceleration, Along-track acceleration, Cross-track acceleration, Heading, Pitch, Roll, Heading accuracy, Tilt accuracy, Horizontal acceleration accuracy, Vertical acceleration accuracy | `tst_fusion_rows::allFusionPlotsAreExplicitBacked`; `audit naming` (fifteen rows); `manual M58` |
| 1603 | 2 | demand: checking any of the three creates demand for the fit exactly as checking a fused acceleration does, one row alone one fit; a stored fit draws them at once, without computing; a logbook column over any of the three works as over any fusion value, loaded and unloaded; none applies to a session without IMU data | `tst_fusion_rows::totalSpeedRowIsBlockedByFusion`, `allFusionPlotsAreExplicitBacked`; `tst_fusion_store::restoredAfterRestartIsBitIdentical`, `fusionColumnFillsUnloadedSessions`; `tst_fusion_jobs::noImuSessionCannotHaveAJob`; `manual M58` |
| 1604 | 2 | the fit is unchanged: its outputs, its algorithm string, its stored results and the goldens; no wind correction (`GNSS/velH` and `GNSS/vel` have none) and no speed accuracy; a profile that named the plots before this change is applied as before | `tst_fusion_session::registrationShape`; `tst_fusion_golden::successFixturesMatchGolden`; `tst_calculation_demand::profileNamingRemovedPlotsAppliesWithoutThem`; `audit accuracy-channels` (four accuracy rows) |
| 1605 | 3 | the colours, chosen by `docs/PLOT_COLOURS.md`: each row is kin of its GNSS counterpart, the neighbouring hue at the other edge of the lightness band, admissible within the speeds group (the three GNSS speeds, Wind-corrected horizontal speed, Speed accuracy and the three fused speeds); Horizontal speed `#bc378e`, Vertical speed `#1b8278`, Total speed `#d446ff`, literals in the plot table with a comment that says what each is coloured by and its figures; each fused line is told from its twin and from the rest of the group on both backgrounds | `audit naming` (a literal colour per row); `manual M58` |
| 1606 | 4 | the documents: COMPUTED_PLOTS.md (the category's count and list, and reading a fused speed beside its GNSS speed: together where the receiver is accurate, apart where it is not, the separation the fit's compromise between the receiver and the IMU, the receiver's Speed accuracy plot saying how far to trust its side of it), SENSOR_FUSION.md section 4 (`velH` and `vel` among the values derived from the outputs; Vertical speed is `velD` itself), CALCULATIONS.md (the two calculations with their inputs and outputs, their definitions, the category's list in its new order), the contract of `registerFusionCalculations` (the two calculations; Vertical speed is `velD` itself); every count of twelve fusion plots in the documents and tests/README.md becomes fifteen | `audit naming` (no count of twelve in `docs/` or the root README) |
| 1607 | 5, first bullet | test: known answers on a fit result with chosen `velN`, `velE` and `velD`, every sample of `Fusion/velH` and `Fusion/vel` the GNSS definition applied to the fused components; both unavailable before the fit has published and when the inputs' lengths differ; both appear with the fit and start none | `tst_fusion_derived::fusedSpeedsKnownAnswers`, `fusedSpeedsAreTheGnssDefinitions`, `derivedValuesWaitOnTheFit`; `tst_fusion_session::requestRunsOnceAndPublishesTogether`, `readsNeverRunTheFit` |
| 1608 | 5, second bullet | test: the category holds the fifteen in the order of 1602; the three new rows are named, united and typed as their GNSS counterparts and backed by the explicit calculation; a legend value over each formats as its GNSS counterpart's does | `tst_fusion_rows::allFusionPlotsAreExplicitBacked`; `tst_plot_format::speedTypesFormatAsTheConverterDoes` |
| 1609 | 5, third bullet | test: checking Total speed alone on a fusable recording without a stored fit starts one fit; a stored fit draws the three without computing | `tst_fusion_rows::totalSpeedRowIsBlockedByFusion`; `tst_fusion_store::restoredAfterRestartIsBitIdentical`, `fusionColumnFillsUnloadedSessions` |
| 1610 | 5, fourth bullet | the audit's `naming` group: the pinned count of fusion plots is fifteen, with `fusionPlots()` of `tests/fusion/fusionsessions.cpp`; the rule that keeps the fit's own channels out of the plot registry drops `velD`, as it already omits `accD`, and its comment says why; the documents' pattern refuses a count of twelve; the comments and documents that counted twelve are updated; the audit and the acceptance-map check stay green | `audit naming` |
| 1611 | 5, fifth bullet | the manual step M58: the three plots in the list in their place; one fit for the three; the three drawn beside the GNSS speeds, Wind-corrected horizontal speed and Speed accuracy, in the light and the dark theme, each fused line told from its twin and from the rest; the legend and the measure tool format each as its counterpart; a column over Total speed fills loaded and unloaded rows | `audit naming`; `manual M58` |
| 1612 | 5, sixth bullet | the acceptance items in the next free hundred of `tests/acceptance_map.txt`, with appendix Q and section 9.17 of `tests/README.md`; items 115, 509, 801, 802, 842, 854, 862 and 863, which counted the fusion plots, and 803, which listed the fused down velocity among the removed plots, restated "(as amended)" | `audit naming` |

### 9.18 Fused position and speed accuracy (items 1701-1717)

The specification "Fused position and speed accuracy"
(`PLANS/fused-accuracies.md`), stated in
[appendix R](#appendix-r-the-acceptance-items-of-the-fused-position-and-speed-accuracy-1701-1717),
has seventeen acceptance items, 1701-1717; the same four line forms as 9.2.
"Section" is the section of the specification; its sections 1 and 2
(motivation, principles) have no item. Items 1701 and 1702 are its section
3, 1703 and 1704 the first two bullets of its section 8, 1705 its section 7
as it concerns the fit's outputs and the record, and 1706 the first clause of
its audit bullet; 1707 is its section 4, 1708 the third bullet of its section
8, and 1709 its section 7 as it concerns the derived accuracies with the
second clause of its audit bullet; 1710-1717 follow with the plots (sections
5 and 6 and the rest of sections 7 and 8), and are written with them. Items
230 and 235 (section 9.3, appendix C), 337 (section 9.4,
appendix D) and 912 (section 9.10, appendix J), which count the fit's
channels or outputs, are restated as amended: thirty-three channels,
thirty-four outputs; item 1302 (section 9.14, appendix N), whose growth
through a hole read as the sample covariance's, is restated as amended: the
step chain's covariance `P_j`; items 1033, 1042 and 1044 (section 9.11), whose evidence
alone counts them, have their evidence text updated. The `audit
accuracy-channels` line of 1705 is a floor, not a proof: the rule shows that
no document counts the fit's channels as twenty-one or its outputs as
twenty-two, not that the paragraphs of `SENSOR_FUSION.md` sections 4, 7 and 8
say what 1705 lists; those are read by the reviewer. Likewise for 1709: the
rule shows that the derived names are spelled in `src` where they should be,
not that the derived table of `SENSOR_FUSION.md`, the registration table and
derived values of `CALCULATIONS.md` and the header's contract describe the
three; those are read by the reviewer.

| Item | Section | Statement | Evidence |
| --- | --- | --- | --- |
| 1701 | 3 | the twelve outputs: `Fusion/posCovNN`, `posCovNE`, `posCovND`, `posCovEE`, `posCovED`, `posCovDD` (m^2) and `velCovNN`, `velCovNE`, `velCovND`, `velCovEE`, `velCovED`, `velCovDD` (m^2/s^2), measurement outputs of the fit after the four accuracies, aligned with `Fusion/_time`: the upper triangles of the position and velocity blocks of the composed sample covariance, turned from the reconstruction's tangent into the navigation frame with the published attitude as the attitude block is, a sample on a fix carrying the fix's own marginal; widened once, by the square of the sample's widening factor; filled for a success whose covariance was computed and empty for every other outcome, exactly when the four accuracies are, the diagnostics' `accuracy` account unchanged; diagonal finite and non-negative wherever filled | `tst_fusion_kernel::sampleCovarianceMatchesTheEdgeGraph`, `sampleOnAFixHasTheFixMarginal`, `wideningGrowsWithAnUnderstatedSigma`, `accuraciesFiniteAndPositive`, `covarianceFailureLeavesTheFitAsItIs`, `imuRateIsWhatTheFitPublishes`; `tst_fusion_session::registrationShape` |
| 1702 | 3 | `v10` and what is unchanged: `Fusion::Algorithm` is `batch-temperature-bias-v10`; a record written under `v9` is stale at load and computed again once, as a result is needed, shown like any computation; nothing else of the record's format changes; the fit's inputs, its solution, the seventeen state channels, the four accuracies and the diagnostics are unchanged and the goldens' existing channels remain bit for bit; neither `Cov(x_j, g)` nor the position-velocity cross block is published | `tst_fusion_session::registrationShape`, `restoredFitIsIndistinguishable`; `tst_fusion_store::recordUnderPreviousAlgorithmIsComputedAgainOnce`; `tst_fusion_golden::successFixturesMatchGolden`, `rejectionFixturesMatchGolden`; `tst_fusion_kernel::fitTraceMatchesGolden`; `audit accuracy-channels`; section 11's capture of 2026-10-06 (the pre-capture check) |
| 1703 | 8, first bullet | test: the kernel: on the fixtures with a computed covariance the published blocks equal the position and velocity marginals of a graph with a state at every edge, on a fix the fix's own marginal; symmetric with a finite, non-negative diagonal wherever filled, empty exactly when the four accuracies are; on `bridged_hole` the composed velocity covariance before the widening is larger inside the hole than beside the fixes around it and returns at the next fix, the step chain's share of the position and velocity covariance grows across the hole and collapses at the next fix, and the composed position figures are recorded, not asserted (the hole's IMU factor ties the two fix marginals so that the position variance follows them across the hole), the published figures recorded beside them and not asserted, and the error against the generating trajectory in units of the published standard deviation is measured, recorded in section 8 of the fusion document and held below 3; the seventeen and the four of every golden unchanged bit for bit, the goldens gaining the twelve | the six kernel tests of 1701; `tst_fusion_kernel::bridgedHoleFollowsTheTruth`; `tst_fusion_golden::successFixturesMatchGolden`, `channelsWriterIsTheInverseOfTheLoader`, `comparatorHoldsItsBounds` |
| 1704 | 8, second bullet | test: the registration and the record: thirty-three measurement outputs in order, thirty-four with the diagnostics; the twelve published and stored with the rest and restored bit for bit after a restart; a record written under `v9` stale at load and the fit run again once; the stored success without the covariance restores the seventeen and leaves the sixteen accuracy channels unavailable | `tst_fusion_session::registrationShape`, `requestRunsOnceAndPublishesTogether`; `tst_fusion_runner::outputTableMatchesGolden`, `successMatchesDirectRun`; `tst_fusion_store::restoredAfterRestartIsBitIdentical`, `recordUnderPreviousAlgorithmIsComputedAgainOnce`, `restoredFitWithoutAccuracyDrawsTheRest`; `tst_fusion_jobs::jobPublishesAllOutputsTogether` |
| 1705 | 7 | the documents of the fit's outputs and the record: `SENSOR_FUSION.md` section 4 (the rows of `J_j`, the twelve in the outputs table, the squared widening, the growth through a hole), section 7 (thirty-four outputs, `v10`) and section 8 (the validation with its measured numbers); `CALCULATIONS.md`'s thirty-four outputs; `DATA_SCHEMA.md`'s thirty-three measurements and `v10`; the contract of `registerFusionCalculations`; every count of twenty-one or twenty-two channels in the documents, the comments and `tests/README.md` updated | `audit accuracy-channels` (a floor; the reviewer reads the rest) |
| 1706 | 8, audit bullet, first clause | the audit's `accuracy-channels` group: the twelve covariance names, quoted, are spelled in `src` in the registration's output table alone, the kernel holding them as `Result` members; the rule planted once; the range 1701-1717 declared and items 1701-1706 complete; the audit and the map check green | `audit accuracy-channels` |
| 1707 | 4 | the derived accuracies: `builtin.fusion.hAcc` (`Fusion/hAcc`, m, from `Fusion/posCovNN`, `posCovNE` and `posCovEE`: the square root of the larger eigenvalue of the horizontal block, one figure for the horizontal plane as `GNSS/hAcc` is, and the cautious one), `builtin.fusion.vAcc` (`Fusion/vAcc`, m, from `Fusion/posCovDD`: the square root of the down variance) and `builtin.fusion.sAcc` (`Fusion/sAcc`, m/s, from the six `velCov` channels, then `velN`, `velE` and `velD`: the standard deviation along the published velocity where the speed is at least that standard deviation, otherwise, a zero velocity included, the square root of the largest eigenvalue of the block, the rule of the horizontal acceleration accuracy); on demand under the Fusion sensor, no title, no result version, registered after `accH` and before the system time in that order; one candidate each with the fit alone behind it, so they appear with the fit and never start it; unavailable when an input is absent or the inputs differ in length, finite and non-negative otherwise; no widening of their own | `tst_fusion_derived::derivedRegistrationShape`, `fusedAccuraciesKnownAnswers`; `tst_fusion_session::registrationShape` |
| 1708 | 8, third bullet | test: known answers on chosen blocks (a diagonal block, a rotated one whose larger eigenvalue is exact, a velocity along one axis and one below its standard deviation, a full 3x3 block); unavailable without the blocks and on unequal lengths; waiting on the fit and never starting it; appearing with the fit | `tst_fusion_derived::fusedAccuraciesKnownAnswers`, `derivedValuesWaitOnTheFit`; `tst_fusion_session::requestRunsOnceAndPublishesTogether`, `readsNeverRunTheFit` |
| 1709 | 7; 8, audit bullet, second clause | the documents and the audit: the derived table of `SENSOR_FUSION.md` section 4, the registration table and derived values of `CALCULATIONS.md` section 17 (seventeen calculations, eleven derived), the contract of `registerFusionCalculations`; the `accuracy-channels` rule that the three derived names, sensor-qualified, are spelled in `src` in the registration and the plot rows alone, planted once; the range's completeness check at 1701-1709; the audit and the map check green | `audit accuracy-channels` (a floor for the documents; the reviewer reads the rest) |

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
- **group `constant-defaults`** (items 835-839, 851): a compute function that
  ignores its context, which is a constant default, appears anywhere but
  `src/calculations/attributecalculations.h`, where the one helper
  `addConstantDefault` writes every one; the local helpers the SP and WS-P
  calculations registered their defaults with before reappear in `src` or
  `tests`; or the importer or the legacy backfill (`dataimporter.*`,
  `logbookmanager.*`) names wind;
- **group `orientation`** (item 821): an orientation token (a signed axis, a
  comma, a signed axis) is spelled in `src` anywhere but
  `src/fusion/orientation.*`: one type owns the mount vocabulary;
- **group `sensor-configuration`** (items 1001, 1004-1006): a sensor
  configuration key is spelled as a quoted literal in `src` anywhere but the
  vocabulary, `src/sensorconfiguration.*`; the firmware version the default
  describes is spelled in `src` on other than exactly one line, or outside the
  vocabulary; or the constants of the five keys nothing uses (the receiver's
  dynamic model and rate, the barometer's, humidity sensor's and
  magnetometer's rates) are named in `src` outside the vocabulary;
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
- **group `branch-mechanisms`** (items 120, 739): a modal progress dialog
  other than the import dialog, a nested event loop, a thread-local or "a
  calculation is running" flag, anything about calculations or jobs in the
  idle scheduler (or the idle scheduler in the executor's code), the plot
  widget's rebuild guard, the sibling-frame scaffolding, a dialog or message
  box for a calculation outcome (which the status bar's warning and the
  logbook row report), the status bar used by the kernel, the calculations or
  the plot list, or a hand-cached failure in the fusion registration
  appears. `m_pendingRebuildLevel` in the plot widget is `master`'s own and is
  not part of the rule;
- **group `naming`** (items 120, 801, 803, 852, 862, 863, 1037, 1065, 1602,
  1605, 1606, 1610-1612): something is named
  after a filter (the case-sensitive pattern `EKF|[Ee]kf`), a branch output
  name (`posN` ...) or the branch's sensor key reappears; `MainWindow` does not
  register exactly fifteen "Sensor fusion" plots (the count pins the
  application's list to `fusionPlots()`, the tests' mirror); a plot row of
  `MainWindow` computes its default colour from a scheme (`QColor::fromHsl`)
  or names a Qt colour instead of stating a literal; the "GNSS (Local
  frame)" category or the tests' old helper for it appears in `src` or
  `tests`; a plot row names a removed fusion measurement (the position, the
  north and east velocity, the north and east acceleration, the device-frame
  roll, pitch and yaw, the quaternion, the twelve entries of the position and
  velocity covariance blocks; the down velocity and the down acceleration
  have rows, Vertical speed and Vertical acceleration); or `docs`
  or the root `README.md` describe an old count of fusion plots (seventeen,
  eight or twelve: `[Ee]ight (real )?(fusion )?plots`, `[Aa]ll eight`,
  `[Tt]welve (real )?(fusion )?plots`, `[Aa]ll twelve`; "the same eight"
  inputs of `CALCULATIONS.md` do not match), the local-frame category or a
  quaternion plot. This file is excluded: this section spells the patterns.
  The fifteen and the document patterns were planted once when they changed;
- **group `solver-confinement`** (items 101, 234, 236, 848, 850, 928, 939,
  1007): a
  GTSAM header is included outside `src/fusion/` and the five GTSAM test and tool
  sources (`tst_solver_smoke.cpp`, `solverprobe.h`, `solver_deploy_probe.cpp`,
  `tst_fusion_kernel.cpp`, `fusion_golden_capture.cpp`), a oneTBB header is
  included anywhere but `src/fusion/solverthreads.cpp` (the adapter that runs
  the solver's helper threads at the caller's priority), the public and
  registration files of the fusion library and its orientation type
  (`fusion.h`, `fusionregistration.*`, `orientation.*`) include GTSAM or
  Eigen, the kernel includes a session, engine, executor, preference or GUI
  header (the registration adapter, `fusionregistration.*`, excepted) or
  logs, `flysight_core`'s calculation, engine, session-model, executor or
  demand-layer code references the fusion library, or Boost reappears in the
  sources or the build;
- **group `one-worker`**: a thread is created anywhere but in `src/jobqueue.*`,
  a lock of any kind appears in `src`, or an atomic other than the executor's
  cancel flag does;
- **group `gestures`** (items 116, 306, 501, 519, 527, 534, 543, 544, 562,
  605, 608, 625, 628, 629, 651, 659, 661, 737, 740, 1509, 1523): a
  gesture entry point (`plotCheckedByUser`, `refreshPressed`, `cancelPressed`)
  is named in `src` or `tests`; `prepare(` / `publish(` is called outside the
  executor and the engine; `request(` outside the engine; anything in `src`
  outside `src/engine` calls the engine's synchronous `request()` (spelled
  through `calculationEngine()`, `engine.` or `engine->`); `offer(` is called
  on other than exactly one line of `src`, or anywhere but
  `calculationdemand.cpp`, or `withdrawChosenNext(` anywhere but there (the
  demand layer is the only caller of the executor); the executor's `cancel(`
  is called on other than exactly one line of `src`, or anywhere but
  `calculationdemand.cpp` (the demand layer is the one product caller of
  cancel, for a recording switched off); the UI refers to the executor beyond
  `AppContext.h`;
  `EvaluationPolicy::Explicit` is tested outside the engine
  (`CalculationRegistry::explicitDependencies()` is the one authority for
  "explicit-backed"; `dependsOnExplicit()` is its non-emptiness);
  `cancel(JobId` is not declared exactly once in `src/jobqueue.h` (with the
  caller's reason); the executor's removed idle and queued signals, its two-job
  query or its busy-period bookkeeping (`jobQueued`, `activeJobs(`,
  `JobQueue::idle`, `announceIdleIfIdle`, `m_idleAnnounced`, `AfterEnd`)
  appear in `src` or `tests`, or `idle()` in `src`; or the demand layer's
  own-offer memory (`m_offeredJob`, `withdrawOwnOffer`) reappears;
- **group `widget-free-core`**: the executor, the job model, the demand
  layer (its four files: its presentation values and their text use Qt Core
  only) or the plot model include a widget header (a style option and a
  header view included);
- **group `fusion-model`** (items 212, 218, 234, 247, 902, 903, 927, 929,
  939, 940, 1027): a name of the retired resting-window detector or of its gates
  reappears in `src`, `tests`, `cmake`, `docs`, `README.md` or
  `CMakeLists.txt`; the silent cancellation
  poll or the retired single-anchor initial-attitude type reappears in `src`
  or `tests`; a retired constant-bias algorithm string (the `v1` / `v2`
  strings the goldens once carried; the goldens say `batch-temperature-bias-v10`,
  and a hit under `tests/data/fusion` means a stale capture) appears in `src`,
  `tests`, `docs` or `README.md`; the name of the branch the kernel was ported
  from appears anywhere but in this file's historical notes and in the
  provenance comment of `tst_fusion_kernel.cpp`, or this file's count of it
  changes; the kernel has other than four `checkpoint(` call sites (`Starting
  fit`, the pass iteration, `Integrating IMU factors`, `Releasing the scale
  factors`: the four kinds of boundary) or other than one `.integrateMeasurement(` call in
  `imuintegration.cpp` (the per-step covariance is set before it); an
  integration call (the member-call form of `integrateMeasurement(`) or the
  library's static tangent update (`UpdatePreintegrated`) appears anywhere in
  `src` or `tests` but `imuintegration.cpp`, or a sensor covariance
  (`accelerometerCovariance`, `gyroscopeCovariance`, `integrationCovariance`)
  is assigned there: the step model has one author, and the reconstruction
  and its tests read it (the step-model tests read the covariances through
  the observer and `pim.p()`, which is not an assignment; every step's
  transition is the library's update on a copy, taken in the integration loop
  and handed to the observer); or
  `docs/SENSOR_FUSION.md` names a
  retired mechanism (a resting or candidate window, the coarse-only
  initializer, a frozen algorithm, the bias-shift test, the branch, or the
  input count from before the configuration inputs, banned as an input count
  only: the fit has thirty-four outputs since the fused accuracies).
  This file is excluded from the text rules of this group because this
  section spells the patterns;
- **group `noise-model`** (items 247, 1012-1014, 1016, 1017, 1046): a
  retired tuning constant of the noise (`accDensity`, `gyroDensity`,
  `accStepSlope`, `gyroStepSlope`, `stepSigma`) or the retired diagnostics
  keys (`per_step`, `gyro_slope_s`, `acc_slope_s`) appear in `src`, `tests`
  (the goldens included: a hit under `tests/data/fusion` is a stale capture),
  `docs` or the root `README.md`; a figure of the datasheet's table (the
  gyro's LPF2 cutoffs `1441.8`, `1320.7`, `1108.1`, `295.5`, `135.9`, and the
  noise densities as `src/fusion/sensornoise.cpp` spells them, `110e-6`,
  `80e-6`, `75e-6`, `70e-6`, `3.8e-3`) appears in `src` outside that unit; the
  unit includes GTSAM or Eigen; `docs/SENSOR_FUSION.md` calls the densities
  modelling weights or the slopes calibrated; or that document stops citing
  Table 2 (nine lines), Table 18 (two), Table 65 and Figure 17 (one each), or
  stops writing the derivation's `h^3 / 12` (one line). This file and the
  acceptance map are excluded from the first rule because they name the
  retired constants in their history;
- **group `scale-state`** (items 1018, 1023, 1048): the legacy gyro
  correction (`1.14688`, or the conversion layer's `kLegacyGyroScale`)
  appears in `src/fusion`, which receives corrected readings; the scaled IMU
  factor (`ScaledImuFactor`) appears in `src` outside its unit
  (`scaledimufactor.h` / `.cpp`) and the graph builder (`factorgraphfit.cpp`);
  the temperature factor it replaced (`TemperatureImuFactor`, its header
  `temperatureimufactor`) or the switch of the scale state (`scaleState`)
  appears in `src`, `tests`, `docs` or the root `README.md`;
  a document under `docs` says the scale factor is not fitted; or
  `docs/SENSOR_FUSION.md` stops naming the `scale_prior` residual kind or
  `model.scale` (one line each). Each rule was planted once when it was
  written;
- **group `accuracy-channels`** (items 1022, 1025, 1030, 1032, 1036, 1037, 1049,
  1064, 1065, 1702, 1705, 1706, 1709):
  `jointMarginalCovariance`, the library's joint marginals, appears in `src`
  (the covariance comes from one factorization; the tests use them as a
  reference); an elimination call (`eliminateMultifrontal`,
  `eliminateSequential`) appears in `src` outside the covariance step's unit
  (`src/fusion/fitcovariance.cpp`), or that unit has other than one
  `eliminateMultifrontal(` call; `src/fusion` defines the attitude sigma's cap
  (`constexpr double kYawSigmaCapDeg`) other than once; one of the four
  accuracy channels' names (`"headingAcc"`, `"tiltAcc"`, `"accHAcc"`,
  `"accDAcc"`, quoted) appears in `src` outside the registration's output
  table (`fusionregistration.cpp`) and the plot rows (`mainwindow.cpp`); one
  of the twelve covariance names (`"posCovNN"` ... `"posCovDD"`,
  `"velCovNN"` ... `"velCovDD"`, quoted) appears in `src` outside the
  registration (the kernel holds them as `Result` members, unquoted, and no
  plot row names them); one of the three derived accuracies' names,
  qualified by the `Fusion` sensor (`kSensor, "hAcc"`, `"Fusion", "vAcc"`,
  ..., `sAcc`), appears in `src` outside the registration
  (`fusionregistration.cpp`) and the plot rows (`mainwindow.cpp`) (the bare
  strings are also the receiver's names, so only the qualified form is
  matched); `docs` or the root `README.md` count the fit's
  channels or outputs as they were before the covariance blocks
  (`twenty-one (measurement|channel)|twenty-two outputs`; this file is not
  searched, since section 11's capture history counts what each capture
  wrote); `src/mainwindow.cpp` has other than four "Sensor fusion" rows whose name
  ends in " accuracy", each with a literal colour (`QColor(0x...)`); `no
  uncertainty` appears in `src` or `docs`; or `docs/SENSOR_FUSION.md` stops
  writing, on one line each, the propagation `a = R (f / s - b) + g`,
  "gravity's own uncertainty", "a-posteriori variance factor", "never
  tightens" and the accuracy's one sentence, or "cross-axis sensitivity" on
  two (the accuracy's omissions and section 5's limitations). Each rule was
  planted once when it was written;
- **group `model-validation`** (items 1024, 1055, 1064, 1065):
  `docs/SENSOR_FUSION.md` has other than one line with the sentence "...
  measures what the model does not yet describe", or other than one of each
  of the bold heads `**Validating the model.**` and `**What is and is not
  validated.**` (section 8: the normalized residuals of the reference
  recordings and of the fixtures that fit, as measured by M52). Each rule was
  planted once when it was written;
- **group `fusion-tooling`** (items 231, 233, 848, 928, 939, 1049):
  `fusion_runner.cpp` names the preferences singleton, the logbook manager, the engine's preference
  provider, the session model, the application's import driver, the
  import-time defaults or the executor; the runner or the capture tool
  formats a number with anything but `CsvFormat` (`QString::number(`, the
  shortest-form flag, `std::to_chars`, `QLocale`, a `'g', 17` format); an
  install rule names `fusion_runner`, `fusion_golden_capture` or
  `solver_deploy_probe`; or anything under `tests` other than
  `tst_fusion_kernel.cpp` includes an internal fusion header (`sensornoise.h`,
  `scaledimufactor.h` and `fitcovariance.h` among them; the runner sees
  `fusion/fusion.h` and `fusion/fusionregistration.h` only; the capture tool
  and `fusiontrace.h` see the trace seam `fusion/fusionpipeline.h`, which is
  not in the pattern);
- **group `fusion-reconstruction`** (items 917, 926, 939, 940): a name of the
  linear reconstruction the IMU-rate one replaced (`DenseTrajectory`,
  `reconstructTrajectory`, `endpointCorrection`, `propagateThroughInterval`)
  or the replaced diagnostics key `display_position_velocity` appears in
  `src`, `tests` (the goldens included: a hit under `tests/data/fusion` is a
  stale capture), `docs` or the root `README.md`; or a text there says that
  the fused output is interpolated ("interpolated linearly onto", "states
  interpolated", "interpolation of the optimized GNSS states", "interpolated
  for display", "display-only interpolation", "not an IMU-rate smoothing
  posterior", "distributed correction"; the linear interpolation of the
  readings between samples that the integration applies matches none of
  them). This file is excluded because this section spells the patterns;
- **group `gnss-acceleration-accuracy`** (item 1101): `src` holds a central
  difference over two intervals (`[i + 1] - <name>[i - 1]`) other than once,
  the derivative's stencil that `GNSS/accAcc` shares; `"accAcc"` is spelled in
  `src` outside its registration (`gnsscalculations.cpp`) and its plot row
  (`mainwindow.cpp`); `src/mainwindow.cpp` has other than one "GNSS
  (Advanced)" row "Acceleration accuracy" in m/s^2 of the `acceleration` type
  with a literal colour (`QColor(0x...)`); or
  `docs/CALCULATIONS.md` stops writing, on one line each, the assumption
  ("the two fixes' velocity errors are independent") and the measurement
  ("0.09 g RMS"). Each rule was planted once when it was written;
- **group `sample-continuity`** (items 1201, 1202, 1204, 1208, 1210, 1211,
  1213): the continuity factor (`1.5` as a number of its own) appears in
  `src` outside `src/samplecontinuity.*`; the phrase that defines a hole
  ("strictly greater than 1.5 times the nominal interval") is spelled other
  than once in `src`, or outside `src/samplecontinuity.h`; a median of a time
  axis or a selection over its intervals (the kernel's former median and
  quantile functions by name, or `nth_element`) appears in `src` outside
  `src/samplecontinuity.cpp`; the ten
  files that consult the authority directly (the plot utilities, the
  interpolation family, the derivative helper, the attribute calculations,
  the simplified track, the two map models, and `fusion.cpp`,
  `fusionsamples.cpp` and `imuintegration.cpp` of the kernel) do not each
  include its header; `PlotWidget.cpp` builds its graphs other than with
  `graphData()` once or reads them other than with `interpolateGraphAt()`
  once; `setgroundtool.cpp` reads the elevation other than with
  `groundElevationAt()` or searches samples of its own (`lower_bound`,
  `qFuzzyCompare`); the kernel's former gap constant or its former statistics
  unit is named in `src`, `tests`, `docs` or `README.md`;
  `docs/COMPUTED_PLOTS.md` has other than one section "Holes in the data" or
  stops saying once that a crossing inside a hole is "placed by linear
  interpolation between the two samples around the hole";
  `docs/CALCULATIONS.md` stops saying once that "a stencil never spans a
  hole"; or `docs/SENSOR_FUSION.md` names a gap factor of 1.6 times the median
  IMU interval or stops stating once that a gap is longer than 1.5 times it.
  Each rule was planted once when it was written;
- **group `gnss-holes`** (items 1301, 1305-1313): the kernel's former rule
  on the spacing of the GNSS fixes (its check, its two constants), its
  rejection reason, or the rejection fixture that tested it is named in
  `src`, `tests` (this file and the goldens included), `docs`, `README.md` or
  `CMakeLists.txt`; the cap on a hole's length is defined other than once in
  `src`, in `fusion.cpp`, or its value is written elsewhere in `src` than in
  that definition, or `docs/SENSOR_FUSION.md` stops stating it once with the
  measurement behind it; the slow tail's bound on the normalized RMS is
  compared in `factorgraphfit.cpp` on other than three lines (position,
  velocity and IMU); the previous algorithm string appears in `src`, `tests`
  (the goldens included) or `docs`, this file quoting it as history; the
  goldens under `tests/data/fusion` hold other than fourteen `algorithm` lines
  with the current string, four fits, ten rejections and four `gnss_holes`
  keys; the key `gnss_holes` is written other than once in `src`, or outside
  `fusionoutput.cpp`; the holes of a window are walked other than once in
  `src`, outside `fusionsamples.cpp`; `docs/SENSOR_FUSION.md` states a rule on
  the spacing of the fixes, stops saying once that a hole is "bridged by the
  IMU", stops naming the slow tail's three bounds, stops naming the first of
  the two reference recordings of its section 8 once and the second twice
  (section 4's case of the staged scale and section 8's row), or names the
  current string other than twice; a document counts the golden fits and rejections as they
  were before; `docs/COMPUTED_PLOTS.md` stops saying once that the fusion
  plots "draw through a hole in the GNSS fixes"; or `docs/DATA_SCHEMA.md`
  (twice) or `docs/CALCULATIONS.md` (once) stops naming the current string.
  Each rule was planted once when it was written;
- **group `staged-scale`** (items 1401, 1404-1407, 1412, 1414, 1415): the
  previous algorithm string, `batch-temperature-bias-v8`, appears in `src`,
  `tests` (the goldens included), `docs` or `README.md`, this file and the
  acceptance map quoting it as history; the test helper that held the scale
  at one with a second fit (`withScaleHeldAtOne`) reappears in `src`, `tests`
  or `docs`; the diagnostics key `scale_release` is written other than once
  in `src`, or outside `fusionoutput.cpp`, or the goldens hold it other than
  four times; the rule `diverged` is defined other than once, or the text
  `Releasing the scale factors` is reported other than once, in `src`; the
  divergence bounds are read in `src` outside the tuning (`fusionsamples.h`,
  and its validation in `fusionsamples.cpp`), the fit and the writer of the
  stopping account, or the release budget outside the tuning (the same two
  files) and the fit; or `docs/SENSOR_FUSION.md` names `scale_release` or the
  release boundary other than once, stops stating the rule `diverged`
  ("rebuilt after a pass has left the model", once), counts six rules, or
  stops attributing the measured release to M56 (once). This file is
  excluded from the helper's rule because this section spells its name. Each
  rule was planted once when it was written;
- **group `background-computation`** (items 1501, 1508, 1509, 1512,
  1522-1524): the Compute attribute's tokens (`"on"`, `"off"`) are spelled in
  `src` anywhere but `src/sessiondata.h`, or its labels (`"On"`, `"Off"`)
  anywhere but `src/calculations/attributeregistration.cpp`; `_COMPUTE` or
  `SessionKeys::Compute`, `ComputeOn`, `ComputeOff` is named in `src` outside
  the key's home, the demand layer (`calculationdemand.cpp`), the importer,
  the index (`logbookmanager.cpp`), the definition, the constant default and
  the preference's registration (`mainwindow.cpp`) and page
  (`importsettingspage.cpp`): the engine, the stores, the column worker, the
  fusion library and the kernel never read it; `isComputeOff(` is called
  outside the index and the demand layer; the cancel's reason "Switched off
  for this recording", the cell's word `"excluded"` or its tooltip is spelled
  other than once in `src`; the index entry `"computeOff"` is on other than
  two lines of `logbookmanager.cpp` (its read and its write) or quoted outside
  the manager; the import page's text is spelled other than once in
  `importsettingspage.cpp`; `docs/COMPUTED_PLOTS.md` names "Set Compute..."
  other than once, `docs/CALCULATIONS.md` the cancel's reason other than once,
  or this file has other than one step **M57**. Each rule was planted once
  when it was written;
- **group `stored-results`** (items 304, 305, 316, 317, 326, 327, 330, 333,
  334, 346, 348, 349, 921, 930): the record file extension is spelled as a literal in a
  `.cpp` other than `calculationrecord.cpp`; a record file name is built or
  parsed outside the record format and the logbook manager; a record is
  written, read or removed by anything but the result store and the logbook
  manager; the result store is used outside the session model; anything but
  the store exports or restores an engine result; anything but the session
  model installs the explicit-result listener or calls `restoreSession`; the
  session model has other than exactly two `restoreSession` call sites (a
  row's load, `restoreStoredResults()`, and the column worker's temporary
  copy, `restoreForColumnWorker()`, which is called from one place only - the
  worker's step - so the bulk edit's temporary load never restores); the
  store names the executor, the demand layer or a request / offer / prepare
  / publish call; the exporter, the importer, the merge, the number formatter or
  `SessionData` names a record; the record or store code includes a widget
  header; the fusion algorithm string appears anywhere but
  `src/fusion/fusion.h`; the compatibility rule stops naming the result
  version exactly once in `builtincalculations.h` or in
  `docs/CALCULATIONS.md`; the engine (`src/engine/`) includes anything from
  `calculations/`, `fusion/`, the logbook or the session model (the layer
  below the store knows nothing above it); or any text outside this file says
  that requested results are kept in memory only or are not saved. This file
  is excluded from the last rule because this section describes it;
- **group `result-validity`** (items 409, 410, 413, 415, 417, 429, 439, 441):
  the record format, the result store or the snapshot names the calculation
  environment fingerprint (`calculationEnvironment...`); the plug-in code
  identity is computed, or the plug-in folder read for it, outside
  `plugincodeidentity.*` and the plug-in host; a result version is assigned
  outside the engine, the fusion registration and the plug-in host; a
  registration is removed as teardown (`Removal::Teardown`) anywhere but the
  altitude-marker manager's destructor; the plug-in host or its adapters
  unregister anything; the plug-in host is initialised anywhere but once in
  `MainWindow`, or anything in `src` watches files; the removed whole-session
  marking of records as unconfirmed on an environment change reappears
  anywhere in `src` or `tests`, or anything but the result store marks a
  record skipped; the new bump-rule sentence is not in
  `builtincalculations.h` exactly once, or the old one ("Bump it, or the
  calculation's result version") appears in `src`, `docs` or `README.md`; or
  any text outside this file says that a change of registrations or
  preferences makes every stored result stale. This file is excluded from the
  last rule because this section describes it;
- **group `demand`** (items 506, 508, 513, 515, 518, 527, 529, 530, 533-535,
  538, 540-544, 546, 547, 563, 601-605, 607, 612, 613, 615, 616, 622, 624,
  627, 631, 636, 641, 644, 646, 647, 649-652, 659-662, 701, 702, 706, 708,
  723-728, 732, 736-744, 752-754, 853):
  `CalculationDemand` is named outside the demand layer's files
  (`calculationdemand`, `demandstate`, `demandfill`, `demandsettleclock`),
  `MainWindow`, `AppContext.h`, the logbook view, cell delegate and dock
  feature, the plot widget and the status bar (`StatusBarFeature.*`); the
  executor, the job model, the session model, the scheduler, the logbook, the
  column store, the plot model, the profile bridge, the result store or the
  engine includes a header of the demand layer; anything in `src/ui` or
  `MainWindow` runs a pass, the load step or a settle seam of the demand
  layer; the cell delegate or the status bar handles a mouse or key event of
  its own; the pending and excluded queries (`isCellPending`, `showsPending`,
  `pendingText(`, `pendingToolTip(`, `isCellExcluded`, `showsExcluded`,
  `excludedText(`, `excludedToolTip(`) appear outside the demand layer and
  the cell delegate; the session model, the logbook, the column store, the scheduler
  or the plot model names the executor; the idle scheduler mentions demand,
  calculations, jobs or an executor; `ColumnFillTask` appears outside the
  reconciler, the fill, `sessionmodel.h` and the status bar (which maps the
  fill to the computations), or `loadPinnedSession(` outside the reconciler,
  the fill and the session model; the demand layer loads a session or reads a
  record itself; the demand layer loads or pins other than through its fill;
  the fill or the settle clock calls the executor or names a type of the walk
  or of the presentation (`BlockerReport`, `TrackCondition`,
  `RowStabilityGuard`, `PairMemory`, `LearnedFact`, `DemandProgress`,
  `SessionFailures`, `FailedCalculation`); `kMaxRunningJobs` is assigned
  other than once, the load bound in `demandfill.h` is not
  `JobQueue::kMaxRunningJobs + 1`, or the worker is not started with
  `QThread::LowPriority`; the removed queue API (`oldestQueued`,
  `cancelUnwantedQueued`, `cancelSession(`, `cancelAll(`, `RequestResult`)
  appears in `src`, `docs` or `README.md`; the plot request logic
  (`PlotRequests`, `PlotRowState`, `PlotTrackCondition`, `tst_plot_requests`)
  appears anywhere; a refresh or cancel control is named in `src`, `docs` or
  `README.md` ("refresh icon", "cancel control", "press refresh", "circled x"
  ...), or its code (`drawRefreshGlyph`, `controlHit` ...) in `src` or
  `tests`; a profile in `src/resources/profiles` carries a column over a
  Sensor fusion value; `logbookColumnExplicitCalculations(` is called outside
  the registry-side definition (`logbookcolumn.*`), the session model and the
  logbook index (one computation of a column's requested calculations); the
  demand layer calls the registry's `explicitDependencies(` or
  `staticDependencies(` more than once each (a plot's), reads a record's
  reason more than once, or holds more than one row stability guard; the
  demand layer or the executor names `SessionKeys::Description` or
  `_DESCRIPTION` (one display name, the session model's); the demand layer
  observes a display change of the model (a `::dataChanged` other than the
  plot model's one check-state connection); a name the one walk and the one
  pair memory replaced (`isFillEnding`, `Settlement`, `ColumnWalk`,
  `walkColumns`, `plotCandidates`, `syncColumns`, `rowDisplayName`,
  `onSessionDataChanged`, `CalculationDemand::buildToolTip` ..., a track's
  `.settling`, a state's `.waiting`) appears in `src` or `tests`; a view
  keeps a label, a cluster or a clock (`progressLabel`, `clusterRect`,
  `syncAnimation`, `WorkingAnimation`, `followDemand`, `workingClock`,
  `frameAdvanced` in `src` or `tests`); anything in `src/ui` but the cell
  delegate and the status bar connects to a `destroyed` signal (each view
  that holds the demand layer learns of its end itself); the per-source
  presentation (`DemandState`, `DemandTrack`, `DemandCondition`,
  `plotState(`, `columnState(`, `workingPlotIds`, `workingColumnIds`, their
  change signals, the per-source counts ...) appears in `src` or `tests`, or
  the demand layer keeps a per-source tally, list or tooltip (`addTrack`,
  `runningCount`, `jobFailure`, `isWorking(`, `toolTip` ...); the indicators
  and their plumbing (`DemandIndicator`, `drawDemandGlyph`, `glyphMetrics`,
  `showIndicatorToolTip`, `PlotRowDelegate`, `PlotRowLayout`,
  `LogbookHeaderView` ...) appear in `src` or `tests`; the style's warning
  icon (`SP_MessageBoxWarning`) is drawn anywhere but the status bar and the
  cell delegate (one drawing of the warning glyph); the plot list names the
  demand layer or installs a delegate (it presents nothing of the demand
  layer); a label of background work ("Saving sessions", "Loading sessions",
  "Updating sessions", "Computing columns", "Computing results") appears
  outside `StatusBarFeature.cpp`, anything else connects the scheduler's
  `activeTaskChanged`, `progressChanged` or `schedulerIdle`, or anything but
  `mainwindow.cpp` creates the status bar's component (the status bar's
  labels, the scheduler's signals and its creation in one place each); the
  logbook's sources name a progress bar, the scheduler, `minimumSizeHint` or
  `cancelRequested` (the logbook presents no task progress); the fill
  registers a progress or keeps a high-water mark (the fill reports no
  progress); or `docs` or `README.md` name the removed executor API, the
  per-cell memory, `progressLabel`, the fill's ending step, one clock per
  view, a "k of n" beside the arc or a count beside the triangle, or the
  removed presentation (`plotState`, `columnState`, `DemandState`,
  `jobFailure`, `WorkingAnimation`, `workingClock`, `DemandIndicator`,
  `PlotRowDelegate`, `PlotRowLayout`, `LogbookHeaderView`, a working
  indicator, a warning badge or badges, a turning arc, the progress line
  ...): the documents name none of it. This file is excluded from the text
  rules because this section spells the patterns;
- a line of `tests/acceptance_map.txt` is malformed, names a test function, a
  manual step (`**M<k> ` in this file), a CI token or an audit group that does
  not exist, or an item outside 1-19, 101-120, 201-247, 301-350, 401-442,
  501-563, 601-662, 701-754, 801-863, 901-940, 1001-1065, 1101, 1201-1213,
  1301-1313 and 1401-1415; an item 1-19 has no line; or an item 101-120,
  201-247, 301-350, 401-442, 501-563, 601-662, 701-754, 801-863, 901-940,
  1001-1065, 1101, 1201-1213, 1301-1313 or 1401-1415 has no test or audit
  line.

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
`flysight_fusion`) produced for fourteen synthetic fixtures when the goldens were
last captured by `fusion_golden_capture`, the capture tool built with the
fusion tests. `tst_fusion_golden` and `tst_fusion_kernel` compare the kernel
with them, and so, through the registered calculation and the executor, do
`tst_fusion_session`, `tst_fusion_jobs` and `tst_fusion_rows` and, through a
stored and restored record, `tst_fusion_store` (sensor-fusion-jobs acceptance
4). A golden test that goes red means that the
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
capture the branch is the source of nothing. Since then every phase that
changed numerical results re-captured (the stopping rule, the per-step term,
the segmented initializer, the temperature model, the reconstruction at the IMU
samples, the mid-step rotation, the documented noise model, the GNSS holes
bridged by the IMU); the goldens on disk are the capture of the last of them.

The capture of 2026-09-30 (the reconstruction at the IMU samples): the
published channels became the fitted state at every IMU sample
(`docs/SENSOR_FUSION.md` section 4) and the algorithm string changed to
`batch-temperature-bias-v4`. What changed: in the three successes, the
channels other than `_time` and, in the diagnostics, `algorithm`,
`dense_output` (in place of `display_position_velocity`), `limitations`,
`max_endpoint_correction_deg` (now the attitude part of the mismatch, the same
quantity to rounding) and the three new keys `max_step_correction_m_s2`,
`max_step_correction_time_s` and `max_velocity_mismatch_m_s`; in the nine
rejections, `algorithm` alone. What did not: the fit, so `trace`, `progress`,
`rows`, `objective`, `residuals`, `seeds`, `stopping`, `quality`, `model`,
`initializer`, `input`, `start_s`, `end_s` and `imu_outputs` are those of the
previous capture, and the time axis: the `_time` column of every success
fixture is byte-identical to the previous capture's (`coarse_linear` 160
lines, `coarse_maneuver` 540, `stationary_spin` 995).

The capture of 2026-09-30 (the mid-step rotation, the same day): the
accelerometer reading of every integration step is turned by half the step's
rotation before the library applies it (`docs/SENSOR_FUSION.md` section 4),
and the algorithm string changed to `batch-temperature-bias-v5`. What
changed: the fit itself, so in the three successes every number of the
diagnostics that the fit produces (`trace`, `objective`, `residuals`,
`stopping`, `quality`, `model`, `initializer`, the reconstruction's four
numbers) and every channel but `_time`; in the nine rejections, `algorithm`
alone. What did not: the time axis, checked byte for byte as before, the
row counts, and every rejection's reason.

The capture of 2026-10-01 (the documented noise model, part 1, phase 2): the
fit's noise is the IMU datasheet's at each fixture's stated configuration,
with the derived sampling term and mid-step remainder in place of the
per-step slopes, the damping ceiling is 1e12, every fixture states its
configuration and its readings are rounded onto that configuration's lattice,
and the algorithm string changed to `batch-temperature-bias-v6`
(`docs/SENSOR_FUSION.md` sections 3, 4 and 6). What changed: in the three
successes everything but `_time`, `rows` and the counts (every channel, the
`trace`, and in the diagnostics `algorithm`, the new `configuration`,
`model.noise` in place of `model.per_step`, `stopping` with its new
`lambda_upper_bound`, `objective`, `residuals`, `seeds`, `quality`, the
`initializer` account and the reconstruction's four numbers); in the nine
rejections that were there, `algorithm` alone; and two new rejections,
`reject_lattice.json` and `reject_rate.json`, whose reasons are the new
checks'. What did not: the time axis (`coarse_linear` 160 lines,
`coarse_maneuver` 540, `stationary_spin` 995, each `_time` column
byte-identical to the previous capture's), the row and state counts, every
GNSS value of every fixture (the rounding draws nothing), and every earlier
rejection's reason, the configuration's checks coming after every other.

The capture of 2026-10-01 (the documented noise model, part 1, phase 3): the
full fit carries the scale state, six scale factors by which the readings are
divided, with their prior at the datasheet's sensitivity tolerance, the graph
re-preintegrated at the fitted scale, and in the diagnostics `model.scale`,
the `scale_prior` residual and `limitations` naming the held scale factors
(`docs/SENSOR_FUSION.md` section 4); the algorithm string stays
`batch-temperature-bias-v6`. Before the capture, with the state switched off
in `planFit` and the two diagnostics additions held back (a local edit, not
committed), every golden test passed against the previous capture, exact
mode included. What changed: in the three successes every channel but
`_time`, the fit's `trace` history (`coarse_maneuver` now settles in two
passes of six iterations, against one of four, and so has more `progress`
texts), and in the diagnostics `objective`, `residuals`, `seeds`, `stopping`,
`quality`, `model` (the new `scale`), `limitations` and the reconstruction's
numbers; the z accelerometer biases by about 10 %, which the z factors now
share under gravity (`coarse_maneuver` 0.0903 to 0.0828 m/s^2 with `s_z`
0.99917, `stationary_spin` 0.0498 to 0.0450 with 0.99951). What did not: the
eleven rejection goldens, byte-identical to the previous capture's; the time
axis (`coarse_linear` 160 lines, `coarse_maneuver` 540, `stationary_spin`
995, each `_time` column byte-identical); the row and state counts; and the
initializer's account, whose fits are stock and take the readings at unit
scale.

The capture of 2026-10-01 (the documented noise model, part 1, phase 4): the
fit publishes its accuracy (`docs/SENSOR_FUSION.md` section 4): after
convergence the covariance step factorizes the converged graph once, the
reconstruction composes the covariance at every sample, and each sample's
heading, tilt, horizontal and vertical acceleration accuracies, widened where
the residuals exceed the model, are four new channels; the diagnostics gain
`accuracy`, the scale factors' sigmas `acc_sigma` and `gyro_sigma` under
`model.scale`, and `limitations` describing the accuracy; the algorithm
string stays `batch-temperature-bias-v6`. Before the capture, the capture
tool of this build (with the seventeen-column channels file of the previous
capture) wrote, against the previous capture, the seventeen columns of every
channels file byte-identical, every diagnostics key but `limitations`
identical once the new keys are set aside (the trace, the progress texts and
the rows included), and the eleven rejection goldens byte-identical: this
phase changes no existing number. What changed: each channels file's column
line lists the twenty-one names under the same `v1` header, the four
accuracies added after `qw`; and in the three successes' diagnostics the new
`accuracy` (every widening one: `max_widening` 1, no sample widened;
`coarse_linear`'s 160 headings at the cap, `undetermined_heading_samples`
160), the scale sigmas (`coarse_maneuver`'s accelerometer z 0.0095, the other
factors at their prior's 0.01) and `limitations`. What did not: every number
there was, the eleven rejection goldens, and the time axis (`coarse_linear`
160 lines, `coarse_maneuver` 540, `stationary_spin` 995, each `_time` column
byte-identical).

The capture of 2026-10-01 (the lattice rule, a fixup after part 1): the
range a recording shows is the coarsest lattice all but one in a thousand of
its readings fit, where before every reading had to; two reference
recordings (`24-09-05/11-16-56`, `24-09-04/16-16-09`) with one or two gyro
readings nine units off at 500 deg/s had been rejected. The algorithm string
changed to `batch-temperature-bias-v7` so that a stored rejection from
before the rule is tried again. What changed: `algorithm` in all fourteen
`.json` files and nothing else; every channel file and the time axes are
byte-identical to the previous capture.

The capture of 2026-10-03 (GNSS holes bridged by the IMU), at `683050c`: the
kernel no longer rejects a recording for the spacing of its GNSS fixes below
a cap of 30 s; a hole in the fixes is bridged by the one IMU factor that
spans it, the cutter of the initializer merges any piece with fewer than
three fixes, the success diagnostics' input audit gains `gnss_holes`, the
slow tail is bounded on the IMU normalized RMS too, and the algorithm string
changed to `batch-temperature-bias-v8` (`docs/SENSOR_FUSION.md` sections 4 to
7). No fixture has a hole longer than the cap or ends on a slow tail, so
neither of those changes a golden. The
recording of the former GNSS gap rejection became the fourth success fixture,
`bridged_hole`: its `.json` and `.channels.txt` are new, the rejection's golden
was deleted by hand (the tool never deletes one), and the goldens hold four
fits and ten rejections. What changed besides: in `coarse_linear`,
`coarse_maneuver` and `stationary_spin`, `algorithm` and an empty
`input.gnss_holes` and nothing else, since none of them has a hole or a sparse
piece of the cut; in the ten rejections, `algorithm` alone; and
`capture.json`. What did not: the three channel files, byte-identical to the
previous capture's (160, 540 and 995 lines), every other number and text of
those three `.json` files (`trace`, `progress`, `rows`, `objective`,
`residuals`, `stopping`, `quality`, `model`, `initializer`, `accuracy`), and
every rejection's reason. A capture that had changed anything else would have
been a kernel regression, not a golden update. `bridged_hole` settles in three
passes (9 iterations, objective 1.131): 16 states, 540 output samples, 260 of
them inside the hole, and one entry in `gnss_holes` (`start_s` 2.2, `length_s`
2.6).

The capture of 2026-10-03 (the scale factors as a refinement), at `021a65f`:
the full fit runs in two stages, the scale factors held at one by a prior a
thousand times tighter than the datasheet's until the fit has converged and
then released from that solution with a budget of three passes, the held fit
reported when the release does not converge; a pass whose rebuilt graph has
left the model ends its stage under `diverged`; the success diagnostics gain
`scale_release` and the stopping account the two divergence bounds; and the
algorithm string changed to `batch-temperature-bias-v9`
(`docs/SENSOR_FUSION.md` sections 4 and 7). Every fit's release settles and
is kept: `coarse_linear` holds in one pass of 2 iterations and releases in
one of 1, `coarse_maneuver` in one of 4 and two of 5, `stationary_spin` in
three of 10 and two of 4, `bridged_hole` in three of 9 and two of 5, so
`seeds[0].iterations` is 3, 9, 14 and 14 (2, 6, 10 and 9 before) and
`stopping.passes`, the released stage's own, 1, 2, 2 and 2 (1, 2, 3 and 3).
What changed: in the four fits, `scale_release` (new), `stopping` (the
released stage's account and the two bounds), `trace.history` (the released
stage's rows, `outer` numbered on from the held stage's), `progress`
(`Releasing the scale factors` and the released passes: 29, 100, 195 and 81
texts against 25, 94, 187 and 72), and every number of the fit (`objective`,
`residuals`, `quality`, `seeds`, `model`, the reconstruction's four
summaries; the objective by at most 3e-7 of itself, since each release ends
at the minimum of the single fit with the factors free from the first pass); every column of the four channel files but
`_time`, except `coarse_linear`'s `qw` and `headingAcc`, which are
byte-identical; in the ten rejections, `algorithm` alone; and
`capture.json`. What did not: the `_time` axes, the initializer's account
(`initializer` and the trace's segments, the prefix iteration counts among
them: the initializer's fits are untouched), `input`, `accuracy`, `rows`,
every outcome and every rejection's reason.

The capture of 2026-10-06 (the fused accuracies, phase 1), at `9a3ccc0`:
the reconstruction composes the position and velocity blocks of the sample
covariance beside the attitude's, the output stage publishes their upper
triangles, widened by the square of the widening factor, as twelve channels
after the four accuracies (`posCovNN` ... `velCovDD`), and the algorithm
string changed to `batch-temperature-bias-v10` (`docs/SENSOR_FUSION.md`
sections 4 and 7), which changes the record's shape and no number of the
fit. Before the capture, the capture tool of this build wrote into a scratch
directory, against the previous capture: the twenty-one existing columns of
the four channels files byte-identical, every `.json` file byte-identical
once its `algorithm` is set back to `v9` (the trace, the progress texts, the
rows and every diagnostics key included; the ten rejections changed in
`algorithm` alone), and the `_time` axes identical (step 3). What changed:
each channels file's column line lists the thirty-three names under the same
`v1` header, the twelve covariance entries added after `accDAcc`; `algorithm`
in all fourteen `.json`; and `capture.json`. What did not: every number
there was, every text, the `accuracy` account, the outcomes and the time axes
(`coarse_linear` 160 lines, `coarse_maneuver` 540, `stationary_spin` 995,
`bridged_hole` 540).

### Fixtures

Generated in code by `tests/fusion/fusionfixtures.cpp`, which uses Qt Core and
the standard library only, includes nothing from `src/`, and is compiled once,
into `flysight_fusion_test_support`, for the tests and for
`fusion_golden_capture`, so the goldens and the tests see the same bits
(`capture.json` records the generator's hash). To be
bit-identical with any IEEE-754 compiler the generators use only `+ - * /` and
`std::round` (exact by definition), compute every sample from its index, and
draw noise from SplitMix64 mapped to [-1, 1) in one fixed order (all GNSS
fixes in time order, each drawing north, east, down, velN, velE, velD; then
all IMU samples, each drawing ax, ay, az, wx, wy, wz). All times are UTC
seconds, `1700000000.0 + t`. The local origin attributes are 45, -75, 100
(they appear in the diagnostics only). Every fixture states its
configuration (`FusionFixture::accelFsG`, `gyroFsDegS`, `accelOdrHz`,
`gyroOdrHz`, which `toChannels()` copies and `sessionFromFixture()` stores):
+/-16 g, +/-2000 deg/s and the listed rate nearest its IMU sampling, within
4 %; and after the noise its readings are rounded onto that configuration's
lattice, `round(v / s) * s` with `s = 16 / 32768 x 9.80665` m/s^2 for the
accelerometer and `0.070` deg/s for the gyro, as a FlySight's readings are
counts times the step. The rounding draws nothing, so every GNSS value is
what it was before it.

| Fixture | Content | Exercises |
|---|---|---|
| `coarse_linear` | The exact constant-velocity case. IMU `t = i*.01`, `i = 0..200`, force `(0, 0, -9.80665)`, rate 0. GNSS `t = .037 + i*.2`, `i = 0..8`, position `(7, 8, 9) + t*(12, -4, 2)`, velocity `(12, -4, 2)`, `hAcc = vAcc = 1`, `sAcc = .1`. Origin index 0. No noise. States 104 Hz; its readings (0 and -2048 counts) are on the lattice already | a window shorter than one segment (one segment, the 60 s prefix covers it), an exactly unobservable yaw (the four prefix starts tie and the first wins), near-zero objective, boundary timing; 9 states, 160 output samples; zero signal change and no rotation, so the sampling term and the remainder are identically zero and this golden's numbers are the densities' alone; a constant 25 degC IMU temperature (`model.gyro_bias`: `b1` at its prior, `t_ref_degc` 25) |
| `coarse_maneuver` | 6 s. IMU `i = 0..600` at `.01`. GNSS `t = -.163 + j*.2`, `j = 0..32` (one fix before and two after IMU coverage). Origin index 3; `hAcc = 12` for `j < 3`, else `1.5`; `vAcc = 2.5`, `sAcc = .3`. NED acceleration `(1.5 - .4t, .8t, -.6 + .1t^2)`, `v(0) = (20, -5, 3)`, `p(0) = 0`, velocity and position its exact polynomial integrals; attitude identity, so force `= a - (0, 0, 9.80665) + (.05, -.03, .08)` and rate `= (.2, -.15, .3)` deg/s. Uniform noise: force `.02`, rate `.05` deg/s, position `.3`, velocity `.1`; seed `0x8F050002`. States 104 Hz; readings rounded onto the lattice | trimming of fixes outside IMU coverage, a fit that starts after the first fix, the full fit started from the segment fit's solution, non-zero biases and residuals; 28 states, 540 output samples; the sampling term on a noisy 100 Hz stream, where it reads the noise as curvature; a constant 25 degC IMU temperature (`model.gyro_bias`: `b1` at its prior, `t_ref_degc` 25) |
| `stationary_spin` | 40 s. IMU `i = 0..1000` at `.04` (25 Hz keeps the golden small). GNSS `t = .1 + j*.2`, `j = 0..199`. Position and velocity zero. Force `(0, 0, -9.80665) + (.03, -.02, .05)`; rate `(.2, -.1, .15)` deg/s plus 90 deg/s on `wz` for `t >= 31`. `hAcc = 1`, `vAcc = 1.5`, `sAcc = .1`. Noise: force `.005`, rate `.02`, position `.2`, velocity `.03`; seed `0x8F050003`. States 26 Hz; readings rounded onto the lattice | one segment at rest whose prefix grows from 60 s to 120 s to cover it (yaw unobservable and arbitrary; the anchor is the first fix, every sAcc being equal), yaw through more than two turns (unwrap); 200 states, 995 output samples; the sampling term and the remainder at 25 Hz, including the 90 deg/s step at 31 s and the turn after it; a constant 25 degC IMU temperature (`model.gyro_bias`: `b1` at its prior, `t_ref_degc` 25) |
| `bridged_hole` | `coarse_maneuver` with GNSS fixes `j = 12..23` removed: a hole of 2.6 s in the fixes, from the fix at 2.037 s to the one at 4.637 s (2.2 s and 4.8 s since the epoch), with the IMU continuous through it. Everything else is `coarse_maneuver`'s | a hole in the fixes bridged by the one IMU factor that spans it: 16 states (9 before the hole, 7 after), 540 output samples, 260 of them inside the hole, each with its state and its accuracies; `input.gnss_holes` one entry (`start_s` 2.2, `length_s` 2.6); against the generating trajectory inside the hole (`tst_fusion_kernel::bridgedHoleFollowsTheTruth`) |

Rejections, each one mutation, with the reason the kernel gives:

| Fixture | Base and mutation | Reason |
|---|---|---|
| `reject_nonfinite` | `coarse_linear`, `ax[10] = NaN` | Nonfinite IMU/ax |
| `reject_time_order` | `coarse_linear`, `imuTime[4] = imuTime[3]` | Timestamps must be finite and strictly increasing |
| `reject_too_few_fixes` | `coarse_linear`, GNSS channels truncated to 2 | Sensor fusion needs GNSS, IMU and shared UTC time conversion |
| `reject_coverage` | `coarse_linear`, IMU channels truncated to the first 36 samples | Fewer than three GNSS fixes in IMU coverage |
| `reject_imu_gap` | `coarse_linear`, IMU samples 40..49 removed (.11 s > 1.5 x .01 s, the continuity rule) | IMU gap at 0.353000 s; fusion unavailable across missing data |
| `reject_sigma` | `coarse_linear`, `sAcc[0] = 0` | GNSS measurement sigmas must be positive |
| `reject_length` | `coarse_linear`, last `velE` sample removed | Missing or mismatched Local/velE |
| `reject_origin` | `coarse_linear`, `originIndex = 9` | Local origin index outside GNSS samples |
| `reject_lattice` | `coarse_maneuver`'s generator with the accelerometer rounded onto the +/-8 g lattice, +/-16 g stated | ACCEL_FS_G states +/-16 g but the accelerometer readings lie on the +/-8 g lattice; sensor fusion unavailable |
| `reject_rate` | `coarse_linear` stating 12.5 Hz for both rates (a file without keys from a firmware that logs faster than the default) | ACCEL_ODR_HZ states 12.5 Hz but the IMU is logged at 100.0 Hz; sensor fusion unavailable |

There is no temperature rejection fixture: every one of the fourteen carries the
channel (a constant 25 degC, `kFixtureTemperatureDegC`, exactly representable
and noise-free), and the missing or malformed temperature is asserted in
`tst_fusion_kernel::validationRejectsEachDefect` on mutated channels of
`coarse_maneuver` (reasons `Missing or mismatched IMU/temperature` and
`Nonfinite IMU/temperature`, reported after every other channel's defect),
without a golden, and on the engine side in
`tst_fusion_session::missingInputsAreNotApplicable`.

**Initializer recordings (not goldens).** Six more synthetic recordings: four
for the initializer's tests of the specification's section 10,
`scale_recording` for the scale state's and `long_hole` for the GNSS holes'. `initializerFixture(name)`
returns them (beside `fusionFixtures()`, which never lists them, so
`fusion_golden_capture` never sees them); their expected values live in
`tst_fusion_kernel`, stated from their construction, not in goldens. Rotation
constants are exact rationals (Pythagorean `.6 / .8` and `.96 / .28`), the
body force is `R^T (a - g) + b_a`, and the gyro reads its bias only (the
attitude is constant in every recording); the same SplitMix64 noise rules
apply.

| Recording | Content | Exercises |
|---|---|---|
| `motion_start` | 90 s that start in motion. GNSS 5 Hz, `t = j * .2`, `j = 0..449`; IMU 25 Hz, `t = i * .04`, `i = 0..2250`. Attitude `Rz(psi)`, `cos psi = .6`, `sin psi = .8` (53.13 deg). `vN = 20`, `pN = 20 t`; `aE = 2` for `40 <= t < 50`, else 0; `vE` and `pE` its exact integrals; down zero. Body force `(.8 aE + .05, .6 aE - .03, -9.80665 + .08)`; gyro `(.2, -.15, .3)` deg/s. `hAcc = 1.5`, `vAcc = 2.5`, `sAcc = .3` (every fix: the anchor is the first). Noise force `.005`, gyro `.02` deg/s, position `.2`, velocity `.03`; seed `0x8F050004`. 450 states. Temperature 25 degC. States 26 Hz; readings rounded onto the lattice | test 1, "starts in motion" (`startsInMotionGrowsToTheManoeuvre`): the 60 s window `[0, 30]` has no yaw information, the 120 s window `[0, 60]` holds the manoeuvre, so the prefix grows once (`prefix_fits` 8) and stops `observable`; the full fit within 2 degrees of the truth. Also the growth of a prefix whose starts all fail (`allPrefixFitsFailFallsBack`: 60, 120, 240 s, twelve failed starts) |
| `rest_throughout` | 300 s at rest, tilted. IMU 25 Hz, `i = 0..7500`; GNSS `t = .1 + j * .2`, `j = 0..1499`; position and velocity zero. Attitude `Ry(theta)`, `cos theta = .96`, `sin theta = .28`; body force `(.28 * 9.80665 + .03, -.02, -.96 * 9.80665 + .05)`; gyro `(.2, -.1, .15)` deg/s. `hAcc = 1`, `vAcc = 1.5`, `sAcc = .1`. States 26 Hz. IMU noise at the level the datasheet gives a resting unit, amplitudes the per-sample sigmas of the stated configuration (force `.0041278` m/s^2, gyro `.022982` deg/s), readings rounded onto the lattice; position `.2`, velocity `.03`; seed `0x8F050005`. 1500 states. Temperature 25 degC | test 2, "at rest throughout, longer than two doublings" (`atRestPrefixStopsGrowing`): neither the 60 s nor the 120 s window has yaw information (yaw sigmas 180 and 151 degrees), the second sigma is not 20 % below the first, so growth stops at 120 s (`no_gain`); the full fit converges (`settled`) with roll and pitch within 0.5 degrees; the yaw is arbitrary and logged. Under a damping ceiling of 1e5, from its coarse start, the full fit is the solver failure `damping saturated` (`dampingSaturationIsASolverFailure`) |
| `sacc_anchor` | 300 s at 15 m/s north with a 3 m/s^2 east manoeuvre from `t = 190` to `200` s. GNSS 1 Hz, `t = j`, `j = 0..300`; IMU 12.5 Hz, `t = i / 12.5`, `i = 0..3750` (exact at the integer manoeuvre bounds). Attitude identity. Body force `(.05, aE - .03, -9.80665 + .08)`; gyro `(.2, -.15, .3)` deg/s. `hAcc = 1.5`, `vAcc = 2.5`; `sAcc = 2` for every fix except `j == 200`, where it is `.3`. Noise `.005, .02, .2, .03`; seed `0x8F050006`. 301 states. Temperature 25 degC. States 12.5 Hz; readings rounded onto the lattice | test 3, the sAcc anchor (`smallestSaccFixIsTheAnchor`): the fix at 200 s is the anchor, the first prefix is the unclipped window 170-230 s (61 fixes) and contains the manoeuvre (`prefix_fits` 4, observable at 60 s), and the segment's start attitude is the prefix fit's carried back by the gyro with the prefix fit's bias |
| `drifting_bias` | 200 s at 15 m/s north with an east manoeuvre in every 30 s block (`aE = 2` for `10 <= s < 15`, `-2` for `15 <= s < 20`, `s` the time within the block) and a gyro z bias that drifts linearly by exactly 1 deg/s over the length while the attitude does not rotate. GNSS 1 Hz, `j = 0..200`; IMU 12.5 Hz, `t = i / 12.5`, `i = 0..2500` (the block of a sample `i / 375`). Attitude identity. Body force `(.05, aE - .03, -9.80665 + .08)`; gyro `(.2, -.15, .3 + t / 200)` deg/s. `hAcc = 1.5`, `vAcc = 2.5`, `sAcc = .3`. Noise `.005, .02, .2, .03`; seed `0x8F050007`. 201 states. The temperature ramps `25 + t / 10` degC (25 to 45 over 200 s), so the drift is `b1 = (0, 0, 0.05 deg/s per degC)` by construction, `T_ref = 35` (the mean of the ramp) and `b0 = (.2, -.15, .8)` deg/s, the bias at `T_ref`. States 12.5 Hz; readings rounded onto the lattice | test 4, the drifting bias (`driftingBiasSegmentsConverge`, under `segmentLength = 60`, `minFinalSegment = 12`: four segments of 60, 60, 60 and 21 fixes, each with its manoeuvre inside the anchor's 30 s half-window): every segment fit converges on a constant bias; the full fit with the temperature model recovers `b1` within 20 % in at most 30 iterations of the full fit (12: 7 in the held stage and 5 in the released stage, kept), `t_ref_degc` 35, `b0` at `T_ref`, the three priors last. The constant-temperature variant of test 9 (`constantTemperatureKeepsSlopeAtPrior`: 2501 values of 35.0) is made in the test, not in the generator; `tst_fusion_session::temperatureReachesTheKernel` runs the recording through a session |
| `scale_recording` | 60 s level, heading north, not rotating, with the accelerometer's x axis 2 % high. GNSS 5 Hz, `t = .1 + j * .2`, `j = 0..299`; IMU 25 Hz, `t = i * .04`, `i = 0..1500`. North acceleration `a = A c x (1 - x)(1 - 2x)`, `A = 5` m/s^2, `c = 10`, `x = tau / P`, `P = 10` s, periodic with zero mean (peak 4.8 m/s^2); the period index and phase by integer arithmetic (`k = i / 250`, `tau = (i - 250 k) * .04`; for GNSS `k = (2 j + 1) / 100`, `tau = .1 + (j - 50 k) * .2`); `vN = 10 + A P c x^2 (1 - x)^2 / 2`, `pN = k (10 P + A P^2 c / 60) + 10 tau + A P^2 c (x^3/3 - x^4/2 + x^5/5) / 2`; east and down zero. Body force `(1.02 (a + .05), -.03, -9.80665 + .08)`; gyro `(.2, -.15, .3)` deg/s. `hAcc = 1`, `vAcc = 1.5`, `sAcc = .1`. Noise `.005, .02, .2, .03`; seed `0x8F050009`. 300 states. Temperature 25 degC. States 26 Hz; readings rounded onto the lattice | clause 54 and item 1408, the scale (`scaleRecordingRecoversTheFactor`): one segment; the full fit's held stage settles in one pass of 3 iterations (objective 198.4), and its released stage, kept, moves `s_ax` to 1.0197 (within 0.01 of 1.02), the other factors staying at one, and the objective to 19.8, strictly below the held stage's as the test asserts (the ratio 0.100, logged), in three passes of 3, 2 and 2 iterations: the graph rebuilt after the second pass still moves the cost, and the third pass's rebuild agrees (under a budget of two passes the release ended `bias not settled` and was discarded). The forced fallback and the released stage's divergence (`releaseFailureFallsBackToTheHeldFit`, `divergenceEndsTheReleasedStage`) use it because its release moves a factor |
| `long_hole` | `longHole(30)` of the generator, which takes the hole's length in whole seconds: 120 s with a 30 s hole in the fixes and the IMU continuous. GNSS 5 Hz, `t = .1 + j * .2`, `j = 0..599`, the fixes `j = 150 .. 148 + 5 L` removed for a hole of `L` seconds (their noise still drawn, so every kept sample is the same bits whatever the hole): at 30 s the fixes `j = 150..298`, the hole running from the fix at 29.9 s to the one at 59.9 s (29.8 s and 59.8 s since the epoch), 150 fixes before it and 301 after; IMU 100 Hz, `t = i * .01`, `i = 0..12000`. Attitude identity. North acceleration of `scale_recording`'s form with `A = 2` m/s^2, `c = 10`, `P = 10` s (peak 1.92 m/s^2), the period index and phase by integer arithmetic (`k = i / 1000`, `tau = (i - 1000 k) * .01`; for GNSS `k = (2 j + 1) / 100`, `tau = .1 + (j - 50 k) * .2`), `vN = 20 + A P c x^2 (1 - x)^2 / 2`, `pN = k (20 P + A P^2 c / 60) + 20 tau + A P^2 c (x^3/3 - x^4/2 + x^5/5) / 2`; east and down constant, `vE = -5`, `vD = 3` m/s. Body force `(a + .05, -.03, -9.80665 + .08)`; gyro `(.2, -.15, .3)` deg/s (`coarse_maneuver`'s biases). `hAcc = 1.5`, `vAcc = 2.5`, `sAcc = .3`. `coarse_maneuver`'s noise: force `.02`, gyro `.05` deg/s, position `.3`, velocity `.1`; seed `0x8F05000A`. `601 - 5 L` states, 451 at 30 s. Temperature 25 degC. States 104 Hz; readings rounded onto the lattice | the long hole at the cap (`longHoleConverges`): the 30 s hole, the longest the fit bridges, fitted under the production tuning, its held stage settled in three passes of 5, 4 and 3 iterations and its released stage, kept, in two of 2 and 2, one IMU factor across it, the accuracies inside the hole and the residuals at the fix after it logged; `input.gnss_holes` one entry of 30 s (`gnssHolesInTheAudit`); the same generator with a 60 s and a 31 s hole rejected by the cap, a 30 s one passing the plan (`holeAboveTheCapIsRejected`), those two not listed by `initializerFixture()`; under `segmentLength = 29.57`, `minFinalSegment = 12`, the cut's second piece holds the two fixes at 29.6 and 29.8 s, which end the first segment: three segments, the first ending at the fix before the hole and the second starting at the fix after it (`sparsePiecesAreMerged`). The periodic manoeuvre puts horizontal acceleration in every window, so yaw is observable on both sides of the hole |

The other five recordings carry the constant 25 degC of every
golden fixture: the temperature is a required input, so none is
temperature-free. `sacc_anchor` and `drifting_bias` logged at 10 Hz until the
documented noise model: no listed rate is within 10 % of it, so they were
resampled at 12.5 Hz; neither is a golden.

### Files in `tests/data/fusion/`

- `<fixture>.json` (all fourteen; `QJsonDocument::Indented`, so every double is
  in shortest round-trip form and therefore exact): `fixture`; `outcome`
  (`"succeeded"` or `"rejected"`); `diagnostics`, the kernel's diagnostics
  object (the key list of `docs/SENSOR_FUSION.md` section 4: the input audit,
  the configuration, the segment account, objective, biases, residuals,
  `stopping`, `quality`, `model`, `accuracy`; for a rejection it is
  `{"algorithm", "failure"}`); `progress`, the kernel's progress texts in
  order (`Starting fit`, `Integrating IMU factors`, `Pass N, iteration M`;
  empty for rejections, since nothing is reported before the fit starts); and
  for successes `trace` (`initializer` with `segment_length_s` and per
  segment its bounds, anchor, prefix window, yaw sigma, fit counts,
  `converged`, `fallback`, start quaternion and biases; `converged`;
  `history` as `[pass, iteration, cost before, cost after]` rows), `rows` and
  `channels_file`.
- `<fixture>.channels.txt` (the four successes): line 1
  `# flysight fusion golden channels v1`; line 2 `# columns: _time north east
  down velN velE velD accN accE accD roll pitch yaw qx qy qz qw headingAcc
  tiltAcc accHAcc accDAcc posCovNN posCovNE posCovND posCovEE posCovED posCovDD
  velCovNN velCovNE velCovND velCovEE velCovED velCovDD` (roll, pitch and yaw
  are the **unwrapped** values; the four accuracies and the twelve covariance
  entries are the published, widened ones); then one line per
  output sample: thirty-three space-separated 16-digit upper-case hexadecimal IEEE-754 bit
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
  `sha256_lf`, the SHA-256 of each of the eighteen fixture files as written
  (LF line endings; `sha256_note`); and `fixture_generator_sha256_lf`, the
  SHA-256 of `tests/fusion/fusionfixtures.cpp` and `.h` with CRLF normalized
  to LF (`fixture_generator_note`: the goldens are valid for those inputs
  only).

  Revision convention: the capture happens before the phase's commit exists,
  so `repository_revision` is the parent of the commit that contains the
  goldens, and that commit's own changes are what the kernel was at capture
  time. The commit that holds the goldens is
  `git log -1 -- tests/data/fusion/capture.json`.

About 1.0 MB in total.

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
  `tst_fusion_session_exact`, `tst_fusion_jobs_exact`,
  `tst_fusion_rows_exact` and `tst_fusion_runner_exact` (label `exact`) are
  the ordinary fusion executables
  run again with `FLYSIGHT_FUSION_EXACT=1`, so on the capture configuration an
  ordinary `ctest -C Release` fails on the first bit that differs. With
  `FLYSIGHT_FUSION_EXACT_TESTS=AUTO` (the default) `tests/CMakeLists.txt`
  registers them only when the compiler is 64-bit MSVC at exactly
  `compiler.cl_version` in `capture.json` (19.44.35220.0; the file is read
  at configure time, so a re-capture moves the gate with it). The gate is
  the whole compiler version because code generation and the runtime's math
  decide the last bit: a Visual Studio patch, 19.44.35229 on the CI image of
  2026-10-04, moved one yaw sample of `stationary_spin` by one ulp. It
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
  under `stopping`, and under `accuracy` the kernel's two constants and its
  two counts, `heading_prior_sigma_rad`, `widening_half_width_s`,
  `widened_samples` and `undetermined_heading_samples`) and the initializer
  account's counts, fix times and
  lengths (`index`, `prefix_fits`, `start_s`, `end_s`, `anchor_s`,
  `anchor_sacc_m_s`, `prefix_start_s`, `prefix_end_s`, `prefix_length_s`,
  `segment_length_s`, `fallback_segments`), and every string, bool and null; for every other number
  `|got - golden| <= floor + 1e-7 * |golden|`, where the floor is `1e-7` in
  the solver's own units (m, m/s, m/s^2, rad, rad/s) and therefore
  `1e-7 * 180/pi = 5.73e-6` for a number expressed in degrees (`roll`,
  `pitch`, `tiltAcc`, and JSON keys ending in `_deg`), except for the heading:
  `yaw`, `headingAcc` (on a recording whose heading the data barely
  determine, the sigma of that flat direction), the quaternion channels and
  JSON keys ending in `_quaternion_xyzw` take
  `4e-6` rad in their unit (`2.29e-4` degrees of yaw, `2e-6` of a unit
  quaternion component), for the reason recorded below
  (`kPortableAbsolute`, `kPortableAbsoluteDegrees`, `kPortableAbsoluteHeading`,
  `kPortableAbsoluteQuaternion`, `kPortableRelative`,
  `portableFloor()` and `withinPortableBound()` in
  `tests/fusion/fusiongolden.h`; the same bound serves the channels, the
  diagnostics and the fit trace). Other platforms differ legitimately in the
  last bits (a different `sin`/`cos` in the C library, fma contraction inside
  GTSAM, a GTSAM compiled by another compiler) and the solver amplifies that by
  its conditioning: the fit stops at a relative cost decrease of `1e-8`, so
  where it stops is known no better than that.

  One number is an argmax: `max_step_correction_time_s`, the middle of the step
  with the largest step correction. An argmax is not portable: on
  `coarse_maneuver` the largest correction (4.6e-4 m/s^2) leads the largest at
  another step by only 3.2e-7 m/s^2, and on `coarse_linear` every correction
  is rounding (5.6e-11 m/s^2), so a compiler that moves two corrections by
  their last bits swaps which is largest, and the time jumps by a whole step
  while every value stays within its bound. Portable mode therefore does not
  compare it; exact mode compares it bit for bit like any number (the
  midpoint of two copied times has the same bits on every IEEE platform).

  One number is a switch: `accHAcc` takes the larger horizontal principal
  value where the published horizontal acceleration falls below its
  directional accuracy (`docs/SENSOR_FUSION.md` section 4), so a sample at
  the switch can take either value on two platforms, and no floor covers
  that. In the probe of the accuracy the closest sample of a golden fixture
  lay 3.2e-4 relative from the switch (`stationary_spin`; `coarse_maneuver`
  5.9, `coarse_linear` 1), against platform differences of order 1e-7. If CI
  shows a flip anyway, the remedy is a reported sample, not a wider floor.

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
  compilers (the comparison now always covers every channel, so the
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

  The heading's floor (2026-09-30, the capture of the mid-step rotation, on
  macOS 15 Intel and Ubuntu 22.04): both platforms failed `stationary_spin`
  alone, in `yaw` by 6.6e-6 and 6.2e-6 degrees (1.1e-7 rad, a constant
  offset along the whole channel; the bound allowed 6.5e-6 and 6.2e-6) and in
  the trace's `initializer.segments[0].start_quaternion_xyzw[2]` by 3.2e-7
  and 3.0e-7 (an attitude 6.4e-7 rad off; the bound allowed 1.1e-7). Every
  other channel, fixture and number passed. That fixture's heading is
  undetermined (its `yaw_sigma_deg` is at the 180-degree cap): a flat
  direction of the fit, along which where the solver stops depends on the
  last bits, so the platforms differ there far more than on anything the
  data determines. The heading-bearing quantities (`yaw`, `qx`..`qw`, keys
  ending `_quaternion_xyzw`) therefore take a floor of `4e-6` rad in their
  unit, six times the largest difference observed; roll, pitch and every
  other number keep `1e-7`.

  What the floor means per channel: 0.1 micrometre of position; `1e-7` m/s of
  velocity; `1e-7` m/s^2 of acceleration, a hundred-millionth of g; 5.7e-6
  degrees of roll or pitch; and for the heading, 2.3e-4 degrees of yaw or, for
  a unit quaternion component (bound `2.1e-6` at magnitude one), a rotation of
  about 2.4e-4 degrees. All of that is
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
ctest --test-dir <tree>/FlySightViewer-build -C Release -L exact --output-on-failure
# where the exact tests are not registered (another compiler), by hand:
$env:FLYSIGHT_FUSION_EXACT = "1"; ctest --test-dir <tree>/FlySightViewer-build -C Release -R "tst_fusion_(golden|kernel|session|jobs|rows|runner)$" --output-on-failure
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
repository root, on the capture machine (64-bit MSVC 19.44, Release), in a
tree `<tree>` configured as section 2 describes (the superbuild with the tests
on and the third-party builds off), which links the dependency installs under
`third-party/*-install`:

```bash
# 1. Build the kernel, the tests and the tool (Release).
cmake --build <tree> --config Release

# 2. Capture into tests/data/fusion/ (the default output directory), recording HEAD.
#    PATH: Qt's bin, then the GTSAM and oneTBB install bin directories (gtsam.dll,
#    metis-gtsam.dll, cephes-gtsam.dll, tbb12.dll, tbbmalloc.dll). No other solver on PATH.
PATH="/c/Qt/6.9.3/msvc2022_64/bin:$PWD/third-party/GTSAM-install/bin:$PWD/third-party/oneTBB-install/bin:$PATH" \
  <tree>/FlySightViewer-build/Release/fusion_golden_capture.exe --revision "$(git rev-parse HEAD)"
#    Check: fourteen lines, one per fixture, in fixture order; the four success fixtures
#    "succeeded", the ten reject_* "rejected (<reason>)"; no "** UNEXPECTED **";
#    then "wrote 19 files to .../tests/data/fusion"; exit status 0. In the written
#    stationary_spin.json, initializer.segments[0].prefix_length_s is 120; in
#    capture.json, solver.gtsam_dir is under third-party/GTSAM-install.

# 3. For a capture that must not move the time axis (every capture so far): the first
#    field of every data line of each success fixture's channels file, against HEAD's
#    (a capture that adds a success fixture checks the ones HEAD has).
for f in coarse_linear coarse_maneuver stationary_spin bridged_hole; do
  cmp <(git show HEAD:tests/data/fusion/$f.channels.txt | tr -d '\r' | awk 'NR > 2 { print $1 }') \
      <(tr -d '\r' < tests/data/fusion/$f.channels.txt | awk 'NR > 2 { print $1 }') \
    && echo "$f: _time identical"
done
#    Check: four "identical" lines. A difference is a defect of the change, not a
#    finding to record.

# 4. Reconfigure the application build: the exact-test gate reads capture.json at
#    configure time (file(READ) is not a dependency, so this step is explicit).
cmake <tree>/FlySightViewer-build
#    Check: the log says "Fusion exact tests registered for Release (... MSVC 19.44.35220.0)".

# 5. Run the fusion tests in both modes.
ctest --test-dir <tree>/FlySightViewer-build -C Release -L fusion --output-on-failure
ctest --test-dir <tree>/FlySightViewer-build -C Release -L exact --output-on-failure
#    Check: all green. -L fusion covers the portable comparison and (on this machine) the
#    exact runs; -L exact alone is the explicit bit-identity proof of the rebuilt kernel.

# 6. Report the changed golden files; they are part of the phase's files.
git status --porcelain -- tests/data/fusion/
git diff --stat -- tests/data/fusion/
```

PowerShell equivalent of step 2:
`$env:PATH = "C:\Qt\6.9.3\msvc2022_64\bin;$PWD\third-party\GTSAM-install\bin;$PWD\third-party\oneTBB-install\bin;$env:PATH"`
then
`<tree>\FlySightViewer-build\Release\fusion_golden_capture.exe --revision (git rev-parse HEAD)`.

What changes, and what it means:

- `capture.json` changes on every capture (date, revision, hashes).
- A `<fixture>.json` / `.channels.txt` changes when the phase changed that
  fixture's numbers or texts. A phase that changes the `algorithm` string
  changes all fourteen `.json` files; the ten `reject_*.json` otherwise
  change only when a rejection reason changes. The `algorithm` string is
  `batch-temperature-bias-v10` since the fit publishes the position and
  velocity covariance blocks (`v9` was the scale factors released from the
  held solution, `v8` the GNSS holes bridged by the IMU, `v7` the lattice
  rule, `v6` the documented noise model); a capture that prints another
  string is from a stale build.
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

`tst_fusion_session`, `tst_fusion_derived`, `tst_fusion_jobs`,
`tst_fusion_rows` and `tst_fusion_store` test sensor fusion as a registered
calculation, on real sessions. `tests/fusion/fusionsessions.h`
(`flysight_fusion_session_support`) turns a fixture into a `SessionData` whose
eighteen measurements, four origin attributes and four configuration
attributes are the fixture's (the configuration stored as the header text
`QString::number` makes of the fixture's values: the stated path), so that
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
(GNSS, IMU at 12.5 Hz, TIME; nothing under `Local`, no stored origin, no
stored fit, no configuration key), for the real input chain and the default
path, its readings on the default's lattices; its expectations are
structural, not golden.
`syntheticFitSession()` is the opposite premise to the fixture sessions: the
fit's outputs, not its inputs, stored as source data `Fusion/<name>` (the
thirty-three names only, each with its output's unit text, which the conversion
layer passes through), so that what is derived from them can be tested
without the solver (`tst_fusion_derived`); its attitude tests store `qx..qw`
(and `_time` for a golden's quaternion) this way, with a GNSS track for the
course reference. A reader of a stored output gets
exactly its samples and resolution never reaches the fit, since the engine
never falls through from source data to a derived candidate. Such a session
carries only the identity attributes of the fixture sessions, the test stores
what a derivation reads, and it never goes into a `SessionModel`, whose saver
would write the stored outputs into a session file. The header
also holds what those tests share: `fixtureSession()` (a fixture session with
what the Heading, Pitch and Roll plots read besides the fit: an exit marker
inside the fit, `kFixtureExitTime`, the course reference's default, and the
fixture's velocity stored again as `GNSS/velN` and `velE`, the GNSS course
their heading is referenced to; the fit reads neither), `fusionKey()`,
`goldenDifference()` (a session's published channels against a golden) and
`addSessions()` (sessions into an empty `SessionModel` as the application adds
them, waited for until idle; empty text on success) and
`fixtureSessionWithSAccStoredAs()` (a fixture session whose `GNSS/sAcc` source
data is stored under another name, so that only a registered calculation can
provide `GNSS/sAcc`: `tst_fusion_store`'s lookup test). The Orientation
column's tests in `tst_fusion_derived` use `ChoiceFixture` (section 8) on
`fixtureSession()` sessions, which may enter a `SessionModel`, never on
`syntheticFitSession()` ones.
`registerFusionOnce()` registers the fusion calculations on the global registry
after `TestEnvironment::registerBuiltIns()`, as the application does;
`TestEnvironment` itself stays GTSAM-free. The real fit is never run on
`asyncdriver.h`'s thread modes (default stacks): `ComputeMode::Inline` or the
executor's worker.

### Real recordings

Real recordings are not in the repository. The comparison against one,
documented in the branch's `docs/PORT_VALIDATION.md` (recording 17-26-24:
objective 65602.22485051976, 9247 GNSS states, 24411 outputs), is an optional
local check and not a CI test: `tst_fusion_jobs::realRecordingCheck`. It skips
unless the environment variable `FLYSIGHT_FUSION_RECORDING` names a folder that
contains `TRACK.CSV` and `SENSOR.CSV`; then it imports both through the
application's import path into the test's temporary logbook, requests the fit
through the executor, waits up to 30 minutes, requires a Succeeded job without
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
(`docs/DATA_SCHEMA.md` section 4). The branch's objective was reproduced on a
copy of the folder whose `SENSOR.CSV` had `$VAR,SCHEMA_VER,2` added after the
`$FLYS,1` line; since the documented noise model such a copy is rejected,
because read literally its gyro readings lie on no range's lattice
(`docs/SENSOR_FUSION.md` section 6), so the branch's objective can no longer
be reproduced. With the unmodified legacy file the counts (9247 GNSS states,
24411 outputs) still hold and the objective is not the branch's: the model
changed. The
four reference recordings of the specification, with their expected
objectives, are in section 12.2.

## 12. Manual verification

Fifteen scripts. Each step opens with its bold id and, in parentheses, the items
of `tests/acceptance_map.txt` it is evidence for; the map cites the ids
(`manual M<k>`) and `audit_cleanup` checks that they exist here.

### 12.1 Plot-driven jobs

What no automated test can reach: the real `MainWindow` start-up and profile
paths (sensor-fusion-jobs acceptance 16), quitting with a job running and one
chosen next (17), and interactivity during a fit (19), plus what the plot
rows and the status bar show (15).

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

**M1 Startup (116, 321, 514).** Check "Sensor fusion > Roll" with three fusable tracks visible that have no stored fit: the three fits start one after another with no other action, and the status bar shows "Computing results: 0 / 3". As soon as the first track has published, uncheck Roll: nothing is in demand any more, so the status bar goes empty at once; the running fit (the second track's) is no longer counted but continues (CPU busy) to its end, its record file appears in `cache/`, no third fit starts, and the CPU then goes idle. Quit and restart. After the restart the row is still checked. Track visibility is not kept across restarts, so no track is visible, the status bar is empty and no job starts (CPU idle). Show the three tracks: the first two tracks' roll is drawn at once (their stored results), and the third track's fit starts by itself: the status bar shows "Computing results: 0 / 1" (its hover names the recording and its step), then goes empty. The plot row looks as any other throughout. The debug output contains no "No data available" line for the fusion plot.

**M2 Profile (116, 514, 515, 556).** Save a profile that checks Roll. Uncheck Roll, and show two fusable tracks without stored fits: nothing starts. Apply the profile: Roll is checked and the two fits start at once, one after the other, exactly as after a click on the check box (this is the real `applyProfile()` path, which no automated test can construct). Uncheck Roll and hide every track, apply the profile again: nothing starts (no track is visible); show one fusable track without a stored fit: its fit starts at once. In Manage Profiles restore the default profiles and apply each of them in turn: none adds a logbook column over a "Sensor fusion" value: while every track is hidden, the status bar never shows "Computing results" and no fit starts. (The status bar may show "Computing columns" for the cheap columns of a profile; "Computing results" would mean a column over a requested output.) (The Plots menu and its shortcuts list a fixed set of GNSS plots; no fusion plot can be toggled from there.)

**M3 Computing in the status bar (115, 501, 535).** With three fusable tracks without stored fits visible, check Roll: the status bar shows "Computing results: 0 / 3" and its bar; the plot row looks as any other, with no glyph and no number; Heading and Pitch, if checked, add nothing to the count (one computation per track serves all three). The hover of the label reads "Computing results: 0 / 3" and, under it, "<name>: <progress text>". As each fit publishes, its graph appears without any further action, the count advances ("1 / 3", "2 / 3"), and finally the status bar goes empty. The legend and any fusion logbook column fill in at the same moments.

**M4 No controls (115, 534).** On a fresh set of tracks, uncheck and re-check a fusion plot by clicking its check box: fits start. Do the same with Space. While they run, click, right-click and double-click the status bar's "Computing results" and its bar: nothing happens, and there is no cancel button; click the plot row: it is selected as by any click, and nothing is cancelled or toggled. No context menu, menu item or shortcut offers a refresh or a cancel for a calculation.

**M5 Interactive during a fit (119).** While a fit runs: pan and zoom the plot, switch tools, hide and show other tracks, edit a session's description, set a marker, open Preferences. Nothing blocks; no dialog appears.

**M6 Hide and uncheck (115, 518, 519, 534).** With one fit running and two tracks waiting (the status bar reads "Computing results: 0 / 3"), hide one waiting track: the count advances at once ("1 / 3": the hidden track no longer counts). Uncheck the plot: nothing is in demand any more, so the status bar goes empty at once (neither the other waiting track nor the running fit is counted); the running fit continues (CPU busy) to its end, its record file appears in `cache/`, no further fit starts, and the CPU then goes idle. Check the plot again: the remaining visible track is fitted.

**M7 Fourth track and failure (115, 525, 537).** Show a fourth fusable track: it is fitted with no other action, after the running fit if one runs. Show the recording without IMU data: it is never counted and never listed. For a rejected recording, as soon as its fit is rejected the status bar shows the warning "1 session could not be computed" beside "Computing results", and alone once the other fits are done; the warning's hover and the recording's logbook row give the reason, nothing offers to try again, and no message box appears.

**M8 Remove and unload (117).** With one fit running and another track waiting, delete the waiting track's session, then the running one's: no crash, no hang, nothing published for them; the status bar's count of the computations drops them, and the status bar goes empty.

**M9 Quit (117).** With one fit running and two tracks waiting, close the window: a wait cursor for at most one solver step, then the application exits; no crash dialog, the process is gone from the task manager, and on restart the logbook is intact. Repeat with File > Exit.

Possible without the fusion plots, after any change to the plot list or to
`MainWindow`'s close path:

- Every existing plot row looks exactly as before (compare with a `master` build side by side, light and dark theme, 100% and 150% scaling): no glyph, no count, no tooltip, same row height, same elision.
- Checking and unchecking plots by mouse, by Space, through the Plots menu, by shortcut, and by applying a profile behaves as before.
- With a visible recording that lacks a sensor (for example no IMU) and an IMU plot checked, the "No data available for plot" warning still appears in the debug output.
- Quit through File > Exit and through the close button, in Debug and Release: no crash, no hang, no debugger output about destroyed objects or running threads.

There is no refresh and no cancel for a calculation anywhere: unchecking a
plot, hiding a track or removing a logbook column is how work is dropped.

### 12.2 The fusion runner and the reference recordings

**Not part of the automated tests.** The specification's reference recordings
(its section 12) are real recordings that are not in the repository, so the
runner is exercised on them by hand; the map's items 208 and 233 cite these
steps beside their automated evidence. Michael's corpus comparison (M15) is
his and not part of any acceptance.

Preamble. Build the test tree `<tree>`, configured as section 2 describes
(`cmake --build <tree> --config Release`). Git Bash, from the repository root;
the runner links `flysight_core`, hence GeographicLib, and the solver:

```bash
PATH="/c/Qt/6.9.3/msvc2022_64/bin:$PWD/third-party/GeographicLib-install/bin:$PWD/third-party/GTSAM-install/bin:$PWD/third-party/oneTBB-install/bin:$PATH"
R=<tree>/FlySightViewer-build/Release/fusion_runner.exe
mkdir -p TEMP/runs
```

(The PowerShell form of the `PATH` is as in section 11's re-capture procedure,
with `third-party\GeographicLib-install\bin` added.) The recordings are under
`TEMP/data/` (untracked, on Michael's machine); the outputs go to `TEMP/runs/`
(never staged). The JSON is read with Python 3, which is on the machine (`jq`
is not assumed):

```bash
python -c "import json,sys; d=json.load(open(sys.argv[1])); print(d['stopping']['rule'], d['seeds'][0]['iterations'], round(d['objective']), d['quality'], d['model']['gyro_bias'], [(s['prefix_length_s'], s['yaw_sigma_deg'], s['iterations'], s['fallback']) for s in d['initializer']['segments']])" TEMP/runs/<name>.json
```

**M10 Runner streams and exit codes (233).** `"$R" --help; echo $?`: the usage on stdout, 64. `"$R" "TEMP/data/Data comp 5 - FS 2 - serie nr 2 - 014667 (test 10)/24-09-05/11-17-12" > TEMP/runs/11-17-12.json 2> TEMP/runs/11-17-12.log; echo $?`: stdout one JSON line; stderr the progress texts (`Starting fit`, `Integrating IMU factors`, `Segment i of n: prefix L s, pass p, iteration k`, `Segment i of n: pass p, iteration k`, `Pass p, iteration k`, `Releasing the scale factors`), then the outcome line; exit 0 for `Succeeded`. With `--csv TEMP/runs/11-17-12.csv`: a CSV whose header is the thirty-three names (the seventeen of the state, then `headingAcc`, `tiltAcc`, `accHAcc`, `accDAcc`, then the twelve covariance entries `posCovNN` ... `posCovDD`, `velCovNN` ... `velCovDD`) and whose row count equals `imu_outputs`. A folder without `SENSOR.CSV`: exit 3.

**M11 Reference recording `11-17-12` (208).** The recording of M10 (bias 1.03 deg/s after the schema correction; the previous initializer accepted no resting window and did not converge in five passes). Expected (specification section 12): converged (`settled` or `slow tail accepted`), about 12 full-fit iterations, objective near 45,000 with the density-only model; with the per-step term the objective is lower (the lab measured 26,515 with a weaker term), so the iteration count and the rule are the comparison and the objective is recorded. First segment: the unit rests for 180 s, so its prefix grows to 240 s (`prefix_length_s` 240, `yaw_sigma_deg` a few degrees). On the current build the first segment's 60 s prefix is already observable (its anchor, the smallest-sAcc fix at about 246 s, is in motion), so `prefix_length_s` is 60, not 240, and the objective is about 20,000; the specification's expectation stays the reference here until Michael decides which is. On the current build the full fit took 7 iterations (the M46 run of 2026-09-30).

**M12 Reference recording `08-35-23` (208).** `"$R" "TEMP/data/Data comp 1 - FS 2 - serie nr 2 - 01465 (test 08)/24-09-07/08-35-23" > TEMP/runs/08-35-23.json 2> TEMP/runs/08-35-23.log; echo $?`. No resting window; the previous initializer "converged" it to an objective of 14 million with a position RMS of 25 m. Expected: converged, objective about 11,000, position RMS 0.7 m (`position_residual_rms_m` in `seeds[0]`), under 40 iterations, `prefix_length_s` 60.

**M13 Reference recording `10-15-24` (208, 219).** `"$R" --dump-inputs TEMP/runs/10-15-24.in.txt "TEMP/data/Data comp 2 - FS 2 - serie nr 2 - 01086 (pers.)/24-09-07/10-15-24" > TEMP/runs/10-15-24.json 2> TEMP/runs/10-15-24.log; echo $?`. The drifting-bias unit, 45 minutes, unconverged before. Expected: converged in 10-15 iterations, objective about 80,000 with a constant bias and lower with the temperature model; five segments; `b1_rad_s_per_degc[1]` near `2.1e-3` (0.12 deg/s per degC), `t_ref_degc` in the thirties; `quality.imu_nrms` below the constant-bias value (about 1.1). In `TEMP/runs/10-15-24.in.txt` the `IMU/temperature` line holds values between 26 and 42.

**M14 Reference recording `08-35-41` (208, 219).** `"$R" "TEMP/data/Data comp 2 - FS 2 - serie nr 2 - 01086 (pers.)/24-09-07/08-35-41" > TEMP/runs/08-35-41.json 2> TEMP/runs/08-35-41.log; echo $?`. The same unit and day, 40 minutes: converged in 10-15 iterations, about 108,000 with a constant bias, the same temperature signature (`b1_rad_s_per_degc[1]` of the same sign and size as M13's).

**M15 Corpus tally (208).** Michael's experiment tooling (`experiments/fusion_lab`, untracked), not this repository: 97 recordings, one fit each; the specification expects every recording the corpus fitted worst converged in 9-17 iterations. Recorded here for completeness; no command of this repository runs it.

Pass / fail and the numbers per step (the exit code, `stopping.rule`,
`seeds[0].iterations`, `objective`, `quality`, the segments' `prefix_length_s`
/ `yaw_sigma_deg` / `iterations` / `fallback`, and for M13 and M14
`model.gyro_bias`) go in the phase report. A recording that misses its
expectation is reported with its numbers, not adjusted: the expectations are
the specification's, and whether a miss is a defect or a correction of the
specification is Michael's call.

### 12.3 Stored results

What the automated tests cannot show: the real application hiding, showing and
restarting with stored results, the files it leaves in the logbook, and the
settings changes, plugin edits and file locks that must not cost a stored fit.
Use
the preamble of 12.1 (a COPY of a logbook, never the real one), the same
recordings, and a file browser open on
`<scratch>/FlySight Viewer/logbook/`: the recordings are in `sessions/`, the
stored results in `cache/`, which the first fit creates.

**M16 Hide and show again (321, 325).** Set Preferences > Logbook > "Maximum cached sessions" to 0. With three fusable tracks visible and "Sensor fusion > Roll" checked, let the three fits finish (they start when Roll is checked). `cache/` now holds three `<uuid>.builtin%2Efusion%2Efit.fvresult` files, and `sessions/` holds no such file. Hide the three tracks; with a capacity of 0 every hidden track is unloaded at once. Show them again: roll is drawn at once for each, the status bar stays empty (nothing is counted), no job starts, and the modification times of the three files are unchanged. Set the preference back.

**M17 Restart (323, 325, 349).** Add a logbook column over a fusion value (roll at the exit marker). Every other recording of the logbook with IMU data and no stored fit is now fitted in the background too (M24); on a large logbook copy wait until the status bar no longer shows "Computing results", or use a logbook copy that holds only the script's recordings. With the three tracks of M16 fitted and the column filled, quit and start again. Before any track is shown, the column shows a number for each of the three at once, without loading them, and no fit starts. The recording without IMU data is the only one loaded for the column: its cell reads "···" and the status bar shows "Computing results" until the fill has loaded it (that the fit does not apply is remembered for a run only), then its cell is blank and the status bar goes empty. The rejected recording is not loaded: its rejection is stored, and the status bar's warning lists it. Show them: the plots draw at once, the status bar shows no computation, no job starts.

**M18 Change an input (315, 520).** Edit the description of one fitted track: its roll stays drawn and its record file keeps its modification time. Then re-import a copy of that track's `SENSOR.CSV` in which the `az` value of one `$IMU` row was changed (the header, `SESSION_ID` included, unchanged). The track's roll disappears, the status bar shows "Computing results", the record file is gone and the track's cell in the column of M17 shows "···"; about a second after the re-import a fit starts by itself, a record file appears again, roll is drawn and the cell shows the new number.

**M19 Delete (329, 330).** Delete one fitted track from the logbook: its `.csv` in `sessions/` and its `.fvresult` files in `cache/` are gone. With the application closed, copy another track's record file within `cache/` to a name whose `<uuid>` part matches no `.csv` (change one character). Start the application: the copy is gone, no extra track appears, and no dialog is shown. Quit, delete the whole `cache/` folder, and start again: every track is still in the logbook, the cells of the column of M17 show "···" and fill in again as the recordings are fitted in the background, and `cache/` reappears with the first finished fit. Remove the column, quit, delete `cache/` again and start: showing a track draws no roll until Roll is checked, which starts its fit, and no `cache/` folder appears until a fit finishes.

**M20 Unrelated changes keep a fit (404, 430).** Add the column of M17 again (M19 removed it) and let it fill. With the three tracks of M16 fitted and shown, open Preferences > Altitude Markers, add the altitude 1000 and close the dialog. Roll stays drawn for the three tracks, the status bar shows no computation, no job starts, and the three record files in `cache/` keep their modification times. In Preferences > Import set "Descent pause timeout" to 45: the same. The logbook column of M17 is recomputed, the hidden tracks' cells included: the column worker reads the stored fits of recordings that are not loaded, so every cell shows its number again and no fit starts. Quit and start again, show the three tracks: roll is drawn at once, the status bar shows no computation, no job starts, the files keep their modification times, and the column shows the three numbers. Remove the altitude and set the timeout back: the same, also after another restart.

**M21 Editing a plugin (419, 420).** Quit. Copy the repository's `python_plugins/` folder to a scratch folder, copy `examples/imu_tilt.py` from the copy into its top level, and start the application with the environment variable `FLYSIGHT_PLUGINS` set to the scratch folder. The debug output shows `[PluginHost] Plug-in code identity: plugins-sha256:` followed by 64 hex digits and `(3 files)`. Add a logbook column over `_IMU_PEAK_ACCEL` and let it fill. Quit and start again without edits (the interpreter has written `__pycache__/` into the folder): the identity is the same, and the column shows its values for hidden tracks without loading them. Quit, add a comment line to the top-level `imu_tilt.py`, start: the identity differs, the column's values are recomputed in the background and are the same numbers, and showing the three fitted tracks draws roll at once with no job, their record files keeping their modification times (the fit looks up no plugin output). Quit, undo the edit and add a comment line to `examples/imu_tilt.py` instead (never imported), start: the identity differs from both earlier ones. Remove the scratch folder and unset `FLYSIGHT_PLUGINS`.

**M22 A record that cannot be read (421, 422, 442).** Windows only. Uncheck Roll and remove the column of M17 first: while either is on, a record that cannot be read reads as not computed and would be fitted again. Quit. In PowerShell, hold one fitted track's record open without sharing: `$f = [IO.File]::Open('<scratch>\FlySight Viewer\logbook\cache\<uuid>.builtin%2Efusion%2Efit.fvresult', 'Open', 'Read', 'None')`. Start the application and show that track: nothing starts, the debug output has one line containing "skipped (kept for the next load)", the file is still in `cache/` with its modification time. Do not check Roll while the file is held. Quit, run `$f.Close()`, start again, check Roll and show the track: roll is drawn at once, the status bar shows no computation and no job starts.

Pass / fail and a note per step go in the phase report. A step that fails is
reported as it failed, not adjusted.

### 12.4 Demand-driven calculations

What the automated tests cannot show: the status bar and the pending cells in
the real views and themes, a whole-logbook fill with real fits and its memory, the application staying usable while a fill runs, and a
job-level failure tried again after a restart. Use the preamble of 12.1 (a
COPY of a logbook, never the real one) with a logbook of 20-50 recordings with
IMU data, several of them never fitted, one without IMU data and one the model
rejects; Task Manager (Details tab) and, for M27, Sysinternals Process
Explorer.

**M23 Working indicator and hover.** Superseded by M34: the plot row and the column header show nothing about computing, and the status bar carries the count and the hover.

**M24 A logbook column fills in the background (510, 529, 530, 539).** Hide every track. In the column editor add "roll at the exit marker" over Sensor fusion. At once the status bar shows "Computing results: k / n" with its bar and no cancel button (n: the recordings with IMU data and a local origin, plus those without that the fill has not loaded yet), k rising as fits finish; its hover names the running recording with its progress; the column's header looks as any other. Values replace "···" row by row, from the top. Task Manager's memory for FlySight Viewer stays flat after the first loads (at most two recordings beyond "Maximum cached sessions" are held for the fill). The recording without IMU data reads "···" and is counted until the fill has loaded it; then it turns blank and counts as done. When the fill ends, the status bar's "Computing results" goes (the warning stays if M7's rejected recording is in the logbook: its hover lists it), `index.json` in the logbook folder holds the values and no "···". While a fit runs, Task Manager shows no main-thread activity beyond the fit (the scheduler waits, it does not spin). Quit and start again: the column shows its values at once, and nothing is fitted for it. The recording without IMU data reads "···" again and is loaded once more (that the fit does not apply to it is remembered for a run only, since only fits are stored), then turns blank; no other recording is loaded for the column.

**M25 Pending cells (538).** During a fill (M24's, or, once it has finished, after deleting a few records in `cache/` with the application closed and starting again with the column enabled): cells still to come show a grey "···" (a lighter one on a selected row); blank cells are recordings without IMU data; hovering a "···" cell shows "Pending: this value is being computed". Sort by the column ascending, then descending: numbers first, "···" and blank cells together at the bottom both ways. Drag the column elsewhere, narrow it and double-click the section's right edge: the header looks and resizes as any other.

**M26 Visible first, then disabling (518, 524).** While a fill runs, check Roll and show a recording far down the list whose fit is missing: it is fitted next, right after the running fit, and its roll is drawn. Then remove the column in the column editor: the column is gone, the running fit finishes and its record appears in `cache/`, nothing else starts, and the status bar goes empty.

**M27 Background work does not get in the way (504, 540).** During a fill: pan and zoom plots, show and hide tracks, edit a description, sort and scroll the logbook: everything responds as without the fill. On Windows, in Process Explorer (FlySight Viewer > Properties > Threads) the threads that use the CPU - the executor's worker and the solver's helper threads while they help the fit - run at "Below Normal" priority, the application's other threads at "Normal". (On Linux the worker's priority is not lowered: docs/CALCULATIONS.md, section 15.5; only the responsiveness is checked there.)

**M28 A failure is tried again after a restart (526).** Windows only. This step assumes that a session file held open without sharing makes its load fail on Windows; that is not verified yet, so if the recording loads and is fitted, the step could not be carried out (record it), rather than a failure. Quit. Pick a recording with IMU data and no stored fit (delete its `.fvresult` in `cache/` if it has one) and hold its session file open without sharing: `$f = [IO.File]::Open('<scratch>\FlySight Viewer\logbook\sessions\<uuid>.csv', 'Open', 'Read', 'None')`. Start the application with the column of M24 enabled. When the fill reaches that recording its load fails: the status bar shows the warning at once, and its hover lists the recording under its description as the logbook shows it (the recording is named, not identified by its id), with "Sensor fusion: The session file could not be loaded (tried again at the next start)"; the recording's row shows the warning glyph right after the text of its first cell, with the same entry as its hover. Wait a minute: that recording is not tried again (no fit; once the fill ends the status bar shows the warning alone). Quit, run `$f.Close()`, start again: the recording is loaded and fitted in the background, its cell gets its number and the warning is gone.

Pass / fail and a note per step go in the phase report. A step that fails is
reported as it failed, not adjusted.

### 12.5 Calculation refinements

What the automated tests cannot show: the status bar switching texts in the
real window, a real record write that fails and a session file that loads
later in the run. Use the preamble of
12.1 (a COPY of a logbook, never the real one) and the recordings of 12.4.

**M29 One glyph.** Superseded by M34 (the plot row shows nothing about computing, the status bar the count and its hover) and M36 (the status bar's warning and the logbook row).

**M30 One clock.** Superseded by M34: no clock exists, and nothing turns or repaints by itself while work runs.

**M31 The fill in the status bar (539, 647).** Hide every track and add the fusion column of M24 on a logbook with recordings that are not loaded and have no stored fit: the status bar shows "Computing results: k / n", with no cancel button. While it fills, add a column that needs no computing (for example the exit time): the status bar shows "Computing columns: k / n", with its cancel button, while that column is filled, and its hover lists both; then "Computing results: k / n" returns, without a cancel button; it never shows one label with two totals. When the fill ends the status bar goes empty.

**M32 A result that cannot be stored (526, 618, 619, 620, 648).** Quit. Pick a fusable recording without a stored fit and, in `<scratch>/FlySight Viewer/logbook/cache/`, create a folder with its record file's name (`<uuid>.builtin%2Efusion%2Efit.fvresult`; on Linux or macOS, `chmod a-w cache` instead). Start, show the recording and check Roll: the fit runs once; the plot draws it, the status bar shows the warning, and its hover and the recording's logbook row list "Sensor fusion: Couldn't write file ... (tried again at the next start)". Hide and show the recording, set "Maximum cached sessions" to 0 and hide it: no second fit starts (CPU idle). Add the fusion column: the warning keeps the same entry, and the recording is not loaded or fitted for it. Quit and start again with the folder in place: the fit runs once more and fails the same way. Quit, remove the folder (or `chmod u+w cache`), start: the fit runs, its record file appears and the warning is gone.

**M33 A session file that loads later in the run (639).** Windows only, with the caveat of M28 (if the held file still loads, record that the step could not be carried out). Reproduce M28 up to the warning that lists the recording with "The session file could not be loaded". Without quitting, run `$f.Close()` and show that recording: it loads, its entry leaves the warning's hover and its row's glyph goes, its fit runs (a visible session first) and its cell gets its number; the warning goes once nothing else failed.

Pass / fail and a note per step go in the phase report. A step that fails is
reported as it failed, not adjusted.

### 12.6 One status bar for background work

What the automated tests cannot show: the status bar and the row warning in
the real window and themes, overlapping work and its cancel button, and the
warning across a real restart. Use the preamble of 12.1 (a COPY of a logbook,
never the real one) and the recordings of 12.4.

**M34 The status bar (535, 536, 644-646, 701, 703, 723, 733, 735, 736, 738).** From the start the status bar is at the bottom of the window, and empty. Show two fusable tracks without stored fits and check Roll: the status bar shows "Computing results: 0 / 2" and its bar; its hover lists "Computing results: 0 / 2" and, under it, the recording being computed with its step; there is no cancel button. The plot row looks as any other (no glyph, no number; the same height and elision as a row that needs no computing), and the second track's roll is absent from the plot until its fit publishes, without a "No data available" line in the debug output. Add a logbook column over a fusion value: its header looks as any other, and a cell still to come shows "···". The window's layout does not jump when the work starts or ends: the status bar keeps its height. When the fits end the status bar is empty and Task Manager shows FlySight Viewer at 0 % CPU (nothing repaints by itself). Repeat in the light and the dark theme.

**M35 Overlap and cancel (702, 707, 709).** During the column fill of M24, select many recordings in the logbook and change one attribute of all of them at once (the logbook's multi-row edit, a bulk edit): the status bar shows "Updating sessions: k / n" with a cancel button, and its hover lists it and "Computing results: k / n" with the recording being computed. Cancel it with the button: the bulk edit stops, and "Computing results" returns, without a cancel button. Edit a recording's description, so that it is saved: while "Saving sessions" shows, there is no cancel button.

**M36 The warning and the row (537, 711, 712, 718, 735).** During a fill that reaches a recording the model rejects and that has no stored rejection yet (delete its `.fvresult` in `cache/` with the application closed if it has one; a stored rejection shows from the start): as soon as its fit is rejected the warning "1 session could not be computed" appears beside "Computing results"; once the fill ends it stands alone. Its hover lists the recording under its name, with "Sensor fusion: <reason>". The recording's logbook row shows the warning glyph right after the text of its first cell, and hovering the glyph shows the same "Sensor fusion: <reason>"; its cell in the fusion column is blank. Move another column to the front, then hide the first column: the glyph moves to the new first cell each time. Row heights do not change, and rows without a failure look as before. Select the row: the glyph stays readable.

**M37 After a restart (714, 715, 734).** With the fusion column enabled and the rejected recording of M36 listed, quit and start again: once the logbook's background column pass has run, the warning counts the stored rejection, without loading the recording (it stays unloaded) and without a fit. Reproduce the failed write of M32: its entry says "(tried again at the next start)". Quit and start again with the folder still in place: the entry is absent until the retry runs and fails again. Remove the folder and start again: the retry succeeds, and the entry does not return.

**M38 Clearing (716, 717).** With the warning of M36 shown and Roll unchecked, disable the fusion column: the warning goes. Enable it again: the warning is back at once, and no fit runs. Click the warning: nothing changes, and nothing dismisses it. Re-import the rejected recording with changed data: the warning goes until the new fit fails again.

Pass / fail and a note per step go in the phase report. A step that fails is
reported as it failed, not adjusted.

### 12.7 Sensor fusion plots, attitude and the orientation attribute

What the automated tests cannot show: the real plot list and its colours,
real fits through turns, the logbook's editors on a real logbook with loaded
and unloaded rows, a hand-edited session file, an old profile and a column
kept from an earlier build. Use the preamble of 12.1 (a COPY of a logbook,
never the real one) and the recordings of 12.4, plus one recording with
several turns.

**M39 The fusion plots (801, 802, 803, 842).** The plot list has "Sensor fusion" with Elevation, the three fused speeds (M58), Horizontal acceleration, Vertical acceleration, Along-track acceleration, Cross-track acceleration, Heading, Pitch and Roll, in that order, then the four accuracy plots (M51), and no "GNSS (Local frame)" category. With two fusable tracks without stored fits visible, check the eight that are neither M58's speeds nor M51's accuracies (Elevation, the four accelerations, Heading, Pitch and Roll): the status bar shows "Computing results: 0 / 2", and one fit per track fills the eight. Check each GNSS counterpart as well (Elevation; the four accelerations; Course): each pair shares one axis and unit, and the two lines are told apart by colour. Pitch and Roll share the angle axis with Heading. Hide and show a track: its plots draw at once, with no computation. Check this in the light and the dark theme.

**M40 Attitude through turns (812, 814, 843).** On the recording with several turns, with the default orientation and a unit on the back of a helmet, label up: Heading shows no jump of 360 degrees at north, and reads the compass heading of the helmet's forward axis; with the Course reference marker moved, Course shifts and Heading does not. Heading turns with Course through every turn in flight; before exit and under canopy it goes where the head looks. In straight flight Roll is near zero; it is positive in a right turn and negative in a left one.

**M41 The orientation column (817, 819, 822, 823, 824, 829, 844).** The Add Column dialog, "Session Attribute", lists "Orientation" in the "Session" group; a new logbook's default columns do not include it. Add it: every row shows "forward +y, up +z", and a text editor shows no `$VAR,_ORIENTATION` line in any session file; import a new recording: the same. Double-click a cell: a drop-down list opens with the 24 labels and nothing else, and it accepts no typing. Choose "forward +x, up +z": the cell shows it; the session file gains `$VAR,_ORIENTATION,+x,+z`; with Heading, Pitch and Roll checked for that track, they redraw at once; no "Computing results" appears, and the track's record file in `cache/` keeps its modification time. Choose "forward +y, up +z": the line now says so, and the plots are back. In Preferences > Import, the Orientation list offers the same 24 and shows "forward +y, up +z"; choose "forward +x, up +z" and import a new recording: its cell shows that, and its file carries the line from the start.

**M42 Setting it for several recordings (823, 824, 826, 830, 831).** Select rows that are loaded and rows that are not, right-click, and choose "Set Orientation...". A list of the 24 labels opens, with no text box and no other entry. Choose a label: every selected row shows it, and every file has the line. Choose another: every file's line changes. Cancel: nothing changes. Then, with the application closed, edit one session file's line to `$VAR,_ORIENTATION,sideways`. Start: its cell shows "sideways" and its Heading, Pitch and Roll draw nothing. Choosing a label in the cell draws them.

**M43 An old profile (805, 845).** With the application closed, add `Fusion/qx`, `Fusion/roll` and `Local/north` to the `enabledPlots` list of a copy of a profile file in `Documents/FlySight Viewer/profiles/`, beside a GNSS plot it already lists, and note the file's modification time. Start and apply the profile: the GNSS plot is checked, and Sensor fusion > Roll is not. No message box appears, and the debug output has no line about the three ids. The file keeps its bytes and modification time.

**M44 A column kept from before (804).** Set up a logbook copy with a build from before this change, with a column over Sensor fusion > Roll at the exit marker, filled. Start this build: the column still shows its values, labelled "Fusion/roll @ <marker>", with no fit. The Add Column dialog's measurement tree lists fifteen Sensor fusion measurements and no local-frame group.

Pass / fail and a note per step go in the phase report. A step that fails is
reported as it failed, not adjusted.

### 12.8 The fused state at every IMU sample

What the automated tests cannot show: a real recording's fused plots between
fixes, the first start of this build over fits stored by an earlier one, and
the runner's account of the reconstruction on a real recording. Use the
preamble of 12.1 (a COPY of a logbook, never the real one) for M45 and the
preamble of 12.2 for M46.

**M45 The fused plots between fixes, and the first start (913, 914, 916, 922).** Use a logbook copy whose recordings have fits stored by a build from before this change, with a logbook column over a Sensor fusion measurement enabled (roll at the exit marker, say). Start this build: the status bar computes every recording with IMU data once ("Computing results: k / n"), and each recording's record file in `cache/` is written again; quit and start again: nothing is computed. With Sensor fusion > Elevation and Vertical acceleration checked beside their GNSS counterparts, zoom into a few seconds of freefall: the fused lines have a point at every IMU sample, with no straight run between fixes and no bend where two samples straddle a fix; through the parachute opening the fused vertical acceleration shows the opening at the IMU rate, and the fused elevation's slope changes where it does.

**M46 The runner's account of the reconstruction (912, 916, 917, 918, 921, 925).** M10's recording, with `--csv`: `"$R" --csv TEMP/runs/11-17-12.csv "TEMP/data/Data comp 5 - FS 2 - serie nr 2 - 014667 (test 10)/24-09-05/11-17-12" > TEMP/runs/11-17-12.json 2> TEMP/runs/11-17-12.log; echo $?`: exit 0; `algorithm` is `batch-temperature-bias-v4`; `dense_output` and `limitations` are the texts `docs/SENSOR_FUSION.md` section 4 quotes, and the diagnostics have no `display_position_velocity` key; the four numbers `max_endpoint_correction_deg`, `max_velocity_mismatch_m_s`, `max_step_correction_m_s2` and `max_step_correction_time_s` are finite, `max_step_correction_time_s` lies between `start_s` and `end_s`, and all four are recorded; the CSV's row count equals `imu_outputs`, its first `_time` less `input.epoch_utc_s` is at or after `start_s` and its last before `end_s`, and consecutive `_time` differences are the recording's IMU interval (0.075 to 0.076 s for this recording); `stopping.rule`, `seeds[0].iterations` and `objective` equal those of the same run on a build from before this change: the fit is unchanged. The numbers, in one line:

```bash
python -c "import json,csv,sys; d=json.load(open(sys.argv[1])); t=[float(r[0]) for r in list(csv.reader(open(sys.argv[2])))[1:]]; e=d['input']['epoch_utc_s']; s=[b-a for a,b in zip(t,t[1:])]; print(d['algorithm'], 'display_position_velocity' in d, [d[k] for k in ('max_endpoint_correction_deg','max_velocity_mismatch_m_s','max_step_correction_m_s2','max_step_correction_time_s','start_s','end_s','imu_outputs')], len(t), t[0]-e, t[-1]-e, min(s), max(s), d['stopping']['rule'], d['seeds'][0]['iterations'], d['objective'])" TEMP/runs/11-17-12.json TEMP/runs/11-17-12.csv
```

Pass / fail and the numbers per step go in the phase report. A step that fails
is reported as it failed, not adjusted.

### 12.9 The documented noise model and the accuracy, part 1

What the automated tests cannot show: the configuration checks, the noise
model, the scale state and the accuracy on the reference recordings, the
accuracy's cost on the longest of them, the first start of this build over
fits stored by an earlier one, the accuracy plots in the real window, and the
validation of the model on the reference recordings. Use the preamble of 12.2
for M47, M49, M50 and M52 and the preamble of 12.1 (a COPY of a logbook, never
the real one) for M48 and M51.

**M47 The configuration on the reference recordings (1008, 1040, 1051).** M11-M14's commands, each run again: each exits 0 or names its outcome. In each diagnostics, `configuration` is 16 / 2000 / 12.5 / 12.5 (the default: the recordings state no keys), and no recording is rejected by the lattice or the rate check. Record `stopping.rule`, the iterations per pass (`seeds[0].iterations`, `stopping.passes`), `objective`, `quality`, `model.noise`, the segments, and whether any fit ended `damping saturated`, which the stopping rule now reports instead of calling a stuck start converged. The numbers, in one line per recording:

```bash
python -c "import json,sys; d=json.load(open(sys.argv[1])); print(d.get('failure'), d.get('configuration'), d['stopping']['rule'], d['stopping']['passes'], d.get('seeds', [{}])[0].get('iterations'), d.get('objective'), d.get('quality'), d.get('model', {}).get('noise'), [(s['prefix_length_s'], s['yaw_sigma_deg'], s['iterations'], s['fallback']) for s in d.get('initializer', {}).get('segments', [])])" TEMP/runs/<name>.json
```

Then the lattice of each recording's readings, by Python on its `SENSOR.CSV`: the largest distance of an accelerometer reading (g x 9.80665) from the +/-16 g lattice (16 / 32768 x 9.80665 m/s^2) and of a gyro reading (deg/s x 1.14688, the legacy correction: the recordings state no `SCHEMA_VER`) from the +/-2000 deg/s lattice (0.070 deg/s), against the tolerances 9.80665e-5 m/s^2 and 1.14688e-3 deg/s:

```bash
python -c "import sys; c=None; a=[]; w=[]
for l in open(sys.argv[1]):
    p=l.strip().split(',')
    if p[0]=='\$COL' and p[1]=='IMU': c=p[2:]
    elif p[0]=='\$IMU' and c: a+=[float(p[1+c.index(k)])*9.80665 for k in ('ax','ay','az')]; w+=[float(p[1+c.index(k)])*1.14688 for k in ('wx','wy','wz')]
s=16/32768*9.80665; print(len(a)//3, max(abs(v-round(v/s)*s) for v in a), max(abs(v-round(v/.07)*.07) for v in w))" "<recording>/SENSOR.CSV"
```

**M48 The first start (1041).** As M45's first start: a logbook copy whose recordings have fits stored by a build from before this change (a `v5` build), with a logbook column over a Sensor fusion measurement enabled. Start this build: each stored fit is dropped when its recording is loaded and fitted again once when something switched on needs it, counted in the status bar ("Computing results: k / n"); each record file in `cache/` is written again with `batch-temperature-bias-v6`; quit and start again: nothing is computed.

**M49 The scale on the reference recordings (1020, 1021).** M47's commands, run again with the scale state. Record per recording `model.scale`, `stopping.rule`, the iterations per pass (`seeds[0].iterations`, `stopping.passes`), `objective`, `quality` and the fit time against M47's run, and whether any pass settled with its first iteration's cost unchanged (the trace's first `before` equal to its `after`: phase 2's decision on the damping). Reference: `11-17-12` is unit 014667, whose gyro the lab found to read 2-3 % low (`experiments/fusion_lab/NOTES.md` 7.4, where the best fit multiplies the nominal conversion by about 1.02), and since the kernel divides the reading by its factor, its fitted gyro factors are expected below one by about that much on the turning axes (about 0.98); `08-35-23` is from a unit found on nominal. The numbers go in the report as they are, not adjusted. In one line per recording:

```bash
python -c "import json,sys; d=json.load(open(sys.argv[1])); print(d.get('failure'), d['stopping']['rule'], d['stopping']['passes'], d.get('seeds', [{}])[0].get('iterations'), d.get('objective'), d.get('quality'), d.get('model', {}).get('scale'))" TEMP/runs/<name>.json
```

**M50 The accuracy on the reference recordings (1056).** M49's commands, run again with `--csv TEMP/runs/m50-<name>.csv`, the outputs named `m50-<name>.*`. Per recording: exit 0 and `accuracy.computed` true; the CSV has thirty-three columns, the four accuracies and then the twelve covariance entries last, and `imu_outputs` rows; record the four accuracies' minimum, median and maximum (from the CSV), `accuracy.max_widening`, `widened_samples` and `undetermined_heading_samples`, and the scale factors' `acc_sigma` and `gyro_sigma`. On `11-17-12`, the runner's wall time against M49's (30.7 s with the scale state), and, from the stderr lines timestamped as they arrive, the time from the last `Pass p, iteration k` text to the runner's end: the covariance step, the reconstruction with its composition, the widening, the diagnostics and the CSV. The increase over M49 must be at most 20 % of M49's fit time. Wall times move with the machine's state between sessions, so the cost is also measured in one process, the post-fit work with and without the accuracy on the same fit, by `experiments/accuracy_cost` (untracked; it compiles `src/fusion` and reads an inputs dump of `--dump-inputs`). The numbers go in the report as they are, not adjusted. In one line per recording:

```bash
python -c "import json,sys,csv,statistics as s; d=json.load(open(sys.argv[1])); r=list(csv.reader(open(sys.argv[2]))); h=r[0]; print(len(h), len(r)-1, d['imu_outputs'], d['accuracy'], d['model']['scale']['acc_sigma'], d['model']['scale']['gyro_sigma'], [(c, min(v), s.median(v), max(v)) for c in h[17:] for v in [[float(x[h.index(c)]) for x in r[1:]]]])" TEMP/runs/m50-<name>.json TEMP/runs/m50-<name>.csv
```

**M51 The accuracy plots (1037, 1038, 1039).** Use 12.1's preamble and a fusable recording without a stored fit. The plot list: "Sensor fusion" ends Heading accuracy, Tilt accuracy, Horizontal acceleration accuracy, Vertical acceleration accuracy, after Roll; their axes read "(deg)", "(deg)", "(g)", "(g)". One fit: show the recording and check Heading accuracy alone: the status bar shows "Computing results: 0 / 1" and one fit runs; then check the other fourteen: they draw at once, with no computation. Colours: in the light and the dark theme, with Heading, Pitch, Roll, the two fused accelerations and the three GNSS accuracies (Horizontal, Vertical and Speed accuracy) checked beside the four, each accuracy line is told apart from all of them. Values: heading and tilt accuracy stay within 0-180 with no 360-degree jump; the legend and the measure tool show one decimal for them and four decimals of g for the two acceleration accuracies. The menu: the Plots menu is unchanged (it lists GNSS plots only). A rejected recording: the hand-edited `SCHEMA_VER,2` legacy file of section 11, which the lattice rejects: the four draw nothing for it and the status bar's warning lists it once. A column: the Add Column dialog's Sensor fusion group lists fifteen; add "Horizontal acceleration accuracy" at the exit marker: loaded and unloaded rows fill, at four decimals; the label is "Horizontal acceleration accuracy @ <exit marker>"; after a restart the values show without a load.

**M52 The validation of the model (1024, 1055).** Use 12.2's preamble. First write the step's script to `TEMP/m52.py` (untracked):

```bash
cat > TEMP/m52.py <<'EOF'
import json,sys,os,csv,bisect,statistics as s
d=json.load(open(sys.argv[2])); d=d.get('diagnostics',d); a=d.get('accuracy') or {}; t={}; g={}; m={}; F=[]
for x in d.get('residuals') or []:
    k,n,e=x['kind'],x['node'],x['squared_whitened_error']
    if k in ('position','velocity'): t[n]=x['time_s']; g[n]=g.get(n,0)+e
    elif k=='imu': m[n]=e
T=[t[k] for k in sorted(t)]; N=len(T); h=a.get('widening_half_width_s')
for k in range(N):
    c=min(k,N-2); lo=min(bisect.bisect_left(T,T[k]-h),c); hi=max(bisect.bisect_right(T,T[k]+h)-1,c+1)
    F.append((sum(g[i] for i in range(lo,hi+1))+sum(m.get(i,0) for i in range(lo+1,hi+1)))/(6*(hi-lo+1)-9))
r=list(csv.reader(open(sys.argv[3]))) if sys.argv[1]=='0' and len(sys.argv)>3 and os.path.exists(sys.argv[3]) else [[]]
print('exit', sys.argv[1], 'failure', d.get('failure'), 'rule', d.get('stopping',{}).get('rule'), 'passes', d.get('stopping',{}).get('passes'), 'iterations', d.get('seeds',[{}])[0].get('iterations'), 'objective', d.get('objective'))
print('fixes', d.get('gnss_states'), 'imu_outputs', d.get('imu_outputs'), 'quality', {q: d.get('quality',{}).get(q) for q in ('position_nrms','velocity_nrms','imu_nrms')})
print('scale', d.get('model',{}).get('scale'))
print('max_widening', a.get('max_widening'), 'widened_share', a['widened_samples']/d['imu_outputs'] if a.get('widened_samples') is not None else None, 'undetermined_heading_samples', a.get('undetermined_heading_samples'))
print('medians', [(c, s.median(float(x[r[0].index(c)]) for x in r[1:])) for c in ('headingAcc','tiltAcc','accHAcc','accDAcc') if c in r[0]])
print('window factor', (s.median(F), s.quantiles(F,n=20,method='inclusive')[18], max(F), sum(f>1 for f in F)/N) if N>1 else None)
EOF
```

Then, for each of M11-M14's recordings (`<name>` `11-17-12`, `08-35-23`, `10-15-24`, `08-35-41`, `<recording>` its folder as there), one run on this build and the script on its outputs, with the run's exit code. The runner writes the CSV only for a fit that succeeds, so the command deletes the CSV first and the script reads it only after exit code 0; no CSV of an earlier run is read:

```bash
rm -f TEMP/runs/<name>.v6.csv; "$R" --csv TEMP/runs/<name>.v6.csv "<recording>" > TEMP/runs/<name>.v6.json 2> TEMP/runs/<name>.v6.log; python TEMP/m52.py $? TEMP/runs/<name>.v6.json TEMP/runs/<name>.v6.csv
```

and the script on each golden fixture that fits, with `-` for the exit code and no CSV: `python TEMP/m52.py - tests/data/fusion/<fit>.json` for `coarse_linear`, `coarse_maneuver` and `stationary_spin`. It prints the exit code, `failure`, `stopping.rule`, the passes, the iterations and `objective`; the fixes and IMU samples and the three normalized RMS of `quality`; `model.scale` with its sigmas; `accuracy.max_widening`, `widened_samples` as a share of `imu_outputs` and `undetermined_heading_samples`; the medians of the CSV's four accuracy columns; and the widening's window factor at the fixes, from the diagnostics' `residuals` by the kernel's rule (`docs/SENSOR_FUSION.md` section 4, Widening: the fixes within `widening_half_width_s` of the fix, at least the two around it; the position and velocity terms of those fixes and the `imu` terms of the intervals between them, the priors excluded, over `6N - 9`): its median, 95th percentile, maximum and the share of fixes above one. A fit that does not converge prints its exit code, rule and failure and no residuals, since a failed fit's diagnostics carry none. Record the printout, unadjusted, in the phase report, and write it, rounded to two or three significant figures, into the tables of `docs/SENSOR_FUSION.md` section 8 ("Validating the model."). The step passes when every command ran and the document's numbers are the printout's, whatever the numbers are: no constant of the noise model is changed to move them.

Pass / fail and the numbers per step go in the phase report. A step that fails
is reported as it failed, not adjusted.

### 12.10 GNSS acceleration accuracy

What the automated tests cannot show: the plot in the real window. Use the
preamble of 12.1 (a COPY of a logbook, never the real one).

**M53 The GNSS acceleration accuracy plot (1101).** Show one recording with GNSS data. "GNSS (Advanced)" lists "Acceleration accuracy" after Cross-track acceleration; check it: it is drawn at once, nothing appears in the status bar for it, its axis reads "(g)", and its colour, a deep pink, is told apart from Horizontal, Vertical, Along-track and Cross-track acceleration checked beside it, from the GNSS accuracy plots of "GNSS (Basic)" and from the fused accuracies. Hovering over the plot, the legend shows two decimals of g; on a 5 Hz recording with `sAcc` near 0.5 m/s it reads about 0.18 (sqrt(2) x 0.5 m/s over 0.4 s), and it rises where the speed accuracy does. At the first and last fix it is about twice its neighbour's, having one interval in place of two.

Pass / fail per step goes in the phase report.

### 12.11 Sample continuity

What the automated tests cannot show: a real hole in the real window and on
the map page. Use the preamble of 12.1 (a COPY of a logbook, never the real
one) and a corpus recording with a GNSS hole during the climb,
`24-09-07/08-35-48` (16 s) or `24-09-04/13-35-10` (13 s).

**M54 A hole in the GNSS samples (1202, 1204, 1205, 1206, 1208).** Show the
recording alone and zoom onto the hole. Elevation and a GNSS speed (say
Vertical speed) stop at the last fix before the hole and resume at the first
after it, with nothing drawn between; the same with the x axis switched to
system time. An IMU plot (say an accelerometer axis) draws straight through
the hole. Hover over the hole: the legend reads "--" for elevation and the
speed, and a value for the IMU plot; at the fixes on either side it reads
their values. Measure with the measure tool from a point inside the hole to
a point after it: the end inside reads "--" and so does the change; from a
fix before the hole to one after it, the change is shown. With the Set
Ground tool, click inside the hole: the ground elevation does not change.
On the map, the track has a visible break where the hole is, and the cursor
dot disappears while the cursor is inside the hole and is back at the fixes
around it. Moving the mouse along the map's track, the hover never runs
along the gap.

Pass / fail per step goes in the phase report.

### 12.12 GNSS holes bridged by the IMU

What the automated tests cannot show: the two recordings of the reference
corpus that the kernel rejected for a hole in their GNSS fixes, each with the
hole in the aircraft during the climb and the IMU running through it. Use the
preamble of 12.2.

**M55 The reference recordings with a GNSS hole (1310, 1313).** For each of
`08-35-48` (`TEMP/data/Data comp 3 - FS 2 - serie nr 2 - 00769 (pers.)/24-09-07/08-35-48`,
unit 00769, 35 minutes) and `13-35-10`
(`TEMP/data/Data comp 2 - FS 2 - serie nr 2 - 01086 (pers.)/24-09-04/13-35-10`,
unit 01086, 56 minutes), `<name>` its name and `<recording>` its folder, one
run on this build with the exit code; the runner writes the CSV only for a fit
that succeeds, so the command deletes the CSV of an earlier run first:

```bash
rm -f TEMP/runs/m55-<name>.csv; "$R" --csv TEMP/runs/m55-<name>.csv "<recording>" > TEMP/runs/m55-<name>.json 2> TEMP/runs/m55-<name>.log; echo $?
```

Then the numbers, in one line per recording: the outcome (`failure`, `None`
for a success), `stopping.rule`, `stopping.passes`, `seeds[0].iterations`,
`objective`, `quality`, the fixes and IMU samples (`gnss_states`,
`imu_outputs`), the number of `input.gnss_holes` and the longest, and from the
CSV the largest `headingAcc` and `tiltAcc` among the rows whose `_time -
input.epoch_utc_s` lies strictly inside the longest hole, each against its
median over all rows:

```bash
python -c "import json,sys,os,csv,statistics as s; d=json.load(open(sys.argv[1])); H=d.get('input',{}).get('gnss_holes',[]); L=max(H,key=lambda h:h['length_s']) if H else None; e=d.get('input',{}).get('epoch_utc_s'); r=list(csv.reader(open(sys.argv[2]))) if L and os.path.exists(sys.argv[2]) else [[]]; h=r[0]; I=[x for x in r[1:] if L['start_s']<float(x[h.index('_time')])-e<L['start_s']+L['length_s']]; print(d.get('failure'), d.get('stopping',{}).get('rule'), d.get('stopping',{}).get('passes'), d.get('seeds',[{}])[0].get('iterations'), d.get('objective'), d.get('quality'), d.get('gnss_states'), d.get('imu_outputs'), len(H), L, len(I), [(c, max(float(x[h.index(c)]) for x in I), s.median(float(x[h.index(c)]) for x in r[1:])) for c in ('headingAcc','tiltAcc')] if I else None)" TEMP/runs/m55-<name>.json TEMP/runs/m55-<name>.csv
```

And the iterations of each pass of the full fit, from the progress texts on
stderr (`Pass p, iteration k`):

```bash
python -c "import re,sys; n={}; [n.__setitem__(int(m[1]), int(m[2])) for m in (re.match(r'Pass (\d+), iteration (\d+)$', l.strip()) for l in open(sys.argv[1])) if m]; print(n)" TEMP/runs/m55-<name>.log
```

Expected from the specification: one hole of about 15.6 s on `08-35-48`, ten
minutes before exit; seven on `13-35-10`, the longest about 13.2 s, in the
five minutes before exit. `input.gnss_holes` counts every hole of the fitted
window's fixes, whose threshold at 5 Hz is 0.3 s, so it also lists the short
holes of 0.4 s and more beside those. Under the slow tail's three bounds and
`v8`, `13-35-10` was expected to end `iteration limit`, a solver failure (its
second initializer segment's fit reaches its limit and the full fit starts
from that attitude), and did: its JSON then held `failure`, `stopping` and
`quality` only, the printout `None` for the fixes, the holes and the
accuracies, and no CSV was written. Since the scale factors are held until
the fit is stable (`v9`), it converges; M56 (12.13) records its current
outcome and its holes. A recording that does not converge is reported with
its rule and its numbers, not tuned. The printout goes in the phase report as
it is and, rounded to two or three significant figures, into the table of
`docs/SENSOR_FUSION.md` section 8 ("GNSS holes on the reference recordings").

Pass / fail and the numbers per step go in the phase report.

### 12.13 The scale factors as a refinement

What the automated tests cannot show: the two stages of the full fit on the
reference recordings, whether each release is kept, and the recording whose
free scale factors were the failure. Use the preamble of 12.2.

**M56 The staged scale on the reference recordings (1313, 1412).** M49's commands
on its four recordings (`<name>` `11-17-12`, `08-35-23`, `10-15-24`,
`08-35-41`, `<recording>` its folder as in M11-M14), the outputs named
`m56-<name>.*`, each with the exit code:

```bash
"$R" "<recording>" > TEMP/runs/m56-<name>.json 2> TEMP/runs/m56-<name>.log; echo $?
```

and M55's command on `13-35-10` (its folder as in M55), with its CSV:

```bash
rm -f TEMP/runs/m56-13-35-10.csv; "$R" --csv TEMP/runs/m56-13-35-10.csv "TEMP/data/Data comp 2 - FS 2 - serie nr 2 - 01086 (pers.)/24-09-04/13-35-10" > TEMP/runs/m56-13-35-10.json 2> TEMP/runs/m56-13-35-10.log; echo $?
```

Then the numbers, in one line per recording: the outcome (`failure`, `None`
for a success), `stopping.rule`, `stopping.passes`, `seeds[0].iterations`,
`objective`, `quality`, `model.scale` and `scale_release`:

```bash
python -c "import json,sys; d=json.load(open(sys.argv[1])); print(d.get('failure'), d.get('stopping',{}).get('rule'), d.get('stopping',{}).get('passes'), d.get('seeds',[{}])[0].get('iterations'), d.get('objective'), d.get('quality'), d.get('model',{}).get('scale'), d.get('scale_release'))" TEMP/runs/m56-<name>.json
```

and the iterations of each pass of the full fit from the progress texts on
stderr, as M55 prints them, with the place of the boundary between the
stages (the number of the first pass after `Releasing the scale factors`,
`None` when the held stage did not converge):

```bash
python -c "import re,sys; n={}; r=None
for l in open(sys.argv[1]):
    l=l.strip(); m=re.match(r'Pass (\d+), iteration (\d+)$', l)
    if l=='Releasing the scale factors': r=max(n)+1 if n else 1
    elif m: n[int(m[1])]=int(m[2])
print(n, 'released from pass', r)" TEMP/runs/m56-<name>.log
```

For `13-35-10` also M55's printout of the holes and the accuracies inside the
longest one, on `m56-13-35-10.json` and `m56-13-35-10.csv`. Expected from the
specification: `13-35-10` converges (held at one, the probe converged in two
passes and 33 iterations with an IMU normalized RMS of 0.45); the four are at
or near their M49 numbers (`docs/SENSOR_FUSION.md` section 8), the released
stage adding a few iterations. A recording that misses the expectation is
reported with its numbers, not tuned. The printout goes in the phase report
as it is and, rounded to two or three significant figures, into
`docs/SENSOR_FUSION.md` section 8 (the staged scale's table and the second
row of the table of GNSS holes).

Pass / fail and the numbers per step go in the phase report.

### 12.14 Background computation per recording

What the automated tests cannot show: the Compute switch in the running
application, as the status bar, a logbook cell, a row and a plot present it,
and the Import preferences page, which no test compiles.

**M57 The Compute switch (1505, 1520, 1522).** A logbook with a recording whose
sensor fusion fit takes long (one of M49's) and a few others with IMU data;
the logbook's Compute column added (Add Column, "Session": every cell reads
"On" and no session file changes) and a column over a Sensor fusion value
(roll at the exit marker, say) enabled.

1. While the long recording is being computed (the status bar shows
   "Computing results" and, in its hover, that recording), switch it off by
   editing its Compute cell to "Off". The status bar's count drops at once and
   its item ends or moves on to the next recording; the recording's cell in
   the Sensor fusion column reads a grey "excluded", whose tooltip is "Not
   computed: background computation is switched off for this recording"; its
   row has no warning triangle; with its track shown and a Sensor fusion plot
   checked, its track is absent from the plot and nothing warns. Its session
   file now carries `$VAR,_COMPUTE,off`.
2. Switch it on again ("On"): it is counted again and its fit runs from its
   start (the hover's step starts over), then its cell fills in.
3. Select half the recordings and choose "Set Compute..." from the context
   menu, then "Off" (their cells read "Off"; those not loaded stay unloaded).
   Disable and re-enable the Sensor fusion column: the other recordings are
   loaded and fitted, the ones switched off are never loaded (the status bar
   never counts them) and their cells read "excluded", unless they have a
   kept value, which they show.
4. Preferences > Import: uncheck "Compute newly imported recordings in the
   background", close and reopen the dialog (still unchecked), and import
   one recording: its session file carries `$VAR,_COMPUTE,off`, its Compute
   cell reads "Off", and nothing is computed for it. Check the box again.

Pass / fail per step goes in the phase report.

### 12.15 Fused speed plots

What the automated tests cannot show: the three fused speeds in the real plot
list, their colours as thin lines on both plot backgrounds beside the speeds
they are read with, the legend and the measure tool, and a column over one
made in the Add Column dialog.

**M58 The fused speeds (1602, 1603, 1605, 1611).** Use 12.1's preamble (a
COPY of a logbook, never the real one) and a fusable recording without a
stored fit.

1. The plot list: "Sensor fusion" has Elevation, then Horizontal speed,
   Vertical speed and Total speed, then Horizontal acceleration and the rest
   as M39 and M51 list them. The three speeds' axes read the unit of the GNSS
   speeds.
2. One fit: show the recording and check Total speed alone. The status bar
   shows "Computing results: 0 / 1" and one fit runs; when it ends, Total
   speed draws. Check Horizontal speed and Vertical speed: they draw at once,
   with no computation.
3. Colours: check the three GNSS speeds (GNSS (Basic): Horizontal speed,
   Vertical speed, Total speed), Wind-corrected horizontal speed and Speed
   accuracy beside the three fused speeds, in the light and the dark theme.
   Each fused line is told apart from its GNSS twin where the two overlay and
   from every other line of the group, and its tick labels read on both
   backgrounds. Where the receiver's accuracy is good, each fused line lies on
   its twin.
4. Values: the legend and the measure tool show each fused speed with the
   unit and the decimals of its GNSS counterpart, in both unit systems
   (Preferences > General).
5. A column: the Add Column dialog's Sensor fusion group lists fifteen; add
   "Total speed" at the exit marker. Loaded and unloaded rows fill, in the
   speed unit; the label is "Total speed @ <exit marker>"; after a restart
   the values show without a load.

A colour that looks wrong is moved within the rule of `docs/PLOT_COLOURS.md`,
its new figures written into the comment of the plot table, and the step is
run again.

Pass / fail per step goes in the phase report.

## Appendix A. The acceptance items (1-19)

The numbered acceptance list that section 9.1, `tests/acceptance_map.txt`, and
the "acceptance N" comments on test functions refer to. All of it must be
demonstrated by automated tests that use generated fixtures in temporary
directories, never the user's logbook or preferences, with expected values
stated independently rather than computed by the code under test. Clause 6
is stated as amended by the specification "Sensor fusion plots, attitude and
the orientation attribute" (appendix I).

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
6. (as amended) A released-format logbook file (normalized units, no
   `SCHEMA_VER`) loads, its gyro is corrected once, and saving it does not
   rescale or relabel it. Loading it adds only jumper mass and planform area
   where they are absent (the legacy backfill), additively and once. Wind is
   not written: a recording without it reads zero from a constant default.
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
functions refers to them. Clauses 10, 11, 13, 14, 15, 16 and 17 are stated as
amended by the specification "Demand-driven requested calculations"
(appendix F), clauses 11 and 15 again by the specification "One status bar
for background work" (appendix H), and clause 15 again by the specification
"Sensor fusion plots, attitude and the orientation attribute" (appendix I), by
the specification "The documented noise model and the accuracy, part 1"
(appendix K), whose four accuracy plots made the real fusion plots twelve, and
by the specification "Fused speed plots" (appendix Q), whose three fused
speeds make them fifteen.
"The branch" is `sensor-fusion-clean-port`, the behavioural reference the
fusion kernel was ported from.

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
10. (as amended) Cancelling a running job through the executor stops it at the
    next solver boundary, publishes nothing, and leaves the calculation
    requestable. The chosen next job then starts.
11. (as amended) A session with no IMU data is never counted in the progress of
    the computations nor listed among the recordings that could not be
    computed, for any fusion plot, and no job can be created for it.
12. Blocker inspection reports fusion for `accH` on an unrequested session,
    nothing after publication, and never triggers a fit.
13. (as amended) With two explicit test calculations where B consumes A,
    checking a plot of B's output runs A then B with no other action.
14. (as amended) Never more than one job runs at a time. The executor holds at
    most the running job and one chosen next job, and an offer equal to either
    creates no duplicate job.
15. (as amended) Row behaviour, tested without widgets: checking a fusion plot
    with three visible fusable tracks computes them one after another, the
    sessions still to compute falling as each publishes; unchecking mid-way
    drops the tracks that are waiting and lets the running one finish; checking
    again resumes; a fourth track shown afterwards is computed with no other
    action. The real fusion plots are the fifteen of the "Sensor fusion"
    category (the four accuracies since the specification of 1001-1065, the
    three fused speeds after Elevation since the specification of
    1601-1612), and heading, pitch and roll share one job.
16. (as amended) Starting the application with fusion plots checked starts no
    job, because every track starts hidden. Applying a profile that checks
    fusion plots, the Plots menu and any other programmatic check create
    demand exactly as a click on the check box does.
17. (as amended) Removing or unloading a session with a running job or the
    chosen next job, and quitting with a job running and one chosen next,
    neither crash nor hang, and publish nothing stale.
18. The job model reports every transition through model signals and retains
    finished jobs with state, timing, and reason; a test view can render the
    whole history from the model alone.
19. The application stays interactive during a fit: plots pan and zoom,
    tracks can be shown and hidden, and sessions can be edited.
20. None of the branch's modal dialog, re-entrancy guard, idle-scheduler
    pause, or plot-widget rebuild guard exists in the result.

## Appendix C. The acceptance items of the sensor fusion improvements (201-247)

The requirements of the specification "Sensor fusion: segmented initializer,
stopping rule and IMU model" (`PLANS/fusion-improvements.md`), one sentence
each, grouped by the specification's section with the section number in
front. These are items 201-247 of `tests/acceptance_map.txt` (item = 200 + the
number below) and the rows of section 9.3. Two requirements the specification
added after the first numbering are stated as the second sentence of items
10 and 39. Clause 28 is stated as amended by the specification "One status
bar for background work" (appendix H), and clauses 15, 16, 17, 21, 27, 30,
35, 45, 46 and 47 by the specification "The documented noise model and the
accuracy, part 1" (appendix K), which replaces the per-step term and its
slopes by the datasheet's noise and the derived sampling term and remainder,
adds the scale state, without which a constant temperature still reproduces
the constant-bias fit (46), and adds the four accuracy channels to the result
and to the runner's CSV (30, 35). Clauses 19 and 46 are stated as amended
again by the specification "The scaled IMU factor stands alone" (section 9.3 says how).
Clause 44 is stated as amended by the specification "GNSS holes bridged by
the IMU" (appendix N), under which the slow tail bounds the IMU normalized
RMS too, and clause 43 by the specification "The scale factors as a
refinement" (appendix O), under which the full fit runs its passes in two
stages. Clauses 30 and 35 are stated as amended again by the specification
"Fused position and speed accuracy" (appendix R), which adds the twelve
entries of the position and velocity covariance blocks: thirty-three channels,
thirty-four outputs.

1. (3.2, step 1) The fitted window is cut into consecutive segments of 600 s
   from its first fix; a final piece shorter than 120 s is merged into the
   segment before it; a window shorter than one segment is one segment.
2. (3.2, step 2a) In each segment the anchor is the fix with the smallest
   sAcc, the earliest on a tie, and the coarse attitude there aligns the
   measured force with GNSS acceleration minus gravity, with zero gyro bias.
3. (3.2, step 2b) The prefix is the 60 s window centred on the anchor, clipped
   to the segment, started from the coarse attitude carried to the window's
   first fix with zero bias, and fitted from four heading offsets with the
   production graph and tuning.
4. (3.2, step 2c) The yaw sigma is the marginal standard deviation of the
   window's first pose about the navigation vertical, from the best start's
   graph rebuilt at its fitted bias.
5. (3.2, step 2d) The window doubles while the sigma exceeds 20 degrees, the
   window does not cover the segment, and the last doubling cut the sigma by
   at least 20 %; otherwise growth stops.
6. (3.2, step 2e) The whole segment is fitted once from the best prefix fit's
   attitude carried backwards and forwards with that fit's gyro bias, and the
   fitted attitude of every fix and the fitted bias are kept.
7. (3.2, step 3) The full graph's initial values are each pose's attitude from
   its segment's fit, the GNSS positions and velocities, the first segment's
   gyro bias and a zero accelerometer bias.
8. (3.2, step 4; 3.4) The full fit runs from the stitched state and converges;
   a resting segment's yaw is arbitrary and accepted; the corpus and the
   reference recordings converge as the specification's sections 3.4 and 12
   say.
9. (3.3) Prefix and segment fits are cancellable and report progress, each
   iteration a boundary, with texts that name the segment and, for a prefix,
   its length.
10. (3.3) A failed prefix or segment fit is a start with infinite objective;
    all four starts failing grows the prefix; all failing at the segment's
    full length falls back to the coarse attitude propagated with zero bias
    over that segment only, and the diagnostics say so. A prefix fit is one
    pass of at most 50 iterations, a segment fit uses the production limits,
    the stopping rule with its slow tail applies to both, and a fit that ends
    on the limit is still used as a start and reported as such.
11. (3.3) A segment satisfies the validity rules of the fitted window: fixes
    inside IMU coverage, no IMU gap, at least three fixes.
12. (3.3) No stationary-window detector takes part; it decides nothing.
13. (4.1) The bias-settled test is the re-preintegration cost test at 1e-6
    relative; the pass structure (at most five passes) and the settle
    tolerance 1e-8 are unchanged.
14. (4.2) A final pass at the iteration limit is accepted when the last 20
    iterations' mean relative decrease is below 1e-4 and the position and
    velocity normalized RMS are both below 2, the diagnostics say so, and a
    fit that meets neither stays a solver failure.
15. (5, as amended) The per-step term is the sampling term and the rotation
    remainder of the specification of 1001-1065, added in quadrature so that
    the step's covariance is `(D^2 + eps^2 / dt) I`, and a step without
    signal change and without rotation has the density covariance exactly.
16. (5, as amended) There are no slopes; the densities are derived from the
    configuration (the specification of 1001-1065); the step boundaries and
    the midpoint sampling are unchanged.
17. (5, as amended) The term has no constant and falls with the step as the
    sampling error does.
18. (5) The per-step covariance is applied by setting the preintegration's
    shared parameters before each `integrateMeasurement` call.
19. (6, as amended) The gyro bias is `b(t) = b0 + b1 (T(t) - T_ref)` with
    `T_ref` the mean IMU temperature over the fitted window, the accelerometer
    bias stays constant, and each interval is evaluated at its own bias in the
    fit and in the reconstruction; in the fit, inside its one IMU factor, the
    scaled one.
20. (6) `b0` keeps today's prior (0.03 rad/s); `b1` is zero-mean with sigma
    0.010 deg/s per degC.
21. (6, as amended) `IMU/temperature` is a required input of the fit (the
    twenty-second when it was added; the fit has twenty-six since the
    specification of 1001-1065), one value per IMU sample; a recording handed no temperature is rejected by the kernel
    with a reason naming the channel, and a session without the column is
    blocked like any missing input.
22. (6) A recording whose temperature does not change leaves `b1` at its
    prior.
23. (6) The initializer is unaffected: segment and prefix fits use a constant
    bias and the full fit starts `b1` at zero.
24. (7) The diagnostics report the segments (start, end, prefix length, yaw
    sigma, iterations) and any segment that fell back.
25. (7) The diagnostics report which rule ended the fit, the last pass's mean
    relative decrease and the re-preintegration cost difference.
26. (7) The diagnostics report the normalized RMS of the IMU, position and
    velocity factors and the objective per state.
27. (7, as amended) The diagnostics report the configuration, the noise
    model (`model.noise`, the specification of 1001-1065) and the fitted `b0`
    and `b1` with `T_ref`, always as numbers.
28. (7, as amended) The failure text, the status bar warning's hover and the
    logbook row's, keeps showing the reason, as the tooltip did; the
    diagnostics remain an account, not an input.
29. (8) The runner takes a folder or the two paths, imports them exactly as the
    application does (parser, conversion layer, on-demand derivation, the
    legacy gyro scale) and feeds the kernel the channels the job queue would.
30. (8, as amended) The runner prints the diagnostics JSON on standard
    output, writes the seventeen channels as CSV on request (since the
    specification of 1001-1065 the twenty-one and since that of 1701-1717
    the thirty-three, or the seventeen when the accuracy is absent), and
    exits 0 for Succeeded and non-zero otherwise
    with the outcome and reason on standard error.
31. (8) The runner has no GUI and neither reads nor writes the user's logbook
    or settings.
32. (8) Progress texts go to standard error; cancellation is not required.
33. (8) The runner is a test/tooling target, built with the tests and never
    shipped.
34. (9) The kernel's purity, threading and cancellation rules are unchanged: a
    function of the channels, no session or GUI object, every solver
    iteration a boundary, cancellation observed at boundaries only.
35. (9, as amended) The public result contract (channels, outcomes) is
    unchanged by this specification (the specification of 1001-1065 adds the
    four accuracy channels and that of 1701-1717 the twelve covariance
    entries: thirty-four outputs), and a slow-tail acceptance is a Succeeded
    result.
36. (9) Nothing outside the fusion library links GTSAM.
37. (10) The parity tests against the reference branch are retired; goldens are
    captured from the changed kernel with the same harness, the regression
    tests compare against them, and the fixtures are deterministic.
38. (10, test 1) A recording that starts in motion: prefixes grow until the yaw
    sigma is below 20 degrees exactly at the first length containing the
    manoeuvre, and the full fit converges to the truth attitude within 2
    degrees.
39. (10, test 2) A recording at rest throughout, longer than two doublings:
    growth stops at 120 s because the doubling gains nothing, the full fit
    converges, and roll and pitch are within 0.5 degrees of the truth. Prefix
    fits run one pass of at most 50 iterations, and a prefix and a segment fit
    forced onto the iteration limit are still used as the start and the
    diagnostics say so.
40. (10, test 3) A recording with sAcc 2 m/s except 0.3 m/s at 200 s: that fix
    is the anchor, the first prefix is the window 170-230 s, and the segment's
    start attitude is the prefix fit's carried back by the gyro.
41. (10, test 4) A recording longer than two segments with a bias drifting
    linearly by 1 deg/s: every segment fit converges, and the full fit with
    the temperature model recovers `b1` within 20 % in at most 30 iterations.
42. (10, test 5) A segment whose prefix fits all fail falls back to
    propagation, the diagnostics say so, and the full fit still runs.
43. (10, test 6, as amended) Bias-settled test: a fit whose cost stops
    changing converges within two passes in each stage of the full fit, the
    held and the released.
44. (10, test 7, as amended) Slow tail: a forced final pass at the limit is
    accepted when every condition holds, the mean relative decrease and the
    position, velocity and IMU normalized RMS below their bounds, and is a
    failure when any fails, the IMU normalized RMS alone included (the
    specification of 1301-1313).
45. (10, test 8, as amended) Per-step term: a step without signal change and
    without rotation has the density covariance exactly, a signal with a known
    second derivative gives the derived sampling term, and a constant turn the
    derived remainder (the specification of 1001-1065).
46. (10, test 9, as amended) A recording without `IMU/temperature` is
    rejected by the kernel and blocked in a session like any missing input; a
    constant temperature leaves `b1` at its prior, and with the scale factors
    of the specification of 1001-1065 held at one by a prior a thousand times
    tighter than the datasheet's, reproduces the constant-bias fit.
47. (11, as amended) `docs/` describes the segmented initializer, the
    stopping rule with its slow-tail acceptance, the noise model of the
    specification of 1001-1065 in place of the per-step term, and the
    temperature-dependent bias, in the place that documents the fusion
    model.

## Appendix D. The acceptance items of stored requested results (301-350)

The clauses of the specification "Storing requested calculation results with
the session", one sentence each, with the specification's section number in
front. These are items 301-350 of `tests/acceptance_map.txt` (item = 300 + the
number below) and the rows of section 9.4. Two sentences of the
specification's scope have no item, because no test can show them: stored
results are not shared between logbooks or machines, and a record need not be
readable by people. Clauses 10, 16, 17, 34 and 41 are stated as amended by
the specification "Stored results: validity that mirrors memory" (appendix E).
Clauses 4, 6, 21 and 37 are stated as amended by the specification
"Demand-driven requested calculations" (appendix F), and clauses 6, 21, 37 and
39 by the specification "One status bar for background work" (appendix H).
Clause 37 is stated as amended by the specification "The documented noise
model and the accuracy, part 1" (appendix K), and again by the specification
"Fused position and speed accuracy" (appendix R): thirty-three channels.

1. (2) The result of every explicitly requested calculation that ended as a
   function of its inputs is stored on disk, in the logbook's cache: a
   successful result with all its outputs, or a rejection or solver failure
   with its reason and diagnostics.
2. (2) It is restored when the session is loaded, so that readers, plot rows
   and logbook columns see it exactly as if it had just been published,
   provided it is still valid.
3. (2) On-demand and plugin calculation results are never stored.
4. (2, as amended) A stored result is a memory, not a request: when it is
   stale or missing, the calculation reads as not requested until something
   switched on - a checked plot over a visible session, or an enabled logbook
   column over it - has it computed.
5. (2) The session file is unchanged: its bytes, its format and what it
   enumerates; derived sensors never appear in enumeration or in the session
   file.
6. (2, as amended) The kernel is unchanged, and plot rows and logbook columns
   count a restored result as computed: it is not counted in the progress of
   the computations and runs no job.
7. (3) For each (session, requested calculation) there is at most one record,
   written when the result is published (the moment the engine installs it, on
   the main thread) and replaced by the next publish for the same pair.
8. (3) A record holds the calculation id and the outcome (`Ok` with outputs,
   or `Ok` with the reason of a rejection or solver failure and no
   measurements), every output the publish installed (measurements with
   samples and unit, attributes with the diagnostics), and the validity stamp.
9. (3) Nothing is stored for a result the engine did not install (`Cancelled`,
   `ResourceExhausted`, a refused publish) or for an `UndeclaredRead` or
   `InvalidOutput` status, and such a run deletes nothing.
10. (3, as amended) A record is removed only when it is found stale on load,
    when its session is deleted, when an input it depends on changes or a
    registry change made while the application runs drops its result, or when
    the next publish for the same pair replaces it; never by eviction,
    unloading, a registry change that does not reach it, a teardown, or
    quitting.
11. (3) Values round-trip bit for bit: every double of a restored measurement
    equals the published one, `-0`, NaN and the infinities included, and a
    restored attribute string is byte-identical.
12. (3) The golden tests cannot tell a restored fusion result from a freshly
    computed one.
13. (4.1) The input fingerprint covers the values the result depended on as
    the engine's dependency records name them (source measurements with their
    samples and unit text, attributes, declared preferences), directly or
    transitively, absent ones included; the record lists their names so that
    the check can be repeated on load.
14. (4.1) An edit the result does not depend on (moving a marker, a
    description) leaves the record valid.
15. (4.1) An edit it depends on (a merge that adds IMU data, a changed
    `SCHEMA_VER`, any other dependency) invalidates it.
16. (4.2, as amended) A record is valid only while
    `CalculationCompatibilityVersion` and the calculation's result version
    equal the current ones and every name it looked up resolves as it did
    (appendix E, clauses 6-8); for sensor fusion the result version is the
    kernel's algorithm string. The environment fingerprint is no longer part
    of a record.
17. (4.2, as amended) The compatibility-version rule gains the clause: bump
    it, or the result version of the calculation concerned, whenever a change
    can alter what a requested calculation or anything it reads produces.
18. (4) A record that fails any check is deleted when the session is loaded,
    and the calculation reads as not requested; nothing is recomputed on its
    own.
19. (5) On every load of a session (from the logbook at start-up, on show, on
    reveal, on an import-merge into an existing session) every valid record is
    installed into the engine before any reader asks.
20. (5) A restored result has the same outputs, status, detail and dependency
    edges as a fresh publish, so that later invalidation behaves identically.
21. (5, as amended) Plot rows count a session with a restored result as
    computed: it is not counted in the progress of the computations and runs no
    job; a stale or absent result is in demand like any missing one.
22. (5) Blocker inspection reports a restored result as it reports a published
    one, a failure's `NotProduced` with its detail included.
23. (5) Logbook columns that depend on a requested calculation are computed
    from the restored or published result when the session is loaded, and
    cached in `index.json` like any other column, stamped so that dropping the
    record drops them; without a record the column stays unavailable.
24. (5) Dropping or writing a record drops the cached values stamped with it,
    whatever happens around it: a crash, an index written before the stamp, a
    failed write, an environment change.
25. (5) Unloading a session (eviction, hiding beyond the cache capacity,
    quitting) loses nothing: the record is already on disk.
26. (5) Publishing writes the record, and nothing else does: a restore never
    rewrites it.
27. (5) Records are written through the logbook manager with the atomicity of
    a session save; a failed write leaves the previous record, if any, intact
    and the in-memory result untouched, and the write is tried again at the
    next publish.
28. (5, as settled) A session imported and computed before its first save
    stores its result under the file stem reserved for it; if it is never
    saved, the record is removed at the next start.
29. (5) A record is deleted when its session is deleted from the logbook and
    when it is found stale.
30. (6) There is one record file per (session, calculation) in the logbook's
    `cache/` folder, a sibling of `sessions/`, named from the session's file
    stem and the calculation id, in a form the logbook's session scan can
    never mistake for a session. `sessions/` holds only the recordings;
    everything in `cache/` is derived and may be deleted while the
    application is closed, after which every requested calculation reads as
    not requested until requested again.
31. (6) The encoding round-trips doubles bit for bit, makes one record
    readable without the session, has a size of the order of the session file,
    and carries a version field so that a future format can refuse or migrate
    an old record.
32. (6) Existing logbooks have no records: every requested calculation reads
    as not requested until it is requested; there is no migration.
33. (7) Publish, restore and the writing of records happen on the main thread;
    the engine keeps its threading rules.
34. (7, as amended) The store reads a record only for a session being loaded,
    or for the column worker's temporary copy of a session that is not
    loaded when a missing logbook column needs it (a load for reading, never
    for writing), and never starts a calculation.
35. (7) Purity holds: with or without a record, the value every reader sees
    for a requested output is the same function of the session's inputs.
36. (7) Saving a session with a stored result gives the same bytes as saving
    it without.
37. (8, as amended) Test: a fusion fixture fitted, saved, unloaded and reloaded
    yields the thirty-three channels (the seventeen of the state, since the
    specification of 1001-1065 the four accuracies and since that of
    1701-1717 the twelve covariance entries) and the diagnostics
    bit-identical to the
    goldens, with no job created; nothing is counted in the progress of the
    computations and nothing is listed among the failures.
38. (8) Test: the same after an application restart (the test's logbook
    directory survives across two `SessionModel` lifetimes).
39. (8, as amended) Test: a rejection and a solver failure are restored with
    their reason and listed among the recordings that could not be computed,
    not tried again at the next start; no job runs.
40. (8) Test: editing an attribute the result does not depend on keeps the
    record; merging IMU data, or changing any dependency, drops it, the
    calculation reads not requested, and the file is gone.
41. (8, as amended) Test: bumping `CalculationCompatibilityVersion`, the
    calculation's result version, or the result version recorded for a
    calculation its lookups went through drops the record on load.
42. (8) Test: a record whose write fails leaves the in-memory result usable
    and the previous record intact.
43. (8) Test: deleting a session removes its records; a stray record whose
    session does not exist is ignored and removed by the next logbook scan.
44. (8) Test: a logbook column over `Fusion/roll` is cached from a valid
    record, unavailable without one, and invalidated when the record is
    dropped.
45. (8) Test: saving a session with a stored result produces the same
    session-file bytes as without it.
46. (9) `docs/` describes the record files (what they hold, when they are
    valid, that the session file is untouched) and amends the data schema's
    section 11, the calculation note's sections 8 and 9 and its description of
    restore, and the sensor fusion document's statement that results are lost
    on restart.
47. (10) A requested result is expensive and deterministic: it is kept as long
    as its inputs and its code are the same, and not a moment longer.
48. (10) The session file is the recording; derived data lives beside it,
    never in it.
49. (10) Restoring is not requesting: nothing starts on its own.
50. (10) A restored result is indistinguishable from a fresh one, to the bit.

## Appendix E. The acceptance items of stored-result validity (401-442)

The clauses of the specification "Stored results: validity that mirrors
memory", which amends "Storing requested calculation results with the
session" (appendix D), one sentence each, with the specification's section
number in front. These are items 401-442 of `tests/acceptance_map.txt`
(item = 400 + the number below) and the rows of section 9.5. The
specification's section 1 (motivation) has no item. Clause 16 is stated as
settled when it was implemented: subfolders are included. Clauses 3 and 31
are stated as settled when runtime registry changes were made as precise as
the load check: a change drops a result only when it can change what a name
the result looked up resolves to (docs/CALCULATIONS.md, section 5). Clause
27 is stated as amended by section 9.1, added later, and carries its section
10 test as well.

1. (2) A stored result goes stale under exactly the conditions that would
   drop the same result in memory, plus a change of the code that computed
   it; nothing unrelated to what the result reached makes it stale.
2. (3.1, 3.2) In memory, an installed requested result is dropped when a
   value it reached changes on its session (the samples or unit text of a
   source measurement, or a stored attribute, reached directly or through
   the calculations and conversions its inputs resolved through, a value it
   looked for and did not find that later appears included), or when a
   declared preference it reached changes.
3. (3.3, as settled) It is dropped when a registry change touches a name it
   resolved, directly or transitively, so that the name's answer can change:
   a calculation or family that can now answer such a name is added, the
   calculation whose result it used is removed, or the source-conversion
   layer changes under recorded data. A candidate behind the session's own
   data or behind the provider, and a removed candidate that was passed
   over, change nothing (unless the result's lookups reached something only
   through that candidate).
4. (3) Everything else leaves it installed: edits to values it did not
   reach (markers, description, wind), registrations of names it never
   looked up (altitude markers, unrelated plug-in outputs), and preferences
   it did not reach.
5. (4.1) Inputs, unchanged: the fingerprint over the values of every leaf
   the result reached, directly or transitively, present or absent,
   declared preference values included, matches the session and the
   preferences as they are now.
6. (4.2) Resolutions: for every name the result looked up while its inputs
   were gathered, directly or transitively, the record states what
   provided it: a calculation (its instance id and result version), the
   session's own data, or nothing.
7. (4.2) At load the same lookups are repeated against the current
   registry and must give the same answers.
8. (4.3) Code: `CalculationCompatibilityVersion` and the calculation's own
   result version are unchanged.
9. (4) The calculation environment fingerprint is no longer part of a
   record.
10. (4) The bump-rule clause reads: bump `CalculationCompatibilityVersion`,
    or the result version of the calculation concerned, whenever a change
    can alter what a requested calculation or anything it reads produces.
11. (4) A record that fails any check is deleted when its session is
    loaded, and the calculation reads as not requested; nothing is
    recomputed on its own.
12. (5) A registry change made while the application runs that drops an
    installed requested result also deletes that result's record, exactly
    like an input change.
13. (5) Tearing the registry down at shutdown, destroying or evicting a
    session, and replacing a session's contents without an input change
    delete nothing.
14. (5) A registry change that does not touch a result leaves both the
    installed result and its record alone, and logbook column values over
    that record remain cacheable.
15. (6) Every calculation, measurement and attribute a Python plug-in
    registers declares a result version: the plug-in code identity.
16. (6, as settled) The plug-in code identity is one digest over every
    `.py` file under the plug-in folder, subfolders included (its name
    relative to the folder and its bytes, in name order; `__pycache__` and
    hidden folders left out), the plug-in SDK file, and the Python and
    numpy versions the plug-ins run on.
17. (6) It is computed once, when the plug-ins are loaded.
18. (6) Editing any plug-in file, adding or removing one, or upgrading
    Python or numpy changes it, so a stored result whose lookups went
    through any plug-in calculation goes stale at its next load.
19. (6) A stored result whose lookups touched no plug-in calculation is
    unaffected by plug-in changes.
20. (6) The calculation environment fingerprint that stamps the logbook
    column cache in `index.json` also covers every registration's result
    version, so a plug-in edit or a built-in result-version change discards
    cached column values as a registry change does; its other contents and
    its role are unchanged. (Amended since: the environment is one per
    logbook column, over what the column's static closure can observe, so a
    change discards only the columns whose environment it changes;
    `docs/CALCULATIONS.md` section 9.)
21. (7) A record file that exists but cannot be opened or read in full
    when its session loads is skipped for that load: it is neither
    restored nor deleted, and the calculation reads as not requested for
    that load.
22. (7) The next load of the session tries again; a new publish for the
    same pair replaces it; deleting the session or the stray pass at
    start-up removes it.
23. (7) A record that was read but is not a record, is damaged, or has a
    format version this build does not read is deleted as stale, as
    before.
24. (7) Logbook column values of a session that depend on a skipped record
    are not cached in `index.json` for as long as the record stays
    skipped.
25. (8) The record gains the resolutions and loses the environment
    fingerprint, so its format version increases.
26. (8) A record of an earlier format version is deleted as stale when its
    session loads; there is no migration.
27. (9) Everything in "Storing requested calculation results with the
    session" not amended still holds: restoring is not requesting,
    publishing writes the record and nothing else does (except the
    deletions listed there and in clause 12), records are read only for a
    session being loaded, the session file is untouched, and a restored
    result is bit-identical to a fresh one. As amended by section 9.1: a
    logbook column's value is the same function of a session's valid stored
    results whether or not the session is loaded; the background worker
    restores them into its temporary copy of an unloaded session with the
    checks of a load (a stale record deleted, an unreadable one skipped) and
    computes the value from them - that copy counts as a load for reading,
    never for writing, and never requests or runs a requested calculation.
    Test: such a column, for a session that is not loaded, is refilled from
    the stored result after its cached value is discarded, without loading
    the session into the model and without any job.
28. (9) The engine keeps its threading rules; repeating the lookups at load
    uses the same resolution the engine uses for a fresh request and never
    runs a requested calculation.
29. (9) Plug-in loading stays a start-up operation: nothing reloads
    plug-ins or watches their files.
30. (10) Test: a stored fit survives each of these and is restored with no
    job: adding and removing an altitude marker; registering and
    unregistering a calculation whose outputs the fit never looks up;
    changing the descent-pause preference; a different plug-in set whose
    calculations the fit never looks up; an application restart after any
    of these.
31. (10, as settled) Test: a registry change made while the application
    runs that changes what a name the fit looked up resolves to (registering
    a calculation that provides a name it looked up and found missing, or
    removing the provider of one) drops the installed fit and deletes its
    record; a candidate registered behind the provider does not.
32. (10) Test: a record whose lookups resolve differently at load (a new
    candidate for a looked-up name, one that computes from the same inputs,
    registered before the load) is deleted and the fit reads not
    requested.
33. (10) Test: with a synthetic requested calculation that reads a plug-in
    calculation's output, editing any plug-in file, adding one, or changing
    the recorded Python or numpy version between runs makes its record
    stale; editing a plug-in file when the requested calculation reads no
    plug-in output does not.
34. (10) Test: the plug-in code identity is deterministic: the same files
    and versions give the same digest, and each listed ingredient changes
    it.
35. (10) Test: a plug-in edit discards cached logbook column values over
    plug-in calculations at the next start.
36. (10) Test: a record that cannot be read at load (held open without
    sharing on Windows, or a directory at its path) is kept, not restored,
    and restored at a later load once readable; its session's dependent
    column values are not cached while it is skipped.
37. (10) Test: a record of the previous format version is deleted as
    stale.
38. (10) Test: the existing tests of stored results pass, with the tests
    that asserted environment-change staleness rewritten to these rules.
39. (11) `docs/` and the plug-in README describe validity (the record's
    resolutions, the format version, unreadable records), the column values
    over skipped records, the environment fingerprint covering result
    versions, the bump rule and the plug-in code identity, and no
    user-facing note says any longer that unrelated settings changes make
    stored results stale.
40. (12) A stored result is a memory of an in-memory result: it goes stale
    when the in-memory one would be dropped, and when the code changes, and
    at no other time.
41. (12) What a result reached decides its validity, never what else is
    registered.
42. (12) A transient failure to read is not evidence that a record is
    wrong.

## Appendix F. The acceptance items of demand-driven requested calculations (501-563)

The clauses of the specification "Demand-driven requested calculations", which
amends "Sensor fusion as an explicit calculation, with plot-driven background
jobs" (appendix B) and "Storing requested calculation results with the
session" (appendix D), one sentence each, with the specification's section
number in front. These are items 501-563 of `tests/acceptance_map.txt`
(item = 500 + the number below) and the rows of section 9.6. The
specification's sections 1 (motivation) and 4 (terms) have no item. Clauses
13, 26, 28, 30, 32, 35-37, 39, 46, 47, 57 and 59 are stated as amended by the
specification "Calculation refinements" (appendix G). Clauses 17, 25, 26, 30,
35-37, 39, 42, 54, 55, 57, 59 and 63 are stated as amended by the
specification "One status bar for background work" (appendix H), which
restates them again where appendix G had. Clause 9 is stated as amended by the
specification "Sensor fusion plots, attitude and the orientation attribute"
(appendix I), again by the specification "The documented noise model and the
accuracy, part 1" (appendix K), which made the fusion plots twelve, five of
them outputs of the fit, and again by the specification "Fused speed plots"
(appendix Q): the fusion plots are fifteen, six of them outputs of the fit.

1. (2) The user expresses intent through plots and logbook columns, never
   through calculations: what is switched on is the request.
2. (2) Anything needed to complete what is switched on is wanted at once;
   anything no longer needed is dropped.
3. (2) Finished work is never wasted: results are stored.
4. (2) Background work must not degrade the rest of the application.
5. (2) A failure is shown, never retried in a loop.
6. (2) Each component's contract can be stated without naming the others
   (section 12 gives the contracts).
7. (3) The fusion kernel, its outputs and the record format are unchanged; the
   job history the jobs dock will read stays as it is.
8. (3) Nothing about demand or the job history is persisted across restarts,
   and no user-facing switch pauses or throttles background work.
9. (5, as amended) Plot demand: for every checked plot whose value is a
   requested output, every visible session needs the requested calculations
   that block that output. The fifteen plots of the "Sensor fusion" category
   are such plots: six are outputs of the fit (vertical acceleration, the four
   accuracies since the specification of 1001-1065, and vertical speed since
   the specification of 1601-1612), and the nine derived from its outputs are
   blocked by it.
10. (5) Column demand: for every enabled logbook column whose value depends on
    a requested output, every session in the logbook needs the requested
    calculations that block that value.
11. (5) A pair is in demand only while it has no result; a result counts
    whether published in this run or restored, success or input-determined
    failure.
12. (5) A pair whose calculation cannot apply (a declared input missing) is
    never in demand and is not reported anywhere.
13. (5, as amended) Whether a result exists: blocker inspection for a loaded
    session; for a session not loaded, first what this run remembers of its
    pairs (a failure; a not-applicable verdict of that column), then the
    logbook's record names and the reasons the index recorded for them, no
    record opened; a cell has a result only when every calculation it needs
    has a record; a record with a reason is a failed result before and after a
    restart alike; a reason the index learns later is announced as a record
    change; a known record counts until the column worker's restore deletes it
    as stale, which moves the pair into demand; the demand layer checks no
    staleness itself.
14. (5) Demand does not depend on how the state arose (a gesture, a profile,
    the start-up restore); at start-up every session is hidden, so plots
    create no demand until sessions are shown, while enabled columns do.
15. (5) Nothing about demand is persisted: it is derived again at the next
    start; a profile carrying a column over a requested output computes it for
    every session without a result, so no default profile carries such a
    column.
16. (5) When a requested calculation depends on another, the demand covers
    both, upstream first.
17. (6, as amended) A pair that enters demand is wanted at once, with no
    gesture (checking, showing, enabling, an input change that drops a demanded
    result), and progress counts its session from the first change.
18. (6) A pair that leaves demand (plot unchecked, session hidden, column
    disabled, result appeared by other means) is dropped before it starts;
    there is no queue of accepted requests to prune.
19. (6, as amended) The running job is never stopped because its pair left
    demand: it finishes and its result is published and stored; it is
    stopped only when its inputs change, its session goes away, its
    recording is switched off for background computation (amended by the
    specification of 1501-1524), or the application closes.
20. (6) After an input change of a demanded session the pair is in demand at
    once but starts only once the inputs have been still for a short moment,
    so a burst of edits runs one job; showing, hiding, checking and enabling
    take effect without the wait.
21. (6) Jobs run one at a time on the executor's worker thread.
22. (7) The next job is chosen when a job ends and whenever demand changes,
    from the demand as it is at that moment, not from the order in which pairs
    entered it.
23. (7) Plot demand first (the focused session, then the other visible
    sessions in logbook row order), then column demand: visible sessions, then
    the other loaded sessions, then sessions not loaded as the fill loads
    them, each in logbook row order.
24. (7) A session made visible while column demand is worked through is
    computed next, waiting at most for the running job, which is not
    preempted.
25. (8, as amended) An input-determined failure (rejection, solver failure) is
    a result: stored, listed among the failures with its reason, never run
    again.
26. (8, as amended) A failure that is not a function of the inputs (the worker
    could not start, out of memory, a session file that could not be loaded, a
    result whose record could not be written) is listed among the failures with
    its reason, marked as tried again at the next start, not offered and its
    session not loaded again in the run unless its inputs change, not stored,
    so tried again at the next start; the demand layer remembers it for the
    pair.
27. (8) There is no retry control.
28. (9, as amended) For column demand the demand layer, not the column worker,
    has a session that is not loaded loaded the way showing it would, without
    making it visible: an ordinary hidden session, pinned from its load until
    no column it needs is still waiting or running, the executor is shut down
    or the demand layer goes, then left to ordinary eviction.
29. (9) At most a small fixed number of sessions is loaded for this purpose at
    a time, not smaller than the number of jobs that may run at once; the next
    is loaded when one has ended.
30. (9, as amended) The fill is an idle-scheduler task below saving, loading
    visible sessions, bulk edits and column work, whose steps are the loads, so
    saves and bulk edits come first; it has work for the whole fill and steps
    only while it can load; it reports no progress of its own, and the status
    bar shows the computations while it works; the scheduler has the generic
    notion of a task with work it cannot step right now, rests instead of
    spinning, completes such a task once its work is gone without a step, and
    learns nothing about jobs.
31. (9) The session-id correction of a first load happens before the pair is
    offered to the executor.
32. (9, as amended) A session whose requested calculation turns out not to
    apply once loaded is remembered as not applicable for that column for the
    run, without a job, and is not loaded again for it; its column value stays
    unavailable.
33. (9) The column worker is unchanged and never knows a job exists; a record
    written later drops the cached value and the loaded-row refresh computes
    the new one; cheap column values never wait for a requested calculation.
34. (10) No refresh and no cancel for requested calculations anywhere;
    unchecking a plot, hiding sessions or disabling a column is how the user
    changes what is wanted.
35. (10, as amended) Plot rows and logbook column headers show nothing about
    computing: no indicator, no reserved room and no hover of their own, so
    they look as any other; progress is shown once, in the status bar, as
    "Computing results: k / n".
36. (10, as amended) The status bar's hover carries the numbers (done of the
    high-water mark) and the recording being computed with its step; the
    warning's hover lists the recordings that could not be computed with their
    reasons, a result that could not be stored included.
37. (10, as amended) The status bar's warning counts the recordings that could
    not be computed, a result that could not be stored included, as soon as a
    failure is current: beside the computations while they continue, alone once
    they end; its hover lists them with reasons.
38. (10) A column cell whose pair is in demand reads as pending, distinct from
    unavailable and from the unreadable-record pending state; pending is the
    view's presentation of demand, never a cached value, never in the index;
    the value underneath stays unavailable until the record is written, and
    sorting treats pending as unavailable.
39. (10, as amended) The status bar shows the fill as "Computing results: k /
    n", with no cancel button, distinct from the column worker's "Computing
    columns: k / n"; the fill reports no progress of its own.
40. (11) The executor's worker thread runs below normal priority, so that the
    user interface stays responsive.
41. (11) The number of jobs that may run at once is a single bound of the
    executor (one today); the load bound of section 9 follows it, and raising
    it changes no contract of the demand layer or the column worker.
42. (12, as amended) One widget-free demand layer replaces the plot rows'
    request logic: it reads the plot model, the session model, the enabled
    columns, blocker reports and the record set, derives demand, applies the
    priority, has sessions loaded, offers the next pair, remembers this run's
    failures and not-applicable pairs, and publishes progress, failures and
    pending cells.
43. (12) The demand layer is the only caller of the executor; nothing calls
    back into it: it observes the models and the executor's signals.
44. (12) The executor stays the only place a requested calculation runs and
    never loads a session; it holds at most the running job and one chosen
    next job, with no order of arrival, no deduplication against a list and no
    pruning.
45. (12) The executor's lifecycle (start, stale while running, cancel,
    supersede, fail, shutdown), its pinning, the publication and storing of
    results, and the job history are unchanged.
46. (12, as amended) The column worker and the idle scheduler keep their tasks
    and priorities; the column fill is a new lowest-priority task; the
    scheduler has the notion of a task with work it cannot step right now,
    completes it when its work is gone, and knows nothing of jobs; the manager
    and the store record each stored result's outcome in the index, the
    manager announces a changed outcome, and the store announces a record it
    could not write.
47. (12, as amended) The flow is one way: the demand layer chooses, the
    executor publishes, the listener writes the record or the store announces
    that it could not, the index notes the outcome and announces it, the
    record change drops cached column values and the demand layer's memos, the
    loaded-row refresh recomputes the values, and the demand layer sees the
    result or the failure through the inspection, the record set and the pair
    memory it always reads.
48. (12) What stays true: restoring is not requesting; the column worker's
    temporary copy never writes a stored result and never requests; a restored
    result is indistinguishable from a published one; the engine's threading
    rules; the plot widget's "no data" warning asks the engine directly.
49. (13) Test: checking a plot starts the visible sessions without a result;
    showing another session while it is checked starts it with no other
    action; hiding drops its waiting pair; unchecking drops all waiting pairs;
    the running job finishes and its result is stored.
50. (13) Test: a session made visible during column demand is the next job to
    start; within column demand visible sessions come before hidden loaded
    ones, and those before sessions not yet loaded.
51. (13) Test: enabling a column over a requested output wants every session
    without a result, loads unloaded ones a bounded number at a time as hidden
    pinned sessions, fills the column, ends with every session computed, not
    applicable or failed, and they leave the pool by ordinary eviction.
52. (13) Test: the column worker's behaviour and statistics are unchanged by
    column demand, and a stale record it deletes moves the pair into demand.
53. (13) Test: stored results are restored, not recomputed: enabling the
    column on a logbook whose sessions all have valid stored results creates
    no job; a stored rejection of a session not loaded is listed as failed
    with its reason after a restart without loading it; a column over a chain
    whose upstream record alone is stored is completed.
54. (13, as amended) Test: an input-determined failure is stored, listed among
    the failures and never run again; a job-level failure is listed among the
    failures as tried again at the next start, not run again in the run, and
    tried again after a restart.
55. (13, as amended) Test: a burst of input changes on a demanded session
    produces one job; progress counts the session from the first change.
56. (13) Test: applying a profile with such a column creates demand; start-up
    with a checked plot and no visible sessions creates none.
57. (13, as amended) Test: progress and failures reflect waiting, running, done
    and failed tracks of plots and columns alike; plot rows and column headers
    show nothing; no control to refresh or cancel a calculation exists.
58. (13) Test: a pending column cell is distinguishable from an unavailable
    one, is not written to the logbook index, and becomes the value when the
    record is written.
59. (13, as amended) Test: saves and bulk edits still precede the fill's loads;
    no result is computed from a file being rewritten; the fill works from its
    first load to its last result without any step that loads nothing and
    reports no progress of its own, the status bar showing the computations
    while it works, and the scheduler does not spin while the fill waits on a
    job.
60. (13) Test: the executor never holds more than the running job and one
    chosen next job; changing demand replaces the chosen next job.
61. (13) Test: the UI thread is not blocked by a running requested
    calculation, and the calculation runs below normal priority.
62. (13) Test: the existing tests of stored results and of the executor pass,
    those that asserted the refresh gesture, the queue order or the pruning of
    queued jobs rewritten to the demand rules.
63. (14, as amended) `docs/` describe the executor, the demand layer, logbook
    columns over requested results, what the user sees (no refresh, the status
    bar and the row warning, pending cells, failures) and where a column over a
    requested calculation stays unavailable or pending.

## Appendix G. The acceptance items of calculation refinements (601-662)

The testable statements of the specification "Calculation refinements", which
amends "Demand-driven requested calculations" (appendix F), one sentence each,
with the specification's section number in front. The specification numbers
no clauses; the numbers below are this list's, and item = 600 + the number
(items 601-662 of `tests/acceptance_map.txt`, the rows of section 9.7). Its
sections 1 (motivation) and 4 (terms) have no item; section 13's contracts
are clauses 51 and 52 and the amended clauses of appendix F. Clause 34 is
stated as settled when it was implemented (9.7 says how). Clauses 4, 12, 18,
24, 27, 31, 42, 44-51, 53, 54, 56, 57, 60 and 62 are stated as amended by the
specification "One status bar for background work" (appendix H); clauses 42
and 50 were stated as settled before.

1. (2) The principles of the demand-driven specification hold unchanged: what
   is switched on is the request, anything wanted is wanted at once, finished
   work is never wasted, background work never degrades the application, a
   failure is shown and never retried in a loop, each component's contract can
   be stated without naming the others.
2. (2) A fact is computed by the component that owns it and read by the
   others: each enabled column's closure and requested calculations and a
   row's display name by the session model, a record's reason by the logbook
   index, a failed record write by the result store.
3. (2) A component announces what it changes; no component infers another's
   change from a signal about something else.
4. (2, as amended) One rule, one path: plots and columns are classified and
   filed as candidates by one walk, the difference between them being data.
5. (2) What no product code uses is removed, unless a named later feature
   needs it, in which case it is kept and says so: the executor's cancel
   operation and job history stay for the jobs dock.
6. (3) Unchanged: the executor's lifecycle, the fusion kernel, the record
   format, the logbook index's other contents, and the column worker's
   behaviour and statistics.
7. (3) Unchanged: what is in demand, its priority, the input-settle wait, the
   bound on held sessions, and the one-way flow between the demand layer, the
   executor, the store and the column worker.
8. (3) The jobs dock is out of scope: the executor keeps its cancel operation
   and its job history for it; the column worker does not load sessions for
   the demand layer.
9. (5) A task registered as one that can wait (TaskDef::canStep) which the
   scheduler last reported active and which has no work any more is completed
   at the next tick whether or not it was stepped: its progress is reported
   one last time and its completion is called, not cancelled, before the next
   active task is reported or the scheduler goes idle; whoever takes a resting
   task's work away wakes the scheduler.
10. (5) A task that lost its work because it was cancelled completes once, as
    cancelled; a task unregistered while active is never completed.
11. (5) The other tasks (save, load, bulk edit, column work) complete through
    their step or by cancel at the same moments as before: they can lose work
    outside a step, so the rule applies to tasks that can wait only.
12. (5, as amended) The fill has no ending state and no step that loads
    nothing; it reports no progress of its own and keeps no high-water mark:
    the status bar shows the computations while it works, and a burst of
    computations after none starts its own count.
13. (5) The scheduler still learns nothing about jobs or demand.
14. (6) Recording a reason that differs from what the logbook index held emits
    the record-changed signal for that session and calculation, as a record
    write or removal does; an unchanged reason emits nothing, and a write or
    removal still emits exactly once.
15. (6) A reason taught to the index by a restore into the column worker's
    copy reaches the demand layer through that signal alone; the demand layer
    compares no reasons on the model's display change.
16. (6) The bulk edit announces its edit as a dependency change on both of its
    paths (a loaded session, and the temporary copy of one that is not
    loaded), and the demand layer observes no display change of the model: a
    bulk edit reaches it as an input change, and the column worker's display
    changes are never read.
17. (7) The result store announces a record write that failed, with the
    reason, for the session and calculation, after the index's record change
    of that pair; the result stays installed and nothing retries the write.
18. (7, as amended) A track whose record could not be written is failed with
    that reason, for plots and columns alike, whether the session is loaded or
    not, and is a failure of the recording, shown by the status bar's warning
    and the logbook row: a source is done only when none of its storable
    calculations is remembered so.
19. (7) Such a pair is not offered again and its session is not loaded again
    for it in the run until its inputs change, so the user's view is the same
    before and after an eviction.
20. (7) A later successful write of the pair's record clears the failure;
    nothing is persisted, so the next start tries the write again.
21. (8) The session model exposes, per enabled column, its static dependency
    closure and its requested calculations, current under the registrations at
    the moment of the call and valid between two column rebuilds.
22. (8) The demand layer reads each column's closure and requested
    calculations from the session model and computes neither; a column change
    reaches it through the model's reset alone; it still computes a plot's
    closure and requested calculations from the registry.
23. (8) The session model exposes one display name of a row: the loaded
    session's description, else the description the logbook index caches for
    the row, else the session id.
24. (8, as amended) The demand layer's failure entries and progress and the
    executor's job records name a session by that display name, for loaded
    rows, stubs and failed-load placeholders alike, and neither computes a
    name.
25. (9) The demand layer keeps no memory of its own offer: it is the only
    offerer, so the chosen next job is always its own, and when the choice
    finds nothing it is withdrawn.
26. (9) The demand layer does not compare a candidate with the chosen next job
    before offering it; it acts on the executor's answer, and an offer equal
    to the chosen next job keeps it as it is.
27. (9, as amended) A track has no settling flag and no job id; the per-source
    counts and lists are gone, and nothing the user sees depends on them.
28. (9) The executor has no idle signal, no queued signal, no query of both
    active jobs and no busy-period bookkeeping; its job model, its idle and
    running queries and its other signals carry the same facts.
29. (9, as amended) The executor keeps its cancel operation, with the
    caller's reason; the demand layer is its one product caller, for the
    running job of a recording switched off, and the jobs dock may call it
    too; its comment and the audit say so (amended by the specification of
    1501-1524).
30. (9) The executor's refusal kinds are unchanged; the demand layer offers
    upstream first, so a Blocked refusal is not expected from it, and the
    documentation says so instead of the code guarding for it.
31. (10.1, as amended) One walk over the session rows derives progress,
    failures, pending cells and candidates, for plots and columns at once,
    under one row stability guard, returning plain values; offers, withdrawals,
    holds, loads and signals happen after it.
32. (10.1) A loaded session is classified from the engine's blocker inspection
    of the source's names, combined alike for a plot's one name and a column's
    names, the running job, the pair memory and the settle wait; the combined
    report is memoized per session and source and dropped by an input change
    or publication, a load, a record change, a job's end, a reset or a
    registry change.
33. (10.1, as amended) A session that is not loaded is a track of columns
    only, classified in this order, after the exclusion of a session switched
    off for background computation, which is decided first from the index
    (amended by the specification of 1501-1524): no storable calculation, not
    applicable; a storable
    calculation remembered failed (a job, a load or a write that failed),
    failed with the remembered reason; every storable calculation remembered
    not applicable for that column, not applicable; every storable calculation
    with a record, done, or failed with the first recorded reason; a
    failed-load placeholder, failed ("The session file could not be loaded");
    otherwise waiting.
34. (10.1, as settled) Candidates are filed in the walk by tier: the focused
    session's plot pairs, the other visible sessions' plot pairs, then the
    column pairs of visible loaded sessions, then of hidden loaded sessions,
    each in row order; within a session source order, then blocker order; a
    pair once, in its first tier; never a pair with a remembered failure or
    refusal, the running job not asked to stop, or a pair of a settling
    session; a column track running or failed on one blocker files its other
    blockers, as a plot track does.
35. (10.1) The load candidates are the waiting, hidden, unloaded sessions that
    are not settling and not held, in row order, up to the bound.
36. (10.2) One memory of the run, keyed by pair (session, requested
    calculation), holds not applicable (the executor refused the pair, or a
    loaded column found its source not applicable) or failed with a reason (a
    job that ended failed, a result that is never stored, a load that failed,
    a record write that failed); there is no memory per cell, and every
    verdict is derived from the engine or the record set and this memory.
37. (10.2) A column's not-applicable verdict holds for that column only: it
    keeps its session from being loaded again for that column, and it never
    makes another source's track not applicable nor keeps a pair from being
    offered.
38. (10.2) The memory is cleared for a session by a relevant input change, for
    a pair by a record change of that pair, for everything by a registry
    change; a model reset forgets the pairs of sessions that no longer have a
    row; nothing is persisted.
39. (10.2) A bulk edit of a session clears what the run remembered of it,
    through the dependency change it publishes, for a loaded session and for
    one that is not loaded alike; nothing else clears a session's facts, the
    column worker's display changes in particular; a load of the session that
    succeeds forgets its failed-load facts.
40. (10.2) A session found not applicable, a job-level failure, a failed load
    and a failed write are not loaded or offered again after an eviction, a
    sort, or the column worker's processing of the stub.
41. (11) The demand layer stays one component with one contract towards the
    views and the executor, divided into parts with contracts of their own
    (the presentation values, the fill, the settle clock and the reconciler);
    the fill and the settle clock expose nothing of the walk, and the walk
    calls neither the executor nor the session model's loading and pinning.
42. (11, as amended) The fill keeps at most the bound of hidden sessions loaded
    and pinned for column demand, releases a hold when its session has no
    pending cell or no loaded row, loads the next candidate when a hold is free
    and the scheduler steps it, reports no progress of its own, and holds
    nothing once the executor is shut down or the component goes.
43. (11) The settle clock holds the per-session deadlines of the input-settle
    wait and one timer for the earliest, answers whether a session is settling
    and when the next wait ends, and calls its owner when a wait ends.
44. (12, as amended) A plot row and a logbook column header show nothing about
    computing: no glyph, no label, no count and no room reserved for one, so
    they look as any other; progress is shown once, in the status bar, and a
    failure on the recording's logbook row.
45. (12, as amended) The status bar's hover carries the numbers and the running
    recording's step, and the warning's hover the failures.
46. (12, as amended) No working-indicator clock exists: nothing repaints by
    itself while work runs, and the status bar's progress bar needs no clock of
    its own.
47. (12, as amended) The status bar shows "Computing results: k / n" while the
    fill is the active task, with no cancel button, distinct from the column
    worker's "Computing columns: k / n"; the fill is not cancellable, and
    saves, loads, bulk edits and column work still come first.
48. (12, as amended) A stored result that could not be written is a failure of
    the recording, listed with its reason by the status bar's warning and on
    the recording's logbook row.
49. (12, as amended) Nothing else the user sees changes: pending cells, a
    failure's meaning, and the absence of any refresh or cancel for
    computations.
50. (12, as amended) One drawing of the warning glyph exists, the style's
    standard warning icon, used by the status bar and the logbook row; no view
    shares a glyph painting or a hover helper, and each view that holds the
    demand layer learns of its end itself.
51. (13, as amended) The demand layer remains widget-free, the only offerer and
    observed by nothing; the status bar and the logbook present its values and
    decide nothing.
52. (13) The flow stays one way: the demand layer chooses; the executor
    publishes; the store writes the record or announces that it could not; the
    index notes the outcome and announces it; the record change drops the
    cached column values and the demand layer's memos; the demand layer sees
    the result, or the failure, through what it always reads.
53. (14, as amended) Test: a task with work it cannot step loses its work
    without a step and is completed once, not cancelled, after a final progress
    report, before the next active task is reported or the scheduler goes idle;
    the existing tasks complete at the same moments as before; the fill works
    from its first load to its last result without any step that loads nothing
    and reports no progress of its own, the status bar showing the
    computations.
54. (14, as amended) Test: a reason recorded by a restore into the column
    worker's copy reaches the demand layer through the manager's record-changed
    signal alone: a stored rejection of a session that is not loaded is listed
    among the failures with its reason after the worker's pass, and no reason
    comparison happens on the model's display change.
55. (14) Test: a record write that fails is a shown failure: listed as failed
    with the reason while the session is loaded and after its eviction, not
    offered again in the run, cleared by a later successful write, tried again
    at a restart; no session is loaded twice for it.
56. (14, as amended) Test: the demand layer reads each column's closure and
    requested calculations from the session model, and a column change reaches
    it through the model's reset alone; the failure entries, progress and the
    job records name a session by the model's display name, for loaded rows,
    stubs and failed-load placeholders alike.
57. (14, as amended) Test: plots and columns are classified and filed by one
    walk, which computes progress and failures and announces them only when
    they change, and the acceptance items of the demand-driven specification on
    demand, priority, the settle wait, failures, not applicable, chains,
    unloaded sessions and the pair memory pass unchanged in what they assert.
58. (14) Test: every case the per-cell memory covered is covered by a pair
    fact: a session found not applicable, a job-level failure, a failed load
    and a failed write are not loaded or offered again after eviction, a sort
    or the column worker's pass; a bulk edit makes the session applicable
    again; a record change of the pair, an input change of the session and a
    registry change clear what is remembered.
59. (14) Test: no product code reads the removed signals, queries and fields;
    the executor never holds more than the running and the chosen next job;
    the chosen next job is withdrawn when demand no longer wants it.
60. (14, as amended) Test: the plot row shows nothing; no clock exists; the
    status bar shows the fill as the computations; a stored result that could
    not be written is listed in the logbook row's and the status bar's hover.
61. (14) The audit's demand and gestures groups keep the removed and moved
    names out, and a rule forbids a second computation of a column's requested
    calculations outside the session model and the registry.
62. (15, as amended) `docs/` and `tests/README.md` describe the executor's
    signals and queries (cancel with the caller's reason, the demand layer its
    one product caller, for the running job of a recording switched off, and
    the jobs dock may call it too), the one walk, the one memory, the parts of
    the demand layer, the fill's completion, the failed write, the session
    model as the source of column knowledge and display names, and the status
    bar and the row warning; not the indicator, the clock or the fill's
    progress text (amended by the specification of 1501-1524).

## Appendix H. The acceptance items of one status bar for background work (701-754)

The testable statements of the specification "One status bar for background
work", which amends "Sensor fusion as an explicit calculation, with
plot-driven background jobs" (appendix B), "Sensor fusion: segmented
initializer, stopping rule and IMU model" (appendix C), "Storing requested
calculation results with the session" (appendix D), "Demand-driven requested
calculations" (appendix F) and "Calculation refinements" (appendix G), one
sentence each, with the specification's section number in front. The
specification numbers no clauses; the numbers below are this list's, and
item = 700 + the number (items 701-754 of `tests/acceptance_map.txt`, the rows
of section 9.8). Its sections 1-4 (motivation, principles, scope, terms) have
no item, their principles being carried by the clauses of the sections that
apply them. Clauses 45-53 are its section 13 tests, one per bullet, and clause
54 its section 14. Clauses 3, 25 and 31 are stated as settled (9.8 says how).

1. (5) The main window has a status bar, always present, empty when nothing is
   in progress and nothing has failed; it never appears or disappears, so the
   layout does not jump.
2. (5) The activity area presents the scheduler's active task with its existing
   label ("Saving sessions", "Loading sessions", "Updating sessions",
   "Computing columns") and its progress as the scheduler reports it.
3. (5, as settled) The computations are one item, "Computing results": the
   sessions with a track waiting or running in any source, plots and columns
   alike, each once, shown done out of the high-water mark since the count was
   last zero; the item exists while the count is above zero.
4. (5) The computations' hover names the recording being computed and the
   running job's latest progress text.
5. (5) The fill is never an item: its count is contained in the computations',
   and while it is the scheduler's active task the computations are shown.
6. (5) The fill keeps its scheduler registration and contract (completed when
   its work is gone, not cancellable) and reports no progress of its own.
7. (6) The shown item is the scheduler's active task when it is an item,
   otherwise the computations, which return to the label when the task ends.
8. (6) The hover lists every item in progress, the shown item first, each with
   its label and count, and for the computations the recording and its step;
   nothing in it is a control.
9. (6) The cancel control is present exactly while the shown item is a
   scheduler task registered as cancellable, and cancels that task; it is
   absent for saving and for the computations.
10. (6) With no item in progress the activity area is empty; the warning is not
    part of it and stays.
11. (7) A warning is shown beside the activity area while any recording has a
    current failure: the warning glyph and the number of recordings that could
    not be computed, counting recordings, not pairs.
12. (7) The warning is shown while computations continue, so a failure found
    early is visible at once, and stands alone once nothing is computing.
13. (7) Its hover lists the recordings in session-model row order, each with
    the failed calculation and the reason, says which are tried again at the
    next start, and lists at most ten, then how many more.
14. (7) It persists across restarts with nothing new written: a stored
    rejection is counted at the next start without loading the recording, once
    its source is restored and the column worker's pass has reported the record
    set.
15. (7) A failure that is not stored is tried again at the next start and shows
    again only if it fails again; a retry that succeeds clears it.
16. (7) It clears as the pair memory clears (a record change of the pair, an
    input change of the session, a registry change, the session losing its row)
    or when the last source over the calculation is switched off; nothing else
    clears it and nothing dismisses it.
17. (7) A failure of a calculation no plot or column wants is not current and
    is not counted.
18. (8) A recording with a current failure shows one warning glyph on its
    logbook row, right after the text of its first cell, whatever column that is.
19. (8) The glyph's hover lists the recording's failed calculations with their
    reasons, in the form of the status bar's hover for that recording.
20. (8) A row that is not loaded shows the glyph from the record set, without
    loading.
21. (8) The cells over the failed calculation are blank; the pending cell is
    unchanged.
22. (8) The glyph takes room in the first cell's text rectangle only while it
    is shown: a logbook without failures looks exactly as before.
23. (9) The working indicator and the warning badge on plot rows and column
    headers go, with the room reserved for them and the header's hover: such a
    row or header looks as any other.
24. (9) The working-indicator clock goes: its creation, its following of the
    demand layer, its place in the application context and the views' repaint
    on its frames.
25. (9, as settled) The shared glyph painting and its hover helper go; one
    drawing of the warning glyph exists, the style's standard warning icon,
    used by the status bar and the logbook row.
26. (9) The logbook's progress line, its cancel button, the dock feature's
    scheduler wiring and the fixed minimum size hint go.
27. (9) The fill's separate progress text and its display progress total go.
28. (9) The demand layer's per-source state, its queries, its change signals
    and the working id lists go; the audit's demand group keeps the names out.
29. (10) Progress: the sessions with a track waiting or running in any source,
    the high-water mark, the running recording's display name and step,
    announced when any of them changes; the step changes without a pass.
30. (10) Failures: for each session with a current failure, in row order, its
    display name and failed pairs, each with the calculation's title, the
    reason and whether it is tried again at the next start, announced when the
    set or an entry changes; asked for the whole list or for one session.
31. (10, as settled) Pending cells are unchanged, with an announcement of their
    own per column.
32. (10) The three values are computed in the one walk from the tracks it
    classifies and from the pair memory, with no second pass and no second key;
    counts no view reads are not kept.
33. (11) The status bar is the one place background work is shown: a label
    naming the work and its count, a bar showing the same, a hover listing
    everything in progress, a cancel control only for work that can be stopped,
    and nothing when the application is idle.
34. (11) The warning says how many recordings could not be computed for as long
    as that is true, restart or not, and its hover which and why; it stands
    alone when nothing is computing.
35. (11) The logbook row of a recording that could not be computed carries one
    glyph whose hover says what failed and why; its cells over the failed
    calculation are blank; a cell still to come shows the pending mark.
36. (11) Plot rows and column headers show nothing about computing; a track
    still to come is absent from the plot until it arrives, and the plot widget
    does not warn about it.
37. (11) Nothing offers a refresh or a cancel for computations.
38. (12) The main window owns the status bar and the component that fills it,
    created from the application context like the dock features and not a dock;
    the component reads the scheduler's and the demand layer's values, asks the
    scheduler to cancel, and decides nothing.
39. (12) The scheduler keeps its contract, knows nothing of jobs or demand and
    reports the fill as a task, which the status bar maps to the computations;
    it gains one read-only query, whether any task has work, so that a test can
    wait for background work to end.
40. (12) The demand layer stays widget-free, the only offerer and observed by
    nothing that changes what it does; its presentation surface is progress,
    failures and pending cells.
41. (12) The logbook view presents the row glyph from the demand layer's
    per-session failures and the pending cell, and no task progress; its dock
    feature connects nothing of the scheduler.
42. (12) The plot list presents nothing of the demand layer.
43. (12) The application context carries the demand layer for the logbook and
    the status bar, and no clock.
44. (12) The flow stays one way, ending in the demand layer presenting progress
    and failures as values that the status bar and the logbook read.
45. (13) Test: the activity area presents the scheduler's tasks with the
    existing labels and the scheduler's counts, and presents the computations
    as one item counting sessions with a waiting or running track across plots
    and columns, out of the high-water mark, which resets when the count
    reaches zero; the fill is never the shown item, and while it is the
    scheduler's active task the computations are shown.
46. (13) Test: with a scheduler task and a computation in progress, the task is
    the shown item and the computations return when it ends; the hover lists
    both with their own counts and the recording being computed with its step.
47. (13) Test: the cancel control is present exactly while the shown item is a
    cancellable scheduler task, and cancels that task; it is absent for saving
    and for the computations.
48. (13) Test: the warning is shown while any session in demand has a current
    failure, counts recordings, lists them in row order with calculation and
    reason, says which are tried again at the next start, caps the list at ten,
    is shown alongside the computations while they continue, stands alone once
    they end, and is absent when nothing has failed.
49. (13) Test: the warning persists through a restart for a stored rejection:
    with the source restored and the record set reported, the recording is
    counted without being loaded; a failure that is not stored is absent after
    a restart until it is tried again and fails again; a retry that succeeds
    leaves nothing shown.
50. (13) Test: the warning clears when the pair memory clears or the last
    source over the calculation is switched off, and nothing dismisses it by
    hand.
51. (13) Test: the logbook row of a recording with a current failure shows one
    glyph right after the text of its first cell, for a loaded row and for one that is
    not loaded; its hover lists the failed calculations with reasons; the cells
    over the failed calculation are blank; a row without a failure takes no
    room for the glyph; the pending cell is unchanged.
52. (13) Test: removed presentation: no plot row or column header paints a
    glyph or reserves room for one; no clock exists; the logbook view has no
    progress line and no cancel button and receives no scheduler signal; no
    product code reads the removed per-source state, queries, signals or lists;
    the audit's demand group keeps their names out.
53. (13) Test: the demand layer's progress and failures are computed in the one
    walk and announced only when they change; the acceptance items of the
    earlier specifications on demand, priority, the settle wait, failures, not
    applicable, chains, unloaded sessions and the pair memory pass unchanged in
    what they assert.
54. (14) `docs/` and `tests/README.md` describe the status bar, the row
    warning, the demand layer's progress, failures and pending cells and what
    stays visible after a restart, and no document names the removed surface.

## Appendix I. The acceptance items of sensor fusion plots, attitude and the orientation attribute (801-863)

The testable statements of the specification "Sensor fusion plots, attitude
and the orientation attribute", which amends the schema and
calculation-engine items (appendix A), "Sensor fusion as an explicit
calculation, with plot-driven background jobs" (appendix B) and
"Demand-driven requested calculations" (appendix F), one sentence each, with
the specification's section number in front. The specification numbers no
clauses; the numbers below are this list's, and item = 800 + the number
(items 801-863 of `tests/acceptance_map.txt`, the rows of section 9.9). Its
sections 1-4 (motivation, principles, scope, terms) have no item, their
principles being carried by the clauses of the sections that apply them; the
two statements of its section 3 that a test can observe are clauses 4 and 40.
Clauses 54-62 are its section 13 tests, one per bullet, and clause 63 its
section 14. Clauses 2, 7, 8, 10, 14, 22, 26, 36, 39 and 50 are stated as
settled (9.9 says how). Clauses 47 and 61 are stated as amended by the
specification "The fused state at every IMU sample" (appendix J), and again by
the specification "The documented noise model and the accuracy, part 1"
(appendix K): it adds the four configuration inputs, and the fit's values,
diagnostics, algorithm string, goldens and stored results are its own.
Clauses 1, 2, 42, 54, 62 and 63 are stated as amended by the same
specification's plots: the category held twelve plots, the eight and then
the four accuracy plots, which have no GNSS counterpart and are named as the
GNSS accuracy plots are. Clauses 1, 2, 42, 54, 62 and 63 are stated as
amended again by the specification "Fused speed plots" (appendix Q): the
category holds fifteen plots, the three fused speeds after Elevation, named,
united and typed as the GNSS speeds. Clause 3 is stated as amended by the
same specification: the fused down velocity is drawn as Vertical speed.

1. (5, as amended) The "Sensor fusion" category holds exactly fifteen plots,
   in this order: Elevation, Horizontal speed, Vertical speed, Total speed,
   Horizontal acceleration, Vertical acceleration, Along-track acceleration,
   Cross-track acceleration, Heading, Pitch, Roll, Heading accuracy, Tilt
   accuracy, Horizontal acceleration accuracy, Vertical acceleration accuracy
   (the three speeds since the specification of 1601-1612, the last four
   since the specification of 1001-1065).
2. (5, as settled, as amended) Each of the first eleven is named, united and
   typed as its GNSS counterpart (Elevation, the three speeds since the
   specification of 1601-1612, the four accelerations, Heading as Course),
   Pitch and Roll as angles, so that the two overlay; a fused plot's colour
   differs from its counterpart's; the four accuracy plots have no GNSS
   counterpart and are named as the GNSS accuracy plots are.
3. (5, as amended) The fused north, east and down position, north and east
   velocity, north and east acceleration, roll, pitch, yaw and quaternion
   plots are removed, and the "GNSS (Local frame)" category with them; the
   fused down velocity is drawn as Vertical speed since the specification of
   1601-1612.
4. (5) Every measurement behind a removed plot still exists and is computed as
   before: the local frame feeds the fit and a column, and the fit's channels
   are its record, read by a column kept from before, the stored result and
   plug-ins.
5. (5) Applying a profile enables the listed plots the application has and
   ignores the rest silently (no message, no log, no failure); nothing rewrites
   a profile.
6. (6) Each derived quantity is an on-demand calculation over the fit's
   outputs, registered with the fit: blocked by it until it publishes, it
   appears with it and never starts one.
7. (6, as settled) Elevation is the local origin's height above mean sea level
   minus the fused down position, above the ground elevation as the GNSS
   elevation is, so the two overlay; it is unavailable when either attribute is
   not a number.
8. (6, as settled) Vertical acceleration is the fit's own fused down
   acceleration, positive down like the GNSS one; no second calculation
   publishes it.
9. (6) Along-track and cross-track acceleration are the GNSS definitions
   applied to the fused velocity and acceleration, wind-corrected, so their
   signs and the meaning of "track" agree.
10. (6, as settled) One definition of the track-relative accelerations and of
    the wind rule serves both categories, and the GNSS values are unchanged.
11. (7) The orientation's forward and up axes fix the body frame, right being
    their cross product; the body-to-device rotation is a constant signed axis
    permutation, a proper rotation for each of the 24 pairs, never a
    reflection.
12. (7) Heading, pitch and roll are the aircraft Euler angles of the fit's
    quaternion composed with that rotation: heading the forward axis's
    direction clockwise from north, pitch its elevation above the horizontal,
    roll the rotation about it, positive right side down.
13. (7) They are computed when read, from the quaternion channels and the
    orientation attribute, never by the fit.
14. (7, as settled) Heading is unwrapped by the application's one unwrap rule,
    the course's, and measured from north: a compass heading, not referenced
    to the course reference (GNSS course is relative to its reference, so the
    two are not meant to overlay); the attitude reads the quaternion and the
    orientation only, so heading, pitch and roll are available for every
    fitted recording.
15. (7) Pitch and roll are reported in their natural ranges, pitch from -90 to
    90 degrees and roll from -180 to 180.
16. (7) Where the forward axis is vertical, heading and roll are not defined
    and the derivation reports what the standard formulas give.
17. (7) A change of the orientation attribute recomputes the angles through
    ordinary invalidation and never refits.
18. (8) The orientation is a token naming the forward and the up axis in a
    fixed form the fusion module owns (the default `+y,+z`); the 24 valid
    pairs, and only they, are its choices, labelled "forward +y, up +z" and so
    on, the default first.
19. (8, as amended) Its constant default, forward +y and up +z, is a
    calculated attribute, so a recording without a stored orientation has one
    with nothing written to its file; a new import stores the orientation of
    the Import preferences (the same default), a fact of the import, editable
    afterwards.
20. (8, as amended) A stored value wins over the default; nothing an edit can
    carry removes a stored orientation.
21. (8) One orientation type in the fusion module owns the vocabulary (the axis
    pair, token, label, body-frame rotation, default and enumeration); the
    derivation parses with it, the attribute's choices come from it, and
    nothing else spells a token.
22. (8, as settled) The fusion registration registers the attribute's
    definition once per process (key `_ORIENTATION`, "Orientation", category
    "Session", a choice attribute, editable) with the derivation that reads it;
    the key is part of the session file's vocabulary.
23. (8) It is edited as any editable attribute: a logbook column the user can
    add, not among the default columns, edited in place, and set for the
    selected sessions from the context menu.
24. (8, as amended) Both editors offer the 24 orientations and nothing else: no
    free text and no entry that removes the stored value.
25. (8) A value outside the list is refused by the model, the in-place editor
    and the bulk edit.
26. (8, as settled) A stored token outside the list (a hand-edited file) is
    kept and shown as written, and heading, pitch and roll are unavailable for
    it until it is changed or removed.
27. (9) The attribute registry's format types gain a fifth, choice, whose
    definition carries its allowed values, each a stored token and a display
    label.
28. (9) A choice is displayed and sorted by its label.
29. (9) In-place editing of a choice gives a list editor in place of the line
    edit.
30. (9) The context menu's "Set ..." action offers a list in place of the text
    prompt.
31. (9) The bulk edit sets the chosen token for the selected sessions, loaded
    or not.
32. (9, as amended) A token outside the list, an empty and an invalid value are
    refused on every edit path; no edit removes a stored attribute.
33. (9) Nothing about the type is specific to orientation: any choice
    definition gets all of it.
34. (10) A constant default is a registered calculation with no inputs whose
    one output is the attribute: it runs once and is cached, a stored value
    wins, setting or removing a stored value invalidates what read the
    attribute, and a dependent follows every change.
35. (10) One helper registers a constant default for an attribute key and a
    value, beside the exit-time defaults, so that every constant default is
    found by one search; the orientation default uses it from the fusion
    registration.
36. (10, as settled) The seven constant defaults of the SP and WS-P
    calculations are registered through the same helper, with their values.
37. (10) The importer stores only what is a fact of the import; anything that
    stands in for a value the user has not set is a calculation, derived where
    possible, constant otherwise.
38. (10) Wind north and east are constants of zero: a recording without stored
    wind reads zero, one with stored wind keeps it, and the importer no longer
    writes them.
39. (10, as settled) The logbook's legacy backfill no longer writes wind
    either; it adds jumper mass and planform area only.
40. (10) Jumper mass, planform area and the fixed ground elevation stay
    creation defaults of the importer (section 3).
41. (10, as amended) A stored attribute wins even when invalid or empty, so no
    edit path stores a blank as a way back to a default; the engine's removal
    of a stored value falls back to the calculation.
42. (11, as amended) The plot list shows "Sensor fusion" with fifteen plots,
    the eleven named as the GNSS plots (the three fused speeds after
    Elevation since the specification of 1601-1612) and the four accuracies
    (since the specification of 1001-1065) named as the GNSS accuracy plots,
    and no local-frame category; checking a fusion plot starts the fit as
    before, the status bar shows it, and a stored fit draws at once.
43. (11) The attitude plots are in the aircraft convention for the mount the
    orientation describes, heading continuous through turns.
44. (11, as amended) The orientation column, once added, shows "forward +y, up
    +z" for every recording not set and the chosen label for one that is;
    editing offers the 24 orientations; changing it redraws the attitude plots
    without a fit; the Import preferences page offers the same list for new
    imports.
45. (11) Profiles that name removed plots apply without complaint.
46. (11) Wind reads zero where nothing was stored, as before.
47. (12, as amended) The fusion plots leave the fusion kernel and the fit
    calculation as they are (inputs, outputs, diagnostics, algorithm string),
    except that the specification of 1001-1065 adds the four configuration
    inputs, and derive what they add from the fit's published outputs, whose
    values, diagnostics and algorithm string are those of the specification
    of 1001-1065, with its goldens and stored results.
48. (12) GTSAM stays confined to the fusion kernel, the new fusion files
    include neither GTSAM nor Eigen, and the rules on the fusion tooling hold.
49. (12) The fusion registration gains the derived kinematics, the attitude
    derivation, the orientation type and the orientation attribute with its
    constant default; each derivation is an ordinary on-demand calculation with
    declared inputs, the attitude's including the orientation attribute.
50. (12, as settled) The fusion library reaches nothing of the core library:
    the attribute registry lives in the model library, the constant-default and
    track helpers are header-only, and the calculations never name the fusion
    library.
51. (12) The attribute calculations hold the constant-default helper and the
    wind defaults; the importer holds neither.
52. (12) The plot registry loses the removed plots and the local-frame
    category; the profile rule is the plot model's, one function the profile
    bridge calls.
53. (12) The demand layer is untouched: a derived plot's blockers lead to the
    fit through the chain it already follows, and heading, pitch and roll are
    filled by one job.
54. (13, as amended) Test: the plot list has the fifteen fusion plots (the
    four accuracies since the specification of 1001-1065, the three fused
    speeds after Elevation since the specification of 1601-1612) and no
    local-frame plots; every fusion plot is explicit-backed (waits on the fit) as the
    seventeen were; an existing column over a removed plot's measurement still
    computes, and the export of the fit, its stored record, still holds the
    measurement.
55. (13) Test: applying a profile that names a removed plot enables its other
    plots and ignores that one, silently.
56. (13) Test: elevation equals the origin height minus down minus the ground
    elevation, and is unavailable without a ground elevation; vertical
    acceleration is the down acceleration; along-track and cross-track equal
    the GNSS definitions applied to the fused velocity and acceleration, on
    synthetic data with known answers.
57. (13) Test: attitude, on synthetic quaternions with known answers: for the
    default orientation, heading, pitch and roll of a level, north-facing body
    are zero; a known heading, pitch and roll come back; heading unwraps
    through a full turn and is measured from north, unchanged by a GNSS track
    or a stored course reference of any kind and available without GNSS data;
    a different orientation (a side mount) changes the angles as the axis
    permutation predicts; the rotation is proper for all 24 pairs.
58. (13, as amended) Test: the orientation attribute: every recording reads the
    default without a stored value and without any write to its file; a stored
    token wins and the attitude recomputes without a fit; a token outside the
    list, an empty and an invalid value are refused by the model, the editor
    and the bulk edit; the labels and tokens come from one enumeration of 24; a
    new import stores the preference's orientation.
59. (13) Test: the choice type: display and sort by label; the in-place editor
    and the context menu offer the list; the bulk edit sets the token for the
    selected sessions.
60. (13) Test: constant defaults: the helper registers a calculation that the
    engine serves as a default (the existing engine test covers the mechanism);
    wind north and east read zero for a recording without stored wind, keep a
    stored value, and the importer no longer writes them.
61. (13, as amended) Test: the fusion plots leave the fit as the golden
    fixtures and the stored-result tests hold it, against the goldens and the
    algorithm string of the specification of 1001-1065.
62. (13, as amended) Test: the audit keeps the removed plot names out of the
    registry, and the documents describe the fifteen plots (the four
    accuracies since the specification of 1001-1065, the three fused speeds
    after Elevation since the specification of 1601-1612), the attitude
    convention, the orientation attribute and the rule for defaults.
63. (14, as amended) `docs/` describes the fifteen fusion plots (the four
    accuracies since the specification of 1001-1065, the three fused speeds
    after Elevation since the specification of 1601-1612) and the derived
    quantities, the attitude convention with the orientation attribute, its
    default and its limits (it describes the mount, not the wearer's posture;
    a forward axis pointing straight up or down makes heading and roll
    meaningless), the choice type and the orientation in the session file,
    the rule for defaults and its helper, and that wind is no longer written
    at import; no document describes a removed plot or the "GNSS (Local
    frame)" category.

## Appendix J. The acceptance items of the fused state at every IMU sample (901-940)

The testable statements of the specification "The fused state at every IMU
sample", which amends "Sensor fusion plots, attitude and the orientation
attribute" (appendix I), one sentence each, with the specification's section
number in front. The specification numbers no clauses; the numbers below are
this list's, and item = 900 + the number (items 901-940 of
`tests/acceptance_map.txt`, the rows of section 9.10). Its sections 1-4
(motivation, principles, scope, terms) have no item, their principles being
carried by the clauses of the sections that apply them; the statements of its
sections 2 and 3 that a test can observe are clauses 12, 16, 20, 25, 27 and 30.
Clauses 31-39 are its section 10 tests, one per bullet, and clause 40 its
section 11. Clauses 4, 5, 10, 11, 14, 15, 17, 18, 19, 21, 22, 24, 29, 31 and
34-37 are stated as settled (9.10 says how). Clauses 1, 10, 14, 15, 21, 25,
30 and 35 are stated as amended by the specification "The documented noise
model and the accuracy, part 1" (appendix K): it changes the algorithm string
again, and the noise model the pass reads, under which the bound on the
published acceleration also includes the kink of the rotated reading at a fix
(the kernel has not changed; under the datasheet's noise the corrections are
small enough that this term shows); and it adds the scale state, so the pass
integrates the readings divided by the fitted scale factors, its step
corrections and the published acceleration take them so divided, and it
holds the scale factors with the states and the biases (1, 10, 14, 25); and
its accuracy, so the fit publishes the uncertainty among its channels (12),
and the plot registry gains the four accuracy plots (30). Clause 12 is stated
as amended again by the specification "Fused position and speed accuracy"
(appendix R): the fit publishes thirty-three channels, the position and
velocity covariance blocks among them.

1. (5, as amended) For every fix interval of a fit that succeeds, the pass
   integrates gyro and accelerometer together from the fitted state at the
   interval's first fix with the biases and scale factors the fit assigns to
   that interval: the attitude advances with the gyro, the reading, divided
   by the scale and bias removed, is rotated by it and gravity added, and
   velocity and position advance with the result.
2. (5) The integration is the fit's own: its edges are every IMU sample in the
   interval and the two fix times, with the same midpoint interpolation of the
   readings and the same per-step noise as the fit's IMU factors.
3. (5) The pass accumulates at every edge the covariance of the noise the steps
   so far have added, from the transition and noise of the solver library's own
   preintegration update; at the second fix it is the covariance of the fit's
   IMU factor for the interval.
4. (5, as settled) The mismatch is the fitted state at the interval's second
   fix in the local coordinates of the forward state there, nine components;
   not the IMU factor's residual, which the library takes the other way round
   and whose negative misses the fitted end.
5. (5, as settled) The correction at each edge is the conditional mean of the
   step chain given both ends, linearized about the forward states, computed in
   the preintegration's tangent coordinates, where the covariance lives, and
   mapped into each forward state's local coordinates by the Jacobian of the
   library's retraction; the formula applied literally in the local coordinates
   is wrong at first order under rotation.
6. (5) The share is nothing at the first fix and the whole mismatch at the
   second, one nine-component correction applied jointly, so that an attitude
   share turns the direction of every later reading.
7. (5) Where the per-step noise is uniform the velocity share grows with
   elapsed time; a step that carries more noise takes more of the mismatch.
8. (5) The corrected state is the forward state with its share applied by the
   retraction that inverts the local coordinates; at the second fix it is the
   fitted state, exactly.
9. (5) The corrections are computed once, about the forward states, and applied
   once; the ends are exact by construction, and the linearization costs only
   the split inside the interval, within the equivalence test's bound.
10. (5, as settled, as amended) A step correction for every step: the
    corrected velocity change over the step length, less the mean of the
    readings at its two edges, divided by the scale, bias removed, each
    rotated by the corrected attitude at its own edge, less gravity (at a fix
    the reading is the one the integration uses there).
11. (5, as settled) Where the mismatch is zero the pass is the forward
    integration; the step corrections are zero where the recording does not
    rotate, and under rotation they are minus the integration's rotation lag.
12. (6, as amended) The fit publishes the thirty-three channels of the fusion
    document's section 4, the uncertainty among them since the specification
    of 1001-1065 and the position and velocity covariance blocks since that
    of 1701-1717, on the same time axis.
13. (6) At every IMU sample the attitude (the quaternion, and roll, pitch and
    yaw from it) is the corrected attitude at the sample's edge, and velocity
    and position are the corrected state there.
14. (6, as settled, as amended) The acceleration is the sample's own reading,
    divided by the fitted scale, bias removed, rotated by the corrected
    attitude, plus gravity, plus the mean of the
    corrections of the steps beside its edge (a step beside a fix being the
    part-step in that fix interval); only a sample exactly on the first fix has
    one such step, since the last fix is never published; where the corrections
    are negligible it is the rotated reading.
15. (6, as settled, as amended) Integrated by the kernel's rule, the
    published acceleration reproduces the published velocity change over any
    run of samples, the whole fitted interval included, to within the spread
    of the corrections at its ends and at the fixes inside it plus, for a
    sample step that contains a fix, the difference between the part-steps'
    trapezoids of the rotated reading and the sample step's (the kink at the
    fix), the bound the fusion document's section 8 states; not step by step.
16. (6) The time axis is the IMU samples in the half-open interval from the
    first fix to the last, whatever the GNSS rate, a GNSS rate above the IMU's
    included: no fix time added, nothing resampled, a sample on a fix published
    once.
17. (7, as settled) The success diagnostics' account of the dense output names
    the reconstruction (`dense_output`, in place of the previous key), and
    `limitations` no longer denies that the output is the IMU-rate posterior;
    the documentation quotes both texts.
18. (7, as settled) The success diagnostics report the largest step correction
    (`max_step_correction_m_s2`), the middle of its step in seconds since the
    epoch (`max_step_correction_time_s`) and the largest velocity mismatch of
    any interval (`max_velocity_mismatch_m_s`), beside
    `max_endpoint_correction_deg`, which stays.
19. (7, as settled) The diagnostics of a rejected or failed fit are unchanged
    but for the algorithm string, which changes for every outcome (the
    specification's section 8).
20. (8) The record stores and restores the same channels, with the new values,
    in the record format as it is.
21. (8, as settled, as amended) The algorithm string changes (to
    `batch-temperature-bias-v4` with the reconstruction, to `v5` with the
    mid-step rotation of the accelerometer reading that followed it, and to
    `v6` with the specification of 1001-1065), so a fit stored under the
    previous one is stale at its recording's next load, by the existing
    validity rules.
22. (8, as settled) After the change every stored fit is computed again once,
    when something switched on needs it, not all at the next start (a stale
    record is deleted at its recording's load), and the status bar shows it as
    any computation.
23. (8) The goldens are captured again from the changed kernel with the
    existing capture tool, and the golden tests compare against them.
24. (8, as settled) Every fixture's time axis is unchanged: checked once, at
    the re-capture, each success fixture's `_time` byte for byte against the
    previous capture (section 11), and permanently against the IMU samples of
    its fitted interval.
25. (9, as amended) The fit is unchanged by the pass (the graph, the noise
    model, the initializer, the solver and the stopping rule; the
    specification of 1001-1065 changes the noise model, the damping ceiling
    and the stopping rules, and adds the scale state to the graph), and so
    are its states at the fixes, its biases and its scale factors: the pass
    reads the converged solution (states, biases and scale factors) and
    changes nothing in it.
26. (9) The pass replaces the kernel's linear reconstruction, which no code,
    test, golden or document names.
27. (9) The step model is read from the library's preintegration as it
    advances, never written again: one integration call and one author of the
    per-step covariance, and the tests check the pass's covariance against the
    factor's.
28. (9) GTSAM stays confined to the kernel, and the rules on the fusion tooling
    hold.
29. (9, as settled) The pass runs after the fit's last iteration, inside the
    same cancellable job, and is not a cancellation boundary.
30. (9, as amended) The fit calculation publishes the same channels and its
    result version follows the algorithm string; the registration, the plot
    registry and everything above them are unchanged by the reconstruction;
    since the specification of 1001-1065 the registration also declares the
    four configuration inputs and registers their constant defaults, and the
    plot registry gains the four accuracy plots.
31. (10, as settled) Test: equivalence: against a graph with a state at every
    edge, one-step IMU factors with the same per-step covariance and interval
    bias, and the fix states and biases held at the fit's values, the
    reconstruction agrees within a tolerance stated against the mismatch, on
    `coarse_maneuver` and on a rotating recording; a reference with free fix
    states and biases drifts along the unobservable heading, so that no such
    tolerance exists for it.
32. (10) Test: on every success fixture, the corrected state at each interval's
    second fix is the fitted state to rounding, and the pass's covariance there
    equals the factor's.
33. (10) Test: a step carrying more noise takes the larger share; with uniform
    noise the velocity share grows with elapsed time.
34. (10, as settled) Test: on a recording that does not rotate and whose
    readings integrate exactly to the fitted states, the reconstruction is the
    forward integration, the step corrections are zero and the acceleration is
    the rotated reading.
35. (10, as settled, as amended) Test: on every success fixture the
    published acceleration integrated by the kernel's rule reproduces the
    published velocity change over runs of samples, the whole fitted interval
    included, within the stated bound, the kink of the rotated reading at a
    fix included.
36. (10, as settled) Test: the published time axis of every success fixture and
    of a synthetic recording whose GNSS rate exceeds its IMU rate is exactly
    the IMU samples of the fitted interval; the before-and-after identity is
    the re-capture's check.
37. (10, as settled) Test: the diagnostics carry the account of the
    specification's section 7, and the golden comparison covers it; the time
    of the largest step correction, an argmax, is compared in exact mode only.
38. (10) Test: a fit stored under the previous algorithm string is stale at the
    next load, and one stored after the change restores bit for bit.
39. (10) Test: the audit's confinement rules hold, and the documents describe
    the reconstruction, what it publishes and what its one pass leaves out.
40. (11) `docs/` describe the reconstruction and that its output is the
    IMU-rate posterior to within one linearization, what that leaves out, what
    is published (the acceleration's consequence, the time axis in one
    sentence), the diagnostics keys, the algorithm string and the first start,
    and what the fusion plots show between fixes; tests/README.md section 11
    records the goldens captured again and why; the map gives the specification
    its range, with the amended items restated.

## Appendix K. The acceptance items of the documented noise model and the accuracy, part 1 (1001-1065)

The testable statements of the specification "The documented noise model and
the accuracy, part 1", which amends "Sensor fusion as an explicit
calculation, with plot-driven background jobs" (appendix B), "Sensor fusion
improvements" (appendix C), "Storing requested calculation results with the
session" (appendix D), "Demand-driven requested calculations" (appendix F),
"Sensor fusion plots, attitude and the orientation attribute" (appendix I)
and "The fused state at every IMU sample" (appendix J), one sentence each,
with the specification's section number in front. The specification numbers no
clauses; the numbers below are this list's, and item = 1000 + the number
(items 1001-1065 of `tests/acceptance_map.txt`, the rows of section 9.11). Its
sections 1-4 (motivation, principles, scope, terms) have no item, their
principles being carried by the clauses of the sections that apply them.
Clauses 50-64 are its section 11 tests and clause 65 its section 12. Clauses
6, 8, 9, 10, 11, 14, 15, 18, 20, 24, 25, 26, 31, 35, 37, 44, 55 and 61 are
stated as settled (9.11 says how), and clause 54 as amended by the
specification "The scaled IMU factor stands alone" (its comparison holds the
scale at one instead of switching the state off) and again by the
specification of 1401-1415 (its comparison is the full fit's held stage, read
from the fit's account, and the objective's). The specification amends
the earlier items
115 (appendix B); 215, 216, 217, 221, 227, 230, 235, 245, 246 and 247
(appendix C); 337 (appendix D); 509 (appendix F); 801, 802, 842, 847, 854,
861, 862 and 863 (appendix I); and 901, 910, 912, 914, 915, 921, 925, 930
and 935 (appendix J): each is restated "(as amended)" in its appendix, its
row of section 9 and the map.

1. (5) The configuration keys, with actual values and the unit in the name:
   in `SENSOR.CSV` `ACCEL_FS_G` (2, 4, 8, 16), `GYRO_FS_DEG_S` (250, 500,
   1000, 2000), `ACCEL_ODR_HZ` and `GYRO_ODR_HZ` (12.5, 26, 52, 104, 208,
   416, 833, 1666, 3333, 6666, and 1.6 for the accelerometer), `BARO_ODR_HZ`,
   `HUM_ODR_HZ` and `MAG_ODR_HZ`; in `TRACK.CSV` `GNSS_MODEL` by name
   (portable, stationary, pedestrian, automotive, sea, airborne_1g,
   airborne_2g, airborne_4g) and `GNSS_RATE_HZ`; recorded with their value
   forms in `docs/DATA_SCHEMA.md` section 2 beside `SCHEMA_VER`.
2. (5, 9) The importer stores them as header attributes, as it stores
   `SCHEMA_VER`, and they travel with the session as `SCHEMA_VER` does: kept
   as recorded, merged by the attribute conflict rule, written back by the
   exporter.
3. (5) A malformed value is an import error naming the key, and nothing of
   the file is imported, as for a malformed `SCHEMA_VER`.
4. (5) A recording that lacks a key takes the value of the firmware of the
   recordings on disk, v2023.09.22: +/-16 g, +/-2000 deg/s, 12.5 Hz for both
   sensors; no dynamic model and no rate; the default is written once in the
   documentation and once in the code, with the firmware version it
   describes.
5. (5) The IMU's low-pass filters have no key: that firmware's fixed setting
   (the gyro's LPF2 at the cutoff of its 12.5 Hz rate and no LPF1, the
   accelerometer at its ODR bandwidth) is documented as part of the default,
   with the firmware version.
6. (5, as settled) The dynamic model, the GNSS rate and the barometer's,
   humidity sensor's and magnetometer's rates are read and stored with the
   other attributes; nothing uses them, and they are not inputs of the
   fit.
7. (5, 10) The configuration reaches the kernel as part of its channels, as
   the origin does: the fit declares the four IMU keys as inputs, as it
   declares the origin, the input adapter carries them, the kernel reads no
   preference or constant for them, and nothing above the registration
   changes.
8. (5, as settled) Whether stated or defaulted, each IMU sensor's range is
   checked against the lattice of its readings (the gyro's sensitivity at the
   range, 70 mdps at +/-2000 deg/s, 1.14688 times the range over 32768; the
   accelerometer's range over 32768, which the printed sensitivity rounds; in
   effective units, within the rounding of the file's decimals): the coarsest
   range whose lattice all but one in a thousand of the readings fit must be
   the configured one (a real recording carries a few readings off its
   lattice, and they do not decide); otherwise the fit rejects the recording
   with a reason that names the range stated and the range the values show,
   and does not guess.
9. (10, as settled) The lattice check is a kernel validation rule, one of the
   rules that decide whether a recording can be fitted, and reads the
   readings as the kernel receives them, before any correction of its
   own.
10. (5, as settled) A logged IMU interval that disagrees with a stated rate by
    more than the oscillator's tolerance (10 %) is a rejection that names
    both.
11. (6, as settled) The per-sample noise of each sensor and axis is
    `sqrt(density^2 x bandwidth + step^2 / 12)`: the datasheet's density at
    the configured range, the datasheet's bandwidth for the configured rate
    and filter, and the quantization step at the range (the gyro's
    sensitivity, 70 mdps at +/-2000 deg/s, 1.14688 times the range over
    32768; the accelerometer's range over 32768, which the printed
    sensitivity rounds).
12. (6) The documentation carries the table: the density per range for each
    sensor and the bandwidth per rate and filter, with the datasheet's table
    numbers.
13. (6) The density the integration uses is the per-sample noise times the
    square root of the nominal sample interval at the configured rate, so that
    a step of the nominal length carries one sample's variance; `accDensity`
    and `gyroDensity` are gone, and no density in the model is derived
    otherwise.
14. (6, as settled) The sampling term replaces the slopes: per step, the
    trapezoid rule's error on a piecewise-linear signal, `dt^3 / 12` times the
    second derivative for a step that is a whole sample interval, and for a
    part of one the same derivation's integral over the part, the second
    derivative estimated per sample interval as the larger of the changes of
    slope at its two ends, for velocity from the specific force and for the
    angle from the rate, added in quadrature per step, with no fitted
    coefficient.
15. (6, as settled) The remainder of the mid-step scheme to second order, for
    rate and force linear across the step: its pure rotation term, second
    order in the step's rotation, and the same expansion's terms in the change
    of force and of rate across the step (coning included), derived and added
    the same way.
16. (6) The documentation writes the derivations out, including what they
    assume (a smooth signal between samples, the samples exact).
17. (6) The bias priors stay at 0.3 m/s^2, 0.03 rad/s and 0.010 deg/s per
    degC, and the documentation cites the datasheet's table for each.
18. (6, as settled) A scale-factor state: per fit, one factor per axis for
    each sensor, starting at one, with the datasheet's sensitivity tolerance
    (G_So%, +/-1 %, for both sensors: Table 2 states none for the
    accelerometer) as a zero-mean prior on its departure from one.
19. (6) The readings enter the integration divided by the scale, and the
    graph is re-preintegrated at the fitted scale as it is at the fitted
    bias, under the same settled test.
20. (6, as settled) A recording without motion leaves the scale at one within
    its prior: the data then constrain only the corrected reading along
    gravity, which the scale and the bias of that axis share in proportion to
    their priors' variances; this holds for the deterministic part of the
    readings, and on a noise-dominated gyro axis the noise moves the factor
    within its prior.
21. (6) The fitted scale factors are reported in the diagnostics beside the
    biases.
22. (6) The sigma of each fitted scale factor is reported beside it, from the
    covariance of section 7.
23. (6) The 1.14688 schema correction of legacy recordings is not a scale
    factor and is untouched.
24. (6, as settled) Validation, not tuning: the normalized residuals
    (position, velocity, IMU) of the reference recordings of `tests/README.md`
    section 12.2 and of the committed fixtures that fit (the three golden
    successes; the rejections have no residuals) are reported in the
    documentation as measured under this model, with the sentence that a value far from one measures what the
    model does not yet describe; no constant of section 6 is changed to move
    them.
25. (7, as settled) After convergence, from the converged graph and one
    factorization: the marginal covariance of every fix state, the joint
    covariance of each pair of adjacent fix states, and their
    cross-covariance with the biases and the scale factors, by the clique
    marginals of one factorization (one multifrontal QR elimination in time
    order with the globals last, each clique's marginal from its parent's),
    not by the library's joint marginals; a heading prior of 1000 rad on the
    first fix, in the factorization only, keeps an undetermined heading
    finite and moves a determined one by less than 5e-6 of itself.
26. (7, as settled) Through the reconstruction's pass, the conditional
    covariance of the state at every IMU sample given the two fixes around
    it, composed with the covariance of those fixes, the biases and the
    scales into the covariance of the state at the sample.
27. (7) The covariance step runs inside the same cancellable job and is not a
    cancellation boundary.
28. (7) The heading accuracy of a sample is the square root of the
    navigation-frame rotation covariance's element about the vertical, the
    tilt accuracy the square root of the sum of its two horizontal elements,
    both in degrees and capped at 180, which means undetermined.
29. (7) The acceleration accuracy of a sample is the first-order propagation
    of the joint covariance of attitude, accelerometer bias and accelerometer
    scale through `a = R (f / s - b) + g`, plus the accelerometer's per-sample
    noise rotated; the horizontal accuracy is the standard deviation along the
    horizontal acceleration's direction, or the larger horizontal principal
    value where the horizontal acceleration is below that accuracy, and the
    vertical accuracy is the vertical element; both in m/s^2.
30. (7) The documentation states the propagation in symbols and what it
    leaves out (gravity's own uncertainty, cross-axis sensitivity, the
    interpolation between nodes).
31. (7, as settled) Widening: over the fixes within 2.5 s of each sample, the
    sum of the squared whitened residuals of the window's factors divided by
    the window's degrees of freedom is a factor; the sample's four accuracies
    are multiplied by its square root where it exceeds one and are unchanged
    where it does not.
32. (7) The documentation says what the factor is (the a-posteriori variance
    factor of the window), that it assumes every sigma is off by the same
    ratio, and that it never tightens.
33. (7) `Fusion/headingAcc`, `Fusion/tiltAcc`, `Fusion/accHAcc` and
    `Fusion/accDAcc` are outputs of the fit, aligned with `Fusion/_time`.
34. (7) They are absent for a rejected or failed fit and for a successful fit
    whose factorization failed, which the diagnostics say; nothing else about
    that fit changes.
35. (7, as settled) An undetermined heading does not fail the computation:
    the cap applies, the heading's variance enters the acceleration's
    propagation at most at the cap's, and the acceleration accuracies use the
    horizontal magnitude's direction, which a heading error does not move.
36. (2, 7) The documentation says in one sentence that the accuracy is one
    standard deviation from the covariance of the converged solution under the
    documented model, widened where the residuals exceed what the model
    allows.
37. (8, as settled) The "Sensor fusion" category gains four plots after Roll:
    Heading accuracy and Tilt accuracy in degrees, Horizontal acceleration
    accuracy and Vertical acceleration accuracy in the acceleration unit,
    each drawn as the quiet member of the value it qualifies, in a colour
    that reaches 3:1 on both plot backgrounds (as amended by the
    specification "Plot colours: one colour per plot, readable on both
    backgrounds"); the acceleration accuracies in g at four decimals, a type
    of their own.
38. (8) The four plots are absent, like any unavailable value, where the fit
    did not compute them.
39. (8) A logbook column over any of the four works as over any fusion
    value.
40. (8) The diagnostics show the configuration the fit ran under.
41. (8, 9) The first start after the change finds every stored fit stale at
    its recording's load and recomputes it when something switched on needs
    it, counted in the status bar as any computation.
42. (9) The fit's outputs gain the four channels, and the record stores and
    restores them with the rest, in the record format as it is.
43. (9) The algorithm string changes once, for the whole specification.
44. (9, as settled) The golden fixtures are captured again with the existing
    tool at the end of each phase that changes numerical results; their time
    axes are unchanged and checked byte for byte; the last capture's
    comparison covers the new channels, the scale factors and the
    configuration in the diagnostics.
45. (9) The fixtures state their configuration explicitly, so that the stated
    path (the kernel's and the sessions' fixtures) and the default path (a
    session without the keys) are both exercised.
46. (10) The kernel derives its noise from the configuration in one unit, the
    only place the datasheet table exists in code.
47. (10) The sampling term and the rotation remainder are computed where the
    steps are integrated, from the readings, with no constant of their
    own.
48. (10) The scale factors are variables of the graph beside the biases.
49. (10) GTSAM stays confined to the kernel, and the covariance step is a unit
    of its own, as the reconstruction is.
50. (11) Test: a file with every key imports them; a malformed one is an
    import error naming the key; a file without them takes the default.
51. (11) Test: a fixture whose readings sit on the +/-8 g lattice with a
    header stating +/-16 g is rejected naming both; the lattice check
    identifies the range of every committed fixture and of the reference
    recordings.
52. (11) Test: the per-sample noise and the density follow the table for
    every configuration the fixtures state, bit for bit against the
    formula.
53. (11) Test: a step with a constant signal has no sampling term; the
    sampling term of a step follows the derivation on a synthetic signal with
    a known second derivative; the rotation remainder follows its derivation
    on a constant turn.
54. (11, as amended) Test: a synthetic recording with the accelerometer 2 %
    high on one axis recovers the factor within the prior's tolerance through
    the full fit's released stage, and its objective falls below that of the
    full fit's held stage, whose prior a thousand times tighter than the
    datasheet's holds the scale at one, read from the fit's account (as
    amended by the specification of 1401-1415); a recording at rest leaves
    the scale at one within its prior.
55. (11, as settled) Test: the normalized residuals of the committed fixtures
    that fit are in the goldens; the reference recordings' are recorded in the
    documentation.
56. (11) Test: on the committed fixtures a successful fit publishes finite,
    positive accuracies for every sample, and scaling every GNSS accuracy up
    never lowers them.
57. (11) Test: the heading accuracy of the first node agrees with the heading
    check.
58. (11) Test: the acceleration accuracy follows its propagation on synthetic
    inputs with known answers, and agrees with the library's joint marginals
    on a short fixture.
59. (11) Test: the widening is one where the residuals are at the model, and
    grows where a factor's sigma is understated by a known ratio.
60. (11) Test: the cap holds on a fixture whose heading is undetermined.
61. (11, as settled) Test: on a build where the configuration is carried and
    defaulted and the noise model is the previous one (the end of phase 1),
    the existing channels and diagnostics of every fixture are bit-identical
    to the goldens.
62. (11) Test: a stored fit from before the change is stale.
63. (11) Test: the four plots are explicit-backed like the other fusion plots,
    and a column over one works.
64. (11) Test: the audit's confinement rules hold, and the documents carry the
    table, the derivations and the validation.
65. (12) `docs/` describe the change: `DATA_SCHEMA.md` (section 2, the keys;
    section 11, the algorithm string; section 12, the record),
    `SENSOR_FUSION.md` (section 3, the configuration inputs and the lattice
    check; section 4, the noise model with its table and derivations, the
    scale state, the covariance and its propagation, the widening, the outputs
    and diagnostics; section 8, the validation results and what is and is not
    validated), `CALCULATIONS.md` section 17, `COMPUTED_PLOTS.md` (the four
    plots); `tests/README.md` section 11 records the goldens captured again
    and why; the map gives the specification its range, with the amended
    items restated.

## Appendix L. The acceptance item of the GNSS acceleration accuracy (1101)

The testable statement of the specification "GNSS acceleration accuracy",
with the specification's section numbers in front: one item, 1101 of
`tests/acceptance_map.txt`, the row of section 9.12. Its section 1
(motivation) has no item.

1. (2, 3, 4) One on-demand GNSS calculation, `GNSS/accAcc`, m/s^2, aligned
   with `GNSS/_time`, from `GNSS/_time` and `GNSS/sAcc` only:
   `accAcc[i] = sqrt(sAcc[i+1]^2 + sAcc[i-1]^2) / (t[i+1] - t[i-1])` for the
   interior samples and, at the two ends, the forward and backward forms that
   `computeDerivative` uses there; strictly the receiver's `sAcc` and the
   sample times, with no correction factor, nothing from the fusion and
   nothing measured on a corpus; unavailable when `sAcc` or `_time` is
   missing or shorter than two samples, as the derivative is. One plot,
   "Acceleration accuracy" in "GNSS (Advanced)", type `acceleration`, the
   quiet member of the category's accelerations, in a colour that reaches 3:1
   on both plot backgrounds (as amended by the specification "Plot colours:
   one colour per plot, readable on both backgrounds"). The documentation says, in
   one sentence each, that the figure is the receiver's stated speed accuracy
   propagated through the central difference assuming the two fixes' errors
   are independent; that the assumption is conservative; and the measurement
   on the reference recording `24-09-05/11-17-12` (0.09 g RMS overall and
   0.058 g in steady flight against the formula's 0.19 g, a velocity error
   of 0.18 m/s against a stated 0.52, an autocorrelation over two fixes of
   0.36), which describes the gap and is not applied. Tests: known answers bit
   for bit, the ends included; unavailable without `sAcc`, with one sample,
   with lengths that differ; the plot row's type and palette, and a legend
   value over it formatted as an acceleration.

## Appendix M. The acceptance items of sample continuity (1201-1213)

The testable statements of the specification "Sample continuity", with the
specification's section numbers in front: thirteen items, 1201-1213 of
`tests/acceptance_map.txt`, the rows of section 9.13. Its section 1
(motivation) has no item.

1. (3) A sensor's time axis is its `_time` column as the session holds it;
   its nominal interval is the median of the successive differences of that
   axis over the whole session; an interval between two successive samples
   is a hole when it is strictly greater than 1.5 times the nominal
   interval; a time axis with fewer than three samples has no nominal
   interval and no holes. The two samples around a hole are ordinary
   samples: no sample is removed, inserted or altered.
2. (4, plots) A series is drawn on its sensor's time axis, and its line
   stops at the last sample before a hole and resumes at the first after
   it, with nothing between, for every series of the sensor, recorded and
   computed alike, whichever independent variable the plot uses.
3. (2, 4, the fusion plots; per sensor) Each sensor has its own time axis
   and its own holes: a hole in the GNSS samples is not a hole in the IMU's,
   and a series on the IMU's axis draws through it; a hole in the IMU's
   samples breaks a series on it like any other.
4. (4, legend and crosshair) The legend and the crosshair show "--" for a
   series at a cursor time inside one of its holes, as beyond the series'
   ends.
5. (4, measure tool) The measure tool reads a series at its two ends; an end
   inside a hole reads "--" and the measurement that needs it is not shown;
   a measurement whose two ends are on samples is reported even when the
   span between them contains a hole.
6. (4, map) The map draws one polyline per run of connected fixes, so the
   track has a visible break at a hole; the cursor dot is absent while the
   cursor is inside a GNSS hole.
7. (4, logbook measurements at a marker) Measurements at a marker, and the
   interpolation calculation behind them, are unavailable when the marker's
   time falls inside a hole of the sensor they read, as outside the
   sensor's coverage.
8. (4, ground elevation reads) The Set Ground tool's elevation at the
   clicked time and the automatic ground elevation at the analysis end have
   no value at a time inside a hole, as outside the samples: the click sets
   nothing and the attribute is unavailable.
9. (4, derived values) A stencil that spans a hole yields no value: the
   derivative and its accuracy are unavailable at the samples whose centred
   difference crosses a hole, and the one-interval forms at the ends are
   unavailable when that interval is a hole; every other sample is what it
   was. A value computed from a single sample is unaffected.
10. (4, crossing times) The exit, the altitude markers and the analysis
    windows of the WS-P and SP methods find a crossing by linear
    interpolation between the two samples around it, also when they are the
    sides of a hole; the documentation says so.
11. (5) The authority is one unit of the model library: given a time axis,
    its nominal interval, whether the interval between two successive
    samples is a hole, and the runs of connected samples; the factor 1.5 is
    its one constant. The sensor fusion kernel's IMU gap rule reads it: the
    tuning's maximum gap is its threshold for the IMU axis, which the
    attitude propagation and the window checks read, and the kernel's own
    gap constant and comparison are gone. Every reader consults it and none
    re-derives it; nothing about holes is stored.
12. (6) The IMU gap fixture still rejects with the same reason, and the
    fusion goldens are unchanged bit for bit: no fixture and no reference
    recording has an IMU interval between 1.013 and 1.6 nominal intervals.
13. (7) `docs/COMPUTED_PLOTS.md` has a section "Holes in the data" (what a
    hole is, that nothing is drawn, read or differenced across one, that the
    fusion plots are on the IMU's axis, that the crossing times are the one
    exception); `docs/CALCULATIONS.md` section 18 and the derivative helper's
    contract state that a stencil never spans a hole; `docs/SENSOR_FUSION.md`
    section 6 states the IMU gap rule as the application's continuity rule at
    1.5; this file has the appendix and the matrix.

## Appendix N. The acceptance items of GNSS holes bridged by the IMU (1301-1313)

The testable statements of the specification "GNSS holes bridged by the
IMU", as amended after its first implementation (the cap, the growth claim,
the slow-tail rule), with the specification's section numbers in front:
thirteen items, 1301-1313 of `tests/acceptance_map.txt`, the rows of section
9.14. Its section 1 (motivation) has no item. It restates item 244 of
appendix C as amended. Item 13 is stated as amended by the specification
"The scale factors as a refinement" (appendix O). Item 2 is stated as
amended by the specification "Fused position and speed accuracy" (appendix
R): the growth through a hole is the step chain's covariance `P_j`.

1. (2, 3) Connectedness is the IMU's. The one disconnection rule is the IMU
   gap rule of the continuity authority; the kernel's rule on the spacing of
   the GNSS fixes and its two constants are deleted, and below the cap the
   plan of a fit runs no check on fix spacing: the preintegrated covariance
   prices the hole, and the published accuracies report it. The cap is
   measured, not guessed: one kernel constant, the longest GNSS hole the fit
   bridges, 30 s, and one check in the plan after the window's validation on
   the fitted window's GNSS axis (the continuity authority's); a longer hole
   rejects the recording with a reason that names the hole's length and the
   limit.
2. (2, 3, as amended) Everything is published. Every IMU sample inside a hole
   gets its reconstructed state and its accuracies; nothing is suppressed.
   The growth through a hole is in the position and velocity of the step
   chain's covariance (`P_j`, read through the reconstruction's per-interval
   seam), which grow through the hole and collapse at the next fix; the four
   published accuracies (heading, tilt, two accelerations) are bounded by
   global terms and grow through a hole only where those terms allow; inside
   a hole they
   are never below their values at the fixes around it (measured false on
   `bridged_hole` and not asserted; the numbers are in
   `docs/SENSOR_FUSION.md` section 8). The fit and the
   reconstruction are unchanged in kind: the IMU factor spans the hole as it
   spans any interval, the per-interval pass publishes every IMU sample of
   the interval, and the covariance step and the widening, whose window
   extends to the two fixes around a sample when none fall inside it, need
   nothing.
3. (2) The fixes around a hole are ordinary fixes: they carry their stated
   sigmas, and the model weights them as it weights every fix.
4. (3, 4) The segment cutter merges any sparse piece: a piece with fewer
   than three fixes is merged into the piece before it, or into the piece
   after it when it is the first; a piece that is the whole window is left
   alone, and the fitted window's own rule of three fixes still rejects a
   recording with fewer; a stretch of whole segment lengths without a fix
   yields no piece. A window whose cut yields a middle piece of two fixes is
   fitted, the piece merged into its predecessor; a window whose first piece
   has two fixes merges it into its successor.
5. (3) The input audit names the holes: `input` gains `gnss_holes`, one
   object per hole of the fitted window, each `start_s` (seconds since the
   epoch, like `start_s` of the fit) and `length_s`, in time order, empty
   when there is none; a hole is the continuity authority's hole for the
   GNSS axis of the window.
6. (2, 3) A change that alters what a fit returns changes the algorithm
   string: it becomes `batch-temperature-bias-v8`, and every stored result,
   fits and rejections alike, is dropped at its recording's next load and
   fitted again once when something needs it.
7. (4) The bridged fixture: the recording of the former GNSS gap rejection
   (`coarse_maneuver` with fixes 12 to 23 removed, a 2.6 s hole) becomes the
   success fixture `bridged_hole`: it converges, its golden is captured, and
   the golden suite holds four fits and ten rejections. The published
   attitude and accelerations at every IMU sample inside the hole lie within
   three of their own published accuracies of the generating trajectory; the
   four published accuracies inside the hole are never below their values at
   the published samples nearest the two fixes around it (measured false on
   `bridged_hole` and not asserted; the numbers are in
   `docs/SENSOR_FUSION.md` section 8); and the position
   and velocity parts of the sample covariance, read through the
   reconstruction's per-interval seam, grow through the hole and collapse at
   the fix after it.
8. (4) The long hole, at the cap and above it: a second synthetic recording
   with `coarse_maneuver`'s kind of motion and a manoeuvre on both sides of
   a 30 s hole, the IMU continuous, converges under the production tuning;
   the test logs its iterations per pass, the largest accuracy inside the
   hole and the residuals at the fix after it. The same recording with a
   hole above the cap is rejected with the reason naming the length and the
   limit. The measurement behind the cap (every length to 40 s settles, 50 s
   marginal, 60 s cycles) is recorded in the documentation, not re-run by a
   test.
9. (4) The audit: `gnss_holes` is in the diagnostics of every fit, empty for
   the three unbroken fixtures and one entry of 2.6 s for `bridged_hole`; no
   outage constant and no outage check remain in `src`, `tests` or `docs`;
   the cap is one constant, named by the rejection's reason and the
   documentation.
10. (4) The reference recordings: a manual step runs `24-09-07/08-35-48` and
    `24-09-04/13-35-10` through the runner and records the outcome, the
    stopping rule, the iterations per pass, the objective, the quality,
    `gnss_holes`, and the largest heading and tilt accuracy inside the
    longest hole against the median accuracy of the recording; the numbers
    are reported as they are.
11. (4) The acceptance map opens a new hundred for this specification's
    items; `audit_cleanup` and the whole suite are green, the exact tests
    included.
12. (5) The documents: `docs/SENSOR_FUSION.md` section 6 drops the GNSS gap
    rejection and states that a hole in the fixes up to the cap is bridged by
    the IMU, with the measurement behind the cap and why a single factor
    imposes it, and that a longer hole is rejected naming the limit; the
    stopping-rule paragraph states the slow tail's three bounds; section 2
    says what the user sees over a hole and that the GNSS plots break there
    while the fusion plots draw through; the input audit gains `gnss_holes`
    and the algorithm string's history `v8`, naming both the bridged holes
    and the continuity rule's 1.5 threshold that the kernel adopted under the
    compatibility marker's bump to 3; section 8 gains the two recordings'
    numbers and counts four fits and ten rejections.
    `docs/COMPUTED_PLOTS.md`'s section on holes gains the sentence about the
    fusion plots; `docs/DATA_SCHEMA.md` section 12 names `v8`; this file has
    the fixture table, the new manual step, the appendix and the matrix.
13. (2, 3, as amended) A slow tail is accepted only when every factor kind
    fits: a final pass at its iteration limit is accepted as a slow tail only when its mean
    relative decrease over the window is below the bound and the position,
    velocity and IMU normalized RMS are all below `slowTailMaxNrms`;
    otherwise the rule is `iteration limit`, a solver failure. The tuning
    field keeps its name and value. The slow-tail test gains the case where
    the IMU normalized RMS alone is above the bound and the tail is refused;
    under the rule `24-09-04/13-35-10` was expected to end `iteration limit`,
    and with `v8` it did. (As amended by the specification of 1401-1415: the
    rule is unchanged; the recording now converges, its scale factors held
    until the fit is stable and then released, M56.)

## Appendix O. The acceptance items of the scale factors as a refinement (1401-1415)

The testable statements of the specification "The scale factors as a
refinement: held until the fit is stable, then released", as amended before
its implementation (the released stage's own budget, the divergence bounds
reported), and with the budget set to three passes at implementation (the
specification's two discarded releases that had reached their minimum, on
`scale_recording` and four of the five M56 recordings, all but `10-15-24`),
with the specification's section numbers in
front: fifteen items, 1401-1415 of `tests/acceptance_map.txt`, the rows of
section 9.15. Its section 1 (motivation) has no item. It restates items 43
of appendix C, 54 of appendix K and 13 of appendix N as amended.

1. (2, 3) The full fit runs in two stages. The pass loop is unchanged in
   kind; under the temperature model it runs twice on one graph whose scale
   prior's sigma is the only difference: the held stage from the
   initializer's start with the sensitivity tolerance divided by a thousand,
   then the released stage from the held stage's values (every state, the
   bias, the slope and the factors) with the tolerance itself. The held stage
   has the tuning's pass budget (`maxPasses`) and per-pass iteration budget;
   the released stage has a budget of its own, `releasePasses`, three passes
   of the same iteration budget. The initializer's prefix and segment fits,
   which have no scale state, are untouched.
2. (2, 3) The release trigger is convergence: the released stage runs only
   when the held stage ended `settled` or `slow tail accepted`. A held stage
   that ends any other way ends the fit as before, under the same rule, with
   the same reason and diagnostics shape; no failure carries
   `scale_release`.
3. (2, 3) The fallback. A released stage that ends `settled` or `slow tail
   accepted` is the fit. One that ends under any other rule, diverges or
   fails is discarded: the held stage's values, graph, objective, residuals
   and quality are the fit, `converged` is true, the stopping account is the
   held stage's, the factors are at one with the held stage's sigmas, and
   the diagnostics record the release's outcome. A refinement never turns a
   converged fit into a failure; a cancellation is not a failure and cancels
   the fit.
4. (2, 3) Divergence between passes. After every pass's rebuild, in either
   stage and before the cost test, the IMU normalized RMS of the rebuilt
   graph must be below `divergenceMaxImuNrms`, 10, and each of the six
   factors strictly inside `divergenceScaleRange`, 0.5 to 2; a pass beyond
   either ends its stage at once under `diverged`: in the held stage a
   solver failure with the failure diagnostics of a completed pass
   (`stopping` and `quality`), in the released stage the fallback. A zero
   bound or an empty interval refuses deterministically, and the stopping
   account reports both bounds (`divergence_max_imu_nrms`,
   `divergence_scale_range`) beside the other thresholds. The production
   bounds change nothing on any fixture.
5. (3) The pass numbering and the boundary. Passes are numbered across both
   stages in the trace and the progress texts, so the released stage's
   first pass is one more than the held stage's last; the boundary before
   it reports `Releasing the scale factors`, a cancellation boundary like
   every other. Each stage's `stopping.passes` counts its own passes.
6. (3) The diagnostics. `stopping` and `quality` describe the reported fit.
   A new top-level object, `scale_release`, carries the account: `held` with
   the held stage's `rule`, `passes`, `iterations` and `objective`;
   `released` with the same four for the released stage when it ran, or
   `null`; `kept`, true when the released stage is the fit; and `reason`,
   the rule the released stage ended under when it was discarded, or
   `null`. `model.scale` reports the factors of the reported fit;
   `seeds[0].iterations` counts both stages.
7. (2, 3) The algorithm string becomes `batch-temperature-bias-v9`: every
   fit's numbers move, so every stored result is dropped once; `v8` remains
   only as history.
8. (4) The recovery holds: `scale_recording`, whose accelerometer factor is
   2 % off, recovers it through the released stage, and its objective falls
   against the held stage's, read from the diagnostics' account in place of
   a second fit with the scale held.
9. (4) The resting recording leaves every factor at its prior through both
   stages, and its released stage settles.
10. (4) The fallback forced: a checkpoint that throws at the release
    boundary leaves a converged fit with the held stage's values and factors
    at one, `scale_release.kept` false with the reason, and the published
    channels those of the held fit.
11. (4) Divergence forced: `divergenceMaxImuNrms` at zero ends the held stage
    under `diverged` as a solver failure with the completed-pass failure
    shape, and so does an empty `divergenceScaleRange`; a range that
    excludes the recovered factor of `scale_recording` ends its released
    stage under `diverged`, the fallback. The IMU bound is not forced in the
    released stage alone: no stage-specific bound exists.
12. (4) The reference recordings: a manual step in the form of M49 runs the
    four reference recordings and `24-09-04/13-35-10`, with `scale_release`
    read from each diagnostics: `13-35-10` is expected to converge; the four
    at or near their M49 numbers, the released stage adding a few
    iterations; the numbers go in the report and in the documentation as
    measured.
13. (4) The goldens are captured again under the new string: four fits, ten
    rejections; the capture note records what moved and why.
14. (4) The acceptance map opens a new hundred; `audit_cleanup` and the
    whole suite are green, the exact tests included.
15. (5) The documents: `docs/SENSOR_FUSION.md` section 4 describes the two
    stages, the trigger, the fallback, the divergence rule and its bounds,
    with the probe's numbers on `13-35-10` as the case, the stopping rules
    gain `diverged`, and the diagnostics gain `scale_release`; section 7 adds
    the fourth boundary and `v9` to the string's history; section 8 gains the
    manual step's numbers. `docs/DATA_SCHEMA.md` section 12 and
    `docs/CALCULATIONS.md` section 17 name `v9`. This file has the kernel
    test rows, the manual step, the appendix and the matrix.

## Appendix P. The acceptance items of background computation per recording (1501-1524)

The testable statements of the specification "Background computation per
recording: the Compute attribute", with the specification's section numbers
in front: twenty-four items, 1501-1524 of `tests/acceptance_map.txt`, the
rows of section 9.16. Items 1-12 are the attribute, the column, the
preference, the exclusion, the cancel, the index entry and the cell; items
13-23 are the eleven bullets of its section 6 in order; item 24 is its
section 7. Its section 1 (motivation) has no item. It restates items 19 of
appendix F and 29, 33 and 62 of appendix G as amended.

1. (2, 3) The Compute attribute: a session attribute `_COMPUTE`, a Choice of
   on ("On") and off ("Off") in that order, category "Session", display name
   "Compute", editable, a built-in of the core registered beside the other
   built-in definitions; its tokens are spelled beside the key and its labels
   in the definition; read by the demand layer alone, and named in src only by
   the key's home, the demand layer, the importer, the index, the definition,
   the constant default and the preference's registration and page.
2. (3) A session without the line reads the constant default on: the column
   shows "On", the file is not touched and index.json caches on for its stub;
   `setData()` writes on or off verbatim and refuses any other value; no edit
   removes the line; a token outside the list is shown as written and the
   session is computed.
3. (2, 3) Not an input: setting the attribute while a job of the session runs
   leaves it not asked to stop, keeps the stored records, the cached values
   and every other session's results, starts no settle wait and runs one pass
   of its own; the compatibility marker and the algorithm string are
   unchanged.
4. (3) The column: the header's tooltip "compute results for this recording in
   the background" (no other header has one); "Set Compute..." offers the
   definition's list and applies a bulk edit, on a stub without loading it.
5. (3) The import preference "Compute newly imported recordings in the
   background", on by default and holding the token: a recording imported
   while it reads exactly off is stored with the off line, one with its own
   line keeps it, and any other value stores nothing; the page round-trips the
   setting.
6. (4) A track of a session that reads off is excluded, decided first, for
   loaded and unloaded sessions, plot and column tracks: no job, no load, no
   hold, not counted in progress, not a failure, its cell excluded and not
   pending; the other sessions compute.
7. (2, 4) What exists is shown: a stored result of a session switched off is
   restored when it is loaded and fills its column from the record without a
   load; its stored rejection is not a failure and shows no row warning, and
   is listed again, without computing, when the session is switched on.
8. (4) The excluded cell: a requested column's cell of a session switched off
   that has no value reads "excluded", muted like the pending mark, with the
   tooltip "Not computed: background computation is switched off for this
   recording"; a value always wins; it is a presentation of demand, never in
   the model, the cached values or the index, sorting treats it as
   unavailable, and `pendingCellsChanged(id)` announces and repaints its
   column.
9. (4) Switching off: a pass runs at once; the session's chosen next job is
   withdrawn ("No longer needed"); its running job ends Cancelled with the
   reason "Switched off for this recording", publishing and storing nothing,
   and the next job starts after it; the executor's cancel takes the caller's
   reason; the pair memory is not touched.
10. (4) Switching on: the session's missing results enter demand under the
    ordinary priority, a stopped computation runs again from its start, and a
    failure remembered for the pair still keeps it from being offered.
11. (4) The plot widget's warning, priority, the settle wait, the fill's
    bound, the executor's limits and the status bar's form are unchanged; the
    counts and lists simply omit an excluded session, and the running job of
    one is described by nothing.
12. (5) Sessions that are not loaded: the index records, per session,
    `"computeOff": true` when the file was last seen switched off, learned at
    a save and a load (the import, a loaded row's save, the bulk edit's stub
    path, the column worker's copy), carried by a remap, dropped by a removal
    and a reset; an index without it reads on; an off file the index does not
    know is loaded at most once and never computed.
13. (6, first bullet) A session switched off is not computed for a plot
    checked while it is visible, when shown while a plot is checked, or for an
    enabled column; the others are; progress never counts it; its cell reads
    excluded and is not pending.
14. (6, second bullet) Switching off while the job runs ends it Cancelled with
    the reason, publishes and stores nothing, releases the hold and runs the
    next candidate; while it is the chosen next job, it is withdrawn; while
    the session settles, nothing is left waiting.
15. (6, third bullet) Switching on creates demand for the session's missing
    results and no other; a stopped computation runs again from its start.
16. (6, fourth bullet) A session switched off with a stored result restores
    and draws it, fills the column from it and is not loaded again for it; its
    stored rejection is not a failure and shows no row warning, and is listed
    again when it is switched on.
17. (6, fifth bullet) Setting the attribute invalidates no stored result and
    no cached value, starts no settle wait and makes no running job stale; the
    marker and the algorithm string are unchanged.
18. (6, sixth bullet) The column fill never loads a session switched off; with
    every unloaded session off, enabling the column creates no load and no
    job; the index records the attribute on the bulk edit's stub path and
    reports it without a load.
19. (6, seventh bullet) The column: the default shows "On" without a write; an
    edit stores a token and refuses others; "Set Compute..." bulk-edits the
    selected sessions; a hand-edited token is shown as written and read as on.
20. (6, eighth bullet) A recording imported while the preference is off
    carries the off line and is excluded at once for an enabled column and a
    checked plot; one imported while it is on carries no line; the page
    round-trips the setting.
21. (6, ninth bullet) A session whose attribute is off on disk but absent from
    the index is loaded at most once by the fill and never computed.
22. (6, tenth bullet) The manual step M57: a long-running recording switched
    off while it is computed (the status bar's count, the cell, the row, the
    plot), switched on again, the whole-logbook column fill with half the
    recordings off, the import preference.
23. (6, eleventh bullet) The audit's gestures rule reads that the demand layer
    is the one product caller of cancel; a new hundred in the acceptance map;
    `audit_cleanup` and the whole suite green.
24. (7) The documents: COMPUTED_PLOTS.md (switching a recording off; the
    status bar, logbook and limitation sections), CALCULATIONS.md sections 15
    and 16 (the condition, the reading, the stop of the running job, the fill,
    the cell's third text), DATA_SCHEMA.md sections 9 and 11, SENSOR_FUSION.md
    section 2, tests/README.md (the rows, M57, appendix P, matrix 9.16).

## Appendix Q. The acceptance items of the fused speed plots (1601-1612)

The testable statements of the specification "Fused speed plots", with the
specification's section numbers in front: twelve items, 1601-1612 of
`tests/acceptance_map.txt`, the rows of section 9.17. Items 1-4 are its
section 2 (the calculations, the rows, demand, what is unchanged), item 5 its
section 3 (the colours), item 6 its section 4 (the documents), and items 7-12
the six bullets of its section 5 in order. Its section 1 (motivation) has no
item. It restates item 15 of appendix B, item 9 of appendix F and items 1, 2,
3, 42, 54, 62 and 63 of appendix I as amended.

1. (2) The calculations: `builtin.fusion.velH` (`Fusion/velH` from
   `Fusion/velN` and `Fusion/velE`, the magnitude of the two, as `GNSS/velH`
   is of the GNSS components) and `builtin.fusion.vel` (`Fusion/vel` from
   `Fusion/velH` and `Fusion/velD`, as `GNSS/vel`), on demand under the Fusion
   sensor, no title, no result version, `velH` registered before `vel`; each
   output has one candidate and the fit alone behind it, so they appear with
   the fit and never start it; Vertical speed is the fit's own `Fusion/velD`,
   whose one producer is the fit.
2. (2) The rows: Horizontal speed (`Fusion/velH`), Vertical speed
   (`Fusion/velD`) and Total speed (`Fusion/vel`) in the "Sensor fusion"
   category after Elevation and before Horizontal acceleration, each named,
   united (m/s) and typed (`speed`, `vertical_speed`, `speed`) as its GNSS
   counterpart so that the two overlay; the category holds fifteen plots, in
   this order: Elevation, Horizontal speed, Vertical speed, Total speed,
   Horizontal acceleration, Vertical acceleration, Along-track acceleration,
   Cross-track acceleration, Heading, Pitch, Roll, Heading accuracy, Tilt
   accuracy, Horizontal acceleration accuracy, Vertical acceleration accuracy.
3. (2) Demand: checking any of the three creates demand for the fit exactly as
   checking a fused acceleration does, one row alone one fit; a stored fit
   draws them at once, without computing; a logbook column over any of the
   three works as over any fusion value, loaded and unloaded; none applies to
   a session without IMU data.
4. (2) The fit is unchanged: its outputs, its algorithm string, its stored
   results and the goldens; no wind correction (`GNSS/velH` and `GNSS/vel`
   have none) and no speed accuracy; a profile that named the plots before
   this change is applied as before.
5. (3) The colours, chosen by `docs/PLOT_COLOURS.md`: each row is kin of its
   GNSS counterpart, the neighbouring hue at the other edge of the lightness
   band, admissible within the speeds group (the three GNSS speeds,
   Wind-corrected horizontal speed, Speed accuracy and the three fused
   speeds); Horizontal speed `#bc378e`, Vertical speed `#1b8278`, Total speed
   `#d446ff`, literals in the plot table with a comment that says what each is
   coloured by and its figures; each fused line is told from its twin and from
   the rest of the group on both backgrounds.
6. (4) The documents: COMPUTED_PLOTS.md (the category's count and list, and
   reading a fused speed beside its GNSS speed: together where the receiver is
   accurate, apart where it is not, the separation the fit's compromise
   between the receiver and the IMU, the receiver's Speed accuracy plot
   saying how far to trust its side of it), SENSOR_FUSION.md section 4 (`velH` and `vel` among the
   values derived from the outputs; Vertical speed is `velD` itself),
   CALCULATIONS.md (the two calculations with their inputs and outputs, their
   definitions, the category's list in its new order), the contract of
   `registerFusionCalculations` (the two calculations; Vertical speed is
   `velD` itself); every count of twelve fusion plots in the documents and
   tests/README.md becomes fifteen.
7. (5, first bullet) Test: known answers on a fit result with chosen `velN`,
   `velE` and `velD`, every sample of `Fusion/velH` and `Fusion/vel` the GNSS
   definition applied to the fused components; both unavailable before the fit
   has published and when the inputs' lengths differ; both appear with the fit
   and start none.
8. (5, second bullet) Test: the category holds the fifteen in the order of
   1602; the three new rows are named, united and typed as their GNSS
   counterparts and backed by the explicit calculation; a legend value over
   each formats as its GNSS counterpart's does.
9. (5, third bullet) Test: checking Total speed alone on a fusable recording
   without a stored fit starts one fit; a stored fit draws the three without
   computing.
10. (5, fourth bullet) The audit's `naming` group: the pinned count of fusion
    plots is fifteen, with `fusionPlots()` of
    `tests/fusion/fusionsessions.cpp`; the rule that keeps the fit's own
    channels out of the plot registry drops `velD`, as it already omits
    `accD`, and its comment says why; the documents' pattern refuses a count
    of twelve; the comments and documents that counted twelve are updated; the
    audit and the acceptance-map check stay green.
11. (5, fifth bullet) The manual step M58: the three plots in the list in
    their place; one fit for the three; the three drawn beside the GNSS
    speeds, Wind-corrected horizontal speed and Speed accuracy, in the light
    and the dark theme, each fused line told from its twin and from the rest;
    the legend and the measure tool format each as its counterpart; a column
    over Total speed fills loaded and unloaded rows.
12. (5, sixth bullet) The acceptance items in the next free hundred of
    `tests/acceptance_map.txt`, with appendix Q and section 9.17 of
    `tests/README.md`; items 115, 509, 801, 802, 842, 854, 862 and 863, which
    counted the fusion plots, and 803, which listed the fused down velocity
    among the removed plots, restated "(as amended)".

## Appendix R. The acceptance items of the fused position and speed accuracy (1701-1717)

The acceptance items of the specification "Fused position and speed accuracy"
(`PLANS/fused-accuracies.md`), one sentence each, with the specification's
section in front. These are items 1701-1717 of `tests/acceptance_map.txt`
(item = 1700 + the number below) and the rows of section 9.18. Its sections 1
and 2 (motivation, principles) have no item. It amends the specifications of
the sensor fusion improvements (appendix C, clauses 30 and 35), of stored
requested results (appendix D, clause 37), of the fused state at every IMU
sample (appendix J, clause 12) and of GNSS holes bridged by the IMU (appendix
N, item 2): each is stated as amended.

1. (3) The twelve outputs: `Fusion/posCovNN`, `posCovNE`, `posCovND`,
   `posCovEE`, `posCovED`, `posCovDD` (m^2) and `velCovNN`, `velCovNE`,
   `velCovND`, `velCovEE`, `velCovED`, `velCovDD` (m^2/s^2) are measurement
   outputs of the fit after the four accuracies, aligned with
   `Fusion/_time`: the upper triangles of the position and velocity blocks
   of the composed sample covariance, turned from the reconstruction's
   tangent into the navigation frame with the published attitude as the
   attitude block is, a sample on a fix carrying the fix's own marginal;
   widened once, by the square of the sample's widening factor; filled for a
   success whose covariance was computed and empty for every other outcome,
   exactly when the four accuracies are, the diagnostics' `accuracy` account
   unchanged; their diagonal finite and non-negative wherever filled.
2. (3) `Fusion::Algorithm` is `batch-temperature-bias-v10`; a record written
   under `v9` is stale at load and computed again once, as a result is
   needed, shown like any computation; nothing else of the record's format
   changes; the fit's inputs, its solution, the seventeen state channels, the
   four accuracies and the diagnostics are unchanged and the goldens'
   existing channels remain bit for bit; neither `Cov(x_j, g)` nor the
   position-velocity cross block is published.
3. (8, first bullet) Test: on the fixtures with a computed covariance the
   published position and velocity blocks equal the position and velocity
   marginals of a graph with a state at every edge, and on a fix the fix's
   own marginal; they are symmetric with a finite, non-negative diagonal
   wherever filled, and empty exactly when the four accuracies are; on
   `bridged_hole` the composed velocity covariance before the widening is
   larger inside the hole than beside the fixes around it and returns at the
   next fix, the step chain's share of the position and velocity covariance
   grows across the hole and collapses at the next fix, and the composed
   position figures are recorded, not asserted (the hole's IMU factor ties the
   two fix marginals so that the position variance follows them across the
   hole), the published figures recorded beside them and not asserted, and
   the error against the generating trajectory, in units of the
   published standard deviation, is measured, recorded in section 8 of the
   fusion document and held below 3; the seventeen state channels and the
   four accuracies of every golden are unchanged bit for bit, and the goldens
   gain the twelve channels.
4. (8, second bullet) Test: thirty-three measurement outputs in order,
   thirty-four with the diagnostics; the twelve are published and stored with
   the rest and restored bit for bit after a restart; a record written under
   `v9` is stale at load and the fit runs again once; the stored success
   without the covariance restores the seventeen and leaves the sixteen
   accuracy channels unavailable.
5. (7) The documents of the fit's outputs and the record: `SENSOR_FUSION.md`
   section 4 (the attitude, position and velocity rows of `J_j` and their
   frame, the twelve in the outputs table, the squared widening, what the
   composed position and velocity covariance does through a hole), section
   7 (thirty-four outputs, `v10`) and section 8 (the validation with its
   measured numbers); `CALCULATIONS.md`'s thirty-four outputs;
   `DATA_SCHEMA.md`'s thirty-three measurements and `v10`; the contract of
   `registerFusionCalculations`; and every count of twenty-one or twenty-two
   channels in the documents, the comments and this file updated.
6. (8, audit bullet, first clause) The audit's `accuracy-channels` group: the
   twelve covariance names, quoted, are spelled in `src` in the
   registration's output table alone, the kernel holding them as `Result`
   members; the rule is planted once; the range 1701-1717 is declared and
   items 1701-1706 are complete; the audit and the map check are green.
7. (4) The derived accuracies, on demand under the Fusion sensor, with no
   title and no result version, registered after `accH` and before the
   system time in this order: `builtin.fusion.hAcc` (`Fusion/hAcc`, m, from
   `Fusion/posCovNN`, `posCovNE` and `posCovEE`), the square root of the
   larger eigenvalue of the horizontal block, one figure for the horizontal
   plane as `GNSS/hAcc` is, and the cautious one; `builtin.fusion.vAcc`
   (`Fusion/vAcc`, m, from `Fusion/posCovDD`), the square root of the down
   variance; and `builtin.fusion.sAcc` (`Fusion/sAcc`, m/s, from the six
   `velCov` channels, then `velN`, `velE` and `velD`), the standard
   deviation along the published velocity where the speed is at least that
   standard deviation, otherwise (a zero velocity included) the square root
   of the largest eigenvalue of the block: the rule of the horizontal
   acceleration accuracy, applied to the speed `GNSS/sAcc` qualifies. Each
   output has one candidate with the fit alone behind it, so the three
   appear with the fit and never start it; each is unavailable when an input
   is absent or the inputs differ in length, finite and non-negative
   otherwise, and applies no widening of its own.
8. (8, third bullet) Test: known answers on chosen blocks (a diagonal block,
   a rotated one whose larger eigenvalue is exact, a velocity along one axis
   and one below its standard deviation, a full 3x3 block); unavailable
   without the blocks and on unequal lengths; waiting on the fit and never
   starting it; appearing with the fit.
9. (7; 8, audit bullet, second clause) The documents and the audit: the
   derived table of `SENSOR_FUSION.md` section 4, the registration table and
   derived values of `CALCULATIONS.md` section 17 (seventeen calculations,
   eleven derived) and the contract of `registerFusionCalculations` describe
   the three; the audit's `accuracy-channels` group holds the three derived
   names, sensor-qualified, to the registration and the plot rows in `src`,
   and the rule is planted once; the range's completeness check covers
   1701-1709; the audit and the map check are green.

Items 10-17 follow with the plots (the specification's sections 5 and 6,
the three rows, their colours and demand, the manual step and the restated
items).
