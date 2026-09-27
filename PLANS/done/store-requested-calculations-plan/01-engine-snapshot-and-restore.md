# Phase 1: Engine snapshot and restore

## Overview

The calculation engine learns three things. It can **export** the installed
`Ok` result of a plain Explicit calculation as a plain value
(`StoredCalculationResult`): the bundle, the detail, the result version, the
sorted dependency leaves it reached and a SHA-256 input fingerprint. It can
**restore** such a snapshot into an engine as though it had just been
published: same edges, same status and detail, no compute, no ticket. It
**tells a listener** when an Explicit result is installed by a request or a
publish, and when one is dropped by an input change. Descriptors gain an
optional per-calculation result version, and sensor fusion declares its kernel's
algorithm string as that version. The record format (Phase 2) and the store
(Phase 3) are built on these pieces. This phase changes no product behaviour:
nothing in `src/` outside the engine calls the new API yet.

## Dependencies

- **Depends on:** None. Can begin immediately on branch
  `store-requested-calculations` (from `fusion-improvements`, 8dc4e38).
- **Blocks:** Phase 2 (the record format serialises `StoredCalculationResult`
  and uses the leaf kind codes defined here), Phase 3 (the store calls
  `exportResult`, `restoreResult` and the explicit-result listener), and
  through them Phases 4 and 5.
- **Assumptions:** The engine is as described in
  `00-overview.md` "Key Patterns & References" (line numbers below are from
  8dc4e38 and are guides). Only one Explicit calculation exists in product code
  (`builtin.fusion.fit`). The test fakes' explicit world (`expA` → `expB`) is
  available in `tests/support/fakesessionstate.h`.

## Tasks

### Task 1.1: Result version on descriptors; fusion declares its algorithm string

**Purpose:** Give every registration an optional code stamp for its results,
declare fusion's without duplicating the kernel's literal, and state the new
bump rule where `CalculationCompatibilityVersion` is defined.

**Files to modify:**
- `src/engine/calculationdescriptor.h`: new field `resultVersion`.
- `src/fusion/fusion.h`: new public constant `Fusion::Algorithm`.
- `src/fusion/fusionoutput.cpp`: the local `kAlgorithm` (line 18) is deleted and
  its two uses (181, 210) use `Algorithm`.
- `src/fusion/fusionregistration.cpp`: `registerFit()` (195-205) sets the
  result version.
- `src/calculations/builtincalculations.h`: the bump-rule comment (19-38) gains
  the result-version clause.

**Technical Approach:**

1. `CalculationDescriptor` gets a new **last** member (after `compute`, so no
   positional initialisation can shift; none exists today):
   ```cpp
   QString resultVersion;
   ```
   Doc comment, in the header's style: optional; identifies the arithmetic of
   the calculation's results for results stored beside the session. A stored
   result is used only while the version it was stored with equals this one.
   Change it whenever a code change can alter what the calculation produces
   from the same inputs. Empty means none is declared. The engine treats it as
   opaque text: it never affects evaluation, identity, candidate order or the
   environment fingerprint (`calculationEnvironmentFingerprint()` is **not**
   changed). A family's `instantiate` may set it on the descriptors it returns;
   the registry keeps what it is given.
2. `src/fusion/fusion.h` (GTSAM-free public header), inside
   `namespace FlySight::Fusion`, next to `run()`:
   ```cpp
   inline constexpr char Algorithm[] = "batch-temperature-bias-v3";
   ```
   Doc comment: names the model and arithmetic of `run()`. It is written as
   `"algorithm"` in every diagnostics object and declared as the result version
   of `builtin.fusion.fit`, so changing it drops every stored fit. Change it
   whenever a change can alter what `run()` returns for the same channels.
3. `src/fusion/fusionoutput.cpp` is in `namespace FlySight::Fusion::Detail`,
   so it can name `Algorithm` unqualified (it already includes `fusion/fusion.h`
   through `fusionoutput.h`). Delete `const char kAlgorithm[] = ...;` and
   replace both `{"algorithm", kAlgorithm}` with `{"algorithm", Algorithm}`.
   The literal then appears exactly once in `src/`.
4. `registerFit()`: `d.resultVersion = QString::fromLatin1(Fusion::Algorithm);`
   (`fusion/fusion.h` is already included). `builtin.fusion.accH` and
   `builtin.fusion.systemTime` declare none.
5. `builtincalculations.h`: after the paragraph "Bump whenever a code change can
   alter the value that ANY existing session yields for ANY logbook column: ...",
   add a paragraph with this content (wording may be adjusted to fit the
   comment, the meaning may not):
   > Bump it, or the calculation's result version
   > (`CalculationDescriptor::resultVersion`), whenever a change can alter what
   > a requested calculation produces. A stored result of an explicit
   > calculation is used only while this marker, the environment fingerprint
   > and the result version it was stored with all equal the current ones.
   > Bumping a result version drops the stored results of that calculation
   > only; bumping this marker drops every stored result and every cached
   > column value.

   Two audit rules apply to this comment. It must not contain the text
   `CalculationCompatibilityVersion =` (the "one authority: compatibility
   marker" count is 1 line), and it must not contain `fusion/` or `Fusion::`
   ("nobody but the application references the fusion library" covers
   `src/calculations`). Do not name the fusion constant there.

**Acceptance Criteria:**
- [ ] `CalculationDescriptor::resultVersion` exists, defaults to empty, and is
      not read by `calculationEnvironmentFingerprint()` (the existing
      `tst_logbook_index` / `tst_column_cache` environment tests pass unchanged).
- [ ] `git grep -n "batch-temperature-bias-v3" -- src` returns exactly one line,
      in `src/fusion/fusion.h`.
- [ ] `registry.instance("builtin.fusion.fit")->descriptor->resultVersion ==
      "batch-temperature-bias-v3"`; `builtin.fusion.accH` and
      `builtin.fusion.systemTime` have empty result versions (Task 1.7,
      `registrationShape`).
- [ ] The golden tests (`tst_fusion_golden`, `tst_fusion_kernel`) still pass:
      the diagnostics JSON is byte-identical.
- [ ] `builtincalculations.h` carries the clause, and the cleanup audit passes.

**Complexity:** M

---

### Task 1.2: The snapshot type and the input fingerprint

**Purpose:** Define the value type exchanged between engine and store (the
Phase 1/Phase 2 contract), the pinned leaf kind codes, the leaf order, and the
canonical fingerprint encoding.

**Files to create:**
- `src/engine/storedcalculationresult.h`: type, leaf helpers, fingerprint
  functions, comparison.
- `src/engine/storedcalculationresult.cpp`: implementation.

**Files to modify:**
- `src/CMakeLists.txt`: add `engine/storedcalculationresult.cpp
  engine/storedcalculationresult.h` to `flysight_model` (lines 237-253), and
  **move** `csvformat.cpp csvformat.h` from `flysight_core` (line 272) to
  `flysight_model`, with a comment saying why (see "Decisions Made"). The files
  stay where they are (`src/csvformat.*`).

**Technical Approach:**

Namespace `FlySight`. The header includes `calctypes.h`,
`calculationresult.h`, `sessionstate.h`, `<QByteArray>`, `<QList>`,
`<optional>`. Only the `.cpp` includes `"../csvformat.h"` and
`<QCryptographicHash>` / `<QtEndian>`.

**The type** is a plain value type with public members (copyable and movable;
all members implicitly shared, so a copy can be handed to another thread for
encoding while the engine keeps its own):

```cpp
struct StoredCalculationResult {
    CalculationId      calculationId;     // plain registration id (== instance id)
    QString            resultVersion;     // descriptor's resultVersion at publish; may be empty
    QString            detail;            // == bundle.reason() (restore checks this)
    CalculationResult  bundle;            // the installed bundle, outputs in its order
    QList<GraphNode>   leaves;            // sorted, unique; leaf kinds only
    QByteArray         inputFingerprint;  // SHA-256, InputFingerprintSize raw bytes
};
inline constexpr int InputFingerprintSize = 32;
```

The status is not a member: only `Ok` results are exported. Phase 2 reads the
bundle through `CalculationResult`'s public API: `setOutputs()` (order),
`isAvailable()`, `attributeValue()`, `measurementValues()`,
`measurementUnit()` and `reason()`. It rebuilds the bundle in the same order
with `setAttribute` / `setMeasurement(values, unit)` for an available output,
`setUnavailable(key)` for an unavailable one, then `setReason(detail)`. This
round-trips exactly, because "available" is equivalent to "valid QVariant /
non-empty samples" (`calculationresult.cpp` 15-43). Document this in the
header comment.

**Leaves.** Each is a `GraphNode` of kind `StoredAttribute` (`a` = key),
`SourceMeasurement` or `SourceUnit` (`a` = sensor, `b` = measurement name) or
`Preference` (`a` = key). `b` is empty for attribute and preference leaves, and
`measurementName` is always false. The helpers pin the wire codes so that
Phase 2 never serialises the enum's underlying value directly:

```cpp
bool isStoredLeafKind(GraphNode::Kind kind);
/// 0 StoredAttribute, 1 SourceMeasurement, 2 SourceUnit, 3 Preference. Pinned:
/// part of the fingerprint encoding and of the record format.
quint8 storedLeafKindCode(GraphNode::Kind kind);          // asserts on a non-leaf kind, returns 0xFF
/// The leaf for a code read back from a record; nullopt for an unknown code.
std::optional<GraphNode> storedLeafFromCode(quint8 code, const QString &a, const QString &b);
/// The order of StoredCalculationResult::leaves: by kind code, then a, then b,
/// each compared with QString::compare (case-sensitive, UTF-16 code units).
bool storedLeafLess(const GraphNode &lhs, const GraphNode &rhs);
```

**Fingerprint encoding** (version 1). The fingerprint is SHA-256
(`QCryptographicHash::Sha256`) over this byte string. All integers are
little-endian. `str(s)` means a u32 byte count followed by `s.toUtf8()` with
no terminator.

| Part | Bytes |
|------|-------|
| magic | the 18 ASCII bytes `flysight-inputs-v1` followed by one `0x00` byte (19 bytes) |
| leaf count | u32 |
| then per leaf, in `leaves` order: kind | u8, `storedLeafKindCode` |
| `a`, `b` | `str(a)`, `str(b)` |
| present | u8, 1 or 0 |
| value (only when present) | by kind, below |

The present flag and value by kind:

- **StoredAttribute:** present = `state.hasStoredAttribute(a)`. Value:
  `t = CsvFormat::formatAttributeValue(state.storedAttribute(a))`. If `t` has a
  value, u8 `1` then `str(*t)`. Otherwise u8 `0` (an invalid or unrepresentable
  value). This is the session file's text, so an attribute that reloads as a
  `QString` gives the same bytes as the `double` / `qlonglong` it was set as.
- **SourceMeasurement:** present = `state.hasSourceMeasurement(a, b)`. Value:
  u64 sample count, then each sample as its u64 IEEE-754 bit pattern.
  Every NaN (`std::isnan`), whatever its sign and payload, is written as
  `0x7FF8000000000000`. `-0.0` stays `0x8000000000000000`, distinct from
  `+0.0`, and infinities keep their bits. A present measurement with no
  samples writes count 0.
- **SourceUnit:** present = `state.hasSourceMeasurement(a, b)` (the unit exists
  exactly when the measurement does, as in `readLeafInput`,
  `calculationengine.cpp` 535-541). Value: `str(state.sourceUnit(a, b))`.
- **Preference:** `v = preferences ? preferences->preferenceValue(a) :
  QVariant()`, present = `v.isValid()`. Value: as for an attribute, u8 `1` +
  `str(*formatAttributeValue(v))`, or u8 `0` when it returns nullopt. This is
  the same text form `calculationEnvironmentFingerprint()` uses for
  preferences (`builtincalculations.cpp` 69-77).

Functions:

```cpp
/// The canonical byte string above (exposed for the known-answer test).
QByteArray inputFingerprintEncoding(const QList<GraphNode> &leaves, const ISessionState &state,
                                    const IPreferenceProvider *preferences);
/// SHA-256 of inputFingerprintEncoding(): InputFingerprintSize raw bytes.
QByteArray inputFingerprint(const QList<GraphNode> &leaves, const ISessionState &state,
                            const IPreferenceProvider *preferences);
/// Every member equal: id, version, detail, leaves, fingerprint bytes; the
/// bundle's setOutputs() order and reason; per output availability, attribute
/// (bit-exact: metatype, then double/float bits, else QVariant ==; coordinator fixup cfcc236), samples by bit pattern (NaN == NaN, -0 != +0) and unit.
/// The rule of CalculationEngine::sameValue(), applied to every output.
bool sameContent(const StoredCalculationResult &a, const StoredCalculationResult &b);
```

Both fingerprint functions encode `leaves` in the order given. The engine
always passes the sorted list. They are pure reads of `state` and
`preferences`, following the `ISessionState` contract.

Header comment (the Phase 1/2 contract, stated once): what each member means;
that the code stamps (`CalculationCompatibilityVersion`, environment
fingerprint) are deliberately absent because the engine layer does not depend
on `src/calculations/`, and the store adds them; that the leaf codes and the
encoding are pinned, and that changing the encoding means a new magic
(`-v2`), which makes every stored fingerprint mismatch (conservative: records
go stale, nothing is misread).

**Acceptance Criteria:**
- [ ] `flysight_model` builds with the two new files and `csvformat.cpp`;
      `flysight_core` no longer lists `csvformat.cpp`; `tst_csvformat` and the
      Python bridge target still build and pass.
- [ ] Known-answer test (Task 1.6, `fingerprintKnownAnswer`):
      `inputFingerprintEncoding` equals a hand-written byte literal for the
      documented example, and `inputFingerprint` equals a literal lower-case
      hex SHA-256 computed independently of the code under test (for example
      with Python's `hashlib` over the same bytes; the command goes in a comment).
- [ ] Two NaNs with different payloads/signs give equal fingerprints; `-0.0`
      and `+0.0` give different ones; a stored attribute `QVariant(1.5)` and
      `QVariant(QStringLiteral("1.5"))` give equal ones; an absent attribute and
      one present with value `""` give different ones (Task 1.6,
      `fingerprintCanonicalForms`).
- [ ] `storedLeafKindCode` / `storedLeafFromCode` round-trip the four leaf kinds
      with codes 0-3, and `storedLeafFromCode(4, ...)` is nullopt.

**Complexity:** M

---

### Task 1.3: Export: the leaf closure and `exportResult`

**Purpose:** Let the store obtain the snapshot of an installed Explicit result,
with the leaves it reached (absent ones included) and their fingerprint.

**Files to modify:**
- `src/engine/calculationengine.h`: include `storedcalculationresult.h`, new
  public section "stored results", private helper.
- `src/engine/calculationengine.cpp`: implementation (a new section after
  "Asynchronous request").

**Technical Approach:**

Private helper:

```cpp
/// Every leaf reached from `direct` through the recorded edges: breadth-first
/// over m_dependsOn, following Resolution and Result nodes (explicit results
/// included) and collecting StoredAttribute / SourceMeasurement / SourceUnit /
/// Preference nodes; Prepared nodes are ignored. Sorted by storedLeafLess,
/// unique. Const; touches the edge maps only.
QList<GraphNode> leafClosure(const QSet<GraphNode> &direct) const;
```

Absent leaves need no special handling. The engine records a leaf **before**
it checks it: `resolve()` notes `storedAttribute(key)` / `sourceMeasurement`
whether or not the value exists (`calculationengine.cpp` 349-350, 362), and
`gatherInputs()` notes preference and source leaves "available or not"
(573-586). Absent leaves are therefore already edge targets, and the closure
collects them. Their absence is expressed by the fingerprint's present flag.
Provisional (uncached) intermediates handed up their `looked` sets to the
nearest cached ancestor (`handUp`, 256-267), so their leaves are direct edges
of that ancestor. A noted but uncached Resolution/Result node simply has no
`m_dependsOn` entry, and the walk continues past it.

While a result is installed, every cached node below it is still cached with
the same edges, because invalidating any of them would have dropped the result
through the reverse edges. The closure at export is therefore the closure at
publish time.

Public API:

```cpp
// ---- stored results (see storedcalculationresult.h) ---------------------
/// The installed result of the plain explicit calculation `id` as a snapshot,
/// or nullopt when there is none to store: unknown id, a family (or family
/// instance) id, an on-demand calculation, no registry, nothing cached, or a
/// cached status other than Ok (NotRequested, MissingInput, Cycle, Failed,
/// UndeclaredRead, InvalidOutput). Const: never resolves, never computes,
/// never changes the cache; reads the session state and the preference
/// provider for the fingerprint. May be called from an explicit-result
/// listener; not from inside a compute function.
std::optional<StoredCalculationResult> exportResult(const CalculationId &id) const;
```

Implementation:
1. Look up `m_registry->instance(id)` (empty instance output). If none, or the
   policy is not Explicit, return nullopt.
2. `m_results.constFind(GraphNode::result(instance->instanceId))`. Continue
   only if the entry exists, `requested`, `status == Ok` and `bundle` is set.
3. Fill `calculationId = instance->instanceId` and `resultVersion =
   entry.instance.descriptor->resultVersion` (the descriptor the result was
   published under; a registry change would have dropped the result, so it is
   also the current one). Fill `detail = entry.detail`, `bundle =
   *entry.bundle`, `leaves = leafClosure(m_dependsOn.value(C))`, and
   `inputFingerprint = inputFingerprint(leaves, *m_state, provider)`, where
   `provider = m_registry->preferenceProvider()`.

**Acceptance Criteria:**
- [ ] Before any request, and for `derivA` (on demand), an unknown id and a
      family id: `exportResult` returns nullopt (`exportOnlyInstalledOk`).
- [ ] After `request("expA")` with `EA_IN = 4`, the snapshot has
      `calculationId "expA"`, empty `resultVersion`, empty `detail`, bundle
      outputs `[EA1, EA2, EA_DIAG]` with values 5, 8, `"ok"`, and
      `leaves == [storedAttribute("EA_IN")]`.
- [ ] With `EA_IN = -1` (rejection) the snapshot's detail and bundle reason are
      `"negative input"`, the bundle's only output is `EA_DIAG = "rejected"`,
      and `EA1`/`EA2` are not available.
- [ ] A `Failed` result (a thrower) and a `MissingInput` result (no `EA_IN`)
      export nullopt.
- [ ] After `expA` then `expB` (`EB_IN = 10`): `leaves ==
      [storedAttribute("EA2"), storedAttribute("EA_IN"), storedAttribute("EB_IN")]`.
      `EA2` is absent from the state: an absent looked-at leaf, and a leaf
      reached through an explicit Result node.
- [ ] An explicit calculation with a preference input has
      `leaves == [storedAttribute("EA_IN"), preference("p")]`; one with a
      measurement input on source data has
      `[sourceMeasurement("S","m"), sourceUnit("S","m")]` (private registry
      without source conversions: passthrough notes both).
- [ ] `exportResult` changes nothing in the engine: `cachedNodeCount()`,
      `edgeCount()`, `totalRunCount()` and `preparedCount()` are the same
      before and after. (The state's `readCount()` grows, because the
      fingerprint reads state; the test does not assert on it.)

**Complexity:** M

---

### Task 1.4: Restore

**Purpose:** Install a snapshot as if it had just been published: gather as
`prepare()` does, validate, and install through the publish's install step,
without computing and without leaving a ticket.

**Files to modify:**
- `src/engine/calculationengine.h`: `RestoreOutcome`, `restoreResult`, two
  private helpers.
- `src/engine/calculationengine.cpp`: refactor `prepare()`,
  `requestInstance()` and `publishPrepared()` onto the helpers; implement
  `restoreResult`.

**Technical Approach:**

*Refactor first (no behaviour change), so that the two paths cannot drift:*

```cpp
/// The availability pass of an explicit request, as the root of an evaluation:
/// pushes the Result scope of `instance`, gathers its inputs into `context`,
/// pops, and hands back the closed scope. Shared by prepare() and restoreResult().
ResultStatus gatherAsRoot(const CalculationInstance &instance, EvaluationContext &context, Scope &closed);

enum class InstallOrigin { Request, Restore };
/// The one install step of a requested result: caches `entry` under C and
/// publishes the edges and provisional verdicts of `scope` (publishEdges).
/// Request: queues an Installed event (Task 1.5) when the calculation is
/// explicit. Restore: queues nothing. Used by requestInstance(), prepare()'s
/// NothingToRun path, publishPrepared() and restoreResult().
void installRequested(const GraphNode &C, const ResultEntry &entry, const Scope &scope, InstallOrigin origin);
```

- `gatherAsRoot` is the block at `calculationengine.cpp` 1132-1142 (ScopeGuard
  on `C`, `gatherInputs`, `guard.finish()`, the context-free assertion).
  `prepare()` calls it.
- `installRequested` is the pair `m_results.insert(C, entry);
  publishEdges(C, scope);`. It replaces that pair in `requestInstance()`
  (1072-1073), in `prepare()`'s NothingToRun path (1152-1153) and in
  `publishPrepared()` (1273-1274), each with `InstallOrigin::Request`. The
  surrounding `flushPending()` calls stay where they are.

*Public API:*

```cpp
struct RestoreOutcome {
    enum class Kind {
        NotFound,           ///< no registry, unknown id, or a family: nothing changed
        NotExplicit,        ///< an on-demand calculation: nothing changed
        AlreadyInstalled,   ///< a result is cached (`status`; any but NotRequested): nothing changed
        Stale,              ///< `staleCheck` failed: nothing installed
        Restored            ///< installed with status Ok
    };
    enum class StaleCheck {
        None,
        ResultVersion,      ///< snapshot.resultVersion != the descriptor's
        Bundle,             ///< an output the descriptor does not declare, or detail != bundle.reason()
        InputsUnavailable,  ///< gathering ended MissingInput or Cycle (`status`)
        Leaves,             ///< the current leaf list differs from snapshot.leaves
        Fingerprint         ///< same leaves, different input fingerprint
    };
    Kind kind = Kind::NotFound;
    StaleCheck staleCheck = StaleCheck::None;
    /// AlreadyInstalled: the cached status. Restored: Ok. Stale/InputsUnavailable:
    /// the gathering status. Otherwise NotRequested.
    ResultStatus status = ResultStatus::NotRequested;
    /// Cached names dropped because they had been read while the calculation
    /// was "not requested", exactly like PublishOutcome::invalidated. Non-empty
    /// only when gathering ran (Stale with InputsUnavailable / Leaves /
    /// Fingerprint, or Restored). The caller passes them on so consumers re-read.
    QSet<DependencyKey> invalidated;
};
/// Installs `snapshot` as the published result of its calculation, provided it
/// is still valid here. Not a request: it runs no compute function (on-demand
/// inputs are evaluated as for a fresh request, as prepare() does), counts no
/// run, creates no ticket, and queues no Installed event. On success the
/// edges, status, detail and bundle are those a fresh publish would install.
/// An outstanding ticket for the same calculation then publishes as
/// RefusedStale / AlreadyPublished. Main thread, between evaluations; from
/// inside an evaluation it asserts and returns NotFound.
RestoreOutcome restoreResult(const StoredCalculationResult &snapshot);
```

*Algorithm* (checks in this order; the first failure decides):

1. Not inside an evaluation (else assert, return default). `flushPending()`.
   Without a registry, return NotFound.
2. `instance = m_registry->instance(snapshot.calculationId)`; none → NotFound.
   Policy not Explicit → NotExplicit.
3. `C = result(instanceId)`. If a cached entry exists with status other than
   `NotRequested`, return AlreadyInstalled with that status. This is the same
   test as `prepare()` 1119-1124 and `publishPrepared()` 1246-1253, so a
   `MissingInput` cached by an earlier request also counts. Nothing changes.
4. `snapshot.resultVersion != instance->descriptor->resultVersion` → Stale /
   ResultVersion.
5. Any key of `snapshot.bundle.setOutputs()` not in
   `instance->descriptor->outputs`, or `snapshot.detail !=
   snapshot.bundle.reason()` → Stale / Bundle. Steps 4-5 touch nothing.
6. `outcome.invalidated = dropNotRequested(C)`, for the reason given in
   `dropNotRequested`'s comment (1025-1042): a nested lookup must not be served
   "not requested".
7. `EvaluationContext context(instanceId, /*quiet=*/true); Scope scope;
   status = gatherAsRoot(*instance, context, scope);`. If `status != Ok`:
   `flushPending()`, return Stale / InputsUnavailable with `status`. **Nothing
   is cached for C** (unlike `prepare()`, which caches `MissingInput` as a
   requested result): the calculation reads "not requested" again at the next
   read. Resolutions cached during gathering stay cached, as after any read.
8. `looked = scope.looked` minus `C` (as `prepare()` 1177 and
   `publishEdges()` 713-716 do). `leaves = leafClosure(looked)`. If
   `leaves != snapshot.leaves`, Stale / Leaves. Else if
   `inputFingerprint(leaves, *m_state, provider) !=
   snapshot.inputFingerprint`, Stale / Fingerprint. Either way `flushPending()`
   and return; nothing is cached for C.
9. Install: `ResultEntry entry; entry.instance = *instance; entry.requested =
   true; entry.status = Ok; entry.bundle =
   std::make_shared<const CalculationResult>(snapshot.bundle); entry.detail =
   snapshot.detail; entry.sawCycle = scope.sawCycle;` then
   `installRequested(C, entry, scope, InstallOrigin::Restore)`, `flushPending()`,
   deliver queued explicit-result events (Task 1.5; a restore can queue
   Dropped events through `dropNotRequested`, never Installed), and return
   Restored / Ok.

Never call `acceptRun()` (it counts a run, 618-620). No `PreparedCalculation`
is created, so `preparedCount()` is unchanged and `m_preparedSerial` is not
incremented.

Class-level documentation: add a "Stored results" paragraph to the
`CalculationEngine` comment (h 25-67). Export is an inspection that reads
state for the fingerprint. Restore is a publication without a run. Both are
main-thread only. Run counters count runs, and a restore is not one.

**Acceptance Criteria:**
- [ ] Refactor is behaviour-neutral: `tst_calcengine`, `tst_calcengine_async`,
      `tst_calcengine_safety`, `tst_calcengine_oracle`,
      `tst_calcengine_blockers` and `tst_jobqueue` pass unchanged.
- [ ] A snapshot of `expA` restored into a second engine (same registry,
      identical state) returns Restored / Ok. There, `attribute("EA1") == 5`,
      `resultStatus("expA") == Ok`, `resultDetail("expA")` equals the source
      engine's, `dependenciesOf(GraphNode::result("expA"))` equals the source
      engine's, and `runCount("expA") == 0`, `totalRunCount() == 0`,
      `preparedCount() == 0` (`restoreIntoFreshEngine`).
- [ ] In that engine `verifyAgainstFresh(explicitNames())` is empty (the
      restored values equal a recomputation).
- [ ] `exportResult("expA")` of the restored engine is `sameContent` with the
      original snapshot.
- [ ] Names read before the restore (`EA1`, `DA`) are in
      `RestoreOutcome::invalidated`, and afterwards read their restored values
      (`DA == 105`).
- [ ] A rejection snapshot (`EA_IN = -1`) restores with
      `resultDetail == "negative input"`. `blockers(attr("EA1"))` is
      `NotProduced` with one note (status Ok, detail `"negative input"`), equal
      to the source engine's report (`restoreRejection`).
- [ ] AlreadyInstalled: after `request("expA")` in the same engine,
      restoring a snapshot whose bundle was altered (`EA1 = 999`) returns
      AlreadyInstalled / Ok, and `attribute("EA1")` is still 5. After a
      `request` that cached MissingInput, the kind is AlreadyInstalled with status
      MissingInput (`restoreNeverReplaces`).
- [ ] Each stale check is reached and installs nothing: ResultVersion (a
      registry whose `expA`-like descriptor declares `"v2"` while the snapshot says
      `"v1"`), Bundle (an undeclared output; a detail different from the reason),
      InputsUnavailable (`EA_IN` absent; also `expB` while `expA` is not
      installed), Leaves (`expB` into a state where `EA2` is a stored attribute),
      Fingerprint (`EA_IN = 5` instead of 4). After each,
      `resultStatus` is nullopt or NotRequested, `readiness(id).state` is not
      Done, and `runCount` is 0 (`restoreStaleChecks`, data-driven).
- [ ] `EA_IN` stored as `QString("4")` in the target state (instead of int 4)
      restores (the session text form is what is fingerprinted).
- [ ] `expB` restores after `expA` has been restored into the same engine.
- [ ] Unknown id → NotFound; family id (`neg` of the shared world, in a
      registry that has it) → NotFound; `derivA` → NotExplicit. None changes
      `cachedNodeCount()`.
- [ ] A ticket outstanding when the restore happens publishes
      `RefusedStale` / `AlreadyPublished`, and the restored values remain
      (`restoreBeatsOutstandingTicket`).
- [ ] After a restore, an input change drops the result exactly as after a
      publish: both engines, having read the same names, return equal
      invalidated sets from `state.setAttribute(engine, "EA_IN", 7)`, and both
      then read `EA1` unavailable (`restoredResultInvalidatesLikePublished`).

**Complexity:** L

---

### Task 1.5: Explicit-result listener: Installed and DroppedByInputChange

**Purpose:** Tell the owner of an engine when an Explicit result was installed
by a request or a publish (so the store can write), and when one was dropped by
an input change (so the store can delete). A drop by `clear()`, a registry
change, the registry's destruction or the engine's destruction must not
produce a drop event.

**Files to modify:**
- `src/engine/calculationengine.h`: event type, listener type and setter,
  queue member, private delivery function, parameters of `invalidate` /
  `deliverBroadcast`, pending registry-seed list.
- `src/engine/calculationengine.cpp`: queuing and delivery.

**Technical Approach:**

Public API, next to `setInvalidationListener` (h 96-100). The existing
names listener is untouched: its semantics, and every test that uses it, stay
as they are.

```cpp
/// What happened to an explicit calculation's cached result.
struct ExplicitResultEvent {
    enum class Kind {
        Installed,              ///< request(), prepare() (NothingToRun / Blocked) or publish() cached a requested result
        DroppedByInputChange    ///< an invalidation that started at an input dropped a requested result
    };
    Kind kind = Kind::Installed;
    QString instanceId;                                 ///< == the calculation id for a plain calculation
    ResultStatus status = ResultStatus::NotRequested;   ///< the status installed / dropped
};
using ExplicitResultListener = std::function<void(const ExplicitResultEvent &)>;
/// Called once per event, in the order the events happened, at the end of the
/// engine call that caused them and never inside an evaluation. The listener
/// may call exportResult() and any const inspection; it must not mutate the
/// session. Never called for restoreResult()'s own install, clear(), a
/// registry change, the registry's destruction or the engine's destruction.
/// Travels with the engine (a moved SessionData keeps it), like the
/// invalidation listener.
void setExplicitResultListener(ExplicitResultListener l);
```

**Which entries count.** An entry of `m_results` whose `requested` is true and
whose `instance.descriptor->policy == Explicit`. For an explicit calculation,
`requested` is true exactly when the status is not `NotRequested`
(`computeResult` 478-483). On-demand results are never reported.

**Installed.** `installRequested(..., InstallOrigin::Request)` appends
`{Installed, instanceId, entry.status}` when the calculation is explicit and
a listener is set. That covers:
- the synchronous path: `request()` → `requestInstance()` (every status,
  `Ok` / `Failed` / `MissingInput` / `Cycle` / `UndeclaredRead` /
  `InvalidOutput`);
- `prepare()`'s NothingToRun / Blocked path (`MissingInput` / `Cycle`);
- the asynchronous path: `publishPrepared()` when the outcome is Published.
  Refused and discarded publishes install nothing and queue nothing.
The detached engine of `evaluateFresh()` has no listener, so its replays queue
nothing. The store (Phase 3) keeps only `Ok`. The engine reports every install
and does not know the store's policy.

**DroppedByInputChange.** `invalidate()` (798-839) gains a parameter
`ExplicitDrops report` (`enum class ExplicitDrops { Report, Suppress }`). When
it is `Report` and it removes a `Result` node whose entry counts (see above),
it appends `{DroppedByInputChange, n.a, entry.status}` **before** the
`m_results.remove(n)`. Callers:
- `notifyLeafChanged()` (841-857), i.e. `attributeChanged`,
  `sourceMeasurementChanged`, `sourceUnitChanged`: **Report**.
- `deliverBroadcast()` (895-907) gains the same parameter.
  `onPreferenceChanged()` passes **Report**, `onRegistryChanged()` passes
  **Suppress**. When deferred (inside an evaluation), Report seeds go to
  `m_pendingSeeds` as today, and Suppress seeds go to a new
  `m_pendingRegistrySeeds`. (The registry refuses changes during an evaluation
  via `checkMutable`, so that list stays empty in practice, but causes must
  never be mixed.)
- `flushPending()` (909-924): `invalidate(m_pendingSeeds, Report)` and
  `invalidate(m_pendingRegistrySeeds, Suppress)`.
- `dropNotRequested()` (1025-1042): **Report**. A requested explicit result
  that depended on another calculation being "not requested" has had an input
  change when that calculation gets requested, published or restored.
- `clear()` / `clearCaches()` / `registryDestroyed()` / the destructor do not
  go through `invalidate()` and report nothing.

**Queue and delivery.** Member `QList<ExplicitResultEvent>
m_explicitEvents`. Nothing is appended when no explicit listener is set. A
private `void deliverExplicitEvents()` does nothing while `m_scopes` is
non-empty; otherwise it swaps the queue into a local list and calls the
listener for each event. Swapping first makes it safe for the listener to
re-enter a read, which may deliver later events itself. Call it at the end of:
`notifyLeafChanged()` (after `invalidate`, on the non-deferred path),
`deliverBroadcast()` (after the names listener), `flushPending()` (last
statement, so every read and every public entry point that flushes also
delivers), `request()`, `prepare()`, `publishPrepared()` (after its
`flushPending()`, before returning Published) and `restoreResult()`. Within
one call, Dropped events queued by `dropNotRequested()` come before the
Installed event of the same call, in queue order.

An Installed event can be followed in the same call by a Dropped event for the
same result, for example when a deferred invalidation flushed right after the
install drops it. The listener then sees Installed and `exportResult()`
returns nullopt, then sees Dropped. Document this on the listener.

**Acceptance Criteria:**
- [ ] `request("expA")` delivers exactly `[Installed expA Ok]`. A second
      `request("expA")` delivers nothing (`installedOnSyncAndAsync`).
- [ ] prepare + compute + publish of `expA` (inline and `StdThread` compute
      modes) delivers exactly `[Installed expA Ok]` at publish and nothing at
      prepare. A publish refused because `EA_IN` changed delivers no Installed.
- [ ] `prepare("expA")` with `EA_IN` absent delivers `[Installed expA
      MissingInput]`; `request("thrower")` delivers `[Installed thrower Failed]`.
- [ ] Requesting `derivA` (on demand) through `request()` delivers nothing.
- [ ] `restoreResult` of a valid snapshot delivers nothing
      (`restoreIsNotAnInstall`).
- [ ] With `expA` and `expB` installed, `state.setAttribute(engine, "EA_IN", 7)`
      delivers exactly two events, `DroppedByInputChange` for `expA` and for
      `expB`, both with status Ok (compared as a set: their relative order
      follows the breadth-first walk and is not specified). Both are
      delivered before `setAttribute` returns (`droppedByInputChange`).
- [ ] With an explicit calculation declaring preference `p`, installed:
      `prefs.set(registry, "p", 6)` delivers `[DroppedByInputChange <id> Ok]`
      after the names listener received its set. `prefs.set(registry, "q", 1)`
      (undeclared) delivers nothing.
- [ ] A source-measurement change (`state.setMeasurement(engine, "S", "m",
      ...)`) and a unit change (`state.setUnit(engine, ...)`) each deliver
      Dropped for the explicit calculation reading `S/m`.
- [ ] `request("expB")` before `expA` (cached MissingInput), then
      `request("expA")`, delivers `[DroppedByInputChange expB MissingInput,
      Installed expA Ok]` in this order.
- [ ] No drop event for: `engine.clear()` with `expA` installed; registering,
      then unregistering, a calculation that declares output `EA_IN` (both
      registry changes drop `expA`'s result, and `resultStatus` confirms it);
      destroying the engine (held in a `std::unique_ptr`) with `expA`
      installed (`noDropEventWithoutInputChange`).
- [ ] The existing names listener still receives exactly what it did: the
      existing listener assertions in `tst_calcengine`, `tst_calcengine_async`,
      `tst_calcengine_safety`, `tst_session_engine` and `tst_harness` pass
      unchanged.

**Complexity:** M

---

### Task 1.6: Engine tests: `tst_calcengine_restore`

**Purpose:** One core test executable that proves Tasks 1.2-1.5 with synthetic
calculations against the fake session state.

**Files to create:**
- `tests/tst_calcengine_restore.cpp`

**Files to modify:**
- `tests/CMakeLists.txt`: `flysight_add_test(tst_calcengine_restore SOURCES
  tst_calcengine_restore.cpp)` directly after `tst_calcengine_blockers` in the
  "Calculation engine" block (around line 125).
- `tests/README.md`: one row in the "Calculation engine (synthetic
  calculations)" catalogue table (after `tst_calcengine_blockers`). Covers:
  snapshot export (leaf closure, absent leaves, fingerprint canonical forms and
  known answer), restore into a fresh engine (identical edges, status, detail,
  blockers, invalidation; no run, no ticket), every stale check, and the
  explicit-result listener (installed on both paths, never on restore; dropped
  by input changes only). The acceptance matrix and map are Phase 5's.

**Technical Approach:**

Follow `tests/tst_calcengine_async.cpp`: a `World` struct in declaration order
registry → prefs → state → engine (a registry must outlive its engines), with
`Synthetic::registerExplicitWorld(registry)` plus local descriptors.
`FLYSIGHT_TEST_MAIN(...)`, `#include "tst_calcengine_restore.moc"`, and
`runAsync` / `computeOn` from `asyncdriver.h`. The World also records
explicit-result events in a `QList<ExplicitResultEvent>`, and names-listener
deliveries as `tst_calcengine_async` does (lines 38-60).

Two Worlds sharing one registry are needed for "fresh engine" tests. Give the
World a constructor taking an external `CalculationRegistry &` (and
`FakePreferenceProvider &`), or build a `Pair` struct holding one registry,
one provider and two state/engine pairs, in that declaration order. Separate
registries are needed only for the ResultVersion check (two descriptors with
the same id and different `resultVersion`).

Local descriptors in the test file (literals in their comments, as in
`tst_calcengine_async`):
- `withPref`: Explicit; inputs `attr EA_IN`, `pref p`; `WP = EA_IN * 100 + p`.
- `measExplicit`: Explicit; input `meas S/m`; `MS` = the count of samples of
  `S/m` (an int, so NaN inputs still give a comparable output).
- `versioned(const QString &version)`: `expA`'s shape under id `"verA"` with
  `resultVersion = version`.
- `thrower`: as in `tst_calcengine_async` (156-169).
- `altIn`: on demand; no inputs; output `attr EA_IN` = 0 (a candidate that
  never wins over the stored value; used only to cause registry changes).

Test functions (the names are used in the acceptance criteria above):
`fingerprintKnownAnswer`, `fingerprintCanonicalForms`, `leafKindCodes`,
`exportOnlyInstalledOk`, `exportLeaves` (expA, expB, withPref, measExplicit),
`restoreIntoFreshEngine`, `restoreRejection`, `restoreNeverReplaces`,
`restoreStaleChecks` (+ `_data`), `restoreChain` (expB fails before expA,
succeeds after), `restoreIgnoresAttributeType`, `restoreNotFound`,
`restoreBeatsOutstandingTicket`, `restoredResultInvalidatesLikePublished`,
`restoreNaNPayloadAndSignedZero` (measExplicit: a snapshot taken over
`{1.0, qNaN, -0.0}` restores into a state holding
`{1.0, bit_cast(0xFFF8000000000123), -0.0}` and is Stale / Fingerprint into
one holding `{1.0, qNaN, +0.0}`), `installedOnSyncAndAsync`,
`installedForEveryStatus`, `restoreIsNotAnInstall`, `droppedByInputChange`,
`droppedByPreferenceAndSource`, `droppedByRequestOfUpstream`,
`noDropEventWithoutInputChange`.

Known-answer example for `fingerprintKnownAnswer` (fixed here so that the
literal can be computed independently). State: attribute `K = QVariant(1.5)`;
measurement `S/m = {1.0, -0.0, qNaN}`, unit `"u"`. Preferences: `p = 5` (int).
Leaves, in this order: `storedAttribute("K")`, `storedAttribute("Z")`
(absent), `sourceMeasurement("S","m")`, `sourceUnit("S","m")`,
`preference("p")`, `preference("q")` (absent). The expected encoding is
written in the test as a hex literal assembled field by field, one commented
line per field, and compared with `inputFingerprintEncoding`. The expected
digest is the SHA-256 of those bytes, as a 64-character hex literal produced
outside the code under test (put the one-line `python -c "import hashlib; ..."`
used to produce it in a comment).

Spell leaves with the `GraphNode` factory functions
(`GraphNode::sourceMeasurement(...)`, `GraphNode::storedAttribute(...)`),
never with `GraphNode::Kind::SourceMeasurement` / `Kind::SourceUnit`. The
audit rule "source inputs: conversion layer only" forbids the enum spelling
outside `src/conversion`, `src/engine` and three named tests.

**Acceptance Criteria:**
- [ ] `tst_calcengine_restore` is registered, has label `core`, and passes.
- [ ] Every acceptance criterion of Tasks 1.2-1.5 that names a test function
      is implemented by a function of that name in this file (the
      `fingerprintKnownAnswer` literal digest included).
- [ ] The test registers nothing on the global registry (private registries
      only) and leaves no engine enrolled.
- [ ] `tests/README.md` catalogue lists the new target.

**Complexity:** L

---

### Task 1.7: Fusion: restored fit is indistinguishable

**Purpose:** Prove, on the real fit, that export → restore into a fresh
engine gives bit-identical outputs, identical `dependenciesOf`, identical
blockers and identical invalidation behaviour, and that the fit declares its
result version.

**Files to modify:**
- `tests/tst_fusion_session.cpp`: extend `registrationShape`; add
  `restoredFitIsIndistinguishable` (+ `_data`).

**Technical Approach:**

- `registrationShape` (190-253): `QCOMPARE(fit->descriptor->resultVersion,
  QStringLiteral("batch-temperature-bias-v3"))` (literal, per the test
  conventions; `tst_fusion_kernel` already holds it); `accH` and
  `systemTime` result versions are empty.
- `restoredFitIsIndistinguishable_data`: rows `coarse_linear` (success) and
  `reject_sigma` (rejection published as `Ok` with a reason).
- The test, symmetric in two sessions built from the same fixture
  (`fixtureSession(fixture, "f1")` twice). Each has its own engine on the
  global registry. Record explicit-result events on both.
  1. In **A** and **B** alike, read every `fusionNames()` name (all
     unavailable).
  2. A: `request(kFit)` → Ok. `snapshot = A.exportResult(kFit)`, which must
     have a value. Check `calculationId == kFit`, `resultVersion ==
     "batch-temperature-bias-v3"`, and that the diagnostics' `"algorithm"`
     equals `resultVersion`. Leaves are sorted by `storedLeafLess`, unique,
     and contain `GraphNode::sourceMeasurement("IMU","az")` and
     `GraphNode::storedAttribute("_LOCAL_ORIGIN_LAT")`. The fingerprint is 32
     bytes long.
  3. B: `restoreResult(snapshot)` → Restored / Ok. `outcome.invalidated ==`
     A's `RequestOutcome::invalidated`, and `B.runCount(kFit) == 0`.
  4. In A and B alike, read every `fusionNames()` name again. Then compare:
     every one of the seventeen channels plus `accH` and `_system_time` by
     `sameBitsEverywhere`; `_FUSION_DIAGNOSTICS` byte-identical as UTF-8;
     `resultStatus`, `resultDetail`;
     `dependenciesOf(GraphNode::result(kFit))`; `edgeCount()`;
     `cachedNodeCount()`. For `roll`, `accH`, `_FUSION_DIAGNOSTICS` and
     `fusionRollAtExit()`, compare the `blockers()` reports: state, blocker
     instance ids, and each note's instance id, status and detail. For the
     success row, `goldenDifference(B, golden)` is empty. For the rejection
     row, the `NotProduced` detail equals the golden's failure.
     `sameContent(B.exportResult(kFit), snapshot)`.
  5. Change one sample of `IMU/az` in both, as
     `changeAfterPublicationDropsEverything` does (509-512). The two returned
     sets are equal and contain every `fusionNames()` name. A's events are
     `[Installed Ok, DroppedByInputChange Ok]`, B's
     `[DroppedByInputChange Ok]`. Neither can export any more.
     `B.restoreResult(snapshot)` is Stale / Fingerprint, and
     `B.runCount(kFit) == 0` still.
- Never call `verifyAgainstFresh` / `evaluateFresh` on a session with a fit
  installed here: the oracle replays requested calculations and would run the
  fit again (slow, and not what is under test).

**Acceptance Criteria:**
- [ ] `registrationShape` checks the three result versions and passes.
- [ ] `restoredFitIsIndistinguishable` passes for both rows in the portable
      mode, and as `tst_fusion_session_exact` (label `exact`,
      `FLYSIGHT_FUSION_EXACT=1`) where the build registers it
      (`tests/CMakeLists.txt` 442). The bit-for-bit comparisons between A
      and B hold in both modes.
- [ ] `tst_fusion_session` stays within its 600 s timeout (the new test adds
      one success fit and one rejection).

**Complexity:** M

---

## Testing Requirements

### Unit Tests
- New `tst_calcengine_restore` (Task 1.6): fingerprint encoding and canonical
  forms, leaf codes, export, restore and every stale check, listener events.
- Existing engine tests must pass unchanged. They cover the behaviour-neutral
  refactor of `prepare` / `requestInstance` / `publishPrepared` onto
  `gatherAsRoot` / `installRequested`, and the untouched names listener.
- `tst_csvformat` must pass after `csvformat.cpp` moves to `flysight_model`.

### Integration Tests
- `tst_fusion_session` (Task 1.7): real fit, export → restore in a second
  session, bit-for-bit and graph-for-graph comparison, invalidation symmetry,
  result version.
- `tst_fusion_golden` / `tst_fusion_kernel`: the kernel's diagnostics are
  unchanged after `kAlgorithm` became `Fusion::Algorithm`.
- The whole suite, because the engine sits under everything:
  `tst_session_oracle`, `tst_calcengine_oracle`, `tst_jobqueue`,
  `tst_fusion_jobs`, `tst_fusion_rows`, `tst_column_cache`,
  `tst_logbook_index`.
- The cleanup audit (`-L audit`): the new files in `src/engine` must not
  contain `fileName`, `filePath`, `QFileInfo`, `QDate`, `PreferencesManager`,
  `QSettings`, `QDateTime::current`, `fusion/` or `Fusion::`.
  `builtincalculations.h` must not contain `CalculationCompatibilityVersion =`,
  `fusion/` or `Fusion::`.

### Verification commands (Michael's machine)
- Build: `cmake --build build-phase1 --config Release`. **Never build
  `build/`**: it has third-party ON and would overwrite the Boost-enabled
  solver install. The superbuild re-runs the inner configure because
  `src/CMakeLists.txt` and `tests/CMakeLists.txt` changed.
- Focused: `ctest --test-dir build-phase1/FlySightViewer-build -C Release
  --output-on-failure -R "tst_calcengine|tst_calcregistry|tst_csvformat|tst_fusion_session|tst_fusion_golden|tst_fusion_kernel"`
- Audit: `ctest --test-dir build-phase1/FlySightViewer-build -C Release
  --output-on-failure -L audit`
- Full suite at the end: `ctest --test-dir build-phase1/FlySightViewer-build
  -C Release --output-on-failure` (fusion tests have a 600 s timeout each).
- Running one executable by hand needs Qt's bin, GeographicLib,
  `build-solver-deps/GTSAM-install/bin` and
  `build-solver-deps/oneTBB-install/bin` on `PATH` (`tests/README.md` §2-4).

### Manual Verification
- None required: no product code calls the new API in this phase. Optionally
  start the application from `build-phase1`, fit one track, and confirm it
  behaves exactly as before (plot rows, job, diagnostics).

## Notes for Implementer

### Gotchas
- **Restore must not cache a failure.** `prepare()` caches `MissingInput` /
  `Cycle` as a requested result. `restoreResult` must not: a stale restore
  leaves the calculation "not requested". Tests check `resultStatus` and
  `readiness` after each stale check.
- **Order of checks matters for side effects.** ResultVersion and Bundle are
  decided before `dropNotRequested`, so they return an empty `invalidated`.
  InputsUnavailable / Leaves / Fingerprint come after gathering and must still
  return what `dropNotRequested` dropped.
- **Exclude C from the looked set** before `leafClosure` and before
  installing, exactly as `publishEdges()` does, or a ring through the
  calculation's own output (`explicitE` / `feedsE`) makes it depend on itself.
- **No run counting.** Build the `ResultEntry` by hand; do not route through
  `acceptRun()`.
- **Queue events only with a listener**, and deliver only when `m_scopes` is
  empty. `evaluateFresh()`'s detached engine must stay silent.
- **Record the drop before removing the entry** in `invalidate()`: after
  `m_results.remove(n)` the status is gone.
- **The fingerprint reads state; inspection "never touches state".** Put
  `exportResult` in its own documented section, not under the "inspection:
  const, never resolves, never computes, never touches state" banner (h 150).
- **Leaves reached through an explicit Result node** (`expB` → `expA`) mean an
  explicit calculation downstream of another restores only after the upstream
  one is installed. Phase 3 must restore a session's records in dependency
  order, or in passes until nothing more restores (the pattern of
  `evaluateFresh`, 1649-1658). Only fusion exists today, so it is moot in
  product, but the engine test `restoreChain` pins the behaviour.
- **`CsvFormat::formatAttributeValue` returns nullopt for an invalid QVariant.**
  A stored attribute that is present with an invalid value encodes present=1,
  representable=0. Do not collapse it into "absent".
- **Audit spellings:** in tests, use `GraphNode::sourceMeasurement(...)` rather
  than `GraphNode::Kind::SourceMeasurement` (see Task 1.6). In `src/engine`,
  avoid the words `fileName` / `filePath` even in comments.
- **Moving `csvformat.cpp` between libraries:** remove it from `flysight_core`
  in the same edit; listing it in both gives duplicate symbols in every
  executable that links both.
- **`fusionoutput.cpp` resolves `Algorithm` by namespace lookup.** If a local
  name `Algorithm` ever appears in `FlySight::Fusion::Detail`, qualify it as
  `Fusion::Algorithm`.

### Decisions Made
- **Type name and header:** `StoredCalculationResult` in
  `src/engine/storedcalculationresult.h`, a struct with public members
  `calculationId`, `resultVersion`, `detail`, `bundle` (`CalculationResult`),
  `leaves` (`QList<GraphNode>`), `inputFingerprint` (`QByteArray`, 32 raw
  bytes). Phase 2 reads and writes the members directly and uses
  `storedLeafKindCode` / `storedLeafFromCode` for the leaf kind on disk and
  `sameContent` in its round-trip tests.
- **Leaf kind wire codes** 0 StoredAttribute, 1 SourceMeasurement, 2
  SourceUnit, 3 Preference. Pinned, and not tied to the enum's underlying
  values.
- **Fingerprint encoding** as tabled in Task 1.2, with a versioned magic
  `flysight-inputs-v1\0`. Attribute and preference values use
  `CsvFormat::formatAttributeValue` (preferences as in the environment
  fingerprint). Source samples are LE bits with NaN canonicalised to
  `0x7FF8000000000000` and signed zero kept. Present flags come from
  `hasStoredAttribute` / `hasSourceMeasurement` / `QVariant::isValid`.
- **`csvformat.cpp` moves from `flysight_core` to `flysight_model`.** The
  engine lives in `flysight_model` (Qt Core only), which `flysight_core`
  links. It cannot call into `flysight_core` without a cycle. `csvformat`
  needs Qt Core only, so moving it keeps one number/attribute formatter (the
  audit's "one authority") and adds no dependency to `flysight_model` or the
  Python bridge that links it.
- **Plain calculations only.** Export and restore address plain explicit
  registrations by id. Families (and family instances) are NotFound. No
  explicit family exists. Supporting one would need the instance output in
  the snapshot.
- **Restore API:** `restoreResult(const StoredCalculationResult &)` returning
  `RestoreOutcome {kind, staleCheck, status, invalidated}` as specified in
  Task 1.4. It adds a sixth kind of check, `Bundle` (see below).
- **Listener:** a second, separate listener (`setExplicitResultListener`,
  `ExplicitResultEvent {Installed | DroppedByInputChange, instanceId,
  status}`) rather than widening the existing names listener, so no existing
  consumer or test changes. Installed is reported for every requested install
  of an explicit calculation (any status). Filtering to `Ok` is the store's
  job.
- **What counts as an input change for drops:** leaf notifications,
  preference broadcasts, deferred leaf/preference seeds, and
  `dropNotRequested` cascades. Registry-change broadcasts, `clear()`,
  registry destruction and engine destruction do not count.
- **Result version is not in the environment fingerprint** and does not
  change `index.json` stamps. It is a separate stamp in the record
  (Phases 2/3).

### Deviations / questions for the coordinator
1. **The leaf walk goes through explicit Result nodes too.** The overview
   says "transitively (through Resolution and on-demand Result nodes)". This
   document follows every recorded edge through every Resolution and Result
   node, explicit ones included. The spec (§4.1) asks for every input the
   result reached "directly or transitively", and the test world's
   `expB` → `expA` chain reaches `EA_IN` only through `expA`. The effect is a
   larger, never smaller, leaf list for an explicit calculation that consumes
   another. Consequence for Phase 3: restore in dependency order or in passes
   (see Gotchas). No effect on fusion today. If the coordinator prefers to
   stop at explicit Result nodes, `leafClosure` changes in one place and
   `exportLeaves` / `restoreChain` change their literals.
2. **An extra stale check, `Bundle`**, beyond the three the overview lists
   (result version, leaf list, fingerprint). It refuses a snapshot that no
   current descriptor could have produced (an undeclared output, or a detail
   that differs from the bundle's reason). It is cheap, touches nothing, and
   Phase 3 treats it like any other staleness (delete the record).
3. **`InputsUnavailable`** is a fourth reachable stale outcome (gathering ends
   `MissingInput` / `Cycle`, so there are no edges to compare). The overview's
   "leaf list differs" covers it in spirit. Phase 3 should treat it as stale
   only once dependency-order restore has been attempted (point 1).
4. **Installed events for non-`Ok` statuses** (`request()` of a failing
   calculation, `prepare()`'s NothingToRun). The overview's "the engine
   notifies its listener for either [install path]" is honoured literally.
   The store must ignore everything but `Ok` (and must not delete on a
   non-`Ok` Installed: "Such a run also deletes nothing").
5. **Dropped events from `dropNotRequested` cascades** count as input changes
   (the dependent's input changed from "not produced" to produced). With only
   fusion explicit, this cannot happen in product code today.

## Definition of Done

This phase is complete when:
1. All tasks have passing acceptance criteria.
2. `cmake --build build-phase1 --config Release` succeeds, and the full
   `ctest --test-dir build-phase1/FlySightViewer-build -C Release` passes,
   audit included.
3. Code follows the patterns of the reference files (engine sections and
   comment style of `calculationengine.*`, test idioms of
   `tst_calcengine_async.cpp` and `tst_fusion_session.cpp`, `tests/README.md` §8).
4. No TODOs or placeholder code remain. No file outside those listed in the
   tasks is changed, and `PLANS/`, `experiments/`, `build*/` are not touched.
