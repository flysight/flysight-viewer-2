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

## 15. Background jobs

Sections 12-14 describe the engine's half of running an explicit calculation
in the background. The other half is `JobQueue` (`src/jobqueue.h`) and its
`JobModel` (`src/jobmodel.h`), in `flysight_core` next to `SessionModel`: Qt
Core and Gui only, no widgets, no GTSAM. There is one queue per application. It
owns the application's only worker thread.

**Reads never start jobs.** A job is created by `JobQueue::request()` and by
nothing else; the queue never re-requests on its own. A result that was
cancelled, superseded, or failed is simply missing, and whoever still wants it
asks again.

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
failure included, or not an explicit calculation), `Ready` -> a new Queued job
(`Created`). It never prepares and never starts anything synchronously.

**Deduplication.** A request whose `(sessionId, instanceId)` equals that of a
queued or running job creates nothing. The exception: a *running job that has
been asked to cancel* does not count. It is winding down, and a new request
creates a new queued job, which cannot start before the old one has ended.
Without this a refresh pressed right after a cancel would be lost.
`activeJob()` applies the same rule.

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
| an end was decided while it ran (cancel, abandonment, shutdown, failed start) | that state | that reason; the ticket is destroyed **without** `publish()` |
| `Published`, status `Ok` | Succeeded | `PublishOutcome::detail` (a rejection's reason; empty for a plain success); `resultStatus` = `Ok` |
| `Published`, any other status | Succeeded | "Calculation failed: %1"; `resultStatus` = that status. The failure is a function of the inputs: published, cached, not requestable again |
| `RefusedStale / InputsChanged` | Superseded | "Inputs changed" |
| `RefusedStale / AlreadyPublished` | Superseded | "Result is already available" |
| `RefusedGone / SessionGone` | Superseded | "Session removed or unloaded" |
| `RefusedGone / RegistrationRemoved` | Superseded | "Calculation is no longer registered" |
| `Discarded / ResourceExhausted` | Failed | "Out of memory"; nothing cached, requestable again |
| `Discarded / Cancelled` | Cancelled | "Cancelled by the calculation" (it threw `CalculationCancelled` unasked) |

`publish()` is always called when no end is pending, so the engine's staleness
verdict wins over a discarded run. A worker thread that cannot be started ends
the job Failed ("The worker thread could not be started") with nothing cached.
"Succeeded" means *this job published a result*; `AlreadyValid` and
`AlreadyPublished` are therefore Superseded.

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
  worker without a timeout**. The specification asks both for no hang and for
  no crash; abandoning a live thread inside a solver and letting teardown
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
refuses the ticket, and the job ends Superseded. `SessionModel` knows pinned
ids and nothing about jobs.

`SessionModel::publishCalculationInvalidation(sessionId, keys)` is how
engine-returned invalidations reach consumers: one publishing `dataChanged` for
the row, `dependencyChanged` per key, `modelChanged` - immediately, and only
for a loaded row. A published result is not a persistent change: no cached
logbook column is invalidated, nothing is marked dirty, nothing is saved.

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
  never. Nothing is persisted.

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
| `activeJob(sessionId, instanceId)` | the job a request would be deduplicated against, 0 if none |
| `activeJobs()` | queued and running ids in request order; the running job first |
| `runningJob()`, `job(id)`, `isIdle()`, `isShutDown()` | queries; `job()` returns a default record (id 0) for an unknown or removed job |
| `cancel(id)` | false: unknown or already finished |
| `cancelSession(sessionId)`, `cancelAll()` | number of jobs newly cancelled or asked to stop |
| `cancelUnwantedQueued(isWanted)` | number cancelled; never the running job |
| `shutdown()` | 15.3 |
| `failNextWorkerStarts(n)` | test seam: thread-creation failure cannot be provoked portably |
| signals `jobQueued(id)`, `jobStarted(id)`, `jobProgress(id, text)`, `jobCancelRequested(id)`, `jobFinished(id, state)`, `jobsChanged()` (after each of the others except `jobProgress`), `idle()` (the last active job ended) | |
| `JobModel(QObject *parent)`; `rowCount`, `columnCount`, `data`, `headerData`, `flags`, `roleNames`, `removeRows`; `rowOf(id)`, `record(row)`, `record(id)`, `stateText(state)`, `removeFinished(id)`, `clearFinished()`, `finishedLimit()`, `setFinishedLimit(n)`, `kDefaultFinishedLimit` | 15.6. Only `JobQueue` (a friend) appends rows and changes job state |
| `JobId`, `JobState`, `JobRecord` (`isFinished()`, `isActive()`) | the vocabulary, `src/jobmodel.h` |
| `SessionModel::pinSession(id)`, `unpinSession(id)`, `isSessionPinned(id)` | 15.4. `unpinSession()` never evicts synchronously, so it is safe in a slot |
| `SessionModel::publishCalculationInvalidation(id, keys)` | 15.4. Not while a `RowStabilityGuard` is held (it emits) |

Tests: `tests/tst_jobqueue.cpp`, `tests/tst_jobmodel.cpp`, and the controllable
calculations of `tests/support/jobfixture.h`.

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

- A **live job** is a queued or running job that was **not asked to cancel**
  (the rule of `JobQueue::activeJob()`, section 15.2). A track whose job is
  winding down after a cancel is `Missing` at once; if a newer queued job exists
  for the same (session, instance), that one is the live job. The index is
  built once per pass from `activeJobs()` and `job(id)`.
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
(`candidatesFor()`) with explicit policy. This is a pure function of the
registrations, memoized per plot id, and dropped by a registry observer. It is
exact: `staticDependencies()` is a superset of every dynamic dependency set and
a blocker is always reached through declared inputs, so a plot that is not
explicit-backed can never report a blocker.

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
starts a job; the affected tracks are `Missing` and the refresh control shows:

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
`Failed`, or is asked to cancel (unless the track still has another live job
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
