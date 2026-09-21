#ifndef ANGLEHELPER_H
#define ANGLEHELPER_H

#include <QVector>

namespace FlySight {
namespace Calculations {

/// Unwraps a sequence of angles given in [-180, 180] degrees into a continuous
/// one. The first angle is kept; every later one continues from its
/// predecessor by the shortest adjacent change, so whole turns accumulate
/// over the sequence. A change of exactly half a turn is not wrapped.
///
/// The one unwrap rule of the application: the GNSS course and the sensor
/// fusion's roll, pitch and yaw both use it.
inline QVector<double> unwrapDegrees(const QVector<double> &angles)
{
    if (angles.isEmpty())
        return {};

    QVector<double> result;
    result.reserve(angles.size());
    result.append(angles.front());
    for (qsizetype i = 1; i < angles.size(); ++i) {
        double delta = angles[i] - angles[i - 1];
        if (delta > 180.0) delta -= 360.0;
        if (delta < -180.0) delta += 360.0;
        result.append(result.back() + delta);
    }
    return result;
}

} // namespace Calculations
} // namespace FlySight

#endif // ANGLEHELPER_H
