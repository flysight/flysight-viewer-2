# Phase 2: Constant defaults

## Purpose

Spec §10, with the wind parts of §11, §12, §13 and §14, and overview
decisions 10 and 11. A default that stands in for a value the user has not
set becomes a registered calculation. The application gets one helper that
registers a constant default. Wind north and wind east become constants of
zero registered through it. The importer and the logbook's legacy backfill
stop writing wind, and the documents state the rule.

This is one phase because the helper, its first product users and the removal
of the two writers are one change of authority. Doing any of them alone would
leave two authorities for the wind default, or a helper nothing uses.

## Dependencies

- Depends on nothing. Phase 1 (the choice attribute type) is in place when
  this phase starts. It touches none of this phase's files.
- Blocks phase 4, which registers the orientation default through this
  phase's helper from `Fusion::registerFusionCalculations()`.
- Phase 3 runs after this phase and edits `docs/CALCULATIONS.md` §17. This
  phase edits only §5 of that document.

## What changes

### The helper (`src/calculations/attributecalculations.h`)

Declare `Calculations::addConstantDefault` with the signature fixed under
"Interfaces" below. It registers, through `Calculations::addCalculation`
(`src/calculations/registration.h`), a calculation that:

- has the id `builtin.default.` followed by the attribute key;
- has no inputs and one output, `DependencyKey::attribute(attributeKey)`;
- has a compute that returns `value` for that output and ignores its context;
- has no title, no result version and the default on-demand policy.

Define it `inline` in the header, as `addCalculation` is defined in
`registration.h`. The header then includes `registration.h` plus `QString`
and `QVariant` in place of today's lone forward declaration. The inline
definition is decision D1 below. Give the helper a comment that states its
contract: a constant default is a calculation with no inputs; a stored
attribute of the same key wins over it; returning to the default means
removing the stored attribute, never storing a blank. Update the comment on
`registerAttributeCalculations()`, which today says "(ids builtin.attr.*)", so
that it also names the two `builtin.default.` ids it registers.

### Wind as constants of zero (`src/calculations/attributecalculations.cpp`)

In `registerAttributeCalculations()`, register `_WIND_N` and `_WIND_E` with
value `0.0` through the helper, using `SessionKeys::WindN` / `WindE`. Place
the calls beside the exit-time defaults, the block that begins
`// Video sync time and course reference: each defaults to exit time`, so
that the data-derived and the constant attribute defaults sit together. Say in
a comment that wind stands in for a value the user has not set: zero means no
wind correction, and a stored value wins.

`windComponent()` in `gnsscalculations.cpp` and every calculation that
declares `CalcInput::attribute(SessionKeys::WindN)` / `WindE` (`wcVel`,
`wcVelH`, `trackInputs`, `aeroInputs`) stay exactly as they are. Their inputs
are now available in every session: through the stored value when there is
one, and otherwise through the constant.

### The existing constant defaults move onto the helper

Spec §10 requires that "every constant default in the application is found by
one search". Overview decision 11 names that search: `addConstantDefault(`.
Two local copies of the helper exist today:

- `registerSpDefault` in `src/calculations/spcalculations.cpp`, for three
  defaults: `_SP_PERF_WINDOW_HEIGHT`, `_SP_VAL_WINDOW_HEIGHT` and
  `_SP_BREAKOFF_ALT`;
- `registerWspDefault` in `src/calculations/wspcalculations.cpp`, for four
  defaults: `_WSP_VERSION`, `_WSP_TOP_ALT`, `_WSP_BOTTOM_ALT` and `_WSP_TASK`.

Both copies go. Each of their calls becomes an `addConstantDefault` call, in
the same place and with the same value and the same `QVariant` type: the SP
defaults stay doubles and the WSP version and task stay strings. The ids
change from `builtin.sp.default.*` and `builtin.wsp.default.*` to
`builtin.default.<key>`. This is decision D2 below.

Each attribute has one candidate, so the candidate order of every name is
unchanged. The "Group A: Parameter defaults" headings may stay.

### The importer stops writing wind (`src/dataimporter.cpp`, `.h`)

In `DataImporter::applyCreationDefaults()`, remove step 5 together with its
comment `// 5. Wind defaults (placeholder; will be calculated per-track in
future)`, and renumber the steps that follow. The other steps stay as they
are, including jumper mass, planform area and the fixed ground elevation (spec
§3).

In `dataimporter.h`, remove wind from both lists of import-time defaults: the
class comment's "(description, import time, wind, mass, area, ...)" and the
`applyCreationDefaults` comment's `_WIND_N / _WIND_E`.

### The legacy backfill stops writing wind (`src/logbookmanager.cpp`, `.h`)

In `LogbookManager::applyLegacyBackfill()`, remove the block
`// Backfill wind defaults for sessions saved before wind attributes existed`
(overview decision 10). The mass and area backfill stays, and so do the
function's name, its callers and its "not an import default" contract.

In `logbookmanager.h`, the comment "predate `_JUMPER_MASS` /
`_PLANFORM_AREA` / `_WIND_N` / `_WIND_E`" names only the two keys that are
still backfilled.

### Documents

- `docs/CALCULATIONS.md` §5, next to "Stored data always wins over any
  calculation.": state the rule for defaults in spec §10's terms and name the
  helper.
  - The importer stores only what is a fact of the import: identity,
    provenance, and a choice the user made at import (mass, area, the fixed
    ground elevation).
  - Anything that stands in for a value the user has not set is a
    calculation. It is derived where possible (ground elevation, sync time,
    course reference). Otherwise it is constant, registered with
    `Calculations::addConstantDefault` as `builtin.default.<key>`, and every
    such default is found by searching for that name.
  - A stored value wins even when it is invalid or empty (resolution step 1
    in the same section). Returning to the default therefore removes the
    stored attribute.

  Edit only §5. Phase 3 edits §17.
- `docs/DATA_SCHEMA.md` §8: take `_WIND_N` and `_WIND_E` out of the list of
  import-time defaults. Say that wind is not written at import: a session
  without stored wind reads zero from a constant default (`docs/CALCULATIONS.md`
  §5), and a file that carries wind keeps it.
- `docs/DATA_SCHEMA.md` §10: the legacy backfill adds only `_JUMPER_MASS` and
  `_PLANFORM_AREA`, and wind is no longer backfilled.
- `docs/DATA_SCHEMA.md` §9 and §11 need no change. §11 already says that a
  changed candidate list discards a column's cached values once.

### What must not change

- The values of every GNSS calculation that reads wind: `tst_builtins_golden`,
  `goldenOnEngine` and the oracle's golden end state all pass with unchanged
  literals.
- `CalculationCompatibilityVersion`. `docs/CALCULATIONS.md` §9 says: "Do not
  bump it for added, removed, or renamed registrations". No existing
  session's column value changes: a stored wind still wins, and an absent
  wind reads the same zero that the backfill used to store.
- The fit, its inputs and its stored results. The fit reads no wind and no
  SP or WSP attribute.
- Jumper mass, planform area and the fixed ground elevation in both the
  importer and the backfill.

## Interfaces

### Provided (used by phase 4)

```cpp
namespace FlySight::Calculations {
void addConstantDefault(CalculationRegistry &registry,
                        const QString &attributeKey, const QVariant &value);
}
```

- Declared, and defined inline, in `src/calculations/attributecalculations.h`.
- Registers `builtin.default.<attributeKey>`: no inputs, one output
  `DependencyKey::attribute(attributeKey)`, value `value`.
- Registered by this phase: `builtin.default._WIND_N` and
  `builtin.default._WIND_E`, both with value `0.0` (a double), from
  `registerAttributeCalculations()`.
- Also registered by this phase, at their existing sites:
  `builtin.default._SP_PERF_WINDOW_HEIGHT`, `_SP_VAL_WINDOW_HEIGHT`,
  `_SP_BREAKOFF_ALT`, `_WSP_VERSION`, `_WSP_TOP_ALT`, `_WSP_BOTTOM_ALT` and
  `_WSP_TASK`.

Phase 4 calls the helper from `src/fusion/fusionregistration.cpp` for
`_ORIENTATION`. The helper needs only headers that `flysight_fusion` already
reaches through `flysight_model`, so that call adds no link dependency.

### Consumed

Nothing from phase 1.

## Acceptance criteria

1. `attributecalculations.h` declares `addConstantDefault` with the signature
   above. Registering through it produces `builtin.default.<key>` with an
   empty input list and exactly one output, the attribute (§10 "one helper";
   decision 11).
2. `registerBuiltInCalculations()` registers `builtin.default._WIND_N` and
   `builtin.default._WIND_E`, each with the value `0.0` (§10, §12).
3. In `src`, `addConstantDefault(` is the only way a constant default is
   registered. `registerSpDefault` and `registerWspDefault` no longer exist.
   Every compute lambda with an unnamed `const EvaluationContext &` parameter
   in `src` is the helper's (§10 "found by one search"; decision 11).
4. The seven SP and WSP defaults keep their values and value types, and their
   ids are `builtin.default.<key>`.
5. A session with no stored `_WIND_N` / `_WIND_E` reads `0.0` for both through
   `SessionData::getAttribute`, and `hasAttribute` stays false. A stored value
   wins. Removing it returns the value to `0.0`, and `GNSS/wcVel` follows each
   change (§10, §11 "Wind columns show zero", §13).
6. `applyCreationDefaults` writes neither `_WIND_N` nor `_WIND_E`. A session
   created by import has neither stored, and its saved file has no
   `$VAR,_WIND_` line. A Viewer-saved file that carries wind, imported as a
   new session, keeps its value (§10, §13 "the importer no longer writes
   them").
7. `applyLegacyBackfill` adds `_JUMPER_MASS` and `_PLANFORM_AREA` only. A
   released file without these attributes gains exactly those two lines on
   its next save, and a further load and save changes nothing (decision 10;
   §3).
8. `src/dataimporter.*` and `src/logbookmanager.*` contain no `_WIND_`,
   `WindN` or `WindE`. The placeholder comment is gone (§10).
9. `docs/CALCULATIONS.md` §5 states the rule and names `addConstantDefault`.
   `docs/DATA_SCHEMA.md` §8 and §10 no longer list wind among the import-time
   defaults or the backfill (§14).
10. The golden values, `CalculationCompatibilityVersion` and every fusion
    test are unchanged. The whole suite passes, including `audit_cleanup`.

## Tests

All are in existing executables. No executable is added, so
`tests/CMakeLists.txt` does not change.

- **`tst_builtins_engine`**
  - `inventory`: the expected list gains `builtin.default._WIND_N` and
    `builtin.default._WIND_E` at their registration position. The seven SP
    and WSP ids take their new names. The count becomes 75 and its comment
    changes to "72 calculations".
  - `noUndeclaredReads`: `plain` becomes 72.
  - Add `constantDefaults()`:
    - every registered id that starts with `builtin.default.` has no inputs
      and exactly one attribute output, named by the id's suffix;
    - the set of these ids is exactly the nine above, with their values;
    - on the descent fixture, which no longer stores wind, `_WIND_N` reads
      `0.0`, a stored `5.0` wins and changes `GNSS/wcVel`, and removing it
      restores both.

  This test proves criteria 1 to 5 at the engine level. The mechanism itself
  stays proved by `tst_calcengine::constantCalculationIsADefault`, which is
  unchanged.
- **`tst_smoke::importAppliesCreationDefaults`**: replace the two
  `hasAttribute("_WIND_*")` checks with `!hasAttribute`. Keep the
  `getAttribute(...) == 0.0` checks, which now read the calculation.
- **`tst_importer::creationDefaultsOnlyFillAbsent`**: `_WIND_N` and `_WIND_E`
  are not stored. Also give the input file `.var("_WIND_N", "3")` and assert
  that the stored `"3"` survives: a recording that has wind keeps it. The
  final `file.data.attributeKeys()` list gains `_WIND_N` if that variable is
  added.
- **`tst_import_merge::newSessionGetsDefaults`**: `!hasStoredAttribute("_WIND_N")`,
  and the saved file contains no `$VAR,_WIND_` line.
- **`tst_logbook_index::rawLoadSkipsBackfill`**:
  - the ordinary load's key list and the `partial` key list lose `_WIND_E`
    and `_WIND_N`;
  - the `storedAttribute("_WIND_N")` comparison becomes "not stored, and
    `getAttribute` gives 0".
- **`tst_persistence_roundtrip::releasedLogbookBackfillIsAdditive`**:
  - exactly two lines are added (`$VAR,_JUMPER_MASS,1` and
    `$VAR,_PLANFORM_AREA,1`), and no `_WIND_` line appears;
  - the final byte comparison against `releasedFile(true)...` must no longer
    expect the wind lines. How is the implementer's choice: split
    `releasedFile`'s parameter, or compare otherwise.
  - The comments on `releasedFile` ("the four attributes loadSession()
    backfills") and on the test ("gains exactly those four lines") say two.
  - `logbookSaveReloadCycle`'s comment "(import time as a double, mass, area,
    wind)" loses wind.
  - `typedAttributesRoundTrip` keeps its explicit `_WIND_N`, which is a user
    value.
- **`tests/support/oraclecatalogue.cpp`**: `_WIND_N`'s edits become
  `{0.0, 5.0, remove}`, so removal back to the default is exercised.
  - `tst_session_oracle`'s `endState` drops its `_WIND_N` special case: the
    ordinary `removeAttribute` restores the fixture, which no longer stores
    wind. The `_JUMPER_MASS` special case stays.
  - The catalogue's attribute `_WIND_N` stays.
- **Fixtures whose comment ties wind to the backfill.** Each of these stores
  wind with a comment that says loadSession would otherwise backfill it:
  - `tst_logbook_index` `makeSession`;
  - `tst_column_cache` `gyroSession`;
  - `tst_result_columns` `columnSession`;
  - `tst_result_records` `makeSession`.

  Remove their `_WIND_N` / `_WIND_E` lines and correct the comments ("the two
  attributes"). If a test there turns out to depend on stored wind, keep the
  lines, reword the comment so that it does not claim a backfill, and report
  it.
- **Unchanged, checked**: `tst_workflow`'s `kReleased` and the other
  `releasedFile(true, ...)` uses. They are released files that carry wind,
  and stored wind survives.
- **Any other failure** that counts an imported session's attributes, or
  compares its saved bytes, reflects the two keys that are no longer written.
  Amend it to the new contract and report it.
- **Audit** (`tests/audit/cleanup_audit.cmake`): add a group
  `audit_group(constant-defaults)` in its own block. Put it after the
  ungrouped "one authority per fact" rules and before
  `audit_group(local-projection)`. Give each rule an "Allow:" comment, and add
  a clause to the file's header list of single authorities. The rules:
  - `expect_only("one authority: constant defaults"
    "\\]\\(const EvaluationContext *&\\)"
    "^src/calculations/attributecalculations\\.h$" src)`. A compute that
    ignores its context is a constant, and only the helper writes one. Today
    this pattern matches in `src` only the two local helpers that this phase
    removes. The `using` aliases do not match, because a `(` precedes them,
    not a `]`.
  - `expect_none("the replaced default helpers stay gone"
    "register(Sp|Wsp)Default" src tests)`.
  - `expect_none("no wind default in the importer or the backfill"
    "_WIND_|WindN|WindE" src/dataimporter.cpp src/dataimporter.h
    src/logbookmanager.cpp src/logbookmanager.h)`.

  Before relying on each rule, prove it by planting a hit once.
  `tests/README.md` section 10 is phase 6's (overview), so this phase does not
  describe the rules there.
- **`tests/README.md` section 1**: the `tst_builtins_engine` row gains "the
  constant defaults (`constantDefaults`)". No other row changes: the
  `tst_importer`, `tst_persistence_roundtrip` and `tst_logbook_index` rows
  stay true.
- **`tests/acceptance_map.txt`**: no line changes. No function the map cites
  is renamed or removed. `6 tst_persistence_roundtrip
  releasedLogbookBackfillIsAdditive` keeps its name and now pins the smaller
  backfill.
  - No item in appendices A-H says that the importer or the backfill writes
    wind. For phase 6, the wording that changes meaning is item 6's
    section 9.1 row ("the `loadSession` backfill"), which now covers mass and
    area only, and `docs/DATA_SCHEMA.md` §8 and §10.

## Decisions

- **D1. The helper is defined inline in the header.** `attributecalculations.cpp`
  is part of `flysight_core`, but `flysight_fusion` links only
  `flysight_model` (`src/CMakeLists.txt`). Phase 4 calls the helper from the
  fusion registration. An inline definition needs only the engine headers,
  which `fusionregistration.cpp` already includes through `registration.h`.
  So that call adds no link dependency, as `addCalculation` adds none today.
  The wind calls still sit beside the exit-time defaults, and the search for
  `addConstantDefault(` still finds everything.
- **D2. The SP and WSP defaults join the helper, and their ids change.**
  Otherwise decision 11 ("every constant default in `src` is found by
  searching for `addConstantDefault(`") would be false on the day it is
  written, and two copies of the mechanism would stay beside the one that
  replaces them. The rename changes the column environment of the SP and WSP
  columns and of every column over a wind-dependent measurement: the new wind
  candidate enters their closure. Each such column discards its cached
  `index.json` values once and recomputes them in the background, as
  `docs/DATA_SCHEMA.md` §11 describes. No stored result is affected, because
  the fit looks up neither wind nor SP or WSP attributes. Keeping the old ids
  would have needed an id parameter that the fixed interface does not have.
- **D3. No attribute definition for wind.** Wind has no entry in the attribute
  registry today, and the spec adds none. §11's "Wind columns show zero" is
  met where a column or a calculation reads the attribute, through
  `getAttribute`.
- **D4. Files without wind remain readable by older versions.** Released
  Viewer versions backfill wind on load, so a file saved after this change
  loads there as before. Nothing migrates existing files. Stored wind stays in
  them and wins.

Ready, with one caveat (D2): the phase also moves the seven SP and WSP constant defaults onto the helper and renames their ids, which the overview did not name but decision 11 requires.
