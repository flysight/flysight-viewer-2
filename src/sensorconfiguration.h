#ifndef FLYSIGHT_SENSORCONFIGURATION_H
#define FLYSIGHT_SENSORCONFIGURATION_H

#include <optional>

#include <QString>
#include <QStringList>
#include <QVariant>

// The one authority on the sensor configuration keys: the header attributes in
// which the firmware states how the sensors were configured, their names,
// the values each accepts, the message for a value it does not, and the
// default of the four keys the fit reads. Shared by the importer and the
// exporter (which reject a malformed value) and the fusion registration (which
// declares the four as inputs of the fit and registers their defaults), so it
// lives in flysight_model, which both reach.
//
// Seven keys are written in SENSOR.CSV: the accelerometer's and the gyro's
// full-scale range and output data rate, and the barometer's, the humidity
// sensor's and the magnetometer's rates. Two are written in TRACK.CSV: the
// receiver's dynamic model and its measurement rate. Nothing reads which file
// carries a key: the importer validates a key wherever it appears, as it does
// SCHEMA_VER, so this is prose and not data.
//
// The value is a key's actual setting, in the unit its name ends with, as the
// rest of the file is written; a value is checked after surrounding
// whitespace is trimmed, and the recorded text is stored as it is. A key
// with a list accepts exactly the spellings of its list ("16", never "16.0");
// a key without one accepts a plain positive decimal, digits with an optional
// fraction, no sign and no exponent.
//
// The default is that of the firmware of the recordings on disk, which writes
// none of these keys: FirmwareVersion names it. Only the four IMU keys have
// one; the receiver's model and rate and the other sensors' rates have none.
// The importer never writes it: a default is a calculation
// (docs/CALCULATIONS.md section 5), registered by the fusion registration.
//
// Pure: Qt Core only, no logging, no state. Safe to use from any thread.

namespace FlySight {
namespace SensorConfiguration {

constexpr char AccelFsG[] = "ACCEL_FS_G";        ///< accelerometer full-scale range, g
constexpr char GyroFsDegS[] = "GYRO_FS_DEG_S";   ///< gyro full-scale range, deg/s
constexpr char AccelOdrHz[] = "ACCEL_ODR_HZ";    ///< accelerometer output data rate, Hz
constexpr char GyroOdrHz[] = "GYRO_ODR_HZ";      ///< gyro output data rate, Hz
constexpr char BaroOdrHz[] = "BARO_ODR_HZ";      ///< barometer output data rate, Hz
constexpr char HumOdrHz[] = "HUM_ODR_HZ";        ///< humidity sensor output data rate, Hz
constexpr char MagOdrHz[] = "MAG_ODR_HZ";        ///< magnetometer output data rate, Hz
constexpr char GnssModel[] = "GNSS_MODEL";       ///< the receiver's dynamic model, by name
constexpr char GnssRateHz[] = "GNSS_RATE_HZ";    ///< the receiver's measurement rate, Hz

/// The firmware whose fixed configuration the default describes.
constexpr char FirmwareVersion[] = "v2023.09.22";

/// The nine keys, in the order above.
QStringList keys();

/// True iff `key` is one of the nine.
bool isKey(const QString &key);

/// True iff `recorded`, with surrounding whitespace trimmed, is a value `key`
/// accepts. False for a key that is not one of the nine.
bool isValidValue(const QString &key, const QVariant &recorded);

/// The error for a value `key` does not accept, with the recorded text as it
/// is: "Unsupported ACCEL_FS_G '16.0' (supported: 2, 4, 8, 16)" for a key with
/// a list, "Unsupported BARO_ODR_HZ '0' (supported: a positive decimal number)"
/// for one without.
QString unsupportedMessage(const QString &key, const QVariant &recorded);

/// The default of a key as text ("16", "2000", "12.5", "12.5" for the four IMU
/// keys); nothing for the other five, which have no default.
std::optional<QString> defaultValue(const QString &key);

} // namespace SensorConfiguration
} // namespace FlySight

#endif // FLYSIGHT_SENSORCONFIGURATION_H
