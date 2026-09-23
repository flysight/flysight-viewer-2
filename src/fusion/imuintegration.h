#ifndef FLYSIGHT_FUSION_IMUINTEGRATION_H
#define FLYSIGHT_FUSION_IMUINTEGRATION_H

#include <memory>
#include <vector>

#include <gtsam/geometry/Rot3.h>
#include <gtsam/navigation/ImuFactor.h>

#include "fusion/fusionsamples.h"

// Internal to the fusion library: integration of IMU samples between exact
// boundary times. IMU samples are treated as a piecewise-linear signal, and
// every integration step takes the signal at the midpoint of the step, so a
// boundary that falls between two samples is honoured exactly. Each step's
// measurement covariance is the density plus a white-noise term proportional
// to the change of the signal across the step (Tuning::gyroStepSlope,
// accStepSlope).

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

/// The rotation increment (rotation vector, rad) of the step `from` -> `to`:
/// the bias-corrected rate at the midpoint of the step times its length.
gtsam::Vector3 gyroIncrement(const Samples &samples, double from, double to,
                             const gtsam::Vector3 &gyroBias);

/// Preintegration settings carrying the noise densities of `tuning`.
std::shared_ptr<gtsam::PreintegrationParams> preintegrationParams(const Tuning &tuning);

/// The preintegrated IMU measurement between two times (in practice two
/// successive fixes), linearized at `bias`, with the per-step noise term of
/// `tuning`.
gtsam::PreintegratedImuMeasurements preintegrateImu(const Samples &samples, double start, double end,
                                                    const gtsam::imuBias::ConstantBias &bias,
                                                    const Tuning &tuning);

/// `rotation` (body to NED at `start`) carried to `end` by the bias-corrected
/// gyro. `end` may precede `start`; the increments are then undone in reverse
/// order. Throws when the span contains an IMU gap: an attitude cannot be
/// carried across missing data.
gtsam::Rot3 propagateAttitude(const Samples &samples, gtsam::Rot3 rotation, double start, double end,
                              const gtsam::Vector3 &gyroBias);

} // namespace FlySight::Fusion::Detail

#endif // FLYSIGHT_FUSION_IMUINTEGRATION_H
