#ifndef FLYSIGHT_FUSION_IMUINTEGRATION_H
#define FLYSIGHT_FUSION_IMUINTEGRATION_H

#include <functional>
#include <memory>
#include <vector>

#include <gtsam/geometry/Rot3.h>
#include <gtsam/navigation/ImuFactor.h>

#include "fusion/fusionsamples.h"

// Internal to the fusion library: integration of IMU samples between exact
// boundary times. IMU samples are treated as a piecewise-linear signal, and
// every integration step takes the signal at the midpoint of the step, so a
// boundary that falls between two samples is honoured exactly. The readings
// are divided, axis by axis, by the scale the integration is linearized at
// (the fit's scale factors, docs/SENSOR_FUSION.md section 4; one for every fit
// without the scale state), and everything a step computes is computed from
// the divided readings. The accelerometer reading of a step is turned by half
// the step's rotation before the library applies it, so that it acts at the attitude of the
// step's middle and not, as the library would have it, at the attitude of
// the step's start. Each step's measurement covariance is the integration
// density of the datasheet's noise (sensornoise.h) plus, in quadrature, the
// errors the integration itself makes on the step: the sampling term (the
// piecewise-linear signal against a smooth one) and the remainder of the
// mid-step scheme, both derived (docs/SENSOR_FUSION.md section 4) with no
// constant of their own; and the temperature at a fix, for the gyro bias
// model.

namespace FlySight::Fusion::Detail {

/// The step boundaries for integrating from `start` to `end`: start, every
/// IMU time strictly between them, end. With `includeGnss`, GNSS times
/// strictly between them are boundaries too (sorted, duplicates removed).
/// Throws unless imuTime.front() <= start < end <= imuTime.back().
std::vector<double> integrationEdges(const Samples &samples, double start, double end,
                                     bool includeGnss = false);

/// `values` (sampled at `times`) linearly interpolated at `t`. Throws when `t`
/// lies outside `times`.
gtsam::Vector3 interpolateAt(const std::vector<double> &times, const Vectors &values, double t);

/// The scalar series `values` (sampled at `times`) linearly interpolated at `t`; the same rule as the vector form.
double interpolateAt(const std::vector<double> &times, const std::vector<double> &values, double t);

/// The IMU temperature interpolated at fix `k` of `samples`, degC. Requires
/// a non-empty temperature series.
double temperatureAtFix(const Samples &samples, size_t k);

/// The rotation increment (rotation vector, rad) of the step `from` -> `to`:
/// the bias-corrected rate at the midpoint of the step times its length.
gtsam::Vector3 gyroIncrement(const Samples &samples, double from, double to,
                             const gtsam::Vector3 &gyroBias);

/// Preintegration settings carrying the integration densities of `noise`.
/// Throws std::invalid_argument("Invalid fusion configuration") when a density
/// is not finite and positive (a noise that was never derived).
std::shared_ptr<gtsam::PreintegrationParams> preintegrationParams(const ImuNoise &noise);

/// One step of preintegrateImu(): what that step integrates.
struct ImuStep {
    double start, end;          ///< the step's two integration edges, s
    double dt;                  ///< end - start, the length passed to the integration
    gtsam::Vector3 force, gyro; ///< the readings passed to the integration, divided by the scale and not bias-corrected: the midpoint rate, and the midpoint reading turned by half the step's bias-corrected rotation
    gtsam::Matrix9 transition;  ///< A, the library's transition of the preintegration across this step (its update() on a copy, the same bits the step propagates its covariance with)
    /// H at the step's first edge: the scale Jacobian of the steps before this
    /// one (zero for the first), when preintegrateImu() was asked for the
    /// scale Jacobian; zero otherwise.
    gtsam::Matrix96 scaleJacobian;
};

/// Sees each step of preintegrateImu() before it is integrated, with the
/// preintegration of the steps before it.
using ImuStepObserver = std::function<void(const gtsam::PreintegratedImuMeasurements &, const ImuStep &)>;

/// The preintegrated IMU measurement between two times (in practice two
/// successive fixes), linearized at `bias` and at `scale`, with the step model
/// of `noise`.
///
/// `scale` holds the accelerometer's factors x, y, z, then the gyro's: every
/// reading enters divided by its axis's factor (f~ = f ./ s_a, w~ = w ./ s_g),
/// and below "force" and "rate" are the divided readings. At unit scale every
/// division is exact, so the result is the bits of the undivided readings.
///
/// Step [a, b], dt = b - a, lies inside one sample interval [t_k, t_k+1] of
/// `samples` (the edges include every IMU time), whose second derivative is
/// estimated as c_k, the larger norm of the changes of slope at its two ends
/// (D_i = 2 (s_i - s_i-1) / (h_i-1 + h_i) at sample i, where it exists; 0
/// when neither does). With w = 1/2 integral_a^b (t - t_k)(t_k+1 - t) dt
/// (h^3 / 12 for a whole interval), the sampling terms are s_v = w c_k(force)
/// and s_theta = w c_k(rate). With theta = (rate_mid - b_g) dt, dtheta =
/// (rate(b) - rate(a)) dt, fbar = force_mid - b_a and df = force(b) -
/// force(a), the remainders of the mid-step scheme to second order are
/// r_v = |(dt/24) theta x (theta x fbar) + (dt/12)(theta x df - dtheta x fbar)|
/// and r_theta = |theta x dtheta| / 12. The step's sensor covariances are
/// (D_a^2 + (s_v^2 + r_v^2) / dt) I and (D_g^2 + (s_theta^2 + r_theta^2) / dt) I,
/// D the integration densities (the datasheet's, which are not divided): a
/// step with a constant signal and no rotation has the densities'
/// covariance exactly.
///
/// `scaleJacobian`, when given, receives H, the derivative of the returned
/// preintegrated() with respect to the scale at `scale` (rows theta, position,
/// velocity of the tangent preintegration; columns the six factors), from the
/// library's own per-step Jacobians: with A, B, C its update()'s transition and
/// input Jacobians of the step, R = Exp(phi) the half-step turn, phi =
/// (rate_mid - b_g) dt / 2, u = force_mid - b_a, F = diag(force_mid ./ s_a)
/// and W = diag(rate_mid ./ s_g), the step's input is
/// G = [-B R F | B R [u]x Jr(phi) W dt/2 - C W] and H_(j+1) = A H_j + G from
/// H_0 = 0. The middle term is the half-step turn's dependence on the gyro
/// scale. With the Jacobian asked for, each step the observer sees carries
/// H_j, the Jacobian at its first edge (ImuStep::scaleJacobian): what the
/// reconstruction composes the accuracy at an edge with.
///
/// `observer`, when given, is called once per step, in time order, before
/// that step's covariance is written into the shared params and before the
/// step is integrated: with the preintegration after the steps before it (so
/// its preintMeasCov(), preintegrated() and deltaTij() are those of the step's
/// first edge) and with the step, its transition included. A, B and C are
/// taken (update() on a copy) only when an observer or the Jacobian is asked
/// for; neither changes a bit of the result. The returned preintegration is that of the
/// last edge. The observer receives both by const reference, but GTSAM's p()
/// and params() still hand out the shared params mutably, so the observer
/// must only read them. The step's two sensor covariances are written after
/// the observer returns, so they are always this function's own; with no
/// observer, or with one that only reads, the result is the same bits. So at
/// the observer's call for step j the shared params hold step j - 1's sensor
/// covariances (the densities' for the first step), and after the return the
/// last step's: what a test reads the step model through.
/// It exists so that the reconstruction (trajectoryreconstruction.h) reads
/// the transition and the covariance of every step from the fit's own
/// preintegration instead of restating the step model.
gtsam::PreintegratedImuMeasurements preintegrateImu(const Samples &samples, double start, double end,
                                                    const gtsam::imuBias::ConstantBias &bias,
                                                    const gtsam::Vector6 &scale, const ImuNoise &noise,
                                                    const ImuStepObserver &observer = ImuStepObserver(),
                                                    gtsam::Matrix96 *scaleJacobian = nullptr);

/// `rotation` (body to NED at `start`) carried to `end` by the bias-corrected
/// gyro. `end` may precede `start`; the increments are then undone in reverse
/// order. Throws when the span contains an IMU gap, an IMU interval that is a
/// hole against `maxGap` (the tuning's, which planFit() derived from the whole
/// recording's IMU axis; the callers pass it, so a sub-window is judged by the
/// recording's rule, not its own): an attitude cannot be carried across
/// missing data.
gtsam::Rot3 propagateAttitude(const Samples &samples, gtsam::Rot3 rotation, double start, double end,
                              const gtsam::Vector3 &gyroBias, double maxGap);

} // namespace FlySight::Fusion::Detail

#endif // FLYSIGHT_FUSION_IMUINTEGRATION_H
