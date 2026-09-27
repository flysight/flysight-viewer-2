# Phase 2: Calculation engine core

## Overview

This phase builds the new calculation engine as standalone code inside the
`flysight_model` library, together with a complete Qt Test suite that drives it
with synthetic calculations against a fake session state. The engine implements
spec sections 7.1-7.9: registered calculations with declared inputs and
outputs, engine-owned per-session results, multi-output and partial results,
ordered candidates, resolution-time dependency recording (including rejected
candidates and absent stored values), parameterized calculation families,
nested scopes, cycle and exception safety, cross-session invalidation on
registry change, explicit policy, and test instrumentation.

Nothing in the application calls the engine yet. `CalculatedValue`,
`DependencyManager`, and `SessionData` are untouched and keep serving every
read until Phase 3. No application behavior changes.

## Dependencies

- **Depends on:** Phase 1.
- **Blocks:** Phase 3 (and transitively 4-8).
- **Assumptions:**
  - Work happens on branch `schema-and-calculations` (created from
    `v2026.04.1` in Phase 1), not on `master`. The old-engine files
    (`calculatedvalue.*`, `dependencymanager.*`, `dependencykey.h`,
    `sessiondata.cpp`, `src/calculations/*`) are identical at the tag and on
    `master`; `sessiondata.h` at the tag has no `DataSchema` namespace.
  - `flysight_model` (STATIC, Qt Core only; sources `sessiondata`,
    `dependencymanager`, `calculatedvalue`, `dependencykey.h`) and
    `flysight_core` exist as Phase 1 left them. `flysight_cpp_bridge` links
    `flysight_model` + Qt Core only.
  - `FLYSIGHT_BUILD_TESTS`, `tests/CMakeLists.txt`, `flysight_add_test()`,
    `flysight_test_support`, `FLYSIGHT_TEST_MAIN`, and the `tst_<area>` naming
    rule exist as specified in `01-baseline-and-test-harness.md`.
  - Phase 1's decision stands: engine code that `SessionData` will depend on
    lives in `flysight_model`, so it may use Qt Core only - no Qt Gui, no
    `PreferencesManager` (which is compiled into `flysight_core`), no
    GeographicLib, no Boost.

## Architecture summary (read before the tasks)

All new code is in namespace `FlySight`, in a new directory `src/engine/`,
compiled into `flysight_model`.

| File | Contents |
|---|---|
| `src/engine/calctypes.h` | `CalculationId`, `EvaluationPolicy`, `CalcInput`, `GraphNode`, `ResultStatus`, hashing/equality |
| `src/engine/sessionstate.h` | `ISessionState`, `IPreferenceProvider` (pure interfaces) |
| `src/engine/calculationresult.h/.cpp` | `CalculationResult` (the bundle a calculation returns) |
| `src/engine/evaluationcontext.h/.cpp` | `EvaluationContext` (declared-input read access) |
| `src/engine/calculationdescriptor.h` | `CalculationDescriptor`, `CalculationFamily`, `ComputeFunction` |
| `src/engine/calculationregistry.h/.cpp` | `CalculationRegistry` (global registrations, families, source-conversion hook, engine enrolment, preference broadcast) |
| `src/engine/calculationengine.h/.cpp` | `CalculationEngine` (per-session result store, dependency graph, resolution, invalidation, request, instrumentation, oracle) |

### Public-name type: `DependencyKey` is reused, unchanged

`src/dependencykey.h` and `src/dependencykey_bindings.cpp` are **not modified**
in this phase. `DependencyKey` (Attribute | Measurement) is exactly the
"public name" concept of spec 7.3, is already hashable, is already bound to
Python, and is the payload of `SessionModel::dependencyChanged` and all its
subscribers (`sessionmodel.cpp` 962-974, 1000-1012). The engine therefore uses
`DependencyKey` for (a) declared outputs and (b) the invalidated-name sets it
returns, so Phase 3 can forward those sets to `dependencyChanged` with no
conversion, and the bridge module keeps compiling with no edits.

What `DependencyKey` cannot express - preference inputs, the source layer,
graph-internal nodes - is expressed by two **new** types (`CalcInput`,
`GraphNode`) that never cross into the UI or Python in this phase. Phase 7
decides how `CalcInput` kinds are exposed to Python.

### Graph identities

`GraphNode` has these kinds (the overview's "Graph identities" decision):

| Kind | Identity | Role |
|---|---|---|
| `StoredAttribute` | attribute key | Leaf. Presence **and** value of the stored attribute. |
| `SourceMeasurement` | sensor, name | Leaf. Presence and samples of the recorded measurement. |
| `SourceUnit` | sensor, name | Leaf. Recorded unit text. |
| `Preference` | preference key | Leaf. |
| `Resolution` | `DependencyKey` public name | The candidate-choice node. What an ordinary read of that name returns: for a measurement this **is** the effective measurement; for an attribute, the stored-or-calculated attribute. |
| `Result` | calculation instance id | One calculation instance's whole result bundle. "Calculated attribute/measurement output" identities are (`Result` node, output name); the graph node is per instance because a bundle is published and invalidated atomically. |

Edges point from a dependent node to the nodes it looked at. Only
`Resolution` and `Result` nodes have outgoing edges and cache entries. Only a
`Resolution(measurement)` node and source-conversion `Result` nodes may have
edges to `SourceMeasurement` / `SourceUnit`; ordinary calculations cannot
declare source inputs (registration rejects it), which enforces "only the
conversion layer depends on source nodes".

### Availability (one definition, used everywhere)

- Attribute value: available iff the `QVariant` `isValid()`.
- Measurement value: available iff the vector is non-empty (matches the old
  engine, `calculatedvalue.cpp` 55-61).
- Preference: available iff the provider returns a valid `QVariant`.
- Source measurement: available iff present and non-empty. Source unit:
  available iff the source measurement is present (empty unit text is a value).
- A calculation output set to an invalid `QVariant` or an empty vector is
  normalized to "unavailable" (this is how the WS-P "clear" writes,
  `wspcalculations.cpp` 257-261, are expressed).
- A stored attribute always wins resolution even if its stored `QVariant` is
  invalid (then the name is unavailable, as today: `sessiondata.cpp` 27-30).

---

## Tasks

### Task 2.1: Core value types and the session-state interfaces

**Purpose:** Define the vocabulary every other engine file uses, and the narrow interface through which the engine sees a session.

**Files to create:**
- `src/engine/calctypes.h`
- `src/engine/sessionstate.h`

**Technical Approach:**

`calctypes.h` (header-only; include `"../dependencykey.h"`):

```cpp
using CalculationId = QString;               // stable identity, e.g. "builtin.analysisRange"
enum class EvaluationPolicy { OnDemand, Explicit };

struct CalcInput {
    enum class Kind { Attribute, Measurement, Preference, SourceMeasurement, SourceUnit };
    Kind kind; QString key;            // Attribute / Preference key
    QString sensor, name;              // Measurement / Source* identity
    static CalcInput attribute(const QString &key);
    static CalcInput measurement(const QString &sensor, const QString &name);
    static CalcInput preference(const QString &key);
    static CalcInput sourceMeasurement(const QString &sensor, const QString &name);
    static CalcInput sourceUnit(const QString &sensor, const QString &name);
};

struct GraphNode {
    enum class Kind { StoredAttribute, SourceMeasurement, SourceUnit, Preference, Resolution, Result };
    Kind kind; QString a, b;           // key | sensor,name | instance id; Resolution uses name + isMeasurement
    bool measurementName = false;      // Resolution only
    static GraphNode storedAttribute(const QString &key);
    static GraphNode sourceMeasurement(const QString &sensor, const QString &name);
    static GraphNode sourceUnit(const QString &sensor, const QString &name);
    static GraphNode preference(const QString &key);
    static GraphNode resolution(const DependencyKey &name);
    static GraphNode result(const QString &instanceId);
    DependencyKey publicName() const;  // Resolution only
};

enum class ResultStatus { Ok, MissingInput, NotRequested, Cycle, Failed, UndeclaredRead, InvalidOutput };
```

Provide `operator==` and `size_t qHash(const T&, size_t seed)` for `CalcInput`
and `GraphNode` (use `qHashMulti`). Do not touch `DependencyKey`'s existing
`uint qHash` - it already works in `QSet` under Qt 6.

Instance id convention (a plain `QString`): for a plain calculation the
instance id **is** its `CalculationId`; for a family instance it is
`familyId + QLatin1Char('#') + instanceKey`. `'#'` is therefore forbidden in a
`CalculationId` (checked at registration).

`sessionstate.h`:

```cpp
class ISessionState {
public:
    virtual ~ISessionState() = default;
    virtual bool            hasStoredAttribute(const QString &key) const = 0;
    virtual QVariant        storedAttribute(const QString &key) const = 0;
    virtual bool            hasSourceMeasurement(const QString &sensor, const QString &name) const = 0;
    virtual QVector<double> sourceMeasurement(const QString &sensor, const QString &name) const = 0;
    virtual QString         sourceUnit(const QString &sensor, const QString &name) const = 0;
};

class IPreferenceProvider {
public:
    virtual ~IPreferenceProvider() = default;
    virtual QVariant preferenceValue(const QString &key) const = 0;   // invalid QVariant = no such preference
};
```

Contract of `ISessionState` (document it in the header): every method is a
pure read of persistent state; none may call back into the engine; none may
compute. Whoever mutates that state must afterwards call the matching
`CalculationEngine` notification (Task 2.5). Phase 3 implements the interface
on `SessionData` (`m_attributes`, `m_sensors`, `m_units`); Phase 4 keeps the
same interface while `SessionData`'s *public* getters become effective reads.

Preference change notification is **push**, not part of `IPreferenceProvider`:
the owner of the real preferences calls
`CalculationRegistry::notifyPreferenceChanged(key)` (Task 2.3). Phase 3 will add,
in `flysight_core`, an adapter that implements `IPreferenceProvider` over
`PreferencesManager::getValue` and connects
`PreferencesManager::preferenceChanged` (`preferencesmanager.h` 69) to that
call. `flysight_model` thereby never includes `preferencesmanager.h`.

**Acceptance Criteria:**
- [ ] Both headers compile including only Qt Core headers and `dependencykey.h`.
- [ ] `GraphNode` and `CalcInput` work as `QSet` / `QHash` keys; two nodes of different kind with equal strings compare unequal (e.g. `storedAttribute("x") != preference("x")`, `sourceMeasurement("IMU","wx") != resolution(DependencyKey::measurement("IMU","wx"))`).
- [ ] `src/dependencykey.h` and `src/dependencykey_bindings.cpp` are byte-identical to Phase 1's.

**Complexity:** S

---

### Task 2.2: Descriptor, result bundle, and evaluation context

**Purpose:** Define what a registered calculation is, what it returns, and the only window it has onto session state.

**Files to create:**
- `src/engine/calculationdescriptor.h`
- `src/engine/calculationresult.h` / `.cpp`
- `src/engine/evaluationcontext.h` / `.cpp`

**Technical Approach:**

```cpp
using ComputeFunction = std::function<CalculationResult(const EvaluationContext &)>;

struct CalculationDescriptor {
    CalculationId          id;
    QList<CalcInput>       inputs;        // all required (spec 7.1); order = availability-check order
    QList<DependencyKey>   outputs;       // >= 1
    EvaluationPolicy       policy = EvaluationPolicy::OnDemand;
    ComputeFunction        compute;
};

struct CalculationFamily {
    CalculationId id;
    EvaluationPolicy policy = EvaluationPolicy::OnDemand;
    // Pure function of the name. Returns nullopt when the name is not an output of this
    // family. Otherwise returns the instance: a descriptor whose `outputs` contains `name`
    // and whose `id` field holds the *instanceKey* (canonical parameter text, no '#').
    std::function<std::optional<CalculationDescriptor>(const DependencyKey &name)> instantiate;
};
```

Rules for `instantiate` (document in the header): deterministic; two names that
belong to the same instance must return descriptors with the same instanceKey,
inputs, and outputs; the lambda captures the parameters in `compute`. Example
for Phase 3's interpolation family: name `"{t}:{s}/{tv}/{m}"` parsed exactly as
`sessiondata.cpp` 172-183 -> instanceKey = the name, inputs
`attribute(t)`, `measurement(s,tv)`, `measurement(s,m)`, one output.

`CalculationResult` - a value type holding per-output values:

```cpp
class CalculationResult {
public:
    CalculationResult() = default;                              // everything unavailable
    static CalculationResult unavailable();                     // same, reads better at call sites
    CalculationResult &setAttribute(const QString &key, const QVariant &value);
    CalculationResult &setMeasurement(const QString &sensor, const QString &name,
                                      const QVector<double> &values, const QString &unit = QString());
    CalculationResult &setUnavailable(const DependencyKey &output);   // explicit form of "not set"
    bool     contains(const DependencyKey &output) const;       // was set (available or not)
    bool     isAvailable(const DependencyKey &output) const;
    QVariant attributeValue(const QString &key) const;
    QVector<double> measurementValues(const QString &sensor, const QString &name) const;
    QString  measurementUnit(const QString &sensor, const QString &name) const;
    QList<DependencyKey> setOutputs() const;
};
```

Any declared output not set is unavailable. Invalid `QVariant` / empty vector
are normalized to unavailable at `set*` time.

`EvaluationContext` - non-copyable, constructed only by the engine
(`friend class CalculationEngine`), valid only during one `compute` call:

```cpp
class EvaluationContext {
public:
    QVariant        attribute(const QString &key) const;
    QVector<double> measurement(const QString &sensor, const QString &name) const;
    QString         measurementUnit(const QString &sensor, const QString &name) const; // of a declared Measurement input
    QVariant        preference(const QString &key) const;
    QVector<double> sourceMeasurement(const QString &sensor, const QString &name) const;
    QString         sourceUnit(const QString &sensor, const QString &name) const;
};
```

Each accessor checks the read against the descriptor's declared `inputs`. A
declared read returns the value the engine already resolved during the
availability pass (Task 2.4) - it never starts a new resolution, so `compute`
calls never nest inside each other. An **undeclared** read: returns an
invalid/empty value, records a violation `{instanceId, CalcInput}` on the
engine, emits one `qWarning`, and marks the evaluation so that the engine
discards whatever `compute` returns and publishes status `UndeclaredRead` (all
outputs unavailable). No C++ exception is thrown for this (the Python bridge
will call these accessors in Phase 7). There is deliberately no accessor for
the session, the engine, "which output was requested", the clock, or RNG.

**Acceptance Criteria:**
- [ ] `CalculationResult` default-constructed reports every name unavailable; `setAttribute(k, QVariant())` and `setMeasurement(s, n, {})` leave that output unavailable while `contains()` is true.
- [ ] `EvaluationContext` has no public constructor and exposes no pointer/reference to `CalculationEngine`, `ISessionState`, or `SessionData`.
- [ ] None of the three headers includes `sessiondata.h`, `calculatedvalue.h`, or `dependencymanager.h`.

**Complexity:** M

---

### Task 2.3: `CalculationRegistry`

**Purpose:** Hold global, session-free registrations in deterministic order, instantiate families, and fan registry/preference changes out to every loaded session's engine.

**Files to create:**
- `src/engine/calculationregistry.h` / `.cpp`

**Technical Approach:**

```cpp
struct CalculationInstance {                      // what the engine consumes
    QString instanceId;                           // id, or familyId#instanceKey
    CalculationId registrationId;                 // id or familyId (for counters / unregister)
    std::shared_ptr<const CalculationDescriptor> descriptor;   // descriptor->id == instanceId
    bool sourceConversion = false;
};

class CalculationRegistry {
public:
    static CalculationRegistry &instance();        // process-wide registry used by the app from Phase 3
    CalculationRegistry();                          // tests construct private registries
    ~CalculationRegistry();                         // asserts no engine is still enrolled

    bool registerCalculation(const CalculationDescriptor &d);
    bool registerFamily(const CalculationFamily &f);
    bool registerSourceConversion(const CalculationFamily &f);   // ordered; see below
    bool unregister(const CalculationId &id);                    // calculation, family, or conversion family

    bool contains(const CalculationId &id) const;
    bool hasCandidateFor(const DependencyKey &name) const;       // Phase 3: hasRegisteredCalculation()
    QList<CalculationInstance> candidatesFor(const DependencyKey &name) const;        // registration order
    QList<CalculationInstance> sourceConversionsFor(const QString &sensor, const QString &name) const;
    bool hasSourceConversions() const;
    std::optional<CalculationInstance> instance(const CalculationId &id,
                                                const DependencyKey &instanceOutput = DependencyKey()) const;

    void setPreferenceProvider(const IPreferenceProvider *p);    // not owned; may be null
    const IPreferenceProvider *preferenceProvider() const;
    void notifyPreferenceChanged(const QString &key);            // broadcast to enrolled engines

private:
    friend class CalculationEngine;
    void enrol(CalculationEngine *e);                            // called by engine ctor
    void withdraw(CalculationEngine *e);                         // called by engine dtor
};
```

- **Order.** Every successful registration takes the next value of a
  monotonically increasing sequence number. `candidatesFor(name)` returns, in
  sequence order, every plain calculation whose `outputs` contains `name` and
  every family whose `instantiate(name)` returns a value. There are no
  priorities. Re-registering after `unregister` goes to the end.
- **Validation** (`register*` returns `false` and `qWarning`s; nothing is
  registered): empty id; id containing `'#'`; id already registered (as any
  kind); no outputs; duplicate outputs; null `compute` / `instantiate`; an
  output that is also one of the calculation's own `Attribute`/`Measurement`
  inputs; a `SourceMeasurement` / `SourceUnit` input on anything not registered
  through `registerSourceConversion`; a call made while any enrolled engine is
  mid-evaluation (`Q_ASSERT` in debug as well). Family instances are validated
  with the same rules on first instantiation; an invalid instance is treated
  as "family does not match" plus a warning.
- **Family memoization.** Instances are memoized per `(familyId, instanceKey)`
  in the registry (this is registration-derived, not session state) and the
  memo for a family is dropped on `unregister`. The `descriptor->id` of an
  instance is rewritten to the full instance id.
- **Source conversion hook (consumed by Phase 4).** Conversion families are an
  ordered candidate list for resolution step 1 of measurements. Their
  `instantiate` receives `DependencyKey::measurement(sensor, name)`. Being a
  list allows Phase 4 to express "with `SCHEMA_VER`" and "without `SCHEMA_VER`"
  as two candidates, since all declared inputs are required (spec 7.1).
- **Enrolment and broadcast.** The registry keeps a `QList<CalculationEngine*>`.
  After a successful `register*` / `unregister`, for each enrolled engine it
  calls `engine->onRegistryChanged(change)` where `change` carries: the
  registration id, whether it was added or removed, the explicit output names
  (plain calculation) or the family's `instantiate` function (family /
  conversion). `notifyPreferenceChanged(key)` calls
  `engine->onPreferenceChanged(key)` on each. Engine-side behavior is Task 2.5.
  This is what fixes the `AltitudeMarkerManager::refresh()` stale-value bug
  (`altitudemarkerfeature.cpp` 149-160 unregisters globally and flushes no
  session).
- The registry holds no per-session data and never runs a calculation.

**Acceptance Criteria:**
- [ ] `candidatesFor` returns candidates in registration order, interleaving plain calculations and family instances by sequence number.
- [ ] Every validation rule above rejects with `false` and leaves `contains(id)` false.
- [ ] Two names of the same family instance yield the same `instanceId` and the same `descriptor` pointer.
- [ ] After `unregister(familyId)`, `candidatesFor` no longer returns its instances and the memo is empty.
- [ ] A registry constructed locally in a test shares nothing with `CalculationRegistry::instance()`.

**Complexity:** M

---

### Task 2.4: `CalculationEngine` - result store, resolution, and dependency recording

**Purpose:** Implement spec 7.2-7.6: the per-session cache, the resolution rule, recording of everything a resolution looked at, negative caching, and atomic publication.

**Files to create:**
- `src/engine/calculationengine.h` / `.cpp`

**Technical Approach:**

```cpp
class CalculationEngine {
public:
    CalculationEngine(const ISessionState *state,
                      CalculationRegistry *registry = &CalculationRegistry::instance());
    ~CalculationEngine();                                   // withdraws from the registry
    Q_DISABLE_COPY_MOVE(CalculationEngine)
    void rebind(const ISessionState *state);                // same logical state at a new address; keeps caches

    // Ordinary reads (may compute)
    QVariant        attribute(const QString &key);
    QVector<double> measurement(const QString &sensor, const QString &name);
    QString         measurementUnit(const QString &sensor, const QString &name);
    bool            isAvailable(const DependencyKey &name);

    // Tasks 2.5 / 2.7 / 2.8 add: notifications, listener, request(), inspection, instrumentation, oracle
};
```

`rebind` exists because `SessionData` is a copyable/movable value type; Phase 3
decides ownership (engine held by pointer inside `SessionData`, a copied
`SessionData` gets a fresh engine).

**Cache contents.**
- `Resolution` entry: provider (`Stored`, `Source`, `Calculation{instanceId}`,
  `None`), availability, and the value (`QVariant`, or `QVector<double>` +
  unit). Values are Qt implicitly-shared, so a passthrough measurement costs no
  second buffer (spec 5.3).
- `Result` entry: `ResultStatus`, a `std::shared_ptr<const CalculationResult>`,
  and `bool requested`.
- Forward edges `QHash<GraphNode, QSet<GraphNode>> m_dependsOn` and reverse
  edges `m_dependents`.

**Evaluation scopes.** A `std::vector<Scope>` stack; `Scope { GraphNode node;
QSet<GraphNode> looked; bool cycle = false; }`. Every read of a node made while
a scope is open inserts that node into the **top** scope's `looked` set -
whether the node was served from cache, freshly evaluated, found absent, or
found to be on the stack (cycle). There is no shared side-effect list
(contrast `calculatedvalue.h` 55). A scope is pushed/popped by an RAII guard.

**`resolve(name)`** (private; all public reads go through it):

1. `R = Resolution(name)`. Note `R` in the enclosing scope, if any.
2. If `R` is cached, return the entry.
3. If `R` is already on the scope stack -> cycle handling (Task 2.6); return
   "unavailable" without caching anything for `R`.
4. Push scope `R`.
5. **Stored-first.**
   - Attribute: note `StoredAttribute(key)` **unconditionally** (this is the
     "absence of a stored value" dependency). If `state->hasStoredAttribute`,
     provider = `Stored`, value = `storedAttribute(key)`; go to 8.
   - Measurement: note `SourceMeasurement(sensor,name)` unconditionally. If
     `state->hasSourceMeasurement`:
     - no conversion families registered: provider = `Source`, value = the
       source samples, unit = source unit; also note `SourceUnit(sensor,name)`.
     - otherwise: iterate `registry->sourceConversionsFor(sensor,name)` exactly
       as step 6; if none produces the output the name is unavailable
       (provider `None`). It never falls through to derived candidates
       (spec 7.3 rule 1).
     Go to 8.
6. **Candidates in registration order.** For each
   `registry->candidatesFor(name)`: `ensureResult(instance)` (which notes
   `Result(instanceId)` in `R`'s scope - so **rejected earlier candidates are
   dependencies of `R`**). If the result's status is `Ok` and the bundle has
   `name` available, provider = `Calculation{instanceId}`; stop. A candidate
   that ran but reported this particular output unavailable is simply not the
   winner; the next candidate is tried.
7. No winner: provider = `None`, unavailable. This is cached too (negative).
8. Pop scope; **publish**: in one step store the `Resolution` entry and replace
   `m_dependsOn[R]` with the scope's `looked` set (updating `m_dependents`).

**`ensureResult(instance)`** (private):

1. `C = Result(instanceId)`; note `C` in the enclosing scope.
2. Cached -> return it. On the stack -> cycle handling, return unavailable.
3. Push scope `C`.
4. If `descriptor->policy == Explicit` and this call does not come from
   `request()` (Task 2.7): status `NotRequested`; go to 7 (no inputs looked at).
5. **Availability pass**, inputs in declared order, stopping at the first
   unavailable one (short-circuit is sound: while that input stays
   unavailable the answer cannot change, and the input is recorded):
   - `Attribute` / `Measurement`: `resolve(...)` recursively - this is where
     nested evaluation happens - available per the definition above.
   - `Preference`: note `Preference(key)`; read
     `registry->preferenceProvider()` (null provider = unavailable).
   - `SourceMeasurement` / `SourceUnit`: note the leaf; read `state`.
   If any is unavailable: status `MissingInput`; go to 7. If this scope was
   marked `cycle` during the pass: status `Cycle`; go to 7.
   Availability is thus decided only by persistent state and recursive
   resolution. A cached node may short-cut the *work*, never the *answer*,
   because a cache entry exists only while nothing it looked at has changed.
6. **Run.** Increment the run counter (Task 2.8). Build an `EvaluationContext`
   over the descriptor's inputs and call `compute` inside
   `try { ... } catch (const std::exception &) { ... } catch (...) { ... }`.
   - exception -> status `Failed`, `qWarning` with the id (and `what()`).
   - undeclared read flagged -> status `UndeclaredRead`.
   - bundle sets an output that is not declared, or sets a declared attribute
     output via `setMeasurement` (or vice versa) -> status `InvalidOutput`.
   - otherwise status `Ok` with the bundle as returned.
   For every non-`Ok` status the stored bundle is empty (all outputs
   unavailable): a failing calculation never publishes a partial result.
7. Pop scope; publish `Result` entry + edges in one step, exactly as step 8
   above. All statuses, including the unavailable ones, are cached with their
   dependencies and are not re-run until one of them is invalidated
   (spec 7.2).

**Atomic multi-output publication** follows from the structure: a bundle is
one immutable `shared_ptr` swapped in at step 7, and every output's
`Resolution` node depends on that single `Result` node, so an invalidation of
the result drops all of them together. A consumer can never combine outputs of
two runs.

**User override of one output (spec 7.3, acceptance 11).** With calculation
`M` declaring outputs `{A, B, C}` and a stored attribute `B`:
`Resolution(B)` stops at step 5 and has edges only to `StoredAttribute(B)`;
`Resolution(A)` and `Resolution(C)` reach `Result(M)` through step 6 as usual.
`M` does not depend on `Resolution(B)` (a calculation may not declare its own
output as input), so no cycle exists, and `M` still computes its own `B`, which
is simply never selected while the stored value exists. Removing the stored
`B` invalidates `StoredAttribute(B)` -> `Resolution(B)` only; the next read of
`B` finds `Result(M)` still cached and does **not** re-run `M`.

**Public reads.** `attribute()` / `measurement()` / `measurementUnit()` /
`isAvailable()` call `resolve` with an empty scope stack (or, if called
re-entrantly from inside a `compute` - which is a bug in the calculation -
`Q_ASSERT` and return unavailable without resolving). Unavailable reads return
an invalid `QVariant` / empty vector / empty string.

**Acceptance Criteria:**
- [ ] A stored attribute/measurement is returned without consulting any candidate (run counters stay 0) and `Resolution(name)` has an edge to the stored/source leaf.
- [ ] With candidates `[c1, c2]` for one name, `c1`'s input absent: read returns `c2`'s value, and `m_dependsOn[Resolution(name)]` contains `StoredAttribute(name)`, `Result(c1)`, and `Result(c2)`; `Result(c1)` has status `MissingInput` and an edge to the missing input's `Resolution` node.
- [ ] A candidate that runs and reports the requested output unavailable is skipped in favor of the next candidate; its other available outputs remain readable from the same run.
- [ ] A three-output calculation's `compute` is invoked exactly once across reads of all three outputs in any order, repeated any number of times.
- [ ] An unavailable result (`MissingInput`, `Failed`, `Ok` with all outputs unavailable) is not re-run by repeated reads.
- [ ] `grep` shows no `static`/global mutable per-session data in `calculationengine.cpp`.

**Complexity:** L

---

### Task 2.5: Invalidation, mutation notifications, and cross-session broadcast

**Purpose:** One transitive, computation-free invalidation path for state edits, preference changes, and registry changes, with edge cleanup, returning the public names Phase 3 turns into `SessionModel::dependencyChanged`.

**Files to modify:**
- `src/engine/calculationengine.h` / `.cpp`

**Technical Approach:**

```cpp
    // Called by the owner of the state AFTER it has mutated the state.
    QSet<DependencyKey> attributeChanged(const QString &key);                        // set or remove
    QSet<DependencyKey> sourceMeasurementChanged(const QString &sensor, const QString &name);
    QSet<DependencyKey> sourceUnitChanged(const QString &sensor, const QString &name);
    QSet<DependencyKey> clear();                                                     // drop every cache entry and edge

    // Broadcast-originated invalidations (registry / preference changes) have no caller to
    // return a set to; they are delivered here. Direct notifications above do NOT call it.
    using InvalidationListener = std::function<void(const QSet<DependencyKey> &)>;
    void setInvalidationListener(InvalidationListener l);

private:
    friend class CalculationRegistry;
    void onRegistryChanged(const RegistryChange &change);
    void onPreferenceChanged(const QString &key);
```

**`invalidate(seedNodes)`** (private), breadth-first like
`dependencymanager.cpp` 17-52 but over `GraphNode`:

- For each visited node `n`: remove its cache entry (if any); take
  `m_dependsOn[n]` and remove `n` from `m_dependents[d]` for each `d` in it
  (erase empty sets) - **edge cleanup**, which the old engine never did; then
  enqueue every node in `m_dependents[n]` not yet visited. Propagation does
  not depend on whether `n` itself was cached.
- It never calls `resolve`, `ensureResult`, `compute`, the state, or the
  preference provider. **Invalidation never triggers computation.**
- Returns the `publicName()` of every visited `Resolution` node.

**Seeds and returned sets.**
- `attributeChanged(key)`: seed `StoredAttribute(key)`; the returned set always
  also contains `DependencyKey::attribute(key)` itself, even if nothing was
  cached (keeps today's contract, `dependencymanager.cpp` 26-27, which
  `SessionModel::updateAttribute` relies on).
- `sourceMeasurementChanged(s,n)`: seed `SourceMeasurement(s,n)`; always
  includes `DependencyKey::measurement(s,n)`.
- `sourceUnitChanged(s,n)`: seed `SourceUnit(s,n)`; always includes the
  measurement name.
- `onPreferenceChanged(key)`: seed `Preference(key)`; nothing added. If the
  resulting set is non-empty, call the listener. A preference no calculation
  declared (for example one that is only snapshotted into a session attribute
  at import, spec 7.8) has no dependents, so the set is empty, nothing is
  invalidated, and the listener is not called.
- `onRegistryChanged`:
  - removed calculation/family: seed every cached `Result` node whose
    `registrationId` matches (so no cache entry outlives its registration),
    plus `Resolution(name)` for each explicit output;
  - added or removed **family** (incl. conversion families): additionally seed
    every currently cached `Resolution` node whose name the family's
    `instantiate` accepts (for a conversion family, measurement names only);
  - added plain calculation: seed `Resolution(name)` for each output, so a
    cached fallback or a cached `None` re-resolves and can pick up the new
    candidate.
  Deliver the set through the listener if non-empty.

Only names that have actually been resolved appear in a set (beyond the
changed name itself). That is sufficient for the UI: a subscriber can only be
showing a value it has read, and every read - including an unavailable one -
leaves a cached `Resolution` node.

**Notifications during evaluation.** A notification arriving while the scope
stack is non-empty (a calculation or state object misbehaving) is a
programming error: `Q_ASSERT` in debug; in release the seeds are queued and
applied when the outermost scope unwinds, and the resulting set goes to the
listener. Never invalidate in the middle of an evaluation.

**Acceptance Criteria:**
- [ ] After any notification, every run counter is unchanged and no `ISessionState` / `IPreferenceProvider` method was called (checked with a counting fake).
- [ ] Invalidation is transitive across a chain of three calculations and returns all three public names plus the changed name.
- [ ] After `attributeChanged` on an input, the sum of all edge-set sizes for the invalidated nodes is 0; repeating *edit + read* 1000 times leaves `edgeCount()` equal to its value after the first read.
- [ ] Adding the missing input of a rejected preferred candidate returns a set containing the public name that had fallen back; the next read selects the preferred candidate; the fallback calculation is not re-run.
- [ ] Unregistering a calculation with two engines enrolled removes its cached results from both, each engine's listener receives the affected names, and no computation runs. Registering a calculation for a name cached as `None` invalidates that name in both.

**Complexity:** M

---

### Task 2.6: Safety - scope stack discipline, cycles, exceptions, undeclared reads

**Purpose:** Guarantee spec 7.5's "cycles are detected and reported, evaluation unwinds cleanly, an exception leaves no partial result and no corrupted scope" without breaking the idempotency invariant (7.4).

**Files to modify:**
- `src/engine/calculationengine.h` / `.cpp`

**Technical Approach:**

**Cycle semantics (decision).** A cycle is detected when `resolve` or
`ensureResult` is asked for a node that is already on the scope stack at index
`i`. Then:

1. The re-entrant read is recorded as looked-at in the top scope (so the edge
   exists) and returns **unavailable**.
2. Every `Result` scope at stack index `>= i` is marked `cycle = true`. A
   marked calculation stops its availability pass as soon as control returns
   to it, does **not** run, and is published with status `Cycle` (all outputs
   unavailable) together with the dependencies recorded so far.
3. `Resolution` scopes inside the cycle are not marked: they continue down
   their candidate list, so a name whose preferred candidate is cyclic falls
   back to a later candidate or to unavailable.
4. The cycle is reported once per detection: `qWarning` with the node path,
   `cycleCount()` incremented, `lastCyclePath()` recorded (Task 2.8). It is a
   *registration error*, never a crash, never an exception.

Why "every calculation in the cycle is unavailable" rather than "only the
closing edge fails" (which is what `calculatedvalue.cpp` 35-38 does): with the
old rule the answer depends on where the cycle was entered. Example: `X` has
candidates `[P(input Y), Q()]`, `Y` has `[R(input X), S()]`. Reading `X` first
under the old rule gives `X = P(S)`; reading `Y` first gives `X = Q`. Under the
new rule `P` and `R` are both `Cycle` from either entry point, so `X = Q` and
`Y = S` always - the value is a function of state and registry, as 7.4
requires. Because every edge of the ring is recorded, any change that could
break the cycle (for example the user storing `Y`) invalidates the whole ring
and the next read re-resolves.

**What this means for Phase 3 (informative).** The declared-input graph of the
built-ins is acyclic: analysis range <- `GNSS/hMSL`, `GNSS/_time`
(`attributecalculations.cpp` 91-109); `_GROUND_ELEV` <- `_ANALYSIS_END_TIME`
(572-578); `_LANDING_TIME` <- analysis range + `_GROUND_ELEV` (375-386);
`GNSS/z` <- `_GROUND_ELEV` (`gnsscalculations.cpp` 19). The old engine's
re-entrancy comes from the multi-output helpers reading their own key while it
is in `m_activeCalculations` (`attributecalculations.cpp` 84 and 343,
`timecalculations.cpp` 88, `wspcalculations.cpp` 264) and from SP's
`_SP_WINDOW_START_ALT` recipe forcing `_SP_WINDOW_START_TIME` and then reading
itself (`spcalculations.cpp` 89-101). All of these disappear when each group
becomes one calculation returning a bundle (`_SP_WINDOW_START_TIME` +
`_SP_WINDOW_START_ALT` = one two-output calculation). Phase 3 must not rely on
cycle fallback for correct built-in behavior; if it finds a genuine cycle it
must break it with ordered candidates, and the semantics above tell it exactly
what a user would see meanwhile.

**Exception safety.**
- Scope push/pop is RAII, so the stack depth after any public call equals the
  depth before it, whatever is thrown.
- Exceptions from `compute` are caught in `ensureResult` step 6 -> status
  `Failed`, cached with the input dependencies (so it is retried only after an
  input changes). Callers of the engine never see a calculation's exception.
- An exception from anywhere else (e.g. `std::bad_alloc` inside the engine)
  propagates to the caller, but the guards pop the scopes and nothing is
  published for the nodes that were open: publication is the last, non-throwing
  step (prepare the new entry and edge sets first, then swap/insert).
- `family.instantiate` throwing is caught in the registry and treated as "does
  not match".

**Undeclared reads.** As specified in Task 2.2; the engine keeps
`undeclaredReadCount()` and `lastUndeclaredRead()`.

**Re-entrancy guards.** Public reads from inside `compute`, registration
during evaluation, and notifications during evaluation are handled as
specified in Tasks 2.4, 2.3, 2.5.

**Acceptance Criteria:**
- [ ] The `P/Q/R/S` example yields `X == Q's value` and `Y == S's value` for both read orders, with `cycleCount() >= 1`, `runCount(P) == runCount(R) == 0`.
- [ ] Storing `Y` afterwards invalidates `X`; the next read gives `X == P(stored Y)`.
- [ ] A self-dependent pair with no fallbacks returns unavailable for both names, terminates, and leaves `scopeDepth() == 0`.
- [ ] A calculation that throws `std::runtime_error` (and one that throws `int`) yields unavailable for all of its outputs, including outputs it had already `set` on the bundle before throwing; `scopeDepth() == 0`; a nested outer calculation depending on it gets `MissingInput`; the thrower is not re-run until its input changes, then runs exactly once more.
- [ ] An undeclared `ctx.attribute("other")` makes all outputs unavailable, `undeclaredReadCount() == 1`, and `lastUndeclaredRead()` names the calculation and the input.

**Complexity:** M

---

### Task 2.7: Explicit evaluation policy and the request operation

**Purpose:** Provide spec 7.7's flag and synchronous request-by-identity so a future job queue needs no engine redesign.

**Files to modify:**
- `src/engine/calculationengine.h` / `.cpp`

**Technical Approach:**

```cpp
    struct RequestOutcome { bool found; ResultStatus status; QSet<DependencyKey> invalidated; };
    RequestOutcome request(const CalculationId &id,
                           const DependencyKey &instanceOutput = DependencyKey());  // 2nd arg selects a family instance
```

- Works for any policy; for `OnDemand` it is simply an eager evaluation.
- Unknown id / non-matching family name: `found = false`, nothing changes.
- If `Result(instance)` is cached with a status other than `NotRequested`:
  return it (`invalidated` empty) - a valid result is never recomputed.
- Otherwise: evaluate via `ensureResult` with the "from request" flag
  (availability pass, run, all safety rules). Before publishing, collect the
  dependents of `Result(instance)` (the `Resolution` nodes that cached
  `NotRequested`) and invalidate **them** (not the result node). Then publish
  the result with `requested = true`. All outputs therefore appear at once.
  `invalidated` is returned to the caller (a future model-level caller forwards it to
  `dependencyChanged`; no phase of this plan calls `request()` from the model); the listener is not called.
- When an input later changes, the result is invalidated like any other and
  the calculation is back to `NotRequested` until requested again. Reads never
  start it.
- Nothing is migrated to `Explicit` in this change.

**Acceptance Criteria:**
- [ ] Before `request`, reading each output of an explicit calculation returns unavailable, `runCount == 0`, and a later on-demand candidate for the same name (if any) wins.
- [ ] `request` runs it once; `invalidated` contains every output name previously read; all outputs are then readable with `runCount == 1`; a second `request` does not run it again.
- [ ] After a declared input changes, outputs read as unavailable again with no run until the next `request`.

**Complexity:** S

---

### Task 2.8: Inspection, instrumentation, and the fresh-evaluation oracle

**Purpose:** Spec 7.2 ("inspecting never triggers computation") and 7.9 (run counters; cached answer versus fresh evaluation).

**Files to modify:**
- `src/engine/calculationengine.h` / `.cpp`

**Technical Approach:**

```cpp
    // Inspection - const, never resolves, never computes, never touches state/provider
    enum class CachedState { NotCached, Available, Unavailable };
    CachedState cachedState(const DependencyKey &name) const;
    std::optional<ResultStatus> resultStatus(const CalculationId &id,
                                             const DependencyKey &instanceOutput = DependencyKey()) const;

    // Instrumentation (per engine, always compiled - cheap integer bookkeeping)
    int  runCount(const CalculationId &registrationId) const;        // family: sum over instances
    int  runCountForInstance(const QString &instanceId) const;
    int  totalRunCount() const;
    void resetRunCounts();
    int  cycleCount() const;            QList<GraphNode> lastCyclePath() const;
    int  undeclaredReadCount() const;   std::pair<QString, CalcInput> lastUndeclaredRead() const;
    int  scopeDepth() const;            // 0 outside evaluation
    int  edgeCount() const;             // total forward edges
    int  cachedNodeCount() const;
    QSet<GraphNode> dependenciesOf(const GraphNode &n) const;        // for tests

    // Oracle
    struct Value { bool available; QVariant attribute; QVector<double> samples; QString unit; };
    Value evaluateFresh(const DependencyKey &name) const;
    static bool sameValue(const Value &a, const Value &b);
    QList<DependencyKey> verifyAgainstFresh(const QList<DependencyKey> &names);   // returns mismatching names
```

- A run is counted immediately before `compute` is invoked (so a throwing
  calculation counts).
- `evaluateFresh` constructs a temporary `CalculationEngine` over the same
  `ISessionState` and registry **without enrolling it** (private constructor
  flag), replays `request` for every instance whose `Result` is currently
  cached with `requested == true` (explicit results are the one piece of
  non-derivable cache state), resolves `name`, and returns the value. The
  temporary's counters are discarded; the real engine's cache, edges, and
  counters are untouched.
- `sameValue`: availability equal; attributes equal by `QVariant ==`; samples
  equal element-wise by bit pattern (`std::memcmp` of the `double`s, so
  NaN == NaN and -0.0 != 0.0); units equal.
- `verifyAgainstFresh` reads each name normally, compares with
  `evaluateFresh`, and returns the names that differ. Phase 8 builds the
  session-level oracle on top of this.

**Acceptance Criteria:**
- [ ] `cachedState` / `resultStatus` on a never-read name return `NotCached` / `nullopt`, leave `totalRunCount()` and the counting fake's call counts unchanged, and add no cache entry (`cachedNodeCount()` unchanged).
- [ ] `evaluateFresh` does not change `totalRunCount()`, `edgeCount()`, or `cachedNodeCount()` of the real engine, and the registry's enrolled-engine list is the same before and after.
- [ ] `sameValue` treats two NaN samples with identical bits as equal.

**Complexity:** M

---

### Task 2.9: Build wiring and test fakes

**Purpose:** Compile the engine into `flysight_model` and give the tests a synthetic session state and preference provider.

**Files to modify:**
- `src/CMakeLists.txt` - append to the `flysight_model` source list (the block at 234-241 in the baseline numbering) the eleven files of `src/engine/` listed in the architecture table. No new link dependency; no new include directory (includes are written `"engine/calculationengine.h"` from `src/`, `"../dependencykey.h"` from inside `engine/`).
- `tests/CMakeLists.txt` - add the two support files to `flysight_test_support`; register the four test targets of Task 2.10 with `flysight_add_test(<name> SOURCES <name>.cpp)`.

**Files to create:**
- `tests/support/fakesessionstate.h` / `.cpp`

**Technical Approach:**

In namespace `FlySightTest`:

```cpp
class FakeSessionState : public FlySight::ISessionState {
public:
    // mutators change the maps only; the test (or the convenience overloads below) notifies the engine
    void setAttribute(const QString &key, const QVariant &v);
    void removeAttribute(const QString &key);
    void setMeasurement(const QString &sensor, const QString &name, const QVector<double> &v, const QString &unit = {});
    void removeMeasurement(const QString &sensor, const QString &name);
    void setUnit(const QString &sensor, const QString &name, const QString &unit);
    // convenience: mutate + notify, returning the engine's invalidated set
    QSet<FlySight::DependencyKey> setAttribute(FlySight::CalculationEngine &e, const QString &key, const QVariant &v);
    QSet<FlySight::DependencyKey> removeAttribute(FlySight::CalculationEngine &e, const QString &key);
    QSet<FlySight::DependencyKey> setMeasurement(FlySight::CalculationEngine &e, const QString &sensor, const QString &name, const QVector<double> &v, const QString &unit = {});
    int readCount() const;          // total ISessionState calls, for "does not compute / does not touch state" checks
    void resetReadCount();
    // ISessionState ...
};

class FakePreferenceProvider : public FlySight::IPreferenceProvider {
public:
    void set(const QString &key, const QVariant &v);      // map only
    void set(FlySight::CalculationRegistry &r, const QString &key, const QVariant &v);   // map + r.notifyPreferenceChanged(key)
    int readCount() const;
    QVariant preferenceValue(const QString &key) const override;
};
```

Every test constructs its **own** `CalculationRegistry` (never
`CalculationRegistry::instance()`), so tests are order-independent and the
process-wide registry stays empty for Phase 3.

The bridge module: `flysight_cpp_bridge` links `flysight_model`, so it now
links a library with more objects; none are referenced, nothing else changes.
Verify it still builds.

**Acceptance Criteria:**
- [ ] `cmake --build build --config Release` succeeds with `FLYSIGHT_BUILD_TESTS` ON and OFF; `FlySightViewer` and `flysight_cpp_bridge` build; `flysight_model`'s link dependencies are still Qt Core only.
- [ ] `git diff --stat` for this phase touches only `src/CMakeLists.txt`, `src/engine/*`, `tests/CMakeLists.txt`, `tests/support/fakesessionstate.*`, `tests/tst_calc*.cpp` (plus `tests/README.md` if a line is added listing the new targets).
- [ ] No file under `src/` outside `src/engine/` includes an engine header.

**Complexity:** S

---

### Task 2.10: Engine test suite

**Purpose:** Demonstrate acceptance items 9-15 at engine level and pin every behavior specified above.

**Files to create:**
- `tests/tst_calcregistry.cpp` (class `CalcRegistryTest`)
- `tests/tst_calcengine.cpp` (class `CalcEngineTest`)
- `tests/tst_calcengine_safety.cpp` (class `CalcEngineSafetyTest`)
- `tests/tst_calcengine_oracle.cpp` (class `CalcEngineOracleTest`)

**Technical Approach:**

Each file uses `FLYSIGHT_TEST_MAIN(<Class>)` and ends with its `.moc` include
(Phase 1, Task 1.5). Each test function builds a local registry, a
`FakeSessionState`, a `FakePreferenceProvider`, and one or two engines.
Expected values are literals; the only place a computed value is the
expectation is the oracle comparison that spec 7.4 mandates, and that file
also contains literal checkpoints.

Shared synthetic calculations (define as small factory functions at the top of
the files that need them; attribute values are `int` `QVariant`s):

| Id | Inputs | Outputs |
|---|---|---|
| `sum` | attr `A`, attr `B` | `X = A + B` |
| `fallbackX` | attr `C` | `X = C * 10` |
| `constX` | - | `X = -1` |
| `triple` | attr `X`, pref `p` | `Y = X + p`, `Z = X * 2`, `W = X * 3` if `X >= 0` else unavailable |
| `wAlt` | attr `Z` | `W = Z + 1000` |
| `meas` | meas `S/m`, attr `Y` | meas `S/d[i] = m[i] + Y`, unit `"u"` |
| family `neg` | name `"neg:<attr>"` -> attr `<attr>` | that name `= -value` |
| `P`,`Q`,`R`,`S` | `P`: attr `Y2`; `R`: attr `X2`; `Q`,`S`: none | `P: X2 = Y2 + 1`, `Q: X2 = 100`, `R: Y2 = X2 + 1`, `S: Y2 = 200` (registered `P, Q, R, S`) |

Registration order for the shared world: `sum, fallbackX, constX, triple,
wAlt, meas, neg, P, Q, R, S`.

`tst_calcregistry`: every criterion of Task 2.3 (order interleaving, each
validation rejection, family memo identity, unregister, local-registry
isolation, `hasCandidateFor`).

`tst_calcengine`:

| Test | Scenario | Expected literals |
|---|---|---|
| `storedWins` | `A=1,B=2`, stored `X=7` | `X == 7`; `runCount(sum) == 0` |
| `threeOutputsRunOnce` (acc. 9) | `A=1,B=2,p=5`; read `Z,Y,W,Y,Z`; then new engine, read `W,Z,Y` | `Y=8, Z=6, W=9`; `runCount(triple) == 1` in both |
| `declaredInputChangeRunsOnceMore` (9) | then `p=6` via provider+notify; read `Y,Z,W` twice | `Y=9`; `runCount(triple) == 2` |
| `unrelatedChangeRunsNothing` (9) | set attr `U=1`, source `T/q`, pref `other` | returned sets contain only the changed name / are empty; `totalRunCount()` unchanged after re-reading |
| `preferredCandidateReplacesFallback` (10) | only `C=4`: `X == 40`; set `A=1` (set contains `X`), read `X == 40` with `runCount(fallbackX) == 1`; set `B=2`, read | `X == 3`; `runCount(sum) == 1`; `runCount(fallbackX) == 1` |
| `rejectedCandidatesAreDependencies` | after first read above | `dependenciesOf(resolution(X))` == {`storedAttribute(X)`, `result("sum")`, `result("fallbackX")`}; `resultStatus("sum") == MissingInput` |
| `removingStoredFallsBackToCalc` | stored `X=7`, `A=1,B=2`; remove `X` | set contains `X`; `X == 3` |
| `partialResultNextCandidate` | `constX` only for `X` (unregister `sum`,`fallbackX`), `p=0` | `W == 998` (from `wAlt`: `Z = -2`), `Y == -1`; `runCount(triple) == 1` |
| `unavailableIsCached` | nothing stored except none for `sum`; read `X` with only `sum` registered 3 times | unavailable; state `readCount()` does not grow after the first read; `cachedState(X) == Unavailable` |
| `overrideOneOutput` (11) | `A=1,B=2,p=5`, stored `Z=50` | `Z == 50`, `Y == 8`, `W == 9`; `cycleCount() == 0`; `runCount(triple) == 1`; remove `Z` -> `Z == 6`, `runCount(triple)` still 1; `wAlt` never needed |
| `overrideFeedsDownstream` (11) | same with `triple` giving `W` unavailable (`constX` world) and stored `Z=50` | `W == 1050` |
| `twoSessionsIndependent` (13) | one registry, two states (`A=1,B=2` / `A=10,B=20`), two engines | `X == 3` and `X == 30`; editing state 1 leaves engine 2's `cachedState(X) == Available` and run counts |
| `unregisterInvalidatesEverySession` (13) | both engines have read `X`; `unregister("sum")` | both listeners receive a set containing `X`; both `resultStatus("sum") == nullopt`; `totalRunCount()` unchanged by the unregister; next reads give unavailable (only `sum` registered) |
| `registerInvalidatesNegativeCache` | read `X` with nothing registered; then register `constX` | listener set contains `X`; `X == -1` |
| `explicitPolicy` (14) | `triple` registered `Explicit` | Task 2.7 criteria with `Y=8,Z=6,W=9` |
| `declaredPreferenceInvalidates` (15) | `p` 5 -> 6 | listener set == {`Y`,`Z`,`W`} (names that were read); values `9, 6, 9` |
| `snapshottedPreferenceDoesNot` (15) | calc `massUser`: input attr `_JUMPER_MASS` -> `M2 = mass * 2`; stored `_JUMPER_MASS = 80` (copied "at import"); provider `aero/mass` 80 -> 90 + notify | listener not called; `M2 == 160`; `runCount == 1` |
| `missingPreferenceIsUnavailable` | `p` unset | `Y` unavailable, `resultStatus("triple") == MissingInput` |
| `familyInstancesAreDistinct` | `A=1, B=2`; read `neg:A`, `neg:B` | `-1`, `-2`; `runCount("neg") == 2`; `runCountForInstance("neg#neg:A") == 1`; `attributeChanged("A")` set contains `neg:A` and not `neg:B`; re-reading `neg:B` runs nothing |
| `inspectDoesNotCompute` | Task 2.8 criterion 1 | |
| `invalidateDoesNotCompute` | Task 2.5 criterion 1 | |
| `edgesAreCleanedUp` | Task 2.5 criterion 3 | |
| `measurementPassthrough` | source `S/m = {1,2,3}` unit `"raw"`, no conversion registered | `measurement == {1,2,3}`, unit `"raw"`; `runCount == 0`; deps of `resolution(S/m)` == {`sourceMeasurement`, `sourceUnit`} |
| `derivedMeasurementAndUnit` | plus `A=1,B=2,p=5` | `S/d == {9,10,11}`, unit `"u"`; changing `S/m` returns a set containing `S/d` |
| `sourceConversionHook` | conversion family: inputs `sourceMeasurement`, `sourceUnit`; output = samples x 2, unit `"conv"` | `S/m == {2,4,6}`, unit `"conv"`; `sourceUnitChanged` invalidates `S/m`; a plain calculation declaring a `sourceMeasurement` input is rejected by `registerCalculation` |
| `undeclaredReadDetected` | Task 2.6 criterion 5 | |
| `invalidOutputRejected` | calc sets an undeclared output | all outputs unavailable; `resultStatus == InvalidOutput` |

`tst_calcengine_safety` (acc. 12): the criteria of Task 2.6 - nested chain
`sum -> triple -> meas` evaluated from a single read of `S/d` with
`scopeDepth() == 0` afterwards and each calculation run once; `P/Q/R/S` both
orders (`X2 == 100`, `Y2 == 200`), then stored `Y2 = 5` -> `X2 == 6`; pure
cycle without fallbacks; `std::runtime_error` and `throw 42`; exception in an
inner calculation of a nested chain leaves the outer unavailable and
previously cached unrelated results intact; a throwing calculation that had
already `set` one output publishes none; after each scenario
`verifyAgainstFresh` over all names returns an empty list.

`tst_calcengine_oracle` (acc. 10, spec 7.4):
- RNG: `std::mt19937 rng(seed)`; pick with `rng() % n` only (do **not** use
  `std::uniform_int_distribution`, whose output differs between standard
  libraries). Data-driven over seeds `1..25`, 400 operations each.
- World: the shared registrations; names
  `{X, Y, Z, W, X2, Y2, neg:A, neg:X, S/d, S/m}`.
- Operation mix: 50% read a random name and compare to `evaluateFresh`
  (`QVERIFY2` with seed + step in the message); 30% set/remove one of
  attributes `A,B,C,X,Z,Y2` (values `rng() % 7`); 8% set/remove `S/m`
  (length 3); 7% set/clear pref `p`; 5% unregister or re-register one of
  `fallbackX`, `wAlt`, `neg` (covers registry changes and re-ordering).
- After every mutation assert `totalRunCount()` did not change during the
  notification. At the end of each sequence `verifyAgainstFresh(all names)`
  must be empty and `scopeDepth() == 0`.
- Second engine on a second state runs an independent sequence against the
  same registry in the same loop (cross-session registry changes).
- Literal checkpoint at the start of every sequence: with `A=1,B=2,p=5`,
  `S/m={1,2,3}`: `X=3, Y=8, Z=6, W=9, X2=100, Y2=200, neg:A=-1, S/d={9,10,11}`.

**Acceptance Criteria:**
- [ ] All four targets are registered through `flysight_add_test`, appear in `ctest -N`, and pass on Windows Release along with `tst_harness` and `tst_smoke`.
- [ ] Each of acceptance items 9, 10, 11, 12, 13, 14, 15 is named in a comment on at least one test function.
- [ ] No test uses `CalculationRegistry::instance()`, `PreferencesManager`, `SessionData`, the clock, or an unseeded RNG.
- [ ] Deliberately breaking dependency recording for rejected candidates (temporarily skipping the `Result` note in `resolve` step 6) makes `preferredCandidateReplacesFallback` and the oracle test fail - checked once by hand, not committed.

**Complexity:** L

---

## Testing Requirements

### Unit Tests
- New: `tst_calcregistry`, `tst_calcengine`, `tst_calcengine_safety`, `tst_calcengine_oracle` (Task 2.10).
- Existing `tst_harness` and `tst_smoke` must pass unchanged - this phase alters no application behavior, so no `// BASELINE:` expectation changes.

### Integration Tests
- Build with `FLYSIGHT_BUILD_TESTS=ON` through the superbuild and run `ctest --test-dir build/FlySightViewer-build -C Release --output-on-failure`: 6/6 pass.
- Build with the option OFF: the application, `flysight_model`, `flysight_core`, and `flysight_cpp_bridge` build; no test target exists.

### Manual Verification
1. Launch `build/install/FlySightViewer.exe`; open a session; confirm plots, markers, logbook columns, altitude markers, and Python plugins behave exactly as after Phase 1 (the engine is linked but unused).
2. `git grep -n "engine/" -- src ':!src/engine' ':!src/CMakeLists.txt'` returns nothing.

## Notes for Implementer

### Gotchas
- **Record before you branch.** The dependency on `StoredAttribute(name)` / `SourceMeasurement(...)` must be noted *before* testing presence, and `Result(candidate)` must be noted for every candidate tried, or a cached fallback will survive the arrival of a preferred input - the exact bug spec 7.4 describes in the old engine (`calculatedvalue.cpp` 69-87 records deps for the winner only).
- **Edges are recorded even when the target is not (yet) cached** - cycle re-entry and failed evaluations included - and invalidation propagates through uncached nodes.
- **Publish last, and as one step.** Build the entry and the new edge sets, then insert; replacing `m_dependsOn[n]` must first detach the old reverse edges.
- **Short-circuit the availability pass** at the first missing input, in declared order; do not resolve later inputs. The tests pin the resulting dependency sets.
- **Never call `compute` during invalidation, inspection, registration, or `evaluateFresh`'s effect on the real engine.**
- `DependencyKey`'s default constructor leaves `type` uninitialized (`dependencykey.h` 10-20). Where a "no name" argument is needed (`request`, `instance`), test emptiness via both strings being empty, or pass `std::optional<DependencyKey>` internally; do not read `type` of a default-constructed key. Do not edit `dependencykey.h`.
- `QSet<DependencyKey>` relies on the existing `uint qHash(const DependencyKey&, uint)`; new types use `size_t`. Do not add a second `qHash` overload for `DependencyKey`.
- A registry must outlive its engines; in tests declare the registry before the engines.
- Interpolation-style attribute names contain `':'` and `'/'`; instance ids only reserve `'#'`.
- `flysight_model` is also linked into the pybind11 module; keep the engine free of static objects with non-trivial constructors other than the Meyers singleton inside `CalculationRegistry::instance()`.
- Qt Test on MSVC: keep each test source under the default object-size limits; four files rather than one is deliberate.

### Decisions Made
- **Library/location:** `src/engine/` in `flysight_model`, Qt Core only, namespace `FlySight`.
- **`DependencyKey` kept as the public-name type, untouched;** new `CalcInput` (declared inputs incl. preference and source kinds) and `GraphNode` (graph identities) added beside it. Rationale: zero churn for `SessionModel::dependencyChanged` subscribers and the Python binding; the bridge compiles unchanged. Phase 3 deletes `toDependencyKey` users with the old engine if they become dead; Phase 7 decides the Python face of `CalcInput`.
- **Graph node per calculation instance, not per output** - a bundle is atomic; per-output identity is (`Result` node, name) and lives in the `Resolution` nodes.
- **Session-state interface `ISessionState`** exposes stored attributes and *source* measurements + unit text only. In Phases 2-3, with no conversion family registered, a stored measurement resolves as a passthrough of its source (no copy); Phase 4 only has to register conversion families - the source/effective split is already in the graph.
- **Source conversion is an ordered candidate list**, so Phase 4 can honor "all declared inputs are required" with a `SCHEMA_VER`-declaring candidate followed by a no-`SCHEMA_VER` candidate. If conversion families exist but none produces a value, the name is unavailable; it never falls through to derived candidates.
- **Preferences:** pull through `IPreferenceProvider` set on the registry; push change notification through `CalculationRegistry::notifyPreferenceChanged`. `flysight_model` never sees `PreferencesManager`.
- **Invalidated names:** returned to the caller for direct state notifications and `request`; delivered through a per-engine `InvalidationListener` for broadcast events (registry, preference). Sets contain resolved (cached) names plus the changed name itself.
- **Availability = valid `QVariant` / non-empty vector**, identical to the old engine; invalid/empty outputs mean "unavailable" (replaces WS-P/SP "clear" writes).
- **A calculation may not declare one of its own outputs as an input.**
- **Compute functions never nest:** inputs are fully resolved in the availability pass (where scope nesting happens); `EvaluationContext` reads are cache lookups.
- **Cycle semantics:** every calculation on the ring is unavailable with status `Cycle`, names fall back to later candidates, the cycle is warned and counted, nothing throws. Chosen over "fail only the closing edge" because only this is read-order independent (spec 7.4).
- **Exceptions:** caught around `compute`; status `Failed`; negatively cached with input dependencies; no partial publication. Undeclared reads and malformed bundles are handled the same way, without exceptions.
- **Explicit results** revert to `NotRequested` when invalidated; the oracle replays currently valid requests because they are the only cache state not derivable from persistent state.
- **Counters are per engine** and always compiled in.
- **Tests use private registries**; the process-wide registry stays empty until Phase 3.
- **No batch API for registry changes** - `AltitudeMarkerManager::refresh()` will cause several listener calls in Phase 3; Phase 3 may coalesce at the `SessionModel` level if needed.

### Open Questions
- The cycle rule is argued and tested to be read-order independent for rings reached through ordered candidates (the `P/Q/R/S` shape, in the randomized oracle too). A formal proof for arbitrary overlapping cycles is not attempted; since cycles are registration errors and the built-in graph is acyclic, this is accepted. If the oracle test ever reports a mismatch that involves `Cycle` statuses, escalate rather than weakening the test.

## Definition of Done

This phase is complete when:
1. All tasks have passing acceptance criteria.
2. `tst_calcregistry`, `tst_calcengine`, `tst_calcengine_safety`, `tst_calcengine_oracle`, `tst_harness`, and `tst_smoke` pass via CTest on Windows Release; the application and `flysight_cpp_bridge` build with `FLYSIGHT_BUILD_TESTS` ON and OFF.
3. The application behaves exactly as at the end of Phase 1; no file outside `src/engine/`, `src/CMakeLists.txt`, and `tests/` changed.
4. Spec 7.1-7.9 each map to at least one acceptance criterion above (7.1: Tasks 2.2/2.3; 7.2: 2.4/2.8; 7.3: 2.4; 7.4: 2.6/2.10 oracle; 7.5: 2.4-2.6; 7.6: 2.3/2.10 family tests; 7.7: 2.7; 7.8: 2.5/2.10; 7.9: 2.8).
5. No TODOs or placeholder code remain; nothing has been pushed.
