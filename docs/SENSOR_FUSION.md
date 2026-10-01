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
follows the IMU temperature and a scale factor per sensor axis, and stops
when the cost has stopped moving
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

The fit declares twenty-six inputs, all required, and reads nothing else:

```
GNSS/_time
Local/north  Local/east  Local/down  Local/velN  Local/velE  Local/velD
GNSS/hAcc    GNSS/vAcc   GNSS/sAcc
IMU/_time
IMU/ax  IMU/ay  IMU/az  IMU/wx  IMU/wy  IMU/wz  IMU/temperature
_LOCAL_ORIGIN_INDEX  _LOCAL_ORIGIN_LAT  _LOCAL_ORIGIN_LON  _LOCAL_ORIGIN_HMSL
ACCEL_FS_G  GYRO_FS_DEG_S  ACCEL_ODR_HZ  GYRO_ODR_HZ
```

**Configuration.** The last four are the IMU's configuration: the
accelerometer's and the gyro's full-scale range and output data rate, as
`SENSOR.CSV` states them, or their constant defaults where it does not
([DATA_SCHEMA.md](DATA_SCHEMA.md), section 2, gives the keys, their values and
the default). They are therefore always available: a recording never lacks
them. They reach the kernel as numbers in `Channels::imuConfiguration`, as the
origin does, and the model is built from them: the noise of every reading is
the datasheet's at the configured range and rate (section 4), and the
diagnostics report the configuration the fit ran under. The kernel also checks
the recording against its configuration, stated or defaulted, after every
other check (section 6): the readings must lie on the lattice of the
configured ranges, and the IMU must be logged at the configured rates. The
receiver's dynamic model and rate and the other sensors' rates are stored with
the recording and are not inputs.

**The lattice.** A FlySight writes a reading as a count times its sensor's
step at the configured range: the range over 32768 counts for the
accelerometer (16 / 32768 g at +/-16 g), the datasheet's sensitivity for the
gyro (70 mdps at +/-2000 deg/s, which the legacy correction of
[DATA_SCHEMA.md](DATA_SCHEMA.md), section 4, restores for a legacy file). The
readings of a range therefore lie on a lattice of multiples of its step, and
the coarsest range whose lattice every reading of the three axes fits is the
range the recording shows; it must be the configured one, or the fit rejects
the recording naming both, and does not guess. The check reads every reading
as the kernel receives it (effective values in m/s^2 and deg/s, inside the
fitted window or not), before any correction of the kernel's own. A reading
fits when it is within one unit of the file's last decimal, carried through
the conversion layer's largest factor, of a lattice point: 1e-5 g x 9.80665 =
9.8e-5 m/s^2, and 1e-3 deg/s x 1.14688 = 1.15e-3 deg/s. One unit and not
half of one, because firmware v2023.09.22 truncates: the recording `17-26-24`
(24,511 samples) has residuals up to 63/64 and 255/256 of a unit, at least
three times below half the finest step. Against the printed 0.488 mg/LSB the
same file would be 2.4e-3 m/s^2 off: the lattice is the range over 32768, as
the firmware writes it, not the printed sensitivity.

**The rate.** The median logged IMU interval of the whole recording must be
within 10 % of the nominal interval, one over the rate, of each of
`ACCEL_ODR_HZ` and `GYRO_ODR_HZ`. The datasheet states no tolerance for its
oscillator; the recordings on disk log at 13.2-13.3 Hz against their 12.5 Hz
(5-7 % fast), and the listed rates are a factor of two apart, so 10 % neither
rejects a healthy unit nor mistakes one rate for another.

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
datasheet's typical offsets and drift (Table 2: LA_TyOff +/-20 mg, 0.2 m/s^2;
G_TyOff +/-1 deg/s, 0.017 rad/s; G_OffDr +/-0.010 deg/s per degC): a
recording without a temperature change leaves `b1` at its prior, and a
well-behaved unit loses nothing. The initializer
(section 5) only chooses where the solver starts; its own prefix and segment
fits hold the gyro bias constant and take the readings at unit scale.

**The scale factors.** Beside the biases the graph has six scale factors `S`,
one per axis for each sensor, in the order accelerometer x, y, z, then gyro
x, y, z: the factors themselves, by which an axis's reading differs from the
true specific force or rate, so that the integration takes each reading
divided by its axis's factor (`f / s`, axis by axis). They start at one, the
datasheet's nominal sensitivity, and have a zero-mean prior on their
departure from one whose sigma is the datasheet's sensitivity tolerance, 1 %
for both sensors (G_So% of Table 2, applied to the accelerometer too: the
table has no tolerance row for it). Like `b1` they belong to the full fit
only; a fit without them is the same fit with every factor held at one. Each
IMU factor depends on them to first order: the derivative of its
preintegration with respect to the six factors is accumulated step by step
from the solver library's own per-step Jacobians (a step's input depends on
the gyro's factors also through the half-step turn below), and the fit
re-preintegrates at the fitted factors between passes, as at the fitted bias
(Solver and stopping). The factors in the graph's order: per fix the position
and velocity factors and the IMU factor to the fix before; then the bias
prior, the slope prior and, last, the scale prior. The diagnostics report the
fitted factors as `model.scale` (below).

*What the data determine.* The factors are weakly observable on most
recordings. An axis whose reading does not change beyond its bias has no
information on its factor that is not also information on its bias, so the
two are separated by their priors alone. Under gravity the corrected reading
of the vertical axis is `f_z / s_z - b_z`, and a bias error there is shared
in proportion to the priors' variances: the factor takes
`g^2 sigma_s^2 / (g^2 sigma_s^2 + sigma_b^2)`, 9.7 %, and the bias the rest,
so a unit whose z bias is 0.08 m/s^2 fits `s_z` 0.9992 and `b_z` 0.072, and
the fitted z biases of the fixtures of section 8 moved by about that much
when the factors were added. At rest the accelerometer's factors stay within
a tenth of their tolerance. That sharing holds for the deterministic part of
a reading. A gyro axis whose reading at rest is its bias and noise moves,
within its prior, toward a factor above one, because the IMU factor whitens
the residual of the divided readings with a noise model that does not depend
on the factor: a factor above one shrinks the noise left in that residual
while its weight stays the same. Dividing the densities by the scale the
preintegration is taken at would not remove this, since the covariance is
fixed within a pass; only a weight that changes with the factor itself (a
residual in the reading's units) would. The departure grows with the noise's
variance and with the prior's: on the `rest_throughout` recording of the
kernel's tests, whose y gyro reading dithers by a whole lattice step, the y
factor fits 1.0033, a third of its tolerance; with that axis's noise halved
1.0009 and doubled 1.0108, with a tolerance of 0.5 % 1.0008 and of 2 %
1.0128, and with the gyro's noise removed, one. A drifting gyro reading trades
with the gyro bias. An axis that sees a horizontal acceleration with zero
mean, changing over the recording, separates its factor from its bias: on the
`scale_recording` of the kernel's tests a 2 % departure is recovered to 3e-4.

**Integration.** IMU integration splits at exact GNSS boundaries and
original IMU timestamps, using linearly interpolated midpoint inputs. The
solver library applies a step's accelerometer reading at the attitude of the
step's start, half a step behind the reading, an error of about
`|omega x f| dt / 2`: several m/s^2 at 13 Hz where the unit turns at 5-7
rad/s, as a helmet does through a parachute opening. So the kernel turns each
step's reading by half the step's bias-corrected rotation before the library
applies it, and the reading then acts at the attitude of the step's middle;
what remains is second order in the step's rotation, and is modelled below.
The readings enter divided by the scale factors the preintegration is taken
at (above): the midpoint readings, the half-step turn and the integration's
own errors below are all computed from the divided readings, while the
densities, the datasheet's figures, are not divided (a factor's departure is
below a density's precision, and a divided density would make the noise a
function of the fitted state). The integration covariance is `I x 1e-8`.

**Noise: the datasheet.** Every number of the noise model is the IMU's
datasheet (LSM6DSO, DS12140 Rev 3) at the recording's configuration, or is
derived from it; none is chosen because recordings fit better with it. The
table, which exists in the code in one unit (`src/fusion/sensornoise.cpp`):

| Quantity | Value | Source |
| --- | --- | --- |
| accelerometer noise density An, high-performance mode | 70, 75, 80, 110 ug/sqrt(Hz) at +/-2, 4, 8, 16 g | Table 2 (note 8: independent of the rate) |
| gyro noise density Rn, high-performance mode | 3.8 mdps/sqrt(Hz) | Table 2 (note 6: independent of the rate and the range) |
| accelerometer step (LA_So) | the range over 32768 counts: 1/16384, 1/8192, 1/4096, 1/2048 g | Table 2 prints 0.061, 0.122, 0.244, 0.488 mg/LSB: these to three decimals |
| gyro step (G_So) | 8.75, 17.50, 35, 70 mdps at +/-250, 500, 1000, 2000 deg/s | Table 2 |
| accelerometer bandwidth | the rate / 2 | Figure 17, note 1 (LPF1 at ODR / 2 in high-performance mode), and Table 65 (LPF2_XL_EN = 0) |
| gyro bandwidth (LPF2 cutoff, no LPF1) | 4.2, 8.3, 16.6, 33.0, 66.8, 135.9, 295.5, 1108.1, 1320.7, 1441.8 Hz at 12.5, 26, 52, 104, 208, 416, 833, 1666, 3333, 6666 Hz | Table 18 |
| sensitivity tolerance | 1 % for both sensors | Table 2, G_So% (the table has no row for the accelerometer, whose tolerance section 4.6.1 places there) |

The step is the quantization step and the lattice of section 3. The
accelerometer's is the range over 32768 exactly, not the printed 0.488 mg:
the firmware writes `counts x range / 32768`, and the printed figure would put
a 1 g reading 0.58 mg off. The gyro's is the sensitivity, 1.14688 times the
range over 32768 ([DATA_SCHEMA.md](DATA_SCHEMA.md), section 4). Table 18
prints the rates 417, 1667, 3333 and 6667 and Table 2 prints 416, 1666, 3332
and 6664; the keys write 416, 1666, 3333 and 6666. The table assumes what
firmware v2023.09.22 runs: both sensors in high-performance mode (XL_HM_MODE
and G_HM_MODE at their reset value, which that firmware does not change), the
gyro's LPF2 alone and the accelerometer's LPF1 alone, the fixed filters
[DATA_SCHEMA.md](DATA_SCHEMA.md), section 2, records with the default. A
configuration the table has no entry for (a value outside a key's list, the
accelerometer's 1.6 Hz, which exists only in low-power mode, or a value that
is not a number) is a rejection naming the key (section 6). The sensitivity
tolerance is the sigma of the scale factors' prior (above).

**Per-sample noise and density.** One reading of a sensor, on each axis, has
the standard deviation `sigma = sqrt(density^2 x bandwidth + step^2 / 12)`:
the datasheet's noise density over the bandwidth, and the uniform rounding
error of one step. The bandwidth stands in for the filter's noise-equivalent
bandwidth, which the datasheet does not give (it gives cutoffs, not filter
orders). The density the integration uses is `D = sigma x sqrt(1 / rate)`,
at the configured rate, so that a step of the nominal length carries one
sample's variance; no density of the model is derived otherwise. At +/-16 g
and +/-2000 deg/s:

| Rate | accelerometer sigma, m/s^2 | accelerometer `D`, m/s^2/sqrt(Hz) | gyro sigma, rad/s (deg/s) | gyro `D`, rad/s/sqrt(Hz) |
| --- | --- | --- | --- | --- |
| 12.5 Hz (the default) | 0.0030304 | 8.5714e-4 | 3.7797e-4 (0.02166) | 1.0691e-4 |
| 26 Hz | 0.0041278 | 8.0952e-4 | 4.0112e-4 (0.02298) | 7.8665e-5 |
| 104 Hz | 0.0079007 | 7.7473e-4 | 5.1917e-4 (0.02975) | 5.0909e-5 |

The default's accelerometer sigma, 0.0030 m/s^2, is what the quietest windows
of the reference corpus show, 0.0029-0.0035 m/s^2 per sample; its gyro sigma
is below the corpus median, 0.030 deg/s. These are checks of the model, not
its source.

**The integration's own errors.** Each step adds two errors of the
integration itself to the densities' noise, in quadrature, each derived and
with no constant of its own.

*The sampling term.* The integration treats the samples as a
piecewise-linear signal `L`, and the midpoint reading integrates each linear
piece exactly, so a step's whole sampling error is the interpolant's. For a
signal `f` with a constant second derivative across the sample interval
`[t_k, t_k+1]`, `f - L = (f''/2)(t - t_k)(t - t_k+1)`, so over a step
`[a, b]` inside it the error is `-f'' w`, with

```
w = 1/2 integral_a^b (t - t_k)(t_k+1 - t) dt
  = 1/2 [h (u_b^2 - u_a^2) / 2 - (u_b^3 - u_a^3) / 3],   u = t - t_k,  h = t_k+1 - t_k
```

For a step that is a whole interval `w` is `h^3 / 12`, the trapezoid rule's
error; the parts of an interval that a fix splits sum to it (taking the whole
interval's weight for each part would understate a half-interval part
fourfold). `f''` is estimated per sample interval as the larger norm of the
changes of slope at its two ends, `D_i = 2 (s_i - s_i-1) / (h_i-1 + h_i)` at
sample `i`, `s_i` the slope of interval `i` (exact for a quadratic at any
spacing); the first and last interval have one end, and fewer than three
samples give no term. The terms are `s_v = w c(f)` for the velocity (m/s)
and `s_theta = w c(omega)` for the angle (rad), `c` that estimate; the gyro
bias, constant within an interval, cancels. `f` and `omega`, here and in
the remainder, are the readings divided by the scale.

*The remainder of the mid-step scheme.* With `theta = (omega_mid - b_g) dt`,
`dtheta = (omega(b) - omega(a)) dt`, `fbar = f_mid - b_a` and
`df = f(b) - f(a)`, the true velocity change of a step,
`R_a integral_0^dt Exp(phi(s)) f(s) ds`, minus the scheme's,
`R_a Exp(theta / 2) fbar dt`, expanded to second order for a rate and a force
linear across the step, is

```
r_v     = | (dt/24) theta x (theta x fbar) + (dt/12) (theta x df - dtheta x fbar) |    m/s
r_theta = | theta x dtheta | / 12                                                     rad
```

The first term is the pure rotation remainder, second order in the step's
rotation; the bracket and `r_theta` (coning: the rotation of a rate that
turns within the step) are the same expansion's other second-order terms.
On real data the pure term is the smallest of them: at 13 Hz, 1 rad/s, a
turn-rate change of 0.2 rad/s per step and 15 m/s^2 it is 2.7e-4 m/s and the
term in the change of rate 1.4e-3 m/s, against 2.3e-4 m/s of noise per step.
Position remainders are fourth order in `dt` and stay in the integration
covariance.

*The step's covariance.* The library adds a covariance per unit time, so a
step's variance is that times `dt`; the step's sensor covariances are
therefore `(D_a^2 + (s_v^2 + r_v^2) / dt) I` and
`(D_g^2 + (s_theta^2 + r_theta^2) / dt) I`. They are isotropic, from vector
norms, so the half-step turn leaves them as they are. A step of the nominal
length without either error carries one sample's variance; a step with a
constant signal and no rotation has the densities' covariance exactly; and
the added variance of a sliver step beside a fix vanishes as `dt^3` or
faster, so a fix next to a sample costs nothing.

*What the derivations assume.* A smooth signal between samples: a step change
of the signal at a sample is underestimated (on a recording whose force steps
by 2 m/s^2 the true error is six times the term). The samples exact: on noisy
readings the changes of slope read noise as curvature, which for noise at the
model's level adds about 4-8 % of a full step's noise variance. And in the
remainder, a rate and a force linear across the step.

**Solver and stopping.** Batch Levenberg-Marquardt uses QR, 100 iterations per
pass, a relative cost-change threshold of 1e-8, a ceiling of 1e12 on its
damping, and up to five bias reintegrations. GTSAM's default ceiling, 1e5, is
below the damping a resting recording needs under the datasheet's densities
(the IMU blocks of the Hessian are about 1e9): at that ceiling every iteration
returns the same values. A pass has settled when an iteration lowers the cost
by at most 1e-8 of max(1, cost), also when the step did not move while the
damping is below its ceiling (no better point at that damping). Within a
pass the bias and the scale factors enter the IMU factors to first order,
through the preintegration's Jacobians (the library's for the bias, the
accumulated one of the scale); between passes the graph is
re-preintegrated at the pass's fitted bias and scale factors. The fit has
converged when the graph
re-preintegrated at the settled pass's bias and scale factors changes that
cost by at most 1e-6 relative (of max(1, cost)): the bias and the scale have
stopped moving as far as the preintegration can tell. A fifth pass that reaches its iteration limit
without settling is accepted when, over its last 20 iterations, the mean
relative decrease per iteration is below 1e-4 and the position and velocity
normalized RMS (root mean squared whitened residual per scalar component) are
both below 2: a slow tail is a fit that is done for any practical purpose
(four ground recordings of the reference corpus end this way with position
RMS of 0.3-0.5 m); a fit that is still descending faster, or that disagrees
with GNSS, remains a solver failure. The diagnostics name the rule that ended
the fit: `settled`, `slow tail accepted`, `iteration limit` (the last pass
reached its limit and the slow tail was refused), `bias not settled` (the
last pass settled but re-preintegrating still moved the cost),
`cost increased` (an iteration made the cost non-finite or larger) or
`damping saturated` (an iteration left the cost unchanged with the damping at
its ceiling: the optimizer has given up there, and the start it would have
called converged may be far from a minimum). The last two are the failures
the fit cannot continue from, and are solver failures, never a convergence.
Reported factors are reintegrated at the final bias and scale factors.

**The state at every IMU sample.** The fit estimates the state at each GNSS
fix; what it publishes is the state at every IMU sample between the first and
the last fix, reconstructed one fix interval at a time from the fitted states
at the interval's two ends and the fitted biases and scale factors, which it
leaves as they are. From the fitted state at the first fix the IMU is
integrated forward with the interval's bias and the fitted scale factors
through the fit's own preintegration: the same step
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
measurements would give, with the states at the fixes and the biases and
scale factors held (what the one pass leaves out: section 8). Where the mismatch is zero it is
the forward integration.

**Acceleration.** For each step, the step correction `c` is the corrected
velocity change across the step divided by its length, less the mean of the
readings at its two edges, divided by the fitted scale factors and bias
removed, each rotated by the corrected attitude at its own edge (at a fix,
the interpolated reading the integration uses), less gravity: the part of the
velocity change the rotated readings do not explain, as an acceleration. The
published acceleration at a sample is
`R * (specific_force / scale - accelerometer_bias) + [0, 0, 9.80665] + (c_before + c_after) / 2`:
the sample's own reading divided by the fitted accelerometer scale factors,
axis by axis, with the bias removed, rotated by the corrected
attitude `R`, plus gravity, plus the mean of the corrections of the two steps
beside the sample (only a sample exactly on the first fix has one and takes it
alone; the last published sample has two, since the last fix is never
published; the step beside a fix is the part-step between the
sample and the fix). The reading is the sample itself, not the interpolated
midpoint value the integration uses, so nothing smooths the accelerometer's
signal: only the correction is spread, over the two steps beside each sample.
The consequence: integrated with the kernel's own rule, the published
acceleration reproduces the published velocity change over any run of
samples, the whole fitted interval included, to within the spread of the
corrections at its ends and at the fixes inside it (the bound is in
section 8), but not step by step. Where the corrections are negligible, in
steady flight, the published acceleration is the rotated reading. It has no
added low-pass filtering and is not a derivative of the GNSS velocity.

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

- *identity and audit*: `algorithm` (`batch-temperature-bias-v6`), `input`
  (the input audit: `epoch_utc_s`, `imu_count`, `gnss_count`, `origin_index`,
  `origin`, `height_method`, `time_method`) and `configuration` (the
  configuration the fit ran under: `accel_fs_g`, `gyro_fs_deg_s`,
  `accel_odr_hz`, `gyro_odr_hz`);
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
  factor, kinds `position`, `velocity`, `imu`, `bias_prior`, `slope_prior`
  and `scale_prior`, the three priors last in that order, each with `node`,
  `time_s` and `squared_whitened_error`),
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
- `stopping`: `rule` (one of the six texts above), `passes`,
  `last_pass_mean_relative_decrease`, `repreintegration_cost_difference`,
  `bias_settled_tolerance`, `lambda_upper_bound` (the damping ceiling), and
  `slow_tail` with `window`, `max_mean_relative_decrease` and `max_nrms` (the
  thresholds in force);
- `quality`: `imu_nrms`, `position_nrms`, `velocity_nrms` (the normalized RMS
  of each factor kind's whitened residuals) and `objective_per_state`;
- `model`: `noise`, the datasheet's noise at the configuration (above), with
  `acc` (`datasheet_density_m_s2_rthz`, `bandwidth_hz`, `step_m_s2`,
  `sample_sigma_m_s2`, `density_m_s2_rthz`) and `gyro`
  (`datasheet_density_rad_s_rthz`, `bandwidth_hz`, `step_rad_s`,
  `sample_sigma_rad_s`, `density_rad_s_rthz`); `gyro_bias` with
  `b0_rad_s`, `b1_rad_s_per_degc` and `t_ref_degc`, always numbers (the
  temperature is a required input); and `scale`, the fitted scale factors
  themselves (one is nominal), with `acc` and `gyro`, each `[x, y, z]`;
- `dense_output` and `limitations`, as text: `dense_output` is
  "IMU-rate reconstruction at original IMU times: between fixes the IMU integrated from the fitted state, the mismatch with the next fitted state shared over the steps by their noise, in one linearized pass", and `limitations` is
  "Local batch convergence; heading may be ambiguous. Between fixes one linearized pass with the fitted fix states, biases and scale factors held; no uncertainty is published."

When the recording was rejected, or the fit stage raised anything but a
stopping-rule failure, the diagnostics are `{"algorithm", "failure"}` with the
reason. When the fit completed a pass and did not converge (`iteration limit`,
`bias not settled`) they are `{"algorithm", "failure", "quality", "stopping"}`.
When a pass raised the cost (`cost increased`) or saturated the damping
(`damping saturated`) they are `{"algorithm", "failure", "stopping"}`: there
is no rebuilt graph, so no quality. A successful stop describes the optimizer's numerical behaviour, not
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
stopping test treats a no-update step below the damping ceiling as settled,
and a slow tail is accepted on numerical grounds alone. Heading ambiguity, local minima, the
shared accelerometer bias and the linear temperature model of the gyro bias,
and sampling limits remain material limitations. The scale factors are one
constant factor per axis per fit: the sensitivity's change with temperature
is not modelled (LA_SoDr +/-0.01 %/degC and G_SoDr +/-0.007 %/degC in
Table 2, about 0.3 % over 30 degC); there is no cross-axis sensitivity or
misalignment; without motion an axis stays at its prior; the z
accelerometer's factor shares the z bias under gravity in proportion to
their priors (section 4); and the unit of the reference corpus whose gyro is
2 % off nominal is fitted with a 1 % prior, which the data must outweigh.
Not in scope: the magnetometer is not read. Inspect the diagnostics and the
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

Then, after every check above, the configuration (section 3), in this order:

- a configuration the datasheet table has no entry for, naming the first key
  in key order: `No datasheet entry for ACCEL_ODR_HZ = 1.6; sensor fusion
  unavailable`;
- readings whose coarsest lattice is not the configured range, the
  accelerometer before the gyro: `ACCEL_FS_G states +/-16 g but the
  accelerometer readings lie on the +/-8 g lattice; sensor fusion
  unavailable`, or `... lie on no range's lattice; ...` when none fits
  (`GYRO_FS_DEG_S states +/-2000 deg/s but the gyro readings ...` for the
  gyro);
- a median logged IMU interval more than 10 % from a stated rate's,
  `ACCEL_ODR_HZ` before `GYRO_ODR_HZ`: `ACCEL_ODR_HZ states 12.5 Hz but the IMU
  is logged at 100.0 Hz; sensor fusion unavailable`, the realistic case of a
  file without keys from a firmware that logs faster than the default.

A consequence for the escape hatch of [DATA_SCHEMA.md](DATA_SCHEMA.md),
section 7: a legacy file given `$VAR,SCHEMA_VER,2` by hand is read without the
legacy correction, so its gyro readings are multiples of the range over 32768
rather than of the sensitivity, show no range's lattice (or a finer range's),
and the fit rejects the recording.

A solver that does not converge is reported the same way, with the stopping
rule that ended it in the reason (`Batch fusion did not converge (iteration
limit); sensor fusion unavailable`, or `bias not settled`, or `Nonfinite or
increasing optimizer cost`, or `Optimizer damping saturated without
progress`).

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
twenty-six inputs. *Compute* runs on the application's one worker thread, which
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
(`batch-temperature-bias-v6` since the documented noise model, `v5` having
been the mid-step rotation of the accelerometer reading): a
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
| `tst_fusion_golden` | the kernel through its public API reproduces its goldens for fourteen synthetic fixtures (three fits, eleven rejections; every fixture states its configuration and lies on its lattice), the progress texts at its boundaries, cancellation at each kind of boundary (prefix, segment and full-fit iterations included), determinism and thread independence |
| `tst_fusion_kernel` | the kernel's stages: the segmented initializer on the five synthetic recordings of the specification, the stopping rules forced through the tuning (damping saturation a solver failure, the damping ceiling changing nothing below it), the datasheet's noise by configuration bit for bit against its formula and the configurations without an entry, the step model (no sampling term on a constant signal, the sampling term against its derivation on a quadratic signal, the rotation remainder against its derivation on a constant turn and against a thousand-fold subdivided integration on a ramp), the lattice and rate checks and their order after every other check, the noise model in the diagnostics, the temperature factor's Jacobians and the three temperature cases, the scale factors (the readings divided by the scale, the scale Jacobian against central differences, the scaled factor's Jacobians and its agreement with the temperature factor where the scale is the preintegration's, the graph with and without them, re-preintegration at the fitted scale, the reconstruction at the fitted scale, a resting recording left at its prior, a 2 % accelerometer factor recovered and the position misfit falling against the fit without them, the factors in the diagnostics), the fit trace iteration by iteration against the goldens; the reconstruction at the IMU samples (the ends are the fitted states, sharing by noise, zero mismatch with and without rotation, consistency, equivalence with a graph with a state at every sample), the channels and diagnostics the fit publishes as that reconstruction bit for bit, and the time axis on the three fits and on a recording whose GNSS rate is above its IMU's |
| `tst_fusion_session` | the registered calculation on real sessions: reads never run it, one request publishes everything, rejections are cached results, a session without `IMU/temperature` has a missing input, the configuration reaches the kernel (a session stating it fits to the golden, the same session without the keys reads the 12.5 Hz default and is rejected by the rate check, and a recording without keys logged at 12.5 Hz fits under the default), a fit exported and restored into another session is indistinguishable, with what provided each name it looked up |
| `tst_fusion_derived` | what is derived from the outputs, without the solver: the outputs stored as data, elevation and the track accelerations held to exact known answers, the track accelerations equal to the GNSS ones on the same samples, and each derived value waiting on the fit and never starting it; the orientation vocabulary (24 pairs, each a proper rotation, the attribute's choices), heading, pitch and roll held to hand-built known answers, finite with pitch at +90 or -90 where the forward axis is exactly vertical, side mounts, a GNSS track and a course reference that change nothing, and the fit's own angles for the device frame, an invalid or changed orientation without a fit, and the Orientation column's display, edit and bulk edit |
| `tst_fusion_jobs` | the real fit through the executor: supersede, cancel, rejection, shutdown, the logbook column cached from the stored result and kept, for an unloaded session, through an altitude marker added at run time or at the next start |
| `tst_fusion_rows` | the demand layer with the real fusion plots, end to end: fits started and dropped by what is checked and visible, with no gesture; progress and failures as each fit ends |
| `tst_fusion_store` | the fit's stored result: bit for bit after unloading and after a restart (also when fitted before the first save), a rejection and a solver failure listed among the recordings that could not be computed, with their reasons, dropped by a dependency edit, a merge or a code-stamp change and kept by an unrelated edit, the session file untouched, not requested after the logbook's `cache/` folder was deleted; kept across altitude-marker, registration, descent-pause and plugin-set changes, in memory and after a restart; dropped at once, with its record, by a registry change that changes what a name it looked up resolves to (the removal of its provider), kept by a candidate registered behind the provider; deleted when a lookup resolves differently at load; a logbook column over roll filled for recordings that are not loaded, and nothing fitted again after a restart |
| `tst_fusion_runner` | `fusion_runner`, the command-line fit on a recording written as `TRACK.CSV` / `SENSOR.CSV`, against a direct kernel run and against the application's own import path |

The goldens live in `tests/data/fusion/`. In exact mode
(`FLYSIGHT_FUSION_EXACT=1`, on the capture configuration) every output sample
must equal the golden bit for bit; the portable mode used everywhere else
allows `1e-7 + 1e-7 * |golden|` (`1e-7` rad, that is `5.73e-6`, for numbers in
degrees; ten times the largest difference measured between compilers in CI;
the heading, `yaw`, the quaternion and the initializer's start attitude,
takes `4e-6` rad, because on a recording whose heading the data does not
determine the platforms stop at different points along that flat direction:
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
covariance, the same interval bias and the same scale factors as the fit's
factor, the states at the fixes and the biases and scale factors held at the
fit's values, solved by the fit's solver from
the forward states. At every IMU sample the two agree to within the one
linearization and the difference between one preintegration of many steps
and a chain of one-step factors. On `coarse_maneuver`, whose largest mismatch
is 3.7e-7 degrees of attitude, 3.1e-7 m/s of velocity and 3.1e-8 m of
position, they differ by 7.1e-11 degrees, 2.5e-9 m/s and 9.1e-11 m (1.9e-4,
8.2e-3 and 2.9e-3 of the mismatch); on a synthetic recording that turns at
3 rad/s about a horizontal axis (IMU 25 Hz, GNSS 5 Hz, exact readings, the
true states as the fit), whose velocity mismatch is 0.0035 m/s and position
mismatch 0.00036 m, by 2.0e-6 m/s and 3.5e-7 m (5.7e-4 and 9.8e-4 of the
mismatch) and 1.1e-8 degrees of attitude. Under the earlier modelling
densities they agreed to a few 1e-8 of the mismatch; the difference is the
chain's, and grows as the accelerometer's noise ceases to dominate: with an
accelerometer density twenty times the datasheet's, `coarse_maneuver`'s
agree to below 4e-6 of the mismatch again. Either way it is far below anything
physical. The test allows 2e-2 of the largest mismatch plus 1e-12 per
component on `coarse_maneuver`, 5e-3 on the turning recording (where applying
the sharing unmapped misses by 5-15 %), and there 5e-5 degrees of attitude,
whose mismatch is at rounding. The reference holds the fix states because,
freed with their GNSS factors and the biases, they move along the unobservable
heading, which is not what the test measures: by 0.9 times the attitude
mismatch on `coarse_maneuver`, 0.18 degrees on `stationary_spin` and 5.7
degrees on the `rest_throughout` recording, so that no tolerance could be
stated against the mismatch. With the states at the fixes settled, each fix
interval is solved on its own.

What the one pass leaves out: it is linearized once, about the forward
states, so the split of the mismatch inside an interval carries an error
quadratic in the mismatch, about `2.2e-5 |d_v|^2` m/s of velocity and
`5.4e-4 |d_att|^2` degrees of attitude (the mismatch in m/s and degrees;
measured on `coarse_maneuver`, its fitted fixes perturbed up to 100 times); the
states at the fixes and the biases and scale factors are the fit's and are
not solved again;
chaining one-step preintegrations differs from preintegrating many steps at
once (1.1e-6 degrees on `stationary_spin`); and no uncertainty is published.

The consistency of acceleration and velocity is bounded: integrated by the
kernel's rule from sample `a` to sample `b`, the published acceleration
reproduces the published velocity change to within
`dt_a / 4 |c_before(a) - c_after(a)| + dt_b / 4 |c_after(b) - c_before(b)|`,
plus, for every sample step that contains a fix, `dt / 2` times the largest
difference between the corrections of its part-steps, and the kink of the
rotated reading at the fix: the part-steps integrate the reading, divided by
the scale and bias-corrected, rotated by the corrected attitude at each edge, the fix's own
included, where the kernel's rule takes the straight line between the two
samples, so the bound adds the difference between the part-steps' trapezoids
and the sample step's (half the step's length times the rotated reading at
the fix's departure from that line). Over the whole fitted interval the same
bound holds, its fix terms included: the corrections of the part-steps beside
a fix do not cancel. The test allows 1.01 times the bound plus 1e-12 m/s.
The bound is tight: the worst ratio of error to bound is 1.0006 on
`coarse_linear` (whose errors are rounding, 1e-15 m/s), 0.99998 on
`coarse_maneuver` and 1 on `stationary_spin`, and over the whole fitted
interval the error is 1.1e-15, 2.7e-7 and 8.9e-6 m/s.

Under fast rotation the corrections carry what remains of the integration's
own discretization error. The solver library applies a step's reading at the
attitude of the step's start, an error of about `|omega x f| dt / 2`, so the
kernel turns each reading by half the step's rotation first (section 4); the
remainder is second order in the step's rotation. With exact readings and the
true states at the fixes that remainder alone leaves a mismatch, which the
pass shares out as corrections. On the turning recording above (3 rad/s), the
published acceleration against the true one:

| IMU rate | velocity mismatch, m/s | published acceleration, error, m/s^2 | the rotated reading alone, error, m/s^2 |
| --- | --- | --- | --- |
| 13 Hz | 0.013 | 0.017 | 1.3e-7 |
| 25 Hz | 0.0035 | 0.0033 | 3.6e-7 |
| 100 Hz | 0.00023 | 0.00050 | 1.2e-7 |

With the library's start-of-step rotation alone the errors were 0.136, 0.133
and 0.050 m/s^2, from velocity mismatches of 0.21, 0.11 and 0.030 m/s. On a
real fit that lag did not even appear as a mismatch, because the states at
the fixes came from the same integration; the corrections then carried it
into the published acceleration wherever the unit turned, and on the
reference recording `11-17-12` the two integrations differ by up to 4.8
m/s^2 at the opening, where the unit turns at 5-7 rad/s. With the mid-step
rotation the fit agrees with that recording better: its objective is 22 %
lower (20,203 to 15,759), its velocity and IMU normalized RMS fall from 0.63
and 0.59 to 0.53 each, the largest velocity mismatch of any fix interval
from 1.45 to 0.92 m/s, and against the receiver's velocity changes over the
fix intervals where the unit turns at 1-4 rad/s it agrees better in about
three of four.

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
Such a copy is now rejected: its gyro readings, read literally, lie on no
range's lattice (section 6). With the unmodified file a different objective
is expected and correct.
