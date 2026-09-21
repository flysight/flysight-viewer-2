#include "gnsscalculations.h"
#include "anglehelper.h"
#include "derivativehelper.h"
#include "isadensity.h"
#include "../sessiondata.h"
#include "../dependencykey.h"
#include "registration.h"
#include <QVector>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>

using namespace FlySight;

namespace {

using GnssFunction = std::function<std::optional<QVector<double>>(const EvaluationContext &)>;

CalcInput gnss(const char *name)
{
    return CalcInput::measurement(QStringLiteral("GNSS"), QString::fromLatin1(name));
}

// Registers the single-output calculation builtin.gnss.<name> -> GNSS/<name>.
// No unit is reported for derived measurements.
void registerGnss(CalculationRegistry &registry, const char *name,
                  const QList<CalcInput> &inputs, GnssFunction fn)
{
    const QString measurement = QString::fromLatin1(name);

    CalculationDescriptor d;
    d.id = QStringLiteral("builtin.gnss.") + measurement;
    d.inputs = inputs;
    d.outputs = { DependencyKey::measurement(QStringLiteral("GNSS"), measurement) };
    d.compute = [measurement, fn](const EvaluationContext &ctx) -> CalculationResult {
        const std::optional<QVector<double>> values = fn(ctx);
        if (!values)
            return CalculationResult::unavailable();
        return CalculationResult().setMeasurement(QStringLiteral("GNSS"), measurement, *values);
    };
    Calculations::addCalculation(registry, d);
}

// Time derivative of GNSS/<source> against the raw GNSS time.
void registerGnssDerivative(CalculationRegistry &registry, const char *name, const char *source)
{
    const QString sourceName = QString::fromLatin1(source);
    registerGnss(registry, name, { gnss(source), gnss("time") },
        [sourceName](const EvaluationContext &ctx) -> std::optional<QVector<double>> {
            QVector<double> values = ctx.measurement("GNSS", sourceName);
            QVector<double> time = ctx.measurement("GNSS", "time");

            if (values.isEmpty()) {
                return std::nullopt;
            }

            return Calculations::computeDerivative(values, time);
        });
}

// A wind component; a stored non-numeric value counts as no wind.
double windComponent(const EvaluationContext &ctx, const char *key)
{
    bool ok;
    double wind = ctx.attribute(key).toDouble(&ok);
    if (!ok) wind = 0.0;
    return wind;
}

enum class AeroCoefficient { Lift, Drag };

// Lift / drag coefficient.
// Uses gravity-corrected acceleration: aeroD = accD - g, so that
// level constant-speed flight (zero kinematic acceleration) correctly
// shows ~1g of lift. Drag is negated so that deceleration along track gives
// positive values.
std::optional<QVector<double>> computeAeroCoefficient(const EvaluationContext &ctx,
                                                      AeroCoefficient which)
{
    constexpr double g = 9.80665;

    QVector<double> accN = ctx.measurement("GNSS", "accN");
    QVector<double> accE = ctx.measurement("GNSS", "accE");
    QVector<double> accD = ctx.measurement("GNSS", "accD");
    QVector<double> velN = ctx.measurement("GNSS", "velN");
    QVector<double> velE = ctx.measurement("GNSS", "velE");
    QVector<double> velD = ctx.measurement("GNSS", "velD");
    QVector<double> wcVel = ctx.measurement("GNSS", "wcVel");
    QVector<double> hMSL = ctx.measurement("GNSS", "hMSL");

    if (accN.isEmpty() || accE.isEmpty() || accD.isEmpty() ||
        velN.isEmpty() || velE.isEmpty() || velD.isEmpty() ||
        wcVel.isEmpty() || hMSL.isEmpty()) {
        return std::nullopt;
    }

    int n = accN.size();
    if (accE.size() != n || accD.size() != n ||
        velN.size() != n || velE.size() != n || velD.size() != n ||
        wcVel.size() != n || hMSL.size() != n) {
        return std::nullopt;
    }

    const double windN = windComponent(ctx, SessionKeys::WindN);
    const double windE = windComponent(ctx, SessionKeys::WindE);

    QVariant massVar = ctx.attribute(SessionKeys::JumperMass);
    QVariant areaVar = ctx.attribute(SessionKeys::PlanformArea);
    if (!massVar.isValid() || !areaVar.isValid()) return std::nullopt;
    double mass = massVar.toDouble();
    double area = areaVar.toDouble();

    QVector<double> result;
    result.reserve(n);
    for (int i = 0; i < n; ++i) {
        double rho = Calculations::isaDensity(hMSL[i]);
        double mu = wcVel[i];
        if (mu < 1e-9 || rho < 1e-12 || area < 1e-12) {
            result.append(std::numeric_limits<double>::quiet_NaN());
            continue;
        }

        // Gravity-corrected acceleration
        double aeroN = accN[i];
        double aeroE = accE[i];
        double aeroD = accD[i] - g;

        // Wind-corrected velocity unit vector
        double wcN = velN[i] - windN;
        double wcE = velE[i] - windE;
        double wcD = velD[i];
        double wcMag = std::sqrt(wcN * wcN + wcE * wcE + wcD * wcD);

        if (wcMag < 1e-9) {
            result.append(std::numeric_limits<double>::quiet_NaN());
            continue;
        }

        double uN = wcN / wcMag;
        double uE = wcE / wcMag;
        double uD = wcD / wcMag;

        double alongTrack = aeroN * uN + aeroE * uE + aeroD * uD;

        if (which == AeroCoefficient::Lift) {
            // Cross-track magnitude
            double aMag2 = aeroN * aeroN + aeroE * aeroE + aeroD * aeroD;
            double crossTrack = std::sqrt(std::max(0.0, aMag2 - alongTrack * alongTrack));
            result.append(2.0 * mass * crossTrack / (rho * mu * mu * area));
        } else {
            // Along-track component (negated: drag positive)
            result.append(-2.0 * mass * alongTrack / (rho * mu * mu * area));
        }
    }
    return result;
}

} // namespace

void Calculations::registerGnssCalculations(CalculationRegistry &registry)
{
    // GNSS altitude above ground (z)
    registerGnss(registry, "z",
        { gnss("hMSL"), CalcInput::attribute(SessionKeys::GroundElev) },
        [](const EvaluationContext &ctx) -> std::optional<QVector<double>> {
        QVector<double> hMSL = ctx.measurement("GNSS", "hMSL");

        bool ok;
        double groundElev = ctx.attribute(SessionKeys::GroundElev).toDouble(&ok);
        if (!ok) {
            qWarning() << "Cannot calculate z due to missing groundElev";
            return std::nullopt;
        }

        if (hMSL.isEmpty()) {
            qWarning() << "Cannot calculate z due to missing hMSL";
            return std::nullopt;
        }

        QVector<double> z;
        z.reserve(hMSL.size());
        for(int i = 0; i < hMSL.size(); ++i){
            z.append(hMSL[i] - groundElev);
        }
        return z;
    });

    // GNSS horizontal velocity (velH)
    registerGnss(registry, "velH",
        { gnss("velN"), gnss("velE") },
        [](const EvaluationContext &ctx) -> std::optional<QVector<double>> {
        QVector<double> velN = ctx.measurement("GNSS", "velN");
        QVector<double> velE = ctx.measurement("GNSS", "velE");

        if (velN.isEmpty() || velE.isEmpty()) {
            qWarning() << "Cannot calculate velH due to missing velN or velE";
            return std::nullopt;
        }

        if (velN.size() != velE.size()) {
            qWarning() << "velN and velE size mismatch";
            return std::nullopt;
        }

        QVector<double> velH;
        velH.reserve(velN.size());
        for(int i = 0; i < velN.size(); ++i){
            velH.append(std::sqrt(velN[i]*velN[i] + velE[i]*velE[i]));
        }
        return velH;
    });

    // GNSS total velocity (vel)
    registerGnss(registry, "vel",
        { gnss("velH"), gnss("velD") },
        [](const EvaluationContext &ctx) -> std::optional<QVector<double>> {
        QVector<double> velH = ctx.measurement("GNSS", "velH");
        QVector<double> velD = ctx.measurement("GNSS", "velD");

        if (velH.isEmpty() || velD.isEmpty()) {
            qWarning() << "Cannot calculate vel due to missing velH or velD";
            return std::nullopt;
        }

        if (velH.size() != velD.size()) {
            qWarning() << "velH and velD size mismatch";
            return std::nullopt;
        }

        QVector<double> vel;
        vel.reserve(velH.size());
        for(int i = 0; i < velH.size(); ++i){
            vel.append(std::sqrt(velH[i]*velH[i] + velD[i]*velD[i]));
        }
        return vel;
    });

    // GNSS vertical / northward / eastward acceleration
    registerGnssDerivative(registry, "accD", "velD");
    registerGnssDerivative(registry, "accN", "velN");
    registerGnssDerivative(registry, "accE", "velE");

    // GNSS wind-corrected total speed (wcVel)
    registerGnss(registry, "wcVel",
        { gnss("velN"), gnss("velE"), gnss("velD"),
          CalcInput::attribute(SessionKeys::WindN), CalcInput::attribute(SessionKeys::WindE) },
        [](const EvaluationContext &ctx) -> std::optional<QVector<double>> {
        QVector<double> velN = ctx.measurement("GNSS", "velN");
        QVector<double> velE = ctx.measurement("GNSS", "velE");
        QVector<double> velD = ctx.measurement("GNSS", "velD");

        if (velN.isEmpty() || velE.isEmpty() || velD.isEmpty()) {
            return std::nullopt;
        }
        if (velN.size() != velE.size() || velN.size() != velD.size()) {
            return std::nullopt;
        }

        const double windN = windComponent(ctx, SessionKeys::WindN);
        const double windE = windComponent(ctx, SessionKeys::WindE);

        QVector<double> result;
        result.reserve(velN.size());
        for (int i = 0; i < velN.size(); ++i) {
            double wcN = velN[i] - windN;
            double wcE = velE[i] - windE;
            double wcD = velD[i];
            result.append(std::sqrt(wcN * wcN + wcE * wcE + wcD * wcD));
        }
        return result;
    });

    // GNSS course (unwrapped heading minus reference)
    registerGnss(registry, "course",
        { gnss("velN"), gnss("velE"), gnss(SessionKeys::Time),
          CalcInput::attribute(SessionKeys::CourseRef) },
        [](const EvaluationContext &ctx) -> std::optional<QVector<double>> {
        QVector<double> velN = ctx.measurement("GNSS", "velN");
        QVector<double> velE = ctx.measurement("GNSS", "velE");
        QVector<double> time = ctx.measurement("GNSS", SessionKeys::Time);

        if (velN.isEmpty() || velE.isEmpty() || time.isEmpty()) {
            return std::nullopt;
        }
        if (velN.size() != velE.size() || velN.size() != time.size()) {
            return std::nullopt;
        }

        // Compute raw headings in degrees
        QVector<double> rawDeg;
        rawDeg.reserve(velN.size());
        for (int i = 0; i < velN.size(); ++i) {
            rawDeg.append(std::atan2(velE[i], velN[i]) * 180.0 / M_PI);
        }

        // Unwrap phase
        QVector<double> course = Calculations::unwrapDegrees(rawDeg);

        // Determine reference angle from CourseRef time
        double courseRef = 0.0;
        bool ok;
        double refTime = ctx.attribute(SessionKeys::CourseRef).toDouble(&ok);
        if (ok && refTime >= time.first() && refTime <= time.last()) {
            auto it = std::lower_bound(time.constBegin(), time.constEnd(), refTime);
            int idx = std::clamp<int>(int(it - time.constBegin()), 1, time.size() - 1);
            if (qFuzzyCompare(refTime, time[idx])) {
                courseRef = course[idx];
            } else if (qFuzzyCompare(refTime, time[idx - 1])) {
                courseRef = course[idx - 1];
            } else {
                double t = (refTime - time[idx - 1]) / (time[idx] - time[idx - 1]);
                courseRef = course[idx - 1] + t * (course[idx] - course[idx - 1]);
            }
        }

        // Subtract course reference
        for (int i = 0; i < course.size(); ++i) {
            course[i] -= courseRef;
        }

        return course;
    });

    // GNSS course rate (rate of change of course)
    registerGnssDerivative(registry, "courseRate", "course");

    // GNSS glide ratio
    registerGnss(registry, "glideRatio",
        { gnss("velH"), gnss("velD") },
        [](const EvaluationContext &ctx) -> std::optional<QVector<double>> {
        QVector<double> velH = ctx.measurement("GNSS", "velH");
        QVector<double> velD = ctx.measurement("GNSS", "velD");

        if (velH.isEmpty() || velD.isEmpty()) {
            return std::nullopt;
        }
        if (velH.size() != velD.size()) {
            return std::nullopt;
        }

        QVector<double> result;
        result.reserve(velH.size());
        for (int i = 0; i < velH.size(); ++i) {
            if (std::abs(velD[i]) < 1e-6) {
                result.append(std::numeric_limits<double>::quiet_NaN());
            } else {
                result.append(velH[i] / velD[i]);
            }
        }
        return result;
    });

    // GNSS dive angle
    registerGnss(registry, "diveAngle",
        { gnss("velH"), gnss("velD") },
        [](const EvaluationContext &ctx) -> std::optional<QVector<double>> {
        QVector<double> velH = ctx.measurement("GNSS", "velH");
        QVector<double> velD = ctx.measurement("GNSS", "velD");

        if (velH.isEmpty() || velD.isEmpty()) {
            return std::nullopt;
        }
        if (velH.size() != velD.size()) {
            return std::nullopt;
        }

        QVector<double> result;
        result.reserve(velH.size());
        for (int i = 0; i < velH.size(); ++i) {
            result.append(std::atan2(velD[i], velH[i]) * 180.0 / M_PI);
        }
        return result;
    });

    // GNSS dive angle rate (rate of change of dive angle)
    registerGnssDerivative(registry, "diveAngleRate", "diveAngle");

    // GNSS horizontal acceleration (accH)
    // Magnitude of the horizontal component of the total acceleration vector,
    // so that sqrt(accH^2 + accD^2) equals the total acceleration magnitude.
    registerGnss(registry, "accH",
        { gnss("accN"), gnss("accE") },
        [](const EvaluationContext &ctx) -> std::optional<QVector<double>> {
        QVector<double> accN = ctx.measurement("GNSS", "accN");
        QVector<double> accE = ctx.measurement("GNSS", "accE");

        if (accN.isEmpty() || accE.isEmpty()) {
            return std::nullopt;
        }
        if (accN.size() != accE.size()) {
            return std::nullopt;
        }

        QVector<double> accH;
        accH.reserve(accN.size());
        for (int i = 0; i < accN.size(); ++i) {
            accH.append(std::sqrt(accN[i] * accN[i] + accE[i] * accE[i]));
        }
        return accH;
    });

    // GNSS wind-corrected horizontal speed (wcVelH)
    registerGnss(registry, "wcVelH",
        { gnss("velN"), gnss("velE"),
          CalcInput::attribute(SessionKeys::WindN), CalcInput::attribute(SessionKeys::WindE) },
        [](const EvaluationContext &ctx) -> std::optional<QVector<double>> {
        QVector<double> velN = ctx.measurement("GNSS", "velN");
        QVector<double> velE = ctx.measurement("GNSS", "velE");

        if (velN.isEmpty() || velE.isEmpty()) {
            return std::nullopt;
        }
        if (velN.size() != velE.size()) {
            return std::nullopt;
        }

        const double windN = windComponent(ctx, SessionKeys::WindN);
        const double windE = windComponent(ctx, SessionKeys::WindE);

        QVector<double> result;
        result.reserve(velN.size());
        for (int i = 0; i < velN.size(); ++i) {
            double wcN = velN[i] - windN;
            double wcE = velE[i] - windE;
            result.append(std::sqrt(wcN * wcN + wcE * wcE));
        }
        return result;
    });

    // Inputs shared by the track-relative accelerations
    const QList<CalcInput> trackInputs = {
        gnss("accN"), gnss("accE"), gnss("accD"),
        gnss("velN"), gnss("velE"), gnss("velD"),
        CalcInput::attribute(SessionKeys::WindN), CalcInput::attribute(SessionKeys::WindE)
    };

    // GNSS along-track acceleration (accAlongTrack)
    registerGnss(registry, "accAlongTrack", trackInputs,
        [](const EvaluationContext &ctx) -> std::optional<QVector<double>> {
        QVector<double> accN = ctx.measurement("GNSS", "accN");
        QVector<double> accE = ctx.measurement("GNSS", "accE");
        QVector<double> accD = ctx.measurement("GNSS", "accD");
        QVector<double> velN = ctx.measurement("GNSS", "velN");
        QVector<double> velE = ctx.measurement("GNSS", "velE");
        QVector<double> velD = ctx.measurement("GNSS", "velD");

        if (accN.isEmpty() || accE.isEmpty() || accD.isEmpty() ||
            velN.isEmpty() || velE.isEmpty() || velD.isEmpty()) {
            return std::nullopt;
        }

        int n = accN.size();
        if (accE.size() != n || accD.size() != n ||
            velN.size() != n || velE.size() != n || velD.size() != n) {
            return std::nullopt;
        }

        const double windN = windComponent(ctx, SessionKeys::WindN);
        const double windE = windComponent(ctx, SessionKeys::WindE);

        QVector<double> result;
        result.reserve(n);
        for (int i = 0; i < n; ++i) {
            double wcN = velN[i] - windN;
            double wcE = velE[i] - windE;
            double wcD = velD[i];
            double wcMag = std::sqrt(wcN * wcN + wcE * wcE + wcD * wcD);

            if (wcMag < 1e-9) {
                result.append(0.0);
            } else {
                double uN = wcN / wcMag;
                double uE = wcE / wcMag;
                double uD = wcD / wcMag;
                double dot = accN[i] * uN + accE[i] * uE + accD[i] * uD;
                result.append(dot);
            }
        }
        return result;
    });

    // GNSS cross-track acceleration (accCrossTrack)
    registerGnss(registry, "accCrossTrack", trackInputs,
        [](const EvaluationContext &ctx) -> std::optional<QVector<double>> {
        QVector<double> accN = ctx.measurement("GNSS", "accN");
        QVector<double> accE = ctx.measurement("GNSS", "accE");
        QVector<double> accD = ctx.measurement("GNSS", "accD");
        QVector<double> velN = ctx.measurement("GNSS", "velN");
        QVector<double> velE = ctx.measurement("GNSS", "velE");
        QVector<double> velD = ctx.measurement("GNSS", "velD");

        if (accN.isEmpty() || accE.isEmpty() || accD.isEmpty() ||
            velN.isEmpty() || velE.isEmpty() || velD.isEmpty()) {
            return std::nullopt;
        }

        int n = accN.size();
        if (accE.size() != n || accD.size() != n ||
            velN.size() != n || velE.size() != n || velD.size() != n) {
            return std::nullopt;
        }

        const double windN = windComponent(ctx, SessionKeys::WindN);
        const double windE = windComponent(ctx, SessionKeys::WindE);

        QVector<double> result;
        result.reserve(n);
        for (int i = 0; i < n; ++i) {
            double wcN = velN[i] - windN;
            double wcE = velE[i] - windE;
            double wcD = velD[i];
            double wcMag = std::sqrt(wcN * wcN + wcE * wcE + wcD * wcD);

            double alongTrack;
            if (wcMag < 1e-9) {
                alongTrack = 0.0;
            } else {
                double uN = wcN / wcMag;
                double uE = wcE / wcMag;
                double uD = wcD / wcMag;
                double dot = accN[i] * uN + accE[i] * uE + accD[i] * uD;
                alongTrack = -dot;
            }

            double aMag2 = accN[i] * accN[i] + accE[i] * accE[i] + accD[i] * accD[i];
            result.append(std::sqrt(std::max(0.0, aMag2 - alongTrack * alongTrack)));
        }
        return result;
    });

    // GNSS lift and drag coefficients
    const QList<CalcInput> aeroInputs = {
        gnss("accN"), gnss("accE"), gnss("accD"),
        gnss("velN"), gnss("velE"), gnss("velD"),
        gnss("wcVel"), gnss("hMSL"),
        CalcInput::attribute(SessionKeys::WindN), CalcInput::attribute(SessionKeys::WindE),
        CalcInput::attribute(SessionKeys::JumperMass), CalcInput::attribute(SessionKeys::PlanformArea)
    };

    registerGnss(registry, "lift", aeroInputs,
        [](const EvaluationContext &ctx) {
            return computeAeroCoefficient(ctx, AeroCoefficient::Lift);
        });

    registerGnss(registry, "drag", aeroInputs,
        [](const EvaluationContext &ctx) {
            return computeAeroCoefficient(ctx, AeroCoefficient::Drag);
        });

    // GNSS specific energy
    registerGnss(registry, "specificEnergy",
        { gnss("vel"), gnss("z") },
        [](const EvaluationContext &ctx) -> std::optional<QVector<double>> {
        QVector<double> vel = ctx.measurement("GNSS", "vel");
        QVector<double> z = ctx.measurement("GNSS", "z");

        if (vel.isEmpty() || z.isEmpty()) {
            return std::nullopt;
        }
        if (vel.size() != z.size()) {
            return std::nullopt;
        }

        const double g = 9.80665;

        QVector<double> result;
        result.reserve(vel.size());
        for (int i = 0; i < vel.size(); ++i) {
            result.append(0.5 * vel[i] * vel[i] + g * z[i]);
        }
        return result;
    });

    // GNSS specific energy rate
    registerGnssDerivative(registry, "specificEnergyRate", "specificEnergy");
}
