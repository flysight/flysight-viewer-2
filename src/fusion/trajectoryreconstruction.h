#ifndef FLYSIGHT_FUSION_TRAJECTORYRECONSTRUCTION_H
#define FLYSIGHT_FUSION_TRAJECTORYRECONSTRUCTION_H

#include <vector>

#include <gtsam/geometry/Rot3.h>
#include <gtsam/navigation/ImuBias.h>
#include <gtsam/navigation/NavState.h>

#include "fusion/factorgraphfit.h"
#include "fusion/fusionsamples.h"

// Internal to the fusion library: from the fitted GNSS-rate states to a value
// at every original IMU sample.
//
// The IMU-rate reconstruction (reconstructAtImuRate()) is the state a fit with
// a state at every integration edge would give, to within one linearization,
// with the fitted states at the fixes and the biases and scale factors held:
// each fix interval is solved on its own, in one pass over its steps. It
// takes its step model from the fit's own preintegration, at the fitted bias
// and scale, through preintegrateImu()'s observer (imuintegration.h): the
// forward states are the library's prediction, the covariance P_j and the
// transition F_j of every step (ImuStep::transition) are the library's, and
// nothing restates how a step integrates or what noise it carries.

namespace FlySight::Fusion::Detail {

/// Navigation states in a standard container (see Vectors).
using NavStates = std::vector<gtsam::NavState, Eigen::aligned_allocator<gtsam::NavState>>;

/// One fix interval of the IMU-rate reconstruction, from fix k to fix k+1
/// (docs/SENSOR_FUSION.md section 4), as reconstructInterval() computes it and
/// reconstructAtImuRate() publishes it. Edge j is the time the preintegration
/// has reached after j steps; step j runs from edge j to edge j+1. The forward
/// state at edge j is the preintegration's prediction from the fitted state at
/// fix k; the mismatch d is the fitted state at fix k+1 in the local
/// coordinates of the forward state there (attitude, position, velocity, the
/// order of the IMU factor); the corrected state at edge j is the forward state
/// with its share of d applied, and at the last edge it is the fitted state
/// itself (to rounding). The step correction c_j is the corrected velocity
/// change across step j over its length, less the mean of the readings at its
/// two edges, divided by the fitted scale and bias removed, each rotated by the
/// corrected attitude there, less gravity.
struct IntervalReconstruction {
    gtsam::imuBias::ConstantBias bias;   ///< the interval's bias: intervalBias() of the fit
    std::vector<double> edges;           ///< the integration edges, s since the epoch: fix k first, fix k+1 last
    NavStates forward;                   ///< per edge
    NavStates corrected;                 ///< per edge
    gtsam::Matrix9 endCovariance;        ///< P_n, the preintegration covariance at fix k+1: the interval's IMU factor's
    gtsam::Vector9 mismatch;             ///< d: attitude (rad), position (m), velocity (m/s)
    Vectors stepCorrection;              ///< c_j per step, NED, m/s^2
};

/// Interval k (fix k to fix k+1) of `window` under `fit`, preintegrated at the
/// interval's bias and at `fit.scale` with `tuning`, which must be the tuning
/// the fit ran with, so that the step model is the fit's. Lets what preintegrateImu() throws
/// propagate. The sharing: P_j and F_j are in the preintegration's tangent
/// coordinates (the frame of the state at fix k), and M_j, the Jacobian of
/// the retraction at the forward state with respect to that tangent, maps
/// them to the local coordinates of the forward state; with
/// lambda_n = P_n^-1 M_n^-1 d and lambda_j = F_j^T lambda_(j+1), the correction
/// at edge j is M_j P_j lambda_j. A fix interval whose preintegrated rotation
/// nears a full turn makes M_n singular; the preintegration degrades long
/// before that, far beyond what the fit handles, so it is not special-cased.
IntervalReconstruction reconstructInterval(const Samples &window, const FitResult &fit, const Tuning &tuning,
                                           size_t k);

/// The fitted window at every original IMU sample (docs/SENSOR_FUSION.md
/// section 4).
/// Everything but the four summaries aligns with `time`.
struct ImuRateTrajectory {
    std::vector<double> time;          ///< IMU samples in [first fix, last fix), s since the epoch
    std::vector<gtsam::Rot3> rotation; ///< corrected attitude, body to NED
    Vectors position, velocity;        ///< corrected state, NED, m and m/s
    Vectors acceleration;              ///< the model's acceleration (docs/SENSOR_FUSION.md section 4), NED, m/s^2
    double maxEndpointCorrectionDeg = 0; ///< largest |attitude part of d| over the intervals, deg
    double maxStepCorrection = 0;      ///< largest |c_j| over the window, m/s^2
    double maxStepCorrectionTime = 0;  ///< midpoint of that step, s since the epoch
    double maxVelocityMismatch = 0;    ///< largest |velocity part of d| over the intervals, m/s
};

/// Every fix interval of the fitted `window` reconstructed (reconstructInterval())
/// and published at the window's IMU samples in [first fix, last fix). A
/// sample lies in the interval whose [fix k, fix k+1) contains it, at the edge
/// of its own time; a sample exactly on a fix is published once, at the first
/// edge of the interval that fix starts. Attitude, position and velocity are
/// the corrected state at the sample's edge; the acceleration is
/// R (f ./ s_a - b_a) + g + (c_before + c_after) / 2, f the sample's own
/// reading, s_a the fitted accelerometer scale factors (./ divides axis by
/// axis), b_a the fitted accelerometer bias, and c_before, c_after the corrections of the
/// steps that end and start at the sample's edge in the window's sequence of
/// steps (a sample on a fix takes c_before from the last step of the interval
/// before it; a sample on the first fix has only c_after and takes it alone).
/// The summaries are the largest over the window, the first on a tie; the
/// step's time is its midpoint. `tuning` is the fit's. No checkpoint, no
/// validation: the window is the one the fit ran on.
ImuRateTrajectory reconstructAtImuRate(const Samples &window, const FitResult &fit, const Tuning &tuning);

} // namespace FlySight::Fusion::Detail

#endif // FLYSIGHT_FUSION_TRAJECTORYRECONSTRUCTION_H
