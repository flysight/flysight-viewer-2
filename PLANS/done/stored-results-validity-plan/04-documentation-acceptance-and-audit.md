# Phase 4: Documentation, acceptance map and audit

## Overview

This phase brings every document, test catalogue and stale code comment in line
with "Stored results: validity that mirrors memory" as Phases 1-3 implement it,
and adds the machine-checked evidence for that specification: a new acceptance
range, 401-442, one item per clause, plus in-place amendments of the four 3xx
items the new specification supersedes. A new audit rule group,
`result-validity`, keeps the new boundaries in place (no environment
fingerprint in a record, one place for the plug-in code identity, teardown
removals only at shutdown, plug-ins loaded once) and keeps the documents from
saying again that unrelated changes make stored results stale. Nothing in this
phase changes behaviour; the only source edits are comments found by the sweep
of Task 4.5.

## Dependencies

- **Depends on:** Phases 1, 2 and 3 (documented; implemented and committed
  before this phase starts). This document uses the names those phase
  documents fix: test targets, test functions, classes, methods, status texts.
  If a committed phase deviated from its document, follow the code as
  committed, point the map line at the committed name only if that function
  asserts the same clause, and report the difference. Never delete a map line
  to make the audit pass.
- **Blocks:** None.
- **Assumptions:**
  - Phase 1 committed: `StoredResolution` / `StoredCalculationResult::resolutions`,
    `RestoreOutcome::StaleCheck::Resolutions` (order ResultVersion, Bundle,
    InputsUnavailable, Resolutions, Leaves, Fingerprint),
    `CalculationRegistry::Removal {Change, Teardown}` and
    `RegistryChange::teardown`, runtime registry drops delivered as
    `DroppedByInputChange`, the altitude-marker manager's destructor
    unregistering with `Removal::Teardown`; tests `resolutionCodes`,
    `exportResolutions`, `ringIsNeverStored`, `restoreAcrossRegistries`,
    `droppedByRegistryChange`, `registryDestructionReportsNothing`
    (`tst_calcengine_restore`), `altitudeMarkerTeardownReportsNothing`
    (`tst_builtins_engine`).
  - Phase 2 committed: `src/plugincodeidentity.{h,cpp}` in `flysight_core`
    (`pluginCodeIdentity()`, `readPluginCodeFiles()`, `readWholeFile()`,
    prefix `plugins-sha256:`, token `none`), `PluginHost::codeIdentity()`, the
    log line `[PluginHost] Plug-in code identity: ... (<n> files)`, the
    environment fingerprint's `#<id>#<version>` lines; the new executable
    `tst_plugin_identity` (label `core` only) with `identityIsDeterministic`,
    `encodingIsPinned`, `eachIngredientChangesIdentity`,
    `absentVersionUsesFixedToken`, `readsTheFolderRecursively`,
    `subfolderFileChangesIdentity`, `pycacheAndHiddenDirectoriesAreIgnored`,
    `unreadableFileIsNotEmpty`; `tst_builtins_engine::fingerprintCoversResultVersions`,
    `tst_python_bridge::pluginRegistrationsCarryCodeIdentity`,
    `tst_column_cache::pluginEditDiscardsCachedValues`.
  - Phase 3 committed: record format version 2 without `calculationEnvironment`,
    `CalculationRecord::stampsAreCurrent()` comparing the compatibility marker
    only, `Stats::recordsSkipped`, the warning text "skipped (kept for the next
    load)", `LogbookManager::markCalculationRecordSkipped()`,
    `markCalculationRecordsUnconfirmed()` removed; support helpers
    `UnreadableFile` and `asFormatOne()` (`tests/support/logbookprobe.h`),
    `ExtraRegistrations` (`tests/support/jobfixture.h`),
    `fixtureSessionWithSAccStoredAs()` (`tests/fusion/fusionsessions.h`); tests
    `formatOneIsRefused` (`tst_result_records`),
    `formatOneRecordIsDeletedOnLoad`, `unreadableRecordIsSkipped`,
    `dependentOfSkippedRecordIsKept`, `registryChangeDeletesRecord`,
    `recordSurvivesUnrelatedChanges`, `lookupResolvingDifferentlyDeletesRecord`,
    `pluginEditStalesRecordsThatReadIt` (`tst_result_store`),
    `registryChangeKeepsLoadedRowConfirmed` (renamed from
    `environmentChangeUnconfirmsLoadedRows`; Phase 3 already changed map line
    324) and `skippedRecordValuesStayOutOfIndex` (`tst_result_columns`),
    `storedFitSurvivesUnrelatedChanges`, `runtimeRegistrationDropsFitAndRecord`,
    `lookupResolvingDifferentlyAtLoadDeletesFit` (`tst_fusion_store`); the
    `builtincalculations.h` bump paragraph rewritten to begin
    "Bump it, or the result version of the calculation concerned".
  - `PLANS/` is untracked. Nothing written in this phase links to it or names
    a file in it. Specifications are cited by title: "Storing requested
    calculation results with the session" (items 301-350) and "Stored results:
    validity that mirrors memory" (items 401-442).
  - Build and test only in `build-phase1/`. **Never build `build/`.**

## Tasks

### Task 4.1: `docs/DATA_SCHEMA.md` sections 11 and 12

**Purpose:** The data schema must state the new validity (spec 4), the record's
resolutions and format version 2 (spec 8), unreadable records (spec 7), the
column cache over skipped records and the environment fingerprint covering
result versions (spec 6, 7), and the plug-in code identity; and it must stop
saying that unrelated changes make stored results stale (spec 11).

**Files to modify:**
- `docs/DATA_SCHEMA.md`

**Technical Approach:**

1. **§11, the `calculationEnvironment` bullet** (lines ~307-308). Replace with
   this meaning:
   - `calculationEnvironment`, a fingerprint of the registered calculation ids
     in the order candidates are tried, the result version each registration
     declares, and the values of the preferences that calculations declare as
     inputs. Every Python plugin registration declares the plug-in code
     identity (section 12) as its result version, so editing, adding or
     removing a plugin file, or upgrading the Python or NumPy the plugins run
     on, changes the fingerprint; so does an update that changes the sensor
     fusion algorithm.
2. **§11, after "If either is missing or different, all cached values are
   discarded ... untouched."** (before "This happened once on the upgrade"),
   add one sentence: such a change discards cached column values only; it
   never makes a stored calculation result stale (section 12, "Validity").
3. **§11, the paragraph at ~343-351** ("Before a record is written ... An index
   written before stamps existed ..."): replace the parenthesis "(a record
   write or removal that failed, or a change of the registered calculations or
   of a declared preference while the session was loaded)" with "(a record
   write or removal that failed, or a record that could not be read when the
   session was loaded, section 12)". After "... or the session is unloaded."
   add: an unloaded session whose record was skipped shows such a column empty
   (pending) until it is loaded again, when the record is read again. The rest
   of the paragraph stays.
4. **§12 "Format."** (~393-409). Change the first sentence to: the magic
   `FVRESULT`, a format version (2), then the code stamp
   (`CalculationCompatibilityVersion`), the calculation id, the result
   version, the reason, the input fingerprint (SHA-256), the names of the
   inputs the result depended on, the outputs, and what provided each name the
   result looked up (its resolutions, see "Validity"). Keep the attribute-type
   and checksum sentences. Replace "A record of another format version, or a
   damaged one, is treated as stale." with: a record of another format
   version, or a damaged one, is treated as stale. Records written by earlier
   versions of FlySight Viewer have format version 1; they are deleted at
   their session's next load, and the calculation has to be requested again.
   There is no migration.
5. **§12 "Validity."** (~411-429). Replace the whole part with this content
   (the wording may be smoothed; every statement must be there):
   - A record is used only while all of these hold:
     - **Code.** `CalculationCompatibilityVersion` (section 11) and the
       calculation's result version equal the ones it was written with. For
       sensor fusion the result version is the kernel's algorithm string, the
       `"algorithm"` of its diagnostics.
     - **Inputs.** The names of the inputs the result reached are the same
       (source measurements with their unit text, attributes and declared
       preferences, directly or through other calculations, including inputs
       that were looked at and found absent), and a fingerprint over their
       current values equals the stored one.
     - **Lookups.** For every name the result looked up, directly or through
       other calculations, the record states what provided it: a calculation
       (its id and its result version), the session's own data (a stored
       attribute or recorded data), or nothing. When the session is loaded the
       same lookups are repeated against the calculations registered now, and
       every answer must be the same.
   - So an edit the result does not depend on (a marker, a description) keeps
     the record, and an edit it depends on (a merge that adds or changes IMU
     data, a changed `SCHEMA_VER`, a changed local origin) makes it stale. What
     else is registered, and preferences the result did not read, never
     matter: adding or removing an altitude marker, loading a set of plugins
     whose calculations the result never looked up, or changing the descent
     pause timeout keeps every stored sensor fusion result valid. A new
     calculation that would now provide a name the result looked up (a plugin
     that declares one of its inputs, say) makes it stale. In short, a record
     goes stale when the same result in memory would have been dropped, or
     when the code that computed it changed.
   - A new paragraph **Plug-in code identity.** Each Python plugin registration
     (attribute, measurement or calculation) declares one result version, the
     plug-in code identity: a SHA-256 digest, written `plugins-sha256:<hex>`,
     over every `*.py` file under the plugin folder, subfolders included (its
     path relative to the folder and its bytes; files in `__pycache__` and in
     hidden folders, whose names start with `.`, are left out), the plugin SDK
     file, and the Python and NumPy versions (`none` for a version that cannot
     be read). It is computed once, when the plugins are loaded at start-up,
     and written to the log. Editing, adding or removing any such file (also
     under `examples/`, which is never imported) or upgrading Python or NumPy
     changes it for every plugin registration at once, so a stored result whose
     lookups went through any plugin calculation is stale at its session's next
     load, and the cached column values are discarded at start-up (section 11).
     A stored result whose lookups touched no plugin calculation is unaffected.
     Plugin results themselves are never stored.
6. **§12 "Lifecycle."** (~431-447):
   - Bullet 2 ("It is deleted when ..."): add, after "when an input it depends
     on changes,": "when a change of the registered calculations made while
     the application runs drops its result (a calculation registered that
     provides a name it looked up, for example),".
   - Bullet 3: replace with "It is never deleted by hiding a track, unloading a
     session, quitting, or a change of the registered calculations that does
     not reach it." Delete "Such a change is checked at the next load
     instead."
   - New bullet after bullet 4: A record that exists but cannot be opened or
     read in full when its session is loaded (another program holding the file
     locked, for example) is skipped for that load: it is neither restored nor
     deleted, the calculation reads as not requested, and a warning is logged.
     A record whose result reads the result of a skipped record is skipped too.
     The next load tries again; a new publish for the same session and
     calculation replaces the record, and deleting the session, or the start-up
     pass for a session file that no longer exists, removes it. Logbook column
     values that depend on a skipped record are not cached in `index.json` while
     it stays skipped (section 11). A record that was read but is not a record,
     is damaged, or has another format version is deleted as stale.
7. "Guarantees." stays.
8. Do not renumber sections; do not add any link to `PLANS/`.

**Acceptance Criteria:**
- [ ] `git grep -n -e "makes every record stale" -e "Such a change is checked at the next load" -e "a change of the registered calculations or of a declared" -- docs/DATA_SCHEMA.md` prints nothing.
- [ ] §11's `calculationEnvironment` bullet names the result versions and the plug-in code identity; §11 says an environment change never makes a stored result stale; the unconfirmed paragraph names skipped records.
- [ ] §12 contains "format version (2)", the three validity conditions (code, inputs, lookups), a "Plug-in code identity." paragraph naming subfolders, `__pycache__`, hidden folders, `examples/`, the SDK, the Python and NumPy versions and `none`, and the skipped-record lifecycle bullet.
- [ ] `audit_cleanup` passes (including the `result-validity` rules of Task 4.7).

**Complexity:** M

---

### Task 4.2: `docs/CALCULATIONS.md` sections 5, 7, 8, 9, 12, 15.8 and 17

**Purpose:** The developer note must describe the resolutions and the new stale
check, runtime registry drops and teardown, the new bump rule, the store's
treatment of unreadable records, and the plug-in code identity as a result
version (spec 11: sections 9, 12, 15 and 17; plus 5, 7 and 8, which state the
same facts).

**Files to modify:**
- `docs/CALCULATIONS.md`

**Technical Approach:**

1. **§5 Candidates and order.** Append a paragraph: what each name resolved to
   also matters beyond the session. A stored result (15.8) records, for every
   name it looked up, what provided it (a calculation instance with its result
   version, the session's own data, or nothing), and a restore repeats those
   lookups against the current registry (section 12, "Export and restore"). A
   registration that changes which candidate wins for such a name therefore
   makes the stored result stale; a candidate registered after the winner, or
   one for names the result never looked up, does not.
2. **§7 Preferences**, last sentence ("Changing *which calculations exist*
   ... which invalidates every loaded session."): replace "which invalidates
   every loaded session" with: "which, in every loaded session, invalidates
   what depended on the names concerned. A requested result that never looked
   those names up stays installed, and so does its stored copy (15.8); one
   that did is dropped and its record deleted. `AltitudeMarkerManager`'s
   destructor unregisters with `CalculationRegistry::Removal::Teardown`, which
   reports nothing (section 12)."
3. **§8 Explicit policy**, the sentences at ~161-165 ("A descriptor may
   declare ... The engine never interprets it and it is not part of the
   environment fingerprint; a stored result is used only while it is
   unchanged (section 9). `builtin.fusion.fit` declares ..."): keep the first
   sentence; replace the rest with: the engine never interprets it. It is part
   of the calculation environment fingerprint of the logbook column cache
   (section 9), and a stored result records it for its own calculation and for
   every calculation its lookups went through (section 12, "Export and
   restore"); a stored result is used only while those are unchanged (section
   9). `builtin.fusion.fit` declares its kernel's `Fusion::Algorithm` (section
   17), and every Python plugin registration declares the plug-in code
   identity (`src/plugincodeidentity.h`; the plugin README, section 7). Keep
   the sentence about explicit family instances and the rest of the paragraph.
4. **§9 When to bump.** Replace the paragraph at ~198-205 and the paragraph at
   ~207-211 with the following. The first line of the first paragraph must
   hold the whole phrase `Bump it, or the result version of the calculation concerned`
   (the audit counts that line, Task 4.7):

   > Bump it, or the result version of the calculation concerned
   > (`CalculationDescriptor::resultVersion`, section 8), whenever a change can
   > alter what a requested calculation, or anything it reads, produces. A
   > stored result is used only while this marker and the result version it
   > was stored with equal the current ones, and every name it looked up still
   > resolves to the same provider with the same result version (15.8).
   > Bumping a result version drops the stored results of that calculation and
   > of every requested calculation whose lookups went through it; bumping the
   > marker drops every stored result and every cached column value.
   >
   > Do not bump it for added, removed, or renamed registrations, or for a
   > changed result version: the environment fingerprint
   > (`calculationEnvironmentFingerprint`) covers those for the logbook column
   > cache. It covers every registration's result version too, so a plugin
   > edit discards cached column values at the next start. A stored result
   > does not depend on the fingerprint: a registration makes it stale only by
   > changing what a name it looked up resolves to. Never reuse a value, and
   > never use 0.

   The markdown source may wrap the quoted text differently, except for the
   first line.
5. **§12 Asynchronous request, "Export and restore."** (~375-432):
   - `exportResult(id)`: "or nothing unless a plain explicit calculation (not a
     family) has an installed result with status `Ok`" gains "whose evaluation
     met no dependency ring (a ring is a registration error; what provided a
     name then cannot be stated)".
   - Members: after `leaves`, add `resolutions`: for every name the result
     looked up, reached by the same walk as the leaves (rejected candidates and
     explicit results included), a `StoredResolution {name, provider,
     instanceId, resultVersion}`; provider `Calculation` (the instance id,
     `<familyId>#<key>` for a family instance, and that registration's result
     version), `SessionData` (a stored attribute, or source data read through
     the passthrough) or `Nothing`. Sorted by `storedResolutionLess()`, one
     entry per name. A source conversion is a `Calculation`, so registering
     the first source conversion changes every measurement that has source
     data. A preference is a leaf, not a resolution.
   - "The code stamps are not part of the snapshot (...); the record adds them."
     becomes "The code stamp (`CalculationCompatibilityVersion`) is not part
     of the snapshot (the engine does not depend on `src/calculations/`); the
     record adds it."
   - Stale checks, in order: `ResultVersion`, `Bundle` (...), `InputsUnavailable`,
     `Resolutions` (the gathering met a ring, or a looked-up name resolved
     differently: another provider, another instance or result version, or a
     name looked up in only one of the two), `Leaves`, `Fingerprint`. Add: a
     change of the session or the registry that alters a lookup reports
     `Resolutions`, also when the leaves differ as well; with the same
     resolutions the gathering takes the same paths, so `Leaves` is in
     practice a check of the snapshot itself.
   - Listener, the `DroppedByInputChange` bullet: add "and a registry change
     made while the application runs that reaches the result (a registration,
     or a removal with `CalculationRegistry::Removal::Change`, the default of
     `unregister()`)". The "neither is reported for" bullet becomes:
     `restoreResult()`'s own install, `clear()`, a removal with
     `Removal::Teardown` (an owner being destroyed at shutdown), or the
     destruction of the registry or the engine.
6. **§15.8 Stored results** (~796-868):
   - `Installed` with status `Ok`: "`CalculationRecord::stamped()` (which adds
     `CalculationCompatibilityVersion` and the environment fingerprint,
     computed fresh)" becomes "`CalculationRecord::stamped()` (which adds
     `CalculationCompatibilityVersion`)".
   - `DroppedByInputChange` bullet: "(an input change, or a registry change
     made while the application runs, dropped the result)".
   - The known-records bullet: replace "Otherwise the listing of the session's
     record files is the source of ids." with "Otherwise the ids read are the
     listing of the session's record files together with the ids the manager
     knows, so that something other than a file standing at a known record's
     path (a directory) is read, and found unreadable, rather than ignored."
   - Replace the bullet "These are deleted at a load: unreadable records, ..."
     with two bullets:
     - These are deleted at a load: a file that is not a record, a damaged
       record, a record of another format version (format 1, written by
       earlier versions, included: no migration), a record whose stamp is not
       current (`CalculationRecord::stampsAreCurrent()`: the compatibility
       marker only), a record failing a stale check (`Resolutions` included),
       and a record of a calculation that is not registered as explicit. A
       record whose result is already installed (`AlreadyInstalled`) is kept.
     - A record that exists but cannot be opened or read in full
       (`CalculationRecordStatus::Unreadable`) is skipped: neither restored nor
       deleted, counted in `recordsSkipped`, warned once ("skipped (kept for
       the next load)"), and marked with
       `LogbookManager::markCalculationRecordSkipped()`, which keeps the column
       values over it out of `index.json` until the record is written or
       removed or the row is evicted. A record that stays `InputsUnavailable`
       only because it reads the result of a skipped record is skipped too.
       The next load tries again.
   - The deletions bullet ("Records are deleted with their session ... Eviction,
     unloading, a registry change and the model's destruction never delete
     one."): replace the last sentence with "Eviction, unloading, a registry
     change that does not reach a requested result, a removal as teardown and
     the model's destruction never delete one. A registry change made while
     the application runs that drops a requested result deletes its record
     like an input change. `AltitudeMarkerManager`'s destructor removes its
     registrations as teardown; the plugin host never unregisters."
   - Test seam: "(records written, restore calls and listings, records read,
     restored, kept, skipped and deleted, and the time spent)".
   - Tests line: add `tests/tst_plugin_identity.cpp` (the plug-in code
     identity) and `tests/tst_result_columns.cpp`.
7. **§17 Sensor fusion, "Stored results."** (~1320-1328): after "Markers and
   preferences are not among them." add: its resolutions name what provided
   each name the fit looked up (the conversion layer's instances for recorded
   measurements, the calculations behind derived channels such as the local
   frame and the time fit, the session's own attributes). With the built-ins,
   no altitude marker, no preference and no plugin calculation is among them,
   so adding or removing an altitude marker, changing the descent pause
   timeout, or editing a plugin keeps a stored fit; a plugin or a registration
   that declares one of the names the fit looks up, ahead of the built-in,
   makes it stale.
8. **§17, "Logbook columns."** (~1330-1353): "Unconfirmed records (a failed
   write or removal, an environment change while loaded)" becomes "Unconfirmed
   records (a failed write or removal, a record skipped at the load because it
   could not be read)". After that sentence add: an environment change (a
   registration, a declared preference, a changed result version such as a
   plugin edit) discards every cached value but leaves the records valid;
   loaded rows recompute from the engine and stubs with a record stay pending
   until loaded.
9. **§17 Tests paragraph:** in the `tests/tst_fusion_store.cpp` parenthesis add
   "kept across altitude markers, unrelated registrations, the descent pause
   and another plugin set; dropped at once by a registration that provides a
   name it looked up; deleted when a lookup resolves differently at load".

**Acceptance Criteria:**
- [ ] `git grep -c "Bump it, or the result version of the calculation concerned" -- docs/CALCULATIONS.md` prints 1, and that line is in §9.
- [ ] `git grep -n -e "Bump it, or the calculation's result version" -e "makes every stored result stale" -e "not part of the environment fingerprint; a stored" -e "and the environment fingerprint, computed fresh" -e "an environment change while loaded" -e "registry change and the model's destruction never delete" -- docs/CALCULATIONS.md` prints nothing.
- [ ] §12 names `resolutions`, `StoredResolution`, the provider words `Calculation` / `SessionData` / `Nothing`, the stale check `Resolutions` in its place in the order, and `Removal::Teardown`.
- [ ] §15.8 names `recordsSkipped`, `markCalculationRecordSkipped`, "skipped (kept for the next load)" and format 1.
- [ ] §5, §7, §8 and §17 contain the statements of steps 1-3 and 7-9.

**Complexity:** M

---

### Task 4.3: `python_plugins/README.md`

**Purpose:** Plugin authors must learn what the plug-in code identity covers and
that editing a plugin stales stored results that used it and discards cached
column values (spec 6, 11); the "Cached logbook columns" advice to rename a
plugin is now wrong.

**Files to modify:**
- `python_plugins/README.md`

**Technical Approach:**

1. **§1**, the sentence "Subfolders are not scanned, so nothing in `examples/`
   is loaded: to try an example, copy it one level up and restart." becomes:
   "Subfolders are not imported, so nothing in `examples/` is loaded: to try
   an example, copy it one level up and restart. (Every `*.py` file in the
   folder and its subfolders still counts for the plug-in code identity,
   section 7.)"
2. **§7, replace the subsection "### Cached logbook columns"** (lines ~199-206)
   by "### Plug-in code identity: stored results and cached columns" with this
   content:
   - At startup, before any plugin is imported, FlySight Viewer computes one
     digest, the *plug-in code identity*, over: every `*.py` file in the plugin
     folder and its subfolders (its path relative to the folder and its
     bytes), except files in `__pycache__` and in hidden folders (names that
     start with `.`); the SDK file; the Python version and the NumPy version
     (`none` if one cannot be read). `examples/` counts although nothing in it
     is imported. The log shows it:
     `[PluginHost] Plug-in code identity: plugins-sha256:... (3 files)`.
   - Every attribute, measurement and calculation a plugin registers declares
     it as its result version. So editing, adding, removing or renaming any
     file in the folder (not only the plugin you changed), or upgrading Python
     or NumPy, changes it for all plugins at once.
   - The logbook column values cached for sessions that are not loaded are
     then discarded at the next start and recomputed in the background: a
     column over a plugin value never keeps showing what old code returned. No
     renaming is needed.
   - Plugin results are never stored: they are recomputed when read. But a
     stored result of a requested calculation (sensor fusion today) whose
     inputs were looked up through any plugin calculation (a plugin that
     provides a name the calculation reads, section 8) is stale at its
     session's next load after such a change, and must be requested again. A
     stored result that looked up no plugin output is not affected.
   - Plugins are loaded once, at startup. An edit takes effect at the next
     start; nothing watches the files.
3. **§8 Precedence**, append: a plugin that declares a name a requested
   calculation looks up also changes what that name resolves to; adding or
   removing such a plugin makes the stored results that looked the name up
   stale at their next load (they are requested again from the plot list).
4. Spelling: the document says "plugin"; the term "plug-in code identity" is
   kept hyphenated because it is the name in the log line and in
   `src/plugincodeidentity.h`.

**Acceptance Criteria:**
- [ ] `git grep -n -e "not when a plugin's code" -e "rename the plugin" -e "Subfolders are not scanned" -- python_plugins/README.md` prints nothing.
- [ ] §7 names `__pycache__`, hidden folders, `examples/`, the SDK, the Python and NumPy versions, `plugins-sha256:`, stored results going stale and cached column values discarded; §1 and §8 contain the statements of steps 1 and 3.
- [ ] The HTML comments that name exercising tests (`<!-- exercised by ... -->`) are untouched.

**Complexity:** S

---

### Task 4.4: `docs/SENSOR_FUSION.md`, `docs/COMPUTED_PLOTS.md`, root `README.md`

**Purpose:** The user-facing documents must stop saying that unrelated settings
changes make stored results stale (spec 11) and describe what does.

**Files to modify:**
- `docs/SENSOR_FUSION.md`
- `docs/COMPUTED_PLOTS.md`
- `README.md` (root)

**Technical Approach:**

1. **SENSOR_FUSION §2**, the bullet at ~50-54 ("A stored result is dropped when
   ..."). Replace with this meaning: a stored result is dropped when an input
   of the fit changes (a re-import or merge of different data, a changed
   `SCHEMA_VER`, a changed local origin) and after an update that changes the
   fit's arithmetic; the plot then shows the refresh icon again. Adding or
   removing altitude markers, changing preferences and installing or editing
   Python plugins keep it, unless a plugin provides one of the fit's inputs.
   Nothing is recomputed on its own.
2. **SENSOR_FUSION §7 "Stored results."** (~380-387): after "Its code stamp is
   the algorithm string of the diagnostics (`batch-temperature-bias-v3`): ..."
   add: the record also states what provided each input the fit looked up,
   and a load repeats those lookups, so only a change of what the fit reads,
   or of the code that computes it, drops a stored fit. At the end add: a
   stored fit whose file cannot be read when its recording is loaded (another
   program holding it, say) is kept: the recording reads as not fitted until
   it is loaded again.
3. **SENSOR_FUSION §8 table:**
   - `tst_fusion_session` row: "a fit exported and restored into another
     session is indistinguishable" gains ", with what provided each name it
     looked up".
   - `tst_fusion_store` row: add "kept across altitude-marker, registration,
     descent-pause and plugin-set changes, in memory and after a restart;
     dropped at once, with its record, by a registration that provides a name
     it looked up; deleted when a lookup resolves differently at load".
4. **COMPUTED_PLOTS §6**, replace the bullet "**After an update of FlySight
   Viewer** that changes how such a plot is computed, or after a change to
   calculations or to preferences that calculations read (for example editing
   the altitude markers), kept results are discarded when their track is next
   shown. ..." by two bullets:
   - **After an update of FlySight Viewer** that changes how such a plot is
     computed, kept results are discarded when their track is next shown. The
     track is "not computed" again.
   - **Changing settings the computation does not read** (altitude markers,
     other preferences, Python plugins it does not use) never discards a kept
     result.
   and add a third bullet after the `cache/` bullet: **If a kept result cannot
   be read** when its track is shown (another program has its file open, for
   example), the track is "not computed" for that time only: the file is kept,
   and the result comes back the next time the track is loaded.
5. **Root README**, the source tree (~380-381): add a line for the new files
   before `pluginhost.*`, in the style of the neighbours:
   `│   ├── plugincodeidentity.*               # Plug-in code identity: one digest over the plugin`
   `│   │                                      #   folder, the SDK and the Python / NumPy versions`
   (the `│   │` continuation pattern of the lines above it). Nothing else in
   the root README changes.
6. **Words to avoid** (existing audit rules): in `docs/SENSOR_FUSION.md`
   `frozen`, `stationary window`, `candidate window`, `coarse initializer`,
   `bias shifts below`, `zero bias shift`, `twenty-one`,
   `sensor-fusion-clean-port` (group `fusion-model`); anywhere `EKF` / `ekf`
   (group `naming`); "Results are not saved", "kept in memory only" (group
   `stored-results`); and the phrases of Task 4.7's statements rule.

**Acceptance Criteria:**
- [ ] `git grep -n -i -e "preference that calculations read" -e "preferences that" -- docs/SENSOR_FUSION.md docs/COMPUTED_PLOTS.md` prints nothing.
- [ ] SENSOR_FUSION §2, §7 and §8 and COMPUTED_PLOTS §6 contain the statements of steps 1-4.
- [ ] The root README tree names `plugincodeidentity.*` and renders as a tree (column alignment of the comment column kept).
- [ ] `audit_cleanup` passes (`fusion-model`, `naming`, `stored-results`, `result-validity`).

**Complexity:** S

---

### Task 4.5: Comment sweep in `src/` and `tests/`

**Purpose:** No comment may still say that a record carries the environment
fingerprint, that a registry change keeps records, or that unrelated changes
make stored results stale; the audit rules of Task 4.7 must hold for the
committed Phase 1-3 code.

**Files to modify** (only if the checks below find something; comments only):
- whatever the sweep reports; expected candidates are listed in step 2.

**Technical Approach:**

1. Each of these must print nothing (run from the repository root):
   ```
   git grep -n -E "makes every (record|stored result) stale|of a preference that calculations read|marker, the environment fingerprint|Bump it, or the calculation's result version" -- src tests docs README.md ":!tests/README.md" ":!tests/audit"
   git grep -n -E "calculationEnvironment|CalculationEnvironment" -- "src/calculationrecord.*" "src/calculationresultstore.*" "src/engine/storedcalculationresult.*"
   git grep -n -E "markCalculationRecordsUnconfirmed" -- src tests
   git grep -n -i -E "registry change.{0,60}keeps? (their|its) records?|keeps their records" -- src
   ```
   and `git grep -c "Bump it, or the result version of the calculation concerned" -- src/calculations/builtincalculations.h` must print 1.
2. Known candidates at ce2fb2b, which Phases 1 and 3 were told to change; fix
   any that survived, following the wording of the phase that owns the file:
   - `tests/tst_fusion_store.cpp` ~845, "Spec 8: bumping the compatibility
     marker, the environment fingerprint or ..." (Phase 3 Task 3.9 step 1
     wording: "Bumping the compatibility marker, the fit's result version, or
     the result version recorded for a calculation its lookups went through
     drops the record on load.").
   - `src/calculations/builtincalculations.h`, the bump paragraph (Phase 3
     Task 3.4 step 5 wording). The phrase `Bump it, or the result version of the calculation concerned`
     must stand on one comment line; if it was wrapped, re-wrap the comment.
     `CalculationDescriptor::resultVersion` stays on exactly one line.
   - `src/engine/storedcalculationresult.h` ~77-78, "The code stamps (the
     calculation-compatibility marker and the calculation environment
     fingerprint) are deliberately absent ... Whoever stores a snapshot adds
     them." becomes "The code stamp (the calculation-compatibility marker) is
     deliberately absent: ... Whoever stores a snapshot adds it." (The
     sentence at ~150 about the text form the environment fingerprint uses for
     preferences is still true and stays.)
   - `src/calculationresultstore.cpp` ~132 ("calculation compatibility or
     environment changed") and ~175 ("environment fingerprint covers
     registrations"), `src/logbookmanager.h` ~192, `src/sessionmodel.cpp`
     ~1376 and `src/sessionmodel.h` ~149: Phase 3 rewrites or removes them;
     verify.
3. **Leave as they are** (checked, still true): the environment fingerprint's
   own comment in `builtincalculations.h`, `logbookmanager.*` and
   `sessionmodel.*` (the column-cache meaning); `storedcalculationresult.h`
   ~150; `tests/README.md` (this phase rewrites it in Task 4.8).
4. Change no code. List every file changed in the phase report.

**Acceptance Criteria:**
- [ ] The four sweep commands of step 1 print nothing and the count is 1.
- [ ] `git diff --stat -- src tests/*.cpp tests/support tests/fusion` shows comment-only edits, each listed in the phase report (none is also acceptable).

**Complexity:** S

---

### Task 4.6: `tests/acceptance_map.txt`: the range 401-442 and the amended 3xx items

**Purpose:** Every clause of "Stored results: validity that mirrors memory" is
traceable to evidence, individually, and the four 3xx items it supersedes say
what now holds.

**Files to modify:**
- `tests/acceptance_map.txt`

**Technical Approach:**

1. **Header (lines 1-30).**
   - "Four specifications, four item ranges" becomes "Five specifications, five
     item ranges". After the 301-350 entry add:
     ```
     #   401-442   stored results: validity that mirrors memory (resolutions,
     #             runtime registry drops, the plug-in code identity, unreadable
     #             records, record format 2): item = 400 + clause number. Stated
     #             in full in tests/README.md, appendix E. It amends the
     #             specification of 301-350; items 310, 316, 317 and 341 are
     #             stated as amended.
     ```
   - "Every item must lie in one of the four ranges" becomes "five ranges";
     "every item 101-120, 201-247 or 301-350" becomes "every item 101-120,
     201-247, 301-350 or 401-442"; "sections 9.1, 9.2, 9.3 and 9.4" becomes
     "9.1 to 9.5".
2. **Amend four 3xx items in place** (item numbers and existing lines stay):
   - `# 310 - ...` becomes
     `# 310 - (3, as amended) a record is removed only when stale on load, with its session, on an input change or a runtime registry change that drops its result, or replaced; never by eviction, unload, a registry change that does not reach it, a teardown removal, quit`
     and add the line `310 tst_result_store registryChangeDeletesRecord` after
     the existing 310 lines.
   - `# 316 - ...` becomes
     `# 316 - (4.2, as amended) valid only while CalculationCompatibilityVersion and the result version match and every lookup resolves as it did; fusion's result version is the algorithm string`
     and add `316 tst_calcengine_restore restoreAcrossRegistries`.
   - `# 317 - ...` becomes
     `# 317 - (4.2, as amended) the bump rule's clause: bump it, or the result version of the calculation concerned, whenever a change can alter what a requested calculation or anything it reads produces`
     and add `317 audit result-validity`.
   - `# 341 - ...` becomes
     `# 341 - (8, as amended) test: bumping the compatibility version, the result version, or the result version recorded for a calculation its lookups went through drops the record on load`
     (lines unchanged).
   - Item 324 already names `registryChangeKeepsLoadedRowConfirmed` (Phase 3);
     verify.
3. **Append this block at the end of the file, exactly** (one blank line before
   it). Item numbers follow appendix E (Task 4.8).

```
# ---- Stored results: validity that mirrors memory: item = 400 + clause number ----

# 401 - (2) a stored result goes stale exactly when the same result in memory would be dropped, or when the code that computed it changes; nothing unrelated to what it reached makes it stale
401 tst_fusion_store storedFitSurvivesUnrelatedChanges
401 tst_fusion_store runtimeRegistrationDropsFitAndRecord
401 tst_fusion_store codeStampChangeDropsRecordOnLoad
401 tst_result_store recordSurvivesUnrelatedChanges
401 tst_result_store registryChangeDeletesRecord

# 402 - (3.1, 3.2) in memory a requested result is dropped when a value it reached changes (samples, unit text, a stored attribute, directly or through calculations and conversions, a missing value that appears) or a declared preference it reached changes
402 tst_calcengine_restore droppedByInputChange
402 tst_calcengine_restore droppedByPreferenceAndSource
402 tst_calcengine_restore droppedByRequestOfUpstream
402 tst_fusion_store dependencyEditDropsRecord

# 403 - (3.3) it is dropped when a registry change touches a name it resolved, directly or transitively: a calculation whose outputs include it added or removed, a calculation whose result it used removed, a family accepting it added or removed, the source-conversion layer changed
403 tst_calcengine_restore droppedByRegistryChange
403 tst_builtins_engine altitudeMarkerTeardownReportsNothing
403 tst_fusion_store runtimeRegistrationDropsFitAndRecord

# 404 - (3) everything else leaves it installed: edits to values it did not reach, registrations of names it never looked up (altitude markers, unrelated plug-in outputs), preferences it did not reach
404 tst_calcengine_restore droppedByRegistryChange
404 tst_fusion_store storedFitSurvivesUnrelatedChanges
404 tst_fusion_store unrelatedEditKeepsRecord
404 tst_result_store recordSurvivesUnrelatedChanges
404 tst_result_store noDeleteWithoutInputChange
404 manual M20

# 405 - (4.1) inputs, unchanged: the fingerprint over every leaf the result reached, present or absent, declared preference values included, matches the session and preferences as they are now
405 tst_calcengine_restore fingerprintKnownAnswer
405 tst_calcengine_restore exportLeaves
405 tst_calcengine_restore restoreStaleChecks
405 tst_fusion_store dependencyEditDropsRecord

# 406 - (4.2) the record states, for every name the result looked up directly or transitively, what provided it: a calculation (instance id and result version), the session's own data, or nothing
406 tst_calcengine_restore resolutionCodes
406 tst_calcengine_restore exportResolutions
406 tst_fusion_session restoredFitIsIndistinguishable
406 tst_result_records layoutIsPinned
406 tst_result_records roundTripIsBitExact
406 tst_result_records corruptInputIsRefused

# 407 - (4.2) at load the same lookups are repeated against the current registry and must give the same answers
407 tst_calcengine_restore restoreAcrossRegistries
407 tst_calcengine_restore restoreStaleChecks
407 tst_calcengine_restore ringIsNeverStored
407 tst_result_store lookupResolvingDifferentlyDeletesRecord
407 tst_fusion_store lookupResolvingDifferentlyAtLoadDeletesFit

# 408 - (4.3) code: CalculationCompatibilityVersion and the calculation's own result version are unchanged
408 tst_result_records stampsAreCurrent
408 tst_result_store staleRecordDeletedOnLoad
408 tst_fusion_store codeStampChangeDropsRecordOnLoad
408 tst_calcengine_restore restoreStaleChecks

# 409 - (4) the calculation environment fingerprint is no longer part of a record
409 tst_result_records stampsAreCurrent
409 tst_result_records layoutIsPinned
409 tst_result_store recordSurvivesUnrelatedChanges
409 audit result-validity

# 410 - (4) the bump-rule clause reads: bump CalculationCompatibilityVersion, or the result version of the calculation concerned, whenever a change can alter what a requested calculation or anything it reads produces
410 audit result-validity
410 audit stored-results

# 411 - (4) a record that fails any check is deleted when its session is loaded and the calculation reads not requested; nothing is recomputed
411 tst_result_store staleRecordDeletedOnLoad
411 tst_result_store lookupResolvingDifferentlyDeletesRecord
411 tst_result_store pluginEditStalesRecordsThatReadIt
411 tst_result_store formatOneRecordIsDeletedOnLoad
411 tst_fusion_store lookupResolvingDifferentlyAtLoadDeletesFit

# 412 - (5) a registry change made while the application runs that drops an installed requested result also deletes its record, like an input change
412 tst_result_store registryChangeDeletesRecord
412 tst_fusion_store runtimeRegistrationDropsFitAndRecord
412 tst_calcengine_restore droppedByRegistryChange
412 tst_result_columns registryChangeKeepsLoadedRowConfirmed

# 413 - (5) tearing the registry down at shutdown, destroying or evicting a session, and replacing a session's contents without an input change delete nothing
413 tst_calcengine_restore noDropEventWithoutInputChange
413 tst_calcengine_restore registryDestructionReportsNothing
413 tst_builtins_engine altitudeMarkerTeardownReportsNothing
413 tst_result_store registryChangeDeletesRecord
413 tst_result_store noDeleteWithoutInputChange
413 audit result-validity

# 414 - (5) a registry change that does not touch a result leaves the installed result and its record alone, and column values over that record remain cacheable
414 tst_result_store recordSurvivesUnrelatedChanges
414 tst_result_store noDeleteWithoutInputChange
414 tst_result_columns registryChangeKeepsLoadedRowConfirmed
414 tst_fusion_store storedFitSurvivesUnrelatedChanges
414 tst_calcengine_restore droppedByRegistryChange

# 415 - (6) every calculation, measurement and attribute a plug-in registers declares a result version: the plug-in code identity
415 tst_python_bridge pluginRegistrationsCarryCodeIdentity
415 audit result-validity

# 416 - (6, as settled) the identity is one digest over every .py file under the plug-in folder, subfolders included (relative name and bytes, in name order; __pycache__ and hidden folders left out), the SDK file, and the Python and numpy versions
416 tst_plugin_identity encodingIsPinned
416 tst_plugin_identity eachIngredientChangesIdentity
416 tst_plugin_identity absentVersionUsesFixedToken
416 tst_plugin_identity readsTheFolderRecursively
416 tst_plugin_identity subfolderFileChangesIdentity
416 tst_plugin_identity pycacheAndHiddenDirectoriesAreIgnored
416 tst_plugin_identity unreadableFileIsNotEmpty
416 tst_python_bridge pluginRegistrationsCarryCodeIdentity

# 417 - (6) it is computed once, when the plug-ins are loaded
417 tst_python_bridge pluginRegistrationsCarryCodeIdentity
417 tst_python_bridge secondInitialiseIsNoOp
417 audit result-validity

# 418 - (6) editing any plug-in file, adding or removing one, or upgrading Python or numpy changes it, so a stored result whose lookups went through any plug-in calculation goes stale at its next load
418 tst_result_store pluginEditStalesRecordsThatReadIt
418 tst_plugin_identity eachIngredientChangesIdentity
418 tst_plugin_identity subfolderFileChangesIdentity

# 419 - (6) a stored result whose lookups touched no plug-in calculation is unaffected by plug-in changes
419 tst_result_store pluginEditStalesRecordsThatReadIt
419 tst_fusion_store storedFitSurvivesUnrelatedChanges
419 manual M21

# 420 - (6) the environment fingerprint of the logbook column cache also covers every registration's result version: a plug-in edit or a built-in result-version change discards cached column values; its other contents and its role are unchanged
420 tst_builtins_engine fingerprintCoversResultVersions
420 tst_builtins_engine fingerprintChanges
420 tst_column_cache pluginEditDiscardsCachedValues
420 tst_column_cache altitudeMarkerChangeDiscards
420 tst_logbook_index differentEnvironmentDiscards
420 manual M21

# 421 - (7) a record that exists but cannot be opened or read in full at its session's load is skipped for that load: neither restored nor deleted; the calculation reads not requested
421 tst_result_store unreadableRecordIsSkipped
421 tst_result_store dependentOfSkippedRecordIsKept
421 manual M22

# 422 - (7) the next load tries again; a new publish for the pair replaces it; deleting the session or the stray pass at start-up removes it
422 tst_result_store unreadableRecordIsSkipped
422 tst_result_columns skippedRecordValuesStayOutOfIndex
422 tst_result_records writeReadReplace
422 tst_result_records removeSessionDeletesRecords
422 tst_result_records strayRecordsRemovedAtScan
422 manual M22

# 423 - (7) a record that was read but is not a record, is damaged, or has a format version this build does not read is deleted as stale
423 tst_result_store staleRecordDeletedOnLoad
423 tst_result_store formatOneRecordIsDeletedOnLoad
423 tst_result_records readStatuses
423 tst_result_records corruptInputIsRefused

# 424 - (7) logbook column values of a session that depend on a skipped record are not cached in index.json while it stays skipped
424 tst_result_columns skippedRecordValuesStayOutOfIndex
424 tst_result_store unreadableRecordIsSkipped

# 425 - (8) the record gains the resolutions and loses the environment fingerprint, and its format version increases
425 tst_result_records layoutIsPinned
425 tst_result_records futureVersionIsRefused
425 tst_result_records formatOneIsRefused
425 tst_result_records stampsAreCurrent

# 426 - (8) a record of an earlier format version is deleted as stale when its session loads; no migration
426 tst_result_store formatOneRecordIsDeletedOnLoad
426 tst_result_records formatOneIsRefused

# 427 - (9) the amended specification still holds where not amended: restoring is not requesting, publishing writes the record, records are read only for a session being loaded, the session file is untouched, a restored result is bit-identical
427 tst_calcengine_restore restoreIsNotAnInstall
427 tst_result_store restoreOnEveryLoadPath
427 tst_result_store temporaryLoadsNeverRestore
427 tst_fusion_store sessionFileBytesUnaffectedByRecord
427 tst_fusion_store restoredAfterRestartIsBitIdentical
427 audit stored-results

# 428 - (9) the engine keeps its threading rules; repeating the lookups at load uses the resolution of a fresh request and never runs a requested calculation
428 tst_calcengine_restore restoreAcrossRegistries
428 tst_calcengine_restore restoreIsNotAnInstall
428 tst_result_store lookupResolvingDifferentlyDeletesRecord
428 audit one-worker
428 audit stored-results

# 429 - (9) plug-in loading stays a start-up operation: nothing reloads plug-ins or watches their files
429 tst_python_bridge secondInitialiseIsNoOp
429 audit result-validity

# 430 - (10) test: a stored fit survives, restored with no job, adding and removing an altitude marker, registering and unregistering a calculation it never looks up, the descent-pause preference, another plug-in set, and a restart after any of these
430 tst_fusion_store storedFitSurvivesUnrelatedChanges
430 tst_result_store recordSurvivesUnrelatedChanges
430 manual M20

# 431 - (10) test: registering, while the application runs, a calculation that provides a name the fit looked up drops the installed fit and deletes its record
431 tst_fusion_store runtimeRegistrationDropsFitAndRecord
431 tst_result_store registryChangeDeletesRecord

# 432 - (10) test: a record whose lookups resolve differently at load (a new candidate with the same inputs, registered before the load) is deleted and the fit reads not requested
432 tst_fusion_store lookupResolvingDifferentlyAtLoadDeletesFit
432 tst_result_store lookupResolvingDifferentlyDeletesRecord
432 tst_calcengine_restore restoreAcrossRegistries

# 433 - (10) test: with a requested calculation that reads a plug-in output, editing any plug-in file, adding one, or changing the Python or numpy version makes its record stale; a plug-in edit does not when it reads no plug-in output
433 tst_result_store pluginEditStalesRecordsThatReadIt

# 434 - (10) test: the plug-in code identity is deterministic, and each listed ingredient changes it
434 tst_plugin_identity identityIsDeterministic
434 tst_plugin_identity encodingIsPinned
434 tst_plugin_identity eachIngredientChangesIdentity
434 tst_plugin_identity absentVersionUsesFixedToken

# 435 - (10) test: a plug-in edit discards cached logbook column values over plug-in calculations at the next start
435 tst_column_cache pluginEditDiscardsCachedValues

# 436 - (10) test: an unreadable record (held open without sharing on Windows, a directory at its path) is kept, not restored, and restored at a later load; its dependent column values are not cached meanwhile
436 tst_result_store unreadableRecordIsSkipped
436 tst_result_store dependentOfSkippedRecordIsKept
436 tst_result_columns skippedRecordValuesStayOutOfIndex

# 437 - (10) test: a record of the previous format version is deleted as stale
437 tst_result_store formatOneRecordIsDeletedOnLoad
437 tst_result_records formatOneIsRefused

# 438 - (10) test: the existing tests of stored results pass, those that asserted environment staleness rewritten to these rules
438 tst_result_store staleRecordDeletedOnLoad
438 tst_fusion_store codeStampChangeDropsRecordOnLoad
438 tst_result_columns environmentChangeDiscardsCachedValue
438 tst_result_columns writeAfterStartupDropFlushesIndexFirst
438 tst_result_columns registryChangeKeepsLoadedRowConfirmed

# 439 - (11) docs/ and the plug-in README describe validity, the resolutions, the format version, unreadable records, column values over skipped records, the fingerprint covering result versions, the bump rule and the plug-in code identity; no note says unrelated changes make stored results stale
439 audit result-validity

# 440 - (12) a stored result is a memory of an in-memory result: stale when that one would be dropped, and when the code changes, and at no other time
440 tst_fusion_store storedFitSurvivesUnrelatedChanges
440 tst_fusion_store runtimeRegistrationDropsFitAndRecord
440 tst_fusion_store codeStampChangeDropsRecordOnLoad

# 441 - (12) what a result reached decides its validity, never what else is registered
441 tst_calcengine_restore restoreAcrossRegistries
441 tst_result_store recordSurvivesUnrelatedChanges
441 audit result-validity

# 442 - (12) a transient failure to read is not evidence that a record is wrong
442 tst_result_store unreadableRecordIsSkipped
442 tst_result_store dependentOfSkippedRecordIsKept
442 manual M22
```

4. Every function above is one that Phases 1-3 name, or one that already
   exists (checked at ce2fb2b: `droppedByInputChange`,
   `droppedByPreferenceAndSource`, `droppedByRequestOfUpstream`,
   `fingerprintKnownAnswer`, `exportLeaves`, `restoreStaleChecks`,
   `noDropEventWithoutInputChange`, `restoreIsNotAnInstall`,
   `fingerprintChanges`, `altitudeMarkerChangeDiscards`,
   `differentEnvironmentDiscards`, `secondInitialiseIsNoOp`,
   `noDeleteWithoutInputChange`, `temporaryLoadsNeverRestore`,
   `restoreOnEveryLoadPath`, `staleRecordDeletedOnLoad`,
   `unrelatedEditKeepsRecord`, `dependencyEditDropsRecord`,
   `codeStampChangeDropsRecordOnLoad`, `sessionFileBytesUnaffectedByRecord`,
   `restoredAfterRestartIsBitIdentical`, `readStatuses`, `writeReadReplace`,
   `removeSessionDeletesRecords`, `strayRecordsRemovedAtScan`,
   `corruptInputIsRefused`, `roundTripIsBitExact`, `layoutIsPinned`,
   `stampsAreCurrent`, `futureVersionIsRefused`, `restoredFitIsIndistinguishable`).
   If one does not exist under that name in the committed test file, report it
   under "Deviations"; never delete a line to make the audit pass.

**Acceptance Criteria:**
- [ ] The header describes five ranges, including 401-442.
- [ ] Items 310, 316, 317 and 341 carry "as amended" `#` lines and the three added lines.
- [ ] `grep -c "^4[0-9][0-9] " tests/acceptance_map.txt` prints 156 (the lines of the block above), and every item 401-442 has at least one test or audit line.
- [ ] Every new test function of Phases 1-3 (the 31 new or renamed functions listed under Dependencies > Assumptions) appears in at least one 4xx line.
- [ ] No line lies outside 1-19, 101-120, 201-247, 301-350 and 401-442; `audit_cleanup` passes.

**Complexity:** M

---

### Task 4.7: `tests/audit/cleanup_audit.cmake`: group `result-validity`, the bump-rule pattern, the range 401-442

**Purpose:** Keep the new boundaries in place with text rules that hold for the
Phase 1-3 code as documented, and extend the traceability checker to the new
range.

**Files to modify:**
- `tests/audit/cleanup_audit.cmake`

**Technical Approach:**

1. **Header (lines 1-23).** After the stored-results bullet add:
   ```
   #   - a stored result goes stale only when the same result in memory would be
   #     dropped or its code changed: a record carries no environment
   #     fingerprint, the plug-in code identity is computed in one place at
   #     start-up, only an owner destroyed at shutdown removes registrations as
   #     teardown, and no document says that unrelated changes make stored
   #     results stale (items 401-442).
   ```
2. **Group `stored-results`, the docs count** (line ~566): the pattern
   `"Bump it, or the calculation's result version"` becomes
   `"Bump it, or the result version of the calculation concerned"` (count 1 in
   `docs/CALCULATIONS.md`, label unchanged). The code count
   (`CalculationDescriptor::resultVersion`, 1 in `builtincalculations.h`) is
   unchanged.
3. **New block** after the `stored-results` group and before "leftover
   markers", in the style of the existing blocks (`=====` banner, `─────` group
   line, an `Allow:` comment on every rule):

```cmake
# =============================================================================
# Stored results: validity that mirrors memory (acceptance items 401-442): a
# stored result goes stale when the same result in memory would be dropped, and
# when the code that computed it changes. What a result looked up is part of
# its record; what else is registered is not. The rules below keep the
# environment fingerprint out of the record, the plug-in code identity in one
# place and computed once at start-up, teardown removals at shutdown only, the
# environment check away from records, and the documents current.
# =============================================================================

# ─────────────────────────────── result-validity (items 409, 410, 413, 415, 417, 429, 439, 441)
audit_group(result-validity)
# Allow: none expected. What else is registered never makes a record stale, so
# neither the record format, the store nor the snapshot names the calculation
# environment fingerprint (the column cache in logbookmanager.* and
# sessionmodel.* does). The camelCase spelling only: a comment may still say
# "environment fingerprint" in words.
expect_none("a record carries no environment fingerprint" "calculationEnvironment|CalculationEnvironment"
  "src/calculationrecord.*" "src/calculationresultstore.*" "src/engine/storedcalculationresult.*")
# Allow: none expected. The digest is computed by plugincodeidentity.cpp and
# asked for by the plug-in host at start-up (tests build stand-in identities
# with the same functions; tests are not searched).
expect_only("one authority: the plug-in code identity" "pluginCodeIdentity\\(|readPluginCodeFiles\\("
  "^src/plugincodeidentity\\.(cpp|h)$|^src/pluginhost\\.(cpp|h)$" src)
# Allow: a new owner of registrations that declares a result version (a new
# explicit built-in, say) is added to the allowed-file regex. The plug-in
# adapters build descriptors; the host stamps every plug-in registration in
# registerEach(). `==` comparisons do not match.
expect_only("result versions are declared by the engine, the fusion registration and the plug-in host"
  "[.>]resultVersion *=[^=]"
  "^src/engine/|^src/fusion/fusionregistration\\.cpp$|^src/pluginhost\\.cpp$" src)
# A removal marked as teardown reports no drop, so the records of the results
# it drops survive: only an owner destroyed at shutdown uses it, today the
# altitude-marker manager's destructor. Allow: a new owner that unregisters in
# its destructor is added to both rules (and to docs/CALCULATIONS.md 15.8);
# a runtime change never is.
expect_only("teardown removals only at shutdown" "Removal::Teardown"
  "^src/engine/|^src/altitudemarkerfeature\\.cpp$" src)
expect_count("teardown removals only at shutdown" "Removal::Teardown" 1 src ":!src/engine")
# Allow: none expected. Plug-in registrations live as long as the process.
expect_none("the plug-in host never unregisters" "unregister\\("
  "src/pluginhost.*" "src/pluginadapters.*")
# Plug-in loading stays a start-up operation: MainWindow initialises the host
# once, and nothing reloads plug-ins or watches their files. Allow: a file
# watcher for something other than plug-ins gets a ":!path" exclusion in the
# third rule.
expect_count("plug-ins are loaded once, at start-up" "[.>]initialise\\(" 1
  src ":!src/pluginhost.cpp" ":!src/pluginhost.h")
expect_only("plug-ins are loaded once, at start-up" "[.>]initialise\\("
  "^src/mainwindow\\.cpp$" src ":!src/pluginhost.cpp" ":!src/pluginhost.h")
expect_none("nothing watches the plug-in files" "QFileSystemWatcher" src)
# The environment check touches no record: its whole-session marking existed
# only because records carried the environment fingerprint. A record is
# unconfirmed only after a failed write or removal, or when the store skips it.
# Allow: none expected.
expect_none("the environment check touches no record" "markCalculationRecordsUnconfirmed" src tests)
expect_only("records are skipped by the result store" "markCalculationRecordSkipped\\("
  "^src/logbookmanager\\.(cpp|h)$|^src/calculationresultstore\\.(cpp|h)$" src)
# The bump rule of the amended specification, in the code; the note's copy is
# counted by the stored-results group. Allow: reword around it, never
# duplicate it; the old wording does not come back.
expect_count("the bump rule covers what a requested calculation reads"
  "Bump it, or the result version of the calculation concerned" 1
  src/calculations/builtincalculations.h)
expect_none("the old bump-rule wording is gone" "Bump it, or the calculation's result version"
  src docs README.md)
# Allow: tests/README.md is excluded because its section 10 describes this rule.
# Say what a stored result depends on instead.
expect_none("no text says an unrelated change makes stored results stale"
  "makes every (record|stored result) stale|of a preference that calculations read|marker, the environment fingerprint"
  src tests docs README.md ":!tests/README.md")
```

   Why each rule holds for the documented Phase 1-3 code (no false positive):
   - **No environment fingerprint.** Phase 3 AC: `grep calculationEnvironment
     src/calculationrecord.h src/calculationrecord.cpp` finds nothing; the
     store never named it in camelCase (its old texts, "calculation
     compatibility or environment changed" and "environment fingerprint covers
     registrations", are words, and Phase 3 rewrites them);
     `storedcalculationresult.h` says "calculation environment fingerprint" in
     words only. `checkCalculationEnvironment` lives in `sessionmodel.*`, which
     is not searched.
   - **Identity.** Phase 2: declared in `plugincodeidentity.h`, defined in its
     `.cpp`, called only in `pluginhost.cpp`; the new comments in
     `builtincalculations.h` and `calculationdescriptor.h` say "the plug-in
     code identity" in words. Tests (`tst_plugin_identity`, `tst_column_cache`,
     `tst_result_store`, `tst_fusion_store`) are outside `src`.
   - **Result-version setters.** At ce2fb2b plus Phase 1 the only member
     assignments in `src` are `calculationengine.cpp` (two) and
     `fusionregistration.cpp`; Phase 2 adds `d.resultVersion = resultVersion;`
     in `pluginhost.cpp` and leaves the adapters unchanged. The record decoder
     reads through `>>` and brace initialisation; `a.resultVersion ==
     b.resultVersion` in `storedcalculationresult.cpp` is excluded by `[^=]`
     (and is in `src/engine/` anyway).
   - **Teardown.** Phase 1: `Removal::Teardown` appears outside `src/engine`
     only in `altitudemarkerfeature.cpp`'s destructor (one line; its comment
     says "as teardown"). `sessionmodel.h` (Phase 3) and
     `calculationresultstore.h` say "a teardown removal" / "removed as
     teardown" in words. Tests use it through `ExtraRegistrations` and the
     engine tests, outside `src`.
   - **Host never unregisters.** Integration Notes: "the plug-in host never
     unregisters"; there is no `unregister(` in `pluginhost.*` or
     `pluginadapters.*` at ce2fb2b, and Phase 2 adds none.
   - **Loaded once.** The only `.initialise(` call in `src` outside the host is
     `PluginHost::instance().initialise(pluginDir)` in `mainwindow.cpp` (~161);
     no phase adds one. No `QFileSystemWatcher` exists in `src`.
   - **Unconfirmed / skipped.** Phase 3 removes `markCalculationRecordsUnconfirmed`
     (AC: `grep -rn` over `src tests` finds nothing) and adds
     `markCalculationRecordSkipped()` in the manager, called only by the
     store; the model's comments describe it in words.
   - **Bump rule.** Phase 3 Task 3.4 writes the phrase in
     `builtincalculations.h`; Task 4.5 re-wraps if needed; Task 4.2 writes it
     once in the note and removes the old one. Appendix D of `tests/README.md`
     spells the old clause in lower case and is not searched.
   - **Statements.** Tasks 4.1, 4.2, 4.4 remove them from the documents;
     Phase 3 rewrites the `tst_fusion_store.cpp` comment ~845 ("marker, the
     environment fingerprint") and Task 4.5 verifies. `python_plugins/README.md`
     is excluded from every rule (`ALWAYS_EXCLUDED`), so its text is checked
     by Task 4.3's criteria, not by the audit.

   If a rule trips on committed Phase 1-3 code anyway (a comment worded
   differently from its document), reword the **comment** (Task 4.5). Change a
   rule only when the committed code has a legitimate second user, and report
   that.
4. **Traceability checker.**
   - The comment above it (~578-582): "... and 301-350 (storing requested
     calculation results with the session, item = 300 + clause number)"
     becomes "..., 301-350 (storing requested calculation results with the
     session, item = 300 + clause number) and 401-442 (stored results:
     validity that mirrors memory, item = 400 + clause number). Four line
     forms; see the head of the map."
   - The range check (~651-654):
     ```cmake
     if(NOT ((item GREATER_EQUAL 1 AND item LESS_EQUAL 19) OR (item GREATER_EQUAL 101 AND item LESS_EQUAL 120)
             OR (item GREATER_EQUAL 201 AND item LESS_EQUAL 247) OR (item GREATER_EQUAL 301 AND item LESS_EQUAL 350)
             OR (item GREATER_EQUAL 401 AND item LESS_EQUAL 442)))
       _violation("[traceability] item ${item} is outside 1-19, 101-120, 201-247, 301-350 and 401-442: ${line}")
     endif()
     ```
   - After the `foreach(item RANGE 301 350)` block, the same block for
     `RANGE 401 442`.

**Acceptance Criteria:**
- [ ] `cmake -DREPO=. -P tests/audit/cleanup_audit.cmake` from the repository root prints "cleanup audit passed", with the rule count raised by exactly 14 over the count before this phase.
- [ ] Planted-hit proof. Plant each in turn, run the script, see the violation named, revert. None of this is committed.
      - (a) `// calculationEnvironment` in `src/calculationresultstore.cpp` → "a record carries no environment fingerprint".
      - (b) `// pluginCodeIdentity(` in `src/sessionmodel.cpp` → "one authority: the plug-in code identity".
      - (c) `// d.resultVersion = x;` in `src/pluginadapters.cpp` → "result versions are declared by ...".
      - (d) `// Removal::Teardown` in `src/sessionmodel.cpp` → both "teardown removals only at shutdown" rules.
      - (e) `// unregister(` in `src/pluginhost.cpp` → "the plug-in host never unregisters".
      - (f) `// x.initialise(` in `src/sessionmodel.cpp` → both "plug-ins are loaded once" rules.
      - (g) `// QFileSystemWatcher` in `src/mainwindow.cpp` → "nothing watches the plug-in files".
      - (h) `// markCalculationRecordsUnconfirmed` in `tests/tst_result_store.cpp` → "the environment check touches no record".
      - (i) `// markCalculationRecordSkipped(` in `src/sessionmodel.cpp` → "records are skipped by the result store".
      - (j) a second comment line `/// Bump it, or the result version of the calculation concerned` in `builtincalculations.h` → "the bump rule covers ..." (count 2).
      - (k) the phrase `Bump it, or the calculation's result version` in `docs/COMPUTED_PLOTS.md` → "the old bump-rule wording is gone".
      - (l) the phrase `makes every record stale` in `docs/DATA_SCHEMA.md` → "no text says an unrelated change ...".
      - (m) a map line `443 audit result-validity` → "outside ... 401-442".
      - (n) every `433` line commented out → "acceptance item 433 has no resolving test or audit line".
      - (o) a map line `401 tst_plugin_identity noSuchFunction` → "has no test function".
- [ ] The docs count rule of `stored-results` uses the new phrase.

**Complexity:** M

---

### Task 4.8: `tests/README.md`

**Purpose:** The test README must describe the suite as it is after Phases 1-3:
counts, catalogue rows, the global-registry notes, helpers, the traceability
matrices (9.4 amended, a new 9.5), the audit group, manual steps, and the
clause lists (appendix D amended, a new appendix E).

**Files to modify:**
- `tests/README.md`

**Technical Approach:**

1. **Table of contents (3-22).** Add after the appendix D line:
   `[Appendix E. The acceptance items of stored-result validity (401-442)](#appendix-e-the-acceptance-items-of-stored-result-validity-401-442)`.
2. **§1 counts (~39-44).** Replace "47 test executables" / "48 entries" / "55"
   with the numbers `ctest --test-dir build-phase1/FlySightViewer-build -C Release -N`
   shows. Expected: 48 executables (Phase 2 adds `tst_plugin_identity`), 49
   entries, 56 with the seven exact runs. If the numbers differ, write the
   real ones and say why in the report.
3. **§1 catalogue rows** (each exists once; edit in place):
   - `tst_calcengine_restore`: "every stale check (result version, bundle,
     inputs unavailable, leaves, fingerprint)" becomes "the resolutions (what
     provided each name the result looked up, their pinned codes and order;
     a result that met a dependency ring is never exported), every stale check
     (result version, bundle, inputs unavailable, resolutions, leaves,
     fingerprint), restore across registries (the same or unrelated
     registrations and a losing candidate restore; a new winning candidate with
     the same inputs, a provider's result version or a source conversion is
     stale)"; and "dropped by input changes only, never by `clear()`, a
     registry change or destruction" becomes "dropped by input changes and by
     registry changes made while the application runs that reach it (a
     registration, a removal, a family, a source conversion); never by
     unrelated registrations, a teardown removal, `clear()`, or the
     destruction of the registry or the engine".
   - `tst_builtins_engine`: add "the environment fingerprint covering every
     registration's result version (`fingerprintCoversResultVersions`), and the
     altitude-marker manager's destructor removing its registrations as
     teardown, which reports no drop while a marker removed at run time does
     (`altitudeMarkerTeardownReportsNothing`)".
   - `tst_result_store`: add "a registry change made while the application
     runs that reaches a result deletes its record, a teardown removal does not;
     unrelated registrations and the descent-pause preference keep it valid
     across loads and restarts; a lookup that resolves differently, or a
     changed plug-in code identity of a calculation the result read, makes it
     stale; an unreadable record (a directory at its path, a Windows lock,
     POSIX permissions; rows skip where the platform does not honour them) is
     skipped and restored at a later load, and a record that reads it is kept;
     a format-1 record is deleted".
   - `tst_result_columns`: add "values over a record skipped at a load never
     cached while it is skipped; a registry change that does not reach a result
     keeps its row confirmed (`registryChangeKeepsLoadedRowConfirmed`); an
     environment change discards cached values while the record restores".
   - `tst_result_records`: "the code stamps computed fresh" becomes "the code
     stamp (the compatibility marker) computed fresh, never made stale by a
     registration"; "the pinned byte layout" becomes "the pinned byte layout of
     format version 2, the resolutions included"; "other format versions" gains
     "(format 1, in both of its layouts, included)".
   - `tst_column_cache`: add "a plug-in edit (a changed plug-in code identity)
     discarding the cached values at the next start
     (`pluginEditDiscardsCachedValues`)".
   - `tst_fusion_session`: in the stored-result sentence add "the fit's
     snapshot lists what provided each name it looked up".
   - `tst_fusion_store`: add "kept across altitude-marker, registration,
     descent-pause and plugin-set changes, in memory and after a restart;
     dropped at once, with its record, by a registration that provides a name
     it looked up; deleted when a lookup resolves differently at load; a
     provider's result version in the record".
   - Rename the group heading "**Python plugin bridge**" to "**Python plugins**"
     and add after the `tst_python_bridge` row:
     `| \`tst_plugin_identity\` | The plug-in code identity (\`src/plugincodeidentity.h\`) without Python (label \`core\` only): the pinned encoding (a hand-built byte string for \`a.py\` and \`pkg/helper.py\` passed out of order), determinism and independence of the order files are passed in, each ingredient changing it (a file's bytes, a rename, a helper module added or removed, the SDK's bytes or readability, a file's readability, the Python and NumPy versions, bytes moved between files), the token \`none\` for a version that cannot be read, and the folder walk: every \`*.py\` under the folder, subfolders included, in name order with \`/\` names, \`__pycache__\` and hidden folders left out |`
   - `tst_python_bridge`: add "every plugin registration declares the plug-in
     code identity as its result version, equal to the digest recomputed from
     the folder, the SDK and the versions read independently
     (`pluginRegistrationsCarryCodeIdentity`)".
   - `audit_cleanup`: add "and a stored result goes stale only when its
     in-memory twin would be dropped or its code changed (no environment
     fingerprint in a record, the plug-in code identity computed in one place,
     teardown removals at shutdown only)".
4. **§1 notes.**
   - ~177-183: "(except `fingerprintSurvivesRuntimeAltitudeMarker`, which
     drives the real `AltitudeMarkerManager` ...)" becomes "(except
     `fingerprintSurvivesRuntimeAltitudeMarker` and
     `altitudeMarkerTeardownReportsNothing`, which drive the real
     `AltitudeMarkerManager` on the process-wide one and remove what they
     added)".
   - ~201-206: replace "Stored results follow the same rule: ... to simulate
     an environment change." with: stored results do not follow that rule. A
     record carries no environment fingerprint, so registering a calculation
     between writing a record and loading its session keeps the record valid
     unless the registration changes what a name the result looked up resolves
     to. To make a record stale on purpose, change a resolution (register a
     winning candidate for a looked-up name, as
     `tst_result_store::lookupResolvingDifferentlyDeletesRecord` does) or
     rewrite the record (`rewriteRecord` in `tst_result_store` and
     `tst_fusion_store`).
5. **§5.** After "The plugin files are copied into a temporary directory first,
   because `PluginHost` imports every `*.py` in the plugin folder" add: "and
   digests every `*.py` under it, subfolders included, for the plug-in code
   identity; the test's folder holds top-level files only, so the imported and
   the digested files are the same".
6. **§8 Writing a test.**
   - In the `logbookprobe.h` list add `UnreadableFile` (makes an existing file
     unreadable while it lives and puts it back on `release()`: `Directory` on
     every platform, `LockedWithoutSharing` on Windows, `NoReadPermission`
     elsewhere; `skipReason()` is non-empty where the mechanism is not honoured
     and the row `QSKIP`s) and `asFormatOne` (a format-2 record's bytes in the
     format-1 layout).
   - In the `jobfixture.h` sentence add `ExtraRegistrations` (calculations one
     test function adds to the global registry; destroyed after the queue and
     the model, it unregisters them newest first, as runtime changes).
   - The stored-results bullet: after "never with permission bits (Windows
     ignores the read-only attribute on directories)" add: "Make a record
     unreadable with `UnreadableFile`. A directory at a record's path is never
     listed, so the store reaches it only through the ids the logbook manager
     knows: within a run, not after a restart; restart rows use the lock or the
     permissions. Make a record stale by rewriting it or by changing a
     resolution; a registration alone does not."
   - The registry bullet (~614-617): after "Destroy a `SessionModel` before
     unregistering test calculations (a live model schedules a
     calculation-environment check)" add: "and a runtime removal that reaches
     a requested result deletes its record. `ExtraRegistrations` and `JobWorld`
     are destroyed after the model for that reason. Never switch a fixture to
     `CalculationRegistry::Removal::Teardown` to get around the order: it is for
     owners destroyed at shutdown."
7. **§9 Acceptance traceability.**
   - Intro: "Four specifications, four ranges ... the four tables below" →
     five.
   - 9.4 intro: add "Clauses 10, 16, 17 and 41 are stated as amended by the
     specification "Stored results: validity that mirrors memory" (9.5)."
   - 9.4 rows 310, 316, 317, 341: Section gains ", as amended", Clause is the
     new `#` text of Task 4.6 step 2, Evidence gains the added line
     (`tst_result_store::registryChangeDeletesRecord`,
     `tst_calcengine_restore::restoreAcrossRegistries`, `audit result-validity`).
   - 9.4 row 324: `environmentChangeUnconfirmsLoadedRows` becomes
     `registryChangeKeepsLoadedRowConfirmed`.
   - New **9.5 Stored results: validity that mirrors memory (items 401-442)**
     after 9.4. Intro: the forty-two clauses of the specification "Stored
     results: validity that mirrors memory", stated in full in
     [appendix E](#appendix-e-the-acceptance-items-of-stored-result-validity-401-442);
     item = 400 + clause number; the same four line forms as 9.2, and every
     item has at least one test or audit line; "Section" is the section of
     the specification; its section 1 (motivation) has no item; clauses 30-38
     are its section 10 tests, one per bullet, and 40-42 its principles; the
     specification amends the one of 9.4. Then a table
     `| # | Section | Clause | Evidence |` with one row per item 401-442:
     Section and Clause from the item's `#` line in the map; Evidence renders
     the item's map lines in the style of 9.3 and 9.4 (`` `target::function` ``,
     further functions of the same target comma-separated, targets separated
     by `;`, then `` `audit <group>` `` and `` `manual M<k>` ``). The table
     and the map list the same evidence.
8. **§10 Cleanup audit.**
   - Add a bullet after the `stored-results` bullet: **group
     `result-validity`** (items 409, 410, 413, 415, 417, 429, 439, 441): the
     record format, the result store or the snapshot names the calculation
     environment fingerprint (`calculationEnvironment...`); the plug-in code
     identity is computed, or the plug-in folder read for it, outside
     `plugincodeidentity.*` and the plug-in host; a result version is assigned
     outside the engine, the fusion registration and the plug-in host; a
     registration is removed as teardown (`Removal::Teardown`) anywhere but the
     altitude-marker manager's destructor; the plug-in host or its adapters
     unregister anything; the plug-in host is initialised anywhere but once in
     `MainWindow`, or anything in `src` watches files; the whole-session
     `markCalculationRecordsUnconfirmed` reappears, or anything but the result
     store marks a record skipped; the new bump-rule sentence is not in
     `builtincalculations.h` exactly once, or the old one ("Bump it, or the
     calculation's result version") appears in `src`, `docs` or `README.md`;
     or any text outside this file says that a change of registrations or
     preferences makes every stored result stale. This file is excluded from
     the last rule because this section describes it.
   - The traceability bullet: "an item outside 1-19, 101-120, 201-247 and
     301-350" → "... 301-350 and 401-442"; "an item 101-120, 201-247 or 301-350
     has no test or audit line" → "an item 101-120, 201-247, 301-350 or 401-442
     ...".
9. **§11 "Fusion sessions"** (~1440-1446): in the list of what the header
   holds, add `fixtureSessionWithSAccStoredAs()` (a fixture session whose
   `GNSS/sAcc` source data is stored under another name, so that only a
   registered calculation can provide `GNSS/sAcc`: `tst_fusion_store`'s lookup
   test).
10. **§12.3 Stored results.** The intro gains "and the settings changes, plugin
    edits and file locks that must not cost a stored fit". Append three steps
    after M19, before the closing "Pass / fail" paragraph, each opening with
    its bold id (one paragraph each, like M16-M19):
    - **M20 Unrelated changes keep a fit (404, 430).** With the three tracks of
      M16 fitted and shown, open Preferences > Altitude Markers, add the
      altitude 1000 and close the dialog. Roll stays drawn for the three
      tracks, the row stays plain, no job starts, and the three record files
      in `cache/` keep their modification times. In Preferences > Import set
      "Descent pause timeout" to 45: the same. The logbook column of M17 is
      recomputed; a hidden track's cell may stay empty until the track is shown
      (records are read only when a recording is loaded). Quit and start again,
      show the three tracks: roll is drawn at once, the row is plain, no job
      starts, the files keep their modification times, and the column shows
      the three numbers. Remove the altitude and set the timeout back: the
      same, also after another restart.
    - **M21 Editing a plugin (419, 420).** Quit. Copy the repository's
      `python_plugins/` folder to a scratch folder, copy `examples/imu_tilt.py`
      from the copy into its top level, and start the application with the
      environment variable `FLYSIGHT_PLUGINS` set to the scratch folder. The
      debug output shows `[PluginHost] Plug-in code identity: plugins-sha256:`
      followed by 64 hex digits and `(3 files)`. Add a logbook column over
      `_IMU_PEAK_ACCEL` and let it fill. Quit and start again without edits
      (the interpreter has written `__pycache__/` into the folder): the identity
      is the same, and the column shows its values for hidden tracks without
      loading them. Quit, add a comment line to the top-level `imu_tilt.py`,
      start: the identity differs, the column's values are recomputed in the
      background and are the same numbers, and showing the three fitted tracks
      draws roll at once with no job, their record files keeping their
      modification times (the fit looks up no plugin output). Quit, undo the
      edit and add a comment line to `examples/imu_tilt.py` instead (never
      imported), start: the identity differs from both earlier ones. Remove the
      scratch folder and unset `FLYSIGHT_PLUGINS`.
    - **M22 A record that cannot be read (421, 422, 442).** Windows only. Quit.
      In PowerShell, hold one fitted track's record open without sharing:
      `$f = [IO.File]::Open('<scratch>\FlySight Viewer\logbook\cache\<uuid>.builtin%2Efusion%2Efit.fvresult', 'Open', 'Read', 'None')`.
      Start the application and show that track: its roll is not drawn, the
      row shows the refresh control with 1, the debug output has one line
      containing "skipped (kept for the next load)", the file is still in
      `cache/` with its modification time, and that track's cell in the column
      of M17 is empty. Do not press refresh. Quit, run `$f.Close()`, start
      again: the cell stays empty until the track is shown; show it: roll is
      drawn at once, the row is plain, no job starts, and the cell shows its
      number.
    Section 12's intro ("Three scripts.") stays: the steps join 12.3.
11. **Appendix D.** Intro: add after "the rows of section 9.4": "Clauses 10,
    16, 17 and 41 are stated as amended by the specification "Stored results:
    validity that mirrors memory" (appendix E)." Replace those four clauses:
    - "10. (3, as amended) A record is removed only when it is found stale on
      load, when its session is deleted, when an input it depends on changes
      or a registry change made while the application runs drops its result,
      or when the next publish for the same pair replaces it; never by
      eviction, unloading, a registry change that does not reach it, a
      teardown, or quitting."
    - "16. (4.2, as amended) A record is valid only while
      `CalculationCompatibilityVersion` and the calculation's result version
      equal the current ones and every name it looked up resolves as it did
      (appendix E, clauses 6-8); for sensor fusion the result version is the
      kernel's algorithm string. The environment fingerprint is no longer part
      of a record."
    - "17. (4.2, as amended) The compatibility-version rule gains the clause:
      bump it, or the result version of the calculation concerned, whenever a
      change can alter what a requested calculation or anything it reads
      produces."
    - "41. (8, as amended) Test: bumping `CalculationCompatibilityVersion`, the
      calculation's result version, or the result version recorded for a
      calculation its lookups went through drops the record on load."
12. **Appendix E** after appendix D, titled "Appendix E. The acceptance items
    of stored-result validity (401-442)". Intro: the clauses of the
    specification "Stored results: validity that mirrors memory", which amends
    "Storing requested calculation results with the session" (appendix D), one
    sentence each with the specification's section number in front; items
    401-442 of `tests/acceptance_map.txt` (item = 400 + the number below) and
    the rows of section 9.5; section 1 (motivation) has no item; clause 16 is
    stated as the plan settled it (subfolders included). Then these clauses:

    1. (2) A stored result goes stale under exactly the conditions that would
       drop the same result in memory, plus a change of the code that computed
       it; nothing unrelated to what the result reached makes it stale.
    2. (3.1, 3.2) In memory, an installed requested result is dropped when a
       value it reached changes on its session (the samples or unit text of a
       source measurement, or a stored attribute, reached directly or through
       the calculations and conversions its inputs resolved through, a value it
       looked for and did not find that later appears included), or when a
       declared preference it reached changes.
    3. (3.3) It is dropped when a registry change touches a name it resolved,
       directly or transitively: a calculation whose outputs include such a
       name is added or removed, a calculation whose result it used is
       removed, a family that accepts such a name is added or removed, or the
       source-conversion layer changes.
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
        its role are unchanged.
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
        result is bit-identical to a fresh one.
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
    31. (10) Test: registering, while the application runs, a calculation that
        provides a name the fit looked up drops the installed fit and deletes
        its record.
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
13. Do not add any mention of `sensor-fusion-clean-port` (the audit pins its
    count in this file at 4) and do not link `PLANS/`.

**Acceptance Criteria:**
- [ ] The §1 counts equal `ctest -N`'s; `git grep -c -E "^\| \`tst_plugin_identity\` \|" -- tests/README.md` prints 1.
- [ ] Every catalogue row named in step 3 contains its new function names (`fingerprintCoversResultVersions`, `altitudeMarkerTeardownReportsNothing`, `registryChangeKeepsLoadedRowConfirmed`, `pluginEditDiscardsCachedValues`, `pluginRegistrationsCarryCodeIdentity`).
- [ ] `git grep -n -e "to simulate an" -e "environmentChangeUnconfirmsLoadedRows" -- tests/README.md` prints nothing.
- [ ] §8 names `UnreadableFile`, `asFormatOne`, `ExtraRegistrations` and `Removal::Teardown`; §11 names `fixtureSessionWithSAccStoredAs`.
- [ ] 9.5 has 42 rows, 401-442, matching the map line for line; 9.4 rows 310, 316, 317, 341 and 324 are updated.
- [ ] Appendix E has 42 numbered clauses; appendix D's clauses 10, 16, 17 and 41 read "as amended".
- [ ] `**M20 `, `**M21 `, `**M22 ` exist in 12.3.
- [ ] §10 has the `result-validity` bullet and the 401-442 range.
- [ ] `audit_cleanup` passes.

**Complexity:** L

---

### Task 4.9: Verification

**Purpose:** Prove that the documents, map and audit agree with the code and
that nothing regressed.

**Technical Approach:**
1. `cmake --build build-phase1 --config Release`. **Never build `build/`** (it
   would overwrite the Boost-enabled solver install). This phase changes no
   CMake file; the build proves the comment edits of Task 4.5 compile.
2. `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure -L audit`.
3. The planted-hit proofs of Task 4.7, each with
   `cmake -DREPO=. -P tests/audit/cleanup_audit.cmake` (no build needed).
   Revert every plant; `git status --short` afterwards shows only this phase's
   files and the pre-existing untracked paths.
4. `ctest --test-dir build-phase1/FlySightViewer-build -C Release -N` for the
   counts of Task 4.8 step 2.
5. Full suite: `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure`.
6. Render check: open the changed documents and `tests/README.md` in a
   Markdown preview. Tables keep the column counts of their header rows (9.5:
   four columns), the table-of-contents anchors resolve (appendix E), and the
   root README tree stays aligned.

**Acceptance Criteria:**
- [ ] Build succeeds; `-L audit` passes; the full suite passes.
- [ ] Each planted hit (a)-(o) was reported by the script, and none remains.
- [ ] The phase report lists every changed file, every comment rewritten in Task 4.5, and any map line pointed at a committed name that differs from its phase document.

**Complexity:** S

## Testing Requirements

### Unit Tests
- None new: this phase adds evidence, not tests.
- The audit is the phase's test: the `result-validity` group, the changed
  `stored-results` docs pattern, the extended checker and every map line must
  pass, and every new rule is proven by a planted hit (Task 4.7).

### Integration Tests
- The full suite passes unchanged in `build-phase1` (labels core, fusion,
  exact, python, audit). No test file changes apart from comments the Task 4.5
  sweep finds.

### Manual Verification
- Michael runs M20, M21 and M22 of `tests/README.md` §12.3 (and, as a
  regression, M16-M19) on a development build from `build-phase1`, on a copy of
  a logbook (the 12.1 preamble). Pass / fail per step goes in the phase report.
- Read-through against the behaviour seen: `docs/DATA_SCHEMA.md` §11-12,
  `docs/CALCULATIONS.md` §5, §7, §8, §9, §12, §15.8, §17,
  `python_plugins/README.md` §1, §7, §8, `docs/SENSOR_FUSION.md` §2, §7, §8,
  `docs/COMPUTED_PLOTS.md` §6.

## Notes for Implementer

### Gotchas
- **The checker finds a test function by the text `::<function>()`** in
  `tests/<target>.cpp`. A function defined inside its class body has no such
  text and its map line fails; move the definition out of line (a test-file
  edit, listed in the report), never drop the line.
- **Data-driven tests** (`restoreStaleChecks`, `restoreAcrossRegistries`,
  `eachIngredientChangesIdentity`, `staleRecordDeletedOnLoad`,
  `unreadableRecordIsSkipped`, `registryChangeDeletesRecord`,
  `recordSurvivesUnrelatedChanges`, `pluginEditStalesRecordsThatReadIt`,
  `skippedRecordValuesStayOutOfIndex`, `formatOneIsRefused`,
  `codeStampChangeDropsRecordOnLoad`, `storedFitSurvivesUnrelatedChanges`, ...)
  are cited by their base name, never with `_data`.
- **`tst_plugin_identity` is a new target.** Its map lines resolve only if
  `tests/tst_plugin_identity.cpp` exists under that exact name.
- **Exact twins are CTest tests, not executables**; the map names
  `tst_fusion_store` only.
- **Audit patterns are case-sensitive and line-based.** Keep
  `Bump it, or the result version of the calculation concerned` on one line in
  `docs/CALCULATIONS.md` §9 and in `builtincalculations.h`. Do not write
  "makes every record stale", "makes every stored result stale", "of a
  preference that calculations read" or "marker, the environment fingerprint"
  anywhere outside `tests/README.md`, not even to quote the old behaviour.
- **`python_plugins/README.md` is excluded from every audit search**
  (`ALWAYS_EXCLUDED`): its text is guarded only by Task 4.3's criteria.
- **SENSOR_FUSION.md is under `fusion-model`'s text rule** (no `frozen`,
  `twenty-one`, `stationary window`, `candidate window`, `coarse initializer`,
  `bias shifts below`, `zero bias shift`, branch name). "Candidate" alone is
  fine.
- **The `sensor-fusion-clean-port` count in tests/README.md is pinned at 4.**
  9.5 and appendix E must not name it.
- **Item numbers are fixed by clause order**; 9.5, appendix E and the map use
  the same numbering (401 = clause 1). Do not renumber when adding evidence.
- **Do not link `PLANS/`.** Cite the specifications by title.
- **The rule count** printed by the audit rises by exactly 14 (the
  `result-validity` group); the `stored-results` docs rule changes its pattern,
  not the count.
- **The environment check still discards cached values.** Documents must say
  that a registration or preference change discards cached column values (and
  so an unloaded row with a record can stay empty until shown) while the record
  stays valid. Do not write that such a change "keeps everything".
- **Line numbers in this document are from ce2fb2b** and are guides; Phases 1-3
  moved them.

### Decisions Made
- **A new range, 401-442 (item = 400 + clause number), rather than lines under
  the 3xx items.** The specification is a separate document that amends
  "Storing requested calculation results with the session"; most of its
  clauses (resolutions, runtime registry drops and teardown, the plug-in code
  identity, unreadable records, format 2, its own tests and principles) have
  no 3xx clause to hang on, and squeezing them under 3xx items would merge
  several clauses into one item and lose individual traceability. A separate
  range gives each clause one item, its own matrix (9.5) and its own clause
  list (appendix E), exactly as 201-247 did for the fusion improvements. Cost:
  the checker's hard-coded ranges, the map header, README §9 and §10 change
  (Tasks 4.6-4.8).
- **Superseded 3xx items are amended in place, not retired.** Items 310, 316,
  317 and 341 (and appendix D's clauses 10, 16, 17, 41) state behaviour the new
  specification changes ("never by a registry change", "the environment
  fingerprint"). Their numbers stay (the map and matrix are keyed by them);
  their text gains "as amended", following the precedent of appendix C's
  "(10, test 9) ... as amended" and appendix D's "(5, as settled)". Their
  evidence lines still resolve (Phase 3 rewrote the functions, not their
  names); each gains one line that shows the amended behaviour.
- **Clause enumeration.** One item per statement of sections 2-9 that a test
  or rule can show (29), one per section 10 test bullet (9: items 430-438),
  one for section 11 (439, audit only), one per section 12 principle (3:
  440-442). Section 1 (motivation) has no item. Section 3 is "for reference",
  but it states the engine behaviour that section 4 builds on and Phase 1
  changed (condition 3 now reported), so it has items 402-404.
- **Clause 16 is stated as settled** (every `*.py` under the folder, subfolders
  recursively, `__pycache__` and hidden folders left out; `examples/`
  included), the coordinator decision recorded in the Integration Notes,
  superseding the specification's "every `.py` file in the plug-in folder".
- **Item 422's "a new publish replaces it; deleting the session or the stray
  pass removes it"** is evidenced by the general mechanisms
  (`writeReadReplace`, `removeSessionDeletesRecords`,
  `strayRecordsRemovedAtScan`) plus the retry tests: no phase tests these three
  paths on a record that is unreadable at that moment, and on Windows a locked
  file cannot be replaced or removed while the lock is held, which is the
  point of skipping it. Manual M22 shows the retry.
- **A new audit group, `result-validity`,** rather than more rules in
  `stored-results`: the map can then cite the new boundaries separately
  (items 409, 410, 413, 415, 417, 429, 439, 441). The `stored-results` docs
  count keeps its place (item 317, as amended) with the new phrase.
- **Rules chosen for zero false positives on the documented code**: camelCase
  spellings for identifiers that comments describe in words
  (`calculationEnvironment`, `Removal::Teardown`, `pluginCodeIdentity(`),
  member-assignment form for result versions (`[.>]resultVersion *=[^=]`),
  call form for `initialise(` and `unregister(`, and `src`-only scopes where
  tests legitimately use the names. Rejected: a rule that `Unreadable` is
  never deleted (not a text fact; carried by `unreadableRecordIsSkipped`) and a
  positive rule that each document mentions the identity (brittle; carried by
  Tasks 4.1-4.3's criteria).
- **The manual steps join 12.3** (same preamble, same fitted tracks) as M20-M22
  rather than a new 12.4. There is no user gesture that registers a
  calculation providing a name the fit looks up, so the runtime drop (item 431)
  has no manual step; it is covered by `runtimeRegistrationDropsFitAndRecord`.
- **Documents beyond spec 11's list change too:** CALCULATIONS §5, §7 and §8
  (they state the resolution rule, the altitude-marker registrations and the
  result version's relation to the fingerprint), `docs/COMPUTED_PLOTS.md` §6
  and `docs/SENSOR_FUSION.md` §2 (the user-facing "unrelated settings" notes
  the spec orders removed), and the root README's source tree (new files).
- **Spelling:** each document keeps its own "plugin"; the term "plug-in code
  identity" stays hyphenated everywhere because it is the name in the log line
  and the source file.

### Open Questions
- None blocking.

## Definition of Done

This phase is complete when:
1. Every task's acceptance criteria pass.
2. `cmake --build build-phase1 --config Release` succeeds and the full
   `ctest --test-dir build-phase1/FlySightViewer-build -C Release` passes,
   `-L audit` included.
3. The documents follow the style of their existing sections (numbered
   sections, bold lead-ins, tables for vocabularies, no class layouts in user
   documents, no links to `PLANS/`).
4. No TODOs or placeholder text remain. The only files changed are
   `docs/DATA_SCHEMA.md`, `docs/CALCULATIONS.md`, `docs/SENSOR_FUSION.md`,
   `docs/COMPUTED_PLOTS.md`, `python_plugins/README.md`, `README.md`,
   `tests/README.md`, `tests/acceptance_map.txt` and
   `tests/audit/cleanup_audit.cmake`, plus the comment-only edits of Task 4.5
   listed in the phase report. `PLANS/`, `experiments/` and `build*/` are
   untouched.

```
Phase 4 documentation complete.
- Tasks: 9
- Estimated complexity: 16 (M, M, S, S, S, M, M, L, S)
- Ready for implementation: Yes
```
