# Phase 1: Engine resolutions and registry drops

## Overview

A stored result must go stale when, and only when, the same result in memory
would be dropped (spec sections 3-5). This phase gives the engine's snapshot
(`StoredCalculationResult`) the list of what every name the result looked up
resolved to, makes `restoreResult()` repeat those lookups and refuse a
difference with a new stale check `Resolutions`, and makes a registry change
made while the application runs report the requested results it drops exactly
like an input change, while every teardown path reports nothing. The record
codec carries the new list so that stored results keep restoring between this
phase and Phase 3 (see Decisions Made, D1).

## Dependencies

- **Depends on:** None - can begin immediately.
- **Blocks:** Phase 3 (record format 2, store, columns), Phase 4 (documentation, acceptance map, audit).
- **Assumptions:**
  - Branch `store-requested-calculations` at ce2fb2b or a descendant containing only this plan's commits; Phase 2 (plug-in code identity) may land before or after this phase. It touches `src/pluginhost.*`, `src/pluginadapters.*`, `src/calculations/builtincalculations.*` and their tests only; nothing here touches those files.
  - Build and test only in `build-phase1/`: `cmake --build build-phase1 --config Release`, then `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure`. **Never build `build/`.** No new test executable is added, so no reconfigure is needed.

## Background the implementer needs

What the engine records today (all in `src/engine/calculationengine.cpp` unless noted):

- `resolve()` (~323-399) notes `Resolution(name)` in the parent scope, then notes the stored/source leaf, then tries candidates. It caches a `ResolutionEntry {provider, instanceId, available, ...}` in `m_resolutions` only when the answer is context-free (no ring reached above it). `Provider` is `None | Stored | Source | Calculation`; an unavailable answer is normalized so that `Calculation` becomes `None` (a stored attribute with an invalid value keeps `Stored`).
  - Attribute: stored wins (`Stored`), else registry candidates (`Calculation` winner, or `None`).
  - Measurement with source data: if any source conversion is registered, the conversion instances are the only candidates (`Calculation` = the conversion instance, e.g. `builtin.conversion.default#IMU/az`, or `None`); otherwise passthrough (`Source`).
  - Measurement without source data: registry candidates.
- `leafClosure()` (~1369-1398) is the BFS from a result's direct edges through `Resolution` and `Result` nodes; `exportResult()` (~1400) and `restoreResult()` (~1426) both use it.
- `restoreResult()` checks, in order: `ResultVersion`, `Bundle` (both before gathering), then `dropNotRequested`, `gatherAsRoot`, `InputsUnavailable`, `Leaves`, `Fingerprint`.
- `onRegistryChanged()` (~977-1044) seeds the invalidation and delivers with `ExplicitDrops::Suppress`; `deliverBroadcast()` (~929) defers seeds met during an evaluation into `m_pendingSeeds` (Report) or `m_pendingRegistrySeeds` (Suppress); `flushPending()` (~948) applies both.
- Uncached `Resolution` nodes exist in a result's closure only after a dependency ring: a provisional answer is handed up (`handUp`) and never cached, and a cached answer may be "turned away" (`cachedAnswerUsable`) and re-evaluated provisionally with a possibly different answer. Both set `sawCycle` on every scope above them, and `ResultEntry::sawCycle` of the requested result records it. Without `sawCycle`, every `Resolution` node the result reached is cached, its entry is exactly the answer the result used, and it stays cached as long as the result is installed (invalidating it would drop the result through the reverse edges).

Shutdown paths that remove registrations or engines (established by reading the code):

| Path | What it does | Reports today | After this phase |
| --- | --- | --- | --- |
| `MainWindow::closeEvent` (`src/mainwindow.cpp` ~354) | shuts the job queue down, `model->flushDirtySessions()`, saves the dock layout | no registry change | unchanged |
| `MainWindow::~MainWindow` (~340) | deletes `m_plotRequests`, `m_jobQueue`, `ui`; then QObject deletes children in creation order: `m_settings`, `m_plotViewSettingsModel`, `model` (the `SessionModel`: every `SessionData`, so every engine, and the `CalculationResultStore` member), `m_plotModel`, `m_markerModel`, `m_momentModel`, ..., `m_altitudeMarkerManager` (created at ~180, after `model`) | - | unchanged |
| `AltitudeMarkerManager::~AltitudeMarkerManager` (`src/altitudemarkerfeature.cpp` ~35-42) | `registry.unregister(...)` for every altitude calculation, through the ordinary registry-change path | would report nothing (Suppress); in the application no engine is enrolled any more because `model` was deleted first | unregisters with `Removal::Teardown`: reports nothing even if an engine is still enrolled (Task 1.3) |
| `PluginHost` (`src/pluginhost.cpp`) | registers at `initialise()` only; never unregisters; has no destructor that touches the registry | - | unchanged |
| `~CalculationEngine` | withdraws from the registry, marks tickets gone | nothing | unchanged |
| `~CalculationRegistry` (the process-wide registry is a function-local static, destroyed after `main()` returns) | calls `registryDestroyed()` on any engine still enrolled (none in the application) | nothing (`clearCaches()`) | unchanged |
| `CalculationEngine::clear()` (session copy-assignment) | drops every cache entry | nothing | unchanged |

So shutdown never deletes a record for two independent reasons: the engines are gone before the only shutdown unregistration runs, and that unregistration is marked as teardown, which the engine never reports.

## Tasks

### Task 1.1: The resolutions in the snapshot

**Purpose:** Give `StoredCalculationResult` a pinned, sorted, comparable list of what each looked-up name resolved to, so the engine can export it and compare it at restore, and the record codec can store it.

**Files to modify:**
- `src/engine/storedcalculationresult.h` - new `StoredResolution` type, member `resolutions`, code and order functions, docs.
- `src/engine/storedcalculationresult.cpp` - their definitions; `sameContent()` compares `resolutions`.

**Technical Approach:**

1. Add, above `StoredCalculationResult`:

   ```cpp
   /// What provided one public name a result looked up.
   struct StoredResolution {
       enum class Provider {
           Nothing,        ///< unavailable: no stored value / source data and no candidate produced it,
                           ///< or the name has source data and every source conversion failed
           SessionData,    ///< the session's own data: a stored attribute (even one holding an invalid
                           ///< value), or a source measurement read through the passthrough
                           ///< (no source conversion registered)
           Calculation     ///< a calculation instance produced it (a derived candidate or a source conversion)
       };
       DependencyKey name;
       Provider provider = Provider::Nothing;
       QString instanceId;     ///< Calculation: the instance id ("<familyId>#<key>" for a family instance); else empty
       QString resultVersion;  ///< Calculation: that instance's descriptor resultVersion (may be empty); else empty
   };
   bool operator==(const StoredResolution &a, const StoredResolution &b);   // every member; name by DependencyKey ==
   inline bool operator!=(...)
   ```

   Declare `operator==` / `operator!=` so `QList<StoredResolution>` compares with `==`. `DependencyKey`'s default constructor leaves `type` unset; always build `name` with `DependencyKey::attribute()` / `measurement()`.

2. Pinned codes, following the pattern of `storedLeafKindCode()` / `storedLeafFromCode()` (h ~70-81, cpp ~119-148); codes are not tied to the enum's underlying values:
   - `quint8 storedResolutionProviderCode(StoredResolution::Provider p)` - `0` Nothing, `1` SessionData, `2` Calculation.
   - `std::optional<StoredResolution::Provider> storedResolutionProviderFromCode(quint8 code)` - nullopt for any other code.
   The name is encoded by whoever stores it (the record uses its own output key codes, Task 1.4); the engine layer needs no name code.

3. Order: `bool storedResolutionLess(const StoredResolution &lhs, const StoredResolution &rhs)` - attribute names before measurement names; then the first string (attribute key, or sensor), then the second (empty for an attribute, or measurement name), each with `QString::compare(..., Qt::CaseSensitive)` (UTF-16 code units), exactly like `storedLeafLess()`. A result looks up each name at most once as a node, so names are unique in a list and the order is total.

4. `StoredCalculationResult` gains `QList<StoredResolution> resolutions;` after `leaves`. Extend the member documentation (h ~21-51):
   - `resolutions`: for every `Resolution` node reached through the recorded edges by the same walk as `leaves` (directly or through any number of Resolution and Result nodes, explicit results included, rejected candidates included), the public name and what provided it when the result was published. Sorted by `storedResolutionLess()`, one entry per name. A restore repeats the lookups and refuses any difference.
   - Replace the paragraph "The code stamps (... environment fingerprint) are deliberately absent" only as far as needed to stay true (the stamps are still added by the record in this phase; Phase 3 changes the stamps).

5. `sameContent()` (cpp ~226): add `a.resolutions != b.resolutions` to the first comparison; update its doc comment ("id, version, detail, leaves, resolutions, fingerprint bytes").

**Acceptance Criteria:**
- [ ] `storedResolutionProviderCode` gives 0/1/2 for Nothing/SessionData/Calculation; `storedResolutionProviderFromCode` inverts it and returns nullopt for 3 and 0xFF.
- [ ] `storedResolutionLess` orders every attribute name before every measurement name, `EA2` before `EA_IN`, `B` before `a`, `S/a` before `S/b`, `R/z` before `S/a`, and is irreflexive.
- [ ] Two snapshots equal in everything but one resolution's `resultVersion` are not `sameContent`; equal ones are.

**Complexity:** S

---

### Task 1.2: Derive the resolutions from the graph; export and the `Resolutions` stale check

**Purpose:** Export the list with the result, and at restore derive it again from the graph that the repeated gathering recorded and refuse a mismatch before the leaf and fingerprint checks.

**Files to modify:**
- `src/engine/calculationengine.h` - replace `leafClosure()` with the closure helpers; `RestoreOutcome::StaleCheck::Resolutions`; docs of `exportResult()`, `RestoreOutcome`, `restoreResult()`, the class comment's "Stored results" paragraph.
- `src/engine/calculationengine.cpp` - the walk, the derivation, `exportResult()`, `restoreResult()`.
- `src/calculationresultstore.cpp` - `staleCheckName()` gains `case StaleCheck::Resolutions: return "resolutions";` (the switch must stay exhaustive).

**Technical Approach:**

1. One walk for both lists (reuse, do not duplicate, the BFS of `leafClosure()` ~1369-1398). In the private section replace `leafClosure()` by:

   ```cpp
   /// Everything a result reached from `direct` through the recorded edges:
   /// breadth-first over m_dependsOn following Resolution and Result nodes
   /// (explicit results included); Prepared nodes are ignored.
   struct Closure {
       QList<GraphNode> leaves;         ///< StoredAttribute / SourceMeasurement / SourceUnit / Preference; sorted by storedLeafLess, unique
       QList<GraphNode> resolutions;    ///< every Resolution node visited, unique, in no particular order
   };
   Closure closureOf(const QSet<GraphNode> &direct) const;

   /// The StoredResolution of each node, sorted by storedResolutionLess; nullopt when
   /// one cannot be stated from the cache: a node without an m_resolutions entry, or a
   /// Calculation provider whose Result node has no m_results entry (or no descriptor).
   /// Neither happens for a result whose evaluation met no ring (see exportResult()).
   std::optional<QList<StoredResolution>> storedResolutions(const QList<GraphNode> &resolutionNodes) const;
   ```

   `closureOf` is the current loop with one addition: when a visited node is a `Resolution` node, append it to `resolutions` before following its edges (a `Resolution` node with no edges, i.e. never cached, is still collected - that is what makes `storedResolutions()` return nullopt for it).

   `storedResolutions` maps each node's cached `ResolutionEntry` (`m_resolutions.constFind(node)`): `name = node.publicName()`; `Provider::None` -> `Nothing`; `Provider::Stored` and `Provider::Source` -> `SessionData` (instanceId and resultVersion left default-constructed); `Provider::Calculation` -> `Calculation` with `instanceId = entry.instanceId` and `resultVersion = m_results.value(GraphNode::result(entry.instanceId)).instance.descriptor->resultVersion` (look it up with `constFind`; missing entry or null descriptor -> nullopt). Use the cached Result entry's descriptor, not a registry lookup: it is the descriptor the answer was computed under, and a registry change of that registration would have dropped the result (the same reasoning `exportResult()` already states for the root at ~1416). Sort with `storedResolutionLess`. Const; touches `m_resolutions` / `m_results` only.

2. `exportResult()` (~1400-1424):
   - After the existing checks, return nullopt when `cached->sawCycle` is true: the evaluation met a dependency ring (a registration error, warned about on every detection), so what provided a name may have been a provisional answer that no cache entry holds.
   - `const Closure closure = closureOf(m_dependsOn.value(C));` `snapshot.leaves = closure.leaves;` `snapshot.resolutions = storedResolutions(closure.resolutions)`, returning nullopt if that is nullopt (defensive; unreachable without a ring). Fingerprint as today over `snapshot.leaves`.
   - Doc (h ~196-204): add "a result whose evaluation met a dependency ring" to the nullopt cases, and "the resolutions" to what the snapshot carries.

3. `RestoreOutcome::StaleCheck` (h ~214-221): insert `Resolutions` between `InputsUnavailable` and `Leaves`:
   `Resolutions, ///< the gathering met a ring, or a looked-up name resolved differently (another provider, instance or result version, a name looked up in only one of the two)`.
   Update the `invalidated` comment: "Stale with InputsUnavailable / Resolutions / Leaves / Fingerprint, or Restored".

4. `restoreResult()` (~1491-1502). After `gatherAsRoot` returns Ok and `looked` is built (as today):
   ```
   const Closure closure = closureOf(looked);
   if (scope.sawCycle)                                   -> flushPending(); stale(Resolutions)
   resolutions = storedResolutions(closure.resolutions);
   if (!resolutions || *resolutions != snapshot.resolutions) -> flushPending(); stale(Resolutions)
   if (closure.leaves != snapshot.leaves)                -> flushPending(); stale(Leaves)       (unchanged)
   fingerprint over closure.leaves                        -> stale(Fingerprint)                (unchanged)
   ```
   Final order of checks: `ResultVersion`, `Bundle`, (gathering) `InputsUnavailable`, `Resolutions`, `Leaves`, `Fingerprint`. Nothing is installed on any stale outcome and the calculation stays "not requested", exactly as for the existing checks. The calculation's own result version stays the separate `ResultVersion` check.

   Consequence to be aware of (and pinned by the tests of Task 1.5): a change of the session that makes a looked-up name resolve differently (e.g. a stored attribute appears where a calculation used to provide the name) now reports `Resolutions`, not `Leaves`; `Leaves` remains reachable for a snapshot whose leaf list alone differs.

5. Comments: rewrite the block comment above `leafClosure` (~1349-1367) to cover both lists ("The leaves and resolutions of a result are what its recorded edges reach ..."); the class comment "Stored results" paragraph (h ~64-74) mentions that restore repeats the lookups and compares their answers.

6. `src/calculationresultstore.cpp`: add the `Resolutions` case to `staleCheckName()` (~16-26). No other store change in this task.

**Acceptance Criteria:**
- [ ] `exportResult("expA")` (standard test world, EA_IN = 4) has `resolutions == [{attr EA_IN, SessionData, "", ""}]`.
- [ ] `exportResult("expB")` after expA and expB are requested has `resolutions == [{attr EA2, Calculation, "expA", ""}, {attr EA_IN, SessionData}, {attr EB_IN, SessionData}]`.
- [ ] `exportResult` returns nullopt for an Ok requested result whose evaluation met a ring, and leaves every engine counter unchanged, as for its other nullopt cases.
- [ ] A snapshot restored into an engine whose gathering meets a ring, or whose repeated lookups give another answer for any name, is `Stale` with `StaleCheck::Resolutions`, installs nothing, runs nothing, queues no event.
- [ ] When both the resolutions and the leaves differ, the check reported is `Resolutions`.
- [ ] `restoreIntoFreshEngine`, `restoreChain`, `restoreRejection`, `restoreNaNPayloadAndSignedZero`, `restoreBeatsOutstandingTicket` and `restoredResultInvalidatesLikePublished` pass unchanged; a restored result re-exports `sameContent` with its snapshot (resolutions included).

**Complexity:** M

---

### Task 1.3: Runtime registry drops are reported; teardown is not

**Purpose:** A registry change made while the application runs that drops an installed requested result reports it with the same event as an input change (spec section 5), while tearing registrations down, destroying the registry, destroying an engine and `clear()` report nothing.

**Files to modify:**
- `src/engine/calculationregistry.h` / `.cpp` - `Removal` enum, `unregister(id, removal)`, `RegistryChange::teardown`.
- `src/engine/calculationengine.h` / `.cpp` - `onRegistryChanged()` chooses Report / Suppress; rename `m_pendingRegistrySeeds`; listener and enum docs.
- `src/altitudemarkerfeature.h` / `.cpp` - the destructor unregisters as teardown.
- `src/calculationresultstore.cpp` - comment and log text of the drop branch.

**Technical Approach:**

1. Registry (`calculationregistry.h`):
   ```cpp
   /// Why a registration is removed.
   enum class Removal {
       Change,     ///< the application changes what is registered while it runs
       Teardown    ///< the registration's owner is being destroyed (shutdown)
   };
   bool unregister(const CalculationId &id, Removal removal = Removal::Change);
   ```
   (class scope: `CalculationRegistry::Removal`). `RegistryChange` gains `bool teardown = false; ///< removed with Removal::Teardown` with a doc line: engines drop exactly what they drop for any removal and report no requested result to an explicit-result listener. `unregister()` (cpp ~162-193) sets `change.teardown = (removal == Removal::Teardown)`; registrations always broadcast `teardown = false`. Document on `unregister()`: a `Change` removal that drops a requested result is reported like an input change (the result store then deletes its record); `Teardown` is for owners being destroyed. Everything else about `unregister()` (validation, `checkMutable`, memo, observers) is unchanged; the default argument keeps every existing call compiling with its current meaning of a runtime change.

2. Engine:
   - `onRegistryChanged()` (~1041-1043): replace the final comment and call with `deliverBroadcast(seeds, change.teardown ? ExplicitDrops::Suppress : ExplicitDrops::Report);` and a comment: a registry change made while the application runs that drops a requested result is reported like an input change (spec: condition 3 of "when a result in memory is dropped"); a teardown removal reports nothing. Seeding is unchanged (it already implements condition 3: removed results by registration id and family prefix, output resolutions of an added/removed calculation, resolutions a family accepts, every measurement resolution for a source conversion).
   - `ExplicitDrops` doc (h ~447-450): "Report for input changes and for registry changes made while the application runs; Suppress for registrations removed as teardown."
   - Rename `m_pendingRegistrySeeds` to `m_pendingTeardownSeeds` (h ~493-496, cpp `deliverBroadcast` ~936-939 and `flushPending` ~953-962); its comment: seeds of an invalidation that reports no explicit drops (a teardown removal), kept apart from `m_pendingSeeds`, whose drops are reported. With the change above, a deferred runtime registry change lands in `m_pendingSeeds` (Report) and a deferred teardown in `m_pendingTeardownSeeds` (Suppress); `flushPending()` applies `m_pendingSeeds` with Report and `m_pendingTeardownSeeds` with Suppress, as it does today with the old name. (The registry refuses changes while any enrolled engine evaluates, `checkMutable`, so this deferral exists for completeness; `deliverBroadcast` keeps asserting.)
   - `registryDestroyed()`, `clearCaches()`, `clear()`, the destructor: unchanged (they never queue events).
   - `ExplicitResultEvent::Kind::DroppedByInputChange` keeps its name (Decisions Made, D3); its doc becomes "an invalidation that started at an input, or at a registry change made while the application runs, dropped a requested result".
   - `setExplicitResultListener()` doc (h ~126-145): "An input change counts when it is a leaf notification, a preference change, a calculation being requested, published or restored whose "not requested" answer a requested result had used, or a registry change (a registration, or a removal with Removal::Change) that reaches the result. Never called for restoreResult()'s own install, clear(), a removal with Removal::Teardown, the registry's destruction or the engine's destruction." Class comment (h ~72-74): the listener hears of every drop by an input change or by a registry change made while the application runs.

3. `AltitudeMarkerManager::~AltitudeMarkerManager()` (`altitudemarkerfeature.cpp` ~35-42): `registry.unregister(calculationId(key), CalculationRegistry::Removal::Teardown);` and a comment stating the shutdown rule (the manager is destroyed at shutdown; its removals must never delete a stored result; in the application the session model, and with it every engine, is destroyed before it anyway). `refresh()` (~173-178) keeps the default `Change`: removing an altitude marker at run time is a runtime change. Header doc of the destructor (`altitudemarkerfeature.h` ~20): "unregisters the calculations it registered, as teardown".

4. `src/calculationresultstore.cpp` `onExplicitResultEvent()` drop branch (~80-85): comment "DroppedByInputChange, whatever its status: an input of the result or a registry change reaching it dropped it; a record, if any, no longer describes it"; the log reason `"an input changed"` becomes `"an input or the registry changed"`. Behavior unchanged: the store deletes the record for every drop event.

5. No change to `src/mainwindow.cpp`, `src/pluginhost.cpp` or `src/sessionmodel.cpp`: the table in "Background" is the analysis; the comment at `sessionmodel.cpp` ~1376 ("A registry change drops explicit results ... and keeps their records") becomes inaccurate for dropped results and is removed together with that code path in Phase 3.

**Acceptance Criteria:**
- [ ] With expA installed, registering a calculation whose outputs include `EA_IN` queues exactly `DroppedByInputChange expA Ok`, delivered after the names listener (`log == ["names", "explicit expA"]`).
- [ ] A `Change` removal of such a calculation, a family registration accepting a looked-up name, a source-conversion registration under a measurement input, and the removal of a calculation whose result a requested result used each report the drop(s).
- [ ] Registering and removing a calculation whose outputs the result never looked up reports nothing, the result stays installed (`resultStatus == Ok`, `exportResult` has a value) and the names listener is not called.
- [ ] `unregister(id, Removal::Teardown)`, `clear()`, `~CalculationEngine` and `~CalculationRegistry` (release builds) drop results without any event.
- [ ] Destroying an `AltitudeMarkerManager` while an engine holds a requested result that looked up its attribute reports nothing; removing the same marker through `refresh()` reports the drop.
- [ ] Every existing `unregister(id)` call compiles unchanged and behaves as a runtime change.

**Complexity:** M

---

### Task 1.4: The record codec carries the resolutions

**Purpose:** Keep stored results restorable between this phase and Phase 3: a record decoded without its resolutions would now always restore as `Stale/Resolutions` and be deleted, which would break every store and fusion-store test (Decisions Made, D1).

**Files to modify:**
- `src/calculationrecord.h` - layout comment.
- `src/calculationrecord.cpp` - encode and decode section 11.

**Technical Approach:**

1. Layout (header comment ~94-122): the checksum becomes item 12 and a new item 11 follows the outputs:
   ```
   //   11  resolution count            quint32
   //  11a  per resolution (snapshot    quint8 name code (1 attribute, 2 measurement: the output
   //       order)                      key codes), QString first (key or sensor), QString second
   //                                   (measurement name; null for an attribute), quint8
   //                                   storedResolutionProviderCode, QString instance id,
   //                                   QString result version (both as held: empty/null unless
   //                                   Calculation)
   //   12  checksum                    32 raw bytes: SHA-256 of every preceding byte
   ```
   Add one sentence under the table: section 11 was added to format version 1 together with the snapshot's resolutions; a record without it decodes as Corrupt and is deleted as stale at load; the version number changes once for this plan, in Phase 3 (format 2). `CalculationRecordFormatVersion` stays `1` in this phase.

2. Encoder (`encodeCalculationRecord` ~226-309): after the outputs loop, `stream << quint32(result.resolutions.size());` then per entry in list order (never re-sorted): the name code (`kAttributeCode` / `kMeasurementCode`) and strings exactly as the outputs write them (attribute: `key.attributeKey << QString()`), `storedResolutionProviderCode(r.provider)`, `r.instanceId`, `r.resultVersion`. Nothing is refused.

3. Decoder (`decodeCalculationRecord` ~315-474): after the outputs loop and before the `atEnd` check:
   - `constexpr qint64 kMinResolutionBytes = 1 + 4 + 4 + 1 + 4 + 4;` next to `kMinLeafBytes`.
   - read the count; stream failure -> Corrupt "the record is truncated"; `count > remaining() / kMinResolutionBytes` -> Corrupt "resolution count %1 exceeds the record".
   - per entry read code, first, second, provider code, instance id, result version; stream failure -> truncated; unknown name code -> Corrupt "unknown resolution name kind %1"; `storedResolutionProviderFromCode` nullopt -> Corrupt "unknown resolution provider %1"; append `{DependencyKey::attribute(first) | measurement(first, second), provider, id, version}`.
   - the trailing check's text becomes "unexpected bytes after the last resolution".
   - No sortedness or uniqueness check: a restore compares the list with the derived one, so a malformed list is stale, not misread.

**Acceptance Criteria:**
- [ ] `layoutIsPinned` matches the hand-written bytes including section 11 (Task 1.6).
- [ ] A snapshot with Nothing, SessionData and Calculation resolutions (attribute and measurement names, null and non-null strings) round-trips with `recordDifference` empty and `sameContent` true.
- [ ] Unknown name code, unknown provider code, an oversized resolution count and a stray byte after the last resolution each decode as Corrupt with the texts above; *out stays untouched.
- [ ] `tst_result_store`'s `writesOnOkInstall` (`sameContent(read.record->result, *exported)`, ~427) passes.

**Complexity:** M

---

### Task 1.5: Engine tests (`tests/tst_calcengine_restore.cpp`)

**Purpose:** Pin the resolutions, the new stale check and the drop/teardown reporting at engine level with private registries.

**Files to modify:**
- `tests/tst_calcengine_restore.cpp`

**Technical Approach:** Follow the file's idioms: `Registry` / `Session` / `Pair` worlds (~128-179), `describeEvents`, literal expectations, `init()`/`cleanup()` global-registry checks. Update the file header comment to mention resolutions and registry drops. Add a `describeResolutions(const QList<StoredResolution> &)` helper giving, per entry, the name (`key`, or `sensor/name`), a space, the provider word, and for a Calculation a space, the instance id, a space and the result version: e.g. `"EA2 Calculation expA "` (empty version), `"S/m SessionData"`, `"A Nothing"`. Expected lists below are written in that form.

New local calculations (next to `altIn()`):
- `readsX()`: Explicit; input attr `X`; output `RX` = X. (In the shared world with empty A, B, C: sum and fallbackX are MissingInput, constX wins, X = -1.)
- `pxFrom(id, version)`: OnDemand; input attr `PA`; output `PX` = PA + 1; `resultVersion = version`.
- `readsPX()`: Explicit; input attr `PX`; output `RPX` = PX.
- `readsE1()`: Explicit; input attr `E1`; output `RE1` = E1 (for the tangle world).
- `unrelated()`: OnDemand; no inputs; output `UNRELATED_OUT` = 1.
- `constantX(id)`: OnDemand; no inputs; output `X` = -1.
- `familyForEaIn()`: a `CalculationFamily` "famEA" (OnDemand) whose instantiate accepts only `attr("EA_IN")` (instance key "k", no inputs, output EA_IN = 0).
- `passConversion()`: a source-conversion family "conv" accepting any measurement: instance key `sensor + "/" + name`, inputs `sourceMeasurement` and `sourceUnit`, output the measurement, compute passes samples and unit through.

Changes and additions:

1. `resolutionCodes()` (new): the Task 1.1 criteria - provider codes and inverse, unknown codes, `storedResolutionLess` cases, and `!sameContent` for snapshots differing only in one resolution's result version (build two `StoredCalculationResult`s by hand).

2. `exportResolutions()` (new), in a `Pair` with `S/m = {1,2,3}` in the source state and `PA` unset:
   - expA: `["EA_IN SessionData"]`.
   - expB (after expA): `["EA2 Calculation expA ", "EA_IN SessionData", "EB_IN SessionData"]`.
   - withPref: `["EA_IN SessionData"]` (a preference is a leaf, not a resolution).
   - measExplicit (no source conversion): `["S/m SessionData"]`.
   - readsX: `["A Nothing", "C Nothing", "X Calculation constX "]` (sum stops at A, fallbackX at C).
   - Each list is strictly increasing under `storedResolutionLess`.

3. `ringIsNeverStored()` (new):
   - Private registry with `Synthetic::registerTangleWorld` + `readsE1`; `request("readsE1")` is Ok with `RE1 == 101`; `exportResult("readsE1")` is nullopt; engine counters unchanged by the export (`EngineCounters`).
   - Source registry with only `tangle("oQ")`, `tangle("eA")`, `readsE1`: request, export (has a value; resolutions `["E1 Calculation eA ", "OX Calculation oQ "]`). Restore into a session on the tangle registry: `Stale`, `Resolutions`, status NotRequested, no run, no event, `RE1` unavailable afterwards.

4. `restoreStaleChecks_data()/restoreStaleChecks()` (modify, keep the function name - it is in `tests/acceptance_map.txt`):
   - Row `leaves` (target stores EA2, expB) is renamed `stored instead of calculated` (`storedInsteadOfCalculated`) and now expects `StaleCheck::Resolutions` (EA2 resolves to SessionData instead of expA). Keep it chained.
   - New row `leaves`: expA; `snapshot->leaves.append(GraphNode::storedAttribute("ZZ"))` -> `Leaves`.
   - New row `resolutions`: expA; `snapshot->resolutions.first().provider = StoredResolution::Provider::Nothing` -> `Resolutions`.
   - The existing assertions (nothing installed, no run, `invalidated` contains the output for post-gathering checks) apply to all rows.

5. `restoreAcrossRegistries_data()/restoreAcrossRegistries()` (new): the cross-run analogue with a source registry and a separate target registry. Columns: `row` (QString), `id` (QString), `kind` (int), `check` (int). Both sessions get `PA = 3` and `S/m = {1,2,3}` (unit "u"). Source registrar: `registerStandard` + `pxFrom("px1", "v1")` + `readsPX` + `readsX`. "As source" below means these same registrations, in this order. Target registrar by row (switch on `row` in the body, like `restoreStaleChecks`):

   | Row | Target registrations | id | Expected |
   |---|---|---|---|
   | same registrations | as source | expA | Restored |
   | unrelated registration first | `unrelated()` first, then as source | readsPX | Restored |
   | losing candidate for a looked-up name | as source + `altIn()` (the stored EA_IN still wins) | expA | Restored |
   | new winning candidate, same inputs | `pxFrom("px0", "v1")` first, then as source | readsPX | Stale / Resolutions |
   | provider result version | as source but `pxFrom("px1", "v2")` | readsPX | Stale / Resolutions |
   | source conversion registered | as source + `passConversion()` | measExplicit | Stale / Resolutions |
   | resolutions and leaves differ | `constantX("x0")` first, then as source | readsX | Stale / Resolutions |

   For Restored rows: `runCount(id) == 0`, `totalRunCount() == 0`, `sameContent(*target.exportResult(id), *snapshot)`. For Stale rows: nothing installed (`isNotRun`, not Done, output unavailable), no event. For the three rows that must differ only in resolutions (`new winning candidate`, `provider result version`, `source conversion registered`), a third session `probe` on the target registry requests `id` and exports it: `probe.leaves == snapshot->leaves`, `probe.inputFingerprint == snapshot->inputFingerprint`, `probe.resolutions != snapshot->resolutions` - this is the spec's "a new candidate ... that computes from the same inputs". For `resolutions and leaves differ`, the probe's leaves differ from the snapshot's (`["X"]` vs `["A", "C", "X"]` as stored attributes) and the reported check is still `Resolutions`.

6. `droppedByRegistryChange()` (new), on `Pair` worlds (the private registry is `w.reg.registry`):
   - (a) expA installed, events taken, `log` cleared; `registerCalculation(altIn())` -> events `["DroppedByInputChange expA Ok"]`, `log == ["names", "explicit expA"]`, `resultStatus("expA")` nullopt, `exportResult` nullopt.
   - (b) expA requested again (EA1 == 5); `unregister("altIn")` -> `["DroppedByInputChange expA Ok"]`.
   - (c) expA requested; `registerFamily(familyForEaIn())` -> `["DroppedByInputChange expA Ok"]`; expA requested; `unregister("famEA")` -> same event.
   - (d) target session with `S/m`: measExplicit requested (MS == 3); `registerSourceConversion(passConversion())` -> `["DroppedByInputChange measExplicit Ok"]`.
   - (e) fresh `Pair`: expA and expB requested; `unregister("expA")` -> exactly two events, as a set `{"DroppedByInputChange expA Ok", "DroppedByInputChange expB Ok"}`.
   - (f) fresh `Pair`: expA requested, events and log cleared; `registerCalculation(unrelated())` then `unregister("unrelated")` -> no event, `log` empty, `resultStatus("expA") == Ok`, `exportResult("expA")` has a value.

7. `noDropEventWithoutInputChange()` (modify, keep the name - acceptance map item 310): keep the `clear()` and engine-destruction parts. Replace the registry part: register `altIn()` before requesting (nothing installed, no event), request expA (Ok, EA1 == 5; events taken), then `unregister("altIn", CalculationRegistry::Removal::Teardown)` -> `resultStatus("expA")` nullopt and no event. Update the comment: teardown removals, `clear()`, the registry's and the engine's destruction report nothing; runtime registry changes are covered by `droppedByRegistryChange`.

8. `registryDestructionReportsNothing()` (new; release only, the `#ifndef QT_NO_DEBUG QSKIP(...)` pattern of `tst_calcengine_safety.cpp` ~747): a heap `Registry` (standard world), a `FakeSessionState` with EA_IN = 4 and an engine on it with an explicit-result listener; request expA (one Installed event, taken); reset the registry (`~CalculationRegistry` calls `registryDestroyed()`); no event, `resultStatus("expA")` nullopt, `exportResult("expA")` nullopt; then destroy the engine (it must not touch the dead registry).

9. Register every new slot in the class declaration (~261-290).

**Acceptance Criteria:**
- [ ] All tests above exist with these names and pass in `build-phase1` Release.
- [ ] Every pre-existing function of the file keeps its name (the acceptance map references `restoreIntoFreshEngine`, `exportOnlyInstalledOk`, `restoreNotFound`, `restoreStaleChecks`, `installedOnSyncAndAsync`, `installedForEveryStatus`, `droppedByInputChange`, `droppedByPreferenceAndSource`, `noDropEventWithoutInputChange`, `droppedByRequestOfUpstream`, `restoreNaNPayloadAndSignedZero`, `sameContentBitExactAttributes`, `fingerprintKnownAnswer`, `fingerprintCanonicalForms`, `leafKindCodes`, `exportLeaves`, `restoreChain`, `restoreIgnoresAttributeType`, `restoredResultInvalidatesLikePublished`, `restoreBeatsOutstandingTicket`, `restoreNeverReplaces`, `restoreRejection`, `restoreIsNotAnInstall`).
- [ ] The global registry is untouched and no engine stays enrolled (`cleanup()` passes after every function).

**Complexity:** L

---

### Task 1.6: Tests outside the engine test

**Purpose:** Keep the suite green under the new rules and pin the fusion, altitude-marker and record-codec parts.

**Files to modify:**
- `tests/tst_result_records.cpp` - codec section 11.
- `tests/tst_result_store.cpp` - `noDeleteWithoutInputChange` part (b).
- `tests/tst_fusion_session.cpp` - `restoredFitIsIndistinguishable`.
- `tests/tst_builtins_engine.cpp` - altitude-marker teardown.

**Technical Approach:**

1. `tst_result_records.cpp`:
   - `recordDifference()` (~208-270): compare `resolutions` after the leaves (count, then per index name type, strings, provider, instance id, result version, with `sameStringDifference` for strings so null vs empty differences are reported as the other string fields are).
   - `sampleSnapshot()` (~120): add three resolutions in sorted order: `{attr "_JUMPER_MASS", SessionData}`, `{attr "_SCHEMA", Nothing}`, `{meas IMU/wx, Calculation, "builtin.conversion.default#IMU/wx", "v7"}` (so `roundTripIsBitExact` and every writer test carry them).
   - `layoutIsPinned()` (~774-822): give `r` two resolutions `{attr "a", SessionData}` and `{meas S/t, Calculation, "c", "v"}` and append to `expectedPrefix`, after the outputs:
     ```
     "02000000"                                  // 11 two resolutions
     "01" "02000000" "6100" "FFFFFFFF"           // 11a attribute "a", second null
     "01" "FFFFFFFF" "FFFFFFFF"                  //     SessionData, id and version null
     "02" "02000000" "5300" "02000000" "7400"    // 11a measurement "S" "t"
     "02" "02000000" "6300" "02000000" "7600"    //     Calculation "c", version "v"
     ```
     and update the comment ("... format version 1 with the resolutions of plan stored-results-validity, Phase 1").
   - `corruptInputIsRefused_data()` (~911-918): the row `bytes after the last output` becomes `bytes after the last resolution`: body `s << quint32(0) << quint32(1) << quint8(1) << "a" << QString() << false << quint32(0) << quint8(0);`, error "unexpected bytes after the last resolution". New rows (leaves 0, outputs 0, then): `unknown resolution name kind` (`quint32(1) << quint8(3) << "a" << QString() << quint8(1) << QString() << QString()`, "unknown resolution name kind 3"); `unknown resolution provider` (`quint32(1) << quint8(1) << "a" << QString() << quint8(7) << QString() << QString()`, "unknown resolution provider 7"); `resolution count too large` (`quint32(1000)`, "resolution count"). The other crafted rows fail before section 11 and keep their texts.
2. `tst_result_store.cpp` `noDeleteWithoutInputChange()` (~704-741), part (b) only: a registration that drops the result now deletes its record (Phase 3 tests that end to end), so (b) becomes "a registry change that does not reach the result drops neither it nor the record": register `constantCalculation(kShadow, QStringLiteral("_STORE_SHADOW"), 0)` (an output expA never looks up), `QCOMPARE(engine("s1").resultStatus(kExpA), Ok)` after the registration, unregister, flush, bytes unchanged, `droppedRecordsDeleted == 0`. Update the comment. Parts (a) and (c) unchanged. Keep the function name (acceptance map items 310, 325).
3. `tst_fusion_session.cpp` `restoredFitIsIndistinguishable()` (~956-1063), step 2 after the leaf assertions:
   - `snapshot->resolutions` is non-empty and strictly increasing under `storedResolutionLess`.
   - it contains `{meas Local/north, Calculation, "builtin.local.coordinates", ""}` and `{meas IMU/az, Calculation, "builtin.conversion.default#IMU/az", ""}` (az is not schema-dependent, so only the default conversion accepts it);
   - every `Attribute` / `Measurement` input of the fit's descriptor (`CalculationRegistry::instance().instance(kFit)->descriptor->inputs`) has an entry;
   - no entry has instance id `kFit`.
   Step 4: `QVERIFY(again->resolutions == snapshot->resolutions)` next to the existing `sameContent` check. Everything else in the test is unchanged (step 5's `Fingerprint` stays: changing a sample of az changes no resolution).
4. `tst_builtins_engine.cpp`: new `altitudeMarkerTeardownReportsNothing()`, modelled on `fingerprintSurvivesRuntimeAltitudeMarker()` (~622-658) on the global registry:
   - `TestEnvironment::instance().registerBuiltIns()`; note `registeredIds()`; `writeAltitudes({1000})`; create the manager; set `AltitudeMarkersUnits` to "Metric"; `refresh()`.
   - Register, with a `qScopeGuard` declared before the engine that unregisters it, an Explicit calculation `test.readsAltitude`: input attr `_ALTITUDE_1000_M`, output `_TEST_READS_ALTITUDE` = that value.
   - A `FakeSessionState` (empty) and a `CalculationEngine` on the global registry with an explicit-result listener recording `"<kind> <id> <status>"`.
   - `request("test.readsAltitude")` is `MissingInput` (no GNSS data: the altitude calculation cannot run); one `Installed` event.
   - `writeAltitudes({})` (the manager refreshes and removes the marker with `Change`) -> exactly one `DroppedByInputChange test.readsAltitude MissingInput`.
   - `writeAltitudes({1000})`; request again (Installed); clear the events; `manager.reset()` -> no event and `resultStatus("test.readsAltitude")` nullopt.
   - End: engine destroyed, test calculation unregistered, `writeAltitudes({})`, `registeredIds()` equal to the start.

**Acceptance Criteria:**
- [ ] The four files' tests pass; no existing test function is renamed or removed.
- [ ] Full suite green: labels core, fusion, exact, python, audit.

**Complexity:** M

## Testing Requirements

### Unit Tests
- New in `tst_calcengine_restore`: `resolutionCodes`, `exportResolutions`, `ringIsNeverStored`, `restoreAcrossRegistries`, `droppedByRegistryChange`, `registryDestructionReportsNothing`.
- Modified in `tst_calcengine_restore`: `restoreStaleChecks` (rows `storedInsteadOfCalculated`, `leaves`, `resolutions`), `noDropEventWithoutInputChange` (teardown removal).
- New in `tst_builtins_engine`: `altitudeMarkerTeardownReportsNothing`.
- Modified in `tst_result_records`: `layoutIsPinned`, `corruptInputIsRefused` rows, `recordDifference`, `sampleSnapshot`.
- Modified in `tst_result_store`: `noDeleteWithoutInputChange` part (b).
- Modified in `tst_fusion_session`: `restoredFitIsIndistinguishable`.

### Integration Tests
- Full suite in `build-phase1`: `cmake --build build-phase1 --config Release` then `ctest --test-dir build-phase1/FlySightViewer-build -C Release --output-on-failure`. The store and fusion-store tests (`tst_result_store`, `tst_result_columns`, `tst_fusion_store`) must pass unchanged apart from the part (b) edit: their records now carry resolutions and restore as before; their registry changes are made with no requested result reaching them or with the session evicted.
- The cleanup audit (label `audit`) must pass: no test function the acceptance map names is renamed; no new rule is needed in this phase.

### Manual Verification
- Optional, on a scratch logbook with `build-phase1`'s application: fit a session, restart, confirm the fit is restored with no job (records now carry resolutions); add and remove an altitude marker in the preferences while the fit is shown, confirm the fit stays; close the application and confirm the record file in `cache/` is still there.

## Notes for Implementer

### Gotchas
- `QString()` (null) and `QString("")` compare equal, but `QDataStream` writes them differently (`FFFFFFFF` vs `00000000`). The pinned layout test fixes which one the fixture uses; do not normalize strings in the codec.
- Build `StoredResolution::name` only through `DependencyKey::attribute()` / `measurement()`; a default-constructed key has an unset `type`.
- `closureOf()` must collect a `Resolution` node even when it has no `m_dependsOn` entry (a node that looked at nothing is impossible for an attribute, but the defensive nullopt of `storedResolutions()` relies on seeing every visited node).
- The result version of a Calculation provider comes from the cached `Result` entry's descriptor, not from `m_registry->instance(...)`: a family instance needs the name to be looked up, and the cached descriptor is the one the answer was computed with.
- `flushPending()` must run before returning any post-gathering stale outcome (as the existing Leaves/Fingerprint returns do), so that drops queued by `dropNotRequested()` are delivered.
- Keep `ExplicitResultEvent::Kind` unchanged: tests and the store switch on `Installed` vs everything else, and `tst_fusion_session`'s `eventTexts` maps non-Installed to the text "DroppedByInputChange".
- A test that unregisters a calculation while a `SessionModel` with installed requested results is alive now deletes their records. `tests/README.md` §8 already requires destroying the model before unregistering test calculations; if a suite fails because a fixture breaks that order, fix the order, do not switch the fixture to `Removal::Teardown`.
- `tst_calcengine_restore`'s `Registry` must be declared before the sessions that use it (a registry must outlive its engines), except in `registryDestructionReportsNothing`, which destroys it on purpose and runs only in release builds.

### Decisions Made
- **D1 - The record codec carries the resolutions in this phase, under format version 1.** The overview assigns the record format to Phase 3, but the `Resolutions` check makes every record decoded without resolutions stale, so without the codec change every restore in `tst_result_store`, `tst_result_columns` and `tst_fusion_store` would fail and the suite could not end green. Section 11 is appended after the outputs (the least disturbance to the pinned layout and to the crafted corrupt payloads). The format version is not bumped here: it changes once, in Phase 3, when the environment stamp leaves the record and the layout is final (format 2); nothing is pushed between phases, and a record of the earlier version-1 layout decodes as Corrupt and is deleted as stale at load, the treatment spec section 8 gives any earlier format. **Hand-off to Phase 3:** keep section 11 as specified here, remove item 4 (environment), bump to 2, re-pin `layoutIsPinned`, update `craft()`'s header (version, no environment) and `futureVersionIsRefused`'s rows.
- **D2 - Encoding.** Provider codes 0 Nothing, 1 SessionData, 2 Calculation; the record's name codes are its output key codes (1 attribute, 2 measurement); order by name kind, then first, then second string, case-sensitive UTF-16. Stored attribute and passthrough source data are both `SessionData` (the name's type already tells them apart); a source conversion is a `Calculation` (its instance id), so registering the first source conversion changes every measurement with source data from SessionData to Calculation. Non-Calculation entries carry empty instance id and result version. `resolutions` participates in `sameContent()`.
- **D3 - Runtime registry drops reuse `DroppedByInputChange`.** The overview binds "the same drop event as an input change", spec section 5 says "exactly like an input change", and the store already deletes on that event; a new kind would change nothing the store does and would ripple through the tests' event texts, the docs and the acceptance map. Only the enumerator's documentation widens.
- **D4 - Teardown is explicit.** `CalculationRegistry::unregister(id, Removal::Teardown)` marks a removal by an owner being destroyed; the altitude-marker manager's destructor uses it. Today's application already destroys every engine before that destructor runs (QObject child order in `MainWindow`), but that is an accident of creation order; the explicit mark makes "shutdown deletes nothing" hold whatever the order. `registryDestroyed()`, engine destruction and `clear()` already report nothing.
- **D5 - A result whose evaluation met a dependency ring is not exported.** With a ring, a looked-up name may have been answered provisionally (uncached, or re-evaluated past a cached answer), so the graph cannot state what provided it. Such results are registration errors (warned about on every detection); none exists among the built-ins or the fit. Restoring into an engine where the gathering meets a ring is `Stale/Resolutions`.
- **D6 - `Leaves` becomes a tamper check in practice.** With identical resolutions the gathering takes identical paths, so the leaf set can differ only when the snapshot itself was altered; a state change that alters a lookup (the old `leaves` row) now reports `Resolutions`. The check stays, per the overview.
- **D7 - `tst_result_store` part (b)** is minimally rewritten here to an unrelated registration; the store-level test that a runtime registry drop deletes the record (spec section 10, second bullet) is Phase 3's, as is the rewrite of the environment rows.

### Open Questions
- None blocking. The coordinator should confirm D1 (codec change in this phase, version bump deferred to Phase 3) when writing Phase 3's document.

## Definition of Done

This phase is complete when:
1. All tasks have passing acceptance criteria.
2. The full suite passes in `build-phase1` (Release): labels core, fusion, exact, python, audit.
3. Code follows the patterns of the reference files (pinned codes and order as for the leaves, private-registry tests, literal expectations).
4. No TODOs or placeholder code remain; every comment that described registry changes as unreported (engine header and source, store, altitude manager) is updated, except `sessionmodel.cpp` ~1376, which Phase 3 removes with its code path.
