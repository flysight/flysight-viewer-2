#include "fusion/scaledimufactor.h"

#include <gtsam/navigation/NavState.h>

namespace FlySight::Fusion::Detail {

ScaledImuFactor::ScaledImuFactor(gtsam::Key pose_i, gtsam::Key vel_i, gtsam::Key pose_j, gtsam::Key vel_j,
                                 gtsam::Key bias, gtsam::Key slope, gtsam::Key scale,
                                 const gtsam::PreintegratedImuMeasurements &pim, double temperatureDelta,
                                 const gtsam::Matrix96 &scaleJacobian, const gtsam::Vector6 &linearizationScale)
    // The noise model ImuFactor builds from the same preintegration.
    : Base(gtsam::noiseModel::Gaussian::Covariance(pim.preintMeasCov()), pose_i, vel_i, pose_j, vel_j, bias, slope,
           scale),
      m_pim(pim),
      m_temperatureDelta(temperatureDelta),
      m_scaleJacobian(scaleJacobian),
      m_linearizationScale(linearizationScale)
{
}

gtsam::NonlinearFactor::shared_ptr ScaledImuFactor::clone() const
{
    return std::static_pointer_cast<gtsam::NonlinearFactor>(
        gtsam::NonlinearFactor::shared_ptr(new ScaledImuFactor(*this)));
}

gtsam::Vector ScaledImuFactor::evaluateError(const gtsam::Pose3 &pose_i, const gtsam::Vector3 &vel_i,
                                             const gtsam::Pose3 &pose_j, const gtsam::Vector3 &vel_j,
                                             const gtsam::imuBias::ConstantBias &bias, const gtsam::Vector3 &slope,
                                             const gtsam::Vector6 &scale,
                                             gtsam::OptionalMatrixType H1, gtsam::OptionalMatrixType H2,
                                             gtsam::OptionalMatrixType H3, gtsam::OptionalMatrixType H4,
                                             gtsam::OptionalMatrixType H5, gtsam::OptionalMatrixType H6,
                                             gtsam::OptionalMatrixType H7) const
{
    // TemperatureImuFactor's interval bias. No branch on a zero slope, a zero
    // dT or S = s^: x + 0.0 == x for every finite x, so at S = s^ the delta is
    // biasCorrectedDelta() itself by arithmetic.
    const gtsam::imuBias::ConstantBias intervalBias(bias.accelerometer(), bias.gyroscope() + slope*m_temperatureDelta);
    const bool wantStateI = H1 || H2, wantBias = H5 || H6, wantDelta = wantBias || H7;
    gtsam::Matrix96 deltaByBias;
    const gtsam::Vector9 delta = m_pim.biasCorrectedDelta(intervalBias, wantBias ? &deltaByBias : nullptr)
                                 + m_scaleJacobian*(scale-m_linearizationScale);

    // PreintegrationBase::predict() with `delta` in place of the bias-corrected
    // delta, through NavState's public functions, asking for the Jacobians
    // whenever predict() would.
    const gtsam::NavState state_i(pose_i, vel_i), state_j(pose_j, vel_j);
    const gtsam::PreintegrationParams &p = m_pim.p();
    gtsam::Matrix9 xiByState, xiByDelta;
    const gtsam::Vector9 xi = state_i.correctPIM(delta, m_pim.deltaTij(), p.n_gravity, p.omegaCoriolis,
                                                 p.use2ndOrderCoriolis, wantStateI ? &xiByState : nullptr,
                                                 wantDelta ? &xiByDelta : nullptr);
    gtsam::Matrix9 predictedByState, predictedByXi;
    const gtsam::NavState predicted = state_i.retract(xi, wantStateI ? &predictedByState : nullptr,
                                                      (wantStateI || wantDelta) ? &predictedByXi : nullptr);
    // PreintegrationBase::computeError().
    gtsam::Matrix9 errorByStateJ, errorByPredicted;
    const gtsam::Vector9 error = state_j.localCoordinates(predicted, (H3 || H4) ? &errorByStateJ : nullptr,
                                                          (wantStateI || wantDelta) ? &errorByPredicted : nullptr);

    // computeErrorAndJacobians(): the velocity columns of a NavState tangent
    // are in the body frame, a velocity variable's in the navigation frame.
    if (wantStateI) {
        const gtsam::Matrix9 errorByStateI = errorByPredicted*gtsam::Matrix9(predictedByState + predictedByXi*xiByState);
        if (H1)
            *H1 = errorByStateI.leftCols<6>();
        if (H2)
            *H2 = errorByStateI.rightCols<3>()*state_i.R().transpose();
    }
    if (H3)
        *H3 = errorByStateJ.leftCols<6>();
    if (H4)
        *H4 = errorByStateJ.rightCols<3>()*state_j.R().transpose();
    if (wantDelta) {
        // Associated as predict() associates the bias Jacobian, so that H5 and
        // H6 are TemperatureImuFactor's bit for bit at S = s^.
        const gtsam::Matrix9 predictedByDelta = predictedByXi*xiByDelta;
        if (wantBias) {
            const gtsam::Matrix96 errorByBias = errorByPredicted*gtsam::Matrix96(predictedByDelta*deltaByBias);
            if (H5)
                *H5 = errorByBias;
            if (H6)
                *H6 = errorByBias.rightCols<3>()*m_temperatureDelta;
        }
        if (H7)
            *H7 = errorByPredicted*gtsam::Matrix96(predictedByDelta*m_scaleJacobian);
    }
    return error;
}

} // namespace FlySight::Fusion::Detail
