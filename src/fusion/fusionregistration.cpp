#include "fusion/fusionregistration.h"

#include <cmath>

#include <QCoreApplication>
#include <QString>
#include <QVector>

#include "calculations/registration.h"
#include "calculations/timefithelper.h"
#include "engine/calculationprogress.h"
#include "fusion/fusion.h"
#include "sessiondata.h"

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

// The eighteen measurement inputs of the fit and the member of the kernel's
// Channels each one fills: exactly its vector members, in member order. One
// table serves the declaration and the hand-over, like kFitOutputs.
struct FitInput {
    const char *sensor;
    const char *name;
    QVector<double> Fusion::Channels::*samples;
};

constexpr FitInput kFitInputs[] = {
    { "GNSS",  SessionKeys::Time, &Fusion::Channels::gnssTime },
    { "Local", "north", &Fusion::Channels::north },
    { "Local", "east",  &Fusion::Channels::east },
    { "Local", "down",  &Fusion::Channels::down },
    { "Local", "velN",  &Fusion::Channels::velN },
    { "Local", "velE",  &Fusion::Channels::velE },
    { "Local", "velD",  &Fusion::Channels::velD },
    { "GNSS",  "hAcc",  &Fusion::Channels::hAcc },
    { "GNSS",  "vAcc",  &Fusion::Channels::vAcc },
    { "GNSS",  "sAcc",  &Fusion::Channels::sAcc },
    { "IMU",   SessionKeys::Time, &Fusion::Channels::imuTime },
    { "IMU",   "ax",    &Fusion::Channels::ax },
    { "IMU",   "ay",    &Fusion::Channels::ay },
    { "IMU",   "az",    &Fusion::Channels::az },
    { "IMU",   "wx",    &Fusion::Channels::wx },
    { "IMU",   "wy",    &Fusion::Channels::wy },
    { "IMU",   "wz",    &Fusion::Channels::wz },
    { "IMU",   "temperature", &Fusion::Channels::imuTemperature }
};

} // namespace

// ---- the tables, as the public header exposes them --------------------------
// Three functions over kFitInputs and kFitOutputs for the registration below
// and for the tooling (fusion_runner), so that the tool feeds the kernel what
// computeFit() feeds it by construction: one table, one assembly.

// The measurements of kFitInputs, then the four origin attributes. Everything
// behind them (GNSS/lat, the TIME sensor, the time fit) is transitive and
// tracked by the engine. Markers and preferences are not inputs: they do not
// move the fit.
QList<CalcInput> Fusion::fitInputs()
{
    QList<CalcInput> inputs;
    for (const FitInput &input : kFitInputs)
        inputs.append(CalcInput::measurement(input.sensor, input.name));
    inputs.append(CalcInput::attribute(SessionKeys::LocalOriginIndex));
    inputs.append(CalcInput::attribute(SessionKeys::LocalOriginLat));
    inputs.append(CalcInput::attribute(SessionKeys::LocalOriginLon));
    inputs.append(CalcInput::attribute(SessionKeys::LocalOriginHmsl));
    return inputs;
}

// The effective input values as the kernel takes them. Field-by-field copies
// of implicitly shared vectors: no sample is copied and none is touched.
Fusion::Channels Fusion::channelsFrom(const MeasurementReader &measurement, const AttributeReader &attribute)
{
    Fusion::Channels channels;
    for (const FitInput &input : kFitInputs)
        channels.*(input.samples) = measurement(input.sensor, input.name);

    // A stored, hand-edited origin index that is not a number must not
    // silently mean "fix 0": -1 is the kernel's "outside the GNSS samples".
    bool isNumber = false;
    const qlonglong originIndex = attribute(SessionKeys::LocalOriginIndex).toLongLong(&isNumber);
    channels.originIndex = isNumber ? originIndex : -1;
    channels.originLat = attribute(SessionKeys::LocalOriginLat).toDouble();
    channels.originLon = attribute(SessionKeys::LocalOriginLon).toDouble();
    channels.originHMSL = attribute(SessionKeys::LocalOriginHmsl).toDouble();
    return channels;
}

QList<Fusion::FitOutputChannel> Fusion::fitOutputChannels(const Result &result)
{
    QList<FitOutputChannel> channels;
    for (const FitOutput &output : kFitOutputs)
        channels.append({ QString::fromLatin1(output.name), result.*(output.samples) });
    return channels;
}

// ---- the registered calculations ---------------------------------------------

namespace {

QList<DependencyKey> fitOutputs()
{
    QList<DependencyKey> outputs;
    for (const FitOutput &output : kFitOutputs)
        outputs.append(DependencyKey::measurement(kSensor, output.name));
    outputs.append(DependencyKey::attribute(SessionKeys::FusionDiagnostics));
    return outputs;
}

// The engine's context as the two readers of the public assembly.
Fusion::Channels channelsFrom(const EvaluationContext &ctx)
{
    return Fusion::channelsFrom(
        [&ctx](const QString &sensor, const QString &name) { return ctx.measurement(sensor, name); },
        [&ctx](const QString &key) { return ctx.attribute(key); });
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
    d.inputs = Fusion::fitInputs();
    d.outputs = fitOutputs();
    d.policy = EvaluationPolicy::Explicit;
    d.compute = computeFit;
    // A stored fit is used only while the kernel's arithmetic is the same.
    d.resultVersion = QString::fromLatin1(Fusion::Algorithm);
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
        const std::optional<QVector<double>> systemTime = Calculations::systemTimeFromUtc(
            ctx.measurement(kSensor, SessionKeys::Time),
            ctx.attribute(SessionKeys::TimeFitA).toDouble(),
            ctx.attribute(SessionKeys::TimeFitB).toDouble());
        if (!systemTime.has_value())
            return CalculationResult::unavailable();  // Cannot invert: degenerate fit

        // An axis with a hole in it is no axis
        for (const double t : *systemTime) {
            if (!std::isfinite(t))
                return CalculationResult::unavailable();
        }
        return CalculationResult().setMeasurement(kSensor, SessionKeys::SystemTime, *systemTime);
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
