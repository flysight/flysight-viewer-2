#include "fusion/stationarywindow.h"

#include <algorithm>

#include "fusion/samplestatistics.h"

namespace FlySight::Fusion::Detail {

namespace {

// Data a window needs before any statistic means something
constexpr size_t kMinImuSamples = 10;
constexpr size_t kMinGnssSamples = 5;
constexpr double kMaxGnssInterval = .4;               // s; also the allowed slack at both window ends

// Gate thresholds. Rates are deg/s, forces m/s^2, velocities m/s.
constexpr double kMaxVelocityDeviationP95 = .3;       // about the trimmed mean velocity
constexpr double kMaxVelocitySigmaP95 = .5;           // reported GNSS speed accuracy
constexpr double kMaxNormalizedVelocityP95 = 3.5;     // deviation in units of its sigma
constexpr double kMaxVelocityDrift = .3;              // first half vs second half
constexpr double kMaxGyroStddev = .06;
constexpr double kMaxGyroDeviationP95 = .15;
constexpr double kMaxGyroDeviationPeak = .5;
constexpr double kMaxMeanRate = 1.;
constexpr double kMaxForceStddev = .025;
constexpr double kMaxForceDeviationP95 = .08;
constexpr double kMaxForceDeviationPeak = .25;
constexpr double kMaxTiltDrift = .03;                 // mean force, first half vs second half
constexpr double kMinGravityRatio = .7;               // |mean force| relative to standard gravity
constexpr double kMaxGravityRatio = 1.3;

/// The IMU samples inside a window, with the force also split at mid-window.
struct ImuWindow {
    std::vector<double> time;
    Vectors rate;                         // deg/s: the gyro gates are stated in degrees
    Vectors force, forceFirstHalf, forceSecondHalf;
};

/// The GNSS fixes inside a window, with the velocity also split at mid-window.
struct GnssWindow {
    std::vector<double> time;
    Vectors velocity, velocitySigma, velocityFirstHalf, velocitySecondHalf;
    std::vector<double> sigmaComponents;  // every component of every velocity sigma
};

ImuWindow collectImuWindow(const Samples &d, double start, double end)
{
    ImuWindow w;
    for (size_t i = 0; i < d.imuTime.size(); ++i) {
        if (d.imuTime[i] >= start && d.imuTime[i] < end) {
            w.time.push_back(d.imuTime[i]);
            w.rate.push_back(d.gyro[i]*180/kPi);
            w.force.push_back(d.force[i]);
            (d.imuTime[i] < (start+end)/2 ? w.forceFirstHalf : w.forceSecondHalf).push_back(d.force[i]);
        }
    }
    return w;
}

GnssWindow collectGnssWindow(const Samples &d, double start, double end)
{
    GnssWindow w;
    for (size_t i = 0; i < d.gnssTime.size(); ++i) {
        if (d.gnssTime[i] >= start && d.gnssTime[i] < end) {
            w.time.push_back(d.gnssTime[i]);
            w.velocity.push_back(d.velocity[i]);
            w.velocitySigma.push_back(d.velocitySigma[i]);
            (d.gnssTime[i] < (start+end)/2 ? w.velocityFirstHalf : w.velocitySecondHalf).push_back(d.velocity[i]);
            for (int j = 0; j < 3; ++j)
                w.sigmaComponents.push_back(d.velocitySigma[i][j]);
        }
    }
    return w;
}

/// Enough samples overall, and some in each half (the drift gates compare halves).
bool hasMinimumSamples(const ImuWindow &imu, const GnssWindow &gnss)
{
    return !(imu.time.size() < kMinImuSamples || gnss.time.size() < kMinGnssSamples
             || imu.forceFirstHalf.empty() || imu.forceSecondHalf.empty()
             || gnss.velocityFirstHalf.empty() || gnss.velocitySecondHalf.empty());
}

/// Distance of every value from `centre`.
std::vector<double> deviationNorms(const Vectors &values, const gtsam::Vector3 &centre)
{
    std::vector<double> norms;
    norms.reserve(values.size());
    for (const auto &x : values)
        norms.push_back((x-centre).norm());
    return norms;
}

/// Distance of every velocity from `centre`: in m/s (`deviation`) and in units
/// of that fix's own sigma (`normalized`).
void velocityDeviations(const GnssWindow &gnss, const gtsam::Vector3 &centre,
                        std::vector<double> &deviations, std::vector<double> &normalized)
{
    for (size_t i = 0; i < gnss.velocity.size(); ++i) {
        const gtsam::Vector3 deviation = gnss.velocity[i]-centre;
        deviations.push_back(deviation.norm());
        normalized.push_back(deviation.cwiseQuotient(gnss.velocitySigma[i]).norm());
    }
}

double largestInterval(const std::vector<double> &times)
{
    double gap = 0;
    for (size_t i = 1; i < times.size(); ++i)
        gap = std::max(gap, times[i]-times[i-1]);
    return gap;
}

/// Both sensors reach (nearly) to both ends of the window without a hole.
bool coversWindow(const ImuWindow &imu, const GnssWindow &gnss, double start, double end, double imuGap)
{
    return imu.time.front()-start < imuGap && end-imu.time.back() < imuGap
        && gnss.time.front()-start < kMaxGnssInterval && end-gnss.time.back() < kMaxGnssInterval
        && largestInterval(imu.time) < imuGap && largestInterval(gnss.time) < kMaxGnssInterval;
}

} // namespace

StationaryWindow assessStationaryWindow(const Samples &samples, double start, double end)
{
    StationaryWindow c;
    c.start = start;
    c.end = end;

    const ImuWindow imu = collectImuWindow(samples, start, end);
    const GnssWindow gnss = collectGnssWindow(samples, start, end);
    c.imuCount = imu.time.size();
    c.gnssCount = gnss.time.size();
    if (!hasMinimumSamples(imu, gnss)) {
        c.rejected = {"coverage"};
        return c;
    }

    const gtsam::Vector3 wm = componentMean(imu.rate, true), fm = componentMean(imu.force, true),
                         ws = componentStddev(imu.rate), fs = componentStddev(imu.force);
    // Constant translation is compatible with gravity-based initialization.
    // Test the velocity vector's stability, independent of its nonzero offset.
    const gtsam::Vector3 vm = componentMean(gnss.velocity, true);
    std::vector<double> velocityDeviation, normalized;
    velocityDeviations(gnss, vm, velocityDeviation, normalized);
    const std::vector<double> wd = deviationNorms(imu.rate, wm);
    const std::vector<double> fd = deviationNorms(imu.force, fm);

    auto check = [&c](bool ok, const char *name) { if (!ok) c.rejected.push_back(name); };
    const double gap = kImuGapMedians*medianInterval(samples.imuTime);

    // Gate order is the order of `rejected`.
    check(coversWindow(imu, gnss, start, end, gap), "coverage");
    check(quantile(velocityDeviation, .95) <= kMaxVelocityDeviationP95, "velocity_variability");
    check(quantile(gnss.sigmaComponents, .95) <= kMaxVelocitySigmaP95, "uncertainty");
    check(quantile(normalized, .95) <= kMaxNormalizedVelocityP95, "normalized_velocity_variability");
    check((componentMean(gnss.velocityFirstHalf)-componentMean(gnss.velocitySecondHalf)).norm() <= kMaxVelocityDrift, "velocity_drift");
    check(ws.maxCoeff() <= kMaxGyroStddev, "gyro_variability");
    check(quantile(wd, .95) <= kMaxGyroDeviationP95, "gyro_p95");
    check(quantile(wd, 1) <= kMaxGyroDeviationPeak, "gyro_peak");
    check(wm.norm() <= kMaxMeanRate, "mean_rate");
    check(fs.maxCoeff() <= kMaxForceStddev, "force_variability");
    check(quantile(fd, .95) <= kMaxForceDeviationP95, "force_p95");
    check(quantile(fd, 1) <= kMaxForceDeviationPeak, "force_peak");
    check((componentMean(imu.forceFirstHalf)-componentMean(imu.forceSecondHalf)).norm() <= kMaxTiltDrift, "tilt_drift");
    check(fm.norm() > kMinGravityRatio*kGravity.z() && fm.norm() < kMaxGravityRatio*kGravity.z(), "plausible_gravity");

    c.accepted = c.rejected.empty();
    c.forceMean = fm;
    c.gyroMean = wm*kPi/180;
    c.score = fs.norm();
    return c;
}

} // namespace FlySight::Fusion::Detail
