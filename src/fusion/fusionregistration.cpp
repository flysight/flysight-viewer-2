#include "fusion/fusionregistration.h"

#include "calculations/registration.h"
#include "engine/calculationprogress.h"
#include "fusion/fusion.h"
#include "sessiondata.h"

#include <QCoreApplication>
#include <QVector>
#include <cmath>

// Sensor fusion as registered calculations: a thin adapter between the engine
// and the kernel (fusion.h). The adapter copies values and nothing else: every
// validation rule, unit conversion and message belongs to the kernel, where
// the golden tests hold it to the reference.

using namespace FlySight;

namespace {

constexpr char kSensor[] = "Fusion";

// The seventeen measurement outputs of the fit and the array of the kernel's
// result behind each. One table serves the declaration and the publication,
// so the two cannot drift apart.
struct FitOutput {
    const char *name;
    QVector<double> Fusion::Result::*samples;
};

constexpr FitOutput kFitOutputs[] = {
    { SessionKeys::Time, &Fusion::Result::time },
    { "north", &Fusion::Result::north },
    { "east",  &Fusion::Result::east },
    { "down",  &Fusion::Result::down },
    { "velN",  &Fusion::Result::velN },
    { "velE",  &Fusion::Result::velE },
    { "velD",  &Fusion::Result::velD },
    { "accN",  &Fusion::Result::accN },
    { "accE",  &Fusion::Result::accE },
    { "accD",  &Fusion::Result::accD },
    { "roll",  &Fusion::Result::roll },
    { "pitch", &Fusion::Result::pitch },
    { "yaw",   &Fusion::Result::yaw },
    { "qx",    &Fusion::Result::qx },
    { "qy",    &Fusion::Result::qy },
    { "qz",    &Fusion::Result::qz },
    { "qw",    &Fusion::Result::qw }
};

// Exactly the members of Fusion::Channels, in member order. Everything behind
// them (GNSS/lat, the TIME sensor, the time fit) is transitive and tracked by
// the engine. Markers and preferences are not inputs: they do not move the fit.
QList<CalcInput> fitInputs()
{
    return {
        CalcInput::measurement("GNSS", SessionKeys::Time),
        CalcInput::measurement("Local", "north"),
        CalcInput::measurement("Local", "east"),
        CalcInput::measurement("Local", "down"),
        CalcInput::measurement("Local", "velN"),
        CalcInput::measurement("Local", "velE"),
        CalcInput::measurement("Local", "velD"),
        CalcInput::measurement("GNSS", "hAcc"),
        CalcInput::measurement("GNSS", "vAcc"),
        CalcInput::measurement("GNSS", "sAcc"),
        CalcInput::measurement("IMU", SessionKeys::Time),
        CalcInput::measurement("IMU", "ax"),
        CalcInput::measurement("IMU", "ay"),
        CalcInput::measurement("IMU", "az"),
        CalcInput::measurement("IMU", "wx"),
        CalcInput::measurement("IMU", "wy"),
        CalcInput::measurement("IMU", "wz"),
        CalcInput::attribute(SessionKeys::LocalOriginIndex),
        CalcInput::attribute(SessionKeys::LocalOriginLat),
        CalcInput::attribute(SessionKeys::LocalOriginLon),
        CalcInput::attribute(SessionKeys::LocalOriginHmsl)
    };
}

QList<DependencyKey> fitOutputs()
{
    QList<DependencyKey> outputs;
    for (const FitOutput &output : kFitOutputs)
        outputs.append(DependencyKey::measurement(kSensor, output.name));
    outputs.append(DependencyKey::attribute(SessionKeys::FusionDiagnostics));
    return outputs;
}

// The effective input values as the kernel takes them. Field-by-field copies
// of implicitly shared vectors: no sample is copied and none is touched.
Fusion::Channels channelsFrom(const EvaluationContext &ctx)
{
    Fusion::Channels channels;
    channels.gnssTime = ctx.measurement("GNSS", SessionKeys::Time);
    channels.north = ctx.measurement("Local", "north");
    channels.east = ctx.measurement("Local", "east");
    channels.down = ctx.measurement("Local", "down");
    channels.velN = ctx.measurement("Local", "velN");
    channels.velE = ctx.measurement("Local", "velE");
    channels.velD = ctx.measurement("Local", "velD");
    channels.hAcc = ctx.measurement("GNSS", "hAcc");
    channels.vAcc = ctx.measurement("GNSS", "vAcc");
    channels.sAcc = ctx.measurement("GNSS", "sAcc");
    channels.imuTime = ctx.measurement("IMU", SessionKeys::Time);
    channels.ax = ctx.measurement("IMU", "ax");
    channels.ay = ctx.measurement("IMU", "ay");
    channels.az = ctx.measurement("IMU", "az");
    channels.wx = ctx.measurement("IMU", "wx");
    channels.wy = ctx.measurement("IMU", "wy");
    channels.wz = ctx.measurement("IMU", "wz");

    // A stored, hand-edited origin index that is not a number must not
    // silently mean "fix 0": -1 is the kernel's "outside the GNSS samples".
    bool isNumber = false;
    const qlonglong originIndex = ctx.attribute(SessionKeys::LocalOriginIndex).toLongLong(&isNumber);
    channels.originIndex = isNumber ? originIndex : -1;
    channels.originLat = ctx.attribute(SessionKeys::LocalOriginLat).toDouble();
    channels.originLon = ctx.attribute(SessionKeys::LocalOriginLon).toDouble();
    channels.originHMSL = ctx.attribute(SessionKeys::LocalOriginHmsl).toDouble();
    return channels;
}

// A fit that ended with a result, as the bundle the engine caches. A rejected
// recording and a solver failure are functions of the inputs like a success:
// the measurements stay unset (unavailable), the diagnostics carry the reason,
// and asking again with the same inputs runs nothing.
CalculationResult publish(const Fusion::Result &fit)
{
    CalculationResult result;
    result.setAttribute(SessionKeys::FusionDiagnostics, fit.diagnosticsJson);
    if (fit.outcome != Fusion::Outcome::Succeeded) {
        result.setReason(fit.reason);
        return result;
    }
    for (const FitOutput &output : kFitOutputs)
        result.setMeasurement(kSensor, output.name, fit.*(output.samples));
    return result;
}

// The explicit calculation. It may run on the job queue's worker while another
// session evaluates the same descriptor, so it keeps no state and logs
// nothing. Memory exhaustion is deliberately left to propagate: the engine
// classifies it, and nothing is cached for it.
CalculationResult computeFit(const EvaluationContext &ctx)
{
    const Fusion::Channels channels = channelsFrom(ctx);

    // The engine's facility, as the kernel's two callbacks. On the synchronous
    // path it is CalculationProgress::none(): text is dropped and cancellation
    // never happens, so both paths run the same fit on the same values.
    CalculationProgress &facility = ctx.progress();
    const Fusion::Result fit = Fusion::run(
        channels,
        [&facility](const QString &text) { facility.report(text); },
        [&facility] { return facility.isCancelled(); });

    // Abandoned at a boundary: the engine's cancellation, so nothing is published
    if (fit.outcome == Fusion::Outcome::Cancelled)
        throw CalculationCancelled();
    return publish(fit);
}

void registerFit(CalculationRegistry &registry)
{
    CalculationDescriptor d;
    d.id = QString::fromLatin1(Fusion::FitCalculationId);
    d.title = QCoreApplication::translate("Fusion", "Sensor fusion");
    d.inputs = fitInputs();
    d.outputs = fitOutputs();
    d.policy = EvaluationPolicy::Explicit;
    d.compute = computeFit;
    Calculations::addCalculation(registry, d);
}

// Fusion/accH: horizontal acceleration. On demand, but its inputs exist only
// once the fit has published, so it appears with the fit through ordinary
// invalidation and never starts one.
void registerHorizontalAcceleration(CalculationRegistry &registry)
{
    CalculationDescriptor d;
    d.id = QStringLiteral("builtin.fusion.accH");
    d.inputs = {
        CalcInput::measurement(kSensor, "accN"),
        CalcInput::measurement(kSensor, "accE")
    };
    d.outputs = { DependencyKey::measurement(kSensor, "accH") };
    d.compute = [](const EvaluationContext &ctx) -> CalculationResult {
        const QVector<double> accN = ctx.measurement(kSensor, "accN");
        const QVector<double> accE = ctx.measurement(kSensor, "accE");
        if (accN.size() != accE.size())
            return CalculationResult::unavailable();

        QVector<double> accH;
        accH.reserve(accN.size());
        for (int i = 0; i < accN.size(); ++i)
            accH.append(std::sqrt(accN[i]*accN[i] + accE[i]*accE[i]));
        return CalculationResult().setMeasurement(kSensor, "accH", accH);
    };
    Calculations::addCalculation(registry, d);
}

// Fusion/_system_time: the inverse time fit of Fusion/_time, as for GNSS
// (builtin.time.system.GNSS). Not a passthrough of IMU/time: the fused
// timestamps are a subset of the IMU samples, expressed in UTC.
void registerSystemTime(CalculationRegistry &registry)
{
    CalculationDescriptor d;
    d.id = QStringLiteral("builtin.fusion.systemTime");
    d.inputs = {
        CalcInput::measurement(kSensor, SessionKeys::Time),
        CalcInput::attribute(SessionKeys::TimeFitA),
        CalcInput::attribute(SessionKeys::TimeFitB)
    };
    d.outputs = { DependencyKey::measurement(kSensor, SessionKeys::SystemTime) };
    d.compute = [](const EvaluationContext &ctx) -> CalculationResult {
        const QVector<double> utcTime = ctx.measurement(kSensor, SessionKeys::Time);
        const double a = ctx.attribute(SessionKeys::TimeFitA).toDouble();
        const double b = ctx.attribute(SessionKeys::TimeFitB).toDouble();
        if (a == 0.0)
            return CalculationResult::unavailable();  // Cannot invert: degenerate fit

        QVector<double> result(utcTime.size());
        for (int i = 0; i < utcTime.size(); ++i) {
            result[i] = (utcTime[i] - b) / a;
            // An axis with a hole in it is no axis
            if (!std::isfinite(result[i]))
                return CalculationResult::unavailable();
        }
        return CalculationResult().setMeasurement(kSensor, SessionKeys::SystemTime, result);
    };
    Calculations::addCalculation(registry, d);
}

} // namespace

void Fusion::registerFusionCalculations(CalculationRegistry &registry)
{
    registerFit(registry);
    registerHorizontalAcceleration(registry);
    registerSystemTime(registry);
}
