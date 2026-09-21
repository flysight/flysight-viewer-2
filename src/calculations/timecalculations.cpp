#include "timecalculations.h"
#include "registration.h"
#include "timefithelper.h"
#include "../sessiondata.h"
#include "../dependencykey.h"
#include <QVector>
#include <QDateTime>
#include <algorithm>

using namespace FlySight;

namespace {

// Linear fit utc = a * systemTime + b
struct TimeFit {
    double a;
    double b;
};

// Least-squares fit of UTC (from GPS week + time of week) against system time.
std::optional<TimeFit> fitSystemTimeToUtc(const QVector<double> &systemTime,
                                          const QVector<double> &tow,
                                          const QVector<double> &week)
{
    int N = std::min({systemTime.size(), tow.size(), week.size()});
    if (N < 2) {
        return std::nullopt;
    }

    // Compute UTC time from GPS week + time-of-week
    QVector<double> utcTime(N);
    for (int i = 0; i < N; ++i) {
        utcTime[i] = week[i] * 604800 + tow[i] + 315964800;
    }

    // Least-squares linear fit: utcTime = a * systemTime + b, with centered
    // sums. The textbook normal equations (N * sumSU - sumS * sumU, and so on)
    // subtract huge, nearly equal products of Unix UTC (~1.7e9) and device
    // uptime (~1e5): the products need more digits than a double has, and the
    // fit loses tens of milliseconds even for an exactly linear clock. Here
    // every value is first taken relative to the first sample and then to the
    // mean, so the sums run over small numbers and nothing cancels.
    //
    // The operation order below is part of the contract: consumers compare
    // times derived from a and b bit for bit.
    const double systemReference = systemTime[0];
    const double utcReference = utcTime[0];

    // First pass: means, relative to the first sample
    double meanS = 0.0, meanU = 0.0;
    for (int i = 0; i < N; ++i) {
        meanS += systemTime[i] - systemReference;
        meanU += utcTime[i] - utcReference;
    }
    meanS /= N;
    meanU /= N;

    // Second pass: variance of system time and its covariance with UTC
    double variance = 0.0, covariance = 0.0;
    for (int i = 0; i < N; ++i) {
        const double S = (systemTime[i] - systemReference) - meanS;
        const double U = (utcTime[i] - utcReference) - meanU;
        variance += S * S;
        covariance += S * U;
    }

    // Degenerate fit: every pulse at the same system time (or NaN data)
    if (!(variance > 0.0)) {
        return std::nullopt;
    }

    // The line passes through the means. The two brackets of b are kept
    // apart: the first has UTC's magnitude, the second is small-minus-small,
    // and it is only added once it has been formed at full precision.
    TimeFit fit;
    fit.a = covariance / variance;
    fit.b = (utcReference - fit.a * systemReference) + (meanU - fit.a * meanS);
    return fit;
}

} // namespace

void Calculations::registerTimeCalculations(CalculationRegistry &registry)
{
    // System-time-to-UTC linear fit. Both coefficients come from one
    // computation. They are published as strings with 17 significant digits;
    // consumers call toDouble().
    {
        CalculationDescriptor d;
        d.id = QStringLiteral("builtin.time.fit");
        d.inputs = {
            CalcInput::measurement("TIME", "time"),
            CalcInput::measurement("TIME", "tow"),
            CalcInput::measurement("TIME", "week")
        };
        d.outputs = {
            DependencyKey::attribute(SessionKeys::TimeFitA),
            DependencyKey::attribute(SessionKeys::TimeFitB)
        };
        d.compute = [](const EvaluationContext &ctx) -> CalculationResult {
            const auto fit = fitSystemTimeToUtc(ctx.measurement("TIME", "time"),
                                                ctx.measurement("TIME", "tow"),
                                                ctx.measurement("TIME", "week"));
            if (!fit)
                return CalculationResult::unavailable();
            return CalculationResult()
                .setAttribute(SessionKeys::TimeFitA, QString::number(fit->a, 'g', 17))
                .setAttribute(SessionKeys::TimeFitB, QString::number(fit->b, 'g', 17));
        };
        addCalculation(registry, d);
    }

    // _time for GNSS (time is already in UTC): passthrough sharing the buffer
    {
        CalculationDescriptor d;
        d.id = QStringLiteral("builtin.time.utc.GNSS");
        d.inputs = { CalcInput::measurement("GNSS", "time") };
        d.outputs = { DependencyKey::measurement("GNSS", SessionKeys::Time) };
        d.compute = [](const EvaluationContext &ctx) -> CalculationResult {
            return CalculationResult().setMeasurement("GNSS", SessionKeys::Time,
                                                      ctx.measurement("GNSS", "time"));
        };
        addCalculation(registry, d);
    }

    // _time for the other sensors (system time converted through the fit)
    const QStringList sensors = {"BARO", "HUM", "MAG", "IMU", "TIME", "VBAT"};
    for (const QString &sens : sensors) {
        CalculationDescriptor d;
        d.id = QStringLiteral("builtin.time.utc.") + sens;
        d.inputs = {
            CalcInput::measurement(sens, "time"),
            CalcInput::attribute(SessionKeys::TimeFitA),
            CalcInput::attribute(SessionKeys::TimeFitB)
        };
        d.outputs = { DependencyKey::measurement(sens, SessionKeys::Time) };
        d.compute = [sens](const EvaluationContext &ctx) -> CalculationResult {
            const QVector<double> sensorSystemTime = ctx.measurement(sens, "time");
            const double a = ctx.attribute(SessionKeys::TimeFitA).toDouble();
            const double b = ctx.attribute(SessionKeys::TimeFitB).toDouble();

            QVector<double> result(sensorSystemTime.size());
            for (int i = 0; i < sensorSystemTime.size(); ++i) {
                result[i] = a * sensorSystemTime[i] + b;
            }
            return CalculationResult().setMeasurement(sens, SessionKeys::Time, result);
        };
        addCalculation(registry, d);
    }

    // _system_time for GNSS (inverse fit: UTC -> system time)
    {
        CalculationDescriptor d;
        d.id = QStringLiteral("builtin.time.system.GNSS");
        d.inputs = {
            CalcInput::measurement("GNSS", "time"),
            CalcInput::attribute(SessionKeys::TimeFitA),
            CalcInput::attribute(SessionKeys::TimeFitB)
        };
        d.outputs = { DependencyKey::measurement("GNSS", SessionKeys::SystemTime) };
        d.compute = [](const EvaluationContext &ctx) -> CalculationResult {
            const std::optional<QVector<double>> systemTime = systemTimeFromUtc(
                ctx.measurement("GNSS", "time"),
                ctx.attribute(SessionKeys::TimeFitA).toDouble(),
                ctx.attribute(SessionKeys::TimeFitB).toDouble());
            if (!systemTime.has_value())
                return CalculationResult::unavailable();  // Cannot invert: degenerate fit
            return CalculationResult().setMeasurement("GNSS", SessionKeys::SystemTime, *systemTime);
        };
        addCalculation(registry, d);
    }

    // _system_time for the other sensors (passthrough of the raw system time)
    for (const QString &sens : sensors) {
        CalculationDescriptor d;
        d.id = QStringLiteral("builtin.time.system.") + sens;
        d.inputs = { CalcInput::measurement(sens, "time") };
        d.outputs = { DependencyKey::measurement(sens, SessionKeys::SystemTime) };
        d.compute = [sens](const EvaluationContext &ctx) -> CalculationResult {
            return CalculationResult().setMeasurement(sens, SessionKeys::SystemTime,
                                                      ctx.measurement(sens, "time"));
        };
        addCalculation(registry, d);
    }
}

std::optional<double> Calculations::systemTimeToUtc(const SessionData &session, double systemTime)
{
    QVariant vA = session.getAttribute(SessionKeys::TimeFitA);
    QVariant vB = session.getAttribute(SessionKeys::TimeFitB);
    if (!vA.isValid() || !vB.isValid()) {
        return std::nullopt;
    }
    double a = vA.toDouble();
    double b = vB.toDouble();
    return a * systemTime + b;
}

std::optional<double> Calculations::utcToSystemTime(const SessionData &session, double utc)
{
    QVariant vA = session.getAttribute(SessionKeys::TimeFitA);
    QVariant vB = session.getAttribute(SessionKeys::TimeFitB);
    if (!vA.isValid() || !vB.isValid()) {
        return std::nullopt;
    }
    double a = vA.toDouble();
    double b = vB.toDouble();
    if (a == 0.0) {
        return std::nullopt;  // Cannot invert: degenerate fit
    }
    return (utc - b) / a;
}
