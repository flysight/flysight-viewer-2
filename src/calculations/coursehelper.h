#ifndef COURSEHELPER_H
#define COURSEHELPER_H

#include <algorithm>
#include <cmath>
#include <optional>

#include <QVariant>
#include <QVector>

#include "anglehelper.h"

namespace FlySight {
namespace Calculations {

/// The course and its reference as the application defines them.
///
/// GNSS/course is the unwrapped course less the course reference angle, the
/// two functions below. Header-only, so that a library that does not link the
/// built-in calculations could share them; the fused heading, which once did,
/// is a compass heading and is not referenced to anything.

/// The course over ground in degrees, clockwise from north, sample by sample
/// and unwrapped by the one unwrap rule (anglehelper.h). Nothing when an
/// array is empty or the lengths differ: then there is no course.
inline std::optional<QVector<double>> unwrappedCourse(const QVector<double> &velN,
                                                      const QVector<double> &velE,
                                                      const QVector<double> &time)
{
    if (velN.isEmpty() || velE.isEmpty() || time.isEmpty())
        return std::nullopt;
    if (velN.size() != velE.size() || velN.size() != time.size())
        return std::nullopt;

    // The value of M_PI, spelled here so the header does not depend on a
    // platform defining it; the expression order keeps GNSS/course's bits
    constexpr double kPi = 3.14159265358979323846;
    QVector<double> rawDeg;
    rawDeg.reserve(velN.size());
    for (qsizetype i = 0; i < velN.size(); ++i)
        rawDeg.append(std::atan2(velE[i], velN[i]) * 180.0 / kPi);
    return unwrapDegrees(rawDeg);
}

/// The course reference angle: `course` (from unwrappedCourse(), on the same
/// `time`) interpolated linearly at `referenceTime`, the _COURSE_REF
/// attribute. Zero when the reference is not a number or lies outside the
/// time range, so the course is then measured from north.
inline double courseReferenceAngle(const QVector<double> &course, const QVector<double> &time,
                                   const QVariant &referenceTime)
{
    bool ok = false;
    const double refTime = referenceTime.toDouble(&ok);
    // Written as "not inside" so that a reference that is not a number, for
    // which every comparison is false, gives zero too
    if (!ok || !(refTime >= time.first() && refTime <= time.last()))
        return 0.0;
    // One sample: the reference is that sample (there is no interval to
    // interpolate in)
    if (time.size() == 1)
        return course[0];

    auto it = std::lower_bound(time.constBegin(), time.constEnd(), refTime);
    const int idx = std::clamp<int>(int(it - time.constBegin()), 1, int(time.size()) - 1);
    if (qFuzzyCompare(refTime, time[idx]))
        return course[idx];
    if (qFuzzyCompare(refTime, time[idx - 1]))
        return course[idx - 1];
    const double t = (refTime - time[idx - 1]) / (time[idx] - time[idx - 1]);
    return course[idx - 1] + t * (course[idx] - course[idx - 1]);
}

} // namespace Calculations
} // namespace FlySight

#endif // COURSEHELPER_H
