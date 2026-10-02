#include "derivativehelper.h"
#include <QDebug>
#include <cmath>

namespace FlySight {
namespace Calculations {

namespace {

// The one stencil of the derivative: a forward difference at the first
// sample, a centred difference over two intervals for the interior, a
// backward difference at the last. The derivative and its accuracy share it
// so that an accuracy always qualifies the samples the derivative actually
// differenced. combine(earlier, later) is the numerator over the stencil's
// two samples; caller names the public function in the warnings.
template <typename Combine>
std::optional<QVector<double>> differenceOverStencil(const char *caller,
                                                     const QVector<double>& values,
                                                     const QVector<double>& times,
                                                     Combine combine)
{
    if (values.isEmpty()) {
        qWarning("%s: values array is empty.", caller);
        return std::nullopt;
    }

    if (values.size() != times.size()) {
        qWarning("%s: values and times size mismatch.", caller);
        return std::nullopt;
    }

    if (values.size() < 2) {
        qWarning("%s: not enough data points (need at least 2).", caller);
        return std::nullopt;
    }

    QVector<double> result;
    result.reserve(values.size());

    // Forward difference for first point
    {
        double dt = times[1] - times[0];
        if (dt == 0.0) {
            qWarning("%s: zero time difference between indices 0 and 1.", caller);
            return std::nullopt;
        }
        result.append(combine(values[0], values[1]) / dt);
    }

    // Centered difference for interior points
    for (int i = 1; i < values.size() - 1; ++i) {
        double dt = times[i + 1] - times[i - 1];
        if (dt == 0.0) {
            qWarning("%s: zero time difference for indices %d and %d", caller, i - 1, i + 1);
            return std::nullopt;
        }
        result.append(combine(values[i - 1], values[i + 1]) / dt);
    }

    // Backward difference for last point
    {
        int last = values.size() - 1;
        double dt = times[last] - times[last - 1];
        if (dt == 0.0) {
            qWarning("%s: zero time difference at end indices %d and %d", caller, last - 1, last);
            return std::nullopt;
        }
        result.append(combine(values[last - 1], values[last]) / dt);
    }

    return result;
}

} // namespace

std::optional<QVector<double>> computeDerivative(
    const QVector<double>& values,
    const QVector<double>& times)
{
    return differenceOverStencil("computeDerivative", values, times,
                                 [](double earlier, double later) { return later - earlier; });
}

std::optional<QVector<double>> computeDerivativeAccuracy(
    const QVector<double>& sigmas,
    const QVector<double>& times)
{
    // The variance of a difference of independent errors is the sum of their
    // variances; the sum of squares is written out, not std::hypot, so that
    // the result is the documented formula to the last bit
    return differenceOverStencil("computeDerivativeAccuracy", sigmas, times,
                                 [](double earlier, double later) {
                                     return std::sqrt(later * later + earlier * earlier);
                                 });
}

} // namespace Calculations
} // namespace FlySight
