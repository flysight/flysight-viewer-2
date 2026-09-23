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

/// The model of this fit: the per-step constants of the tuning, which are
/// not fitted, and the fitted gyro bias model.
QJsonObject modelSummary(const Tuning &tuning, const FitResult &fit)
{
    return QJsonObject{
        {"per_step", QJsonObject{
            {"gyro_slope_s", tuning.gyroStepSlope},
            {"acc_slope_s", tuning.accStepSlope}}},
        {"gyro_bias", gyroBiasObject(fit)}};
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

void fillOutputChannels(const DenseTrajectory &dense, double epoch, Result &result)
{
    const qsizetype count = qsizetype(dense.time.size());
    for (QVector<double> *channel : { &result.time, &result.north, &result.east, &result.down,
                                      &result.velN, &result.velE, &result.velD,
                                      &result.accN, &result.accE, &result.accD,
                                      &result.roll, &result.pitch, &result.yaw,
                                      &result.qx, &result.qy, &result.qz, &result.qw })
        channel->reserve(count);

    for (size_t i = 0; i < dense.time.size(); ++i) {
        result.time.append(epoch+dense.time[i]);
        const auto a = dense.acceleration[i], p = dense.position[i], v = dense.velocity[i];
        const gtsam::Vector3 rpy = dense.rotation[i].rpy()*180/kPi;
        const auto q = dense.rotation[i].toQuaternion();
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
}

QJsonObject successDiagnostics(const PreparedInput &prepared, const InitializerAccount &account,
                               const FitResult &fit, const Samples &window,
                               const DenseTrajectory &dense, const Tuning &tuning)
{
    return QJsonObject{
        {"algorithm", Algorithm},
        {"model", modelSummary(tuning, fit)},
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
        {"imu_outputs", int(dense.time.size())},
        {"seed_comparison_performed", false},
        {"max_seed_vs_selected_angle_deg", QJsonValue::Null},
        {"max_seed_vs_selected_acceleration_m_s2", QJsonValue::Null},
        {"max_endpoint_correction_deg",
         *std::max_element(dense.endpointCorrection.begin(), dense.endpointCorrection.end())},
        {"display_position_velocity", "linear interpolation of optimized GNSS states at original IMU times"},
        {"orientation", "body to fixed NED; quaternion xyzw; RPY degrees"},
        {"limitations", "Local batch convergence; heading may be ambiguous. Dense output is not an IMU-rate smoothing posterior."},
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
