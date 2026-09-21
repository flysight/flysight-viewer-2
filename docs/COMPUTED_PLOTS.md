# Plots that are computed on request

1. [Why some plots need computing](#1-why-some-plots-need-computing)
2. [What a row can show](#2-what-a-row-can-show)
3. [Starting a computation](#3-starting-a-computation)
4. [Cancelling](#4-cancelling)
5. [When you stop needing a result](#5-when-you-stop-needing-a-result)
6. [When results go away](#6-when-results-go-away)
7. [When a track cannot be computed](#7-when-a-track-cannot-be-computed)
8. [While computing](#8-while-computing)
9. [Known limitations](#9-known-limitations)

## 1. Why some plots need computing

Most plots are instant: check one and it is drawn for every visible track.
A few plots take from seconds to several minutes per track to work out. Today
these are the plots of the "Sensor fusion" category
([what they are](SENSOR_FUSION.md)). FlySight Viewer computes them only when
you ask, in the background, so that nothing you do by accident - opening the
application, showing a track, switching a profile - starts minutes of work.

A check mark therefore means "show this plot wherever its data is available".
It does not mean "compute it". A plot that is checked but not yet computed for
some tracks is an ordinary state: the plot simply does not draw those tracks
yet, and its row in the plot list tells you so.

## 2. What a row can show

At the right-hand end of a checked plot's row in the plot list:

| You see | It means |
| --- | --- |
| A refresh icon (circular arrows) with a number | That many visible tracks have not been computed for this plot. Press the icon to compute them |
| "k of n" with a circled x | Computing. Of the n tracks this row was waiting for, k are done. The circled x cancels |
| A warning triangle with a number | That many visible tracks could not be computed (section 7). It can appear next to either of the other two |
| Nothing | Everything that can be shown is shown - or the plot never needs computing. Such a row looks exactly as it always has |

Hover over the row for details. The tooltip has up to three parts:

- **Computing** - the tracks being worked on: the current step of the one that
  is running, and "queued" for those waiting their turn;
- **Not computed (press refresh to compute)** - the tracks behind the number
  on the refresh icon;
- **Could not be computed** - the tracks behind the warning, each with the
  reason.

Several rows can show the same progress at the same time. Roll, pitch and yaw,
for example, come from one and the same computation per track, so they wait
for the same thing, and one computation fills them all in.

Only visible tracks count. A track whose recording lacks the needed sensor
data never appears in any number or tooltip.

## 3. Starting a computation

There are exactly two ways:

- **Check the plot** in the plot list, by clicking its check box or by pressing
  Space on the selected row. Every visible track that is not computed yet is
  queued.
- **Press the refresh icon** on the row. The same, for the tracks that are
  missing now.

As each track finishes, its graph appears by itself, the legend and any logbook
column that uses the value fill in, and the number on the row falls.

Nothing else starts a computation. In particular:

- starting the application with such plots already checked;
- applying a profile that checks them;
- toggling a plot through the Plots menu or its keyboard shortcut;
- showing a track, or selecting other tracks;
- importing or merging files;
- editing a recording.

After any of these the row simply shows the refresh icon with the number of
tracks that are not computed. Press it when you want them.

## 4. Cancelling

The circled x stops the computations that row is waiting for: the ones still
queued are removed, and the one that is running stops at its next opportunity,
normally within a moment. The plot stays checked. Its tracks count as "not
computed" again, so the refresh icon returns with their number. Other rows
that were waiting for the same computations change in the same way. Nothing
is unchecked for you.

## 5. When you stop needing a result

If you uncheck a plot or hide a track while computations are waiting,
computations that have not started yet and that no checked plot on a visible
track still needs are dropped. The one that is already running is left to
finish, and its result is kept: hiding a track for a moment does not throw
away minutes of work. Only the circled x, or quitting, stops a running
computation.

## 6. When results go away

- **When the recording's data changes** - a re-import, a merge of another file
  into the recording, a changed input - results that depended on the old data
  are discarded and the track is "not computed" again. If that happens while
  the track is being computed, that computation is stopped - its result could
  not be used - and the row shows the refresh icon at once instead of the
  cancel control. Nothing is recomputed automatically; pressing refresh computes the track
  from the new data as soon as the old computation has stopped.
- **When you quit.** Results are not saved. After a restart the plots are
  still checked and the rows show the refresh icon.
- Moving markers, zooming, panning and changing display settings never discard
  a result.

## 7. When a track cannot be computed

Some recordings cannot be computed: for sensor fusion, for example, one with a
gap in its sensor data. The row then shows the warning triangle, and the
tooltip names the track and gives the reason. No message box appears.

There is no "try again" for such a track, because the same data would give the
same answer. When the recording's data changes (section 6), the track becomes
"not computed" again and the refresh icon offers it.

This is different from a track that lacks the needed sensor altogether - a
recording without IMU data, for sensor fusion. That track is silently absent
from the plot, as it is from every other plot whose sensor it lacks: no number,
no warning.

## 8. While computing

The application stays fully usable: you can pan and zoom, show and hide
tracks, edit recordings, set markers, and import files. One computation runs at
a time, in the order they were requested; the others wait. Quitting stops
them; expect a short wait while the running one reaches a point where it can
stop.

## 9. Known limitations

- **Refresh and cancel have no keyboard access.** There is no shortcut, menu
  item or context menu for them. From the keyboard you can start a computation
  by unchecking and re-checking the selected row with Space; you cannot cancel
  one.
- There is no window that lists computations. The rows and their tooltips are
  where progress and failures are reported.
- A logbook column based on a computed plot is blank for recordings that are
  not loaded, and after a restart until the plot has been computed again.
