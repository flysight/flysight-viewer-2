#include "fusionfixtures.h"

#include <cmath>
#include <limits>

// Rules that keep these recordings bit-identical with any IEEE-754 compiler
// (the capture harness and the ported tests must see the same inputs):
//   - only + - * / and std::round (exact by definition) are used; no
//     transcendental function, no <random> distribution;
//   - every sample is computed from its index (t = i * .01), never by
//     accumulation;
//   - noise comes from SplitMix64, mapped to [-1, 1), drawn in one documented
//     order: all GNSS samples first, in time order, each drawing north, east,
//     down, velN, velE, velD; then all IMU samples in time order, each drawing
//     ax, ay, az, wx, wy, wz. Accuracies, times and the temperature carry
//     no noise.
// Every fixture states its configuration: +/-16 g, +/-2000 deg/s and the
// listed rate nearest its IMU sampling (within 4 %), and its readings are
// rounded onto the stated ranges' lattices after the noise is added,
// round(v / s) * s, as a FlySight's readings are counts times the step: s is
// 16 / 32768 x 9.80665 m/s^2 for the accelerometer and .070 deg/s for the gyro
// (the datasheet's sensitivity at +/-2000 deg/s). The rounding draws nothing,
// so the GNSS samples are what they were before it.
// All times are UTC seconds: kEpochUtc + t.

namespace FlySightTest {

namespace {

constexpr double kEpochUtc = 1700000000.0;
constexpr double kStandardGravity = 9.80665;

// The stated ranges of every fixture, g and deg/s, and the gyro's lattice step
// at that range, deg/s.
constexpr double kAccelerometerRangeG = 16;
constexpr double kGyroRangeDegS = 2000;
constexpr double kGyroStepDegS = .070;

/// SplitMix64: a complete, portable generator in four lines of integer
/// arithmetic, so the noise is the same on every platform.
class NoiseSource {
public:
    explicit NoiseSource(quint64 seed) : m_state(seed) {}

    /// The next value, uniform in [-1, 1).
    double next()
    {
        quint64 z = (m_state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        return double(z >> 11) * 0x1.0p-53 * 2 - 1;
    }

private:
    quint64 m_state;
};

/// Uniform noise amplitudes of a fixture; all zero means exact data.
struct NoiseLevels {
    double force = 0;      // m/s^2
    double gyro = 0;       // deg/s
    double position = 0;   // m
    double velocity = 0;   // m/s
};

void appendGnss(FusionFixture &f, double t,
                double north, double east, double down,
                double velN, double velE, double velD,
                double hAcc, double vAcc, double sAcc)
{
    f.gnssTime.append(kEpochUtc + t);
    f.north.append(north);  f.east.append(east);  f.down.append(down);
    f.velN.append(velN);    f.velE.append(velE);  f.velD.append(velD);
    f.hAcc.append(hAcc);    f.vAcc.append(vAcc);  f.sAcc.append(sAcc);
}

void appendImu(FusionFixture &f, double t,
               double ax, double ay, double az,
               double wx, double wy, double wz,
               double temperature)
{
    f.imuTime.append(kEpochUtc + t);
    f.ax.append(ax);  f.ay.append(ay);  f.az.append(az);
    f.wx.append(wx);  f.wy.append(wy);  f.wz.append(wz);
    f.imuTemperature.append(temperature);
}

/// The fixtures' configuration: the stated ranges, and `rateHz` for both
/// sensors.
void stateConfiguration(FusionFixture &f, double rateHz)
{
    f.accelFsG = kAccelerometerRangeG;
    f.gyroFsDegS = kGyroRangeDegS;
    f.accelOdrHz = rateHz;
    f.gyroOdrHz = rateHz;
}

/// Every reading of `f` rounded onto a lattice: the gyro onto the stated
/// range's, the accelerometer onto that of `accelerometerRangeG` (the stated
/// range, except for the fixture whose readings must show another).
void roundOntoLattice(FusionFixture &f, double accelerometerRangeG = kAccelerometerRangeG)
{
    const double accelerometerStep = accelerometerRangeG / 32768 * kStandardGravity;
    for (QVector<double> *axis : { &f.ax, &f.ay, &f.az }) {
        for (double &value : *axis)
            value = std::round(value / accelerometerStep) * accelerometerStep;
    }
    for (QVector<double> *axis : { &f.wx, &f.wy, &f.wz }) {
        for (double &value : *axis)
            value = std::round(value / kGyroStepDegS) * kGyroStepDegS;
    }
}

// ---------------------------------------------------------------------------
// Success fixtures
// ---------------------------------------------------------------------------

/// The reference self-test's "linear" case: 2 s of constant velocity
/// (12, -4, 2) m/s from (7, 8, 9) m, level and not rotating, exact data.
/// IMU 100 Hz, i = 0..200; GNSS t = .037 + i * .2, i = 0..8; hAcc = vAcc = 1,
/// sAcc = .1; origin index 0. Shorter than one segment (the 60 s prefix
/// covers it), with an exactly unobservable yaw (the four prefix starts tie
/// and the first wins), and its objective is nearly zero. IMU temperature
/// 25 degC throughout. States 104 Hz; its readings (0 and -9.80665 m/s^2,
/// -2048 counts) are on the lattice already.
FusionFixture coarseLinear()
{
    FusionFixture f;
    f.name = QStringLiteral("coarse_linear");
    for (int i = 0; i <= 8; ++i) {
        const double t = .037 + i * .2;
        appendGnss(f, t, 7 + t * 12, 8 + t * -4, 9 + t * 2, 12, -4, 2, 1, 1, .1);
    }
    for (int i = 0; i <= 200; ++i)
        appendImu(f, i * .01, 0, 0, -kStandardGravity, 0, 0, 0, kFixtureTemperatureDegC);
    f.originIndex = 0;
    stateConfiguration(f, 104);
    roundOntoLattice(f);
    return f;
}

/// 6 s of smoothly changing acceleration with sensor biases and noise.
///
/// NED acceleration a(t) = (1.5 - .4t, .8t, -.6 + .1t^2), v(0) = (20, -5, 3),
/// p(0) = 0; velocity and position are its exact polynomial integrals. The
/// attitude is the identity, so specific force = a - (0, 0, g) + accelerometer
/// bias (.05, -.03, .08) and the gyro reads only its bias (.2, -.15, .3) deg/s.
/// IMU 100 Hz, i = 0..600. GNSS t = -.163 + j * .2, j = 0..32: one fix before
/// and two after IMU coverage (trimmed, not rejected). Origin index 3; hAcc is
/// 12 m before the origin and 1.5 m from it on; vAcc = 2.5, sAcc = .3.
/// Uniform noise: force .02, gyro .05 deg/s, position .3, velocity .1;
/// seed 0x8F050002. IMU temperature 25 degC throughout. States 104 Hz; the
/// accelerometer is rounded onto the lattice of `accelerometerRangeG` (the
/// stated +/-16 g unless reject_lattice asks for another).
FusionFixture coarseManeuver(double accelerometerRangeG = kAccelerometerRangeG)
{
    FusionFixture f;
    f.name = QStringLiteral("coarse_maneuver");
    const NoiseLevels noise{ .02, .05, .3, .1 };
    NoiseSource source(0x8F050002ull);

    for (int j = 0; j <= 32; ++j) {
        const double t = -.163 + j * .2;
        const double north = 20 * t + (.75 * t * t - .2 * t * t * t / 3);
        const double east = -5 * t + .4 * t * t * t / 3;
        const double down = 3 * t + (-.3 * t * t + t * t * t * t / 120);
        const double velN = 20 + (1.5 * t - .2 * t * t);
        const double velE = -5 + .4 * t * t;
        const double velD = 3 + (-.6 * t + t * t * t / 30);
        const double n0 = source.next(), n1 = source.next(), n2 = source.next();
        const double n3 = source.next(), n4 = source.next(), n5 = source.next();
        appendGnss(f, t,
                   north + noise.position * n0, east + noise.position * n1, down + noise.position * n2,
                   velN + noise.velocity * n3, velE + noise.velocity * n4, velD + noise.velocity * n5,
                   j < 3 ? 12 : 1.5, 2.5, .3);
    }
    for (int i = 0; i <= 600; ++i) {
        const double t = i * .01;
        const double ax = (1.5 - .4 * t) + .05;
        const double ay = .8 * t + -.03;
        const double az = (-.6 + .1 * t * t) - kStandardGravity + .08;
        const double n0 = source.next(), n1 = source.next(), n2 = source.next();
        const double n3 = source.next(), n4 = source.next(), n5 = source.next();
        appendImu(f, t,
                  ax + noise.force * n0, ay + noise.force * n1, az + noise.force * n2,
                  .2 + noise.gyro * n3, -.15 + noise.gyro * n4, .3 + noise.gyro * n5,
                  kFixtureTemperatureDegC);
    }
    f.originIndex = 3;
    stateConfiguration(f, 104);
    roundOntoLattice(f, accelerometerRangeG);
    return f;
}

/// 40 s at rest, level, then spinning about the vertical axis.
///
/// Position and velocity are zero. Specific force = (0, 0, -g) + accelerometer
/// bias (.03, -.02, .05); the gyro reads its bias (.2, -.1, .15) deg/s, plus
/// 90 deg/s on wz from t = 31 s on (810 degrees of yaw: more than two turns).
/// IMU 25 Hz, i = 0..1000 (keeps the golden small); GNSS t = .1 + j * .2,
/// j = 0..199; hAcc = 1, vAcc = 1.5, sAcc = .1; origin index 0. Uniform noise:
/// force .005, gyro .02 deg/s, position .2, velocity .03; seed 0x8F050003.
/// One segment at rest whose prefix grows from 60 s to 120 s to cover it
/// (yaw unobservable and arbitrary; the anchor is the first fix, every sAcc
/// being equal). IMU temperature 25 degC throughout. States 26 Hz.
FusionFixture stationarySpin()
{
    FusionFixture f;
    f.name = QStringLiteral("stationary_spin");
    const NoiseLevels noise{ .005, .02, .2, .03 };
    NoiseSource source(0x8F050003ull);

    for (int j = 0; j <= 199; ++j) {
        const double t = .1 + j * .2;
        const double n0 = source.next(), n1 = source.next(), n2 = source.next();
        const double n3 = source.next(), n4 = source.next(), n5 = source.next();
        appendGnss(f, t,
                   noise.position * n0, noise.position * n1, noise.position * n2,
                   noise.velocity * n3, noise.velocity * n4, noise.velocity * n5,
                   1, 1.5, .1);
    }
    for (int i = 0; i <= 1000; ++i) {
        const double t = i * .04;
        const double spin = t >= 31 ? 90 : 0;
        const double n0 = source.next(), n1 = source.next(), n2 = source.next();
        const double n3 = source.next(), n4 = source.next(), n5 = source.next();
        appendImu(f, t,
                  .03 + noise.force * n0, -.02 + noise.force * n1,
                  (-kStandardGravity + .05) + noise.force * n2,
                  .2 + noise.gyro * n3, -.1 + noise.gyro * n4, (.15 + spin) + noise.gyro * n5,
                  kFixtureTemperatureDegC);
    }
    f.originIndex = 0;
    stateConfiguration(f, 26);
    roundOntoLattice(f);
    return f;
}

// ---------------------------------------------------------------------------
// The synthetic recordings of the kernel's model tests: the initializer's
// (spec section 10), the scale state's and the long GNSS hole's. Not golden
// fixtures: their expectations are stated in tst_fusion_kernel. Rotation
// constants are exact rationals (.6 / .8 and .96 / .28, Pythagorean), so no
// transcendental function appears; the body force is R^T (a - g) + b_a with
// g = (0, 0, 9.80665) in NED, and the gyro reads its bias only (the attitude
// is constant in every recording). Each states its configuration and is
// rounded onto its lattice like the golden fixtures; the two at 1 Hz GNSS log
// the IMU at 12.5 Hz, the nearest listed rate's own, and the long-hole
// recording logs it at 100 Hz like coarse_maneuver.
// ---------------------------------------------------------------------------

/// 90 s that start in motion: constant 20 m/s north with a 2 m/s^2 east
/// manoeuvre from t = 40 to 50 s, so the 60 s prefix window [0, 30] has no
/// yaw information and the 120 s window [0, 60] has.
///
/// GNSS 5 Hz, t = j * .2, j = 0..449; IMU 25 Hz, t = i * .04, i = 0..2250.
/// Attitude Rz(psi) with cos psi = .6, sin psi = .8 (53.13 deg). NED: vN = 20,
/// pN = 20 t; aE = 2 for 40 <= t < 50, else 0; vE = 0 / 2 (t - 40) / 20 and
/// pE = 0 / (t - 40)^2 / 100 + 20 (t - 50) on the three pieces; down zero.
/// Body force (.8 aE + .05, .6 aE - .03, -9.80665 + .08); gyro (.2, -.15, .3)
/// deg/s. hAcc = 1.5, vAcc = 2.5, sAcc = .3 (every fix: the anchor is the
/// first). Noise: force .005, gyro .02 deg/s, position .2, velocity .03;
/// seed 0x8F050004. 450 states. States 26 Hz.
FusionFixture motionStart()
{
    FusionFixture f;
    f.name = QStringLiteral("motion_start");
    const NoiseLevels noise{ .005, .02, .2, .03 };
    NoiseSource source(0x8F050004ull);

    const auto eastAcceleration = [](double t) { return t >= 40 && t < 50 ? 2. : 0.; };
    for (int j = 0; j <= 449; ++j) {
        const double t = j * .2;
        const double velE = t < 40 ? 0 : t < 50 ? 2 * (t - 40) : 20;
        const double east = t < 40 ? 0 : t < 50 ? (t - 40) * (t - 40) : 100 + 20 * (t - 50);
        const double n0 = source.next(), n1 = source.next(), n2 = source.next();
        const double n3 = source.next(), n4 = source.next(), n5 = source.next();
        appendGnss(f, t,
                   20 * t + noise.position * n0, east + noise.position * n1, noise.position * n2,
                   20 + noise.velocity * n3, velE + noise.velocity * n4, noise.velocity * n5,
                   1.5, 2.5, .3);
    }
    for (int i = 0; i <= 2250; ++i) {
        const double t = i * .04;
        const double aE = eastAcceleration(t);
        const double n0 = source.next(), n1 = source.next(), n2 = source.next();
        const double n3 = source.next(), n4 = source.next(), n5 = source.next();
        appendImu(f, t,
                  (.8 * aE + .05) + noise.force * n0, (.6 * aE - .03) + noise.force * n1,
                  (-kStandardGravity + .08) + noise.force * n2,
                  .2 + noise.gyro * n3, -.15 + noise.gyro * n4, .3 + noise.gyro * n5,
                  kFixtureTemperatureDegC);
    }
    f.originIndex = 0;
    stateConfiguration(f, 26);
    roundOntoLattice(f);
    return f;
}

/// 300 s at rest, tilted: longer than two prefix doublings, so that the
/// growth stop of spec 3.2 d can be exercised. Roll and pitch are the
/// truth's; yaw is arbitrary.
///
/// IMU 25 Hz, t = i * .04, i = 0..7500; GNSS t = .1 + j * .2, j = 0..1499;
/// position and velocity zero. Attitude Ry(theta), cos theta = .96,
/// sin theta = .28, as the matrix [[.96, 0, .28], [0, 1, 0], [-.28, 0, .96]];
/// body force (.28 * 9.80665 + .03, -.02, -.96 * 9.80665 + .05); gyro
/// (.2, -.1, .15) deg/s. hAcc = 1, vAcc = 1.5, sAcc = .1. States 26 Hz. The
/// IMU noise is at the level the datasheet gives a resting unit: its
/// amplitudes are the per-sample sigmas of the stated configuration (force
/// .0041278 m/s^2, gyro .022982 deg/s: docs/SENSOR_FUSION.md section 4);
/// position .2, velocity .03; seed 0x8F050005. 1500 states.
FusionFixture restThroughout()
{
    FusionFixture f;
    f.name = QStringLiteral("rest_throughout");
    const NoiseLevels noise{ .0041278, .022982, .2, .03 };
    NoiseSource source(0x8F050005ull);

    for (int j = 0; j <= 1499; ++j) {
        const double t = .1 + j * .2;
        const double n0 = source.next(), n1 = source.next(), n2 = source.next();
        const double n3 = source.next(), n4 = source.next(), n5 = source.next();
        appendGnss(f, t,
                   noise.position * n0, noise.position * n1, noise.position * n2,
                   noise.velocity * n3, noise.velocity * n4, noise.velocity * n5,
                   1, 1.5, .1);
    }
    for (int i = 0; i <= 7500; ++i) {
        const double t = i * .04;
        const double n0 = source.next(), n1 = source.next(), n2 = source.next();
        const double n3 = source.next(), n4 = source.next(), n5 = source.next();
        appendImu(f, t,
                  (.28 * kStandardGravity + .03) + noise.force * n0, -.02 + noise.force * n1,
                  (-.96 * kStandardGravity + .05) + noise.force * n2,
                  .2 + noise.gyro * n3, -.1 + noise.gyro * n4, .15 + noise.gyro * n5,
                  kFixtureTemperatureDegC);
    }
    f.originIndex = 0;
    stateConfiguration(f, 26);
    roundOntoLattice(f);
    return f;
}

/// 300 s at 15 m/s north with a 3 m/s^2 east manoeuvre from t = 190 to
/// 200 s, whose fixes carry sAcc 2 m/s except the one at 200 s with .3: that
/// fix is the anchor, the first prefix is the window 170..230 s (length 60,
/// unclipped) and it contains the manoeuvre.
///
/// GNSS 1 Hz, t = j, j = 0..300; IMU 12.5 Hz, t = i / 12.5, i = 0..3750
/// (exact at the integer manoeuvre bounds). Attitude identity. vN = 15, pN = 15 t; aE = 3 for 190 <= t < 200, else 0; vE =
/// 0 / 3 (t - 190) / 30 and pE = 0 / 1.5 (t - 190)^2 / 150 + 30 (t - 200) on
/// the three pieces. Body force (.05, aE - .03, -9.80665 + .08); gyro
/// (.2, -.15, .3) deg/s. hAcc = 1.5, vAcc = 2.5. Noise .005, .02, .2, .03;
/// seed 0x8F050006. 301 states; the prefix is 61. States 12.5 Hz.
FusionFixture saccAnchor()
{
    FusionFixture f;
    f.name = QStringLiteral("sacc_anchor");
    const NoiseLevels noise{ .005, .02, .2, .03 };
    NoiseSource source(0x8F050006ull);

    for (int j = 0; j <= 300; ++j) {
        const double t = j;
        const double velE = t < 190 ? 0 : t < 200 ? 3 * (t - 190) : 30;
        const double east = t < 190 ? 0 : t < 200 ? 1.5 * (t - 190) * (t - 190) : 150 + 30 * (t - 200);
        const double n0 = source.next(), n1 = source.next(), n2 = source.next();
        const double n3 = source.next(), n4 = source.next(), n5 = source.next();
        appendGnss(f, t,
                   15 * t + noise.position * n0, east + noise.position * n1, noise.position * n2,
                   15 + noise.velocity * n3, velE + noise.velocity * n4, noise.velocity * n5,
                   1.5, 2.5, j == 200 ? .3 : 2);
    }
    for (int i = 0; i <= 3750; ++i) {
        const double t = i / 12.5;
        const double aE = t >= 190 && t < 200 ? 3 : 0;
        const double n0 = source.next(), n1 = source.next(), n2 = source.next();
        const double n3 = source.next(), n4 = source.next(), n5 = source.next();
        appendImu(f, t,
                  .05 + noise.force * n0, (aE - .03) + noise.force * n1,
                  (-kStandardGravity + .08) + noise.force * n2,
                  .2 + noise.gyro * n3, -.15 + noise.gyro * n4, .3 + noise.gyro * n5,
                  kFixtureTemperatureDegC);
    }
    f.originIndex = 0;
    stateConfiguration(f, 12.5);
    roundOntoLattice(f);
    return f;
}

/// 200 s at 15 m/s north with an east manoeuvre in every 30 s block and a
/// gyro z bias that drifts linearly by exactly 1 deg/s over the length while
/// the attitude does not rotate: under segmentLength = 60, minFinalSegment =
/// 12 it has four segments of 60, 60, 60 and 21 fixes, each with its
/// manoeuvre in the first 20 s (inside the 30 s half-window of the anchor,
/// which is the segment's first fix), and every segment fit converges on a
/// constant bias.
///
/// GNSS 1 Hz, t = j, j = 0..200; IMU 12.5 Hz, t = i / 12.5, i = 0..2500.
/// Attitude identity. vN = 15, pN = 15 t. With k = j / 30 (GNSS) or i / 375
/// (IMU) as integer division and s = t - 30 k: aE = 2 for 10 <= s < 15, -2 for
/// 15 <= s < 20, else 0; vE = 0 / 2 (s - 10) / 10 - 2 (s - 15) / 0; pE = 50 k
/// + (0 / (s - 10)^2 / 25 + 10 (s - 15) - (s - 15)^2 / 50) on the four pieces
/// (continuous: 25 at s = 15, 50 at s = 20). Body force (.05, aE - .03,
/// -9.80665 + .08); gyro (.2, -.15, .3 + t / 200) deg/s. hAcc = 1.5,
/// vAcc = 2.5, sAcc = .3. Noise .005, .02, .2, .03; seed 0x8F050007. 201 states.
/// States 12.5 Hz.
///
/// The IMU temperature ramps linearly, 25 + t / 10 degC (25 at t = 0 to 45
/// at t = 200), so the z bias .3 + t / 200 deg/s is .3 + (T - 25) / 20 deg/s:
/// exactly 0.05 deg/s per degC. Under the temperature model this is
/// b1 = (0, 0, 0.05 deg/s/degC), T_ref = 35 (the mean of 25 + (i / 12.5) / 10
/// over i = 0..2500) and b0 = (.2, -.15, .8) deg/s, the bias at T_ref. The 20 degC
/// excursion is what makes the data dominate the slope's prior (the model's
/// information on b1 grows with the square of the excursion).
FusionFixture driftingBias()
{
    FusionFixture f;
    f.name = QStringLiteral("drifting_bias");
    const NoiseLevels noise{ .005, .02, .2, .03 };
    NoiseSource source(0x8F050007ull);

    const auto eastAcceleration = [](double s) { return s < 10 ? 0. : s < 15 ? 2. : s < 20 ? -2. : 0.; };
    for (int j = 0; j <= 200; ++j) {
        const double t = j;
        const int k = j / 30;
        const double s = t - 30 * k;
        const double velE = s < 10 ? 0 : s < 15 ? 2 * (s - 10) : s < 20 ? 10 - 2 * (s - 15) : 0;
        const double east = 50 * k + (s < 10 ? 0
                                      : s < 15 ? (s - 10) * (s - 10)
                                      : s < 20 ? 25 + 10 * (s - 15) - (s - 15) * (s - 15)
                                      : 50);
        const double n0 = source.next(), n1 = source.next(), n2 = source.next();
        const double n3 = source.next(), n4 = source.next(), n5 = source.next();
        appendGnss(f, t,
                   15 * t + noise.position * n0, east + noise.position * n1, noise.position * n2,
                   15 + noise.velocity * n3, velE + noise.velocity * n4, noise.velocity * n5,
                   1.5, 2.5, .3);
    }
    for (int i = 0; i <= 2500; ++i) {
        const double t = i / 12.5;
        const int k = i / 375;
        const double aE = eastAcceleration(t - 30 * k);
        const double n0 = source.next(), n1 = source.next(), n2 = source.next();
        const double n3 = source.next(), n4 = source.next(), n5 = source.next();
        appendImu(f, t,
                  .05 + noise.force * n0, (aE - .03) + noise.force * n1,
                  (-kStandardGravity + .08) + noise.force * n2,
                  .2 + noise.gyro * n3, -.15 + noise.gyro * n4, (.3 + t / 200) + noise.gyro * n5,
                  25 + t / 10);
    }
    f.originIndex = 0;
    stateConfiguration(f, 12.5);
    roundOntoLattice(f);
    return f;
}

/// 60 s level, heading north and not rotating, with the accelerometer's x
/// axis 2 % high: the recording of the scale-state test (the documented noise
/// model, clause 54). The north acceleration is periodic with zero mean, so
/// that the scale of the x axis is not confounded with its bias.
///
/// GNSS 5 Hz, t = .1 + j * .2, j = 0..299; IMU 25 Hz, t = i * .04,
/// i = 0..1500. Period P = 10 s, amplitude A = 5 m/s^2, c = 10; the period
/// index k and the phase tau by integer arithmetic: k = i / 250 and
/// tau = (i - 250 k) * .04 for the IMU, k = (2 j + 1) / 100 and
/// tau = .1 + (j - 50 k) * .2 for GNSS (no fix falls on a period boundary).
/// With x = tau / P: north acceleration a = A c x (1 - x)(1 - 2 x) (zero mean
/// per period, C1 across period ends, peak 4.8 m/s^2); vN = 10 + A P c x^2
/// (1 - x)^2 / 2 (a swing of 15.6 m/s); pN = k (10 P + A P^2 c / 60) + 10 tau
/// + A P^2 c (x^3 / 3 - x^4 / 2 + x^5 / 5) / 2; east and down zero. Body force
/// (1.02 (a + .05), -.03, -9.80665 + .08): the x reading, bias included, 2 %
/// high, the other axes nominal; gyro (.2, -.15, .3) deg/s. hAcc = 1,
/// vAcc = 1.5, sAcc = .1. Noise: force .005, gyro .02 deg/s, position .2,
/// velocity .03; seed 0x8F050009. 300 states. States 26 Hz.
FusionFixture scaleRecording()
{
    FusionFixture f;
    f.name = QStringLiteral("scale_recording");
    const NoiseLevels noise{ .005, .02, .2, .03 };
    NoiseSource source(0x8F050009ull);
    const double A = 5, c = 10, P = 10, vNominal = 10, accelerometerScaleX = 1.02;

    for (int j = 0; j <= 299; ++j) {
        const double t = .1 + j * .2;
        const int k = (2 * j + 1) / 100;
        const double tau = .1 + (j - 50 * k) * .2;
        const double x = tau / P;
        const double velN = vNominal + A * P * c * x * x * (1 - x) * (1 - x) / 2;
        const double north = k * (vNominal * P + A * P * P * c / 60) + vNominal * tau
                             + A * P * P * c * (x * x * x / 3 - x * x * x * x / 2 + x * x * x * x * x / 5) / 2;
        const double n0 = source.next(), n1 = source.next(), n2 = source.next();
        const double n3 = source.next(), n4 = source.next(), n5 = source.next();
        appendGnss(f, t,
                   north + noise.position * n0, noise.position * n1, noise.position * n2,
                   velN + noise.velocity * n3, noise.velocity * n4, noise.velocity * n5,
                   1, 1.5, .1);
    }
    for (int i = 0; i <= 1500; ++i) {
        const double t = i * .04;
        const int k = i / 250;
        const double tau = (i - 250 * k) * .04;
        const double x = tau / P;
        const double aN = A * c * x * (1 - x) * (1 - 2 * x);
        const double n0 = source.next(), n1 = source.next(), n2 = source.next();
        const double n3 = source.next(), n4 = source.next(), n5 = source.next();
        appendImu(f, t,
                  accelerometerScaleX * (aN + .05) + noise.force * n0, -.03 + noise.force * n1,
                  (-kStandardGravity + .08) + noise.force * n2,
                  .2 + noise.gyro * n3, -.15 + noise.gyro * n4, .3 + noise.gyro * n5,
                  kFixtureTemperatureDegC);
    }
    f.originIndex = 0;
    stateConfiguration(f, 26);
    roundOntoLattice(f);
    return f;
}

} // namespace

/// 120 s with a hole of `holeSeconds` in the fixes and the IMU running through
/// it: the long-hole recording of the GNSS-hole tests, long_hole with the hole
/// at the longest the fit bridges, 30 s, and the rejection above that limit
/// with 60 s. coarse_maneuver's kind of motion with bounded acceleration over
/// the whole length, and scale_recording's periodic manoeuvre on the north
/// axis, so that every window, the hole and each side of it included, holds
/// the horizontal acceleration that makes yaw observable.
///
/// GNSS 5 Hz, t = .1 + j * .2, j = 0..599, with the fixes
/// j = 150 .. 148 + 5 holeSeconds removed: a hole of `holeSeconds` (whole
/// seconds, 1 to 90) from the fix at 29.9 s, 150 fixes before it and
/// 451 - 5 holeSeconds after (301 at 30 s: j = 150..298, the next fix at
/// 59.9 s; 151 at 60 s: j = 150..448, the next at 89.9 s); the noise of every
/// j is drawn, removed or not, so the fixes kept are the same bits whatever
/// the hole. IMU 100 Hz, t = i * .01, i = 0..12000, continuous.
/// Attitude identity. North acceleration a = A c x (1 - x)(1 - 2 x) with
/// A = 2 m/s^2, c = 10, x = tau / P, P = 10 s (zero mean per period, peak
/// 1.92 m/s^2); the period index and phase by integer arithmetic, k = i / 1000
/// and tau = (i - 1000 k) * .01 for the IMU, k = (2 j + 1) / 100 and
/// tau = .1 + (j - 50 k) * .2 for GNSS; vN = 20 + A P c x^2 (1 - x)^2 / 2,
/// pN = k (20 P + A P^2 c / 60) + 20 tau + A P^2 c (x^3 / 3 - x^4 / 2 + x^5 / 5) / 2.
/// East and down constant, vE = -5 and vD = 3 m/s from p = 0. Body force
/// (a + .05, -.03, -9.80665 + .08); gyro (.2, -.15, .3) deg/s: coarse_maneuver's
/// biases. hAcc = 1.5, vAcc = 2.5, sAcc = .3 (every fix: the anchor is the
/// first). coarse_maneuver's noise, force .02, gyro .05 deg/s, position .3,
/// velocity .1; seed 0x8F05000A. 601 - 5 holeSeconds states (451 at 30 s).
/// Temperature 25 degC. States 104 Hz; readings rounded onto the lattice.
FusionFixture longHole(int holeSeconds)
{
    FusionFixture f;
    f.name = QStringLiteral("long_hole");
    const NoiseLevels noise{ .02, .05, .3, .1 };
    NoiseSource source(0x8F05000Aull);
    const double A = 2, c = 10, P = 10, vNominal = 20, vEast = -5, vDown = 3;

    for (int j = 0; j <= 599; ++j) {
        const double t = .1 + j * .2;
        const int k = (2 * j + 1) / 100;
        const double tau = .1 + (j - 50 * k) * .2;
        const double x = tau / P;
        const double velN = vNominal + A * P * c * x * x * (1 - x) * (1 - x) / 2;
        const double north = k * (vNominal * P + A * P * P * c / 60) + vNominal * tau
                             + A * P * P * c * (x * x * x / 3 - x * x * x * x / 2 + x * x * x * x * x / 5) / 2;
        const double n0 = source.next(), n1 = source.next(), n2 = source.next();
        const double n3 = source.next(), n4 = source.next(), n5 = source.next();
        if (j >= 150 && j < 149 + 5 * holeSeconds)
            continue;
        appendGnss(f, t,
                   north + noise.position * n0, vEast * t + noise.position * n1, vDown * t + noise.position * n2,
                   velN + noise.velocity * n3, vEast + noise.velocity * n4, vDown + noise.velocity * n5,
                   1.5, 2.5, .3);
    }
    for (int i = 0; i <= 12000; ++i) {
        const double t = i * .01;
        const int k = i / 1000;
        const double tau = (i - 1000 * k) * .01;
        const double x = tau / P;
        const double aN = A * c * x * (1 - x) * (1 - 2 * x);
        const double n0 = source.next(), n1 = source.next(), n2 = source.next();
        const double n3 = source.next(), n4 = source.next(), n5 = source.next();
        appendImu(f, t,
                  (aN + .05) + noise.force * n0, -.03 + noise.force * n1,
                  (-kStandardGravity + .08) + noise.force * n2,
                  .2 + noise.gyro * n3, -.15 + noise.gyro * n4, .3 + noise.gyro * n5,
                  kFixtureTemperatureDegC);
    }
    f.originIndex = 0;
    stateConfiguration(f, 104);
    roundOntoLattice(f);
    return f;
}

namespace {

// ---------------------------------------------------------------------------
// Fixtures made by one mutation: bridged_hole and the rejections
// ---------------------------------------------------------------------------

FusionFixture rejection(FusionFixture base, const char *name)
{
    base.name = QString::fromLatin1(name);
    base.expectSuccess = false;
    return base;
}

/// Every per-fix GNSS channel of `f`, so a mutation can treat them alike.
QList<QVector<double> *> gnssChannels(FusionFixture &f)
{
    return { &f.gnssTime, &f.north, &f.east, &f.down, &f.velN, &f.velE, &f.velD,
             &f.hAcc, &f.vAcc, &f.sAcc };
}

/// Every per-sample IMU channel of `f`.
QList<QVector<double> *> imuChannels(FusionFixture &f)
{
    return { &f.imuTime, &f.ax, &f.ay, &f.az, &f.wx, &f.wy, &f.wz, &f.imuTemperature };
}

void removeSamples(const QList<QVector<double> *> &channels, qsizetype first, qsizetype count)
{
    for (QVector<double> *channel : channels)
        channel->remove(first, count);
}

void truncateTo(const QList<QVector<double> *> &channels, qsizetype count)
{
    for (QVector<double> *channel : channels)
        channel->resize(count);
}

/// coarse_maneuver with GNSS fixes 12..23 removed: a hole of 2.6 s in the
/// fixes (from the fix at 2.037 s to the one at 4.637 s, seconds of the
/// generator) with the IMU continuous through it, bridged by the one IMU
/// factor that spans the interval. The fit starts at the origin, j = 3: 9
/// fitted fixes before the hole, 7 after.
FusionFixture bridgedHole()
{
    FusionFixture f = coarseManeuver();
    f.name = QStringLiteral("bridged_hole");
    removeSamples(gnssChannels(f), 12, 12);
    return f;
}

QList<FusionFixture> rejectionFixtures()
{
    QList<FusionFixture> fixtures;

    // A non-finite sample in a declared input
    FusionFixture f = rejection(coarseLinear(), "reject_nonfinite");
    f.ax[10] = std::numeric_limits<double>::quiet_NaN();
    fixtures.append(f);

    // Time that does not strictly increase
    f = rejection(coarseLinear(), "reject_time_order");
    f.imuTime[4] = f.imuTime[3];
    fixtures.append(f);

    // Fewer than three GNSS fixes in the recording
    f = rejection(coarseLinear(), "reject_too_few_fixes");
    truncateTo(gnssChannels(f), 2);
    fixtures.append(f);

    // IMU coverage (0 .. .35 s) holds only two of the fixes
    f = rejection(coarseLinear(), "reject_coverage");
    truncateTo(imuChannels(f), 36);
    fixtures.append(f);

    // IMU samples 40..49 missing: a .11 s gap inside the GNSS span
    f = rejection(coarseLinear(), "reject_imu_gap");
    removeSamples(imuChannels(f), 40, 10);
    fixtures.append(f);

    // A GNSS accuracy that is not positive
    f = rejection(coarseLinear(), "reject_sigma");
    f.sAcc[0] = 0;
    fixtures.append(f);

    // One channel shorter than its time axis
    f = rejection(coarseLinear(), "reject_length");
    f.velE.removeLast();
    fixtures.append(f);

    // Local origin index past the last fix
    f = rejection(coarseLinear(), "reject_origin");
    f.originIndex = 9;
    fixtures.append(f);

    // Accelerometer readings on the +/-8 g lattice under a stated +/-16 g
    fixtures.append(rejection(coarseManeuver(8), "reject_lattice"));

    // 12.5 Hz stated for both sensors of a recording logged at 100 Hz: a
    // key-less file from a faster firmware
    f = rejection(coarseLinear(), "reject_rate");
    stateConfiguration(f, 12.5);
    fixtures.append(f);

    return fixtures;
}

} // namespace

QList<FusionFixture> fusionFixtures()
{
    QList<FusionFixture> fixtures{ coarseLinear(), coarseManeuver(), stationarySpin(), bridgedHole() };
    fixtures.append(rejectionFixtures());
    return fixtures;
}

FusionFixture fusionFixture(const QString &name)
{
    const QList<FusionFixture> fixtures = fusionFixtures();
    for (const FusionFixture &fixture : fixtures) {
        if (fixture.name == name)
            return fixture;
    }
    return FusionFixture();
}

FusionFixture initializerFixture(const QString &name)
{
    if (name == QStringLiteral("motion_start"))
        return motionStart();
    if (name == QStringLiteral("rest_throughout"))
        return restThroughout();
    if (name == QStringLiteral("sacc_anchor"))
        return saccAnchor();
    if (name == QStringLiteral("drifting_bias"))
        return driftingBias();
    if (name == QStringLiteral("scale_recording"))
        return scaleRecording();
    if (name == QStringLiteral("long_hole"))
        return longHole(30);
    return FusionFixture();
}

} // namespace FlySightTest
