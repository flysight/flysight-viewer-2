# Coordinator integration audit (pending Phase9)

This is a bounded inventory, not completed validation.

- Full application cache: build/FlySightViewer-build, baseline Release succeeded before changes. Final Release rebuild is mandatory after phases finish. Build using normalized Windows environment runner; /m:1 /nr:false.
- MainWindow::closeEvent (src/mainwindow.cpp) currently ignores flushDirtySessions failure. Phase7 changes it to bool; final UI must keep unsaved state open and report error, never silently close after failed save.
- MainWindow::on_action_Delete_triggered currently removes model rows then separately calls LogbookManager.removeSession/flushIndex without checking each disk failure. Coordinate Phase7 ownership and final error handling; avoid double deletion after model lifecycle change.
- External sessionRef callers: plottool/setgroundtool.cpp; ui/docks/video/VideoWidget.cpp; ui/docks/plot/PlotWidget.cpp (six). Phase7 adds trySessionRef returning null on load failure; migrate consumers to skip/report safely rather than uncaught legacy-reference error.
- MainWindow importFiles uses a fresh SessionData per path and then mergeSessions. New row preserves incoming snapshot/choice; existing row imports additional source content preserving destination overlays. Saved codec detection in importer skips new-import defaults. File-dialog/drop extension handling must include .fsv.
- ProfileManager::profileDirectory unconditionally uses real Documents. Before UI smoke, provide explicit scoped profile root override for tests, redirect QSettings before singleton creation, seed GeneralLogbookFolder to a QTemporaryDir, keep plugins isolated. Do not launch normal executable against user state.
- Final UI check can use Qt offscreen harness + screenshots inspected with view_image; exercise actual command/preference wiring. Real desktop interaction not yet performed.
- PluginHost static interpreter and global registry callable destruction order needs Phase6 explicit unregister/shutdown/lifetime behavior; callback-scoped Python view must reject retained references after callback (raw stack SessionData pointer is unsafe). Actual bridge tests required, no mock-only substitute.
- Remaining compatibility adapters intentionally supported: one-output pure scoped SessionData callback; old cache setters currently throw and require Phase5/6 migration. Retired old calculatedvalue/dependencymanager files removed from build by Phase3 but still present for Phase9 removal. DataImporter GyroScaling enum remains temporary, rejects non-Automatic, Phase9 removes prototype API.
- Phase6 registry ownership audit: src/CMakeLists.txt links flysight_model STATIC into both application and external flysight_cpp_bridge .pyd. Existing host registration occurs in executable and methods operate on passed session/view, which can work. Do not expose new bridge-side static registration and accidentally register into a second copy of SessionData::calculationRegistry. Keep host registrations explicitly owned by app-side core, or deliberately unify module/core linkage. Actual host + external-module bridge testing should catch this. Avoid changing to shared library casually without deployment/export handling.
- Persisted cache compatibility final gate: provider replacement/unregister must also invalidate unloaded model rows (Phase7 fixed via registry revision sync). Across restarts, calculation-core token alone is insufficient for external Python/provider changes. Phase6/9 must supply deterministic plugin compatibility fingerprint or conservatively reject cached columns where compatibility cannot be proved. Consider dynamic altitude-marker registration removal too: keys include threshold/unit, but a persisted column for a removed marker must not remain valid. Runtime numeric registry revision alone is NOT a stable startup signature. Keep a bounded compatibility policy; no second evaluator.

Coordinator startup-cache decision for Phase6/9: runtime registry revision handles in-process provider changes. Across restarts, arbitrary Python providers have no declared durable compatibility guarantee. Use a small conservative policy: PluginHost exposes whether calculation providers were registered; when present, application tells LogbookManager to rebuild persisted column values at startup (session/index identities still recover normally). Do not invent a callable hash or second registry. For native dynamic altitude markers, include their settings definition (units and altitude array) in the existing index calculation context with descent pause. Built-in algorithm token still changes independently of SCHEMA_VER. Document the deliberate startup-recomputation cost. Phase9 owns manager/app wiring + restart regression after Phase7 releases; Phase6 owns host provider-presence accessor.

Coordinator integration changes after Phase7 release:
- External throwing sessionRef consumers migrated to checked trySessionRef in setgroundtool.cpp, VideoWidget.cpp, and all five PlotWidget.cpp call sites. Existing failure/skip return behavior retained; actual full app compilation still Phase9 gate.
- LogbookManager exposes setPersistentColumnReuseEnabled(bool), initialize requires policy plus persisted persistentColumnsReusable=true; writes retain the flag so removing plugins next launch cannot rehabilitate old plugin results. Native altitude marker units/array now part of index context. Additional source-byte-preserving startup regression added to actual model suite. Phase9 MUST wire !PluginHost.hasCalculationProviders() before initialize; host accessor6.
- removeSession of an ID without any committed mapping succeeds as no-op, allowing confirmed deletion of never-saved imported rows. Dedicated model regression added.

## Phase 9 cutover audit (2026-09-17)

Audited the actual native getter/presence/enumeration call sites across built-ins,
MainWindow, plot utilities/tools, plot/video/map/legend/analysis docks, SessionModel,
SessionData, importer/exporter, units and the Python bridge. Findings:

| Boundary | Current result |
| --- | --- |
| Plots and tools | Ordinary getMeasurement/getAttribute; display conversions stay in UnitConverter; failed loads use trySessionRef |
| Map and simplification | TrackMapModel reads native effective Simplified vectors; simplification reads effective GNSS inputs and returns one four-field result |
| Markers/analysis | Ordinary tracked attributes; hasAttribute intentionally describes persistent selection for bold/reset controls; remove/reset suppresses source inheritance |
| Stored enumeration | Plot full extent enumerates stored sensor keys then reads effective time; no cache-dependent enumeration is needed |
| Logbook columns | Ordinary effective getters; persistent file/index lifetime in model; runtime provider revision sync before reads |
| Python | Callback-scoped read view, effective results returned in declared units; explicit source reads tracked; invalid prerequisite kinds rejected |
| Save/merge | DataExporter serializes persistentSnapshot; importer/model merge source snapshots; no effective values materialized as source |
| Source construction | RecordingParser and validated Mutation carry source/unit/axis context; setMeasurement/setUnit only replace existing complete tuples, and have no production callers |
| Dependencies | InputKey internal source/session kinds stay separate from public two-kind DependencyKey notifications/prerequisites; no source kind falls through as measurement |

Removed inactive calculatedvalue.{h,cpp} and dependencymanager.{h,cpp}. Production
build already used the single MeasurementEvaluator. Removed temporary Automatic-
only DataImporter::GyroScaling signature and its obsolete test assertion. The
excluded dataimporter_test.cpp has a prominent historical-only marker; its old
prototype assertions are intentionally retained as requested baseline evidence,
not as current buildable API examples. Pure one-output registration adapters and
cache-setter names that throw migration errors remain documented compatibility.

Current DATA_SCHEMA, README and plugin documentation now describe source versus
effective values, per-source schema versus session policy, normalized units,
source-only FSV storage, suppression/inheritance, numerical limits and cache
compatibility. The Viewer portion of TEMP/firmware-changes.md and historical
measurement-source proposal are marked superseded; no firmware code changed.

The opt-in FLYSIGHT_MEASUREMENT_LIFECYCLE_TEST application harness redirects both
INI settings scopes, explicit profile directory, logbook, controlled plugin root,
WebEngine storage/cache, and blocks web network requests before MainWindow starts.
A normal build refuses the test switch. No normal executable launch is authorized
for validation. Actual build/lifecycle and integrated results are recorded in the
Phase 9 handoff; this audit alone is not application acceptance.

Remaining loader query names are retained as compatibility adapters over the new
core, by coordinator decision: initialize always supplies real-identity stubs;
hasDeferredScan/scannedUuids are inactive historical queries, and loadAllSessions
uses the new source-preserving restore. The startup fallback branch is therefore
inactive, not another parser or identity authority. The obsolete remapSessionId
mutation path was removed by coordinator; an FSV identity cannot be remapped only
in the disposable index.

Final actual application lifecycle (build/measurement-app-final-8) passed both
separate processes with real renderer readiness and exit0. The restricted Windows
test child needed QTWEBENGINE_DISABLE_SANDBOX=1; generated content and network
blocking remained active, and no production sandbox default changed. The test also
found an actual dock ownership defect: closing docks detached them from MainWindow,
leaving the browser page alive during profile cleanup. MainWindow now deletes
feature wrappers and guarded owned docks before models/Python teardown, with safe
empty feature lookup during deletion. Plot feature callbacks bind to their feature
lifetime. Test page guards confirm browser deletion; the final run has no profile
release warning. Normal OFF Release configure/build passed; both harness switches
were rejected with exit2 before MainWindow and without creating the test root.
