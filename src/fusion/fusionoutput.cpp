#include "fusion/fusionoutput.h"

#include <algorithm>

#include <QJsonArray>
#include <QJsonDocument>

#include <gtsam/inference/Symbol.h>
#include <gtsam/navigation/ImuBias.h>

#include "calculations/anglehelper.h"

namespace FlySight::Fusion::Detail {

namespace {

const char kAlgorithm[] = "batch-shared-bias-v1";

QJsonArray toJsonArray(const gtsam::Vector3 &v)
{
    return QJsonArray{v.x(), v.y(), v.z()};
}

/// The one fit that was run, in the shape of a list of candidate fits: the
/// diagnostics format allows several starting headings although the model
/// uses one.
QJsonArray seedSummary(const FitResult &fit)
{
    const auto bias = fit.values.at<gtsam::imuBias::ConstantBias>(gtsam::symbol_shorthand::B(0));
    return QJsonArray{QJsonObject{
        {"heading_deg", fit.heading},
        {"converged", fit.converged},
        {"objective", fit.objective},
        {"iterations", int(fit.history.size())},
        {"acc_bias_m_s2", toJsonArray(bias.accelerometer())},
        {"gyro_bias_rad_s", toJsonArray(bias.gyroscope())},
        {"position_residual_rms_m", fit.positionRms},
        {"velocity_residual_rms_m_s", fit.velocityRms}}};
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

QJsonObject successDiagnostics(const PreparedInput &prepared, const InitialAttitude &attitude,
                               const FitResult &fit, const Samples &window,
                               const DenseTrajectory &dense)
{
    return QJsonObject{
        {"algorithm", kAlgorithm},
        {"input", prepared.audit},
        {"seeds", seedSummary(fit)},
        {"initialization", QString::fromStdString(attitude.method)},
        {"stationary_interval_s", QJsonArray{attitude.intervalStart, attitude.intervalEnd}},
        {"anchor_time_s", attitude.anchorTime},
        {"selected_heading_deg", fit.heading},
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
        {"residuals", residualArray(fit)}};
}

QJsonObject failureDiagnostics(const QString &reason)
{
    return QJsonObject{{"algorithm", kAlgorithm}, {"failure", reason}};
}

QString toCompactJson(const QJsonObject &object)
{
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

} // namespace FlySight::Fusion::Detail
