#include "fusion/imuintegration.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace FlySight::Fusion::Detail {

namespace {

// GTSAM's integrationCovariance: the continuous-time variance of the error
// made by integrating position from velocity.
constexpr double kIntegrationVariance = 1e-8;
// The preintegrated steps must add up to the interval between the two fixes
// to within this many seconds.
constexpr double kDurationTolerance = 1e-10;

/// No step between successive `edges` may exceed `limit` seconds.
void requireNoImuGap(const std::vector<double> &edges, double limit)
{
    for (size_t i = 1; i < edges.size(); ++i) {
        if (edges[i]-edges[i-1] > limit)
            throw std::invalid_argument("Anchor propagation cannot bridge an IMU gap");
    }
}

/// One increment per step of `edges`, in time order.
Vectors gyroIncrements(const Samples &samples, const std::vector<double> &edges,
                       const gtsam::Vector3 &gyroBias)
{
    Vectors increments;
    increments.reserve(edges.size());
    for (size_t i = 1; i < edges.size(); ++i)
        increments.push_back(gyroIncrement(samples, edges[i-1], edges[i], gyroBias));
    return increments;
}

} // namespace

std::vector<double> integrationEdges(const Samples &samples, double start, double end,
                                     bool includeGnss)
{
    const std::vector<double> &imuTime = samples.imuTime;
    if (!(imuTime.front() <= start && start < end && end <= imuTime.back()))
        throw std::invalid_argument("Integration outside IMU coverage");

    std::vector<double> edges{start};
    edges.insert(edges.end(),
                 std::upper_bound(imuTime.begin(), imuTime.end(), start),
                 std::lower_bound(imuTime.begin(), imuTime.end(), end));
    if (includeGnss) {
        const std::vector<double> &gnssTime = samples.gnssTime;
        edges.insert(edges.end(),
                     std::upper_bound(gnssTime.begin(), gnssTime.end(), start),
                     std::lower_bound(gnssTime.begin(), gnssTime.end(), end));
        std::sort(edges.begin(), edges.end());
        edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    }
    edges.push_back(end);
    return edges;
}

gtsam::Vector3 interpolateAt(const std::vector<double> &times, const Vectors &values, double t)
{
    if (t < times.front() || t > times.back())
        throw std::invalid_argument("Interpolation outside coverage");
    const auto it = std::lower_bound(times.begin(), times.end(), t);
    const size_t j = size_t(it-times.begin());
    // On a sample (or at the very first one) there is nothing to interpolate.
    if (!j || *it == t)
        return values[j];
    const double f = (t-times[j-1])/(times[j]-times[j-1]);
    return values[j-1] + f*(values[j]-values[j-1]);
}

gtsam::Vector3 gyroIncrement(const Samples &samples, double from, double to,
                             const gtsam::Vector3 &gyroBias)
{
    return (interpolateAt(samples.imuTime, samples.gyro, (to+from)/2)-gyroBias)*(to-from);
}

std::shared_ptr<gtsam::PreintegrationParams> preintegrationParams(const Tuning &tuning)
{
    // "D": the z axis of the navigation frame points down, so gravity is +z.
    auto params = gtsam::PreintegrationParams::MakeSharedD(kGravity.z());
    params->accelerometerCovariance = gtsam::I_3x3*tuning.accDensity*tuning.accDensity;
    params->gyroscopeCovariance = gtsam::I_3x3*tuning.gyroDensity*tuning.gyroDensity;
    params->integrationCovariance = gtsam::I_3x3*kIntegrationVariance;
    return params;
}

gtsam::PreintegratedImuMeasurements preintegrateImu(const Samples &samples, double start, double end,
                                                    const gtsam::imuBias::ConstantBias &bias,
                                                    const Tuning &tuning)
{
    gtsam::PreintegratedImuMeasurements pim(preintegrationParams(tuning), bias);
    const std::vector<double> e = integrationEdges(samples, start, end);
    for (size_t i = 1; i < e.size(); ++i) {
        const double mid = (e[i]+e[i-1])/2;
        pim.integrateMeasurement(interpolateAt(samples.imuTime, samples.force, mid),
                                 interpolateAt(samples.imuTime, samples.gyro, mid), e[i]-e[i-1]);
    }
    // The steps must add up to the interval, or the factor would relate the
    // two states over the wrong duration.
    if (std::abs(pim.deltaTij()-(end-start)) > kDurationTolerance)
        throw std::runtime_error("Preintegration duration mismatch");
    return pim;
}

gtsam::Rot3 propagateAttitude(const Samples &samples, gtsam::Rot3 rotation, double start, double end,
                              const gtsam::Vector3 &gyroBias)
{
    if (start == end)
        return rotation;
    const double lo = std::min(start, end), hi = std::max(start, end);

    // The gap test looks at IMU samples only; the increments also break at
    // GNSS times, as every other integration in the model does.
    const std::vector<double> imuEdges = integrationEdges(samples, lo, hi);
    const double gap = kImuGapMedians*medianInterval(samples.imuTime);
    requireNoImuGap(imuEdges, gap);
    const Vectors increments = gyroIncrements(samples, integrationEdges(samples, lo, hi, true), gyroBias);

    if (end < start) {
        // Backwards: undo each step, last first. Rotations do not commute, so
        // this is not the inverse of the summed increments.
        for (auto it = increments.rbegin(); it != increments.rend(); ++it)
            rotation = rotation.compose(gtsam::Rot3::Expmap(-*it));
    } else {
        for (const auto &increment : increments)
            rotation = rotation.compose(gtsam::Rot3::Expmap(increment));
    }
    return rotation;
}

} // namespace FlySight::Fusion::Detail
