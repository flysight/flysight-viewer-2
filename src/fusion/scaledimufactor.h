#ifndef FLYSIGHT_FUSION_SCALEDIMUFACTOR_H
#define FLYSIGHT_FUSION_SCALEDIMUFACTOR_H

#include <gtsam/geometry/Pose3.h>
#include <gtsam/navigation/ImuBias.h>
#include <gtsam/navigation/ImuFactor.h>
#include <gtsam/nonlinear/NonlinearFactor.h>

// Internal to the fusion library: the one IMU factor of the full fit, over
// X(k-1), V(k-1), X(k), V(k), the shared bias B(0), the temperature slope T(0)
// and the scale factors S(0) (accelerometer x, y, z, then gyro x, y, z: the
// factors themselves, by which the readings are divided). The temperature
// model is inside it: the interval's gyro bias is B_g + T dT.
// The initializer's constant-bias fits use the stock gtsam::ImuFactor.
//
// The contract. The factor is built from a preintegration taken at the
// interval's bias and at the linearization scale s^, with H_s, the Jacobian
// of its preintegrated() with respect to the scale at s^ (preintegrateImu()'s
// scaleJacobian), and with dT, the interval's temperature less the window's
// reference. At the variables it evaluates GTSAM's predict() and
// computeError() with the preintegrated delta
//
//   zeta = biasCorrectedDelta([B_a; B_g + T dT]) + H_s (S - s^),
//
// that is, gtsam::ImuFactor's error at the interval bias [B_a; B_g + T dT]
// with the scale's first-order correction added to the delta. Its noise model
// is the preintegration's covariance, as ImuFactor's. The products of the
// chain are associated as predict() and computeError() associate them, so
// that at S = s^ the error and the Jacobians of the first five variables are
// ImuFactor's on the same preintegration at the interval bias, bit for bit,
// and the sixth is ImuFactor's gyro-bias columns times dT; the seventh,
// d error / d S, is the same chain applied to H_s. The correction is first order: the fit re-preintegrates
// at the fitted scale between passes, as at the fitted bias. The factor holds
// no state beyond its construction, so GTSAM may linearize it in parallel.

namespace FlySight::Fusion::Detail {

class ScaledImuFactor
    : public gtsam::NoiseModelFactorN<gtsam::Pose3, gtsam::Vector3, gtsam::Pose3, gtsam::Vector3,
                                      gtsam::imuBias::ConstantBias, gtsam::Vector3, gtsam::Vector6> {
public:
    using Base = gtsam::NoiseModelFactorN<gtsam::Pose3, gtsam::Vector3, gtsam::Pose3, gtsam::Vector3,
                                          gtsam::imuBias::ConstantBias, gtsam::Vector3, gtsam::Vector6>;
    using Base::evaluateError;

    /// `pim` preintegrated at this interval's bias and at `linearizationScale`
    /// (the caller's job), `scaleJacobian` its preintegrated()'s derivative
    /// with respect to the scale there, `temperatureDelta` = T_i - T_ref, degC.
    ScaledImuFactor(gtsam::Key pose_i, gtsam::Key vel_i, gtsam::Key pose_j, gtsam::Key vel_j,
                    gtsam::Key bias, gtsam::Key slope, gtsam::Key scale,
                    const gtsam::PreintegratedImuMeasurements &pim, double temperatureDelta,
                    const gtsam::Matrix96 &scaleJacobian, const gtsam::Vector6 &linearizationScale);

    gtsam::NonlinearFactor::shared_ptr clone() const override;
    const gtsam::PreintegratedImuMeasurements &preintegratedMeasurements() const { return m_pim; }
    double temperatureDelta() const { return m_temperatureDelta; }
    /// H_s: rows theta, position, velocity of the preintegration's tangent; columns the six factors.
    const gtsam::Matrix96 &scaleJacobian() const { return m_scaleJacobian; }
    /// s^, the scale the preintegration was taken at.
    const gtsam::Vector6 &linearizationScale() const { return m_linearizationScale; }

    /// The error of the class comment. H1..H4 follow ImuFactor's tangent
    /// conventions; H5 is the bias Jacobian whole (the bias enters the
    /// interval bias with the identity), H6 its gyro columns times dT, H7 the
    /// derivative with respect to S.
    gtsam::Vector evaluateError(const gtsam::Pose3 &pose_i, const gtsam::Vector3 &vel_i,
                                const gtsam::Pose3 &pose_j, const gtsam::Vector3 &vel_j,
                                const gtsam::imuBias::ConstantBias &bias, const gtsam::Vector3 &slope,
                                const gtsam::Vector6 &scale,
                                gtsam::OptionalMatrixType H1, gtsam::OptionalMatrixType H2,
                                gtsam::OptionalMatrixType H3, gtsam::OptionalMatrixType H4,
                                gtsam::OptionalMatrixType H5, gtsam::OptionalMatrixType H6,
                                gtsam::OptionalMatrixType H7) const override;

private:
    gtsam::PreintegratedImuMeasurements m_pim;
    double m_temperatureDelta;
    gtsam::Matrix96 m_scaleJacobian;
    gtsam::Vector6 m_linearizationScale;

public:
    GTSAM_MAKE_ALIGNED_OPERATOR_NEW
};

} // namespace FlySight::Fusion::Detail

#endif // FLYSIGHT_FUSION_SCALEDIMUFACTOR_H
