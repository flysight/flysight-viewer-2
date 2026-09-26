# Writing a registered calculation, and running one in the background

For C++ contributors. Users and firmware developers want
[DATA_SCHEMA.md](DATA_SCHEMA.md); Python plugin authors want
[the plugin README](../python_plugins/README.md), where the same rules apply
with a simpler surface.

Sections 1-11 are about writing a calculation. Sections 12-17 are about
explicit calculations that run in the background: the asynchronous request
(12), blocker inspection (13), the threading rule (14), the executor of
background jobs (15), the demand layer that decides what they compute (16),
and sensor fusion as a registered calculation (17).
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

**Registry changes while the application runs.** A registration or removal
made while sessions are loaded re-resolves exactly the cached names whose
answer it can change, and drops what was computed from them. Resolution has
three steps, and whichever applies is final:

1. an attribute with a stored value: that value, even an invalid one;
2. a measurement with source data: the source conversions in registration
   order while any is registered, otherwise the passthrough;
3. any other name: its candidates, plain calculations and families
   interleaved in registration order.

A candidate that is not requested (explicit), lacks an input, fails or runs
without providing the name is passed over and the next one is tried; the
ones after the winner are never tried. A registration always goes to the end
of the order, so it changes only a name that resolved to nothing in the step
it belongs to, and the first source conversion changes every passthrough. It
never changes a name that the session's own data answers or that an earlier
candidate provides. A removal changes a name only when the removed
registration provided it, or when the last source conversion goes. Removing
a candidate that was tried and passed over leaves every answer alone; a
requested result that has a stored copy (an `Ok` result `exportResult()`
exports) is dropped by it only when its lookups reached something solely
through that candidate, because its stored copy lists those lookups and
would no longer match after a restart. A requested result without a stored
copy (`MissingInput`, `Failed`, one that met a ring) is never dropped that
way. So a runtime change drops a
requested result exactly when a restore of its stored copy under the changed
registry would be stale, with one conservative exception: a registration for
a name that resolved to nothing and that does not provide it either (an
explicit calculation nobody requested, or one that fails having looked up
only what the result already reached) drops the result, because whether a
new candidate provides a name is known only by running it and invalidation
never computes, while the restore would succeed. Answers whose evaluation met
a dependency ring always re-resolve.
`tst_calcengine_restore::registryChangeMirrorsRestore` checks the equivalence
case by case.

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
every loaded session, invalidates what depended on a name whose answer the
change alters (section 5). A requested result whose lookups the change does
not alter (it never looked those names up, or the session's own data or an
earlier candidate answers them) stays installed, and so does its stored copy
(15.8); one whose lookups it alters is dropped and its record deleted.
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
part of the column environment of every logbook column whose closure reaches
the calculation (section 9), and a stored result records it for its own calculation and for
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
loaded (`SessionModel::settleExplicitColumns`, section 17); while a column over
such an output is enabled, the demand layer then has the calculation computed
for that session (section 16.8). The registry does
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
result version: the column environment covers those for the logbook column
cache (below). It covers every registration's result version too, so a plugin
edit discards, at the next start, the cached values of the columns that can be
computed through a plugin calculation. A stored result does not depend on the
column environment: a registration makes it stale only by changing what a
name it looked up resolves to. Never reuse a value, and never use 0.

**The column environment.** Each logbook column has one
(`logbookColumnEnvironment()` = `calculationEnvironmentDigest()` over
`logbookColumnNames()`, `src/calculations/builtincalculations.h` has the exact
encoding), recorded in `index.json` next to the column's definition. It is a
SHA-1 over the column's static closure C (`staticDependencies()` of every
name the column reads: every name reachable through the declared inputs of
every candidate and, for a measurement, of every source conversion - not only
the ones that would win) and its preferences P: for every name of C, the
candidates in the order they are tried (`candidatesFor()`, family instances
included) with each one's result version; for a measurement also whether any
source conversion is registered and the conversions accepting it in order;
and the value of every preference of P. A cached column value is kept while
the marker and its column's environment are unchanged, at start-up
(`LogbookManager::initialize()`) and at run time
(`SessionModel::checkCalculationEnvironment()` on every registry change and
on every change of a preference some closure or calculation declares,
`LogbookManager::checkColumnEnvironments()`); a column whose environment
changed loses its cached values in every row and is recomputed (an
explicit-backed column of an unloaded row from the session's stored results,
which the column worker restores into its temporary copy: section 17). The
run-time check is queued for the next event-loop pass; every path that stores
a column value (`fillMissingColumns`, `settleExplicitColumns`,
`restoreForColumnWorker`) runs a pending check first, so a value computed
after a change is never stored, or flushed, under its column's previous
environment.

What "a change can affect a column" means, and why keeping the others is
safe: evaluating the column resolves only names of C (a name is resolved by
reading session state - a stored attribute, source data through the
passthrough - or by trying exactly the candidates listed for it; a candidate
is a pure function of its declared inputs, which are names of C, preferences
of P, or the source layer). So the value is a function of the session file,
the session's records, the code, and exactly the registry and preference
facts the digest lists. A change that leaves the digest equal cannot alter
what a fresh evaluation gives; one that does not leave it equal is treated as
affecting the column, even when the value would come out the same (a new
candidate tried after a stored attribute, say). In particular:

- a registration providing a name in C, or a family accepting one, changes
  the candidate list of that name; a registration of a name outside C (an
  altitude marker no column reads) changes nothing for the column - only a
  column that reads the marker, directly or through its inputs, is
  recomputed; a registration cannot enlarge C without being a candidate for a
  name already in it;
- a removal is the same change in reverse;
- a preference change reaches only the columns whose P holds the key;
- a plug-in edit changes the result version of every plugin registration, so
  exactly the columns whose C has a plugin candidate for some name;
- the conversion layer: whether any source conversion is registered is part
  of the environment of every measurement name, and the conversions accepting
  a name of C are listed for it;
- a column created by the change (a marker column, say) has no cached value
  yet; a removed or disabled column is not written to the index, and a
  re-enabled one is checked against its recorded environment before its
  values are used (`SessionModel::rebuildColumns()`).

Not covered, as before: code changes behind an unchanged id and result version
(the marker's job), session edits (unsaved marks and `invalidateColumns`) and
record changes (the `"records"` stamp).

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
engine creates no thread: the caller (the executor, section 15) decides where
step 2 runs.

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
  the engine and not part of any column environment.
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
is the executor's decision (it does not: cancel wins).

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
  - `Dropped` is reported for leaf notifications, preference broadcasts, the
    cascade when an upstream calculation that a requested result read as "not
    requested" is requested, published or restored, and a registry change made
    while the application runs that alters what a name the result looked up
    resolves to, or, for a result with a stored copy, that removes a
    passed-over candidate through which alone its lookups reached something
    (section 5; a registration, or a removal
    with `CalculationRegistry::Removal::Change`; `unregister()` has no
    default, so every caller states which removal it makes);
  - neither is reported for `restoreResult()`'s own install, `clear()`, a
    removal with `Removal::Teardown` (an owner being destroyed at shutdown),
    or the destruction of the registry or the engine;
  - events are delivered in order at the end of the engine call, never inside
    an evaluation. The listener travels with the engine (a moved `SessionData`
    keeps it);
  - an `Installed` can be followed in the same call by a `Dropped` for the
    same result; `exportResult()` then already returns nothing at the
    `Installed` event.

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
  outstanding ticket does not change a report: "waiting" and "running" are
  the demand layer's notions (section 16.1).

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

## 15. Background jobs: the executor

Sections 12-14 describe the engine's half of running an explicit calculation
in the background. The other half is `JobQueue` (`src/jobqueue.h`), called
**the executor** in this note, and its `JobModel` (`src/jobmodel.h`), in
`flysight_core` next to `SessionModel`: Qt Core and Gui only, no widgets, no
GTSAM. There is one executor per application. It owns the application's only
worker thread, which runs below normal priority (15.5).

**It is not a queue.** It holds at most the running job and the one job its
caller has chosen to run next. It keeps no order of arrival, no list to
deduplicate against, and prunes nothing. Which job runs next needs the plots,
the columns, the focus and the row order, which the executor must not know:
the demand layer (section 16) decides, and is the only product caller of
`offer()` and `withdrawChosenNext()`. The executor names no caller.

**Reads never start jobs.** A job is created by `JobQueue::offer()` and by
nothing else; the executor never offers on its own. Nor does product code
run explicit work any other way: the engine's synchronous `request()` (section
8) is for tests, and the cleanup audit (group `gestures`) keeps every call of
it out of `src/` outside `src/engine`. Python plugins are ordinary on-demand
readers and start nothing either
(`tst_python_bridge::pluginsNeverStartExplicitWork`). Loading a session starts
none either: a stored result is restored, not requested (15.8). A job that
ends cancelled, superseded or failed leaves its result missing, and the demand
layer offers it again while it is in demand - except a job-level failure,
which it remembers for the rest of the run (16.7).

### 15.1 What a job is

A job is one explicit calculation for one session: `(sessionId, instanceId)`,
where the instance id already contains the registration id (`"<family>#<key>"`
for a family instance). A job stores the session **id** only - never a row, a
`SessionData *`, or an engine pointer. Each time the executor needs the session it
looks it up with `SessionModel::loadedSession()` under a `RowStabilityGuard` and
releases the guard before it emits anything.

**The executor never loads a session.** It never calls `sessionRef()`. An
offer for a session that has no row or is not loaded is refused
(`SessionNotLoaded`), and a chosen next job whose session stops being loaded
ends Superseded at once rather than waiting for something nobody promised.

### 15.2 Lifecycle

```
offer() --> Queued (the chosen next job) --> Running --> Succeeded | Cancelled | Superseded | Failed
               |
               +--> Cancelled | Superseded          (ended before it ever ran)
```

A chosen next job ends Cancelled when another offer replaces it, when it is
withdrawn, cancelled or shut down, and Superseded when it goes stale at start
or its session goes. Every job ends in exactly one end state, enforced in one
place (`JobModel::markFinished`).

`offer()` checks, in this order: shut down (`ShuttingDown`); equal to the
chosen next job, or to the running job that was not asked to stop
(`AlreadyActive`, with its id; nothing changes, and an offer equal to the
running job leaves the chosen next job alone); session not loaded
(`SessionNotLoaded`); then `CalculationEngine::readiness()`: `Unknown` ->
`UnknownCalculation`, `MissingInput` -> `MissingInput` (no job can be created
for a session without the inputs), `Blocked` -> `Blocked` (offer the blockers
instead: chaining is the caller's), `Done` -> `NothingToDo` (already computed,
a cached rejection or failure included, a restored result included, or not an
explicit calculation), `Ready` -> a new Queued job (`Created`). A refusal
changes nothing, the current chosen next job included. It never prepares and
never starts anything synchronously.

**Replacement.** A `Ready` offer whose `(sessionId, instanceId)` differs from
the chosen next job replaces it: the old one ends Cancelled ("No longer
needed"; it never started, and its pin is released), with no `idle()` between
the old job and the new one, and the executor validates the offer again after
that end's signals (a slot may shut it down or offer). The new job is Queued,
pinned, announced (`jobQueued`, `jobsChanged`) and started from the event loop,
never synchronously. **Withdrawal.** `withdrawChosenNext()` ends the chosen
next job Cancelled ("No longer needed"); `idle()` follows when nothing runs.

**A running job asked to stop does not count.** A running job that has been
asked to stop - cancelled, or stopped because its inputs went stale (section
15.3) - is winding down, so an offer of the same `(sessionId, instanceId)`
becomes the chosen next job, and it starts only after the old worker has been
joined, with the new inputs. `activeJob()` applies the same rule.

**Inputs are captured when a job starts**, not when it is offered: a job
chosen behind a five-minute fit sees the session as it is five minutes later.
Jobs run one at a time (`JobQueue::kMaxRunningJobs`, 1; the implementation has
one run slot and asserts it); the chosen next job starts when nothing runs,
always from the event loop. The running slot is freed only after the worker
thread has been joined, which is what guarantees that the next job cannot
start before the running one has ended.

At start (`prepare()`); no compute ran and `startedAt` stays invalid:

| `PrepareOutcome::Kind` | End state | Reason |
|---|---|---|
| `NotFound` | Superseded | "Calculation is no longer registered" |
| `NotExplicit` | Superseded | "Calculation is no longer requested explicitly" |
| `AlreadyValid` | Superseded | "Result is already available" |
| `NothingToRun` | Superseded | "Inputs changed: nothing to compute" (`invalidated` is published) |
| `Blocked` | Superseded | "Inputs changed: waiting for %1", the blockers' titles (`invalidated` is published) |
| `Ready` | runs | |

A job that ends at start ends that attempt: the next chosen next job (one a
slot of the end offered) is tried at the next turn of the event loop, never in
the same call. Should an offer's readiness and `prepare()` ever disagree, that
costs one attempt per turn at most, never a loop.

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
can, so the executor asks it (`loadedSession()`) at the moment it maps the
refusal: a loaded row with the job's session id means "Session data replaced",
anything else - no row, a stub, a model that is being destroyed - "Session
removed or unloaded". A worker thread that
cannot be started ends the job Failed ("The worker thread could not be
started") with nothing cached. "Succeeded" means *this job published a
result*; `AlreadyValid` and `AlreadyPublished` are therefore Superseded.

**Order of a job's end**, whatever the path: (1) the model's end transition;
(2) for a publication, `SessionModel::publishCalculationInvalidation()` with
the engine's `invalidated` set, so a `dependencyChanged` listener that looks at
the job model already sees the job finished; while it runs, `publishingJob()`
returns the job's id, so a listener can tell a publication's
`dependencyChanged` from an edit's; (3) `jobFinished`, `jobsChanged`; (4) the
session is unpinned; (5) the model trims its finished rows; (6) `idle()`, or
the chosen next job, which a slot may have offered during (3), is scheduled. A
chosen next job that an offer replaces skips (6). The demand layer runs its
pass synchronously in its `jobFinished` slot, so the next link of a chain is
chosen before step (6) and the executor reports no `idle()` between links
(16.4). Slots connected to the executor's signals may call `offer()`,
`withdrawChosenNext()`, `cancel()` and `shutdown()`.

### 15.3 Cancellation, abandonment, shutdown

- `cancel(id)` on the chosen next job ends it Cancelled ("Cancelled") at
  once; its record stays as a finished entry. On the running job it requests
  cancellation; the job stays Running (`cancelRequested`) until the compute
  function returns, and the next job does not start before then.
  `cancel(JobId)` is kept for a later jobs dock; no product code calls it
  (audit group `gestures`, "no product code cancels a job"). A job cancelled
  from outside while its pair is still in demand is offered again at once by
  the demand layer; a jobs dock that cancels will need its own policy (for
  example remembering a user cancel like a job-level failure).
- **Cancel wins over a late result.** Once cancellation was requested the job
  ends Cancelled and publishes nothing, even if the compute function returned a
  complete result. The outcome does not depend on a race the user cannot see.
- **A running job whose ticket is certain to be refused is stopped early.** An
  input edit, a merge, a removed session, a removed registration or a cleared
  cache makes the engine mark the ticket at once (section 12), but the worker
  cannot see that and would compute to the end - minutes, for a fit - only to
  be refused. So the executor asks the ticket (`willBeRefused()`, main thread) on
  every main-thread signal that follows such a change:
  `SessionModel::dependencyChanged`, `modelChanged`, `dataChanged`,
  `modelReset`, `rowsRemoved` and `destroyed`, and the registry's observer
  call. There is no timer and no polling. When the answer is yes it requests
  cancellation through the same flag and records the end `publish()` would
  have reported: **Superseded**, with the refusal's reason text ("Inputs
  changed", "Session removed or unloaded", "Session data replaced",
  "Calculation is no longer registered"). `jobCancelRequested` is emitted and the job is, from that
  moment, a running job that was asked to stop: `activeJob()` does not return
  it, a new offer for the same calculation becomes the chosen next job and
  runs after the old worker has been joined, with the new inputs. Nothing is
  published, nothing is cached, and the executor still offers nothing by
  itself. The engine decides; the executor only stops waiting for a verdict it
  already has.
  A compute function that ignores the request, and a staleness none of those
  signals announces (an engine cleared behind the model's back), end
  Superseded at publish exactly as before.
- **The pending end is decided once: the first writer wins.** A job the user
  cancelled ends Cancelled even if its inputs change afterwards; a job stopped
  for a stale ticket ends Superseded ("Inputs changed") even if the user
  cancels it afterwards (`cancel()` still returns true). `shutdown()` follows
  the same rule.
- **Nothing is pruned.** The chosen next job is replaced by a different offer
  or withdrawn by its caller (15.2), both ending it Cancelled ("No longer
  needed"). The running job is never ended because its caller no longer wants
  it: it finishes, and its result is published and stored.
- When the session model removes rows or resets, the chosen next job, if its
  session is no longer loaded, ends Superseded ("Session removed or
  unloaded"), and the running job of such a session is abandoned: cancellation
  is requested so that a long fit for a deleted session does not hold the
  worker, and the job ends Superseded, which is what happened.
- `shutdown()` refuses later offers (`ShuttingDown`), ends the chosen next job
  Cancelled ("Application closing"), requests cancellation of the running
  job, and **waits for the worker without a timeout**. Quitting must neither hang nor crash, and
  abandoning a live thread inside a solver and letting teardown
  proceed is a crash. The bound "one solver step" is delivered by the compute
  function's cancellation boundaries; the executor adds nothing on top: the wait
  ends as soon as `compute()` returns, whatever it returns. Idempotent; called
  by the destructor. The application calls it before tearing anything else down.
- Teardown order is not load-bearing for memory safety: the executor holds the
  session model weakly, and an engine that dies nulls its tickets (section 12).

### 15.4 Pinned sessions

A session with an active job should not be unloaded from under it. Every job
pins its session from creation to its end (`SessionModel::pinSession()` /
`unpinSession()`, counted per session id); a job is the running or the chosen
next job. The demand layer additionally pins each session it loads for column
demand while that session has column demand left (16.8); pins are counted per
session id, so both kinds coexist. A pinned loaded row is passed over
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
ends. At most one exists at a time (`JobQueue::kMaxRunningJobs`). Its `run()`
is one statement: `result = ticket->compute(&progress)`. The ticket, the
facility and the result slot are handed over before `start()` and read back only after `wait()`; those
two calls are the happens-before edges, so there is no lock.

**Priority.** The worker is started with `QThread::LowPriority`, so that the
interface stays responsive and the machine usable while a whole-logbook column
fills in. Windows and macOS lower the thread's scheduling priority. On Linux
under the default `SCHED_OTHER` policy Qt applies no change
(`QThread::priority()` still reports `LowPriority`, which is what
`tst_jobqueue::workerRunsBelowNormalPriority` checks): there the guarantee is
only that the main thread never waits for the worker
(`tst_jobqueue::mainThreadIsNotBlockedByARunningJob`). A calculation that
spreads its work over helper threads must not undo this. The fusion kernel's
solver spreads it (GTSAM runs its elimination on oneTBB's worker threads, which
oneTBB creates at normal priority), so each fit runs in its own oneTBB arena
whose helper threads take the priority of the thread that runs the fit while
they help it (`Fusion::runWithSolverThreadsAtCallerPriority`,
`src/fusion/solverthreads.h`; `tst_fusion_jobs::solverThreadsRunAtWorkerPriority`).
Without it the normal-priority helpers starve the below-normal worker they
wait for (a priority inversion) and the fit stalls under load. Where the
platform ignores the priority (Linux, as above) this changes nothing.

Exactly two things cross threads while a job runs:

- **Progress text.** `CalculationProgress::report()` posts a queued invocation
  to the executor with the text copied by value (Qt's thread-safe event queue,
  not shared state). Posts that arrive for a job that has ended are ignored;
  posts pending when the executor dies are dropped by Qt.
- **The cancel request.** One `std::atomic<bool>` inside the facility, written
  by the main thread, read by `isCancelled()` on the worker. It is the
  progress-and-cancel facility itself, not a lock, and it is the only atomic in
  the executor.

The executor never pauses, wakes, or registers with `IdleScheduler`: saves,
loads, bulk edits and column work continue during a job. (The demand layer's
hidden loads are an idle-scheduler task of its own, 16.8.)

### 15.6 The model

`JobModel` is the store of the `JobRecord`s, not a copy: the executor keeps no job
list of its own, so the model is the single source of truth about work in
progress and a jobs dock can be a pure view of it.

- One row per job in offer order (ascending `JobId`; ids start at 1 and are
  never reused). Rows are appended; a row index changes only when earlier rows
  are removed. Remember jobs by `JobIdRole`.
- Columns (`Qt::DisplayRole`): `SessionColumn` (name), `CalculationColumn`
  (title), `StateColumn` (`stateText()`), `ProgressColumn`, `QueuedColumn`,
  `StartedColumn`, `FinishedColumn` (local short-format text, empty when
  invalid), `ReasonColumn`.
- Roles, answered on every column: `JobIdRole`, `SessionIdRole`,
  `SessionNameRole` (a snapshot at the offer: `_DESCRIPTION`, else the id),
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
- A chosen next job that is replaced or withdrawn stays in the history as a
  Cancelled row with the reason "No longer needed"; while demand changes
  quickly such rows accumulate like any finished row, and the retention bound
  below trims them. The job history is the one the jobs dock will read; the
  demand layer does not change it.
- Finished rows are kept for the life of the application up to
  `finishedLimit()` (default 200, `setFinishedLimit()`); the oldest finished
  rows are trimmed after a job's end transition has been signalled; active rows
  never. No job is persisted: the job history lives as long as the
  application. (The result a job published may be: 15.8.)

### 15.7 API and threading rules

Everything below is **main thread only**, signals included. `offer()`,
`withdrawChosenNext()`, `cancel()` and `shutdown()` must additionally not be
called from inside a calculation or an engine callback (they inspect or
publish to engines).

| Member | Meaning |
|---|---|
| `JobQueue(SessionModel *, QObject *parent)` / `~JobQueue()` | holds the session model weakly; the destructor calls `shutdown()` |
| `JobQueue::kWorkerStackSize` | 64 MiB |
| `model()` | the `JobModel` (a child of the executor) |
| `JobQueue::kMaxRunningJobs` | 1: jobs that may run at once; the demand layer's load bound follows it (16.8) |
| `offer(sessionId, CalculationBlocker)` / `offer(sessionId, plainCalculationId)` -> `OfferResult {kind, job, created()}` | 15.2; `job` is non-zero for `Created` and `AlreadyActive` |
| `withdrawChosenNext()` | false when there is no chosen next job; else ends it Cancelled "No longer needed" |
| `chosenNextJob()` | the one Queued job, 0 if none |
| `publishingJob()` | the job whose publication is being delivered (15.2, step 2), 0 at every other moment |
| `activeJob(sessionId, instanceId)` | the chosen next or running job an offer would be equal to, 0 if none; never a running job that was asked to stop |
| `activeJobs()` | the running job (also one winding down), then the chosen next job: at most two ids |
| `runningJob()`, `job(id)`, `isIdle()`, `isShutDown()` | queries; `isIdle()` is "nothing runs and no chosen next job"; `job()` returns a default record (id 0) for an unknown or removed job |
| `cancel(id)` | false: unknown or already finished. Kept for a jobs dock; no product caller |
| `shutdown()` | 15.3 |
| `failNextWorkerStarts(n)` | test seam: thread-creation failure cannot be provoked portably |
| signals `jobQueued(id)`, `jobStarted(id)`, `jobProgress(id, text)`, `jobCancelRequested(id)`, `jobFinished(id, state)`, `jobsChanged()` (after each of the others except `jobProgress`), `idle()` (the last active job ended) | `jobQueued` follows the model's `rowsInserted`. A job that a `rowsInserted` slot ended (cancel, shutdown) is not announced as queued afterwards: `jobFinished` was its only signal, and `offer()` still returns `Created` |
| `JobModel(QObject *parent)`; `rowCount`, `columnCount`, `data`, `headerData`, `flags`, `roleNames`, `removeRows`; `rowOf(id)`, `record(row)`, `record(id)`, `records()`, `stateText(state)`, `removeFinished(id)`, `clearFinished()`, `finishedLimit()`, `setFinishedLimit(n)`, `kDefaultFinishedLimit` | 15.6. Only `JobQueue` appends rows and changes job state: it is a friend for the private mutators only, and reads through `records()` like any view. `records()` is a reference to the rows, valid until the model next changes |
| `JobId`, `JobState`, `JobRecord` (`isFinished()`, `isActive()`) | the vocabulary, `src/jobmodel.h` |
| `SessionModel::pinSession(id)`, `unpinSession(id)`, `isSessionPinned(id)` | 15.4. `unpinSession()` never evicts synchronously, so it is safe in a slot |
| `SessionModel::publishCalculationInvalidation(id, keys)` | 15.4. Not while a `RowStabilityGuard` is held (it emits) |

Tests: `tests/tst_jobqueue.cpp` (the executor), `tests/tst_jobmodel.cpp`, and
the controllable calculations of `tests/support/jobfixture.h` (`waitStarted`,
`waitIdle`, `Quiet`).

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
  listener, so both install paths write: the executor's publish and a
  synchronous `request()`.
- `Installed` with status `Ok`: `exportResult()`, then
  `CalculationRecord::stamped()` (which adds `CalculationCompatibilityVersion`),
  then
  `LogbookManager::writeCalculationRecord()`, on the main thread. The write is
  atomic (`QSaveFile`). A failure is warned once, leaves the previous record
  and the in-memory result untouched, and is not retried before the next `Ok`
  publish.
- `Installed` with any other status writes nothing and deletes nothing.
- `Dropped` (an input change, or a registry change made while the
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
- Records are restored upstream first, in passes. A record whose resolutions
  name another pending record of the session as a `Calculation` provider (the
  requested result it read) waits until that one is restored or dropped, so
  its lookups resolve as they did when it was stored: restored before its
  upstream, a looked-up name with a second candidate would resolve to that
  candidate and the record would be refused as `Resolutions`.
  Explicit-on-explicit chains therefore restore in any file order. A record
  that stays `InputsUnavailable` although it waits for nothing is retried
  after a pass that changed something else and counts as stale only then.
- These are deleted at a load (or at the column worker's restore, below): a
  file that is not a record, a damaged record,
  a record of another format version (such as format 1 from development
  builds: no migration), a record whose stamp is not current
  (`CalculationRecord::stampsAreCurrent()`: the compatibility marker only), a
  record failing a stale check (`Resolutions` included), and a record of a
  calculation that is not registered as explicit. A record whose result is
  already installed (`AlreadyInstalled`) is kept.
- A record that exists but cannot be opened or read in full
  (`CalculationRecordStatus::Unreadable`: a lock, a permission, a short read,
  a directory at its path) is skipped: neither restored nor deleted, counted
  in `recordsSkipped`, warned once ("skipped (kept for the next load)"), and
  marked with `LogbookManager::markCalculationRecordSkipped()`, which keeps
  the column values over it out of `index.json` until the record is written
  or removed or the row is evicted. A record whose resolutions name a skipped
  record is skipped too, before it is tried, whatever its own checks would
  say. The next load tries again.
- The column worker's temporary copy of an unloaded session
  (`SessionModel::restoreForColumnWorker()`, once per worker step) is restored
  through the same `restoreSession()` path and checks when a missing logbook
  column of the row depends on a calculation the manager knows a record of
  (section 17); otherwise it reads no record. The copy counts as a load for
  reading, never for writing: it has no explicit-result listener (a restore
  reports no install anyway), so nothing it does writes, rewrites or deletes
  a record beyond the restore's own stale deletions; it is never installed
  into the row, emits no `sessionLoaded`, and nothing is requested, prepared
  or run in it. A record it skips is marked like one skipped at a load, and
  the mark is discarded with the copy
  (`LogbookManager::discardUnconfirmedCalculationRecords()`, as at eviction).
- The bulk edit's temporary load never reads a record.
- Records are deleted with their session (`LogbookManager::removeSession`) and,
  as strays whose session file does not exist in `sessions/`, at
  `initialize()`; that pass deletes in `cache/` only. Eviction, unloading, a
  registry change that does not alter a requested result's lookups (section
  5: a candidate behind the session's data or behind the provider, a name
  never looked up), a removal as teardown and the model's destruction never
  delete one. A registry change made while the application runs that drops a
  requested result deletes its record like an input change. `AltitudeMarkerManager`'s destructor removes
  its registrations as teardown; the plugin host never unregisters.
- `cache/` may be deleted while the application is closed: `initialize()` then
  knows no record, every explicit calculation reads as not requested, what is
  switched on computes it again (section 16), and the
  start-up stamp check (DATA_SCHEMA section 11) drops each cached column value
  over a vanished record.
- A session created by an import has its file stem reserved
  (`LogbookManager::reserveSessionFile`), so a result published before its
  first save is stored under the name that save will use. A session that is
  never saved leaves a stray, removed at the next start.
- After a restore `readiness()` is `Done`, so `JobQueue::offer()` answers
  `NothingToDo` and the demand layer counts the pair as done.
- Record format and file names: `src/calculationrecord.h` and DATA_SCHEMA
  section 12. Test seam: `SessionModel::storedResultStats()` /
  `resetStoredResultStats()` (records written, restore calls and listings,
  records read, restored, kept, skipped and deleted, and the time spent).

Tests: `tests/tst_calcengine_restore.cpp`, `tests/tst_result_records.cpp`,
`tests/tst_result_store.cpp`, `tests/tst_result_columns.cpp`,
`tests/tst_fusion_store.cpp`, `tests/tst_plugin_identity.cpp` (the plug-in
code identity).

## 16. The demand layer: what is computed, and when

Users think in plots and logbook columns, not calculations: a checked plot
means "show this for the visible sessions", an enabled column means "this
value for every session in the logbook". What is switched on is the request;
there is no refresh and no cancel.

`CalculationDemand` (`src/calculationdemand.h`), the **demand layer**, is
widget-free and lives in `flysight_core` (Qt Core and Gui only) next to
`SessionModel`, `PlotModel`, `LogbookColumnStore` and the executor. It derives
**demand** - the pairs (session, requested calculation) that something
switched on needs and that have no result - and keeps the executor's chosen
next job equal to its first choice. It is the **only product caller of
`JobQueue::offer()` and `withdrawChosenNext()`**. Nothing calls into it but the
views' read-only queries; it observes the models, the logbook's record
changes, the registry and the executor's signals. Everything it decides is
tested without widgets (`tests/tst_calculation_demand.cpp`).

The principles it implements:

- intent is expressed through plots and logbook columns, never through
  calculations;
- anything needed is wanted at once, and anything no longer needed is dropped;
- finished work is never wasted: results are stored (15.8);
- background work must not degrade the rest of the application (15.5, 16.8);
- a failure is shown, never retried in a loop (16.7);
- each component's contract can be stated without naming the others (16.11).

### 16.1 Demand and track conditions

**Plot demand.** For every checked plot whose y name is requested (16.3),
every **visible, loaded track** - a session-model row with `isLoaded() &&
visible && !loadFailed`, exactly the rows the plot widget draws - needs the
requested calculations that block that name for that session.

**Column demand.** For every enabled logbook column whose value depends on a
requested output (16.3), every **session row of the logbook**, loaded or not,
needs the requested calculations that block that value.

**Where a result is looked up** - two sources only:

- A loaded session (visible or in the hidden pool): blocker inspection
  (section 13) of the plot's y name, or of the column's names (one, or two for
  a `Delta` column, combined: any `NotApplicable` -> `NotApplicable`, else any
  `NotProduced` -> `NotProduced`, else any `Blocked` -> `Blocked` with the
  union of the blockers, else `Available`).
- A session that is not loaded (a stub, or a failed-load placeholder, whose
  engine holds no stored result): the logbook's known record names
  (`LogbookManager::knownCalculationRecords()`) and the reasons the index
  recorded for them (`LogbookManager::calculationRecordReason()`, 16.7); no
  record is opened. A cell has a result only when every storable calculation
  it needs has a record. A known record counts as a result until something
  restores it and finds it stale: the column worker does so when it fills the
  session's column values from a temporary copy, deletes the stale record,
  and that record change moves the pair into demand. The demand layer performs
  no staleness check of its own. Explicit family instances are never stored,
  so they create no column demand for a session that is not loaded.

**Conditions** (`DemandCondition`) of a track of a loaded session:

| Report | Condition |
|---|---|
| `Available` | `Done` |
| `NotApplicable` | `NotApplicable`: silently absent, as plots treat missing data |
| `NotProduced` | `Failed`: an input-determined failure, with a reason built from the notes |
| `Blocked`, a blocker is the running job and it was not asked to stop | `Running` |
| `Blocked`, otherwise a blocker is remembered as a job-level failure (16.7) | `Failed` (job-level) |
| `Blocked`, otherwise every blocker is remembered as not applicable | `NotApplicable` |
| `Blocked`, otherwise | `Waiting` (with `settling` while the session's inputs settle, 16.5) |

`Blocked` wins over `NotProduced`, as in section 13. There is no "stale"
condition: a result invalidated by an input change reports `Blocked` again. A
cell of a session that is not loaded is decided in this order: no storable
calculation -> `NotApplicable`; a settlement of this run (16.7) -> its
verdict; every record known -> `Done`, or `Failed` with the reason the index
recorded for one of them; a remembered job-level failure -> `Failed`; every
calculation remembered not applicable -> `NotApplicable`; a failed-load
placeholder -> `Failed` ("The session file could not be loaded", settled);
otherwise `Waiting`.

- A session without the calculation's inputs is `NotApplicable`: it is never
  waiting, running or failed, never counted, and never offered.
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

### 16.2 Plot and column state

`plotState(plotId)` and `columnState(columnId)` return a `DemandState`, a plain
value. `plotId` is `"<sensorID>/<measurementID>"`, equal to
`PlotModel::PlotValueIdRole`; `columnId` is
`logbookColumnDefinitionKey(column)`. The default value ("plain") is returned
for a plot that is unchecked, not requested or unknown, and for a column that
is not enabled, not requested or unknown.

| Field | Meaning |
|---|---|
| `sourceId` | the plot or column id |
| `requested` | the source depends on a requested calculation; false: never inspected |
| `wantedCount` | tracks that are not `NotApplicable` |
| `doneCount` | `Done` plus `Failed`: nothing is left to compute for them |
| `waitingCount`, `runningCount` | `runningCount` is 0 or 1 |
| `failedCount` | input-determined and job-level failures |
| `running`, `waiting`, `failed` | lists of `DemandTrack` in row order; a column state counts its waiting tracks without listing them |
| `progressLabel` | "<done> of <wanted>" while working, else empty: a failed track counts as done |
| `toolTip` | `buildToolTip(*this)`; empty for a plain state |

`isWorking()` is "anything waiting or running"; `showsWarning()` is "not
working and something failed" (the badge replaces the indicator); `isPlain()`
is neither. There is no "episode": the counts are the truth of the moment.

`DemandTrack`: `sessionId`; `sessionName` (the name the logbook shows: the live
`_DESCRIPTION` of a loaded session; for a session that is not loaded or failed
to load, the description cached in `index.json`; the session id only when no
description exists - `tst_calculation_demand::visibleFailedLoadIsSettledAsFailed`
asserts the name); `condition`; `calculationTitles`; `reason` (failed);
`jobFailure` (failed: a job-level failure, not stored); `settling` (waiting);
`job`; `progressText` (running).

The tooltip, built by the pure `buildToolTip(state)`, is a block of plain
text; each part is omitted when empty:

```
Computing: <done> of <wanted> done
  <session name> - <titles>: <progress text | running>
Could not be computed:
  <session name> - <reason>
  and <n> more
```

Each list shows at most `kToolTipListLimit` (10) sessions and ends with "and
<n> more" when there are more; the state's own lists stay complete. No message
box reports a calculation outcome.

`isCellPending(sessionId, columnId)` and `isCellPending(row, column)` are true
exactly for a `Waiting` or `Running` cell of a requested column; the row is
mapped to its session on every call. `workingPlotIds()` and
`workingColumnIds()` return the ids whose state is working, which is what the
views' animation clocks follow.

Signals: `plotStateChanged(plotId)` and `columnStateChanged(columnId)` (the
state, or for a column its set of pending cells, changed), per id, then
`statesChanged()` once per pass; a state is stored before it is announced. The
running job's progress text updates the states without a pass and without
inspection. A state may be one event-loop pass behind the models: a consumer
that must not be (the plot widget's "no data" warning) asks the engine
(16.10).

### 16.3 Requested plots and columns, and what inspection costs

A plot is **requested** (explicit-backed) when any name in the static
dependency closure of its y name - `CalculationRegistry::staticDependencies(name).names`,
which includes the name itself and looks through source conversions - has a
candidate or a source conversion with explicit policy. One registry query
answers it, `CalculationRegistry::dependsOnExplicit(name)`. The logbook column
cache asks `CalculationRegistry::explicitDependencies(name)` (which explicit
calculations; section 17), of which `dependsOnExplicit()` is the
non-emptiness, so rows and columns cannot disagree. It is a pure function of
the registrations, memoized in the registry and per plot id, and dropped by a
registry observer. It is exact: `staticDependencies()` is a superset of every
dynamic dependency set and a blocker is always reached through declared
inputs, so a plot that is not requested can never report a blocker. A column
is requested when `logbookColumnExplicitCalculations()` is not empty (the same
registry answer the column cache uses; section 17). The demand layer never
tests `EvaluationPolicy::Explicit` itself.

**What a pass costs.** Plots that are not requested are never inspected: no
`blockers()` call, no read, no signal, one hash lookup. Blocker inspection runs
only for (checked, requested plots) x (visible, loaded tracks) - names the plot
widget reads for the same tracks anyway - and for (enabled, requested columns)
x (loaded rows whose report memo was dropped: by an input change or a
publication, a load, a record change, a job's end or a single-row display
change), through `SessionModel::loadedSession()` under one
`RowStabilityGuard`: nothing is loaded, evicted or touched in the LRU inside a
pass, and the guard is released before anything is offered, withdrawn, pinned
or emitted. Plot classifications are not cached across passes (after A
publishes, the blocker of B's output changes from A to B although B's output
may not be re-announced); the engine's caches make a repeated `blockers()`
cheap. A session that is not loaded costs one `knownCalculationRecords()` call
between changes to its records (a memo). With requested columns enabled a pass
is O(rows) plus O(rows x requested columns) hash lookups; with none, the rows
are not walked at all.

**When a pass runs.** Passes are coalesced to one per event-loop pass (a
zero-interval timer). A pass runs at once, synchronously, in the executor's
`jobFinished` slot (16.4), before a load step of the column fill when a pass
is pending, and when a plot is unchecked or a session hidden while a chosen
next job exists (so that the waiting pair is withdrawn before it can start).
Otherwise a pass is scheduled by:

| Source | Signal |
|---|---|
| `PlotModel` | a check-state `dataChanged`, `modelReset` |
| `SessionModel` | `visibilityChanged`, `sessionLoaded`, `modelChanged`, `focusedSessionChanged`, `modelReset` (which a column change causes), `dependencyChanged` of a relevant name (16.5), and a single-row display `dataChanged` of a session that is not loaded when it changed what the demand layer knows of it (a bulk edit, the column worker: they emit no `dependencyChanged`) |
| `LogbookManager` | `calculationRecordsChanged` |
| the executor | `jobStarted`, `jobCancelRequested`; `jobFinished` runs the pass at once; `jobProgress` updates texts only |
| the registry | its observer call |
| the demand layer | the end of a settle wait (16.5); a load of the column fill (16.8); an offer refused as not applicable (16.7) |

A session-model reset reaches the executor first, which may end a chosen next
job whose session is gone; its `jobFinished` runs a pass at once. So the
demand layer drops its column set and its memos on `modelAboutToBeReset`,
before any of that, and the pass reads the new rows and columns.

### 16.4 Work follows demand

**No gesture.** A pair that enters demand is wanted at once, and the indicator
shows it immediately. What creates demand:

- checking a plot in any way - a click on the check box, Space, the Plots menu,
  applying a profile, the start-up restore: the check state in `PlotModel` is
  the only thing read;
- showing a session while a plot is checked;
- a visible session finishing its load;
- enabling a column (the column editor, a profile: `LogbookColumnStore`);
- an input change that drops a demanded result (after the settle wait, 16.5);
- a record the column worker deletes as stale;
- a registry change that makes a plot or a column requested.

**Start-up.** Every session starts hidden, so restored checked plots create no
demand until a session is shown; enabled columns create demand at once, and
their hidden loads wait behind the start-up work by scheduler priority (16.8).
Nothing about demand is persisted.

**Profiles.** Applying a profile that carries a column over a requested output
computes that calculation for every session of the logbook that lacks a
result. That is intended, and it is why no default profile carries such a
column; the cleanup audit (group `demand`) checks the profiles shipped in
`src/resources/profiles/`.

**What drops demand:** unchecking the plot, hiding the session, disabling the
column, or the result appearing by other means. A waiting pair that leaves
demand is withdrawn before it starts (the executor ends it Cancelled "No
longer needed"). There is no queue of accepted requests to prune: demand is
the only list of waiting work.

**The running job is never stopped by the demand layer.** It finishes, and its
result is published and stored, even when its pair left demand. It is stopped
only as 15.3 says: its inputs changed, its session went away, or the
application closes.

**Offering.** Each pass walks the candidates in priority order (16.6); a
candidate that is already the chosen next job is kept as it is, and otherwise
the first candidate the executor accepts becomes the chosen next job:

| Executor's answer | Reaction |
|---|---|
| `Created` | the chosen next job (a different one was replaced); done |
| `MissingInput`, `NothingToDo`, `UnknownCalculation` | remembered as not applicable (16.7); next candidate |
| `Blocked`, `SessionNotLoaded`, `AlreadyActive` | next candidate; nothing remembered |
| `ShuttingDown` | stop |

When no candidate is accepted, the demand layer withdraws the chosen next job
if it offered it itself; a chosen next job it did not offer is replaced when
it has a choice and otherwise left alone (the executor's own tests drive the
executor directly).

**Chained calculations.** When requested calculation B consumes requested A,
demand covers both, upstream first, as blocker inspection orders them. The
pass that runs synchronously in `jobFinished` (before the executor decides
between `idle()` and the next start, 15.2) offers the next link, so the
executor reports no `idle()` between links; the engine state is current there
because the publication's `dependencyChanged` precedes `jobFinished`. A hold
(16.8) lasts across the links.

**After shutdown** nothing is offered or loaded, and the states stay working
until the demand layer is destroyed.

### 16.5 The input-settle wait

`CalculationDemand::kInputSettleMs` (1000 ms). A `dependencyChanged` of a
relevant name (the static closure of every checked requested plot and every
enabled requested column) on a session is an input change, unless it is the
delivery of a publication (`JobQueue::publishingJob()` is a job of that
session). An input change forgets the session's remembered failures,
not-applicable verdicts and settlements, and starts or restarts its wait.

- While the wait runs, the session's pairs are in demand and counted
  `Waiting` (with `settling`), so the indicator shows from the first change,
  but they are not offered; a burst of edits runs one job. The executor
  already stops a running job whose inputs changed (15.3); the wait decides
  only when the replacement starts.
- An irrelevant edit (a description) starts no wait and clears nothing.
  Showing, hiding, checking, enabling, profiles and loading take effect
  without a wait.

### 16.6 Priority

At every pass, and so when a job ends and whenever demand changes, the
candidates are, in order:

1. plot demand of the focused session (if it is one of the tracks);
2. plot demand of the other visible sessions, in logbook row order;
3. column demand of every loaded session: the visible ones in logbook row
   order, then the hidden ones (the pool, and the sessions the fill has
   loaded) in logbook row order, so that what the user is looking at is
   computed before what only the logbook shows.

Within a session: plot-model order, then column order, then the blockers'
order (upstream first); each (session, instance) once, in its first tier. A
pair is not a candidate while it is remembered (16.7), while it is the running
job not asked to stop, or while its session settles. A session that is not
loaded is never offered: it enters tier 3 once the fill has loaded it (16.8),
in row order.

The choice is made from demand as it is at that moment, not from the order in
which pairs entered it: a different first choice replaces the executor's
chosen next job (Cancelled "No longer needed"). The running job is not
preempted. So a session shown during a column fill is computed next, after the
running job.

### 16.7 Failures, not applicable, settlements

- **Input-determined failures are results.** A rejection or solver failure
  (`NotProduced`) is stored, shown with the warning badge and its reason, and
  never offered again; an input change makes it `Blocked` again. A result the
  engine cached as `Failed` (a compute function that threw) is kept in memory,
  not stored: badged, not offered again in this run, computed again after a
  restart.
- **Job-level failures** (`JobState::Failed`: the worker could not be started,
  or the calculation ran out of memory) are remembered per (session,
  calculation instance) with the reason "<title>: <reason>" (for example
  "Sensor fusion: Out of memory"). The pair is badged (`jobFailure`) and not
  offered again until the session's inputs change or the registry changes. The
  memory is not persisted, so the next start of the application tries again.
- **Not applicable.** An offer the executor refuses as `MissingInput`,
  `NothingToDo` or `UnknownCalculation` is remembered like a failure, the
  track reads `NotApplicable`, and the memory clears the same way. The refusal
  schedules a pass: column cells are classified before the offers, so the
  next pass is the one that reads the memory. A remembered pair is not offered
  again, so this never repeats.
- **Memory and resets.** A session-model reset (a sort resets the model)
  forgets only the entries of sessions that no longer have a row; a registry
  change forgets everything.
- **Settlements** (columns only): the last final verdict (`Done`, `Failed`,
  `NotApplicable`) of a session's cell, kept per (session, column) for the
  run, so that a session evicted after its verdict is not loaded again to find
  the same answer (a calculation that does not apply, a thrown exception, a
  failed record write, a job-level failure, a failed load). For a session that
  is not loaded a settlement takes precedence over the record names. Cleared
  by the session's input change, by a record change of a calculation of that
  column, by a single-row display change of the session while it is not
  loaded (a bulk edit), by the column leaving the enabled set, by a registry
  change, and for sessions whose row is gone. Multi-row changes (the unit
  system, the environment check) clear nothing.
- **A session whose file cannot be loaded** (a failed-load placeholder, visible
  or hidden) is settled as a job-level failure, "The session file could not be
  loaded": badged, never pending, not retried in this run unless its data
  change; the next start tries again.
- **There is no retry control.** Changing an input is how a failure is
  retried; restarting the application retries job-level failures.
- **Stored failures of sessions that are not loaded.** Records are never
  opened for such a session; the logbook index records each record's reason
  instead (`LogbookManager::calculationRecordReason()`, DATA_SCHEMA section
  11), learned by the result store when the record is written and when it is
  restored. A stored rejection or solver failure of a session that is not
  loaded is therefore a failed result with its reason, listed in the column's
  failure list and badge before and after a restart alike, without a load. A
  record written by an earlier build has no recorded reason until its next
  restore (the column worker's copy or a load) and counts as a success until
  then.

### 16.8 Sessions that are not loaded

The executor needs its session loaded (15.1), so the demand layer, not the
column worker, has the sessions of column demand loaded.

- **The column fill** is an `IdleScheduler` task the demand layer registers
  through `SessionModel::scheduler()` under `SessionModel::ColumnFillTask` (4),
  at priority 5, below saving (1), loading visible sessions (2), bulk edits (3)
  and column work (4); it is unregistered when the demand layer is destroyed.
  It has work while any session has a waiting or running column cell; it can
  step (load) only while a hold is free and an unloaded session waits; its
  progress is the sessions that still have such a cell, of the fill's
  high-water mark, and one last step, which loads nothing, reports the fill
  complete. So the logbook's progress line shows "Computing columns: k / n"
  for the whole fill, from the first load to the last result, under the same
  label as the cheap column pass (to the user a column fills in the same way
  whether its values are cheap or requested). For this the scheduler gained
  the generic notion of a task with work it cannot step right now
  (`TaskDef::canStep`): it is reported as active with its progress, not
  stepped, and the scheduler rests until it is woken instead of spinning. The
  scheduler learns nothing about jobs: the task is one more source of steps.
  It reports the active task once per tick, so a count that lasts less than a
  tick (a job that ends before the fill's next tick) is never shown, and a
  tick that goes to a higher-priority task shows that task's progress instead.
- **A load step** takes the first session in row order that is not loaded,
  not visible (a visible stub belongs to the visible loader), not settling,
  not held and has a waiting cell, runs a pending pass first so that it
  chooses from demand as it is, and calls
  `SessionModel::loadPinnedSession(id)`: the real load of showing it
  (`sessionRef()`: the session-id correction, the engine attached, its stored
  results restored, `sessionLoaded`, the LRU and its eviction pass) without
  making the row visible, pinned under the id the row has after the load. The
  session-id correction therefore always precedes the offer; no pair of a
  session that is not loaded is ever offered. A load that fails settles the
  session (16.7).
- **The bound.** At most `CalculationDemand::kMaxHeldSessions`
  (`JobQueue::kMaxRunningJobs + 1`, so 2: the running job's session and the
  chosen next job's) are held at a time; the next is loaded when one is
  released. Sessions that were already loaded are never held and do not
  count: the executor pins them while their job is chosen or running.
- **Holds.** A hold is the demand layer's own pin, begun when the load returns
  and released when the session has no `Waiting` or `Running` cell left (its
  job ended and the result was published, it was settled, its column was
  disabled, its result appeared by other means), when its row is gone or
  unloaded, when the demand layer is inert, and at its destruction. A hold
  lasts across the links of a chain. The executor's own pin keeps a running
  job's session loaded after a hold is released. Released sessions stay in the
  hidden pool until ordinary LRU eviction; the pool may exceed "Maximum cached
  sessions" by at most the holds, and a capacity of 0 works.
- **Ordering.** The scheduler steps the highest-priority task that has work
  and can step, so a load step never runs while a save, a visible load, a bulk
  edit or column work has work: a stub is never dirty, a bulk edit on a stub
  finishes (edit, save, index) before any load step, and a bulk edit on a held
  session takes the loaded path, so a job over the old value goes stale and is
  offered again. No result is computed from a file that is about to be
  rewritten.
- **No cancel.** The progress line shows no cancel button for the fill: the
  task is not cancellable, because a cancel that left the column enabled would
  be undone on the next tick. Disabling the column is how the work is dropped.
- **The column worker is unchanged.** It settles a column over a requested
  output as unavailable when the session has no record, computes it from its
  temporary copy when there is one, and never knows a job exists. When a job
  writes the record, the record change drops the cached value and the
  loaded-row refresh computes the new one (or the column worker, from the
  same loaded session, when its tick comes before the queued refresh; which
  comes first is up to the event loop). Cheap column values never wait for
  a requested calculation.
- **Edge cases:**
  - a column disabled mid-load: the next pass releases the hold and withdraws
    its pair; a running job finishes and is stored;
  - a held session shown by the user: the hold ends as usual; a visible row is
    not in the LRU and stays loaded;
  - the logbook reopened, or sessions removed: the model reset releases the
    holds;
  - a calculation found not applicable after the load: settled without a job;
    the worker's cached "unavailable" stays;
  - the executor shut down: nothing more is loaded.
- **Known costs.**
  - A session without a record for which the calculation turns out not to
    apply (for sensor fusion, a recording without IMU data) is loaded once
    per run to find that out: "not applicable" is a settlement of the run
    (16.7), not a record, so the next start loads it again. Until the fill
    has loaded it, its cell is `Waiting` ("…"), counted in the column's
    "k of n" and in the fill's progress; then it is not applicable, blank and
    not counted.
  - A column whose value reaches a requested calculation only through one of
    several candidates of an on-demand name would load each session without
    a record once per run to find out. No registered column is like this
    today (every fusion output has one candidate); if one appears, the fix
    belongs to `CalculationRegistry::explicitDependencies()`, not to the
    demand layer.

### 16.9 API and threading rules

Everything is **main thread only**, and no member may be called from inside a
calculation or an engine callback (they inspect engines and offer to the
executor). Create the demand layer after the executor; destroy it before the
executor and before the session model (it registered a scheduler task and
holds pins there). Every collaborator is held weakly; a missing one makes the
component inert: default states, nothing offered or loaded, no session held,
every test seam safe.

| Member | Meaning |
|---|---|
| `CalculationDemand(SessionModel *, PlotModel *, JobQueue *, QObject *parent)` / `~CalculationDemand()` | the destructor removes the registry observer, unregisters the column fill and releases the holds |
| `kInputSettleMs`, `kMaxHeldSessions`, `kToolTipListLimit` | 1000 ms (16.5); `JobQueue::kMaxRunningJobs + 1` (16.8); 10 (16.2) |
| `plotId(sensorId, measurementId)`, `plotId(PlotValue)`, `columnId(column)` | the ids of 16.2 |
| `plotState(plotId)`, `columnState(columnId)` | 16.2; the last computed state, at most one event-loop pass behind the models |
| `isCellPending(sessionId, columnId)`, `isCellPending(row, column)` | 16.2 |
| `workingPlotIds()`, `workingColumnIds()` | 16.2 |
| `buildToolTip(state)` | 16.2; static and pure |
| `isMerelyUncomputed(session, sensorId, measurementId)` | 16.10; static |
| signals `plotStateChanged(plotId)`, `columnStateChanged(columnId)`, `statesChanged()` | 16.2 |
| `flush()`, `hasPendingUpdate()`, `passCount()`, `setInputSettleDelay(ms)`, `inputSettleDelay()`, `endInputSettleWaits()`, `isSettling(id)`, `hasSettlingSessions()`, `heldSessionIds()`, `hasFillWork()`, `canLoad()`, `runLoadStep()`, `recordSetLookups()` | test seams; the views never call them |
| `DemandCondition`, `DemandTrack`, `DemandState` (`isWorking()`, `showsWarning()`, `isPlain()`, `operator==`) | 16.1, 16.2 |
| `SessionModel::loadPinnedSession(id)` | 16.8: returns the id the row has after the load, empty (nothing pinned) when there is no such row or the file cannot be loaded; the caller unpins that id |
| `SessionModel::ColumnFillTask` | 16.8; the task id the demand layer registers |
| `IdleScheduler::registerTask(id, def)`, `unregisterTask(id)`, `TaskDef::canStep` | `registerTask` replaces a task already registered under the id; `unregisterTask` removes one without calling its `onComplete` |

Tests: `tests/tst_calculation_demand.cpp` (plot and column demand, with the
synthetic plots of `tests/support/plotfixture.h` over the calculations of
`jobfixture.h`), `tests/tst_result_columns.cpp` (the column worker unchanged by
demand), `tests/tst_column_cache.cpp` (`loadPinnedSession()`),
`tests/tst_session_model_engine.cpp` (`unregisterTask`, a task that cannot
step), `tests/tst_logbook_index.cpp` and `tests/tst_result_store.cpp` (the
recorded reasons).

### 16.10 The views and application wiring

**Plot rows.** `PlotRowDelegate`
(`src/ui/docks/plotselection/PlotRowDelegate.h`), installed on the plot list
by `PlotSelectionDockFeature`, paints `DemandState` right-aligned in the row:
while working, the working indicator (an open 270-degree arc in the row's text
colour, turning about once a second) and `progressLabel` ("k of n"); once the
work is finished and some sessions failed, the warning badge with
`failedCount`; never both. Plain rows are painted by the unmodified
`QStyledItemDelegate`, pixel for pixel; the name is elided, never the cluster;
the row height never changes. The tooltip is `DemandState::toolTip`, over the
whole row. The geometry is the pure `layoutPlotRow()` (`PlotRowLayout.h`),
which has no hit rectangle. **No gestures:** the delegate handles no event of
its own; checking a row is the base class's write to `PlotModel`, which the
demand layer observes like every other check change.

**The shared glyphs.** `src/ui/docks/DemandIndicator.h` (Qt Core and Gui only)
holds `drawWorkingGlyph()`, `drawWarningGlyph()` and `WorkingAnimation`, one
clock per view, ticking (80 ms per frame, 30 degrees per frame) only while its
view has something working (`workingPlotIds()` / `workingColumnIds()`). It
stops and returns to frame 0 when nothing is, and each frame repaints only the
working rows or sections. The glyphs are drawn with `QPainter` (no bundled
images).

**Logbook column headers.** `LogbookView` takes the demand layer and installs
`LogbookHeaderView` (`src/ui/docks/logbook/LogbookHeaderView.h`): a requested
column's header shows the same indicator or badge immediately right of its
centred text, inside the style's label rect (clear of the sort arrow), each
line of the text elided to make room; the header shows no count. Hovering the
section shows `columnState(id).toolTip`; double-clicking the section's edge
fits text and glyph; clicks sort exactly as before. The section-to-column
mapping is computed per call (sorting and a column change reset the model).

**Pending cells.** `LogbookCellDelegate`
(`src/ui/docks/logbook/LogbookCellDelegate.h`) paints a cell whose pair is in
demand (`isCellPending`) and whose model value is empty as a muted "…"
(U+2026, in the palette's placeholder colour), with the tooltip "Pending: this
value is being computed". A value always wins. The three looks of a cell: a
value; empty (unavailable, or a value over a record that could not be read,
which is not cached); "…" (in demand). Pending is a presentation of demand:
the model, its cached values, `pendingColumns`, `index.json` and
`SessionModel::sort()` never see it; the cached value underneath stays
unavailable until the record is written, so sorting treats a pending cell as
unavailable. A `columnStateChanged` repaints that column's visible rect only.
Cells are not animated.

**The progress line** is unchanged in form; the column fill reports on it
(16.8).

**Ownership and order.** `MainWindow` creates the `JobQueue` and then the
`CalculationDemand` in its constructor, after the session model is populated
and the calculations are registered and before any dock exists, and hands both
to the docks through `AppContext` (`jobQueue`, `calculationDemand`). Restored
plots and a first-launch profile reach the demand layer as ordinary model
changes and every session starts hidden, so start-up starts no job for plots,
while an enabled column over a requested output does start its fill.
`closeEvent()` calls `JobQueue::shutdown()` **first**, before sessions are
flushed and the layout is saved (with a wait cursor when the executor is busy;
the wait is at most one solver step). A future veto of the close must be
decided before that call: an executor that was shut down refuses every later
offer. `~MainWindow()` deletes the demand layer, then the executor,
explicitly and before everything else: `QObject` deletes children in creation
order, which would destroy the session model under the executor. The plot
list's delegate and the logbook's header view and cell delegate hold the
demand layer weakly and turn plain once it is gone. No signal of the executor
or the demand layer is connected to anything that shows a dialog; no message
box, status message, or progress dialog reports a calculation outcome.

**The "no data" warning.** One reader warns when a checked plot has no data for
a visible track: `PlotWidget::updatePlot()`. For the value it just read as
empty it asks `CalculationDemand::isMerelyUncomputed(session, sensor,
measurement)`, a static predicate of the widget-free core
(`src/calculationdemand.h`) over `blockers(y name).state`, and stays silent
when that is true: `Blocked` (not computed yet) and `NotProduced` (ran and
rejected its inputs, or failed); both are shown by the plot list instead. The
predicate asks the engine, not the plot's state, which may be one event-loop
pass behind; it inspects only, so it runs no explicit calculation, creates no
job and loads nothing. A recording that simply lacks the sensor
(`NotApplicable`) still logs the warning. The four states are unit-tested in
`tst_calculation_demand::merelyUncomputedIsNotWorthAWarning`; the widget's one
call is step M1 of the manual script.

**Results appear through ordinary invalidation only.** Publishing a job's
result emits `dependencyChanged` per name, `dataChanged` for the row, and
`modelChanged` (`SessionModel::publishCalculationInvalidation()`, 15.4). The
plot widget and the legend rebuild on `modelChanged`, the logbook repaints from
`dataChanged`, the measure tool reads at interaction time. Nothing connects the
executor or the demand layer to the plot, the legend, or the logbook's values.
A restored result (15.8) needs no publication: it is installed before the
row's `sessionLoaded`, which already makes the plot, the legend and the rows
read the session.

Tests: `tests/tst_plot_row_layout.cpp` (geometry, no widgets),
`tests/tst_plot_row_delegate.cpp` and `tests/tst_logbook_indicators.cpp` (the
delegate, the header view and the cell delegate in offscreen views: the only
tests that link Qt Widgets, behind `FLYSIGHT_BUILD_WIDGET_TESTS`), and the
manual script in `tests/README.md`, section 12 (steps M1-M9 and M23-M28). That
nothing but the demand layer offers work, and that the views only read it, are
rules of the cleanup audit (groups `gestures` and `demand`).

### 16.11 Component contracts and the one-way flow

One sentence of contract per component, none naming another's workings:

- **The demand layer:** from what is switched on and where results are, it
  derives demand, chooses by priority, has sessions loaded for column demand
  within its bound, offers the next pair, remembers this run's failures, and
  publishes the state the views present.
- **The executor:** runs one requested calculation for one loaded session with
  the engine's prepare / compute / publish steps, holding at most the running
  job and one chosen next job; it never loads a session and knows no caller.
- **The column worker:** keeps cached column values a function of what is on
  disk; it never requests, prepares or runs a requested calculation and never
  knows a job exists.
- **The idle scheduler:** runs steps of registered tasks by priority; it knows
  nothing about jobs.
- **The views:** present the demand layer's state; they never run a pass,
  offer, or write.

**The flow is one way:** the demand layer chooses; the executor publishes into
the loaded session and the engine's listener writes the record, whose reason
the index notes; the record change drops the cached column values over it and
the loaded-row refresh recomputes them; the demand layer sees the result
through the same blocker inspection and record names it always reads, and the
pair leaves demand. No component asks another what it intends to do.

**What stays true:** restoring a stored result is not requesting (only a
missing result creates demand); the temporary copy used for cheap column
values never writes a stored result and never requests; a restored result is
indistinguishable from a published one; the engine's threading rules are
unchanged; the plot widget's "no data" warning keeps asking the engine.

The cleanup audit's group `demand` holds these boundaries as text rules
(`tests/README.md`, section 10).

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
`_time`, and `accH`. They are requested (16.3): a checked fusion plot has the
fit computed for the visible sessions, and nothing else about a plot starts
one (section 16).

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
unavailable when the calculation is not requested. While such a column is
enabled, the demand layer has the calculation computed for every session of
the logbook that has no result, loaded or not (section 16; sessions that are
not loaded are loaded as hidden sessions, two at a time, 16.8); until the
record is written the cached value stays unavailable and the view paints the
cell as pending (16.10), and the record change then drops the value and the
loaded-row refresh computes it. (A column that reached a requested
calculation only through one of several candidates of an on-demand name would
cost one load per session without a record and per run; no registered column
does, 16.8.) It is cached in
`index.json` together with the session's `"records"` stamp (calculation id ->
result version of each record in `cache/`). Writing or deleting a record drops the
values over that calculation at once
(`LogbookManager::calculationRecordsChanged`); a loaded row recomputes them on
the next event-loop pass (`refreshRecordColumns`, which emits nothing: loaded
cells are live). For a stub the value is the same function of the session's
valid stored results: unavailable, without a load, when the logbook knows no
record of it; otherwise the column worker loads a temporary copy, restores
the session's records into it with the checks of a load (15.8: a stale record
deleted, an unreadable one skipped) and caches the value computed from it with
the stamp, exactly as for a loaded row. A value over a record the copy skipped
is not cached (the cell is empty, not the "…" of 16.10) until the session is
loaded; the worker does not come back to it. The copy never writes a record
and never requests or runs a calculation, and the row is not loaded. A bulk edit on a
stub reads no record: it leaves such values missing for the worker. The
ordering rule: a record write flushes
the index first when the index on disk lists that calculation under a cached
value, so no crash leaves a value that disagrees with the records. Unconfirmed
records (a failed write or removal, a record skipped at the load because it
could not be read) keep their values out of `index.json` until the row is
evicted or the record is written or deleted again. An environment change (a
registration, a declared preference, a changed result version such as a plugin
edit) discards the cached values of the columns whose environment it changes
(section 9), and only those, and leaves the records valid; loaded rows
recompute those columns from the engine, and the column worker recomputes a
stub's from its stored results. A Fusion/roll column keeps its cached value
through an altitude marker added at run time or at the next start (no name of
its closure changes), also for a session that is not loaded.
`CalculationCompatibilityVersion` did not change for
this: an index written before the stamp holds explicit-backed values only as
"unavailable", and at start-up they are kept only for sessions without a
record. The marker's current value, 2, identifies the
centered time fit (`_TIME_FIT_A` / `_TIME_FIT_B`, and with them every non-GNSS
`_time`); the constant's comment lists what each value stands for. A stored
rejection of a session that is not loaded is a failed result with the reason
the index recorded for it (16.7): not computed again, listed in the column's
failures before and after a restart alike, and the session is not loaded for
it.

Tests (label `fusion`, behind `FLYSIGHT_BUILD_FUSION_TESTS`):
`tests/tst_fusion_session.cpp` (real `SessionData` engines, the fit on the
test's main thread), `tests/tst_fusion_jobs.cpp` (the executor's worker on a
real `SessionModel`), `tests/tst_fusion_rows.cpp` (the demand layer and the
plot rows of section 16 with the seventeen real plots and real fits: fits
start and are dropped with no gesture),
`tests/tst_fusion_runner.cpp` (the command-line runner against the
application's import path) and `tests/tst_fusion_store.cpp` (the fit's stored
result: unload, restart, rejections, invalidation, merges, the session file
untouched; kept across altitude markers, unrelated registrations, the descent
pause and another plugin set; dropped at once by a registration that provides
a name it looked up; deleted when a lookup resolves differently at load; a
logbook column over roll filled for sessions that are not loaded, and not
fitted again after a restart); the column rule without GTSAM in
`tst_calculation_demand::enablingColumnFillsEveryUnloadedSession` (column
demand), `tst_result_columns::columnWorkerIsUnchangedByDemand`,
`tst_column_cache::explicitBackedColumnFollowsItsResult` and
`tests/tst_result_columns.cpp` (the stamp, crash points, stubs filled from
their stored results by the column worker), and with a real fit in
`tst_fusion_jobs::columnOnFusionOutputIsCachedFromRecord`,
`tst_fusion_jobs::altitudeMarkerKeepsColumnsOfUnloadedSession` and
`tst_fusion_jobs::workerRefillsColumnFromStoredFit`. The model, its
limitations and what is rejected are in [SENSOR_FUSION.md](SENSOR_FUSION.md).
