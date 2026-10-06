# Plot colours

How the default colour of a plot is chosen. The colours themselves are in
one place, the plot table of `registerBuiltInPlots()` in
`src/mainwindow.cpp`; this document records the decisions that led to them,
in enough detail that a new plot can be coloured the same way and the whole
set could be derived again from scratch. It does not list the colours.

1. [What the colour has to do](#1-what-the-colour-has-to-do)
2. [One colour, two backgrounds](#2-one-colour-two-backgrounds)
3. [Readable on both](#3-readable-on-both)
4. [Coloured by what a plot is read with](#4-coloured-by-what-a-plot-is-read-with)
5. [How much room there is](#5-how-much-room-there-is)
6. [The Plots menu](#6-the-plots-menu)
7. [How the families are laid out](#7-how-the-families-are-laid-out)
8. [Measuring a colour](#8-measuring-a-colour)
9. [Adding a plot](#9-adding-a-plot)
10. [Deriving the set again](#10-deriving-the-set-again)
11. [What this document does not cover](#11-what-this-document-does-not-cover)

## 1. What the colour has to do

A plot's colour does three jobs at once. It has to be seen: a thin line on
the plot background, and the tick labels of the plot's own axis, which are
drawn in the same colour. It has to identify the plot when several are
drawn on one canvas. And it has to say how the plot relates to its
neighbours: whether two lines are two views of one quantity or two
different quantities. The guidelines below give each job a rule, and when
the rules pull against each other, they say which wins.

## 2. One colour, two backgrounds

The application draws plots on white in the light theme and on the Fusion
style's dark base, a near-black grey, in the dark theme. Each plot has one
default colour, used in both themes.

A colour per theme was considered and rejected. It would double the design
work for every new plot, need two preferences per plot where there is one,
and make a plot look like a different plot when the theme changes. The cost
of one colour is that it has to read on both backgrounds, which is the
constraint everything else here follows from.

## 3. Readable on both

Every default colour reaches a contrast ratio of at least 3:1 against both
backgrounds. The ratio is the WCAG one, and 3:1 is that standard's floor for
graphical objects (criterion 1.4.11 of WCAG 2.1); tick labels are text and
would strictly need 4.5:1, but they are secondary to the line and the line's
floor is the one kept.

The ratio compares brightness only, so it is a floor and not a verdict. The
eye resolves colour differences unevenly in fine detail: a thin line that
differs from white mainly by lacking blue, as a yellow does, fades more than
its ratio suggests, while one that lacks red, as a cyan does, keeps an edge.
Yellows and yellow-greens on white are therefore the known weak spot and get
a look by eye before they are accepted. A colour that passes the number but
looks faint is moved.

Passing on both backgrounds has a geometric consequence that shapes the
whole palette. A colour dark enough for white and light enough for the dark
base can only sit in a narrow band of perceived lightness, roughly the
middle sixth of the range, and that holds for every hue. Lightness therefore
cannot be used to tell plots apart the way it could when the two themes
were not both served. Hue and saturation do the separating.

## 4. Coloured by what a plot is read with

A plot is coloured by its relationships, not by its position in a table.
There are three.

- **Kin.** Variations of one quantity: the three components of a triad,
  the fused form of a GNSS plot, a corrected form of a plot such as the
  wind-corrected horizontal speed, an accuracy and the value it qualifies.
  Kin look related and are still told apart where their lines cross. They
  take neighbouring hues, or the same hue at a different saturation.
- **Contrast.** Different quantities that are read together on one canvas:
  horizontal, vertical and total speed are the model; the four
  accelerations; heading, pitch and roll; the four temperatures. Contrasts
  are plainly unlike: hues well apart.
- **Nothing in particular.** Plots that are never read together only have
  to avoid being identical by accident.

Within a family of kin, two roles recur. An **accuracy** is the quiet
member of its value: the same hue, less saturated, at the darker edge of
the band. A **triad's total** is the neutral member of its family: nearly
grey, with a trace of the family's hue. Both roles were once expressed by
lightness (a "deep" tier), which is what failed on the dark background; in
the band of section 3 they are expressed by saturation instead.

Similar colours are not a fault. A triad reads better as three siblings than
as three strangers; the fault is two lines that cannot be told apart.

## 5. How much room there is

Inside the band of section 3 there are only about a dozen colours that are
clearly different from one another, by the measure of section 8. There are
far more plots than that, so colours repeat across the table. The rule for
repeats is the one of section 4: a colour may be shared by plots that are
never read together, and is never shared, or nearly shared, inside a group
of plots that are. Near-twins across groups are accepted and expected.

The groups that count as "read together", as judged when the set was made:
elevation (GNSS and fused); the speeds, GNSS and fused; GNSS quality (the three accuracies
and the satellite count, with the fused Horizontal, Vertical and Speed accuracy, each
read beside the receiver's of the same name); the accelerations, GNSS and fused, with their
accuracies; the accelerometer triad; the rotation rates (the gyro triad and
the menu's course rate and dive angle rate); the angles (course, dive angle,
heading, pitch, roll and the heading and tilt accuracies); the magnetometer
triad; the temperatures; glide ratio, lift, drag and the specific energies.
Everything else is read alone.

## 6. The Plots menu

The plots that have shortcuts in the Plots menu are the ones users know by
colour. Their hues are kept as they have always been; only lightness and
saturation move, and only as far as section 3 requires. A plot that already
passed on both backgrounds was not touched.

Where two menu plots share one hue, both are moved rather than one: the pair
is compressed into the band with its order kept, the value vivid and its
accuracy quieter, so that the two stay apart instead of the moved one
landing on the other. Where that was not enough, the hue was nudged, by a
decision taken for each case: lift a step away from vertical speed within
green, drag a step away from total speed within blue, and course and course
rate apart within cyan. Bright cyan cannot reach 3:1 on white at any
saturation, so course had to come down into the band whatever else was
decided.

## 7. How the families are laid out

What the current set does, as the pattern to repeat.

- **Speeds.** Red, green and blue for horizontal, vertical and total, the
  inherited contrast. The wind-corrected horizontal speed is kin of the
  horizontal speed on the warm side of it.
- **A GNSS plot and its fused form.** The fused row takes the neighbouring
  hue at the other edge of the lightness band, so that the two lines
  separate where they overlay without looking like different quantities.
  The fused elevation, whose counterpart is grey, is a quiet blue.
- **Four quantities read together**, such as the accelerations, take four
  arcs of the hue circle, each pair of GNSS and fused rows on its own arc.
- **A triad** spans about a third of the hue circle in three roughly even
  steps, with its total nearly neutral. The three triads take three
  families: warm for the accelerometer, violet for the gyro, blue for the
  magnetometer. The gyro left green because course rate and dive angle rate,
  which share its unit, live there.
- **Heading** is kin of course, in blue beside it; pitch and roll contrast
  with it and with each other.
- **Four temperatures** take four contrasting hues, one per sensor.
- **Greys** are reserved for plots that stand alone and for the GNSS
  elevation; a repeated grey among plots that are never read together is
  harmless.

## 8. Measuring a colour

The judgements above were checked with numbers, and a new colour should be
checked the same way before it is accepted. Nothing here needs more than a
colour converter.

- **Contrast**: the WCAG ratio against `#ffffff` and against `#242424`, at
  least 3.0 on both.
- **Separation** between two colours: the Euclidean distance between them
  in OKLab, times 100. Under 15, two thin lines cannot be told apart; 15 to
  30 is the kin range; above 30 the colours are distinct. The accelerometer
  triad, which already read well before the review, sits at 16 to 30 and
  set the kin range. A contrast pair is comfortable at 25 or more, which is
  about what the band allows; the set's contrast pairs average there.
- **Colour-blind separation**: the same distance after simulating protan
  and deutan vision (the Machado, Oliveira and Fernandes model at full
  severity), taking the smaller. This figure is reported, not enforced.
  The inherited hues of the Plots menu put red against green for the speeds,
  and under that simulation red and green are about 3 apart whatever their
  lightness, so every pair that is kin to the speeds inherits the same
  figure; about a quarter of the set's kin and contrast pairs are under 8.
  Where the hues allow a choice, the candidate with the larger colour-blind
  separation is preferred, and a figure under 8 is worth a line style or a
  label when a pair is known to be read by colour-blind users. The set is
  chosen on normal-vision separation and lightness.
- **Lightness and hue** are read in OKLCH, where equal lightness looks
  equal across hues; HSL hue steps are uneven to the eye and were the
  reason the previous scheme's even steps gave uneven results.

Numbers decide whether a colour is admissible. Which admissible colour to
use is a judgement, made by looking at thin lines on both backgrounds
beside the plots the new one will be read with.

## 9. Adding a plot

1. Decide what the plot is read with: its kin, its contrasts, or neither.
2. Choose a hue by section 7: a neighbour of its kin, or a free arc among
   its contrasts. Check the table for what already uses that hue inside the
   same group; across groups a repeat is fine.
3. Choose saturation by role: vivid for a value, quiet for an accuracy,
   nearly neutral for a total.
4. Set lightness inside the band and check the two contrast ratios; adjust
   lightness until both pass, then saturation until the separations of
   section 8 hold against the plot's group.
5. Look at it as a thin line with a tick label on white and on the dark
   base, beside its group. Yellows on white get a second look.
6. Write the colour into the table as a literal. A row that computes its
   colour from a scheme, or names a Qt colour, is refused by the cleanup
   audit: those were never held to either background.

## 10. Deriving the set again

If the set were lost or had to be remade, the path that produced it was:

1. **Survey** every plot: its colour's lightness, hue and saturation in
   OKLCH, its two contrast ratios, and the separations within each group of
   plots read together, normal and colour-blind.
2. **Freeze the Plots menu's hues** and compress each same-hue pair into
   the band, order kept; nudge a hue only where a pair still collides, and
   record each nudge as a decision.
3. **Lay out the families** by section 7, choosing hue by relationship and
   saturation by role, every colour inside the band.
4. **Check** the whole set by section 8: every colour at 3:1 on both
   backgrounds, no confusable pair inside a group, kin pairs in their
   range, contrast pairs as far apart as the band allows; the colour-blind
   figures reported beside them.
5. **Look** at every colour as a thin line on both backgrounds, lowest
   contrast first, and at each group side by side; move what looks wrong,
   then check again.

The first pass of step 3 is never right; the set that shipped took three
rounds of steps 3 to 5.

## 11. What this document does not cover

- **A user's own colour.** The colour a user sets in Preferences > Plots
  wins over the default for that plot; `plotColor()` in `src/plotutils.h`
  is the one place that decides, and it is unchanged by any of this.
- **Plugin plots.** A Python plugin registers its plots with colours of its
  own choosing. These guidelines are advice to a plugin author, not a check.
- **The axes.** Each plot has its own y-axis, coloured like its line, with
  its own range. Two kin plots overlay on one canvas but not on one scale.
- **Stored defaults.** The preferences write every default into the
  settings at the first start, so a changed default reaches an existing
  installation only after its plot settings are reset.
