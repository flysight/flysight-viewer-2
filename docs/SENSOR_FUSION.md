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
orientation at the IMU's sample rate in a fixed north/east/down frame. The
solver starts from attitudes fitted segment by segment, holds a gyro bias that
follows the IMU temperature, and stops when the cost has stopped moving
(sections 4 and 5).

To the application it is one registered calculation, titled "Sensor fusion".
It is expensive (seconds to minutes per recording), so it runs only for what
you have switched on - a checked fusion plot for the visible recordings, a
logbook column over a fusion value for every recording - in the background,
while the application stays usable.
[Plots and logbook columns that are computed in the background](COMPUTED_PLOTS.md)
explains what starts and stops a fit and what the rows and columns show.
The fit always covers the whole recording; zoom and markers do not select a
fit window. No Python runtime is needed.

How the calculation is registered with the engine (ids, declared inputs,
outcome mapping) is in [CALCULATIONS.md](CALCULATIONS.md), section 17. This
document describes what is computed, from what, and how far to trust it.

## 2. Using it

- The plot list has a "Sensor fusion" category with eight plots: Elevation,
  Horizontal acceleration, Vertical acceleration, Along-track acceleration,
  Cross-track acceleration, Heading, Pitch and Roll. Each is named, united and
  typed as its GNSS counterpart (Heading as the GNSS Course), so that the two
  overlay on one axis, in a colour of its own. Elevation is the origin's
  height less the fused down position above the ground, as the GNSS elevation
  is; horizontal acceleration is the magnitude of the fused north and east
  acceleration; vertical acceleration is the fused down acceleration; the
  along-track and cross-track accelerations are the GNSS definitions applied
  to the fused velocity and acceleration; heading, pitch and roll are the
  body's attitude, as below (section 4 has the detail of each). The fit's
  other outputs (position, velocity, north and east acceleration, the device
  frame's roll, pitch and yaw, and the quaternion) have no plot and remain
  measurements: a logbook column kept from an earlier version still shows
  them, Python plugins read them, and the stored fit keeps them. A profile
  saved with a plot the list no longer has applies without it, silently.
- Heading, pitch and roll of the body the FlySight is mounted on are derived
  from the fit's attitude (its quaternion) in the aircraft convention.
  Heading is the direction of the body's forward axis, clockwise from north;
  pitch is the forward axis's elevation above the horizontal, from -90 to 90
  degrees; roll is the rotation about the forward axis, positive right side
  down, from -180 to 180 degrees. Heading is a compass heading, unwrapped by
  the rule of the GNSS course and referenced to nothing; the GNSS course is
  relative to its course reference, so the two are not meant to overlay. The body is defined by the recording's **Orientation** attribute:
  which device axes point forward and which up. The default is forward +y,
  up +z, a FlySight 2 on the back of a helmet with its label up. Preferences >
  Import chooses the orientation stored into each newly imported recording,
  for a batch recorded with the unit mounted some other way; to change one
  recording, add the Orientation column to the logbook (Add Column,
  "Session") and edit its cell, or use "Set Orientation..." from the
  logbook's context menu for the selected recordings. Every list offers the
  24 possible orientations and nothing else. The attribute describes how the unit is
  mounted and nothing about the posture of whoever wears it: the angles are
  those of the mount's body frame (for a helmet mount, of the head). A mount,
  or a posture, that points the forward axis straight up or down makes heading
  and roll meaningless there. Changing the orientation recomputes heading,
  pitch and roll at once, without a new fit.
- A recording needs matching `TRACK.CSV` and `SENSOR.CSV` data (GNSS and IMU
  with a shared time base) and at least one GNSS fix with a horizontal accuracy
  under 10 m. A recording without them is simply absent from these plots, like
  any plot whose sensor a recording lacks.
- Check a fusion plot and every visible recording without a result is fitted,
  one after another; a logbook column over a fusion value (roll at the exit
  marker, say) fits every recording of the logbook in the background. A fit
  takes from seconds to several minutes, depending on the length of the
  recording. All eight plots and every fusion column of one recording come
  from the same fit, so it runs once.
- Results are stored with the recording in the logbook (in a file in the
  logbook's `cache/` folder, never in the session file) and come back when the
  recording is loaded again, after hiding it or after a restart. A fitted
  recording is not fitted again. Deleting `cache/` while FlySight Viewer is
  closed only means that the recordings are fitted again when something
  switched on needs them.
- A stored result is dropped when an input of the fit changes (a re-import or
  merge of different data, a changed `SCHEMA_VER`, a changed local origin) and
  after an update that changes the fit's arithmetic; while a fusion plot is
  checked for it or a fusion column is enabled, the recording is fitted again
  a moment after the change. Adding or removing altitude markers, changing
  preferences and installing or editing Python plugins keep it, unless a
  plugin provides one of the fit's inputs. Nothing is fitted again unless
  something switched on needs it.

## 3. Inputs

The fit declares twenty-two inputs, all required, and reads nothing else:

```
GNSS/_time
Local/north  Local/east  Local/down  Local/velN  Local/velE  Local/velD
GNSS/hAcc    GNSS/vAcc   GNSS/sAcc
IMU/_time
IMU/ax  IMU/ay  IMU/az  IMU/wx  IMU/wy  IMU/wz  IMU/temperature
_LOCAL_ORIGIN_INDEX  _LOCAL_ORIGIN_LAT  _LOCAL_ORIGIN_LON  _LOCAL_ORIGIN_HMSL
```

**Effective values only.** Like every calculation, the fit reads effective
values: the recorded data after the conversion layer. Accelerations arrive in
m/s^2 whatever unit the file used, and the gyroscope channels of a recording
without `SCHEMA_VER` carry the legacy correction
([DATA_SCHEMA.md](DATA_SCHEMA.md), section 4). Rates are degrees per second at
the boundary of the fit and are converted to radians per second once, inside
it.

**Temperature.** `IMU/temperature` is the IMU's own temperature in degrees
Celsius, as recorded and never converted, one value per IMU sample; it is a
column of every FlySight 2 `SENSOR.CSV` and drives the gyro bias model of
section 4. A recording without the column has a missing input, like one
without IMU data (section 6).

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

The shared time base is the `_TIME_FIT_A` / `_TIME_FIT_B` calculation that
every sensor uses. Its least-squares sums are centered on the mean device time
and the mean UTC time, so the fit keeps its precision at realistic device
uptimes, where uncentered sums would lose tens of milliseconds;
`tst_time_fit` holds the conversion of an exact synthetic clock at high uptime
to the microsecond level.

## 4. Model and output contract

**The graph.** The GTSAM graph has a body-to-fixed-NED pose and a velocity at
each GNSS fix, one shared accelerometer bias, and a gyroscope bias
`b(t) = b0 + b1 (T(t) - T_ref)` of the IMU temperature `T`, with `T_ref` the
mean IMU temperature over the fitted window. Every fix has position and
velocity factors; adjacent states are connected by IMU preintegration factors
in the form that takes `b0` and `b1`, each interval evaluated at the bias of
its first fix. There are no attitude, stationary, zero-velocity or magnetic
measurement factors. The zero-centered bias priors have sigmas 0.3 m/s^2 for
the accelerometer, 0.03 rad/s for `b0` and 0.010 deg/s per degC for `b1`, the
datasheet's typical drift: a recording without a temperature change leaves
`b1` at its prior, and a well-behaved unit loses nothing. The initializer
(section 5) only chooses where the solver starts; its own prefix and segment
fits hold the gyro bias constant.

**Integration and noise.** IMU integration splits at exact GNSS boundaries and
original IMU timestamps, using linearly interpolated midpoint inputs. The
modelling noise densities are 0.015 m/s^2/sqrt(Hz) and 0.001 rad/s/sqrt(Hz),
with integration covariance `I x 1e-8`. The integration treats the IMU stream
as piecewise linear between samples. At the default 12.5 Hz output rate that is
wrong during manoeuvres by an amount that grows with the change of the signal
across a step, so each step adds a white-noise term in quadrature:
`sigma = slope x dt x |change of the interpolated signal across the step|`,
with slopes 0.026 s (gyro; `sigma` in radians) and 0.40 s (accelerometer;
m/s), and the step's covariance is `(density^2 + sigma^2 x dt) I`, i.e. a
per-step variance of `density^2 / dt + sigma^2`. The slopes were calibrated
at a 0.076 s step. The `dt` factor is what keeps them valid at higher output
rates: the sampling error falls with the square of the sample interval, and
so does the term, so at 50 Hz and above it vanishes against the density and
the model is the density-only one. A step with no signal change has the
density covariance exactly, whatever the slopes. These are modelling weights,
not sensor specifications; the slopes are reported under `model.per_step`.

**Solver and stopping.** Batch Levenberg-Marquardt uses QR, 100 iterations per
pass, a relative cost-change threshold of 1e-8, and up to five bias
reintegrations. A pass has settled when an iteration lowers the cost by at
most 1e-8 of max(1, cost). The fit has converged when the graph
re-preintegrated at the settled pass's bias changes that cost by at most
1e-6 relative (of max(1, cost)): the bias has stopped moving as far as the
preintegration can tell. A fifth pass that reaches its iteration limit
without settling is accepted when, over its last 20 iterations, the mean
relative decrease per iteration is below 1e-4 and the position and velocity
normalized RMS (root mean squared whitened residual per scalar component) are
both below 2: a slow tail is a fit that is done for any practical purpose
(four ground recordings of the reference corpus end this way with position
RMS of 0.3-0.5 m); a fit that is still descending faster, or that disagrees
with GNSS, remains a solver failure. The diagnostics name the rule that ended
the fit: `settled`, `slow tail accepted`, `iteration limit` (the last pass
reached its limit and the slow tail was refused), `bias not settled` (the
last pass settled but re-preintegrating still moved the cost) or
`cost increased` (an iteration made the cost non-finite or larger, the one
failure the fit cannot continue from). Reported factors are reintegrated at
the final bias.

**The state at every IMU sample.** The fit estimates the state at each GNSS
fix; what it publishes is the state at every IMU sample between the first and
the last fix, reconstructed one fix interval at a time from the fitted states
at the interval's two ends and the fitted biases, which it leaves as they are.
From the fitted state at the first fix the IMU is integrated forward with the
interval's bias through the fit's own preintegration: the same step
boundaries, the same midpoint readings and the same per-step noise as the
interval's IMU factor, giving a forward state at every integration edge (each
IMU sample and the two fixes). The forward state at the second fix misses the
fitted one; the mismatch is the fitted state there in the local coordinates of
the forward state, nine components of attitude, position and velocity. It is
shared over the steps as the conditional mean of the chain of steps given both
ends: the covariance and the transition of every step, read from the
preintegration as it advances, decide how much of the mismatch each step
takes, so a step that carries more noise, because the signal changed across
it, takes more, and where the noise is uniform the velocity's share grows with
elapsed time. The share is one correction of all nine components together: an
attitude share turns the direction in which every later reading was applied,
and the sharing carries that into velocity and position. It is computed once,
linearized about the forward states, and applied once. The share is nothing at
the first fix and the whole mismatch at the second, so the ends are the fitted
states exactly; inside the interval the published state is, to within that one
linearization, what a fit with a state at every IMU sample and the same
measurements would give, with the states at the fixes and the biases held
(what the one pass leaves out: section 8). Where the mismatch is zero it is
the forward integration.

**Acceleration.** For each step, the step correction `c` is the corrected
velocity change across the step divided by its length, less the mean of the
bias-corrected readings at its two edges, each rotated by the corrected
attitude at its own edge (at a fix, the interpolated reading the integration
uses), less gravity: the part of the velocity change the rotated readings do
not explain, as an acceleration. The published acceleration at a sample is
`R * (specific_force - accelerometer_bias) + [0, 0, 9.80665] + (c_before + c_after) / 2`:
the sample's own reading with the bias removed, rotated by the corrected
attitude `R`, plus gravity, plus the mean of the corrections of the two steps
beside the sample (the first and the last sample of the fitted interval have
one and take it alone; the step beside a fix is the part-step between the
sample and the fix). The reading is the sample itself, not the interpolated
midpoint value the integration uses, so nothing smooths the accelerometer's
signal: only the correction is spread, over the two steps beside each sample.
The consequence: integrated with the kernel's own rule, the published
acceleration reproduces the published velocity change over the fitted interval
as a whole, and over any run of samples to within the spread of the
corrections (the bound is in section 8), but not step by step. Where the
corrections are negligible, in steady flight, the published acceleration is
the rotated reading. It has no added low-pass filtering and is not a
derivative of the GNSS velocity.

**Outputs**, published together under the sensor `Fusion`:

| Measurement | Meaning |
| --- | --- |
| `_time` | UTC seconds: the original IMU samples inside the half-open fitted interval `[first GNSS fix, last GNSS fix)` |
| `north`, `east`, `down` | position in the local frame, metres: the fitted state at each sample (above), the fitted states at the fixes joined by the IMU |
| `velN`, `velE`, `velD` | velocity, m/s: the fitted state at each sample, like the position |
| `accN`, `accE`, `accD` | acceleration, m/s^2 (the display layer may show g): the rotated reading plus gravity plus the model's share of the correction (above); integrated, it reproduces the velocity |
| `roll`, `pitch`, `yaw` | degrees, unwrapped |
| `qx`, `qy`, `qz`, `qw` | the device-to-NED quaternion in x/y/z/w order (the fit's own device frame, where "body" means the device; it does not follow the Orientation attribute of section 2) |

All arrays align with `_time`, which is the IMU's samples whatever the GNSS
rate, also when the GNSS rate is above the IMU's: no fix time is added and
nothing is resampled. Roll, pitch and yaw unwrap successive angles by
adding or subtracting 360 degrees, with the same rule as the GNSS course
(`src/calculations/anglehelper.h`). They keep the first angle and accumulate
turns over the whole fit, independently of zoom or markers. That removes
wrap-boundary jumps; Euler-angle singularities remain. They are the angles of
the device frame: the same aircraft angles as `bodyRoll`, `bodyPitch` and
`bodyHeading` below for the orientation forward +x, up -z, except that all
three are unwrapped and yaw is measured from north, without the course
reference.

These values are derived from the outputs on demand, under the same sensor,
and appear with them; none of them starts a fit:

| Measurement | Meaning |
| --- | --- |
| `accH` | horizontal acceleration, m/s^2: the magnitude of `accN` and `accE` |
| `_system_time` | the device-time axis of `_time` (the inverse time fit) |
| `z` | elevation above the ground, metres: `_LOCAL_ORIGIN_HMSL - down - _GROUND_ELEV`, the same ground as `GNSS/z`; unavailable when either attribute is not a number |
| `accAlongTrack`, `accCrossTrack` | along-track and cross-track acceleration, m/s^2: the GNSS definitions, relative to the wind-corrected velocity (`_WIND_N`, `_WIND_E`), applied to the fused velocity and acceleration |
| `bodyHeading`, `bodyPitch`, `bodyRoll` | heading, pitch and roll of the body frame the `_ORIENTATION` attribute defines, degrees (section 2): the aircraft angles of the quaternion composed with the orientation's body-to-device rotation, published together; heading unwrapped by the course's rule and measured from north, not referenced to the course reference; the three read the quaternion and the orientation only, and are unavailable for a stored orientation that is not one of the 24 |

Vertical acceleration is `accD` itself, positive down like `GNSS/accD`; it has
no derived measurement of its own. `z` and `GNSS/z` stand on the same ground
elevation but can differ away from the origin: `down` is measured along the
origin's vertical, so a point at constant height lies lower in the frame by
about d^2/2R at a distance d from the origin (R the Earth's radius), some
0.08 m at 1 km and 2 m at 5 km (the frame is that of
[LOCAL_COORDINATES.md](LOCAL_COORDINATES.md), section 7).

The attribute `_FUSION_DIAGNOSTICS` is compact JSON. After a successful fit
its top-level keys are, grouped:

- *identity and audit*: `algorithm` (`batch-temperature-bias-v4`) and `input`
  (the input audit: `epoch_utc_s`, `imu_count`, `gnss_count`, `origin_index`,
  `origin`, `height_method`, `time_method`);
- *the initializer*: `initialization` (`segmented initialization; heading
  from segment fits`); `stationary_interval_s`, `anchor_time_s` and
  `selected_heading_deg`, all `null` (the keys remain for readers of older
  diagnostics); `initializer`, with `segment_length_s`, `segments` (one
  object per segment: `index`, `start_s`, `end_s`, `anchor_s`,
  `anchor_sacc_m_s`, `prefix_length_s`, `yaw_sigma_deg` (`null` when every
  prefix start failed), `prefix_fits`, `prefix_iterations`, `prefix_passes`,
  `prefix_on_limit`, `segment_on_limit`, `growth_stop` (`observable`,
  `covers`, `no_gain` or `all_failed`), `iterations`, `fallback`) and
  `fallback_segments` (the indices of the segments that fell back);
- *the fit*: `start_s`, `end_s`, `gnss_states`, `imu_outputs`, `objective`,
  `orientation` (the output convention, as text: the fit's own device frame,
  where "body" means the device; it does not follow the Orientation
  attribute of section 2), `residuals` (one entry per
  factor, kinds `position`, `velocity`, `imu`, `bias_prior` and
  `slope_prior`, each with `node`, `time_s` and `squared_whitened_error`),
  `seeds` (one entry, the fit that was run: `heading_deg` `null`,
  `converged`, `objective`, `iterations`, `acc_bias_m_s2`, `gyro_bias_rad_s`
  (`b0`), `position_residual_rms_m`, `velocity_residual_rms_m_s`),
  `seed_comparison_performed` (`false`), `max_seed_vs_selected_angle_deg` and
  `max_seed_vs_selected_acceleration_m_s2` (`null`);
- *the reconstruction* (above): `max_endpoint_correction_deg`, the largest
  attitude part of the mismatch over the fix intervals, degrees;
  `max_velocity_mismatch_m_s`, the largest velocity part of the mismatch over
  the fix intervals, m/s; `max_step_correction_m_s2`, the largest step
  correction of the fit, m/s^2; `max_step_correction_time_s`, the middle of
  that step, seconds since the epoch like `start_s`;
- `stopping`: `rule` (one of the five texts above), `passes`,
  `last_pass_mean_relative_decrease`, `repreintegration_cost_difference`,
  `bias_settled_tolerance`, and `slow_tail` with `window`,
  `max_mean_relative_decrease` and `max_nrms` (the thresholds in force);
- `quality`: `imu_nrms`, `position_nrms`, `velocity_nrms` (the normalized RMS
  of each factor kind's whitened residuals) and `objective_per_state`;
- `model`: `per_step` with `gyro_slope_s` and `acc_slope_s`, and `gyro_bias`
  with `b0_rad_s`, `b1_rad_s_per_degc` and `t_ref_degc`, always numbers (the
  temperature is a required input);
- `dense_output` and `limitations`, as text: `dense_output` is
  "IMU-rate reconstruction at original IMU times: between fixes the IMU integrated from the fitted state, the mismatch with the next fitted state shared over the steps by their noise, in one linearized pass", and `limitations` is
  "Local batch convergence; heading may be ambiguous. Between fixes one linearized pass with the fitted fix states and biases held; no uncertainty is published."

When the recording was rejected, or the fit stage raised anything but a
stopping-rule failure, the diagnostics are `{"algorithm", "failure"}` with the
reason. When the fit completed a pass and did not converge (`iteration limit`,
`bias not settled`) they are `{"algorithm", "failure", "quality", "stopping"}`.
When a pass raised the cost (`cost increased`) they are
`{"algorithm", "failure", "stopping"}`: there is no rebuilt graph, so no
quality. A successful stop describes the optimizer's numerical behaviour, not
an independent accuracy assessment.

## 5. Initialization and limitations

**The segmented initializer.** The fitted window is cut into consecutive
segments of 600 s from its first fix; a final piece shorter than 120 s joins
the segment before it, and a window shorter than one segment is one segment.
In each segment, independently: the fix with the smallest logged speed
accuracy (sAcc) is the anchor, the earliest on a tie (the single-fix GNSS
acceleration has an error of about seven times sAcc, so this bounds the tilt
error of the start), and the coarse attitude there aligns the measured force
with GNSS acceleration minus gravity, with zero gyro bias. That attitude has a
tilt but no yaw and no bias, so a prefix is fitted first: the window of 60 s
centred on the anchor, clipped to the segment, started from the coarse
attitude carried to the window's first fix by the gyro with zero bias, from
four heading offsets (0, 90, 180, 270 degrees about the vertical) with the
production graph and tuning. For the lowest-objective start the marginal
covariance of the window's first pose is taken from that start's graph
rebuilt at its fitted bias, its rotation block rotated into the navigation
frame, and the square root of the vertical element is the yaw sigma. While
that sigma exceeds 20 degrees, the window does not yet cover the segment, and
the last doubling cut the sigma by at least 20 %, the window doubles (still
centred on the anchor, clipped to the segment) and is fitted again; otherwise
growth stops. The whole segment is then fitted once, from the best prefix
fit's attitude at the window's first fix carried backwards to the segment's
first fix and forwards to its last with that fit's gyro bias, and the fitted
attitude of every fix and the fitted bias are kept. The full fit starts from
every segment's fitted attitudes, the first segment's gyro bias and a zero
accelerometer bias. Prefix and segment fits are ordinary fits under the
stopping rule of section 4, cancellable, with progress texts that name the
segment and, for a prefix, its length; a prefix fit is a start, not an
answer, and runs one pass of at most 50 iterations, while a segment fit uses
the production limits; a fit that ends on its iteration limit is still used
as the start, and `prefix_on_limit` / `segment_on_limit` say so. A start that
fails (a non-finite or increasing cost) counts as infinite objective; when
all four starts of a prefix fail the prefix grows; when all fail at the
segment's full length the segment's attitudes are the coarse attitude
propagated by the gyro with zero bias over that segment only, and
`initializer.fallback_segments` says so. A segment entirely at rest has no yaw
information: its prefix grows until a doubling gains nothing, and its yaw is
arbitrary. That is acceptable: yaw is unobservable there in the full fit too,
and neighbouring segments carry the heading through their own motion.
Agreement of the fitted yaw between starts is not an observability test and
is not used. Heading remains free during optimization; the fit does not assume
a known mounting heading or equate GNSS course with sensor orientation. The
Orientation attribute (section 2) is not an input of the fit either: it only
turns the fit's quaternion into heading, pitch and roll, so changing it never
refits and never makes a stored fit stale.

**Why.** One anchor attitude propagated with one bias through a long recording
ends hundreds of degrees off on a unit whose bias drifts, and a resting unit
whose bias exceeds 1 deg/s failed the previous initializer's resting-window
detector. Starting every pose near its answer converged every recording the
reference corpus (97 recordings from seven units) fitted worst in 8-17
iterations. Nothing is propagated further than a segment.

**Numerical convergence does not establish physical accuracy.** The recording
`24-09-07/08-35-23` has no resting window; the previous initializer converged
it to an objective of 14 million with a position RMS of 25 m, the segmented one
is expected to reach about 11,000 and 0.7 m in under 40 iterations. The
temperature model exists for two units of the reference corpus whose gyro bias
follows the temperature at 0.10-0.13 deg/s per degC on one axis, ten times the
datasheet's typical value; with a constant bias their fits converge with an
IMU normalized RMS of about 1.1 where their 600 s segments reach 0.2-0.5. The
stopping test can treat a no-update step as settled, and a slow tail is
accepted on numerical grounds alone. Heading ambiguity, local minima, the
shared accelerometer bias and the linear temperature model of the gyro bias,
and sampling limits remain material limitations. Not in scope: the
magnetometer is not read, and a per-unit gyro scale factor is not fitted (the
corpus shows one unit 2 % off nominal). Inspect the diagnostics and the
physical plausibility of a result before interpreting it.

## 6. What is rejected

The fit refuses input it cannot use. A rejection is a result: the measurement
outputs are unavailable, `_FUSION_DIAGNOSTICS` carries the reason, and the
recording is listed among those that could not be computed, with that reason:
in the status bar's warning and on its logbook row
([COMPUTED_PLOTS.md](COMPUTED_PLOTS.md), section 7).

- a non-finite value in any input channel (`IMU/temperature` included);
- a GNSS accuracy (sigma) that is not positive;
- timestamps that are not finite and strictly increasing;
- input columns of one sensor that differ in length (`IMU/temperature` must
  have one value per IMU sample, like every other IMU channel);
- no `IMU/temperature` handed to the kernel at all (the reason names the
  channel; inside the application this cannot happen, because a session
  without the column has a missing input and the fit never runs);
- fewer than three GNSS fixes and two IMU samples, or fewer than three GNSS
  fixes inside IMU coverage;
- an IMU gap longer than 1.6 times the median IMU interval;
- a GNSS gap longer than max(2 s, 5 median GNSS intervals): a disconnected
  recording is unavailable rather than joined into one trajectory;
- a local origin index outside the GNSS samples.

A solver that does not converge is reported the same way, with the stopping
rule that ended it in the reason (`Batch fusion did not converge (iteration
limit); sensor fusion unavailable`, or `bias not settled`, or `Nonfinite or
increasing optimizer cost`).

GNSS fixes outside IMU coverage are trimmed, not rejected. A recording with no
IMU data, no `IMU/temperature` column, no local origin, or no shared time fit
is not rejected either: it has a *missing input*, there is nothing to compute,
and it is silently absent from the fusion plots.

## 7. Calculation lifecycle

**Unavailable until requested.** The fit has explicit policy: until it has been
requested for a recording, every fusion value of that recording is unavailable,
and no read ever starts it. Plots, the legend, the measure tool, logbook
columns, the map, exports and plugins all read "unavailable" and move on.

**What is switched on is the request.** Checking a fusion plot in any way (a
click, Space, a profile), showing a recording while one is checked, or
enabling a logbook column over a fusion value creates one job per recording
that lacks the result; unchecking, hiding or disabling drops the ones that
have not started, and a running fit always finishes and is stored. At
start-up every recording is hidden, so checked plots start nothing until
recordings are shown; an enabled fusion column continues its fill at once.
Loading a recording whose stored result is still valid restores that result:
no job.

**Three steps.** *Prepare*, on the main thread, resolves and captures the
twenty-two inputs. *Compute* runs on the application's one worker thread, which
has a 64 MiB stack for GTSAM's deep elimination trees, and sees the captured
inputs and nothing else: no session, no engine, no preference. *Publish*, back
on the main thread, installs all eighteen outputs at once; ordinary
invalidation then repaints the plots, the legend and everything else that had
read "unavailable". The engine itself refuses a result whose inputs changed
while it was being computed, and it knows that at the moment of the change: the
executor asks the fit to stop there and then (it stops at its next
cancellation boundary, below, instead of running for minutes towards a result
nobody can use). Such a job ends as superseded, nothing is published or
cached, and the recording counts as waiting at once; a new fit starts by
itself once the inputs have been still for about a second and the old one has
stopped.

**Cancellation** is observed at three kinds of boundary: before the fit starts
(preparation neither reports nor asks; the progress texts begin with
`Starting fit`); every 256 states of graph construction, in the initializer's
prefix and segment fits as in the full fit; and before each optimizer
iteration of any of those fits (the segment fits' texts name the segment and,
for a prefix, its length). A linear solve in progress finishes first. The
reconstruction at the IMU samples (section 4), after the last iteration of the
full fit, is not a boundary: it runs to its end, a fraction of a second. A
cancelled fit publishes nothing and caches nothing.

**Outcomes.** A rejection (section 6) and a solver failure are functions of the
inputs, so they are cached like any result: the recording is listed among
those that could not be computed (the status bar's warning and the logbook
row), with its reason, and nothing offers a retry until an input changes,
because the same inputs would give the same answer. Running out of memory,
failing to start the worker, or failing to write the fit's stored copy is not
a function of the inputs and is never cached as a failure: the recording is
listed the same way with its reason, marked as tried again at the next start;
it is not tried again while the application runs unless an input changes.

**Invalidation.** A change to a declared input (a re-import, a merge, a changed
`SCHEMA_VER`, a changed origin) drops the result, every value derived from
it, and its stored copy. Moving markers, zooming, and display preferences do
not.

**Stored results.** The fit's result is stored when it is published: a
success, a rejection or a solver failure. It is restored bit for bit when the
recording is loaded, and the restored result is indistinguishable from a fresh
one. Its code stamp is the algorithm string of the diagnostics
(`batch-temperature-bias-v4` since the reconstruction at the IMU samples): a
change that can alter what the fit returns changes that string, and every
stored fit is then dropped at its recording's next load. So the first start
after such an update finds every stored fit stale when its recording is
loaded: the record is deleted, and the recording is fitted again once when
something switched on needs it (after a start, every recording a fusion
column covers and every visible one a checked fusion plot covers), counted in
the status bar like any fit. The record also states what provided each input the fit looked up,
and a load repeats those lookups, so only a change of what the fit reads, or
of the code that computes it, drops a stored fit. A cancelled fit, or one that
ran out of memory, stores nothing. A stored rejection is listed again among
the recordings that could not be computed, with its reason, also after a
restart and without loading the recording. A stored fit whose file cannot be read when its
recording is loaded (another program holding it, say) is kept: the recording
reads as not fitted until it is loaded again (and is fitted again meanwhile if
something switched on needs it).

**One at a time.** Fits run one after another, below normal priority: the
focused recording first, then the other visible recordings from top to
bottom, then the recordings fusion columns need
([CALCULATIONS.md](CALCULATIONS.md), section 16.6). The solver's helper
threads, on which GTSAM parallelizes the elimination, take the worker's
priority while they help a fit, so the whole fit runs below normal priority
where the operating system applies it (not on Linux; CALCULATIONS.md, section
15.5). Quitting cancels them and waits for the running fit to reach its next
cancellation boundary: at most one solver step and the reconstruction.

**Logbook columns** over fusion values show the live value for a loaded
recording, and for an unloaded one the value cached from its stored result.
While such a column is enabled, every recording without a stored result is
fitted in the background (recordings that are not loaded are loaded two at a
time, as hidden recordings); its cell shows "···" until then. A recording
without IMU data shows none. One with a stored result whose
value is not cached yet (after an update, say) stays empty until it is loaded.

## 8. Validation

For identical inputs the kernel must reproduce its goldens (objective, biases,
residuals, output timestamps and output channels), captured from it by
`fusion_golden_capture` at the end of the last phase that changed numerical
results ([tests/README.md](../tests/README.md), section 11). That is
demonstrated by tests, all labelled `fusion`:

| Test | What it holds |
| --- | --- |
| `tst_fusion_golden` | the kernel through its public API reproduces its goldens for twelve synthetic fixtures (three fits, nine rejections), the progress texts at its boundaries, cancellation at each kind of boundary (prefix, segment and full-fit iterations included), determinism and thread independence |
| `tst_fusion_kernel` | the kernel's stages: the segmented initializer on the five synthetic recordings of the specification, the two stopping rules forced through the tuning, the per-step covariance, the temperature factor's Jacobians and the three temperature cases, the fit trace iteration by iteration against the goldens; the reconstruction at the IMU samples (the ends are the fitted states, sharing by noise, zero mismatch, consistency, equivalence with a graph with a state at every sample), the channels and diagnostics the fit publishes as that reconstruction bit for bit, and the time axis on the three fits and on a recording whose GNSS rate is above its IMU's |
| `tst_fusion_session` | the registered calculation on real sessions: reads never run it, one request publishes everything, rejections are cached results, a session without `IMU/temperature` has a missing input, a fit exported and restored into another session is indistinguishable, with what provided each name it looked up |
| `tst_fusion_derived` | what is derived from the outputs, without the solver: the outputs stored as data, elevation and the track accelerations held to exact known answers, the track accelerations equal to the GNSS ones on the same samples, and each derived value waiting on the fit and never starting it; the orientation vocabulary (24 pairs, each a proper rotation, the attribute's choices), heading, pitch and roll held to hand-built known answers, finite with pitch at +90 or -90 where the forward axis is exactly vertical, side mounts, a GNSS track and a course reference that change nothing, and the fit's own angles for the device frame, an invalid or changed orientation without a fit, and the Orientation column's display, edit and bulk edit |
| `tst_fusion_jobs` | the real fit through the executor: supersede, cancel, rejection, shutdown, the logbook column cached from the stored result and kept, for an unloaded session, through an altitude marker added at run time or at the next start |
| `tst_fusion_rows` | the demand layer with the real fusion plots, end to end: fits started and dropped by what is checked and visible, with no gesture; progress and failures as each fit ends |
| `tst_fusion_store` | the fit's stored result: bit for bit after unloading and after a restart (also when fitted before the first save), a rejection and a solver failure listed among the recordings that could not be computed, with their reasons, dropped by a dependency edit, a merge or a code-stamp change and kept by an unrelated edit, the session file untouched, not requested after the logbook's `cache/` folder was deleted; kept across altitude-marker, registration, descent-pause and plugin-set changes, in memory and after a restart; dropped at once, with its record, by a registry change that changes what a name it looked up resolves to (the removal of its provider), kept by a candidate registered behind the provider; deleted when a lookup resolves differently at load; a logbook column over roll filled for recordings that are not loaded, and nothing fitted again after a restart |
| `tst_fusion_runner` | `fusion_runner`, the command-line fit on a recording written as `TRACK.CSV` / `SENSOR.CSV`, against a direct kernel run and against the application's own import path |

The goldens live in `tests/data/fusion/`. In exact mode
(`FLYSIGHT_FUSION_EXACT=1`, on the capture configuration) every output sample
must equal the golden bit for bit; the portable mode used everywhere else
allows `1e-7 + 1e-7 * |golden|` (`1e-7` rad, that is `5.73e-6`, for numbers in
degrees; ten times the largest difference measured between compilers in CI:
the numbers are in the tolerance policy of `tests/README.md`). Exact mode is
not opt-in on the capture configuration: where the compiler matches
`tests/data/fusion/capture.json` (64-bit MSVC 19.44, as the capture tool
recorded it) and the configuration is Release, CTest runs each of these
tests but `tst_fusion_derived`, which holds nothing to a golden bit for bit
(the one golden it reads is compared within `1e-6`), a second time
as `tst_fusion_*_exact` (label `exact`; CMake option
`FLYSIGHT_FUSION_EXACT_TESTS`, `AUTO` by default). With any other compiler the
configure log says that they were not registered, and only the portable mode
runs. The fixtures, the two modes and the capture procedure are described in
[tests/README.md](../tests/README.md), section 11.

**Validating the reconstruction.** The reconstruction of section 4 is held to
a reference by `tst_fusion_kernel`: a graph with a state at every integration
edge, a one-step IMU factor across every step with the same per-step
covariance and the same interval bias as the fit's factor, the states at the
fixes and the biases held at the fit's values, solved by the fit's solver from
the forward states. At every IMU sample the two agree to within the one
linearization. On `coarse_maneuver`, whose largest mismatch is 1.1e-4 degrees
of attitude, 9.1e-5 m/s of velocity and 9.1e-6 m of position, they differ by
3.6e-12 degrees, 2.8e-13 m/s and 2.9e-14 m (3.3e-8, 3.1e-9 and 3.2e-9 of the
mismatch); on a synthetic recording that turns at 3 rad/s about a horizontal
axis (IMU 25 Hz, GNSS 5 Hz, exact readings, the true states as the fit), whose
velocity mismatch is 0.11 m/s and position mismatch 0.011 m, by 1.0e-9 m/s
and 5.9e-11 m (9.4e-9 and 5.3e-9 of the mismatch) and 2.0e-6 degrees of
attitude. The test allows 1e-5 of the largest mismatch plus 1e-12 per
component, and on the turning recording 5e-5 degrees of attitude, whose
mismatch there is at rounding. The reference holds the fix states because,
freed with their GNSS factors and the biases, they move along the unobservable
heading, which is not what the test measures: by 0.9 times the attitude
mismatch on `coarse_maneuver`, 0.18 degrees on `stationary_spin` and 5.7
degrees on the `rest_throughout` recording, so that no tolerance could be
stated against the mismatch. With the states at the fixes settled, each fix
interval is solved on its own.

What the one pass leaves out: it is linearized once, about the forward
states, so the split of the mismatch inside an interval carries an error
quadratic in the mismatch, about `2.2e-5 |d_v|^2` m/s of velocity and
`5.4e-4 |d_att|^2` degrees of attitude (the mismatch in m/s and degrees); the
states at the fixes and the biases are the fit's and are not solved again;
chaining one-step preintegrations differs from preintegrating many steps at
once (1.1e-6 degrees on `stationary_spin`); and no uncertainty is published.

The consistency of acceleration and velocity is bounded: integrated by the
kernel's rule from sample `a` to sample `b`, the published acceleration
reproduces the published velocity change to within
`dt_a / 4 |c_before(a) - c_after(a)| + dt_b / 4 |c_after(b) - c_before(b)|`,
plus, for every sample step that contains a fix, `dt / 2` times the largest
difference between the corrections of its part-steps; over the whole fitted
interval only the first two terms remain. The test allows 1.01 times the
bound plus 1e-12 m/s. The bound is tight: the worst ratio of error to bound is
0.986 on `coarse_linear`, 0.99998 on `coarse_maneuver` and 0.99999 on
`stationary_spin`, and over the whole fitted interval the error is 4.2e-13,
1.9e-6 and 8.2e-6 m/s.

Under fast rotation the corrections carry the integration's own
discretization error. The fit's integration rotates each reading by the
attitude at the start of its step, which is off by about `|omega x f| dt / 2`;
with exact readings and the true states at the fixes that alone leaves a
mismatch, which the pass shares out as corrections. On the turning recording
above (3 rad/s), the published acceleration against the true one:

| IMU rate | velocity mismatch, m/s | published acceleration, error, m/s^2 | the rotated reading alone, error, m/s^2 |
| --- | --- | --- | --- |
| 13 Hz | 0.21 | 0.136 | 1.0e-3 |
| 25 Hz | 0.11 | 0.133 | 1.2e-3 |
| 100 Hz | 0.030 | 0.050 | 3.4e-4 |

At 1 rad/s the error is smaller, 0.017 m/s^2 at 100 Hz. The remedy is to
rotate each reading by the attitude at the middle of its step, a change to
the fit's integration, and so to the fitted states, that a later
specification makes.

**The runner.** `fusion_runner` is a test/tooling target, built with the
fusion tests and never installed: the fit on one recording, imported exactly
as the application imports it (the same parser, conversion layer and
on-demand derivation, so a recording without `SCHEMA_VER` gets the legacy gyro
correction), with the diagnostics JSON on standard output and the progress
texts on standard error. Command line: `fusion_runner [options] <folder>` or
`fusion_runner [options] <TRACK.CSV> <SENSOR.CSV>`, with `--csv <path>`
(the seventeen output channels as CSV, on success) and
`--dump-inputs <path>` (the effective input channels the fit was given). Exit
codes: 0 Succeeded, 1 Rejected, 2 SolverFailed, 3 the files could not be
imported, 4 an output file could not be written, 5 an internal error, 64 a
usage error (and `--help`). It never reads or writes the user's logbook or
settings. The reference recordings and the expected results are in
[tests/README.md](../tests/README.md), section 12.2.

**Optional check against a real recording.** Real recordings are not in the
repository, so this is a local check and not a CI test. Set
`FLYSIGHT_FUSION_RECORDING` to a folder that contains `TRACK.CSV` and
`SENSOR.CSV` and run the test function directly:

```powershell
$env:FLYSIGHT_FUSION_RECORDING = "D:\recordings\17-26-24"
.\tst_fusion_jobs.exe realRecordingCheck
```

Reference numbers recorded before this plan's changes (the branch, constant
bias, resting-window start) for recording 17-26-24: objective
65602.22485051976, 9247 GNSS states, 24411 outputs. With the current kernel
the counts still match and the objective differs: the model changed. The
recorded objective was reached only with a copy of `SENSOR.CSV` that carries
`$VAR,SCHEMA_VER,2`: the reference read the gyro channels of every file
literally, while this implementation applies the legacy gyroscope correction
to a file without `SCHEMA_VER` ([DATA_SCHEMA.md](DATA_SCHEMA.md), section 4).
With the unmodified file a different objective is expected and correct.
