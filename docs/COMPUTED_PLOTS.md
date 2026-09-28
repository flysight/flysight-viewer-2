# Plots and logbook columns that are computed in the background

1. [Why some values need computing](#1-why-some-values-need-computing)
2. [What the status bar shows](#2-what-the-status-bar-shows)
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

## 2. What the status bar shows

The status bar at the bottom of the main window is where FlySight Viewer shows
the work it does in the background. What is running is shown at its left, with
a small bar and, when the work can be stopped, a cancel button; what could not
be done is shown at its right. It is empty while nothing is running and
nothing has failed, and it is always there, so the window's layout does not
jump when work starts or ends.

| You see | It means |
| --- | --- |
| "Computing results: k / n" and a bar | Computing. n recordings have been waiting to be computed since the status bar last had none, and k of them are done. Each recording counts once, however many plots and logbook columns want it |
| "Saving sessions", "Loading sessions", "Updating sessions" or "Computing columns", with its count and bar | Other background work of the logbook (section 8) |
| A warning triangle and, for example, "2 recordings could not be computed" | Some recordings could not be computed (section 7). It is shown beside "Computing results" while computing continues, and alone once it ends |
| Nothing | Nothing is running and nothing has failed |

Hover over the label or the bar for details: it lists everything in progress,
each with its own count, and under "Computing results" the recording being
computed and its current step. A cancel button appears beside the bar only for
work that can be stopped; computing results never has one (section 5).

Plot rows show nothing about computing: a row over a computed plot looks
exactly as any other row. A track whose result is still to come is absent
from the plot until it arrives, and appears by itself when it does.

One computation per recording can serve several plots. Roll, pitch and yaw,
for example, come from one and the same computation, so one computation fills
them all in, and the recording is counted once.

A track whose recording lacks the needed sensor data is never listed among
the recordings that could not be computed. A plot never counts it; a logbook
column counts it only until the recording has been loaded to find that out
(section 4).

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
count of "Computing results" in the status bar advances.

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
time, as hidden recordings. The status bar shows the fill as "Computing
results: k / n", with no cancel button, counted together with what the plots
are computing. The logbook's other background work, such as filling a column
that needs no computing ("Computing columns: k / n"), saving or loading, goes
first and is shown first (section 8).

A column header looks as any other header while its column fills. A recording
that could not be computed shows the warning triangle right after the text in
the first cell of its row (section 7).

A cell whose value is still to come shows a grey "…". A blank cell means the
value does not exist for that recording (for sensor fusion, a recording
without IMU data). FlySight Viewer learns that only by loading the recording:
until the fill has loaded it, such a recording shows "…" and is counted in
"Computing results" in the status bar, and then turns blank and leaves the
count.
Only computed results are kept, so it is loaded again, once, at every start.
Sorting by the column puts "…" and blank cells together at the bottom.

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
  again at the next start and is computed again as soon as something switched
  on needs it.
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
gap in its sensor data. As soon as such a failure is found, not once the work
is done, two things show it:

- the status bar shows a warning triangle and the number of recordings that
  could not be computed ("1 recording could not be computed"). A recording
  with two calculations that failed counts once. While computing continues
  the warning stands beside "Computing results", so a failure found early in
  a long fill is visible at once; when nothing is computing it stands alone;
- the recording's row in the logbook shows the warning triangle at the left
  of its first cell, whichever column that is (moving or hiding columns moves
  it with them). A row without a failure looks exactly as before and takes no
  room for it.

Hovering over the status bar's warning lists the recordings, in the order of
the logbook's rows, each with the calculation that could not be computed and
the reason; it lists ten and then says how many more there are. Hovering over
a row's triangle lists that recording's calculations and reasons in the same
form. The row's cells over the calculation that failed are blank: the
triangle says why. No message box appears.

Nothing offers to try again, because the same data give the same answer. When
the recording's data change (section 6), it is computed again.

A few failures are not about the data: the computer ran out of memory, the
recording's file could not be read, or a computed result could not be kept
(the disk is full, or the logbook's `cache/` folder cannot be written). They
are shown the same way, with "(tried again at the next start)" after the
reason: they are not tried again while FlySight Viewer runs, and are tried
again the next time it starts.

A result that could not be kept is listed the same way, with the reason
("Couldn't write file ..."): while the recording stays loaded the plot still
draws it, but the status bar's warning and the recording's row show it, the
recording is not computed again while FlySight Viewer runs unless its data
change, and the next start computes and keeps it again.

**What stays visible after a restart.** A failure about the data is kept with
the recording in the logbook's `cache/` folder, and nothing else is written.
At the next start it shows again without loading the recording or computing
anything, as soon as the plot or column that wants it is restored and the
logbook has read its columns in the background. A failure that is not about
the data is not kept: it is tried again at the next start and shows only if it
fails again, so a disk that has since been freed, or a file that has since
been restored, clears it.

The warning is about what you have switched on, and it shows for as long as
the recording cannot be computed. It clears when its cause does: the
recording's data change, or the last plot or column that wants the
calculation is switched off (switching it on again shows the warning again,
without computing). Nothing dismisses it by hand, and clicking it does
nothing.

This is different from a track that lacks the needed sensor altogether - a
recording without IMU data, for sensor fusion. That track is silently absent
from the plot, as it is from every other plot whose sensor it lacks: no number,
no warning.

## 8. While computing

The application stays fully usable: you can pan and zoom, show and hide
tracks, edit recordings, set markers, and import files. Computations run one
at a time, in the
background, at a lower priority than the rest of the application: first the
track you are looking at (the focused one), then the other visible tracks
from top to bottom, then what logbook columns need, from top to bottom.
Quitting stops them; expect a short wait while the running one reaches a
point where it can stop.

The status bar shows one thing at a time. The logbook's other background work
(saving, loading, updating recordings after an edit of many at once, filling a
column that needs no computing) comes first, with its own count and, where it
can be stopped, a cancel button; "Computing results" returns when it ends.
Hover over the status bar to see both.

## 9. Known limitations

- There is no window that lists computations. The status bar is where
  progress and the warning are shown, and the logbook rows where each
  recording's failures are shown.
- The logbook cannot be sorted or filtered by the row warning.
- There is no switch that pauses background work. To stop it, remove the
  column or uncheck the plot.
- On Linux the background computation does not run at a lower
  operating-system priority. The application stays responsive all the same.
