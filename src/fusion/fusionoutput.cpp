#include "fusion/fusionoutput.h"

#include <algorithm>
#include <cmath>

#include <QJsonArray>
#include <QJsonDocument>

#include <gtsam/inference/Symbol.h>
#include <gtsam/navigation/ImuBias.h>

#include "calculations/anglehelper.h"

namespace FlySight::Fusion::Detail {

namespace {

// The legacy `initialization` key names the method; the keys that described
// the stationary window and the selected heading are null and stay present.
const char kInitializationMethod[] = "segmented initialization; heading from segment fits";

// What the published samples are, and what the one pass behind them leaves
// out; docs/SENSOR_FUSION.md section 4 quotes both.
const char kDenseOutput[] = "IMU-rate reconstruction at original IMU times: between fixes the IMU integrated "
                            "from the fitted state, the mismatch with the next fitted state shared over the "
                            "steps by their noise, in one linearized pass";
const char kLimitations[] = "Local batch convergence; heading may be ambiguous. Between fixes one linearized pass "
                            "with the fitted fix states, biases and scale factors held. Accuracies are first-order, "
                            "one standard deviation under the documented noise model, widened where the residuals "
                            "exceed it.";

QJsonArray toJsonArray(const gtsam::Vector3 &v)
{
    return QJsonArray{v.x(), v.y(), v.z()};
}

/// A measurement that could not be taken (NaN) is null, written explicitly:
/// what QJsonValue(double) makes of a NaN is not part of the format.
QJsonValue numberOrNull(double value)
{
    return std::isfinite(value) ? QJsonValue(value) : QJsonValue(QJsonValue::Null);
}

/// The rule that ended the fit, what it was judged on, and the thresholds in force.
QJsonObject stoppingObject(const Stopping &s)
{
    return QJsonObject{
        {"rule", QString::fromStdString(s.rule)},
        {"passes", s.passes},
        {"last_pass_mean_relative_decrease", numberOrNull(s.lastPassMeanRelativeDecrease)},
        {"repreintegration_cost_difference", numberOrNull(s.repreintegrationCostDifference)},
        {"bias_settled_tolerance", s.biasSettledTolerance},
        {"lambda_upper_bound", s.lambdaUpperBound},
        {"slow_tail", QJsonObject{
            {"window", s.slowTailWindow},
            {"max_mean_relative_decrease", s.slowTailMaxMeanRelativeDecrease},
            {"max_nrms", s.slowTailMaxNrms}}}};
}

QJsonObject qualityObject(const Quality &q)
{
    return QJsonObject{
        {"imu_nrms", q.imuNrms},
        {"position_nrms", q.positionNrms},
        {"velocity_nrms", q.velocityNrms},
        {"objective_per_state", q.objectivePerState}};
}

/// The initializer's account, the diagnostics' `initializer` object: one
/// object per segment, the segment length and the indices of the segments
/// that fell back.
QJsonObject initializerObject(const InitializerAccount &a)
{
    QJsonArray segments, fallbacks;
    for (const SegmentAccount &s : a.segments) {
        segments.append(QJsonObject{
            {"index", s.index},
            {"start_s", s.start},
            {"end_s", s.end},
            {"anchor_s", s.anchorTime},
            {"anchor_sacc_m_s", s.anchorSacc},
            {"prefix_length_s", s.prefixLength},
            {"yaw_sigma_deg", numberOrNull(s.yawSigmaDeg)},
            {"prefix_fits", s.prefixFits},
            {"prefix_iterations", s.prefixIterations},
            {"prefix_passes", s.prefixPasses},
            {"prefix_on_limit", s.prefixOnLimit},
            {"segment_on_limit", s.segmentOnLimit},
            {"growth_stop", QString::fromStdString(s.growthStop)},
            {"iterations", s.iterations},
            {"fallback", s.fallback}});
        if (s.fallback)
            fallbacks.append(s.index);
    }
    return QJsonObject{
        {"segment_length_s", a.segmentLength},
        {"segments", segments},
        {"fallback_segments", fallbacks}};
}

/// The one fit that was run, in the shape of a list of candidate fits: the
/// diagnostics format allows several starting headings although nothing
/// selects one any more (`heading_deg` is null). `gyro_bias_rad_s` is `b0`,
/// the bias at the reference temperature; the slope is under `model.gyro_bias`.
QJsonArray seedSummary(const FitResult &fit)
{
    const auto bias = fit.values.at<gtsam::imuBias::ConstantBias>(gtsam::symbol_shorthand::B(0));
    return QJsonArray{QJsonObject{
        {"heading_deg", QJsonValue::Null},
        {"converged", fit.converged},
        {"objective", fit.objective},
        {"iterations", int(fit.history.size())},
        {"acc_bias_m_s2", toJsonArray(bias.accelerometer())},
        {"gyro_bias_rad_s", toJsonArray(bias.gyroscope())},
        {"position_residual_rms_m", fit.positionRms},
        {"velocity_residual_rms_m_s", fit.velocityRms}}};
}

/// The fitted gyro bias model: b0 is B(0)'s gyro part (also seeds[0].gyro_bias_rad_s),
/// b1 the slope per degC, t_ref the reference temperature of the fitted window.
/// The full fit always runs the temperature model, so the two numbers are
/// never null and there is no flag.
QJsonObject gyroBiasObject(const FitResult &fit)
{
    const auto bias = fit.values.at<gtsam::imuBias::ConstantBias>(gtsam::symbol_shorthand::B(0));
    return QJsonObject{
        {"b0_rad_s", toJsonArray(bias.gyroscope())},
        {"b1_rad_s_per_degc", toJsonArray(fit.gyroBiasSlope)},
        {"t_ref_degc", fit.biasModel.tRef}};
}

/// The configuration the fit ran under: the four values of the attributes.
QJsonObject configurationObject(const ImuConfiguration &c)
{
    return QJsonObject{
        {"accel_fs_g", c.accelFsG},
        {"gyro_fs_deg_s", c.gyroFsDegS},
        {"accel_odr_hz", c.accelOdrHz},
        {"gyro_odr_hz", c.gyroOdrHz}};
}

/// The fitted scale factors, the factors themselves (one is the datasheet's
/// nominal sensitivity): accelerometer x, y, z and gyro x, y, z; and their
/// sigmas, the square roots of the diagonal of the covariance step's S(0)
/// block, each null when the covariance was not computed.
QJsonObject scaleObject(const FitResult &fit, const FitCovariance &covariance)
{
    const auto sigmas = [&](int first) -> QJsonValue {
        if (!covariance.computed)
            return QJsonValue::Null;
        QJsonArray array;
        for (int i = first; i < first+3; ++i)
            array.append(std::sqrt(covariance.globals(kScaleOffset+i, kScaleOffset+i)));
        return array;
    };
    return QJsonObject{
        {"acc", QJsonArray{fit.scale(0), fit.scale(1), fit.scale(2)}},
        {"gyro", QJsonArray{fit.scale(3), fit.scale(4), fit.scale(5)}},
        {"acc_sigma", sigmas(0)},
        {"gyro_sigma", sigmas(3)}};
}

/// The account of the accuracy: whether the covariance step computed it and,
/// if not, its failure; the heading prior of that step and the widening's
/// half-width, the kernel's constants; and over the published samples the
/// largest widening, how many samples were widened (w > 1) and how many
/// headings are at the cap, each null when nothing was computed.
QJsonObject accuracyObject(const FitCovariance &covariance, const std::vector<double> &widenings,
                           const Result &result)
{
    QJsonObject accuracy{
        {"computed", covariance.computed},
        {"failure", covariance.computed ? QJsonValue(QJsonValue::Null)
                                        : QJsonValue(QString::fromLatin1(kCovarianceFailure))},
        {"heading_prior_sigma_rad", kHeadingPriorSigmaRad},
        {"widening_half_width_s", kWideningHalfWidthS},
        {"max_widening", QJsonValue::Null},
        {"widened_samples", QJsonValue::Null},
        {"undetermined_heading_samples", QJsonValue::Null}};
    if (!covariance.computed)
        return accuracy;
    double largest = 1;
    int widened = 0;
    for (const double w : widenings) {
        largest = std::max(largest, w);
        widened += w > 1;
    }
    accuracy.insert("max_widening", largest);
    accuracy.insert("widened_samples", widened);
    accuracy.insert("undetermined_heading_samples",
                    int(std::count(result.headingAcc.begin(), result.headingAcc.end(), kYawSigmaCapDeg)));
    return accuracy;
}

/// The model of this fit: the noise the datasheet gives for its
/// configuration, which is not fitted, the fitted gyro bias model and the
/// fitted scale factors with their sigmas.
QJsonObject modelSummary(const ImuNoise &noise, const FitResult &fit, const FitCovariance &covariance)
{
    const SensorNoise &a = noise.accelerometer, &g = noise.gyroscope;
    return QJsonObject{
        {"noise", QJsonObject{
            {"acc", QJsonObject{
                {"datasheet_density_m_s2_rthz", a.datasheetDensity},
                {"bandwidth_hz", a.bandwidth},
                {"step_m_s2", a.step},
                {"sample_sigma_m_s2", a.sampleSigma},
                {"density_m_s2_rthz", a.density}}},
            {"gyro", QJsonObject{
                {"datasheet_density_rad_s_rthz", g.datasheetDensity},
                {"bandwidth_hz", g.bandwidth},
                {"step_rad_s", g.step},
                {"sample_sigma_rad_s", g.sampleSigma},
                {"density_rad_s_rthz", g.density}}}}},
        {"gyro_bias", gyroBiasObject(fit)},
        {"scale", scaleObject(fit, covariance)}};
}

QJsonArray residualArray(const FitResult &fit)
{
    QJsonArray residuals;
    for (const FactorResidual &r : fit.residuals) {
        residuals.append(QJsonObject{
            {"kind", QString::fromStdString(r.kind)},
            {"node", int(r.node)},
            {"time_s", r.time},
            {"squared_whitened_error", r.squaredWhitenedError}});
    }
    return residuals;
}

} // namespace

void fillOutputChannels(const ImuRateTrajectory &trajectory, const std::vector<double> &widenings, double epoch,
                        Result &result)
{
    const qsizetype count = qsizetype(trajectory.time.size());
    for (QVector<double> *channel : { &result.time, &result.north, &result.east, &result.down,
                                      &result.velN, &result.velE, &result.velD,
                                      &result.accN, &result.accE, &result.accD,
                                      &result.roll, &result.pitch, &result.yaw,
                                      &result.qx, &result.qy, &result.qz, &result.qw })
        channel->reserve(count);

    for (size_t i = 0; i < trajectory.time.size(); ++i) {
        result.time.append(epoch+trajectory.time[i]);
        const auto a = trajectory.acceleration[i], p = trajectory.position[i], v = trajectory.velocity[i];
        const gtsam::Vector3 rpy = trajectory.rotation[i].rpy()*180/kPi;
        const auto q = trajectory.rotation[i].toQuaternion();
        result.accN.append(a.x());   result.accE.append(a.y());   result.accD.append(a.z());
        result.north.append(p.x());  result.east.append(p.y());   result.down.append(p.z());
        result.velN.append(v.x());   result.velE.append(v.y());   result.velD.append(v.z());
        result.roll.append(rpy.x()); result.pitch.append(rpy.y()); result.yaw.append(rpy.z());
        result.qx.append(q.x());     result.qy.append(q.y());     result.qz.append(q.z());
        result.qw.append(q.w());
    }
    // rpy() wraps at +/-180 degrees; a plot wants a continuous angle.
    result.roll = Calculations::unwrapDegrees(result.roll);
    result.pitch = Calculations::unwrapDegrees(result.pitch);
    result.yaw = Calculations::unwrapDegrees(result.yaw);

    // The accuracies, widened: heading and tilt before the cap, so that a
    // widened sigma never exceeds it; the accelerations uncapped.
    if (trajectory.headingAcc.empty())
        return;
    for (QVector<double> *channel : { &result.headingAcc, &result.tiltAcc, &result.accHAcc, &result.accDAcc })
        channel->reserve(count);
    for (size_t i = 0; i < trajectory.time.size(); ++i) {
        const double w = widenings[i];
        result.headingAcc.append(std::min(kYawSigmaCapDeg, w*trajectory.headingAcc[i]));
        result.tiltAcc.append(std::min(kYawSigmaCapDeg, w*trajectory.tiltAcc[i]));
        result.accHAcc.append(w*trajectory.accHAcc[i]);
        result.accDAcc.append(w*trajectory.accDAcc[i]);
    }
}

QJsonObject successDiagnostics(const PreparedInput &prepared, const InitializerAccount &account,
                               const FitResult &fit, const Samples &window,
                               const ImuRateTrajectory &trajectory, const Tuning &tuning,
                               const FitCovariance &covariance, const std::vector<double> &widenings,
                               const Result &result)
{
    return QJsonObject{
        {"algorithm", Algorithm},
        {"configuration", configurationObject(tuning.noise.configuration)},
        {"model", modelSummary(tuning.noise, fit, covariance)},
        {"accuracy", accuracyObject(covariance, widenings, result)},
        {"input", prepared.audit},
        {"seeds", seedSummary(fit)},
        {"initialization", kInitializationMethod},
        {"stationary_interval_s", QJsonValue::Null},
        {"anchor_time_s", QJsonValue::Null},
        {"selected_heading_deg", QJsonValue::Null},
        {"initializer", initializerObject(account)},
        {"objective", fit.objective},
        {"start_s", window.gnssTime.front()},
        {"end_s", window.gnssTime.back()},
        {"gnss_states", int(window.gnssTime.size())},
        {"imu_outputs", int(trajectory.time.size())},
        {"seed_comparison_performed", false},
        {"max_seed_vs_selected_angle_deg", QJsonValue::Null},
        {"max_seed_vs_selected_acceleration_m_s2", QJsonValue::Null},
        {"max_endpoint_correction_deg", trajectory.maxEndpointCorrectionDeg},
        {"max_velocity_mismatch_m_s", trajectory.maxVelocityMismatch},
        {"max_step_correction_m_s2", trajectory.maxStepCorrection},
        {"max_step_correction_time_s", trajectory.maxStepCorrectionTime},
        {"dense_output", kDenseOutput},
        {"orientation", "body to fixed NED; quaternion xyzw; RPY degrees"},
        {"limitations", kLimitations},
        {"residuals", residualArray(fit)},
        {"stopping", stoppingObject(fit.stopping)},
        {"quality", qualityObject(fit.quality)}};
}

QJsonObject failureDiagnostics(const QString &reason, const Stopping *stopping, const Quality *quality)
{
    QJsonObject diagnostics{{"algorithm", Algorithm}, {"failure", reason}};
    if (stopping)
        diagnostics.insert("stopping", stoppingObject(*stopping));
    if (quality)
        diagnostics.insert("quality", qualityObject(*quality));
    return diagnostics;
}

QString toCompactJson(const QJsonObject &object)
{
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

} // namespace FlySight::Fusion::Detail
