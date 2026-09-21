#include "fusion/stationarywindow.h"

#include <algorithm>
#include <utility>

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

/// Index range [first, second) of the entries with start <= t < end. `times`
/// is strictly increasing, so these are exactly the entries that a scan of the
/// whole axis with that test selects, in the same order, without the scan.
std::pair<size_t, size_t> indexRange(const std::vector<double> &times, double start, double end)
{
    if (!(start < end))
        return { 0, 0 };
    const auto first = std::lower_bound(times.begin(), times.end(), start);
    const auto last = std::lower_bound(first, times.end(), end);
    return { size_t(first-times.begin()), size_t(last-times.begin()) };
}

ImuWindow collectImuWindow(const Samples &d, double start, double end)
{
    ImuWindow w;
    const auto [first, last] = indexRange(d.imuTime, start, end);
    for (size_t i = first; i < last; ++i) {
        w.time.push_back(d.imuTime[i]);
        w.rate.push_back(d.gyro[i]*180/kPi);
        w.force.push_back(d.force[i]);
        (d.imuTime[i] < (start+end)/2 ? w.forceFirstHalf : w.forceSecondHalf).push_back(d.force[i]);
    }
    return w;
}

GnssWindow collectGnssWindow(const Samples &d, double start, double end)
{
    GnssWindow w;
    const auto [first, last] = indexRange(d.gnssTime, start, end);
    for (size_t i = first; i < last; ++i) {
        w.time.push_back(d.gnssTime[i]);
        w.velocity.push_back(d.velocity[i]);
        w.velocitySigma.push_back(d.velocitySigma[i]);
        (d.gnssTime[i] < (start+end)/2 ? w.velocityFirstHalf : w.velocitySecondHalf).push_back(d.velocity[i]);
        for (int j = 0; j < 3; ++j)
            w.sigmaComponents.push_back(d.velocitySigma[i][j]);
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

double imuGapLimit(const Samples &samples)
{
    return kImuGapMedians*medianInterval(samples.imuTime);
}

StationaryWindow assessStationaryWindow(const Samples &samples, double start, double end)
{
    return assessStationaryWindow(samples, start, end, imuGapLimit(samples));
}

StationaryWindow assessStationaryWindow(const Samples &samples, double start, double end,
                                        double imuGap)
{
    StationaryWindow verdict;
    verdict.start = start;
    verdict.end = end;

    const ImuWindow imu = collectImuWindow(samples, start, end);
    const GnssWindow gnss = collectGnssWindow(samples, start, end);
    verdict.imuCount = imu.time.size();
    verdict.gnssCount = gnss.time.size();
    if (!hasMinimumSamples(imu, gnss)) {
        verdict.rejected = {"coverage"};
        return verdict;
    }

    const gtsam::Vector3 rateMean = componentMean(imu.rate, true),
                         forceMean = componentMean(imu.force, true),
                         rateStddev = componentStddev(imu.rate),
                         forceStddev = componentStddev(imu.force);
    // Constant translation is compatible with gravity-based initialization.
    // Test the velocity vector's stability, independent of its nonzero offset.
    const gtsam::Vector3 velocityMean = componentMean(gnss.velocity, true);
    std::vector<double> velocityDeviation, normalized;
    velocityDeviations(gnss, velocityMean, velocityDeviation, normalized);
    const std::vector<double> rateDeviation = deviationNorms(imu.rate, rateMean);
    const std::vector<double> forceDeviation = deviationNorms(imu.force, forceMean);

    auto check = [&verdict](bool ok, const char *name) { if (!ok) verdict.rejected.push_back(name); };

    // Gate order is the order of `rejected`.
    check(coversWindow(imu, gnss, start, end, imuGap), "coverage");
    check(quantile(velocityDeviation, .95) <= kMaxVelocityDeviationP95, "velocity_variability");
    check(quantile(gnss.sigmaComponents, .95) <= kMaxVelocitySigmaP95, "uncertainty");
    check(quantile(normalized, .95) <= kMaxNormalizedVelocityP95, "normalized_velocity_variability");
    check((componentMean(gnss.velocityFirstHalf)-componentMean(gnss.velocitySecondHalf)).norm() <= kMaxVelocityDrift, "velocity_drift");
    check(rateStddev.maxCoeff() <= kMaxGyroStddev, "gyro_variability");
    check(quantile(rateDeviation, .95) <= kMaxGyroDeviationP95, "gyro_p95");
    check(quantile(rateDeviation, 1) <= kMaxGyroDeviationPeak, "gyro_peak");
    check(rateMean.norm() <= kMaxMeanRate, "mean_rate");
    check(forceStddev.maxCoeff() <= kMaxForceStddev, "force_variability");
    check(quantile(forceDeviation, .95) <= kMaxForceDeviationP95, "force_p95");
    check(quantile(forceDeviation, 1) <= kMaxForceDeviationPeak, "force_peak");
    check((componentMean(imu.forceFirstHalf)-componentMean(imu.forceSecondHalf)).norm() <= kMaxTiltDrift, "tilt_drift");
    check(forceMean.norm() > kMinGravityRatio*kGravity.z() && forceMean.norm() < kMaxGravityRatio*kGravity.z(), "plausible_gravity");

    verdict.accepted = verdict.rejected.empty();
    verdict.forceMean = forceMean;
    verdict.gyroMean = rateMean*kPi/180;
    verdict.score = forceStddev.norm();
    return verdict;
}

} // namespace FlySight::Fusion::Detail
