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
    auto bias = fit.values.at<gtsam::imuBias::ConstantBias>(B(0));
    for (size_t k = 0; k+1 < d.gnssTime.size(); ++k) {
        double start = d.gnssTime[k], end = d.gnssTime[k+1];
        auto e = integrationEdges(d, start, end);
        auto p0 = fit.values.at<gtsam::Pose3>(X(k)), p1 = fit.values.at<gtsam::Pose3>(X(k+1));
        auto v0 = fit.values.at<gtsam::Vector3>(V(k)), v1 = fit.values.at<gtsam::Vector3>(V(k+1));
        const std::vector<gtsam::Rot3> rotations = propagateThroughInterval(d, e, p0.rotation(), bias.gyroscope());

        // What the gyro integration misses of the next state's fitted
        // attitude, as a rotation vector applied on the left (NED side).
        gtsam::Vector3 error = gtsam::Rot3::Logmap(p1.rotation().compose(rotations.back().inverse()));
        dense.endpointCorrection.push_back(error.norm()*180/kPi);

        // Every original IMU sample in [start, end)
        auto a = std::lower_bound(d.imuTime.begin(), d.imuTime.end(), start);
        auto b = std::lower_bound(d.imuTime.begin(), d.imuTime.end(), end);
        for (auto it = a; it != b; ++it) {
            double t = *it, fraction = (t-start)/(end-start);
            size_t j = std::lower_bound(e.begin(), e.end(), t)-e.begin(), index = it-d.imuTime.begin();
            gtsam::Rot3 adjusted = gtsam::Rot3::Expmap(error*fraction).compose(rotations[j]);
            dense.time.push_back(t);
            dense.rotation.push_back(adjusted);
            dense.acceleration.push_back(adjusted.rotate(d.force[index]-bias.accelerometer())+kGravity);
            // Display-only linear interpolation of optimized GNSS states; it is
            // independent of the gyro/force acceleration reconstruction above.
            dense.position.push_back((1-fraction)*p0.translation()+fraction*p1.translation());
            dense.velocity.push_back((1-fraction)*v0+fraction*v1);
        }
    }
    return dense;
}

} // namespace FlySight::Fusion::Detail
