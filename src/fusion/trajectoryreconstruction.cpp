#include "fusion/trajectoryreconstruction.h"

#include <algorithm>
#include <optional>
#include <utility>

#include <Eigen/Cholesky>
#include <Eigen/LU>

#include <gtsam/geometry/Pose3.h>
#include <gtsam/inference/Symbol.h>
#include <gtsam/navigation/ImuBias.h>

#include "fusion/imuintegration.h"

namespace FlySight::Fusion::Detail {

using gtsam::symbol_shorthand::B;
using gtsam::symbol_shorthand::V;
using gtsam::symbol_shorthand::X;

namespace {

/// The fitted state at fix `k`.
gtsam::NavState fittedState(const FitResult &fit, size_t k)
{
    return gtsam::NavState(fit.values.at<gtsam::Pose3>(X(k)), fit.values.at<gtsam::Vector3>(V(k)));
}

/// The graph's tangent of a fix state (Pose3: rotation, translation; V in
/// NED) into the NavState's (rotation, position, velocity in the body), to
/// first order: N = diag(I, I, R^T).
gtsam::Matrix9 graphToNavState(const gtsam::Rot3 &attitude)
{
    gtsam::Matrix9 N = gtsam::Matrix9::Identity();
    N.block<3, 3>(6, 6) = attitude.matrix().transpose();
    return N;
}

/// The accuracy composition of one fix interval (reconstructAtImuRate()'s
/// contract): what every edge of the interval shares, formed once, and the
/// attitude rows of J_j at an edge.
class IntervalComposition {
public:
    IntervalComposition(const IntervalSensitivity &s, const FitCovariance &covariance, size_t k,
                        const gtsam::Rot3 &startAttitude, const gtsam::Rot3 &endAttitude, double temperatureDelta)
        : m_s(s), m_z(covariance.pair(k)), m_globals(covariance.globals), m_dT(temperatureDelta),
          m_startToNav(graphToNavState(startAttitude)), m_endToNav(graphToNavState(endAttitude))
    {
        const size_t n = s.transition.size();
        // Phi_j = F_(n-1) ... F_j, backwards from Phi_n = I.
        m_chain.resize(n+1);
        m_chain[n] = gtsam::Matrix9::Identity();
        for (size_t j = n; j-- > 0;)
            m_chain[j] = m_chain[j+1]*s.transition[j];
        m_endInverse = s.covariance[n].ldlt().solve(gtsam::Matrix9::Identity());
        m_share = m_endInverse*s.retraction[n].partialPivLu().inverse();
        m_endByStart = s.byForward*s.byStart[n];
        m_endByBias = s.byForward*s.byBias[n];
        m_endByScale = s.byForward*s.retraction[n]*s.byScale[n];
    }

    /// The joint covariance of (phi, b_a, s_a) at edge j: phi the attitude
    /// error in the body frame (the forward state's tangent), b_a and s_a the
    /// accelerometer's parts of B(0) and S(0).
    gtsam::Matrix9 attitudeBiasScale(size_t j) const
    {
        const gtsam::Matrix9 &P = m_s.covariance[j], &M = m_s.retraction[j];
        const gtsam::Matrix9 G = P*m_chain[j].transpose();
        const Eigen::Matrix<double, 3, 9> attitudeRetraction = M.topRows<3>();
        const Eigen::Matrix<double, 3, 9> K = attitudeRetraction*G*m_share;
        const gtsam::Matrix3 conditional = attitudeRetraction*(P-G*m_endInverse*G.transpose())
                                           *attitudeRetraction.transpose();
        Eigen::Matrix<double, 3, 33> J;
        J.block<3, 9>(0, 0) = (m_s.byStart[j].topRows<3>()+K*m_endByStart)*m_startToNav;
        J.block<3, 9>(0, 9) = K*m_s.byFitted*m_endToNav;
        const Eigen::Matrix<double, 3, 6> byBias = m_s.byBias[j].topRows<3>()+K*m_endByBias;
        // g follows the two fix states in z_k.
        J.block<3, 6>(0, 18+kBiasOffset) = byBias;
        J.block<3, 3>(0, 18+kSlopeOffset) = byBias.rightCols<3>()*m_dT;
        J.block<3, 6>(0, 18+kScaleOffset) = attitudeRetraction*m_s.byScale[j]+K*m_endByScale;

        gtsam::Matrix3 attitude = J*m_z*J.transpose()+conditional;
        attitude = (attitude+attitude.transpose())/2;
        const Eigen::Matrix<double, 3, 15> withGlobals = J*m_z.rightCols<15>();
        // The accelerometer's parts of g: the first three of B(0) and of S(0).
        gtsam::Matrix9 joint;
        joint.block<3, 3>(0, 0) = attitude;
        joint.block<3, 3>(0, 3) = withGlobals.block<3, 3>(0, kBiasOffset);
        joint.block<3, 3>(0, 6) = withGlobals.block<3, 3>(0, kScaleOffset);
        joint.block<3, 3>(3, 0) = joint.block<3, 3>(0, 3).transpose();
        joint.block<3, 3>(6, 0) = joint.block<3, 3>(0, 6).transpose();
        joint.block<3, 3>(3, 3) = m_globals.block<3, 3>(kBiasOffset, kBiasOffset);
        joint.block<3, 3>(3, 6) = m_globals.block<3, 3>(kBiasOffset, kScaleOffset);
        joint.block<3, 3>(6, 3) = m_globals.block<3, 3>(kScaleOffset, kBiasOffset);
        joint.block<3, 3>(6, 6) = m_globals.block<3, 3>(kScaleOffset, kScaleOffset);
        return joint;
    }

private:
    const IntervalSensitivity &m_s;
    Matrix33 m_z;                       ///< Sigma_z, the joint of (x_k, x_k+1, B, T, S)
    Matrix15 m_globals;
    double m_dT;
    gtsam::Matrix9 m_startToNav, m_endToNav;
    Matrices m_chain;                   ///< Phi_j
    gtsam::Matrix9 m_endInverse;        ///< P_n^-1
    gtsam::Matrix9 m_share;             ///< P_n^-1 M_n^-1
    gtsam::Matrix9 m_endByStart;        ///< D_1 Psi_n
    gtsam::Matrix96 m_endByBias;        ///< D_1 Psi^b_n
    gtsam::Matrix96 m_endByScale;       ///< D_1 M_n H^s_n
};

} // namespace

IntervalReconstruction reconstructInterval(const Samples &window, const FitResult &fit, const Tuning &tuning,
                                           size_t k, IntervalSensitivity *sensitivity)
{
    IntervalReconstruction r;
    r.bias = intervalBias(window, k, fit.values.at<gtsam::imuBias::ConstantBias>(B(0)), fit.gyroBiasSlope,
                          fit.biasModel);
    const gtsam::NavState start = fittedState(fit, k), fitted = fittedState(fit, k+1);

    // Per edge the covariance P_j and the retraction's Jacobian M_j; per step
    // the transition F_j.
    Matrices covariance, retraction, transition;
    // The forward state is the factor's own prediction. M_j is the Jacobian of
    // the retraction predict() makes, taken from the same retraction of the
    // same tangent (at the preintegration's own bias the bias-corrected delta
    // predict() starts from is preintegrated() itself); the state is still
    // predict()'s, since a retraction that also computes a Jacobian is not
    // promised to give the same bits.
    const auto atEdge = [&](const gtsam::PreintegratedImuMeasurements &pim) {
        r.forward.push_back(pim.predict(start, r.bias));
        covariance.push_back(pim.preintMeasCov());
        const gtsam::PreintegrationParams &params = pim.p();
        gtsam::Matrix9 M;
        start.retract(start.correctPIM(pim.preintegrated(), pim.deltaTij(), params.n_gravity,
                                       params.omegaCoriolis, params.use2ndOrderCoriolis), {}, M);
        retraction.push_back(M);
        // The forward state's Jacobians, from a second predict(): the state
        // above keeps predict()'s own bits.
        if (sensitivity) {
            gtsam::Matrix9 byStart;
            gtsam::Matrix96 byBias;
            pim.predict(start, r.bias, byStart, byBias);
            sensitivity->byStart.push_back(byStart);
            sensitivity->byBias.push_back(byBias);
        }
    };
    const ImuStepObserver observer = [&](const gtsam::PreintegratedImuMeasurements &pim, const ImuStep &step) {
        if (r.edges.empty())
            r.edges.push_back(step.start);
        r.edges.push_back(step.end);
        atEdge(pim);
        // F_j is the A of the library's own update, bit for bit what the step
        // propagates its covariance with, taken once by the integration.
        transition.push_back(step.transition);
        if (sensitivity)
            sensitivity->byScale.push_back(step.scaleJacobian);
    };
    // At the fitted scale, as the fit's reported graph is preintegrated: the
    // forward states are predict() at the preintegration's own linearization.
    // Asking for the scale Jacobian changes no bit of the preintegration.
    gtsam::Matrix96 endByScale;
    const gtsam::PreintegratedImuMeasurements pim =
        preintegrateImu(window, window.gnssTime[k], window.gnssTime[k+1], r.bias, fit.scale, tuning.noise, observer,
                        sensitivity ? &endByScale : nullptr);
    atEdge(pim);
    const size_t n = transition.size();

    // d takes the forward state to the fitted one; the IMU factor's residual
    // is the same difference taken the other way round, which is not its
    // negation beyond first order.
    r.endCovariance = covariance[n];
    r.mismatch = r.forward[n].localCoordinates(fitted);
    if (sensitivity) {
        sensitivity->byScale.push_back(endByScale);
        // D_1 and D_2 from a second call, so that d keeps its bits.
        r.forward[n].localCoordinates(fitted, sensitivity->byForward, sensitivity->byFitted);
    }

    // The conditional mean of the step chain given both ends, linearized about
    // the forward states, without forming any step's noise: backwards from
    // lambda_n = P_n^-1 M_n^-1 d. The covariance lives in the preintegration's
    // tangent, the frame of the state at fix k; applied unmapped in the local
    // coordinates of each forward state it would be wrong at first order
    // under rotation.
    gtsam::Vector9 lambda = r.endCovariance.ldlt().solve(retraction[n].partialPivLu().solve(r.mismatch));
    r.corrected.resize(n+1);
    // The end from d itself, so that it is the fitted state whatever the
    // rounding of the solve.
    r.corrected[n] = r.forward[n].retract(r.mismatch);
    for (size_t j = n; j-- > 0;) {
        lambda = transition[j].transpose()*lambda;
        r.corrected[j] = r.forward[j].retract(retraction[j]*(covariance[j]*lambda));
    }

    // The readings at the edges (the sample's own at a sample), divided by
    // the scale and bias removed: what the integration integrates.
    const gtsam::Vector3 &accBias = r.bias.accelerometer();
    const gtsam::Vector3 accScale = fit.scale.head<3>();
    Vectors reading;
    reading.reserve(n+1);
    for (double e : r.edges)
        reading.push_back(interpolateAt(window.imuTime, window.force, e).cwiseQuotient(accScale)-accBias);
    // The trapezoid of the edge-rotated readings: with it the published
    // acceleration integrates to the published velocity change over the
    // window. The start-of-step rotation the integration applies would leave
    // the rotation lag in every correction.
    r.stepCorrection.reserve(n);
    for (size_t j = 0; j < n; ++j) {
        const gtsam::NavState &from = r.corrected[j], &to = r.corrected[j+1];
        r.stepCorrection.push_back((to.velocity()-from.velocity())/(r.edges[j+1]-r.edges[j])
                                   - .5*(from.attitude().rotate(reading[j])+to.attitude().rotate(reading[j+1]))
                                   - kGravity);
    }
    if (sensitivity) {
        sensitivity->covariance = std::move(covariance);
        sensitivity->retraction = std::move(retraction);
        sensitivity->transition = std::move(transition);
    }
    return r;
}

ImuRateTrajectory reconstructAtImuRate(const Samples &window, const FitResult &fit, const Tuning &tuning,
                                       const FitCovariance *covariance)
{
    ImuRateTrajectory out;
    const std::vector<double> &imuTime = window.imuTime;
    const bool accuracy = covariance && covariance->computed;
    // The correction of the step that ends at the current interval's first
    // fix; none before the first.
    gtsam::Vector3 endingStep = gtsam::Vector3::Zero();
    bool anyStep = false;
    for (size_t k = 0; k+1 < window.gnssTime.size(); ++k) {
        IntervalSensitivity sensitivity;
        const IntervalReconstruction r = reconstructInterval(window, fit, tuning, k,
                                                             accuracy ? &sensitivity : nullptr);
        const gtsam::Vector3 &accBias = r.bias.accelerometer();
        const gtsam::Vector3 accScale = fit.scale.head<3>();
        std::optional<IntervalComposition> composition;
        if (accuracy) {
            composition.emplace(sensitivity, *covariance, k, fittedState(fit, k).attitude(),
                                fittedState(fit, k+1).attitude(),
                                temperatureAtFix(window, k)-fit.biasModel.tRef);
        }

        out.maxEndpointCorrectionDeg = std::max(out.maxEndpointCorrectionDeg, r.mismatch.head<3>().norm()*180/kPi);
        out.maxVelocityMismatch = std::max(out.maxVelocityMismatch, r.mismatch.tail<3>().norm());
        for (size_t j = 0; j < r.stepCorrection.size(); ++j) {
            const double size = r.stepCorrection[j].norm();
            if (!anyStep || size > out.maxStepCorrection) {
                out.maxStepCorrection = size;
                out.maxStepCorrectionTime = (r.edges[j]+r.edges[j+1])/2;
                anyStep = true;
            }
        }

        // Every IMU sample in [fix k, fix k+1), each an edge of this interval.
        const auto first = std::lower_bound(imuTime.begin(), imuTime.end(), r.edges.front());
        const auto last = std::lower_bound(imuTime.begin(), imuTime.end(), r.edges.back());
        for (auto it = first; it != last; ++it) {
            const size_t j = size_t(std::lower_bound(r.edges.begin(), r.edges.end(), *it)-r.edges.begin());
            const size_t sample = size_t(it-imuTime.begin());
            const gtsam::NavState &state = r.corrected[j];
            // The steps on either side of the edge in the window's sequence of
            // steps; the last fix is never published, so a step starts at
            // every published edge.
            const gtsam::Vector3 &after = r.stepCorrection[j];
            const gtsam::Vector3 correction = j ? gtsam::Vector3((r.stepCorrection[j-1]+after)/2)
                                            : k ? gtsam::Vector3((endingStep+after)/2)
                                                : after;
            out.time.push_back(*it);
            out.rotation.push_back(state.attitude());
            out.position.push_back(state.position());
            out.velocity.push_back(state.velocity());
            out.acceleration.push_back(state.attitude().rotate(window.force[sample].cwiseQuotient(accScale)-accBias)
                                       +kGravity+correction);
            if (composition) {
                const gtsam::Matrix9 joint = composition->attitudeBiasScale(j);
                const gtsam::Matrix3 R = state.attitude().matrix();
                out.attitudeCovariance.push_back(R*joint.block<3, 3>(0, 0)*R.transpose());
                const AttitudeAccuracy attitude = attitudeAccuracy(out.attitudeCovariance.back());
                const AccelerationAccuracy acceleration = accelerationAccuracy(
                    joint, state.attitude(), window.force[sample], accScale, accBias, out.acceleration.back(),
                    tuning.noise.accelerometer.sampleSigma);
                out.headingAcc.push_back(attitude.heading);
                out.tiltAcc.push_back(attitude.tilt);
                out.accHAcc.push_back(acceleration.horizontal);
                out.accDAcc.push_back(acceleration.vertical);
            }
        }
        endingStep = r.stepCorrection.back();
    }
    return out;
}

} // namespace FlySight::Fusion::Detail
