# Plot colours: one colour per plot, readable on both backgrounds

Date: 2026-10-05.
Status: specification of a change made directly, after a survey of the
built-in plots and a trial set reviewed by Michael. It has no acceptance
items of its own; it amends items 1037 and 1101.
Baseline: branch `store-requested-calculations` at `355ef86`.
Related: `src/mainwindow.cpp` (`registerBuiltInPlots()`, the one table of
default colours), `src/plotutils.h` (`plotColor()`, the one colour a plot is
drawn in: the user's choice, else the default), `tests/audit/cleanup_audit.cmake`
(groups `naming`, `accuracy-channels`, `gnss-acceleration-accuracy`),
`docs/SENSOR_FUSION.md` section 2, `docs/CALCULATIONS.md` section 18.

## 1. Motivation

The default colours grew by hue arithmetic in HSL: a family at red, green
or blue, steps of forty degrees, a deeper tier for accuracies. A survey of
the 56 built-in plots showed what that left. They shared 38 colours, and in
each of the three triads the total was drawn in exactly the colour of the Y
component. Hue steps in HSL are uneven to the eye: the gyro triad's three
greens could not be told apart, and GNSS vertical acceleration and the fused
along-track acceleration were the same green on the same unit. Only 29 of
the 56 reached a contrast of 3:1 on both the white and the dark plot
background: bright cyan Course all but vanished on white, navy Drag on dark.

## 2. Principles

- **One colour per plot.** The same colour in the light and the dark theme:
  one colour to choose for a new plot, one preference for the user, and a
  plot that looks like itself in both.
- **Readable on both backgrounds.** Every default reaches a contrast of 3:1,
  the WCAG ratio, on `#ffffff` and on the Fusion style's dark base,
  `#242424`.
- **Coloured by what a plot is read with.** A variation of a quantity that
  has a plot is **kin**: a component of a triad, the fused form of a GNSS
  plot, a corrected form such as the wind-corrected horizontal speed. Kin
  take a neighbouring hue, or the same hue quieter: close enough to belong
  together, far enough to separate where the lines cross. A different
  quantity read on the same canvas is a **contrast**: a hue well away from
  the others it is read with. Horizontal, vertical and total speed are the
  model contrast. An accuracy is the quiet member of the value it
  qualifies, and a triad's total the neutral member of its family.
- **The Plots menu keeps its hues.** Its fifteen plots are the ones users
  know by colour. Their hue stays; lightness and saturation move only as
  far as the two rules above need. Four are nudged in hue, by Michael's
  ruling, where two menu plots shared one: Lift within green, Drag within
  blue, and Course and Course rate apart within cyan.
- **A judgement, not a formula.** No rule computes a colour from a plot's
  position in a table. The table states each colour, and its comment states
  what a new plot is coloured by.

## 3. What the rule costs

A colour that passes on white and on dark can only sit in a narrow range of
lightness, about 0.54 to 0.67 in OKLCH for every hue. Hue and saturation
therefore do the separating, and only about a dozen colours exist that are
clearly different from one another. With 56 plots, colours repeat across
categories; they do not repeat inside a group of plots that are read
together. Pairs meant to contrast sit closer than they did where both were
visible, typically 25 against 35 in OKLab distance times 100, and remain
plainly different hues. Kin pairs improved: 26 of 34 are separable, against
17 before.

## 4. What changes

- `registerBuiltInPlots()` states every default as a literal colour. The
  HSL scheme and its constants are removed, and so are the Qt named colours
  of the menu rows. The table's comment states the principles of section 2
  as the rule a new plot follows.
- The gyro triad moves from green to a violet family, clear of Course rate
  and Dive angle rate, which share its unit. The accelerometer triad stays
  warm and the magnetometer triad blue.
- The accuracies stop being "the same hue, much deeper", which is what
  failed on the dark background, and become the quiet members of their
  values. Items 1037 and 1101, which named the deep scheme and a distance of
  forty degrees, are restated as amended.
- Specific energy and its rate have colours of their own instead of the
  colours of Lift and Drag.
- Nothing else changes: `plotColor()` remains the one authority, a user's
  stored colour still wins, and plots registered by Python plugins bring
  their own colours.

A colour already written to a user's settings is not replaced: the
preferences store each default at the first start, so an existing
installation keeps its colours until they are reset. Storing only what the
user chose is a separate change.

## 5. Tests and documentation

- The cleanup audit keeps the table honest where a text rule can: no plot
  row computes its colour from a scheme or names a Qt colour; the four
  fusion accuracy rows and the GNSS acceleration accuracy row exist as
  literal-coloured rows.
- The 3:1 contrast and the separations were measured when the colours were
  chosen and are not checked by a test: the table lives in the main window,
  outside the test library. Moving the table into the core, where a unit
  test could hold every default to the contrast rule, is a follow-up.
- `docs/SENSOR_FUSION.md` and `docs/CALCULATIONS.md` say how the accuracy
  plots are drawn; `tests/README.md` restates items 1037 and 1101 and the
  audit bullets.
- Manual step M51 already asks that each accuracy line be told apart from
  the lines it is read beside, in the light and the dark theme.
