#include "fusion/trajectoryreconstruction.h"

#include <algorithm>

#include <gtsam/geometry/Pose3.h>
#include <gtsam/inference/Symbol.h>
#include <gtsam/navigation/ImuBias.h>

#include "fusion/imuintegration.h"

namespace FlySight::Fusion::Detail {

using gtsam::symbol_shorthand::B;
using gtsam::symbol_shorthand::V;
using gtsam::symbol_shorthand::X;

namespace {

/// The attitude at every edge of one GNSS interval, integrating the
/// bias-corrected gyro from `start`. Element i belongs to edges[i].
std::vector<gtsam::Rot3> propagateThroughInterval(const Samples &d, const std::vector<double> &e,
                                                  const gtsam::Rot3 &start, const gtsam::Vector3 &gyroBias)
{
    gtsam::Rot3 r = start;
    std::vector<gtsam::Rot3> rotations{r};
    rotations.reserve(e.size());
    for (size_t i = 1; i < e.size(); ++i) {
        r = r.compose(gtsam::Rot3::Expmap(gyroIncrement(d, e[i-1], e[i], gyroBias)));
        rotations.push_back(r);
    }
    return rotations;
}

} // namespace

DenseTrajectory reconstructTrajectory(const Samples &d, const FitResult &fit)
{
    DenseTrajectory dense;
    // B(0): the constant accelerometer bias and b0; the gyro bias of each
    // interval is the model's at the interval's first fix.
    const auto bias = fit.values.at<gtsam::imuBias::ConstantBias>(B(0));
    for (size_t k = 0; k+1 < d.gnssTime.size(); ++k) {
        const double start = d.gnssTime[k], end = d.gnssTime[k+1];
        const std::vector<double> edges = integrationEdges(d, start, end);
        const auto startPose = fit.values.at<gtsam::Pose3>(X(k)), endPose = fit.values.at<gtsam::Pose3>(X(k+1));
        const auto startVelocity = fit.values.at<gtsam::Vector3>(V(k)), endVelocity = fit.values.at<gtsam::Vector3>(V(k+1));
        const std::vector<gtsam::Rot3> rotations = propagateThroughInterval(
            d, edges, startPose.rotation(), intervalBias(d, k, bias, fit.gyroBiasSlope, fit.biasModel).gyroscope());

        // What the gyro integration misses of the next state's fitted
        // attitude, as a rotation vector applied on the left (NED side).
        const gtsam::Vector3 error = gtsam::Rot3::Logmap(endPose.rotation().compose(rotations.back().inverse()));
        dense.endpointCorrection.push_back(error.norm()*180/kPi);

        // Every original IMU sample in [start, end)
        const auto firstSample = std::lower_bound(d.imuTime.begin(), d.imuTime.end(), start);
        const auto lastSample = std::lower_bound(d.imuTime.begin(), d.imuTime.end(), end);
        for (auto it = firstSample; it != lastSample; ++it) {
            const double t = *it, fraction = (t-start)/(end-start);
            const size_t edge = std::lower_bound(edges.begin(), edges.end(), t)-edges.begin();
            const size_t sample = it-d.imuTime.begin();
            const gtsam::Rot3 adjusted = gtsam::Rot3::Expmap(error*fraction).compose(rotations[edge]);
            dense.time.push_back(t);
            dense.rotation.push_back(adjusted);
            dense.acceleration.push_back(adjusted.rotate(d.force[sample]-bias.accelerometer())+kGravity);
            // Display-only linear interpolation of optimized GNSS states; it is
            // independent of the gyro/force acceleration reconstruction above.
            dense.position.push_back((1-fraction)*startPose.translation()+fraction*endPose.translation());
            dense.velocity.push_back((1-fraction)*startVelocity+fraction*endVelocity);
        }
    }
    return dense;
}

} // namespace FlySight::Fusion::Detail
