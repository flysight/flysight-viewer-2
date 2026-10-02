#ifndef FLYSIGHT_FUSION_SENSORNOISE_H
#define FLYSIGHT_FUSION_SENSORNOISE_H

#include <limits>

#include <QVector>

#include "fusion/fusion.h"

// Internal to the fusion library: the IMU's datasheet, the only place in the
// code where it exists (LSM6DSO, DS12140 Rev 3; docs/SENSOR_FUSION.md section
// 4 carries the table with its sources). For a configuration it gives each
// sensor's noise as the fit models it: the noise density and the bandwidth of
// the datasheet at the configured range and rate, the quantization step at
// the range (the gyro's sensitivity; the accelerometer's range over 32768
// counts, which its printed sensitivity rounds), the per-sample sigma they
// make, the density the integration uses, derived from that sigma and nothing
// else, and the sensitivity tolerance. And for a recording it tells which range the
// readings show: the coarsest range whose lattice of possible values every
// reading lies on.
//
// The table assumes what the firmware of the default configuration
// (SensorConfiguration::FirmwareVersion) runs: both sensors in
// high-performance mode, the gyro's LPF2 alone (no LPF1) and the
// accelerometer's LPF1 alone (LPF2_XL_EN = 0). Every number is the
// datasheet's or derived from it here; nothing is fitted to recordings.
//
// Qt Core and the standard library only, no GTSAM and no Eigen: the checks
// and the integration read it, and so may a test without the solver.

namespace FlySight::Fusion::Detail {

// The kernel's one spelling of pi and of standard gravity (m/s^2), the g of
// the datasheet's units and of the conversion layer. They live here, beside
// the datasheet that needs both, so that the unit stays solver-free while the
// rest of the kernel (fusionsamples.h includes this header) shares them.
constexpr double kPi = 3.14159265358979323846;
constexpr double kStandardGravity = 9.80665;

/// One sensor at its configured range and rate, in kernel units (m/s^2 and
/// rad/s) unless a member says otherwise. Every member is a quiet NaN until
/// imuNoise() fills it.
struct SensorNoise {
    static constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
    double rate = kNaN;                  ///< the configured output data rate, Hz (the key's value)
    double datasheetDensity = kNaN;       ///< the datasheet's noise density at the range: An (m/s^2/sqrt(Hz)) or Rn (rad/s/sqrt(Hz))
    double bandwidth = kNaN;              ///< the datasheet's bandwidth at the rate and fixed filter, Hz
    double step = kNaN;                   ///< the quantization step at the range, the step of its lattice: m/s^2 or rad/s
    double sampleSigma = kNaN;            ///< one reading's sigma, sqrt(datasheetDensity^2 bandwidth + step^2 / 12)
    double density = kNaN;                ///< the integration's density, sampleSigma sqrt(1 / rate): a step of the nominal length carries one sample's variance
    double sensitivityTolerance = kNaN;   ///< the datasheet's sensitivity tolerance, a fraction
};

/// The noise of both sensors under one configuration, which it carries.
struct ImuNoise {
    ImuConfiguration configuration;
    SensorNoise accelerometer, gyroscope;
};

/// The datasheet's noise for `configuration`. Throws std::invalid_argument
/// with "No datasheet entry for <KEY> = <value>; sensor fusion unavailable"
/// (the value as QString::number writes it, "nan" for a NaN) naming the first
/// member, in key order (ACCEL_FS_G, GYRO_FS_DEG_S, ACCEL_ODR_HZ, GYRO_ODR_HZ),
/// that is not a value of its list: the accelerometer's 1.6 Hz, which exists
/// only in low-power mode, has no entry.
ImuNoise imuNoise(const ImuConfiguration &configuration);

enum class ImuSensor { Accelerometer, Gyroscope };

/// The coarsest full-scale range of `sensor` (g or deg/s) whose lattice all
/// but one in a thousand of the values of `x`, `y` and `z` fit, the three
/// axes together; NaN when none does. (A real recording carries a few
/// readings off its lattice; they must not decide.) The values are in the
/// units of Channels (m/s^2, deg/s), as the
/// kernel receives them and before any correction of its own. A value fits a
/// lattice when it is within one unit of the last decimal the file writes (of
/// g, of deg/s), carried through the conversion layer's largest factor, of a
/// multiple of the lattice step: the firmware truncates, so a reading may lie
/// a whole unit below the value it stands for.
double rangeShownByReadings(ImuSensor sensor, const QVector<double> &x, const QVector<double> &y,
                            const QVector<double> &z);

} // namespace FlySight::Fusion::Detail

#endif // FLYSIGHT_FUSION_SENSORNOISE_H
