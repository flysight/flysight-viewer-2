# Writing a registered calculation, and running one in the background

For C++ contributors. Users and firmware developers want
[DATA_SCHEMA.md](DATA_SCHEMA.md); Python plugin authors want
[the plugin README](../python_plugins/README.md), where the same rules apply
with a simpler surface.

Sections 1-11 are about writing a calculation. Sections 12-17 are about
explicit calculations that run in the background: the asynchronous request
(12), blocker inspection (13), the threading rule (14), the job queue (15),
plot-driven requests (16), and sensor fusion as a registered calculation (17).
What the user sees of it is in [COMPUTED_PLOTS.md](COMPUTED_PLOTS.md); the
fusion model itself is in [SENSOR_FUSION.md](SENSOR_FUSION.md).

## 1. The model

A registered calculation is an **id**, its **declared inputs**, its **declared
outputs**, and a **pure `compute` function** (`CalculationDescriptor`,
`src/engine/calculationdescriptor.h`). Registrations are global
(`CalculationRegistry`) and hold no per-session state. Each session has one
`CalculationEngine` that owns every cached result and the one dependency graph:
a calculation never writes anywhere, it returns a `CalculationResult`. Ids are
dotted (`builtin.<area>.<name>`, `plugin.*`) and never contain `#`.

## 2. A minimal example

The time fit, from `src/calculations/timecalculations.cpp`. Registered from
`registerBuiltInCalculations` (`src/calculations/builtincalculations.cpp`)
through `Calculations::registerTimeCalculations`:

```cpp
CalculationDescriptor d;
d.id = QStringLiteral("builtin.time.fit");
d.inputs = {
    CalcInput::measurement("TIME", "time"),
    CalcInput::measurement("TIME", "tow"),
    CalcInput::measurement("TIME", "week")
};
d.outputs = {
    DependencyKey::attribute(SessionKeys::TimeFitA),
    DependencyKey::attribute(SessionKeys::TimeFitB)
};
d.compute = [](const EvaluationContext &ctx) -> CalculationResult {
    const auto fit = fitSystemTimeToUtc(ctx.measurement("TIME", "time"),
                                        ctx.measurement("TIME", "tow"),
                                        ctx.measurement("TIME", "week"));
    if (!fit)
        return CalculationResult::unavailable();
    return CalculationResult()
        .setAttribute(SessionKeys::TimeFitA, QString::number(fit->a, 'g', 17))
        .setAttribute(SessionKeys::TimeFitB, QString::number(fit->b, 'g', 17));
};
addCalculation(registry, d);
```

`addCalculation` (`src/calculations/registration.h`) asserts that the registry
accepted the descriptor.

## 3. Declared inputs

- `CalcInput::attribute(key)`, `CalcInput::measurement(sensor, name)`,
  `CalcInput::preference(key)`.
- **All declared inputs are required.** The calculation runs only when every
  one of them is available. Availability is checked in declared order and
  stops at the first unavailable input.
- Never declare an input "to be safe": it becomes a requirement and a
  dependency.
- Reads go only through the `EvaluationContext`. Measurements read there are
  *effective* values (see DATA_SCHEMA.md, section 5).
- An undeclared read makes every output unavailable
  (`ResultStatus::UndeclaredRead`) and is caught by
  `tst_builtins_engine::noUndeclaredReads`.
- No clock, no random numbers, no `PreferencesManager`, no `SessionData`
  inside `compute`. A calculation that consults anything else is not a function
  of state, and no invalidation scheme can make it correct.
- Ordinary calculations cannot declare source inputs
  (`CalcInput::sourceMeasurement` / `sourceUnit`): the registry refuses them
  everywhere except in `registerSourceConversion`. Only the conversion layer
  reads the source layer; everything else, Python plugins included, reads
  effective values.

## 4. Multi-output calculations and partial results

One computation is one calculation, however many outputs it has; do not group
unrelated calculations because their outputs share a prefix. Set the outputs
you found and leave the rest unset: an unset output is unavailable. Never pass
an invalid `QVariant` to "clear" something. The result is published atomically,
and the calculation runs once however many of its outputs are read. When the
user stores an attribute that is also an output (a marker dragged by hand), the
stored value overrides that one output while the others stay available.
`builtin.local.coordinates` is the example of one calculation with attribute
and measurement outputs together; see
[LOCAL_COORDINATES.md](LOCAL_COORDINATES.md), section 4.

## 5. Candidates and order

Several calculations may declare the same output. They are tried in
registration order; the first whose inputs are all available *and* which
produces the output wins. "Works with or without X" is therefore two
registrations, the one that needs X first.

Registration order is the order in `registerBuiltInCalculations`: the
conversion families, attribute, GNSS, IMU, MAG, time, local coordinates,
simplification, WS-P, SP, interpolation. Python plugins are registered before
the built-ins. Stored data always wins over any calculation.
The application then registers sensor fusion from its own library
(`Fusion::registerFusionCalculations`, `src/fusion/fusionregistration.cpp`);
`flysight_core` does not know it.

The engine records everything a resolution looked at, including the candidates
it rejected, so a cached fallback is replaced when a preferred candidate
becomes viable. A dependency cycle makes every calculation on the ring
unavailable and logs a warning; never rely on that. The answers do not depend
on read order, also when rings overlap: a result is cached only if its
evaluation never ran into something that was being evaluated *above* it (so it
is what an evaluation started at that node produces; the root of a read always
qualifies, and so does a node with a ring closed entirely beneath it), anything
else is used once and its dependencies pass to the nearest cached ancestor, and
a cached answer that involved a cycle is re-evaluated rather than served when
something it looked at is being evaluated right now. Work is repeated only
inside cyclic regions; acyclic graphs cost exactly what they did.
`resultStatus` for a calculation whose last evaluation was provisional is only
a diagnostic from the most recent such evaluation: in tangled rings it can
depend on read order (values never do), and it can revert to "no status" when
a differing verdict replaced an earlier one and the answer holding the new one
is invalidated, although an older answer is still cached. The built-ins
are acyclic, and `tst_session_oracle` asserts that no cycle ever occurs with
them.

What each name resolved to also matters beyond the session. A stored result
(15.8) records, for every name it looked up, what provided it (a calculation
instance with its result version, the session's own data, or nothing), and a
restore repeats those lookups against the current registry (section 12,
"Export and restore"). A registration that changes which candidate wins for
such a name therefore makes the stored result stale; a candidate registered
after the winner, or one for names the result never looked up, does not.

## 6. Parameterized families

`CalculationFamily::instantiate(name)` turns a public name into a calculation
instance on demand; instance ids are `family#key`. There are three:

- `builtin.interpolation`: the attribute `{timeAttr}:{sensor}/{timeVector}/{measurement}`
  (build the key with `SessionData::interpolationKey`);
- `builtin.conversion.schema` and `builtin.conversion.default`: the conversion
  layer, one instance per recorded measurement
  (`src/conversion/sourceconversion.cpp`).

## 7. Preferences

A preference read at compute time is a declared input
(`CalcInput::preference`); today that is only `import/descentPauseSeconds`.
Changing it invalidates dependents in every loaded session. Preferences that
are snapshotted into session attributes when a session is created (mass, area,
fixed ground elevation) are read as attributes and do not affect existing
sessions. Changing *which calculations exist* (the altitude markers,
`AltitudeMarkerManager`) is done by registering and unregistering, which, in
every loaded session, invalidates what depended on the names concerned. A
requested result that never looked those names up stays installed, and so does
its stored copy (15.8); one that did is dropped and its record deleted.
`AltitudeMarkerManager`'s destructor unregisters with
`CalculationRegistry::Removal::Teardown`, which reports nothing (section 12).

## 8. Explicit policy

`EvaluationPolicy::Explicit` calculations run only through
`CalculationEngine::request(id)`; before that their outputs read as unavailable
(`ResultStatus::NotRequested`) without starting work, and they revert to that
state when an input changes. A result an explicit calculation installs with
status `Ok` is stored in the logbook's `cache/` folder and restored when the
session is loaded again (section 15.8); restoring is not requesting. Explicit family
instances are not stored. A descriptor may declare
`CalculationDescriptor::resultVersion`, opaque text that identifies the
arithmetic of the calculation's results. The engine never interprets it. It is
part of the calculation environment fingerprint of the logbook column cache
(section 9), and a stored result records it for its own calculation and for
every calculation its lookups went through (section 12, "Export and restore");
a stored result is used only while those are unchanged (section 9).
`builtin.fusion.fit` declares its kernel's `Fusion::Algorithm` (section 17),
and every Python plugin registration declares the plug-in code identity
(`src/plugincodeidentity.h`; the plugin README, section 7). The same
calculation can be run in the background (section 12), and section 13 reports
which explicit calculations stand behind a name.

An explicit calculation's outputs must have no other candidate. Only then do
they read as unavailable whenever the calculation is not requested, and the
logbook relies on that: a session that is not loaded and has no stored result
gets "unavailable" cached for every column over such an output without being
loaded (`SessionModel::settleExplicitColumns`, section 17). The registry does
not enforce it; `tst_fusion_session::explicitOutputsHaveOneCandidate` checks it
for the registered built-ins.

`request` returns the names whose cached "not requested"
answer was dropped: a future model-level caller must publish that set through
`SessionModel`, which is the single emitter of `dependencyChanged`. An explicit
calculation whose input transitively depends on its own output is a cycle like
any other: `request` drops the cached "not requested" answers before it
evaluates, reports the cycle, returns `ResultStatus::Cycle`, and publishes
nothing. `builtin.fusion.fit` (title "Sensor fusion") is the first explicit
calculation (section 17).

## 9. When to bump `CalculationCompatibilityVersion`

The constant is in `src/calculations/builtincalculations.h`; its comment is the
authority. Bump it whenever a code change can alter the value that any
existing session yields for any logbook column:

- a built-in calculation's arithmetic, inputs, or candidate order;
- the schema table or the unit-normalization table (`src/conversion`,
  `src/units/unitconversion.h`);
- the interpolation family;
- `SessionModel::computeColumnValues` (what a column stores, or its unit).

Bump it, or the result version of the calculation concerned
(`CalculationDescriptor::resultVersion`, section 8), whenever a change can
alter what a requested calculation, or anything it reads, produces. A stored
result is used only while this marker and the result version it was stored
with equal the current ones, and every name it looked up still resolves to the
same provider with the same result version (15.8). Bumping a result version
drops the stored results of that calculation and of every requested
calculation whose lookups went through it; bumping the marker drops every
stored result and every cached column value.

Do not bump it for added, removed, or renamed registrations, or for a changed
result version: the environment fingerprint
(`calculationEnvironmentFingerprint`) covers those for the logbook column
cache. It covers every registration's result version too, so a plugin edit
discards cached column values at the next start. A stored result does not
depend on the fingerprint: a registration makes it stale only by changing what
a name it looked up resolves to. Never reuse a value, and never use 0.

## 10. Testing a calculation

- Unit level: a private `CalculationRegistry` and a `FakeSessionState`, as in
  `tests/tst_builtins_engine.cpp`.
- Add golden rows to `goldenValues()` (`tests/support/builtinfixture.cpp`) and
  extend the `inventory` literal in `tst_builtins_engine`.
- The session-level oracle (`tst_session_oracle`) picks up new golden names
  automatically.

See [the test README](../tests/README.md).

## 11. Invalidation and the model

Mutate a session only through the `SessionData` setters or
`SessionMerge::apply`; they tell the engine what changed. Every model-level
mutation must also call `invalidateColumns` or `invalidateAllColumns` before
returning to the event loop (the rule is spelled out in `src/sessionmodel.h`),
which keeps the cached logbook columns in step with the saved file. A mutation
that changes an input of a stored result deletes its record through the
engine's explicit-result listener (section 15.8); the call site needs to do
nothing more.

## 12. Asynchronous request

An explicit calculation can take minutes. `request` would block the main thread
for that long, so the same evaluation is also available in three steps. The
engine creates no thread: the caller (the job queue) decides where step 2 runs.

| Step | Call | Thread |
|---|---|---|
| 1. prepare | `CalculationEngine::prepare(id, instanceOutput)` | main |
| 2. compute | `PreparedCalculation::compute(progress)` | any, once |
| 3. publish | `PreparedCalculation::publish(computed)` | main, once, after compute has returned |

```cpp
CalculationEngine::PrepareOutcome prepared = engine.prepare("builtin.fusion.fit");
if (prepared.kind != CalculationEngine::PrepareOutcome::Kind::Ready)
    return;                                     // see the table below
std::unique_ptr<PreparedCalculation> ticket = std::move(prepared.ticket);
ComputedCalculation computed;
std::thread worker([&] { computed = ticket->compute(&progress); });
worker.join();                                  // or a queued "finished" signal
const PublishOutcome outcome = ticket->publish(std::move(computed));
if (outcome.kind == PublishOutcome::Kind::Published)
    publishToModel(outcome.invalidated);        // as for RequestOutcome::invalidated
```

**Prepare** mirrors the first half of `request`: it resolves every declared
input, computing on-demand inputs as needed, and captures the values. It never
runs an explicit calculation - not this one, and not one behind an input.
`CalculationEngine::PrepareOutcome::Kind`:

| Kind | Meaning | Cache |
|---|---|---|
| `NotFound` | unknown id, or the name is not of that family | unchanged |
| `NotExplicit` | only explicit calculations can be prepared, so on-demand and plugin compute functions never leave the main thread (`request` still accepts any policy) | unchanged |
| `AlreadyValid` | a valid result is cached (`status`); it is never recomputed | unchanged |
| `NothingToRun` | an input is unavailable (`MissingInput`) or on a ring (`Cycle`) | exactly as `request` leaves it; the caller publishes `invalidated` |
| `Blocked` | `NothingToRun` with `MissingInput`, and the input is missing only because the explicit calculations in `blockers` have not been requested (section 13) | as `NothingToRun` |
| `Ready` | inputs captured; `ticket` is set | nothing is cached for the calculation; `invalidated` is informational, `publish` reports those names again |

While a ticket is outstanding the calculation is still "not requested" for
every reader, for the fresh-evaluation oracle, and for inspection. Destroying
an unpublished ticket (main thread) leaves it that way, as if it had never been
asked, and leaves nothing in the dependency graph.

**Compute** runs the compute function against the captured inputs and returns a
`ComputedCalculation`: an opaque payload plus how the run ended. It never
throws and never logs.

| Thrown by the compute function | `ComputedCalculation::Kind` | At publish |
|---|---|---|
| nothing | `Completed` | installed; the status (`Ok`, `UndeclaredRead`, `InvalidOutput`) is decided there |
| `CalculationCancelled` | `Cancelled` | `Discarded`: nothing published, nothing cached |
| `std::bad_alloc` | `ResourceExhausted` | `Discarded`: nothing published, nothing cached |
| any other `std::exception` | `Failed`, `failureText` = `what()` | installed and cached as `ResultStatus::Failed`, as for `request` |
| anything else | `Failed`, fixed text | the same |

A failure that is a function of the inputs is cached; one that is not (no
memory, a cancel) never is, so the calculation can be requested again. The
synchronous `request` has no cancel facility and nowhere to "not cache", so
there both remain ordinary `Failed` results, as before.

**Publish** lets the engine - not the caller - decide whether the result may be
installed, from its own dependency records: the ticket is a node in the
dependency graph (`GraphNode::prepared`) with the edges the result would have
had, so whatever would have invalidated the published result marks the ticket.
`PublishOutcome::Kind` / `Reason`, checked in this order:

| Kind / Reason | When |
|---|---|
| `RefusedGone` / `SessionGone` | the engine (the session) was destroyed; the ticket may outlive it |
| `RefusedGone` / `RegistrationRemoved` | the calculation was unregistered since prepare, even if the same id was registered again |
| `RefusedStale` / `InputsChanged` | anything the prepared inputs depended on was invalidated: an attribute, source, unit, declared preference or registry change that reaches an input transitively, `clear()`, or copy-assignment over the session. An unrelated edit does not refuse |
| `Discarded` / `Cancelled`, `ResourceExhausted` | the run produced nothing to install |
| `RefusedStale` / `AlreadyPublished` | a synchronous `request` published in between (by purity, the same result) |
| `Published` | installed for all outputs at once, with `status` and `detail` |

`publish()` is called between evaluations, never from inside one (a compute
function or an engine callback). A debug build asserts that; a release build
refuses such a call as `RefusedStale` / `InputsChanged`, because nothing may be
installed in the middle of an evaluation.

**Knowing a refusal in advance.** The engine marks a ticket the moment it
becomes stale or gone, not at publish. `PreparedCalculation::willBeRefused()`
(**main thread**, like everything of a ticket except `compute()`) reads that
mark back: true while a `publish()` still to come is certain to be refused as
`SessionGone`, `RegistrationRemoved` or `InputsChanged`, and `refusalReason()`
names which (`None` otherwise). The engine still decides staleness, from its
own records; the query hands its verdict to a caller that may want to stop a
computation nobody can use (section 15.3), and that caller decides nothing.
`compute()` never reads it, and there is still no lock and no atomic in the
library: the mark is a main-thread field of the ticket. Its semantics are
narrow on purpose:

- once true it stays true until `publish()`; undoing the edit does not revive
  the ticket;
- it is false for a healthy ticket, and false again once `publish()` was
  called, whatever that returned. It says nothing about a publication that has
  happened, and it does not follow the published result's later invalidation;
- false is not a promise. `AlreadyPublished` (a synchronous `request` in
  between) and an invalidation that arrived during an evaluation and was
  deferred are decided by `publish()` alone.

A refused or discarded result is dropped whole and is not counted as a run. A
published one leaves the engine exactly as `request` would have: same status,
edges, run and undeclared-read counters, and the same warnings (logged at
publish, on the main thread). `PublishOutcome::invalidated` holds the names
that had been read while the calculation was "not requested" - before prepare
or since. **The caller must publish that set** through `SessionModel`, exactly
like `RequestOutcome::invalidated`; that is what makes plots appear.

Vocabulary that goes with it:

- `CalculationDescriptor::title` - interface text ("Sensor fusion"), opaque to
  the engine and not part of the environment fingerprint.
  `CalculationRegistry::title(id)`, `PreparedCalculation::title()` and
  `CalculationBlocker::title` fall back to the id. Main thread.
- `CalculationResult::setReason(text)` / `reason()` - why outputs are
  unavailable; part of the result, a function of the inputs, cached with it.
  `CalculationEngine::resultDetail(id)` (main thread) returns it, or the
  exception text of a `Failed` result; `PublishOutcome::detail` and
  `UnproducedNote::detail` carry the same string.
- `CalculationProgress` - `report(text)`, `isCancelled()`,
  `throwIfCancelled()`, `none()`. Implemented by the caller of `compute`; both
  virtual functions are called **on the compute thread** and must not throw. A
  compute function reaches it through `EvaluationContext::progress()`, which is
  never null (`none()` on the synchronous path: never cancelled). The facility
  is not an input: it cannot influence the result except by abandoning it.
- `CalculationCancelled` - what `throwIfCancelled()` throws. Deliberately not a
  `std::exception`, so code that catches `std::exception` cannot swallow it.
- `PreparedCalculation::registrationId()`, `instanceId()`, `title()` - what a
  job record shows. Main thread.
- `PreparedCalculation::willBeRefused()`, `refusalReason()` - the engine's
  mark on an outstanding ticket, above. Main thread; never read by `compute()`.
- `CalculationEngine::preparedCount()` - outstanding tickets (instrumentation;
  main thread). `edgeCount()` includes their edges, `cachedNodeCount()` does not.

A compute function that returns normally although cancellation was requested
yields `Completed`, and publishing it would be correct. Whether to publish it
is the job queue's decision (it does not: cancel wins).

**Export and restore.** The engine's half of stored results (the store is
section 15.8). Main thread only.

- `exportResult(id)` returns a `StoredCalculationResult`
  (`src/engine/storedcalculationresult.h`), or nothing unless a plain explicit
  calculation (not a family) has an installed result with status `Ok` whose
  evaluation met no dependency ring (a ring is a registration error; what
  provided a name then cannot be stated). Its members:
  - `calculationId`, `resultVersion` (the descriptor's at publish), `detail`
    (always the bundle's reason), and `bundle` (the outputs, in order);
  - `leaves`: every stored attribute, source measurement, source unit and
    declared preference the result reached through the recorded edges,
    transitively, through on-demand and explicit results alike, the ones that
    were looked at and found absent included. The list is sorted and unique;
  - `resolutions`: for every name the result looked up, reached by the same
    walk as the leaves (rejected candidates and explicit results included), a
    `StoredResolution {name, provider, instanceId, resultVersion}`; provider
    `Calculation` (the instance id, `<familyId>#<key>` for a family instance,
    and that registration's result version), `SessionData` (a stored
    attribute, or source data read through the passthrough) or `Nothing`.
    Sorted by `storedResolutionLess()`, one entry per name. A source
    conversion is a `Calculation`, so registering the first source conversion
    changes every measurement that has source data. A preference is a leaf,
    not a resolution;
  - `inputFingerprint`: SHA-256 over a pinned canonical encoding of the leaves
    and their current values. Attribute and preference values use the session
    file's text; samples are encoded as their bits, with every NaN as one
    pattern and `-0` kept. A change of the encoding means a new magic, so every
    stored fingerprint then mismatches.

  Export is an inspection that reads the state and the preferences for the
  fingerprint. It never computes. The code stamp
  (`CalculationCompatibilityVersion`) is not part of the snapshot (the engine
  does not depend on `src/calculations/`); the record adds it.
- `restoreResult(snapshot)` returns a `RestoreOutcome {kind, staleCheck,
  status, invalidated}`:
  - kinds: `NotFound`, `NotExplicit`, `AlreadyInstalled` (a result is cached,
    whatever its status: nothing changes, a cached result is never replaced),
    `Stale`, `Restored`;
  - stale checks, in order: `ResultVersion`, `Bundle` (an undeclared output, or
    a detail that is not the bundle's reason), `InputsUnavailable`,
    `Resolutions` (the gathering met a ring, or a looked-up name resolved
    differently: another provider, another instance or result version, or a
    name looked up in only one of the two), `Leaves`, `Fingerprint`. A change
    of the session or the registry that alters a lookup reports `Resolutions`,
    also when the leaves differ as well; with the same resolutions the
    gathering takes the same paths, so `Leaves` is in practice a check of the
    snapshot itself;
  - it gathers the inputs exactly as `prepare()` does (on-demand intermediates
    are evaluated as for a fresh request) and never runs the compute function;
  - a stale restore caches nothing for the calculation: it still reads "not
    requested";
  - a successful one installs through the same step as a publish, so status,
    detail, bundle, edges, blockers, `dependenciesOf()` and later invalidation
    equal those of a fresh publish;
  - it counts no run and creates no ticket;
  - an outstanding ticket for the calculation then publishes as `RefusedStale`
    / `AlreadyPublished`;
  - `invalidated` must be published like `RequestOutcome::invalidated`
    whenever anything may have read the engine before the restore.
- `setExplicitResultListener(listener)` receives `ExplicitResultEvent {kind,
  instanceId, status}`:
  - `Installed` is reported for every requested install of an explicit
    calculation (a synchronous `request()`, `prepare()`'s `NothingToRun` /
    `Blocked`, a `Published` publish), whatever its status;
  - `DroppedByInputChange` is reported for leaf notifications, preference
    broadcasts, the cascade when an upstream calculation that a requested
    result read as "not requested" is requested, published or restored, and a
    registry change made while the application runs that reaches the result
    (a registration, or a removal with `CalculationRegistry::Removal::Change`,
    the default of `unregister()`);
  - neither is reported for `restoreResult()`'s own install, `clear()`, a
    removal with `Removal::Teardown` (an owner being destroyed at shutdown),
    or the destruction of the registry or the engine;
  - events are delivered in order at the end of the engine call, never inside
    an evaluation. The listener travels with the engine (a moved `SessionData`
    keeps it);
  - an `Installed` can be followed in the same call by a
    `DroppedByInputChange` for the same result; `exportResult()` then already
    returns nothing at the `Installed` event.

## 13. Blocker inspection

"Why is this name unavailable, and can requesting something change that?"
`CalculationEngine::blockers(name)` answers it without running any explicit
calculation (main thread; not from inside a compute function). `BlockerReport::State`:

| State | Meaning |
|---|---|
| `Available` | an ordinary read returns a value |
| `Blocked` | `blockers` lists the explicit calculations to request **now** |
| `NotProduced` | an explicit calculation ran and did not produce it; `notProduced` says which, with its status and detail |
| `NotApplicable` | unavailable for ordinary reasons: no data, a missing input, an unknown name |

Rules:

- The report sees through on-demand intermediates: a name derived from an
  explicit output, however many on-demand calculations away, reports that
  explicit calculation.
- Availability of inputs is decided before policy. A calculation whose declared
  inputs are unavailable is not a blocker, and **all** of its inputs are
  examined: with one input blocked and another genuinely missing, no request
  could help, so nothing is reported.
- With explicit B consuming unrequested explicit A the blocker is A alone; once
  A has published it is B. A consumer keeps requesting blockers until none
  remain.
- "Ran and did not produce" covers a clean run that reported the output
  unavailable (a rejection: status `Ok`, detail = the result's reason) and
  `Failed`, `UndeclaredRead`, `InvalidOutput`; it extends to everything derived
  from such an output. The same inputs would give the same answer, so nothing
  offers to run it again; an input change makes the name `Blocked` again.
- `Blocked` outranks `NotProduced`; `notProduced` may be non-empty in both.
- A restored result (section 12, export and restore) is reported exactly as a
  published one: a stored rejection is `NotProduced` with its detail.
- A name another candidate already provides is `Available`, even if an explicit
  candidate precedes the provider. A stored attribute or a name with source
  data never consults derived candidates. Rings end the walk.
- Blockers and notes are unique by instance id, in discovery order (candidate
  order, then declared input order). A `CalculationBlocker` carries
  `registrationId` and `instanceOutput` - exactly the arguments of `prepare` /
  `request`, also for a family instance - plus `instanceId` and `title`.
- Inspection performs ordinary reads and const lookups only. It may compute
  cheap on-demand values; it has no path to an explicit compute function; and
  by the idempotency invariant it never changes what a later read returns. An
  outstanding ticket does not change a report: "pending" is the job model's
  notion.

`CalculationEngine::readiness(id, instanceOutput)` (main thread) classifies one
calculation instance with the same walk - `CalculationReadiness::State`:
`Unknown`, `MissingInput`, `Blocked` (with `blockers`), `Ready` (explicit, every
input available, no valid result: `prepare` would return `Ready`), `Done`
(nothing to request: a valid result, with its `status`, or an on-demand
calculation). "Can a job be created for this session" is `Ready`.

## 14. The threading rule

One thread owns all state. The engine, the registry, and every session are used
from the main thread only. The library creates no thread and contains no lock
and no atomic. The single exception is narrow and must stay narrow:

- A `PreparedCalculation` is created, inspected (`willBeRefused()` included),
  published, and destroyed on the main thread. **Only `compute()` may run elsewhere**, once, and the caller
  guarantees it has returned before `publish()` or the destructor runs (a
  thread join, or a queued "finished" signal).
- `compute()` sees the captured inputs, the descriptor (kept alive by the
  ticket, so it survives an unregister), and the `CalculationProgress` it was
  given - nothing else: no engine, no registry, no session, no preference
  store, no Qt GUI object. The ticket's fields are partitioned between the two
  threads (see `src/engine/preparedcalculation.h`); that is why no lock is needed.
- Captured values are Qt implicitly shared copies taken during prepare. Qt's
  reference counts are atomic and every main-thread writer detaches before it
  writes, so the worker's buffers never change and later session edits cannot
  reach them. No deep copy is made.
- The compute function of an explicit calculation must be re-entrant: no
  mutable captured state, no statics. It may be running on a worker while
  another session evaluates the same descriptor.
- Nothing is logged from `compute()`. Warnings caused by an asynchronous run
  are emitted by `publish()`.
- A `ComputedCalculation` is handed over, not shared: the worker returns it,
  the main thread publishes it.
- Do not add locks to make shared access safe; remove the sharing.

## 15. Background jobs

Sections 12-14 describe the engine's half of running an explicit calculation
in the background. The other half is `JobQueue` (`src/jobqueue.h`) and its
`JobModel` (`src/jobmodel.h`), in `flysight_core` next to `SessionModel`: Qt
Core and Gui only, no widgets, no GTSAM. There is one queue per application. It
owns the application's only worker thread.

**Reads never start jobs.** A job is created by `JobQueue::request()` and by
nothing else; the queue never re-requests on its own. Nor does product code
run explicit work any other way: the engine's synchronous `request()` (section
8) is for tests, and the cleanup audit (group `gestures`) keeps every call of
it out of `src/` outside `src/engine`. Python plugins are ordinary on-demand
readers and start nothing either
(`tst_python_bridge::pluginsNeverStartExplicitWork`). Loading a session starts
none either: a stored result is restored, not requested (15.8). A result that
was cancelled, superseded, or failed is simply missing, and whoever still wants
it asks again.

### 15.1 What a job is

A job is one explicit calculation for one session: `(sessionId, instanceId)`,
where the instance id already contains the registration id (`"<family>#<key>"`
for a family instance). A job stores the session **id** only - never a row, a
`SessionData *`, or an engine pointer. Each time the queue needs the session it
looks it up with `SessionModel::loadedSession()` under a `RowStabilityGuard` and
releases the guard before it emits anything.

**The queue never loads a session.** It never calls `sessionRef()`. A request
for a session that has no row or is not loaded is refused
(`SessionNotLoaded`), and a queued job whose session stops being loaded ends
Superseded at once rather than waiting for something nobody promised.

### 15.2 Lifecycle

```
request() --> Queued --> Running --> Succeeded | Cancelled | Superseded | Failed
                 |
                 +--> Cancelled | Superseded          (ended before it ever ran)
```

Every job ends in exactly one end state, enforced in one place
(`JobModel::markFinished`).

`request()` checks, in this order: shut down (`ShuttingDown`); an equal active
job (`AlreadyActive`, with its id); session not loaded (`SessionNotLoaded`);
then `CalculationEngine::readiness()`: `Unknown` -> `UnknownCalculation`,
`MissingInput` -> `MissingInput` (no job can be created for a session without
the inputs), `Blocked` -> `Blocked` (request the blockers instead: chaining is
the caller's), `Done` -> `NothingToDo` (already computed, a cached rejection or
failure included, a restored result included, or not an explicit
calculation), `Ready` -> a new Queued job (`Created`). It never prepares and
never starts anything synchronously.

**Deduplication.** A request whose `(sessionId, instanceId)` equals that of a
queued or running job creates nothing. The exception: a *running job that has
been asked to stop* - cancelled, or stopped because its inputs went stale
(section 15.3) - does not count. It is winding down, and a new request
creates a new queued job, which cannot start before the old one has ended.
Without this a refresh pressed right after a cancel, or right after an edit,
would be lost. `activeJob()` applies the same rule.

**Inputs are captured when a job starts**, not when it is requested: a job
queued behind a five-minute fit sees the session as it is five minutes later.
Jobs start one at a time, oldest first, always from the event loop. The running
slot is freed only after the worker thread has been joined, which is what
guarantees that the next job cannot start before the running one has ended.

At start (`prepare()`); no compute ran and `startedAt` stays invalid:

| `PrepareOutcome::Kind` | End state | Reason |
|---|---|---|
| `NotFound` | Superseded | "Calculation is no longer registered" |
| `NotExplicit` | Superseded | "Calculation is no longer requested explicitly" |
| `AlreadyValid` | Superseded | "Result is already available" |
| `NothingToRun` | Superseded | "Inputs changed: nothing to compute" (`invalidated` is published) |
| `Blocked` | Superseded | "Inputs changed: waiting for %1", the blockers' titles (`invalidated` is published) |
| `Ready` | runs | |

When the worker has returned, in this order:

| Condition | End state | Reason / extras |
|---|---|---|
| an end was decided while it ran (cancel, a stale ticket, abandonment, shutdown, failed start) | that state | that reason; the ticket is destroyed **without** `publish()` |
| `Published`, status `Ok` | Succeeded | `PublishOutcome::detail` (a rejection's reason; empty for a plain success); `resultStatus` = `Ok` |
| `Published`, any other status | Succeeded | "Calculation failed: %1"; `resultStatus` = that status. The failure is a function of the inputs: published, cached, not requestable again |
| `RefusedStale / InputsChanged` | Superseded | "Inputs changed" |
| `RefusedStale / AlreadyPublished` | Superseded | "Result is already available" |
| `RefusedGone / SessionGone` | Superseded | "Session removed or unloaded"; "Session data replaced" when the session model still has a loaded row with that id (see below) |
| `RefusedGone / RegistrationRemoved` | Superseded | "Calculation is no longer registered" |
| `Discarded / ResourceExhausted` | Failed | "Out of memory"; nothing cached, requestable again |
| `Discarded / Cancelled` | Cancelled | "Cancelled by the calculation" (it threw `CalculationCancelled` unasked) |

`publish()` is always called when no end is pending, so the engine's staleness
verdict wins over a discarded run. The four refusal rows are also what a job
stopped early for a stale ticket ends with (section 15.3): one mapping from
`PublishOutcome::Reason` to the reason text serves both. `SessionGone` says
only that the engine died; that also happens when the row's `SessionData` is
replaced by another object (a move-assignment brings its own engine) while
the row stays. The engine cannot tell the two apart and the session model
can, so the queue asks it (`loadedSession()`) at the moment it maps the
refusal: a loaded row with the job's session id means "Session data replaced",
anything else - no row, a stub, a model that is being destroyed - "Session
removed or unloaded". A worker thread that
cannot be started ends the job Failed ("The worker thread could not be
started") with nothing cached. "Succeeded" means *this job published a
result*; `AlreadyValid` and `AlreadyPublished` are therefore Superseded.

**Order of a job's end**, whatever the path: (1) the model's end transition;
(2) for a publication, `SessionModel::publishCalculationInvalidation()` with
the engine's `invalidated` set, so a `dependencyChanged` listener that looks at
the job model already sees the job finished; (3) `jobFinished`, `jobsChanged`;
(4) the session is unpinned; (5) the model trims its finished rows; (6)
`idle()`, or the next start is scheduled. Slots connected to the queue's
signals may call `request()`, `cancel*()` and `shutdown()`.

### 15.3 Cancellation, abandonment, shutdown

- `cancel(id)` on a queued job ends it Cancelled ("Cancelled") at once; its
  record stays as a finished entry. On the running job it requests
  cancellation; the job stays Running (`cancelRequested`) until the compute
  function returns, and the next job does not start before then.
- **Cancel wins over a late result.** Once cancellation was requested the job
  ends Cancelled and publishes nothing, even if the compute function returned a
  complete result. The outcome does not depend on a race the user cannot see.
- **A running job whose ticket is certain to be refused is stopped early.** An
  input edit, a merge, a removed session, a removed registration or a cleared
  cache makes the engine mark the ticket at once (section 12), but the worker
  cannot see that and would compute to the end - minutes, for a fit - only to
  be refused. So the queue asks the ticket (`willBeRefused()`, main thread) on
  every main-thread signal that follows such a change:
  `SessionModel::dependencyChanged`, `modelChanged`, `dataChanged`,
  `modelReset`, `rowsRemoved` and `destroyed`, and the registry's observer
  call. There is no timer and no polling. When the answer is yes it requests
  cancellation through the same flag and records the end `publish()` would
  have reported: **Superseded**, with the refusal's reason text ("Inputs
  changed", "Session removed or unloaded", "Session data replaced",
  "Calculation is no longer registered"). `jobCancelRequested` is emitted and the job is, from that
  moment, a running job that was asked to stop: `activeJob()` does not return
  it, a new request for the same calculation is `Created` and runs after the
  old worker has been joined, with the new inputs. Nothing is published,
  nothing is cached, and the queue still re-requests nothing by itself. The
  engine decides; the queue only stops waiting for a verdict it already has.
  A compute function that ignores the request, and a staleness none of those
  signals announces (an engine cleared behind the model's back), end
  Superseded at publish exactly as before.
- **The pending end is decided once: the first writer wins.** A job the user
  cancelled ends Cancelled even if its inputs change afterwards; a job stopped
  for a stale ticket ends Superseded ("Inputs changed") even if the user
  cancels it afterwards (`cancel()` still returns true; `cancelSession()` and
  `cancelAll()` do not count it again). `shutdown()` follows the same rule.
- `cancelUnwantedQueued(isWanted)` ends the *queued* jobs the predicate rejects
  ("No longer needed"). The running job is never offered to the predicate: its
  result is valid and worth keeping.
- When the session model removes rows or resets, queued jobs whose session is
  no longer loaded end Superseded ("Session removed or unloaded"), and the
  running job of such a session is abandoned: cancellation is requested so that
  a long fit for a deleted session does not hold the queue, and the job ends
  Superseded, which is what happened.
- `shutdown()` refuses later requests, ends queued jobs Cancelled ("Application
  closing"), requests cancellation of the running job, and **waits for the
  worker without a timeout**. Quitting must neither hang nor crash, and
  abandoning a live thread inside a solver and letting teardown
  proceed is a crash. The bound "one solver step" is delivered by the compute
  function's cancellation boundaries; the queue adds nothing on top: the wait
  ends as soon as `compute()` returns, whatever it returns. Idempotent; called
  by the destructor. The application calls it before tearing anything else down.
- Teardown order is not load-bearing for memory safety: the queue holds the
  session model weakly, and an engine that dies nulls its tickets (section 12).

### 15.4 Pinned sessions

A session with an active job should not be unloaded from under it. Every job
pins its session from creation to its end (`SessionModel::pinSession()` /
`unpinSession()`, counted per session id). A pinned loaded row is passed over
by LRU eviction exactly like a row whose save failed, so the cache may exceed
its capacity by the number of pinned rows; releasing the last pin schedules one
eviction pass for the next event-loop pass. A pin prevents eviction and nothing
else: `removeSessions()`, a merge, and a repopulation go ahead, the engine
marks the ticket, and the job is stopped and ends Superseded (section 15.3).
`SessionModel` knows pinned ids and nothing about jobs.

`SessionModel::publishCalculationInvalidation(sessionId, keys)` is how
engine-returned invalidations reach consumers: one publishing `dataChanged` for
the row, `dependencyChanged` per key, `modelChanged` - immediately, and only
for a loaded row. Publishing marks nothing dirty and saves no session file.
The engine's explicit-result listener stores an `Ok` result as a record in the
logbook's `cache/` folder during the install itself (15.8), and writing that record
drops the session's cached values of the columns over that calculation
(section 17).

### 15.5 The worker and what crosses threads

One private `QThread` exists per running job, created when the job starts, with
a 64 MiB stack (`JobQueue::kWorkerStackSize`; large GTSAM elimination trees
overflow default stacks; the space is reserved, committed lazily, and exists
only while a job runs), joined and deleted on the main thread when the job
ends. At most one exists at a time. Its `run()` is one statement:
`result = ticket->compute(&progress)`. The ticket, the facility and the result
slot are handed over before `start()` and read back only after `wait()`; those
two calls are the happens-before edges, so there is no lock.

Exactly two things cross threads while a job runs:

- **Progress text.** `CalculationProgress::report()` posts a queued invocation
  to the queue with the text copied by value (Qt's thread-safe event queue, not
  shared state). Posts that arrive for a job that has ended are ignored; posts
  pending when the queue dies are dropped by Qt.
- **The cancel request.** One `std::atomic<bool>` inside the facility, written
  by the main thread, read by `isCancelled()` on the worker. It is the
  progress-and-cancel facility itself, not a lock, and it is the only atomic in
  the queue.

The queue never pauses, wakes, or registers with `IdleScheduler`: saves, loads
and column work continue during a job.

### 15.6 The model

`JobModel` is the store of the `JobRecord`s, not a copy: the queue keeps no job
list of its own, so the model is the single source of truth about work in
progress and a jobs dock can be a pure view of it.

- One row per job in request order (ascending `JobId`; ids start at 1 and are
  never reused). Rows are appended; a row index changes only when earlier rows
  are removed. Remember jobs by `JobIdRole`.
- Columns (`Qt::DisplayRole`): `SessionColumn` (name), `CalculationColumn`
  (title), `StateColumn` (`stateText()`), `ProgressColumn`, `QueuedColumn`,
  `StartedColumn`, `FinishedColumn` (local short-format text, empty when
  invalid), `ReasonColumn`.
- Roles, answered on every column: `JobIdRole`, `SessionIdRole`,
  `SessionNameRole` (a snapshot at request: `_DESCRIPTION`, else the id),
  `CalculationIdRole`, `InstanceIdRole`, `CalculationTitleRole`, `StateRole`
  (`int(JobState)`), `CancelRequestedRole`, `ProgressTextRole` (kept after the
  end), `QueuedTimeRole` / `StartedTimeRole` / `FinishedTimeRole` (`QDateTime`,
  UTC; started is invalid for a job that never ran), `ReasonRole`,
  `ResultStatusRole` (`int(ResultStatus)` for Succeeded, else invalid),
  `IsFinishedRole`. `roleNames()` exposes them in camelCase.
- Every transition is its own signal, never coalesced: `rowsInserted` (Queued);
  `dataChanged` over the whole row for Queued -> Running, for cancel-requested,
  and for the end; `dataChanged` on `ProgressColumn` with
  `{Qt::DisplayRole, ProgressTextRole}` for progress; `rowsRemoved` for removal
  and trimming. No `modelReset` after construction.
- `removeFinished(id)`, `clearFinished()` and `removeRows()` remove finished
  rows only; `removeRows()` over a range containing an active job removes
  nothing and returns false.
- Finished rows are kept for the life of the application up to
  `finishedLimit()` (default 200, `setFinishedLimit()`); the oldest finished
  rows are trimmed after a job's end transition has been signalled; active rows
  never. No job is persisted: the job history lives as long as the
  application. (The result a job published may be: 15.8.)

### 15.7 API and threading rules

Everything below is **main thread only**, signals included. `request()`,
`cancel()`, `cancelSession()`, `cancelAll()`, `cancelUnwantedQueued()` and
`shutdown()` must additionally not be called from inside a calculation or an
engine callback (they inspect or publish to engines).

| Member | Meaning |
|---|---|
| `JobQueue(SessionModel *, QObject *parent)` / `~JobQueue()` | holds the session model weakly; the destructor calls `shutdown()` |
| `JobQueue::kWorkerStackSize` | 64 MiB |
| `model()` | the `JobModel` (a child of the queue) |
| `request(sessionId, CalculationBlocker)` / `request(sessionId, plainCalculationId)` -> `RequestResult {kind, job, created()}` | 15.2; `job` is non-zero for `Created` and `AlreadyActive` |
| `activeJob(sessionId, instanceId)` | the job a request would be deduplicated against, 0 if none; never a running job that was asked to stop |
| `activeJobs()` | queued and running ids in request order; the running job first |
| `runningJob()`, `job(id)`, `isIdle()`, `isShutDown()` | queries; `job()` returns a default record (id 0) for an unknown or removed job |
| `cancel(id)` | false: unknown or already finished |
| `cancelSession(sessionId)`, `cancelAll()` | number of jobs newly cancelled or asked to stop |
| `cancelUnwantedQueued(isWanted)` | number cancelled; never the running job |
| `shutdown()` | 15.3 |
| `failNextWorkerStarts(n)` | test seam: thread-creation failure cannot be provoked portably |
| signals `jobQueued(id)`, `jobStarted(id)`, `jobProgress(id, text)`, `jobCancelRequested(id)`, `jobFinished(id, state)`, `jobsChanged()` (after each of the others except `jobProgress`), `idle()` (the last active job ended) | `jobQueued` follows the model's `rowsInserted`. A job that a `rowsInserted` slot ended (cancel, shutdown) is not announced as queued afterwards: `jobFinished` was its only signal, and `request()` still returns `Created` |
| `JobModel(QObject *parent)`; `rowCount`, `columnCount`, `data`, `headerData`, `flags`, `roleNames`, `removeRows`; `rowOf(id)`, `record(row)`, `record(id)`, `records()`, `stateText(state)`, `removeFinished(id)`, `clearFinished()`, `finishedLimit()`, `setFinishedLimit(n)`, `kDefaultFinishedLimit` | 15.6. Only `JobQueue` appends rows and changes job state: it is a friend for the private mutators only, and reads through `records()` like any view. `records()` is a reference to the rows, valid until the model next changes |
| `JobId`, `JobState`, `JobRecord` (`isFinished()`, `isActive()`) | the vocabulary, `src/jobmodel.h` |
| `SessionModel::pinSession(id)`, `unpinSession(id)`, `isSessionPinned(id)` | 15.4. `unpinSession()` never evicts synchronously, so it is safe in a slot |
| `SessionModel::publishCalculationInvalidation(id, keys)` | 15.4. Not while a `RowStabilityGuard` is held (it emits) |

Tests: `tests/tst_jobqueue.cpp`, `tests/tst_jobmodel.cpp`, and the controllable
calculations of `tests/support/jobfixture.h`.

### 15.8 Stored results

The application's half of stored results (the engine's is section 12, export
and restore; the files are described in
[DATA_SCHEMA.md](DATA_SCHEMA.md#12-stored-calculation-results) section 12).

- `CalculationResultStore` (`src/calculationresultstore.h`) is owned by
  `SessionModel`, holds no state but counters, and runs on the main thread
  only. Every record file is read, written and deleted through
  `LogbookManager`, in the logbook's `cache/` folder (a sibling of
  `sessions/`, which holds only the session files). The manager creates
  `cache/` at the first record write; a failure to create it is a failed write
  like any other. A missing `cache/` holds no record.
- `SessionModel::attachSession()` installs the engine's explicit-result
  listener, so both install paths write: the queue's publish and a synchronous
  `request()`.
- `Installed` with status `Ok`: `exportResult()`, then
  `CalculationRecord::stamped()` (which adds `CalculationCompatibilityVersion`),
  then
  `LogbookManager::writeCalculationRecord()`, on the main thread. The write is
  atomic (`QSaveFile`). A failure is warned once, leaves the previous record
  and the in-memory result untouched, and is not retried before the next `Ok`
  publish.
- `Installed` with any other status writes nothing and deletes nothing.
- `DroppedByInputChange` (an input change, or a registry change made while the
  application runs, dropped the result):
  `LogbookManager::removeCalculationRecord()`, which looks at the record's one
  path (no directory listing).
- Explicit family instances (`<familyId>#<key>`) are not stored: the store
  ignores their events (`exportResult()` refuses them), so their results are
  lost on unload like on-demand ones. No built-in family is explicit.
- Every path that installs a session into a row restores that session's valid
  records after `attachSession()` and before the row is published
  (`sessionLoaded`, `dataChanged`, any plot pass): `sessionRef()`, the unloaded
  branch of `mergeSessions()`, and the promotion of a bulk edit's temporary
  session. The loaded-in-place merge needs no restore: it changes the session
  through the setters, so only the results it touches are dropped, with their
  records. The names a restore invalidates need no publication after a fresh
  load, because nothing has read the new engine yet; the unloaded merge
  publishes them with the names the merge changed.
- A session the logbook manager knows no record of
  (`LogbookManager::knownCalculationRecords()`: the names seen at start-up
  plus its own writes and removals) is restored without listing
  `cache/`. Otherwise the ids read are the listing of the session's record
  files together with the ids the manager knows, so that something other than
  a file standing at a known record's path (a directory) is read, and found
  unreadable, rather than ignored.
- Records are restored in passes until a pass restores none, so
  explicit-on-explicit chains restore in any file order. `InputsUnavailable`
  counts as stale only after the last pass.
- These are deleted at a load: a file that is not a record, a damaged record,
  a record of another format version (format 1, written by earlier versions,
  included: no migration), a record whose stamp is not current
  (`CalculationRecord::stampsAreCurrent()`: the compatibility marker only), a
  record failing a stale check (`Resolutions` included), and a record of a
  calculation that is not registered as explicit. A record whose result is
  already installed (`AlreadyInstalled`) is kept.
- A record that exists but cannot be opened or read in full
  (`CalculationRecordStatus::Unreadable`) is skipped: neither restored nor
  deleted, counted in `recordsSkipped`, warned once ("skipped (kept for the
  next load)"), and marked with
  `LogbookManager::markCalculationRecordSkipped()`, which keeps the column
  values over it out of `index.json` until the record is written or removed
  or the row is evicted. A record that stays `InputsUnavailable` only because
  it reads the result of a skipped record is skipped too. The next load tries
  again.
- The column worker's and the bulk edit's temporary loads never read a record.
- Records are deleted with their session (`LogbookManager::removeSession`) and,
  as strays whose session file does not exist in `sessions/`, at
  `initialize()`; that pass deletes in `cache/` only. Eviction, unloading, a
  registry change that does not reach a requested result, a removal as
  teardown and the model's destruction never delete one. A registry change
  made while the application runs that drops a requested result deletes its
  record like an input change. `AltitudeMarkerManager`'s destructor removes
  its registrations as teardown; the plugin host never unregisters.
- `cache/` may be deleted while the application is closed: `initialize()` then
  knows no record, every explicit calculation reads as not requested, and the
  start-up stamp check (DATA_SCHEMA section 11) drops each cached column value
  over a vanished record.
- A session created by an import has its file stem reserved
  (`LogbookManager::reserveSessionFile`), so a result published before its
  first save is stored under the name that save will use. A session that is
  never saved leaves a stray, removed at the next start.
- After a restore `readiness()` is `Done`, so `JobQueue::request()` answers
  `NothingToDo`.
- Record format and file names: `src/calculationrecord.h` and DATA_SCHEMA
  section 12. Test seam: `SessionModel::storedResultStats()` /
  `resetStoredResultStats()` (records written, restore calls and listings,
  records read, restored, kept, skipped and deleted, and the time spent).

Tests: `tests/tst_calcengine_restore.cpp`, `tests/tst_result_records.cpp`,
`tests/tst_result_store.cpp`, `tests/tst_result_columns.cpp`,
`tests/tst_fusion_store.cpp`, `tests/tst_plugin_identity.cpp` (the plug-in
code identity).

## 16. Plot-driven requests

Users think in plots, not calculations: "show fusion roll for these tracks" is
the request. `PlotRequests` (`src/plotrequests.h`) is the widget-free component
that turns that into jobs. It lives in `flysight_core` (Qt Core and Gui only)
next to `SessionModel`, `JobQueue` and `PlotModel` - the store of plot check
state, which is in `flysight_core` for this reason - and it is the **only place
in the application that calls `JobQueue::request()`**. The plot list's view
paints what it reports and forwards clicks to it; every decision about state,
counts, and what to request is made here and tested without widgets
(`tests/tst_plot_requests.cpp`).

A plot's checkbox means "show this plot wherever its data is available". It
makes no promise that the data will be computed: "checked but not computed" is
an ordinary state.

### 16.1 Track conditions

For each checked plot that is explicit-backed (16.3), every **visible, loaded
track** - a session-model row with `isLoaded() && visible && !loadFailed`,
exactly the rows the plot widget draws - is in one `PlotTrackCondition`. It is
derived from `CalculationEngine::blockers()` (section 13) of the plot's y name
and from the job queue:

| `BlockerReport::state` | Condition |
|---|---|
| `Available` | `Available`: the plot draws it |
| `Blocked`, at least one blocker has a live job | `Pending` |
| `Blocked`, every blocker was refused by the queue (below) | `NotApplicable` |
| `Blocked`, otherwise | `Missing` |
| `NotProduced` | `Failed`, with a reason built from the notes |
| `NotApplicable` | `NotApplicable`: silently absent, as plots treat missing data today |

- A **live job** is a queued or running job that was **not asked to stop**
  (the rule of `JobQueue::activeJob()`, section 15.2). A track whose job is
  winding down after a cancel is `Missing` at once, and so is a track whose
  running job the queue stopped because its inputs went stale (section 15.3):
  the row shows the refresh control, not cancel, from the moment of the edit,
  and a refresh pressed then queues a new job behind the old one. If a newer
  queued job exists for the same (session, instance), that one is the live
  job. The index is built once per pass from `activeJobs()` and `job(id)`.
- There is no "stale" condition. A result invalidated by an input change
  reports `Blocked` again and the track is simply `Missing`; a failed track
  whose inputs change becomes `Missing`, and therefore refreshable, the same
  way. `Blocked` wins over `NotProduced` as in section 13.
- A session without the explicit calculation's inputs reports `NotApplicable`:
  it is never missing, pending, or failed, and nothing is ever requested for it.
  As a second line of defence, a blocker for which `request()` answered
  `MissingInput`, `NothingToDo`, or `UnknownCalculation` is remembered as
  *refused* for that session until any of the session's names changes (or the
  session model is reset, or the registry changes); a track all of whose
  blockers are refused is `NotApplicable`, so that a row never shows a refresh
  control that can do nothing.
- The failure reason is, per note, `"<title>: <detail>"`; when the detail is
  empty, "Calculation failed" for `ResultStatus::Failed` and "No result for
  this recording" otherwise. Several notes are joined with `"; "`.

**Only the y name is inspected**: `DependencyKey::measurement(sensorID,
measurementID)`. The x-axis variable is a per-view setting of the application
and is not read here. This requires of every sensor produced by an explicit
calculation that **each of its time axes is an output of that calculation or is
derived on demand from its outputs**, so that "y available" implies "x
available" and a track whose y is blocked gets its x from the same job.
The registry cannot check this when a calculation is registered (plots and the
view's choice of axis belong to the application), so a **debug build** checks
it where a track is classified `Available`: `blockers()` of `<sensor>/_time`
and `<sensor>/_system_time` must not report `Blocked`, else a warning is
logged. Like every inspection it starts nothing and loads nothing; a release
build does not contain the check.

### 16.2 Row state

`PlotRequests::rowState(plotId)` returns a `PlotRowState`, a plain value;
`plotId` is `"<sensorID>/<measurementID>"`, equal to
`PlotModel::PlotValueIdRole` (`PlotRequests::plotId()` builds it). Over the
visible loaded tracks in session-model row order:

- `pending`, `missing`, `failed`: lists of `PlotTrackState` (`sessionId`,
  `sessionName` - `_DESCRIPTION`, else the id, read live - `condition`,
  `calculationTitles`, `reason` for a failed track, and for a pending one
  `job`, `jobState` and `jobProgressText`; `job` is the running live job among
  the track's blockers, else the oldest queued one). `pendingCount`,
  `missingCount`, `failedCount` are their sizes. Available and not-applicable
  tracks appear nowhere.
- `control()`: `Cancel` while anything is pending; else `Refresh` while anything
  is missing; else `None`. `controlCount()` is the number next to it
  (`pendingCount` / `missingCount` / 0). `showsWarning()` (`failedCount > 0`) is
  independent of both. There is no retry for a failed track: the same inputs
  give the same failure.
- `waitingTotal` / `waitingDone` and `progressLabel` ("1 of 3", empty unless
  `control() == Cancel`) come from the row's **waiting set**: the tracks the row
  is or was waiting for in the current episode. A gesture enters the tracks it
  got a job for, or found pending; a pass enters every track it observes
  pending (a row checked programmatically while another row's jobs run waits on
  them too). A waited-for track counts as done once it is available, failed, or
  not applicable. The set ends when the row has no pending track, on cancel,
  and on uncheck; a hidden or removed session leaves it at once.
- `jobProgressText`: the running job's progress text when this row waits on it.
  Rows that wait on the same job (roll, pitch and yaw share one fit) carry the
  same text - they are waiting on the same thing.
- `toolTip`: ready-made plain text, also available as the pure function
  `PlotRequests::buildToolTip(state)`. Sections are omitted when empty:

  ```
  Computing (1 of 3 done):
    <session name> - <titles>: <progress text | running | queued>
  Not computed (press refresh to compute):
    <session name>
  Could not be computed:
    <session name> - <reason>
  ```

  Until a jobs dock exists, this is where failures are reported; no message box
  is shown for a calculation outcome.
- `isPlain()`: nothing pending, missing, or failed - the row is painted exactly
  as today. `explicitBacked` is false, and the whole state is the default
  value, for an unchecked plot, a plot that is not explicit-backed, and an
  unknown id.

`rowStateChanged(plotId)` is emitted for each row whose state differs after a
pass (`operator==` on the whole value) and `rowStatesChanged()` once per pass in
which any did. A plot that stops being inspected falls back to the default
state and is announced once. `jobProgress` updates the texts of the stored
states without a pass and without inspection.

### 16.3 Explicit-backed plots, and what inspection costs

A plot is **explicit-backed** when any name in the static dependency closure of
its y name - `CalculationRegistry::staticDependencies(name).names`, which
includes the name itself and looks through source conversions - has a candidate
or a source conversion with explicit policy. One registry query answers it,
`CalculationRegistry::dependsOnExplicit(name)`. The logbook column cache asks
`CalculationRegistry::explicitDependencies(name)` (which explicit calculations;
section 17), of which `dependsOnExplicit()` is the non-emptiness, so rows and
columns cannot disagree. It is a pure
function of the registrations, memoized in the registry and per plot id, and
dropped by a registry observer. It is exact: `staticDependencies()` is a
superset of every dynamic dependency set and a blocker is always reached
through declared inputs, so a plot that is not explicit-backed can never report
a blocker.

Such plots are **never inspected**: no `blockers()` call, no read, no signal.
They cost one hash lookup. `blockers()` is called only for (checked and
explicit-backed plots) x (visible and loaded tracks) - names the plot widget
reads for the same tracks anyway, plus the cheap on-demand inputs the job would
capture - through `SessionModel::loadedSession()` under a `RowStabilityGuard`:
nothing is loaded, evicted, or touched in the LRU, and the guard is released
before anything is requested, cancelled, or emitted. Classifications are not
cached across passes (after A publishes, the blocker of B's output changes
from A to B although B's output may not be re-announced); the engine's caches
make a repeated `blockers()` cheap, and passes are coalesced to one per
event-loop pass with a zero-interval timer.

A pass is scheduled by: the queue's `jobsChanged`, `jobFinished`,
`jobCancelRequested`; `SessionModel::dependencyChanged` for a name in the
static closure of a checked explicit-backed plot (other names are ignored),
`visibilityChanged`, `modelChanged`, `sessionLoaded`, `modelReset`; the
`PlotModel`'s check-state `dataChanged` and `modelReset`; and a registry change.

### 16.4 What starts work: two gestures

| Gesture | Call |
|---|---|
| The user checked the plot by direct interaction with its row | `plotCheckedByUser(plotId)`, **after** the check state was written to the `PlotModel` |
| The user pressed the row's refresh control | `refreshPressed(plotId)` - the same request |

Both request, once, the blockers of every visible track that is missing for
that plot (blockers with a live job, and refused ones, are skipped), and return
the number of jobs created. It is a one-shot request, not a standing order. The
pass runs before the call returns, so the row is `Pending` when the view
repaints. A gesture on a plot that is not checked in the `PlotModel` (read
directly, so a wrong call order degrades to "nothing requested"), not
explicit-backed, or unknown does nothing and returns 0.

**A gesture is an explicit call from the view. It is never inferred from a
model change** ("when in doubt, it is not a gesture"). None of the following
starts a job. An affected track is `Missing` and the refresh control shows,
unless its session was loaded with a valid stored result (15.8): that makes it
`Available`, or `Failed` for a stored rejection, exactly as after a publish,
and it adds nothing to the refresh count:

- restoring checked plots at startup (`PlotModel::setPlots()` with settings),
  applying a profile (`setPlotEnabled()`), the Plots menu and its shortcuts
  (`togglePlot()`), and any other programmatic write of the check state,
  `setData(CheckStateRole)` included - the write is not the gesture;
- showing a track, loading a session, importing or merging a file;
- an input change that invalidates a published result;
- a job ending cancelled, superseded, or failed;
- a registry change; `rowState()`, `flush()`, and the pass itself;
- reads by the plot, legend, measure tool, logbook columns, the idle scheduler,
  the map, exports, and plugins (sections 8 and 13).

### 16.5 Chained continuation

When explicit calculation B consumes an output of unrequested explicit
calculation A, the blocker of a plot of B's output is A, and once A publishes
it is B (section 13). When a job **succeeded**, `PlotRequests` inspects, for
the job's session, every checked plot whose waiting set holds that session
with its `continues` flag set - the tracks **a gesture asked about** - and
requests the blockers that remain, without another gesture. This happens
synchronously in the `jobFinished` slot, that is inside `JobQueue::endJob()`
between `jobFinished` and the idle check, so the queue never reports `idle()`
between the links of a chain; the engine state is current there because the
publication's `dependencyChanged` precedes `jobFinished` (section 15).

Continuation stops - the flag is reset, the track is `Missing`, the refresh
control returns - when a job of the session ends `Cancelled`, `Superseded`, or
`Failed`, or is asked to stop - by a cancel, or by the queue because its
inputs went stale - (unless the track still has another live job
among its blockers); when the plot is unchecked; and when the track is hidden,
unloaded, or removed. A track that is shown again does not regain it. A row
that is only observed pending (checked programmatically while another row's
job runs) shows progress and continues nothing; a plot the *user* checks while
another row's job runs adopts the pending tracks and completes its own chain.

### 16.6 Cancel

`cancelPressed(plotId)` cancels every live job among the blockers of the row's
visible tracks - queued jobs end at once, the running job is asked to stop -
ends the row's waiting set, and returns the number of jobs cancelled or asked
to cancel. The plot stays checked: the component never writes to the
`PlotModel`. By the live-job rule the tracks are `Missing` before the worker
has returned, and every other row waiting on the same jobs changes the same
way in the same pass, because all rows are derived from the same queue state. A
refresh pressed while the cancelled job winds down creates a new job behind it.

### 16.7 Jobs nobody wants

When a plot is unchecked (or vanishes from the `PlotModel`) or a track is
hidden, the component ends, synchronously, the **queued** jobs that no checked
plot needs on a visible track (`JobQueue::cancelUnwantedQueued()`, reason "No
longer needed"). "Needed" is derived from (checked, explicit-backed plots) x
(visible, loaded tracks) - the (session, instance) of every reported blocker -
and never from who requested a job, so a queued job another checked plot still
needs survives. The running job is never offered: its result is valid and
cached, and hiding a track for a moment must not throw away minutes of work.
Only a cancel gesture or shutdown stops a running job. Removed sessions need no
pruning: the queue supersedes their jobs itself (section 15.3).

### 16.8 API and threading rules

Everything is **main thread only**, and no member may be called from inside a
calculation or an engine callback (they call `blockers()` and the queue).
Create the component after the `JobQueue` and destroy it before the queue. Every
collaborator is held weakly; a missing one makes the component inert (default
states, gestures return 0).

| Member | Meaning |
|---|---|
| `PlotRequests(SessionModel *, PlotModel *, JobQueue *, QObject *parent)` / `~PlotRequests()` | schedules one initial pass, so rows restored at startup show their refresh control without any event; the destructor removes the registry observer |
| `plotId(sensorId, measurementId)`, `plotId(PlotValue)` | the row's id, equal to `PlotModel::PlotValueIdRole` |
| `rowState(plotId)` | 16.2; the last computed state, at most one event-loop pass behind the models. A consumer that must not be behind (the "no data" warning of the plot widget) asks `blockers()` itself |
| `plotCheckedByUser(plotId)`, `refreshPressed(plotId)` | 16.4; number of jobs created |
| `cancelPressed(plotId)` | 16.6; number of jobs cancelled or asked to cancel |
| `flush()`, `hasPendingUpdate()` | run / report a pending pass (tests; the view never needs them) |
| `buildToolTip(state)` | 16.2; static and pure |
| `passCount()` | test seam: passes run so far |
| signals `rowStateChanged(plotId)`, `rowStatesChanged()` | 16.2 |
| `PlotRowState`: `plotId`, `explicitBacked`, `pendingCount`, `missingCount`, `failedCount`, `waitingTotal`, `waitingDone`, `progressLabel`, `jobProgressText`, `pending`, `missing`, `failed`, `toolTip`, `Control`, `control()`, `showsWarning()`, `controlCount()`, `isPlain()`, `operator==` | 16.2 |
| `PlotTrackState`: `sessionId`, `sessionName`, `condition`, `calculationTitles`, `reason`, `job`, `jobState`, `jobProgressText`, `operator==`; `PlotTrackCondition` | 16.1, 16.2 |

Tests: `tests/tst_plot_requests.cpp`, with the synthetic plots of
`tests/support/plotfixture.h` over the calculations of `jobfixture.h`.

### 16.9 The plot list view and application wiring

**The view paints and forwards; it decides nothing.** `PlotRowDelegate`
(`src/ui/docks/plotselection/PlotRowDelegate.h`), installed on the plot list by
`PlotSelectionDockFeature`, paints `PlotRowState` right-aligned in the row: the
refresh control with `controlCount()`, or `progressLabel` ("k of n") with the
cancel control, and independently the warning badge with `failedCount`. Refresh
and cancel share the right-most slot. The plot's name is elided to make room,
never the cluster; the row height never changes. A row whose state `isPlain()`
- every ordinary plot, every unchecked row, every category - is painted and
handled by the unmodified `QStyledItemDelegate`, pixel for pixel. The glyphs
are drawn with `QPainter` in the row's text colour (no bundled images). The row
tooltip is `PlotRowState::toolTip`, shown over the whole row; the job's
progress text appears there only. The delegate contains no text of its own.
The geometry and the control's hit rectangle are one pure function,
`layoutPlotRow()` (`PlotRowLayout.h`), tested without widgets.

**What counts as the check gesture.** `editorEvent()` compares the model's
check state before and after the base class handled the event. The base class
writes `Qt::CheckStateRole` only for a left click on the check box and for
Space / Select on the current row, synchronously, so "became Checked inside
this call" is exactly "checked by direct interaction with the row" - and the
model already holds Checked when `plotCheckedByUser()` is called. The three
programmatic paths never reach the delegate and are therefore never gestures:
the Plots menu and its shortcuts (`MainWindow::togglePlot`), applying a profile
(`applyProfile()` in `src/profilestatebridge.cpp`), and the restore of checked
plots from the settings (`PlotModel::setPlots()`). The delegate never connects
to `dataChanged` and never infers a gesture from a model change. Unchecking is
not a gesture.

**The control.** A left-button press and release, both inside the control's
hit rectangle (the icon's column over the full row height, out to the row's
edge), on the same row, with the same control showing at both moments, calls
`refreshPressed()` or `cancelPressed()`. The press is consumed, so the row is
neither selected nor toggled. A double click forgets the press: it requests
once and can never land on the cancel control that replaced the refresh
control. The label and the badge are inert. **There is no keyboard, menu, or
context-menu surface for refresh and cancel** (exactly two gestures start work;
when in doubt, it is not a gesture): a keyboard user unchecks and checks the
row with Space.

**Repaint.** `rowStateChanged(plotId)` updates that row of the view. There is no
animation and no timer.

**Ownership and order.** `MainWindow` creates the `JobQueue` and then the
`PlotRequests` in its constructor, after the session model is populated and the
calculations are registered and before any dock exists, and hands both to the
docks through `AppContext` (`jobQueue`, `plotRequests`). Restored plots and a
first-launch profile reach `PlotRequests` as ordinary model changes, so
start-up starts no job. `closeEvent()` calls `JobQueue::shutdown()` **first**,
before sessions are flushed and the layout is saved (with a wait cursor when
the queue is busy; the wait is at most one solver step). A future veto of the
close must be decided before that call: a queue that was shut down refuses
every later request. `~MainWindow()` deletes the `PlotRequests`, then the
`JobQueue`, explicitly and before everything else: `QObject` deletes children
in creation order, which would destroy the session model under the queue. The
delegate holds the component weakly and is the base delegate without it. No
signal of the queue or the component is connected to anything that shows a
dialog; no message box, status message, or progress dialog reports a
calculation outcome.

**The "no data" warning.** One reader warns when a checked plot has no data for
a visible track: `PlotWidget::updatePlot()`. For the value it just read as
empty it asks `PlotRequests::isMerelyUncomputed(session, sensor,
measurement)`, a static predicate of the widget-free core (`src/plotrequests.h`)
over `blockers(y name).state`, and stays silent when that is true: `Blocked`
(not computed yet) and `NotProduced` (ran and rejected its inputs, or failed);
both are shown by the plot list instead. The predicate asks the engine, not
`rowState()`, which may be one event-loop pass behind; it inspects only, so it
runs no explicit calculation, creates no job and loads nothing. A recording
that simply lacks the sensor (`NotApplicable`) still logs the warning. The
four states are unit-tested in
`tst_plot_requests::merelyUncomputedIsNotWorthAWarning`; the widget's one call
is step M1 of the manual script.

**Results appear through ordinary invalidation only.** Publishing a job's
result emits `dependencyChanged` per name, `dataChanged` for the row, and
`modelChanged` (`SessionModel::publishCalculationInvalidation()`, 15.4). The
plot widget and the legend rebuild on `modelChanged`, the logbook repaints from
`dataChanged`, the measure tool reads at interaction time. Nothing connects the
queue or the component to the plot, the legend, or the logbook. A restored
result (15.8) needs no publication: it is installed before the row's
`sessionLoaded`, which already makes the plot, the legend and the rows read the
session.

Tests: `tests/tst_plot_row_layout.cpp` (geometry, no widgets) and
`tests/tst_plot_row_delegate.cpp` (the delegate in an offscreen `QTreeView`;
the only test that links Qt Widgets, behind `FLYSIGHT_BUILD_WIDGET_TESTS`). The
real `MainWindow` paths - start-up, profiles, quit with jobs running, and
interactivity during a fit - are the manual script in `tests/README.md`,
section 12 (steps M1-M9). That `MainWindow` never calls a gesture is a rule of
the cleanup audit (group `gestures`).

## 17. Sensor fusion as a registered calculation

The batch GNSS/IMU fit (`src/fusion/fusion.h`, the kernel) reaches the engine
through one file, `src/fusion/fusionregistration.cpp`, and one entry point,
`Fusion::registerFusionCalculations(registry)`. Both live in the static library
`flysight_fusion`, the only product target that links GTSAM. `flysight_core`
never references it: `MainWindow` calls the entry point directly after
`registerBuiltInCalculations()`, and the fusion tests call it after
`TestEnvironment::registerBuiltIns()` (`FlySightTest::registerFusionOnce()`).
Three calculations are registered, in this order:

| Id | Policy | Inputs | Outputs |
| --- | --- | --- | --- |
| `builtin.fusion.fit` (title "Sensor fusion") | Explicit | the 22 below | the 18 below |
| `builtin.fusion.accH` | OnDemand | `Fusion/accN`, `Fusion/accE` | `Fusion/accH` |
| `builtin.fusion.systemTime` | OnDemand | `Fusion/_time`, `_TIME_FIT_A`, `_TIME_FIT_B` | `Fusion/_system_time` |

**Inputs of the fit** (all required; exactly the vector members of
`Fusion::Channels`, in member order, then the four origin attributes):

```
GNSS/_time
Local/north  Local/east  Local/down  Local/velN  Local/velE  Local/velD
GNSS/hAcc    GNSS/vAcc   GNSS/sAcc
IMU/_time
IMU/ax  IMU/ay  IMU/az  IMU/wx  IMU/wy  IMU/wz  IMU/temperature
_LOCAL_ORIGIN_INDEX  _LOCAL_ORIGIN_LAT  _LOCAL_ORIGIN_LON  _LOCAL_ORIGIN_HMSL
```

Effective values only: accelerations in m/s^2, rates in deg/s (the kernel
converts to radians), temperatures in degC, times in shared UTC. Everything
behind them - `GNSS/lat`, the `TIME` sensor, the time fit, `SCHEMA_VER` - is
transitive and tracked by the engine. Markers and preferences are not inputs:
dragging the exit marker does not drop a fit. A recording without IMU data,
without `IMU/temperature`, without a local origin (no fix under 10 m), or
without a time fit has a **missing input**: there is
nothing to compute, no job can be created, and blocker inspection reports
`NotApplicable`, never "not requested".

**Outputs of the fit**, published together: the measurements `Fusion/_time`,
`north`, `east`, `down`, `velN`, `velE`, `velD`, `accN`, `accE`, `accD`, `roll`,
`pitch`, `yaw`, `qx`, `qy`, `qz`, `qw` (no unit reported, like every derived
measurement), and the attribute `_FUSION_DIAGNOSTICS`
(`SessionKeys::FusionDiagnostics`, compact JSON as a string; not a logbook
attribute).

**No arithmetic in the adapter.** `channelsFrom()` copies the implicitly shared
input vectors field by field; the only logic is that a stored
`_LOCAL_ORIGIN_INDEX` that is not a number becomes -1 (the kernel's "outside
the GNSS samples") instead of silently meaning fix 0. Every validation rule,
unit conversion, and message is the kernel's and is held to the kernel's
goldens by the golden tests (`tests/README.md` section 11), so the
session-level outputs are bit-identical to the kernel's goldens.

**Outcome mapping.**

| `Fusion::Outcome` | The compute function |
| --- | --- |
| `Succeeded` | returns all seventeen measurements and `_FUSION_DIAGNOSTICS` |
| `Rejected`, `SolverFailed` | returns **only** `_FUSION_DIAGNOSTICS` and `setReason(reason)`: the measurements are unset, so unavailable. A function of the inputs, cached like any result (`ResultStatus::Ok`): the job ends Succeeded with that reason, `resultDetail()` returns it, blocker inspection reports `NotProduced` with that detail, and a second request runs nothing until a declared input changes. Stored and restored like a success (15.8) |
| `Cancelled` | throws `CalculationCancelled`: nothing is published, nothing is cached, nothing is stored |
| `std::bad_alloc` | not handled: it propagates to the engine (`ResourceExhausted` on the asynchronous path; nothing cached, nothing stored) |

**Progress and cancellation.** The compute function hands the kernel two
callbacks over `ctx.progress()`: one forwards each progress text to
`report()`, the other returns `isCancelled()`. The kernel calls them, in that
order, at its boundaries only: before the fit, every 256 states of every
graph build, and before each optimizer iteration of every fit, the
initializer's prefix and segment fits included (`SENSOR_FUSION.md` section
7); a linear solve in progress finishes first. `CalculationCancelled` is thrown by the compute function itself after
`run()` has returned `Cancelled`, never from a callback. On the synchronous
path the facility is `CalculationProgress::none()`, so `request()` and the
three-step path run the same fit on the same values. The compute function
holds no state and logs nothing (section 14).

**Derived values.** `Fusion/accH[i] = sqrt(accN[i]*accN[i] + accE[i]*accE[i])`.
`Fusion/_system_time[i] = (Fusion/_time[i] - b) / a` with the time fit's `a`
and `b`, as `builtin.time.system.GNSS` (both call
`Calculations::systemTimeFromUtc()`, `src/calculations/timefithelper.h`,
header-only because the fusion library does not link the built-in
calculations); unavailable when `a == 0` or when any
result is not finite. It is not a passthrough of `IMU/time`: the fused samples
are a subset of the IMU samples. Both are on demand, but their inputs exist
only once the fit has published, so they are blocked by the fit (section 13),
appear with it through ordinary invalidation, and never start one. Together
with `Fusion/_time` (an output of the fit) they satisfy the time-axis rule of
section 16.1.

**Plots.** Seventeen plots in the category "Sensor fusion"
(`MainWindow::registerBuiltInPlots`): the sixteen measurements other than
`_time`, and `accH`. They are explicit-backed (16.3), so they are computed
from the plot list and nowhere else.

**Stored results.** The fit's result version is `Fusion::Algorithm`
(`src/fusion/fusion.h`), the same string as the diagnostics' `"algorithm"`. The
literal exists once in `src/`, in that header. Change it whenever a change can
alter what the fit returns for the same channels; that drops every stored fit.
The record holds the seventeen measurements and `_FUSION_DIAGNOSTICS`, or, for
a rejection or solver failure, the diagnostics and the reason. Its leaves are
the source data and attributes behind the 22 inputs: the IMU and GNSS source
columns, `SCHEMA_VER`, the `TIME` sensor, the stored origin attributes. Markers
and preferences are not among them. Its resolutions name what provided each
name the fit looked up (the conversion layer's instances for recorded
measurements, the calculations behind derived channels such as the local frame
and the time fit, the session's own attributes). With the built-ins, no
altitude marker, no preference and no plugin calculation is among them, so
adding or removing an altitude marker, changing the descent pause timeout, or
editing a plugin keeps a stored fit; a plugin or a registration that declares
one of the names the fit looks up, ahead of the built-in, makes it stale.

**Logbook columns.** A column over names for which
`CalculationRegistry::explicitDependencies()` is not empty
(`logbookColumnExplicitCalculations()`, the same registry answer as 16.3) is,
for a loaded row, computed from the engine like any other column
(`SessionModel::computeColumnValues`): the restored or published value, or
unavailable when the calculation is not requested. It is cached in
`index.json` together with the session's `"records"` stamp (calculation id ->
result version of each record in `cache/`). Writing or deleting a record drops the
values over that calculation at once
(`LogbookManager::calculationRecordsChanged`); a loaded row recomputes them on
the next event-loop pass (`refreshRecordColumns`, which emits nothing: loaded
cells are live). A stub is settled without a load: pending (not cached, shown
empty) when the logbook knows a record of it, unavailable otherwise. The
column worker never reads a record. The ordering rule: a record write flushes
the index first when the index on disk lists that calculation under a cached
value, so no crash leaves a value that disagrees with the records. Unconfirmed
records (a failed write or removal, a record skipped at the load because it
could not be read) keep their values out of `index.json` until the row is
evicted or the record is written or deleted again. An environment change (a
registration, a declared preference, a changed result version such as a plugin
edit) discards every cached value but leaves the records valid; loaded rows
recompute from the engine and stubs with a record stay pending until loaded.
`CalculationCompatibilityVersion` did not change for
this: an index written before the stamp holds explicit-backed values only as
"unavailable", and at start-up they are kept only for sessions without a
record. The marker's current value, 2, identifies the
centered time fit (`_TIME_FIT_A` / `_TIME_FIT_B`, and with them every non-GNSS
`_time`); the constant's comment lists what each value stands for.

Tests (label `fusion`, behind `FLYSIGHT_BUILD_FUSION_TESTS`):
`tests/tst_fusion_session.cpp` (real `SessionData` engines, the fit on the
test's main thread), `tests/tst_fusion_jobs.cpp` (the job queue's worker on a
real `SessionModel`), `tests/tst_fusion_rows.cpp` (the plot rows of section
16 with the seventeen real plots and real fits),
`tests/tst_fusion_runner.cpp` (the command-line runner against the
application's import path) and `tests/tst_fusion_store.cpp` (the fit's stored
result: unload, restart, rejections, invalidation, merges, the session file
untouched; kept across altitude markers, unrelated registrations, the descent
pause and another plugin set; dropped at once by a registration that provides
a name it looked up; deleted when a lookup resolves differently at load); the
column rule without GTSAM in
`tst_column_cache::explicitBackedColumnFollowsItsResult` and
`tests/tst_result_columns.cpp` (the stamp, crash points, pending stubs), and
with a real fit in
`tst_fusion_jobs::columnOnFusionOutputIsCachedFromRecord`. The model, its
limitations and what is rejected are in [SENSOR_FUSION.md](SENSOR_FUSION.md).
