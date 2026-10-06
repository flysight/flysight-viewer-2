# Implementation plan: Fused speed plots

## Feature Specification

## Fused speed plots

Date: 2026-10-05
Status: specification for planning. Not an implementation plan. Small enough
to be implemented directly by one agent in one phase.
Baseline: branch `store-requested-calculations` at the head that carries
`docs/PLOT_COLOURS.md`; the committed code is authoritative.
Related: `docs/PLOT_COLOURS.md` (how a plot is coloured), `docs/SENSOR_FUSION.md`
(section 4, the fit's model and the values derived from its outputs),
`docs/CALCULATIONS.md` (the fusion calculations and the plot list),
`docs/COMPUTED_PLOTS.md` (what the user sees), `src/mainwindow.cpp`
(`registerBuiltInPlots`, the "Sensor fusion" rows), `src/fusion/fusionregistration.h`
(the derived calculations' contract), `tests/audit/cleanup_audit.cmake`
(the `naming` group's fusion rules), `tests/acceptance_map.txt`,
`tests/README.md`. `PLANS/done/fusion-plots.md` chose the present fusion
plots; this specification adds to its section 5 and restates what it
counted.

### 1. Motivation

The "Sensor fusion" category offers the fused form of the GNSS elevation
and of the four GNSS accelerations, under the GNSS names so that the two
overlay. It does not offer the fused speeds. The earlier specification left
them out because GNSS measures velocity well and nobody had asked; both
have changed.

The fused state is pulled onto the GNSS fixes by position and velocity
factors weighted by the receiver's stated accuracies, so where the receiver
is accurate the fused speed lies on the GNSS speed. Where it is not, with
few satellites in the fix, a weak signal or a hole, the IMU carries more of
the weight and the two lines separate: the fused elevation already shows
this beside the GNSS elevation. Overlaying the speeds shows the same two
things for velocity. Where the lines coincide, the IMU is consistent with
the receiver's velocity, which validates it; where they separate, the
receiver's figure is in doubt and the fused one is the fit's estimate of
the speed with the IMU's dead reckoning in it, which is the better figure
when the receiver's own accuracy says so.

A divergence is the fit's compromise between its two sources, not an
independent IMU measurement of speed, and the documentation says so. The
user judges which line to believe beside the receiver's accuracy plots.

### 2. What is published

Three plots in the "Sensor fusion" category, each named, united and typed as
its GNSS counterpart so that the two overlay, placed after Elevation and
before Horizontal acceleration, in the order of the GNSS (Basic) category:

| Plot | Value | Unit, type | Definition |
| --- | --- | --- | --- |
| Horizontal speed | `Fusion/velH` | m/s, `speed` | the magnitude of `velN` and `velE`, as `GNSS/velH` is of the GNSS components |
| Vertical speed | `Fusion/velD` | m/s, `vertical_speed` | the fit's own output, positive down like `GNSS/velD`; no derived measurement of its own, as Vertical acceleration is `accD` itself |
| Total speed | `Fusion/vel` | m/s, `speed` | the magnitude of `velH` and `velD`, as `GNSS/vel` |

`velH` and `vel` are on-demand calculations under the `Fusion` sensor, in
the manner of `Fusion/accH`: their inputs exist only once the fit has
published, so they appear with the fit through ordinary invalidation and
never start one. Checking any of the three plots creates demand for the fit
exactly as checking a fused acceleration does; a stored fit draws them at
once; a logbook column over any of the three works as over any fusion
value. No wind correction: `GNSS/velH` and `GNSS/vel` have none, and the
wind-corrected speed is a plot of its own. No speed accuracy: that would be
a new output of the fit, and the fit is unchanged by this specification,
its outputs, its algorithm string and its stored results included.

The category therefore holds fifteen plots, in this order: Elevation,
Horizontal speed, Vertical speed, Total speed, Horizontal acceleration,
Vertical acceleration, Along-track acceleration, Cross-track acceleration,
Heading, Pitch, Roll, Heading accuracy, Tilt accuracy, Horizontal
acceleration accuracy, Vertical acceleration accuracy. Items 509, 801 and
842, which count twelve, are restated "(as amended)"; a profile that named
the plots before this change is applied as before.

### 3. Colours

The three rows are kin of their GNSS counterparts, chosen by
`docs/PLOT_COLOURS.md`: the neighbouring hue at the other edge of the
lightness band, so that the two lines separate where they overlay without
looking like different quantities, and admissible by its section 8 within
the speeds group (the three GNSS speeds, Wind-corrected horizontal speed,
Speed accuracy and the three fused speeds). The colours, as literals in the
plot table with a comment that says what each is coloured by:

| Plot | Colour | OKLCH (L, C, H) | Contrast on `#ffffff` / `#242424` | Why |
| --- | --- | --- | --- | --- |
| Horizontal speed | `#bc378e` | 0.56, 0.19, 345 | 5.15 / 3.01 | the cool neighbour of the GNSS red, which has the wind-corrected speed on its warm side; the lower edge, its twin being at the upper |
| Vertical speed | `#1b8278` | 0.55, 0.09, 185 | 4.66 / 3.33 | the teal neighbour of the GNSS green at the lower edge; a near-twin of the fused vertical acceleration across groups, which section 5 accepts, and which keeps "fused vertical" one hue |
| Total speed | `#d446ff` | 0.67, 0.27, 317 | 3.43 / 4.52 | the violet neighbour of the GNSS blue at the upper edge; the other side has no colour that passes there, and a quieter violet falls within 15 of Speed accuracy |

Measured separations (OKLab distance times 100): each fused row 17 to 22
from its twin, at least 17 from every other plot of the speeds group, and
28, 17 and 36 between the three fused rows. The colour-blind figures are
reported beside them, as section 8 now provides, and are 3 between the rose
and the teal and between the violet and its twin: the figure the inherited
red, green and blue carry, which every kin of theirs inherits.

The numbers decide admissibility; the look decides the colour. A manual
step (section 5) looks at the three as thin lines on both backgrounds
beside the speeds group, and a colour that looks wrong moves within the
rule, with its new figures recorded in the plot table's comment.

### 4. What the documentation says

- `docs/COMPUTED_PLOTS.md`: the category's count and list; one paragraph on
  reading a fused speed beside its GNSS speed, in the terms of section 1:
  the two lie together where the receiver is accurate and separate where
  it is not, the separation is the fit's compromise between the receiver
  and the IMU, and the receiver's accuracy plots say which to believe.
- `docs/SENSOR_FUSION.md` section 4: `velH` and `vel` in the table of
  values derived from the outputs, and the sentence that says Vertical
  speed is `velD` itself, beside the one that says it of `accD`.
- `docs/CALCULATIONS.md`: the two calculations in the fusion table with
  their inputs and outputs, their definitions among the derived values, the
  plot list of the category in its new order.
- `docs/PLOT_COLOURS.md`: nothing beyond what its author has written; the
  speeds group already names the fused speeds.
- `src/fusion/fusionregistration.h`: the two calculations in the contract of
  `registerFusionCalculations`, and the sentence about Vertical
  acceleration extended to Vertical speed.
- Every count of twelve fusion plots in the documents and in
  `tests/README.md` becomes fifteen: the manual steps that list the Add
  Column dialog's Sensor fusion group and the measurement tree among them.

### 5. Tests

- Known answers (`tst_fusion_derived` pattern): on a fit result with chosen
  `velN`, `velE`, `velD`, every sample of `Fusion/velH` and `Fusion/vel`
  equals the GNSS definition applied to the fused components; both
  unavailable before the fit has published and when the inputs' lengths
  differ; both appear with the fit and start none.
- The plot rows (`tst_fusion_rows` pattern): the category holds the fifteen
  in the order of section 2; the three new rows are named, united and typed
  as their GNSS counterparts and backed by the explicit calculation; a
  legend value over each formats as its GNSS counterpart's does.
- Demand: checking Total speed alone on a fusable recording without a stored
  fit starts one fit; a stored fit draws the three without computing.
- `tests/audit/cleanup_audit.cmake`, `naming` group: the pinned count of
  fusion plots becomes fifteen, with `tests/fusion/fusionsessions.cpp`'s
  list; the rule that keeps the fit's own channels out of the plot registry
  drops `velD` from its list, as it already omits `accD`, and its comment
  says why; the comments and documents that count twelve are updated. The
  audit and the acceptance-map check stay green.
- Manual, one step in `tests/README.md` section 12: the three plots in the
  list in their place; one fit for the three; the three drawn beside the
  GNSS speeds, Wind-corrected horizontal speed and Speed accuracy, in the
  light and the dark theme, each fused line told from its twin and from
  the rest; the legend and the measure tool format each as its counterpart;
  a column over Total speed fills loaded and unloaded rows.
- The acceptance items in the next free hundred of `tests/acceptance_map.txt`,
  with their appendix and section in `tests/README.md`, and items 509, 801
  and 842 restated "(as amended)".

### Commit Policy

Michael has authorized commits for this plan on the existing working branch.
Nothing is ever pushed, and nothing is ever committed on `master` or
`fusion-improvements`. One commit, subject `Fused speed plots`, with the
session's attribution line, staged by explicit path; `PLANS/` is never
staged.

## Phases

The specification says it is small enough for one agent in one phase, and
its Commit Policy asks for one commit. The plan has one phase.

| # | Name | Purpose | Depends on |
| --- | --- | --- | --- |
| 1 | Fused speed plots | the two on-demand calculations `Fusion/velH` and `Fusion/vel`, the three plot rows in their colours, the tests' mirror and the tests, the audit's pins, the acceptance items and every document and count the change touches | nothing |

Nothing blocks anything; there is no parallelism.

## Key patterns and references

Read each in full unless the line says which part.

### The calculations

- `src/fusion/fusionregistration.h`: the contract of
  `registerFusionCalculations`, the list of registrations the comment
  states (the two new calculations join it), and the sentence "Vertical
  acceleration is Fusion/accD itself" that the specification extends to
  Vertical speed.
- `src/fusion/fusionregistration.cpp`: `registerHorizontalAcceleration`
  (`builtin.fusion.accH`) is the pattern for an on-demand magnitude of two
  fit outputs: descriptor, inputs, `unavailable()` on unequal lengths, the
  comment that says why it never starts the fit; `registerFusionCalculations`
  at the end fixes the registration order ("the vocabulary before its
  reader").
- `src/calculations/gnsscalculations.cpp`, the `velH` and `vel`
  registrations: the GNSS definitions the fused ones must equal
  (`velH = sqrt(velN^2 + velE^2)`, `vel = sqrt(velH^2 + velD^2)`). The
  fusion library does not link the built-in calculations, so nothing here
  is called from `src/fusion/`.
- `docs/CALCULATIONS.md` section 17: the registration table ("Twelve
  calculations are registered, in this order"), the "Derived values"
  paragraph, the sentence "The six derived calculations", the "Plots"
  paragraph with the category's list, and the paragraph near the end that
  describes `tst_fusion_rows` "with the twelve real plots".
- `docs/SENSOR_FUSION.md` section 2 (the category's list, "All twelve
  plots"), section 4 (the table "These values are derived from the outputs
  on demand" and the sentence "Vertical acceleration is accD itself"), and
  section 8's test table row for `tst_fusion_rows`.

### The plot rows and their colours

- `src/mainwindow.cpp`, `registerBuiltInPlots`: the table's head comment
  (how a plot is coloured), the "GNSS (Basic)" rows for Horizontal,
  Vertical and Total speed (names, units, measurements, types), the
  "Sensor fusion" block's comment and twelve rows. The new rows go after
  Elevation, before Horizontal acceleration.
- `docs/PLOT_COLOURS.md` sections 4, 5, 7, 8 and 9: kin, the speeds group,
  how a fused row is coloured, how a colour is measured, how a plot is
  added. The specification's section 3 already did steps 1 to 4 and 6 and
  gives the literals; the manual step does step 5.
- `git show 33daa71 -- src/mainwindow.cpp`: how the table's per-row and
  per-block comments say what a plot is coloured by.

### The tests' mirror and the fusion tests

- `tests/fusion/fusionsessions.h` and `.cpp`: `fusionPlots()` mirrors the
  application's rows (the audit pins the two to the same count); its
  comment counts twelve. `fusionNames()` lists "everything" that a read
  must never start the fit for, `Fusion/accH` among them.
- `tests/tst_fusion_derived.cpp`: the known-answer pattern.
  `derivedRegistrationShape` pins the descriptors of the derived
  calculations and three ids after `builtin.fusion.systemTime`;
  `derivedValuesWaitOnTheFit` proves blocked, merely uncomputed, empty and
  no run; `trackAccelerationsAreTheGnssDefinitions` computes the GNSS
  calculation on the same samples as the expectation;
  `syntheticFitSession()` stores chosen fit outputs as data.
- `tests/tst_fusion_rows.cpp`: `allFusionPlotsAreExplicitBacked` holds the
  mirror to a literal table of twelve and checks every row is drawn after
  one fit; `accHRowIsBlockedByFusion` is the pattern for "one row alone
  starts one fit and is blocked by it"; `checkAllFusionPlots` and the
  comments count twelve.
- `tests/tst_fusion_session.cpp`: `registrationShape` pins the last twelve
  registered ids as a literal list (the two new ids change it);
  `plotNames()` reads `fusionPlots()` and its comment counts twelve.
- `tests/tst_fusion_jobs.cpp`, `noImuSessionCannotHaveAJob`: iterates
  `fusionPlots()`; the three new rows are covered without a change, and
  the README row that describes it counts twelve.
- `tests/tst_fusion_store.cpp`: `fusionColumnWithStoredFitsRunsNothing`
  and `restoredAfterRestartIsBitIdentical` are the patterns for "a stored
  fit draws without computing"; `accuracyColumnFillsUnloadedSessions` for
  a column over a fusion row typed as the row, loaded and unloaded.
- `tests/tst_plot_format.cpp`: `formatValue()` by measurement type is the
  one way a legend value is written; `accelerationAccuracyKeepsItsDigits`
  and `gnssAccelerationAccuracyIsAnAcceleration` show how a row's type is
  held to a format.

### The audit, the acceptance map and the test guide

- `tests/audit/cleanup_audit.cmake`: the header bullets that count twelve
  (the items 801-863 bullet and the items 1001-1065 bullet); the `naming`
  group (`expect_count("twelve fusion plots" ...)`, the rule "the removed
  fusion plots stay out of the registry" whose list names `velD`, the
  rule "the documents describe the twelve fusion plots" and its comment);
  the `accuracy-channels` group's count of four accuracy rows (unaffected,
  but read it to be sure); the acceptance traceability section's header
  comment that lists the item ranges, and the four line forms it checks.
- `tests/acceptance_map.txt`: the header (sixteen specifications, one
  paragraph per range, the amendments each range records), the "# ----"
  block header and item form of 1501-1524 at the end, and items 115, 509,
  801, 842, 854, 862 and 863 with their evidence lines.
- `tests/README.md`: the test table rows for `tst_fusion_derived`,
  `tst_fusion_rows`, `tst_fusion_session`, `tst_fusion_jobs`,
  `tst_fusion_store` and `tst_plot_format`; section 9.16 and appendix P as
  the pattern for a new section and appendix; sections 9.2, 9.6, 9.9 and
  9.11 and appendices B, F, I and K where the restated items live; section
  10's description of the `naming` group; section 12's preamble
  ("Fourteen scripts"), 12.1's preamble (the logbook copy), M39, M44 and
  M51 (the steps that count twelve), and 12.14 as the pattern for a new
  manual section; every other line `grep -n twelve tests/README.md` finds
  that counts the fusion plots.
- `docs/COMPUTED_PLOTS.md` section 1: the category's count and list, and
  the paragraphs that follow it, where the reading of a fused speed beside
  its GNSS speed belongs.
- `PLANS/done/fusion-plots.md` section 5 and `PLANS/done/noise-model-part-1.md`:
  how the present twelve were chosen and counted, for the wording of the
  restated items.

## Decisions and constraints

- **One phase, one commit.** The specification says so and its Commit
  Policy allows one commit. The phase leaves the tree green at its end;
  there is no intermediate state to keep green.
- **Registration order.** `velH` is registered before `vel`, which reads
  it ("the vocabulary before its reader", the comment in
  `registerFusionCalculations`). Where the pair sits among the other
  on-demand registrations is the implementer's; whatever the choice, the
  two tests that pin the order (`tst_fusion_session::registrationShape`,
  the whole tail as a literal; `tst_fusion_derived::derivedRegistrationShape`,
  three ids after the system time) are updated to match, and so is the
  registration table of `docs/CALCULATIONS.md` and the list in the header's
  contract.
- **The magnitude is inlined, as `accH` inlines it.** The GNSS `velH` is a
  lambda in `gnsscalculations.cpp`, which the fusion library does not link;
  `accH` already spells `sqrt(a^2 + b^2)` itself. Two square roots are not a
  fact to centralize, and the known-answer test holds the fused value to the
  GNSS calculation on the same samples, as the track accelerations' test
  does. No new shared header. Michael may prefer one; it would be a header
  in `src/calculations/` like `trackhelper.h`, called from both.
- **Every item whose statement counts the fusion plots is restated.** The
  specification names 509, 801 and 842, "which count twelve"; the map also
  counts twelve in the statements of 115, 854, 862 and 863, and the
  specification's section 4 says every count of twelve becomes fifteen.
  When the accuracies made eight twelve, every such item was restated. The
  phase restates every item whose statement counts the plots, "(as
  amended)", and leaves alone an item whose evidence column alone counts
  them (1037, 1063: the evidence text changes, the statement does not).
  Michael may confine the restatement to the three the specification
  names.
- **The documents' pattern for the old count is planted.** The `naming`
  rule that keeps old counts out of `docs` and `README.md` lists seventeen
  and eight; the phase adds the pattern for twelve fusion plots, as the
  eight was planted when it stopped being true, so that a stray "twelve
  plots" fails the audit. The exact regular expression is the implementer's;
  `tests/README.md` is excluded from that rule as before, and nothing else
  in `docs/` counts twelve of anything that the pattern would catch
  (`grep -n twelve docs README.md` is the check).
- **Numbering in the acceptance map and the test guide.** The next free
  hundred is 1601; the items are 1601 onwards, in the specification's
  section order, their count the documenter's. The test guide gains section
  9.17 and appendix Q for them, the manual step is M58 in a new section
  12.15, and section 12's preamble counts fifteen scripts. The map's header
  counts seventeen specifications and seventeen ranges, and the audit's
  traceability comment lists the new range.
- **The test mirror is the single list.** `fusionPlots()` gains the three
  rows in the application's order; every test that iterates it
  (`tst_fusion_jobs::noImuSessionCannotHaveAJob`, `tst_fusion_session`'s
  `plotNames()`, `tst_fusion_store`) covers the new rows with no change of
  its own, and the literal table in `allFusionPlotsAreExplicitBacked` and
  its count of twelve change with the mirror. `fusionNames()` ("everything
  a read never starts the fit for") gains `Fusion/velH` and `Fusion/vel`
  beside `Fusion/accH`.
- **Formatting is by type.** A legend value is written by the row's
  measurement type alone, so "formats as its GNSS counterpart's does" is
  proved by the rows carrying the counterparts' types (`speed`,
  `vertical_speed`, `speed`) and by `formatValue()` giving the same text
  for the fused row's type as for the GNSS row's. Where that assertion
  lives (`tst_fusion_rows`, as the specification's bullet groups it, or
  `tst_plot_format`) is the documenter's call, stated in the phase document.
- **The fit is untouched.** No file under `src/fusion/` other than the
  registration pair changes; the algorithm string, the stored record format
  and the goldens are as they are. A review that sees a kernel file in the
  diff rejects the phase.
- **Colours are the specification's literals.** The three hex values and
  the "coloured by" comments come from section 3. The manual step may move
  a colour within the rule; that is Michael's step after the commit, not
  the implementer's.

## Interfaces between phases

One phase; no interface between phases. The names the change fixes, for
the implementer, the reviewer and the documents to agree on:

- calculations `builtin.fusion.velH` (output `Fusion/velH`, inputs
  `Fusion/velN`, `Fusion/velE`) and `builtin.fusion.vel` (output
  `Fusion/vel`, inputs `Fusion/velH`, `Fusion/velD`), both OnDemand, no
  title, no result version, under the `Fusion` sensor;
- plot rows "Horizontal speed" (`Fusion/velH`, m/s, `speed`, `#bc378e`),
  "Vertical speed" (`Fusion/velD`, m/s, `vertical_speed`, `#1b8278`),
  "Total speed" (`Fusion/vel`, m/s, `speed`, `#d446ff`), in the "Sensor
  fusion" category after Elevation and before Horizontal acceleration;
- acceptance items 1601 onwards, appendix Q, section 9.17, manual section
  12.15 with step M58.

## Commit Policy

Michael has authorized commits for this plan on the existing working branch.
Nothing is ever pushed, and nothing is ever committed on `master` or
`fusion-improvements`. One commit, subject `Fused speed plots`, with the
session's attribution line, staged by explicit path; `PLANS/` is never
staged.
