#include "timecalculations.h"
#include "registration.h"
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

    // Least-squares linear fit: utcTime = a * systemTime + b
    double sumS = 0.0, sumU = 0.0, sumSS = 0.0, sumSU = 0.0;
    for (int i = 0; i < N; ++i) {
        double S = systemTime[i];
        double U = utcTime[i];
        sumS += S;
        sumU += U;
        sumSS += S * S;
        sumSU += S * U;
    }

    double denom = (N * sumSS - sumS * sumS);
    if (denom == 0.0) {
        return std::nullopt;
    }

    TimeFit fit;
    fit.a = (N * sumSU - sumS * sumU) / denom;
    fit.b = (sumU - fit.a * sumS) / N;
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
            const QVector<double> utcTime = ctx.measurement("GNSS", "time");
            const double a = ctx.attribute(SessionKeys::TimeFitA).toDouble();
            const double b = ctx.attribute(SessionKeys::TimeFitB).toDouble();
            if (a == 0.0) {
                return CalculationResult::unavailable();  // Cannot invert: degenerate fit
            }

            QVector<double> result(utcTime.size());
            for (int i = 0; i < utcTime.size(); ++i) {
                result[i] = (utcTime[i] - b) / a;
            }
            return CalculationResult().setMeasurement("GNSS", SessionKeys::SystemTime, result);
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
