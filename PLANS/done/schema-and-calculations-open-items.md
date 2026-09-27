# Schema and calculations: open items

**MERGED into `master` on 2026-09-20** as merge commit `13c6115` (pushed to
`upstream`). CI was green on all four platforms for the merged content
(`6389c80`); the merged `master` builds and passes 25 of 25 tests locally.
The superseded import-time gyro commit `4668f48` was never pushed; it was
dropped from `master` and is kept on the local branch
`superseded/import-time-gyro`. The feature branch (local and `upstream`) and
the `plan/phase-N-done` tags were deleted after the merge; the per-phase
commits remain reachable under the merge commit (`git log 7ed7382..13c6115`).

Sections 1 and 2 below are closed and kept as a record. What remains open is
section 3 (robustness notes, for a separate branch) and the parked spec
`PLANS/altitude-time-references.md`.

Branch `schema-and-calculations`, as of the engine cycle fix (2026-09-20).

**Status 2026-09-20:** CI is green on Windows, Linux, macOS arm64 and macOS
Intel (first run failed on two test-only platform assumptions, fixed in
`7fd2b9b`; `tst_python_bridge` passed everywhere, so the macOS RPATH and NumPy
concerns in 1.2 did not materialise). Michael has completed the manual GUI
checklist. GCC visibility warnings fixed in `382c59b`. What remains is the
decisions in section 2 still marked open, and the robustness list.
All 8 phases and the pre-merge fixups are committed; 25 of 25 tests pass on
Windows Release. Nothing has been pushed.

Fixed since this list was first written (removed from section 3):

- `7c3eb8c` marker bubble context menu held a reference across `menu.exec()`.
- `0a3c63a` logbook "Set ..." menu held row and column indices across its dialogs.
- `caa194a` bulk-edit queue addressed sessions by row index; it now queues by
  session id and attribute key, and follows identity-stub id remaps.
- `f9dd38d` plot, legend, and measure tool read sessions under a
  `RowStabilityGuard` (`updatePlot`, external cursor update, traced-sessions
  loop, `LegendPresenter`, `measuretool`, `referenceSession()`).
- `11983c8` duplicate logbook columns made every start reload every session
  (found on the real 105-session logbook; pre-existing at `v2026.04.1`).
- `74dfcf6` the environment fingerprint depended on registration order, so
  adding an altitude marker at runtime cost one extra full recompute at the
  next start. Logbooks recompute their cached columns once after this change.
- `823472a` import failure dialog groups files by reason and shows each
  delete-and-re-import hint once.

This file gathers everything still open in one place. Sources:

- Manual checklist M1-M14: `PLANS/implementation-plan/08-docs-acceptance-and-audit.md`, Task 8.12 (lines 690-711). Copied below.
- Open questions Q1-Q17: same file, "Consolidated open questions for Michael" (line 718). Copied below with their current state.
- Cross-phase findings F1-F12: same file, "Cross-phase findings" (line 135). All were handled during Phase 8; not repeated here.
- Robustness notes: these came from the final review agents and the fixup reviewers. Until now they existed only in the chat session, not in any file.

`PLANS/` is git-excluded, so this file is local only.

---

## 1. Must happen before merge

### 1.1 Manual GUI checklist (never run by any agent)

No agent launched the application, to protect the real logbook. Run this
yourself.

**Always on a copy.** Point Preferences -> General -> logbook folder at a copy
of a released-version logbook (or an empty folder) before anything else, and
restore the preference at the end. M1 needs a logbook written by a released
build. (A logbook written by the sensor-fusion build holds `.fsv` session
files, which this branch cannot read: its rows show blank and its sessions
cannot be re-imported until `index.json` is deleted. See section 3.1.)

| # | Do | Expect |
|---|---|---|
| M1 | Start with the copied released logbook, with a gyro-dependent logbook column enabled (measurement-at-marker on `IMU/wx`) | Cells blank briefly, then fill with values about 14.7 % larger than the released build showed; `index.json` gains `calculationCompatibility`; no file under `sessions/` has a new modification time; no dialog |
| M2 | Import a legacy `TRACK.CSV` + `SENSOR.CSV` device folder | One session; description from the folder; no dialog, badge, or warning |
| M3 | Plot gyro (`wx`, `wy`, `wz`, `wTotal`) for that legacy session next to the released build | 1.14688 x the released build's values; acceleration, magnetometer, temperature, pressure, GNSS plots identical |
| M4 | Add `$VAR,SCHEMA_VER,2` to a copy of a `SENSOR.CSV` with a different `SESSION_ID` and import | Gyro plots equal the numbers in the file, unchanged relative to the released build |
| M5 | Escape hatch on the M2 session (same `SESSION_ID`, add `SCHEMA_VER,2`, re-import); then a copy saying `SCHEMA_VER,1` | Same session, gyro drops by 1/1.14688, edits intact; second import: dialog shows the file, `SCHEMA_VER`, both values, and the delete / re-import hint |
| M6 | Markers: drag exit / manoeuvre markers, "Reset to default" on a bubble, WS-P and SP docks change parameters and restore defaults | Dependent markers, plots, and docks update; defaults return |
| M7 | Edit a description containing a comma; restart | Logbook shows it; the CSV line is verbatim; only that `$VAR` line changed in the file (diff against a copy) |
| M8 | Preferences -> Import -> descent pause 30 -> 5 with a session plotted and many unloaded sessions | Analysis-range markers move without re-import; exit-dependent logbook columns blank and refill; nothing under `sessions/` is rewritten. Mass / area changes move nothing |
| M9 | Preferences -> Altitude markers: add an altitude, remove it, switch units, with a session plotted | Markers appear / disappear on the plot, no stale values. (The plan's "altitude logbook column" cannot be created: the Add Column dialog skips grouped markers, and always has. See section 2, "Altitude markers as logbook columns".) After adding a marker, close and restart twice: the second start shows no "Computing columns" |
| M10 | Re-import identical files | Nothing happens: no save progress, file modification time unchanged |
| M11 | Conflicting `FIRMWARE_VER` copy; a folder with more than five broken files | Dialog names attribute and both values; files that failed for the same reason are listed under that reason; at most ten files shown, then "...and N more."; the delete-and-re-import hint appears once, at the end; same dialog box as before (title, icon) |
| M12 | Plugin example: copy `python_plugins/examples/imu_tilt.py` next to the SDK in `build/install/python_plugins/`, restart | "Tilt pitch" / "Tilt roll" plots exist and plot in degrees; a scratch plugin that raises logs one traceback and the app keeps working; clean exit |
| M13 | Kill the process right after a description edit of a large session; restart | The logbook cell always matches the `_DESCRIPTION` line in that session's CSV |
| M14 | Look through Preferences and menus | No new preference, page, dialog, badge, or menu item compared with `v2026.04.1` |

Added by the fixup round (not in the plan's list):

| # | Do | Expect |
|---|---|---|
| M15 | Try to edit `DEVICE_ID` in the logbook (double-click, and the right-click "Set ..." menu) | Not editable; no "Set Device ID" menu entry |
| M16 | Shift+Z (zoom to extent) with several sessions visible; import a track | Same framing as the released build; no pause on large sessions |
| M17 | Right-click a marker bubble with a stored override, leave the menu open for several seconds while sessions load in the background, then choose "Reset to default" | Marker returns to its calculated position; no crash |
| M19 | Hover the plot with several sessions visible: plain hover, Shift multi-trace, Ctrl focus lock; drag the measure tool in single-track and multi-track mode; show the legend in point mode and range mode | Cursor, tracers, legend, and measurements behave as in the released build. The multi-track measure average matches the released build to the last digit on the same data |
| M20 | Change the reference marker, then edit an attribute it depends on (e.g. drag the exit marker) | The viewport shifts to keep the view, as in the released build |
| M21 | Set the logbook cache size small (Preferences), hover a visible session, then hide it from the logbook while the mouse is over the plot | Cursor and legend recover on the next mouse move; no crash, no pause |
| M22 | In Preferences -> Logbook, try to add a column that already exists | The existing column is selected (and enabled); no second copy appears |

M18 (sort while a bulk edit is running) was dropped: each bulk-edit step on an
unloaded session parses and rewrites a 20-35 MB file on the UI thread, so the
interface does not respond until the batch finishes and the scenario cannot be
produced by hand. Five tests in `tests/tst_column_cache.cpp` cover it through
the real scheduler.

**Debug build, once.** The `RowStabilityGuard` asserts compile out in Release
and there is no Debug build on this machine, so they have never been seen
running. Configure a Debug build, point it at the copied logbook, and run M16,
M19, M20, M21, and a large import. No `assertRowsMutable` or `loadedSession`
assert should fire. One that does is either a real lifetime bug or a guard span
that is too wide; both are worth knowing before merge.

Note for M12: `pitot_tube.py` and `allan_variance.py` in
`build/install/python_plugins/` import SDK classes (`DefaultStartTime` and
friends) that no longer exist. They will fail to import; this is logged and
non-fatal. They are local files, not in the repository.

### 1.2 First CI run

The three `CI test step (unverified)` commits (`1cae709`, `d7878ba`,
`c6368b9`) have never run. Tests run on branch / PR builds only; tag builds keep
the plain release pipeline.

- Push the branch and wait for green on all matrix entries before tagging.
  Same-SHA runs cancel each other by design.
- Most likely failures: macOS test executables not finding `libGeographicLib`
  or `libpython` (the `BUILD_RPATH` line in `tests/CMakeLists.txt` is marked
  unverified); `tst_python_bridge` not locating NumPy or the bridge module in
  the single-configuration (Ninja) layout on macOS / Linux; NumPy wheel
  availability on `macos-15-intel`.
- `tests/CMakeLists.txt` (~line 163) runs an `execute_process` to find
  `sys.base_prefix` without checking the result; a failure would set an empty
  `PYTHONHOME`.
- "Run tests" sits before Install / Package / Upload, so a failing test means
  no artifact for that push. Decide whether that is what you want.
- `tst_python_bridge` is marked `DISABLED` when NumPy is missing at configure
  time or CMake is older than 3.22, and ctest still reports success. Consider
  making that a hard failure when `FLYSIGHT_BUILD_PYTHON_TESTS=ON`.

---

## 2. Decisions for Michael (Q1-Q17)

**Recorded 2026-09-20:**

- Q1: yes. Python plugins register before built-ins and supersede them.
- Q7: accepted as implemented (gyro as recorded plus a warning in memory; the
  exporter refuses to save an unsupported `SCHEMA_VER`).
- Q9: keep numeric seconds. ISO timestamps become double epoch seconds; sensor
  system time stays seconds. `QDateTime` attributes were tried in the past and
  abandoned. Amend the spec 9.2 sentence accordingly.
- Q2: keep (the session's `_` value wins; an import should not undo edits).
- Q4: FIXED in the engine (context-free caching plus a footprint check on
  nested hits of cycle-derived entries). Reviewed with an independent
  cache-free reference fuzzer, about 40,000 seeds, no mismatch.
- Q10: keep the environment fingerprint; refine later (section 3.1).
- Q14: all conflicts reported and the backfill shim stay. The `n/a`
  exemption is nearly dead code (only FlySight 1 units lack an id, and they
  produce one file per session, whose id is the file hash); left in as
  harmless unless Michael asks for its removal.
- Q15: RESOLVED by removal. Plugins read effective values only;
  `allowSourceInputs`, SDK `source()`, and the three source methods on the
  Python view are gone, and the audit forbids their return. Spec 7.5 now holds
  to the letter. A plugin that needs the recorded schema declares
  `attr("SCHEMA_VER")`.
- Qt deprecations fixed in `81e9c9a`; check by hand that the plot re-themes
  when Windows switches between light and dark.

"Default" is what the code does now. None of these has a recorded answer. The
five marked **decide** are the ones worth a deliberate answer before merge; the
rest can stand unless you disagree.

| Q | Question | Default in the code | State |
|---|---|---|---|
| Q1 | Registration order: should Python plugins precede built-ins, so a plugin declaring a built-in output wins? | Yes, preserved from baseline | **decide** |
| Q2 | `_` attributes in an incoming Viewer-saved file on merge | Existing session value wins, absent ones are added, never a conflict | **decide** |
| Q3 | Should CI run the tests, and also on release tags? | Branch / PR builds yes; tag builds no | Implemented; unverified until a push |
| Q4 | The engine's cycle rule has no proof for arbitrary overlapping cycles: with two interlocking rings, which calculation reads as unavailable can depend on read order | Accepted. Built-ins are acyclic and the oracle asserts zero cycles; only a plugin overriding built-in outputs could create this. Documented in `docs/CALCULATIONS.md` section 5 | Open; low exposure |
| Q5 | Stale cached columns of unloaded sessions on preference / registration change | Resolved by the environment fingerprint (Q10) | Resolved |
| Q6 | Nine golden values are captured from the old engine, not hand-derived (`_WSP_DIST_RESULT`, `_WSP_SPEED_RESULT`, `_SP_WINDOW_END_TIME`, sample [19] of six GNSS measurements) | Keep the captured literals | Stands |
| Q7 | An unsupported `SCHEMA_VER` set in memory (cannot come from a file) | Gyro as recorded plus a warning; the exporter refuses to save the session | Implemented; confirm |
| Q8 | Header strictness: duplicate `$COL`, more units than columns, empty column name, missing `$DATA`, FS1 header with a trailing comma all reject the file. The released build accepted some of these | Reject; downgrade a rule to a warning only if a real file trips it | Watch during M2 with real FS1 files |
| Q9 | Spec 9.2 says "ISO timestamps are written at millisecond precision". Time columns are saved as numeric epoch seconds (as every release did), e.g. `1718900000.123`; a whole second can appear as `1.7189e+09`. Values round-trip bit-exactly. Only `QDateTime` attributes are written as ISO text | Numeric seconds. The alternative needs a per-column "recorded as ISO" flag touching import, merge, and export | **decide** (accept and amend the spec sentence, or change) |
| Q10 | The `calculationEnvironment` fingerprint in `index.json` goes beyond the spec's integer marker | Keep. It is what makes preference and registration changes discard cached columns of unloaded sessions | Stands |
| Q11 | Changing a Python plugin's code without changing its id does not invalidate cached columns | Documented workaround in the plugin README | Stands |
| Q12 | An orphan CSV whose `SESSION_ID` is already indexed shows as a duplicate row | Leave as is | Stands |
| Q13 | `_DESCRIPTION` for file pairs outside a device folder depends on which file created the session | Leave | Stands |
| Q14 | `DEVICE_ID` placeholder `n/a` treated as absent on merge; legacy mass / area / wind backfill kept as a load-time shim; all conflicts reported, not only the first | As stated | **decide** on the backfill shim (it reads preferences that are not in the fingerprint) |
| Q15 | `allowSourceInputs` lets plugin calculations declare source inputs, against the letter of spec 7.5 ("the conversion layer depends on the source; nothing else does") | Keep. Spec section 8 requires plugin source access, and untracked reads would break invalidation. All three reviewers agreed | **decide**: reword spec 7.5 to "...and plugin calculations that opt in to source access" |
| Q16 | macOS / Linux behaviour of `tst_python_bridge` in CI | Unverified until a push | See 1.2 |
| Q17 | Developer note location | `docs/CALCULATIONS.md`, separate from `DATA_SCHEMA.md` | Done |

Decided on 2026-09-19: `$VAR` values stay unquoted. The format is not RFC
CSV; the value is everything after the second comma, which is unambiguous
because keys cannot contain a comma. Quoting would show literal quote
characters in older Viewer versions and need an escaping rule. (An older Viewer
reading a file written by this version still cuts the value at the first
comma, as it always did.)

**Altitude markers as logbook columns (separate spec, parked).** The Add Column
dialog skips markers that have a group (`src/preferences/addcolumndialog.cpp`
~249), and altitude markers all have the group "altitude", so neither
"measurement at an altitude marker" nor "delta between two altitude markers"
can be created. Unchanged since `v2026.04.1`. Settled in discussion and
written up as its own spec, `PLANS/altitude-time-references.md`: the altitude
crossing becomes one always-available parameterized calculation selected by
attribute name, the marker manager becomes display only, and a column chooses
"at marker or at altitude". Parked until this branch is closed off; not part
of this merge.

Two more things that are not in the plan's list but need your eye:

- **Number text in saved files.** Doubles are written in Qt's shortest
  round-trip form, so a released `0.0001` is re-saved as `1e-04`. The value,
  unit, and effective value are unchanged, and older Viewer versions, Python,
  and Excel parse it. Byte stability starts from the first file this version
  writes. If you dislike the look, the rule lives in one place:
  `CsvFormat::formatDouble` in `src/csvformat.cpp`.
- **`parseDouble(" 1")` succeeds.** The plan's table expected surrounding
  whitespace to fail; `QStringView::toDouble` accepts it, and the implementer
  kept "whatever loads today still loads". Pinned by a test and documented in
  `src/csvformat.h`.

The Phase 8 agent also proposed amendments to the plan documents (the
dependency-graph note in `00-overview.md`, the "do not touch" lists in 05 and
07, 02 Task 2.7, 03 Task 3.10, 04 Task 4.3 criterion 1, 05 Testing
Requirements, 07 Tasks 7.4 and 7.8). None was applied. They only matter if the
plan is to be kept as an accurate record.

---

## 3. Robustness notes (agreed: a separate branch)

None of these blocks the merge. "Pre-existing" means the code was already so
at `v2026.04.1`; line numbers are approximate.

### 3.1 Logbook and model

| Note | Where | New or pre-existing | Suggested fix |
|---|---|---|---|
| **Unreadable session file is a dead end.** A session listed in `index.json` whose file is missing or unreadable shows as a row with every cell blank (nothing to identify or select), blocks re-import of its own tracks ("Existing session ... could not be loaded"), and cannot be recovered without deleting `index.json`. You hit this with the `.fsv` logbook | `src/sessionmodel.cpp` `populateFromIndex` (~752), merge load-failure branch (~640) | New consequence of the spec 6.2 rule (baseline silently replaced the session) | Show the session id and an "unreadable session file" marker in an existing text column so the row can be selected and deleted; make the import error say to delete that session and re-import. Decide whether an index entry with no file should be flagged (recommended) or dropped at startup |
| **Column worker spins** on a stub row whose file fails to load: `hasWork` stays true, so the same file is re-parsed on every idle step and rows after it never fill | `src/sessionmodel.cpp` ~1671-1675 | Pre-existing; the stricter importer makes unreadable files more likely | Record failed ids and skip them |
| A transient load failure can leave **blank values cached** in the index for a good session (`processNextDirtyColumn` / `fillMissingColumns` compute from a failed-load placeholder) | `src/sessionmodel.cpp` | Pre-existing | Skip placeholders when filling columns |
| **Bulk edit on a loaded row emits no `dependencyChanged`**, so MomentModel, the WS-P / SP widgets, and VideoWidget can go stale after a bulk `_GROUND_ELEV`, mass, or area edit (plots recover via `modelChanged`) | `src/sessionmodel.cpp` ~1788 | Pre-existing | One line: `publishInvalidation(item.row, keys)` |
| On a session id remap (identity stub -> real `SESSION_ID`), `m_loadQueue`, `m_lruList`, and the focused / hovered ids still hold the old id. The bulk-edit queue is now rewritten on a remap; these are not | `src/sessionmodel.cpp` `setRowSessionId` | Pre-existing | `setRowSessionId` is now the single place every remap goes through, so update the other id holders there |
| After a failed bulk-edit save on an unloaded session, the promoted row is dropped from the load batch, so **no `visibilityChanged`** is emitted for it if it was a visible stub still in the load queue | `src/sessionmodel.cpp` ~1859-1865 (comment in code) | New (fixup round), rare | Add the row to `m_loadedDuringBatch` when promoting |
| **Unsaved column marks can go stale** on a long-lived `saveFailed` row: they are computed against the column-dependency map at edit time, and `checkCalculationEnvironment` does not re-mark dirty rows the way `rebuildColumns` does | `src/sessionmodel.cpp` | Pre-existing window, lengthened by the failed-save fix | Re-mark dirty rows on an environment change |
| First launch after upgrade with a **legacy flat index**: a session file the index does not list stays hidden until the second launch (orphan adoption only runs for the extended-format index) | `src/logbookmanager.cpp` `initialize` | New (Phase 6 startup change) | Run orphan adoption in the legacy branch too |
| An identity stub hidden behind a failed-load placeholder is skipped by `resolveIdentityStubs`, so importing that session creates a **duplicate row** (the old file is never overwritten) | `src/sessionmodel.cpp` ~736-752 | New (Phase 6), rare | Resolve stubs regardless of placeholder state |
| A file with `$VAR,SESSION_ID,` (empty value) gets a synthesized match id that is never stored, giving an **unsavable session** | `src/dataimporter.cpp` `applyCreationDefaults` | Pre-existing behaviour | Treat an empty recorded id as absent |
| Any environment change (one altitude marker, one plugin, the descent-pause preference) **discards every cached column in the whole logbook**, so the idle worker reparses every session | `src/sessionmodel.cpp` ~1256-1280 | New (Phase 5); correct but coarse | Restrict the discard to columns whose static dependencies touch the change |
| **Bulk edit and column fill block the UI.** Each step on an unloaded session parses and rewrites a whole session file on the UI thread (scheduler unchanged from baseline). Not measured: whether the new parser / round-trip number writer make each step slower than the released build | `src/sessionmodel.cpp` `processNextBulkEdit`, `processNextDirtyColumn`; `src/dataimporter.cpp`, `src/csvformat.cpp` | Pre-existing design; possible new slowdown | Time load and save of one large session against `v2026.04.1`; the row parser allocates a vector per row (section 3.5) |
| Two log lines per failed save (one from `LogbookManager`, one from `SessionModel`); save failures are never shown to the user | both files | Pre-existing | Optional: surface the error |

### 3.2 Calculations (all pre-existing arithmetic, migrated unchanged)

| Note | Where | Suggested fix |
|---|---|---|
| Single-sample track: `std::clamp` with lo > hi is undefined behaviour, followed by an out-of-bounds read | `src/calculations/gnsscalculations.cpp` ~314 (baseline :241) | Guard `time.size() < 2` |
| Exit time: equal consecutive `velD` values of exactly 10.0 give `a` = NaN, and NaN passes every guard. `velD` has 0.01 m/s resolution, so this is realistic | `src/calculations/attributecalculations.cpp` ~261 | `if (!(a >= 0 && a <= 1)) continue;` |
| Division by `t1 - t0` without a guard | `src/calculations/attributecalculations.cpp` ~596 | Guard zero |
| Track simplification silently drops every point after a miss | `src/calculations/simplificationcalculations.cpp` ~67-85 | Continue past the miss |
| `canConvert<double>()` is true for any string, so a non-numeric attribute becomes 0.0 | about 20 sites in `src/calculations/` | One shared `numericAttribute(ctx, key)` helper using `toDouble(&ok)` |
| Non-const locals holding `ctx.measurement(...)` deep-copy the vector on first non-const access; worst case copies 8 vectors for lift and again for drag | `gnsscalculations.cpp` ~83-90; also `imucalculations.cpp`, `magcalculations.cpp`, `attributecalculations.cpp`, `wspcalculations.cpp`, `altitudemarkerfeature.cpp` | Make those locals `const` |
| A few unreachable `canConvert` checks remain for top / bottom / exit | `src/calculations/wspcalculations.cpp` ~268-278 | Remove (harmless) |

### 3.3 Engine and plugins

| Note | Where | New or pre-existing | Suggested fix |
|---|---|---|---|
| A plugin can supply `SCHEMA_VER` as a calculated attribute and so choose the schema for unmarked files, against "only `SCHEMA_VER` decides" in spirit | `src/conversion/sourceconversion.cpp` ~90 (declares it as an attribute input) | New | Reject `SCHEMA_VER` as a plugin output in `src/pluginadapters.cpp`, or add a stored-only attribute input kind |
| Overlapping dependency cycles: read-order-dependent answers (see Q4) | `src/engine/calculationengine.cpp` | New | Optional: report cycles found at registration time via `staticDependencies`; add an overlapping-ring case to `tst_calcengine_safety` to pin the behaviour |
| In `PluginHost::initialise()`, `sdk.attr("_simple_plots")`, `sdk.attr("_markers")`, and two `py::len` calls sit outside any try block; the summary counts skipped plot / marker definitions | `src/pluginhost.cpp` | Pre-existing, contrived | Wrap like the registration sections |
| `getAttribute` from Python returns bools as 0.0/1.0, non-numeric variants as None, and all-digit ids as lossy floats; the `has*` methods raise for an undeclared key, so they cannot be used as probes | `src/sessiondata_bindings.cpp` ~57-75 | New | Document, or return strings for ids |

### 3.4 Session references in the UI (what is left)

The held-pointer sites were fixed in `f9dd38d`; every session read in
`updatePlot`, the external cursor update, the traced-sessions loop,
`LegendPresenter`, `measuretool`, and the reference-session lookup now happens
under a `SessionModel::RowStabilityGuard` that ends before anything that can
emit or mutate. `SessionModel::loadedSession(id)` (guard required, asserted)
and `forEachLoadedSession` are the tools for any new code. What remains is
lower risk:

- **Unguarded single-iteration reads.** `src/ui/docks/plot/PlotWidget.cpp` ~1997 (the Attribute branch of `updateMomentVLines`, whose sibling branch is now guarded), ~831, and ~1315 hold `const SessionData &s = sr.session.value()` for one loop iteration with no guard. Read-only and used immediately, so no bug, but inconsistent with the guarded sites. Same class: `SpeedSkydivingWidget.cpp` ~153-275, `WingsuitPerformanceWidget.cpp` ~189-326, `MapCursorDotModel.cpp` ~309-313, `TrackMapModel.cpp` ~162-166, `VideoWidget.cpp` ~664-668, `mainwindow.cpp` ~605, ~755. Fix: wrap each in a guard for consistency.
- **Hidden load / evict side effect of a read.** `PlotWidget.cpp` ~411, ~1271, ~1636, `src/ui/docks/video/VideoWidget.cpp` ~820, and `src/plottool/setgroundtool.cpp` ~83 call `sessionRef` on a possibly unloaded row and use the result immediately. The reference itself is fine; the issue is that a read can load one session and evict another. Fix: make the load an explicit step, then read with `loadedSession`.
- **Behaviour note from `f9dd38d`.** A traced session that was hidden and evicted before the 0 ms plot rebuild is now skipped for that one mouse event instead of being loaded from disk inside the mouse move. It self-corrects on the rebuild. Covered by manual check M21.
- **Assumption.** The external cursor update now resolves a session id with `getSessionRow` (first match); the old pointer hash let the last row win on duplicate ids. Session ids are unique everywhere else in the code, so this only matters together with the duplicate-row notes in 3.1.

### 3.5 Tests, docs, and build (nice to have)

- Test gaps the requirements review noted: the acceptance 7 conflict test makes no before/after compare of attributes and source data for the loaded case; the fake-state oracle has no merge operation and never calls `request()`; the "released" `index.json` fixture for acceptance 18 is a new-format index with two keys deleted, not a file captured from v2026.04.1; no automated plot test exists.
- `QVERIFY` inside void helpers returns only from the helper, so one failure cascades (e.g. `tests/tst_column_cache.cpp` ~311).
- The oracle test keeps per-seed temp directories until process exit.
- `src/profilemanager.cpp:39` writes under `DocumentsLocation`, which Qt's test mode does not redirect. No test touches it today.
- `registerCorePreferences` in `tests/support/testenvironment.cpp` hand-mirrors MainWindow's defaults and will drift.
- `DataImporter::findFlySightRoot` climbs parent directories looking for `FLYSIGHT.TXT`, so one FS1 fixture in `tst_importer::neverStampsSchema` can read outside its temp directory (read-only). Add `DEVICE_ID` to that fixture.
- Audit patterns use `\b` in `git grep -E`, which a macOS git built on the system regex library may treat literally (weakens three patterns; cannot cause a false failure).
- `python_plugins/README.md`: names `FlySightViewer.app` (the bundle is `FlySight Viewer.app`); gives a Linux plugin path inside the read-only AppImage (tell users to set `FLYSIGHT_PLUGINS`); lists "a duplicate id" as a rejection cause, which cannot occur; omits that adding or reordering plugin files shifts ids and drops the cache. Its links point at `blob/master/docs/...`, which shows the superseded document until this branch reaches `master`.
- `docs/DATA_SCHEMA.md` ~235: says errors appear "in the import dialog"; it is a single warning box after the batch. Sections 9-10 should say that number text written by released versions may be re-spelled on the next save.
- Duplication in the migrated calculations: the vector-magnitude body appears five times; `registerWspDefault` and `registerSpDefault` are the same function; the "first 10 m/s crossing after exit" logic is implemented twice; g = 9.80665 is defined twice in `gnsscalculations.cpp`. `registerGnssCalculations` and `registerAttributeCalculations` are each about 420 lines.
- `sessionimport.cpp:84` wraps `QCoreApplication::translate` in a lambda on non-literal text, so `lupdate` will not extract those strings (only matters if translations are planned).
- `LogbookManager::hasIndexData()` has no production callers; four test files use it.
- Performance: `importDataRow` heap-allocates a `QVector<double>` per row (`src/dataimporter.cpp` ~487).

---

## 4. Audit trail

```
git log --oneline v2026.04.1..HEAD                 # one line per phase, plus fixups
git show --stat plan/phase-3-done                  # a phase as accepted
git log --oneline --grep='fixup' v2026.04.1..HEAD  # everything that reopened a closed phase
```
