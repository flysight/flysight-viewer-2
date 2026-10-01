#ifndef FLYSIGHT_FUSION_FITCOVARIANCE_H
#define FLYSIGHT_FUSION_FITCOVARIANCE_H

#include <string>
#include <vector>

#include <gtsam/geometry/Rot3.h>

#include "fusion/factorgraphfit.h"

// Internal to the fusion library: the accuracy of a converged fit
// (docs/SENSOR_FUSION.md section 4, "Accuracy").
//
// The covariance step (fitCovariance()) computes, after convergence and from
// the converged graph alone, the covariance of every fix state, of every pair
// of adjacent fix states, and their cross-covariance with the globals (the
// biases B(0), the slope T(0) and the scale factors S(0)): one factorization,
// then the clique marginals of its Bayes tree. It is not a cancellation
// boundary and changes nothing of the fit. The reconstruction
// (trajectoryreconstruction.h) composes it at every IMU sample, since only its
// pass holds the step chain; this unit owns the formulas that turn the
// composed covariance into the four published accuracies, and the widening
// that the fit's residuals impose on them.

namespace FlySight::Fusion::Detail {

using Matrix9x15 = Eigen::Matrix<double, 9, 15>;
using Matrix15 = Eigen::Matrix<double, 15, 15>;
using Matrix33 = Eigen::Matrix<double, 33, 33>;

/// The sigma, rad, of the prior on the first fix's heading that the covariance
/// step adds to its linearized graph, and only there: it keeps a heading the
/// data do not determine finite (180 when published) and moves a determined
/// one by sigma^2 / (2 x 1000^2) of itself, under 5e-6 at the cap.
constexpr double kHeadingPriorSigmaRad = 1000;

/// The half-width, s, of the widening window: the fixes within this of a
/// sample (and at least the two around it).
constexpr double kWideningHalfWidthS = 2.5;

/// The covariance step's failure, the diagnostics' `accuracy.failure`.
extern const char kCovarianceFailure[];

/// The covariance of a converged fit, in the graph's tangent: a fix state x_k
/// is the Pose3 tangent (rotation, translation, both in the body frame), then
/// V(k) in the navigation frame, 9 numbers; the globals g are B(0) (6:
/// accelerometer, gyro), T(0) (3) and S(0) (6: accelerometer x, y, z, gyro),
/// 15 numbers.
struct FitCovariance {
    bool computed = false;      ///< false: every block is empty and `failure` says why
    std::string failure;        ///< kCovarianceFailure when not computed; empty otherwise
    std::vector<gtsam::Matrix9, Eigen::aligned_allocator<gtsam::Matrix9>> node;   ///< Sigma_k, per fix
    std::vector<gtsam::Matrix9, Eigen::aligned_allocator<gtsam::Matrix9>> next;   ///< Sigma_(k,k+1) = Cov(x_k, x_k+1), per fix but the last
    std::vector<Matrix9x15, Eigen::aligned_allocator<Matrix9x15>> global;          ///< Sigma_(k,g) = Cov(x_k, g), per fix
    Matrix15 globals = Matrix15::Zero();                                           ///< Sigma_gg

    /// The 33x33 joint covariance of z_k = (x_k, x_k+1, B(0), T(0), S(0)), for
    /// k < the number of fixes - 1. Requires `computed`.
    Matrix33 pair(size_t k) const;
};

/// The covariance step on the converged `fit` of `states` fixes, which must
/// carry T(0) and S(0) (the full fit). The graph is linearized at the fitted
/// values, without damping; a prior of kHeadingPriorSigmaRad on X(0)'s
/// heading, (e_D^T R_0, 0) / sigma on its tangent, is added to that linear
/// graph only; it is eliminated once by multifrontal QR in the chain ordering
/// X(0), V(0), ..., X(N-1), V(N-1), B(0), T(0), S(0), whose Bayes tree is a
/// path of cliques (x_k | x_k+1, g) under a root (x_N-1, g); the cliques are
/// visited root first, so that each clique's marginal is taken from its
/// parent's cached separator marginal, and each clique's joint covariance is
/// R^-1 R^-T of the QR of that marginal. The tree is then discarded. Not the
/// library's joint marginals, which factorize again for every query.
///
/// Fails (computed false, failure kCovarianceFailure) when linearization or
/// elimination throws a std::exception, when a block is not finite, or when a
/// diagonal entry of a fix's or the globals' covariance is not positive; it
/// never throws then. std::bad_alloc propagates, as run() promises. An
/// undetermined heading is not a failure: the prior keeps it finite.
FitCovariance fitCovariance(const FitResult &fit, size_t states);

/// The attitude accuracy of one sample, degrees, each capped at
/// kYawSigmaCapDeg: heading, the square root of the navigation-frame rotation
/// covariance's element about the vertical, and tilt, the square root of the
/// sum of its two horizontal elements. `navigation` is that covariance,
/// R Sigma_phi R^T for an attitude R perturbed on the right (R Exp(phi)). A
/// variance that is not finite, or is negative, reads as the cap.
struct AttitudeAccuracy { double heading = 0, tilt = 0; };
AttitudeAccuracy attitudeAccuracy(const gtsam::Matrix3 &navigation);

/// The acceleration accuracy of one sample, m/s^2: the first-order
/// propagation of the joint covariance of (phi^n, b_a, s_a) through
/// a = R (f ./ s_a - b_a) + g, plus the accelerometer's per-sample noise,
/// Sigma_a = L Sigma_q L^T + sigma^2 I with
/// L = [-[R u]x | -R | -R diag(f_i / s_i^2)], u = f ./ s_a - b_a.
/// `joint` is the 9x9 covariance of (phi, b_a, s_a) with phi in the body
/// frame (the graph's tangent); it is turned into the navigation frame, and
/// where its heading variance exceeds pi^2 its heading row and column are
/// scaled down to that: an undetermined heading enters at most at the cap.
/// `horizontal` is the standard deviation along the direction of the
/// published horizontal acceleration `published` (N, E) when its magnitude is
/// at least that standard deviation, and otherwise (a zero one included) the
/// square root of the larger eigenvalue of the horizontal block; `vertical`
/// is the square root of the D element.
struct AccelerationAccuracy { double horizontal = 0, vertical = 0; };
AccelerationAccuracy accelerationAccuracy(const gtsam::Matrix9 &joint, const gtsam::Rot3 &attitude,
                                          const gtsam::Vector3 &reading, const gtsam::Vector3 &accScale,
                                          const gtsam::Vector3 &accBias, const gtsam::Vector3 &published,
                                          double sampleSigma);

/// The a-posteriori variance factor of the window of each time of `times` (s
/// since the epoch, each in [first fix, last fix)), from the fit's
/// `residuals` (collectResiduals()'s, an IMU residual's node the later fix's)
/// over the fixes `gnssTime`. The window is the fixes within
/// kWideningHalfWidthS of the time, extended to the two fixes around it: a
/// contiguous lo..hi of N >= 2 fixes. The factor is
/// (sum over k = lo..hi of the position and velocity residuals + sum over
/// k = lo+1..hi of the IMU residuals) / (6N - 9): the window's squared
/// whitened residuals over its redundancy. Prefix sums: one pass over the
/// residuals, one search per time.
std::vector<double> wideningFactors(const std::vector<double> &gnssTime,
                                    const std::vector<FactorResidual> &residuals,
                                    const std::vector<double> &times);

/// The widening of a factor: its square root where it exceeds one, one
/// otherwise. Never below one.
double widening(double factor);

} // namespace FlySight::Fusion::Detail

#endif // FLYSIGHT_FUSION_FITCOVARIANCE_H
