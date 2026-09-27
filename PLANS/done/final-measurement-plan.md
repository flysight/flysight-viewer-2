# Measurement sources and calculation ownership: implementation plan

Date: 2026-09-17

Status: implementation explicitly authorized by the subsequent user request.
Phases 0–9 are implemented and accepted, including the normal Release build and
isolated two-process application lifecycle. See
[measurement-implementation-status.md](measurement-implementation-status.md) for
current ownership and validation. The baseline descriptions below are historical.

## 1. Outcome and scope

Preserve imported measurements and all of their imported source attributes while
allowing Viewer to reinterpret measurements after import. Ordinary consumers keep
using the existing attribute and sensor/measurement names. Conversion is a thin,
calculated representation over retained source values.

The motivating workflow is approximately a hundred recordings whose legacy gyro
correction can be enabled or disabled after import, including in a batch, without
losing descriptions, markers, organization, or other session edits.

Implement a coherent core with explicit ownership and testable invariants. Do not
merely add correction-specific exceptions to the existing cache and merge paths.
Keep existing mathematical algorithms and familiar public names wherever possible.

This plan incorporates the discussion after [measurement-source.md](measurement-source.md).
Where they differ, this plan describes the intended implementation direction:

- Preserve the **complete imported attribute map for each source**, including
  unknown attributes and imported descriptions or markers. A metadata whitelist
  is insufficient when arbitrary attributes can differ between sources.
- Associate each source measurement with its source. Mixed schemas are supported
  through that association; do not reconcile all measurements into one schema.
- Give a calculation an identity independent of its output names. Cache one
  result per calculation per session and expose its fields as ordinary values.
- Calculations return results; the evaluator owns publication and caching.
- Use one authoritative dependency mechanism and one persistent mutation path.
- Keep execution policy separate from calculation definition and result access.
  A future explicit job queue for expensive calculations is **out of scope**.

### In scope

- Source storage, complete source attributes, source associations, session edits,
  and explicit source access without renaming columns.
- Source unit preservation and calculated normalization/correction.
- A calculation evaluator with shared multi-output results and reliable
  dependency tracking, including missing inputs and fallback selection.
- Migration of existing built-in recipes and the supported plugin interface.
- Source-preserving session persistence, including mixed-source metadata.
- Correct merge, edit, notification, logbook-index, and save behavior.
- A session correction control, batch editing, and an isolated import default.
- Focused automated coverage, integration validation, and final documentation.

### Out of scope

- Implementing sensor fusion, changing its numerical algorithms, or running its
  experiments. Use small artificial calculations to test expensive-work sharing.
- A job queue, worker pool, asynchronous calculation framework, job UI, or
  automatic scheduling/retry policy for expensive calculations.
- Arbitrary representation priorities, a transformation language, or a general
  expression language.
- Corrected-data export, persistent calculated-vector caches, correction history,
  or a general source-version archive.
- Byte-identical file archival, original record ordering, and original numeric
  spelling. See the semantic preservation contract below.
- Reconstructing original data from the unreleased import-time-correction
  prototype. Never divide modified recordings to manufacture source fixtures.
- Firmware changes, broad numerical cleanup, or unrelated UI redesigns.

## 2. Repository baseline to verify

The working tree contains local changes. Reinspect files when starting a phase;
do not assume the implementation is identical to the review snapshot.

| Area | Current behavior relevant to this work |
| --- | --- |
| `src/sessiondata.{h,cpp}` | Stored attributes and measurement vectors precede calculations. Units are a separate mutable map. Enumeration describes stored data. |
| `src/dataimporter.{h,cpp}` | The local prototype rescales gyro values, normalizes units in place, and stamps schema 2. Parsing appends directly through friendship. |
| `src/calculatedvalue.{h,cpp}` | Recipes are globally registered, ordered, and cached per session. Only a successful recipe's declared dependencies are registered. `setValue()` also records side effects. |
| `src/dependencykey.h`, `src/dependencymanager.*` | Only attribute and measurement identities exist. Reverse edges drive transitive invalidation. |
| `src/calculations/` | Time-fit, simplified-track, analysis-range, flare, WS-P, and SP calculations set several caches from one callback. Other recipes mostly return one value. |
| `src/sessiondata.cpp` | Interpolation expressions have a special computation/cache/dependency path. |
| `src/dataexporter.cpp` | Enumerates stored keys but reads ordinary getters. Numeric rows use `QByteArray::number(..., 'g', 15)`. |
| `src/sessionmodel.cpp` | Merging **does overwrite matching measurement keys**, omits units, and replaces unloaded stubs rather than first restoring their full state. |
| `src/sessionmodel.cpp` | Bulk edits use row/column indices, omit fine-grained dependency notifications, and several save paths ignore failure before clearing dirty state. |
| `src/logbookmanager.cpp` | Saved column values live in `index.json`; its prototype `dataSchemaVersion` is not a sufficient calculation-validity contract. |
| `src/sessiondata_bindings.cpp`, `src/pluginhost.cpp`, `python_plugins/flysight_plugin_sdk.py` | Ordinary reads and one-output recipes exist. Python also exposes `setCalculatedMeasurement()` for side effects. Non-attribute dependency kinds are currently treated as measurements. |
| `tests/` | The standalone Qt target exercises the importer/exporter/prototype preferences. It does not currently cover the full model, built-in calculations, or Python integration. |

Relevant background: [sensor-fusion.md](sensor-fusion.md),
[`docs/DATA_SCHEMA.md`](../docs/DATA_SCHEMA.md), and
[`TEMP/firmware-changes.md`](../TEMP/firmware-changes.md), where present. The latter
documents may describe the superseded prototype. Their import-time storage
behavior is not the contract for this implementation.

## 3. Architectural contracts

The contracts below are required behavior. C++ class names, container choices,
member layouts, and helper signatures are illustrative unless explicitly stated.
Phase 0 establishes the small shared interface contract before agents implement
dependent phases. Do not independently invent incompatible versions of it.

### 3.1 One authoritative home for each persistent fact

The persistent model contains:

1. **Source records.** Each accepted input retains its complete imported attribute
   map under the original keys. Sources have stable local identities that survive
   save/reload. Sources with equal schema or firmware are not automatically the
   same source.
2. **Source measurements.** A logical `(sensor, measurement)` key selects one
   current source series, its recorded unit, and its source association. The
   association is per measurement even when many measurements share one source.
3. **Session state.** Session identity, subsequent edits, interpretation choices,
   and explicit overrides/suppressions of inherited attribute values.

Measurements refer to a shared source record; do not copy an attribute bag into
every measurement. Do not also maintain a second editable copy of every source
attribute in a flat session map. Reuse existing value/container types where useful.

For example, an imported `_DESCRIPTION = "Test 1"` remains in its source record
after the user changes the current description to `"Good recording"`. The second
value is a session override. No synchronization between the two is needed.

"Immutable source" means calculation and interpretation cannot change it.
Explicit source addition/replacement through a validated mutation is allowed.
Replacing a current series does not require archiving the previous sample buffer.
Retaining accepted source attribute records is not a correction-history system.

### 3.2 Attribute resolution and removal

Retaining source attributes introduces inheritance that the old flat map did not
have. Define it deliberately, including deletion; do not leave this to callers.

Recommended compatibility behavior to finalize in Phase 0:

- An explicit session override wins.
- Otherwise, an ordinary imported attribute uses the selected source value.
  Retain the existing incoming-value precedence using a persisted source order
  or explicit source selection, rather than copying the winning value. Both
  original source values remain available through source access.
- If there is no applicable stored value, try calculated defaults/recipes.
- Recorded-source access bypasses session overrides and returns the original
  imported value or absence.
- Session-level reads of imported metadata follow the same selection rule. They
  expose a selected recorded value, not a claim that every measurement shares
  that fact. In particular, ordinary `SCHEMA_VER` selection must never determine
  a measurement's interpretation. Use its associated source instead. A caller
  needing a common-value/conflict summary can derive one explicitly from the
  current measurements' sources; no additional persistent summary is needed.

Removal needs two distinguishable operations:

- Clearing an override to inherit the original source value again.
- Removing the current stored value so a calculated default becomes applicable.

Preserve the intended behavior of existing reset/remove callers. If an imported
value must be hidden to restore a calculated default, use a persisted suppression
marker/tombstone in session state; do not delete the source attribute. Setting a
new override clears that suppression. A suppression must continue to work after
reload and after another source containing the same key is merged.

Phase 0 must write and test a resolution table for absent values, source
conflicts, overrides, suppression, and defaults. This is a contract decision, not
a request to add a general configuration/inheritance framework or new UI.

### 3.3 Enumeration and source access

Keep native names. There are no imported `wx_raw` or `source:wx` columns.

Provide explicit source measurement, source unit, and recorded source attribute
access. Accessors or typed references are sufficient; no new string syntax is
needed. Source measurement access never falls back to a derived result.

Preserve the distinction between stored presence and computability:

- `sensorKeys()`, `measurementKeys()`, and `hasMeasurement()` describe current
  stored source series. They do not enumerate calculation outputs because a
  calculation happened to run.
- Session attribute enumeration/presence describes applicable persistent values,
  incorporating source selection, overrides, and suppression. Calculated
  defaults and caches do not become stored attributes.
- Provide separate explicit enumeration of each source's recorded attributes.
- The writer traverses the persistent model directly. It must not infer what to
  save from compatibility enumeration plus ordinary calculated getters.

If compatibility names become misleading, add explicit helpers and document the
old names' meaning. Audit callers before changing their semantics. Presence tests
performed during calculation are tracked reads, including negative results.

### 3.4 Fixed measurement resolution

An ordinary measurement request resolves as follows:

1. If a source series exists, use its source-to-effective calculation.
2. Otherwise, try the existing derived calculation candidates in their declared
   order.

Caching accelerates the chosen path; cache contents never select precedence.
Source conversion failure does not cause fallback to an unconverted source or
to a derived provider for the same key. An empty but present source series must
remain distinguishable internally from absent source input.

An explicitly supplied `wTotal` remains a supplied measurement and wins over its
derived recipe. An absent `wTotal` is derived from effective gyro axes. The
correction in this change affects only `wx`, `wy`, and `wz`; do not silently
recompute or rescale arbitrary imported derived columns.

Use one generic source-normalization mechanism. Do not add a session-specific
global recipe for every imported column. Unit conversion and applicable schema
correction can be combined in one pass with one effective result buffer.

### 3.5 Schema and interpretation policy

Keep `SCHEMA_VER`. It is not replaced by patch-specific flags.

| Recorded schema of a measurement's source | Gyro correction enabled | Effective gyro behavior |
| --- | --- | --- |
| Missing or `1` | Yes | Multiply recorded gyro rates by `1.14688`, with normal unit normalization. |
| Missing or `1` | No | Retain recorded gyro scale, with normal unit normalization. |
| `2` | Either | Normalize units only; no legacy multiplier or inverse multiplier. |

The ratio is `0.070 / (2000 / 32768) = 1.14688`. It applies only to `IMU.wx`,
`IMU.wy`, and `IMU.wz`. Do not infer schema from firmware cutoffs, filenames,
dates, motion, or numeric ordering of unknown schema versions.

Apply the missing-schema default **per source**. Keep the recorded marker absent
and obtain interpreted schema 1 through a default calculation/helper participating
in dependency tracking. Explicitly malformed markers are not missing markers.

For this implementation, reject unsupported/malformed source schemas during
import without modifying an existing session. Backend interpretation of an
unsupported source must also fail explicitly, never return identity by accident.
Partial interpretation of unknown future schemas is not required.

Use a session attribute for correction enablement; `_CORRECT_LEGACY_GYRO` is a
recommended name, to be finalized with the UI wording in Phase 0. Its calculated
default is a constant enabled value. New-session initialization snapshots the
Import preference unless a valid saved choice already exists. Reloading, merging
an additional source, or changing global preferences must not replace that choice.

The prototype's Automatic/Treat-as-legacy/Treat-as-corrected preference is not the
same control. Do not reinterpret its meanings silently or edit recorded schema
to emulate it. An advanced per-source interpretation override for mislabelled
custom files can be designed separately; its UI and a general override framework
are not required in this work. Keep any later override distinct from recorded
source attributes and ordinary correction enablement.

### 3.6 Unit and numerical contracts

- Retain parsed source samples and the recorded unit labels.
- Effective acceleration is in `m/s^2` (`1 g` becomes `9.80665 m/s^2`).
- Effective magnetic field is in tesla (gauss is multiplied by `0.0001`).
- Gyro rates remain in `deg/s`; do not introduce radians/s as an incidental change.
- Preserve Viewer's existing angle and temperature conventions and its separate
  display-unit conversion layer.
- Disabling the gyro correction never disables unit normalization.
- Source and effective unit access must agree with their respective samples.
- Recognize already-normalized units, including the canonical labels emitted by
  existing Viewer saves. Distinguish supported identity from unsupported conversion.
- Opaque custom fields can retain an unknown unit and unchanged values. Known
  physical quantities must not be represented as successfully normalized when
  their required unit conversion is unsupported.

Define transformation order explicitly: for this correction, correct the legacy
source scale and then normalize units. Do not derive order from priority numbers.

The preservation guarantee is semantic: parsed doubles, supported timestamp
precision, labels, recorded units, complete imported attribute maps, source
associations, and session edits round-trip. Use enough precision to round-trip
stored doubles, including the actual bulk numeric writer; changing only
`QTextStream` precision is insufficient.

The current ISO timestamp parser reduces dates to milliseconds. Preserve that
supported precision and document it. Do not claim restoration of precision lost
during parsing or recovery of rounding/clipping in the recording. Define handling
of non-finite values explicitly rather than silently converting them to zero.

### 3.7 Calculation definitions, outputs, and cache ownership

Represent these three concepts independently:

```text
Calculation definition: definition ID -> function producing a result
Output binding:         attribute/measurement key -> calculation instance + field
Session result cache:   instance key -> immutable result + dependencies/state
```

A fixed calculation such as time-fit needs only its definition ID as its instance
key. A generic calculation also includes its immutable request parameters: source
normalization for `IMU.wx` and `IMU.wy` must have distinct instances, as must two
different interpolation expressions. Register the generic definition once; do
not register a new global function per session or imported column. These instance
parameters select the request, while changing source associations, samples, and
policy remain tracked inputs that invalidate its result.

A calculation may return one scalar, one vector, or a result containing several
outputs. A typed result with field bindings, or a validated result bundle with
declared output slots, is acceptable. Do not force arbitrary results into fake
imported attributes, JSON strings, or synthetic source sensors.

For example, one time-fit calculation returns `{a, b}`; the existing coefficient
attributes select those fields. One simplified-track calculation returns all
four vectors; their existing measurement names select the fields.

Required properties:

- Global registrations contain definitions, not captured per-session values.
- Calculation identity is stable for its definition and independent of which
  output is requested first. Alternative providers have distinct identities.
- Results are owned and cached once per session and calculation instance.
  Bindings are lightweight projections and do not duplicate all sample buffers.
- Calculations receive read access and return results. They cannot mutate stored
  input or write arbitrary caches. Publish each returned result atomically.
- Existing one-output registration can remain a convenience adapter over the
  same mechanism, not a second evaluator.
- Partial availability is explicit: a multi-output result can provide diagnostics
  while other outputs are unavailable. Never leave fields from an earlier run
  mixed with fields from a new run.
- Unavailable/failed evaluations retain enough dependency information to become
  eligible after relevant input changes. Reading another output must not cause
  an identical failed computation to run repeatedly.
- No independent cache or execution rules in source normalization, interpolation,
  plugins, or multi-output helpers.

Cache lifetime remains bounded by the loaded session. Once-per-result means once
while the relevant result is retained and valid, not once forever across eviction
or process restarts. Do not build a new eviction framework in this change.

### 3.8 One authoritative dependency mechanism

Use a read context that records actual reads during evaluation. This is the
recommended architecture for the existing optional inputs and synthesized
references. Explicit plugin input lists can remain prerequisites/documentation,
but must not be a competing, incomplete source of invalidation truth.

Track typed identities for at least the following concepts:

- Persistent session attributes/overrides and their presence/suppression.
- A source measurement's samples, unit, association, and presence.
- Recorded source attributes and their presence.
- Logical effective attributes/measurements.
- Calculation results and the provider bindings used to resolve public outputs.
- Source ordering/selection or membership when a query depends on it.

These are conceptual identities, not a requirement for one enum case per bullet.
For example, samples and unit metadata can share one source-series identity if
both mutations invalidate it. No per-sample dependency graph is needed.

The evaluator must:

- Record dependencies on cache hits as well as newly calculated values.
- Record failed lookups and negative presence tests.
- Retain the reads involved in unsuccessful earlier candidates when a fallback
  wins; changing those inputs may change which candidate should win.
- Track source absence when choosing a derived provider, so adding stored source
  invalidates a previously cached derived result under the same public key.
- Use a separate evaluation scope/stack for each nested calculation, including
  cross-type calls. No shared mutable side-effect list.
- Detect cycles and unwind cleanly after unavailable results and exceptions.
- Replace obsolete dependency edges after recalculation, rather than allowing
  them to grow indefinitely or omit newly observed inputs.
- Make registration/unregistration changes visible to affected session caches.
  A registry revision with conservative invalidation is acceptable for rare
  registry changes; one session's cache must not outlive a removed global recipe.

All mutable inputs affecting a result must enter through tracked reads. Audit
direct preference reads and capture import defaults in session state where that
is their intended meaning. Do not attempt to intercept arbitrary C++ globals.

### 3.9 Execution policy and future jobs

Keep current ordinary calculations synchronous/on-demand during this work.
Separate the evaluator's explicit evaluate/request operation from definition,
output binding, and inspection of an already available result.

A future queue should be able to run a calculation instance by its key and publish
one result for all its outputs. Ordinary output access for such a future
calculation should be able to report availability without initiating work.
Invalidating a result must not inherently enqueue a new expensive calculation.

Do not implement asynchronous states, futures, worker threads, queues, or fusion
here. Do not use their future needs to expand this into a general task framework.
The separation of responsibilities is the compatibility provision being made now.

### 3.10 Persistent mutation, merge, and notifications

Use a single operation for applying persistent edits to an existing session.
It validates and applies related changes together, invalidates calculations,
reports affected logical values, marks the session dirty, and notifies consumers.
Callers must not individually assemble a sequence of raw setters and cache clears.

Merge is an operation on persistent input, never effective getters or caches.
Recommended default: preserve current replacement of incoming matching series,
but transfer samples, units, source association, and required sample-axis context
together. Preserve unmatched series. An add-missing-only mode, if needed by a
caller, must be an explicit policy rather than incidental map behavior.

Validate sample/time alignment for measurements kept together in a sensor. Equal
vector lengths alone do not prove alignment. Reject incompatible partial merges
or replace a coherent block; do not silently splice unrelated sample grids.
Defining how to resample unrelated recordings is outside scope.

Existing and newly loaded sessions use the same merge semantics. Restore an
unloaded row before merging; do not replace it with only the incoming file.
Preserve session edits and correction choices. Parsing an incoming file must
not apply new-session defaults to an already established session.

Batch operations use stable session IDs and attribute keys. Sorting, row deletion,
or column changes during processing must not redirect an edit to another session.
Single and batch edits produce equivalent invalidation, notification, and save
behavior. Save failure retains dirty state and must not be reported as success.

## 4. Persistence contract to settle before implementation

The existing single `$VAR,SCHEMA_VER,...` header cannot represent the required
mixed-source state. This work requires an explicit saved-session representation
of source contexts and their associations, not an invented composite schema.

Phase 0 chooses and records the encoding once. Recommended direction: one
self-contained, versioned Viewer session file containing metadata and the source
numeric payload. Reusing buffered CSV-style numeric rows is reasonable. Use a
distinct format signature so an older reader cannot simply ignore the new
metadata and interpret mixed sources as one ordinary FlySight CSV. Keep the device
CSV formats readable and do not alter the firmware `SCHEMA_VER` contract.

The saved representation must carry:

- Every retained source's complete attribute map, including marker absence.
- Current source series, exact native keys, source units, and source associations.
- Any sample-axis/context associations needed to interpret those series safely.
- Session overrides, suppression markers, identity, interpretation choice, and
  deterministic source-selection information.
- A Viewer storage-format version distinct from recorded data schemas and from
  calculated-column cache compatibility.

Do not persist effective vectors or calculation results as source. Loading a
saved session restores these records; it must not wrap the saved container as a
new source on every reload, apply current Import defaults, or grow nested source
metadata after repeated saves.

Keep a bounded compatibility reader for existing released logbook CSVs: treat
the numeric data and metadata actually present in that file as its source. Earlier
unit normalization cannot be undone to recover original device text; do not try.
Supporting these existing files is different from migrating the unreleased gyro
prototype's intermediate artifacts, which remains out of scope.

If filenames/extensions or index entries change, specify atomic write ordering
and restart recovery. Never remove the only valid saved copy before a replacement
and its lookup information are committed. Do not overwrite user recording files.
There is no requirement to add a corrected export or a new user-facing export UI.

Attribute values and names must survive the supported encoding, including unknown
keys, Unicode, commas, and escaping where applicable. Document the device `$VAR`
grammar and do not silently truncate supported values. Semantic preservation does
not require retaining duplicate header occurrences or unknown non-attribute record
types verbatim; state the parser's supported interpretation of those cases.

The index is disposable derived data. On interpretation/algorithm compatibility
changes, rebuild it without rewriting source. Per-session edits must refresh the
affected values through the model lifecycle. Never use the maximum source schema
as a universal calculation-cache version. Ensure a failed save or interrupted
index update cannot leave durable cached columns presented as matching a different
saved session state; a revision check or conservative invalidation is acceptable.

## 5. Phase assignments and dependency graph

Each phase is a bounded assignment to one implementation agent. A phase agent
reads this whole plan, then its prerequisites' handoffs and the actual code.
Independent phases can be assigned in parallel once their common interfaces are
established; parallel work is optional, not a reason to duplicate implementations.

| Phase | Deliverable | Depends on |
| --- | --- | --- |
| 0 | Contracts, format decision, fixtures, and test targets | None |
| 1 | Persistent source/session model and attribute semantics | 0 |
| 2 | Calculation evaluator and shared result ownership | 0 |
| 3 | Native-name read facade and calculated interpretation | 1, 2 |
| 4 | Source parser and saved-session codec | 1 |
| 5 | Built-in recipe and interpolation migration | 3 |
| 6 | Plugin API integration and multi-output contract | 3 |
| 7 | Model mutation, merge, logbook, and index lifecycle | 3, 4 |
| 8 | Session correction command and import-default UI | 5, 7 |
| 9 | Complete cutover, integrated regression, and documentation | 4, 5, 6, 7, 8 |

Useful concurrency: Phases 1 and 2; Phase 4 alongside Phase 3; Phases 5 and 6
alongside Phase 7. Coordinate edits to `SessionData`, public headers, and test
CMake files. Use isolated branches/worktrees if multiple agents implement at once.
Assign one integrator for shared interfaces and the final cutover.

Do not activate ordinary effective lookup in a production save/merge path while
those paths still read ordinary getters as if they were source. New components
may be implemented and tested separately before the coordinated cutover. Avoid
temporary production flags or compatibility hacks that become permanent designs.

Every handoff includes: changed files, implemented contracts, test commands and
results, intentional compatibility changes, unresolved issues, and any temporary
adapter with the phase responsible for removing it. A phase is complete only when
its observable acceptance checks pass; compilation alone is insufficient.

## Phase 0 - Establish interfaces and independent tests

**Decisions:** [measurement-contracts.md](measurement-contracts.md) fixes shared
interfaces, format version 1, attribute resolution, source/axis identity, mutation,
correction preference and atomic index strategy. See
[measurement-handoffs/phase-0.md](measurement-handoffs/phase-0.md) for validation.

**Goal:** Make the decisions shared by later agents concrete without prescribing
every class or writing the implementation ahead of them.

**Depends on:** None.

**Primary areas:** This plan or a short linked contract note; `tests/`; root/src
build configuration only as needed for independent test targets.

**Work:**

- Verify the repository baseline and inventory actual setter, enumeration,
  calculation-registration, interpolation, and plugin call sites.
- Finalize the source/session ownership API, attribute resolution/removal table,
  stored-presence definitions, and typed identities crossing phase boundaries.
  Distinguish stable session identity from source IDs, including collision/remap
  behavior when importing another saved session's source records.
- Finalize calculation definition/result/binding/read-context interfaces, including
  parameterized instance keys, unavailable outputs, alternative providers, and
  registry/result/read-view lifetimes.
- Choose the saved-session encoding/signature and compatibility path described
  above. Write small fixtures illustrating all persisted fields, not a new generic
  file-format framework. Settle source ordering/selection and source-ID round trips.
- Finalize correction attribute/preference names and the meaning of new-session
  initialization versus additional-source import and saved-session restoration.
- Split or extend the standalone test setup so core storage/evaluation tests need
  only Qt/C++17. Plan separate model and Python integration targets where needed;
  do not force every core test to link the application's entire dependency stack.
- Preserve current prototype tests as baseline evidence, but identify assertions
  to replace at cutover, particularly schema-2 stamping and in-place scaling.

**Fixtures:**

- Unmarked, explicit-schema-1, and schema-2 sources, with reordered gyro columns.
- Compatible TRACK/SENSOR pairs in both schema combinations and import orders.
- Sources with conflicting known and unknown attributes, imported descriptions
  and markers, punctuation/Unicode values, and an absent versus explicit schema.
- Already-normalized legacy logbook input, unknown custom columns, empty series,
  malformed schema, and an incompatible sample-grid merge.
- A multi-output test calculation with a call counter, and candidate recipes that
  exercise missing inputs and fallback selection. No real fusion dependency.

**Acceptance:**

1. Existing isolated tests can run against temporary settings/files without
   touching the user's preferences, recordings, or logbook.
2. A small baseline fixture is demonstrably parsed by the current supported reader.
3. The new fixtures have independently specified expected source/metadata values
   and expected effective gyro/acceleration values.
4. The shared contract contains no unresolved ambiguity that blocks Phases 1/2/4.
   Routine type and encoding choices are the phase owner's responsibility; a
   change to the user-visible requirements must be called out rather than hidden.

**Handoff:** Public contract, chosen format examples, fixture inventory, commands,
and explicit ownership of shared headers during subsequent work.

## Phase 1 - Implement the persistent source/session model

**Goal:** Represent source input and session edits without duplicate authority or
dependency on the calculation engine.

**Depends on:** Phase 0.

**Primary areas:** `src/sessiondata.{h,cpp}` or a small underlying input-model
module; storage-focused tests. Keep parser and evaluator integration out of this
phase except for the agreed read/mutation interfaces.

**Work:**

- Implement stable source records with complete attribute maps and source series
  containing samples, units, and source references.
- Implement session overrides, suppression, source selection/order, and identity.
- Provide read-only source access and a persistent snapshot/view suitable for the
  writer. Expose recorded absence separately from calculated defaults.
- Implement controlled addition/replacement/removal operations that return changed
  input identities. Transfer series metadata as a unit; validate references and
  the agreed sample-axis constraints. Define how programmatically supplied source
  data declares its units and interpretation context; do not assume all numeric
  setters receive device values or all receive already-normalized values.
- Implement the Phase 0 attribute selection/removal table and stored-presence /
  enumeration contracts. Preserve unknown attributes without classifying them by
  a whitelist of names.
- Ensure independent sessions/snapshots cannot mutate one another accidentally
  through shared vectors or source metadata. Do not retain references to caller-
  owned temporary import buffers.

**Acceptance:**

1. Two sources can retain different values for the same arbitrary attribute.
   Selecting a session value or editing it leaves both originals unchanged.
2. Several measurements can reference one source, while measurements in the same
   sensor can reference different sources when their sample axes are compatible.
3. Overrides, suppression, restoration of inheritance, and ordinary removal have
   the specified observable behavior, including removal of an imported marker.
4. Replacing a series moves samples, units, and association together; failed
   validation leaves the original state intact.
5. Enumeration/presence excludes calculated-only values and distinguishes empty
   stored series from missing series.
6. Two session instances with shared immutable input can be edited independently.
7. Snapshots retain all source attributes, original schema absence, and session
   state needed for persistence without invoking any calculation.

**Handoff:** Input-model API, mutation change-set identities, attribute-resolution
examples, and storage tests reusable by the parser and evaluator agents.

## Phase 2 - Implement the evaluator and calculation-owned results

**Goal:** Establish one engine whose cached answers agree with fresh evaluation,
including multiple outputs and changes in input availability.

**Depends on:** Phase 0. Use the agreed abstract input/read interface so this can
proceed alongside Phase 1 with a small fixture-backed input store.

**Primary areas:** `src/calculatedvalue.*`, `src/dependencykey.h`,
`src/dependencymanager.*`, or bounded replacement modules; core evaluator tests.

**Work:**

- Implement calculation definitions, stable instance keys, typed/validated output
  bindings, and one per-session result cache keyed by calculation instance.
- Implement tracked reads, nested evaluation scopes, dependency replacement,
  cycle detection, and transitive invalidation under section 3.8.
- Preserve alternative-recipe ordering without numeric priorities. Track failed
  candidates and provider availability as part of resolution.
- Support scalar, vector, and multi-output results, including partial and wholly
  unavailable results. Publish complete immutable results through the evaluator.
- Keep cache insertion private to the evaluator. There is no general public
  setter for arbitrary calculated keys and no shared side-effect list.
- Separate explicit evaluation from inspecting an available result. Keep this
  implementation synchronous; do not add a queue or asynchronous machinery.
- Provide a test-only way to compare against fresh evaluation without cached
  answers, and a way for tests to count actual calculation invocations.

**Acceptance:**

1. A three-output calculation executes once when all outputs are requested in
   every order; repeat reads and mixed scalar/vector outputs share that result.
2. Two sessions use independent results with one global definition.
3. A relevant input edit causes one new execution on demand. An unrelated edit
   leaves the retained result reusable.
4. A missing input becoming present invalidates a cached unavailable result and
   a previously successful fallback. A preferred candidate becoming available
   changes the selected result on the next request.
5. Dependencies recorded through cache hits behave identically to cold reads.
6. Conditional dependencies are replaced correctly after a branch changes.
7. Nested multi-output calculations, cross-type dependencies, cycles, and thrown
   exceptions leave no partial publication or corrupted evaluation scope.
8. A failed/partially available multi-output result is not rerun for each output;
   a subsequent relevant input change makes it eligible again.
9. Unregistering/replacing a provider invalidates affected cached results in more
   than one session. Registration never captures values from a fixture session.
10. Inspecting an available result never starts a calculation, and invalidation
    itself never performs or schedules calculation work.
11. Two parameterized instances of one generic definition have distinct results
    and dependencies, while two bindings to the same instance share its result.

**Handoff:** Evaluator API, output-binding examples, mutation/invalidation adapter
requirements, and passing counter/fallback/cycle tests. Explain how the old core
will be removed rather than left as a parallel engine.

## Phase 3 - Integrate native-name lookup and source interpretation

**Goal:** Ordinary reads use the new core and return coherent effective values
while source reads remain exact and independent of calculation.

**Depends on:** Phases 1 and 2.

**Primary areas:** `src/sessiondata.*`, `src/units/unitconversion.h`, the
calculation registry/facade, and interpretation tests.

**Work:**

- Connect the input model to tracked reads and implement the fixed public
  attribute/measurement resolution rules.
- Implement scoped source-schema defaults, source normalization, and the legacy
  gyro correction as ordinary calculation work in the new evaluator.
- Keep source/effective identities distinct without changing public column names.
- Make the conversion generic across imported columns and source units. Preserve
  normalized units and fail required unsupported conversions explicitly.
- Establish consistent source/effective unit access; avoid an independent mutable
  effective-unit map that can diverge from values.
- Add one-output registration adapters for existing recipes. These must use the
  new evaluator rather than maintain old caches in parallel.
- Connect controlled mutations to input invalidation, including source presence,
  unit changes, source association, and override/suppression changes.

**Acceptance:**

1. Legacy `wx=62.5`, `wy=-125`, `wz=0` yields `71.68`, `-143.36`, `0` with
   correction enabled, and the recorded values with it disabled. Sources do not
   change across enable/disable/re-enable sequences.
2. Schema-2 values are never corrected again or divided when correction is off.
3. Recorded `1 g` remains `1 g`; ordinary acceleration is `9.80665 m/s^2` in
   either correction mode. Gauss and identity/alias units behave consistently.
4. Missing schema remains absent in recorded access and resolves to 1 for that
   source. Changing source context through a controlled replacement invalidates
   interpretation; changing the correction choice does not change source schema.
5. Two series with different source schemas in one session each use their own
   schema. Selecting a different source's session-level attribute cannot steer
   either conversion.
6. Adding/removing a source under a key already served by a derived calculation
   changes resolution correctly, regardless of cache warm-up order.
7. Stored derived-named columns retain their precedence, and source access to an
   actually derived-only value reports absence.
8. Source absence, empty vectors, unsupported units/schemas, and required
   conversion failure follow the specified rules without hidden identity fallback.
9. Arbitrary unknown columns retain native labels and do not add duplicate global
   registrations when several sessions are imported/constructed.

**Handoff:** Working facade, conversion behavior, API compatibility notes, and
clear cutover dependencies: production parsing/writing/merging must not yet mix
old normalization assumptions with these new lookup semantics.

## Phase 4 - Preserve sources through parsing and persistence

**Goal:** Read recordings into the source model and round-trip complete saved
sessions without depending on effective lookup or the evaluator.

**Depends on:** Phase 1 and the format decision from Phase 0. This phase can run
alongside Phase 3 using source-model fixtures.

**Primary areas:** `src/dataimporter.{h,cpp}`, `src/dataexporter.{h,cpp}`, a bounded
saved-session codec if needed, and importer/codec tests.

**Work:**

- Parse each recording into a temporary, complete source record and its series.
  Preserve native labels, recorded units, and all supported imported attributes.
  Remove import-time gyro scaling, unit normalization, and schema stamping from
  the new parsing path. Do not access calculations to discover source values.
- Separate recording parsing, new-session initialization, and saved-session
  restoration. Parsing an additional source must not manufacture session edits,
  copy global defaults, or replace an established session's identity.
- Retain original FlySight and FlySight 2 readers, GNSS-only input, custom columns,
  supported timestamps, and the documented header grammar. Validate a complete
  import before publishing it; invalid schema or structural errors must not
  leave a partially modified session.
- Implement the chosen saved-session representation from section 4. Restore
  source IDs/associations, all source attributes, overrides, suppression, source
  selection, identity, and the saved interpretation choice directly.
- Implement the compatibility reader for released logbook CSVs. Their contained
  normalized samples/units become source input, without attempting to recreate
  older device values. Do not add a prototype-data migration framework.
- Make source-only writing explicit in the interface. Serialize the persistent
  snapshot, with sufficient numeric precision in the actual row-writing path.
  Reject inconsistent vectors or dangling references before indexing/writing
  them. Use transactional file replacement and propagate write/commit failures.
- Keep storage-format detection separate from device schema validation. A new
  saved-session file must not be mistaken for ordinary `$FLYS` input by an older
  reader that ignores unknown metadata.

**Acceptance:**

1. Parsing unmarked/schema-1/schema-2 fixtures retains their original numbers,
   units, labels, complete attribute maps, and explicit-versus-absent markers.
2. A mixed-source session with conflicting arbitrary attributes round-trips with
   the correct source attached to every current measurement.
3. Imported descriptions/markers, later overrides, suppression, Unicode, and
   supported punctuation survive saving and restoration without changing scope.
4. Repeated save/reload cycles do not rescale values, introduce a schema default
   into source metadata, lose double precision, or multiply/nest source records.
5. A writer test uses only the persistent model; no calculation is needed to
   serialize it. Cache-independent effective/source integration is checked again
   after Phase 3 is integrated.
6. Existing released CSV saves with canonical normalized unit labels are accepted
   and retain the values actually contained in them. Original FlySight and
   GNSS-only fixtures still load.
7. Unsupported/malformed schema, corrupt saved metadata, invalid source links,
   incompatible vector lengths, and interrupted/truncated input fail cleanly.
   Existing session state and the last successfully saved file remain intact.
8. Storage-version rejection, non-finite values, supported timestamp precision,
   and attribute escaping follow the Phase 0 format contract explicitly.

**Handoff:** Codec API, format/version documentation, representative fixture
files, compatibility boundaries, and failure results needed by Phase 7. Do not
activate a partially converted application persistence path independently.

## Phase 5 - Migrate built-in calculations and interpolation

**Goal:** All built-in values use the same pure calculation/result model while
preserving their established names and numerical behavior.

**Depends on:** Phase 3.

**Primary areas:** `src/calculations/`, interpolation in `src/sessiondata.cpp`,
registration/startup code, and built-in regression tests.

**Work:**

- Move existing one-output recipes to the read-context interface or its adapter.
  Check their actual reads, optional inputs, and alternate recipes. Preserve
  existing formulas and intentional precedence.
- Replace side-effect publication in time-fit, simplified-track, analysis-range,
  flare, WS-P, and SP helpers with calculation-owned result bundles. Bind all
  existing public output keys to the appropriate result fields. A group should
  reflect one actual computation; do not combine unrelated calculations merely
  because they produce attributes of the same type.
- Preserve partial results and diagnostics. A user override of one output must
  coexist with the calculation's remaining outputs. When an algorithm uses that
  overridden value as an input, read it through the public resolution contract
  rather than bypassing it by selecting a private result field. Keep a producer
  separate from a downstream calculation that consumes its resolved output, so
  that a group never reads its own unresolved public output and creates a cycle.
  The SP window-start group and SP scoring group illustrate this separation.
- Implement synthesized interpolation as an ordinary parameterized/synthesized
  calculation using the same evaluator and tracked reads. Preserve the existing
  `{timeAttribute}:{sensor}/{timeVector}/{measurement}` syntax and its supported
  parsing behavior; no representation prefixes or new expression language.
- Audit preferences and other mutable values read inside recipes. For the
  existing descent pause-timeout preference, for example, explicitly retain
  its intended live/default behavior and provide tracked invalidation, or use a
  session snapshot if that is the intended established setting semantics. Do
  not silently change when a preference takes effect to make tracking easier.
- Where interpretation depends on recorded facts, use the source associated with
  the relevant measurement. Ordinary session attribute selection is appropriate
  for session edits/settings, but cannot establish the source schema or other
  source-specific calibration facts.
- Remove helper calls that write neighboring caches and interpolation's separate
  dependency registration. Keep solver algorithms and display conversions intact.

**Acceptance:**

1. Representative GNSS, IMU, barometer, time, and analysis outputs match fixed
   baseline expectations with the intended source correction accounted for.
   Expectations must not be calculated by calling the implementation under test.
2. Requesting each time-fit/simplified-track group in different output orders
   performs one underlying computation while its result remains valid.
3. Missing/partial WS-P and SP results retain available diagnostic fields without
   stale fields from a prior successful run or repeated work for every output.
4. Correction toggles update derived `wTotal` and an interpolated gyro attribute;
   source vectors and unrelated outputs remain unchanged. A supplied `wTotal`
   retains the stored-value precedence specified in section 3.4.
5. Adding a previously missing preferred input, such as a GNSS timestamp for a
   start-time fallback, changes the selected value after invalidation.
6. Marker overrides, clearing an override, and suppressing an imported marker
   produce the agreed defaults and downstream recalculation. Partial output
   overrides do not cause cycles or prevent access to other group outputs.
7. Nested groups and interpolation use the core dependency/cycle tests' behavior;
   there is no second cache path or hidden source mutation.
8. Changing an audited live preference invalidates affected cached values, while
   changing a new-import default does not reinterpret an established session.

**Handoff:** Migrated recipe/group inventory, any deliberate numerical or
compatibility differences, regression fixtures, and confirmation that built-ins
no longer require public calculated-cache setters. No fusion work is included.

## Phase 6 - Integrate plugins with the same calculation model

**Goal:** Keep ordinary plugins simple and give calculations with several outputs
an explicit return-value contract without restoring arbitrary cache mutation.

**Depends on:** Phase 3. Coordinate public-header changes with Phase 5.

**Primary areas:** `src/sessiondata_bindings.cpp`,
`src/dependencykey_bindings.cpp`, `src/pluginhost.{h,cpp}`,
`python_plugins/flysight_plugin_sdk.py`, plugin examples, and bridge tests.

**Work:**

- Preserve ordinary attribute/measurement reads and native public names through
  a tracked, read-only calculation view. Add explicit source access with the same
  recorded/effective distinction as C++; do not double-normalize plugin results.
- Retain `AttributePlugin` and `MeasurementPlugin` as one-output adapters. Keep
  their declared inputs as tracked prerequisites where currently required, while
  actual reads determine dependencies too. Missing prerequisites must themselves
  be recorded so a previously unavailable plugin becomes eligible later.
- Add a small multi-output registration/return API using a calculation identity
  and declared output bindings. One callback returns one validated result bundle.
  Reuse the core evaluator; do not cache Python groups in a parallel registry of
  per-session results. Supply a short example with two vectors and a diagnostic.
- Decode dependency kinds explicitly. New source/dependency kinds must not fall
  through to the existing generic measurement case, and invalid keys should be
  rejected with a useful error.
- Audit the exposed `setCalculatedMeasurement()` contract and actual consumers.
  The destination architecture has no general cache writer. A temporary adapter
  may collect outputs inside one declared calculation's return transaction, if
  this is bounded and unambiguous; it must not publish directly into arbitrary
  keys. Otherwise provide an explicit migration to the multi-output API and a
  clear error for unsupported side-effect use. Do not claim full compatibility
  with untracked cache mutation or preserve a second evaluator to emulate it.
- Make array ownership and error cleanup explicit at the Python/C++ boundary.
  Returned data must remain valid after the Python callback completes, and a
  Python exception or invalid output must not partly publish a result.

**Acceptance:**

1. Representative existing one-output attribute and measurement plugins work
   through the actual C++ bridge with unchanged ordinary read semantics.
2. A counter-based multi-output Python plugin runs once across all of its output
   reads and once again after a relevant edit; two sessions have separate results.
3. Source samples/units/schema remain accessible independently of corrected
   ordinary reads, and source/negative-presence reads invalidate correctly.
4. Declared prerequisites, cache hits, and inputs actually read inside a plugin
   have the same invalidation behavior as native calculations.
5. Invalid dependency kinds, malformed arrays, missing outputs, exceptions, and
   attempted unsupported cache writes fail without partial publication or a
   corrupted evaluation scope. Verify that returned array lifetimes are safe.
6. Provider replacement/unregistration cannot leave another session using a stale
   Python result or a callable whose lifetime has ended.

**Handoff:** Supported SDK compatibility contract, multi-output example, source
access examples, bridge test commands/results, and any explicit migration note.
Do not add background Python execution or asynchronous plugin scheduling.

## Phase 7 - Unify model mutation, merge, and logbook persistence

**Goal:** Make loaded, unloaded, single-session, and batch operations obey the
same persistent-state and invalidation rules, including failure handling.

**Depends on:** Phases 3 and 4. Test with small registered recipes so this phase
does not wait for every built-in migration.

**Primary areas:** `src/sessionmodel.{h,cpp}`, `src/logbookmanager.{h,cpp}`,
existing edit/save worker integration, and model/logbook tests.

**Work:**

- Route ordinary edits, removal/reset, table edits, batch changes, and source
  merges through the agreed validated mutation operation. Carry changed input
  identities through invalidation, `dependencyChanged`/model notifications,
  calculated-column refresh, dirty tracking, and saving.
- Merge persistent records instead of reading effective measurements. Transfer
  units, source associations, and sample-axis context with replacement series.
  Preserve unmatched series, complete source attribute maps, session edits,
  suppression, and correction choices under the Phase 0 precedence table.
- Restore an unloaded session before mutation/merge. A failed restore must not
  be replaced by a new empty session and then saved over the existing recording.
  Validate incompatible partial sensor merges before committing any part of them.
- Use stable session IDs and attribute keys in queued edits. Support editing an
  attribute without requiring a visible logbook column. Resolve current rows only
  when notifying views; deleted sessions must be skipped/reported, never redirect
  the queued edit to the next row.
- Use the source-preserving codec for logbook saves and reloads. Preserve saved
  choices on restoration and snapshot the import default only when a genuinely
  new session is established. Merging an additional parsed source is distinct
  from restoring or explicitly importing another saved Viewer session.
- Honor persistence results in ordinary saving, flushing, eviction, and bulk
  editing. Failed writes retain dirty state and the valid saved copy; eviction
  must not discard the only unsaved state. Report failures through an appropriate
  existing error path without creating an endless idle-worker retry loop.
- Give `index.json` derived-column validity an explicit contract independent of
  `SCHEMA_VER`. Invalidate/rebuild on algorithm/interpretation compatibility changes
  and refresh after relevant session edits. Check correspondence with committed
  session state, or conservatively discard cache entries when it is uncertain.
- Handle the file/index commit ordering selected in Phase 0, including restart
  recovery. Cancellation, deleted rows, and failed batch items must leave clear
  results; do not report unapplied or unsaved changes as durably completed.

**Acceptance:**

1. Matching measurement keys follow the explicit replacement policy; samples,
   units, and source references move together. Unmatched inputs survive. The
   result is identical for a loaded row and an initially unloaded row.
2. TRACK/SENSOR imports in both orders and both schema combinations interpret
   each gyro series using its own source. Conflicting arbitrary attributes remain
   available per source, while ordinary attributes follow the selected precedence.
3. User descriptions, imported-marker suppression, correction choices, and other
   session edits survive merge/save/reload. Incoming new-import defaults do not
   overwrite them. Incompatible sample grids fail without partial mutation.
4. Single and batch edits invalidate the same derived/interpolated values and
   emit the affected logical keys needed to refresh consumers and index columns.
5. A batch over approximately 100 sessions exceeds the configured loaded-session
   capacity, changes every intended ID, and survives eviction and restart.
   Sorting, row removal, or column reconfiguration during processing cannot
   change the identity or attribute targeted by queued work.
6. Injected load/write/commit failures preserve original saved data and unsaved
   edits. Dirty state is cleared only after a successful commit, and failed items
   are distinguishable from completed/cancelled items.
7. A simulated interruption between session-file and index updates causes cache
   rebuilding or rejection of stale columns after restart, not presentation of
   an old calculated value as valid for different source/policy state.
8. Changing calculation compatibility invalidates old index values without
   rewriting source data. Both cold and warm calculations save identical
   persistent state, including schema absence and source associations.

**Handoff:** One documented mutation flow, merge/reset semantics, index validity
contract, stable-ID batch API, failure behavior, and passing temporary-logbook
integration tests. Identify the remaining production cutover work for Phase 9.

## Phase 8 - Add the session control and isolate import defaults

**Goal:** Let users change correction for selected sessions after import, with
clear batch behavior and persistent choices.

**Depends on:** Phases 5 and 7.

**Primary areas:** Existing logbook/context-menu and action code,
`src/mainwindow.cpp`, `src/preferences/importsettingspage.{h,cpp}`,
`src/preferences/preferencekeys.h`, attribute metadata/formatting as needed,
and command/preference integration tests.

**Work:**

- Add a concise command/control for selected sessions, using the existing UI
  patterns. Explicit Enable/Disable actions or an equivalent unambiguous control
  are suitable. Mixed selection is a display state, not a third persisted policy.
  Do not introduce the previously rejected general session-details panel.
- Route the action through the stable-ID/attribute-key batch API from Phase 7,
  including initially unloaded sessions. Do not require a special column to be
  visible and do not edit recorded `SCHEMA_VER` from the command.
- Replace the prototype's three-way import reinterpretation control with the
  agreed new-session correction default. Use distinct preference semantics/keys
  rather than silently treating Treat-as-corrected as Disable or rewriting schema.
  Migration of unreleased prototype preference artifacts is not a requirement.
- Apply the default only when creating a new session that lacks a saved choice.
  Define the first adoption of a released legacy logbook session consistently
  with the constant default; current global Import settings must not reinterpret
  it as if it were a newly imported recording.
- Keep normal legacy handling quiet and describe the actual operation in control
  text/help: enable or disable Viewer's legacy gyro correction; unit normalization
  always applies, and schema-2 values are unaffected. No routine plot badges,
  import warnings, or per-recording correction pop-ups.

**Acceptance:**

1. Enable/disable/re-enable works for one selected session and a mixed selection,
   including unloaded sessions, and updates affected displays through normal
   notifications. No source values or recorded metadata change.
2. Choices survive saving, eviction, restart, and later addition of another source.
   Descriptions, markers, organization, and other session state remain intact.
3. A changed Import default affects the next new recording but no existing
   session, saved-session reload, or additional-source merge. Test both enabled
   and disabled defaults, plus an explicit saved choice.
4. Schema-2 gyro values are identical in both settings; acceleration and magnetic
   normalization still work when legacy gyro correction is disabled.
5. Mixed selection is displayed honestly, empty selection is handled, and load/
   save failures use the Phase 7 failure behavior instead of apparent success.
6. Verify the actual command/preference wiring and perform a brief manual UI
   check. Ordinary legacy viewing does not introduce the rejected notifications
   or a new details panel.

**Handoff:** Final labels/preference semantics, command entry point, automated
checks and manual UI observations, and user-facing documentation requirements.

## Phase 9 - Complete cutover and verify the whole lifecycle

**Goal:** Ship one coherent implementation, remove obsolete paths, and establish
that all supported consumers use the intended representation.

**Depends on:** Phases 4, 5, 6, 7, and 8, including their transitive prerequisites.

**Primary areas:** Application wiring/build files, remaining audited callers,
`tests/`, `README.md`, `docs/DATA_SCHEMA.md`, plugin documentation, and the Viewer
sections of `TEMP/firmware-changes.md` if that handoff remains in use.

**Work:**

- Coordinate activation of the new parser, source model, evaluator, writer, and
  merge path. Remove the old cache engine, side-effect publication, import-time
  correction/schema stamping, and any temporary adapters no longer needed.
  Retain only explicitly documented compatibility adapters backed by the new core.
- Audit every ordinary/source getter, setter, presence/enumeration call, and
  dependency-kind switch. Check programmatic source construction and any
  remaining direct map/friendship mutation: units/schema context must be explicit,
  and already-effective plugin calculation outputs must not become source input.
- Verify plots, map/simplification, markers, analysis, logbook columns, and Python
  consumers obtain effective values under native names. Persistence and merge
  obtain source state independently of what consumers have evaluated.
- Run the integrated matrix below through real application-level boundaries.
  Add only the missing meaningful regression coverage; do not duplicate each
  phase's private implementation tests or introduce a new benchmark framework.
- Build the application as well as isolated tests with the supported toolchain.
  Run core tests in a configuration with assertions enabled. Exercise the actual
  Python bridge; a mocked Python-only SDK test is insufficient for ownership and
  dependency integration.
- Update current docs to distinguish recorded schema, Viewer correction policy,
  normalized effective units, and Viewer storage/cache versions. Document source
  access, mixed-source behavior, removal/reset semantics, plugin migration, the
  saved format/legacy reader, and numerical preservation limits.
- Mark superseded proposal/prototype descriptions clearly so future agents do
  not restore the old storage behavior. Keep firmware's recorded schema contract
  intact and do not modify firmware or implement the future fusion queue.

**Acceptance:**

1. The integrated tests pass, the full application builds, and ordinary supported
   recording workflows work without reliance on the obsolete engine or setters.
2. Enabling/disabling correction across saved mixed-source sessions changes only
   interpretation and the selected session policy; persistence retains the
   agreed source/session facts and stable associations.
3. There is one calculation cache/publication mechanism and one model mutation
   flow. Warm-up order, alternate providers, and multiple outputs do not alter
   correctness or create separate caches with incompatible invalidation.
4. No production save/merge call can accidentally materialize effective vectors
   as source. No recorded schema is stamped, erased, or repurposed by a toggle.
5. Remaining compatibility limits and any unavailable environment-dependent
   validation are reported explicitly. Do not mark the implementation complete
   merely because the isolated importer target passes.

**Handoff:** Changed-file summary, architecture/API notes, exact validation
commands/results, supported compatibility boundaries, and any material unresolved
issue. This phase is the final integration responsibility, not a new redesign.

## 6. Integrated regression matrix

Use deterministic, small fixtures for most coverage and a temporary-logbook batch
for the multi-session workflow. Source comparisons inspect persistent snapshots,
not effective getters. Cached-versus-fresh comparisons are useful for detecting
missed dependency edges after sequences of mutations.

| Scenario | Required observation | Main phases |
| --- | --- | --- |
| Unmarked/schema-1 gyro; enabled, disabled, re-enabled | Known effective values; exact retained source; no injected source schema | 3, 4, 8 |
| Schema-2 gyro with either policy | No multiplier or inverse multiplier; recorded schema unchanged | 3, 8 |
| Recorded g/gauss and already-normalized units | Consistent source/effective unit-value pairs; normalization independent of gyro choice | 3, 4 |
| Mixed schemas and arbitrary conflicting attributes | Each series uses its own source facts; all original attribute maps survive | 1, 4, 7 |
| Source added/replaced/removed under a derived key | Deterministic stored precedence; coherent samples/units/association; correct invalidation | 1, 2, 3, 7 |
| Empty source, missing input, and failed preferred recipe | Absence is tracked; fallback changes when new input arrives | 2, 3, 5, 6 |
| Group output order, partial output, and user override | One valid shared result; no stale fields; public override semantics preserved | 2, 5, 6 |
| Interpolated gyro, derived magnitude, and calculated columns | All update after a relevant edit; unrelated cached values remain usable | 3, 5, 7 |
| Source attribute edit overlay, removal, and reset | Original retained; inheritance/suppression/default semantics survive reload | 1, 4, 5, 7 |
| Repeated cold/warm save and reload | Stable source meaning/precision/metadata; no effective-vector persistence or nested sources | 4, 7, 9 |
| Loaded/unloaded merge and incompatible sample grids | Same merge policy; no lost unmatched data or invalid partial commit | 1, 7 |
| About 100 queued edits with sorting/eviction/restart | Every intended session ID changes once as intended; annotations and choices persist | 7, 8 |
| Failed load/save, cancelled batch, interrupted index update | No silent overwrite/loss; retained dirty state; truthful status; stale index rejected | 4, 7 |
| Global Import default change | Future new sessions use it; existing sessions and additional sources do not | 7, 8 |
| Native and Python provider replacement/unregistration | No stale result or callback lifetime; ordinary output bindings remain valid | 2, 6 |

## 7. Validation and completion rules for phase agents

- Use temporary directories, isolated settings, and generated/small source
  fixtures. Do not run mutation or migration tests against the user's logbook,
  preferences, original recordings, or local experiment results.
- Extend the existing standalone CMake test setup and document each added target
  in `tests/README.md`. Use the actual local Qt/toolchain configuration rather
  than embedding another agent's machine-specific SDK paths in source files.
- Each phase records reproducible configure/build/CTest commands and the specific
  cases it exercised. Keep core, model, and Python tests separately runnable when
  their dependencies differ. Missing optional dependencies should be identified,
  not represented as a passing test that never exercised its subject.
- Prefer observable assertions: independently specified numbers, source snapshot
  equality, invocation counters, notifications, failure state, and reload results.
  Avoid assertions about a chosen private container layout or tests that merely
  reproduce the implementation formula. Use justified tolerances for calculated
  floating-point results; do not use a loose tolerance to conceal changes to
  retained finite source doubles across the specified lossless round trip.
- After a phase's meaningful checks pass, broaden testing only for integration,
  a new change, a failure, or an unresolved concern. Full application and lifecycle
  checks remain mandatory at the final cutover.
- Leave explicit handoffs and update this plan's decision notes when Phase 0
  resolves an illustrative choice. Later agents should not need this conversation
  to discover source ownership, attribute precedence, cache behavior, or scope.

The work is complete when source data and complete source attributes have one
persistent authority; session edits have their own explicit scope; native-name
reads obtain the correct thin calculated representation; multi-output work has
one per-session result; and mutation, persistence, and all consumers obey those
same contracts. An asynchronous execution system remains separate future work.
