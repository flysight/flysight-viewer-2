#ifndef FLYSIGHT_FUSION_TRAJECTORYRECONSTRUCTION_H
#define FLYSIGHT_FUSION_TRAJECTORYRECONSTRUCTION_H

#include <vector>

#include <gtsam/geometry/Rot3.h>

#include "fusion/factorgraphfit.h"
#include "fusion/fusionsamples.h"

// Internal to the fusion library: from the fitted GNSS-rate states to a value
// at every original IMU sample.

namespace FlySight::Fusion::Detail {

/// The fit at IMU rate. Everything but endpointCorrection aligns with `time`.
struct DenseTrajectory {
    std::vector<double> time;                  ///< original IMU times inside the fitted interval, s since the epoch
    std::vector<double> endpointCorrection;    ///< per GNSS interval, deg: see reconstructTrajectory()
    std::vector<gtsam::Rot3> rotation;         ///< body to NED
    Vectors acceleration;                      ///< inertial acceleration, NED, m/s^2
    Vectors position, velocity;                ///< display-only interpolation of the fitted states
};

/// Between two fitted states the attitude is the gyro integration from the
/// first state, which does not land exactly on the second state's fitted
/// attitude. The mismatch (endpointCorrection, a small angle) is distributed
/// linearly in time over the interval, so the dense attitude is continuous and
/// agrees with the fit at every fix. Acceleration is the bias-corrected force
/// rotated by that attitude, plus gravity. Samples at or after the last fix
/// are not produced.
DenseTrajectory reconstructTrajectory(const Samples &samples, const FitResult &fit);

} // namespace FlySight::Fusion::Detail

#endif // FLYSIGHT_FUSION_TRAJECTORYRECONSTRUCTION_H
