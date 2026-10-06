# Phase 3: The plots

## Purpose

The specification's sections 5 and 6, the fourth, audit (third clause) and
manual bullets of its section 8, the user-facing parts of its section 7, and
the last bullet of section 8. After this phase the "Sensor fusion" category
holds eighteen plots: the fifteen in their order, then Horizontal accuracy,
Vertical accuracy and Speed accuracy over `Fusion/hAcc`, `Fusion/vAcc` and
`Fusion/sAcc`, each named, united and typed as its GNSS (Basic) counterpart
so that the two overlay, in the specification's colours; the tests' mirror,
the rows and column tests, the audit's counts, every count of fifteen fusion
plots, the documents, the manual step M59 and items 1710-1717 follow.

It is one phase because a row, the mirror the audit pins it to, the tests
that iterate the mirror and the counts the audit refuses in the documents
cannot be green apart. The pattern is
`PLANS/done/fused-speed-plots-plan/01-fused-speed-plots.md`, which added
three rows through the same files; the fit and the calculations are
untouched: nothing under `src/fusion/` changes.

## Dependencies

Depends on phases 1 and 2, whose documents fix what this phase may assume:
the fit publishes the twelve covariance channels under
`batch-temperature-bias-v10`, filled exactly when `headingAcc` is;
`builtin.fusion.hAcc`, `vAcc` and `sAcc` are on demand under `Fusion`, each
output with one candidate and `explicitDependencies(output) ==
{"builtin.fusion.fit"}`, unavailable when an input is empty (no fit, a fit
whose covariance failed) or the inputs differ in length; `fusionNames()`
lists the three; the `accuracy-channels` rule for the derived names already
allows `src/mainwindow.cpp`; `kAccuracies` of `tst_fusion_store` is the
sixteen; the completeness loop is `foreach(item RANGE 1701 1709)`; section
9.18 and appendix R hold items 1-9. The plot table still ends in Vertical
acceleration accuracy, `fusionPlots()` has fifteen rows and the audit pins
fifteen and four accuracy rows. Blocks nothing.

## What changes

### The plot rows (`src/mainwindow.cpp`, `registerBuiltInPlots`)

Three rows at the end of the "Sensor fusion" block, after
`{"Sensor fusion", "Vertical acceleration accuracy", ...}`, in this order,
in the `QColor(0x......)` form the table uses:

| Name | Unit | Measurement | Type | Colour |
| --- | --- | --- | --- | --- |
| Horizontal accuracy | `m` | `Fusion` / `hAcc` | `distance` | `0xc57fa6` |
| Vertical accuracy | `m` | `Fusion` / `vAcc` | `distance` | `0x08a2af` |
| Speed accuracy | `m/s` | `Fusion` / `sAcc` | `speed` | `0x8c619b` |

The name, unit and type of each are those of the "GNSS (Basic)" rows
`hAcc`, `vAcc`, `sAcc` (section 5). The block's comment: "Each of the first
eleven rows is named, united and typed as its GNSS counterpart" also covers
the last three; the paragraph on the four accuracy rows stays and a new
paragraph after it says what the three are coloured by, in the words of
section 6, as `d18c8fc` wrote the speeds' paragraph: each is the quiet
member of the fused value it qualifies (the same hue, less saturated, at the
other edge of the lightness band) and kin of its GNSS counterpart, read in
the GNSS quality group (the three GNSS accuracies, the satellite count, now
these three); the group's quiet colours cluster near grey, so no trio reaches
the kin floor of 15 against every other plot of the group and every fused
value, and the trio sits at the floor of 11 the shipped set's own accuracy
pairs sit at (fused `accDAcc` against `accD`, 11), keeping each row's story
where a trio one point better abandons the hues. The figures of section 6
(its table's "Why" column and contrast, and the measured separations and
colour-blind figures of the paragraph after it) go in that comment, where
the manual step records a moved colour's new ones. The table's head comment
is unchanged.

### The tests' mirror (`tests/fusion/fusionsessions.h`, `.cpp`)

`fusionPlots()` gains the three rows in the application's order (name, units,
measurement, type; no colour); both comments count eighteen and list them.
Nothing else in the pair changes.

### The audit (`tests/audit/cleanup_audit.cmake`)

- Group `naming`: `expect_count("fifteen fusion plots" ...)` becomes
  eighteen, label and count; the rule "the documents describe the fifteen
  fusion plots" is labelled eighteen and its pattern gains
  `[Ff]ifteen (real )?(fusion )?plots|[Aa]ll fifteen` beside the ones for
  seventeen, eight and twelve, its comment saying fifteen is a count the
  category had before. Before planting it, every count in `docs/` and the
  root `README.md` is eighteen (`grep -n fifteen docs README.md` then finds
  nothing). The group's header lists the new items that cite it.
- Group `accuracy-channels`: `expect_count("the four fusion accuracy rows"
  ...)` becomes seven, label and count; its comment says the four without a
  counterpart are the quiet member of the value each qualifies, the three
  with one the quiet member of the fused value and kin of its counterpart,
  and an eighth accuracy row raises the count with the row. The group's
  header lists the new items. The derived-names rule is not touched: the
  three rows are hits in a file it allows.
- The header bullets that count fifteen (items 801-863, 1001-1065 and
  1601-1612) say eighteen. Acceptance traceability: the loop becomes
  `foreach(item RANGE 1701 1717)` and the range comment's clause for
  1710-1717 (the rows, demand, the colours, the documents, the tests, the
  counts, the manual step, the map) replaces what phases 1 and 2 wrote.

### The acceptance map (`tests/acceptance_map.txt`)

- Items 1710-1717 appended to the 1701 block in the form of its first nine,
  statements under Tests, closing the range.
- The header: the paragraph of 1701-1717 says it amends 101-120 (115),
  501-563 (509), 801-863 (801, 802, 842, 854, 862, 863) and 1601-1612
  (1602, 1608, 1610); the paragraphs of those four ranges each gain "It is
  amended by the specification of 1701-1717: item(s) ... stated as amended".
- Items 115, 509, 801, 802, 842, 854, 862, 863, 1602, 1608 and 1610 are
  restated "(as amended)": eighteen, the three accuracies after Vertical
  acceleration accuracy "since the specification of 1701-1717", in the
  wording the speeds' restatements used; in 509 the plots derived from the
  fit's outputs become twelve (six outputs of the fit stand); in 802 "the
  first eleven" become the first eleven and the last three; 801 and 1602
  list the eighteen in order. 1606 and 1611 are not restated (Decisions).

### The test guide (`tests/README.md`)

- Section 9.18: the intro says 1710-1711 are the specification's section 5,
  1712 section 6, 1713 section 7, 1714 section 8's fourth bullet, 1715 the
  audit bullet's third clause, 1716 the manual bullet, 1717 the last bullet;
  which items it restates and that 1037 and 1063 stand with their evidence
  counting eighteen rows; the `audit naming` floor sentence of 9.17 for
  1713, 1716 and 1717; the table gains the eight rows. Appendix R gains
  items 10-17, loses its "10-17 follow" sentence, and its preamble names the
  restated items of appendices B, F, I and Q.
- Sections 9.2, 9.6, 9.9, 9.11 and 9.17 and appendices B, F, I, K and Q:
  the restated items say eighteen, each preamble says which items 9.18
  restates, 1037's and 1063's evidence text says eighteen rows. A sentence
  narrating what an earlier specification did ("whose three fused speeds
  make them fifteen") is made plainly past or continues with this one;
  afterwards no line states fifteen as the present count of fusion plots
  (`grep -n fifteen tests/README.md`; "fifteen items" is not plots).
- The test table: the rows of `tst_fusion_rows`, `tst_fusion_store`,
  `tst_fusion_jobs` and `audit_cleanup` describe what they now hold
  (eighteen; the Speed accuracy row alone; seven accuracy plots; a column
  over Speed accuracy; the three derived rows absent with the sixteen;
  "its eighteen fusion plots, the seven accuracy rows each a literal
  colour").
- Section 10: `naming` says eighteen and lists fifteen among the document
  patterns; `accuracy-channels` says seven rows whose name ends in
  " accuracy".
- Section 12: "Sixteen scripts"; M39 lists the three after the four ("and
  the three accuracy plots with GNSS counterparts (M59)") outside its eight;
  M44's tree and M51's and M58's Add Column group list eighteen; M51's "the
  other fourteen" becomes seventeen and its "ends ..." says the four follow
  Roll and M59's three follow them; section 12.16 "Fused position and speed
  accuracy" with M59 on 12.15's pattern (content under Tests).

### The documents (the same change)

- `docs/COMPUTED_PLOTS.md` section 1: eighteen, the list gaining the three
  after Vertical acceleration accuracy; the sentence on an absent accuracy
  plot stands for the seven; the paragraph on reading a fused speed beside
  its GNSS speed gains the fused Speed accuracy: set beside the receiver's,
  the two say how far each side of the compromise is trusted. Section 8's
  example "Heading, pitch and roll and the four accuracies" says seven.
- `docs/SENSOR_FUSION.md` section 2: the list in order, "Each of the first
  eleven" and the last three; the accuracy bullet says seven accuracy plots,
  the three new ones named with their units and that each overlays its GNSS
  counterpart (the four have none), absent like the four where the fit did
  not compute the covariance; the hole bullet says the fit's uncertainty
  about position and velocity grows through a hole and returns at the next
  fix, which the horizontal, vertical and speed accuracy plots show, the
  per-window widening apart (section 4's statement is phase 1's); "All fifteen plots" becomes
  eighteen. Section 8's rows for `tst_fusion_rows` and `tst_fusion_store`
  as the test table says. Each sentence on one line.
- `docs/CALCULATIONS.md` section 17, "Plots": eighteen in order with the
  three (types `distance`, `distance`, `speed`); `velD`, `accD` and the four
  accuracies outputs of the fit, the other twelve on demand; "All eighteen
  are requested"; the three unavailable, like an accuracy the fit did not
  set, where the covariance was not computed; "the fifteen real plots" in
  the tests paragraph becomes eighteen.
- `docs/PLOT_COLOURS.md` section 5: the GNSS quality group names the three
  fused accuracies. Nothing else in it changes.
- The root `README.md` counts no fusion plots; `DATA_SCHEMA.md` is phase 1's.

### What must not change

Everything under `src/fusion/` (a diff there rejects the phase), the GNSS
rows, the Plots menu, `fusionNames()`, `fusionMeasurementNames()`, the
goldens, the derived-names and covariance-names rules, phase 1's and 2's
items and their lines, the type of any existing row.

## Interfaces

Provided (contracts): the three rows of the table above, in that order after
Vertical acceleration accuracy, mirrored by `fusionPlots()`; the category of
eighteen; `naming` pinning eighteen, `accuracy-channels` seven, the
documents' pattern refusing fifteen; items 1710-1717 with the range closed;
section 12.16, step M59, section 9.18 and appendix R complete. Consumed:
what Dependencies lists. Nothing follows this phase.

## Acceptance criteria

1. **The rows** (section 5; item 1710). `fusionPlots()` and the application's
   block hold eighteen rows in the specification's order, the last three the
   table above, each `PlotRole::Dependent`, explicit-backed by the fit alone;
   `audit naming` counts eighteen, `audit accuracy-channels` seven.
2. **Demand** (section 5; item 1711). Checking all eighteen on a visible
   fusable session starts one fit and every row is drawn after it on the
   fit's time axis; checking `sAcc` alone on a session without a stored fit
   puts one session in progress and one job, "Sensor fusion", `offer()` of
   `builtin.fusion.sAcc` creates nothing, afterwards `sAcc` has the length
   of `Fusion/_time` and the fit and `sAcc` ran once each; a session without
   IMU data is `NotApplicable` for all eighteen.
3. **Absent where the covariance failed, the rest drawn** (section 5; items
   1711, 1714). The stored success rewritten without the sixteen restores
   with `hAcc`, `vAcc` and `sAcc` empty, unavailable, `NotProduced`, and the
   Roll row `Available` with nothing failed and nothing fitted again; for a
   rejected recording the seven accuracy rows are empty and `NotProduced`,
   the recording listed once, and for the success all seven have the fit's
   length.
4. **A stored fit draws them** (section 5; item 1711). After a restart with
   a fit stored, checking `sAcc` beside `bodyRoll` and `vel` and showing the
   session draws the three bit for bit with the fresh publish, no job,
   `runCount(fit) == 0`.
5. **A column** (section 5; items 1711, 1714). A column over `Fusion/sAcc`
   typed `speed` at the exit marker, made from the row, fills loaded and
   unloaded rows, labelled "Speed accuracy @ ...", its text
   `UnitConverter::formatValue(v, "speed")`.
6. **Colours** (section 6; item 1712). The three literals are `0xc57fa6`,
   `0x08a2af`, `0x8c619b` in the `QColor(0x...)` form, with the comment
   stating what each is coloured by, the floor of 11 and its figures.
7. **Documents and counts** (section 7; item 1713). `grep -n fifteen docs
   README.md` finds nothing; the documents carry what "The documents" lists;
   the audit's pattern catches "Fifteen plots" when planted.
8. **Audit, map and guide** (section 8; items 1715-1717). The rules as under
   The audit; items 1710-1717 with resolving lines; the eleven items
   restated; 9.18, R, 12.16 and `**M59` exist, "Sixteen scripts";
   `audit_cleanup` and the whole suite pass, run sequentially from
   `build-agent/`; no file under `src/fusion/`, no golden, no GNSS row and
   no existing type in the diff (section 2, second principle).

## Tests

Function names are proposals; the map and the guide cite whatever exists.

- `tst_fusion_rows`: `allFusionPlotsAreExplicitBacked`'s literal table gains
  the three and `QCOMPARE(plots.size(), 15)` becomes 18 (criteria 1, 2).
  `accuracyPlotsAreAbsentWithoutAFit`: `accuracies.size() == 7`; its
  assertions hold for the derived rows unchanged (the engine reports a value
  derived from a rejected fit `NotProduced`, as its comment at the end of
  `inspectName` says), and its comment counts seven (criterion 3). New
  `speedAccuracyRowIsBlockedByFusion` on the pattern of
  `totalSpeedRowIsBlockedByFusion`, with a `kSAcc` constant (criterion 2).
  The comments that count fifteen (the head, `noImuSessionIsNeverCounted`)
  count eighteen.
- `tst_fusion_store`: `capture()` adds `hAcc`, `vAcc`, `sAcc` beside `velH`,
  `vel`, `accH`, and `restoredAfterRestartIsBitIdentical` checks `sAcc`
  beside `vel` after the restart (criterion 4).
  `restoredFitWithoutAccuracyDrawsTheRest`: a `kDerivedAccuracies` list of
  the three, each asserted empty, unavailable and `NotProduced` after the
  reload, beside the sixteen (criterion 3); the compared count of 17 stands.
  `fusionColumnFillsUnloadedSessions_data` gains the row `sAcc` / `speed` /
  "Speed accuracy" (criterion 5); the head comment and the function's
  comment name it.
- `tst_fusion_jobs`: `noImuSessionCannotHaveAJob`'s 15 becomes 18.
- `tst_fusion_session`: `plotNames()`'s comment counts eighteen and names the
  derived accuracies among the derivations; nothing else.
- `tst_plot_format`: no new assertion. `typedValueIsTheConverterFormat`
  already holds `distance` and `speed` to the converter's text and
  `speedTypesFormatAsTheConverterDoes` holds `speed` in both unit systems;
  the rows take the GNSS rows' types, which the literal table pins, and the
  column test holds the text by type (Decisions).
- Audit, map and guide as under What changes. The items, their statements
  the contracts above in the specification's words:

| Item | Section | Statement (summary) | Lines |
| --- | --- | --- | --- |
| 1710 | 5 | the rows: Horizontal accuracy (`Fusion/hAcc`, m, `distance`), Vertical accuracy (`Fusion/vAcc`, m, `distance`), Speed accuracy (`Fusion/sAcc`, m/s, `speed`) after Vertical acceleration accuracy in the order of GNSS (Basic), each named, united and typed as its counterpart so that the two overlay; eighteen, listed in order | `tst_fusion_rows allFusionPlotsAreExplicitBacked`; `audit naming`; `audit accuracy-channels`; `manual M59` |
| 1711 | 5 | demand: requested as every fusion plot, one row alone one fit, a stored fit draws them at once, a column works loaded and unloaded, absent like the four where the covariance was not computed, none for a session without IMU data | `tst_fusion_rows speedAccuracyRowIsBlockedByFusion`, `allFusionPlotsAreExplicitBacked`, `accuracyPlotsAreAbsentWithoutAFit`; `tst_fusion_store restoredAfterRestartIsBitIdentical`, `fusionColumnFillsUnloadedSessions`, `restoredFitWithoutAccuracyDrawsTheRest`; `tst_fusion_jobs noImuSessionCannotHaveAJob`; `manual M59` |
| 1712 | 6 | the colours: the quiet member of the fused value and kin of the counterpart in the GNSS quality group; the floor of 11 and why; `#c57fa6`, `#08a2af`, `#8c619b` as literals with the comment's figures; a colour that looks wrong moves within the rule with its figures recorded | `audit naming`; `audit accuracy-channels`; `manual M59` |
| 1713 | 7 | the documents: `COMPUTED_PLOTS.md`, `PLOT_COLOURS.md` section 5, `SENSOR_FUSION.md` section 2 and its test rows, `CALCULATIONS.md`'s eighteen; every count of fifteen becomes eighteen; the pattern gains fifteen | `audit naming` |
| 1714 | 8, fourth bullet | test: eighteen in order; the three typed and backed by the fit; absent where the covariance failed with the other plots drawn; one row alone one fit; a Speed accuracy column fills loaded and unloaded rows, formatted by its type | `tst_fusion_rows allFusionPlotsAreExplicitBacked`, `accuracyPlotsAreAbsentWithoutAFit`, `speedAccuracyRowIsBlockedByFusion`; `tst_fusion_store restoredFitWithoutAccuracyDrawsTheRest`, `fusionColumnFillsUnloadedSessions` |
| 1715 | 8, audit bullet, third clause | `naming` pins eighteen with the mirror and refuses fifteen in the documents; `accuracy-channels` counts seven; both planted once; the audit and the map check green | `audit naming`; `audit accuracy-channels` |
| 1716 | 8, manual bullet | the manual step M59 | `audit naming`; `manual M59` |
| 1717 | 8, last bullet | items 1701-1717 complete with appendix R and section 9.18; 115, 509, 801, 802, 842, 854, 862, 863, 1602, 1608 and 1610 restated | `audit naming` |

The `audit naming` line of 1713, 1716 and 1717 is a floor, as 1606's and
1611's are: the rule shows no stale count survives, not that the reading
paragraph exists; the reviewer reads the documents against What changes and
M59 is the real check of 1716.

M59 (section 12.16, "The fused accuracies (1710, 1711, 1712, 1716)"), in
M58's idiom, on 12.1's logbook copy with a fusable recording without a
stored fit: (1) the list: "Sensor fusion" ends Horizontal accuracy, Vertical
accuracy, Speed accuracy after Vertical acceleration accuracy, their axes
reading the GNSS accuracies' units; (2) one fit: Speed accuracy checked
alone, "Computing results: 0 / 1", one fit, then the other two draw at once;
(3) colours: the three beside the GNSS quality rows (the three accuracies,
Number of satellites) and beside the fused speeds and fused Elevation, both
themes, each told from its counterpart and the rest, tick labels readable;
(4) values: the legend and the measure tool show each with its counterpart's
unit and decimals in both unit systems; (5) absent: M51's rejected recording
draws nothing for the three and is listed once; a recording whose
covariance was not computed, if one is at hand (`fusion_runner` reporting
`accuracy.computed` false; no reference recording is one), draws the three
absent with the fit's other plots drawn, else the step records that none
exists (Decisions); (6) a column: the Add Column group lists eighteen;
"Speed accuracy" at the exit marker fills loaded and unloaded rows in the
speed unit, labelled `Speed accuracy @ <marker>`, shown after a restart
without a load; (7) the update: a logbook copy whose fits a build from
before this plan stored (as M44 sets one up), started with this build and a
fusion plot checked: each shown recording is computed again once, shown in
the status bar, and not again after a hide, show or restart. M58's closing
sentences on a colour that looks wrong and on the phase report.

## Decisions

1. **The restated items are the eleven whose statements count the plots.**
   `grep -n fifteen tests/acceptance_map.txt` finds 115, 509, 801, 842, 854,
   862, 863, 1602, 1606, 1608 and 1610; 802 counts "the first eleven". 1606
   says what the speeds' specification did to the documents (twelve became
   fifteen), true as history, and is left; 1611, which the overview names,
   does not count the plots in its statement (M58's "lists fifteen" is a
   README line, updated as M51's was) and is left too. Michael may confine
   or extend the restatement.
2. **No new format assertion.** The specification's rows bullet asks that a
   column over Speed accuracy be formatted by its type, which
   `fusionColumnFillsUnloadedSessions` asserts against the converter; the
   types themselves are the GNSS rows' and are already held by
   `tst_plot_format`. A `distance` loop in both unit systems would repeat
   `typedValueIsTheConverterFormat` for a type no new row introduces.
3. **The seven accuracy rows share one count rule.** The specification says
   the count becomes seven; a second rule for the three would be generality
   nothing uses. The comment says why the two kinds are both "accuracy rows".
4. **The covariance-failure case of M59 is conditional.** A record is binary
   and no reference recording fails the covariance step (M50 asserts
   `accuracy.computed` true on each), so a manual step cannot make one; the
   rejected recording shows the three absent with the warning, and the
   covariance case is proved by `restoredFitWithoutAccuracyDrawsTheRest`,
   which rewrites a record. The step says so rather than inventing a recipe.
5. **The restart test covers the three through `capture()`**, as it covered
   the speeds; a fit-running test of their own would repeat it.
6. **`PLOT_COLOURS.md` changes in section 5 alone**, as the specification's
   section 7 says; the floor-of-11 account belongs to the plot table's
   comment, where the figures live and a moved colour's are written.

Ready with one caveat: Decision 4 (the manual covariance-failure check is
conditional on a recording that exists only if one is found).
