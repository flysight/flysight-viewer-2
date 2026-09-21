#ifndef TIMEFITHELPER_H
#define TIMEFITHELPER_H

#include <optional>

#include <QVector>

namespace FlySight {
namespace Calculations {

/// Inverts the time fit utc = a * systemTime + b for a sequence of UTC times:
/// systemTime[i] = (utcTime[i] - b) / a. Nothing when the fit is degenerate
/// (a == 0) and cannot be inverted.
///
/// The one inverse of the time fit: the device-time axis of the GNSS sensor
/// and that of the sensor fusion's trajectory both use it. Header-only, so
/// that a library that does not link the built-in calculations can share it.
inline std::optional<QVector<double>> systemTimeFromUtc(const QVector<double> &utcTime, double a, double b)
{
    if (a == 0.0)
        return std::nullopt;

    QVector<double> result(utcTime.size());
    for (qsizetype i = 0; i < utcTime.size(); ++i)
        result[i] = (utcTime[i] - b) / a;
    return result;
}

} // namespace Calculations
} // namespace FlySight

#endif // TIMEFITHELPER_H
