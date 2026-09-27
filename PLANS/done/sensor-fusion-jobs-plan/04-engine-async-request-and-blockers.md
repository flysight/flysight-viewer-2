# Phase 4: Engine: asynchronous request and blocker inspection

## Overview

The per-session `CalculationEngine` stays single-threaded but gains two
capabilities (spec section 7). First, an explicit calculation can be run in
three steps: **prepare** on the main thread (resolve and capture inputs,
record what they depended on), **compute** on any thread against the captured
inputs only, and **publish** on the main thread, where the engine itself
refuses a result whose inputs were invalidated, whose session is gone, or
whose registration was removed. Second, **blocker inspection** reports, without
running any explicit calculation, which explicit calculations stand between a
public name and its availability. The synchronous `request()` is unchanged and
both paths give identical results. Everything is tested with synthetic explicit
calculations; no GTSAM, no job queue, no thread is created by the library.

## Dependencies

- **Depends on:** None - can begin immediately (baseline `master` at or after `13c6115`).
- **Blocks:** Phase 5 (job queue and job model), Phase 9 (fusion calculation), Phase 10.
- **Assumptions:**
  - The engine is as on `master`: `src/engine/calculationengine.cpp` with
    `computeResult()` (lines 450-583), `requestInstance()` (915-960),
    `invalidate()` (707-740), `clearCaches()` (116-124),
    `onRegistryChanged()` (834-888).
  - All engine changes stay in `flysight_model` (Qt Core only; overview
    Decision 2). The library creates no thread and adds no lock (Decision 5,
    spec section 12).
  - Python plugin calculations are always `OnDemand` (nothing in
    `src/pluginadapters.cpp` sets `policy`) and stay untouched.
  - Phases 2 and 1 may be open at the same time; edits to the shared files
    `src/CMakeLists.txt`, `tests/CMakeLists.txt`, `docs/CALCULATIONS.md`,
    `tests/acceptance_map.txt`, `tests/README.md` must be small, additive,
    and in their own hunks (Decision 10).

## Design summary (read before the tasks)

### New and changed types

| Type / member | File | Role |
|---|---|---|
| `CalculationDescriptor::title` (`QString`) | `src/engine/calculationdescriptor.h` | Human-readable name for the interface ("Sensor fusion"). Opaque to the engine; never affects evaluation, identity, or the environment fingerprint. |
| `CalculationRegistry::title(id)` | `calculationregistry.h/.cpp` | Title of a plain calculation; the id when the title is empty or `id` is a family; empty string for an unknown id. |
| `CalculationResult::setReason()` / `reason()` | `calculationresult.h/.cpp` | Optional human-readable text explaining why outputs are unavailable. Part of the result (a function of the inputs), cached with it. |
| `CalculationProgress`, `CalculationCancelled` | new `src/engine/calculationprogress.h/.cpp` | The progress-text and cancellation facility, and the exception a compute function throws to abandon. |
| `EvaluationContext::progress()` | `evaluationcontext.h/.cpp` | How a compute function reaches the facility. Existing compute functions never call it and stay source-compatible. |
| `PreparedCalculation`, `ComputedCalculation`, `PublishOutcome` | new `src/engine/preparedcalculation.h/.cpp` | The prepared request ("ticket"), what `compute()` returns, and what `publish()` reports. |
| `CalculationEngine::PrepareOutcome`, `prepare()` | `calculationengine.h/.cpp` | Step 1. |
| `CalculationBlocker`, `UnproducedNote`, `BlockerReport`, `CalculationReadiness` | new `src/engine/blockerreport.h` | Inspection result types. |
| `CalculationEngine::blockers()`, `readiness()`, `resultDetail()`, `preparedCount()` | `calculationengine.h/.cpp` | Inspection and instrumentation. |
| `GraphNode::Kind::Prepared`, `GraphNode::prepared()` | `src/engine/calctypes.h` | Graph identity of an outstanding ticket, so that ordinary invalidation reaches it. |

### API (signatures are part of the contract for phases 5, 6, 9)

```cpp
// calculationprogress.h -------------------------------------------------------
class CalculationProgress {                 // implemented by the caller (phase 5)
public:
    virtual ~CalculationProgress();
    virtual void report(const QString &text) = 0;   // called on the compute thread; must not throw
    virtual bool isCancelled() const = 0;           // called on the compute thread; must not throw
    void throwIfCancelled() const;                  // throws CalculationCancelled when isCancelled()
    static CalculationProgress &none();             // stateless: never cancelled, drops text
};
class CalculationCancelled {};              // deliberately NOT derived from std::exception

// evaluationcontext.h ---------------------------------------------------------
CalculationProgress &progress() const;      // never null; none() unless compute() was given one

// preparedcalculation.h -------------------------------------------------------
struct ComputedCalculation {                // movable value; opaque payload
    enum class Kind { Completed, Failed, Cancelled, ResourceExhausted };
    Kind kind = Kind::Cancelled;
    QString failureText;                    // Failed: what(), or "non-standard exception"
    // private to the engine: CalculationResult bundle; QList<CalcInput> undeclaredReads
};

struct PublishOutcome {
    enum class Kind { Published, RefusedStale, RefusedGone, Discarded };
    enum class Reason { None,
                        InputsChanged, AlreadyPublished,        // RefusedStale
                        SessionGone, RegistrationRemoved,       // RefusedGone
                        Cancelled, ResourceExhausted };         // Discarded
    Kind kind = Kind::RefusedGone;
    Reason reason = Reason::SessionGone;
    ResultStatus status = ResultStatus::NotRequested;   // Published only: the cached status
    QString detail;                                     // Published only: == resultDetail()
    QSet<DependencyKey> invalidated;                    // Published only; caller must publish it
};

class PreparedCalculation {                 // held by std::unique_ptr; not copyable, not movable
public:
    ~PreparedCalculation();                 // MAIN THREAD. Withdraws from the engine if it still exists.
    CalculationId registrationId() const;   // main thread
    QString instanceId() const;             // main thread
    QString title() const;                  // main thread; descriptor title, else the instance id

    /// ANY THREAD, at most once. Touches the captured inputs, the descriptor,
    /// and `progress` - nothing else. Never throws.
    ComputedCalculation compute(CalculationProgress *progress = nullptr);

    /// MAIN THREAD, at most once, after compute() has returned.
    PublishOutcome publish(ComputedCalculation computed);
};

// calculationengine.h ---------------------------------------------------------
struct PrepareOutcome {
    enum class Kind { NotFound, NotExplicit, AlreadyValid, NothingToRun, Blocked, Ready };
    Kind kind = Kind::NotFound;
    ResultStatus status = ResultStatus::NotRequested;   // AlreadyValid / NothingToRun / Blocked
    QList<CalculationBlocker> blockers;                 // Blocked only
    std::unique_ptr<PreparedCalculation> ticket;        // Ready only
    QSet<DependencyKey> invalidated;                    // see "Prepare"
};
PrepareOutcome prepare(const CalculationId &id,
                       const DependencyKey &instanceOutput = DependencyKey::attribute(QString()));

BlockerReport        blockers(const DependencyKey &name);
CalculationReadiness readiness(const CalculationId &id,
                               const DependencyKey &instanceOutput = DependencyKey::attribute(QString()));
QString resultDetail(const CalculationId &id,
                     const DependencyKey &instanceOutput = DependencyKey::attribute(QString())) const;
int preparedCount() const;                  // outstanding tickets (instrumentation)

// blockerreport.h -------------------------------------------------------------
struct CalculationBlocker {                 // enough to call prepare()/request() and to label a job
    CalculationId registrationId;
    DependencyKey instanceOutput;           // empty name (isEmptyName) for a plain calculation
    QString instanceId;
    QString title;                          // descriptor title, else instanceId
};
struct UnproducedNote {                     // "ran and did not produce"
    CalculationBlocker calculation;
    ResultStatus status;                    // Ok (output unavailable), Failed, UndeclaredRead, InvalidOutput
    QString detail;                         // result reason, or the failure text; may be empty
};
struct BlockerReport {
    enum class State { Available, Blocked, NotProduced, NotApplicable };
    State state = State::NotApplicable;
    QList<CalculationBlocker> blockers;     // non-empty iff Blocked; unique by instanceId, discovery order
    QList<UnproducedNote> notProduced;      // non-empty when NotProduced; may also be non-empty when Blocked
};
struct CalculationReadiness {
    enum class State { Unknown, MissingInput, Blocked, Ready, Done };
    State state = State::Unknown;
    QList<CalculationBlocker> blockers;     // Blocked only
    std::optional<ResultStatus> status;     // Done: the cached status, when there is one
};
```

### Threading rule (the narrow rule of spec 7.1)

- `PreparedCalculation` is created, inspected, published, and destroyed on the
  main thread. **Only `compute()` may run elsewhere**, once, and the caller
  guarantees it has returned before `publish()` or the destructor runs
  (a thread join or a queued "finished" signal gives that ordering).
- `compute()` reads exactly: the `std::shared_ptr<const CalculationDescriptor>`
  held by the ticket, the ticket's own `EvaluationContext`, a "compute started"
  flag, and the `CalculationProgress` it was given. It never dereferences the
  engine pointer, the staleness fields, the registry, or a session. There is no
  lock and no atomic in the library; the fields are partitioned between the
  threads.
- Captured values are Qt implicitly shared copies (`QVector<double>`,
  `QString`, `QVariant`) taken during prepare. Qt's reference counts are
  atomic and every main-thread writer detaches before writing, so the worker's
  buffers are immutable for its purposes and later session edits cannot reach
  them. No deep copy is made (overview Decision 5: "shared-immutable handles").
- The compute function of an explicit calculation must be re-entrant: no
  mutable captured state, no statics. It may be running on the worker while
  another session evaluates the same descriptor synchronously in a test.
- All logging caused by an asynchronous run happens on the main thread, in
  `publish()` (the test `WarningCapture` and message handlers are not
  thread-safe). The ticket's context is therefore constructed quiet.

### Prepare

`prepare()` mirrors the first half of `request()` exactly, so the two paths
cannot diverge:

1. Asserts `m_scopes.empty()` (as `request()` does), `flushPending()`, looks the
   instance up. Unknown id / wrong family name: `NotFound`, nothing changes.
2. `policy != Explicit`: `NotExplicit`, nothing changes. (Keeps on-demand and
   plugin compute functions off worker threads by construction.)
3. A cached result with status other than `NotRequested`: `AlreadyValid` with
   that status. A valid result is never recomputed.
4. Otherwise drops the cached `NotRequested` entry and invalidates its
   dependents - the same statements as `requestInstance()` lines 939-943 - so
   that no input is served from an answer derived from "not requested" and a
   ring through the calculation's own output is detected exactly as in the
   synchronous path. The dropped names are remembered.
5. Runs the availability pass (Task 4.2's `gatherInputs`) inside a `ScopeGuard`
   for `Result(instanceId)`, filling a heap-allocated, quiet
   `EvaluationContext`. On-demand inputs are computed as needed; explicit ones
   are never started (the ordinary read path already guarantees that).
6. Pass did not end `Ok` (`MissingInput` or `Cycle`): the result entry is
   cached and its edges published **exactly as `request()` would**
   (`requested = true`). Kind is `NothingToRun`, or `Blocked` when the status
   is `MissingInput` and `readiness()` of this instance reports blockers
   (spec 7.1 step 1, last sentence). `invalidated` = the dropped names; the
   caller treats it like `RequestOutcome::invalidated`.
7. Pass ended `Ok`: nothing is cached for the calculation. A
   `PreparedCalculation` is created holding the instance, the context, the
   closed scope's `looked` set, `provisional` statuses and `sawCycle`, and the
   dropped names. The engine registers the ticket (see "Staleness"). Kind is
   `Ready`. `invalidated` is informational here: no value changed, and
   `publish()` reports these names again, so the caller need not publish it.

While a ticket is outstanding the calculation is still "not requested" for
every reader, for `evaluateFresh()`, and for inspection. Cancelling or
dropping the ticket leaves it that way (spec 7.1: "as if it had never been
asked").

### Staleness: decided from the engine's own dependency records

Each `Ready` ticket gets a graph node `GraphNode::prepared(instanceId, serial)`
(`serial` from a per-engine counter, as text in `b`). The engine calls
`setEdges(node, looked)` with the scope's `looked` set (minus
`Result(instanceId)` should it occur), so the ticket has precisely the forward
and reverse edges the published result would have had. `m_prepared`
(`QHash<GraphNode, PreparedCalculation *>`) maps the node to the ticket.

- `invalidate()`: when the breadth-first walk visits a `Prepared` node, the
  ticket is marked stale (`Reason::InputsChanged`) and removed from
  `m_prepared`; the generic `dropForwardEdges(n)` already removes its edges.
  This covers attribute, source, unit, preference, and registry changes that
  reach any input transitively, including through provisional descendants
  (their `looked` sets were handed up into the ticket's scope).
- `clearCaches()` (reached from `clear()` and `registryDestroyed()`): every
  outstanding ticket is marked stale (`InputsChanged`) before the maps are
  wiped. `SessionData::operator=(const SessionData &)` therefore supersedes.
- `onRegistryChanged()` with `!change.added`: every ticket whose
  `registrationId` equals `change.registrationId` is marked gone
  (`RegistrationRemoved`) immediately and withdrawn, whether or not any seed
  reaches it. Re-registering the same id later does not revive it.
- `~CalculationEngine()`: every outstanding ticket gets its engine pointer
  nulled and is marked gone (`SessionGone`). `~PreparedCalculation()` calls
  `engine->forget(this)` only when the pointer is still set. Both run on the
  main thread, so plain pointers suffice. The engine is held by
  `std::unique_ptr` in `SessionData`, so moving a session does not move the
  engine and does not disturb a ticket; destroying or move-assigning over a
  session destroys its engine and supersedes.

### Compute

`PreparedCalculation::compute(progress)` sets the context's progress pointer
(`none()` when null), calls the shared run helper (Task 4.2), and returns a
`ComputedCalculation`. Exception classification, in this catch order:

| Thrown | `Kind` | Later effect |
|---|---|---|
| nothing | `Completed` | published; status decided at publish (Ok / UndeclaredRead / InvalidOutput) |
| `CalculationCancelled` | `Cancelled` | nothing published, nothing cached |
| `std::bad_alloc` (and subclasses) | `ResourceExhausted` | nothing published, nothing cached (spec 8.2 "Failed") |
| any other `std::exception` | `Failed`, `failureText = what()` | published and cached as `ResultStatus::Failed`, as today |
| anything else | `Failed`, fixed text | same |

A compute function that returns normally although cancellation was requested
yields `Completed`; publishing it is correct (the facility "cannot influence
the result except by abandoning it"). Whether a cancelled job publishes such a
result is phase 5's decision. A second call of `compute()` asserts and returns
`Cancelled`. On the `bad_alloc` path nothing is allocated (no text is built).

### Publish

`ticket->publish(std::move(computed))`, main thread:

1. Engine pointer null: `RefusedGone / SessionGone`.
2. Otherwise `m_scopes` must be empty (assert); `flushPending()`.
3. Ticket marked gone: `RefusedGone / RegistrationRemoved`. Marked stale:
   `RefusedStale / InputsChanged`.
4. `computed.kind` is `Cancelled` or `ResourceExhausted`: `Discarded` with the
   matching reason. (Checked after staleness so that a superseded job is
   reported as superseded.)
5. A result with status other than `NotRequested` is already cached for the
   instance (a synchronous `request()` ran in between):
   `RefusedStale / AlreadyPublished`. The cached result is identical by purity.
6. Otherwise, exactly as `requestInstance()`: withdraw the ticket's graph node
   first; remove a re-cached `NotRequested` entry; `invalidated` = names
   dropped at prepare, united with `invalidate(m_dependents.value(C).values())`;
   build the `ResultEntry` with Task 4.2's `acceptRun` (`requested = true`,
   `sawCycle` from the ticket); `m_results.insert(C, entry)`;
   `publishEdges(C, scope rebuilt from the ticket)`; `flushPending()`. All
   outputs appear at once because the bundle is one immutable object.
   Kind `Published`, with `status` and `detail`.

In every branch the ticket is withdrawn and spent; a second `publish()`
asserts and returns `RefusedStale / InputsChanged`. Run counters
(`runCount`, `totalRunCount`) and undeclared-read counters are updated only in
step 6, so after a successful asynchronous request they equal those of a
synchronous one; refused and discarded runs are not counted.

### Blocker inspection

`readiness(id, instanceOutput)` classifies one calculation instance; input
availability is decided **before** policy:

1. Unknown instance: `Unknown`.
2. Every declared input is checked (all of them, not only up to the first
   unavailable one): Attribute / Measurement inputs by an ordinary top-level
   read (`readTopLevel(name).available`); Preference by the provider;
   SourceMeasurement / SourceUnit by the state, as in `computeResult()`.
3. For each unavailable Attribute / Measurement input, the name-level walk
   below is applied recursively. If any unavailable input is a missing
   preference / source, or its sub-report is `NotApplicable` or `NotProduced`:
   `MissingInput` (no blockers: there is nothing a request could fix from
   here). If every unavailable input is `Blocked`: `Blocked`, blockers = union
   in discovery order.
4. All inputs available: `Done` when a result with status other than
   `NotRequested` is cached (with that status) or when the policy is
   `OnDemand` (nothing to request); otherwise `Ready`.

`blockers(name)`:

1. Ordinary top-level read of `name`. Available: `State::Available`, empty lists.
2. Otherwise walk the candidates the resolution rule would consider: none when
   an attribute is stored (a stored value wins even when invalid);
   `sourceConversionsFor` when a measurement has source data and conversions
   are registered; else `candidatesFor(name)`. For each candidate, in order:
   - `readiness` `Blocked`: contributes its blockers.
   - `Ready` (explicit, inputs available, never run): contributes itself.
   - `Done`, policy `Explicit`: contributes an `UnproducedNote` (status and
     `resultDetail`).
   - `MissingInput` whose unavailable inputs include a `NotProduced`
     sub-report and no `NotApplicable` one: contributes those notes (a failed
     fusion makes `accH` "not produced", not "not applicable").
   - anything else (`Done` on demand, `MissingInput`, `Unknown`): nothing.
3. State precedence: `Blocked` if any blockers, else `NotProduced` if any
   notes, else `NotApplicable`.
4. A set of names currently being walked ends rings: a revisited name
   contributes nothing.

Consequences, each of which is tested: `accH`-style names report the explicit
calculation behind them through any number of on-demand intermediates; a
calculation with a genuinely missing input is never a blocker and never
"not requested"; with explicit B consuming unrequested explicit A the blocker
is A alone, and B once A has published; a name that another candidate already
makes available reports nothing even if an explicit candidate precedes it.
Inspection performs only ordinary reads and const lookups, so by the
idempotency invariant (engine spec 7.4) it can never change what a later read
returns; it may compute cheap on-demand values, and it has no path to
`computeResult(..., fromRequest = true)`. An outstanding ticket does not
change the report: "pending" is phase 6's notion, derived from the job model.

## Tasks

### Task 4.1: Title, result reason, and the progress/cancel facility

**Purpose:** Add the small value-level vocabulary the rest of the phase and phases 5, 6, 9 build on, without changing any behavior.

**Files to modify:**
- `src/engine/calculationdescriptor.h` - add `QString title;` to `CalculationDescriptor` after `id`, documented as interface text only ("opaque to the engine; registrars pass translated text; empty means none").
- `src/engine/calculationregistry.h` / `.cpp` - add `QString title(const CalculationId &id) const;` next to `contains()`. No validation of titles; `validate()` is unchanged.
- `src/engine/calculationresult.h` / `.cpp` - add `CalculationResult &setReason(const QString &text);` and `QString reason() const;` (member `QString m_reason`). Document: "why outputs are unavailable; a function of the inputs like every other part of the bundle".
- `src/engine/evaluationcontext.h` / `.cpp` - add `CalculationProgress &progress() const;`, private `CalculationProgress *m_progress = nullptr;`, `friend class PreparedCalculation;`. Extend the class comment: the facility is not an input and "cannot influence the result except by abandoning it".
- `src/engine/calctypes.h` - fix the `EvaluationPolicy::Explicit` comment ("runs only through `CalculationEngine::request()` or `prepare()`").
- `src/CMakeLists.txt` - add `engine/calculationprogress.cpp engine/calculationprogress.h` to the `flysight_model` source list (lines 239-252), one new line.

**Files to create:**
- `src/engine/calculationprogress.h` / `.cpp` - `CalculationProgress`, `CalculationCancelled` as in the design summary. The `.cpp` holds the out-of-line destructor, `throwIfCancelled()`, and `none()` (function-local static of a private stateless subclass).

**Technical Approach:**
- Follow the comment density and include style of `src/engine/evaluationcontext.h`.
- `CalculationCancelled` must not derive from `std::exception`, so a compute function (or a library under it) that catches `std::exception` cannot swallow a cancellation.
- `EvaluationContext::progress()` returns `*m_progress` when set, else `CalculationProgress::none()`. In the synchronous path it is never set, so `isCancelled()` is always false there.
- Nothing in `src/calculations/`, `src/conversion/`, `src/pluginadapters.cpp`, `src/pluginhost.cpp`, `src/pluginsessionview.h`, or `src/sessiondata_bindings.cpp` changes; they must compile untouched.

**Acceptance Criteria:**
- [ ] `flysight_model`, `flysight_core`, `flysight_cpp_bridge`, and the application build with no edit to any existing compute function or registration.
- [ ] `CalculationRegistry::title()` returns the title, the id for an empty title or a family id, and an empty string for an unknown id (`tst_calcregistry::titleLookup`).
- [ ] `CalculationResult().setReason("x").reason() == "x"`; a default result has an empty reason (`tst_calcregistry` value-type section, or `tst_calcengine_async`).
- [ ] `calculationEnvironmentFingerprint()` is unchanged by a title (existing `tst_builtins_engine` fingerprint tests pass unchanged).
- [ ] All existing tests pass unchanged.

**Complexity:** S

---

### Task 4.2: Split `computeResult()` into gather / run / accept (pure refactor)

**Purpose:** Give the synchronous and asynchronous paths one implementation of each step so that "identical results" (acceptance 7) holds by construction.

**Files to modify:**
- `src/engine/calculationengine.h` / `.cpp` - private helpers; `computeResult()` rewritten in terms of them.
- `src/engine/preparedcalculation.h` / `.cpp` (created here, completed in 4.3) - `ComputedCalculation` and the private static run helper.
- `src/CMakeLists.txt` - add `engine/preparedcalculation.cpp engine/preparedcalculation.h` and `engine/blockerreport.h` to `flysight_model` (one hunk with Task 4.1's line).

**Technical Approach:**
- `ResultStatus CalculationEngine::gatherInputs(const CalculationInstance &, EvaluationContext &ctx)`: the availability loop of lines 475-529, verbatim in behavior (declared order, stop at the first unavailable input, `Cycle` check on `m_scopes.back().cycle`). The caller has already pushed the `ScopeGuard`.
- `static ComputedCalculation PreparedCalculation::run(const CalculationDescriptor &, EvaluationContext &, CalculationProgress *)` (private; `CalculationEngine` is a friend): sets `ctx.m_progress`, invokes `compute`, classifies exceptions per the table in the design summary, moves `ctx.m_undeclaredReads` into the outcome, clears `ctx.m_progress`. No engine access, no logging, never throws.
- `ResultEntry CalculationEngine::acceptRun(const CalculationInstance &, ComputedCalculation &&, bool requested, bool logUndeclaredReads)`: run counters (lines 534-536), the failure warnings (same text as lines 543-549, built from `failureText`), undeclared-read accounting (552-557), declared-output validation (559-572), bundle creation (575-576). New: `ResultEntry` gains `QString detail` = the bundle's `reason()` for a clean run, `failureText` for `Failed`. `logUndeclaredReads` is true only for the asynchronous path (its context was quiet); the message text equals the one in `EvaluationContext::declared()`.
- Synchronous `computeResult()`: `gatherInputs`, then `run(d, ctx, nullptr)`, then - to keep today's semantics exactly - map `Cancelled` and `ResourceExhausted` to `Failed` (`std::bad_alloc` is a `std::exception` and is cached as `Failed` today; see "Decisions Made"), then `acceptRun`.
- Counters are still incremented for a throwing calculation (it "counts as a run"); the order of warnings relative to each other is unchanged.

**Acceptance Criteria:**
- [ ] `tst_calcengine`, `tst_calcengine_safety`, `tst_calcengine_oracle`, `tst_builtins_engine`, `tst_session_engine`, `tst_session_oracle`, and `tst_python_bridge` pass with no edit to their sources.
- [ ] `resultDetail("thrower")` in a test equals `"synthetic failure"` after the `std::runtime_error` case of `exceptionLeavesNoPartialResult`-style setup (asserted in `tst_calcengine_async::ordinaryExceptionIsCachedFailure`, synchronous half).
- [ ] No behavior of `request()` or ordinary reads changes (warning texts included).

**Complexity:** M

---

### Task 4.3: `PreparedCalculation` and `CalculationEngine::prepare()`

**Purpose:** Implement step 1 and step 2 of the asynchronous request: capture on the main thread, compute anywhere with no engine access.

**Files to modify:**
- `src/engine/preparedcalculation.h` / `.cpp` - the ticket class as in the design summary. Private state, partitioned in the header by comment: *compute-thread fields* (`std::shared_ptr<const CalculationDescriptor> m_descriptor`, `std::unique_ptr<EvaluationContext> m_context`, `bool m_computeStarted`) and *main-thread fields* (`CalculationEngine *m_engine`, `CalculationInstance m_instance`, `GraphNode m_node`, `QSet<GraphNode> m_looked`, `QHash<GraphNode, ResultStatus> m_provisional`, `bool m_sawCycle`, `QSet<DependencyKey> m_droppedAtPrepare`, `bool m_spent`, `PublishOutcome::Kind`/`Reason` of a pending refusal). `Q_DISABLE_COPY_MOVE`. Private constructor; `friend class CalculationEngine`.
- `src/engine/calculationengine.h` / `.cpp` - `PrepareOutcome`, `prepare()`, `preparedCount()`, private `forget(PreparedCalculation *)`, `m_prepared`, `m_preparedSerial`; a new section "Asynchronous request" after "Explicit evaluation". The header includes `preparedcalculation.h` (which only forward-declares the engine).
- `src/engine/calctypes.h` - `GraphNode::Kind::Prepared`, `GraphNode::prepared(instanceId, serial)`, and its `describe()` case (`prepared(<id>#<serial>)`).

**Technical Approach:**
- Follow the numbered "Prepare" flow in the design summary. Reuse the statements of `requestInstance()` for the drop-and-invalidate step rather than paraphrasing them; if that means extracting a small private helper used by both, do so.
- The context is created with `quiet = true` regardless of `m_quiet` (logging happens at publish).
- For `NothingToRun` / `Blocked`, insert the entry and call `publishEdges(C, scope)` as `requestInstance()` lines 955-958 do, including the `Q_ASSERT(isContextFree(scope))`.
- `Blocked` detection calls `readiness()` (Task 4.5) after the entry is cached. Tasks 4.3-4.5 land in one phase commit, so implement 4.5's `readiness()` before wiring this branch; its test (`blockedPrepareNamesBlocker`) lives in Task 4.6.
- `compute()`: assert-and-refuse a second call; otherwise `return run(*m_descriptor, *m_context, progress);`.
- An exception thrown by the *state* during the availability pass propagates to the caller of `prepare()` with scopes popped and nothing registered, as for `request()` (`foreignExceptionPublishesNothing`).
- Update the class comment of `CalculationEngine` ("Single-threaded.") to state the threading rule in two sentences and point to `preparedcalculation.h`.

**Acceptance Criteria:**
- [ ] `prepare()` of an unknown id, an on-demand id, a calculation with a valid cached result, and one with a missing input return `NotFound`, `NotExplicit`, `AlreadyValid`, `NothingToRun` respectively; only the last changes the cache, and it changes it exactly as `request()` does (same `resultStatus`, same `dependenciesOf(Result)`, same `invalidated`).
- [ ] `prepare()` on the `explicitE` / `feedsE` ring (from `tst_calcengine_safety`) returns `NothingToRun` with `status == Cycle`, with the same `cycleCount`, `lastCyclePath`, and cached states as `request()`, in both "nothing cached" and "not-requested answers cached" variants.
- [ ] `prepare()` never runs an explicit calculation: with B consuming unrequested A, `prepare(B)` returns `Blocked` with `blockers == [A]` and `runCount(A) == 0`.
- [ ] After a `Ready` prepare, reads of the outputs still return unavailable, `evaluateFresh` agrees, and `verifyAgainstFresh` is empty.
- [ ] `compute()` produces the correct bundle after the engine, the state, and the registry (in that order) have been destroyed; `publish()` then returns `RefusedGone / SessionGone` without touching freed memory.
- [ ] With the inputs already read (so their resolutions are cached), `prepare()` followed by destroying the unpublished ticket leaves `edgeCount()` and `cachedNodeCount()` exactly as they were before `prepare()`, and `preparedCount() == 0`; repeating it 1000 times does not grow the graph.

**Complexity:** M

---

### Task 4.4: Staleness tracking and `publish()`

**Purpose:** Let the engine, not the caller, decide whether a computed result may be installed, and install it with the semantics of `request()`.

**Files to modify:**
- `src/engine/calculationengine.h` / `.cpp` - ticket registration with `setEdges`; the `Prepared` case in `invalidate()`; marking in `clearCaches()`, `onRegistryChanged()`, `registryDestroyed()`, `~CalculationEngine()`; private `PublishOutcome publishPrepared(PreparedCalculation &, ComputedCalculation &&)`.
- `src/engine/preparedcalculation.cpp` - `publish()` and the destructor.

**Technical Approach:**
- Follow "Staleness" and "Publish" in the design summary step by step; the order of the checks in `publish()` is part of the contract.
- In `invalidate()`, add `else if (n.kind == GraphNode::Kind::Prepared)` next to the `Result` case (line 726). Do not add `Prepared` nodes to `names`. `knownNodes()` needs no change.
- In `onRegistryChanged()`, mark tickets before building the seeds and independently of `deliverBroadcast()` (which defers while evaluating).
- Withdraw the ticket's node (`dropForwardEdges`, erase from `m_prepared`) **before** invalidating the dependents of `C` in step 6.
- Rebuild a `Scope` from the ticket's `m_looked`, `m_provisional`, `m_sawCycle` and pass it to the existing `publishEdges()` so provisional-status bookkeeping is identical.
- `edgeCount()` includes ticket edges while a ticket is outstanding; `cachedNodeCount()` does not. Say so in the header comments of both.
- `evaluateFresh()` needs no change: it replays `requested` entries, and a published asynchronous result has `requested == true`.

**Acceptance Criteria:**
- [ ] Inline and threaded asynchronous requests leave the engine in the same observable state as `request()` on an identical world: `status`, `invalidated`, every name's value (`sameValue`), `dependenciesOf(Result(id))`, `resultStatus`, `resultDetail`, `runCount`, `undeclaredReadCount`; `verifyAgainstFresh` is empty (acceptance 7).
- [ ] Names read while unrequested are reported by `publish()` even if they were not re-read after `prepare()`; names read between prepare and publish are reported too; all outputs appear together.
- [ ] Changing a declared input (directly, through an on-demand intermediate, through a declared preference, through a registry change that alters an input's resolution, or `clear()`) between prepare and publish gives `RefusedStale / InputsChanged`, publishes nothing, counts no run, and a new `prepare()` returns `Ready` (acceptance 8, first half). An unrelated edit does not refuse.
- [ ] After a publication, changing a declared input drops the result and every dependent; the outputs read unavailable and `prepare()` returns `Ready` again (acceptance 8, second half).
- [ ] Unregistering the calculation between prepare and publish gives `RefusedGone / RegistrationRemoved`, also when the same id is registered again before publish. Destroying the engine gives `RefusedGone / SessionGone`. Neither crashes when the ticket outlives the engine.
- [ ] `Cancelled` and `ResourceExhausted` give `Discarded`, cache nothing (`resultStatus` is not `Failed`), and leave the calculation preparable; an ordinary exception gives `Published` with `status == Failed`, a non-empty `detail`, and a later `prepare()` returns `AlreadyValid / Failed`.
- [ ] A synchronous `request()` between prepare and publish gives `RefusedStale / AlreadyPublished` and leaves the synchronous result in place.

**Complexity:** M

---

### Task 4.5: Blocker inspection

**Purpose:** Report which explicit calculations stand between a public name and availability, without running any of them (spec 7.2).

**Files to create:**
- `src/engine/blockerreport.h` - the four structs of the design summary (header only; includes `calctypes.h`, `<optional>`).

**Files to modify:**
- `src/engine/calculationengine.h` / `.cpp` - `blockers()`, `readiness()`, `resultDetail()`; private recursive helpers taking the set of names being walked; a new section "Blocker inspection" after "Inspection and instrumentation". In the header, place these under a new group comment: "blocker inspection: may compute on-demand values; never runs an explicit calculation; never changes what a read returns" - distinct from the existing const "never computes" inspection group.

**Technical Approach:**
- Follow "Blocker inspection" in the design summary. Both public functions assert `m_scopes.empty()` and return the default-constructed report otherwise (release behavior, as `readTopLevel()` does).
- `resultDetail()` is const and mirrors `resultStatus()` for the instance lookup; it returns `ResultEntry::detail` of a cached entry, else an empty string.
- Build `CalculationBlocker` from the `CalculationInstance` and the name that selected it: `instanceOutput` is the empty name for a plain calculation (`instanceId == registrationId`), otherwise the name passed to `candidatesFor` / `instance`.
- Deduplicate blockers and notes by `instanceId`, keeping first-discovery order (deterministic: candidate order, then declared input order).
- Do not read `m_results` to decide input availability (engine spec 7.3: availability is never decided by what is cached); use it only for "has this explicit calculation a valid result".

**Acceptance Criteria:**
- [ ] For an on-demand name two levels above an unrequested explicit calculation, `blockers()` reports exactly that calculation (id, empty `instanceOutput`, title); after publication it reports `Available`; `runCount` of the explicit calculation is 0 until it is requested (acceptance 12, engine half).
- [ ] With the explicit calculation's own input absent, its outputs and everything above report `NotApplicable`, `readiness()` is `MissingInput`, and `prepare()` returns `NothingToRun / MissingInput`, never `Blocked`.
- [ ] With explicit B consuming explicit A: blockers of B's output and of a name derived from it are `[A]`; after A publishes, `[B]`; after B publishes, `Available`. A loop "inspect, prepare/compute/publish each blocker, repeat" terminates with A run once then B run once (acceptance 13, engine half), with compute inline and on a thread.
- [ ] After an explicit calculation ran and rejected its inputs, its outputs and their dependents report `NotProduced` with the calculation, `status == Ok`, and the reason text; its diagnostics attribute output is `Available`; after an input change they report `Blocked` again. A throwing explicit calculation reports `NotProduced` with `status == Failed` and the exception text.
- [ ] A name made available by a later on-demand candidate reports `Available` although an explicit candidate precedes it; a stored attribute reports `Available` / `NotApplicable` without consulting candidates; an unknown name reports `NotApplicable`.
- [ ] Inspection on the tangle world (`Synthetic::registerTangleWorld`) terminates for every name.
- [ ] Before and after any number of `blockers()` / `readiness()` calls, in any order, `verifyAgainstFresh(allNames)` is empty and no explicit run count has changed.

**Complexity:** M

---

### Task 4.6: Tests for the asynchronous request

**Purpose:** Prove acceptance 7 and 8 (engine half), cancellation, progress, failure classification, and lifetime safety, with compute driven inline and on real threads.

**Files to create:**
- `tests/support/asyncdriver.h` - header-only: `enum class ComputeMode { Inline, StdThread, QtThread }`; `ComputedCalculation computeOn(ComputeMode, PreparedCalculation &, CalculationProgress * = nullptr)` (runs `compute()` directly, on a joined `std::thread`, or on `QThread::create(...)` + `start()` + `wait()`); `void addComputeModeRows()` for `_data()` functions; `class RecordingProgress : public CalculationProgress` with a `std::atomic<bool>` cancel flag and a `QStringList` that is read only after the join.
- `tests/tst_calcengine_async.cpp` - class `CalcEngineAsyncTest`, a `World` like the one in `tst_calcengine.cpp` lines 29-51.

**Files to modify:**
- `tests/support/fakesessionstate.h` / `.cpp` - add `Synthetic::registerExplicitWorld(registry)` and its descriptors, documented in a table like the existing ones:

  | Id | Policy | Inputs | Outputs |
  |---|---|---|---|
  | `expA` (title "Explicit A") | Explicit | attr `EA_IN` | `EA1 = EA_IN + 1`, `EA2 = EA_IN * 2`, `EA_DIAG = "ok"`; when `EA_IN < 0`: `EA1`, `EA2` unavailable, `EA_DIAG = "rejected"`, reason `"negative input"` |
  | `derivA` | OnDemand | attr `EA1` | `DA = EA1 + 100` |
  | `derivA2` | OnDemand | attr `DA` | `DDA = DA + 1000` |
  | `expB` (title "Explicit B") | Explicit | attr `EA2`, attr `EB_IN` | `EB1 = EA2 + EB_IN` |
  | `derivB` | OnDemand | attr `EB1` | `DB = EB1 + 1` |

  Literals for `EA_IN = 4`, `EB_IN = 10`: `EA1 5`, `EA2 8`, `DA 105`, `DDA 1105`, `EB1 18`, `DB 19`.
- `tests/CMakeLists.txt` - add `support/asyncdriver.h` to `flysight_test_support`; `find_package(Threads REQUIRED)`; `flysight_add_test(tst_calcengine_async SOURCES tst_calcengine_async.cpp LIBS Threads::Threads)` directly under the other `tst_calcengine*` lines.

**Technical Approach:**
- Test functions (each `_data()` uses `addComputeModeRows()` unless noted): `asyncMatchesSync`, `outputsAppearTogether`, `namesReadWhileUnrequestedAreInvalidated`, `prepareCapturesInputs`, `inputChangeWhileRunningRefuses`, `transitiveChangesRefuse` (rows: on-demand intermediate, preference, registry change of an input candidate, `clear()`), `unrelatedChangeDoesNotRefuse`, `changeAfterPublicationDropsDependents`, `registrationRemovedRefuses`, `engineDestroyedRefuses`, `computeNeedsNoEngine`, `cancelPublishesNothing`, `progressTextReachesCaller`, `resourceExhaustionIsNotCached`, `ordinaryExceptionIsCachedFailure`, `undeclaredReadAndInvalidOutputMatchSync`, `missingInputPreparesNothing`, `cycleThroughOwnOutputMatchesSync`, `notExplicitIsRefused`, `blockedPrepareNamesBlocker`, `syncRequestInBetween`, `abandonedTicketLeavesNothing`.
- `inputChangeWhileRunningRefuses` and `cancelPublishesNothing` hold the worker *inside* compute with test-local `QSemaphore`s captured by a test-local descriptor ("entered" released by the worker, "proceed" released by the test), so the edit or the cancel provably happens while compute is running. Inline rows perform the edit between `prepare()` and `compute()` instead. Test-local synchronization is fine; the rule against locks concerns the library.
- `asyncMatchesSync` builds two identical `World`s, runs `request()` in one and prepare/compute/publish in the other, and compares everything listed in Task 4.4's first criterion, plus literal values.
- Expected values are literals (tests/README.md section 8); never derive an expectation from the code under test, except the explicit sync-vs-async comparison, which is the rule being tested.
- Compute functions that misbehave on purpose (throwing `std::bad_alloc` on the first call only) use a test-local `std::atomic<int>`; note in a comment that real compute functions may not hold state.

**Acceptance Criteria:**
- [ ] Every function above exists, passes in Debug and Release on Windows, and runs in well under the 120 s CTest timeout (no sleeps; semaphores only).
- [ ] Every criterion of Tasks 4.3 and 4.4 is asserted by at least one of them, with threaded rows where a worker is meaningful.
- [ ] `tst_calcengine_async` links only `flysight_test_support` and `Threads::Threads`.

**Complexity:** L

---

### Task 4.7: Tests for blocker inspection, the oracle, and a real session

**Purpose:** Prove acceptance 12 and 13 (engine half) and that inspection and asynchronous requests never disturb idempotency.

**Files to create:**
- `tests/tst_calcengine_blockers.cpp` - class `CalcEngineBlockersTest`: `derivedNameReportsExplicitBlocker`, `missingInputIsNotABlocker`, `chainedBlockers` (compute-mode rows), `ranAndDidNotProduce`, `failedCalculationIsNotProduced`, `availableThroughFallbackReportsNothing`, `storedAndUnknownNames`, `explicitFamilyInstance` (an explicit `CalculationFamily`; asserts `instanceOutput` round-trips into `prepare()`), `blockedWinsOverNotProduced`, `ringsTerminate`, `inspectionNeverRunsExplicit`, `readinessStates`.

**Files to modify:**
- `tests/tst_calcengine_oracle.cpp` - new `randomizedExplicitSequences_data()` / `randomizedExplicitSequences()` (seeds 1-25, 300 steps): shared world + explicit world; random reads, edits of `EA_IN` / `EB_IN` / `A`, `blockers()` and `readiness()` calls, synchronous `request()`, inline prepare/compute/publish, and prepare-edit-publish (expecting a refusal); after every step `verifyAgainstFresh` of a few random names, and explicit run counts change only in request / publish steps. The existing functions and their seeds are not touched.
- `tests/tst_calcregistry.cpp` - `titleLookup`.
- `tests/tst_session_engine.cpp` - `asyncRequestOnSession`, modelled on `explicitPolicyOnSession` (lines 534-590, `registerTemporary`): a published asynchronous request on a real `SessionData`; the session moved (`SessionData moved = std::move(session)`) between prepare and publish still publishes; destroyed gives `RefusedGone`; copy-assigned over gives `RefusedStale`; `setAttribute` on the declared input gives `RefusedStale`; `blockers()` on the outputs before and after.
- `tests/CMakeLists.txt` - `flysight_add_test(tst_calcengine_blockers SOURCES tst_calcengine_blockers.cpp LIBS Threads::Threads)`.

**Acceptance Criteria:**
- [ ] Every criterion of Task 4.5 is asserted with literal expectations.
- [ ] `randomizedExplicitSequences` passes for all seeds with `QT_HASH_SEED` fixed by `FLYSIGHT_TEST_MAIN`.
- [ ] `tst_session_engine` leaves the global registry as it found it (its `cleanup()` check still passes).

**Complexity:** M

---

### Task 4.8: Documentation and traceability

**Purpose:** Spec section 10: the engine documentation gains the asynchronous request, blocker inspection, and the threading rule.

**Files to modify:**
- `docs/CALCULATIONS.md` - in section 8 replace only the sentence "Nothing uses the policy yet; it exists for a future job queue." with a pointer to the new sections; append three sections at the end (own hunk), numbered 12-14 whatever other phases have already landed (overview, Integration Note 2): **12. Asynchronous request** (prepare / compute / publish, the outcome enums, who publishes `invalidated`, failure classification, `title`, `setReason`, the progress facility and `CalculationCancelled`, a 10-line usage sketch), **13. Blocker inspection** (the four states, the rules of spec 7.2, `readiness()`), **14. The threading rule** (the bullets of "Threading rule" above, including re-entrancy of explicit compute functions and "no locks: remove the sharing").
- `tests/README.md` - the executable count (add 2 to the current count, whatever it is), two rows in the "Calculation engine" table, `tst_session_engine` and `tst_calcengine_oracle` row text, and one sentence on `asyncdriver.h` in section 8.
- `tests/acceptance_map.txt` - append a comment-only block (the numbered lines belong to the previous plan's items 1-19 and the audit parses them; phase 10 owns the final map):

  ```
  # ---- Sensor fusion as an explicit calculation (PLANS/sensor-fusion-jobs.md) ----
  # Items of that specification are numbered 100 + n, so that they cannot be
  # confused with items 1-19 above.
  # SFJ 7  tst_calcengine_async asyncMatchesSync
  # SFJ 8  tst_calcengine_async inputChangeWhileRunningRefuses
  # SFJ 8  tst_calcengine_async changeAfterPublicationDropsDependents
  # SFJ 12 tst_calcengine_blockers derivedNameReportsExplicitBlocker
  # SFJ 13 tst_calcengine_blockers chainedBlockers
  ```

  The three header lines are Phase 2's form (02 Task 2.7); if another phase has already added them, reuse them instead of repeating them. The `# SFJ n` comment lines are converted to audited `100 + n` lines by Phase 10 (overview, Integration Note 1).

**Acceptance Criteria:**
- [ ] `audit_cleanup` passes (comment lines are skipped by `tests/audit/cleanup_audit.cmake` line 230).
- [ ] Every public type and function added in this phase is named in `docs/CALCULATIONS.md` with the thread it may be used on.
- [ ] The edits to the three shared documentation files are append-only except for the one replaced sentence and the README count/rows.

**Complexity:** S

## Testing Requirements

### Unit Tests
- New: `tst_calcengine_async`, `tst_calcengine_blockers` (Tasks 4.6, 4.7).
- Extended: `tst_calcengine_oracle::randomizedExplicitSequences`, `tst_calcregistry::titleLookup`.
- Unchanged and still passing: every other test, in particular `tst_calcengine::explicitPolicy`, `tst_calcengine_safety::requestThroughOwnOutputIsCycle`, `requestOnRingWithFallback`, `exceptionLeavesNoPartialResult`, which pin the synchronous semantics this phase must not alter.

### Integration Tests
- `tst_session_engine::asyncRequestOnSession` exercises the ticket against real `SessionData` ownership (move, destroy, copy-assign).
- `tst_python_bridge` (when enabled) must pass untouched: evidence that plugin calculations were not disturbed.
- Build `flysight_cpp_bridge` and the application: they link `flysight_model` and must not need new dependencies.

### Manual Verification
- `cmake --build build --config Release`, then `ctest --test-dir build -C Release -L core --output-on-failure`.
- Run `tst_calcengine_async` and `tst_calcengine_blockers` 50 times in a loop (Release and Debug) to shake out ordering assumptions in the threaded rows.
- Confirm with `git grep -n "QMutex\|std::mutex\|QReadWriteLock\|std::atomic" src/engine` that the library gained no lock and no atomic.

## Notes for Implementer

### Gotchas
- Withdraw the ticket's `Prepared` node before invalidating the dependents of the result node in `publish()`; and mark tickets stale in `clearCaches()` *before* wiping `m_dependents`.
- Never install ticket edges under `Result(instanceId)`: a read between prepare and publish re-caches `NotRequested` for that node and `setEdges` would wipe them. That is why the ticket has its own node kind.
- `prepare()` drops the cached `NotRequested` answers, so names read only *before* prepare are absent from the cache at publish time. They must come from the ticket (`m_droppedAtPrepare`), or plots that showed "unavailable" would never be told to re-read.
- Preparing calculation A invalidates resolutions that looked at A's `NotRequested` result; a ticket for another calculation that depends on such a resolution (A was a rejected candidate) goes stale. This is conservative and correct; with one job at a time it does not occur in practice.
- `CalculationCancelled` lands in `catch (...)` on the synchronous path and is cached as `Failed`; that is intended (the synchronous path has no cancel facility, so throwing it there is a bug in the calculation).
- On the `bad_alloc` path build no `QString`. Catch `std::bad_alloc` before `std::exception`.
- `EvaluationContext` is non-copyable and has a private constructor; the engine (a friend) creates it on the heap for the ticket. `PreparedCalculation` must be a friend to set the progress pointer and to take `m_undeclaredReads`.
- Log nothing from `compute()`. The undeclared-read warning for asynchronous runs is emitted by `acceptRun` on the main thread.
- `QThread::create` needs no event loop; `wait()` it before touching the ticket again.
- `GraphNode::Kind` gained a value: check every `switch` over it (`describe()`), and that `knownNodes()` callers are unaffected.
- Do not touch `src/calculations/*`, `src/pluginadapters.cpp`, `src/pluginhost.cpp`, `src/sessiondata.*`, or `src/sessionmodel.*` in this phase. Publishing `PublishOutcome::invalidated` through `SessionModel` is phase 5.
- Commits are made by the orchestrator only (overview "Commit Policy"); report the exact list of files created and modified.

### Decisions Made
- **The ticket is a graph node.** Staleness uses the same reverse edges and the same breadth-first invalidation as a published result, which is the literal reading of "from its own dependency records". A generation counter per engine was rejected: it would refuse on unrelated edits and make a long fit unpublishable while the user edits anything.
- **Prepare drops "not requested" answers like `request()` does.** Without it a ring through the calculation's own output would not be detected during prepare and the two paths would differ.
- **Non-runnable prepares publish like `request()`.** `MissingInput` and `Cycle` are functions of state; caching them keeps both paths identical and costs nothing.
- **`prepare()` accepts explicit calculations only.** On-demand and Python plugin compute functions can never reach a worker thread, which keeps the threading rule narrow. `request()` still accepts any policy.
- **Plain back-pointers, no shared control block.** Engine and ticket are both destroyed on the main thread, so each nulls the other; nothing needs reference counting or atomics.
- **Implicitly shared captures, no deep copy.** Allowed by overview Decision 5; a fusion input set is tens of megabytes and a deep copy buys nothing.
- **Synchronous `std::bad_alloc` stays a cached `Failed`.** The spec requires unchanged `request()` semantics; an on-demand read has no way to "not cache" without re-running on every read. Only the asynchronous path classifies resource exhaustion.
- **Reason text travels in the result** (`CalculationResult::setReason`, `resultDetail()`, `UnproducedNote::detail`, `PublishOutcome::detail`). The job queue and the row logic are generic and cannot parse a calculation-specific diagnostics attribute; phase 9 sets the reason from the same string it writes into `_FUSION_DIAGNOSTICS`.
- **`readiness()` is public.** Phase 5 needs "can a job be created for this session" (acceptance 11) without knowing an output name; it is the same routine `blockers()` uses per candidate.
- **All unavailable inputs are examined**, not only the first: a calculation with one blocked input and one genuinely missing input must not report a blocker, since requesting it could never help.
- **Run counters count installed runs only**, so counters after an asynchronous publish equal those after `request()`.
- **Acceptance map lines are comments** in this phase because the file's numeric items belong to the previous plan and the audit enforces items 1-19; phase 10 defines the final scheme.

### Open Questions
- None blocking. For phase 5 to decide: whether a job whose compute returned `Completed` after a cancel request publishes (correct and cheap) or ends as cancelled (what the user asked for). The engine supports both. **Answered by Phase 5: cancel wins** (overview, Integration Note 12).

## Definition of Done

This phase is complete when:
1. All tasks have passing acceptance criteria
2. All tests pass (`ctest -L core`, plus `audit_cleanup`), in Debug and Release on Windows
3. Code follows patterns established in reference files (comment density and structure of `src/engine/`, test conventions of `tests/README.md` section 8)
4. No TODOs or placeholder code remains
5. `flysight_model` still links Qt Core only, creates no thread, and contains no lock or atomic
6. No file under `src/calculations/`, no plugin file, and no existing test expectation was changed
