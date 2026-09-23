#ifndef FLYSIGHT_FUSION_FUSION_H
#define FLYSIGHT_FUSION_FUSION_H

#include <QString>
#include <QVector>
#include <QtGlobal>

#include <functional>

namespace FlySight::Fusion {

/// Effective values of the fit's inputs, exactly as the engine supplies them.
///
/// GNSS channels share gnssTime's length and IMU channels imuTime's; anything
/// else is a rejection, not a precondition.
struct Channels {
    QVector<double> gnssTime;            ///< GNSS/_time, UTC s
    QVector<double> north, east, down;   ///< Local/north|east|down, m
    QVector<double> velN, velE, velD;    ///< Local/velN|velE|velD, m/s
    QVector<double> hAcc, vAcc, sAcc;    ///< GNSS/hAcc|vAcc (m), GNSS/sAcc (m/s)
    QVector<double> imuTime;             ///< IMU/_time, UTC s
    QVector<double> ax, ay, az;          ///< IMU/ax|ay|az, specific force, m/s^2
    QVector<double> wx, wy, wz;          ///< IMU/wx|wy|wz, deg/s
    QVector<double> imuTemperature;      ///< IMU/temperature, degC: the IMU's own temperature, one value per imuTime sample
    qint64 originIndex = -1;            ///< _LOCAL_ORIGIN_INDEX: the fit starts at this fix
    /// _LOCAL_ORIGIN_LAT|LON|HMSL. Recorded in the diagnostics only; the
    /// numbers do not depend on them.
    double originLat = 0, originLon = 0, originHMSL = 0;
};

/// How a run ended. Rejected and SolverFailed are both results: functions of
/// the inputs, so the same inputs give the same outcome again.
enum class Outcome {
    Succeeded,      ///< the fit converged; the arrays are filled
    Rejected,       ///< the model cannot use these inputs (decided before the fit starts)
    SolverFailed,   ///< the fit started and did not produce a usable solution
    Cancelled       ///< cancelRequested() returned true at a boundary
};

/// What run() returns.
struct Result {
    Outcome outcome = Outcome::Cancelled;
    /// Rejected / SolverFailed: why, in the words that are also the
    /// diagnostics' "failure". Empty otherwise.
    QString reason;
    /// Compact JSON. On success: input audit, initializer, objective, biases,
    /// the gyro bias model (`model.gyro_bias`: `b0`, `b1`, the reference
    /// temperature), residuals. On Rejected / SolverFailed: the algorithm
    /// name and the failure. Empty only when Cancelled.
    QString diagnosticsJson;
    /// Succeeded only; otherwise all empty. All the same length and aligned
    /// with `time` (UTC s): the original IMU samples inside the fitted
    /// interval. Position m and velocity m/s are the optimized GNSS states
    /// interpolated for display; acceleration m/s^2 is inertial, NED. roll,
    /// pitch and yaw are degrees, unwrapped across the whole fit with the rule
    /// the GNSS course uses; qx..qw is the same body-to-NED attitude as a
    /// quaternion.
    QVector<double> time, north, east, down, velN, velE, velD, accN, accE, accD,
                    roll, pitch, yaw, qx, qy, qz, qw;
};

/// Receives a short text describing the stage the fit has reached. Must not throw.
using ProgressFn = std::function<void(const QString &text)>;
/// Asked at each boundary whether to abandon the fit, immediately after
/// ProgressFn has been called for that boundary; true abandons it there. Must
/// not throw.
using CancelFn = std::function<bool()>;

/// The batch GNSS/IMU factor-graph fit.
///
/// Pure: a function of `channels`. It touches no session, engine, preference
/// or GUI object and keeps no state between calls, so it may run on any
/// thread (give that thread a 64 MiB stack: large elimination trees recurse
/// deeply). The callbacks cannot influence the result except by abandoning it.
/// Preparation is a few single passes over the recording and asks nothing.
/// Cancellation is observed at every reported boundary: `Starting fit`;
/// `Integrating IMU factors`, every 256 states of every graph build; and
/// every iteration of every optimizer pass, in the initializer's prefix and
/// segment fits (whose texts name the segment) as in the full fit. A linear
/// solve in progress finishes first.
///
/// Every std::exception raised inside (the checks, the solver) becomes a
/// Rejected or SolverFailed result. Two things propagate: std::bad_alloc,
/// which is not a function of the inputs, and an exception that is not a
/// std::exception, which neither this library nor the solver throws and a
/// callback must not.
Result run(const Channels &channels, const ProgressFn &progress = {},
           const CancelFn &cancelRequested = {});

} // namespace FlySight::Fusion

#endif // FLYSIGHT_FUSION_FUSION_H
