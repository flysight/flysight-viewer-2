#ifndef FLYSIGHT_FUSION_FUSION_H
#define FLYSIGHT_FUSION_FUSION_H

#include <QString>
#include <QVector>
#include <QtGlobal>

#include <functional>
#include <limits>

namespace FlySight::Fusion {

/// How the IMU was configured, as the recording's configuration attributes
/// state it or their constant defaults supply it (docs/DATA_SCHEMA.md section
/// 2): the full-scale ranges in g and deg/s and the output data rates in Hz,
/// the keys' own values as numbers. A member is a quiet NaN when its attribute
/// is not a number, and stays one unless set.
struct ImuConfiguration {
    double accelFsG = std::numeric_limits<double>::quiet_NaN();     ///< ACCEL_FS_G, g
    double gyroFsDegS = std::numeric_limits<double>::quiet_NaN();   ///< GYRO_FS_DEG_S, deg/s
    double accelOdrHz = std::numeric_limits<double>::quiet_NaN();   ///< ACCEL_ODR_HZ, Hz
    double gyroOdrHz = std::numeric_limits<double>::quiet_NaN();    ///< GYRO_ODR_HZ, Hz
};

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
    /// The four IMU configuration attributes: the fit's noise model comes from
    /// them (docs/SENSOR_FUSION.md section 4), the readings must lie on the
    /// lattice of the stated ranges and be logged at the stated rates, and the
    /// diagnostics report them.
    ImuConfiguration imuConfiguration;
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
    /// Compact JSON. On success: input audit, the configuration the fit ran
    /// under (`configuration`), initializer, objective, biases, the model
    /// (`model.noise`: each sensor's datasheet density, bandwidth, step,
    /// per-sample sigma and integration density; `model.gyro_bias`: `b0`,
    /// `b1`, the reference temperature; `model.scale`: the fitted scale
    /// factors, `acc` and `gyro`, each [x, y, z], and their sigmas
    /// `acc_sigma` and `gyro_sigma`, null when the covariance was not
    /// computed), residuals, and the account of the accuracy (`accuracy`:
    /// whether the covariance was computed and why not, the heading prior and
    /// the widening window, and the widening's and the undetermined
    /// heading's counts). On Rejected / SolverFailed: the algorithm name and
    /// the failure. Empty only when Cancelled.
    QString diagnosticsJson;
    /// Succeeded only; otherwise all empty. All the same length and aligned
    /// with `time` (UTC s): the original IMU samples inside the fitted
    /// interval, whatever the GNSS rate. Attitude, position m and velocity m/s
    /// are the fitted state at every IMU sample: between two fixes the IMU
    /// integrated from the fitted state at the first, joined to the fitted
    /// state at the second by sharing the mismatch over the steps by their
    /// noise (docs/SENSOR_FUSION.md section 4). Acceleration m/s^2 is
    /// inertial, NED: the reading divided by the fitted accelerometer scale
    /// factors, with the bias removed, rotated by that attitude, plus
    /// gravity, plus the model's share of the correction, so that integrated
    /// it reproduces the velocity. roll, pitch and yaw are
    /// degrees, unwrapped across the whole fit with the rule the GNSS course
    /// uses; qx..qw is the same body-to-NED attitude as a quaternion.
    QVector<double> time, north, east, down, velN, velE, velD, accN, accE, accD,
                    roll, pitch, yaw, qx, qy, qz, qw;
    /// The accuracy at every sample, aligned with `time` (docs/SENSOR_FUSION.md
    /// section 4): one standard deviation from the covariance of the converged
    /// solution under the documented model, widened where the residuals
    /// exceed what the model allows. headingAcc and tiltAcc are degrees in
    /// (0, 180], 180 meaning undetermined; accHAcc and accDAcc are m/s^2, the
    /// horizontal acceleration's along its direction and the vertical's.
    /// Filled only for Succeeded with the covariance computed; empty for every
    /// other outcome and for a success whose covariance failed, which the
    /// diagnostics' `accuracy` says (the seventeen arrays above are then
    /// filled as ever).
    QVector<double> headingAcc, tiltAcc, accHAcc, accDAcc;
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
/// solve in progress finishes first, and so does the work after the last
/// iteration, which has no boundary: the covariance step (one factorization of
/// the converged graph) and the reconstruction at the IMU samples, which
/// composes the accuracy in the same pass, a few seconds on the longest
/// recording.
///
/// Every std::exception raised inside (the checks, the solver) becomes a
/// Rejected or SolverFailed result. Two things propagate: std::bad_alloc,
/// which is not a function of the inputs, and an exception that is not a
/// std::exception, which neither this library nor the solver throws and a
/// callback must not.
Result run(const Channels &channels, const ProgressFn &progress = {},
           const CancelFn &cancelRequested = {});

/// Names the model and arithmetic of run(). It is written as "algorithm" in
/// every diagnostics object and declared as the result version of the fit
/// registration (CalculationDescriptor::resultVersion), so changing it drops
/// every stored fit. Change it whenever a change can alter what run() returns
/// for the same channels.
inline constexpr char Algorithm[] = "batch-temperature-bias-v8";

} // namespace FlySight::Fusion

#endif // FLYSIGHT_FUSION_FUSION_H
