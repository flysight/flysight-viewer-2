# Phase 5: Plots, validation and traceability

## Purpose

This phase implements the specification's section 8, and finishes what remains of
sections 6, 11 and 12:

- the four accuracy plots in the "Sensor fusion" category, over the channels
  phase 4 publishes, and a logbook column over any of them;
- the validation of the model: the normalized residuals of the reference
  recordings and of the committed fixtures, recorded in
  `docs/SENSOR_FUSION.md` section 8 as measured;
- whatever clause 65 still lacks in the documents;
- the acceptance range 1001-1065 closed.

No number of the fit changes. The kernel, the registration, the record, the
algorithm string (`batch-temperature-bias-v6`) and the goldens stay as phase 4
leaves them. This is one phase because the plots, the validation and the closing
of the range all depend on the final model and change only the application's
plot list, tests, documents and traceability.

Clauses owned: 24, 37, 38, 39, 55, 63, 64, 65 (items 1024, 1037-1039, 1055,
1063-1065).

- **Restated "(as settled)":** 24, 37 and 55 (Decisions 1, 3).
- **Restated "(as amended)":** 115, 509, 801, 802, 842, 854, 862, 863 and
  930 (Traceability).

## Dependencies

**Depends on phases 1-4.** At the start:

- `Fusion/headingAcc` (deg), `Fusion/tiltAcc` (deg), `Fusion/accHAcc` (m/s^2)
  and `Fusion/accDAcc` (m/s^2) are declared outputs of `builtin.fusion.fit`.
  `kFitOutputs` has them after `qw`: twenty-one measurements and
  `_FUSION_DIAGNOSTICS`.
- The four channels are unset (unavailable, never empty) before a fit, for
  `Rejected` and `SolverFailed`, and for a success with
  `accuracy.computed == false`. When they are present:
  - they are aligned with `Fusion/_time`;
  - heading and tilt lie in (0, 180], and 180 means undetermined. They are not
    unwrapped;
  - the two acceleration accuracies are finite and at least the accelerometer's
    per-sample sigma.
- The record stores and restores them. `fusionMeasurementNames()` and
  `goldenDifference` (`tests/fusion/fusionsessions.*`) cover them.
  `coarse_linear` publishes `headingAcc` = 180 at every sample.
- The diagnostics carry `quality` (`position_nrms`, `velocity_nrms`,
  `imu_nrms`), `residuals` (kind, node, `time_s`, `squared_whitened_error`),
  `model.scale` with its sigmas, and `accuracy` (`computed`, `failure`,
  `widening_half_width_s`, `max_widening`, `widened_samples`,
  `undetermined_heading_samples`). The goldens of phase 4's capture hold all
  of these.
- The audit groups `sensor-configuration`, `noise-model`, `scale-state` and
  `accuracy-channels` exist. `accuracy-channels` allows the quoted channel
  names in `src/fusion/fusionregistration.cpp` only. The traceability block
  checks an explicit completeness list that ends at phase 4's items.
- `tests/README.md` section 12.9 holds M47-M50. Section 9.11 and appendix K
  hold phases 1-4's rows, readings and amendments.
- The plot list still has the eight "Sensor fusion" rows. The `naming` group
  pins it at eight. `tst_fusion_rows::removedPlotMeasurementsStayAvailable`
  covers the four channels, which have no plot yet.

**Blocks nothing.** This is the last phase. The final reviews follow it (see
"For the final review").

## What changes

### The plot rows: `src/mainwindow.cpp` (clauses 37, 38)

`registerBuiltInPlots` gains four rows after Roll, in this order, within the
"Sensor fusion" block. Names, units and type keys are contract: the tests'
mirror and the audit spell them.

| Name | Units | Colour | Sensor / measurement | Type |
| --- | --- | --- | --- | --- |
| Heading accuracy | `deg` | `QColor::fromHsl(280, S_dk, L_db)` | `Fusion` / `headingAcc` | `angle` |
| Tilt accuracy | `deg` | `QColor::fromHsl( 80, S_dk, L_dc)` | `Fusion` / `tiltAcc` | `angle` |
| Horizontal acceleration accuracy | `m/s^2` | `QColor::fromHsl( 40, S_dk, L_dw)` | `Fusion` / `accHAcc` | `acceleration_accuracy` |
| Vertical acceleration accuracy | `m/s^2` | `QColor::fromHsl(160, S_dk, L_dc)` | `Fusion` / `accDAcc` | `acceleration_accuracy` |

**Names.** They follow the GNSS accuracy rows ("Horizontal accuracy", ...):
the quantity, then "accuracy", in sentence case. These are the
specification's own names.

**Colours** (Decision 2). The deep scheme of the GNSS accuracy rows:
saturation `S_dk`, with the deep lightness of each hue's family (warm `L_dw`
for H 320-50, cool `L_dc` for 60-180, blue `L_db` for 200-300).

- **The hues.** Each hue lies at least `group_a` (40 degrees) from the three
  GNSS accuracy rows (0, 120, 240) and from the other three new rows. Only the
  arcs 40-80, 160-200 and 280-320 allow that.
- **Families.** Each sits in the family of the value it qualifies:
  - heading accuracy at 280 is blue, beside Heading's 240;
  - tilt accuracy at 80 is cool, beside Pitch's 60;
  - horizontal acceleration accuracy at 40 is warm, like the accelerations;
  - vertical acceleration accuracy at 160 is cool, beside Vertical
    acceleration's 170.
- **No duplicates.** None of the four equals the colour of any registered
  row.
- **The comment.** The block's comment says this, after the existing comment
  on the fused rows.

**Type keys** (Decision 3).

- **Heading and tilt.** They are angles: type `angle`, one decimal in deg.
  The type converts and rounds, and nothing in the converter, the axis ticker
  or the measure tool unwraps (`unwrapDegrees` is applied by the kernel and the
  attitude calculation to their own channels). Values from 0 to 180 are
  therefore plotted and measured as they are.
- **The two acceleration accuracies.** They take a new type,
  `acceleration_accuracy`. It is the acceleration's unit (`g` in both
  systems, from m/s^2 by the acceleration row's scale) at **four** decimals.
  The `acceleration` type has two decimals of g (0.098 m/s^2), and the
  accuracies on the reference recording `11-17-12` have medians of 0.040 and
  0.0046 m/s^2 (phase 4's probe). With the `acceleration` type, the axis ticks
  (`UnitConvertingTicker` uses the type's precision), the legend, the measure
  tool and a logbook column would all read "0.00" over most of a recording.
- **Where the type goes.** Add `MeasurementTypes::AccelerationAccuracy`
  (`"acceleration_accuracy"`) and its entry to `src/units/unitdefinitions.h`,
  in the pattern of `SpSpeedAcc`, the existing accuracy type with its own
  precision. Its comment says why it exists.

**No menu shortcut** (Decision 4). `initializePlotsMenu`'s `PlotMenuItem`s
are not touched. The Plots menu lists GNSS plots only, and no fusion plot has
an entry. The specification's section 10 limits the change to the plot
registry.

**What must not change:**

- the eight existing fusion rows and every other row;
- the kernel, the registration, `kFitOutputs`, the record, the goldens and
  `Fusion::Algorithm`;
- the demand layer, the plot model and the plot widget.

The new rows get their colour, axis-mode and axis-range preferences from the
existing loop in `initializePreferences`. `plotColor()` serves them like any
row, so `tst_plot_color` needs no change.

### Absent where not computed (clause 38)

**Nothing is added.** A plot or column over an unset output already draws
nothing and shows the unavailable mark:

- **Before a fit:** the value is merely uncomputed and waits on the fit.
- **Rejected or failed fit:** `NotProduced` with the fit's reason, listed once
  per recording.
- **Recording without IMU data:** `NotApplicable`, silent.

**A success whose covariance failed** leaves the four unset while the other
seventeen are present.

- **Loaded recording.** With an accuracy plot checked, the demand layer lists
  the recording with its generic "No result for this session". The fit's
  result carries no reason: a reason on an Ok record would mark every column
  over the fit failed at the next start (rule 4 of the stub walk in
  `CalculationDemand`, "a result, failed when a record carries a reason").
  Plots and columns over the other seventeen show nothing unusual.
- **Recording not loaded, after a restart.** The cell over an accuracy is
  blank with no triangle.
- **No change in this phase.** This is reported for Michael (caveat 2), and
  the phase changes neither the demand layer nor the registration for it.
  Phase 4 makes the case practically unreachable: a rank-deficient heading is
  capped, not failed.

### The tests' mirror: `tests/fusion/fusionsessions.*`

`fusionPlots()` gains the four rows (name, units, measurement, type, as in
the table) after Roll. Its header comment says "twelve".
`tst_fusion_session::plotNames()`'s comment says "twelve". Count literals
become 12, the `QCOMPARE(plots.size(), 8)` in
`tst_fusion_jobs::noImuSessionCannotHaveAJob` included.

### The validation (clauses 24, 55; overview decision 20)

**The runs.** M52 runs `fusion_runner --csv` on the four reference recordings
of `tests/README.md` section 12.2, on the build of this phase: `11-17-12`
(M10/M11), `08-35-23` (M12), `10-15-24` (M13) and `08-35-41` (M14). Each run's
numbers are printed by one script, which is written into the step as M46's
one-liner is. It prints:

- the exit code, `stopping.rule`, the iterations and `objective`;
- `quality.position_nrms`, `velocity_nrms` and `imu_nrms`;
- `model.scale` and its sigmas;
- `accuracy.max_widening`, `widened_samples` (as a share of `imu_outputs`) and
  `undetermined_heading_samples`;
- the medians of the four accuracy columns of the CSV;
- the window factor at the fixes: its median, 95th percentile and maximum, and
  the share of fixes with F > 1.

**The window factor F** is computed from the diagnostics' `residuals`, by
phase 4's rule:

- the window is the fixes within `widening_half_width_s` of the fix, at least
  two;
- the position and velocity terms are summed over the window, and the `imu`
  terms of nodes lo+1..hi;
- F is that sum divided by 6N - 9;
- the priors are excluded.

The same script, run on a golden's `diagnostics`, gives the committed fixtures'
numbers.

**The committed fixtures** are the three golden fixtures that fit
(`coarse_linear`, `coarse_maneuver`, `stationary_spin`). Their normalized
residuals are the goldens' `quality`, held by
`tst_fusion_golden::successFixturesMatchGolden` (clause 55). The rejections
have no residuals. The initializer fixtures and `scale_recording` are model
tests with no golden (Decision 1).

**No constant changes, whatever the numbers are** (specification section 2).
A recording that does not converge is reported with its exit code, rule and
failure, and with no residuals, since a failed fit's diagnostics carry none.

**In the document.** `docs/SENSOR_FUSION.md` section 8 gains two bold-led
paragraphs, in the form of "**Validating the reconstruction.**":
"**Validating the model.**" and then "**What is and is not validated.**". They
go after the reconstruction and accuracy-agreement paragraphs and before
"**The runner.**".

*Validating the model* holds:

1. **An opening paragraph.** It says what is reported: the normalized RMS of
   each factor kind (root mean squared whitened residual per scalar component,
   section 4), under the model of section 4, measured with
   `batch-temperature-bias-v6` on <date>, by `tests/README.md` M52. It says
   that no constant of the noise model was changed to move these numbers. It
   then gives the sentence, on one line:
   "A value far from one measures what the model does not yet describe." It
   says what each direction means:
   - below one, the stated sigmas exceed the scatter (for GNSS, the receiver's
     accuracies; for the IMU, the datasheet noise and the derived step terms);
   - above one, something the model does not describe.
2. **A table of the reference recordings.** One row per recording (its name
   and unit) with:
   - the fixes and IMU samples;
   - the outcome and stopping rule, the iterations and the objective;
   - the position, velocity and IMU normalized RMS;
   - the largest scale departure, accelerometer and gyro.
3. **A table of what the widening and the accuracy showed on them.** Per
   recording:
   - the median, 95th percentile and maximum window factor;
   - the share of samples widened and the largest widening;
   - the samples with an undetermined heading;
   - the median of each of the four accuracies.
4. **A table of the committed fixtures.** The three fits, with the three
   normalized RMS as the goldens hold them and the median window factor. Then
   one sentence on what they measure: the generator's noise against the
   accuracies the fixture states. That checks the arithmetic, not the model.
5. **"What the widening showed".** This is a statement of facts. An example of
   the kind expected, not a number to copy: phase 4's probe found a median
   factor of 0.08 on `11-17-12`, with 0.26 % of the samples widened. This
   means:
   - the GNSS residuals there are far below the receiver's stated accuracies;
   - so the widening seldom acts;
   - and where GNSS dominates, the published accuracy reflects the stated
     sigmas, not the scatter.
   
   The paragraph reports this and does not answer it. The GNSS weighting is
   out of the specification's scope (its section 3).
6. **The noise floor.** It states two figures:
   - **Accelerometer.** The per-sample sigma at the default configuration,
     0.0030 m/s^2, against the quietest-window noise of 95 corpus recordings,
     0.0029-0.0035 m/s^2.
   - **Gyro.** The per-sample sigma, 0.0217 deg/s, against the corpus median,
     0.030 deg/s (phase 2's figures). The model is below the corpus.

   Each figure is stated as a fact of validation. No path under
   `experiments/` is cited.

*What is and is not validated* is two short lists.

- **Validated.** Each entry names its test or step:
  - the arithmetic against the goldens;
  - the covariance against the joint marginals;
  - the composition against the edge graph;
  - the propagation on known answers;
  - the widening on a known understatement;
  - the scale recovered on a synthetic recording;
  - the per-sample noise against the table;
  - the accelerometer noise floor against quiet windows;
  - the configuration and lattice on the reference recordings.
- **Not validated:**
  - the accuracy against an independent truth (no reference trajectory or
    attitude was recorded);
  - per-unit noise, bias instability and the sensitivity's temperature change
    (part 2);
  - the receiver's stated accuracies, which the residuals show pessimistic on
    these recordings;
  - the time correlation of GNSS errors (the whitened residuals assume
    independent fixes);
  - the widening's assumption that every sigma is off by one ratio;
  - any configuration but the default on real data (no recording on disk
    states one; the fixtures do).

**The numbers in the tables are M52's printout**, rounded to two or three
significant figures. The phase report carries the raw printout, so a reviewer
can hold the tables against it.

### The documents (clause 65)

**What this phase writes:**

- **`docs/SENSOR_FUSION.md` section 2.**
  - The category has twelve plots: the eight as now, then the four accuracy
    plots.
  - A sentence on what an accuracy plot shows: one standard deviation, linked
    to section 4's "Accuracy"; heading accuracy 180 means undetermined; heading
    and tilt in degrees and not unwrapped; the two acceleration accuracies in
    g at four decimals.
  - They have no GNSS counterpart to overlay, and are drawn in the deep
    colours like the GNSS accuracies.
  - They are absent where the fit did not compute them.
  - "All eight plots" becomes "All twelve plots".
- **`docs/SENSOR_FUSION.md` section 8.**
  - The validation above.
  - The `tst_fusion_rows` row: twelve plots.
  - The `tst_fusion_store` row: the accuracy column.
- **`docs/CALCULATIONS.md` section 17.**
  - The "Plots." paragraph: twelve plots in order. `accD` and the four
    accuracies are outputs of the fit, and the other seven are the on-demand
    calculations blocked by it. "All twelve are requested".
  - The test paragraph's "the eight real plots" becomes "the twelve".
- **`docs/COMPUTED_PLOTS.md`.**
  - Section 1's list becomes twelve, naming the four.
  - Section 2's "one computation ... can serve several plots" names the
    accuracies with heading, pitch and roll.
  - One sentence: an accuracy plot is absent for a recording whose fit did not
    compute it, while the fit's other plots are drawn.

**What phases 1-4 left: check, and complete in the document that should carry
it.** Go document by document against clause 65 and the specification's
section 12. Report each gap found and fixed. The list to check:

- **`DATA_SCHEMA.md`:**
  - section 2: the keys, their forms, the import error and the default with
    its filter setting, stated once in the documents;
  - section 4: the correction is not the scale state;
  - section 7: the hand-edited legacy file is rejected by the lattice;
  - section 11: `v6`;
  - section 12: twenty-one measurements.
- **`SENSOR_FUSION.md`:**
  - section 3: the configuration inputs, "twenty-six", the lattice and rate
    checks;
  - section 4: the table with its sources, the formula, the density, both
    derivations with what they assume, the bias priors with their rows, the
    scale state, the covariance, the composition, the propagation with what it
    leaves out, the widening, the clause 36 sentence, the twenty-one outputs,
    every diagnostics key named by phases 2-4, and `limitations`;
  - section 5: the scale limitation;
  - section 6: the three rejections;
  - section 7: `v6`, its first start, twenty-two outputs, and the covariance
    time M50 measured in place of the probe's;
  - section 8: fourteen fixtures, "the runner" paragraph's `--csv` (twenty-one
    columns, seventeen when the accuracy is absent), and no "no uncertainty is
    published".
- **`CALCULATIONS.md`:**
  - section 5: the four defaults;
  - section 15: `shutdown()`;
  - section 17: twelve calculations, the 26 inputs, the outcome mapping with
    the four, the record and its leaves, `v6`.
- **`tests/README.md`:**
  - section 11: three history paragraphs (phases 2, 3, 4);
  - section 12's intro: the count of scripts;
  - the section 1 rows of every executable phases 1-5 changed.

### Traceability: the range closed (clauses 64, 65)

**The audit's traceability block.**

- Replace the explicit completeness list with
  `foreach(item RANGE 1001 1065)`, in the form of the other ranges: a test or
  audit line is required.
- Remove the comment that says each phase appends to the list.
- The block's head comment names the range "the documented noise model and
  the accuracy, part 1 (item = 1000 + clause number)".
- The file's head bullet (phase 1's) is completed to state the invariants of
  all of 1001-1065:
  - one vocabulary of the keys and one default;
  - the datasheet in one unit;
  - the retired constants gone;
  - one scale-state factor;
  - one factorization and no joint marginals;
  - the channel names in the registration and the plot rows;
  - twelve fusion plots;
  - the validation in the fusion document.

**The map's head paragraph, final.**

- "Eleven specifications, eleven item ranges"; "sections 9.1 to 9.11";
  1001-1065 among the ranges that need a test or audit line.
- The 1001-1065 paragraph describes the whole specification, as the 901-940
  one does. It lists every item it amends, by range:
  - 115 (101-120);
  - 215, 216, 217, 221, 227, 230, 235, 245, 246, 247 (201-247);
  - 337 (301-350);
  - 509 (501-563);
  - 801, 802, 842, 847, 854, 861, 862, 863 (801-863);
  - 901, 910, 912, 914, 921, 925, 930 (901-940).

  Check the list against the appendices: phases 1-4 may have added others.
- The paragraphs of 101-120, 501-563, 801-863 and 901-940 each name their
  items of that list ("It is amended by the specification of 1001-1065: items
  ... are stated as amended"). 201-247 and 301-350 already do, from phases 1-4.

**`tests/README.md` section 9.11 and appendix K, final.**

- **9.11's intro** says, for every as-settled clause, how it reads the letter:
  6, 8, 9, 10, 11, 14, 15, 18, 20, 24, 25, 26, 31, 35, 37, 44, 55, 61. It
  names the amended items of every earlier specification.
- **Rows.** 9.11 has a row per item 1001-1065.
- **Appendix K's head paragraph** lists the same as-settled clauses. It says
  that the specification amends the earlier items above.

## Interfaces

**Provided** (to the final reviews; nothing consumes them in code):

- four plot rows `Fusion/headingAcc`, `tiltAcc`, `accHAcc`, `accDAcc`, named,
  united, typed and coloured as in the table;
- the measurement type `acceleration_accuracy`;
- `fusionPlots()` with twelve rows;
- the test function `tst_fusion_rows::allFusionPlotsAreExplicitBacked`,
  renamed from `allEightFusionPlotsAreExplicitBacked`;
- the audit group `model-validation`;
- manual steps M51 and M52;
- the closed range.

**Consumed:** phase 4's channel names, their absence rule and their value
ranges, the diagnostics keys listed under Dependencies, and the goldens of
phase 4's capture.

## Acceptance criteria

1. **The rows.** `registerBuiltInPlots` has twelve "Sensor fusion" rows. The
   last four are those of the table, after Roll, with those names, units,
   measurements, types and colours. No `PlotMenuItem` is added (clause 37;
   audit `naming`, `accuracy-channels`).
2. **The type.** `acceleration_accuracy` converts m/s^2 to g with the
   acceleration row's scale in both systems, at four decimals.
   `formatValue(0.0046, "acceleration_accuracy")` is `"0.0005"`, where
   `"acceleration"` gives `"0.00"` (clause 37).
3. **Explicit-backed.** All twelve plots are explicit-backed, with
   `explicitDependencies` exactly `{builtin.fusion.fit}`. Checking all twelve
   with one visible fusable session starts one fit. After it, every row has
   samples on the fit's time axis, the four included (clauses 37, 63).
4. **Absent where not computed** (clause 38):
   - before the fit, all four are merely uncomputed;
   - for a rejected recording, all four are absent and the recording is listed
     once, with the fit's reason;
   - a session without IMU data is `NotApplicable` for all twelve;
   - for a stored success whose record lacks the four, the four are absent and
     the seventeen are present and bit-identical; a Roll plot on it shows no
     failure, and nothing is fitted again.
5. **A column over each of the four** (clauses 39, 63):
   - at the exit marker, over a session that is not loaded, it is filled by the
     demand layer: one fit, the record written, the value cached and indexed
     with the fit's stamp, the session left unloaded;
   - the cached value is bit-identical to the loaded session's interpolated
     value;
   - its display text is the unit converter's for the row's type;
   - its label is the row's name, "Horizontal acceleration accuracy @ ...".
6. **The validation** (clauses 24, 55):
   - section 8 holds the three tables and the two paragraphs described above;
   - the reference recordings' numbers equal M52's printout, rounded;
   - the fixtures' numbers equal the goldens' `quality`;
   - the clause 24 sentence is present once;
   - no file under `src/fusion/` changed in this phase.
7. **The documents** carry what "The documents" lists. Every gap of the
   clause 65 sweep is fixed or reported (clause 65).
8. **The range.** The audit checks `RANGE 1001 1065`, and every item has a
   resolving test or audit line. Every earlier map line that cited
   `allEightFusionPlotsAreExplicitBacked` cites the new name. No file in the
   repository spells the old name (clauses 64, 65).
9. **The suite** is green in `build-agent/` (Release, sequentially),
   `audit_cleanup` included.

## Tests

### `tst_fusion_rows`

- **Rename** `allEightFusionPlotsAreExplicitBacked` to
  `allFusionPlotsAreExplicitBacked`. It has a name without a count, so the next
  plot does not rename it. Its literal mirror gains the four rows,
  `plots.size()` is 12, and its comments say "twelve" (criterion 3).
- **Add** `accuracyPlotsAreAbsentWithoutAFit` (criterion 4, first three
  points). It checks the four accuracy rows on:
  - a `coarse_linear` session before its fit;
  - `reject_origin` after its fit: absent; `failures()` lists it once with
    "Local origin index outside GNSS samples";
  - a session without IMU data.
- **Amend** `removedPlotMeasurementsStayAvailable`: if phase 4 added the four
  names to its record loop, remove them. They now have plots, and the record
  holding them is `tst_fusion_store`'s.
- **Update** the file comment ("the twelve rows"), the comment of
  `noImuSessionIsNeverCounted`, and the comments that count.

### `tst_fusion_store`

- **Add** `accuracyColumnFillsUnloadedSessions_data` /
  `accuracyColumnFillsUnloadedSessions` (criterion 5).
  - One row per accuracy, with the column at the exit marker typed as its row.
  - Follow `columnOverFusionFillsUnloadedSessions` on `coarse_maneuver`:
    `logbookColumnExplicitCalculations` is `{kFit}`; the session is unloaded;
    one job runs; the record file is written; the cached and indexed value has
    the stamp; a fresh load gives the same bits with `runCount(kFit) == 0`.
  - Also check the cell's display text against
    `UnitConverter::formatValue(value, type)`, and the column's label against
    `logbookColumnLabel`, which must start with the row's name.
- **Add** `restoredFitWithoutAccuracyDrawsTheRest` (criterion 4, last point).
  It follows `restoredSolverFailureShowsBadge`:
  - fit `coarse_maneuver`;
  - with `rewriteRecord`, drop the four channels from the bundle, as
    `publish()` leaves them for a success whose covariance failed (no reason),
    and set the diagnostics' `accuracy.computed` to false with phase 4's
    failure text;
  - unload and reload;
  - check: the four are absent; the seventeen equal the golden; the Roll row's
    track is Done with no failure; nothing is fitted again over three spins.
  - Do not assert the generic failure entry an accuracy plot gets (caveat 2).

### Others

- **`tst_fusion_jobs::noImuSessionCannotHaveAJob`:** the count is 12.
- **`tst_fusion_session::plotNames`:** the comment. Its uses then cover the
  four.
- **`tst_plot_format`:**
  - `typedValueIsTheConverterFormat`'s loop gains `acceleration_accuracy`;
  - add `accelerationAccuracyKeepsItsDigits` (criterion 2: the two literals,
    and that the type's unit label is the acceleration's).
- **`tst_plot_color`:** no change. The new rows are ordinary rows.

### Audit (`tests/audit/cleanup_audit.cmake`)

Each changed or new rule gets an "Allow:" comment and is planted once to prove
it.

- **`naming`.**
  - `expect_count("twelve fusion plots" "^ *\\{\"Sensor fusion\", " 12 src/mainwindow.cpp)`.
    Its comment drops "PLANS/fusion-accuracy.md will".
  - The documents rule's label becomes "the documents describe the twelve
    fusion plots". Its regex gains `[Ee]ight (real )?(fusion )?plots|[Aa]ll eight`
    in `docs README.md`. The existing "the same eight" (inputs) in
    `CALCULATIONS.md` does not match.
- **`accuracy-channels`.**
  - The quoted channel-name rule allows
    `^src/(fusion/fusionregistration\.cpp|mainwindow\.cpp)$`.
  - Add `expect_count("the accuracy plots are deep" ...)`: exactly 4 lines in
    `src/mainwindow.cpp` match a "Sensor fusion" row whose name ends in
    " accuracy" and whose colour is `QColor::fromHsl(<hue>, S_dk, L_d[wcb])`.
- **New group `model-validation`**, beside `accuracy-channels`:
  - `expect_count` of `measures what the model does not yet describe`, 1 line,
    in `docs/SENSOR_FUSION.md`;
  - `expect_count` of `\*\*Validating the model\.\*\*` and of
    `\*\*What is and is not validated\.\*\*`, 1 each, in the same file.
- **Traceability:** as above.
- **`tests/README.md` section 10:**
  - the `naming` bullet becomes twelve;
  - the `accuracy-channels` bullet gains the plot rows and the deep rule;
  - a bullet for `model-validation`;
  - the `audit_cleanup` row of section 1 says "twelve fusion plots".

### Traceability

Map lines (`tests/acceptance_map.txt`, comment line first):

- 1024 (as settled) `audit model-validation`; `tst_fusion_golden successFixturesMatchGolden`; `manual M52`
- 1037 (as settled) `tst_fusion_rows allFusionPlotsAreExplicitBacked`; `tst_plot_format accelerationAccuracyKeepsItsDigits`; `audit naming`; `audit accuracy-channels`; `manual M51`
- 1038 `tst_fusion_rows accuracyPlotsAreAbsentWithoutAFit`, `noImuSessionIsNeverCounted`; `tst_fusion_store restoredFitWithoutAccuracyDrawsTheRest`; `manual M51`
- 1039 `tst_fusion_store accuracyColumnFillsUnloadedSessions`; `manual M51`
- 1055 (as settled) `tst_fusion_golden successFixturesMatchGolden`; `audit model-validation`; `manual M52`
- 1063 `tst_fusion_rows allFusionPlotsAreExplicitBacked`; `tst_fusion_store accuracyColumnFillsUnloadedSessions`; `tst_fusion_jobs noImuSessionCannotHaveAJob`
- 1064 `audit solver-confinement`; `audit fusion-tooling`; `audit noise-model`; `audit scale-state`; `audit accuracy-channels`; `audit model-validation`
- 1065 `audit sensor-configuration`; `audit noise-model`; `audit scale-state`; `audit accuracy-channels`; `audit model-validation`; `audit naming`

**Appendix K restates three clauses "(as settled)":**

- **24:** "... the normalized residuals (position, velocity, IMU) of the
  reference recordings of section 12.2 and of the committed fixtures that fit
  (the three golden successes; the rejections have no residuals) ...".
- **37:** "... drawn in the deep scheme of the GNSS accuracy plots (their
  saturation and lightness, each hue at least 40 degrees from theirs and from
  the others'); the acceleration accuracies in g at four decimals, a type of
  their own".
- **55:** "... the committed fixtures that fit ...".

**Amended items.** Each is restated "(as amended)" in its appendix, its 9.x
row and the map comment. Each appendix head paragraph names the amendment.

- **115** (B 15): "The real fusion plots are the twelve of the 'Sensor fusion'
  category (the four accuracies since the specification of 1001-1065), and
  heading, pitch and roll share one job." 9.2 row 11's evidence note becomes
  "(all twelve real rows)".
- **509** (F 9): "The twelve plots ... are such plots: five are outputs of the
  fit (vertical acceleration and, since the specification of 1001-1065, the
  four accuracies), and the seven derived from its outputs are blocked by it."
- **801** (I 1): "exactly twelve plots, in this order: ..., Roll, Heading
  accuracy, Tilt accuracy, Horizontal acceleration accuracy, Vertical
  acceleration accuracy (the last four since the specification of
  1001-1065)".
- **802** (I 2, as settled and as amended): "each of the first eight is named,
  united and typed as its GNSS counterpart ...; the four accuracy plots have no
  GNSS counterpart and are named as the GNSS accuracy plots are".
- **842** (I 42): "... with twelve plots, the eight named as the GNSS plots and
  the four accuracies ...".
- **854** (I 54): "the plot list has the twelve fusion plots ...".
- **862** (I 62) and **863** (I 63): "the twelve plots".
- **930** (J 30, after phase 1's amendment): "... and the plot registry gains
  the four accuracy plots".

**Searched and holding.** Their citations change with the rename only:

- 803, 806, 808, 853 and 856;
- 804 (the removed plots' measurements);
- 111 (its statement names no count);
- M39 and M44 hold as steps. M39 adds "then the four accuracy plots (M51)",
  and M44's "eight Sensor fusion measurements" becomes twelve.

**`tests/README.md` section 1 rows:**

- `tst_fusion_rows`: twelve, and the new function;
- `tst_fusion_store`: the accuracy column and the restored fit without
  accuracy;
- `tst_fusion_jobs`;
- `tst_plot_format`: the type;
- `audit_cleanup`.

### Manual steps (`tests/README.md` section 12.9)

- **M51 The accuracy plots (1037, 1038, 1039).** Use 12.1's preamble (a COPY of
  a logbook) and a fusable recording without a stored fit.
  - **The plot list.** "Sensor fusion" ends Heading accuracy, Tilt accuracy,
    Horizontal acceleration accuracy, Vertical acceleration accuracy, after
    Roll. Their axes read "(deg)", "(deg)", "(g)", "(g)".
  - **One fit.** Check Heading accuracy alone: "Computing results: 0 / 1", and
    one fit. Then the other eleven draw at once.
  - **Colours.** In the light and the dark theme, beside Heading, Pitch, Roll,
    the two accelerations and the three GNSS accuracies, each accuracy line is
    told apart from all of them.
  - **Values.** Heading and tilt stay within 0-180 with no 360-degree jump. The
    legend and the measure tool show one decimal for them and four decimals of
    g for the acceleration accuracies.
  - **The menu.** The Plots menu is unchanged.
  - **A rejected recording.** On one (the hand-edited `SCHEMA_VER,2` legacy
    file of section 11, which the lattice rejects), the four draw nothing and
    the warning lists it once.
  - **A column.** The Add Column dialog's Sensor fusion group lists twelve. Add
    "Horizontal acceleration accuracy" at the exit marker:
    - loaded and unloaded rows fill, at four decimals;
    - the label is "Horizontal acceleration accuracy @ <exit marker>";
    - after a restart the values show without a load.
- **M52 The validation of the model (1024, 1055).** Use 12.2's preamble.
  - **The runs.** Run each of M11-M14's recordings with
    `--csv TEMP/runs/<name>.v6.csv`, output to `TEMP/runs/<name>.v6.json`.
  - **The script.** Then run the step's script on each JSON and CSV, and on
    `tests/data/fusion/<fit>.json` for the three fits.
  - **The record.** Record the printout, unadjusted, in the phase report, and
    write it into `docs/SENSOR_FUSION.md` section 8's tables.
  - **Pass condition.** The step passes when every command ran and the
    document's numbers are the printout's. It passes whatever the numbers are.

## Decisions

1. **"The committed fixtures" are the three golden fixtures that fit** (clauses
   24, 55 as settled). Clause 55 places their residuals in the goldens, and
   only golden fixtures have goldens. Rejections have no residuals. The
   initializer fixtures are model tests whose numbers nothing holds, and
   their synthetic noise says nothing about the model.
2. **The hues: 280, 80, 40, 160 in the deep scheme** (clause 37 as settled,
   for Michael). The letter "the deep colours of the GNSS accuracy plots" is
   read as their scheme (`S_dk`, `L_d*`), not their four colours. The GNSS
   colours are taken, and Heading accuracy at 240 would be Speed accuracy's
   exact colour. Each hue keeps `group_a` from the GNSS accuracies and from
   the other three new rows, in its value's family.
3. **Heading and tilt typed `angle`; the acceleration accuracies a new type,
   `acceleration_accuracy`, g at four decimals** (clause 37 as settled, for
   Michael). The GNSS accuracies are typed as their quantity (`distance`,
   `speed`), and so are heading and tilt. But at the acceleration type's 0.01 g
   the acceleration accuracies read "0.00" on the axis, the legend, the
   measure tool and a column, over most of a recording. `sp_speed_acc` is the
   precedent for an accuracy with its own precision.
4. **No menu shortcut.** No fusion plot has a Plots-menu entry, the GNSS
   shortcuts are the menu's whole content, and section 10 changes the
   registry only.
5. **The test is renamed to `allFusionPlotsAreExplicitBacked`.** It is named
   without a count, so the next plot does not rename it again. Every map line
   citing the old name changes in this phase.
6. **A success without accuracy gets no reason.** See "Absent where not
   computed". A reason on an Ok record would read as a failure of every
   column over the fit after a restart. The generic entry is left, and is
   reported.
7. **The window factor in the report is computed at the fixes from the
   diagnostics' residuals** by phase 4's rule. The diagnostics hold no
   per-sample factor, and a sample on a fix has exactly that factor.

## For the final review

Checks across the whole plan that no single phase makes:

- **The range.**
  - Items 1001-1065 each have a resolving test or audit line, and the audit's
    loop is `RANGE 1001 1065`.
  - Every map comment line equals its appendix K item, the as-settled and
    as-amended texts included.
  - Section 9.11 has 65 rows, and its intro explains every as-settled clause:
    6, 8, 9, 10, 11, 14, 15, 18, 20, 24, 25, 26, 31, 35, 37, 44, 55, 61.
  - Every amended earlier item is restated in its appendix, its 9.x row and
    its map comment, and is named in the map's head paragraph of its range and
    of 1001-1065.
- **One authority.**
  - The default of the four IMU keys and v2023.09.22 are written once in the
    documents (`DATA_SCHEMA.md` section 2) and once in `src`
    (`sensorconfiguration.cpp`).
  - The datasheet figures exist only in `sensornoise.cpp`.
  - `batch-temperature-bias-v6` is spelled once in `src`; no `v5` remains
    outside history.
  - One cap constant; one `eliminateMultifrontal`; no
    `jointMarginalCovariance`; `checkpoint(` counted 3.
- **The documents agree with each other and with the code on:**
  - the noise model: table, formula, density, step terms, bias priors;
  - the scale: six factors, the 1 % prior, the z split;
  - the accuracy: the composition, the propagation, the cap, the widening, the
    clause 36 sentence, `limitations` quoted as the code writes it;
  - the counts:
    - twenty-six inputs;
    - twenty-two outputs, twenty-one channels (seventeen when the accuracy is
      absent);
    - twelve registered calculations;
    - fourteen golden fixtures (three fits, eleven rejections) and five
      initializer fixtures;
    - twelve fusion plots;
    - nine keys, four of them inputs.
- **Probe numbers replaced.**
  - The probes' numbers quoted in the documents are replaced by measured ones:
    M47-M50's results, and the covariance time of section 7.
  - Every "for Michael" decision of the five phase documents is listed for him
    in the final report:
    - overview 4, 6, 7, 8, 15, 17;
    - phase 2: decisions 3-7 and the resampled fixtures;
    - phase 3: 4, 5, 7, 8;
    - phase 4: 1, 2, 5;
    - phase 5: 2, 3 and caveat 2.
- **Manual steps.**
  - M47-M52 exist in section 12.9, each cited by the items it names.
  - Their results are in the phase reports, unadjusted, and M52's in
    `docs/SENSOR_FUSION.md`.
- **The goldens.**
  - Three history paragraphs in `tests/README.md` section 11.
  - `capture.json`'s hashes current.
  - The `_time` columns unchanged since 2026-09-30.
  - Exact and portable modes green.

## Caveats

1. **M52's outcome is unknown until run.** A reference recording may not
   converge under the datasheet model. It is then reported, with no constant
   changed (overview decision 20), and Michael decides what follows.
2. **A success whose covariance failed** shows a generic "No result for this
   session" for a checked accuracy plot on a loaded recording, and a blank cell
   without the triangle for an unloaded one. Making both say why would need a
   reason on a successful record, which the demand layer reads as a failure of
   every value of that calculation, or a per-output reason the engine does not
   have. Left for Michael; phase 4 makes the case practically unreachable.
3. **Colours are checked by eye.** The audit pins the deep scheme, and M51 the
   distinctness. `MainWindow` is outside the test library, so no automated test
   reads the colours.

Ready with caveats: Decisions 2 and 3 (clause 37 as settled), Decision 1
(clauses 24 and 55 as settled) and caveat 2 are flagged for Michael; caveat 1
is the specification's own. The phase can be implemented as written.
