#include "fusion/imuintegration.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace FlySight::Fusion::Detail {

namespace {

// GTSAM's integrationCovariance: the continuous-time variance of the error
// made by integrating position from velocity.
constexpr double kIntegrationVariance = 1e-8;
// The preintegrated steps must add up to the interval between the two fixes
// to within this many seconds.
constexpr double kDurationTolerance = 1e-10;

/// No step between successive `edges` may exceed `limit` seconds.
void requireNoImuGap(const std::vector<double> &edges, double limit)
{
    for (size_t i = 1; i < edges.size(); ++i) {
        if (edges[i]-edges[i-1] > limit)
            throw std::invalid_argument("Anchor propagation cannot bridge an IMU gap");
    }
}

/// One increment per step of `edges`, in time order.
Vectors gyroIncrements(const Samples &samples, const std::vector<double> &edges,
                       const gtsam::Vector3 &gyroBias)
{
    Vectors increments;
    increments.reserve(edges.size());
    for (size_t i = 1; i < edges.size(); ++i)
        increments.push_back(gyroIncrement(samples, edges[i-1], edges[i], gyroBias));
    return increments;
}

/// The change of slope of `values` at sample `i` (1 <= i <= m - 2) as a
/// second derivative: 2 (s_i - s_i-1) / (h_i-1 + h_i), s the slopes of the
/// two intervals beside it. Exact for a quadratic, at any spacing.
gtsam::Vector3 slopeChange(const std::vector<double> &times, const Vectors &values, size_t i)
{
    const double before = times[i]-times[i-1], after = times[i+1]-times[i];
    const gtsam::Vector3 slopeBefore = (values[i]-values[i-1])/before, slopeAfter = (values[i+1]-values[i])/after;
    return 2*(slopeAfter-slopeBefore)/(before+after);
}

/// The second derivative of `values` divided axis by axis by `scale` on
/// sample interval k, as the bound the sampling term takes: the larger norm of
/// the changes of slope at its two ends, of those that exist; 0 when neither
/// does (fewer than three samples). The change of slope is linear in the
/// values, so dividing it is dividing the readings, and at unit scale it is
/// exact.
double curvature(const std::vector<double> &times, const Vectors &values, size_t k, const gtsam::Vector3 &scale)
{
    double c = 0;
    if (k >= 1)
        c = slopeChange(times, values, k).cwiseQuotient(scale).norm();
    if (k+2 < times.size())
        c = std::max(c, slopeChange(times, values, k+1).cwiseQuotient(scale).norm());
    return c;
}

/// The reading of `values` at `t`, divided axis by axis by `scale`: what a
/// step integrates.
gtsam::Vector3 dividedAt(const std::vector<double> &times, const Vectors &values, double t,
                         const gtsam::Vector3 &scale)
{
    return interpolateAt(times, values, t).cwiseQuotient(scale);
}

/// w = 1/2 integral_a^b (t - t_k)(t_k+1 - t) dt for the step [a, b] inside
/// the sample interval [tk, tk1]: the step's share of the trapezoid rule's
/// error per unit of second derivative, h^3 / 12 for the whole interval.
double samplingWeight(double tk, double tk1, double a, double b)
{
    const double h = tk1-tk, ua = a-tk, ub = b-tk;
    return .5*(h*(ub*ub-ua*ua)/2-(ub*ub*ub-ua*ua*ua)/3);
}

/// The step's measurement covariance: the density and the step's own error
/// (a sigma over the step, in quadrature) as a covariance per unit time, so
/// that the step's variance is density^2 dt + error^2. With the error exactly
/// zero this is exactly the density covariance of preintegrationParams().
gtsam::Matrix3 stepCovariance(double density, double errorSquared, double dt)
{
    return gtsam::I_3x3*(density*density + errorSquared/dt);
}

} // namespace

std::vector<double> integrationEdges(const Samples &samples, double start, double end,
                                     bool includeGnss)
{
    const std::vector<double> &imuTime = samples.imuTime;
    if (!(imuTime.front() <= start && start < end && end <= imuTime.back()))
        throw std::invalid_argument("Integration outside IMU coverage");

    std::vector<double> edges{start};
    edges.insert(edges.end(),
                 std::upper_bound(imuTime.begin(), imuTime.end(), start),
                 std::lower_bound(imuTime.begin(), imuTime.end(), end));
    if (includeGnss) {
        const std::vector<double> &gnssTime = samples.gnssTime;
        edges.insert(edges.end(),
                     std::upper_bound(gnssTime.begin(), gnssTime.end(), start),
                     std::lower_bound(gnssTime.begin(), gnssTime.end(), end));
        std::sort(edges.begin(), edges.end());
        edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    }
    edges.push_back(end);
    return edges;
}

gtsam::Vector3 interpolateAt(const std::vector<double> &times, const Vectors &values, double t)
{
    if (t < times.front() || t > times.back())
        throw std::invalid_argument("Interpolation outside coverage");
    const auto it = std::lower_bound(times.begin(), times.end(), t);
    const size_t j = size_t(it-times.begin());
    // On a sample (or at the very first one) there is nothing to interpolate.
    if (!j || *it == t)
        return values[j];
    const double f = (t-times[j-1])/(times[j]-times[j-1]);
    return values[j-1] + f*(values[j]-values[j-1]);
}

// The same statements as the vector form, so that a scalar series and the
// same series stored as (v, 0, 0) interpolate to the same bits.
double interpolateAt(const std::vector<double> &times, const std::vector<double> &values, double t)
{
    if (t < times.front() || t > times.back())
        throw std::invalid_argument("Interpolation outside coverage");
    const auto it = std::lower_bound(times.begin(), times.end(), t);
    const size_t j = size_t(it-times.begin());
    if (!j || *it == t)
        return values[j];
    const double f = (t-times[j-1])/(times[j]-times[j-1]);
    return values[j-1] + f*(values[j]-values[j-1]);
}

double temperatureAtFix(const Samples &samples, size_t k)
{
    return interpolateAt(samples.imuTime, samples.temperature, samples.gnssTime[k]);
}

gtsam::Vector3 gyroIncrement(const Samples &samples, double from, double to,
                             const gtsam::Vector3 &gyroBias)
{
    return (interpolateAt(samples.imuTime, samples.gyro, (to+from)/2)-gyroBias)*(to-from);
}

std::shared_ptr<gtsam::PreintegrationParams> preintegrationParams(const ImuNoise &noise)
{
    const double accelerometer = noise.accelerometer.density, gyroscope = noise.gyroscope.density;
    // Unreachable through planFit(), which derives the noise: it guards a
    // caller that forgot to.
    for (double density : {accelerometer, gyroscope}) {
        if (!std::isfinite(density) || density <= 0)
            throw std::invalid_argument("Invalid fusion configuration");
    }
    // "D": the z axis of the navigation frame points down, so gravity is +z.
    auto params = gtsam::PreintegrationParams::MakeSharedD(kGravity.z());
    params->accelerometerCovariance = gtsam::I_3x3*accelerometer*accelerometer;
    params->gyroscopeCovariance = gtsam::I_3x3*gyroscope*gyroscope;
    params->integrationCovariance = gtsam::I_3x3*kIntegrationVariance;
    return params;
}

gtsam::PreintegratedImuMeasurements preintegrateImu(const Samples &samples, double start, double end,
                                                    const gtsam::imuBias::ConstantBias &bias,
                                                    const gtsam::Vector6 &scale, const ImuNoise &noise,
                                                    const ImuStepObserver &observer, gtsam::Matrix96 *scaleJacobian)
{
    // The params are shared with `pim`, and integrateMeasurement() reads the
    // two sensor covariances on every call, so writing them before each call
    // gives every step its own covariance.
    std::shared_ptr<gtsam::PreintegrationParams> params = preintegrationParams(noise);
    gtsam::PreintegratedImuMeasurements pim(params, bias);
    const std::vector<double> &t = samples.imuTime;
    const std::vector<double> e = integrationEdges(samples, start, end);
    const gtsam::Vector3 accScale = scale.head<3>(), gyroScale = scale.tail<3>();
    // The scale Jacobian of the steps so far.
    gtsam::Matrix96 H = gtsam::Matrix96::Zero();
    // The signal at the start of the step: the end of the previous one.
    gtsam::Vector3 forceStart = dividedAt(t, samples.force, e[0], accScale);
    gtsam::Vector3 gyroStart = dividedAt(t, samples.gyro, e[0], gyroScale);
    for (size_t i = 1; i < e.size(); ++i) {
        const double dt = e[i]-e[i-1], mid = (e[i]+e[i-1])/2;
        const gtsam::Vector3 gyroMid = dividedAt(t, samples.gyro, mid, gyroScale);
        const gtsam::Vector3 forceMid = dividedAt(t, samples.force, mid, accScale);
        // The library rotates a step's reading by the attitude at the step's
        // start, half a step behind the reading: an error of about
        // |omega x f| dt / 2, several m/s^2 at 13 Hz through a parachute
        // opening. So the reading is turned by half the step's bias-corrected
        // rotation first, and the library then applies it at the attitude of
        // the step's middle. The bias, which is in the device frame, is
        // removed for the turn and put back, since the library removes it
        // itself.
        const gtsam::Vector3 &accBias = bias.accelerometer();
        const gtsam::Vector3 halfAngle = (gyroMid-bias.gyroscope())*(dt/2);
        const gtsam::Rot3 halfStep = gtsam::Rot3::Expmap(halfAngle);
        ImuStep step{e[i-1], e[i], dt, halfStep.rotate(forceMid-accBias)+accBias, gyroMid, gtsam::Matrix9::Zero()};
        if (observer || scaleJacobian) {
            // The library's own transition and input Jacobians of the step:
            // integrateMeasurement() calls update() qualified, so the step
            // itself cannot be intercepted, and a copy updated with the same
            // readings computes the same A that the step propagates its
            // covariance with. Taken here once, for the observer (the
            // reconstruction's F_j) and for the scale Jacobian.
            gtsam::PreintegratedImuMeasurements copy = pim;
            gtsam::Matrix93 byForce, byRate;
            copy.update(step.force, step.gyro, step.dt, &step.transition, &byForce, &byRate);
            if (scaleJacobian) {
                // The step's input with respect to the scale: the divided
                // force through the turn, and the divided rate both directly
                // and through the half-step turn it sets.
                const gtsam::Matrix3 R = halfStep.matrix();
                const gtsam::Matrix3 F = forceMid.cwiseQuotient(accScale).asDiagonal();
                const gtsam::Matrix3 W = gyroMid.cwiseQuotient(gyroScale).asDiagonal();
                gtsam::Matrix96 input;
                input.leftCols<3>() = -byForce*R*F;
                input.rightCols<3>() = byForce*R*gtsam::skewSymmetric(forceMid-accBias)
                                       *gtsam::Rot3::ExpmapDerivative(halfAngle)*W*(dt/2) - byRate*W;
                H = step.transition*H + input;
            }
        }
        // Before the step's covariance is written, so the two sensor
        // covariances the step integrates with are always the ones set
        // below. The observer can still reach the other shared params
        // through p(), which GTSAM does not make const; it must only read.
        if (observer)
            observer(pim, step);
        const gtsam::Vector3 forceEnd = dividedAt(t, samples.force, e[i], accScale);
        const gtsam::Vector3 gyroEnd = dividedAt(t, samples.gyro, e[i], gyroScale);

        // The sampling term: the midpoint reading integrates the linear
        // interpolant exactly, so the step's error is the interpolant's,
        // w times the second derivative of the sample interval [t_k, t_k+1]
        // that holds the step (every IMU time is an edge, so one does). The
        // gyro bias is constant there and cancels.
        const size_t k = size_t(std::upper_bound(t.begin(), t.end(), mid)-t.begin())-1;
        const double w = samplingWeight(t[k], t[k+1], e[i-1], e[i]);
        const double samplingV = w*curvature(t, samples.force, k, accScale);
        const double samplingTheta = w*curvature(t, samples.gyro, k, gyroScale);
        // The remainder of the mid-step scheme against the true integral, to
        // second order, for rate and force linear across the step: the pure
        // rotation term, and the terms in the change of force and of rate
        // (coning, for the angle).
        const gtsam::Vector3 theta = (gyroMid-bias.gyroscope())*dt, dTheta = (gyroEnd-gyroStart)*dt;
        const gtsam::Vector3 fbar = forceMid-accBias, dForce = forceEnd-forceStart;
        const double remainderV = (dt/24*theta.cross(theta.cross(fbar))
                                   + dt/12*(theta.cross(dForce)-dTheta.cross(fbar))).norm();
        const double remainderTheta = theta.cross(dTheta).norm()/12;
        // Isotropic, from vector norms: an isotropic covariance is unchanged
        // by the half-step turn.
        params->accelerometerCovariance = stepCovariance(
            noise.accelerometer.density, samplingV*samplingV+remainderV*remainderV, dt);
        params->gyroscopeCovariance = stepCovariance(
            noise.gyroscope.density, samplingTheta*samplingTheta+remainderTheta*remainderTheta, dt);
        pim.integrateMeasurement(step.force, step.gyro, step.dt);
        forceStart = forceEnd;
        gyroStart = gyroEnd;
    }
    // The params now hold the last step's covariance, as the header promises;
    // the factor's noise model comes from preintMeasCov(), so they are not
    // restored.
    // The steps must add up to the interval, or the factor would relate the
    // two states over the wrong duration.
    if (std::abs(pim.deltaTij()-(end-start)) > kDurationTolerance)
        throw std::runtime_error("Preintegration duration mismatch");
    if (scaleJacobian)
        *scaleJacobian = H;
    return pim;
}

gtsam::Rot3 propagateAttitude(const Samples &samples, gtsam::Rot3 rotation, double start, double end,
                              const gtsam::Vector3 &gyroBias)
{
    if (start == end)
        return rotation;
    const double lo = std::min(start, end), hi = std::max(start, end);

    // The gap test looks at IMU samples only; the increments also break at
    // GNSS times, as every other integration in the model does.
    const std::vector<double> imuEdges = integrationEdges(samples, lo, hi);
    const double gap = kImuGapMedians*medianInterval(samples.imuTime);
    requireNoImuGap(imuEdges, gap);
    const Vectors increments = gyroIncrements(samples, integrationEdges(samples, lo, hi, true), gyroBias);

    if (end < start) {
        // Backwards: undo each step, last first. Rotations do not commute, so
        // this is not the inverse of the summed increments.
        for (auto it = increments.rbegin(); it != increments.rend(); ++it)
            rotation = rotation.compose(gtsam::Rot3::Expmap(-*it));
    } else {
        for (const auto &increment : increments)
            rotation = rotation.compose(gtsam::Rot3::Expmap(increment));
    }
    return rotation;
}

} // namespace FlySight::Fusion::Detail
