#include "fusion/temperatureimufactor.h"

namespace FlySight::Fusion::Detail {

TemperatureImuFactor::TemperatureImuFactor(gtsam::Key pose_i, gtsam::Key vel_i, gtsam::Key pose_j, gtsam::Key vel_j,
                                           gtsam::Key bias, gtsam::Key slope,
                                           const gtsam::PreintegratedImuMeasurements &pim, double temperatureDelta)
    // The noise model ImuFactor builds from the same preintegration.
    : Base(gtsam::noiseModel::Gaussian::Covariance(pim.preintMeasCov()), pose_i, vel_i, pose_j, vel_j, bias, slope),
      m_pim(pim),
      m_temperatureDelta(temperatureDelta)
{
}

gtsam::NonlinearFactor::shared_ptr TemperatureImuFactor::clone() const
{
    return std::static_pointer_cast<gtsam::NonlinearFactor>(
        gtsam::NonlinearFactor::shared_ptr(new TemperatureImuFactor(*this)));
}

gtsam::Vector TemperatureImuFactor::evaluateError(const gtsam::Pose3 &pose_i, const gtsam::Vector3 &vel_i,
                                                  const gtsam::Pose3 &pose_j, const gtsam::Vector3 &vel_j,
                                                  const gtsam::imuBias::ConstantBias &bias, const gtsam::Vector3 &slope,
                                                  gtsam::OptionalMatrixType H1, gtsam::OptionalMatrixType H2,
                                                  gtsam::OptionalMatrixType H3, gtsam::OptionalMatrixType H4,
                                                  gtsam::OptionalMatrixType H5, gtsam::OptionalMatrixType H6) const
{
    // The accelerometer bias is constant; only the gyro part follows the
    // temperature. No branch on a zero slope or a zero dT: x + 0.0 == x for
    // every finite x, so the factor reproduces ImuFactor there by arithmetic.
    const gtsam::imuBias::ConstantBias intervalBias(bias.accelerometer(), bias.gyroscope() + slope*m_temperatureDelta);

    // ImuFactor::evaluateError is exactly this call. H1..H4 pass through
    // untouched; the bias Jacobian is asked for whenever H5 or H6 is wanted.
    gtsam::Matrix96 H5full;
    const gtsam::Vector9 error = m_pim.computeErrorAndJacobians(pose_i, vel_i, pose_j, vel_j, intervalBias,
                                                                H1, H2, H3, H4, (H5 || H6) ? &H5full : nullptr);
    // ConstantBias is a vector space and the interval bias is
    // [acc; gyro] + [0; slope dT]: the chain rule is the identity for B(0)
    // and [0_3x3; dT I_3] for T(0).
    if (H5)
        *H5 = H5full;
    if (H6)
        *H6 = H5full.rightCols<3>()*m_temperatureDelta;
    return error;
}

} // namespace FlySight::Fusion::Detail
