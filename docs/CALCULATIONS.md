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
  (`CalcInput::sourceMeasurement` / `sourceUnit`). Only the two conversion
  families depend on the source layer, with one exception: Python plugins that
  declare `source()` inputs, for which the plugin host sets
  `CalculationDescriptor::allowSourceInputs`. Never set it in C++.

## 4. Multi-output calculations and partial results

One computation is one calculation, however many outputs it has; do not group
unrelated calculations because their outputs share a prefix. Set the outputs
you found and leave the rest unset: an unset output is unavailable. Never pass
an invalid `QVariant` to "clear" something. The result is published atomically,
and the calculation runs once however many of its outputs are read. When the
user stores an attribute that is also an output (a marker dragged by hand), the
stored value overrides that one output while the others stay available.

## 5. Candidates and order

Several calculations may declare the same output. They are tried in
registration order; the first whose inputs are all available *and* which
produces the output wins. "Works with or without X" is therefore two
registrations, the one that needs X first.

Registration order is the order in `registerBuiltInCalculations`: the
conversion families, attribute, GNSS, IMU, MAG, time, simplification, WS-P, SP,
interpolation. Python plugins are registered before the built-ins. Stored data
always wins over any calculation.

The engine records everything a resolution looked at, including the candidates
it rejected, so a cached fallback is replaced when a preferred candidate
becomes viable. A dependency cycle makes every calculation on the ring
unavailable and logs a warning; never rely on that. (Known limitation: for
*overlapping* cycles, which calculation is reported unavailable can depend on
read order. The built-ins are acyclic, and `tst_session_oracle` asserts that no
cycle ever occurs with them.)

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
state when an input changes. Nothing uses the policy yet; it exists for a
future job queue. `request` returns the names whose cached "not requested"
answer was dropped: a future model-level caller must publish that set through
`SessionModel`, which is the single emitter of `dependencyChanged`. (Known
limitation: an explicit calculation whose input transitively depends on its own
output is a cycle that `request` does not diagnose up front; do not write one.)

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
