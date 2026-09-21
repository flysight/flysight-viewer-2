#include "fusionfixtures.h"

#include <limits>

// Rules that keep these recordings bit-identical with any IEEE-754 compiler
// (the capture harness and the ported tests must see the same inputs):
//   - only + - * / are used; no transcendental function, no <random>
//     distribution;
//   - every sample is computed from its index (t = i * .01), never by
//     accumulation;
//   - noise comes from SplitMix64, mapped to [-1, 1), drawn in one documented
//     order: all GNSS samples first, in time order, each drawing north, east,
//     down, velN, velE, velD; then all IMU samples in time order, each drawing
//     ax, ay, az, wx, wy, wz. Accuracies and times carry no noise.
// All times are UTC seconds: kEpochUtc + t.

namespace FlySightTest {

namespace {

constexpr double kEpochUtc = 1700000000.0;
constexpr double kStandardGravity = 9.80665;

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
               double wx, double wy, double wz)
{
    f.imuTime.append(kEpochUtc + t);
    f.ax.append(ax);  f.ay.append(ay);  f.az.append(az);
    f.wx.append(wx);  f.wy.append(wy);  f.wz.append(wz);
}

// ---------------------------------------------------------------------------
// Success fixtures
// ---------------------------------------------------------------------------

/// The reference self-test's "linear" case: 2 s of constant velocity
/// (12, -4, 2) m/s from (7, 8, 9) m, level and not rotating, exact data.
/// IMU 100 Hz, i = 0..200; GNSS t = .037 + i * .2, i = 0..8; hAcc = vAcc = 1,
/// sAcc = .1; origin index 0. Too short for a stationary window, so it uses
/// the coarse initializer, and its objective is nearly zero.
FusionFixture coarseLinear()
{
    FusionFixture f;
    f.name = QStringLiteral("coarse_linear");
    for (int i = 0; i <= 8; ++i) {
        const double t = .037 + i * .2;
        appendGnss(f, t, 7 + t * 12, 8 + t * -4, 9 + t * 2, 12, -4, 2, 1, 1, .1);
    }
    for (int i = 0; i <= 200; ++i)
        appendImu(f, i * .01, 0, 0, -kStandardGravity, 0, 0, 0);
    f.originIndex = 0;
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
/// seed 0x8F050002.
FusionFixture coarseManeuver()
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
                  .2 + noise.gyro * n3, -.15 + noise.gyro * n4, .3 + noise.gyro * n5);
    }
    f.originIndex = 3;
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
/// The stationary window [0, 30) is accepted and [5, 35) is not, so the
/// attitude is anchored mid-window and propagated backwards to the start.
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
                  .2 + noise.gyro * n3, -.1 + noise.gyro * n4, (.15 + spin) + noise.gyro * n5);
    }
    f.originIndex = 0;
    return f;
}

// ---------------------------------------------------------------------------
// Rejection fixtures: one mutation each
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
    return { &f.imuTime, &f.ax, &f.ay, &f.az, &f.wx, &f.wy, &f.wz };
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

    // GNSS fixes 12..23 missing: a 2.6 s outage, above max(2 s, 5 x .2 s)
    f = rejection(coarseManeuver(), "reject_gnss_gap");
    removeSamples(gnssChannels(f), 12, 12);
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

    return fixtures;
}

} // namespace

QList<FusionFixture> fusionFixtures()
{
    QList<FusionFixture> fixtures{ coarseLinear(), coarseManeuver(), stationarySpin() };
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

} // namespace FlySightTest
