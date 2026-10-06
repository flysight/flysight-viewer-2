# Fused position and speed accuracy

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

## 1. Motivation

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

## 2. Principles

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

## 3. What the fit publishes

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

## 4. The derived accuracies

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

## 5. The plots

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

## 6. Colours

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

## 7. What the documentation says

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

## 8. Tests

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

## Commit Policy

Michael has authorized commits for this plan on the existing working branch.
Nothing is ever pushed, and nothing is ever committed on `master` or
`fusion-improvements`. One commit per accepted phase, subject
`Fused accuracies: <phase>`, with the session's attribution line, staged by
explicit path; `PLANS/` is never staged.
