# Phase 1: The engine's provenance query

Plan: `PLANS/implementation-plan/00-overview.md` (the specification is inside
it and is the authority wherever this document and it disagree). Baseline:
branch `store-requested-calculations` at `dbf5a78`.

## Purpose

`EvaluationContext` gains one boolean query: for a declared attribute input,
whether the value it serves is the session's stored attribute or came from a
candidate. The engine already knows (the first step of `resolve()` for an
attribute name is the stored attribute, `Provider::Stored`); this phase carries
the fact into the context beside the value, so that a calculation can word a
message or a record by where its input came from without reaching outside its
declared inputs (specification section 4, first bullet, the engine's half).

It is one phase because the engine, its tests and its document share no code
with the kernel (phase 2) or the registration (phase 3) and leave the tree
green on their own; it also opens the acceptance range, 1801-1802, in the four
shared files, which is why phase 2 follows it although independent in code.

## Dependencies

- Depends on nothing. Assumes the baseline: `InputValue` holds `value`,
  `samples` and `unit`; `declared()` records an undeclared read and returns
  null; `gatherInputs()` fills the context for every input kind.
- Blocks phase 3, which calls the query from the fusion registration.
- Phase 2 consumes nothing here but extends the shared lines this phase owns
  (under "Interfaces"), so it is implemented after this one.

## What changes, by area

### The context (`src/engine/evaluationcontext.h`, `.cpp`)

- Public, beside `attribute()`:
  `bool attributeIsStored(const QString &key) const;` True iff the declared
  attribute input `key` was served by the session's stored attribute; false
  when a candidate provided it. For a key the calculation did not declare it
  returns false and records an undeclared read, exactly as `attribute(key)`
  does: it goes through `declared(CalcInput::attribute(key))`, so the same
  warning is logged on the synchronous path and the result is discarded
  (`ResultStatus::UndeclaredRead`). It is a read, not a silent query; its
  comment states the contrast with `isDeclared()`. A calculation cannot learn
  whether an input it did not declare is stored.
- Private: `InputValue` gains a `bool` (name the implementer's; false by
  default), meaningful for an Attribute input and false for every other kind.
  `provide()` keeps its signature; the value arrives filled.
- The class comment keeps its "deliberately absent" list and adds that the
  provenance of an attribute input is the one fact about *how* an input was
  resolved that the context tells, captured with the value for both paths.

### The engine (`src/engine/calculationengine.cpp`)

- `gatherInputs()`, the Attribute case: beside `value.value = r.attribute;`
  set the flag from `r.provider == Provider::Stored`. This is the one fill
  site, and every path gathers through it: `computeResult()` (ordinary reads
  and `request()`), `prepare()` and `restoreResult()` through
  `gatherAsRoot()`. `Provider` stays private; the context carries a bool, not
  the engine's vocabulary.
- Nothing else changes: not `resolve()` and its rule, `ResolutionEntry`,
  `Provider`, `Layer`, `readLeafInput()` (preferences and source inputs have
  no provenance), the graph, the cache, invalidation, the oracle's `Value` or
  `evaluateFresh()`, `inspectInstance()`; no new public member of
  `CalculationEngine`; `calctypes.h` (`CalcInput`) unchanged.

### The prepared and restore paths, the plugin bridge: no change

- `preparedcalculation.h`, `.cpp`: the context is filled at prepare and moved
  into the ticket; the flag sits in `m_inputs` beside the value, a plain bool
  read by `compute()` on whatever thread. A stored attribute set or removed
  between prepare and publish reaches the ticket through the `StoredAttribute`
  leaf the resolution noted, so publish refuses (`InputsChanged`). No diff.
- `restoreResult()` gathers the same way and runs no compute; the bundle it
  installs was computed under the provenance the snapshot's `resolutions`
  record (`StoredResolution::Provider::SessionData` against `Calculation`),
  and a snapshot whose attribute input is provided otherwise now is `Stale`
  with `StaleCheck::Resolutions`: `tst_calcengine_restore::restoreStaleChecks`,
  row "stored instead of calculated". Cited, not duplicated.
- `src/pluginsessionview.h`, `src/pluginadapters.cpp`,
  `src/sessiondata_bindings.cpp`: untouched; the query is not for plugins
  (overview), and the audit rule below keeps the name out of them. Nothing
  under `src/fusion/`, `src/calculations/`, `src/sensorconfiguration.*`; no
  golden or algorithm string; `tst_builtins_engine::noUndeclaredReads` keeps
  its counts (74 plain calculations, no undeclared read).

## Interfaces

### Provided to phase 3

`bool EvaluationContext::attributeIsStored(const QString &key) const` in
`src/engine/evaluationcontext.h`, the semantics above, identical on the
synchronous and the prepared paths. The registration will pass
`ctx.attributeIsStored` into `Fusion::channelsFrom()`'s reader.

### Shared lines this phase owns (phase 2 extends them)

- `tests/acceptance_map.txt`: the header's first line ("Nineteen
  specifications, nineteen item ranges"); the range paragraph for 1801-1802
  (phase 2 rewrites its bounds to 1801-1809 and extends its item sentence);
  "one of the nineteen ranges"; the list of ranges needing an automated line
  (gains "or 1801-1802"); "sections 9.1 to 9.19"; the `# 1801` and `# 1802`
  blocks at the end of the file (phase 2 appends `# 1803` onwards).
- `tests/README.md`: the contents line for appendix S; section 1's rows for
  `tst_calcengine` and `tst_calcengine_async`; section 9's preamble (nineteen,
  three times); section 9.19's heading "(items 1801-1802)", paragraph and two
  rows (phase 2 restates the heading and item sentence, appends rows); section
  10's bullet for group `inferred-configuration` (phase 2 extends its item and
  rule lists); appendix S's heading, preamble and items 1-2 (phase 2 appends).
- `docs/CALCULATIONS.md`: one bullet in section 3; phases 2 and 3 edit
  sections 5 and 17 only.
- `tests/audit/cleanup_audit.cmake`: the last bullet of the header comment
  (phase 2 extends its text); the group block `inferred-configuration` (its
  title comment's item list and rules; phase 2 appends under the same
  `audit_group`); in the traceability block the comment listing the ranges,
  the range check's `OR (item GREATER_EQUAL 1801 AND item LESS_EQUAL 1802)`
  with its violation text, and `foreach(item RANGE 1801 1802)` (phase 2 turns
  each 1802 into 1809).

## Acceptance criteria

"Spec 4.1" is section 4, first bullet, of the specification in the overview.

1. (spec 4.1, the engine's half) `evaluationcontext.h` declares
   `bool attributeIsStored(const QString &key) const` exactly once, public,
   with a comment stating the three answers (stored, candidate, undeclared).
2. (spec 4.1) Stored reads true, a candidate's output false; storing over the
   candidate invalidates the calculation and reads true, removing it reads
   false; a constant default reads false until a value is stored over it.
3. (spec 4.1) An undeclared key reads false even when the session stores it,
   ends the run `UndeclaredRead`, counts one undeclared read of
   `CalcInput::attribute(key)` (`undeclaredReadCount()`, `lastUndeclaredRead()`)
   and never resolves the key on the calculation's behalf
   (`cachedState(attr(key)) == NotCached`); a declared preference does not
   license the query of the same key.
4. (spec 4.1; overview, "The provenance query is one boolean") The flag is set
   in `gatherInputs()` from the resolution's provider and nowhere else;
   `preparedcalculation.h`, `.cpp`, `calctypes.h` and the three plugin bridge
   files have no diff; `CalculationEngine` gains no public member.
5. (spec 4.1; `prepare()`'s contract, "both paths give identical results") In
   each compute mode (inline, `std::thread`, `QThread`) the compute function
   sees what a `request()` on an identical world gives; the undeclared variant
   publishes `UndeclaredRead` with the same warning at publish; the two
   engines' undeclared-read instrumentation agrees.
6. (spec 5, the engine's section) `docs/CALCULATIONS.md` section 3 has a
   bullet naming `EvaluationContext::attributeIsStored`: what it answers, an
   undeclared key an undeclared read, both paths alike.
7. (spec 6, last bullet, this phase's share) `audit_cleanup` passes with group
   `inferred-configuration` planted and 1801-1802 declared in its three places;
   the map's header counts nineteen; 1801 and 1802 each have a test or audit
   line; section 9.19, appendix S and the contents line exist.
8. The whole suite is green, run sequentially in `build-agent/`; no file under
   `src/fusion/`, `src/calculations/` or `tests/data/fusion/` changes.

## Tests

### `tests/tst_calcengine.cpp` (two functions; the map cites the names)

- `attributeIsStoredFollowsTheProvider`, after `storedWins`,
  `removingStoredFallsBackToCalc` and `constantCalculationIsADefault`: on the
  shared world (`A`, `B` stored; `X` the output of `sum`) a calculation
  declaring `A` and `X` records, through captured locals, `A` true and `X`
  false; storing `X` invalidates it and the next run reads `X` true; removing
  `X` reads false again, with `removingStoredFallsBackToCalc`'s run counts.
  On a private world the constant `K` and a dependent, as
  `constantCalculationIsADefault`: false, then true under a stored `K`. No
  undeclared read throughout.
- `attributeIsStoredOfAnUndeclaredKeyIsARead`, after `undeclaredReadDetected`:
  a calculation declaring `A` asks for `other`, which the state stores: false,
  outputs unavailable, `resultStatus` `UndeclaredRead`, one undeclared read of
  `CalcInput::attribute("other")`, `cachedState(attr("other"))` `NotCached`,
  one run. Then the kind: declaring `preference("p")` and asking
  `attributeIsStored("p")` counts a second read, of `CalcInput::attribute("p")`.
  `isDeclaredIsSilent` stays as the contrast.

Both lambdas name their context parameter: the audit's "one authority:
constant defaults" rule matches `](const EvaluationContext &)` in `src`.

### `tests/tst_calcengine_async.cpp` (one data-driven function)

- `attributeIsStoredMatchesSync_data() { addComputeModeRows(); }` and
  `attributeIsStoredMatchesSync`, after `asyncMatchesSync`,
  `prepareCapturesInputs` (the `Seen` atomics) and
  `undeclaredReadAndInvalidOutputMatchSync`: an explicit calculation declaring
  `EA_IN` (stored) and `X` (`sum`'s output) records both answers in atomics;
  two worlds, one through `request()`, one through `runAsync(...,
  ComputeMode(mode))`: true for `EA_IN`, false for `X` on both, no undeclared
  read. Store `X` in both (the result drops), run again: true for `X` on both.
  The capture: prepare, store `X`, compute, publish refuses `InputsChanged`.
  The sneaky half: an explicit calculation declaring `EA_IN` that asks
  `attributeIsStored("EB_IN")` publishes `UndeclaredRead` on both paths, the
  warning (`read an undeclared input: attribute(EB_IN)`) captured once from
  the synchronous world and once at publish (`WarningCapture`),
  `lastUndeclaredRead()` equal, `verifyAgainstFresh` empty.

### The audit (`tests/audit/cleanup_audit.cmake`)

- Header comment, a last bullet after the fused position and speed accuracy:
  the IMU configuration is inferred where a key is absent; the engine tells a
  calculation, for a declared attribute input, whether the session stated it,
  through one query of the evaluation context, spelled in `src` in the engine
  and the fusion registration alone, never in the plugin bridge, declared once
  in the context's contract and described by the calculations document (items
  1801-1802). Phase 2 extends the sentence.
- A block for `inferred-configuration (items 1801, 1802)` in the file's
  rule-line style, before the `leftover markers` block, opened with
  `audit_group(inferred-configuration)`; three rules, each with an "Allow:"
  comment:
  - `expect_only("the provenance query is spelled in the engine and the fusion
    registration alone" "attributeIsStored"
    "^src/(engine/|fusion/fusionregistration\\.cpp)" src)`. The registration
    is allowed now although nothing there asks yet: phase 3 is its one reader
    and then touches no rule. The plugin bridge lies outside the allowed set,
    which is the "not exposed to plugins" check. Allow: none; a second reader
    is a design change.
  - `expect_count("the provenance query is declared once"
    "bool +attributeIsStored\\(const QString &key\\) const" 1
    src/engine/evaluationcontext.h)`: one authority for the contract.
  - `expect_count("the calculations document describes the provenance query"
    "EvaluationContext::attributeIsStored" 1 docs/CALCULATIONS.md)`. Allow:
    this counts lines; a later section naming the query in its qualified form
    (phase 3's section 17 may) raises the count here.
- Traceability block: the comment listing the ranges gains "and 1801-1802
  (inferred IMU configuration: the engine's provenance query, its document and
  the audit rule)"; the range check gains
  `OR (item GREATER_EQUAL 1801 AND item LESS_EQUAL 1802)` and its violation
  text "... 1701-1717 and 1801-1802"; a loop `foreach(item RANGE 1801 1802)`
  after the 1701-1717 one, same body. Plant one hit (an item 1803 line, or a
  misspelt group) to prove the loop and the group check fire, then remove it.

### The map (`tests/acceptance_map.txt`)

Header: "Nineteen specifications, nineteen item ranges"; after the 1701-1717
paragraph: `1801-1802 inferred IMU configuration (the configuration inferred
where a key is absent, the firmware default its fallback; the engine's query
that says whether an attribute input was stated): item = 1800 + item number of
appendix S. 1801 the engine's half of the first bullet of its section 4, 1802
the engine's sentence of its section 5. Stated in full in tests/README.md,
appendix S. No item of an earlier specification is restated by these two.`
(phase 3 rewrites the last sentence); "one of the nineteen ranges";
"1701-1717 or 1801-1802 needs at least one test or audit line"; "sections 9.1
to 9.19". At the end of the file:

```text
# 1801 - (4, first bullet, the engine's half) the provenance query: EvaluationContext::attributeIsStored(key) tells a calculation, for a declared attribute input, whether the value is the session's stored attribute (true: the first step of the resolution served it) or came from a candidate (false); for a key the calculation did not declare it returns false and records an undeclared read exactly as attribute(key) does, so the result is discarded (UndeclaredRead) and the key is never resolved on the calculation's behalf; the fact travels in the context beside the value, filled once where the inputs are gathered, so the synchronous, prepared and restore paths cannot differ: a prepared run sees what a request sees in every compute mode, and a stored result whose attribute input is provided otherwise at restore is stale; it is not exposed to Python plugins, and no built-in asks it yet
1801 tst_calcengine attributeIsStoredFollowsTheProvider
1801 tst_calcengine attributeIsStoredOfAnUndeclaredKeyIsARead
1801 tst_calcengine_async attributeIsStoredMatchesSync
1801 tst_calcengine_restore restoreStaleChecks
1801 audit inferred-configuration

# 1802 - (5, the engine's section) docs/CALCULATIONS.md section 3 describes the provenance query: what it answers, that an undeclared key is an undeclared read, that it is served alike on both paths; the audit's inferred-configuration group holds the query's name in src to the engine and the fusion registration (its reader to come), never the plugin bridge, declared once in the context's contract and named in the calculations document; the range 1801-1802 declared and complete, the map's header at nineteen specifications, section 9.19 and appendix S of tests/README.md
1802 audit inferred-configuration
```

### The guide (`tests/README.md`)

- Contents: a line for appendix S after appendix R's.
- Section 1: `tst_calcengine`'s row gains "the provenance query of an
  attribute input (the stored attribute or a candidate's; an undeclared key is
  an undeclared read)"; `tst_calcengine_async`'s row gains "the provenance
  query identical on both paths".
- Section 9's preamble: nineteen, three times.
- Section 9.19 "Inferred IMU configuration (items 1801-1802)", on 9.18's head:
  the specification `PLANS/inferred-imu-configuration.md`, stated in appendix
  S, the same four line forms as 9.2, sections 1 and 2 without an item, which
  item is which; the `audit inferred-configuration` line of 1802 is a floor
  (the rule shows the name is in section 3, not that the bullet says what 1802
  lists; the reviewer reads it). Two rows in the matrix form, the statements
  the map's.
- Section 10: a bullet for group `inferred-configuration` (items 1801, 1802)
  after the `accuracy-channels` bullet, naming the three rules as the bullets do.
- Appendix S "The acceptance items of the inferred IMU configuration
  (1801-1802)", on appendix R's preamble (item = 1800 + the number below; no
  earlier item restated by these two), then items 1 and 2, the section in front.
- Section 12: nothing (M60 is phase 3's).

### The document (`docs/CALCULATIONS.md` section 3)

One bullet after "Reads go only through the `EvaluationContext`": a
calculation may ask `EvaluationContext::attributeIsStored(key)` whether a
declared attribute input is the session's stored attribute (the first step of
section 5's order) or a candidate's output, so that it can word a message or a
record by where the value came from; it is a read of the declared input, not a
silent query like `isDeclared()`: for a key the calculation did not declare it
returns false and counts as an undeclared read, exactly as `attribute(key)`
does; captured with the value at prepare, so the asynchronous path (section
12) serves it as the synchronous one does; no such query exists for a
preference or a measurement.

## Decisions

- **A field of `InputValue`, not a parallel set.** The value and its provenance
  are one input: one struct, one `provide()`, one fill site; nothing of the
  engine's `Provider` vocabulary crosses into the context.
- **Attributes only.** No `measurementIsStored`: a measurement with source data
  is the conversion layer's output and nothing asks (no generality nothing
  uses, `CLAUDE.md`).
- **Not silent.** `isDeclared()` is silent because an empty value can be
  legitimate; a false for an undeclared key here would teach a calculation
  something it did not declare (the overview fixes this). The warning stays
  the one text of `declared()`.
- **Test names fixed here**, because the map cites them.
- **The audit's source rule already allows the registration**, so phase 3 adds
  its caller without touching the rule; the document rule counts the qualified
  name and its "Allow:" says a later section raises the count. The group block
  sits last among the groups so that phases 2 and 3 append to one block.
- **No restore code, one cited test.** The restore path repeats the lookups and
  refuses a changed provider (`StaleCheck::Resolutions`); the phase cites
  `restoreStaleChecks` rather than proving the same row again.
- **The guide's contents list** is not named in the overview; the last tidy
  commit extended it to appendix R, so this phase extends it to S.

Ready.
