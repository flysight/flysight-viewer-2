# Sensor fusion

1. [Summary](#1-summary)
2. [Using it](#2-using-it)
3. [Inputs](#3-inputs)
4. [Model and output contract](#4-model-and-output-contract)
5. [Initialization and limitations](#5-initialization-and-limitations)
6. [What is rejected](#6-what-is-rejected)
7. [Calculation lifecycle](#7-calculation-lifecycle)
8. [Validation](#8-validation)

## 1. Summary

Sensor fusion is a batch GNSS/IMU fit: one GTSAM factor graph over the whole
recording, solved once, giving position, velocity, acceleration and
orientation at the IMU's sample rate in a fixed north/east/down frame.

To the application it is one registered calculation, titled "Sensor fusion".
It is expensive (seconds to minutes per recording), so it never runs by
itself: it runs only when you ask for it from the plot list, in the
background, while the application stays usable.
[Plots that are computed on request](COMPUTED_PLOTS.md) explains the controls.
The fit always covers the whole recording; zoom and markers do not select a
fit window. No Python runtime is needed.

How the calculation is registered with the engine (ids, declared inputs,
outcome mapping) is in [CALCULATIONS.md](CALCULATIONS.md), section 17. This
document describes what is computed, from what, and how far to trust it.

## 2. Using it

- The plot list has a "Sensor fusion" category with seventeen plots: north,
  east and down position, velocity and acceleration, horizontal acceleration,
  roll, pitch and yaw, and the four quaternion components.
- A recording needs matching `TRACK.CSV` and `SENSOR.CSV` data (GNSS and IMU
  with a shared time base) and at least one GNSS fix with a horizontal accuracy
  under 10 m. A recording without them is simply absent from these plots, like
  any plot whose sensor a recording lacks.
- Check a plot, or press the refresh icon on its row. A fit takes from seconds
  to several minutes, depending on the length of the recording. All seventeen
  plots of one recording come from the same fit, so it runs once.
- Results are kept in memory only. After a restart the plots are checked but
  not computed; press refresh.

## 3. Inputs

The fit declares twenty-one inputs, all required, and reads nothing else:

```
GNSS/_time
Local/north  Local/east  Local/down  Local/velN  Local/velE  Local/velD
GNSS/hAcc    GNSS/vAcc   GNSS/sAcc
IMU/_time
IMU/ax  IMU/ay  IMU/az  IMU/wx  IMU/wy  IMU/wz
_LOCAL_ORIGIN_INDEX  _LOCAL_ORIGIN_LAT  _LOCAL_ORIGIN_LON  _LOCAL_ORIGIN_HMSL
```

**Effective values only.** Like every calculation, the fit reads effective
values: the recorded data after the conversion layer. Accelerations arrive in
m/s^2 whatever unit the file used, and the gyroscope channels of a recording
without `SCHEMA_VER` carry the legacy correction
([DATA_SCHEMA.md](DATA_SCHEMA.md), section 4). Rates are degrees per second at
the boundary of the fit and are converted to radians per second once, inside
it.

**Shared frame.** Position and velocity come from the recording-wide local
frame, `Local/...`, and its origin attributes
([LOCAL_COORDINATES.md](LOCAL_COORDINATES.md)): the first GNSS fix with valid
coordinates and a horizontal accuracy under 10 m fixes the position and the
orientation of one north/east/down frame for the whole recording. CSV hMSL is
used as the height input of the GeographicLib transform. hMSL is height above
mean sea level rather than ellipsoid height, so the frame is approximate; this
is documented, not corrected. The fit reads imported measurements only and does
not look for a `RAW.UBX` file.

**Shared time.** Both sensors are read on the application's shared UTC time
(`GNSS/_time`, `IMU/_time`). The fit subtracts one common epoch for its solver
coordinates and adds it back for the output. There is no clock model or clock
compensation of its own.

That shared time fit once lost precision at realistic device uptimes because
its least-squares sums were not centered: tens of milliseconds on an exact
synthetic clock, and up to about 10 ms on a real recording. The sums are
centered now, inside the existing `_TIME_FIT_A` / `_TIME_FIT_B` calculation
that every sensor uses; `tst_time_fit` holds the conversion of an exact
synthetic clock at high uptime to the microsecond level.

## 4. Model and output contract

The GTSAM graph has a body-to-fixed-NED pose and a velocity at each GNSS fix,
one shared accelerometer bias, and one shared gyroscope bias. Every fix has
position and velocity factors. Adjacent states are connected by standard
`ImuFactor` preintegration. There are no attitude, stationary, zero-velocity or
magnetic measurement factors. The zero-centered bias prior has sigmas
0.3 m/s^2 and 0.03 rad/s; stationary averages only initialize the free
variables.

IMU integration splits at exact GNSS boundaries and original IMU timestamps,
using linearly interpolated midpoint inputs. Batch Levenberg-Marquardt uses QR,
100 iterations per pass, a relative cost-change threshold of 1e-8, and up to
five bias reintegrations. Convergence requires a settled cost and bias shifts
below 1e-5 m/s^2 and 1e-6 rad/s. Reported factors are reintegrated at the final
bias. Modeling noise densities are 0.015 m/s^2/sqrt(Hz) and
0.001 rad/s/sqrt(Hz), with integration covariance I x 1e-8. These are modeling
weights, not sensor specifications.

Dense orientation follows bias-corrected gyro increments with a distributed
correction in the fixed NED frame to reach the next optimized attitude.
Acceleration is `R * (specific_force - accelerometer_bias) + [0, 0, 9.80665]`.
It has no added low-pass filtering and is not a GNSS velocity derivative. This
reconstruction is not a full IMU-rate smoothing posterior.

**Outputs**, published together under the sensor `Fusion`:

| Measurement | Meaning |
| --- | --- |
| `_time` | UTC seconds: the original IMU samples inside the half-open fitted interval `[first GNSS fix, last GNSS fix)` |
| `north`, `east`, `down` | position in the local frame, metres; the optimized GNSS states interpolated linearly onto `_time` |
| `velN`, `velE`, `velD` | velocity, m/s, interpolated the same way |
| `accN`, `accE`, `accD` | acceleration, m/s^2 (the display layer may show g) |
| `roll`, `pitch`, `yaw` | degrees, unwrapped |
| `qx`, `qy`, `qz`, `qw` | the body-to-NED quaternion in x/y/z/w order |

All arrays align with `_time`. Roll, pitch and yaw unwrap successive angles by
adding or subtracting 360 degrees, with the same rule as the GNSS course
(`src/calculations/anglehelper.h`). They keep the first angle and accumulate
turns over the whole fit, independently of zoom or markers. That removes
wrap-boundary jumps; Euler-angle singularities remain.

Two values are derived from the outputs on demand, and appear with them:
`Fusion/accH`, the horizontal magnitude of `accN` and `accE`, and
`Fusion/_system_time`, the device-time axis of `Fusion/_time`.

The attribute `_FUSION_DIAGNOSTICS` is compact JSON. After a successful fit
its top-level keys are `algorithm`, `input` (the input audit: counts, epoch,
origin, height and time method), `initialization`, `stationary_interval_s`,
`anchor_time_s`, `start_s`, `end_s`, `gnss_states`, `imu_outputs`,
`objective`, `orientation` (the output convention, as text), `residuals`
(one entry per position and velocity factor), `selected_heading_deg`, `seeds`
(per starting heading: biases, convergence, iterations, objective, residual
RMS), `seed_comparison_performed`,
`max_seed_vs_selected_angle_deg`, `max_seed_vs_selected_acceleration_m_s2`,
`max_endpoint_correction_deg`, `display_position_velocity` and `limitations`.
When the recording was rejected or the solver failed it is
`{"algorithm", "failure"}` with the reason. A successful stop describes the
optimizer's numerical behaviour, not an independent accuracy assessment.

## 5. Initialization and limitations

The initializer scans 30-second windows every 5 seconds throughout the
recording and ranks accepted windows by force variability, then time. A
qualifying window can have a nonzero constant velocity vector. Relative to its
per-axis 10% trimmed mean velocity, the 95th percentile vector deviation must
be at most 0.3 m/s and the uncertainty-normalized deviation at most 3.5. The
difference between the two half-window mean velocities must be at most
0.3 m/s. Velocity uncertainty remains limited to 0.5 m/s (95th percentile).
Coverage, gyro variability, force variability and drift, and gravity
plausibility gates also apply. Constant speed through a turn is not constant
velocity.

The gravity anchor propagates forward or backward to the start of the graph
using the initial gyro bias. If no window passes, the coarse first-fix
GNSS/force tilt initializer is used. The fit starts with one 0 degree
world-vertical heading offset; heading remains free during optimization. It
does not assume a known mounting heading or equate GNSS course with sensor
orientation. No preliminary short-fit initializer or heading search is
implemented.

**Numerical convergence does not establish physical accuracy.** In the
diagnostic recording `24-09-07/08-35-23` no stationary window passed and the
coarse initializer produced a poor converged fit, with large residuals
(position and velocity RMS of 25.9 m and 7.5 m/s) and an accelerometer bias
near 19 m/s^2. The stopping test can treat a no-update step and a zero bias
shift as settled. That behaviour is part of the frozen algorithm. Heading
ambiguity, local minima, the shared constant-bias assumption and sampling
limits remain material limitations. Inspect the diagnostics and the physical
plausibility of a result before interpreting it.

## 6. What is rejected

The fit refuses input it cannot use. A rejection is a result: the measurement
outputs are unavailable, `_FUSION_DIAGNOSTICS` carries the reason, and the plot
row shows a warning badge with that reason.

- a non-finite value in any input channel;
- a GNSS accuracy (sigma) that is not positive;
- timestamps that are not finite and strictly increasing;
- input columns of one sensor that differ in length;
- fewer than three GNSS fixes and two IMU samples, or fewer than three GNSS
  fixes inside IMU coverage;
- an IMU gap longer than 1.6 times the median IMU interval;
- a GNSS gap longer than max(2 s, 5 median GNSS intervals): a disconnected
  recording is unavailable rather than joined into one trajectory;
- a local origin index outside the GNSS samples.

A solver that does not converge is reported the same way.

GNSS fixes outside IMU coverage are trimmed, not rejected. A recording with no
IMU data, no local origin, or no shared time fit is not rejected either: it has
a *missing input*, there is nothing to compute, and it is silently absent from
the fusion plots.

## 7. Calculation lifecycle

**Unavailable until requested.** The fit has explicit policy: until it has been
requested for a recording, every fusion value of that recording is unavailable,
and no read ever starts it. Plots, the legend, the measure tool, logbook
columns, the map, exports and plugins all read "unavailable" and move on.

**The plot is the request.** Checking a fusion plot by hand, or pressing the
refresh control on its row, creates one job per visible recording that lacks
the result. Nothing else does: not restoring checked plots at start-up, not a
profile, not showing a track, not an import.

**Three steps.** *Prepare*, on the main thread, resolves and captures the
twenty-one inputs. *Compute* runs on the application's one worker thread, which
has a 64 MiB stack for GTSAM's deep elimination trees, and sees the captured
inputs and nothing else: no session, no engine, no preference. *Publish*, back
on the main thread, installs all eighteen outputs at once; ordinary
invalidation then repaints the plots, the legend and everything else that had
read "unavailable". The engine itself refuses a result whose inputs changed
while it was being computed, and it knows that at the moment of the change: the
job queue asks the fit to stop there and then (it stops at its next
cancellation boundary, below, instead of running for minutes towards a result
nobody can use). Such a job ends as superseded, nothing is published or
cached, and the row shows the refresh control again at once; a refresh queues
a new fit, which starts when the old one has stopped.

**Cancellation** is observed at four kinds of boundary: during preparation,
before each candidate window of the search for a stationary interval (the one
part of preparation that grows with the recording beyond a few single passes;
nothing is reported there, so the progress texts begin with the fit); before
the fit starts; every 256 states of graph construction; and before each
optimizer iteration. A linear solve in progress finishes first. A cancelled fit
publishes nothing and caches nothing.

**Outcomes.** A rejection (section 6) and a solver failure are functions of the
inputs, so they are cached like any result: the row shows the warning badge,
and nothing offers a retry until an input changes, because the same inputs
would give the same answer. Running out of memory, or failing to start the
worker, is not a function of the inputs and is never cached: the recording
stays "not computed".

**Invalidation.** A change to a declared input (a re-import, a merge, a changed
`SCHEMA_VER`, a changed origin) drops the result and every value derived from
it. Moving markers, zooming, and display preferences do not.

**One at a time.** Jobs run one after another in the order requested. Quitting
cancels them and waits for the running fit to reach its next cancellation
boundary: at most one solver step.

**Logbook columns** over fusion values show the live value for a loaded
recording and are never cached for unloaded ones, because the result is not
saved with the recording.

## 8. Validation

The algorithm is frozen: for identical inputs this implementation must produce
the same objective, biases, residuals, output timestamps and output channels as
the reference implementation on the branch `sensor-fusion-clean-port`
(revision `83a64479fd4e7e2e10bce0b5477c5dd7a49dee7d`). That is demonstrated by
tests, all labelled `fusion`:

| Test | What it holds |
| --- | --- |
| `tst_fusion_parity` | the kernel reproduces the goldens captured from the reference for twelve synthetic fixtures (three fits, nine rejections) |
| `tst_fusion_kernel` | the kernel's stages, and the fit trace iteration by iteration against the goldens |
| `tst_fusion_session` | the registered calculation on real sessions: reads never run it, one request publishes everything, rejections are cached results |
| `tst_fusion_jobs` | the real fit through the job queue: supersede, cancel, rejection, shutdown |
| `tst_fusion_rows` | the plot rows with the real fusion plots, end to end |

The goldens live in `tests/data/fusion/`. In exact mode
(`FLYSIGHT_FUSION_EXACT=1`, on the capture configuration) every output sample
must equal the golden bit for bit; the portable mode used everywhere else
allows `1e-9 + 1e-7 * |golden|`. The fixtures, the two modes and the capture
procedure are described in [tests/README.md](../tests/README.md), section 11.

**Optional check against a real recording.** Real recordings are not in the
repository, so this is a local check and not a CI test. Set
`FLYSIGHT_FUSION_RECORDING` to a folder that contains `TRACK.CSV` and
`SENSOR.CSV` and run the test function directly:

```powershell
$env:FLYSIGHT_FUSION_RECORDING = "D:\recordings\17-26-24"
.\tst_fusion_jobs.exe realRecordingCheck
```

Reference numbers from the branch for recording 17-26-24: objective
65602.22485051976, 9247 GNSS states, 24411 outputs. The counts should match as
they are. The objective matches only with a copy of `SENSOR.CSV` that carries
`$VAR,SCHEMA_VER,2`: the reference predates the legacy gyroscope correction,
so it read the gyro channels of an unmarked file literally. With the unmodified
file a different objective is expected and correct.
