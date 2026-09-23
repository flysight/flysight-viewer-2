#ifndef FLYSIGHT_FUSION_TEMPERATUREIMUFACTOR_H
#define FLYSIGHT_FUSION_TEMPERATUREIMUFACTOR_H

#include <gtsam/geometry/Pose3.h>
#include <gtsam/navigation/ImuBias.h>
#include <gtsam/navigation/ImuFactor.h>
#include <gtsam/nonlinear/NonlinearFactor.h>

// Internal to the fusion library: the IMU factor of the temperature-dependent
// gyro bias model. The same measurement, error and noise model as
// gtsam::ImuFactor, with the bias of the interval evaluated from the shared
// constant bias B(0) and the slope T(0): b = B(0) + [0; T(0) * dT], where dT is
// this interval's IMU temperature at its first fix minus the fitted window's
// reference temperature, both fixed at construction.

namespace FlySight::Fusion::Detail {

class TemperatureImuFactor
    : public gtsam::NoiseModelFactorN<gtsam::Pose3, gtsam::Vector3, gtsam::Pose3, gtsam::Vector3,
                                      gtsam::imuBias::ConstantBias, gtsam::Vector3> {
public:
    using Base = gtsam::NoiseModelFactorN<gtsam::Pose3, gtsam::Vector3, gtsam::Pose3, gtsam::Vector3,
                                          gtsam::imuBias::ConstantBias, gtsam::Vector3>;
    using Base::evaluateError;

    /// `pim` preintegrated at this interval's bias (the caller's job), `temperatureDelta` = T_i - T_ref, degC.
    TemperatureImuFactor(gtsam::Key pose_i, gtsam::Key vel_i, gtsam::Key pose_j, gtsam::Key vel_j,
                         gtsam::Key bias, gtsam::Key slope,
                         const gtsam::PreintegratedImuMeasurements &pim, double temperatureDelta);

    gtsam::NonlinearFactor::shared_ptr clone() const override;
    const gtsam::PreintegratedImuMeasurements &preintegratedMeasurements() const { return m_pim; }
    double temperatureDelta() const { return m_temperatureDelta; }

    /// ImuFactor's error at the interval bias [acc; gyro + slope * dT]. H1..H4
    /// are ImuFactor's; H5 is ImuFactor's bias Jacobian whole (the bias enters
    /// the interval bias with the identity), H6 its gyro columns times dT.
    gtsam::Vector evaluateError(const gtsam::Pose3 &pose_i, const gtsam::Vector3 &vel_i,
                                const gtsam::Pose3 &pose_j, const gtsam::Vector3 &vel_j,
                                const gtsam::imuBias::ConstantBias &bias, const gtsam::Vector3 &slope,
                                gtsam::OptionalMatrixType H1, gtsam::OptionalMatrixType H2,
                                gtsam::OptionalMatrixType H3, gtsam::OptionalMatrixType H4,
                                gtsam::OptionalMatrixType H5, gtsam::OptionalMatrixType H6) const override;

private:
    gtsam::PreintegratedImuMeasurements m_pim;
    double m_temperatureDelta;
};

} // namespace FlySight::Fusion::Detail

#endif // FLYSIGHT_FUSION_TEMPERATUREIMUFACTOR_H
