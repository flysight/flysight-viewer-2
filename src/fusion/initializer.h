#ifndef FLYSIGHT_FUSION_INITIALIZER_H
#define FLYSIGHT_FUSION_INITIALIZER_H

#include <string>

#include <gtsam/geometry/Rot3.h>
#include <gtsam/nonlinear/Values.h>

#include "fusion/fusionsamples.h"

// Internal to the fusion library: where the optimizer starts. Roll and pitch
// come from gravity (a stationary window when the recording has one, else a
// coarse comparison of measured force with GNSS acceleration). Heading is
// never initialized: it starts at zero and is left to the fit.

namespace FlySight::Fusion::Detail {

/// The attitude and gyro bias the fit starts from.
struct InitialAttitude {
    gtsam::Rot3 rotation;                                  ///< body to NED at startTime
    gtsam::Vector3 gyroBias = gtsam::Vector3::Zero();      ///< rad/s
    double startTime = 0;                                  ///< the first fix of the graph
    /// Stationary method only (zero otherwise): the fix the attitude was
    /// anchored at and the window it was measured over.
    double anchorTime = 0, intervalStart = 0, intervalEnd = 0;
    std::string method = "coarse GNSS/force initialization; heading unknown";
};

/// The rotation that takes direction `from` to direction `to` along the
/// shortest arc. Identity when either vector is too short to have a direction.
gtsam::Rot3 rotationAligning(const gtsam::Vector3 &from, const gtsam::Vector3 &to);

/// The initial attitude at `graphStart`. Scans the FULL recording for the
/// quietest stationary 30 s window, including samples outside the fitted
/// window (a stationary minute before the origin fix is still the best place
/// to measure gravity), and falls back to the coarse method when none passes.
InitialAttitude initialAttitude(const Samples &fullRecording, double graphStart);

/// The optimizer's initial values for `window`: the shared bias B(0), and per
/// fix the pose X(k) (the initial attitude carried forward by the gyro, the
/// measured position) and the measured velocity V(k).
gtsam::Values initialValues(const Samples &window, double headingDeg, const InitialAttitude &attitude);

} // namespace FlySight::Fusion::Detail

#endif // FLYSIGHT_FUSION_INITIALIZER_H
