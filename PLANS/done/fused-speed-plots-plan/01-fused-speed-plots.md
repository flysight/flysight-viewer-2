# Phase 1: Fused speed plots

## Purpose

Publish the fused speeds beside the GNSS speeds: two on-demand calculations
(`Fusion/velH`, `Fusion/vel`) in the fusion registration, three "Sensor
fusion" plot rows (Horizontal speed, Vertical speed, Total speed) in the
specification's colours and order, and everything that counts or lists the
fusion plots brought from twelve to fifteen: the tests' mirror and the tests,
the audit's pins, the acceptance map, the test guide and the documents. It is
one phase because the specification says it is small enough for one agent in
one phase and its Commit Policy asks for one commit; there is no intermediate
state to keep green. The fit is not touched.

## Dependencies

Depends on nothing and blocks nothing. The code is as the head of
`store-requested-calculations` leaves it (`515889c`, the plot-colours
document): the plot table of `registerBuiltInPlots()` holds literal colours
with per-block "coloured by" comments, `docs/PLOT_COLOURS.md` exists and
already counts "the speeds, GNSS and fused" as one group, and nothing in the
repository names `Fusion/velH`, `Fusion/vel` or `builtin.fusion.vel`
(`git grep` finds none). The tree must be green at the end: build, the whole
suite run sequentially, `audit_cleanup`, the acceptance-map check.

## What changes

### The calculations (`src/fusion/fusionregistration.cpp`, `.h`)

Two registrations in the manner of `registerHorizontalAcceleration`
(`builtin.fusion.accH`): a descriptor with the id, the two inputs, one
output, a compute lambda that reads both inputs through the context, returns
`CalculationResult::unavailable()` when their lengths differ and otherwise
the per-sample square root of the sum of squares, and a comment that says
why it never starts the fit. The magnitude is spelled in the lambda as
`accH` spells it (`std::sqrt(accN[i]*accN[i] + accE[i]*accE[i])`); nothing is
called from `src/calculations/`, which the fusion library does not link, and
no shared header is added.

| Id | Inputs | Output |
| --- | --- | --- |
| `builtin.fusion.velH` | `Fusion/velN`, `Fusion/velE` | `Fusion/velH` |
| `builtin.fusion.vel` | `Fusion/velH`, `Fusion/velD` | `Fusion/vel` |

Both OnDemand, no title, no result version, under the `Fusion` sensor
(`kSensor`). The definitions are the GNSS ones of
`src/calculations/gnsscalculations.cpp` (`registerGnss(registry, "velH", ...)`
and `"vel"`): `velH = sqrt(velN^2 + velE^2)`, `vel = sqrt(velH^2 + velD^2)`,
with `GNSS/velH` an input of `GNSS/vel` as `Fusion/velH` is of `Fusion/vel`.

`registerFusionCalculations` registers `velH` before `vel` ("the vocabulary
before its reader"). Where the pair sits among the other on-demand
registrations is the implementer's; the two tests that pin the order and the
registration table of `docs/CALCULATIONS.md` section 17 follow the choice
(see Tests).

The header's contract comment for `registerFusionCalculations` gains the two
bullets in the registered order, worded as the `builtin.fusion.accH` bullet
is, and the sentence "Vertical acceleration is Fusion/accD itself (positive
down, like GNSS/accD) and has no calculation of its own" is extended to say
the same of Vertical speed and `Fusion/velD`.

Nothing else under `src/fusion/` changes: not the kernel, not
`Fusion::Algorithm` (`batch-temperature-bias-v9`), not the output tables
(`kFitOutputs`, twenty-one measurements and the diagnostics), not the goldens.
A kernel file in the diff fails the review.

### The plot rows (`src/mainwindow.cpp`, `registerBuiltInPlots`)

Three rows in the "Sensor fusion" block, between the Elevation row and the
Horizontal acceleration row, in this order, each with the counterpart's
name, unit and type from the "GNSS (Basic)" rows and the specification's
colour literal in the `QColor(0x......)` form the table uses:

| Name | Unit | Measurement | Type | Colour |
| --- | --- | --- | --- | --- |
| Horizontal speed | `m/s` | `Fusion` / `velH` | `speed` | `#bc378e` |
| Vertical speed | `m/s` | `Fusion` / `velD` | `vertical_speed` | `#1b8278` |
| Total speed | `m/s` | `Fusion` / `vel` | `speed` | `#d446ff` |

The block's comment ("Category: Sensor fusion. Each of the first eight rows
is named, united and typed as its GNSS counterpart and is its kin ...")
counts eleven and says what each speed is coloured by, in the words of the
specification's section 3 "Why" column, as `33daa71` wrote the per-block
comments: the rose is the cool neighbour of the GNSS red at the lower edge
(the wind-corrected speed sits on the red's warm side); the teal is the
neighbour of the GNSS green at the lower edge, a near-twin of the fused
vertical acceleration across groups, which `PLOT_COLOURS.md` section 5
accepts; the violet is the neighbour of the GNSS blue at the upper edge,
the lower side having no colour that passes and a quieter violet falling
within 15 of Speed accuracy. The measured figures (contrast, separations)
belong in that comment too, since the manual step records a moved colour's
new figures there. The table's head comment is unchanged.

### The tests' mirror (`tests/fusion/fusionsessions.h`, `.cpp`)

`fusionPlots()` gains the three rows in the application's order (name,
units, measurement, type; no colour), and its comment counts fifteen and
lists them in order. `fusionNames()` ("everything") gains
`Fusion/velH` and `Fusion/vel` beside `Fusion/accH`, and its comment says so.
Every test that iterates `fusionPlots()` or `fusionNames()` then covers the
new rows and names with no change of its own; the ones that pin a count or a
literal change as listed under Tests.

### The audit (`tests/audit/cleanup_audit.cmake`)

In the `naming` group:

- `expect_count("twelve fusion plots" "^ *\{\"Sensor fusion\", " 12 ...)`
  becomes fifteen, label included; its "Allow:" comment still says the count
  pins the application's list to `fusionPlots()`.
- The rule "the removed fusion plots stay out of the registry" drops `velD`
  from its alternation, as it already omits `accD`, and its comment says why:
  a fit channel that has a row (`accD`, `velD`) is drawn as a plot; the rest
  of the fit's own channels have none.
- The rule "the documents describe the twelve fusion plots" gains the
  pattern for twelve fusion plots beside the ones for seventeen and eight
  (the exact regular expression is the implementer's; `[Tt]welve (real
  )?(fusion )?plots|[Aa]ll twelve` is the shape of the existing eight), so
  that a stray "twelve plots" in `docs/` or the root `README.md` fails. Its
  label and comment say fifteen. Before planting it, every present count in
  `docs/` is fifteen (`grep -n twelve docs README.md` then finds nothing the
  pattern catches; "Twelve calculations" in `CALCULATIONS.md` becomes
  fourteen in the same change). `tests/README.md` stays excluded from the
  rule.
- The group's header comment lists the new items that cite it.

Elsewhere: the two header bullets that count twelve ("the sensor fusion
category is the twelve plots of the tests' mirror", items 801-863; "the
sensor fusion category has twelve plots", items 1001-1065) say fifteen; the
acceptance traceability comment lists `1601-1612` with a clause in the
style of its neighbours; the range test (`if(NOT ((item GREATER_EQUAL 1 ...`),
its violation message and a new `foreach(item RANGE 1601 1612)` block
requiring a resolving test or audit line follow the pattern of 1501-1524.
The `accuracy-channels` count of four accuracy rows is unaffected (the new
names do not end in " accuracy"); the literal-colour rule is satisfied by
the `QColor(0x...)` form.

### The acceptance map (`tests/acceptance_map.txt`)

- The header: seventeen specifications and ranges; a paragraph for
  `1601-1612` in the style of `1501-1524` ("Stated in full in
  tests/README.md, appendix Q", the items it restates); the paragraphs of
  101-120, 501-563 and 801-863 each gain "It is amended by the specification
  of 1601-1612: item(s) ... stated as amended"; the "Every item must lie in
  one of the sixteen ranges" sentence and its list include `1601-1612`.
- A block `# ---- Fused speed plots (PLANS/fused-speed-plots.md): twelve items ----`
  at the end, in the form of the 1501 block.
- Items 115, 509, 801, 842, 854, 862 and 863 restated "(as amended)": fifteen,
  the three fused speeds after Elevation "since the specification of
  1601-1612", in the wording the accuracies' restatements used ("the four
  accuracies since the specification of 1001-1065"); in 509 the outputs of
  the fit among the plots become six (vertical speed joins) and the derived
  nine. The evidence lines of 1037 and 1063 are reworded where they count
  twelve (the statements stand); 1063 follows the rename under Tests.

### The test guide (`tests/README.md`)

- Section 9.17 "Fused speed plots (items 1601-1612)" after 9.16 and appendix
  Q after P, on the pattern of 9.16 and P (section numbers, the sections
  without an item, the bullets of section 5, the items restated).
- Sections 9.2, 9.6, 9.9 and 9.11 and appendices B, F, I and K: the restated
  items say fifteen and name the fused speeds; each preamble says which items
  9.17 restates. A sentence narrating what the noise-model specification did
  ("make the real fusion plots twelve") is made plainly past or says fifteen;
  afterwards no line states twelve as the present count of fusion plots
  (`grep -n twelve tests/README.md`; "twelve minutes", "twelve failed
  starts", "twelve fixtures" are not plots and stay).
- The test table rows for the six executables under Tests and for
  `audit_cleanup` describe what the tests now hold (`tst_fusion_session`:
  fourteen registrations).
- Section 10, the `naming` paragraph: fifteen; "the velocity" becomes "the
  north and east velocity"; the document patterns include twelve.
- Section 12: "Fifteen scripts"; M39 lists the category in its new order
  ("(M58)" as it says "(M51)") and keeps its eight checks; M44's measurement
  tree and M51's Add Column group list fifteen, M51's "the other eleven"
  becomes fourteen; section 12.15 "Fused speed plots" with step M58 on
  12.14's pattern.
- The section 9 preamble ("Twelve specifications, twelve ranges ...") counts
  seventeen (Decisions).

### The documents

- `docs/COMPUTED_PLOTS.md` section 1: fifteen, the list in order; one new
  paragraph on reading a fused speed beside its GNSS speed, in the terms of
  the specification's section 1 (together where the receiver is accurate,
  apart where it is not; the separation is the fit's compromise between the
  receiver and the IMU, not an independent IMU measurement; the receiver's
  accuracy plots say which to believe), placed after the paragraph on the
  accuracy plots and before "FlySight Viewer computes them in the
  background".
- `docs/SENSOR_FUSION.md` section 2: the list in order, "Each of the first
  eleven", the three definitions in the sentence that defines each plot, and
  "The fit's other outputs (position, velocity, ...)" no longer counts the
  down velocity among the outputs without a plot; "All twelve plots" later in
  the section becomes fifteen. Section 4: `velH` and `vel` rows in the table
  "These values are derived from the outputs on demand", and the sentence
  "Vertical acceleration is `accD` itself ..." gains its twin for Vertical
  speed and `velD`. Section 8: the `tst_fusion_rows` row counts fifteen.
- `docs/CALCULATIONS.md` section 17: "Twelve calculations are registered"
  becomes fourteen with the two rows in the registered order; the "Derived
  values" paragraph gains the two definitions; "The six derived
  calculations" becomes eight; the "Plots" paragraph counts fifteen in the
  new order, says `velD`, `accD` and the four accuracies are outputs of the
  fit and the other nine on demand, "All twelve are requested" becomes
  fifteen, and `velD` leaves the list of outputs that "have no plot"; the
  test paragraph near the end says "the fifteen real plots".
- `docs/PLOT_COLOURS.md`: nothing.

## Interfaces

One phase; nothing is consumed from or provided to another phase. The names
everyone agrees on are the two calculation ids, the three rows and the
numbering above (items 1601-1612, appendix Q, section 9.17, section 12.15,
step M58).

## Acceptance criteria

Each is checkable without a judgement call; the section is the
specification's.

1. (2) `registry.instance("builtin.fusion.velH")` and `"builtin.fusion.vel"`
   exist, OnDemand, empty title and result version, inputs exactly
   {`Fusion/velN`, `Fusion/velE`} and {`Fusion/velH`, `Fusion/velD`}, one
   output each, one candidate per output, `explicitDependencies(output) ==
   {"builtin.fusion.fit"}`; `velH` precedes `vel` in `registeredIds()`;
   `Fusion/velD` has one candidate, the fit.
2. (2) On a `syntheticFitSession` with chosen `velN`, `velE`, `velD`, every
   sample of `Fusion/velH` and `Fusion/vel` equals the GNSS definition and,
   on the same samples stored under `GNSS`, equals `GNSS/velH` and `GNSS/vel`
   within `sameRecomputedValue()`; unequal lengths make both unavailable; on
   a fixture session without a fit both are `Blocked` by the fit alone,
   merely uncomputed, empty, and `runCount(fit) == 0`.
3. (2) `fusionPlots()` and the application's "Sensor fusion" block hold
   fifteen rows in the order of the specification's section 2, rows 2-4 the
   table above, each `PlotRole::Dependent` and explicit-backed by the fit
   alone; `audit naming` counts fifteen.
4. (2) Checking `Fusion/vel` alone on a visible fusable session without a
   stored fit puts one session in progress and one job, "Sensor fusion", for
   the fit; `offer()` of `builtin.fusion.vel` creates nothing; afterwards
   `Fusion/vel` has the length of `Fusion/_time` and the fit, `velH` and
   `vel` have run once each.
5. (2) After a restart with a fit stored, checking a fused speed row and
   showing the session draws all three bit for bit with no job and
   `runCount(fit) == 0`; a column over `Fusion/vel` typed `speed` fills
   loaded and unloaded rows, labelled "Total speed @ ...", in the unit
   converter's text for `speed`; a session without IMU data is
   `NotApplicable` for all fifteen.
6. (2) `Fusion::Algorithm`, the fit's 26 inputs and 22 outputs and the
   goldens are unchanged; no file under `src/fusion/` other than the
   registration pair is in the diff; the `accuracy-channels` count of four
   holds; `profileNamingRemovedPlotsAppliesWithoutThem` passes unchanged.
7. (3) The three colours are the literals `0xbc378e`, `0x1b8278`, `0xd446ff`
   in the `QColor(0x...)` form, with a comment stating what each is coloured
   by and its figures.
8. (2, 3) `formatValue(v, "speed")` and `formatValue(v, "vertical_speed")`
   equal `UnitConverter::formatValue(v, type)` in both unit systems and
   differ from the untyped text.
9. (4) `grep -n twelve docs README.md` finds no count of fusion plots;
   `CALCULATIONS.md` says fourteen calculations; the header's contract names
   both calculations and says Vertical speed is `velD` itself;
   `COMPUTED_PLOTS.md` has the reading paragraph; `SENSOR_FUSION.md` section
   4's table has `velH` and `vel`.
10. (5) Items 1601-1612 exist with resolving lines; 115, 509, 801, 842, 854,
    862 and 863 are restated "(as amended)"; `tests/README.md` has section
    9.17, appendix Q, section 12.15 with `**M58`, "Fifteen scripts";
    `audit_cleanup` and the whole suite pass, run sequentially from
    `build-agent/`.

## Tests

Function names below are proposals; the implementer may choose others, and
the map and the guide cite whatever exists (the audit checks).

- `tst_fusion_derived`: `derivedRegistrationShape` gains the two descriptors
  in its `expected[]` table and, beside "Vertical acceleration is the fit's
  own accD: no second producer", the same for `velD`; its order pin
  (`ids.mid(systemTime + 1, 3)`) follows where the pair is registered.
  `derivedValuesWaitOnTheFit` adds `"velH"` and `"vel"` to its names.
  New `fusedSpeedsKnownAnswers`: exactly representable literals (Pythagorean
  triples serve), `runCount(fit) == 0`, `undeclaredReadCount() == 0`, and
  unequal lengths making both unavailable (a short `velE` takes `velH` and
  with it `vel`; a short `velD` takes `vel` alone). New
  `fusedSpeedsAreTheGnssDefinitions`: the pattern of
  `trackAccelerationsAreTheGnssDefinitions`, the same `velN`, `velE`, `velD`
  stored as `GNSS` source data (m/s), `Fusion/velH` against `GNSS/velH` and
  `Fusion/vel` against `GNSS/vel` with `sameRecomputedValue()` (two libraries
  compile `sqrt(a*a + b*b)`, one without contraction). The file's head
  comment names the speeds.
- `tst_fusion_rows`: `allFusionPlotsAreExplicitBacked`'s literal table gains
  the three rows and `QCOMPARE(plots.size(), 12)` becomes 15. New
  `totalSpeedRowIsBlockedByFusion` on the pattern of
  `accHRowIsBlockedByFusion` (criterion 4). `removedPlotMeasurementsStayAvailable`
  keeps `"velD"` in its loop over the stored record: the assertion (the
  record carries the channel, bit for bit with what the session reads) is
  as true of a plotted channel as of an unplotted one, and the loop is the
  one bit-for-bit check of `velD` in the record. Only its comment changes,
  from "every fit channel that has no plot" to "every fit channel the fit
  publishes and no derived calculation produces, plotted (`velD`) or not".
  The comments that count twelve (the head, `allFusionPlotsAreExplicitBacked`,
  `accuracyPlotsAreAbsentWithoutAFit`, `noImuSessionIsNeverCounted`) count
  fifteen.
- `tst_fusion_session`: `registrationShape`'s literal tail becomes the
  fourteen ids in the registered order (`.mid(size - 14)`); `plotNames()`'s
  comment counts fifteen and names `velD` among the fit's own channels.
- `tst_fusion_jobs`: `noImuSessionCannotHaveAJob`'s `QCOMPARE(plots.size(), 12)`
  becomes 15; nothing else.
- `tst_fusion_store`: `capture()` adds `velH` and `vel` beside `accH` and
  `_system_time`, so `restoredAfterRestartIsBitIdentical` compares the fused
  speeds too; that test checks a fused speed row (`check("vel")`) beside
  `bodyRoll` after the restart. `accuracyColumnFillsUnloadedSessions` gains
  the data row `vel` / `speed` / "Total speed" and is renamed count-free
  (`fusionColumnFillsUnloadedSessions`, its `_data` with it, its comment
  reworded); item 1063's line and the README citations follow.
- `tst_plot_format`: new `speedTypesFormatAsTheConverterDoes` (criterion 8),
  on the pattern of `gnssAccelerationAccuracyIsAnAcceleration`, over both
  unit systems, restoring the system afterwards.
- Audit, map and guide: as under What changes. The acceptance items:

| Item | Section | Statement (summary) | Evidence |
| --- | --- | --- | --- |
| 1601 | 2 | the two calculations, their inputs, outputs, policy, order; `velD` one producer | `tst_fusion_derived::derivedRegistrationShape`; `tst_fusion_session::registrationShape` |
| 1602 | 2 | the three rows after Elevation, named, united and typed as their counterparts; fifteen in order | `tst_fusion_rows::allFusionPlotsAreExplicitBacked`; `audit naming`; `manual M58` |
| 1603 | 2 | demand: a fused speed checked creates demand for the fit as a fused acceleration does, one row alone one fit; a stored fit draws the three without computing; a column works, loaded and unloaded; none for a session without IMU data | `tst_fusion_rows::totalSpeedRowIsBlockedByFusion`, `allFusionPlotsAreExplicitBacked`; `tst_fusion_store::restoredAfterRestartIsBitIdentical`, `fusionColumnFillsUnloadedSessions`; `tst_fusion_jobs::noImuSessionCannotHaveAJob`; `manual M58` |
| 1604 | 2 | the fit unchanged (outputs, algorithm string, stored results, goldens); no wind correction, no speed accuracy; an old profile applies as before | `tst_fusion_session::registrationShape`; `tst_fusion_golden::successFixturesMatchGolden`; `tst_calculation_demand::profileNamingRemovedPlotsAppliesWithoutThem`; `audit accuracy-channels` |
| 1605 | 3 | the colours as literals with their "coloured by" comment and figures; told from their twins and the speeds group on both backgrounds | `audit naming`; `manual M58` |
| 1606 | 4 | the documents and the header's contract; every count of twelve becomes fifteen | `audit naming` |
| 1607 | 5, first bullet | known answers, unavailable before the fit and on unequal lengths, appear with the fit, start none | `tst_fusion_derived::fusedSpeedsKnownAnswers`, `fusedSpeedsAreTheGnssDefinitions`, `derivedValuesWaitOnTheFit` |
| 1608 | 5, second bullet | the rows' test: fifteen in order, the three typed and backed by the fit, formatting by type | `tst_fusion_rows::allFusionPlotsAreExplicitBacked`; `tst_plot_format::speedTypesFormatAsTheConverterDoes` |
| 1609 | 5, third bullet | demand: Total speed alone starts one fit; a stored fit draws the three without computing | `tst_fusion_rows::totalSpeedRowIsBlockedByFusion`; `tst_fusion_store::restoredAfterRestartIsBitIdentical`, `fusionColumnFillsUnloadedSessions` |
| 1610 | 5, fourth bullet | the audit: fifteen pinned with the mirror, `velD` out of the registry rule, the documents' pattern for twelve, comments and documents updated, audit and map check green | `audit naming` |
| 1611 | 5, fifth bullet | the manual step M58 | `audit naming`; `manual M58` |
| 1612 | 5, sixth bullet | items 1601-1612 with appendix Q and section 9.17; 115, 509, 801, 842, 854, 862, 863 restated | `audit naming` |

The `audit naming` line of 1606 and 1611 is a floor, not a proof: the rule
it names shows that no stale count of the plots survives in `docs/` and the
root README, not that the reading paragraph of `COMPUTED_PLOTS.md` or the
rows of `SENSOR_FUSION.md`'s table exist. Those are checked by the reviewer
reading the documents against What changes, and by M58, which is the real
check of 1611. The line is there because the map requires one, as 1414 and
1522 have theirs.

M58 (section 12.15) is the specification's section 5 manual bullet as
numbered steps in M51's idiom: 12.1's logbook copy, a fusable recording
without a stored fit, Total speed checked alone ("Computing results: 0 / 1",
one fit), the other two drawn at once, the speeds group beside the three in
both themes, the legend and measure tool, the Add Column group listing
fifteen, a column over Total speed labelled "Total speed @ <marker>" filling
loaded and unloaded rows and surviving a restart without a load. A colour
that looks wrong is moved within `PLOT_COLOURS.md`'s rule with its new
figures written into the row's comment: Michael's step, after the commit.

## Decisions

- **Twelve items, 1601-1612, in section order.** Items 1-4 are section 2
  (the calculations, the rows, demand, what is unchanged), 5 is section 3, 6
  is section 4, 7-12 are the six bullets of section 5 in order. Section 1
  has no item. Every item has a test or audit line, as the audit requires of
  a range; the manual and documentation items cite `audit naming`, which
  holds their count, as 1414 and 1522 cite their groups. No new audit group:
  the specification places every pin in `naming`.
- **The format assertion lives in `tst_plot_format`.** A legend value is
  written by type alone (`formatValue()`), so the proof has two halves: the
  rows carry the counterparts' types (the literal table of
  `allFusionPlotsAreExplicitBacked`) and the type fixes the text
  (`tst_plot_format`, which is where every other "a row's type is held to a
  format" test lives and which needs neither the fusion library nor a fit).
  Item 1608 cites both.
- **The accuracy-column test becomes the fusion-column test.** Adding a
  Total speed row to the data-driven `accuracyColumnFillsUnloadedSessions`
  is the one place that proves a column over a fusion row, loaded and
  unloaded, with the row's type and label; a second function would repeat
  it. Its name then lies, so it is renamed count-free and its three
  citations (map 1063, the README row and table) follow.
- **The restart test covers the speeds through `capture()`.** Two names
  appended to the captured values make "a stored fit draws the three without
  computing" a bit-for-bit statement after a restart, where the alternative
  was a new fit-running test.
- **M39's list is updated.** It states the category's order, which item 801
  as amended changes; a manual step that contradicts the map is a defect.
- **"Twelve specifications" in section 9's preamble becomes seventeen.** The
  line counts specifications, not plots, and was already stale at sixteen;
  the phase adds the seventeenth section to the preamble it introduces, so
  the correction is in scope and one word.
- **`velD` leaves every "has no plot" list, but not the record check.** The
  audit rule and the two documents' lists of outputs without a plot name
  `velD`; Vertical speed makes each false, and the rule's comment says why
  `accD` and `velD` are omitted. `removedPlotMeasurementsStayAvailable` is
  different: what it asserts of `velD` (the stored record carries it) stays
  true, and only its comment claimed the channel had no plot. The channel
  stays in the loop and the comment is reworded (Tests), so the record's
  `velD` keeps its bit-for-bit check.
- **Registration order stays the implementer's**, as the overview says; the
  document names what moves with it (two pins, the registration table, the
  header's list) rather than choosing.

Ready.
