# Fused speed plots

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

## 1. Motivation

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
the speed with the IMU's dead reckoning in it.

A divergence is the fit's compromise between its two sources, not an
independent IMU measurement of speed, and the documentation says so. The
receiver's Speed accuracy plot says how far to trust its side of the
compromise; the document describes the separation and promises no accuracy.

## 2. What is published

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

## 3. Colours

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

## 4. What the documentation says

- `docs/COMPUTED_PLOTS.md`: the category's count and list; one paragraph on
  reading a fused speed beside its GNSS speed, in the terms of section 1:
  the two lie together where the receiver is accurate and separate where
  it is not, the separation is the fit's compromise between the receiver
  and the IMU, and the receiver's Speed accuracy plot says how far to trust
  its side of it.
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

## 5. Tests

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

## Commit Policy

Michael has authorized commits for this plan on the existing working branch.
Nothing is ever pushed, and nothing is ever committed on `master` or
`fusion-improvements`. One commit, subject `Fused speed plots`, with the
session's attribution line, staged by explicit path; `PLANS/` is never
staged.
