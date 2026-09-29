#ifndef TRACKHELPER_H
#define TRACKHELPER_H

#include <algorithm>
#include <cmath>
#include <optional>

#include <QVariant>
#include <QVector>

#include "../engine/evaluationcontext.h"
#include "../sessiondata.h"

namespace FlySight {
namespace Calculations {

/// A wind component as the calculations read the attribute: a value that is
/// not a number (a hand-edited or blank stored value) counts as no wind.
///
/// The one wind rule of the application: every GNSS calculation that reads
/// wind and the sensor fusion's track accelerations use it.
inline double windComponent(const QVariant &value)
{
    bool ok = false;
    const double wind = value.toDouble(&ok);
    return ok ? wind : 0.0;
}

/// The samples the track-relative accelerations are defined on: an
/// acceleration and a velocity in north-east-down, sample by sample, and the
/// wind (north and east) that turns the velocity into the velocity through
/// the air.
///
/// alongTrackAcceleration() and crossTrackAcceleration() below are the one
/// definition of the track-relative accelerations: the GNSS along-track and
/// cross-track calculations and the sensor fusion's track accelerations all
/// use them, so the two categories agree in sign and in what "track" means.
/// Header-only, so that a library that does not link the built-in
/// calculations can share them.
struct TrackSamples {
    QVector<double> accN, accE, accD;
    QVector<double> velN, velE, velD;
    double windN = 0.0;
    double windE = 0.0;
};

/// The samples of one sensor, read from `ctx` as every track-relative
/// acceleration reads them: the sensor's six north-east-down arrays and the
/// two wind attributes. The GNSS calculations read "GNSS", the sensor
/// fusion's "Fusion"; the calculation declares the same eight inputs.
inline TrackSamples trackSamples(const EvaluationContext &ctx, const char *sensor)
{
    TrackSamples s;
    s.accN = ctx.measurement(sensor, "accN");
    s.accE = ctx.measurement(sensor, "accE");
    s.accD = ctx.measurement(sensor, "accD");
    s.velN = ctx.measurement(sensor, "velN");
    s.velE = ctx.measurement(sensor, "velE");
    s.velD = ctx.measurement(sensor, "velD");
    s.windN = windComponent(ctx.attribute(SessionKeys::WindN));
    s.windE = windComponent(ctx.attribute(SessionKeys::WindE));
    return s;
}

/// Whether every array of `s` has samples and all six have the same length:
/// otherwise there is no track to project on.
inline bool hasTrack(const TrackSamples &s)
{
    const qsizetype n = s.accN.size();
    return n > 0 && s.accE.size() == n && s.accD.size() == n
        && s.velN.size() == n && s.velE.size() == n && s.velD.size() == n;
}

/// The along-track acceleration of sample i: the acceleration projected on
/// the unit vector of the wind-corrected velocity, zero when that velocity is
/// below 1e-9 m/s and has no direction.
inline double alongTrackAt(const TrackSamples &s, qsizetype i)
{
    const double wcN = s.velN[i] - s.windN;
    const double wcE = s.velE[i] - s.windE;
    const double wcD = s.velD[i];
    const double wcMag = std::sqrt(wcN * wcN + wcE * wcE + wcD * wcD);
    if (wcMag < 1e-9)
        return 0.0;

    const double uN = wcN / wcMag;
    const double uE = wcE / wcMag;
    const double uD = wcD / wcMag;
    return s.accN[i] * uN + s.accE[i] * uE + s.accD[i] * uD;
}

/// Along-track acceleration, sample by sample: positive when the body speeds
/// up through the air. Nothing when an array is empty or the lengths differ.
inline std::optional<QVector<double>> alongTrackAcceleration(const TrackSamples &s)
{
    if (!hasTrack(s))
        return std::nullopt;

    QVector<double> result;
    result.reserve(s.accN.size());
    for (qsizetype i = 0; i < s.accN.size(); ++i)
        result.append(alongTrackAt(s, i));
    return result;
}

/// Cross-track acceleration, sample by sample: the magnitude of what remains
/// of the acceleration once its along-track component is taken away, never
/// negative. With no direction of travel it is the whole magnitude. Nothing
/// when an array is empty or the lengths differ.
inline std::optional<QVector<double>> crossTrackAcceleration(const TrackSamples &s)
{
    if (!hasTrack(s))
        return std::nullopt;

    QVector<double> result;
    result.reserve(s.accN.size());
    for (qsizetype i = 0; i < s.accN.size(); ++i) {
        const double alongTrack = alongTrackAt(s, i);
        const double aMag2 = s.accN[i] * s.accN[i] + s.accE[i] * s.accE[i] + s.accD[i] * s.accD[i];
        result.append(std::sqrt(std::max(0.0, aMag2 - alongTrack * alongTrack)));
    }
    return result;
}

} // namespace Calculations
} // namespace FlySight

#endif // TRACKHELPER_H
