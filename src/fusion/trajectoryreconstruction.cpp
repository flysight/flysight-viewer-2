#include "fusion/trajectoryreconstruction.h"

#include <algorithm>

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

using Matrices = std::vector<gtsam::Matrix9, Eigen::aligned_allocator<gtsam::Matrix9>>;

/// The fitted state at fix `k`.
gtsam::NavState fittedState(const FitResult &fit, size_t k)
{
    return gtsam::NavState(fit.values.at<gtsam::Pose3>(X(k)), fit.values.at<gtsam::Vector3>(V(k)));
}

} // namespace

IntervalReconstruction reconstructInterval(const Samples &window, const FitResult &fit, const Tuning &tuning,
                                           size_t k)
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
    };
    const ImuStepObserver observer = [&](const gtsam::PreintegratedImuMeasurements &pim, const ImuStep &step) {
        if (r.edges.empty())
            r.edges.push_back(step.start);
        r.edges.push_back(step.end);
        atEdge(pim);
        // F_j is the A of the library's own update, bit for bit what the step
        // propagates its covariance with: integrateMeasurement() calls update()
        // qualified, so the step itself cannot be intercepted, and a copy
        // updated with the same raw readings computes the same A.
        gtsam::PreintegratedImuMeasurements copy = pim;
        gtsam::Matrix9 F;
        gtsam::Matrix93 byAcc, byGyro;
        copy.update(step.force, step.gyro, step.dt, &F, &byAcc, &byGyro);
        transition.push_back(F);
    };
    const gtsam::PreintegratedImuMeasurements pim =
        preintegrateImu(window, window.gnssTime[k], window.gnssTime[k+1], r.bias, tuning.noise, observer);
    atEdge(pim);
    const size_t n = transition.size();

    // d takes the forward state to the fitted one; the IMU factor's residual
    // is the same difference taken the other way round, which is not its
    // negation beyond first order.
    r.endCovariance = covariance[n];
    r.mismatch = r.forward[n].localCoordinates(fitted);

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

    // The readings at the edges (the sample's own at a sample), bias removed.
    const gtsam::Vector3 &accBias = r.bias.accelerometer();
    Vectors reading;
    reading.reserve(n+1);
    for (double e : r.edges)
        reading.push_back(interpolateAt(window.imuTime, window.force, e)-accBias);
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
    return r;
}

ImuRateTrajectory reconstructAtImuRate(const Samples &window, const FitResult &fit, const Tuning &tuning)
{
    ImuRateTrajectory out;
    const std::vector<double> &imuTime = window.imuTime;
    // The correction of the step that ends at the current interval's first
    // fix; none before the first.
    gtsam::Vector3 endingStep = gtsam::Vector3::Zero();
    bool anyStep = false;
    for (size_t k = 0; k+1 < window.gnssTime.size(); ++k) {
        const IntervalReconstruction r = reconstructInterval(window, fit, tuning, k);
        const gtsam::Vector3 &accBias = r.bias.accelerometer();

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
            out.acceleration.push_back(state.attitude().rotate(window.force[sample]-accBias)+kGravity+correction);
        }
        endingStep = r.stepCorrection.back();
    }
    return out;
}

} // namespace FlySight::Fusion::Detail
