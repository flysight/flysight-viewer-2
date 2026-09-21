# Shared local GNSS coordinates

Every recording has one recording-wide north/east/down (NED) frame. The
**GNSS (Local frame)** plots show it, and other calculations that need metric
coordinates build on it instead of projecting the track themselves.

## Channels and attributes

`Local/north`, `east`, and `down` are metres in that frame. `Local/velN`,
`velE`, and `velD` are the recorded per-fix GNSS velocity vectors rotated into
the same frame: a FlySight records velocity in the north/east/down axes *at the
fix*, which are not the axes at the origin once the track has moved away from
it. The recorded geographic and velocity channels (`GNSS/lat`, `GNSS/velN`, ...)
are preserved unchanged.

The calculated attributes `_LOCAL_ORIGIN_LAT`, `_LOCAL_ORIGIN_LON`, and
`_LOCAL_ORIGIN_HMSL` identify the frame's position and orientation, and
`_LOCAL_ORIGIN_INDEX` the GNSS sample they were taken from. The three
coordinates are that sample's recorded values, unrounded.

## The origin

The origin is the first fix with latitude in [-90, 90], longitude in
[-180, 180], finite hMSL, and finite horizontal accuracy `0 <= hAcc < 10 m`.
Horizontal accuracy selects the origin only: later fixes get coordinates
whatever their accuracy.

A recording with no qualifying fix has no frame: all ten outputs (the four
attributes and the six channels) are unavailable.

The following do **not** affect the frame: markers (exit, analysis range,
ground elevation, ...), zoom, display preferences, and speed and speed
accuracy. Only a change to the GNSS source data moves it.

## One calculation

One on-demand registered calculation, `builtin.local.coordinates`, produces
the four attributes and the six channels together, in one atomic result, so the
origin attributes and the channels always describe the same frame. It runs once
per recording however many of its outputs are read. It is cheap: it needs no
job and no request, and the plots that show it are ordinary plots.

Its declared inputs are `GNSS/lat`, `GNSS/lon`, `GNSS/hMSL`, `GNSS/hAcc`,
`GNSS/velN`, `GNSS/velE`, and `GNSS/velD`. All seven are required and must have
the same length. A change to any of them invalidates all ten outputs; the next
read computes the frame again.

## Invalid samples

Each channel has exactly one entry per GNSS sample, so the channels stay
aligned with the GNSS time axes and with each other:

- A sample whose position is invalid (non-finite latitude, longitude, or hMSL,
  or latitude or longitude off the globe) is NaN in all six channels at that
  index.
- A sample whose position is valid but whose velocity has a non-finite
  component is NaN in the three velocity channels only.

An invalid sample never shortens or shifts the arrays.

## Time axes

`Local/_time` and `Local/_system_time` are the GNSS axes themselves
(`GNSS/_time` and `GNSS/_system_time`): shared, not refitted. They come from two
small calculations of their own, `builtin.local.time` and
`builtin.local.systemTime`, so that a recording without a TIME sensor, which
has no system-time axis, still has its positions and its UTC axis.

## The transform and its approximation

GeographicLib (`LocalCartesian`) performs the transform on the WGS84
ellipsoid, and its rotation matrix at each fix rotates the velocity. CSV hMSL is
used as an approximate ellipsoid height, without geoid correction. This is
documented, not corrected: the channels are a local-frame approximation and do
not reproduce reference results that used ellipsoid heights. Within one
recording the geoid separation is practically constant, so the effect on
displacements is small; the absolute height of the frame is off by the local
geoid separation.

## Validation

`tst_local_coordinates` covers the origin gates, analytically known
displacements and velocity rotation, NaN placement, the no-origin case, the
time axes, and invalidation. The golden rows for the descent fixture are in
`tests/support/builtinfixture.cpp`.

## Simplified map track

The map draws `Simplified/lat`, `lon`, `hMSL` and `_time`: the recorded
geographic samples and their UTC time. `Simplified/north`, `east` and `down`
are the same samples in the local frame. One on-demand calculation,
`builtin.simplified.track`, produces all seven; its inputs are `GNSS/lat`,
`lon`, `hMSL`, `_time` and `Local/north`, `east`, `down`.

The simplification is a horizontal Ramer-Douglas-Peucker on `Local/north` and
`Local/east` with a tolerance of 0.5 m: a sample survives only when it is
strictly further than that from the segment it would be dropped onto. The
calculation performs no projection and has no second origin. The projection
runs once per recording, in the local-coordinates calculation.

It retains sample indices. Every output is the recorded sample at those
indices, all seven outputs always have the same length, and distinct samples
at the same position (a stationary start or end) stay distinct. There is no
coordinate re-matching.

A sample whose local coordinates are not all finite is left out of the
simplified track and the path is joined across it. One invalid fix does not
cost the recording its track, and the track ends at the first and last samples
that have coordinates.

With no qualifying origin the simplified track is unavailable (a missing
input). The map then shows no track and no cursor dot for that recording and
leaves it out of its bounds; when no visible recording has a track, the model's
bounds are cleared and the map keeps its current view. There is no fallback to
an unsimplified or privately projected track: a recording in which no fix ever
reaches 10 m horizontal accuracy has no track worth drawing. Correcting the
source brings track, dot and bounds back through ordinary invalidation.

`tst_simplified_track` covers index alignment, the tolerance,
duplicate-position endpoints, closed, degenerate and empty tracks, non-finite
samples, the single projection, and the no-origin case with recovery.
`tst_map_models` covers the two map models on a real session model. The golden
rows for the descent fixture are in `tests/support/builtinfixture.cpp`.

## Consumers

The simplified map track (above) and sensor fusion read this frame and no
other: the fusion outputs `Fusion/north`, `east`, `down` and the fused
velocities use this same frame and origin, so they can be compared with
`Local/...` directly. See [SENSOR_FUSION.md](SENSOR_FUSION.md).
