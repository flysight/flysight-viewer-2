#include "fusion/inputadapter.h"

#include <cmath>
#include <stdexcept>
#include <string>

#include <QJsonArray>

namespace FlySight::Fusion::Detail {

namespace {

/// A channel must have one finite value per sample of its time axis. `label`
/// is the public name of the channel, for the rejection reason.
void requireChannel(const char *label, const QVector<double> &values, qsizetype count)
{
    if (values.size() != count)
        throw std::invalid_argument(std::string("Missing or mismatched ") + label);
    for (double value : values) {
        if (!std::isfinite(value))
            throw std::invalid_argument(std::string("Nonfinite ") + label);
    }
}

/// Every channel against its time axis, in the order that decides which
/// defect is reported first.
void requireAllChannels(const Channels &c)
{
    const qsizetype ng = c.gnssTime.size(), ni = c.imuTime.size();
    requireChannel("Local/north", c.north, ng);
    requireChannel("Local/east", c.east, ng);
    requireChannel("Local/down", c.down, ng);
    requireChannel("Local/velN", c.velN, ng);
    requireChannel("Local/velE", c.velE, ng);
    requireChannel("Local/velD", c.velD, ng);
    requireChannel("GNSS/hAcc", c.hAcc, ng);
    requireChannel("GNSS/vAcc", c.vAcc, ng);
    requireChannel("GNSS/sAcc", c.sAcc, ng);
    requireChannel("IMU/ax", c.ax, ni);
    requireChannel("IMU/ay", c.ay, ni);
    requireChannel("IMU/az", c.az, ni);
    requireChannel("IMU/wx", c.wx, ni);
    requireChannel("IMU/wy", c.wy, ni);
    requireChannel("IMU/wz", c.wz, ni);
}

/// GNSS fixes relative to `epoch`. Horizontal accuracy is the sigma of both
/// horizontal position axes; speed accuracy the sigma of all three velocity axes.
void appendGnssSamples(Samples &d, const Channels &c, double epoch)
{
    for (qsizetype k = 0; k < c.gnssTime.size(); ++k) {
        if (!std::isfinite(c.gnssTime[k]))
            throw std::invalid_argument("Nonfinite GNSS UTC timestamp");
        if (c.hAcc[k] <= 0 || c.vAcc[k] <= 0 || c.sAcc[k] <= 0)
            throw std::invalid_argument("GNSS measurement sigmas must be positive");
        d.gnssTime.push_back(c.gnssTime[k] - epoch);
        d.position.emplace_back(c.north[k], c.east[k], c.down[k]);
        d.velocity.emplace_back(c.velN[k], c.velE[k], c.velD[k]);
        d.positionSigma.emplace_back(c.hAcc[k], c.hAcc[k], c.vAcc[k]);
        d.velocitySigma.emplace_back(c.sAcc[k], c.sAcc[k], c.sAcc[k]);
    }
}

/// IMU samples relative to `epoch`. Effective acceleration is already m/s^2;
/// effective rate is deg/s and the model works in rad/s.
void appendImuSamples(Samples &d, const Channels &c, double epoch)
{
    constexpr double radians = kPi / 180;
    for (qsizetype k = 0; k < c.imuTime.size(); ++k) {
        if (!std::isfinite(c.imuTime[k]))
            throw std::invalid_argument("Nonfinite IMU UTC timestamp");
        d.imuTime.push_back(c.imuTime[k] - epoch);
        d.force.emplace_back(c.ax[k], c.ay[k], c.az[k]);
        d.gyro.emplace_back(c.wx[k] * radians, c.wy[k] * radians, c.wz[k] * radians);
    }
}

/// What the fit was given, recorded in the diagnostics.
QJsonObject inputAudit(const Channels &c, double epoch)
{
    return {
        {"epoch_utc_s", epoch},
        {"imu_count", int(c.imuTime.size())},
        {"gnss_count", int(c.gnssTime.size())},
        {"origin_index", int(c.originIndex)},
        {"origin", QJsonArray{c.originLat, c.originLon, c.originHMSL}},
        {"height_method", "CSV hMSL used as approximate ellipsoid height in GeographicLib local frame"},
        {"time_method", "SessionData shared UTC conversion; common epoch subtraction only"}
    };
}

} // namespace

PreparedInput prepareInput(const Channels &c)
{
    if (c.gnssTime.size() < 3 || c.imuTime.size() < 2)
        throw std::invalid_argument("Sensor fusion needs GNSS, IMU and shared UTC time conversion");
    if (c.originIndex < 0 || c.originIndex >= c.gnssTime.size())
        throw std::invalid_argument("Local origin index outside GNSS samples");
    requireAllChannels(c);

    PreparedInput out;
    out.epoch = c.gnssTime.front();
    // Fixes before the local origin did not meet its accuracy gate; the fit
    // starts at the origin fix.
    out.usableStart = c.gnssTime[c.originIndex] - out.epoch;
    appendGnssSamples(out.recording, c, out.epoch);
    appendImuSamples(out.recording, c, out.epoch);

    // Both time axes must be strictly increasing: every later stage searches
    // them by bisection.
    requireIncreasingFiniteTimes(out.recording.imuTime);
    requireIncreasingFiniteTimes(out.recording.gnssTime);

    out.audit = inputAudit(c, out.epoch);
    return out;
}

} // namespace FlySight::Fusion::Detail
