#ifndef DERIVATIVEHELPER_H
#define DERIVATIVEHELPER_H

#include <QVector>
#include <optional>

namespace FlySight {
namespace Calculations {

// The time derivative of values: a forward difference at the first sample, a
// centred difference over two intervals for the interior, a backward
// difference at the last. Unavailable (nullopt) when values is empty, has
// fewer than two samples or a length other than times', or when the stencil
// meets two equal times. A stencil never spans a hole of times (the
// continuity rule, samplecontinuity.h): a sample whose stencil holds an
// interval that is a hole, interior or at an end, is NaN, and every other
// sample is what it would be without the rule.
std::optional<QVector<double>> computeDerivative(
    const QVector<double>& values,
    const QVector<double>& times);

// The standard deviation of computeDerivative's result when each value's
// error has the standard deviation in sigmas and the errors of different
// samples are independent: sqrt(sigma_later^2 + sigma_earlier^2) / dt over
// the same stencil, unavailable exactly where the derivative is and NaN at
// exactly the samples where it is NaN for a hole.
std::optional<QVector<double>> computeDerivativeAccuracy(
    const QVector<double>& sigmas,
    const QVector<double>& times);

} // namespace Calculations
} // namespace FlySight

#endif // DERIVATIVEHELPER_H
