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
// boundary that falls between two samples is honoured exactly. The
// accelerometer reading of a step is turned by half the step's rotation
// before the library applies it, so that it acts at the attitude of the
// step's middle and not, as the library would have it, at the attitude of
// the step's start. Each step's measurement covariance is the density plus a
// white-noise term proportional to the change of the signal across the step
// (Tuning::gyroStepSlope, accStepSlope); and the temperature at a fix, for
// the gyro bias model.

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

/// Preintegration settings carrying the noise densities of `tuning`.
std::shared_ptr<gtsam::PreintegrationParams> preintegrationParams(const Tuning &tuning);

/// One step of preintegrateImu(): what that step integrates.
struct ImuStep {
    double start, end;          ///< the step's two integration edges, s
    double dt;                  ///< end - start, the length passed to the integration
    gtsam::Vector3 force, gyro; ///< the readings passed to the integration, not bias-corrected: the midpoint rate, and the midpoint reading turned by half the step's bias-corrected rotation
};

/// Sees each step of preintegrateImu() before it is integrated, with the
/// preintegration of the steps before it.
using ImuStepObserver = std::function<void(const gtsam::PreintegratedImuMeasurements &, const ImuStep &)>;

/// The preintegrated IMU measurement between two times (in practice two
/// successive fixes), linearized at `bias`, with the per-step noise term of
/// `tuning`.
///
/// `observer`, when given, is called once per step, in time order, before
/// that step's covariance is written into the shared params and before the
/// step is integrated: with the preintegration after the steps before it (so
/// its preintMeasCov(), preintegrated() and deltaTij() are those of the step's
/// first edge) and with the step. The returned preintegration is that of the
/// last edge. The observer receives both by const reference, but GTSAM's p()
/// and params() still hand out the shared params mutably, so the observer
/// must only read them. The step's two sensor covariances are written after
/// the observer returns, so they are always this function's own; with no
/// observer, or with one that only reads, the result is the same bits.
/// It exists so that the reconstruction (trajectoryreconstruction.h) reads
/// the transition and the covariance of every step from the fit's own
/// preintegration instead of restating the step model.
gtsam::PreintegratedImuMeasurements preintegrateImu(const Samples &samples, double start, double end,
                                                    const gtsam::imuBias::ConstantBias &bias,
                                                    const Tuning &tuning,
                                                    const ImuStepObserver &observer = ImuStepObserver());

/// `rotation` (body to NED at `start`) carried to `end` by the bias-corrected
/// gyro. `end` may precede `start`; the increments are then undone in reverse
/// order. Throws when the span contains an IMU gap: an attitude cannot be
/// carried across missing data.
gtsam::Rot3 propagateAttitude(const Samples &samples, gtsam::Rot3 rotation, double start, double end,
                              const gtsam::Vector3 &gyroBias);

} // namespace FlySight::Fusion::Detail

#endif // FLYSIGHT_FUSION_IMUINTEGRATION_H
