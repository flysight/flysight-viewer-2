# Writing a registered calculation

For C++ contributors. Users and firmware developers want
[DATA_SCHEMA.md](DATA_SCHEMA.md); Python plugin authors want
[the plugin README](../python_plugins/README.md), where the same rules apply
with a simpler surface.

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
[LOCAL_COORDINATES.md](LOCAL_COORDINATES.md).

## 5. Candidates and order

Several calculations may declare the same output. They are tried in
registration order; the first whose inputs are all available *and* which
produces the output wins. "Works with or without X" is therefore two
registrations, the one that needs X first.

Registration order is the order in `registerBuiltInCalculations`: the
conversion families, attribute, GNSS, IMU, MAG, time, local coordinates,
simplification, WS-P, SP, interpolation. Python plugins are registered before the built-ins. Stored data
always wins over any calculation.

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
`AltitudeMarkerManager`) is done by registering and unregistering, which
invalidates every loaded session.

## 8. Explicit policy

`EvaluationPolicy::Explicit` calculations run only through
`CalculationEngine::request(id)`; before that their outputs read as unavailable
(`ResultStatus::NotRequested`) without starting work, and they revert to that
state when an input changes. The same calculation can be run in the background
(section 12), and section 13 reports which explicit calculations stand behind a
name. `request` returns the names whose cached "not requested"
answer was dropped: a future model-level caller must publish that set through
`SessionModel`, which is the single emitter of `dependencyChanged`. An explicit
calculation whose input transitively depends on its own output is a cycle like
any other: `request` drops the cached "not requested" answers before it
evaluates, reports the cycle, returns `ResultStatus::Cycle`, and publishes
nothing.

## 9. When to bump `CalculationCompatibilityVersion`

The constant is in `src/calculations/builtincalculations.h`; its comment is the
authority. Bump it whenever a code change can alter the value that any
existing session yields for any logbook column:

- a built-in calculation's arithmetic, inputs, or candidate order;
- the schema table or the unit-normalization table (`src/conversion`,
  `src/units/unitconversion.h`);
- the interpolation family;
- `SessionModel::computeColumnValues` (what a column stores, or its unit).

Do not bump it for added, removed, or renamed registrations: the environment
fingerprint (`calculationEnvironmentFingerprint`) covers those. Never reuse a
value, and never use 0.

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
which keeps the cached logbook columns in step with the saved file.

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
- `CalculationEngine::preparedCount()` - outstanding tickets (instrumentation;
  main thread). `edgeCount()` includes their edges, `cachedNodeCount()` does not.

A compute function that returns normally although cancellation was requested
yields `Completed`, and publishing it would be correct. Whether to publish it
is the job queue's decision (it does not: cancel wins).

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

- A `PreparedCalculation` is created, inspected, published, and destroyed on the
  main thread. **Only `compute()` may run elsewhere**, once, and the caller
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
