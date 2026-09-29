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

[Appendix A. The acceptance items (1-19)](#appendix-a-the-acceptance-items-1-19)
[Appendix B. The acceptance items of sensor fusion and plot-driven jobs (101-120)](#appendix-b-the-acceptance-items-of-sensor-fusion-and-plot-driven-jobs-101-120)
[Appendix C. The acceptance items of the sensor fusion improvements (201-247)](#appendix-c-the-acceptance-items-of-the-sensor-fusion-improvements-201-247)
[Appendix D. The acceptance items of stored requested results (301-350)](#appendix-d-the-acceptance-items-of-stored-requested-results-301-350)
[Appendix E. The acceptance items of stored-result validity (401-442)](#appendix-e-the-acceptance-items-of-stored-result-validity-401-442)
[Appendix F. The acceptance items of demand-driven requested calculations (501-563)](#appendix-f-the-acceptance-items-of-demand-driven-requested-calculations-501-563)
[Appendix G. The acceptance items of calculation refinements (601-662)](#appendix-g-the-acceptance-items-of-calculation-refinements-601-662)
[Appendix H. The acceptance items of one status bar for background work (701-754)](#appendix-h-the-acceptance-items-of-one-status-bar-for-background-work-701-754)
[Appendix I. The acceptance items of sensor fusion plots, attitude and the orientation attribute (801-863)](#appendix-i-the-acceptance-items-of-sensor-fusion-plots-attitude-and-the-orientation-attribute-801-863)

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

There are 50 test executables plus the audit. `ctest -N` lists 51 entries, or
58 where the bit-exact runs of the seven fusion golden tests are registered
(`tst_fusion_*_exact`: the same executables a second time, label `exact`,
Release only; sections 3 and 11). `solver_deploy_probe`,
`fusion_golden_capture` and `fusion_runner` are also built, but are not tests
(see below).

**Harness**

| Test | Covers |
|------|--------|
| `tst_harness` | The test-support code itself: settings and logbook isolation, fixture builders |
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
| `tst_builtins_engine` | The built-ins on a private registry and `FakeSessionState`: golden values, registration inventory, declared inputs only, multi-output groups, candidate order, the declared preference, interpolation family, altitude descriptor, the constant defaults (`constantDefaults`), the column environment digest: per set of names, changed only by what their static closure reaches (`digestChanges`), covering every candidate's result version (`digestCoversResultVersions`) and the conversion layer (`digestCoversConversionLayer`), the same for an altitude marker registered at run time or at the next start and unchanged for every other name (`digestSurvivesRuntimeAltitudeMarker`), and the altitude-marker manager's destructor removing its registrations as teardown, which reports no drop while a marker removed at run time does (`altitudeMarkerTeardownReportsNothing`) (fusion-plots items 810, 834-836, 838, 846, 851, 860) |
| `tst_time_fit` | The system-time-to-UTC fit: microsecond-level conversion of an exact synthetic clock at high device uptime (the regression test of the centered sums), invalidation through the TIME sensor, GPS week rollover, degenerate clocks |
| `tst_local_coordinates` | The recording-wide `Local` frame: origin gates, analytically known displacements and velocity rotation on WGS84, NaN at the index of an invalid sample only, all outputs unavailable without a qualifying fix, the shared GNSS time axes, invalidation on source changes and independence from markers on a real `SessionData` |
| `tst_simplified_track` | The simplified map track on the shared `Local` frame: all seven outputs at the same retained sample indices, every dropped sample within 0.5 m of the path and the strictly-greater rule, duplicate-position endpoints, closed, degenerate and empty tracks, non-finite samples left out, one projection per recording, unavailable without a local origin and back after a source correction, siblings invalidated together |
| `tst_session_engine` | `SessionData` on the engine with the real built-ins: run-once, invalidation, candidate replacement, overrides, preferences, the fresh-evaluation oracle, copy/move semantics; an explicit-policy calculation and a throwing / nested / cyclic set of calculations registered temporarily on the global registry (acceptance 14, 12); an asynchronous request against real session ownership (published, session moved, destroyed, move- and copy-assigned over, declared input edited) |
| `tst_session_model_engine` | `SessionModel` + `AltitudeMarkerManager`: registry and preference broadcasts reaching `dependencyChanged`, coalescing, merges, rows surviving sort, the read-only `DEVICE_ID` column, a marker only for an altitude whose calculation registered; the executor's hooks: counted session pins that defer LRU eviction (and nothing else), the idle scheduler's `unregisterTask` and replacing `registerTask` (`schedulerTaskCanBeUnregistered`) and a task with work it cannot step, which the scheduler waits on without spinning (`schedulerWaitingTaskDoesNotSpin`); a task that can wait and loses its work without a step is completed once, not cancelled, after a final progress report and before the next active task or idle, while a task without `canStep` is not (`schedulerCompletesWaitingTaskWhoseWorkIsGone`); the read-only `hasWork()`, true while some task has work, whether or not it can step, the fill's kind included, and neither ticking nor waking (`schedulerHasWorkFollowsItsTasks`; status-bar items 706, 739); a bulk edit publishing its edit as a dependency change on both of its paths (`bulkEditAnnouncesADependencyChange`), and immediate publication of engine-returned invalidations without any persistent effect |
| `tst_result_store` | Stored results of explicit calculations on a real model and logbook (synthetic calculations): written on an Ok install (also before a new session's first save), deleted on an input change, restored on every load path, upstream records first, so that a record whose lookup has a fallback candidate restores in any id order (a session without a known record is not listed), stale records deleted; the bulk edit's temporary load never reads one, the column worker's copy only when a missing column needs it (then with the checks of a load); write failures, each announced with its reason after the record change of the pair (`writeFailureIsAnnounced`); an explicit family instance is not stored; a registry change made while the application runs that changes what a name a result looked up resolves to deletes its record, a candidate registered behind the provider and a teardown removal do not; unrelated registrations and the descent-pause preference keep it valid across loads and restarts; a lookup that resolves differently, or a changed plug-in code identity of a calculation the result read, makes it stale; an unreadable record (a directory at its path, a Windows lock, POSIX permissions; rows skip where the platform does not honour them) is skipped and restored at a later load, a record that reads it is kept, the next publish replaces it (the column value over it then reaches `index.json`), and deleting its session removes it, leaves a locked file as a stray for the next start, or leaves a directory (never a record); a format-1 record is deleted; the reason of a stored rejection recorded in `index.json` at the write and at every restore, and a changed reason announced as a record change (`recordReasonRecordedAtWriteAndRestore`) |
| `tst_result_columns` | Logbook columns over explicit results: cached from the restored or published result with a record stamp in index.json, dropped when a record is written or deleted, filled for an unloaded session with a record by the column worker from its stored results restored into a temporary copy (the row never loaded, no record written, nothing run; a stale record deleted and the value cached unavailable; an unreadable one skipped and the value pending; no record read while the missing columns are all on demand), crash points, old indexes, write failures, and a `cache/` folder deleted while the application was closed (no record, nothing requested, the values over it dropped at start-up, nothing run); values over a record skipped at a load never cached while it is skipped; a registry change that does not reach a result (also a provider registered behind a stored input) keeps its row confirmed, one that does deletes the record and its stamp (`registryChangeKeepsLoadedRowConfirmed`); an environment change discards cached values while the record restores; the column worker's behaviour and statistics unchanged with column demand active, and a stale record it deletes moving the pair into demand (`columnWorkerIsUnchangedByDemand`, `staleRecordDeletedByWorkerCreatesDemand`); the session model's column knowledge - each enabled column's requested calculations and closure, current before the queued environment check (`sessionModelExposesColumnKnowledge`) - and a row's display name for loaded rows, stubs and failed-load placeholders (`sessionDisplayNameOfEveryRowKind`) |
| `tst_session_oracle` | The session-level idempotency oracle (section 7): randomized, seeded sequences of reads, edits, merges, preference and registry changes on real `SessionData` objects (part A) and on the real `SessionModel` / `LogbookManager` through the application's import path, with restarts, simulated crashes and a persisted-state check (part B), compared against a fresh evaluation |

**The executor, the demand layer and the demand views** (synthetic explicit calculations; no GTSAM)

| Test | Covers |
|------|--------|
| `tst_jobqueue` | The executor, `JobQueue`, on a real `SessionModel`, real session engines and the global registry, with the synthetic explicit calculations of `jobfixture.h` (no GTSAM): publication through the session model, the 64 MiB worker thread at below-normal priority (`workerRunsBelowNormalPriority`), the main thread free while a job computes (`mainThreadIsNotBlockedByARunningJob`), at most the running job and one chosen next job (`holdsAtMostRunningAndChosenNext`), an equal offer creating nothing, a different offer replacing the chosen next job, which ends Cancelled "No longer needed" (`offerReplacesChosenNext`), withdrawing it (`withdrawEndsChosenNext`), one job at a time in offer order, refusals (missing input, unloaded or unknown session, blocked, done, unknown), never loading a session, every superseded / succeeded / failed / cancelled path with its reason text, a running job whose ticket went stale (input edit, merge, registration removed, model destroyed, the row's session data replaced - "Session data replaced", not "removed or unloaded") asked to stop at once and ended Superseded with the refusal's reason without its compute reaching the end, a new offer behind it becoming the chosen next job and run with the new inputs, first writer wins between a user cancel and a stale stop, cancel wins over a completed compute, a job cancelled from a `rowsInserted` slot (no pin left), session removal, deferred eviction, repopulation, merge, sort, shutdown in every order, the idle scheduler working during a job, and the job record naming its session by the session model's display name (`runsAndPublishes`) (sensor-fusion-jobs acceptance 8, 10, 11, 14, 17; demand items 521, 540, 544, 560, 561; refinement items 606, 624, 628, 629) |
| `tst_jobmodel` | The `JobModel` contract under `QAbstractItemModelTester`: every role on every column, a test view that renders the whole job history from model signals alone, never more than one running row, a replaced chosen next job recorded as Cancelled "No longer needed", ordered UTC timestamps, progress and cancel-requested as their own signals, removal of finished rows only, the retention bound, and no job persisted: the settings and the logbook folder are byte-identical after jobs of every ending, except for the stored results of the two jobs that published `Ok` (sensor-fusion-jobs acceptance 18; store-requested item 309) |
| `tst_calculation_demand` | `CalculationDemand`, the widget-free demand layer, on a real `PlotModel`, executor, `SessionModel`, `LogbookColumnStore`, logbook, real session engines and the global registry, with the synthetic plots of `plotfixture.h` (no widgets, no GTSAM). **Plot demand:** checking a plot starts the visible sessions without a result, showing a session or loading a visible one starts it, hiding drops its waiting pair and unchecking all of them while the running job finishes and is stored (`rowScript`, `showingASessionStartsIt`, `hidingASessionDropsItsWaitingPair`, `uncheckingDropsWaitingPairsKeepsRunning`); a waiting pair whose result appears by other means dropped before it starts (`resultAppearingWhileWaitingDropsThePair`); a click, `setPlotEnabled`, `togglePlot`, `setData` and a profile (applied by `PlotModel::setEnabledPlotIds()`, as `applyProfile()` does) create the same demand, the start-up restore with every session hidden none; a profile naming plots the model does not have enables its others and ignores those, silently: no message of any type, no row added (`profileNamingRemovedPlotsAppliesWithoutThem`); chained calculations upstream first, the executor never idle between links; the focused session, then row order, the chosen next job replaced when demand changes, never more than the running and one chosen next job; the one-second input-settle wait (a burst runs one job, counted from the first change); failures (an input-determined one stored, listed and never re-run; a job-level one listed, not re-run in the run and re-run by a new demand layer, as after a restart); not-applicable sessions never listed; progress, failures and their change signals (`changeSignalsAreMinimal`), coalesced passes, `isMerelyUncomputed()` (the plot widget's "No data available" warning is withheld for a value that waits on a requested calculation or was rejected by one, and for nothing else); session removal, registry changes, executor shutdown and null collaborators. **Column demand:** enabling a column over a requested output fills every session of the logbook, loading sessions that are not loaded at most two at a time as hidden, pinned sessions that leave by ordinary eviction (`enablingColumnFillsEveryUnloadedSession`), a session shown meanwhile running next, stored results creating no job (a stored rejection of a session that is not loaded listed with its reason after a restart, without a load), one pair memory (a refused offer or a column's not-applicable verdict; a failed job, load or record write; an exception result) that keeps a session from being loaded or a pair from being offered again after eviction, a sort or the column worker's pass (`settledPairsSurviveEvictionSortAndColumnWorker`), cleared by a record change, an input change or a registry change (`pairMemoryIsClearedByRecordInputAndRegistryChanges`), a column's verdict for that column only (`columnVerdictDoesNotSuppressAnotherColumn`), a column pair the executor refuses at offer time remembered and followed by a pass so that its cell does not stay pending (`columnOfferRefusalIsNotLeftPending`), chains keeping their hold, holds released on disable, show, removal, repopulation and destruction, the identity stub offered under its real id, the column's progress and pending cells (`columnProgressAndPendingCells`), the load step below saves, bulk edits and column work, not cancellable and reporting no progress of its own (`fillTaskRestsWhileWaiting`), and a pass over 2000 stubs reading each record set once. **Refinements:** one walk for plots and columns, a column track running on one blocker filing its others (`runningColumnTrackFilesItsOtherBlockers`); a record that could not be written listed among the failures, for a column and for a plot alike, not retried in the run, cleared by a later write, tried again after a restart (`failedRecordWriteIsShownAndNotRetried`, `failedRecordWriteIsShownOnThePlotRow`); a reason learned by the column worker's restore reaching the demand layer through the record change alone (`recordReasonReachesDemandThroughRecordChange`); column knowledge from the session model (`columnKnowledgeComesFromTheSessionModel`); the choice acting on the executor's answer and withdrawing a chosen next job nothing wants (`chosenNextJobFollowsTheExecutorsAnswer`); the settle clock (`settleClockAnswersItsQuestions`); the fill completed by the scheduler, a new burst of computations after a fill that ends behind another task starting its own high-water mark (`fillEndingBehindAnotherTaskStartsNextCountFresh`), holds released at the executor's shutdown; a successful load forgetting a session's failed-load facts (`successfulLoadForgetsFailedLoadFacts`). **Progress and failures:** the sessions with a waiting or running track counted once across plots and columns, a stub with a waiting cell included, out of a high-water mark that holds until the count is 0, and the running recording with its step (`progressCountsEachSessionOnce`); the step changing without a pass (`progressTextWithoutAPass`); one entry per recording in row order, each pair once, with its title, its reason and whether the next start tries it again (`failuresAreOnePerSessionInRowOrder`, `failuresNameEachCalculationOnce`); a stored rejection a failure of a new demand layer without a load, an unstored one only once its retry fails again (`storedRejectionIsAFailureWithoutLoad`, `unstoredFailureReturnsOnlyWhenItFailsAgain`); failures clearing as the pair memory clears and following what is switched on (`failuresClearAsThePairMemoryClears`, `failuresFollowWhatIsSwitchedOn`); the pending cells' own announcement per column (`pendingCellsChangedPerColumn`); the one text form of the failures and its limit of ten (`failureTextAndItsLimit`) (sensor-fusion-jobs acceptance 11, 13, 15, 16; demand items 501-560; refinement items 601-604, 607, 609, 611, 612, 615, 616, 618-620, 622, 624-627, 630-643, 645, 647, 648, 651-659; status-bar items 703-706, 711, 713-717, 720, 727-732, 736, 740, 744, 745, 748-750, 753; fusion-plots items 805, 845, 852, 855) |
| `tst_logbook_indicators` | `LogbookView` with `LogbookCellDelegate` on the tree's own header, in an offscreen window beside a reference `QTreeView` with the base delegate, on a real demand layer, executor and `SessionModel`; the first of the three tests that link Qt Widgets (`FLYSIGHT_BUILD_WIDGET_TESTS`, label `widgets`). Without a failure the header and every cell that is not pending are the reference's, with the same sizes, while a requested column works and once it has finished (`plainHeaderAndCellsAreIdenticalToBase`); the header is the tree's own `QHeaderView`, identical to the reference's and reserving no room while a column works and after one has failed, nothing repaints by itself while a column works, and the view has no progress bar or cancel button (`headerIsPlainAndNothingAnimates`); a recording with a current failure shows one glyph, the style's warning icon, right after the text of its row's first visual cell, attached to it and leaving the text where it was, for a loaded row and for one that stays unloaded, every other cell (the failed calculation's blank one included) the base delegate's and every size unchanged (`rowWarningFollowsTheText`); the hover over the glyph is exactly `SessionFailures::text()`, the rest of a pending first cell keeping the pending tooltip (`rowWarningHoverIsTheSessionsFailures`); the glyph in the new first visual cell after a section is moved to the front or hidden, a sort and a rebuild of the columns (`rowWarningFollowsTheFirstVisualColumn`); the glyph appearing when a job fails and going after an input change whose retry succeeds and when the last source is disabled (`rowWarningFollowsFailures`); a session file that cannot be loaded warning on its row, its cell not pending (`failedLoadSessionShowsRowWarningNotPending`); a record that could not be written named in the row's hover, tried again at the next start (`failedWriteIsListedInTheHover`); a click on the glyph selecting as a click elsewhere in the cell, starting and cancelling nothing (`clickOnRowWarningIsAClickOnTheCell`); pending cells distinct from unavailable ones and from the unreadable-record state, never in the model or `index.json`, sorted as unavailable, and replaced by the value when the record is written; `pendingCellsChanged` repainting that column and `failuresChanged` the first visual column, with no model signal (`pendingCellsChangeRepaintsOnlyThatColumn`); a demand layer destroyed first, with a glyph and pending cells shown (`survivesDemandDestroyedFirst`) (demand items 526, 527, 533-538, 543, 547, 557, 558; refinement items 618, 633, 644, 646, 648-650, 652, 660; status-bar items 718-726, 731, 735-737, 741, 744, 750-752) |
| `tst_status_bar` | `StatusBarFeature` in the status bar of an offscreen `QMainWindow`, on a real `SessionModel` with its idle scheduler, executor and demand layer, with the synthetic calculations of `jobfixture.h` and the plots of `plotfixture.h`; the second of the three tests that link Qt Widgets (label `widgets`). The scheduler's four tasks under their labels, with the scheduler's count for that id on the label and the bar, a report for another id changing nothing, and idle leaving the activity area empty (`schedulerTasksShowTheirLabelsAndCounts`); the computations as one item, counting the sessions of plots and columns together, each once, out of the high-water mark, shown while the fill is the active task, the fill never reporting a progress of its own, and a later burst starting its own total (`computationsAreOneItem`); a task shown over the computations, the hover listing both with the recording being computed and its step, and the computations returning when the task ends (`taskShownOverComputationsAndHoverListsBoth`); the cancel button exactly while the shown item is a cancellable task, never for saving, the fill or the computations, and a click cancelling a visible load (`cancelOnlyForACancellableShownTask`); the warning beside the computations and then alone, counting recordings with the style's warning icon, its hover the capped list of `SessionFailures::listText()` (`warningBesideComputationsThenAlone`, `warningCountsRecordingsAndListsThem`, `warningListsAtMostTenRecordings`), absent without a failure, not dismissed by a click and gone with the last source (`warningAbsentWhenNothingFailedAndNotDismissable`), and after a restart counting a stored rejection without a load, also from an index without "recordReasons", and an unstored failure only once its retry fails again (`warningAfterRestart`); one height idle, with a task, the cancel button, the warning and both (`heightNeverChanges`); the activity at the left, label then a compact bar then the cancel button, the warning at the right, and no width kept by a hidden widget (`activityLeftAndWarningRight`); a demand layer destroyed first, and none at all (`survivesDemandDestroyedFirst`) (demand items 530, 534-537, 539, 559; refinement items 612, 644, 645, 647, 650, 653, 660; status-bar items 701-705, 707-716, 725, 727, 733, 734, 737-739, 745-750) |
| `tst_choice_attribute` | The Choice attribute format type (`AttributeFormatType::Choice`) through the logbook, on a real `SessionModel` and logbook, with Choice attributes the test registers (three choices whose label, token and definition orders all differ, one token holding a comma; one attribute with a no-input default calculation, one without) and driven through `ChoiceFixture` (`support/choicefixture.h`); the third of the three tests that link Qt Widgets (label `widgets`), with a `LogbookView` and no demand layer. On loaded rows and on stubs after a restart: a cell shows the label of the effective token, stored or calculated, and a token outside the list (planted with `updateAttribute()`) its raw text (`choiceShowsTheLabelOfTheEffectiveToken`); sorting orders by that text, missing values last both ways (`choiceSortsByLabel`); `setData()` stores a token of the list verbatim (`$VAR,<key>,b,1`), shows its label and publishes the change, refuses the stored token again, and compares with the stored value, so the default's own token can be pinned (`choiceEditStoresAToken`); a token outside the list, a label, an empty string or another type is refused with nothing emitted, saved or loaded (`choiceEditRefusesATokenOutsideTheList`); an invalid value removes the stored attribute, the default's label returning, and is refused when nothing is stored (`choiceDefaultRemovesTheStoredValue`); the bulk edit sets a token on loaded and stub rows, files, `index.json` and cells agreeing (`bulkEditSetsAToken`), removes it for an invalid value (`bulkEditDefaultRemovesTheStoredValue`) and queues nothing for any other value, the bulk edit task never active (`bulkEditRefusesATokenOutsideTheList`); the in-place editor is a non-editable `QComboBox` of "Default" then the labels in definition order, opening on the cell's label or on no entry for a raw token, a label storing its token, "Default" removing the stored value and Enter on the opening entry writing nothing, a Text cell keeping the base class's `QLineEdit` (`cellEditorOffersTheList`); the "Set ..." dialog, `LogbookView::askAndSetAttribute()` driven through its modal `QInputDialog` by a timer, is a non-editable list of the same entries opening on "Default", a label bulk-editing its token onto the given sessions, "Default" removing it and a rejection changing nothing, a Text attribute keeping the text prompt (`setDialogOffersTheList`) (fusion-plots items 823-833, 841, 844, 858, 859) |

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
| `tst_persistence_roundtrip` | Save / reload on the real importer, exporter and logbook: acceptance 5 (bit-identical samples, units and header attributes preserved, `SCHEMA_VER` only if recorded, effective values unchanged, second cycle byte-identical, independent of any cache) and acceptance 6 (a released logbook file is not rescaled, relabelled or stamped by a save; the `loadSession` backfill is additive and idempotent (mass and area; wind is not backfilled)); non-finite samples, ragged sensors, unrepresentable text; the file writer and the in-memory writer agree (also across the 4 MB flush boundary); an unsupported stored `SCHEMA_VER` is never written |
| `tst_logbook_index` | `LogbookManager`'s `index.json` column cache (and each session's `"recordReasons"`, `recordReasonsRoundTrip`; a changed reason announced as a record change, an unchanged one silent, `recordReasonChangeIsAnnounced`): the calculation-compatibility marker gates every cached value and each column's recorded environment the values of that column (acceptance 18 at the storage level; `differentColumnEnvironmentDiscardsThatColumn`, `missingColumnEnvironmentsDiscardOnce`, `environmentIsTheCachedOne`), unsaved-column tracking and save ordering (an interrupted save never leaves a cached column that disagrees with the session file), orphan session files adopted, marks follow remap / remove / reset; the raw load with its failure reason, the legacy backfill as a separate step, identity-entry queries, a legacy flat index coming up as stubs without rewriting a session file |
| `tst_result_records` | Stored requested-calculation results: the record file name (percent-encoded calculation id, canonical, dot-free, distinct under case folding; the parse of a name), the code stamp (the compatibility marker) computed fresh, never made stale by a registration, the binary record format (bit-exact round trip of `-0`, NaN payloads, infinities and subnormals, null / empty / non-ASCII strings and unavailable outputs; a round trip of every accepted attribute type; the pinned byte layout of format version 2, the resolutions included; other format versions (format 1, in both of its layouts, included), damaged and crafted payloads refused without allocating; every other attribute type, `long` and `unsigned long` included, refused at encode; size), and `LogbookManager`'s record files in the logbook's `cache/` folder: a short read is `Unreadable`, never decoded (`readWholeDevice()` on a buffer that holds less than expected: no file system gives a short read on demand); write, read, replace, list, remove, the folder created by the first write only (a missing folder holds no record), write failures leaving the previous record intact (a `cache/` that cannot be created included), removal with the session (dotted identity stems), stray records removed from `cache/` by `initialize()` in all three index branches, `sessions/` untouched (a name spelling the extension in another case is not a record: neither listed nor removed), orphan adoption, remap, and a session save that never depends on records |
| `tst_column_cache` | The same through `SessionModel`: upgrade discards and lazily recomputes (acceptance 18), an edit refreshes only the affected columns with a warm and a cold engine, merges and bulk edits, interrupted saves, environment changes discarding, in loaded and unloaded rows and without saving, exactly the columns whose environment they change: a declared preference only the columns that read it (`preferenceChangeDiscardsOnlyReadingColumns`), a registration only the columns whose closure it reaches (`environmentCheckDropsExactlyTheReachedColumn`), new altitude markers no column at all, at run time and after a restart (`altitudeMarkerChangeKeepsOtherColumns`), and a value computed after a change but before the queued check (an eviction) stored and flushed only under its column's new environment (`valueComputedBeforeCheckIsStoredUnderItsEnvironment`), save failures (the row stays dirty and loaded, is skipped by the idle saver and the LRU, stays out of the index, and is saved by a later edit or the shutdown flush), line breaks flattened at edit, a column over an explicit result cached from the result the session has and following its record, shown by a stub after a restart without a load (`explicitBackedColumnFollowsItsResult`), a plug-in edit (a changed plug-in code identity) discarding the cached values of the plug-in column at the next start and keeping the others (`pluginEditDiscardsPluginColumns`), and `loadPinnedSession()`: a hidden session loaded the way showing it would, pinned under its corrected id, nothing pinned for a file that cannot be loaded (`loadPinnedSessionLoadsWithoutShowing`, `loadPinnedSessionFollowsIdentityRemap`, `loadPinnedSessionFailedLoadPinsNothing`) |

**Import and merge, workflows**

| Test | Covers |
|------|--------|
| `tst_session_merge` | `SessionMerge`, the pure plan-then-apply merge (attribute conflict rule, measurement merge) on programmatic sessions: absent attributes added, equal ones ignored, different header attributes conflict (all reported, sorted, with the delete-and-re-import hint), `_` attributes keep the session's value, the `n/a` device placeholder counts as absent, equality on the on-disk text form, columns replaced / added / kept with samples and unit together, bitwise column comparison (NaN, `-0`), the ragged rule, purity of `plan()`, the invalidation set of `apply()` |
| `tst_import_merge` | The import path (`SessionImport::importFiles` -> `SessionModel::mergeSessions`) against a temporary logbook: acceptance 3 (a rejected file leaves the session untouched), 7 (TRACK/SENSOR order independence loaded, unloaded and in one batch; conflicts change nothing; edits and unmatched measurements survive), 8 (the `SCHEMA_VER` escape hatch), 10 and 18 (merge parts); defaults only at creation, failed loads are errors, failed-load placeholders are never saved, identity stubs are matched, identical re-imports are no-ops |
| `tst_import_batch` | `SessionImport`: one result per file in input order with parse failures included, cancellation through the progress callback, and the text of the import-failure dialog with each file's error |
| `tst_workflow` | End-to-end workflows on the application's own code path (acceptance 19): import through `SessionImport::importFiles`, rows and columns, marker and attribute edits, save, reopen as stubs served from `index.json`; the model's warm save equals a cold export (acceptance 5); a released session file together with a released `index.json` (acceptance 6, 18) |
| `tst_map_models` | `TrackMapModel` and `MapCursorDotModel` on a real `SessionModel`: a recording without a local-frame origin has no track and no cursor dot and is left out of the bounds, the bounds are cleared when no track remains, and all of it returns after a source correction through `mergeSessions`; hidden recordings and the plot-range filter on a recovered track. It compiles the two map models and their helpers (`plotrangemodel.cpp`, `plotutils.cpp`) directly and needs no Widgets or WebEngine |

**Python plugins**

| Test | Covers |
|------|--------|
| `tst_python_bridge` | The Python plugin bridge through the real embedded interpreter and the real `flysight_cpp_bridge` module (acceptance 17, plugin half): effective reads in single-output plugins, declared-read diagnostics (`UndeclaredInputError`), effective values and units matching C++, no source access (a `source` key is an unknown kind; the view has no source methods), the multi-output form running once, exceptions and malformed output giving a clean unavailable result with negative caching, returned arrays copied, explicit key decoding with per-plugin rejection, plugin-before-built-in precedence, the bundled `imu_tilt.py` example; plugins never start explicit work (`pluginsNeverStartExplicitWork`, on the synthetic explicit calculation `expA`: a plugin that declares its output, one that declares an on-demand value derived from it, and one whose Python code reaches for both undeclared all stay unavailable with a run count of 0 until the calculation is requested, and the first two show the value afterwards). See "The embedded-Python bridge test" below. `pluginWorkflowThroughModel`: a plugin-fed logbook column through import, save and restart (acceptance 17 / 19); every plugin registration declares the plug-in code identity as its result version, equal to the digest recomputed from the folder, the SDK and the versions read independently (`pluginRegistrationsCarryCodeIdentity`) |
| `tst_plugin_identity` | The plug-in code identity (`src/plugincodeidentity.h`) without Python (label `core` only): the pinned encoding (a hand-built byte string for `a.py` and `pkg/helper.py` passed out of order), determinism and independence of the order files are passed in, each ingredient changing it (a file's bytes, a rename, a helper module added or removed, the SDK's bytes or readability, a file's readability, the Python and NumPy versions, bytes moved between files), the token `none` for a version that cannot be read, and the folder walk: every `*.py` under the folder, subfolders included, in name order with `/` names, hidden files counted (a dot-named one; on Windows one with the hidden attribute), `__pycache__` and hidden folders left out, a linked folder read through under its own name and a link loop ended (a symbolic link elsewhere, a junction made with `mklink /J` on Windows; skipped where neither can be made) |

**Solver and sensor fusion** (label `fusion`; `FLYSIGHT_BUILD_FUSION_TESTS`)

| Test | Covers |
|------|--------|
| `tst_solver_smoke` | GTSAM's exported CMake target compiles, links and runs in a test: the install is the shipped configuration (`4.3a0`, TBB on, bundled Eigen 3.4, built without Boost: `GTSAM_ENABLE_BOOST_SERIALIZATION` and `GTSAM_USE_BOOST_FEATURES` are `0`), a small pose graph optimizes to its analytic answer (Eigen, METIS, TBB, library loading), and the main thread really has the 64 MiB stack of `flysight_solver_stack()` (the test uses 48 MiB of it; with a default stack it crashes). Label `fusion`. Nothing from the fusion model is involved |
| `tst_fusion_golden` | The fusion kernel (`flysight_fusion`) through its public API, `src/fusion/fusion.h`, only. What it reaches: `Fusion::run()` on the twelve committed synthetic fixtures and nothing internal. In spec order: the initializer's prefix and segment fits are boundaries of the same kinds as the full fit's, so cancellation at each kind of boundary (`Starting fit`, graph construction, a prefix fit iteration, a segment fit iteration, a full fit iteration) leaves an empty result and no state behind, and preparation has no boundary of its own (the first is `Starting fit`); the progress texts at the kernel's boundaries, prefix and segment texts included, are the golden's; two runs are bit-identical with TBB on, a 64 MiB worker thread matches the main thread, and nothing depends on the caller's data. The golden comparison: for every fixture the fit reproduces the goldens captured from the kernel by `fusion_golden_capture` (three successes: seventeen channels and the diagnostics with their `initializer`, `stopping`, `quality` and `model` objects; nine rejections: the exact reason), the channel writer of the capture tool is the inverse of the loader on the committed files (and the hex sample form round-trips signed zero, a NaN and a subnormal by bit pattern), and the comparator holds its bounds (sensor-fusion-jobs acceptance 4; section 11). Label `fusion` |
| `tst_fusion_kernel` | The kernel's internals through the seams of `src/fusion/` (the only test that includes those headers), with the literal expectations of the reference's self-test: the shared unwrap rule, preintegration across exact boundaries, every validation defect, backward attitude propagation, heading freedom, dense reconstruction timing and endpoint correction, an exact constant-velocity fit, TBB really on. In spec order: the segmented initializer (segment cutting on fixes and the merge of a short final piece, a window shorter than one segment, the smallest-sAcc anchor and the carried-back start, prefix growth on the marginal yaw sigma about the vertical, the growth stop when a doubling gains nothing, the prefix budget of one pass and 50 iterations and starts that end on the limit, the fallback when every prefix start fails, its progress texts and diagnostics keys, the four synthetic initializer recordings of the specification and `coarse_maneuver`); the stopping rule (the bias-settled cost test, the slow tail accepted and refused on each bound, non-convergence and a never-settling bias as solver failures with their failure shapes); the per-step IMU noise term (the density covariance exactly without a signal change, the specified covariance for a known change, the `dt` scaling, the constants in `model.per_step`); the temperature-dependent gyro bias (the custom IMU factor's six Jacobians against finite differences and its equivalence with `ImuFactor` at zero slope, the graph shape with `T_ref` and the slope prior last, the reconstruction at each interval's own bias, a recording without the temperature channel rejected by name, a constant temperature leaving `b1` at its prior and agreeing with the constant-bias fit, the drifting-bias recording recovering `b1` within 20 % in at most 30 iterations). The golden comparison: the fit trace (the segment account and the cost before and after every optimizer iteration) against the goldens, which localizes a golden failure to a stage, and the chosen prefix fit's iteration count against the golden's (acceptance 4; section 11). Label `fusion` |
| `tst_fusion_session` | Sensor fusion as a registered calculation (`src/fusion/fusionregistration.cpp`) on real `SessionData` engines bound to the global registry, with the real fit on the test's main thread. What it reaches: the engine's request, prepare / compute / publish and blocker paths on fixture sessions whose effective inputs are bit-identical to the kernel's fixtures, and a natural session through the real input chain. The registration's shape: eight registrations, the fit with 22 inputs (all required, `IMU/temperature` the last measurement) and 18 outputs, explicit, title "Sensor fusion". In spec order: cancellation at each kind of boundary through the engine's facility publishes and caches nothing (sensor-fusion-jobs acceptance 10); a session with the temperature column carries it to the kernel bit for bit and matches the kernel's direct run, and a session without `IMU/temperature` is `MissingInput` / `NotApplicable` like one without IMU data, a local origin or a time fit (11). The lifecycle: reads of every fusion value, `accH`, the system-time axis, the diagnostics and an interpolated logbook value never run the fit, in any order, nor does the exporter (5); a request runs once, publishes all outputs together, and brings `accH` and `_system_time` with it (6); prepare / compute / publish equals `request()` bit for bit (7); an input change after publication drops everything while markers do not (8); a rejection is a cached result with its reason, `NotProduced` for inspection, and requestable again after an input change (9); blocker inspection reports the fit through on-demand intermediates and never starts it (12); two sessions are independent. The fit declares its kernel's algorithm string as its result version, no output of an explicit built-in has another candidate (`explicitOutputsHaveOneCandidate`), and a fit exported from one session and restored into another is indistinguishable from the fresh one: every channel bit for bit, the diagnostics byte for byte, status, detail, dependency edges (`SCHEMA_VER` among its leaves), blockers and invalidation, and the fit's snapshot lists what provided each name it looked up (`restoredFitIsIndistinguishable`). The golden comparison: every published result equals the kernel's goldens. Label `fusion` |
| `tst_fusion_derived` | What is derived on demand from the fit's published outputs (`src/fusion/fusionregistration.cpp`), without the solver: the fit's outputs are stored as data on real `SessionData` engines bound to the global registry (`syntheticFitSession()`, section 11), and every expected value is an exactly representable literal or, for "one definition", the GNSS calculation on the same samples. The seam's premise: stored `Fusion/<name>` reads back bit for bit and the fit never runs. The registrations of `builtin.fusion.z`, `accAlongTrack` and `accCrossTrack`: on demand, no title or result version, their inputs in order (the track accelerations those of the GNSS ones), one candidate each, the fit their one explicit dependency, and `Fusion/accD` with no second producer (vertical acceleration). On a fixture session that has the fit's inputs and no fit, each waits on the fit alone: `Blocked` by it, merely uncomputed, unavailable, and nothing runs it. Elevation is `_LOCAL_ORIGIN_HMSL` less `down` less `_GROUND_ELEV`, recomputed from an attribute edit without the fit and unavailable when either attribute is not a number. The track accelerations' known answers: along north, reversed, a vertical descent, a wind that turns a skewed ground velocity north, no motion through the air, a wind that is not a number (zero), no stored wind (the constant zero default) and unequal lengths (unavailable). The fused and the GNSS track accelerations agree on the same samples and wind (bit-exact in exact mode, within 4 ulp otherwise). The orientation vocabulary (`src/fusion/orientation.h`): exactly 24 pairs in the enumeration order, the default first, distinct tokens and labels, each token parsing back and nothing else parsing, every body-to-device rotation exact and proper with the columns forward, forward x up and -up; the attribute's one definition, its choices the enumeration, also after the entry point registers on a private registry. The attitude (`builtin.fusion.attitude`, after the orientation's constant default `builtin.default._ORIENTATION`): its registration, waiting on the fit on a fixture session, and on the quaternion stored as data (expected values built by hand from Euler angles and a hand-written default mount, within 1e-9 degrees): a level north-facing body, known heading, pitch and roll, two turns of heading unwrapped and offset by the course reference exactly as `GNSS/course` is (and unavailable exactly when it is), roll and pitch in their ranges through a barrel roll and a loop, the forward axis exactly vertical (the body pitched straight up and down between ordinary samples under the default orientation, and a level device under the mounts forward +z and forward -z, the sample whose pitch argument rounds past 1): all three angles published and finite, pitch at +90 or -90, roll in range and the later samples still reading their own angles, side mounts, the fit's own yaw, pitch and roll from a success golden's quaternion under forward +x, up -z (within 1e-6 degrees), an invalid stored orientation (unavailable) and a stored one recomputed without a fit. The Orientation column's model and bulk edit through `ChoiceFixture` on fixture sessions, loaded and as stubs: the default's label with nothing written and its token cached, a token stored and written verbatim, a token outside the list, a label and an empty string refused, "Default" removing the stored value, and the bulk edit setting, removing and refusing (fusion-plots items 806-827, 831, 835, 843, 844, 849, 850, 856-858). Nothing is held to a golden bit for bit (the one golden it reads, `coarse_maneuver`'s, is compared within 1e-6 degrees), so no `_exact` run. Label `fusion` |
| `tst_fusion_jobs` | The real fit through the executor on a real `SessionModel`, on the executor's 64 MiB worker: one job publishes all outputs together and announces them through the session model (acceptance 6); the executor gives the bits a synchronous request gives (7); the solver's oneTBB helper threads run at the worker's below-normal priority while they help a fit (`solverThreadsRunAtWorkerPriority`); an input edit during the fit asks the fit to stop at once, ends the job Superseded, publishes nothing, and leaves it requestable (8); a rejected recording is a Succeeded job carrying the reason, with nothing to do on re-request and a fresh run after an input change (9); cancel during the fit publishes nothing and the next job starts afterwards (10); a session without IMU data cannot have a job (11); the logbook column over `Fusion/roll`, the column worker and the saver never start a fit (5), and that column is cached as unavailable before the fit, from the published result after it (with the `"records"` stamp in `index.json`), shown by the unloaded row after a restart without a load, and dropped with the record by an input change (`columnOnFusionOutputIsCachedFromRecord`); an altitude marker added at run time and registered again after a restart keeps that column's and the description's cached values of an unloaded session - no load, nothing pending (`altitudeMarkerKeepsColumnsOfUnloadedSession`); once that value of an unloaded session is gone from `index.json` (cleared, or dropped by an exit-marker move made while the application was closed: `_EXIT_TIME` is not bulk-editable, so the test repeats the `LogbookManager` calls of the bulk edit's stub path - temporary load, column marked unsaved, save), the column worker refills it after a restart from the stored fit restored into its temporary copy: the roll at the marker's time, bit-identical to the loaded session's, with no load of the row, no job, no fit and the record's bytes unchanged (`workerRefillsColumnFromStoredFit`); while the loaded row's cell shows the golden's number the moment the job publishes (`dataChanged` for that row only, and the number already there when the view is told); shutdown during a fit. Mid-run actions are taken in a slot on the job's first progress text, which the executor delivers before the job's end: no gate, no sleeps. `realRecordingCheck` is the optional local check of section 11 and skips unless `FLYSIGHT_FUSION_RECORDING` is set. Label `fusion` |
| `tst_fusion_runner` | `fusion_runner`, the command-line fit, driven as a child process on fixtures written out as `TRACK.CSV` / `SENSOR.CSV`: its diagnostics equal a direct `Fusion::run()` on the fixture and equal the application's own import-and-fit path (`SessionImport` on a `SessionModel`); the CSV output reloads bit for bit; `--dump-inputs` shows the effective inputs, including the legacy gyro scale of a file without `SCHEMA_VER`; a rejection exits 1 with the failure JSON and writes no CSV; usage and import failures exit 64 and 3; no calculation on the fit's input path declares a preference (the premise of the model-free import); and the seventeen output channels of `fitOutputChannels()` are the golden's columns in order. Label `fusion` |
| `tst_fusion_golden_exact`, `tst_fusion_kernel_exact`, `tst_fusion_session_exact`, `tst_fusion_jobs_exact`, `tst_fusion_rows_exact`, `tst_fusion_store_exact`, `tst_fusion_runner_exact` | Not executables: the seven tests above that compare with the goldens, run a second time with `FLYSIGHT_FUSION_EXACT=1` and otherwise the same environment, so that every golden comparison is bit equality (section 11, "Tolerance policy"). Registered only where that is a fair demand, the compiler the goldens were captured with (`FLYSIGHT_FUSION_EXACT_TESTS`, section 3). The first two decide bit-identity; the next four show that the bits survive the engine, the executor's worker thread, the demand layer's plot demand and a stored and restored record; the last is bit identity across the process boundary (the runner's output against an in-process run). Labels `fusion` and `exact` |
| `tst_fusion_rows` | The plot-row script with the **real** fusion plots: `PlotModel` + `CalculationDemand` + the executor + `SessionModel` + the fusion registration, with real fits on the executor's 64 MiB worker and the eight plots of `fusionPlots()` (`tests/fusion/fusionsessions.h`, which mirrors `MainWindow::registerBuiltInPlots()`; `audit_cleanup` pins the application's list at eight rows). All eight plots are explicit-backed (the fit their one requested calculation) and every one is drawn after the one fit, on the fit's time axis (`allEightFusionPlotsAreExplicitBacked`); the row script of acceptance 15 on three real tracks (checking fits them one after another with no other action, the sessions still to compute falling as each publishes; unchecking mid-way drops the waiting one and lets the running one finish; checking again resumes; a fourth track shown is fitted with no other action), with every published track held to the kernel's goldens and the job history as a literal; Heading, Pitch and Roll share one job and one progress text (`headingPitchRollShareOneJob`); `accH` is blocked by the fit and never has a job of its own; a session without IMU data is never counted and never listed among failures before, during and after a fit (11); a rejected recording is listed among failures with the reason, offers no retry, and is fitted again after its input changes and settles (9); sessions are edited, tracks hidden and shown and other values read while a real fit runs, without disturbing it, and are fitted afterwards with no other action (19, the half that needs no widget); the demand layer's items on demand, failures and chains unchanged in what they assert with real fits (status-bar item 753); the measurements of the removed plots still serve a logbook column kept from before (the fit's roll and the local frame's north at the exit marker, labelled with the measurement's name, `Fusion/roll @ ...`, beside a column over the Roll row labelled `Roll @ ...`) and the fit's stored record carries every fit channel that has no plot (`removedPlotMeasurementsStayAvailable`) (fusion-plots items 801-804, 806, 808, 842, 853, 854, 856). Steered by the first progress text of a job and by `jobFinished`: no gate, no sleeps. Label `fusion` |
| `tst_fusion_store` | The fit's stored result: bit-identical after unload and restart (also when fitted before the first save), rejection / solver failure listed with its reason, dependency (a declared input, `SCHEMA_VER`) and code-stamp invalidation, merges, session file untouched, not requested after the `cache/` folder was deleted while closed (one fit offered while the Roll plot is checked, dropped when unchecked, nothing run); kept across altitude-marker, registration, descent-pause and plugin-set changes, in memory and after a restart; dropped at once, with its record, by a registry change that changes what a name it looked up resolves to (the removal of its provider), kept by a candidate registered behind the provider; deleted when a lookup resolves differently at load; a provider's result version in the record; a logbook column over roll filled for sessions that are not loaded, and nothing fitted again after a restart (`columnOverFusionFillsUnloadedSessions`, `fusionColumnWithStoredFitsRunsNothing`); also run as `_exact` |

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

`fusion_golden_capture` is the second: it runs the product kernel on the twelve
synthetic fixtures and writes the goldens of `tests/data/fusion/` (section 11,
"The capture tool" and "Re-capture procedure"). `fusion_runner` is the third:
the fit on one recording imported as the application imports it, with the
diagnostics on standard output, for the reference recordings and the corpus
comparison (section 12.2).

**Audit**

| Test | Covers |
|------|--------|
| `audit_cleanup` | No old mechanism remains, each fact has one authority, none of the mechanisms of `sensor-fusion-clean-port` that have no successor exists, the structural rules of background work hold (one worker, no locks, GTSAM confined, work started only by the demand layer through the executor, views that only read the demand layer, a model and scheduler that know no jobs, a widget-free core), stored results live in the logbook's `cache/` folder, never in the session file, and are named, written, read, restored and deleted in one place each, and a stored result goes stale only when its in-memory twin would be dropped or its code changed (no environment fingerprint in a record, the plug-in code identity computed in one place, teardown removals at shutdown only), and no refresh, cancel, queue or plot-request logic remains in code or documents, background work is shown in one place, the status bar, and a failure once per recording, on its logbook row (the per-source presentation, its indicators, its clock and the logbook's progress line gone from code and documents), a default standing in for a value the user has not set is a calculation, a constant one registered by one helper, one type owns the mount vocabulary, the plot list has its eight fusion plots, and no local-frame or removed fusion plot remains in code or documents, and every line of `tests/acceptance_map.txt` resolves (section 10) |

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
| `FLYSIGHT_FUSION_EXACT_TESTS` | `AUTO` | Only with the fusion tests: register the bit-exact runs `tst_fusion_*_exact` (label `exact`; no new executable). `AUTO` registers them when the compiler is 64-bit MSVC of the same major.minor as `cl_version` in `tests/data/fusion/capture.json` (19.44; the file is written by `fusion_golden_capture` at every capture), and otherwise prints "Fusion exact tests not registered: ..." at configure time; `ON` registers them whatever the compiler is; `OFF` never does. In every mode they exist for the Release configuration only. Forwarded by the root `CMakeLists.txt` like the others (section 11, "Tolerance policy") |

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
(`build-solver-deps/GTSAM-install/bin`, `build-solver-deps/oneTBB-install/bin`;
section 11's re-capture procedure and section 12.2 spell the full `PATH`).

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

Nine specifications, nine ranges of items in `tests/acceptance_map.txt`, the
machine-checked form of the nine tables below (section 10); keep them in sync.

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
attribute" (9.9).

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
| 11 | (as amended) no-IMU session: never counted in progress nor listed among the failures; no job possible | `tst_jobqueue::refusesMissingInput`; `tst_calculation_demand::sessionWithoutInputIsNeverListed`; `tst_fusion_session::missingInputsAreNotApplicable`; `tst_fusion_jobs::noImuSessionCannotHaveAJob`; `tst_fusion_rows::noImuSessionIsNeverCounted` (all eight real rows) |
| 12 | blockers report fusion for `accH`, nothing after publication, never a fit | `tst_calcengine_blockers::derivedNameReportsExplicitBlocker`, `inspectionNeverRunsExplicit`; `tst_fusion_session::blockersReportFusion` |
| 13 | (as amended) B consumes A: checking the plot runs A then B | `tst_calcengine_blockers::chainedBlockers`; `tst_calculation_demand::chainedBlockersContinue` |
| 14 | (as amended) one job at a time; at most the running and one chosen next job; no duplicates | `tst_jobqueue::oneAtATimeInOfferOrder`, `duplicateOffersCreateNoDuplicates`, `holdsAtMostRunningAndChosenNext` |
| 15 | (as amended) row script, without widgets, synthetic: checking computes the visible tracks one after another, the sessions still to compute falling; unchecking drops the waiting ones, a track shown is computed | `tst_calculation_demand::rowScript`, `showingASessionStartsIt`, `uncheckingDropsWaitingPairsKeepsRunning` |
| 15 | (as amended) row script with the real fusion plots (the eight); heading / pitch / roll share one job; `accH` blocked by fusion | `tst_fusion_rows::realRowScript`, `headingPitchRollShareOneJob`, `accHRowIsBlockedByFusion`, `rejectedTrackShowsBadge`, `allEightFusionPlotsAreExplicitBacked` |
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
(9.8).

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
| 215 | 5 formula | `sigma_w = slope x dt x norm(delta omega)`, `sigma_a = slope x dt x norm(delta f)`, change of the interpolated signal across the step, covariance `(density^2 + sigma^2 x dt) I`; zero change gives the density covariance exactly | `tst_fusion_kernel::perStepTermMatchesSpecifiedCovariance`, `perStepTermIsZeroWithoutSignalChange`; `tst_fusion_golden::successFixturesMatchGolden` (`coarse_linear`'s numbers are the density-only model) |
| 216 | 5 constants | slopes 0.026 (gyro) and 0.40 (accelerometer); densities, step boundaries and midpoint sampling unchanged | `tst_fusion_kernel::diagnosticsReportPerStepConstants` (`Tuning{}` values and `model.per_step`), `preintegrationHonoursExactBoundaries` |
| 217 | 5 dt scaling | the dt factor keeps the constants valid at higher rates | `tst_fusion_kernel::perStepTermScalesWithStep` (a wrong-dt expectation is rejected) |
| 218 | 5 constraint | the per-step covariance is applied by setting the shared parameters before each `integrateMeasurement` call | `audit fusion-model` (exactly one `.integrateMeasurement(` call in `imuintegration.cpp`); `tst_fusion_kernel::perStepTermMatchesSpecifiedCovariance` (the effect) |
| 219 | 6 model | `b(t) = b0 + b1 (T(t) - T_ref)`, `T_ref` the mean IMU temperature over the fitted window, the accelerometer bias constant; each interval evaluated at its own bias in the fit and in the reconstruction | `tst_fusion_kernel::temperatureGraphShape` (`tRef` the index-order mean; the factor's `temperatureDelta`), `temperatureFactorJacobians` (six Jacobians; equal to `ImuFactor` at zero slope), `reconstructionUsesIntervalBias`, `driftingBiasSegmentsConverge` (`b1` within 20 %, `t_ref_degc` 35, `b0` at `T_ref`; `t_ref_degc` and `b1_rad_s_per_degc` are always numbers) |
| 220 | 6 priors | `b0` under today's prior (0.03 rad/s); `b1` zero-mean with sigma 0.010 deg/s per degC | `tst_fusion_kernel::temperatureGraphShape` (the slope prior's sigmas equal `Tuning{}.gyroBiasSlopeSigma`), `validationRejectsEachDefect` (a non-positive `gyroBiasSlopeSigma` is refused), `driftingBiasSegmentsConverge` (`slope_prior` last, after `bias_prior`) |
| 221 | 6 input channel | `IMU/temperature` is the twenty-second input, required, one value per IMU sample; a recording handed no temperature is rejected by the kernel with a reason naming `IMU/temperature`, and a session without the column is blocked like any missing input | `tst_fusion_kernel::validationRejectsEachDefect` (absent, wrong length, non-finite: the reason names `IMU/temperature`, and after every other channel's defect); `tst_fusion_session::registrationShape` (22 inputs, all required), `missingInputsAreNotApplicable` (the `no-temperature` row), `temperatureReachesTheKernel`, `inputsAreBitIdenticalToFixture` (there is no temperature rejection golden: the twelve fixtures all carry the channel) |
| 222 | 6 constant temperature | a recording whose temperature does not change leaves `b1` at its prior | `tst_fusion_kernel::constantTemperatureKeepsSlopeAtPrior` (every component of `b1` below 1 % of the prior sigma in magnitude; the slope prior's residual zero; the objective equals the constant-bias fit's) |
| 223 | 6 initializer unaffected | segment and prefix fits use a constant bias; the full fit starts `b1` at zero | `tst_fusion_kernel::driftingBiasSegmentsConverge` (stock prefix fits: `prefix_fits` 4, `prefix_length_s` 60), `temperatureGraphShape` (the four-argument builder is the stock graph); `tst_fusion_golden::successFixturesMatchGolden` (the goldens as Phase 6 re-captured them: the `initializer` objects unchanged by the temperature model) |
| 224 | 7 initializer | the diagnostics report the segments (start, end, prefix length, yaw sigma, iterations) and any fallback | `tst_fusion_kernel::initializerDiagnosticsShape` (the key sets), `allPrefixFitsFailFallsBack` (`fallback_segments`); `tst_fusion_golden::successFixturesMatchGolden` |
| 225 | 7 stopping | which rule ended the fit, the last pass's mean relative decrease, the re-preintegration cost difference | `tst_fusion_kernel::biasSettledByCostTest`, `slowTailAtTheIterationLimit`, `failureDiagnosticsShape` (`cost increased` with nulls), `nonConvergenceIsSolverFailure` (the failure key set) |
| 226 | 7 quality | normalized RMS of the IMU, position and velocity factors, and the objective per state | `tst_fusion_kernel::biasSettledByCostTest` (recomputed from the residual array), `exactConstantVelocityFit` |
| 227 | 7 model | the per-step constants and the fitted `b0`, `b1` with `T_ref` (always numbers: the temperature is required) | `tst_fusion_kernel::diagnosticsReportPerStepConstants`, `driftingBiasSegmentsConverge`, `constantTemperatureKeepsSlopeAtPrior` |
| 228 | 7 tooltip / account, as amended | the failure text (the status bar's and the logbook row's hover) keeps showing the reason; the diagnostics are an account, not an input | `tst_fusion_rows::rejectedTrackShowsBadge` (the reason in the failure entry); `tst_fusion_session::registrationShape` (`_FUSION_DIAGNOSTICS` is an output and no input) |
| 229 | 8 input | a folder or two paths; imported as the application imports (parser, conversion layer, on-demand derivation, the legacy gyro scale); the kernel gets the job queue's channels | `tst_fusion_runner::matchesTheApplicationImportPath`, `successMatchesDirectRun`, `legacySchemaScalesTheGyro`, `outputTableMatchesGolden` |
| 230 | 8 output | diagnostics JSON on stdout; `--csv` writes the seventeen channels; exit 0 for Succeeded, non-zero otherwise with the outcome and reason on stderr | `tst_fusion_runner::successMatchesDirectRun`, `rejectionExitsOne`, `usageAndImportFailures` |
| 231 | 8 no GUI, logbook, preferences | the runner reads and writes no user logbook or settings | `tst_fusion_runner::noPreferenceOnTheFitPath`; `audit fusion-tooling` (`fusion_runner.cpp` names no `LogbookManager`, `PreferencesManager`, `SessionModel`, `SessionImport`, `EnginePreferenceProvider`, `applyCreationDefaults`, `JobQueue`) |
| 232 | 8 progress | progress texts on stderr; cancellation not required | `tst_fusion_runner::successMatchesDirectRun` (stderr is the kernel's texts in order, then `Succeeded`) |
| 233 | 8 tooling target | built with the tests, never shipped | `audit fusion-tooling` (no install rule names `fusion_runner` or `fusion_golden_capture`); `manual M10` |
| 234 | 9 purity, threading, cancellation | a function of the channels, no session or GUI object, every solver iteration a boundary, cancellation observed at boundaries only | `tst_fusion_golden::resultIsIndependentOfCallerState`, `workerThreadMatchesMainThread`, `twoRunsAreBitIdentical`, `cancelAtEachKindOfBoundary`, `cancelDuringPreparation`, `cancelNeverRequestedChangesNothing`; `tst_fusion_session::cancelStopsAtNextBoundary`; `audit solver-confinement` (the kernel is pure, does not log); `audit fusion-model` (exactly three `checkpoint(` call sites, no silent poll) |
| 235 | 9 result contract | channels and outcomes unchanged; a slow-tail acceptance is `Succeeded` | `tst_fusion_kernel::slowTailAtTheIterationLimit` (`Succeeded`, seventeen channels filled, empty reason); `tst_fusion_session::registrationShape` (18 outputs); `tst_fusion_runner::outputTableMatchesGolden` (seventeen channels in order); the closing audit's `fusion.h` diff against `master` (comment lines and the one added field) |
| 236 | 9 GTSAM | nothing outside the fusion library links GTSAM | `audit solver-confinement`; `flysight_assert_solver_confinement()` at configure time (`GTSAM link confinement: OK (14 targets reach gtsam)`) |
| 237 | 10 goldens | the parity tests are retired; goldens are captured from the changed kernel with the same harness and the regression tests compare against them; fixtures are deterministic | `tst_fusion_golden::successFixturesMatchGolden`, `rejectionFixturesMatchGolden`, `progressMatchesGoldenBoundaries`, `channelsWriterIsTheInverseOfTheLoader`, `fixturesAreDeterministic`, `comparatorHoldsItsBounds`; `tst_fusion_kernel::fitTraceMatchesGolden`, `initializerFixturesAreDeterministic`; the closing audit's fresh capture equal to the goldens on disk (section 11) |
| 238 | 10 test 1 | starts in motion: prefixes grow until the sigma is below 20 degrees exactly at the first length containing the manoeuvre; the full fit within 2 degrees of the truth | `tst_fusion_kernel::startsInMotionGrowsToTheManoeuvre` |
| 239 | 10 test 2 | at rest throughout, longer than two doublings: growth stops at 120 s (`no_gain`); the full fit converges; roll and pitch within 0.5 degrees | `tst_fusion_kernel::atRestPrefixStopsGrowing` |
| 239 | 10 test 2b | prefix fits run one pass of at most 50 iterations; a prefix and a segment fit forced onto the iteration limit are still used as the start and the diagnostics say so | `tst_fusion_kernel::startsOnTheLimitAreStillUsed`; every initializer test (`prefix_passes` 1, `prefix_iterations` at most 50): `startsInMotionGrowsToTheManoeuvre`, `atRestPrefixStopsGrowing`, `smallestSaccFixIsTheAnchor`, `driftingBiasSegmentsConverge` |
| 240 | 10 test 3 | sAcc 2 m/s except 0.3 m/s at 200 s: that fix is the anchor, the first prefix is 170-230 s, the segment's start attitude is the prefix fit's carried back | `tst_fusion_kernel::smallestSaccFixIsTheAnchor` |
| 241 | 10 test 4 | longer than two segments with a 1 deg/s linear drift: every segment fit converges; with section 6 the full fit recovers `b1` within 20 % in at most 30 iterations | `tst_fusion_kernel::driftingBiasSegmentsConverge` |
| 242 | 10 test 5 | a segment whose prefix fits all fail falls back, the diagnostics say so, the full fit still runs | `tst_fusion_kernel::allPrefixFitsFailFallsBack` |
| 243 | 10 test 6 | bias-settled test: a fit whose cost stops changing converges within two passes | `tst_fusion_kernel::biasSettledByCostTest` |
| 244 | 10 test 7 | slow tail: a forced final pass at the limit is accepted when both conditions hold and fails when either fails | `tst_fusion_kernel::slowTailAtTheIterationLimit` |
| 245 | 10 test 8 | per-step term: zero change gives the density covariance exactly; a known change gives the specified covariance; the term scales with dt | `tst_fusion_kernel::perStepTermIsZeroWithoutSignalChange`, `perStepTermMatchesSpecifiedCovariance`, `perStepTermScalesWithStep` |
| 246 | 10 test 9 | section 6, as amended: a recording without `IMU/temperature` is rejected by the kernel (the reason names the channel) and blocked in a session like any missing input; constant temperature leaves `b1` at its prior | `tst_fusion_kernel::validationRejectsEachDefect`, `constantTemperatureKeepsSlopeAtPrior`; `tst_fusion_session::missingInputsAreNotApplicable` |
| 247 | 11 | `docs/` describes the segmented initializer, the stopping rule with its slow tail, the per-step term and its meaning at other output rates, and the temperature-dependent bias, in the place that documents the fusion model | `docs/SENSOR_FUSION.md` sections 4 and 5; `audit fusion-model` (the document describes no retired mechanism: no resting-window detector, candidate window, coarse-only initializer, frozen algorithm, bias-shift test, branch name, or input count from before the temperature channel) |

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
the specification "One status bar for background work" (9.8).

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
| 337 | 8, as amended | test: a fusion fixture fitted, saved, unloaded, reloaded: seventeen channels and diagnostics bit-identical to the goldens, no job, nothing counted in progress and nothing listed among the failures | `tst_fusion_store::restoredAfterEvictionIsBitIdentical` |
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
orientation attribute" (9.9).

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
| 509 | 5, as amended | plot demand: for every checked plot whose value is a requested output, every visible session needs the requested calculations that block that output; the eight plots of the "Sensor fusion" category are such plots: one is an output of the fit, and the seven derived from its outputs are blocked by it | `tst_calculation_demand::rowScript`, `ordinaryPlotsAreNeverInspected`, `hiddenAndStubRowsAreNotTracks`, `failedLoadPlaceholderIsNotATrack`, `plotIdMatchesPlotModelRole`, `uncheckedPlotsAreNeverInspected`; `tst_fusion_rows::allEightFusionPlotsAreExplicitBacked` |
| 510 | 5 | column demand: for every enabled logbook column whose value depends on a requested output, every session in the logbook needs the requested calculations that block that value | `tst_calculation_demand::enablingColumnFillsEveryUnloadedSession`, `loadedHiddenSessionsNeedNoLoad`, `ordinaryColumnsCreateNoDemand`, `columnIdIsTheDefinitionKey`; `tst_fusion_store::columnOverFusionFillsUnloadedSessions`; `manual M24` |
| 511 | 5 | a pair is in demand only while it has no result; a result counts whether published in this run or restored, success or input-determined failure | `tst_calculation_demand::onlyRequestableCalculationsAreOffered`, `storedResultsCreateNoJob`, `inputDeterminedFailureIsStoredBadgedNeverRerun`, `columnFailuresAreBadgedNotReloaded`; `tst_fusion_store::restoredRejectionShowsBadge`, `restoredAfterRestartIsBitIdentical` |
| 512 | 5 | a pair whose calculation cannot apply (a declared input missing) is never in demand and is not reported anywhere | `tst_calculation_demand::sessionWithoutInputIsNeverListed`, `notApplicableSessionIsSettledWithoutAJob`; `tst_jobqueue::refusesMissingInput`; `tst_fusion_rows::noImuSessionIsNeverCounted`; `tst_fusion_store::columnOverFusionFillsUnloadedSessions` |
| 513 | 5, as amended | whether a result exists: blocker inspection for a loaded session; for a session not loaded, first what this run remembers of its pairs (a failure; a not-applicable verdict of that column), then the logbook's record names and the reasons the index recorded for them, no record opened; a cell has a result only when every calculation it needs has a record; a record with a reason is a failed result before and after a restart alike; a reason the index learns later is announced as a record change; a known record counts until the column worker's restore deletes it as stale, which moves the pair into demand; the demand layer checks no staleness itself | `tst_calculation_demand::storedResultsCreateNoJob`, `chainedColumnWithUpstreamRecordIsCompleted`, `storedRejectionIsBadgedAfterRestartWithoutLoad`, `passOverManyStubsReadsEachRecordSetOnce`, `columnProgressAndPendingCells`, `recordReasonReachesDemandThroughRecordChange`, `failedRecordWriteIsShownAndNotRetried`; `tst_logbook_index::recordReasonsRoundTrip`, `recordReasonChangeIsAnnounced`; `tst_result_store::recordReasonRecordedAtWriteAndRestore`; `tst_result_columns::staleRecordDeletedByWorkerCreatesDemand`; `audit demand` |
| 514 | 5 | demand does not depend on how the state arose (a gesture, a profile, the start-up restore); at start-up every session is hidden, so plots create no demand until sessions are shown, while enabled columns do | `tst_calculation_demand::programmaticCheckCreatesDemand`, `profileStyleApplyCreatesDemand`, `startupRestoreWithHiddenSessionsStartsNothing`, `profileStyleColumnsCreateDemand`, `startupWithEnabledColumnLoadsAfterColumnWorker`; `manual M1`, `M2` |
| 515 | 5 | nothing about demand is persisted: it is derived again at the next start; a profile carrying a column over a requested output computes it for every session without a result, so no default profile carries such a column | `tst_calculation_demand::jobLevelFailureIsBadgedNotRerunUntilRestart`, `profileStyleColumnsCreateDemand`; `audit demand`; `manual M2` |
| 516 | 5 | when a requested calculation depends on another, the demand covers both, upstream first | `tst_calculation_demand::chainedBlockersContinue`, `heldChainContinues`, `chainCompletesAfterFirstJobDoesNotSucceed`, `chainedColumnKeepsItsHold`; `tst_calcengine_blockers::chainedBlockers` |
| 517 | 6, as amended | a pair that enters demand is wanted at once, with no gesture (checking, showing, enabling, an input change that drops a demanded result), and progress counts its session from the first change | `tst_calculation_demand::showingASessionStartsIt`, `loadingAVisibleSessionStartsIt`, `mergeCreatesDemandForShownSessions`, `plotCheckedDuringAJobJoinsIt`, `enablingColumnFillsEveryUnloadedSession`, `inputBurstRunsOneJob`, `programmaticCheckCreatesDemand` |
| 518 | 6 | a pair that leaves demand (plot unchecked, session hidden, column disabled, result appeared by other means) is dropped before it starts; there is no queue of accepted requests to prune | `tst_calculation_demand::hidingASessionDropsItsWaitingPair`, `uncheckingDropsWaitingPairsKeepsRunning`, `waitingPairNeededByAnotherPlotSurvives`, `chainStopsForHiddenTrackOrUncheckedPlot`, `disablingColumnReleasesHeldSessions`, `onlyRequestableCalculationsAreOffered`, `resultAppearingWhileWaitingDropsThePair`; `tst_jobqueue::withdrawEndsChosenNext`; `audit demand`; `manual M6`, `M26` |
| 519 | 6 | the running job is never stopped because its pair left demand: it finishes and its result is published and stored; it is stopped only when its inputs change, its session goes away, or the application closes | `tst_calculation_demand::uncheckingDropsWaitingPairsKeepsRunning`, `hidingASessionDropsItsWaitingPair`, `disablingColumnReleasesHeldSessions`, `removedSessionLeavesNoTrace`; `tst_jobqueue::staleRunningJobIsStoppedAtOnce`, `removeSessionWithRunningJob`, `shutdownWithQueuedAndRunning`; `tst_fusion_rows::realRowScript`; `audit gestures`; `manual M6` |
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
| 629 | 9 | the executor keeps its cancel operation for the jobs dock; no product code calls it, and its comment and the audit say so | `tst_jobqueue::cancelRunningThenNextStarts`, `cancelQueued`; `audit gestures` |
| 630 | 9 | the executor's refusal kinds are unchanged; the demand layer offers upstream first, so a Blocked refusal is not expected from it, and the documentation says so instead of the code guarding for it | `tst_jobqueue::refusesBlockedAndDone`, `refusesMissingInput`; `tst_calculation_demand::chainedBlockersContinue`, `columnOfferRefusalIsNotLeftPending` |
| 631 | 10.1, as amended | one walk over the session rows derives progress, failures, pending cells and candidates, for plots and columns at once, under one row stability guard, returning plain values; offers, withdrawals, holds, loads and signals happen after it | `tst_calculation_demand::changeSignalsAreMinimal`, `columnProgressAndPendingCells`, `passOverManyStubsReadsEachRecordSetOnce`; `audit demand` |
| 632 | 10.1 | a loaded session is classified from the engine's blocker inspection of the source's names, combined alike for a plot's one name and a column's names, the running job, the pair memory and the settle wait; the combined report is memoized per session and source and dropped by an input change or publication, a load, a record change, a job's end, a reset or a registry change | `tst_calculation_demand::hiddenAndStubRowsAreNotTracks`, `ordinaryPlotsAreNeverInspected`, `uncheckedPlotsAreNeverInspected`, `progressTextWithoutAPass`, `chainedBlockersContinue`, `registryChangeReclassifies` |
| 633 | 10.1 | a session that is not loaded is a track of columns only, classified in this order: no storable calculation, not applicable; a storable calculation remembered failed (a job, a load or a write that failed), failed with the remembered reason; every storable calculation remembered not applicable for that column, not applicable; every storable calculation with a record, done, or failed with the first recorded reason; a failed-load placeholder, failed ("The session file could not be loaded"); otherwise waiting | `tst_calculation_demand::storedRejectionIsBadgedAfterRestartWithoutLoad`, `unloadableSessionIsSettledAsFailed`, `failedRecordWriteIsShownAndNotRetried`, `columnVerdictDoesNotSuppressAnotherColumn`, `passOverManyStubsReadsEachRecordSetOnce`; `tst_logbook_indicators::failedLoadSessionShowsRowWarningNotPending` |
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
| 662 | 15, as amended | docs/ and tests/README.md describe the executor's signals and queries (cancel kept for the jobs dock), the one walk, the one memory, the parts of the demand layer, the fill's completion, the failed write, the session model as the source of column knowledge and display names, and the status bar and the row warning; not the indicator, the clock or the fill's progress text | `audit demand` |

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
both categories; heading takes the course's reference and its availability,
so a recording without a course reference has neither (a stored reference
that is not a number or lies outside the GNSS time gives no offset); the
attribute's key is `_ORIENTATION`, in category "Session", registered once per
process; an invalid stored token is kept and makes the attitude unavailable;
the SP and WS-P defaults move onto the constant-default helper; the legacy
backfill no longer writes wind; the attribute registry moves to the model
library and the helpers the fusion library calls are header-only. The
specification amends those of 9.1 (item 6), 9.2 (item 115) and 9.6 (item
509).

| # | Section | Clause | Evidence |
|---|---|---|---|
| 801 | 5 | the "Sensor fusion" category holds exactly eight plots, in this order: Elevation, Horizontal acceleration, Vertical acceleration, Along-track acceleration, Cross-track acceleration, Heading, Pitch, Roll | `tst_fusion_rows::allEightFusionPlotsAreExplicitBacked`; `audit naming`; `manual M39` |
| 802 | 5, as settled | each is named, united and typed as its GNSS counterpart (Elevation, the four accelerations, Heading as Course), Pitch and Roll as angles, so that the two overlay; a fused plot's colour differs from its counterpart's | `tst_fusion_rows::allEightFusionPlotsAreExplicitBacked`; `manual M39` |
| 803 | 5 | the fused north, east and down position and velocity, north and east acceleration, roll, pitch, yaw and quaternion plots are removed, and the "GNSS (Local frame)" category with them | `tst_fusion_rows::allEightFusionPlotsAreExplicitBacked`; `audit naming`; `manual M39` |
| 804 | 5 | every measurement behind a removed plot still exists and is computed as before: the local frame feeds the fit and a column, and the fit's channels are its record, read by a column kept from before, the stored result and plug-ins | `tst_fusion_rows::removedPlotMeasurementsStayAvailable`; `tst_fusion_jobs::columnOnFusionOutputIsCachedFromRecord`; `manual M44` |
| 805 | 5 | applying a profile enables the listed plots the application has and ignores the rest silently (no message, no log, no failure); nothing rewrites a profile | `tst_calculation_demand::profileNamingRemovedPlotsAppliesWithoutThem`; `manual M43` |
| 806 | 6 | each derived quantity is an on-demand calculation over the fit's outputs, registered with the fit: blocked by it until it publishes, it appears with it and never starts one | `tst_fusion_derived::derivedRegistrationShape`, `derivedValuesWaitOnTheFit`, `attitudeWaitsOnTheFit`; `tst_fusion_rows::accHRowIsBlockedByFusion`, `allEightFusionPlotsAreExplicitBacked` |
| 807 | 6, as settled | elevation is the local origin's height above mean sea level minus the fused down position, above the ground elevation as the GNSS elevation is, so the two overlay; it is unavailable when either attribute is not a number | `tst_fusion_derived::elevationIsOriginHeightMinusDownAboveGround` |
| 808 | 6, as settled | vertical acceleration is the fit's own fused down acceleration, positive down like the GNSS one; no second calculation publishes it | `tst_fusion_rows::allEightFusionPlotsAreExplicitBacked`; `tst_fusion_derived::derivedRegistrationShape`; `tst_fusion_session::explicitOutputsHaveOneCandidate` |
| 809 | 6 | along-track and cross-track acceleration are the GNSS definitions applied to the fused velocity and acceleration, wind-corrected, so their signs and the meaning of "track" agree | `tst_fusion_derived::trackAccelerationsKnownAnswers`, `trackAccelerationsAreTheGnssDefinitions` |
| 810 | 6, as settled | one definition of the track-relative accelerations and of the wind rule serves both categories, and the GNSS values are unchanged | `tst_fusion_derived::trackAccelerationsAreTheGnssDefinitions`; `tst_builtins_golden::sessionDataMatchesGolden`; `tst_builtins_engine::goldenOnEngine` |
| 811 | 7 | the orientation's forward and up axes fix the body frame, right being their cross product; the body-to-device rotation is a constant signed axis permutation, a proper rotation for each of the 24 pairs, never a reflection | `tst_fusion_derived::orientationRotationIsProper` |
| 812 | 7 | heading, pitch and roll are the aircraft Euler angles of the fit's quaternion composed with that rotation: heading the forward axis's direction clockwise from north, pitch its elevation above the horizontal, roll the rotation about it, positive right side down | `tst_fusion_derived::levelNorthFacingBodyReadsZero`, `knownAnglesComeBack`, `sideMountPermutesTheAngles`, `deviceFrameMountGivesTheFitsOwnAngles`; `manual M40` |
| 813 | 7 | they are computed when read, from the quaternion channels and the orientation attribute, never by the fit | `tst_fusion_derived::attitudeRegistrationShape`, `attitudeWaitsOnTheFit`, `storedOrientationRecomputesWithoutAFit`; `tst_fusion_session::registrationShape` |
| 814 | 7, as settled | heading is unwrapped by the application's one unwrap rule, the course's, and less the course reference angle the GNSS course subtracts, computed in one place for both (zero for a stored reference that is not a number or lies outside the GNSS time), so heading and course overlay in straight flight and heading minus course reads as sideslip; heading has the course's inputs and availability, so a recording without a course reference has neither | `tst_fusion_derived::headingUnwrapsThroughAFullTurn`, `attitudeRegistrationShape`; `manual M40` |
| 815 | 7 | pitch and roll are reported in their natural ranges, pitch from -90 to 90 degrees and roll from -180 to 180 | `tst_fusion_derived::rollAndPitchStayInTheirNaturalRanges` |
| 816 | 7 | where the forward axis is vertical, heading and roll are not defined and the derivation reports what the standard formulas give | `tst_fusion_derived::forwardAxisVerticalGivesTheStandardFormulas` |
| 817 | 7 | a change of the orientation attribute recomputes the angles through ordinary invalidation and never refits | `tst_fusion_derived::storedOrientationRecomputesWithoutAFit`; `manual M41` |
| 818 | 8 | the orientation is a token naming the forward and the up axis in a fixed form the fusion module owns (the default `+y,+z`); the 24 valid pairs, and only they, are its choices, labelled "forward +y, up +z" and so on, the default first | `tst_fusion_derived::orientationVocabularyHasTwentyFourPairs`, `orientationDefinitionIsTheEnumeration` |
| 819 | 8 | its default, forward +y and up +z, is a constant calculated attribute: every recording has it, imported before or after, and nothing is written into any session file | `tst_fusion_derived::orientationColumnShowsTheDefaultWithoutAWrite`, `storedOrientationRecomputesWithoutAFit`, `attitudeRegistrationShape`; `manual M41` |
| 820 | 8 | a stored value wins over the default, and removing it returns to the default | `tst_fusion_derived::storedOrientationRecomputesWithoutAFit`, `orientationDefaultRemovesTheStoredValue` |
| 821 | 8 | one orientation type in the fusion module owns the vocabulary (the axis pair, token, label, body-frame rotation, default and enumeration); the derivation parses with it, the attribute's choices come from it, and nothing else spells a token | `tst_fusion_derived::orientationDefinitionIsTheEnumeration`, `orientationVocabularyHasTwentyFourPairs`; `audit orientation` |
| 822 | 8, as settled | the fusion registration registers the attribute's definition once per process (key `_ORIENTATION`, "Orientation", category "Session", a choice attribute, editable) with the derivation that reads it; the key is part of the session file's vocabulary | `tst_fusion_derived::orientationDefinitionIsTheEnumeration`, `attitudeRegistrationShape`, `orientationEditStoresATokenAndRefusesOthers`; `manual M41` |
| 823 | 8 | it is edited as any editable attribute: a logbook column the user can add, not among the default columns, edited in place, and set for the selected sessions from the context menu | `tst_fusion_derived::orientationEditStoresATokenAndRefusesOthers`, `orientationBulkEdit`; `tst_choice_attribute::cellEditorOffersTheList`, `setDialogOffersTheList`; `manual M41`, `M42` |
| 824 | 8 | both editors offer the list, not free text, and a "Default" entry that removes the stored value rather than storing anything | `tst_choice_attribute::cellEditorOffersTheList`, `setDialogOffersTheList`; `tst_fusion_derived::orientationDefaultRemovesTheStoredValue`, `orientationBulkEdit`; `manual M41`, `M42` |
| 825 | 8 | a value outside the list is refused by the model, the in-place editor and the bulk edit | `tst_fusion_derived::orientationEditStoresATokenAndRefusesOthers`, `orientationBulkEdit`; `tst_choice_attribute::cellEditorOffersTheList` |
| 826 | 8, as settled | a stored token outside the list (a hand-edited file) is kept and shown as written, and heading, pitch and roll are unavailable for it until it is changed or removed | `tst_fusion_derived::invalidStoredOrientationMakesAttitudeUnavailable`; `tst_choice_attribute::choiceShowsTheLabelOfTheEffectiveToken`; `manual M42` |
| 827 | 9 | the attribute registry's format types gain a fifth, choice, whose definition carries its allowed values, each a stored token and a display label | `tst_choice_attribute::choiceShowsTheLabelOfTheEffectiveToken`, `choiceEditStoresAToken`; `tst_fusion_derived::orientationDefinitionIsTheEnumeration` |
| 828 | 9 | a choice is displayed and sorted by its label | `tst_choice_attribute::choiceShowsTheLabelOfTheEffectiveToken`, `choiceSortsByLabel` |
| 829 | 9 | in-place editing of a choice gives a list editor in place of the line edit | `tst_choice_attribute::cellEditorOffersTheList`; `manual M41` |
| 830 | 9 | the context menu's "Set ..." action offers a list in place of the text prompt | `tst_choice_attribute::setDialogOffersTheList`; `manual M42` |
| 831 | 9 | the bulk edit sets the chosen token for the selected sessions, loaded or not | `tst_choice_attribute::bulkEditSetsAToken`; `tst_fusion_derived::orientationBulkEdit`; `manual M42` |
| 832 | 9 | "Default" removes the stored attribute on every edit path, and a token outside the list is refused | `tst_choice_attribute::choiceDefaultRemovesTheStoredValue`, `bulkEditDefaultRemovesTheStoredValue`, `choiceEditRefusesATokenOutsideTheList`, `bulkEditRefusesATokenOutsideTheList` |
| 833 | 9 | nothing about the type is specific to orientation: any choice definition gets all of it | `tst_choice_attribute::choiceEditStoresAToken`, `cellEditorOffersTheList` |
| 834 | 10 | a constant default is a registered calculation with no inputs whose one output is the attribute: it runs once and is cached, a stored value wins, setting or removing a stored value invalidates what read the attribute, and a dependent follows every change | `tst_calcengine::constantCalculationIsADefault`; `tst_builtins_engine::constantDefaults` |
| 835 | 10 | one helper registers a constant default for an attribute key and a value, beside the exit-time defaults, so that every constant default is found by one search; the orientation default uses it from the fusion registration | `tst_builtins_engine::constantDefaults`; `tst_fusion_derived::attitudeRegistrationShape`; `audit constant-defaults` |
| 836 | 10, as settled | the seven constant defaults of the SP and WS-P calculations are registered through the same helper, with their values | `tst_builtins_engine::constantDefaults`, `inventory`; `audit constant-defaults` |
| 837 | 10 | the importer stores only what is a fact of the import; anything that stands in for a value the user has not set is a calculation, derived where possible, constant otherwise | `tst_importer::creationDefaultsOnlyFillAbsent`; `tst_smoke::importAppliesCreationDefaults`; `audit constant-defaults` |
| 838 | 10 | wind north and east are constants of zero: a recording without stored wind reads zero, one with stored wind keeps it, and the importer no longer writes them | `tst_builtins_engine::constantDefaults`; `tst_smoke::importAppliesCreationDefaults`; `tst_importer::creationDefaultsOnlyFillAbsent`; `tst_import_merge::newSessionGetsDefaults`; `audit constant-defaults` |
| 839 | 10, as settled | the logbook's legacy backfill no longer writes wind either; it adds jumper mass and planform area only | `tst_persistence_roundtrip::releasedLogbookBackfillIsAdditive`; `tst_logbook_index::rawLoadSkipsBackfill`; `audit constant-defaults` |
| 840 | 10 | jumper mass, planform area and the fixed ground elevation stay creation defaults of the importer (section 3) | `tst_importer::creationDefaultsOnlyFillAbsent`, `fixedGroundElevationOnlyInFixedMode`; `tst_smoke::importAppliesCreationDefaults` |
| 841 | 10 | a stored attribute wins even when invalid or empty, so returning to a default removes the stored attribute and never stores a blank | `tst_calcengine::storedInvalidValueStillWins`, `removingStoredFallsBackToCalc`; `tst_choice_attribute::choiceDefaultRemovesTheStoredValue`, `bulkEditDefaultRemovesTheStoredValue` |
| 842 | 11 | the plot list shows "Sensor fusion" with eight plots named as the GNSS plots and no local-frame category; checking a fusion plot starts the fit as before, the status bar shows it, and a stored fit draws at once | `tst_fusion_rows::allEightFusionPlotsAreExplicitBacked`, `realRowScript`; `tst_fusion_store::restoredAfterRestartIsBitIdentical`; `manual M39` |
| 843 | 11 | the attitude plots are in the aircraft convention for the mount the orientation describes, heading continuous through turns | `tst_fusion_derived::sideMountPermutesTheAngles`, `headingUnwrapsThroughAFullTurn`; `manual M40` |
| 844 | 11 | the orientation column, once added, shows "forward +y, up +z" for every recording not set and the chosen label for one that is; editing offers the list and "Default"; changing it redraws the attitude plots without a fit | `tst_fusion_derived::orientationColumnShowsTheDefaultWithoutAWrite`, `orientationEditStoresATokenAndRefusesOthers`, `storedOrientationRecomputesWithoutAFit`; `tst_choice_attribute::cellEditorOffersTheList`; `manual M41` |
| 845 | 11 | profiles that name removed plots apply without complaint | `tst_calculation_demand::profileNamingRemovedPlotsAppliesWithoutThem`; `manual M43` |
| 846 | 11 | wind reads zero where nothing was stored, as before | `tst_builtins_engine::constantDefaults`; `tst_smoke::importAppliesCreationDefaults` |
| 847 | 12 | the fusion kernel and the fit calculation are unchanged (inputs, outputs, diagnostics, algorithm string), and the golden fixtures and stored results stay valid | `tst_fusion_session::registrationShape`; `tst_fusion_golden::successFixturesMatchGolden`, `rejectionFixturesMatchGolden`; `tst_fusion_kernel::fitTraceMatchesGolden`; `tst_fusion_store::restoredAfterRestartIsBitIdentical` |
| 848 | 12 | GTSAM stays confined to the fusion kernel, the new fusion files include neither GTSAM nor Eigen, and the rules on the fusion tooling hold | `audit solver-confinement`, `audit fusion-tooling` |
| 849 | 12 | the fusion registration gains the derived kinematics, the attitude derivation, the orientation type and the orientation attribute with its constant default; each derivation is an ordinary on-demand calculation with declared inputs, the attitude's including the orientation attribute | `tst_fusion_session::registrationShape`; `tst_fusion_derived::derivedRegistrationShape`, `attitudeRegistrationShape` |
| 850 | 12, as settled | the fusion library reaches nothing of the core library: the attribute registry lives in the model library, the constant-default and track helpers are header-only, and the calculations never name the fusion library | `tst_fusion_derived::orientationDefinitionIsTheEnumeration`; `audit solver-confinement` |
| 851 | 12 | the attribute calculations hold the constant-default helper and the wind defaults; the importer holds neither | `tst_builtins_engine::constantDefaults`; `audit constant-defaults` |
| 852 | 12 | the plot registry loses the removed plots and the local-frame category; the profile rule is the plot model's, one function the profile bridge calls | `tst_calculation_demand::profileNamingRemovedPlotsAppliesWithoutThem`, `profileStyleApplyCreatesDemand`; `audit naming` |
| 853 | 12 | the demand layer is untouched: a derived plot's blockers lead to the fit through the chain it already follows, and heading, pitch and roll are filled by one job | `tst_fusion_rows::accHRowIsBlockedByFusion`, `headingPitchRollShareOneJob`, `allEightFusionPlotsAreExplicitBacked`; `audit demand` |
| 854 | 13 | test: the plot list has the eight fusion plots and no local-frame plots; every fusion plot is explicit-backed (waits on the fit) as the seventeen were; an existing column over a removed plot's measurement still computes, and the export of the fit, its stored record, still holds the measurement | `tst_fusion_rows::allEightFusionPlotsAreExplicitBacked`, `removedPlotMeasurementsStayAvailable`, `noImuSessionIsNeverCounted`; `tst_fusion_session::missingInputsAreNotApplicable`; `tst_fusion_jobs::noImuSessionCannotHaveAJob` |
| 855 | 13 | test: applying a profile that names a removed plot enables its other plots and ignores that one, silently | `tst_calculation_demand::profileNamingRemovedPlotsAppliesWithoutThem` |
| 856 | 13 | test: elevation equals the origin height minus down minus the ground elevation, and is unavailable without a ground elevation; vertical acceleration is the down acceleration; along-track and cross-track equal the GNSS definitions applied to the fused velocity and acceleration, on synthetic data with known answers | `tst_fusion_derived::syntheticOutputsAreServedWithoutAFit`, `elevationIsOriginHeightMinusDownAboveGround`, `trackAccelerationsKnownAnswers`, `trackAccelerationsAreTheGnssDefinitions`, `derivedRegistrationShape`; `tst_fusion_rows::allEightFusionPlotsAreExplicitBacked` |
| 857 | 13 | test: attitude, on synthetic quaternions with known answers: for the default orientation, heading, pitch and roll of a level, north-facing body are zero; a known heading, pitch and roll come back; heading unwraps through a full turn; a stored course reference offsets heading by exactly the angle it offsets the GNSS course, and heading is unavailable exactly when the course is; a different orientation (a side mount) changes the angles as the axis permutation predicts; the rotation is proper for all 24 pairs | `tst_fusion_derived::levelNorthFacingBodyReadsZero`, `knownAnglesComeBack`, `headingUnwrapsThroughAFullTurn`, `sideMountPermutesTheAngles`, `orientationRotationIsProper` |
| 858 | 13 | test: the orientation attribute: every recording reads the default without a stored value and without any write to its file; a stored token wins and the attitude recomputes without a fit; "Default" removes the stored value; a token outside the list is refused by the model, the editor and the bulk edit; the labels and tokens come from one enumeration of 24 | `tst_fusion_derived::orientationColumnShowsTheDefaultWithoutAWrite`, `storedOrientationRecomputesWithoutAFit`, `orientationDefaultRemovesTheStoredValue`, `orientationEditStoresATokenAndRefusesOthers`, `orientationBulkEdit`, `orientationDefinitionIsTheEnumeration`; `tst_choice_attribute::cellEditorOffersTheList` |
| 859 | 13 | test: the choice type: display and sort by label; the in-place editor and the context menu offer the list; the bulk edit sets the token for the selected sessions | `tst_choice_attribute::choiceShowsTheLabelOfTheEffectiveToken`, `choiceSortsByLabel`, `cellEditorOffersTheList`, `setDialogOffersTheList`, `bulkEditSetsAToken` |
| 860 | 13 | test: constant defaults: the helper registers a calculation that the engine serves as a default (the existing engine test covers the mechanism); wind north and east read zero for a recording without stored wind, keep a stored value, and the importer no longer writes them | `tst_builtins_engine::constantDefaults`; `tst_calcengine::constantCalculationIsADefault`; `tst_smoke::importAppliesCreationDefaults`; `tst_importer::creationDefaultsOnlyFillAbsent`; `tst_import_merge::newSessionGetsDefaults` |
| 861 | 13 | test: the fit is unchanged: the golden fixtures and the stored-result tests pass unchanged; the algorithm string is the same | `tst_fusion_golden::successFixturesMatchGolden`, `rejectionFixturesMatchGolden`; `tst_fusion_kernel::fitTraceMatchesGolden`; `tst_fusion_store::restoredAfterRestartIsBitIdentical`, `codeStampChangeDropsRecordOnLoad`; `tst_fusion_session::registrationShape` |
| 862 | 13 | test: the audit keeps the removed plot names out of the registry, and the documents describe the eight plots, the attitude convention, the orientation attribute and the rule for defaults | `audit naming` |
| 863 | 14 | `docs/` describes the eight fusion plots and the derived quantities, the attitude convention with the orientation attribute, its default and its limits (it describes the mount, not the wearer's posture; a forward axis pointing straight up or down makes heading and roll meaningless), the choice type and the orientation in the session file, the rule for defaults and its helper, and that wind is no longer written at import; no document describes a removed plot or the "GNSS (Local frame)" category | `audit naming` |

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
- **group `naming`** (items 120, 801, 803, 852, 862, 863): something is named
  after a filter (the case-sensitive pattern `EKF|[Ee]kf`), a branch output
  name (`posN` ...) or the branch's sensor key reappears; `MainWindow` does not
  register exactly eight "Sensor fusion" plots (the count pins the
  application's list to `fusionPlots()`, the tests' mirror); the "GNSS (Local
  frame)" category or the tests' old helper for it appears in `src` or
  `tests`; a plot row names a removed fusion measurement (the position, the
  velocity, the north and east acceleration, the device-frame roll, pitch and
  yaw, the quaternion); or `docs` or the root `README.md` describe the old
  count of fusion plots, the local-frame category or a quaternion plot. This
  file is excluded: this section spells the patterns;
- **group `solver-confinement`** (items 101, 234, 236, 848, 850): a GTSAM
  header is included outside `src/fusion/` and the five GTSAM test and tool
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
  605, 608, 625, 628, 629, 651, 659, 661, 737, 740): a
  gesture entry point (`plotCheckedByUser`, `refreshPressed`, `cancelPressed`)
  is named in `src` or `tests`; `prepare(` / `publish(` is called outside the
  executor and the engine; `request(` outside the engine; anything in `src`
  outside `src/engine` calls the engine's synchronous `request()` (spelled
  through `calculationEngine()`, `engine.` or `engine->`); `offer(` is called
  on other than exactly one line of `src`, or anywhere but
  `calculationdemand.cpp`, or `withdrawChosenNext(` anywhere but there (the
  demand layer is the only caller of the executor); product code cancels a
  job; the UI refers to the executor beyond `AppContext.h`;
  `EvaluationPolicy::Explicit` is tested outside the engine
  (`CalculationRegistry::explicitDependencies()` is the one authority for
  "explicit-backed"; `dependsOnExplicit()` is its non-emptiness);
  `cancel(JobId` is not declared exactly once in `src/jobqueue.h` (kept for
  the jobs dock); the executor's removed idle and queued signals, its two-job
  query or its busy-period bookkeeping (`jobQueued`, `activeJobs(`,
  `JobQueue::idle`, `announceIdleIfIdle`, `m_idleAnnounced`, `AfterEnd`)
  appear in `src` or `tests`, or `idle()` in `src`; or the demand layer's
  own-offer memory (`m_offeredJob`, `withdrawOwnOffer`) reappears;
- **group `widget-free-core`**: the executor, the job model, the demand
  layer (its four files: its presentation values and their text use Qt Core
  only) or the plot model include a widget header (a style option and a
  header view included);
- **group `fusion-model`** (items 212, 218, 234, 247): a name of the retired
  resting-window detector or of its gates reappears in `src`, `tests`,
  `cmake`, `docs`, `README.md` or `CMakeLists.txt`; the silent cancellation
  poll or the retired single-anchor initial-attitude type reappears in `src`
  or `tests`; a retired constant-bias algorithm string (the `v1` / `v2`
  strings the goldens once carried; the goldens say `batch-temperature-bias-v3`,
  and a hit under `tests/data/fusion` means a stale capture) appears in `src`,
  `tests`, `docs` or `README.md`; the name of the branch the kernel was ported
  from appears anywhere but in this file's historical notes and in the
  provenance comment of `tst_fusion_kernel.cpp`, or this file's count of it
  changes; the kernel has other than three `checkpoint(` call sites (`Starting
  fit`, the pass iteration, `Integrating IMU factors`: the three kinds of
  boundary) or other than one `.integrateMeasurement(` call in
  `imuintegration.cpp` (the per-step covariance is set before it); or
  `docs/SENSOR_FUSION.md` names a retired mechanism (a resting or candidate
  window, the coarse-only initializer, a frozen algorithm, the bias-shift
  test, the branch, or the input count from before the temperature channel).
  This file is excluded from the text rules of this group because this
  section spells the patterns;
- **group `fusion-tooling`** (items 231, 233, 848): `fusion_runner.cpp` names the
  preferences singleton, the logbook manager, the engine's preference
  provider, the session model, the application's import driver, the
  import-time defaults or the executor; the runner or the capture tool
  formats a number with anything but `CsvFormat` (`QString::number(`, the
  shortest-form flag, `std::to_chars`, `QLocale`, a `'g', 17` format); an
  install rule names `fusion_runner`, `fusion_golden_capture` or
  `solver_deploy_probe`; or anything under `tests` other than
  `tst_fusion_kernel.cpp` includes an internal fusion header (the runner sees
  `fusion/fusion.h` and `fusion/fusionregistration.h` only; the capture tool
  and `fusiontrace.h` see the trace seam `fusion/fusionpipeline.h`, which is
  not in the pattern);
- **group `stored-results`** (items 304, 305, 316, 317, 326, 327, 330, 333,
  334, 346, 348, 349): the record file extension is spelled as a literal in a
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
  its own; the pending queries appear outside the demand layer and the cell
  delegate; the session model, the logbook, the column store, the scheduler
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
  501-563, 601-662, 701-754 and 801-863; an item 1-19 has no line; or an item
  101-120, 201-247, 301-350, 401-442, 501-563, 601-662, 701-754 or 801-863 has
  no test or audit line.

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
the segmented initializer, the temperature model); the goldens on disk are the
capture of the last of them, and the closing audit of the plan re-ran the tool
and found no difference.

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
| `coarse_linear` | The exact constant-velocity case. IMU `t = i*.01`, `i = 0..200`, force `(0, 0, -9.80665)`, rate 0. GNSS `t = .037 + i*.2`, `i = 0..8`, position `(7, 8, 9) + t*(12, -4, 2)`, velocity `(12, -4, 2)`, `hAcc = vAcc = 1`, `sAcc = .1`. Origin index 0. No noise | a window shorter than one segment (one segment, the 60 s prefix covers it), an exactly unobservable yaw (the four prefix starts tie and the first wins), near-zero objective, boundary timing; 9 states, 160 output samples; zero signal change, so the per-step IMU noise term is identically zero and this golden's numbers are the density-only model (they did not change when the term was added); a constant 25 degC IMU temperature (`model.gyro_bias`: `b1` at its prior, `t_ref_degc` 25) |
| `coarse_maneuver` | 6 s. IMU `i = 0..600` at `.01`. GNSS `t = -.163 + j*.2`, `j = 0..32` (one fix before and two after IMU coverage). Origin index 3; `hAcc = 12` for `j < 3`, else `1.5`; `vAcc = 2.5`, `sAcc = .3`. NED acceleration `(1.5 - .4t, .8t, -.6 + .1t^2)`, `v(0) = (20, -5, 3)`, `p(0) = 0`, velocity and position its exact polynomial integrals; attitude identity, so force `= a - (0, 0, 9.80665) + (.05, -.03, .08)` and rate `= (.2, -.15, .3)` deg/s. Uniform noise: force `.02`, rate `.05` deg/s, position `.3`, velocity `.1`; seed `0x8F050002` | trimming of fixes outside IMU coverage, a fit that starts after the first fix, the full fit started from the segment fit's solution, non-zero biases and residuals; 28 states, 540 output samples; the per-step term on a noisy 100 Hz stream, where it is small against the density; a constant 25 degC IMU temperature (`model.gyro_bias`: `b1` at its prior, `t_ref_degc` 25) |
| `stationary_spin` | 40 s. IMU `i = 0..1000` at `.04` (25 Hz keeps the golden small). GNSS `t = .1 + j*.2`, `j = 0..199`. Position and velocity zero. Force `(0, 0, -9.80665) + (.03, -.02, .05)`; rate `(.2, -.1, .15)` deg/s plus 90 deg/s on `wz` for `t >= 31`. `hAcc = 1`, `vAcc = 1.5`, `sAcc = .1`. Noise: force `.005`, rate `.02`, position `.2`, velocity `.03`; seed `0x8F050003` | one segment at rest whose prefix grows from 60 s to 120 s to cover it (yaw unobservable and arbitrary; the anchor is the first fix, every sAcc being equal), yaw through more than two turns (unwrap); 200 states, 995 output samples; the per-step term at 25 Hz, including the 90 deg/s step at 31 s (one step with a change of 1.57 rad/s); a constant 25 degC IMU temperature (`model.gyro_bias`: `b1` at its prior, `t_ref_degc` 25) |

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

There is no temperature rejection fixture: every one of the twelve carries the
channel (a constant 25 degC, `kFixtureTemperatureDegC`, exactly representable
and noise-free), and the missing or malformed temperature is asserted in
`tst_fusion_kernel::validationRejectsEachDefect` on mutated channels of
`coarse_maneuver` (reasons `Missing or mismatched IMU/temperature` and
`Nonfinite IMU/temperature`, reported after every other channel's defect),
without a golden, and on the engine side in
`tst_fusion_session::missingInputsAreNotApplicable`.

**Initializer recordings (not goldens).** Four more synthetic recordings, for
the initializer's tests of the specification's section 10. `initializerFixture(name)`
returns them (beside `fusionFixtures()`, which never lists them, so
`fusion_golden_capture` never sees them); their expected values live in
`tst_fusion_kernel`, stated from their construction, not in goldens. Rotation
constants are exact rationals (Pythagorean `.6 / .8` and `.96 / .28`), the
body force is `R^T (a - g) + b_a`, and the gyro reads its bias only (the
attitude is constant in every recording); the same SplitMix64 noise rules
apply.

| Recording | Content | Exercises |
|---|---|---|
| `motion_start` | 90 s that start in motion. GNSS 5 Hz, `t = j * .2`, `j = 0..449`; IMU 25 Hz, `t = i * .04`, `i = 0..2250`. Attitude `Rz(psi)`, `cos psi = .6`, `sin psi = .8` (53.13 deg). `vN = 20`, `pN = 20 t`; `aE = 2` for `40 <= t < 50`, else 0; `vE` and `pE` its exact integrals; down zero. Body force `(.8 aE + .05, .6 aE - .03, -9.80665 + .08)`; gyro `(.2, -.15, .3)` deg/s. `hAcc = 1.5`, `vAcc = 2.5`, `sAcc = .3` (every fix: the anchor is the first). Noise force `.005`, gyro `.02` deg/s, position `.2`, velocity `.03`; seed `0x8F050004`. 450 states. Temperature 25 degC | test 1, "starts in motion" (`startsInMotionGrowsToTheManoeuvre`): the 60 s window `[0, 30]` has no yaw information, the 120 s window `[0, 60]` holds the manoeuvre, so the prefix grows once (`prefix_fits` 8) and stops `observable`; the full fit within 2 degrees of the truth. Also the growth of a prefix whose starts all fail (`allPrefixFitsFailFallsBack`: 60, 120, 240 s, twelve failed starts) |
| `rest_throughout` | 300 s at rest, tilted. IMU 25 Hz, `i = 0..7500`; GNSS `t = .1 + j * .2`, `j = 0..1499`; position and velocity zero. Attitude `Ry(theta)`, `cos theta = .96`, `sin theta = .28`; body force `(.28 * 9.80665 + .03, -.02, -.96 * 9.80665 + .05)`; gyro `(.2, -.1, .15)` deg/s. `hAcc = 1`, `vAcc = 1.5`, `sAcc = .1`. Noise as `stationary_spin`; seed `0x8F050005`. 1500 states. Temperature 25 degC | test 2, "at rest throughout, longer than two doublings" (`atRestPrefixStopsGrowing`): neither the 60 s nor the 120 s window has yaw information, the second sigma is not 20 % below the first, so growth stops at 120 s (`no_gain`); the full fit converges with roll and pitch within 0.5 degrees; the yaw is arbitrary and logged |
| `sacc_anchor` | 300 s at 15 m/s north with a 3 m/s^2 east manoeuvre from `t = 190` to `200` s. GNSS 1 Hz, `t = j`, `j = 0..300`; IMU 10 Hz, `t = i * .1`, `i = 0..3000`. Attitude identity. Body force `(.05, aE - .03, -9.80665 + .08)`; gyro `(.2, -.15, .3)` deg/s. `hAcc = 1.5`, `vAcc = 2.5`; `sAcc = 2` for every fix except `j == 200`, where it is `.3`. Noise `.005, .02, .2, .03`; seed `0x8F050006`. 301 states. Temperature 25 degC | test 3, the sAcc anchor (`smallestSaccFixIsTheAnchor`): the fix at 200 s is the anchor, the first prefix is the unclipped window 170-230 s (61 fixes) and contains the manoeuvre (`prefix_fits` 4, observable at 60 s), and the segment's start attitude is the prefix fit's carried back by the gyro with the prefix fit's bias |
| `drifting_bias` | 200 s at 15 m/s north with an east manoeuvre in every 30 s block (`aE = 2` for `10 <= s < 15`, `-2` for `15 <= s < 20`, `s` the time within the block) and a gyro z bias that drifts linearly by exactly 1 deg/s over the length while the attitude does not rotate. GNSS 1 Hz, `j = 0..200`; IMU 10 Hz, `i = 0..2000`. Attitude identity. Body force `(.05, aE - .03, -9.80665 + .08)`; gyro `(.2, -.15, .3 + t / 200)` deg/s. `hAcc = 1.5`, `vAcc = 2.5`, `sAcc = .3`. Noise `.005, .02, .2, .03`; seed `0x8F050007`. 201 states. The temperature ramps `25 + t / 10` degC (25 to 45 over 200 s), so the drift is `b1 = (0, 0, 0.05 deg/s per degC)` by construction, `T_ref = 35` (the mean of the ramp) and `b0 = (.2, -.15, .8)` deg/s, the bias at `T_ref` | test 4, the drifting bias (`driftingBiasSegmentsConverge`, under `segmentLength = 60`, `minFinalSegment = 12`: four segments of 60, 60, 60 and 21 fixes, each with its manoeuvre inside the anchor's 30 s half-window): every segment fit converges on a constant bias; the full fit with the temperature model recovers `b1` within 20 % in at most 30 iterations, `t_ref_degc` 35, `b0` at `T_ref`, the two priors last. The constant-temperature variant of test 9 (`constantTemperatureKeepsSlopeAtPrior`: 2001 values of 35.0) is made in the test, not in the generator; `tst_fusion_session::temperatureReachesTheKernel` runs the recording through a session |

The other three initializer recordings carry the constant 25 degC of every
golden fixture: the temperature is a required input, so none is
temperature-free.

### Files in `tests/data/fusion/`

- `<fixture>.json` (all twelve; `QJsonDocument::Indented`, so every double is
  in shortest round-trip form and therefore exact): `fixture`; `outcome`
  (`"succeeded"` or `"rejected"`); `diagnostics`, the kernel's diagnostics
  object (the key list of `docs/SENSOR_FUSION.md` section 4: the input audit,
  the segment account, objective, biases, residuals, `stopping`, `quality`,
  `model`; for a rejection it is
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
  `tst_fusion_session_exact`, `tst_fusion_jobs_exact`,
  `tst_fusion_rows_exact` and `tst_fusion_runner_exact` (label `exact`) are
  the ordinary fusion executables
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
$env:FLYSIGHT_FUSION_EXACT = "1"; ctest --test-dir build/FlySightViewer-build -C Release -R "tst_fusion_(golden|kernel|session|jobs|rows|runner)$" --output-on-failure
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
#    then "wrote 16 files to .../tests/data/fusion"; exit status 0. In the written
#    stationary_spin.json, initializer.segments[0].prefix_length_s is 120.

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
  only when a rejection reason changes. The `algorithm` string is
  `batch-temperature-bias-v3` since the temperature model; a capture that
  prints another string is from a stale build.
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
twenty-two effective inputs are bit-identical to the fixture, so that
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
the real input chain; its expectations are structural, not golden.
`syntheticFitSession()` is the opposite premise to the fixture sessions: the
fit's outputs, not its inputs, stored as source data `Fusion/<name>` (the
seventeen names only, each with its output's unit text, which the conversion
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
(`docs/DATA_SCHEMA.md` section 4). To reproduce the branch's objective, run the
check on a copy of the folder whose `SENSOR.CSV` has `$VAR,SCHEMA_VER,2` added
after the `$FLYS,1` line (the documented escape hatch). With the unmodified
legacy file a different objective is expected and correct. With the current
kernel the counts (9247 GNSS states, 24411 outputs) still hold and the
objective is not the branch's whatever the file says: the model changed. The
four reference recordings of the specification, with their expected
objectives, are in section 12.2.

## 12. Manual verification

Seven scripts. Each step opens with its bold id and, in parentheses, the items
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

Preamble. Build the test tree (`cmake --build build-phase1 --config Release`).
Git Bash, from the repository root; the runner links `flysight_core`, hence
GeographicLib, and the solver:

```bash
PATH="/c/Qt/6.9.3/msvc2022_64/bin:$PWD/third-party/GeographicLib-install/bin:$PWD/build-solver-deps/GTSAM-install/bin:$PWD/build-solver-deps/oneTBB-install/bin:$PATH"
R=build-phase1/FlySightViewer-build/Release/fusion_runner.exe
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

**M10 Runner streams and exit codes (233).** `"$R" --help; echo $?`: the usage on stdout, 64. `"$R" "TEMP/data/Data comp 5 - FS 2 - serie nr 2 - 014667 (test 10)/24-09-05/11-17-12" > TEMP/runs/11-17-12.json 2> TEMP/runs/11-17-12.log; echo $?`: stdout one JSON line; stderr the progress texts (`Starting fit`, `Integrating IMU factors`, `Segment i of n: prefix L s, pass p, iteration k`, `Segment i of n: pass p, iteration k`, `Pass p, iteration k`), then the outcome line; exit 0 for `Succeeded`. With `--csv TEMP/runs/11-17-12.csv`: a CSV whose header is the seventeen names and whose row count equals `imu_outputs`. A folder without `SENSOR.CSV`: exit 3.

**M11 Reference recording `11-17-12` (208).** The recording of M10 (bias 1.03 deg/s after the schema correction; the previous initializer accepted no resting window and did not converge in five passes). Expected (specification section 12): converged (`settled` or `slow tail accepted`), about 12 full-fit iterations, objective near 45,000 with the density-only model; with the per-step term the objective is lower (the lab measured 26,515 with a weaker term), so the iteration count and the rule are the comparison and the objective is recorded. First segment: the unit rests for 180 s, so its prefix grows to 240 s (`prefix_length_s` 240, `yaw_sigma_deg` a few degrees). On the current build the first segment's 60 s prefix is already observable (its anchor, the smallest-sAcc fix at about 246 s, is in motion), so `prefix_length_s` is 60, not 240, and the objective is about 20,000; the specification's expectation stays the reference here until Michael decides which is.

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

**M39 The eight fusion plots (801, 802, 803, 842).** The plot list has "Sensor fusion" with Elevation, Horizontal acceleration, Vertical acceleration, Along-track acceleration, Cross-track acceleration, Heading, Pitch and Roll, in that order, and no "GNSS (Local frame)" category. With two fusable tracks without stored fits visible, check all eight: the status bar shows "Computing results: 0 / 2", and one fit per track fills all eight. Check each GNSS counterpart as well (Elevation; the four accelerations; Course): each pair shares one axis and unit, and the two lines are told apart by colour. Pitch and Roll share the angle axis with Heading. Hide and show a track: its plots draw at once, with no computation. Check this in the light and the dark theme.

**M40 Attitude through turns (812, 814, 843).** On the recording with several turns, with the default orientation and a unit on the back of a helmet, label up: Heading shows no jump of 360 degrees at north. It overlays Course in straight flight and turns with it through every turn; the difference between them, read with the measure tool at a few points, is the sideslip and changes slowly. In straight flight Roll is near zero; it is positive in a right turn and negative in a left one.

**M41 The orientation column (817, 819, 822, 823, 824, 829, 844).** The Add Column dialog, "Session Attribute", lists "Orientation" in the "Session" group; a new logbook's default columns do not include it. Add it: every row shows "forward +y, up +z", and a text editor shows no `$VAR,_ORIENTATION` line in any session file; import a new recording: the same. Double-click a cell: a drop-down list opens, "Default" first and then the 24 labels, and it accepts no typing. Choose "forward +x, up +z": the cell shows it; the session file gains `$VAR,_ORIENTATION,+x,+z`; with Heading, Pitch and Roll checked for that track, they redraw at once; no "Computing results" appears, and the track's record file in `cache/` keeps its modification time. Choose "Default": the line is gone and the plots are back.

**M42 Setting it for several recordings (823, 824, 826, 830, 831).** Select rows that are loaded and rows that are not, right-click, and choose "Set Orientation...". A list opens, "Default" first, with no text box. Choose a label: every selected row shows it, and every file has the line. Choose "Default": the lines go. Cancel: nothing changes. Then, with the application closed, edit one session file's line to `$VAR,_ORIENTATION,sideways`. Start: its cell shows "sideways" and its Heading, Pitch and Roll draw nothing. Choosing a label in the cell draws them.

**M43 An old profile (805, 845).** With the application closed, add `Fusion/qx`, `Fusion/roll` and `Local/north` to the `enabledPlots` list of a copy of a profile file in `Documents/FlySight Viewer/profiles/`, beside a GNSS plot it already lists, and note the file's modification time. Start and apply the profile: the GNSS plot is checked, and Sensor fusion > Roll is not. No message box appears, and the debug output has no line about the three ids. The file keeps its bytes and modification time.

**M44 A column kept from before (804).** Set up a logbook copy with a build from before this change, with a column over Sensor fusion > Roll at the exit marker, filled. Start this build: the column still shows its values, labelled "Fusion/roll @ <marker>", with no fit. The Add Column dialog's measurement tree lists eight Sensor fusion measurements and no local-frame group.

Pass / fail and a note per step go in the phase report. A step that fails is
reported as it failed, not adjusted.

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
"Sensor fusion plots, attitude and the orientation attribute" (appendix I).
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
    action. The real fusion plots are the eight of the "Sensor fusion"
    category, and heading, pitch and roll share one job.
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
bar for background work" (appendix H).

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
15. (5) The per-step term is `sigma = slope x dt x |change of the interpolated
    signal across the step|`, added in quadrature so that the step's
    covariance is `(density^2 + sigma^2 x dt) I`, and a step without signal
    change has the density covariance exactly.
16. (5) The slopes are 0.026 (gyro) and 0.40 (accelerometer); the densities,
    the step boundaries and the midpoint sampling are unchanged.
17. (5) The `dt` factor keeps the constants valid at higher output rates.
18. (5) The per-step covariance is applied by setting the preintegration's
    shared parameters before each `integrateMeasurement` call.
19. (6) The gyro bias is `b(t) = b0 + b1 (T(t) - T_ref)` with `T_ref` the mean
    IMU temperature over the fitted window, the accelerometer bias stays
    constant, and each interval is evaluated at its own bias in the fit and
    in the reconstruction.
20. (6) `b0` keeps today's prior (0.03 rad/s); `b1` is zero-mean with sigma
    0.010 deg/s per degC.
21. (6) `IMU/temperature` is the twenty-second input, required, one value per
    IMU sample; a recording handed no temperature is rejected by the kernel
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
27. (7) The diagnostics report the per-step constants and the fitted `b0` and
    `b1` with `T_ref`, always as numbers.
28. (7, as amended) The failure text, the status bar warning's hover and the
    logbook row's, keeps showing the reason, as the tooltip did; the
    diagnostics remain an account, not an input.
29. (8) The runner takes a folder or the two paths, imports them exactly as the
    application does (parser, conversion layer, on-demand derivation, the
    legacy gyro scale) and feeds the kernel the channels the job queue would.
30. (8) The runner prints the diagnostics JSON on standard output, writes the
    seventeen channels as CSV on request, and exits 0 for Succeeded and
    non-zero otherwise with the outcome and reason on standard error.
31. (8) The runner has no GUI and neither reads nor writes the user's logbook
    or settings.
32. (8) Progress texts go to standard error; cancellation is not required.
33. (8) The runner is a test/tooling target, built with the tests and never
    shipped.
34. (9) The kernel's purity, threading and cancellation rules are unchanged: a
    function of the channels, no session or GUI object, every solver
    iteration a boundary, cancellation observed at boundaries only.
35. (9) The public result contract (channels, outcomes) is unchanged, and a
    slow-tail acceptance is a Succeeded result.
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
43. (10, test 6) Bias-settled test: a fit whose cost stops changing converges
    within two passes.
44. (10, test 7) Slow tail: a forced final pass at the limit is accepted when
    both conditions hold and is a failure when either fails.
45. (10, test 8) Per-step term: zero signal change gives the density
    covariance exactly, a known change gives the specified covariance, and the
    term scales with `dt`.
46. (10, test 9) A recording without `IMU/temperature` is rejected by the
    kernel and blocked in a session like any missing input; a constant
    temperature leaves `b1` at its prior and reproduces the constant-bias fit.
47. (11) `docs/` describes the segmented initializer, the stopping rule with
    its slow-tail acceptance, the per-step term and its meaning at other
    output rates, and the temperature-dependent bias, in the place that
    documents the fusion model.

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
    yields the seventeen channels and the diagnostics bit-identical to the
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
(appendix I).

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
   that block that output. The eight plots of the "Sensor fusion" category are
   such plots: one is an output of the fit, and the seven derived from its
   outputs are blocked by it.
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
19. (6) The running job is never stopped because its pair left demand: it
    finishes and its result is published and stored; it is stopped only when
    its inputs change, its session goes away, or the application closes.
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
29. (9) The executor keeps its cancel operation for the jobs dock; no product
    code calls it, and its comment and the audit say so.
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
33. (10.1) A session that is not loaded is a track of columns only, classified
    in this order: no storable calculation, not applicable; a storable
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
    signals and queries (cancel kept for the jobs dock), the one walk, the one
    memory, the parts of the demand layer, the fill's completion, the failed
    write, the session model as the source of column knowledge and display
    names, and the status bar and the row warning; not the indicator, the clock
    or the fill's progress text.

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
settled (9.9 says how).

1. (5) The "Sensor fusion" category holds exactly eight plots, in this order:
   Elevation, Horizontal acceleration, Vertical acceleration, Along-track
   acceleration, Cross-track acceleration, Heading, Pitch, Roll.
2. (5, as settled) Each is named, united and typed as its GNSS counterpart
   (Elevation, the four accelerations, Heading as Course), Pitch and Roll as
   angles, so that the two overlay; a fused plot's colour differs from its
   counterpart's.
3. (5) The fused north, east and down position and velocity, north and east
   acceleration, roll, pitch, yaw and quaternion plots are removed, and the
   "GNSS (Local frame)" category with them.
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
    the course's, and less the course reference angle the GNSS course
    subtracts, computed in one place for both (zero for a stored reference that
    is not a number or lies outside the GNSS time), so heading and course
    overlay in straight flight and heading minus course reads as sideslip;
    heading has the course's inputs and availability, so a recording without a
    course reference has neither.
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
19. (8) Its default, forward +y and up +z, is a constant calculated attribute:
    every recording has it, imported before or after, and nothing is written
    into any session file.
20. (8) A stored value wins over the default, and removing it returns to the
    default.
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
24. (8) Both editors offer the list, not free text, and a "Default" entry that
    removes the stored value rather than storing anything.
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
32. (9) "Default" removes the stored attribute on every edit path, and a token
    outside the list is refused.
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
41. (10) A stored attribute wins even when invalid or empty, so returning to a
    default removes the stored attribute and never stores a blank.
42. (11) The plot list shows "Sensor fusion" with eight plots named as the GNSS
    plots and no local-frame category; checking a fusion plot starts the fit as
    before, the status bar shows it, and a stored fit draws at once.
43. (11) The attitude plots are in the aircraft convention for the mount the
    orientation describes, heading continuous through turns.
44. (11) The orientation column, once added, shows "forward +y, up +z" for
    every recording not set and the chosen label for one that is; editing
    offers the list and "Default"; changing it redraws the attitude plots
    without a fit.
45. (11) Profiles that name removed plots apply without complaint.
46. (11) Wind reads zero where nothing was stored, as before.
47. (12) The fusion kernel and the fit calculation are unchanged (inputs,
    outputs, diagnostics, algorithm string), and the golden fixtures and stored
    results stay valid.
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
54. (13) Test: the plot list has the eight fusion plots and no local-frame
    plots; every fusion plot is explicit-backed (waits on the fit) as the
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
    through a full turn; a stored course reference offsets heading by exactly
    the angle it offsets the GNSS course, and heading is unavailable exactly
    when the course is; a different orientation (a side mount) changes the
    angles as the axis permutation predicts; the rotation is proper for all 24
    pairs.
58. (13) Test: the orientation attribute: every recording reads the default
    without a stored value and without any write to its file; a stored token
    wins and the attitude recomputes without a fit; "Default" removes the
    stored value; a token outside the list is refused by the model, the editor
    and the bulk edit; the labels and tokens come from one enumeration of 24.
59. (13) Test: the choice type: display and sort by label; the in-place editor
    and the context menu offer the list; the bulk edit sets the token for the
    selected sessions.
60. (13) Test: constant defaults: the helper registers a calculation that the
    engine serves as a default (the existing engine test covers the mechanism);
    wind north and east read zero for a recording without stored wind, keep a
    stored value, and the importer no longer writes them.
61. (13) Test: the fit is unchanged: the golden fixtures and the stored-result
    tests pass unchanged; the algorithm string is the same.
62. (13) Test: the audit keeps the removed plot names out of the registry, and
    the documents describe the eight plots, the attitude convention, the
    orientation attribute and the rule for defaults.
63. (14) `docs/` describes the eight fusion plots and the derived quantities,
    the attitude convention with the orientation attribute, its default and its
    limits (it describes the mount, not the wearer's posture; a forward axis
    pointing straight up or down makes heading and roll meaningless), the
    choice type and the orientation in the session file, the rule for defaults
    and its helper, and that wind is no longer written at import; no document
    describes a removed plot or the "GNSS (Local frame)" category.
