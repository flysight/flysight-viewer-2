#ifndef FLYSIGHT_FUSION_TRAJECTORYRECONSTRUCTION_H
#define FLYSIGHT_FUSION_TRAJECTORYRECONSTRUCTION_H

#include <vector>

#include <gtsam/geometry/Rot3.h>
#include <gtsam/navigation/ImuBias.h>
#include <gtsam/navigation/NavState.h>

#include "fusion/factorgraphfit.h"
#include "fusion/fitcovariance.h"
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
// nothing restates how a step integrates or what noise it carries. Since the
// pass alone holds the step chain, it also composes the fit's covariance
// (fitcovariance.h) at every sample it publishes.

namespace FlySight::Fusion::Detail {

/// Navigation states in a standard container (see Vectors).
using NavStates = std::vector<gtsam::NavState, Eigen::aligned_allocator<gtsam::NavState>>;
/// 9x9 and 9x6 matrices in a standard container.
using Matrices = std::vector<gtsam::Matrix9, Eigen::aligned_allocator<gtsam::Matrix9>>;
using Matrices96 = std::vector<gtsam::Matrix96, Eigen::aligned_allocator<gtsam::Matrix96>>;

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

/// What the composition of the accuracy at an edge needs of an interval
/// beyond its reconstruction (docs/SENSOR_FUSION.md section 4, "Accuracy"),
/// each the reconstruction's own or taken beside it by a separate call, so
/// that the reconstructed states keep their bits. Per edge j = 0..n: P_j,
/// M_j, Psi_j and Psi^b_j (the Jacobians of the forward state with respect to
/// the state at fix k, NavState tangent, and to the interval's bias, from
/// predict()) and H^s_j (the preintegration's scale Jacobian at the edge,
/// ImuStep::scaleJacobian, the last edge's from preintegrateImu()'s
/// out-parameter); per step F_j; and D_1, D_2, the Jacobians of the mismatch d
/// with respect to the forward state at the last edge and to the fitted state
/// at fix k+1.
struct IntervalSensitivity {
    Matrices covariance;          ///< P_j
    Matrices retraction;          ///< M_j
    Matrices transition;          ///< F_j, per step
    Matrices byStart;             ///< Psi_j
    Matrices96 byBias;            ///< Psi^b_j, columns accelerometer then gyro
    Matrices96 byScale;           ///< H^s_j
    gtsam::Matrix9 byForward;     ///< D_1
    gtsam::Matrix9 byFitted;      ///< D_2
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
/// With `sensitivity`, also fills it; the reconstruction is the same bits
/// either way.
IntervalReconstruction reconstructInterval(const Samples &window, const FitResult &fit, const Tuning &tuning,
                                           size_t k, IntervalSensitivity *sensitivity = nullptr);

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
    /// With a computed covariance only (reconstructAtImuRate()), aligned with
    /// `time`; empty otherwise. The attitude covariance in the navigation
    /// frame, R Sigma_phi R^T, and the four accuracies before the widening:
    /// heading and tilt in degrees, capped (attitudeAccuracy()), and the
    /// horizontal and vertical acceleration accuracies in m/s^2
    /// (accelerationAccuracy()).
    std::vector<gtsam::Matrix3> attitudeCovariance;
    std::vector<double> headingAcc, tiltAcc, accHAcc, accDAcc;
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
///
/// With `covariance` given and computed (fitCovariance() of this fit), every
/// published sample also gets its accuracy, composed in the same pass. For a
/// sample at edge j of interval k, with Phi_j = F_(n-1) ... F_j (Phi_n = I),
/// G_j = P_j Phi_j^T, K_j = M_j G_j P_n^-1 M_n^-1 (the correction at edge j is
/// K_j d) and C_j = M_j (P_j - G_j P_n^-1 G_j^T) M_j^T (the step chain's
/// covariance given both ends), the state's sensitivity to
/// z_k = (x_k, x_k+1, B(0), T(0), S(0)) is
/// J_j = [J^k_j N_k | J^k+1_j N_k+1 | J^b_j | J^b_j[:, gyro] dT_k | J^s_j] with
/// J^k_j = Psi_j + K_j D_1 Psi_n, J^k+1_j = K_j D_2, J^b_j = Psi^b_j + K_j D_1
/// Psi^b_n, J^s_j = M_j H^s_j + K_j D_1 M_n H^s_n, N = diag(I, I, R^T) (the
/// graph's tangent to the NavState's: V is NED, the NavState's velocity
/// tangent is the body's) and dT_k = T(fix k) - T_ref; and
/// Sigma_j = J_j Sigma_z J_j^T + C_j, Cov(x_j, g) = J_j Sigma_z,g (the law of
/// total variance: given z_k the edge depends on the interval's IMU chain
/// alone). Only the attitude rows are formed. A sample on a fix has
/// P_0 = 0, Psi_0 = I: the fix's own marginal. The accuracies are those of
/// attitudeAccuracy() at the published attitude and accelerationAccuracy() of
/// the joint of (phi, b_a, s_a), the sample's reading and published
/// acceleration and the accelerometer's per-sample sigma of `tuning`.
ImuRateTrajectory reconstructAtImuRate(const Samples &window, const FitResult &fit, const Tuning &tuning,
                                       const FitCovariance *covariance = nullptr);

} // namespace FlySight::Fusion::Detail

#endif // FLYSIGHT_FUSION_TRAJECTORYRECONSTRUCTION_H
