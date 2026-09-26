# Plots and logbook columns that are computed in the background

1. [Why some values need computing](#1-why-some-values-need-computing)
2. [What a plot row shows](#2-what-a-plot-row-shows)
3. [What starts a computation](#3-what-starts-a-computation)
4. [Logbook columns over computed values](#4-logbook-columns-over-computed-values)
5. [When you stop needing a result](#5-when-you-stop-needing-a-result)
6. [When results go away](#6-when-results-go-away)
7. [When a track cannot be computed](#7-when-a-track-cannot-be-computed)
8. [While computing](#8-while-computing)
9. [Known limitations](#9-known-limitations)

## 1. Why some values need computing

Most plots and logbook columns are instant: check a plot and it is drawn for
every visible track, add a column and it fills in. A few take from seconds to
several minutes per recording to work out. Today these are the plots of the
"Sensor fusion" category and any logbook column over a Sensor fusion value
([what they are](SENSOR_FUSION.md)).

FlySight Viewer computes them in the background for what you have switched
on: a checked plot for the tracks that are visible, and a logbook column for
every recording in the logbook. There is nothing to press. Switching a plot or
a column on is the request; switching it off drops what has not started yet.
A result, once computed, is kept with the recording (section 6), so it is
computed only once.

## 2. What a plot row shows

At the right-hand end of a checked plot's row in the plot list:

| You see | It means |
| --- | --- |
| A turning arc with "k of n" | Computing. Of the n visible tracks that can be computed for this plot, k are done (a track that could not be computed counts as done) |
| A warning triangle with a number | Computing has finished, and that many visible tracks could not be computed (section 7) |
| Nothing | Everything that can be shown is shown - or the plot never needs computing. Such a row looks exactly as it always has |

The arc and the triangle are never shown together: the triangle appears once
the work is done.

Hover over the row, the arc or the triangle for details. The tooltip has up to
two parts:

- **Computing: k of n done**, followed by the track being worked on and its
  current step;
- **Could not be computed:**, followed by each track that could not be
  computed, with the reason.

Each part lists at most ten tracks and then says how many more there are.

Several rows can show the same progress at the same time. Roll, pitch and yaw,
for example, come from one and the same computation per track, so they show
the same progress, and one computation fills them all in.

Only visible tracks count. A track whose recording lacks the needed sensor
data never appears in any number or tooltip.

## 3. What starts a computation

Anything that switches such a value on:

- checking the plot - by clicking its check box, pressing Space on the
  selected row, through the Plots menu, or by applying a profile;
- showing a track while the plot is checked;
- adding or enabling a logbook column over such a value (in the column editor,
  or by applying a profile).

Tracks whose result was kept from earlier (section 6) are drawn at once and
are not computed again. As each other track finishes, its graph appears by
itself, the legend and any logbook column that uses the value fill in, and the
count on the row rises.

When you start FlySight Viewer every track is hidden, so checked plots start
nothing until you show tracks. A logbook column over such a value continues
filling at once (section 4).

After you edit a recording's data, the track waits about a second after your
last edit before it is computed again, so a series of edits costs one
computation.

## 4. Logbook columns over computed values

A logbook column over a computed value (roll at the exit marker, say) is
filled for every recording in the logbook, whether or not it is shown.
Recordings that are not loaded are loaded in the background, at most two at a
time, as hidden recordings. The progress line under the logbook shows
"Computing columns: k / n" for the whole fill, with no cancel button.

While a column fills, its header shows the turning arc at the right of its
name. Hovering the header shows the counts, the recording being computed with
its current step, and any recordings that could not be computed with their
reasons. When the work is done the arc goes, or the warning triangle takes its
place.

A cell whose value is still to come shows a grey "…". A blank cell means the
value does not exist for that recording (for sensor fusion, a recording
without IMU data). Sorting by the column puts "…" and blank cells together at
the bottom.

Visible tracks go first: a track you show while a column fills is computed
next, after the computation that is running. Removing or disabling the column
stops further work; the computation that is running finishes and is kept.

Applying a profile that includes such a column fills it for the whole
logbook, which can take a long time on a large logbook;
none of the profiles that come with FlySight Viewer includes one.

## 5. When you stop needing a result

Unchecking a plot, hiding a track, or removing a column drops the
computations that have not started, at once. The one that is running is left
to finish and its result is kept: hiding a track for a moment does not throw
away minutes of work. There is no refresh and no cancel; quitting stops the
running computation.

## 6. When results go away

- **When the recording's data changes** - a re-import, a merge of another file
  into the recording, a changed input - results that depended on the old data
  are discarded. If that happens while the track is being computed, that
  computation is stopped - its result could not be used. While the plot is
  checked and the track visible, or a column needs it, the track is computed
  again from the new data a moment after your last edit.
- **Hiding a track or quitting loses nothing.** Results are kept with the
  recording in the logbook, in its `cache/` folder. When the track is shown
  again, also after a restart, the plot is drawn at once, without computing.
- **Deleting the logbook's `cache/` folder** while FlySight Viewer is closed is
  safe: the recordings are untouched, and every such track is "not computed"
  again at the next start; every such track is computed again as soon as
  something switched on needs it.
- **If a kept result cannot be read** when its track is shown (another program
  has its file open, for example), the track is "not computed" for that time
  only: the file is kept, and the result comes back the next time the track is
  loaded (while it cannot be read, a checked plot or a column computes the
  track again).
- **After an update of FlySight Viewer** that changes how such a plot is
  computed, kept results are discarded when their track is next loaded. The
  track is "not computed" again.
- **Changing settings the computation does not read** (altitude markers, other
  preferences, Python plugins it does not use) never discards a kept result.
- Moving markers, zooming, panning and changing display settings never discard
  a result.

## 7. When a track cannot be computed

Some recordings cannot be computed: for sensor fusion, for example, one with a
gap in its sensor data. Once the work is done the plot row, or the column
header, shows the warning triangle, and the tooltip names the recording and
gives the reason. No message box appears.

Nothing offers to try again, because the same data give the same answer. When
the recording's data change (section 6), it is computed again.

A few failures are not about the data: the computer ran out of memory, or the
recording's file could not be read. They are shown the same way, are not tried
again while FlySight Viewer runs, and are tried again the next time it starts.

This is different from a track that lacks the needed sensor altogether - a
recording without IMU data, for sensor fusion. That track is silently absent
from the plot, as it is from every other plot whose sensor it lacks: no number,
no warning.

## 8. While computing

The application stays fully usable: you can pan and zoom, show and hide
tracks, edit recordings, set markers, and import files. Computations run one
at a time, in the background, at a lower priority than the rest of the
application: first the track you are looking at (the focused one), then the
other visible tracks from top to bottom, then what logbook columns need, from
top to bottom. Quitting stops them; expect a short wait while the running one
reaches a point where it can stop.

## 9. Known limitations

- There is no window that lists computations. The plot rows, the column
  headers and their tooltips are where progress and failures are reported.
- There is no switch that pauses background work. To stop it, remove the
  column or uncheck the plot.
- On Linux the background computation does not run at a lower
  operating-system priority. The application stays responsive all the same.
