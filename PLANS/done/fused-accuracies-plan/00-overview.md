# Fused position and speed accuracy: implementation plan overview

## Feature specification

## Fused position and speed accuracy

Date: 2026-10-06
Status: specification for planning. Not an implementation plan.
Baseline: branch `store-requested-calculations` at the head that archives
the fused speed plots (`9a3ccc0`); the committed code is authoritative.
Related: `docs/SENSOR_FUSION.md` (section 4, "Accuracy": the covariance
step, the composition at every sample, the four published accuracies and
the widening; section 8, the validation), `docs/CALCULATIONS.md` (the fusion
calculations and the plot list), `docs/DATA_SCHEMA.md` (the stored record
and its validity), `docs/COMPUTED_PLOTS.md`, `docs/PLOT_COLOURS.md`,
`src/fusion/fusion.h` (`Algorithm`, `Result`), `src/fusion/fitcovariance.h`
and `trajectoryreconstruction.h` (the composition), `src/fusion/fusionregistration.h`
(the fit's outputs and the derived calculations), `src/mainwindow.cpp`
(`registerBuiltInPlots`), `tests/audit/cleanup_audit.cmake` (`naming`,
`accuracy-channels`), `tests/acceptance_map.txt`, `tests/README.md`.
`PLANS/done/noise-model-part-1.md` specified the four accuracies this one
extends; `PLANS/done/fused-speed-plots.md` the speeds these qualify.

### 1. Motivation

The fit publishes four accuracies: heading, tilt, horizontal and vertical
acceleration. It publishes none for the quantities users compare most
directly with the receiver's own figures: the position and the velocity,
for which GNSS states Horizontal accuracy, Vertical accuracy and Speed
accuracy at every fix. The fused speeds exist to be read beside the GNSS
speeds and to show where the two sources disagree; without a fused speed
accuracy the user can see the receiver's doubt but not the fit's.

The fit's covariance already holds these figures. The composition at every
sample (`SENSOR_FUSION.md` section 4) forms the state's covariance from the
fit's and the step chain's; today only its attitude rows are formed, because
the four accuracies read nothing else. Forming the position and velocity
rows is the same Jacobian with more rows.

Sensor fusion has not been released. Every output the fit adds changes the
shape of its stored record, so its algorithm string bumps and every stored
fit is computed again: free before the release, a background recompute of
every fitted recording for every user after it. What the fit publishes is
therefore settled now, and settled as the fact rather than one presentation
of it: the covariance blocks, from which any accuracy figure is a derived
calculation that can be added or changed later without touching the fit.

### 2. Principles

- **The fit publishes facts; a scalar accuracy is a presentation.** The fit
  publishes the position and velocity covariance blocks at every sample.
  Horizontal accuracy, Vertical accuracy and Speed accuracy are on-demand
  derivations of them, in the manner of `Fusion/velH`, and a later
  presentation (an along-track speed accuracy, a glide-angle accuracy) is
  another derivation, not another output of the fit.
- **The fit's arithmetic is unchanged.** The solution, the seventeen state
  channels and the four accuracies are bit for bit what they were; only the
  outputs grow. The algorithm string bumps for the record's shape.
- **One rule for every accuracy.** First order, under the documented model,
  one standard deviation, widened where the residuals exceed what the model
  allows, absent where the covariance step failed: as the four are.
- **A fused accuracy is read beside the receiver's.** The three rows take
  the GNSS names, units and types, so that each overlays its counterpart.

### 3. What the fit publishes

Twelve measurement outputs after the four accuracies, aligned with
`Fusion/_time`: the upper triangles of two symmetric 3x3 covariance blocks of
the published state at each sample, in the navigation frame (north, east,
down), widened.

| Measurements | Meaning | Unit |
| --- | --- | --- |
| `posCovNN`, `posCovNE`, `posCovND`, `posCovEE`, `posCovED`, `posCovDD` | the covariance of the published position | m^2 |
| `velCovNN`, `velCovNE`, `velCovND`, `velCovEE`, `velCovED`, `velCovDD` | the covariance of the published velocity | m^2/s^2 |

- They are the position and velocity blocks of the composed sample
  covariance `Sigma_j` of section 4, turned from the tangent in which the
  reconstruction holds them into the navigation frame with the published
  attitude, as the attitude block is turned today. A sample on a fix carries
  the fix's own marginal.
- They are widened as the four accuracies are: by the square of the sample's
  widening factor, so that a standard deviation taken from them is widened by
  the factor itself. The widening is applied once, here; a derived accuracy
  applies none.
- They are filled for a success whose covariance was computed and empty for
  every other outcome, exactly when the four accuracies are; the diagnostics'
  `accuracy` account is unchanged. Their diagonal is finite and
  non-negative wherever they are filled.
- `Fusion::Algorithm` becomes `batch-temperature-bias-v10`. A record written
  under `v9` is stale and is computed again once, as a result is needed,
  shown in the status bar like any computation; nothing else about the
  record's format changes. The fit's inputs, its solution, the seventeen
  state channels, the four accuracies and the diagnostics are unchanged, and
  the goldens' existing channels remain bit for bit.

The acceleration accuracies keep their cross-covariance with the bias and
scale factors inside the pass; nothing of `Cov(x_j, g)` is published. The
position-velocity cross block is not published either: no presentation in
view needs it, and adding it later would be a bump that is hard to see the
demand for.

### 4. The derived accuracies

Three on-demand calculations under the `Fusion` sensor, whose inputs exist
only once the fit has published, so they appear with it and never start it:

| Calculation | Output | Inputs | Definition |
| --- | --- | --- | --- |
| `builtin.fusion.hAcc` | `Fusion/hAcc`, m | the `posCov` block | the square root of the larger eigenvalue of the horizontal (N, E) block: one figure for the horizontal plane, as `GNSS/hAcc` is, and the cautious one |
| `builtin.fusion.vAcc` | `Fusion/vAcc`, m | `posCovDD` | the square root of the down variance |
| `builtin.fusion.sAcc` | `Fusion/sAcc`, m/s | the `velCov` block, `velN`, `velE`, `velD` | the standard deviation along the direction of the published velocity, `sqrt(e^T Sigma_v e)` with `e` the unit velocity, where the velocity's magnitude is at least that standard deviation; otherwise (a zero velocity included) the square root of the largest eigenvalue of the block: the rule of the horizontal acceleration accuracy, applied to the speed `GNSS/sAcc` qualifies |

Each is unavailable when its inputs are absent (a fit whose covariance
failed, no fit) or differ in length, finite and non-negative otherwise. The
three are the accuracies of `Fusion/z` (vertical), of the horizontal
position that has no fused plot, as `GNSS/hAcc` qualifies none, and of
`Fusion/vel`.

### 5. The plots

Three rows at the end of the "Sensor fusion" category, after Vertical
acceleration accuracy, in the order of GNSS (Basic), each named, united and
typed as its GNSS counterpart so that the two overlay:

| Plot | Value | Unit, type |
| --- | --- | --- |
| Horizontal accuracy | `Fusion/hAcc` | m, `distance` |
| Vertical accuracy | `Fusion/vAcc` | m, `distance` |
| Speed accuracy | `Fusion/sAcc` | m/s, `speed` |

The category holds eighteen plots: the fifteen in their order, then these
three. Each is requested as every fusion plot is: checking one creates
demand for the fit, a stored fit draws it at once, a logbook column over it
works as over any fusion value, and it is absent, like the four, for a
recording whose fit did not compute the covariance. The items that count the
category's plots are restated "(as amended)", as the fused speed plots
restated them.

### 6. Colours

Each row is the quiet member of the fused value it qualifies (the same hue,
less saturated, at the other edge of the lightness band) and kin of its GNSS
counterpart, read in the GNSS quality group of `docs/PLOT_COLOURS.md`
section 5, which gains the three. The group now has seven plots, three of
them quiet, and the quiet colours cluster near grey: a joint search over the
three slots finds no trio at the kin floor of 15 against every other plot of
the group and every fused value. The best the band allows is a floor of 11,
which is where the shipped set's own accuracy pairs sit (fused `accDAcc`
against fused `accD`, 11), and the colours below reach it while keeping each
row's story; a trio that scores one point better abandons the hues.

| Plot | Colour | OKLCH (L, C, H) | Contrast on `#ffffff` / `#242424` | Why |
| --- | --- | --- | --- | --- |
| Horizontal accuracy | `#c57fa6` | 0.68, 0.10, 346 | 3.02 / 5.15 | the quiet rose at the upper edge: the quiet member of fused Horizontal speed (15), kin of GNSS Horizontal accuracy (15) |
| Vertical accuracy | `#08a2af` | 0.65, 0.11, 205 | 3.09 / 5.02 | the quiet cyan-teal at the upper edge: the quiet member of fused Vertical speed (11), kin of GNSS Vertical accuracy (15); 13 from fused Elevation, whose vertical it also qualifies |
| Speed accuracy | `#9460c8` | 0.59, 0.16, 305 | 4.41 / 3.52 | the violet in the middle of the band: the quiet member of fused Total speed (14), kin of GNSS Speed accuracy (11), the least saturated violet on the m/s canvas; lifted at M59's look from the quiet violet at the lower edge (`#8c619b`, 3.18 on the dark base), which read dim |

Measured separations (OKLab distance times 100): against the GNSS quality
rows 11 to 32, the closest Speed accuracy against GNSS Speed accuracy at 11;
between the three 14, 20 and 22; against the fused values other than their
own 13 to 33, the closest Speed accuracy against fused Horizontal speed and
Vertical accuracy against fused Elevation at 13. Colour-blind figures,
reported as section 8 provides: 4 to 19, the lowest where the inherited hues
already collapse (fused Horizontal accuracy against fused Vertical accuracy
4; fused Speed accuracy against Number of satellites 4). The numbers decide
admissibility; the look decides the colour: a manual step (section 8) looks
at the three beside the GNSS quality rows and beside the fused values, on
both backgrounds, and a colour that looks wrong moves within the rule with
its new figures recorded in the plot table's comment.

### 7. What the documentation says

- `docs/SENSOR_FUSION.md` section 4: the composition forms the position and
  velocity rows of `J_j` as well as the attitude's; the twelve outputs in
  the outputs table, in the navigation frame, widened by the squared factor;
  the three derived accuracies in the derived table with their definitions;
  "Accuracy" says that the composed velocity covariance, before the
  widening, grows inside a hole in the fixes and returns at the next fix,
  its growth the propagated term's (the fix states and the globals carried
  into the hole through the chain) rather than the step chain's own share,
  which is millimetric; that the composed position covariance need not
  grow, since the hole's one IMU factor ties the two fix marginals to within
  the chain's covariance and the position then follows the fixes' own
  marginals across the hole; and that the published accuracy carries the
  widening as well, a per-window factor that can move it either way, so the
  growth is stated of the composition and the published figures are
  recorded beside it. Section 2: the accuracy plots are seven, the three new
  ones named. Section 8: the validation of section 8 below, with its
  measured numbers.
- `docs/CALCULATIONS.md`: the fit's thirty-four outputs; the three
  calculations in the registration table and among the derived values; the
  category's eighteen plots in order.
- `docs/DATA_SCHEMA.md`: the record holds thirty-three measurements, the
  twelve covariance channels after the four accuracies (the seventeen alone
  for a fit whose covariance could not be computed); `v10` and why.
- `docs/COMPUTED_PLOTS.md`: the count and list; the paragraph on reading a
  fused speed beside its GNSS speed gains the fused Speed accuracy: set
  beside the receiver's, the two say how far each side of the compromise is
  trusted.
- `docs/PLOT_COLOURS.md` section 5: the GNSS quality group names the fused
  accuracies.
- `src/fusion/fusionregistration.h`: the outputs and the three calculations
  in the contract.
- Every count of fifteen fusion plots in the documents and `tests/README.md`
  becomes eighteen; the audit's pattern for stale counts gains fifteen.

### 8. Tests

- **The kernel.** On the fixtures with a computed covariance, the published
  position and velocity blocks at every sample equal the position and
  velocity marginals of a graph with a state at every edge, as the attitude
  block is held today; on a fix they equal the fix's own marginal; they are
  symmetric, with a finite, non-negative diagonal, wherever they are filled,
  and empty exactly when the four accuracies are. On `bridged_hole`, the
  composed velocity covariance before the widening is larger inside the
  hole than beside the fixes around it and returns at the next fix; the
  composed position covariance is recorded, not asserted, since it follows
  the two fixes' own marginals across the hole (the published figures, which
  carry the per-window widening, are recorded beside both),
  and the error against the generating trajectory, in units of the published
  standard deviation, is measured and recorded in section 8 of the fusion
  document, as the four accuracies' ratios are; the test holds it below a
  bound the plan sets from the measurement. The seventeen state channels and
  the four accuracies of every golden are unchanged bit for bit; the
  goldens gain the twelve channels.
- **The registration and the record.** Thirty-three outputs in order; the
  twelve are published and stored with the rest and restored bit for bit
  after a restart; a record written under `v9` is stale at load and the fit
  runs again once; the stored success without the covariance restores the
  seventeen and leaves the sixteen accuracy channels unavailable.
- **The derived accuracies** (`tst_fusion_derived` pattern): known answers
  on chosen blocks (a diagonal block, a rotated one whose larger eigenvalue
  is exact, a velocity along one axis and one below its standard deviation),
  unavailable without the blocks and on unequal lengths, waiting on the fit
  and never starting it.
- **The rows** (`tst_fusion_rows` pattern): eighteen in order; the three
  typed and backed by the fit; absent where the covariance failed, with the
  fit's other plots drawn; one row alone starts one fit; a column over Speed
  accuracy fills loaded and unloaded rows, formatted by its type.
- **The audit.** `naming`: the pinned count is eighteen, with the tests'
  mirror; the documents' pattern refuses fifteen. `accuracy-channels`: the
  twelve covariance names are spelled in the kernel's result and the
  registration alone, the three derived names in the registration and the
  plot rows alone, and the count of accuracy rows is seven.
- **Manual**, one step in `tests/README.md` section 12: the three rows in
  place; one fit for the three; each absent for the recording whose
  covariance fails, with the fit's other plots drawn; the colours beside the
  GNSS quality rows and beside the fused speeds and Elevation, in both
  themes; the legend and the measure tool; a Speed accuracy column; after
  the update a stored `v9` fit is computed again once, shown in the status
  bar.
- The acceptance items in the next free hundred of `tests/acceptance_map.txt`,
  with their appendix and section in `tests/README.md`, and the items that
  count the category's plots restated "(as amended)".

### Commit Policy

Michael has authorized commits for this plan on the existing working branch.
Nothing is ever pushed, and nothing is ever committed on `master` or
`fusion-improvements`. One commit per accepted phase, subject
`Fused accuracies: <phase>`, with the session's attribution line, staged by
explicit path; `PLANS/` is never staged.

## Phases

The specification draws three boundaries: what the fit publishes (section
3), what is derived from it (section 4) and what the user sees (sections 5
and 6). The plan cuts along them.

| # | Name | Purpose | Depends on |
| --- | --- | --- | --- |
| 1 | The fit publishes the covariance blocks | The reconstruction forms the position and velocity rows of `J_j` and turns the two blocks into the navigation frame; the output stage widens them by the squared factor and publishes twelve channels after the four accuracies; `Fusion::Algorithm` becomes `v10`; the registration's output table, the record, the goldens, the runner and the tests' mirror of the channel names follow; the kernel tests of section 8 (the edge graph, the fix marginal, `bridged_hole` with its measured ratio); the documents that describe the fit's outputs and the record; the acceptance range 1701-1717 opened with items 1701-1706 | nothing |
| 2 | The derived accuracies | `builtin.fusion.hAcc`, `vAcc` and `sAcc` as on-demand calculations under the `Fusion` sensor, with their known-answer tests and the registration-shape tests; the derived-names audit rule; the registration table and derived-values text of the documents; items 1707-1709 | 1 |
| 3 | The plots | The three rows in their colours, the tests' plot mirror, the rows and column tests, the audit's counts (eighteen plots, seven accuracy rows, the documents' pattern refusing fifteen), every count of fifteen fusion plots, the user-facing documents, the manual step M59, the restated items, and items 1710-1717 | 2 |

Every phase blocks the next; nothing runs in parallel. Phase 2's
calculations read channels only phase 1 publishes (an input no calculation
declares has no candidate, and the "waits on the fit" tests cannot pass);
phase 3's rows draw measurements only phase 2 produces (a row over a name
nothing produces is not explicit-backed, and `tst_fusion_rows` fails). All
three edit `src/fusion/fusionregistration.cpp` or its header, and 2 and 3
both edit `tests/audit/cleanup_audit.cmake`, `tests/acceptance_map.txt` and
`tests/README.md`, so they could not overlap even if they were independent.

```text
1 The fit publishes the covariance blocks -> 2 The derived accuracies -> 3 The plots
```

## Key patterns and references

Read each in full unless the line says which part.

### The kernel (phase 1)

- `src/fusion/fusion.h`: `Result` (the seventeen arrays, then the four
  accuracies with their contract comment: the twelve join them), the
  `run()` contract (the covariance step and the reconstruction compose the
  accuracy in one pass), `Algorithm` (`v9`, and the comment that says when
  it changes).
- `src/fusion/trajectoryreconstruction.h`: `ImuRateTrajectory` (the
  attitude covariance and the four unwidened accuracies, "with a computed
  covariance only"), and the long comment on `reconstructAtImuRate()` that
  states the composition (`J_j`, `Sigma_j`, "Only the attitude rows are
  formed", a sample on a fix has the fix's own marginal). The position and
  velocity blocks are the same formula with more rows.
- `src/fusion/trajectoryreconstruction.cpp`: the composer class near the top
  (the `N` matrix that takes the graph's tangent to the NavState's, the
  three-row `J` blocks built from `byStart`, `byFitted`, `byBias`, `byScale`
  and `K`), and the publication loop near the end where
  `attitudeCovariance` is rotated into the navigation frame with
  `R*joint.block<3, 3>(0, 0)*R.transpose()` and the four accuracies are
  pushed. The frames: `Sigma_j` is in the forward state's tangent, where the
  translation and the velocity are both in the body frame, so each block is
  turned as the attitude's is; the graph's `V(k)` is NED, which is what `N`
  accounts for on the input side.
- `src/fusion/fitcovariance.h` / `.cpp`: `FitCovariance` (the tangent's
  layout: Pose3 tangent, rotation then translation in the body frame, then
  `V` in NED), `kBiasOffset`, `kSlopeOffset`, `kScaleOffset`, the attitude
  and acceleration accuracy formulas, `wideningFactors()`, `widening()`.
  Nothing here changes; it is the vocabulary the composition uses.
- `src/fusion/fusionoutput.h` / `.cpp`: `fillOutputChannels()` (the
  seventeen, then the four accuracies each multiplied by the widening `w`,
  heading and tilt capped), `accuracyObject()` (the diagnostics' `accuracy`
  account, unchanged by this change), the file comment that counts the
  channels.
- `src/fusion/fusion.cpp`: the success assembly (`reconstructAtImuRate`
  with the covariance, the widenings from the residuals, then
  `fillOutputChannels` and `successDiagnostics`).
- `src/fusion/factorgraphfit.h`: `FitResult`, `kYawSigmaCapDeg` (the cap
  that applies to the attitude accuracies and to nothing here).

### The registration, the record and the tooling (phase 1)

- `src/fusion/fusionregistration.cpp`: `kFitOutputs` (the one table of the
  fit's outputs, name beside `Result` member, "for the declaration and the
  publication"), `fitOutputs()`, `fitOutputChannels()`, `registerFit()`
  (`resultVersion` is `Fusion::Algorithm`).
- `src/fusion/fusionregistration.h`: the contract of
  `registerFusionCalculations` (the fit's "twenty-two outputs published
  together", the four accuracies, the success whose covariance failed) and
  the comment of `fitOutputChannels()` ("twenty-one measurement outputs").
- `tests/fusion/fusiongolden.h` / `.cpp`: `fusionChannelNames()` (the
  golden column order, its own table beside `Result` members) and
  `fusionChannel()`, the comparator and its per-channel floors
  (`comparatorHoldsItsBounds` in `tst_fusion_golden.cpp` pins them), the
  channels-file format. The twelve columns need a floor each; the
  covariance of a position is a different magnitude from a sigma.
- `tests/fusion_golden_capture.cpp`: the capture tool; it writes
  `fusionChannelsText(result)` and needs no change of its own once the
  names table has the twelve. `tests/README.md` section 11 says how a
  re-capture is recorded (a dated paragraph: what moved and why).
- `tests/data/fusion/`: the fourteen fixtures; the four `*.channels.txt`
  of the successes gain twelve columns at the re-capture.
- `tests/fusion_runner.cpp`: `--csv` writes `fitOutputChannels()`; its
  help text and comments count twenty-one.
- `tests/fusion/fusionsessions.h` / `.cpp`: `fusionMeasurementNames()`
  (the twenty-one literal names in output order) and the unit table of
  `syntheticFitSession()` (`Q_ASSERT`ed to the same size: the twelve take
  `m^2` and `m^2/s^2`), `fusionNames()` ("everything" a read never starts
  the fit for), `goldenDifference()`.
- `tests/tst_fusion_kernel.cpp`: `sampleCovarianceMatchesTheEdgeGraph`
  (the attitude block and four accuracies against `jointOf(marginals,
  {X(j), V(j), B(0), S(0)})` of a graph with a state at every edge: the
  reference for the position block is `X(j)`'s translation part, in the
  body frame of the Pose3 tangent, and for the velocity `V(j)`, which the
  graph holds in NED), `sampleOnAFixHasTheFixMarginal`,
  `bridgedHoleFollowsTheTruth` (the error inside the hole in units of the
  accuracy, and the position and velocity of the sample covariance read
  through `reconstructInterval()`'s seam, which this change makes
  published), `imuRateIsWhatTheFitPublishes` (`fusionChannelNames().size()
  == 21`), `accuraciesFiniteAndPositive`, `covarianceFailureLeavesTheFitAsItIs`,
  `kAccuracyFixtures`.
- `tests/tst_fusion_golden.cpp`: `successFixturesMatchGolden` and the
  comparator test.
- `tests/tst_fusion_runner.cpp`: `outputTableMatchesGolden`
  (`fitOutputChannels()` against `fusionChannelNames()`, size 21).
- `tests/tst_fusion_session.cpp`: `registrationShape` (twenty-two outputs,
  `resultVersion` `v9`, the tail of registered ids as a literal),
  `requestRunsOnceAndPublishesTogether`, `restoredFitIsIndistinguishable`
  (the snapshot's `resultVersion`).
- `tests/tst_fusion_store.cpp`: the hand-written record literal near the
  top with `"algorithm":"batch-temperature-bias-v9"`,
  `restoredAfterRestartIsBitIdentical`, `restoredFitWithoutAccuracyDrawsTheRest`
  (the record rewritten without the four: now without the sixteen;
  `kAccuracies`), `codeStampChangeDropsRecordOnLoad` (the pattern for "a
  record under an old string is stale at load and the fit runs again
  once"; it stamps `v5`).
- `tests/tst_fusion_jobs.cpp`: the comment "All twenty-two outputs".
- `docs/SENSOR_FUSION.md` section 4: "The state at every IMU sample",
  "Accuracy" with its five starred paragraphs (the composition's formula
  block and "Only the attitude rows of `J_j` are formed"; "Widening"), the
  "Outputs" table and the paragraph after it ("The four accuracies are
  absent ... the other seventeen"); section 8: "Validating the accuracy"
  (the measured agreements and the `bridged_hole` passage: "The four
  published accuracies inside the hole do not grow ... the growth through a
  hole is in the sample covariance's position and velocity", which this
  change makes a statement about published channels), "What is and is not
  validated"; section 7 "Three steps" ("installs all twenty-two outputs")
  and "Stored results" (`v9` and why). The audit counts several of this
  document's sentences (group `accuracy-channels`): keep each one a single
  line.
- `docs/CALCULATIONS.md` section 17: the registration table ("Fourteen
  calculations are registered"; the fit's row "the 22 below"), "Outputs of
  the fit" ("twenty-two outputs, one table"), "Outcome mapping", "It is
  `batch-temperature-bias-v9` since ...".
- `docs/DATA_SCHEMA.md` section 11 (the `"records"` example with `v9`) and
  section 12 ("Since the documented noise model it holds twenty-one
  measurements"; "Validity", "Code": `v9` and why, `v8` before it).
- `tests/README.md`: the rows of `tst_fusion_golden`, `tst_fusion_kernel`,
  `tst_fusion_jobs`, `tst_fusion_runner`, `tst_fusion_session`,
  `tst_fusion_store` and every other line `grep -n "twenty-one\|twenty-two"
  tests/README.md` finds; section 11's capture history (the pattern for the
  new capture's paragraph).
- `PLANS/done/noise-model-part-1-plan/04-accuracy.md`: how the four
  accuracies were added through the same files, as the pattern this phase
  repeats with twelve more channels.

### The derived calculations (phase 2)

- `src/fusion/fusionregistration.cpp`: `registerHorizontalSpeed`,
  `registerTotalSpeed`, `registerHorizontalAcceleration` (the on-demand
  pattern: descriptor, `kSensor` inputs, one output, `unavailable()` on
  unequal lengths, the comment that says why it never starts the fit),
  `registerFusionCalculations` at the end (the order, "the vocabulary
  before its reader"). `registerTrackAcceleration` shows a calculation of
  more than two inputs. The file includes no Eigen or GTSAM header (audit
  `solver-confinement`: "public and registration files are GTSAM-free"), so
  the eigenvalue of a symmetric 2x2 or 3x3 block is written out in plain
  arithmetic.
- `src/fusion/fitcovariance.cpp`, `accelerationAccuracy()`: the rule
  `sAcc` repeats (along the published direction where the magnitude is at
  least the sigma, else the largest eigenvalue), written with Eigen there;
  the derived calculation states the same rule without it.
- `src/calculations/gnsscalculations.cpp`: how GNSS names its `hAcc`,
  `vAcc`, `sAcc` as source measurements (the same strings; the derived
  names are told apart from them by the `Fusion` sensor alone).
- `tests/tst_fusion_derived.cpp`: `syntheticFitSession()` use
  (`trackChannels`, the fit's outputs stored as data), `derivedRegistrationShape`
  (pins `{velH, vel, accH}` after the fit and `{z, accAlongTrack,
  accCrossTrack}` after the system time), `derivedValuesWaitOnTheFit`,
  `fusedSpeedsKnownAnswers` (the known-answer pattern), `kFit`, `kVelH`.
- `tests/tst_fusion_session.cpp`: `registrationShape` (the tail of
  registered ids as a literal list), `readsNeverRunTheFit`,
  `requestRunsOnceAndPublishesTogether`.
- `tests/fusion/fusionsessions.cpp`: `fusionNames()` gains the three.
- `tests/audit/cleanup_audit.cmake`, group `accuracy-channels`: the rule
  "the accuracy channels are named in the registration and the plot rows"
  (`expect_only` of the quoted names, paths registration and
  `mainwindow.cpp`) as the pattern for the derived names' rule; group
  `solver-confinement` for what the registration may include.
- `docs/CALCULATIONS.md` section 17: the registration table, "Derived
  values", "The eight derived calculations"; `docs/SENSOR_FUSION.md`
  section 4: the table "These values are derived from the outputs on
  demand"; `src/fusion/fusionregistration.h`: the contract's list.
- `tests/README.md`: the rows of `tst_fusion_derived` and
  `tst_fusion_session`; section 10's description of `accuracy-channels`.

### The plots (phase 3)

- `src/mainwindow.cpp`, `registerBuiltInPlots`: the table's head comment,
  the "GNSS (Basic)" rows for Horizontal, Vertical and Speed accuracy
  (names, units, measurements, types `distance`, `distance`, `speed`), the
  "Sensor fusion" block's comment (how each row is coloured, with figures)
  and its fifteen rows ending in Vertical acceleration accuracy. The new
  rows go after it.
- `git show d18c8fc -- src/mainwindow.cpp`: how the fused speed plots
  added three rows and their "coloured by" comment.
- `docs/PLOT_COLOURS.md` sections 4, 5 (the groups read together: "GNSS
  quality (the three accuracies and the satellite count)" gains the
  three), 7, 8 and 9. The specification's section 6 did steps 1 to 4 and 6
  of section 9 and gives the literals and figures; the manual step does
  step 5.
- `tests/fusion/fusionsessions.h` / `.cpp`: `fusionPlots()` (the mirror of
  the application's rows, counted fifteen in both comments; the audit pins
  the two to the same count).
- `tests/tst_fusion_rows.cpp`: `allFusionPlotsAreExplicitBacked` (a
  literal table of fifteen; every row drawn after one fit),
  `accuracyPlotsAreAbsentWithoutAFit` (`accuracies.size() == 4`),
  `totalSpeedRowIsBlockedByFusion` (the pattern for "one row alone starts
  one fit"), `checkAllFusionPlots`, the comments that count fifteen.
- `tests/tst_fusion_store.cpp`: `fusionColumnFillsUnloadedSessions_data`
  (one row per accuracy column, typed as its row: Speed accuracy joins
  it), `restoredFitWithoutAccuracyDrawsTheRest` (the stored success without
  the covariance: the three derived rows are absent with the sixteen).
- `tests/tst_fusion_jobs.cpp`, `noImuSessionCannotHaveAJob`: iterates
  `fusionPlots()`; its README row counts fifteen.
- `tests/tst_plot_format.cpp`: `formatValue()` by type; the `speed` and
  `distance` types are already held to their formats.
- `tests/audit/cleanup_audit.cmake`: the header bullets that count fifteen;
  group `naming` (`expect_count("fifteen fusion plots" ...)`, the rule "the
  documents describe the fifteen fusion plots" and its pattern of old
  counts); group `accuracy-channels` ("the four fusion accuracy rows",
  count 4, whose comment says "a fifth accuracy row raises the count with
  the row"); the acceptance traceability section.
- `tests/acceptance_map.txt`: the header, the block of 1601-1612 at the
  end (item form), and every item whose statement counts the fusion plots
  (`grep -n fifteen tests/acceptance_map.txt`).
- `tests/README.md`: section 9.17 and appendix Q (the pattern for a
  section and appendix; phase 1 opens 9.18 and appendix R), section 12's
  preamble ("Fifteen scripts"), 12.15 with M58 (the pattern for the manual
  section), M39, M44 and M51 (the steps that list the category), section
  10's description of `naming`, and every line `grep -n fifteen
  tests/README.md` finds that counts the fusion plots.
- `docs/COMPUTED_PLOTS.md` section 1 (the count and list; the paragraph on
  reading a fused speed beside its GNSS speed, which gains the fused Speed
  accuracy); `docs/SENSOR_FUSION.md` section 2 (the category's list, "All
  fifteen plots") and section 8's row for `tst_fusion_rows`;
  `docs/CALCULATIONS.md` section 17 ("All fifteen are requested", "the
  fifteen real plots").
- `PLANS/done/fused-speed-plots-plan/01-fused-speed-plots.md`: the last
  plan's phase document for three new rows, as the pattern for the scope
  of this phase.

## Decisions and constraints

- **Three phases, one commit each.** The Commit Policy allows a commit per
  accepted phase. Each phase leaves the tree green: build, tests, audit,
  acceptance map.
- **The registration's output table and the record belong to phase 1, not
  to a phase of their own.** `tst_fusion_runner::outputTableMatchesGolden`
  holds `fitOutputChannels()` to the goldens' columns, and the goldens are
  captured from `Result`; a kernel that publishes twelve more arrays
  without the registration declaring them, or goldens without them, cannot
  be green. So the kernel, the output table, the goldens, the runner, the
  record tests and the documents that describe the fit's outputs move
  together, as the noise model's phase 4 moved the four accuracies.
- **The composition keeps its split.** The reconstruction composes the
  unwidened blocks in the navigation frame beside `attitudeCovariance`
  (two more members of `ImuRateTrajectory`, one 3x3 per sample each), and
  `fillOutputChannels()` widens by `w*w` and splits each into six channels,
  as it widens the four by `w` today. The widening is applied once, there;
  nothing downstream widens. The exact member names inside the kernel are
  the implementer's; the twelve published names are the specification's.
- **The edge-graph reference's frames.** In `sampleCovarianceMatchesTheEdgeGraph`
  the joint of `X(j)` carries the translation in the Pose3 tangent (body
  frame) and `V(j)` in NED. The position block's reference is turned with
  the solution's attitude as the attitude block's is; the velocity block's
  is not. The documenter of phase 1 states this so the implementer does not
  rotate the wrong block.
- **The `bridged_hole` bound.** The specification asks the plan to set the
  bound from the measurement, which does not exist before the
  implementation. Phase 1's document sets the rule: the test asserts the
  error inside the hole, in units of the published standard deviation, below
  3 for both position and velocity (the bound the four accuracies' hole test
  already uses), and the implementer records the measured maxima in
  section 8 of `docs/SENSOR_FUSION.md` beside the four's; if the
  measurement is far below 3, the documenter may tighten the bound in the
  document and the reviewer checks the recorded figure against it.
- **Every count of the fit's channels is updated.** The specification names
  the documents (thirty-four outputs, thirty-three measurements). Comments
  and `tests/README.md` lines that count twenty-one or twenty-two are
  updated with them, in phase 1, since a stale count is the defect the
  audit's naming rule exists to catch for the plots. An acceptance item
  whose statement counts the channels (230, 235, 337, 912 and any other
  `grep -n "twenty-one\|twenty-two" tests/acceptance_map.txt` finds) is
  restated "(as amended)" in the map and its appendix; an item whose
  evidence text alone counts them has the evidence text updated. Michael
  may confine the restatement.
- **The stale-`v9` test.** `codeStampChangeDropsRecordOnLoad` stamps `v5`,
  the string users' logbooks hold. The specification wants a record under
  `v9` shown stale at load with the fit run again once; phase 1 adds that
  as a data row or a sibling of the same pattern, the documenter's call,
  and keeps the `v5` row.
- **The derived accuracies are registered after `builtin.fusion.accH` and
  before `builtin.fusion.systemTime`, in the order `hAcc`, `vAcc`, `sAcc`.**
  That keeps both assertions of `derivedRegistrationShape` (`{velH, vel,
  accH}` after the fit; `{z, accAlongTrack, accCrossTrack}` after the
  system time) true, and the three sit beside the other magnitudes derived
  from the fit's outputs. `tst_fusion_session::registrationShape`'s literal
  tail, the registration table of `docs/CALCULATIONS.md` and the header's
  contract follow. Michael may prefer them at the end of the on-demand
  block.
- **The eigenvalue is written in plain arithmetic.** The registration file
  includes no Eigen (audit `solver-confinement`); the larger eigenvalue of
  a symmetric 2x2 block and the largest of a symmetric 3x3 are closed
  forms. The known-answer tests choose blocks whose eigenvalues are exact.
- **The derived names' audit rule matches the `Fusion` spelling.** `"hAcc"`,
  `"vAcc"` and `"sAcc"` are also the GNSS names, spelled in
  `gnsscalculations.cpp`, the GNSS rows and Plots-menu items of
  `mainwindow.cpp`, and the `Channels` input table of the registration
  itself. The rule the specification asks for ("the three derived names in
  the registration and the plot rows alone") must therefore match the
  sensor-qualified form (`kSensor, "hAcc"` in the registration, `"Fusion",
  "hAcc"` in a plot row), never the bare string. Phase 2 writes the rule
  with `mainwindow.cpp` allowed, so that it is already true when phase 3
  adds the rows. The twelve covariance names' rule is phase 1's; the files
  that legitimately spell them (the `Result` members in `fusion.h`, their
  filling in `fusionoutput.cpp`, the output table) are either allowed by
  path or the rule matches the quoted form, which only the output table
  has.
- **Acceptance items are fixed by number here.** The next free hundred is
  1701. The audit's completeness loop requires every item of a declared
  range to have an automated line, so the range grows with the phases:
  phase 1 opens 1701-1706 (the audit's comment, range check and
  completeness loop; the map's header, "Eighteen specifications, eighteen
  ranges"; section 9.18 and appendix R of `tests/README.md`), phase 2
  extends it to 1709, phase 3 to 1717. The subjects are listed under
  "Interfaces between phases"; the statements are the documenters'.
- **Every item whose statement counts the fusion plots is restated** "(as
  amended)" in phase 3, as the fused speed plots did (115, 509, 801, 802,
  842, 854, 862, 863, and now 1602, 1608 and 1610 among the 1601
  items; `grep -n fifteen tests/acceptance_map.txt` is the list). An item
  whose evidence alone counts them has its evidence text updated. Michael
  may confine the restatement.
- **The test guide's numbering.** Section 9.18 and appendix R for items
  1701-1717; manual section 12.16 with step M59; section 12's preamble
  counts sixteen scripts.
- **Colours are the specification's literals.** The three hex values and
  their figures come from section 6 and go into the plot table's comment.
  The manual step may move a colour within the rule; that is Michael's step
  after the commit, not the implementer's.
- **The fit's arithmetic is untouched.** No test's expected numbers for the
  seventeen channels, the four accuracies or the diagnostics change; the
  goldens' existing columns are bit for bit the previous capture's, which
  the re-capture note records. A review that sees a change to
  `factorgraphfit.*`, `imuintegration.*`, `fitcovariance.*`,
  `initializer.*` or `scaledimufactor.*` in phase 1's diff rejects the
  phase; `fitcovariance.h` is read, not changed.

## Interfaces between phases

### Phase 1 provides

- In `src/fusion/fusion.h`, twelve `QVector<double>` members of
  `Fusion::Result` after `accDAcc`, named `posCovNN`, `posCovNE`,
  `posCovND`, `posCovEE`, `posCovED`, `posCovDD`, `velCovNN`, `velCovNE`,
  `velCovND`, `velCovEE`, `velCovED`, `velCovDD`: the upper triangles of
  the position (m^2) and velocity (m^2/s^2) covariance blocks of the
  published state in the navigation frame (north, east, down), widened by
  the squared widening factor, aligned with `time`; filled exactly when the
  four accuracies are, empty otherwise. `Fusion::Algorithm` is
  `batch-temperature-bias-v10`.
- `kFitOutputs` and `fitOutputChannels()` list them in that order after
  `accDAcc`, published as `Fusion/posCovNN` ... `Fusion/velCovDD`:
  thirty-three measurement outputs, thirty-four outputs with the
  diagnostics attribute.
- `fusionChannelNames()` (goldens) and `fusionMeasurementNames()` (the
  tests' mirror, with units `m^2` and `m^2/s^2`) list the thirty-three in
  the same order; the four success goldens carry the twelve columns.
- The acceptance range 1701-1717 declared in the audit's range check and
  comment, with items 1701-1706 complete in the map, section 9.18 and
  appendix R opened in `tests/README.md`, the map's header counting
  eighteen specifications. The subjects: 1701 the twelve outputs (section
  3: names, meaning, frame, the squared widening, filled exactly when the
  four are, finite non-negative diagonal); 1702 `v10` and what is
  unchanged (section 3: the stale `v9` record, the seventeen, the four and
  the diagnostics bit for bit, the cross blocks not published); 1703 the
  kernel tests (section 8, first bullet); 1704 the registration and the
  record (section 8, second bullet); 1705 the documents of the fit's
  outputs and the record (section 7: `SENSOR_FUSION.md` sections 4 and 8,
  `CALCULATIONS.md`'s thirty-four, `DATA_SCHEMA.md`'s thirty-three and
  `v10`, the header's contract, the counts of channels); 1706 the audit's
  rule for the twelve names (section 8, audit bullet, first clause).

### Phase 2 provides

- Three on-demand calculations under the `Fusion` sensor, registered after
  `builtin.fusion.accH` and before `builtin.fusion.systemTime`, in this
  order, no title, no result version:
  - `builtin.fusion.hAcc`: output `Fusion/hAcc` (m), inputs
    `Fusion/posCovNN`, `posCovNE`, `posCovEE`; the square root of the
    larger eigenvalue of the horizontal block.
  - `builtin.fusion.vAcc`: output `Fusion/vAcc` (m), input
    `Fusion/posCovDD`; its square root.
  - `builtin.fusion.sAcc`: output `Fusion/sAcc` (m/s), inputs the six
    `velCov` channels and `Fusion/velN`, `velE`, `velD`; the standard
    deviation along the published velocity where the speed is at least
    that standard deviation, else the square root of the largest
    eigenvalue.
  Each unavailable when an input is absent or the inputs differ in length;
  finite and non-negative otherwise.
- `fusionNames()` gains `Fusion/hAcc`, `Fusion/vAcc`, `Fusion/sAcc`.
- The `accuracy-channels` rule for the derived names, allowing the
  registration and `mainwindow.cpp`.
- Items 1707-1709 complete, the range extended to 1709: 1707 the three
  calculations (section 4); 1708 their tests (section 8, third bullet);
  1709 their documents and the derived-names audit rule (section 7: the
  derived table, the registration table and derived values, the header's
  contract; section 8, audit bullet, second clause).

### Phase 3 provides

- Plot rows "Horizontal accuracy" (`Fusion/hAcc`, m, `distance`,
  `#c57fa6`), "Vertical accuracy" (`Fusion/vAcc`, m, `distance`,
  `#08a2af`), "Speed accuracy" (`Fusion/sAcc`, m/s, `speed`, `#8c619b`),
  in the "Sensor fusion" category after Vertical acceleration accuracy, in
  that order; `fusionPlots()` mirrors them; the category holds eighteen.
- The audit's counts: eighteen fusion plots, seven accuracy rows, the
  documents' pattern refusing fifteen.
- Items 1710-1717 complete, the range closed at 1717: 1710 the rows
  (section 5: names, units, types, order, eighteen); 1711 demand (section
  5: requested as every fusion plot, a stored fit draws them, a column,
  absent where the covariance failed); 1712 the colours (section 6); 1713
  the documents (section 7: `COMPUTED_PLOTS.md`, `PLOT_COLOURS.md` section
  5, `SENSOR_FUSION.md` section 2, `CALCULATIONS.md`'s eighteen, every
  count of fifteen, the audit's pattern); 1714 the rows' tests (section 8,
  fourth bullet); 1715 the audit (section 8, audit bullet: `naming`'s
  eighteen and mirror, the pattern refusing fifteen, `accuracy-channels`'
  seven); 1716 the manual step M59 (section 8, manual bullet); 1717 the
  acceptance items, appendix R, section 9.18 and the restated items
  (section 8, last bullet).
- Manual section 12.16 with step M59; section 9.18 and appendix R
  complete.

## Commit Policy

Michael has authorized commits for this plan on the existing working branch.
Nothing is ever pushed, and nothing is ever committed on `master` or
`fusion-improvements`. One commit per accepted phase, subject
`Fused accuracies: <phase>`, with the session's attribution line, staged by
explicit path; `PLANS/` is never staged.
